#include "material.h" // (Q4: mat_set, the Light Functions)
#include "gi_cache.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "landscape.h"
#include "render.h"
#include "gi.h"
#include "vkctx.h"

static const uint32_t s_cache[] =
#include "gi_cache.comp.inc"
    ;
static const uint32_t s_cache_sw[] = // (A1: the rays by software, sw_rt.glsl)
#include "gi_cache_sw.comp.inc"
    ;

#define CACHE_ATLAS 1024         // the atlas's side: 1 M texels (8 MB per image)
#define CACHE_MAX_CHARTS 16384
#define CACHE_RADIUS 6000.0f     // cm: the meshes within 60 m of the camera (their nearest point) get a chart
#define CACHE_MOVE 1500.0f       // the camera this far from the last allocation's center: reallocate
#define CACHE_FALLOFF 1500.0f   // cm: past this the texels grow coarser with the distance (allocate brings it nearer when the atlas is full)
#define CACHE_PAD GI_CACHE_PAD   // texels around a chart: bilinear reads stay inside it (they weigh validity)
#define CACHE_SIZES 7            // chart sides 8 .. 512 (each side on its own: a wall gets a long chart)
#define CACHE_IMAGES 5           // direct, indirect, meta (the noise and the updates averaged), walls (the distance to one: gi.glsl), the indirect filtered (pass 3: what is read)
#define CACHE_MOVING_MAX 98304u // the moving actors' texels updated a frame at most (the rest the next frames)
#define CACHE_FILL 0.7f          // the share of the atlas the charts may take (shelf packing wastes the rest)

typedef struct CacheEntryGpu { // shaders/gi_cache.comp CacheEntry
    uint32_t slot, pad[3];
    float st[4];
} CacheEntryGpu;
_Static_assert(sizeof(CacheTexelGpu) == 64 && sizeof(CacheEntryGpu) == 32, "gi_cache.comp layouts");

// A chart's texels for one shape at one size, in mesh space (the same for every mesh of that shape and size: rasterized
// once, copied per chart)
typedef struct Template {
    CacheTexelGpu* t; // xy: inside the chart
    uint32_t count;
    bool done;
    volatile int32_t ready; // (Q16) made: read without the lock
} Template;

typedef struct Chart {
    uint32_t slot, w, h, x, y, shape;
    uint32_t dyn; // a Movable actor's: its texels first in the list, a pass over them every frame (redone while it moves)
    uint32_t tfirst, tcount; // its texels in the list
} Chart;

typedef struct Candidate {
    float dist;
    uint32_t slot;
    float scale[3];
    uint32_t tris; // an imported mesh's triangles (the dense ones: cand_sides)
    float plane[2]; // a Landscape's chunk: its x and y extent (cm; its UV2 is the chunk seen from above), else 0
    uint8_t wi, hi; // the chart's sides: 8 << wi, 8 << hi
    uint8_t shape;
    uint8_t dyn;
    uint8_t ok; // it may have a chart at the density allocate reached (cand_sides)
} Candidate;

static struct {
    bool rt, active;
    VkPipeline pipe;
    VkPipelineLayout layout;
    GpuImage img[CACHE_IMAGES], tmp[CACHE_IMAGES]; // direct, indirect, meta; tmp: the kept charts during a reallocation
    GpuBuf table;            // vec4 per scene slot: its chart (x 0: none)
    GpuBuf texels;           // device: the texel list
    GpuBuf staging;          // host: the texel list to upload
    GpuBuf entries;          // host: the slots and charts the table gets
    GpuBuf moving[RE_FRAMES_IN_FLIGHT]; // host: this frame's moving texels (their indices; pass 2 and its filter, one dispatch each)
    uint32_t frame_slot;
    Template tpl[RE_MAX_MESHES][CACHE_SIZES][CACHE_SIZES]; // per mesh slot (the shapes, then the imported meshes)
    Chart charts[CACHE_MAX_CHARTS], prev[CACHE_MAX_CHARTS];
    int ncharts;
    uint32_t moving_cursor; // where the moving charts' update stopped last frame (its budget)
    uint16_t* chart_of; // per scene slot: its chart + 1 (0: none)
    Candidate* cand;
    uint32_t count, cursor, rays, per_frame;
    uint32_t dyn_count; // the Movable actors' texels (the first ones)
    uint32_t rev, changes;
    float center[3], texel;
    // the allocation's density (allocate): texels grow coarser past `falloff` cm from the camera, and all of them by
    // `coarse`, as far as needed for every mesh within reach to get a chart
    float falloff, coarse;
    float vel[3], last_cam[3], speed; // the camera's velocity (cm/s): the allocation ahead of it, more updates when fast
    double last_t;
    bool has_alloc, pending;
    uint32_t since; // frames since the last reallocation
    // the reallocation waiting for gi_cache_record
    bool upload;
    uint32_t nentries, ncopies;
    VkImageCopy copies[CACHE_MAX_CHARTS];
    uint32_t nblits; // kept charts whose sides changed (the camera came nearer): their light resampled, not dropped
    VkImageBlit blits[CACHE_MAX_CHARTS];
    GiCacheStats stats;
} C;

bool gi_cache_active(void) { return C.active && C.count > 0; }
const GiCacheStats* gi_cache_stats(void) { return &C.stats; }
VkImageView gi_cache_view(int which) { return C.img[which >= 0 && which < CACHE_IMAGES ? which : 0].view; }

