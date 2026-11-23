// The vendors' temporal upscalers (upscale_vendor.h; notas-dev/upscaling.md)
#include "upscale_vendor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upscale.h"
#include "vkctx.h"

#if defined(_WIN32) && !defined(__ANDROID__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define UPS_VENDORS 1
#endif

#ifdef UPS_VENDORS
#ifdef RE_HAVE_DLSS
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#endif
#ifdef RE_HAVE_FSR
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
// (from ffx_api/vk/ffx_api_vk.h, whose inline helpers are C++: the backend's description alone)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};
#endif
#ifdef RE_HAVE_XESS
#include "xess/xess_vk.h"
#endif
#endif

// Each runtime's state: why it can't run (NULL: it can), started, its version
typedef struct Vendor {
    const char* why;
    bool probed, started, failed;
    char version[32];
    uint32_t rw, rh, ow, oh; // the sizes its context was made for
    int quality;             // (XeSS: the quality its context was made with)
} Vendor;
static Vendor g_v[UPS_COUNT];
static VkInstance g_inst;
static VkPhysicalDevice g_gpu;
static VkDevice g_dev;
static uint32_t g_vendor_id;
static bool g_off; // REALENGINE_NO_VENDOR_UPSCALERS=1: none (the measure without their extensions)

#ifdef UPS_VENDORS
// The runtime's DLL beside the executable only (never one found elsewhere on the PATH)
static HMODULE load_beside(const char* name) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return NULL;
    char* slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash + 1 - path) + strlen(name) + 1 > sizeof path) return NULL;
    strcpy(slash + 1, name);
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static bool beside_exists(const char* name) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char* slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash + 1 - path) + strlen(name) + 1 > sizeof path) return false;
    strcpy(slash + 1, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
#endif

static bool add_ext(const char** exts, uint32_t* count, uint32_t cap, const char* e) {
    // (VK_EXT_buffer_device_address, which NGX and XeSS list: core since 1.2 and on in the engine's Vulkan12Features;
    // both together is invalid)
    if (!strcmp(e, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) return true;
    for (uint32_t i = 0; i < *count; i++)
        if (!strcmp(exts[i], e)) return true;
    if (*count >= cap) return false;
    exts[(*count)++] = e;
    return true;
}
static bool device_has(VkPhysicalDevice gpu, const char* e) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties* p = n ? (VkExtensionProperties*)malloc(sizeof *p * n) : NULL; // (once, at the device's creation)
    if (n && !p) return false;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, p);
    bool ok = false;
    for (uint32_t i = 0; i < n && !ok; i++) ok = !strcmp(p[i].extensionName, e);
    free(p);
    return ok;
}
static bool instance_has(const char* e) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties* p = n ? (VkExtensionProperties*)malloc(sizeof *p * n) : NULL;
    if (n && !p) return false;
    vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    bool ok = false;
    for (uint32_t i = 0; i < n && !ok; i++) ok = !strcmp(p[i].extensionName, e);
    free(p);
    return ok;
}

// The jitter and the motion as the three runtimes read them (measured, notas-dev/upscaling.md): the jitter as the engine
// applies it (a point at q drawn at q + jitter, render pixels; the opposite sign: XeSS's stillness 0.69% -> 1.44% of
// flicker), the motion in render pixels pointing back to the previous frame (the engine's: uv, current - previous; the
// opposite sign doubled the floor's grid lines behind a moving camera in all three)
#define UPS_JSIGN 1.0f
#define UPS_MVSIGN -1.0f

