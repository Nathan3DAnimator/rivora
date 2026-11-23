// (Q17) Nanite Tessellation: the CPU side (tess.h). The GPU does the work: tess.comp's four steps per DAG phase, then
// one indirect draw per pattern (TESS_MAX of them) through the opaque pipelines' tess twins, tess.vert (Q22) placing each micro
// vertex. Here: the patterns (once), the buffer (on the first displaced material), the budget (read back a frame late).
#include "tess.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gi.h"
#include "material.h"
#include "vkctx.h"
#include "re_sync.h"

static const uint32_t s_tess_comp[] =
#include "tess.comp.inc"
    ;

#define TESS_DUP 0x80000000u
#define TESS_READ_WORDS 16u

static struct {
    VkCtx* vk;
    VkPipeline pipe;
    GpuBuf buf;                          // header, draws, triangles, records (TESS_BYTES)
    GpuBuf pat_i;                        // the patterns' triangles (batched: TESS_BATCH)
    GpuBuf read[RE_FRAMES_IN_FLIGHT];    // the header for the CPU
    bool read_valid[RE_FRAMES_IN_FLIGHT];
    bool tried;                          // (the buffer: made once, or failed once)
    bool on, subdiv, seams;
    float px, max_factor, budget, scale;
    TessStats st;
} T;

static bool upload_buf(VkCtx* vk, GpuBuf* dst, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    GpuBuf staging;
    if (!gpu_buf_create(vk, dst, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) return false;
    if (!gpu_buf_create(vk, &staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return gpu_buf_destroy(vk, dst), false;
    memcpy(staging.mapped, data, (size_t)size);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy r = { 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, dst->buffer, 1, &r);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &staging);
    return true;
}

bool tess_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&T, 0, sizeof T);
    T.vk = vk, T.scale = 1.0f;
    T.pipe = gpu_compute_pipeline(vk, layout, s_tess_comp, sizeof s_tess_comp);
    // the patterns N = 1..TESS_MAX: (N+1)(N+2)/2 points (i along the 0 -> 1 edge, j along 0 -> 2), N^2 triangles in the
    // base's winding, repeated TESS_BATCH(N) times (an instance carries that many base triangles: one triangle an
    // instance left the vertex shader's waves nearly empty, 9 ms of main pass however few micro triangles). Each index
    // is the point itself (i | j << 6 | N - 1 << 12 | copy << 17, under 2^24): mesh.vert decodes it, no table read
    uint32_t ni = 0;
    for (uint32_t n = 1; n <= TESS_MAX; n++) ni += TESS_BATCH(n) * 3 * n * n;
    uint32_t* pv = NULL;
    uint32_t* pi = malloc(sizeof(uint32_t) * ni);
    bool ok = T.pipe && pi;
    if (ok) {
        uint32_t ib = 0;
        for (uint32_t n = 1; n <= TESS_MAX; n++)
            for (uint32_t r = 0; r < TESS_BATCH(n); r++) {
#define PT(i, j) ((uint32_t)(i) | ((uint32_t)(j) << 6) | ((n - 1u) << 12) | (r << 17))
                for (uint32_t j = 0; j < n; j++)
                    for (uint32_t i = 0; i + j < n; i++) {
                        pi[ib++] = PT(i, j), pi[ib++] = PT(i + 1, j), pi[ib++] = PT(i, j + 1);
                        if (i + j + 1 < n) pi[ib++] = PT(i + 1, j), pi[ib++] = PT(i + 1, j + 1), pi[ib++] = PT(i, j + 1);
                    }
#undef PT
            }
        ok = ib == ni && upload_buf(vk, &T.pat_i, pi, sizeof(uint32_t) * ni, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    free(pv), free(pi);
    if (!ok) printf("[RealEngine] Geometry Tessellation: off (its pipeline or patterns not made)\n");
    return ok;
}

void tess_shutdown(VkCtx* vk) {
    if (T.pipe) vkDestroyPipeline(vk->device, T.pipe, NULL);
    gpu_buf_destroy(vk, &T.buf), gpu_buf_destroy(vk, &T.pat_i);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &T.read[f]);
    memset(&T, 0, sizeof T);
}

bool tess_prepare(VkCtx* vk, const GiSettings* gi, bool can_dice, bool any) {
    static int no_tess = -1;
    if (no_tess < 0) no_tess = getenv("REALENGINE_NO_TESS") != NULL; // (the measure's "without")
    // on: the displaced materials' base vertices move (any mesh, any pass); diced too where their DAG is drawn
    T.on = T.pipe && T.pat_i.buffer && gi && gi->tessellation && !no_tess && any;
    T.px = gi && gi->tess_px > 0.25f ? gi->tess_px : 2.0f;
    T.max_factor = gi ? (float)(gi->tess_max < 1 ? 1 : gi->tess_max > (int)TESS_MAX ? (int)TESS_MAX : gi->tess_max) : 1.0f;
    T.budget = gi && gi->tess_budget > 0.05f ? gi->tess_budget * 1e6f : 4e6f;
    { const char* e = getenv("REALENGINE_TESS_PX"); if (e && atof(e) > 0.25) T.px = (float)atof(e); } // (the measures: px per micro edge)
    { const char* e = getenv("REALENGINE_TESS_MAX"); if (e && atoi(e) >= 1) T.max_factor = (float)(atoi(e) > (int)TESS_MAX ? (int)TESS_MAX : atoi(e)); }
    { const char* e = getenv("REALENGINE_TESS_BUDGET"); if (e && atof(e) > 0.0) T.budget = (float)(atof(e) * 1e6); } // (the budget test: millions)
    T.subdiv = T.on && can_dice && T.max_factor > 1.0f;
    if (T.subdiv && !T.buf.buffer && !T.tried) {
        T.tried = true;
        bool ok = gpu_buf_create(vk, &T.buf, TESS_BYTES,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 false);
        for (int f = 0; ok && f < RE_FRAMES_IN_FLIGHT; f++) ok = gpu_buf_create(vk, &T.read[f], TESS_READ_WORDS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!ok) {
            gpu_buf_destroy(vk, &T.buf);
            for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &T.read[f]);
            printf("[RealEngine] Geometry Tessellation: its buffer (%.0f MB) not made: displaced only\n", TESS_BYTES / 1048576.0);
        } else
            printf("[RealEngine] Geometry Tessellation: on (%.0f MB, %u base triangles a frame, patterns 1..%u)\n", TESS_BYTES / 1048576.0,
                   TESS_MAX_TRIS, TESS_MAX);
    }
    if (!T.buf.buffer) T.subdiv = false;
    T.st.on = T.on, T.st.subdiv = T.subdiv, T.st.px = T.px, T.st.max_factor = T.max_factor;
    if (!T.subdiv) T.scale = 1.0f;
    return T.on;
}

