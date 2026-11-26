// (2026-11-24) --scene firstframe: the materials' color before any bake and their textures' climb (mat_first_test.c)
#pragma once
#include <stdbool.h>

struct Editor;
struct VkCtx;
bool mat_first_scene(struct Editor* ed);
void mat_first_tick(struct VkCtx* vk); // every frame
bool mat_first_check(struct Editor* ed); // after the run: the first frame's shot read back
