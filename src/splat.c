#include "splat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"

static const uint32_t s_splat_sort_comp[] =
#include "splat_sort.comp.inc"
    ;

#define SPLAT_MAX_ACTORS 16
#define SPLAT_BINS 65536u

typedef struct SplatGpu { float pos_op[4], cov0[4], cov1[4], extra[4]; } SplatGpu; // (splat.glsl)
typedef struct SplatActorGpu {
    float rows[3][4];
    uint32_t first, count, mode, pad;
    float look[4];
} SplatActorGpu;
typedef struct SplatFrameGpu {
    VkDeviceAddress data, actors, keys, vals, sorted, hist;
    uint32_t nactors, total, pad0, pad1;
} SplatFrameGpu;
_Static_assert(sizeof(SplatGpu) == 64 && sizeof(SplatActorGpu) == 80 && sizeof(SplatFrameGpu) == 64, "splat.glsl");

typedef struct Loaded {
    Entity e;
    uint32_t key;     // (the file and its options: loaded again when they change)
    SplatGpu* data;   // CPU copy until packed
    uint32_t n, first;
    bool used;
} Loaded;

static struct {
    struct VkCtx* vk;
    VkPipelineLayout layout;
    VkPipeline sort;
    GpuBuf data, keys, vals, sorted, hist, actors[RE_FRAMES_IN_FLIGHT], frame[RE_FRAMES_IN_FLIGHT];
    uint32_t cap, total;
    Loaded l[SPLAT_MAX_ACTORS];
    uint32_t nl, nactors, slot;
    SplatStats stats;
} SP;

const SplatStats* splat_stats(void) { return &SP.stats; }
bool splat_any(void) { return SP.nactors > 0 && SP.total > 0; }

bool splat_init(struct VkCtx* vk, VkPipelineLayout layout) {
    memset(&SP, 0, sizeof SP);
    SP.vk = vk, SP.layout = layout;
    SP.sort = gpu_compute_pipeline(vk, layout, s_splat_sort_comp, sizeof s_splat_sort_comp);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &SP.actors[i], sizeof(SplatActorGpu) * SPLAT_MAX_ACTORS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &SP.frame[i], sizeof(SplatFrameGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
            return false;
    return SP.sort != VK_NULL_HANDLE &&
           gpu_buf_create(vk, &SP.hist, (SPLAT_BINS + 8u) * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
}

static void free_work(struct VkCtx* vk) {
    gpu_buf_destroy(vk, &SP.data), gpu_buf_destroy(vk, &SP.keys), gpu_buf_destroy(vk, &SP.vals), gpu_buf_destroy(vk, &SP.sorted);
    SP.cap = 0;
}

void splat_shutdown(struct VkCtx* vk) {
    if (SP.sort) vkDestroyPipeline(vk->device, SP.sort, NULL);
    free_work(vk);
    gpu_buf_destroy(vk, &SP.hist);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &SP.actors[i]), gpu_buf_destroy(vk, &SP.frame[i]);
    for (uint32_t i = 0; i < SP.nl; i++) free(SP.l[i].data);
    memset(&SP, 0, sizeof SP);
}

// --- Loading ---

// The file's axes into the engine's (Z up): 0 as it is, 1 -Y up (3DGS / COLMAP), 2 Y up
static void axes(uint32_t up, float m[3][3]) {
    static const float id[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, ydown[3][3] = { { 1, 0, 0 }, { 0, 0, 1 }, { 0, -1, 0 } },
                       yup[3][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };
    memcpy(m, up == 1 ? ydown : up == 2 ? yup : id, sizeof id);
}
// One Gaussian from the file's numbers: center, scale (linear), rotation (w, x, y, z), color 0..1, opacity 0..1
static void make(SplatGpu* o, const float p[3], const float s[3], const float q[4], const float col[3], float op, float unit, const float A[3][3]) {
    float w = q[0], x = q[1], y = q[2], z = q[3], l = sqrtf(w * w + x * x + y * y + z * z);
    if (l < 1e-12f) w = 1.0f, x = y = z = 0.0f, l = 1.0f;
    w /= l, x /= l, y /= l, z /= l;
    float R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
                      { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
                      { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) } };
    float M[3][3], AM[3][3]; // M = R S (cm), then the axes: A M
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) M[i][j] = R[i][j] * s[j] * unit;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) AM[i][j] = A[i][0] * M[0][j] + A[i][1] * M[1][j] + A[i][2] * M[2][j];
    float C[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) C[i][j] = AM[i][0] * AM[j][0] + AM[i][1] * AM[j][1] + AM[i][2] * AM[j][2];
    for (int i = 0; i < 3; i++) o->pos_op[i] = (A[i][0] * p[0] + A[i][1] * p[1] + A[i][2] * p[2]) * unit;
    o->pos_op[3] = op;
    o->cov0[0] = C[0][0], o->cov0[1] = C[0][1], o->cov0[2] = C[0][2], o->cov0[3] = C[1][1];
    o->cov1[0] = C[1][2], o->cov1[1] = C[2][2];
    uint32_t rgba = 0;
    for (int k = 0; k < 3; k++) {
        float c = col[k] < 0.0f ? 0.0f : col[k] > 1.0f ? 1.0f : col[k];
        rgba |= (uint32_t)(sqrtf(c) * 255.0f + 0.5f) << (8 * k); // (stored sqrt: splat.vert squares it back)
    }
    memcpy(&o->cov1[2], &rgba, 4);
    o->cov1[3] = 0.0f;
    memset(o->extra, 0, sizeof o->extra);
}

