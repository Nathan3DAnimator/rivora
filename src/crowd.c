// Crowds (crowd.h).
#include "crowd.h"
#include "city.h" // (Q27.10) the city's routines

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "re_sync.h"

#include "ai.h"
#include "blueprint.h"
#include "components.h"
#include "platform.h"
#include "render.h"

float bp_trace_boxes(const float a[3], const float b[3], float radius); // (bp_vm.c: the world's boxes while playing)

enum { ROLE_ROAM = 0, ROLE_CHASE, ROLE_RING, ROLE_FLEE };
#define CROWD_FAR3 6000.0f // (Q27.1) LOD 3 past 60 m: the two biggest parts, a figure a few pixels tall

typedef struct CrowdRun {
    Entity e;
    CCrowd c;
    uint32_t cfg_hash;      // the component and the actor's place it was made from (the editor's edits)
    SkelMesh* mesh;
    float home[3], home_yaw;
    uint32_t n, cap;
    // the agents (structure of arrays)
    float *x, *y, *z, *nx, *ny, *vx, *vy, *yaw, *pa, *pb, *blend, *speed_k, *size_k;
    uint32_t* look; // (Q27.9) each agent's look (CrowdAgentGpu.look)
    uint16_t* visit; // (Q27.10) the Smart Object it goes to or stays at (its index + 1 in city_smart_objects; 0: none)
    float* stay;     // (Q27.10) real seconds it still stays there
    uint32_t nlooks; // its distinct looks
    float *think, *mem_x, *mem_y, *mem_t, *goal_x, *goal_y;
    float *panic, *pan_x, *pan_y; // (Q27.1) the seconds of panic left (0: calm) and the alarm it runs from
    uint8_t *role, *clip_a, *clip_b, *knows;
    uint32_t* slot;
    CrowdAgentGpu* gpu;
    // the look: the parts baked per clip and frame
    float* baked;
    uint8_t* big;
    uint32_t nparts, version;
    float clip_len[3], clip_speed[3]; // the baked clips' cycles (s) and their speeds over the ground (cm/s, 0: unknown)
    // the hive's decision clock
    float hive_think;
    uint32_t n_chase, n_ring, n_know; // (the stats: counted when they decide)
    uint32_t nring, layers; // the ring's agents and its concentric layers (as many as they need: each holds its circumference)
    bool hive_knows;
    float hive_mem[2], hive_mem_t;
} CrowdRun;

static struct {
    CrowdRun runs[CROWD_MAX_RUNS];
    uint32_t nruns;
    CrowdDraw draws[CROWD_MAX_RUNS];
    CrowdStats stats;
    bool playing;
    float player[3], player_v[3], player_prev[3];
    bool has_player, had_player;
    // the spatial hash (one run at a time)
    uint32_t *cell_start, *cell_items, hash_cap, items_cap;
    uint32_t rng;
    uint32_t version_seq;
    // (Q27.1) the alarms: a wave from each point (its front at CROWD_WAVE cm/s up to its radius); the agents it passed
    // wait in line to panic, promoted at most CROWD_PROMOTE a frame
    struct { float x, y, radius, front; bool on; } alarm[8];
    uint32_t frame, stats_peak, promoted, alarms_total;
} C;

// --- The flow field: a breadth first search of the navigation grid from a target cell, on a worker thread ---
// dist[cell]: steps to the target (0xFFFF: unreachable / not walkable). Double buffered: the agents read the front one
// while the worker fills the back; a new target is asked for when the old one moved a cell.

static struct {
    ReThread* thread;
    volatile int32_t quit, want, busy, ready; // want: a target is waiting; ready: the back buffer is done
    uint16_t *front, *back;
    uint32_t* queue;
    uint32_t w, h, cells;
    float origin[2], cell;
    const float* height;
    float target[2], next_target[2];   // the front's target, the one being built
    int32_t want_cell;
    double build_ms;
    uint32_t builds;
    bool valid;                        // front holds a field
} F;

static int cell_of(float x, float y) {
    if (!F.cells) return -1;
    int cx = (int)floorf((x - F.origin[0]) / F.cell), cy = (int)floorf((y - F.origin[1]) / F.cell);
    if (cx < 0 || cy < 0 || cx >= (int)F.w || cy >= (int)F.h) return -1;
    return cy * (int)F.w + cx;
}
static bool walkable(int c) { return c >= 0 && F.height && F.height[c] > -1e29f; }

static void flow_build(uint16_t* dist, float tx, float ty) {
    for (uint32_t i = 0; i < F.cells; i++) dist[i] = 0xFFFFu;
    int t = cell_of(tx, ty);
    if (!walkable(t)) { // (the target off the grid: the nearest walkable cell in a small square)
        int best = -1;
        float bd = 1e30f;
        int cx = (int)floorf((tx - F.origin[0]) / F.cell), cy = (int)floorf((ty - F.origin[1]) / F.cell);
        for (int dy = -8; dy <= 8; dy++)
            for (int dx = -8; dx <= 8; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= (int)F.w || y >= (int)F.h || !walkable(y * (int)F.w + x)) continue;
                float d = (float)(dx * dx + dy * dy);
                if (d < bd) bd = d, best = y * (int)F.w + x;
            }
        t = best;
    }
    if (t < 0) return;
    uint32_t qh = 0, qt = 0;
    dist[t] = 0, F.queue[qt++] = (uint32_t)t;
    static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
    while (qh < qt) {
        uint32_t c = F.queue[qh++];
        int x = (int)(c % F.w), y = (int)(c / F.w);
        uint16_t nd = (uint16_t)(dist[c] + 1u);
        if (nd == 0xFFFFu) continue;
        for (int k = 0; k < 4; k++) {
            int ax = x + dx4[k], ay = y + dy4[k];
            if (ax < 0 || ay < 0 || ax >= (int)F.w || ay >= (int)F.h) continue;
            uint32_t a = (uint32_t)(ay * (int)F.w + ax);
            if (dist[a] != 0xFFFFu || !walkable((int)a)) continue;
            // (a step up or down of over 45 cm is a wall)
            if (fabsf(F.height[a] - F.height[c]) > 45.0f) continue;
            dist[a] = nd, F.queue[qt++] = a;
        }
    }
}

static int flow_worker(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    while (!re_atomic_load(&F.quit)) {
        if (!re_atomic_cas(&F.want, 1, 0)) {
            re_sleep_ms(2);
            continue;
        }
        re_atomic_store(&F.busy, 1);
        double t0 = re_time_seconds();
        flow_build(F.back, F.next_target[0], F.next_target[1]);
        F.build_ms = (re_time_seconds() - t0) * 1000.0;
        re_atomic_store(&F.busy, 0);
        re_atomic_store(&F.ready, 1);
    }
    return 0;
}

static void flow_start(void) {
    const NavInfo* nav = ai_nav_info();
    if (!nav || !nav->w || !nav->h || !nav->height) return;
    F.w = nav->w, F.h = nav->h, F.cells = nav->w * nav->h;
    F.origin[0] = nav->origin[0], F.origin[1] = nav->origin[1], F.cell = nav->cell, F.height = nav->height;
    F.front = malloc(sizeof(uint16_t) * F.cells), F.back = malloc(sizeof(uint16_t) * F.cells);
    F.queue = malloc(sizeof(uint32_t) * F.cells);
    if (!F.front || !F.back || !F.queue) {
        free(F.front), free(F.back), free(F.queue);
        memset(&F, 0, sizeof F);
        return;
    }
    F.quit = F.want = F.busy = F.ready = 0;
    F.valid = false, F.want_cell = -1;
    F.thread = re_thread_start(flow_worker, NULL);
}

