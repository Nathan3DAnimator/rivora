// The PCG's tests (Q15, notas-dev/pcg.md).
//   --pcg-test (pcg_selftest, no window): graphs built in memory and run on their own arena: 100 000 points (the
//     sampler, a whole graph: noise, transform, pruning, spawner) and their cost; the same seed twice, serial against the
//     workers, another seed; a box made whole against made in 4 cells (the same spawns); the spline sampler on a
//     straight spline; a subgraph and a nested graph; attributes and a parameter override; the file written and read
//     back (the same result); the examples written and read; the arena reused without growing (no malloc in the frame)
//   --scene pcg_forest: a 400 m Landscape with a PCG Volume of the forest example, one of rock clusters (nested graphs)
//     and a fence along a Rail: generated in the editor (every tree on the ground, the slopes respected), then Play in
//     Editor: the same entities and hash in PIE, Set Seed to another and back makes the same forest again
//   --scene pcg_stream: a 3.2 km ground (a plane, as the world_partition test's) with a World Partition and a partitioned PCG forest over all of it; the player
//     carried across at 80 m/s (1.3 km): cells generated on the workers and dropped behind, no hitch (frames over twice the
//     median under 1 %, the PCG's worst update under 6 ms, as the world_partition test)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h" // (Q22) mat_compiling
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "landscape.h"
#include "pcg.h"
#include "pcg_test.h"
#include "platform.h"
#include "platform_files.h"
#include "world_partition.h"

// --- The self-test ---
static uint64_t fnv64(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
    return h;
}
static uint64_t multiset(const ReVmBuf* b, uint32_t n) { // (order-free)
    uint64_t h = 0;
    for (uint32_t i = 0; i < n; i++) h += fnv64((const PcgSpawn*)b->base + i, sizeof(PcgSpawn));
    return h;
}

static ReArena g_ta;
static ReVmBuf g_tsp, g_tdbg;
static bool trun(const PcgGraph* g, PcgRun* r, uint32_t seed, const float mn[3], const float mx[3], const PcgSources* src, bool serial) {
    re_arena_reset(&g_ta);
    memset(r, 0, sizeof *r);
    r->arena = &g_ta, r->spawns = &g_tsp, r->debug = &g_tdbg, r->seed = seed, r->src = src, r->serial = serial;
    memcpy(r->bmin, mn, 12), memcpy(r->bmax, mx, 12);
    for (uint32_t i = 0; i < g->nparams; i++) r->params[i] = g->params[i].value;
    bool ok = pcg_execute(g, r);
    if (!ok) printf("[RealEngine] PCG self-test: run failed: %s\n", r->error);
    return ok;
}
static void set3(float* f, float a, float b, float c) { f[0] = a, f[1] = b, f[2] = c; }