// antimatter15's .splat: 32 bytes each (position, linear scale, rgba8 with the opacity, rotation as 4 bytes w x y z)
static SplatGpu* load_splat(const uint8_t* b, size_t len, uint32_t* n, float unit, const float A[3][3]) {
    uint32_t c = (uint32_t)(len / 32u);
    SplatGpu* o = c ? malloc(sizeof *o * c) : NULL;
    if (!o) return NULL;
    for (uint32_t i = 0; i < c; i++) {
        const uint8_t* r = b + 32u * i;
        float p[3], s[3], q[4], col[3];
        memcpy(p, r, 12), memcpy(s, r + 12, 12);
        for (int k = 0; k < 3; k++) col[k] = r[24 + k] / 255.0f;
        for (int k = 0; k < 4; k++) q[k] = (r[28 + k] - 128.0f) / 128.0f;
        // (sRGB colors: the shader's square is close enough to their curve)
        for (int k = 0; k < 3; k++) col[k] = col[k] * col[k];
        make(&o[i], p, s, q, col, r[27] / 255.0f, unit, A);
    }
    *n = c;
    return o;
}

// The 3DGS .ply (binary little endian, float properties): x y z, f_dc_0..2 (the color's SH band 0), opacity (a logit),
// scale_0..2 (logs), rot_0..3 (w x y z); the rest (normals, the higher SH bands) skipped
static SplatGpu* load_ply(const uint8_t* b, size_t len, uint32_t* n, float unit, const float A[3][3]) {
    const char* end = NULL;
    for (size_t i = 0; i + 11 <= len && i < 65536; i++)
        if (!memcmp(b + i, "end_header\n", 11)) { end = (const char*)b + i + 11; break; }
    if (!end || len < 4 || memcmp(b, "ply", 3)) return NULL;
    char hdr[65536];
    size_t hl = (size_t)(end - (const char*)b);
    if (hl >= sizeof hdr) return NULL;
    memcpy(hdr, b, hl), hdr[hl] = 0;
    if (!strstr(hdr, "binary_little_endian")) return NULL;
    uint32_t count = 0, nprop = 0;
    int at[17]; // x y z dc0 dc1 dc2 opacity s0 s1 s2 r0 r1 r2 r3
    static const char* const want[14] = { "x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3" };
    for (int i = 0; i < 14; i++) at[i] = -1;
    bool in_vertex = false;
    for (char* line = strtok(hdr, "\n"); line; line = strtok(NULL, "\n")) {
        char name[64], type[16];
        unsigned u;
        if (sscanf(line, "element vertex %u", &u) == 1) count = u, in_vertex = true;
        else if (!strncmp(line, "element ", 8)) in_vertex = false;
        else if (in_vertex && sscanf(line, "property %15s %63s", type, name) == 2) {
            if (strcmp(type, "float") && strcmp(type, "float32")) return NULL; // (only all-float records)
            for (int i = 0; i < 14; i++)
                if (!strcmp(name, want[i])) at[i] = (int)nprop;
            nprop++;
        }
    }
    for (int i = 0; i < 14; i++)
        if (at[i] < 0) return NULL;
    size_t stride = (size_t)nprop * 4u;
    if (!count || hl + stride * count > len) return NULL;
    SplatGpu* o = malloc(sizeof *o * count);
    if (!o) return NULL;
    const uint8_t* d = b + hl;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* r = d + stride * i;
        float v[14];
        for (int k = 0; k < 14; k++) memcpy(&v[k], r + 4u * (uint32_t)at[k], 4);
        float p[3] = { v[0], v[1], v[2] }, s[3] = { expf(v[7]), expf(v[8]), expf(v[9]) }, q[4] = { v[10], v[11], v[12], v[13] };
        float col[3] = { 0.5f + 0.28209479f * v[3], 0.5f + 0.28209479f * v[4], 0.5f + 0.28209479f * v[5] };
        make(&o[i], p, s, q, col, 1.0f / (1.0f + expf(-v[6])), unit, A);
    }
    *n = count;
    return o;
}

