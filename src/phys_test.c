// The rigid bodies' tests (Q8, notas-dev/fisica.md "Corpos rígidos profissionais"): --scene phys_stack / phys_ccd /
// phys_joints / phys_heavy with --play --bench N. Each scene is made here (no names: 20 thousand unique names would be
// the editor's cost, not the physics'), ticked each frame (phys_test_tick: what happens during the run, what is
// recorded) and checked at the end (phys_test_check: PASS / FAIL lines).
//   phys_stack:  a pyramid of 1035 boxes (45 at the base, 50 cm each), sleeping off: it stands still, nothing trembles
//   phys_ccd:    bullets (2 cm balls and 10 cm boxes, 50 to 1000 m/s) at a wall 2 cm thick and a heavy block: none passes
//   phys_joints: each joint (hinge, slider, cone/twist, 6DOF) with its limit, motor, spring and break
//   phys_heavy:  20 thousand bodies (boxes, balls, capsules, cylinders) poured into an arena, two explosions: the frame
//                rate and the step's phases (broadphase, narrowphase, solver)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "phys_test.h"
#include "physics.h"
#include "project.h"
#include "render.h"

#define STACK_BASE 45
#define STACK_MAX 1100
#define BULLETS 8

static struct {
    int scene; // 1 stack, 2 ccd, 3 joints, 4 heavy
    bool awake; // phys_stack_awake: sleeping off, 6 substeps (the strict one: awake, nothing may creep)
    Entity ent[STACK_MAX];
    float p0[STACK_MAX][3], p1[STACK_MAX][3]; // built; settled (1 s in: the contacts' springs took the weight)
    bool settled;
    uint32_t n;
    float max_drift, max_speed_late; // the stack: the farthest a box moved; the fastest in the last second
    int first_moved;
    uint32_t worst;
    float worst_t, worst_d[3];
    // ccd
    Entity bullet[BULLETS];
    float bullet_speed[BULLETS], bullet_wall[BULLETS]; // the speed it was fired at; the face it must stay in front of
    float bullet_worst[BULLETS];                        // its farthest x
    Entity block;
    bool fired;
    // heavy
    uint32_t booms, most_awake;
    double frame_ms_sum;
    uint32_t frames;
    // joints
    Entity jb[16], jc[16];
    float jv[16][4]; // per joint: what was measured
} T;

static float vlen(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static Entity mesh(World* w, int shape, const float at[3], const float scale[3], const float color[3], bool body) {
    Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | (body ? COMP_BIT(COMP_RIGID_BODY) : 0));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12), memcpy(t->scale, scale, 12);
    t->rotation[0] = t->rotation[1] = t->rotation[2] = 0.0f;
    t->flags = body ? MOBILITY_MOVABLE : MOBILITY_STATIC;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape;
    memcpy(m->base_color, color, 12);
    m->roughness = 0.55f;
    m->cast_shadow = 1;
    return e;
}

static void floor_and_light(Editor* ed, float size_m) {
    World* w = &ed->world;
    mesh(w, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, (float[3]){ size_m, size_m, 1 }, (float[3]){ 0.45f, 0.45f, 0.47f }, false);
    Entity sun = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    CTransform* st = ecs_get(w, sun, COMP_TRANSFORM);
    st->location[2] = 500.0f, st->rotation[0] = -50.0f, st->rotation[1] = 35.0f;
    CLight* l = ecs_get(w, sun, COMP_LIGHT);
    l->type = LIGHT_DIRECTIONAL;
    Entity sky = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    ((CTransform*)ecs_get(w, sky, COMP_TRANSFORM))->location[2] = 450.0f;
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->type = LIGHT_SKY;
}