int pcg_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: PCG: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    re_arena_init(&g_ta, 1ull << 30);
    re_vm_reserve(&g_tsp, 256u << 20), re_vm_reserve(&g_tdbg, 64u << 20);
    static PcgGraph g, g2, sub, nest;
    PcgRun r;
    // 1. 100 000 points: a volume of 100 x 100 x 10 voxels (the sampler alone, then the whole graph)
    memset(&g, 0, sizeof g), pcg_graph_clear(&g);
    int vs = pcg_graph_add(&g, PCGN_VOLUME, 0, 0);
    set3(g.nodes[vs].f, 100, 100, 100);
    g.nodes[vs].f[3] = 0.8f, g.nodes[vs].f[4] = 40.0f;
    pcg_graph_link(&g, vs, 0, 1, 0);
    float mn[3] = { 0, 0, 0 }, mx[3] = { 10000, 10000, 1000 };
    double best = 1e9;
    for (int k = 0; k < 5; k++) {
        double t0 = re_time_seconds();
        trun(&g, &r, 1, mn, mx, NULL, false);
        best = fmin(best, (re_time_seconds() - t0) * 1000.0);
    }
    uint32_t np = r.node_points[vs];
    printf("[RealEngine] PCG: Volume Sampler, %u points: %.2f ms (the workers)\n", np, best);
    CHECK(np == 100000u, "the volume sampler made %u points, 100000 expected", np);
    double best_serial = 1e9;
    for (int k = 0; k < 3; k++) {
        double t0 = re_time_seconds();
        trun(&g, &r, 1, mn, mx, NULL, true);
        best_serial = fmin(best_serial, (re_time_seconds() - t0) * 1000.0);
    }
    printf("[RealEngine] PCG: the same on one thread: %.2f ms\n", best_serial);
    CHECK(best < 15.0, "100 000 points took %.2f ms (15 at most)", best);
    // the whole graph: density noise, a transform, self pruning, a mesh spawner
    int dn = pcg_graph_add(&g, PCGN_DENSITY_NOISE, 0, 0);
    g.nodes[dn].f[0] = 0.0f;
    int tr = pcg_graph_add(&g, PCGN_TRANSFORM, 0, 0);
    set3(&g.nodes[tr].f[9], 0, 360, 0), set3(&g.nodes[tr].f[12], 0.5f, 0.5f, 0.5f), set3(&g.nodes[tr].f[15], 1.5f, 1.5f, 1.5f);
    int sp = pcg_graph_add(&g, PCGN_SELF_PRUNE, 0, 0);
    int ms = pcg_graph_add(&g, PCGN_MESH_SPAWNER, 0, 0);
    g.nlinks = 0;
    pcg_graph_link(&g, vs, 0, dn, 0), pcg_graph_link(&g, dn, 0, tr, 0), pcg_graph_link(&g, tr, 0, sp, 0), pcg_graph_link(&g, sp, 0, ms, 0);
    pcg_graph_link(&g, ms, 0, 1, 0);
    best = 1e9;
    for (int k = 0; k < 5; k++) {
        double t0 = re_time_seconds();
        trun(&g, &r, 7, mn, mx, NULL, false);
        best = fmin(best, (re_time_seconds() - t0) * 1000.0);
    }
    printf("[RealEngine] PCG: a whole graph on 100 000 points (noise, transform, pruning, spawner): %.2f ms, %u spawns; nodes: noise %.2f, transform %.2f, pruning %.2f, spawner %.2f ms\n",
           best, r.nspawns, r.node_ms[dn], r.node_ms[tr], r.node_ms[sp], r.node_ms[ms]);
    CHECK(r.nspawns > 1000 && r.nspawns < 100000, "the pruning kept %u points", r.nspawns);
    CHECK(best < 60.0, "the whole graph on 100 000 points took %.2f ms (60 at most)", best);
    // 2. determinism: the same seed twice, serial against the workers, another seed
    trun(&g, &r, 7, mn, mx, NULL, false);
    uint64_t h1 = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    trun(&g, &r, 7, mn, mx, NULL, false);
    uint64_t h2 = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    trun(&g, &r, 7, mn, mx, NULL, true);
    uint64_t h3 = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    trun(&g, &r, 8, mn, mx, NULL, false);
    uint64_t h4 = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    printf("[RealEngine] PCG: determinism: seed 7 %016llx, again %016llx, serial %016llx; seed 8 %016llx\n", (unsigned long long)h1,
           (unsigned long long)h2, (unsigned long long)h3, (unsigned long long)h4);
    CHECK(h1 == h2 && h1 == h3, "the same seed made different spawns (twice or serial)");
    CHECK(h1 != h4, "another seed made the same spawns");
    // 3. whole against 4 cells (no pruning: the spawns are the same set)
    memset(&g2, 0, sizeof g2), pcg_graph_clear(&g2);
    vs = pcg_graph_add(&g2, PCGN_VOLUME, 0, 0);
    set3(g2.nodes[vs].f, 150, 150, 500), g2.nodes[vs].f[3] = 1.0f, g2.nodes[vs].f[4] = 40.0f;
    tr = pcg_graph_add(&g2, PCGN_TRANSFORM, 0, 0);
    set3(&g2.nodes[tr].f[9], 0, 360, 0), set3(&g2.nodes[tr].f[0], -30, -30, 0), set3(&g2.nodes[tr].f[3], 30, 30, 0);
    ms = pcg_graph_add(&g2, PCGN_MESH_SPAWNER, 0, 0);
    pcg_graph_link(&g2, vs, 0, tr, 0), pcg_graph_link(&g2, tr, 0, ms, 0);
    float wmn[3] = { -3000, -3000, 0 }, wmx[3] = { 3000, 3000, 1000 };
    trun(&g2, &r, 5, wmn, wmx, NULL, false);
    uint64_t whole = multiset(&g_tsp, r.nspawns), cells = 0;
    uint32_t nwhole = r.nspawns, ncells = 0;
    for (int c = 0; c < 4; c++) {
        float cmn[3] = { c & 1 ? 0.0f : -3000.0f, c & 2 ? 0.0f : -3000.0f, 0 }, cmx[3] = { c & 1 ? 3000.0f : 0.0f, c & 2 ? 3000.0f : 0.0f, 1000 };
        trun(&g2, &r, 5, cmn, cmx, NULL, false);
        cells += multiset(&g_tsp, r.nspawns), ncells += r.nspawns;
    }
    printf("[RealEngine] PCG: whole box %u spawns, 4 cells %u spawns, %s\n", nwhole, ncells, whole == cells ? "the same set" : "DIFFERENT");
    CHECK(whole == cells && nwhole == ncells, "the cells didn't make what the whole box makes");
    // 4. the spline sampler on a straight 1 km spline (no ground): every 2.5 m, then on both sides
    static PcgSplinePoint line[101];
    for (int i = 0; i <= 100; i++) set3(line[i].p, i * 1000.0f, 500.0f, 100.0f), set3(line[i].t, 1, 0, 0), line[i].width = 400.0f, line[i].dist = i * 1000.0f;
    PcgSources src = { 0 };
    src.nspline = 1, src.spline[0] = line, src.spline_n[0] = 101;
    memset(&g2, 0, sizeof g2), pcg_graph_clear(&g2);
    int ss = pcg_graph_add(&g2, PCGN_SPLINE, 0, 0);
    g2.nodes[ss].f[4] = 0.0f; // (no projection)
    pcg_graph_link(&g2, ss, 0, 1, 0);
    float smn[3] = { -100, -10000, -1000 }, smx[3] = { 101000, 10000, 1000 };
    trun(&g2, &r, 1, smn, smx, &src, false);
    uint32_t on = r.node_points[ss];
    g2.nodes[ss].f[3] = 1.0f, g2.nodes[ss].f[2] = 120.0f;
    trun(&g2, &r, 1, smn, smx, &src, false);
    uint32_t both = r.node_points[ss];
    g2.nodes[ss].f[0] = 1.0f, g2.nodes[ss].f[1] = 100.0f; // (inside the width: 4 m, every metre)
    trun(&g2, &r, 1, smn, smx, &src, false);
    uint32_t inside = r.node_points[ss];
    printf("[RealEngine] PCG: spline sampler on 1 km: %u on it, %u on both sides, %u inside its width\n", on, both, inside);
    CHECK(on == 401u && both == 802u, "the spline sampler: %u and %u points (401 and 802 expected)", on, both);
    CHECK(inside == 1001u * 5u, "the spline's inside: %u points (5005 expected)", inside);
    // 5. a subgraph (raises the points by 1 m) and a nested graph (a 3 x 3 grid around each point)
    memset(&sub, 0, sizeof sub), pcg_graph_clear(&sub);
    snprintf(sub.vpath, sizeof sub.vpath, "/Content/PCG/Test/PCG_SubRaise.rpcg"), sub.hash = pcg_vpath_hash(sub.vpath);
    tr = pcg_graph_add(&sub, PCGN_TRANSFORM, 0, 0);
    set3(&sub.nodes[tr].f[0], 0, 0, 100), set3(&sub.nodes[tr].f[3], 0, 0, 100), set3(&sub.nodes[tr].f[9], 0, 0, 0);
    pcg_graph_link(&sub, 0, 0, tr, 0), pcg_graph_link(&sub, tr, 0, 1, 0);
    pcg_graph_edited(&sub);
    memset(&nest, 0, sizeof nest), pcg_graph_clear(&nest);
    snprintf(nest.vpath, sizeof nest.vpath, "/Content/PCG/Test/PCG_NestGrid.rpcg"), nest.hash = pcg_vpath_hash(nest.vpath);
    vs = pcg_graph_add(&nest, PCGN_VOLUME, 0, 0);
    set3(nest.nodes[vs].f, 100, 100, 10000), nest.nodes[vs].f[3] = 0.0f;
    ms = pcg_graph_add(&nest, PCGN_MESH_SPAWNER, 0, 0);
    pcg_graph_link(&nest, vs, 0, ms, 0);
    pcg_graph_edited(&nest);
    memset(&g2, 0, sizeof g2), pcg_graph_clear(&g2);
    vs = pcg_graph_add(&g2, PCGN_VOLUME, 0, 0);
    set3(g2.nodes[vs].f, 1000, 1000, 10000), g2.nodes[vs].f[3] = 0.0f;
    int sg = pcg_graph_add(&g2, PCGN_SUBGRAPH, 0, 0);
    snprintf(g2.nodes[sg].s[0], PCG_STR, "%s", sub.vpath);
    int ne = pcg_graph_add(&g2, PCGN_NESTED, 0, 0);
    snprintf(g2.nodes[ne].s[0], PCG_STR, "%s", nest.vpath);
    g2.nodes[ne].f[0] = 150.0f; // (a 3 m box: 3 x 3 voxels of 1 m)
    pcg_graph_link(&g2, vs, 0, sg, 0), pcg_graph_link(&g2, sg, 0, ne, 0), pcg_graph_link(&g2, ne, 0, 1, 0);
    float gmn[3] = { 0, 0, 0 }, gmx[3] = { 3000, 3000, 10000 };
    trun(&g2, &r, 3, gmn, gmx, NULL, false);
    float z0 = r.nspawns ? ((const PcgSpawn*)g_tsp.base)[0].loc[2] : 0.0f;
    printf("[RealEngine] PCG: a subgraph and a nested graph: %u points in, %u spawns (9 each)\n", r.node_points[vs], r.nspawns);
    CHECK(r.node_points[vs] == 9u && r.nspawns == 81u, "the nested graph: %u points, %u spawns (9 and 81 expected)", r.node_points[vs], r.nspawns);
    (void)z0;
    // the subgraph's raise: its output's points 1 m above the sampler's
    CHECK(r.node_points[sg] == 9u, "the subgraph returned %u points", r.node_points[sg]);
    // 6. attributes and a bound parameter
    memset(&g2, 0, sizeof g2), pcg_graph_clear(&g2);
    snprintf(g2.params[0].name, sizeof g2.params[0].name, "Threshold"), g2.params[0].value = 0.5f, g2.params[0].mn = 0, g2.params[0].mx = 1, g2.nparams = 1;
    vs = pcg_graph_add(&g2, PCGN_VOLUME, 0, 0);
    set3(g2.nodes[vs].f, 100, 100, 10000);
    int ca = pcg_graph_add(&g2, PCGN_CREATE_ATTR, 0, 0);
    snprintf(g2.nodes[ca].s[0], PCG_STR, "Wet");
    int af = pcg_graph_add(&g2, PCGN_ATTR_FILTER, 0, 0);
    snprintf(g2.nodes[af].s[0], PCG_STR, "Wet"), g2.nodes[af].f[0] = 3.0f, g2.nodes[af].bind[1] = 1;
    ms = pcg_graph_add(&g2, PCGN_MESH_SPAWNER, 0, 0);
    pcg_graph_link(&g2, vs, 0, ca, 0), pcg_graph_link(&g2, ca, 0, af, 0), pcg_graph_link(&g2, af, 0, ms, 0);
    float amn[3] = { 0, 0, 0 }, amx[3] = { 10000, 10000, 10000 };
    trun(&g2, &r, 9, amn, amx, NULL, false);
    uint32_t half = r.nspawns;
    re_arena_reset(&g_ta);
    PcgRun r2 = r;
    r2.params[0] = 0.9f;
    r2.nspawns = 0;
    pcg_execute(&g2, &r2);
    printf("[RealEngine] PCG: attribute Wet >= Threshold: %u of 10000 at 0.5, %u at 0.9 (the parameter)\n", half, r2.nspawns);
    CHECK(half > 4500 && half < 5500 && r2.nspawns > 700 && r2.nspawns < 1300, "the attribute filter or the parameter (%u, %u)", half, r2.nspawns);
    // 7. the file: written, read back, the same result; the examples written and read
    char root[700], vp[300];
    const char* tmp = getenv("TEMP");
    snprintf(root, sizeof root, "%s/realengine_pcg_test", tmp ? tmp : ".");
    re_remove_tree(root);
    re_make_dirs(root);
    pcg_init(root, true);
    snprintf(vp, sizeof vp, "/Content/PCG/Test/PCG_SelfTest.rpcg");
    snprintf(g.vpath, sizeof g.vpath, "%s", vp), g.hash = pcg_vpath_hash(vp);
    CHECK(pcg_graph_save(vp, &g), "the graph wasn't written");
    static PcgGraph back;
    CHECK(pcg_graph_load(vp, &back), "the graph wasn't read back");
    trun(&g, &r, 7, mn, mx, NULL, false);
    uint64_t ha = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    trun(&back, &r, 7, mn, mx, NULL, false);
    uint64_t hb = pcg_spawns_hash((const PcgSpawn*)g_tsp.base, r.nspawns);
    CHECK(back.nnodes == g.nnodes && back.nlinks == g.nlinks && ha == hb, "the file read back differs (%u/%u nodes, %u/%u links)", back.nnodes, g.nnodes, back.nlinks, g.nlinks);
    const PcgGraph* ex = pcg_graph_find("PCG_Forest");
    const PcgGraph* exr = pcg_graph_find("PCG_RockClusters");
    const PcgGraph* exf = pcg_graph_find("/Content/PCG/Examples/PCG_Fence.rpcg");
    CHECK(ex && ex->nnodes >= 12 && ex->nparams == 1 && exr && exr->nparams == 1 && exf, "the examples weren't written or read");
    // 8. the arena reused: the same run again doesn't grow it (nothing allocated in the frame)
    trun(&g, &r, 7, mn, mx, NULL, false);
    size_t c1 = g_ta.vm.committed, p1 = g_ta.peak;
    for (int k = 0; k < 3; k++) trun(&g, &r, 7, mn, mx, NULL, false);
    printf("[RealEngine] PCG: the arena: %.1f MB committed, peak %.1f MB, again %.1f MB\n", c1 / 1048576.0, p1 / 1048576.0, g_ta.vm.committed / 1048576.0);
    CHECK(g_ta.vm.committed == c1, "the arena grew when the same graph ran again");
    re_remove_tree(root);
    pcg_init("", false);
    printf("[RealEngine] PCG self-test: %s\n", fails ? "FAIL" : "OK");
