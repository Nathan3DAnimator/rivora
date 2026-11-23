// Level prototyping (notas-dev/prototipagem.md): Unreal's blockout tools, simpler. A Prototype actor (CProto) is a
// shape made by its numbers (Box, Stairs, Ramp, Wall with an opening, Room, Corridor, Platform), built from boxes:
// its child entities (CAttach kind ATTACH_PROTO), the cube with the grid material (its size readable at a glance).
// The boxes are what the Characters collide with and the navigation walks on, so a blockout plays at once. Change a
// number in Details (or move / turn the actor) and it is built again.
#pragma once
#include "ecs.h"

void proto_init(const char* content_root); // (the grid material's folder)
void proto_update(World* w);               // every frame: the shapes whose numbers changed, built again
bool proto_spline_kind(uint32_t kind); // (2026-11-10) the shapes that can follow a spline (CProto.npoints): Wall, Corridor, Box, Ramp, Platform
extern const char* const g_proto_kind_names[];
