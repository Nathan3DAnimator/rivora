// The joints' test (Q8, --scene phys_joints --play --bench 5; notas-dev/fisica.md "Juntas"): away from the floor's middle
// (y 3000), each joint with what it promises:
//   hinge:   a door with a 45 degree limit driven by a motor at 60 deg/s: it stops at the limit
//   wheel:   a hinge with a motor at 180 deg/s and nothing else: it spins at that speed
//   slider:  a box on X, its motor at 80 cm/s to its 100 cm limit, held on the axis
//   cone:    a pendulum thrown sideways inside a 30 degree cone: it never leaves it
//   drive:   a 6DOF free along X, Y, Z pulled by a linear drive (2 Hz) to 100 cm along X, no gravity: it gets there
//   slerp:   a 6DOF held at its anchor turned by an angular drive to yaw 90: it turns there
//   soft:    a 6DOF hanging along its X (down) with a soft limit of 30 cm: it goes past it, springs back, rests past it
//   break:   a box held to the world by a fixed joint that breaks at 50 kg of pull while it weighs 80: it falls
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "phys_test.h"
#include "physics.h"
#include "render.h"

enum { J_HINGE = 0, J_WHEEL, J_SLIDER, J_CONE, J_DRIVE, J_SLERP, J_SOFT, J_BREAK, J_COUNT };
static Entity g_b[J_COUNT], g_c[J_COUNT];
static float g_p0[J_COUNT][3], g_cone_max, g_soft_max, g_wheel_w, g_slider_off;
static const float Y = 3000.0f;

static Entity jbody(Editor* ed, const float at[3], const float scale[3], float mass, bool gravity) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_RIGID_BODY));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12), memcpy(t->scale, scale, 12);
    t->flags = MOBILITY_MOVABLE;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = SHAPE_CUBE, m->base_color[0] = 0.8f, m->base_color[1] = 0.5f, m->base_color[2] = 0.2f, m->roughness = 0.5f, m->cast_shadow = 1;
    CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
    rb->mass = mass, rb->enable_gravity = gravity, rb->linear_damping = 0.0f, rb->angular_damping = 0.0f;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "JointBody");
    return e;
}

static CConstraint* jjoint(Editor* ed, int k, const float at[3], const float rot[3], Entity a1, uint32_t type) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CONSTRAINT));
    if (e == ENTITY_NONE) return NULL;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12), memcpy(t->rotation, rot, 12);
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "PhysicsConstraint");
    CConstraint* c = ecs_get(w, e, COMP_CONSTRAINT);
    c->actor1 = a1, c->actor2 = ENTITY_NONE, c->type = type;
    g_c[k] = e;
    return c;
}

