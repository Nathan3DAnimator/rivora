// Particles (particles.h, notas-dev/particulas.md): the Particle System assets (Niagara's emitters and modules), the
// Tlender's particle systems on mesh actors, their runtime (clocks, pools) and their GPU work.
#include "particles_gpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "components.h"
#include "io_json.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "render_gpu.h"
#include "vkctx.h"

#define MAX_ASSETS 256
#define MAX_INST 1024u          // emitters playing at once
#define ARGS_STRIDE 64u         // quads (16 bytes), meshes (20, at 16), points (16, at 36), padding
#define POOL_MAX (32u << 20)    // particles in all (32 M: 1 GB of pool)
#define SHAPE_TRIS_MAX 65536u   // a mesh's triangles sampled for its emission (by stride past it)

// --- The modules (Niagara's names; the parameters' meaning: particles.glsl Emitter) ---
#define P(n) n
static const PsModuleInfo g_modules[PM_COUNT] = {
    [PM_SPAWN_RATE] = { "Spawn Rate", PSS_EMITTER, 2, 0, { "Rate (/s)", "Duration (s, 0: forever)" }, { 100, 0 }, { 0, 0 }, { 1e7f, 1e5f },
                        "Particles born each second, for a duration (0: as long as it plays)." },
    [PM_SPAWN_BURST] = { "Spawn Burst", PSS_EMITTER, 3, 0, { "Count", "Time (s)", "Interval (s, 0: once)" }, { 50, 0, 0 }, { 0, 0, 0 },
                         { 1e7f, 1e5f, 1e5f }, "Particles born at once at a time, again each interval." },
    [PM_LIFETIME] = { "Lifetime", PSS_SPAWN, 2, 0, { "Min (s)", "Max (s)" }, { 1.5f, 2.5f }, { 0.01f, 0.01f }, { 1e4f, 1e4f },
                      "How long each particle lives (a random time between Min and Max)." },
    [PM_SHAPE] = { "Shape Location", PSS_SPAWN, 8, 0,
                   { "Shape (0 point, 1 sphere, 2 box, 3 cone, 4 disc, 5 mesh, 6 cylinder, 7 torus)", "Radius", "Size X", "Size Y", "Size Z",
                     "Surface Only", "Cone Angle", "Cone Length", "Minor Radius" },
                   { 0, 50, 100, 100, 100, 0, 25, 0, 10 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 7, 1e6f, 1e6f, 1e6f, 1e6f, 1, 180, 1e6f, 1e6f },
                   "Where the particles are born around the emitter (a mesh: the actor's own Static Mesh, its faces; a cylinder: Radius and "
                   "Size Z; a torus: Radius and Minor Radius)." },
    [PM_INIT_VELOCITY] = { "Initial Velocity", PSS_SPAWN, 7, 0,
                           { "Direction X", "Direction Y", "Direction Z", "Speed (cm/s)", "Cone Angle", "Speed Random", "Outward Speed" },
                           { 0, 0, 1, 300, 15, 0.3f, 0 }, { -1, -1, -1, 0, 0, 0, -1e5f }, { 1, 1, 1, 1e6f, 180, 1, 1e5f },
                           "The speed at birth: along a direction, spread in a cone, and away from the shape." },
    [PM_INIT_SIZE] = { "Initial Size", PSS_SPAWN, 2, 0, { "Min (cm)", "Max (cm)" }, { 5, 10 }, { 0, 0 }, { 1e5f, 1e5f },
                       "Each particle's size (a random one between Min and Max)." },
    [PM_INIT_COLOR] = { "Initial Color", PSS_SPAWN, 8, 0, { "R", "G", "B", "Alpha", "R (2)", "G (2)", "B (2)", "Alpha (2)" },
                        { 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 100, 100, 100, 1, 100, 100, 100, 1 },
                        "Each particle's color: a random mix of the two (linear, alpha its opacity)." },
    [PM_INIT_ROTATION] = { "Initial Rotation", PSS_SPAWN, 2, 0, { "Random (deg)", "Rate (deg/s)" }, { 180, 0 }, { 0, -1e4f }, { 180, 1e4f },
                           "Each particle's turn at birth (random up to this) and its spin." },
    [PM_GRAVITY] = { "Gravity Force", PSS_UPDATE, 3, 0, { "X", "Y", "Z" }, { 0, 0, -980 }, { -1e5f, -1e5f, -1e5f }, { 1e5f, 1e5f, 1e5f },
                     "A constant acceleration (cm/s^2): the Earth's is -980 on Z." },
    [PM_DRAG] = { "Drag", PSS_UPDATE, 1, 0, { "Drag (1/s)" }, { 1 }, { 0 }, { 100 }, "Slows the particles down (air)." },
    [PM_CURL] = { "Curl Noise Force", PSS_UPDATE, 3, 0, { "Strength", "Scale (cm)", "Speed" }, { 200, 100, 1 }, { 0, 1, 0 }, { 1e5f, 1e5f, 100 },
                  "Swirls without sources (smoke, magic): a divergence-free field moving in time." },
    [PM_BROWNIAN] = { "Turbulence (Brownian)", PSS_UPDATE, 1, 0, { "Strength" }, { 100 }, { 0 }, { 1e5f }, "Random jitter every frame." },
    [PM_VORTEX] = { "Vortex Force", PSS_UPDATE, 5, 0, { "Rate (deg/s)", "Pull", "Axis X", "Axis Y", "Axis Z" }, { 90, 0, 0, 0, 1 },
                    { -1e4f, -1e5f, -1, -1, -1 }, { 1e4f, 1e5f, 1, 1, 1 }, "Turns the particles around an axis through the emitter." },
    [PM_ATTRACT] = { "Point Attraction Force", PSS_UPDATE, 6, 0, { "X", "Y", "Z", "Strength", "Radius (0: all)", "Kill Radius" },
                     { 0, 0, 200, 500, 0, 0 }, { -1e6f, -1e6f, -1e6f, -1e6f, 0, 0 }, { 1e6f, 1e6f, 1e6f, 1e6f, 1e6f, 1e6f },
                     "Pulls the particles toward a point of the emitter (gone when they reach the kill radius)." },
    [PM_WIND] = { "Wind Force", PSS_UPDATE, 4, 0, { "X (cm/s)", "Y (cm/s)", "Z (cm/s)", "Response" }, { 300, 0, 0, 1 }, { -1e5f, -1e5f, -1e5f, 0 },
                  { 1e5f, 1e5f, 1e5f, 100 }, "Carries the particles toward the wind's speed." },
    [PM_COLLISION] = { "Collision", PSS_UPDATE, 4, 0, { "Ground Height (cm)", "Bounce", "Friction", "Die On Contact" }, { 0, 0.4f, 0.2f, 0 },
                       { -1e6f, 0, 0, 0 }, { 1e6f, 1, 1, 1 }, "The particles bounce on the ground (or die there)." },
    [PM_SPEED_LIMIT] = { "Speed Limit", PSS_UPDATE, 2, 0, { "Max (cm/s)", "Min (cm/s)" }, { 1000, 0 }, { 0, 0 }, { 1e6f, 1e6f },
                         "Keeps the speeds between Min and Max." },
    [PM_KILL_BOX] = { "Kill Particles in Box", PSS_UPDATE, 7, 0, { "Min X", "Min Y", "Min Z", "Max X", "Max Y", "Max Z", "Kill Inside" },
                      { -500, -500, -100, 500, 500, 1000, 0 }, { -1e7f, -1e7f, -1e7f, -1e7f, -1e7f, -1e7f, 0 },
                      { 1e7f, 1e7f, 1e7f, 1e7f, 1e7f, 1e7f, 1 }, "Particles leaving (or entering) a box of the world die." },
    [PM_COLOR_LIFE] = { "Color Over Life", PSS_UPDATE, 18, 0,
                        { "R 0", "G 0", "B 0", "A 0", "R 1", "G 1", "B 1", "A 1", "R 2", "G 2", "B 2", "A 2", "R 3", "G 3", "B 3", "A 3", "Time 1",
                          "Time 2" },
                        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.6f, 1, 1, 1, 0, 0.33f, 0.66f },
                        { 0 }, { 100, 100, 100, 1, 100, 100, 100, 1, 100, 100, 100, 1, 100, 100, 100, 1, 1, 1 },
                        "The color along the life: four stops (birth, two middle ones at their times, death)." },
    [PM_SIZE_LIFE] = { "Size Over Life", PSS_UPDATE, 4, 0, { "Birth", "1/3", "2/3", "Death" }, { 1, 1, 1, 0.5f }, { 0, 0, 0, 0 },
                       { 100, 100, 100, 100 }, "The size along the life (a factor)." },
    [PM_SPRITE] = { "Sprite Renderer", PSS_RENDER, 8, 1,
                    { "Shape (0 soft, 1 disc, 2 square, 3 texture)", "Softness", "Lit", "Additive", "Glow (nits)", "Opacity", "Velocity Aligned",
                      "Stretch" },
                    { 0, 1.5f, 0, 1, 2000, 1, 0, 0.02f }, { 0, 0.05f, 0, 0, 0, 0, 0, 0 }, { 3, 16, 1, 1, 1e6f, 1, 1, 1 },
                    "Draws each particle as a quad facing the camera (lit like smoke, or glowing and added like fire)." },
    [PM_MESH] = { "Mesh Renderer", PSS_RENDER, 8, 2,
                  { "R", "G", "B", "Metallic", "Roughness", "Glow (nits)", "Sprite Under (px)", "Receive Shadows" },
                  { 0.8f, 0.8f, 0.8f, 0, 0.6f, 0, 3, 1 }, { 0, 0, 0, 0, 0.05f, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1e6f, 64, 1 },
                  "Draws a mesh at each particle, lit by the sun and the sky; small on screen it becomes a sprite of its color." },
    [PM_EMITTER_STATE] = { "Emitter State", PSS_EMITTER, 4, 0,
                           { "Loop Behavior (0 infinite, 1 once, 2 multiple)", "Loop Duration (s)", "Loop Count", "Start Delay (s)" },
                           { 0, 2, 3, 0 }, { 0, 0.01f, 1, 0 }, { 2, 1e5f, 1e4f, 1e5f },
                           "How the emitter plays: forever, once or a number of loops of a duration, after a delay. A burst with no "
                           "interval fires again at every loop." },
    [PM_SUBUV] = { "Sub UV Animation", PSS_UPDATE, 5, 0,
                   { "Columns", "Rows", "Mode (0 over life, 1 frame rate, 2 random)", "Frame Rate (fps)", "Blend Frames" },
                   { 4, 4, 0, 16, 1 }, { 1, 1, 0, 0, 0 }, { 64, 64, 2, 240, 1 },
                   "Plays a flipbook: the Sprite Renderer's texture cut into a grid of frames, over the life, at a frame rate or one "
                   "at random (fire, smoke, explosions)." },
    [PM_SCALE_SPEED] = { "Scale by Speed", PSS_UPDATE, 6, 0,
                         { "Speed Min (cm/s)", "Speed Max (cm/s)", "Size at Min", "Size at Max", "Alpha at Min", "Alpha at Max" },
                         { 0, 500, 0.5f, 1.5f, 1, 1 }, { 0, 0, 0, 0, 0, 0 }, { 1e6f, 1e6f, 100, 100, 1, 1 },
                         "Size and opacity by how fast each particle goes (sparks that shrink as they slow down)." },
    [PM_CAMERA_OFFSET] = { "Camera Offset", PSS_UPDATE, 1, 0, { "Offset (cm)" }, { 20 }, { -1e5f }, { 1e5f },
                           "Pushes the particles toward the camera (sprites that would cut into a wall or the floor)." },
    [PM_RIBBON] = { "Ribbon Renderer", PSS_RENDER, 7, 1,
                    { "Width Scale", "UV Repeat (particles)", "Tail Fade", "Lit", "Additive", "Glow (nits)", "Opacity" },
                    { 1, 20, 1, 0, 1, 4000, 1 }, { 0.01f, 1, 0, 0, 0, 0, 0 }, { 100, 1e5f, 1, 1, 1, 1e6f, 1 },
                    "Draws a strip through the particles one after the other (each birth joined to the next), its width their size: trails, beams, ribbons of magic. A material's texture runs along it." },
};
#undef P

