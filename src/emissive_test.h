// (2026-11-23) Emissive materials lighting the scene: the scenes and their check (emissive_test.c, notas-dev/luz.md)
#pragma once
#include <stdbool.h>

#include "editor.h"

bool emissive_test_scene(Editor* ed, const char* name); // emit, emit_off (false: not one of these)
bool emissive_test_check(Editor* ed, const char* shot); // after the bench: PASS / FAIL printed (shot: --shot's PNG)