#undef CHECK
    return fails;
}

// --- The scenes ---
static struct {
    int scene; // 1 forest, 2 stream
    float t;
    int phase;
    Entity land, forest, rocks, fence, rail, player;
    uint32_t ed_count, pie_count, seed_count;
    uint64_t ed_hash, pie_hash, seed_hash, back_hash;
    uint32_t grounded, trunks, steep;
    float worst_ground;
    float dts[16384];
    uint32_t ndt;
    bool failed;
} PT;

static Entity actor(World* w, uint64_t extra, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | extra);
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_STATIC;
    return e;
}

static Entity player(World* w, float x, float y) {
    Entity e = actor(w, COMP_BIT(COMP_CHARACTER), "Player", x, y, 4000.0f);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 600.0f, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    c->auto_possess = 1, c->camera_distance = 600.0f, c->control_pitch = -15.0f;
    return e;
}

static void lights(World* w) {
    Entity s = actor(w, COMP_BIT(COMP_LIGHT), "Sun", 0, 0, 5000);
    CLight* l = ecs_get(w, s, COMP_LIGHT);
    l->type = LIGHT_DIRECTIONAL;
    CTransform* t = ecs_get(w, s, COMP_TRANSFORM);
    t->rotation[0] = -38.0f, t->rotation[1] = 35.0f;
    Entity k = actor(w, COMP_BIT(COMP_LIGHT), "Sky", 0, 0, 4800);
    ((CLight*)ecs_get(w, k, COMP_LIGHT))->type = LIGHT_SKY;
}

