// Physics (physics.h, notas-dev/fisica.md). Each frame while playing:
//   1. the bodies: kept from frame to frame (an entity -> body map; gathered again only when bodies come or go), synced
//      with what Blueprints and the Details changed (velocities, a teleport, the mass, the material)
//   2. fixed 120 Hz steps (up to max_substeps a frame), each one:
//      - broadphase: the awake bodies in a 3D hash grid (rebuilt in parallel every step), the sleeping ones in their own
//        grid (rebuilt only when one sleeps or wakes), the level's colliders in a 2D grid; the pairs counted then filled
//        block by block (the same order every run: deterministic)
//      - narrowphase in parallel: boxes by the separating axes and the clipped faces, spheres by their closest points,
//        the rest by GJK / EPA with the supporting faces clipped (physics_shapes.c); up to 4 points a manifold;
//        speculative points as far as the pair may close this step
//      - the solver: Box2D v3's soft step (TGS soft, Catto 2024) in 3D: solver_substeps substeps, each integrating the
//        velocities, warm starting, solving with soft contacts (a mass-spring of 30 Hz, the statics' twice as stiff),
//        integrating the positions and relaxing without the bias; restitution after them. The separation of each
//        point is updated from the bodies' motion in the substep (no narrowphase inside the step). Parallel by graph
//        coloring: the constraints of one color share no awake body, so each color is solved by every worker at once;
//        one parallel_for per step runs all the stages (atomic counters, no locks)
//      - joints: rows of the same soft step (hinge, slider, cone/twist, 6DOF with soft limits, drives, breaking)
//      - CCD: a body flagged (or faster than half its size a step) is swept from its start to its end by
//        conservative advancement (GJK distance), and stopped at the first time of impact
//      - islands: the awake bodies joined by contacts and joints (union-find); an island still for 0.5 s sleeps whole,
//        and wakes whole (a ring of its bodies) when something touches it or a Blueprint pushes one
//   3. write back the moved bodies (in parallel; not touched in the ECS log: Movable actors are sent every frame anyway)
//   4. fracture: a body hit harder than its Fracture speed becomes n x n x n pieces
// No allocation per frame: every array is a reservation committed as it grows (mem.h); a mesh's hull and BVH are made
// once (a cache, again after an import).
#include "physics.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "mem.h"
#include "phys_simd.h"
#ifdef _MSC_VER
#include <intrin.h> // (_BitScanForward)
#endif
#include "physmat.h"
#include "platform.h"
#include "project.h"
#include "physics_shapes.h"
#include "render.h"
#include "render_gpu.h"
#include "vehicle.h" // (B12f: water_query, the Buoyancy components' water)

#define STEP (P.step) // (the fixed step: 1 / Project Settings' Physics Rate, 120 Hz by default; Physics Quality)
#define MAX_STEPS ((uint32_t)project()->max_substeps)
#define SUBSTEPS ((uint32_t)(project()->solver_substeps > 0 ? project()->solver_substeps : 4))
#define SLOP 0.5f        // cm of overlap left alone (no jitter)
#define MARGIN 1.0f      // cm: contacts kept this far apart too (speculative: a resting box keeps its four corners)
#define SPEC_MAX 50.0f   // cm: the most a pair's speculative margin grows with its closing speed (CCD does the rest)
#define SKIN 1.0f        // cm: the GJK shapes' rounding inside them (a quarter of the thinnest at most)
#define MAX_DEPENETRATION 300.0f // cm/s: the fastest an overlap is pushed out (Unreal's Max Depenetration Velocity)
#define CONTACT_HZ 120.0f        // the contacts' softness (Box2D v3: 30 Hz at 240 Hz substeps, damping ratio 10; the statics'
                                 // twice), at most an eighth of the substeps' rate (4 substeps: 60 Hz, 8: 120 Hz). A stack sinks
                                 // N g / (2 w^2) per level: 30 Hz sank a 45-box pyramid 14 cm, 60 Hz 3.4, 120 Hz under 1
#define CONTACT_ZETA 10.0f
#define JOINT_HZ 60.0f           // the joints' locked rows
#define JOINT_ZETA 5.0f
#define JOINT_ITERATIONS 4       // the joints' passes in each substep's solve (half in the relax)
#define RESTITUTION_SPEED 100.0f // cm/s: slower hits don't bounce
#define MAX_SPEED 200000.0f      // cm/s (2 km/s: bullets pass, a blow-up doesn't run away)
#define SLEEP_V 8.0f     // cm/s (its spin at its rim counted)
#define SLEEP_TIME 0.5f
#define CELL 400.0f      // the static grid's cells (cm)
#define BUCKETS 65536u
#define MAX_BODIES 131072u
#define MAX_PAIRS (1u << 21)
#define MAX_MANIFOLDS (1u << 21)
#define MAX_POINTS (1u << 23)
#define MAX_PIECES_FRAME 512u
#define MAX_COLORS 24u   // the overflow (24) is solved by one worker
#define COLOR_OVERFLOW MAX_COLORS
#define MAX_STAGES 1024u
#define BLOCK_BODIES 256u
#define BLOCK_PAIRS 64u
#define BLOCK_CONTACTS 96u
#define GRID_BUCKETS_MAX (1u << 20)

enum { SHAPE_BOX = 0, SHAPE_BALL, SHAPE_CAPS, SHAPE_CYL, SHAPE_HULL, SHAPE_TRIS };

// A body's or a static's shape, around its center p with its axes R (local: x = sum l[a] R[a])
typedef struct Geom {
    uint32_t kind, axis;     // SHAPE_; a capsule's local axis
    float half[3];           // the bounds' half extents around bc (a box: the box)
    float radius, hz;        // a ball's / capsule's / cylinder's radius; a capsule's segment / a cylinder's half length
    float bc[3];             // the bounds' center (local)
    float s[3], o[3];        // a hull's / a mesh's vertex in local space: s * v + o
    const PhysHull* hull;    // (a static's: NULL until a body comes near it, made then; its bounds' box till then)
    const PhysMesh* mesh;
    uint32_t mesh_id;
    float skin, hk;          // a hull's skin and its core's scale toward its center of mass (physics_shapes.h)
} Geom;

typedef struct Body {
    Entity e;
    CTransform* t;
    CRigidBody* rb;
    const CBuoyancy* by;     // (B12f) its Buoyancy component, NULL: none
    float p[3], q[4], v[3], w[3];
    float R[3][3];           // rows: the local axes in the world
    float p0[3], q0[4];      // the step's start (the CCD sweeps from it)
    Geom g;
    float offset[3];         // the center (of mass) in the actor's space (scaled): p = location + R offset
    float mass, inv_mass, inv_il[3][3];
    float friction, restitution;
    uint32_t fcomb, rcomb;   // its Physical Material's combine modes (PCOMB_COUNT: none, the old rules)
    float extent, thin;      // its largest and smallest half extent
    float mn[3], mx[3];      // the world bounds (this step's, grown by its travel)
    float impact;            // the hardest approach speed this frame (cm/s): fracture
    float sleep_time;
    float wt[9];             // the transform as written last (any other change: a teleport)
    float src_mass;          // what its shape and mass were made from (a change: made again)
    uint32_t src_shape, src_mesh, src_mat;
    uint32_t gnext;          // asleep: the next body of its island (a ring)
    uint32_t awake, contacts, touching, fast, njoint, moved, wake_next;
    double ms;               // its share of the step (the Debug Viewer)
} Body;

typedef struct Static {
    float c[3], R[3][3];
    Geom g;
    float mn[3], mx[3];
    float v[3];              // a Character's velocity (kinematic): 0 for the level's meshes
} Static;

// The solver's view of a body (64 bytes, a cache line): velocity, inverse mass, spin, the world inverse inertia (its 6
// entries: xx yy zz xy xz yz); dyn 0: a static, a sleeping body or a Character (its velocity, never changed)
typedef struct State { float v[3], im; float w[3]; uint32_t dyn; float I[6]; float pad[2]; } State;
typedef struct Delta { float dp[3], pad; float dq[4]; } Delta; // its motion since the step began (the substeps')

typedef struct Pair { int32_t a, b; uint32_t first; uint8_t cap, n, touch, pad; float impact; } Pair; // b < 0: the static -(b + 1)
typedef struct Man { float n[3]; uint32_t np; float p[4][3], d[4]; } Man;          // a manifold: one normal (b to a)

// A contact constraint (a manifold) and its points
typedef struct Contact {
    uint32_t sa, sb, first, np;
    float n[3], mu;
    float t1[3], e;
    float t2[3], twr;        // twr: the points' mean distance from their middle (the twist's arm)
    int32_t a, b;
    uint32_t rec, soft;      // its first cache record (in pair order); 1: against a wall (the stiffer softness)
    uint32_t touch, slip;    // slip: it slid this step (new anchors where it is now)
    // the patch's friction at the points' middle: arms, masses, impulses (two directions, the twist)
    float cra[3], crb[3];
    float mt1, mt2, mtw, jt1, jt2, jtw;
    float drift[2];          // static friction: how far its anchors were apart along t1, t2 when the step began
    float aa[3], ab[3];      // its anchors (A's space; B's, or the world's for a static): kept while it holds
} Contact;
typedef struct Pt {
    float ra[3], rb[3];      // from the bodies' centers at the step's start
    float adj;               // the separation less the anchors' (Box2D's adjusted separation)
    float mn, jn, jmax, relv;
    float pad[5];
} Pt;
// warm starting: a point's impulse, its patch's friction and anchors, kept a step
typedef struct Rec { float lp[3], jn, ft[3], n[3], aa[3], ab[3], jtw; } Rec;
typedef struct Slot { uint32_t ka, kb, first, count; } Slot;     // ... found by the pair's keys

typedef struct Soft { float rate, mass, imp; } Soft;
typedef struct IParam { float g, ld, ad, wmax; } IParam; // a body's substep integration: gravity's dv, damping factors, top spin

// A Physics Constraint this step: its rows, made again each substep from the poses (directions, errors, masses)
enum { JR_EQ = 0, JR_LIMIT, JR_SOFTLIMIT, JR_SPRING, JR_MOTOR };
typedef struct JRow {
    float d[3], ra[3], rb[3];
    float C, vt, m, lo, hi, acc;
    uint8_t ang, kind, slot, pad; // pad 1: a limit's upper side (its slot keeps the impulse negative)
    Soft s;
} JRow;
typedef struct Joint {
    int32_t a, b;            // bodies (b < 0: the world)
    uint32_t sa, sb;
    CConstraint* c;
    JRow row[16];
    uint32_t nrow;
    uint32_t anch;           // its point is locked: the angular rows turn each body around the anchor (its inertia there)
    float IA[6], IB[6];      // ... those inverse inertias (world)
} Joint;
#define MAX_JOINTS 8192u

enum { ST_INTV = 0, ST_WARM, ST_SOLVE, ST_INTP, ST_RELAX, ST_REST, ST_STORE, ST_JWARM, ST_JSOLVE, ST_JRELAX,
       ST_WPACK, ST_WWARM, ST_WSOLVE, ST_WRELAX, ST_WREST, ST_WSTORE }; // (W: the wides of the colored contacts)
typedef struct Stage {
    uint32_t type, begin, count, block;
    int32_t nblocks;
    volatile int32_t next, done;
    uint32_t flip; // (odd substeps: the points the other way)
} Stage;

// a grid entry: a body in a cell (the cell's coordinates kept: different cells may share a bucket)
typedef struct GEnt { uint32_t body; int32_t x, y, z; float mn[3], mx[3]; } GEnt; // (its body's bounds: the pairs read no Body)
typedef struct Grid {
    ReVmBuf heads, ents, tmp;   // bucket starts (n + 1), the entries by bucket, the entries before sorting
    uint32_t nbuckets, nents;
    float cell;
} Grid;

static bool g_sleep_off; // (the tests': physics_set_sleep_enabled)
// (measures, read when play begins) REALENGINE_PHYS_SCALAR=1: the scalar solver for every color
static bool g_scalar;
static int g_solver_threads; // (REALENGINE_PHYS_THREADS: the solver's threads; 0: all)
static struct {
    bool ready, playing, rebuild;
    ReVmBuf bodies, map, statics, cells, refs, big, joints;
    ReVmBuf state, delta, awake, cmask, parent, mint, mcol, dynb;
    ReVmBuf pairs, mans, contacts, points, mdest, fast, ccd_t, blk;
    ReVmBuf rec[2], tab[2];
    uint32_t tab_size[2];
    int cur;
    uint32_t nbodies, nstatics, nbig, nkin, nawake, npairs, nmans, ncontacts, npoints, njoints, nfast, nbigb;
    uint32_t nrec;
    uint32_t statics_revision, statics_frames;
    uint32_t color_first[MAX_COLORS + 2], color_pfirst[MAX_COLORS + 2], color_wfirst[MAX_COLORS + 2];
    ReVmBuf wides;              // the colored contacts 8 at a time (the wide solver), by color
    ReVmBuf gcnt, gcur;         // the parallel grid's offsets and cursors
    ReVmBuf mnp;                // each manifold slot's points (the colors read only these)
    ReVmBuf iparam;             // per body: its substeps' integration (IParam)
    ReVmBuf sreq;               // the wakes and sleeps asked this frame (sync_bodies)
    uint32_t nwides;
    Grid awake_grid, sleep_grid;
    uint32_t sleep_version, sleep_grid_version, groups;
    ReVmBuf bigb, sbig;         // the awake / sleeping bodies too big for the grids (tested against every body)
    uint32_t nsbig;
    Stage stages[MAX_STAGES];
    uint32_t nstages;
    float h, inv_h;             // the substep
    Soft soft_contact, soft_static, soft_joint;
    float acc, frame_dt;
    PhysicsStats stats;
    uint32_t rng;
    uint32_t gjk0, epa0, fb0;
    double t_broad, t_narrow, t_solve, t_phase[12];
    volatile int32_t ccd_sweeps;
    volatile uint32_t sleepy;
    uint32_t nsteps, flip;
    float step;                 // the fixed step (s): 1 / physics_rate, taken when play begins and each frame
} P;

static Body* bodies(void) { return (Body*)P.bodies.base; }
static Static* statics(void) { return (Static*)P.statics.base; }
static uint32_t* bmap(void) { return (uint32_t*)P.map.base; }
static State* states(void) { return (State*)P.state.base; }
static Delta* deltas(void) { return (Delta*)P.delta.base; }
static Joint* joints(void) { return (Joint*)P.joints.base; }

// --- Small vector math ---

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static void mat_vec(const float m[3][3], const float v[3], float o[3]) {
    float x = dot3(m[0], v), y = dot3(m[1], v), z = dot3(m[2], v);
    o[0] = x, o[1] = y, o[2] = z;
}
static float frand(void) {
    P.rng = P.rng * 1664525u + 1013904223u;
    return (float)(P.rng >> 8) / 16777216.0f;
}

// quaternion (x, y, z, w) <-> axes (rows: local X, Y, Z in the world; a point: sum local[a] * R[a])
static void quat_axes(const float q[4], float R[3][3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    // the rotation matrix M (columns: the axes): R[a][k] = M[k][a]
    float M[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                      { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                      { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) R[a][k] = M[k][a];
}
static void axes_quat(const float R[3][3], float q[4]) {
    float m00 = R[0][0], m11 = R[1][1], m22 = R[2][2];
    float m01 = R[1][0], m10 = R[0][1], m02 = R[2][0], m20 = R[0][2], m12 = R[2][1], m21 = R[1][2]; // M[i][j] = R[j][i]
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; k++) q[k] /= l > 1e-12f ? l : 1.0f;
}

static void bounds_of(const float c[3], const float R[3][3], const Geom* g, float mn[3], float mx[3]) {
    for (int k = 0; k < 3; k++) {
        float m = c[k] + g->bc[0] * R[0][k] + g->bc[1] * R[1][k] + g->bc[2] * R[2][k];
        float e = g->kind == SHAPE_BALL ? g->radius : fabsf(R[0][k]) * g->half[0] + fabsf(R[1][k]) * g->half[1] + fabsf(R[2][k]) * g->half[2];
        mn[k] = m - e, mx[k] = m + e;
    }
}

// --- The meshes' hulls and triangle trees: made once each (a cache; dropped when the meshes are reloaded) ---

static struct {
    uint32_t gen, cap;
    PhysHull** hull;
    PhysMesh** tris;
    uint8_t *hull_tried, *tris_tried;
} MC;

static void mesh_cache_check(void) {
    uint32_t gen = render_meshes_generation();
    if (gen == MC.gen && MC.cap) return;
    for (uint32_t i = 0; i < MC.cap; i++) free(MC.hull[i]), phys_mesh_free(MC.tris[i]);
    if (MC.cap) {
        memset(MC.hull, 0, sizeof(void*) * MC.cap), memset(MC.tris, 0, sizeof(void*) * MC.cap);
        memset(MC.hull_tried, 0, MC.cap), memset(MC.tris_tried, 0, MC.cap);
    }
    MC.gen = gen;
}

static bool mesh_cache_slot(uint32_t slot) {
    mesh_cache_check();
    if (slot < MC.cap) return true;
    uint32_t cap = slot + 64; // (grows once per new mesh range: a cache, not a frame's)
    PhysHull** h = realloc(MC.hull, sizeof(void*) * cap);
    if (h) MC.hull = h;
    PhysMesh** t = realloc(MC.tris, sizeof(void*) * cap);
    if (t) MC.tris = t;
    uint8_t* a = realloc(MC.hull_tried, cap);
    if (a) MC.hull_tried = a;
    uint8_t* b = realloc(MC.tris_tried, cap);
    if (b) MC.tris_tried = b;
    if (!h || !t || !a || !b) return false;
    memset(MC.hull + MC.cap, 0, sizeof(void*) * (cap - MC.cap)), memset(MC.tris + MC.cap, 0, sizeof(void*) * (cap - MC.cap));
    memset(MC.hull_tried + MC.cap, 0, cap - MC.cap), memset(MC.tris_tried + MC.cap, 0, cap - MC.cap);
    MC.cap = cap;
    return true;
}

static const PhysHull* hull_for(uint32_t mesh);
static void hull_skin(Geom* g); // (below, with the GJK shapes)

// A mesh's hull if it was made (tried: it was attempted)
static const PhysHull* hull_peek(uint32_t mesh, bool* tried) {
    uint32_t slot = render_mesh_slot(mesh);
    mesh_cache_check();
    *tried = slot < MC.cap && MC.hull_tried[slot];
    return *tried ? MC.hull[slot] : NULL;
}

static void note_build(double ms) {
    P.stats.shapes_build_ms += ms;
    if (ms > P.stats.shapes_build_peak_ms) P.stats.shapes_build_peak_ms = ms;
}

// The convex hull of a mesh (its LOD 0's vertices); NULL: flat or not loaded (it collides as its box)
static const PhysHull* hull_for(uint32_t mesh) {
    uint32_t slot = render_mesh_slot(mesh);
    if (!mesh_cache_slot(slot)) return NULL;
    if (MC.hull_tried[slot]) return MC.hull[slot];
    MC.hull_tried[slot] = 1;
    const void* v;
    const uint32_t* idx;
    uint32_t nv, ni;
    if (!render_shape_cpu(slot, 0, &v, &nv, &idx, &ni) || !nv) return NULL;
    PhysHull* h = malloc(sizeof(PhysHull));
    if (h && !phys_hull_build(v, sizeof(GpuVertex), nv, h)) free(h), h = NULL;
    if (h) note_build(h->build_ms), P.stats.hulls++;
    return MC.hull[slot] = h;
}

// A mesh's triangles in a BVH (Use Complex Collision)
static const PhysMesh* tris_for(uint32_t mesh) {
    uint32_t slot = render_mesh_slot(mesh);
    if (!mesh_cache_slot(slot)) return NULL;
    if (MC.tris_tried[slot]) return MC.tris[slot];
    MC.tris_tried[slot] = 1;
    const void* v;
    const uint32_t* idx;
    uint32_t nv, ni;
    if (!render_shape_cpu(slot, 0, &v, &nv, &idx, &ni) || !nv || ni < 3) return NULL;
    PhysMesh* m = phys_mesh_build(v, sizeof(GpuVertex), nv, idx, ni);
    if (m) note_build(m->build_ms), P.stats.tri_meshes++;
    return MC.tris[slot] = m;
}

// (2026-11-19) the traces' Trace Complex (scene_query.c): a mesh's triangles, made on first use (the main thread)
const PhysMesh* physics_mesh_tris(uint32_t mesh) { return tris_for(mesh); }

// The collision shape of a mesh at a transform (Details > Physics > Collision Shape; a static's Use Complex Collision).
// offset: where the shape's center is in the actor's space (scaled)
static void geom_of(const CStaticMesh* sm, const CTransform* t, const CRigidBody* rb, bool fixed, Geom* g, float offset[3]) {
    memset(g, 0, sizeof *g);
    float mn[3] = { -50, -50, -50 }, mx[3] = { 50, 50, 50 }, half[3];
    // (the mesh's slot found once: render_mesh_slot searches the meshes; a slot passed on is taken as it is)
    uint32_t slot = sm ? render_mesh_slot(sm->mesh) : SHAPE_CUBE, mesh = slot;
    if (sm) render_mesh_bounds(slot, mn, mx);
    for (int k = 0; k < 3; k++) {
        float s = fabsf(t->scale[k]);
        half[k] = fmaxf((mx[k] - mn[k]) * 0.5f * s, 0.5f); // a plane: 1 cm thick
        offset[k] = (mx[k] + mn[k]) * 0.5f * t->scale[k];
        g->s[k] = t->scale[k];
    }
    memcpy(g->half, half, 12);
    uint32_t want = rb ? rb->collision_shape : COLSHAPE_AUTO;
    uint32_t kind = SHAPE_BOX;
    // (2026-11-24, item 4) the mesh asset's Collision Complexity (its viewer: .rlod) over the actor's Use Complex Collision
    int asset_cc = sm ? render_mesh_collision(slot) : 0;
    bool complex = asset_cc == 2 || (asset_cc == 0 && rb && rb->complex_collision);
    if (fixed && complex && sm && slot != SHAPE_CUBE && slot != SHAPE_PLANE) kind = SHAPE_TRIS;
    else if (want == COLSHAPE_BOX) kind = SHAPE_BOX;
    else if (want == COLSHAPE_SPHERE) kind = SHAPE_BALL;
    else if (want == COLSHAPE_CAPSULE) kind = SHAPE_CAPS;
    else if (want == COLSHAPE_CONVEX) kind = SHAPE_HULL;
    else if (!sm || slot == SHAPE_CUBE || slot == SHAPE_PLANE) kind = SHAPE_BOX;
    else if (slot == SHAPE_SPHERE) kind = SHAPE_BALL;
    else if (slot == SHAPE_CYLINDER && fabsf(half[0] - half[1]) <= 0.02f * fmaxf(half[0], half[1])) kind = SHAPE_CYL;
    else kind = SHAPE_HULL; // the cone, an oval cylinder, an imported mesh
    if (kind == SHAPE_TRIS) {
        const PhysMesh* m = tris_for(mesh);
        if (m) {
            g->kind = SHAPE_TRIS, g->mesh = m;
            for (int k = 0; k < 3; k++) {
                g->bc[k] = (m->mx[k] + m->mn[k]) * 0.5f * t->scale[k];
                g->half[k] = fmaxf((m->mx[k] - m->mn[k]) * 0.5f * fabsf(t->scale[k]), 0.5f);
                offset[k] = 0.0f; // (its center: the actor's location; the triangles in its space)
            }
            return;
        }
        kind = SHAPE_HULL;
    }
    if (kind == SHAPE_HULL && fixed && sm) { // a static: around its bounds' center; its hull made when a body comes near
        bool tried;
        const PhysHull* h = hull_peek(mesh, &tried);
        if (!tried || (h && !h->boxlike)) {
            g->kind = SHAPE_HULL, g->hull = h, g->mesh_id = mesh;
            for (int k = 0; k < 3; k++) g->o[k] = -offset[k];
            if (h) hull_skin(g);
            return;
        }
        kind = SHAPE_BOX;
    }
    if (kind == SHAPE_HULL) {
        const PhysHull* h = sm ? hull_for(mesh) : NULL;
        if (!h || h->boxlike) kind = SHAPE_BOX; // (a box's hull: the boxes' fast path)
        else {
            g->kind = SHAPE_HULL, g->hull = h;
            for (int k = 0; k < 3; k++) {
                offset[k] = h->com[k] * t->scale[k];
                g->o[k] = -offset[k];
                g->bc[k] = (h->mx[k] + h->mn[k]) * 0.5f * t->scale[k] - offset[k];
                g->half[k] = fmaxf((h->mx[k] - h->mn[k]) * 0.5f * fabsf(t->scale[k]), 0.5f);
            }
            hull_skin(g);
            return;
        }
    }
    g->kind = kind;
    if (kind == SHAPE_BALL) g->radius = fmaxf(half[0], fmaxf(half[1], half[2]));
    else if (kind == SHAPE_CAPS) { // along its longest axis, as wide as the other two
        g->axis = half[0] >= half[1] && half[0] >= half[2] ? 0 : half[1] >= half[2] ? 1 : 2;
        g->radius = fmaxf(half[(g->axis + 1) % 3], half[(g->axis + 2) % 3]);
        g->hz = fmaxf(half[g->axis] - g->radius, 0.0f);
        for (int k = 0; k < 3; k++) g->half[k] = k == (int)g->axis ? g->hz + g->radius : g->radius;
    } else if (kind == SHAPE_CYL) {
        g->radius = fmaxf(half[0], half[1]), g->hz = half[2], g->axis = 2;
        g->half[0] = g->half[1] = g->radius;
    }
}

static float geom_volume(const Geom* g) {
    switch (g->kind) {
    case SHAPE_BALL: return 4.18879f * g->radius * g->radius * g->radius;
    case SHAPE_CAPS: return 3.14159265f * g->radius * g->radius * (2.0f * g->hz) + 4.18879f * g->radius * g->radius * g->radius;
    case SHAPE_CYL: return 3.14159265f * g->radius * g->radius * 2.0f * g->hz;
    case SHAPE_HULL: return g->hull->volume * fabsf(g->s[0] * g->s[1] * g->s[2]);
    default: return 8.0f * g->half[0] * g->half[1] * g->half[2];
    }
}

static bool invert3(const float m[3][3], float o[3][3]) {
    float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2], c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(fabsf(det) > 1e-30f)) return false;
    float id = 1.0f / det;
    o[0][0] = c00 * id, o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id, o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    o[1][0] = c01 * id, o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id, o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    o[2][0] = c02 * id, o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id, o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return true;
}