static bool load(Loaded* L, const CSplat* c) {
    char path[1200];
    if (c->path[0] == '/' || c->path[0] == '\\' || (c->path[0] && c->path[1] == ':')) snprintf(path, sizeof path, "%s", c->path);
    else snprintf(path, sizeof path, "%s/%s", mat_root(), c->path);
    const uint8_t* b = NULL;
    uint64_t len64 = 0;
    ReFileMap* fm = re_map_file(path, &b, &len64);
    size_t len = (size_t)len64;
    if (!fm) {
        printf("[RealEngine] aviso: Splat: nao abriu %s\n", path);
        return false;
    }
    double t0 = re_time_seconds();
    float A[3][3];
    axes(c->up_axis, A);
    float unit = c->unit > 0.0f ? c->unit : 100.0f;
    uint32_t n = 0;
    size_t pl = strlen(path);
    SplatGpu* d = pl > 4 && !strcmp(path + pl - 4, ".ply") ? load_ply(b, len, &n, unit, A) : load_splat(b, len, &n, unit, A);
    re_unmap_file(fm);
    if (!d) {
        printf("[RealEngine] aviso: Splat: %s nao e um .ply 3DGS nem um .splat\n", path);
        return false;
    }
    if (n >= (1u << 26)) n = (1u << 26) - 1u; // (the sort's value: 26 bits an actor's index)
    free(L->data);
    L->data = d, L->n = n;
    SP.stats.load_ms = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] Splat: %u gaussians from %s (%.1f ms)\n", n, path, SP.stats.load_ms);
    return true;
}

