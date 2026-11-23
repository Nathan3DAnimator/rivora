// The asset previews (render.h render_preview, notas-dev/editor.md): Unreal's asset editors' viewports drawn by the GPU
// into pictures of their own. Each preview is a handful of meshes (a Skeletal Mesh's parts at a pose, a Static Mesh, a
// Blueprint's components) from the scene's shared vertex and index buffers with their materials (the baked textures
// the main pass reads), lit by a studio (preview.frag: key light with a shadow map, fill, rim, environment), 4x MSAA,
// on a floor with the grid. The UI asks for one while it is built (its draws copied, no allocation); main.c records
// them after the scene; nothing runs while no preview is open.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h"
#include "theme.h"
#include "platform.h"
#include "render.h"
#include "render_gpu.h"
#include "vkctx.h"

static const uint32_t s_preview_vert[] =
#include "preview.vert.inc"
    ;
static const uint32_t s_preview_frag[] =
#include "preview.frag.inc"
    ;
static const uint32_t s_preview_bg_frag[] =
#include "preview_bg.frag.inc"
    ;
static const uint32_t s_fullscreen_vert[] =
#include "fullscreen.vert.inc"
    ;
static const uint32_t s_preview_ao_frag[] =
#include "preview_ao.frag.inc"
    ;

#define PV_MAX 8          // previews at once (each one's picture kept while it is asked for)
#define PV_INST_MAX 4096  // their meshes per frame
#define PV_SHADOW_SIDE 2048
#define PV_DEAD_MAX 16
#define PV_PUSH_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)

// shaders/preview.glsl PvFrame (scalar layout)
typedef struct PvFrame {
    float view_proj[16], light_matrix[16];
    float eye[4], key[4], fill[4], rim[4], floor_params[4], target[4], bg_top[4], bg_bottom[4], viewport[4];
    VkDeviceAddress materials;
    uint32_t mode, flags;
    float cam_fwd[4], cam_right[4], cam_up[4]; // (Q7) the view's axes, w: tan of the half fov (up's: y, right's: x)
    VkDeviceAddress ao_depth;                  // (Q7) the AO's depth pass (uint bits of the view depth per pixel)
    uint32_t ao_w, ao_h;
} PvFrame;
_Static_assert(sizeof(PvFrame) % 16 == 0, "PvFrame: each job's at a 16-aligned address");
#define PVI_SELECTED 1u
#define PVI_FLOOR 2u
#define PVI_LOD_SHIFT 8u // (Q7) the LOD drawn (bits 8-11): the LOD view mode's tint
#define PVF_GRID 1u
#define PVF_SHADOW 2u
#define PVF_SKY 4u // (Q7)

typedef struct PvTarget {
    uint32_t key, w, h, cap_w, cap_h, gen;
    uint64_t used;         // the frame it was last asked for
    GpuImage msaa, depth, color; // color: the resolved picture the UI samples
} PvTarget;

typedef struct PvJob {
    uint32_t target, first, count, w, h; // count: its instances (the floor's last)
    bool shadow, floor, wire, lit, ao, overdraw;
    PvFrame frame;
} PvJob;

static struct {
    VkCtx* vk;
    bool tried, ok;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline shadow_pipe, main_pipe, wire_pipe, bg_pipe, add_pipe, ao_pipe; // (Q7: Overdraw's additive, AO's depth pass)
    VkSampler shadow_sampler;
    GpuImage shadow;
    VkSampleCountFlagBits samples;
    GpuBuf buf[RE_FRAMES_IN_FLIGHT]; // per frame slot: the jobs' PvFrames, then their instances (host)
    PvTarget t[PV_MAX];
    struct { PvTarget t; uint64_t after; } dead[PV_DEAD_MAX];
    // this frame's work, gathered while the UI is built
    PvJob jobs[PV_MAX];
    uint32_t njobs, ninst;
    GpuInstance inst[PV_INST_MAX];
    bool inst_skin[PV_INST_MAX];      // its vertices are in the skin buffer (bent on the CPU this frame)
    GpuVertex* skin_cpu;              // this frame's bent skins (grown when a bigger one comes; the editor's previews)
    uint32_t nskin, skin_cap;
    GpuBuf skin[RE_FRAMES_IN_FLIGHT]; // their copy the GPU reads (host)
    VkQueryPool queries;
    bool queried[RE_FRAMES_IN_FLIGHT];
    PreviewStats stats;
    GpuBuf ao[RE_FRAMES_IN_FLIGHT]; // (Q7) the AO's depth per pixel, per frame slot (grown to the biggest view in AO)
    // (Q7) the test's readback: a picture copied into a host buffer after a frame, read once its fence waited
    struct { uint32_t key, w, h; int state; uint64_t frame; GpuBuf buf; } rb;
} P;

const PreviewStats* render_preview_stats(void) { return &P.stats; }

static void target_free(VkCtx* vk, PvTarget* t) {
    gpu_image_destroy(vk, &t->msaa), gpu_image_destroy(vk, &t->depth), gpu_image_destroy(vk, &t->color);
}

static void dead_collect(bool all) {
    for (int i = 0; i < PV_DEAD_MAX; i++)
        if (P.dead[i].t.color.image && (all || P.vk->frame_number >= P.dead[i].after)) {
            target_free(P.vk, &P.dead[i].t);
            memset(&P.dead[i], 0, sizeof P.dead[i]);
        }
}

