#include "material.h" // (Q4: mat_set, the Light Functions)
#include "weather.h"
#include "weather_field.h"
#include "water.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloud_noise.h"
#include "components.h"
#include "lights.h" // (lights_level_weight: a Lighting Level's blend)
#include "platform.h"
#include "vkctx.h"
#include "render.h" // (render_transform_rows)

static const uint32_t s_weather[] =
#include "weather.comp.inc"
    ;
static const uint32_t s_weather_rt[] =
#include "weather_rt.comp.inc"
    ;

// The images (shaders/weather.glsl): the panorama 1024 x 512, the shadow 256 x 256 over 20 km, the fog's froxels
// 128 x 72 x 64 as an atlas of slices, the noise 64^3 as an atlas
#define PANO_W 3072 // ~8.5 px per degree: at 1024 x 512 a 1440p view magnified it ten times (blurred blobs)
#define PANO_H 768
#define PANO_PHASES 64 // the rows updated a frame: one in 64 (the wind is slow: the whole sky every 64 frames, ~0.3 s at 200 fps; the Tlender march is heavier)
#define SHADOW_SIDE 256
#define SHADOW_SPAN 2.0e6f // cm
#define FOG_X 128
#define FOG_Y 72
#define FOG_Z 64

#define CLOUD_SCR_W 1920 // the screen's clouds: half of 4K each way (Epic: half the view; High a quarter: a smaller corner, GpuFrame.cloud_scr)
#define CLOUD_SCR_H 1080

static struct {
    VkPipeline pipe;
    VkPipelineLayout layout;
    GpuImage img[7]; // pano, shadow, fog, fog light, noise, the screen's clouds (written), their history (read)
    GpuBuf sky;      // the SH with the clouds
    GpuImage detail; // the clouds' detail noise (3D; img[4] the shape)
    GpuImage wmap;   // their coverage map (2D, tiling)
    GpuBuf tables;   // their coverage thresholds and prefiltered means (cloud_noise.c)
    double noise_ms; // (made at startup on the CPU)
    bool have_clouds, have_fog, volume;
    uint32_t phase;
    GpuBuf fogvol[RE_FRAMES_IN_FLIGHT]; // (A9.5b) the Local Fog Volumes a frame (one buffer per frame in flight)
    uint32_t fogvol_slot;
} W;
#define FOGVOL_MAX 16

VkImageView weather_view(int i) { return i == 8 ? W.wmap.view : i == 7 ? W.detail.view : W.img[i >= 0 && i < 7 ? i : 0].view; } // (7: the detail noise)