static void stack_scene(Editor* ed) {
    World* w = &ed->world;
    floor_and_light(ed, 60.0f);
    const float S = 50.0f, gap = 1.0f; // (a centimeter apart side by side: they touch only the rows below and above)
    for (int row = 0; row < STACK_BASE; row++)
        for (int i = 0; i < STACK_BASE - row && T.n < STACK_MAX; i++) {
            float x = ((float)i - (float)(STACK_BASE - row - 1) * 0.5f) * (S + gap), z = S * 0.5f + (float)row * S;
            float c[3] = { 0.55f + 0.3f * (float)(row % 3) / 2.0f, 0.38f + 0.2f * (float)(i % 4) / 3.0f, 0.22f };
            Entity e = mesh(w, SHAPE_CUBE, (float[3]){ x, 0, z }, (float[3]){ 0.5f, 0.5f, 0.5f }, c, true);
            CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
            rb->mass = 10.0f, rb->friction = 0.6f, rb->restitution = 0.0f;
            T.ent[T.n] = e;
            T.p0[T.n][0] = x, T.p0[T.n][1] = 0, T.p0[T.n][2] = z;
            T.n++;
        }
    if (T.awake) { // (asleep, a box can't tremble: the strict test is that it doesn't even awake)
        physics_set_sleep_enabled(false);
        project()->solver_substeps = 6;
    }
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -3400.0f, ed->cam.pos[2] = 1150.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -2.0f;
}

static void ccd_scene(Editor* ed) {
    World* w = &ed->world;
    floor_and_light(ed, 40.0f);
    // the wall: 2 cm thick, 4 m tall, across the bullets' lanes at x = 1000 (its face toward them at 999)
    mesh(w, SHAPE_CUBE, (float[3]){ 1000, 0, 200 }, (float[3]){ 0.02f, 6.0f, 4.0f }, (float[3]){ 0.7f, 0.7f, 0.72f }, false);
    // a heavy block (a body, 1 m, 2000 kg) in the last lanes' way, before the wall
    T.block = mesh(w, SHAPE_CUBE, (float[3]){ 600, 200, 50 }, (float[3]){ 1, 1, 1 }, (float[3]){ 0.3f, 0.35f, 0.5f }, true);
    ((CRigidBody*)ecs_get(w, T.block, COMP_RIGID_BODY))->mass = 2000.0f;
    static const float speed[BULLETS] = { 5000, 20000, 50000, 100000, 20000, 50000, 50000, 100000 }; // cm/s
    for (int i = 0; i < BULLETS; i++) {
        bool box = i == 4 || i == 5, to_block = i >= 6;
        float y = to_block ? 160.0f + 60.0f * (float)(i - 6) : -250.0f + 70.0f * (float)i, z = to_block ? 50.0f : 150.0f;
        float s = box ? 0.1f : 0.04f;
        T.bullet[i] = mesh(w, box ? SHAPE_CUBE : SHAPE_SPHERE, (float[3]){ 0, y, z }, (float[3]){ s, s, s }, (float[3]){ 0.9f, 0.6f, 0.1f }, true);
        CRigidBody* rb = ecs_get(w, T.bullet[i], COMP_RIGID_BODY);
        rb->mass = box ? 0.5f : 0.01f, rb->enable_gravity = 0, rb->restitution = 0.0f;
        rb->linear_damping = 0.0f;
        physics_set_ccd(w, T.bullet[i], true);
        T.bullet_speed[i] = speed[i];
        T.bullet_wall[i] = to_block ? 550.0f : 999.0f;
        T.bullet_worst[i] = 0.0f;
    }
    ed->cam.pos[0] = 500.0f, ed->cam.pos[1] = -1300.0f, ed->cam.pos[2] = 450.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -15.0f;
}

