// PCG (Q15, notas-dev/pcg.md): Unreal's Procedural Content Generation framework, simpler. A PCG Graph asset (.rpcg, a
// small JSON) is a graph of nodes that make, filter, change and spawn POINTS (a place, a rotation, a scale, a density,
// an extent, a seed and named attributes each):
//   - sources: points on the surface (the Landscape, the meshes in the volume), in a volume (a 3D grid) and along a
//     spline (a Rail, a River; the Spline Component through pcg_spline_add_provider);
//   - filters: by attribute, density, steepness (the slope) and the Landscape's painted layers;
//   - transforms: offset, rotation and scale with randomness, projection onto the ground, density noise (random or
//     spatial: clustered forests), attributes created;
//   - self pruning (overlapping points: the biggest kept) and difference (a set kept away from another);
//   - spawners: meshes (entities like the Foliage's: no Blueprint, culled and lit as any mesh), actors (a Blueprint
//     class) and nested PCG (another graph run around each point); subgraphs; parameters exposed to the component and
//     the Blueprints.
// A PCG component (an actor's: Add > PCG Volume) runs its graph inside its box: in the editor whenever something it
// reads changes, and in the game when the level loads, or cell by cell of the World Partition grid around the player
// (Partitioned: generated on the job system's workers within a budget, spawned within another, dropped when far).
// Deterministic: every random number comes from the component's seed, the node and the point's own seed (made from its
// place on a world-aligned grid), so the same seed makes the same points whatever the threads, the order or the cells.
// No malloc in the frame: each generation writes into its own linear arena (mem.h) and growable buffers reserved once.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "mem.h"

#define PCG_MAX_NODES 64
#define PCG_MAX_LINKS 128
#define PCG_MAX_PARAMS 16
#define PCG_MAX_ATTRS 8
#define PCG_NODE_F 48   // a node's number settings
#define PCG_NODE_S 8    // a node's text settings (assets, names)
#define PCG_STR 96
#define PCG_MAX_PINS 4
#define PCG_MAX_PROPS 40
#define PCG_MESH_ENTRIES 4
#define PCG_ATTR_NAME 24

// The node types (the file keeps their names: the order may change)
typedef enum PcgNodeType {
    PCGN_INPUT = 0, PCGN_OUTPUT,
    PCGN_SURFACE, PCGN_VOLUME, PCGN_SPLINE,
    PCGN_ATTR_FILTER, PCGN_DENSITY_FILTER, PCGN_STEEPNESS_FILTER, PCGN_LAYER_FILTER,
    PCGN_DENSITY_NOISE, PCGN_SPATIAL_NOISE, PCGN_CREATE_ATTR, PCGN_TRANSFORM, PCGN_PROJECT,
    PCGN_SELF_PRUNE, PCGN_DIFFERENCE, PCGN_MERGE,
    PCGN_MESH_SPAWNER, PCGN_ACTOR_SPAWNER, PCGN_NESTED, PCGN_SUBGRAPH,
    PCGN_TYPE_COUNT
} PcgNodeType;

// A setting's kind (the Details of the node and the file)
enum { PP_FLOAT = 0, PP_INT, PP_BOOL, PP_ENUM, PP_VEC3, PP_COLOR, PP_MESH, PP_MATERIAL, PP_CLASS, PP_GRAPH, PP_NAME, PP_SPLINE };
typedef struct PcgProp {
    const char* name;  // English (TR); the file's key
    uint8_t kind, slot; // slot: f[] (numbers, 3 for a vector) or s[] (texts)
    float def[3], mn, mx;
    const char* fmt;   // a number's format ("%.0f cm")
    const char* const* enums; uint8_t nenums;
} PcgProp;
// Node families (the header's color in the editor)
enum { PCGF_FAM_IO = 0, PCGF_FAM_SOURCE, PCGF_FAM_FILTER, PCGF_FAM_POINTS, PCGF_FAM_SPAWN, PCGF_FAM_GRAPH };
typedef struct PcgNodeInfo {
    const char* name;      // English (TR)
    const char* id;        // the file's ("SurfaceSampler")
    const char* desc;      // the node's description (its tip)
    uint8_t family, nin, nout;
    const char* in_names[PCG_MAX_PINS];
    const char* out_names[2];
    uint8_t nprops;
    PcgProp props[PCG_MAX_PROPS];
} PcgNodeInfo;
const PcgNodeInfo* pcg_node_info(int type);
int pcg_node_type_by_id(const char* id); // -1: none

#define PCGNF_DEBUG 1u    // its points shown in the viewport (the D key)
#define PCGNF_DISABLED 2u // passes its first input through

typedef struct PcgNode {
    uint16_t type, flags;
    uint32_t id;           // stable: the random streams follow it (made once, never reused in the graph)
    float pos[2];          // in the editor's canvas
    float f[PCG_NODE_F];
    uint8_t bind[PCG_NODE_F]; // a number bound to a graph parameter (its index + 1; 0: none)
    char s[PCG_NODE_S][PCG_STR];
} PcgNode;
typedef struct PcgLink { uint8_t from, from_pin, to, to_pin; } PcgLink;
typedef struct PcgParam { char name[32]; float value, mn, mx; } PcgParam;