static void flow_stop(void) {
    if (F.thread) {
        re_atomic_store(&F.quit, 1);
        re_thread_join(F.thread);
    }
    free(F.front), free(F.back), free(F.queue);
    memset(&F, 0, sizeof F);
}

// A new target: built in the background when it moved to another cell; a finished build swapped in
static void flow_target(float x, float y) {
    if (!F.thread) return;
    if (re_atomic_cas(&F.ready, 1, 0)) {
        uint16_t* t = F.front;
        F.front = F.back, F.back = t;
        F.target[0] = F.next_target[0], F.target[1] = F.next_target[1];
        F.valid = true;
        C.stats.flow_ms = F.build_ms;
        F.builds++;
    }
    int c = cell_of(x, y);
    if (c == F.want_cell || re_atomic_load(&F.busy) || re_atomic_load(&F.want)) return;
    F.want_cell = c;
    F.next_target[0] = x, F.next_target[1] = y;
    re_atomic_store(&F.want, 1);
}

// The way down the field at a point: toward the lowest of the 8 neighbors (a smooth blend of the lower ones); false
// where there is no field (off it, unreachable)
static bool flow_dir(float x, float y, float out[2]) {
    if (!F.valid) return false;
    int c = cell_of(x, y);
    if (c < 0 || F.front[c] == 0xFFFFu) return false;
    int cx = c % (int)F.w, cy = c / (int)F.w;
    float d0 = F.front[c], gx = 0.0f, gy = 0.0f;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int x2 = cx + dx, y2 = cy + dy;
            if (x2 < 0 || y2 < 0 || x2 >= (int)F.w || y2 >= (int)F.h) continue;
            uint16_t d = F.front[y2 * (int)F.w + x2];
            if (d == 0xFFFFu || (float)d >= d0) continue;
            // (a diagonal only when both of its sides are open: no corner cutting into a wall)
            if (dx && dy && (F.front[cy * (int)F.w + x2] == 0xFFFFu || F.front[y2 * (int)F.w + cx] == 0xFFFFu)) continue;
            float wgt = (d0 - (float)d) / (dx && dy ? 1.41421356f : 1.0f);
            float l = dx && dy ? 0.70710678f : 1.0f;
            gx += (float)dx * l * wgt, gy += (float)dy * l * wgt;
        }
    float l = sqrtf(gx * gx + gy * gy);
    if (l < 1e-6f) return false;
    out[0] = gx / l, out[1] = gy / l;
    return true;
}
static float flow_height(float x, float y, float fallback) {
    int c = cell_of(x, y);
    return walkable(c) ? F.height[c] : fallback;
}

// --- The runs ---

static uint32_t rnd(uint32_t* s) {
    *s ^= *s << 13, *s ^= *s >> 17, *s ^= *s << 5;
    return *s;
}
static float rndf(uint32_t* s) { return (float)(rnd(s) & 0xFFFFFF) / 16777216.0f; }

static uint32_t hash_cfg(const CCrowd* c, const CTransform* t) {
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)c;
    for (size_t i = 0; i < sizeof *c; i++) h = (h ^ p[i]) * 16777619u;
    p = (const uint8_t*)t->location;
    for (size_t i = 0; i < 24; i++) h = (h ^ p[i]) * 16777619u; // (location and rotation)
    return h;
}

static void run_free(CrowdRun* r) {
    float** fs[] = { &r->x, &r->y, &r->z, &r->nx, &r->ny, &r->vx, &r->vy, &r->yaw, &r->pa, &r->pb, &r->blend, &r->speed_k, &r->size_k,
                     &r->think, &r->mem_x, &r->mem_y, &r->mem_t, &r->goal_x, &r->goal_y, &r->panic, &r->pan_x, &r->pan_y };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) free(*fs[i]), *fs[i] = NULL;
    free(r->role), free(r->clip_a), free(r->clip_b), free(r->knows), free(r->slot), free(r->gpu), free(r->baked), free(r->big), free(r->look);
    free(r->visit), free(r->stay);
    r->role = r->clip_a = r->clip_b = r->knows = r->big = NULL, r->slot = NULL, r->gpu = NULL, r->baked = NULL, r->look = NULL;
    r->visit = NULL, r->stay = NULL;
    r->n = r->cap = 0;
}

static bool run_alloc(CrowdRun* r, uint32_t n) {
    float** fs[] = { &r->x, &r->y, &r->z, &r->nx, &r->ny, &r->vx, &r->vy, &r->yaw, &r->pa, &r->pb, &r->blend, &r->speed_k, &r->size_k,
                     &r->think, &r->mem_x, &r->mem_y, &r->mem_t, &r->goal_x, &r->goal_y, &r->panic, &r->pan_x, &r->pan_y };
    bool ok = true;
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) ok &= (*fs[i] = calloc(n + 1, sizeof(float))) != NULL;
    ok &= (r->role = calloc(n + 1, 1)) != NULL && (r->clip_a = calloc(n + 1, 1)) != NULL && (r->clip_b = calloc(n + 1, 1)) != NULL;
    ok &= (r->knows = calloc(n + 1, 1)) != NULL && (r->slot = calloc(n + 1, sizeof(uint32_t))) != NULL;
    ok &= (r->gpu = calloc(n + 1, sizeof(CrowdAgentGpu))) != NULL && (r->look = calloc(n + 1, sizeof(uint32_t))) != NULL;
    ok &= (r->visit = calloc(n + 1, sizeof(uint16_t))) != NULL && (r->stay = calloc(n + 1, sizeof(float))) != NULL; // (Q27.10)
    r->cap = n;
    return ok;
}

