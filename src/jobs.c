#include "jobs.h"

#include <string.h>

#include "platform.h"

void schedule_init(Schedule* s) { memset(s, 0, sizeof *s); }

static bool conflicts(const SystemDesc* a, const SystemDesc* b) {
    return (a->writes & (b->reads | b->writes)) || (b->writes & a->reads);
}

bool schedule_add(Schedule* s, const SystemDesc* d) {
    if (s->count >= RE_MAX_SYSTEMS) return false;
    int i = s->count++;
    s->systems[i] = *d;
    // its stage: one past the latest earlier system it conflicts with
    int stage = 0;
    for (int k = 0; k < i; k++)
        if (conflicts(&s->systems[k], d) && s->stage_of[k] + 1 > stage) stage = s->stage_of[k] + 1;
    s->stage_of[i] = stage;
    if (stage + 1 > s->stage_count) s->stage_count = stage + 1;
    return true;
}

static void run_task(void* arg, int index) {
    Schedule* s = arg;
    const SystemTask* t = &s->tasks[index];
    s->systems[t->system].fn(s->world, &s->world->archetypes[t->archetype], t->begin, t->end, s->dt);
}

void schedule_run(Schedule* s, World* w, float dt) {
    s->world = w;
    s->dt = dt;
    for (int stage = 0; stage < s->stage_count; stage++) {
        double t0 = re_time_seconds();
        s->task_count = 0;
        // the stage's rows, cut in blocks (bigger blocks when there would be too many tasks)
        uint64_t rows = 0;
        for (int i = 0; i < s->count; i++) {
            if (s->stage_of[i] != stage) continue;
            for (uint32_t a = 0; a < w->archetype_count; a++)
                if ((w->archetypes[a].mask & s->systems[i].require) == s->systems[i].require) rows += w->archetypes[a].count;
        }
        uint32_t block = RE_TASK_ROWS;
        while (rows / block > RE_MAX_TASKS / 2) block *= 2;
        for (int i = 0; i < s->count; i++) {
            if (s->stage_of[i] != stage) continue;
            for (uint32_t a = 0; a < w->archetype_count; a++) {
                const EcsArchetype* arch = &w->archetypes[a];
                if ((arch->mask & s->systems[i].require) != s->systems[i].require || arch->count == 0) continue;
                for (uint32_t b = 0; b < arch->count && s->task_count < RE_MAX_TASKS; b += block) {
                    SystemTask* t = &s->tasks[s->task_count++];
                    t->system = (uint16_t)i;
                    t->archetype = (uint16_t)a;
                    t->begin = b;
                    t->end = b + block < arch->count ? b + block : arch->count;
                }
            }
        }
        re_parallel_for(s->task_count, run_task, s); // the caller works too; returns when all are done
        s->stage_ms[stage] = (re_time_seconds() - t0) * 1000.0;
    }
}
