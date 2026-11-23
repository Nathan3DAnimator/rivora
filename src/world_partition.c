// World Partition (world_partition.h; notas-dev/mundos-grandes.md): grid streaming, Data Layers and HLOD while playing.
// At wp_begin each managed actor (spatially loaded, or in a Data Layer) is written as a record (its mask, then each of
// its components' bytes) into one arena; the cells and records are fixed for the play. Each frame the cells within the
// Loading Range of the focus are wanted (out past it plus a quarter cell: no flapping on a border); the records whose
// state differs go into a queue, done within the Streaming Budget (ms), unloads first, then the loads nearest first.
// The proxies (near HLOD) and the region meshes (far HLOD) are actors made once at wp_begin, only shown or hidden.
#include "world_partition.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "material.h"
#include "mesh_build.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "meshoptimizer.h"

const char* const g_wp_layer_state_names[WP_LAYER_STATE_COUNT] = { "Unloaded", "Loaded", "Activated" };
const char* const g_wp_spatial_names[WP_SPATIAL_COUNT] = { "Auto", "Yes", "No" };

typedef struct Rec {
    uint32_t off, size; // its bytes in W.blob
    uint64_t mask;
    int32_t cell;       // W.cells index; -1: not spatial (in Data Layers only)
    uint32_t layers;
    Entity e;           // in the world (ENTITY_NONE: out)
    Entity proxy;       // its near HLOD (ENTITY_NONE: none)
    uint8_t queued, proxy_shown, hidden, shown; // shown: in and its layers Activated (not hidden by them)
} Rec;
typedef struct Cell {
    int32_t cx, cy;
    uint32_t nrec, region; // region: W.regions index (UINT32_MAX: none)
    float dist;            // to the focus (cm, 2D, to the cell's square)
    float eta;             // (Q27.7) s until the focus's path reaches it (0: in range now): the loads' order
    uint8_t want, in;      // wanted by the range; in: every one of its records that may be in, is
} Cell;
typedef struct Region { int32_t rx, ry; Entity e; uint8_t shown; } Region;

static struct {
    bool active;
    World* w;
    CWorldPartition cfg;
    uint8_t* blob;
    size_t blob_size;
    Rec* recs;
    uint32_t nrecs;
    Cell* cells;
    uint32_t ncells;
    int32_t* hash; // cell key -> index (open addressing, hash_cap slots)
    uint32_t hash_cap;
    Region* regions;
    uint32_t nregions;
    uint32_t* queue; // record indices
    uint32_t nqueue;
    bool sort_queue;
    uint8_t layer_state[WP_MAX_LAYERS];
    uint32_t layers_changed; // bits since the last update
    WpCellView* view;
    WpStats st;
    // (Q27.7) the focus's velocity (cm/s, smoothed over ~0.3 s) for the cells ahead
    float prev[2], vel[2];
    double prev_t;
    bool have_prev;
} W;

Entity wp_settings(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & COMP_BIT(COMP_WORLD_PARTITION)) && a->count) return ecs_arch_entities(a)[0];
    }
    return ENTITY_NONE;
}

void wp_cell_of(float cell_size, const float p[3], int32_t* cx, int32_t* cy) {
    float c = cell_size > 100.0f ? cell_size : 100.0f;
    *cx = (int32_t)floorf(p[0] / c), *cy = (int32_t)floorf(p[1] / c);
}

// Auto: what is in one place and only matters near it (props, local lights, pickups, enemies...) streams; what the whole
// level needs (sun, sky, fog, landscape, the player and the game's rules, volumes that cover it all) stays
static bool auto_spatial(World* w, Entity e, uint64_t m) {
    const uint64_t global = COMP_BIT(COMP_CAMERA) | COMP_BIT(COMP_PLAYER_START) | COMP_BIT(COMP_FOG) | COMP_BIT(COMP_CLOUD) |
                            COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_POST_PROCESS) | COMP_BIT(COMP_CROWD) | COMP_BIT(COMP_LANDSCAPE) |
                            COMP_BIT(COMP_FOLIAGE) | COMP_BIT(COMP_PROC_WORLD) | COMP_BIT(COMP_RAIN) | COMP_BIT(COMP_WATER) |
                            COMP_BIT(COMP_RAIL) | COMP_BIT(COMP_RIVER) | COMP_BIT(COMP_NAV) | COMP_BIT(COMP_GAME_RULES) |
                            COMP_BIT(COMP_QUEST_LOG) | COMP_BIT(COMP_HUD_LAYOUT) | COMP_BIT(COMP_SAVEABLE) | COMP_BIT(COMP_INVENTORY) |
                            COMP_BIT(COMP_INTERACTOR) | COMP_BIT(COMP_SPLAT) | COMP_BIT(COMP_AUDIO_VOLUME) | COMP_BIT(COMP_VEHICLE) |
                            COMP_BIT(COMP_CONSTRAINT) | COMP_BIT(COMP_WORLD_PARTITION) | COMP_BIT(COMP_OUTLINER_FOLDER) |
                            COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_STREAMED) | COMP_BIT(COMP_CHECKPOINT);
    if ((m & global) || !(m & COMP_BIT(COMP_TRANSFORM))) return false;
    if (m & COMP_BIT(COMP_LIGHT)) {
        const CLight* l = ecs_get(w, e, COMP_LIGHT);
        if (l->type == LIGHT_DIRECTIONAL || l->type == LIGHT_SKY) return false;
    }
    return true;
}