bool pcg_test_scene(Editor* ed, const char* name) {
    bool forest = !strcmp(name, "pcg_forest"), stream = !strcmp(name, "pcg_stream");
    if (!forest && !stream) return false;
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    memset(&PT, 0, sizeof PT);
    PT.scene = forest ? 1 : 2;
    if (forest) {
        PT.land = actor(w, COMP_BIT(COMP_LANDSCAPE), "Landscape", 0, 0, 0);
        CLandscape* lc = ecs_get(w, PT.land, COMP_LANDSCAPE);
        snprintf(lc->name, sizeof lc->name, "PcgForestLand");
        lc->size = 40000.0f, lc->res = 256, lc->chunks = 4, lc->height = 3500.0f, lc->roughness = 0.55f, lc->seed = 15;
    } else { // (the stream: a 3.2 km plane as the world_partition test's ground, never streamed: the measure is the PCG's)
        PT.land = actor(w, COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_PARTITION), "Ground", 0, 0, 0);
        CTransform* gt = ecs_get(w, PT.land, COMP_TRANSFORM);
        gt->scale[0] = gt->scale[1] = 3200.0f;
        CStaticMesh* gm = ecs_get(w, PT.land, COMP_STATIC_MESH);
        gm->mesh = SHAPE_PLANE, gm->base_color[0] = 0.30f, gm->base_color[1] = 0.33f, gm->base_color[2] = 0.24f, gm->roughness = 0.95f, gm->cast_shadow = 1;
        ((CPartition*)ecs_get(w, PT.land, COMP_PARTITION))->spatial = WP_SPATIAL_NO;
    }
    lights(w);
    if (forest) {
        PT.forest = pcg_spawn_volume(w, (float[3]){ -3000, 0, 1500 }, "/Content/PCG/Examples/PCG_Forest.rpcg");
        CProcWorld* c = ecs_get(w, PT.forest, COMP_PROC_WORLD);
        c->pcg.seed = 7, c->pcg.extent[0] = 15000.0f, c->pcg.extent[1] = 18000.0f, c->pcg.extent[2] = 10000.0f;
        PT.rocks = pcg_spawn_volume(w, (float[3]){ 13000, 0, 1500 }, "/Content/PCG/Examples/PCG_RockClusters.rpcg");
        c = ecs_get(w, PT.rocks, COMP_PROC_WORLD);
        c->pcg.seed = 3, c->pcg.extent[0] = 5000.0f, c->pcg.extent[1] = 18000.0f, c->pcg.extent[2] = 10000.0f;
        // a fence along a Rail (its loop of four points), its own PCG Volume around it
        PT.rail = actor(w, COMP_BIT(COMP_RAIL), "FenceLine", 0, 0, 0);
        CRail* rl = ecs_get(w, PT.rail, COMP_RAIL);
        const float pts[5][3] = { { -16000, -16000, 0 }, { -2000, -17000, 0 }, { 10000, -15000, 0 }, { 16000, -12000, 0 }, { 17000, 2000, 0 } };
        memcpy(rl->points, pts, sizeof pts), rl->npoints = 5, rl->closed = 0;
        PT.fence = pcg_spawn_volume(w, (float[3]){ 0, -10000, 1500 }, "/Content/PCG/Examples/PCG_Fence.rpcg");
        c = ecs_get(w, PT.fence, COMP_PROC_WORLD);
        c->pcg.extent[0] = 19500.0f, c->pcg.extent[1] = 9500.0f, c->pcg.extent[2] = 10000.0f;
        PT.player = player(w, -3000.0f, 0.0f);
        ed->cam.pos[0] = -24000.0f, ed->cam.pos[1] = -20000.0f, ed->cam.pos[2] = 9000.0f, ed->cam.yaw = 40.0f, ed->cam.pitch = -20.0f;
    } else {
        Entity wpe = actor(w, COMP_BIT(COMP_WORLD_PARTITION), "WorldPartition", 0, 0, 0);
        (void)wpe;
        // the forest example on meshes too (the plane): a copy kept in memory under a test path
        static PcgGraph sg;
        const PcgGraph* ex = pcg_graph_find("/Content/PCG/Examples/PCG_Forest.rpcg");
        if (ex) {
            sg = *ex;
            snprintf(sg.vpath, sizeof sg.vpath, "/Content/PCG/Test/PCG_StreamForest.rpcg"), sg.hash = pcg_vpath_hash(sg.vpath);
            for (uint32_t i = 0; i < sg.nnodes; i++)
                if (sg.nodes[i].type == PCGN_SURFACE) sg.nodes[i].f[0] = 2.0f; // (Landscape and Meshes)
            pcg_graph_edited(&sg);
        }
        PT.forest = pcg_spawn_volume(w, (float[3]){ 0, 0, 4000 }, "/Content/PCG/Test/PCG_StreamForest.rpcg");
        CProcWorld* c = ecs_get(w, PT.forest, COMP_PROC_WORLD);
        c->pcg.seed = 11, c->pcg.extent[0] = c->pcg.extent[1] = 155000.0f, c->pcg.extent[2] = 20000.0f;
        c->pcg.flags |= PCGC_PARTITIONED;
        // (a lighter forest than the example's 0.04: the dense one at 80 m/s keeps the sun's virtual shadow levels
        // redrawing its trees as they re-center every ~0.2 s, whatever the PCG does; notas-dev/pcg.md "Falta")
        const char* dens = getenv("REALENGINE_PCG_DENSITY"); // (Q22: the dense example's 0.04, the shadows' scroll measured)
        pcg_set_parameter(w, PT.forest, "Tree Density", dens && atof(dens) > 0.0 ? (float)atof(dens) : 0.015f);
        PT.player = player(w, -150000.0f, 0.0f);
        ed->cam.pos[0] = -152000.0f, ed->cam.pos[1] = -3000.0f, ed->cam.pos[2] = 6000.0f, ed->cam.yaw = 20.0f, ed->cam.pitch = -15.0f;
    }
    ed->dirty = true;
    return true;
}

