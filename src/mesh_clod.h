// The cluster LOD DAG of a big mesh (notas-dev/render.md "LOD de clusters"; mesh_clod.cpp over meshoptimizer's
// clusterlod.h, MIT): its LOD 0 cut in clusters of up to 128 triangles, groups of them simplified together level after
// level (the group's border locked: no cracks between levels) into a DAG. The GPU draws, per cluster, the ones whose own
// error on screen is under the limit and whose parent's is over it (cull.comp pass 6): one object near and far at once.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A cluster of the DAG: its bounding sphere (the cull), the spheres and errors (cm, mesh space) of its own simplification
// (0: the original triangles) and of its group's (FLT_MAX: the coarsest, never replaced), its indices' range
typedef struct MeshDagCluster {
    float sphere[4];
    float self_sphere[4], parent_sphere[4];
    float self_err, parent_err;
    uint32_t first, count;
} MeshDagCluster;

// pos: 3 floats per vertex (stride bytes); attr: nattr floats per vertex weighted by weights (normal, uv), protect: the
// attributes whose seams stay (bits). *out and *idx malloc'd (indices into the same vertices). Returns the clusters
uint32_t mesh_clod_build(const float* pos, uint32_t stride, uint32_t nv, const float* attr, uint32_t nattr, const float* weights,
                         uint32_t protect, const uint32_t* idx, uint32_t ni, MeshDagCluster** out, uint32_t** didx, uint32_t* dni,
                         uint32_t* levels);

#ifdef __cplusplus
}
#endif
