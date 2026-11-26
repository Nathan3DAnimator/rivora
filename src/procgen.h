// Procedural worlds (notas-dev/procedural.md): a Procedural World actor (CProcWorld) makes the level around the player
// from a seed, the same every time:
//   - Terrain (Minecraft, Peak, open worlds): columns of blocks in chunks streamed around the player, finer near and
//     coarser far (rings of detail out to the view distance), biomes (plains, forest, desert, snow, mountains, beaches,
//     lakes and sea under a Water Body), trees, ruins and towers, creatures by biome; the ground the Characters, the
//     AI, the vehicles and the ragdolls stand on is the same height function (landscape_height's hook).
//   - Facility (Lethal Company, Repo): rooms and corridors grown on a grid from an entrance (a tree, with some loops),
//     doors between them, lights, items and creatures more numerous the deeper the room.
//   - Long distances (100 km and more): the chunks' coordinates are integers and the noise is double precision; when
//     the player goes past the rebase distance the whole world is moved back toward 0 (Unreal's World Origin
//     Rebasing) so floats keep their centimeters; the systems that keep world positions are told (procgen_rebase_hook).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

enum { PROCGEN_TERRAIN = 0, PROCGEN_FACILITY, PROCGEN_MODE_COUNT };
extern const char* const g_procgen_mode_names[PROCGEN_MODE_COUNT];

enum { BIOME_OCEAN = 0, BIOME_BEACH, BIOME_PLAINS, BIOME_FOREST, BIOME_DESERT, BIOME_SNOW, BIOME_MOUNTAIN, BIOME_COUNT };
extern const char* const g_biome_names[BIOME_COUNT];

void procgen_init(const char* content_root);
// Every frame (editor and game): the chunks streamed around the focus (the player, or the editor's camera), the
// facility built when its numbers changed, the origin moved back when far. focus: cm (NULL: none)
void procgen_update(World* w, const float focus[3], bool playing);
void procgen_begin_play(World* w);
// Everything the generator made, gone (before a level is saved: the file keeps only the actor; made again next frame)
void procgen_purge(World* w);
void procgen_end_play(World* w);                 // (the play's chunks and creatures dropped; the editor's preview back)

// The terrain's height at (x, y) (cm, the world's current origin), the column's top under it; false: no world there
bool procgen_height(float x, float y, float* z);
// Its biome there (BIOME_)
int procgen_biome(float x, float y);
// The world's origin now: where (0, 0, 0) of the level is in the endless world (cm, double)
void procgen_origin(double out[3]);
// A system that keeps world positions of its own (vehicles, bodies, particles...) moved when the origin is: d (cm) is
// added to every position
typedef void (*ProcRebaseFn)(World* w, const float d[3]);
void procgen_rebase_hook(ProcRebaseFn fn);
// The editor's cameras move with it (editor.c)
void procgen_set_camera_hook(void (*fn)(const float d[3]));

typedef struct ProcStats {
    uint32_t chunks, columns, trees, structures, creatures, rooms, rebases;
    double ms, ms_peak;
    double origin[3];
} ProcStats;
const ProcStats* procgen_stats(void);
// (2026-11-25, item 19: the tests) the last facility's deepest room: its floor's middle (cm) and its side; false: none built
bool procgen_facility_deepest(float center[3], float* side);
int procgen_selftest(void); // the noise's determinism and range, the biomes, a facility's connectivity; 0 = ok
