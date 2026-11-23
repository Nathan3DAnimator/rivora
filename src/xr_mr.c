// (Q25b) Mixed reality (xr_mr.h, notas-dev/xr.md): the passthrough, the room (Space Setup) and the real depth of a Meta
// headset through OpenXR (no SDK: xr_openxr_mr.h), and what the game makes of them: the room's actors, line traces, the
// renderer's numbers (render_mr_frame: the eye's alpha, the occlusion, the shadows on the real room), the simulated room.
// Engine space: X forward, Y right, Z up, cm; OpenXR's: -Z forward, X right, Y up, meters (a reflection: x = -z, y = x, z = y).
#include "xr_mr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "platform.h"
#include "project.h"
#include "render.h"
#include "vr_pawn.h"
#include "xr.h"
#include "xr_internal.h"
#include "xr_openxr_mr.h"

#ifdef __ANDROID__
#include "../android/re_android.h"
#endif

// --- The labels ---
static const char* const g_label_names[MR_LABEL_COUNT] = { "Floor", "Ceiling", "Wall", "Table", "Couch", "Bed", "Storage", "Screen",
                                                           "Window", "Door", "Lamp", "Plant", "Wall Art", "Invisible Wall",
                                                           "Global Mesh", "Other" };
const char* mr_label_name(int l) { return l >= 0 && l < MR_LABEL_COUNT ? g_label_names[l] : "Other"; }
int mr_label_from_runtime(const char* s) {
    static const struct { const char* k; int l; } map[] = {
        { "FLOOR", MR_LABEL_FLOOR }, { "CEILING", MR_LABEL_CEILING }, { "WALL_FACE", MR_LABEL_WALL }, { "INVISIBLE_WALL_FACE", MR_LABEL_INVISIBLE_WALL },
        { "TABLE", MR_LABEL_TABLE }, { "DESK", MR_LABEL_TABLE }, { "COUCH", MR_LABEL_COUCH }, { "BED", MR_LABEL_BED },
        { "STORAGE", MR_LABEL_STORAGE }, { "SCREEN", MR_LABEL_SCREEN }, { "WINDOW_FRAME", MR_LABEL_WINDOW }, { "DOOR_FRAME", MR_LABEL_DOOR },
        { "LAMP", MR_LABEL_LAMP }, { "PLANT", MR_LABEL_PLANT }, { "WALL_ART", MR_LABEL_WALL_ART }, { "GLOBAL_MESH", MR_LABEL_GLOBAL_MESH },
        { "OTHER", MR_LABEL_OTHER },
    };
    if (!s) return MR_LABEL_OTHER;
    const char* p = s;
    while (*p) { // (comma separated: the first one known)
        char tok[40];
        size_t n = 0;
        while (*p == ',' || *p == ' ') p++;
        while (*p && *p != ',' && n < sizeof tok - 1) tok[n++] = *p++;
        tok[n] = 0;
        while (*p && *p != ',') p++;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (n && !strcmp(tok, map[i].k)) return map[i].l;
    }
    return MR_LABEL_OTHER;
}

// --- The state ---
#define MR_MAX_WINDOWS 16
#define SIM_DEPTH 160 // (the simulated real depth: px per side, about the headset's density over the eye's field)

typedef XrResult (*PFN_xrGetSystemProperties)(XrInstance, XrSystemId, XrSystemProperties*); // (xr.c's own, not in a header)
typedef XrResult (*PFN_xrLocateSpace)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
#define XRF(name) static PFN_##name p_##name
XRF(xrGetSystemProperties); XRF(xrLocateSpace); XRF(xrDestroySpace); XRF(xrEnumerateEnvironmentBlendModes);
XRF(xrCreatePassthroughFB); XRF(xrDestroyPassthroughFB); XRF(xrPassthroughStartFB); XRF(xrPassthroughPauseFB);
XRF(xrCreatePassthroughLayerFB); XRF(xrDestroyPassthroughLayerFB); XRF(xrPassthroughLayerPauseFB); XRF(xrPassthroughLayerResumeFB);
XRF(xrPassthroughLayerSetStyleFB);
XRF(xrSetSpaceComponentStatusFB); XRF(xrGetSpaceComponentStatusFB); XRF(xrQuerySpacesFB); XRF(xrRetrieveSpaceQueryResultsFB);
XRF(xrGetSpaceBoundingBox2DFB); XRF(xrGetSpaceBoundingBox3DFB); XRF(xrGetSpaceSemanticLabelsFB); XRF(xrGetSpaceBoundary2DFB);
XRF(xrGetSpaceContainerFB); XRF(xrRequestSceneCaptureFB); XRF(xrGetSpaceTriangleMeshMETA);
XRF(xrCreateEnvironmentDepthProviderMETA); XRF(xrDestroyEnvironmentDepthProviderMETA); XRF(xrStartEnvironmentDepthProviderMETA);
XRF(xrStopEnvironmentDepthProviderMETA); XRF(xrCreateEnvironmentDepthSwapchainMETA); XRF(xrDestroyEnvironmentDepthSwapchainMETA);
XRF(xrEnumerateEnvironmentDepthSwapchainImagesMETA); XRF(xrGetEnvironmentDepthSwapchainStateMETA); XRF(xrAcquireEnvironmentDepthImageMETA);
XRF(xrSetEnvironmentDepthHandRemovalMETA);

enum { Q_IDLE = 0, Q_ROOMS, Q_ENTITIES, Q_LOCATABLE, Q_CAPTURE };

static struct {
    MrStatus st;
    MrRoom room;
    bool wanted;                   // Project Settings > VR > Mixed Reality on (or REALENGINE_MR=1): the extensions asked for
    bool ext_entity, ext_query, ext_container, ext_scene;
    // the passthrough
    XrPassthroughFB pt;
    XrPassthroughLayerFB layer;
    bool pt_failed;
    XrCompositionLayerPassthroughFB comp;
    float style_key[16];
    // the room's query
    int q;
    XrAsyncRequestIdFB q_req;
    uint32_t q_pending;
    int q_tries;
    bool capture_asked, scene_permission_asked;
    double perm_retry;
    XrSpace rooms[4];
    uint32_t nrooms;
    XrUuidEXT uuids[MR_MAX_ANCHORS];
    uint32_t nuuids;
    XrSpace spaces[MR_MAX_ANCHORS];  // the entities found (room.a[i] from spaces[i] when room_from_spaces)
    uint32_t nspaces;
    int anchor_space[MR_MAX_ANCHORS]; // room.a[i]'s space (-1: none)
    double relocate_t;
    bool room_reload;
    // the real depth
    XrEnvironmentDepthProviderMETA dprov;
    XrEnvironmentDepthSwapchainMETA dsc;
    VkImage dimg[8];
    uint32_t dcount;
    bool dstarted, dvalid;
    XrEnvironmentDepthImageMETA dlast;
    uint64_t dserial;
    // the game
    bool playing;
    uint64_t room_seen;
    bool event_due;
    Entity actors[MR_MAX_ANCHORS];
    int actor_anchor[MR_MAX_ANCHORS];
    uint32_t nactors;
    float pawn_key[4];
    Entity windows[MR_MAX_WINDOWS];
    uint32_t nwindows;
    Entity hidden_floor[64];
    uint32_t nhidden;
    uint32_t light_key;
    // the renderer's numbers (this frame's)
    float boxes[MR_MAX_BOXES][4][4];
    uint32_t nboxes;
    uint16_t* sim_depth; // (2 x SIM_DEPTH^2)
    float sim_key[2][12];
    uint64_t sim_serial;
} M;

const MrStatus* mr_status(void) { return &M.st; }
const MrRoom* mr_room(void) { return &M.room; }

static void note(const char* s) {
    snprintf(M.st.note, sizeof M.st.note, "%s", s);
    printf("[RealEngine] MR: %s\n", s);
}

