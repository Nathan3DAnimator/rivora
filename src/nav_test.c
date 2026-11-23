// The NavMesh (--nav-test, the test navmesh; notas-dev/ia.md "NavMesh"): a level of four lanes on a 60 m floor, an AI
// Character in each told to walk across (AI Move To):
//   1. stairs (12 steps of 20 cm) up to a 2.4 m platform;
//   2. a ramp (6 cm steps) up to a 1.2 m platform;
//   3. two 3 m blocks with a 1.5 m gap and a Nav Link over it (no other way: it must jump);
//   4. a wall with a 1.2 m corridor and, nearer, a 50 cm slot too narrow for the agent (it must go round by the corridor).
// Before playing, the NavMesh is built in the background (async) and queried: the paths' shapes, a projection, random
// points, their cost (ms). Played, each agent must reach its goal by the right way.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "navmesh.h"
#include "navmesh_internal.h"
#include "platform.h"
#include "proto.h"
#include "render.h"

enum { LANE_STAIRS = 0, LANE_RAMP, LANE_LINK, LANE_CORRIDOR, LANES };
static const char* const g_lane[LANES] = { "stairs", "ramp", "Nav Link", "corridor" };
static Entity g_agent[LANES];
static float g_start[LANES][3], g_goal[LANES][3];
static float g_min_y = 1e30f;          // the corridor agent's lowest y (it went by the corridor at y 1800)
static bool g_sent, g_link_in_path, g_slot_round, g_project_ok, g_random_ok, g_drop_ok;
static uint32_t g_path_pts[LANES];
static double g_path_ms, g_build_s;
static float g_floor_feet[LANES];
static double g_dense_ms;
// the rebuild of a tile: a Static box moved in the editor (its tiles only), then playing a Movable one (none) and the
// Static one again (its tiles)
static Entity g_static_box, g_movable_box;
static uint32_t g_edit_rebuilt, g_built_t0, g_built_t1, g_built_t2;
static int g_phase;
static uint32_t g_dense_polys;

// The builder alone on a dense tile (the worst a level gives one): 400 boxes, crates to walls, over a hilly landscape
static void dense_tile(void) {
    static NavInBox boxes[400];
    static float land[60 * 60];
    NavTileInput in;
    memset(&in, 0, sizeof in);
    NavCfg* c = &in.cfg;
    c->cs = 12.0f, c->ch = 5.0f, c->radius = 34.0f, c->height = 176.0f, c->climb = 45.0f, c->slope_deg = 45.0f;
    c->tile = 96, c->border = 6, c->max_error = 1.3f, c->max_edge = 100, c->min_region = 64, c->detail_step = 2;
    float side = (float)(c->tile + 2 * c->border) * c->cs;
    in.bmin[0] = in.bmin[1] = -(float)c->border * c->cs, in.bmin[2] = -300.0f, in.bmax[0] = in.bmax[1] = in.bmin[0] + side, in.bmax[2] = 600.0f;
    uint32_t r = 12345u;
#define RND() (r ^= r << 13, r ^= r >> 17, r ^= r << 5, (float)(r & 0xFFFF) / 65535.0f)
    for (int i = 0; i < 400; i++) {
        float x = in.bmin[0] + RND() * side, y = in.bmin[1] + RND() * side, wx = 40.0f + RND() * 160.0f, wy = 40.0f + RND() * 160.0f, h = 30.0f + RND() * 220.0f;
        boxes[i] = (NavInBox){ { x, y, -50.0f }, { x + wx, y + wy, h } };
    }
#undef RND
    in.boxes = boxes, in.nboxes = 400;
    in.lstep = 25.0f, in.lw = in.lh = (int)ceilf(side / 25.0f) + 1;
    if (in.lw > 60) in.lw = in.lh = 60;
    for (int y = 0; y < in.lh; y++)
        for (int x = 0; x < in.lw; x++) land[y * in.lw + x] = 80.0f * sinf((float)x * 25.0f / 300.0f) * cosf((float)y * 25.0f / 400.0f);
    in.land = land;
    double t0 = re_time_seconds();
    for (int k = 0; k < 10; k++) {
        NavTileData* d = nav_build_tile(&in);
        if (d) g_dense_polys = d->npolys;
        free(d);
    }
    g_dense_ms = (re_time_seconds() - t0) * 1000.0 / 10.0;
}