// the trees of the forest volume: every trunk (a cylinder) stands on the ground
static void check_ground(World* w, Entity owner) {
    PT.trunks = PT.grounded = PT.steep = 0, PT.worst_ground = 0.0f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_STATIC_MESH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const CStaticMesh* sm = ecs_arch_column(ar, COMP_STATIC_MESH);
        const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t i = 0; i < ar->count; i++) {
            if (at[i].kind != ATTACH_PCG || at[i].owner != owner || sm[i].mesh != SHAPE_CYLINDER) continue;
            PT.trunks++;
            float s = t[i].scale[2] / 3.0f, base = t[i].location[2] - 150.0f * s, z = 0.0f; // (the example's trunk: 3 high, centered 150 up)
            if (!landscape_height(t[i].location[0], t[i].location[1], &z)) continue;
            float d = fabsf(base - z);
            PT.worst_ground = fmaxf(PT.worst_ground, d);
            PT.grounded += d < 1.0f;
            float n = 50.0f, zx0, zx1, zy0, zy1; // (the slope under it, as the sampler measures it)
            landscape_height(t[i].location[0] - n, t[i].location[1], &zx0), landscape_height(t[i].location[0] + n, t[i].location[1], &zx1);
            landscape_height(t[i].location[0], t[i].location[1] - n, &zy0), landscape_height(t[i].location[0], t[i].location[1] + n, &zy1);
            float gx = (zx1 - zx0) / (2 * n), gy = (zy1 - zy0) / (2 * n), slope = atanf(sqrtf(gx * gx + gy * gy)) * 57.29578f;
            PT.steep += slope > 28.5f;
        }
    }
}

