// The demos' game modes (sugestões #29, #31, #34; notas-dev/demos.md): native gameplay, as Unreal's samples keep theirs
// in C++ Game Modes. A project whose Game Mode is "DemoFighting", "DemoHorror" or "DemoRPG" (the demo templates set it)
// plays it in Play in Editor and in the packaged game alike; everything else is the project's own Blueprints.
//   - Fighting: rounds, combos, block, dodge, roll, air attacks, an AI that fights back, the fight camera, the HUD;
//   - Horror: the power puzzle, notes, the stalker, the flashlight, the exit;
//   - RPG: the village's merchants and their shop, ~50 items, money, jobs, the quest log.
#pragma once
#include <stdbool.h>

#include "ecs.h"

void demo_play_begin(World* w, const char* game_mode); // play starts (project_begin_play)
void demo_play_tick(World* w, float dt);               // after the Blueprints (bp_tick)
void demo_play_end(void);
bool demo_play_view(World* w, float pos[3], float* yaw, float* pitch); // its camera (the fight's), when it has one
const char* demo_play_report(World* w);                // a line for the tests ("" when no demo plays)
bool demo_play_check(World* w);                        // the tests: false (and a FAIL line) when the mode did not do its job
