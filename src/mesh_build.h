// The imported meshes' processing (meshoptimizer, external/meshoptimizer): the importers' one corner per triangle welded
// into shared vertices, the triangles ordered for the vertex cache and for overdraw, the vertices for fetch, and LODs
// simplified from them (each with its geometric error, cull.comp picks the LOD whose error stays under a pixel).
// .rmesh v2 holds the result; v1 files (one corner per triangle) are processed the same way when they load.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "render.h"
#include "render_gpu.h"

// Up to this many triangles a mesh keeps one UV2 cell per triangle (its lightmap and surface cache chart: the props);
// past it the vertices are welded and it has no atlas (RMESH_NO_ATLAS: lit by the probes, like the characters' parts).
// A UV2 cell per triangle can't be welded, and a chart of millions of cells has under a texel each
#define MESH_ATLAS_MAX_TRIS 65536u

// Morph targets (RMESH_MORPHS): names, default weights and the deltas by vertex (CSR: vertex v's are d[first[v]] to
// d[first[v + 1] - 1]). The importers hand them by their source vertex (nv: the source's), the mesh keeps them by its own
typedef struct MeshMorphs {
    uint32_t count, nv, nd;
    char (*name)[RMESH_MORPH_NAME];
    float* weight;
    uint32_t* first;
    RMeshMorphDelta* d;
} MeshMorphs;
void mesh_morphs_free(MeshMorphs* m);

typedef struct MeshBuilt {
    GpuVertex* v;
    uint32_t nv;
    uint32_t* idx; // every LOD's, LOD 0 first
    uint32_t ni;
    RMeshLods lods;
    uint32_t flags; // RMESH_
    float mn[3], mx[3], area;
    double ms; // the processing's time
    MeshMorphs morph; // (count 0: none)
    uint32_t* col;    // the vertex colors, RGBA8 a vertex (RMESH_COLORS; NULL: none)
} MeshBuilt;

// From one corner per triangle (the importers' layout and v1 files): welded when past MESH_ATLAS_MAX_TRIS, ordered, LODs
// when lods. Leaves the input as it is
bool mesh_build_corners(const GpuVertex* corners, uint32_t ntri, bool lods, MeshBuilt* out);
// The same with a color per corner (RGBA8; NULL: as mesh_build_corners): corners of different colors never weld; the
// vertices the LODs make of their own take the color of LOD 0's vertex at their place
bool mesh_build_corners_col(const GpuVertex* corners, const uint32_t* colors, uint32_t ntri, bool lods, MeshBuilt* out);
// From an indexed mesh: ordered; atlas: its UV2 kept (the landscape's blocks), else uv2 -1 (RMESH_NO_ATLAS); LODs when lods
// A skin (RMESH_SKINNED) from one corner per triangle whose uv2 holds the bones and weights (render.h): welded (the
// corners that are the same vertex with the same weights), ordered, LODs on LOD 0's vertices (mesh_skinned_lods), no atlas
// morphs (may be NULL): the targets by source vertex, csrc each corner's source vertex (UINT32_MAX: none); the corners
// of different source vertices never weld, the deltas follow the welding and the reordering
bool mesh_build_skinned(const GpuVertex* corners, uint32_t ntri, const uint32_t* csrc, const MeshMorphs* morphs, MeshBuilt* out);
bool mesh_build_indexed(const GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, bool atlas, bool lods, MeshBuilt* out);
// A skin's LODs (RMESH_SKINNED, one LOD): simplified over the positions, on LOD 0's vertices (skin.comp bends those only),
// appended to idx. The importer's skins have them; older .rmesh skins get them when they load. False: none made
bool mesh_skinned_lods(MeshBuilt* mb);
void mesh_built_free(MeshBuilt* m);
bool rmesh_write(const char* path, const MeshBuilt* m);
// A .rmesh into memory: v2 as written, v1 processed (welding, LODs). False when unreadable or over the limits
bool rmesh_read(const char* path, MeshBuilt* out, uint32_t max_verts, uint32_t max_indices);

// A LOD's triangles as clusters (meshlets of up to 128 vertices and 128 triangles, meshopt_buildMeshlets): idx reordered
// in place cluster by cluster; each cluster's bounding sphere, normal cone (axis, cos cutoff) and index range (from idx's
// start). Returns how many (0: none, idx as it was). The cull tests them one by one (render.c, cull.comp pass 6)
typedef struct MeshCluster {
    float sphere[4], cone[4];
    uint32_t first, count;
} MeshCluster;
// perm (6 x cap, may be NULL): the clusters front to back seen along +X, -X, +Y, -Y, +Z, -Z (perm[o * n + k]: the k-th)
uint32_t mesh_clusters(const GpuVertex* v, uint32_t nv, uint32_t* idx, uint32_t ni, MeshCluster* out, uint32_t cap, uint32_t* perm);
// A big mesh's LODs in clusters (mesh_clusters per LOD; first: from mb->idx's start, mb->idx reordered): *out and *perm
// (6 per cluster) malloc'd, per LOD its first cluster and count. Kept in the cache folder by content (a 1.5 M triangle
// mesh: ~1 s). Returns the clusters (0: none)
uint32_t mesh_built_clusters(MeshBuilt* mb, MeshCluster** out, uint32_t** perm, uint32_t first[RMESH_MAX_LODS], uint32_t count[RMESH_MAX_LODS]);
// A big mesh's cluster LOD DAG (mesh_clod.h) over its LOD 0: *out, *perm (6 view orders per cluster, as above) and *idx
// (its own indices, into mb's vertices) malloc'd. Kept in the cache folder by content (~1-3 s for 1.5 M triangles).
// Returns the clusters (0: none)
#include "mesh_clod.h"
uint32_t mesh_built_dag(const MeshBuilt* mb, MeshDagCluster** out, uint32_t** perm, uint32_t** idx, uint32_t* nidx);
