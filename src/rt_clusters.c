// The static meshes' BLAS made of clusters (A8): rt_clusters.h, notas-dev/rt-clusters.md
#include "rt_clusters.h"
#ifndef VK_NV_cluster_acceleration_structure // (the NDK's Vulkan headers: no clusters there; the classic BLAS)
RtcPrep* rtc_prepare(struct VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n) {
    (void)vk, (void)v, (void)nv, (void)idx, (void)meshes, (void)n;
    return NULL;
}
bool rtc_commit(struct VkCtx* vk, RtcPrep* p, VkDeviceAddress* out, RtcSet* set) { (void)vk, (void)p, (void)out, (void)set; return false; }
void rtc_drop(struct VkCtx* vk, RtcPrep* p) { (void)vk, (void)p; }
bool rtc_build(struct VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n, VkDeviceAddress* out, RtcSet* set) {
    (void)vk, (void)v, (void)nv, (void)idx, (void)meshes, (void)n, (void)out, (void)set;
    return false;
}
void rtc_free(struct VkCtx* vk, RtcSet* set) { (void)vk, (void)set; }
#else
#include "vkctx.h"
#include "platform.h"
#include "meshoptimizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTC_TRIS 128u  // a cluster's most triangles (NVIDIA's sweet spot; the device allows 256)
#define RTC_VERTS 256u // ... and vertices (8-bit indices)

typedef struct Cl {
    uint32_t geo_first, tris; // its triangles' geometry indices (each its triangle in the mesh + 1: rt.glsl)
    uint32_t pos_first, verts; // its own vertices in the positions' array
    uint32_t idx_first;        // its 8-bit indices
} Cl;

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }

// A triangle by its three vertices, turned so the smallest comes first (its winding kept), and its hash
static void tri_key(const uint32_t* t, uint32_t* k) {
    int f = t[0] <= t[1] && t[0] <= t[2] ? 0 : t[1] <= t[2] ? 1 : 2;
    k[0] = t[f], k[1] = t[(f + 1) % 3], k[2] = t[(f + 2) % 3];
}
static uint32_t tri_hash(const uint32_t* k) { return (k[0] * 0x9E3779B1u) ^ (k[1] * 0x85EBCA77u) ^ (k[2] * 0xC2B2AE3Du); }

void rtc_free(VkCtx* vk, RtcSet* s) {
    gpu_buf_destroy(vk, &s->clas);
    gpu_buf_destroy(vk, &s->blas);
    memset(s, 0, sizeof *s);
}

// One command buffer, run and waited for (the build happens when the meshes load: the GPU is idle then)
static void run(VkCtx* vk, const VkClusterAccelerationStructureCommandsInfoNV* ci, int n, PFN_vkCmdBuildClusterAccelerationStructureIndirectNV fn) {
    VkCommandBuffer cmd = gpu_begin_once(vk);
    for (int i = 0; i < n; i++) {
        if (i) gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                           VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        fn(cmd, &ci[i]);
    }
    gpu_end_once(vk, cmd);
}

// The clusters made on the CPU and their inputs in host memory (any thread: the meshes' worker), for rtc_commit
struct RtcPrep {
    int n;
    uint32_t nc, nb, np, maxt, maxv, max_geo, max_per_mesh, tris_total;
    uint32_t *bmap, *mfirst, *mcount;
    GpuBuf b_pos, b_ix, b_info, b_sizes, b_addrs, b_binfo, b_bsizes, b_baddrs, b_count, b_geo;
    double ms;
};

void rtc_drop(VkCtx* vk, RtcPrep* p) {
    if (!p) return;
    GpuBuf* b[] = { &p->b_pos, &p->b_ix, &p->b_info, &p->b_sizes, &p->b_addrs, &p->b_binfo, &p->b_bsizes, &p->b_baddrs, &p->b_count, &p->b_geo };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) gpu_buf_destroy(vk, b[i]);
    free(p->bmap), free(p->mfirst), free(p->mcount), free(p);
}