// The parts of the mesh baked: idle, walk, run, each resampled to CROWD_FRAMES over its cycle
static void bake(CrowdRun* r) {
    free(r->baked), free(r->big);
    r->baked = NULL, r->big = NULL, r->nparts = 0;
    SkelMesh* m = r->mesh;
    if (!m || !m->nparts) return;
    static const char* const fallback[3] = { "MM_Idle", "MM_Walk", "MM_Run" };
    AnimSeq* seq[3];
    for (int k = 0; k < 3; k++) {
        seq[k] = r->c.anim[k] ? anim_seq(r->c.anim[k]) : NULL;
        if (!seq[k]) seq[k] = anim_seq_name(fallback[k]);
    }
    uint32_t np = m->nparts;
    r->baked = malloc(sizeof(float) * 12 * np * CROWD_FRAMES * 3);
    r->big = calloc(np, 1);
    if (!r->baked || !r->big) return;
    static AnimXf pose[ANIM_MAX_BONES];
    static float bones[ANIM_MAX_BONES][3][4];
    const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    for (int k = 0; k < 3; k++)
        for (uint32_t f = 0; f < CROWD_FRAMES; f++) {
            if (seq[k]) anim_sample(seq[k], m, seq[k]->length * (float)f / (float)CROWD_FRAMES, pose);
            else
                for (uint32_t b = 0; b < m->nbones; b++) pose[b] = m->bones[b].bind;
            anim_world(m, pose, id, bones);
            for (uint32_t p = 0; p < np; p++)
                anim_part_rows(&m->parts[p], bones[m->parts[p].bone], (float(*)[4]) & r->baked[(((size_t)k * CROWD_FRAMES + f) * np + p) * 12]);
        }
    // the big parts (kept far away): the larger half by volume (the scale of the part's rows times its mesh's bounds)
    float vol[ANIM_MAX_PARTS], sorted[ANIM_MAX_PARTS];
    for (uint32_t p = 0; p < np; p++) {
        float mn[3], mx[3];
        render_mesh_bounds(m->parts[p].mesh, mn, mx);
        const float* rw = &r->baked[(size_t)p * 12];
        float v = 1.0f;
        for (int a = 0; a < 3; a++) {
            float s = sqrtf(rw[a] * rw[a] + rw[4 + a] * rw[4 + a] + rw[8 + a] * rw[8 + a]);
            v *= fmaxf((mx[a] - mn[a]) * s, 1.0f);
        }
        vol[p] = sorted[p] = v;
    }
    for (uint32_t i = 1; i < np; i++) // (insertion sort: a few dozen)
        for (uint32_t j = i; j > 0 && sorted[j] > sorted[j - 1]; j--) {
            float t = sorted[j];
            sorted[j] = sorted[j - 1], sorted[j - 1] = t;
        }
    float cut = sorted[np > 6 ? 5 : np - 1], cut2 = sorted[np > 2 ? 1 : np - 1];
    for (uint32_t p = 0; p < np; p++) r->big[p] = (uint8_t)((vol[p] >= cut ? 1u : 0u) | (vol[p] >= cut2 ? 2u : 0u)); // (bit 1: kept at LOD 3, Q27.1)
    // (Q27.9) each part's region for the looks (bits 2-3): the head, the neck and the hands the skin, the spine and the
    // arms the top, the pelvis and the legs the bottom; anything else keeps its color
    for (uint32_t p = 0; p < np; p++) {
        char nm[ANIM_NAME];
        snprintf(nm, sizeof nm, "%s", m->bones[m->parts[p].bone].name);
        for (char* q = nm; *q; q++) *q = (char)tolower((unsigned char)*q);
        uint32_t reg = 3u;
        if (strstr(nm, "head") || strstr(nm, "neck") || strstr(nm, "hand") || strstr(nm, "finger") || strstr(nm, "thumb") || strstr(nm, "index") || strstr(nm, "pinky") || strstr(nm, "ring") || strstr(nm, "middle")) reg = 0u;
        else if (strstr(nm, "spine") || strstr(nm, "chest") || strstr(nm, "clavicle") || strstr(nm, "shoulder") || strstr(nm, "arm")) reg = 1u;
        else if (strstr(nm, "pelvis") || strstr(nm, "hip") || strstr(nm, "thigh") || strstr(nm, "calf") || strstr(nm, "leg") || strstr(nm, "knee") || strstr(nm, "foot") || strstr(nm, "ball") || strstr(nm, "toe")) reg = 2u;
        r->big[p] = (uint8_t)(r->big[p] | reg << 2);
    }
    r->nparts = np;
    // the phases go as fast as the clip's planted foot: at any speed the feet keep their place (no sliding)
    for (int k = 0; k < 3; k++) {
        r->clip_len[k] = seq[k] && seq[k]->length > 0.05f ? seq[k]->length : (k == 0 ? 2.0f : k == 1 ? 1.0f : 0.7f);
        r->clip_speed[k] = k && seq[k] ? anim_seq_ground_speed(seq[k], m) : 0.0f;
    }
    r->version = ++C.version_seq;
}

// (Q27.9) Each agent's look by the seed (its own stream: the rest of spawn keeps its numbers): a skin tone of 8, a top of
// 32 colors, a bottom of 16, a build of 16 (wider or slimmer); Looks under 1 lets that share of them keep the mesh's own
// colors. The distinct ones counted (an open-addressed set)
static void make_looks(CrowdRun* r) {
    uint32_t s = r->c.seed * 2246822519u + 0x5bd1e995u;
    float share = fminf(fmaxf(r->c.looks, 0.0f), 1.0f);
    for (uint32_t i = 0; i < r->n; i++) {
        uint32_t h = s ^ (i * 0x9E3779B9u);
        h ^= h >> 16, h *= 0x7feb352du, h ^= h >> 15, h *= 0x846ca68bu, h ^= h >> 16;
        bool on = share > 0.0f && (float)(h >> 8) / 16777216.0f < share * 1.0001f;
        uint32_t h2 = h * 0x9E3779B1u + 0x7F4A7C15u;
        h2 ^= h2 >> 15;
        r->look[i] = on ? ((h & 7u) | ((h2 & 31u) << 3) | (((h2 >> 5) & 15u) << 8) | (((h2 >> 9) & 15u) << 12) | 0x80000000u) : 0u;
    }
    static uint32_t set[8192];
    memset(set, 0, sizeof set);
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->n; i++) {
        uint32_t k = r->look[i] | 1u, j = (k * 2654435761u) & 8191u; // (0 is the empty slot: the bit 0 forced on for the set only)
        if (!r->look[i]) continue;
        for (uint32_t probe = 0; probe < 8192u; probe++, j = (j + 1u) & 8191u) {
            if (set[j] == r->look[i]) break;
            if (!set[j]) {
                set[j] = r->look[i], n++;
                break;
            }
        }
        (void)k;
    }
    r->nlooks = n;
}

// The agents where they start: a disk around the actor, facing out at random, each its own speed and size
static void spawn(CrowdRun* r, const CTransform* t) {
    uint32_t s = r->c.seed * 2654435761u + 12345u;
    if (!s) s = 1;
    float rad = fmaxf(r->c.radius, 50.0f), var = fminf(fmaxf(r->c.variety, 0.0f), 1.0f);
    for (uint32_t i = 0; i < r->n; i++) {
        float a = rndf(&s) * 6.2831853f, d = sqrtf(rndf(&s)) * rad;
        r->x[i] = t->location[0] + cosf(a) * d, r->y[i] = t->location[1] + sinf(a) * d;
        r->z[i] = t->location[2];
        r->vx[i] = r->vy[i] = 0.0f;
        r->yaw[i] = rndf(&s) * 360.0f;
        r->pa[i] = r->pb[i] = rndf(&s);
        r->blend[i] = 0.0f, r->clip_a[i] = 0, r->clip_b[i] = 1;
        r->speed_k[i] = 1.0f + (rndf(&s) * 2.0f - 1.0f) * 0.25f * var;
        r->size_k[i] = 1.0f + (rndf(&s) * 2.0f - 1.0f) * 0.12f * var;
        r->think[i] = rndf(&s) * fmaxf(r->c.reaction, 0.05f); // (staggered: not all decide on the same frame)
        r->goal_x[i] = r->x[i], r->goal_y[i] = r->y[i];
        r->role[i] = ROLE_ROAM, r->knows[i] = 0, r->mem_t[i] = -1e9f;
        r->panic[i] = 0.0f;
        r->slot[i] = (uint32_t)i;
    }
    r->hive_think = 0.0f, r->hive_knows = false, r->hive_mem_t = -1e9f;
}

static CrowdRun* run_for(Entity e) {
    for (uint32_t i = 0; i < C.nruns; i++)
        if (C.runs[i].e == e) return &C.runs[i];
    return NULL;
}