// The inverse inertia in the body's axes (mass m)
static void geom_inertia(const Geom* g, float m, float inv[3][3]) {
    float I[3][3] = { { 0 } };
    float r = g->radius;
    switch (g->kind) {
    case SHAPE_BALL: I[0][0] = I[1][1] = I[2][2] = 0.4f * m * r * r; break;
    case SHAPE_CAPS: {
        float h = 2.0f * g->hz, vc = 3.14159265f * r * r * h, vs = 4.18879f * r * r * r, mc = m * vc / (vc + vs), ms = m - mc;
        float ia = mc * r * r * 0.5f + ms * 0.4f * r * r, it = mc * (h * h / 12.0f + r * r * 0.25f) + ms * (0.4f * r * r + h * h * 0.25f + 0.375f * h * r);
        for (int k = 0; k < 3; k++) I[k][k] = k == (int)g->axis ? ia : it;
        break;
    }
    case SHAPE_CYL: {
        float h = 2.0f * g->hz;
        I[0][0] = I[1][1] = m * (3.0f * r * r + h * h) / 12.0f, I[2][2] = m * r * r * 0.5f;
        break;
    }
    case SHAPE_HULL: { // the second moment scaled (S C S |det S|), I = tr(C) - C, per its volume
        const PhysHull* hl = g->hull;
        float det = fabsf(g->s[0] * g->s[1] * g->s[2]), C[3][3], vol = hl->volume * det;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) C[i][j] = det * g->s[i] * g->s[j] * hl->cov[i][j];
        float tr = C[0][0] + C[1][1] + C[2][2], k = vol > 1e-9f ? m / vol : 0.0f;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) I[i][j] = ((i == j ? tr : 0.0f) - C[i][j]) * k;
        break;
    }
    default:
        for (int k = 0; k < 3; k++) {
            float u = 2.0f * g->half[(k + 1) % 3], v = 2.0f * g->half[(k + 2) % 3];
            I[k][k] = m * (u * u + v * v) / 12.0f;
        }
    }
    for (int k = 0; k < 3; k++) I[k][k] = fmaxf(I[k][k], 1e-6f);
    if (g->kind != SHAPE_HULL) { // diagonal
        memset(inv, 0, sizeof(float) * 9);
        for (int k = 0; k < 3; k++) inv[k][k] = 1.0f / I[k][k];
        return;
    }
    if (!invert3(I, inv)) {
        memset(inv, 0, sizeof(float) * 9);
        for (int k = 0; k < 3; k++) inv[k][k] = 1.0f / I[k][k];
    }
}

// --- The static colliders: the level's meshes (and, every frame, the Characters as moving walls) ---

static uint32_t cell_hash(int x, int y) { return ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & (BUCKETS - 1); }

static void add_static(const float c[3], const float R[3][3], const Geom* g, const float v[3]) {
    if (!re_vm_ensure(&P.statics, (size_t)(P.nstatics + 1) * sizeof(Static))) return;
    Static* s = &statics()[P.nstatics++];
    memcpy(s->c, c, 12);
    memcpy(s->R, R, sizeof s->R);
    s->g = *g;
    memcpy(s->v, v, 12);
    bounds_of(c, R, g, s->mn, s->mx);
}

static void gather_statics(World* w) {
    P.nstatics = 0;
    const float zero[3] = { 0, 0, 0 };
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || (a->mask & COMP_BIT(COMP_CHARACTER)) || !a->count) continue;
        if (a->mask & COMP_BIT(COMP_CLOTH)) continue; // (Q9: a soft body's mesh isn't solid: what lands on it feels its push, cloth.c)
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const CRigidBody* rb = (a->mask & COMP_BIT(COMP_RIGID_BODY)) ? ecs_arch_column(a, COMP_RIGID_BODY) : NULL;
        const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            if ((rb && rb[i].simulate) || (tr[i].flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_COLLISION))) continue; // (NoCollision: the HLOD's proxies too)
            if (at && ecs_get(w, at[i].owner, COMP_CHARACTER)) continue; // a Character's own mesh
            if (at && at[i].kind == ATTACH_PROCGEN) continue;               // (a procedural world's: its ground is landscape_height)
            float off[3], R[3][3], c[3];
            Geom g;
            geom_of(&sm[i], &tr[i], rb ? &rb[i] : NULL, true, &g, off);
            rotator_axes(tr[i].rotation, R);
            for (int k = 0; k < 3; k++) c[k] = tr[i].location[k] + off[0] * R[0][k] + off[1] * R[1][k] + off[2] * R[2][k];
            add_static(c, R, &g, zero);
        }
    }
    // the grid: counts, offsets, the references (a collider over 64 cells goes to the big list)
    uint32_t* cells = (uint32_t*)P.cells.base;
    memset(cells, 0, (BUCKETS + 1) * sizeof(uint32_t));
    P.nbig = 0;
    if (!re_vm_ensure(&P.big, (size_t)(P.nstatics + 1) * 4u)) return;
    uint32_t* big = (uint32_t*)P.big.base;
    uint32_t nref = 0;
    const Static* st = statics();
    for (int pass = 0; pass < 2; pass++) {
        uint32_t* refs = (uint32_t*)P.refs.base;
        for (uint32_t i = 0; i < P.nstatics; i++) {
            int x0 = (int)floorf(st[i].mn[0] / CELL), x1 = (int)floorf(st[i].mx[0] / CELL);
            int y0 = (int)floorf(st[i].mn[1] / CELL), y1 = (int)floorf(st[i].mx[1] / CELL);
            if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
                if (pass == 0) big[P.nbig++] = i;
                continue;
            }
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++) {
                    uint32_t h = cell_hash(x, y);
                    if (pass == 0) cells[h + 1]++, nref++;
                    else refs[cells[h]++] = i;
                }
        }
        if (pass == 0) {
            for (uint32_t h = 0; h < BUCKETS; h++) cells[h + 1] += cells[h];
            if (!re_vm_ensure(&P.refs, (size_t)(nref ? nref : 1) * 4u)) return;
        }
    }
    memmove(cells + 1, cells, BUCKETS * sizeof(uint32_t)); // the second pass moved each start to its end
    cells[0] = 0;
    P.stats.statics = P.nstatics;
    P.nkin = 0;
}

// The Characters: capsules as upright boxes that push (infinite mass, their velocity), after the level's colliders
static void gather_characters(World* w, uint32_t base) {
    P.nstatics = base;
    P.nkin = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
        for (uint32_t i = 0; i < a->count; i++) {
            float R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            Geom g;
            memset(&g, 0, sizeof g);
            g.kind = SHAPE_BOX, g.half[0] = g.half[1] = ch[i].radius, g.half[2] = ch[i].half_height;
            add_static(tr[i].location, R, &g, ch[i].velocity);
            P.nkin++;
        }
    }
}

// --- Contacts: each pair's points (with their normals) gathered in a Gen, then made manifolds of up to 4 points ---

#define GEN_MAX 64
typedef struct Gen { uint32_t n; float nrm[GEN_MAX][3], p[GEN_MAX][3], d[GEN_MAX]; } Gen;

static void gen_push(Gen* g, const float n[3], const float p[3], float depth) {
    if (g->n >= GEN_MAX) return;
    memcpy(g->nrm[g->n], n, 12), memcpy(g->p[g->n], p, 12);
    g->d[g->n++] = depth;
}

// Sphere (center c, radius r) against a box (center bc, axes R, half h): the normal from the box to the sphere
static void sphere_box(Gen* G, float margin, const float c[3], float r, const float bc[3], const float R[3][3], const float h[3], bool flip) {
    float d[3] = { c[0] - bc[0], c[1] - bc[1], c[2] - bc[2] }, l[3], cp[3];
    for (int k = 0; k < 3; k++) l[k] = fminf(fmaxf(dot3(d, R[k]), -h[k]), h[k]);
    bool inside = fabsf(dot3(d, R[0])) <= h[0] && fabsf(dot3(d, R[1])) <= h[1] && fabsf(dot3(d, R[2])) <= h[2];
    float n[3], depth;
    if (inside) { // the nearest face
        int best = 0;
        float bd = 1e30f;
        for (int k = 0; k < 3; k++) {
            float dk = h[k] - fabsf(dot3(d, R[k]));
            if (dk < bd) bd = dk, best = k;
        }
        float s = dot3(d, R[best]) >= 0.0f ? 1.0f : -1.0f;
        for (int k = 0; k < 3; k++) n[k] = R[best][k] * s;
        depth = bd + r;
    } else {
        for (int k = 0; k < 3; k++) cp[k] = bc[k] + l[0] * R[0][k] + l[1] * R[1][k] + l[2] * R[2][k];
        float e[3] = { c[0] - cp[0], c[1] - cp[1], c[2] - cp[2] }, dl = len3(e);
        if (dl >= r + margin || dl < 1e-6f) return;
        for (int k = 0; k < 3; k++) n[k] = e[k] / dl;
        depth = r - dl;
    }
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = c[k] - n[k] * (r - depth * 0.5f);
    if (flip)
        for (int k = 0; k < 3; k++) n[k] = -n[k];
    gen_push(G, n, p, depth);
}

// A box's face polygon: the face of axis ax on side s (its 4 corners)
static void face_of(const float c[3], const float R[3][3], const float h[3], int ax, float s, float out[4][3]) {
    int u = (ax + 1) % 3, v = (ax + 2) % 3;
    static const float su[4] = { 1, -1, -1, 1 }, sv[4] = { 1, 1, -1, -1 };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) out[i][k] = c[k] + R[ax][k] * h[ax] * s + R[u][k] * h[u] * su[i] + R[v][k] * h[v] * sv[i];
}

// Clips a polygon to the side of a plane (dot(n, p) <= d)
static int clip(const float in[][3], int n, const float pn[3], float d, float out[][3]) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const float* a = in[i];
        const float* b = in[(i + 1) % n];
        float da = dot3(pn, a) - d, db = dot3(pn, b) - d;
        if (da <= 0.0f) memcpy(out[m++], a, 12);
        if ((da <= 0.0f) != (db <= 0.0f) && m < 16) {
            float t = da / (da - db);
            for (int k = 0; k < 3; k++) out[m][k] = a[k] + (b[k] - a[k]) * t;
            m++;
        }
    }
    return m;
}

// Box A against box B (b < 0: a static). The contact normals point from B to A.
static void box_box(Gen* G, float margin, const float ca[3], const float Ra[3][3], const float ha[3], const float cb[3], const float Rb[3][3],
                    const float hb[3]) {
    float T[3] = { cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2] };
    float best = 1e30f, bestn[3] = { 0, 0, 1 };
    int best_axis = -1;
    float best_face = 1e30f;
    int face_axis = -1;
    float face_n[3] = { 0, 0, 1 };
    for (int i = 0; i < 15; i++) {
        float L[3];
        if (i < 3) memcpy(L, Ra[i], 12);
        else if (i < 6) memcpy(L, Rb[i - 3], 12);
        else {
            cross3(Ra[(i - 6) / 3], Rb[(i - 6) % 3], L);
            float l = len3(L);
            if (l < 1e-4f) continue; // parallel edges: the face axes cover it
            for (int k = 0; k < 3; k++) L[k] /= l;
        }
        float ra = fabsf(dot3(Ra[0], L)) * ha[0] + fabsf(dot3(Ra[1], L)) * ha[1] + fabsf(dot3(Ra[2], L)) * ha[2];
        float rb = fabsf(dot3(Rb[0], L)) * hb[0] + fabsf(dot3(Rb[1], L)) * hb[1] + fabsf(dot3(Rb[2], L)) * hb[2];
        float dist = dot3(T, L), overlap = ra + rb - fabsf(dist);
        if (overlap < -margin) return; // separated
        float s = dist > 0.0f ? 1.0f : -1.0f; // L oriented from A to B
        if (i < 6 && overlap < best_face) {
            best_face = overlap, face_axis = i;
            for (int k = 0; k < 3; k++) face_n[k] = L[k] * s;
        }
        if (overlap < best) {
            best = overlap, best_axis = i;
            for (int k = 0; k < 3; k++) bestn[k] = L[k] * s;
        }
    }
    // an edge axis wins only when clearly smaller (a face gives the flat, stable manifold)
    if (best_axis >= 6 && best > best_face * 0.95f - 0.25f) best_axis = face_axis, best = best_face, memcpy(bestn, face_n, 12);
    float n_ba[3] = { -bestn[0], -bestn[1], -bestn[2] }; // from B to A
    if (best_axis < 6) {
        // the reference face (on the box whose axis it is) and the incident face of the other, clipped to it
        bool ref_a = best_axis < 3;
        const float* rc = ref_a ? ca : cb;
        const float(*rR)[3] = ref_a ? Ra : Rb;
        const float* rh = ref_a ? ha : hb;
        const float* ic = ref_a ? cb : ca;
        const float(*iR)[3] = ref_a ? Rb : Ra;
        const float* ih = ref_a ? hb : ha;
        int ax = best_axis % 3;
        float rn[3]; // the reference face's outward normal, toward the other box
        for (int k = 0; k < 3; k++) rn[k] = ref_a ? bestn[k] : -bestn[k];
        float rs = dot3(rn, rR[ax]) > 0.0f ? 1.0f : -1.0f;
        // the incident face: the other box's face most against rn
        int iax = 0;
        float imin = 1e30f, is = 1.0f;
        for (int k = 0; k < 3; k++) {
            float d = dot3(iR[k], rn);
            if (-fabsf(d) < imin) imin = -fabsf(d), iax = k, is = d > 0.0f ? -1.0f : 1.0f;
        }
        float poly[16][3], tmp[16][3];
        face_of(ic, iR, ih, iax, is, poly);
        int np = 4;
        int u = (ax + 1) % 3, v = (ax + 2) % 3;
        const int sides[4] = { u, u, v, v };
        for (int sd = 0; sd < 4 && np > 0; sd++) {
            float pn[3], sg = sd % 2 ? -1.0f : 1.0f;
            for (int k = 0; k < 3; k++) pn[k] = rR[sides[sd]][k] * sg;
            float d = dot3(pn, rc) + rh[sides[sd]];
            np = clip((const float(*)[3])poly, np, pn, d, tmp);
            memcpy(poly, tmp, sizeof(float) * 3 * (size_t)np);
        }
        float plane = dot3(rn, rc) + rh[ax] * rs * dot3(rn, rR[ax]);
        int made = 0;
        for (int i = 0; i < np && made < 8; i++) {
            float sep = dot3(rn, poly[i]) - plane;
            if (sep > margin) continue;
            float p[3];
            for (int k = 0; k < 3; k++) p[k] = poly[i][k] - rn[k] * sep * 0.5f;
            gen_push(G, n_ba, p, -sep);
            made++;
        }
        if (made) return;
    }
    // edge against edge: the supporting edges' closest points
    int i = (best_axis - 6) / 3, j = (best_axis - 6) % 3;
    if (best_axis < 6) i = 0, j = 0;
    float pa[3], pb[3];
    memcpy(pa, ca, 12), memcpy(pb, cb, 12);
    for (int k = 0; k < 3; k++) {
        if (k != i) {
            float s = dot3(Ra[k], bestn) > 0.0f ? 1.0f : -1.0f;
            for (int c = 0; c < 3; c++) pa[c] += Ra[k][c] * ha[k] * s;
        }
        if (k != j) {
            float s = dot3(Rb[k], bestn) > 0.0f ? -1.0f : 1.0f;
            for (int c = 0; c < 3; c++) pb[c] += Rb[k][c] * hb[k] * s;
        }
    }
    const float* da = Ra[i];
    const float* db = Rb[j];
    float r[3] = { pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2] };
    float d11 = dot3(da, da), d22 = dot3(db, db), d12 = dot3(da, db), d1r = dot3(da, r), d2r = dot3(db, r);
    float den = d11 * d22 - d12 * d12, sa = 0.0f, sb = 0.0f;
    if (fabsf(den) > 1e-6f) sa = (d12 * d2r - d22 * d1r) / den, sb = (d11 * d2r - d12 * d1r) / den;
    sa = fminf(fmaxf(sa, -ha[i]), ha[i]), sb = fminf(fmaxf(sb, -hb[j]), hb[j]);
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = (pa[k] + da[k] * sa + pb[k] + db[k] * sb) * 0.5f;
    gen_push(G, n_ba, p, best);
}

// A hull's skin: the core scaled toward the center of mass so its nearest face is SKIN in (a quarter at most)
static void hull_skin(Geom* g) {
    float in = phys_hull_inner(g->hull, g->s);
    g->skin = fminf(SKIN, 0.25f * in), g->hk = in > 1e-6f ? 1.0f - g->skin / in : 1.0f;
}

// A shape as GJK sees it: boxes, cylinders and hulls as a core shrunk by their skin, rounded by it (resting shapes touch
// with their cores apart: GJK answers, not EPA)
static void cvx_of(const float p[3], const float R[3][3], const Geom* g, Cvx* c) {
    memcpy(c->c, p, 12);
    memcpy(c->R, R, 36);
    c->radius = 0.0f, c->axis = 2, c->hull = NULL;
    switch (g->kind) {
    case SHAPE_BALL: c->kind = CVX_POINT, c->radius = g->radius; break;
    case SHAPE_CAPS:
        c->kind = CVX_SEGMENT, c->axis = g->axis, c->radius = g->radius;
        c->h[0] = c->h[1] = c->h[2] = 0.0f, c->h[g->axis] = g->hz;
        break;
    case SHAPE_CYL: {
        float sk = fminf(SKIN, 0.25f * fminf(g->radius, g->hz));
        c->kind = CVX_CYLINDER, c->h[0] = c->h[1] = g->radius - sk, c->h[2] = g->hz - sk, c->radius = sk;
        break;
    }
    case SHAPE_HULL: { // local = k (s v + o - q) + q, q the center of mass in local space
        c->kind = CVX_HULL, c->hull = g->hull, c->radius = g->skin;
        for (int k = 0; k < 3; k++) {
            float q = g->hull->com[k] * g->s[k] + g->o[k];
            c->s[k] = g->s[k] * g->hk, c->o[k] = g->o[k] * g->hk + q * (1.0f - g->hk);
        }
        break;
    }
    default: {
        float sk = fminf(SKIN, 0.25f * fminf(g->half[0], fminf(g->half[1], g->half[2])));
        c->kind = CVX_BOX, c->radius = sk;
        for (int k = 0; k < 3; k++) c->h[k] = g->half[k] - sk;
    }
    }
}

// A pair's speculative margin: as far as they may close this step (a fast body is caught before it is inside)
static float spec_margin(const float va[3], const float vb[3]) {
    float d[3] = { va[0] - vb[0], va[1] - vb[1], va[2] - vb[2] };
    return MARGIN + fminf(len3(d) * STEP, SPEC_MAX);
}

// A convex body against a mesh's triangles (the BVH around its bounds; one-sided: a triangle it is behind is skipped)
typedef struct TriQuery {
    Gen* G;
    const Body* A;
    const Static* S;
    const Cvx* cA;
    float flip, margin;
} TriQuery;

static bool tri_contact(void* user, const float t[3][3]) {
    const TriQuery* q = user;
    const Static* S = q->S;
    Cvx T;
    T.kind = CVX_TRIANGLE, T.radius = 0.0f, T.axis = 2, T.hull = NULL;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            T.t[c][k] = S->c[k] + t[c][0] * S->g.s[0] * S->R[0][k] + t[c][1] * S->g.s[1] * S->R[1][k] + t[c][2] * S->g.s[2] * S->R[2][k];
    float e1[3] = { T.t[1][0] - T.t[0][0], T.t[1][1] - T.t[0][1], T.t[1][2] - T.t[0][2] };
    float e2[3] = { T.t[2][0] - T.t[0][0], T.t[2][1] - T.t[0][1], T.t[2][2] - T.t[0][2] };
    cross3(e1, e2, T.tn);
    float l = len3(T.tn);
    if (l < 1e-8f) return true;
    for (int k = 0; k < 3; k++) T.tn[k] *= q->flip / l;
    float d[3] = { q->A->p[0] - T.t[0][0], q->A->p[1] - T.t[0][1], q->A->p[2] - T.t[0][2] };
    if (dot3(d, T.tn) < 0.0f) return true; // (behind it: the mesh's other side)
    for (int k = 0; k < 3; k++) T.c[k] = (T.t[0][k] + T.t[1][k] + T.t[2][k]) / 3.0f;
    static const float I3[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    memcpy(T.R, I3, 36);
    PhysManifold m;
    if (phys_collide(q->cA, &T, q->margin, &m))
        for (uint32_t i = 0; i < m.np; i++) gen_push(q->G, m.n, m.p[i], m.depth[i]);
    return q->G->n + 4 < GEN_MAX;
}

// A body's bounds (with this step's travel) in a static mesh's space
static void tris_box(const Body* A, const Static* S, float mn[3], float mx[3]) {
    float wc[3], wh[3], d[3];
    for (int k = 0; k < 3; k++) wc[k] = (A->mn[k] + A->mx[k]) * 0.5f, wh[k] = (A->mx[k] - A->mn[k]) * 0.5f + MARGIN, d[k] = wc[k] - S->c[k];
    for (int k = 0; k < 3; k++) {
        float s = S->g.s[k], as = fmaxf(fabsf(s), 1e-6f);
        float lc = dot3(S->R[k], d) / (s >= 0.0f ? as : -as);
        float lh = (fabsf(S->R[k][0]) * wh[0] + fabsf(S->R[k][1]) * wh[1] + fabsf(S->R[k][2]) * wh[2]) / as;
        mn[k] = lc - lh, mx[k] = lc + lh;
    }
}

static void collide_tris(Gen* G, const Body* A, const Static* S) {
    float mn[3], mx[3];
    tris_box(A, S, mn, mx);
    Cvx cA;
    cvx_of(A->p, A->R, &A->g, &cA);
    TriQuery q = { G, A, S, &cA, S->g.s[0] * S->g.s[1] * S->g.s[2] < 0.0f ? -1.0f : 1.0f, spec_margin(A->v, S->v) };
    phys_mesh_query(S->g.mesh, mn, mx, tri_contact, &q);
}

// Any two shapes (S: B is that static): the boxes' and spheres' own tests, the rest by GJK
static void collide_pair(Gen* G, const Body* A, const float pb[3], const float Rb[3][3], const Geom* gb, const float vb[3], const Static* S) {
    const Geom* ga = &A->g;
    if (gb->kind == SHAPE_TRIS) {
        if (S) collide_tris(G, A, S);
        return;
    }
    float margin = spec_margin(A->v, vb);
    if (ga->kind == SHAPE_BALL && gb->kind == SHAPE_BALL) {
        float d[3] = { A->p[0] - pb[0], A->p[1] - pb[1], A->p[2] - pb[2] }, l = len3(d), r = ga->radius + gb->radius;
        if (l >= r + margin || l < 1e-6f) return;
        float n[3] = { d[0] / l, d[1] / l, d[2] / l }, p[3];
        for (int k = 0; k < 3; k++) p[k] = A->p[k] - n[k] * ga->radius;
        gen_push(G, n, p, r - l);
    } else if (ga->kind == SHAPE_BALL && gb->kind == SHAPE_BOX) sphere_box(G, margin, A->p, ga->radius, pb, Rb, gb->half, false);
    else if (ga->kind == SHAPE_BOX && gb->kind == SHAPE_BALL) sphere_box(G, margin, pb, gb->radius, A->p, A->R, ga->half, true);
    else if (ga->kind == SHAPE_BOX && gb->kind == SHAPE_BOX) box_box(G, margin, A->p, A->R, ga->half, pb, Rb, gb->half);
    else {
        Cvx ca, cb;
        cvx_of(A->p, A->R, ga, &ca), cvx_of(pb, Rb, gb, &cb);
        PhysManifold m;
        if (phys_collide(&ca, &cb, margin, &m))
            for (uint32_t i = 0; i < m.np; i++) gen_push(G, m.n, m.p[i], m.depth[i]);
    }
}

// Up to 4 of a group's points: the deepest, the farthest from it, the largest triangle, the far side of it
static void reduce4(const Gen* G, const uint32_t* idx, uint32_t n, const float nrm[3], Man* m) {
    uint32_t k[4] = { 0, 0, 0, 0 };
    float best = -1e30f;
    for (uint32_t i = 0; i < n; i++)
        if (G->d[idx[i]] > best) best = G->d[idx[i]], k[0] = i;
    const float* p0 = G->p[idx[k[0]]];
    best = -1.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d[3] = { G->p[idx[i]][0] - p0[0], G->p[idx[i]][1] - p0[1], G->p[idx[i]][2] - p0[2] };
        if (dot3(d, d) > best) best = dot3(d, d), k[1] = i;
    }
    float e[3] = { G->p[idx[k[1]]][0] - p0[0], G->p[idx[k[1]]][1] - p0[1], G->p[idx[k[1]]][2] - p0[2] }, area[GEN_MAX];
    best = 0.0f;
    int side = 1;
    for (uint32_t i = 0; i < n; i++) {
        float d[3] = { G->p[idx[i]][0] - p0[0], G->p[idx[i]][1] - p0[1], G->p[idx[i]][2] - p0[2] }, c[3];
        cross3(e, d, c);
        area[i] = dot3(c, nrm);
        if (fabsf(area[i]) > best) best = fabsf(area[i]), k[2] = i, side = area[i] >= 0.0f ? 1 : -1;
    }
    best = 1e-6f;
    k[3] = k[2];
    for (uint32_t i = 0; i < n; i++)
        if (-(float)side * area[i] > best) best = -(float)side * area[i], k[3] = i;
    m->np = 0;
    for (int j = 0; j < 4; j++) {
        bool seen = false;
        for (int q = 0; q < j; q++) seen |= k[q] == k[j];
        if (seen) continue;
        memcpy(m->p[m->np], G->p[idx[k[j]]], 12);
        m->d[m->np++] = G->d[idx[k[j]]];
    }
}

// A pair's points to its manifolds (one per normal; a mesh's triangles that face the same way merged), the deepest
// first when there are more than its slots
static uint32_t gen_manifolds(const Gen* G, bool merge, Man* out, uint32_t cap) {
    if (!G->n || !cap) return 0;
    uint8_t grp[GEN_MAX];
    float gn[GEN_MAX][3], deep[GEN_MAX];
    uint32_t ng = 0;
    for (uint32_t i = 0; i < G->n; i++) {
        int g = -1;
        if (i && !memcmp(G->nrm[i], G->nrm[i - 1], 12)) g = grp[i - 1];
        else if (merge)
            for (uint32_t k = 0; k < ng && g < 0; k++)
                if (dot3(gn[k], G->nrm[i]) > 0.995f) g = (int)k;
        if (g < 0) g = (int)ng++, memcpy(gn[g], G->nrm[i], 12), deep[g] = -1e30f;
        grp[i] = (uint8_t)g;
        if (G->d[i] > deep[g]) deep[g] = G->d[i];
    }
    uint32_t order[GEN_MAX];
    for (uint32_t k = 0; k < ng; k++) order[k] = k;
    uint32_t nm = ng < cap ? ng : cap;
    for (uint32_t k = 0; k < nm && ng > cap; k++) // (a partial selection sort: few groups)
        for (uint32_t j = k + 1; j < ng; j++)
            if (deep[order[j]] > deep[order[k]]) {
                uint32_t t = order[k];
                order[k] = order[j], order[j] = t;
            }
    for (uint32_t k = 0; k < nm; k++) {
        uint32_t g = order[k], idx[GEN_MAX], n = 0;
        for (uint32_t i = 0; i < G->n; i++)
            if (grp[i] == g) idx[n++] = i;
        Man* m = &out[k];
        memcpy(m->n, gn[g], 12);
        if (n <= 4) {
            m->np = n;
            for (uint32_t i = 0; i < n; i++) memcpy(m->p[i], G->p[idx[i]], 12), m->d[i] = G->d[idx[i]];
        } else
            reduce4(G, idx, n, gn[g], m);
    }
    return nm;
}

// --- Small math for the solver ---

