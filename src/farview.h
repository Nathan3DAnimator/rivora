// (Q21) The distant view, as far as Red Dead Redemption 2 shows it (notas-dev/visao-distante.md):
// - the terrain's far shadow: the landscapes' heights in a grid (the CPU, again when they change), the steepest rise toward
//   the sun per texel on the GPU (shaders/farshadow.comp, again when the sun turns), read by the sun past its virtual
//   maps (vsm.glsl -> farview.glsl): mountains shade the valleys kilometers away;
// - impostors: a mesh many instances share (trees, rocks, the HLOD's proxies) baked from 8 x 8 directions of the upper
//   hemisphere into two atlases (base color and alpha; normal, depth, roughness), drawn far away as one camera-facing card
//   per instance (cull.comp swaps the draw, mesh.vert turns the card, mesh.frag / masked.frag read the three nearest
//   frames with a step of parallax): the far forest at the cost of its pixels;
// - the analytic fog's ambient light from the sky's horizon (weather.glsl): mountains fade into the sky behind them.
// The renderer calls it around its frame (render.c); the settings are GiSettings' far_* / impostor_* (render_scalability).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "render_gpu.h"

struct VkCtx;
struct GiSettings;

#define FAR_SHADOW 1u   // GpuFrame far_u.x (shaders/farview.glsl)
#define FAR_IMP 2u
#define FAR_FOG_SKY 4u
#define FARV_FRAMES 8u  // an impostor's frames a side (hemi-octahedral)
#define FARV_MAX_IMP 96 // impostors at once (two atlases each)

typedef struct FarviewStats {
    // the far shadow
    bool shadow_on;
    uint32_t res;                // texels a side
    float texel_cm, extent_km;   // the heightfield's
    uint32_t hf_builds, shadow_runs;
    double hf_ms;                // the last heightfield's CPU time (parallel)
    double shadow_gpu_ms;        // the last horizon pass's GPU time (timestamps 32-33)
    // the impostors
    bool imp_on;
    uint32_t impostors, candidates, failed;
    uint32_t atlas;              // the atlases' side
    double bake_ms, bake_ms_total; // the last bake (a one-time submit: CPU + GPU), all of them
    uint64_t bytes;              // the atlases' memory
    float switch_px;             // the screen radius under which the card is drawn
    float fog_sky;
    uint32_t shadow_runs_total;
} FarviewStats;

// The bake's pipeline (on the renderer's raster layout: set 1 the materials) and the horizon's compute
bool farview_init(struct VkCtx* vk, VkPipelineLayout raster_layout);
void farview_shutdown(struct VkCtx* vk);
// The meshes were loaded again: every impostor dropped (their slots may hold other meshes now)
void farview_meshes_changed(struct VkCtx* vk);
// render.c instance_of: an instance of this mesh (slot) with this material (its slot + 1; 0xFFFFFFFF: no material, its
// color) that an impostor may stand for (static, opaque or Masked, not a skin); plant: a foliage instance; allowed false:
// its actor turned the impostors off (a Foliage actor's Impostors): the mesh never gets one
void farview_note_instance(uint32_t mesh, uint32_t key, bool plant, bool allowed, const float bmn[3], const float bmx[3]);
// Once a frame before the cull (main thread): the settings, the heightfield again when the landscapes changed, at most
// one impostor baked (a one-time submit). sun: toward the sun (unit; NULL: none)
void farview_prepare(struct VkCtx* vk, const struct GiSettings* gi, const float sun[3]);
// The horizon pass when the sun turned or the heightfield changed (before the main passes; compute). Returns true when it
// recorded (the caller wrote the timestamps around it)
bool farview_record(VkCommandBuffer cmd);
// The frame's fields (card: the card mesh's slot and its first vertex; NONE: no impostors)
void farview_fill_frame(GpuFrame* fr, uint32_t card_slot, int32_t card_vofs);
void farview_collect(double shadow_gpu_ms); // (the timestamps' read back, when the pass ran)
const FarviewStats* farview_stats(void);
// The far shadow at a point on the CPU (the same as the shader: 1 lit) and whether the grid holds it (the tests)
bool farview_shadow_at(const float p[3], float* vis);
void farview_rebuild(void); // the heightfield again (a landscape changed without its edits counted)
void farview_set_switch_override(float px); // the impostors' switch (screen radius, px) forced (0: the settings'; the tests)
