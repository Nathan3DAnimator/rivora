// VR through OpenXR (xr.h, notas-dev/xr.md): the runtime loaded and negotiated with as the loader does (the Quest: the
// Khronos loader in the APK), no SDK in the build (xr_openxr.h declares what is called). Engine space: X forward, Y right, Z up, cm; OpenXR's: -Z forward,
// X right, Y up, meters.
#include "xr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkctx.h"
#include "xr_internal.h"
#include "xr_openxr.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__ANDROID__)
#include <dlfcn.h>
#include <jni.h>
#include "../android/re_android.h"
#endif

#define XRF(name) static PFN_##name p_##name
typedef XrResult (*PFN_xrCreateInstance)(const XrInstanceCreateInfo*, XrInstance*);
typedef XrResult (*PFN_xrDestroyInstance)(XrInstance);
typedef XrResult (*PFN_xrGetInstanceProperties)(XrInstance, XrInstanceProperties*);
typedef XrResult (*PFN_xrGetSystem)(XrInstance, const XrSystemGetInfo*, XrSystemId*);
typedef XrResult (*PFN_xrGetSystemProperties)(XrInstance, XrSystemId, XrSystemProperties*);
typedef XrResult (*PFN_xrEnumerateViewConfigurationViews)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*);
typedef XrResult (*PFN_xrGetVulkanInstanceExtensionsKHR)(XrInstance, XrSystemId, uint32_t, uint32_t*, char*);
typedef XrResult (*PFN_xrGetVulkanDeviceExtensionsKHR)(XrInstance, XrSystemId, uint32_t, uint32_t*, char*);
typedef XrResult (*PFN_xrGetVulkanGraphicsDeviceKHR)(XrInstance, XrSystemId, VkInstance, VkPhysicalDevice*);
typedef XrResult (*PFN_xrGetVulkanGraphicsRequirementsKHR)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
typedef XrResult (*PFN_xrCreateSession)(XrInstance, const XrSessionCreateInfo*, XrSession*);
typedef XrResult (*PFN_xrDestroySession)(XrSession);
typedef XrResult (*PFN_xrBeginSession)(XrSession, const XrSessionBeginInfo*);
typedef XrResult (*PFN_xrEndSession)(XrSession);
typedef XrResult (*PFN_xrCreateReferenceSpace)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
typedef XrResult (*PFN_xrEnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t*, int64_t*);
typedef XrResult (*PFN_xrCreateSwapchain)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
typedef XrResult (*PFN_xrDestroySwapchain)(XrSwapchain);
typedef XrResult (*PFN_xrEnumerateSwapchainImages)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
typedef XrResult (*PFN_xrAcquireSwapchainImage)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
typedef XrResult (*PFN_xrWaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo*);
typedef XrResult (*PFN_xrReleaseSwapchainImage)(XrSwapchain, const XrSwapchainImageReleaseInfo*);
typedef XrResult (*PFN_xrWaitFrame)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
typedef XrResult (*PFN_xrBeginFrame)(XrSession, const XrFrameBeginInfo*);
typedef XrResult (*PFN_xrEndFrame)(XrSession, const XrFrameEndInfo*);
typedef XrResult (*PFN_xrLocateViews)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
typedef XrResult (*PFN_xrPollEvent)(XrInstance, XrEventDataBuffer*);
typedef XrResult (*PFN_xrStringToPath)(XrInstance, const char*, XrPath*);
typedef XrResult (*PFN_xrCreateActionSet)(XrInstance, const XrActionSetCreateInfo*, XrActionSet*);
typedef XrResult (*PFN_xrCreateAction)(XrActionSet, const XrActionCreateInfo*, XrAction*);
typedef XrResult (*PFN_xrSuggestInteractionProfileBindings)(XrInstance, const XrInteractionProfileSuggestedBinding*);
typedef XrResult (*PFN_xrAttachSessionActionSets)(XrSession, const XrSessionActionSetsAttachInfo*);
typedef XrResult (*PFN_xrCreateActionSpace)(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
typedef XrResult (*PFN_xrSyncActions)(XrSession, const XrActionsSyncInfo*);
typedef XrResult (*PFN_xrGetActionStateFloat)(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
typedef XrResult (*PFN_xrGetActionStateBoolean)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
typedef XrResult (*PFN_xrGetActionStateVector2f)(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
typedef XrResult (*PFN_xrLocateSpace)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
typedef XrResult (*PFN_xrApplyHapticFeedback)(XrSession, const XrHapticActionInfo*, const XrHapticBaseHeader*);
XRF(xrCreateInstance); XRF(xrDestroyInstance); XRF(xrGetInstanceProperties); XRF(xrGetSystem); XRF(xrGetSystemProperties);
XRF(xrEnumerateViewConfigurationViews); XRF(xrGetVulkanInstanceExtensionsKHR); XRF(xrGetVulkanDeviceExtensionsKHR);
XRF(xrGetVulkanGraphicsDeviceKHR); XRF(xrGetVulkanGraphicsRequirementsKHR); XRF(xrCreateSession); XRF(xrDestroySession);
XRF(xrBeginSession); XRF(xrEndSession); XRF(xrCreateReferenceSpace); XRF(xrEnumerateSwapchainFormats); XRF(xrCreateSwapchain);
XRF(xrDestroySwapchain); XRF(xrEnumerateSwapchainImages); XRF(xrAcquireSwapchainImage); XRF(xrWaitSwapchainImage);
XRF(xrReleaseSwapchainImage); XRF(xrWaitFrame); XRF(xrBeginFrame); XRF(xrEndFrame); XRF(xrLocateViews); XRF(xrPollEvent);
XRF(xrStringToPath); XRF(xrCreateActionSet); XRF(xrCreateAction); XRF(xrSuggestInteractionProfileBindings);
XRF(xrAttachSessionActionSets); XRF(xrCreateActionSpace); XRF(xrSyncActions); XRF(xrGetActionStateFloat);
XRF(xrGetActionStateBoolean); XRF(xrGetActionStateVector2f); XRF(xrLocateSpace); XRF(xrApplyHapticFeedback);

XRF(xrEnumerateDisplayRefreshRatesFB); XRF(xrGetDisplayRefreshRateFB); XRF(xrRequestDisplayRefreshRateFB);
XRF(xrCreateHandTrackerEXT); XRF(xrDestroyHandTrackerEXT); XRF(xrLocateHandJointsEXT);

#define MAX_SC_IMAGES 8
enum { A_POSE_GRIP = 0, A_POSE_AIM, A_TRIGGER, A_GRIP, A_STICK, A_PRIMARY, A_SECONDARY, A_MENU, A_STICK_CLICK, A_HAPTIC, A_COUNT };

static struct {
    XrInfo info;
    XrConfig cfg;
    bool cfg_set;
    PFN_xrGetInstanceProcAddr gipa;
    void* lib;
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace space, head_space, hand_space[2], aim_space[2];
    XrSessionState state;
    bool running, frame_begun, should_render;
    XrTime display_time;
    XrDuration period;
    XrSwapchain sc[2];
    uint32_t sc_count[2], sc_index[2];
    VkImage sc_img[2][MAX_SC_IMAGES];
    VkImageView sc_view[2][MAX_SC_IMAGES];
    VkDevice device;
    VkFormat format;
    XrView views[2];
    XrFovf fov_used[2];
    uint32_t used_w[2], used_h[2];
    int32_t crop[2][4];
    uint32_t sc_w, sc_h, max_w, max_h; // the eyes' swapchain images (room for the symmetric frustum), the runtime's most      // (the eye's own fov inside the symmetric image: x, y, w, h; the compositor is given that and its fov)
    char inst_exts[1024], dev_exts[2048];
    XrActionSet set;
    XrAction act[A_COUNT];
    XrPath hand_path[2];
    XrHand hands[2];
    XrHandTrackerEXT tracker[2];
    float head_pos[3], head_fwd[3], head_up[3];
    // the VR menu panel: a quad layer of its own swapchain
    XrSwapchain panel_sc;
    uint32_t panel_w, panel_h, panel_count, panel_index;
    VkImage panel_img[MAX_SC_IMAGES];
    VkImageView panel_view[MAX_SC_IMAGES];
    bool panel_drawn, panel_shown;
    float panel_pos[3], panel_fwd[3], panel_size[2];
    // the simulated mode: our own eye images (the window shows them side by side), its head and hands set by the tests
    bool sim;
    float sim_pos[3], sim_yaw, sim_pitch;
    bool sim_head_set;
    void* android_vm;
    void* android_activity;
} X;

const XrInfo* xr_info(void) { return &X.info; }
// (Q25b) the session as xr_mr.c sees it
void xr_internal(XrInternal* o) {
    o->instance = X.instance, o->system = X.system, o->session = X.session, o->space = X.space, o->display_time = X.display_time;
    o->gipa = X.gipa, o->device = X.device, o->running = X.running, o->sim = X.sim;
}
bool xr_active(void) { return X.sim || X.running; }
bool xr_focused(void) { return X.sim || (X.running && X.state == XR_SESSION_STATE_FOCUSED); }
double xr_frame_period_ms(void) { return X.period > 0 ? (double)X.period * 1e-6 : 1000.0 / 90.0; }

// --- The configuration (game.ini; Project Settings > VR) ---
void xr_config_defaults(XrConfig* c) {
    memset(c, 0, sizeof *c);
    c->refresh_hz = 90, c->quality = 4, c->foveation = 2, c->foveation_dynamic = true, c->render_scale = 1.0f;
}
bool xr_config_key(XrConfig* c, const char* k, const char* v) {
    if (!strcmp(k, "vr")) c->start = atoi(v) != 0;
    else if (!strcmp(k, "vr_refresh")) c->refresh_hz = atoi(v);
    else if (!strcmp(k, "vr_tracking")) c->tracking = atoi(v);
    else if (!strcmp(k, "vr_quality")) c->quality = atoi(v);
    else if (!strcmp(k, "vr_foveation")) c->foveation = atoi(v);
    else if (!strcmp(k, "vr_foveation_dynamic")) c->foveation_dynamic = atoi(v) != 0;
    else if (!strcmp(k, "vr_spacewarp")) c->spacewarp = atoi(v) != 0;
    else if (!strcmp(k, "vr_render_scale")) c->render_scale = (float)atof(v);
    else return false;
    if (c->foveation < 0) c->foveation = 0;
    if (c->foveation > 3) c->foveation = 3;
    if (!(c->render_scale >= 0.5f)) c->render_scale = c->render_scale > 0.0f ? 0.5f : 1.0f;
    if (c->render_scale > 1.5f) c->render_scale = 1.5f;
    return true;
}
void xr_set_config(const XrConfig* c) { X.cfg = *c, X.cfg_set = true; }
const XrConfig* xr_config(void) {
    if (!X.cfg_set) xr_config_defaults(&X.cfg), X.cfg_set = true;
    return &X.cfg;
}

// --- Finding and negotiating the runtime ---
#ifdef _WIN32
static bool runtime_library(char* out, size_t cap) {
    char json[1024] = "";
    const char* env = getenv("XR_RUNTIME_JSON");
    if (env && env[0]) snprintf(json, sizeof json, "%s", env);
    if (!json[0]) {
        DWORD n = sizeof json;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ, NULL, json, &n) != ERROR_SUCCESS) json[0] = 0;
    }
    if (!json[0]) return false;
    FILE* f = fopen(json, "rb");
    if (!f) return false;
    char text[8192];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    const char* k = strstr(text, "\"library_path\"");
    if (!k) return false;
    k = strchr(k + 14, '"');
    if (!k) return false;
    k++;
    char lib[512];
    size_t i = 0;
    for (; *k && *k != '"' && i < sizeof lib - 1; k++) {
        if (*k == '\\' && k[1] == '\\') k++;
        lib[i++] = *k;
    }
    lib[i] = 0;
    bool absolute = lib[0] == '/' || lib[0] == '\\' || (lib[0] && lib[1] == ':');
    if (absolute) snprintf(out, cap, "%s", lib);
    else { // (relative to the JSON's folder)
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", json);
        char* s = strrchr(dir, '\\');
        char* s2 = strrchr(dir, '/');
        if (s2 > s) s = s2;
        if (s) *s = 0;
        snprintf(out, cap, "%s\\%s", dir, lib);
    }
    return true;
}
#endif

static PFN_xrVoidFunction get(const char* name) {
    PFN_xrVoidFunction f = NULL;
    if (X.gipa) X.gipa(X.instance, name, &f);
    return f;
}

static bool negotiate(void) {
#ifdef _WIN32
    char lib[1100];
    if (!runtime_library(lib, sizeof lib)) return false;
    snprintf(X.info.runtime_name, sizeof X.info.runtime_name, "%s", lib);
    HMODULE m = LoadLibraryA(lib);
    if (!m) return false;
    X.lib = m;
    PFN_xrNegotiateLoaderRuntimeInterface neg = (PFN_xrNegotiateLoaderRuntimeInterface)(void*)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
    if (!neg) return false;
    XrNegotiateLoaderInfo li = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, 1, sizeof li, 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
    XrNegotiateRuntimeRequest rq = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, 1, sizeof rq, 0, 0, NULL };
    if (!XR_SUCCEEDED(neg(&li, &rq)) || !rq.getInstanceProcAddr) return false;
    X.gipa = rq.getInstanceProcAddr;
    return true;
#elif defined(__ANDROID__)
    // (Q25) the Quest: the Khronos loader the APK carries (package_apk.ps1 -Quest), started with the app's VM and activity
    // (XR_KHR_loader_init_android); it finds the headset's runtime itself
    void* m = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!m) return false;
    X.lib = m;
    X.gipa = (PFN_xrGetInstanceProcAddr)dlsym(m, "xrGetInstanceProcAddr");
    if (!X.gipa) return false;
    JNIEnv* env = re_android_env();
    JavaVM* vm = NULL;
    if (env) (*env)->GetJavaVM(env, &vm);
    X.android_vm = vm, X.android_activity = re_android_activity();
    PFN_xrInitializeLoaderKHR init = NULL;
    X.gipa(NULL, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&init);
    if (init) {
        XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, NULL, X.android_vm, X.android_activity };
        if (!XR_SUCCEEDED(init((const XrLoaderInitInfoBaseHeaderKHR*)&li))) return false;
    }
    snprintf(X.info.runtime_name, sizeof X.info.runtime_name, "libopenxr_loader.so");
    X.info.android = true;
    return true;
