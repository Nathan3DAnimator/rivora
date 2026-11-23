// Ray tracing by software (A1): sw_rt.h
#include "sw_rt.h"
#include "render.h"
#include "vkctx.h"
#include "platform.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW_LEAF 4u    // the most triangles (instances) a leaf holds
#define SW_BINS 16    // the SAH's bins along the widest axis of the centroids
#define SW_EMPTY 0xFFFFFFFFu

// a set's header on the GPU (sw_rt.glsl SwScene): its nodes, its instances, the meshes' nodes and triangles
typedef struct SwHeader {
    VkDeviceAddress nodes, inst, mesh_nodes, mesh_tris;
    uint32_t count, pad[3];
} SwHeader;

// A triangle in a leaf (sw_rt.glsl SwTri): its corners, the first's w its index in the mesh (the hit's primitive)
typedef struct SwTri {
    float p0[3];
    uint32_t prim;
    float p1[3], pad1;
    float p2[3], pad2;
} SwTri;

static struct {
    bool on;
    GpuBuf mesh_nodes, mesh_tris;
    uint64_t root[RE_MAX_MESHES];
    // the sets (0 static, 1 dynamic), one buffer a frame slot (host memory: written here, read by that frame's rays)
    GpuBuf set_buf[2][RE_FRAMES_IN_FLIGHT];
    SwStats stats;
} S;

const SwStats* swrt_stats(void) { return &S.stats; }
uint64_t swrt_mesh_root(int mesh) { return S.on && mesh >= 0 && mesh < RE_MAX_MESHES ? S.root[mesh] : 0; }

bool swrt_available(VkCtx* vk) { return !vk->ray_query && !getenv("REALENGINE_NO_SWRT"); }

// --- The builder: primitives by their boxes and centroids, sorted in place by refs ---------------------------------

typedef struct Builder {
    const float *mn, *mx, *cen; // per primitive: 3 floats each
    uint32_t* ref;               // the primitives' order (the leaves' ranges index it)
    SwNode* nodes;
    uint32_t nnodes, cap;
    bool ok;
} Builder;

static float area(const float* mn, const float* mx) {
    float e0 = mx[0] - mn[0], e1 = mx[1] - mn[1], e2 = mx[2] - mn[2];
    return e0 < 0.0f ? 0.0f : 2.0f * (e0 * e1 + e1 * e2 + e2 * e0);
}
static void box_of(const Builder* b, uint32_t first, uint32_t count, float* mn, float* mx) {
    for (int k = 0; k < 3; k++) mn[k] = FLT_MAX, mx[k] = -FLT_MAX;
    for (uint32_t i = first; i < first + count; i++) {
        uint32_t r = b->ref[i];
        for (int k = 0; k < 3; k++) {
            if (b->mn[r * 3 + k] < mn[k]) mn[k] = b->mn[r * 3 + k];
            if (b->mx[r * 3 + k] > mx[k]) mx[k] = b->mx[r * 3 + k];
        }
    }
}
static uint32_t node_new(Builder* b) {
    if (b->nnodes + 1 > b->cap) {
        uint32_t c = b->cap ? b->cap * 2 : 1024;
        SwNode* n = realloc(b->nodes, sizeof(SwNode) * c);
        if (!n) { b->ok = false; return 0; }
        b->nodes = n, b->cap = c;
    }
    memset(&b->nodes[b->nnodes], 0, sizeof(SwNode));
    return b->nnodes++;
}

