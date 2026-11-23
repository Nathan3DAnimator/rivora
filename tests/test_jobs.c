// The job system: stages from the read/write sets, the order kept between dependent systems, the parallel run equal
// to the serial one, and the speed-up on 1M entities.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jobs.h"
#include "platform.h"
#include "systems.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void set_x(World* w, EcsArchetype* a, uint32_t b, uint32_t e, float dt) {
    (void)w; (void)dt;
    CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
    for (uint32_t i = b; i < e; i++) t[i].location[0] = 1.0f;
}
static void copy_x(World* w, EcsArchetype* a, uint32_t b, uint32_t e, float dt) {
    (void)w; (void)dt;
    const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
    CRotating* r = ecs_arch_column(a, COMP_ROTATING);
    for (uint32_t i = b; i < e; i++) r[i].rate[0] = t[i].location[0] + 1.0f;
}
static void nop(World* w, EcsArchetype* a, uint32_t b, uint32_t e, float dt) { (void)w; (void)a; (void)b; (void)e; (void)dt; }

static Schedule s; // big: not on the stack

int main(void) {
    const uint64_t T = COMP_BIT(COMP_TRANSFORM), R = COMP_BIT(COMP_ROTATING), L = COMP_BIT(COMP_LIGHT), N = COMP_BIT(COMP_NAME);
    // Stages: A writes T; B reads T, writes R (after A); C reads N (with A); D writes N (after C); E reads L (stage 0)
    schedule_init(&s);
    schedule_add(&s, &(SystemDesc){ "A", T, 0, T, set_x });
    schedule_add(&s, &(SystemDesc){ "B", T | R, T, R, copy_x });
    schedule_add(&s, &(SystemDesc){ "C", N, N, 0, nop });
    schedule_add(&s, &(SystemDesc){ "D", N, 0, N, nop });
    schedule_add(&s, &(SystemDesc){ "E", L, L, 0, nop });
    CHECK(s.stage_of[0] == 0 && s.stage_of[1] == 1 && s.stage_of[2] == 0 && s.stage_of[3] == 1 && s.stage_of[4] == 0);
    CHECK(s.stage_count == 2);

    // The order between dependent systems, over 300k entities in two archetypes
    World w;
    ecs_init(&w);
    for (int i = 0; i < 200000; i++) ecs_create(&w, T | R);
    for (int i = 0; i < 100000; i++) ecs_create(&w, T | R | N);
    schedule_run(&s, &w, 0.016f);
    int bad = 0;
    for (uint32_t a = 0; a < w.archetype_count; a++) {
        const EcsArchetype* arch = &w.archetypes[a];
        const CRotating* r = ecs_arch_column(arch, COMP_ROTATING);
        for (uint32_t i = 0; i < arch->count; i++) bad += r[i].rate[0] != 2.0f;
    }
    CHECK(bad == 0);
    ecs_shutdown(&w);

    // Rotating Movement: the parallel schedule equals the serial function; and how much faster it is
    World p, q;
    ecs_init(&p);
    ecs_init(&q);
    for (int i = 0; i < 1000000; i++) {
        Entity a = ecs_create(&p, T | R), b = ecs_create(&q, T | R);
        float rate = (float)(i % 360) - 180.0f;
        ((CRotating*)ecs_get(&p, a, COMP_ROTATING))->rate[1] = rate;
        ((CRotating*)ecs_get(&q, b, COMP_ROTATING))->rate[1] = rate;
    }
    systems_update(&p, 0.0f); // the workers start
    double t0 = re_time_seconds();
    for (int f = 0; f < 10; f++) system_rotating(&q, 0.016f);
    double t1 = re_time_seconds();
    for (int f = 0; f < 10; f++) systems_update(&p, 0.016f);
    double t2 = re_time_seconds();
    const CTransform* tp = ecs_arch_column(&p.archetypes[0], COMP_TRANSFORM);
    const CTransform* tq = ecs_arch_column(&q.archetypes[0], COMP_TRANSFORM);
    CHECK(memcmp(tp, tq, sizeof(CTransform) * 1000000) == 0);
    printf("jobs: rotating 1M serial %.3f ms, parallel %.3f ms (%d threads)\n", (t1 - t0) * 100.0, (t2 - t1) * 100.0,
           re_pool_threads());
    ecs_shutdown(&p);
    ecs_shutdown(&q);
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