#else
    return false;
#endif
}

// --- Space conversion ---
static void xr_to_engine_vec(const XrVector3f* v, float out[3], float scale) { out[0] = -v->z * scale, out[1] = v->x * scale, out[2] = v->y * scale; }
static void quat_rotate(const XrQuaternionf* q, const float v[3], float out[3]) {
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty), out[1] = v[1] + w * ty + (z * tx - x * tz), out[2] = v[2] + w * tz + (x * ty - y * tx);
}
static void pose_to_engine(const XrPosef* p, float pos[3], float fwd[3], float right[3], float up[3]) {
    xr_to_engine_vec(&p->position, pos, 100.0f);
    const float f0[3] = { 0, 0, -1 }, r0[3] = { 1, 0, 0 }, u0[3] = { 0, 1, 0 };
    float f[3], r[3], u[3];
    quat_rotate(&p->orientation, f0, f), quat_rotate(&p->orientation, r0, r), quat_rotate(&p->orientation, u0, u);
    XrVector3f fv = { f[0], f[1], f[2] }, rv = { r[0], r[1], r[2] }, uv = { u[0], u[1], u[2] };
    if (fwd) xr_to_engine_vec(&fv, fwd, 1.0f);
    if (right) xr_to_engine_vec(&rv, right, 1.0f);
    if (up) xr_to_engine_vec(&uv, up, 1.0f);
}
// engine (cm, X forward, Y right, Z up) -> OpenXR (m, -Z forward, X right, Y up): x = y, y = z, z = -x
static XrVector3f engine_to_xr(const float v[3], float scale) { return (XrVector3f){ v[1] * scale, v[2] * scale, -v[0] * scale }; }

// --- Start ---
static void add_exts(char* list, const char** out, uint32_t* n, uint32_t cap) {
    char* s = list;
    while (*s && *n < cap) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        out[(*n)++] = s;
        while (*s && *s != ' ') s++;
    }
}

static bool has_ext(const XrExtensionProperties* props, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(props[i].extensionName, name)) return true;
    return false;
}

