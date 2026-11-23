// The Character against physics and moving things (--char-physics-test, the test char_physics; notas-dev/fisica.md
// "Character contra física"): on a floor, a Character walking along +x steps up a 30 cm step, walks on it into a 50 cm
// box with physics and pushes it; a second Character stands still on a Movable platform this test slides along +y.
// Checked at the end of the run: the first on the step and past the box's start, the box pushed, the second carried.
// The leftovers (2026-11-09): a wall spawned while playing stops a third Character (its box added without making the
// level's grid again) until it is destroyed (its box swept out); a turning platform turns its rider and its control yaw
// (the camera); 3000 small cubes make the level's grid big enough to measure an add, a sweep and a rebuild.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "blueprint.h"
#include "ecs.h"
#include "editor.h"
#include "render.h"

static Entity g_walker, g_rider, g_box, g_platform;
static float g_box_x0, g_rider_y0, g_platform_y0;
static Entity g_blocked, g_wall, g_turner, g_spinner; // (the spawned wall's Character; the turning platform and its rider)
static float g_blocked_x_at_wall = -1.0f, g_spin_yaw0, g_turner_yaw0, g_turner_ctl0;
static bool g_spin_started; // (the turning platform's measure began: its rider on it)
static int g_box_phase;
static double g_add_ms = -1, g_sweep_ms = -1, g_rebuild_ms = -1;
static uint32_t g_boxes_level;
static Entity g_spawned; // (2026-11-10) a Character starting on a Plane scaled the same on every axis (the user's FirstPersonMap)

static Entity cube(Editor* ed, const float at[3], const float scale[3], uint32_t mobility) {
    Entity e = editor_spawn_shape(ed, SHAPE_CUBE, at, 0.0);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    if (t) memcpy(t->scale, scale, 12), t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | mobility;
    return e;
}

static Entity character(Editor* ed, const float at[3]) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 300.0f, c->jump_z_velocity = 420.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "TestCharacter");
    return e;
}

// Each frame while playing (before the Blueprints and the Characters move): the walker's input, the platform's slide
static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    CCharacter* c = ecs_alive(w, g_walker) ? ecs_get(w, g_walker, COMP_CHARACTER) : NULL;
    if (c) c->input[0] = 1.0f, c->input[1] = 0.0f;
    CTransform* p = ecs_alive(w, g_platform) ? ecs_get(w, g_platform, COMP_TRANSFORM) : NULL;
    if (p && ed->play_time > 0.3) { // (the rider settled first)
        p->location[1] += (float)(100.0 * dt);
        ecs_touch(w, g_platform);
    }
    CCharacter* bc = ecs_alive(w, g_blocked) ? ecs_get(w, g_blocked, COMP_CHARACTER) : NULL;
    if (bc) bc->input[0] = 1.0f, bc->input[1] = 0.0f;
    CTransform* sp = ecs_alive(w, g_spinner) ? ecs_get(w, g_spinner, COMP_TRANSFORM) : NULL;
    if (sp && ed->play_time > 0.3) { // 45 degrees a second around its center (z)
        // (the measure starts once its rider stands on it while it turns: a long first frame before it landed turned the
        // platform alone, a constant offset that read as the ride lagging)
        const CCharacter* tc = ecs_alive(w, g_turner) ? ecs_get(w, g_turner, COMP_CHARACTER) : NULL;
        const CTransform* tt = ecs_alive(w, g_turner) ? ecs_get(w, g_turner, COMP_TRANSFORM) : NULL;
        if (!g_spin_started && tc && tt && tc->on_ground) {
            g_spin_started = true;
            g_spin_yaw0 = sp->rotation[1], g_turner_yaw0 = tt->rotation[1], g_turner_ctl0 = tc->control_yaw;
        }
        sp->rotation[1] += (float)(45.0 * dt);
        ecs_touch(w, g_spinner);
    }
    // (the rebuild measured first, before the platforms move: a rebuild starts the boxes' moves again, a frame's carry lost)
    if (g_box_phase == 0 && ed->play_time > 0.05) {
        bp_boxes_changed(); // the whole set made again at the next tick
        g_box_phase = 1;
    } else if (g_box_phase == 1) {
        bp_box_stats(NULL, NULL, NULL, &g_rebuild_ms);
        g_box_phase = 2;
    } else if (g_box_phase == 2 && ed->play_time > 0.4) { // the wall, spawned while playing in the third Character's way
        g_wall = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ 500, -1400, 100 }, 0.0);
        CTransform* t = ecs_get(w, g_wall, COMP_TRANSFORM);
        if (t) t->scale[0] = 0.5f, t->scale[1] = 3.0f, t->scale[2] = 2.0f, t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_STATIC;
        bp_box_spawned(g_wall); // (what Spawn Actor does: bp_spawn)
        g_box_phase = 3;
    } else if (g_box_phase == 3) {
        bp_box_stats(&g_boxes_level, NULL, NULL, &g_add_ms); // (added at the tick after the spawn)
        g_box_phase = 4;
    } else if (g_box_phase == 4 && ed->play_time > 2.2) {
        const CTransform* bt = ecs_get(w, g_blocked, COMP_TRANSFORM);
        g_blocked_x_at_wall = bt ? bt->location[0] : -1.0f;
        bp_destroy_actor(w, g_wall, false);
        g_box_phase = 5;
    } else if (g_box_phase == 5) {
        bp_box_stats(NULL, NULL, NULL, &g_sweep_ms);
        g_box_phase = 6;
    }
}

