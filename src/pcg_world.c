// The PCG component at work (Q15, pcg.h; notas-dev/pcg.md). Each PCG actor (a Procedural World in mode PROCGEN_PCG)
// is a "unit" of work: its whole box, or (Partitioned) each grid cell of it near the streaming source. A unit goes
// queued -> generated (its points made and its spawns listed, on a worker, in its own arena) -> spawning (the entities
// made on the main thread within the frame's budget) -> done; a cell far again is dropped (its entities destroyed).
// The editor generates a box at once when something it reads changes (its settings, its graph, its transform, the
// meshes and splines in it, the ground's heights); the game when the level loads (On Load) or when asked (On Demand),
// a partitioned one cell by cell. What it made is flagged TRANSFORM_PCG (its meshes are ATTACH_PCG children of the
// actor): never saved (pcg_purge before a save), made again from the graph, the same by the seed.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "debug_viewer.h"
#include "landscape.h"
#include "pcg.h"
#include "platform.h"
#include "procgen.h"
#include "render.h"
#include "river.h"
#include "spline.h"
#include "world_partition.h"

const char* const g_pcg_trigger_names[PCG_TRIGGER_COUNT] = { "Generate on Load", "Generate on Demand" };

#define MAX_COMPS 64
#define MAX_UNITS 2048
#define MAX_JOBS 16
#define PCG_BATCH_SECONDS 0.5 // (the cells are made 256 m ahead: half a second late doesn't show)
enum { U_FREE = 0, U_QUEUED, U_SPAWNING, U_DONE, U_DROP }; // (U_DROP: far, its entities go with the next batch)

typedef struct Unit {
    uint8_t state;
    bool whole, wanted;
    uint16_t comp;
    int32_t cx, cy;
    float dist2;
    float bmin[3], bmax[3];
    ReVmBuf spawns, ents, dbg;
    uint32_t nspawns, next, nents, ndbg, points;
    double gen_ms;
    uint64_t hash;
    char error[128];
} Unit;

typedef struct Comp {
    Entity e;
    bool seen, dirty, generated;
    uint32_t hash;          // what it reads (the editor's change test)
    float bmin[3], bmax[3];
    float cell, range;
    bool partitioned;
    PcgSources src;         // gathered for the whole box (a cell takes its share)
    ReVmBuf src_mesh, src_spline;
} Comp;

static struct {
    World* w;
    bool playing, purge_all;
    uint32_t revision;
    Comp comp[MAX_COMPS];
    uint32_t ncomp;
    Unit units[MAX_UNITS];
    ReArena arena[MAX_JOBS];
    bool arenas;
    PcgStats st;
    uint32_t last_changes;
    PcgSplineFn providers[8];
    uint32_t nproviders;
} S;

static inline uint32_t fnv(const void* p, size_t n, uint32_t h) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// --- Splines: the Rail and the River built in ---
static uint32_t finish(PcgSplinePoint* o, uint32_t n) { // the distances and the tangents of a polyline
    if (n < 2) return 0;
    o[0].dist = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = i ? i - 1 : 0, b = i + 1 < n ? i + 1 : n - 1;
        float d[3] = { o[b].p[0] - o[a].p[0], o[b].p[1] - o[a].p[1], o[b].p[2] - o[a].p[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) o[i].t[k] = l > 1e-6f ? d[k] / l : (k == 0 ? 1.0f : 0.0f);
        if (i) {
            float e[3] = { o[i].p[0] - o[i - 1].p[0], o[i].p[1] - o[i - 1].p[1], o[i].p[2] - o[i - 1].p[2] };
            o[i].dist = o[i - 1].dist + sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        }
    }
    return n;
}
// A Spline Component (Rail, Spline or Cable: spline.h) by the common curve: its point types, tangents, roll and scale
static uint32_t rail_provider(World* w, Entity e, PcgSplinePoint* out, uint32_t cap) {
    const CRail* r = ecs_get(w, e, COMP_SPLINE);
    if (!r || cap < 2) return 0;
    SplineCurve c; // (~10 KB on this thread's stack: the gather may run on a worker)
    if (!spline_world_curve(w, e, &c) || c.n < 2 || c.length <= 0.0f) return 0;
    uint32_t want = spline_segment_count(&c) * 16u + 1u, cnt = want < cap ? want : cap;
    float step = c.length / (float)(cnt - 1), base = spline_kind(r) == SPLINE_KIND_RAIL ? r->gauge + 150.0f : 100.0f;
    for (uint32_t m = 0; m < cnt; m++) {
        float right[3], up[3], sc[2];
        spline_frame_at_key(&c, spline_key_at_distance(&c, m + 1 == cnt ? c.length : step * (float)m), out[m].p, out[m].t, right, up, sc);
        out[m].width = spline_kind(r) == SPLINE_KIND_RAIL ? base : base * sc[0]; // (sc[0]: the scale across, Y)
    }
    return finish(out, cnt);
}
static uint32_t river_provider(World* w, Entity e, PcgSplinePoint* out, uint32_t cap) {
    const CRiver* r = ecs_get(w, e, COMP_RIVER);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!r || !t) return 0;
    static RiverSample rs[RIVER_SAMPLES];
    uint32_t n = river_samples(r, t, rs, RIVER_SAMPLES);
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) memcpy(out[i].p, rs[i].p, 12), out[i].width = rs[i].width;
    return finish(out, n);
}
void pcg_spline_add_provider(PcgSplineFn fn) {
    for (uint32_t i = 0; i < S.nproviders; i++)
        if (S.providers[i] == fn) return;
    if (S.nproviders < 8) S.providers[S.nproviders++] = fn;
}
uint32_t pcg_spline_sample(World* w, Entity e, PcgSplinePoint* out, uint32_t cap) {
    if (!S.nproviders) pcg_spline_add_provider(rail_provider), pcg_spline_add_provider(river_provider);
    for (uint32_t i = 0; i < S.nproviders; i++) {
        uint32_t n = S.providers[i](w, e, out, cap);
        if (n >= 2) return n;
    }
    return 0;
}

