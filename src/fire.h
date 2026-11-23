// Fire (A14; notas-dev/particulas.md "Fogo"; shaders/fire.glsl, fire_sim.comp, fire_render.comp): every Fire actor
// (CFire) a gas on a grid of its own on the GPU (Niagara Fluids' Grid 3D Gas): fuel burnt into heat and smoke, rising,
// curling (vorticity confinement), the pressure projected; drawn by marching its grid over the scene (the flame's black
// body light, the smoke lit by the sun through itself and by the sky); the actor's Light flickering with it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;
struct CFire;

bool fire_init(struct VkCtx* vk, VkPipelineLayout layout);
void fire_shutdown(struct VkCtx* vk);
// The frame: the fires of the world (a new one or a resized one starts empty), their table into this frame's buffer,
// their lights' flicker; dt in seconds (0: held), the sun's direction (toward it, world; NULL: none)
void fire_fill(World* w, float dt, const float sun_dir[3], uint32_t slot);
// Before the main pass: each fire's step (7 passes and the pressure's iterations)
void fire_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push);
// After the opaque scene (the color in GENERAL, the depth readable): the fires over it
void fire_render(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t width, uint32_t height);
bool fire_any(void);
void fire_apply_preset(struct CFire* c, int preset);
extern const char* const g_fire_preset_names[];

typedef struct FireStats { uint32_t fires, cells, dispatches; } FireStats;
const FireStats* fire_stats(void);
