// The local lights' shadow maps (notas-dev/luz.md, "Sombras das luzes locais por mapa"): Low and the GPUs without ray
// tracing, where the point, spot and rect lights had no shadow (a lamp behind a wall lit the other room). One depth atlas
// (D32, two layers: the maps, and their static casters alone) shared by up to LSM_MAX_LIGHTS lights, picked every frame
// by importance (their sphere's size on screen times the fourth root of the intensity; a held one wins ties), each with
// a resolution by its size on screen (powers of two, a buddy quadtree keeps the tiles where they are). A point (or rect,
// or a spot wider than 60 degrees) light draws six cube faces, a spot one; every face keeps a border of LSM_BORDER texels
// so the filter never reads outside it. Cached like the sun's virtual shadow maps: a face is drawn again only when its
// light moved, a static caster changed inside it (static layer drawn, copied) or a dynamic one moved inside it (the static
// layer copied back, the dynamic ones drawn over). The cull (cull.comp pass 0) adds the casters inside the spheres of the
// lights drawn this frame to the shadow list, and the vertex shader (mesh.vert pass 1) drops every instance outside the
// face's frustum before its vertices are read. mesh.frag reads them with a 2x2 / 3x3 grid of bilinear compares (smooth,
// no noise), a normal offset of a texel and a half plus the pipeline's slope bias (no acne).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "render_gpu.h"

struct VkCtx;

#define LSM_MAX_LIGHTS 64 // lights with a map at once (shaders/lsm.glsl LSM_MAX)
#define LSM_MAX_JOBS 96   // faces drawn in a frame
#define LSM_MAX_CULL 32   // lights drawn in a frame (their spheres for the cull)
#define LSM_HASH 256      // the light slot -> map table (open addressing: at most a quarter full, the shader probes 8)
#define LSM_BORDER 3      // texels around each face (the 3x3 filter reaches 2.5)
#define LSM_FORMAT VK_FORMAT_D32_SFLOAT

typedef struct LsmSettings {
    int lights;  // lights with a map at once (0: off)
    int atlas;   // texels a side (2048 or 4096)
    int max_res; // a face's largest side
    int taps;    // 4: a 2x2 grid of bilinear compares, 9: 3x3
    int redraw;  // faces of lights already drawn redrawn whole in a frame (moved, a static change); new ones and the
                 // dynamic casters' redraws are outside it (up to LSM_MAX_JOBS)
} LsmSettings;

// One face drawn this frame (render.c): its tile, whole (the static layer drawn, then copied) or the dynamic casters only
typedef struct LsmJob {
    uint32_t x, y, size;
    uint32_t index; // into the table's jobs (mesh.vert: push.pad)
    bool full;
} LsmJob;

typedef struct LsmStats {
    int lights;        // picked this frame
    int mapped;        // with a drawing in the table (the shaders read them)
    int candidates;    // casting lights in the view
    int atlas;
    uint32_t faces_full, faces_dyn; // drawn this frame
    double texels;     // millions redrawn this frame
    double cpu_ms;     // lsm_plan
    uint64_t full_total, dyn_total, frames;
    double gpu_sum, gpu_max, cpu_sum; // the frames that drew (render.c's timestamps), the plans' CPU
    uint64_t gpu_frames;
} LsmStats;

bool lsm_init(struct VkCtx* vk);
void lsm_shutdown(struct VkCtx* vk);
// (Re)creates the atlas for these settings (waits for the GPU then; lights 0: a 1x1 stand-in); views changed: rebind
bool lsm_configure(struct VkCtx* vk, const LsmSettings* s, bool* views_changed);
bool lsm_enabled(void);
int lsm_atlas_side(void);
VkImageView lsm_view(void);           // the maps (sampler2DShadow, binding 66)
VkImageView lsm_layer_view(int layer); // 0: the maps, 1: their static casters (the depth attachments)
VkImage lsm_image(void);

// A shadow caster changed inside this world box (dynamic: before and after a move), a static one, everything
void lsm_dirty_box(const float mn[3], const float mx[3]);
void lsm_dirty_static(const float mn[3], const float mx[3]);
void lsm_invalidate(void);

// This frame: the casting local lights in the view (lights.c's list) picked and given tiles, the faces to draw (jobs,
// at most LSM_MAX_JOBS), the table into this frame slot's buffer; fills fr's lsm_* fields. planes: the view's (inside:
// dot >= 0), px_per_unit: radius / distance -> pixels
int lsm_plan(GpuFrame* fr, uint32_t frame_slot, const float camera[3], const float planes[6][4], float px_per_unit,
             LsmJob* jobs);
// The frame slot's table into the GPU's (with the frame's data: the same barrier after)
void lsm_upload(VkCommandBuffer cmd, uint32_t frame_slot);
const LsmStats* lsm_stats(void);
void lsm_note_gpu(double ms); // (render.c: a frame's drawing measured)
void lsm_stats_reset(void);