// --- The components ---
bool pcg_is_component(World* w, Entity e) {
    const CProcWorld* c = e ? ecs_get(w, e, COMP_PROC_WORLD) : NULL;
    return c && c->mode == PROCGEN_PCG;
}
static CProcWorld* settings(World* w, Entity e) {
    CProcWorld* c = ecs_get(w, e, COMP_PROC_WORLD);
    return c && c->mode == PROCGEN_PCG ? c : NULL;
}

bool pcg_component_bounds(World* w, Entity e, float mn[3], float mx[3]) {
    const CProcWorld* c = settings(w, e);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!c || !t) return false;
    for (int a = 0; a < 3; a++) {
        float h = fabsf(c->pcg.extent[a] * t->scale[a]);
        mn[a] = t->location[a] - h, mx[a] = t->location[a] + h;
    }
    return true;
}

Entity pcg_spawn_volume(World* w, const float at[3], const char* graph_vpath) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROC_WORLD));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "PCGVolume");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    CProcWorld* c = ecs_get(w, e, COMP_PROC_WORLD);
    c->mode = PROCGEN_PCG;
    c->pcg.seed = 42u, c->pcg.extent[0] = c->pcg.extent[1] = 5000.0f, c->pcg.extent[2] = 5000.0f;
    c->pcg.flags = PCGC_ON_CHANGE, c->pcg.trigger = PCG_TRIGGER_ON_LOAD;
    if (graph_vpath && graph_vpath[0]) pcg_graph_remember(graph_vpath), c->pcg.graph = pcg_vpath_hash(graph_vpath);
    return e;
}

static Comp* comp_of(Entity e) {
    for (uint32_t i = 0; i < S.ncomp; i++)
        if (S.comp[i].e == e) return &S.comp[i];
    return NULL;
}

// What a component reads, gathered for its box: the static meshes (not the generated ones, not the landscape's chunks:
// the landscape is read by its height), the splines; a hash of all of it and of the ground's heights
static uint32_t gather(World* w, Comp* c, const CProcWorld* cfg) {
    PcgSources* s = &c->src;
    s->nmesh = 0, s->nspline = 0;
    if (!c->src_mesh.reserved) re_vm_reserve(&c->src_mesh, 64u << 20), re_vm_reserve(&c->src_spline, sizeof(PcgSplinePoint) * PCG_MAX_SPLINES * PCG_SPLINE_PTS + 65536);
    uint32_t h = 2166136261u;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!ar->count || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
        const CAttach* at = (ar->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(ar, COMP_ATTACH) : NULL;
        const Entity* ents = ecs_arch_entities(ar);
        if (ar->mask & COMP_BIT(COMP_STATIC_MESH)) {
            const CStaticMesh* sm = ecs_arch_column(ar, COMP_STATIC_MESH);
            for (uint32_t i = 0; i < ar->count; i++) {
                if ((tr[i].flags & (TRANSFORM_PCG | TRANSFORM_HIDDEN)) || (at && at[i].kind != ATTACH_SCS)) continue;
                float rows[3][4], lo[3], hi[3], wl[3], wh[3];
                render_transform_rows(&tr[i], rows);
                render_mesh_bounds(sm[i].mesh, lo, hi);
                for (int a = 0; a < 3; a++) {
                    float m = rows[a][3] + rows[a][0] * (lo[0] + hi[0]) * 0.5f + rows[a][1] * (lo[1] + hi[1]) * 0.5f + rows[a][2] * (lo[2] + hi[2]) * 0.5f, e = 0.0f;
                    for (int k = 0; k < 3; k++) e += fabsf(rows[a][k]) * (hi[k] - lo[k]) * 0.5f;
                    wl[a] = m - e, wh[a] = m + e;
                }
                bool in = true;
                for (int a = 0; a < 3; a++) in &= wh[a] >= c->bmin[a] && wl[a] <= c->bmax[a];
                if (!in || !re_vm_ensure(&c->src_mesh, (size_t)(s->nmesh + 1) * sizeof(PcgMeshSource))) continue;
                PcgMeshSource* ms = &((PcgMeshSource*)c->src_mesh.base)[s->nmesh++];
                ms->mesh = sm[i].mesh;
                memcpy(ms->rows, rows, sizeof rows), memcpy(ms->mn, wl, 12), memcpy(ms->mx, wh, 12);
                h = fnv(ms, sizeof *ms, h);
            }
        }
        if (at) continue; // (the splines: actors)
        for (uint32_t i = 0; i < ar->count && s->nspline < PCG_MAX_SPLINES; i++) {
            if (tr[i].flags & TRANSFORM_PCG) continue;
            PcgSplinePoint* sp = (PcgSplinePoint*)c->src_spline.base + (size_t)s->nspline * PCG_SPLINE_PTS;
            if (!re_vm_ensure(&c->src_spline, (size_t)(s->nspline + 1) * PCG_SPLINE_PTS * sizeof(PcgSplinePoint))) break;
            if (!(ar->mask & (COMP_BIT(COMP_RAIL) | COMP_BIT(COMP_RIVER))) && S.nproviders <= 2) continue; // (the built-ins: only those)
            uint32_t n = pcg_spline_sample(w, ents[i], sp, PCG_SPLINE_PTS);
            if (n < 2) continue;
            float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
            for (uint32_t k = 0; k < n; k++) lo[0] = fminf(lo[0], sp[k].p[0] - sp[k].width), lo[1] = fminf(lo[1], sp[k].p[1] - sp[k].width), hi[0] = fmaxf(hi[0], sp[k].p[0] + sp[k].width), hi[1] = fmaxf(hi[1], sp[k].p[1] + sp[k].width);
            if (hi[0] < c->bmin[0] || lo[0] > c->bmax[0] || hi[1] < c->bmin[1] || lo[1] > c->bmax[1]) continue;
            const CName* nm = (ar->mask & COMP_BIT(COMP_NAME)) ? &((const CName*)ecs_arch_column(ar, COMP_NAME))[i] : NULL;
            snprintf(s->spline_name[s->nspline], sizeof s->spline_name[0], "%s", nm ? nm->text : "");
            s->spline[s->nspline] = sp, s->spline_n[s->nspline] = n;
            h = fnv(sp, sizeof *sp * n, h);
            s->nspline++;
        }
    }
    s->mesh = (PcgMeshSource*)c->src_mesh.base;
    // the ground: its heights on a coarse grid (a sculpt regenerates)
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            float z = 0.0f;
            landscape_height(c->bmin[0] + (c->bmax[0] - c->bmin[0]) * x / 8.0f, c->bmin[1] + (c->bmax[1] - c->bmin[1]) * y / 8.0f, &z);
            h = fnv(&z, 4, h);
        }
    (void)cfg;
    return h;
}