// 20 thousand bodies in an arena 60 m across (walls 2 m tall), poured from 8 layers: boxes, balls, capsules, cylinders
static void heavy_scene(Editor* ed, uint32_t count) {
    World* w = &ed->world;
    floor_and_light(ed, 90.0f);
    const float H = 3000.0f;
    float wc[3] = { 0.5f, 0.5f, 0.55f };
    mesh(w, SHAPE_CUBE, (float[3]){ H + 50, 0, 100 }, (float[3]){ 1, 62, 2 }, wc, false);
    mesh(w, SHAPE_CUBE, (float[3]){ -H - 50, 0, 100 }, (float[3]){ 1, 62, 2 }, wc, false);
    mesh(w, SHAPE_CUBE, (float[3]){ 0, H + 50, 100 }, (float[3]){ 60, 1, 2 }, wc, false);
    mesh(w, SHAPE_CUBE, (float[3]){ 0, -H - 50, 100 }, (float[3]){ 60, 1, 2 }, wc, false);
    uint32_t seed = 7u;
#define RND() ((seed = seed * 1664525u + 1013904223u) >> 8) * (1.0f / 16777216.0f)
    const uint32_t side = 50, layer = side * side;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t L = i / layer, k = i % layer;
        float x = ((float)(k % side) - (side - 1) * 0.5f) * 110.0f + (RND() - 0.5f) * 30.0f;
        float y = ((float)(k / side) - (side - 1) * 0.5f) * 110.0f + (RND() - 0.5f) * 30.0f;
        float z = 80.0f + (float)L * 110.0f;
        float s = 0.3f + RND() * 0.3f, r = RND();
        int shape = r < 0.55f ? SHAPE_CUBE : r < 0.8f ? SHAPE_SPHERE : SHAPE_CYLINDER;
        bool capsule = shape == SHAPE_CYLINDER && r < 0.9f;
        float sc[3] = { s, s * (shape == SHAPE_CUBE ? 0.6f + RND() * 0.8f : 1.0f), capsule ? s * 2.2f : s };
        float c[3] = { 0.2f + 0.7f * RND(), 0.2f + 0.6f * RND(), 0.2f + 0.7f * RND() };
        Entity e = mesh(w, shape, (float[3]){ x, y, z }, sc, c, true);
        if (e == ENTITY_NONE) break;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->rotation[0] = RND() * 90.0f, t->rotation[1] = RND() * 360.0f, t->rotation[2] = RND() * 90.0f;
        CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
        if (capsule) rb->collision_shape = COLSHAPE_CAPSULE;
    }
#undef RND
    ed->cam.pos[0] = -4200.0f, ed->cam.pos[1] = -4200.0f, ed->cam.pos[2] = 2600.0f;
    ed->cam.yaw = 45.0f, ed->cam.pitch = -27.0f;
}

// (2026-11-19, the Q8 leftover) 20 thousand bodies as a game has them: a warehouse of 2500 pallets (2 x 2 x 2 crates or
// barrels of 50 cm, a meter cube), 10 cm apart, asleep; an explosion every 1.5 s somewhere: the pallets there fly and
// hit their neighbors, the rest sleeps. The frame rate with 20 thousand Movable bodies and the active part simulated
static void crates_scene(Editor* ed, uint32_t count) {
    World* w = &ed->world;
    floor_and_light(ed, 90.0f);
    uint32_t seed = 11u;
#define RND() ((seed = seed * 1664525u + 1013904223u) >> 8) * (1.0f / 16777216.0f)
    const uint32_t side = 50, high = 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t col = i / high, k = i % high;
        if (col >= side * side) break;
        float x = ((float)(col % side) - (side - 1) * 0.5f) * 110.0f + ((k & 1) ? 25.0f : -25.0f);
        float y = ((float)(col / side) - (side - 1) * 0.5f) * 110.0f + ((k & 2) ? 25.0f : -25.0f);
        bool barrel = (col * 2654435761u >> 28) < 4u; // (a quarter of the pallets: barrels)
        float z = (k & 4) ? 75.0f : 25.0f;
        float c[3] = { barrel ? 0.25f : 0.55f + 0.2f * RND(), barrel ? 0.32f : 0.4f + 0.1f * RND(), barrel ? 0.5f : 0.25f };
        Entity e = mesh(w, barrel ? SHAPE_CYLINDER : SHAPE_CUBE, (float[3]){ x, y, z }, (float[3]){ 0.5f, 0.5f, 0.5f }, c, true);
        if (e == ENTITY_NONE) break;
        CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
        rb->mass = barrel ? 30.0f : 12.0f, rb->friction = 0.6f, rb->restitution = 0.0f;
        rb->sleeping = 1; // (settled: it wakes when something hits it)
    }