const PsModuleInfo* ps_module_info(int kind) { return kind >= 0 && kind < PM_COUNT ? &g_modules[kind] : NULL; }
int ps_module_kind(const char* name) {
    for (int k = 0; k < PM_COUNT; k++)
        if (!strcmp(g_modules[k].name, name)) return k;
    return -1;
}

const char* const g_ps_template_names[PST_COUNT] = { "Empty", "Fire", "Smoke", "Sparks", "Rain", "Snow", "Fountain", "Magic", "Explosion",
                                                     "Dust", "Debris", "Galaxy (5 M)", "Ribbon Trail" };

// --- State ---
typedef struct PartInst {
    Entity e;
    uint32_t key;          // its emitter's index (0xFFFF: the Tlender's)
    uint32_t version;      // what it was compiled from (reset on a change)
    uint32_t base, capacity;
    float time, stop, end_time; // end_time: when its last particle dies (finite ones), 0: never
    uint32_t seen;         // last frame seen (gone: freed)
    uint8_t active;
    GpuEmitter g;          // compiled (its clock and place set each frame)
} PartInst;

typedef struct ShapeEntry { uint32_t mesh, first, count; float center[3]; } ShapeEntry;

static struct {
    uint32_t ribbon_segments; // (B10) the ribbons' quads drawn this frame
    char root[600];
    bool scanned;
    ParticleSystemAsset* assets[MAX_ASSETS];
    uint32_t nassets;
    PartInst inst[MAX_INST];
    uint32_t ninst, frame;
    ParticleStats stats;
    // GPU
    VkCtx* vk;
    VkPipeline sim; // the simulation and the cull in one pass (particle_sim.comp)
    GpuBuf pool, list_s, list_m, list_p, args, shape;
    bool points;
    GpuBuf blocks[RE_FRAMES_IN_FLIGHT], readback[RE_FRAMES_IN_FLIGHT];
    uint32_t pool_cap, nslots[RE_FRAMES_IN_FLIGHT], slot_inst[RE_FRAMES_IN_FLIGHT][MAX_INST];
    ShapeEntry shapes[64];
    uint32_t nshapes, shape_used, shape_cap;
    float* shape_cpu; // (kept: a new mesh appends)
    uint32_t pending_reset; // the pool made again: every emitter starts over
} S;

// --- Assets ---
static uint32_t vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(S.root);
    if (rl && !strncmp(disk, S.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}

void particles_set_root(const char* content_root) {
    snprintf(S.root, sizeof S.root, "%s", content_root ? content_root : "");
    for (char* c = S.root; *c; c++)
        if (*c == '\\') *c = '/';
    S.scanned = false;
}

static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        if (strcmp(e->name, "Saved") && strcmp(e->name, "Intermediate")) scan_dir(path, 1);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (dot && !strcmp(dot, ".rparticle")) {
        vpath_of(path, vpath, sizeof vpath);
        ps_load(vpath);
    }
}
static void scan_dir(const char* disk, int depth) {
    if (depth > 12) return;
    re_dir_walk(disk, scan_entry, (void*)disk);
}
void ps_rescan(void) { S.scanned = false; }
static void scan(void) {
    if (S.scanned || !S.root[0]) return;
    S.scanned = true;
    scan_dir(S.root, 0);
}
uint32_t ps_asset_count(void) {
    scan();
    return S.nassets;
}
ParticleSystemAsset* ps_asset_at(uint32_t i) { return i < S.nassets ? S.assets[i] : NULL; }

static void module_defaults(PsModule* m, int kind) {
    memset(m, 0, sizeof *m);
    m->kind = (uint8_t)kind, m->on = 1;
    for (int k = 0; k < g_modules[kind].nparams; k++) m->p[k] = g_modules[kind].def[k];
}

PsModule* ps_add_module(PsEmitter* e, int kind) {
    if (kind < 0 || kind >= PM_COUNT || e->nmodules >= PS_MAX_MODULES) return NULL;
    // in its stage's place (Niagara's stack: emitter, spawn, update, render)
    uint32_t at = e->nmodules;
    while (at > 0 && g_modules[e->m[at - 1].kind].stage > g_modules[kind].stage) at--;
    memmove(&e->m[at + 1], &e->m[at], sizeof(PsModule) * (e->nmodules - at));
    e->nmodules++;
    module_defaults(&e->m[at], kind);
    return &e->m[at];
}
void ps_remove_module(PsEmitter* e, uint32_t i) {
    if (i >= e->nmodules) return;
    memmove(&e->m[i], &e->m[i + 1], sizeof(PsModule) * (e->nmodules - i - 1));
    e->nmodules--;
}
void ps_touch(ParticleSystemAsset* a) {
    a->version++;
    a->dirty = true;
}