// ---------------------------------------------------------------------------------------------------------------------
// XeSS (libxess.dll, loaded at run time)
#if defined(UPS_VENDORS) && defined(RE_HAVE_XESS)
static struct {
    HMODULE dll;
    xess_result_t (*inst_exts)(uint32_t*, const char* const**, uint32_t*);
    xess_result_t (*dev_exts)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**);
    xess_result_t (*dev_features)(VkInstance, VkPhysicalDevice, void**);
    xess_result_t (*create)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t*);
    xess_result_t (*init)(xess_context_handle_t, const xess_vk_init_params_t*);
    xess_result_t (*execute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*);
    xess_result_t (*destroy)(xess_context_handle_t);
    xess_result_t (*optimal)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t, xess_2d_t*, xess_2d_t*, xess_2d_t*);
    xess_result_t (*vel_scale)(xess_context_handle_t, float, float);
    xess_result_t (*version)(xess_version_t*);
    xess_context_handle_t ctx;
    bool dev_ok; // its device extensions all there (and enabled)
} X;
static bool xess_load(void) {
    if (X.dll) return true;
    if (!beside_exists("libxess.dll")) return false;
    X.dll = load_beside("libxess.dll");
    if (!X.dll) return false;
#define XL(f, n) *(FARPROC*)&X.f = GetProcAddress(X.dll, n)
    XL(inst_exts, "xessVKGetRequiredInstanceExtensions"), XL(dev_exts, "xessVKGetRequiredDeviceExtensions");
    XL(dev_features, "xessVKGetRequiredDeviceFeatures"), XL(create, "xessVKCreateContext"), XL(init, "xessVKInit");
    XL(execute, "xessVKExecute"), XL(destroy, "xessDestroyContext"), XL(optimal, "xessGetOptimalInputResolution");
    XL(vel_scale, "xessSetVelocityScale"), XL(version, "xessGetVersion");
#undef XL
    if (!X.inst_exts || !X.dev_exts || !X.dev_features || !X.create || !X.init || !X.execute || !X.destroy || !X.optimal ||
        !X.vel_scale) {
        FreeLibrary(X.dll), memset(&X, 0, sizeof X);
        return false;
    }
    return true;
}
// The quality whose share range holds the render's (XeSS takes any input between its mode's min and max)
static xess_quality_settings_t xess_quality(float share) {
    if (share >= 0.95f) return XESS_QUALITY_SETTING_AA;
    if (share >= 0.72f) return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    if (share >= 0.62f) return XESS_QUALITY_SETTING_QUALITY;
    if (share >= 0.54f) return XESS_QUALITY_SETTING_BALANCED;
    if (share >= 0.42f) return XESS_QUALITY_SETTING_PERFORMANCE;
    return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}