#undef RND
    ed->cam.pos[0] = -4200.0f, ed->cam.pos[1] = -4200.0f, ed->cam.pos[2] = 2600.0f;
    ed->cam.yaw = 45.0f, ed->cam.pitch = -27.0f;
}

bool phys_test_scene(Editor* ed, const char* name) {
    memset(&T, 0, sizeof T);
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    physics_set_sleep_enabled(true);
    if (!strncmp(name, "phys_stack", 10)) T.scene = 1, T.awake = !strcmp(name, "phys_stack_awake"), stack_scene(ed);
    else if (!strcmp(name, "phys_ccd")) T.scene = 2, ccd_scene(ed);
    else if (!strcmp(name, "phys_joints")) T.scene = 3, phys_joints_scene(ed);
    else if (!strncmp(name, "phys_heavy", 10)) {
        int n = atoi(name + 10);
        T.scene = 4, heavy_scene(ed, n > 0 ? (uint32_t)n : 20000u);
    } else if (!strncmp(name, "phys_crates", 11)) {
        int n = atoi(name + 11);
        T.scene = 5, crates_scene(ed, n > 0 ? (uint32_t)n : 20000u);
    } else
        return false;
    ed->dirty = true;
    return true;
}

void phys_test_tick(Editor* ed, float dt) {
    World* w = &ed->world;
    const PhysicsStats* s = physics_stats();
    float t = s->play_time;
    if (T.scene == 1) {
        bool settle = !T.settled && t >= 1.0f;
        T.settled |= settle;
        for (uint32_t i = 0; i < T.n; i++) {
            const CTransform* tr = ecs_get(w, T.ent[i], COMP_TRANSFORM);
            const CRigidBody* rb = ecs_get(w, T.ent[i], COMP_RIGID_BODY);
            if (!tr || !rb) continue;
            float d[3] = { tr->location[0] - T.p0[i][0], tr->location[1] - T.p0[i][1], tr->location[2] - T.p0[i][2] };
            float l = vlen(d), v = vlen(rb->velocity);
            if (settle) memcpy(T.p1[i], tr->location, 12);
            if (T.settled) { // (the first second: the boxes settle on each other; from then on nothing may move)
                float e[3] = { tr->location[0] - T.p1[i][0], tr->location[1] - T.p1[i][1], tr->location[2] - T.p1[i][2] };
                if (vlen(e) > T.max_drift) T.max_drift = vlen(e), T.worst = i, T.worst_t = t, memcpy(T.worst_d, e, 12);
            }
            if (getenv("REALENGINE_PHYSICS_DEBUG") && l > 3.0f && !T.first_moved) {
                T.first_moved = 1;
                printf("[RealEngine]   phys_stack: box %u (z %.0f) moved first, %.1f cm at %.2f s (v %.1f; moved %.1f %.1f %.1f, rot %.1f %.1f %.1f)\n", i, T.p0[i][2], l, t, v, d[0], d[1], d[2],
                       tr->rotation[0], tr->rotation[1], tr->rotation[2]);
            }
            if (t > 3.0f && v > T.max_speed_late) T.max_speed_late = v;
        }
    } else if (T.scene == 2) {
        if (!T.fired && t > 0.25f) { // fired once the play runs (the block has settled)
            for (int i = 0; i < BULLETS; i++) {
                CRigidBody* rb = ecs_get(w, T.bullet[i], COMP_RIGID_BODY);
                if (rb) rb->velocity[0] = T.bullet_speed[i], rb->velocity[1] = rb->velocity[2] = 0.0f, rb->sleeping = 0;
            }
            T.fired = true;
        }
        for (int i = 0; i < BULLETS; i++) {
            const CTransform* tr = ecs_get(w, T.bullet[i], COMP_TRANSFORM);
            if (tr && tr->location[0] > T.bullet_worst[i]) T.bullet_worst[i] = tr->location[0];
        }
    } else if (T.scene == 3) {
        phys_joints_tick(ed, t);
    } else if (T.scene == 4) {
        // two explosions under the heap: the bodies there wake and fly again
        if ((T.booms == 0 && t > 4.0f) || (T.booms == 1 && t > 7.0f)) {
            float o[3] = { T.booms ? 1200.0f : -800.0f, T.booms ? -900.0f : 600.0f, -50.0f };
            physics_radial_impulse(w, o, 1800.0f, 1400.0f);
            T.booms++;
        }
        if (t > 0.5f) T.frame_ms_sum += dt * 1000.0f, T.frames++;
    } else if (T.scene == 5) {
        // an explosion every 1.5 s, each somewhere else in the warehouse (a 5 m blast at a tower's foot)
        if (t > 0.5f + 1.5f * (float)T.booms) {
            static const float at[8][2] = { { -1500, 900 }, { 1200, -1300 }, { 300, 1800 }, { -2100, -1700 }, { 2000, 600 }, { -400, -300 }, { 900, 2300 }, { -2400, 1600 } };
            float o[3] = { at[T.booms % 8][0], at[T.booms % 8][1], 20.0f };
            physics_radial_impulse(w, o, 500.0f, 1000.0f);
            T.booms++;
        }
        if (t > 0.5f) T.frame_ms_sum += dt * 1000.0f, T.frames++;
        if (s->awake > T.most_awake) T.most_awake = s->awake;
    }
}