bool char_test_setup(Editor* ed) {
    World* w = &ed->world;
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, 0.0);
    CTransform* ft = ecs_get(w, floor, COMP_TRANSFORM);
    if (ft) ft->scale[0] = ft->scale[1] = 100.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    // the step: 30 cm high from x 200 to 1400 (its top at z 30)
    cube(ed, (float[3]){ 800, -700, 15 }, (float[3]){ 12, 3, 0.3f }, MOBILITY_STATIC);
    // the box with physics on the step, in the walker's way
    g_box = cube(ed, (float[3]){ 600, -700, 56 }, (float[3]){ 0.5f, 0.5f, 0.5f }, MOBILITY_MOVABLE);
    if (ecs_add(w, g_box, COMP_RIGID_BODY)) {
        CRigidBody* rb = ecs_get(w, g_box, COMP_RIGID_BODY);
        rb->simulate = 1, rb->enable_gravity = 1, rb->mass = 20.0f, rb->friction = 0.4f;
    }
    // the platform (Movable) and its rider, away from the walker
    g_platform = cube(ed, (float[3]){ 0, 800, 20 }, (float[3]){ 2, 2, 0.4f }, MOBILITY_MOVABLE);
    g_walker = character(ed, (float[3]){ 0, -700, 90 }); // (y -700: clear of the default level's shapes)
    g_rider = character(ed, (float[3]){ 0, 800, 40 + 88 + 1 });
    // the leftovers: a third Character the spawned wall stops, the turning platform with its rider 80 cm off its center,
    // 3000 small cubes far off (the level's grid: a measure of adding to it, sweeping it, making it again)
    g_blocked = character(ed, (float[3]){ 0, -1400, 90 });
    g_spinner = cube(ed, (float[3]){ 0, 2000, 20 }, (float[3]){ 3, 3, 0.4f }, MOBILITY_MOVABLE);
    g_turner = character(ed, (float[3]){ 80, 2000, 40 + 88 + 1 });
    for (int i = 0; i < 3000; i++)
        cube(ed, (float[3]){ 3000.0f + (float)(i % 60) * 150.0f, 4000.0f + (float)(i / 60) * 150.0f, 25 }, (float[3]){ 0.5f, 0.5f, 0.5f }, MOBILITY_STATIC);
    // a Plane scaled 30 on every axis (its thickness was scaled too: a 30 cm box around the feet pushed the Character out
    // sideways, to the floor's edge, on the first tick) with a Character standing on it
    Entity big = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ -8000, -8000, 200 }, 0.0);
    CTransform* bgt = ecs_get(w, big, COMP_TRANSFORM);
    if (bgt) bgt->scale[0] = bgt->scale[1] = bgt->scale[2] = 30.0f, bgt->flags = MOBILITY_STATIC;
    g_spawned = character(ed, (float[3]){ -8000, -8000, 200 + 88 + 2 });
    const CTransform* bt = ecs_get(w, g_box, COMP_TRANSFORM);
    g_box_x0 = bt ? bt->location[0] : 0.0f;
    g_rider_y0 = 800.0f, g_platform_y0 = 800.0f;
    g_spin_yaw0 = g_turner_yaw0 = g_turner_ctl0 = 0.0f, g_box_phase = 0, g_spin_started = false;
    ed->cam.pos[0] = 300.0f, ed->cam.pos[1] = -1500.0f, ed->cam.pos[2] = 400.0f;
    ed->cam.yaw = 75.0f, ed->cam.pitch = -20.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return g_walker != ENTITY_NONE && g_rider != ENTITY_NONE && g_box != ENTITY_NONE;
}

