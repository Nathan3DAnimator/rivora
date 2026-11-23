// (Q11) The outline pass (notas-dev/toon.md, shaders/outline_id.frag, shaders/outline.comp): the outlined actors (a
// material's outline, or an actor's own) drawn into an id target at the render's size (their entry in the frame's
// table, a hash of the actor, the normal; their own depth), then two compute passes over it: the silhouette's pixels
// (depth + normal + id, against the scene's depth: nothing hidden is outlined), each with its width in pixels from its own
// distance (Auto Adaptive: the curve), spread as a disk by two separable passes (r^2 - dx^2, then - dy^2: exact circles,
// one row and one column of shared memory a pixel), its edge anti-aliased, into the scene's color before the TSR (which
// smooths it like every edge: the render's jitter, no shimmer). Its cost measured by its own timestamps.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "render_gpu.h"
#include "toon.h"

struct VkCtx;

#define OUTLINE_MAX_ENTRIES 255
#define OUTLINE_MAX_RADIUS 12 // render px (a 24 px line at most)

// One look the frame's outlines take (shaders/outline.comp OlEntry): a material's or an actor's
typedef struct OutlineEntry {
    float color[4];   // rgb linear (the line as the screen shows it), w the width in px at 1440p
    float curve[4];   // near cm, far cm, near scale, far scale (1, 1: not adaptive)
    float edge[4];    // the curve's exponent, the depth's sensitivity (a share of the distance), cos of the crease (< -1: none), opacity
    float pad[4];
} OutlineEntry;

// An outlined actor's draw (its scene slot as the instance; its mesh's LOD 0)
typedef struct OutlineDraw {
    uint32_t slot, first_index, index_count, entry; // entry: 1..255
    int32_t vertex_offset;
} OutlineDraw;

bool outline_init(struct VkCtx* vk, VkDescriptorSetLayout set0, VkPipelineLayout raster_layout, VkShaderModule mesh_vs);
void outline_shutdown(struct VkCtx* vk);
// The entry of a material's / an actor's outline values (the frame's table)
void outline_entry_of(const MatToon* t, OutlineEntry* e);
// Records the pass (the scene's color already resolved in color_img at w x h, the scene's depth bound at set 0's 21 in
// a read-only layout): the id targets drawn, the two compute passes; color_img left in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
void outline_record(struct VkCtx* vk, VkCommandBuffer cmd, uint32_t frame_slot, VkDescriptorSet set0, VkDescriptorSet mat_set,
                    VkPipelineLayout raster_layout, const Push* push, uint32_t w, uint32_t h, GpuImage* color_img,
                    const OutlineDraw* draws, uint32_t ndraws, const OutlineEntry* entries, uint32_t nentries);

typedef struct OutlineStats {
    uint32_t draws, entries, width, height;
    double gpu_ms, gpu_max_ms; // the pass's GPU time (its timestamps; the worst since the start)
    double gpu_avg_ms;         // ... the median of the last 64 frames it ran (the measures)
    double cpu_ms;             // the frame's list (render.c)
    double line_share;         // the line's pixels (coverage over 1/2) over the render's (the test's image measure)
} OutlineStats;
const OutlineStats* outline_stats(void);
void outline_set_cpu_ms(double ms);
void outline_register_debug(void); // the Debug Viewer's "Toon Outline" list (debug_viewer.c)
