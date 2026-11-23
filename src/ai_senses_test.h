// (Q24) The AI's damage sense and local avoidance: the scene and its checks (ai_senses_test.c, notas-dev/ia.md)
#pragma once
#include <stdbool.h>

#include "editor.h"

bool ai_senses_test_scene(Editor* ed, const char* name); // ai_senses (false: not this one)
void ai_senses_test_tick(Editor* ed, float dt);          // each frame while playing: the hit, the walkers' orders, the closest pass
bool ai_senses_test_check(Editor* ed);                   // after the bench: PASS / FAIL printed
