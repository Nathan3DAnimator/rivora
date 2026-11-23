// ARCore (ar_arcore.h, notas-dev/xr.md): the session, the camera's pose and picture, the light, the planes; and the
// simulated session of the PC (the same data from a picture and one floor).
#include "ar_arcore.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"

void bp_boxes_changed(void); // (bp_vm.c: the Characters' collision boxes gathered again)

#define MAX_PLANES 64
#define MAX_POLY 128

typedef struct Plane {
    bool up;                  // faces up (a floor, a table): receives the shadows
    float center[3], axis_x[3], axis_z[3], normal[3]; // engine space (cm), its frame
    float ext_x, ext_z;       // its size (cm)
    uint32_t npoly;
    float poly[MAX_POLY][3];  // its outline (engine, cm)
    uint64_t id;              // (the trackable's pointer: the same plane keeps its actor)
} Plane;

static struct {
    ArcoreView v;
    Plane planes[MAX_PLANES];
    uint32_t nplanes;
    uint8_t *ybuf, *uvbuf;
    uint32_t cap;
    uint16_t* dbuf; // the depth (mm)
    uint32_t dcap;
    bool no_occlusion;
    float sim_key[14]; // the simulated depth's camera (made again when it moves)
    // the simulated session's camera (the level's)
    float sim_cam[4][3], sim_hfov;
    float sim_screen[2]; // (the screen's size: arcore_hit's pixels)
    // the actors made for the planes
    Entity actors[MAX_PLANES];
    uint64_t actor_id[MAX_PLANES];
    uint32_t nactors;
} A;

const ArcoreView* arcore_view(void) { return &A.v; }

const char* arcore_state_name(ArcoreState s) {
    static const char* const n[] = { "off", "unsupported", "needs Google Play Services for AR", "no camera permission", "paused", "searching", "tracking" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

static bool ensure_image(uint32_t w, uint32_t h) {
    uint32_t need = w * h;
    if (need <= A.cap && A.ybuf) return true;
    free(A.ybuf), free(A.uvbuf);
    A.ybuf = malloc(need), A.uvbuf = malloc(need / 2 + 2);
    A.cap = A.ybuf && A.uvbuf ? need : 0;
    return A.cap != 0;
}

static bool ensure_depth(uint32_t w, uint32_t h) {
    if (w * h <= A.dcap && A.dbuf) return true;
    free(A.dbuf);
    A.dbuf = malloc((size_t)w * h * 2);
    A.dcap = A.dbuf ? w * h : 0;
    return A.dcap != 0;
}

void arcore_set_occlusion(bool on) { A.no_occlusion = !on; }

// ARCore's space (m, Y up, -Z forward, right-handed) -> the engine's (cm, X forward, Y right, Z up)
static void to_engine(const float a[3], float s, float e[3]) { e[0] = -a[2] * s, e[1] = a[0] * s, e[2] = a[1] * s; }

static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// --- The planes (shared by both sessions) ---

uint32_t arcore_floor_triangles(float* out, uint32_t max_verts) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < A.nplanes; i++) {
        const Plane* p = &A.planes[i];
        if (!p->up || p->npoly < 3) continue;
        for (uint32_t k = 1; k + 1 < p->npoly && n + 3 <= max_verts; k++) { // (a convex outline: a fan)
            memcpy(out + 3 * n++, p->poly[0], 12);
            memcpy(out + 3 * n++, p->poly[k], 12);
            memcpy(out + 3 * n++, p->poly[k + 1], 12);
        }
    }
    return n;
}

static bool hit_planes(const float o[3], const float d[3], float out[3], float nrm[3]) {
    float best = 1e30f;
    bool hit = false;
    for (uint32_t i = 0; i < A.nplanes; i++) {
        const Plane* p = &A.planes[i];
        float dn = d[0] * p->normal[0] + d[1] * p->normal[1] + d[2] * p->normal[2];
        if (fabsf(dn) < 1e-6f) continue;
        float t = ((p->center[0] - o[0]) * p->normal[0] + (p->center[1] - o[1]) * p->normal[1] + (p->center[2] - o[2]) * p->normal[2]) / dn;
        if (t <= 0.0f || t >= best) continue;
        float q[3] = { o[0] + d[0] * t - p->center[0], o[1] + d[1] * t - p->center[1], o[2] + d[2] * t - p->center[2] };
        float lx = q[0] * p->axis_x[0] + q[1] * p->axis_x[1] + q[2] * p->axis_x[2];
        float lz = q[0] * p->axis_z[0] + q[1] * p->axis_z[1] + q[2] * p->axis_z[2];
        if (fabsf(lx) > p->ext_x * 0.5f || fabsf(lz) > p->ext_z * 0.5f) continue;
        best = t, hit = true;
        for (int k = 0; k < 3; k++) out[k] = o[k] + d[k] * t, nrm[k] = p->normal[k];
    }
    return hit;
}