// every loaded actor's Gaussians into the device buffer (and the working buffers at that size); the GPU idled
static bool pack(void) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < SP.nl; i++) SP.l[i].first = total, total += SP.l[i].n;
    vkDeviceWaitIdle(SP.vk->device);
    if (total > SP.cap) {
        free_work(SP.vk);
        uint32_t cap = total + total / 4u + 1024u;
        VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!gpu_buf_create(SP.vk, &SP.data, (VkDeviceSize)cap * sizeof(SplatGpu), u | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !gpu_buf_create(SP.vk, &SP.keys, (VkDeviceSize)cap * 4u, u, false) || !gpu_buf_create(SP.vk, &SP.vals, (VkDeviceSize)cap * 4u, u, false) ||
            !gpu_buf_create(SP.vk, &SP.sorted, (VkDeviceSize)cap * 4u, u, false)) {
            free_work(SP.vk);
            printf("[RealEngine] aviso: Splat: sem memoria para %u gaussianas\n", total);
            return false;
        }
        SP.cap = cap;
    }
    SP.total = total;
    if (!total) return true;
    GpuBuf st;
    if (!gpu_buf_create(SP.vk, &st, (VkDeviceSize)total * sizeof(SplatGpu), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    for (uint32_t i = 0; i < SP.nl; i++) memcpy((SplatGpu*)st.mapped + SP.l[i].first, SP.l[i].data, sizeof(SplatGpu) * SP.l[i].n);
    VkCommandBuffer cmd = gpu_begin_once(SP.vk);
    VkBufferCopy rg = { 0, 0, (VkDeviceSize)total * sizeof(SplatGpu) };
    vkCmdCopyBuffer(cmd, st.buffer, SP.data.buffer, 1, &rg);
    gpu_end_once(SP.vk, cmd);
    gpu_buf_destroy(SP.vk, &st);
    return true;
}

static uint32_t splat_key(const CSplat* c) {
    uint32_t h = 2166136261u;
    for (const char* p = c->path; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t u;
    memcpy(&u, &c->unit, 4);
    return ((h ^ c->up_axis * 0x9E3779B9u) * 16777619u ^ u) | 1u;
}

void splat_fill(World* w, uint32_t slot) {
    SP.slot = slot % RE_FRAMES_IN_FLIGHT;
    SP.nactors = 0;
    SP.stats.actors = SP.stats.gaussians = 0;
    if (!SP.vk || !SP.sort || getenv("REALENGINE_NO_SPLATS")) return;
    for (uint32_t i = 0; i < SP.nl; i++) SP.l[i].used = false;
    bool changed = false;
    struct { uint32_t li; float rows[3][4]; const CSplat* c; } todo[SPLAT_MAX_ACTORS];
    uint32_t nt = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && nt < SPLAT_MAX_ACTORS; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_SPLAT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CSplat* sc = ecs_arch_column(a, COMP_SPLAT);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t r = 0; r < a->count && nt < SPLAT_MAX_ACTORS; r++) {
            if (!sc[r].path[0] || (tr[r].flags & TRANSFORM_HIDDEN_GAME)) continue;
            Loaded* L = NULL;
            for (uint32_t i = 0; i < SP.nl && !L; i++)
                if (SP.l[i].e == ents[r]) L = &SP.l[i];
            if (!L && SP.nl < SPLAT_MAX_ACTORS) L = &SP.l[SP.nl++], memset(L, 0, sizeof *L), L->e = ents[r];
            if (!L) continue;
            L->used = true;
            uint32_t key = splat_key(&sc[r]);
            if (L->key != key) {
                L->key = key;
                if (load(L, &sc[r])) changed = true;
                else free(L->data), L->data = NULL, L->n = 0, changed = true;
            }
            if (!L->n) continue;
            todo[nt].li = (uint32_t)(L - SP.l), todo[nt].c = &sc[r];
            render_transform_rows(&tr[r], todo[nt].rows);
            nt++;
        }
    }
    for (uint32_t i = 0; i < SP.nl;) {
        if (!SP.l[i].used) {
            free(SP.l[i].data);
            SP.l[i] = SP.l[--SP.nl];
            for (uint32_t k = 0; k < nt; k++)
                if (todo[k].li == SP.nl) todo[k].li = i;
            changed = true;
        } else
            i++;
    }
    if (changed && !pack()) return;
    if (!SP.total) return;
    SplatActorGpu* out = SP.actors[SP.slot].mapped;
    for (uint32_t k = 0; k < nt; k++) {
        const Loaded* L = &SP.l[todo[k].li];
        SplatActorGpu* o = &out[SP.nactors++];
        memset(o, 0, sizeof *o);
        memcpy(o->rows, todo[k].rows, sizeof o->rows);
        o->first = L->first, o->count = L->n, o->mode = todo[k].c->mode ? 1u : 0u;
        o->look[0] = todo[k].c->brightness > 0.0f ? todo[k].c->brightness : 1000.0f;
        o->look[1] = todo[k].c->opacity > 0.0f ? todo[k].c->opacity : 1.0f;
        SP.stats.actors++, SP.stats.gaussians += L->n;
    }
    SplatFrameGpu* fg = SP.frame[SP.slot].mapped;
    fg->data = SP.data.addr, fg->actors = SP.actors[SP.slot].addr, fg->keys = SP.keys.addr, fg->vals = SP.vals.addr;
    fg->sorted = SP.sorted.addr, fg->hist = SP.hist.addr, fg->nactors = SP.nactors, fg->total = SP.total;
}