static uint32_t settings_hash(World* w, Entity e, const CProcWorld* c) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    uint32_t h = fnv(&c->pcg, sizeof c->pcg, 2166136261u);
    h = fnv(t->location, 12, fnv(t->scale, 12, h));
    const PcgGraph* g = pcg_graph_get(c->pcg.graph);
    return h ^ (g ? pcg_graph_prepare(g, 0) : 0x9E3779B9u);
}

// --- Units ---
static void unit_reserve(Unit* u) {
    if (!u->spawns.reserved) re_vm_reserve(&u->spawns, 256u << 20);
    if (!u->ents.reserved) re_vm_reserve(&u->ents, 64u << 20);
    if (!u->dbg.reserved) re_vm_reserve(&u->dbg, 16u << 20);
}

static void destroy_ents(World* w, Unit* u) {
    Entity* es = (Entity*)u->ents.base;
    bool actors = false;
    for (uint32_t i = 0; i < u->nents; i++) {
        if (!ecs_alive(w, es[i])) continue;
        bool child = (ecs_mask(w, es[i]) & COMP_BIT(COMP_ATTACH)) != 0;
        if (!child) actors = true;
        if (S.playing) bp_destroy_actor(w, es[i], false); // (the Characters' boxes swept)
        else ecs_destroy(w, es[i]);
    }
    if (actors) { // a Blueprint actor's components (owned by a dead actor now)
        static Entity dead[8192];
        for (int round = 0; round < 64; round++) {
            uint32_t nd = 0;
            for (uint32_t ai = 0; ai < w->archetype_count && nd < 8192; ai++) {
                const EcsArchetype* ar = &w->archetypes[ai];
                if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
                const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
                for (uint32_t i = 0; i < ar->count && nd < 8192; i++)
                    if (at[i].kind == ATTACH_SCS && at[i].owner && !ecs_alive(w, at[i].owner)) dead[nd++] = ecs_arch_entities(ar)[i];
            }
            for (uint32_t i = 0; i < nd; i++) ecs_destroy(w, dead[i]);
            if (nd < 8192) break;
        }
    }
    u->nents = 0;
}

static void unit_drop(World* w, Unit* u) {
    if (u->state == U_FREE) return;
    destroy_ents(w, u);
    u->state = U_FREE, u->nspawns = u->next = u->ndbg = 0, u->wanted = false;
}

static Unit* unit_new(uint16_t comp, bool whole, int32_t cx, int32_t cy) {
    for (uint32_t i = 0; i < MAX_UNITS; i++)
        if (S.units[i].state == U_FREE) {
            Unit* u = &S.units[i];
            unit_reserve(u);
            u->state = U_QUEUED, u->comp = comp, u->whole = whole, u->cx = cx, u->cy = cy, u->wanted = true;
            u->nspawns = u->next = u->nents = u->ndbg = u->points = 0, u->error[0] = 0;
            return u;
        }
    return NULL;
}

static void comp_drop_units(World* w, uint16_t ci) {
    for (uint32_t i = 0; i < MAX_UNITS; i++)
        if (S.units[i].state != U_FREE && S.units[i].comp == ci) unit_drop(w, &S.units[i]);
}