static PsModule* add(PsEmitter* e, int kind, const float* p, int n) {
    PsModule* m = ps_add_module(e, kind);
    for (int k = 0; m && k < n; k++) m->p[k] = p[k];
    return m;
}
static PsEmitter* emitter(ParticleSystemAsset* a, const char* name) {
    if (a->nemitters >= PS_MAX_EMITTERS) return NULL;
    PsEmitter* e = &a->e[a->nemitters++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->on = 1, e->seed = 1u + a->nemitters * 7919u;
    return e;
}
#define ADD(e, k, ...) do { const float p_[] = { __VA_ARGS__ }; add(e, k, p_, (int)(sizeof p_ / sizeof p_[0])); } while (0)

void ps_template(ParticleSystemAsset* a, int t) {
    a->nemitters = 0;
    PsEmitter* e;
    switch (t) {
    case PST_FIRE:
        e = emitter(a, "Flames");
        ADD(e, PM_SPAWN_RATE, 600, 0);
        ADD(e, PM_LIFETIME, 0.5f, 1.1f);
        ADD(e, PM_SHAPE, 4, 35);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 160, 12, 0.5f, 0);
        ADD(e, PM_INIT_SIZE, 22, 40);
        ADD(e, PM_INIT_COLOR, 1.0f, 0.45f, 0.08f, 1, 1.0f, 0.25f, 0.03f, 1);
        ADD(e, PM_INIT_ROTATION, 180, 40);
        ADD(e, PM_CURL, 260, 60, 2.5f);
        ADD(e, PM_GRAVITY, 0, 0, 180);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 0.9f, 1, 0.8f, 0.6f, 0.8f, 0.6f, 0.25f, 0.1f, 0.35f, 0.2f, 0.05f, 0.02f, 0, 0.2f, 0.6f);
        ADD(e, PM_SIZE_LIFE, 0.6f, 1, 0.8f, 0.3f);
        ADD(e, PM_SPRITE, 0, 1.4f, 0, 1, 9000, 1, 0, 0);
        e = emitter(a, "Embers");
        ADD(e, PM_SPAWN_RATE, 40, 0);
        ADD(e, PM_LIFETIME, 1.5f, 3.0f);
        ADD(e, PM_SHAPE, 4, 30);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 220, 25, 0.6f, 0);
        ADD(e, PM_INIT_SIZE, 1.2f, 2.5f);
        ADD(e, PM_INIT_COLOR, 1, 0.5f, 0.1f, 1, 1, 0.3f, 0.05f, 1);
        ADD(e, PM_CURL, 400, 40, 3);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.6f, 0.4f, 0.8f, 0.5f, 0.1f, 0.05f, 0, 0.3f, 0.7f);
        ADD(e, PM_SPRITE, 1, 1, 0, 1, 20000, 1, 0, 0);
        break;
    case PST_SMOKE:
        e = emitter(a, "Smoke");
        ADD(e, PM_SPAWN_RATE, 60, 0);
        ADD(e, PM_LIFETIME, 4, 6);
        ADD(e, PM_SHAPE, 4, 40);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 90, 15, 0.4f, 0);
        ADD(e, PM_INIT_SIZE, 60, 110);
        ADD(e, PM_INIT_COLOR, 0.35f, 0.35f, 0.36f, 0.5f, 0.55f, 0.55f, 0.56f, 0.4f);
        ADD(e, PM_INIT_ROTATION, 180, 12);
        ADD(e, PM_CURL, 60, 200, 0.5f);
        ADD(e, PM_WIND, 120, 40, 0, 0.3f);
        ADD(e, PM_DRAG, 0.4f);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0.7f, 1, 1, 1, 0, 0.12f, 0.6f);
        ADD(e, PM_SIZE_LIFE, 0.5f, 1.2f, 1.8f, 2.4f);
        ADD(e, PM_SPRITE, 0, 1.0f, 1, 0, 0, 0.8f, 0, 0);
        break;
    case PST_SPARKS:
        e = emitter(a, "Sparks");
        ADD(e, PM_SPAWN_BURST, 400, 0, 1.5f);
        ADD(e, PM_LIFETIME, 0.6f, 1.6f);
        ADD(e, PM_SHAPE, 1, 5);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 900, 70, 0.6f, 0);
        ADD(e, PM_INIT_SIZE, 1, 2);
        ADD(e, PM_INIT_COLOR, 1, 0.75f, 0.35f, 1, 1, 0.5f, 0.12f, 1);
        ADD(e, PM_GRAVITY, 0, 0, -980);
        ADD(e, PM_DRAG, 0.6f);
        ADD(e, PM_COLLISION, 0, 0.35f, 0.3f, 0);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 1, 1, 0.9f, 0.7f, 1, 1, 0.5f, 0.2f, 1, 0.6f, 0.1f, 0.02f, 0, 0.3f, 0.7f);
        ADD(e, PM_SPRITE, 0, 0.8f, 0, 1, 40000, 1, 1, 0.03f);
        break;
    case PST_RAIN:
        e = emitter(a, "Rain");
        ADD(e, PM_SPAWN_RATE, 20000, 0);
        ADD(e, PM_LIFETIME, 1.2f, 1.4f);
        ADD(e, PM_SHAPE, 2, 0, 2500, 2500, 10);
        ADD(e, PM_INIT_VELOCITY, 0.1f, 0, -1, 1100, 2, 0.1f, 0);
        ADD(e, PM_INIT_SIZE, 0.6f, 0.9f);
        ADD(e, PM_INIT_COLOR, 0.7f, 0.75f, 0.8f, 0.5f, 0.8f, 0.85f, 0.9f, 0.6f);
        ADD(e, PM_COLLISION, 0, 0, 0, 1);
        ADD(e, PM_SPRITE, 0, 0.6f, 1, 0, 0, 0.7f, 1, 0.05f);
        break;
    case PST_SNOW:
        e = emitter(a, "Snow");
        ADD(e, PM_SPAWN_RATE, 6000, 0);
        ADD(e, PM_LIFETIME, 8, 10);
        ADD(e, PM_SHAPE, 2, 0, 3000, 3000, 10);
        ADD(e, PM_INIT_VELOCITY, 0, 0, -1, 90, 10, 0.4f, 0);
        ADD(e, PM_INIT_SIZE, 1.2f, 2.5f);
        ADD(e, PM_INIT_COLOR, 1, 1, 1, 0.95f, 0.9f, 0.95f, 1, 0.85f);
        ADD(e, PM_CURL, 40, 150, 0.4f);
        ADD(e, PM_COLLISION, 0, 0, 1, 0);
        ADD(e, PM_SPRITE, 0, 1.2f, 1, 0, 0, 1, 0, 0);
        break;
    case PST_FOUNTAIN:
        e = emitter(a, "Water");
        ADD(e, PM_SPAWN_RATE, 3000, 0);
        ADD(e, PM_LIFETIME, 1.6f, 2.2f);
        ADD(e, PM_SHAPE, 4, 6);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 700, 8, 0.15f, 0);
        ADD(e, PM_INIT_SIZE, 2, 4);
        ADD(e, PM_INIT_COLOR, 0.6f, 0.75f, 0.9f, 0.6f, 0.75f, 0.85f, 1, 0.7f);
        ADD(e, PM_GRAVITY, 0, 0, -980);
        ADD(e, PM_COLLISION, 0, 0.2f, 0.4f, 0);
        ADD(e, PM_SPRITE, 0, 1, 1, 0, 0, 0.8f, 1, 0.01f);
        break;
    case PST_MAGIC:
        e = emitter(a, "Swirl");
        ADD(e, PM_SPAWN_RATE, 2500, 0);
        ADD(e, PM_LIFETIME, 1.5f, 2.5f);
        ADD(e, PM_SHAPE, 4, 80);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 120, 30, 0.5f, 0);
        ADD(e, PM_INIT_SIZE, 1, 3);
        ADD(e, PM_INIT_COLOR, 0.3f, 0.5f, 1, 1, 0.8f, 0.3f, 1, 1);
        ADD(e, PM_VORTEX, 160, 40, 0, 0, 1);
        ADD(e, PM_CURL, 150, 50, 2);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0.1f, 0.7f);
        ADD(e, PM_SPRITE, 0, 1, 0, 1, 15000, 1, 0, 0);
        break;
    case PST_EXPLOSION:
        e = emitter(a, "Fireball");
        ADD(e, PM_SPAWN_BURST, 300, 0, 3);
        ADD(e, PM_LIFETIME, 0.6f, 1.4f);
        ADD(e, PM_SHAPE, 1, 30);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 500, 180, 0.7f, 200);
        ADD(e, PM_INIT_SIZE, 40, 90);
        ADD(e, PM_INIT_COLOR, 1, 0.5f, 0.1f, 1, 1, 0.3f, 0.05f, 1);
        ADD(e, PM_DRAG, 3);
        ADD(e, PM_GRAVITY, 0, 0, 150);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 1, 1, 0.7f, 0.4f, 0.8f, 0.3f, 0.15f, 0.1f, 0.4f, 0.1f, 0.08f, 0.07f, 0, 0.15f, 0.5f);
        ADD(e, PM_SIZE_LIFE, 0.5f, 1.3f, 1.8f, 2.2f);
        ADD(e, PM_SPRITE, 0, 1.2f, 0, 1, 12000, 1, 0, 0);
        e = emitter(a, "Debris");
        ADD(e, PM_SPAWN_BURST, 120, 0, 3);
        ADD(e, PM_LIFETIME, 1.5f, 2.5f);
        ADD(e, PM_SHAPE, 1, 20);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 800, 80, 0.5f, 100);
        ADD(e, PM_INIT_SIZE, 4, 10);
        ADD(e, PM_INIT_ROTATION, 180, 360);
        ADD(e, PM_GRAVITY, 0, 0, -980);
        ADD(e, PM_COLLISION, 0, 0.3f, 0.5f, 0);
        ADD(e, PM_MESH, 0.25f, 0.22f, 0.2f, 0, 0.8f, 0, 3, 1);
        break;
    case PST_DUST:
        e = emitter(a, "Motes");
        ADD(e, PM_SPAWN_RATE, 300, 0);
        ADD(e, PM_LIFETIME, 6, 10);
        ADD(e, PM_SHAPE, 2, 0, 600, 600, 200);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 5, 180, 1, 0);
        ADD(e, PM_INIT_SIZE, 0.2f, 0.5f);
        ADD(e, PM_INIT_COLOR, 1, 0.95f, 0.85f, 0.8f, 1, 1, 1, 0.6f);
        ADD(e, PM_CURL, 15, 80, 0.3f);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0.2f, 0.8f);
        ADD(e, PM_SPRITE, 0, 1, 1, 0, 50, 1, 0, 0);
        break;
    case PST_DEBRIS:
        e = emitter(a, "Rocks");
        ADD(e, PM_SPAWN_RATE, 200, 0);
        ADD(e, PM_LIFETIME, 3, 4);
        ADD(e, PM_SHAPE, 1, 30);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 700, 40, 0.5f, 0);
        ADD(e, PM_INIT_SIZE, 6, 14);
        ADD(e, PM_INIT_ROTATION, 180, 300);
        ADD(e, PM_GRAVITY, 0, 0, -980);
        ADD(e, PM_COLLISION, 0, 0.35f, 0.5f, 0);
        ADD(e, PM_MESH, 0.45f, 0.42f, 0.38f, 0, 0.85f, 0, 3, 1);
        break;
    case PST_GALAXY: // the stress: 5 million glowing points turning around the center
        e = emitter(a, "Stars");
        ADD(e, PM_SPAWN_RATE, 500000, 0);
        ADD(e, PM_LIFETIME, 9, 10);
        ADD(e, PM_SHAPE, 4, 6000);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 20, 90, 1, 0);
        ADD(e, PM_INIT_SIZE, 12, 30);
        ADD(e, PM_INIT_COLOR, 0.4f, 0.6f, 1, 1, 1, 0.7f, 0.4f, 1);
        ADD(e, PM_VORTEX, 30, 30, 0, 0, 1);
        ADD(e, PM_COLOR_LIFE, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0.1f, 0.9f);
        ADD(e, PM_SPRITE, 0, 1, 0, 1, 60, 1, 0, 0);
        break;
    case PST_RIBBON: // (B10) a glowing ribbon rising and swirling (the curl noise bends it)
        e = emitter(a, "Ribbon");
        ADD(e, PM_SPAWN_RATE, 60, 0);
        ADD(e, PM_LIFETIME, 2, 2);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 70, 0, 0, 0);
        ADD(e, PM_INIT_SIZE, 30, 30);
        ADD(e, PM_INIT_COLOR, 0.3f, 0.7f, 1, 1, 0.6f, 0.3f, 1, 1);
        ADD(e, PM_CURL, 60, 120, 0.5f);
        ADD(e, PM_SIZE_LIFE, 1, 0.3f);
        ADD(e, PM_RIBBON, 1, 20, 1, 0, 1, 40000, 1);
        break;
    default: // Empty: one emitter with the usual stack
        e = emitter(a, "Emitter");
        ADD(e, PM_SPAWN_RATE, 100, 0);
        ADD(e, PM_LIFETIME, 1.5f, 2.5f);
        ADD(e, PM_SHAPE, 1, 20);
        ADD(e, PM_INIT_VELOCITY, 0, 0, 1, 300, 15, 0.3f, 0);
        ADD(e, PM_INIT_SIZE, 5, 10);
        ADD(e, PM_GRAVITY, 0, 0, -980);
        ADD(e, PM_SPRITE, 0, 1.5f, 1, 0, 0, 1, 0, 0);
        break;
    }
    a->version++;
}