bool wp_actor_spatial(World* w, Entity e) {
    uint64_t m = ecs_mask(w, e);
    if (m & COMP_BIT(COMP_PARTITION)) {
        const CPartition* p = ecs_get(w, e, COMP_PARTITION);
        if (p->spatial == WP_SPATIAL_YES) return (m & COMP_BIT(COMP_TRANSFORM)) && !(m & (COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_WORLD_PARTITION)));
        if (p->spatial == WP_SPATIAL_NO) return false;
    }
    return auto_spatial(w, e, m);
}
uint32_t wp_actor_layers(World* w, Entity e) {
    const CPartition* p = (ecs_mask(w, e) & COMP_BIT(COMP_PARTITION)) ? ecs_get(w, e, COMP_PARTITION) : NULL;
    return p ? p->layers : 0u;
}

// --- cells by key ---
static uint32_t key_hash(int32_t cx, int32_t cy) { return ((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u); }
static int32_t cell_find(int32_t cx, int32_t cy) {
    if (!W.hash_cap) return -1;
    for (uint32_t h = key_hash(cx, cy) & (W.hash_cap - 1);; h = (h + 1) & (W.hash_cap - 1)) {
        int32_t k = W.hash[h];
        if (k < 0) return -1;
        if (W.cells[k].cx == cx && W.cells[k].cy == cy) return k;
    }
}
static int32_t cell_add(int32_t cx, int32_t cy) {
    int32_t k = cell_find(cx, cy);
    if (k >= 0) return k;
    k = (int32_t)W.ncells++;
    W.cells[k] = (Cell){ cx, cy, 0, UINT32_MAX, 0.0f, 0, 0 };
    for (uint32_t h = key_hash(cx, cy) & (W.hash_cap - 1);; h = (h + 1) & (W.hash_cap - 1))
        if (W.hash[h] < 0) {
            W.hash[h] = k;
            break;
        }
    return k;
}

// --- records: an actor as bytes and back ---
static size_t rec_bytes(uint64_t mask) {
    size_t n = 0;
    for (int c = 0; c < COMP_COUNT; c++)
        if (mask & COMP_BIT(c)) n += g_components[c].size;
    return n;
}
static void rec_write(World* w, Entity e, uint64_t mask, uint8_t* dst) {
    for (int c = 0; c < COMP_COUNT; c++)
        if (mask & COMP_BIT(c)) {
            memcpy(dst, ecs_get(w, e, (ComponentId)c), g_components[c].size);
            dst += g_components[c].size;
        }
}
static const void* rec_comp(const Rec* r, ComponentId want) {
    const uint8_t* p = W.blob + r->off;
    for (int c = 0; c < COMP_COUNT; c++)
        if (r->mask & COMP_BIT(c)) {
            if (c == (int)want) return p;
            p += g_components[c].size;
        }
    return NULL;
}

static bool layers_allow(uint32_t layers, int at_least) {
    for (uint32_t i = 0; i < WP_MAX_LAYERS && i < W.cfg.nlayers; i++)
        if ((layers >> i & 1u) && W.layer_state[i] < at_least) return false;
    return true;
}
static bool rec_want(const Rec* r) { return (r->cell < 0 || W.cells[r->cell].want) && layers_allow(r->layers, WP_LAYER_LOADED); }
static bool rec_shown(const Rec* r) { return !r->hidden && layers_allow(r->layers, WP_LAYER_ACTIVATED); }

static void set_hidden(World* w, Entity e, bool hidden) {
    CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (!t) return;
    uint32_t f = hidden ? (t->flags | TRANSFORM_HIDDEN_GAME) : (t->flags & ~TRANSFORM_HIDDEN_GAME);
    if (f != t->flags) t->flags = f, ecs_touch(w, e);
}

static void rec_load(World* w, Rec* r, bool playing) {
    Entity e = ecs_create(w, r->mask);
    if (e == ENTITY_NONE) return;
    const uint8_t* p = W.blob + r->off;
    for (int c = 0; c < COMP_COUNT; c++)
        if (r->mask & COMP_BIT(c)) {
            memcpy(ecs_get(w, e, (ComponentId)c), p, g_components[c].size);
            p += g_components[c].size;
        }
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    r->shown = rec_shown(r);
    if (t && !r->shown) t->flags |= TRANSFORM_HIDDEN_GAME;
    CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    if (b) {
        b->inst = 0;
        bp_components_build(w, e, NULL);
        if (playing) bp_start_instance(w, e); // its BeginPlay
    }
    ecs_touch(w, e);
    if (playing) bp_box_spawned(e); // (its collision joins the Characters' at the next tick)
    r->e = e;
    W.st.loads++;
}
static void rec_unload(World* w, Rec* r) {
    if (ecs_alive(w, r->e)) bp_destroy_actor(w, r->e, true); // (its End Play; its components go with it)
    r->e = ENTITY_NONE;
    W.st.unloads++;
}

static void queue_push(uint32_t i) {
    if (W.recs[i].queued) return;
    W.recs[i].queued = 1;
    W.queue[W.nqueue++] = i;
    W.sort_queue = true;
}

// the queue's order: unloads first, then the loads by their cell's distance
static int queue_cmp(const void* a, const void* b) {
    const Rec *ra = &W.recs[*(const uint32_t*)a], *rb = &W.recs[*(const uint32_t*)b];
    int la = rec_want(ra), lb = rec_want(rb);
    if (la != lb) return la - lb;
    // (Q27.7) the loads by when the focus gets there (the cells ahead of a car before the ones beside it), then the nearest
    float ea = ra->cell >= 0 ? W.cells[ra->cell].eta : 0.0f, eb = rb->cell >= 0 ? W.cells[rb->cell].eta : 0.0f;
    if (ea != eb) return (ea > eb) - (ea < eb);
    float da = ra->cell >= 0 ? W.cells[ra->cell].dist : 0.0f, db = rb->cell >= 0 ? W.cells[rb->cell].dist : 0.0f;
    return (da > db) - (da < db);
}

// --- HLOD actors: the proxies and the regions' meshes ---
static Entity mesh_actor(World* w, const float loc[3], const float rot[3], const float scale[3], const CStaticMesh* sm) {
    Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, loc, 12), memcpy(t->rotation, rot, 12), memcpy(t->scale, scale, 12);
    t->flags = MOBILITY_STATIC | TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_COLLISION | TRANSFORM_NO_ICON;
    *(CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH) = *sm;
    ecs_touch(w, e);
    return e;
}