static bool make_image(VkCtx* vk, GpuImage* im, uint32_t side, int which) {
    return gpu_image_create(vk, im, side, side, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

static void clear_image(VkCommandBuffer cmd, GpuImage* im) {
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
}

bool gi_cache_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&C, 0, sizeof C);
    C.rt = gi_traced(); // (by hardware or by software: A1)
    C.falloff = CACHE_FALLOFF;
    C.coarse = 1.0f;
    C.layout = layout;
    uint32_t side = C.rt ? CACHE_ATLAS : 1;
    for (int i = 0; i < CACHE_IMAGES; i++)
        if (!make_image(vk, &C.img[i], side, i) || (C.rt && !make_image(vk, &C.tmp[i], side, i))) return false;
    if (C.rt) {
        size_t texels = (size_t)CACHE_ATLAS * CACHE_ATLAS;
        if (!gpu_buf_create(vk, &C.table, (VkDeviceSize)RE_ECS_MAX_ENTITIES * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !gpu_buf_create(vk, &C.texels, (VkDeviceSize)texels * sizeof(CacheTexelGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !gpu_buf_create(vk, &C.staging, (VkDeviceSize)texels * sizeof(CacheTexelGpu), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) ||
            !gpu_buf_create(vk, &C.entries, (VkDeviceSize)CACHE_MAX_CHARTS * sizeof(CacheEntryGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
            return false;
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++)
            if (!gpu_buf_create(vk, &C.moving[f], (VkDeviceSize)CACHE_MOVING_MAX * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
        C.pipe = vk->ray_query ? gpu_compute_pipeline(vk, layout, s_cache, sizeof s_cache) : gpu_compute_pipeline(vk, layout, s_cache_sw, sizeof s_cache_sw);
        C.chart_of = calloc(RE_ECS_MAX_ENTITIES, sizeof *C.chart_of); // once, at startup
        C.cand = malloc(sizeof *C.cand * RE_ECS_MAX_ENTITIES);
        if (!C.pipe || !C.chart_of || !C.cand) return false;
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    for (int i = 0; i < CACHE_IMAGES; i++) {
        GpuImage* ims[2] = { &C.img[i], &C.tmp[i] };
        for (int k = 0; k < 2; k++) {
            if (!ims[k]->image) continue;
            gpu_transition(cmd, ims[k], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                           VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
            clear_image(cmd, ims[k]);
        }
    }
    if (C.rt) vkCmdFillBuffer(cmd, C.table.buffer, 0, VK_WHOLE_SIZE, 0);
    gpu_end_once(vk, cmd);
    return true;
}

void gi_cache_shutdown(VkCtx* vk) {
    if (C.pipe) vkDestroyPipeline(vk->device, C.pipe, NULL);
    for (int i = 0; i < CACHE_IMAGES; i++) {
        gpu_image_destroy(vk, &C.img[i]);
        gpu_image_destroy(vk, &C.tmp[i]);
    }
    gpu_buf_destroy(vk, &C.table);
    gpu_buf_destroy(vk, &C.texels);
    gpu_buf_destroy(vk, &C.staging);
    gpu_buf_destroy(vk, &C.entries);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &C.moving[f]);
    for (int s = 0; s < SHAPE_COUNT; s++)
        for (int a = 0; a < CACHE_SIZES; a++)
            for (int b = 0; b < CACHE_SIZES; b++) free(C.tpl[s][a][b].t);
    free(C.chart_of);
    free(C.cand);
    memset(&C, 0, sizeof C);
}

// --- The charts' texels: the shape's UV2 rasterized at the chart's size (conservatively: a texel any triangle touches,
// a texel center inside a triangle winning), the mesh-space point and normal per texel ---

static void oct_decode(uint32_t o, float n[3]) {
    float x = (float)(int16_t)(o & 0xFFFF) / 32767.0f, y = (float)(int16_t)(o >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ax = (1.0f - fabsf(y)) * (x >= 0 ? 1.0f : -1.0f), ay = (1.0f - fabsf(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ax, y = ay;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}

// (Q16) a bake while playing makes its charts on a thread (gi_bake.c): one template made at a time
static volatile int32_t g_tpl_lock;
static const Template* template_make(Template* T, uint32_t shape, uint32_t wi, uint32_t hi);
static const Template* template_for(uint32_t shape, uint32_t wi, uint32_t hi) {
    Template* T = &C.tpl[shape][wi][hi];
    if (re_atomic_load(&T->ready)) return T;
    while (!re_atomic_cas(&g_tpl_lock, 0, 1)) re_yield();
    if (!T->done) template_make(T, shape, wi, hi);
    re_atomic_store(&T->ready, 1);
    re_atomic_store(&g_tpl_lock, 0);
    return T;
}
static const Template* template_make(Template* T, uint32_t shape, uint32_t wi, uint32_t hi) {
    T->done = true;
    uint32_t cw = 8u << wi, chh = 8u << hi;
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t fi, ic;
    int32_t vo;
    render_mesh_cpu(shape, &v, &idx, &fi, &ic, &vo);
    // a texel's state: 0 empty, 1 touched, 2 its center inside a triangle
    uint8_t* state = calloc((size_t)cw * chh, 1);
    CacheTexelGpu* g = calloc((size_t)cw * chh, sizeof *g); // once per shape and size
    if (!state || !g) {
        free(state);
        free(g);
        return T;
    }
    float su = (float)(cw - 2 * CACHE_PAD), sv = (float)(chh - 2 * CACHE_PAD), o = (float)CACHE_PAD;
    for (uint32_t t = 0; t + 2 < ic; t += 3) {
        const GpuVertex* tv[3];
        float px[3], py[3];
        for (int k = 0; k < 3; k++) {
            tv[k] = &v[idx[fi + t + k] + vo];
            px[k] = o + tv[k]->uv2[0] * su;
            py[k] = o + tv[k]->uv2[1] * sv;
        }
        float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (fabsf(area) < 1e-8f) continue;
        // the mesh-space step of one texel along the atlas x and y (the triangle's UV2 Jacobian, inverted)
        float dux[3], duy[3];
        for (int j = 0; j < 3; j++) {
            float e1 = tv[1]->pos[j] - tv[0]->pos[j], e2 = tv[2]->pos[j] - tv[0]->pos[j];
            dux[j] = (e1 * (py[2] - py[0]) - e2 * (py[1] - py[0])) / area;
            duy[j] = (e2 * (px[1] - px[0]) - e1 * (px[2] - px[0])) / area;
        }
        float el[3];
        for (int k = 0; k < 3; k++) {
            int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
            el[k] = sqrtf((px[k2] - px[k1]) * (px[k2] - px[k1]) + (py[k2] - py[k1]) * (py[k2] - py[k1]));
        }
        int x0 = (int)floorf(fminf(px[0], fminf(px[1], px[2])) - 1), x1 = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])) + 1);
        int y0 = (int)floorf(fminf(py[0], fminf(py[1], py[2])) - 1), y1 = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])) + 1);
        for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < (int)chh; y++)
            for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < (int)cw; x++) {
                float cx = (float)x + 0.5f, cy = (float)y + 0.5f, w[3];
                bool inside = true, near = true;
                for (int k = 0; k < 3; k++) {
                    int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
                    w[k] = ((px[k2] - px[k1]) * (cy - py[k1]) - (py[k2] - py[k1]) * (cx - px[k1])) / area;
                    float dist = w[k] * fabsf(area) / fmaxf(el[k], 1e-6f);
                    inside = inside && w[k] >= 0.0f;
                    near = near && dist >= -0.75f;
                }
                if (!near) continue;
                size_t at = (size_t)y * cw + x;
                if (state[at] >= 2 || (!inside && state[at] >= 1)) continue;
                float s = 0.0f, wr[3] = { w[0], w[1], w[2] };
                for (int k = 0; k < 3; k++) s += (w[k] = fmaxf(w[k], 0.0f));
                if (!inside) {
                    // a texel the triangle only touches: its point half a texel inside the triangle, not on the edge (on
                    // an edge where a wall meets the floor half its rays start inside the floor: a bright line there).
                    // Straight in from the edge(s) it is outside of, never along them: toward the triangle's center a
                    // corner texel slid half a texel sideways, through a wall thinner than that (a wall's foot lit by the
                    // room behind it)
                    float qx = 0.0f, qy = 0.0f, dx = 0.0f, dy = 0.0f;
                    for (int k = 0; k < 3; k++) qx += px[k] * w[k] / s, qy += py[k] * w[k] / s;
                    for (int k = 0; k < 3; k++) {
                        if (wr[k] >= 0.0f) continue;
                        int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
                        float nx = -(py[k2] - py[k1]), ny = px[k2] - px[k1]; // the edge's normal, toward vertex k
                        if (nx * (px[k] - px[k1]) + ny * (py[k] - py[k1]) < 0.0f) nx = -nx, ny = -ny;
                        float nl = sqrtf(nx * nx + ny * ny);
                        if (nl > 1e-6f) dx += nx / nl, dy += ny / nl;
                    }
                    float dl = sqrtf(dx * dx + dy * dy);
                    if (dl > 1e-6f) {
                        float step = fminf(0.5f, dl) / dl;
                        qx += dx * step, qy += dy * step;
                        s = 0.0f;
                        for (int k = 0; k < 3; k++) {
                            int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
                            w[k] = fmaxf(((px[k2] - px[k1]) * (qy - py[k1]) - (py[k2] - py[k1]) * (qx - px[k1])) / area, 0.0f);
                            s += w[k];
                        }
                    }
                }
                CacheTexelGpu* tx = &g[at];
                float n[3] = { 0, 0, 0 };
                memset(tx->lpos, 0, sizeof tx->lpos);
                for (int k = 0; k < 3; k++) {
                    float vn[3];
                    oct_decode(tv[k]->normal_oct, vn);
                    for (int j = 0; j < 3; j++) {
                        tx->lpos[j] += tv[k]->pos[j] * w[k] / s;
                        n[j] += vn[j] * w[k] / s;
                    }
                }
                float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int j = 0; j < 3; j++) tx->lnrm[j] = l > 0.0f ? n[j] / l : (j == 2);
                tx->xy = (uint32_t)x | (uint32_t)y << 16;
                memcpy(tx->ldu, dux, sizeof dux);
                memcpy(tx->ldv, duy, sizeof duy);
                tx->pad0 = tx->pad1 = 0.0f;
                state[at] = inside ? 2 : 1;
            }
    }
    uint32_t count = 0;
    for (size_t i = 0; i < (size_t)cw * chh; i++)
        if (state[i]) g[count++] = g[i];
    free(state);
    T->t = g;
    T->count = count;
    return T;
}