static Entity box(Editor* ed, float x0, float y0, float z0, float x1, float y1, float z1) {
    Entity e = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f }, 0.0);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    if (t) {
        t->scale[0] = (x1 - x0) / 100.0f, t->scale[1] = (y1 - y0) / 100.0f, t->scale[2] = (z1 - z0) / 100.0f;
        t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_STATIC;
    }
    return e;
}

static Entity proto(World* w, uint32_t kind, float x, float y, float L, float W, float H) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROTO));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", g_proto_kind_names[kind % PROTO_KIND_COUNT]);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 0.0f;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    CProto* p = ecs_get(w, e, COMP_PROTO);
    p->kind = kind, p->size[0] = L, p->size[1] = W, p->size[2] = H;
    return e;
}

static Entity agent(Editor* ed, int lane, const float at[3]) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_AI_AGENT));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Walker_%s", g_lane[lane]);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->location[2] += 89.0f;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 600.0f, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    CAIAgent* a = ecs_get(w, e, COMP_AI_AGENT);
    a->mode = AI_MODE_NONE, a->team = 0, a->sight_radius = 100.0f;
    return e;
}

// Each frame while playing: the walks sent once; the corridor walker's lowest y
static void step(Editor* ed, double dt) {
    (void)dt;
    World* w = &ed->world;
    if (!g_sent && ed->play_time > 0.2) {
        for (int i = 0; i < LANES; i++) ai_move_to(w, g_agent[i], g_goal[i], 40.0f);
        g_sent = true;
    }
    CTransform* mt = ecs_get(w, g_movable_box, COMP_TRANSFORM);
    CTransform* st = ecs_get(w, g_static_box, COMP_TRANSFORM);
    if (g_phase == 0 && ed->play_time > 2.0 && mt) { // a Movable one moves: no tile again
        g_built_t0 = navmesh_stats()->tiles_built;
        mt->location[0] += 300.0f, ecs_touch(w, g_movable_box);
        g_phase = 1;
    } else if (g_phase == 1 && ed->play_time > 3.0 && st) { // a Static one moves: its tiles again
        g_built_t1 = navmesh_stats()->tiles_built;
        st->location[1] += 300.0f, ecs_touch(w, g_static_box);
        g_phase = 2;
    } else if (g_phase == 2 && ed->play_time > 5.0) {
        g_built_t2 = navmesh_stats()->tiles_built;
        g_phase = 3;
    }
    const CTransform* t = ecs_alive(w, g_agent[LANE_CORRIDOR]) ? ecs_get(w, g_agent[LANE_CORRIDOR], COMP_TRANSFORM) : NULL;
    if (t && g_sent && t->location[0] > -350.0f && t->location[0] < 350.0f && t->location[1] < g_min_y) g_min_y = t->location[1];
}