static xess_vk_image_view_info xess_img(const UpsImage* m) {
    xess_vk_image_view_info v = { 0 };
    v.imageView = m->view, v.image = m->image, v.format = m->format, v.width = m->w, v.height = m->h;
    v.subresourceRange = (VkImageSubresourceRange){ m->format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return v;
}
static bool xess_eval(VkCtx* vk, const UpsEval* e) {
    Vendor* v = &g_v[UPS_XESS];
    float share = (float)e->color.w / (float)e->output.w;
    int q = (int)xess_quality(share);
    if (!X.ctx || v->ow != e->output.w || v->oh != e->output.h || v->quality != q) {
        vkDeviceWaitIdle(vk->device); // (its init is blocking and needs the GPU idle)
        if (X.ctx) X.destroy(X.ctx), X.ctx = NULL;
        if (X.create(g_inst, g_gpu, g_dev, &X.ctx) != XESS_RESULT_SUCCESS) return v->failed = true, false;
        xess_vk_init_params_t ip = { 0 };
        ip.outputResolution = (xess_2d_t){ e->output.w, e->output.h };
        ip.qualitySetting = (xess_quality_settings_t)q;
        ip.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH; // (the render's colors are already exposed: exposure 1)
        xess_result_t r = X.init(X.ctx, &ip);
        if (r != XESS_RESULT_SUCCESS) {
            fprintf(stderr, "[RealEngine] XeSS: xessVKInit failed (%d)\n", (int)r);
            X.destroy(X.ctx), X.ctx = NULL;
            return v->failed = true, false;
        }
        float ms = UPS_MVSIGN;
        X.vel_scale(X.ctx, ms * (float)e->color.w, ms * (float)e->color.h); // (uv, current - previous: render px back)
        v->ow = e->output.w, v->oh = e->output.h, v->quality = q;
        xess_2d_t o = { e->output.w, e->output.h }, opt, mn, mx;
        if (X.optimal(X.ctx, &o, (xess_quality_settings_t)q, &opt, &mn, &mx) == XESS_RESULT_SUCCESS)
            printf("[RealEngine] XeSS: %ux%u -> %ux%u (its range %ux%u to %ux%u)\n", e->color.w, e->color.h, o.x, o.y, mn.x, mn.y, mx.x, mx.y);
    }
    xess_vk_execute_params_t ep = { 0 };
    ep.colorTexture = xess_img(&e->color), ep.velocityTexture = xess_img(&e->motion), ep.depthTexture = xess_img(&e->depth);
    ep.outputTexture = xess_img(&e->output);
    ep.jitterOffsetX = UPS_JSIGN * e->jitter_x, ep.jitterOffsetY = UPS_JSIGN * e->jitter_y;
    ep.exposureScale = 1.0f;
    ep.resetHistory = e->reset ? 1u : 0u;
    ep.inputWidth = e->color.w, ep.inputHeight = e->color.h;
    xess_result_t r = X.execute(X.ctx, e->cmd, &ep);
    if (r != XESS_RESULT_SUCCESS) {
        fprintf(stderr, "[RealEngine] XeSS: xessVKExecute failed (%d)\n", (int)r);
        return false;
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// FSR 3.1 (amd_fidelityfx_vk.dll, loaded at run time)
#if defined(UPS_VENDORS) && defined(RE_HAVE_FSR)
static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    ffxContext ctx;
} F;
static bool fsr_load(void) {
    if (F.dll) return true;
    if (!beside_exists("amd_fidelityfx_vk.dll")) return false;
    F.dll = load_beside("amd_fidelityfx_vk.dll");
    if (!F.dll) return false;
    *(FARPROC*)&F.create = GetProcAddress(F.dll, "ffxCreateContext");
    *(FARPROC*)&F.destroy = GetProcAddress(F.dll, "ffxDestroyContext");
    *(FARPROC*)&F.query = GetProcAddress(F.dll, "ffxQuery");
    *(FARPROC*)&F.dispatch = GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.dispatch) {
        FreeLibrary(F.dll), memset(&F, 0, sizeof F);
        return false;
    }
    return true;
}
static void fsr_message(uint32_t type, const wchar_t* msg) {
    fprintf(stderr, "[RealEngine] FSR (%u): %ls\n", type, msg);
}
static struct FfxApiResource fsr_res(const UpsImage* m, bool depth, bool uav) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    r.resource = (void*)m->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = m->format == VK_FORMAT_D32_SFLOAT ? FFX_API_SURFACE_FORMAT_R32_FLOAT
                         : m->format == VK_FORMAT_R16G16_SFLOAT ? FFX_API_SURFACE_FORMAT_R16G16_FLOAT
                                                                : FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    r.description.width = m->w, r.description.height = m->h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}
