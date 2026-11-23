// Clouds and fog (notas-dev/luz.md, "Céu, nuvens e neblina"): the Volumetric Cloud and Exponential Height Fog actors
// on the GPU (shaders/weather.comp, weather.glsl). Nothing per pixel carries history: no ghosting.
#pragma once
#include <stdbool.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

bool weather_init(struct VkCtx* vk, VkPipelineLayout layout);
void weather_shutdown(struct VkCtx* vk);
// The descriptor set's images: 0 the cloud panorama, 1 the clouds' shadow, 2 the fog, 3 the fog's froxel light, 4 noise
VkImageView weather_view(int i);
// The first fog and cloud actors into the frame (none: off); volumetric: the fog's froxels (else the analytic fog)
void weather_fill_frame(GpuFrame* fr, World* w, bool volumetric);
// The frame's passes (after the light grid and the TLAS, before anything that shades): the clouds' panorama rows, their
// shadow and the sky light with them
void weather_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr);
// The fog's froxels and their integration: after the sun's virtual shadow maps of this frame (the shafts read them)
void weather_record_fog(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr);
