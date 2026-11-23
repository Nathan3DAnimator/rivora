#include "systems.h"

#include <math.h>

#include "jobs.h"

// Rotating Movement: Transform.rotation += Rotating.rate * dt, over one block of rows
static void rotating_block(World* w, EcsArchetype* a, uint32_t begin, uint32_t end, float dt) {
    (void)w;
    CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
    const CRotating* r = ecs_arch_column(a, COMP_ROTATING);
    for (uint32_t i = begin; i < end; i++)
        for (int k = 0; k < 3; k++) {
            float v = t[i].rotation[k] + r[i].rate[k] * dt;
            // kept in [-180, 180): selects, not floorf (vectorizes); a step past a full turn takes more frames
            v = v >= 180.0f ? v - 360.0f : v;
            t[i].rotation[k] = v < -180.0f ? v + 360.0f : v;
        }
}

void system_rotating(World* w, float dt) {
    const uint64_t need = COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ROTATING);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & need) == need) rotating_block(w, a, 0, a->count, dt);
    }
}

// The game's schedule: built once, run every frame while playing
static Schedule g_schedule;
static bool g_built;

const Schedule* systems_schedule(void) { return g_built ? &g_schedule : NULL; }

void systems_update(World* w, float dt) {
    if (!g_built) {
        schedule_init(&g_schedule);
        const uint64_t tr = COMP_BIT(COMP_TRANSFORM), rot = COMP_BIT(COMP_ROTATING);
        schedule_add(&g_schedule, &(SystemDesc){ "Rotating Movement", tr | rot, rot, tr, rotating_block });
        g_built = true;
    }
    schedule_run(&g_schedule, w, dt);
}