static float rec_size(const CTransform* t, const CStaticMesh* sm) {
    float mn[3], mx[3], s = 0.0f;
    render_mesh_bounds(sm->mesh, mn, mx);
    for (int k = 0; k < 3; k++) s = fmaxf(s, (mx[k] - mn[k]) * fabsf(t->scale[k]));
    return s;
}

void wp_level_name(const char* level_path, char* out, size_t cap) {
    const char* p = level_path ? level_path : "";
    const char* base = strrchr(p, '/');
    const char* base2 = strrchr(p, '\\');
    if (base2 > base) base = base2;
    snprintf(out, cap, "%s", base ? base + 1 : p);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
    if (!out[0]) snprintf(out, cap, "Untitled"); // (a level never saved)
}

void wp_editor_layer_removed(World* w, int layer) {
    if (layer < 0 || layer >= WP_MAX_LAYERS) return;
    const uint32_t low = (1u << layer) - 1u;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_PARTITION)) || !a->count) continue;
        CPartition* p = ecs_arch_column(a, COMP_PARTITION);
        for (uint32_t i = 0; i < a->count; i++) p[i].layers = (p[i].layers & low) | ((p[i].layers >> 1) & ~low); // (the ones after it move down)
    }
}

void wp_hlod_dir(const char* content_root, const char* level_name, char* out, size_t cap) {
    snprintf(out, cap, "%s/HLOD/%s", content_root, level_name);
}

static void region_of(int32_t cx, int32_t cy, int32_t* rx, int32_t* ry) {
    int32_t n = W.cfg.hlod_region > 0 ? (int32_t)W.cfg.hlod_region : 4;
    *rx = cx >= 0 ? cx / n : -((-cx + n - 1) / n), *ry = cy >= 0 ? cy / n : -((-cy + n - 1) / n);
}

static void make_regions(World* w, const char* content_root, const char* level_name) {
    char dir[900];
    wp_hlod_dir(content_root, level_name, dir, sizeof dir);
    const float cell = W.cfg.cell_size, rs = cell * (float)(W.cfg.hlod_region > 0 ? W.cfg.hlod_region : 4);
    CStaticMesh sm = { 0 };
    sm.base_color[0] = sm.base_color[1] = sm.base_color[2] = 1.0f, sm.roughness = 0.75f, sm.cast_shadow = 1;
    sm.material = mat_vpath_hash("/Content/Materials/M_HLOD.rmat"), sm.uv_scale = 1.0f;
    for (uint32_t c = 0; c < W.ncells; c++) {
        int32_t rx, ry;
        region_of(W.cells[c].cx, W.cells[c].cy, &rx, &ry);
        uint32_t k = 0;
        while (k < W.nregions && (W.regions[k].rx != rx || W.regions[k].ry != ry)) k++;
        if (k == W.nregions) {
            char vp[300], disk[1000];
            snprintf(vp, sizeof vp, "/Content/HLOD/%s/r_%d_%d.rmesh", level_name, rx, ry);
            snprintf(disk, sizeof disk, "%s/r_%d_%d.rmesh", dir, rx, ry);
            uint32_t id = re_file_stat(disk, NULL, NULL, NULL) ? render_mesh_id_of(vp) : 0;
            W.regions[k] = (Region){ rx, ry, ENTITY_NONE, 0 };
            if (id) {
                sm.mesh = id;
                float loc[3] = { ((float)rx + 0.5f) * rs, ((float)ry + 0.5f) * rs, 0.0f }, rot[3] = { 0, 0, 0 }, one[3] = { 1, 1, 1 };
                W.regions[k].e = mesh_actor(w, loc, rot, one, &sm);
            }
            W.nregions++;
        }
        W.cells[c].region = k;
    }
}