// The split of [first, first + count): the binned SAH on the centroids' widest axis; 0: a leaf
static uint32_t split(Builder* b, uint32_t first, uint32_t count, const float* pmn, const float* pmx) {
    if (count <= SW_LEAF) return 0;
    float cmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, cmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint32_t i = first; i < first + count; i++)
        for (int k = 0; k < 3; k++) {
            float c = b->cen[b->ref[i] * 3 + k];
            if (c < cmn[k]) cmn[k] = c;
            if (c > cmx[k]) cmx[k] = c;
        }
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (cmx[k] - cmn[k] > cmx[ax] - cmn[ax]) ax = k;
    float ext = cmx[ax] - cmn[ax];
    uint32_t mid;
    if (ext <= 0.0f) mid = count / 2; // (all at one point: halves)
    else {
        uint32_t bc[SW_BINS] = { 0 };
        float bmn[SW_BINS][3], bmx[SW_BINS][3];
        for (int i = 0; i < SW_BINS; i++)
            for (int k = 0; k < 3; k++) bmn[i][k] = FLT_MAX, bmx[i][k] = -FLT_MAX;
        float scale = (float)SW_BINS / ext;
        for (uint32_t i = first; i < first + count; i++) {
            uint32_t r = b->ref[i];
            int bi = (int)((b->cen[r * 3 + ax] - cmn[ax]) * scale);
            bi = bi < 0 ? 0 : bi >= SW_BINS ? SW_BINS - 1 : bi;
            bc[bi]++;
            for (int k = 0; k < 3; k++) {
                if (b->mn[r * 3 + k] < bmn[bi][k]) bmn[bi][k] = b->mn[r * 3 + k];
                if (b->mx[r * 3 + k] > bmx[bi][k]) bmx[bi][k] = b->mx[r * 3 + k];
            }
        }
        // the cost of each cut between bins (left sweep, right sweep)
        float lcost[SW_BINS], lmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, lmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        uint32_t ln = 0;
        for (int i = 0; i < SW_BINS - 1; i++) {
            ln += bc[i];
            for (int k = 0; k < 3; k++) lmn[k] = bmn[i][k] < lmn[k] ? bmn[i][k] : lmn[k], lmx[k] = bmx[i][k] > lmx[k] ? bmx[i][k] : lmx[k];
            lcost[i] = (float)ln * area(lmn, lmx);
        }
        float best = FLT_MAX, rmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, rmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        int cut = -1;
        uint32_t rn = 0;
        for (int i = SW_BINS - 1; i > 0; i--) {
            rn += bc[i];
            for (int k = 0; k < 3; k++) rmn[k] = bmn[i][k] < rmn[k] ? bmn[i][k] : rmn[k], rmx[k] = bmx[i][k] > rmx[k] ? bmx[i][k] : rmx[k];
            float c = lcost[i - 1] + (float)rn * area(rmn, rmx);
            if (rn && rn < count && c < best) best = c, cut = i;
        }
        // (a leaf when no cut beats testing them all: only for a few, up to 2 * SW_LEAF)
        if (cut >= 0 && count <= 2 * SW_LEAF && best >= (float)count * area(pmn, pmx)) return 0;
        mid = count / 2;
        if (cut >= 0) { // partition by the bin
            uint32_t i = first, j = first + count;
            while (i < j) {
                uint32_t r = b->ref[i];
                int bi = (int)((b->cen[r * 3 + ax] - cmn[ax]) * scale);
                bi = bi < 0 ? 0 : bi >= SW_BINS ? SW_BINS - 1 : bi;
                if (bi < cut) i++;
                else b->ref[i] = b->ref[--j], b->ref[j] = r;
            }
            mid = i - first;
        }
        if (mid == 0 || mid >= count) mid = count / 2;
    }
    return mid;
}

// A child (its descriptor and box) for [first, first + count): a leaf, or a node of its own
static void child(Builder* b, uint32_t first, uint32_t count, float* mn, float* mx, uint32_t* idx, uint32_t* cnt) {
    box_of(b, first, count, mn, mx);
    uint32_t mid = split(b, first, count, mn, mx);
    if (!mid) {
        *idx = first, *cnt = count;
        return;
    }
    uint32_t n = node_new(b);
    if (!b->ok) return;
    *idx = n, *cnt = 0;
    float amn[3], amx[3], bmn[3], bmx[3];
    uint32_t ai, ac, bi, bc;
    child(b, first, mid, amn, amx, &ai, &ac);
    child(b, first + mid, count - mid, bmn, bmx, &bi, &bc);
    if (!b->ok) return;
    SwNode* nd = &b->nodes[n];
    memcpy(nd->amn, amn, 12), memcpy(nd->amx, amx, 12), memcpy(nd->bmn, bmn, 12), memcpy(nd->bmx, bmx, 12);
    nd->a = ai, nd->acount = ac, nd->b = bi, nd->bcount = bc;
}