// --- Allocation ---

static uint32_t side_index(float texels) { // the power of two (8 .. 512) at least this many texels
    uint32_t i = 0;
    while (i + 1 < CACHE_SIZES && (float)(8u << i) < texels) i++;
    return i;
}

// The UV2 Jacobian of the flat shapes' triangles (mesh space, scale 1): how far a step in u or in v goes on the surface.
// A chart is sized for its largest face (a cube's UV2 gives each face the same share whatever the scale): a wall 10 x 3
// m gets a long chart (256 x 64 at 15 cm) with square texels, not a square one with texels five times longer than wide
typedef struct TriJ { float du[3], dv[3], e1[3], e2[3]; } TriJ;
static TriJ g_trij[RE_MAX_MESHES][16];
static uint32_t g_ntri[RE_MAX_MESHES];

static void shape_jacobians(uint32_t shape) {
    if (g_ntri[shape]) return;
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t fi, ic;
    int32_t vo;
    render_mesh_cpu(shape, &v, &idx, &fi, &ic, &vo);
    for (uint32_t t = 0; t + 2 < ic && g_ntri[shape] < 16; t += 3) {
        const GpuVertex *a = &v[idx[fi + t] + vo], *b = &v[idx[fi + t + 1] + vo], *c = &v[idx[fi + t + 2] + vo];
        TriJ* j = &g_trij[shape][g_ntri[shape]++];
        float u1 = b->uv2[0] - a->uv2[0], v1 = b->uv2[1] - a->uv2[1], u2 = c->uv2[0] - a->uv2[0], v2 = c->uv2[1] - a->uv2[1];
        float det = u1 * v2 - u2 * v1;
        for (int k = 0; k < 3; k++) {
            j->e1[k] = b->pos[k] - a->pos[k];
            j->e2[k] = c->pos[k] - a->pos[k];
            j->du[k] = fabsf(det) > 1e-12f ? (j->e1[k] * v2 - j->e2[k] * v1) / det : 0.0f;
            j->dv[k] = fabsf(det) > 1e-12f ? (j->e2[k] * u1 - j->e1[k] * u2) / det : 0.0f;
        }
    }
}

