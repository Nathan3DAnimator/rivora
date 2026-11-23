// Upscaling (upscale.h, notas-dev/upscaling.md)
#include "upscale.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mobile.h"
#include "upscale_vendor.h"

static const char* const g_kinds[UPS_COUNT] = { "TSR", "AMD FSR", "NVIDIA DLSS", "Intel XeSS" };
static const char* const g_modes[UPM_COUNT] = { "Preset", "Native", "Quality", "Balanced", "Performance", "Ultra Performance" };
static const float g_scales[UPM_COUNT] = { 0.0f, 1.0f, 0.667f, 0.58f, 0.5f, 0.333f };

const char* upscale_kind_name(int k) { return k >= 0 && k < UPS_COUNT ? g_kinds[k] : ""; }
const char* upscale_mode_name(int m) { return m >= 0 && m < UPM_COUNT ? g_modes[m] : ""; }
float upscale_mode_scale(int m, float preset) {
    if (m <= UPM_PRESET || m >= UPM_COUNT) return preset;
    return g_scales[m];
}

bool upscale_available(int k, const char** why) {
    const char* w = NULL;
    bool ok = false;
    if (k == UPS_TSR) ok = true;
    else if (k > UPS_TSR && k < UPS_COUNT) ok = ups_vendor_usable(k, &w); // (its runtime beside the executable, the GPU: upscale_vendor.c)
    else w = "unknown";
    if (why) *why = w;
    return ok;
}
int upscale_resolve(int k) { return upscale_available(k, NULL) ? k : UPS_TSR; }

float upscale_dynamic_floor(float top, int min_pct) {
    if (min_pct > 0) { // (2026-11-16) the dev's Dynamic Resolution Min (Unreal's MinScreenPercentage): as far down as that
        float m = (float)min_pct / 100.0f;
        if (m < 0.25f) m = 0.25f;
        return m < top ? m : top;
    }
    float lo = top * 0.7f; // Auto: 70% of the mode's share, never under half the output
    if (lo < 0.5f) lo = top < 0.5f ? top : 0.5f;
    return lo;
}

float upscale_dynamic(UpsDynamic* d, double gpu_ms, float target_fps, float min_s, float max_s) {
    if (min_s < 0.25f) min_s = 0.25f;
    if (max_s > 1.0f) max_s = 1.0f;
    if (min_s > max_s) min_s = max_s;
    if (target_fps <= 0.0f) return d->scale = max_s;
    if (d->scale <= 0.0f) d->scale = max_s;
    if (d->scale > max_s) d->scale = max_s;
    if (d->scale < min_s) d->scale = min_s;
    if (gpu_ms > 0.0) d->window_ms += gpu_ms, d->window_frames++;
    // half a second of frames a window (at the target's rate)
    uint32_t per = (uint32_t)(target_fps * 0.5f);
    if (per < 8) per = 8;
    if (d->window_frames >= per) {
        double mean = d->window_ms / (double)d->window_frames;
        float next = mobile_res_step(d->scale, min_s, max_s, mean, 1000.0 / (double)target_fps, &d->calm);
        if (fabsf(next - d->scale) > 0.001f) d->scale = next, d->steps++;
        d->window_ms = 0.0, d->window_frames = 0;
    }
    return d->scale;
}

bool upscale_selftest(void) {
    bool ok = true;
#define T(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: upscale: " __VA_ARGS__); printf("\n"); ok = false; } } while (0)
    T(fabsf(upscale_mode_scale(UPM_QUALITY, 0.75f) - 0.667f) < 1e-3f, "Quality is not 67%%");
    T(fabsf(upscale_mode_scale(UPM_PERFORMANCE, 0.75f) - 0.5f) < 1e-3f, "Performance is not 50%%");
    T(fabsf(upscale_mode_scale(UPM_PRESET, 0.75f) - 0.75f) < 1e-3f, "Preset is not the preset's share");
    T(upscale_mode_scale(UPM_NATIVE, 0.5f) == 1.0f, "Native is not 100%%");
    T(upscale_available(UPS_TSR, NULL) && upscale_resolve(UPS_DLSS) == (upscale_available(UPS_DLSS, NULL) ? UPS_DLSS : UPS_TSR),
      "an unavailable upscaler does not fall back to TSR");
    // a GPU of 16 ms at full size (its time with the pixels: the share squared), a 100 fps target: the share comes down
    // under 10 ms and stays (no oscillation once there)
    UpsDynamic d = { 0 };
    float s = 1.0f;
    uint32_t steps_late = 0;
    for (int f = 0; f < 3000; f++) {
        double gpu = 1.0 + 15.0 * (double)(s * s);
        float n = upscale_dynamic(&d, gpu, 100.0f, 0.5f, 1.0f);
        if (f > 2000 && n != s) steps_late++;
        s = n;
    }
    double end_ms = 1.0 + 15.0 * (double)(s * s);
    printf("[RealEngine] Upscale: dynamic resolution settled at %.0f%% (%.2f ms of 10), %u steps, %u in the last third\n", s * 100.0f, end_ms,
           d.steps, steps_late);
    T(end_ms <= 10.0 && s >= 0.5f, "the dynamic resolution did not reach the target (%.2f ms at %.0f%%)", end_ms, s * 100.0f);
    T(steps_late <= 2, "the dynamic resolution keeps changing (%u steps late)", steps_late);
    // well under the target: back up to the most
    for (int f = 0; f < 3000; f++) s = upscale_dynamic(&d, 2.0, 100.0f, 0.5f, 1.0f);
    T(s == 1.0f, "the dynamic resolution did not come back up (%.0f%%)", s * 100.0f);
    for (int k = 1; k < UPS_COUNT; k++) {
        const char* why = NULL;
        bool on = upscale_available(k, &why);
        printf("[RealEngine] Upscale: %s %s%s%s\n", g_kinds[k], on ? "available" : "not available", why ? ": " : "", why ? why : "");
    }
#undef T
    if (ok) printf("[RealEngine] Upscale test: OK\n");
    return ok;
}
