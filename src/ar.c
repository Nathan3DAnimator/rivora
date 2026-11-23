#include "ar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"

#ifdef __ANDROID__
#include <android/looper.h>
#include <android/sensor.h>
#endif

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// McCamy's approximation: the correlated color temperature from the CIE 1931 chromaticity
static float cct_mccamy(float x, float y) {
    float n = (x - 0.3320f) / (0.1858f - y);
    return 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;
}

void ar_estimate(const uint8_t* rgba, uint32_t w, uint32_t h, float hfov_deg, ArLight* o) {
    memset(o, 0, sizeof *o);
    if (!rgba || !w || !h) return;
    // a coarse grid of the picture (64 x 48 cells at most): the means, the brightest cells
    uint32_t gx = w < 64 ? w : 64, gy = h < 48 ? h : 48;
    double sum[3] = { 0, 0, 0 }, lsum = 0.0, logsum = 0.0;
    float lum[64 * 48];
    float cells[64 * 48][3];
    float tanx = tanf(hfov_deg * 0.5f * 0.017453293f), tany = tanx * (float)h / (float)w;
    for (uint32_t cy = 0; cy < gy; cy++)
        for (uint32_t cx = 0; cx < gx; cx++) {
            uint32_t x0 = cx * w / gx, x1 = (cx + 1) * w / gx, y0 = cy * h / gy, y1 = (cy + 1) * h / gy;
            double c[3] = { 0, 0, 0 };
            uint32_t n = 0;
            for (uint32_t y = y0; y < y1; y += 2)
                for (uint32_t x = x0; x < x1; x += 2, n++) {
                    const uint8_t* p = rgba + 4 * ((size_t)y * w + x);
                    for (int k = 0; k < 3; k++) c[k] += srgb_to_linear((float)p[k] / 255.0f);
                }
            float* cc = cells[cy * gx + cx];
            for (int k = 0; k < 3; k++) cc[k] = n ? (float)(c[k] / n) : 0.0f, sum[k] += cc[k];
            float l = 0.2126f * cc[0] + 0.7152f * cc[1] + 0.0722f * cc[2];
            lum[cy * gx + cx] = l, lsum += l, logsum += log(l + 1e-4);
        }
    uint32_t nc = gx * gy;
    float mean[3] = { (float)(sum[0] / nc), (float)(sum[1] / nc), (float)(sum[2] / nc) };
    o->mean_lum = (float)(lsum / nc);
    // the exposure: the log-average luminance as a middle grey's (0.18), in the camera's encoded scale -> EV100 as a
    // daylight scene's (an encoded 0.18 ~ EV100 12 at the camera's own exposure: relative stops from there)
    float logavg = (float)exp(logsum / nc);
    o->ev100 = 12.0f + log2f(fmaxf(logavg, 1e-4f) / 0.18f);
    // the white balance: gray world (the mean's chromaticity is the light's)
    float X = 0.4124f * mean[0] + 0.3576f * mean[1] + 0.1805f * mean[2];
    float Y = 0.2126f * mean[0] + 0.7152f * mean[1] + 0.0722f * mean[2];
    float Z = 0.0193f * mean[0] + 0.1192f * mean[1] + 0.9505f * mean[2];
    float s = X + Y + Z;
    float cx = s > 1e-6f ? X / s : 0.3127f, cy = s > 1e-6f ? Y / s : 0.3290f;
    o->kelvin = fminf(fmaxf(cct_mccamy(cx, cy), 1500.0f), 15000.0f);
    o->tint = (mean[0] + mean[2]) * 0.5f - mean[1]; // (+ magenta, - green)
    float mx = fmaxf(mean[0], fmaxf(mean[1], mean[2]));
    for (int k = 0; k < 3; k++) o->color[k] = mx > 1e-6f ? mean[k] / mx : 1.0f;
    // the main light: the brightest 3% of the cells, their luminance-weighted center as a direction in the view
    float thr = 0.0f;
    {
        float sorted[64 * 48];
        memcpy(sorted, lum, sizeof(float) * nc);
        for (uint32_t i = 0; i < nc; i++) // (a partial selection: the top 3%)
            for (uint32_t j = i + 1; j < nc && i < nc / 33 + 1; j++)
                if (sorted[j] > sorted[i]) { float t = sorted[i]; sorted[i] = sorted[j], sorted[j] = t; }
        thr = sorted[nc / 33];
    }
    double dx = 0, dy = 0, wsum = 0, peak = 0;
    for (uint32_t cy2 = 0; cy2 < gy; cy2++)
        for (uint32_t cx2 = 0; cx2 < gx; cx2++) {
            float l = lum[cy2 * gx + cx2];
            if (l < thr) continue;
            double wgt = l - thr + 1e-4;
            dx += wgt * (((cx2 + 0.5) / gx) * 2.0 - 1.0), dy += wgt * (1.0 - ((cy2 + 0.5) / gy) * 2.0), wsum += wgt;
            if (l > peak) peak = l;
        }
    o->strength = (float)fminf(fmaxf((peak / fmaxf(o->mean_lum, 1e-4f) - 1.5) / 6.0, 0.0), 1.0);
    if (wsum > 0.0 && o->strength > 0.05f) {
        float v[3] = { (float)(dx / wsum) * tanx, (float)(dy / wsum) * tany, 1.0f };
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; k++) o->dir_view[k] = v[k] / l;
    }
    // the ambient: 9 SH of the picture spread over the front hemisphere (each cell a direction), mirrored behind
    for (uint32_t cy2 = 0; cy2 < gy; cy2++)
        for (uint32_t cx2 = 0; cx2 < gx; cx2++) {
            float u = ((cx2 + 0.5f) / gx) * 2.0f - 1.0f, vv = 1.0f - ((cy2 + 0.5f) / gy) * 2.0f;
            float d[3] = { u * 1.6f, vv * 1.6f, 1.0f }; // (the picture covers ~120 degrees of the sphere)
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int mirror = 0; mirror < 2; mirror++) {
                float x = d[0] / l, y = d[1] / l, z = (mirror ? -d[2] : d[2]) / l;
                float b[9] = { 0.282095f, 0.488603f * y, 0.488603f * z, 0.488603f * x, 1.092548f * x * y, 1.092548f * y * z,
                               0.315392f * (3.0f * z * z - 1.0f), 1.092548f * x * z, 0.546274f * (x * x - y * y) };
                const float* c = cells[cy2 * gx + cx2];
                for (int i = 0; i < 9; i++)
                    for (int k = 0; k < 3; k++) o->sh[i][k] += c[k] * b[i] * (12.566371f / (2.0f * (float)nc));
            }
        }
}