static bool fsr_eval(VkCtx* vk, const UpsEval* e) {
    Vendor* v = &g_v[UPS_FSR];
    if (!F.ctx || v->ow != e->output.w || v->oh != e->output.h) {
        vkDeviceWaitIdle(vk->device);
        if (F.ctx) F.destroy(&F.ctx, NULL), F.ctx = NULL;
        struct ffxCreateBackendVKDesc be;
        memset(&be, 0, sizeof be);
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.vkDevice = g_dev, be.vkPhysicalDevice = g_gpu, be.vkDeviceProcAddr = vkGetDeviceProcAddr;
        struct ffxCreateContextDescUpscale cd;
        memset(&cd, 0, sizeof cd);
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, cd.header.pNext = &be.header;
        // (the render at any share up to the output: the dynamic resolution and the modes without a new context)
        cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                   FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION; // (the render's colors are already exposed: exposure 1)
        if (getenv("REALENGINE_UPS_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING; // (its messages in the log)
        cd.maxRenderSize = (struct FfxApiDimensions2D){ e->output.w, e->output.h };
        cd.maxUpscaleSize = (struct FfxApiDimensions2D){ e->output.w, e->output.h };
        cd.fpMessage = (ffxApiMessage)fsr_message;
        ffxReturnCode_t r = F.create(&F.ctx, &cd.header, NULL);
        if (r != FFX_API_RETURN_OK) {
            fprintf(stderr, "[RealEngine] FSR: ffxCreateContext failed (%u)\n", r);
            F.ctx = NULL;
            return v->failed = true, false;
        }
        v->ow = e->output.w, v->oh = e->output.h;
    }
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void*)e->cmd;
    d.color = fsr_res(&e->color, false, false), d.depth = fsr_res(&e->depth, true, false), d.motionVectors = fsr_res(&e->motion, false, false);
    d.output = fsr_res(&e->output, false, true);
    d.jitterOffset = (struct FfxApiFloatCoords2D){ UPS_JSIGN * e->jitter_x, UPS_JSIGN * e->jitter_y };
    d.motionVectorScale = (struct FfxApiFloatCoords2D){ UPS_MVSIGN * (float)e->color.w, UPS_MVSIGN * (float)e->color.h };
    d.renderSize = (struct FfxApiDimensions2D){ e->color.w, e->color.h };
    d.upscaleSize = (struct FfxApiDimensions2D){ e->output.w, e->output.h };
    d.frameTimeDelta = e->frame_ms > 0.0f ? e->frame_ms : 16.7f;
    d.preExposure = e->pre_exposure > 0.0f ? e->pre_exposure : 1.0f;
    d.reset = e->reset;
    // (reversed infinite Z as FSR names it: the near plane at FLT_MAX, the far one at the camera's near)
    d.cameraNear = 3.402823466e38f, d.cameraFar = e->near_cm, d.cameraFovAngleVertical = e->fov_y;
    d.viewSpaceToMetersFactor = 0.01f; // (centimeters)
    ffxReturnCode_t r = F.dispatch(&F.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        fprintf(stderr, "[RealEngine] FSR: ffxDispatch failed (%u)\n", r);
        return false;
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// DLSS (NGX: its static library; nvngx_dlss.dll beside the executable)
#if defined(UPS_VENDORS) && defined(RE_HAVE_DLSS)
static struct {
    bool init;
    NVSDK_NGX_Parameter* params;
    NVSDK_NGX_Handle* feature;
} D;
// (a project id for the engine's own build: NVIDIA's for a custom engine is any GUID)
#define RE_NGX_PROJECT_ID "6b3f5a62-0c1e-4b8e-9d2a-7a41c0e5f3b9"
static bool dlss_start(void) {
    Vendor* v = &g_v[UPS_DLSS];
    if (D.init) return D.params != NULL;
    D.init = true;
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) wcscpy(tmp, L".");
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(RE_NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", tmp, g_inst, g_gpu, g_dev,
                                                             vkGetInstanceProcAddr, vkGetDeviceProcAddr, NULL, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        v->why = "NGX did not start (the driver)";
        fprintf(stderr, "[RealEngine] DLSS: NGX init failed (0x%08x)\n", (unsigned)r);
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&D.params)) || !D.params) {
        v->why = "NGX gave no capabilities";
        D.params = NULL;
        return false;
    }
    int avail = 0, upd = 0;
    NVSDK_NGX_Parameter_GetI(D.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &upd);
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(D.params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail)) || !avail) {
        v->why = upd ? "the NVIDIA driver is too old for DLSS" : "this GPU has no DLSS (an RTX is needed)";
        NVSDK_NGX_VULKAN_DestroyParameters(D.params), D.params = NULL;
        return false;
    }
    snprintf(v->version, sizeof v->version, "310.9.16"); // (the SDK's DLL tools/fetch_upscalers.ps1 puts beside the executable)
    return true;
}
static NVSDK_NGX_PerfQuality_Value dlss_quality(float share) {
    if (share >= 0.95f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (share >= 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (share >= 0.54f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (share >= 0.42f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}
static NVSDK_NGX_Resource_VK dlss_img(const UpsImage* m, bool rw) {
    VkImageSubresourceRange sr = { m->format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return NVSDK_NGX_Create_ImageView_Resource_VK(m->view, m->image, sr, m->format, m->w, m->h, rw);
}
static bool dlss_eval(VkCtx* vk, const UpsEval* e) {
    Vendor* v = &g_v[UPS_DLSS];
    if (!dlss_start()) return false;
    // (2026-11-14, "o DLSS tá bem bugado: flickando", with the dynamic resolution) made for the largest render seen and
    // the output; a smaller render goes as the subrect (DLSS's own dynamic resolution). It was made again at every step
    // of the dynamic resolution: the GPU stopped and DLSS lost its history, a flash of aliasing and noise each time
    if (!D.feature || e->color.w > v->rw || e->color.h > v->rh || v->ow != e->output.w || v->oh != e->output.h) {
        if (D.feature) {
            vkDeviceWaitIdle(vk->device);
            NVSDK_NGX_VULKAN_ReleaseFeature(D.feature), D.feature = NULL;
        }
        NVSDK_NGX_DLSS_Create_Params cp;
        memset(&cp, 0, sizeof cp);
        uint32_t mw = e->color.w, mh = e->color.h;
        if (v->ow == e->output.w && v->oh == e->output.h && D.feature == NULL && v->rw) mw = mw > v->rw ? mw : v->rw, mh = mh > v->rh ? mh : v->rh;
        cp.Feature.InWidth = mw, cp.Feature.InHeight = mh;
        cp.Feature.InTargetWidth = e->output.w, cp.Feature.InTargetHeight = e->output.h;
        cp.Feature.InPerfQualityValue = dlss_quality((float)mw / (float)e->output.w);
        // (the render's colors are already exposed by the engine's own auto exposure: DLSS's on top of it fought it, the
        // image pulsing and darker; exposure 1)
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        { // the transformer's preset K on every mode (the cheapest: on the RTX 3060 at 2156x1330, 2.2 ms in any mode,
          // against the default M of Performance's 4.1 ms and L's 5.1 to 7.8 ms; notas-dev/upscaling.md)
            const unsigned int k = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
            NVSDK_NGX_Parameter_SetUI(D.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, k);
            NVSDK_NGX_Parameter_SetUI(D.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, k);
            NVSDK_NGX_Parameter_SetUI(D.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, k);
            NVSDK_NGX_Parameter_SetUI(D.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, k);
            NVSDK_NGX_Parameter_SetUI(D.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, k);
        }
        NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, e->cmd, 1, 1, &D.feature, D.params, &cp);
        if (NVSDK_NGX_FAILED(r) || !D.feature) {
            fprintf(stderr, "[RealEngine] DLSS: the feature was not created (0x%08x)\n", (unsigned)r);
            D.feature = NULL;
            return v->failed = true, false;
        }
        v->rw = mw, v->rh = mh, v->ow = e->output.w, v->oh = e->output.h;
    }
    NVSDK_NGX_Resource_VK col = dlss_img(&e->color, false), dep = dlss_img(&e->depth, false), mv = dlss_img(&e->motion, false);
    NVSDK_NGX_Resource_VK out = dlss_img(&e->output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep;
    memset(&ep, 0, sizeof ep);
    ep.Feature.pInColor = &col, ep.Feature.pInOutput = &out;
    ep.pInDepth = &dep, ep.pInMotionVectors = &mv;
    ep.InJitterOffsetX = UPS_JSIGN * e->jitter_x, ep.InJitterOffsetY = UPS_JSIGN * e->jitter_y;
    ep.InRenderSubrectDimensions = (NVSDK_NGX_Dimensions){ e->color.w, e->color.h };
    ep.InReset = e->reset ? 1 : 0;
    ep.InMVScaleX = UPS_MVSIGN * (float)e->color.w, ep.InMVScaleY = UPS_MVSIGN * (float)e->color.h;
    ep.InFrameTimeDeltaInMsec = e->frame_ms;
    ep.InPreExposure = e->pre_exposure > 0.0f ? e->pre_exposure : 1.0f;
    // (2026-11-14, "a imagem fica diferente, flickando, muito ghosting") DLSS divides the color by the pre-exposure and then
    // tone maps it internally by its exposure (1 without a texture): the engine's physical units (the sun in lux) came
    // out tens of thousands of times over its range, its history and its rejection worked on a white-clipped image. The
    // same scale as its exposure: what it sees internally is the exposed image the engine shows
    ep.InExposureScale = ep.InPreExposure;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(e->cmd, D.feature, D.params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        fprintf(stderr, "[RealEngine] DLSS: evaluate failed (0x%08x)\n", (unsigned)r);
        return false;
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
void ups_vendor_instance_exts(const char** exts, uint32_t* count, uint32_t cap) {
    g_off = getenv("REALENGINE_NO_VENDOR_UPSCALERS") != NULL;
    if (g_off) return;
#if defined(UPS_VENDORS) && defined(RE_HAVE_DLSS)
    if (beside_exists("nvngx_dlss.dll")) {
        unsigned int ni = 0, nd = 0;
        const char **ie = NULL, **de = NULL;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de)))
            for (unsigned int i = 0; i < ni; i++)
                if (instance_has(ie[i])) add_ext(exts, count, cap, ie[i]);
    }
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_XESS)
    if (xess_load()) {
        uint32_t n = 0, ver = 0;
        const char* const* e = NULL;
        if (X.inst_exts(&n, &e, &ver) == XESS_RESULT_SUCCESS)
            for (uint32_t i = 0; i < n; i++)
                if (instance_has(e[i])) add_ext(exts, count, cap, e[i]);
    }
#endif
    (void)exts, (void)count, (void)cap;
}

void ups_vendor_device_exts(VkInstance inst, VkPhysicalDevice gpu, const char** exts, uint32_t* count, uint32_t cap, void** features) {
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(gpu, &pr);
    g_vendor_id = pr.vendorID;
    (void)inst, (void)exts, (void)count, (void)cap, (void)features;
    if (g_off) return;
#if defined(UPS_VENDORS) && defined(RE_HAVE_DLSS)
    if (pr.vendorID == 0x10DE && beside_exists("nvngx_dlss.dll")) { // (NVIDIA only: elsewhere its extensions don't exist)
        unsigned int ni = 0, nd = 0;
        const char **ie = NULL, **de = NULL;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de))) {
            bool all = true;
            for (unsigned int i = 0; i < nd; i++) all &= device_has(gpu, de[i]);
            if (all)
                for (unsigned int i = 0; i < nd; i++) all &= add_ext(exts, count, cap, de[i]);
            if (!all) g_v[UPS_DLSS].why = "the GPU lacks a Vulkan extension DLSS needs";
        }
    }
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_FSR)
    // FSR's Vulkan backend calls vkGetBufferMemoryRequirements2KHR whenever the GPU lists VK_KHR_dedicated_allocation (its
    // breadcrumbs' buffer, at the context's creation): the KHR names resolve only with the (core since 1.1) extensions on
    if (beside_exists("amd_fidelityfx_vk.dll")) {
        if (device_has(gpu, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
            add_ext(exts, count, cap, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        if (device_has(gpu, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)) add_ext(exts, count, cap, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        // its shaders' half floats and 16-bit integers (the fast path it picks when the GPU has them): on in the chain
        VkPhysicalDeviceVulkan12Features h12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 h2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h12 };
        vkGetPhysicalDeviceFeatures2(gpu, &h2);
        for (VkBaseOutStructure* b = (VkBaseOutStructure*)*features; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) ((VkPhysicalDeviceFeatures2*)b)->features.shaderInt16 |= h2.features.shaderInt16;
            else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) ((VkPhysicalDeviceVulkan12Features*)b)->shaderFloat16 |= h12.shaderFloat16;
    }
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_XESS)
    if (X.dll) {
        uint32_t n = 0;
        const char* const* e = NULL;
        bool all = X.dev_exts(inst, gpu, &n, &e) == XESS_RESULT_SUCCESS;
        for (uint32_t i = 0; all && i < n; i++) all &= device_has(gpu, e[i]);
        for (uint32_t i = 0; all && i < n; i++) all &= add_ext(exts, count, cap, e[i]);
        if (all) all = X.dev_features(inst, gpu, features) == XESS_RESULT_SUCCESS; // (patches the chain: what it needs on)
        X.dev_ok = all;
        if (!all) g_v[UPS_XESS].why = "the GPU lacks a Vulkan extension XeSS needs";
    }
#endif
}

