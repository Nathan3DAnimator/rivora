// The PCG's tests (Q15, pcg_test.c, notas-dev/pcg.md): --pcg-test (pcg_selftest, pcg.h), --scene pcg_forest / pcg_stream
#pragma once
#include <stdbool.h>

typedef struct Editor Editor;

bool pcg_test_scene(Editor* ed, const char* name); // false: not one of these scenes
void pcg_test_tick(Editor* ed, float dt);          // each frame (the phases: editor, PIE, seeds; the player carried)
bool pcg_test_check(Editor* ed);                   // after the bench: PASS / FAIL printed
