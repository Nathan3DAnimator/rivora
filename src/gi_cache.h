// The surface cache (notas-dev/gi.md, "Cache de superfície"): real-time radiosity on the surfaces near the camera.
// Each mesh within reach gets a chart of a texel atlas (its UV2, ~25 cm per texel on High), each texel a point of the
// surface in mesh space; shaders/gi_cache.comp updates a share of them each frame (exact direct light, a few bounce
// rays that read the cache where they hit: multiple bounces, no light through closed walls), and the main pass reads
// the indirect light of a pixel from its own surface's texels. Ray tracing only; elsewhere the probes stay.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "gi.h"
#include "render_gpu.h"

struct VkCtx;

bool gi_cache_init(struct VkCtx* vk, VkPipelineLayout layout);
void gi_cache_shutdown(struct VkCtx* vk);
VkImageView gi_cache_view(int which); // 0: direct, 1: indirect (1x1 stand-ins without ray tracing)
// The charts around the camera, kept while they fit: reallocated when the scene's meshes near it change or the
// camera went far (the kept charts' light is copied over: no flash). CPU, before the frame is recorded.
void gi_cache_update(struct VkCtx* vk, World* w, const float camera[3], const GiSettings* s);
// The imported meshes changed (render_meshes_reload): their chart templates are made again, the charts reallocated
void gi_cache_meshes_changed(void);
// The frame's fields (GI_SURFACE_CACHE is render.c's, with the TLAS)
void gi_cache_fill_frame(GpuFrame* fr, uint32_t frame_index);
bool gi_cache_active(void);
// The reallocation's uploads and copies, then this frame's share of the texels (after the probes, before the main
// pass; render.c's set and push)
void gi_cache_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr);

typedef struct CacheTexelGpu { // shaders/gi_cache.comp CacheTexel
    float lpos[3];
    uint32_t slot;
    float lnrm[3];
    uint32_t xy; // the atlas texel: x | y << 16
    float ldu[3], pad0; // mesh space: one texel along the atlas x, along y (the walls' test: gi_cache.comp)
    float ldv[3], pad1;
} CacheTexelGpu;
#define GI_CACHE_PAD 1 // texels around a chart (its texels' xy include it)

// A shape's chart at scale s and a texel size (cm): its sides (powers of two, 8 .. 512; a wall gets a long chart with
// square texels) and its texels in mesh space, rasterized once per shape and size (the lightmap bake shares them)
const CacheTexelGpu* gi_cache_chart(uint32_t shape, const float s[3], float texel, uint32_t* w, uint32_t* h, uint32_t* count);

typedef struct GiCacheStats {
    uint32_t charts, texels, per_frame, reallocations;
    uint32_t dynamic; // the texels of the actors moving right now, redone every frame
    float texel_cm;
    uint32_t candidates;      // the meshes within reach that may have a chart (charts: those that got one)
    float falloff_cm, coarse; // the density the allocation reached (gi_cache.c allocate)
    uint32_t quiet;           // (2026-11-25) frames with nothing changed (settled: a quarter of the share a frame)
    bool light_short;         // (2026-11-25) a local light changed lately: the short running mean (6 updates, not 512)
} GiCacheStats;
const GiCacheStats* gi_cache_stats(void);
// The frames one pass over all the texels takes (a share of them each frame): an event (render.c) lasts two of them
uint32_t gi_cache_cycle_frames(void);
