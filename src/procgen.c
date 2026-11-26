// Procedural worlds (procgen.h, notas-dev/procedural.md)
#include "procgen.h"
#include "pcg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "anim.h"
#include "blueprint.h"
#include "components.h"
#include "gi.h"
#include "ikrig.h"
#include "landscape.h"
#include "platform.h"
#include "render.h"
#include "vehicle.h"

void bp_boxes_changed(void); // (bp_vm.c: the level's collision boxes gathered again)

const char* const g_procgen_mode_names[PROCGEN_MODE_COUNT] = { "Terrain", "Facility" };
const char* const g_biome_names[BIOME_COUNT] = { "Ocean", "Beach", "Plains", "Forest", "Desert", "Snow", "Mountain" };

#define CHUNK_COLS 16          // columns per chunk side at the finest detail
#define LODS 5                 // 16, 8, 4, 2, 1 columns a side
#define MAX_CHUNKS 4096
#define CHUNK_ENTS 400         // columns + trees + a structure's blocks
#define TABLE 8192             // the chunks' hash table (a power of two)
#define JOBS 32                // chunks whose heights are made a frame (on the workers); their entities within 2.5 ms
#define MAX_CREATURES 64
// (2026-11-25, item 20) a facility's rooms: as many as asked (the arrays made for them), held only by the entities a
// level can hold (each room up to ~9: two slabs, walls, a light, scrap, a creature): past FACILITY_ROOMS_SLOW it is said
// to be slow, past FACILITY_ROOMS_MAX it is held there and said so (the log)
#define FACILITY_ROOMS_SLOW 2000
#define FACILITY_ROOMS_MAX 50000