bool xr_init(bool sim) {
    XrConfig keep = *xr_config();
    memset(&X, 0, sizeof X);
    X.cfg = keep, X.cfg_set = true;
    X.info.sim = sim;
    // (REALENGINE_XR_SIM_ONLY=1, the self-tests: the simulated headset even with a runtime and a headset connected)
    bool sim_only = sim && getenv("REALENGINE_XR_SIM_ONLY") && getenv("REALENGINE_XR_SIM_ONLY")[0] == '1';
    if (!sim_only && negotiate()) {
        X.info.runtime = true;
        p_xrCreateInstance = (PFN_xrCreateInstance)get("xrCreateInstance");
        // the extensions: Vulkan (required); the display's refresh, tracked hands, SpaceWarp when the runtime has them
        PFN_xrEnumerateInstanceExtensionProperties enum_ext = (PFN_xrEnumerateInstanceExtensionProperties)get("xrEnumerateInstanceExtensionProperties");
        static XrExtensionProperties props[128];
        uint32_t np = 0;
        if (enum_ext) {
            for (int i = 0; i < 128; i++) props[i] = (XrExtensionProperties){ XR_TYPE_EXTENSION_PROPERTIES };
            if (!XR_SUCCEEDED(enum_ext(NULL, 128, &np, props))) np = 0;
        }
        const char* exts[24];
        uint32_t ne = 0;
        exts[ne++] = "XR_KHR_vulkan_enable";
        if (has_ext(props, np, "XR_FB_display_refresh_rate")) exts[ne++] = "XR_FB_display_refresh_rate", X.info.ext_refresh = true;
        if (has_ext(props, np, "XR_EXT_hand_tracking")) exts[ne++] = "XR_EXT_hand_tracking", X.info.ext_hands = true;
        if (has_ext(props, np, "XR_FB_space_warp")) X.info.ext_spacewarp = true; // (reported: the motion vectors aren't sent yet)
        xr_mr_pick_extensions(props, np, exts, &ne, 22); // (Q25b) passthrough, the room, the real depth (xr_mr.c)
        XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
        exts[ne++] = "XR_KHR_android_create_instance";
        XrInstanceCreateInfoAndroidKHR aci = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, NULL, X.android_vm, X.android_activity };
        ci.next = &aci;
#endif
        snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "Real Engine");
        snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "Real Engine");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledExtensionCount = ne, ci.enabledExtensionNames = exts;
        if (p_xrCreateInstance && XR_SUCCEEDED(p_xrCreateInstance(&ci, &X.instance))) {
#define L(n) p_##n = (PFN_##n)get(#n)
            L(xrDestroyInstance); L(xrGetInstanceProperties); L(xrGetSystem); L(xrGetSystemProperties); L(xrEnumerateViewConfigurationViews);
            L(xrGetVulkanInstanceExtensionsKHR); L(xrGetVulkanDeviceExtensionsKHR); L(xrGetVulkanGraphicsDeviceKHR);
            L(xrGetVulkanGraphicsRequirementsKHR); L(xrCreateSession); L(xrDestroySession); L(xrBeginSession); L(xrEndSession);
            L(xrCreateReferenceSpace); L(xrEnumerateSwapchainFormats); L(xrCreateSwapchain); L(xrDestroySwapchain);
            L(xrEnumerateSwapchainImages); L(xrAcquireSwapchainImage); L(xrWaitSwapchainImage); L(xrReleaseSwapchainImage);
            L(xrWaitFrame); L(xrBeginFrame); L(xrEndFrame); L(xrLocateViews); L(xrPollEvent); L(xrStringToPath);
            L(xrCreateActionSet); L(xrCreateAction); L(xrSuggestInteractionProfileBindings); L(xrAttachSessionActionSets);
            L(xrCreateActionSpace); L(xrSyncActions); L(xrGetActionStateFloat); L(xrGetActionStateBoolean); L(xrGetActionStateVector2f);
            L(xrLocateSpace); L(xrApplyHapticFeedback);
            if (X.info.ext_refresh) { L(xrEnumerateDisplayRefreshRatesFB); L(xrGetDisplayRefreshRateFB); L(xrRequestDisplayRefreshRateFB); }
            if (X.info.ext_hands) { L(xrCreateHandTrackerEXT); L(xrDestroyHandTrackerEXT); L(xrLocateHandJointsEXT); }
#undef L
            XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
            if (p_xrGetInstanceProperties && XR_SUCCEEDED(p_xrGetInstanceProperties(X.instance, &ip)))
                snprintf(X.info.runtime_name, sizeof X.info.runtime_name, "%s", ip.runtimeName);
            XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
            if (p_xrGetSystem && XR_SUCCEEDED(p_xrGetSystem(X.instance, &sg, &X.system))) {
                X.info.headset = true;
                XrSystemHandTrackingPropertiesEXT ht = { XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
                XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES, X.info.ext_hands ? &ht : NULL };
                if (XR_SUCCEEDED(p_xrGetSystemProperties(X.instance, X.system, &sp))) snprintf(X.info.system_name, sizeof X.info.system_name, "%s", sp.systemName);
                X.info.hand_tracking = X.info.ext_hands && ht.supportsHandTracking;
                xr_mr_system_ready(); // (Q25b) the mixed reality's functions and what the system has
                XrViewConfigurationView vv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
                uint32_t nv = 0;
                if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(X.instance, X.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vv)) && nv == 2) {
                    X.info.rec_w = vv[0].recommendedImageRectWidth, X.info.rec_h = vv[0].recommendedImageRectHeight;
                    // (Q25) the render scale over the recommended, inside the runtime's most (and even: the 2x2 shading rate)
                    float k = X.cfg.render_scale > 0.0f ? X.cfg.render_scale : 1.0f;
                    uint32_t w = (uint32_t)((float)X.info.rec_w * k + 0.5f) & ~1u, h = (uint32_t)((float)X.info.rec_h * k + 0.5f) & ~1u;
                    X.max_w = vv[0].maxImageRectWidth, X.max_h = vv[0].maxImageRectHeight;
                    if (vv[0].maxImageRectWidth && w > vv[0].maxImageRectWidth) w = vv[0].maxImageRectWidth;
                    if (vv[0].maxImageRectHeight && h > vv[0].maxImageRectHeight) h = vv[0].maxImageRectHeight;
                    X.info.eye_w = w < 256 ? 256 : w, X.info.eye_h = h < 256 ? 256 : h;
                }
                // the Vulkan extensions the runtime needs: to vkctx before it makes the instance and the device
                uint32_t n = 0;
                if (p_xrGetVulkanInstanceExtensionsKHR && XR_SUCCEEDED(p_xrGetVulkanInstanceExtensionsKHR(X.instance, X.system, sizeof X.inst_exts, &n, X.inst_exts)))
                    add_exts(X.inst_exts, g_vk_extra_instance_exts, &g_vk_extra_ninst, 16);
                if (p_xrGetVulkanDeviceExtensionsKHR && XR_SUCCEEDED(p_xrGetVulkanDeviceExtensionsKHR(X.instance, X.system, sizeof X.dev_exts, &n, X.dev_exts)))
                    add_exts(X.dev_exts, g_vk_extra_device_exts, &g_vk_extra_ndev, 24);
            }
        }
    }
    printf("[RealEngine] XR: runtime %s, headset %s (%s, %u x %u per eye; extensions: refresh %d, hands %d, SpaceWarp %d)%s\n",
           X.info.runtime ? X.info.runtime_name : "none", X.info.headset ? "found" : "not found", X.info.system_name, X.info.eye_w, X.info.eye_h,
           X.info.ext_refresh, X.info.hand_tracking, X.info.ext_spacewarp, sim && !X.info.headset ? "; simulated" : "");
    if (!X.info.headset) {
        if (X.instance && p_xrDestroyInstance) p_xrDestroyInstance(X.instance), X.instance = NULL;
        X.sim = sim;
        if (sim) { // the Quest 3's recommended per eye (2064 x 2208 at 1.0) scaled to the simulated 1280 wide, 90 Hz
            float k = X.cfg.render_scale > 0.0f ? X.cfg.render_scale : 1.0f;
            X.info.rec_w = 1280, X.info.rec_h = 1400;
            X.info.eye_w = (uint32_t)(1280.0f * k) & ~1u, X.info.eye_h = (uint32_t)(1400.0f * k) & ~1u;
            X.info.refresh_hz = X.cfg.refresh_hz > 0 ? (float)X.cfg.refresh_hz : 90.0f;
            X.period = (XrDuration)(1e9 / X.info.refresh_hz);
            X.info.stage = X.cfg.tracking == 0;
            X.sim_pos[2] = X.cfg.tracking == 1 ? 0.0f : 170.0f;
        }
        return sim;
    }
    return true;
}

