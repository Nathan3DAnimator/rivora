#include "material.h" // (Q4: mat_set, the Light Functions)
#include "gi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "rt_clusters.h"
#include "sw_rt.h"
#include "vkctx.h"

static const uint32_t s_probe_trace[] =
#include "probe_trace.comp.inc"
    ;
static const uint32_t s_probe_update[] =
#include "probe_update.comp.inc"
    ;
static const uint32_t s_occluder_light[] =
#include "occluder_light.comp.inc"
    ;
// the same by software (A1, sw_rt.glsl: RE_SWRT), on the GPUs without ray queries
static const uint32_t s_probe_trace_sw[] =
#include "probe_trace_sw.comp.inc"
    ;
static const uint32_t s_occluder_light_sw[] =
#include "occluder_light_sw.comp.inc"
    ;
static const uint32_t s_pathtrace[] =
#include "pathtrace.comp.inc"
    ;
static const uint32_t s_view2[] =
#include "view2.comp.inc"
    ;
static bool g_bake_no_emissive; // (Lighting Levels: gi_bake_set_no_emissive)
static uint32_t g_bake_stride = 1; // (Q16) a bake in the game's frame budget: a probe out of every stride a frame (1: all)

// The probe grid (cells of probe_spacing cm around the camera): 32 x 32 x 12 probes, 8x8 texels each + a border
#define PROBES_X 32
#define PROBES_Y 32
#define PROBES_Z 12
#define PROBE_TILE 10
#define PROBE_RAYS_MAX 64
#define ATLAS_W (PROBES_X * PROBES_Z * PROBE_TILE)
#define ATLAS_H (PROBES_Y * PROBE_TILE)
#define VOLUME_IRR_BYTES ((size_t)ATLAS_W * ATLAS_H * 8)                // RGBA16F
#define VOLUME_BYTES (VOLUME_IRR_BYTES + (size_t)ATLAS_W * ATLAS_H * 4) // + RG16F

typedef struct Blas {
    GpuBuf buf;
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
    VkMicromapEXT omm; // its opacity micromap (gi_blas_omm; VK_NULL_HANDLE: none), alive as long as the BLAS
    GpuBuf omm_buf;
} Blas;

typedef struct Tlas {
    GpuBuf buf, scratch;
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
    uint32_t cap, built; // built: the instance count of the last build (a refit needs the same)
} Tlas;

static struct {
    bool rt;
    bool sw; // (A1: no ray queries, the rays by software: sw_rt.c; the GI's probes and cache as with them)
    VkCtx* vk;
    VkDevice dev;
    PFN_vkCreateAccelerationStructureKHR create_as;
    PFN_vkDestroyAccelerationStructureKHR destroy_as;
    PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_addr;
    PFN_vkCreateMicromapEXT create_mm; // (VK_EXT_opacity_micromap: vk->omm)
    PFN_vkDestroyMicromapEXT destroy_mm;
    PFN_vkGetMicromapBuildSizesEXT mm_sizes;
    PFN_vkCmdBuildMicromapsEXT cmd_build_mm;
    uint32_t scratch_align;
    Blas blas[RE_MAX_MESHES];
    GpuBuf blas_pool; // (gi_blas_prepare's: every BLAS in one buffer)
    int nblas;
    RtcSet clusters;  // A8: the BLAS made of clusters (rt_clusters.c) in place of the classic ones: their addresses in blas[]
    // 0: the static scene (its instances made on the GPU from the slots: rebuilt on edits), 1: the dynamic set (the
    // Movable actors, from the CPU: rebuilt every frame, small). The ray queries trace both (rt.glsl).
    Tlas tlas[2];
    GpuImage probe_irr, probe_dist, accum, lightmap[3];
    GpuBuf probe_rays, probe_state, occ_light;
    // The probe slots' check (the measures): the GPU counts, after the last slot of probe_state, the probes that found
    // another cell of the current grid in their slot (never right: two cells sharing one slot reset each other at each
    // update, the actors lit by the probes flickered); copied here each measured frame
    GpuBuf probe_check;
    bool check_on;
    uint32_t check_base; // (the count when the measure started)
    VkPipeline probe_trace, probe_update, pathtrace, occluder_light, view2;
    VkPipelineLayout layout;
    bool probes_cleared;
    bool atlas_full;     // the probe atlases at their size (not the 1x1 stand-ins)
    bool update, upload; // this frame: trace the probes / put the baked volume back in the atlases
    // the baked volume: its grid and its atlases in host memory; whether the atlases hold it now
    GpuBuf volume;
    GiVolume vol, bake;
    bool vol_present, baking, atlas_volume;
    uint32_t bake_frames;
    GiStats stats;
} G;

bool gi_available(void) { return G.rt; }
bool gi_sw(void) { return G.sw; }
bool gi_traced(void) { return G.rt || G.sw; }
static bool g_far_need = true; // (A2: the scene reaches past the near grid: render.c each frame)
void gi_set_far_needed(bool need) { g_far_need = need; }
VkImageView gi_probe_irradiance_view(void) { return G.probe_irr.view; }
VkImageView gi_probe_distance_view(void) { return G.probe_dist.view; }
VkImageView gi_accum_view(void) { return G.accum.view; }
VkImageView gi_lightmap_view(int i) { return G.lightmap[i >= 0 && i < 3 ? i : 0].view; }
VkDeviceAddress gi_probe_rays_address(void) { return G.probe_rays.addr; }
VkDeviceAddress gi_probe_state_address(void) { return G.probe_state.addr; }
const GiStats* gi_stats(void) { return &G.stats; }
void gi_probe_check(bool on) {
    G.check_on = on && G.probe_check.mapped;
    if (G.probe_check.mapped) G.check_base = *(volatile const uint32_t*)G.probe_check.mapped;
}
uint32_t gi_probe_collisions(void) {
    if (!G.probe_check.mapped) return 0u;
    uint32_t n = *(volatile const uint32_t*)G.probe_check.mapped;
    return n >= G.check_base ? n - G.check_base : n; // (the probes cleared since: the count started over)
}
void gi_reset_probes(void) { G.probes_cleared = false; }

static VkDeviceAddress as_address(VkAccelerationStructureKHR as) {
    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = as;
    return G.as_addr(G.dev, &ai);
}

static bool create_as(VkCtx* vk, GpuBuf* buf, VkAccelerationStructureKHR* as, VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
    if (!gpu_buf_create(vk, buf, size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false)) return false;
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = buf->buffer;
    ci.size = size;
    ci.type = type;
    return G.create_as(vk->device, &ci, NULL, as) == VK_SUCCESS;
}