void ups_vendor_device_ready(VkCtx* vk) {
    g_inst = vk->instance, g_gpu = vk->gpu, g_dev = vk->device;
}

void ups_vendor_shutdown(VkCtx* vk) {
    (void)vk;
    if (!g_dev) return;
    vkDeviceWaitIdle(g_dev);
#if defined(UPS_VENDORS) && defined(RE_HAVE_DLSS)
    if (D.feature) NVSDK_NGX_VULKAN_ReleaseFeature(D.feature), D.feature = NULL;
    if (D.params) NVSDK_NGX_VULKAN_DestroyParameters(D.params), D.params = NULL;
    if (D.init) NVSDK_NGX_VULKAN_Shutdown1(g_dev);
    D.init = false;
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_FSR)
    if (F.ctx) F.destroy(&F.ctx, NULL), F.ctx = NULL;
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_XESS)
    if (X.ctx) X.destroy(X.ctx), X.ctx = NULL;
#endif
    for (int k = 0; k < UPS_COUNT; k++) g_v[k].started = false, g_v[k].failed = false, g_v[k].ow = g_v[k].oh = g_v[k].rw = g_v[k].rh = 0;
    g_dev = VK_NULL_HANDLE;
}

bool ups_vendor_usable(int k, const char** why) {
    const char* w = NULL;
    bool ok = false;
    if (k <= UPS_TSR || k >= UPS_COUNT) w = "unknown";
    else if (g_off) w = "turned off (REALENGINE_NO_VENDOR_UPSCALERS)";
    else if (g_v[k].failed) w = "its runtime failed on this GPU (Output Log)";
    else {
#ifdef UPS_VENDORS
        switch (k) {
        case UPS_DLSS:
#ifdef RE_HAVE_DLSS
            if (!beside_exists("nvngx_dlss.dll")) w = "its runtime is not installed (nvngx_dlss.dll)";
            else if (g_vendor_id && g_vendor_id != 0x10DE) w = "an NVIDIA RTX GPU is needed";
            else if (g_v[k].why) w = g_v[k].why;
            else if (!g_dev) w = "the GPU has not started yet";
            else if (!dlss_start()) w = g_v[k].why ? g_v[k].why : "NGX did not start";
            else ok = true;
#else
            w = "not in this build (the SDK: tools/fetch_upscalers.ps1)";
#endif
            break;
        case UPS_FSR:
#ifdef RE_HAVE_FSR
            if (!fsr_load()) w = "its runtime is not installed (amd_fidelityfx_vk.dll)";
            else ok = true, snprintf(g_v[k].version, sizeof g_v[k].version, "3.1");
#else
            w = "not in this build (the SDK: tools/fetch_upscalers.ps1)";
#endif
            break;
        case UPS_XESS:
#ifdef RE_HAVE_XESS
            if (!X.dll && !xess_load()) w = "its runtime is not installed (libxess.dll)";
            else if (!X.dev_ok) w = g_v[k].why ? g_v[k].why : "its Vulkan extensions were not enabled";
            else {
                ok = true;
                xess_version_t ver;
                if (X.version && X.version(&ver) == XESS_RESULT_SUCCESS)
                    snprintf(g_v[k].version, sizeof g_v[k].version, "%u.%u.%u", ver.major, ver.minor, ver.patch);
            }
#else
            w = "not in this build (the SDK: tools/fetch_upscalers.ps1)";
#endif
            break;
        }
#else
        w = "Windows only";
#endif
    }
    if (why) *why = w;
    return ok;
}

