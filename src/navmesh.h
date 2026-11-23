// The NavMesh (notas-dev/ia.md "NavMesh"): Unreal's navigation (Recast / Detour), written here from the same ideas.
//   * Built in tiles (64 cells of 15 cm, a coarser cell on big levels) off the main thread (async.h), from what blocks the
//     Character: the meshes' boxes and the Collision components (bp_vm.c's), the Landscape as triangles. Per tile:
//     voxels -> the agent's filters (head room, step, ledges, slope) -> the walkable spans eroded by the agent's radius ->
//     monotone regions -> contours -> convex polygons (up to 6 sides) with a height grid of their own (the detail).
//   * Where: inside the Nav Mesh Bounds Volumes; without one, the whole level (a huge one: around the AI agents).
//   * A Static or Stationary actor that moves, appears or goes rebuilds only its tiles (Movable ones too in the editor).
//   * Queries: A* over the polygons, then the funnel (string pulling) for the straight path; a point projected onto the
//     mesh; a random reachable point in a radius. Nav Links (two points, one or both ways) jump gaps and drop off edges;
//     Nav Modifier Volumes give an area its cost (or take it out).
// The AI uses it in place of the grid (ai.c) once a tile is in; the grid stays as the fallback.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

// The path's points (navmesh_path): a point where a Nav Link starts (the next point is where it lands)
enum { NAV_PT_LINK = 1u << 0, NAV_PT_LINK_JUMP = 1u << 1 }; // (JUMP: a gap or a rise; a link that only drops is walked off)

// The areas (Nav Modifier Volume): what each costs to cross (Unreal's NavArea classes)
enum { NAV_AREA_NULL = 0, NAV_AREA_DEFAULT, NAV_AREA_OBSTACLE, NAV_AREA_WATER, NAV_AREA_COUNT };
extern const char* const g_nav_area_names[NAV_AREA_COUNT];
extern const float g_nav_area_cost[NAV_AREA_COUNT];

// Each frame (editor and game; main thread): the level's changes into dirty tiles, the builds started and put in place.
// playing: only Static / Stationary actors rebuild (a Movable one counts where it was when play started)
void navmesh_update(World* w, bool playing, double now);
void navmesh_rebuild(World* w);            // every tile again (Build > Build Paths)
void navmesh_set_wanted(bool on);          // the editor's P view: build even with no agent nor volume
void navmesh_reset(void);                  // a new level / project: everything forgotten
bool navmesh_ready(void);                  // a tile in: the AI uses the NavMesh (else the grid)
bool navmesh_building(void);
void navmesh_wait(World* w, double max_s); // the tests: until nothing is left to build

bool navmesh_path(const float from[3], const float to[3], float (*out)[3], uint8_t* flags, uint32_t cap, uint32_t* n); // n points after `from`
bool navmesh_project(const float p[3], const float extent[3], float out[3]); // the nearest point on the mesh in the box
bool navmesh_random_point(const float around[3], float radius, float out[3]); // reachable from around, within radius
float navmesh_path_length(const float from[3], const float to[3]);              // < 0: no path

typedef struct NavMeshStats {
    uint32_t tiles, polys, verts, links, offmesh, offmesh_linked;
    uint32_t builds, tiles_built, pending; // batches done, tiles built in all, tiles waiting
    double last_batch_ms, last_tile_ms, max_tile_ms, total_build_ms; // the worker's (a tile: voxels to polygons)
    double apply_ms;                       // the main thread's: putting a batch in place (links)
    double gather_ms;                      // the main thread's: the tiles' inputs
    uint32_t queries;                      // paths, projections, random points (all time)
    double query_ms_last, query_ms_max, query_us_avg;
    float cell, tile_size;                 // cm
    float agent_radius, agent_height, agent_climb;
    uint32_t area_mode;                    // 0 none, 1 Nav Mesh Bounds Volumes, 2 the level, 3 around the agents
} NavMeshStats;
const NavMeshStats* navmesh_stats(void);

// The editor's view (P): the polygons near a point; fn gets each one's corners (with their height), its area, its
// tile's parity (two shades), and the Nav Links (both ends, linked or not, one way or both)
typedef void (*NavPolyFn)(void* user, const float (*pts)[3], int n, int area, int parity);
typedef void (*NavLinkFn)(void* user, const float a[3], const float b[3], bool both_ways, bool linked);
void navmesh_debug(const float center[3], float radius, NavPolyFn poly, NavLinkFn link, void* user);
// The tiles' edges near a point (the borders: thin lines), for the view
typedef void (*NavEdgeFn)(void* user, const float a[3], const float b[3], bool portal);
void navmesh_debug_edges(const float center[3], float radius, NavEdgeFn fn, void* user);

void navmesh_register_debug(void); // the Debug Viewer's list (debug_viewer.c)
