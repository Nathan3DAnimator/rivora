#include "atmosphere.h"

#include <math.h>
#include <string.h>

#include "vkctx.h"

static const uint32_t s_atmosphere[] =
#include "atmosphere.comp.inc"
    ;

#define TRANSMITTANCE_W 256
#define TRANSMITTANCE_H 64
#define MULTISCATTER_SIZE 32
#define SKYVIEW_W 192
#define SKYVIEW_H 108
#define AP_RES 32 // shaders/atmosphere.glsl: AP_RES x AP_RES froxels, AP_RES slices side by side

static struct {
    VkPipeline pipe;
    VkPipelineLayout layout;
    GpuImage transmittance, multiscatter, skyview, aerial;
    GpuBuf sky;
    bool luts_ready, sky_ready;
    float last_sun[3], last_alt_km;
} A;

VkImageView atmosphere_view(int i) {
    return i == 0 ? A.transmittance.view : i == 1 ? A.skyview.view : i == 2 ? A.multiscatter.view : A.aerial.view;
}
VkDeviceAddress atmosphere_sky_address(void) { return A.sky.addr; }

static bool make(VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h) {
    if (!gpu_image_create(vk, im, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    VkCommandBuffer cmd = gpu_begin_once(vk); // GENERAL from the start: written by compute, sampled by everything
    gpu_transition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true);
    gpu_end_once(vk, cmd);
    return true;
}

bool atmosphere_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&A, 0, sizeof A);
    A.layout = layout;
    A.pipe = gpu_compute_pipeline(vk, layout, s_atmosphere, sizeof s_atmosphere);
    return A.pipe && make(vk, &A.transmittance, TRANSMITTANCE_W, TRANSMITTANCE_H) &&
           make(vk, &A.multiscatter, MULTISCATTER_SIZE, MULTISCATTER_SIZE) && make(vk, &A.skyview, SKYVIEW_W, SKYVIEW_H) &&
           make(vk, &A.aerial, AP_RES * AP_RES, AP_RES) &&
           gpu_buf_create(vk, &A.sky, 9 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
}

void atmosphere_shutdown(VkCtx* vk) {
    if (A.pipe) vkDestroyPipeline(vk->device, A.pipe, NULL);
    gpu_image_destroy(vk, &A.transmittance);
    gpu_image_destroy(vk, &A.multiscatter);
    gpu_image_destroy(vk, &A.skyview);
    gpu_image_destroy(vk, &A.aerial);
    gpu_buf_destroy(vk, &A.sky);
    memset(&A, 0, sizeof A);
}

static void pass(VkCommandBuffer cmd, Push p, uint32_t n, uint32_t gx, uint32_t gy) {
    p.pass = n;
    vkCmdPushConstants(cmd, A.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof p, &p);
    vkCmdDispatch(cmd, gx, gy, 1);
    // the next pass (and every shader after) samples what this one wrote
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

bool atmosphere_update(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const float sun[3], float camera_z_cm,
                       bool aerial) {
    float alt = fminf(fmaxf(camera_z_cm * 1e-5f, 0.001f), 80.0f);
    bool sun_moved = fabsf(sun[0] - A.last_sun[0]) + fabsf(sun[1] - A.last_sun[1]) + fabsf(sun[2] - A.last_sun[2]) > 1e-4f;
    bool rose = fabsf(alt - A.last_alt_km) > fmaxf(0.05f, A.last_alt_km * 0.1f); // 50 m near the ground, 10% higher up
    bool sky = !A.luts_ready || !A.sky_ready || sun_moved || rose;
    if (!sky && !aerial) return false;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, A.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, A.pipe);
    // the previous frame's shaders are done sampling the LUTs before they change
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0);
    if (!A.luts_ready) {
        pass(cmd, *push, 0, (TRANSMITTANCE_W + 7) / 8, (TRANSMITTANCE_H + 7) / 8);
        pass(cmd, *push, 1, MULTISCATTER_SIZE / 8, MULTISCATTER_SIZE / 8);
        A.luts_ready = true;
    }
    if (sky) {
        pass(cmd, *push, 2, (SKYVIEW_W + 7) / 8, (SKYVIEW_H + 7) / 8);
        pass(cmd, *push, 3, 1, 1);
        memcpy(A.last_sun, sun, sizeof A.last_sun);
        A.last_alt_km = alt;
        A.sky_ready = true;
    }
    if (aerial) pass(cmd, *push, 4, AP_RES / 8, AP_RES / 8); // every frame: it follows the camera
    return true;
}

// --- The same atmosphere on the CPU: the optical depth toward the sun (atmosphere.glsl's constants) ---

#define RG 6360.0f
#define RT 6460.0f

void atmosphere_transmittance(const float dir[3], float camera_z_cm, float out[3]) {
    float r = RG + fminf(fmaxf(camera_z_cm * 1e-5f, 0.001f), 80.0f), mu = dir[2];
    // below the horizon: the planet is in the way
    if (mu < 0.0f && r * r * (mu * mu - 1.0f) + RG * RG >= 0.0f) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    float b = r * mu, c = r * r - RT * RT, t = -b + sqrtf(fmaxf(b * b - c, 0.0f));
    const int N = 64;
    float dt = t / (float)N, depth[3] = { 0, 0, 0 };
    for (int i = 0; i < N; i++) {
        float ti = ((float)i + 0.5f) * dt;
        float h = sqrtf(r * r + ti * ti + 2.0f * r * mu * ti) - RG;
        float dr = expf(-h / 8.0f), dm = expf(-h / 1.2f), dz = fmaxf(0.0f, 1.0f - fabsf(h - 25.0f) / 15.0f);
        static const float ray[3] = { 5.802e-3f, 13.558e-3f, 33.1e-3f }, ozone[3] = { 0.650e-3f, 1.881e-3f, 0.085e-3f };
        for (int k = 0; k < 3; k++) depth[k] += (ray[k] * dr + 4.40e-3f * dm + ozone[k] * dz) * dt;
    }
    for (int k = 0; k < 3; k++) out[k] = expf(-depth[k]);
}