// (Q22) the tess twins: base pipeline -> its copy with tess.vert
#define TESS_TWINS 96
static struct { VkPipeline base, twin; } g_twin[TESS_TWINS];
static ReLock g_twin_lock = RE_LOCK_INIT;
bool tess_twin_put(VkPipeline base, VkPipeline twin) {
    bool ok = false;
    re_lock(&g_twin_lock);
    for (int i = 0; i < TESS_TWINS && !ok; i++)
        if (!g_twin[i].base || g_twin[i].base == base) g_twin[i].base = base, g_twin[i].twin = twin, ok = true;
    re_unlock(&g_twin_lock);
    return ok;
}
VkPipeline tess_twin(VkPipeline base) {
    VkPipeline t = VK_NULL_HANDLE;
    if (!base) return t;
    re_lock(&g_twin_lock);
    for (int i = 0; i < TESS_TWINS; i++)
        if (g_twin[i].base == base) { t = g_twin[i].twin; break; }
    re_unlock(&g_twin_lock);
    return t;
}
VkPipeline tess_twin_take(VkPipeline base) {
    VkPipeline t = VK_NULL_HANDLE;
    if (!base) return t;
    re_lock(&g_twin_lock);
    for (int i = 0; i < TESS_TWINS; i++)
        if (g_twin[i].base == base) { t = g_twin[i].twin, g_twin[i].base = g_twin[i].twin = VK_NULL_HANDLE; break; }
    re_unlock(&g_twin_lock);
    return t;
}

bool tess_active(void) { return T.on && T.subdiv; }
void tess_set_seam_check(bool on) { T.seams = on; }
const TessStats* tess_stats(void) { return &T.st; }