void arcore_sync_planes(World* w) {
    bool changed = false;
    // the actors whose plane is gone (or merged into another): removed
    for (uint32_t i = 0; i < A.nactors;) {
        bool alive = false;
        for (uint32_t k = 0; k < A.nplanes && !alive; k++) alive = A.planes[k].id == A.actor_id[i];
        if (alive && ecs_alive(w, A.actors[i])) { i++; continue; }
        if (ecs_alive(w, A.actors[i])) ecs_destroy(w, A.actors[i]);
        A.actors[i] = A.actors[A.nactors - 1], A.actor_id[i] = A.actor_id[A.nactors - 1], A.nactors--;
        changed = true;
    }
    for (uint32_t k = 0; k < A.nplanes; k++) {
        const Plane* p = &A.planes[k];
        Entity e = ENTITY_NONE;
        for (uint32_t i = 0; i < A.nactors; i++)
            if (A.actor_id[i] == p->id) e = A.actors[i];
        if (e == ENTITY_NONE) {
            if (A.nactors >= MAX_PLANES) continue;
            e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_COLLISION));
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "AR Plane %u", A.nactors + 1);
            A.actors[A.nactors] = e, A.actor_id[A.nactors++] = p->id;
            changed = true;
        }
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        CCollision* c = ecs_get(w, e, COMP_COLLISION);
        if (!t || !c) continue;
        // a box 4 cm thick under the plane's surface, turned to its frame (yaw for a floor, pitch for a wall)
        float loc[3];
        for (int i = 0; i < 3; i++) loc[i] = p->center[i] - p->normal[i] * 2.0f;
        float yaw = atan2f(p->axis_x[1], p->axis_x[0]) * 57.29578f;
        float pitch = p->up ? 0.0f : 90.0f;
        float ext[3] = { p->ext_x * 0.5f, p->ext_z * 0.5f, 2.0f };
        if (memcmp(t->location, loc, 12) || t->rotation[1] != yaw || memcmp(c->extent, ext, 12)) changed = true;
        memcpy(t->location, loc, 12);
        t->rotation[0] = pitch, t->rotation[1] = yaw, t->rotation[2] = 0.0f;
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
        c->shape = COLLISION_BOX, c->block = 1;
        memcpy(c->extent, ext, 12);
    }
    if (changed) bp_boxes_changed(); // (the Characters' collision boxes made again)
}

// --- The simulated session (the PC) ---

void arcore_sim_camera(const float pos[3], const float fwd[3], const float right[3], const float up[3], float hfov_deg) {
    memcpy(A.sim_cam[0], pos, 12), memcpy(A.sim_cam[1], fwd, 12), memcpy(A.sim_cam[2], right, 12), memcpy(A.sim_cam[3], up, 12);
    A.sim_hfov = hfov_deg;
}

bool arcore_simulate(const uint8_t* rgba, uint32_t w, uint32_t h, float hfov_deg) {
    w &= ~1u, h &= ~1u;
    if (!rgba || w < 2 || h < 2 || !ensure_image(w, h)) return false;
    // the picture as a camera's: YUV 4:2:0 (BT.601, full range)
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = rgba + 4 * ((size_t)y * w + x);
            A.ybuf[y * w + x] = (uint8_t)fminf(255.0f, 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] + 0.5f);
        }
    for (uint32_t y = 0; y < h / 2; y++)
        for (uint32_t x = 0; x < w / 2; x++) {
            float r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const uint8_t* p = rgba + 4 * ((size_t)(2 * y + k / 2) * w + 2 * x + (k & 1));
                r += p[0] * 0.25f, g += p[1] * 0.25f, b += p[2] * 0.25f;
            }
            float u = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f, v = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
            A.uvbuf[2 * (y * (w / 2) + x)] = (uint8_t)fminf(fmaxf(u + 0.5f, 0.0f), 255.0f);
            A.uvbuf[2 * (y * (w / 2) + x) + 1] = (uint8_t)fminf(fmaxf(v + 0.5f, 0.0f), 255.0f);
        }
    memset(&A.v, 0, sizeof A.v);
    A.v.state = ARCORE_TRACKING, A.v.simulated = true;
    A.v.y = A.ybuf, A.v.uv = A.uvbuf, A.v.img_w = w, A.v.img_h = h, A.v.img_serial = 1;
    A.v.hfov_deg = hfov_deg;
    // one floor: 8 x 8 m at the origin
    Plane* p = &A.planes[0];
    memset(p, 0, sizeof *p);
    p->up = true, p->id = 1, p->ext_x = p->ext_z = 800.0f;
    p->axis_x[0] = 1.0f, p->axis_z[1] = 1.0f, p->normal[2] = 1.0f;
    const float c[4][2] = { { -400, -400 }, { 400, -400 }, { 400, 400 }, { -400, 400 } };
    p->npoly = 4;
    for (int k = 0; k < 4; k++) p->poly[k][0] = c[k][0], p->poly[k][1] = c[k][1], p->poly[k][2] = 0.0f;
    A.nplanes = 1;
    A.v.planes = 1;
    return true;
}