// Everything generated in the world (flagged TRANSFORM_PCG), whoever made it (an undo, a restored snapshot, a loaded
// level may bring back entities the units don't know)
static void purge_world(World* w) {
    static Entity dead[16384];
    for (int round = 0; round < 1024; round++) {
        uint32_t n = 0;
        for (uint32_t ai = 0; ai < w->archetype_count && n < 16384; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
            const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t r = 0; r < ar->count && n < 16384; r++)
                if (t[r].flags & TRANSFORM_PCG) dead[n++] = ents[r];
        }
        for (uint32_t i = 0; i < n; i++) ecs_destroy(w, dead[i]);
        if (n < 16384) break;
    }
    for (uint32_t i = 0; i < MAX_UNITS; i++) S.units[i].state = U_FREE, S.units[i].nents = 0;
    for (uint32_t i = 0; i < S.ncomp; i++) S.comp[i].generated = false, S.comp[i].hash = 0;
}

void pcg_purge(World* w) {
    purge_world(w);
    S.revision = w->revision + 1u; // (made again on the next update)
}

// --- Generation (a worker) ---
typedef struct Job { Unit* u; const PcgGraph* g; PcgSources src; uint32_t seed; float params[PCG_MAX_PARAMS]; bool debug_all; uint32_t pts[PCG_MAX_NODES]; float ms[PCG_MAX_NODES]; } Job;
static Job g_jobs[MAX_JOBS];
static void last_store(uint32_t graph, const uint32_t* pts, const float* ms);

static void gen_job(void* arg, int k) {
    (void)arg;
    Job* j = &g_jobs[k];
    Unit* u = j->u;
    double t0 = re_time_seconds();
    re_arena_reset(&S.arena[k]);
    PcgRun r = { 0 };
    r.arena = &S.arena[k], r.spawns = &u->spawns, r.debug = &u->dbg, r.debug_all = j->debug_all;
    r.seed = j->seed, r.src = &j->src;
    memcpy(r.bmin, u->bmin, 12), memcpy(r.bmax, u->bmax, 12), memcpy(r.params, j->params, sizeof r.params);
    bool ok = pcg_execute(j->g, &r);
    u->nspawns = ok ? r.nspawns : 0, u->ndbg = r.ndebug, u->points = r.points_out, u->next = 0;
    snprintf(u->error, sizeof u->error, "%s", r.error);
    u->hash = pcg_spawns_hash((const PcgSpawn*)u->spawns.base, u->nspawns);
    u->gen_ms = (re_time_seconds() - t0) * 1000.0;
    memcpy(j->pts, r.node_points, sizeof j->pts), memcpy(j->ms, r.node_ms, sizeof j->ms);
}

// The job of a unit: its graph, its share of the component's sources (main thread)
static bool job_prepare(World* w, Job* j, Unit* u) {
    Comp* c = &S.comp[u->comp];
    const CProcWorld* cfg = settings(w, c->e);
    const PcgGraph* g = cfg ? pcg_graph_get(cfg->pcg.graph) : NULL;
    if (!g) return false;
    pcg_graph_prepare(g, 0);
    j->u = u, j->g = g, j->seed = cfg->pcg.seed, j->debug_all = (cfg->pcg.flags & PCGC_SHOW_POINTS) != 0;
    for (uint32_t i = 0; i < g->nparams; i++) {
        j->params[i] = g->params[i].value;
        for (uint32_t k = 0; k < cfg->pcg.noverrides && k < 8; k++)
            if (!_stricmp(cfg->pcg.override_name[k], g->params[i].name)) j->params[i] = cfg->pcg.override_value[k];
    }
    j->src = c->src;
    if (!u->whole) { // (a cell: its meshes only; the splines are few, all of them)
        static PcgMeshSource cell_mesh[MAX_JOBS][4096];
        uint32_t k = (uint32_t)(j - g_jobs), n = 0;
        for (uint32_t i = 0; i < c->src.nmesh && n < 4096; i++) {
            const PcgMeshSource* m = &c->src.mesh[i];
            if (m->mx[0] >= u->bmin[0] - 2000.0f && m->mn[0] <= u->bmax[0] + 2000.0f && m->mx[1] >= u->bmin[1] - 2000.0f && m->mn[1] <= u->bmax[1] + 2000.0f)
                cell_mesh[k][n++] = *m;
        }
        j->src.mesh = cell_mesh[k], j->src.nmesh = n;
    }
    return true;
}

// Up to `max` queued units generated now, nearest first, in parallel (each its own arena)
static uint32_t generate_some(World* w, uint32_t max) {
    if (!S.arenas) {
        for (int k = 0; k < MAX_JOBS; k++) re_arena_init(&S.arena[k], 1ull << 30);
        S.arenas = true;
    }
    if (max > MAX_JOBS) max = MAX_JOBS;
    Unit* pick[MAX_JOBS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_UNITS; i++) {
        Unit* u = &S.units[i];
        if (u->state != U_QUEUED) continue;
        if (n < max) pick[n++] = u;
        else {
            uint32_t worst = 0;
            for (uint32_t k = 1; k < n; k++)
                if (pick[k]->dist2 > pick[worst]->dist2) worst = k;
            if (u->dist2 < pick[worst]->dist2) pick[worst] = u;
        }
    }
    uint32_t nj = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (job_prepare(w, &g_jobs[nj], pick[i])) nj++;
        else pick[i]->state = U_DONE, pick[i]->nspawns = 0;
    }
    if (!nj) return 0;
    double t0 = re_time_seconds();
    if (nj == 1) gen_job(NULL, 0);
    else re_parallel_for((int)nj, gen_job, NULL);
    S.st.gen_ms += (re_time_seconds() - t0) * 1000.0;
    for (uint32_t i = 0; i < nj; i++) {
        Unit* u = g_jobs[i].u;
        u->state = U_SPAWNING;
        S.st.last_gen_ms = u->gen_ms, S.st.last_hash = u->hash, S.st.cells_generated += !u->whole;
        last_store(g_jobs[i].g->hash, g_jobs[i].pts, g_jobs[i].ms);
        if (u->error[0]) printf("[RealEngine] PCG: %s (%s)\n", u->error, pcg_graph_vpath(g_jobs[i].g->hash));
    }
    return nj;
}