static bool make(VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h, VkFormat fmt) {
    return gpu_image_create(vk, im, w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

static bool make_tex(VkCtx* vk, GpuImage* im, uint32_t n, uint32_t depth, VkFormat fmt, const void* data, size_t bytes);
// A 3D (n x n x n) or 2D (depth 1: n x n) texture of the clouds: sampled with repeat, filled once from the CPU
static bool make3d(VkCtx* vk, GpuImage* im, uint32_t n, VkFormat fmt, const void* data, size_t bytes) { return make_tex(vk, im, n, n, fmt, data, bytes); }
static bool make_tex(VkCtx* vk, GpuImage* im, uint32_t n, uint32_t depth, VkFormat fmt, const void* data, size_t bytes) {
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = (VkExtent3D){ n, n, depth };
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &im->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im->image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &im->memory) != VK_SUCCESS) return false;
    vkBindImageMemory(vk->device, im->image, im->memory, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->image;
    vi.viewType = depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(vk->device, &vi, NULL, &im->view) != VK_SUCCESS) return false;
    im->format = fmt;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    GpuBuf st;
    if (!gpu_buf_create(vk, &st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(st.mapped, data, bytes);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { n, n, depth } };
    vkCmdCopyBufferToImage(cmd, st.buffer, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    gpu_transition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &st);
    return true;
}

// The clouds' noise (cloud_noise.c, the Tlender's model): the shape and detail textures and the coverage tables
static bool make_noise(VkCtx* vk) {
    size_t ns = (size_t)CLOUD_SHAPE_N * CLOUD_SHAPE_N * CLOUD_SHAPE_N * 4, nd = (size_t)CLOUD_DETAIL_N * CLOUD_DETAIL_N * CLOUD_DETAIL_N * 4;
    uint16_t* shape = malloc(ns * sizeof *shape);
    uint8_t* detail = malloc(nd);
    uint16_t* wmap = malloc(sizeof(uint16_t) * CLOUD_WEATHER_N * CLOUD_WEATHER_N);
    float tables[CLOUD_TABLES];
    double t0 = re_time_seconds();
    bool ok = shape && detail && wmap && cloud_noise_make(shape, detail, tables);
    if (ok) cloud_weather_make(wmap);
    if (ok) ok = make3d(vk, &W.img[4], CLOUD_SHAPE_N, VK_FORMAT_R16G16B16A16_UNORM, shape, ns * sizeof *shape) &&
                 make3d(vk, &W.detail, CLOUD_DETAIL_N, VK_FORMAT_R8G8B8A8_UNORM, detail, nd) &&
                 make_tex(vk, &W.wmap, CLOUD_WEATHER_N, 1, VK_FORMAT_R16_UNORM, wmap, sizeof(uint16_t) * CLOUD_WEATHER_N * CLOUD_WEATHER_N) &&
                 gpu_buf_create(vk, &W.tables, sizeof tables, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (ok) memcpy(W.tables.mapped, tables, sizeof tables);
    free(shape);
    free(detail);
    free(wmap);
    W.noise_ms = (re_time_seconds() - t0) * 1000.0;
    return ok;
}

static VkCtx* g_wvk; // (A9.5b: the fog volumes' buffers, made when first needed)
bool weather_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&W, 0, sizeof W);
    g_wvk = vk;
    W.layout = layout;
    if (!make(vk, &W.img[0], PANO_W, PANO_H, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(vk, &W.img[1], SHADOW_SIDE, SHADOW_SIDE, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(vk, &W.img[2], FOG_X * FOG_Z, FOG_Y, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(vk, &W.img[3], FOG_X * FOG_Z, FOG_Y, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(vk, &W.img[5], CLOUD_SCR_W, CLOUD_SCR_H, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !make(vk, &W.img[6], CLOUD_SCR_W, CLOUD_SCR_H, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !gpu_buf_create(vk, &W.sky, 16 * (9 + 4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) // (+4: the clouds' light, pass 7)
        return false;
    W.pipe = vk->ray_query ? gpu_compute_pipeline(vk, layout, s_weather_rt, sizeof s_weather_rt)
                           : gpu_compute_pipeline(vk, layout, s_weather, sizeof s_weather);
    if (!W.pipe) return false;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkClearColorValue clear = { { 0, 0, 0, 1 } }; // no clouds, nothing in front: transmittance 1
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (int i = 0; i < 7; i++) {
        if (i == 4) continue; // (the shape noise: make_noise)
        gpu_transition(cmd, &W.img[i], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        vkCmdClearColorImage(cmd, W.img[i].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &r);
    }
    gpu_end_once(vk, cmd);
    if (!make_noise(vk)) return false;
    printf("[RealEngine] Clouds: noise made in %.0f ms\n", W.noise_ms);
    return true;
}

void weather_shutdown(VkCtx* vk) {
    if (W.pipe) vkDestroyPipeline(vk->device, W.pipe, NULL);
    for (int i = 0; i < 7; i++) gpu_image_destroy(vk, &W.img[i]);
    gpu_image_destroy(vk, &W.detail);
    gpu_image_destroy(vk, &W.wmap);
    gpu_buf_destroy(vk, &W.tables);
    gpu_buf_destroy(vk, &W.sky);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &W.fogvol[i]);
    memset(&W, 0, sizeof W);
}

// (2026-11-17, Q3: "o Lighting Level tem que levar tudo o que ilumina") the height fog and the cloud layer are the mix
// of every one shown, each at its Lighting Level's weight (lights_level_weight: 1 outside one, 0 when its level is off):
// Day's and Night's sky presets in two Lighting Levels follow Set Lighting Level Blend like their lights. The fog's media
// add (densities summed), the rest is averaged by each one's share; a hidden level's (its eye) is left out. One fog or
// one cloud at weight 1: as before.
static bool weather_skip(World* w, Entity e, uint32_t flags, float* wt) {
    if (flags & (TRANSFORM_LEVEL_HIDDEN | TRANSFORM_HIDDEN_GAME)) return true;
    *wt = lights_level_weight(w, e);
    return *wt <= 0.0f;
}
void weather_fill_frame(GpuFrame* fr, World* w, bool volumetric) {
    W.have_fog = W.have_clouds = false;
    fr->weather_flags = 0;
    double fd = 0, fz = 0, fo = 0, fa[3] = { 0 }, fe[3] = { 0 }, fan = 0, ffall = 0, fstart = 0, fvd = 0, fsh = 0;
    double cw = 0, cov = 0, cden = 0, cbot = 0, ctop = 0, cwind[2] = { 0 }, cdet = 0, cshad = 0, calb[3] = { 0 }, ccir = 0;
    uint32_t nfog = 0;
    bool fvol = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !(a->mask & (COMP_BIT(COMP_FOG) | COMP_BIT(COMP_CLOUD)))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        const CFog* g = (a->mask & COMP_BIT(COMP_FOG)) ? ecs_arch_column(a, COMP_FOG) : NULL;
        const CCloud* c = (a->mask & COMP_BIT(COMP_CLOUD)) ? ecs_arch_column(a, COMP_CLOUD) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            float wt = 1.0f;
            if (weather_skip(w, ents[i], tr[i].flags, &wt)) continue;
            if (g && g[i].density > 0.0f) {
                double d = (double)g[i].density * wt, s = d; // (each one's share: its density at its weight)
                fd += d, fz += s * tr[i].location[2], fo += s * (g[i].max_opacity > 0.0f ? fminf(g[i].max_opacity, 1.0f) : 1.0f);
                for (int k = 0; k < 3; k++) fa[k] += s * g[i].albedo[k], fe[k] += (double)wt * g[i].emissive[k];
                fan += s * fmaxf(fminf(g[i].anisotropy, 0.95f), -0.95f), ffall += s * fmaxf(g[i].height_falloff, 0.0f);
                fstart += s * fmaxf(g[i].start_distance, 0.0f), fvd += s * (g[i].volumetric_distance > 1000.0f ? g[i].volumetric_distance : 6000.0f);
                fsh += s, nfog++;
                fvol |= g[i].volumetric != 0;
            }
            if (c && c[i].coverage > 0.0f && c[i].top > c[i].bottom) {
                cw += wt, cov += (double)wt * fminf(c[i].coverage, 1.0f), cden += (double)wt * c[i].density;
                cbot += (double)wt * c[i].bottom, ctop += (double)wt * c[i].top;
                cwind[0] += (double)wt * c[i].wind[0], cwind[1] += (double)wt * c[i].wind[1];
                cdet += (double)wt * fminf(fmaxf(c[i].detail, 0.0f), 1.0f), cshad += (double)wt * fminf(fmaxf(c[i].shadow_strength, 0.0f), 1.0f);
                for (int k = 0; k < 3; k++) calb[k] += (double)wt * c[i].albedo[k];
                ccir += (double)wt * fminf(fmaxf(c[i].cirrus, 0.0f), 1.0f);
            }
        }
    }
    if (fsh > 0.0) {
        double inv = 1.0 / fsh;
        W.have_fog = true;
        fr->fog0[0] = (float)(fd * 0.01); // 1/m -> 1/cm
        fr->fog0[1] = (float)(ffall * inv * 0.01);
        fr->fog0[2] = (float)(fz * inv);
        fr->fog0[3] = (float)(fo * inv);
        double ew = nfog > 1 ? 1.0 / (double)nfog : 1.0; // (their own light: averaged when several)
        for (int k = 0; k < 3; k++) fr->fog1[k] = (float)(fa[k] * inv), fr->fog2[k] = (float)(fe[k] * ew);
        fr->fog1[3] = (float)(fan * inv);
        fr->fog2[3] = (float)(fstart * inv);
        fr->fog3[0] = (float)(fvd * inv);
        W.volume = volumetric && fvol;
    }
    if (cw > 0.0) {
        // the layer's shape averaged; how much of it: its weight (two at 1: averaged; one fading out: thinner)
        double inv = 1.0 / cw, amt = cw < 1.0 ? cw : 1.0;
        W.have_clouds = true;
        static int s_off = -1; // (REALENGINE_NO_CLOUDS=1: none, the comparison)
        if (s_off < 0) s_off = getenv("REALENGINE_NO_CLOUDS") != NULL;
        if (s_off) W.have_clouds = false;
        float t = fr->camera_pos[3]; // seconds: the wind's offset (no state: the same at any frame rate)
        fr->cloud0[0] = (float)(cov * inv * amt);
        if (wfield_on()) { // (Q27.6) the regional weather: overcast under a storm, the layer's own coverage elsewhere
            WeatherSample s;
            wfield_sample(fr->camera_pos[0], fr->camera_pos[1], &s);
            fr->cloud0[0] = fmaxf(fr->cloud0[0], fminf(fr->cloud0[0] + 0.65f * s.clouds, 0.95f));
        }
        fr->cloud0[1] = (float)(cden * inv); // (1/m: the extinction at density 1, the Tlender's 0.045 / m; weather.comp works in m)
        fr->cloud0[2] = (float)(cbot * inv);
        fr->cloud0[3] = (float)(ctop * inv);
        fr->cloud1[0] = fmodf((float)(cwind[0] * inv) * 100.0f * t, 1.2e6f * 64.0f);
        fr->cloud1[1] = fmodf((float)(cwind[1] * inv) * 100.0f * t, 1.2e6f * 64.0f);
        fr->cloud1[2] = (float)(cdet * inv);
        fr->cloud1[3] = (float)(cshad * inv * amt);
        for (int k = 0; k < 3; k++) fr->cloud2[k] = (float)(calb[k] * inv);
        fr->cloud2[3] = (float)(ccir * inv * amt); // (the cirrus: sky.frag)
        fr->cloud_shadow_origin[0] = fr->camera_pos[0];
        fr->cloud_shadow_origin[1] = fr->camera_pos[1];
        fr->cloud_shadow_origin[2] = SHADOW_SPAN;
    }
    // (2026-11-15, "realista em tempestades") the rain's haze: the drops in the air thicken the height fog by the rain's
    // rate (water.c water_rain_mist); with no fog actor, a fog of the rain alone over the ground near the camera
    {
        float mist = water_rain_mist() * 0.01f; // (1/m -> 1/cm)
        if (mist > 0.0f) {
            if (!W.have_fog) {
                W.have_fog = true;
                fr->fog0[0] = 0.0f, fr->fog0[1] = 0.0004f, fr->fog0[2] = fr->camera_pos[2] - 300.0f, fr->fog0[3] = 1.0f; // (thins out 25 m up)
                for (int k = 0; k < 3; k++) fr->fog1[k] = 0.85f, fr->fog2[k] = 0.0f;
                fr->fog1[3] = 0.55f; // (the drops scatter forward: the haze glows toward the sun and the lamps)
                fr->fog2[3] = 0.0f, fr->fog3[0] = 6000.0f;
                W.volume = false;
            }
            fr->fog0[0] += mist * expf(-fr->fog0[1] * (fr->camera_pos[2] - 300.0f - fr->fog0[2])); // (as dense at the ground under the camera)
        }
    }
    // (A9.5b) the Local Fog Volumes: into this frame's buffer; with no height fog the froxels run for them alone (the
    // analytic fog then adds nothing: a density of 0)
    fr->fog_volumes = 0, fr->fog_volume_count = 0;
    if (volumetric && !getenv("REALENGINE_NO_FOG_VOLUMES")) {
        W.fogvol_slot = (W.fogvol_slot + 1u) % RE_FRAMES_IN_FLIGHT;
        GpuBuf* fb = &W.fogvol[W.fogvol_slot];
        uint32_t n = 0;
        for (uint32_t ai = 0; ai < w->archetype_count && n < FOGVOL_MAX; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!a->count || !(a->mask & COMP_BIT(COMP_FOG_VOLUME)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            if (!fb->buffer && !gpu_buf_create(g_wvk, fb, 16u * 6u * FOGVOL_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) break;
            const CFogVolume* fv = ecs_arch_column(a, COMP_FOG_VOLUME);
            const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t r = 0; r < a->count && n < FOGVOL_MAX; r++) {
                float lw = 1.0f;
                if (fv[r].density <= 0.0f || weather_skip(w, ecs_arch_entities(a)[r], tr[r].flags, &lw)) continue;
                CTransform t = tr[r];
                for (int k = 0; k < 3; k++) t.scale[k] *= 50.0f; // (its unit space: [-1, 1] over 100 cm a unit of scale)
                float rows[3][4], inv[3][4];
                render_transform_rows(&t, rows);
                float a0 = rows[0][0], b0 = rows[0][1], c0 = rows[0][2], d0 = rows[1][0], e0 = rows[1][1], f0 = rows[1][2], g0 = rows[2][0], h0 = rows[2][1], k0 = rows[2][2];
                float A = e0 * k0 - f0 * h0, B = -(d0 * k0 - f0 * g0), C = d0 * h0 - e0 * g0, det = a0 * A + b0 * B + c0 * C;
                if (fabsf(det) < 1e-12f) continue;
                float id = 1.0f / det;
                float m[3][3] = { { A * id, -(b0 * k0 - c0 * h0) * id, (b0 * f0 - c0 * e0) * id },
                                  { B * id, (a0 * k0 - c0 * g0) * id, -(a0 * f0 - c0 * d0) * id },
                                  { C * id, -(a0 * h0 - b0 * g0) * id, (a0 * e0 - b0 * d0) * id } };
                for (int i = 0; i < 3; i++) {
                    for (int j = 0; j < 3; j++) inv[i][j] = m[i][j];
                    inv[i][3] = -(m[i][0] * rows[0][3] + m[i][1] * rows[1][3] + m[i][2] * rows[2][3]);
                }
                float* o = (float*)fb->mapped + 24u * n;
                memcpy(o, inv, 48);
                o[12] = (float)fv[r].shape, o[13] = fv[r].density * lw * 0.01f, o[14] = fminf(fmaxf(fv[r].edge_falloff, 0.01f), 1.0f);
                o[15] = fmaxf(fv[r].height_falloff, 0.0f);
                memcpy(o + 16, fv[r].albedo, 12), o[19] = 0.0f;
                memcpy(o + 20, fv[r].emissive, 12), o[23] = 0.0f;
                n++;
            }
        }
        if (n) {
            fr->fog_volumes = fb->addr, fr->fog_volume_count = n;
            if (!W.have_fog) { // (the froxels for the volumes alone: no height fog)
                W.have_fog = W.volume = true;
                fr->fog0[0] = 1e-12f, fr->fog0[1] = 0.0f, fr->fog0[2] = 0.0f, fr->fog0[3] = 1.0f;
                for (int k = 0; k < 3; k++) fr->fog1[k] = 1.0f, fr->fog2[k] = 0.0f;
                fr->fog1[3] = 0.2f, fr->fog2[3] = 0.0f, fr->fog3[0] = 6000.0f;
            }
            W.volume = true;
        }
    }
    if (W.have_fog) fr->weather_flags |= 1u | (W.volume ? 2u : 0u);
    if (W.have_clouds) {
        fr->weather_flags |= 4u;
        fr->weather_sky = W.sky.addr;
        fr->cloud_tables = W.tables.addr;
        fr->sky = W.sky.addr; // the sky light sees the clouds (weather.comp pass 5 writes it before anything reads it)
    }
}

static void dispatch(VkCommandBuffer cmd, Push p, uint32_t pass, uint32_t pad, uint32_t gx, uint32_t gy) {
    p.pass = pass;
    p.pad = pad;
    vkCmdPushConstants(cmd, W.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof p, &p);
    vkCmdDispatch(cmd, gx, gy, 1);
}

void weather_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr) {
    if (!W.have_clouds && !(W.have_fog && W.volume)) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    { VkDescriptorSet sets_[2] = { set, mat_set() }; // (Q4: the Light Functions' materials, set 1)
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.layout, 0, 2, sets_, 0, NULL); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.pipe);
    // last frame's shading read what this writes
    gpu_barrier(cmd, FS | CS, RW, CS, RW);
    if (W.have_clouds) {
        gpu_mark_detail(cmd, "nuvens (luz)");
        dispatch(cmd, *push, 7, 0, 1, 1); // the clouds' light, once (what every march reads)
        gpu_barrier(cmd, CS, RW, CS, RW);
        gpu_mark_detail(cmd, "nuvens (panorama, sombra)");
        dispatch(cmd, *push, 1, W.phase++ % PANO_PHASES, PANO_W / 8, (PANO_H / PANO_PHASES + 7) / 8); // a share of the rows
        dispatch(cmd, *push, 2, 0, SHADOW_SIDE / 8, SHADOW_SIDE / 8);
        if (fr->cloud_scr[3] > 0.5f) { // the screen's clouds, then into their history (what the sky reads)
            uint32_t sw = (uint32_t)fr->cloud_scr[0], sh = (uint32_t)fr->cloud_scr[1];
            gpu_mark_detail(cmd, "nuvens (na tela)");
            dispatch(cmd, *push, 6, 0, (sw + 7) / 8, (sh + 7) / 8);
            gpu_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy c = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { sw, sh, 1 } };
            vkCmdCopyImage(cmd, W.img[5].image, VK_IMAGE_LAYOUT_GENERAL, W.img[6].image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS | FS, RW);
        }
        gpu_barrier(cmd, CS, RW, CS, RW);
        gpu_mark_detail(cmd, "nuvens (luz do ceu)");
        dispatch(cmd, *push, 5, 0, 1, 1); // the sky light with them
    }
    gpu_barrier(cmd, CS, RW, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// (2026-11-19, the metro's shafts) the froxels after the sun's virtual shadow maps were drawn this frame: pass 3 reads
// them (jittered taps inside each froxel: smooth shafts, not one binary ray a froxel)
void weather_record_fog(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr) {
    if (!(W.have_fog && W.volume && fr->fog0[0] > 0.0f)) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    { VkDescriptorSet sets_[2] = { set, mat_set() };
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.layout, 0, 2, sets_, 0, NULL); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, W.pipe);
    gpu_barrier(cmd, FS | CS, RW, CS, RW); // (last frame's shading read the froxels this writes)
    gpu_mark_detail(cmd, "neblina (froxels)");
    dispatch(cmd, *push, 3, 0, FOG_X * FOG_Z / 8, FOG_Y / 8);
    gpu_barrier(cmd, CS, RW, CS, RW);
    gpu_mark_detail(cmd, "neblina (integra)");
    dispatch(cmd, *push, 4, 0, FOG_X / 8, FOG_Y / 8);
    gpu_barrier(cmd, CS, RW, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}
