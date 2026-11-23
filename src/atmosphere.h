// The physical sky (notas-dev/luz.md): Earth's atmosphere (Hillaire 2020) in three LUTs made on the GPU by
// shaders/atmosphere.comp, sampled by every shader that sees the sky (shaders/atmosphere.glsl), and the sky light's
// SH computed from them. The LUTs are for a sun of 1 lux: the frame's sun_illuminance scales them.
#pragma once
#include <stdbool.h>

#include "render_gpu.h"

struct VkCtx;

bool atmosphere_init(struct VkCtx* vk, VkPipelineLayout layout);
void atmosphere_shutdown(struct VkCtx* vk);
// The descriptor set's images (render.c): 0 transmittance, 1 sky view, 2 multiple scattering, 3 aerial perspective
VkImageView atmosphere_view(int i);
VkDeviceAddress atmosphere_sky_address(void); // SkyBuf: the sky light's SH (for 1 lux of sun)
// The LUTs that changed (the first frame: all; then the sky view and the SH when the sun turned or the camera's
// altitude changed) and, with aerial, the aerial perspective's froxels (every frame), recorded in cmd before anything
// samples them; returns true when it recorded work
bool atmosphere_update(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const float sun[3], float camera_z_cm,
                       bool aerial);
// The sun's light left after the atmosphere, seen from the camera's altitude toward dir (the same model, on the CPU):
// the directional lights' illuminance at the ground is their lux times this
void atmosphere_transmittance(const float dir[3], float camera_z_cm, float out[3]);