// --- Spawning (the main thread) ---
static Entity spawn_one(World* w, Entity owner, const PcgSpawn* s) {
    if (s->kind == PCG_SPAWN_ACTOR) {
        BpAsset* cls = bp_find_hash(s->asset);
        Entity e = cls ? bp_spawn(w, cls, s->loc, s->rot) : ENTITY_NONE;
        CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t) memcpy(t->scale, s->scale, 12), t->flags |= TRANSFORM_PCG;
        return e;
    }
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    memcpy(((CName*)ecs_get(w, e, COMP_NAME))->text, "PCG", 4);
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = at->parent = owner, at->kind = ATTACH_PCG, at->node = s->node;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    sm->mesh = s->asset, sm->material = s->material, sm->uv_scale = 1.0f, sm->roughness = 0.8f;
    memcpy(sm->base_color, s->color, 12);
    sm->cast_shadow = ((s->flags & PCG_SPAWN_SHADOW) ? 1u : 0u) | (s->wind > 0.0f ? CAST_SHADOW_WIND : 0u);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, s->loc, 12), memcpy(t->rotation, s->rot, 12), memcpy(t->scale, s->scale, 12);
    t->flags = MOBILITY_STATIC | TRANSFORM_PCG | ((s->flags & PCG_SPAWN_COLLISION) ? 0u : TRANSFORM_NO_COLLISION);
    if (S.playing && (s->flags & PCG_SPAWN_COLLISION)) bp_box_spawned(e);
    return e;
}

// The units' spawns within budget_ms (< 0: all), nearest first. Playing, also at most PCG_SPAWN_FRAME entities a batch:
// an entity made is ~0.2 us, so 1 ms would make ~4000, and more than the ECS's change ring (4096, shared with every
// system) in one frame makes the renderer send the whole scene again (a 50-60 ms hitch: the pcg_stream test's). The
// batches come every PCG_BATCH_SECONDS (pcg_update): a static caster added or removed redraws the sun's shadow
// levels over it (one a frame, vsm.c), so cells spawned a little every frame kept a level redrawing every frame
#define PCG_SPAWN_FRAME 2048u
static void spawn_some(World* w, double budget_ms) {
    double t0 = re_time_seconds();
    uint32_t made = 0;
    for (;;) {
        Unit* best = NULL;
        for (uint32_t i = 0; i < MAX_UNITS; i++) {
            Unit* u = &S.units[i];
            if (u->state == U_SPAWNING && (!best || u->dist2 < best->dist2)) best = u;
        }
        if (!best) break;
        Entity owner = S.comp[best->comp].e;
        const PcgSpawn* sp = (const PcgSpawn*)best->spawns.base;
        while (best->next < best->nspawns) {
            if (!re_vm_ensure(&best->ents, (size_t)(best->nents + 1) * sizeof(Entity))) break;
            Entity e = spawn_one(w, owner, &sp[best->next++]);
            if (e) ((Entity*)best->ents.base)[best->nents++] = e, S.st.spawned++;
            made++;
            if (budget_ms >= 0.0 && (made >= PCG_SPAWN_FRAME || ((best->next & 31u) == 0 && (re_time_seconds() - t0) * 1000.0 > budget_ms))) {
                S.st.spawn_ms += (re_time_seconds() - t0) * 1000.0;
                return;
            }
        }
        best->state = U_DONE;
        S.comp[best->comp].generated = true;
    }
    S.st.spawn_ms += (re_time_seconds() - t0) * 1000.0;
}

// --- The update ---
static void sync_components(World* w) {
    for (uint32_t i = 0; i < S.ncomp; i++) S.comp[i].seen = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_PROC_WORLD)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CProcWorld* pw = ecs_arch_column(ar, COMP_PROC_WORLD);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t i = 0; i < ar->count; i++) {
            if (pw[i].mode != PROCGEN_PCG) continue;
            Comp* c = comp_of(ents[i]);
            if (!c && S.ncomp < MAX_COMPS) {
                c = &S.comp[S.ncomp++];
                ReVmBuf m = c->src_mesh, s = c->src_spline; // (the reservations stay with the slot)
                memset(c, 0, sizeof *c);
                c->src_mesh = m, c->src_spline = s;
                c->e = ents[i], c->dirty = true;
            }
            if (c) c->seen = true;
        }
    }
    for (uint32_t i = 0; i < S.ncomp; i++) { // (a component removed: what it made goes, the slot is reused)
        if (S.comp[i].seen) continue;
        comp_drop_units(w, (uint16_t)i);
        Comp last = S.comp[S.ncomp - 1];
        for (uint32_t k = 0; k < MAX_UNITS; k++)
            if (S.units[k].state != U_FREE && S.units[k].comp == S.ncomp - 1) S.units[k].comp = (uint16_t)i;
        ReVmBuf m = S.comp[i].src_mesh, s = S.comp[i].src_spline;
        S.comp[i] = last;
        S.comp[S.ncomp - 1].src_mesh = m, S.comp[S.ncomp - 1].src_spline = s; // (swap the reservations, none lost)
        S.ncomp--, i--;
    }
}