// --- Noise: gradients on an integer lattice (endless: int64 cells), double inside ---
static uint64_t mix64(uint64_t x) {
    x ^= x >> 30, x *= 0xBF58476D1CE4E5B9ull, x ^= x >> 27, x *= 0x94D049BB133111EBull, x ^= x >> 31;
    return x;
}
static uint64_t hash3(int64_t a, int64_t b, uint64_t c) { return mix64((uint64_t)a * 0x9E3779B97F4A7C15ull ^ mix64((uint64_t)b + 0x632BE59BD9B4E019ull) ^ c); }
static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
static double g_grad[256][2];
static void grad_init(void) {
    if (g_grad[0][0] != 0.0 || g_grad[0][1] != 0.0) return;
    for (int i = 0; i < 256; i++) g_grad[i][0] = cos(i * 6.283185307179586 / 256.0), g_grad[i][1] = sin(i * 6.283185307179586 / 256.0);
}
static double grad(int64_t ix, int64_t iy, uint32_t seed, double dx, double dy) {
    const double* g = g_grad[hash3(ix, iy, seed) >> 56];
    return g[0] * dx + g[1] * dy;
}
static double perlin(double x, double y, uint32_t seed) { // about [-0.7, 0.7]
    double fx = floor(x), fy = floor(y);
    int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double dx = x - fx, dy = y - fy, u = fade(dx), v = fade(dy);
    double a = grad(ix, iy, seed, dx, dy), b = grad(ix + 1, iy, seed, dx - 1.0, dy);
    double c = grad(ix, iy + 1, seed, dx, dy - 1.0), d = grad(ix + 1, iy + 1, seed, dx - 1.0, dy - 1.0);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
static double fbm(double x, double y, uint32_t seed, int oct) { // about [-1, 1]
    double s = 0.0, amp = 1.0, norm = 0.0;
    for (int o = 0; o < oct; o++, x *= 2.03, y *= 2.03, amp *= 0.5) s += perlin(x, y, seed + (uint32_t)o * 7919u) * amp, norm += amp;
    return s / norm * 1.45;
}
static double ridged(double x, double y, uint32_t seed, int oct) { // [0, 1]: sharp crests
    double s = 0.0, amp = 1.0, norm = 0.0;
    for (int o = 0; o < oct; o++, x *= 2.0, y *= 2.0, amp *= 0.5) {
        double r = 1.0 - fabs(perlin(x, y, seed + 101u + (uint32_t)o * 104729u) * 1.4);
        s += r * r * amp, norm += amp;
    }
    return s / norm;
}
static double smooth01(double e0, double e1, double x) {
    double t = (x - e0) / (e1 - e0);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

// --- The world's numbers (a copy of the actor's component) ---
typedef struct Gen {
    uint32_t seed;
    double block, step, height, water, biome_scale; // cm
} Gen;

// The terrain at an endless point (cm): its height above the actor (cm, before rounding) and its biome
static double terrain_raw(const Gen* g, double x, double y, int* biome) {
    grad_init();
    double m = 1.0 / 100.0; // (meters)
    double bs = g->biome_scale * m;
    // a little domain warp: no straight coasts
    double wx = x * m + fbm(x * m / 300.0, y * m / 300.0, g->seed + 11u, 3) * 40.0;
    double wy = y * m + fbm(x * m / 300.0 + 5.2, y * m / 300.0 - 1.3, g->seed + 13u, 3) * 40.0;
    double cont = fbm(wx / (bs * 2.5), wy / (bs * 2.5), g->seed, 4);             // land and sea
    double hills = fbm(wx / 180.0, wy / 180.0, g->seed + 3u, 4);                  // rolling ground
    double mask = smooth01(0.05, 0.55, fbm(wx / (bs * 1.6), wy / (bs * 1.6), g->seed + 5u, 3) + cont * 0.35);
    double mnt = ridged(wx / 700.0, wy / 700.0, g->seed + 7u, 5) * mask;          // ranges
    double H = g->height * m;
    double h = cont * 28.0 + hills * 6.0 + mnt * H + 2.0; // meters above the water
    if (biome) {
        double t = fbm(wx / bs + 100.0, wy / bs, g->seed + 17u, 3), wet = fbm(wx / bs - 100.0, wy / bs, g->seed + 19u, 3);
        int b;
        if (h < -1.5) b = BIOME_OCEAN;
        else if (h < 1.2) b = BIOME_BEACH;
        else if (mnt > 0.45 && h > H * 0.35) b = BIOME_MOUNTAIN;
        else if (t < -0.28) b = BIOME_SNOW;
        else if (t > 0.25 && wet < 0.0) b = BIOME_DESERT;
        else if (wet > 0.12) b = BIOME_FOREST;
        else b = BIOME_PLAINS;
        *biome = b;
    }
    return h * 100.0 + g->water;
}
static double round_step(const Gen* g, double h) { return floor(h / g->step + 0.5) * g->step; }

// --- State ---
typedef struct Chunk {
    int64_t cx, cy;
    uint8_t lod, used, pad[2];
    uint32_t seen, n, ncreature, ntrees, nstruct;
    Entity* ents;            // the pool's CHUNK_ENTS
    Entity creatures[4];
} Chunk;

static struct {
    char root[600];
    World* w;
    Entity actor;
    uint32_t cfg_hash;
    Gen g;
    int mode;
    double origin[3];        // cm: where the level's 0 is in the endless world (the rebases' sum)
    float base[3];           // the actor's place (cm, the level's)
    Chunk chunks[MAX_CHUNKS];
    int32_t table[TABLE];    // chunk index + 1, open addressing
    Entity* pool;
    uint32_t frame;
    Entity water;
    int64_t focus_cx, focus_cy;
    bool focus_ok, playing, pending;
    uint32_t revision;       // (the world's after this module's last change: a bulk change since - an undo, a load - rebuilds)
    float view, rebase_at;
    ProcRebaseFn hooks[16];
    uint32_t nhooks;
    void (*cam_hook)(const float d[3]);
    ProcStats stats;
    // the facility
    Entity* rooms_ents; // (grown as the rooms need: 128 rooms and 4096 actors were all it held)
    float deep[3], deep_side; // (the tests) the last facility's deepest room
    bool deep_ok;
    uint32_t nroom_ents, room_ents_cap;
    uint32_t facility_hash;
    Entity creatures[MAX_CREATURES];
    uint32_t ncreatures;
} P;

void procgen_init(const char* content_root) {
    snprintf(P.root, sizeof P.root, "%s", content_root ? content_root : "");
    procgen_rebase_hook(vehicle_rebase), procgen_rebase_hook(ikphys_rebase);
}
void procgen_rebase_hook(ProcRebaseFn fn) {
    for (uint32_t i = 0; i < P.nhooks; i++)
        if (P.hooks[i] == fn) return;
    if (P.nhooks < 16) P.hooks[P.nhooks++] = fn;
}
void procgen_set_camera_hook(void (*fn)(const float d[3])) { P.cam_hook = fn; }
bool procgen_facility_deepest(float center[3], float* side) {
    if (!P.deep_ok) return false;
    for (int k = 0; k < 3; k++) center[k] = P.deep[k];
    *side = P.deep_side;
    return true;
}
const ProcStats* procgen_stats(void) { return &P.stats; }
void procgen_origin(double out[3]) { memcpy(out, P.origin, sizeof P.origin); }

static Chunk* chunk_find(int64_t cx, int64_t cy) {
    uint32_t h = (uint32_t)hash3(cx, cy, 0) & (TABLE - 1);
    for (uint32_t k = 0; k < TABLE; k++, h = (h + 1) & (TABLE - 1)) {
        int32_t i = P.table[h];
        if (!i) return NULL;
        if (i > 0 && P.chunks[i - 1].used && P.chunks[i - 1].cx == cx && P.chunks[i - 1].cy == cy) return &P.chunks[i - 1];
    }
    return NULL;
}
static void table_rebuild(void) {
    memset(P.table, 0, sizeof P.table);
    for (uint32_t i = 0; i < MAX_CHUNKS; i++) {
        if (!P.chunks[i].used) continue;
        uint32_t h = (uint32_t)hash3(P.chunks[i].cx, P.chunks[i].cy, 0) & (TABLE - 1);
        while (P.table[h]) h = (h + 1) & (TABLE - 1);
        P.table[h] = (int32_t)i + 1;
    }
}

// --- The world's ground (landscape_height's hook) ---
static float chunk_cm(void) { return (float)(P.g.block * CHUNK_COLS); }
bool procgen_height(float x, float y, float* z) {
    if (!P.w || P.mode != PROCGEN_TERRAIN || !P.actor) return false;
    // (the finest column's top under the point: what the near chunks draw)
    double ex = (double)x + P.origin[0] - P.base[0], ey = (double)y + P.origin[1] - P.base[1];
    double cx = (floor(ex / P.g.block) + 0.5) * P.g.block, cy = (floor(ey / P.g.block) + 0.5) * P.g.block;
    double h = round_step(&P.g, terrain_raw(&P.g, cx, cy, NULL));
    if (h < P.g.water - P.g.step * 40.0) h = P.g.water - P.g.step * 40.0;
    *z = (float)(h + P.base[2] - P.origin[2]);
    return true;
}
int procgen_biome(float x, float y) {
    if (!P.w || !P.actor) return BIOME_PLAINS;
    int b = BIOME_PLAINS;
    terrain_raw(&P.g, (double)x + P.origin[0] - P.base[0], (double)y + P.origin[1] - P.base[1], &b);
    return b;
}

// --- Entities ---
static Entity block(World* w, const float c[3], const float s[3], int shape, const float col[3], float rough) {
    Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, c, 12), t->scale[0] = s[0] / 100.0f, t->scale[1] = s[1] / 100.0f, t->scale[2] = s[2] / 100.0f;
    t->flags = MOBILITY_STATIC;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape, memcpy(m->base_color, col, 12), m->roughness = rough, m->cast_shadow = 1;
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = at->parent = P.actor, at->kind = ATTACH_PROCGEN;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    return e;
}

static void biome_color(int b, double h, double hmax, bool cliff, uint64_t jit, float out[3]) {
    static const float c[BIOME_COUNT][3] = { { 0.33f, 0.30f, 0.22f }, { 0.78f, 0.70f, 0.48f }, { 0.20f, 0.42f, 0.11f }, { 0.11f, 0.29f, 0.07f },
                                             { 0.80f, 0.62f, 0.36f }, { 0.90f, 0.92f, 0.95f }, { 0.42f, 0.40f, 0.38f } };
    const float* k = c[b];
    if (b == BIOME_MOUNTAIN && h > hmax * 0.62) k = c[BIOME_SNOW];
    static const float rock[3] = { 0.38f, 0.36f, 0.33f };
    if (cliff && b != BIOME_SNOW && b != BIOME_OCEAN && b != BIOME_BEACH) k = rock;
    float j = 0.92f + (float)(jit & 255u) / 255.0f * 0.16f;
    out[0] = k[0] * j, out[1] = k[1] * j, out[2] = k[2] * j;
}

static void chunk_free(World* w, Chunk* c) {
    for (uint32_t i = 0; i < c->n; i++)
        if (ecs_alive(w, c->ents[i])) ecs_destroy(w, c->ents[i]);
    for (uint32_t i = 0; i < c->ncreature; i++)
        if (ecs_alive(w, c->creatures[i])) ecs_destroy(w, c->creatures[i]);
    c->used = 0, c->n = 0, c->ncreature = 0;
}

static Entity creature(World* w, const CProcWorld* cfg, const float at[3], float yaw) {
    Entity e = ENTITY_NONE;
    BpAsset* cls = cfg->creature_class ? bp_find_hash(cfg->creature_class) : NULL;
    float rot[3] = { 0, yaw, 0 };
    if (cls) return bp_spawn(w, cls, at, rot);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    if (!m) return ENTITY_NONE;
    e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_AI_AGENT));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Creature");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12), t->location[2] += 92.0f, t->rotation[1] = yaw, t->flags = MOBILITY_MOVABLE;
    CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    ch->max_walk_speed = 380.0f, ch->jump_z_velocity = 520.0f, ch->gravity_scale = 1.0f, ch->radius = 34.0f, ch->half_height = 88.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m->hash, sk->anim_class = abp ? abp->hash : 0;
    CAIAgent* ag = ecs_get(w, e, COMP_AI_AGENT);
    const BehaviorTree* bt = ai_tree_name("BT_Chase");
    ag->mode = bt ? AI_MODE_TREE : AI_MODE_NONE, ag->asset = bt ? bt->hash : 0, ag->team = 2;
    return e;
}