VkPhysicalDevice xr_vulkan_gpu(VkInstance instance) {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    if (X.instance && p_xrGetVulkanGraphicsDeviceKHR) p_xrGetVulkanGraphicsDeviceKHR(X.instance, X.system, instance, &gpu);
    return gpu;
}

static XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(X.instance, s, &p);
    return p;
}

static void make_actions(void) {
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(si.actionSetName, sizeof si.actionSetName, "gameplay");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Gameplay");
    if (!XR_SUCCEEDED(p_xrCreateActionSet(X.instance, &si, &X.set))) return;
    X.hand_path[0] = path("/user/hand/left"), X.hand_path[1] = path("/user/hand/right");
    static const struct { const char* name; XrActionType type; } defs[A_COUNT] = {
        { "grip_pose", XR_ACTION_TYPE_POSE_INPUT }, { "aim_pose", XR_ACTION_TYPE_POSE_INPUT }, { "trigger", XR_ACTION_TYPE_FLOAT_INPUT },
        { "grip", XR_ACTION_TYPE_FLOAT_INPUT }, { "stick", XR_ACTION_TYPE_VECTOR2F_INPUT }, { "primary", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT }, { "menu", XR_ACTION_TYPE_BOOLEAN_INPUT }, { "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT },
    };
    for (int i = 0; i < A_COUNT; i++) {
        XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
        snprintf(ai.actionName, sizeof ai.actionName, "%s", defs[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", defs[i].name);
        ai.actionType = defs[i].type, ai.countSubactionPaths = 2, ai.subactionPaths = X.hand_path;
        p_xrCreateAction(X.set, &ai, &X.act[i]);
    }
    // the Touch controllers (Quest, Rift), the Quest 3 / 3S's Touch Plus, the Index, and the simple controller every runtime has
    const char* hs[2] = { "left", "right" };
    XrActionSuggestedBinding b[32];
    uint32_t n = 0;
    char p[128];
    static const char* const touch[] = { "/interaction_profiles/oculus/touch_controller", "/interaction_profiles/meta/touch_plus_controller" };
    for (int prof = 0; prof < 2; prof++) {
        n = 0;
        for (int h = 0; h < 2; h++) {
#define B(a, s) snprintf(p, sizeof p, "/user/hand/%s/%s", hs[h], s), b[n].action = X.act[a], b[n++].binding = path(p)
            B(A_POSE_GRIP, "input/grip/pose"); B(A_POSE_AIM, "input/aim/pose"); B(A_TRIGGER, "input/trigger/value"); B(A_GRIP, "input/squeeze/value");
            B(A_STICK, "input/thumbstick"); B(A_PRIMARY, h ? "input/a/click" : "input/x/click"); B(A_SECONDARY, h ? "input/b/click" : "input/y/click");
            B(A_STICK_CLICK, "input/thumbstick/click"); B(A_HAPTIC, "output/haptic");
            if (!h) B(A_MENU, "input/menu/click");
#undef B
        }
        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL, path(touch[prof]), n, b };
        p_xrSuggestInteractionProfileBindings(X.instance, &sb); // (a runtime without the profile refuses it: the others stand)
    }
    n = 0;
    for (int h = 0; h < 2; h++) {
#define B(a, s) snprintf(p, sizeof p, "/user/hand/%s/%s", hs[h], s), b[n].action = X.act[a], b[n++].binding = path(p)
        B(A_POSE_GRIP, "input/grip/pose"); B(A_POSE_AIM, "input/aim/pose"); B(A_TRIGGER, "input/trigger/value"); B(A_GRIP, "input/squeeze/value");
        B(A_STICK, "input/thumbstick"); B(A_PRIMARY, "input/a/click"); B(A_SECONDARY, "input/b/click"); B(A_STICK_CLICK, "input/thumbstick/click");
        B(A_MENU, "input/system/click"); B(A_HAPTIC, "output/haptic");
#undef B
    }
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL, path("/interaction_profiles/valve/index_controller"), n, b };
    p_xrSuggestInteractionProfileBindings(X.instance, &sb);
    n = 0;
    for (int h = 0; h < 2; h++) {
#define B(a, s) snprintf(p, sizeof p, "/user/hand/%s/%s", hs[h], s), b[n].action = X.act[a], b[n++].binding = path(p)
        B(A_POSE_GRIP, "input/grip/pose"); B(A_POSE_AIM, "input/aim/pose"); B(A_PRIMARY, "input/select/click"); B(A_MENU, "input/menu/click");
        B(A_HAPTIC, "output/haptic");
#undef B
    }
    sb.interactionProfile = path("/interaction_profiles/khr/simple_controller"), sb.countSuggestedBindings = n;
    p_xrSuggestInteractionProfileBindings(X.instance, &sb);
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, NULL, 1, &X.set };
    p_xrAttachSessionActionSets(X.session, &at);
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo as = { XR_TYPE_ACTION_SPACE_CREATE_INFO, NULL, X.act[A_POSE_GRIP], X.hand_path[h], { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
        p_xrCreateActionSpace(X.session, &as, &X.hand_space[h]);
        as.action = X.act[A_POSE_AIM];
        p_xrCreateActionSpace(X.session, &as, &X.aim_space[h]);
    }
}

static void make_swapchain(XrSwapchain* sc, int64_t fmt, uint32_t w, uint32_t h, uint32_t* count, VkImage* imgs_out, VkImageView* views_out) {
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | 0x8ULL; // (and transfer source: the window's mirror)
    ci.format = fmt, ci.sampleCount = 1, ci.width = w, ci.height = h, ci.faceCount = 1, ci.arraySize = 1, ci.mipCount = 1;
    *count = 0;
    if (!XR_SUCCEEDED(p_xrCreateSwapchain(X.session, &ci, sc))) {
        *sc = NULL;
        return;
    }
    XrSwapchainImageVulkanKHR imgs[MAX_SC_IMAGES];
    for (int i = 0; i < MAX_SC_IMAGES; i++) imgs[i] = (XrSwapchainImageVulkanKHR){ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR };
    p_xrEnumerateSwapchainImages(*sc, MAX_SC_IMAGES, count, (XrSwapchainImageBaseHeader*)imgs);
    for (uint32_t i = 0; i < *count; i++) {
        imgs_out[i] = imgs[i].image;
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = imgs[i].image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = (VkFormat)fmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(X.device, &vi, NULL, &views_out[i]);
    }
}

bool xr_start_session(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, uint32_t queue_family, VkFormat color_format) {
    X.device = device, X.format = color_format;
    if (X.sim) return true;
    if (!X.instance || !X.info.headset) return false;
    XrGraphicsRequirementsVulkanKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
    p_xrGetVulkanGraphicsRequirementsKHR(X.instance, X.system, &req); // (required before the session)
    XrGraphicsBindingVulkanKHR gb = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, NULL, instance, gpu, device, queue_family, 0 };
    XrSessionCreateInfo sc = { XR_TYPE_SESSION_CREATE_INFO, &gb, 0, X.system };
    if (!XR_SUCCEEDED(p_xrCreateSession(X.instance, &sc, &X.session))) {
        printf("[RealEngine] XR: the session could not start (the headset asleep or in use?)\n");
        return false;
    }
    // (Q25) the play space: the floor (STAGE: the guardian's center on the floor) standing, LOCAL seated or without a stage
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, X.cfg.tracking == 1 ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_STAGE,
                                      { { 0, 0, 0, 1 }, { 0, 0, 0 } } };
    X.info.stage = rs.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE;
    if (!XR_SUCCEEDED(p_xrCreateReferenceSpace(X.session, &rs, &X.space)))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, X.info.stage = false, p_xrCreateReferenceSpace(X.session, &rs, &X.space);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    p_xrCreateReferenceSpace(X.session, &rs, &X.head_space);
    // the eyes' swapchains in the window's format when the runtime has it (the composite's pipelines are made for it)
    int64_t fmts[64];
    uint32_t nf = 0;
    p_xrEnumerateSwapchainFormats(X.session, 64, &nf, fmts);
    int64_t fmt = nf ? fmts[0] : (int64_t)color_format;
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i] == (int64_t)color_format) fmt = fmts[i];
    if (fmt != (int64_t)color_format) printf("[RealEngine] XR: the runtime has no swapchain in the window's format %d (it took %lld)\n", (int)color_format, (long long)fmt);
    // the symmetric frustum holds the eye's asymmetric fov: wider than it (a Quest 3S: 2 x 52 degrees for 52 + 45), so the
    // image is made larger than the recommended size for the eye's own part of it to keep the recommended pixels (25% / 15%
    // of room: any headset's asymmetry so far), within the runtime's most
    X.sc_w = (uint32_t)((float)X.info.eye_w * 1.25f + 0.5f) & ~1u, X.sc_h = (uint32_t)((float)X.info.eye_h * 1.15f + 0.5f) & ~1u;
    if (X.max_w && X.sc_w > X.max_w) X.sc_w = X.max_w;
    if (X.max_h && X.sc_h > X.max_h) X.sc_h = X.max_h;
    for (int e = 0; e < 2; e++) {
        make_swapchain(&X.sc[e], fmt, X.sc_w, X.sc_h, &X.sc_count[e], X.sc_img[e], X.sc_view[e]);
        if (!X.sc[e]) return false;
    }
    // the VR menu panel's quad (1280 x 800: the UI's pixels at about the headset's own density at 1.5 m)
    X.panel_w = 1280, X.panel_h = 800;
    make_swapchain(&X.panel_sc, fmt, X.panel_w, X.panel_h, &X.panel_count, X.panel_img, X.panel_view);
    make_actions();
    // (Q25) tracked hands
    if (X.info.hand_tracking && p_xrCreateHandTrackerEXT)
        for (int h = 0; h < 2; h++) {
            XrHandTrackerCreateInfoEXT hc = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, NULL, h ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT, 0 };
            if (!XR_SUCCEEDED(p_xrCreateHandTrackerEXT(X.session, &hc, &X.tracker[h]))) X.tracker[h] = NULL;
        }
    xr_mr_session_ready(); // (Q25b) the passthrough layer, the real depth's provider, the room's query
    // (Q25) the display's refresh rates
    if (X.info.ext_refresh && p_xrEnumerateDisplayRefreshRatesFB) {
        uint32_t nr = 0;
        if (XR_SUCCEEDED(p_xrEnumerateDisplayRefreshRatesFB(X.session, 8, &nr, X.info.refresh_rates))) X.info.refresh_count = (int)nr;
        float now = 0.0f;
        if (p_xrGetDisplayRefreshRateFB && XR_SUCCEEDED(p_xrGetDisplayRefreshRateFB(X.session, &now))) X.info.refresh_hz = now;
    }
    printf("[RealEngine] XR: session ready (%s, %u x %u per eye, format %lld, %s space, %d refresh rates)\n", X.info.system_name, X.info.eye_w, X.info.eye_h,
           (long long)fmt, X.info.stage ? "floor" : "local", X.info.refresh_count);
    return true;
}