static void qmul(const float a[4], const float b[4], float o[4]) { // (x, y, z, w)
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}
static void qconj(const float q[4], float o[4]) { o[0] = -q[0], o[1] = -q[1], o[2] = -q[2], o[3] = q[3]; }
static void qnorm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    l = l > 1e-12f ? 1.0f / l : 1.0f;
    for (int k = 0; k < 4; k++) q[k] *= l;
}
static void qrot(const float q[4], const float v[3], float o[3]) { // v + 2w (u x v) + 2 u x (u x v)
    float u[3] = { q[0], q[1], q[2] }, t[3], c[3];
    cross3(u, v, t);
    for (int k = 0; k < 3; k++) t[k] *= 2.0f;
    cross3(u, t, c);
    for (int k = 0; k < 3; k++) o[k] = v[k] + q[3] * t[k] + c[k];
}
static void qintegrate(float q[4], const float w[3], float h) { // q += 0.5 h (w, 0) q
    float dq[4] = { w[0] * q[3] + w[1] * q[2] - w[2] * q[1], -w[0] * q[2] + w[1] * q[3] + w[2] * q[0],
                    w[0] * q[1] - w[1] * q[0] + w[2] * q[3], -w[0] * q[0] - w[1] * q[1] - w[2] * q[2] };
    for (int k = 0; k < 4; k++) q[k] += 0.5f * h * dq[k];
    qnorm(q);
}
static void to_world(const float R[3][3], const float l[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = l[0] * R[0][k] + l[1] * R[1][k] + l[2] * R[2][k];
}
static void to_local(const float R[3][3], const float w[3], float o[3]) {
    float x = dot3(R[0], w), y = dot3(R[1], w), z = dot3(R[2], w);
    o[0] = x, o[1] = y, o[2] = z;
}
static void perp(const float n[3], float u[3], float v[3]) {
    if (fabsf(n[2]) < 0.9f) u[0] = -n[1], u[1] = n[0], u[2] = 0.0f;
    else u[0] = 0.0f, u[1] = -n[2], u[2] = n[1];
    float l = len3(u);
    for (int k = 0; k < 3; k++) u[k] /= l;
    cross3(n, u, v);
}
// The world inverse inertia R^T L R (R's rows the axes), its 6 entries
static void inv_world(const float R[3][3], const float L[3][3], bool diag, float I[6]) {
    float M[3][3];
    if (diag) {
        for (int i = 0; i < 3; i++)
            for (int j = i; j < 3; j++) M[i][j] = R[0][i] * R[0][j] * L[0][0] + R[1][i] * R[1][j] * L[1][1] + R[2][i] * R[2][j] * L[2][2];
    } else {
        float t[3][3];
        for (int a = 0; a < 3; a++)
            for (int j = 0; j < 3; j++) t[a][j] = L[a][0] * R[0][j] + L[a][1] * R[1][j] + L[a][2] * R[2][j];
        for (int i = 0; i < 3; i++)
            for (int j = i; j < 3; j++) M[i][j] = R[0][i] * t[0][j] + R[1][i] * t[1][j] + R[2][i] * t[2][j];
    }
    I[0] = M[0][0], I[1] = M[1][1], I[2] = M[2][2], I[3] = M[0][1], I[4] = M[0][2], I[5] = M[1][2];
}
static void imul(const float I[6], const float v[3], float o[3]) {
    float x = I[0] * v[0] + I[3] * v[1] + I[4] * v[2], y = I[3] * v[0] + I[1] * v[1] + I[5] * v[2], z = I[4] * v[0] + I[5] * v[1] + I[2] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}
static Soft make_soft(float hz, float zeta, float h) { // (Box2D's b2MakeSoft: a spring of hz, damped by zeta, implicit)
    if (hz <= 0.0f) return (Soft){ 0.0f, 1.0f, 0.0f };
    float om = 6.2831853f * hz, a1 = 2.0f * zeta + h * om, a2 = h * om * a1, a3 = 1.0f / (1.0f + a2);
    return (Soft){ om / a1, a2 * a3, a3 };
}
static uint32_t ctz32(uint32_t x) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long n;
    return _BitScanForward(&n, x) ? (uint32_t)n : 32u;
#else
    return x ? (uint32_t)__builtin_ctz(x) : 32u;
#endif
}

// --- Bodies: kept between frames, synced with the actors ---

static void body_shape(Body* b, const CStaticMesh* sm) {
    const CRigidBody* rb = b->rb;
    geom_of(sm, b->t, rb, false, &b->g, b->offset);
    const PhysMaterial* pm = rb->phys_material ? physmat_get(rb->phys_material) : NULL;
    float vol = geom_volume(&b->g);
    float m = rb->mass > 0.0f ? rb->mass : pm && pm->density > 0.0f ? fmaxf(vol * pm->density * 1e-3f, 0.05f) : fmaxf(vol * 1e-4f, 0.05f);
    b->mass = m, b->inv_mass = 1.0f / m;
    geom_inertia(&b->g, m, b->inv_il);
    if (b->g.kind == SHAPE_BALL) b->extent = b->thin = b->g.radius;
    else {
        b->extent = fmaxf(b->g.half[0], fmaxf(b->g.half[1], b->g.half[2]));
        b->thin = fminf(b->g.half[0], fminf(b->g.half[1], b->g.half[2]));
    }
    b->src_mass = rb->mass, b->src_shape = rb->collision_shape, b->src_mesh = sm ? sm->mesh : ~0u, b->src_mat = rb->phys_material;
}

static void body_material(Body* b) {
    const CRigidBody* rb = b->rb;
    const PhysMaterial* pm = rb->phys_material ? physmat_get(rb->phys_material) : NULL;
    if (pm) b->friction = pm->friction, b->restitution = pm->restitution, b->fcomb = pm->friction_combine, b->rcomb = pm->restitution_combine;
    else b->friction = rb->friction, b->restitution = rb->restitution, b->fcomb = b->rcomb = PCOMB_COUNT;
}

static void body_pose(Body* b) {
    const CTransform* t = b->t;
    rotator_axes(t->rotation, b->R);
    axes_quat(b->R, b->q);
    for (int k = 0; k < 3; k++) b->p[k] = t->location[k] + b->offset[0] * b->R[0][k] + b->offset[1] * b->R[1][k] + b->offset[2] * b->R[2][k];
    memcpy(b->wt, t->location, 36);
    bounds_of(b->p, b->R, &b->g, b->mn, b->mx);
}

static void delta_reset(Delta* d) { memset(d, 0, sizeof *d), d->dq[3] = 1.0f; }

// Wakes a sleeping body and every body of its island (the ring)
static void wake_island(uint32_t i) {
    Body* bd = bodies();
    if (i >= P.nbodies || bd[i].awake) return;
    uint32_t k = i, guard = 0;
    do {
        Body* b = &bd[k];
        uint32_t nx = b->gnext;
        b->awake = 1, b->sleep_time = 0.0f, b->gnext = k;
        b->rb->sleeping = 0, b->rb->sleep_time = 0.0f, b->rb->sleep_group = 0;
        k = nx;
    } while (k != i && k < P.nbodies && !bd[k].awake && ++guard < P.nbodies);
    P.sleep_version++;
}

static void body_asleep(uint32_t i, uint32_t group) {
    Body* b = &bodies()[i];
    b->awake = 0, b->moved = 1;
    memset(b->v, 0, 12), memset(b->w, 0, 12);
    b->rb->sleeping = 1, b->rb->sleep_group = group;
    memset(&states()[i + 1], 0, sizeof(State));
    delta_reset(&deltas()[i + 1]);
    bounds_of(b->p, b->R, &b->g, b->mn, b->mx);
    P.sleep_version++;
}

static bool reserve_bodies(uint32_t n) {
    size_t s = (size_t)n + 4096u + 2u; // (the static slot, the bodies, the Characters)
    return re_vm_ensure(&P.bodies, (size_t)n * sizeof(Body)) && re_vm_ensure(&P.state, s * sizeof(State)) && re_vm_ensure(&P.delta, s * sizeof(Delta)) &&
           re_vm_ensure(&P.awake, (size_t)n * 4u) && re_vm_ensure(&P.cmask, (size_t)n * 8u + 64u) && re_vm_ensure(&P.parent, (size_t)n * 8u + 64u) &&
           re_vm_ensure(&P.mint, (size_t)n * 4u + 64u) && re_vm_ensure(&P.fast, (size_t)n * 8u + 64u) && re_vm_ensure(&P.ccd_t, (size_t)n * 4u + 64u) &&
           re_vm_ensure(&P.bigb, (size_t)n * 4u + 64u) && re_vm_ensure(&P.sbig, (size_t)n * 4u + 64u) && re_vm_ensure(&P.dynb, (size_t)n + 64u) && re_vm_ensure(&P.iparam, (size_t)n * sizeof(IParam) + 64u);
}

// Every simulating actor a body again (play began, bodies came or went): their sleep and islands kept (their rings
// made again from the groups their CRigidBody keeps)
static void rebuild_bodies(World* w) {
    uint32_t* map = bmap();
    Body* bd = bodies();
    for (uint32_t i = 0; i < P.nbodies; i++) map[entity_index(bd[i].e)] = ~0u;
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        CRigidBody* rb = ecs_arch_column(a, COMP_RIGID_BODY);
        const CStaticMesh* sm = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        const CBuoyancy* bys = (a->mask & COMP_BIT(COMP_BUOYANCY)) ? ecs_arch_column(a, COMP_BUOYANCY) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < MAX_BODIES; i++) {
            if (!rb[i].simulate || (tr[i].flags & TRANSFORM_HIDDEN_GAME)) continue;
            if (!reserve_bodies(n + 1)) break;
            bd = bodies();
            Body* b = &bd[n];
            memset(b, 0, sizeof *b);
            b->e = ents[i], b->t = &tr[i], b->rb = &rb[i], b->by = bys ? &bys[i] : NULL;
            body_shape(b, sm ? &sm[i] : NULL);
            body_material(b);
            body_pose(b);
            memcpy(b->v, rb[i].velocity, 12), memcpy(b->w, rb[i].angular_velocity, 12);
            b->awake = !rb[i].sleeping, b->sleep_time = rb[i].sleep_time, b->gnext = n;
            if (!b->awake) memset(b->v, 0, 12), memset(b->w, 0, 12);
            rb[i].started = 1;
            map[entity_index(ents[i])] = n++;
        }
    }
    P.nbodies = n;
    reserve_bodies(n + 1);
    bd = bodies();
    // the rings of the islands asleep: a hash of their groups (the first one seen of each), the others put after it
    uint32_t size = 64;
    while (size < 2 * n) size <<= 1;
    if (!re_vm_ensure(&P.cmask, (size_t)size * 4u + 64u) || !re_vm_ensure(&P.parent, (size_t)size * 4u + 64u)) return;
    uint32_t* key = (uint32_t*)P.cmask.base;
    uint32_t* val = (uint32_t*)P.parent.base;
    memset(key, 0, (size_t)size * 4u);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = bd[i].awake ? 0u : bd[i].rb->sleep_group;
        if (!g) continue;
        for (uint32_t h = (g * 0x9E3779B1u) & (size - 1);; h = (h + 1) & (size - 1)) {
            if (!key[h]) {
                key[h] = g, val[h] = i;
                break;
            }
            if (key[h] == g) {
                uint32_t f = val[h];
                bd[i].gnext = bd[f].gnext, bd[f].gnext = i;
                break;
            }
        }
        if (g > P.groups) P.groups = g;
    }
    memset(P.state.base, 0, ((size_t)n + 2u) * sizeof(State));
    for (uint32_t i = 0; i < n + 2u; i++) delta_reset(&deltas()[i]);
    P.sleep_version++;
    P.rebuild = false;
}

// Each frame: the bodies' columns found again (rows move), what changed outside taken in; false: bodies came or went.
// In parallel by blocks of rows (2026-11-19: 0.8 ms a frame serial with 20 thousand); a wake or a sleep a Blueprint
// asked (they touch other bodies: the island's ring) done after, in order
#define SYNC_ROWS 512u
static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}
typedef struct SyncJob {
    World* w;
    uint32_t arch[RE_ECS_MAX_ARCHETYPES], first[RE_ECS_MAX_ARCHETYPES + 1], narch; // the archetypes with bodies, their first block
    volatile int32_t seen, bad, nreq;
    uint32_t* req; // (body << 1) | 1: put it to sleep alone; << 1: wake its island
} SyncJob;
static void sync_fn(void* arg, int blk) {
    SyncJob* j = arg;
    uint32_t k = 0;
    while (k + 1 < j->narch && j->first[k + 1] <= (uint32_t)blk) k++;
    const EcsArchetype* a = &j->w->archetypes[j->arch[k]];
    const uint32_t* map = bmap();
    Body* bd = bodies();
    CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
    CRigidBody* rb = ecs_arch_column(a, COMP_RIGID_BODY);
    const CStaticMesh* sm = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
    const CBuoyancy* bys = (a->mask & COMP_BIT(COMP_BUOYANCY)) ? ecs_arch_column(a, COMP_BUOYANCY) : NULL;
    const Entity* ents = ecs_arch_entities(a);
    uint32_t i0 = ((uint32_t)blk - j->first[k]) * SYNC_ROWS, i1 = i0 + SYNC_ROWS < a->count ? i0 + SYNC_ROWS : a->count, seen = 0;
    for (uint32_t i = i0; i < i1; i++) {
        if (!rb[i].simulate || (tr[i].flags & TRANSFORM_HIDDEN_GAME)) continue;
        uint32_t bi = map[entity_index(ents[i])];
        if (bi >= P.nbodies || bd[bi].e != ents[i]) {
            j->bad = 1;
            return;
        }
        Body* b = &bd[bi];
        b->t = &tr[i], b->rb = &rb[i], b->by = bys ? &bys[i] : NULL;
        seen++;
        const CStaticMesh* sms = sm ? &sm[i] : NULL;
        bool tele = memcmp(b->wt, tr[i].location, 36) != 0; // (location, rotation, scale: moved by someone else)
        bool reshape = (tele && memcmp(b->wt + 6, tr[i].scale, 12)) || rb[i].mass != b->src_mass || rb[i].collision_shape != b->src_shape ||
                       (sms ? sms->mesh : ~0u) != b->src_mesh || rb[i].phys_material != b->src_mat;
        if (reshape) body_shape(b, sms);
        if (rb[i].phys_material || b->fcomb != PCOMB_COUNT) body_material(b);
        else b->friction = rb[i].friction, b->restitution = rb[i].restitution;
        bool wake = false;
        if (tele || reshape) {
            body_pose(b);
            wake = !b->awake;
            b->moved = 1;
        }
        if (!b->awake && !rb[i].sleeping) wake = true; // (a Blueprint pushed it)
        else if (b->awake && rb[i].sleeping) {         // (a Blueprint put it to sleep: alone)
            j->req[re_atomic_add(&j->nreq, 1) - 1] = bi << 1 | 1u;
            continue;
        }
        if (wake) j->req[re_atomic_add(&j->nreq, 1) - 1] = bi << 1;
        if (b->awake) memcpy(b->v, rb[i].velocity, 12), memcpy(b->w, rb[i].angular_velocity, 12);
    }
    re_atomic_add(&j->seen, (int32_t)seen);
}
static bool sync_bodies(World* w) {
    if (P.rebuild) return false;
    SyncJob j;
    memset(&j, 0, sizeof j);
    j.w = w;
    uint32_t nb = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        j.arch[j.narch] = ai, j.first[j.narch++] = nb;
        nb += (a->count + SYNC_ROWS - 1) / SYNC_ROWS;
    }
    j.first[j.narch] = nb;
    if (!nb) return P.nbodies == 0;
    if (!re_vm_ensure(&P.sreq, ((size_t)P.nbodies + 64u) * 4u)) return false;
    j.req = (uint32_t*)P.sreq.base;
    re_parallel_for((int)nb, sync_fn, &j);
    if (j.bad) return false;
    // the asked wakes and sleeps, in the bodies' order (the same every run)
    uint32_t n = (uint32_t)j.nreq;
    qsort(j.req, n, 4, cmp_u32);
    for (uint32_t a = 0; a < n; a++) {
        uint32_t bi = j.req[a] >> 1;
        Body* b = &bodies()[bi];
        if (j.req[a] & 1u) {
            if (b->awake) body_asleep(bi, 0);
        } else {
            if (!b->awake) wake_island(bi); // (its island's first: the others asked are awake by now)
            if (b->awake) memcpy(b->v, b->rb->velocity, 12), memcpy(b->w, b->rb->angular_velocity, 12);
        }
    }
    return (uint32_t)j.seen == P.nbodies;
}

// --- Broadphase ---

static uint32_t hash3(int x, int y, int z) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)z * 83492791u); }

static bool overlap(const float amn[3], const float amx[3], const float bmn[3], const float bmx[3]) {
    return amn[0] <= bmx[0] && amx[0] >= bmn[0] && amn[1] <= bmx[1] && amx[1] >= bmn[1] && amn[2] <= bmx[2] && amx[2] >= bmn[2];
}

static void cell_range(const float mn[3], const float mx[3], float ic, int lo[3], int hi[3]) {
    for (int k = 0; k < 3; k++) lo[k] = (int)floorf(mn[k] * ic), hi[k] = (int)floorf(mx[k] * ic);
}

// A grid of the listed bodies, built in parallel (2026-11-19: serial it was 1.3 ms a step with 20 thousand): each
// body's cells counted, the entries written at their body's offset, counted per bucket (atomics), the buckets' starts
// summed, the entries scattered (atomics) and each bucket sorted by body and cell (deterministic). The ones over 64 cells
// into big (their count returned)
#define GB_BODIES 256u
#define GB_ENTS 2048u
#define GB_BUCKETS 8192u
typedef struct GridJob {
    Grid* g;
    const uint32_t* list;
    uint32_t n, nbk, nents;
    float ic;
    uint32_t* cnt;  // per listed body: its first entry (after the sum); its cell count before (~0u: too big)
    uint32_t* cur;  // the buckets' cursors while scattering
    uint32_t* bsum; // the bucket blocks' sums
} GridJob;
static void gb_count_fn(void* arg, int blk) {
    GridJob* j = arg;
    const Body* bd = bodies();
    uint32_t i1 = ((uint32_t)blk + 1) * GB_BODIES < j->n ? ((uint32_t)blk + 1) * GB_BODIES : j->n;
    for (uint32_t i = (uint32_t)blk * GB_BODIES; i < i1; i++) {
        int lo[3], hi[3];
        cell_range(bd[j->list[i]].mn, bd[j->list[i]].mx, j->ic, lo, hi);
        int64_t c = (int64_t)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
        j->cnt[i] = c > 64 ? ~0u : (uint32_t)c;
    }
}
static void gb_fill_fn(void* arg, int blk) {
    GridJob* j = arg;
    const Body* bd = bodies();
    GEnt* tmp = (GEnt*)j->g->tmp.base;
    volatile int32_t* heads = (volatile int32_t*)j->g->heads.base;
    uint32_t i1 = ((uint32_t)blk + 1) * GB_BODIES < j->n ? ((uint32_t)blk + 1) * GB_BODIES : j->n, m1 = j->nbk - 1;
    for (uint32_t i = (uint32_t)blk * GB_BODIES; i < i1; i++) {
        if (j->cnt[i + 1] == j->cnt[i]) continue; // (too big: no entries)
        const Body* b = &bd[j->list[i]];
        int lo[3], hi[3];
        cell_range(b->mn, b->mx, j->ic, lo, hi);
        uint32_t m = j->cnt[i];
        for (int x = lo[0]; x <= hi[0]; x++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int z = lo[2]; z <= hi[2]; z++) {
                    tmp[m++] = (GEnt){ j->list[i], x, y, z, { b->mn[0], b->mn[1], b->mn[2] }, { b->mx[0], b->mx[1], b->mx[2] } };
                    re_atomic_add(&heads[(hash3(x, y, z) & m1) + 1], 1);
                }
    }
}
static void gb_bsum_fn(void* arg, int blk) { // a block of buckets' total (heads[h + 1]: the counts)
    GridJob* j = arg;
    const uint32_t* heads = (const uint32_t*)j->g->heads.base;
    uint32_t h1 = ((uint32_t)blk + 1) * GB_BUCKETS < j->nbk ? ((uint32_t)blk + 1) * GB_BUCKETS : j->nbk, s = 0;
    for (uint32_t h = (uint32_t)blk * GB_BUCKETS; h < h1; h++) s += heads[h + 1];
    j->bsum[blk] = s;
}
static void gb_prefix_fn(void* arg, int blk) { // the starts: heads[h] (the block's offset summed in), the cursors
    GridJob* j = arg;
    uint32_t* heads = (uint32_t*)j->g->heads.base;
    uint32_t h1 = ((uint32_t)blk + 1) * GB_BUCKETS < j->nbk ? ((uint32_t)blk + 1) * GB_BUCKETS : j->nbk, s = j->bsum[blk];
    for (uint32_t h = (uint32_t)blk * GB_BUCKETS; h < h1; h++) {
        uint32_t c = heads[h + 1];
        heads[h + 1] = s + c, j->cur[h] = s, s += c; // (heads[h + 1]: the bucket's end = the next one's start)
    }
}
static void gb_scatter_fn(void* arg, int blk) {
    GridJob* j = arg;
    const GEnt* tmp = (const GEnt*)j->g->tmp.base;
    GEnt* ents = (GEnt*)j->g->ents.base;
    uint32_t i1 = ((uint32_t)blk + 1) * GB_ENTS < j->nents ? ((uint32_t)blk + 1) * GB_ENTS : j->nents, m1 = j->nbk - 1;
    for (uint32_t i = (uint32_t)blk * GB_ENTS; i < i1; i++) {
        uint32_t h = hash3(tmp[i].x, tmp[i].y, tmp[i].z) & m1;
        ents[re_atomic_add((volatile int32_t*)&j->cur[h], 1) - 1] = tmp[i];
    }
}
static bool gent_less(const GEnt* a, const GEnt* b) {
    if (a->body != b->body) return a->body < b->body;
    if (a->x != b->x) return a->x < b->x;
    if (a->y != b->y) return a->y < b->y;
    return a->z < b->z;
}
static void gb_sort_fn(void* arg, int blk) { // (a bucket: a few entries; insertion sort)
    GridJob* j = arg;
    const uint32_t* heads = (const uint32_t*)j->g->heads.base;
    GEnt* e = (GEnt*)j->g->ents.base;
    uint32_t h1 = ((uint32_t)blk + 1) * GB_BUCKETS < j->nbk ? ((uint32_t)blk + 1) * GB_BUCKETS : j->nbk;
    for (uint32_t h = (uint32_t)blk * GB_BUCKETS; h < h1; h++)
        for (uint32_t i = heads[h] + 1; i < heads[h + 1]; i++) {
            GEnt t = e[i];
            uint32_t k = i;
            for (; k > heads[h] && gent_less(&t, &e[k - 1]); k--) e[k] = e[k - 1];
            e[k] = t;
        }
}
static uint32_t grid_build_par(Grid* g, const uint32_t* list, uint32_t n, float cell, uint32_t* big) {
    g->cell = cell, g->nents = 0;
    GridJob j = { g, list, n, 0, 0, 1.0f / cell, NULL, NULL, NULL };
    if (!re_vm_ensure(&P.gcnt, ((size_t)n + 2u) * 4u)) return 0;
    j.cnt = (uint32_t*)P.gcnt.base;
    re_parallel_for((int)((n + GB_BODIES - 1) / GB_BODIES), gb_count_fn, &j);
    uint32_t ne = 0, nb = 0;
    for (uint32_t i = 0; i < n; i++) { // (the offsets; the big ones apart)
        uint32_t c = j.cnt[i];
        j.cnt[i] = ne;
        if (c == ~0u) big[nb++] = list[i];
        else ne += c;
    }
    j.cnt[n] = ne;
    uint32_t nbk = 1024;
    while (nbk < 2 * ne && nbk < GRID_BUCKETS_MAX) nbk <<= 1;
    g->nbuckets = j.nbk = nbk, j.nents = ne;
    uint32_t nbb = (nbk + GB_BUCKETS - 1) / GB_BUCKETS;
    if (!re_vm_ensure(&g->heads, ((size_t)nbk + 2u) * 4u) || !re_vm_ensure(&g->ents, ((size_t)ne + 1u) * sizeof(GEnt)) ||
        !re_vm_ensure(&g->tmp, ((size_t)ne + 1u) * sizeof(GEnt)) || !re_vm_ensure(&P.gcur, ((size_t)nbk + nbb + 2u) * 4u))
        return nb;
    j.cur = (uint32_t*)P.gcur.base, j.bsum = j.cur + nbk;
    memset(g->heads.base, 0, ((size_t)nbk + 2u) * 4u);
    re_parallel_for((int)((n + GB_BODIES - 1) / GB_BODIES), gb_fill_fn, &j);
    re_parallel_for((int)nbb, gb_bsum_fn, &j);
    for (uint32_t b = 0, s = 0; b < nbb; b++) {
        uint32_t c = j.bsum[b];
        j.bsum[b] = s, s += c;
    }
    re_parallel_for((int)nbb, gb_prefix_fn, &j);
    re_parallel_for((int)((ne + GB_ENTS - 1) / GB_ENTS), gb_scatter_fn, &j);
    re_parallel_for((int)nbb, gb_sort_fn, &j);
    g->nents = ne;
    return nb;
}

static bool joined(uint32_t a, int32_t b) { // two bodies a constraint holds without collision
    const Body* bd = bodies();
    if (b < 0 || !bd[a].njoint || !bd[b].njoint) return false;
    const Joint* j = joints();
    for (uint32_t i = 0; i < P.njoints; i++)
        if (!j[i].c->keep_collision && ((j[i].a == (int32_t)a && j[i].b == b) || (j[i].a == b && j[i].b == (int32_t)a))) return true;
    return false;
}

static uint32_t emit(Pair* out, uint32_t n, uint32_t a, int32_t b) {
    if (joined(a, b)) return n;
    if (out) {
        Pair* p = &out[n];
        p->a = (int32_t)a, p->b = b, p->n = 0;
        p->cap = b < 0 && statics()[-(b + 1)].g.kind == SHAPE_TRIS ? 8 : 1;
    }
    return n + 1;
}

// The awake bodies of one bucket against each other (a pair once: in the cell of the larger of their lower corners)
static uint32_t bucket_pairs(uint32_t h, Pair* out) {
    const Grid* g = &P.awake_grid;
    const uint32_t* heads = (const uint32_t*)g->heads.base;
    const GEnt* e = (const GEnt*)g->ents.base;
    float ic = 1.0f / g->cell;
    uint32_t n = 0;
    for (uint32_t i = heads[h]; i < heads[h + 1]; i++)
        for (uint32_t j = i + 1; j < heads[h + 1]; j++) {
            if (e[i].x != e[j].x || e[i].y != e[j].y || e[i].z != e[j].z) continue;
            const GEnt* A = &e[i];
            const GEnt* B = &e[j];
            if (!overlap(A->mn, A->mx, B->mn, B->mx)) continue;
            if ((int)floorf(fmaxf(A->mn[0], B->mn[0]) * ic) != e[i].x || (int)floorf(fmaxf(A->mn[1], B->mn[1]) * ic) != e[i].y ||
                (int)floorf(fmaxf(A->mn[2], B->mn[2]) * ic) != e[i].z)
                continue;
            uint32_t a = e[i].body < e[j].body ? e[i].body : e[j].body, b = e[i].body ^ e[j].body ^ a;
            n = emit(out, n, a, (int32_t)b);
        }
    return n;
}