static void read_module(const JsonValue* o, PsModule* m) {
    int kind = ps_module_kind(json_get_str(o, "kind", ""));
    if (kind < 0) kind = 0;
    module_defaults(m, kind);
    m->on = json_get_bool(o, "on", true);
    json_get_floats(o, "p", m->p, PS_MAX_PARAMS);
    m->asset = (uint32_t)json_get_num(o, "asset", 0);
}

ParticleSystemAsset* ps_find_hash(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < S.nassets; i++)
        if (S.assets[i]->hash == hash) return S.assets[i];
    return NULL;
}

ParticleSystemAsset* ps_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < S.nassets; i++)
        if (S.assets[i]->hash == h) return S.assets[i];
    if (S.nassets >= MAX_ASSETS) return NULL;
    char path[800];
    disk_of(vpath, path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // (a load)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    ParticleSystemAsset* a = NULL;
    JsonDoc doc;
    char err[160];
    if (ok && json_parse(t, (size_t)n, &doc, err, sizeof err)) {
        a = calloc(1, sizeof *a); // (an asset)
        if (a) {
            snprintf(a->path, sizeof a->path, "%s", path);
            const char* s = strrchr(path, '/');
            snprintf(a->name, sizeof a->name, "%s", s ? s + 1 : path);
            char* dot = strrchr(a->name, '.');
            if (dot) *dot = 0;
            a->hash = h, a->version = 1;
            const JsonValue* es = json_get(&doc.root, "emitters");
            for (size_t i = 0; i < json_len(es) && a->nemitters < PS_MAX_EMITTERS; i++) {
                const JsonValue* eo = json_at(es, i);
                PsEmitter* e = &a->e[a->nemitters++];
                snprintf(e->name, sizeof e->name, "%s", json_get_str(eo, "name", "Emitter"));
                e->on = json_get_bool(eo, "on", true), e->local = json_get_bool(eo, "local", false);
                e->seed = (uint32_t)json_get_num(eo, "seed", 1), e->max_particles = (uint32_t)json_get_num(eo, "max", 0);
                const JsonValue* ms = json_get(eo, "modules");
                for (size_t k = 0; k < json_len(ms) && e->nmodules < PS_MAX_MODULES; k++) read_module(json_at(ms, k), &e->m[e->nmodules++]);
            }
            S.assets[S.nassets++] = a;
        }
        json_free(&doc);
    }
    free(t);
    return a;
}

