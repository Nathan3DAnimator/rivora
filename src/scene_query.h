// (2026-11-19) Scene queries: Unreal's traces (Line / Sphere / Box / Capsule, single and multi, by channel and for
// objects) and batches of them in parallel, against the play world: the actors' collision boxes, the Characters'
// capsules and the landscapes (notas-dev/traces.md).
//
// The world is copied once a frame (the first query of the frame, on the main thread) into primitives - a core box and
// a radius: a box (radius 0) or a capsule (a vertical segment and its radius) - under a BVH of 8 children a node,
// tested 8 at a time with AVX2 (phys_simd.h; plain loops elsewhere), built by binned SAH when the primitives' count
// changes and refitted when something moves. Every shape is exact: the query's shape summed with each primitive
// (Minkowski) is again a core box and a radius, and the segment meets that rounded box exactly (its faces by slabs,
// its edges and corners by capsules). The landscapes are walked tile by tile (landscape_raycast).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef enum SqShape { SQ_LINE = 0, SQ_SPHERE, SQ_BOX, SQ_CAPSULE, SQ_SHAPE_COUNT } SqShape;
// what a query sees (Unreal's object types); 0 is all of them
enum { SQ_OBJ_STATIC = 1u, SQ_OBJ_DYNAMIC = 2u, SQ_OBJ_PAWN = 4u, SQ_OBJ_LANDSCAPE = 8u, SQ_OBJ_ALL = 15u };

typedef struct SqQuery {
    float start[3], end[3];
    float half[3];         // SQ_BOX: its half size (axis aligned); SQ_CAPSULE: half[2] its half height (Unreal's: radius included)
    float radius;          // SQ_SPHERE, SQ_CAPSULE
    uint8_t shape;         // SqShape
    uint8_t objects;       // SQ_OBJ_* (0: all)
    uint16_t nignore;
    const Entity* ignore;  // actors left out (Ignore Self, Actors to Ignore)
    uint8_t cplx;          // Trace Complex: an actor's own mesh by its triangles (the line exact; the shapes by conservative
                           // advancement with GJK), not its collision box
    uint8_t pad[7];
} SqQuery;

typedef struct SqHit {
    float location[3];      // the shape's center where it stopped (Unreal's Location; the trace's end when nothing)
    float impact_point[3];  // the point touched on what was hit
    float normal[3];        // the swept shape's normal there (Unreal's Normal)
    float impact_normal[3]; // the surface's (Unreal's Impact Normal)
    float distance, time;   // from the start (cm), 0..1 of the way
    Entity actor;           // ENTITY_NONE: a landscape without an actor, or nothing
    uint8_t blocking;       // something was hit
    uint8_t start_penetrating; // the shape started inside it (time 0)
    uint8_t object;         // SQ_OBJ_* of what was hit
    uint8_t pad;
} SqHit;

typedef struct SqStats {
    uint32_t prims, nodes, boxes, pawns, landscapes;
    uint32_t rebuilds, refits;          // since play began
    uint32_t queries_frame;             // last frame's queries (single, multi and the batches' items)
    uint32_t batches_frame;
    double build_ms, refit_ms;          // the last ones
    double query_ms_frame;              // last frame's time inside the queries (the batches: wall time)
} SqStats;

// The nearest hit along the way (false: none; *hit then holds the trace's end and blocking 0)
bool sq_trace(World* w, const SqQuery* q, SqHit* hit);
// Every actor hit along the way, nearest first (one hit each), at most cap; the count
uint32_t sq_trace_multi(World* w, const SqQuery* q, SqHit* hits, uint32_t cap);
// n queries at once on the workers (one per core): hits[i] for qs[i]. The way to run hundreds of thousands a frame
void sq_trace_batch(World* w, const SqQuery* qs, uint32_t n, SqHit* hits);
// ... and drawn this frame (Draw Debug Type For One Frame): each worker writes its traces' records (debug_draw.h DdTrace,
// expanded on the GPU), nothing laid out after. colors: dd_rgb565(trace) << 16 | dd_rgb565(hit)
struct DdTrace;
void sq_trace_batch_drawn(World* w, const SqQuery* qs, uint32_t n, SqHit* hits, uint32_t colors);
// (2026-11-19) Async: the batch runs on background threads while the frame goes on (its draw, the GPU), its results the
// next frame, as Unreal's Async Line Trace. sq_async_begin gives room for n queries (filled by the caller, any thread
// it likes) and a handle; sq_async_kick starts them (draw_colors as sq_trace_batch_drawn's; 0: not drawn). The next
// frame (after sq_async_join) sq_async_result has them; two frames later they are gone. 64 a frame at most
SqQuery* sq_async_begin(World* w, uint32_t n, uint32_t* handle);
void sq_async_kick(uint32_t handle, uint32_t draw_colors);
bool sq_async_result(uint32_t handle, const SqHit** hits, uint32_t* n); // false: not ready yet, or gone
void sq_async_join(void); // the main loop, after dd_frame_begin: last frame's batches finished, their lines into this frame
// The frame's world in the BVH (the queries call it; the main thread only). A frame's later calls cost nothing
void sq_sync(World* w);
void sq_reset(void); // play ended: the world's copy dropped (its memory kept)
void sq_frame_end(void); // the frame's counts into the stats (the main loop)
SqStats sq_stats(void);
void sq_register_debug(void); // the Debug Viewer's list (Scene Queries)
int sq_selftest(void);         // --sq-test: the shapes against a brute force, the BVH against no BVH, the batch's speed
