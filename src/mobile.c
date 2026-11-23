// The game on phones and tablets running smoothly (mobile.h; notas-dev/android.md "Rodar liso").
#include "mobile.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gi.h"
#include "material.h"
#include "platform.h"
#include "render.h"
#include "vkctx.h"

#ifdef __ANDROID__
typedef struct AThermalManager AThermalManager; // (android/thermal.h does not compile as C in NDK 27: loaded by name)
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include "../android/re_android.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static MobileState S;
static struct {
    double window_t, gpu_sum, cpu_sum, log_t, sys_t;
    int window_n, calm, low_memory_seen;
    uint64_t frames_logged;
    double log_from, deadline;
    bool log_always;
    VkCtx* vk;
#ifdef __ANDROID__
    AThermalManager* thermal;
    float (*headroom_fn)(AThermalManager*, int);
    int (*status_fn)(AThermalManager*);
    void* hint_session;
    void (*hint_report)(void*, int64_t);
    void (*hint_target)(void*, int64_t);
#endif
} P;

const MobileState* mobile_state(void) { return &S; }

// --- The device's tier ---

static int number_after(const char* s, const char* key) {
    const char* p = strstr(s, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p && !isdigit((unsigned char)*p)) {
        if (isalpha((unsigned char)*p) && *p != 't' && *p != 'm') return -1; // ("Adreno (TM) 740": the digits after the mark)
        p++;
    }
    return isdigit((unsigned char)*p) ? atoi(p) : -1;
}

int mobile_tier_for(const char* gpu_name, uint64_t ram_bytes) {
    char n[256];
    size_t i = 0;
    for (; gpu_name && gpu_name[i] && i + 1 < sizeof n; i++) n[i] = (char)tolower((unsigned char)gpu_name[i]);
    n[i] = 0;
    int tier = MOBILE_MEDIUM;
    int a = number_after(n, "adreno"), m = number_after(n, "mali-g");
    if (a >= 0) tier = (a >= 730 || (a >= 800 && a < 1000) || (a >= 8 && a < 10)) ? MOBILE_HIGH : a >= 640 ? MOBILE_MEDIUM : MOBILE_LOW;
    else if (strstr(n, "immortalis") || strstr(n, "xclipse")) tier = MOBILE_HIGH;
    else if (m >= 0) tier = m >= 710 ? MOBILE_HIGH : (m >= 610 || (m >= 76 && m < 100)) ? MOBILE_MEDIUM : MOBILE_LOW;
    else if (strstr(n, "powervr") || strstr(n, "mali-t")) tier = MOBILE_LOW;
    else if (strstr(n, "nvidia") || strstr(n, "geforce") || strstr(n, "radeon") || strstr(n, "intel")) tier = MOBILE_HIGH; // (the PC, an emulator)
    // the memory caps it: a phone of 4 GB shares them with the system and the other apps
    const uint64_t gb = 1ull << 30;
    if (ram_bytes && ram_bytes < 4 * gb - gb / 4) tier = MOBILE_LOW;
    else if (ram_bytes && ram_bytes < 6 * gb - gb / 4 && tier > MOBILE_MEDIUM) tier = MOBILE_MEDIUM;
    return tier;
}

static uint64_t ram_bytes(void) {
#ifdef __ANDROID__
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? (uint64_t)pages * (uint64_t)size : 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX ms = { sizeof ms };
    return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
#else
    return 0;
#endif
}

// --- Time ---

static void sleep_s(double s) {
    if (s <= 0.0) return;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000.0));
#else
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
#endif
}

// --- The dynamic resolution's controller ---

float mobile_res_step(float res, float res_min, float res_max, double gpu_ms, double budget_ms, int* calm_windows) {
    // over 92% of the budget: down at once, more when far over; under 70% for 4 windows (2 s): up a step. Steps of 0.05:
    // each one reallocates the frame's targets (a short pause), so few and only when sustained
    float next = res;
    if (gpu_ms > budget_ms * 0.92) {
        double over = gpu_ms / (budget_ms * 0.85); // (the GPU's cost goes with the pixels: the share's square)
        float want = res / (float)sqrt(over);
        next = floorf(want * 20.0f) / 20.0f;
        if (next > res - 0.05f) next = res - 0.05f;
        *calm_windows = 0;
    } else if (gpu_ms < budget_ms * 0.70) {
        if (++*calm_windows >= 4) next = res + 0.05f, *calm_windows = 0;
    } else *calm_windows = 0;
    if (next < res_min) next = res_min;
    if (next > res_max) next = res_max;
    return roundf(next * 100.0f) / 100.0f;
}