// --- the play ---
void wp_begin(World* w, const char* level_path, const char* content_root, const float focus[3]) {
    wp_end(w);
    Entity s = wp_settings(w);
    if (s == ENTITY_NONE) return;
    const CWorldPartition* cfg = ecs_get(w, s, COMP_WORLD_PARTITION);
    if (!cfg->enabled) return;
    double t0 = re_time_seconds();
    memset(&W, 0, sizeof W);
    W.cfg = *cfg;
    if (W.cfg.cell_size < 100.0f) W.cfg.cell_size = 12800.0f;
    if (W.cfg.loading_range < W.cfg.cell_size * 0.5f) W.cfg.loading_range = W.cfg.cell_size * 0.5f;
    if (W.cfg.budget_ms <= 0.0f) W.cfg.budget_ms = 1.0f;
    for (uint32_t i = 0; i < WP_MAX_LAYERS; i++) W.layer_state[i] = i < W.cfg.nlayers ? (uint8_t)W.cfg.layers[i].initial : WP_LAYER_ACTIVATED;
    W.w = w;
    // the managed actors (top level: a Blueprint's components are made again with it)
    uint32_t cap = 0;
    size_t bytes = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = 0;
        size_t off = 0;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!a->count || (a->mask & (COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_WORLD_PARTITION) | COMP_BIT(COMP_OUTLINER_FOLDER))) ||
                !(a->mask & COMP_BIT(COMP_TRANSFORM)))
                continue;
            const Entity* ents = ecs_arch_entities(a);
            size_t rb = rec_bytes(a->mask);
            for (uint32_t i = 0; i < a->count; i++) {
                bool sp = wp_actor_spatial(w, ents[i]);
                uint32_t ly = wp_actor_layers(w, ents[i]) & ((W.cfg.nlayers >= 32 ? 0xFFFFFFFFu : (1u << W.cfg.nlayers) - 1u));
                if (!sp && !ly) continue;
                if (pass == 1) {
                    Rec* r = &W.recs[n];
                    memset(r, 0, sizeof *r);
                    r->off = (uint32_t)off, r->size = (uint32_t)rb, r->mask = a->mask, r->layers = ly, r->e = ents[i], r->proxy = ENTITY_NONE;
                    rec_write(w, ents[i], a->mask, W.blob + off);
                    const CTransform* t = ecs_get(w, ents[i], COMP_TRANSFORM);
                    r->hidden = (t->flags & TRANSFORM_HIDDEN_GAME) != 0;
                    r->cell = -1;
                    if (sp) {
                        int32_t cx, cy;
                        wp_cell_of(W.cfg.cell_size, t->location, &cx, &cy);
                        r->cell = cell_add(cx, cy);
                        W.cells[r->cell].nrec++;
                    } else W.st.layered++;
                }
                n++, off += rb;
            }
        }
        if (pass == 0) {
            cap = n, bytes = off;
            if (!n) return;
            W.blob = malloc(bytes ? bytes : 1); // (a play's start, not a frame)
            W.recs = calloc(n, sizeof *W.recs);
            W.queue = malloc(n * sizeof *W.queue);
            W.cells = calloc(n, sizeof *W.cells);
            W.regions = calloc(n, sizeof *W.regions);
            W.view = calloc(n, sizeof *W.view);
            W.hash_cap = 64;
            while (W.hash_cap < n * 2) W.hash_cap <<= 1;
            W.hash = malloc(W.hash_cap * sizeof *W.hash);
            if (!W.blob || !W.recs || !W.queue || !W.cells || !W.regions || !W.view || !W.hash) {
                printf("[RealEngine] World Partition: out of memory for %u actors\n", n);
                wp_end(w);
                return;
            }
            memset(W.hash, 0xFF, W.hash_cap * sizeof *W.hash);
        }
    }
    W.nrecs = cap, W.blob_size = bytes;
    W.active = true;
    // the HLOD: the proxies of the big meshes, the regions' merged meshes
    if (W.cfg.hlod) {
        char lname[128];
        wp_level_name(level_path, lname, sizeof lname);
        make_regions(w, content_root, lname);
        for (uint32_t i = 0; i < W.nrecs; i++) {
            Rec* r = &W.recs[i];
            if (r->cell < 0 || r->hidden || (r->mask & (COMP_BIT(COMP_BLUEPRINT) | COMP_BIT(COMP_RIGID_BODY))) || !(r->mask & COMP_BIT(COMP_STATIC_MESH)))
                continue;
            const CTransform* t = rec_comp(r, COMP_TRANSFORM);
            const CStaticMesh* sm = rec_comp(r, COMP_STATIC_MESH);
            if (rec_size(t, sm) < W.cfg.hlod_min_size) continue;
            r->proxy = mesh_actor(w, t->location, t->rotation, t->scale, sm);
            if (r->proxy != ENTITY_NONE) W.st.proxies++;
        }
        for (uint32_t k = 0; k < W.nregions; k++) W.st.regions += W.regions[k].e != ENTITY_NONE;
    }
    // what isn't wanted from the start leaves now (before the BeginPlays: they never run for it)
    wp_update(w, focus, false);
    for (uint32_t i = 0; i < W.nrecs; i++) {
        Rec* r = &W.recs[i];
        r->queued = 0;
        if (!rec_want(r)) {
            if (ecs_alive(w, r->e)) bp_destroy_actor(w, r->e, false);
            r->e = ENTITY_NONE;
        } else {
            bool sh = rec_shown(r);
            r->shown = sh;
            if (!sh) set_hidden(w, r->e, true);
        }
    }
    W.nqueue = 0;
    for (uint32_t c = 0; c < W.ncells; c++) W.cells[c].in = W.cells[c].want;
    wp_update(w, focus, false); // (the HLOD shown for what left)
    W.st.loads = W.st.unloads = 0;
    W.st.missing = W.st.missing_frames = W.st.missing_peak = W.st.frames = W.st.ahead = 0, W.st.speed = 0.0f;
    W.have_prev = false, W.vel[0] = W.vel[1] = 0.0f;
    W.st.begin_ms = (re_time_seconds() - t0) * 1000.0;
    W.st.ms_peak = 0.0;
    printf("[RealEngine] World Partition: %u actors in %u cells of %.0f m (%u in Data Layers only), %u proxies, %u HLOD regions, %.1f KB kept, %.2f ms\n",
           W.nrecs, W.ncells, W.cfg.cell_size / 100.0f, W.st.layered, W.st.proxies, W.st.regions, (double)W.blob_size / 1024.0, W.st.begin_ms);
}