static void sim_update(uint32_t sw, uint32_t sh) {
    A.v.frame++;
    A.sim_screen[0] = (float)sw, A.sim_screen[1] = (float)sh;
    memcpy(A.v.pos, A.sim_cam[0], 12), memcpy(A.v.fwd, A.sim_cam[1], 12), memcpy(A.v.right, A.sim_cam[2], 12), memcpy(A.v.up, A.sim_cam[3], 12);
    // the picture fills the screen as a camera's preview does (cropped to the screen's shape, centered)
    float sa = sh ? (float)sw / (float)sh : 1.0f, ia = (float)A.v.img_w / (float)A.v.img_h;
    float cx = sa < ia ? sa / ia : 1.0f, cy = sa < ia ? 1.0f : ia / sa;
    for (int k = 0; k < 4; k++) {
        float nx = (k & 1) ? 1.0f : -1.0f, ny = (k & 2) ? 1.0f : -1.0f;
        A.v.uv_corners[k][0] = 0.5f + 0.5f * nx * cx, A.v.uv_corners[k][1] = 0.5f + 0.5f * ny * cy;
    }
    A.v.hfov_deg = A.sim_hfov > 0.0f ? A.sim_hfov : 70.0f;
    A.v.vfov_deg = 2.0f * atanf(tanf(A.v.hfov_deg * 0.00872665f) / fmaxf(sa, 1e-3f)) * 57.29578f;
    if (A.no_occlusion) { A.v.depth = NULL; return; }
    // the depth a phone would measure: the floor (its 8 x 8 m) along each pixel's ray, 160 pixels across the picture as
    // ARCore's (made again only when the camera moves)
    float key[14];
    memcpy(key, A.v.pos, 12), memcpy(key + 3, A.v.fwd, 12), memcpy(key + 6, A.v.right, 12), memcpy(key + 9, A.v.up, 12);
    key[12] = A.v.hfov_deg, key[13] = sa;
    uint32_t dw = 160, dh = (uint32_t)fmaxf(2.0f, 160.0f * (float)A.v.img_h / (float)A.v.img_w + 0.5f);
    if (A.v.depth && !memcmp(key, A.sim_key, sizeof key)) return;
    if (!ensure_depth(dw, dh)) return;
    memcpy(A.sim_key, key, sizeof key);
    float th = tanf(A.v.hfov_deg * 0.00872665f), tv = tanf(A.v.vfov_deg * 0.00872665f);
    const Plane* fl = &A.planes[0];
    const char* fh = getenv("REALENGINE_AR_SIM_FLOOR"); // (cm: the real floor raised, the virtual things' bases hidden: the check)
    float floor_z = fh ? (float)atof(fh) : 0.0f;
    for (uint32_t j = 0; j < dh; j++)
        for (uint32_t i = 0; i < dw; i++) {
            float u = ((float)i + 0.5f) / (float)dw, v = ((float)j + 0.5f) / (float)dh;
            float nx = (u - 0.5f) / (0.5f * cx), ny = (v - 0.5f) / (0.5f * cy); // (the screen's NDC of that picture point)
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = A.v.fwd[k] + A.v.right[k] * nx * th - A.v.up[k] * ny * tv;
            uint16_t mm = 0;
            if (d[2] < -1e-4f) {
                float t = (floor_z - A.v.pos[2]) / d[2]; // (d . fwd = 1: t is the depth along the view axis)
                float hx = A.v.pos[0] + d[0] * t, hy = A.v.pos[1] + d[1] * t;
                if (t > 0.0f && fabsf(hx) <= fl->ext_x * 0.5f && fabsf(hy) <= fl->ext_z * 0.5f) mm = (uint16_t)fminf(t * 10.0f, 65535.0f);
            }
            A.dbuf[j * dw + i] = mm;
        }
    A.v.depth = A.dbuf, A.v.depth_w = dw, A.v.depth_h = dh, A.v.depth_serial++;
    memcpy(A.v.depth_corners, A.v.uv_corners, sizeof A.v.depth_corners);
}

// --- Android: ARCore itself ---
#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>

#include "arcore_c_api.h"
#include "re_android.h"

#define FNS(X) \
    X(ArCoreApk_checkAvailability) X(ArSession_create) X(ArConfig_create) X(ArConfig_destroy) X(ArConfig_setPlaneFindingMode) \
    X(ArConfig_setLightEstimationMode) X(ArConfig_setUpdateMode) X(ArConfig_setFocusMode) X(ArConfig_setTextureUpdateMode) \
    X(ArSession_configure) X(ArSession_resume) X(ArSession_pause) X(ArSession_destroy) X(ArSession_setDisplayGeometry) \
    X(ArFrame_create) X(ArFrame_destroy) X(ArSession_update) X(ArFrame_acquireCamera) X(ArCamera_getTrackingState) \
    X(ArCamera_getDisplayOrientedPose) X(ArCamera_getProjectionMatrix) X(ArCamera_release) X(ArPose_create) X(ArPose_destroy) \
    X(ArPose_getMatrix) X(ArFrame_transformCoordinates2d) X(ArFrame_acquireCameraImage) X(ArImage_getWidth) X(ArImage_getHeight) \
    X(ArImage_getFormat) X(ArImage_getPlaneData) X(ArImage_getPlaneRowStride) X(ArImage_getPlanePixelStride) X(ArImage_release) \
    X(ArLightEstimate_create) X(ArLightEstimate_destroy) X(ArFrame_getLightEstimate) X(ArLightEstimate_getState) \
    X(ArLightEstimate_getEnvironmentalHdrMainLightDirection) X(ArLightEstimate_getEnvironmentalHdrMainLightIntensity) \
    X(ArLightEstimate_getEnvironmentalHdrAmbientSphericalHarmonics) X(ArTrackableList_create) X(ArTrackableList_destroy) \
    X(ArSession_getAllTrackables) X(ArTrackableList_getSize) X(ArTrackableList_acquireItem) X(ArTrackable_release) \
    X(ArTrackable_getTrackingState) X(ArPlane_acquireSubsumedBy) X(ArPlane_getType) X(ArPlane_getCenterPose) \
    X(ArPlane_getPolygonSize) X(ArPlane_getPolygon) X(ArPlane_getExtentX) X(ArPlane_getExtentZ) X(ArHitResultList_create) \
    X(ArHitResultList_destroy) X(ArFrame_hitTest) X(ArHitResultList_getSize) X(ArHitResult_create) X(ArHitResult_destroy) \
    X(ArHitResultList_getItem) X(ArHitResult_getHitPose) X(ArCameraConfigFilter_create) X(ArCameraConfigFilter_destroy) \
    X(ArSession_getSupportedCameraConfigsWithFilter) X(ArCameraConfigList_create) X(ArCameraConfigList_destroy) \
    X(ArCameraConfigList_getSize) X(ArCameraConfigList_getItem) X(ArCameraConfig_create) X(ArCameraConfig_destroy) \
    X(ArCameraConfig_getImageDimensions) X(ArSession_setCameraConfig) X(ArSession_isDepthModeSupported) X(ArConfig_setDepthMode) \
    X(ArFrame_acquireDepthImage16Bits) X(ArImage_getTimestamp)
