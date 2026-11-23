// The sun's virtual shadow maps: a cached clipmap (vsm.h). What is kept per level is its drawing's frame (center,
// basis, depth range): the main pass reads each level with the matrix it was drawn with, so a level waiting for its
// turn (the budget) stays right, only older.
#include "vsm.h"

#include <math.h>
#include <string.h>

#include "vkctx.h"

#define MAX_BOXES 256 // dirty world boxes a frame: cells of an 8 m grid (more: one overflow box)
#define GRID 1024     // the hash table of those cells
#define MAX_RECTS 4   // dirty rectangles a level keeps (more merge)
#define RESUN_COS 0.99999985f // the sun turned past ~0.03 degrees: the level is drawn again with the new direction

typedef struct Level {
    bool valid;            // drawn with the current settings
    bool redo;             // drawn whole again on its turn (camera, sun, depth range, a static change)
    double cx, cy;         // its center along lr / lu (cm)
    float lr[3], lu[3], lf[3];
    float z0, z1;          // its depth range along lf (cm)
    int nrects;
    float rects[MAX_RECTS][4]; // pending (center-relative cm): x0, y0, x1, y1
    int nlazy;
    float lazy[MAX_RECTS][4];  // dirty but unseen: inside the finer level's part of the view (no pixel reads it); made
                               // dirty when a finer level is drawn again (re-centered: it may show)
    uint32_t drawn_at;     // the frame it was last drawn whole
} Level;

typedef struct Cell Cell;
static struct {
    VkCtx* vk;
    VsmSettings s;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, layer[VSM_MAX_LEVELS], slayer[VSM_MAX_LEVELS]; // slayer: the static casters alone (layers L..2L-1)
    float sbox[6];       // a static caster changed here this frame (its levels drawn whole again)
    bool cleared;          // the layers start at depth 1 (vsm_plan's first frame: a clear job each)
    Level L[VSM_MAX_LEVELS];
    struct Cell { float box[6]; int ix, iy, iz; uint32_t stamp; } cells[GRID];
    uint16_t used[MAX_BOXES];
    int nboxes;
    uint32_t stamp;   // (this frame's cells: the others are free)
    float over[6];    // the overflow box (empty: min > max)
    bool all;              // everything drawn again (vsm_invalidate)
    uint32_t frame;
    VsmStats stats;
} V;

static void reset_boxes(void);
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool vsm_init(VkCtx* vk) {
    memset(&V, 0, sizeof V);
    V.vk = vk;
    reset_boxes();
    return true;
}

static void destroy_image(void) {
    VkDevice d = V.vk->device;
    for (int i = 0; i < VSM_MAX_LEVELS; i++) {
        if (V.layer[i]) vkDestroyImageView(d, V.layer[i], NULL), V.layer[i] = VK_NULL_HANDLE;
        if (V.slayer[i]) vkDestroyImageView(d, V.slayer[i], NULL), V.slayer[i] = VK_NULL_HANDLE;
    }
    if (V.view) vkDestroyImageView(d, V.view, NULL), V.view = VK_NULL_HANDLE;
    if (V.image) vkDestroyImage(d, V.image, NULL), V.image = VK_NULL_HANDLE;
    if (V.memory) vkFreeMemory(d, V.memory, NULL), V.memory = VK_NULL_HANDLE;
}

void vsm_shutdown(VkCtx* vk) {
    (void)vk;
    if (V.vk) destroy_image();
}

