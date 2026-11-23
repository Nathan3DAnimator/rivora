// Splines and cables (Q14, notas-dev/splines.md). The engine's one curve: Unreal's Spline Component (points with arrive
// and leave tangents, roll and scale, the types Curve / Curve Custom Tangent / Linear / Constant, a closed loop) evaluated
// by one function (cubic Hermite; Curve's auto tangents are Catmull-Rom's, so the rivers and the rails that sampled
// Catmull-Rom before give the same points), queried by distance or by input key (position, direction, transform,
// length). On it:
// - the Spline Mesh: a mesh bent along each segment (or each Mesh Length) on the GPU by skin.comp's spline branch (the
//   mesh is never copied: its vertices are read and the bent ones written to the skins' buffer, once, again only when the
//   curve changes), its shadow, rays and motion vectors as any skin's;
// - instances along the curve (spacing, a seed's jitter, aligned, scaled): plain static meshes;
// - the Cable (Unreal's Cable Component): particles joined by XPBD distance and bending constraints (substeps,
//   iterations, stiffness, length), held at both ends by actors or their sockets, colliding with the landscape, the
//   level's boxes, rigid bodies and Characters, pushed by the wind; drawn as a tube or a ribbon of bent pieces.
// The component is CRail (components.h, COMP_SPLINE): the 64 component ids are all taken.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

struct CRail;
struct CTransform;

typedef enum SplineKind { SPLINE_KIND_RAIL = 0, SPLINE_KIND_SPLINE, SPLINE_KIND_CABLE, SPLINE_KIND_COUNT } SplineKind;
typedef enum SplinePointType { SPLINE_POINT_CURVE = 0, SPLINE_POINT_CUSTOM, SPLINE_POINT_LINEAR, SPLINE_POINT_CONSTANT, SPLINE_POINT_COUNT } SplinePointType;
typedef enum SplineAlign { SPLINE_ALIGN_UPRIGHT = 0, SPLINE_ALIGN_FULL, SPLINE_ALIGN_NONE, SPLINE_ALIGN_COUNT } SplineAlign;
typedef enum CableDraw { CABLE_DRAW_TUBE = 0, CABLE_DRAW_RIBBON, CABLE_DRAW_HIDDEN, CABLE_DRAW_COUNT } CableDraw;
#define SPLINE_F_GAME_LINE 1u   // its line drawn in the game too (a debug)
#define SPLINE_F_MESH_COLLIDE 2u // its instances block (the Characters, traces)
#define SPLINE_COLLIDE_WORLD 1u  // the landscape and the level's static boxes
#define SPLINE_COLLIDE_BODIES 2u // rigid bodies and Characters
#define SPLINE_NODE_MESH 0x10000u
#define SPLINE_NODE_INST 0x20000u
#define SPLINE_NODE_CABLE 0x40000u
#define SPLINE_NODE_INDEX 0xFFFFu
#define CABLE_MAX_SEGMENTS 64
#define SPLINE_STEPS 16 // arc-length samples per segment (Unreal's reparam table: 10)
#define SPLINE_MAX_INSTANCES 4096
#define SPLINE_MAX_MESHES 512 // Spline Mesh pieces per spline

extern const char* const g_spline_kind_names[SPLINE_KIND_COUNT];
extern const char* const g_spline_point_names[SPLINE_POINT_COUNT];

// A curve ready to evaluate: the points and their resolved tangents (auto ones computed) in one space (the actor's, or
// the world's when built with rows), and its arc-length table. ~10 KB: on the stack or static, never per frame on the heap
typedef struct SplineCurve {
    uint32_t n;      // points
    bool closed;
    float p[SPLINE_MAX_POINTS][3], arrive[SPLINE_MAX_POINTS][3], leave[SPLINE_MAX_POINTS][3];
    float roll[SPLINE_MAX_POINTS];      // radians
    float scale[SPLINE_MAX_POINTS][2];
    uint8_t type[SPLINE_MAX_POINTS];
    float up[3];     // the space's up (the actor's Z)
    float dist[SPLINE_MAX_POINTS * SPLINE_STEPS + 1]; // the distance at each sample (segment s, step k: s * STEPS + k)
    float length;
} SplineCurve;

// --- The curve (pure: no world) ---
// From plain points (auto tangents, Curve), in their space or through rows (affine: the world's)
void spline_curve_points(SplineCurve* c, const float (*pts)[3], uint32_t n, bool closed, const float rows[3][4]);
// From a component: its points, types, tangents, roll and scale; rows NULL: the actor's space
void spline_curve_from(SplineCurve* c, const struct CRail* r, const float rows[3][4]);
// After p / type / leave / arrive changed: the auto tangents and the arc length again
void spline_curve_finish(SplineCurve* c);
uint32_t spline_segment_count(const SplineCurve* c);
float spline_key_max(const SplineCurve* c); // the last input key (n - 1; closed: n)
// By input key (Unreal's: point i at key i): the position and the derivative (cm per key)
void spline_eval_key(const SplineCurve* c, float key, float pos[3], float deriv[3]);
float spline_key_at_distance(const SplineCurve* c, float d);
float spline_distance_at_key(const SplineCurve* c, float key);
void spline_location_at_distance(const SplineCurve* c, float d, float out[3]);
void spline_direction_at_distance(const SplineCurve* c, float d, float out[3]); // unit
// The frame at a key: forward along the curve, right and up turned by the roll; the scale across
void spline_frame_at_key(const SplineCurve* c, float key, float pos[3], float fwd[3], float right[3], float up[3], float scale[2]);
float spline_roll_at_key(const SplineCurve* c, float key); // radians
float spline_closest_key(const SplineCurve* c, const float p[3]);
// A segment's Hermite: P0, T0 (leave), P1, T1 (arrive), cm per key
void spline_segment(const SplineCurve* c, uint32_t s, float p0[3], float t0[3], float p1[3], float t1[3]);
// A frame from a forward and the curve's up, turned by roll (the GPU's spline_frame in skin.comp: the same math)
void spline_frame(const float fwd[3], const float up[3], float roll, float right[3], float upo[3]);