// --- Start ---

#ifdef __ANDROID__
#include <sys/system_properties.h>
static int debug_prop(const char* name) { // (adb shell setprop debug.realengine.<name> <n>: the measures; -1 unset)
    char v[PROP_VALUE_MAX] = "";
    return __system_property_get(name, v) > 0 ? atoi(v) : -1;
}
#endif

bool mobile_init(VkCtx* vk, const MobileConfig* cfg) {
    memset(&S, 0, sizeof S);
    memset(&P, 0, sizeof P);
    P.vk = vk;
    P.log_t = P.window_t = P.sys_t = re_time_seconds();
    const char* env = getenv("REALENGINE_MOBILE");
#ifdef __ANDROID__
    S.on = !(env && env[0] == '0') && debug_prop("debug.realengine.mobile") != 0;
    P.log_always = true; // (logcat shows the frame rate even with the profile off)
#else
    S.on = env && env[0] == '1';
#endif
    if (!S.on) return false;
    uint64_t ram = ram_bytes();
    S.tier = cfg && cfg->quality >= MOBILE_LOW && cfg->quality <= MOBILE_HIGH ? cfg->quality : mobile_tier_for(vk->gpu_props.deviceName, ram);
    const char* et = getenv("REALENGINE_MOBILE_TIER");
    if (et && atoi(et) >= MOBILE_LOW && atoi(et) <= MOBILE_HIGH) S.tier = atoi(et);
    S.target_fps = cfg && cfg->target_fps >= 20 ? cfg->target_fps : 60;
    const char* ef = getenv("REALENGINE_MOBILE_FPS");
    if (ef && atoi(ef) >= 20) S.target_fps = atoi(ef);
#ifdef __ANDROID__
    if (debug_prop("debug.realengine.fps") >= 20) S.target_fps = debug_prop("debug.realengine.fps");
    if (debug_prop("debug.realengine.tier") >= MOBILE_LOW && debug_prop("debug.realengine.tier") <= MOBILE_HIGH) S.tier = debug_prop("debug.realengine.tier");
#endif
    S.fps_cap = S.target_fps;
    static const float res_min[4] = { 0, 0.5f, 0.55f, 0.6f }, res_max[4] = { 0, 0.7f, 0.8f, 0.9f }, scale[4] = { 0, 0.7f, 0.8f, 0.9f };
    S.res_min = res_min[S.tier], S.res_max = res_max[S.tier], S.res = S.res_max;
    if (cfg && !cfg->dynamic_res) S.res_min = S.res_max;
    S.content_scale = cfg && cfg->content_scale >= 0.3f && cfg->content_scale <= 1.0f ? cfg->content_scale : scale[S.tier];
    S.thermal = -1, S.headroom = -1.0f;
    // the textures: a share of a memory the system and the other apps share too
    static const uint64_t budget_mb[4] = { 0, 384, 640, 1024 };
    S.texture_budget = budget_mb[S.tier] << 20;
    if (ram && S.texture_budget > ram / 4) S.texture_budget = ram / 4;
    material_set_texture_budget(S.texture_budget);
#ifdef __ANDROID__
    re_android_set_content_scale(S.content_scale);
    re_android_set_frame_rate((float)S.fps_cap);
    vk->swapchain_dirty = true; // (the window's buffers changed size)
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib) {
        AThermalManager* (*acquire)(void) = (AThermalManager * (*)(void)) dlsym(lib, "AThermal_acquireManager"); // (Android 11)
        P.status_fn = (int (*)(AThermalManager*))dlsym(lib, "AThermal_getCurrentThermalStatus");
        P.thermal = acquire && P.status_fn ? acquire() : NULL;
        P.headroom_fn = (float (*)(AThermalManager*, int))dlsym(lib, "AThermal_getThermalHeadroom"); // (Android 12)
        void* (*get_mgr)(void) = (void* (*)(void))dlsym(lib, "APerformanceHint_getManager"); // (Android 13: ADPF)
        void* (*create)(void*, const int32_t*, size_t, int64_t) = (void* (*)(void*, const int32_t*, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
        P.hint_report = (void (*)(void*, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        P.hint_target = (void (*)(void*, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
        void* mgr = get_mgr ? get_mgr() : NULL;
        int32_t tid = (int32_t)gettid();
        if (mgr && create && P.hint_report) P.hint_session = create(mgr, &tid, 1, (int64_t)(1e9 / S.fps_cap));
        S.perf_hint = P.hint_session != NULL;
    }
#else
    (void)vk;
#endif
    static const char* const names[4] = { "", "Low", "Medium", "High" };
    printf("[RealEngine] Mobile: tier %s (%s, %.1f GB), %d fps, window %.0f%%, resolution %.0f-%.0f%%, textures %llu MB, ADPF %s\n",
           names[S.tier], vk->gpu_props.deviceName, (double)ram / (1 << 30), S.target_fps, S.content_scale * 100.0f, S.res_min * 100.0f,
           S.res_max * 100.0f, (unsigned long long)(S.texture_budget >> 20), S.perf_hint ? "on" : "off");
    fflush(stdout);
    return true;
}

// The render's share of the output: on a phone the window is already at the content scale; the PC (REALENGINE_MOBILE=1)
// can't shrink its window, so the same pixels come from the TSR's share
static float render_share(void) {
#ifdef __ANDROID__
    return S.res;
#else
    return S.res * S.content_scale;
#endif
}

void mobile_apply(GiSettings* gi, int* msaa) {
    if (!S.on) return;
    int level = S.tier == MOBILE_HIGH ? 2 : S.tier == MOBILE_MEDIUM ? 1 : 0;
    render_scalability(level, msaa, gi);
    *msaa = 1;
    gi->rt_shadows = gi->reflections = false;          // (no traced light on a phone's power)
    gi->surface_cache = false;
    gi->probes = S.tier >= MOBILE_MEDIUM;
    gi->screen_clouds = gi->clouds_half = false;       // the clouds' panorama
    gi->volumetric_fog = S.tier >= MOBILE_HIGH;
    gi->vsm_levels = S.tier == MOBILE_HIGH ? 6 : S.tier == MOBILE_MEDIUM ? 5 : 4;
    gi->vsm_res = S.tier == MOBILE_HIGH ? 2048 : 1024;
    gi->vsm_taps = S.tier == MOBILE_HIGH ? 12 : S.tier == MOBILE_MEDIUM ? 8 : 4;
    gi->vsm_full_budget = 1;
    gi->lod_error_px = S.tier == MOBILE_HIGH ? 2.0f : S.tier == MOBILE_MEDIUM ? 4.0f : 6.0f;
    gi->tess_max = S.tier == MOBILE_HIGH ? 4 : 1, gi->tess_px = 8.0f, gi->tess_budget = 0.5f; // (Q17: a phone dices little)
    gi->far_res = S.tier == MOBILE_HIGH ? 1024 : 512, gi->impostor_res = 512; // (Q21: the impostors early, small atlases)
    gi->impostor_px = S.tier == MOBILE_HIGH ? 32.0f : 40.0f;
    gi->tsr_scale = render_share();
}

// --- Every frame ---

static void set_cap(int fps) {
    if (fps == S.fps_cap) return;
    S.fps_cap = fps;
#ifdef __ANDROID__
    re_android_set_frame_rate((float)fps);
    if (P.hint_session && P.hint_target) P.hint_target(P.hint_session, (int64_t)(1e9 / fps));
#endif
    printf("[RealEngine] Mobile: %d fps now (thermal %d, headroom %.2f, battery saver %s)\n", fps, S.thermal, S.headroom,
           S.power_save ? "on" : "off");
}

static void system_poll(void) { // (once a second: the headroom must not be asked more often)
#ifdef __ANDROID__
    if (P.thermal) S.thermal = P.status_fn(P.thermal);
    if (P.thermal && P.headroom_fn) {
        float h = P.headroom_fn(P.thermal, 5); // (in 5 s at this load)
        if (!isnan(h)) S.headroom = h;
    }
    static int tick;
    if (++tick % 5 == 0) { // the battery saver (Java's PowerManager)
        char b[8] = "";
        if (re_android_call_string("powerSave", "()Ljava/lang/String;", b, sizeof b)) S.power_save = b[0] == '1';
    }
    int lm = re_android_low_memory_count();
    if (lm != P.low_memory_seen) { // the system is short: the textures give a quarter back
        P.low_memory_seen = lm;
        S.texture_budget = S.texture_budget / 4 * 3;
        if (S.texture_budget < (128ull << 20)) S.texture_budget = 128ull << 20;
        material_set_texture_budget(S.texture_budget);
        printf("[RealEngine] Mobile: low memory: textures %llu MB\n", (unsigned long long)(S.texture_budget >> 20));
    }
#endif
    const char* et = getenv("REALENGINE_MOBILE_HEADROOM"); // (the tests: a hot device made up)
    if (et) S.headroom = (float)atof(et);
    // the heat: before the system throttles, the frame's cost goes down (first the pixels, then half the rate)
    bool hot = S.headroom >= 0.95f || S.thermal >= 3, warm = S.headroom >= 0.85f || S.thermal == 2;
    bool cool = (S.headroom < 0 || S.headroom < 0.75f) && S.thermal <= 1;
    static const float res_max[4] = { 0, 0.7f, 0.8f, 0.9f };
    float top = res_max[S.tier];
    if (hot || warm) S.res_max = top - 0.1f;
    else if (cool) S.res_max = top;
    if (S.res > S.res_max) S.res = S.res_max;
    int cap = S.target_fps;
    if (hot) cap = S.target_fps / 2 < 30 ? 30 : S.target_fps / 2;
    if (S.power_save && cap > 30) cap = 30;
    if (hot || S.power_save || cool) set_cap(cap);
}

void mobile_frame_end(GiSettings* gi, double frame_start, double cpu_ms, double gpu_ms) {
    double now = re_time_seconds();
    if (!S.on) { // (Android with the profile off: only the frame rate in logcat)
        if (!P.log_always) return;
        P.window_n++;
        if (now - P.log_t >= 5.0) {
            printf("[RealEngine] Mobile (off): %.1f fps, CPU %.2f ms, GPU %.2f ms, %ux%u\n", P.window_n / (now - P.log_t), cpu_ms, gpu_ms,
                   P.vk ? P.vk->extent.width : 0, P.vk ? P.vk->extent.height : 0);
            fflush(stdout);
            P.log_t = now, P.window_n = 0;
        }
        return;
    }
    P.gpu_sum += gpu_ms, P.cpu_sum += cpu_ms, P.window_n++;
#ifdef __ANDROID__
    if (P.hint_session) P.hint_report(P.hint_session, (int64_t)(cpu_ms * 1e6)); // (ADPF: the CPU's work this frame)
#endif
    if (now - P.sys_t >= 1.0) P.sys_t = now, system_poll();
    if (now - P.window_t >= 0.5 && P.window_n > 0) { // a window: the averages, the resolution's step
        S.gpu_avg_ms = P.gpu_sum / P.window_n, S.cpu_avg_ms = P.cpu_sum / P.window_n;
        S.fps = P.window_n / (now - P.window_t);
        float next = mobile_res_step(S.res, S.res_min, S.res_max, S.gpu_avg_ms, 1000.0 / S.fps_cap, &P.calm);
        if (next != S.res) S.res = next, S.res_changes++;
        P.gpu_sum = P.cpu_sum = 0.0, P.window_n = 0, P.window_t = now;
    }
    if (gi) gi->tsr_scale = render_share();
    if (now - P.log_t >= 5.0) { // (logcat: what a tester reads)
        P.log_t = now;
        printf("[RealEngine] Mobile: %.1f fps (cap %d), CPU %.2f ms, GPU %.2f ms, resolution %.0f%% of %ux%u, thermal %d, headroom %.2f\n",
               S.fps, S.fps_cap, S.cpu_avg_ms, S.gpu_avg_ms, S.res * 100.0f, P.vk ? P.vk->extent.width : 0, P.vk ? P.vk->extent.height : 0,
               S.thermal, S.headroom);
        fflush(stdout);
    }
    // the pace: one frame a period at the cap, on a running deadline (no drift; a late frame starts a new one). The
    // display, told the rate, presents on time and no frame is drawn early only to be dropped
    (void)frame_start;
    double period = 1.0 / S.fps_cap;
    now = re_time_seconds();
    P.deadline += period;
    if (P.deadline < now - period || P.deadline > now + 2.0 * period) P.deadline = now; // (late, or the cap changed)
    if (now < P.deadline - 0.0005) sleep_s(P.deadline - 0.0005 - now);
}

// --- The test ---

int mobile_selftest(void) {
    int fails = 0;
#define MCHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: mobile: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    const uint64_t gb = 1ull << 30;
    MCHECK(mobile_tier_for("Adreno (TM) 740", 12 * gb) == MOBILE_HIGH, "Adreno 740 not High");
    MCHECK(mobile_tier_for("Adreno (TM) 650", 8 * gb) == MOBILE_MEDIUM, "Adreno 650 not Medium");
    MCHECK(mobile_tier_for("Adreno (TM) 610", 4 * gb) == MOBILE_LOW, "Adreno 610 not Low");
    MCHECK(mobile_tier_for("Mali-G715", 8 * gb) == MOBILE_HIGH, "Mali-G715 not High");
    MCHECK(mobile_tier_for("Mali-G78 MP14", 8 * gb) == MOBILE_MEDIUM, "Mali-G78 not Medium");
    MCHECK(mobile_tier_for("Mali-G57 MC2", 4 * gb) == MOBILE_LOW, "Mali-G57 not Low");
    MCHECK(mobile_tier_for("Immortalis-G925", 12 * gb) == MOBILE_HIGH, "Immortalis not High");
    MCHECK(mobile_tier_for("Adreno (TM) 740", 3 * gb) == MOBILE_LOW, "3 GB of memory not Low");
    MCHECK(mobile_tier_for("Mali-G715", 4 * gb + gb / 2) == MOBILE_MEDIUM, "4.5 GB not capped to Medium");
    // the resolution: over budget goes down at once, far over by more; a calm GPU climbs only after 2 s
    int calm = 0;
    float r = mobile_res_step(0.9f, 0.6f, 0.9f, 20.0, 16.67, &calm);
    MCHECK(r <= 0.8f && r >= 0.6f, "20 ms against 16.7 gave %.2f", r);
    float r2 = mobile_res_step(0.9f, 0.6f, 0.9f, 16.0, 16.67, &calm);
    MCHECK(r2 <= 0.85f, "16 ms (96%%) gave %.2f", r2);
    MCHECK(mobile_res_step(0.6f, 0.6f, 0.9f, 40.0, 16.67, &calm) == 0.6f, "went under its minimum");
    calm = 0;
    float up = 0.7f;
    for (int i = 0; i < 3; i++) up = mobile_res_step(up, 0.6f, 0.9f, 8.0, 16.67, &calm);
    MCHECK(up == 0.7f, "climbed before 4 calm windows (%.2f)", up);
    up = mobile_res_step(up, 0.6f, 0.9f, 8.0, 16.67, &calm);
    MCHECK(up > 0.7f && up <= 0.75f + 1e-4f, "did not climb a step after 4 calm windows (%.2f)", up);
    MCHECK(mobile_res_step(0.75f, 0.6f, 0.9f, 13.0, 16.67, &calm) == 0.75f, "moved inside the band (78%%)");
    // a steady load: a GPU whose cost follows the pixels settles under the budget without hunting
    float res = 0.9f;
    int changes = 0;
    calm = 0;
    for (int w = 0; w < 120; w++) {
        double ms = 26.0 * res * res; // (26 ms at 100%: 21 ms at 90%, over a 16.7 ms budget)
        float n = mobile_res_step(res, 0.5f, 0.9f, ms, 16.67, &calm);
        if (n != res) changes++, res = n;
    }
    double settled = 26.0 * res * res;
    MCHECK(settled < 16.67 && settled > 16.67 * 0.6, "settled at %.2f ms (%.2f)", settled, res);
    MCHECK(changes <= 4, "%d steps in 60 s: it hunts", changes);
    printf("[RealEngine] mobile: steady load settled at %.0f%% (%.1f ms of 16.7) in %d steps\n", res * 100.0f, settled, changes);
    if (!fails) printf("[RealEngine] mobile self-test: ok\n");
#undef MCHECK
    return fails;
}
