// The horror kit's tests (terror_test.c, notas-dev/terror.md): --scene terror_filters / terror_exposure / terror_channels --bench N
#pragma once
#include <stdbool.h>

typedef struct Editor Editor;
struct VkCtx;

bool terror_test_scene(Editor* ed, const char* name);          // false: not one of these scenes
void terror_test_tick(Editor* ed, struct VkCtx* vk, float dt); // each frame (the viewport read back, the checks)
bool terror_test_check(Editor* ed);                            // after the bench: PASS / FAIL printed