// An awake body against the sleeping ones, the level's colliders and the Characters
static uint32_t body_pairs(uint32_t i, Pair* out) {
    const Body* bd = bodies();
    const Body* A = &bd[i];
    uint32_t n = 0;
    const Grid* g = &P.sleep_grid;
    if (g->nents) {
        float ic = 1.0f / g->cell;
        int lo[3], hi[3];
        cell_range(A->mn, A->mx, ic, lo, hi);
        const uint32_t* heads = (const uint32_t*)g->heads.base;
        const GEnt* e = (const GEnt*)g->ents.base;
        if ((int64_t)(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) <= 64) {
            for (int x = lo[0]; x <= hi[0]; x++)
                for (int y = lo[1]; y <= hi[1]; y++)
                    for (int z = lo[2]; z <= hi[2]; z++) {
                        uint32_t h = hash3(x, y, z) & (g->nbuckets - 1);
                        for (uint32_t k = heads[h]; k < heads[h + 1]; k++) {
                            if (e[k].x != x || e[k].y != y || e[k].z != z) continue;
                            const GEnt* B = &e[k];
                            if (!overlap(A->mn, A->mx, B->mn, B->mx)) continue;
                            if ((int)floorf(fmaxf(A->mn[0], B->mn[0]) * ic) != x || (int)floorf(fmaxf(A->mn[1], B->mn[1]) * ic) != y ||
                                (int)floorf(fmaxf(A->mn[2], B->mn[2]) * ic) != z)
                                continue;
                            n = emit(out, n, i, (int32_t)e[k].body);
                        }
                    }
        } else // (a body bigger than the grid's cells: every sleeping one)
            for (uint32_t k = 0; k < P.nbodies; k++)
                if (!bd[k].awake && overlap(A->mn, A->mx, bd[k].mn, bd[k].mx)) n = emit(out, n, i, (int32_t)k);
    }
    const uint32_t* sbig = (const uint32_t*)P.sbig.base; // (the sleeping ones too big for their grid)
    for (uint32_t k = 0; k < P.nsbig; k++)
        if (!bd[sbig[k]].awake && overlap(A->mn, A->mx, bd[sbig[k]].mn, bd[sbig[k]].mx)) n = emit(out, n, i, (int32_t)sbig[k]);
    const Static* st = statics();
    const uint32_t* big = (const uint32_t*)P.big.base;
    for (uint32_t k = 0; k < P.nbig; k++)
        if (overlap(A->mn, A->mx, st[big[k]].mn, st[big[k]].mx)) n = emit(out, n, i, -(int32_t)(big[k] + 1));
    uint32_t nlevel = P.nstatics - P.nkin;
    int x0 = (int)floorf(A->mn[0] / CELL), x1 = (int)floorf(A->mx[0] / CELL), y0 = (int)floorf(A->mn[1] / CELL), y1 = (int)floorf(A->mx[1] / CELL);
    if (nlevel) {
        const uint32_t* cells = (const uint32_t*)P.cells.base;
        const uint32_t* refs = (const uint32_t*)P.refs.base;
        if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) <= 64) {
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++) {
                    uint32_t h = cell_hash(x, y);
                    for (uint32_t r = cells[h]; r < cells[h + 1]; r++) {
                        uint32_t s = refs[r];
                        if (!overlap(A->mn, A->mx, st[s].mn, st[s].mx)) continue;
                        // (a collider in several cells: once, in the cell of the larger lower corner)
                        if ((int)floorf(fmaxf(A->mn[0], st[s].mn[0]) / CELL) != x || (int)floorf(fmaxf(A->mn[1], st[s].mn[1]) / CELL) != y) continue;
                        n = emit(out, n, i, -(int32_t)(s + 1));
                    }
                }
        } else // (a body over 64 cells: every collider of the grid)
            for (uint32_t s = 0; s < nlevel; s++) {
                bool isbig = false;
                for (uint32_t k = 0; k < P.nbig && !isbig; k++) isbig = big[k] == s;
                if (!isbig && overlap(A->mn, A->mx, st[s].mn, st[s].mx)) n = emit(out, n, i, -(int32_t)(s + 1));
            }
    }
    for (uint32_t s = nlevel; s < P.nstatics; s++) // the Characters
        if (overlap(A->mn, A->mx, st[s].mn, st[s].mx)) n = emit(out, n, i, -(int32_t)(s + 1));
    return n;
}

// The awake bodies too big for the grid: against every other body (the big ones among them once)
static uint32_t big_pairs(uint32_t bi, Pair* out) {
    const Body* bd = bodies();
    const uint32_t* bigb = (const uint32_t*)P.bigb.base;
    uint32_t i = bigb[bi], n = 0;
    for (uint32_t k = 0; k < P.nbodies; k++) {
        if (k == i || !overlap(bd[i].mn, bd[i].mx, bd[k].mn, bd[k].mx) || !bd[k].awake) continue;
        bool kbig = false;
        for (uint32_t q = 0; q < P.nbigb && !kbig; q++) kbig = bigb[q] == k;
        if (kbig && k < i) continue; // (two big ones: once)
        n = emit(out, n, i < k ? i : k, (int32_t)(i < k ? k : i));
    }
    return n;
}

// The blocks of pair finding: the awake grid's buckets, the awake bodies, the big ones (counted, then filled in order)
#define PB_BUCKETS 512u
#define PB_BODIES 64u
typedef struct PairJob { uint32_t nb1, nb2, nb3; bool fill; } PairJob;
static uint32_t pair_block(const PairJob* j, uint32_t b, Pair* out) {
    uint32_t n = 0;
    if (b < j->nb1) {
        uint32_t h1 = (b + 1) * PB_BUCKETS < P.awake_grid.nbuckets ? (b + 1) * PB_BUCKETS : P.awake_grid.nbuckets;
        for (uint32_t h = b * PB_BUCKETS; h < h1; h++) n += bucket_pairs(h, out ? out + n : NULL);
    } else if (b < j->nb1 + j->nb2) {
        uint32_t k0 = (b - j->nb1) * PB_BODIES, k1 = k0 + PB_BODIES < P.nawake ? k0 + PB_BODIES : P.nawake;
        const uint32_t* aw = (const uint32_t*)P.awake.base;
        for (uint32_t k = k0; k < k1; k++) n += body_pairs(aw[k], out ? out + n : NULL);
    } else
        for (uint32_t k = 0; k < P.nbigb; k++) n += big_pairs(k, out ? out + n : NULL);
    return n;
}
static void pair_count_fn(void* arg, int b) {
    const PairJob* j = arg;
    ((uint32_t*)P.blk.base)[b] = pair_block(j, (uint32_t)b, NULL);
}
static void pair_fill_fn(void* arg, int b) {
    const PairJob* j = arg;
    Pair* out = (Pair*)P.pairs.base + ((uint32_t*)P.blk.base)[b];
    pair_block(j, (uint32_t)b, out);
}

static void broadphase(void) {
    Body* bd = bodies();
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    // the grids' cells: twice the bodies' mean size (at least 50 cm)
    double ext = 0.0;
    for (uint32_t k = 0; k < P.nawake; k++) ext += bd[aw[k]].extent;
    float cell = P.nawake ? fminf(fmaxf((float)(4.0 * ext / P.nawake), 50.0f), 1000.0f) : 200.0f;
    double g0 = re_time_seconds();
    P.nbigb = grid_build_par(&P.awake_grid, aw, P.nawake, cell, (uint32_t*)P.bigb.base);
    if (P.sleep_version != P.sleep_grid_version) { // the sleeping ones: when one slept or woke (their own cell size: the
        uint32_t* list = (uint32_t*)P.mint.base, ns = 0; // awake ones' changes every step, which made this every step)
        double sext = 0.0;
        for (uint32_t i = 0; i < P.nbodies; i++)
            if (!bd[i].awake) list[ns++] = i, sext += bd[i].extent;
        float scell = ns ? fminf(fmaxf((float)(4.0 * sext / ns), 50.0f), 1000.0f) : 200.0f;
        P.nsbig = grid_build_par(&P.sleep_grid, list, ns, scell, (uint32_t*)P.sbig.base);
        P.sleep_grid_version = P.sleep_version;
    }
    double g1 = re_time_seconds();
    P.t_phase[10] += (g1 - g0) * 1000.0;
    PairJob job = { (P.awake_grid.nbuckets + PB_BUCKETS - 1) / PB_BUCKETS, (P.nawake + PB_BODIES - 1) / PB_BODIES, P.nbigb ? 1u : 0u, false };
    uint32_t nblk = job.nb1 + job.nb2 + job.nb3;
    if (!re_vm_ensure(&P.blk, ((size_t)nblk + 1u) * 4u)) return;
    re_parallel_for((int)nblk, pair_count_fn, &job);
    uint32_t* blk = (uint32_t*)P.blk.base, total = 0;
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t c = blk[b];
        blk[b] = total, total += c;
    }
    if (total > MAX_PAIRS) total = MAX_PAIRS; // (never in practice: 2 million pairs)
    if (!re_vm_ensure(&P.pairs, ((size_t)total + 1u) * sizeof(Pair))) return;
    re_parallel_for((int)nblk, pair_fill_fn, &job);
    P.npairs = total, P.stats.pairs = total;
    P.t_phase[11] += (re_time_seconds() - g1) * 1000.0;
}


// --- Narrowphase (in parallel: each pair writes its own manifold slots) ---

static void atomic_max(float* f, float v) { // (positive floats order as their bits)
    if (!(v > 0.0f)) return;
    volatile int32_t* p = (volatile int32_t*)f;
    int32_t nv;
    memcpy(&nv, &v, 4);
    for (int32_t old = re_atomic_load(p); old < nv; old = re_atomic_load(p))
        if (re_atomic_cas(p, old, nv)) return;
}

static void narrow_fn(void* arg, int blk) {
    (void)arg;
    Pair* pairs = (Pair*)P.pairs.base;
    Man* mans = (Man*)P.mans.base;
    const Body* bd = bodies();
    const Static* st = statics();
    static const float zero[3] = { 0, 0, 0 };
    uint32_t i1 = ((uint32_t)blk + 1) * BLOCK_PAIRS < P.npairs ? ((uint32_t)blk + 1) * BLOCK_PAIRS : P.npairs;
    for (uint32_t i = (uint32_t)blk * BLOCK_PAIRS; i < i1; i++) {
        Pair* p = &pairs[i];
        const Body* A = &bd[p->a];
        Gen G;
        G.n = 0;
        bool tris = false;
        if (p->b >= 0) {
            const Body* B = &bd[p->b];
            collide_pair(&G, A, B->p, B->R, &B->g, B->awake ? B->v : zero, NULL);
        } else {
            const Static* S = &st[-(p->b + 1)];
            tris = S->g.kind == SHAPE_TRIS;
            collide_pair(&G, A, S->c, S->R, &S->g, S->v, S);
        }
        p->n = (uint8_t)gen_manifolds(&G, tris, &mans[p->first], p->cap);
        for (uint32_t k = 0; k < p->n; k++) ((uint8_t*)P.mnp.base)[p->first + k] = (uint8_t)mans[p->first + k].np; // (the colors read these)
        // touching (a sleeping one wakes), and the hit's speed (fracture): the bodies' velocities at the first points
        p->touch = 0, p->impact = 0.0f;
        for (uint32_t k = 0; k < p->n; k++) {
            const Man* m = &mans[p->first + k];
            for (uint32_t q = 0; q < m->np; q++) p->touch |= m->d[q] > -SLOP;
            float vb[3] = { 0, 0, 0 }, r[3], c[3], vn = 0.0f;
            if (p->b >= 0 && bd[p->b].awake) {
                const Body* B = &bd[p->b];
                for (int q = 0; q < 3; q++) r[q] = m->p[0][q] - B->p[q];
                cross3(B->w, r, c);
                for (int q = 0; q < 3; q++) vb[q] = B->v[q] + c[q];
            } else if (p->b < 0)
                memcpy(vb, st[-(p->b + 1)].v, 12);
            for (int q = 0; q < 3; q++) r[q] = m->p[0][q] - A->p[q];
            cross3(A->w, r, c);
            for (int q = 0; q < 3; q++) vn += (A->v[q] + c[q] - vb[q]) * m->n[q];
            if (-vn > p->impact) p->impact = -vn;
        }
        if (!p->n) continue;
        // the bodies told (several workers may write a body: the same 1s, an atomic max and count)
        Body* WA = (Body*)A;
        Body* WB = p->b >= 0 ? (Body*)&bd[p->b] : NULL;
        if (p->touch) {
            WA->touching = 1;
            if (WB) {
                WB->touching = 1;
                if (!WB->awake) WB->wake_next = 1; // (touched: its island wakes for the next step)
            }
        }
        atomic_max(&WA->impact, p->impact);
        re_atomic_add((volatile int32_t*)&WA->contacts, p->n);
        if (WB) atomic_max(&WB->impact, p->impact), re_atomic_add((volatile int32_t*)&WB->contacts, p->n);
    }
}

static void narrowphase(void) {
    Pair* pairs = (Pair*)P.pairs.base;
    Static* st = statics();
    uint32_t slots = 0;
    for (uint32_t i = 0; i < P.npairs; i++) {
        Pair* p = &pairs[i];
        p->first = slots, slots += p->cap;
        if (p->b < 0) { // a static's hull: now, the first time a body comes near (a cache: once a mesh; one thread)
            Geom* g = &st[-(p->b + 1)].g;
            if (g->kind == SHAPE_HULL && !g->hull) {
                const PhysHull* h = hull_for(g->mesh_id);
                if (!h || h->boxlike) g->kind = SHAPE_BOX;
                else g->hull = h, hull_skin(g);
            }
        }
    }
    if (!re_vm_ensure(&P.mans, ((size_t)slots + 1u) * sizeof(Man)) || !re_vm_ensure(&P.mdest, ((size_t)slots + 1u) * 16u) ||
        !re_vm_ensure(&P.mnp, (size_t)slots + 64u)) {
        P.npairs = 0;
        return;
    }
    re_parallel_for((int)((P.npairs + BLOCK_PAIRS - 1) / BLOCK_PAIRS), narrow_fn, NULL);
}

// --- The contacts: woken neighbors, the impacts, the colors (one serial pass), then made in parallel ---

typedef struct MDest { uint32_t c, pt, rec, color; } MDest;

static uint32_t key_of(int32_t b) { return b >= 0 ? bodies()[b].e : ~(uint32_t)(-(b + 1)); }

static void color_contacts(void) {
    Pair* pairs = (Pair*)P.pairs.base;
    const uint8_t* mnp = (const uint8_t*)P.mnp.base;
    MDest* md = (MDest*)P.mdest.base;
    Body* bd = bodies();
    uint32_t* mask = (uint32_t*)P.cmask.base;
    const uint8_t* dyn = (const uint8_t*)P.dynb.base;
    uint32_t count[MAX_COLORS + 1] = { 0 }, pcount[MAX_COLORS + 1] = { 0 }, nrec = 0, nm = 0;
    uint32_t bynp[MAX_COLORS + 1][5] = { { 0 } }; // (within a color: the contacts by their points, 4 first: the wides' loops)
    for (uint32_t i = 0; i < P.npairs; i++) {
        const Pair* p = &pairs[i];
        bool bdyn = p->b >= 0 && dyn[p->b];
        for (uint32_t k = 0; k < p->n; k++) {
            uint32_t mp = mnp[p->first + k];
            MDest* d = &md[p->first + k];
            // the color: the first one neither awake body has yet (a wall's side counts for nothing)
            uint32_t ma = mask[p->a], mb = bdyn ? mask[p->b] : 0u;
            uint32_t freec = ~(ma | mb) & ((1u << MAX_COLORS) - 1u), col = freec ? ctz32(freec) : COLOR_OVERFLOW;
            if (col < MAX_COLORS) {
                mask[p->a] |= 1u << col;
                if (bdyn) mask[p->b] |= 1u << col;
            }
            d->color = col, d->rec = nrec;
            nrec += mp, nm++;
            count[col]++, pcount[col] += mp, bynp[col][mp < 4 ? mp : 4]++;
        }
    }
    uint32_t cf = 0, pf = 0, wf = 0;
    for (uint32_t c = 0; c <= MAX_COLORS; c++) {
        P.color_first[c] = cf, P.color_pfirst[c] = pf, P.color_wfirst[c] = wf;
        cf += count[c], pf += pcount[c];
        if (c < MAX_COLORS) wf += (count[c] + FW_LANES - 1) / FW_LANES;
    }
    P.color_first[MAX_COLORS + 1] = cf, P.color_pfirst[MAX_COLORS + 1] = pf, P.color_wfirst[MAX_COLORS] = P.color_wfirst[MAX_COLORS + 1] = wf;
    P.nwides = wf;
    uint32_t cur[MAX_COLORS + 1][5], pcur[MAX_COLORS + 1];
    for (uint32_t c = 0; c <= MAX_COLORS; c++) {
        uint32_t at = P.color_first[c];
        for (int np = 4; np >= 0; np--) cur[c][np] = at, at += bynp[c][np];
        pcur[c] = P.color_pfirst[c];
    }
    for (uint32_t i = 0; i < P.npairs; i++)
        for (uint32_t k = 0; k < pairs[i].n; k++) {
            MDest* d = &md[pairs[i].first + k];
            uint32_t np = mnp[pairs[i].first + k];
            d->c = cur[d->color][np < 4 ? np : 4]++, d->pt = pcur[d->color];
            pcur[d->color] += np;
        }
    P.ncontacts = nm, P.npoints = pf, P.nrec = nrec;
    uint32_t used = 0;
    for (uint32_t c = 0; c < MAX_COLORS; c++) used += count[c] != 0;
    P.stats.colors = used > P.stats.colors ? used : P.stats.colors, P.stats.overflow = count[COLOR_OVERFLOW];
    // the touched sleepers wake now (this step they were walls: their slots stay still)
    for (uint32_t i = 0; i < P.nbodies; i++)
        if (bd[i].wake_next) bd[i].wake_next = 0, wake_island(i);
}

// The friction and the bounce of a pair (the higher combine mode wins; without materials: the old rules)
static float combine(float a, float b, uint32_t ma, uint32_t mb, bool friction) {
    if (ma == PCOMB_COUNT && mb == PCOMB_COUNT) return friction ? sqrtf(a * b) : fmaxf(a, b);
    uint32_t m = ma == PCOMB_COUNT ? mb : mb == PCOMB_COUNT ? ma : (ma > mb ? ma : mb);
    switch (m) {
    case PCOMB_MIN: return fminf(a, b);
    case PCOMB_MULTIPLY: return a * b;
    case PCOMB_MAX: return fmaxf(a, b);
    default: return 0.5f * (a + b);
    }
}

static const Slot* cache_slot(int prev, uint32_t ka, uint32_t kb) {
    if (!P.tab_size[prev]) return NULL;
    const Slot* tab = (const Slot*)P.tab[prev].base;
    uint32_t m = P.tab_size[prev] - 1;
    uint32_t h = (ka * 0x9E3779B1u ^ (kb + 0x7F4A7C15u) * 0x85EBCA77u);
    for (h = (h ^ (h >> 16)) & m;; h = (h + 1) & m) {
        if (!tab[h].ka) return NULL;
        if (tab[h].ka == ka && tab[h].kb == kb) return &tab[h];
    }
}

