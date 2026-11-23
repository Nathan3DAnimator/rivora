// Build Lighting (notas-dev/gi.md, layer 1): the Static meshes get a rectangle of a lightmap atlas (~10 cm per texel),
// their UV2 rasterized on the CPU into a G-buffer (position, normal per texel), and the GPU traces their hemisphere
// frame after frame (shaders/lightmap_bake.comp), shown in the viewport while it converges. The result is saved next
// to the level (<level>.rlight) per entity, with a hash of its transform: a Static mesh moved after the bake falls
// back to the probes until the next build.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

typedef enum BakeQuality { BAKE_PREVIEW = 0, BAKE_PRODUCTION } BakeQuality;

// (Q16, notas-dev/em-jogo.md) where a bake runs: the editor's Build Lighting (one sweep of the atlas a frame, the
// lightmaps shown converging); the game behind its loading screen (as many sweeps as its budget, 30 ms: the game waits);
// the game while it plays (a band of the atlas's rows in the frame's budget, 1 ms; the probes in turns; the lightmaps
// hidden until it ends: the dynamic GI lights the level meanwhile)
typedef enum GiBakeMode { GI_BAKE_EDITOR = 0, GI_BAKE_BLOCKING, GI_BAKE_PROGRESSIVE } GiBakeMode;
typedef struct GiBakeStats {
    GiBakeMode mode;
    bool baking;
    uint32_t atlas, rows_per_frame, frames;
    float budget_ms;
    double start_ms;           // the CPU's start (the charts, the G-buffer, the GPU's buffers)
    double gpu_avg_ms, gpu_max_ms; // the bake's own share of a frame (its timestamps)
    double seconds;            // since it started
} GiBakeStats;

bool gi_bake_init(struct VkCtx* vk, VkPipelineLayout layout);
void gi_bake_shutdown(struct VkCtx* vk);
// Starts a bake of the world's Static meshes. False with a reason (no ray tracing, no Static mesh).
bool gi_bake_start(struct VkCtx* vk, World* w, BakeQuality q, char* why, size_t why_cap);
bool gi_bake_start_ex(struct VkCtx* vk, World* w, BakeQuality q, GiBakeMode mode, float budget_ms, char* why, size_t why_cap);
GiBakeMode gi_bake_mode(void);
void gi_bake_stats(GiBakeStats* s);
void gi_bake_note_gpu(double ms, uint32_t rows);
void gi_bake_frame(struct VkCtx* vk); // (the host before the render: a bake while playing taken over from its thread) // (render.c: the bake's timestamps of a frame and the rows it did)
void gi_bake_cancel(void);
bool gi_baking(void);
float gi_bake_progress(void);      // 0..1
bool gi_bake_take_finished(void);  // true once, when a bake just ended
// Records this frame's share of the bake (after the TLAS and the probes; render.c's set and push); the atlas's rows done
uint32_t gi_bake_step(VkCommandBuffer cmd, VkDescriptorSet set, Push push);
// The lightmap rectangle of a Static mesh whose transform still matches the bake (false: no lightmap)
// (found by its entity, else by its transform and mesh asset: a sublevel's actor reopened has another handle)
bool gi_lightmap_for(Entity e, const CTransform* t, uint32_t mesh_slot, float st[4]);
uint64_t gi_transform_hash(const CTransform* t, uint32_t mesh);
uint64_t gi_stable_hash(const CTransform* t, uint32_t mesh_slot); // (what a bake keeps: the mesh's asset id, not its GPU slot)
bool gi_lightmaps_present(void);
bool gi_lightmaps_shown(void); // present and not hidden (a bake in the game shows its lightmaps once it ends)
// <level>.rlight: save after a bake (or with the level), load with the level, clear for a new one
bool gi_lightmaps_save(struct VkCtx* vk, const char* path);
bool gi_lightmaps_load(struct VkCtx* vk, const char* path);
void gi_lightmaps_clear(struct VkCtx* vk);
// (2026-11-16, Lighting Levels: sublevels.c) several .rlight added into what is shown: the light is linear, so the bakes
// of separate groups of lights sum to the bake of them all. The first file's atlas and entries; the others must have
// the same (the same Static meshes baked in one Build Lighting) or they are left out. L0 summed, the direction weighted
// by each one's luminance, the walls the first's, the probe volume's irradiance summed. n == 1: gi_lightmaps_load
bool gi_lightmaps_load_sum(struct VkCtx* vk, const char* const* paths, int n);
// (2026-11-16) the same files as layers kept on the GPU and blended by weight (lightmap_blend.comp): layer_of[i] = the
// layer of paths[i] (-1: left out); weights[i] its start (NULL: 1). Returns the layers (1: nothing to blend)
#define GI_LM_LAYERS 33
int gi_lightmaps_load_layers(struct VkCtx* vk, const char* const* paths, int n, const float* weights, int* layer_of);
int gi_lightmaps_layers(void);
void gi_lightmaps_layer_weight(int layer, float w); // the blend made again on the GPU next frame (no file, no CPU sum)
struct Push;
void gi_lightmaps_blend_step(VkCommandBuffer cmd, VkDescriptorSet set, struct Push push); // render.c, each frame
