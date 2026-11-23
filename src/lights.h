// The lights at scale (notas-dev/luz.md): every local light (point, spot, rect) in a buffer on the GPU, a slot per
// entity like the scene's instances, sent only when it changes (the ECS change log, the selection, the Movable ones
// compared each frame); a world grid of them around the camera rebuilt each frame on the GPU (light_grid.comp), which
// the raster, the probes, the reflections and the path tracer walk (shaders/lights.glsl). The directional and sky
// lights are a short list render.c reads each frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

bool lights_init(struct VkCtx* vk, VkPipelineLayout layout, const uint32_t* grid_code, size_t grid_bytes);
void lights_shutdown(struct VkCtx* vk);
// Room for cap slots (the GPU idle first; every slot empty: a rebuild follows)
bool lights_reserve(struct VkCtx* vk, uint32_t cap);
// Every light sent again (a new level, an undo, the PIE's snapshot)
bool lights_rebuild(struct VkCtx* vk, World* w, bool editor);
// This frame's changes: the change log's slots (hot), the selection, the Movable lights; into the frame slot's staging
bool lights_prepare(struct VkCtx* vk, World* w, bool editor, uint32_t frame_slot, const uint32_t* hot, uint32_t nhot,
                    Entity selected);
// The copies, then the grid (three passes); the frame's data already holds lights_fill_frame's fields
void lights_record(VkCommandBuffer cmd, uint32_t frame_slot, VkPipelineLayout layout, VkDescriptorSet set, Push push);
// The grid around the camera, the buffers, the budget of traced shadows per pixel
void lights_fill_frame(GpuFrame* fr, const float camera[3], uint32_t slots, uint32_t shadow_budget);
// The directional and sky lights' slots (entity indices), in slot order
const uint32_t* lights_globals(uint32_t* n);
// The light of an entity in the GPU's terms (no atmosphere: render.c tints the directional ones); false: none
bool lights_gpu(World* w, Entity e, bool editor, GpuLight* g);
// (2026-11-23) an actor whose material glows enough lights the scene as a point light (g: it; NULL: whether it does)
bool lights_emissive_light(World* w, Entity e, bool editor, GpuLight* g);
// (Q4) a light with a Light Function was sent to the GPU since the start (render.c: the RE_EXT variants read them)
bool lights_function_seen(void);
bool lights_toon_force_seen(void); // (Q11) a Force Toon light was sent (the RE_TOON pipelines from then on)
// (2026-11-16, Lighting Levels' blend) a level's lights times a weight (its CStreamed id; sublevels.c sets it and touches
// the lights): a Lighting Level faded by a timeline dims its own lights with its baked light
void lights_set_level_weight(uint32_t level_id, float w);
float lights_level_weight(World* w, Entity e);
uint32_t lights_local_count(void);
uint32_t lights_cast_count(void); // the local lights casting shadows (the many lights' sampling: render.c)
// The local lights casting shadows: their slots and their lights as last sent (lsm.c picks the shadow maps' among them)
uint32_t lights_cast_list(const uint32_t** slots, const GpuLight** gl);
uint32_t lights_area_count(void); // the local area lights casting shadows (spheres of Source Radius, rectangles)
// The items the grid asked for in the frame that used this slot before (over the capacity: lights were dropped)
uint32_t lights_grid_items(uint32_t frame_slot, uint32_t* capacity);
// A light's color times its intensity as the renderer sees it (the temperature and Affects World applied; cd or lux)
void lights_radiance(const CLight* l, float out[3]);
// Its scales: the GI's share, the highlights', the fog's (1 for a light saved before them)
void lights_scales(const CLight* l, float* indirect, float* specular, float* volumetric);
// The intensity in units u (Details), and back into the stored cd / lux
float lights_to_units(const CLight* l, LightUnits u);
void lights_from_units(CLight* l, LightUnits u, float value);