bool ps_save(ParticleSystemAsset* a) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_key(&w, "emitters");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < a->nemitters; i++) {
        const PsEmitter* e = &a->e[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", e->name);
        jw_kbool(&w, "on", e->on), jw_kbool(&w, "local", e->local);
        jw_kint(&w, "seed", e->seed), jw_kint(&w, "max", e->max_particles);
        jw_key(&w, "modules");
        jw_arr_begin(&w);
        for (uint32_t k = 0; k < e->nmodules; k++) {
            const PsModule* m = &e->m[k];
            jw_obj_begin(&w);
            jw_kstr(&w, "kind", g_modules[m->kind].name);
            jw_kbool(&w, "on", m->on);
            jw_kfloats(&w, "p", m->p, g_modules[m->kind].nparams);
            if (m->asset) jw_kint(&w, "asset", m->asset);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = !w.fail;
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", a->path);
    FILE* f = ok ? re_fopen(tmp, "wb") : NULL;
    ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && re_replace_file(tmp, a->path);
    jw_free(&w);
    if (ok) a->dirty = false;
    return ok;
}

uint32_t ps_create(const char* folder_vpath, const char* name, int tmpl) {
    char vpath[700], path[800];
    snprintf(vpath, sizeof vpath, "%s/%s.rparticle", folder_vpath, name);
    disk_of(vpath, path, sizeof path);
    ParticleSystemAsset* a = calloc(1, sizeof *a); // (an asset)
    if (!a || S.nassets >= MAX_ASSETS) return free(a), 0u;
    snprintf(a->path, sizeof a->path, "%s", path);
    snprintf(a->name, sizeof a->name, "%s", name);
    a->hash = vpath_hash(vpath);
    ps_template(a, tmpl);
    if (!ps_save(a)) return free(a), 0u;
    for (uint32_t i = 0; i < S.nassets; i++) // (one of that name already: replaced)
        if (S.assets[i]->hash == a->hash) {
            *S.assets[i] = *a;
            free(a);
            return S.assets[i]->hash;
        }
    S.assets[S.nassets++] = a;
    return a->hash;
}

// --- The components' defaults ---
// (particles_system_defaults and particles_tl_defaults live in ecs.c: re_core's tests link without the renderer)

// --- Compiling an emitter into its GPU block ---
static const PsModule* find_mod(const PsEmitter* e, int kind) {
    for (uint32_t i = 0; i < e->nmodules; i++)
        if (e->m[i].kind == kind && e->m[i].on) return &e->m[i];
    return NULL;
}

// The mesh's emission table (its triangles by area, its center): found or made (NULL: none)
static const ShapeEntry* shape_of(uint32_t mesh);

static void compile_emitter(const PsEmitter* pe, const CParticleSystem* c, GpuEmitter* g, uint32_t mesh_of_actor) {
    memset(g, 0, sizeof *g);
    float spawn_scale = c ? c->spawn_scale : 1.0f, size_scale = c ? c->size_scale : 1.0f;
    const PsModule* m;
    g->life_min = 1.5f, g->life_max = 2.5f;
    if ((m = find_mod(pe, PM_LIFETIME))) g->life_min = fmaxf(m->p[0], 0.01f), g->life_max = fmaxf(fmaxf(m->p[1], m->p[0]), 0.01f);
    g->stop = 3.0e38f;
    float rate = 0.0f, burst = 0.0f;
    if ((m = find_mod(pe, PM_SPAWN_RATE))) {
        rate = fmaxf(m->p[0], 0.0f) * spawn_scale;
        if (m->p[1] > 0.0f) g->stop = m->p[1];
    }
    if ((m = find_mod(pe, PM_SPAWN_BURST))) {
        burst = floorf(fmaxf(m->p[0], 0.0f) * spawn_scale);
        g->burst_time = m->p[1], g->burst_interval = m->p[2], g->burst_count = burst;
    } else
        g->burst_time = 3.0e38f;
    if ((m = find_mod(pe, PM_EMITTER_STATE))) { // Niagara's Emitter State: the loops, the delay (extra[3], the runtime's)
        int beh = (int)(m->p[0] + 0.5f);
        float dur = fmaxf(m->p[1], 0.01f), loops = fmaxf(floorf(m->p[2] + 0.5f), 1.0f);
        if (beh == 1) g->stop = fminf(g->stop, dur);
        else if (beh == 2) g->stop = fminf(g->stop, dur * loops);
        if (beh != 1 && burst > 0.0f && g->burst_interval <= 0.0f) g->burst_interval = dur; // (a burst per loop)
        g->extra[3] = fmaxf(m->p[3], 0.0f);
    }
    g->rate = rate;
    uint32_t rs = rate > 0.0f ? (uint32_t)ceilf(rate * g->life_max * 1.02f) + 1u : 0u;
    uint64_t cap = (uint64_t)rs + (uint64_t)burst;
    if (pe->max_particles && cap > pe->max_particles) { // (the cap: the rate's slots shrink, the oldest die first)
        uint64_t over = cap - pe->max_particles;
        rs = rs > over ? rs - (uint32_t)over : 0u;
        cap = pe->max_particles;
    }
    if (cap > POOL_MAX) cap = POOL_MAX;
    g->rate_slots = rs, g->capacity = (uint32_t)cap;
    g->seed = pe->seed * 0x9E3779B1u + (c ? c->seed : 0u);
    g->scale = size_scale;
    g->flags = pe->local ? PF_LOCAL : 0u;
    // the shape
    g->shape_kind = 0;
    if ((m = find_mod(pe, PM_SHAPE))) {
        int s = (int)m->p[0];
        g->shape_kind = (uint32_t)(s < 0 ? 0 : s > 5 ? (s == 6 ? 8 : 9) : s); // (6, 7: the shader's cylinder 8, torus 9)
        if (s == 1) g->shape_p[0] = m->p[1], g->shape_p[1] = m->p[5];
        else if (s == 2) g->shape_p[0] = m->p[2] * 0.5f, g->shape_p[1] = m->p[3] * 0.5f, g->shape_p[2] = m->p[4] * 0.5f;
        else if (s == 3) g->shape_p[0] = m->p[6], g->shape_p[1] = m->p[7];
        else if (s == 4) g->shape_p[0] = m->p[1];
        else if (s == 6) g->shape_p[0] = m->p[1], g->shape_p[1] = m->p[4], g->shape_p[2] = m->p[5];
        else if (s == 7) g->shape_p[0] = m->p[1], g->shape_p[1] = m->p[8] > 0.0f ? m->p[8] : m->p[1] * 0.2f, g->shape_p[2] = m->p[5];
        else if (s == 5) {
            const ShapeEntry* se = mesh_of_actor ? shape_of(mesh_of_actor) : NULL;
            if (se) g->shape_tris = se->count, g->shape = (VkDeviceAddress)se->first, memcpy(g->shape_p, se->center, 12);
            else g->shape_kind = 0;
        }
    }
    g->vel_dir[2] = 1.0f;
    if ((m = find_mod(pe, PM_INIT_VELOCITY))) {
        g->vel_dir[0] = m->p[0], g->vel_dir[1] = m->p[1], g->vel_dir[2] = m->p[2], g->vel_dir[3] = m->p[3];
        g->vel_p[0] = m->p[4], g->vel_p[1] = m->p[5], g->vel_p[2] = m->p[6];
    }
    g->size_p[0] = 5, g->size_p[1] = 10;
    if ((m = find_mod(pe, PM_INIT_SIZE))) g->size_p[0] = m->p[0], g->size_p[1] = fmaxf(m->p[1], m->p[0]);
    if ((m = find_mod(pe, PM_INIT_ROTATION))) g->size_p[2] = m->p[0], g->size_p[3] = m->p[1];
    for (int k = 0; k < 4; k++) g->color0[k] = g->color1[k] = 1.0f;
    if ((m = find_mod(pe, PM_INIT_COLOR))) memcpy(g->color0, m->p, 16), memcpy(g->color1, m->p + 4, 16);
    if (c)
        for (int k = 0; k < 4; k++) g->color0[k] *= c->color[k], g->color1[k] *= c->color[k];
    if ((m = find_mod(pe, PM_GRAVITY))) g->flags |= PF_GRAVITY, memcpy(g->gravity, m->p, 12);
    if ((m = find_mod(pe, PM_DRAG))) g->flags |= PF_DRAG, g->gravity[3] = m->p[0];
    if ((m = find_mod(pe, PM_CURL))) g->flags |= PF_CURL, g->curl[0] = m->p[0], g->curl[1] = m->p[1], g->curl[2] = m->p[2];
    if ((m = find_mod(pe, PM_BROWNIAN))) g->flags |= PF_BROWNIAN, g->curl[3] = m->p[0];
    if ((m = find_mod(pe, PM_VORTEX))) g->flags |= PF_VORTEX, g->vortex[0] = m->p[0], g->vortex[1] = m->p[1], memcpy(g->vortex_axis, m->p + 2, 12);
    if ((m = find_mod(pe, PM_ATTRACT))) g->flags |= PF_ATTRACT, memcpy(g->attract, m->p, 16), g->attract2[0] = m->p[4], g->attract2[1] = m->p[5];
    if ((m = find_mod(pe, PM_WIND))) g->flags |= PF_WIND, memcpy(g->wind, m->p, 16);
    if ((m = find_mod(pe, PM_COLLISION))) g->flags |= PF_COLLIDE, memcpy(g->collide, m->p, 16);
    if ((m = find_mod(pe, PM_SPEED_LIMIT))) g->flags |= PF_LIMIT, g->limits[0] = m->p[0], g->limits[1] = m->p[1];
    if ((m = find_mod(pe, PM_KILL_BOX))) {
        g->flags |= PF_KILLBOX;
        memcpy(g->killbox_min, m->p, 12), memcpy(g->killbox_max, m->p + 3, 12);
        g->killbox_min[3] = m->p[6];
    }
    if ((m = find_mod(pe, PM_COLOR_LIFE))) {
        g->flags |= PF_COLOR_LIFE;
        memcpy(g->col_life, m->p, 64);
        g->col_life_t[0] = 0.0f, g->col_life_t[1] = m->p[16], g->col_life_t[2] = fmaxf(m->p[17], m->p[16]), g->col_life_t[3] = 1.0f;
    }
    if ((m = find_mod(pe, PM_SIZE_LIFE))) g->flags |= PF_SIZE_LIFE, memcpy(g->size_life, m->p, 16);
    if ((m = find_mod(pe, PM_SUBUV))) {
        g->flags |= PF_SUBUV | (m->p[4] > 0.5f ? PF_SUBUV_BLEND : 0u);
        g->sub_uv[0] = fmaxf(floorf(m->p[0] + 0.5f), 1.0f), g->sub_uv[1] = fmaxf(floorf(m->p[1] + 0.5f), 1.0f);
        g->sub_uv[2] = floorf(m->p[2] + 0.5f), g->sub_uv[3] = m->p[3];
    }
    if ((m = find_mod(pe, PM_SCALE_SPEED))) {
        g->flags |= PF_SPEED_SCALE;
        g->speed_scale[0] = m->p[0], g->speed_scale[1] = fmaxf(m->p[1], m->p[0] + 0.001f), g->speed_scale[2] = m->p[2], g->speed_scale[3] = m->p[3];
        g->extra[1] = m->p[4], g->extra[2] = m->p[5];
    }
    if ((m = find_mod(pe, PM_CAMERA_OFFSET))) g->extra[0] = m->p[0];
    // the renderer: a mesh when it has one, else sprites
    g->render_p[1] = 1.5f, g->render2[1] = 1.0f;
    if ((m = find_mod(pe, PM_MESH))) {
        uint32_t first, count;
        int32_t vo;
        uint32_t mesh = m->asset ? m->asset : 0u; // (0: the cube)
        if (render_mesh_range(mesh, &first, &count, &vo)) {
            g->flags |= PF_MESH;
            g->mesh_first_index = first, g->mesh_index_count = count, g->mesh_vertex_offset = (uint32_t)vo;
            float mn[3], mx[3];
            render_mesh_bounds(mesh, mn, mx);
            float ext = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]), fmaxf(mx[2] - mn[2], 1e-3f));
            for (int k = 0; k < 3; k++) g->mesh_scale[k] = 1.0f / ext; // (a mesh of size 1: the particle's size across)
            g->mesh_scale[3] = 0.87f;
            g->mesh_color[0] = m->p[0], g->mesh_color[1] = m->p[1], g->mesh_color[2] = m->p[2], g->mesh_color[3] = m->p[3];
            g->render2[3] = m->p[4], g->render_p[3] = m->p[5], g->render2[0] = fmaxf(m->p[6], 0.5f);
            if (m->p[7] > 0.5f) g->flags |= PF_SHADOWS;
            g->flags |= PF_LIT; // (its far sprites: lit too)
        }
    }
    if (!(g->flags & PF_MESH) && (m = find_mod(pe, PM_RIBBON))) { // (B10) a ribbon: soft across, its texture along
        g->flags |= PF_RIBBON;
        g->ribbon[0] = m->p[0], g->ribbon[1] = fmaxf(m->p[1], 1.0f), g->ribbon[2] = m->p[2];
        g->render_p[0] = 0.0f, g->render_p[1] = 1.0f, g->render_p[3] = m->p[5], g->render2[1] = m->p[6];
        if (m->p[3] > 0.5f) g->flags |= PF_LIT;
        if (m->p[4] > 0.5f) g->flags |= PF_ADDITIVE;
        if (m->asset) {
            uint32_t tex = mat_base_texture(m->asset);
            if (tex) g->render_p[0] = 2.0f, g->render2[2] = (float)(tex + 1u); // (square across: ribbon.frag samples it along)
        }
    } else if (!(g->flags & PF_MESH) && (m = find_mod(pe, PM_SPRITE))) {
        g->render_p[0] = m->p[0], g->render_p[1] = m->p[1], g->render_p[3] = m->p[4], g->render_p[2] = m->p[7];
        g->render2[1] = m->p[5];
        if (m->p[2] > 0.5f) g->flags |= PF_LIT;
        if (m->p[3] > 0.5f) g->flags |= PF_ADDITIVE;
        if (m->p[6] > 0.5f) g->flags |= PF_VELOCITY_ALIGN;
        if (m->asset) { // a material's base color texture (the sprite's shape 3)
            uint32_t tex = mat_base_texture(m->asset);
            if (tex) g->render2[2] = (float)(tex + 1u);
        }
    } else if (!(g->flags & PF_MESH)) // no renderer: soft glowing points
        g->render_p[3] = 1000.0f, g->flags |= PF_ADDITIVE;
}