static float rect_dist2(const float f[3], const float mn[3], const float mx[3]) {
    float dx = f[0] < mn[0] ? mn[0] - f[0] : f[0] > mx[0] ? f[0] - mx[0] : 0.0f;
    float dy = f[1] < mn[1] ? mn[1] - f[1] : f[1] > mx[1] ? f[1] - mx[1] : 0.0f;
    return dx * dx + dy * dy;
}

static void partition_cells(World* w, uint16_t ci, const float focus[3]) {
    Comp* c = &S.comp[ci];
    float cs = c->cell, range = c->range;
    for (uint32_t i = 0; i < MAX_UNITS; i++) {
        Unit* u = &S.units[i];
        if (u->state == U_FREE || u->comp != ci) continue;
        if (u->whole) { // (it was a whole box: now cells)
            unit_drop(w, u);
            continue;
        }
        float d2 = focus ? rect_dist2(focus, u->bmin, u->bmax) : 0.0f;
        u->dist2 = d2;
        float out = range + cs * 0.25f;
        if (focus && d2 > out * out && u->state != U_DROP) {
            if (S.playing) u->state = U_DROP; // (with the next batch)
            else unit_drop(w, u);
            S.st.cells_dropped++;
        }
    }
    if (!focus) return;
    int32_t x0 = (int32_t)floorf(fmaxf(focus[0] - range, c->bmin[0]) / cs), x1 = (int32_t)floorf(fminf(focus[0] + range, c->bmax[0] - 0.01f) / cs);
    int32_t y0 = (int32_t)floorf(fmaxf(focus[1] - range, c->bmin[1]) / cs), y1 = (int32_t)floorf(fminf(focus[1] + range, c->bmax[1] - 0.01f) / cs);
    for (int32_t cy = y0; cy <= y1; cy++)
        for (int32_t cx = x0; cx <= x1; cx++) {
            float mn[3] = { fmaxf((float)cx * cs, c->bmin[0]), fmaxf((float)cy * cs, c->bmin[1]), c->bmin[2] };
            float mx[3] = { fminf((float)(cx + 1) * cs, c->bmax[0]), fminf((float)(cy + 1) * cs, c->bmax[1]), c->bmax[2] };
            if (mn[0] >= mx[0] || mn[1] >= mx[1]) continue;
            float d2 = rect_dist2(focus, mn, mx);
            if (d2 > range * range) continue;
            bool have = false;
            for (uint32_t i = 0; i < MAX_UNITS && !have; i++) {
                Unit* o = &S.units[i];
                if (o->state == U_FREE || o->comp != ci || o->whole || o->cx != cx || o->cy != cy) continue;
                if (o->state == U_DROP) o->state = o->next < o->nspawns ? U_SPAWNING : U_DONE; // (back before it went)
                have = true;
            }
            if (have) continue;
            Unit* u = unit_new(ci, false, cx, cy);
            if (!u) return;
            memcpy(u->bmin, mn, 12), memcpy(u->bmax, mx, 12), u->dist2 = d2;
        }
}

