// Ray tracing by software (A1, notas-dev/gi.md "GI sem ray tracing por hardware"): on a GPU without VK_KHR_ray_query
// (Android, the Quest, AMD/Intel before RDNA 2 / Arc), the GI's rays (the surface cache, the probes, the occluders' light)
// walk BVHs in buffers instead of the TLAS: one per mesh (built on the CPU when the meshes load: binned SAH, up to 4
// triangles a leaf, the triangles' positions in the leaves' order) and one over the instances (the static set when the
// scene changes, the dynamic set every frame it changed). The shaders: sw_rt.glsl, the same calls as rt.glsl
// (closest_in / visible_in: RE_SWRT). FrameBuf.tlas and .tlas_dyn hold the two sets' headers' addresses then;
// GpuMeshInfo.blas a mesh's root node (| 1 << 32). REALENGINE_NO_SWRT=1: off (the GI of the sky and the lightmaps, as
// before).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "render_gpu.h"
#include "gi.h"

struct VkCtx;

// A node: two children's boxes (min.w: the child's index; max.w: its count: >0 a leaf's range of triangles or instances,
// 0 an inner node, ~0 none). The GPU layout (sw_rt.glsl SwNode)
typedef struct SwNode {
    float amn[3];
    uint32_t a;
    float amx[3];
    uint32_t acount;
    float bmn[3];
    uint32_t b;
    float bmx[3];
    uint32_t bcount;
} SwNode;

// An instance as the instances' BVH leaves hold it (the static or the dynamic set)
typedef struct SwInst {
    uint32_t slot; // InstanceBuf's index (the hit's instance)
    uint32_t mask; // rt.glsl's RT_MASK_ bits
    float mn[3], mx[3];
} SwInst;

bool swrt_available(struct VkCtx* vk); // no ray query, the env allows it
// The meshes' BVHs (all of them, again on every reload). False: none (the GI stays without rays)
// prim_base[i]: added to a hit's triangle (the mesh's range is a coarser LOD's: its triangles counted from LOD 0's)
bool swrt_meshes(struct VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, const int32_t* prim_base, int n);
// The same in two steps: the CPU's part on any thread (swrt_prepare: NULL, none), the GPU's on the main one (swrt_commit:
// frees the prepared); swrt_drop: one thrown away
typedef struct SwPrep SwPrep;
SwPrep* swrt_prepare(const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, const int32_t* prim_base, int n);
bool swrt_commit(struct VkCtx* vk, SwPrep* p);
void swrt_drop(SwPrep* p);
uint64_t swrt_mesh_root(int mesh); // (GpuMeshInfo.blas: the root node | 1 << 32; 0: none)
// A set's BVH over its instances, into the frame slot's buffer (which: 0 static, 1 dynamic): its header's address (0:
// empty)
VkDeviceAddress swrt_set(struct VkCtx* vk, int which, uint32_t frame_slot, const SwInst* inst, uint32_t n);
void swrt_shutdown(struct VkCtx* vk);
typedef struct SwStats {
    uint32_t meshes, mesh_nodes, triangles, static_inst, dyn_inst;
    double mesh_mb, mesh_ms, set_ms;
} SwStats;
const SwStats* swrt_stats(void);