#define DECL(n) static __typeof__(n)* p_##n;
FNS(DECL)

static struct {
    void* lib;
    ArSession* s;
    ArFrame* f;
    ArPose* pose;
    ArLightEstimate* le;
    ArTrackableList* tl;
    bool running, wants;      // resumed; started by the game (resumed again when the app comes back)
    bool depth;               // the Depth API on (the occlusion)
    int64_t depth_ts;         // the last depth image's
    uint32_t sw, sh, rot;
    double next_try;
} K;

static bool load(void) {
    if (K.lib) return true;
    K.lib = dlopen("libarcore_sdk_c.so", RTLD_NOW | RTLD_LOCAL);
    if (!K.lib) return false;
#define LOAD(n) if (!(p_##n = (__typeof__(n)*)dlsym(K.lib, #n))) { __android_log_print(ANDROID_LOG_WARN, "RealEngine", "ARCore: %s missing", #n); return false; }
    FNS(LOAD)
#undef LOAD
    return true;
}

static void on_pause(bool paused) { // (the activity's lifecycle: the camera goes back to the system in the background)
    if (!K.s || !K.wants) return;
    if (paused && K.running) p_ArSession_pause(K.s), K.running = false, A.v.state = ARCORE_PAUSED;
    else if (!paused && !K.running && p_ArSession_resume(K.s) == AR_SUCCESS) K.running = true, A.v.state = ARCORE_SEARCHING;
}

static bool has_camera(void) {
    char r[8] = "";
    re_android_call_string("hasCameraPermission", "()Ljava/lang/String;", r, sizeof r);
    return r[0] == '1';
}