// (A1) the meshes' software BVHs, from the CPU copy (render_sw_meshes: the LOD with up to max triangles)
static bool sw_build(VkCtx* vk) {
    const GpuVertex* cv;
    const uint32_t* ci;
    uint32_t cnv;
    render_cpu_mesh_data(&cv, &ci, &cnv);
    static GiMesh gm[RE_MAX_MESHES];
    static int32_t base[RE_MAX_MESHES];
    // (LOD 0: a coarser LOD measured slower in the heavy scene's foliage, its holes letting the rays run on, and it
    // would hit its own surface where LOD 0 draws it a few cm away)
    int nm = render_sw_meshes(gm, base, 0xFFFFFFFFu);
    return swrt_meshes(vk, cv, cnv, ci, gm, base, nm);
}

// A8: every mesh's BLAS made of clusters (rt_clusters.c), where the device has them; false: the classic ones
static bool clusters_try(VkCtx* vk, const GiMesh* meshes, int n) {
    if (!vk->clas || n <= 0 || n > RE_MAX_MESHES) return false;
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t nv;
    render_cpu_mesh_data(&v, &idx, &nv);
    static VkDeviceAddress out[RE_MAX_MESHES];
    rtc_free(vk, &G.clusters);
    if (!rtc_build(vk, v, nv, idx, meshes, n, out, &G.clusters)) {
        printf("[RealEngine] BLAS de clusters: nao deu (as BLAS classicas no lugar)\n");
        return false;
    }
    for (int i = 0; i < n; i++) memset(&G.blas[i], 0, sizeof G.blas[i]), G.blas[i].addr = out[i];
    G.nblas = n;
    return true;
}