static float scaled_len(const float v[3], const float s[3]) {
    float x = v[0] * s[0], y = v[1] * s[1], z = v[2] * s[2];
    return sqrtf(x * x + y * y + z * z);
}

// The chart's sides for a mesh at scale s and a texel size (cm)
static void chart_sides(uint32_t shape, const float s[3], float texel, uint8_t* wi, uint8_t* hi) {
    if (shape == SHAPE_CUBE || shape == SHAPE_PLANE) { // the largest face's own u and v lengths
        shape_jacobians(shape);
        float best = -1.0f, du = 0.0f, dv = 0.0f;
        for (uint32_t t = 0; t < g_ntri[shape]; t++) {
            const TriJ* j = &g_trij[shape][t];
            float a[3] = { j->e1[0] * s[0], j->e1[1] * s[1], j->e1[2] * s[2] }, b[3] = { j->e2[0] * s[0], j->e2[1] * s[1], j->e2[2] * s[2] };
            float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
            float area = cx * cx + cy * cy + cz * cz;
            if (area > best) best = area, du = scaled_len(j->du, s), dv = scaled_len(j->dv, s);
        }
        *wi = (uint8_t)side_index(du / texel);
        *hi = (uint8_t)side_index(dv / texel);
        return;
    }
    // the round shapes and the imported meshes: one square chart, sized for their area at the largest scale pair (an
    // imported mesh's UV2 packs its triangles in a grid: a third more room for the gaps)
    float sx = fabsf(s[0]), sy = fabsf(s[1]), sz = fabsf(s[2]);
    float k = fmaxf(sx * sy, fmaxf(sy * sz, sx * sz));
    *wi = *hi = (uint8_t)side_index(sqrtf(render_mesh_area(shape) * k) / texel * (shape >= SHAPE_COUNT ? 1.6f : 1.25f));
}

void gi_cache_meshes_changed(void) {
    for (uint32_t s = SHAPE_COUNT; s < RE_MAX_MESHES; s++) { // the imported meshes' templates: made again on use
        for (int a = 0; a < CACHE_SIZES; a++)
            for (int b = 0; b < CACHE_SIZES; b++) {
                free(C.tpl[s][a][b].t);
                memset(&C.tpl[s][a][b], 0, sizeof C.tpl[s][a][b]);
            }
        g_ntri[s] = 0;
    }
    C.pending = true; // (the charts allocated again)
}

const CacheTexelGpu* gi_cache_chart(uint32_t shape, const float s[3], float texel, uint32_t* w, uint32_t* h, uint32_t* count) {
    uint8_t wi, hi;
    if (shape >= render_mesh_count()) shape = SHAPE_CUBE;
    chart_sides(shape, s, texel, &wi, &hi);
    const Template* T = template_for(shape, wi, hi);
    *w = 8u << wi;
    *h = 8u << hi;
    *count = T->t ? T->count : 0;
    return T->t;
}

// The chart's sides for a candidate at the allocation's density (C.texel, C.coarse, C.falloff): finer near the camera,
// coarser away from it (the same texels on screen, more meshes in the atlas). A dense small mesh (a character's part:
// hundreds of triangles in a few hundred texels) would get a texel or two per triangle, each its own light: a mosaic. It
// gets no chart (false: the probes light it smoothly), decided at the near density, never by the distance: far away its
// coarser chart grows back to 6 texels a triangle (the mesh had the cache near the camera and the probes away from it:
// its indirect light showed only near it)
static bool cand_sides(Candidate* c) {
    float base = C.texel * C.coarse;
    float tx = base * fmaxf(1.0f, c->dist / C.falloff);
    if (c->plane[0] > 0.0f) { // (by its area, a third more for the gaps, like an imported mesh: twice the texels)
        c->wi = (uint8_t)side_index(c->plane[0] / tx);
        c->hi = (uint8_t)side_index(c->plane[1] / tx);
        return true;
    }
    chart_sides(c->shape, c->scale, tx, &c->wi, &c->hi);
    if (!c->tris) return true;
    uint8_t bw, bh;
    chart_sides(c->shape, c->scale, base, &bw, &bh);
    if ((float)(8u << bw) * (float)(8u << bh) < 6.0f * (float)c->tris) return false;
    while ((float)(8u << c->wi) * (float)(8u << c->hi) < 6.0f * (float)c->tris) {
        if (c->wi < bw && (c->wi <= c->hi || c->hi >= bh)) c->wi++;
        else c->hi++;
    }
    return true;
}