void wp_update(World* w, const float focus[3], bool playing) {
    if (!W.active || w != W.w) return;
    double t0 = re_time_seconds();
    const float cs = W.cfg.cell_size, R = W.cfg.loading_range, out_r = R + cs * 0.25f;
    // (Q27.7, GTA 6's streaming for a car at speed) the focus's velocity: the cells along its next WP_PREDICT_S seconds
    // come in too (a corridor as wide as the range), ordered by when it gets there; a jump (a teleport, a level's start)
    // isn't travel
    if (playing) {
        double now = re_time_seconds();
        if (W.have_prev && now - W.prev_t > 1e-4) {
            float fdt = (float)(now - W.prev_t), vx = (focus[0] - W.prev[0]) / fdt, vy = (focus[1] - W.prev[1]) / fdt;
            if (vx * vx + vy * vy > 2e5f * 2e5f) vx = vy = 0.0f; // (over 2 km/s)
            float a = 1.0f - expf(-fdt / 0.3f);
            W.vel[0] += (vx - W.vel[0]) * a, W.vel[1] += (vy - W.vel[1]) * a;
        }
        W.prev[0] = focus[0], W.prev[1] = focus[1], W.prev_t = now, W.have_prev = true;
    }
    static int s_nopred = -1; // (REALENGINE_WP_NO_PREDICT=1: the range alone, the comparison)
    if (s_nopred < 0) s_nopred = getenv("REALENGINE_WP_NO_PREDICT") != NULL;
    float sp = sqrtf(W.vel[0] * W.vel[0] + W.vel[1] * W.vel[1]);
    bool pred = playing && !s_nopred && sp > 500.0f; // (faster than a run)
    float sx = pred ? W.vel[0] * WP_PREDICT_S : 0.0f, sy = pred ? W.vel[1] * WP_PREDICT_S : 0.0f, sl2 = sx * sx + sy * sy;
    if (sl2 > 4.0f * R * R) { // (at most twice the range ahead: a jet's 800 m would load what the next seconds change anyway)
        float k = 2.0f * R / sqrtf(sl2);
        sx *= k, sy *= k, sl2 = 4.0f * R * R;
    }
    W.st.speed = sp / 100.0f;
    // the cells wanted (their square's distance to the focus, or to its path ahead)
    uint32_t cells_in = 0, ahead = 0;
    for (uint32_t c = 0; c < W.ncells; c++) {
        Cell* k = &W.cells[c];
        float x0 = (float)k->cx * cs, y0 = (float)k->cy * cs;
        float dx = fmaxf(fmaxf(x0 - focus[0], focus[0] - (x0 + cs)), 0.0f), dy = fmaxf(fmaxf(y0 - focus[1], focus[1] - (y0 + cs)), 0.0f);
        k->dist = sqrtf(dx * dx + dy * dy);
        float lim = k->want ? out_r : R;
        uint8_t want = k->dist <= lim;
        k->eta = want ? 0.0f : 1e9f;
        if (!want && pred) { // the path's point nearest the cell's middle, the square's distance from it
            float mx = x0 + 0.5f * cs - focus[0], my = y0 + 0.5f * cs - focus[1];
            float t = fminf(fmaxf((mx * sx + my * sy) / sl2, 0.0f), 1.0f);
            float px = focus[0] + sx * t, py = focus[1] + sy * t;
            float ex = fmaxf(fmaxf(x0 - px, px - (x0 + cs)), 0.0f), ey = fmaxf(fmaxf(y0 - py, py - (y0 + cs)), 0.0f);
            if (ex * ex + ey * ey <= lim * lim) want = 1, k->eta = t * sqrtf(sl2) / sp, ahead++;
        }
        if (want != k->want) {
            k->want = want;
            k->in = 0; // (its records re-checked below)
        }
        cells_in += k->want;
    }
    // the records whose state changed: their cell's want, or their layers'
    bool any_cell = false;
    for (uint32_t c = 0; c < W.ncells && !any_cell; c++) any_cell = !W.cells[c].in;
    if (any_cell || W.layers_changed) {
        for (uint32_t i = 0; i < W.nrecs; i++) {
            Rec* r = &W.recs[i];
            bool check = (r->cell >= 0 && !W.cells[r->cell].in) || (r->layers & W.layers_changed);
            if (!check) continue;
            bool want = rec_want(r), in = r->e != ENTITY_NONE;
            if (want != in) queue_push(i);
            else if (in && rec_shown(r) != r->shown) { // Loaded <-> Activated: only shown or hidden
                r->shown = rec_shown(r);
                set_hidden(w, r->e, !r->shown);
            }
        }
        for (uint32_t c = 0; c < W.ncells; c++) W.cells[c].in = 1;
        W.layers_changed = 0;
    }
    // the queue within the budget (at least one a frame)
    if (W.nqueue) {
        if (W.sort_queue) qsort(W.queue, W.nqueue, sizeof *W.queue, queue_cmp), W.sort_queue = false;
        uint32_t done = 0;
        const double budget = playing ? W.cfg.budget_ms / 1000.0 : 1e9;
        while (done < W.nqueue) {
            Rec* r = &W.recs[W.queue[done]];
            r->queued = 0;
            bool want = rec_want(r), in = r->e != ENTITY_NONE;
            if (want && !in) rec_load(w, r, playing);
            else if (!want && in) rec_unload(w, r);
            done++;
            if (re_time_seconds() - t0 > budget) break;
        }
        memmove(W.queue, W.queue + done, (W.nqueue - done) * sizeof *W.queue);
        W.nqueue -= done;
    }
    // (Q27.7) what the player could see missing: records still waiting to load in a cell well inside the range
    uint32_t missing = 0;
    for (uint32_t i = 0; i < W.nqueue; i++) {
        const Rec* r = &W.recs[W.queue[i]];
        if (r->e == ENTITY_NONE && r->cell >= 0 && W.cells[r->cell].dist <= 0.75f * R && rec_want(r)) missing++;
    }
    W.st.missing = missing, W.st.ahead = ahead;
    if (playing) {
        W.st.frames++;
        if (missing) W.st.missing_frames++;
        if (missing > W.st.missing_peak) W.st.missing_peak = missing;
    }
    // the HLOD: a region whose cells are all out (and none waiting) shows its merged mesh; else the proxies of its
    // records that are out
    uint32_t regions_shown = 0, proxies_shown = 0, records_in = 0;
    if (W.nregions) {
        for (uint32_t k = 0; k < W.nregions; k++) W.regions[k].shown = W.regions[k].e != ENTITY_NONE;
        for (uint32_t c = 0; c < W.ncells; c++)
            if (W.cells[c].want && W.cells[c].region != UINT32_MAX) W.regions[W.cells[c].region].shown = 0;
        for (uint32_t i = 0; i < W.nqueue; i++) { // (a region waiting for an unload keeps its actors' place)
            const Rec* r = &W.recs[W.queue[i]];
            if (r->cell >= 0 && r->e != ENTITY_NONE && W.cells[r->cell].region != UINT32_MAX) W.regions[W.cells[r->cell].region].shown = 0;
        }
        for (uint32_t k = 0; k < W.nregions; k++) {
            if (W.regions[k].e != ENTITY_NONE) set_hidden(w, W.regions[k].e, !W.regions[k].shown);
            regions_shown += W.regions[k].shown;
        }
    }
    for (uint32_t i = 0; i < W.nrecs; i++) {
        Rec* r = &W.recs[i];
        records_in += r->e != ENTITY_NONE;
        if (r->proxy == ENTITY_NONE) continue;
        bool region = W.cells[r->cell].region != UINT32_MAX && W.regions[W.cells[r->cell].region].shown;
        bool show = r->e == ENTITY_NONE && !region && layers_allow(r->layers, WP_LAYER_ACTIVATED);
        if (show != r->proxy_shown) r->proxy_shown = show, set_hidden(w, r->proxy, !show);
        proxies_shown += show;
    }
    W.st.cells = W.ncells, W.st.cells_in = cells_in, W.st.records = W.nrecs, W.st.records_in = records_in;
    W.st.proxies_shown = proxies_shown, W.st.regions_shown = regions_shown, W.st.queued = W.nqueue;
    W.st.ms = (re_time_seconds() - t0) * 1000.0;
    if (playing && W.st.ms > W.st.ms_peak) W.st.ms_peak = W.st.ms;
}