// A chunk made at its detail: its columns (and, near, its trees, a structure, its creatures)
// A chunk's heights with a border (each column's sides reach down to its lowest neighbor): pure, on the workers
typedef struct HeightJob { int64_t cx, cy; int lod; Chunk* old; double hh[18][18]; int bb[18][18]; } HeightJob;
static void heights(void* arg, int index) {
    HeightJob* j = (HeightJob*)arg + index;
    const Gen* g = &P.g;
    int n = CHUNK_COLS >> j->lod;
    double s = g->block * (double)(1 << j->lod), csz = g->block * CHUNK_COLS;
    double x0 = (double)j->cx * csz, y0 = (double)j->cy * csz;
    for (int y = -1; y <= n; y++)
        for (int x = -1; x <= n; x++) {
            double h = round_step(g, terrain_raw(g, x0 + (x + 0.5) * s, y0 + (y + 0.5) * s, &j->bb[y + 1][x + 1]));
            if (h < g->water - g->step * 40.0) h = g->water - g->step * 40.0;
            j->hh[y + 1][x + 1] = h;
        }
}

static uint32_t chunk_make(World* w, const CProcWorld* cfg, Chunk* c, double (*hh)[18], int (*bb)[18]) {
    const Gen* g = &P.g;
    int n = CHUNK_COLS >> c->lod;
    double s = g->block * (double)(1 << c->lod), csz = g->block * CHUNK_COLS;
    double x0 = (double)c->cx * csz, y0 = (double)c->cy * csz;
    float off[3] = { (float)(P.base[0] - P.origin[0]), (float)(P.base[1] - P.origin[1]), (float)(P.base[2] - P.origin[2]) };
    c->n = 0, c->ntrees = 0, c->nstruct = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            double top = hh[j + 1][i + 1], lo = top;
            for (int k = 0; k < 4; k++) {
                static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                lo = fmin(lo, hh[j + 1 + d[k][1]][i + 1 + d[k][0]]);
            }
            bool cliff = top - lo > g->step * 2.5 * (double)(1 << c->lod);
            double bottom = lo - g->step;
            float col[3];
            biome_color(bb[j + 1][i + 1], top - g->water, g->height, cliff, hash3(c->cx * 64 + i, c->cy * 64 + j, g->seed), col);
            float ctr[3] = { (float)(x0 + (i + 0.5) * s) + off[0], (float)(y0 + (j + 0.5) * s) + off[1], (float)((top + bottom) * 0.5) + off[2] };
            float sz[3] = { (float)s, (float)s, (float)(top - bottom) };
            Entity e = block(w, ctr, sz, SHAPE_CUBE, col, bb[j + 1][i + 1] == BIOME_SNOW ? 0.5f : 0.85f);
            if (e && c->n < CHUNK_ENTS) c->ents[c->n++] = e;
        }
    // trees (near): their columns' biome, by a hash per column
    if (c->lod <= 1) {
        double dens = cfg->tree_density / 1000.0 * (s / 100.0) * (s / 100.0); // a column's chance
        for (int j = 0; j < n && c->n + 2 < CHUNK_ENTS; j++)
            for (int i = 0; i < n && c->n + 2 < CHUNK_ENTS; i++) {
                int b = bb[j + 1][i + 1];
                double top = hh[j + 1][i + 1];
                if (top <= g->water + g->step || (b != BIOME_FOREST && b != BIOME_PLAINS && b != BIOME_SNOW)) continue;
                double chance = dens * (b == BIOME_FOREST ? 4.0 : b == BIOME_SNOW ? 1.5 : 0.6);
                uint64_t hsh = hash3(c->cx * 64 + i, c->cy * 64 + j, g->seed + 999u);
                if ((double)(hsh & 0xFFFFFF) / 16777216.0 >= chance) continue;
                float tall = 450.0f + (float)((hsh >> 24) & 255u) / 255.0f * 450.0f;
                float base[3] = { (float)(x0 + (i + 0.5) * s) + off[0], (float)(y0 + (j + 0.5) * s) + off[1], (float)top + off[2] };
                const float bark[3] = { 0.24f, 0.15f, 0.08f }, leaf[3] = { 0.07f, 0.22f + (float)((hsh >> 32) & 63u) / 900.0f, 0.06f };
                const float pine[3] = { 0.05f, 0.16f, 0.08f };
                float tc[3] = { base[0], base[1], base[2] + tall * 0.3f }, ts[3] = { 40, 40, tall * 0.6f };
                Entity e = block(w, tc, ts, SHAPE_CYLINDER, bark, 0.9f);
                if (e) c->ents[c->n++] = e, c->ntrees++;
                bool conifer = b == BIOME_SNOW || (hsh >> 40) % 3 == 0;
                float lc[3] = { base[0], base[1], base[2] + tall * (conifer ? 0.62f : 0.72f) };
                float ls[3] = { tall * 0.55f, tall * 0.55f, tall * (conifer ? 0.85f : 0.5f) };
                e = block(w, lc, ls, conifer ? SHAPE_CONE : SHAPE_SPHERE, conifer ? pine : leaf, 0.8f);
                if (e) c->ents[c->n++] = e;
            }
    }
    // a structure (near): one chance per chunk, a ruin of stone walls or a watchtower, on land
    if (c->lod <= 1 && cfg->structure_density > 0.0f) {
        uint64_t hsh = hash3(c->cx, c->cy, g->seed + 4242u);
        double area_km2 = (csz / 100000.0) * (csz / 100000.0);
        double ci = (double)n * 0.5;
        int mi = (int)ci, mj = (int)ci;
        double top = hh[mj + 1][mi + 1];
        if ((double)(hsh & 0xFFFFFF) / 16777216.0 < cfg->structure_density * area_km2 && top > g->water + g->step && c->n + 24 < CHUNK_ENTS) {
            float cx = (float)(x0 + csz * 0.5) + off[0], cy = (float)(y0 + csz * 0.5) + off[1], cz = (float)top + off[2];
            BpAsset* cls = cfg->structure_class ? bp_find_hash(cfg->structure_class) : NULL;
            if (cls) {
                Entity e = bp_spawn(w, cls, (float[3]){ cx, cy, cz }, (float[3]){ 0, (float)((hsh >> 24) % 360), 0 });
                if (e) c->ents[c->n++] = e;
            } else {
                const float stone[3] = { 0.47f, 0.45f, 0.42f };
                bool tower = (hsh >> 30) & 1u;
                if (tower) {
                    for (int k = 0; k < 6; k++) {
                        float bc[3] = { cx, cy, cz + 150.0f + (float)k * 300.0f }, bs[3] = { 500.0f - (float)k * 30.0f, 500.0f - (float)k * 30.0f, 300.0f };
                        Entity e = block(w, bc, bs, SHAPE_CUBE, stone, 0.9f);
                        if (e) c->ents[c->n++] = e;
                    }
                } else {
                    for (int k = 0; k < 4; k++) { // four broken walls around a court
                        float a = (float)k * 1.5707963f, len = 900.0f, hgt = 150.0f + (float)((hsh >> (k * 8)) & 255u);
                        float wc[3] = { cx + cosf(a) * 450.0f, cy + sinf(a) * 450.0f, cz + hgt * 0.5f };
                        float ws[3] = { k & 1 ? 60.0f : len, k & 1 ? len : 60.0f, hgt };
                        Entity e = block(w, wc, ws, SHAPE_CUBE, stone, 0.9f);
                        if (e) c->ents[c->n++] = e;
                    }
                }
            }
            c->nstruct++;
        }
    }
    // creatures (playing, the nearest chunks): by the biome's own share
    c->ncreature = 0;
    if (P.playing && c->lod == 0 && cfg->creature_density > 0.0f && P.ncreatures < MAX_CREATURES) {
        uint64_t hsh = hash3(c->cx, c->cy, g->seed + 777u);
        double area = (csz / 10000.0) * (csz / 10000.0); // (in 100 m x 100 m)
        double expect = cfg->creature_density * area;
        int count = (int)expect + ((double)(hsh & 0xFFFF) / 65536.0 < expect - floor(expect) ? 1 : 0);
        for (int k = 0; k < count && c->ncreature < 4 && P.ncreatures < MAX_CREATURES; k++) {
            uint64_t q = hash3(c->cx * 4 + k, c->cy, g->seed + 31u);
            int i = (int)(q % (uint64_t)n), j = (int)((q >> 16) % (uint64_t)n);
            double top = hh[j + 1][i + 1];
            if (top <= g->water) continue;
            float at[3] = { (float)(x0 + (i + 0.5) * s) + off[0], (float)(y0 + (j + 0.5) * s) + off[1], (float)top + off[2] };
            Entity e = creature(w, cfg, at, (float)((q >> 32) % 360));
            if (e) c->creatures[c->ncreature++] = e, P.creatures[P.ncreatures++] = e;
        }
    }
    return (uint32_t)(n * n);
}