bool xr_set_refresh(float hz) {
    if (X.sim) return X.info.refresh_hz = hz, X.period = (XrDuration)(1e9 / hz), true;
    if (!X.session || !p_xrRequestDisplayRefreshRateFB) return false;
    for (int i = 0; i < X.info.refresh_count; i++)
        if (fabsf(X.info.refresh_rates[i] - hz) < 0.5f) return XR_SUCCEEDED(p_xrRequestDisplayRefreshRateFB(X.session, X.info.refresh_rates[i]));
    return false;
}

void xr_shutdown(void) {
    if (X.session) {
        xr_mr_shutdown(); // (Q25b)
        for (int h = 0; h < 2; h++)
            if (X.tracker[h] && p_xrDestroyHandTrackerEXT) p_xrDestroyHandTrackerEXT(X.tracker[h]);
        for (int e = 0; e < 2; e++) {
            for (uint32_t i = 0; i < X.sc_count[e]; i++)
                if (X.sc_view[e][i]) vkDestroyImageView(X.device, X.sc_view[e][i], NULL);
            if (X.sc[e]) p_xrDestroySwapchain(X.sc[e]);
        }
        for (uint32_t i = 0; i < X.panel_count; i++)
            if (X.panel_view[i]) vkDestroyImageView(X.device, X.panel_view[i], NULL);
        if (X.panel_sc) p_xrDestroySwapchain(X.panel_sc);
        if (X.running) p_xrEndSession(X.session);
        p_xrDestroySession(X.session);
    }
    if (X.instance && p_xrDestroyInstance) p_xrDestroyInstance(X.instance);
#ifdef _WIN32
    if (X.lib) FreeLibrary((HMODULE)X.lib);
#elif defined(__ANDROID__)
    if (X.lib) dlclose(X.lib);
#endif
    XrConfig keep = X.cfg;
    memset(&X, 0, sizeof X);
    X.cfg = keep, X.cfg_set = true;
}

// --- A frame ---
static void poll_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (p_xrPollEvent(X.instance, &ev) == 0) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* sc = (const XrEventDataSessionStateChanged*)&ev;
            X.state = sc->state;
            if (X.state == XR_SESSION_STATE_READY && !X.running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                X.running = XR_SUCCEEDED(p_xrBeginSession(X.session, &bi));
                if (X.running && X.cfg.refresh_hz > 0 && xr_set_refresh((float)X.cfg.refresh_hz))
                    printf("[RealEngine] XR: %d Hz asked of the display\n", X.cfg.refresh_hz);
            } else if (X.state == XR_SESSION_STATE_STOPPING && X.running) {
                p_xrEndSession(X.session);
                X.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const XrEventDataDisplayRefreshRateChangedFB* rc = (const XrEventDataDisplayRefreshRateChangedFB*)&ev;
            X.info.refresh_hz = rc->toDisplayRefreshRate;
            printf("[RealEngine] XR: the display runs at %.0f Hz\n", rc->toDisplayRefreshRate);
        } else
            xr_mr_event(&ev); // (Q25b) the passthrough's state, the room's queries, the Space Setup
        ev = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static float dist3(const float a[3], const float b[3]) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// the tracked hand's joints (engine space); a fist and a pinch give the grip and the trigger as a controller's would
