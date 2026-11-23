// Cloth (cloth.c, notas-dev/tecido.md): every cloth's vertices in one set of buffers, found from a header (pc.instances).
// A cloth is a regular grid (res_x x res_y) hanging in its actor's XZ plane; its springs come from the grid itself
// (no adjacency stored): stretch to the 4 neighbors, shear to the 4 diagonals, bending to the 4 at distance 2. A spring
// torn sets a bit in its vertex's mask (12 bits: the order of CL_NB).
// Or a mesh of its own (CLF_MESH, cloth_mesh.h): nx its vertices, ny 1; its rest places (pinned in w), UVs, springs
// (adjacency lists: stretch along the edges, bending across them) and triangles in the header's m* buffers; its
// normals written by pass 9. A vertex cut (mask != 0) drops its springs and its triangles.
#ifndef CLOTH_GLSL
#define CLOTH_GLSL

struct ClothInfo {
    vec4 rows[3];       // actor -> world (cm)
    uint base, nx, ny, flags;    // flags: CLF_*
    float dx, dy, mass, inv_h;    // rest spacing (m), mass a vertex (kg), 1 / substep (1/s)
    float k_stretch, k_shear, k_bend, damping;
    float friction, thickness, tear, gravity; // thickness (m), tear: strain that tears (0 never), gravity scale
    vec4 wind;          // m/s, w: gust (0..1)
    uint col_first, col_count, color, iter;  // its colliders; a mesh's: its first triangle (in mtris), its triangles
    vec4 tint;          // rgb, roughness
    vec4 look;          // sheen, two-sided light, self collision radius (m; 0: off), time
    // (Q9) a soft body's (CLF_SOFT): x its lattice's vertices along y (nx is along x, ny = y * z: nx * ny its count), y 1
    // asleep (nothing moves), z 1 its hold let go (Set Soft Body Pinned), w its plasticity's yield (m)
    vec4 soft;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer ClothInfoBuf { ClothInfo c[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer ClothVec4 { vec4 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer ClothUint { uint v[]; };
// The header: the buffers' addresses, the counts
struct ClothHeader {
    uvec2 info, pos, prev, vel, target, owner, mask, colliders;
    uint ncloth, nverts, ncolliders, color;    // color: the pass's (0..8), the colliders
    vec4 time;                                 // x: time (s), y: dt of a substep, z: the substep, w: -
    vec4 cut;                                  // a cut (Blueprint Tear Cloth): center (m), radius (m; 0: none)
    uvec2 order;                               // the vertices by color (32 lists, cloth.c builds them on a layout change)
    uint cfirst[32], ccount[32];               // each color's list in it
    uvec2 idx;                                 // the triangles for its BLAS (cloth_sim.comp pass 5)
    uvec2 head, next, selfnb;                  // self collision: the hash's buckets, each vertex's next, 4 contacts a vertex
    float cell;                                // the hash's cell (m: twice the largest radius)
    uint nbuckets;                             // (a power of 2; 0: no cloth collides with itself)
    uvec2 mrest, muv, madj_off, madj, mtris, mnormal; // the meshes': rest (cm, actor space; w pinned), UV, adjacency (first,
                                               // count; entries: other | kind << 30, rest m bits or triangle), triangles, normals
    // (Q9) a soft body reuses them: mrest its lattice's rest (cm, scaled actor space; w its hold 0..1), muv x how far outside
    // its mesh the vertex is (m), y its 8 cells' bits; mnormal its plastic offset (m, actor space: the dent it keeps)
    vec4 kick, kick_vel;                       // Add Soft Body Impulse: center (m), radius (m; 0 none); velocity (m/s), w the cloth (-1 all)
    uvec2 feedback;                            // what the CPU reads back (cloth.c): the colliders' forces, each soft body's mean and speed
    uint fb_cloth, pad_h;                      // feedback's ints before the bodies' (8 a collider)
};
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer ClothHeaderBuf { ClothHeader h; };

#define CLF_PIN_TOP 1u
#define CLF_PIN_CORNERS 2u
#define CLF_PIN_LEFT 4u
#define CLF_PIN_ALL_CORNERS 8u
#define CLF_ACTIVE 16u
#define CLF_MESH 64u
#define CLF_SOFT 128u
#define CLF_BLOCK 256u // (Q9) a soft body solved by the block pass (cloth_sim.comp SOFT_BLOCK: its lattice in shared memory)
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer ClothUvec2 { uvec2 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer ClothUvec3 { uvec3 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer ClothVec2 { vec2 v[]; };
bool cl_mesh(ClothInfo c) { return (c.flags & CLF_MESH) != 0u; }

const ivec2 CL_NB[12] = ivec2[12](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),       // stretch
                                  ivec2(1, 1), ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1),     // shear
                                  ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2));      // bending
// a slot's mirror (the same spring seen from the neighbor)
const int CL_OPP[12] = int[12](1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10);

bool cl_pinned(ClothInfo c, ivec2 g) {
    if ((c.flags & CLF_PIN_TOP) != 0u && g.y == 0) return true;
    if ((c.flags & CLF_PIN_CORNERS) != 0u && g.y == 0 && (g.x == 0 || g.x == int(c.nx) - 1)) return true;
    if ((c.flags & CLF_PIN_LEFT) != 0u && g.x == 0) return true;
    if ((c.flags & CLF_PIN_ALL_CORNERS) != 0u && (g.x == 0 || g.x == int(c.nx) - 1) && (g.y == 0 || g.y == int(c.ny) - 1)) return true;
    return false;
}
// The rest place of a grid vertex in the actor's space (cm): x along the width, -z down the height
vec3 cl_rest_local(ClothInfo c, ivec2 g) { return vec3(float(g.x) * c.dx * 100.0 - (float(c.nx) - 1.0) * c.dx * 50.0, 0.0, -float(g.y) * c.dy * 100.0); }
vec3 cl_to_world(ClothInfo c, vec3 p) { vec4 q = vec4(p, 1.0); return vec3(dot(c.rows[0], q), dot(c.rows[1], q), dot(c.rows[2], q)); }

#endif