static bool build_blas(VkCtx* vk, VkDeviceAddress vaddr, uint32_t vertex_count, VkDeviceAddress iaddr, const GiMesh* meshes, int n) {
    if (clusters_try(vk, meshes, n)) return true;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    GpuBuf scratch[RE_MAX_MESHES];
    memset(scratch, 0, sizeof scratch);
    bool ok = true;
    for (int i = 0; i < n && i < RE_MAX_MESHES && ok; i++) {
        VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR* tri = &geo.geometry.triangles;
        tri->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri->vertexData.deviceAddress = vaddr + (VkDeviceAddress)meshes[i].vertex_offset * sizeof(GpuVertex);
        tri->vertexStride = sizeof(GpuVertex);
        tri->maxVertex = vertex_count - (uint32_t)meshes[i].vertex_offset - 1;
        tri->indexType = VK_INDEX_TYPE_UINT32;
        tri->indexData.deviceAddress = iaddr + (VkDeviceAddress)meshes[i].first_index * sizeof(uint32_t);
        VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geo;
        uint32_t prims = meshes[i].index_count / 3;
        VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        G.build_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
        Blas* b = &G.blas[i];
        ok = create_as(vk, &b->buf, &b->as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize) &&
             gpu_buf_create(vk, &scratch[i], sz.buildScratchSize + G.scratch_align,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        if (!ok) break;
        bi.dstAccelerationStructure = b->as;
        bi.scratchData.deviceAddress = (scratch[i].addr + G.scratch_align - 1) & ~(VkDeviceAddress)(G.scratch_align - 1);
        VkAccelerationStructureBuildRangeInfoKHR range = { prims, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        G.cmd_build(cmd, 1, &bi, &pr);
        G.nblas = i + 1;
    }
    gpu_end_once(vk, cmd);
    for (int i = 0; i < RE_MAX_MESHES; i++) gpu_buf_destroy(vk, &scratch[i]);
    double mb = 0.0;
    for (int i = 0; i < G.nblas; i++) G.blas[i].addr = as_address(G.blas[i].as), mb += (double)G.blas[i].buf.size / 1048576.0;
    printf("[RealEngine] BLAS: %d meshes, %.1f MB\n", G.nblas, mb);
    return ok;
}

static void tlas_geometry(VkAccelerationStructureGeometryKHR* geo, VkAccelerationStructureBuildGeometryInfoKHR* bi,
                          VkDeviceAddress instances) {
    memset(geo, 0, sizeof *geo);
    geo->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo->geometry.instances.data.deviceAddress = instances;
    memset(bi, 0, sizeof *bi);
    bi->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bi->type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // refit when only transforms moved (the Movable actors), rebuilt when the scene changed
    bi->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    bi->geometryCount = 1;
    bi->pGeometries = geo;
}

// The TLAS with room for cap instances (grows to a power of two: a rebuild waits for the GPU anyway)
static bool tlas_reserve(VkCtx* vk, Tlas* t, uint32_t cap) {
    if (t->as && cap <= t->cap) return true;
    uint32_t c = t->cap ? t->cap : 1024;
    while (c < cap) c *= 2;
    vkDeviceWaitIdle(vk->device);
    if (t->as) G.destroy_as(vk->device, t->as, NULL);
    gpu_buf_destroy(vk, &t->buf);
    gpu_buf_destroy(vk, &t->scratch);
    t->as = VK_NULL_HANDLE;
    VkAccelerationStructureGeometryKHR geo;
    VkAccelerationStructureBuildGeometryInfoKHR bi;
    tlas_geometry(&geo, &bi, 0);
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    G.build_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &c, &sz);
    VkDeviceSize scratch = sz.buildScratchSize > sz.updateScratchSize ? sz.buildScratchSize : sz.updateScratchSize;
    if (!create_as(vk, &t->buf, &t->as, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sz.accelerationStructureSize) ||
        !gpu_buf_create(vk, &t->scratch, scratch + G.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false))
        return false;
    t->addr = as_address(t->as);
    t->cap = c;
    t->built = 0;
    return true;
}

static void blas_free(VkCtx* vk, Blas* b) {
    if (b->as) G.destroy_as(vk->device, b->as, NULL);
    gpu_buf_destroy(vk, &b->buf);
    if (b->omm) G.destroy_mm(vk->device, b->omm, NULL);
    gpu_buf_destroy(vk, &b->omm_buf);
    memset(b, 0, sizeof *b);
}

// A Masked mesh's BLAS again, with an opacity micromap (notas-dev/render.md "Opacity micromaps"): data (16 bytes a
// triangle, 4-state, level 3: render.c's omm.comp) for its ntri triangles. Its geometry no longer opaque: the micromap
// says which micro triangles the ray commits (opaque), skips (transparent) or hands the shader (unknown). Waits for the
// GPU (once per mesh). False: the BLAS stays as it was
bool gi_blas_omm(VkCtx* vk, int mesh, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices, const GiMesh* m,
                 VkDeviceAddress data, uint32_t ntri, uint32_t level) {
    if (!G.rt || !vk->omm || !G.create_mm || mesh < 0 || mesh >= G.nblas || !ntri) return false;
    uint32_t stride = (1u << (2u * level)) / 4u; // (bytes a triangle: 2 bits a micro triangle)
    // the triangles' table (each its data at 16 * i, level 3, 4-state) and the BLAS's micromap indices (each its own)
    GpuBuf tris = { 0 }, ids = { 0 }, mm = { 0 }, scratch = { 0 }, ascratch = { 0 };
    Blas nb;
    memset(&nb, 0, sizeof nb);
    bool ok = gpu_buf_create(vk, &tris, (VkDeviceSize)ntri * sizeof(VkMicromapTriangleEXT), VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT, true) &&
              gpu_buf_create(vk, &ids, (VkDeviceSize)ntri * 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    if (ok) {
        VkMicromapTriangleEXT* t = tris.mapped;
        uint32_t* id = ids.mapped;
        for (uint32_t i = 0; i < ntri; i++)
            t[i].dataOffset = i * stride, t[i].subdivisionLevel = (uint16_t)level, t[i].format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT, id[i] = i;
    }
    VkMicromapUsageEXT usage = { ntri, level, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT };
    VkMicromapBuildInfoEXT mb = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
    mb.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    mb.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
    mb.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    mb.usageCountsCount = 1;
    mb.pUsageCounts = &usage;
    VkMicromapBuildSizesInfoEXT ms = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
    if (ok) G.mm_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mb, &ms);
    ok = ok && ms.micromapSize && gpu_buf_create(vk, &nb.omm_buf, ms.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false) &&
         gpu_buf_create(vk, &mm, ms.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    if (ok) {
        VkMicromapCreateInfoEXT ci = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
        ci.buffer = nb.omm_buf.buffer;
        ci.size = ms.micromapSize;
        ci.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        ok = G.create_mm(vk->device, &ci, NULL, &nb.omm) == VK_SUCCESS;
    }
    // the BLAS: the mesh's triangles, not opaque, the micromap by index
    VkAccelerationStructureTrianglesOpacityMicromapEXT tom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT };
    tom.indexType = VK_INDEX_TYPE_UINT32;
    tom.indexBuffer.deviceAddress = ids.addr;
    tom.indexStride = 4;
    tom.micromap = nb.omm;
    tom.usageCountsCount = 1; // (the BLAS's size counts the micromap's triangles: without them the build overran it)
    tom.pUsageCounts = &usage;
    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = 0; // (not opaque: the micromap's unknown micro triangles reach the shader)
    VkAccelerationStructureGeometryTrianglesDataKHR* tri = &geo.geometry.triangles;
    tri->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri->pNext = &tom;
    tri->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri->vertexData.deviceAddress = vertices->addr + (VkDeviceAddress)m->vertex_offset * sizeof(GpuVertex);
    tri->vertexStride = sizeof(GpuVertex);
    tri->maxVertex = vertex_count - (uint32_t)m->vertex_offset - 1;
    tri->indexType = VK_INDEX_TYPE_UINT32;
    tri->indexData.deviceAddress = indices->addr + (VkDeviceAddress)m->first_index * sizeof(uint32_t);
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    if (ok) G.build_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &ntri, &sz);
    ok = ok && create_as(vk, &nb.buf, &nb.as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize) &&
         gpu_buf_create(vk, &ascratch, sz.buildScratchSize + G.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    if (ok) {
        VkCommandBuffer cmd = gpu_begin_once(vk);
        mb.dstMicromap = nb.omm;
        mb.data.deviceAddress = data;
        mb.triangleArray.deviceAddress = tris.addr;
        mb.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        mb.scratchData.deviceAddress = (mm.addr + 255) & ~(VkDeviceAddress)255;
        G.cmd_build_mm(cmd, 1, &mb);
        VkMemoryBarrier2 mbar = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mbar.srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
        mbar.srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
        mbar.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mbar.dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mbar;
        vkCmdPipelineBarrier2(cmd, &dep);
        bi.dstAccelerationStructure = nb.as;
        bi.scratchData.deviceAddress = (ascratch.addr + G.scratch_align - 1) & ~(VkDeviceAddress)(G.scratch_align - 1);
        VkAccelerationStructureBuildRangeInfoKHR range = { ntri, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        G.cmd_build(cmd, 1, &bi, &pr);
        gpu_end_once(vk, cmd);
        nb.addr = as_address(nb.as);
        vkDeviceWaitIdle(vk->device); // (the frames in flight trace the old BLAS)
        blas_free(vk, &G.blas[mesh]);
        G.blas[mesh] = nb;
    } else
        blas_free(vk, &nb);
    gpu_buf_destroy(vk, &tris), gpu_buf_destroy(vk, &ids), gpu_buf_destroy(vk, &mm), gpu_buf_destroy(vk, &scratch),
        gpu_buf_destroy(vk, &ascratch);
    return ok;
}

// The BLAS made apart (gi.h): their sizes, one buffer for all of them and one scratch, on any thread (the meshes read
// again in the background: render_meshes_reload_async); the main thread only records the builds (gi_blas_commit)
struct GiBlasPrep {
    int n;
    bool ok;
    bool clas; // (A8: made of clusters at the commit; the classic ones there if that fails)
    RtcPrep* rtc; // (its clusters made here, on the worker: rtc_prepare)
    SwPrep* swp;  // (A1: the software BVHs made here, on the worker: swrt_prepare)
    VkDeviceAddress vbase, ibase;
    uint32_t vertex_count;
    GiMesh meshes[RE_MAX_MESHES];
    Blas blas[RE_MAX_MESHES];
    GpuBuf pool, scratch;
    VkDeviceSize scratch_off[RE_MAX_MESHES], as_off[RE_MAX_MESHES], as_size[RE_MAX_MESHES];
    VkDeviceAddress vaddr[RE_MAX_MESHES], iaddr[RE_MAX_MESHES];
    uint32_t maxv[RE_MAX_MESHES], prims[RE_MAX_MESHES];
};

static void blas_geometry(VkAccelerationStructureGeometryKHR* geo, VkAccelerationStructureBuildGeometryInfoKHR* bi, VkDeviceAddress va,
                          uint32_t maxv, VkDeviceAddress ia) {
    memset(geo, 0, sizeof *geo);
    geo->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR* tri = &geo->geometry.triangles;
    tri->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri->vertexData.deviceAddress = va;
    tri->vertexStride = sizeof(GpuVertex);
    tri->maxVertex = maxv;
    tri->indexType = VK_INDEX_TYPE_UINT32;
    tri->indexData.deviceAddress = ia;
    memset(bi, 0, sizeof *bi);
    bi->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bi->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi->geometryCount = 1;
    bi->pGeometries = geo;
}

static void dyn_geometry(VkAccelerationStructureGeometryKHR* geo, VkAccelerationStructureBuildGeometryInfoKHR* bi, VkDeviceAddress va,
                         uint32_t maxv, VkDeviceAddress ia) {
    blas_geometry(geo, bi, va, maxv, ia);
    bi->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
}

bool gi_dyn_blas_create(VkCtx* vk, GiDynBlas* b, uint32_t prims, uint32_t maxv) {
    memset(b, 0, sizeof *b);
    if (!G.rt || !prims) return false;
    VkAccelerationStructureGeometryKHR geo;
    VkAccelerationStructureBuildGeometryInfoKHR bi;
    dyn_geometry(&geo, &bi, 0, maxv, 0);
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    G.build_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
    VkDeviceSize scratch = sz.buildScratchSize > sz.updateScratchSize ? sz.buildScratchSize : sz.updateScratchSize;
    if (!create_as(vk, &b->buf, &b->as, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize) ||
        !gpu_buf_create(vk, &b->scratch, scratch + G.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) {
        gi_dyn_blas_free(vk, b);
        return false;
    }
    b->addr = as_address(b->as);
    b->prims = prims;
    return true;
}

void gi_dyn_blas_record(VkCommandBuffer cmd, GiDynBlas* b, VkDeviceAddress vertices, uint32_t maxv, VkDeviceAddress indices) {
    if (!b->as) return;
    VkAccelerationStructureGeometryKHR geo;
    VkAccelerationStructureBuildGeometryInfoKHR bi;
    dyn_geometry(&geo, &bi, vertices, maxv, indices);
    if (b->stride) geo.geometry.triangles.vertexStride = b->stride;
    bool refit = !b->always_build && b->updates % 64u != 0u; // (a full build the first time and every 64: a refit loosens as the pose strays)
    bi.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.srcAccelerationStructure = refit ? b->as : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = b->as;
    bi.scratchData.deviceAddress = (b->scratch.addr + G.scratch_align - 1) & ~(VkDeviceAddress)(G.scratch_align - 1);
    VkAccelerationStructureBuildRangeInfoKHR range = { b->prims, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    G.cmd_build(cmd, 1, &bi, &pr);
    b->updates++;
}

// (2026-11-24, item 4) many skins' BLAS in one vkCmdBuildAccelerationStructuresKHR (each its own scratch: one call, n infos;
// 5000 animated skins were 5000 calls a frame). The same refit / build rule as gi_dyn_blas_record
void gi_dyn_blas_record_batch(VkCommandBuffer cmd, const GiDynBlasJob* jobs, uint32_t n) {
    enum { CHUNK = 256 };
    VkAccelerationStructureGeometryKHR geo[CHUNK];
    VkAccelerationStructureBuildGeometryInfoKHR bi[CHUNK];
    VkAccelerationStructureBuildRangeInfoKHR range[CHUNK];
    const VkAccelerationStructureBuildRangeInfoKHR* pr[CHUNK];
    uint32_t m = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (m && (i == n || m == CHUNK)) G.cmd_build(cmd, m, bi, pr), m = 0;
        if (i == n) break;
        GiDynBlas* b = jobs[i].blas;
        if (!b->as) continue;
        dyn_geometry(&geo[m], &bi[m], jobs[i].vertices, jobs[i].maxv, jobs[i].indices);
        bi[m].pGeometries = &geo[m];
        if (b->stride) geo[m].geometry.triangles.vertexStride = b->stride;
        bool refit = !b->always_build && b->updates % 64u != 0u;
        bi[m].mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi[m].srcAccelerationStructure = refit ? b->as : VK_NULL_HANDLE;
        bi[m].dstAccelerationStructure = b->as;
        bi[m].scratchData.deviceAddress = (b->scratch.addr + G.scratch_align - 1) & ~(VkDeviceAddress)(G.scratch_align - 1);
        range[m] = (VkAccelerationStructureBuildRangeInfoKHR){ b->prims, 0, 0, 0 };
        pr[m] = &range[m];
        b->updates++;
        m++;
    }
}

void gi_dyn_blas_free(VkCtx* vk, GiDynBlas* b) {
    if (b->as) G.destroy_as(vk->device, b->as, NULL);
    gpu_buf_destroy(vk, &b->buf);
    gpu_buf_destroy(vk, &b->scratch);
    memset(b, 0, sizeof *b);
}

void gi_blas_drop(VkCtx* vk, GiBlasPrep* p) {
    if (!p) return;
    rtc_drop(vk, p->rtc);
    swrt_drop(p->swp);
    for (int i = 0; i < p->n; i++)
        if (p->blas[i].as) G.destroy_as(vk->device, p->blas[i].as, NULL);
    gpu_buf_destroy(vk, &p->pool);
    gpu_buf_destroy(vk, &p->scratch);
    free(p);
}

GiBlasPrep* gi_blas_prepare(VkCtx* vk, VkDeviceAddress vbase, uint32_t vertex_count, VkDeviceAddress ibase, const GiMesh* meshes, int n,
                            const GpuVertex* cpu_v, const uint32_t* cpu_i) {
    if (G.sw && cpu_v && cpu_i) { // (A1: the software BVHs on this worker; the commit only uploads them)
        GiBlasPrep* p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->swp = swrt_prepare(cpu_v, vertex_count, cpu_i, meshes, NULL, n);
        return p;
    }
    if (!G.rt) return NULL;
    GiBlasPrep* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->n = n < RE_MAX_MESHES ? n : RE_MAX_MESHES;
    if (vk->clas) { // (A8: the commit makes them of clusters, from the CPU copy then in place; nothing allocated here)
        p->clas = true, p->ok = true, p->vbase = vbase, p->ibase = ibase, p->vertex_count = vertex_count;
        memcpy(p->meshes, meshes, sizeof(GiMesh) * (size_t)p->n);
        p->rtc = rtc_prepare(vk, cpu_v, vertex_count, cpu_i, meshes, p->n); // (NULL: the commit makes them, or the classic ones)
        return p;
    }
    static const VkDeviceSize AS_ALIGN = 256; // (an acceleration structure's offset in its buffer)
    VkDeviceSize as_total = 0, sc_total = 0, sa = G.scratch_align ? G.scratch_align : 256;
    VkDeviceSize* as_off = p->as_off; // (in the heap: the threads' stacks are small)
    VkDeviceSize* as_size = p->as_size;
    for (int i = 0; i < p->n; i++) {
        p->vaddr[i] = vbase + (VkDeviceAddress)meshes[i].vertex_offset * sizeof(GpuVertex);
        p->iaddr[i] = ibase + (VkDeviceAddress)meshes[i].first_index * sizeof(uint32_t);
        p->maxv[i] = vertex_count - (uint32_t)meshes[i].vertex_offset - 1;
        p->prims[i] = meshes[i].index_count / 3;
        VkAccelerationStructureGeometryKHR geo;
        VkAccelerationStructureBuildGeometryInfoKHR bi;
        blas_geometry(&geo, &bi, p->vaddr[i], p->maxv[i], p->iaddr[i]);
        VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        G.build_sizes(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &p->prims[i], &sz);
        as_off[i] = as_total, as_size[i] = sz.accelerationStructureSize;
        as_total += (sz.accelerationStructureSize + AS_ALIGN - 1) / AS_ALIGN * AS_ALIGN;
        p->scratch_off[i] = sc_total;
        sc_total += (sz.buildScratchSize + sa - 1) / sa * sa;
    }
    bool ok = p->n > 0 && gpu_buf_create(vk, &p->pool, as_total, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false) &&
              gpu_buf_create(vk, &p->scratch, sc_total + sa, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    for (int i = 0; ok && i < p->n; i++) {
        VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = p->pool.buffer;
        ci.offset = as_off[i];
        ci.size = as_size[i];
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        ok = G.create_as(vk->device, &ci, NULL, &p->blas[i].as) == VK_SUCCESS;
    }
    p->ok = ok;
    return p;
}

void gi_blas_commit(VkCtx* vk, GiBlasPrep* p) {
    if (G.sw) { // (A1: the meshes' BVHs again: the worker's, else from the CPU copy now in place)
        SwPrep* sp = p ? p->swp : NULL;
        if (p) p->swp = NULL;
        gi_blas_drop(vk, p);
        G.sw = sp ? swrt_commit(vk, sp) : sw_build(vk);
        return;
    }
    if (!G.rt) {
        gi_blas_drop(vk, p);
        return;
    }
    for (int i = 0; i < G.nblas; i++) blas_free(vk, &G.blas[i]); // (the GPU is idle: the caller waited)
    gpu_buf_destroy(vk, &G.blas_pool);
    rtc_free(vk, &G.clusters);
    G.nblas = 0;
    if (p && p->clas) {
        static VkDeviceAddress out[RE_MAX_MESHES];
        bool made = false;
        if (p->rtc && rtc_commit(vk, p->rtc, out, &G.clusters)) { // (the worker's clusters)
            for (int i = 0; i < p->n; i++) memset(&G.blas[i], 0, sizeof G.blas[i]), G.blas[i].addr = out[i];
            G.nblas = p->n;
            made = true;
        }
        if (!made) made = build_blas(vk, p->vbase, p->vertex_count, p->ibase, p->meshes, p->n);
        free(p);
        if (!made) G.rt = false;
        return;
    }
    if (!p || !p->ok) {
        gi_blas_drop(vk, p);
        G.rt = false;
        return;
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkDeviceSize sa = G.scratch_align ? G.scratch_align : 256;
    VkDeviceAddress sbase = (p->scratch.addr + sa - 1) & ~(VkDeviceAddress)(sa - 1);
    for (int i = 0; i < p->n; i++) { // (each its own scratch range: no barrier between them)
        VkAccelerationStructureGeometryKHR geo;
        VkAccelerationStructureBuildGeometryInfoKHR bi;
        blas_geometry(&geo, &bi, p->vaddr[i], p->maxv[i], p->iaddr[i]);
        bi.dstAccelerationStructure = p->blas[i].as;
        bi.scratchData.deviceAddress = sbase + p->scratch_off[i];
        VkAccelerationStructureBuildRangeInfoKHR range = { p->prims[i], 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        G.cmd_build(cmd, 1, &bi, &pr);
    }
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &p->scratch);
    for (int i = 0; i < p->n; i++) G.blas[i].as = p->blas[i].as, G.blas[i].addr = as_address(p->blas[i].as);
    G.nblas = p->n;
    G.blas_pool = p->pool;
    printf("[RealEngine] BLAS: %d meshes, %.1f MB\n", p->n, (double)p->pool.size / 1048576.0);
    free(p);
}

void gi_rebuild_blas(VkCtx* vk, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices, const GiMesh* meshes, int nmeshes) {
    if (G.sw) {
        G.sw = sw_build(vk);
        return;
    }
    if (!G.rt) return;
    gi_blas_commit(vk, gi_blas_prepare(vk, vertices->addr, vertex_count, indices->addr, meshes, nmeshes, NULL, NULL));
}

VkDeviceAddress gi_blas_address(int mesh) {
    if (G.sw) return swrt_mesh_root(mesh); // (A1: the mesh's root node | 1 << 32, sw_rt.glsl)
    return G.rt && mesh >= 0 && mesh < G.nblas ? G.blas[mesh].addr : 0;
}

VkDeviceAddress gi_tlas(VkCtx* vk, VkCommandBuffer cmd, int which, VkDeviceAddress instances, uint32_t n, GiTlasWork work) {
    if (!G.rt) return 0;
    Tlas* t = &G.tlas[which & 1];
    if (which == 1 && n == 0) return 0; // no dynamic set: the shaders skip the second query
    if (work == GI_TLAS_KEEP && t->as && t->built == n) return t->addr;
    if (!tlas_reserve(vk, t, n ? n : 1)) return 0;
    if (!t->as) return 0;
    VkAccelerationStructureGeometryKHR geo;
    VkAccelerationStructureBuildGeometryInfoKHR bi;
    tlas_geometry(&geo, &bi, instances);
    // a refit keeps the instance count; anything else (or a TLAS just made) is a full build
    bool update = work == GI_TLAS_REFIT && t->built == n && n > 0;
    bi.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.srcAccelerationStructure = update ? t->as : VK_NULL_HANDLE;
    bi.dstAccelerationStructure = t->as;
    bi.scratchData.deviceAddress = (t->scratch.addr + G.scratch_align - 1) & ~(VkDeviceAddress)(G.scratch_align - 1);
    VkAccelerationStructureBuildRangeInfoKHR range = { n, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    // the previous frame's ray queries are done with it; the instances were just written by a compute shader
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
    G.cmd_build(cmd, 1, &bi, &pr);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    t->built = n;
    if (which == 0) G.stats.tlas_instances = n;
    G.stats.tlas_builds += update ? 0u : 1u;
    G.stats.tlas_refits += update ? 1u : 0u;
    return t->addr;
}

static bool make_image(VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h, VkFormat fmt) {
    return gpu_image_create(vk, im, w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

// Images in GENERAL from the start (sampled and stored), cleared
static void clear_general(VkCommandBuffer cmd, GpuImage* im) {
    gpu_transition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
}

bool gi_init(VkCtx* vk, VkPipelineLayout layout, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices,
             const GiMesh* meshes, int nmeshes) {
    memset(&G, 0, sizeof G);
    G.vk = vk;
    G.dev = vk->device;
    G.layout = layout;
    G.rt = vk->ray_query;
    if (G.rt) {
        G.create_as = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(vk->device, "vkCreateAccelerationStructureKHR");
        G.destroy_as = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(vk->device, "vkDestroyAccelerationStructureKHR");
        G.build_sizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(vk->device, "vkGetAccelerationStructureBuildSizesKHR");
        G.cmd_build = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(vk->device, "vkCmdBuildAccelerationStructuresKHR");
        G.as_addr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(vk->device, "vkGetAccelerationStructureDeviceAddressKHR");
        G.rt = G.create_as && G.destroy_as && G.build_sizes && G.cmd_build && G.as_addr;
        if (vk->omm) {
            G.create_mm = (PFN_vkCreateMicromapEXT)vkGetDeviceProcAddr(vk->device, "vkCreateMicromapEXT");
            G.destroy_mm = (PFN_vkDestroyMicromapEXT)vkGetDeviceProcAddr(vk->device, "vkDestroyMicromapEXT");
            G.mm_sizes = (PFN_vkGetMicromapBuildSizesEXT)vkGetDeviceProcAddr(vk->device, "vkGetMicromapBuildSizesEXT");
            G.cmd_build_mm = (PFN_vkCmdBuildMicromapsEXT)vkGetDeviceProcAddr(vk->device, "vkCmdBuildMicromapsEXT");
            if (!G.create_mm || !G.destroy_mm || !G.mm_sizes || !G.cmd_build_mm) G.create_mm = NULL;
        }
        VkPhysicalDeviceAccelerationStructurePropertiesKHR ap = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ap };
        vkGetPhysicalDeviceProperties2(vk->gpu, &p2);
        G.scratch_align = ap.minAccelerationStructureScratchOffsetAlignment ? ap.minAccelerationStructureScratchOffsetAlignment : 256;
    }
    if (G.rt && !build_blas(vk, vertices->addr, vertex_count, indices->addr, meshes, nmeshes)) G.rt = false;
    if (!G.rt && swrt_available(vk)) G.sw = sw_build(vk); // (A1: the GI's rays by software)
    const bool traced = G.rt || G.sw;

    // The probe atlases (1x1 stand-ins without ray tracing: the descriptor set needs them), the lightmaps (1x1 until a
    // bake), the path tracer's accumulation (sized by gi_resize)
    uint32_t aw = traced ? ATLAS_W : 1, ah = traced ? ATLAS_H * 2 : 1; // (A2: the far grid's tiles under the near grid's)
    G.atlas_full = traced;
    bool ok = make_image(vk, &G.probe_irr, aw, ah, VK_FORMAT_R16G16B16A16_SFLOAT) &&
              make_image(vk, &G.probe_dist, aw, ah, VK_FORMAT_R16G16_SFLOAT) &&
              make_image(vk, &G.lightmap[0], 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT) &&
              make_image(vk, &G.lightmap[1], 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT) &&
              make_image(vk, &G.lightmap[2], 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT) &&
              make_image(vk, &G.accum, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT);
    if (!ok) return false;
    uint32_t total = PROBES_X * PROBES_Y * PROBES_Z;
    if (traced) {
        // room for every probe's rays (a bake traces them all each frame)
        ok = gpu_buf_create(vk, &G.probe_rays, (VkDeviceSize)total * PROBE_RAYS_MAX * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false) &&
             gpu_buf_create(vk, &G.probe_state, (VkDeviceSize)total * 32 * 2 + 64, // (+ the slots' check: probe_update.comp; + the far grid's, A2)
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) &&
             gpu_buf_create(vk, &G.probe_check, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true) &&
             gpu_buf_create(vk, &G.occ_light, (VkDeviceSize)MAX_OCCLUDERS * 6 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        if (!ok) return false;
        G.probe_trace = G.rt ? gpu_compute_pipeline(vk, layout, s_probe_trace, sizeof s_probe_trace)
                             : gpu_compute_pipeline(vk, layout, s_probe_trace_sw, sizeof s_probe_trace_sw);
        G.probe_update = gpu_compute_pipeline(vk, layout, s_probe_update, sizeof s_probe_update);
        if (G.rt) { // (the path tracer and the second view: hardware only)
            G.pathtrace = gpu_compute_pipeline(vk, layout, s_pathtrace, sizeof s_pathtrace);
            G.view2 = gpu_compute_pipeline(vk, layout, s_view2, sizeof s_view2);
        }
        G.occluder_light = G.rt ? gpu_compute_pipeline(vk, layout, s_occluder_light, sizeof s_occluder_light)
                                : gpu_compute_pipeline(vk, layout, s_occluder_light_sw, sizeof s_occluder_light_sw);
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    clear_general(cmd, &G.probe_irr);
    clear_general(cmd, &G.probe_dist);
    clear_general(cmd, &G.lightmap[0]);
    clear_general(cmd, &G.lightmap[1]);
    clear_general(cmd, &G.lightmap[2]);
    clear_general(cmd, &G.accum);
    if (traced) vkCmdFillBuffer(cmd, G.probe_state.buffer, 0, VK_WHOLE_SIZE, 0);
    gpu_end_once(vk, cmd);
    G.probes_cleared = true;
    G.stats.probes = total;
    printf("[RealEngine] GI: %s (%d BLAS, %u sondas)\n",
           G.rt ? "ray tracing por hardware" : G.sw ? "ray tracing por software (A1)" : "sem ray tracing (luz do céu)", G.nblas,
           traced ? total : 0);
    return true;
}

bool gi_resize(VkCtx* vk, uint32_t w, uint32_t h) {
    gpu_image_destroy(vk, &G.accum);
    if (!make_image(vk, &G.accum, w * 2, h, VK_FORMAT_R32G32B32A32_SFLOAT)) return false; // (two halves: this frame's, the last one's)
    VkCommandBuffer cmd = gpu_begin_once(vk);
    clear_general(cmd, &G.accum);
    gpu_end_once(vk, cmd);
    return true;
}

void gi_shutdown(VkCtx* vk) {
    if (!G.dev) return;
    vkDeviceWaitIdle(vk->device);
    VkPipeline ps[] = { G.probe_trace, G.probe_update, G.pathtrace, G.occluder_light, G.view2 };
    for (int i = 0; i < (int)(sizeof ps / sizeof ps[0]); i++)
        if (ps[i]) vkDestroyPipeline(vk->device, ps[i], NULL);
    for (int i = 0; i < 2; i++) {
        if (G.tlas[i].as) G.destroy_as(vk->device, G.tlas[i].as, NULL);
        gpu_buf_destroy(vk, &G.tlas[i].buf);
        gpu_buf_destroy(vk, &G.tlas[i].scratch);
    }
    for (int i = 0; i < G.nblas; i++) blas_free(vk, &G.blas[i]);
    gpu_buf_destroy(vk, &G.blas_pool);
    rtc_free(vk, &G.clusters);
    swrt_shutdown(vk);
    gpu_image_destroy(vk, &G.probe_irr);
    gpu_image_destroy(vk, &G.probe_dist);
    gpu_image_destroy(vk, &G.lightmap[0]);
    gpu_image_destroy(vk, &G.lightmap[1]);
    gpu_image_destroy(vk, &G.lightmap[2]);
    gpu_image_destroy(vk, &G.accum);
    gpu_buf_destroy(vk, &G.probe_rays);
    gpu_buf_destroy(vk, &G.probe_state);
    gpu_buf_destroy(vk, &G.probe_check);
    gpu_buf_destroy(vk, &G.occ_light);
    gpu_buf_destroy(vk, &G.volume);
    memset(&G, 0, sizeof G);
}

void gi_fill_frame(GpuFrame* fr, const float camera[3], uint32_t frame_index, const GiSettings* s, bool lightmaps) {
    // The mobility rule (notas-dev/gi.md): the dynamic probes around the camera when they are on; else the baked
    // volume, sampled as it is; during a bake, the bake's grid with every probe each frame
    bool dynamic = (G.rt || G.sw) && s->probes;
    const GiVolume* grid = G.baking ? &G.bake : !dynamic && G.vol_present ? &G.vol : NULL;
    float spacing = grid ? grid->spacing : s->probe_spacing > 10.0f ? s->probe_spacing : 200.0f;
    int dims[3] = { PROBES_X, PROBES_Y, PROBES_Z };
    for (int k = 0; k < 3; k++) {
        fr->probe_min[k] = grid ? grid->min[k] : (int32_t)floorf(camera[k] / spacing) - dims[k] / 2;
        fr->probe_dims[k] = dims[k];
    }
    if (!grid) fr->probe_min[2] = (int32_t)floorf(camera[2] / spacing) - dims[2] / 3; // more room above the camera than below it
    fr->probe_min[3] = G.baking ? PROBE_RAYS_MAX
                       : s->rays_per_probe > 0 && s->rays_per_probe <= PROBE_RAYS_MAX ? s->rays_per_probe : PROBE_RAYS_MAX;
    // phase | stride << 8 (Q16: a bake in the game's budget traces its probes in turns, as the dynamic ones do)
    fr->probe_dims[3] = G.baking ? (int32_t)((G.bake_frames % g_bake_stride) | (g_bake_stride << 8)) : (int32_t)((frame_index & 3u) | (4u << 8));
    fr->probe_params[0] = spacing;
    // a bake converges to the running mean; the dynamic probes keep a short history (no ghosting)
    // (Q16: in turns, a probe's own updates so far)
    float bn = (float)(G.bake_frames / g_bake_stride);
    fr->probe_params[1] = G.baking ? fminf(0.99f, bn / (bn + 1.0f)) : 0.97f;
    fr->probe_params[2] = spacing * 0.1f;
    fr->probe_params[3] = spacing * 0.2f;
    fr->gi_params[0] = s->intensity;
    fr->gi_params[2] = s->reflection_roughness;
    fr->gi_params[3] = 1.0f;
    fr->frame_index = frame_index;
    G.update = (G.rt || G.sw) && (G.baking || dynamic);
    G.upload = !G.update && grid && !G.atlas_volume;
    if (G.baking) G.bake_frames++;
    fr->gi_flags = 0;
    if (G.rt) fr->gi_flags |= GI_RT;
    if (G.update || grid) fr->gi_flags |= GI_PROBES;
    // (A2) the far grid with the dynamic probes (not during a bake, not over a baked volume); REALENGINE_NO_FAR_PROBES=1: off
    static int no_far = -1;
    if (no_far < 0) no_far = getenv("REALENGINE_NO_FAR_PROBES") != NULL;
    if (G.update && dynamic && !G.baking && G.atlas_full && !no_far && g_far_need) fr->gi_flags |= GI_PROBES_FAR;
    static int far_was = -1;
    if (far_was != (int)((fr->gi_flags & GI_PROBES_FAR) != 0))
        far_was = (fr->gi_flags & GI_PROBES_FAR) != 0, printf("[RealEngine] Sondas distantes (A2): %s\n", far_was ? "ligadas" : "desligadas");
    if (G.rt && s->reflections) fr->gi_flags |= GI_REFLECTIONS;
    if (G.rt && s->rt_shadows) fr->gi_flags |= GI_RT_SHADOWS;
    if (G.baking) fr->gi_flags |= GI_BAKING;
    if (G.baking && g_bake_no_emissive) fr->gi_flags |= GI_NO_EMISSIVE;
    if (lightmaps) fr->gi_flags |= GI_LIGHTMAPS;
    fr->probe_rays = G.probe_rays.addr;
    fr->probe_state = G.probe_state.addr;
    fr->occ_light = G.occ_light.addr;
    G.stats.probes_per_frame = !G.update ? 0 : G.baking ? (G.stats.probes + g_bake_stride - 1) / g_bake_stride : (G.stats.probes + 3) / 4;
    G.stats.rays_per_frame = G.stats.probes_per_frame * (uint32_t)fr->probe_min[3];
}

// The baked volume back in the atlases (after the dynamic probes, or loaded with a level)
static void upload_volume(VkCommandBuffer cmd) {
    VkPipelineStageFlags2 SH = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    gpu_barrier(cmd, SH, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    GpuImage* im[2] = { &G.probe_irr, &G.probe_dist };
    for (int i = 0; i < 2; i++) {
        VkBufferImageCopy r = { i ? VOLUME_IRR_BYTES : 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { ATLAS_W, ATLAS_H, 1 } };
        vkCmdCopyBufferToImage(cmd, G.volume.buffer, im[i]->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, SH,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    G.atlas_volume = true;
}

void gi_update_probes(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr) {
    if (G.upload && G.volume.buffer && G.atlas_full) {
        upload_volume(cmd);
        G.upload = false;
    }
    if (!G.update || !fr->tlas) return;
    if (G.atlas_volume && !G.baking) { // the dynamic probes start over where the volume was
        G.atlas_volume = false;
        G.probes_cleared = false;
    }
    if (!G.probes_cleared) { // every probe starts over (its stored cell no longer matches)
        vkCmdFillBuffer(cmd, G.probe_state.buffer, 0, VK_WHOLE_SIZE, 0);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        G.probes_cleared = true;
    }
    uint32_t groups = G.stats.probes_per_frame;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    { VkDescriptorSet sets_[2] = { set, mat_set() }; // (Q4: the Light Functions' materials, set 1)
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 2, sets_, 0, NULL); }
    vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof *push, push);
    // the last frame's update wrote the atlases the rays read
    gpu_barrier(cmd, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // (A2) the far grid: a probe out of 16 a frame, its rays after the near grid's (pc.pad 1: probe_trace.comp)
    uint32_t far_groups = (fr->gi_flags & GI_PROBES_FAR) ? (G.stats.probes + 15) / 16 : 0;
    Push pf = *push;
    pf.pad = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.probe_trace);
    vkCmdDispatch(cmd, groups, 1, 1);
    if (far_groups) {
        vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pf, &pf);
        vkCmdDispatch(cmd, far_groups, 1, 1);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.probe_update);
    if (far_groups) {
        vkCmdDispatch(cmd, far_groups, 1, 1); // (pf still pushed)
        vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof *push, push);
    }
    vkCmdDispatch(cmd, groups, 1, 1);
    if (G.check_on) { // the slots' check (a measure): 4 bytes to host memory
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy c = { (VkDeviceSize)G.stats.probes * 32, 0, 4 };
        vkCmdCopyBuffer(cmd, G.probe_state.buffer, G.probe_check.buffer, 1, &c);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // the main pass samples them
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void gi_view2(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t w, uint32_t h) {
    if (!G.rt || !G.view2) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof *push, push);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.view2);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

void gi_path_trace(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t w, uint32_t h) {
    if (!G.rt) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof *push, push);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pathtrace);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    // the denoise (pass 1) reads the half just written
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL };
    vkCmdPipelineBarrier2(cmd, &dep);
    Push p2 = *push;
    p2.pass = 1;
    vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p2, &p2);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

GpuImage* gi_lightmap_image(int i) { return &G.lightmap[i >= 0 && i < 3 ? i : 0]; }

bool gi_lightmap_resize(VkCtx* vk, uint32_t side) {
    for (int i = 0; i < 3; i++) {
        gpu_image_destroy(vk, &G.lightmap[i]);
        if (!make_image(vk, &G.lightmap[i], side, side, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    clear_general(cmd, &G.lightmap[0]);
    clear_general(cmd, &G.lightmap[1]);
    clear_general(cmd, &G.lightmap[2]);
    gpu_end_once(vk, cmd);
    return true;
}

// (Q16) a bake while playing (gi_bake.c): its lightmap images made on its thread (images only: no queue), swapped in on
// the frame with the GPU idle (the old ones were read by the frames in flight), cleared there
bool gi_lightmap_make(VkCtx* vk, GpuImage out[3], uint32_t side) {
    for (int i = 0; i < 3; i++)
        if (!make_image(vk, &out[i], side, side, VK_FORMAT_R16G16B16A16_SFLOAT)) {
            for (int k = 0; k <= i; k++) gpu_image_destroy(vk, &out[k]);
            return false;
        }
    return true;
}
void gi_lightmap_swap(VkCtx* vk, GpuImage in[3]) {
    vkDeviceWaitIdle(vk->device);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    for (int i = 0; i < 3; i++) {
        gpu_image_destroy(vk, &G.lightmap[i]);
        G.lightmap[i] = in[i];
        memset(&in[i], 0, sizeof in[i]);
        clear_general(cmd, &G.lightmap[i]);
    }
    gpu_end_once(vk, cmd);
}

// --- The baked probe volume ---

bool gi_volume_present(void) { return G.vol_present; }
const GiVolume* gi_volume(void) { return &G.vol; }
size_t gi_volume_bytes(void) { return VOLUME_BYTES; }
const void* gi_volume_data(size_t* bytes) {
    *bytes = G.vol_present && G.volume.mapped ? VOLUME_BYTES : 0;
    return G.volume.mapped;
}

static bool volume_buffer(void) {
    return G.volume.buffer ||
           gpu_buf_create(G.vk, &G.volume, VOLUME_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

// (Lighting Levels) the bakes after the first leave the emissive out: the bakes are summed (sublevels.c)

void gi_bake_set_no_emissive(bool on) { g_bake_no_emissive = on; }
void gi_volume_bake_stride(uint32_t stride) { g_bake_stride = stride < 1 ? 1 : stride > 16 ? 16 : stride; }

void gi_volume_bake_begin(const float lo[3], const float hi[3]) {
    if (!(G.rt || G.sw) || !volume_buffer()) return;
    // the grid fitted to the bounds with a cell to spare (never finer than 50 cm)
    int dims[3] = { PROBES_X, PROBES_Y, PROBES_Z };
    float s = 50.0f;
    for (int k = 0; k < 3; k++) s = fmaxf(s, (hi[k] - lo[k]) / (float)(dims[k] - 3));
    for (int k = 0; k < 3; k++) G.bake.min[k] = (int32_t)floorf((lo[k] + hi[k]) * 0.5f / s) - dims[k] / 2;
    G.bake.spacing = s;
    G.baking = true;
    G.bake_frames = 0;
    G.atlas_volume = false;
    G.probes_cleared = false; // every probe starts over on the bake's grid
    printf("[RealEngine] GI: volume de sondas %dx%dx%d a %.0f cm\n", dims[0], dims[1], dims[2], s);
}

void gi_volume_bake_end(VkCommandBuffer cmd, bool keep) {
    if (!G.baking) return;
    G.baking = false;
    G.probes_cleared = false;
    if (!keep || !cmd) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    GpuImage* im[2] = { &G.probe_irr, &G.probe_dist };
    for (int i = 0; i < 2; i++) {
        VkBufferImageCopy r = { i ? VOLUME_IRR_BYTES : 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { ATLAS_W, ATLAS_H, 1 } };
        vkCmdCopyImageToBuffer(cmd, im[i]->image, VK_IMAGE_LAYOUT_GENERAL, G.volume.buffer, 1, &r);
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    G.vol = G.bake;
    G.vol_present = true;
    G.atlas_volume = true; // the dynamic probes (when on) restart from zero
}

void gi_volume_dims(uint32_t* w, uint32_t* h) { *w = ATLAS_W, *h = ATLAS_H; }

bool gi_volume_load(VkCtx* vk, const GiVolume* v, const void* data, size_t bytes) {
    if (bytes != VOLUME_BYTES || v->spacing < 1.0f || !volume_buffer()) return false;
    if (!G.atlas_full) { // the 1x1 stand-ins (no ray tracing): the volume is sampled from now on
        vkDeviceWaitIdle(vk->device);
        gpu_image_destroy(vk, &G.probe_irr);
        gpu_image_destroy(vk, &G.probe_dist);
        if (!make_image(vk, &G.probe_irr, ATLAS_W, ATLAS_H, VK_FORMAT_R16G16B16A16_SFLOAT) ||
            !make_image(vk, &G.probe_dist, ATLAS_W, ATLAS_H, VK_FORMAT_R16G16_SFLOAT))
            return false;
        VkCommandBuffer cmd = gpu_begin_once(vk);
        clear_general(cmd, &G.probe_irr);
        clear_general(cmd, &G.probe_dist);
        gpu_end_once(vk, cmd);
        G.atlas_full = true;
    }
    vkDeviceWaitIdle(vk->device); // (a frame in flight may still copy the last volume from this buffer: a level load, rare)
    memcpy(G.volume.mapped, data, bytes);
    G.vol = *v;
    G.vol_present = true;
    G.atlas_volume = false; // copied into the atlases by the next frame that samples it
    return true;
}

void gi_volume_clear(void) {
    G.vol_present = false;
    G.baking = false;
    if (G.atlas_volume) {
        G.atlas_volume = false;
        G.probes_cleared = false;
    }
}

void gi_light_occluders(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, GpuFrame* fr) {
    if (!(G.rt || G.sw) || !fr->tlas || !fr->occluder_count) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof *push, push);
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    // last frame's main pass read the buffer this writes
    gpu_barrier(cmd, FS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.occluder_light);
    vkCmdDispatch(cmd, (fr->occluder_count * 6 + 63) / 64, 1, 1);
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}
