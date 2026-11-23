#include "gi_bake.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gi.h"
#include "gi_cache.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"

static const uint32_t s_bake[] =
#include "lightmap_bake.comp.inc"
    ;
static const uint32_t s_blend[] =
#include "lightmap_blend.comp.inc"
    ;

typedef struct BlendParamsGpu {
    VkDeviceAddress data;
    uint32_t n, atlas, vw, vh, has_vol, pad;
    uint64_t layer_texels, pad2;
    float w[36];
} BlendParamsGpu;
#define LB_PARAMS 4
static struct {
    VkPipeline pipe;
    GpuBuf data, params[LB_PARAMS];
    int n, dirty;
    uint32_t atlas, vw, vh, slot;
    bool has_vol;
    uint64_t layer_texels;
    float w[GI_LM_LAYERS];
} LB;

static void layers_free(VkCtx* vk) {
    if (LB.data.buffer) vkDeviceWaitIdle(vk->device), gpu_buf_destroy(vk, &LB.data);
    LB.n = 0, LB.dirty = 0;
}


#define MAX_LM 4096
#define TEXEL_CM 10.0f // the lightmap density: one texel per 10 cm
#define RLIGHT_MAGIC 0x54474C52u /* "RLGT" */
#define RLIGHT_VERSION 4u // 2: the baked probe volume after the lightmaps; 3: physical units, stored times GI_STORE; 4: the walls image
#define BOUNCE_PASSES 12 // the first passes only spread the bounces (a hit reads the lightmap itself): then the mean restarts
#define V2_TO_V3 78.125f  // old units -> lux (x20 000, editor.c) times GI_STORE (1/256, shaders/gi.glsl)
#define WARMUP_FRAMES 48  // the probe volume converges a little before the lightmaps start reading its bounces

enum { STAGE_BAKE = 0, STAGE_PREP, STAGE_UPLOAD };
#define UPLOAD_RING (RE_FRAMES_IN_FLIGHT + 1)
#define UPLOAD_SLICE ((size_t)6 << 20) // the G-buffer's bytes sent a frame while playing (~0.5 ms of memcpy)
#define UPLOAD_SLICE_LOADING ((size_t)64 << 20) // ... behind a loading screen (the frame may take longer: fewer frames)
// the texels of the G-buffer: each covered texel's world position, normal, atlas steps, walls
typedef struct Texel {
    float pos[4];   // xyz, w: 1 covered
    float nrm[4];   // xyz, w: valid (pass 2: the texel isn't inside another mesh)
    float du[4];    // the world step of one texel along the atlas x
    float dv[4];    // ... along y
    float walls[4]; // pass 2: 1 - the distance to a wall / 1.5 texels along +x, -x, +y, -y (gi.glsl atlas_read)
} Texel;

typedef struct LmEntry {
    Entity e;
    float st[4];
    uint64_t hash;
} LmEntry;

typedef struct BakeParamsGpu {
    VkDeviceAddress gbuf, accum;
    uint32_t width, height, samples_done, rays;
} BakeParamsGpu;

static struct {
    VkPipeline pipe;
    VkPipelineLayout layout;
    bool baking, finished, prepared; // prepared: pass 2 (validity, walls) ran
    uint32_t bounces;        // the bounce passes done (BOUNCE_PASSES)
    uint32_t atlas;          // the atlas's side (texels)
    uint32_t done, target, rays, warm;
    GpuBuf params, gbuf, accum;
    LmEntry entries[MAX_LM];
    int count;
    bool present;            // lightmaps exist (baked or loaded)
    // (Q16) a bake in the game: its mode, the atlas's rows a frame (the budget's controller), the next row, the rows the
    // validity pass (2) has covered, the lightmaps hidden until it ends (the dynamic GI meanwhile), the measures
    GiBakeMode mode;
    float budget_ms, rows, ms_per_row;
    // (Q16) a bake while playing starts without a hitch: its G-buffer filled by a thread (prep), then sent to the GPU a
    // slice a frame through a small ring of staging buffers (upload), then traced (bake)
    int stage;
    volatile int32_t prep_done; // 1: filled, 2: out of memory
    ReThread* prep_thread;
    Texel* cpu_gbuf;
    size_t uploaded;
    uint32_t ring_i;
    GpuBuf ring[UPLOAD_RING];
    uint32_t cursor, prep_rows, last_rows, stride;
    bool hidden;
    double t0, start_ms, gpu_max_ms, gpu_sum_ms;
    uint32_t frames, measured;
} B;

bool gi_baking(void) { return B.baking; }
GiBakeMode gi_bake_mode(void) { return B.mode; }
bool gi_lightmaps_shown(void) { return B.present && B.count > 0 && !(B.baking && B.hidden); }
float gi_bake_progress(void) { return B.target ? (float)B.done / (float)B.target : 0.0f; }
bool gi_lightmaps_present(void) { return B.present && B.count > 0; }
bool gi_bake_take_finished(void) {
    bool f = B.finished;
    B.finished = false;
    return f;
}

uint64_t gi_transform_hash(const CTransform* t, uint32_t mesh) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = (const uint8_t*)t;
    for (size_t i = 0; i < sizeof(float) * 9; i++) h = (h ^ p[i]) * 1099511628211ull;
    return (h ^ mesh) * 1099511628211ull;
}

static int entry_cmp(const void* a, const void* b) {
    Entity x = ((const LmEntry*)a)->e, y = ((const LmEntry*)b)->e;
    return x < y ? -1 : x > y;
}

// (2026-11-17, Q3: "níveis pretos ao reabrir") the key a bake keeps for a mesh: its transform and the mesh's asset id,
// never its GPU slot (the slots follow the order the meshes were read in: a sublevel opened after the persistent level
// reads them in another order than the session that baked)
static uint32_t mesh_key(uint32_t slot) {
    uint32_t id = slot >= SHAPE_COUNT ? render_mesh_id(slot) : 0;
    return id ? id : slot;
}
uint64_t gi_stable_hash(const CTransform* t, uint32_t mesh_slot) { return gi_transform_hash(t, mesh_key(mesh_slot)); }

// The entries by hash too: an actor of a sublevel comes back from the file under another handle than at the bake (the
// level's actors are copied in after the persistent level's), so a miss by entity looks for its transform and mesh
typedef struct LmByHash { uint64_t h; int i; } LmByHash;
static LmByHash g_byhash[MAX_LM];
static int hash_cmp(const void* a, const void* b) {
    uint64_t x = ((const LmByHash*)a)->h, y = ((const LmByHash*)b)->h;
    return x < y ? -1 : x > y;
}

// The entries sorted by entity (after a bake or a load): a binary search per instance, for scenes of millions
static void sort_entries(void) {
    qsort(B.entries, (size_t)B.count, sizeof(LmEntry), entry_cmp);
    for (int i = 0; i < B.count; i++) g_byhash[i].h = B.entries[i].hash, g_byhash[i].i = i;
    qsort(g_byhash, (size_t)B.count, sizeof(LmByHash), hash_cmp);
}

