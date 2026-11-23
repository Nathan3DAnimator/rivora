// (Q21) The distant view: farview.h, notas-dev/visao-distante.md
#include "farview.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gi.h"
#include "landscape.h"
#include "material.h"
#include "platform.h"
#include "render.h"
#include "vkctx.h"
#include "vsm.h"

static const uint32_t s_far_comp[] =
#include "farshadow.comp.inc"
    ;
static const uint32_t s_bake_vert[] =
#include "impostor_bake.vert.inc"
    ;
static const uint32_t s_bake_frag[] =
#include "impostor_bake.frag.inc"
    ;

#define FARV_FOCAL_PX 1280.0f      // (a prop's switch distance: 2560 px wide at 90 degrees)
#define FARV_REF_PX 24.0f          // (... at the default switch: the measures' overrides don't change who gets one)
#define FARV_MIN_SWITCH_CM 6000.0f // (props whose card would take over nearer than 60 m keep their LODs)

enum { IMP_NONE = 0, IMP_CANDIDATE, IMP_BAKED, IMP_FAILED };
typedef struct ImpRec {
    uint32_t key;    // the material's slot + 1 (0xFFFFFFFF: the instance's color); the first one seen
    uint32_t count;  // instances seen with that key
    uint32_t tex_a, tex_n;
    float rad;       // the largest instance's radius (cm; its box's half diagonal): how far its card would take over
    uint8_t state;
    bool plant, blocked;
} ImpRec;

typedef struct FarParams { float grid[4], sun[4]; } FarParams; // (farshadow.comp FarParamBuf)
typedef struct BakeParams { float sphere[4]; uint32_t info[4]; VkDeviceAddress materials; uint64_t pad; } BakeParams; // (impostor_bake ImpBakeBuf)

static struct {
    VkCtx* vk;
    VkPipelineLayout layout;
    VkPipeline comp, bake;
    // the far shadow
    GpuBuf far, params;  // device local: the heights then the horizon; the pass's parameters (vkCmdUpdateBuffer)
    float* heights;      // the CPU's copy (farview_shadow_at)
    uint32_t res, res_want;
    float origin[2], texel;
    uint32_t land_gen;
    bool hf_valid, want_shadow, shadow_dirty, force;
    float sun[3], sun_done[3];
    float split, split_done; // (cm: the horizon's near / far words; a quarter of the sun's virtual maps' side)
    bool sun_on;
    // the impostors
    ImpRec rec[RE_MAX_MESHES];
    float smn[3], smx[3]; // the still instances' bounds (cm): a scene smaller than a card's distance never shows it
    bool s_any;
    GpuBuf tab;           // device local: a uvec4 per mesh slot (farview.glsl ImpTabBuf)
    uint32_t tab_cpu[RE_MAX_MESHES][4];
    bool tab_dirty;
    GpuImage ta, tn, td;  // the bake's targets
    uint32_t target_res;
    GpuBuf bake_buf;
    uint32_t atlas;
    float switch_px, fog_sky, strength;
    bool imp_on;
    float switch_force;
    double next_scan;
    FarviewStats st;
} F;