// smooth: a Landscape's chunk (thousands of triangles, but a gently curved ground: no mosaic at a texel per triangle).
// The dense rule took its chunks off the cache away from the camera; on the probes, buried in its hills, they went black
static bool candidate(const CTransform* t, const CStaticMesh* m, const float cam[3], bool smooth, Candidate* c) {
    if (t->flags & TRANSFORM_HIDDEN) return false;
    uint32_t slot = render_mesh_slot(m->mesh);
    if (!render_mesh_has_atlas(slot)) return false; // (a welded heavy mesh, no UV2: the probes light it, mesh_build.c)
    // the bounding sphere's nearest point: the mesh's own box (a Landscape's chunk is 50 m wide, its middle far from
    // the actor's origin; the unit shapes' radius, 87 cm, put the chunk under the camera tens of meters away)
    float mn[3], mx[3], ctr[3], r2 = 0.0f, d[3];
    render_mesh_bounds(m->mesh, mn, mx);
    for (int k = 0; k < 3; k++) {
        float h = (mx[k] - mn[k]) * 0.5f * t->scale[k];
        ctr[k] = (mn[k] + mx[k]) * 0.5f * t->scale[k];
        r2 += h * h;
        d[k] = t->location[k] - cam[k];
    }
    if (ctr[0] * ctr[0] + ctr[1] * ctr[1] + ctr[2] * ctr[2] > 1.0f) { // (the shapes are centered: no rotation needed)
        float ax[3][3];
        rotator_axes(t->rotation, ax);
        for (int k = 0; k < 3; k++) d[k] += ctr[0] * ax[0][k] + ctr[1] * ax[1][k] + ctr[2] * ax[2][k];
    }
    c->dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - sqrtf(r2);
    if (c->dist > CACHE_RADIUS) return false;
    c->shape = (uint8_t)slot;
    memcpy(c->scale, t->scale, sizeof c->scale);
    c->tris = 0;
    c->plane[0] = smooth ? (mx[0] - mn[0]) * fabsf(t->scale[0]) : 0.0f;
    c->plane[1] = smooth ? (mx[1] - mn[1]) * fabsf(t->scale[1]) : 0.0f;
    if (slot >= SHAPE_COUNT && !smooth) {
        const GpuVertex* v;
        const uint32_t* idx;
        uint32_t first, count;
        int32_t off;
        render_mesh_cpu(m->mesh, &v, &idx, &first, &count, &off);
        c->tris = count / 3;
    }
    return cand_sides(c);
}

// Whether a mesh entity is a Landscape's chunk (its plants are not: they are dense small meshes like any other)
static bool landscape_chunk(const CAttach* at) { return at && at->kind == ATTACH_LANDSCAPE && at->node < LANDSCAPE_PLANT_NODE; }

static int by_dist(const void* a, const void* b) {
    float x = ((const Candidate*)a)->dist, y = ((const Candidate*)b)->dist;
    return x < y ? -1 : x > y;
}
static int by_height(const void* a, const void* b) { // the tallest first (the shelves), then the widest
    const Chart *x = a, *y = b;
    if (x->h != y->h) return x->h > y->h ? -1 : 1;
    return x->w > y->w ? -1 : x->w < y->w;
}

// Shelf packing of power-of-two rectangles sorted by height
static bool pack(Chart* c, int n) {
    uint32_t x = 0, y = 0, row = 0;
    for (int i = 0; i < n; i++) {
        if (x + c[i].w > CACHE_ATLAS) {
            y += row;
            x = 0;
            row = 0;
        }
        if (y + c[i].h > CACHE_ATLAS) return false;
        c[i].x = x;
        c[i].y = y;
        if (!row) row = c[i].h;
        x += c[i].w;
    }
    return true;
}

static uint32_t log2_8(uint32_t side) { // 8 << i == side
    uint32_t i = 0;
    while ((8u << i) < side) i++;
    return i;
}

// The charts of the nearest `keep` candidates that may have one (ok), at the sides cand_sides last gave them; their texels.
// A chart one step from its old sides keeps them (and its light); once the density was tightened, only when they aren't
// larger (or the old sides would hold the texels the tightening frees)
static uint64_t build(Chart* next, int* nc, uint32_t n, uint32_t keep, bool grow) {
    uint64_t used = 0;
    int k = 0;
    for (uint32_t i = 0; i < n && (uint32_t)k < keep; i++) {
        const Candidate* c = &C.cand[i];
        if (!c->ok) continue;
        Chart* ch = &next[k++];
        *ch = (Chart){ c->slot, 8u << c->wi, 8u << c->hi, 0, 0, c->shape, c->dyn };
        uint16_t old = C.chart_of[ch->slot];
        const Chart* o = old ? &C.charts[old - 1] : NULL;
        if (o && o->shape == ch->shape && o->w * 2u >= ch->w && o->w <= ch->w * (grow ? 2u : 1u) && o->h * 2u >= ch->h &&
            o->h <= ch->h * (grow ? 2u : 1u))
            ch->w = o->w, ch->h = o->h;
        used += (uint64_t)ch->w * ch->h;
    }
    *nc = k;
    return used;
}

