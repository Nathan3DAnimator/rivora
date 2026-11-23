// The physics' collision shapes (physics.c, notas-dev/fisica.md "Formas de colisão"): convex hulls made from a mesh's
// vertices (quickhull over its extreme points, at most 64 vertices, faces merged), triangle meshes in a BVH for the
// level, and the convex pair test: GJK for the distance (speculative contacts within the margin), EPA for the
// penetration, and the manifold from the two supporting faces clipped (up to 4 points: a box or a hull resting on a
// face keeps its corners). Shapes are "cores" with a rounding radius (a sphere: a point, a capsule: a segment).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PHYS_HULL_MAX_V 64
#define PHYS_HULL_MAX_F 128
#define PHYS_HULL_MAX_FI 512

typedef struct PhysHull {
    uint32_t nv, nf;
    float v[PHYS_HULL_MAX_V][3];     // the mesh's space (cm, unscaled)
    float n[PHYS_HULL_MAX_F][3];     // the faces' outward normals
    uint16_t first[PHYS_HULL_MAX_F]; // each face's loop in idx
    uint8_t count[PHYS_HULL_MAX_F];
    uint8_t idx[PHYS_HULL_MAX_FI];
    float volume, com[3], cov[3][3]; // cm^3, the center of mass, the second moment about it (unit density)
    float mn[3], mx[3];
    uint32_t boxlike;                // it is its bounds' box (collides by the boxes' fast path)
    double build_ms;
} PhysHull;

typedef struct PhysMeshNode {
    float mn[3], mx[3];
    uint32_t first, count; // a leaf: count triangles from first; inner (count 0): the left child follows, first is the right
} PhysMeshNode;

typedef struct PhysMesh {
    uint32_t ntri, nnode;
    float (*tri)[3][3]; // the mesh's space, in the tree's order
    PhysMeshNode* node;
    float mn[3], mx[3];
    double build_ms;
} PhysMesh;

// From vertices (pos: 3 floats every stride bytes). False for a flat or empty set (collide as a box instead).
bool phys_hull_build(const void* pos, uint32_t stride, uint32_t n, PhysHull* h);
// The nearest face of a hull scaled by s, from its center of mass (cm): how far a skin may shrink it
float phys_hull_inner(const PhysHull* h, const float s[3]);
// From indexed triangles (indices into the vertices); NULL when empty. Freed with phys_mesh_free.
PhysMesh* phys_mesh_build(const void* pos, uint32_t stride, uint32_t nverts, const uint32_t* idx, uint32_t nidx);
void phys_mesh_free(PhysMesh* m);
// The triangles whose bounds overlap a box (the mesh's space): calls fn for each; stops when it returns false
typedef bool (*PhysTriFn)(void* user, const float tri[3][3]);
void phys_mesh_query(const PhysMesh* m, const float mn[3], const float mx[3], PhysTriFn fn, void* user);

// A convex shape in the world
enum { CVX_POINT = 0, CVX_SEGMENT, CVX_BOX, CVX_CYLINDER, CVX_HULL, CVX_TRIANGLE };
typedef struct Cvx {
    uint32_t kind, axis;  // axis: a segment's local axis
    float c[3], R[3][3];  // the center and the axes (rows: local X, Y, Z in the world)
    float h[3];           // a box's half extents; a segment: h[axis] its half length; a cylinder: h[0] radius, h[2] half height
    float radius;         // the rounding (a sphere's, a capsule's radius)
    const PhysHull* hull; // a hull: local point = s * vertex + o
    float s[3], o[3];
    float t[3][3], tn[3]; // a triangle and its normal (world)
} Cvx;

typedef struct PhysManifold {
    float n[3];           // from B to A
    uint32_t np;
    float p[4][3], depth[4]; // depth > 0: overlapping; < 0: a gap within the margin (speculative)
} PhysManifold;

// A against B: false when they are farther apart than margin. Counts (for the measures): GJK and EPA runs.
// (physics.c gives boxes, cylinders and hulls a skin: the core shrunk by it, its radius the skin, so two resting shapes
// touch with their cores apart and GJK alone answers; EPA only for a hit deeper than the skins)
bool phys_collide(const Cvx* a, const Cvx* b, float margin, PhysManifold* m);
void phys_shape_counters(uint32_t* gjk, uint32_t* epa, uint32_t* epa_fallback);
// (Q8, CCD) the distance of the rounded shapes (<= 0: touching), n from B to A; maxd + 1 when farther than maxd
float phys_distance(const Cvx* a, const Cvx* b, float maxd, float n[3]);