static float eff(const State* A, const State* B, const float ra[3], const float rb[3], const float n[3]) {
    float c[3], t[3], k = A->im + B->im;
    cross3(ra, n, c), imul(A->I, c, t), k += dot3(c, t);
    cross3(rb, n, c), imul(B->I, c, t), k += dot3(c, t);
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

static void prepare_fn(void* arg, int blk) {
    (void)arg;
    const Pair* pairs = (const Pair*)P.pairs.base;
    const Man* mans = (const Man*)P.mans.base;
    const MDest* md = (const MDest*)P.mdest.base;
    Contact* C = (Contact*)P.contacts.base;
    Pt* PT = (Pt*)P.points.base;
    const Body* bd = bodies();
    const State* S = states();
    const Static* st = statics();
    uint32_t nlevel = P.nstatics - P.nkin, prev = (uint32_t)P.cur ^ 1u;
    const Rec* rec = (const Rec*)P.rec[prev].base;
    uint32_t i1 = ((uint32_t)blk + 1) * BLOCK_PAIRS < P.npairs ? ((uint32_t)blk + 1) * BLOCK_PAIRS : P.npairs;
    for (uint32_t i = (uint32_t)blk * BLOCK_PAIRS; i < i1; i++) {
        const Pair* p = &pairs[i];
        if (!p->n) continue;
        const Body* A = &bd[p->a];
        const Body* B = p->b >= 0 ? &bd[p->b] : NULL;
        uint32_t sb = B ? (uint32_t)p->b + 1u : (uint32_t)(-(p->b + 1)) >= nlevel ? P.nbodies + 1u + (uint32_t)(-(p->b + 1)) - nlevel : 0u;
        const State* SA = &S[p->a + 1];
        const State* SB = &S[sb];
        float mu = B ? combine(A->friction, B->friction, A->fcomb, B->fcomb, true) : A->friction;
        float e = B ? combine(A->restitution, B->restitution, A->rcomb, B->rcomb, false) : A->restitution;
        const Slot* slot = cache_slot((int)prev, A->e, key_of(p->b));
        float bp[3];
        if (B) memcpy(bp, B->p, 12);
        else memcpy(bp, st[-(p->b + 1)].c, 12);
        bool arm_b = B || sb; // (a body, or a Character: its arm from its middle)
        for (uint32_t k = 0; k < p->n; k++) {
            const Man* m = &mans[p->first + k];
            const MDest* d = &md[p->first + k];
            Contact* c = &C[d->c];
            memset(c, 0, sizeof *c);
            c->sa = (uint32_t)p->a + 1u, c->sb = sb, c->first = d->pt, c->np = m->np;
            c->a = p->a, c->b = p->b, c->rec = d->rec, c->soft = !SB->dyn;
            memcpy(c->n, m->n, 12);
            c->mu = mu, c->e = e;
            perp(m->n, c->t1, c->t2);
            const Rec* r0 = NULL; // (the cached point nearest this manifold's first: its friction)
            float rr = 0.0f;
            for (uint32_t q = 0; q < m->np; q++) {
                Pt* t = &PT[d->pt + q];
                memset(t, 0, sizeof *t);
                for (int k2 = 0; k2 < 3; k2++) t->ra[k2] = m->p[q][k2] - A->p[k2], t->rb[k2] = arm_b ? m->p[q][k2] - bp[k2] : 0.0f;
                for (int k2 = 0; k2 < 3; k2++) c->cra[k2] += t->ra[k2] / (float)m->np, c->crb[k2] += t->rb[k2] / (float)m->np;
                float sep = -m->d[q], drr[3] = { t->ra[0] - t->rb[0], t->ra[1] - t->rb[1], t->ra[2] - t->rb[2] };
                t->adj = sep - dot3(drr, m->n);
                t->mn = eff(SA, SB, t->ra, t->rb, m->n);
                if (m->d[q] > -SLOP) c->touch = 1;
                // the approach speed (the bounce after the substeps)
                float wa[3], wb[3], vr[3];
                cross3(SA->w, t->ra, wa), cross3(SB->w, t->rb, wb);
                for (int k2 = 0; k2 < 3; k2++) vr[k2] = SA->v[k2] + wa[k2] - SB->v[k2] - wb[k2];
                t->relv = dot3(vr, m->n);
                // warm start: last step's impulse at the nearest point (the normal turned less than 25 degrees)
                if (slot) {
                    float lp[3], best = 4.0f * 4.0f;
                    to_local(A->R, t->ra, lp);
                    const Rec* r = NULL;
                    for (uint32_t s = slot->first; s < slot->first + slot->count; s++) {
                        float dd[3] = { rec[s].lp[0] - lp[0], rec[s].lp[1] - lp[1], rec[s].lp[2] - lp[2] };
                        if (dot3(dd, dd) < best) best = dot3(dd, dd), r = &rec[s];
                    }
                    if (r && dot3(r->n, m->n) > 0.9f) {
                        t->jn = r->jn;
                        if (!r0) r0 = r;
                    }
                }
            }
            // the patch's friction: at the points' middle, two directions and the twist around the normal (a disc of
            // the friction times the normal impulses; a pile doesn't creep the way points solved one by one walk)
            for (uint32_t q = 0; q < m->np; q++) {
                const Pt* t = &PT[d->pt + q];
                float dd[3] = { t->ra[0] - c->cra[0], t->ra[1] - c->cra[1], t->ra[2] - c->cra[2] };
                rr += len3(dd) / (float)m->np;
            }
            c->twr = fmaxf(rr, 0.5f);
            c->mt1 = eff(SA, SB, c->cra, c->crb, c->t1), c->mt2 = eff(SA, SB, c->cra, c->crb, c->t2);
            float tn[3], k = 0.0f;
            imul(SA->I, m->n, tn), k += dot3(tn, m->n);
            imul(SB->I, m->n, tn), k += dot3(tn, m->n);
            c->mtw = k > 1e-12f ? 1.0f / k : 0.0f;
            c->slip = 1;
            if (r0) {
                c->jt1 = dot3(r0->ft, c->t1), c->jt2 = dot3(r0->ft, c->t2), c->jtw = r0->jtw;
                // static friction: its anchors, where they are now (apart more than 2 cm: let go; a Character's: none)
                if (sb == 0 || B) {
                    float wa[3], wb[3], dd[3];
                    to_world(A->R, r0->aa, wa);
                    if (B) to_world(B->R, r0->ab, wb);
                    for (int k2 = 0; k2 < 3; k2++) wa[k2] += A->p[k2], wb[k2] = B ? wb[k2] + bp[k2] : r0->ab[k2], dd[k2] = wa[k2] - wb[k2];
                    float d1 = dot3(dd, c->t1), d2 = dot3(dd, c->t2);
                    if (d1 * d1 + d2 * d2 < 2.0f * 2.0f) {
                        c->drift[0] = d1, c->drift[1] = d2, c->slip = 0;
                        memcpy(c->aa, r0->aa, 12), memcpy(c->ab, r0->ab, 12);
                    }
                }
            }
        }
    }
}

// --- The solver's kernels (a contact: its points' normal impulses, its patch's friction) ---

static void apply_pair(State* A, State* B, float va[3], float wa[3], float vb[3], float wb[3], const float ra[3], const float rb[3],
                       const float P3[3]) {
    float rc[3], dw[3];
    for (int q = 0; q < 3; q++) va[q] += A->im * P3[q], vb[q] -= B->im * P3[q];
    cross3(ra, P3, rc), imul(A->I, rc, dw);
    for (int q = 0; q < 3; q++) wa[q] += dw[q];
    cross3(rb, P3, rc), imul(B->I, rc, dw);
    for (int q = 0; q < 3; q++) wb[q] -= dw[q];
}

static void solve_contact(Contact* c, bool bias, bool flip) {
    State* A = &states()[c->sa];
    State* B = &states()[c->sb];
    const Delta* dA = &deltas()[c->sa];
    const Delta* dB = &deltas()[c->sb];
    Pt* pt = (Pt*)P.points.base + c->first;
    float va[3], wa[3], vb[3], wb[3];
    memcpy(va, A->v, 12), memcpy(wa, A->w, 12), memcpy(vb, B->v, 12), memcpy(wb, B->w, 12);
    const Soft s = c->soft ? P.soft_static : P.soft_contact;
    const float* n = c->n;
    float inv_h = P.inv_h, total = 0.0f;
    for (uint32_t kk = 0; kk < c->np; kk++) {
        Pt* t = &pt[bias != flip ? kk : c->np - 1 - kk]; // (forward, then back in the relax, the other way the next substep: a fixed order pushes one side first)
        // the separation now (from the bodies' motion in the substeps: the anchors turned with them)
        float pra[3], prb[3], d[3];
        qrot(dA->dq, t->ra, pra), qrot(dB->dq, t->rb, prb);
        for (int q = 0; q < 3; q++) d[q] = dA->dp[q] - dB->dp[q] + pra[q] - prb[q];
        float sep = dot3(d, n) + t->adj;
        float x[3], y[3], vr[3];
        cross3(wa, t->ra, x), cross3(wb, t->rb, y);
        for (int q = 0; q < 3; q++) vr[q] = va[q] + x[q] - vb[q] - y[q];
        float vn = dot3(vr, n), bv = 0.0f, ms = 1.0f, is = 0.0f;
        if (sep > 0.0f) bv = sep * inv_h; // (speculative: it may close the gap, no more)
        else if (bias) bv = fmaxf(s.rate * sep, -MAX_DEPENETRATION), ms = s.mass, is = s.imp;
        float imp = -t->mn * ms * (vn + bv) - is * t->jn;
        float nj = fmaxf(t->jn + imp, 0.0f);
        imp = nj - t->jn, t->jn = nj, total += nj;
        if (imp > t->jmax) t->jmax = imp;
        float P3[3] = { n[0] * imp, n[1] * imp, n[2] * imp };
        apply_pair(A, B, va, wa, vb, wb, t->ra, t->rb, P3);
    }
    // the patch's friction; while it holds, a soft spring pulls its anchors back together (static friction)
    {
        float x[3], y[3], vr[3];
        cross3(wa, c->cra, x), cross3(wb, c->crb, y);
        for (int q = 0; q < 3; q++) vr[q] = va[q] + x[q] - vb[q] - y[q];
        float b1 = 0.0f, b2 = 0.0f, fm = 1.0f, fi = 0.0f, dr2 = 0.0f;
        if (bias && !c->slip) {
            float pra[3], prb[3], e[3];
            qrot(dA->dq, c->cra, pra), qrot(dB->dq, c->crb, prb);
            for (int q = 0; q < 3; q++) e[q] = dA->dp[q] - dB->dp[q] + pra[q] - prb[q] - c->cra[q] + c->crb[q];
            float e1 = c->drift[0] + dot3(e, c->t1), e2 = c->drift[1] + dot3(e, c->t2);
            b1 = s.rate * e1, b2 = s.rate * e2, fm = s.mass, fi = s.imp, dr2 = e1 * e1 + e2 * e2;
        }
        float n1 = c->jt1 - c->mt1 * fm * (dot3(vr, c->t1) + b1) - fi * c->jt1, n2 = c->jt2 - c->mt2 * fm * (dot3(vr, c->t2) + b2) - fi * c->jt2;
        float lim = c->mu * total, l2 = n1 * n1 + n2 * n2;
        if (l2 > lim * lim) {
            float f = lim / sqrtf(l2);
            n1 *= f, n2 *= f;
            if (bias && dr2 > 0.25f) c->slip = 1; // (sliding past its anchors by more than 5 mm: new anchors)
        }
        float d1 = n1 - c->jt1, d2 = n2 - c->jt2;
        c->jt1 = n1, c->jt2 = n2;
        float P3[3];
        for (int q = 0; q < 3; q++) P3[q] = c->t1[q] * d1 + c->t2[q] * d2;
        apply_pair(A, B, va, wa, vb, wb, c->cra, c->crb, P3);
        // the twist around the normal (a box turning on the floor stops)
        float wr = dot3(wa, n) - dot3(wb, n), tl = lim * c->twr;
        float nt = fminf(fmaxf(c->jtw - c->mtw * wr, -tl), tl), dt = nt - c->jtw, tq[3], dw[3];
        c->jtw = nt;
        for (int q = 0; q < 3; q++) tq[q] = n[q] * dt;
        imul(A->I, tq, dw);
        for (int q = 0; q < 3; q++) wa[q] += dw[q];
        imul(B->I, tq, dw);
        for (int q = 0; q < 3; q++) wb[q] -= dw[q];
    }
    if (A->dyn) memcpy(A->v, va, 12), memcpy(A->w, wa, 12);
    if (B->dyn) memcpy(B->v, vb, 12), memcpy(B->w, wb, 12);
}

static void warm_contact(Contact* c) {
    State* A = &states()[c->sa];
    State* B = &states()[c->sb];
    const Pt* pt = (const Pt*)P.points.base + c->first;
    float va[3], wa[3], vb[3], wb[3];
    memcpy(va, A->v, 12), memcpy(wa, A->w, 12), memcpy(vb, B->v, 12), memcpy(wb, B->w, 12);
    for (uint32_t k = 0; k < c->np; k++) {
        float P3[3] = { c->n[0] * pt[k].jn, c->n[1] * pt[k].jn, c->n[2] * pt[k].jn };
        apply_pair(A, B, va, wa, vb, wb, pt[k].ra, pt[k].rb, P3);
    }
    float P3[3], tq[3], dw[3];
    for (int q = 0; q < 3; q++) P3[q] = c->t1[q] * c->jt1 + c->t2[q] * c->jt2, tq[q] = c->n[q] * c->jtw;
    apply_pair(A, B, va, wa, vb, wb, c->cra, c->crb, P3);
    imul(A->I, tq, dw);
    for (int q = 0; q < 3; q++) wa[q] += dw[q];
    imul(B->I, tq, dw);
    for (int q = 0; q < 3; q++) wb[q] -= dw[q];
    if (A->dyn) memcpy(A->v, va, 12), memcpy(A->w, wa, 12);
    if (B->dyn) memcpy(B->v, vb, 12), memcpy(B->w, wb, 12);
}

static void restitution_contact(Contact* c) {
    if (c->e <= 0.0f) return;
    State* A = &states()[c->sa];
    State* B = &states()[c->sb];
    Pt* pt = (Pt*)P.points.base + c->first;
    for (uint32_t k = 0; k < c->np; k++) {
        Pt* t = &pt[k];
        if (t->relv > -RESTITUTION_SPEED || t->jmax <= 0.0f) continue;
        float x[3], y[3], vr[3];
        cross3(A->w, t->ra, x), cross3(B->w, t->rb, y);
        for (int q = 0; q < 3; q++) vr[q] = A->v[q] + x[q] - B->v[q] - y[q];
        float imp = -t->mn * (dot3(vr, c->n) + c->e * t->relv), nj = fmaxf(t->jn + imp, 0.0f);
        imp = nj - t->jn, t->jn = nj;
        float P3[3] = { c->n[0] * imp, c->n[1] * imp, c->n[2] * imp }, va[3], wa[3], vb[3], wb[3];
        memcpy(va, A->v, 12), memcpy(wa, A->w, 12), memcpy(vb, B->v, 12), memcpy(wb, B->w, 12);
        apply_pair(A, B, va, wa, vb, wb, t->ra, t->rb, P3);
        if (A->dyn) memcpy(A->v, va, 12), memcpy(A->w, wa, 12);
        if (B->dyn) memcpy(B->v, vb, 12), memcpy(B->w, wb, 12);
    }
}

static void store_contact(const Contact* c) { // (the records in pair order: next step's warm start)
    const Body* A = &bodies()[c->a];
    const Body* B = c->b >= 0 ? &bodies()[c->b] : NULL;
    const Pt* pt = (const Pt*)P.points.base + c->first;
    Rec* r = (Rec*)P.rec[P.cur].base + c->rec;
    float ft[3], aa[3], ab[3];
    for (int q = 0; q < 3; q++) ft[q] = c->t1[q] * c->jt1 + c->t2[q] * c->jt2;
    if (!c->slip) memcpy(aa, c->aa, 12), memcpy(ab, c->ab, 12); // (held: the same anchors)
    else { // (slid or new: the anchors at the patch's middle, on each)
        to_local(A->R, c->cra, aa);
        if (B) to_local(B->R, c->crb, ab);
        else
            for (int q = 0; q < 3; q++) ab[q] = A->p[q] + c->cra[q];
    }
    for (uint32_t k = 0; k < c->np; k++) {
        to_local(A->R, pt[k].ra, r[k].lp);
        r[k].jn = pt[k].jn, r[k].jtw = c->jtw;
        memcpy(r[k].ft, ft, 12), memcpy(r[k].n, c->n, 12), memcpy(r[k].aa, aa, 12), memcpy(r[k].ab, ab, 12);
    }
}

// --- The wide solver (2026-11-19, the Q8 leftover): the contacts of one color 8 at a time (AVX2), the same kernels as
// above lane by lane. A color shares no awake body, so its lanes never touch the same body and the order inside a color
// doesn't matter: the result is the scalar one (rounding apart). The bodies are read as 8 rows of 32 bytes transposed
// (State: v im w dyn | I; Delta: dp pad dq) and written back the same way, only the awake lanes. Within a color the
// contacts go by their points' count (4 first), so a wide's loop runs to the most points of its 8. The overflow color
// (bodies shared) stays scalar, one worker in order ---
typedef struct WPt { float ra[3][FW_LANES], rb[3][FW_LANES], adj[FW_LANES], mn[FW_LANES], jn[FW_LANES], jmax[FW_LANES], relv[FW_LANES]; } WPt;
typedef struct Wide {
    int32_t sa[FW_LANES], sb[FW_LANES];
    float n[3][FW_LANES], t1[3][FW_LANES], t2[3][FW_LANES];
    float mu[FW_LANES], e[FW_LANES], soft[FW_LANES];
    float cra[3][FW_LANES], crb[3][FW_LANES];
    float mt1[FW_LANES], mt2[FW_LANES], mtw[FW_LANES], jt1[FW_LANES], jt2[FW_LANES], jtw[FW_LANES], twr[FW_LANES];
    float drift[2][FW_LANES], slip[FW_LANES];
    uint32_t first, count, np, pad[5]; // its contacts C[first, first + count); the most points of them
    WPt pt[4];
} Wide;
#define WBLOCK 4u // wides a block (32 contacts)
_Static_assert(sizeof(Wide) % 32 == 0 && sizeof(State) == 64 && sizeof(Delta) == 32, "the wide solver's rows: 32-byte aligned");

typedef struct WBody { fw v[3], im, w[3], dyn; } WBody;
static void w_gather(const int32_t* idx, WBody* b, fw I[6], fw dp[3], fw dq[4]) {
    fw r[8];
    fw_gather8(states(), idx, sizeof(State), r);
    b->v[0] = r[0], b->v[1] = r[1], b->v[2] = r[2], b->im = r[3], b->w[0] = r[4], b->w[1] = r[5], b->w[2] = r[6], b->dyn = r[7];
    if (I) {
        fw_gather8((const char*)states() + 32, idx, sizeof(State), r);
        for (int k = 0; k < 6; k++) I[k] = r[k];
    }
    if (dp) {
        fw_gather8(deltas(), idx, sizeof(Delta), r);
        dp[0] = r[0], dp[1] = r[1], dp[2] = r[2], dq[0] = r[4], dq[1] = r[5], dq[2] = r[6], dq[3] = r[7];
    }
}
static void w_scatter(const int32_t* idx, const WBody* b) {
    fw r[8] = { b->v[0], b->v[1], b->v[2], b->im, b->w[0], b->w[1], b->w[2], b->dyn };
    fw_scatter8(states(), idx, sizeof(State), r, fw_nonzero_bits(b->dyn)); // (only the awake lanes: dyn is a uint)
}
static void w_load3(const float s[3][FW_LANES], fw o[3]) { o[0] = fw_load(s[0]), o[1] = fw_load(s[1]), o[2] = fw_load(s[2]); }
static fw w_dot(const fw a[3], const fw b[3]) { return fw_madd(a[0], b[0], fw_madd(a[1], b[1], fw_mul(a[2], b[2]))); }
static void w_cross(const fw a[3], const fw b[3], fw o[3]) {
    fw x = fw_sub(fw_mul(a[1], b[2]), fw_mul(a[2], b[1])), y = fw_sub(fw_mul(a[2], b[0]), fw_mul(a[0], b[2])),
       z = fw_sub(fw_mul(a[0], b[1]), fw_mul(a[1], b[0]));
    o[0] = x, o[1] = y, o[2] = z;
}
static void w_imul(const fw I[6], const fw v[3], fw o[3]) {
    fw x = fw_madd(I[0], v[0], fw_madd(I[3], v[1], fw_mul(I[4], v[2]))), y = fw_madd(I[3], v[0], fw_madd(I[1], v[1], fw_mul(I[5], v[2]))),
       z = fw_madd(I[4], v[0], fw_madd(I[5], v[1], fw_mul(I[2], v[2])));
    o[0] = x, o[1] = y, o[2] = z;
}
static void w_qrot(const fw q[4], const fw v[3], fw o[3]) { // v + 2w (u x v) + 2 u x (u x v)
    fw t[3], c[3], two = fw_set(2.0f);
    w_cross(q, v, t);
    t[0] = fw_mul(t[0], two), t[1] = fw_mul(t[1], two), t[2] = fw_mul(t[2], two);
    w_cross(q, t, c);
    for (int k = 0; k < 3; k++) o[k] = fw_add(fw_madd(q[3], t[k], v[k]), c[k]);
}
// va += ima P, vb -= imb P, wa += IA (ra x P), wb -= IB (rb x P)
static void w_apply(WBody* A, WBody* B, const fw IA[6], const fw IB[6], const fw ra[3], const fw rb[3], const fw P3[3]) {
    fw rc[3], dw[3];
    for (int q = 0; q < 3; q++) A->v[q] = fw_madd(A->im, P3[q], A->v[q]), B->v[q] = fw_nmadd(B->im, P3[q], B->v[q]);
    w_cross(ra, P3, rc), w_imul(IA, rc, dw);
    for (int q = 0; q < 3; q++) A->w[q] = fw_add(A->w[q], dw[q]);
    w_cross(rb, P3, rc), w_imul(IB, rc, dw);
    for (int q = 0; q < 3; q++) B->w[q] = fw_sub(B->w[q], dw[q]);
}
static void w_twist(WBody* A, WBody* B, const fw IA[6], const fw IB[6], const fw n[3], fw dt) {
    fw tq[3] = { fw_mul(n[0], dt), fw_mul(n[1], dt), fw_mul(n[2], dt) }, dw[3];
    w_imul(IA, tq, dw);
    for (int q = 0; q < 3; q++) A->w[q] = fw_add(A->w[q], dw[q]);
    w_imul(IB, tq, dw);
    for (int q = 0; q < 3; q++) B->w[q] = fw_sub(B->w[q], dw[q]);
}

// a wide made from its contacts (once a step, before the substeps)
static void wide_pack(Wide* W, uint32_t first, uint32_t count) {
    const Contact* C = (const Contact*)P.contacts.base;
    const Pt* PT = (const Pt*)P.points.base;
    memset(W, 0, offsetof(Wide, pt));
    W->first = first, W->count = count;
    uint32_t np = 0;
    for (uint32_t l = 0; l < count; l++) np = C[first + l].np > np ? C[first + l].np : np;
    W->np = np;
    memset(W->pt, 0, sizeof(WPt) * np);
    for (uint32_t l = 0; l < count; l++) {
        const Contact* c = &C[first + l];
        W->sa[l] = (int32_t)c->sa, W->sb[l] = (int32_t)c->sb;
        for (int q = 0; q < 3; q++) {
            W->n[q][l] = c->n[q], W->t1[q][l] = c->t1[q], W->t2[q][l] = c->t2[q];
            W->cra[q][l] = c->cra[q], W->crb[q][l] = c->crb[q];
        }
        W->mu[l] = c->mu, W->e[l] = c->e, W->soft[l] = c->soft ? 1.0f : 0.0f;
        W->mt1[l] = c->mt1, W->mt2[l] = c->mt2, W->mtw[l] = c->mtw, W->jt1[l] = c->jt1, W->jt2[l] = c->jt2, W->jtw[l] = c->jtw, W->twr[l] = c->twr;
        W->drift[0][l] = c->drift[0], W->drift[1][l] = c->drift[1], W->slip[l] = c->slip ? 1.0f : 0.0f;
        for (uint32_t k = 0; k < c->np; k++) {
            const Pt* t = &PT[c->first + k];
            WPt* w = &W->pt[k];
            for (int q = 0; q < 3; q++) w->ra[q][l] = t->ra[q], w->rb[q][l] = t->rb[q];
            w->adj[l] = t->adj, w->mn[l] = t->mn, w->jn[l] = t->jn, w->jmax[l] = t->jmax, w->relv[l] = t->relv;
        }
    }
}

static void wide_warm(Wide* W) {
    WBody A, B;
    fw IA[6], IB[6], n[3], ra[3], rb[3], P3[3];
    w_gather(W->sa, &A, IA, NULL, NULL), w_gather(W->sb, &B, IB, NULL, NULL);
    w_load3(W->n, n);
    for (uint32_t k = 0; k < W->np; k++) {
        const WPt* t = &W->pt[k];
        fw jn = fw_load(t->jn);
        w_load3(t->ra, ra), w_load3(t->rb, rb);
        P3[0] = fw_mul(n[0], jn), P3[1] = fw_mul(n[1], jn), P3[2] = fw_mul(n[2], jn);
        w_apply(&A, &B, IA, IB, ra, rb, P3);
    }
    fw t1[3], t2[3], jt1 = fw_load(W->jt1), jt2 = fw_load(W->jt2);
    w_load3(W->t1, t1), w_load3(W->t2, t2), w_load3(W->cra, ra), w_load3(W->crb, rb);
    for (int q = 0; q < 3; q++) P3[q] = fw_madd(t1[q], jt1, fw_mul(t2[q], jt2));
    w_apply(&A, &B, IA, IB, ra, rb, P3);
    w_twist(&A, &B, IA, IB, n, fw_load(W->jtw));
    w_scatter(W->sa, &A), w_scatter(W->sb, &B);
}

static void wide_solve(Wide* W, bool bias, bool flip) {
    WBody A, B;
    fw IA[6], IB[6], dpa[3], dqa[4], dpb[3], dqb[4], n[3], ra[3], rb[3], P3[3];
    w_gather(W->sa, &A, IA, dpa, dqa), w_gather(W->sb, &B, IB, dpb, dqb);
    w_load3(W->n, n);
    const fw zero = fw_zero(), one = fw_set(1.0f);
    fw soft = fw_gt(fw_load(W->soft), zero);
    fw rate = fw_sel(soft, fw_set(P.soft_static.rate), fw_set(P.soft_contact.rate));
    fw smass = fw_sel(soft, fw_set(P.soft_static.mass), fw_set(P.soft_contact.mass));
    fw simp = fw_sel(soft, fw_set(P.soft_static.imp), fw_set(P.soft_contact.imp));
    fw inv_h = fw_set(P.inv_h), maxdep = fw_set(-MAX_DEPENETRATION), total = zero;
    fw dd[3] = { fw_sub(dpa[0], dpb[0]), fw_sub(dpa[1], dpb[1]), fw_sub(dpa[2], dpb[2]) };
    fw dsep = w_dot(dd, n);
    uint32_t np = W->np;
    for (uint32_t kk = 0; kk < np; kk++) {
        WPt* t = &W->pt[bias != flip ? kk : np - 1 - kk]; // (as solve_contact: forward, then back in the relax)
        w_load3(t->ra, ra), w_load3(t->rb, rb);
        fw pra[3], prb[3], d[3];
        w_qrot(dqa, ra, pra), w_qrot(dqb, rb, prb);
        for (int q = 0; q < 3; q++) d[q] = fw_sub(pra[q], prb[q]);
        fw sep = fw_add(fw_add(dsep, w_dot(d, n)), fw_load(t->adj));
        fw x[3], y[3], vr[3];
        w_cross(A.w, ra, x), w_cross(B.w, rb, y);
        for (int q = 0; q < 3; q++) vr[q] = fw_sub(fw_add(A.v[q], x[q]), fw_add(B.v[q], y[q]));
        fw vn = w_dot(vr, n);
        fw spec = fw_gt(sep, zero);
        fw bv, ms, is;
        if (bias) {
            bv = fw_sel(spec, fw_mul(sep, inv_h), fw_max(fw_mul(rate, sep), maxdep));
            ms = fw_sel(spec, one, smass), is = fw_sel(spec, zero, simp);
        } else
            bv = fw_and(spec, fw_mul(sep, inv_h)), ms = one, is = zero;
        fw jn = fw_load(t->jn), mn = fw_load(t->mn);
        fw imp = fw_sub(fw_mul(fw_mul(fw_sub(zero, mn), ms), fw_add(vn, bv)), fw_mul(is, jn));
        fw nj = fw_max(fw_add(jn, imp), zero);
        imp = fw_sub(nj, jn);
        fw_store(t->jn, nj), total = fw_add(total, nj);
        fw_store(t->jmax, fw_max(fw_load(t->jmax), imp));
        P3[0] = fw_mul(n[0], imp), P3[1] = fw_mul(n[1], imp), P3[2] = fw_mul(n[2], imp);
        w_apply(&A, &B, IA, IB, ra, rb, P3);
    }
    // the patch's friction (static friction's anchors while it holds), then the twist
    fw t1[3], t2[3], x[3], y[3], vr[3];
    w_load3(W->t1, t1), w_load3(W->t2, t2), w_load3(W->cra, ra), w_load3(W->crb, rb);
    w_cross(A.w, ra, x), w_cross(B.w, rb, y);
    for (int q = 0; q < 3; q++) vr[q] = fw_sub(fw_add(A.v[q], x[q]), fw_add(B.v[q], y[q]));
    fw b1 = zero, b2 = zero, fm = one, fi = zero, dr2 = zero, hold = zero;
    fw slip = fw_gt(fw_load(W->slip), zero);
    if (bias) {
        hold = fw_andnot(slip, fw_all(1)); // (held: the anchors' spring)
        fw pra[3], prb[3], e[3];
        w_qrot(dqa, ra, pra), w_qrot(dqb, rb, prb);
        for (int q = 0; q < 3; q++) e[q] = fw_sub(fw_add(fw_sub(dpa[q], dpb[q]), fw_sub(pra[q], prb[q])), fw_sub(ra[q], rb[q]));
        fw e1 = fw_add(fw_load(W->drift[0]), w_dot(e, t1)), e2 = fw_add(fw_load(W->drift[1]), w_dot(e, t2));
        b1 = fw_and(hold, fw_mul(rate, e1)), b2 = fw_and(hold, fw_mul(rate, e2));
        fm = fw_sel(hold, smass, one), fi = fw_and(hold, simp);
        dr2 = fw_and(hold, fw_madd(e1, e1, fw_mul(e2, e2)));
    }
    fw jt1 = fw_load(W->jt1), jt2 = fw_load(W->jt2);
    fw n1 = fw_sub(fw_nmadd(fw_mul(fw_load(W->mt1), fm), fw_add(w_dot(vr, t1), b1), jt1), fw_mul(fi, jt1));
    fw n2 = fw_sub(fw_nmadd(fw_mul(fw_load(W->mt2), fm), fw_add(w_dot(vr, t2), b2), jt2), fw_mul(fi, jt2));
    fw lim = fw_mul(fw_load(W->mu), total), l2 = fw_madd(n1, n1, fw_mul(n2, n2));
    fw over = fw_gt(l2, fw_mul(lim, lim));
    fw f = fw_sel(over, fw_div(lim, fw_sqrt(fw_max(l2, fw_set(1e-30f)))), one);
    n1 = fw_mul(n1, f), n2 = fw_mul(n2, f);
    if (bias) { // (sliding past its anchors by more than 5 mm: new anchors)
        fw slid = fw_and(over, fw_gt(dr2, fw_set(0.25f)));
        fw_store(W->slip, fw_sel(fw_or(slip, slid), one, zero));
    }
    fw d1 = fw_sub(n1, jt1), d2 = fw_sub(n2, jt2);
    fw_store(W->jt1, n1), fw_store(W->jt2, n2);
    for (int q = 0; q < 3; q++) P3[q] = fw_madd(t1[q], d1, fw_mul(t2[q], d2));
    w_apply(&A, &B, IA, IB, ra, rb, P3);
    fw wr = fw_sub(w_dot(A.w, n), w_dot(B.w, n)), tl = fw_mul(lim, fw_load(W->twr)), jtw = fw_load(W->jtw);
    fw nt = fw_min(fw_max(fw_nmadd(fw_load(W->mtw), wr, jtw), fw_sub(zero, tl)), tl);
    fw_store(W->jtw, nt);
    w_twist(&A, &B, IA, IB, n, fw_sub(nt, jtw));
    w_scatter(W->sa, &A), w_scatter(W->sb, &B);
}

static void wide_restitution(Wide* W) {
    fw e = fw_load(W->e), zero = fw_zero();
    if (!fw_bits(fw_gt(e, zero))) return;
    WBody A, B;
    fw IA[6], IB[6], n[3], ra[3], rb[3], P3[3];
    w_gather(W->sa, &A, IA, NULL, NULL), w_gather(W->sb, &B, IB, NULL, NULL);
    w_load3(W->n, n);
    fw rs = fw_set(-RESTITUTION_SPEED);
    for (uint32_t k = 0; k < W->np; k++) {
        WPt* t = &W->pt[k];
        fw relv = fw_load(t->relv);
        fw on = fw_and(fw_gt(e, zero), fw_and(fw_le(relv, rs), fw_gt(fw_load(t->jmax), zero)));
        if (!fw_bits(on)) continue;
        w_load3(t->ra, ra), w_load3(t->rb, rb);
        fw x[3], y[3], vr[3];
        w_cross(A.w, ra, x), w_cross(B.w, rb, y);
        for (int q = 0; q < 3; q++) vr[q] = fw_sub(fw_add(A.v[q], x[q]), fw_add(B.v[q], y[q]));
        fw jn = fw_load(t->jn);
        fw imp = fw_mul(fw_sub(zero, fw_load(t->mn)), fw_madd(e, relv, w_dot(vr, n)));
        fw nj = fw_sel(on, fw_max(fw_add(jn, imp), zero), jn);
        imp = fw_sub(nj, jn);
        fw_store(t->jn, nj);
        P3[0] = fw_mul(n[0], imp), P3[1] = fw_mul(n[1], imp), P3[2] = fw_mul(n[2], imp);
        w_apply(&A, &B, IA, IB, ra, rb, P3);
    }
    w_scatter(W->sa, &A), w_scatter(W->sb, &B);
}

// the impulses back into the contacts (the islands, the warm start's records)
static void wide_unpack(const Wide* W) {
    Contact* C = (Contact*)P.contacts.base;
    Pt* PT = (Pt*)P.points.base;
    for (uint32_t l = 0; l < W->count; l++) {
        Contact* c = &C[W->first + l];
        c->jt1 = W->jt1[l], c->jt2 = W->jt2[l], c->jtw = W->jtw[l], c->slip = W->slip[l] > 0.0f;
        for (uint32_t k = 0; k < c->np; k++) PT[c->first + k].jn = W->pt[k].jn[l], PT[c->first + k].jmax = W->pt[k].jmax[l];
    }
}

static void integrate_v(uint32_t i) {
    const IParam* ip = &((const IParam*)P.iparam.base)[i]; // (made when the step began: no Body, no component read here)
    State* s = &states()[i + 1];
    s->v[2] -= ip->g;
    for (int k = 0; k < 3; k++) s->v[k] *= ip->ld, s->w[k] *= ip->ad;
    float vl2 = s->v[0] * s->v[0] + s->v[1] * s->v[1] + s->v[2] * s->v[2];
    if (vl2 > MAX_SPEED * MAX_SPEED) {
        float f = MAX_SPEED / sqrtf(vl2);
        for (int k = 0; k < 3; k++) s->v[k] *= f;
    }
    float wl2 = s->w[0] * s->w[0] + s->w[1] * s->w[1] + s->w[2] * s->w[2];
    if (wl2 > ip->wmax * ip->wmax) {
        float f = ip->wmax / sqrtf(wl2);
        for (int k = 0; k < 3; k++) s->w[k] *= f;
    }
}

static void integrate_p(uint32_t i) {
    const State* s = &states()[i + 1];
    Delta* d = &deltas()[i + 1];
    float h = P.h;
    for (int k = 0; k < 3; k++) d->dp[k] += s->v[k] * h;
    qintegrate(d->dq, s->w, h);
}


// --- Physics Constraints (Unreal's, all six axes): rows of the same soft step ---
// The frame: the constraint actor's pose when play began, kept in each body's space (fa, fb). Linear X, Y, Z are that
// frame's axes on body 1; the angles are body 2's frame seen from body 1's: Twist around X, Swing 1 around Z, Swing 2
// around Y (a twist-swing split). A row is Locked (equal, soft as the joints' 60 Hz), Limited (a one-sided limit,
// speculative inside it: it never overshoots; soft: a spring past it), a Drive (a spring toward a target and its
// velocity, Hz and damping ratio: mass-free) or a Motor (a velocity, its force capped).

static int32_t body_of(Entity e) {
    if (e == ENTITY_NONE) return -1;
    uint32_t k = bmap()[entity_index(e)];
    return k < P.nbodies && bodies()[k].e == e ? (int32_t)k : -1;
}

static void gather_joints(World* w) {
    P.njoints = 0;
    Body* bd = bodies();
    for (uint32_t i = 0; i < P.nbodies; i++) bd[i].njoint = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_CONSTRAINT)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        CConstraint* cs = ecs_arch_column(ar, COMP_CONSTRAINT);
        const CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t i = 0; i < ar->count && P.njoints < MAX_JOINTS; i++) {
            CConstraint* c = &cs[i];
            if (c->broken) continue;
            int32_t a = body_of(c->actor1), b = body_of(c->actor2);
            if (a < 0) a = b, b = -1; // (actor 1 not a body: actor 2 held to where actor 1 is, the world)
            if (a < 0 || a == b) continue;
            if (!re_vm_ensure(&P.joints, (size_t)(P.njoints + 1) * sizeof(Joint))) return;
            Body* A = &bd[a];
            Body* B = b >= 0 ? &bd[b] : NULL;
            if (c->started != 2) { // the frames and anchors from the poses now (Unreal's constraint frames)
                float R[3][3], d[3], qc[4], cq[4];
                rotator_axes(tr[i].rotation, R);
                axes_quat(R, qc);
                for (int k = 0; k < 3; k++) d[k] = tr[i].location[k] - A->p[k];
                to_local(A->R, d, c->la);
                qconj(A->q, cq), qmul(cq, qc, c->fa);
                if (B) {
                    for (int k = 0; k < 3; k++) d[k] = tr[i].location[k] - B->p[k];
                    to_local(B->R, d, c->lb);
                    qconj(B->q, cq), qmul(cq, qc, c->fb);
                } else
                    memcpy(c->lb, tr[i].location, 12), memcpy(c->fb, qc, 16);
                memset(c->acc, 0, sizeof c->acc);
                c->started = 2;
            }
            Joint* j = &joints()[P.njoints++];
            j->a = a, j->b = b, j->c = c, j->nrow = 0;
            j->sa = (uint32_t)a + 1u, j->sb = b >= 0 ? (uint32_t)b + 1u : 0u;
            A->njoint++;
            if (B) B->njoint++;
        }
    }
    P.stats.joints = P.njoints;
}