bool arcore_start(void) {
    if (K.s) return true;
    memset(&A.v, 0, sizeof A.v);
    if (!load()) { A.v.state = ARCORE_UNSUPPORTED; return false; }
    JNIEnv* env = re_android_env();
    jobject act = re_android_activity();
    ArAvailability av = AR_AVAILABILITY_UNKNOWN_ERROR;
    p_ArCoreApk_checkAvailability(env, act, &av);
    if (av == AR_AVAILABILITY_SUPPORTED_NOT_INSTALLED || av == AR_AVAILABILITY_SUPPORTED_APK_TOO_OLD) { A.v.state = ARCORE_NEEDS_INSTALL; return false; }
    if (av != AR_AVAILABILITY_SUPPORTED_INSTALLED && av != AR_AVAILABILITY_UNKNOWN_CHECKING) { A.v.state = ARCORE_UNSUPPORTED; return false; }
    if (!has_camera()) {
        re_android_call_void("requestCameraPermission", "()V");
        A.v.state = ARCORE_NO_PERMISSION;
        return false;
    }
    if (p_ArSession_create(env, act, &K.s) != AR_SUCCESS) { K.s = NULL, A.v.state = ARCORE_UNSUPPORTED; return false; }
    ArConfig* cfg;
    { // the camera's configuration with the largest CPU picture up to 1920 x 1080 (the default is 640 x 480: the picture
        // behind the level looked soft on the S23; the CPU copy of a 1080p picture is ~3 MB a camera frame)
        ArCameraConfigFilter* flt;
        ArCameraConfigList* list;
        ArCameraConfig *cc, *best;
        p_ArCameraConfigFilter_create(K.s, &flt);
        p_ArCameraConfigList_create(K.s, &list);
        p_ArSession_getSupportedCameraConfigsWithFilter(K.s, flt, list);
        p_ArCameraConfig_create(K.s, &cc), p_ArCameraConfig_create(K.s, &best);
        int32_t n = 0, bw = 0, bh = 0;
        p_ArCameraConfigList_getSize(K.s, list, &n);
        for (int32_t i = 0; i < n; i++) {
            int32_t w = 0, h = 0;
            p_ArCameraConfigList_getItem(K.s, list, i, cc);
            p_ArCameraConfig_getImageDimensions(K.s, cc, &w, &h);
            if (w <= 1920 && h <= 1080 && w * h > bw * bh) bw = w, bh = h, p_ArCameraConfigList_getItem(K.s, list, i, best);
        }
        if (bw > 0) {
            ArStatus cs = p_ArSession_setCameraConfig(K.s, best);
            __android_log_print(ANDROID_LOG_INFO, "RealEngine", "ARCore: camera picture %dx%d (%s, %d configurations)", bw, bh, cs == AR_SUCCESS ? "set" : "refused", n);
        }
        p_ArCameraConfig_destroy(cc), p_ArCameraConfig_destroy(best);
        p_ArCameraConfigList_destroy(list);
        p_ArCameraConfigFilter_destroy(flt);
    }
    int32_t dsup = 0; // the Depth API (the occlusion): where the phone has it (from its cameras and motion, or a ToF sensor)
    if (!A.no_occlusion) p_ArSession_isDepthModeSupported(K.s, AR_DEPTH_MODE_AUTOMATIC, &dsup);
    K.depth = dsup != 0;
    __android_log_print(ANDROID_LOG_INFO, "RealEngine", "ARCore: depth (occlusion) %s", K.depth ? "on" : A.no_occlusion ? "off" : "unsupported");
    p_ArConfig_create(K.s, &cfg);
    p_ArConfig_setPlaneFindingMode(K.s, cfg, AR_PLANE_FINDING_MODE_HORIZONTAL_AND_VERTICAL);
    p_ArConfig_setLightEstimationMode(K.s, cfg, AR_LIGHT_ESTIMATION_MODE_ENVIRONMENTAL_HDR);
    if (K.depth) p_ArConfig_setDepthMode(K.s, cfg, AR_DEPTH_MODE_AUTOMATIC);
    p_ArConfig_setUpdateMode(K.s, cfg, AR_UPDATE_MODE_LATEST_CAMERA_IMAGE);
    p_ArConfig_setFocusMode(K.s, cfg, AR_FOCUS_MODE_AUTO);
    // no OpenGL texture: the picture comes as a CPU image (copied to the GPU by render.c), the camera's frames as
    // hardware buffers ARCore keeps (it needs one of the two texture modes to run)
    p_ArConfig_setTextureUpdateMode(K.s, cfg, AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER);
    ArStatus st = p_ArSession_configure(K.s, cfg);
    p_ArConfig_destroy(cfg);
    if (st != AR_SUCCESS) { // (Environmental HDR unsupported here: the basic light estimate's mode instead)
        p_ArConfig_create(K.s, &cfg);
        p_ArConfig_setPlaneFindingMode(K.s, cfg, AR_PLANE_FINDING_MODE_HORIZONTAL_AND_VERTICAL);
        p_ArConfig_setUpdateMode(K.s, cfg, AR_UPDATE_MODE_LATEST_CAMERA_IMAGE);
        p_ArConfig_setTextureUpdateMode(K.s, cfg, AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER);
        if (K.depth) p_ArConfig_setDepthMode(K.s, cfg, AR_DEPTH_MODE_AUTOMATIC);
        st = p_ArSession_configure(K.s, cfg);
        p_ArConfig_destroy(cfg);
    }
    if (st != AR_SUCCESS || p_ArSession_resume(K.s) != AR_SUCCESS) {
        p_ArSession_destroy(K.s), K.s = NULL;
        A.v.state = ARCORE_UNSUPPORTED;
        return false;
    }
    p_ArFrame_create(K.s, &K.f);
    p_ArPose_create(K.s, NULL, &K.pose);
    p_ArLightEstimate_create(K.s, &K.le);
    p_ArTrackableList_create(K.s, &K.tl);
    K.running = K.wants = true;
    re_android_set_pause_callback(on_pause);
    A.v.state = ARCORE_SEARCHING;
    __android_log_print(ANDROID_LOG_INFO, "RealEngine", "ARCore: session started");
    return true;
}

void arcore_stop(void) {
    if (!K.s) return;
    re_android_set_pause_callback(NULL);
    if (K.running) p_ArSession_pause(K.s);
    p_ArTrackableList_destroy(K.tl), p_ArLightEstimate_destroy(K.le), p_ArPose_destroy(K.pose), p_ArFrame_destroy(K.f);
    p_ArSession_destroy(K.s);
    memset(&K, 0, sizeof K);
    A.v.state = ARCORE_OFF;
    A.v.depth = NULL;
    A.nplanes = 0;
}

static void read_image(void) {
    ArImage* img = NULL;
    if (p_ArFrame_acquireCameraImage(K.s, K.f, &img) != AR_SUCCESS || !img) return; // (not yet: the last one stays)
    ArImageFormat fmt;
    int32_t w = 0, h = 0;
    p_ArImage_getFormat(K.s, img, &fmt);
    p_ArImage_getWidth(K.s, img, &w), p_ArImage_getHeight(K.s, img, &h);
    w &= ~1, h &= ~1;
    if (fmt == AR_IMAGE_FORMAT_YUV_420_888 && w > 1 && h > 1 && ensure_image((uint32_t)w, (uint32_t)h)) {
        const uint8_t *py, *pu, *pv;
        int32_t ly, lu, lv, ry, ru, rv, su, sv;
        p_ArImage_getPlaneData(K.s, img, 0, &py, &ly), p_ArImage_getPlaneRowStride(K.s, img, 0, &ry);
        p_ArImage_getPlaneData(K.s, img, 1, &pu, &lu), p_ArImage_getPlaneRowStride(K.s, img, 1, &ru), p_ArImage_getPlanePixelStride(K.s, img, 1, &su);
        p_ArImage_getPlaneData(K.s, img, 2, &pv, &lv), p_ArImage_getPlaneRowStride(K.s, img, 2, &rv), p_ArImage_getPlanePixelStride(K.s, img, 2, &sv);
        for (int32_t y = 0; y < h; y++) memcpy(A.ybuf + (size_t)y * w, py + (size_t)y * ry, (size_t)w);
        for (int32_t y = 0; y < h / 2; y++) {
            uint8_t* o = A.uvbuf + (size_t)y * w; // (w / 2 pairs a row)
            const uint8_t *u = pu + (size_t)y * ru, *v = pv + (size_t)y * rv;
            for (int32_t x = 0; x < w / 2; x++) o[2 * x] = u[x * su], o[2 * x + 1] = v[x * sv];
        }
        A.v.y = A.ybuf, A.v.uv = A.uvbuf, A.v.img_w = (uint32_t)w, A.v.img_h = (uint32_t)h, A.v.img_serial++;
    }
    p_ArImage_release(img);
}