bool vsm_configure(VkCtx* vk, const VsmSettings* s, bool* views_changed) {
    *views_changed = false;
    VsmSettings n = *s;
    if (n.levels < 0) n.levels = 0;
    if (n.levels > VSM_MAX_LEVELS) n.levels = VSM_MAX_LEVELS;
    n.res = n.res <= 1024 ? 1024 : 2048;
    int layers = n.levels ? n.levels : 1; // (off: one layer, never drawn: the descriptor stays valid)
    bool remake = !V.image || n.res != V.s.res || layers != (V.s.levels ? V.s.levels : 1);
    V.s = n;
    if (!remake) return true;
    if (V.image) vkDeviceWaitIdle(vk->device);
    destroy_image();
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VSM_FORMAT;
    ii.extent = (VkExtent3D){ (uint32_t)n.res, (uint32_t)n.res, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = (uint32_t)layers * 2u; // (the levels, then their static casters alone: Unreal's static / dynamic split)
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &V.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, V.image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &V.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(vk->device, V.image, V.memory, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = V.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VSM_FORMAT;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, (uint32_t)layers };
    if (vkCreateImageView(vk->device, &vi, NULL, &V.view) != VK_SUCCESS) return false;
    for (int l = 0; l < layers; l++) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, (uint32_t)l, 1 };
        if (vkCreateImageView(vk->device, &vi, NULL, &V.layer[l]) != VK_SUCCESS) return false;
        vi.subresourceRange.baseArrayLayer = (uint32_t)(layers + l);
        if (vkCreateImageView(vk->device, &vi, NULL, &V.slayer[l]) != VK_SUCCESS) return false;
    }
    // into GENERAL at depth 1 once (it stays GENERAL: drawn into and sampled, barriers between)
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = V.image;
    b.subresourceRange = vi.subresourceRange;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = (uint32_t)layers * 2u;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    VkClearDepthStencilValue one = { 1.0f, 0 };
    vkCmdClearDepthStencilImage(cmd, V.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cmd, &di);
    gpu_end_once(vk, cmd);
    for (int l = 0; l < VSM_MAX_LEVELS; l++) V.L[l].valid = false;
    *views_changed = true;
    return true;
}

VkImageView vsm_view(void) { return V.view; }
VkImageView vsm_layer_view(int level) { return level >= 0 && level < VSM_MAX_LEVELS ? V.layer[level] : VK_NULL_HANDLE; }
VkImageView vsm_static_view(int level) { return level >= 0 && level < VSM_MAX_LEVELS ? V.slayer[level] : VK_NULL_HANDLE; }
int vsm_levels(void) { return V.s.levels; }
void vsm_dirty_static(const float mn[3], const float mx[3]) {
    for (int k = 0; k < 3; k++) V.sbox[k] = fminf(V.sbox[k], mn[k]), V.sbox[k + 3] = fmaxf(V.sbox[k + 3], mx[k]);
}
VkImage vsm_image(void) { return V.image; }
bool vsm_enabled(void) { return V.s.levels > 0 && V.image; }
const VsmStats* vsm_stats(void) { return &V.stats; }
void vsm_stats_reset(void) { V.stats.full_total = V.stats.partial_total = V.stats.frames = 0; }

static void reset_boxes(void) {
    V.nboxes = 0;
    V.stamp++;
    for (int k = 0; k < 3; k++) V.over[k] = V.sbox[k] = 1e30f, V.over[k + 3] = V.sbox[k + 3] = -1e30f;
}

void vsm_invalidate(void) { V.all = true; }

void vsm_dirty_box(const float mn[3], const float mx[3]) {
    if (!(mn[0] <= mx[0])) return;
    // into the cell of an 8 m grid its center falls in (a hash table): O(1) a box. A thousand animated characters send
    // ~60000 boxes a frame (each part where it is and where it was); merging each into the nearest of a list was 5 ms
    int ix = (int)floorf((mn[0] + mx[0]) * 0.5f / 800.0f), iy = (int)floorf((mn[1] + mx[1]) * 0.5f / 800.0f), iz = (int)floorf((mn[2] + mx[2]) * 0.5f / 800.0f);
    uint32_t h = ((uint32_t)ix * 73856093u ^ (uint32_t)iy * 19349663u ^ (uint32_t)iz * 83492791u) & (GRID - 1);
    for (int probe = 0; probe < 8; probe++, h = (h + 1) & (GRID - 1)) {
        Cell* c = &V.cells[h];
        if (c->stamp != V.stamp) { // a free cell (this frame)
            if (V.nboxes >= MAX_BOXES) break;
            c->stamp = V.stamp, c->ix = ix, c->iy = iy, c->iz = iz;
            memcpy(c->box, mn, 3 * sizeof(float));
            memcpy(c->box + 3, mx, 3 * sizeof(float));
            V.used[V.nboxes++] = (uint16_t)h;
            return;
        }
        if (c->ix == ix && c->iy == iy && c->iz == iz) {
            for (int k = 0; k < 3; k++) c->box[k] = fminf(c->box[k], mn[k]), c->box[k + 3] = fmaxf(c->box[k + 3], mx[k]);
            return;
        }
    }
    // (the table full: into the overflow box)
    for (int k = 0; k < 3; k++) V.over[k] = fminf(V.over[k], mn[k]), V.over[k + 3] = fmaxf(V.over[k + 3], mx[k]);
}