void splat_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push) {
    if (!splat_any()) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // (last frame's draw read the sorted list and the arguments)
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                VK_PIPELINE_STAGE_2_CLEAR_BIT | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    vkCmdFillBuffer(cmd, SP.hist.buffer, 0, VK_WHOLE_SIZE, 0);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SP.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SP.sort);
    Push p = *push;
    p.instances = SP.frame[SP.slot].addr;
    const SplatActorGpu* acts = SP.actors[SP.slot].mapped;
    for (uint32_t a = 0; a < SP.nactors; a++) {
        p.pass = 0, p.pad = a;
        vkCmdPushConstants(cmd, SP.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (acts[a].count + 255u) / 256u, 1, 1);
    }
    gpu_barrier(cmd, CS, RW, CS, RW);
    p.pass = 1, p.pad = 0;
    vkCmdPushConstants(cmd, SP.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, 1, 1, 1);
    gpu_barrier(cmd, CS, RW, CS, RW);
    p.pass = 2;
    vkCmdPushConstants(cmd, SP.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (SP.total + 255u) / 256u, 1, 1);
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

uint32_t splat_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe) {
    if (!splat_any() || !pipe) return 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p = *push;
    p.instances = SP.frame[SP.slot].addr;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDrawIndirect(cmd, SP.hist.buffer, (VkDeviceSize)(SPLAT_BINS + 1u) * 4u, 1, 16);
    return 1;
}

// --- The examples (made when missing) ---
static bool write_all(const char* path, const void* data, size_t n) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static float frand(uint32_t* s) {
    *s ^= *s << 13, *s ^= *s >> 17, *s ^= *s << 5;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}