void wp_end(World* w) {
    (void)w;
    free(W.blob), free(W.recs), free(W.queue), free(W.cells), free(W.hash), free(W.regions), free(W.view);
    WpStats keep = W.st;
    memset(&W, 0, sizeof W);
    W.st = keep; // (the last play's numbers: the tests read them after it)
}

bool wp_active(void) { return W.active; }
const WpStats* wp_stats(void) { return &W.st; }

static int layer_index(const char* name) {
    for (uint32_t i = 0; name && i < W.cfg.nlayers && i < WP_MAX_LAYERS; i++)
        if (!_stricmp(W.cfg.layers[i].name, name)) return (int)i;
    return -1;
}
bool wp_set_layer_state(const char* name, int state) {
    int i = W.active ? layer_index(name) : -1;
    if (i < 0 || state < 0 || state >= WP_LAYER_STATE_COUNT) return false;
    if (W.layer_state[i] != (uint8_t)state) W.layer_state[i] = (uint8_t)state, W.layers_changed |= 1u << i;
    return true;
}
int wp_get_layer_state(const char* name) {
    int i = W.active ? layer_index(name) : -1;
    return i < 0 ? -1 : W.layer_state[i];
}
bool wp_location_loaded(const float p[3]) {
    if (!W.active) return true;
    int32_t cx, cy;
    wp_cell_of(W.cfg.cell_size, p, &cx, &cy);
    int32_t k = cell_find(cx, cy);
    return k < 0 || W.cells[k].want != 0; // (an empty cell: nothing to wait for)
}

uint32_t wp_cells_view(const WpCellView** out) {
    if (!W.active) return 0;
    for (uint32_t c = 0; c < W.ncells; c++) {
        const Cell* k = &W.cells[c];
        W.view[c] = (WpCellView){ k->cx, k->cy, k->nrec, k->want != 0, k->region != UINT32_MAX && W.regions[k->region].shown };
    }
    *out = W.view;
    return W.ncells;
}

void wp_editor_layer_visible(World* w, int layer, bool visible) {
    if (layer < 0 || layer >= WP_MAX_LAYERS) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_PARTITION)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        const CPartition* p = ecs_arch_column(a, COMP_PARTITION);
        CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (p[i].layers >> layer & 1u) {
                t[i].flags = visible ? (t[i].flags & ~TRANSFORM_HIDDEN) : (t[i].flags | TRANSFORM_HIDDEN);
                ecs_touch(w, ents[i]);
            }
    }
}