bool char_test_check(Editor* ed) {
    World* w = &ed->world;
    const CTransform* wt = ecs_get(w, g_walker, COMP_TRANSFORM);
    const CTransform* rt = ecs_get(w, g_rider, COMP_TRANSFORM);
    const CTransform* bt = ecs_get(w, g_box, COMP_TRANSFORM);
    const CTransform* pt = ecs_get(w, g_platform, COMP_TRANSFORM);
    const CCharacter* rc = ecs_get(w, g_rider, COMP_CHARACTER);
    if (!wt || !rt || !bt || !pt || !rc) {
        printf("[RealEngine] FAIL: char-physics: the actors are gone\n");
        return false;
    }
    float moved = pt->location[1] - g_platform_y0, carried = rt->location[1] - g_rider_y0;
    printf("[RealEngine] char-physics: walker at (%.0f, %.0f, %.1f); box x %.0f (from %.0f); platform moved %.0f cm, rider %.0f cm, z %.1f, on ground %u\n",
           wt->location[0], wt->location[1], wt->location[2], bt->location[0], g_box_x0, moved, carried, rt->location[2], rc->on_ground);
    bool ok = true;
    if (wt->location[2] < 30.0f + 88.0f - 3.0f) printf("[RealEngine] FAIL: char-physics: the walker didn't step up (z %.1f)\n", wt->location[2]), ok = false;
    if (bt->location[0] < g_box_x0 + 100.0f) printf("[RealEngine] FAIL: char-physics: the box wasn't pushed\n"), ok = false;
    if (wt->location[0] < g_box_x0) printf("[RealEngine] FAIL: char-physics: the walker stopped at the box\n"), ok = false;
    if (moved < 100.0f || fabsf(carried - moved) > 10.0f || !rc->on_ground)
        printf("[RealEngine] FAIL: char-physics: the rider wasn't carried by the platform\n"), ok = false;
    // the spawned wall: it stopped the third Character (its front at 475, the capsule's radius 34) until it went
    const CTransform* bk = ecs_get(w, g_blocked, COMP_TRANSFORM);
    uint32_t boxes = 0, spawned = 0, turns = 0;
    bp_box_stats(&boxes, &spawned, &turns, NULL);
    printf("[RealEngine] char-physics: spawned wall: the Character at x %.0f while it stood, %.0f at the end; %u boxes, %u added "
           "while playing; add %.3f ms, sweep %.3f ms, rebuild %.3f ms (%u boxes)\n",
           g_blocked_x_at_wall, bk ? bk->location[0] : -1.0f, boxes, spawned, g_add_ms, g_sweep_ms, g_rebuild_ms, g_boxes_level);
    if (g_box_phase < 6) printf("[RealEngine] FAIL: char-physics: the wall's steps didn't all run (%d)\n", g_box_phase), ok = false;
    if (!spawned) printf("[RealEngine] FAIL: char-physics: the spawned wall's box wasn't added\n"), ok = false;
    if (g_blocked_x_at_wall < 380.0f || g_blocked_x_at_wall > 445.0f)
        printf("[RealEngine] FAIL: char-physics: the spawned wall didn't stop the Character (x %.0f)\n", g_blocked_x_at_wall), ok = false;
    if (!bk || bk->location[0] < 560.0f) printf("[RealEngine] FAIL: char-physics: the destroyed wall still blocks\n"), ok = false;
    // the turning platform: the rider turned with it, around its center, and its control yaw (the camera) too
    const CTransform* st = ecs_get(w, g_spinner, COMP_TRANSFORM);
    const CTransform* tt = ecs_get(w, g_turner, COMP_TRANSFORM);
    const CCharacter* tc = ecs_get(w, g_turner, COMP_CHARACTER);
    if (st && tt && tc) {
        float spun = st->rotation[1] - g_spin_yaw0;
        float dx = tt->location[0] - st->location[0], dy = tt->location[1] - st->location[1];
        float r = sqrtf(dx * dx + dy * dy), around = atan2f(dy, dx) * 57.29578f;
        float body = remainderf(tt->rotation[1] - g_turner_yaw0 - spun, 360.0f), at = remainderf(around - spun, 360.0f);
        float ctl = remainderf(tc->control_yaw - g_turner_ctl0 - spun, 360.0f);
        printf("[RealEngine] char-physics: turning platform %.1f deg: rider at %.1f cm from its center, %.1f deg around it, its "
               "yaw off by %.1f, control yaw off by %.1f, %u frames turned\n", spun, r, around, body, ctl, turns);
        if (spun < 45.0f || fabsf(r - 80.0f) > 6.0f || fabsf(at) > 4.0f || fabsf(body) > 4.0f || fabsf(ctl) > 4.0f || !tc->on_ground)
            printf("[RealEngine] FAIL: char-physics: the turning platform didn't turn its rider\n"), ok = false;
    } else
        printf("[RealEngine] FAIL: char-physics: the turning platform is gone\n"), ok = false;
    const CTransform* sg = ecs_get(w, g_spawned, COMP_TRANSFORM);
    if (sg) printf("[RealEngine] char-physics: on the Plane scaled 30: the Character at (%.0f, %.0f, %.1f)\n", sg->location[0], sg->location[1], sg->location[2]);
    if (!sg || fabsf(sg->location[0] + 8000.0f) > 1.0f || fabsf(sg->location[1] + 8000.0f) > 1.0f || fabsf(sg->location[2] - (200.0f + 1.0f + 88.0f)) > 3.0f)
        printf("[RealEngine] FAIL: char-physics: the Character didn't stand where it started on the scaled Plane\n"), ok = false;
    editor_stop(ed, 0.0);
    return ok;
}