// --- Math ---
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void q_rot(const XrQuaternionf* q, const float v[3], float out[3]) {
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty), out[1] = v[1] + w * ty + (z * tx - x * tz), out[2] = v[2] + w * tz + (x * ty - y * tx);
}
static void to_engine(const float v[3], float s, float o[3]) { o[0] = -v[2] * s, o[1] = v[0] * s, o[2] = v[1] * s; }
// An OpenXR pose as a frame in the play space (cm): its origin and its X, Y, Z axes in engine axes
static void pose_frame(const XrPosef* p, float pos[3], float ax[3], float ay[3], float az[3]) {
    const float pp[3] = { p->position.x, p->position.y, p->position.z }, X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    float r[3];
    to_engine(pp, 100.0f, pos);
    q_rot(&p->orientation, X, r), to_engine(r, 1.0f, ax);
    q_rot(&p->orientation, Y, r), to_engine(r, 1.0f, ay);
    q_rot(&p->orientation, Z, r), to_engine(r, 1.0f, az);
}
static void anchor_point(const MrAnchor* a, float lx, float ly, float lz, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = a->pos[k] + a->ax[k] * lx + a->ay[k] * ly + a->az[k] * lz;
}
// An anchor's box (play space): center and the three half axes (a plane: thick cm behind its face; along -az)
static bool anchor_box(const MrAnchor* a, float thick, float c[3], float h[3][3]) {
    if (a->volume) {
        float cx = (a->box_min[0] + a->box_max[0]) * 0.5f, cy = (a->box_min[1] + a->box_max[1]) * 0.5f, cz = (a->box_min[2] + a->box_max[2]) * 0.5f;
        anchor_point(a, cx, cy, cz, c);
        for (int k = 0; k < 3; k++) {
            h[0][k] = a->ax[k] * (a->box_max[0] - a->box_min[0]) * 0.5f;
            h[1][k] = a->ay[k] * (a->box_max[1] - a->box_min[1]) * 0.5f;
            h[2][k] = a->az[k] * (a->box_max[2] - a->box_min[2]) * 0.5f;
        }
        return true;
    }
    if (!a->plane) return false;
    anchor_point(a, a->rect[0] + a->rect[2] * 0.5f, a->rect[1] + a->rect[3] * 0.5f, -thick * 0.5f, c);
    for (int k = 0; k < 3; k++) h[0][k] = a->ax[k] * a->rect[2] * 0.5f, h[1][k] = a->ay[k] * a->rect[3] * 0.5f, h[2][k] = a->az[k] * thick * 0.5f;
    return true;
}
// A ray against a box (center, half axes): the entry t (-1: none, or starting inside); its normal
static float ray_box(const float o[3], const float d[3], const float c[3], const float h[3][3], float n[3]) {
    float tn = -1e30f, tf = 1e30f;
    int axis = -1;
    float sgn = 1.0f;
    for (int i = 0; i < 3; i++) {
        float l2 = dot3(h[i], h[i]);
        if (l2 < 1e-8f) return -1.0f;
        float rel[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
        float lo = dot3(rel, h[i]) / l2, ld = dot3(d, h[i]) / l2;
        if (fabsf(ld) < 1e-9f) {
            if (lo < -1.0f || lo > 1.0f) return -1.0f;
            continue;
        }
        float t0 = (-1.0f - lo) / ld, t1 = (1.0f - lo) / ld;
        float a0 = fminf(t0, t1), a1 = fmaxf(t0, t1);
        if (a0 > tn) tn = a0, axis = i, sgn = ld > 0.0f ? -1.0f : 1.0f;
        if (a1 < tf) tf = a1;
    }
    if (tf < tn || tn <= 0.0f || axis < 0) return -1.0f;
    float l = sqrtf(dot3(h[axis], h[axis]));
    for (int k = 0; k < 3; k++) n[k] = h[axis][k] / l * sgn;
    return tn;
}
static float ray_tri(const float o[3], const float d[3], const float* a, const float* b, const float* c, float n[3]) {
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] }, p[3], q[3];
    cross3(d, e2, p);
    float det = dot3(e1, p);
    if (fabsf(det) < 1e-9f) return -1.0f;
    float inv = 1.0f / det, s[3] = { o[0] - a[0], o[1] - a[1], o[2] - a[2] };
    float u = dot3(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    cross3(s, e1, q);
    float v = dot3(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    float t = dot3(e2, q) * inv;
    if (t <= 0.0f) return -1.0f;
    cross3(e1, e2, n);
    float l = sqrtf(dot3(n, n));
    if (l > 0.0f) n[0] /= l, n[1] /= l, n[2] /= l;
    if (dot3(n, d) > 0.0f) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    return t;
}

// The play space and the world (the VR pawn's: its origin and yaw)
static void play_to_world(const float p[3], float o[3]) {
    if (vr_pawn_active()) vr_pawn_to_world(p, o);
    else memcpy(o, p, 12);
}
static void play_dir_to_world(const float d[3], float o[3]) {
    if (vr_pawn_active()) vr_pawn_dir_to_world(d, o);
    else memcpy(o, d, 12);
}
static void world_to_play(const float wp[3], float o[3]) {
    float z[3] = { 0, 0, 0 }, org[3], x[3] = { 1, 0, 0 }, fx[3];
    play_to_world(z, org), play_dir_to_world(x, fx);
    float yaw = atan2f(fx[1], fx[0]);
    float d[3] = { wp[0] - org[0], wp[1] - org[1], wp[2] - org[2] };
    float c = cosf(-yaw), s = sinf(-yaw);
    o[0] = d[0] * c - d[1] * s, o[1] = d[0] * s + d[1] * c, o[2] = d[2];
}
static void world_dir_to_play(const float wd[3], float o[3]) {
    float x[3] = { 1, 0, 0 }, fx[3];
    play_dir_to_world(x, fx);
    float yaw = atan2f(fx[1], fx[0]), c = cosf(-yaw), s = sinf(-yaw);
    o[0] = wd[0] * c - wd[1] * s, o[1] = wd[0] * s + wd[1] * c, o[2] = wd[2];
}

// --- OpenXR: the extensions, the system, the session ---
static bool has(const XrExtensionProperties* p, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name)) return true;
    return false;
}
void xr_mr_pick_extensions(const void* props_, uint32_t np, const char** names, uint32_t* n, uint32_t cap) {
    const XrExtensionProperties* props = props_;
    memset(&M.st, 0, sizeof M.st);
    M.ext_entity = M.ext_query = M.ext_container = M.ext_scene = false;
    const char* env = getenv("REALENGINE_MR");
    M.wanted = project()->vr.mr_enabled || (env && env[0] == '1');
    if (!M.wanted) return; // (pure VR: none of it asked for)
#define WANT(name, flag) do { if (has(props, np, name) && *n < cap) { names[(*n)++] = name; flag = true; } } while (0)
    WANT("XR_FB_passthrough", M.st.passthrough_ext);
    WANT("XR_FB_spatial_entity", M.ext_entity);
    WANT("XR_FB_spatial_entity_query", M.ext_query);
    WANT("XR_FB_spatial_entity_container", M.ext_container);
    WANT("XR_FB_scene", M.ext_scene);
    WANT("XR_FB_scene_capture", M.st.capture_ext);
    WANT("XR_META_spatial_entity_mesh", M.st.mesh_ext);
    WANT("XR_META_environment_depth", M.st.depth_ext);
#undef WANT
    M.st.scene_ext = M.ext_entity && M.ext_query && M.ext_scene;
    printf("[RealEngine] MR: the runtime has passthrough %d, scene %d (container %d, capture %d, mesh %d), real depth %d\n", M.st.passthrough_ext,
           M.st.scene_ext, M.ext_container, M.st.capture_ext, M.st.mesh_ext, M.st.depth_ext);
}

static PFN_xrVoidFunction xget(const XrInternal* x, const char* name) {
    PFN_xrVoidFunction f = NULL;
    if (x->gipa) x->gipa(x->instance, name, &f);
    return f;
}

void xr_mr_system_ready(void) {
    if (!M.wanted) return;
    XrInternal x;
    xr_internal(&x);
#define L(n) p_##n = (PFN_##n)xget(&x, #n)
    L(xrGetSystemProperties); L(xrLocateSpace); L(xrDestroySpace); L(xrEnumerateEnvironmentBlendModes);
    if (M.st.passthrough_ext) {
        L(xrCreatePassthroughFB); L(xrDestroyPassthroughFB); L(xrPassthroughStartFB); L(xrPassthroughPauseFB); L(xrCreatePassthroughLayerFB);
        L(xrDestroyPassthroughLayerFB); L(xrPassthroughLayerPauseFB); L(xrPassthroughLayerResumeFB); L(xrPassthroughLayerSetStyleFB);
    }
    if (M.st.scene_ext) {
        L(xrSetSpaceComponentStatusFB); L(xrGetSpaceComponentStatusFB); L(xrQuerySpacesFB); L(xrRetrieveSpaceQueryResultsFB);
        L(xrGetSpaceBoundingBox2DFB); L(xrGetSpaceBoundingBox3DFB); L(xrGetSpaceSemanticLabelsFB); L(xrGetSpaceBoundary2DFB);
    }
    if (M.ext_container) L(xrGetSpaceContainerFB);
    if (M.st.capture_ext) L(xrRequestSceneCaptureFB);
    if (M.st.mesh_ext) L(xrGetSpaceTriangleMeshMETA);
    if (M.st.depth_ext) {
        L(xrCreateEnvironmentDepthProviderMETA); L(xrDestroyEnvironmentDepthProviderMETA); L(xrStartEnvironmentDepthProviderMETA);
        L(xrStopEnvironmentDepthProviderMETA); L(xrCreateEnvironmentDepthSwapchainMETA); L(xrDestroyEnvironmentDepthSwapchainMETA);
        L(xrEnumerateEnvironmentDepthSwapchainImagesMETA); L(xrGetEnvironmentDepthSwapchainStateMETA); L(xrAcquireEnvironmentDepthImageMETA);
        L(xrSetEnvironmentDepthHandRemovalMETA);
    }
#undef L
    // what the system has: the passthrough (its color), the real depth (its hand removal)
    XrSystemPassthroughProperties2FB pp2 = { XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB };
    XrSystemPassthroughPropertiesFB pp = { XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &pp2 };
    XrSystemEnvironmentDepthPropertiesMETA dp = { XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META };
    const void* chain = NULL;
    if (M.st.depth_ext) dp.next = chain, chain = &dp;
    if (M.st.passthrough_ext) pp2.next = chain, chain = &pp;
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES, (void*)chain };
    if (chain && p_xrGetSystemProperties && XR_SUCCEEDED(p_xrGetSystemProperties(x.instance, x.system, &sp))) {
        M.st.passthrough_supported = M.st.passthrough_ext && (pp.supportsPassthrough || (pp2.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB));
        M.st.passthrough_color = (pp2.capabilities & XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB) != 0;
        M.st.depth_supported = M.st.depth_ext && dp.supportsEnvironmentDepth;
        M.st.hand_removal_supported = M.st.depth_ext && dp.supportsHandRemoval;
    }
    // the ALPHA_BLEND environment (a runtime with passthrough but without the FB layer)
    XrEnvironmentBlendMode modes[8];
    uint32_t nm = 0;
    if (p_xrEnumerateEnvironmentBlendModes && XR_SUCCEEDED(p_xrEnumerateEnvironmentBlendModes(x.instance, x.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &nm, modes)))
        for (uint32_t i = 0; i < nm; i++) M.st.blend_alpha |= modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    printf("[RealEngine] MR: system: passthrough %s%s, ALPHA_BLEND %d, real depth %d (hand removal %d)\n",
           M.st.passthrough_supported ? "yes" : "no", M.st.passthrough_color ? " (color)" : "", M.st.blend_alpha, M.st.depth_supported,
           M.st.hand_removal_supported);
}

static void depth_start(const XrInternal* x) {
    if (!M.st.depth_supported || M.dprov || !p_xrCreateEnvironmentDepthProviderMETA) return;
    XrEnvironmentDepthProviderCreateInfoMETA pc = { XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META };
    if (!XR_SUCCEEDED(p_xrCreateEnvironmentDepthProviderMETA(x->session, &pc, &M.dprov))) { M.dprov = NULL; note("the real depth's provider could not start"); return; }
    XrEnvironmentDepthSwapchainCreateInfoMETA sc = { XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META };
    if (!XR_SUCCEEDED(p_xrCreateEnvironmentDepthSwapchainMETA(M.dprov, &sc, &M.dsc))) { M.dsc = NULL; note("the real depth's swapchain could not be made"); return; }
    XrEnvironmentDepthSwapchainStateMETA ss = { XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META };
    p_xrGetEnvironmentDepthSwapchainStateMETA(M.dsc, &ss);
    XrSwapchainImageVulkanKHR im[8];
    for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkanKHR){ XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR };
    M.dcount = 0;
    p_xrEnumerateEnvironmentDepthSwapchainImagesMETA(M.dsc, 8, &M.dcount, (XrSwapchainImageBaseHeader*)im);
    for (uint32_t i = 0; i < M.dcount && i < 8; i++) M.dimg[i] = im[i].image;
    M.st.depth_w = ss.width, M.st.depth_h = ss.height;
    printf("[RealEngine] MR: the real depth: %u x %u, %u images\n", ss.width, ss.height, M.dcount);
}

void xr_mr_session_ready(void) {
    if (!M.wanted) return;
    XrInternal x;
    xr_internal(&x);
    if (M.st.passthrough_supported && p_xrCreatePassthroughFB) { // made paused: started while the game asks for it
        XrPassthroughCreateInfoFB pc = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, NULL, 0 };
        if (XR_SUCCEEDED(p_xrCreatePassthroughFB(x.session, &pc, &M.pt))) {
            XrPassthroughLayerCreateInfoFB lc = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, NULL, M.pt, 0, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB };
            if (!XR_SUCCEEDED(p_xrCreatePassthroughLayerFB(x.session, &lc, &M.layer))) M.layer = NULL, note("the passthrough's layer could not be made");
        } else
            M.pt = NULL, note("the passthrough could not be made (Meta Horizon Link: Settings > Beta > Developer Runtime Features, Passthrough over Meta Quest Link)");
    }
    if (project()->vr.mr_occlusion == MR_OCC_DEPTH) depth_start(&x);
    memset(M.style_key, 0, sizeof M.style_key), M.style_key[0] = -1.0f;
    M.q = Q_IDLE;
    M.room_reload = project()->vr.mr_scene; // (queried at the first tick: Android may ask for the permission first)
}

void xr_mr_shutdown(void) {
    if (M.layer && p_xrDestroyPassthroughLayerFB) p_xrDestroyPassthroughLayerFB(M.layer);
    if (M.pt && p_xrDestroyPassthroughFB) p_xrDestroyPassthroughFB(M.pt);
    if (M.dsc && p_xrDestroyEnvironmentDepthSwapchainMETA) p_xrDestroyEnvironmentDepthSwapchainMETA(M.dsc);
    if (M.dprov && p_xrDestroyEnvironmentDepthProviderMETA) {
        if (M.dstarted && p_xrStopEnvironmentDepthProviderMETA) p_xrStopEnvironmentDepthProviderMETA(M.dprov);
        p_xrDestroyEnvironmentDepthProviderMETA(M.dprov);
    }
    for (uint32_t i = 0; i < M.nspaces; i++)
        if (M.spaces[i] && p_xrDestroySpace) p_xrDestroySpace(M.spaces[i]);
    for (uint32_t i = 0; i < M.nrooms; i++)
        if (M.rooms[i] && p_xrDestroySpace) p_xrDestroySpace(M.rooms[i]);
    M.layer = NULL, M.pt = NULL, M.dsc = NULL, M.dprov = NULL, M.dstarted = M.dvalid = false;
    M.nspaces = M.nrooms = 0, M.q = Q_IDLE;
    M.st.passthrough_running = M.st.depth_running = false;
}