void tess_fill_frame(GpuFrame* fr) {
    fr->tess_buf = T.subdiv ? T.buf.addr : 0;
    fr->tess_pat = 0; // (free: the patterns' points are their indices)
    fr->tess[0] = T.px * T.scale, fr->tess[1] = T.subdiv ? T.max_factor : 1.0f, fr->tess[2] = fr->tess[3] = 0.0f;
    fr->tess_u[0] = (T.on ? TESS_ON : 0u) | (T.subdiv ? TESS_SUBDIV : 0u) | (T.subdiv && T.seams ? TESS_SEAMS : 0u);
    fr->tess_u[1] = T.subdiv ? TESS_MAX_TRIS : 0u;
    static int dbg = -1; // (the seam check's diagnosis: 1 the cut's edges alone, 2 the points without the factors)
    if (dbg < 0) dbg = (getenv("REALENGINE_TESS_SEAMS_TOPO") ? 1 : 0) | (getenv("REALENGINE_TESS_SEAMS_E1") ? 2 : 0);
    fr->tess_u[2] = (uint32_t)dbg, fr->tess_u[3] = 0u;
}

void tess_reset(VkCommandBuffer cmd) {
    if (!T.subdiv) return;
    vkCmdFillBuffer(cmd, T.buf.buffer, 0, 64, 0); // (words 0-15: the cursors, the sums, the seam check)
}