// --- The component ---
void spline_component_defaults(struct CRail* r, uint32_t kind); // a new Spline / Cable / Rail's fields
uint32_t spline_kind(const struct CRail* r);
// A point added after i (a copy halfway to the next; i = n - 1: past the end), removed; false: no room / too few
bool spline_insert_point(struct CRail* r, int i);
bool spline_remove_point(struct CRail* r, int i);
void spline_set_point_type(struct CRail* r, int i, uint32_t type); // a Custom one starts from its auto tangents

// --- The world (spline_world.c) ---
// Every frame (editor and game, after the animation): the pieces of the splines that changed rebuilt (Spline Mesh pieces,
// instances, a cable's segments), the cables simulated (dt 0: held as they are), their pieces placed
void spline_update(World* w, float dt, bool playing);
void spline_begin_play(World* w); // the cables start again from their rest
void spline_end_play(World* w);
// A Spline Mesh piece's bend for the renderer (render.c skin_bind): the Hermite in the world, the roll, the scale, the
// mesh's forward and its extent along it; version changes whenever any of it does. False: not a bent piece
typedef struct SplineMeshView {
    float p0[3], t0[3], p1[3], t1[3]; // world cm (cm per segment)
    float roll0, roll1;               // radians
    float s0[2], s1[2];
    float up[3];
    uint32_t axis;                    // 0 X, 1 Y, 2 Z
    float xmin, inv_len;
    uint32_t version;
} SplineMeshView;
bool spline_mesh_view(World* w, Entity e, SplineMeshView* out);
// A spline actor's curve in the world (the Blueprint nodes, the editor); false: e has no spline
bool spline_world_curve(World* w, Entity e, SplineCurve* out);
// The Blueprint nodes' changes (the points in the actor's space unless world)
bool spline_bp_add_point(World* w, Entity e, const float p[3], bool world);
bool spline_bp_set_point(World* w, Entity e, int i, const float p[3], bool world);
bool spline_bp_remove_point(World* w, Entity e, int i);
bool spline_bp_clear(World* w, Entity e);
bool spline_bp_set_closed(World* w, Entity e, bool closed);
// A cable's: attach an end to an actor (and a socket), let it go, its length, its particles' positions (world cm)
bool cable_attach(World* w, Entity cable, bool end, Entity to, const char* socket);
bool cable_detach(World* w, Entity cable, bool end);
bool cable_set_length(World* w, Entity cable, float cm);
uint32_t cable_points(World* w, Entity cable, float (*out)[3], uint32_t cap);
bool cable_add_impulse(World* w, Entity cable, const float at[3], const float v[3], float radius);
// Stats (the Debug Viewer, the tests): splines, Spline Mesh pieces, instances, cables, particles; the update's CPU ms
typedef struct SplineStats {
    uint32_t splines, meshes, instances, cables, particles, rebuilt, contacts;
    double update_ms, cable_ms, rebuild_ms;
    double cable_ms_peak;
} SplineStats;
const SplineStats* spline_stats(void);
void spline_register_debug(void); // its Debug Viewer list ("Splines and Cables": the update, each cable's share)

// --- Pure cable simulation (the tests run it without a world) ---
typedef struct CableSim {
    uint32_t n;                         // particles (segments + 1)
    float x[CABLE_MAX_SEGMENTS + 1][3]; // cm
    float prev[CABLE_MAX_SEGMENTS + 1][3];
    float rest;                         // cm per segment
    float inv_mass[CABLE_MAX_SEGMENTS + 1];
    float lambda[CABLE_MAX_SEGMENTS + 1];
    uint32_t contacts;
} CableSim;
typedef struct CableParams {
    float stiffness, bend, damping, gravity, friction, radius;
    uint32_t iterations, substeps;
    float wind[3]; // cm/s
} CableParams;
// Laid straight from a to b with total length len (longer than the distance: it sags as it settles)
void cable_sim_init(CableSim* s, uint32_t segments, const float a[3], const float b[3], float len);
// Collision: spheres-out-of-boxes callback (world cm): pushes a point out (through the face it came in by: prev, its
// place before this substep), returns true when it touched
typedef bool (*CableCollideFn)(void* user, float p[3], const float prev[3], float radius, float normal[3]);
void cable_sim_step(CableSim* s, const CableParams* p, float dt, const float* pin_a, const float* pin_b, CableCollideFn collide, void* user);

// The tests (spline_test.c): spline_eval, spline_rail_river, cable_hang
bool spline_selftest_named(const char* name);
