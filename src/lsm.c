// The local lights' shadow maps (lsm.h): the atlas, the pick by importance, the tiles (a buddy quadtree), the cache (what
// was drawn is kept: the table holds each light as drawn, so a light waiting for its redraw stays right, only older),
// the frame's table for the shaders (shaders/lsm.glsl).
#include "lsm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lights.h"
#include "platform.h"
#include "vkctx.h"

#define MIN_RES 64
#define MAX_DEPTH 7 // the quadtree's levels: 4096 down to 64
#define MAX_NODES 5461 // (4^7 - 1) / 3
#define MAX_BOXES 512 // dirty boxes kept a frame (more: one overflow box)
#define NEAR_CM 2.0f
#define WIDE_SPOT 0.5f // a spot wider than 60 degrees (cos_outer under this) draws the six faces
enum { NODE_FREE = 0, NODE_SPLIT = 1, NODE_USED = 2 };

// The GPU's table (shaders/lsm.glsl LsmBuf, scalar layout)
typedef struct GpuLsmLight {
    float pos_near[4]; // the light as drawn, the near plane (cm)
    float params[4];   // far (cm), the face's side (texels), 1 / the atlas's side, k = far / (far - near)
    uint32_t t0[4];    // faces 0-3: x | y << 16 (texels)
    uint32_t t1[4];    // faces 4-5, faces (1 or 6), the projection's scale s as float bits
    float m[16];       // a spot's one face: world -> clip (column-major)
} GpuLsmLight;
typedef struct GpuLsmJob {
    float m[16];       // world -> the face's clip
    float planes[4][4]; // the face's sides (inside: dot >= 0)
    float sphere[4];   // the light, its range
} GpuLsmJob;
typedef struct GpuLsmTable {
    uint32_t hash[LSM_HASH][2]; // slot + 1 (0: empty), its light's index
    GpuLsmLight l[LSM_MAX_LIGHTS];
    GpuLsmJob j[LSM_MAX_JOBS];
    float cull[LSM_MAX_CULL][4]; // the lights drawn: center, range (negative: only their dynamic casters)
} GpuLsmTable;

typedef struct Ent {
    bool used, valid; // valid: every face drawn once (in the table)
    uint32_t slot;
    GpuLight drawn;   // as drawn (its geometry)
    int faces, res;
    int node[6];
    uint16_t tx[6], ty[6];
    uint8_t full, dyn; // faces waiting: drawn whole / their dynamic casters
    bool moved;       // its geometry changed: every face at once (one frame of drawing for the whole light)
    float imp;
    float s, k, range; // as drawn: the projection's scale, the depth's
    float m[6][16], planes[6][4][4];
    uint32_t seen;
} Ent;

static const float FACE[6][3][3] = { // W (the face's axis), U, V: shaders/lsm.glsl lsm_face matches
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },  { { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } },
    { { 0, 1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { 1, 0, 0 }, { 0, -1, 0 } },
};

static struct {
    VkCtx* vk;
    LsmSettings s;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, layer[2];
    GpuBuf tab[RE_FRAMES_IN_FLIGHT], dev; // the table written here per frame slot, copied to the GPU's (lsm_upload)
    uint8_t node[MAX_NODES];
    int depth; // the quadtree's levels for this atlas
    Ent e[LSM_MAX_LIGHTS];
    float dyn[MAX_BOXES][6], stat[MAX_BOXES][6], dyn_over[6], stat_over[6];
    int ndyn, nstat;
    bool all;
    uint32_t frame;
    LsmStats stats;
} M;

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void empty_box(float b[6]) { b[0] = b[1] = b[2] = 1e30f, b[3] = b[4] = b[5] = -1e30f; }