static int find_by_hash(uint64_t h) {
    int lo = 0, hi = B.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_byhash[mid].h == h) return g_byhash[mid].i;
        if (g_byhash[mid].h < h) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool gi_lightmap_for(Entity e, const CTransform* t, uint32_t mesh_slot, float st[4]) {
    if (!B.present) return false;
    uint64_t hs = gi_stable_hash(t, mesh_slot), hl = gi_transform_hash(t, mesh_slot); // (hl: a .rlight from before 0.43)
    int lo = 0, hi = B.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        Entity m = B.entries[mid].e;
        if (m == e) {
            if (B.entries[mid].hash != hs && B.entries[mid].hash != hl) break; // (moved since, or another actor now: by its key)
            memcpy(st, B.entries[mid].st, sizeof(float) * 4);
            return true;
        }
        if (m < e) lo = mid + 1;
        else hi = mid - 1;
    }
    int k = find_by_hash(hs);
    if (k < 0) k = find_by_hash(hl);
    if (k < 0) return false;
    memcpy(st, B.entries[k].st, sizeof(float) * 4);
    return true;
}

bool gi_bake_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&B, 0, sizeof B);
    B.layout = layout;
    memset(&LB, 0, sizeof LB);
    LB.pipe = gpu_compute_pipeline(vk, layout, s_blend, sizeof s_blend); // (no ray tracing needed: Lighting Levels everywhere)
    for (int i = 0; i < LB_PARAMS; i++)
        if (!gpu_buf_create(vk, &LB.params[i], sizeof(BlendParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    if (!gi_available()) return true;
    B.pipe = gpu_compute_pipeline(vk, layout, s_bake, sizeof s_bake);
    return gpu_buf_create(vk, &B.params, sizeof(BakeParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
}

static void free_bake_buffers(VkCtx* vk) {
    gpu_buf_destroy(vk, &B.gbuf);
    gpu_buf_destroy(vk, &B.accum);
    for (int i = 0; i < UPLOAD_RING; i++) gpu_buf_destroy(vk, &B.ring[i]);
}
// (Q16) a progressive bake's thread: the G-buffer filled away from the frame
static void prep_join(void) {
    if (B.prep_thread) re_thread_join(B.prep_thread);
    B.prep_thread = NULL;
    free(B.cpu_gbuf);
    B.cpu_gbuf = NULL;
}

void gi_bake_shutdown(VkCtx* vk) {
    prep_join();
    vkDeviceWaitIdle(vk->device);
    if (B.pipe) vkDestroyPipeline(vk->device, B.pipe, NULL);
    layers_free(vk);
    if (LB.pipe) vkDestroyPipeline(vk->device, LB.pipe, NULL);
    for (int i = 0; i < LB_PARAMS; i++) gpu_buf_destroy(vk, &LB.params[i]);
    free_bake_buffers(vk);
    gpu_buf_destroy(vk, &B.params);
    memset(&B, 0, sizeof B);
}

// --- Atlas: the surface cache's charts (gi_cache.c): rectangles sized by the largest face (square texels on a long
// wall), the texels' points pushed straight in from the triangle edges (a corner texel never slides through a thin wall),
// each texel with the world step of one texel along the atlas x and y (the walls' test) ---

typedef struct Chart {
    Entity e;
    uint32_t mesh, w, h, x, y, count, src;
    float rows[3][4];
    float scale[3];
    uint64_t hash;
    const CacheTexelGpu* t;
} Chart;

static int chart_cmp(const void* a, const void* b) { // the tallest first (the shelves), then the widest
    const Chart *x = a, *y = b;
    if (x->h != y->h) return x->h > y->h ? -1 : 1;
    return x->w > y->w ? -1 : x->w < y->w;
}

// Shelf packing of power-of-two rectangles sorted by height: false when they don't fit in side x side
static bool pack(Chart* c, int n, uint32_t side) {
    uint32_t x = 0, y = 0, row = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].w > side) return false;
        if (x + c[i].w > side) {
            y += row;
            x = 0;
            row = 0;
        }
        if (y + c[i].h > side) return false;
        c[i].x = x;
        c[i].y = y;
        if (!row) row = c[i].h;
        x += c[i].w;
    }
    return true;
}

// shaders/lightmap_bake.comp GBuf: 5 vec4 per atlas texel

static void place_chart(Texel* g, uint32_t atlas, const Chart* c) {
    const float(*R)[4] = c->rows;
    // the cofactor of the linear part (normals under non-uniform scale)
    float a[3] = { R[0][0], R[1][0], R[2][0] }, b[3] = { R[0][1], R[1][1], R[2][1] }, cc[3] = { R[0][2], R[1][2], R[2][2] };
    float cof[3][3];
    for (int k = 0; k < 3; k++) {
        int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
        cof[0][k] = b[k1] * cc[k2] - b[k2] * cc[k1];
        cof[1][k] = cc[k1] * a[k2] - cc[k2] * a[k1];
        cof[2][k] = a[k1] * b[k2] - a[k2] * b[k1];
    }
    for (uint32_t i = 0; i < c->count; i++) {
        const CacheTexelGpu* s = &c->t[i];
        uint32_t x = c->x + (s->xy & 0xFFFFu), y = c->y + (s->xy >> 16);
        if (x >= atlas || y >= atlas) continue;
        Texel* tx = &g[(size_t)y * atlas + x];
        float nl = 0.0f;
        for (int r = 0; r < 3; r++) {
            tx->pos[r] = R[r][0] * s->lpos[0] + R[r][1] * s->lpos[1] + R[r][2] * s->lpos[2] + R[r][3];
            tx->nrm[r] = cof[r][0] * s->lnrm[0] + cof[r][1] * s->lnrm[1] + cof[r][2] * s->lnrm[2];
            tx->du[r] = R[r][0] * s->ldu[0] + R[r][1] * s->ldu[1] + R[r][2] * s->ldu[2];
            tx->dv[r] = R[r][0] * s->ldv[0] + R[r][1] * s->ldv[1] + R[r][2] * s->ldv[2];
            nl += tx->nrm[r] * tx->nrm[r];
        }
        nl = nl > 0.0f ? 1.0f / sqrtf(nl) : 0.0f;
        for (int r = 0; r < 3; r++) tx->nrm[r] *= nl;
        tx->pos[3] = 1.0f;
        tx->nrm[3] = 1.0f;
    }
}

static bool upload_buffer(VkCtx* vk, GpuBuf* dst, const void* data, VkDeviceSize size) {
    GpuBuf staging;
    if (!gpu_buf_create(vk, &staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(staging.mapped, data, (size_t)size);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy region = { 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, dst->buffer, 1, &region);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &staging);
    return true;
}

// The Static meshes into charts (their bounds: the probe volume's); n
static int collect(World* w, Chart* charts, float lo[3], float hi[3]) {
    int n = 0;
    for (int k = 0; k < 3; k++) lo[k] = 1e30f, hi[k] = -1e30f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < MAX_LM; i++) {
            if ((tr[i].flags & TRANSFORM_MOBILITY_MASK) != MOBILITY_STATIC) continue;
            if (!render_mesh_has_atlas(render_mesh_slot(sm[i].mesh))) continue; // (no UV2 to bake into: mesh_build.c)
            Chart* c = &charts[n++];
            c->e = ents[i];
            c->mesh = render_mesh_slot(sm[i].mesh);
            for (int k = 0; k < 3; k++) c->scale[k] = fabsf(tr[i].scale[k]);
            render_transform_rows(&tr[i], c->rows);
            c->hash = gi_stable_hash(&tr[i], c->mesh);
            const GpuVertex* v;
            const uint32_t* idx;
            uint32_t fi, ic;
            int32_t vo;
            render_mesh_cpu(c->mesh, &v, &idx, &fi, &ic, &vo);
            for (uint32_t t = 0; t < ic; t++) {
                const float* lp = v[idx[fi + t] + vo].pos;
                for (int r = 0; r < 3; r++) {
                    float p = c->rows[r][0] * lp[0] + c->rows[r][1] * lp[1] + c->rows[r][2] * lp[2] + c->rows[r][3];
                    lo[r] = fminf(lo[r], p);
                    hi[r] = fmaxf(hi[r], p);
                }
            }
        }
    }
    return n;
}