bool phys_test_check(Editor* ed) {
    World* w = &ed->world;
    const PhysicsStats* s = physics_stats();
    bool ok = true;
    double step_ms = s->busy_steps ? s->busy_ms / s->busy_steps : 0.0;
    if (T.scene == 1) {
        float top_z = 0.0f;
        uint32_t fallen = 0;
        for (uint32_t i = 0; i < T.n; i++) {
            const CTransform* tr = ecs_get(w, T.ent[i], COMP_TRANSFORM);
            if (!tr) continue;
            if (i == T.n - 1) top_z = tr->location[2];
            if (fabsf(tr->location[2] - T.p0[i][2]) > 10.0f) fallen++;
        }
        float want = T.p0[T.n - 1][2];
        if (getenv("REALENGINE_PHYSICS_DEBUG"))
            printf("[RealEngine]   phys_stack: the farthest: box %u (built z %.0f, x %.0f), %.2f %.2f %.2f cm at %.1f s\n", T.worst, T.p0[T.worst][2],
                   T.p0[T.worst][0], T.worst_d[0], T.worst_d[1], T.worst_d[2], T.worst_t);
        printf("[RealEngine] phys_stack: %u boxes, the farthest moved after settling %.2f cm, the fastest after 3 s %.2f cm/s, the top at z %.1f (built %.1f), "
               "%u fallen; %.3f ms a step (%u contacts; broadphase %.3f, narrowphase %.3f, solver %.3f ms)\n",
               T.n, T.max_drift, T.max_speed_late, top_z, want, fallen, step_ms, s->contacts, s->busy_steps ? s->busy_broad_ms / s->busy_steps : 0.0,
               s->busy_steps ? s->busy_narrow_ms / s->busy_steps : 0.0, s->busy_steps ? s->busy_solve_ms / s->busy_steps : 0.0);
        uint32_t asleep = 0;
        for (uint32_t i = 0; i < T.n; i++) {
            const CRigidBody* rb = ecs_get(w, T.ent[i], COMP_RIGID_BODY);
            asleep += rb && rb->sleeping;
        }
        printf("[RealEngine] phys_stack: %s, %u solver substeps: %u of %u asleep at the end, sunk %.2f cm\n", T.awake ? "sleeping off" : "sleeping on",
               (unsigned)project()->solver_substeps, asleep, T.n, want - top_z);
        if (fallen || T.max_drift > (T.awake ? 0.5f : 1.0f) || T.max_speed_late > 1.0f || fabsf(top_z - want) > 5.0f ||
            (!T.awake && asleep < T.n))
            printf("[RealEngine] FAIL: phys_stack: the pyramid moved (it must stand still)\n"), ok = false;
    } else if (T.scene == 2) {
        for (int i = 0; i < BULLETS; i++) {
            const CTransform* tr = ecs_get(w, T.bullet[i], COMP_TRANSFORM);
            bool pass = tr && T.bullet_worst[i] < T.bullet_wall[i] + 1.0f;
            printf("[RealEngine]   bullet %d (%s, %.0f m/s): farthest x %.1f, the face at %.0f %s\n", i, i == 4 || i == 5 ? "box 10 cm" : "ball 2 cm",
                   T.bullet_speed[i] * 0.01f, T.bullet_worst[i], T.bullet_wall[i], pass ? "stopped" : "THROUGH");
            if (!pass) ok = false;
        }
        const CTransform* bt = ecs_get(w, T.block, COMP_TRANSFORM);
        printf("[RealEngine] phys_ccd: %d bullets, the block at x %.1f; %u swept (CCD) this play\n", BULLETS, bt ? bt->location[0] : 0.0f, s->ccd_sweeps);
        if (!ok) printf("[RealEngine] FAIL: phys_ccd: a bullet went through\n");
    } else if (T.scene == 3) {
        ok = phys_joints_check(ed);
    } else if (T.scene == 4 || T.scene == 5) {
        double fps = T.frames ? 1000.0 * T.frames / T.frame_ms_sum : 0.0;
        uint32_t below = 0, total = 0;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_RIGID_BODY)) || !a->count) continue;
            const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t i = 0; i < a->count; i++, total++)
                if (t[i].location[2] < -5.0f || fabsf(t[i].location[0]) > 3200.0f || fabsf(t[i].location[1]) > 3200.0f) below++;
        }
        uint32_t bs = s->busy_steps ? s->busy_steps : 1;
        printf("[RealEngine] %s: %u bodies (%u awake at the end), %.0f fps over the play (%.2f ms a frame); physics %.3f ms a frame on average "
               "(peak %.3f), %.3f ms a step busy: broadphase %.3f, narrowphase %.3f, solver %.3f, rest %.3f ms; %u contacts, %u islands, %u threads; "
               "%u outside the arena or through the floor\n",
               T.scene == 5 ? "phys_crates" : "phys_heavy", total, s->awake, fps, T.frames ? T.frame_ms_sum / T.frames : 0.0, s->play_frames ? s->ms_sum / s->play_frames : 0.0, s->ms_peak, step_ms,
               s->busy_broad_ms / bs, s->busy_narrow_ms / bs, s->busy_solve_ms / bs,
               step_ms - (s->busy_broad_ms + s->busy_narrow_ms + s->busy_solve_ms) / bs, s->contacts, s->islands, s->threads, below);
        for (int k = 0; k < 12; k++) printf("[RealEngine]   phase %-12s %.3f ms a step (busy), %.3f over all %u steps\n", physics_phase_name(k), s->phase_ms[k] / bs,
                                            s->all_steps ? s->all_phase_ms[k] / s->all_steps : 0.0, s->all_steps);
        printf("[RealEngine]   outside the steps (ms a frame): gather and sync %.3f, write back %.3f\n", s->play_frames ? s->frame_ms[0] / s->play_frames : 0.0,
               s->play_frames ? s->frame_ms[1] / s->play_frames : 0.0);
        printf("[RealEngine]   graph colors (most a step) %u, overflow (last step) %u contacts, %u solver stages a step; %u pairs (last step), %u GJK runs this play\n", s->colors, s->overflow,
               s->stages, s->pairs, s->gjk);
        if (s->solver_steps) {
            static const char* const sn[8] = { "pack", "integrate", "warm", "solve", "relax", "restitution", "store", "joints" };
            printf("[RealEngine]   solver stages (ms a step):");
            for (int k = 0; k < 8; k++) printf(" %s %.3f", sn[k], s->solver_ms[k] / s->solver_steps);
            printf("\n");
        }
        if (T.scene == 5) printf("[RealEngine] phys_crates: %u explosions, at most %u awake at once\n", T.booms, T.most_awake);
        if (below > total / 1000)
            printf("[RealEngine] FAIL: %s: %u bodies through the floor or out of the arena\n", T.scene == 5 ? "phys_crates" : "phys_heavy", below), ok = false;
    }
    physics_set_sleep_enabled(true);
    project()->solver_substeps = 4;
    return ok;
}
