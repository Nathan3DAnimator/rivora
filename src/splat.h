// Gaussian Splatting (A9.5d; notas-dev/render.md "Gaussian Splatting"; shaders/splat.glsl, splat_sort.comp, splat.vert,
// splat.frag): a Splat actor (CSplat) shows a scene scanned as 3D Gaussians (3DGS .ply, .splat), loaded once into a
// device buffer; each frame its Gaussians culled and sorted back to front on the GPU (a counting sort, 3 dispatches)
// and drawn over the scene, after the opaque meshes and before the translucent ones.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

bool splat_init(struct VkCtx* vk, VkPipelineLayout layout);
void splat_shutdown(struct VkCtx* vk);
// The frame: the Splat actors (a new file loaded), their table into this frame's buffer
void splat_fill(World* w, uint32_t slot);
// Before the main pass: the cull and the sort (the frame's camera)
void splat_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push);
// In a pass over the color with the scene's depth: the Gaussians back to front (an indirect draw)
uint32_t splat_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe);
bool splat_any(void);
// The examples (Content/Splats: a bush as .splat, a flower bed as a 3DGS .ply), made when missing; false: not made
bool splat_examples(void);

typedef struct SplatStats { uint32_t actors, gaussians; double load_ms; } SplatStats;
const SplatStats* splat_stats(void);