// the smallest atlas that holds them at TEXEL_CM (the texels grow when even max_side doesn't); 0: none
static uint32_t pack_atlas(Chart* charts, int n, uint32_t max_side, float* texel_out) {
    uint32_t atlas = 0;
    float texel = TEXEL_CM;
    for (int tries = 0; !atlas && tries < 8; tries++) {
        for (int i = 0; i < n; i++) charts[i].t = gi_cache_chart(charts[i].mesh, charts[i].scale, texel, &charts[i].w, &charts[i].h, &charts[i].count);
        qsort(charts, (size_t)n, sizeof *charts, chart_cmp);
        for (uint32_t side = 512; side <= max_side && !atlas; side *= 2)
            if (pack(charts, n, side)) atlas = side;
        if (!atlas) texel *= 2.0f;
    }
    *texel_out = texel;
    return atlas;
}

// the entries (the lightmap rectangles), the shader's parameters, the counters: a bake ready to trace
static void finalize(const Chart* charts, int n, uint32_t atlas) {
    B.count = n;
    float inv = 1.0f / (float)atlas;
    for (int i = 0; i < n; i++) {
        LmEntry* e = &B.entries[i];
        e->e = charts[i].e;
        e->hash = charts[i].hash;
        e->st[0] = (float)(charts[i].w - 2 * GI_CACHE_PAD) * inv;
        e->st[1] = (float)(charts[i].h - 2 * GI_CACHE_PAD) * inv;
        e->st[2] = (float)(charts[i].x + GI_CACHE_PAD) * inv;
        e->st[3] = (float)(charts[i].y + GI_CACHE_PAD) * inv;
    }
    sort_entries();
    BakeParamsGpu* p = B.params.mapped;
    p->gbuf = B.gbuf.addr;
    p->accum = B.accum.addr;
    p->width = p->height = atlas;
    p->rays = B.rays;
    B.atlas = atlas;
    // (the game's modes start with a few rows and grow by the measure: the first sweep also finds the buried texels and
    // the walls, ~0.7 s of GPU in one frame for a 4096 atlas)
    B.rows = B.mode == GI_BAKE_EDITOR ? (float)atlas : 32.0f;
    B.present = true;
}

