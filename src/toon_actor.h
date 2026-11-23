// (Q11) The toon and the outline changed on an actor while the game runs (the Blueprint nodes; notas-dev/toon.md).
// A Static Mesh actor, or a Skeletal Mesh one (its parts): each mesh's material gets the actor's own dynamic instance,
// whose toon block is written at once (no bake). Names: toon.h toon_set_scalar / toon_set_color (pt or en)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

bool toon_actor_set_scalar(World* w, Entity e, const char* name, float v);
bool toon_actor_set_color(World* w, Entity e, const char* name, const float rgb[3]);
bool toon_actor_get_scalar(World* w, Entity e, const char* name, float* v);
// The actor's own outline (mode: toon.h OUTLINE_ACTOR_; color linear; width px at 1440p, 0: the material's)
bool toon_actor_set_outline(World* w, Entity e, int mode, const float rgb[3], float width);
// A light's toon mode (0 the material says, 1 Force Toon, 2 Never)
bool toon_light_set_mode(World* w, Entity e, int mode);
// CStaticMesh.outline packed / its color back (linear)
uint32_t toon_outline_pack(int mode, const float rgb[3]);
void toon_outline_color(uint32_t packed, float rgb[3]);