void phys_joints_scene(Editor* ed) {
    World* w = &ed->world;
    Entity fl = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->location[1] = Y, ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    CStaticMesh* fm = ecs_get(w, fl, COMP_STATIC_MESH);
    fm->mesh = SHAPE_PLANE, fm->base_color[0] = fm->base_color[1] = fm->base_color[2] = 0.45f, fm->roughness = 0.7f;
    Entity sun = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    ((CTransform*)ecs_get(w, sun, COMP_TRANSFORM))->rotation[0] = -50.0f;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->type = LIGHT_DIRECTIONAL;
    const float zero[3] = { 0, 0, 0 };
    // hinge: a door around a vertical axis (the constraint's X up: pitch 90), limit 45, motor 60 deg/s
    g_b[J_HINGE] = jbody(ed, (float[3]){ -1000, Y + 50, 110 }, (float[3]){ 0.1f, 1.0f, 2.0f }, 20.0f, true);
    CConstraint* c = jjoint(ed, J_HINGE, (float[3]){ -1000, Y, 110 }, (float[3]){ 90, 0, 0 }, g_b[J_HINGE], CONSTRAINT_HINGE);
    if (c) c->limit = 45.0f, c->motor = 1, c->motor_speed = 60.0f, c->motor_strength = 200000.0f;
    // wheel: a hinge around Y (yaw 90: X along Y), motor 180 deg/s
    g_b[J_WHEEL] = jbody(ed, (float[3]){ -700, Y, 150 }, (float[3]){ 0.6f, 0.1f, 0.6f }, 10.0f, false);
    c = jjoint(ed, J_WHEEL, (float[3]){ -700, Y, 150 }, (float[3]){ 0, 90, 0 }, g_b[J_WHEEL], CONSTRAINT_HINGE);
    if (c) c->motor = 1, c->motor_speed = 180.0f;
    // slider along X with its motor to the limit
    g_b[J_SLIDER] = jbody(ed, (float[3]){ -400, Y, 150 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f, true);
    c = jjoint(ed, J_SLIDER, (float[3]){ -400, Y, 150 }, zero, g_b[J_SLIDER], CONSTRAINT_PRISMATIC);
    if (c) c->limit = 100.0f, c->motor = 1, c->motor_speed = 80.0f;
    // cone: a pendulum under its anchor (the constraint's X down), thrown sideways
    g_b[J_CONE] = jbody(ed, (float[3]){ 0, Y, 200 }, (float[3]){ 0.2f, 0.2f, 0.2f }, 5.0f, true);
    c = jjoint(ed, J_CONE, (float[3]){ 0, Y, 300 }, (float[3]){ -90, 0, 0 }, g_b[J_CONE], CONSTRAINT_CONE);
    if (c) c->swing1_limit = c->swing2_limit = 30.0f, c->twist_limit = 20.0f;
    CRigidBody* rb = ecs_get(w, g_b[J_CONE], COMP_RIGID_BODY);
    if (rb) rb->velocity[1] = 600.0f, rb->velocity[0] = 200.0f;
    // drive: a 6DOF free on every linear axis, pulled to +100 cm along X
    g_b[J_DRIVE] = jbody(ed, (float[3]){ 300, Y, 150 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f, false);
    c = jjoint(ed, J_DRIVE, (float[3]){ 300, Y, 150 }, zero, g_b[J_DRIVE], CONSTRAINT_6DOF);
    if (c) {
        for (int k = 0; k < 3; k++) c->lin_motion[k] = CMOTION_FREE, c->ang_motion[k] = CMOTION_LOCKED;
        c->lin_drive = CDRIVE_POSITION, c->lin_target[0] = 100.0f, c->drive_hz = 2.0f, c->drive_damping = 1.0f;
    }
    // slerp: a 6DOF held at its anchor, turned by its angular drive to yaw 90
    g_b[J_SLERP] = jbody(ed, (float[3]){ 600, Y, 150 }, (float[3]){ 0.6f, 0.2f, 0.2f }, 10.0f, false);
    c = jjoint(ed, J_SLERP, (float[3]){ 600, Y, 150 }, zero, g_b[J_SLERP], CONSTRAINT_6DOF);
    if (c) {
        for (int k = 0; k < 3; k++) c->lin_motion[k] = CMOTION_LOCKED, c->ang_motion[k] = CMOTION_FREE;
        c->ang_drive = CDRIVE_POSITION, c->ang_target[1] = 90.0f, c->drive_hz = 2.0f, c->drive_damping = 1.0f;
    }
    // soft: hanging on a soft linear limit (X down), 30 cm, a 3 Hz spring past it
    g_b[J_SOFT] = jbody(ed, (float[3]){ 900, Y, 350 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 10.0f, true);
    c = jjoint(ed, J_SOFT, (float[3]){ 900, Y, 350 }, (float[3]){ -90, 0, 0 }, g_b[J_SOFT], CONSTRAINT_6DOF);
    if (c) {
        c->lin_motion[0] = CMOTION_LIMITED, c->lin_motion[1] = c->lin_motion[2] = CMOTION_LOCKED, c->lin_limit = 30.0f;
        for (int k = 0; k < 3; k++) c->ang_motion[k] = CMOTION_LOCKED;
        c->soft_limits = 1, c->limit_hz = 3.0f, c->limit_damping = 0.3f;
    }
    // break: 80 kg on a fixed joint that holds 50 (kg cm/s2: 50 * 980)
    g_b[J_BREAK] = jbody(ed, (float[3]){ 1200, Y, 300 }, (float[3]){ 0.3f, 0.3f, 0.3f }, 80.0f, true);
    c = jjoint(ed, J_BREAK, (float[3]){ 1200, Y, 330 }, zero, g_b[J_BREAK], CONSTRAINT_FIXED);
    if (c) c->break_force = 50.0f * 980.0f;
    for (int k = 0; k < J_COUNT; k++) {
        const CTransform* t = ecs_get(w, g_b[k], COMP_TRANSFORM);
        if (t) memcpy(g_p0[k], t->location, 12);
    }
    g_cone_max = g_soft_max = g_wheel_w = g_slider_off = 0.0f;
    ed->cam.pos[0] = 100.0f, ed->cam.pos[1] = Y - 1800.0f, ed->cam.pos[2] = 500.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -10.0f;
}

void phys_joints_tick(Editor* ed, float t) {
    World* w = &ed->world;
    const CTransform* ct = ecs_get(w, g_b[J_CONE], COMP_TRANSFORM);
    if (ct) { // the pendulum's angle from straight down
        float d[3] = { ct->location[0] - 0.0f, ct->location[1] - Y, ct->location[2] - 300.0f }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float a = l > 1.0f ? acosf(fminf(-d[2] / l, 1.0f)) * 57.29578f : 0.0f;
        if (t > 0.1f && a > g_cone_max) g_cone_max = a;
    }
    const CTransform* st = ecs_get(w, g_b[J_SOFT], COMP_TRANSFORM);
    if (st && g_p0[J_SOFT][2] - st->location[2] > g_soft_max) g_soft_max = g_p0[J_SOFT][2] - st->location[2];
    const CTransform* sl = ecs_get(w, g_b[J_SLIDER], COMP_TRANSFORM);
    if (sl) {
        float off = fabsf(sl->location[1] - Y) + fabsf(sl->location[2] - 150.0f);
        if (off > g_slider_off) g_slider_off = off;
    }
    const CRigidBody* wr = ecs_get(w, g_b[J_WHEEL], COMP_RIGID_BODY);
    if (wr && t > 1.5f) g_wheel_w = sqrtf(wr->angular_velocity[0] * wr->angular_velocity[0] + wr->angular_velocity[1] * wr->angular_velocity[1] +
                                          wr->angular_velocity[2] * wr->angular_velocity[2]) * 57.29578f;
}

bool phys_joints_check(Editor* ed) {
    World* w = &ed->world;
    const CTransform* t[J_COUNT];
    for (int k = 0; k < J_COUNT; k++)
        if (!(t[k] = ecs_get(w, g_b[k], COMP_TRANSFORM))) {
            printf("[RealEngine] FAIL: phys_joints: body %d is gone\n", k);
            return false;
        }
    const PhysicsStats* s = physics_stats();
    float door = remainderf(t[J_HINGE]->rotation[1], 360.0f), slide = t[J_SLIDER]->location[0] - g_p0[J_SLIDER][0];
    float drive = t[J_DRIVE]->location[0] - g_p0[J_DRIVE][0], yaw = remainderf(t[J_SLERP]->rotation[1], 360.0f);
    float soft = g_p0[J_SOFT][2] - t[J_SOFT]->location[2], fall = t[J_BREAK]->location[2];
    const CConstraint* bc = ecs_get(w, g_c[J_BREAK], COMP_CONSTRAINT);
    float df[3] = { t[J_DRIVE]->location[0] - g_p0[J_DRIVE][0] - 100.0f, t[J_DRIVE]->location[1] - Y, t[J_DRIVE]->location[2] - 150.0f };
    float drive_err = sqrtf(df[0] * df[0] + df[1] * df[1] + df[2] * df[2]);
    printf("[RealEngine] phys_joints: %u joints (%u broken), anchors drifted at most %.2f cm\n", s->joints, s->joints_broken, s->joint_error_max);
    struct { const char* what; float got; float lo, hi; } r[] = {
        { "hinge door at its 45 limit (deg)", fabsf(door), 40.0f, 47.0f },
        { "wheel motor 180 deg/s", g_wheel_w, 165.0f, 195.0f },
        { "slider at its 100 cm limit", slide, 95.0f, 102.0f },
        { "slider off its axis (cm)", g_slider_off, 0.0f, 2.0f },
        { "cone pendulum's widest (deg, limit 30)", g_cone_max, 10.0f, 33.0f },
        { "linear drive to 100 cm (error)", drive_err, 0.0f, 4.0f },
        { "angular drive to yaw 90 (deg)", fabsf(yaw), 85.0f, 95.0f },
        { "soft limit 30 cm: deepest", g_soft_max, 32.0f, 200.0f },
        { "soft limit: resting past it", soft, 30.5f, 120.0f },
        { "broken joint's box fell (z)", fall, -1.0f, 40.0f },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++) {
        bool good = r[i].got >= r[i].lo && r[i].got <= r[i].hi;
        printf("[RealEngine]   %-40s %8.2f (want %.1f..%.1f) %s\n", r[i].what, r[i].got, r[i].lo, r[i].hi, good ? "ok" : "FAIL");
        ok &= good;
    }
    (void)drive;
    if (!bc || !bc->broken) printf("[RealEngine]   the breakable joint held (force %.0f)\n", bc ? bc->force : 0.0f), ok = false;
    if (!ok) printf("[RealEngine] FAIL: phys_joints\n");
    return ok;
}