void tess_record(VkCommandBuffer cmd, VkPipelineLayout layout, const VkDescriptorSet sets[2], Push push, uint32_t set) {
    if (!T.subdiv) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    gpu_barrier(cmd, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, T.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0, NULL);
    push.pad = set;
    for (uint32_t s = 0; s < 4; s++) {
        push.pass = s;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        if (s == 1 || s == 3) vkCmdDispatchIndirect(cmd, T.buf.buffer, 16);
        else vkCmdDispatch(cmd, 1, 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                    RW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

uint32_t tess_draws(VkCommandBuffer cmd, VkPipeline p, uint32_t set, VkBuffer index) {
    if (!T.subdiv || !p) return 0;
    vkCmdBindIndexBuffer(cmd, T.pat_i.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDrawIndexedIndirect(cmd, T.buf.buffer, TESS_CMDS_AT + (VkDeviceSize)set * TESS_MAX * 20u, TESS_MAX, 20);
    vkCmdBindIndexBuffer(cmd, index, 0, VK_INDEX_TYPE_UINT32);
    return 1;
}

// (REALENGINE_TESS_DUMP, the seam check's diagnosis: the cut's triangles read back, their canonical edges counted on the
// CPU against the largest DAG mesh's table)
#define DBG_TRIS 65536u
static GpuBuf g_dbg;
static uint32_t* g_dbg_canon;
static float (*g_dbg_pos)[3];
static uint32_t g_dbg_nv;
static void dbg_analyze(const uint32_t* ix, uint32_t ntri) {
    if (!g_dbg_canon) return;
    uint32_t cap = 1;
    while (cap < ntri * 8u) cap <<= 1;
    uint64_t* key = calloc(cap, 8);
    uint32_t* cnt = calloc(cap, 4);
    if (!key || !cnt) return;
    uint32_t bad_idx = 0;
    for (uint32_t t = 0; t < ntri; t++)
        for (int k = 0; k < 3; k++) {
            uint32_t a = ix[t * 3 + k], b = ix[t * 3 + (k + 1) % 3];
            if (a >= g_dbg_nv || b >= g_dbg_nv) { bad_idx++; continue; }
            uint32_t ca = g_dbg_canon[a] & 0x7FFFFFFFu, cb = g_dbg_canon[b] & 0x7FFFFFFFu;
            if (ca == cb) continue;
            uint64_t kk = ca < cb ? ((uint64_t)ca << 32 | cb) : ((uint64_t)cb << 32 | ca);
            uint32_t h = (uint32_t)((kk * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1u);
            while (cnt[h] && key[h] != kk) h = (h + 1u) & (cap - 1u);
            key[h] = kk, cnt[h]++;
        }
    uint32_t odd = 0, edges = 0, shown = 0, hist[6] = { 0 };
    for (uint32_t h = 0; h < cap; h++)
        if (cnt[h]) {
            edges++, hist[cnt[h] < 5 ? cnt[h] : 5]++;
            if (cnt[h] & 1u) {
                odd++;
                if (shown++ < 12) {
                    uint32_t a = (uint32_t)(key[h] >> 32), b = (uint32_t)key[h];
                    printf("[RealEngine] Tess dump: odd edge x%u: %u (%.4f %.4f %.4f%s) - %u (%.4f %.4f %.4f%s)\n", cnt[h], a, g_dbg_pos[a][0], g_dbg_pos[a][1],
                           g_dbg_pos[a][2], g_dbg_canon[a] >> 31 ? " dup" : "", b, g_dbg_pos[b][0], g_dbg_pos[b][1], g_dbg_pos[b][2], g_dbg_canon[b] >> 31 ? " dup" : "");
                }
            }
        }
    printf("[RealEngine] Tess dump: %u triangles, %u edges (x1 %u, x2 %u, x3 %u, x4 %u, more %u), %u odd, %u bad indices\n", ntri, edges, hist[1], hist[2], hist[3],
           hist[4], hist[5], odd, bad_idx);
    free(key), free(cnt);
}

void tess_readback(VkCommandBuffer cmd, uint32_t slot, bool on) {
    if (slot >= RE_FRAMES_IN_FLIGHT) return;
    T.read_valid[slot] = false;
    if (!on || !T.subdiv || !T.read[slot].buffer) return;
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy r = { 0, 0, TESS_READ_WORDS * 4 };
    vkCmdCopyBuffer(cmd, T.buf.buffer, T.read[slot].buffer, 1, &r);
    T.read_valid[slot] = true;
    if (T.seams && g_dbg_canon && T.st.seam_frames == 40u) {
        if (!g_dbg.buffer) gpu_buf_create(T.vk, &g_dbg, (VkDeviceSize)DBG_TRIS * 12u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkBufferCopy d = { TESS_IDX_AT, 0, (VkDeviceSize)DBG_TRIS * 12u };
        if (g_dbg.buffer) vkCmdCopyBuffer(cmd, T.buf.buffer, g_dbg.buffer, 1, &d);
    }
}

void tess_collect(uint32_t slot, double gpu_ms) {
    if (slot >= RE_FRAMES_IN_FLIGHT || !T.read_valid[slot] || !T.read[slot].mapped) {
        if (!T.subdiv) T.st.micro = 0, T.st.base = 0, T.st.gpu_ms = 0.0, T.st.scale = 1.0f;
        return;
    }
    uint32_t h[TESS_READ_WORDS];
    memcpy(h, T.read[slot].mapped, sizeof h); // (uncached memory: one copy)
    T.st.micro = h[8], T.st.base = h[9], T.st.fallback = h[13], T.st.gpu_ms = gpu_ms;
    // the budget: past it, the px per micro edge scaled up by the square root (micro triangles go as 1 / px^2); under
    // 70% of it, eased back toward the preset's
    double over = T.budget > 0.0f ? (double)h[8] / T.budget : 0.0;
    if (over > 1.0) T.scale = (float)fmin(T.scale * sqrt(over) * 1.03, 64.0);
    else if (over < 0.7 && T.scale > 1.0f) T.scale = fmaxf(1.0f, T.scale * 0.98f);
    T.st.scale = T.scale;
    if (T.seams) {
        T.st.seam_xor[0] = h[10], T.st.seam_xor[1] = h[11], T.st.seam_edges = h[12];
        // (the tess_seams test: after the warm-up, every frame's edges hashed in both triangles along them cancel; one
        // that doesn't is a crack)
        if (T.st.seam_frames == 44u && g_dbg.mapped) dbg_analyze(g_dbg.mapped, h[0] < DBG_TRIS ? h[0] : DBG_TRIS);
        static uint32_t bad, edges, frames;
        if (++T.st.seam_frames > 30u) {
            frames++, edges += h[12] / 3u;
            if (h[10] | h[11]) bad++;
            if (frames == 60u)
                printf("[RealEngine] Tess seams: %u frames, %u base triangles a frame (%u fallbacks), %u frames with an unmatched edge: %s\n", frames,
                       frames ? edges / frames : 0u, h[13], bad, bad == 0u && edges > 0u ? "OK" : "FAIL");
        }
    }
}

// --- The canonical table (render.c, each DAG mesh) ---

static uint32_t hash_cell(int64_t x, int64_t y, int64_t z) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)y * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)z * 0x165667B19E3779F9ull;
    return (uint32_t)(h ^ (h >> 29));
}

uint32_t* tess_canon_table(const GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, uint32_t* nwords) {
    *nwords = 0;
    uint32_t cap = 1;
    while (cap < nv * 2u) cap <<= 1;
    uint32_t* slot = malloc(sizeof(uint32_t) * cap);
    uint32_t* next = malloc(sizeof(uint32_t) * (nv ? nv : 1u));
    uint32_t* out = malloc(sizeof(uint32_t) * ((size_t)nv + 2u));
    if (!slot || !next || !out) return free(slot), free(next), free(out), NULL;
    memset(slot, 0xFF, sizeof(uint32_t) * cap);
    // each vertex's first one at its position, within eps on every axis (a UV seam's or a pole's copies: an exporter
    // writes them on the same floats, a generated sphere's seam at cos(2 pi) a hair apart): cells of eps, the 27
    // around searched
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], v[i].pos[k]), mx[k] = fmaxf(mx[k], v[i].pos[k]);
    float ext = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
    double eps = fmax(1e-4, (double)ext * 2e-6), inv = 1.0 / eps;
    for (uint32_t i = 0; i < nv; i++) {
        int64_t c[3];
        for (int k = 0; k < 3; k++) c[k] = (int64_t)floor((double)v[i].pos[k] * inv);
        uint32_t found = UINT32_MAX;
        for (int dz = -1; dz <= 1 && found == UINT32_MAX; dz++)
            for (int dy = -1; dy <= 1 && found == UINT32_MAX; dy++)
                for (int dx = -1; dx <= 1 && found == UINT32_MAX; dx++)
                    for (uint32_t s = slot[hash_cell(c[0] + dx, c[1] + dy, c[2] + dz) & (cap - 1u)]; s != UINT32_MAX; s = next[s])
                        if (fabs((double)v[s].pos[0] - v[i].pos[0]) <= eps && fabs((double)v[s].pos[1] - v[i].pos[1]) <= eps &&
                            fabs((double)v[s].pos[2] - v[i].pos[2]) <= eps) {
                            found = s;
                            break;
                        }
        if (found != UINT32_MAX) {
            out[2u + i] = found | TESS_DUP, out[2u + found] |= TESS_DUP;
            continue;
        }
        uint32_t h = hash_cell(c[0], c[1], c[2]) & (cap - 1u);
        next[i] = slot[h], slot[h] = i, out[2u + i] = i;
    }
    free(next);
    if (getenv("REALENGINE_TESS_DUMP") && nv > g_dbg_nv) { // (the diagnosis: the largest DAG mesh's table and positions)
        free(g_dbg_canon), free(g_dbg_pos);
        g_dbg_canon = malloc(sizeof(uint32_t) * nv), g_dbg_pos = malloc(sizeof(float) * 3 * nv);
        if (g_dbg_canon && g_dbg_pos) {
            memcpy(g_dbg_canon, out + 2, sizeof(uint32_t) * nv);
            for (uint32_t i = 0; i < nv; i++) memcpy(g_dbg_pos[i], v[i].pos, 12);
            g_dbg_nv = nv;
        }
    }
    free(slot);
    // the UV's units per cm of the mesh: the square root of the UV area over the surface's (the height's mip)
    double au = 0.0, aw = 0.0;
    for (uint32_t t = 0; t + 2 < ni; t += 3) {
        const GpuVertex *a = &v[idx[t]], *b = &v[idx[t + 1]], *c = &v[idx[t + 2]];
        double e1[3], e2[3];
        for (int k = 0; k < 3; k++) e1[k] = b->pos[k] - a->pos[k], e2[k] = c->pos[k] - a->pos[k];
        double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        aw += sqrt(cx * cx + cy * cy + cz * cz);
        au += fabs((double)(b->uv[0] - a->uv[0]) * (c->uv[1] - a->uv[1]) - (double)(c->uv[0] - a->uv[0]) * (b->uv[1] - a->uv[1]));
    }
    float uv_cm = aw > 0.0 ? (float)sqrt(au / aw) : 0.0f;
    out[0] = nv;
    memcpy(&out[1], &uv_cm, 4);
    *nwords = nv + 2u;
    return out;
}
