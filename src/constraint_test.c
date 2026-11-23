// Physics Constraints (--constraint-test, the test constraints; notas-dev/fisica.md "Physics Constraint"): away from the
// default level's shapes (y 1500), a pendulum held to the world by a ball joint, a door on a vertical hinge (limit 60
// degrees) pushed open, two boxes joined fixed falling to the floor, and a box on a slide (X) whose motor drives it to
// its 150 cm limit. Checked at the end: the anchors never drifted apart, each moved as its joint allows.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "physics.h"
#include "render.h"

static Entity g_pend, g_door, g_fa, g_fb, g_slider;
static float g_pend0[3], g_door_yaw0, g_slider_x0;

static Entity body(Editor* ed, const float at[3], const float scale[3], float mass) {
    World* w = &ed->world;
    Entity e = editor_spawn_shape(ed, SHAPE_CUBE, at, 0.0);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) memcpy(t->scale, scale, 12), t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    if (ecs_add(w, e, COMP_RIGID_BODY)) {
        CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
        rb->simulate = 1, rb->enable_gravity = 1, rb->mass = mass, rb->friction = 0.5f;
    }
    return e;
}

static void joint(Editor* ed, const float at[3], float pitch, Entity a1, Entity a2, uint32_t type, float limit, bool motor, float speed) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CONSTRAINT));
    if (e == ENTITY_NONE) return;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->rotation[0] = pitch; // (X: the hinge's / the slide's axis)
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "PhysicsConstraint");
    CConstraint* c = ecs_get(w, e, COMP_CONSTRAINT);
    c->actor1 = a1, c->actor2 = a2, c->type = type, c->limit = limit, c->motor = motor, c->motor_speed = speed;
}

bool constraint_test_setup(Editor* ed) {
    World* w = &ed->world;
    const float Y = 1500.0f;
    g_pend = body(ed, (float[3]){ 100, Y, 300 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f);
    joint(ed, (float[3]){ 0, Y, 300 }, 0.0f, g_pend, ENTITY_NONE, CONSTRAINT_BALL, 0.0f, false, 0.0f);
    g_door = body(ed, (float[3]){ 400, Y + 50, 110 }, (float[3]){ 0.1f, 1.0f, 2.0f }, 20.0f);
    CRigidBody* drb = ecs_get(w, g_door, COMP_RIGID_BODY);
    if (drb) drb->velocity[0] = 250.0f; // (pushed open: around the vertical hinge)
    joint(ed, (float[3]){ 400, Y, 110 }, 90.0f, g_door, ENTITY_NONE, CONSTRAINT_HINGE, 60.0f, false, 0.0f);
    g_fa = body(ed, (float[3]){ 800, Y, 300 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f);
    g_fb = body(ed, (float[3]){ 840, Y, 340 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f);
    joint(ed, (float[3]){ 820, Y, 320 }, 0.0f, g_fa, g_fb, CONSTRAINT_FIXED, 0.0f, false, 0.0f);
    g_slider = body(ed, (float[3]){ 1200, Y, 150 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f);
    joint(ed, (float[3]){ 1200, Y, 150 }, 0.0f, g_slider, ENTITY_NONE, CONSTRAINT_PRISMATIC, 150.0f, true, 100.0f);
    const CTransform* pt = ecs_get(w, g_pend, COMP_TRANSFORM);
    const CTransform* dt = ecs_get(w, g_door, COMP_TRANSFORM);
    if (pt) memcpy(g_pend0, pt->location, 12);
    g_door_yaw0 = dt ? dt->rotation[1] : 0.0f;
    g_slider_x0 = 1200.0f;
    ed->cam.pos[0] = 600.0f, ed->cam.pos[1] = Y - 1300.0f, ed->cam.pos[2] = 400.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
    editor_play(ed, 0.0);
    return g_pend && g_door && g_fa && g_fb && g_slider;
}

bool constraint_test_check(Editor* ed) {
    World* w = &ed->world;
    const PhysicsStats* ps = physics_stats();
    const CTransform* pt = ecs_get(w, g_pend, COMP_TRANSFORM);
    const CTransform* dt = ecs_get(w, g_door, COMP_TRANSFORM);
    const CTransform* at = ecs_get(w, g_fa, COMP_TRANSFORM);
    const CTransform* bt = ecs_get(w, g_fb, COMP_TRANSFORM);
    const CTransform* st = ecs_get(w, g_slider, COMP_TRANSFORM);
    if (!pt || !dt || !at || !bt || !st) {
        printf("[RealEngine] FAIL: constraints: the actors are gone\n");
        return false;
    }
    float pd[3] = { pt->location[0], pt->location[1] - 1500.0f, pt->location[2] - 300.0f };
    float arm = sqrtf(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
    float moved = fabsf(pt->location[2] - g_pend0[2]) + fabsf(pt->location[0] - g_pend0[0]);
    float ab[3] = { bt->location[0] - at->location[0], bt->location[1] - at->location[1], bt->location[2] - at->location[2] };
    float dist = sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
    float yaw = remainderf(dt->rotation[1] - g_door_yaw0, 360.0f), slide = st->location[0] - g_slider_x0;
    printf("[RealEngine] constraints: %u joints, anchors drifted at most %.2f cm; pendulum arm %.1f cm (moved %.0f); door turned %.1f deg; "
           "fixed pair %.1f cm apart (z %.0f); slider %.1f cm along X (y %.1f, z %.1f)\n",
           ps->joints, ps->joint_error_max, arm, moved, yaw, dist, at->location[2], slide, st->location[1] - 1500.0f, st->location[2]);
    bool ok = true;
    if (ps->joints != 4) printf("[RealEngine] FAIL: constraints: %u joints (4 expected)\n", ps->joints), ok = false;
    if (ps->joint_error_max > 3.0f) printf("[RealEngine] FAIL: constraints: the anchors drifted %.2f cm apart\n", ps->joint_error_max), ok = false;
    // (it swings: away from where it was let go, or near it with the speed of its drop there, v^2 = 2 g h: Q8's solver keeps
    // the energy, a full period brings it home)
    const CRigidBody* prb = ecs_get(w, g_pend, COMP_RIGID_BODY);
    float drop = fmaxf(g_pend0[2] - pt->location[2], 0.0f), pv = prb ? sqrtf(prb->velocity[0] * prb->velocity[0] + prb->velocity[1] * prb->velocity[1] +
                                                                                 prb->velocity[2] * prb->velocity[2]) : 0.0f;
    bool swinging = moved >= 30.0f || pv >= 0.5f * sqrtf(2.0f * 980.0f * drop);
    if (fabsf(arm - 100.0f) > 3.0f || !swinging) printf("[RealEngine] FAIL: constraints: the pendulum\n"), ok = false;
    if (fabsf(yaw) < 10.0f || fabsf(yaw) > 63.0f) printf("[RealEngine] FAIL: constraints: the hinge (turned %.1f, limit 60)\n", yaw), ok = false;
    if (fabsf(dist - 56.6f) > 3.0f || at->location[2] > 200.0f) printf("[RealEngine] FAIL: constraints: the fixed pair\n"), ok = false;
    if (slide < 140.0f || slide > 153.0f || fabsf(st->location[1] - 1500.0f) > 3.0f || fabsf(st->location[2] - 150.0f) > 3.0f)
        printf("[RealEngine] FAIL: constraints: the slide (motor to its 150 cm limit, held on its axis)\n"), ok = false;
    editor_stop(ed, 0.0);
    return ok;
}