bool nav_test_setup(Editor* ed) {
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, 0.0);
    CTransform* ft = ecs_get(w, floor, COMP_TRANSFORM);
    if (ft) ft->scale[0] = ft->scale[1] = 60.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    // 1. stairs (x -400..0, rising along +x to 240) and the platform after them (x 0..400)
    proto(w, PROTO_STAIRS, -200.0f, -1800.0f, 400.0f, 300.0f, 240.0f);
    proto(w, PROTO_PLATFORM, 200.0f, -1800.0f, 400.0f, 300.0f, 240.0f);
    g_start[LANE_STAIRS][0] = -1100.0f, g_start[LANE_STAIRS][1] = -1800.0f, g_start[LANE_STAIRS][2] = 0.0f;
    g_goal[LANE_STAIRS][0] = 250.0f, g_goal[LANE_STAIRS][1] = -1800.0f, g_goal[LANE_STAIRS][2] = 240.0f;
    g_floor_feet[LANE_STAIRS] = 240.0f;
    // 2. a ramp (x -400..0 up to 120) and a block after it (x 0..400, 120 high)
    proto(w, PROTO_RAMP, -200.0f, -600.0f, 400.0f, 300.0f, 120.0f);
    box(ed, 0.0f, -750.0f, 0.0f, 400.0f, -450.0f, 120.0f);
    g_start[LANE_RAMP][0] = -1100.0f, g_start[LANE_RAMP][1] = -600.0f, g_start[LANE_RAMP][2] = 0.0f;
    g_goal[LANE_RAMP][0] = 250.0f, g_goal[LANE_RAMP][1] = -600.0f, g_goal[LANE_RAMP][2] = 120.0f;
    g_floor_feet[LANE_RAMP] = 120.0f;
    // 3. two 3 m blocks, a 1.5 m gap (x 0..150), the Nav Link over it (both ways)
    box(ed, -700.0f, 450.0f, 0.0f, 0.0f, 750.0f, 300.0f);
    box(ed, 150.0f, 450.0f, 0.0f, 850.0f, 750.0f, 300.0f);
    Entity link = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_NAV));
    if (link) {
        snprintf(((CName*)ecs_get(w, link, COMP_NAME))->text, RE_NAME_MAX, "NavLinkProxy");
        CTransform* lt = ecs_get(w, link, COMP_TRANSFORM);
        lt->location[0] = 75.0f, lt->location[1] = 600.0f, lt->location[2] = 300.0f;
        lt->scale[0] = lt->scale[1] = lt->scale[2] = 1.0f;
        CNav* nv = ecs_get(w, link, COMP_NAV);
        nv->kind = NAV_LINK_PROXY, nv->direction = NAV_LINK_BOTH_WAYS, nv->snap_radius = 60.0f;
        nv->left[0] = -200.0f, nv->left[1] = nv->left[2] = 0.0f;
        nv->right[0] = 200.0f, nv->right[1] = nv->right[2] = 0.0f;
    }
    // and a one-way link down from block A's far side to the floor (dropped from, not jumped)
    Entity drop = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_NAV));
    if (drop) {
        snprintf(((CName*)ecs_get(w, drop, COMP_NAME))->text, RE_NAME_MAX, "NavLinkProxy_Drop");
        CTransform* dt = ecs_get(w, drop, COMP_TRANSFORM);
        dt->location[0] = -720.0f, dt->location[1] = 600.0f, dt->location[2] = 150.0f;
        dt->scale[0] = dt->scale[1] = dt->scale[2] = 1.0f;
        CNav* nv = ecs_get(w, drop, COMP_NAV);
        nv->kind = NAV_LINK_PROXY, nv->direction = NAV_LINK_LEFT_TO_RIGHT, nv->snap_radius = 60.0f;
        nv->left[0] = 80.0f, nv->left[1] = 0.0f, nv->left[2] = 150.0f;    // (-640, 600, 300): on block A
        nv->right[0] = -80.0f, nv->right[1] = 0.0f, nv->right[2] = -150.0f; // (-800, 600, 0): the floor
    }
    g_start[LANE_LINK][0] = -450.0f, g_start[LANE_LINK][1] = 600.0f, g_start[LANE_LINK][2] = 300.0f;
    g_goal[LANE_LINK][0] = 600.0f, g_goal[LANE_LINK][1] = 600.0f, g_goal[LANE_LINK][2] = 300.0f;
    g_floor_feet[LANE_LINK] = 300.0f;
    // 4. a wall across x 0 from y 1000 to past the floor: a 120 cm corridor at y 1800 (its walls along x -300..300) and a
    // 50 cm slot at y 2300 (straight between the start and the goal)
    box(ed, -10.0f, 1000.0f, 0.0f, 10.0f, 1740.0f, 300.0f);
    box(ed, -10.0f, 1860.0f, 0.0f, 10.0f, 2275.0f, 300.0f);
    box(ed, -10.0f, 2325.0f, 0.0f, 10.0f, 3200.0f, 300.0f);
    box(ed, -300.0f, 1720.0f, 0.0f, 300.0f, 1740.0f, 300.0f);
    box(ed, -300.0f, 1860.0f, 0.0f, 300.0f, 1880.0f, 300.0f);
    g_start[LANE_CORRIDOR][0] = -800.0f, g_start[LANE_CORRIDOR][1] = 2300.0f, g_start[LANE_CORRIDOR][2] = 0.0f;
    g_goal[LANE_CORRIDOR][0] = 800.0f, g_goal[LANE_CORRIDOR][1] = 2300.0f, g_goal[LANE_CORRIDOR][2] = 0.0f;
    g_floor_feet[LANE_CORRIDOR] = 0.0f;
    // two boxes away from the lanes, for the rebuilds
    g_static_box = box(ed, 2200.0f, -2700.0f, 0.0f, 2400.0f, -2500.0f, 200.0f);
    g_movable_box = box(ed, -2400.0f, -2700.0f, 0.0f, -2200.0f, -2500.0f, 200.0f);
    CTransform* mvt = ecs_get(w, g_movable_box, COMP_TRANSFORM);
    if (mvt) mvt->flags = (mvt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    proto_update(w);
    for (int i = 0; i < LANES; i++) g_agent[i] = agent(ed, i, g_start[i]);
    // the NavMesh, built in the background (the editor's async workers), then asked
    double t0 = re_time_seconds();
    navmesh_wait(w, 30.0);
    g_build_s = re_time_seconds() - t0;
    const NavMeshStats* s = navmesh_stats();
    { // the editor: the Static box moved 3 m, its tiles (where it was and is, with their borders) built again, no more
        uint32_t before = s->tiles_built, batches = s->builds;
        CTransform* sbt = ecs_get(w, g_static_box, COMP_TRANSFORM);
        if (sbt) sbt->location[0] += 300.0f, ecs_touch(w, g_static_box);
        double r0 = re_time_seconds();
        navmesh_wait(w, 10.0);
        g_edit_rebuilt = s->tiles_built - before;
        printf("[RealEngine] navmesh: a Static box moved 3 m in the editor: %u tiles built again in %u batch (%.2f ms on the worker, %.1f ms waited)\n",
               g_edit_rebuilt, s->builds - batches, s->last_batch_ms, (re_time_seconds() - r0) * 1000.0);
    }
    printf("[RealEngine] navmesh: %u tiles, %u polygons, %u vertices, %u links, %u Nav Links (%u on the mesh); %u batches, worker %.1f ms (the slowest tile %.2f ms), "
           "put in place %.2f ms, inputs %.2f ms; waited %.2f s; cell %.0f cm, tile %.0f cm, agent r %.0f h %.0f climb %.0f\n",
           s->tiles, s->polys, s->verts, s->links, s->offmesh, s->offmesh_linked, s->builds, s->total_build_ms, s->max_tile_ms, s->apply_ms, s->gather_ms,
           g_build_s, s->cell, s->tile_size, s->agent_radius, s->agent_height, s->agent_climb);
    double q0 = re_time_seconds();
    int reps = 50;
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < LANES; i++) {
            float pts[64][3];
            uint8_t fl[64];
            uint32_t n = 0;
            float from[3] = { g_start[i][0], g_start[i][1], g_start[i][2] + 1.0f }, to[3] = { g_goal[i][0], g_goal[i][1], g_goal[i][2] + 1.0f };
            navmesh_path(from, to, pts, fl, 64, &n);
            if (r) continue;
            g_path_pts[i] = n;
            printf("[RealEngine] navmesh: the %s path, %u points:", g_lane[i], n);
            for (uint32_t k = 0; k < n; k++) printf(" (%.0f %.0f %.0f)%s", pts[k][0], pts[k][1], pts[k][2], fl[k] & NAV_PT_LINK ? (fl[k] & NAV_PT_LINK_JUMP ? "J" : "L") : "");
            printf("\n");
            if (i == LANE_LINK)
                for (uint32_t k = 0; k < n; k++) g_link_in_path |= (fl[k] & NAV_PT_LINK_JUMP) != 0;
        }
    g_path_ms = (re_time_seconds() - q0) * 1000.0 / (reps * LANES);
    { // down by the one-way link (a drop: no jump); up, none (the link goes one way)
        float top[3] = { -450.0f, 600.0f, 301.0f }, ground[3] = { -1100.0f, 600.0f, 1.0f }, pts[64][3];
        uint8_t fl[64];
        uint32_t n = 0, m = 0;
        bool down = false, up = false;
        if (navmesh_path(top, ground, pts, fl, 64, &n))
            for (uint32_t k = 0; k < n; k++) down |= (fl[k] & NAV_PT_LINK) && !(fl[k] & NAV_PT_LINK_JUMP);
        bool reached = n && fabsf(pts[n - 1][0] - ground[0]) < 50.0f && fabsf(pts[n - 1][2]) < 20.0f;
        if (navmesh_path(ground, top, pts, fl, 64, &m))
            for (uint32_t k = 0; k < m; k++) up |= (fl[k] & NAV_PT_LINK) != 0;
        g_drop_ok = down && reached && !up;
        printf("[RealEngine] navmesh: down the one-way link: %s (%u points, a drop), back up by it: %s\n", down && reached ? "yes" : "no", n, up ? "yes (wrong)" : "no");
    }
    // the slot is too narrow: straight through it is no path; round by the corridor
    float a[3] = { -150.0f, 2300.0f, 1.0f }, b[3] = { 150.0f, 2300.0f, 1.0f };
    float len = navmesh_path_length(a, b);
    g_slot_round = len > 1000.0f;
    // a point inside the wall projected out of it; random points around the ramp's start, each one reachable
    float in_wall[3] = { 0.0f, 1500.0f, 50.0f }, ext[3] = { 150.0f, 150.0f, 300.0f }, out[3];
    g_project_ok = navmesh_project(in_wall, ext, out) && fabsf(out[0]) > 30.0f && fabsf(out[2]) < 20.0f;
    int reach = 0;
    for (int k = 0; k < 20; k++) {
        float p[3];
        if (!navmesh_random_point(g_start[LANE_RAMP], 500.0f, p)) continue;
        float dx = p[0] - g_start[LANE_RAMP][0], dy = p[1] - g_start[LANE_RAMP][1];
        reach += sqrtf(dx * dx + dy * dy) < 650.0f && navmesh_path_length(g_start[LANE_RAMP], p) >= 0.0f;
    }
    g_random_ok = reach >= 18;
    printf("[RealEngine] navmesh: a path %.3f ms (avg of %d); through the slot %.0f cm (round by the corridor: %s); the wall's point projected to (%.0f %.0f %.0f): %s; "
           "random points reachable %d / 20; queries avg %.1f us\n",
           g_path_ms, reps * LANES, len, g_slot_round ? "yes" : "no", out[0], out[1], out[2], g_project_ok ? "yes" : "no", reach, s->query_us_avg);
    dense_tile();
    printf("[RealEngine] navmesh: a dense tile (11.5 m, 400 boxes over a hilly landscape): %u polygons in %.2f ms (avg of 10, one thread)\n", g_dense_polys, g_dense_ms);
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -3200.0f, ed->cam.pos[2] = 2600.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -45.0f;
    ed->show_nav = true; // (P: the NavMesh shown, as a check that it draws)
    ed->test_step = step;
    g_sent = false, g_min_y = 1e30f, g_phase = 0;
    editor_play(ed, 0.0);
    return navmesh_ready();
}