// (a picture the GPU may still be drawing or the UI sampling: freed a few frames later)
static void target_retire(PvTarget* t) {
    if (!t->color.image) return;
    for (int i = 0; i < PV_DEAD_MAX; i++)
        if (!P.dead[i].t.color.image) {
            P.dead[i].t = *t, P.dead[i].after = P.vk->frame_number + RE_FRAMES_IN_FLIGHT + 2;
            memset(t, 0, sizeof *t);
            return;
        }
    vkDeviceWaitIdle(P.vk->device); // (every slot waiting: rare)
    target_free(P.vk, t);
    memset(t, 0, sizeof *t);
}

static VkPipeline make_pipe_ex(VkCtx* vk, VkShaderModule vs, VkShaderModule fs, bool color, VkSampleCountFlagBits samples, bool depth_test,
                               bool depth_write, bool wire, bool bias, bool additive, bool no_depth) {
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs, st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // (a handful of meshes: both sides, the shader turns the normal)
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = bias || wire;
    rs.depthBiasConstantFactor = wire ? -2.0f : 2.0f; // (the lines pulled in front of their faces; the shadow pushed back)
    rs.depthBiasSlopeFactor = wire ? -1.0f : 2.5f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depth_test, ds.depthWriteEnable = depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xF;
    if (additive) { // (Overdraw: each layer adds its share)
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE, ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = color ? 1 : 0, cb.pAttachments = &ba;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = color ? 1 : 0, ri.pColorAttachmentFormats = &cf;
    ri.depthAttachmentFormat = no_depth ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    ci.stageCount = fs ? 2 : 1, ci.pStages = st;
    ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dy;
    ci.layout = P.layout;
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = re_time_seconds();
    if (vkCreateGraphicsPipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk->pipelines_ms += (re_time_seconds() - t0) * 1000.0, vk->pipelines++;
    return p;
}
static VkPipeline make_pipe(VkCtx* vk, VkShaderModule vs, VkShaderModule fs, bool color, VkSampleCountFlagBits samples, bool depth_test,
                            bool depth_write, bool wire, bool bias) {
    return make_pipe_ex(vk, vs, fs, color, samples, depth_test, depth_write, wire, bias, false, false);
}

// Made on the first preview asked for (the packaged game never pays for them)
static bool setup(void) {
    if (P.tried) return P.ok;
    P.tried = true;
    VkCtx* vk = P.vk;
    if (!vk || !mat_set_layout()) return false;
    VkSampleCountFlags sc = vk->gpu_props.limits.framebufferColorSampleCounts & vk->gpu_props.limits.framebufferDepthSampleCounts;
    P.samples = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1, li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(vk->device, &li, NULL, &P.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1, pi.poolSizeCount = 1, pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vk->device, &pi, NULL, &P.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = P.pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &P.set_layout;
    if (vkAllocateDescriptorSets(vk->device, &ai, &P.set) != VK_SUCCESS) return false;
    VkDescriptorSetLayout sets[2] = { P.set_layout, mat_set_layout() };
    VkPushConstantRange pr = { PV_PUSH_STAGES, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 2, pl.pSetLayouts = sets, pl.pushConstantRangeCount = 1, pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(vk->device, &pl, NULL, &P.layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_TRUE, si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    if (vkCreateSampler(vk->device, &si, NULL, &P.shadow_sampler) != VK_SUCCESS) return false;
    if (!gpu_image_create(vk, &P.shadow, PV_SHADOW_SIDE, PV_SHADOW_SIDE, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    VkCommandBuffer once = gpu_begin_once(vk); // (sampled before its first write: a cleared-looking map)
    gpu_transition(once, &P.shadow, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
    gpu_end_once(vk, once);
    VkDescriptorImageInfo ii = { P.shadow_sampler, P.shadow.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = P.set, wr.descriptorCount = 1, wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk->device, 1, &wr, 0, NULL);
    VkShaderModule vs = gpu_shader(vk, s_preview_vert, sizeof s_preview_vert), fs = gpu_shader(vk, s_preview_frag, sizeof s_preview_frag);
    VkShaderModule fvs = gpu_shader(vk, s_fullscreen_vert, sizeof s_fullscreen_vert), bfs = gpu_shader(vk, s_preview_bg_frag, sizeof s_preview_bg_frag);
    P.shadow_pipe = make_pipe(vk, vs, VK_NULL_HANDLE, false, VK_SAMPLE_COUNT_1_BIT, true, true, false, true);
    P.main_pipe = make_pipe(vk, vs, fs, true, P.samples, true, true, false, false);
    P.bg_pipe = make_pipe(vk, fvs, bfs, true, P.samples, false, false, false, false);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(vk->gpu, &feats);
    if (feats.fillModeNonSolid) P.wire_pipe = make_pipe(vk, vs, fs, true, P.samples, true, false, true, false);
    P.add_pipe = make_pipe_ex(vk, vs, fs, true, P.samples, false, false, false, false, true, false);
    if (vk->frag_stores) { // (the AO's depth pass: no attachment, the fragments' depth by atomicMin into a buffer)
        VkShaderModule afs = gpu_shader(vk, s_preview_ao_frag, sizeof s_preview_ao_frag);
        P.ao_pipe = make_pipe_ex(vk, vs, afs, false, VK_SAMPLE_COUNT_1_BIT, false, false, false, false, false, true);
        vkDestroyShaderModule(vk->device, afs, NULL);
    }
    vkDestroyShaderModule(vk->device, vs, NULL), vkDestroyShaderModule(vk->device, fs, NULL);
    vkDestroyShaderModule(vk->device, fvs, NULL), vkDestroyShaderModule(vk->device, bfs, NULL);
    bool ok = P.shadow_pipe && P.main_pipe && P.bg_pipe;
    for (uint32_t i = 0; ok && i < RE_FRAMES_IN_FLIGHT; i++)
        ok = gpu_buf_create(vk, &P.buf[i], sizeof(PvFrame) * PV_MAX + sizeof(GpuInstance) * PV_INST_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (ok && vk->timestamps) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 2 * RE_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(vk->device, &qi, NULL, &P.queries) != VK_SUCCESS) P.queries = VK_NULL_HANDLE;
    }
    P.stats.wireframe = P.wire_pipe != VK_NULL_HANDLE;
    P.stats.msaa = (uint32_t)P.samples;
    return P.ok = ok;
}

bool render_preview_available(void) { return setup(); }

void render_preview_init(VkCtx* vk) { P.vk = vk; }

void render_preview_shutdown(VkCtx* vk) {
    if (!P.vk) return;
    vkDeviceWaitIdle(vk->device);
    for (int i = 0; i < PV_MAX; i++) target_free(vk, &P.t[i]);
    dead_collect(true);
    for (uint32_t i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &P.buf[i]), gpu_buf_destroy(vk, &P.skin[i]), gpu_buf_destroy(vk, &P.ao[i]);
    gpu_buf_destroy(vk, &P.rb.buf);
    free(P.skin_cpu), P.skin_cpu = NULL, P.skin_cap = P.nskin = 0;
    gpu_image_destroy(vk, &P.shadow);
    VkPipeline pp[6] = { P.shadow_pipe, P.main_pipe, P.wire_pipe, P.bg_pipe, P.add_pipe, P.ao_pipe };
    for (int i = 0; i < 6; i++)
        if (pp[i]) vkDestroyPipeline(vk->device, pp[i], NULL);
    if (P.queries) vkDestroyQueryPool(vk->device, P.queries, NULL);
    if (P.shadow_sampler) vkDestroySampler(vk->device, P.shadow_sampler, NULL);
    if (P.layout) vkDestroyPipelineLayout(vk->device, P.layout, NULL);
    if (P.pool) vkDestroyDescriptorPool(vk->device, P.pool, NULL);
    if (P.set_layout) vkDestroyDescriptorSetLayout(vk->device, P.set_layout, NULL);
    memset(&P, 0, sizeof P);
}

// column-major 4x4 (GLSL mat4): rows r0..r3 of the math
static void mat_rows(float m[16], const float r[4][4]) {
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) m[c * 4 + k] = r[k][c];
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-8f) v[0] /= l, v[1] /= l, v[2] /= l;
}
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static void srgb_of(uint32_t abgr, float out[4]) {
    for (int k = 0; k < 3; k++) out[k] = (float)((abgr >> (8 * k)) & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

// A target able to hold w x h (made again when it is too small or far too big: a window resized keeps most of them)
static void oct_dec(uint32_t o, float n[3]) {
    float x = (float)(int16_t)(o & 0xFFFFu) / 32767.0f, y = (float)(int16_t)(o >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}
static uint32_t oct_enc(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-20f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t sx = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, x)) * 32767.0f), sy = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, y)) * 32767.0f);
    return (uint32_t)(uint16_t)sx | ((uint32_t)(uint16_t)sy << 16);
}

// A skin bent by its palette (skin.comp's, on the CPU): its vertices [vmin, vmax] into the skin buffer; their first
// there (UINT32_MAX: no room); its bounds widened
static uint32_t skin_bend(const GpuVertex* v, int32_t off, uint32_t vmin, uint32_t vmax, const float (*pal)[3][4], uint32_t nb,
                          float mn[3], float mx[3]) {
    uint32_t nv = vmax - vmin + 1;
    if (P.nskin + nv > P.skin_cap) {
        uint32_t cap = (P.nskin + nv) * 2u;
        if (cap > (32u << 20) / sizeof(GpuVertex)) return UINT32_MAX; // (32 MB: no skin that big in a preview)
        GpuVertex* n = realloc(P.skin_cpu, sizeof(GpuVertex) * cap);
        if (!n) return UINT32_MAX;
        P.skin_cpu = n, P.skin_cap = cap;
    }
    uint32_t base = P.nskin;
    for (uint32_t i = 0; i < nv; i++) {
        const GpuVertex* s = &v[(int64_t)vmin + i + off];
        GpuVertex* o = &P.skin_cpu[base + i];
        uint32_t bits[2];
        memcpy(bits, s->uv2, 8);
        float r[3][4] = { { 0 } }, wsum = 0.0f;
        for (int k = 0; k < 4; k++) {
            float w = (float)((bits[1] >> (k * 8)) & 255u) / 255.0f;
            uint32_t b = (bits[0] >> (k * 8)) & 255u;
            if (w <= 0.0f || b >= nb) continue;
            wsum += w;
            for (int a = 0; a < 3; a++)
                for (int c = 0; c < 4; c++) r[a][c] += pal[b][a][c] * w;
        }
        if (wsum <= 0.0f) r[0][0] = r[1][1] = r[2][2] = 1.0f; // (no weight: as it is)
        float n[3], on[3];
        oct_dec(s->normal_oct, n);
        for (int a = 0; a < 3; a++) {
            o->pos[a] = r[a][0] * s->pos[0] + r[a][1] * s->pos[1] + r[a][2] * s->pos[2] + r[a][3];
            on[a] = r[a][0] * n[0] + r[a][1] * n[1] + r[a][2] * n[2];
            mn[a] = fminf(mn[a], o->pos[a]), mx[a] = fmaxf(mx[a], o->pos[a]);
        }
        o->normal_oct = oct_enc(on);
        o->uv[0] = s->uv[0], o->uv[1] = s->uv[1];
        o->uv2[0] = o->uv2[1] = -1.0f; // (no atlas)
    }
    P.nskin += nv;
    return base;
}

static PvTarget* target_for(uint32_t key, uint32_t w, uint32_t h) {
    PvTarget* t = NULL;
    for (int i = 0; i < PV_MAX && !t; i++)
        if (P.t[i].color.image && P.t[i].key == key) t = &P.t[i];
    if (!t) { // a free one, else the one asked for longest ago
        for (int i = 0; i < PV_MAX && !t; i++)
            if (!P.t[i].color.image) t = &P.t[i];
        if (!t) {
            t = &P.t[0];
            for (int i = 1; i < PV_MAX; i++)
                if (P.t[i].used < t->used) t = &P.t[i];
            if (t->used + 2 >= P.vk->frame_number) return NULL; // (all of them drawn right now)
            target_retire(t);
        }
        t->key = key;
    }
    if (!t->color.image || w > t->cap_w || h > t->cap_h || w * 2 < t->cap_w || h * 2 < t->cap_h) {
        uint32_t gen = t->gen;
        target_retire(t);
        t->key = key, t->gen = gen + 1, P.stats.made++;
        t->cap_w = (w + 63u) & ~63u, t->cap_h = (h + 63u) & ~63u;
        VkCtx* vk = P.vk;
        bool ok = gpu_image_create(vk, &t->color, t->cap_w, t->cap_h, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
                  gpu_image_create(vk, &t->depth, t->cap_w, t->cap_h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, P.samples,
                                   VK_IMAGE_ASPECT_DEPTH_BIT) &&
                  (P.samples == VK_SAMPLE_COUNT_1_BIT ||
                   gpu_image_create(vk, &t->msaa, t->cap_w, t->cap_h, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, P.samples, VK_IMAGE_ASPECT_COLOR_BIT));
        if (!ok) {
            target_free(vk, t);
            memset(t, 0, sizeof *t);
            return NULL;
        }
    }
    t->w = w, t->h = h;
    return t;
}

VkImageView render_preview(uint32_t key, const PreviewView* v, const PreviewDraw* d, uint32_t n, uint32_t* gen, float uv_max[2]) {
    if (!setup() || v->w < 8 || v->h < 8) return VK_NULL_HANDLE;
    double t0 = re_time_seconds();
    dead_collect(false);
    for (uint32_t j = 0; j < P.njobs; j++) // (asked twice in a frame: the last one wins)
        if (P.t[P.jobs[j].target].key == key) return VK_NULL_HANDLE;
    if (P.njobs >= PV_MAX) return VK_NULL_HANDLE;
    PvTarget* t = target_for(key, v->w, v->h);
    if (!t) return VK_NULL_HANDLE;
    t->used = P.vk->frame_number;
    PvJob* job = &P.jobs[P.njobs];
    memset(job, 0, sizeof *job);
    job->target = (uint32_t)(t - P.t), job->first = P.ninst, job->w = v->w, job->h = v->h;
    // the meshes: their instances, the bounds the shadow is fitted to
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    uint64_t tris = 0;
    float lmn_skin[3] = { 0 }, lmx_skin[3] = { 0 };
    for (uint32_t i = 0; i < n && P.ninst < PV_INST_MAX - 1; i++) {
        GpuInstance* o = &P.inst[P.ninst];
        uint32_t first, count;
        int32_t off;
        uint32_t lod = d[i].palette && d[i].nbones ? 0u : d[i].lod;
        if (!render_mesh_lod_range(d[i].mesh, lod, &first, &count, &off) || !count) continue;
        memset(o, 0, sizeof *o);
        memcpy(o->rows, d[i].rows, sizeof o->rows);
        P.inst_skin[P.ninst] = false;
        if (d[i].palette && d[i].nbones) { // a skin: bent here into the skin buffer (its vertices' range read from its indices)
            const GpuVertex* cv;
            const uint32_t* ci;
            uint32_t cf, cc;
            int32_t co;
            render_mesh_cpu(d[i].mesh, &cv, &ci, &cf, &cc, &co);
            if (!cv || !ci || !cc) continue;
            uint32_t vmin = UINT32_MAX, vmax = 0;
            for (uint32_t k = cf; k < cf + cc; k++) vmin = ci[k] < vmin ? ci[k] : vmin, vmax = ci[k] > vmax ? ci[k] : vmax;
            float smn[3] = { 1e30f, 1e30f, 1e30f }, smx[3] = { -1e30f, -1e30f, -1e30f };
            uint32_t base = skin_bend(cv, co, vmin, vmax, d[i].palette, d[i].nbones, smn, smx);
            if (base == UINT32_MAX) continue;
            P.inst_skin[P.ninst] = true;
            first = cf, count = cc;
            off = (int32_t)((int64_t)base - (int64_t)vmin); // (the skin buffer's: index + this = its vertex)
            memcpy(lmn_skin, smn, sizeof smn), memcpy(lmx_skin, smx, sizeof smx);
        }
        memcpy(o->base_metal, d[i].color, 12), o->base_metal[3] = d[i].metallic;
        memcpy(o->emissive_rough, d[i].emissive, 12), o->emissive_rough[3] = d[i].roughness;
        o->ids[1] = (d[i].selected ? PVI_SELECTED : 0u) | ((lod < 15u ? lod : 15u) << PVI_LOD_SHIFT);
        o->ids[2] = first, o->ids[3] = count; // (the draw's range: first index, count; the vertex offset below)
        o->lightmap_st[0] = (float)off;
        if (d[i].material) {
            Material* m = mat_find_hash(d[i].material);
            if (m && m->slot) {
                uint32_t slot = m->slot;
                memcpy(&o->emissive_rough[0], &slot, 4);
                o->emissive_rough[1] = d[i].uv_scale > 0.0f ? d[i].uv_scale : 1.0f;
                {
                    float nits[3] = { m->avg_emis[0] * 100.0f, m->avg_emis[1] * 100.0f, m->avg_emis[2] * 100.0f };
                    uint32_t pk = render_emis_pack(nits, false); // (2026-11-23) the preview's GI sees its glow
                    memcpy(&o->emissive_rough[2], &pk, 4);
                }
                o->ids[1] |= INST_MATERIAL;
                if (!d[i].selected) memcpy(o->base_metal, m->avg_base, 12);
            }
        }
        float lmn[3], lmx[3];
        if (P.inst_skin[P.ninst]) memcpy(lmn, lmn_skin, sizeof lmn), memcpy(lmx, lmx_skin, sizeof lmx); // (bent: its own)
        else render_mesh_bounds(d[i].mesh, lmn, lmx);
        for (int a = 0; a < 3; a++) {
            float c = d[i].rows[a][3], e = 0.0f;
            for (int k = 0; k < 3; k++) {
                c += d[i].rows[a][k] * (lmn[k] + lmx[k]) * 0.5f;
                e += fabsf(d[i].rows[a][k]) * (lmx[k] - lmn[k]) * 0.5f;
            }
            mn[a] = fminf(mn[a], c - e), mx[a] = fmaxf(mx[a], c + e);
        }
        tris += count / 3;
        P.ninst++;
    }
    job->count = P.ninst - job->first;
    bool any = job->count > 0;
    if (!any)
        for (int a = 0; a < 3; a++) mn[a] = v->target[a] - 50.0f, mx[a] = v->target[a] + 50.0f;
    uint32_t mode = v->mode < PREVIEW_MODE_COUNT ? v->mode : PREVIEW_LIT;
    if ((mode == PREVIEW_WIREFRAME || mode == PREVIEW_LIT_WIREFRAME) && !P.wire_pipe) mode = PREVIEW_LIT;
    if (mode == PREVIEW_AO && !P.ao_pipe) mode = PREVIEW_LIGHTING_ONLY;
    if (mode == PREVIEW_OVERDRAW && !P.add_pipe) mode = PREVIEW_LIT;
    job->wire = mode == PREVIEW_WIREFRAME || mode == PREVIEW_LIT_WIREFRAME;
    job->lit = mode != PREVIEW_WIREFRAME;
    job->ao = mode == PREVIEW_AO, job->overdraw = mode == PREVIEW_OVERDRAW;
    bool lit_mode = mode == PREVIEW_LIT || mode == PREVIEW_LIT_WIREFRAME || mode == PREVIEW_LIGHTING_ONLY || mode == PREVIEW_LOD;
    job->shadow = any && lit_mode;
    float radius = fmaxf(v->radius, 10.0f);
    // the floor: a plane under it, fading out
    job->floor = (v->flags & PREVIEW_FLOOR) && mode != PREVIEW_WIREFRAME && mode != PREVIEW_OVERDRAW && P.ninst < PV_INST_MAX;
    float floor_r = fmaxf(radius * 7.0f, 400.0f);
    if (job->floor) {
        GpuInstance* o = &P.inst[P.ninst];
        uint32_t first, count;
        int32_t off;
        if (render_mesh_range(SHAPE_PLANE, &first, &count, &off)) {
            memset(o, 0, sizeof *o);
            float s = floor_r * 2.0f / 100.0f;
            o->rows[0][0] = s, o->rows[1][1] = s, o->rows[2][2] = 1.0f;
            o->rows[0][3] = v->target[0], o->rows[1][3] = v->target[1], o->rows[2][3] = v->floor_z;
            o->emissive_rough[3] = 0.9f;
            o->ids[1] = PVI_FLOOR, o->ids[2] = first, o->ids[3] = count;
            o->lightmap_st[0] = (float)off;
            P.ninst++;
        } else
            job->floor = false;
    }
    // the camera (vertical fov; Unreal's left-handed axes: right is the screen's)
    PvFrame* f = &job->frame;
    float aspect = (float)v->w / (float)v->h, ty = tanf(fminf(fmaxf(v->fov_deg, 5.0f), 170.0f) * 0.5f * 0.0174533f), tx = ty * aspect;
    float dist = sqrtf((v->eye[0] - v->target[0]) * (v->eye[0] - v->target[0]) + (v->eye[1] - v->target[1]) * (v->eye[1] - v->target[1]) +
                       (v->eye[2] - v->target[2]) * (v->eye[2] - v->target[2]));
    float nearz = fmaxf(fminf(dist - radius * 2.0f, dist * 0.2f), 0.5f), farz = dist + floor_r * 2.0f + radius * 4.0f;
    float A = farz / (farz - nearz), B = -farz * nearz / (farz - nearz);
    float r[4][4];
    for (int k = 0; k < 3; k++) r[0][k] = v->right[k] / tx, r[1][k] = -v->up[k] / ty, r[3][k] = v->fwd[k], r[2][k] = v->fwd[k] * A;
    r[0][3] = -dot3(v->right, v->eye) / tx, r[1][3] = dot3(v->up, v->eye) / ty, r[3][3] = -dot3(v->fwd, v->eye);
    r[2][3] = r[3][3] * A + B;
    mat_rows(f->view_proj, r);
    // the lights, turned with the camera (every view lit alike): the key from its upper left, a fill from the right, a rim behind
    for (int k = 0; k < 3; k++) {
        f->key[k] = -v->fwd[k] * 0.55f - v->right[k] * 0.55f + v->up[k] * 0.65f;
        f->fill[k] = -v->fwd[k] * 0.6f + v->right[k] * 0.8f - v->up[k] * 0.1f;
        f->rim[k] = v->fwd[k] * 0.8f + v->up[k] * 0.45f;
    }
    f->key[2] += 0.35f;
    if (v->flags & PREVIEW_LIGHT_WORLD) memcpy(f->key, v->light_dir, 12); // (turned by the user: L + drag, the Lighting menu)
    if (f->key[2] < 0.05f) f->key[2] = 0.05f;                              // (never from under the floor)
    norm3(f->key), norm3(f->fill), norm3(f->rim);
    f->key[3] = 3.0f * (v->light_scale > 0.0f ? v->light_scale : 1.0f), f->fill[3] = 0.9f, f->rim[3] = 1.6f;
    // the shadow map: an orthographic box along the key light around the meshes (and the floor near them)
    float c[3], e[3];
    for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f, e[k] = (mx[k] - mn[k]) * 0.5f;
    float R = fmaxf(sqrtf(dot3(e, e)), 5.0f), Rs = R * 1.6f;
    float lz[3] = { -f->key[0], -f->key[1], -f->key[2] }, lx[3], ly[3], wup[3] = { 0, 0, 1 };
    cross3(fabsf(lz[2]) > 0.95f ? (float[3]){ 1, 0, 0 } : wup, lz, lx);
    norm3(lx);
    cross3(lz, lx, ly);
    float depth = R * 8.0f;
    for (int k = 0; k < 3; k++) r[0][k] = lx[k] / Rs, r[1][k] = ly[k] / Rs, r[2][k] = lz[k] / depth, r[3][k] = 0.0f;
    r[0][3] = -dot3(lx, c) / Rs, r[1][3] = -dot3(ly, c) / Rs, r[2][3] = -dot3(lz, c) / depth + 0.5f, r[3][3] = 1.0f;
    mat_rows(f->light_matrix, r);
    memcpy(f->eye, v->eye, 12), f->eye[3] = v->time;
    float size = sqrtf(dot3(e, e));
    f->floor_params[0] = v->floor_z, f->floor_params[1] = floor_r;
    f->floor_params[2] = size < 60.0f ? 10.0f : size < 400.0f ? 50.0f : 100.0f;
    f->floor_params[3] = Rs * 2.0f / (float)PV_SHADOW_SIDE;
    memcpy(f->target, v->target, 12), f->target[3] = radius;
    srgb_of(TH(TK_PREVIEW_TOP), f->bg_top), srgb_of(TH(TK_PREVIEW_BOTTOM), f->bg_bottom); // (the theme's preview gradient, bp_preview.c's)
    f->viewport[0] = (float)v->w, f->viewport[1] = (float)v->h, f->viewport[2] = 1.0f / (float)v->w, f->viewport[3] = 1.0f / (float)v->h;
    f->materials = mat_buffer_address();
    f->mode = mode;
    f->flags = ((v->flags & PREVIEW_GRID) ? PVF_GRID : 0u) | (job->shadow ? PVF_SHADOW : 0u) | ((v->flags & PREVIEW_SKY) ? PVF_SKY : 0u);
    memcpy(f->cam_fwd, v->fwd, 12), memcpy(f->cam_right, v->right, 12), memcpy(f->cam_up, v->up, 12);
    f->cam_up[3] = ty, f->cam_right[3] = tx, f->cam_fwd[3] = 0.0f;
    f->ao_depth = 0, f->ao_w = v->w, f->ao_h = v->h; // (the address: at record, the slot's buffer)
    P.njobs++;
    P.stats.cpu_ms += (re_time_seconds() - t0) * 1000.0;
    P.stats.frame_views++, P.stats.frame_triangles += tris;
    if (gen) *gen = t->gen;
    if (uv_max) uv_max[0] = (float)v->w / (float)t->cap_w, uv_max[1] = (float)v->h / (float)t->cap_h;
    return t->color.view;
}

// The instances first..first+count (a skin's vertices from the skin buffer: the push's vertex address switched to it)
static void draw_range(VkCommandBuffer cmd, uint32_t first, uint32_t count, Push* push, VkDeviceAddress skin) {
    VkDeviceAddress shared = render_vertex_address();
    for (uint32_t i = first; i < first + count; i++) {
        const GpuInstance* o = &P.inst[i];
        if (P.inst_skin[i] && !skin) continue;
        VkDeviceAddress want = P.inst_skin[i] ? skin : shared;
        if (push->vertices != want) {
            push->vertices = want;
            vkCmdPushConstants(cmd, P.layout, PV_PUSH_STAGES, 0, sizeof *push, push);
        }
        vkCmdDrawIndexed(cmd, o->ids[3], 1, o->ids[2], (int32_t)o->lightmap_st[0], i);
    }
}

static void set_view(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void render_preview_record(VkCtx* vk, VkCommandBuffer cmd) {
    uint32_t slot = vk->frame;
    // last time this slot ran (its frame is done: the fence waited): its GPU time
    if (P.queries && P.queried[slot]) {
        uint64_t q[2];
        if (vkGetQueryPoolResults(vk->device, P.queries, slot * 2, 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            P.stats.gpu_ms = (double)(q[1] - q[0]) * vk->timestamp_ms;
        P.queried[slot] = false;
    }
    P.stats.views = P.stats.frame_views, P.stats.triangles = P.stats.frame_triangles, P.stats.last_cpu_ms = P.stats.cpu_ms;
    P.stats.frame_views = 0, P.stats.frame_triangles = 0, P.stats.cpu_ms = 0.0;
    if (!P.njobs) {
        P.stats.gpu_ms = 0.0;
        return;
    }
    // (Q7) the AO's depth buffer of this slot: as big as the biggest view in AO this frame
    VkDeviceSize ao_need = 0;
    for (uint32_t j = 0; j < P.njobs; j++)
        if (P.jobs[j].ao && (VkDeviceSize)P.jobs[j].w * P.jobs[j].h * 4u > ao_need) ao_need = (VkDeviceSize)P.jobs[j].w * P.jobs[j].h * 4u;
    if (ao_need && P.ao[slot].size < ao_need) {
        gpu_buf_destroy(vk, &P.ao[slot]); // (this slot's frame is done: its fence waited)
        if (!gpu_buf_create(vk, &P.ao[slot], ao_need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
            memset(&P.ao[slot], 0, sizeof P.ao[slot]);
    }
    GpuBuf* b = &P.buf[slot];
    VkDeviceAddress inst_addr = b->addr + sizeof(PvFrame) * PV_MAX;
    memcpy((uint8_t*)b->mapped + sizeof(PvFrame) * PV_MAX, P.inst, sizeof(GpuInstance) * P.ninst);
    // the skins bent this frame: into this slot's buffer (grown when they don't fit)
    VkDeviceAddress skin_addr = 0;
    if (P.nskin) {
        GpuBuf* sb = &P.skin[slot];
        VkDeviceSize need = sizeof(GpuVertex) * (VkDeviceSize)P.nskin;
        if (sb->size < need) {
            gpu_buf_destroy(vk, sb); // (this slot's frame is done: its fence waited)
            if (!gpu_buf_create(vk, sb, need + need / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) memset(sb, 0, sizeof *sb);
        }
        if (sb->mapped) memcpy(sb->mapped, P.skin_cpu, (size_t)need), skin_addr = sb->addr;
    }
    if (P.queries) {
        vkCmdResetQueryPool(cmd, P.queries, slot * 2, 2);
        // (Q7: after the work before it: at the top of the pipe it also timed the scene's tail, 0.1 ms read as 0.8)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, P.queries, slot * 2);
    }
    VkDescriptorSet sets[2] = { P.set, mat_set() };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.layout, 0, 2, sets, 0, NULL);
    vkCmdBindIndexBuffer(cmd, render_index_buffer(), 0, VK_INDEX_TYPE_UINT32);
    for (uint32_t j = 0; j < P.njobs; j++) {
        PvJob* job = &P.jobs[j];
        PvTarget* t = &P.t[job->target];
        if (job->ao && !P.ao[slot].buffer) job->ao = false, job->frame.mode = PREVIEW_LIGHTING_ONLY;
        job->frame.ao_depth = job->ao ? P.ao[slot].addr : 0;
        memcpy((uint8_t*)b->mapped + sizeof(PvFrame) * j, &job->frame, sizeof(PvFrame));
        Push push = { b->addr + sizeof(PvFrame) * j, render_vertex_address(), inst_addr, 0, 0 };
        // the key light's shadow map (one for all: each preview's after the last one read it)
        if (job->shadow) {
            gpu_transition(cmd, &P.shadow, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
            VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            da.imageView = P.shadow.view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea.extent = (VkExtent2D){ PV_SHADOW_SIDE, PV_SHADOW_SIDE }, ri.layerCount = 1, ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            set_view(cmd, PV_SHADOW_SIDE, PV_SHADOW_SIDE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.shadow_pipe);
            push.pass = 1;
            vkCmdPushConstants(cmd, P.layout, PV_PUSH_STAGES, 0, sizeof push, &push);
            draw_range(cmd, job->first, job->count, &push, skin_addr);
            vkCmdEndRendering(cmd);
            gpu_transition(cmd, &P.shadow, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        }
        // (Q7) the AO's depth pass: the nearest view depth of each pixel (atomicMin; the floor too: the contact)
        if (job->ao) {
            VkDeviceSize bytes = (VkDeviceSize)job->w * job->h * 4u;
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(cmd, P.ao[slot].buffer, 0, bytes, UINT32_MAX); // (the farthest depth: nothing drawn there)
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            VkRenderingInfo ari = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            ari.renderArea.extent = (VkExtent2D){ job->w, job->h }, ari.layerCount = 1;
            vkCmdBeginRendering(cmd, &ari);
            set_view(cmd, job->w, job->h);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.ao_pipe);
            push.pass = 4;
            vkCmdPushConstants(cmd, P.layout, PV_PUSH_STAGES, 0, sizeof push, &push);
            draw_range(cmd, job->first, job->count + (job->floor ? 1u : 0u), &push, skin_addr);
            vkCmdEndRendering(cmd);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            push.vertices = render_vertex_address();
        }
        // the picture: the background, the meshes and the floor (4x MSAA, resolved into what the UI samples), the lines
        bool ms = P.samples != VK_SAMPLE_COUNT_1_BIT;
        GpuImage* draw_img = ms ? &t->msaa : &t->color;
        gpu_transition(cmd, &t->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
        if (ms)
            gpu_transition(cmd, &t->msaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
        gpu_transition(cmd, &t->depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = draw_img->view, ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, ca.storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (ms) {
            ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca.resolveImageView = t->color.view, ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        da.imageView = t->depth.view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea.extent = (VkExtent2D){ job->w, job->h }, ri.layerCount = 1;
        ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca, ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        set_view(cmd, job->w, job->h);
        push.pass = 0, push.vertices = render_vertex_address();
        vkCmdPushConstants(cmd, P.layout, PV_PUSH_STAGES, 0, sizeof push, &push);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.bg_pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        uint32_t nmesh = job->count, nall = P.ninst; // (the floor: the instance after the meshes)
        (void)nall;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, job->overdraw ? P.add_pipe : P.main_pipe);
        if (job->lit) draw_range(cmd, job->first, nmesh, &push, skin_addr);
        if (job->overdraw) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.main_pipe);
        if (job->floor) draw_range(cmd, job->first + nmesh, 1, &push, skin_addr);
        if (job->wire) {
            push.pass = 3;
            vkCmdPushConstants(cmd, P.layout, PV_PUSH_STAGES, 0, sizeof push, &push);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.wire_pipe);
            draw_range(cmd, job->first, nmesh, &push, skin_addr);
        }
        vkCmdEndRendering(cmd);
        if (P.rb.state == 1 && P.rb.key == t->key) { // (Q7: the test's readback) the picture into a host buffer
            VkDeviceSize bytes = (VkDeviceSize)job->w * job->h * 4u;
            if (P.rb.buf.size < bytes) {
                vkDeviceWaitIdle(vk->device); // (a test only: the buffer may be read by nothing else)
                gpu_buf_destroy(vk, &P.rb.buf);
                if (!gpu_buf_create(vk, &P.rb.buf, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) memset(&P.rb.buf, 0, sizeof P.rb.buf);
            }
            if (P.rb.buf.buffer) {
                gpu_transition(cmd, &t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
                VkBufferImageCopy bc = { 0 };
                bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, bc.imageSubresource.layerCount = 1;
                bc.imageExtent = (VkExtent3D){ job->w, job->h, 1 };
                vkCmdCopyImageToBuffer(cmd, t->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.rb.buf.buffer, 1, &bc);
                gpu_transition(cmd, &t->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
                P.rb.w = job->w, P.rb.h = job->h, P.rb.state = 2, P.rb.frame = vk->frame_number;
                continue;
            }
        }
        gpu_transition(cmd, &t->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    if (P.queries) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, P.queries, slot * 2 + 1);
        P.queried[slot] = true;
    }
    P.stats.drawn_frames++;
    P.njobs = P.ninst = 0, P.nskin = 0;
}

// --- (Q7) the test's readback ---
void render_preview_readback(uint32_t key) { P.rb.key = key, P.rb.state = 1; }
bool render_preview_read(uint32_t key, const uint8_t** rgba, uint32_t* w, uint32_t* h) {
    if (P.rb.key != key || P.rb.state < 2 || !P.vk || P.vk->frame_number < P.rb.frame + RE_FRAMES_IN_FLIGHT + 1) return false;
    *rgba = P.rb.buf.mapped, *w = P.rb.w, *h = P.rb.h;
    return P.rb.buf.mapped != NULL;
}