// The runs follow the level's spawners: new ones made, edited ones remade, gone ones freed
static void sync_runs(World* w) {
    bool seen[CROWD_MAX_RUNS] = { false };
    const uint64_t need = COMP_BIT(COMP_CROWD) | COMP_BIT(COMP_TRANSFORM);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CCrowd* cc = ecs_arch_column(a, COMP_CROWD);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            CrowdRun* r = run_for(ents[i]);
            if (!r && C.nruns < CROWD_MAX_RUNS) r = &C.runs[C.nruns++], memset(r, 0, sizeof *r), r->e = ents[i];
            if (!r) continue;
            seen[r - C.runs] = true;
            uint32_t h = hash_cfg(&cc[i], &tr[i]);
            if (h == r->cfg_hash && r->n) continue;
            if (C.playing && r->n && r->c.count == cc[i].count && r->c.mesh == cc[i].mesh) { // (playing: the AI's numbers only)
                r->c = cc[i], r->cfg_hash = h;
                continue;
            }
            uint32_t n = cc[i].count > 200000u ? 200000u : cc[i].count; // (a spawner's budget: its agents' buffers)
            if (cc[i].count > n) // (2026-11-25, item 20: a number past the budget is said, not dropped in silence)
                printf("[RealEngine] Crowd: %u agents asked, %u made: a spawner holds 200 000 at most (more spawners for more)\n", cc[i].count, n);
            CCrowd old = r->c;
            r->c = cc[i], r->cfg_hash = h;
            SkelMesh* m = cc[i].mesh ? anim_mesh(cc[i].mesh) : anim_mesh_name("SK_Mannequin");
            if (n > r->cap) {
                run_free(r);
                if (!run_alloc(r, n)) {
                    run_free(r);
                    continue;
                }
            }
            r->n = n;
            if (m != r->mesh || !r->baked || memcmp(old.anim, cc[i].anim, sizeof old.anim)) r->mesh = m, bake(r);
            memcpy(r->home, tr[i].location, 12), r->home_yaw = tr[i].rotation[1];
            spawn(r, &tr[i]);
            make_looks(r); // (Q27.9)
            if (r->visit) memset(r->visit, 0, sizeof(uint16_t) * (n + 1)); // (Q27.10)
        }
    }
    for (uint32_t i = 0; i < C.nruns;) { // (the gone ones: the last moved into their place)
        if (seen[i]) {
            i++;
            continue;
        }
        run_free(&C.runs[i]);
        C.runs[i] = C.runs[--C.nruns];
        seen[i] = seen[C.nruns];
        memset(&C.runs[C.nruns], 0, sizeof C.runs[0]);
    }
}

// --- Thinking and moving ---

typedef struct StepJob {
    CrowdRun* r;
    float dt, now, target[2], ring_center[2];
    bool target_known;
    uint32_t per, frame;
    const CitySO* so; // (Q27.10) the city's Smart Objects (City Routine), the clock's hour and scale
    uint32_t nso;
    float hour, scale;
    volatile int32_t* cnt; // (Q27.10) each Smart Object's visitors, reserved atomically by the steps
} StepJob;

// The neighbors within the personal space pushing apart (the hash: cells of the separation's size); at most 32 looked
// at (a packed spot costs no more than a loose one); their count: the crowd's pressure here
static uint32_t separation(const CrowdRun* r, uint32_t i, float cs, uint32_t hw, float sep, float out[2]) {
    float px = r->x[i], py = r->y[i], ax = 0.0f, ay = 0.0f;
    int cx = (int)floorf(px / cs), cy = (int)floorf(py / cs);
    uint32_t seen = 0, close = 0;
    for (int dy = -1; dy <= 1 && seen < 32; dy++)
        for (int dx = -1; dx <= 1 && seen < 32; dx++) {
            uint32_t h = ((uint32_t)(cx + dx) * 73856093u ^ (uint32_t)(cy + dy) * 19349663u) & (hw - 1u);
            for (uint32_t k = C.cell_start[h]; k < C.cell_start[h + 1] && seen < 32; k++) {
                uint32_t j = C.cell_items[k];
                if (j == i) continue;
                seen++;
                float ddx = px - r->x[j], ddy = py - r->y[j], d2 = ddx * ddx + ddy * ddy;
                if (d2 >= sep * sep) continue;
                close++;
                if (d2 < 1e-4f) { // (on the same spot: apart by their indices)
                    ax += (i < j ? 1.0f : -1.0f), ay += (i < j ? 0.5f : -0.5f);
                    continue;
                }
                float d = sqrtf(d2), f = (sep - d) / sep;
                ax += ddx / d * f, ay += ddy / d * f;
            }
        }
    out[0] = ax, out[1] = ay;
    return close;
}