// The real world's depth (mm, 16 bits; ~160 x 120 on most phones): copied when ARCore has a new one
static void read_depth(void) {
    if (!K.depth) return;
    ArImage* img = NULL;
    if (p_ArFrame_acquireDepthImage16Bits(K.s, K.f, &img) != AR_SUCCESS || !img) return; // (not yet: too few frames seen)
    int64_t ts = 0;
    int32_t w = 0, h = 0;
    p_ArImage_getTimestamp(K.s, img, &ts);
    p_ArImage_getWidth(K.s, img, &w), p_ArImage_getHeight(K.s, img, &h);
    if (ts != K.depth_ts && w > 1 && h > 1 && ensure_depth((uint32_t)w, (uint32_t)h)) {
        const uint8_t* p;
        int32_t len, rs;
        p_ArImage_getPlaneData(K.s, img, 0, &p, &len), p_ArImage_getPlaneRowStride(K.s, img, 0, &rs);
        for (int32_t y = 0; y < h; y++) memcpy(A.dbuf + (size_t)y * w, p + (size_t)y * rs, (size_t)w * 2);
        A.v.depth = A.dbuf, A.v.depth_w = (uint32_t)w, A.v.depth_h = (uint32_t)h, A.v.depth_serial++;
        K.depth_ts = ts;
    }
    p_ArImage_release(img);
}

static void read_light(void) {
    p_ArFrame_getLightEstimate(K.s, K.f, K.le);
    ArLightEstimateState ls = AR_LIGHT_ESTIMATE_STATE_NOT_VALID;
    p_ArLightEstimate_getState(K.s, K.le, &ls);
    A.v.light_valid = ls == AR_LIGHT_ESTIMATE_STATE_VALID;
    if (!A.v.light_valid) return;
    float d[3], c[3], sh[27];
    p_ArLightEstimate_getEnvironmentalHdrMainLightDirection(K.s, K.le, d);
    p_ArLightEstimate_getEnvironmentalHdrMainLightIntensity(K.s, K.le, c);
    p_ArLightEstimate_getEnvironmentalHdrAmbientSphericalHarmonics(K.s, K.le, sh);
    to_engine(d, 1.0f, A.v.light_dir);
    memcpy(A.v.light_rgb, c, 12);
    // its 9 coefficients in ARCore's frame: the bands rotated to the engine's (the linear band is a direction: the same
    // swap; the quadratic band's xy, yz, zx terms permuted as their axes are)
    for (int k = 0; k < 3; k++) {
        float L[9];
        for (int i = 0; i < 9; i++) L[i] = sh[3 * i + k];
        // order (Ramamoorthi & Hanrahan, ARCore's): 1, y, z, x, xy, yz, 3z^2-1, xz, x^2-y^2 (ARCore axes)
        // engine x = -arz, y = arx, z = ary: ar x = e.y, ar y = e.z, ar z = -e.x
        float e[9];
        e[0] = L[0];
        e[1] = L[3];          // e.y = ar.x
        e[2] = L[1];          // e.z = ar.y
        e[3] = -L[2];         // e.x = -ar.z
        e[4] = -L[7];         // e.xy = (-ar.z)(ar.x)
        e[5] = L[4];          // e.yz = ar.x ar.y
        e[6] = L[6];          // (3 e.z^2 - 1) ~ (3 ar.y^2 - 1): the axis of this term changes; kept (a mild error in the ambient)
        e[7] = -L[5];         // e.xz = (-ar.z)(ar.y)
        e[8] = L[8];          // (x^2 - y^2): kept likewise
        for (int i = 0; i < 9; i++) A.v.sh[i][k] = e[i];
    }
}

static void read_planes(void) {
    p_ArSession_getAllTrackables(K.s, AR_TRACKABLE_PLANE, K.tl);
    int32_t n = 0;
    p_ArTrackableList_getSize(K.s, K.tl, &n);
    A.nplanes = 0;
    for (int32_t i = 0; i < n && A.nplanes < MAX_PLANES; i++) {
        ArTrackable* t = NULL;
        p_ArTrackableList_acquireItem(K.s, K.tl, i, &t);
        ArPlane* pl = (ArPlane*)t;
        ArTrackingState ts;
        p_ArTrackable_getTrackingState(K.s, t, &ts);
        ArPlane* by = NULL;
        p_ArPlane_acquireSubsumedBy(K.s, pl, &by);
        if (by) p_ArTrackable_release((ArTrackable*)by);
        if (ts == AR_TRACKING_STATE_TRACKING && !by) {
            Plane* p = &A.planes[A.nplanes];
            ArPlaneType type;
            p_ArPlane_getType(K.s, pl, &type);
            p->up = type == AR_PLANE_HORIZONTAL_UPWARD_FACING;
            p->id = (uint64_t)(uintptr_t)t;
            p_ArPlane_getCenterPose(K.s, pl, K.pose);
            float m[16];
            p_ArPose_getMatrix(K.s, K.pose, m);
            to_engine(m + 12, 100.0f, p->center), to_engine(m + 0, 1.0f, p->axis_x), to_engine(m + 4, 1.0f, p->normal), to_engine(m + 8, 1.0f, p->axis_z);
            float ex, ez;
            p_ArPlane_getExtentX(K.s, pl, &ex), p_ArPlane_getExtentZ(K.s, pl, &ez);
            p->ext_x = ex * 100.0f, p->ext_z = ez * 100.0f;
            int32_t np = 0;
            p_ArPlane_getPolygonSize(K.s, pl, &np); // (floats: x, z pairs)
            float poly[2 * MAX_POLY];
            np = np / 2 > MAX_POLY ? MAX_POLY : np / 2;
            if (np >= 3) {
                p_ArPlane_getPolygon(K.s, pl, poly);
                for (int32_t k = 0; k < np; k++) {
                    float lx = poly[2 * k] * 100.0f, lz = poly[2 * k + 1] * 100.0f;
                    for (int j = 0; j < 3; j++) p->poly[k][j] = p->center[j] + p->axis_x[j] * lx + p->axis_z[j] * lz;
                }
            }
            p->npoly = (uint32_t)(np >= 3 ? np : 0);
            A.nplanes++;
        }
        p_ArTrackable_release(t);
    }
    A.v.planes = A.nplanes;
}