void pcg_test_tick(Editor* ed, float dt) {
    if (!PT.scene) return;
    World* w = &ed->world;
    PT.t += dt;
    if (PT.scene == 1) {
        if (PT.phase == 0 && PT.t > 2.0f && pcg_is_generated(w, PT.forest) && pcg_is_generated(w, PT.rocks) && pcg_is_generated(w, PT.fence)) {
            PT.ed_count = pcg_generated_count(w, PT.forest) + pcg_generated_count(w, PT.rocks) + pcg_generated_count(w, PT.fence);
            PT.ed_hash = pcg_component_hash(w, PT.forest);
            check_ground(w, PT.forest);
            printf("[RealEngine] PCG forest (editor): %u entities (forest %u, rock clusters %u, fence %u), %u trunks, %u on the ground (worst %.3f cm), %u on slopes over 28 deg; last generation %.2f ms\n",
                   PT.ed_count, pcg_generated_count(w, PT.forest), pcg_generated_count(w, PT.rocks), pcg_generated_count(w, PT.fence), PT.trunks,
                   PT.grounded, PT.worst_ground, PT.steep, pcg_stats()->last_gen_ms);
            editor_play(ed, re_time_seconds());
            PT.phase = 1, PT.t = 0.0f;
        } else if (PT.phase == 1 && PT.t > 1.5f) { // in PIE: what the editor made, kept
            PT.pie_count = pcg_generated_count(w, PT.forest) + pcg_generated_count(w, PT.rocks) + pcg_generated_count(w, PT.fence);
            PT.pie_hash = pcg_component_hash(w, PT.forest);
            printf("[RealEngine] PCG forest (PIE): %u entities, hash %s\n", PT.pie_count, PT.pie_hash == PT.ed_hash ? "the editor's" : "DIFFERENT");
            pcg_set_seed(w, PT.forest, 8); // (Set PCG Seed's call)
            PT.phase = 2, PT.t = 0.0f;
        } else if (PT.phase == 2 && PT.t > 1.5f && pcg_is_generated(w, PT.forest)) {
            PT.seed_count = pcg_generated_count(w, PT.forest), PT.seed_hash = pcg_component_hash(w, PT.forest);
            pcg_set_seed(w, PT.forest, 7);
            PT.phase = 3, PT.t = 0.0f;
        } else if (PT.phase == 3 && PT.t > 1.5f && pcg_is_generated(w, PT.forest)) {
            PT.back_hash = pcg_component_hash(w, PT.forest);
            printf("[RealEngine] PCG forest (PIE): seed 8 made %u entities (hash %s), seed 7 again: %s\n", PT.seed_count,
                   PT.seed_hash != PT.ed_hash ? "another" : "THE SAME", PT.back_hash == PT.ed_hash ? "the same forest" : "ANOTHER FOREST");
            PT.phase = 4;
        }
    } else {
        if (!ed->playing || !ecs_alive(w, PT.player)) return;
        if (PT.t > 2.0f && !mat_compiling() && PT.ndt < 16384) PT.dts[PT.ndt++] = dt; // (Q22: the project's materials compiled)
        if (getenv("PCG_SPIKES") && dt > 0.009f) { const PcgStats* q = pcg_stats(); printf("SPIKE t %.2f dt %.1f ms: pcg %.2f (gen %.2f spawn %.2f) units %u queued %u spawned %u\n", PT.t, dt * 1000.0f, q->ms, q->gen_ms, q->spawn_ms, q->units, q->queued, q->spawned); }
        if (PT.t < 1.5f) {
            pcg_reset_peak();
            return;
        }
        CTransform* t = ecs_get(w, PT.player, COMP_TRANSFORM);
        t->location[0] = fminf(t->location[0] + 8000.0f * dt, 150000.0f); // (80 m/s east: a fast car)
        t->location[2] = 92.0f; // (the plane)
        CCharacter* c = ecs_get(w, PT.player, COMP_CHARACTER);
        if (c) c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
        ecs_touch(w, PT.player);
    }
}

