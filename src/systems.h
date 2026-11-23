// The game's systems: functions over blocks of the archetypes' columns, run by the job system (jobs.h) every frame
// while playing.
#pragma once
#include "ecs.h"

struct Schedule;

void systems_update(World* w, float dt);
const struct Schedule* systems_schedule(void); // NULL before the first update
// Rotating Movement on the calling thread alone (the tests compare it with the parallel run)
void system_rotating(World* w, float dt);