static void allocate(World* w, const float cam[3]) {
    C.coarse = 1.0f;
    C.falloff = CACHE_FALLOFF;
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const Entity* ents = ecs_arch_entities(a);
        bool rotating = (a->mask & COMP_BIT(COMP_ROTATING)) != 0;
        const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            Candidate c;
            if (!candidate(&tr[i], &sm[i], cam, landscape_chunk(at ? &at[i] : NULL), &c)) continue;
            c.dyn = rotating || (tr[i].flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE;
            c.slot = entity_index(ents[i]);
            c.ok = 1;
            C.cand[n++] = c;
        }
    }
    qsort(C.cand, n, sizeof *C.cand, by_dist);
    // Every mesh within reach gets a chart: while they don't fit, the texels grow coarser sooner with the distance (down to
    // 4.5 m), then coarser everywhere. Dropping the farthest left them on the probes: 2 m, leaky, flickering on the walls of
    // the houses 16 m away (the RPG demo on Epic had 85 charts of 209; the horror one 6 of 67)
    uint64_t budget = (uint64_t)(CACHE_FILL * CACHE_ATLAS * CACHE_ATLAS);
    static Chart next[CACHE_MAX_CHARTS];
    int nc = 0;
    bool packed = false;
    for (int iter = 0; iter < 24 && !packed; iter++) {
        if (iter)
            for (uint32_t i = 0; i < n; i++) C.cand[i].ok = cand_sides(&C.cand[i]);
        uint64_t used = build(next, &nc, n, CACHE_MAX_CHARTS, iter == 0);
        if (used <= budget) { // (past CACHE_MAX_CHARTS the farthest have none)
            qsort(next, (size_t)nc, sizeof *next, by_height);
            if ((packed = pack(next, nc))) break;
        }
        // past 6 times the preset's texel, or with more charts than fit at their smallest (8 x 8): the farthest out.
        // (Not "while it helps": the sides are powers of two, a step coarser often changes none of them)
        if (C.coarse > 6.0f || (uint64_t)nc * 64u > budget) break;
        if (C.falloff > 450.0f) C.falloff *= 0.7f;
        else C.coarse *= 1.25f;
    }
    uint32_t want = 0;
    for (uint32_t i = 0; i < n; i++) want += C.cand[i].ok;
    C.stats.candidates = want;
    if (!packed) { // the nearest ones that fit (one too large skipped, not the end of it), the farthest tenth out until they pack
        uint64_t used = 0;
        uint32_t keep = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!C.cand[i].ok) continue;
            uint64_t s = (uint64_t)(8u << C.cand[i].wi) * (8u << C.cand[i].hi);
            if (used + s > budget || keep >= CACHE_MAX_CHARTS) {
                C.cand[i].ok = 0;
                continue;
            }
            used += s;
            keep++;
        }
        for (;;) {
            build(next, &nc, n, keep, false);
            qsort(next, (size_t)nc, sizeof *next, by_height);
            if (pack(next, nc) || keep == 0) break;
            keep = keep * 9 / 10;
        }
    }
    // the old charts: what is kept (same slot, shape and sides) has its light copied over
    memcpy(C.prev, C.charts, sizeof(Chart) * (size_t)C.ncharts);
    int nprev = C.ncharts;
    C.ncopies = 0;
    C.nblits = 0;
    CacheTexelGpu* out = C.staging.mapped;
    CacheEntryGpu* ent = C.entries.mapped;
    uint32_t count = 0;
    // the Movable actors' texels first (redone every frame: gi_cache_record), then the rest
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nc; i++) {
            Chart* ch = &next[i];
            if ((ch->dyn != 0) != (pass == 0)) continue;
            const Template* T = template_for(ch->shape, log2_8(ch->w), log2_8(ch->h));
            ch->tfirst = count;
            ch->tcount = T->count;
            for (uint32_t k = 0; k < T->count; k++) {
                CacheTexelGpu t = T->t[k];
                t.slot = ch->slot;
                t.xy = ((t.xy & 0xFFFFu) + ch->x) | (((t.xy >> 16) + ch->y) << 16);
                out[count++] = t;
            }
        }
        if (pass == 0) C.dyn_count = count;
    }
    for (int i = 0; i < nc; i++) {
        Chart* ch = &next[i];
        float inv = 1.0f / (float)CACHE_ATLAS;
        ent[i] = (CacheEntryGpu){ ch->slot, { 0, 0, 0 },
                                  { (float)(ch->w - 2 * CACHE_PAD) * inv, (float)(ch->h - 2 * CACHE_PAD) * inv,
                                    (float)(ch->x + CACHE_PAD) * inv, (float)(ch->y + CACHE_PAD) * inv } };
        uint16_t old = C.chart_of[ch->slot];
        const Chart* o = old ? &C.prev[old - 1] : NULL;
        if (o && o->w == ch->w && o->h == ch->h && o->shape == ch->shape) {
            VkImageCopy r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)o->x, (int32_t)o->y, 0 },
                              { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)ch->x, (int32_t)ch->y, 0 }, { ch->w, ch->h, 1 } };
            C.copies[C.ncopies++] = r;
        } else if (o && o->shape == ch->shape) {
            // resized (a room you walk toward gets finer texels): its light resampled. Started from nothing it went dark
            // and noisy while it converged again. Direct and indirect only: the history (meta) and the walls start over
            // (gi_cache.comp counts the resampled light as a few updates and measures the walls)
            VkImageBlit b = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              { { (int32_t)o->x, (int32_t)o->y, 0 }, { (int32_t)(o->x + o->w), (int32_t)(o->y + o->h), 1 } },
                              { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              { { (int32_t)ch->x, (int32_t)ch->y, 0 }, { (int32_t)(ch->x + ch->w), (int32_t)(ch->y + ch->h), 1 } } };
            C.blits[C.nblits++] = b;
        }
    }
    for (int i = 0; i < nprev; i++) C.chart_of[C.prev[i].slot] = 0;
    memcpy(C.charts, next, sizeof(Chart) * (size_t)nc);
    C.ncharts = nc;
    for (int i = 0; i < nc; i++) C.chart_of[next[i].slot] = (uint16_t)(i + 1);
    C.count = count;
    C.cursor = 0;
    C.nentries = (uint32_t)nc;
    C.upload = true;
    memcpy(C.center, cam, sizeof C.center);
    C.has_alloc = true;
    C.pending = false;
    C.since = 0;
    C.stats.charts = (uint32_t)nc;
    C.stats.texels = count;
    C.stats.falloff_cm = C.falloff;
    C.stats.coarse = C.coarse;
    C.stats.reallocations++;
}