void ar_blend(ArLight* c, const ArLight* n, float t) {
    float* a = (float*)c;
    const float* b = (const float*)n;
    for (size_t i = 0; i < sizeof(ArLight) / sizeof(float); i++) a[i] += (b[i] - a[i]) * t;
}

float ar_apply(World* w, const ArLight* l, const float f[3], const float r[3], const float u[3]) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_LIGHT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CLight* li = ecs_arch_column(a, COMP_LIGHT);
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            if (li[i].type == LIGHT_DIRECTIONAL) { // the sun: the real light's color, strength and (found) direction
                memcpy(li[i].color, l->color, 12);
                li[i].intensity = 10.0f + 110000.0f * powf(2.0f, l->ev100 - 15.0f) * (0.3f + 0.7f * l->strength);
                if (l->dir_view[2] > 0.0f) { // (toward the light: the sun shines from there)
                    float d[3];
                    for (int k = 0; k < 3; k++) d[k] = -(r[k] * l->dir_view[0] + u[k] * l->dir_view[1] + f[k] * l->dir_view[2]);
                    float pitch = asinf(fminf(fmaxf(d[2], -1.0f), 1.0f)) * 57.29578f, yaw = atan2f(d[1], d[0]) * 57.29578f;
                    tr[i].rotation[0] = fminf(pitch, -5.0f), tr[i].rotation[1] = yaw; // (never from below the horizon)
                }
            } else if (li[i].type == LIGHT_SKY) {
                memcpy(li[i].color, l->color, 12);
                li[i].intensity = fminf(fmaxf(l->mean_lum * 4.0f, 0.05f), 4.0f);
            }
        }
    }
    return l->ev100;
}