static void read_tracked(int h) {
    XrHand* o = &X.hands[h];
    bool was = o->tracked;
    o->tracked = false;
    if (!X.tracker[h] || !p_xrLocateHandJointsEXT || !X.cfg_set) return;
    XrHandJointLocationEXT j[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointsLocateInfoEXT li = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, NULL, X.space, X.display_time };
    XrHandJointLocationsEXT jl = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT, NULL, 0, XR_HAND_JOINT_COUNT_EXT, j };
    if (!XR_SUCCEEDED(p_xrLocateHandJointsEXT(X.tracker[h], &li, &jl)) || !jl.isActive) {
        if (was) printf("[RealEngine] XR: hand %d no longer tracked\n", h);
        return;
    }
    for (int k = 0; k < XR_HAND_JOINT_COUNT_EXT; k++) xr_to_engine_vec(&j[k].pose.position, o->joint[k], 100.0f);
    // the real hand (Q25, the Quest 3S's first test): the runtime also gives the "controller" poses from the hand (its
    // emulated profile), their trigger and grip at 0: with the joints active the hand's own pose, pinch and fist always win
    pose_to_engine(&j[0].pose, o->pos, o->fwd, NULL, o->up);
    o->active = false;
    xr_hand_from_joints(o);
    if (!was) printf("[RealEngine] XR: hand %d tracked (the fist grabs, the pinch is the trigger)\n", h);
}
// a finger's curl: how near its tip comes back to its base for its length (straight ~1, a relaxed open hand ~0.9, a fist
// ~0.4): any hand's size the same (the tips' distance to the palm read a relaxed open hand as half closed: things grabbed
// that weren't, never let go, the drag moving with the hand open, Q25's headset test)
const char* const g_xr_gesture_names[XR_GESTURE_COUNT] = { "None", "Open", "Fist", "Point", "Pinch", "Thumbs Up", "Peace" };
// the distance from p to the segment a-b
static float seg_dist(const float p[3], const float a[3], const float b[3]) {
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-6f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0f;
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    float c[3] = { a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t };
    return dist3(p, c);
}
static float finger_curl_n(const XrHand* o, int meta, int bones) {
    float len = 0.0f;
    for (int k = meta; k < meta + bones; k++) len += dist3(o->joint[k], o->joint[k + 1]);
    if (len < 1.0f) return 0.0f;
    float r = dist3(o->joint[meta + bones], o->joint[meta]) / len;
    return fminf(fmaxf((0.86f - r) / 0.40f, 0.0f), 1.0f);
}
static float finger_curl(const XrHand* o, int meta) {
    float len = 0.0f;
    for (int k = meta; k < meta + 4; k++) len += dist3(o->joint[k], o->joint[k + 1]);
    if (len < 1.0f) return 0.0f;
    float r = dist3(o->joint[meta + 4], o->joint[meta]) / len;
    return fminf(fmaxf((0.86f - r) / 0.40f, 0.0f), 1.0f);
}
void xr_hand_from_joints(XrHand* o) {
    o->tracked = true;
    // the pinch: thumb tip to index tip for the hand's size (the index's length), smoothed over a few frames (the
    // tracking's noise flickered the trigger)
    float ilen = 0.0f;
    for (int k = 6; k < 10; k++) ilen += dist3(o->joint[k], o->joint[k + 1]);
    float scale = ilen > 4.0f ? ilen / 9.0f : 1.0f; // (an adult's index ~9 cm)
    float pinch_cm = dist3(o->joint[5], o->joint[10]) / scale;
    float pinch = fminf(fmaxf((3.5f - pinch_cm) / 2.5f, 0.0f), 1.0f);
    o->pinch_s += (pinch - o->pinch_s) * 0.5f;
    o->pinch = o->pinch_s;
    if (o->active) return; // (a controller in the hand: its own trigger and grip)
    // the aim: from the chest (25 cm under the head) through the palm, as the headset's own system pointer does (the
    // index's own direction moves with the pinch)
    float chest[3] = { X.head_pos[0], X.head_pos[1], X.head_pos[2] - 25.0f };
    float d[3] = { o->joint[0][0] - chest[0], o->joint[0][1] - chest[1], o->joint[0][2] - chest[2] };
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 5.0f) { // (no head yet, the tests': along the index)
        d[0] = o->joint[10][0] - o->joint[7][0], d[1] = o->joint[10][1] - o->joint[7][1], d[2] = o->joint[10][2] - o->joint[7][2];
        l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    memcpy(o->aim_pos, o->joint[0], 12);
    if (l > 1e-3f) o->aim_fwd[0] = d[0] / l, o->aim_fwd[1] = d[1] / l, o->aim_fwd[2] = d[2] / l;
    o->active = true;
    o->trigger = o->pinch;
    // the fist: the middle, ring and little fingers curled, smoothed
    float curl = (finger_curl(o, 11) + finger_curl(o, 16) + finger_curl(o, 21)) / 3.0f;
    o->grip_s += (curl - o->grip_s) * 0.5f;
    o->grip = o->grip_s;
    xr_hand_shape(o);
}
// each finger's curl, the touches on the index (a tip within 1 cm of the index's bones: 1; 2.5 cm: 0), the shape
void xr_hand_shape(XrHand* o) {
    const float k = 0.5f; // (smoothed as the grip)
    float c[5] = { finger_curl_n(o, 2, 3), finger_curl(o, 6), finger_curl(o, 11), finger_curl(o, 16), finger_curl(o, 21) };
    for (int f = 0; f < 5; f++) o->curl[f] += (c[f] - o->curl[f]) * k;
    // a touch: the tip on the index's upper half (its middle bone to the tip), 1 cm counts and 2 cm doesn't (for an adult's
    // hand); the middle, ring and little only while curled toward it. An open hand with the fingers together has the middle
    // beside the index and the thumb resting by the index's base: the old 2.5 cm on the whole index read it as a pinch, so
    // things stuck to the open hand (Q25's headset test)
    static const int tips[4] = { 5, 15, 20, 25 };
    float ilen = 0.0f;
    for (int b = 6; b < 10; b++) ilen += dist3(o->joint[b], o->joint[b + 1]);
    float scale = ilen > 4.0f ? ilen / 9.0f : 1.0f;
    for (int f = 0; f < 4; f++) {
        float d = 1e9f;
        for (int b = 8; b < 10; b++) d = fminf(d, seg_dist(o->joint[tips[f]], o->joint[b], o->joint[b + 1]));
        d /= scale;
        float t = fminf(fmaxf(2.0f - d, 0.0f), 1.0f);
        if (f > 0 && c[f + 1] < 0.35f) t = 0.0f;
        o->touch[f] += (t - o->touch[f]) * k;
    }
    const float *C = o->curl;
    bool others_curled = C[2] > 0.6f && C[3] > 0.6f && C[4] > 0.6f;
    int g = XR_GESTURE_NONE;
    if (o->touch[0] > 0.8f && C[2] < 0.6f) g = XR_GESTURE_PINCH;
    else if (others_curled && C[1] > 0.6f && C[0] < 0.3f && o->touch[0] < 0.3f) g = XR_GESTURE_THUMBS_UP;
    else if (others_curled && C[1] > 0.55f) g = XR_GESTURE_FIST;
    else if (C[1] < 0.3f && others_curled) g = XR_GESTURE_POINT;
    else if (C[1] < 0.3f && C[2] < 0.3f && C[3] > 0.6f && C[4] > 0.6f) g = XR_GESTURE_PEACE;
    else if (C[1] < 0.3f && C[2] < 0.3f && C[3] < 0.3f && C[4] < 0.3f) g = XR_GESTURE_OPEN;
    o->gesture = g;
}