// The tree over n primitives: its root node (always a node: a leaf root sits in its first child)
static uint32_t build(Builder* b, uint32_t n) {
    uint32_t root = node_new(b);
    if (!b->ok) return 0;
    float mn[3], mx[3];
    uint32_t i, c;
    box_of(b, 0, n, mn, mx);
    uint32_t mid = split(b, 0, n, mn, mx);
    if (!mid) {
        SwNode* nd = &b->nodes[root];
        memcpy(nd->amn, mn, 12), memcpy(nd->amx, mx, 12);
        nd->a = 0, nd->acount = n;
        for (int k = 0; k < 3; k++) nd->bmn[k] = FLT_MAX, nd->bmx[k] = -FLT_MAX;
        nd->b = 0, nd->bcount = SW_EMPTY;
        return root;
    }
    float amn[3], amx[3], bmn[3], bmx[3];
    uint32_t ai, ac, bi, bc;
    child(b, 0, mid, amn, amx, &ai, &ac);
    child(b, mid, n - mid, bmn, bmx, &bi, &bc);
    (void)i, (void)c;
    if (!b->ok) return 0;
    SwNode* nd = &b->nodes[root];
    memcpy(nd->amn, amn, 12), memcpy(nd->amx, amx, 12), memcpy(nd->bmn, bmn, 12), memcpy(nd->bmx, bmx, 12);
    nd->a = ai, nd->acount = ac, nd->b = bi, nd->bcount = bc;
    return root;
}

// --- The meshes ------------------------------------------------------------------------------------------------------

// The meshes' BVHs made on the CPU (any thread: the meshes' worker), for swrt_commit
struct SwPrep {
    SwNode* nodes;
    SwTri* tris;
    uint32_t nnodes, ntris;
    int n;
    double ms;
    uint64_t root[RE_MAX_MESHES];
};
void swrt_drop(SwPrep* p) {
    if (!p) return;
    free(p->nodes), free(p->tris), free(p);
}

SwPrep* swrt_prepare(const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, const int32_t* prim_base, int n) {
    double t0 = re_time_seconds();
    if (!v || !idx || n <= 0) return NULL;
    uint64_t total = 0;
    for (int m = 0; m < n && m < RE_MAX_MESHES; m++) total += meshes[m].index_count / 3;
    if (!total || total > 0x7FFFFFFFu) return NULL;
    SwPrep* pr = calloc(1, sizeof *pr);
    if (!pr) return NULL;
    SwTri* tris = malloc(sizeof(SwTri) * total);
    Builder b = { 0 };
    b.ok = tris != NULL;
    uint32_t tri_base = 0, maxt = 0;
    for (int m = 0; m < n && m < RE_MAX_MESHES; m++) maxt = meshes[m].index_count / 3 > maxt ? meshes[m].index_count / 3 : maxt;
    float* mn = malloc(sizeof(float) * 3 * (maxt ? maxt : 1));
    float* mx = malloc(sizeof(float) * 3 * (maxt ? maxt : 1));
    float* cen = malloc(sizeof(float) * 3 * (maxt ? maxt : 1));
    uint32_t* ref = malloc(sizeof(uint32_t) * (maxt ? maxt : 1));
    if (!mn || !mx || !cen || !ref) b.ok = false;
    for (int m = 0; b.ok && m < n && m < RE_MAX_MESHES; m++) {
        uint32_t nt = meshes[m].index_count / 3, vo = (uint32_t)meshes[m].vertex_offset;
        if (!nt) continue;
        const uint32_t* mi = idx + meshes[m].first_index;
        bool bad = false;
        for (uint32_t t = 0; t < nt && !bad; t++) {
            ref[t] = t;
            for (int k = 0; k < 3; k++) mn[t * 3 + k] = FLT_MAX, mx[t * 3 + k] = -FLT_MAX, cen[t * 3 + k] = 0.0f;
            for (int c = 0; c < 3; c++) {
                uint32_t g = mi[t * 3 + c] + vo;
                if (g >= nv) { bad = true; break; }
                for (int k = 0; k < 3; k++) {
                    float p = v[g].pos[k];
                    if (p < mn[t * 3 + k]) mn[t * 3 + k] = p;
                    if (p > mx[t * 3 + k]) mx[t * 3 + k] = p;
                    cen[t * 3 + k] += p / 3.0f;
                }
            }
        }
        if (bad) continue; // (a mesh with a broken index: no rays hit it)
        b.mn = mn, b.mx = mx, b.cen = cen, b.ref = ref;
        uint32_t first_node = b.nnodes;
        uint32_t root = build(&b, nt);
        if (!b.ok) break;
        // the leaves' ranges into the global triangles' array (this mesh's start), the triangles in the leaves' order
        for (uint32_t k = first_node; k < b.nnodes; k++) {
            SwNode* nd = &b.nodes[k];
            if (nd->acount && nd->acount != SW_EMPTY) nd->a += tri_base;
            if (nd->bcount && nd->bcount != SW_EMPTY) nd->b += tri_base;
        }
        for (uint32_t t = 0; t < nt; t++) {
            uint32_t o = ref[t];
            SwTri* st = &tris[tri_base + t];
            memset(st, 0, sizeof *st);
            memcpy(st->p0, v[mi[o * 3 + 0] + vo].pos, 12);
            memcpy(st->p1, v[mi[o * 3 + 1] + vo].pos, 12);
            memcpy(st->p2, v[mi[o * 3 + 2] + vo].pos, 12);
            st->prim = (uint32_t)((int32_t)o + (prim_base ? prim_base[m] : 0));
        }
        pr->root[m] = (uint64_t)root | (1ull << 32);
        tri_base += nt;
    }
    free(mn), free(mx), free(cen), free(ref);
    if (!(b.ok && b.nnodes && tri_base)) {
        free(b.nodes), free(tris), free(pr);
        return NULL;
    }
    pr->nodes = b.nodes, pr->tris = tris, pr->nnodes = b.nnodes, pr->ntris = tri_base, pr->n = n;
    pr->ms = (re_time_seconds() - t0) * 1000.0;
    return pr;
}