static void step_agents(void* arg, int index) {
    StepJob* j = arg;
    CrowdRun* r = j->r;
    const CCrowd* c = &r->c;
    uint32_t a0 = (uint32_t)index * j->per, a1 = a0 + j->per;
    if (a1 > r->n) a1 = r->n;
    float cs = fmaxf(c->separation, 30.0f);
    uint32_t hw = C.hash_cap;
    float walk = fmaxf(c->walk_speed, 10.0f), run = fmaxf(c->run_speed, walk + 1.0f);
    float rad = fmaxf(c->radius, 50.0f), ring = fmaxf(c->ring_radius, 100.0f);
    for (uint32_t i = a0; i < a1; i++) {
        // (Q27.1) the simulation's rings: a calm agent far away (drawn at LOD 2 or 3 last frame) steps every other frame
        // with twice the time (half of them each frame): at 30 m a 100 Hz step can't be told from 200 Hz
        uint32_t lod = (r->gpu[i].clips >> 16) & 255u;
        float dt = j->dt;
        if (lod >= 2u && r->panic[i] <= 0.0f) {
            if (((i ^ j->frame) & 1u) != 0u) {
                r->nx[i] = r->x[i], r->ny[i] = r->y[i];
                continue;
            }
            dt *= 2.0f;
        }
        float px = r->x[i], py = r->y[i], dir[2] = { 0, 0 }, speed = 0.0f;
        bool know = c->mind == CROWD_HIVE ? j->target_known : r->knows[i] != 0;
        float tx = j->target[0], ty = j->target[1];
        if (c->mind == CROWD_INDIVIDUAL && know) tx = r->mem_x[i], ty = r->mem_y[i]; // (where IT last saw the player)
        uint8_t role = r->role[i];
        if (!know || c->behavior == CROWD_WANDER) role = ROLE_ROAM;
        float dx = tx - px, dy = ty - py, dist = sqrtf(dx * dx + dy * dy);
        if (r->panic[i] > 0.0f) { // (Q27.1) panicked: running from its alarm, then calm again
            r->panic[i] = fmaxf(r->panic[i] - j->dt, 0.0f);
            float ax = px - r->pan_x[i], ay = py - r->pan_y[i], al = sqrtf(ax * ax + ay * ay);
            uint32_t h = i * 2654435761u + 7u; // (a slant of its own: they scatter, not one line)
            float sl = (rndf(&h) - 0.5f) * 0.9f, cs0 = cosf(sl), sn0 = sinf(sl);
            if (al > 1e-3f) ax /= al, ay /= al;
            else ax = 1.0f, ay = 0.0f;
            dir[0] = ax * cs0 - ay * sn0, dir[1] = ax * sn0 + ay * cs0;
            speed = run;
            role = ROLE_FLEE, know = false;
            tx = r->pan_x[i], ty = r->pan_y[i];
        } else if (role == ROLE_CHASE || role == ROLE_FLEE) {
            float f[2];
            // (the field leads to the shared target; an Individual far from it, or with its own memory, goes straight)
            bool shared = c->mind == CROWD_HIVE || (fabsf(tx - F.target[0]) < 150.0f && fabsf(ty - F.target[1]) < 150.0f);
            if (dist > 120.0f && shared && flow_dir(px, py, f)) dir[0] = f[0], dir[1] = f[1];
            else if (dist > 1e-3f) dir[0] = dx / dist, dir[1] = dy / dist;
            if (role == ROLE_FLEE) dir[0] = -dir[0], dir[1] = -dir[1];
            speed = role == ROLE_FLEE || dist > 60.0f ? run : 0.0f;
        } else if (role == ROLE_RING) { // its slot on the ring around the target (by the field until near, then straight)
            // (slot k in angle order: layer k % L, its place k / L of the layer's share: neighbors stay neighbors)
            uint32_t L = r->layers ? r->layers : 1, nr = r->nring ? r->nring : 1, k = r->slot[i] % nr;
            uint32_t layer = k % L, per = (nr + L - 1) / L;
            float ang = ((float)(k / L) + (layer & 1u ? 0.5f : 0.0f)) / (float)per * 6.2831853f;
            float rr = ring + (float)layer * fmaxf(c->separation, 20.0f) * 1.15f;
            float sx = j->ring_center[0] + cosf(ang) * rr, sy = j->ring_center[1] + sinf(ang) * rr;
            float f[2], cdx = j->ring_center[0] - px, cdy = j->ring_center[1] - py, cd = sqrtf(cdx * cdx + cdy * cdy);
            float gx = sx - px, gy = sy - py, gd = sqrtf(gx * gx + gy * gy);
            if (cd > rr * 1.8f && flow_dir(px, py, f)) dir[0] = f[0], dir[1] = f[1], speed = run;
            else if (gd > 1e-3f) dir[0] = gx / gd, dir[1] = gy / gd, speed = fminf(gd * 2.0f, run); // (arrives softly)
            if (gd < 25.0f) speed = 0.0f;
        } else { // roam: a goal in the spawner's disk, a new one when reached (its random from its index and the time)
            // (Q27.10) City Routine: each quarter hour an agent may go to an open Smart Object with room (half of them,
            // by its index and the quarter); there, it stands at its place around it 10-40 game minutes; closed, it leaves
            if (c->routine && j->nso && r->visit) {
                uint32_t v = r->visit[i];
                if (v && (v > j->nso || !j->so[v - 1].open)) { // (closed, or gone: back to roaming from here)
                    if (v <= j->nso) re_atomic_add(&j->cnt[v - 1], -1);
                    r->visit[i] = 0, v = 0;
                    r->goal_x[i] = px, r->goal_y[i] = py;
                }
                if (!v) {
                    uint32_t h = (i * 2654435761u) ^ ((uint32_t)(j->hour * 4.0f) * 40503u) ^ 0x51ED27u;
                    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12;
                    uint32_t pick = (h >> 8) % j->nso;
                    const CitySO* so = &j->so[pick];
                    if ((h & 1u) && so->open && j->cnt[pick] < (int32_t)so->slots) { // (a slot reserved: the steps run in parallel)
                        if (re_atomic_add(&j->cnt[pick], 1) <= (int32_t)so->slots) {
                            r->visit[i] = (uint16_t)(pick + 1u), v = pick + 1u;
                            r->stay[i] = (600.0f + 1800.0f * (float)((h >> 20) & 255u) / 255.0f) / fmaxf(j->scale, 1.0f);
                        } else re_atomic_add(&j->cnt[pick], -1);
                    }
                }
                if (v) {
                    const CitySO* so = &j->so[v - 1];
                    float ang = (float)(i % 16u) * 0.3927f, sx = so->pos[0] + cosf(ang) * 140.0f, sy = so->pos[1] + sinf(ang) * 140.0f;
                    r->goal_x[i] = sx, r->goal_y[i] = sy;
                    float gx = sx - px, gy = sy - py, gd = sqrtf(gx * gx + gy * gy);
                    if (gd > 40.0f) dir[0] = gx / gd, dir[1] = gy / gd, speed = walk * 0.9f;
                    else if ((r->stay[i] -= j->dt) <= 0.0f) r->visit[i] = 0, re_atomic_add(&j->cnt[v - 1], -1); // (done: off again)
                    goto routine_done;
                }
            }
            float gx = r->goal_x[i] - px, gy = r->goal_y[i] - py, gd = sqrtf(gx * gx + gy * gy);
            if (gd < 50.0f) {
                uint32_t h = (i * 2654435761u) ^ (uint32_t)(j->now * 1000.0f) * 40503u;
                if (!h) h = 1;
                float a = rndf(&h) * 6.2831853f, d = sqrtf(rndf(&h)) * rad;
                r->goal_x[i] = r->home[0] + cosf(a) * d, r->goal_y[i] = r->home[1] + sinf(a) * d;
                gx = r->goal_x[i] - px, gy = r->goal_y[i] - py, gd = sqrtf(gx * gx + gy * gy);
            }
            if (gd > 40.0f) dir[0] = gx / gd, dir[1] = gy / gd, speed = walk * 0.8f;
        }
    routine_done:;
        // personal space
        float sp[2];
        uint32_t press = separation(r, i, cs, hw, fmaxf(c->separation, 20.0f), sp);
        // packed in (over 6 close around it): it stops pushing on (the ones ahead can't move: a crowd waits)
        if (press > 6) speed *= fmaxf(0.0f, 1.0f - (float)(press - 6) * 0.25f);
        float k = r->speed_k[i];
        float vx = dir[0] * speed * k + sp[0] * walk * 1.5f, vy = dir[1] * speed * k + sp[1] * walk * 1.5f;
        float vl = sqrtf(vx * vx + vy * vy), vmax = run * k * 1.1f;
        if (vl > vmax) vx *= vmax / vl, vy *= vmax / vl;
        // inertia: it turns and speeds up over a quarter second
        float blendv = fminf(dt * 4.0f, 1.0f);
        r->vx[i] += (vx - r->vx[i]) * blendv, r->vy[i] += (vy - r->vy[i]) * blendv;
        float nx = px + r->vx[i] * dt, ny = py + r->vy[i] * dt;
        // the walls: off the walkable grid it slides along the axis that stays on it
        if (F.cells && !walkable(cell_of(nx, ny))) {
            if (walkable(cell_of(nx, py))) ny = py, r->vy[i] = 0.0f;
            else if (walkable(cell_of(px, ny))) nx = px, r->vx[i] = 0.0f;
            else nx = px, ny = py, r->vx[i] = r->vy[i] = 0.0f;
        }
        r->nx[i] = nx, r->ny[i] = ny;
        // the look: facing its way, the clips by its speed (idle - walk - run), the phases going at the speed's rate
        float s = sqrtf(r->vx[i] * r->vx[i] + r->vy[i] * r->vy[i]);
        if (s > 15.0f || (know && role != ROLE_ROAM)) { // (moving: its way; standing in the ring: facing the player)
            float want = s > 15.0f ? atan2f(r->vy[i], r->vx[i]) * 57.29578f : atan2f(ty - py, tx - px) * 57.29578f;
            float d = remainderf(want - r->yaw[i], 360.0f);
            r->yaw[i] += d * fminf(dt * 8.0f, 1.0f);
        }
        uint8_t ca, cb;
        float bl;
        if (s < walk) ca = 0, cb = 1, bl = s / walk;
        else ca = 1, cb = 2, bl = fminf((s - walk) / (run - walk), 1.0f);
        bl = bl * bl * (3.0f - 2.0f * bl);
        // (the phases: cycles per second; a clip covers its ground speed in one cycle: s / speed cycles of it per second)
        float sw = r->clip_speed[1] > 1.0f ? r->clip_speed[1] : walk, sr = r->clip_speed[2] > 1.0f ? r->clip_speed[2] : run;
        float rate_a = ca == 0 ? 1.0f / r->clip_len[0] : ca == 1 ? (s / sw) / r->clip_len[1] : (s / sr) / r->clip_len[2];
        float rate_b = cb == 1 ? (fmaxf(s, 1.0f) / sw) / r->clip_len[1] : (fmaxf(s, 1.0f) / sr) / r->clip_len[2];
        if (ca != r->clip_a[i]) r->pa[i] = r->pb[i]; // (walk to run: the cycle goes on where it was)
        r->clip_a[i] = ca, r->clip_b[i] = cb, r->blend[i] = bl;
        float rate = rate_a + (rate_b - rate_a) * bl;
        r->pa[i] = fmodf(r->pa[i] + rate * dt, 1.0f);
        r->pb[i] = r->pa[i]; // (synchronized: walk and run at the same phase)
    }
}