static void read_hands(void) {
    XrActiveActionSet as = { X.set, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO, NULL, 1, &as };
    bool synced = X.set && XR_SUCCEEDED(p_xrSyncActions(X.session, &si));
    for (int h = 0; h < 2; h++) {
        XrHand* o = &X.hands[h];
        o->active = false;
        if (synced) {
            XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO, NULL, X.act[A_TRIGGER], X.hand_path[h] };
            XrActionStateFloat fs = { XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(p_xrGetActionStateFloat(X.session, &gi, &fs))) o->trigger = fs.currentState;
            gi.action = X.act[A_GRIP], fs = (XrActionStateFloat){ XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(p_xrGetActionStateFloat(X.session, &gi, &fs))) o->grip = fs.currentState;
            gi.action = X.act[A_STICK];
            XrActionStateVector2f vs = { XR_TYPE_ACTION_STATE_VECTOR2F };
            if (XR_SUCCEEDED(p_xrGetActionStateVector2f(X.session, &gi, &vs))) o->stick[0] = vs.currentState.x, o->stick[1] = vs.currentState.y;
            bool* bs[4] = { &o->primary, &o->secondary, &o->menu, &o->stick_click };
            const int ba[4] = { A_PRIMARY, A_SECONDARY, A_MENU, A_STICK_CLICK };
            for (int k = 0; k < 4; k++) {
                gi.action = X.act[ba[k]];
                XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (XR_SUCCEEDED(p_xrGetActionStateBoolean(X.session, &gi, &b))) *bs[k] = b.currentState != 0;
            }
            XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
            o->active = X.hand_space[h] && XR_SUCCEEDED(p_xrLocateSpace(X.hand_space[h], X.space, X.display_time, &loc)) &&
                        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
            if (o->active) pose_to_engine(&loc.pose, o->pos, o->fwd, NULL, o->up);
            XrSpaceLocation al = { XR_TYPE_SPACE_LOCATION };
            if (o->active && X.aim_space[h] && XR_SUCCEEDED(p_xrLocateSpace(X.aim_space[h], X.space, X.display_time, &al)) &&
                (al.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                pose_to_engine(&al.pose, o->aim_pos, o->aim_fwd, NULL, NULL);
            else if (o->active)
                memcpy(o->aim_pos, o->pos, 12), memcpy(o->aim_fwd, o->fwd, 12);
        }
        read_tracked(h);
        if (!o->active) // (neither a controller nor a tracked hand: nothing held down; a lost fist was kept closed and the drag
            // followed the hand to where it was seen again: the player thrown across the level, Q25's headset test)
            o->trigger = o->grip = o->pinch = 0.0f, o->stick[0] = o->stick[1] = 0.0f, o->primary = o->secondary = o->menu = o->stick_click = false;
    }
}

// the simulated head: its position and yaw/pitch (the tests', or the slow look the measures want)
static void sim_basis(float fwd[3], float right[3], float up[3]) {
    float y = X.sim_yaw * 0.0174533f, p = X.sim_pitch * 0.0174533f;
    fwd[0] = cosf(p) * cosf(y), fwd[1] = cosf(p) * sinf(y), fwd[2] = sinf(p);
    right[0] = -sinf(y), right[1] = cosf(y), right[2] = 0.0f;
    // (up = fwd x right in the engine's axes; right x fwd turned the simulated camera upside down, its faces culled the wrong way)
    up[0] = fwd[1] * right[2] - fwd[2] * right[1], up[1] = fwd[2] * right[0] - fwd[0] * right[2], up[2] = fwd[0] * right[1] - fwd[1] * right[0];
}

bool xr_begin_frame(XrEye eyes[2]) {
    if (X.sim) { // two eyes 6.4 cm apart at 1.7 m (or the tests' head), a 100 degree fov
        float f[3], r[3], u[3];
        sim_basis(f, r, u);
        float hp[3] = { X.sim_pos[0], X.sim_pos[1], X.sim_pos[2] };
        if (!X.sim_head_set && X.cfg.tracking != 1) hp[2] = 170.0f;
        for (int e = 0; e < 2; e++) {
            XrEye* o = &eyes[e];
            memset(o, 0, sizeof *o);
            for (int k = 0; k < 3; k++) o->pos[k] = hp[k] + r[k] * (e ? 3.2f : -3.2f), o->fwd[k] = f[k], o->right[k] = r[k], o->up[k] = u[k];
            o->tan_left = -1.19f, o->tan_right = 1.19f, o->tan_up = 1.19f, o->tan_down = -1.19f;
            o->w = X.info.eye_w, o->h = X.info.eye_h;
        }
        memcpy(X.head_pos, hp, 12), memcpy(X.head_fwd, f, 12), memcpy(X.head_up, u, 12);
        return true;
    }
    if (!X.session) return false;
    poll_events();
    if (!X.running) return false;
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (!XR_SUCCEEDED(p_xrWaitFrame(X.session, &wi, &fs))) return false;
    X.display_time = fs.predictedDisplayTime;
    X.should_render = fs.shouldRender != 0;
    if (fs.predictedDisplayPeriod > 0) X.period = fs.predictedDisplayPeriod, X.info.refresh_hz = 1e9f / (float)fs.predictedDisplayPeriod;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    X.frame_begun = XR_SUCCEEDED(p_xrBeginFrame(X.session, &bi));
    if (!X.frame_begun) return false;
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO, NULL, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, X.display_time, X.space };
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t n = 0;
    X.views[0] = (XrView){ XR_TYPE_VIEW }, X.views[1] = (XrView){ XR_TYPE_VIEW };
    if (!XR_SUCCEEDED(p_xrLocateViews(X.session, &li, &vs, 2, &n, X.views)) || n != 2) X.should_render = false;
    XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
    if (X.head_space && XR_SUCCEEDED(p_xrLocateSpace(X.head_space, X.space, X.display_time, &hl))) pose_to_engine(&hl.pose, X.head_pos, X.head_fwd, NULL, X.head_up);
    read_hands();
    xr_mr_frame(); // (Q25b) the real depth for this frame
    if (!X.should_render) { xr_end_frame(); return false; }
    for (int e = 0; e < 2; e++) {
        XrEye* o = &eyes[e];
        pose_to_engine(&X.views[e].pose, o->pos, o->fwd, o->right, o->up);
        // a symmetric frustum that holds the eye's (the renderer's projection is centered). The compositor gets the part
        // of it that is the eye's own fov, with that fov: the Quest's are asymmetric (-52/+45 degrees one eye, -45/+52 the
        // other); told the symmetric one, its lenses' mapping put each eye's picture off by the difference, the two eyes
        // turned apart (seen double: Q25's first headset run)
        const XrFovf* fv = &X.views[e].fov;
        float tl = tanf(fv->angleLeft), tr = tanf(fv->angleRight), tu = tanf(fv->angleUp), td = tanf(fv->angleDown);
        float tw = fmaxf(fabsf(tl), fabsf(tr)), th = fmaxf(fabsf(tu), fabsf(td));
        o->tan_left = -tw, o->tan_right = tw, o->tan_up = th, o->tan_down = -th;
        // the rendered size: the eye's own part of it at the recommended size (times the render scale), the rest around it;
        // inside the swapchain image (smaller, same aspect, when it doesn't fit)
        float fw = (float)X.info.eye_w * 2.0f * tw / fmaxf(tr - tl, 1e-3f), fh = fw * th / tw;
        float fit = fminf(fminf((float)X.sc_w / fw, (float)X.sc_h / fh), 1.0f);
        uint32_t w = (uint32_t)(fw * fit) & ~1u, h = (uint32_t)(fh * fit) & ~1u;
        o->w = w, o->h = h;
        X.used_w[e] = w, X.used_h[e] = h;
        // the eye's own fov in that image: x from left (-tw) to right (+tw), y from top (+th) down
        int32_t x0 = (int32_t)floorf((tl + tw) / (2.0f * tw) * (float)w), x1 = (int32_t)ceilf((tr + tw) / (2.0f * tw) * (float)w);
        int32_t y0 = (int32_t)floorf((th - tu) / (2.0f * th) * (float)h), y1 = (int32_t)ceilf((th - td) / (2.0f * th) * (float)h);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)w) x1 = (int32_t)w;
        if (y1 > (int32_t)h) y1 = (int32_t)h;
        X.crop[e][0] = x0, X.crop[e][1] = y0, X.crop[e][2] = x1 - x0, X.crop[e][3] = y1 - y0;
        // (the fov of the pixels given: the rounding to whole pixels kept exact)
        X.fov_used[e] = (XrFovf){ atanf(-tw + 2.0f * tw * (float)x0 / (float)w), atanf(-tw + 2.0f * tw * (float)x1 / (float)w),
                                  atanf(th - 2.0f * th * (float)y0 / (float)h), atanf(th - 2.0f * th * (float)y1 / (float)h) };
    }
    static int said;
    if (!said++ || getenv("REALENGINE_XR_DEBUG"))
        for (int e = 0; e < 2; e++)
            printf("[RealEngine] XR eye %d: fov L %.2f R %.2f U %.2f D %.2f deg, rendered %u x %u (symmetric), given %d,%d %d x %d (fov %.2f %.2f %.2f %.2f)\n", e,
                   X.views[e].fov.angleLeft * 57.2958f, X.views[e].fov.angleRight * 57.2958f, X.views[e].fov.angleUp * 57.2958f,
                   X.views[e].fov.angleDown * 57.2958f, eyes[e].w, eyes[e].h, X.crop[e][0], X.crop[e][1], X.crop[e][2], X.crop[e][3],
                   X.fov_used[e].angleLeft * 57.2958f, X.fov_used[e].angleRight * 57.2958f, X.fov_used[e].angleUp * 57.2958f, X.fov_used[e].angleDown * 57.2958f);
    return true;
}

bool xr_eye_target(uint32_t eye, VkImage* image, VkImageView* view, uint32_t* w, uint32_t* h) {
    if (X.sim || !X.session || eye > 1) return false;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(p_xrAcquireSwapchainImage(X.sc[eye], &ai, &X.sc_index[eye]))) return false;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION };
    p_xrWaitSwapchainImage(X.sc[eye], &wi);
    *image = X.sc_img[eye][X.sc_index[eye]], *view = X.sc_view[eye][X.sc_index[eye]];
    *w = X.used_w[eye] ? X.used_w[eye] : X.info.eye_w, *h = X.used_h[eye] ? X.used_h[eye] : X.info.eye_h;
    return true;
}

void xr_eye_done(uint32_t eye) {
    if (X.sim || !X.session) return;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(X.sc[eye], &ri);
}