// --- The passthrough's state and style (each frame: what the settings and the Blueprints ask) ---
static void style_apply(void) {
    const ProjectVr* v = &project()->vr;
    float key[16] = { v->mr_opacity, (float)v->mr_style, v->mr_brightness, v->mr_contrast, v->mr_saturation, v->mr_tint[0], v->mr_tint[1],
                      v->mr_tint[2], v->mr_edge[0], v->mr_edge[1], v->mr_edge[2], v->mr_edge[3], 0, 0, 0, 0 };
    if (!memcmp(key, M.style_key, sizeof key) || !M.layer || !p_xrPassthroughLayerSetStyleFB) return;
    memcpy(M.style_key, key, sizeof key);
    XrPassthroughStyleFB s = { XR_TYPE_PASSTHROUGH_STYLE_FB, NULL, fminf(fmaxf(v->mr_opacity, 0.0f), 1.0f), { v->mr_edge[0], v->mr_edge[1], v->mr_edge[2], v->mr_edge[3] } };
    static XrPassthroughColorMapMonoToMonoFB mono;
    static XrPassthroughColorMapMonoToRgbaFB tint;
    static XrPassthroughBrightnessContrastSaturationFB bcs;
    if (v->mr_style == MR_STYLE_MONO) {
        mono = (XrPassthroughColorMapMonoToMonoFB){ XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_MONO_FB };
        for (int i = 0; i < 256; i++) mono.textureColorMap[i] = (uint8_t)i;
        s.next = &mono;
    } else if (v->mr_style == MR_STYLE_TINT) {
        tint = (XrPassthroughColorMapMonoToRgbaFB){ XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB };
        for (int i = 0; i < 256; i++) {
            float l = (float)i / 255.0f * 1.6f;
            tint.textureColorMap[i] = (XrColor4f){ fminf(v->mr_tint[0] * l, 1.0f), fminf(v->mr_tint[1] * l, 1.0f), fminf(v->mr_tint[2] * l, 1.0f), 1.0f };
        }
        s.next = &tint;
    } else if (v->mr_style == MR_STYLE_ADJUSTED) {
        bcs = (XrPassthroughBrightnessContrastSaturationFB){ XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB, NULL,
                                                             fminf(fmaxf(v->mr_brightness, -100.0f), 100.0f), fmaxf(v->mr_contrast, 0.0f), fmaxf(v->mr_saturation, 0.0f) };
        s.next = &bcs;
    }
    XrResult r = p_xrPassthroughLayerSetStyleFB(M.layer, &s);
    if (!XR_SUCCEEDED(r)) printf("[RealEngine] MR: the passthrough's style was refused (%d)\n", (int)r);
}

static void passthrough_apply(void) {
    XrInternal x;
    xr_internal(&x);
    const ProjectVr* v = &project()->vr;
    bool want = M.wanted && v->mr_passthrough && x.running && !M.pt_failed;
    if (M.pt && M.layer) {
        if (want && !M.st.passthrough_running) {
            bool ok = XR_SUCCEEDED(p_xrPassthroughStartFB(M.pt)) && XR_SUCCEEDED(p_xrPassthroughLayerResumeFB(M.layer));
            M.st.passthrough_running = ok;
            note(ok ? "the passthrough on" : "the passthrough could not start");
            if (!ok) M.pt_failed = true;
            M.style_key[0] = -1.0f;
        } else if (!want && M.st.passthrough_running) {
            p_xrPassthroughLayerPauseFB(M.layer), p_xrPassthroughPauseFB(M.pt);
            M.st.passthrough_running = false;
            note("the passthrough off");
        }
        if (M.st.passthrough_running) style_apply();
    } else
        M.st.passthrough_running = want && M.st.blend_alpha; // (the ALPHA_BLEND environment: no style)
    // the real depth runs with the passthrough (its cameras)
    bool dwant = want && v->mr_occlusion == MR_OCC_DEPTH;
    if (dwant && !M.dprov) depth_start(&x);
    if (M.dprov) {
        if (dwant && !M.dstarted) {
            if (p_xrSetEnvironmentDepthHandRemovalMETA && M.st.hand_removal_supported) {
                XrEnvironmentDepthHandRemovalSetInfoMETA hr = { XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META, NULL, v->mr_hand_removal };
                p_xrSetEnvironmentDepthHandRemovalMETA(M.dprov, &hr);
            }
            M.dstarted = XR_SUCCEEDED(p_xrStartEnvironmentDepthProviderMETA(M.dprov));
            if (!M.dstarted) note("the real depth could not start (Android: the Scene permission)");
        } else if (!dwant && M.dstarted)
            p_xrStopEnvironmentDepthProviderMETA(M.dprov), M.dstarted = false, M.dvalid = false;
    }
    M.st.depth_running = M.dstarted;
}

const void* xr_mr_compose(XrFlags64* flags, int* blend) {
    *flags = 0;
    if (!M.st.passthrough_running) return NULL;
    *flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // (the eyes' alpha: premultiplied, composite.frag)
    if (project()->vr.mr_unpremultiplied) *flags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    if (M.layer) {
        M.comp = (XrCompositionLayerPassthroughFB){ XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, NULL, M.layer };
        return &M.comp;
    }
    *blend = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    return NULL;
}

void xr_mr_frame(void) {
    if (!M.wanted) return;
    XrInternal x;
    xr_internal(&x);
    if (!M.dstarted || !p_xrAcquireEnvironmentDepthImageMETA) return;
    XrEnvironmentDepthImageAcquireInfoMETA ai = { XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META, NULL, x.space, x.display_time };
    XrEnvironmentDepthImageMETA im = { XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META };
    im.views[0].type = im.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
    XrResult r = p_xrAcquireEnvironmentDepthImageMETA(M.dprov, &ai, &im);
    if (r == 0 && im.swapchainIndex < M.dcount) M.dlast = im, M.dvalid = true, M.dserial++, M.st.depth_frames++;
}

