// (Q21) The distant view's scenes and checks (farview_test.c, notas-dev/visao-distante.md)
#pragma once
#include <stdbool.h>

#include "editor.h"

struct VkCtx;
bool farview_test_scene(Editor* ed, const char* name);            // vista, vista_pop, vista_walk (false: not one of these)
bool farview_test_ready(Editor* ed);                              // the trees, the impostors and the far shadow in place (the bench waits)
void farview_test_frame(Editor* ed, struct VkCtx* vk, bool measuring); // each frame (the lookout found, the pop's shots)
bool farview_test_check(Editor* ed);                              // after the bench: PASS / FAIL printed