static int lod_for(double dist_chunks) {
    return dist_chunks < 2.5 ? 0 : dist_chunks < 6.0 ? 1 : dist_chunks < 14.0 ? 2 : dist_chunks < 30.0 ? 3 : 4;
}

// --- The origin moved back toward the player (every system with world positions told) ---
static void rebase(World* w, const float d[3]) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t r = 0; r < ar->count; r++) t[r].location[0] += d[0], t[r].location[1] += d[1], t[r].location[2] += d[2];
        if (ar->mask & COMP_BIT(COMP_RIGID_BODY)) {
            CRigidBody* rb = ecs_arch_column(ar, COMP_RIGID_BODY);
            for (uint32_t r = 0; r < ar->count; r++) rb[r].rest_pos[0] += d[0], rb[r].rest_pos[1] += d[1], rb[r].rest_pos[2] += d[2];
        }
    }
    for (int k = 0; k < 3; k++) P.origin[k] -= d[k];
    w->revision++; // (a bulk change: the renderer's scene and the colliders gathered again)
    bp_boxes_changed();
    gi_reset_probes();
    for (uint32_t i = 0; i < P.nhooks; i++) P.hooks[i](w, d);
    if (P.cam_hook) P.cam_hook(d);
    P.stats.rebases++;
    memcpy(P.stats.origin, P.origin, sizeof P.origin);
    printf("[RealEngine] Procedural world: origin moved by %.0f, %.0f m (the world's 0 now %.1f, %.1f km away)\n", d[0] / 100.0f, d[1] / 100.0f,
           P.origin[0] / 100000.0, P.origin[1] / 100000.0);
}

// --- The facility ---
typedef struct Room { int x, y, depth; uint8_t door[4]; } Room; // doors: +x, -x, +y, -y
static int room_at(const Room* r, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (r[i].x == x && r[i].y == y) return i;
    return -1;
}
// (2026-11-25) the rooms by their cell: open addressing over (x, y), half full at most (room_at searched them all for each
// try: thousands of rooms were seconds)
typedef struct RoomGrid { int* slot; uint32_t mask; } RoomGrid;
static uint32_t cell_key(int x, int y) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u); }
static bool grid_make(RoomGrid* g, int n) {
    uint32_t s = 64;
    while (s < (uint32_t)n * 2u + 2u) s *= 2u;
    g->slot = malloc(sizeof(int) * s), g->mask = s - 1u;
    if (g->slot) memset(g->slot, 0xFF, sizeof(int) * s);
    return g->slot != NULL;
}
static int grid_at(const RoomGrid* g, const Room* r, int x, int y) {
    for (uint32_t h = cell_key(x, y) & g->mask;; h = (h + 1u) & g->mask) {
        int i = g->slot[h];
        if (i < 0) return -1;
        if (r[i].x == x && r[i].y == y) return i;
    }
}
static void grid_put(RoomGrid* g, const Room* r, int i) {
    uint32_t h = cell_key(r[i].x, r[i].y) & g->mask;
    while (g->slot[h] >= 0) h = (h + 1u) & g->mask;
    g->slot[h] = i;
}
// Grown from the entrance: a random room with a free side gets a neighbor through a door (a tree), then some loops.
// r: room for want rooms (the caller's): want made (a free side is always somewhere: only -y of the entrance's row is
// refused)
static int facility_layout(uint32_t seed, int want, float loops, Room* r) {
    int n = 1;
    if (want < 1) want = 1;
    memset(r, 0, sizeof(Room) * (size_t)want);
    RoomGrid g;
    if (!grid_make(&g, want)) return 1;
    grid_put(&g, r, 0);
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    uint64_t s = mix64(seed + 1u);
    for (long long tries = 0; n < want && tries < (long long)want * 200; tries++) {
        s = mix64(s);
        int from = (int)(s % (uint64_t)n), side = (int)((s >> 20) & 3u);
        int nx = r[from].x + d[side][0], ny = r[from].y + d[side][1];
        if (grid_at(&g, r, nx, ny) >= 0 || ny < 0) continue; // (the entrance's side stays outside: -y)
        r[n].x = nx, r[n].y = ny, r[n].depth = r[from].depth + 1;
        r[from].door[side] = 1, r[n].door[side ^ 1] = 1;
        grid_put(&g, r, n);
        n++;
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k += 2) { // (+x and +y neighbors: each pair once)
            int j = grid_at(&g, r, r[i].x + d[k][0], r[i].y + d[k][1]);
            s = mix64(s);
            if (j >= 0 && !r[i].door[k] && (double)(s & 0xFFFF) / 65536.0 < loops) r[i].door[k] = 1, r[j].door[k ^ 1] = 1;
        }
    r[0].door[3] = 1; // the entrance
    free(g.slot);
    return n;
}