// The GPU's agents (in parallel, playing or not): on the floor (the grid's height), their LOD by the camera's distance
// (LOD 1 from 15 m: no shadows; LOD 2 from 30 m: the big parts only; nearer when too many are near: lod_distances)
typedef struct PackJob { CrowdRun* r; float cam[2], far1, far2; uint32_t per; } PackJob;
static void pack_agents(void* arg, int index) {
    PackJob* j = arg;
    CrowdRun* r = j->r;
    uint32_t a0 = (uint32_t)index * j->per, a1 = a0 + j->per;
    if (a1 > r->n) a1 = r->n;
    const float far1 = j->far1, far2 = j->far2;
    for (uint32_t i = a0; i < a1; i++) {
        r->z[i] = flow_height(r->x[i], r->y[i], r->home[2]);
        CrowdAgentGpu* g = &r->gpu[i];
        g->pos[0] = r->x[i], g->pos[1] = r->y[i], g->pos[2] = r->z[i];
        g->yaw = r->yaw[i];
        g->pa = r->pa[i], g->pb = r->pb[i];
        float dx = r->x[i] - j->cam[0], dy = r->y[i] - j->cam[1], d2 = dx * dx + dy * dy;
        uint32_t lod = d2 > CROWD_FAR3 * CROWD_FAR3 ? 3u : d2 > far2 * far2 ? 2u : d2 > far1 * far1 ? 1u : 0u;
        g->clips = (uint32_t)r->clip_a[i] | (uint32_t)r->clip_b[i] << 8 | lod << 16 | (uint32_t)(fminf(fmaxf(r->size_k[i], 0.5f), 1.5f) * 100.0f) << 24;
        g->blend = r->blend[i];
        g->look = r->look ? r->look[i] : 0u; // (Q27.9)
    }
}

// The LODs' distances with a budget: at most CROWD_FULL agents whole with shadows, CROWD_MID whole (the nearest ones);
// past those, the big parts only. A histogram of the distances (50 cm bins up to 30 m): one pass, no sort
#define CROWD_FULL 1500u
#define CROWD_MID 5000u
static void lod_distances(const CrowdRun* r, const float cam[2], float* far1, float* far2) {
    *far1 = 1500.0f, *far2 = 3000.0f;
    if (r->n <= CROWD_FULL) return;
    uint32_t hist[61] = { 0 };
    for (uint32_t i = 0; i < r->n; i++) {
        float dx = r->x[i] - cam[0], dy = r->y[i] - cam[1];
        int b = (int)(sqrtf(dx * dx + dy * dy) * (1.0f / 50.0f));
        hist[b < 60 ? b : 60]++;
    }
    uint32_t sum = 0;
    for (int b = 0; b < 60; b++) {
        sum += hist[b];
        float d = (float)(b + 1) * 50.0f;
        if (sum > CROWD_FULL && d - 50.0f < *far1) *far1 = fmaxf(d - 50.0f, 300.0f);
        if (sum > CROWD_MID && d - 50.0f < *far2) { *far2 = fmaxf(d - 50.0f, *far1); break; }
    }
}

// The ring's layers: concentric circles, one personal space apart, each holding its circumference; as many as the ring's
// agents need
static uint32_t ring_layers(const CCrowd* c, uint32_t nring) {
    float sep = fmaxf(c->separation, 20.0f) * 1.15f, ring = fmaxf(c->ring_radius, 100.0f);
    uint32_t L = 0, held = 0;
    while (held < nring && L < 64) held += (uint32_t)(6.2831853f * (ring + (float)L * sep) / sep), L++;
    return L ? L : 1;
}

// The decisions: who knows where the player is, who charges and who takes which slot of the ring
static void think(CrowdRun* r, float dt, float now, float target[2], bool* known, float ring_center[2]) {
    const CCrowd* c = &r->c;
    float reaction = fmaxf(c->reaction, 0.02f);
    if (!C.has_player) {
        *known = false;
        for (uint32_t i = 0; i < r->n; i++) r->knows[i] = 0, r->role[i] = ROLE_ROAM;
    }
    if (!C.has_player) return;
    float sight2 = c->sight * c->sight;
    if (c->mind == CROWD_HIVE) {
        r->hive_think -= dt;
        if (r->hive_think > 0.0f) {
            *known = r->hive_knows;
            return;
        }
        r->hive_think = reaction;
        // one sees, all know (sight: the distance; walls: one ray from the nearest few)
        uint32_t near_i = UINT32_MAX;
        float nd = 1e30f;
        for (uint32_t i = 0; i < r->n; i++) {
            float dx = C.player[0] - r->x[i], dy = C.player[1] - r->y[i], d2 = dx * dx + dy * dy;
            if (d2 < nd) nd = d2, near_i = i;
        }
        bool sees = nd < sight2;
        if (sees && C.playing && near_i != UINT32_MAX) {
            float a[3] = { r->x[near_i], r->y[near_i], r->z[near_i] + 150.0f }, b[3] = { C.player[0], C.player[1], C.player[2] + 50.0f };
            sees = bp_trace_boxes(a, b, 0.0f) >= 1.0f;
        }
        if (sees) r->hive_knows = true, r->hive_mem[0] = C.player[0], r->hive_mem[1] = C.player[1], r->hive_mem_t = now;
        else if (now - r->hive_mem_t > fmaxf(c->memory, 0.5f) * 2.0f) r->hive_knows = false;
        *known = r->hive_knows;
        if (!sees && r->hive_knows) target[0] = r->hive_mem[0], target[1] = r->hive_mem[1];
        // the roles: the aggressive share (the nearest) charges; the rest takes the ring, slots by their angle around it
        uint32_t n = r->n, ncharge = (uint32_t)(fminf(fmaxf(c->aggression, 0.0f), 1.0f) * (float)n + 0.5f);
        if (c->behavior == CROWD_SEEK) ncharge = n;
        if (c->behavior == CROWD_FLEE) {
            for (uint32_t i = 0; i < n; i++) r->role[i] = ROLE_FLEE;
            return;
        }
        // (a counting sort by distance in 64 bands: the nearest charge)
        static uint32_t band_count[65];
        memset(band_count, 0, sizeof band_count);
        float maxd = 1.0f;
        for (uint32_t i = 0; i < n; i++) {
            float dx = target[0] - r->x[i], dy = target[1] - r->y[i];
            r->nx[i] = sqrtf(dx * dx + dy * dy); // (nx: scratch here, rewritten by the step)
            maxd = fmaxf(maxd, r->nx[i]);
        }
        for (uint32_t i = 0; i < n; i++) band_count[(uint32_t)(r->nx[i] / maxd * 63.0f)]++;
        uint32_t cut_band = 64, acc = 0;
        for (uint32_t b = 0; b < 64; b++) {
            acc += band_count[b];
            if (acc >= ncharge) {
                cut_band = b;
                break;
            }
        }
        uint32_t nring = 0;
        for (uint32_t i = 0; i < n; i++) {
            bool charge = ncharge && (uint32_t)(r->nx[i] / maxd * 63.0f) <= cut_band && ncharge > 0;
            r->role[i] = charge ? ROLE_CHASE : ROLE_RING;
            nring += !charge;
        }
        r->n_chase = n - nring, r->n_ring = nring, r->n_know = r->hive_knows ? n : 0;
        // the ring's slots: the angles of the ring agents around the center, in order (a counting sort in 256 sectors)
        // so neighbors on the ring are neighbors now: nobody crosses the circle
        static uint32_t sec_count[257];
        memset(sec_count, 0, sizeof sec_count);
        for (uint32_t i = 0; i < n; i++)
            if (r->role[i] == ROLE_RING) {
                float a = atan2f(r->y[i] - target[1], r->x[i] - target[0]) / 6.2831853f + 0.5f;
                uint32_t s = (uint32_t)(a * 255.999f);
                r->slot[i] = (uint32_t)s, sec_count[s + 1]++;
            }
        for (uint32_t s = 0; s < 256; s++) sec_count[s + 1] += sec_count[s];
        for (uint32_t i = 0; i < n; i++)
            if (r->role[i] == ROLE_RING) {
                uint32_t pos = sec_count[r->slot[i]]++;
                r->slot[i] = (uint32_t)pos; // (its place in angle order: the step turns it into a layer and an angle)
            }
        r->nring = nring, r->layers = ring_layers(c, nring);
        ring_center[0] = target[0], ring_center[1] = target[1];
        return;
    }
    // Individual: each one on its own clock: sees (distance, then a ray for a few each frame), remembers, decides
    uint32_t rays = 64;
    for (uint32_t i = 0; i < r->n; i++) {
        r->think[i] -= dt;
        if (r->think[i] > 0.0f) continue;
        r->think[i] = reaction * (0.75f + 0.5f * rndf(&C.rng));
        float dx = C.player[0] - r->x[i], dy = C.player[1] - r->y[i];
        bool sees = dx * dx + dy * dy < sight2;
        if (sees && C.playing && rays) {
            rays--;
            float a[3] = { r->x[i], r->y[i], r->z[i] + 150.0f }, b[3] = { C.player[0], C.player[1], C.player[2] + 50.0f };
            sees = bp_trace_boxes(a, b, 0.0f) >= 1.0f;
        }
        if (sees) { // it aims ahead of the player too, by its own prediction
            r->knows[i] = 1, r->mem_t[i] = now;
            r->mem_x[i] = target[0], r->mem_y[i] = target[1];
        } else if (now - r->mem_t[i] > fmaxf(c->memory, 0.1f))
            r->knows[i] = 0;
        if (c->behavior == CROWD_FLEE) r->role[i] = ROLE_FLEE;
        else if (c->behavior == CROWD_SEEK) r->role[i] = ROLE_CHASE;
        else if (c->behavior == CROWD_SURROUND) {
            r->role[i] = rndf(&C.rng) < c->aggression ? ROLE_CHASE : ROLE_RING;
            // (its slot: its own angle around the player: an individual keeps its side, nobody shares them out)
            float a = atan2f(r->y[i] - target[1], r->x[i] - target[0]) / 6.2831853f + 0.5f;
            r->slot[i] = (uint32_t)(a * (float)r->n);
        } else
            r->role[i] = ROLE_ROAM;
    }
    r->nring = r->n, r->layers = ring_layers(c, r->n); // (an individual's slot: its own angle over the whole crowd)
    r->n_chase = r->n_ring = r->n_know = 0;
    for (uint32_t i = 0; i < r->n; i++)
        r->n_chase += r->role[i] == ROLE_CHASE, r->n_ring += r->role[i] == ROLE_RING, r->n_know += r->knows[i];
    *known = true; // (each reads its own knows[])
    ring_center[0] = target[0], ring_center[1] = target[1];
}

