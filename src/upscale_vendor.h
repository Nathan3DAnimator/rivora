// The vendors' temporal upscalers (upscale_vendor.c; notas-dev/upscaling.md): NVIDIA DLSS Super Resolution (NGX), AMD
// FSR 3.1 (the FidelityFX API's signed Vulkan DLL), Intel XeSS (libxess.dll). Each one's runtime beside the executable
// (tools/fetch_upscalers.ps1 downloads the SDKs into external/upscalers; the build copies the DLLs); a missing one, a GPU
// that can't run it or a call that fails leaves TSR (render.c) doing the frame: never a crash.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct VkCtx;

// Vulkan creation (vkctx.c): the instance's and the device's extensions (and features: XeSS patches the chain) the
// runtimes need, only those the GPU has; before vkCreateInstance / vkCreateDevice
void ups_vendor_instance_exts(const char** exts, uint32_t* count, uint32_t cap);
void ups_vendor_device_exts(VkInstance inst, VkPhysicalDevice gpu, const char** exts, uint32_t* count, uint32_t cap, void** features);
void ups_vendor_device_ready(struct VkCtx* vk); // (the device made: its handles kept for the lazy start)
void ups_vendor_shutdown(struct VkCtx* vk);     // (before the device goes)

// Whether one can run here (UPS_FSR / UPS_DLSS / UPS_XESS), and why not. The first ask starts its runtime (DLSS: NGX's
// capability check)
bool ups_vendor_usable(int kind, const char** why);
void ups_vendor_warm_async(void);
// (2026-11-25) the menus' ask: never waits; *pending while the runtimes are still being asked (the warm, started if none)
bool ups_vendor_usable_nowait(int kind, const char** why, bool* pending); // (2026-11-25) every runtime asked on a thread of its own (the Play, the game's start)
const char* ups_vendor_version(int kind); // "310.9.16"... ("" before it started)

typedef struct UpsImage { VkImage image; VkImageView view; VkFormat format; uint32_t w, h; } UpsImage;
// One frame's upscale: the render's color, depth (D32, reversed infinite Z) and motion (ups_mv.comp: uv, current -
// previous, no jitter) in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, the output (the HDR target) in GENERAL
typedef struct UpsEval {
    VkCommandBuffer cmd;
    UpsImage color, depth, motion, output;
    float jitter_x, jitter_y; // render pixels: a point at q drawn at q + jitter
    bool reset;               // a cut: no history
    float frame_ms, near_cm, fov_y; // (FSR: its depth's linearization; fov_y in radians)
    // the exposure the render's colors were multiplied by (the engine's auto exposure, read back a frame late): DLSS and
    // FSR take it out of their history, which otherwise lagged every exposure change and fought the auto exposure (the
    // image pulsing slowly)
    float pre_exposure;
} UpsEval;
// Records the upscale into e->cmd (recreating the runtime's context when the sizes changed: waits for the GPU). False:
// nothing recorded (the caller runs TSR). The command buffer's bound pipeline, descriptor sets and push constants are
// the runtime's afterwards
bool ups_vendor_eval(struct VkCtx* vk, int kind, const UpsEval* e);