static Entity proto(World* w, int kind, const float c[3], float yaw, const float size[3], float thick, const float opening[3], const float col[3]) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROTO) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Facility");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, c, 12), t->rotation[1] = yaw, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    CProto* p = ecs_get(w, e, COMP_PROTO);
    p->kind = (uint32_t)kind, memcpy(p->size, size, 12), p->thickness = thick;
    if (opening) memcpy(p->opening, opening, 12);
    else memset(p->opening, 0, sizeof p->opening); // (a solid wall: the default has a door)
    if (col) memcpy(p->color, col, 12);
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = at->parent = P.actor, at->kind = ATTACH_PROCGEN;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    return e;
}

static void facility_clear(World* w) {
    for (uint32_t i = 0; i < P.nroom_ents; i++)
        if (ecs_alive(w, P.rooms_ents[i])) ecs_destroy(w, P.rooms_ents[i]);
    P.nroom_ents = 0;
}
static void keep(Entity e) {
    if (!e) return;
    if (P.nroom_ents >= P.room_ents_cap) { // (an edit, not a frame: grown twice as big)
        uint32_t c = P.room_ents_cap ? P.room_ents_cap * 2u : 4096u;
        Entity* ne = realloc(P.rooms_ents, sizeof(Entity) * c);
        if (!ne) return;
        P.rooms_ents = ne, P.room_ents_cap = c;
    }
    P.rooms_ents[P.nroom_ents++] = e;
}

static void facility_build(World* w, const CProcWorld* cfg, const CTransform* at) {
    facility_clear(w);
    // (2026-11-25, item 20) the rooms asked: a number typed past the Details' slider is the number made
    uint32_t want = cfg->rooms ? cfg->rooms : 1u;
    if (want > FACILITY_ROOMS_MAX) {
        printf("[RealEngine] Facility: %u rooms asked, %u made: past that a level can't hold their actors (about 9 a room)\n", want, FACILITY_ROOMS_MAX);
        want = FACILITY_ROOMS_MAX;
    } else if (want > FACILITY_ROOMS_SLOW)
        printf("[RealEngine] Facility: %u rooms (about %u actors): past %u it may get slow to build and to draw\n", want, want * 9u, FACILITY_ROOMS_SLOW);
    Room* r = malloc(sizeof(Room) * want);
    RoomGrid g = { 0 };
    if (!r || !grid_make(&g, (int)want)) {
        free(r);
        printf("[RealEngine] Facility: no memory for %u rooms\n", want);
        return;
    }
    int n = facility_layout(cfg->seed, (int)want, cfg->loop_chance, r);
    for (int i = 0; i < n; i++) grid_put(&g, r, i);
    float S = cfg->room_size, H = cfg->room_height, T = 20.0f;
    const float wall[3] = { 0.55f, 0.56f, 0.58f }, floorc[3] = { 0.32f, 0.33f, 0.35f }, ceilc[3] = { 0.25f, 0.25f, 0.27f };
    // (2026-11-25, item 19: the user's video, light through the walls far away) each wall goes T/2 into the floor and the
    // ceiling instead of touching them face to face: the touching faces left seams the GI's rays and texels went through
    // (the sky's light along every wall-ceiling and wall-floor junction, a bright line up the three-way corners). The
    // overlaps are inside the slabs, nothing seen changes (the slabs stay S: wider, two rooms' would overlap in a doorway,
    // seen, fighting for the depth). The door keeps its 240 cm above the floor
    float door[3] = { 180.0f, 240.0f + T * 0.5f, 0.0f };
    int maxd = 1;
    for (int i = 0; i < n; i++) maxd = r[i].depth > maxd ? r[i].depth : maxd;
    for (int i = 0; i < n; i++) {
        float cx = at->location[0] + (float)r[i].x * S, cy = at->location[1] + (float)r[i].y * S, cz = at->location[2];
        // floor and ceiling (a slab each)
        keep(proto(w, PROTO_BOX, (float[3]){ cx, cy, cz - T }, 0, (float[3]){ S, S, T }, T, NULL, floorc));
        keep(proto(w, PROTO_BOX, (float[3]){ cx, cy, cz + H }, 0, (float[3]){ S, S, T }, T, NULL, ceilc));
        // the walls: +x and +y always (each shared side once), -x and -y where there is no room
        for (int k = 0; k < 4; k++) {
            static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            int j = grid_at(&g, r, r[i].x + d[k][0], r[i].y + d[k][1]);
            if ((k == 1 || k == 3) && j >= 0) continue;
            float wc[3] = { cx + (float)d[k][0] * S * 0.5f, cy + (float)d[k][1] * S * 0.5f, cz - T * 0.5f }; // (T/2 into the floor)
            float yaw = k < 2 ? 90.0f : 0.0f;
            float ws[3] = { S + T, T, H + T }; // (and T/2 into the ceiling)
            keep(proto(w, PROTO_WALL, wc, yaw, ws, T, r[i].door[k] ? door : NULL, wall));
        }
        // a light a room (warmer the deeper), items, creatures deeper in
        Entity l = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_ATTACH));
        if (l) {
            CTransform* lt = ecs_get(w, l, COMP_TRANSFORM);
            lt->location[0] = cx + S * 0.28f, lt->location[1] = cy + S * 0.18f, lt->location[2] = cz + H - 30.0f; // (off the middle: no one stands under it)
            lt->scale[0] = lt->scale[1] = lt->scale[2] = 1.0f;
            lt->flags = MOBILITY_STATIC;
            CLight* li = ecs_get(w, l, COMP_LIGHT);
            float f = (float)r[i].depth / (float)maxd;
            // (2026-11-25, item 19) its candela by the room's floor: 100 lux on it (a dim corridor's, Unreal's interiors
            // start there) through a utilization of 0.5 (walls, ceiling, the light's height): I = E A / (0.5 * 4 pi). The fixed
            // 260 cd (a household bulb) left a 12 m room at ~7 lux: black under an exposure kept from adapting (a Post
            // Process's Min EV100), and the dynamic GI's leaks had been lighting it; after Build Lighting, without them, it
            // looked like the bake had gone wrong
            float floor_m2 = (S / 100.0f) * (S / 100.0f);
            li->type = LIGHT_POINT, li->intensity = 100.0f * floor_m2 / (0.5f * 4.0f * 3.14159265f), li->attenuation_radius = S * 1.1f, li->source_radius = 40.0f;
            li->color[0] = 1.0f, li->color[1] = 0.9f - f * 0.35f, li->color[2] = 0.75f - f * 0.5f;
            li->cast_shadows = 1, li->affects_world = 1, li->ext = 1;
            li->indirect_intensity = li->specular_scale = li->volumetric_scattering = 1.0f, li->temperature = 6500.0f;
            CAttach* la = ecs_get(w, l, COMP_ATTACH);
            la->owner = la->parent = P.actor, la->kind = ATTACH_PROCGEN;
            keep(l);
        }
        uint64_t q = hash3(r[i].x, r[i].y, cfg->seed + 55u);
        int items = (int)(q % 3u);
        for (int k = 0; k < items; k++) { // scrap: small boxes to find (a Blueprint's Spawn Actor can replace them)
            uint64_t z = hash3(r[i].x * 8 + k, r[i].y, cfg->seed + 66u);
            float ic[3] = { cx + ((float)(z & 255u) / 255.0f - 0.5f) * S * 0.6f, cy + ((float)((z >> 8) & 255u) / 255.0f - 0.5f) * S * 0.6f, cz + 20.0f };
            const float gold[3] = { 0.85f, 0.62f, 0.2f };
            Entity it = block(w, ic, (float[3]){ 40, 40, 40 }, SHAPE_CUBE, gold, 0.3f);
            if (it) ((CStaticMesh*)ecs_get(w, it, COMP_STATIC_MESH))->metallic = 1.0f, keep(it);
        }
        if (P.playing && i > 0 && P.ncreatures < MAX_CREATURES && (double)((q >> 16) & 0xFFFF) / 65536.0 < cfg->creature_density * 0.15 * (double)r[i].depth / (double)maxd) {
            Entity cr = creature(w, cfg, (float[3]){ cx, cy, cz }, (float)((q >> 40) % 360));
            if (cr) P.creatures[P.ncreatures++] = cr, keep(cr);
        }
    }
    { // (the tests: the deepest room, farthest from the entrance through the doors)
        int di = 0;
        for (int i = 1; i < n; i++)
            if (r[i].depth > r[di].depth) di = i;
        P.deep[0] = at->location[0] + (float)r[di].x * S, P.deep[1] = at->location[1] + (float)r[di].y * S, P.deep[2] = at->location[2];
        P.deep_side = S, P.deep_ok = true;
    }
    P.stats.rooms = (uint32_t)n, P.stats.creatures = P.ncreatures;
    free(r), free(g.slot);
    bp_boxes_changed();
}