// Whether an entity the change log names changes the allocation (a mesh that should have a chart and hasn't, or the
// other way round, or one whose chart would change its sides or shape). The log holds slots, not handles: the slot's
// entity now (a slot used again has a newer generation; read as a handle of generation 0 it looked destroyed while it kept
// its chart, and every move of it reallocated the cache: 10 times in 5 s in a game whose actors had been respawned, the
// indirect light flickering with each new start)
static bool needs_change(World* w, uint32_t slot, const float cam[3]) {
    if (slot >= RE_ECS_MAX_ENTITIES) return false;
    bool have = C.chart_of[slot] != 0;
    Entity e = ecs_entity_at(w, slot);
    if (e == ENTITY_NONE) return have;
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    Candidate c;
    bool want = t && m && candidate(t, m, cam, landscape_chunk(ecs_get(w, e, COMP_ATTACH)), &c);
    if (want != have) return true;
    if (!have) return false;
    const Chart* ch = &C.charts[C.chart_of[slot] - 1];
    // the sides follow the distance: only a change of two steps reallocates (one step either way stays)
    uint32_t wi = log2_8(ch->w), hi = log2_8(ch->h);
    return ch->shape != c.shape || wi > c.wi + 1u || c.wi > wi + 1u || hi > c.hi + 1u || c.hi > hi + 1u;
}

void gi_cache_update(VkCtx* vk, World* w, const float camera[3], const GiSettings* s) {
    (void)vk;
    C.active = C.rt && s->surface_cache;
    if (!C.active) return;
    C.rays = s->cache_rays > 0 ? (uint32_t)s->cache_rays : 8u;
    C.per_frame = s->cache_per_frame > 0 ? (uint32_t)s->cache_per_frame : 32768u;
    float texel = s->cache_texel > 5.0f ? s->cache_texel : 25.0f;
    C.since++;
    // The camera's speed (cm/s, smoothed): a character running into a room has its charts made before it arrives (the
    // allocation centered 0.6 s ahead of it), and while it moves fast more texels are updated each frame
    double now = re_time_seconds();
    float dt = C.last_t > 0.0 ? (float)(now - C.last_t) : 0.0f;
    C.last_t = now;
    float ahead[3];
    for (int k = 0; k < 3; k++) {
        if (dt > 1e-4f && dt < 0.25f) C.vel[k] = C.vel[k] * 0.8f + (camera[k] - C.last_cam[k]) / dt * 0.2f;
        C.last_cam[k] = camera[k];
        ahead[k] = camera[k] + C.vel[k] * 0.6f;
    }
    C.speed = sqrtf(C.vel[0] * C.vel[0] + C.vel[1] * C.vel[1] + C.vel[2] * C.vel[2]);
    camera = ahead;
    bool need = !C.has_alloc || w->revision != C.rev || texel != C.texel;
    float d[3] = { camera[0] - C.center[0], camera[1] - C.center[1], camera[2] - C.center[2] };
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > CACHE_MOVE * CACHE_MOVE) need = true;
    C.texel = texel;
    if (!need && w->changes != C.changes) {
        uint32_t k = w->changes - C.changes;
        if (k > RE_ECS_CHANGE_RING) need = true;
        for (uint32_t i = 0; i < k && !need; i++) need = needs_change(w, w->changed[(C.changes + i) % RE_ECS_CHANGE_RING], camera);
    }
    C.rev = w->revision;
    C.changes = w->changes;
    if (need) C.pending = true;
    // at most every 8 frames (an editor drag would reallocate every frame; the uploads also wait for the frames in flight)
    if (C.pending && (C.since >= 8 || !C.has_alloc)) allocate(w, camera);
    C.stats.per_frame = C.count < C.per_frame ? C.count : C.per_frame;
    C.stats.texel_cm = C.texel;
}

void gi_cache_fill_frame(GpuFrame* fr, uint32_t frame_index) {
    C.frame_slot = frame_index % RE_FRAMES_IN_FLIGHT;
    fr->cache_st = C.table.addr;
    fr->cache_texels = C.texels.addr;
    fr->cache_count = C.count;
    fr->cache_rays = C.rays;
    // the first frames after an allocation update every texel (the light is there at once); then a share each frame
    // (moving fast, above 6 m/s: twice the share: the light of what comes into view settles twice as fast)
    uint32_t share = C.speed > 600.0f ? C.per_frame * 2u : C.per_frame;
    if (C.since < 60) share *= 3u; // (the first second of an allocation: three times the share, the bounces settle at once)
    fr->cache_n = C.count < share || C.since < 4 ? C.count : share;
    fr->cache_first = C.count ? C.cursor % C.count : 0;
    if (C.count) C.cursor = (C.cursor + fr->cache_n) % C.count;
    fr->cache_params[0] = (float)CACHE_ATLAS;
    fr->cache_params[1] = 512.0f; // the running mean's length (gi_cache.comp): a fraction of a percent an update, no flicker
    fr->cache_params[2] = fr->cache_params[3] = 0.0f;
}