typedef struct PcgGraph {
    uint32_t hash;          // its path's (the other assets' hash)
    uint32_t version;       // bumped by every change (the components regenerate)
    uint32_t next_id;
    uint32_t nnodes, nlinks, nparams;
    PcgNode nodes[PCG_MAX_NODES];
    PcgLink links[PCG_MAX_LINKS];
    PcgParam params[PCG_MAX_PARAMS];
    char vpath[256];
} PcgGraph;

// --- The asset (pcg_graph.c) ---
void pcg_init(const char* content_root, bool examples); // examples: Content/PCG/Examples written when missing (editor)
uint32_t pcg_vpath_hash(const char* vpath);
bool pcg_is_path(const char* vpath);
void pcg_graph_clear(PcgGraph* g);                 // empty: an Input and an Output
int pcg_graph_add(PcgGraph* g, int type, float x, float y); // a node with its defaults (its index; -1: full)
bool pcg_graph_link(PcgGraph* g, int from, int from_pin, int to, int to_pin); // (an input pin takes several wires)
void pcg_graph_remove(PcgGraph* g, int node);
int pcg_graph_param(const PcgGraph* g, const char* name); // -1: none
bool pcg_graph_load(const char* vpath, PcgGraph* g);
bool pcg_graph_save(const char* vpath, const PcgGraph* g);
bool pcg_graph_create(const char* folder_vpath, const char* name, const PcgGraph* from, char* out_vpath, uint32_t cap);
const PcgGraph* pcg_graph_get(uint32_t hash);      // read once, again when its file changes; NULL: none
const PcgGraph* pcg_graph_find(const char* name_or_vpath);
const char* pcg_graph_vpath(uint32_t hash);
void pcg_graph_remember(const char* vpath);
void pcg_graph_edited(const PcgGraph* g);          // the editor's copy is the one the components run (no save needed)
const PcgGraph* pcg_graph_cached(uint32_t hash);  // already read (no file touched: the workers'); NULL: not read
uint32_t pcg_graph_prepare(const PcgGraph* g, int depth); // its Subgraphs and Nested graphs read (main thread); their versions' hash
// A mesh setting's id: a shape's name ("Cube", "Cone"...) or an imported mesh's path; and a material's hash
uint32_t pcg_mesh_id(const char* s);
uint32_t pcg_material_hash(const char* vpath);

// --- Points (pcg_exec.c) ---
typedef struct PcgPoints {
    uint32_t n, cap;
    float *x, *y, *z;         // cm, world
    float *nx, *ny, *nz;      // the surface's normal
    float *pitch, *yaw, *roll; // degrees
    float *sx, *sy, *sz;
    float *density, *ext;     // 0..1; the half size (cm) at scale 1
    uint32_t* seed;
    uint32_t nattr;
    char attr_name[PCG_MAX_ATTRS][PCG_ATTR_NAME];
    float* attr[PCG_MAX_ATTRS];
} PcgPoints;

// --- Splines: the minimal sampling interface (points and tangents) any spline actor answers ---
typedef struct PcgSplinePoint { float p[3], t[3], width, dist; } PcgSplinePoint; // world cm, unit tangent, cm, cm along
// A provider: the actor's spline sampled densely (out up to cap; 0: not a spline of this kind). The Rail and the River
// are built in; the Spline Component (Q14) registers its own
typedef uint32_t (*PcgSplineFn)(World* w, Entity e, PcgSplinePoint* out, uint32_t cap);
void pcg_spline_add_provider(PcgSplineFn fn);
uint32_t pcg_spline_sample(World* w, Entity e, PcgSplinePoint* out, uint32_t cap);

// What a generation reads of the world, gathered on the main thread before it runs (anywhere after)
#define PCG_MAX_SPLINES 8
#define PCG_SPLINE_PTS 2048
typedef struct PcgMeshSource { uint32_t mesh; float rows[3][4]; float mn[3], mx[3]; } PcgMeshSource;
typedef struct PcgSources {
    uint32_t nmesh;
    PcgMeshSource* mesh;      // the static meshes in the volume (not the generated ones)
    uint32_t nspline;
    uint32_t spline_n[PCG_MAX_SPLINES];
    char spline_name[PCG_MAX_SPLINES][64]; // ("": the component's own actor)
    PcgSplinePoint* spline[PCG_MAX_SPLINES];
} PcgSources;

// What a generation makes: entities to create (the main thread does, within its budget)
enum { PCG_SPAWN_MESH = 0, PCG_SPAWN_ACTOR };
#define PCG_SPAWN_COLLISION 1u
#define PCG_SPAWN_SHADOW 2u
typedef struct PcgSpawn {
    uint8_t kind, flags;
    uint16_t node;
    uint32_t asset;            // a mesh id, or a Blueprint class's hash
    uint32_t material;
    float wind;
    float loc[3], rot[3], scale[3], color[3];
} PcgSpawn;
typedef struct PcgDebugPoint { float p[3]; uint32_t color; } PcgDebugPoint;