static float g_now;

// (Q27.1) The alarms' waves: each front grows CROWD_WAVE cm/s (the shot heard, the others seen running) up to its
// radius; the calm agents it passed are promoted to panic, CROWD_PROMOTE at most a frame for all the crowds, the
// front's order keeping the nearest first. GTA 6 woke dozens of NPCs' expensive AI in one frame at a shot into an
// airport crowd and the frame fell to ~20 fps: here a panic's cost is spread over the frames by design
#define CROWD_WAVE 1500.0f
#define CROWD_PROMOTE 64u
void crowd_alarm(const float at[3], float radius) {
    int k = 0;
    while (k < 8 && C.alarm[k].on) k++;
    if (k == 8) k = (int)(C.frame % 8u); // (all eight busy: one of them gives way)
    C.alarm[k].x = at[0], C.alarm[k].y = at[1], C.alarm[k].radius = fmaxf(radius, 100.0f), C.alarm[k].front = 0.0f, C.alarm[k].on = true;
    C.alarms_total++;
}

static void alarms_promote(float dt) {
    uint32_t budget = CROWD_PROMOTE, promoted = 0;
    for (int k = 0; k < 8; k++) {
        if (!C.alarm[k].on) continue;
        float front = C.alarm[k].front = fminf(C.alarm[k].front + CROWD_WAVE * dt, C.alarm[k].radius);
        bool pending = false;
        for (uint32_t ri = 0; ri < C.nruns; ri++) {
            CrowdRun* r = &C.runs[ri];
            for (uint32_t i = 0; i < r->n; i++) {
                if (r->panic[i] > 0.0f) continue;
                float dx = r->x[i] - C.alarm[k].x, dy = r->y[i] - C.alarm[k].y, d2 = dx * dx + dy * dy;
                if (d2 > C.alarm[k].radius * C.alarm[k].radius) continue;
                if (d2 > front * front || !budget) {
                    pending = true;
                    continue;
                }
                uint32_t h = i * 2246822519u + 3u;
                r->panic[i] = 6.0f + rndf(&h) * 6.0f; // (6-12 s: they don't all calm down at once either)
                r->pan_x[i] = C.alarm[k].x, r->pan_y[i] = C.alarm[k].y;
                budget--, promoted++;
            }
        }
        if (!pending && front >= C.alarm[k].radius) C.alarm[k].on = false;
    }
    C.promoted = promoted;
    if (promoted > C.stats_peak) C.stats_peak = promoted;
}

