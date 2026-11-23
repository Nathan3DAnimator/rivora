// The particles' GPU side (particles.c): what render.c calls
#pragma once
#include <vulkan/vulkan.h>

#include "particles.h"

// --- The renderer's side (render.c calls these) ---
struct VkCtx;
// points: the device draws points over a pixel (the small sprites as one vertex each)
bool particles_gpu_init(struct VkCtx* vk, VkPipelineLayout layout, const uint32_t* sim, size_t sim_bytes, bool points);
void particles_gpu_shutdown(struct VkCtx* vk);
// This frame's emitters into slot's blocks (after particles_update)
void particles_prepare(struct VkCtx* vk, World* w, uint32_t slot);
// The simulation and the cull (compute; push: the frame's, the layout bound)
// (async: on the compute queue, A9.7c: its barriers only the compute and transfer stages; the graphics queue waits by a semaphore)
void particles_record(VkCommandBuffer cmd, uint32_t slot, const void* push, VkPipelineLayout layout, bool async);
uint32_t particles_active(uint32_t slot); // the emitters this frame simulates (0: nothing to record)
// The draws in the main pass: meshes (before the sky; their depth), then sprites (after the sky; blended)
uint32_t particles_draw_meshes(VkCommandBuffer cmd, uint32_t slot, const void* push, VkPipelineLayout layout, VkPipeline pipe,
                               VkBuffer indices, uint64_t vertices);
uint32_t particles_draw_sprites(VkCommandBuffer cmd, uint32_t slot, const void* push, VkPipelineLayout layout, VkPipeline quads, VkPipeline points,
                                VkPipeline ribbons);
uint32_t particles_ribbon_segments(void); // (B10) the ribbons' quads drawn last frame
