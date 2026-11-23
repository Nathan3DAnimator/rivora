// The cluster LOD DAG (mesh_clod.h): meshoptimizer's clusterlod.h (MIT, external/meshoptimizer) behind a C function
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "meshoptimizer.h"
#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"

#include "mesh_clod.h"

extern "C" uint32_t mesh_clod_build(const float* pos, uint32_t stride, uint32_t nv, const float* attr, uint32_t nattr, const float* weights,
                                    uint32_t protect, const uint32_t* idx, uint32_t ni, MeshDagCluster** out, uint32_t** didx, uint32_t* dni,
                                    uint32_t* levels) {
    *out = nullptr, *didx = nullptr, *dni = 0, *levels = 0;
    clodConfig cfg = clodDefaultConfig(128);
    clodMesh m = {};
    m.indices = idx, m.index_count = ni, m.vertex_count = nv;
    m.vertex_positions = pos, m.vertex_positions_stride = stride;
    m.vertex_attributes = attr, m.vertex_attributes_stride = sizeof(float) * nattr;
    m.attribute_weights = weights, m.attribute_count = nattr;
    m.attribute_protect_mask = protect; // (permissive simplification: the uv seams kept)
    std::vector<clodBounds> groups;
    std::vector<MeshDagCluster> cl;
    std::vector<uint32_t> ix;
    int depth = 0;
    clodBuild(cfg, m, [&](clodGroup g, const clodCluster* cs, size_t n) -> int {
        int id = (int)groups.size();
        groups.push_back(g.simplified);
        if (g.depth > depth) depth = g.depth;
        for (size_t i = 0; i < n; i++) {
            MeshDagCluster d;
            memset(&d, 0, sizeof d);
            // (its own sphere: clusterlod gives a simplified cluster its whole group's, ~16 clusters wide, too big to fall
            // behind anything in the Hi-Z; the group's stays for the LOD's error)
            meshopt_Bounds cb = meshopt_computeClusterBounds(cs[i].indices, cs[i].index_count, pos, nv, stride);
            memcpy(d.sphere, cb.center, 12), d.sphere[3] = cb.radius;
            // its own error: the group it was simplified from (none: the original triangles, 0); its parent's: this group
            // simplified (FLT_MAX: the coarsest level, never replaced)
            const clodBounds& s = cs[i].refined >= 0 ? groups[(size_t)cs[i].refined] : g.simplified;
            memcpy(d.self_sphere, s.center, 12), d.self_sphere[3] = s.radius;
            d.self_err = cs[i].refined >= 0 ? s.error : 0.0f;
            memcpy(d.parent_sphere, g.simplified.center, 12), d.parent_sphere[3] = g.simplified.radius;
            d.parent_err = g.simplified.error;
            d.first = (uint32_t)ix.size(), d.count = (uint32_t)cs[i].index_count;
            ix.insert(ix.end(), cs[i].indices, cs[i].indices + cs[i].index_count);
            cl.push_back(d);
        }
        return id;
    });
    if (cl.empty()) return 0;
    *out = (MeshDagCluster*)malloc(sizeof(MeshDagCluster) * cl.size());
    *didx = (uint32_t*)malloc(sizeof(uint32_t) * ix.size());
    if (!*out || !*didx) {
        free(*out), free(*didx);
        *out = nullptr, *didx = nullptr;
        return 0;
    }
    memcpy(*out, cl.data(), sizeof(MeshDagCluster) * cl.size());
    memcpy(*didx, ix.data(), sizeof(uint32_t) * ix.size());
    *dni = (uint32_t)ix.size(), *levels = (uint32_t)depth + 1u;
    return (uint32_t)cl.size();
}