// The motions of each axis by the type (the 6DOF: its own; a limit of 0 on a Limited axis: Locked)
static void joint_motions(const CConstraint* c, uint32_t lm[3], uint32_t am[3], float ll[3], float al[3]) {
    for (int k = 0; k < 3; k++) lm[k] = am[k] = CMOTION_LOCKED, ll[k] = al[k] = 0.0f;
    switch (c->type) {
    case CONSTRAINT_HINGE: am[0] = c->limit > 0.0f ? CMOTION_LIMITED : CMOTION_FREE, al[0] = c->limit; break;
    case CONSTRAINT_BALL: am[0] = am[1] = am[2] = CMOTION_FREE; break;
    case CONSTRAINT_PRISMATIC: lm[0] = c->limit > 0.0f ? CMOTION_LIMITED : CMOTION_FREE, ll[0] = c->limit; break;
    case CONSTRAINT_CONE:
        am[0] = am[1] = am[2] = CMOTION_LIMITED;
        al[0] = c->twist_limit, al[1] = c->swing1_limit, al[2] = c->swing2_limit;
        break;
    case CONSTRAINT_6DOF:
        for (int k = 0; k < 3; k++) {
            lm[k] = c->lin_motion[k] < CMOTION_COUNT ? c->lin_motion[k] : CMOTION_LOCKED, ll[k] = c->lin_limit;
            am[k] = c->ang_motion[k] < CMOTION_COUNT ? c->ang_motion[k] : CMOTION_LOCKED;
        }
        al[0] = c->twist_limit, al[1] = c->swing1_limit, al[2] = c->swing2_limit;
        break;
    default: break; // fixed
    }
    for (int k = 0; k < 3; k++) {
        if (lm[k] == CMOTION_LIMITED && ll[k] <= 0.0f) lm[k] = CMOTION_LOCKED;
        if (am[k] == CMOTION_LIMITED && al[k] <= 0.0f) am[k] = CMOTION_LOCKED;
    }
}

// A body's pose in the substep (an awake one moved by its delta; asleep or the world: as it is)
static void joint_pose(int32_t b, uint32_t s, float p[3], float q[4]) {
    if (b < 0) {
        memset(p, 0, 12), q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
        return;
    }
    const Body* B = &bodies()[b];
    if (!states()[s].dyn) {
        memcpy(p, B->p, 12), memcpy(q, B->q, 16);
        return;
    }
    const Delta* d = &deltas()[s];
    for (int k = 0; k < 3; k++) p[k] = B->p0[k] + d->dp[k];
    qmul(d->dq, B->q0, q);
    qnorm(q);
}

static void add_jrow(Joint* j, uint32_t kind, uint32_t slot, bool ang, const float d[3], const float ra[3], const float rb[3], float C, float vt,
                     float lo, float hi, Soft s) {
    if (j->nrow >= 16) return;
    JRow* r = &j->row[j->nrow++];
    memcpy(r->d, d, 12), memcpy(r->ra, ra, 12), memcpy(r->rb, rb, 12);
    r->kind = (uint8_t)kind, r->slot = (uint8_t)slot, r->ang = ang;
    r->C = C, r->vt = vt, r->lo = lo, r->hi = hi, r->s = s;
    const State* A = &states()[j->sa];
    const State* B = &states()[j->sb];
    float k, t[3];
    if (ang) {
        imul(j->anch ? j->IA : A->I, d, t), k = dot3(d, t);
        imul(j->anch ? j->IB : B->I, d, t), k += dot3(d, t);
    } else {
        float c[3];
        k = A->im + B->im;
        cross3(ra, d, c), imul(A->I, c, t), k += dot3(c, t);
        cross3(rb, d, c), imul(B->I, c, t), k += dot3(c, t);
    }
    r->m = k > 1e-12f ? 1.0f / k : 0.0f;
}

// a one-sided limit on x within +-L along d: the nearer side (C: how far inside, >= 0 allowed)
static void add_limit(Joint* j, uint32_t slot, bool ang, const float axis[3], const float ra[3], const float rb[3], float x, float L, bool soft) {
    const CConstraint* c = j->c;
    Soft s = soft ? make_soft(c->limit_hz > 0.0f ? c->limit_hz : 5.0f, c->limit_damping > 0.0f ? c->limit_damping : 1.0f, P.h) : P.soft_joint;
    float d[3], big = 1e30f;
    if (x > 0.0f) {
        for (int k = 0; k < 3; k++) d[k] = -axis[k];
        add_jrow(j, soft ? JR_SOFTLIMIT : JR_LIMIT, slot, ang, d, ra, rb, L - x, 0.0f, 0.0f, big, s);
        j->row[j->nrow - 1].pad = 1; // (the upper side: its stored impulse negative)
    } else {
        add_jrow(j, soft ? JR_SOFTLIMIT : JR_LIMIT, slot, ang, axis, ra, rb, x + L, 0.0f, 0.0f, big, s);
    }
}

// The rows of a joint from the bodies' poses now
static void joint_rows(Joint* j) {
    const CConstraint* c = j->c;
    float pa[3], qa[4], pb[3], qb[4], QA[4], QB[4], RA[3][3], RB[3][3];
    joint_pose(j->a, j->sa, pa, qa);
    joint_pose(j->b, j->sb, pb, qb);
    qmul(qa, c->fa, QA), qmul(qb, c->fb, QB);
    quat_axes(QA, RA), quat_axes(QB, RB);
    float ra[3], rb[3], wa[3], wb[3], dl[3], raB[3];
    qrot(qa, c->la, ra);
    for (int k = 0; k < 3; k++) wa[k] = pa[k] + ra[k];
    if (j->b >= 0) {
        qrot(qb, c->lb, rb);
        for (int k = 0; k < 3; k++) wb[k] = pb[k] + rb[k];
    } else
        memset(rb, 0, 12), memcpy(wb, c->lb, 12);
    for (int k = 0; k < 3; k++) dl[k] = wb[k] - wa[k], raB[k] = ra[k] + dl[k]; // (the arm on body 1 to body 2's anchor)
    uint32_t lm[3], am[3];
    float ll[3], al[3], big = 1e30f, h = P.h;
    joint_motions(c, lm, am, ll, al);
    j->nrow = 0;
    j->anch = lm[0] == CMOTION_LOCKED && lm[1] == CMOTION_LOCKED && lm[2] == CMOTION_LOCKED;
    if (j->anch) { // (I + m (r.r E - r r^T))^-1: each body's inertia around the anchor (parallel axes)
        for (int s = 0; s < 2; s++) {
            const State* S = &states()[s ? j->sb : j->sa];
            float* out = s ? j->IB : j->IA;
            const float* r = s ? rb : ra;
            memset(out, 0, 24);
            if (!S->dyn || S->im <= 0.0f) continue;
            float Mi[3][3] = { { S->I[0], S->I[3], S->I[4] }, { S->I[3], S->I[1], S->I[5] }, { S->I[4], S->I[5], S->I[2] } }, M[3][3], Ia[3][3];
            if (!invert3(Mi, M)) continue;
            float m = 1.0f / S->im, rr = dot3(r, r);
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) M[a][b] += m * ((a == b ? rr : 0.0f) - r[a] * r[b]);
            if (!invert3(M, Ia)) continue;
            out[0] = Ia[0][0], out[1] = Ia[1][1], out[2] = Ia[2][2], out[3] = Ia[0][1], out[4] = Ia[0][2], out[5] = Ia[1][2];
        }
    }
    Soft sj = P.soft_joint;
    bool soft_lin = (c->soft_limits & 1u) != 0, soft_ang = (c->soft_limits & 2u) != 0;
    Soft sd = make_soft(c->drive_hz, c->drive_damping > 0.0f ? c->drive_damping : 1.0f, h);
    if (c->drive_hz <= 0.0f) sd = make_soft(JOINT_HZ, JOINT_ZETA, h);
    float lmax = c->lin_drive_max > 0.0f ? c->lin_drive_max * h : big, amax = c->ang_drive_max > 0.0f ? c->ang_drive_max * h : big;
    static const float zero[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        float x = dot3(dl, RA[k]);
        if (lm[k] == CMOTION_LOCKED) add_jrow(j, JR_EQ, (uint32_t)k, false, RA[k], raB, rb, x, 0.0f, -big, big, sj);
        else if (lm[k] == CMOTION_LIMITED) add_limit(j, (uint32_t)k, false, RA[k], raB, rb, x, ll[k], soft_lin);
        if (lm[k] == CMOTION_LOCKED) continue;
        // the drives and motors: body 1 in body 2's frame (the world's: "to 100 cm along X" moves body 1 there)
        float nb[3] = { -RB[k][0], -RB[k][1], -RB[k][2] }, xu = -dot3(dl, RB[k]);
        if (c->lin_drive & CDRIVE_POSITION)
            add_jrow(j, JR_SPRING, 6u + (uint32_t)k, false, nb, raB, rb, xu - c->lin_target[k], (c->lin_drive & CDRIVE_VELOCITY) ? c->lin_target_vel[k] : 0.0f,
                     -lmax, lmax, sd);
        else if (c->lin_drive & CDRIVE_VELOCITY) add_jrow(j, JR_MOTOR, 6u + (uint32_t)k, false, nb, raB, rb, 0.0f, c->lin_target_vel[k], -lmax, lmax, sd);
        else if (k == 0 && c->type == CONSTRAINT_PRISMATIC && c->motor) {
            float f = c->motor_strength > 0.0f ? c->motor_strength * h : big;
            // (at its limit the way it drives: the limit alone, as before; an unlimited motor against it shook the slide)
            bool held = lm[0] == CMOTION_LIMITED && ((c->motor_speed > 0.0f && xu >= ll[0] - 0.5f) || (c->motor_speed < 0.0f && xu <= -ll[0] + 0.5f));
            if (!held) add_jrow(j, JR_MOTOR, 6u, false, nb, raB, rb, 0.0f, c->motor_speed, -f, f, sd);
        }
    }
    // the angles: body 2's frame from body 1's, split into twist (around X) and swing
    float cq[4], qr[4];
    qconj(QA, cq), qmul(cq, QB, qr);
    if (qr[3] < 0.0f) for (int k = 0; k < 4; k++) qr[k] = -qr[k];
    float tl = sqrtf(qr[0] * qr[0] + qr[3] * qr[3]), tq[4] = { tl > 1e-9f ? qr[0] / tl : 0.0f, 0, 0, tl > 1e-9f ? qr[3] / tl : 1.0f }, ct[4], sw[4];
    float twist = 2.0f * atan2f(tq[0], tq[3]);
    qconj(tq, ct), qmul(qr, ct, sw); // (swing = q * twist^-1: around an axis in Y-Z)
    if (sw[3] < 0.0f) for (int k = 0; k < 4; k++) sw[k] = -sw[k];
    float xt[3];
    for (int k = 0; k < 3; k++) xt[k] = RA[0][k] + RB[0][k];
    float xl = len3(xt);
    for (int k = 0; k < 3; k++) xt[k] = xl > 1e-6f ? xt[k] / xl : RA[0][k];
    const float D2R = 0.0174532925f;
    if (am[0] == CMOTION_LOCKED) add_jrow(j, JR_EQ, 3, true, xt, ra, rb, twist, 0.0f, -big, big, sj);
    else if (am[0] == CMOTION_LIMITED) add_limit(j, 3, true, xt, ra, rb, twist, al[0] * D2R, soft_ang);
    float a1 = 2.0f * atan2f(sw[2], sw[3]), a2 = 2.0f * atan2f(sw[1], sw[3]); // around Z (swing 1), around Y (swing 2)
    if (am[1] == CMOTION_LOCKED) add_jrow(j, JR_EQ, 4, true, RA[2], ra, rb, a1, 0.0f, -big, big, sj);
    if (am[2] == CMOTION_LOCKED) add_jrow(j, JR_EQ, 5, true, RA[1], ra, rb, a2, 0.0f, -big, big, sj);
    if (am[1] == CMOTION_LIMITED && am[2] == CMOTION_LIMITED) { // the cone: an ellipse of the two half angles
        float sl = sqrtf(sw[1] * sw[1] + sw[2] * sw[2]), phi = 2.0f * atan2f(sl, sw[3]);
        if (sl > 1e-6f) {
            float uy = sw[1] / sl, uz = sw[2] / sl, L1 = al[1] * D2R, L2 = al[2] * D2R;
            float L = 1.0f / sqrtf(uz * uz / (L1 * L1) + uy * uy / (L2 * L2)), u[3];
            for (int k = 0; k < 3; k++) u[k] = uy * RA[1][k] + uz * RA[2][k];
            float un = len3(u);
            for (int k = 0; k < 3; k++) u[k] /= un > 1e-6f ? un : 1.0f;
            add_limit(j, 4, true, u, ra, rb, phi, L, soft_ang); // (phi >= 0: always the upper side)
        }
    } else {
        if (am[1] == CMOTION_LIMITED) add_limit(j, 4, true, RA[2], ra, rb, a1, al[1] * D2R, soft_ang);
        if (am[2] == CMOTION_LIMITED) add_limit(j, 5, true, RA[1], ra, rb, a2, al[2] * D2R, soft_ang);
    }
    // the angular drive (a SLERP drive: toward an orientation in body 1's frame, and an angular velocity)
    static const int axis_motion[3] = { 0, 2, 1 }; // frame X: twist, Y: swing 2, Z: swing 1
    if (c->ang_drive & (CDRIVE_POSITION | CDRIVE_VELOCITY)) { // (body 1's orientation in body 2's frame, as the linear)
        float e[3] = { 0, 0, 0 };
        if (c->ang_drive & CDRIVE_POSITION) {
            float TR[3][3], qt[4], ctq[4], qe[4], qu[4];
            rotator_axes(c->ang_target, TR);
            axes_quat(TR, qt);
            qconj(qr, qu);
            qconj(qt, ctq), qmul(ctq, qu, qe);
            if (qe[3] < 0.0f) for (int k = 0; k < 4; k++) qe[k] = -qe[k];
            for (int k = 0; k < 3; k++) e[k] = 2.0f * qe[k];
        }
        for (int k = 0; k < 3; k++) {
            if (am[axis_motion[k]] == CMOTION_LOCKED) continue;
            float vt = (c->ang_drive & CDRIVE_VELOCITY) ? c->ang_target_vel[k] * D2R : 0.0f, nb[3] = { -RB[k][0], -RB[k][1], -RB[k][2] };
            if (c->ang_drive & CDRIVE_POSITION) add_jrow(j, JR_SPRING, 9u + (uint32_t)k, true, nb, ra, rb, e[k], vt, -amax, amax, sd);
            else add_jrow(j, JR_MOTOR, 9u + (uint32_t)k, true, nb, ra, rb, 0.0f, vt, -amax, amax, sd);
        }
    } else if (c->type == CONSTRAINT_HINGE && c->motor) {
        float f = c->motor_strength > 0.0f ? c->motor_strength * h * 10.0f : big, nx[3] = { -xt[0], -xt[1], -xt[2] }; // (a torque's arm: ~10 cm, as before)
        float L = al[0] * D2R, a = -twist; // (body 1's turn; at its limit the way it drives: the limit alone)
        bool held = am[0] == CMOTION_LIMITED && ((c->motor_speed > 0.0f && a >= L - 0.01f) || (c->motor_speed < 0.0f && a <= -L + 0.01f));
        if (!held) add_jrow(j, JR_MOTOR, 9, true, nx, ra, rb, 0.0f, c->motor_speed * D2R, -f, f, sd);
    }
    // each row from its slot's impulse (a limit's side changed: from zero)
    for (uint32_t r = 0; r < j->nrow; r++) {
        JRow* w = &j->row[r];
        float a = j->c->acc[w->slot];
        if (w->kind == JR_LIMIT || w->kind == JR_SOFTLIMIT) a = w->pad ? fmaxf(-a, 0.0f) : fmaxf(a, 0.0f);
        w->acc = fminf(fmaxf(a, w->lo), w->hi);
    }
}


static void jrow_apply(const Joint* j, const JRow* r, float imp) {
    State* A = &states()[j->sa];
    State* B = &states()[j->sb];
    float t[3];
    if (r->ang) {
        if (j->anch) { // (turned around the anchor: the middle moves with it, the anchor stays)
            float dw[3], dv[3];
            if (B->dyn) {
                imul(j->IB, r->d, dw);
                for (int k = 0; k < 3; k++) dw[k] *= imp, B->w[k] += dw[k];
                cross3(r->rb, dw, dv);
                for (int k = 0; k < 3; k++) B->v[k] += dv[k];
            }
            if (A->dyn) {
                imul(j->IA, r->d, dw);
                for (int k = 0; k < 3; k++) dw[k] *= -imp, A->w[k] += dw[k];
                cross3(r->ra, dw, dv);
                for (int k = 0; k < 3; k++) A->v[k] += dv[k];
            }
            return;
        }
        if (B->dyn) imul(B->I, r->d, t), B->w[0] += t[0] * imp, B->w[1] += t[1] * imp, B->w[2] += t[2] * imp;
        if (A->dyn) imul(A->I, r->d, t), A->w[0] -= t[0] * imp, A->w[1] -= t[1] * imp, A->w[2] -= t[2] * imp;
        return;
    }
    float P3[3] = { r->d[0] * imp, r->d[1] * imp, r->d[2] * imp }, c[3];
    if (B->dyn) {
        for (int k = 0; k < 3; k++) B->v[k] += B->im * P3[k];
        cross3(r->rb, P3, c), imul(B->I, c, t);
        for (int k = 0; k < 3; k++) B->w[k] += t[k];
    }
    if (A->dyn) {
        for (int k = 0; k < 3; k++) A->v[k] -= A->im * P3[k];
        cross3(r->ra, P3, c), imul(A->I, c, t);
        for (int k = 0; k < 3; k++) A->w[k] -= t[k];
    }
}

static void joint_solve(Joint* j, bool bias) {
    const State* A = &states()[j->sa];
    const State* B = &states()[j->sb];
    float inv_h = P.inv_h;
    for (uint32_t r = 0; r < j->nrow; r++) {
        JRow* w = &j->row[r];
        float Jv;
        if (w->ang) Jv = dot3(w->d, B->w) - dot3(w->d, A->w);
        else {
            float x[3], y[3], vr[3];
            cross3(B->w, w->rb, y), cross3(A->w, w->ra, x);
            for (int k = 0; k < 3; k++) vr[k] = B->v[k] + y[k] - A->v[k] - x[k];
            Jv = dot3(w->d, vr);
        }
        float imp;
        const Soft s = w->s;
        switch (w->kind) {
        case JR_EQ: imp = bias ? -w->m * s.mass * (Jv + s.rate * w->C) - s.imp * w->acc : -w->m * Jv; break;
        case JR_LIMIT:
            if (w->C > 0.0f) imp = -w->m * (Jv + w->C * inv_h); // (inside: may reach the limit, no further)
            else imp = bias ? -w->m * s.mass * (Jv + s.rate * w->C) - s.imp * w->acc : -w->m * Jv;
            break;
        case JR_SOFTLIMIT:
            imp = w->C > 0.0f ? -w->m * (Jv + w->C * inv_h) : -w->m * s.mass * (Jv + s.rate * w->C) - s.imp * w->acc;
            break;
        case JR_SPRING: imp = -w->m * s.mass * (Jv - w->vt + s.rate * w->C) - s.imp * w->acc; break;
        default: imp = -w->m * (Jv - w->vt); break; // JR_MOTOR
        }
        float acc = fminf(fmaxf(w->acc + imp, w->lo), w->hi);
        imp = acc - w->acc, w->acc = acc;
        jrow_apply(j, w, imp);
    }
    for (uint32_t r = 0; r < j->nrow; r++) {
        const JRow* w = &j->row[r];
        j->c->acc[w->slot] = w->pad ? -w->acc : w->acc;
    }
}

static void joints_stage(uint32_t type) {
    Joint* J = joints();
    for (uint32_t i = 0; i < P.njoints; i++) {
        Joint* j = &J[i];
        if ((!states()[j->sa].dyn && !states()[j->sb].dyn) || j->c->broken) continue;
        if (type == ST_JWARM) {
            joint_rows(j);
            for (uint32_t r = 0; r < j->nrow; r++) jrow_apply(j, &j->row[r], j->row[r].acc);
        } else if (type == ST_JSOLVE) // (a few passes: an angular limit and the arm's linear rows pull on each other; joints are few)
            for (int it = 0; it < JOINT_ITERATIONS; it++) joint_solve(j, true);
        else {
            joint_rows(j);
            for (int it = 0; it < JOINT_ITERATIONS / 2; it++) joint_solve(j, false);
        }
    }
}

// After a step: the pull and the twist (the last substep's impulses over it); past its thresholds it breaks
static void joints_after(void) {
    Joint* J = joints();
    float worst = P.stats.joint_error_max;
    for (uint32_t i = 0; i < P.njoints; i++) {
        Joint* j = &J[i];
        if ((!states()[j->sa].dyn && !states()[j->sb].dyn) || j->c->broken) continue;
        float f[3] = { 0, 0, 0 }, t[3] = { 0, 0, 0 };
        for (uint32_t r = 0; r < j->nrow; r++) {
            const JRow* w = &j->row[r];
            if (w->slot >= 6) continue; // (the drives push: they don't break it)
            float* o = w->ang ? t : f;
            for (int k = 0; k < 3; k++) o[k] += w->d[k] * w->acc;
            if (!w->ang && w->kind == JR_EQ && fabsf(w->C) > worst) worst = fabsf(w->C);
        }
        CConstraint* c = j->c;
        c->force = len3(f) * P.inv_h, c->torque = len3(t) * P.inv_h;
        if ((c->break_force > 0.0f && c->force > c->break_force) || (c->break_torque > 0.0f && c->torque > c->break_torque))
            c->broken = 1, P.stats.joints_broken++;
    }
    P.stats.joint_error_max = worst;
}

// --- The stages: one parallel_for runs the whole solver (each stage's blocks taken by an atomic counter, every
// worker waiting at its end; a worker that starts late finds them done; one thread alone does them all in order) ---

static void run_block(const Stage* st, int32_t blk) {
    uint32_t i0 = st->begin + (uint32_t)blk * st->block, i1 = i0 + st->block;
    if (i1 > st->begin + st->count) i1 = st->begin + st->count;
    Contact* C = (Contact*)P.contacts.base;
    Wide* W = (Wide*)P.wides.base;
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    switch (st->type) {
    case ST_INTV: for (uint32_t i = i0; i < i1; i++) integrate_v(aw[i]); break;
    case ST_INTP: for (uint32_t i = i0; i < i1; i++) integrate_p(aw[i]); break;
    case ST_WARM: for (uint32_t i = i0; i < i1; i++) warm_contact(&C[i]); break;
    case ST_SOLVE: for (uint32_t i = i0; i < i1; i++) solve_contact(&C[i], true, st->flip); break;
    case ST_RELAX: for (uint32_t i = i0; i < i1; i++) solve_contact(&C[i], false, st->flip); break;
    case ST_REST: for (uint32_t i = i0; i < i1; i++) restitution_contact(&C[i]); break;
    case ST_STORE: for (uint32_t i = i0; i < i1; i++) store_contact(&C[i]); break;
    case ST_WPACK:
        for (uint32_t i = i0; i < i1; i++) {
            uint32_t c = 0;
            while (c + 1 < MAX_COLORS && P.color_wfirst[c + 1] <= i) c++; // (its color: the first wide past it)
            uint32_t first = P.color_first[c] + (i - P.color_wfirst[c]) * FW_LANES, end = P.color_first[c + 1];
            wide_pack(&W[i], first, end - first < FW_LANES ? end - first : FW_LANES);
        }
        break;
    case ST_WWARM: for (uint32_t i = i0; i < i1; i++) wide_warm(&W[i]); break;
    case ST_WSOLVE: for (uint32_t i = i0; i < i1; i++) wide_solve(&W[i], true, st->flip); break;
    case ST_WRELAX: for (uint32_t i = i0; i < i1; i++) wide_solve(&W[i], false, st->flip); break;
    case ST_WREST: for (uint32_t i = i0; i < i1; i++) wide_restitution(&W[i]); break;
    case ST_WSTORE:
        for (uint32_t i = i0; i < i1; i++) {
            wide_unpack(&W[i]);
            for (uint32_t l = 0; l < W[i].count; l++) store_contact(&C[W[i].first + l]);
        }
        break;
    default: joints_stage(st->type); break;
    }
}

static int stage_group(uint32_t type) { // (the solver's measure: 0 pack, 1 integrate, 2 warm, 3 solve, 4 relax, 5 restitution, 6 store, 7 joints)
    switch (type) {
    case ST_WPACK: return 0;
    case ST_INTV: case ST_INTP: return 1;
    case ST_WARM: case ST_WWARM: return 2;
    case ST_SOLVE: case ST_WSOLVE: return 3;
    case ST_RELAX: case ST_WRELAX: return 4;
    case ST_REST: case ST_WREST: return 5;
    case ST_STORE: case ST_WSTORE: return 6;
    default: return 7;
    }
}
static void solver_worker(void* arg, int index) {
    (void)arg;
    double t = index == 0 ? re_time_seconds() : 0.0;
    for (uint32_t s = 0; s < P.nstages; s++) {
        Stage* st = &P.stages[s];
        if (!st->nblocks) continue;
        for (;;) {
            int32_t b = re_atomic_add(&st->next, 1) - 1;
            if (b >= st->nblocks) break;
            run_block(st, b);
            re_atomic_add(&st->done, 1);
        }
        // (spin, then yield: a worker the OS paused mid-block must get the core back, not wait out the others' spin)
        for (uint32_t k = 0; re_atomic_load(&st->done) < st->nblocks; k++)
            if (k < 4096u) re_cpu_relax();
            else re_yield();
        if (index == 0) {
            double t1 = re_time_seconds();
            P.stats.solver_ms[stage_group(st->type)] += (t1 - t) * 1000.0, t = t1;
        }
    }
}