// (Q16) a bake while playing: everything but the lightmap images away from the frame (a thread): the charts, the
// atlas, the G-buffer, the GPU's buffers (vkAllocateMemory needs no other thread's lock); then gi_bake_frame
static Chart s_prep_charts[MAX_LM];
static int s_prep_n;
static uint32_t s_prep_atlas;
static float s_prep_texel;
static VkCtx* s_prep_vk;
static GpuImage s_prep_lm[3];
static double s_prep_ms;
static uint32_t s_prep_max; // the atlas's largest side: 2048 while playing (4x less to send), the editor's 4096 behind a loading screen
static size_t s_prep_slice; // the bytes sent a frame
static int prep_main(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    double t0 = re_time_seconds();
    s_prep_atlas = pack_atlas(s_prep_charts, s_prep_n, s_prep_max, &s_prep_texel);
    size_t texels = (size_t)s_prep_atlas * s_prep_atlas;
    Texel* g = s_prep_atlas ? calloc(texels, sizeof *g) : NULL;
    bool ok = g != NULL;
    if (ok)
        for (int i = 0; i < s_prep_n; i++) place_chart(g, s_prep_atlas, &s_prep_charts[i]);
    if (ok) ok = gpu_buf_create(s_prep_vk, &B.gbuf, texels * sizeof(Texel), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                 gpu_buf_create(s_prep_vk, &B.accum, texels * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    for (int i = 0; i < UPLOAD_RING && ok; i++) ok = gpu_buf_create(s_prep_vk, &B.ring[i], s_prep_slice, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (ok) ok = gi_lightmap_make(s_prep_vk, s_prep_lm, s_prep_atlas);
    if (!ok) free(g), g = NULL;
    B.cpu_gbuf = g;
    s_prep_ms = (re_time_seconds() - t0) * 1000.0;
    re_atomic_store(&B.prep_done, ok ? 1 : 2);
    return 0;
}

// (Q16) the host's frame, before the render (runtime_lighting.c): a bake's thread done, the lightmap images made at
// the atlas's size (the frames in flight waited for: they read the old ones) and bound, then the upload starts
void gi_bake_frame(VkCtx* vk) {
    if (!B.baking || B.stage != STAGE_PREP) return;
    int32_t d = re_atomic_load(&B.prep_done);
    if (!d) return;
    double t0 = re_time_seconds();
    if (B.prep_thread) re_thread_join(B.prep_thread), B.prep_thread = NULL;
    if (d == 2) { // (out of memory: the dynamic GI stays)
        printf("[RealEngine] Build Lighting: out of memory (atlas %u)\n", s_prep_atlas);
        gi_bake_cancel();
        return;
    }
    gi_lightmap_swap(vk, s_prep_lm); // (the frames in flight waited for: they read the old images)
    render_rebind_descriptors(vk);
    finalize(s_prep_charts, s_prep_n, s_prep_atlas);
    B.uploaded = 0, B.ring_i = 0, B.stage = STAGE_UPLOAD;
    double ms = (re_time_seconds() - t0) * 1000.0;
    B.start_ms += ms;
    printf("[RealEngine] Build Lighting: %d Static meshes, atlas %ux%u (%.0f cm texels), %u samples, in the game %s "
           "(its thread %.1f ms; the lightmaps made on the frame %.1f ms)\n",
           s_prep_n, s_prep_atlas, s_prep_atlas, s_prep_texel, B.target, B.mode == GI_BAKE_BLOCKING ? "behind its loading screen" : "while it plays", s_prep_ms, ms);
}

bool gi_bake_start(VkCtx* vk, World* w, BakeQuality q, char* why, size_t why_cap) {
    return gi_bake_start_ex(vk, w, q, GI_BAKE_EDITOR, 0.0f, why, why_cap);
}

bool gi_bake_start_ex(VkCtx* vk, World* w, BakeQuality q, GiBakeMode mode, float budget_ms, char* why, size_t why_cap) {
    double t_start = re_time_seconds();
    if (!gi_available() || !B.pipe) {
        snprintf(why, why_cap, "no hardware ray tracing");
        return false;
    }
    static Chart charts[MAX_LM];
    float lo[3], hi[3];
    int n = collect(w, charts, lo, hi);
    if (n == 0) {
        snprintf(why, why_cap, "no Static mesh (set Mobility to Static)");
        return false;
    }
    prep_join();
    B.rays = q == BAKE_PRODUCTION ? 16 : 8; // (a pass a frame: Preview 128 passes, Production 512)
    B.target = q == BAKE_PRODUCTION ? 8192 : 1024; // a room lit through an opening needs thousands (256 read as smoke)
    B.mode = mode;
    float texel = TEXEL_CM;
    uint32_t atlas = 0;
    if (mode != GI_BAKE_EDITOR) { // (Q16) the thread does the rest (the loading screen goes on drawing); the last bake's buffers freed first
        if (B.gbuf.buffer || B.ring[0].buffer) vkDeviceWaitIdle(vk->device), free_bake_buffers(vk); // (none in a game's first bake)
        memcpy(s_prep_charts, charts, (size_t)n * sizeof *charts);
        s_prep_n = n, s_prep_vk = vk;
        s_prep_max = mode == GI_BAKE_PROGRESSIVE ? 2048u : 4096u;
        s_prep_slice = mode == GI_BAKE_PROGRESSIVE ? UPLOAD_SLICE : UPLOAD_SLICE_LOADING;
        B.stage = STAGE_PREP;
        re_atomic_store(&B.prep_done, 0);
        if (!(B.prep_thread = re_thread_start(prep_main, NULL))) {
            B.stage = STAGE_BAKE;
            snprintf(why, why_cap, "no thread");
            return false;
        }
    } else {
        atlas = pack_atlas(charts, n, 4096u, &texel);
        if (!atlas) {
            snprintf(why, why_cap, "too many Static meshes for one atlas");
            return false;
        }
        // The G-buffer: each covered texel's world position, normal and atlas steps
        size_t texels = (size_t)atlas * atlas;
        Texel* g = calloc(texels, sizeof *g); // a bake, not a frame
        if (!g) {
            snprintf(why, why_cap, "out of memory");
            return false;
        }
        for (int i = 0; i < n; i++) place_chart(g, atlas, &charts[i]);
        vkDeviceWaitIdle(vk->device);
        free_bake_buffers(vk);
        bool ok = gpu_buf_create(vk, &B.gbuf, texels * sizeof(Texel), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                  gpu_buf_create(vk, &B.accum, texels * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                  upload_buffer(vk, &B.gbuf, g, texels * sizeof(Texel)) && gi_lightmap_resize(vk, atlas);
        free(g);
        if (!ok) {
            free_bake_buffers(vk);
            snprintf(why, why_cap, "out of GPU memory");
            return false;
        }
        VkCommandBuffer cmd = gpu_begin_once(vk);
        vkCmdFillBuffer(cmd, B.accum.buffer, 0, VK_WHOLE_SIZE, 0);
        gpu_end_once(vk, cmd);
        render_rebind_descriptors(vk); // the lightmap images were replaced
        finalize(charts, n, atlas);
        B.stage = STAGE_BAKE;
    }
    B.done = 0;
    B.warm = 0;
    B.prepared = false;
    B.bounces = 0;
    B.baking = true;
    // (Q16) the game's modes: the editor's one sweep of the atlas a frame, every probe each frame; a loading screen as
    // much as its budget allows (several sweeps a frame); while playing a band of rows in the frame's budget, the probes
    // in turns (a quarter a frame: the dynamic GI's own cost) and the lightmaps hidden until the end (the dynamic GI
    // lights the level meanwhile: no noisy lightmap shown converging)
    B.budget_ms = budget_ms > 0.0f ? budget_ms : mode == GI_BAKE_BLOCKING ? 30.0f : 1.0f;
    B.ms_per_row = 0.0f;
    B.cursor = B.prep_rows = B.last_rows = 0;
    B.stride = mode == GI_BAKE_PROGRESSIVE ? 4u : 1u;
    B.hidden = mode != GI_BAKE_EDITOR;
    B.gpu_max_ms = B.gpu_sum_ms = 0.0;
    B.frames = B.measured = 0;
    gi_volume_bake_stride(B.stride);
    gi_volume_bake_begin(lo, hi);
    B.t0 = re_time_seconds();
    B.start_ms = (B.t0 - t_start) * 1000.0;
    if (mode == GI_BAKE_EDITOR)
        printf("[RealEngine] Build Lighting: %d Static meshes, atlas %ux%u (%.0f cm texels), %u samples%s (started in %.1f ms)\n", n, atlas,
               atlas, texel, B.target, mode == GI_BAKE_BLOCKING ? ", in the game behind its loading screen" : "", B.start_ms);
    return true;
}

void gi_bake_stats(GiBakeStats* s) {
    memset(s, 0, sizeof *s);
    s->mode = B.mode;
    s->baking = B.baking;
    s->atlas = B.atlas;
    s->rows_per_frame = B.last_rows;
    s->budget_ms = B.budget_ms;
    s->start_ms = B.start_ms;
    s->frames = B.frames;
    s->gpu_max_ms = B.gpu_max_ms;
    s->gpu_avg_ms = B.measured ? B.gpu_sum_ms / (double)B.measured : 0.0;
    s->seconds = B.t0 > 0.0 ? re_time_seconds() - B.t0 : 0.0;
}

// (Q16) render.c: the bake's own GPU time of a frame (its timestamps) with the rows it did: the rows a frame for the budget
void gi_bake_note_gpu(double ms, uint32_t rows) {
    if (!B.baking || rows == 0 || ms <= 0.0) return;
    B.measured++;
    B.gpu_sum_ms += ms;
    if (ms > B.gpu_max_ms && B.measured > 2) B.gpu_max_ms = ms; // (the first two: the pipeline's first use)
    if (B.mode == GI_BAKE_EDITOR) return;
    float mpr = (float)ms / (float)rows; // (the mean of the cost of a row: the mean of rows a ms leans high, and overran 1 ms 2.6x)
    B.ms_per_row = B.ms_per_row > 0.0f ? B.ms_per_row * 0.8f + mpr * 0.2f : mpr;
    // (at most 4 sweeps a frame behind a loading screen, and never more than 1.5x the last frame's rows: one cheap frame
    // measured once asked 16 sweeps of a 4096 atlas, a frame of a second on the GPU)
    float cap = B.mode == GI_BAKE_BLOCKING ? (float)B.atlas * 4.0f : (float)B.atlas;
    float r = B.budget_ms / B.ms_per_row, grow = (float)rows * 1.5f;
    if (r > grow) r = grow;
    B.rows = r < 8.0f ? 8.0f : r > cap ? cap : r;
}

void gi_bake_cancel(void) {
    prep_join(); // (Q16: a progressive bake's thread)
    B.stage = STAGE_BAKE;
    B.baking = false;
    gi_volume_bake_end(VK_NULL_HANDLE, false);
    gi_volume_bake_stride(1);
}

// One pass of the bake shader over the atlas's rows y0 .. y0 + n (pass 0, 1 or 2; the band's first row above bit 4)
static void band(VkCommandBuffer cmd, Push push, uint32_t pass, uint32_t y0, uint32_t n) {
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    push.pass = pass | (y0 << 4);
    vkCmdPushConstants(cmd, B.layout, stages, 0, sizeof push, &push);
    vkCmdDispatch(cmd, (B.atlas + 7) / 8, (n + 7) / 8, 1);
}

// A sweep of the whole atlas ended: a bounce more, or the samples counted (and the bake's end)
static void sweep_done(VkCommandBuffer cmd) {
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // The bounces: a ray that hits a lightmapped surface reads the lightmap there (its light so far), so each pass adds a
    // bounce. The first passes saw fewer bounces (darker): after BOUNCE_PASSES the mean starts over from the converged
    // bounces (the lightmap keeps them meanwhile)
    if (B.bounces < BOUNCE_PASSES) {
        if (++B.bounces == BOUNCE_PASSES) {
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(cmd, B.accum.buffer, 0, VK_WHOLE_SIZE, 0);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        return;
    }
    B.done += B.rays;
    if (B.done >= B.target) {
        B.baking = false;
        B.finished = true;
        gi_volume_bake_end(cmd, true); // the volume's atlases copied out (gi_lightmaps_save writes them)
        gi_volume_bake_stride(1);
        printf("[RealEngine] Build Lighting done: %.2f s, %u frames, GPU %.2f ms a frame (%.2f at most), %u rows a frame\n",
               re_time_seconds() - B.t0, B.frames, B.measured ? B.gpu_sum_ms / (double)B.measured : 0.0, B.gpu_max_ms, B.last_rows);
    }
}

uint32_t gi_bake_step(VkCommandBuffer cmd, VkDescriptorSet set, Push push) {
    if (!B.baking) return 0;
    bool warming = B.warm < WARMUP_FRAMES * B.stride; // the probe volume first (traced by gi_update_probes; in turns: longer)
    if (warming) B.warm++;
    if (B.stage == STAGE_PREP) return 0; // (Q16) the thread still at it (gi_bake_frame takes it over)
    if (B.stage == STAGE_UPLOAD) { // (Q16) a slice of the G-buffer a frame (the accumulation cleared with the first)
        size_t total = (size_t)B.atlas * B.atlas * sizeof(Texel), nb = total - B.uploaded < s_prep_slice ? total - B.uploaded : s_prep_slice;
        GpuBuf* r = &B.ring[B.ring_i++ % UPLOAD_RING]; // (its last use: UPLOAD_RING frames ago, done)
        memcpy(r->mapped, (const uint8_t*)B.cpu_gbuf + B.uploaded, nb);
        if (!B.uploaded) vkCmdFillBuffer(cmd, B.accum.buffer, 0, VK_WHOLE_SIZE, 0);
        VkBufferCopy region = { 0, B.uploaded, nb };
        vkCmdCopyBuffer(cmd, r->buffer, B.gbuf.buffer, 1, &region);
        B.uploaded += nb;
        if (B.uploaded >= total) {
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            free(B.cpu_gbuf);
            B.cpu_gbuf = NULL;
            B.stage = STAGE_BAKE;
        }
        return 0;
    }
    if (warming) return 0;
    B.frames++;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    push.instances = B.params.addr;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, B.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, B.pipe);
    // the editor: one sweep of the whole atlas a frame (as before); the game: its budget's rows (whole sweeps on a
    // loading screen, a band of them while playing), always in steps of 8 (the shader's groups)
    uint32_t rows = B.mode == GI_BAKE_EDITOR ? B.atlas : ((uint32_t)B.rows + 7u) & ~7u;
    if (rows < 8) rows = 8;
    uint32_t left = rows, did = 0;
    for (int guard = 0; left > 0 && B.baking && guard < 64; guard++) {
        uint32_t y0 = B.cursor, n = B.atlas - y0 < left ? B.atlas - y0 : left;
        push.pad = B.done; // (the samples done so far: the rays' seed)
        if (B.prep_rows < B.atlas) { // once: which texels are inside another mesh, and the walls around each one
            band(cmd, push, 2, y0, n);
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            B.prep_rows += n;
        }
        band(cmd, push, 0, y0, n);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        band(cmd, push, 1, y0, n);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        B.cursor += n, left -= n, did += n;
        if (B.cursor >= B.atlas) B.cursor = 0, sweep_done(cmd);
    }
    B.last_rows = did;
    return did;
}

// --- .rlight ---

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u, bits;
    if (e == 0) { // zero or subnormal
        float f = (float)m * (1.0f / 16777216.0f);
        return s ? -f : f;
    }
    bits = e == 31 ? s | 0x7F800000u | (m << 13) : s | ((e + 112u) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint16_t float_to_half(float f) { // round to nearest; overflow -> the largest half
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    uint16_t s = (uint16_t)((b >> 16) & 0x8000u);
    float a = fabsf(f);
    if (!(a < 65504.0f)) return (uint16_t)(s | 0x7BFFu);
    if (a < 6.103515625e-5f) return (uint16_t)(s | (uint16_t)(a * 16777216.0f + 0.5f)); // subnormal
    memcpy(&b, &a, sizeof b);
    b += 0x1000u; // rounding
    return (uint16_t)(s | (((b >> 23) - 112u) << 10) | ((b >> 13) & 1023u));
}

// RGBA16F texels (count halves): rgb times k (an old .rlight's units)
static void scale_half_rgb(void* data, size_t count, float k) {
    uint16_t* h = data;
    for (size_t i = 0; i + 4 <= count; i += 4)
        for (int c = 0; c < 3; c++) h[i + c] = float_to_half(half_to_float(h[i + c]) * k);
}

typedef struct RlightHeader {
    uint32_t magic, version, atlas, count;
} RlightHeader;

bool gi_lightmaps_save(VkCtx* vk, const char* path) {
    if (!gi_lightmaps_present()) return false;
    size_t px = (size_t)B.atlas * B.atlas * 8; // RGBA16F
    GpuBuf staging;
    if (!gpu_buf_create(vk, &staging, px * 3, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    vkDeviceWaitIdle(vk->device);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    for (int i = 0; i < 3; i++) {
        VkBufferImageCopy r = { (VkDeviceSize)(px * (size_t)i), 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                { B.atlas, B.atlas, 1 } };
        vkCmdCopyImageToBuffer(cmd, gi_lightmap_image(i)->image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &r);
    }
    gpu_end_once(vk, cmd);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    bool ok = f != NULL;
    if (ok) {
        RlightHeader h = { RLIGHT_MAGIC, RLIGHT_VERSION, B.atlas, (uint32_t)B.count };
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(B.entries, sizeof(LmEntry), (size_t)B.count, f) == (size_t)B.count &&
             fwrite(staging.mapped, 1, px * 3, f) == px * 3;
        // v2: the probe volume (its size, 0 without one; its grid; its atlases)
        size_t vb;
        const void* vd = gi_volume_data(&vb);
        uint32_t vb32 = (uint32_t)vb;
        ok = ok && fwrite(&vb32, sizeof vb32, 1, f) == 1 &&
             (!vb || (fwrite(gi_volume(), sizeof(GiVolume), 1, f) == 1 && fwrite(vd, 1, vb, f) == vb));
        ok = (fclose(f) == 0) && ok;
    }
    gpu_buf_destroy(vk, &staging);
    return ok && re_replace_file(tmp, path);
}

bool gi_lightmaps_load(VkCtx* vk, const char* path) {
    gi_lightmaps_clear(vk);
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    RlightHeader h;
    // no ray tracing needed: the lightmaps and the probe volume are only sampled
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == RLIGHT_MAGIC && h.version >= 1 && h.version <= RLIGHT_VERSION &&
              h.atlas >= 16 && h.atlas <= 8192 && h.count <= MAX_LM;
    size_t px = ok ? (size_t)h.atlas * h.atlas * 8 : 0, imgs = ok && h.version >= 4 ? 3 : 2; // v4: + the walls
    GpuBuf staging = { 0 };
    ok = ok && fread(B.entries, sizeof(LmEntry), h.count, f) == h.count &&
         gpu_buf_create(vk, &staging, px * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) && fread(staging.mapped, 1, px * imgs, f) == px * imgs;
    if (ok && imgs < 3) memset((uint8_t*)staging.mapped + px * 2, 0, px); // no walls: plain bilinear reads (as before)
    if (ok && h.version < 3) scale_half_rgb(staging.mapped, px / 2, V2_TO_V3); // L0 (L1 is a direction)
    uint32_t vb = 0;
    if (ok && h.version >= 2 && fread(&vb, sizeof vb, 1, f) == 1 && vb == gi_volume_bytes()) {
        GiVolume v;
        void* data = malloc(vb); // a level load, not a frame
        if (data && fread(&v, sizeof v, 1, f) == 1 && fread(data, 1, vb, f) == vb) {
            if (h.version < 3) scale_half_rgb(data, vb / 3, V2_TO_V3); // the irradiance atlas: 2/3 of the bytes, RGBA16F
            gi_volume_load(vk, &v, data, vb);
        }
        free(data);
    }
    fclose(f);
    if (ok) ok = gi_lightmap_resize(vk, h.atlas);
    if (ok) {
        VkCommandBuffer cmd = gpu_begin_once(vk);
        for (int i = 0; i < 3; i++) {
            VkBufferImageCopy r = { (VkDeviceSize)(px * (size_t)i), 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                    { h.atlas, h.atlas, 1 } };
            vkCmdCopyBufferToImage(cmd, staging.buffer, gi_lightmap_image(i)->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
        gpu_end_once(vk, cmd);
        render_rebind_descriptors(vk);
        B.count = (int)h.count;
        sort_entries();
        B.atlas = h.atlas;
        B.present = true;
    } else {
        gi_volume_clear();
        render_rebind_descriptors(vk); // the volume may have replaced the probe images
    }
    gpu_buf_destroy(vk, &staging);
    return ok;
}

// --- Lighting Levels: .rlight files added (2026-11-16, sublevels.c) ---
typedef struct LmFile {
    RlightHeader h;
    LmEntry* entries;
    uint8_t* img; // the 3 images (L0, the direction, the walls), RGBA16F each
    uint32_t vb;  // the probe volume's bytes (0: none)
    GiVolume v;
    uint8_t* vol;
} LmFile;
static void lm_file_free(LmFile* f) { free(f->entries), free(f->img), free(f->vol), memset(f, 0, sizeof *f); }
static bool lm_file_read(const char* path, LmFile* out) {
    memset(out, 0, sizeof *out);
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(&out->h, sizeof out->h, 1, f) == 1 && out->h.magic == RLIGHT_MAGIC && out->h.version == RLIGHT_VERSION &&
              out->h.atlas >= 16 && out->h.atlas <= 8192 && out->h.count <= MAX_LM;
    size_t px = ok ? (size_t)out->h.atlas * out->h.atlas * 8 : 0;
    if (ok) { // (a load, not a frame)
        out->entries = malloc(sizeof(LmEntry) * (out->h.count ? out->h.count : 1));
        out->img = malloc(px * 3);
        ok = out->entries && out->img && fread(out->entries, sizeof(LmEntry), out->h.count, f) == out->h.count && fread(out->img, 1, px * 3, f) == px * 3;
    }
    if (ok && fread(&out->vb, sizeof out->vb, 1, f) == 1 && out->vb) {
        out->vol = malloc(out->vb);
        if (!out->vol || fread(&out->v, sizeof out->v, 1, f) != 1 || fread(out->vol, 1, out->vb, f) != out->vb) {
            free(out->vol), out->vol = NULL, out->vb = 0;
        }
    } else out->vb = 0;
    fclose(f);
    if (!ok) lm_file_free(out);
    return ok;
}
// The images (L0, the direction, the walls: one after another, RGBA16F) and the probe volume into the GPU, shown
static bool lightmaps_upload(VkCtx* vk, const RlightHeader* h, const LmEntry* entries, const void* img3, const GiVolume* v,
                             const void* vol, uint32_t vb) {
    gi_lightmaps_clear(vk);
    size_t px = (size_t)h->atlas * h->atlas * 8;
    GpuBuf staging = { 0 };
    bool ok = gpu_buf_create(vk, &staging, px * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (ok) {
        memcpy(B.entries, entries, sizeof(LmEntry) * h->count);
        memcpy(staging.mapped, img3, px * 3);
        if (v && vol && vb == gi_volume_bytes()) gi_volume_load(vk, v, vol, vb);
        ok = gi_lightmap_resize(vk, h->atlas);
    }
    if (ok) {
        VkCommandBuffer cmd = gpu_begin_once(vk);
        for (int i = 0; i < 3; i++) {
            VkBufferImageCopy r = { (VkDeviceSize)(px * (size_t)i), 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                    { h->atlas, h->atlas, 1 } };
            vkCmdCopyBufferToImage(cmd, staging.buffer, gi_lightmap_image(i)->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
        gpu_end_once(vk, cmd);
        render_rebind_descriptors(vk);
        B.count = (int)h->count;
        sort_entries();
        B.atlas = h->atlas;
        B.present = true;
    } else {
        gi_volume_clear();
        render_rebind_descriptors(vk);
    }
    gpu_buf_destroy(vk, &staging);
    return ok;
}

// The bakes read once and kept (a Lighting Level switched on or off in the game: no file read, only the sum and the
// upload); read again when the file changed (a new Build Lighting). At most LM_CACHE_BYTES kept: past it, the others go.
#define LM_CACHE 40
#define LM_CACHE_BYTES (1536ull << 20)
static struct LmCached { char path[760]; int64_t mtime; uint64_t size; LmFile f; } g_lmc[LM_CACHE];
static uint64_t lm_bytes(const LmFile* f) { return (uint64_t)f->h.atlas * f->h.atlas * 24 + f->vb; }
static const LmFile* lm_cached(const char* path) {
    int64_t mt = 0;
    uint64_t sz = 0;
    if (!re_file_stat(path, &sz, &mt, NULL)) return NULL;
    int free_slot = -1;
    uint64_t total = 0;
    for (int i = 0; i < LM_CACHE; i++) {
        struct LmCached* c = &g_lmc[i];
        if (!c->path[0]) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (!strcmp(c->path, path)) {
            if (c->mtime == mt && c->size == sz) return &c->f;
            lm_file_free(&c->f), c->path[0] = 0; // (baked again)
            if (free_slot < 0) free_slot = i;
            continue;
        }
        total += lm_bytes(&c->f);
    }
    LmFile f;
    if (!lm_file_read(path, &f)) return NULL;
    if (total + lm_bytes(&f) > LM_CACHE_BYTES || free_slot < 0) { // (over the budget: the others out)
        for (int i = 0; i < LM_CACHE; i++)
            if (g_lmc[i].path[0]) lm_file_free(&g_lmc[i].f), g_lmc[i].path[0] = 0;
        free_slot = 0;
    }
    struct LmCached* c = &g_lmc[free_slot];
    snprintf(c->path, sizeof c->path, "%s", path);
    c->mtime = mt, c->size = sz, c->f = f;
    return &c->f;
}

// 4 halves <-> 4 floats (F16C with AVX2; plain C elsewhere: Android)
#if defined(__AVX2__)
#include <immintrin.h>
static inline __m128 h4_load(const uint16_t* p) { return _mm_cvtph_ps(_mm_loadl_epi64((const __m128i*)p)); }
static inline void h4_store(uint16_t* p, __m128 v) { _mm_storel_epi64((__m128i*)p, _mm_cvtps_ph(v, 0)); }
#endif

// dst += src, texel by texel: L0 summed (the valid flag: either), the direction weighted by each one's luminance
static void lm_add(uint16_t* d0, uint16_t* d1, const uint16_t* s0, const uint16_t* s1, size_t texels) {
#if defined(__AVX2__)
    const __m128 lw = _mm_setr_ps(0.2126f, 0.7152f, 0.0722f, 0.0f), xyz = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
    for (size_t t = 0; t < texels; t++) {
        __m128 a0 = h4_load(d0 + t * 4), b0 = h4_load(s0 + t * 4);
        __m128 la = _mm_dp_ps(a0, lw, 0x7F), lb = _mm_dp_ps(b0, lw, 0x7F);
        __m128 a1 = h4_load(d1 + t * 4), b1 = h4_load(s1 + t * 4);
        __m128 d = _mm_and_ps(_mm_add_ps(_mm_mul_ps(a1, la), _mm_mul_ps(b1, lb)), xyz);
        __m128 l2 = _mm_dp_ps(d, d, 0x7F);
        if (_mm_cvtss_f32(l2) > 1e-20f) h4_store(d1 + t * 4, _mm_blend_ps(_mm_mul_ps(d, _mm_rsqrt_ps(l2)), a1, 8));
        __m128 sum = _mm_add_ps(a0, b0);
        h4_store(d0 + t * 4, _mm_blend_ps(sum, _mm_max_ps(a0, b0), 8));
    }
#else
    for (size_t t = 0; t < texels; t++) {
        uint16_t *p0 = d0 + t * 4, *p1 = d1 + t * 4;
        const uint16_t *q0 = s0 + t * 4, *q1 = s1 + t * 4;
        float la = 0.2126f * half_to_float(p0[0]) + 0.7152f * half_to_float(p0[1]) + 0.0722f * half_to_float(p0[2]);
        float lb = 0.2126f * half_to_float(q0[0]) + 0.7152f * half_to_float(q0[1]) + 0.0722f * half_to_float(q0[2]);
        float d[3], dl = 0.0f;
        for (int c = 0; c < 3; c++) d[c] = half_to_float(p1[c]) * la + half_to_float(q1[c]) * lb, dl += d[c] * d[c];
        if (dl > 1e-20f) {
            dl = 1.0f / sqrtf(dl);
            for (int c = 0; c < 3; c++) p1[c] = float_to_half(d[c] * dl);
        }
        for (int c = 0; c < 3; c++) p0[c] = float_to_half(half_to_float(p0[c]) + half_to_float(q0[c]));
        if (half_to_float(q0[3]) > half_to_float(p0[3])) p0[3] = q0[3];
    }
#endif
}
// the probe volume's irradiance (RGBA16F texels): RGB summed, A the first's
static void vol_add(uint16_t* d, const uint16_t* s, size_t texels) {
#if defined(__AVX2__)
    for (size_t t = 0; t < texels; t++) {
        __m128 a = h4_load(d + t * 4);
        h4_store(d + t * 4, _mm_blend_ps(_mm_add_ps(a, h4_load(s + t * 4)), a, 8));
    }
#else
    for (size_t t = 0; t < texels; t++)
        for (int c = 0; c < 3; c++) d[t * 4 + c] = float_to_half(half_to_float(d[t * 4 + c]) + half_to_float(s[t * 4 + c]));
#endif
}

// --- Lighting Levels blended on the GPU (2026-11-16, "um node Set Lighting Level Blend que possa ser dinâmico") ---
// Every layer's L0, L1 and probe irradiance in one device buffer; lightmap_blend.comp writes their weighted sum into the
// lightmap images and the probe atlas on the frames a weight changed (and a few after: the volume's own upload lands a
// frame late). A weight is a float: a timeline can fade or flicker a Lighting Level at no file read and no CPU sum.
int gi_lightmaps_layers(void) { return LB.n; }

void gi_lightmaps_layer_weight(int layer, float w) {
    if (layer < 0 || layer >= LB.n) return;
    if (w < 0.0f) w = 0.0f;
    if (LB.w[layer] == w) return;
    LB.w[layer] = w;
    LB.dirty = 3; // (this frame and the next two: a volume upload landing after it is blended again)
}

int gi_lightmaps_load_layers(VkCtx* vk, const char* const* paths, int n, const float* weights, int* layer_of) {
    for (int i = 0; i < n; i++) layer_of[i] = -1;
    if (n <= 0) {
        gi_lightmaps_clear(vk);
        return 0;
    }
    const LmFile* f[GI_LM_LAYERS];
    int idx[GI_LM_LAYERS], m = 0;
    const LmFile* base = NULL;
    for (int i = 0; i < n && m < GI_LM_LAYERS; i++) {
        const LmFile* o = lm_cached(paths[i]);
        if (!o) continue;
        if (!base) base = o;
        else if (o->h.atlas != base->h.atlas || o->h.count != base->h.count || memcmp(o->entries, base->entries, sizeof(LmEntry) * base->h.count)) {
            printf("[RealEngine] Lighting levels: %s was baked with other meshes than the rest (Build Lighting again): left out\n", paths[i]);
            continue;
        }
        f[m] = o, idx[m] = i, m++;
    }
    if (!base) { // (an older .rlight: as it was, alone)
        if (gi_lightmaps_load(vk, paths[0])) layer_of[0] = 0;
        else gi_lightmaps_clear(vk);
        return layer_of[0] == 0;
    }
    // the first layer the usual way: its entries, the images' size, the volume's distances
    if (!lightmaps_upload(vk, &base->h, base->entries, base->img, base->vb ? &base->v : NULL, base->vol, base->vb)) return 0;
    for (int k = 0; k < m; k++) layer_of[idx[k]] = k;
    if (m == 1 || !LB.pipe) return 1; // (one: nothing to blend)
    uint32_t vw = 0, vh = 0;
    gi_volume_dims(&vw, &vh);
    bool vol = base->vb && base->vb == gi_volume_bytes() && vw;
    size_t a2 = (size_t)base->h.atlas * base->h.atlas, vt = vol ? (size_t)vw * vh : 0, lt = 2 * a2 + vt;
    VkDeviceSize bytes = (VkDeviceSize)lt * 8 * m;
    if (!gpu_buf_create(vk, &LB.data, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) {
        printf("[RealEngine] Lighting levels: no GPU memory for %d layers (%.0f MB): only the first shown\n", m, (double)bytes / 1048576.0);
        for (int k = 1; k < m; k++) layer_of[idx[k]] = -1;
        return 1;
    }
    // each layer through one staging buffer (a level's load, not a frame)
    GpuBuf st;
    if (!gpu_buf_create(vk, &st, (VkDeviceSize)lt * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        gpu_buf_destroy(vk, &LB.data);
        return 1;
    }
    for (int k = 0; k < m; k++) {
        uint8_t* d = st.mapped;
        memcpy(d, f[k]->img, a2 * 16); // L0 then L1 (the walls: the first layer's, in the image already)
        if (vol) {
            if (f[k]->vb == base->vb) memcpy(d + a2 * 16, f[k]->vol, vt * 8);
            else memset(d + a2 * 16, 0, vt * 8);
        }
        VkCommandBuffer cmd = gpu_begin_once(vk);
        VkBufferCopy r = { 0, (VkDeviceSize)lt * 8 * k, (VkDeviceSize)lt * 8 };
        vkCmdCopyBuffer(cmd, st.buffer, LB.data.buffer, 1, &r);
        gpu_end_once(vk, cmd);
    }
    gpu_buf_destroy(vk, &st);
    LB.n = m, LB.atlas = base->h.atlas, LB.vw = vw, LB.vh = vh, LB.has_vol = vol, LB.layer_texels = lt;
    for (int k = 0; k < m; k++) LB.w[k] = weights ? weights[idx[k]] : 1.0f;
    LB.dirty = 3;
    printf("[RealEngine] Lighting levels: %d layers on the GPU (%.1f MB), blended by weight\n", m, (double)bytes / 1048576.0);
    return m;
}

bool gi_lightmaps_load_sum(VkCtx* vk, const char* const* paths, int n) {
    int layer_of[GI_LM_LAYERS + 1];
    if (n > GI_LM_LAYERS) n = GI_LM_LAYERS;
    return gi_lightmaps_load_layers(vk, paths, n, NULL, layer_of) > 0;
}

void gi_lightmaps_blend_step(VkCommandBuffer cmd, VkDescriptorSet set, Push push) {
    static int every = -1; // (REALENGINE_BLEND_EVERY_FRAME: blended each frame, the measure of a weight animated without pause)
    if (every < 0) every = getenv("REALENGINE_BLEND_EVERY_FRAME") != NULL;
    if (LB.n < 2 || (!LB.dirty && !every) || !B.present) return;
    if (LB.dirty) LB.dirty--;
    gpu_mark(cmd, "Lighting Levels (blend)");
    BlendParamsGpu* p = LB.params[LB.slot].mapped;
    VkDeviceAddress pa = LB.params[LB.slot].addr;
    LB.slot = (LB.slot + 1) % LB_PARAMS;
    p->data = LB.data.addr, p->n = (uint32_t)LB.n, p->atlas = LB.atlas, p->vw = LB.vw, p->vh = LB.vh, p->has_vol = LB.has_vol;
    p->layer_texels = LB.layer_texels;
    for (int k = 0; k < LB.n; k++) p->w[k] = LB.w[k];
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    // (last frame's draws and the probes' own pass read and wrote these images)
    gpu_barrier(cmd, FS | CS | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    push.instances = pa;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, B.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, LB.pipe);
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    push.pass = 0;
    vkCmdPushConstants(cmd, B.layout, stages, 0, sizeof push, &push);
    vkCmdDispatch(cmd, (LB.atlas + 7) / 8, (LB.atlas + 7) / 8, 1);
    if (LB.has_vol) {
        push.pass = 1;
        vkCmdPushConstants(cmd, B.layout, stages, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (LB.vw + 7) / 8, (LB.vh + 7) / 8, 1);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void gi_lightmaps_clear(VkCtx* vk) {
    layers_free(vk);
    B.baking = false;
    B.present = false;
    gi_volume_clear();
    B.count = 0;
    if (B.atlas) {
        vkDeviceWaitIdle(vk->device);
        free_bake_buffers(vk);
        gi_lightmap_resize(vk, 1);
        render_rebind_descriptors(vk);
        B.atlas = 0;
    }
}