bool lsm_init(VkCtx* vk) {
    memset(&M, 0, sizeof M);
    M.vk = vk;
    empty_box(M.dyn_over), empty_box(M.stat_over);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        if (!gpu_buf_create(vk, &M.tab[f], sizeof(GpuLsmTable), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
        memset(M.tab[f].mapped, 0, sizeof(GpuLsmTable));
    }
    return gpu_buf_create(vk, &M.dev, sizeof(GpuLsmTable), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
}

static void destroy_image(void) {
    VkDevice d = M.vk->device;
    for (int i = 0; i < 2; i++)
        if (M.layer[i]) vkDestroyImageView(d, M.layer[i], NULL), M.layer[i] = VK_NULL_HANDLE;
    if (M.view) vkDestroyImageView(d, M.view, NULL), M.view = VK_NULL_HANDLE;
    if (M.image) vkDestroyImage(d, M.image, NULL), M.image = VK_NULL_HANDLE;
    if (M.memory) vkFreeMemory(d, M.memory, NULL), M.memory = VK_NULL_HANDLE;
}

void lsm_shutdown(VkCtx* vk) {
    if (!M.vk) return;
    destroy_image();
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &M.tab[f]);
    gpu_buf_destroy(vk, &M.dev);
}

static void reset_tiles(void) {
    memset(M.node, NODE_FREE, sizeof M.node);
    for (int i = 0; i < LSM_MAX_LIGHTS; i++) M.e[i].used = false;
}

bool lsm_configure(VkCtx* vk, const LsmSettings* s, bool* views_changed) {
    *views_changed = false;
    LsmSettings n = *s;
    if (n.lights < 0) n.lights = 0;
    if (n.lights > LSM_MAX_LIGHTS) n.lights = LSM_MAX_LIGHTS;
    n.atlas = n.atlas <= 2048 ? 2048 : 4096;
    if (n.max_res > n.atlas / 2) n.max_res = n.atlas / 2;
    if (n.max_res < MIN_RES) n.max_res = MIN_RES;
    if (n.redraw < 6) n.redraw = 6;
    int side = n.lights ? n.atlas : 1; // (off: a 1x1 stand-in, the descriptor stays valid)
    bool remake = !M.image || side != (M.s.lights ? M.s.atlas : 1);
    M.s = n;
    if (!remake) return true;
    if (M.image) vkDeviceWaitIdle(vk->device);
    destroy_image();
    reset_tiles();
    M.depth = 0;
    for (int r = n.atlas; r >= MIN_RES && M.depth < MAX_DEPTH; r /= 2) M.depth++;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = LSM_FORMAT;
    ii.extent = (VkExtent3D){ (uint32_t)side, (uint32_t)side, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 2; // the maps, then their static casters alone
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &M.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, M.image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &M.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(vk->device, M.image, M.memory, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = M.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = LSM_FORMAT;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(vk->device, &vi, NULL, &M.view) != VK_SUCCESS) return false;
    for (int l = 0; l < 2; l++) {
        vi.subresourceRange.baseArrayLayer = (uint32_t)l;
        if (vkCreateImageView(vk->device, &vi, NULL, &M.layer[l]) != VK_SUCCESS) return false;
    }
    // into GENERAL at depth 1 once (it stays GENERAL: drawn into, copied and sampled, barriers between)
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = M.image;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    VkClearDepthStencilValue one = { 1.0f, 0 };
    vkCmdClearDepthStencilImage(cmd, M.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cmd, &di);
    gpu_end_once(vk, cmd);
    *views_changed = true;
    return true;
}

bool lsm_enabled(void) { return M.s.lights > 0 && M.image; }
int lsm_atlas_side(void) { return M.s.lights ? M.s.atlas : 1; }
VkImageView lsm_view(void) { return M.view; }
VkImageView lsm_layer_view(int layer) { return layer >= 0 && layer < 2 ? M.layer[layer] : VK_NULL_HANDLE; }
VkImage lsm_image(void) { return M.image; }
const LsmStats* lsm_stats(void) { return &M.stats; }
void lsm_upload(VkCommandBuffer cmd, uint32_t frame_slot) {
    VkBufferCopy c = { 0, 0, sizeof(GpuLsmTable) };
    if (M.dev.buffer) vkCmdCopyBuffer(cmd, M.tab[frame_slot].buffer, M.dev.buffer, 1, &c);
}
void lsm_stats_reset(void) {
    M.stats.full_total = M.stats.dyn_total = M.stats.frames = M.stats.gpu_frames = 0;
    M.stats.gpu_sum = M.stats.gpu_max = M.stats.cpu_sum = 0.0;
}
void lsm_note_gpu(double ms) { M.stats.gpu_sum += ms, M.stats.gpu_max = ms > M.stats.gpu_max ? ms : M.stats.gpu_max, M.stats.gpu_frames++; }
void lsm_invalidate(void) { M.all = true; }

static void add_box(float list[][6], int* n, float over[6], const float mn[3], const float mx[3]) {
    if (!(mn[0] <= mx[0])) return;
    if (!lsm_enabled()) return;
    if (*n < MAX_BOXES) {
        memcpy(list[*n], mn, 3 * sizeof(float));
        memcpy(list[*n] + 3, mx, 3 * sizeof(float));
        (*n)++;
        return;
    }
    for (int k = 0; k < 3; k++) over[k] = fminf(over[k], mn[k]), over[k + 3] = fmaxf(over[k + 3], mx[k]);
}
void lsm_dirty_box(const float mn[3], const float mx[3]) { add_box(M.dyn, &M.ndyn, M.dyn_over, mn, mx); }
void lsm_dirty_static(const float mn[3], const float mx[3]) { add_box(M.stat, &M.nstat, M.stat_over, mn, mx); }

// --- The tiles: a buddy quadtree over the atlas (node 0 the atlas, children 4i+1..4i+4) ---

// A free node of level `want` (its side atlas >> want) under node i (level l, corner x, y): the split ones searched first
// (a whole free block split only on the second pass: fewer big blocks broken)
static int quad_find(int i, int l, int x, int y, int want, bool split_free, uint16_t* ox, uint16_t* oy) {
    if (M.node[i] == NODE_USED) return -1;
    if (l == want) {
        if (M.node[i] != NODE_FREE) return -1;
        M.node[i] = NODE_USED;
        *ox = (uint16_t)x, *oy = (uint16_t)y;
        return i;
    }
    if (M.node[i] == NODE_FREE && !split_free) return -1;
    if (l + 1 >= M.depth) return -1;
    uint8_t was = M.node[i];
    M.node[i] = NODE_SPLIT;
    int h = (M.s.atlas >> l) / 2;
    for (int c = 0; c < 4; c++) {
        int r = quad_find(4 * i + 1 + c, l + 1, x + (c & 1) * h, y + (c >> 1) * h, want, split_free, ox, oy);
        if (r >= 0) return r;
    }
    M.node[i] = was; // (nothing under it: as it was)
    return -1;
}
static int quad_alloc(int res, uint16_t* x, uint16_t* y) {
    int want = 0;
    while ((M.s.atlas >> want) > res && want + 1 < M.depth) want++;
    int r = quad_find(0, 0, 0, 0, want, false, x, y);
    return r >= 0 ? r : quad_find(0, 0, 0, 0, want, true, x, y);
}
static void quad_free(int i) {
    if (i < 0 || i >= MAX_NODES) return;
    M.node[i] = NODE_FREE;
    while (i > 0) { // (four free siblings: the parent whole again)
        int p = (i - 1) / 4;
        for (int c = 0; c < 4; c++)
            if (M.node[4 * p + 1 + c] != NODE_FREE) return;
        M.node[p] = NODE_FREE;
        i = p;
    }
}

static void ent_free(Ent* e) {
    if (e->used)
        for (int f = 0; f < e->faces; f++) quad_free(e->node[f]);
    e->used = e->valid = false;
}
static bool ent_alloc(Ent* e) {
    for (int f = 0; f < e->faces; f++) {
        e->node[f] = quad_alloc(e->res, &e->tx[f], &e->ty[f]);
        if (e->node[f] < 0) {
            for (int g = 0; g < f; g++) quad_free(e->node[g]);
            return false;
        }
    }
    e->full = (uint8_t)((1u << e->faces) - 1u), e->dyn = 0, e->valid = false, e->moved = true;
    return true;
}

// A light's faces: a spot under 60 degrees one, the others the cube's six
static int light_faces(const GpuLight* g) { return (g->type & 3u) == 2u && g->cos_outer >= WIDE_SPOT ? 1 : 6; }
static bool same_geometry(const GpuLight* a, const GpuLight* b) {
    return !memcmp(a->position, b->position, sizeof a->position) && a->range == b->range && (a->type & 3u) == (b->type & 3u) &&
           ((a->type & 3u) != 2u || (!memcmp(a->direction, b->direction, sizeof a->direction) && !memcmp(a->right, b->right, sizeof a->right) &&
                                     a->cos_outer == b->cos_outer));
}

// The faces' matrices and planes from the light as drawn (W, U, V per face; a spot: its direction, right and up)
static void ent_frame(Ent* e) {
    const GpuLight* g = &e->drawn;
    float nr = NEAR_CM, fr = fmaxf(g->range, nr * 4.0f);
    e->range = fr;
    e->k = fr / (fr - nr);
    float inner = (float)(e->res - 2 * LSM_BORDER) / (float)e->res; // (the border: what the filter reads past the face)
    float tanh_ = 1.0f;
    float axes[6][3][3];
    if (e->faces == 1) {
        float c = fminf(fmaxf(g->cos_outer, 0.05f), 0.9999f);
        tanh_ = sqrtf(1.0f - c * c) / c * 1.02f; // (the cone, a little wider: its edge's smooth falloff)
        const float* w = g->direction;
        const float* u = g->right;
        float v[3] = { w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0] }; // cross(dir, right)
        memcpy(axes[0][0], w, 12), memcpy(axes[0][1], u, 12), memcpy(axes[0][2], v, 12);
    } else
        memcpy(axes, FACE, sizeof axes);
    e->s = inner / tanh_;
    const float* c = g->position;
    for (int f = 0; f < e->faces; f++) {
        const float *W = axes[f][0], *U = axes[f][1], *V = axes[f][2];
        float rows[4][4];
        for (int k = 0; k < 3; k++) rows[0][k] = e->s * U[k], rows[1][k] = e->s * V[k], rows[2][k] = e->k * W[k], rows[3][k] = W[k];
        rows[0][3] = -e->s * dot3(U, c);
        rows[1][3] = -e->s * dot3(V, c);
        rows[2][3] = -e->k * (dot3(W, c) + nr);
        rows[3][3] = -dot3(W, c);
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) e->m[f][k * 4 + r] = rows[r][k]; // (column-major)
        // the sides: |s u| <= w, |s v| <= w -> dot(W -+ s U, p - c) >= 0
        const float* A[4] = { U, U, V, V };
        const float sg[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
        for (int p = 0; p < 4; p++) {
            float nn[3], l2 = 0.0f;
            for (int k = 0; k < 3; k++) nn[k] = W[k] + sg[p] * e->s * A[p][k], l2 += nn[k] * nn[k];
            float il = 1.0f / sqrtf(l2);
            for (int k = 0; k < 3; k++) e->planes[f][p][k] = nn[k] * il;
            e->planes[f][p][3] = -dot3(e->planes[f][p], c);
        }
    }
}

// A world box against a face of e (its sphere first): may a caster in it change the face?
static bool box_hits_face(const Ent* e, int f, const float b[6]) {
    float h[3], ctr[3], d2 = 0.0f;
    const float* c = e->drawn.position;
    for (int k = 0; k < 3; k++) {
        ctr[k] = 0.5f * (b[k] + b[k + 3]), h[k] = 0.5f * (b[k + 3] - b[k]);
        float q = fmaxf(fmaxf(b[k] - c[k], 0.0f), c[k] - b[k + 3]);
        d2 += q * q;
    }
    if (d2 > e->range * e->range) return false;
    for (int p = 0; p < 4; p++) {
        const float* pl = e->planes[f][p];
        if (dot3(pl, ctr) + pl[3] + fabsf(pl[0]) * h[0] + fabsf(pl[1]) * h[1] + fabsf(pl[2]) * h[2] < 0.0f) return false;
    }
    return true;
}
static bool box_hits_sphere(const Ent* e, const float b[6]) {
    float d2 = 0.0f;
    const float* c = e->drawn.position;
    for (int k = 0; k < 3; k++) {
        float q = fmaxf(fmaxf(b[k] - c[k], 0.0f), c[k] - b[k + 3]);
        d2 += q * q;
    }
    return d2 <= e->range * e->range;
}
static void mark_boxes(Ent* e, float list[][6], int n, const float over[6], bool full) {
    for (int i = -1; i < n; i++) {
        const float* b = i < 0 ? over : list[i];
        if (!(b[0] <= b[3]) || !box_hits_sphere(e, b)) continue;
        for (int f = 0; f < e->faces; f++)
            if (box_hits_face(e, f, b)) {
                if (full) e->full |= (uint8_t)(1u << f);
                else e->dyn |= (uint8_t)(1u << f);
            }
    }
}

static uint32_t pow2_at_least(float v) {
    uint32_t r = MIN_RES;
    while ((float)r < v && r < 8192u) r *= 2u;
    return r;
}

typedef struct Cand {
    uint32_t slot;
    const GpuLight* g;
    float imp, px;
    int held;  // its entry (-1: none)
    bool seen; // in the view (one held out of it keeps its tiles while there's room, drawn again only when back)
} Cand;

int lsm_plan(GpuFrame* fr, uint32_t frame_slot, const float camera[3], const float planes[6][4], float px_per_unit, LsmJob* jobs) {
    double t0 = re_time_seconds();
    GpuLsmTable* T = M.tab[frame_slot].mapped;
    fr->lsm_tab = M.dev.addr;
    fr->lsm_cull = 0;
    fr->lsm_flags = (uint32_t)M.s.taps;
    fr->lsm_params[0] = M.s.atlas > 0 ? 1.0f / (float)M.s.atlas : 0.0f;
    fr->lsm_params[1] = (float)LSM_BORDER;
    fr->lsm_params[2] = fr->lsm_params[3] = 0.0f;
    M.frame++;
    M.stats.lights = M.stats.candidates = 0;
    M.stats.faces_full = M.stats.faces_dyn = 0;
    M.stats.texels = 0.0;
    M.stats.atlas = M.s.atlas;
    int njobs = 0;
    if (!lsm_enabled()) {
        M.stats.mapped = 0;
        if (M.frame < 8 && getenv("REALENGINE_LSM_LOG")) printf("[RealEngine] lsm frame %u: off\n", M.frame);
        memset(T->hash, 0, sizeof T->hash);
        M.ndyn = M.nstat = 0, empty_box(M.dyn_over), empty_box(M.stat_over), M.all = false;
        return 0;
    }
    // 1. the candidates: casting local lights whose sphere touches the view, by importance (their size on screen, the
    // fourth root of their intensity; one already held wins ties: x1.5); the best s.lights of them kept, sorted
    const uint32_t* slots;
    const GpuLight* gl;
    uint32_t ncast = lights_cast_list(&slots, &gl);
    int held_at[LSM_MAX_LIGHTS];
    Cand best[LSM_MAX_LIGHTS];
    int nbest = 0, budget = M.s.lights;
    for (uint32_t i = 0; i < ncast; i++) {
        const GpuLight* g = &gl[i];
        uint32_t kind = g->type & 3u;
        if (kind < 1u || kind > 3u) continue;
        if (kind == 1u && g->half_h < 0.0f) continue; // (2026-11-23) an emissive actor's light: by ray only (lights.c)
        const float* c = g->position;
        float r = g->range;
        bool in = true;
        for (int p = 0; p < 6 && in; p++) in = planes[p][0] * c[0] + planes[p][1] * c[1] + planes[p][2] * c[2] + planes[p][3] >= -r;
        int held = -1;
        if (!in) { // (out of the view: only a held one, far behind the others)
            for (int k = 0; k < LSM_MAX_LIGHTS && held < 0; k++)
                if (M.e[k].used && M.e[k].slot == slots[i]) held = k;
            if (held < 0) continue;
        }
        float lum = 0.2126f * g->radiance[0] + 0.7152f * g->radiance[1] + 0.0722f * g->radiance[2];
        if (!(lum > 0.0f)) continue;
        if (in) M.stats.candidates++;
        float d[3] = { c[0] - camera[0], c[1] - camera[1], c[2] - camera[2] };
        float dist = sqrtf(dot3(d, d));
        float cover = fminf(r / fmaxf(dist, 1.0f), 100.0f);
        Cand cd = { slots[i], g, cover * sqrtf(sqrtf(lum)), cover * px_per_unit, held, in };
        if (!in) cd.imp *= 0.05f;
        for (int k = 0; k < LSM_MAX_LIGHTS && in; k++)
            if (M.e[k].used && M.e[k].slot == cd.slot) {
                cd.held = k, cd.imp *= 1.5f;
                break;
            }
        if (nbest == budget && cd.imp <= best[nbest - 1].imp) continue;
        int j = nbest < budget ? nbest++ : nbest - 1;
        while (j > 0 && best[j - 1].imp < cd.imp) best[j] = best[j - 1], j--;
        best[j] = cd;
    }
    // 2. the resolutions: a face about half the light's radius on screen (a held one keeps its size within 2x), then
    // halved from the least important up until the faces fit the atlas (at the smallest: the least important dropped)
    int res[LSM_MAX_LIGHTS], faces[LSM_MAX_LIGHTS];
    double area = 0.0, cap = (double)M.s.atlas * (double)M.s.atlas;
    for (int i = 0; i < nbest; i++) {
        int want = (int)pow2_at_least(best[i].px * 0.5f);
        if (want > M.s.max_res) want = M.s.max_res;
        faces[i] = light_faces(best[i].g);
        if (best[i].held >= 0) {
            const Ent* e = &M.e[best[i].held];
            if (e->faces == faces[i] && (want == e->res || want == e->res * 2 || want * 2 == e->res) && e->res <= M.s.max_res) want = e->res;
        }
        res[i] = want;
        area += (double)faces[i] * want * want;
    }
    while (area > cap && nbest > 0) {
        bool shrunk = false;
        for (int i = nbest - 1; i >= 0 && !shrunk; i--)
            if (res[i] > MIN_RES) area -= 0.75 * (double)faces[i] * res[i] * res[i], res[i] /= 2, shrunk = true;
        if (!shrunk) nbest--, area -= (double)faces[nbest] * res[nbest] * res[nbest];
    }
    M.stats.lights = nbest;
    // 3. the tiles: the held lights not picked (or resized) give theirs back; the new ones get theirs, the largest first
    // (powers of two from the largest down always fit a square that holds their area); fragmented: everything again
    for (int k = 0; k < LSM_MAX_LIGHTS; k++) held_at[k] = -1;
    for (int i = 0; i < nbest; i++)
        if (best[i].held >= 0) held_at[best[i].held] = i;
    for (int k = 0; k < LSM_MAX_LIGHTS; k++) {
        Ent* e = &M.e[k];
        if (!e->used) continue;
        int i = held_at[k];
        if (i < 0 || res[i] != e->res || faces[i] != e->faces) {
            ent_free(e);
            if (i >= 0) best[i].held = -1;
        }
    }
    int order[LSM_MAX_LIGHTS], nnew = 0;
    for (int i = 0; i < nbest; i++)
        if (best[i].held < 0) order[nnew++] = i;
    for (int a = 1; a < nnew; a++) { // (by size, the largest first)
        int v = order[a], b = a;
        while (b > 0 && res[order[b - 1]] < res[v]) order[b] = order[b - 1], b--;
        order[b] = v;
    }
    for (int pass = 0; pass < 2; pass++) {
        bool ok = true;
        for (int a = 0; a < nnew && ok; a++) {
            int i = order[a], k = 0;
            while (k < LSM_MAX_LIGHTS && M.e[k].used) k++;
            Ent* e = &M.e[k];
            memset(e, 0, sizeof *e);
            e->slot = best[i].slot, e->res = res[i], e->faces = faces[i], e->drawn = *best[i].g;
            if (!ent_alloc(e)) {
                ok = false;
                break;
            }
            e->used = true;
            best[i].held = k;
        }
        if (ok) break;
        // fragmented: every tile given back, all allocated again from the largest (drawn again, new lights' way)
        reset_tiles();
        nnew = 0;
        for (int i = 0; i < nbest; i++) best[i].held = -1, order[nnew++] = i;
        for (int a = 1; a < nnew; a++) {
            int v = order[a], b = a;
            while (b > 0 && res[order[b - 1]] < res[v]) order[b] = order[b - 1], b--;
            order[b] = v;
        }
    }
    // 4. what each face waits for: a light that moved (every face, at once), the dirty boxes (static: drawn whole;
    // dynamic: their casters), everything (a new level)
    for (int i = 0; i < nbest; i++) {
        if (best[i].held < 0) continue;
        Ent* e = &M.e[best[i].held];
        e->imp = best[i].imp, e->seen = M.frame;
        if (!same_geometry(&e->drawn, best[i].g)) e->moved = true, e->full = (uint8_t)((1u << e->faces) - 1u);
        e->drawn.type = best[i].g->type; // (not the geometry: the light's kind kept as it is)
        if (e->moved) continue; // (the boxes are tested on the light as drawn: a moved one redraws everything anyway)
        if (M.all) e->full = (uint8_t)((1u << e->faces) - 1u);
        mark_boxes(e, M.stat, M.nstat, M.stat_over, true);
        mark_boxes(e, M.dyn, M.ndyn, M.dyn_over, false);
    }
    M.ndyn = M.nstat = 0, empty_box(M.dyn_over), empty_box(M.stat_over), M.all = false;
    // 5. the jobs: first the lights without a drawing yet (they cast nothing until then), then the dynamic casters' redraws,
    // then the held lights drawn whole again within the budget, all by importance; a moved light's faces all in one frame
    GpuLsmJob* J = T->j;
    int ncull = 0, redraw = 0, cull_of[LSM_MAX_LIGHTS];
    for (int k = 0; k < LSM_MAX_LIGHTS; k++) cull_of[k] = -1;
    for (int phase = 0; phase < 3; phase++)
        for (int i = 0; i < nbest; i++) {
            if (best[i].held < 0) continue;
            Ent* e = &M.e[best[i].held];
            if (phase == 0 && e->valid) continue;
            if (phase > 0 && !e->valid) continue;
            if (!best[i].seen && e->valid) continue; // (out of the view: kept as it is)
            uint8_t want = phase == 1 ? (uint8_t)(e->dyn & ~e->full) : e->full;
            if (phase == 1 && !want) continue;
            if (phase == 2 && !want) continue;
            int nf = 0;
            for (int f = 0; f < e->faces; f++) nf += (want >> f) & 1;
            if (njobs + nf > LSM_MAX_JOBS) continue;
            if (phase == 2 && redraw > 0 && redraw + nf > M.s.redraw) continue;
            // the cull's sphere for this light (or it waits: the cull's room)
            int cs = cull_of[best[i].held];
            bool cs_new = cs < 0;
            if (cs_new) {
                if (ncull >= LSM_MAX_CULL) continue;
                cs = cull_of[best[i].held] = ncull++;
            }
            if (phase != 1 && e->moved) { // the light's new geometry, now (every face drawn this frame)
                e->drawn = *best[i].g;
                ent_frame(e);
                e->moved = false;
                if (phase == 2) want = e->full = (uint8_t)((1u << e->faces) - 1u);
            } else if (e->s == 0.0f)
                ent_frame(e);
            memcpy(T->cull[cs], e->drawn.position, 12);
            bool stat = phase != 1 || (!cs_new && T->cull[cs][3] > 0.0f);
            T->cull[cs][3] = stat ? e->range : -e->range;
            for (int f = 0; f < e->faces; f++) {
                if (!((want >> f) & 1)) continue;
                GpuLsmJob* jb = &J[njobs];
                memcpy(jb->m, e->m[f], sizeof jb->m);
                memcpy(jb->planes, e->planes[f], sizeof jb->planes);
                memcpy(jb->sphere, e->drawn.position, 12);
                jb->sphere[3] = e->range;
                jobs[njobs] = (LsmJob){ e->tx[f], e->ty[f], (uint32_t)e->res, (uint32_t)njobs, phase != 1 };
                njobs++;
                M.stats.texels += (double)e->res * e->res * 1e-6;
                if (phase != 1) M.stats.faces_full++;
                else M.stats.faces_dyn++;
            }
            if (phase == 2) redraw += nf;
            if (phase == 1) e->dyn = 0;
            else e->full = 0, e->dyn = 0, e->valid = true;
        }
    fr->lsm_cull = (uint32_t)ncull;
    // 6. the table: the lights with a drawing, each as drawn
    memset(T->hash, 0, sizeof T->hash);
    int nl = 0;
    for (int k = 0; k < LSM_MAX_LIGHTS; k++) {
        Ent* e = &M.e[k];
        if (!e->used) continue;
        if (e->seen != M.frame) { // (not picked: its tiles back)
            ent_free(e);
            continue;
        }
        if (!e->valid) continue;
        GpuLsmLight* L = &T->l[nl];
        memcpy(L->pos_near, e->drawn.position, 12);
        L->pos_near[3] = NEAR_CM;
        L->params[0] = e->range, L->params[1] = (float)e->res, L->params[2] = 1.0f / (float)M.s.atlas, L->params[3] = e->k;
        for (int f = 0; f < 6; f++) {
            uint32_t t = f < e->faces ? (uint32_t)e->tx[f] | (uint32_t)e->ty[f] << 16 : 0u;
            if (f < 4) L->t0[f] = t;
            else L->t1[f - 4] = t;
        }
        L->t1[2] = (uint32_t)e->faces;
        memcpy(&L->t1[3], &e->s, 4);
        memcpy(L->m, e->m[0], sizeof L->m);
        uint32_t h = (e->slot * 2654435761u) >> 24; // (8 bits: LSM_HASH 256; shaders/lsm.glsl the same)
        for (int p = 0; p < 8; p++, h = (h + 1u) & (LSM_HASH - 1)) // (at most 64 of 256: a free bucket is near; the shader looks at 8)
            if (!T->hash[h][0]) {
                T->hash[h][0] = e->slot + 1u, T->hash[h][1] = (uint32_t)nl;
                break;
            }
        nl++;
    }
    if ((njobs || M.frame < 8) && getenv("REALENGINE_LSM_LOG")) // (a debugging aid: what was drawn when)
        printf("[RealEngine] lsm frame %u: %d jobs (%u whole, %u dynamic), %d lights, %d in the table\n", M.frame, njobs, M.stats.faces_full,
               M.stats.faces_dyn, nbest, nl);
    M.stats.mapped = nl;
    M.stats.full_total += M.stats.faces_full, M.stats.dyn_total += M.stats.faces_dyn, M.stats.frames++;
    M.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
    M.stats.cpu_sum += M.stats.cpu_ms;
    return njobs;
}