// --- Build HLODs: each region's static meshes merged (world positions minus the region's center), each mesh's color its
// material's baked mean (or its own Base Color) as vertex colors, simplified with meshoptimizer to hlod_keep of the
// triangles (sloppy past what the careful pass can reach: far, the parts may fuse) ---
typedef struct Merge {
    float* pos; // 3 a vertex
    uint32_t* nrm;
    uint32_t* col;
    uint32_t nv, cap_v;
    uint32_t* idx;
    uint32_t ni, cap_i;
} Merge;

static bool grow(void** p, uint32_t* cap, uint32_t need, size_t el) {
    if (need <= *cap) return true;
    uint32_t n = *cap ? *cap : 4096;
    while (n < need) n *= 2;
    void* q = realloc(*p, (size_t)n * el);
    if (!q) return false;
    *p = q, *cap = n;
    return true;
}

static uint32_t srgb8(float c) {
    c = fminf(fmaxf(c, 0.0f), 1.0f);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (uint32_t)lroundf(s * 255.0f);
}

static void oct_dec(uint32_t o, float n[3]) {
    float x = (float)(int16_t)(o & 0xFFFF) / 32767.0f, y = (float)(int16_t)(o >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}
static uint32_t oct_enc(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-12f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t ix = (int16_t)lroundf(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f), iy = (int16_t)lroundf(fminf(fmaxf(y, -1.0f), 1.0f) * 32767.0f);
    return (uint16_t)ix | ((uint32_t)(uint16_t)iy << 16);
}

static bool merge_actor(Merge* m, const CTransform* t, const CStaticMesh* sm, const float center[3]) {
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t first, count;
    int32_t voff;
    render_mesh_cpu(sm->mesh, &v, &idx, &first, &count, &voff);
    if (!v || !idx || !count) return true;
    float col[3] = { sm->base_color[0], sm->base_color[1], sm->base_color[2] };
    if (sm->material) {
        const Material* mt = mat_find_hash(sm->material);
        if (mt) memcpy(col, mt->avg_base, 12);
    }
    uint32_t c8 = srgb8(col[0]) | srgb8(col[1]) << 8 | srgb8(col[2]) << 16 | 255u << 24;
    float rows[3][4];
    render_transform_rows(t, rows);
    // the mesh's vertices it uses (its index range), mapped once each
    uint32_t vmin = UINT32_MAX, vmax = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = idx[first + i];
        if (k < vmin) vmin = k;
        if (k > vmax) vmax = k;
    }
    uint32_t nv = vmax - vmin + 1, base = m->nv, old = m->cap_v;
    if (!grow((void**)&m->pos, &m->cap_v, m->nv + nv, 12)) return false; // (its cap counts vertices: 3 floats each)
    if (m->cap_v != old || !m->nrm) {
        uint32_t* a = realloc(m->nrm, (size_t)m->cap_v * 4);
        if (a) m->nrm = a;
        uint32_t* b = realloc(m->col, (size_t)m->cap_v * 4);
        if (b) m->col = b;
        if (!a || !b) return false;
    }
    if (!grow((void**)&m->idx, &m->cap_i, m->ni + count, 4)) return false;
    for (uint32_t i = 0; i < nv; i++) {
        const GpuVertex* g = &v[(int64_t)vmin + i + voff];
        float* p = m->pos + (size_t)(base + i) * 3;
        float n[3], wn[3];
        for (int r = 0; r < 3; r++) p[r] = rows[r][0] * g->pos[0] + rows[r][1] * g->pos[1] + rows[r][2] * g->pos[2] + rows[r][3] - center[r];
        oct_dec(g->normal_oct, n);
        for (int r = 0; r < 3; r++) // (rows = R S: the normal by R S^-1)
            wn[r] = rows[r][0] * n[0] / (t->scale[0] * t->scale[0] + 1e-12f) + rows[r][1] * n[1] / (t->scale[1] * t->scale[1] + 1e-12f) +
                    rows[r][2] * n[2] / (t->scale[2] * t->scale[2] + 1e-12f);
        float l = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (l > 1e-12f) wn[0] /= l, wn[1] /= l, wn[2] /= l;
        m->nrm[base + i] = oct_enc(wn), m->col[base + i] = c8;
    }
    bool flip = t->scale[0] * t->scale[1] * t->scale[2] < 0.0f;
    for (uint32_t i = 0; i < count; i += 3)
        for (int k = 0; k < 3; k++) m->idx[m->ni + i + (uint32_t)k] = base + idx[first + i + (uint32_t)(flip ? 2 - k : k)] - vmin;
    m->nv += nv, m->ni += count;
    return true;
}

