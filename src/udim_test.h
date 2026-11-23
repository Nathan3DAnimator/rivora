// (Q18) UDIM's scenes and checks (udim_test.c, notas-dev/udim.md)
#pragma once
#include <stdbool.h>

#include "editor.h"

bool udim_test_scene(Editor* ed, const char* name);               // udim, udim_seams, udim_stream (false: not one of these)
void udim_test_frame(Editor* ed, bool measuring, double now);     // each frame (the stream's camera flies)
bool udim_test_check(Editor* ed, const char* shot);               // after the bench: PASS / FAIL printed (shot: --shot's PNG)