RtcPrep* rtc_prepare(VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n) {
    if (!vk->clas || !v || !idx || n <= 0) return NULL;
    double t0 = re_time_seconds();
    uint32_t maxt = vk->clas_max_tris < RTC_TRIS ? vk->clas_max_tris : RTC_TRIS;
    uint32_t maxv = vk->clas_max_verts < RTC_VERTS ? vk->clas_max_verts : RTC_VERTS;

    // 1. The clusters, on the CPU: meshoptimizer's builder for ray tracing (meshopt_buildMeshletsSpatial: the cut by the
    // surface area heuristic, as a BVH would), 64 to 128 triangles and up to 256 vertices each; each cluster's vertices
    // its own (the positions copied), its indices 8 bits into them, each triangle's geometry index its place in the
    // mesh + 1 (found back by its three vertices: the builder reorders them)
    uint32_t* mfirst = calloc((size_t)n, sizeof(uint32_t));
    uint32_t* mcount = calloc((size_t)n, sizeof(uint32_t));
    uint32_t ccap = 1024, pcap = 65536, icap = 65536, gcap = 65536, nc = 0, np = 0, ni = 0, ng = 0;
    Cl* cl = malloc(sizeof(Cl) * ccap);
    float* pos = malloc(sizeof(float) * 3 * pcap);
    uint8_t* ix = malloc(icap);
    uint32_t* geo = malloc(sizeof(uint32_t) * gcap);
    bool ok = mfirst && mcount && cl && pos && ix && geo;
    uint32_t max_geo = 0, max_per_mesh = 0, tris_total = 0;
    const uint32_t mint = maxt / 2;
    for (int m = 0; ok && m < n; m++) {
        mfirst[m] = nc;
        uint32_t ntri = meshes[m].index_count / 3, vo = (uint32_t)meshes[m].vertex_offset;
        const uint32_t* mi = idx + meshes[m].first_index;
        if (!ntri) continue;
        if (ntri > vk->clas_max_geometry - 1u || vo >= nv) { ok = false; break; } // (the geometry indices wouldn't fit)
        for (uint32_t q = 0; q < ntri * 3; q++)
            if (mi[q] >= nv - vo) { ok = false; break; }
        if (!ok) break;
        size_t mcap = meshopt_buildMeshletsBound(ntri * 3, maxv, mint);
        struct meshopt_Meshlet* ml = malloc(sizeof *ml * mcap);
        unsigned int* mv = malloc(sizeof(unsigned int) * ntri * 3);
        unsigned char* mt = malloc((size_t)ntri * 3);
        // (the triangles by their vertices, the smallest first in their turn: a hash, chains for the repeated ones)
        uint32_t hcap = 1;
        while (hcap < ntri * 2u) hcap <<= 1;
        uint32_t* head = malloc(sizeof(uint32_t) * hcap);
        uint32_t* next = malloc(sizeof(uint32_t) * ntri);
        if (!ml || !mv || !mt || !head || !next) ok = false;
        size_t nm = 0;
        if (ok) {
            memset(head, 0xFF, sizeof(uint32_t) * hcap);
            for (uint32_t t = 0; t < ntri; t++) {
                uint32_t k[3];
                tri_key(&mi[t * 3], k);
                uint32_t hsh = tri_hash(k) & (hcap - 1u);
                next[t] = head[hsh], head[hsh] = t;
            }
            nm = meshopt_buildMeshletsSpatial(ml, mv, mt, mi, (size_t)ntri * 3, v[vo].pos, nv - vo, sizeof(GpuVertex), maxv, mint, maxt, 0.5f);
        }
        for (size_t c = 0; ok && c < nm; c++) {
            const struct meshopt_Meshlet* me = &ml[c];
            if (nc + 1 > ccap) {
                Cl* c2 = realloc(cl, sizeof(Cl) * (ccap *= 2));
                if (!c2) { ok = false; break; }
                cl = c2;
            }
            while (np + me->vertex_count > pcap) {
                float* p2 = realloc(pos, sizeof(float) * 3 * (pcap *= 2));
                if (!p2) { ok = false; break; }
                pos = p2;
            }
            while (ni + me->triangle_count * 3 > icap) {
                uint8_t* i2 = realloc(ix, (icap *= 2));
                if (!i2) { ok = false; break; }
                ix = i2;
            }
            while (ng + me->triangle_count > gcap) {
                uint32_t* g2 = realloc(geo, sizeof(uint32_t) * (gcap *= 2));
                if (!g2) { ok = false; break; }
                geo = g2;
            }
            if (!ok) break;
            cl[nc++] = (Cl){ ng, me->triangle_count, np, me->vertex_count, ni };
            for (uint32_t k = 0; k < me->vertex_count; k++) memcpy(&pos[(size_t)(np + k) * 3], v[vo + mv[me->vertex_offset + k]].pos, sizeof(float) * 3);
            np += me->vertex_count;
            for (uint32_t t = 0; t < me->triangle_count; t++) {
                uint32_t tv[3], k[3];
                for (int j = 0; j < 3; j++) {
                    uint8_t l = mt[me->triangle_offset + t * 3 + j];
                    ix[ni++] = l;
                    tv[j] = mv[me->vertex_offset + l];
                }
                tri_key(tv, k);
                uint32_t* link = &head[tri_hash(k) & (hcap - 1u)], orig = UINT32_MAX;
                for (; *link != UINT32_MAX; link = &next[*link]) {
                    uint32_t o[3];
                    tri_key(&mi[*link * 3], o);
                    if (o[0] == k[0] && o[1] == k[1] && o[2] == k[2]) { orig = *link, *link = next[*link]; break; } // (taken: a repeat finds the next)
                }
                if (orig == UINT32_MAX) { ok = false; break; }
                geo[ng++] = (orig + 1u) | ((uint32_t)VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV << 29); // (geometryIndex:24, flags:3)
                if (orig + 1u > max_geo) max_geo = orig + 1u;
            }
        }
        free(ml), free(mv), free(mt), free(head), free(next);
        mcount[m] = nc - mfirst[m];
        if (mcount[m] > max_per_mesh) max_per_mesh = mcount[m];
        tris_total += ntri;
    }
    // (the BLAS: the meshes with clusters; an empty one has none)
    uint32_t nb = 0;
    uint32_t* bmap = malloc(sizeof(uint32_t) * (size_t)n);
    for (int m = 0; ok && bmap && m < n; m++)
        if (mcount[m]) bmap[nb++] = (uint32_t)m;
    if (!bmap) ok = false;
    if (!ok || !nc) {
        free(bmap);
        free(mfirst), free(mcount), free(cl), free(pos), free(ix), free(geo);
        return NULL;
    }
    // 2. The inputs on the GPU (host memory: read once by the builds)
    VkBufferUsageFlags in_use = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    GpuBuf b_pos = { 0 }, b_ix = { 0 }, b_info = { 0 }, b_sizes = { 0 }, b_addrs = { 0 }, b_binfo = { 0 }, b_bsizes = { 0 }, b_baddrs = { 0 },
           b_count = { 0 }, b_geo = { 0 };
    ok = gpu_buf_create(vk, &b_pos, sizeof(float) * 3 * np, in_use, true) && gpu_buf_create(vk, &b_ix, ni + 16, in_use, true) &&
         gpu_buf_create(vk, &b_geo, sizeof(uint32_t) * ng, in_use, true) &&
         gpu_buf_create(vk, &b_info, sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV) * nc, in_use, true) &&
         gpu_buf_create(vk, &b_sizes, sizeof(uint32_t) * nc, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) &&
         gpu_buf_create(vk, &b_addrs, sizeof(VkDeviceAddress) * nc, in_use, true) &&
         gpu_buf_create(vk, &b_binfo, sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV) * nb, in_use, true) &&
         gpu_buf_create(vk, &b_bsizes, sizeof(uint32_t) * nb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) &&
         gpu_buf_create(vk, &b_baddrs, sizeof(VkDeviceAddress) * nb, in_use, true) &&
         gpu_buf_create(vk, &b_count, 16, in_use, true);
    if (ok) {
        memcpy(b_pos.mapped, pos, sizeof(float) * 3 * np);
        memcpy(b_ix.mapped, ix, ni);
        memcpy(b_geo.mapped, geo, sizeof(uint32_t) * ng);
        VkClusterAccelerationStructureBuildTriangleClusterInfoNV* ti = b_info.mapped;
        memset(ti, 0, sizeof *ti * nc);
        for (uint32_t i = 0; i < nc; i++) {
            ti[i].clusterID = i;
            ti[i].triangleCount = cl[i].tris;
            ti[i].vertexCount = cl[i].verts;
            ti[i].positionTruncateBitCount = 0; // (lossless: the hits where the raster draws them)
            ti[i].indexType = VK_CLUSTER_ACCELERATION_STRUCTURE_INDEX_FORMAT_8BIT_NV;
            ti[i].baseGeometryIndexAndGeometryFlags.geometryFlags = VK_CLUSTER_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT_NV;
            ti[i].geometryIndexAndFlagsBufferStride = sizeof(uint32_t);
            ti[i].geometryIndexAndFlagsBuffer = b_geo.addr + sizeof(uint32_t) * cl[i].geo_first;
            ti[i].indexBufferStride = 1;
            ti[i].vertexBufferStride = sizeof(float) * 3;
            ti[i].indexBuffer = b_ix.addr + cl[i].idx_first;
            ti[i].vertexBuffer = b_pos.addr + (VkDeviceAddress)cl[i].pos_first * sizeof(float) * 3;
        }
        ((uint32_t*)b_count.mapped)[0] = nc, ((uint32_t*)b_count.mapped)[1] = nb;
    }
    free(cl), free(pos), free(ix), free(geo);
    RtcPrep* pr = ok ? calloc(1, sizeof *pr) : NULL;
    if (!pr) {
        GpuBuf* bb[] = { &b_pos, &b_ix, &b_info, &b_sizes, &b_addrs, &b_binfo, &b_bsizes, &b_baddrs, &b_count, &b_geo };
        for (size_t i = 0; i < sizeof bb / sizeof bb[0]; i++) gpu_buf_destroy(vk, bb[i]);
        free(bmap), free(mfirst), free(mcount);
        return NULL;
    }
    *pr = (RtcPrep){ n, nc, nb, np, maxt, maxv, max_geo, max_per_mesh, tris_total, bmap, mfirst, mcount, b_pos, b_ix, b_info, b_sizes, b_addrs, b_binfo, b_bsizes, b_baddrs, b_count, b_geo, 0.0 };
    pr->ms = (re_time_seconds() - t0) * 1000.0;
    return pr;
}

