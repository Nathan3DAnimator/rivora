// (A9.5a: copied from the Tlender, ../tlender/src/volume_grid.h, adapted: the engine's zlib and files; the format unchanged)
// Sparse voxel grids for exchange between the simulations, the files (OpenVDB, vdb.h) and the renderer: smoke and fire
// fields (density, temperature, flame, velocity), a liquid's level set, a granular medium's density. Leaves of 8^3
// voxels (OpenVDB's LeafNode size) found through a hash of their coordinates; a missing leaf reads the background.
//
// This is the interchange format, not a solver's working layout: a GPU solver keeps its own bricks and converts at
// the boundary (export, bake to disk, render upload). Index space -> world space by `xform` (the voxel center of
// index (i, j, k) is xform * (i, j, k); OpenVDB's cell-centered convention).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vg_math.h"

#define VG_LEAF_DIM 8
#define VG_LEAF_VOXELS (VG_LEAF_DIM * VG_LEAF_DIM * VG_LEAF_DIM)
#define VG_NAME_LEN 64

typedef enum VolumeGridType { VG_FLOAT = 0, VG_VEC3 = 1 } VolumeGridType;
typedef enum VolumeGridClass { VG_CLASS_FOG = 0, VG_CLASS_LEVEL_SET = 1, VG_CLASS_STAGGERED = 2, VG_CLASS_UNKNOWN = 3 } VolumeGridClass;

typedef struct VolumeLeaf {
    int32_t origin[3];        // index of voxel (0,0,0) of the leaf (multiples of 8)
    float* values;            // VG_LEAF_VOXELS * components, x fastest
    uint64_t active[VG_LEAF_VOXELS / 64]; // active voxels (OpenVDB's value mask)
} VolumeLeaf;

typedef struct VolumeGrid {
    char name[VG_NAME_LEN];   // "density", "temperature", "flame", "velocity", "surface"...
    int type;                 // VolumeGridType
    int grid_class;           // VolumeGridClass
    mat4 xform;               // index -> world
    float background[3];
    VolumeLeaf* leaves;
    int leaf_count, leaf_cap;
    int* hash;                // open addressing over leaf origins (leaf index + 1, 0 = empty)
    int hash_cap;
} VolumeGrid;

// The grids of one frame (OpenVDB's file: several named grids).
typedef struct VolumeGridSet {
    VolumeGrid* grids;
    int count, cap;
    float frame;              // the frame it belongs to (sequences)
} VolumeGridSet;

void volume_grid_init(VolumeGrid* g, const char* name, int type, float voxel_size);
void volume_grid_free(VolumeGrid* g);
bool volume_grid_copy(VolumeGrid* dst, const VolumeGrid* src);
int volume_grid_components(const VolumeGrid* g);
// Voxel access by index (set creates the leaf and marks the voxel active).
bool volume_grid_set(VolumeGrid* g, int i, int j, int k, const float* value);
void volume_grid_get(const VolumeGrid* g, int i, int j, int k, float* out);
VolumeLeaf* volume_grid_leaf(const VolumeGrid* g, int i, int j, int k); // the leaf holding the voxel, NULL
VolumeLeaf* volume_grid_touch_leaf(VolumeGrid* g, int i, int j, int k); // created (background) if missing
// Trilinear sample at a world position.
void volume_grid_sample(const VolumeGrid* g, vec3 world, float* out);
// Bounds of the active leaves in index space (false when empty) and in world space.
bool volume_grid_index_bounds(const VolumeGrid* g, int lo[3], int hi[3]);
bool volume_grid_world_bounds(const VolumeGrid* g, vec3* lo, vec3* hi);
size_t volume_grid_memory(const VolumeGrid* g);
float volume_grid_voxel_size(const VolumeGrid* g); // length of the xform's first column

void volume_set_free(VolumeGridSet* set);
VolumeGrid* volume_set_add(VolumeGridSet* set, const char* name, int type, float voxel_size);
VolumeGrid* volume_set_find(const VolumeGridSet* set, const char* name);