bool wp_build_hlods(World* w, const char* content_root, const char* level_name, char* msg, size_t cap) {
    Entity s = wp_settings(w);
    if (s == ENTITY_NONE) {
        snprintf(msg, cap, "No World Partition actor in the level.");
        return false;
    }
    const CWorldPartition* cfg = ecs_get(w, s, COMP_WORLD_PARTITION);
    const float cs = cfg->cell_size >= 100.0f ? cfg->cell_size : 12800.0f;
    const int32_t rn = cfg->hlod_region > 0 ? (int32_t)cfg->hlod_region : 4;
    const float rs = cs * (float)rn, keep = cfg->hlod_keep > 0.001f ? fminf(cfg->hlod_keep, 1.0f) : 0.05f;
    double t0 = re_time_seconds();
    char dir[900];
    wp_hlod_dir(content_root, level_name, dir, sizeof dir);
    re_remove_tree(dir); // (the old regions: made again)
    re_make_dirs(dir);
    // the regions with spatial static meshes (no Blueprint, not simulated, big enough)
    typedef struct RK { int32_t rx, ry; } RK;
    RK* keys = NULL;
    uint32_t nkeys = 0, capk = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH);
        if ((a->mask & need) != need || (a->mask & (COMP_BIT(COMP_BLUEPRINT) | COMP_BIT(COMP_ATTACH))) || !a->count) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            if (!wp_actor_spatial(w, ents[i]) || wp_actor_layers(w, ents[i])) continue; // (a Data Layer's actors may be unloaded: not baked in)
            int32_t cx, cy, rx, ry;
            wp_cell_of(cs, t[i].location, &cx, &cy);
            rx = cx >= 0 ? cx / rn : -((-cx + rn - 1) / rn), ry = cy >= 0 ? cy / rn : -((-cy + rn - 1) / rn);
            uint32_t k = 0;
            while (k < nkeys && (keys[k].rx != rx || keys[k].ry != ry)) k++;
            if (k == nkeys) {
                if (!grow((void**)&keys, &capk, nkeys + 1, sizeof *keys)) break;
                keys[nkeys++] = (RK){ rx, ry };
            }
        }
    }
    uint32_t made = 0, tris_in = 0, tris_out = 0;
    Merge m = { 0 };
    for (uint32_t k = 0; k < nkeys; k++) {
        m.nv = m.ni = 0;
        const float center[3] = { ((float)keys[k].rx + 0.5f) * rs, ((float)keys[k].ry + 0.5f) * rs, 0.0f };
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH);
            if ((a->mask & need) != need || (a->mask & (COMP_BIT(COMP_BLUEPRINT) | COMP_BIT(COMP_ATTACH))) || !a->count) continue;
            const Entity* ents = ecs_arch_entities(a);
            const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
            const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
            const CRigidBody* rb = (a->mask & COMP_BIT(COMP_RIGID_BODY)) ? ecs_arch_column(a, COMP_RIGID_BODY) : NULL;
            for (uint32_t i = 0; i < a->count; i++) {
                if ((rb && rb[i].simulate) || (t[i].flags & TRANSFORM_HIDDEN_GAME) || !wp_actor_spatial(w, ents[i]) || wp_actor_layers(w, ents[i])) continue;
                float mn[3], mx[3], sz = 0.0f;
                render_mesh_bounds(sm[i].mesh, mn, mx);
                for (int q = 0; q < 3; q++) sz = fmaxf(sz, (mx[q] - mn[q]) * fabsf(t[i].scale[q]));
                if (sz < cfg->hlod_min_size) continue;
                int32_t cx, cy, rx, ry;
                wp_cell_of(cs, t[i].location, &cx, &cy);
                rx = cx >= 0 ? cx / rn : -((-cx + rn - 1) / rn), ry = cy >= 0 ? cy / rn : -((-cy + rn - 1) / rn);
                if (rx != keys[k].rx || ry != keys[k].ry) continue;
                if (!merge_actor(&m, &t[i], &sm[i], center)) break;
            }
        }
        if (!m.ni) continue;
        tris_in += m.ni / 3;
        // simplified: careful first (the shapes kept), then sloppy to the target (far: whatever reaches it)
        uint32_t target = (uint32_t)((float)m.ni * keep) / 3 * 3;
        if (target < 3) target = 3;
        uint32_t* out = malloc((size_t)m.ni * 4);
        if (!out) break;
        float err = 0.0f;
        size_t n = meshopt_simplify(out, m.idx, m.ni, m.pos, m.nv, 12, target, 0.05f, 0, &err);
        if (n > target * 2) {
            size_t n2 = meshopt_simplifySloppy(out, m.idx, m.ni, m.pos, m.nv, 12, NULL, target, 0.2f, &err);
            if (n2 >= 3) n = n2;
        }
        if (n < 3) {
            free(out);
            continue;
        }
        // to corners (each its color: the mesh builder welds them again)
        GpuVertex* corners = malloc(n * sizeof *corners);
        uint32_t* cc = malloc(n * 4);
        if (!corners || !cc) {
            free(out), free(corners), free(cc);
            break;
        }
        for (size_t i = 0; i < n; i++) {
            uint32_t vi = out[i];
            memcpy(corners[i].pos, m.pos + (size_t)vi * 3, 12);
            corners[i].normal_oct = m.nrm[vi];
            corners[i].uv[0] = corners[i].uv[1] = 0.0f, corners[i].uv2[0] = corners[i].uv2[1] = -1.0f;
            cc[i] = m.col[vi];
        }
        MeshBuilt mb;
        bool ok = mesh_build_corners_col(corners, cc, (uint32_t)(n / 3), true, &mb);
        free(out), free(corners), free(cc);
        if (!ok) continue;
        mb.flags |= RMESH_NO_ATLAS; // (lit by the probes: a far mesh)
        char path[1000];
        snprintf(path, sizeof path, "%s/r_%d_%d.rmesh", dir, keys[k].rx, keys[k].ry);
        if (rmesh_write(path, &mb)) made++, tris_out += mb.lods.index_count[0] / 3;
        mesh_built_free(&mb);
    }
    free(m.pos), free(m.nrm), free(m.col), free(m.idx), free(keys);
    snprintf(msg, cap, "%u HLOD regions, %u -> %u triangles (%.0f m each), %.0f ms", made, tris_in, tris_out, rs / 100.0f,
             (re_time_seconds() - t0) * 1000.0);
    printf("[RealEngine] Build HLODs: %s\n", msg);
    return made > 0;
}