static float level_side(int l) { return VSM_LEVEL0_CM * (float)(1u << l); }

// A level's rectangle list: merged with one it touches, or (full) with the nearest
static void add_rect(Level* lv, const float r[4]) {
    for (int i = 0; i < lv->nrects; i++) {
        float* q = lv->rects[i];
        if (r[0] <= q[2] && r[2] >= q[0] && r[1] <= q[3] && r[3] >= q[1]) {
            q[0] = fminf(q[0], r[0]), q[1] = fminf(q[1], r[1]), q[2] = fmaxf(q[2], r[2]), q[3] = fmaxf(q[3], r[3]);
            return;
        }
    }
    if (lv->nrects < MAX_RECTS) {
        memcpy(lv->rects[lv->nrects++], r, 4 * sizeof(float));
        return;
    }
    int best = 0;
    float grow = 1e38f;
    for (int i = 0; i < MAX_RECTS; i++) {
        const float* q = lv->rects[i];
        float a0 = (q[2] - q[0]) * (q[3] - q[1]);
        float a1 = (fmaxf(q[2], r[2]) - fminf(q[0], r[0])) * (fmaxf(q[3], r[3]) - fminf(q[1], r[1]));
        if (a1 - a0 < grow) grow = a1 - a0, best = i;
    }
    float* q = lv->rects[best];
    q[0] = fminf(q[0], r[0]), q[1] = fminf(q[1], r[1]), q[2] = fmaxf(q[2], r[2]), q[3] = fmaxf(q[3], r[3]);
}

// (the unseen list: merged the same way)
static void add_lazy(Level* lv, const float r[4]) {
    int n = lv->nrects;
    float keep[MAX_RECTS][4];
    memcpy(keep, lv->rects, sizeof keep);
    lv->nrects = lv->nlazy;
    memcpy(lv->rects, lv->lazy, sizeof lv->lazy);
    add_rect(lv, r);
    lv->nlazy = lv->nrects;
    memcpy(lv->lazy, lv->rects, sizeof lv->lazy);
    lv->nrects = n;
    memcpy(lv->rects, keep, sizeof keep);
}

static void basis_of(const float lf[3], float lr[3], float lu[3]) {
    float ref[3] = { 0, 0, 1 };
    if (fabsf(lf[2]) > 0.99f) ref[0] = 1, ref[2] = 0;
    lr[0] = ref[1] * lf[2] - ref[2] * lf[1];
    lr[1] = ref[2] * lf[0] - ref[0] * lf[2];
    lr[2] = ref[0] * lf[1] - ref[1] * lf[0];
    float n = sqrtf(dot3(lr, lr));
    for (int k = 0; k < 3; k++) lr[k] /= n;
    lu[0] = lf[1] * lr[2] - lf[2] * lr[1];
    lu[1] = lf[2] * lr[0] - lf[0] * lr[2];
    lu[2] = lf[0] * lr[1] - lf[1] * lr[0];
}

