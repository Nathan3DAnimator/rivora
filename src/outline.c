// (Q11) The outline pass: outline.h, notas-dev/toon.md
#include "outline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "debug_viewer.h"
#include "vkctx.h"

static const uint32_t s_outline_id_frag[] =
#include "outline_id.frag.inc"
    ;
static const uint32_t s_outline_comp[] =
#include "outline.comp.inc"
    ;

static struct {
    bool ready;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;        // (set 0 the renderer's, set 1 these targets)
    VkPipeline raster, comp;
    GpuImage id, depth, h;          // the ids (R32_UINT), their depth, the horizontal pass's result (R32_UINT)
    uint32_t w, h_px;
    GpuBuf table[RE_FRAMES_IN_FLIGHT]; // the frame's entries (host visible)
    GpuBuf count[RE_FRAMES_IN_FLIGHT]; // the line's pixels counted (host visible: read when the slot comes around)
    bool counted[RE_FRAMES_IN_FLIGHT];
    VkQueryPool queries;            // 2 a frame slot: the pass's start and end
    bool ts_written[RE_FRAMES_IN_FLIGHT];
    OutlineStats stats;
    double hist[64]; // the last GPU times
    uint32_t nhist;
} O;

#define PUSH_ALL (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)

static VkPipeline raster_pipeline(VkCtx* vk, VkPipelineLayout layout, VkShaderModule vs) {
    VkShaderModule fs = gpu_shader(vk, s_outline_id_frag, sizeof s_outline_id_frag);
    if (!fs) return VK_NULL_HANDLE;
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
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // (two-sided parts, open meshes: the nearest face wins by the depth)
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // (reversed Z)
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1, cb.pAttachments = &ba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R32_UINT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &cf, ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    ci.stageCount = 2, ci.pStages = st;
    ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dy;
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(vk->device, fs, NULL);
    return p;
}

bool outline_init(VkCtx* vk, VkDescriptorSetLayout set0, VkPipelineLayout raster_layout, VkShaderModule mesh_vs) {
    memset(&O, 0, sizeof O);
    VkDescriptorSetLayoutBinding b[3] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, // the ids
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL }, // their depth
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },          // the horizontal pass
    };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk->device, &li, NULL, &O.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1, pi.poolSizeCount = 2, pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(vk->device, &pi, NULL, &O.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = O.pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &O.set_layout;
    if (vkAllocateDescriptorSets(vk->device, &ai, &O.set) != VK_SUCCESS) return false;
    VkDescriptorSetLayout sets[2] = { set0, O.set_layout };
    VkPushConstantRange pr = { PUSH_ALL, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2, pli.pSetLayouts = sets, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(vk->device, &pli, NULL, &O.layout) != VK_SUCCESS) return false;
    O.comp = gpu_compute_pipeline(vk, O.layout, s_outline_comp, sizeof s_outline_comp);
    O.raster = raster_pipeline(vk, raster_layout, mesh_vs);
    for (int k = 0; k < RE_FRAMES_IN_FLIGHT; k++)
        if (!gpu_buf_create(vk, &O.table[k], sizeof(OutlineEntry) * (OUTLINE_MAX_ENTRIES + 1), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &O.count[k], 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
            return false;
    if (vk->timestamps) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * RE_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(vk->device, &qi, NULL, &O.queries) != VK_SUCCESS) O.queries = VK_NULL_HANDLE;
    }
    O.ready = O.comp && O.raster;
    if (!O.ready) printf("[RealEngine] aviso: Outline: its pipelines were not made (no outlines)\n");
    return O.ready;
}

static void point_free(VkCtx* vk); // (the targets' sampler, below)
static void targets_free(VkCtx* vk) {
    gpu_image_destroy(vk, &O.id), gpu_image_destroy(vk, &O.depth), gpu_image_destroy(vk, &O.h);
    O.w = O.h_px = 0;
}

void outline_shutdown(VkCtx* vk) {
    if (!O.set_layout) return;
    vkDeviceWaitIdle(vk->device);
    targets_free(vk);
    point_free(vk);
    for (int k = 0; k < RE_FRAMES_IN_FLIGHT; k++) gpu_buf_destroy(vk, &O.table[k]), gpu_buf_destroy(vk, &O.count[k]);
    if (O.queries) vkDestroyQueryPool(vk->device, O.queries, NULL);
    if (O.comp) vkDestroyPipeline(vk->device, O.comp, NULL);
    if (O.raster) vkDestroyPipeline(vk->device, O.raster, NULL);
    vkDestroyPipelineLayout(vk->device, O.layout, NULL);
    vkDestroyDescriptorPool(vk->device, O.pool, NULL);
    vkDestroyDescriptorSetLayout(vk->device, O.set_layout, NULL);
    memset(&O, 0, sizeof O);
}