static void read_display_rotation(void) {
    char r[8] = "0";
    re_android_call_string("displayRotation", "()Ljava/lang/String;", r, sizeof r);
    K.rot = (uint32_t)atoi(r);
}

const ArcoreView* arcore_update(uint32_t sw, uint32_t sh) {
    if (A.v.simulated) { sim_update(sw, sh); return &A.v; }
    if (!K.s) {
        // asked for the camera: started once it is allowed (tried every second while the game runs)
        if (A.v.state == ARCORE_NO_PERMISSION && re_time_seconds() > K.next_try) {
            K.next_try = re_time_seconds() + 1.0;
            if (has_camera()) arcore_start();
        }
        return &A.v;
    }
    if (!K.running) return &A.v;
    if (sw != K.sw || sh != K.sh || (A.v.frame % 60u) == 0u) { // (the screen turned: the camera's preview follows)
        read_display_rotation();
        p_ArSession_setDisplayGeometry(K.s, (int32_t)K.rot, (int32_t)sw, (int32_t)sh);
        K.sw = sw, K.sh = sh;
    }
    ArStatus st = p_ArSession_update(K.s, K.f);
    A.v.frame++;
    if (st == AR_ERROR_SESSION_PAUSED) { on_pause(false); return &A.v; }
    if (st != AR_SUCCESS) return &A.v;
    ArCamera* cam = NULL;
    p_ArFrame_acquireCamera(K.s, K.f, &cam);
    ArTrackingState ts = AR_TRACKING_STATE_STOPPED;
    p_ArCamera_getTrackingState(K.s, cam, &ts);
    A.v.state = ts == AR_TRACKING_STATE_TRACKING ? ARCORE_TRACKING : ARCORE_SEARCHING;
    if (ts == AR_TRACKING_STATE_TRACKING) {
        float m[16], pr[16];
        p_ArCamera_getDisplayOrientedPose(K.s, cam, K.pose);
        p_ArPose_getMatrix(K.s, K.pose, m);
        to_engine(m + 12, 100.0f, A.v.pos);
        to_engine(m + 0, 1.0f, A.v.right);
        to_engine(m + 4, 1.0f, A.v.up);
        float back[3];
        to_engine(m + 8, 1.0f, back);
        A.v.fwd[0] = -back[0], A.v.fwd[1] = -back[1], A.v.fwd[2] = -back[2]; // (the camera looks along its -Z)
        p_ArCamera_getProjectionMatrix(K.s, cam, 0.05f, 1000.0f, pr);
        A.v.hfov_deg = 2.0f * atanf(1.0f / pr[0]) * 57.29578f, A.v.vfov_deg = 2.0f * atanf(1.0f / pr[5]) * 57.29578f;
    }
    p_ArCamera_release(cam);
    // where the screen's corners fall in the CPU picture (Vulkan's NDC corners: y down; OpenGL's: y up)
    const float ndc[8] = { -1, 1, 1, 1, -1, -1, 1, -1 };
    float uv[8];
    p_ArFrame_transformCoordinates2d(K.s, K.f, AR_COORDINATES_2D_OPENGL_NORMALIZED_DEVICE_COORDINATES, 4, ndc, AR_COORDINATES_2D_IMAGE_NORMALIZED, uv);
    for (int k = 0; k < 4; k++) A.v.uv_corners[k][0] = uv[2 * k], A.v.uv_corners[k][1] = uv[2 * k + 1];
    if (K.depth) { // (the depth image covers the camera's texture: its coordinates, as ARCore's samples sample it)
        p_ArFrame_transformCoordinates2d(K.s, K.f, AR_COORDINATES_2D_OPENGL_NORMALIZED_DEVICE_COORDINATES, 4, ndc, AR_COORDINATES_2D_TEXTURE_NORMALIZED, uv);
        for (int k = 0; k < 4; k++) A.v.depth_corners[k][0] = uv[2 * k], A.v.depth_corners[k][1] = uv[2 * k + 1];
        if (ts == AR_TRACKING_STATE_TRACKING) read_depth();
    }
    read_image();
    read_light();
    if ((A.v.frame & 7u) == 0u) read_planes(); // (8 times a second at 60 fps: the planes grow slowly)
    return &A.v;
}

