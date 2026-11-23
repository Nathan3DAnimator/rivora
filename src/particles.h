// Particles (notas-dev/particulas.md): two systems over one GPU simulation.
//   - Particle System (.rparticle, Unreal's Niagara): emitters, each a stack of modules in Niagara's stages (Emitter
//     Spawn / Update, Particle Spawn, Particle Update, Render); an actor plays it (CParticleSystem); Blueprint nodes
//     spawn, activate and tint them.
//   - Tlender Particles (CParticleTl, Blender's Particle Properties): on a mesh actor, emitted from its faces, vertices
//     or volume, count over a frame range, Newtonian physics, rendered as halos (sprites) or an object (a mesh).
// Both compile into GpuEmitter blocks (render_gpu.h) for shaders/particle_sim.comp, particle_cull.comp and the
// sprite / mesh draws. The births are deterministic (particles.glsl p_birth: slot i born at a time from i alone): no
// free list, no atomics to spawn, millions per emitter at a few tenths of a millisecond.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define PS_MAX_EMITTERS 8
#define PS_MAX_MODULES 24
#define PS_MAX_PARAMS 18
#define PS_NAME 48

typedef enum PsStage { PSS_EMITTER = 0, PSS_SPAWN, PSS_UPDATE, PSS_RENDER, PSS_COUNT } PsStage;

typedef enum PsModuleKind {
    PM_SPAWN_RATE = 0, PM_SPAWN_BURST, PM_LIFETIME, PM_SHAPE, PM_INIT_VELOCITY, PM_INIT_SIZE, PM_INIT_COLOR, PM_INIT_ROTATION,
    PM_GRAVITY, PM_DRAG, PM_CURL, PM_BROWNIAN, PM_VORTEX, PM_ATTRACT, PM_WIND, PM_COLLISION, PM_SPEED_LIMIT, PM_KILL_BOX,
    PM_COLOR_LIFE, PM_SIZE_LIFE, PM_SPRITE, PM_MESH,
    PM_EMITTER_STATE, PM_SUBUV, PM_SCALE_SPEED, PM_CAMERA_OFFSET, // (2026-11-08: Niagara's Emitter State, Sub UV Animation...)
    PM_RIBBON, // (B10) Niagara's Ribbon Renderer: a strip through the particles in their birth order (trails, beams, magic)
    PM_COUNT
} PsModuleKind;

typedef struct PsModuleInfo {
    const char* name;      // Niagara's ("Spawn Rate", "Curl Noise Force"...)
    uint8_t stage;         // PsStage
    uint8_t nparams;
    uint8_t asset;         // 1: a material (its texture), 2: a mesh
    const char* params[PS_MAX_PARAMS];
    float def[PS_MAX_PARAMS], lo[PS_MAX_PARAMS], hi[PS_MAX_PARAMS];
    const char* tip;
} PsModuleInfo;
const PsModuleInfo* ps_module_info(int kind);
int ps_module_kind(const char* name); // -1: none

typedef struct PsModule {
    uint8_t kind, on, pad[2];
    float p[PS_MAX_PARAMS];
    uint32_t asset;        // a material's hash (a sprite's texture) or a mesh's id (the mesh renderer)
} PsModule;

typedef struct PsEmitter {
    char name[PS_NAME];
    uint8_t on, local, pad[2];
    uint32_t seed, max_particles; // max: 0 = from the rate, the life and the bursts
    uint32_t nmodules;
    PsModule m[PS_MAX_MODULES];
} PsEmitter;

typedef struct ParticleSystemAsset {
    char name[PS_NAME], path[512];
    uint32_t hash, version; // version: bumped on every change (the runtime resets its emitters)
    uint32_t nemitters;
    PsEmitter e[PS_MAX_EMITTERS];
    bool dirty;
} ParticleSystemAsset;

// The Content folder (the editor's and the game's); the assets are found by their vpath's hash
void particles_set_root(const char* content_root);
ParticleSystemAsset* ps_load(const char* vpath);
ParticleSystemAsset* ps_find_hash(uint32_t hash);
bool ps_save(ParticleSystemAsset* a);
// A new asset from a template (Content Browser > Add > Visual Effects): its vpath's hash, 0 on failure
typedef enum PsTemplate { PST_EMPTY = 0, PST_FIRE, PST_SMOKE, PST_SPARKS, PST_RAIN, PST_SNOW, PST_FOUNTAIN, PST_MAGIC, PST_EXPLOSION,
                          PST_DUST, PST_DEBRIS, PST_GALAXY, PST_RIBBON, PST_COUNT } PsTemplate;
extern const char* const g_ps_template_names[PST_COUNT];
uint32_t ps_create(const char* folder_vpath, const char* name, int tmpl);
void ps_template(ParticleSystemAsset* a, int tmpl);       // (fills one)
PsModule* ps_add_module(PsEmitter* e, int kind);          // its defaults; NULL when full
void ps_remove_module(PsEmitter* e, uint32_t i);
void ps_touch(ParticleSystemAsset* a);                     // changed: saved by the editor, reset by the runtime
uint32_t ps_asset_count(void);
ParticleSystemAsset* ps_asset_at(uint32_t i);
void ps_rescan(void);

// The Tlender particles' enums (CParticleTl)
enum { PTL_FROM_VERTS = 0, PTL_FROM_FACES, PTL_FROM_VOLUME };
enum { PTL_RENDER_HALO = 0, PTL_RENDER_OBJECT };
enum { PTL_PHYS_NONE = 0, PTL_PHYS_NEWTON };
void particles_tl_defaults(struct CParticleTl* c);
void particles_system_defaults(struct CParticleSystem* c);

// --- The runtime (particles.c) ---
// Every frame: the components' emitters, their clocks (editor: always playing, looping; game: as activated)
void particles_update(World* w, float dt, bool playing);
// Blueprint: a system spawned at a place (an actor with CParticleSystem; auto destroyed when done), activate, tint
Entity particles_spawn(World* w, uint32_t asset, const float loc[3], const float rot[3], bool auto_destroy);
void particles_activate(World* w, Entity e, bool on, bool reset);
// The stats: emitters, their particles (capacity), last frame's visible sprites and meshes
typedef struct ParticleStats {
    uint32_t emitters, capacity, sprites, meshes;
    double cpu_ms;
} ParticleStats;
const ParticleStats* particles_stats(void);
// The self-test (--particles-test): births, the asset's round trip, the templates. 0 = ok
int particles_selftest(void);

// An emitter compiled as for the GPU (render_gpu.h GpuEmitter), for the editor's CPU preview; its end time (0: never)
struct GpuEmitter;
void ps_compile_cpu(const PsEmitter* pe, struct GpuEmitter* g);
float ps_end_time(const struct GpuEmitter* g);

// The renderer's side: particles_gpu.h
bool particles_any(void);