static void add_stage(uint32_t type, uint32_t begin, uint32_t count, uint32_t block) {
    if (P.nstages >= MAX_STAGES) return;
    Stage* s = &P.stages[P.nstages++];
    s->type = type, s->begin = begin, s->count = count, s->block = block ? block : 1;
    s->nblocks = count ? (int32_t)((count + s->block - 1) / s->block) : 0;
    s->next = s->done = 0;
    s->flip = P.flip;
}
static void add_colors(uint32_t type) {
    uint32_t wtype = type == ST_WARM ? ST_WWARM : type == ST_SOLVE ? ST_WSOLVE : type == ST_RELAX ? ST_WRELAX : ST_WREST;
    for (uint32_t c = 0; c <= MAX_COLORS; c++) {
        uint32_t b = P.color_first[c], n = P.color_first[c + 1] - b;
        if (!n) continue; // (only the colors in use: each stage ends at a barrier)
        if (c == COLOR_OVERFLOW) add_stage(type, b, n, n); // (the overflow: one worker, in order, scalar)
        else if (g_scalar) add_stage(type, b, n, BLOCK_CONTACTS);
        else add_stage(wtype, P.color_wfirst[c], P.color_wfirst[c + 1] - P.color_wfirst[c], WBLOCK);
    }
}

static void solve_step(void) {
    P.nstages = 0;
    uint32_t sub = SUBSTEPS, jn = P.njoints ? 1u : 0u;
    add_stage(ST_WPACK, 0, g_scalar ? 0u : P.nwides, WBLOCK * 2u);
    for (uint32_t s = 0; s < sub; s++) {
        P.flip = s & 1u;
        add_stage(ST_INTV, 0, P.nawake, BLOCK_BODIES);
        add_stage(ST_JWARM, 0, jn, 1);
        add_colors(ST_WARM);
        add_stage(ST_JSOLVE, 0, jn, 1);
        add_colors(ST_SOLVE);
        add_stage(ST_INTP, 0, P.nawake, BLOCK_BODIES);
        add_stage(ST_JRELAX, 0, jn, 1);
        add_colors(ST_RELAX);
    }
    P.flip = 0;
    add_colors(ST_REST);
    add_stage(ST_WSTORE, 0, g_scalar ? 0u : P.nwides, WBLOCK * 4u);
    uint32_t of = g_scalar ? 0u : P.color_first[COLOR_OVERFLOW], on = P.color_first[COLOR_OVERFLOW + 1] - of;
    add_stage(ST_STORE, of, on, BLOCK_CONTACTS * 4u);
    P.stats.stages = P.nstages, P.stats.solver_steps++;
    int nt = re_pool_threads();
    if (g_solver_threads > 0 && g_solver_threads < nt) nt = g_solver_threads;
    re_parallel_for(nt, solver_worker, NULL);
}

// --- After the substeps ---

static void finalize_fn(void* arg, int blk) {
    (void)arg;
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    uint32_t i1 = ((uint32_t)blk + 1) * BLOCK_BODIES < P.nawake ? ((uint32_t)blk + 1) * BLOCK_BODIES : P.nawake;
    float h = STEP;
    for (uint32_t k = (uint32_t)blk * BLOCK_BODIES; k < i1; k++) {
        uint32_t i = aw[k];
        Body* b = &bodies()[i];
        const State* s = &states()[i + 1];
        const Delta* d = &deltas()[i + 1];
        for (int q = 0; q < 3; q++) b->p[q] = b->p0[q] + d->dp[q];
        qmul(d->dq, b->q0, b->q);
        qnorm(b->q);
        quat_axes(b->q, b->R);
        memcpy(b->v, s->v, 12), memcpy(b->w, s->w, 12);
        if ((b->g.kind == SHAPE_BALL || b->g.kind == SHAPE_CYL || b->g.kind == SHAPE_CAPS) && b->touching) { // rolling resistance: a ball on the floor slows down and rests
            float kk = 1.0f / (1.0f + h * 1.2f);
            for (int c = 0; c < 3; c++) b->w[c] *= kk;
            float wl = len3(b->w), dw = 2.0f * h; // (and a constant part, 2 rad/s per second: a slow roll ends)
            float f = wl > dw ? 1.0f - dw / wl : 0.0f;
            for (int c = 0; c < 3; c++) b->w[c] *= f;
        }
        float speed = len3(b->v) + len3(b->w) * b->extent;
        b->sleep_time = speed < SLEEP_V ? b->sleep_time + h : 0.0f;
        if (b->sleep_time >= SLEEP_TIME || b->p[2] < -100000.0f) P.sleepy = 1; // (the islands looked at this step)
        b->moved = 1;
    }
}

// CCD: conservative advancement of A from its start to its end against B (a static, or a body at its end): the first
// time it comes within the target, 1: never (or already touching at the start: the contacts' job)
static float toi_cvx(const Body* A, const Cvx* B, float target) {
    float d[3] = { A->p[0] - A->p0[0], A->p[1] - A->p0[1], A->p[2] - A->p0[2] };
    float cq[4], dq[4];
    qconj(A->q0, cq), qmul(A->q, cq, dq);
    if (dq[3] < 0.0f) for (int k = 0; k < 4; k++) dq[k] = -dq[k];
    float ang = 2.0f * acosf(fminf(dq[3], 1.0f)), reach = len3(d) + ang * A->extent;
    float t = 0.0f;
    for (int it = 0; it < 40; it++) {
        float p[3], q[4], R[3][3];
        for (int k = 0; k < 3; k++) p[k] = A->p0[k] + d[k] * t;
        for (int k = 0; k < 4; k++) q[k] = A->q0[k] + (A->q[k] * (dot3(A->q0, A->q) + A->q0[3] * A->q[3] < 0.0f ? -1.0f : 1.0f) - A->q0[k]) * t;
        qnorm(q);
        quat_axes(q, R);
        Cvx a;
        cvx_of(p, R, &A->g, &a);
        float n[3], dist = phys_distance(&a, B, reach * (1.0f - t) + target + 1.0f, n);
        if (dist <= target) return it ? t : 1.0f;
        float closing = -dot3(d, n) + ang * A->extent;
        if (closing <= 1e-4f) return 1.0f;
        t += (dist - 0.5f * target) / closing;
        if (t >= 1.0f) return 1.0f;
    }
    return t;
}

typedef struct TriToi { const Body* A; const Static* S; float t; } TriToi;
static bool tri_toi(void* user, const float tri[3][3]) {
    TriToi* q = user;
    const Static* S = q->S;
    Cvx T;
    memset(&T, 0, sizeof T);
    T.kind = CVX_TRIANGLE, T.axis = 2;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            T.t[c][k] = S->c[k] + tri[c][0] * S->g.s[0] * S->R[0][k] + tri[c][1] * S->g.s[1] * S->R[1][k] + tri[c][2] * S->g.s[2] * S->R[2][k];
    float e1[3], e2[3];
    for (int k = 0; k < 3; k++) e1[k] = T.t[1][k] - T.t[0][k], e2[k] = T.t[2][k] - T.t[0][k], T.c[k] = (T.t[0][k] + T.t[1][k] + T.t[2][k]) / 3.0f;
    cross3(e1, e2, T.tn);
    float l = len3(T.tn);
    if (l < 1e-8f) return true;
    for (int k = 0; k < 3; k++) T.tn[k] /= l;
    T.R[0][0] = T.R[1][1] = T.R[2][2] = 1.0f;
    float t = toi_cvx(q->A, &T, 0.5f * SLOP);
    if (t < q->t) q->t = t;
    return true;
}

static void ccd_fn(void* arg, int idx) {
    (void)arg;
    const uint32_t* fl = (const uint32_t*)P.fast.base; // (body, first candidate, count) per fast body, then the pairs
    uint32_t i = fl[idx * 3], first = fl[idx * 3 + 1], count = fl[idx * 3 + 2];
    const uint32_t* cand = fl + P.nfast * 3;
    const Body* bd = bodies();
    const Body* A = &bd[i];
    const Pair* pairs = (const Pair*)P.pairs.base;
    float best = 1.0f;
    for (uint32_t k = first; k < first + count; k++) {
        const Pair* p = &pairs[cand[k]];
        int32_t other = p->a == (int32_t)i ? p->b : p->a;
        if (other >= 0) {
            if (A->fast < 2) continue; // (fast, not a bullet: against the level only, as Box2D)
            const Body* B = &bd[other];
            Cvx b;
            cvx_of(B->p, B->R, &B->g, &b);
            float t = toi_cvx(A, &b, 0.5f * SLOP);
            if (t < best) best = t;
            continue;
        }
        const Static* S = &statics()[-(other + 1)];
        if (S->g.kind == SHAPE_TRIS) {
            Body tmp = *A; // (its bounds over the whole sweep, in the mesh's space)
            float mn[3], mx[3];
            for (int k2 = 0; k2 < 3; k2++) tmp.mn[k2] = fminf(A->mn[k2], fminf(A->p[k2], A->p0[k2]) - A->extent), tmp.mx[k2] = fmaxf(A->mx[k2], fmaxf(A->p[k2], A->p0[k2]) + A->extent);
            tris_box(&tmp, S, mn, mx);
            TriToi q = { A, S, best };
            phys_mesh_query(S->g.mesh, mn, mx, tri_toi, &q);
            best = q.t;
        } else if (S->g.kind != SHAPE_HULL || S->g.hull) {
            Cvx b;
            cvx_of(S->c, S->R, &S->g, &b);
            float t = toi_cvx(A, &b, 0.5f * SLOP);
            if (t < best) best = t;
        }
    }
    ((float*)P.ccd_t.base)[idx] = best;
    re_atomic_add(&P.ccd_sweeps, 1);
}

static void ccd(void) {
    Body* bd = bodies();
    const Pair* pairs = (const Pair*)P.pairs.base;
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    uint32_t nf = 0;
    if (!re_vm_ensure(&P.fast, ((size_t)P.nawake * 3u + 64u) * 4u)) return;
    uint32_t* slot = (uint32_t*)P.mint.base; // (a fast body's index in the list, per body)
    for (uint32_t k = 0; k < P.nawake; k++)
        if (bd[aw[k]].fast) slot[aw[k]] = nf++;
    if (!nf) return;
    uint32_t* fl = (uint32_t*)P.fast.base;
    for (uint32_t f = 0, k = 0; k < P.nawake; k++)
        if (bd[aw[k]].fast) fl[f * 3] = aw[k], fl[f * 3 + 1] = 0, fl[f * 3 + 2] = 0, f++;
    for (uint32_t i = 0; i < P.npairs; i++) {
        if (bd[pairs[i].a].fast) fl[slot[pairs[i].a] * 3 + 2]++;
        if (pairs[i].b >= 0 && bd[pairs[i].b].fast && bd[pairs[i].b].awake) fl[slot[pairs[i].b] * 3 + 2]++;
    }
    uint32_t total = 0;
    for (uint32_t f = 0; f < nf; f++) fl[f * 3 + 1] = total, total += fl[f * 3 + 2], fl[f * 3 + 2] = 0;
    if (!re_vm_ensure(&P.fast, ((size_t)nf * 3u + total + 1u) * 4u)) return;
    fl = (uint32_t*)P.fast.base;
    uint32_t* cand = fl + nf * 3;
    for (uint32_t i = 0; i < P.npairs; i++) {
        if (bd[pairs[i].a].fast) {
            uint32_t f = slot[pairs[i].a];
            cand[fl[f * 3 + 1] + fl[f * 3 + 2]++] = i;
        }
        if (pairs[i].b >= 0 && bd[pairs[i].b].fast && bd[pairs[i].b].awake) {
            uint32_t f = slot[pairs[i].b];
            cand[fl[f * 3 + 1] + fl[f * 3 + 2]++] = i;
        }
    }
    P.nfast = nf;
    re_parallel_for((int)nf, ccd_fn, NULL);
    const float* tt = (const float*)P.ccd_t.base;
    for (uint32_t f = 0; f < nf; f++) {
        float t = tt[f];
        if (t >= 1.0f) continue;
        Body* b = &bd[fl[f * 3]];
        float q1[4];
        memcpy(q1, b->q, 16);
        float sg = b->q0[0] * q1[0] + b->q0[1] * q1[1] + b->q0[2] * q1[2] + b->q0[3] * q1[3] < 0.0f ? -1.0f : 1.0f;
        for (int k = 0; k < 3; k++) b->p[k] = b->p0[k] + (b->p[k] - b->p0[k]) * t;
        for (int k = 0; k < 4; k++) b->q[k] = b->q0[k] + (q1[k] * sg - b->q0[k]) * t;
        qnorm(b->q);
        quat_axes(b->q, b->R);
    }
}

// The warm start's table: each pair's records (keys: the bodies' entities, a static's index)
#ifdef _MSC_VER
#include <intrin.h>
static int64_t cas64(volatile int64_t* p, int64_t want, int64_t was) { return _InterlockedCompareExchange64((volatile long long*)p, want, was); }
#else
static int64_t cas64(volatile int64_t* p, int64_t want, int64_t was) { return __sync_val_compare_and_swap(p, was, want); }
#endif

// (in parallel: a slot claimed by its keys at once, then filled; read only next step)
static void table_fn(void* arg, int blk) {
    Slot* tab = arg;
    const Pair* pairs = (const Pair*)P.pairs.base;
    const MDest* md = (const MDest*)P.mdest.base;
    const Man* mans = (const Man*)P.mans.base;
    uint32_t size = P.tab_size[P.cur], i1 = ((uint32_t)blk + 1) * 1024u < P.npairs ? ((uint32_t)blk + 1) * 1024u : P.npairs;
    for (uint32_t i = (uint32_t)blk * 1024u; i < i1; i++) {
        const Pair* p = &pairs[i];
        if (!p->n) continue;
        uint32_t ka = bodies()[p->a].e, kb = key_of(p->b), first = md[p->first].rec, count = 0;
        for (uint32_t k = 0; k < p->n; k++) count += mans[p->first + k].np;
        int64_t key = (int64_t)(((uint64_t)kb << 32) | ka);
        uint32_t h = (ka * 0x9E3779B1u ^ (kb + 0x7F4A7C15u) * 0x85EBCA77u);
        for (h = (h ^ (h >> 16)) & (size - 1);; h = (h + 1) & (size - 1))
            if (cas64((volatile int64_t*)&tab[h], key, 0) == 0) {
                tab[h].first = first, tab[h].count = count;
                break;
            }
    }
}

static void cache_table(void) {
    int cur = P.cur;
    const Pair* pairs = (const Pair*)P.pairs.base;
    const MDest* md = (const MDest*)P.mdest.base;
    uint32_t npairs = P.npairs, size = 16;
    while (size < npairs * 2) size <<= 1;
    if (!re_vm_ensure(&P.tab[cur], (size_t)size * sizeof(Slot))) {
        P.tab_size[cur] = 0;
        return;
    }
    Slot* tab = (Slot*)P.tab[cur].base;
    memset(tab, 0, (size_t)size * sizeof(Slot));
    P.tab_size[cur] = size;
    re_parallel_for((int)((P.npairs + 1023) / 1024), table_fn, tab);
    (void)pairs, (void)md;
    P.cur ^= 1;
}

// Islands: the awake bodies joined by touching contacts and joints; the ones still long enough sleep whole
static uint32_t uf_find(uint32_t* par, uint32_t x) {
    while (par[x] != x) par[x] = par[par[x]], x = par[x];
    return x;
}

static void islands(void) {
    Body* bd = bodies();
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    uint32_t* par = (uint32_t*)P.parent.base;
    float* mint = (float*)P.mint.base;
    uint32_t* gid = (uint32_t*)P.cmask.base;
    for (uint32_t k = 0; k < P.nawake; k++) par[aw[k]] = aw[k];
    const Contact* C = (const Contact*)P.contacts.base;
    for (uint32_t i = 0; i < P.ncontacts; i++) {
        const Contact* c = &C[i];
        if (!c->touch || c->b < 0 || !states()[c->sb].dyn) continue;
        uint32_t x = uf_find(par, (uint32_t)c->a), y = uf_find(par, (uint32_t)c->b);
        if (x != y) par[x] = y;
    }
    const Joint* J = joints();
    for (uint32_t i = 0; i < P.njoints; i++) {
        const Joint* j = &J[i];
        const CConstraint* c = j->c;
        bool driven = (c->motor && c->motor_speed != 0.0f) || c->lin_drive || c->ang_drive;
        if (driven) bd[j->a].sleep_time = 0.0f; // (a motor keeps its bodies awake)
        if (j->b < 0 || !states()[j->sa].dyn || !states()[j->sb].dyn) continue;
        if (driven) bd[j->b].sleep_time = 0.0f;
        uint32_t x = uf_find(par, (uint32_t)j->a), y = uf_find(par, (uint32_t)j->b);
        if (x != y) par[x] = y;
    }
    uint32_t nisl = 0;
    for (uint32_t k = 0; k < P.nawake; k++) {
        uint32_t i = aw[k], r = uf_find(par, i);
        if (r == i) mint[i] = 1e30f, gid[i] = ~0u, nisl++;
    }
    for (uint32_t k = 0; k < P.nawake; k++) {
        uint32_t i = aw[k], r = uf_find(par, i);
        if (bd[i].sleep_time < mint[r]) mint[r] = bd[i].sleep_time;
    }
    P.stats.islands = nisl;
    for (uint32_t k = 0; k < P.nawake; k++) {
        uint32_t i = aw[k], r = uf_find(par, i);
        Body* b = &bd[i];
        if (!b->awake) continue;
        if (b->p[2] < -100000.0f) { // fell out of the world: asleep alone
            b->gnext = i;
            body_asleep(i, 0);
            continue;
        }
        if (g_sleep_off || mint[r] < SLEEP_TIME) continue;
        if (gid[r] == ~0u) gid[r] = ++P.groups, bd[r].gnext = r; // (the ring starts at its root)
        if (i != r) b->gnext = bd[r].gnext, bd[r].gnext = i;
        body_asleep(i, gid[r]);
    }
}

// (B12f) Unreal's Buoyancy: the body's bottom (its world box: the four corners and the middle, or the middle alone),
// each pontoon lifted by the water over it (its share of the box's height under the surface times the box's volume,
// the water's density, gravity), at its place (the body tilts with the waves, rights itself); in water its velocity
// taken toward the current, its spin slowed. A body in moving water stays awake (the river carries it)
static void buoyancy(uint32_t bi, float h) {
    Body* b = &bodies()[bi];
    const CBuoyancy* by = b->by;
    float mn[3], mx[3], I[6];
    bounds_of(b->p, b->R, &b->g, mn, mx);
    float H = fmaxf(mx[2] - mn[2], 1.0f);
    float vol = (mx[0] - mn[0]) * (mx[1] - mn[1]) * H * 1e-6f * 0.6f; // m^3 (0.6: a box's corners aren't the body)
    uint32_t np = by->pontoons >= 5 ? 5u : 1u;
    float sub_all = 0.0f, cur_all[2] = { 0.0f, 0.0f };
    inv_world(b->R, b->inv_il, b->g.kind != SHAPE_HULL, I);
    for (uint32_t k = 0; k < np; k++) {
        float q[3] = { k == 0 ? b->p[0] : (k & 1) ? mn[0] : mx[0], k == 0 ? b->p[1] : (k & 2) ? mn[1] : mx[1], mn[2] };
        if (k == 4) q[0] = mx[0], q[1] = mx[1];
        if (k == 3) q[0] = mn[0], q[1] = mn[1];
        float level, cur[2], rho, depth;
        if (!water_query(NULL, q, &level, cur, &rho, &depth)) continue;
        float f = fminf(fmaxf((level - mn[2]) / H, 0.0f), 1.0f);
        if (f <= 0.0f) continue;
        if (!b->awake) wake_island(bi);
        float force = rho * project()->gravity_z * 0.01f * vol * f / (float)np * by->buoyancy; // N (gravity_z cm/s^2)
        float j[3] = { 0.0f, 0.0f, force * h * 100.0f }, r[3], t[3], dw[3]; // kg cm/s
        for (int c = 0; c < 3; c++) r[c] = q[c] - b->p[c];
        r[2] = 0.0f; // (lifted at its bottom's place, not its depth: a steady righting)
        for (int c = 0; c < 3; c++) b->v[c] += j[c] * b->inv_mass;
        cross3(r, j, t), imul(I, t, dw);
        for (int c = 0; c < 3; c++) b->w[c] += dw[c];
        sub_all += f / (float)np, cur_all[0] += cur[0] / (float)np, cur_all[1] += cur[1] / (float)np;
    }
    if (sub_all <= 0.0f) return;
    b->sleep_time = 0.0f;
    float kd = 1.0f - expf(-by->water_drag * sub_all * h), ka = 1.0f - expf(-by->angular_drag * sub_all * h);
    b->v[0] += (cur_all[0] / fmaxf(sub_all, 1e-3f) * sub_all - b->v[0]) * kd;
    b->v[1] += (cur_all[1] / fmaxf(sub_all, 1e-3f) * sub_all - b->v[1]) * kd;
    b->v[2] -= b->v[2] * kd;
    for (int c = 0; c < 3; c++) b->w[c] -= b->w[c] * ka;
}

// The awake bodies' slots for the step: their start, velocity, world inverse inertia, bounds with the travel
static void prepare_bodies_fn(void* arg, int blk) {
    (void)arg;
    const uint32_t* aw = (const uint32_t*)P.awake.base;
    uint32_t i1 = ((uint32_t)blk + 1) * BLOCK_BODIES < P.nawake ? ((uint32_t)blk + 1) * BLOCK_BODIES : P.nawake;
    uint32_t* mask = (uint32_t*)P.cmask.base;
    for (uint32_t k = (uint32_t)blk * BLOCK_BODIES; k < i1; k++) {
        uint32_t i = aw[k];
        Body* b = &bodies()[i];
        State* s = &states()[i + 1];
        memcpy(b->p0, b->p, 12), memcpy(b->q0, b->q, 16);
        memcpy(s->v, b->v, 12), memcpy(s->w, b->w, 12);
        s->im = b->inv_mass, s->dyn = 1;
        inv_world(b->R, b->inv_il, b->g.kind != SHAPE_HULL, s->I);
        delta_reset(&deltas()[i + 1]);
        bounds_of(b->p, b->R, &b->g, b->mn, b->mx);
        for (int q = 0; q < 3; q++) { // grown by this step's travel (fast bodies find what they will hit)
            float m = b->v[q] * STEP;
            if (m < 0) b->mn[q] += m - 1.0f;
            else b->mx[q] += m + 1.0f;
        }
        float travel = len3(b->v) * STEP;
        b->fast = b->rb->ccd ? 2u : travel > 0.5f * b->thin ? 1u : 0u;
        b->contacts = 0, b->touching = 0;
        mask[i] = 0;
        float hs = STEP / (float)SUBSTEPS;
        IParam* ip = &((IParam*)P.iparam.base)[i];
        ip->g = b->rb->enable_gravity ? project()->gravity_z * hs : 0.0f; // (Project Settings > Physics)
        ip->ld = 1.0f / (1.0f + hs * b->rb->linear_damping), ip->ad = 1.0f / (1.0f + hs * b->rb->angular_damping);
        ip->wmax = b->rb->max_angular_velocity > 0.0f ? b->rb->max_angular_velocity * 0.0174533f : 400.0f;
        ((uint8_t*)P.dynb.base)[i] = 1;
    }
}

static void step(float h) {
    Body* bd = bodies();
    for (uint32_t i = 0; i < P.nbodies; i++) // (B12f) the water's lift and drag first (it may wake a body)
        if (bd[i].by) buoyancy(i, h);
    uint32_t* aw = (uint32_t*)P.awake.base;
    P.nawake = 0;
    for (uint32_t i = 0; i < P.nbodies; i++)
        if (bd[i].awake) aw[P.nawake++] = i;
    if (!P.nawake) {
        P.npairs = P.ncontacts = 0;
        return;
    }
    memset(P.dynb.base, 0, P.nbodies); // (the awake ones set while prepared)
    double t0 = re_time_seconds();
    // the walls' slots: the level's (0) and the Characters' (their velocity)
    State* S = states();
    memset(&S[0], 0, sizeof(State));
    delta_reset(&deltas()[0]);
    uint32_t nlevel = P.nstatics - P.nkin;
    for (uint32_t k = 0; k < P.nkin; k++) {
        State* s = &S[P.nbodies + 1 + k];
        memset(s, 0, sizeof *s);
        memcpy(s->v, statics()[nlevel + k].v, 12);
        delta_reset(&deltas()[P.nbodies + 1 + k]);
    }
    double tp[12];
    tp[0] = t0;
    re_parallel_for((int)((P.nawake + BLOCK_BODIES - 1) / BLOCK_BODIES), prepare_bodies_fn, NULL);
    tp[1] = re_time_seconds();
    broadphase();
    double t1 = re_time_seconds();
    tp[2] = t1;
    narrowphase();
    tp[3] = re_time_seconds();
    color_contacts();
    tp[4] = re_time_seconds();
    if (!re_vm_ensure(&P.contacts, ((size_t)P.ncontacts + 1u) * sizeof(Contact)) || !re_vm_ensure(&P.points, ((size_t)P.npoints + 1u) * sizeof(Pt)) ||
        !re_vm_ensure(&P.rec[P.cur], ((size_t)P.nrec + 1u) * sizeof(Rec)) || !re_vm_ensure(&P.wides, ((size_t)P.nwides + 1u) * sizeof(Wide))) {
        P.ncontacts = P.nwides = 0;
        return;
    }
    re_parallel_for((int)((P.npairs + BLOCK_PAIRS - 1) / BLOCK_PAIRS), prepare_fn, NULL);
    double t2 = re_time_seconds();
    tp[5] = t2;
    P.h = h / (float)SUBSTEPS, P.inv_h = 1.0f / P.h;
    float hz = fminf(CONTACT_HZ, 0.125f / P.h); // (an eighth of the substeps' rate: a quarter shook a tall stack)
    P.soft_contact = make_soft(hz, CONTACT_ZETA, P.h);
    P.soft_static = make_soft(2.0f * hz, CONTACT_ZETA, P.h);
    P.soft_joint = make_soft(JOINT_HZ, JOINT_ZETA, P.h);
    solve_step();
    tp[6] = re_time_seconds();
    re_parallel_for((int)((P.nawake + BLOCK_BODIES - 1) / BLOCK_BODIES), finalize_fn, NULL);
    tp[7] = re_time_seconds();
    ccd();
    joints_after();
    tp[8] = re_time_seconds();
    cache_table();
    tp[9] = re_time_seconds();
    if (P.sleepy || !(++P.nsteps & 7u)) islands(); // (only when one may sleep; every 8th step for the count)
    P.sleepy = 0;
    double t3 = re_time_seconds();
    tp[10] = t3;
    for (int k = 0; k < 10; k++) P.t_phase[k] += (tp[k + 1] - tp[k]) * 1000.0; // ([10], [11]: inside the broadphase)
    P.t_broad += (t1 - t0) * 1000.0, P.t_narrow += (t2 - t1) * 1000.0, P.t_solve += (t3 - t2) * 1000.0;
}

// --- Written back (in parallel: each body its own row) ---