// --- The VR menu panel ---
bool xr_panel_target(uint32_t w, uint32_t h, VkImage* image, VkImageView* view) {
    (void)w, (void)h;
    if (X.sim || !X.session || !X.panel_sc) return false;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(p_xrAcquireSwapchainImage(X.panel_sc, &ai, &X.panel_index))) return false;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION };
    p_xrWaitSwapchainImage(X.panel_sc, &wi);
    *image = X.panel_img[X.panel_index], *view = X.panel_view[X.panel_index];
    return true;
}
void xr_panel_done(void) {
    if (X.sim || !X.session || !X.panel_sc) return;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    p_xrReleaseSwapchainImage(X.panel_sc, &ri);
    X.panel_drawn = true;
}
void xr_panel_place(const float pos[3], const float fwd[3], float width_cm, float height_cm, bool shown) {
    memcpy(X.panel_pos, pos, 12), memcpy(X.panel_fwd, fwd, 12);
    X.panel_size[0] = width_cm, X.panel_size[1] = height_cm;
    X.panel_shown = shown;
}
// the panel faces -fwd (toward the player); its right is fwd x up turned: right = (fwd.y, -fwd.x, 0) normalized
static bool panel_basis(float right[3], float up[3]) {
    float f[3] = { X.panel_fwd[0], X.panel_fwd[1], X.panel_fwd[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1]);
    if (l < 1e-4f) return false;
    right[0] = -f[1] / l, right[1] = f[0] / l, right[2] = 0.0f; // (seen from the player, looking along fwd: +Y is right)
    up[0] = right[1] * f[2] - right[2] * f[1], up[1] = right[2] * f[0] - right[0] * f[2], up[2] = right[0] * f[1] - right[1] * f[0];
    float ul = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (ul < 1e-4f) return false;
    for (int k = 0; k < 3; k++) up[k] /= ul;
    if (up[2] < 0.0f)
        for (int k = 0; k < 3; k++) up[k] = -up[k];
    return true;
}
bool xr_panel_hit(const float o[3], const float d[3], float* t_out, float uv[2]) {
    if (!X.panel_shown || X.panel_size[0] <= 0.0f) return false;
    float n[3] = { X.panel_fwd[0], X.panel_fwd[1], X.panel_fwd[2] }; // (its normal: the ray must come toward its face)
    float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (dn >= -1e-4f && dn <= 1e-4f) return false;
    float t = ((X.panel_pos[0] - o[0]) * n[0] + (X.panel_pos[1] - o[1]) * n[1] + (X.panel_pos[2] - o[2]) * n[2]) / dn;
    if (t <= 0.0f) return false;
    float p[3] = { o[0] + d[0] * t - X.panel_pos[0], o[1] + d[1] * t - X.panel_pos[1], o[2] + d[2] * t - X.panel_pos[2] };
    float r[3], u[3];
    if (!panel_basis(r, u)) return false;
    float x = (p[0] * r[0] + p[1] * r[1] + p[2] * r[2]) / X.panel_size[0] + 0.5f;
    float y = 0.5f - (p[0] * u[0] + p[1] * u[1] + p[2] * u[2]) / X.panel_size[1];
    if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return false;
    if (t_out) *t_out = t;
    if (uv) uv[0] = x, uv[1] = y;
    return true;
}

void xr_end_frame(void) {
    if (X.sim || !X.session || !X.frame_begun) return;
    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; e++) {
        pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        pv[e].pose = X.views[e].pose, pv[e].fov = X.fov_used[e];
        pv[e].subImage.swapchain = X.sc[e];
        pv[e].subImage.imageRect.offset.x = X.crop[e][0], pv[e].subImage.imageRect.offset.y = X.crop[e][1];
        pv[e].subImage.imageRect.extent.width = X.crop[e][2] > 0 ? X.crop[e][2] : (int32_t)X.info.eye_w;
        pv[e].subImage.imageRect.extent.height = X.crop[e][3] > 0 ? X.crop[e][3] : (int32_t)X.info.eye_h;
    }
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION, NULL, 0, X.space, 2, pv };
    // (Q25b) mixed reality: the passthrough's layer under the eyes (their alpha shows it), or the ALPHA_BLEND environment
    XrFlags64 proj_flags = 0;
    int blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const XrCompositionLayerBaseHeader* pt = (const XrCompositionLayerBaseHeader*)xr_mr_compose(&proj_flags, &blend);
    layer.layerFlags = proj_flags;
    const XrCompositionLayerBaseHeader* layers[3] = { NULL, NULL, NULL };
    uint32_t nl = 0;
    if (X.should_render) {
        if (pt) layers[nl++] = pt;
        layers[nl++] = (const XrCompositionLayerBaseHeader*)&layer;
    }
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    float r[3], u[3];
    if (X.should_render && X.panel_shown && X.panel_drawn && panel_basis(r, u)) { // the menu panel over the scene, facing the player
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, quad.space = X.space, quad.eyeVisibility = 0;
        quad.subImage.swapchain = X.panel_sc;
        quad.subImage.imageRect.extent.width = (int32_t)X.panel_w, quad.subImage.imageRect.extent.height = (int32_t)X.panel_h;
        quad.pose.position = engine_to_xr(X.panel_pos, 0.01f);
        // its orientation: OpenXR's quad faces +Z (its -Z away from the viewer): the basis (right, up, -fwd) as a quaternion
        XrVector3f xr_r = engine_to_xr(r, 1.0f), xr_u = engine_to_xr(u, 1.0f), xr_b = engine_to_xr(X.panel_fwd, -1.0f);
        float m00 = xr_r.x, m01 = xr_u.x, m02 = xr_b.x, m10 = xr_r.y, m11 = xr_u.y, m12 = xr_b.y, m20 = xr_r.z, m21 = xr_u.z, m22 = xr_b.z;
        float tr = m00 + m11 + m22, qw, qx, qy, qz;
        if (tr > 0.0f) {
            float s = sqrtf(tr + 1.0f) * 2.0f;
            qw = 0.25f * s, qx = (m21 - m12) / s, qy = (m02 - m20) / s, qz = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
            qw = (m21 - m12) / s, qx = 0.25f * s, qy = (m01 + m10) / s, qz = (m02 + m20) / s;
        } else if (m11 > m22) {
            float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
            qw = (m02 - m20) / s, qx = (m01 + m10) / s, qy = 0.25f * s, qz = (m12 + m21) / s;
        } else {
            float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
            qw = (m10 - m01) / s, qx = (m02 + m20) / s, qy = (m12 + m21) / s, qz = 0.25f * s;
        }
        quad.pose.orientation = (XrQuaternionf){ qx, qy, qz, qw };
        quad.size = (XrExtent2Df){ X.panel_size[0] * 0.01f, X.panel_size[1] * 0.01f };
        layers[nl++] = (const XrCompositionLayerBaseHeader*)&quad;
    }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO, NULL, X.display_time, (XrEnvironmentBlendMode)blend, nl, layers };
    p_xrEndFrame(X.session, &ei);
    X.frame_begun = false;
    X.panel_drawn = false;
}

const XrHand* xr_hand(int hand) { return &X.hands[hand ? 1 : 0]; }
void xr_head(float pos[3], float fwd[3], float up[3]) {
    memcpy(pos, X.head_pos, 12), memcpy(fwd, X.head_fwd, 12), memcpy(up, X.head_up, 12);
}
void xr_haptic(int hand, float amplitude, float seconds) {
    if (!X.session || !X.act[A_HAPTIC]) return;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO, NULL, X.act[A_HAPTIC], X.hand_path[hand ? 1 : 0] };
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION, NULL, (XrDuration)(seconds * 1e9), 0.0f, amplitude };
    p_xrApplyHapticFeedback(X.session, &hi, (const XrHapticBaseHeader*)&v);
}

// --- The simulated mode's inputs ---
void xr_sim_head(const float pos[3], float yaw_deg, float pitch_deg) {
    memcpy(X.sim_pos, pos, 12), X.sim_yaw = yaw_deg, X.sim_pitch = pitch_deg, X.sim_head_set = true;
    float f[3], r[3], u[3];
    sim_basis(f, r, u);
    memcpy(X.head_pos, pos, 12), memcpy(X.head_fwd, f, 12), memcpy(X.head_up, u, 12);
}
void xr_sim_hand(int hand, const XrHand* h) { X.hands[hand ? 1 : 0] = *h; }