bool splat_examples(void) {
    char dir[1100], a[1200], b[1200];
    snprintf(dir, sizeof dir, "%s/Splats", mat_root());
    snprintf(a, sizeof a, "%s/Example_Bush2.splat", dir), snprintf(b, sizeof b, "%s/Example_Flowers.ply", dir);
    bool made = false;
    re_make_dirs(dir);
    uint32_t s = 12345u;
    if (!re_file_stat(a, NULL, NULL, NULL)) { // a bush (.splat, meters, Z up): leaves on a lumpy ball, a trunk
        const uint32_t N = 100000; // (leaves 1-2 cm across: a phone capture's Gaussians at a meter or two)
        uint8_t* buf = calloc(N, 32);
        if (buf) {
            for (uint32_t i = 0; i < N; i++) {
                uint8_t* r = buf + 32u * i;
                float p[3], sc[3];
                uint8_t col[4];
                if (i < N / 12) { // the trunk
                    float t = frand(&s), ang = frand(&s) * 6.2831853f;
                    p[0] = cosf(ang) * 0.06f, p[1] = sinf(ang) * 0.06f, p[2] = t * 0.6f;
                    sc[0] = sc[1] = 0.02f, sc[2] = 0.05f;
                    col[0] = 90, col[1] = 60, col[2] = 35;
                } else {
                    float u = frand(&s) * 2.0f - 1.0f, ang = frand(&s) * 6.2831853f, rr = sqrtf(1.0f - u * u);
                    float lump = 0.75f + 0.25f * sinf(ang * 3.0f) * cosf(u * 4.0f);
                    float rad = lump * (0.75f + 0.25f * frand(&s));
                    p[0] = rr * cosf(ang) * rad, p[1] = rr * sinf(ang) * rad, p[2] = 1.0f + u * rad * 0.8f;
                    sc[0] = 0.008f + 0.006f * frand(&s), sc[1] = sc[0] * 0.6f, sc[2] = 0.003f;
                    float g = 0.6f + 0.4f * frand(&s);
                    bool berry = frand(&s) < 0.03f;
                    col[0] = berry ? 200 : (uint8_t)(40 * g), col[1] = berry ? 30 : (uint8_t)(130 * g), col[2] = berry ? 40 : (uint8_t)(35 * g);
                }
                col[3] = 230;
                memcpy(r, p, 12), memcpy(r + 12, sc, 12), memcpy(r + 24, col, 4);
                float q[4] = { frand(&s) * 2 - 1, frand(&s) * 2 - 1, frand(&s) * 2 - 1, frand(&s) * 2 - 1 }, ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (int k = 0; k < 4; k++) r[28 + k] = (uint8_t)(q[k] / fmaxf(ql, 1e-6f) * 127.0f + 128.0f);
            }
            made = write_all(a, buf, (size_t)N * 32u);
            free(buf);
        }
    }
    if (!re_file_stat(b, NULL, NULL, NULL)) { // a flower bed (3DGS .ply: -Y up, logs and logits as trained files have)
        const uint32_t N = 40000;
        char hdr[1024];
        int hl = snprintf(hdr, sizeof hdr, "ply\nformat binary_little_endian 1.0\nelement vertex %u\nproperty float x\nproperty float y\n"
                                           "property float z\nproperty float nx\nproperty float ny\nproperty float nz\nproperty float f_dc_0\n"
                                           "property float f_dc_1\nproperty float f_dc_2\nproperty float opacity\nproperty float scale_0\n"
                                           "property float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\n"
                                           "property float rot_2\nproperty float rot_3\nend_header\n", N);
        size_t sz = (size_t)hl + (size_t)N * 17u * 4u;
        uint8_t* buf = malloc(sz);
        if (buf) {
            memcpy(buf, hdr, (size_t)hl);
            float* v = (float*)(buf + hl);
            for (uint32_t i = 0; i < N; i++, v += 17) {
                float x = (frand(&s) - 0.5f) * 3.0f, z = (frand(&s) - 0.5f) * 1.5f;
                bool flower = frand(&s) < 0.25f;
                float h = flower ? 0.25f + 0.1f * frand(&s) : 0.25f * frand(&s);
                v[0] = x, v[1] = -h, v[2] = z, v[3] = v[4] = v[5] = 0.0f;
                float c[3];
                if (flower) {
                    int kind = (int)(frand(&s) * 3.0f);
                    c[0] = kind == 0 ? 0.95f : kind == 1 ? 0.95f : 0.6f, c[1] = kind == 0 ? 0.85f : kind == 1 ? 0.3f : 0.4f, c[2] = kind == 2 ? 0.95f : 0.2f;
                } else
                    c[0] = 0.18f, c[1] = 0.45f + 0.2f * frand(&s), c[2] = 0.15f;
                for (int k = 0; k < 3; k++) v[6 + k] = (c[k] - 0.5f) / 0.28209479f;
                v[9] = 3.0f; // (a logit: 0.95)
                float sc = flower ? 0.018f : 0.03f;
                v[10] = logf(sc), v[11] = logf(flower ? sc : 0.004f), v[12] = logf(sc * 0.8f);
                v[13] = 1.0f, v[14] = frand(&s) - 0.5f, v[15] = frand(&s) - 0.5f, v[16] = frand(&s) - 0.5f;
            }
            made = write_all(b, buf, sz) || made;
            free(buf);
        }
    }
    return made;
}