bool nav_test_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = navmesh_ready();
    if (!ok) printf("[RealEngine] FAIL: navmesh: not built\n");
    for (int i = 0; i < LANES; i++) {
        const CTransform* t = ecs_alive(w, g_agent[i]) ? ecs_get(w, g_agent[i], COMP_TRANSFORM) : NULL;
        if (!t) {
            printf("[RealEngine] FAIL: navmesh: the %s walker is gone\n", g_lane[i]);
            ok = false;
            continue;
        }
        float dx = t->location[0] - g_goal[i][0], dy = t->location[1] - g_goal[i][1], d = sqrtf(dx * dx + dy * dy);
        float feet = t->location[2] - 88.0f;
        bool there = d < 120.0f && fabsf(feet - g_floor_feet[i]) < 15.0f;
        printf("[RealEngine] navmesh: the %s walker at (%.0f %.0f), feet %.1f cm: %.0f cm from its goal%s\n", g_lane[i], t->location[0], t->location[1], feet, d,
               there ? "" : " (not there)");
        if (!there) printf("[RealEngine] FAIL: navmesh: the %s walker did not reach its goal\n", g_lane[i]), ok = false;
        if (g_path_pts[i] == 0) printf("[RealEngine] FAIL: navmesh: no %s path\n", g_lane[i]), ok = false;
    }
    if (!g_link_in_path) printf("[RealEngine] FAIL: navmesh: the gap's path does not jump the Nav Link\n"), ok = false;
    if (!g_slot_round) printf("[RealEngine] FAIL: navmesh: a path through the 50 cm slot\n"), ok = false;
    if (g_min_y > 1900.0f || g_min_y < 1700.0f) printf("[RealEngine] FAIL: navmesh: the corridor walker did not go by the corridor (lowest y %.0f)\n", g_min_y), ok = false;
    if (!g_drop_ok) printf("[RealEngine] FAIL: navmesh: the one-way drop link\n"), ok = false;
    if (!g_project_ok) printf("[RealEngine] FAIL: navmesh: Project Point to Navigation\n"), ok = false;
    if (!g_random_ok) printf("[RealEngine] FAIL: navmesh: random reachable points\n"), ok = false;
    const NavMeshStats* s = navmesh_stats();
    if (g_path_ms > 1.0) printf("[RealEngine] FAIL: navmesh: a path %.3f ms (more than 1 ms)\n", g_path_ms), ok = false;
    printf("[RealEngine] navmesh: playing, a Movable box moved: %u tiles built again; a Static one: %u\n", g_built_t1 - g_built_t0, g_built_t2 - g_built_t1);
    if (g_edit_rebuilt < 1 || g_edit_rebuilt > 6) printf("[RealEngine] FAIL: navmesh: a moved Static box rebuilt %u tiles (1 to 6 expected)\n", g_edit_rebuilt), ok = false;
    if (g_phase < 3 || g_built_t1 != g_built_t0) printf("[RealEngine] FAIL: navmesh: playing, a Movable box rebuilt tiles\n"), ok = false;
    if (g_phase < 3 || g_built_t2 - g_built_t1 < 1 || g_built_t2 - g_built_t1 > 6) printf("[RealEngine] FAIL: navmesh: playing, a moved Static box rebuilt %u tiles\n", g_built_t2 - g_built_t1), ok = false;
    if (!g_dense_polys || g_dense_ms > 50.0) printf("[RealEngine] FAIL: navmesh: the dense tile: %u polygons, %.1f ms (more than 50)\n", g_dense_polys, g_dense_ms), ok = false;
    if (s->max_tile_ms > 100.0) printf("[RealEngine] FAIL: navmesh: a tile took %.1f ms (more than 100)\n", s->max_tile_ms), ok = false;
    printf("[RealEngine] navmesh: corridor walker's lowest y %.0f; %s\n", g_min_y, ok ? "NavMesh OK" : "failed");
    editor_stop(ed, 0.0);
    return ok;
}