// --- The room: the queries (XR_FB_spatial_entity_query), its entities read into anchors ---
static bool query(const XrSpaceFilterInfoBaseHeaderFB* filter, uint32_t max, int stage) {
    XrInternal x;
    xr_internal(&x);
    XrSpaceQueryInfoFB qi = { XR_TYPE_SPACE_QUERY_INFO_FB, NULL, XR_SPACE_QUERY_ACTION_LOAD_FB, max, 0, filter, NULL };
    XrResult r = p_xrQuerySpacesFB ? p_xrQuerySpacesFB(x.session, &qi, &M.q_req) : -1;
    if (!XR_SUCCEEDED(r)) {
        printf("[RealEngine] MR: xrQuerySpacesFB refused (%d)\n", (int)r);
        return false;
    }
    M.q = stage;
    return true;
}
static void query_rooms(void) {
    if (M.room.simulated) return;
    if (!M.st.scene_ext) { note("no room: the runtime has no Scene (Meta Horizon Link: Developer Runtime Features, Spatial Data over Meta Quest Link)"); return; }
#ifdef __ANDROID__
    char r[8] = "";
    re_android_call_string("hasScenePermission", "()Ljava/lang/String;", r, sizeof r);
    if (r[0] != '1') { // (com.oculus.permission.USE_SCENE: asked once, the query waits for it)
        if (!M.scene_permission_asked) re_android_call_void("requestScenePermission", "()V"), M.scene_permission_asked = true;
        M.room_reload = true, M.perm_retry = re_time_seconds() + 1.0;
        return;
    }
#endif
    for (uint32_t i = 0; i < M.nrooms; i++)
        if (M.rooms[i] && p_xrDestroySpace) p_xrDestroySpace(M.rooms[i]);
    M.nrooms = 0;
    // (as Meta's samples: the rooms stored on the headset, by their ROOM_LAYOUT component)
    static XrSpaceStorageLocationFilterInfoFB loc;
    static XrSpaceComponentFilterInfoFB f;
    loc = (XrSpaceStorageLocationFilterInfoFB){ XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, NULL, XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
    f = (XrSpaceComponentFilterInfoFB){ XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, &loc, XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB };
    if (query((const XrSpaceFilterInfoBaseHeaderFB*)&f, 8, Q_ROOMS)) M.st.room_loading = true, M.st.room_none = false;
    else if (++M.q_tries < 4) M.room_reload = true, M.perm_retry = re_time_seconds() + 2.0; // (tried again: the session may not be focused yet)
    else note("the room's query was refused (Meta Horizon Link: Spatial Data over Meta Quest Link; the headset: the Space Setup done)");
}
static void retrieve(void) {
    XrInternal x;
    xr_internal(&x);
    static XrSpaceQueryResultFB res[MR_MAX_ANCHORS];
    XrSpaceQueryResultsFB qr = { XR_TYPE_SPACE_QUERY_RESULTS_FB, NULL, 0, 0, NULL };
    if (!XR_SUCCEEDED(p_xrRetrieveSpaceQueryResultsFB(x.session, M.q_req, &qr))) return;
    uint32_t n = qr.resultCountOutput < MR_MAX_ANCHORS ? qr.resultCountOutput : MR_MAX_ANCHORS;
    qr.resultCapacityInput = n, qr.results = res;
    if (!n || !XR_SUCCEEDED(p_xrRetrieveSpaceQueryResultsFB(x.session, M.q_req, &qr))) return;
    for (uint32_t i = 0; i < qr.resultCountOutput && i < n; i++) {
        if (M.q == Q_ROOMS && M.nrooms < 4) M.rooms[M.nrooms++] = res[i].space;
        else if (M.q == Q_ENTITIES && M.nspaces < MR_MAX_ANCHORS) M.spaces[M.nspaces++] = res[i].space;
        else if (p_xrDestroySpace) p_xrDestroySpace(res[i].space);
    }
}

static void read_anchor(const XrInternal* x, XrSpace sp, MrAnchor* a) {
    memset(a, 0, sizeof *a);
    a->label = MR_LABEL_OTHER;
    // the labels (the newer ones asked for: desks as tables, the invisible walls)
    static const char recognized[] = "TABLE,COUCH,FLOOR,CEILING,WALL_FACE,WINDOW_FRAME,DOOR_FRAME,STORAGE,BED,SCREEN,LAMP,PLANT,WALL_ART,GLOBAL_MESH,INVISIBLE_WALL_FACE,OTHER";
    XrSemanticLabelsSupportInfoFB si = { XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB, NULL,
                                         XR_SEMANTIC_LABELS_SUPPORT_MULTIPLE_SEMANTIC_LABELS_BIT_FB | XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB |
                                             XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_INVISIBLE_WALL_FACE_BIT_FB, recognized };
    char buf[256] = "";
    XrSemanticLabelsFB sl = { XR_TYPE_SEMANTIC_LABELS_FB, &si, 0, 0, NULL };
    if (XR_SUCCEEDED(p_xrGetSpaceSemanticLabelsFB(x->session, sp, &sl)) && sl.bufferCountOutput) {
        sl.bufferCapacityInput = sl.bufferCountOutput < sizeof buf ? sl.bufferCountOutput : (uint32_t)sizeof buf - 1, sl.buffer = buf;
        if (XR_SUCCEEDED(p_xrGetSpaceSemanticLabelsFB(x->session, sp, &sl))) buf[sl.bufferCapacityInput < sizeof buf ? sl.bufferCapacityInput : sizeof buf - 1] = 0;
    }
    snprintf(a->labels, sizeof a->labels, "%s", buf);
    a->label = mr_label_from_runtime(buf);
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(p_xrLocateSpace(sp, x->space, x->display_time, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        pose_frame(&loc.pose, a->pos, a->ax, a->ay, a->az);
    else
        a->ax[0] = a->ay[1] = a->az[2] = 1.0f;
    XrRect2Df r2;
    if (XR_SUCCEEDED(p_xrGetSpaceBoundingBox2DFB(x->session, sp, &r2))) {
        a->plane = true;
        a->rect[0] = r2.offset.x * 100.0f, a->rect[1] = r2.offset.y * 100.0f, a->rect[2] = r2.extent.width * 100.0f, a->rect[3] = r2.extent.height * 100.0f;
        XrVector2f pv[MR_MAX_POLY];
        XrBoundary2DFB b = { XR_TYPE_BOUNDARY_2D_FB, NULL, 0, 0, NULL };
        if (XR_SUCCEEDED(p_xrGetSpaceBoundary2DFB(x->session, sp, &b)) && b.vertexCountOutput && b.vertexCountOutput <= MR_MAX_POLY) {
            b.vertexCapacityInput = b.vertexCountOutput, b.vertices = pv;
            if (XR_SUCCEEDED(p_xrGetSpaceBoundary2DFB(x->session, sp, &b)))
                for (uint32_t i = 0; i < b.vertexCountOutput; i++) a->poly[i][0] = pv[i].x * 100.0f, a->poly[i][1] = pv[i].y * 100.0f, a->npoly = i + 1;
        }
    }
    XrRect3DfFB r3;
    if (XR_SUCCEEDED(p_xrGetSpaceBoundingBox3DFB(x->session, sp, &r3))) {
        a->volume = true;
        a->box_min[0] = r3.offset.x * 100.0f, a->box_min[1] = r3.offset.y * 100.0f, a->box_min[2] = r3.offset.z * 100.0f;
        a->box_max[0] = a->box_min[0] + r3.extent.width * 100.0f, a->box_max[1] = a->box_min[1] + r3.extent.height * 100.0f;
        a->box_max[2] = a->box_min[2] + r3.extent.depth * 100.0f;
    }
}

// The room's mesh (XR_META_spatial_entity_mesh) into the play space
static void read_mesh(const XrInternal* x, XrSpace sp, const MrAnchor* a) {
    if (!p_xrGetSpaceTriangleMeshMETA || !project()->vr.mr_room_mesh) return;
    XrSpaceTriangleMeshGetInfoMETA gi = { XR_TYPE_SPACE_TRIANGLE_MESH_GET_INFO_META };
    XrSpaceTriangleMeshMETA tm = { XR_TYPE_SPACE_TRIANGLE_MESH_META };
    if (!XR_SUCCEEDED(p_xrGetSpaceTriangleMeshMETA(sp, &gi, &tm)) || !tm.vertexCountOutput || !tm.indexCountOutput) return;
    XrVector3f* v = malloc(sizeof *v * tm.vertexCountOutput);
    uint32_t* idx = malloc(sizeof *idx * tm.indexCountOutput);
    float* out = malloc(sizeof(float) * 3u * tm.vertexCountOutput);
    if (!v || !idx || !out) { free(v), free(idx), free(out); return; }
    tm.vertexCapacityInput = tm.vertexCountOutput, tm.vertices = v, tm.indexCapacityInput = tm.indexCountOutput, tm.indices = idx;
    if (!XR_SUCCEEDED(p_xrGetSpaceTriangleMeshMETA(sp, &gi, &tm))) { free(v), free(idx), free(out); return; }
    for (uint32_t i = 0; i < tm.vertexCountOutput; i++) anchor_point(a, v[i].x * 100.0f, v[i].y * 100.0f, v[i].z * 100.0f, out + 3 * i);
    free(v);
    free(M.room.mesh_v), free(M.room.mesh_i);
    M.room.mesh_v = out, M.room.mesh_nv = tm.vertexCountOutput, M.room.mesh_i = idx, M.room.mesh_ni = tm.indexCountOutput - tm.indexCountOutput % 3u;
}

static void read_room(void) {
    XrInternal x;
    xr_internal(&x);
    M.room.n = 0;
    free(M.room.mesh_v), free(M.room.mesh_i);
    M.room.mesh_v = NULL, M.room.mesh_i = NULL, M.room.mesh_nv = M.room.mesh_ni = 0;
    for (uint32_t i = 0; i < M.nspaces && M.room.n < MR_MAX_ANCHORS; i++) {
        MrAnchor* a = &M.room.a[M.room.n];
        read_anchor(&x, M.spaces[i], a);
        if (a->label == MR_LABEL_GLOBAL_MESH) { read_mesh(&x, M.spaces[i], a); continue; }
        if (!a->plane && !a->volume) continue;
        M.anchor_space[M.room.n++] = (int)i;
    }
    M.room.serial++, M.q = Q_IDLE;
    M.st.room_loading = false, M.st.room_loaded = M.room.n > 0, M.st.room_none = M.room.n == 0;
    char t[160];
    snprintf(t, sizeof t, "the room: %u planes and volumes, its mesh %u triangles", M.room.n, M.room.mesh_ni / 3u);
    note(t);
    M.event_due = true;
}

// a query's end: the rooms -> their entities (by the rooms' containers) -> locatable -> read
static void query_complete(void) {
    if (M.q == Q_ROOMS) {
        if (!M.nrooms) {
            M.q = Q_IDLE, M.st.room_loading = false, M.st.room_none = true;
            if (project()->vr.mr_ask_scan && !M.capture_asked && mr_request_room_setup()) return;
            note("no room: the headset has no Space Setup (Request Room Setup opens it)");
            M.event_due = true;
            return;
        }
        M.nuuids = 0;
        for (uint32_t r = 0; r < M.nrooms && p_xrGetSpaceContainerFB; r++) {
            XrInternal x;
            xr_internal(&x);
            XrSpaceContainerFB c = { XR_TYPE_SPACE_CONTAINER_FB, NULL, 0, 0, NULL };
            if (!XR_SUCCEEDED(p_xrGetSpaceContainerFB(x.session, M.rooms[r], &c)) || !c.uuidCountOutput) continue;
            uint32_t room_left = MR_MAX_ANCHORS - M.nuuids;
            c.uuidCapacityInput = c.uuidCountOutput < room_left ? c.uuidCountOutput : room_left, c.uuids = M.uuids + M.nuuids;
            if (XR_SUCCEEDED(p_xrGetSpaceContainerFB(x.session, M.rooms[r], &c))) M.nuuids += c.uuidCapacityInput < c.uuidCountOutput ? c.uuidCapacityInput : c.uuidCountOutput;
        }
        for (uint32_t i = 0; i < M.nspaces; i++)
            if (M.spaces[i] && p_xrDestroySpace) p_xrDestroySpace(M.spaces[i]);
        M.nspaces = 0;
        static XrSpaceUuidFilterInfoFB uf;
        static XrSpaceComponentFilterInfoFB cf;
        bool ok;
        if (M.nuuids) {
            static XrSpaceStorageLocationFilterInfoFB ul;
            ul = (XrSpaceStorageLocationFilterInfoFB){ XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB, NULL, XR_SPACE_STORAGE_LOCATION_LOCAL_FB };
            uf = (XrSpaceUuidFilterInfoFB){ XR_TYPE_SPACE_UUID_FILTER_INFO_FB, &ul, M.nuuids, M.uuids };
            ok = query((const XrSpaceFilterInfoBaseHeaderFB*)&uf, M.nuuids, Q_ENTITIES);
        } else { // (no container: every entity with labels)
            cf = (XrSpaceComponentFilterInfoFB){ XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB, NULL, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB };
            ok = query((const XrSpaceFilterInfoBaseHeaderFB*)&cf, MR_MAX_ANCHORS, Q_ENTITIES);
        }
        if (!ok) M.q = Q_IDLE, M.st.room_loading = false, note("the room's entities could not be queried");
        return;
    }
    if (M.q == Q_ENTITIES) { // each made locatable (Space Setup's usually are), its mesh component on
        M.q_pending = 0;
        for (uint32_t i = 0; i < M.nspaces; i++) {
            const XrSpaceComponentTypeFB comps[2] = { XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, XR_SPACE_COMPONENT_TYPE_TRIANGLE_MESH_META };
            for (int k = 0; k < (M.st.mesh_ext ? 2 : 1); k++) {
                XrSpaceComponentStatusFB cs = { XR_TYPE_SPACE_COMPONENT_STATUS_FB };
                if (!XR_SUCCEEDED(p_xrGetSpaceComponentStatusFB(M.spaces[i], comps[k], &cs)) || cs.enabled) continue; // (unsupported, or on)
                XrSpaceComponentStatusSetInfoFB set = { XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB, NULL, comps[k], 1, 0 };
                XrAsyncRequestIdFB rq;
                if (!cs.changePending && XR_SUCCEEDED(p_xrSetSpaceComponentStatusFB(M.spaces[i], &set, &rq))) M.q_pending++;
            }
        }
        if (M.q_pending) M.q = Q_LOCATABLE;
        else read_room();
    }
}

void xr_mr_event(const void* evp) {
    if (!M.wanted) return;
    const XrEventDataBuffer* ev = evp;
    switch ((int)ev->type) {
    case (int)XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB: {
        const XrEventDataPassthroughStateChangedFB* e = evp;
        if (e->flags & XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB) M.pt_failed = true, M.st.passthrough_running = false, note("the passthrough failed (not recoverable)");
        else if (e->flags & XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB) note("the passthrough paused by the headset (it comes back)");
        else if (e->flags & XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB) note("the passthrough is back");
        break;
    }
    case (int)XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB: {
        const XrEventDataSpaceQueryResultsAvailableFB* e = evp;
        if (e->requestId == M.q_req && (M.q == Q_ROOMS || M.q == Q_ENTITIES)) retrieve();
        break;
    }
    case (int)XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB: {
        const XrEventDataSpaceQueryCompleteFB* e = evp;
        if (e->requestId == M.q_req) query_complete();
        break;
    }
    case (int)XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB:
        if (M.q == Q_LOCATABLE && M.q_pending && --M.q_pending == 0) read_room();
        break;
    case (int)XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB: {
        const XrEventDataSceneCaptureCompleteFB* e = evp;
        note(XR_SUCCEEDED(e->result) ? "the Space Setup ended: the room loaded again" : "the Space Setup was cancelled");
        M.q = Q_IDLE;
        M.room_reload = true;
        break;
    }
    default: break;
    }
}

// --- The simulated room (--vr-sim): 5 x 4 m, 2.6 m high, a table, a couch, a window, a door, its mesh ---
static void sim_anchor(int label, const char* labels, const float pos[3], const float ax[3], const float ay[3], const float az[3]) {
    MrAnchor* a = &M.room.a[M.room.n++];
    memset(a, 0, sizeof *a);
    a->label = label;
    snprintf(a->labels, sizeof a->labels, "%s", labels);
    memcpy(a->pos, pos, 12), memcpy(a->ax, ax, 12), memcpy(a->ay, ay, 12), memcpy(a->az, az, 12);
    a->uuid[0] = (uint8_t)M.room.n, a->uuid[15] = 0x5E;
}
static void sim_plane(int label, const char* labels, const float pos[3], const float ax[3], const float ay[3], float w, float h) {
    float az[3];
    cross3(ay, ax, az); // (OpenXR's right-handed X x Y = Z, mirrored into the engine's axes: Y x X)
    sim_anchor(label, labels, pos, ax, ay, az);
    MrAnchor* a = &M.room.a[M.room.n - 1];
    a->plane = true, a->rect[0] = -w * 0.5f, a->rect[1] = -h * 0.5f, a->rect[2] = w, a->rect[3] = h;
    const float c[4][2] = { { -w * 0.5f, -h * 0.5f }, { w * 0.5f, -h * 0.5f }, { w * 0.5f, h * 0.5f }, { -w * 0.5f, h * 0.5f } };
    a->npoly = 4;
    memcpy(a->poly, c, sizeof c);
}
static void sim_volume(int label, const char* labels, float cx, float cy, float w, float d, float hgt) {
    const float pos[3] = { cx, cy, hgt }, ax[3] = { 1, 0, 0 }, ay[3] = { 0, 1, 0 }, az[3] = { 0, 0, 1 };
    sim_plane(label, labels, pos, ax, ay, w, d); // (the top face as Meta's: a plane and a volume below it)
    MrAnchor* a = &M.room.a[M.room.n - 1];
    memcpy(a->az, az, 12); // (up: into the room)
    a->volume = true;
    a->box_min[0] = -w * 0.5f, a->box_min[1] = -d * 0.5f, a->box_min[2] = -hgt;
    a->box_max[0] = w * 0.5f, a->box_max[1] = d * 0.5f, a->box_max[2] = 0.0f;
}
static void sim_room(void) {
    M.room.n = 0;
    free(M.room.mesh_v), free(M.room.mesh_i);
    M.room.mesh_v = NULL, M.room.mesh_i = NULL, M.room.mesh_nv = M.room.mesh_ni = 0;
    const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 }, nX[3] = { -1, 0, 0 }, nY[3] = { 0, -1, 0 };
    // x from -100 to 400 (the player at the origin looks 4 m in), y from -200 to 200
    { const float p[3] = { 150, 0, 0 }; sim_plane(MR_LABEL_FLOOR, "FLOOR", p, Y, X, 400, 500); }      // (Y x X = -Z: down... fixed below)
    M.room.a[0].az[0] = 0, M.room.a[0].az[1] = 0, M.room.a[0].az[2] = 1;                              // the floor faces up
    { const float p[3] = { 150, 0, 260 }; sim_plane(MR_LABEL_CEILING, "CEILING", p, X, Y, 500, 400); } // faces down
    { const float p[3] = { 400, 0, 130 }; sim_plane(MR_LABEL_WALL, "WALL_FACE", p, Y, Z, 400, 260); }  // the far wall, faces -X
    { const float p[3] = { -100, 0, 130 }; sim_plane(MR_LABEL_WALL, "WALL_FACE", p, nY, Z, 400, 260); } // behind, faces +X
    { const float p[3] = { 150, -200, 130 }; sim_plane(MR_LABEL_WALL, "WALL_FACE", p, nX, Z, 500, 260); } // left, faces +Y
    { const float p[3] = { 150, 200, 130 }; sim_plane(MR_LABEL_WALL, "WALL_FACE", p, X, Z, 500, 260); }  // right, faces -Y
    sim_volume(MR_LABEL_TABLE, "TABLE", 150, 60, 120, 70, 75);
    sim_volume(MR_LABEL_COUCH, "COUCH", 340, -60, 90, 200, 85);
    { const float p[3] = { 150, -199, 140 }; sim_plane(MR_LABEL_WINDOW, "WINDOW_FRAME", p, nX, Z, 120, 100); }
    { const float p[3] = { 30, 199, 100 }; sim_plane(MR_LABEL_DOOR, "DOOR_FRAME", p, X, Z, 90, 200); }
    // every az into the room (the walls' cross products above point there; checked against the room's middle)
    const float mid[3] = { 150, 0, 130 };
    for (uint32_t i = 0; i < M.room.n; i++) {
        MrAnchor* a = &M.room.a[i];
        if (a->volume) continue;
        float to[3] = { mid[0] - a->pos[0], mid[1] - a->pos[1], mid[2] - a->pos[2] };
        if (dot3(to, a->az) < 0.0f) a->az[0] = -a->az[0], a->az[1] = -a->az[1], a->az[2] = -a->az[2];
    }
    // the mesh: every plane and volume as triangles (the global mesh's stand-in)
    uint32_t nb = 0;
    for (uint32_t i = 0; i < M.room.n; i++) nb += M.room.a[i].volume ? 1u : M.room.a[i].label == MR_LABEL_WINDOW || M.room.a[i].label == MR_LABEL_DOOR ? 0u : 1u;
    M.room.mesh_v = malloc(sizeof(float) * 3u * 8u * nb), M.room.mesh_i = malloc(sizeof(uint32_t) * 36u * nb);
    if (M.room.mesh_v && M.room.mesh_i)
        for (uint32_t i = 0; i < M.room.n; i++) {
            const MrAnchor* a = &M.room.a[i];
            if (!a->volume && (a->label == MR_LABEL_WINDOW || a->label == MR_LABEL_DOOR)) continue;
            float c[3], h[3][3];
            if (!anchor_box(a, 2.0f, c, h)) continue;
            uint32_t v0 = M.room.mesh_nv;
            for (int k = 0; k < 8; k++) {
                float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f, sz = (k & 4) ? 1.0f : -1.0f;
                for (int j = 0; j < 3; j++) M.room.mesh_v[3 * (v0 + k) + j] = c[j] + h[0][j] * sx + h[1][j] * sy + h[2][j] * sz;
            }
            static const uint8_t f[36] = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
            for (int k = 0; k < 36; k++) M.room.mesh_i[M.room.mesh_ni++] = v0 + f[k];
            M.room.mesh_nv += 8;
        }
    M.room.simulated = true;
    M.room.serial++;
    M.st.room_loaded = true, M.st.room_loading = M.st.room_none = false;
    M.event_due = true;
}

// --- The game ---
bool mr_on(void) {
    const XrInfo* xi = xr_info();
    return M.playing && project()->vr.mr_passthrough && (xi->sim ? project()->vr.mr_enabled : M.st.passthrough_running);
}
bool mr_passthrough_active(void) { return mr_on(); }
void mr_set_passthrough(bool on) { project()->vr.mr_passthrough = on; }

void mr_begin_play(World* w) {
    (void)w;
    const XrInfo* xi = xr_info();
    M.playing = true;
    M.nactors = 0, M.nwindows = 0, M.nhidden = 0, M.light_key = 0, M.event_due = false;
    memset(M.pawn_key, 0, sizeof M.pawn_key);
    M.room_seen = M.room.serial;
    if (xi->sim) M.st.passthrough_supported = M.st.passthrough_color = true; // (the stand-in room: made at the first tick that wants it)
    if (M.room.n) M.event_due = true, M.room_seen = 0; // (a level loaded again: the actors made again)
}

// The room's actors: one per plane and volume ("MR Floor", "MR Wall 2"...): a hidden cube that collides (the physics'
// statics, the Characters' boxes); placed from the play space into the world each time the pawn's play space moves
static uint32_t g_cube_mesh_size_ok;
static float g_cube_half[3] = { 50, 50, 50 };
static void actor_place(World* w, Entity e, const MrAnchor* a) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t) return;
    float c[3], h[3][3];
    if (!anchor_box(a, 4.0f, c, h)) return;
    if (!g_cube_mesh_size_ok) {
        float mn[3], mx[3];
        render_mesh_bounds(SHAPE_CUBE, mn, mx);
        for (int k = 0; k < 3; k++)
            if (mx[k] > mn[k]) g_cube_half[k] = (mx[k] - mn[k]) * 0.5f;
        g_cube_mesh_size_ok = 1;
    }
    // the frame (proper: X, Y, X x Y) and the half sizes along it
    float fx[3], fy[3], fz[3], R[3][3], len[3];
    for (int i = 0; i < 3; i++) len[i] = sqrtf(dot3(h[i], h[i]));
    for (int k = 0; k < 3; k++) fx[k] = len[0] > 0 ? h[0][k] / len[0] : (k == 0), fy[k] = len[1] > 0 ? h[1][k] / len[1] : (k == 1);
    cross3(fx, fy, fz);
    play_dir_to_world(fx, R[0]), play_dir_to_world(fy, R[1]), play_dir_to_world(fz, R[2]);
    play_to_world(c, t->location);
    axes_rotator(R, t->rotation);
    for (int k = 0; k < 3; k++) t->scale[k] = fmaxf(len[k], 0.5f) / g_cube_half[k];
}
static void actors_sync(World* w) {
    const ProjectVr* v = &project()->vr;
    bool want = v->mr_scene_actors;
    // the old ones gone (a new room, or the actors turned off)
    for (uint32_t i = 0; i < M.nactors; i++)
        if (ecs_alive(w, M.actors[i])) ecs_destroy(w, M.actors[i]);
    M.nactors = 0;
    if (!want) { bp_boxes_changed(); return; }
    int count[MR_LABEL_COUNT] = { 0 };
    for (uint32_t i = 0; i < M.room.n && M.nactors < MR_MAX_ANCHORS; i++) {
        const MrAnchor* a = &M.room.a[i];
        if (a->label == MR_LABEL_GLOBAL_MESH || a->label == MR_LABEL_WINDOW || a->label == MR_LABEL_DOOR || a->label == MR_LABEL_WALL_ART) continue;
        if (!a->plane && !a->volume) continue;
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        if (e == ENTITY_NONE) break;
        CName* n = ecs_get(w, e, COMP_NAME);
        count[a->label]++;
        if (n) snprintf(n->text, RE_NAME_MAX, count[a->label] > 1 ? "MR %s %d" : "MR %s", mr_label_name(a->label), count[a->label]);
        CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
        if (sm) memset(sm, 0, sizeof *sm), sm->mesh = SHAPE_CUBE, sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.5f, sm->roughness = 0.8f;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (t) t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_RENDER | TRANSFORM_NO_ICON;
        actor_place(w, e, a);
        M.actors[M.nactors] = e, M.actor_anchor[M.nactors++] = (int)i;
    }
    w->revision++; // (the physics gathers its statics again)
    bp_boxes_changed();
}
static void actors_follow(World* w) { // (the pawn's play space moved: a teleport, a turn)
    for (uint32_t i = 0; i < M.nactors; i++)
        if (ecs_alive(w, M.actors[i])) actor_place(w, M.actors[i], &M.room.a[M.actor_anchor[i]]);
    if (M.nactors) w->revision++, bp_boxes_changed();
}

