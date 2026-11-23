// Foliage (notas-dev/landscape.md): Unreal's Foliage mode, simpler. A Foliage actor (CFoliage, the level's Instanced
// Foliage Actor) holds the plants painted on the ground, apart from the landscape's mesh: up to FOLIAGE_TYPES kinds
// (a mesh, a density, sizes, the steepest slope, the wind), each instance a place, a turn and a size, kept in
// Content/Foliage/<name>.rfol and drawn as its child entities (CAttach kind ATTACH_FOLIAGE), static, swaying in the wind
// (mesh.vert INST_WIND). Edited by a brush (paint / erase, up to the type's density inside it) and Fill (a landscape
// covered at the density); an instance added or removed creates or destroys only its own entity.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

void foliage_init(const char* content_root);
// Every frame (editor and game): each Foliage actor's instances read (once) and their entities made; a type's look
// changed: its entities follow. Also moves an older level's landscape plants into a Foliage actor (made if none)
void foliage_update(World* w);
// The brush (the editor): the instances of the painted types inside the circle (cm) brought up to their density
// (times density_scale), or erased; the ground under each one: the landscapes, else the plane of at
void foliage_paint(World* w, Entity e, const float at[3], float radius, float density_scale, bool erase, float dt);
void foliage_stroke_end(World* w, Entity e); // the file written
// Fill: type t (-1: every painted type) at its density over every landscape, in place of what it had; Clear: removed
uint32_t foliage_fill(World* w, Entity e, int t);
void foliage_clear(World* w, Entity e, int t);
uint32_t foliage_count(Entity e, int t); // its instances (of type t, -1 all)
// Type removed is gone (its instances cleared first): the instances of the types after it move down one
void foliage_retype(World* w, Entity e, int removed);
typedef struct FoliageStats { uint32_t actors, instances; double edit_ms, build_ms; } FoliageStats;
const FoliageStats* foliage_stats(void);
