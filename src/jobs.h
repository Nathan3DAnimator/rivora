// The job system: systems declare the components they read and write; the schedule orders them in stages (a system
// goes after every earlier system it conflicts with: one writes what the other reads or writes) and runs each stage
// in parallel on the fixed workers (re_parallel_for: one per core, items taken by an atomic counter, no locks), split
// into blocks of rows of every matching archetype. Systems of one stage never touch the same data, so no locks there
// either; a stage ends before the next starts (the dependencies). Nothing is allocated while running.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define RE_MAX_SYSTEMS 64
#define RE_MAX_TASKS 8192
#define RE_TASK_ROWS 16384 // rows per task (fewer when the world is small: at least one task per archetype)

// One block of rows [begin, end) of one archetype that has the system's required components
typedef void (*SystemFn)(World* w, EcsArchetype* a, uint32_t begin, uint32_t end, float dt);

typedef struct SystemDesc {
    const char* name;
    uint64_t require; // the archetype must have these (COMP_BIT)
    uint64_t reads, writes;
    SystemFn fn;
} SystemDesc;

typedef struct SystemTask {
    uint16_t system, archetype;
    uint32_t begin, end;
} SystemTask;

typedef struct Schedule {
    SystemDesc systems[RE_MAX_SYSTEMS];
    int count;
    int stage_of[RE_MAX_SYSTEMS];
    int stage_count;
    // the run's state (the tasks of the stage being run)
    SystemTask tasks[RE_MAX_TASKS];
    int task_count;
    World* world;
    float dt;
    double stage_ms[RE_MAX_SYSTEMS]; // the last run's time per stage
} Schedule;

void schedule_init(Schedule* s);
// Adds a system after the ones added before it (the order breaks ties: a conflict means "after"). False when full.
bool schedule_add(Schedule* s, const SystemDesc* d);
// Runs every stage in order, each in parallel.
void schedule_run(Schedule* s, World* w, float dt);