typedef struct WriteJob { volatile int32_t awake, ntouch; uint32_t* touch; } WriteJob;
static void write_fn(void* arg, int blk) {
    WriteJob* j = arg;
    uint32_t i1 = ((uint32_t)blk + 1) * BLOCK_BODIES < P.nbodies ? ((uint32_t)blk + 1) * BLOCK_BODIES : P.nbodies, awake = 0;
    for (uint32_t i = (uint32_t)blk * BLOCK_BODIES; i < i1; i++) {
        Body* b = &bodies()[i];
        awake += b->awake;
        if (!b->moved) continue;
        b->moved = 0;
        CRigidBody* rb = b->rb;
        CTransform* t = b->t;
        memcpy(rb->velocity, b->v, 12), memcpy(rb->angular_velocity, b->w, 12);
        rb->sleep_time = b->sleep_time;
        for (int k = 0; k < 3; k++) t->location[k] = b->p[k] - (b->offset[0] * b->R[0][k] + b->offset[1] * b->R[1][k] + b->offset[2] * b->R[2][k]);
        axes_rotator(b->R, t->rotation);
        memcpy(b->wt, t->location, 36);
        // (a Movable actor is sent to the GPU every frame: only the others go in the change log, which 20 thousand
        // moving bodies would overflow, making the renderer and the caches start over every frame)
        if ((t->flags & TRANSFORM_MOBILITY_MASK) != MOBILITY_MOVABLE) j->touch[re_atomic_add(&j->ntouch, 1) - 1] = i;
    }
    re_atomic_add(&j->awake, (int32_t)awake);
}

static void write_back(World* w) {
    WriteJob j = { 0, 0, (uint32_t*)P.sreq.base };
    if (!re_vm_ensure(&P.sreq, ((size_t)P.nbodies + 64u) * 4u)) return;
    j.touch = (uint32_t*)P.sreq.base;
    re_parallel_for((int)((P.nbodies + BLOCK_BODIES - 1) / BLOCK_BODIES), write_fn, &j);
    uint32_t n = (uint32_t)j.ntouch;
    qsort(j.touch, n, 4, cmp_u32); // (the change log in the bodies' order)
    for (uint32_t a = 0; a < n; a++) ecs_touch(w, bodies()[j.touch[a]].e);
    P.stats.awake = (uint32_t)j.awake;
}

// A body broke: pieces n x n x n of its box (cubes), each a body moving as that part of it moved, pushed apart a little
// (after write_back: destroying and creating entities moves the archetypes' rows, so the components are fetched again)
static void fracture(World* w, Body* b, uint32_t* budget) {
    const CRigidBody* src = ecs_get(w, b->e, COMP_RIGID_BODY);
    const CTransform* st = ecs_get(w, b->e, COMP_TRANSFORM);
    if (!src || !st) return;
    uint32_t n = src->fracture_pieces < 2 ? 2 : src->fracture_pieces > 4 ? 4 : src->fracture_pieces;
    if (n * n * n > *budget) return;
    const CStaticMesh* sm = ecs_get(w, b->e, COMP_STATIC_MESH);
    CStaticMesh look = sm ? *sm : (CStaticMesh){ 0 };
    CRigidBody proto = *src;
    float p[3], R[3][3], v[3], wv[3], half[3], rot[3];
    memcpy(R, b->R, sizeof R), memcpy(v, b->v, 12), memcpy(wv, b->w, 12), memcpy(half, b->g.half, 12);
    for (int k = 0; k < 3; k++) p[k] = b->p[k] + b->g.bc[0] * R[0][k] + b->g.bc[1] * R[1][k] + b->g.bc[2] * R[2][k]; // (the bounds' center)
    memcpy(rot, st->rotation, 12);
    uint32_t flags = st->flags;
    float m = 1.0f / b->inv_mass;
    ecs_destroy(w, b->e); // (Play's snapshot brings it back when play ends)
    for (uint32_t ix = 0; ix < n; ix++)
        for (uint32_t iy = 0; iy < n; iy++)
            for (uint32_t iz = 0; iz < n; iz++) {
                Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_RIGID_BODY));
                if (e == ENTITY_NONE) return;
                (*budget)--;
                P.stats.pieces_spawned++;
                float l[3] = { -half[0] + (2.0f * ix + 1.0f) * half[0] / n, -half[1] + (2.0f * iy + 1.0f) * half[1] / n, -half[2] + (2.0f * iz + 1.0f) * half[2] / n };
                CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                float r[3], jit = 0.86f + 0.12f * frand();
                for (int k = 0; k < 3; k++) r[k] = l[0] * R[0][k] + l[1] * R[1][k] + l[2] * R[2][k];
                for (int k = 0; k < 3; k++) t->location[k] = p[k] + r[k];
                memcpy(t->rotation, rot, 12);
                t->rotation[0] += (frand() - 0.5f) * 6.0f, t->rotation[2] += (frand() - 0.5f) * 6.0f;
                t->scale[0] = 2.0f * half[0] / n / 100.0f * jit;
                t->scale[1] = 2.0f * half[1] / n / 100.0f * jit;
                t->scale[2] = 2.0f * half[2] / n / 100.0f * jit;
                t->flags = (flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
                CStaticMesh* pm = ecs_get(w, e, COMP_STATIC_MESH);
                *pm = look;
                pm->mesh = SHAPE_CUBE;
                CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
                *rb = proto;
                rb->fracture = 0, rb->sleeping = 0, rb->sleep_time = 0.0f, rb->started = 1;
                rb->mass = m / (float)(n * n * n);
                float wr[3], out = len3(r) > 1e-3f ? 160.0f / len3(r) : 0.0f;
                cross3(wv, r, wr);
                for (int k = 0; k < 3; k++) rb->velocity[k] = v[k] + wr[k] + r[k] * out + (frand() - 0.5f) * 60.0f;
                for (int k = 0; k < 3; k++) rb->angular_velocity[k] = wv[k] + (frand() - 0.5f) * 6.0f;
                CName* nm = ecs_get(w, e, COMP_NAME);
                snprintf(nm->text, sizeof nm->text, "Fragment");
            }
}


// --- The frame ---

void physics_begin_play(World* w) {
    if (!P.ready) {
        size_t sb = (size_t)MAX_BODIES + 4096u + 2u;
        bool ok = re_vm_reserve(&P.bodies, (size_t)MAX_BODIES * sizeof(Body)) && re_vm_reserve(&P.map, (size_t)RE_ECS_MAX_ENTITIES * 4u) &&
                  re_vm_ensure(&P.map, (size_t)RE_ECS_MAX_ENTITIES * 4u) && re_vm_reserve(&P.statics, (size_t)RE_ECS_MAX_ENTITIES * sizeof(Static)) &&
                  re_vm_reserve(&P.cells, (BUCKETS + 1) * 4u) && re_vm_ensure(&P.cells, (BUCKETS + 1) * 4u) &&
                  re_vm_reserve(&P.refs, (size_t)RE_ECS_MAX_ENTITIES * 64u * 4u) && re_vm_reserve(&P.big, (size_t)RE_ECS_MAX_ENTITIES * 4u) &&
                  re_vm_reserve(&P.joints, (size_t)MAX_JOINTS * sizeof(Joint)) && re_vm_reserve(&P.state, sb * sizeof(State)) &&
                  re_vm_reserve(&P.delta, sb * sizeof(Delta)) && re_vm_reserve(&P.awake, (size_t)MAX_BODIES * 4u) &&
                  re_vm_reserve(&P.cmask, (size_t)MAX_BODIES * 8u + 4096u) && re_vm_reserve(&P.parent, (size_t)MAX_BODIES * 8u + 4096u) &&
                  re_vm_reserve(&P.mint, (size_t)MAX_BODIES * 4u + 4096u) && re_vm_reserve(&P.fast, ((size_t)MAX_BODIES * 3u + (size_t)MAX_PAIRS * 2u) * 4u) &&
                  re_vm_reserve(&P.ccd_t, (size_t)MAX_BODIES * 4u + 4096u) && re_vm_reserve(&P.bigb, (size_t)MAX_BODIES * 4u + 4096u) &&
                  re_vm_reserve(&P.sbig, (size_t)MAX_BODIES * 4u + 4096u) && re_vm_reserve(&P.dynb, (size_t)MAX_BODIES + 4096u) && re_vm_reserve(&P.pairs, (size_t)MAX_PAIRS * sizeof(Pair)) &&
                  re_vm_reserve(&P.mans, (size_t)MAX_MANIFOLDS * sizeof(Man)) && re_vm_reserve(&P.mdest, (size_t)MAX_MANIFOLDS * 16u) &&
                  re_vm_reserve(&P.contacts, (size_t)MAX_MANIFOLDS * sizeof(Contact)) && re_vm_reserve(&P.points, (size_t)MAX_POINTS * sizeof(Pt)) &&
                  re_vm_reserve(&P.blk, (size_t)1u << 22) && re_vm_reserve(&P.wides, ((size_t)MAX_MANIFOLDS / FW_LANES + MAX_COLORS + 1u) * sizeof(Wide)) &&
                  re_vm_reserve(&P.gcnt, (size_t)MAX_BODIES * 4u + 4096u) && re_vm_reserve(&P.mnp, (size_t)MAX_MANIFOLDS + 4096u) && re_vm_reserve(&P.iparam, (size_t)MAX_BODIES * sizeof(IParam) + 4096u) && re_vm_reserve(&P.sreq, (size_t)MAX_BODIES * 4u + 4096u) && re_vm_reserve(&P.gcur, ((size_t)GRID_BUCKETS_MAX + GRID_BUCKETS_MAX / GB_BUCKETS + 4u) * 4u);
        for (int g = 0; g < 2 && ok; g++) {
            Grid* gr = g ? &P.sleep_grid : &P.awake_grid;
            ok = re_vm_reserve(&gr->heads, ((size_t)GRID_BUCKETS_MAX + 2u) * 4u) && re_vm_reserve(&gr->ents, (size_t)MAX_BODIES * 64u * sizeof(GEnt)) &&
                 re_vm_reserve(&gr->tmp, (size_t)MAX_BODIES * 64u * sizeof(GEnt));
        }
        for (int k = 0; k < 2 && ok; k++)
            ok = re_vm_reserve(&P.rec[k], (size_t)MAX_POINTS * sizeof(Rec)) && re_vm_reserve(&P.tab[k], (size_t)MAX_MANIFOLDS * 2u * sizeof(Slot));
        if (!ok) return;
        memset(P.map.base, 0xFF, (size_t)RE_ECS_MAX_ENTITIES * 4u);
        P.ready = true;
    }
    memset(&P.stats, 0, sizeof P.stats);
    phys_shape_counters(&P.gjk0, &P.epa0, &P.fb0);
    P.playing = true, P.rebuild = true;
    P.acc = 0.0f, P.step = 1.0f / 120.0f;
    P.rng = 0x9E3779B9u;
    P.nstatics = P.nkin = 0;
    P.tab_size[0] = P.tab_size[1] = 0;
    P.statics_revision = ~0u; // gathered on the first frame with bodies
    P.sleep_version++, P.groups = 0;
    P.ccd_sweeps = 0;
    g_solver_threads = getenv("REALENGINE_PHYS_THREADS") ? atoi(getenv("REALENGINE_PHYS_THREADS")) : 0;
    g_scalar = getenv("REALENGINE_PHYS_SCALAR") && atoi(getenv("REALENGINE_PHYS_SCALAR"));
    (void)w;
}

void physics_end_play(World* w) {
    (void)w;
    uint32_t* map = bmap();
    for (uint32_t i = 0; P.ready && i < P.nbodies; i++) map[entity_index(bodies()[i].e)] = ~0u;
    P.playing = false;
    P.nbodies = P.nstatics = P.ncontacts = P.njoints = P.npairs = P.nawake = 0;
}

static bool any_body(const World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & COMP_BIT(COMP_RIGID_BODY)) && a->count) return true;
    }
    return false;
}

void physics_update(World* w, float dt) {
    if (!P.ready || !P.playing) return;
    P.stats.ms = 0.0;
    P.frame_dt = dt;
    if (!any_body(w)) { // nothing costs without bodies
        P.stats.bodies = P.stats.awake = P.stats.contacts = 0;
        P.nbodies = 0, P.rebuild = true;
        return;
    }
    int rate = project()->physics_rate >= 30 && project()->physics_rate <= 240 ? project()->physics_rate : 120;
    P.step = 1.0f / (float)rate;
    P.acc = fminf(P.acc + dt, STEP * MAX_STEPS);
    if (P.acc < STEP) { // no 120 Hz step this frame (a fast frame rate): nothing moves, nothing to gather or write back
        P.stats.play_time += dt, P.stats.play_frames++;
        return;
    }
    P.stats.bodies = P.stats.contacts = 0;
    double t0 = re_time_seconds();
    if (MC.cap && MC.gen != render_meshes_generation()) { // (an import while playing: the cache's hulls and trees freed)
        mesh_cache_check();
        P.statics_revision = ~0u, P.rebuild = true;
    }
    // the level's colliders: once, again when the level changed in bulk, and every quarter second for a small level
    // (Blueprints move doors and platforms)
    if (P.statics_revision != w->revision || (P.nstatics - P.nkin < 20000 && ++P.statics_frames >= 30)) {
        gather_statics(w);
        P.statics_revision = w->revision;
        P.statics_frames = 0;
    }
    gather_characters(w, P.nstatics - P.nkin);
    if (!sync_bodies(w)) rebuild_bodies(w);
    if (!reserve_bodies(P.nbodies + 1)) return;
    gather_joints(w);
    water_gather(w); // (B12f: the water bodies for the Buoyancy components)
    P.stats.bodies = P.nbodies;
    double t_sync = (re_time_seconds() - t0) * 1000.0;
    uint32_t steps = 0, contacts_total = 0;
    P.t_broad = P.t_narrow = P.t_solve = 0.0;
    memset(P.t_phase, 0, sizeof P.t_phase);
    while (P.acc >= STEP && steps < MAX_STEPS) {
        step(STEP);
        P.acc -= STEP;
        steps++;
        contacts_total += P.ncontacts;
    }
    P.stats.contacts = steps ? contacts_total / steps : 0;
    phys_shape_counters(&P.stats.gjk, &P.stats.epa, &P.stats.epa_fallback);
    P.stats.gjk -= P.gjk0, P.stats.epa -= P.epa0, P.stats.epa_fallback -= P.fb0;
    P.stats.ccd_sweeps = (uint32_t)P.ccd_sweeps;
    double tw = re_time_seconds();
    write_back(w);
    // the bodies that broke: pieces now (the bodies are gathered again next frame); the Bodies' pointers are stale after
    uint32_t budget = MAX_PIECES_FRAME;
    Body* bd = bodies();
    bool broke = false;
    for (uint32_t i = 0; i < P.nbodies; i++) {
        Body* b = &bd[i];
        float imp = b->impact;
        b->impact = 0.0f;
        const CRigidBody* rb = broke ? ecs_get(w, b->e, COMP_RIGID_BODY) : b->rb; // (a piece made: the rows moved)
        if (!rb || !rb->fracture || imp <= rb->fracture_speed) continue;
        fracture(w, b, &budget);
        broke = true;
    }
    if (broke) P.rebuild = true;
    double t_write = (re_time_seconds() - tw) * 1000.0;
    // the bodies' share of the step (the Debug Viewer): by the contacts each had
    double ms = (re_time_seconds() - t0) * 1000.0;
    uint32_t tc = 0;
    for (uint32_t i = 0; i < P.nbodies && !broke; i++) tc += bd[i].contacts + 1;
    for (uint32_t i = 0; i < P.nbodies && !broke; i++) bd[i].ms = ms * (double)(bd[i].contacts + 1) / (double)(tc ? tc : 1);
    P.stats.ms = ms;
    if (ms > P.stats.ms_peak) P.stats.ms_peak = ms;
    P.stats.play_time += dt;
    P.stats.ms_sum += ms, P.stats.play_frames++, P.stats.threads = (uint32_t)re_pool_threads();
    if (P.stats.awake * 2 >= P.nbodies && P.nbodies)
        P.stats.busy_ms += ms, P.stats.busy_frames++, P.stats.busy_steps += steps, P.stats.busy_broad_ms += P.t_broad, P.stats.busy_narrow_ms += P.t_narrow,
            P.stats.busy_solve_ms += P.t_solve;
    if (P.stats.awake * 2 >= P.nbodies && P.nbodies)
        for (int k = 0; k < 12; k++) P.stats.phase_ms[k] += P.t_phase[k];
    for (int k = 0; k < 12; k++) P.stats.all_phase_ms[k] += P.t_phase[k];
    P.stats.all_steps += steps, P.stats.frame_ms[0] += t_sync, P.stats.frame_ms[1] += t_write;
    if (!P.stats.settle_time && P.stats.awake * 4 <= P.nbodies && P.stats.play_time > 0.5f) P.stats.settle_time = P.stats.play_time;
}

const PhysicsStats* physics_stats(void) { return &P.stats; }

const char* physics_phase_name(int i) {
    static const char* const n[12] = { "bodies", "broadphase", "narrowphase", "colors", "contacts", "solver", "finalize", "ccd+joints", "cache", "islands", "(grids)", "(pairs)" };
    return i >= 0 && i < 12 ? n[i] : "";
}

// --- Blueprints' and the game's calls (the components changed: the bodies take them in next frame) ---

static Body* body_for(World* w, Entity e) {
    (void)w;
    int32_t k = P.ready && P.playing ? body_of(e) : -1;
    return k >= 0 ? &bodies()[k] : NULL;
}

static float mass_of(World* w, Entity e, CRigidBody* rb) {
    Body* b = body_for(w, e);
    if (b) return b->mass;
    if (rb->mass > 0.0f) return rb->mass;
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    float off[3];
    Geom g;
    if (t) geom_of(sm, t, rb, false, &g, off);
    const PhysMaterial* pm = rb->phys_material ? physmat_get(rb->phys_material) : NULL;
    float vol = !t ? 1e6f : geom_volume(&g);
    return pm && pm->density > 0.0f ? fmaxf(vol * pm->density * 1e-3f, 0.05f) : fmaxf(vol * 1e-4f, 0.05f);
}

static void inv_inertia_of(World* w, Entity e, CRigidBody* rb, float I[6]) {
    Body* b = body_for(w, e);
    if (b) {
        inv_world(b->R, b->inv_il, b->g.kind != SHAPE_HULL, I);
        return;
    }
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    float off[3], L[3][3], R[3][3];
    Geom g;
    memset(I, 0, 24);
    if (!t) return;
    geom_of(sm, t, rb, false, &g, off);
    geom_inertia(&g, mass_of(w, e, rb), L);
    rotator_axes(t->rotation, R);
    inv_world(R, L, g.kind != SHAPE_HULL, I);
}

static CRigidBody* sim_rb(World* w, Entity e) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    return rb && rb->simulate ? rb : NULL;
}

void physics_add_impulse(World* w, Entity e, const float impulse[3], bool vel_change) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    float m = vel_change ? 1.0f : mass_of(w, e, rb);
    for (int k = 0; k < 3; k++) rb->velocity[k] += impulse[k] / m;
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_add_impulse_at(World* w, Entity e, const float impulse[3], const float location[3]) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    float m = mass_of(w, e, rb), I[6], r[3], t[3], dw[3];
    if (rb->sleeping) { // (Q9) a body at rest under a push no larger than its weight's over a frame stays asleep (a box resting on a cushion)
        float j2 = impulse[0] * impulse[0] + impulse[1] * impulse[1] + impulse[2] * impulse[2], wg = m * 981.0f / 30.0f; // (its weight over a 30 fps frame)
        if (j2 < wg * wg) return;
    }
    Body* b = body_for(w, e);
    const CTransform* tr = ecs_get(w, e, COMP_TRANSFORM);
    const float* c = b ? b->p : tr ? tr->location : location;
    for (int k = 0; k < 3; k++) r[k] = location[k] - c[k], rb->velocity[k] += impulse[k] / m;
    inv_inertia_of(w, e, rb, I);
    cross3(r, impulse, t), imul(I, t, dw);
    for (int k = 0; k < 3; k++) rb->angular_velocity[k] += dw[k];
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_add_force(World* w, Entity e, const float force[3], bool accel_change) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    float dt = P.frame_dt > 0.0f ? fminf(P.frame_dt, 0.1f) : STEP, m = accel_change ? 1.0f : mass_of(w, e, rb);
    for (int k = 0; k < 3; k++) rb->velocity[k] += force[k] / m * dt;
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_add_torque(World* w, Entity e, const float torque[3], bool accel_change) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    float dt = P.frame_dt > 0.0f ? fminf(P.frame_dt, 0.1f) : STEP, I[6], dw[3];
    if (accel_change) for (int k = 0; k < 3; k++) dw[k] = torque[k] * 0.0174533f * dt; // (deg/s2)
    else inv_inertia_of(w, e, rb, I), imul(I, torque, dw), dw[0] *= dt, dw[1] *= dt, dw[2] *= dt;
    for (int k = 0; k < 3; k++) rb->angular_velocity[k] += dw[k];
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_add_angular_impulse(World* w, Entity e, const float impulse[3], bool vel_change) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    float I[6], dw[3];
    if (vel_change) for (int k = 0; k < 3; k++) dw[k] = impulse[k] * 0.0174533f; // (deg/s added)
    else inv_inertia_of(w, e, rb, I), imul(I, impulse, dw);
    for (int k = 0; k < 3; k++) rb->angular_velocity[k] += dw[k];
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_set_linear_velocity(World* w, Entity e, const float v[3], bool add) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    for (int k = 0; k < 3; k++) rb->velocity[k] = (add ? rb->velocity[k] : 0.0f) + v[k];
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_set_angular_velocity(World* w, Entity e, const float deg_s[3], bool add) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    for (int k = 0; k < 3; k++) rb->angular_velocity[k] = (add ? rb->angular_velocity[k] : 0.0f) + deg_s[k] * 0.0174533f;
    rb->sleeping = 0, rb->sleep_time = 0.0f;
}

void physics_get_angular_velocity(World* w, Entity e, float deg_s[3]) {
    const CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    for (int k = 0; k < 3; k++) deg_s[k] = rb ? rb->angular_velocity[k] * 57.2957795f : 0.0f;
}

float physics_get_mass(World* w, Entity e) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    return rb ? mass_of(w, e, rb) : 0.0f;
}

void physics_set_mass(World* w, Entity e, float kg) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    if (rb) rb->mass = fmaxf(kg, 0.0f);
}

void physics_wake(World* w, Entity e, bool awake) {
    CRigidBody* rb = sim_rb(w, e);
    if (!rb) return;
    rb->sleeping = awake ? 0u : 1u, rb->sleep_time = 0.0f;
    if (!awake) memset(rb->velocity, 0, 12), memset(rb->angular_velocity, 0, 12);
}

bool physics_is_awake(World* w, Entity e) {
    const CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    return rb && rb->simulate && !rb->sleeping;
}

void physics_set_gravity(World* w, Entity e, bool on) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    if (rb) rb->enable_gravity = on, rb->sleeping = 0;
}

void physics_set_material(World* w, Entity e, uint32_t material) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    if (rb) rb->phys_material = material, rb->sleeping = 0;
}

void physics_set_simulate(World* w, Entity e, bool on) {
    CRigidBody* rb = ecs_get(w, e, COMP_RIGID_BODY);
    if (!rb && on) {
        if (!ecs_add(w, e, COMP_RIGID_BODY)) return;
        rb = ecs_get(w, e, COMP_RIGID_BODY);
    }
    if (!rb) return;
    rb->simulate = on;
    rb->sleeping = 0, rb->sleep_time = 0.0f;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t && on) t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    P.statics_revision = ~0u; // it left (or joined) the static colliders
    P.rebuild = true;
}

void physics_set_sleep_enabled(bool on) { g_sleep_off = !on; }

void physics_set_ccd(World* w, Entity e, bool on) {
    CRigidBody* rb = ecs_alive(w, e) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    if (rb) rb->ccd = on;
}

uint32_t physics_profile_items(Entity* e, float* ms, uint32_t* ncontacts, uint32_t cap) {
    uint32_t n = 0;
    const Body* bd = bodies();
    for (uint32_t i = 0; i < P.nbodies && n < cap; i++) {
        if (!bd[i].awake && !bd[i].contacts) continue;
        e[n] = bd[i].e, ms[n] = (float)bd[i].ms, ncontacts[n] = bd[i].contacts;
        n++;
    }
    return n;
}

void physics_radial_impulse(World* w, const float origin[3], float radius, float strength) {
    if (radius <= 0.0f) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        CRigidBody* rb = ecs_arch_column(a, COMP_RIGID_BODY);
        for (uint32_t i = 0; i < a->count; i++) {
            if (!rb[i].simulate) continue;
            float d[3] = { tr[i].location[0] - origin[0], tr[i].location[1] - origin[1], tr[i].location[2] - origin[2] }, l = len3(d);
            if (l >= radius) continue;
            float k = strength * (1.0f - l / radius) / (l > 1e-3f ? l : 1.0f);
            for (int c = 0; c < 3; c++) rb[i].velocity[c] += l > 1e-3f ? d[c] * k : (c == 2 ? strength : 0.0f);
            rb[i].sleeping = 0, rb[i].sleep_time = 0.0f;
        }
    }
}

// --- Physics Constraints from Blueprints (e: the constraint's actor) ---

static CConstraint* constraint_of(World* w, Entity e) { return ecs_alive(w, e) ? ecs_get(w, e, COMP_CONSTRAINT) : NULL; }

static void wake_joint(World* w, const CConstraint* c) {
    CRigidBody* a = ecs_alive(w, c->actor1) ? ecs_get(w, c->actor1, COMP_RIGID_BODY) : NULL;
    CRigidBody* b = ecs_alive(w, c->actor2) ? ecs_get(w, c->actor2, COMP_RIGID_BODY) : NULL;
    if (a) a->sleeping = 0;
    if (b) b->sleeping = 0;
}

void physics_constraint_break(World* w, Entity e) {
    CConstraint* c = constraint_of(w, e);
    if (c && !c->broken) c->broken = 1, P.stats.joints_broken++, wake_joint(w, c);
}

void physics_constraint_motor(World* w, Entity e, bool on, float speed) {
    CConstraint* c = constraint_of(w, e);
    if (c) c->motor = on, c->motor_speed = speed, wake_joint(w, c);
}

void physics_constraint_drive(World* w, Entity e, bool angular, const float target[3], const float velocity[3], uint32_t bits, float hz, float damping,
                              float max) {
    CConstraint* c = constraint_of(w, e);
    if (!c) return;
    if (angular) memcpy(c->ang_target, target, 12), memcpy(c->ang_target_vel, velocity, 12), c->ang_drive = bits, c->ang_drive_max = max;
    else memcpy(c->lin_target, target, 12), memcpy(c->lin_target_vel, velocity, 12), c->lin_drive = bits, c->lin_drive_max = max;
    if (hz >= 0.0f) c->drive_hz = hz;
    if (damping > 0.0f) c->drive_damping = damping;
    wake_joint(w, c);
}

void physics_constraint_limits(World* w, Entity e, float linear, float twist, float swing1, float swing2) {
    CConstraint* c = constraint_of(w, e);
    if (!c) return;
    c->lin_limit = linear, c->twist_limit = twist, c->swing1_limit = swing1, c->swing2_limit = swing2;
    if (c->type == CONSTRAINT_HINGE) c->limit = twist;
    if (c->type == CONSTRAINT_PRISMATIC) c->limit = linear;
    wake_joint(w, c);
}

void physics_constraint_motion(World* w, Entity e, uint32_t axis, uint32_t motion) {
    CConstraint* c = constraint_of(w, e);
    if (!c || axis >= 6 || motion >= CMOTION_COUNT) return;
    if (axis < 3) c->lin_motion[axis] = motion;
    else c->ang_motion[axis - 3] = motion;
    wake_joint(w, c);
}

bool physics_constraint_force(World* w, Entity e, float* force, float* torque) {
    CConstraint* c = constraint_of(w, e);
    if (force) *force = c ? c->force : 0.0f;
    if (torque) *torque = c ? c->torque : 0.0f;
    return c && c->broken;
}