// The Tlender's settings into a block (Blender's units: meters, frames)
static void compile_tl(const CParticleTl* c, GpuEmitter* g, uint32_t mesh_of_actor) {
    memset(g, 0, sizeof *g);
    float fps = c->fps > 0.0f ? c->fps : 24.0f;
    g->flags = PF_TL | (c->loop ? PF_LOOP : 0u) | (c->local ? PF_LOCAL : 0u);
    g->capacity = c->count > POOL_MAX ? POOL_MAX : c->count;
    g->tl_start = (c->frame_start - 1.0f) / fps, g->tl_end = fmaxf(c->frame_end - 1.0f, c->frame_start - 1.0f) / fps;
    float life = fmaxf(c->lifetime, 1.0f) / fps;
    g->life_max = life, g->life_min = life * (1.0f - fminf(fmaxf(c->lifetime_random, 0.0f), 1.0f));
    g->stop = 3.0e38f, g->burst_time = 3.0e38f;
    g->seed = c->seed * 0x9E3779B1u + 0x51ED270Bu;
    g->scale = 1.0f;
    const ShapeEntry* se = mesh_of_actor ? shape_of(mesh_of_actor) : NULL;
    if (se) {
        g->shape_kind = c->emit_from == PTL_FROM_VERTS ? 6u : c->emit_from == PTL_FROM_VOLUME ? 7u : 5u;
        g->shape_tris = se->count, g->shape = (VkDeviceAddress)se->first;
        memcpy(g->shape_p, se->center, 12);
    } else
        g->shape_kind = 1u, g->shape_p[0] = 50.0f; // (no mesh: a sphere of 50 cm)
    // the velocity: along the normal (the mesh shape's outward is the normal), at random, in the actor's space
    g->vel_p[3] = c->normal_factor * 100.0f;
    memcpy(g->vel_dir, c->object_velocity, 12);
    float ov = sqrtf(c->object_velocity[0] * c->object_velocity[0] + c->object_velocity[1] * c->object_velocity[1] +
                     c->object_velocity[2] * c->object_velocity[2]);
    if (ov > 1e-6f)
        for (int k = 0; k < 3; k++) g->vel_dir[k] /= ov;
    else
        g->vel_dir[2] = 1.0f;
    g->vel_dir[3] = ov * 100.0f;
    if (c->random_factor > 0.0f) g->flags |= PF_BROWNIAN, g->vel_p[0] = 180.0f, g->vel_dir[3] += c->random_factor * 100.0f, g->vel_p[1] = 1.0f;
    if (c->physics == PTL_PHYS_NEWTON) {
        if (c->gravity != 0.0f) g->flags |= PF_GRAVITY, g->gravity[2] = -980.0f * c->gravity;
        if (c->drag > 0.0f) g->flags |= PF_DRAG, g->gravity[3] = c->drag * 10.0f;
        if (c->brownian > 0.0f) g->flags |= PF_BROWNIAN, g->curl[3] = c->brownian * 100.0f;
    }
    g->flags &= ~(uint32_t)(c->random_factor > 0.0f && c->brownian <= 0.0f ? PF_BROWNIAN : 0u);
    float sz = c->size * 100.0f * 2.0f; // (Blender's size: a radius in meters)
    g->size_p[0] = sz * (1.0f - fminf(fmaxf(c->size_random, 0.0f), 1.0f)), g->size_p[1] = sz;
    g->size_p[2] = c->rotation_random, g->size_p[3] = c->angular_velocity;
    for (int k = 0; k < 3; k++) g->color0[k] = g->color1[k] = c->color[k];
    g->color0[3] = g->color1[3] = 1.0f;
    g->render_p[1] = 1.5f, g->render2[1] = 1.0f;
    if (c->render_as == PTL_RENDER_OBJECT) {
        uint32_t first, count;
        int32_t vo;
        if (render_mesh_range(c->mesh, &first, &count, &vo)) {
            g->flags |= PF_MESH | PF_LIT | (c->receive_shadows ? PF_SHADOWS : 0u);
            g->mesh_first_index = first, g->mesh_index_count = count, g->mesh_vertex_offset = (uint32_t)vo;
            float mn[3], mx[3];
            render_mesh_bounds(c->mesh, mn, mx);
            float ext = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]), fmaxf(mx[2] - mn[2], 1e-3f));
            for (int k = 0; k < 3; k++) g->mesh_scale[k] = 1.0f / ext;
            g->mesh_scale[3] = 0.87f;
            g->mesh_color[0] = g->mesh_color[1] = g->mesh_color[2] = 1.0f;
            g->render2[3] = 0.6f, g->render_p[3] = c->emissive, g->render2[0] = 3.0f;
        }
    }
    if (!(g->flags & PF_MESH)) { // halos: glowing (added) or lit (blended)
        g->render_p[3] = c->emissive;
        if (c->emissive <= 0.0f) g->flags |= PF_LIT;
        if (c->additive || c->emissive > 0.0f) g->flags |= PF_ADDITIVE;
        if (c->material) {
            uint32_t tex = mat_base_texture(c->material);
            if (tex) g->render_p[0] = 3.0f, g->render2[2] = (float)(tex + 1u);
        }
    }
}

// --- The runtime ---
static uint32_t hash_bytes(const void* p, size_t n, uint32_t h) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static PartInst* inst_find(Entity e, uint32_t key) {
    for (uint32_t i = 0; i < S.ninst; i++)
        if (S.inst[i].e == e && S.inst[i].key == key) return &S.inst[i];
    return NULL;
}

static PartInst* inst_get(Entity e, uint32_t key) {
    PartInst* p = inst_find(e, key);
    if (p || S.ninst >= MAX_INST) return p;
    p = &S.inst[S.ninst++];
    memset(p, 0, sizeof *p);
    p->e = e, p->key = key, p->active = 1;
    return p;
}

// The time its last particle dies (finite emitters: a duration, bursts once, the Tlender's without a loop); 0: never
static float end_of_(const GpuEmitter* g);
static float end_of(const GpuEmitter* g) { // (after Emitter State's start delay)
    float e = end_of_(g);
    return e > 0.0f ? e + g->extra[3] : 0.0f;
}
static float end_of_(const GpuEmitter* g) {
    if (g->flags & PF_TL) return (g->flags & PF_LOOP) ? 0.0f : g->tl_end + g->life_max;
    float end = 0.0f;
    if (g->rate > 0.0f) {
        if (g->stop > 1e37f) return 0.0f;
        end = g->stop + g->life_max;
    }
    if (g->burst_count > 0.0f) {
        if (g->burst_interval > 0.0f && g->stop > 1e37f) return 0.0f;
        end = fmaxf(end, (g->burst_interval > 0.0f ? g->stop : g->burst_time) + g->life_max);
    }
    return end;
}

static void place_rows(World* w, Entity e, float rows[3][4]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) render_transform_rows(t, rows);
    else {
        memset(rows, 0, sizeof(float) * 12);
        rows[0][0] = rows[1][1] = rows[2][2] = 1.0f;
    }
}

static bool hidden(World* w, Entity e, bool playing) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    return t && ((t->flags & TRANSFORM_HIDDEN_GAME) || (!playing && (t->flags & TRANSFORM_HIDDEN)));
}

static void tick(PartInst* p, float dt, bool playing, bool active) {
    if (active && !p->active) p->time = 0.0f, p->stop = 3.0e38f; // (activated: from the start)
    if (!active && p->active) p->stop = p->time;                // (deactivated: no more births)
    p->active = active;
    p->time += dt;
    p->end_time = end_of(&p->g);
    if (!playing && p->end_time > 0.0f && p->time > p->end_time + 0.5f) p->time = 0.0f, p->stop = 3.0e38f; // (the editor: again)
}

void particles_update(World* w, float dt, bool playing) {
    double t0 = re_time_seconds();
    S.frame++;
    if (dt > 0.1f) dt = 0.1f; // (a hitch: not a jump of the simulation)
    if (S.pending_reset) {
        for (uint32_t i = 0; i < S.ninst; i++) S.inst[i].time = 0.0f, S.inst[i].stop = 3.0e38f;
        S.pending_reset = 0;
    }
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!ar->count || !(ar->mask & (COMP_BIT(COMP_PARTICLE_SYSTEM) | COMP_BIT(COMP_PARTICLE_TL)))) continue;
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            Entity e = ents[r];
            if (hidden(w, e, playing)) continue;
            const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
            uint32_t mesh = sm ? sm->mesh : 0u;
            CParticleSystem* c = ecs_get(w, e, COMP_PARTICLE_SYSTEM);
            ParticleSystemAsset* a = c ? ps_find_hash(c->asset) : NULL;
            const char* only = getenv("REALENGINE_PS_EMITTER"); // (a debugging aid: one emitter of each system)
            for (uint32_t k = 0; a && k < a->nemitters; k++) {
                if (!a->e[k].on || (only && (uint32_t)atoi(only) != k)) continue;
                PartInst* p = inst_get(e, k);
                if (!p) break;
                CParticleSystem cv = *c;
                cv.active = 0; // (activating isn't a change of what plays)
                uint32_t ver = hash_bytes(&cv, sizeof cv, a->version * 2654435761u + k) ^ mesh;
                if (ver != p->version) {
                    compile_emitter(&a->e[k], c, &p->g, mesh);
                    p->version = ver, p->time = 0.0f, p->stop = 3.0e38f;
                }
                bool on = playing ? c->active != 0 : true;
                tick(p, dt * (c->time_scale > 0.0f ? c->time_scale : 1.0f), playing, on);
                place_rows(w, e, p->g.rows);
                p->seen = S.frame;
            }
            const CParticleTl* tl = ecs_get(w, e, COMP_PARTICLE_TL);
            if (tl) {
                PartInst* p = inst_get(e, 0xFFFFu);
                if (p) {
                    uint32_t ver = hash_bytes(tl, sizeof *tl, 0x7A11u) ^ mesh;
                    if (ver != p->version) {
                        compile_tl(tl, &p->g, mesh);
                        p->version = ver, p->time = 0.0f, p->stop = 3.0e38f;
                    }
                    tick(p, dt, playing, true);
                    place_rows(w, e, p->g.rows);
                    p->seen = S.frame;
                }
            }
        }
    }
    // the spawned ones that finished: their actor goes (Spawn System's Auto Destroy); the instances not seen go
    for (uint32_t i = 0; i < S.ninst;) {
        PartInst* p = &S.inst[i];
        if (playing && p->key != 0xFFFFu && p->end_time > 0.0f && p->time > p->end_time) {
            CParticleSystem* c = ecs_get(w, p->e, COMP_PARTICLE_SYSTEM);
            if (c && c->auto_destroy && ecs_alive(w, p->e)) ecs_destroy(w, p->e);
        }
        if (p->seen != S.frame) {
            S.inst[i] = S.inst[--S.ninst];
            continue;
        }
        i++;
    }
    S.stats.emitters = S.ninst;
    S.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
}