// The builds (the main thread, the GPU idle): the clusters' sizes, their places, the BLAS'
bool rtc_commit(VkCtx* vk, RtcPrep* pr, VkDeviceAddress* out, RtcSet* set) {
    memset(set, 0, sizeof *set);
    if (!pr) return false;
    PFN_vkGetClusterAccelerationStructureBuildSizesNV sizes_fn =
        (PFN_vkGetClusterAccelerationStructureBuildSizesNV)vkGetDeviceProcAddr(vk->device, "vkGetClusterAccelerationStructureBuildSizesNV");
    PFN_vkCmdBuildClusterAccelerationStructureIndirectNV build_fn =
        (PFN_vkCmdBuildClusterAccelerationStructureIndirectNV)vkGetDeviceProcAddr(vk->device, "vkCmdBuildClusterAccelerationStructureIndirectNV");
    double t0 = re_time_seconds();
    int n = pr->n;
    uint32_t nc = pr->nc, nb = pr->nb, np = pr->np, maxt = pr->maxt, maxv = pr->maxv, max_geo = pr->max_geo, max_per_mesh = pr->max_per_mesh,
             tris_total = pr->tris_total;
    uint32_t *bmap = pr->bmap, *mfirst = pr->mfirst, *mcount = pr->mcount;
    GpuBuf b_pos = pr->b_pos, b_ix = pr->b_ix, b_info = pr->b_info, b_sizes = pr->b_sizes, b_addrs = pr->b_addrs, b_binfo = pr->b_binfo, b_bsizes = pr->b_bsizes, b_baddrs = pr->b_baddrs, b_count = pr->b_count, b_geo = pr->b_geo, b_scratch = { 0 };
    double prep_ms = pr->ms;
    free(pr);
    bool ok = sizes_fn && build_fn;
    // 3. The clusters' sizes (the driver's exact ones), their places, then built there
    VkClusterAccelerationStructureTriangleClusterInputNV tin = { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV };
    tin.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tin.maxGeometryIndexValue = max_geo;
    tin.maxClusterUniqueGeometryCount = maxt; // (each triangle its own)
    tin.maxClusterTriangleCount = maxt;
    tin.maxClusterVertexCount = maxv;
    tin.maxTotalTriangleCount = tris_total;
    tin.maxTotalVertexCount = np;
    VkClusterAccelerationStructureInputInfoNV cin = { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV };
    cin.maxAccelerationStructureCount = nc;
    cin.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    cin.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
    cin.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_COMPUTE_SIZES_NV;
    cin.opInput.pTriangleClusters = &tin;
    VkAccelerationStructureBuildSizesInfoKHR sz_sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    VkAccelerationStructureBuildSizesInfoKHR sz_build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    sizes_fn(vk->device, &cin, &sz_sizes);
    cin.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
    sizes_fn(vk->device, &cin, &sz_build);
    // (the bottom level's: its scratch, the largest of the two)
    VkClusterAccelerationStructureClustersBottomLevelInputNV bin = { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV };
    bin.maxTotalClusterCount = nc;
    bin.maxClusterCountPerAccelerationStructure = max_per_mesh;
    VkClusterAccelerationStructureInputInfoNV bci = { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV };
    bci.maxAccelerationStructureCount = nb;
    bci.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bci.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
    bci.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_COMPUTE_SIZES_NV;
    bci.opInput.pClustersBottomLevel = &bin;
    VkAccelerationStructureBuildSizesInfoKHR bz_sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    VkAccelerationStructureBuildSizesInfoKHR bz_build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    sizes_fn(vk->device, &bci, &bz_sizes);
    bci.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
    sizes_fn(vk->device, &bci, &bz_build);
    VkDeviceSize sc = sz_sizes.buildScratchSize;
    if (sz_build.buildScratchSize > sc) sc = sz_build.buildScratchSize;
    if (bz_sizes.buildScratchSize > sc) sc = bz_sizes.buildScratchSize;
    if (bz_build.buildScratchSize > sc) sc = bz_build.buildScratchSize;
    VkDeviceSize sa = vk->clas_scratch_align ? vk->clas_scratch_align : 256;
    ok = ok && gpu_buf_create(vk, &b_scratch, sc + sa, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    VkDeviceAddress scratch = align_up(b_scratch.addr, sa);
    if (ok) {
        VkClusterAccelerationStructureCommandsInfoNV ci = { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV };
        ci.input = cin;
        ci.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_COMPUTE_SIZES_NV;
        ci.scratchData = scratch;
        ci.dstSizesArray = (VkStridedDeviceAddressRegionKHR){ b_sizes.addr, sizeof(uint32_t), sizeof(uint32_t) * nc };
        ci.srcInfosArray = (VkStridedDeviceAddressRegionKHR){ b_info.addr, sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV),
                                                               sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV) * nc };
        ci.srcInfosCount = b_count.addr;
        run(vk, &ci, 1, build_fn);
        const uint32_t* cs = b_sizes.mapped;
        VkDeviceSize ca = vk->clas_align ? vk->clas_align : 128, total = 0;
        for (uint32_t i = 0; i < nc; i++) total = align_up(total, ca) + cs[i];
        ok = total > 0 && gpu_buf_create(vk, &set->clas, total + ca, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        if (ok) {
            VkDeviceAddress base = align_up(set->clas.addr, ca), off = 0;
            VkDeviceAddress* da = b_addrs.mapped;
            for (uint32_t i = 0; i < nc; i++) off = align_up(off, ca), da[i] = base + off, off += cs[i];
            set->clas_mb = (double)total / 1048576.0;
            // the clusters built at their places, then the BLAS' sizes (from the built clusters' references)
            VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV* bi = b_binfo.mapped;
            for (uint32_t k = 0; k < nb; k++)
                bi[k] = (VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV){ mcount[bmap[k]], sizeof(VkDeviceAddress),
                                                                                         b_addrs.addr + sizeof(VkDeviceAddress) * mfirst[bmap[k]] };
            VkClusterAccelerationStructureCommandsInfoNV c2[2] = { ci, { VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV } };
            c2[0].input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
            c2[0].dstAddressesArray = (VkStridedDeviceAddressRegionKHR){ b_addrs.addr, sizeof(VkDeviceAddress), sizeof(VkDeviceAddress) * nc };
            c2[0].dstSizesArray = (VkStridedDeviceAddressRegionKHR){ 0, 0, 0 };
            c2[1].input = bci;
            c2[1].input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_COMPUTE_SIZES_NV;
            c2[1].scratchData = scratch;
            c2[1].dstSizesArray = (VkStridedDeviceAddressRegionKHR){ b_bsizes.addr, sizeof(uint32_t), sizeof(uint32_t) * nb };
            c2[1].srcInfosArray = (VkStridedDeviceAddressRegionKHR){ b_binfo.addr, sizeof *bi, sizeof *bi * nb };
            c2[1].srcInfosCount = b_count.addr + 4;
            run(vk, c2, 2, build_fn);
            // 4. The BLAS at their places
            const uint32_t* bs = b_bsizes.mapped;
            VkDeviceSize ba = vk->clas_blas_align ? vk->clas_blas_align : 256, btotal = 0;
            for (uint32_t k = 0; k < nb; k++) btotal = align_up(btotal, ba) + bs[k];
            ok = btotal > 0 && gpu_buf_create(vk, &set->blas, btotal + ba, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
            if (ok) {
                VkDeviceAddress bbase = align_up(set->blas.addr, ba), bo = 0;
                VkDeviceAddress* bd = b_baddrs.mapped;
                for (uint32_t k = 0; k < nb; k++) bo = align_up(bo, ba), bd[k] = bbase + bo, bo += bs[k];
                set->blas_mb = (double)btotal / 1048576.0;
                VkClusterAccelerationStructureCommandsInfoNV c3 = c2[1];
                c3.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
                c3.dstAddressesArray = (VkStridedDeviceAddressRegionKHR){ b_baddrs.addr, sizeof(VkDeviceAddress), sizeof(VkDeviceAddress) * nb };
                c3.dstSizesArray = (VkStridedDeviceAddressRegionKHR){ 0, 0, 0 };
                run(vk, &c3, 1, build_fn);
                for (int m = 0; m < n; m++) out[m] = 0;
                for (uint32_t k = 0; k < nb; k++) out[bmap[k]] = bd[k];
            }
        }
    }
    GpuBuf* tmp[] = { &b_geo, &b_pos, &b_ix, &b_info, &b_sizes, &b_addrs, &b_binfo, &b_bsizes, &b_baddrs, &b_count, &b_scratch };
    for (size_t i = 0; i < sizeof tmp / sizeof tmp[0]; i++) gpu_buf_destroy(vk, tmp[i]);
    free(mfirst), free(mcount), free(bmap);
    if (!ok) {
        rtc_free(vk, set);
        return false;
    }
    set->meshes = (uint32_t)n, set->clusters = nc, set->triangles = tris_total;
    set->build_ms = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] BLAS de clusters (A8): %d malhas, %u clusters, %u triangulos: CLAS %.1f MB + BLAS %.1f MB; clusters na CPU %.0f ms, "
           "builds %.0f ms\n", n, nc, tris_total, set->clas_mb, set->blas_mb, prep_ms, set->build_ms);
    return true;
}

bool rtc_build(VkCtx* vk, const GpuVertex* v, uint32_t nv, const uint32_t* idx, const GiMesh* meshes, int n, VkDeviceAddress* out,
               RtcSet* set) {
    return rtc_commit(vk, rtc_prepare(vk, v, nv, idx, meshes, n), out, set);
}
#endif