bool ar_device_rotation(float fwd[3], float right[3], float up[3]) {
#ifdef __ANDROID__
    static ASensorEventQueue* q;
    static const ASensor* s;
    static float rv[4] = { 0, 0, 0, 1 };
    if (!s) {
        ASensorManager* m = ASensorManager_getInstanceForPackage("com.realengine.game");
        if (!m) return false;
        s = ASensorManager_getDefaultSensor(m, ASENSOR_TYPE_ROTATION_VECTOR);
        if (!s) return false;
        q = ASensorManager_createEventQueue(m, ALooper_forThread() ? ALooper_forThread() : ALooper_prepare(0), 1, NULL, NULL);
        if (!q) return false;
        ASensorEventQueue_enableSensor(q, s);
        ASensorEventQueue_setEventRate(q, s, 5000); // (200 Hz: the AR camera must not lag the hand)
    }
    ASensorEvent ev;
    while (ASensorEventQueue_getEvents(q, &ev, 1) > 0)
        if (ev.type == ASENSOR_TYPE_ROTATION_VECTOR) rv[0] = ev.data[0], rv[1] = ev.data[1], rv[2] = ev.data[2], rv[3] = ev.data[3];
    // the device's frame in the world's (Android: X east, Y north, Z up); its camera looks along -Z of the device
    float x = rv[0], y = rv[1], z = rv[2], w = rv[3];
    float m[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                      { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                      { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    // engine: X forward (north), Y right (east), Z up; the camera's forward = the device's -Z, right = +X, up = +Y
    for (int k = 0; k < 3; k++) {
        float cf = -m[k][2], cr = m[k][0], cu = m[k][1];
        int e = k == 0 ? 1 : k == 1 ? 0 : 2; // (east -> Y, north -> X)
        fwd[e] = cf, right[e] = cr, up[e] = cu;
    }
    return true;
#else
    (void)fwd, (void)right, (void)up;
    return false;
#endif
}

// --- The self-test ---
static void fill(uint8_t* img, uint32_t w, uint32_t h, const uint8_t c[3], int spot_x, int spot_y) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = img + 4 * ((size_t)y * w + x);
            bool spot = spot_x >= 0 && abs((int)x - spot_x) < (int)w / 16 && abs((int)y - spot_y) < (int)h / 16;
            for (int k = 0; k < 3; k++) p[k] = spot ? 255 : c[k];
            p[3] = 255;
        }
}

int ar_selftest(void) {
    int fails = 0;
    const uint32_t W = 320, H = 240;
    uint8_t* img = malloc((size_t)W * H * 4);
    if (!img) return 1;
    ArLight a, b, c;
    fill(img, W, H, (const uint8_t[3]){ 200, 150, 90 }, -1, -1); // tungsten-ish
    ar_estimate(img, W, H, 70.0f, &a);
    fill(img, W, H, (const uint8_t[3]){ 120, 150, 200 }, -1, -1); // shade, a blue sky
    ar_estimate(img, W, H, 70.0f, &b);
    fill(img, W, H, (const uint8_t[3]){ 90, 90, 90 }, W / 5, H / 5); // a hard light up-left
    ar_estimate(img, W, H, 70.0f, &c);
    printf("[RealEngine] AR: warm %.0f K, cool %.0f K; grey with a bright corner: light at (%.2f, %.2f, %.2f), strength %.2f, EV100 %.1f\n",
           a.kelvin, b.kelvin, c.dir_view[0], c.dir_view[1], c.dir_view[2], c.strength, c.ev100);
    if (!(a.kelvin < 4000.0f && b.kelvin > 6500.0f)) printf("[RealEngine] FAIL: AR: the white balance\n"), fails++;
    if (!(c.dir_view[0] < -0.2f && c.dir_view[1] > 0.1f && c.strength > 0.3f)) printf("[RealEngine] FAIL: AR: the main light\n"), fails++;
    if (!(fabsf(c.sh[0][0] - c.sh[0][2]) < 0.05f * c.sh[0][0] + 1e-4f)) printf("[RealEngine] FAIL: AR: the ambient of a grey picture is grey\n"), fails++;
    free(img);
    printf("[RealEngine] AR self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