void pcg_update(World* w, const float focus[3], bool playing) {
    double t0 = re_time_seconds();
    static int off = -1;
    if (off < 0) off = getenv("REALENGINE_PCG_OFF") != NULL; // (the measures' "before": nothing generated)
    if (off) return;
    S.st.gen_ms = S.st.spawn_ms = 0.0;
    S.playing = playing;
    if (S.w != w) { // (another world: nothing of ours is in it)
        for (uint32_t i = 0; i < MAX_UNITS; i++) S.units[i].state = U_FREE, S.units[i].nents = 0;
        S.ncomp = 0, S.w = w, S.revision = w->revision, S.purge_all = false;
    }
    if (w->revision != S.revision) { // an undo, a level loaded, a snapshot restored: made again from scratch
        purge_world(w);
        S.revision = w->revision;
    }
    sync_components(w);
    bool changes = w->changes != S.last_changes;
    S.last_changes = w->changes;
    const WpStats* wps = wp_stats();
    Entity wpa = wp_settings(w);
    const CWorldPartition* wpc = wpa ? ecs_get(w, wpa, COMP_WORLD_PARTITION) : NULL;
    (void)wps;
    double budget = wpc && wpc->budget_ms > 0.0f ? wpc->budget_ms : 1.0;
    S.st.components = S.ncomp, S.st.units = S.st.units_in = S.st.queued = S.st.points = 0;
    for (uint16_t ci = 0; ci < S.ncomp; ci++) {
        Comp* c = &S.comp[ci];
        CProcWorld* cfg = settings(w, c->e);
        if (!cfg) continue;
        if (cfg->pcg.spawn_budget > 0.0f) budget = cfg->pcg.spawn_budget;
        pcg_component_bounds(w, c->e, c->bmin, c->bmax);
        c->partitioned = (cfg->pcg.flags & PCGC_PARTITIONED) != 0;
        c->cell = cfg->pcg.cell_size > 100.0f ? cfg->pcg.cell_size : wpc && wpc->cell_size > 100.0f ? wpc->cell_size : 12800.0f;
        c->range = cfg->pcg.load_range > 0.0f ? cfg->pcg.load_range : wpc && wpc->loading_range > 0.0f ? wpc->loading_range : 25600.0f;
        bool cleaned = (cfg->pcg.flags & PCGC_CLEANED) != 0;
        if (cleaned) {
            comp_drop_units(w, ci), c->generated = false;
            continue;
        }
        if (playing && cfg->pcg.trigger == PCG_TRIGGER_ON_DEMAND && !c->generated && !c->dirty) continue;
        // the editor: regenerate when what it reads changed; the game: when told (Generate, Set Seed, Set Parameter)
        bool regen = c->dirty;
        if (!playing && (cfg->pcg.flags & PCGC_ON_CHANGE) && (changes || !c->hash)) {
            uint32_t h = settings_hash(w, c->e, cfg) ^ gather(w, c, cfg) * 31u;
            if (h != c->hash) regen = true, c->hash = h;
        } else if (!c->hash || regen) c->hash = settings_hash(w, c->e, cfg) ^ gather(w, c, cfg) * 31u;
        if (regen) {
            comp_drop_units(w, ci);
            c->dirty = false;
            if (!c->partitioned) {
                Unit* u = unit_new(ci, true, 0, 0);
                if (u) memcpy(u->bmin, c->bmin, 12), memcpy(u->bmax, c->bmax, 12), u->dist2 = 0.0f;
            }
        }
        if (c->partitioned) partition_cells(w, ci, focus);
        else { // (a whole box made once: queued only by a change)
            bool any = false;
            for (uint32_t i = 0; i < MAX_UNITS && !any; i++) any = S.units[i].state != U_FREE && S.units[i].comp == ci;
            if (!any && !c->generated) {
                Unit* u = unit_new(ci, true, 0, 0);
                if (u) memcpy(u->bmin, c->bmin, 12), memcpy(u->bmax, c->bmax, 12), u->dist2 = 0.0f;
            }
        }
    }
    // generate: the editor all of what is queued (its whole boxes at once); playing within the budget, a few cells a
    // frame on the workers
    uint32_t threads = (uint32_t)re_pool_threads();
    if (!playing) {
        for (int guard = 0; guard < 256 && generate_some(w, threads); guard++) {
            bool any_whole = false;
            for (uint32_t i = 0; i < MAX_UNITS; i++) any_whole |= S.units[i].state == U_QUEUED && S.units[i].whole;
            if (!any_whole) break; // (the editor's cells: a few a frame too, the camera moving)
        }
    } else
        generate_some(w, threads < 4 ? threads : 4);
    static double last_batch;
    double now = re_time_seconds();
    if (!playing) {
        for (uint32_t i = 0; i < MAX_UNITS; i++)
            if (S.units[i].state == U_DROP) unit_drop(w, &S.units[i]);
        spawn_some(w, -1.0);
    } else if (now - last_batch >= PCG_BATCH_SECONDS) { // a batch: the far cells' entities gone, the new ones made
        bool any = false;
        for (uint32_t i = 0; i < MAX_UNITS; i++) {
            if (S.units[i].state == U_DROP) unit_drop(w, &S.units[i]), any = true;
            any |= S.units[i].state == U_SPAWNING;
        }
        if (any) spawn_some(w, budget), last_batch = now;
    }
    for (uint32_t i = 0; i < MAX_UNITS; i++) {
        const Unit* u = &S.units[i];
        if (u->state == U_FREE) continue;
        S.st.units++, S.st.units_in += u->state == U_DONE, S.st.queued += u->state == U_QUEUED || u->state == U_SPAWNING;
        S.st.points += u->points;
    }
    S.st.ms = (re_time_seconds() - t0) * 1000.0;
    if (S.st.ms > S.st.ms_peak) S.st.ms_peak = S.st.ms;
}

void pcg_begin_play(World* w) {
    S.playing = true;
    if (S.w != w) return;
    for (uint32_t i = 0; i < S.ncomp; i++) {
        const CProcWorld* cfg = settings(w, S.comp[i].e);
        if (!cfg || (cfg->pcg.flags & PCGC_CLEANED)) continue;
        // (what the editor made stays: the same graph and seed make the same; a partitioned one streams from here. On
        // Demand: nothing until a Blueprint's Generate, as in the packaged game)
        if (cfg->pcg.trigger == PCG_TRIGGER_ON_DEMAND) comp_drop_units(w, (uint16_t)i), S.comp[i].generated = false, S.comp[i].dirty = false;
    }
    S.st.ms_peak = 0.0, S.st.cells_generated = S.st.cells_dropped = 0, S.st.spawned = 0;
}

void pcg_end_play(World* w) {
    (void)w;
    // (the editor's world comes back from its snapshot: our units describe the play's; made again from scratch)
    for (uint32_t i = 0; i < MAX_UNITS; i++) S.units[i].state = U_FREE, S.units[i].nents = 0;
    for (uint32_t i = 0; i < S.ncomp; i++) S.comp[i].generated = false, S.comp[i].hash = 0, S.comp[i].dirty = false;
    S.revision = ~0u; // (purged and made again at the next update)
    S.playing = false;
}