// The level turned to the room: the longest wall along the world's X or Y (the collision boxes exact, the level's walls
// where the room's are)
static void align_to_room(World* w) {
    const MrAnchor* best = NULL;
    for (uint32_t i = 0; i < M.room.n; i++)
        if (M.room.a[i].label == MR_LABEL_WALL && M.room.a[i].plane && (!best || M.room.a[i].rect[2] > best->rect[2])) best = &M.room.a[i];
    if (!best || !vr_pawn_active()) return;
    float d[3] = { best->ax[0], best->ax[1], 0.0f }, wd[3];
    if (fabsf(d[0]) + fabsf(d[1]) < 1e-3f) return;
    play_dir_to_world(d, wd);
    float yaw = atan2f(wd[1], wd[0]) * 57.29578f;
    float delta = roundf(yaw / 90.0f) * 90.0f - yaw;
    if (fabsf(delta) > 0.05f) {
        vr_pawn_turn_play_space(w, delta);
        printf("[RealEngine] MR: the level turned %.1f degrees to the room's walls\n", delta);
    }
}

// The room's light (Project Settings > VR > Mixed Reality > Lighting: Room): the sun from above and warm, the sky light
// low, at the exposure set: the virtual things lit as a room's (the passthrough's own picture can't be read)
static void room_light(World* w, bool on) {
    const ProjectVr* v = &project()->vr;
    uint32_t key = on && v->mr_light == MR_LIGHT_ROOM ? 1u + (uint32_t)(fmaxf(v->mr_ev100, 0.0f) * 100.0f) : 0u;
    if (key == M.light_key) return;
    M.light_key = key;
    if (!key) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_LIGHT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CLight* li = ecs_arch_column(a, COMP_LIGHT);
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            if (li[i].type == LIGHT_DIRECTIONAL) {
                li[i].color[0] = 1.0f, li[i].color[1] = 0.88f, li[i].color[2] = 0.74f; // (~3800 K: a room's lamps)
                li[i].intensity = 10.0f + 110000.0f * powf(2.0f, (v->mr_ev100 > 0.0f ? v->mr_ev100 : 7.0f) - 15.0f) * 0.65f;
                tr[i].rotation[0] = -72.0f, tr[i].rotation[1] = 30.0f; // (from above: the shadows under the things)
            } else if (li[i].type == LIGHT_SKY) {
                li[i].color[0] = 1.0f, li[i].color[1] = 0.95f, li[i].color[2] = 0.9f;
                li[i].intensity = 0.35f;
            }
        }
    }
}