Entity particles_spawn(World* w, uint32_t asset, const float loc[3], const float rot[3], bool auto_destroy) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PARTICLE_SYSTEM));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "ParticleSystem");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (loc) memcpy(t->location, loc, 12);
    if (rot) memcpy(t->rotation, rot, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE;
    CParticleSystem* c = ecs_get(w, e, COMP_PARTICLE_SYSTEM);
    c->asset = asset, c->auto_destroy = auto_destroy ? 1 : 0, c->active = 1;
    return e;
}

void particles_activate(World* w, Entity e, bool on, bool reset) {
    CParticleSystem* c = ecs_get(w, e, COMP_PARTICLE_SYSTEM);
    if (!c) return;
    c->active = on ? 1 : 0;
    if (reset)
        for (uint32_t i = 0; i < S.ninst; i++)
            if (S.inst[i].e == e) S.inst[i].time = 0.0f, S.inst[i].stop = 3.0e38f, S.inst[i].active = on;
}

const ParticleStats* particles_stats(void) { return &S.stats; }
bool particles_any(void) { return S.ninst > 0; }

// --- The GPU ---
bool particles_gpu_init(VkCtx* vk, VkPipelineLayout layout, const uint32_t* sim, size_t sim_bytes, bool points) {
    S.vk = vk, S.points = points;
    S.sim = gpu_compute_pipeline(vk, layout, sim, sim_bytes);
    if (!S.sim) return false;
    VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!gpu_buf_create(vk, &S.args, (VkDeviceSize)MAX_INST * ARGS_STRIDE, st | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
        return false;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++)
        if (!gpu_buf_create(vk, &S.blocks[f], (VkDeviceSize)MAX_INST * sizeof(GpuEmitter), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &S.readback[f], (VkDeviceSize)MAX_INST * ARGS_STRIDE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
            return false;
    return true;
}

void particles_gpu_shutdown(VkCtx* vk) {
    if (S.sim) vkDestroyPipeline(vk->device, S.sim, NULL), S.sim = VK_NULL_HANDLE;
    gpu_buf_destroy(vk, &S.pool), gpu_buf_destroy(vk, &S.list_s), gpu_buf_destroy(vk, &S.list_m), gpu_buf_destroy(vk, &S.list_p), gpu_buf_destroy(vk, &S.args);
    gpu_buf_destroy(vk, &S.shape);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &S.blocks[f]), gpu_buf_destroy(vk, &S.readback[f]);
    free(S.shape_cpu), S.shape_cpu = NULL;
    S.pool_cap = S.nshapes = S.shape_used = S.shape_cap = 0;
}

// The emission table of a mesh: its triangles (local) with their cumulative areas, uploaded with the others
static const ShapeEntry* shape_of(uint32_t mesh) {
    for (uint32_t i = 0; i < S.nshapes; i++)
        if (S.shapes[i].mesh == mesh) return &S.shapes[i];
    if (!S.vk || S.nshapes >= 64) return NULL;
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t first, count;
    int32_t vo;
    render_mesh_cpu(mesh, &v, &idx, &first, &count, &vo);
    if (!v || !idx || count < 3) return NULL;
    uint32_t ntri = count / 3, stride = ntri > SHAPE_TRIS_MAX ? (ntri + SHAPE_TRIS_MAX - 1) / SHAPE_TRIS_MAX : 1u, n = (ntri + stride - 1) / stride;
    if (S.shape_used + n > S.shape_cap) {
        uint32_t cap = S.shape_cap ? S.shape_cap : 65536u;
        while (cap < S.shape_used + n) cap *= 2u;
        float* nc = realloc(S.shape_cpu, sizeof(float) * 12u * cap); // (the tables: grown with a new mesh)
        if (!nc) return NULL;
        S.shape_cpu = nc, S.shape_cap = cap;
    }
    float* out = S.shape_cpu + (size_t)S.shape_used * 12u;
    double total = 0.0;
    double c[3] = { 0, 0, 0 };
    for (uint32_t t = 0; t < n; t++) {
        const uint32_t* tri = idx + first + (size_t)t * stride * 3u;
        const float *a = v[(int32_t)tri[0] + vo].pos, *b = v[(int32_t)tri[1] + vo].pos, *d = v[(int32_t)tri[2] + vo].pos;
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
        float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        total += 0.5 * sqrt((double)cr[0] * cr[0] + (double)cr[1] * cr[1] + (double)cr[2] * cr[2]);
        float* o = out + (size_t)t * 12u;
        memcpy(o, a, 12), memcpy(o + 4, b, 12), memcpy(o + 8, d, 12);
        o[3] = (float)total, o[7] = o[11] = 0.0f;
        for (int k = 0; k < 3; k++) c[k] += (a[k] + b[k] + d[k]) / 3.0;
    }
    for (uint32_t t = 0; t < n; t++) out[(size_t)t * 12u + 3] = total > 0.0 ? (float)(out[(size_t)t * 12u + 3] / total) : (float)(t + 1) / (float)n;
    // the whole table again on the GPU (a new mesh: rare)
    vkDeviceWaitIdle(S.vk->device);
    gpu_buf_destroy(S.vk, &S.shape);
    uint32_t used = S.shape_used + n;
    GpuBuf stg = { 0 };
    if (!gpu_buf_create(S.vk, &S.shape, sizeof(float) * 12u * used, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
        !gpu_buf_create(S.vk, &stg, sizeof(float) * 12u * used, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
        return gpu_buf_destroy(S.vk, &stg), NULL;
    memcpy(stg.mapped, S.shape_cpu, sizeof(float) * 12u * used);
    VkCommandBuffer cmd = gpu_begin_once(S.vk);
    VkBufferCopy rg = { 0, 0, sizeof(float) * 12u * used };
    vkCmdCopyBuffer(cmd, stg.buffer, S.shape.buffer, 1, &rg);
    gpu_end_once(S.vk, cmd);
    gpu_buf_destroy(S.vk, &stg);
    ShapeEntry* se = &S.shapes[S.nshapes++];
    se->mesh = mesh, se->first = S.shape_used, se->count = n;
    for (int k = 0; k < 3; k++) se->center[k] = n ? (float)(c[k] / n) : 0.0f;
    S.shape_used = used;
    return se;
}

// Room for the pools: one range per emitter (packed in the order they come; made again when it grew or changed)
static bool pool_layout(VkCtx* vk) {
    uint64_t need = 0;
    for (uint32_t i = 0; i < S.ninst; i++) need += S.inst[i].g.capacity;
    if (need > POOL_MAX) need = POOL_MAX;
    if (need > S.pool_cap || !S.pool.buffer) {
        uint32_t cap = S.pool_cap ? S.pool_cap : 65536u;
        while (cap < need) cap *= 2u;
        vkDeviceWaitIdle(vk->device);
        gpu_buf_destroy(vk, &S.pool), gpu_buf_destroy(vk, &S.list_s), gpu_buf_destroy(vk, &S.list_m), gpu_buf_destroy(vk, &S.list_p);
        VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!gpu_buf_create(vk, &S.pool, (VkDeviceSize)cap * 32u, st, false) || !gpu_buf_create(vk, &S.list_s, (VkDeviceSize)cap * 4u, st, false) ||
            !gpu_buf_create(vk, &S.list_m, (VkDeviceSize)cap * 4u, st, false) || !gpu_buf_create(vk, &S.list_p, (VkDeviceSize)cap * 4u, st, false))
            return S.pool_cap = 0, false;
        VkCommandBuffer cmd = gpu_begin_once(vk); // (every slot unborn: k = all ones)
        vkCmdFillBuffer(cmd, S.pool.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
        gpu_end_once(vk, cmd);
        S.pool_cap = cap;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < S.ninst; i++) {
        PartInst* p = &S.inst[i];
        uint32_t c = p->g.capacity;
        if (at + c > S.pool_cap) c = S.pool_cap - at;
        if (p->base != at || p->capacity != c) p->time = 0.0f, p->stop = 3.0e38f; // (moved: from the start)
        p->base = at, p->capacity = c;
        at += c;
    }
    return true;
}

void particles_prepare(VkCtx* vk, World* w, uint32_t slot) {
    (void)w;
    // the counts this slot drew last time (its frame is done: the stats)
    if (S.readback[slot].mapped) {
        uint32_t ss = 0, mm = 0;
        for (uint32_t i = 0; i < S.nslots[slot]; i++) {
            const uint32_t* a = (const uint32_t*)((const uint8_t*)S.readback[slot].mapped + (size_t)i * ARGS_STRIDE);
            ss += a[0] / 6u + a[9], mm += a[5];
        }
        S.stats.sprites = ss, S.stats.meshes = mm;
    }
    S.nslots[slot] = 0;
    if (!S.ninst || !S.sim || !pool_layout(vk)) return;
    GpuEmitter* b = S.blocks[slot].mapped;
    uint64_t cap = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.ninst && n < MAX_INST; i++) {
        PartInst* p = &S.inst[i];
        if (!p->capacity) continue;
        GpuEmitter g = p->g;
        g.base = p->base, g.capacity = p->capacity;
        if (g.rate_slots > g.capacity) g.rate_slots = g.capacity;
        g.time = p->time - g.extra[3], g.dt = g.time > 0.0f ? fminf(g.time, 0.1f) : 0.0f; // (extra[3]: Emitter State's delay)
        g.stop = fminf(g.stop, p->stop);
        g.pool = S.pool.addr, g.list_sprite = S.list_s.addr + (VkDeviceAddress)p->base * 4u, g.list_mesh = S.list_m.addr + (VkDeviceAddress)p->base * 4u;
        g.args = S.args.addr + (VkDeviceAddress)n * ARGS_STRIDE;
        g.list_point = S.list_p.addr + (VkDeviceAddress)p->base * 4u;
        g.point_px = S.points ? 48.0f : 0.0f; // (a sprite up to 48 pixels across: a point)
        g.shape = g.shape_tris ? S.shape.addr + (VkDeviceAddress)g.shape * 48u : 0; // (shape held the table's first entry)
        b[n] = g;
        S.slot_inst[slot][n] = i;
        cap += p->capacity;
        n++;
    }
    S.nslots[slot] = n;
    S.stats.capacity = (uint32_t)(cap > 0xFFFFFFFFu ? 0xFFFFFFFFu : cap);
}

uint32_t particles_active(uint32_t slot) { return S.nslots[slot]; }

void particles_record(VkCommandBuffer cmd, uint32_t slot, const void* push_, VkPipelineLayout layout, bool async) {
    uint32_t n = S.nslots[slot];
    if (!n) return;
    const GpuEmitter* b = S.blocks[slot].mapped;
    Push push = *(const Push*)push_;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, TR = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // the frames before done drawing these: then this frame's counts start over
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, TR | CS, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    uint32_t init[MAX_INST * (ARGS_STRIDE / 4u)];
    memset(init, 0, (size_t)n * ARGS_STRIDE);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t* a = init + i * (ARGS_STRIDE / 4u);
        a[1] = 1u; // the sprites: vertexCount 0 (the cull adds 6 each), instanceCount 1
        a[10] = 1u; // the points: vertexCount 0 (one each), instanceCount 1
        a[4] = b[i].mesh_index_count, a[6] = b[i].mesh_first_index, a[7] = b[i].mesh_vertex_offset; // the meshes: instanceCount 0
    }
    vkCmdUpdateBuffer(cmd, S.args.buffer, 0, (VkDeviceSize)n * ARGS_STRIDE, init);
    gpu_barrier(cmd, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, S.sim);
    for (uint32_t i = 0; i < n; i++) {
        push.instances = S.blocks[slot].addr + (VkDeviceAddress)i * sizeof(GpuEmitter);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (b[i].capacity + 63u) / 64u, 1, 1);
    }
    if (async) // (the compute queue: the graphics side sees them through the semaphore it waits on)
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    else
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | TR,
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy rb = { 0, 0, (VkDeviceSize)n * ARGS_STRIDE }; // (the counts for the stats, read two frames later)
    vkCmdCopyBuffer(cmd, S.args.buffer, S.readback[slot].buffer, 1, &rb);
}

uint32_t particles_draw_meshes(VkCommandBuffer cmd, uint32_t slot, const void* push_, VkPipelineLayout layout, VkPipeline pipe, VkBuffer indices,
                               uint64_t vertices) {
    uint32_t n = S.nslots[slot], draws = 0;
    if (!n || !pipe) return 0;
    const GpuEmitter* b = S.blocks[slot].mapped;
    Push push = *(const Push*)push_;
    push.vertices = vertices;
    bool bound = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!(b[i].flags & PF_MESH)) continue;
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindIndexBuffer(cmd, indices, 0, VK_INDEX_TYPE_UINT32);
            bound = true;
        }
        push.instances = S.blocks[slot].addr + (VkDeviceAddress)i * sizeof(GpuEmitter);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDrawIndexedIndirect(cmd, S.args.buffer, (VkDeviceSize)i * ARGS_STRIDE + 16u, 1, 20);
        draws++;
    }
    return draws;
}