// --- The frame ---
static uint32_t cfg_hash(const CProcWorld* c) { // (its numbers only: a rebase moves the actor, not the world)
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)c;
    for (size_t i = 0; i < offsetof(CProcWorld, built); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void drop_all(World* w) {
    for (uint32_t i = 0; i < MAX_CHUNKS; i++)
        if (P.chunks[i].used) chunk_free(w, &P.chunks[i]);
    memset(P.table, 0, sizeof P.table);
    facility_clear(w);
    for (uint32_t i = 0; i < P.ncreatures; i++)
        if (ecs_alive(w, P.creatures[i])) ecs_destroy(w, P.creatures[i]);
    P.ncreatures = 0;
    if (P.water && ecs_alive(w, P.water)) ecs_destroy(w, P.water);
    P.water = ENTITY_NONE;
    P.focus_ok = false;
    memset(&P.stats, 0, sizeof P.stats);
}

// Every entity the generator made (its children), and its tracking, gone
void procgen_purge(World* w) {
    static Entity dead[16384];
    for (;;) {
        uint32_t n = 0;
        for (uint32_t ai = 0; ai < w->archetype_count && n < 16384; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
            const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t r = 0; r < ar->count && n < 16384; r++)
                if (at[r].kind == ATTACH_PROCGEN) dead[n++] = ents[r];
        }
        for (uint32_t i = 0; i < n; i++) ecs_destroy(w, dead[i]);
        if (n < 16384) break;
    }
    for (uint32_t i = 0; i < P.ncreatures; i++)
        if (P.w == w && ecs_alive(w, P.creatures[i])) ecs_destroy(w, P.creatures[i]);
    for (uint32_t i = 0; i < MAX_CHUNKS; i++) P.chunks[i].used = 0, P.chunks[i].n = 0, P.chunks[i].ncreature = 0;
    memset(P.table, 0, sizeof P.table);
    P.nroom_ents = 0, P.ncreatures = 0, P.water = ENTITY_NONE, P.actor = ENTITY_NONE, P.w = NULL, P.focus_ok = false;
}

void procgen_begin_play(World* w) {
    P.playing = true;
    drop_all(w); // (made again as the play's: creatures too)
    P.actor = ENTITY_NONE, P.w = NULL;
    memset(P.origin, 0, sizeof P.origin);
}
void procgen_end_play(World* w) {
    (void)w;
    // (the editor's world comes back from its snapshot: the play's entities are gone with it)
    for (uint32_t i = 0; i < MAX_CHUNKS; i++) P.chunks[i].used = 0, P.chunks[i].n = 0, P.chunks[i].ncreature = 0;
    memset(P.table, 0, sizeof P.table);
    P.nroom_ents = 0, P.ncreatures = 0, P.water = ENTITY_NONE, P.actor = ENTITY_NONE, P.w = NULL, P.playing = false;
    memset(P.origin, 0, sizeof P.origin);
    memset(&P.stats, 0, sizeof P.stats);
}

