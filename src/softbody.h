// Soft body (Q9, notas-dev/soft-body.md): a Cloth of kind CLOTH_KIND_SOFT (components.h) is a volume. Its actor's Static
// Mesh is embedded in a lattice of cubes (the cells the mesh fills: its surface sampled, the outside flooded from the
// lattice's border), each cube 6 tetrahedra (Kuhn's: no orientation to keep across cubes); the lattice's vertices are
// simulated by the cloth's Vertex Block Descent (cloth.c, cloth_sim.comp) on stable Neo-Hookean elasticity (Smith et al.
// 2018: no inversion, the volume kept by Poisson's ratio), the mesh's vertices follow by their trilinear place in their
// cell (skin.comp, the renderer's skin path: its own material, shadows, rays). This file is the CPU's part: the lattice
// built from a mesh once per mesh and settings, the presets, a selftest.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"

#define SOFT_MAX_RES 32

typedef struct SoftTopo {
    uint32_t key[12];
    uint32_t nx, ny, nz;     // lattice vertices per axis (cells + 1; a cell of padding each side, never filled)
    uint32_t nv, nactive;    // nx * ny * nz; the active ones (a filled cell around them)
    float cell;              // cm (the actor's scaled space)
    float origin[3];         // cm: vertex (0, 0, 0) in the actor's scaled space
    uint8_t* cells;          // a vertex's 8 cells filled: bit bx + 2 by + 4 bz is the cell (i - 1 + bx, j - 1 + by, k - 1 + bz)
    float* rest;             // 4 a vertex: its rest place (cm, scaled actor space), w its hold (0 free .. 1 pinned)
    float* aux;              // 2 a vertex: x how far outside the mesh's surface it is (m), y its cells' bits
    uint8_t* color;          // a vertex's color ((i & 1) + 2 (j & 1) + 4 (k & 1): no tetrahedron joins two of one color)
    float* embed;            // 4 a render vertex (the mesh's LOD 0): its lattice coordinates (i + fx, j + fy, k + fz), w 0
    uint32_t nrender;
    uint32_t filled, pinned, held; // filled cells; hard pinned vertices; held by a spring (0 < weight < 1)
    float rest_volume;       // m3: the filled cells' (the tetrahedra's at rest)
    float radius;            // cm: from its center to the farthest active vertex
    float center[3];         // cm: the active vertices' mean (scaled actor space)
    double build_ms;
} SoftTopo;

// The key: a lattice is built again when the mesh, its scale or these settings change
void soft_topo_key(uint32_t key[12], uint32_t mesh, const float scale[3], const CCloth* c);
// From the renderer's mesh (its LOD 0: render_mesh_cpu) in the actor's scale; false when it isn't a mesh
bool soft_topo_build(SoftTopo* t, uint32_t mesh, const float scale[3], const CCloth* c);
// From arrays: positions (cm, already scaled), triangles; colors (RGBA8 a vertex, NULL: none) for SOFT_ATTACH_PAINTED
bool soft_topo_build_arrays(SoftTopo* t, const float* pos, uint32_t npos, const uint32_t* idx, uint32_t nidx, const uint32_t* colors,
                            const CCloth* c);
void soft_topo_free(SoftTopo* t);

void soft_apply_preset(CCloth* c, int preset);
extern const char* const g_soft_preset_names[SOFT_PRESET_COUNT];
extern const char* const g_soft_attach_names[SOFT_ATTACH_COUNT];
// a lattice cube's 6 tetrahedra: their corners as bits (x 1, y 2, z 4)
extern const uint8_t g_soft_tets[6][4];
// The tetrahedra's volume (m3) at these places (4 floats a lattice vertex, m): the volume test's
double soft_volume(const SoftTopo* t, const float* pos4);
int soft_selftest(void);