uint32_t particles_draw_sprites(VkCommandBuffer cmd, uint32_t slot, const void* push_, VkPipelineLayout layout, VkPipeline quads, VkPipeline points,
                                VkPipeline ribbons) {
    uint32_t n = S.nslots[slot], draws = 0;
    if (!n || !quads) return 0;
    Push push = *(const Push*)push_;
    const GpuEmitter* b = S.blocks[slot].mapped;
    S.ribbon_segments = 0;
    for (int pass = 0; pass < (S.points && points ? 2 : 1); pass++) { // the quads, then the points
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass ? points : quads);
        for (uint32_t i = 0; i < n; i++) {
            if (b[i].flags & PF_RIBBON) continue; // (drawn below: their strips)
            push.instances = S.blocks[slot].addr + (VkDeviceAddress)i * sizeof(GpuEmitter);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
            vkCmdDrawIndirect(cmd, S.args.buffer, (VkDeviceSize)i * ARGS_STRIDE + (pass ? 36u : 0u), 1, 16);
            draws++;
        }
    }
    // (B10) the ribbons: a quad between each slot and the next (ribbon.vert drops the pairs not born one after the other)
    bool bound = false;
    for (uint32_t i = 0; ribbons && i < n; i++) {
        if (!(b[i].flags & PF_RIBBON) || b[i].rate_slots < 2) continue;
        if (!bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ribbons), bound = true;
        push.instances = S.blocks[slot].addr + (VkDeviceAddress)i * sizeof(GpuEmitter);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdDraw(cmd, b[i].rate_slots * 6u, 1, 0, 0);
        S.ribbon_segments += b[i].rate_slots, draws++;
    }
    return draws;
}
uint32_t particles_ribbon_segments(void) { return S.ribbon_segments; }

void ps_compile_cpu(const PsEmitter* pe, GpuEmitter* g) { compile_emitter(pe, NULL, g, 0); }
float ps_end_time(const GpuEmitter* g) { return end_of(g); }

// --- The self-test ---
// particles.glsl p_birth on the CPU (the same arithmetic): its slots' births over time
static bool birth_cpu(const GpuEmitter* e, uint32_t i, float t, float* b, uint32_t* k) {
    if (i < e->rate_slots) {
        if (e->rate <= 0.0f) return false;
        float b0 = (float)i / e->rate, period = (float)e->rate_slots / e->rate, last = fminf(t, e->stop);
        if (last < b0) return false;
        float kk = floorf((last - b0) / period);
        *b = b0 + kk * period, *k = (uint32_t)kk;
        return true;
    }
    return false;
}

int particles_selftest(void) {
    int fails = 0;
    // the births: a rate of 100 / s for 2 s of life fills ~204 slots; at t = 3 s about 200 alive, each slot once
    PsEmitter pe;
    memset(&pe, 0, sizeof pe);
    pe.on = 1, pe.seed = 3;
    float p1[] = { 100, 0 }, p2[] = { 2, 2 };
    add(&pe, PM_SPAWN_RATE, p1, 2);
    add(&pe, PM_LIFETIME, p2, 2);
    GpuEmitter g;
    compile_emitter(&pe, NULL, &g, 0);
    uint32_t alive = 0;
    for (uint32_t i = 0; i < g.capacity; i++) {
        float b;
        uint32_t k;
        if (birth_cpu(&g, i, 3.0f, &b, &k) && 3.0f - b < g.life_max) alive++;
    }
    printf("[RealEngine] particles: rate 100/s, life 2 s: %u slots, %u alive at 3 s\n", g.capacity, alive);
    if (g.capacity < 200 || g.capacity > 210 || alive < 195 || alive > 205) printf("[RealEngine] FAIL: particles: the births\n"), fails++;
    // the stack keeps Niagara's order whatever the order added in
    PsEmitter e2;
    memset(&e2, 0, sizeof e2);
    ps_add_module(&e2, PM_SPRITE), ps_add_module(&e2, PM_GRAVITY), ps_add_module(&e2, PM_SPAWN_RATE), ps_add_module(&e2, PM_LIFETIME);
    for (uint32_t i = 1; i < e2.nmodules; i++)
        if (g_modules[e2.m[i - 1].kind].stage > g_modules[e2.m[i].kind].stage) printf("[RealEngine] FAIL: particles: the stack's order\n"), fails++, i = e2.nmodules;
    // every template compiles into something that draws
    for (int t = 0; t < PST_COUNT; t++) {
        ParticleSystemAsset a;
        memset(&a, 0, sizeof a);
        ps_template(&a, t);
        uint64_t cap = 0;
        for (uint32_t k = 0; k < a.nemitters; k++) {
            compile_emitter(&a.e[k], NULL, &g, 0);
            cap += g.capacity;
        }
        if (!a.nemitters || !cap) printf("[RealEngine] FAIL: particles: the template %s is empty\n", g_ps_template_names[t]), fails++;
        if (t == PST_GALAXY && cap < 5000000u) printf("[RealEngine] FAIL: particles: the galaxy has %llu, not 5 M\n", (unsigned long long)cap), fails++;
        if (t == PST_RIBBON && (!(g.flags & PF_RIBBON) || g.rate_slots < 100 || g.ribbon[0] <= 0.0f)) // (B10: a ribbon, its births in the rate slots)
            printf("[RealEngine] FAIL: particles: the ribbon template isn't drawn as a ribbon (%u slots)\n", g.rate_slots), fails++;
    }
    // the Tlender's: count over its frames
    CParticleTl tl;
    particles_tl_defaults(&tl);
    compile_tl(&tl, &g, 0);
    if (g.capacity != 1000u || fabsf(g.tl_end - 199.0f / 24.0f) > 1e-3f || !(g.flags & PF_TL)) printf("[RealEngine] FAIL: particles: the Tlender's\n"), fails++;
    printf("[RealEngine] particles self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