void procgen_update(World* w, const float focus[3], bool playing) {
    double t0 = re_time_seconds();
    P.playing = playing;
    // the world's actor (the first)
    Entity act = ENTITY_NONE;
    CProcWorld* cfg = NULL;
    for (uint32_t ai = 0; ai < w->archetype_count && !act; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_PROC_WORLD)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        CProcWorld* col = ecs_arch_column(ar, COMP_PROC_WORLD);
        for (uint32_t r = 0; r < ar->count && !act; r++) // ((Q15) a PCG Volume is a PCG component, not a world: pcg.h)
            if (col[r].mode != PROCGEN_PCG) act = ecs_arch_entities(ar)[r], cfg = &col[r];
    }
    if (!act) {
        if (P.actor && P.w == w) procgen_purge(w);
        P.actor = ENTITY_NONE, P.w = NULL;
        return;
    }
    if (P.w == w && !playing && w->revision != P.revision) procgen_purge(w); // (an undo, a level loaded: made again)
    const CTransform* at = ecs_get(w, act, COMP_TRANSFORM);
    if (!P.pool && !(P.pool = calloc((size_t)MAX_CHUNKS * CHUNK_ENTS, sizeof(Entity)))) return; // (once)
    uint32_t h = cfg_hash(cfg);
    // (the actor moved by hand in the editor: its world moves with it; a rebase moves it by the origin's change)
    bool moved = P.actor == act && !playing &&
                 (fabs(at->location[0] - (P.base[0] - P.origin[0])) > 1.0 || fabs(at->location[1] - (P.base[1] - P.origin[1])) > 1.0 ||
                  fabs(at->location[2] - (P.base[2] - P.origin[2])) > 1.0);
    if (P.actor != act || P.w != w || h != P.cfg_hash || moved) { // a new world (or its numbers changed): everything made again
        procgen_purge(w); // (and whatever a restored snapshot brought back of an older one)
        P.actor = act, P.w = w, P.cfg_hash = h;
        P.g = (Gen){ cfg->seed, fmax(cfg->block, 25.0), fmax(cfg->step, 5.0), cfg->height, cfg->water_level, fmax(cfg->biome_scale, 1000.0) };
        P.mode = (int)cfg->mode;
        memcpy(P.base, at->location, 12);
        memset(P.origin, 0, sizeof P.origin);
        for (uint32_t i = 0; i < MAX_CHUNKS; i++) P.chunks[i].ents = P.pool + (size_t)i * CHUNK_ENTS, P.chunks[i].used = 0;
        P.facility_hash = 0;
    }
    if (P.mode == PROCGEN_FACILITY) {
        if (!P.facility_hash) {
            double tb = re_time_seconds();
            facility_build(w, cfg, at), P.facility_hash = h;
            P.stats.ms_peak = (re_time_seconds() - tb) * 1000.0;
        }
        P.stats.ms = (re_time_seconds() - t0) * 1000.0;
        P.revision = w->revision;
        return;
    }
    if (!focus) return;
    // far from the origin: the world moved back (while playing; the editor's preview stays put)
    if (playing && cfg->rebase_distance > 0.0f) {
        float dx = focus[0], dy = focus[1];
        if (dx * dx + dy * dy > cfg->rebase_distance * cfg->rebase_distance) {
            float cs = chunk_cm();
            float d[3] = { -floorf(dx / cs) * cs, -floorf(dy / cs) * cs, 0.0f };
            rebase(w, d);
            float nf[3] = { focus[0] + d[0], focus[1] + d[1], focus[2] };
            procgen_update(w, nf, playing);
            return;
        }
    }
    // the water: a Water Body under the player at the sea's level
    if (!P.water || !ecs_alive(w, P.water)) {
        P.water = water_spawn(w, (float[3]){ focus[0], focus[1], P.base[2] + (float)P.g.water - (float)P.origin[2] }, 200000.0f, 200000.0f);
        if (P.water) {
            ecs_add(w, P.water, COMP_ATTACH);
            CAttach* wa = ecs_get(w, P.water, COMP_ATTACH);
            wa->owner = wa->parent = act, wa->kind = ATTACH_PROCGEN;
        }
    }
    CTransform* wt = P.water ? ecs_get(w, P.water, COMP_TRANSFORM) : NULL;
    float cs = chunk_cm();
    if (wt && (fabsf(wt->location[0] - focus[0]) > cs * 4.0f || fabsf(wt->location[1] - focus[1]) > cs * 4.0f))
        wt->location[0] = floorf(focus[0] / cs) * cs, wt->location[1] = floorf(focus[1] / cs) * cs, ecs_touch(w, P.water);
    // the chunks wanted around the focus (endless coordinates), their detail by distance; made nearest first
    double ex = (double)focus[0] + P.origin[0] - P.base[0], ey = (double)focus[1] + P.origin[1] - P.base[1];
    int64_t fcx = (int64_t)floor(ex / cs), fcy = (int64_t)floor(ey / cs);
    float view = fmaxf(cfg->view_distance, cs * 3.0f);
    if (!playing) view = fminf(view, 60000.0f); // (the editor's preview: the nearest 600 m)
    if (P.focus_ok && fcx == P.focus_cx && fcy == P.focus_cy && !P.pending && view == P.view) {
        P.stats.ms = (re_time_seconds() - t0) * 1000.0;
        P.revision = w->revision;
        return; // (nothing to do: the same chunk, everything made)
    }
    P.focus_cx = fcx, P.focus_cy = fcy, P.focus_ok = true, P.view = view;
    int R = (int)ceilf(view / cs);
    if (R > 60) R = 60;
    P.frame++;
    P.pending = false;
    // the chunks wanted that are missing or at another detail: rings outward (the nearest first), a batch a frame
    static HeightJob jobs[JOBS];
    int nj = 0;
    for (int ring = 0; ring <= R; ring++)
        for (int dy = -ring; dy <= ring; dy++)
            for (int dx = -ring; dx <= ring; dx++) {
                if (abs(dx) != ring && abs(dy) != ring) continue;
                double dist = sqrt((double)(dx * dx + dy * dy));
                if (dist * cs > view) continue;
                int lod = lod_for(dist);
                int64_t cx = fcx + dx, cy = fcy + dy;
                Chunk* c = chunk_find(cx, cy);
                if (c) c->seen = P.frame; // (kept at its old detail until its turn)
                if (c && c->lod == lod) continue;
                if (nj >= JOBS) {
                    P.pending = true;
                    continue;
                }
                jobs[nj].cx = cx, jobs[nj].cy = cy, jobs[nj].lod = lod, jobs[nj].old = c, nj++;
            }
    grad_init();
    if (nj > 1) re_parallel_for(nj, heights, jobs);
    else if (nj == 1) heights(jobs, 0);
    // their entities, within the frame's budget (made new, the old detail dropped after: no hole meanwhile)
    double budget_end = re_time_seconds() + (playing ? 0.0025 : 0.008);
    for (int k = 0; k < nj; k++) {
        if (k > 0 && re_time_seconds() > budget_end) {
            P.pending = true;
            break;
        }
        Chunk* fresh = NULL;
        for (uint32_t i = 0; i < MAX_CHUNKS && !fresh; i++)
            if (!P.chunks[i].used) fresh = &P.chunks[i];
        if (!fresh) {
            P.pending = true;
            break;
        }
        fresh->cx = jobs[k].cx, fresh->cy = jobs[k].cy, fresh->lod = (uint8_t)jobs[k].lod, fresh->used = 1, fresh->seen = P.frame;
        chunk_make(w, cfg, fresh, jobs[k].hh, jobs[k].bb);
        if (jobs[k].old) jobs[k].old->seen = 0, chunk_free(w, jobs[k].old);
        table_rebuild();
    }
    // the chunks out of view go (their creatures with them)
    for (uint32_t i = 0; i < MAX_CHUNKS; i++)
        if (P.chunks[i].used && P.chunks[i].seen != P.frame) {
            for (uint32_t k = 0; k < P.chunks[i].ncreature; k++)
                for (uint32_t j = 0; j < P.ncreatures; j++)
                    if (P.creatures[j] == P.chunks[i].creatures[k]) P.creatures[j] = P.creatures[--P.ncreatures];
            chunk_free(w, &P.chunks[i]);
        }
    table_rebuild();
    P.stats.chunks = P.stats.columns = P.stats.trees = P.stats.structures = 0;
    for (uint32_t i = 0; i < MAX_CHUNKS; i++)
        if (P.chunks[i].used)
            P.stats.chunks++, P.stats.columns += P.chunks[i].n - P.chunks[i].ntrees * 2u, P.stats.trees += P.chunks[i].ntrees,
                P.stats.structures += P.chunks[i].nstruct;
    P.stats.creatures = P.ncreatures;
    P.stats.ms = (re_time_seconds() - t0) * 1000.0;
    if (P.stats.ms > P.stats.ms_peak) P.stats.ms_peak = P.stats.ms;
    P.revision = w->revision;
}