// The level's floor planes hidden (still colliding) while the real floor is seen
static void level_floor(World* w, bool hide) {
    if (!hide) {
        for (uint32_t i = 0; i < M.nhidden; i++) {
            CTransform* t = ecs_alive(w, M.hidden_floor[i]) ? ecs_get(w, M.hidden_floor[i], COMP_TRANSFORM) : NULL;
            if (t) t->flags &= ~TRANSFORM_NO_RENDER;
        }
        M.nhidden = 0;
        return;
    }
    if (M.nhidden) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && M.nhidden < 64; i++)
            if (sm[i].mesh == SHAPE_PLANE && !(tr[i].flags & TRANSFORM_NO_RENDER)) tr[i].flags |= TRANSFORM_NO_RENDER, M.hidden_floor[M.nhidden++] = ents[i];
    }
}

void mr_tick(World* w, float dt) {
    (void)dt;
    if (!M.playing) return;
    double t0 = re_time_seconds();
    const ProjectVr* v = &project()->vr;
    const XrInfo* xi = xr_info();
    if (xi->sim) { // the stand-in room once mixed reality is on (a console variable may turn it on after the start)
        if (v->mr_enabled && v->mr_scene && !M.room.n) sim_room();
    } else {
        passthrough_apply();
        if (M.room_reload && M.q == Q_IDLE && re_time_seconds() >= M.perm_retry && v->mr_scene) M.room_reload = false, query_rooms();
        // the anchors located again every 2 s (the play space recentered): the room moves with it
        if (M.room.n && !M.room.simulated && t0 - M.relocate_t > 2.0) {
            M.relocate_t = t0;
            XrInternal x;
            xr_internal(&x);
            bool moved = false;
            for (uint32_t i = 0; i < M.room.n && p_xrLocateSpace; i++) {
                int s = M.anchor_space[i];
                if (s < 0 || (uint32_t)s >= M.nspaces) continue;
                XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
                if (!XR_SUCCEEDED(p_xrLocateSpace(M.spaces[s], x.space, x.display_time, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) continue;
                MrAnchor* a = &M.room.a[i];
                float pos[3];
                const float pp[3] = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
                to_engine(pp, 100.0f, pos);
                if (fabsf(pos[0] - a->pos[0]) + fabsf(pos[1] - a->pos[1]) + fabsf(pos[2] - a->pos[2]) > 1.0f)
                    pose_frame(&loc.pose, a->pos, a->ax, a->ay, a->az), moved = true;
            }
            if (moved) M.room.serial++, M.event_due = false; // (the same room: its actors moved, no event)
        }
    }
    bool on = mr_on();
    if (M.room.serial != M.room_seen) {
        bool first = M.room_seen == 0 || M.event_due;
        M.room_seen = M.room.serial;
        if (v->mr_align && first) align_to_room(w);
        actors_sync(w);
        float z[3] = { 0, 0, 0 }, o[3], x[3] = { 1, 0, 0 }, f[3];
        play_to_world(z, o), play_dir_to_world(x, f);
        M.pawn_key[0] = o[0], M.pawn_key[1] = o[1], M.pawn_key[2] = o[2], M.pawn_key[3] = atan2f(f[1], f[0]);
    } else if (M.nactors) {
        float z[3] = { 0, 0, 0 }, o[3], x[3] = { 1, 0, 0 }, f[3];
        play_to_world(z, o), play_dir_to_world(x, f);
        float key[4] = { o[0], o[1], o[2], atan2f(f[1], f[0]) };
        if (fabsf(key[0] - M.pawn_key[0]) + fabsf(key[1] - M.pawn_key[1]) + fabsf(key[2] - M.pawn_key[2]) > 0.5f || fabsf(key[3] - M.pawn_key[3]) > 1e-3f)
            memcpy(M.pawn_key, key, sizeof key), actors_follow(w);
    }
    if (M.event_due && !M.st.room_loading) { // Event On Room Loaded (Count: the planes and volumes; 0: none)
        M.event_due = false;
        BpValue a[1];
        memset(a, 0, sizeof a);
        a[0].v[0] = (float)M.room.n;
        bp_fire_event(w, BPK_EVENT_MR_ROOM_LOADED, a, 1);
    }
    room_light(w, on);
    level_floor(w, on && v->mr_background == MR_BG_ROOM && v->mr_hide_floor);
    // the windows' actors gone
    for (uint32_t i = 0; i < M.nwindows;)
        if (!ecs_alive(w, M.windows[i])) M.windows[i] = M.windows[--M.nwindows];
        else i++;
    M.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

// The boxes the renderer gets (world): the room's (planes and volumes), the windows (the room's real ones when asked, the
// actors set as Passthrough Windows)
static void box_add(const float c_play[3], const float h_play[3][3], int kind, int label, bool play) {
    if (M.nboxes >= MR_MAX_BOXES) return;
    float(*b)[4] = M.boxes[M.nboxes++];
    float c[3], h[3][3];
    if (play) {
        play_to_world(c_play, c);
        for (int i = 0; i < 3; i++) play_dir_to_world(h_play[i], h[i]);
    } else {
        memcpy(c, c_play, 12);
        for (int i = 0; i < 3; i++) memcpy(h[i], h_play[i], 12);
    }
    // world -> its unit cube (mr.glsl MrBox): each half axis over its length squared, the offset of the center
    for (int i = 0; i < 3; i++) {
        float l2 = fmaxf(dot3(h[i], h[i]), 1e-6f);
        for (int k = 0; k < 3; k++) b[i][k] = h[i][k] / l2;
        b[i][3] = -dot3(b[i], c);
    }
    b[3][0] = (float)kind, b[3][1] = (float)label, b[3][2] = b[3][3] = 0.0f;
}
static bool actor_bounds(World* w, Entity e, float c[3], float h[3][3]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t) return false;
    float mn[3] = { -50, -50, -50 }, mx[3] = { 50, 50, 50 }, R[3][3];
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    const CCollision* co = ecs_get(w, e, COMP_COLLISION);
    if (sm) render_mesh_bounds(sm->mesh, mn, mx);
    else if (co && co->shape == COLLISION_BOX) for (int k = 0; k < 3; k++) mn[k] = -co->extent[k], mx[k] = co->extent[k];
    rotator_axes(t->rotation, R);
    float lc[3];
    for (int k = 0; k < 3; k++) lc[k] = (mn[k] + mx[k]) * 0.5f * t->scale[k];
    for (int k = 0; k < 3; k++) c[k] = t->location[k] + R[0][k] * lc[0] + R[1][k] * lc[1] + R[2][k] * lc[2];
    for (int i = 0; i < 3; i++) {
        float half = fmaxf((mx[i] - mn[i]) * 0.5f * fabsf(t->scale[i]), 0.5f);
        for (int k = 0; k < 3; k++) h[i][k] = R[i][k] * half;
    }
    return true;
}

// The simulated real depth: the stand-in room seen from each eye (D16 as Meta's: 1 - near / z, infinite far)
static void sim_depth(const XrEye eyes[2]) {
    if (!M.sim_depth) M.sim_depth = malloc(sizeof(uint16_t) * 2u * SIM_DEPTH * SIM_DEPTH);
    if (!M.sim_depth) return;
    bool changed = false;
    for (int e = 0; e < 2; e++) {
        const XrEye* x = &eyes[e];
        float key[12] = { x->pos[0], x->pos[1], x->pos[2], x->fwd[0], x->fwd[1], x->fwd[2], x->up[0], x->up[1], x->up[2], x->tan_left, x->tan_up, (float)M.room.serial };
        if (!memcmp(key, M.sim_key[e], sizeof key)) continue;
        memcpy(M.sim_key[e], key, sizeof key), changed = true;
        float bc[MR_MAX_ANCHORS][3], bh[MR_MAX_ANCHORS][3][3];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < M.room.n; i++)
            if (M.room.a[i].label != MR_LABEL_WINDOW && M.room.a[i].label != MR_LABEL_DOOR && anchor_box(&M.room.a[i], 2.0f, bc[nb], bh[nb])) nb++;
        uint16_t* out = M.sim_depth + (size_t)e * SIM_DEPTH * SIM_DEPTH;
        for (int j = 0; j < SIM_DEPTH; j++)
            for (int i = 0; i < SIM_DEPTH; i++) {
                float tx = x->tan_left + (x->tan_right - x->tan_left) * ((float)i + 0.5f) / SIM_DEPTH;
                float ty = x->tan_up + (x->tan_down - x->tan_up) * ((float)j + 0.5f) / SIM_DEPTH; // (row 0 the top)
                float d[3];
                for (int k = 0; k < 3; k++) d[k] = x->fwd[k] + x->right[k] * tx + x->up[k] * ty;
                float best = 1e30f, n[3];
                for (uint32_t b = 0; b < nb; b++) {
                    float t = ray_box(x->pos, d, bc[b], bh[b], n);
                    if (t > 0.0f && t < best) best = t;
                }
                // (d . fwd = 1: t is the depth along the view axis)
                out[j * SIM_DEPTH + i] = best < 1e29f ? (uint16_t)fminf((1.0f - 10.0f / fmaxf(best, 10.5f)) * 65535.0f, 65534.0f) : 65535;
            }
    }
    if (changed) M.sim_serial++, M.st.depth_frames++;
    M.st.depth_w = M.st.depth_h = SIM_DEPTH;
}

void mr_render(World* w, RenderView* view, const XrEye eyes[2]) {
    double t0 = re_time_seconds();
    if (!mr_on()) { render_mr_frame(NULL); return; }
    const ProjectVr* v = &project()->vr;
    const XrInfo* xi = xr_info();
    RenderMr m;
    memset(&m, 0, sizeof m);
    m.on = true, m.background = v->mr_background == MR_BG_ROOM, m.sim = xi->sim, m.shadows = v->mr_shadows > 0.0f;
    m.virtual_opacity = fminf(fmaxf(v->mr_virtual_opacity, 0.0f), 1.0f), m.soft_cm = fmaxf(v->mr_occlusion_soft, 0.5f), m.shadow_strength = v->mr_shadows;
    m.pt_opacity = v->mr_opacity, m.pt_style = v->mr_style, m.brightness = v->mr_brightness, m.contrast = v->mr_contrast, m.saturation = v->mr_saturation;
    memcpy(m.tint, v->mr_tint, 12);
    // the depth: the headset's, or made from the stand-in room
    bool depth_mode = v->mr_occlusion == MR_OCC_DEPTH;
    if (depth_mode && !xi->sim && M.dvalid && M.dlast.swapchainIndex < M.dcount) {
        m.depth = true, m.depth_image = M.dimg[M.dlast.swapchainIndex], m.depth_w = M.st.depth_w, m.depth_h = M.st.depth_h, m.depth_serial = M.dserial;
        m.depth_near_cm = M.dlast.nearZ * 100.0f, m.depth_far_cm = isinf(M.dlast.farZ) || M.dlast.farZ <= 0.0f ? 0.0f : M.dlast.farZ * 100.0f;
        for (int e = 0; e < 2; e++) {
            const XrEnvironmentDepthImageViewMETA* dv = &M.dlast.views[e];
            float p[3], ax[3], ay[3], az[3], fwd[3];
            pose_frame(&dv->pose, p, ax, ay, az);
            for (int k = 0; k < 3; k++) fwd[k] = -az[k]; // (OpenXR's views look along -Z)
            play_to_world(p, m.dpos[e]), play_dir_to_world(fwd, m.dfwd[e]), play_dir_to_world(ax, m.dright[e]), play_dir_to_world(ay, m.dup[e]);
            m.dtan[e][0] = tanf(dv->fov.angleLeft), m.dtan[e][1] = tanf(dv->fov.angleRight), m.dtan[e][2] = tanf(dv->fov.angleUp), m.dtan[e][3] = tanf(dv->fov.angleDown);
        }
    } else if (depth_mode && xi->sim && M.room.n && eyes) {
        sim_depth(eyes);
        if (M.sim_depth) {
            m.depth = true, m.depth_cpu = M.sim_depth, m.depth_w = m.depth_h = SIM_DEPTH, m.depth_serial = M.sim_serial;
            m.depth_near_cm = 10.0f, m.depth_far_cm = 0.0f;
            for (int e = 0; e < 2; e++) {
                play_to_world(eyes[e].pos, m.dpos[e]), play_dir_to_world(eyes[e].fwd, m.dfwd[e]);
                play_dir_to_world(eyes[e].right, m.dright[e]), play_dir_to_world(eyes[e].up, m.dup[e]);
                m.dtan[e][0] = eyes[e].tan_left, m.dtan[e][1] = eyes[e].tan_right, m.dtan[e][2] = eyes[e].tan_up, m.dtan[e][3] = eyes[e].tan_down;
            }
        }
    }
    m.depth_flip_y = v->mr_depth_flip, m.unpremultiplied = v->mr_unpremultiplied && !xi->sim;
    // the boxes: the room's where they're needed (the occlusion by the room, the stand-in, the shadows without a real
    // depth), the windows always
    M.nboxes = 0;
    bool room_boxes = v->mr_occlusion == MR_OCC_ROOM || xi->sim || (m.shadows && !m.depth);
    static int no_boxes = -1; // (REALENGINE_MR_NO_ROOM_BOXES=1: the headset's path measured on the simulated one: no stand-in)
    if (no_boxes < 0) no_boxes = getenv("REALENGINE_MR_NO_ROOM_BOXES") && getenv("REALENGINE_MR_NO_ROOM_BOXES")[0] == '1';
    if (no_boxes) room_boxes = false; // (the stand-in a light wall: the edges over the real world seen)
    m.room_occludes = v->mr_occlusion == MR_OCC_ROOM || (v->mr_occlusion == MR_OCC_DEPTH && !m.depth);
    float lamp[3] = { 0, 0, 0 }, lamp_n = 0.0f;
    for (uint32_t i = 0; i < M.room.n; i++) {
        const MrAnchor* a = &M.room.a[i];
        float c[3], h[3][3];
        bool window = a->label == MR_LABEL_WINDOW || a->label == MR_LABEL_DOOR;
        if (window) {
            if (!v->mr_windows_real || !a->plane) continue;
            if (!anchor_box(a, 60.0f, c, h)) continue;
            for (int k = 0; k < 3; k++) c[k] += a->az[k] * 25.0f; // (from 5 cm behind the wall to 55 cm before it)
            box_add(c, h, 1, a->label, true);
            continue;
        }
        if (a->label == MR_LABEL_CEILING) {
            float cc[3];
            anchor_point(a, a->rect[0] + a->rect[2] * 0.5f, a->rect[1] + a->rect[3] * 0.5f, 20.0f, cc);
            for (int k = 0; k < 3; k++) lamp[k] += cc[k];
            lamp_n += 1.0f;
        }
        if (room_boxes && anchor_box(a, 2.0f, c, h)) box_add(c, h, 0, a->label, true);
    }
    if (lamp_n > 0.0f) {
        for (int k = 0; k < 3; k++) lamp[k] /= lamp_n;
        play_to_world(lamp, m.lamp);
    }
    for (uint32_t i = 0; i < M.nwindows; i++) {
        float c[3], h[3][3];
        if (ecs_alive(w, M.windows[i]) && actor_bounds(w, M.windows[i], c, h)) box_add(c, h, 1, MR_LABEL_WINDOW, false);
    }
    m.boxes = (const float(*)[4][4])M.boxes, m.nboxes = M.nboxes;
    render_mr_frame(&m);
    // the exposure: automatic by default (the histogram leaves the black background out: only the virtual things are
    // metered, as in the flat game; a fixed EV with the level's baked light, made for the sun outside, burned them white:
    // the headset's first test), or the EV set
    if (v->mr_ev100 > 0.0f) view->auto_exposure = false, view->exposure_ev100 = v->mr_ev100;
    M.st.render_ms = (re_time_seconds() - t0) * 1000.0;
}

void mr_end_play(World* w) {
    for (uint32_t i = 0; i < M.nactors; i++)
        if (ecs_alive(w, M.actors[i])) ecs_destroy(w, M.actors[i]);
    level_floor(w, false);
    M.nactors = 0, M.nwindows = 0, M.playing = false, M.light_key = 0;
    render_mr_frame(NULL);
}

// --- The Blueprints' ---
void mr_set_window(World* w, Entity e, bool on) {
    (void)w;
    for (uint32_t i = 0; i < M.nwindows; i++)
        if (M.windows[i] == e) {
            if (!on) M.windows[i] = M.windows[--M.nwindows];
            return;
        }
    if (on && e != ENTITY_NONE && M.nwindows < MR_MAX_WINDOWS) M.windows[M.nwindows++] = e;
}
bool mr_load_room(void) {
    if (xr_info()->sim) { sim_room(); return true; }
    if (!M.st.scene_ext) return false;
    M.room_reload = true;
    return true;
}
bool mr_request_room_setup(void) {
    if (xr_info()->sim) { sim_room(); return true; }
    XrInternal x;
    xr_internal(&x);
    if (!p_xrRequestSceneCaptureFB || !x.session) return false;
    XrSceneCaptureRequestInfoFB ci = { XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB, NULL, 0, NULL };
    XrAsyncRequestIdFB rq;
    if (!XR_SUCCEEDED(p_xrRequestSceneCaptureFB(x.session, &ci, &rq))) return false;
    M.capture_asked = true, M.q = Q_CAPTURE;
    note("the headset's Space Setup opened");
    return true;
}
uint32_t mr_room_actors(Entity* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < M.nactors && n < cap; i++) out[n++] = M.actors[i];
    return n;
}
bool mr_actor_info(World* w, Entity e, int* label, float center[3], float normal[3], float size[3]) {
    for (uint32_t i = 0; i < M.nactors; i++) {
        if (M.actors[i] != e) continue;
        const MrAnchor* a = &M.room.a[M.actor_anchor[i]];
        float c[3], h[3][3];
        anchor_box(a, 0.0f, c, h);
        if (a->plane && !a->volume) anchor_point(a, a->rect[0] + a->rect[2] * 0.5f, a->rect[1] + a->rect[3] * 0.5f, 0.0f, c); // (the face's center)
        *label = a->label;
        play_to_world(c, center), play_dir_to_world(a->az, normal);
        size[0] = a->volume ? a->box_max[0] - a->box_min[0] : a->rect[2];
        size[1] = a->volume ? a->box_max[1] - a->box_min[1] : a->rect[3];
        size[2] = a->volume ? a->box_max[2] - a->box_min[2] : 0.0f;
        (void)w;
        return true;
    }
    return false;
}
bool mr_line_trace(const float a[3], const float b[3], float hit[3], float normal[3], int* label, Entity* actor) {
    float o[3], wd[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, d[3];
    float len = sqrtf(dot3(wd, wd));
    if (len < 1e-3f) return false;
    world_to_play(a, o), world_dir_to_play(wd, d);
    for (int k = 0; k < 3; k++) d[k] /= len;
    float best = len, bn[3] = { 0, 0, 1 };
    int bl = -1, bi = -1;
    for (uint32_t i = 0; i < M.room.n; i++) {
        const MrAnchor* an = &M.room.a[i];
        if (an->label == MR_LABEL_WINDOW || an->label == MR_LABEL_DOOR) continue;
        float c[3], h[3][3], n[3];
        if (!anchor_box(an, 0.5f, c, h)) continue;
        float t = ray_box(o, d, c, h, n);
        if (t > 0.0f && t < best) best = t, memcpy(bn, n, 12), bl = an->label, bi = (int)i;
    }
    for (uint32_t i = 0; i + 2 < M.room.mesh_ni; i += 3) { // the room's mesh: any real thing
        const float* v = M.room.mesh_v;
        float n[3];
        float t = ray_tri(o, d, v + 3 * M.room.mesh_i[i], v + 3 * M.room.mesh_i[i + 1], v + 3 * M.room.mesh_i[i + 2], n);
        if (t > 0.0f && t < best - 0.5f) best = t, memcpy(bn, n, 12), bl = MR_LABEL_GLOBAL_MESH, bi = -1;
    }
    if (bl < 0) return false;
    float p[3] = { o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best };
    play_to_world(p, hit), play_dir_to_world(bn, normal);
    *label = bl;
    *actor = ENTITY_NONE;
    for (uint32_t i = 0; i < M.nactors && bi >= 0; i++)
        if (M.actor_anchor[i] == bi) *actor = M.actors[i];
    return true;
}

// --- The Debug Viewer: the tick, the renderer's feed, the room's actors ---
#include "debug_viewer.h"
static void dv_collect(DvList* l, const DvContext* c) {
    l->gpu = false, l->estimated = false;
    l->ms = M.st.tick_ms + M.st.render_ms;
    snprintf(l->note, sizeof l->note, "%s; room %u (mesh %u triangles), %u actors, %u boxes, real depth %s %ux%u; %s",
             mr_on() ? "on" : "off", M.room.n, M.room.mesh_ni / 3u, M.nactors, M.nboxes, M.st.depth_running ? "on" : M.room.simulated ? "simulated" : "off",
             M.st.depth_w, M.st.depth_h, M.st.note);
    dv_add(l, "Tick", "passthrough, the room's queries and actors", (float)M.st.tick_ms, ENTITY_NONE);
    dv_add(l, "Renderer's feed", "boxes, depth cameras, the simulated depth", (float)M.st.render_ms, ENTITY_NONE);
    World* w = c ? c->world : NULL;
    for (uint32_t i = 0; i < M.nactors && w; i++) {
        if (!ecs_alive(w, M.actors[i])) continue;
        const CName* n = ecs_get(w, M.actors[i], COMP_NAME);
        const MrAnchor* a = &M.room.a[M.actor_anchor[i]];
        char info[48];
        snprintf(info, sizeof info, "%s %.0f x %.0f cm", a->labels, a->volume ? a->box_max[0] - a->box_min[0] : a->rect[2],
                 a->volume ? a->box_max[1] - a->box_min[1] : a->rect[3]);
        dv_add(l, n ? n->text : "MR", info, 0.0f, M.actors[i]);
    }
}
void mr_register_debug(void) { dv_register("Mixed Reality", dv_collect); }

// --- The self-test (--mr-test) ---
int mr_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: mr: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
    double t0 = re_time_seconds();
    // the labels
    CHECK(mr_label_from_runtime("WALL_FACE") == MR_LABEL_WALL && mr_label_from_runtime("DESK,TABLE") == MR_LABEL_TABLE &&
          mr_label_from_runtime("FOO,COUCH") == MR_LABEL_COUCH && mr_label_from_runtime("") == MR_LABEL_OTHER, "the runtime's labels");
    // a pose: OpenXR's +Z (toward the viewer) is the engine's -X; a quarter turn about Y turns -Z (forward) to -X (left)
    XrPosef ps = { { 0, 0.70710678f, 0, 0.70710678f }, { 1, 2, 3 } };
    float p[3], ax[3], ay[3], az[3];
    pose_frame(&ps, p, ax, ay, az);
    CHECK(fabsf(p[0] + 300) < 0.01f && fabsf(p[1] - 100) < 0.01f && fabsf(p[2] - 200) < 0.01f, "a pose's position (%.1f %.1f %.1f)", p[0], p[1], p[2]);
    CHECK(fabsf(ax[0] - 1.0f) < 1e-3f && fabsf(ay[2] - 1.0f) < 1e-3f && fabsf(az[1] - 1.0f) < 1e-3f, "a pose's axes (ax %.2f %.2f %.2f, az %.2f %.2f %.2f)", ax[0], ax[1], ax[2], az[0], az[1], az[2]);
    // the simulated headset and room
    XrConfig c;
    xr_config_defaults(&c);
    xr_set_config(&c);
#ifdef _WIN32
    _putenv("REALENGINE_XR_SIM_ONLY=1");
#else
    setenv("REALENGINE_XR_SIM_ONLY", "1", 1);
#endif
    CHECK(xr_init(true) && xr_info()->sim, "the simulated headset starts");
    ProjectVr* v = &project()->vr;
    v->mr_enabled = v->mr_passthrough = v->mr_scene = v->mr_scene_actors = v->mr_align = true;
    v->mr_occlusion = MR_OCC_DEPTH, v->mr_background = MR_BG_ROOM, v->mr_shadows = 0.6f, v->mr_virtual_opacity = 1.0f, v->mr_ev100 = 7.0f;
    static World w;
    if (!ecs_init(&w)) return fails + 1;
    vr_pawn_begin_play(&w);
    mr_begin_play(&w);
    mr_tick(&w, 1.0f / 90.0f); // (the stand-in room made)
    mr_tick(&w, 1.0f / 90.0f); // (its actors, its event)
    CHECK(mr_on() && M.room.simulated && M.room.n == 10 && M.room.mesh_ni == 8u * 36u, "the stand-in room (%u anchors, %u mesh indices)", M.room.n, M.room.mesh_ni);
    int loaded_events = M.event_due ? 0 : 1;
    CHECK(loaded_events == 1 && M.nactors == 8, "the room's actors (%u: floor, ceiling, 4 walls, table, couch)", M.nactors);
    // the floor's actor: its top at z 0, collides, never drawn
    Entity fl = ENTITY_NONE;
    for (uint32_t i = 0; i < M.nactors; i++)
        if (M.room.a[M.actor_anchor[i]].label == MR_LABEL_FLOOR) fl = M.actors[i];
    const CTransform* ft = fl ? ecs_get(&w, fl, COMP_TRANSFORM) : NULL;
    CHECK(ft && fabsf(ft->location[2] + 2.0f) < 0.05f && (ft->flags & TRANSFORM_NO_RENDER) && !(ft->flags & TRANSFORM_HIDDEN_GAME),
          "the floor's actor 4 cm thick under z 0, not drawn, colliding (z %.2f)", ft ? ft->location[2] : -1.0f);
    const CName* fn = fl ? ecs_get(&w, fl, COMP_NAME) : NULL;
    CHECK(fn && !strcmp(fn->text, "MR Floor"), "its name (%s)", fn ? fn->text : "-");
    // traces: down onto the floor, onto the table's top, into the far wall; the mesh alone (actors off)
    float a0[3] = { 100, 0, 100 }, b0[3] = { 100, 0, -100 }, hit[3], nrm[3];
    int label = -1;
    Entity act = ENTITY_NONE;
    bool h = mr_line_trace(a0, b0, hit, nrm, &label, &act);
    CHECK(h && fabsf(hit[2]) < 0.6f && nrm[2] > 0.99f && label == MR_LABEL_FLOOR && act == fl, "a trace onto the floor (%d, z %.2f, label %d)", h, hit[2], label);
    float a1[3] = { 150, 60, 150 }, b1[3] = { 150, 60, 0 };
    h = mr_line_trace(a1, b1, hit, nrm, &label, &act);
    CHECK(h && fabsf(hit[2] - 75.0f) < 0.6f && label == MR_LABEL_TABLE, "a trace onto the table (%d, z %.2f, label %d)", h, hit[2], label);
    float a2[3] = { 0, 0, 150 }, b2[3] = { 1000, 0, 150 };
    h = mr_line_trace(a2, b2, hit, nrm, &label, &act);
    CHECK(h && fabsf(hit[0] - 400.0f) < 0.6f && nrm[0] < -0.99f && label == MR_LABEL_WALL, "a trace into the far wall (%d, x %.2f, label %d)", h, hit[0], label);
    // the pawn's play space turned 90 degrees: the actors follow (the far wall now along +Y)
    float yaw0 = ft ? ft->rotation[1] : 0.0f;
    vr_pawn_turn_play_space(&w, 90.0f);
    mr_tick(&w, 1.0f / 90.0f);
    float hw[3], z0[3] = { 0, 0, 170 };
    vr_pawn_to_world(z0, hw);
    float a3[3] = { hw[0], hw[1], 150 }, b3[3] = { hw[0], hw[1] + 1000, 150 };
    h = mr_line_trace(a3, b3, hit, nrm, &label, &act);
    CHECK(h && fabsf(hit[1] - hw[1] - 400.0f) < 1.0f && label == MR_LABEL_WALL, "the room turned with the play space (%d, %.1f)", h, hit[1] - hw[1]);
    const CTransform* ft2 = ecs_get(&w, fl, COMP_TRANSFORM);
    float dyaw = ft2 ? fmodf(ft2->rotation[1] - yaw0 + 720.0f, 360.0f) : 0.0f;
    CHECK(ft2 && fabsf(dyaw - 90.0f) < 1.0f, "the floor's actor turned too (%.1f degrees)", dyaw);
    // the alignment: a room 30 degrees off the axes turned back square
    for (uint32_t i = 0; i < M.room.n; i++) {
        MrAnchor* an = &M.room.a[i];
        float* vs[4] = { an->pos, an->ax, an->ay, an->az };
        for (int k = 0; k < 4; k++) {
            float x = vs[k][0], y = vs[k][1], cs = cosf(0.5235988f), sn = sinf(0.5235988f);
            vs[k][0] = x * cs - y * sn, vs[k][1] = x * sn + y * cs;
        }
    }
    M.room.serial++, M.room_seen = 0;
    mr_tick(&w, 1.0f / 90.0f);
    float wall_d[3] = { 0 };
    for (uint32_t i = 0; i < M.room.n; i++)
        if (M.room.a[i].label == MR_LABEL_WALL) { play_dir_to_world(M.room.a[i].ax, wall_d); break; }
    float wy = fmodf(fabsf(atan2f(wall_d[1], wall_d[0]) * 57.29578f), 90.0f);
    CHECK(wy < 0.5f || wy > 89.5f, "the level turned to the room's walls (%.2f degrees off)", wy);
    // the renderer's numbers: the room's boxes, a Passthrough Window, the simulated depth
    Entity win = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    CTransform* wt = ecs_get(&w, win, COMP_TRANSFORM);
    wt->scale[0] = wt->scale[1] = wt->scale[2] = 1.0f, wt->location[0] = 200.0f;
    mr_set_window(&w, win, true);
    XrEye eyes[2];
    float head[3] = { 0, 0, 160 };
    xr_sim_head(head, 0.0f, 0.0f);
    xr_begin_frame(eyes);
    RenderView view;
    memset(&view, 0, sizeof view);
    mr_render(&w, &view, eyes);
    uint32_t nwin = 0, nroom = 0;
    for (uint32_t i = 0; i < M.nboxes; i++) *(M.boxes[i][3][0] > 0.5f ? &nwin : &nroom) += 1;
    CHECK(nroom == 8 && nwin == 1 && !view.auto_exposure && fabsf(view.exposure_ev100 - 7.0f) < 1e-3f, "the boxes (%u room, %u window), the exposure", nroom, nwin);
    // the simulated depth: the middle of the left eye's (looking at the far wall... the play space turned since: whatever it
    // meets) as far as a trace along the same ray says
    const uint16_t* dd = M.sim_depth;
    float dc = dd ? 10.0f / (1.0f - (float)dd[(SIM_DEPTH / 2) * SIM_DEPTH + SIM_DEPTH / 2] / 65535.0f) : 0.0f;
    float ew[3], ef[3], et[3];
    play_to_world(eyes[0].pos, ew), play_dir_to_world(eyes[0].fwd, ef);
    for (int k = 0; k < 3; k++) et[k] = ew[k] + ef[k] * 2000.0f;
    h = mr_line_trace(ew, et, hit, nrm, &label, &act);
    float tdist = h ? sqrtf((hit[0] - ew[0]) * (hit[0] - ew[0]) + (hit[1] - ew[1]) * (hit[1] - ew[1]) + (hit[2] - ew[2]) * (hit[2] - ew[2])) : -1.0f;
    CHECK(dd && h && fabsf(dc - tdist) < tdist * 0.03f + 2.0f, "the simulated real depth (%.1f cm, the trace %.1f)", dc, tdist);
    // windows off, the passthrough off: nothing for the renderer
    mr_set_window(&w, win, false);
    mr_set_passthrough(false);
    CHECK(!mr_on(), "the passthrough off");
    mr_set_passthrough(true);
    // the level's floor planes hidden while the room is seen (still colliding)
    Entity lf = ecs_create(&w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    ((CStaticMesh*)ecs_get(&w, lf, COMP_STATIC_MESH))->mesh = SHAPE_PLANE;
    v->mr_hide_floor = true, M.nhidden = 0;
    mr_tick(&w, 1.0f / 90.0f);
    CHECK((((CTransform*)ecs_get(&w, lf, COMP_TRANSFORM))->flags & TRANSFORM_NO_RENDER) != 0, "the level's floor hidden");
    mr_end_play(&w);
    CHECK(!(((CTransform*)ecs_get(&w, lf, COMP_TRANSFORM))->flags & TRANSFORM_NO_RENDER) && !ecs_alive(&w, fl), "the end: the floor back, the actors gone");
    vr_pawn_end_play(&w);
    xr_shutdown();
    printf("[RealEngine] MR self-test: %d failures (%.1f ms)\n", fails, (re_time_seconds() - t0) * 1000.0);
#undef CHECK
    return fails;
}