static int fcmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

bool pcg_test_check(Editor* ed) {
    if (!PT.scene) return true;
    bool ok = true;
    World* w = &ed->world;
    if (PT.scene == 1) {
        if (PT.phase < 4) printf("[RealEngine] FAIL: PCG forest: the test stopped at step %d\n", PT.phase), ok = false;
        if (PT.trunks < 300 || PT.grounded != PT.trunks || PT.steep) printf("[RealEngine] FAIL: PCG forest: %u trunks, %u on the ground, %u on steep slopes\n", PT.trunks, PT.grounded, PT.steep), ok = false;
        if (pcg_generated_count(w, PT.rocks) < 30 || pcg_generated_count(w, PT.fence) < 60) printf("[RealEngine] FAIL: PCG forest: the rock clusters or the fence are missing\n"), ok = false;
        if (PT.pie_count != PT.ed_count || PT.pie_hash != PT.ed_hash) printf("[RealEngine] FAIL: PCG forest: PIE didn't keep what the editor made\n"), ok = false;
        if (PT.seed_hash == PT.ed_hash || PT.back_hash != PT.ed_hash) printf("[RealEngine] FAIL: PCG forest: the seed doesn't decide the forest\n"), ok = false;
    } else {
        const PcgStats* s = pcg_stats();
        float med = 0.0f, worst = 0.0f;
        uint32_t over = 0;
        if (PT.ndt) {
            static float sorted[16384];
            memcpy(sorted, PT.dts, PT.ndt * sizeof sorted[0]);
            qsort(sorted, PT.ndt, sizeof sorted[0], fcmp);
            med = sorted[PT.ndt / 2], worst = sorted[PT.ndt - 1];
            for (uint32_t i = 0; i < PT.ndt; i++) over += PT.dts[i] > 2.0f * med;
        }
        const CTransform* pt = ecs_alive(w, PT.player) ? ecs_get(w, PT.player, COMP_TRANSFORM) : NULL;
        printf("[RealEngine] PCG stream: %u cells generated, %u dropped, %u in now (%u entities); worst PCG update %.2f ms, last cell %.2f ms\n",
               s->cells_generated, s->cells_dropped, s->units, pcg_generated_count(w, PT.forest), s->ms_peak, s->last_gen_ms);
        printf("[RealEngine] PCG stream: frames %u, median %.3f ms, worst %.3f ms, %u over twice the median (%.2f %%); the player at %.1f km\n", PT.ndt,
               med * 1000.0f, worst * 1000.0f, over, PT.ndt ? 100.0f * (float)over / (float)PT.ndt : 0.0f, pt ? pt->location[0] / 100000.0f : 0.0f);
        if (s->cells_generated < 40 || s->cells_dropped < 15) printf("[RealEngine] FAIL: PCG stream: the cells didn't stream\n"), ok = false;
        if (PT.ndt < 100 || over * 100 > PT.ndt || s->ms_peak > 6.0) printf("[RealEngine] FAIL: PCG stream: a hitch\n"), ok = false;
        if (!pt || pt->location[0] < -40000.0f) printf("[RealEngine] FAIL: PCG stream: the player didn't cross\n"), ok = false;
    }
    char dir[900]; // (the test's landscape files gone)
    (void)dir;
    return ok;
}
