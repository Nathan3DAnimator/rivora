// A cloth on a mesh of its own (cloth.c, notas-dev/tecido.md): an imported mesh's triangles made into the solver's
// graph. Built once per mesh and setting on the CPU (a few ms for 10 000 vertices), sent to the GPU on a layout change.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLOTH_MESH_MAX_VERTS 16384u // a cloth's (welded) vertices
#define CLOTH_MESH_MAX_ADJ 48u      // a vertex's springs and triangles
#define CLOTH_MESH_COLORS 32u       // the solver's colors (VBD: a color's vertices share no spring)

// adjacency entry: x = the other vertex (local) | kind << 30, y = the rest length (float bits, m) or the triangle (kind 2)
#define CLOTH_ADJ_STRETCH 0u
#define CLOTH_ADJ_BEND 1u
#define CLOTH_ADJ_TRI 2u

typedef struct ClothMeshTopo {
    uint32_t key[8];   // the mesh, its scale, the pin band and axis: built for these
    uint32_t nv, nt, nadj, ncolors;
    float* rest;       // nv x 4: the place in the cloth actor's space (cm), w: 1 pinned
    float* uv;         // nv x 2
    uint32_t* adj_off; // nv x 2: first entry, count
    uint32_t* adj;     // nadj x 2 (above)
    uint32_t* tris;    // nt x 3 (local vertices)
    uint8_t* color;    // nv
    float area;        // m2
    float edge;        // the mean edge (m)
    float radius;      // cm: the farthest vertex from the origin (its reach)
    uint32_t pinned;   // how many vertices are pinned
} ClothMeshTopo;

// Built from the renderer's copy of the mesh (render_mesh_cpu): the corners that are one place welded (a UV seam must
// not tear), the triangles, the stretch springs (the edges), the bending ones (the two vertices across each inner edge),
// each vertex's triangles (its normal), the colors (greedy, the most connected first), the pins: the vertices within
// band cm of the top along axis (pin false: none). False: not a mesh, or over the limits
bool cloth_mesh_build(ClothMeshTopo* t, uint32_t mesh, const float scale[3], float band, const float axis[3], bool pin);
void cloth_mesh_free(ClothMeshTopo* t);
// The key the topology was built for (a change: built again)
void cloth_mesh_key(uint32_t key[8], uint32_t mesh, const float scale[3], float band, const float axis[3], bool pin);
int cloth_mesh_selftest(void); // a tube with a UV seam: welded, its springs, colors that share no spring, the pins; 0 = ok
// (the self-test's topology from arrays: positions (cm) and triangles)
bool cloth_mesh_build_arrays(ClothMeshTopo* t, const float* pos, const float* uv, uint32_t ncorner_verts, const uint32_t* idx,
                             uint32_t ni, const float scale[3], float band, const float axis[3], bool pin);