// --- The self-test ---
int procgen_selftest(void) {
    int fails = 0;
    Gen g = { 1337u, 200.0, 50.0, 6000.0, 0.0, 80000.0 };
    // the same point twice, the same height; far points (100 km and 10 000 km out) still varied and in range
    double a = terrain_raw(&g, 12345.0, -9876.0, NULL), b = terrain_raw(&g, 12345.0, -9876.0, NULL);
    double lo = 1e30, hi = -1e30;
    int count[BIOME_COUNT] = { 0 };
    for (int i = 0; i < 4000; i++) {
        double x = (double)(i % 64) * 37000.0 + 1e9 * (i >= 2000), y = (double)(i / 64) * 41000.0 + 1e7;
        int bi;
        double h = terrain_raw(&g, x, y, &bi);
        lo = fmin(lo, h), hi = fmax(hi, h), count[bi]++;
    }
    printf("[RealEngine] procgen: heights %.0f .. %.0f m over 4000 points (100 km and 10 000 km out); biomes", lo / 100.0, hi / 100.0);
    int kinds = 0;
    for (int k = 0; k < BIOME_COUNT; k++) printf(" %s %d", g_biome_names[k], count[k]), kinds += count[k] > 0;
    printf("\n");
    if (a != b || hi - lo < 3000.0 || kinds < 5) printf("[RealEngine] FAIL: procgen: the terrain's noise\n"), fails++;
    // neighboring points 1 m apart: no jumps (a continuous ground)
    double jump = 0.0;
    for (int i = 0; i < 1000; i++) {
        double x = 5e9 + (double)i * 100.0;
        jump = fmax(jump, fabs(terrain_raw(&g, x + 100.0, 777.0, NULL) - terrain_raw(&g, x, 777.0, NULL)));
    }
    printf("[RealEngine] procgen: the steepest meter at 50 000 km out: %.1f m\n", jump / 100.0);
    if (jump > 600.0) printf("[RealEngine] FAIL: procgen: a cliff from the noise's precision\n"), fails++;
    // a facility: every room reached from the entrance through its doors
    static Room r[64];
    int n = facility_layout(42u, 30, 0.2f, r);
    int seen[64] = { 0 }, queue[64], qh = 0, qt = 0, reached = 0;
    seen[0] = 1, queue[qt++] = 0;
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    while (qh < qt) {
        int i = queue[qh++];
        reached++;
        for (int k = 0; k < 4; k++) {
            if (!r[i].door[k]) continue;
            int j = room_at(r, n, r[i].x + d[k][0], r[i].y + d[k][1]);
            if (j >= 0 && !seen[j]) seen[j] = 1, queue[qt++] = j;
        }
    }
    printf("[RealEngine] procgen: a facility of %d rooms, %d reached from the entrance\n", n, reached);
    if (n != 30 || reached != n) printf("[RealEngine] FAIL: procgen: the facility's rooms\n"), fails++;
    // (2026-11-25, item 20 of the user's list: "digitando um número muito alto, o sistema responde a esse número") 3x
    // the Details' slider (Rooms 1..128): the layout makes exactly that many, every one reached through its doors, and
    // the build keeps every actor it made (the old arrays stopped at 128 rooms and 4096 actors)
    {
        enum { BIG = 384 };
        Room* rb = calloc(BIG, sizeof(Room));
        int *sb = calloc(BIG, sizeof(int)), *qb = calloc(BIG, sizeof(int));
        int nb = rb && sb && qb ? facility_layout(7u, BIG, 0.2f, rb) : 0, reached_b = 0;
        if (nb > 0) {
            int h0 = 0, t0 = 0;
            sb[0] = 1, qb[t0++] = 0;
            while (h0 < t0) {
                int i = qb[h0++];
                reached_b++;
                for (int k = 0; k < 4; k++) {
                    if (!rb[i].door[k]) continue;
                    int j = room_at(rb, nb, rb[i].x + d[k][0], rb[i].y + d[k][1]);
                    if (j >= 0 && j < BIG && !sb[j]) sb[j] = 1, qb[t0++] = j;
                }
            }
        }
        free(rb), free(sb), free(qb);
        World fw;
        memset(&fw, 0, sizeof fw);
        ecs_init(&fw);
        CProcWorld cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.mode = PROCGEN_FACILITY, cfg.rooms = BIG, cfg.seed = 7u, cfg.room_size = 800.0f, cfg.room_height = 320.0f, cfg.loop_chance = 0.2f;
        CTransform at;
        memset(&at, 0, sizeof at);
        at.scale[0] = at.scale[1] = at.scale[2] = 1.0f;
        P.actor = ENTITY_NONE;
        facility_build(&fw, &cfg, &at);
        uint32_t built = P.stats.rooms, kept = P.nroom_ents, alive = fw.alive;
        // (2026-11-25, item 19: "o bake da área interior bugado") the rooms' lamps sized by their floor (100 lux through 0.5:
        // ~1019 cd in an 8 m room): the fixed 260 cd left a room dark once Build Lighting took the GI's leaks away
        float lamp = 0.0f;
        for (uint32_t i = 1; i < fw.slot_count && lamp == 0.0f; i++) {
            Entity e = ecs_entity_at(&fw, i);
            const CLight* li = e ? ecs_get(&fw, e, COMP_LIGHT) : NULL;
            const CAttach* at2 = li ? ecs_get(&fw, e, COMP_ATTACH) : NULL;
            if (at2 && at2->kind == ATTACH_PROCGEN) lamp = li->intensity;
        }
        float want_cd = 100.0f * (cfg.room_size / 100.0f) * (cfg.room_size / 100.0f) / (0.5f * 4.0f * 3.14159265f);
        printf("[RealEngine] procgen: the facility's lamp %.0f cd in a room of %.0f m (100 lux on its floor: %.0f cd)\n", lamp, cfg.room_size / 100.0f, want_cd);
        if (fabsf(lamp - want_cd) > 1.0f) printf("[RealEngine] FAIL: procgen: the facility's lamps aren't sized by their room\n"), fails++;
        facility_clear(&fw);
        uint32_t left = fw.alive;
        ecs_shutdown(&fw);
        printf("[RealEngine] procgen: a facility of %d rooms asked (3x the slider's 128): %d laid out, %d reached; built %u rooms, %u actors kept of %u, %u left after clearing\n",
               BIG, nb, reached_b, built, kept, alive, left);
        if (nb != BIG || reached_b != nb || built != BIG || kept != alive || left != 0)
            printf("[RealEngine] FAIL: procgen: a facility past the slider's range must have the rooms asked\n"), fails++;
    }
    printf("[RealEngine] procgen self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