void crowd_update(World* w, float dt, bool playing, const float camera[3]) {
    double t0 = re_time_seconds();
    if (dt > 0.1f) dt = 0.1f;
    city_update(w, dt, playing); // (Q27.10: the city's clock and Smart Objects, with the crowds that use them)
    CitySO* so = NULL;
    uint32_t nso = city_smart_objects(&so);
    static volatile int32_t so_cnt[256]; // (each Smart Object's visitors over every crowd: then reserved by the steps)
    if (nso > 256) nso = 256;
    for (uint32_t k = 0; k < nso; k++) so_cnt[k] = 0;
    for (uint32_t ri = 0; ri < C.nruns; ri++) {
        const CrowdRun* r = &C.runs[ri];
        if (!r->c.routine || !r->visit) continue;
        for (uint32_t i = 0; i < r->n; i++)
            if (r->visit[i] && r->visit[i] <= nso) so_cnt[r->visit[i] - 1]++;
    }
    g_now += dt;
    if (!C.rng) C.rng = 0x9E3779B9u;
    sync_runs(w);
    double flow_ms = C.stats.flow_ms;
    memset(&C.stats, 0, sizeof C.stats);
    C.stats.flow_ms = flow_ms;
    C.stats.crowds = C.nruns;
    // the player and where it will be (its velocity from its last places)
    Entity pl = playing ? bp_player(w) : ENTITY_NONE;
    const CTransform* pt = pl ? ecs_get(w, pl, COMP_TRANSFORM) : NULL;
    C.has_player = pt != NULL;
    if (pt) {
        memcpy(C.player, pt->location, 12);
        if (C.had_player && dt > 1e-4f)
            for (int k = 0; k < 3; k++) C.player_v[k] += ((C.player[k] - C.player_prev[k]) / dt - C.player_v[k]) * fminf(dt * 6.0f, 1.0f);
        memcpy(C.player_prev, C.player, 12);
    }
    C.had_player = C.has_player;
    C.frame++;
    if (playing) alarms_promote(dt);
    for (uint32_t ri = 0; ri < C.nruns; ri++) {
        CrowdRun* r = &C.runs[ri];
        if (!r->n) continue;
        C.stats.agents += r->n;
        C.stats.looks += r->nlooks; // (Q27.9)
        if (playing) {
            // the target: where the player will be, prediction seconds ahead (at most 15 m)
            float pred = fminf(fmaxf(r->c.prediction, 0.0f), 5.0f), tgt[2] = { C.player[0] + C.player_v[0] * pred, C.player[1] + C.player_v[1] * pred };
            float lx = tgt[0] - C.player[0], ly = tgt[1] - C.player[1], ll = sqrtf(lx * lx + ly * ly);
            if (ll > 1500.0f) tgt[0] = C.player[0] + lx / ll * 1500.0f, tgt[1] = C.player[1] + ly / ll * 1500.0f;
            if (C.has_player) flow_target(tgt[0], tgt[1]);
            bool known = false;
            float ring_c[2] = { tgt[0], tgt[1] };
            double tt0 = re_time_seconds();
            think(r, dt, g_now, tgt, &known, ring_c);
            double tt1 = re_time_seconds();
            C.stats.think_ms += (tt1 - tt0) * 1000.0;
            // the spatial hash of this run
            uint32_t hw = 1024;
            while (hw < r->n * 2u) hw *= 2u;
            if (hw > C.hash_cap || r->n > C.items_cap) {
                free(C.cell_start), free(C.cell_items);
                C.hash_cap = hw, C.items_cap = r->n;
                C.cell_start = malloc(sizeof(uint32_t) * (hw + 2)), C.cell_items = malloc(sizeof(uint32_t) * (r->n + 1));
                if (!C.cell_start || !C.cell_items) {
                    C.hash_cap = C.items_cap = 0;
                    continue;
                }
            }
            hw = C.hash_cap;
            float cs = fmaxf(r->c.separation, 30.0f);
            memset(C.cell_start, 0, sizeof(uint32_t) * (hw + 2));
            for (uint32_t i = 0; i < r->n; i++) {
                int cx = (int)floorf(r->x[i] / cs), cy = (int)floorf(r->y[i] / cs);
                C.cell_start[(((uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u) & (hw - 1u)) + 1]++;
            }
            for (uint32_t h = 0; h < hw; h++) C.cell_start[h + 1] += C.cell_start[h];
            static uint32_t* fill;
            static uint32_t fill_cap;
            if (fill_cap < hw) free(fill), fill = malloc(sizeof(uint32_t) * hw), fill_cap = fill ? hw : 0;
            if (!fill) continue;
            memcpy(fill, C.cell_start, sizeof(uint32_t) * hw);
            for (uint32_t i = 0; i < r->n; i++) {
                int cx = (int)floorf(r->x[i] / cs), cy = (int)floorf(r->y[i] / cs);
                C.cell_items[fill[((uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u) & (hw - 1u)]++] = i;
            }
            double tt2 = re_time_seconds();
            C.stats.hash_ms += (tt2 - tt1) * 1000.0;
            // the step, in parallel (positions into nx / ny, then swapped)
            StepJob job = { r, dt, g_now, { tgt[0], tgt[1] }, { ring_c[0], ring_c[1] }, known, 512, C.frame, so, nso, city_hour(), city_time_scale(), so_cnt };
            if (r->c.mind == CROWD_INDIVIDUAL) job.target_known = true;
            re_parallel_for((int)((r->n + job.per - 1) / job.per), step_agents, &job);
            C.stats.step_ms += (re_time_seconds() - tt2) * 1000.0;
            float* t = r->x;
            r->x = r->nx, r->nx = t;
            t = r->y, r->y = r->ny, r->ny = t;
            C.stats.chasing += r->n_chase, C.stats.surrounding += r->n_ring, C.stats.knowing += r->n_know; // (counted when they decided)
        }
        PackJob pj = { r, { camera[0], camera[1] }, 1500.0f, 3000.0f, 1024 };
        double tp0 = re_time_seconds();
        lod_distances(r, pj.cam, &pj.far1, &pj.far2);
        re_parallel_for((int)((r->n + pj.per - 1) / pj.per), pack_agents, &pj);
        C.stats.pack_ms += (re_time_seconds() - tp0) * 1000.0;
    }
    C.stats.promoted = C.promoted, C.stats.promote_peak = C.stats_peak, C.stats.alarms = C.alarms_total;
    for (uint32_t ri = 0; ri < C.nruns; ri++) // (Q27.1) the panicked now
        for (uint32_t i = 0; i < C.runs[ri].n; i++) C.stats.panicked += C.runs[ri].panic[i] > 0.0f;
    for (uint32_t k = 0; k < nso; k++) so[k].visitors = (uint32_t)(so_cnt[k] > 0 ? so_cnt[k] : 0); // (Q27.10: the city's counts)
    C.stats.sim_ms = (re_time_seconds() - t0) * 1000.0;
    C.stats.flow_builds = F.builds;
}

// (Q27.10) the agent nearest a point within radius (the witness of a crime): its place
bool crowd_nearest(const float at[3], float radius, float out[3]) {
    float best = radius * radius;
    bool got = false;
    for (uint32_t ri = 0; ri < C.nruns; ri++) {
        const CrowdRun* r = &C.runs[ri];
        for (uint32_t i = 0; i < r->n; i++) {
            float dx = r->x[i] - at[0], dy = r->y[i] - at[1], d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2, got = true, out[0] = r->x[i], out[1] = r->y[i], out[2] = r->z[i];
        }
    }
    return got;
}

void crowd_begin_play(World* w) {
    C.playing = true;
    city_reset(); // (Q27.10)
    if (!ai_nav_info() || !ai_nav_info()->w) ai_nav_build(w);
    flow_start();
    // (made again from their spawners: the play starts from where the editor shows them; the buffers kept)
    for (uint32_t i = 0; i < C.nruns; i++) C.runs[i].cfg_hash = 0, C.runs[i].n = 0;
    C.has_player = C.had_player = false;
    memset(C.player_v, 0, sizeof C.player_v);
    memset(C.alarm, 0, sizeof C.alarm), C.stats_peak = C.promoted = C.alarms_total = 0; // (Q27.1)
}

void crowd_end_play(World* w) {
    (void)w;
    C.playing = false;
    flow_stop();
    for (uint32_t i = 0; i < C.nruns; i++) C.runs[i].cfg_hash = 0, C.runs[i].n = 0; // (back where they started)
}

uint32_t crowd_draws(const CrowdDraw** out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.nruns; i++) {
        const CrowdRun* r = &C.runs[i];
        if (!r->n || !r->baked || !r->nparts) continue;
        CrowdDraw* d = &C.draws[n++];
        d->e = r->e, d->mesh = r->mesh, d->nagents = r->n, d->nparts = r->nparts;
        d->agents = r->gpu, d->baked = r->baked, d->big = r->big, d->version = r->version;
    }
    *out = C.draws;
    return n;
}

uint32_t crowd_instances(void) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < C.nruns; i++)
        if (C.runs[i].baked) n += (uint64_t)C.runs[i].n * C.runs[i].nparts;
    return n > CROWD_MAX_INSTANCES ? CROWD_MAX_INSTANCES : (uint32_t)n;
}

const CrowdStats* crowd_stats(void) { return &C.stats; }

float crowd_mean_distance(World* w, Entity spawner, const float at[3]) {
    (void)w;
    const CrowdRun* r = run_for(spawner);
    if (!r || !r->n) return -1.0f;
    double s = 0.0;
    for (uint32_t i = 0; i < r->n; i++) s += sqrt((double)(r->x[i] - at[0]) * (r->x[i] - at[0]) + (double)(r->y[i] - at[1]) * (r->y[i] - at[1]));
    return (float)(s / r->n);
}

uint32_t crowd_count_near(const float at[3], float radius) {
    uint32_t n = 0;
    for (uint32_t ri = 0; ri < C.nruns; ri++)
        for (uint32_t i = 0; i < C.runs[ri].n; i++) {
            float dx = C.runs[ri].x[i] - at[0], dy = C.runs[ri].y[i] - at[1];
            n += dx * dx + dy * dy <= radius * radius;
        }
    return n;
}