const char* ups_vendor_version(int k) { return k > UPS_TSR && k < UPS_COUNT ? g_v[k].version : ""; }

bool ups_vendor_eval(VkCtx* vk, int k, const UpsEval* in) {
    if (!ups_vendor_usable(k, NULL)) return false;
    // the time since the last frame (DLSS and FSR weigh the motion by it: 0 read as standing still for a frame of no
    // length, DLSS dropped its history every frame)
    UpsEval ev = *in;
    const UpsEval* e = &ev;
#ifdef UPS_VENDORS
    static LARGE_INTEGER last, freq;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    double ms = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart : 16.7;
    last = now;
    if (ev.frame_ms <= 0.0f) ev.frame_ms = (float)(ms > 0.05 && ms < 250.0 ? ms : 16.7);
#endif
    bool ok = false;
    switch (k) {
#if defined(UPS_VENDORS) && defined(RE_HAVE_DLSS)
    case UPS_DLSS: ok = dlss_eval(vk, e); break;
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_FSR)
    case UPS_FSR: ok = fsr_eval(vk, e); break;
#endif
#if defined(UPS_VENDORS) && defined(RE_HAVE_XESS)
    case UPS_XESS: ok = xess_eval(vk, e); break;
#endif
    default: (void)vk, (void)e; break;
    }
    if (ok && !g_v[k].started) {
        g_v[k].started = true;
        printf("[RealEngine] Upscaler: %s %s active, %ux%u -> %ux%u\n", upscale_kind_name(k), g_v[k].version, e->color.w, e->color.h, e->output.w,
               e->output.h);
    }
    return ok;
}