// A generation: the graph run over [bmin, bmax] (world cm). The arena and the buffers are the caller's (reset by it)
typedef struct PcgRun {
    ReArena* arena;
    ReVmBuf* spawns; uint32_t nspawns;
    ReVmBuf* debug; uint32_t ndebug; bool debug_all; // debug_all: the spawners' points when no node is marked
    uint32_t seed;
    float bmin[3], bmax[3];
    const PcgSources* src;
    float params[PCG_MAX_PARAMS]; // the graph's parameters (the component's overrides applied)
    uint32_t points_made;          // every node's points (the stats)
    uint32_t points_out;           // the points that reached a spawner or the Output
    uint32_t node_points[PCG_MAX_NODES];
    float node_ms[PCG_MAX_NODES];
    bool serial;                   // no re_parallel_for inside (the determinism test's comparison)
    int depth;                     // (subgraphs and nested graphs: 4 deep at most)
    char error[128];
    void* grid;                    // (the meshes' triangles binned, built when a node needs them)
} PcgRun;
bool pcg_execute(const PcgGraph* g, PcgRun* run);
// A hash of what a run spawned (the determinism checks)
uint64_t pcg_spawns_hash(const PcgSpawn* s, uint32_t n);

// --- The component (pcg_world.c): a Procedural World actor in PCG mode (CProcWorld.mode PROCGEN_PCG; the component
// masks are full: notas-dev/pcg.md) ---
#define PROCGEN_PCG 16u
enum { PCG_TRIGGER_ON_LOAD = 0, PCG_TRIGGER_ON_DEMAND, PCG_TRIGGER_COUNT };
extern const char* const g_pcg_trigger_names[PCG_TRIGGER_COUNT];
#define PCGC_ON_CHANGE 1u     // the editor regenerates when something it reads changes
#define PCGC_PARTITIONED 2u   // by the World Partition's cells around the player (the editor: around the camera)
#define PCGC_SHOW_POINTS 4u   // the points in the viewport
#define PCGC_CLEANED 8u       // (Cleanup: nothing until Generate)
bool pcg_is_component(World* w, Entity e);
Entity pcg_spawn_volume(World* w, const float at[3], const char* graph_vpath); // an actor with a PCG component
void pcg_update(World* w, const float focus[3], bool playing); // every frame (editor and game)
void pcg_begin_play(World* w);
void pcg_end_play(World* w);
void pcg_purge(World* w);                          // everything generated gone (before a level is saved)
// The Blueprints' (and the editor's buttons)
bool pcg_generate(World* w, Entity e);             // now (a partitioned one: its cells again)
bool pcg_cleanup(World* w, Entity e);
bool pcg_set_seed(World* w, Entity e, uint32_t seed);
bool pcg_set_parameter(World* w, Entity e, const char* name, float v);
bool pcg_is_generated(World* w, Entity e);
uint32_t pcg_generated_count(World* w, Entity e);  // its entities now
uint64_t pcg_component_hash(World* w, Entity e);   // what it spawned (its units' hashes, in any order)
// The stats (the Details, the Debug Viewer, the tests)
typedef struct PcgStats {
    uint32_t components, units, units_in, queued, spawned, points;
    uint32_t cells_generated, cells_dropped;
    double gen_ms, spawn_ms, ms, ms_peak;   // this frame's (generation on the workers, spawning, the whole update), the worst update
    double last_gen_ms;                      // the last whole generation's
    uint64_t last_hash;                      // what it spawned (pcg_spawns_hash)
} PcgStats;
const PcgStats* pcg_stats(void);
void pcg_register_debug(void); // (the Debug Viewer's PCG list)
void pcg_reset_peak(void);
// The points to show (the components with Show Points, the nodes marked Debug): world positions and colors
// (walk: cursor from 0; each call the next unit with points, NULL after the last)
const PcgDebugPoint* pcg_debug_next(uint32_t* cursor, uint32_t* n);
// The last generation's points and ms per node of a graph (the editor's cards); false: none yet
bool pcg_last_nodes(uint32_t graph_hash, const uint32_t** pts, const float** ms);
// The settings' hash (the editor's change test) and the box (world cm) of a component
bool pcg_component_bounds(World* w, Entity e, float mn[3], float mx[3]);
// The self-test (--pcg-test): determinism by seed, threads, the 100k points' cost; 0 = ok
int pcg_selftest(void);

// --- The editor (pcg_editor.c) ---
void pcg_editor_open(const char* vpath);
void pcg_editor_draw(void);
bool pcg_editor_add_menu(const char* folder_vpath); // Content Browser > Add > PCG Graph
struct ImDrawList;
// The points in the viewport (the components with Show Points), the PCG volumes' boxes when selected
void pcg_draw_debug(struct ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3],
                    float fov_deg, float x0, float y0, float x1, float y1, World* w, Entity selected);
// The component's Details (editor.c's section); true: changed
bool pcg_details(World* w, Entity e);
