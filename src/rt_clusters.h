// The static meshes' BLAS made of clusters (A8, NVIDIA's Mega Geometry: VK_NV_cluster_acceleration_structure;
// notas-dev/rt-clusters.md). Each mesh's triangles cut in clusters of 64 to 128 triangles and up to 256 vertices by
// meshoptimizer's builder for ray tracing (meshopt_buildMeshletsSpatial), each built into a cluster acceleration
// structure (CLAS: compressed by the driver), and a BLAS per mesh that references its clusters. Every structure is sized
// first (the driver's exact sizes) and built at its place: nothing allocated for the worst case. Each triangle keeps
// its own geometry index, its place in the mesh + 1, so a ray query's hit finds its triangle as the geometry index - 1,
// or the primitive index on a classic BLAS (one geometry, index 0: rt.glsl). The shaders are the same for both and need
// no NVIDIA extension.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "render_gpu.h"
#include "gi.h"

struct VkCtx;

typedef struct RtcSet {
    GpuBuf clas, blas;      // the clusters' structures, the BLAS (exact sizes)
    uint32_t meshes, clusters, triangles;
    double clas_mb, blas_mb, build_ms;
} RtcSet;

// The same in two steps: the clusters on the CPU on any thread (rtc_prepare: NULL, none), the builds on the main one
// (rtc_commit: frees the prepared; false, nothing made); rtc_drop: a prepared one thrown away
typedef struct RtcPrep RtcPrep;
RtcPrep* rtc_prepare(struct VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n);
bool rtc_commit(struct VkCtx* vk, RtcPrep* p, VkDeviceAddress* out, RtcSet* set);
void rtc_drop(struct VkCtx* vk, RtcPrep* p);
// Builds every mesh's BLAS from clusters (skip[i]: left out, its address 0). out[i]: its BLAS's address. False: nothing
// made (the caller builds the classic ones)
bool rtc_build(struct VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n,
               VkDeviceAddress* out, RtcSet* set);
void rtc_free(struct VkCtx* vk, RtcSet* set);