// --- The Blueprints' ---
static Comp* comp_for(World* w, Entity e) {
    if (!pcg_is_component(w, e)) return NULL;
    if (S.w != w) return NULL;
    Comp* c = comp_of(e);
    if (!c) sync_components(w), c = comp_of(e);
    return c;
}
bool pcg_generate(World* w, Entity e) {
    CProcWorld* cfg = settings(w, e);
    if (!cfg) return false;
    cfg->pcg.flags &= ~PCGC_CLEANED;
    Comp* c = comp_for(w, e);
    if (c) c->dirty = true;
    else if (S.w != w) S.w = NULL; // (a world not seen yet: the next update takes it from the start)
    return true;
}
bool pcg_cleanup(World* w, Entity e) {
    CProcWorld* cfg = settings(w, e);
    if (!cfg) return false;
    cfg->pcg.flags |= PCGC_CLEANED;
    Comp* c = comp_for(w, e);
    if (c) comp_drop_units(w, (uint16_t)(c - S.comp)), c->generated = false;
    return true;
}
bool pcg_set_seed(World* w, Entity e, uint32_t seed) {
    CProcWorld* cfg = settings(w, e);
    if (!cfg) return false;
    cfg->pcg.seed = seed;
    Comp* c = comp_for(w, e);
    if (c && (c->generated || !S.playing)) c->dirty = true;
    return true;
}
bool pcg_set_parameter(World* w, Entity e, const char* name, float v) {
    CProcWorld* cfg = settings(w, e);
    if (!cfg || !name || !name[0]) return false;
    uint32_t k = 0;
    while (k < cfg->pcg.noverrides && k < 8 && _stricmp(cfg->pcg.override_name[k], name)) k++;
    if (k >= 8) return false;
    if (k == cfg->pcg.noverrides) cfg->pcg.noverrides++, snprintf(cfg->pcg.override_name[k], sizeof cfg->pcg.override_name[k], "%s", name);
    cfg->pcg.override_value[k] = v;
    Comp* c = comp_for(w, e);
    if (c && (c->generated || !S.playing)) c->dirty = true;
    return true;
}
bool pcg_is_generated(World* w, Entity e) {
    Comp* c = comp_for(w, e);
    return c && c->generated;
}
uint32_t pcg_generated_count(World* w, Entity e) {
    Comp* c = comp_for(w, e);
    uint32_t n = 0;
    for (uint32_t i = 0; c && i < MAX_UNITS; i++)
        if (S.units[i].state != U_FREE && S.units[i].comp == (uint16_t)(c - S.comp)) n += S.units[i].nents;
    return n;
}

// What a component spawned, whatever the order its units were made in (a sum of their hashes)
uint64_t pcg_component_hash(World* w, Entity e) {
    Comp* c = comp_for(w, e);
    uint64_t h = 0;
    for (uint32_t i = 0; c && i < MAX_UNITS; i++)
        if (S.units[i].state != U_FREE && S.units[i].comp == (uint16_t)(c - S.comp)) h += S.units[i].hash * 0x9E3779B97F4A7C15ull + S.units[i].nspawns;
    return h;
}
const PcgStats* pcg_stats(void) { return &S.st; }
void pcg_reset_peak(void) { S.st.ms_peak = 0.0; }

const PcgDebugPoint* pcg_debug_next(uint32_t* cursor, uint32_t* n) {
    for (uint32_t i = *cursor; i < MAX_UNITS; i++) {
        const Unit* u = &S.units[i];
        if (u->state == U_FREE || !u->ndbg) continue;
        *n = u->ndbg, *cursor = i + 1;
        return (const PcgDebugPoint*)u->dbg.base;
    }
    *n = 0, *cursor = MAX_UNITS;
    return NULL;
}

// The last generation of each graph: its nodes' points and times (the editor's cards)
typedef struct LastRun { uint32_t graph; uint32_t pts[PCG_MAX_NODES]; float ms[PCG_MAX_NODES]; } LastRun;
static LastRun g_last[8];
static uint32_t g_last_next;
static void last_store(uint32_t graph, const uint32_t* pts, const float* ms) {
    LastRun* l = NULL;
    for (int i = 0; i < 8 && !l; i++)
        if (g_last[i].graph == graph) l = &g_last[i];
    if (!l) l = &g_last[g_last_next++ % 8];
    l->graph = graph;
    memcpy(l->pts, pts, sizeof l->pts), memcpy(l->ms, ms, sizeof l->ms);
}
bool pcg_last_nodes(uint32_t graph_hash, const uint32_t** pts, const float** ms) {
    for (int i = 0; i < 8; i++)
        if (g_last[i].graph == graph_hash && graph_hash) {
            *pts = g_last[i].pts, *ms = g_last[i].ms;
            return true;
        }
    return false;
}

// --- The Debug Viewer ---
static void collect(DvList* l, const DvContext* ctx) {
    (void)ctx;
    l->gpu = false, l->estimated = false, l->ms = S.st.ms;
    snprintf(l->note, sizeof l->note, "%u components, %u units (%u done, %u waiting); %u points; generation %.2f ms, spawning %.2f ms this frame; worst update %.2f ms",
             S.st.components, S.st.units, S.st.units_in, S.st.queued, S.st.points, S.st.gen_ms, S.st.spawn_ms, S.st.ms_peak);
    for (uint32_t i = 0; i < MAX_UNITS; i++) {
        const Unit* u = &S.units[i];
        if (u->state == U_FREE) continue;
        char name[64], info[48];
        if (u->whole) snprintf(name, sizeof name, "PCG volume %u", u->comp);
        else snprintf(name, sizeof name, "PCG cell %d, %d", u->cx, u->cy);
        snprintf(info, sizeof info, "%u points, %u entities", u->points, u->nents);
        dv_add(l, name, info, (float)u->gen_ms, u->comp < S.ncomp ? S.comp[u->comp].e : ENTITY_NONE);
    }
}
void pcg_register_debug(void) { dv_register("PCG", collect); }