static VkSampler g_point; // (nearest, clamped: the ids and depths read texel by texel)
static void point_free(VkCtx* vk) {
    if (g_point) vkDestroySampler(vk->device, g_point, NULL), g_point = VK_NULL_HANDLE;
}
static bool targets(VkCtx* vk, uint32_t w, uint32_t h) {
    if (w == O.w && h == O.h_px && O.id.image) return true;
    vkDeviceWaitIdle(vk->device); // (a resize: the frames in flight may read the old ones)
    targets_free(vk);
    if (!g_point) {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(vk->device, &si, NULL, &g_point) != VK_SUCCESS) return false;
    }
    bool ok = gpu_image_create(vk, &O.id, w, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &O.depth, w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) &&
              gpu_image_create(vk, &O.h, w, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!ok) {
        targets_free(vk);
        return false;
    }
    VkDescriptorImageInfo ii[3] = { { g_point, O.id.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                    { g_point, O.depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL },
                                    { VK_NULL_HANDLE, O.h.view, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++) {
        wr[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr[i].dstSet = O.set, wr[i].dstBinding = (uint32_t)i, wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(vk->device, 3, wr, 0, NULL);
    O.w = w, O.h_px = h;
    return true;
}

// The line's color as stored in the scene's pre-exposed color: what the composite's ACES (with its 0.6) turns back into
// about the chosen color (the exposure cancels with the pre-exposure)
static float aces_inverse(float y) {
    y = y < 0.0f ? 0.0f : y > 0.95f ? 0.95f : y;
    float a = 2.43f * y - 2.51f, b = 0.59f * y - 0.03f, c = 0.14f * y;
    float d = b * b - 4.0f * a * c;
    float x = a != 0.0f ? (-b - sqrtf(d > 0.0f ? d : 0.0f)) / (2.0f * a) : 0.0f;
    return (x > 0.0f ? x : 0.0f) / 0.6f;
}

void outline_entry_of(const MatToon* t, OutlineEntry* e) {
    memset(e, 0, sizeof *e);
    for (int k = 0; k < 3; k++) e->color[k] = aces_inverse(t->outline_color[k]);
    e->color[3] = t->outline_width < 0.0f ? 0.0f : t->outline_width;
    float nd = (t->near_dist > 0.0f ? t->near_dist : 0.0f) * 100.0f, fd = t->far_dist * 100.0f;
    if (fd < nd + 1.0f) fd = nd + 1.0f;
    e->curve[0] = nd, e->curve[1] = fd;
    e->curve[2] = t->adaptive ? t->near_scale : 1.0f, e->curve[3] = t->adaptive ? t->far_scale : 1.0f;
    e->edge[0] = t->curve_exp < 0.1f ? 0.1f : t->curve_exp > 8.0f ? 8.0f : t->curve_exp;
    e->edge[1] = t->depth_sens < 0.002f ? 0.002f : t->depth_sens;
    e->edge[2] = t->crease_deg > 0.5f ? cosf(t->crease_deg * 0.017453292f) : -2.0f;
    e->edge[3] = 1.0f;
}

void outline_record(VkCtx* vk, VkCommandBuffer cmd, uint32_t fs, VkDescriptorSet set0, VkDescriptorSet mat_set, VkPipelineLayout raster_layout,
                    const Push* push, uint32_t w, uint32_t h, GpuImage* color, const OutlineDraw* draws, uint32_t ndraws,
                    const OutlineEntry* entries, uint32_t nentries) {
    // last time this slot ran: its GPU time
    if (O.queries && O.ts_written[fs]) {
        uint64_t q[2];
        if (vkGetQueryPoolResults(vk->device, O.queries, fs * 2, 2, sizeof q, q, sizeof q[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            O.stats.gpu_ms = (double)(q[1] - q[0]) * vk->timestamp_ms;
            if (O.stats.gpu_ms > O.stats.gpu_max_ms && O.stats.gpu_ms < 100.0) O.stats.gpu_max_ms = O.stats.gpu_ms;
            // (the median of the last 64: a frame of pipeline making or a bake doesn't move it)
            O.hist[O.nhist++ & 63u] = O.stats.gpu_ms;
            double srt[64];
            uint32_t n = O.nhist < 64u ? O.nhist : 64u;
            memcpy(srt, O.hist, n * sizeof srt[0]);
            for (uint32_t i = 1; i < n; i++)
                for (uint32_t j = i; j > 0 && srt[j - 1] > srt[j]; j--) {
                    double t = srt[j];
                    srt[j] = srt[j - 1], srt[j - 1] = t;
                }
            O.stats.gpu_avg_ms = srt[n / 2];
        }
        O.ts_written[fs] = false;
    }
    if (O.counted[fs]) { // (its pixels counted then)
        uint32_t lines = ((const uint32_t*)O.count[fs].mapped)[0];
        O.stats.line_share = O.stats.width && O.stats.height ? (double)lines / ((double)O.stats.width * O.stats.height) : 0.0;
        O.counted[fs] = false;
    }
    O.stats.draws = ndraws, O.stats.entries = nentries, O.stats.width = w, O.stats.height = h;
    if (!O.ready || !ndraws || !nentries || !targets(vk, w, h)) {
        O.stats.gpu_ms = 0.0, O.stats.line_share = 0.0;
        return;
    }
    vkCmdFillBuffer(cmd, O.count[fs].buffer, 0, 16, 0);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, COL = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags2 DS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    gpu_mark(cmd, "contorno (Q11)");
    if (O.queries) {
        vkCmdResetQueryPool(cmd, O.queries, fs * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, O.queries, fs * 2);
    }
    memcpy(O.table[fs].mapped, entries, sizeof(OutlineEntry) * (nentries < OUTLINE_MAX_ENTRIES ? nentries + 1 : OUTLINE_MAX_ENTRIES + 1));
    // 1. the ids: the outlined actors at the render's size, jittered as the main pass (mesh.vert pass 7: the slot itself)
    gpu_transition(cmd, &O.id, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, COL,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    gpu_transition(cmd, &O.depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, DS,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = O.id.view, ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = O.depth.view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f; // (reversed Z: far)
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ w, h };
    ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca, ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, O.raster);
    VkDescriptorSet gs[2] = { set0, mat_set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, raster_layout, 0, 2, gs, 0, NULL);
    Push p = *push;
    p.pass = 7; // (mesh.vert: the instance itself, the jittered view)
    for (uint32_t i = 0; i < ndraws; i++) {
        p.pad = draws[i].entry;
        vkCmdPushConstants(cmd, raster_layout, PUSH_ALL, 0, sizeof p, &p);
        vkCmdDrawIndexed(cmd, draws[i].index_count, 1, draws[i].first_index, draws[i].vertex_offset, draws[i].slot);
    }
    vkCmdEndRendering(cmd);
    gpu_transition(cmd, &O.id, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, COL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_transition(cmd, &O.depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, DS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, CS,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    if (O.h.layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &O.h, VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
    else gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (color->layout != VK_IMAGE_LAYOUT_GENERAL)
        gpu_transition(cmd, color, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, RW, false);
    else gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, RW);
    // 2. the silhouettes and their widths, spread along x; 3. along y, into the color
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, O.comp);
    VkDescriptorSet cs[2] = { set0, O.set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, O.layout, 0, 2, cs, 0, NULL);
    p = *push;
    p.instances = O.table[fs].addr;
    p.vertices = O.count[fs].addr;
    p.pass = 0, p.pad = w | (h << 16);
    vkCmdPushConstants(cmd, O.layout, PUSH_ALL, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (w + 63) / 64, (h + 3) / 4, 1);
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    p.pass = 1;
    vkCmdPushConstants(cmd, O.layout, PUSH_ALL, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 31) / 32, 1);
    gpu_transition(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false); // (as the fires leave it)
    // (the renderer's compute bindings back: set 1 is the materials' there)
    VkDescriptorSet back[2] = { set0, mat_set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, raster_layout, 0, 2, back, 0, NULL);
    if (O.queries) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, O.queries, fs * 2 + 1);
        O.ts_written[fs] = true;
    }
    O.counted[fs] = true;
    gpu_mark(cmd, "translucidos e TSR"); // (what follows: the passes' timings don't charge it to the outline)
}

const OutlineStats* outline_stats(void) { return &O.stats; }
void outline_set_cpu_ms(double ms) { O.stats.cpu_ms = ms; }

// The Debug Viewer's list (F10): the pass's GPU time, its actors and looks, the lines' share of the view
static void dv_collect(DvList* l, const DvContext* c) {
    (void)c;
    l->gpu = true, l->estimated = false;
    l->ms = O.stats.draws ? O.stats.gpu_avg_ms : 0.0;
    snprintf(l->note, sizeof l->note, "%u outlined actors, %u looks, render %ux%u, the lines %.2f%% of the view; CPU %.3f ms (worst GPU %.3f ms)",
             O.stats.draws, O.stats.entries, O.stats.width, O.stats.height, O.stats.line_share * 100.0, O.stats.cpu_ms, O.stats.gpu_max_ms);
    char info[48];
    snprintf(info, sizeof info, "%u actors, %u looks", O.stats.draws, O.stats.entries);
    if (O.stats.draws) dv_add(l, "Outline pass (ids, silhouettes, widths)", info, (float)l->ms, ENTITY_NONE);
}
void outline_register_debug(void) { dv_register("Toon Outline", dv_collect); }
