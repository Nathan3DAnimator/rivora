// Water (notas-dev/agua.md; shaders/water.glsl, water_sim.comp, water_surface.*, rain.*): the rain and the water bodies.
//   - Rain (a Rain actor, CRain): drops and splashes drawn from hashes around the camera (no state); the first surface
//     the rain meets traced from above into a map that follows the camera (a few rows a frame); on that map a shallow
//     water simulation (virtual pipes, Mei et al. 2007): the rain adds water, it runs downhill, gathers in puddles and,
//     heavy enough, in streams; the ground drinks it and the air dries it. The surfaces read the map: wet (darker,
//     glossier), streaks down the walls, open water in puddles and streams (ripples, the flow's waves). Under a roof:
//     dry.
//   - Water bodies (CWater): drawn as water (no more a plane). A lake or pool: its two wave trains (the ones the boats
//     feel). An ocean: endless, Tessendorf's FFT waves from a JONSWAP spectrum of the wind (three cascades of 256^2, 400 m,
//     60 m and 8 m), choppy crests, foam where they fold; the boats feel its biggest waves (vehicle_sim.inc).
//   - What moves in water (vehicles, characters, physics bodies) makes ripples and wakes: a wave equation on a grid
//     around the camera.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

bool water_init(struct VkCtx* vk, VkPipelineLayout layout);
void water_shutdown(struct VkCtx* vk);
// The frame's numbers (after the camera is in fr): the Rain and the water bodies of the world; dt in seconds
void water_fill_frame(GpuFrame* fr, World* w, float dt, bool editor);
// The compute passes (after the TLAS, before the main pass)
void water_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr);
// The main pass: the water bodies' surfaces (opaque, with their depth: before the sky), then the rain's drops and splashes
// (blended: after the sky). Their draws
uint32_t water_draw_surfaces(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe);
uint32_t water_draw_rain(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe);
// The ocean's height at x, y (cm): its biggest waves (the boats float on them); false: no ocean
bool water_ocean_height(float x, float y, float* z);
bool water_excluded(float x, float y); // (2026-11-10) inside a Water Body with Exclusion on (no ocean there)
// The rain now (mm/h; 0: none) and the wetness the map holds at a place (0..1; Blueprint Get Wetness)
float water_rain_intensity(void);
// (Q27.6) the persistent wet map (16 m cells, 4 km around the camera): a place's water (cm) and wetness read back a few
// frames after the probe is set (false until then, or outside the map); skip: the places away from the camera evolved
// that many seconds more on the next frame (a test's "10 minutes later")
#define PERSIST_N 256
#define PERSIST_CELL 1600.0f
void water_persist_probe(float x, float y);
bool water_persist_read(float out[2]);
void water_persist_skip(float seconds);
void water_set_rain(World* w, float intensity); // (Blueprint Set Rain Intensity: every Rain actor)

// (2026-11-14) every Rain field by name (Details' labels, spaces or not, any case: "Wind Speed", "WindSpeed", "Thunder"):
// Blueprint Set / Get Rain Property, the scripts' API. A uint field takes the rounded value. The table also lists each
// field's range (the Details' too)
typedef struct RainProp { const char* name; uint16_t offset; uint8_t is_uint; float lo, hi; } RainProp;
extern const RainProp g_rain_props[];
extern const uint32_t g_rain_prop_count;
const RainProp* rain_prop_find(const char* name);
bool water_set_rain_prop(World* w, const char* name, float value); // every Rain actor; false: no such field
float water_get_rain_prop(World* w, const char* name);             // the first Rain actor's (0: none)
// The presets (RainPreset): Drizzle, Light, Moderate, Heavy, Storm, Monsoon, Stylized: the look and the sound together
extern const char* const g_rain_preset_names[RAIN_PRESET_COUNT];
extern const char* const g_rain_sound_names[RAIN_SOUND_COUNT];
void rain_apply_preset(CRain* r, uint32_t preset);
// (2026-11-15) a shading style's defaults (the shading fields only; Stylized also changes the forms in the shaders)
void rain_apply_shading_style(CRain* r, uint32_t style);
extern const char* const g_rain_shading_names[RAIN_SHADE_COUNT];
bool water_set_rain_shading_style(World* w, const char* name_or_index); // (Blueprint Set Rain Shading Style)
// (2026-11-15) the rain's haze in the air: extra extinction (1/m) for the height fog (weather.c); 0 when dry
float water_rain_mist(void);
bool water_set_rain_preset(World* w, const char* name_or_index); // (Blueprint Set Rain Preset: every Rain actor)
// (2026-11-14, "objetos molhados secam muito rápido ao sair da chuva, e molham também muito rápido") what moves (Characters,
// Skeletal Meshes, vehicles, physics bodies) keeps its own wetness: soaking over the Rain's Actor Wet Time while the rain
// reaches it (nothing over it in the rain's map), drying over Actor Dry Time under a roof or after the rain. 0..1; -1:
// not followed (too far, or no rain)
float water_actor_wetness(Entity e);
void water_set_actor_wetness(Entity e, float wet); // (Blueprint Set Actor Wetness: a soaked character out of a lake)
float water_rain_shelter(void); // 0 the camera under the open sky .. 1 under a roof (the rain's sound is muffled)
float water_lightning(void);    // this frame's flash (0..1): the thunder's (audio_world.c strikes it)
void water_strike_lightning(float strength); // a flash now (the thunder's sound follows by its distance: audio_world.c)

typedef struct WaterStats {
    bool rain, ocean;
    uint32_t drops, splashes, cells, lakes;
    uint32_t footprints; // the feet's prints stamped in the snow so far (in the rain: the wet prints and drips)
    uint32_t wet_followed; // (2026-11-15) the moving actors the rain follows now, and the wettest of them
    float wet_max;
    double cpu_ms;
} WaterStats;
const WaterStats* water_stats(void);
int water_selftest(void); // the spectrum (its waves' height for a wind) and the shallow water's balance on the CPU; 0 = ok
