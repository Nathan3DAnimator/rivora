// The rigid bodies' tests (phys_test.c, phys_joints_test.c; notas-dev/fisica.md): --scene phys_stack / phys_ccd /
// phys_joints / phys_heavy[N] --play --bench S
#pragma once
#include <stdbool.h>

typedef struct Editor Editor;

bool phys_test_scene(Editor* ed, const char* name); // false: not one of these scenes
void phys_test_tick(Editor* ed, float dt);          // each frame while playing
bool phys_test_check(Editor* ed);                   // after the bench: PASS / FAIL printed
// (phys_joints_test.c)
void phys_joints_scene(Editor* ed);
void phys_joints_tick(Editor* ed, float t);
bool phys_joints_check(Editor* ed);