bool arcore_hit(float sx, float sy, float out[3], float nrm[3]) {
    if (A.v.simulated || !K.s) goto sim;
    ArHitResultList* hl;
    p_ArHitResultList_create(K.s, &hl);
    p_ArFrame_hitTest(K.s, K.f, sx, sy, hl);
    int32_t n = 0;
    p_ArHitResultList_getSize(K.s, hl, &n);
    bool ok = false;
    if (n > 0) {
        ArHitResult* hr;
        p_ArHitResult_create(K.s, &hr);
        p_ArHitResultList_getItem(K.s, hl, 0, hr);
        p_ArHitResult_getHitPose(K.s, hr, K.pose);
        float m[16];
        p_ArPose_getMatrix(K.s, K.pose, m);
        to_engine(m + 12, 100.0f, out), to_engine(m + 4, 1.0f, nrm);
        p_ArHitResult_destroy(hr);
        ok = true;
    }
    p_ArHitResultList_destroy(hl);
    return ok;
sim:;
#else
bool arcore_start(void) {
    if (!A.v.simulated) A.v.state = ARCORE_UNSUPPORTED;
    return A.v.simulated;
}
void arcore_stop(void) {
    memset(&A.v, 0, sizeof A.v);
    A.nplanes = 0;
}
const ArcoreView* arcore_update(uint32_t sw, uint32_t sh) {
    if (A.v.simulated) sim_update(sw, sh);
    return &A.v;
}
bool arcore_hit(float sx, float sy, float out[3], float nrm[3]) {
#endif
    // the simulated session: the ray through the pixel against the planes
    if (!A.v.simulated || !A.v.hfov_deg) return false;
    float sw = A.sim_screen[0] > 0.0f ? A.sim_screen[0] : 1920.0f, sh = A.sim_screen[1] > 0.0f ? A.sim_screen[1] : 1080.0f; // (sx, sy <= 1: 0..1)
    float nx = sx <= 1.0f ? sx * 2.0f - 1.0f : sx / sw * 2.0f - 1.0f, ny = sy <= 1.0f ? sy * 2.0f - 1.0f : sy / sh * 2.0f - 1.0f;
    float th = tanf(A.v.hfov_deg * 0.00872665f), tv = tanf(A.v.vfov_deg * 0.00872665f);
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = A.v.fwd[k] + A.v.right[k] * nx * th - A.v.up[k] * ny * tv;
    return hit_planes(A.v.pos, d, out, nrm);
}

void arcore_open_store(void) {
#ifdef __ANDROID__
    re_android_call_void("openUrl", "(Ljava/lang/String;)V", (*re_android_env())->NewStringUTF(re_android_env(), "market://details?id=com.google.ar.core"));
#endif
}

int arcore_selftest(void) {
    int fails = 0;
    uint8_t px[16 * 8 * 4];
    for (int i = 0; i < 16 * 8; i++) px[4 * i] = 200, px[4 * i + 1] = 120, px[4 * i + 2] = 60, px[4 * i + 3] = 255;
    if (!arcore_simulate(px, 16, 8, 70.0f)) return 1;
    const float pos[3] = { -300, 0, 150 }, fwd[3] = { 0.8f, 0, -0.6f }, right[3] = { 0, 1, 0 }, up[3] = { 0.6f, 0, 0.8f };
    arcore_sim_camera(pos, fwd, right, up, 70.0f);
    const ArcoreView* v = arcore_update(1920, 1080);
    float tri[64 * 3];
    uint32_t n = arcore_floor_triangles(tri, 64);
    float hit[3], nrm[3];
    bool h = arcore_hit(0.5f, 0.5f, hit, nrm);
    // the YUV of an orange picture: Y ~ 137, U < 128, V > 128
    printf("[RealEngine] ARCore (simulated): %s, %u plane, %u floor vertices, center hit %s (%.0f, %.0f, %.0f), Y %u U %u V %u\n",
           arcore_state_name(v->state), v->planes, n, h ? "yes" : "no", hit[0], hit[1], hit[2], v->y[0], v->uv[0], v->uv[1]);
    if (v->state != ARCORE_TRACKING || v->planes != 1 || n != 6) printf("[RealEngine] FAIL: ARCore: the simulated floor\n"), fails++;
    if (!h || fabsf(hit[2]) > 0.5f || fabsf(hit[0] - (-300.0f + 0.8f / 0.6f * 150.0f)) > 2.0f) printf("[RealEngine] FAIL: ARCore: the hit on the floor\n"), fails++;
    if (!(v->y[0] > 125 && v->y[0] < 150 && v->uv[0] < 110 && v->uv[1] > 150)) printf("[RealEngine] FAIL: ARCore: the picture's YUV\n"), fails++;
    // the depth: the floor 2.5 m ahead along the view axis at the middle (150 cm high, looking 36.9 degrees down),
    // farther at the top row
    uint32_t dmid = v->depth ? v->depth[(v->depth_h / 2) * v->depth_w + v->depth_w / 2] : 0, dtop = v->depth ? v->depth[v->depth_w / 2] : 0;
    printf("[RealEngine] ARCore (simulated) depth: %ux%u, middle %u mm, top %u\n", v->depth_w, v->depth_h, dmid, dtop);
    if (!v->depth || dmid < 2300 || dmid > 2700 || dtop <= dmid) printf("[RealEngine] FAIL: ARCore: the simulated depth\n"), fails++;
    arcore_stop();
    A.v.simulated = false;
    printf("[RealEngine] ARCore self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