// The prepared BVHs onto the GPU in place of the old ones (the main thread, the GPU idle); frees them. False: none now
bool swrt_commit(VkCtx* vk, SwPrep* pr) {
    double t0 = re_time_seconds();
    gpu_buf_destroy(vk, &S.mesh_nodes), gpu_buf_destroy(vk, &S.mesh_tris);
    memset(S.root, 0, sizeof S.root);
    S.on = false;
    if (!pr) return false;
    Builder b = { 0 };
    b.nodes = pr->nodes, b.nnodes = pr->nnodes;
    SwTri* tris = pr->tris;
    uint32_t tri_base = pr->ntris;
    int n = pr->n;
    bool ok = true;
    if (ok) {
        // (device memory through a staging copy: every ray reads them)
        GpuBuf st_n = { 0 }, st_t = { 0 };
        VkDeviceSize nb = sizeof(SwNode) * b.nnodes, tb = sizeof(SwTri) * tri_base;
        ok = gpu_buf_create(vk, &S.mesh_nodes, nb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
             gpu_buf_create(vk, &S.mesh_tris, tb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
             gpu_buf_create(vk, &st_n, nb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) && gpu_buf_create(vk, &st_t, tb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        if (ok) {
            memcpy(st_n.mapped, b.nodes, nb), memcpy(st_t.mapped, tris, tb);
            VkCommandBuffer cmd = gpu_begin_once(vk);
            VkBufferCopy rn = { 0, 0, nb }, rt = { 0, 0, tb };
            vkCmdCopyBuffer(cmd, st_n.buffer, S.mesh_nodes.buffer, 1, &rn);
            vkCmdCopyBuffer(cmd, st_t.buffer, S.mesh_tris.buffer, 1, &rt);
            gpu_end_once(vk, cmd);
        }
        gpu_buf_destroy(vk, &st_n), gpu_buf_destroy(vk, &st_t);
    }
    S.stats.meshes = (uint32_t)n, S.stats.mesh_nodes = b.nnodes, S.stats.triangles = tri_base;
    S.stats.mesh_mb = ((double)sizeof(SwNode) * b.nnodes + (double)sizeof(SwTri) * tri_base) / 1048576.0;
    S.stats.mesh_ms = pr->ms + (re_time_seconds() - t0) * 1000.0;
    if (ok) memcpy(S.root, pr->root, sizeof S.root);
    free(b.nodes), free(tris), free(pr);
    if (!ok) {
        gpu_buf_destroy(vk, &S.mesh_nodes), gpu_buf_destroy(vk, &S.mesh_tris);
        memset(S.root, 0, sizeof S.root);
        printf("[RealEngine] Ray tracing por software: as BVH das malhas nao foram feitas\n");
        return false;
    }
    S.on = true;
    printf("[RealEngine] Ray tracing por software (A1): %d malhas, %u triangulos, %u nos, %.1f MB, %.0f ms\n", n, tri_base, S.stats.mesh_nodes,
           S.stats.mesh_mb, S.stats.mesh_ms);
    return true;
}

bool swrt_meshes(VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, const int32_t* prim_base, int n) {
    if (!swrt_available(vk)) return false;
    return swrt_commit(vk, swrt_prepare(v, nv, idx, meshes, prim_base, n));
}

// --- The sets of instances -------------------------------------------------------------------------------------------

VkDeviceAddress swrt_set(VkCtx* vk, int which, uint32_t frame_slot, const SwInst* inst, uint32_t n) {
    if (!S.on || !n || which < 0 || which > 1 || frame_slot >= RE_FRAMES_IN_FLIGHT) return 0;
    double t0 = re_time_seconds();
    Builder b = { 0 };
    b.ok = true;
    float* mn = malloc(sizeof(float) * 3 * n);
    float* mx = malloc(sizeof(float) * 3 * n);
    float* cen = malloc(sizeof(float) * 3 * n);
    uint32_t* ref = malloc(sizeof(uint32_t) * n);
    VkDeviceAddress out = 0;
    if (mn && mx && cen && ref) {
        for (uint32_t i = 0; i < n; i++) {
            ref[i] = i;
            for (int k = 0; k < 3; k++) mn[i * 3 + k] = inst[i].mn[k], mx[i * 3 + k] = inst[i].mx[k], cen[i * 3 + k] = (inst[i].mn[k] + inst[i].mx[k]) * 0.5f;
        }
        b.mn = mn, b.mx = mx, b.cen = cen, b.ref = ref;
        build(&b, n);
        if (b.ok) {
            // the buffer: the header, the nodes, the instances (slot | mask << 24) in the leaves' order
            VkDeviceSize need = sizeof(SwHeader) + sizeof(SwNode) * b.nnodes + sizeof(uint32_t) * n;
            GpuBuf* buf = &S.set_buf[which][frame_slot];
            if (buf->size < need) {
                vkDeviceWaitIdle(vk->device); // (rare: the set grew past its buffer)
                gpu_buf_destroy(vk, buf);
                if (!gpu_buf_create(vk, buf, need * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) memset(buf, 0, sizeof *buf);
            }
            if (buf->mapped) {
                SwHeader h = { 0 };
                h.nodes = buf->addr + sizeof(SwHeader);
                h.inst = h.nodes + sizeof(SwNode) * b.nnodes;
                h.mesh_nodes = S.mesh_nodes.addr, h.mesh_tris = S.mesh_tris.addr;
                h.count = n;
                uint8_t* p = buf->mapped;
                memcpy(p, &h, sizeof h);
                memcpy(p + sizeof h, b.nodes, sizeof(SwNode) * b.nnodes);
                uint32_t* ii = (uint32_t*)(p + sizeof h + sizeof(SwNode) * b.nnodes);
                for (uint32_t i = 0; i < n; i++) ii[i] = (inst[ref[i]].slot & 0xFFFFFFu) | (inst[ref[i]].mask << 24);
                out = buf->addr;
            }
        }
    }
    free(mn), free(mx), free(cen), free(ref), free(b.nodes);
    if (which == 0) S.stats.static_inst = n;
    else S.stats.dyn_inst = n;
    S.stats.set_ms = (re_time_seconds() - t0) * 1000.0;
    return out;
}

void swrt_shutdown(VkCtx* vk) {
    gpu_buf_destroy(vk, &S.mesh_nodes), gpu_buf_destroy(vk, &S.mesh_tris);
    for (int w = 0; w < 2; w++)
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &S.set_buf[w][f]);
    memset(&S, 0, sizeof S);
}