void gi_cache_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr) {
    if (!gi_cache_active() || !fr->tlas) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkPipelineStageFlags2 XF = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    { VkDescriptorSet sets_[2] = { set, mat_set() }; // (Q4: the Light Functions' materials, set 1)
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 2, sets_, 0, NULL); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    if (C.upload) { // a new allocation: its texels, the slot table, the kept charts' light moved to their new place
        C.upload = false;
        gpu_barrier(cmd, CS | FS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    XF, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (C.count) {
            VkBufferCopy bc = { 0, 0, (VkDeviceSize)C.count * sizeof(CacheTexelGpu) };
            vkCmdCopyBuffer(cmd, C.staging.buffer, C.texels.buffer, 1, &bc);
        }
        vkCmdFillBuffer(cmd, C.table.buffer, 0, VK_WHOLE_SIZE, 0);
        for (int i = 0; i < CACHE_IMAGES; i++) {
            if (C.ncopies || C.nblits) {
                clear_image(cmd, &C.tmp[i]);
                gpu_barrier(cmd, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                if (C.ncopies)
                    vkCmdCopyImage(cmd, C.img[i].image, VK_IMAGE_LAYOUT_GENERAL, C.tmp[i].image, VK_IMAGE_LAYOUT_GENERAL, C.ncopies, C.copies);
                if (C.nblits && i < 2) // direct and indirect (premultiplied by validity: a linear resample stays right)
                    vkCmdBlitImage(cmd, C.img[i].image, VK_IMAGE_LAYOUT_GENERAL, C.tmp[i].image, VK_IMAGE_LAYOUT_GENERAL, C.nblits,
                                   C.blits, VK_FILTER_LINEAR);
            }
        }
        gpu_barrier(cmd, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int i = 0; i < CACHE_IMAGES; i++) {
            if (C.ncopies || C.nblits) { // the whole new layout from tmp: kept charts in place, the rest zero (no history)
                VkImageCopy all = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                    { CACHE_ATLAS, CACHE_ATLAS, 1 } };
                vkCmdCopyImage(cmd, C.tmp[i].image, VK_IMAGE_LAYOUT_GENERAL, C.img[i].image, VK_IMAGE_LAYOUT_GENERAL, 1, &all);
            } else {
                clear_image(cmd, &C.img[i]);
            }
        }
        gpu_barrier(cmd, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        Push p = *push;
        p.pass = 0;
        p.instances = C.entries.addr;
        p.pad = C.nentries;
        vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
        if (C.nentries) vkCmdDispatch(cmd, (C.nentries + 63) / 64, 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    } else {
        // last frame's main pass sampled what this writes
        gpu_barrier(cmd, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    Push p = *push;
    // the texels of the Movable actors moving right now, every frame: their light changes as they move (a short history);
    // at most 96K texels a frame, the rest next frames from where this one stopped (a crowd's parts take turns). Their
    // indices listed, one dispatch for all (2026-11-19, the Q8 leftover: one per run of neighboring charts was thousands
    // of dispatches with 2500 bodies tumbling, and past 128 runs the filter went over the whole atlas: 1.7 ms of GI)
    // (the rays by software, A1: 8K a frame, the moving parts take turns over more frames: in the RPG demo their 23K texels
    // a frame were 2.5 of the GI's 3.4 ms on a 3060, a phone's GPU ten times that)
    uint32_t moving = 0, budget = gi_sw() ? 8192u : CACHE_MOVING_MAX;
    GpuBuf* ml = &C.moving[C.frame_slot];
    uint32_t* list = ml->mapped;
    int start = C.ncharts ? (int)(C.moving_cursor % (uint32_t)C.ncharts) : 0, stop = C.ncharts;
    for (int n = 0; n < C.ncharts && list; n++) {
        int i = (start + n) % C.ncharts;
        const Chart* ch = &C.charts[i];
        if (!ch->dyn || !ch->tcount || !render_slot_moving(ch->slot)) continue;
        if (moving + ch->tcount > budget) { // the budget ran out here: the next frame goes on from it
            stop = i;
            break;
        }
        for (uint32_t t = 0; t < ch->tcount; t++) list[moving + t] = ch->tfirst + t;
        moving += ch->tcount;
    }
    C.moving_cursor = stop < C.ncharts ? (uint32_t)stop : 0u;
    C.stats.dynamic = moving;
    if (moving) {
        p.pass = 2, p.pad = moving, p.instances = ml->addr;
        vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (moving + 63) / 64, 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    p = *push;
    if (fr->cache_n) {
        p.pass = 1;
        vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (fr->cache_n + 63) / 64, 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    // the filter (gi_cache.comp pass 3): the texels just updated, from their raw history into what the pixels and the
    // bounces read (REALENGINE_GI_FILTER=0: copied as they are, the comparison)
    static int s_filter = -1;
    if (s_filter < 0) { const char* e = getenv("REALENGINE_GI_FILTER"); s_filter = !(e && e[0] == '0'); }
    uint32_t off = s_filter ? 0u : 0x80000000u;
    p.pass = 3;
    if (fr->cache_n >= C.count) { // (all of them, from the first)
        p.instances = 0, p.pad = C.count | off;
        vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
        if (C.count) vkCmdDispatch(cmd, (C.count + 63) / 64, 1, 1);
    } else {
        if (fr->cache_n) {
            p.instances = fr->cache_first, p.pad = fr->cache_n | off;
            vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (fr->cache_n + 63) / 64, 1, 1);
        }
        if (moving) { // (the moving ones: the same list)
            p.instances = ml->addr, p.pad = moving | 0x40000000u | off;
            vkCmdPushConstants(cmd, C.layout, stages, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (moving + 63) / 64, 1, 1);
        }
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

uint32_t gi_cache_cycle_frames(void) {
    return C.per_frame && C.count > C.per_frame ? (C.count + C.per_frame - 1) / C.per_frame : 1u;
}