static VkPipeline bake_pipeline(VkCtx* vk) {
    VkShaderModule vs = gpu_shader(vk, s_bake_vert, sizeof s_bake_vert), fs = gpu_shader(vk, s_bake_frag, sizeof s_bake_frag);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs, st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.frontFace = VK_FRONT_FACE_CLOCKWISE, rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE, ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // (reversed)
        VkPipelineColorBlendAttachmentState ba[2] = { { 0 }, { 0 } };
        ba[0].colorWriteMask = ba[1].colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 2, cb.pAttachments = ba;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
        VkFormat cf[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        ri.colorAttachmentCount = 2, ri.pColorAttachmentFormats = cf, ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
        ci.stageCount = 2, ci.pStages = st;
        ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dy;
        ci.layout = F.layout;
        if (vkCreateGraphicsPipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(vk->device, vs, NULL);
    if (fs) vkDestroyShaderModule(vk->device, fs, NULL);
    return p;
}

bool farview_init(VkCtx* vk, VkPipelineLayout raster_layout) {
    memset(&F, 0, sizeof F);
    F.vk = vk;
    F.layout = raster_layout;
    F.comp = gpu_compute_pipeline(vk, raster_layout, s_far_comp, sizeof s_far_comp);
    F.bake = bake_pipeline(vk);
    bool ok = F.comp && F.bake &&
              gpu_buf_create(vk, &F.params, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
              gpu_buf_create(vk, &F.tab, sizeof F.tab_cpu, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
              gpu_buf_create(vk, &F.bake_buf, sizeof(BakeParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!ok) printf("[RealEngine] aviso: Distant view: its pipelines or buffers were not made (no far shadow, no impostors)\n");
    F.tab_dirty = true;
    return ok;
}

static void imp_drop_all(VkCtx* vk) {
    bool any = false;
    for (uint32_t s = 0; s < RE_MAX_MESHES; s++) any |= F.rec[s].tex_a || F.rec[s].tex_n;
    if (any) vkDeviceWaitIdle(vk->device);
    for (uint32_t s = 0; s < RE_MAX_MESHES; s++) {
        if (F.rec[s].tex_a) mat_tex_release(F.rec[s].tex_a);
        if (F.rec[s].tex_n) mat_tex_release(F.rec[s].tex_n);
    }
    memset(F.rec, 0, sizeof F.rec);
    memset(F.tab_cpu, 0, sizeof F.tab_cpu);
    F.tab_dirty = true;
    F.st.impostors = F.st.candidates = F.st.failed = 0;
    F.st.bytes = 0;
}

void farview_shutdown(VkCtx* vk) {
    if (!F.vk) return;
    vkDeviceWaitIdle(vk->device);
    imp_drop_all(vk);
    gpu_buf_destroy(vk, &F.far), gpu_buf_destroy(vk, &F.params), gpu_buf_destroy(vk, &F.tab), gpu_buf_destroy(vk, &F.bake_buf);
    gpu_image_destroy(vk, &F.ta), gpu_image_destroy(vk, &F.tn), gpu_image_destroy(vk, &F.td);
    if (F.comp) vkDestroyPipeline(vk->device, F.comp, NULL);
    if (F.bake) vkDestroyPipeline(vk->device, F.bake, NULL);
    free(F.heights);
    memset(&F, 0, sizeof F);
}

void farview_meshes_changed(VkCtx* vk) {
    if (F.vk) imp_drop_all(vk);
}

void farview_note_instance(uint32_t mesh, uint32_t key, bool plant, bool allowed, const float bmn[3], const float bmx[3]) {
    if (mesh >= RE_MAX_MESHES || !key) return;
    ImpRec* r = &F.rec[mesh];
    float h[3] = { (bmx[0] - bmn[0]) * 0.5f, (bmx[1] - bmn[1]) * 0.5f, (bmx[2] - bmn[2]) * 0.5f };
    float rad = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    if (rad > r->rad) r->rad = rad;
    for (int k = 0; k < 3; k++) {
        float c = (bmn[k] + bmx[k]) * 0.5f;
        if (!F.s_any || c < F.smn[k]) F.smn[k] = c;
        if (!F.s_any || c > F.smx[k]) F.smx[k] = c;
    }
    F.s_any = true;
    if (!allowed && !r->blocked) {
        r->blocked = true;
        if (F.tab_cpu[mesh][0]) memset(F.tab_cpu[mesh], 0, sizeof F.tab_cpu[mesh]), F.tab_dirty = true; // (its card no more)
    }
    if (!r->key) r->key = key;
    if (r->key == key) r->count++;
    r->plant |= plant;
}

// --- The far shadow's heightfield ---

static void hf_row(void* arg, int y) {
    (void)arg;
    float* row = F.heights + (size_t)y * F.res;
    float py = F.origin[1] + ((float)y + 0.5f) * F.texel;
    for (uint32_t x = 0; x < F.res; x++) {
        float z;
        row[x] = landscape_height(F.origin[0] + ((float)x + 0.5f) * F.texel, py, &z) ? z : -1e6f;
    }
}

static bool hf_build(VkCtx* vk) {
    F.hf_valid = false;
    float mn[2], mx[2];
    if (!landscape_bounds(mn, mx)) return false;
    double t0 = re_time_seconds();
    float side = fmaxf(mx[0] - mn[0], mx[1] - mn[1]);
    if (side < 1000.0f) return false;
    uint32_t res = F.res_want;
    if (res != F.res || !F.heights) {
        free(F.heights);
        F.heights = malloc(sizeof(float) * res * res);
        if (!F.heights) return false;
        F.res = res;
        vkDeviceWaitIdle(vk->device); // (the old buffer: a frame in flight may read it)
        gpu_buf_destroy(vk, &F.far);
        if (!gpu_buf_create(vk, &F.far, (VkDeviceSize)res * res * 12, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
            return false;
    }
    F.texel = side / (float)res;
    F.origin[0] = (mn[0] + mx[0]) * 0.5f - side * 0.5f, F.origin[1] = (mn[1] + mx[1]) * 0.5f - side * 0.5f;
    re_parallel_for((int)res, hf_row, NULL);
    // (no ground: the lowest one, nothing rises from there)
    float lo = 1e30f;
    for (size_t i = 0; i < (size_t)res * res; i++)
        if (F.heights[i] > -1e5f && F.heights[i] < lo) lo = F.heights[i];
    if (lo > 1e29f) lo = 0.0f;
    for (size_t i = 0; i < (size_t)res * res; i++)
        if (F.heights[i] <= -1e5f) F.heights[i] = lo;
    GpuBuf stg;
    if (!gpu_buf_create(vk, &stg, sizeof(float) * res * res, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(stg.mapped, F.heights, sizeof(float) * res * res);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy c = { 0, 0, sizeof(float) * res * res };
    vkCmdCopyBuffer(cmd, stg.buffer, F.far.buffer, 1, &c);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &stg);
    F.hf_valid = true;
    F.shadow_dirty = true;
    F.st.hf_builds++;
    F.st.hf_ms = (re_time_seconds() - t0) * 1000.0;
    F.st.res = res, F.st.texel_cm = F.texel, F.st.extent_km = side * 1e-5f;
    printf("[RealEngine] Distant view: the terrain's heightfield %u x %u (%.1f km, %.1f m a texel), %.1f ms\n", res, res, side * 1e-5f,
           F.texel * 0.01f, F.st.hf_ms);
    return true;
}

// --- Impostors ---

static bool targets(VkCtx* vk, uint32_t res) {
    if (F.target_res == res && F.ta.image) return true;
    gpu_image_destroy(vk, &F.ta), gpu_image_destroy(vk, &F.tn), gpu_image_destroy(vk, &F.td);
    F.target_res = 0;
    VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!gpu_image_create(vk, &F.ta, res, res, VK_FORMAT_R8G8B8A8_UNORM, cu, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !gpu_image_create(vk, &F.tn, res, res, VK_FORMAT_R8G8B8A8_UNORM, cu, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !gpu_image_create(vk, &F.td, res, res, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    F.target_res = res;
    return true;
}

// One impostor: the mesh's LOD 0 drawn from the N x N directions into the targets, copied into two textures of the
// materials' bindless set (mips by halving). Its colors premultiplied by the alpha (cleared to 0): the mips and the
// bilinear reads average only what is there, divided back by the alpha (no dark fringe around the leaves)
static bool bake_one(VkCtx* vk, uint32_t s) {
    ImpRec* r = &F.rec[s];
    uint32_t fi = 0, ic = 0;
    int32_t vo = 0;
    if (!render_mesh_lod_range(s, 0, &fi, &ic, &vo) || ic < 3) return false;
    float mn[3], mx[3];
    render_mesh_bounds(s, mn, mx);
    float c[3], he[3];
    for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f, he[k] = (mx[k] - mn[k]) * 0.5f;
    float rad = sqrtf(he[0] * he[0] + he[1] * he[1] + he[2] * he[2]);
    if (rad < 1.0f) return false;
    uint32_t res = F.atlas;
    if (!targets(vk, res)) return false;
    double t0 = re_time_seconds();
    uint32_t ta = mat_tex_new(res, res), tn = mat_tex_new(res, res);
    if (!ta || !tn) {
        if (ta) mat_tex_release(ta);
        if (tn) mat_tex_release(tn);
        return false;
    }
    BakeParams* bp = F.bake_buf.mapped;
    memset(bp, 0, sizeof *bp);
    memcpy(bp->sphere, c, sizeof c), bp->sphere[3] = rad;
    float one = 1.0f;
    bp->info[0] = FARV_FRAMES, bp->info[1] = r->key == 0xFFFFFFFFu ? 0xFFFFFFFFu : r->key - 1u;
    memcpy(&bp->info[2], &one, 4);
    bp->materials = mat_buffer_address();
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, &F.ta, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    gpu_transition(cmd, &F.tn, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    gpu_transition(cmd, &F.td, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true);
    VkRenderingAttachmentInfo ca[2] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
    ca[0].imageView = F.ta.view, ca[1].imageView = F.tn.view;
    for (int k = 0; k < 2; k++) {
        ca[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = F.td.view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 0.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ res, res };
    ri.layerCount = 1, ri.colorAttachmentCount = 2, ri.pColorAttachments = ca, ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, F.bake);
    VkDescriptorSet ms = mat_set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, F.layout, 1, 1, &ms, 0, NULL);
    vkCmdBindIndexBuffer(cmd, render_index_buffer(), 0, VK_INDEX_TYPE_UINT32);
    uint32_t fp = res / FARV_FRAMES;
    for (uint32_t f = 0; f < FARV_FRAMES * FARV_FRAMES; f++) {
        VkViewport vp = { (float)((f % FARV_FRAMES) * fp), (float)((f / FARV_FRAMES) * fp), (float)fp, (float)fp, 0.0f, 1.0f };
        VkRect2D sc = { { (int32_t)((f % FARV_FRAMES) * fp), (int32_t)((f / FARV_FRAMES) * fp) }, { fp, fp } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        Push p = { F.bake_buf.addr, render_vertex_address(), 0, f, 0 };
        vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDrawIndexed(cmd, ic, 1, fi, vo, 0);
    }
    vkCmdEndRendering(cmd);
    gpu_transition(cmd, &F.ta, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    gpu_transition(cmd, &F.tn, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    VkImageCopy cp = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { res, res, 1 } };
    vkCmdCopyImage(cmd, F.ta.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mat_tex_image(ta), VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    vkCmdCopyImage(cmd, F.tn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mat_tex_image(tn), VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    mat_tex_mips_in(cmd, ta);
    mat_tex_mips_in(cmd, tn);
    gpu_end_once(vk, cmd);
    r->tex_a = ta, r->tex_n = tn;
    F.tab_cpu[s][0] = r->key;
    F.tab_cpu[s][1] = ta | (tn << 16);
    F.tab_cpu[s][2] = FARV_FRAMES;
    // the mesh's box's half sides over the sphere's radius (10 bits each): mesh.vert fits the card to the box seen
    uint32_t q3[3];
    for (int k = 0; k < 3; k++) {
        float v = fminf(fmaxf(he[k] / rad, 0.0f), 1.0f);
        q3[k] = (uint32_t)ceilf(v * 1023.0f);
    }
    F.tab_cpu[s][3] = q3[0] | (q3[1] << 10) | (q3[2] << 20);
    F.tab_dirty = true;
    F.st.bake_ms = (re_time_seconds() - t0) * 1000.0;
    F.st.bake_ms_total += F.st.bake_ms;
    F.st.bytes += (uint64_t)res * res * 4u * 2u * 4u / 3u;
    printf("[RealEngine] Distant view: impostor of %s (%u instances, %u triangles, %.1f m) %u x %u, %.1f ms\n", render_mesh_name(s), r->count,
           ic / 3u, rad * 0.01f, res, res, F.st.bake_ms);
    return true;
}

static void tab_upload(VkCtx* vk) {
    if (!F.tab_dirty || !F.tab.buffer) return;
    GpuBuf stg;
    if (!gpu_buf_create(vk, &stg, sizeof F.tab_cpu, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return;
    memcpy(stg.mapped, F.tab_cpu, sizeof F.tab_cpu);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy c = { 0, 0, sizeof F.tab_cpu };
    vkCmdCopyBuffer(cmd, stg.buffer, F.tab.buffer, 1, &c);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &stg);
    F.tab_dirty = false;
}

void farview_prepare(VkCtx* vk, const GiSettings* gi, const float sun[3]) {
    if (!F.vk || !F.comp) return;
    // the settings (env overrides for the measures: REALENGINE_FAR_SHADOW=0, REALENGINE_IMPOSTORS=0)
    const char* e1 = getenv("REALENGINE_FAR_SHADOW");
    const char* e2 = getenv("REALENGINE_IMPOSTORS");
    F.want_shadow = gi->far_shadow && !(e1 && e1[0] == '0');
    F.res_want = gi->far_res >= 256 && gi->far_res <= 4096 ? (uint32_t)gi->far_res : 1024u;
    F.strength = 1.0f;
    F.fog_sky = gi->fog_sky;
    bool imp = gi->impostors && F.bake && !(e2 && e2[0] == '0');
    uint32_t atlas = gi->impostor_res >= 256 && gi->impostor_res <= 2048 ? (uint32_t)gi->impostor_res : 1024u;
    if (atlas != F.atlas && F.atlas) {
        // (a new size: baked again)
        for (uint32_t s = 0; s < RE_MAX_MESHES; s++) {
            ImpRec* r = &F.rec[s];
            if (r->state != IMP_BAKED) continue;
            vkDeviceWaitIdle(vk->device);
            mat_tex_release(r->tex_a), mat_tex_release(r->tex_n);
            r->tex_a = r->tex_n = 0, r->state = IMP_NONE;
            memset(F.tab_cpu[s], 0, sizeof F.tab_cpu[s]);
        }
        F.tab_dirty = true, F.st.bytes = 0, F.st.impostors = 0;
    }
    F.atlas = atlas;
    F.imp_on = imp;
    F.switch_px = gi->impostor_px > 0.0f ? gi->impostor_px : 24.0f;
    { const char* e = getenv("REALENGINE_IMPOSTOR_PX"); if (e && atof(e) > 0.0) F.switch_px = (float)atof(e); }
    if (F.switch_force > 0.0f) F.switch_px = F.switch_force;
    // the heightfield: built when wanted and the landscapes changed
    if (F.want_shadow) {
        uint32_t g = landscape_generation() ^ (F.res_want * 2654435761u);
        if (g != F.land_gen || F.force) {
            F.land_gen = g, F.force = false;
            hf_build(vk);
        }
    }
    { // (the sun's virtual maps reach half their last level's side around the camera: inside them only the far rises)
        int lv = gi->vsm_levels > 0 ? gi->vsm_levels : 1;
        F.split = VSM_LEVEL0_CM * (float)(1u << (lv - 1)) * 0.25f;
        if (F.split != F.split_done) F.shadow_dirty = true;
    }
    F.sun_on = sun != NULL;
    if (sun) {
        memcpy(F.sun, sun, sizeof F.sun);
        float d = F.sun[0] * F.sun_done[0] + F.sun[1] * F.sun_done[1] + F.sun[2] * F.sun_done[2];
        if (d < 0.9999996f) F.shadow_dirty = true; // (~0.05 degrees)
    }
    // the impostors: the candidates baked, one a frame
    if (imp) {
        uint32_t cap = atlas >= 1024 ? 40u : FARV_MAX_IMP;
        uint32_t n = render_mesh_count(), baked = 0, cand = 0, failed = 0;
        int done = 0;
        for (uint32_t s = SHAPE_COUNT; s < n && s < RE_MAX_MESHES; s++) {
            ImpRec* r = &F.rec[s];
            if (r->state == IMP_BAKED) {
                baked++, cand++;
                continue;
            }
            if (r->state == IMP_FAILED) {
                failed++;
                continue;
            }
            if (!r->key || r->blocked || (r->count < 4 && !r->plant)) continue;
            uint32_t tris[4];
            render_mesh_lods(s, tris, NULL);
            if (tris[0] < 200u) continue; // (a few triangles: the mesh is as cheap as its card)
            if (!r->plant) { // (merge of the batch 7: a prop's card only where the view reaches far; in a small room a chest's handle
                // became one, and an impostor's mere presence turns on the Masked pipelines: +0.7 ms GPU, +0.8 ms CPU in demo_fighting)
                float dsw = r->rad * FARV_FOCAL_PX / FARV_REF_PX; // (the distance where it shrinks under the switch, ~1440p at 90 degrees)
                float e[3] = { F.smx[0] - F.smn[0], F.smx[1] - F.smn[1], F.smx[2] - F.smn[2] };
                if (dsw < FARV_MIN_SWITCH_CM || sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) < dsw) continue;
            }
            cand++;
            if (done || baked >= cap) continue;
            bool ok = bake_one(vk, s);
            r->state = ok ? IMP_BAKED : IMP_FAILED;
            baked += ok, failed += !ok, done = 1;
        }
        F.st.impostors = baked, F.st.candidates = cand, F.st.failed = failed;
    }
    tab_upload(vk);
    F.st.shadow_on = F.want_shadow && F.hf_valid;
    F.st.imp_on = imp;
    F.st.atlas = atlas;
    F.st.switch_px = F.switch_px;
    F.st.fog_sky = F.fog_sky;
}

bool farview_record(VkCommandBuffer cmd) {
    if (!F.comp || !F.want_shadow || !F.hf_valid || !F.sun_on || !F.shadow_dirty) return false;
    FarParams p = { { F.origin[0], F.origin[1], F.texel, (float)F.res }, { F.sun[0], F.sun[1], F.sun[2], F.split } };
    vkCmdUpdateBuffer(cmd, F.params.buffer, 0, sizeof p, &p);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.comp);
    Push push = { F.params.addr, F.far.addr, 0, 0, 0 };
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cmd, (F.res + 7) / 8, (F.res + 7) / 8, 1);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    memcpy(F.sun_done, F.sun, sizeof F.sun);
    F.split_done = F.split;
    F.shadow_dirty = false;
    F.st.shadow_runs++;
    return true;
}

void farview_fill_frame(GpuFrame* fr, uint32_t card_slot, int32_t card_vofs) {
    uint32_t flags = 0;
    if (F.want_shadow && F.hf_valid && F.sun_on && F.st.shadow_runs) flags |= FAR_SHADOW;
    if (F.imp_on && F.st.impostors && card_slot < RE_MAX_MESHES) flags |= FAR_IMP;
    if (F.fog_sky > 0.0f) flags |= FAR_FOG_SKY;
    fr->far_buf = F.far.addr, fr->imp_tab = F.tab.addr;
    fr->far_u[0] = flags;
    fr->far_u[1] = card_slot * 4u;
    fr->far_u[2] = (uint32_t)card_vofs;
    fr->far_u[3] = F.res;
    fr->far_f[0] = F.origin[0], fr->far_f[1] = F.origin[1], fr->far_f[2] = F.texel, fr->far_f[3] = F.switch_px;
    fr->far_g[0] = F.strength;
    fr->far_g[1] = asinf(fmaxf(fminf(F.sun[2], 1.0f), -1.0f));
    fr->far_g[2] = F.fog_sky, fr->far_g[3] = 0.0f;
}

void farview_collect(double ms) { F.st.shadow_gpu_ms = ms; }
const FarviewStats* farview_stats(void) { return &F.st; }
void farview_rebuild(void) { F.force = true; }
void farview_set_switch_override(float px) { F.switch_force = px; }

// The same march as farshadow.comp on the CPU's heights, then farview.glsl's sun disk (the tests)
static float hf_at(float gx, float gy) {
    float m = (float)(F.res - 1u) - 0.001f;
    gx = fminf(fmaxf(gx, 0.0f), m), gy = fminf(fmaxf(gy, 0.0f), m);
    uint32_t ix = (uint32_t)gx, iy = (uint32_t)gy;
    float fx = gx - (float)ix, fy = gy - (float)iy;
    const float* h = F.heights;
    uint32_t a = iy * F.res + ix;
    float r0 = h[a] + (h[a + 1] - h[a]) * fx, r1 = h[a + F.res] + (h[a + F.res + 1] - h[a + F.res]) * fx;
    return r0 + (r1 - r0) * fy;
}
bool farview_shadow_at(const float p[3], float* vis) {
    *vis = 1.0f;
    if (!F.hf_valid || !F.heights || !F.sun_on) return false;
    float gx = (p[0] - F.origin[0]) / F.texel - 0.5f, gy = (p[1] - F.origin[1]) / F.texel - 0.5f;
    if (gx < 0.0f || gy < 0.0f || gx >= (float)(F.res - 1u) || gy >= (float)(F.res - 1u)) return false;
    float hl = sqrtf(F.sun[0] * F.sun[0] + F.sun[1] * F.sun[1]);
    if (hl < 1e-4f || F.sun[2] <= -0.2f) return true;
    float dx = F.sun[0] / hl, dy = F.sun[1] / hl;
    float h0 = fmaxf(p[2], hf_at(gx, gy) + 30.0f), best = -1e4f, dist = 0.0f, d = 1.0f;
    for (int k = 0; k < 96; k++) {
        float x = gx + dx * d, y = gy + dy * d;
        if (x < 0.0f || y < 0.0f || x > (float)(F.res - 1u) || y > (float)(F.res - 1u)) break;
        float t = (hf_at(x, y) - h0) / (d * F.texel);
        if (t > best) best = t, dist = d * F.texel;
        d *= 1.09f;
    }
    float e = asinf(fmaxf(fminf(F.sun[2], 1.0f), -1.0f));
    float w = 0.0047f + 0.5f * F.texel / (fmaxf(dist, 1.0f) + F.texel);
    float v = (e - atanf(best)) / (2.0f * w) + 0.5f;
    *vis = fminf(fmaxf(v, 0.0f), 1.0f);
    return true;
}