int vsm_plan(GpuFrame* fr, const float camera[3], const float light_dir[3], const float cmn[3], const float cmx[3],
             VsmJob* jobs, float planes[6][4]) {
    V.frame++;
    VsmStats* st = &V.stats;
    st->levels = V.s.levels, st->res = V.s.res;
    st->jobs = st->full = st->partial = 0;
    st->texels = 0.0;
    memset(fr->vsm_level, 0, sizeof fr->vsm_level);
    for (int p = 0; p < 6; p++) planes[p][0] = planes[p][1] = planes[p][2] = 0.0f, planes[p][3] = -1.0f;
    float tan_r = tanf(0.5f * fmaxf(V.s.sun_angle, 0.0f) * 3.14159265f / 180.0f);
    fr->vsm_params[0] = (float)V.s.levels;
    fr->vsm_params[1] = (float)V.s.taps;
    fr->vsm_params[2] = tan_r;
    fr->vsm_params[3] = 0.0f;
    if (!vsm_enabled()) {
        reset_boxes(), V.all = false;
        return 0;
    }
    st->frames++;
    float lf[3] = { light_dir[0], light_dir[1], light_dir[2] }, lr[3], lu[3];
    basis_of(lf, lr, lu);
    // the casters' depth range along the light (with room: a level isn't drawn again for every crate that falls)
    bool casters = cmn[0] <= cmx[0];
    float zmn = 1e30f, zmx = -1e30f;
    for (int c = 0; casters && c < 8; c++) {
        float p[3] = { c & 1 ? cmx[0] : cmn[0], c & 2 ? cmx[1] : cmn[1], c & 4 ? cmx[2] : cmn[2] };
        float z = dot3(p, lf);
        zmn = fminf(zmn, z), zmx = fmaxf(zmx, z);
    }
    if (!casters) zmn = dot3(camera, lf) - 1000.0f, zmx = zmn + 2000.0f;
    double camx = dot3(camera, lr), camy = dot3(camera, lu);
    int nlev = V.s.levels, njobs = 0;
    bool any_full = false;
    // 1. what each level needs: drawn whole (new, moved off, sun, depth) or its dirty rectangles
    int want_full[VSM_MAX_LEVELS];
    for (int l = 0; l < nlev; l++) {
        Level* lv = &V.L[l];
        float side = level_side(l), texel = side / (float)V.s.res, step = side * 0.125f;
        want_full[l] = 0;
        if (!lv->valid || V.all) {
            want_full[l] = 2; // (new: drawn now, whatever the budget)
        } else {
            if (dot3(lv->lf, lf) < RESUN_COS) lv->redo = true;
            if (fabs(camx - lv->cx) > step * 0.75 || fabs(camy - lv->cy) > step * 0.75) lv->redo = true;
            if (zmn < lv->z0 + 50.0f || zmx > lv->z1 - 50.0f) lv->redo = true;
            // (off by more than half its side: nothing of it is around the camera, it is as good as new)
            if (fabs(camx - lv->cx) > side * 0.5 || fabs(camy - lv->cy) > side * 0.5) want_full[l] = 2;
            else if (lv->redo) want_full[l] = 1;
        }
        // a static caster changed (the editor moved one): the level drawn whole on its turn (the static layer with it)
        if (lv->valid && want_full[l] == 0 && V.sbox[0] <= V.sbox[3]) {
            float r[4] = { 1e30f, 1e30f, -1e30f, -1e30f }, h = side * 0.5f;
            for (int c = 0; c < 8; c++) {
                float p[3] = { c & 1 ? V.sbox[3] : V.sbox[0], c & 2 ? V.sbox[4] : V.sbox[1], c & 4 ? V.sbox[5] : V.sbox[2] };
                float x = (float)(dot3(p, lv->lr) - lv->cx), y = (float)(dot3(p, lv->lu) - lv->cy);
                r[0] = fminf(r[0], x), r[1] = fminf(r[1], y), r[2] = fmaxf(r[2], x), r[3] = fmaxf(r[3], y);
            }
            if (r[0] < h && r[2] > -h && r[1] < h && r[3] > -h) lv->redo = true, want_full[l] = 1;
        }
        // the dirty boxes as rectangles of this level (its own basis: the drawing it holds)
        if (lv->valid && want_full[l] != 2) {
            for (int b = 0; b <= V.nboxes; b++) {
                const float* bx = b < V.nboxes ? V.cells[V.used[b]].box : V.over; // (the last: the overflow box)
                if (!(bx[0] <= bx[3])) continue;
                float r[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
                for (int c = 0; c < 8; c++) {
                    float p[3] = { c & 1 ? bx[3] : bx[0], c & 2 ? bx[4] : bx[1], c & 4 ? bx[5] : bx[2] };
                    float x = (float)(dot3(p, lv->lr) - lv->cx), y = (float)(dot3(p, lv->lu) - lv->cy);
                    r[0] = fminf(r[0], x), r[1] = fminf(r[1], y), r[2] = fmaxf(r[2], x), r[3] = fmaxf(r[3], y);
                }
                float h = side * 0.5f;
                // a texel of room, then snapped to texels and clipped to the level
                r[0] = fmaxf(floorf(r[0] / texel - 1.0f) * texel, -h), r[1] = fmaxf(floorf(r[1] / texel - 1.0f) * texel, -h);
                r[2] = fminf(ceilf(r[2] / texel + 1.0f) * texel, h), r[3] = fminf(ceilf(r[3] / texel + 1.0f) * texel, h);
                if (!(r[0] < r[2] && r[1] < r[3])) continue;
                // every pixel over it reads the finer level (inside its part, short of the blend): left for later. A walking
                // crowd redraws two or three levels, not all eight
                const Level* fl = l > 0 ? &V.L[l - 1] : NULL;
                if (fl && fl->valid && want_full[l - 1] != 2 && dot3(fl->lf, lv->lf) >= 0.99999f) {
                    float fside = level_side(l - 1), read = 0.9f * (fside * 0.5f - 24.0f * fside / (float)V.s.res) - fside / (float)V.s.res;
                    float ox = (float)(lv->cx - fl->cx), oy = (float)(lv->cy - fl->cy);
                    if (fabsf(r[0] + ox) < read && fabsf(r[2] + ox) < read && fabsf(r[1] + oy) < read && fabsf(r[3] + oy) < read) {
                        add_lazy(lv, r);
                        continue;
                    }
                }
                add_rect(lv, r);
            }
        }
    }
    reset_boxes();
    V.all = false;
    // 2. the whole drawings: new ones all, then the budget's worth of the others (finest first, then the oldest)
    int budget = V.s.full_budget > 0 ? V.s.full_budget : 1;
    for (int pass = 0; pass < 2; pass++)
        for (int l = 0; l < nlev; l++) {
            if (want_full[l] != 2 - pass) continue;
            if (pass == 1) {
                if (budget <= 0) break;
                budget--;
            }
            want_full[l] = 3; // (chosen)
        }
    // 3. the jobs and the frame's matrices
    float jmn[2] = { 1e30f, 1e30f }, jmx[2] = { -1e30f, -1e30f };
    bool shared_basis = true;
    for (int l = 0; l < nlev; l++) {
        Level* lv = &V.L[l];
        float side = level_side(l), texel = side / (float)V.s.res, step = side * 0.125f, h = side * 0.5f;
        if (want_full[l] == 3) {
            memcpy(lv->lf, lf, sizeof lf), memcpy(lv->lr, lr, sizeof lr), memcpy(lv->lu, lu, sizeof lu);
            lv->cx = floor(camx / step + 0.5) * step;
            lv->cy = floor(camy / step + 0.5) * step;
            float pad = fmaxf(1000.0f, 0.1f * (zmx - zmn));
            lv->z0 = zmn - pad, lv->z1 = zmx + pad;
            lv->valid = true, lv->redo = false, lv->nrects = 0, lv->drawn_at = V.frame;
            lv->nlazy = 0;
            for (int m = l + 1; m < nlev; m++) { // (the coarser levels' unseen rectangles may show now)
                Level* cl = &V.L[m];
                for (int i = 0; i < cl->nlazy; i++) add_rect(cl, cl->lazy[i]);
                cl->nlazy = 0;
            }
            VsmJob* j = &jobs[njobs++];
            memset(j, 0, sizeof *j);
            j->level = l, j->full = true;
            j->w = j->h = (uint32_t)V.s.res;
            fr->vsm_job[l][0] = -h, fr->vsm_job[l][1] = -h, fr->vsm_job[l][2] = fr->vsm_job[l][3] = 2.0f / side;
            st->full++, st->full_total++;
            st->texels += (double)V.s.res * V.s.res * 1e-6;
            any_full = true;
        } else if (lv->valid && lv->nrects) {
            VsmJob* j = &jobs[njobs++];
            memset(j, 0, sizeof *j);
            j->level = l;
            float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
            for (int i = 0; i < lv->nrects; i++) {
                const float* r = lv->rects[i];
                b[0] = fminf(b[0], r[0]), b[1] = fminf(b[1], r[1]), b[2] = fmaxf(b[2], r[2]), b[3] = fmaxf(b[3], r[3]);
                uint32_t* c = j->clear[j->nclear++];
                c[0] = (uint32_t)lroundf((r[0] + h) / texel), c[1] = (uint32_t)lroundf((r[1] + h) / texel);
                c[2] = (uint32_t)lroundf((r[2] - r[0]) / texel), c[3] = (uint32_t)lroundf((r[3] - r[1]) / texel);
                st->texels += (double)c[2] * c[3] * 1e-6;
            }
            j->x = (uint32_t)lroundf((b[0] + h) / texel), j->y = (uint32_t)lroundf((b[1] + h) / texel);
            j->w = (uint32_t)lroundf((b[2] - b[0]) / texel), j->h = (uint32_t)lroundf((b[3] - b[1]) / texel);
            if (j->x + j->w > (uint32_t)V.s.res) j->w = (uint32_t)V.s.res - j->x;
            if (j->y + j->h > (uint32_t)V.s.res) j->h = (uint32_t)V.s.res - j->y;
            if (!j->w || !j->h) {
                njobs--;
            } else {
                fr->vsm_job[l][0] = b[0], fr->vsm_job[l][1] = b[1];
                fr->vsm_job[l][2] = 2.0f / ((float)j->w * texel), fr->vsm_job[l][3] = 2.0f / ((float)j->h * texel);
                st->partial++, st->partial_total++;
            }
            lv->nrects = 0;
        }
        // its matrix (as drawn): x, y cm from its center along lr / lu, z the depth 0..1 over its range
        if (lv->valid) {
            float* m = fr->vsm_mat[l];
            memset(m, 0, 16 * sizeof(float));
            float dz = lv->z1 - lv->z0;
            for (int k = 0; k < 3; k++) {
                m[k * 4 + 0] = lv->lr[k];
                m[k * 4 + 1] = lv->lu[k];
                m[k * 4 + 2] = lv->lf[k] / dz;
            }
            m[12] = (float)-lv->cx;
            m[13] = (float)-lv->cy;
            m[14] = -lv->z0 / dz;
            m[15] = 1.0f;
            fr->vsm_level[l][0] = h, fr->vsm_level[l][1] = texel, fr->vsm_level[l][2] = dz, fr->vsm_level[l][3] = 1.0f;
        }
        // the shadow box: the jobs' rectangles in the light's frame (absolute)
        if (njobs && jobs[njobs - 1].level == l) {
            const VsmJob* j = &jobs[njobs - 1];
            if (dot3(lv->lf, lf) < 0.99999f) shared_basis = false;
            float x0 = (float)(lv->cx - h) + (float)j->x * texel, y0 = (float)(lv->cy - h) + (float)j->y * texel;
            jmn[0] = fminf(jmn[0], x0), jmn[1] = fminf(jmn[1], y0);
            jmx[0] = fmaxf(jmx[0], x0 + (float)j->w * texel), jmx[1] = fmaxf(jmx[1], y0 + (float)j->h * texel);
        }
    }
    (void)any_full;
    st->jobs = (uint32_t)njobs;
    if (!njobs) return 0;
    if (shared_basis) { // the four sides along lr / lu, the depth range along lf (all of the scene's casters)
        float m = 64.0f; // (a sphere test: a little room for the rows' float error)
        for (int k = 0; k < 3; k++) {
            planes[0][k] = lr[k], planes[1][k] = -lr[k], planes[2][k] = lu[k], planes[3][k] = -lu[k];
            planes[4][k] = lf[k], planes[5][k] = -lf[k];
        }
        planes[0][3] = -(jmn[0] - m), planes[1][3] = jmx[0] + m;
        planes[2][3] = -(jmn[1] - m), planes[3][3] = jmx[1] + m;
        planes[4][3] = -(zmn - 1000.0f), planes[5][3] = zmx + 1000.0f;
    } else { // (a level still in an older sun's frame: every caster)
        for (int p = 0; p < 6; p++) planes[p][0] = planes[p][1] = planes[p][2] = 0.0f, planes[p][3] = 1.0f;
    }
    return njobs;
}
