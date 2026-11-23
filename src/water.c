#include "water.h"
#include "lens_water.h"
#include "weather_field.h"
#include "river.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"
#include "vkctx.h"

static const uint32_t s_water[] =
#include "water_sim.comp.inc"
    ;
static const uint32_t s_water_rt[] =
#include "water_sim_rt.comp.inc"
    ;
static const uint32_t s_water_fft[] = // (the FFT passes, 11 and 12, apart: water_sim.comp RE_WATER_FFT)
#include "water_fft.comp.inc"
    ;

#define OCEAN_N 256
#define TRAIL_N 512     // the snow's trail around the camera (water.glsl W_TRAIL_N)
#define TRAIL_CELL 4.0f // cm
#define TRAIL_STRIDE 38.0f // cm: a walking step (a foot's print every step, left and right)
#define RIPPLE_N 256
#define RIPPLE_CELL 25.0f   // cm
#define MAX_RAIN_N 1024
#define WET_MAX 64
#define MAX_DROPS 160000u // (Drop Amount up to 4x of a storm)
#define MAX_SPLASHES 6000u
#define TOP_WAVES 64        // the ocean's waves the CPU sums (the boats)
static const float k_cascade[3] = { 400.0f, 60.0f, 8.0f }; // m

typedef struct TopWave { float kx, ky, w; float a[2], b[2]; } TopWave; // h0(k), conj(h0(-k))

static struct {
    VkPipeline pipe, fft_pipe; // (fft_pipe: passes 11 and 12, apart)
    VkPipelineLayout layout;
    bool rt;
    GpuBuf surface, water, flux, ripple, fft, ocean, rings, trail;
    // (B12c) the rivers' samples this frame (one buffer per frame in flight: river_slot), their count each
    GpuBuf rivers[RE_FRAMES_IN_FLIGHT];
    uint32_t nrivers, river_n[4], river_slot;
    // the ocean's spectrum h0: made on the CPU when the wind changes (h0_cpu), copied into the frame slot's own buffer
    // (the GPU may still read the other slot's: one written while in use tore the waves for a frame, notas-dev/agua.md)
    GpuBuf h0[RE_FRAMES_IN_FLIGHT];
    float* h0_cpu;
    uint32_t h0_dirty; // slots whose buffer is older than h0_cpu
    uint32_t h0_slot;  // the one this frame reads
    uint32_t n;            // the rain grid's cells across (allocated)
    // this frame
    bool rain, ocean_on, ripples, reset;
    uint32_t rain_n, rows_first, rows_count, drops, splashes, nlakes;
    float time;
    int32_t first[2];
    // (2026-11-14, "a chuva no chão some quando a câmera sobe") the ground under the camera, from the rain's own map: the
    // window's middle cell read back (one host buffer per frame in flight, read when its slot comes round again)
    GpuBuf ground_rb[RE_FRAMES_IN_FLIGHT];
    uint32_t ground_key[RE_FRAMES_IN_FLIGHT], ground_index[RE_FRAMES_IN_FLIGHT];
    bool ground_asked[RE_FRAMES_IN_FLIGHT], ground_ok;
    float ground_z;
    float open_wet, open_water; // the open ground past the window: its wetness and standing water (cm), as a flat cell's
    // (Q27.6) the persistent wet map: 16 m cells on a 4 km toroidal window around the camera (water_sim.comp pass 15), its
    // probe read back for the tests and Get Ground Water At, the 10 minutes a test skips
    GpuBuf persist, persist_rb[RE_FRAMES_IN_FLIGHT];
    bool persist_on, probe_on, probe_ok, probe_asked[RE_FRAMES_IN_FLIGHT];
    uint32_t probe_key[RE_FRAMES_IN_FLIGHT];
    float probe_xy[2], probe_val[2], persist_skip;
    int32_t persist_first[2];
    WeatherSample here; // the regional weather where the camera is
    float snow_cover, snow_key; // the snow's cover where nothing is in the way (beyond the simulated window, new places)
    // the ocean's spectrum (what made it, to make it again when it changes)
    float spec_key[6];
    TopWave top[TOP_WAVES];
    uint32_t ntop;
    float ocean_level;
    float rain_key[4], reset_key[4];
    // the snow's trail: on this frame; the feet of the Characters walking in it (their last place, the way since their
    // last step, the next foot)
    bool trail_on;
    struct { Entity e; float last[2], acc, heading, wet; uint32_t side; bool seen; } feet[16]; // (wet: its feet's water, 2026-11-15)
    float drip_acc[WET_MAX]; // (2026-11-15) the drips a wet actor owes the floor (wet marks)
    uint32_t mark_rng;
    // (2026-11-10) what moves under the rain (Characters, Skeletal Meshes, vehicles, bodies): its place last frame, so
    // the splashes thrown off it carry its speed (water_sim pass 0 adds a skin's own motion: an arm swinging)
    struct { Entity e; float last[3]; bool seen; } movers[32];
    // (2026-11-10) the ocean's exclusions this frame: center x, y, half sizes, the yaw's cos, sin (cm)
    float excl[8][6];
    uint32_t nexcl;
    // (2026-11-14) what moves keeps its own wetness: up to WET_MAX followed (nearest the camera); the rain map's cell over
    // each one (and over the camera: the sound's muffle) read back like the ground, one host buffer per frame in flight;
    // their wetness to the GPU as a byte per entity index (0: not followed, 1 + 254 x wet), a host table per frame in
    // flight, the indices written last time cleared
    struct { Entity e; float wet, top, out_t; int exposed; bool seen; } wet[WET_MAX]; // (out_t: s since the rain last fell on it, 2026-11-15)
    GpuBuf wet_rb[RE_FRAMES_IN_FLIGHT], wet_tab[RE_FRAMES_IN_FLIGHT];
    uint32_t wet_req_n[RE_FRAMES_IN_FLIGHT];          // cells asked in that slot (0 the camera, then the followed)
    Entity wet_req_e[RE_FRAMES_IN_FLIGHT][WET_MAX + 1];
    float wet_req_top[RE_FRAMES_IN_FLIGHT][WET_MAX + 1];
    uint32_t wet_req_cell[RE_FRAMES_IN_FLIGHT][WET_MAX + 1]; // (the cell's index in the map)
    bool wet_req_in[RE_FRAMES_IN_FLIGHT][WET_MAX + 1];       // (inside the window: asked; outside: open sky)
    uint32_t* wet_written[RE_FRAMES_IN_FLIGHT];      // entity indices set in that slot's table
    uint32_t wet_written_n[RE_FRAMES_IN_FLIGHT], wet_written_cap[RE_FRAMES_IN_FLIGHT];
    float shelter;        // the camera under a roof (0..1, eased)
    float gust_drift[2];  // the drops' sideways way so far under the gusting wind (cm): their place moves smoothly
    float flash, flash_hold; // the lightning's flash this frame, and its decay
    float mist; // (2026-11-15) the rain's haze: extra extinction (1/m) the height fog adds (weather.c)
    GpuBuf lens_buf[RE_FRAMES_IN_FLIGHT]; // (2026-11-15) the drops on the lens for composite.frag (lens_water.c)
    WaterStats stats;
} WS;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
#define RIVER_STRIDE (4u + 2u * RIVER_SAMPLES) // (B12c, water.glsl: a river's header and samples, in vec4s)
#define RIVER_ACROSS 16u

static bool buf(VkCtx* vk, GpuBuf* b, VkDeviceSize size, bool host) {
    if (b->buffer && b->size >= size) return true;
    if (b->buffer) vkDeviceWaitIdle(vk->device), gpu_buf_destroy(vk, b);
    return gpu_buf_create(vk, b, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
}

static VkCtx* g_vk;

bool water_init(VkCtx* vk, VkPipelineLayout layout) {
    memset(&WS, 0, sizeof WS);
    g_vk = vk;
    WS.layout = layout;
    WS.rt = vk->ray_query;
    WS.pipe = WS.rt ? gpu_compute_pipeline(vk, layout, s_water_rt, sizeof s_water_rt) : gpu_compute_pipeline(vk, layout, s_water, sizeof s_water);
    WS.fft_pipe = gpu_compute_pipeline(vk, layout, s_water_fft, sizeof s_water_fft);
    if (!WS.pipe || !WS.fft_pipe) return false;
    // the ripples and the ocean are small and fixed: made now; the rain's grid when a Rain asks for it
    if (!buf(vk, &WS.ripple, sizeof(float) * 4 * RIPPLE_N * RIPPLE_N, false) || !buf(vk, &WS.h0[0], sizeof(float) * 4 * 3 * OCEAN_N * OCEAN_N, true) ||
        !buf(vk, &WS.h0[1], sizeof(float) * 4 * 3 * OCEAN_N * OCEAN_N, true) || !(WS.h0_cpu = malloc(sizeof(float) * 4 * 3 * OCEAN_N * OCEAN_N)) ||
        !buf(vk, &WS.fft, sizeof(float) * 4 * 12 * OCEAN_N * OCEAN_N, false) || !buf(vk, &WS.ocean, sizeof(float) * 4 * 6 * OCEAN_N * OCEAN_N, false) ||
        !buf(vk, &WS.rings, sizeof(float) * 4 * 128 * 128, false)) // (W_RIP_N)
        return false;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    vkCmdFillBuffer(cmd, WS.ripple.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, WS.ocean.buffer, 0, VK_WHOLE_SIZE, 0);
    gpu_end_once(vk, cmd);
    return true;
}

void water_shutdown(VkCtx* vk) {
    if (WS.pipe) vkDestroyPipeline(vk->device, WS.pipe, NULL);
    if (WS.persist.buffer) gpu_buf_destroy(vk, &WS.persist);
    if (WS.fft_pipe) vkDestroyPipeline(vk->device, WS.fft_pipe, NULL);
    for (int k = 0; k < RE_FRAMES_IN_FLIGHT; k++) {
        gpu_buf_destroy(vk, &WS.ground_rb[k]);
        if (WS.persist_rb[k].buffer) gpu_buf_destroy(vk, &WS.persist_rb[k]);
        if (WS.wet_rb[k].buffer) gpu_buf_destroy(vk, &WS.wet_rb[k]);
        if (WS.wet_tab[k].buffer) gpu_buf_destroy(vk, &WS.wet_tab[k]);
        if (WS.lens_buf[k].buffer) gpu_buf_destroy(vk, &WS.lens_buf[k]);
        free(WS.wet_written[k]), WS.wet_written[k] = NULL;
    }
    GpuBuf* b[] = { &WS.surface, &WS.water, &WS.flux, &WS.ripple, &WS.h0[0], &WS.h0[1], &WS.fft, &WS.ocean, &WS.rings, &WS.trail, &WS.rivers[0], &WS.rivers[1] };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) // (all of them: the trail was left out)
        if (b[i]->buffer) gpu_buf_destroy(vk, b[i]);
    free(WS.h0_cpu);
    memset(&WS, 0, sizeof WS);
}

// --- The ocean's spectrum (JONSWAP, Hasselmann et al. 1973; the directional spread of Longuet-Higgins cos^2s) ---
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return x;
}
static float urand(uint32_t* s) { *s = hash32(*s + 0x9E3779B9u); return ((float)(*s >> 8) + 0.5f) / 16777216.0f; }
static void gauss2(uint32_t* s, float* a, float* b) {
    float u1 = urand(s), u2 = urand(s), r = sqrtf(-2.0f * logf(u1)), t = 6.2831853f * u2;
    *a = r * cosf(t), *b = r * sinf(t);
}

// S(k, theta) per unit k area (m^4): the wind's spectrum at a wave vector
static float spectrum(float kx, float ky, float U, float fetch_m, float wind_dir, float spread_s) {
    const float g = 9.81f;
    float k = sqrtf(kx * kx + ky * ky);
    if (k < 1e-5f) return 0.0f;
    float w = sqrtf(g * k);
    float wp = fmaxf(22.0f * cbrtf(g * g / (U * fetch_m)), 0.855f * g / U); // (no longer than a fully developed sea: Pierson-Moskowitz)
    float alpha = 0.076f * powf(U * U / (fetch_m * g), 0.22f);
    float sigma = w <= wp ? 0.07f : 0.09f;
    float r = expf(-(w - wp) * (w - wp) / (2.0f * sigma * sigma * wp * wp));
    float S = alpha * g * g / powf(w, 5.0f) * expf(-1.25f * powf(wp / w, 4.0f)) * powf(3.3f, r);
    float th = atan2f(ky, kx) - wind_dir;
    float c = fabsf(cosf(th * 0.5f));
    float norm = expf(lgammaf(spread_s + 1.0f) * 2.0f - lgammaf(2.0f * spread_s + 1.0f)) * powf(2.0f, 2.0f * spread_s - 1.0f) / 3.1415927f;
    float D = norm * powf(c, 2.0f * spread_s);
    float dwdk = g / (2.0f * w);
    float small = expf(-k * k * 0.0001f); // (capillary cutoff, 1 cm)
    return S * D * dwdk / k * small;
}

static void make_spectrum(float U, float wind_dir_deg, float swell) {
    float* h0 = WS.h0_cpu;
    if (!h0) return;
    WS.h0_dirty = (1u << RE_FRAMES_IN_FLIGHT) - 1u;
    U = fmaxf(U, 0.5f);
    float fetch = (50.0f + swell * 800.0f) * 1000.0f;
    float s = 4.0f + swell * 12.0f, dir = wind_dir_deg * 0.017453293f;
    WS.ntop = 0;
    for (int c = 0; c < 3; c++) {
        float L = k_cascade[c], dk = 6.2831853f / L;
        // each cascade its band of wavelengths: the first longer than half the second's side, the second longer than half
        // the third's, the third the rest (down to two of its texels)
        float kmin = c == 0 ? 0.0f : 6.2831853f / k_cascade[c] * 2.0f;
        float kmax = c == 2 ? 1e9f : 6.2831853f / k_cascade[c + 1] * 2.0f;
        for (int y = 0; y < OCEAN_N; y++)
            for (int x = 0; x < OCEAN_N; x++) {
                float kx = dk * (float)(x - OCEAN_N / 2), ky = dk * (float)(y - OCEAN_N / 2);
                float k = sqrtf(kx * kx + ky * ky);
                float amp = 0.0f, ampm = 0.0f;
                if (k >= kmin && k < kmax) {
                    amp = sqrtf(spectrum(kx, ky, U, fetch, dir, s) * dk * dk * 0.25f); // (E|h0|^2 = S dk^2 / 2: the height's variance the spectrum's integral)
                    ampm = sqrtf(spectrum(-kx, -ky, U, fetch, dir, s) * dk * dk * 0.25f);
                }
                uint32_t seed = (uint32_t)(c * 7919 + y * OCEAN_N + x) * 2654435761u + 17u;
                uint32_t seedm = (uint32_t)(c * 7919 + ((OCEAN_N - y) % OCEAN_N) * OCEAN_N + ((OCEAN_N - x) % OCEAN_N)) * 2654435761u + 17u;
                float g1, g2, m1, m2;
                gauss2(&seed, &g1, &g2), gauss2(&seedm, &m1, &m2);
                float* o = h0 + 4 * ((size_t)c * OCEAN_N * OCEAN_N + (size_t)y * OCEAN_N + x);
                o[0] = g1 * amp, o[1] = g2 * amp;      // h0(k)
                o[2] = m1 * ampm, o[3] = -m2 * ampm;   // conj(h0(-k))
                // the biggest waves of the first two cascades for the CPU (one of each +-k pair)
                if (c < 2 && amp > 0.0f && (ky > 0.0f || (ky == 0.0f && kx > 0.0f))) {
                    float a2 = (g1 * g1 + g2 * g2) * amp * amp;
                    uint32_t at = WS.ntop;
                    if (WS.ntop == TOP_WAVES) {
                        at = 0;
                        for (uint32_t i = 1; i < TOP_WAVES; i++)
                            if (WS.top[i].a[0] * WS.top[i].a[0] + WS.top[i].a[1] * WS.top[i].a[1] < WS.top[at].a[0] * WS.top[at].a[0] + WS.top[at].a[1] * WS.top[at].a[1]) at = i;
                        if (WS.top[at].a[0] * WS.top[at].a[0] + WS.top[at].a[1] * WS.top[at].a[1] >= a2) continue;
                    } else WS.ntop++;
                    TopWave* t = &WS.top[at];
                    t->kx = kx, t->ky = ky, t->w = sqrtf(9.81f * k);
                    t->a[0] = o[0], t->a[1] = o[1], t->b[0] = o[2], t->b[1] = o[3];
                }
            }
    }
}

// (2026-11-10) inside a Water Body Exclusion's rectangle: no ocean there
bool water_excluded(float x, float y) {
    for (uint32_t i = 0; i < WS.nexcl; i++) {
        const float* q = WS.excl[i];
        float dx = x - q[0], dy = y - q[1], u = dx * q[4] + dy * q[5], v = -dx * q[5] + dy * q[4];
        if (fabsf(u) <= q[2] && fabsf(v) <= q[3]) return true;
    }
    return false;
}

bool water_ocean_height(float x, float y, float* z) {
    if (!WS.ocean_on || water_excluded(x, y)) return false;
    float xm = x * 0.01f, ym = y * 0.01f, h = 0.0f, t = WS.time;
    for (uint32_t i = 0; i < WS.ntop; i++) {
        const TopWave* w = &WS.top[i];
        float c = cosf(w->w * t), s = sinf(w->w * t);
        // h(k, t) = h0 e^{iwt} + conj(h0(-k)) e^{-iwt}; its two halves (k, -k) sum to 2 Re(h e^{ik.x})
        float hr = w->a[0] * c - w->a[1] * s + w->b[0] * c + w->b[1] * s;
        float hi = w->a[0] * s + w->a[1] * c - w->b[0] * s + w->b[1] * c;
        float ph = w->kx * xm + w->ky * ym;
        h += 2.0f * (hr * cosf(ph) - hi * sinf(ph));
    }
    *z = WS.ocean_level + h * 100.0f;
    return true;
}

float water_rain_intensity(void) { return WS.rain ? WS.rain_key[0] : 0.0f; }
// (Q27.6) the persistent wet map
void water_persist_probe(float x, float y) { WS.probe_xy[0] = x, WS.probe_xy[1] = y, WS.probe_on = true, WS.probe_ok = false; }
bool water_persist_read(float out[2]) {
    if (!WS.probe_ok) return false;
    out[0] = WS.probe_val[0], out[1] = WS.probe_val[1];
    return true;
}
void water_persist_skip(float seconds) { WS.persist_skip += fmaxf(seconds, 0.0f); }
const WaterStats* water_stats(void) { return &WS.stats; }
void water_set_rain(World* w, float intensity) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_RAIN))) continue;
        CRain* r = ecs_arch_column(a, COMP_RAIN);
        for (uint32_t i = 0; i < a->count; i++) r[i].intensity = intensity < 0.0f ? 0.0f : intensity;
    }
}

// (2026-11-14) every Rain field by name: Set / Get Rain Property (Blueprints, scripts), the Details' ranges
#define RP(n, f, lo, hi) { n, (uint16_t)offsetof(CRain, f), 0, lo, hi }
#define RPU(n, f, lo, hi) { n, (uint16_t)offsetof(CRain, f), 1, lo, hi }
const RainProp g_rain_props[] = {
    RP("Rain Intensity", intensity, 0.0f, 300.0f), RP("Wind Speed", wind_speed, 0.0f, 40.0f), RP("Wind Direction", wind_dir, -360.0f, 360.0f),
    RP("Drop Size", drop_size, 0.3f, 8.0f), RP("Drops", drops, 0.0f, 1.0f), RP("Drop Amount", drop_amount, 0.0f, 4.0f),
    RP("Fall Speed", fall_speed, 0.25f, 3.0f), RP("Splashes", splashes, 0.0f, 1.0f), RP("Wetness", wetness, 0.0f, 1.0f),
    RP("Wet Time", wet_time, 0.1f, 3600.0f), RP("Dry Time", dry_time, 0.1f, 36000.0f), RP("Puddles", puddles, 0.0f, 1.0f),
    RP("Water Gain", water_gain, 0.0f, 1000.0f), RP("Flow", flow, 0.0f, 1.0f), RP("Infiltration", infiltration, 0.0f, 500.0f),
    RP("Evaporation", evaporation, 0.0f, 500.0f), RP("Micro Relief", micro_relief, 0.0f, 20.0f), RP("Streaks", streaks, 0.0f, 1.0f),
    RP("Ripples", ripples, 0.0f, 1.0f), RP("Area", area, 8.0f, 1000.0f), RPU("Simulation Resolution", resolution, 128.0f, 1024.0f),
    RP("Start Water", start_water, 0.0f, 2000.0f), RPU("Snow", snow, 0.0f, 1.0f), RP("Flake Size", flake_size, 1.0f, 40.0f),
    RP("Cover Time", cover_time, 1.0f, 36000.0f), RP("Cover Angle", cover_angle, 1.0f, 89.0f), RP("Melt Time", melt_time, 0.0f, 360000.0f),
    RP("Start Cover", start_cover, 0.0f, 1.0f), RP("Drop Color R", drop_color[0], 0.0f, 4.0f), RP("Drop Color G", drop_color[1], 0.0f, 4.0f),
    RP("Drop Color B", drop_color[2], 0.0f, 4.0f), RP("Streak Length", streak_length, 0.25f, 4.0f), RP("Drop Width", drop_width, 0.25f, 4.0f),
    RP("Splash Size", splash_size, 0.25f, 4.0f), RP("Gusts", gusts, 0.0f, 1.0f), RP("Far Rain", far_rain, 0.0f, 1.0f),
    RP("Actor Wet Time", actor_wet_time, 0.5f, 3600.0f), RP("Actor Dry Time", actor_dry_time, 0.5f, 36000.0f),
    RP("Sound Volume", sound_volume, 0.0f, 2.0f), RPU("Sound Style", sound_style, 0.0f, (float)(RAIN_SOUND_COUNT - 1)),
    RP("Muffle Under Roof", muffle, 0.0f, 1.0f), RP("Thunder", thunder, 0.0f, 1.0f), RP("Thunder Volume", thunder_volume, 0.0f, 2.0f),
    RP("Lightning", lightning, 0.0f, 1.0f),
    // (2026-11-15) the shading
    RPU("Shading Style", shading_style, 0.0f, (float)(RAIN_SHADE_COUNT - 1)), RP("Darkening", darkening, 0.0f, 2.0f),
    RP("Water Film", film, 0.0f, 2.0f), RP("Film Roughness", film_roughness, 0.005f, 0.5f),
    RP("Puddle Color R", puddle_color[0], 0.0f, 1.0f), RP("Puddle Color G", puddle_color[1], 0.0f, 1.0f), RP("Puddle Color B", puddle_color[2], 0.0f, 1.0f),
    RP("Puddle Clarity", puddle_clarity, 0.0f, 1.0f), RP("Puddle Edge", puddle_edge, 0.0f, 1.0f),
    RP("Surface Splashes", surface_splashes, 0.0f, 2.0f), RP("Wall Drops", wall_drops, 0.0f, 2.0f), RP("Wall Drop Size", wall_drop_size, 0.2f, 4.0f),
    RP("Drop Light", drop_light, 0.0f, 4.0f), RP("Drop Opacity", drop_opacity, 0.0f, 2.0f), RP("Rain Mist", mist, 0.0f, 3.0f),
    RP("Lens Water", lens_water, 0.0f, 2.0f), RP("Lens Drop Size", lens_drop_size, 0.3f, 3.0f),
    RPU("Regional Weather", regional, 0.0f, 1.0f), RP("Storminess", storminess, 0.0f, 1.0f), RPU("Weather Seed", weather_seed, 0.0f, 1000000.0f), // (Q27.6)
    RP("Lens Gravity", lens_gravity, 0.0f, 3.0f), RP("Lens Stickiness", lens_stickiness, 0.0f, 3.0f), RP("Lens Inertia", lens_inertia, 0.0f, 3.0f),
    RP("Lens Shake Off", lens_shake_off, 0.0f, 3.0f), RP("Lens Airflow", lens_airflow, 0.0f, 3.0f), RP("Lens Dry Time", lens_dry_time, 0.1f, 600.0f),
    RP("Lens Trail", lens_trail, 0.0f, 2.0f), RP("Lens Blur", lens_blur, 0.0f, 2.0f), RP("Lens Refraction", lens_refraction, 0.0f, 2.0f),
    RP("Wet Marks", wet_marks, 0.0f, 2.0f), RP("Wet Marks Dry Time", wet_marks_dry_time, 1.0f, 3600.0f), RP("Actor Drips", actor_drips, 0.0f, 2.0f),
};
#undef RP
#undef RPU
const uint32_t g_rain_prop_count = sizeof g_rain_props / sizeof g_rain_props[0];

static bool name_eq(const char* a, const char* b) { // (no spaces, no case: "WindSpeed" == "Wind Speed")
    for (;;) {
        while (*a == ' ' || *a == '_') a++;
        while (*b == ' ' || *b == '_') b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++, b++;
    }
}
const RainProp* rain_prop_find(const char* name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_rain_prop_count; i++)
        if (name_eq(g_rain_props[i].name, name)) return &g_rain_props[i];
    if (name_eq(name, "Intensity")) return &g_rain_props[0];
    if (name_eq(name, "Muffle")) return rain_prop_find("Muffle Under Roof");
    if (name_eq(name, "Mist")) return rain_prop_find("Rain Mist");
    if (name_eq(name, "Film")) return rain_prop_find("Water Film");
    return NULL;
}
bool water_set_rain_prop(World* w, const char* name, float value) {
    const RainProp* p = rain_prop_find(name);
    if (!p || !w) return false;
    value = value < p->lo ? p->lo : value > p->hi ? p->hi : value;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_RAIN))) continue;
        CRain* r = ecs_arch_column(a, COMP_RAIN);
        for (uint32_t i = 0; i < a->count; i++) {
            uint8_t* f = (uint8_t*)&r[i] + p->offset;
            if (p->is_uint) {
                uint32_t u = (uint32_t)(value + 0.5f);
                memcpy(f, &u, 4);
            } else
                memcpy(f, &value, 4);
            if (p->offset < offsetof(CRain, shading_style)) r[i].preset = RAIN_PRESET_CUSTOM; // (the shading has its own style)
        }
    }
    return true;
}
float water_get_rain_prop(World* w, const char* name) {
    const RainProp* p = rain_prop_find(name);
    if (!p || !w) return 0.0f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_RAIN))) continue;
        const uint8_t* f = (const uint8_t*)ecs_arch_column(a, COMP_RAIN) + p->offset;
        if (p->is_uint) {
            uint32_t u;
            memcpy(&u, f, 4);
            return (float)u;
        }
        float v;
        memcpy(&v, f, 4);
        return v;
    }
    return 0.0f;
}

const char* const g_rain_preset_names[RAIN_PRESET_COUNT] = { "Custom", "Drizzle", "Light Rain", "Moderate Rain", "Heavy Rain", "Storm", "Monsoon", "Stylized" };
const char* const g_rain_sound_names[RAIN_SOUND_COUNT] = { "Open Ground", "Roof", "Tin Roof", "Leaves", "Tent", "Distant" };
// The look and the sound together; the water's simulation (gain, infiltration, area) and the snow's fields stay
void rain_apply_preset(CRain* r, uint32_t preset) {
    if (!r || preset == RAIN_PRESET_CUSTOM || preset >= RAIN_PRESET_COUNT) return;
    // intensity, wind, drop size, drops, amount, fall speed, splashes, streak, width, splash size, gusts, thunder, lightning, sound
    static const float k[RAIN_PRESET_COUNT][14] = {
        { 0 },
        // (2026-11-16: the drop size the median of the real spectrum at that rate, D0 = 3.67 / L; the fall speed by it, x 1)
        { 1.5f, 1.0f, 1.0f, 0.55f, 1.6f, 1.0f, 0.3f, 1.0f, 0.8f, 0.6f, 0.2f, 0.0f, 0.0f, RAIN_SOUND_LEAVES },    // Drizzle
        { 6.0f, 2.0f, 1.3f, 0.8f, 1.0f, 1.0f, 0.7f, 1.0f, 1.0f, 0.8f, 0.25f, 0.0f, 0.0f, RAIN_SOUND_OPEN },      // Light
        { 20.0f, 3.0f, 1.7f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.3f, 0.0f, 0.0f, RAIN_SOUND_OPEN },      // Moderate
        { 50.0f, 6.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.1f, 1.2f, 0.45f, 0.15f, 0.5f, RAIN_SOUND_OPEN },    // Heavy
        { 100.0f, 14.0f, 2.35f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.2f, 1.4f, 0.8f, 0.7f, 0.8f, RAIN_SOUND_OPEN },   // Storm
        { 180.0f, 4.0f, 2.6f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.3f, 1.5f, 0.35f, 0.3f, 0.5f, RAIN_SOUND_LEAVES },  // Monsoon
        { 30.0f, 2.0f, 4.0f, 1.0f, 0.8f, 0.8f, 1.0f, 2.2f, 2.5f, 1.8f, 0.1f, 0.0f, 0.0f, RAIN_SOUND_OPEN },      // Stylized
    };
    const float* v = k[preset];
    r->intensity = v[0], r->wind_speed = v[1], r->drop_size = v[2], r->drops = v[3], r->drop_amount = v[4], r->fall_speed = v[5];
    r->splashes = v[6], r->streak_length = v[7], r->drop_width = v[8], r->splash_size = v[9], r->gusts = v[10];
    r->thunder = v[11], r->lightning = v[12], r->sound_style = (uint32_t)v[13];
    r->drop_color[0] = r->drop_color[1] = r->drop_color[2] = 1.0f;
    if (preset == RAIN_PRESET_STYLIZED) r->drop_color[0] = 0.75f, r->drop_color[1] = 0.9f, r->drop_color[2] = 1.4f; // (thick bright blue lines)
    r->snow = 0, r->preset = preset;
    // (2026-11-15) the Stylized rain shades stylized; the others leave the shading style as the dev chose it, unless it was
    // the Stylized one (going back to a real rain)
    if (preset == RAIN_PRESET_STYLIZED) rain_apply_shading_style(r, RAIN_SHADE_STYLIZED);
    else if (r->shading_style == RAIN_SHADE_STYLIZED) rain_apply_shading_style(r, RAIN_SHADE_REALISTIC);
}

// (2026-11-15, "permitir o shading da chuva ser alterado, para o dev escolher o estilo e poder modificar como quiser")
const char* const g_rain_shading_names[RAIN_SHADE_COUNT] = { "Realistic", "Cinematic Look", "Stylized" }; // (2026-11-15: "Cinematic" is the editor's style's msgid)
void rain_apply_shading_style(CRain* r, uint32_t style) { rain_shading_defaults(r, style); }
bool water_set_rain_shading_style(World* w, const char* name) {
    if (!w || !name) return false;
    uint32_t st = RAIN_SHADE_COUNT;
    for (uint32_t i = 0; i < RAIN_SHADE_COUNT; i++)
        if (name_eq(g_rain_shading_names[i], name)) st = i;
    if (st == RAIN_SHADE_COUNT && name_eq(name, "Cinematic")) st = RAIN_SHADE_CINEMATIC;
    if (st == RAIN_SHADE_COUNT && name[0] >= '0' && name[0] <= '9') st = (uint32_t)atoi(name);
    if (st >= RAIN_SHADE_COUNT) return false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_RAIN))) continue;
        CRain* r = ecs_arch_column(a, COMP_RAIN);
        for (uint32_t i = 0; i < a->count; i++) rain_apply_shading_style(&r[i], st);
    }
    return true;
}
float water_rain_mist(void) { return WS.mist; }

// (2026-11-15) the water on the lens: simulated on the CPU (lens_water.c), its drops into this frame slot's buffer
static void lens_frame(GpuFrame* fr, const CRain* rain, float dt) {
    fr->water[90][0] = 0.0f;
    float lw = clampf(rain->lens_water, 0.0f, 2.0f);
    if (lw <= 0.0f) {
        if (lens_drop_count()) lens_reset();
        return;
    }
    bool raining = rain->intensity > 0.05f; // (rain, or snow: its flakes stick to the lens and melt)
    float arrive = 0.0f;
    if (raining && WS.shelter < 0.5f) {
        float dz = -fmaxf(rain->fall_speed, 0.25f) * 650.0f, wx = fr->water[84][0], wy = fr->water[84][1];
        float dl = sqrtf(wx * wx + wy * wy + dz * dz);
        const float* fw = fr->camera_fwd;
        float into = -(fw[0] * wx + fw[1] * wy + fw[2] * dz) / fmaxf(dl, 1.0f); // (1: looking straight into the rain)
        arrive = 40.0f * clampf(rain->intensity / 40.0f, 0.03f, 3.0f) * clampf(0.12f + 0.88f * (into * 0.5f + 0.5f), 0.05f, 1.0f);
        if (rain->snow) arrive *= 0.5f;
    }
    LensParams p = { lw, rain->lens_drop_size, rain->lens_gravity, rain->lens_stickiness, rain->lens_inertia, rain->lens_shake_off,
                     rain->lens_airflow, rain->lens_dry_time, rain->lens_trail };
    LensCamera c;
    for (int k = 0; k < 3; k++) c.pos[k] = fr->camera_pos[k], c.fwd[k] = fr->camera_fwd[k], c.right[k] = fr->camera_right[k], c.up[k] = fr->camera_up[k];
    c.aspect = fr->viewport[2] > 0.0f ? fr->viewport[3] / fr->viewport[2] : 16.0f / 9.0f;
    lens_step(&p, &c, arrive, !raining || WS.shelter >= 0.5f, rain->snow != 0, dt);
    if (!lens_drop_count()) return;
    uint32_t gs = g_vk->frame % RE_FRAMES_IN_FLIGHT;
    if (!buf(g_vk, &WS.lens_buf[gs], (VkDeviceSize)LENS_VEC4 * 16, true) || !WS.lens_buf[gs].mapped) return;
    if (!lens_pack(WS.lens_buf[gs].mapped, (size_t)LENS_VEC4 * 4)) return;
    fr->water[90][0] = 1.0f, fr->water[90][1] = clampf(rain->lens_blur, 0.0f, 2.0f), fr->water[90][2] = clampf(rain->lens_refraction, 0.0f, 2.0f);
    fr->water[90][3] = c.aspect;
    uint32_t lo = (uint32_t)WS.lens_buf[gs].addr, hi = (uint32_t)(WS.lens_buf[gs].addr >> 32);
    memcpy(&fr->water[91][0], &lo, 4), memcpy(&fr->water[91][1], &hi, 4);
}
bool water_set_rain_preset(World* w, const char* name) {
    if (!w || !name) return false;
    uint32_t p = RAIN_PRESET_COUNT;
    for (uint32_t i = 1; i < RAIN_PRESET_COUNT; i++)
        if (name_eq(g_rain_preset_names[i], name)) p = i;
        else { // ("Light" for "Light Rain")
            char b[48];
            snprintf(b, sizeof b, "%s Rain", name);
            if (name_eq(g_rain_preset_names[i], b)) p = i;
        }
    if (p == RAIN_PRESET_COUNT && name[0] >= '1' && name[0] <= '9') p = (uint32_t)atoi(name);
    if (p == 0 || p >= RAIN_PRESET_COUNT) return false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_RAIN))) continue;
        CRain* r = ecs_arch_column(a, COMP_RAIN);
        for (uint32_t i = 0; i < a->count; i++) rain_apply_preset(&r[i], p);
    }
    return true;
}

float water_actor_wetness(Entity e) {
    for (int k = 0; k < WET_MAX; k++)
        if (WS.wet[k].e == e && e) return WS.wet[k].wet;
    return -1.0f;
}
void water_set_actor_wetness(Entity e, float wet) {
    wet = wet < 0.0f ? 0.0f : wet > 1.0f ? 1.0f : wet;
    int fk = -1;
    for (int k = 0; k < WET_MAX; k++) {
        if (WS.wet[k].e == e && e) {
            WS.wet[k].wet = wet;
            return;
        }
        if (!WS.wet[k].e && fk < 0) fk = k;
    }
    if (fk >= 0 && e) WS.wet[fk].e = e, WS.wet[fk].wet = wet, WS.wet[fk].exposed = -1, WS.wet[fk].top = 0.0f;
}
float water_rain_shelter(void) { return WS.shelter; }
float water_lightning(void) { return WS.flash; }
void water_strike_lightning(float strength) {
    strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
    if (strength > WS.flash_hold) WS.flash_hold = strength;
}

// The followed actors' wetness (water_fill_frame, raining): the exposures read back for this slot, then each one soaks or
// dries, then this slot's byte table (the actor and every part / component it owns: a Character's skeletal mesh's parts)
static float wet_top_of(World* w, Entity e, const CTransform* t) {
    const CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
    if (ch) return t->location[2] + fmaxf(ch->half_height, 40.0f);
    if (ecs_get(w, e, COMP_VEHICLE)) return t->location[2] + 150.0f;
    if (ecs_get(w, e, COMP_SKELETAL_MESH)) return t->location[2] + 160.0f * fabsf(t->scale[2]);
    return t->location[2] + 50.0f * fabsf(t->scale[2]);
}
static void wet_update(GpuFrame* fr, World* w, const CRain* rain, float dt, float sx, float sy, float cell, uint32_t n) {
    uint32_t gs = g_vk->frame % RE_FRAMES_IN_FLIGHT;
    // 1. what this slot asked last time (its frame is done): open (1) or under something (0)
    if (WS.wet_rb[gs].mapped) {
        const float* g = WS.wet_rb[gs].mapped;
        for (uint32_t i = 0; i < WS.wet_req_n[gs]; i++) {
            int open = 1;
            if (WS.wet_req_in[gs][i]) open = WS.wet_req_top[gs][i] >= g[4 * i] - 20.0f || g[4 * i] < -1e8f;
            if (i == 0) {
                WS.shelter += ((open ? 0.0f : 1.0f) - WS.shelter) * fminf(dt * 4.0f, 1.0f);
                continue;
            }
            for (int k = 0; k < WET_MAX; k++)
                if (WS.wet[k].e == WS.wet_req_e[gs][i] && WS.wet[k].e) WS.wet[k].exposed = open;
        }
    }
    WS.wet_req_n[gs] = 0;
    // 2. the followed: the nearest movers to the camera (within the rain's area)
    for (int k = 0; k < WET_MAX; k++) WS.wet[k].seen = false;
    const uint64_t any = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_VEHICLE) | COMP_BIT(COMP_RIGID_BODY);
    float reach = fmaxf(rain->area, 8.0f) * 50.0f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & any) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t r = 0; r < a->count; r++) {
            float dx = tr[r].location[0] - fr->camera_pos[0], dy = tr[r].location[1] - fr->camera_pos[1];
            if (dx * dx + dy * dy > reach * reach) continue;
            int k = -1, fk = -1;
            for (int j = 0; j < WET_MAX; j++) {
                if (WS.wet[j].e == ents[r] && WS.wet[j].e) k = j;
                if (!WS.wet[j].e && fk < 0) fk = j;
            }
            if (k < 0) {
                if (fk < 0) continue;
                k = fk, WS.wet[k].e = ents[r], WS.wet[k].wet = 0.0f, WS.wet[k].exposed = -1, WS.wet[k].out_t = 0.0f;
            }
            WS.wet[k].seen = true;
            WS.wet[k].top = wet_top_of(w, ents[r], &tr[r]);
        }
    }
    bool raining = rain->intensity > 0.05f && !rain->snow;
    float soak = raining ? dt / fmaxf(rain->actor_wet_time, 0.5f) * fminf(fmaxf(rain->intensity / 20.0f, 0.25f), 2.0f) : 0.0f;
    float dry = dt / fmaxf(rain->actor_dry_time, 0.5f);
    // 3. soak or dry; ask for the cell over each (and over the camera: entry 0)
    for (int k = -1; k < WET_MAX; k++) {
        float p[3];
        Entity e = 0;
        if (k < 0) memcpy(p, fr->camera_pos, 12);
        else {
            if (!WS.wet[k].e) continue;
            if (!WS.wet[k].seen) { // (gone or far: forgotten once dry)
                if (!ecs_alive(w, WS.wet[k].e)) WS.wet[k].e = 0;
                continue;
            }
            const CTransform* t = ecs_get(w, WS.wet[k].e, COMP_TRANSFORM);
            if (!t) continue;
            e = WS.wet[k].e;
            p[0] = t->location[0], p[1] = t->location[1], p[2] = WS.wet[k].top;
            int ex = WS.wet[k].exposed;
            if (raining && ex != 0) WS.wet[k].wet = fminf(WS.wet[k].wet + soak * (ex < 0 ? 0.0f : 1.0f), 1.0f);
            else WS.wet[k].wet = fmaxf(WS.wet[k].wet - dry, 0.0f);
            WS.wet[k].out_t = raining && ex == 1 ? 0.0f : fminf(WS.wet[k].out_t + dt, 1e6f);
        }
        uint32_t i = WS.wet_req_n[gs]++;
        float ux = p[0] + sx * p[2], uy = p[1] + sy * p[2];
        int32_t cx = (int32_t)floorf(ux / cell), cy = (int32_t)floorf(uy / cell);
        bool in = cx >= WS.first[0] && cy >= WS.first[1] && cx < WS.first[0] + (int32_t)n && cy < WS.first[1] + (int32_t)n;
        int32_t in_ = (int32_t)n, mx = ((cx % in_) + in_) % in_, my = ((cy % in_) + in_) % in_;
        WS.wet_req_e[gs][i] = e, WS.wet_req_top[gs][i] = p[2], WS.wet_req_in[gs][i] = in;
        WS.wet_req_cell[gs][i] = (uint32_t)my * n + (uint32_t)mx;
    }
    // 4. this slot's table: clear what it held, then the followed and what they own
    if (!buf(g_vk, &WS.wet_tab[gs], RE_ECS_MAX_ENTITIES, true) || !WS.wet_tab[gs].mapped) return;
    uint8_t* tab = WS.wet_tab[gs].mapped;
    for (uint32_t i = 0; i < WS.wet_written_n[gs]; i++) tab[WS.wet_written[gs][i]] = 0;
    WS.wet_written_n[gs] = 0;
#define WET_PUT(idx, v)                                                                                                   \
    do {                                                                                                                  \
        if (WS.wet_written_n[gs] == WS.wet_written_cap[gs]) {                                                           \
            uint32_t nc = WS.wet_written_cap[gs] ? WS.wet_written_cap[gs] * 2u : 1024u;                                   \
            uint32_t* nw = realloc(WS.wet_written[gs], sizeof(uint32_t) * nc);                                           \
            if (!nw) break;                                                                                               \
            WS.wet_written[gs] = nw, WS.wet_written_cap[gs] = nc;                                                        \
        }                                                                                                                 \
        WS.wet_written[gs][WS.wet_written_n[gs]++] = (idx);                                                              \
        tab[(idx)] = (v);                                                                                                 \
    } while (0)
    uint32_t nf = 0;
    for (int k = 0; k < WET_MAX; k++)
        if (WS.wet[k].e && WS.wet[k].seen) {
            WET_PUT(entity_index(WS.wet[k].e), (uint8_t)(1.0f + 254.0f * WS.wet[k].wet + 0.5f));
            nf++;
        }
    if (nf) // what they own (a skeletal mesh's parts and skins, a Blueprint's components: up to 3 owners up)
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!a->count || !(a->mask & COMP_BIT(COMP_ATTACH)) || !(a->mask & COMP_BIT(COMP_STATIC_MESH))) continue;
            const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t r = 0; r < a->count; r++) {
                Entity o = at[r].owner;
                uint8_t v = 0;
                for (int up = 0; up < 3 && o && !v; up++) {
                    v = tab[entity_index(o)];
                    if (!v) {
                        const CAttach* oa = ecs_get(w, o, COMP_ATTACH);
                        o = oa ? oa->owner : 0;
                    }
                }
                if (v && !tab[entity_index(ents[r])]) WET_PUT(entity_index(ents[r]), v);
            }
        }
#undef WET_PUT
    uint32_t lo = (uint32_t)WS.wet_tab[gs].addr, hi = (uint32_t)(WS.wet_tab[gs].addr >> 32);
    memcpy(&fr->water[80][0], &lo, 4), memcpy(&fr->water[80][1], &hi, 4);
    fr->water[80][2] = 1.0f;
}

// --- The frame ---
void water_fill_frame(GpuFrame* fr, World* w, float dt, bool editor) {
    double t0 = re_time_seconds();
    (void)editor;
    memset(fr->water, 0, sizeof fr->water);
    memset(fr->water_addr, 0, sizeof fr->water_addr);
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.0f) dt = 0.0f;
    WS.time += dt;
    WS.rain = WS.ocean_on = WS.trail_on = false;
    WS.mist = 0.0f;
    WS.nlakes = 0, WS.nexcl = 0;
    WS.drops = WS.splashes = 0;
    const CRain* rain = NULL;
    int ocean_body = -1;
    float lakes[4][8], lake_look[4][8];
    // (B12h) the water the camera is in: its surface's height (-1: none), its color and clarity
    float under = -1.0f, under_col[3] = { 0.01f, 0.05f, 0.08f }, under_clarity = 800.0f;
    WS.nrivers = 0, WS.river_slot = (WS.river_slot + 1u) % RE_FRAMES_IN_FLIGHT;
    const CWater* ocean = NULL;
    float ocean_z = 0.0f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        if ((a->mask & COMP_BIT(COMP_RAIN)) && !rain) rain = ecs_arch_column(a, COMP_RAIN);
        if (a->mask & COMP_BIT(COMP_WATER)) {
            const CWater* cw = ecs_arch_column(a, COMP_WATER);
            for (uint32_t r = 0; r < a->count; r++) {
                if (cw[r].exclusion) { // (an exclusion: a hole in the ocean, not water)
                    if (WS.nexcl < 8) {
                        float yaw = tr[r].rotation[1] * 0.017453293f, *q = WS.excl[WS.nexcl++];
                        q[0] = tr[r].location[0], q[1] = tr[r].location[1], q[2] = 50.0f * fabsf(tr[r].scale[0]), q[3] = 50.0f * fabsf(tr[r].scale[1]);
                        q[4] = cosf(yaw), q[5] = sinf(yaw);
                    }
                    continue;
                }
                if (cw[r].ocean) {
                    if (!ocean) ocean = &cw[r], ocean_z = tr[r].location[2];
                    continue;
                }
                if (a->mask & COMP_BIT(COMP_RIVER)) { // (B12c) a river: its samples into this frame's buffer
                    const CRiver* rv = &((const CRiver*)ecs_arch_column(a, COMP_RIVER))[r];
                    if (WS.nrivers >= 4) continue;
                    if (!WS.rivers[WS.river_slot].buffer &&
                        !gpu_buf_create(g_vk, &WS.rivers[WS.river_slot], 16u * RIVER_STRIDE * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
                        continue;
                    static RiverSample rs[RIVER_SAMPLES];
                    uint32_t ns = river_samples(rv, &tr[r], rs, RIVER_SAMPLES);
                    if (ns < 2) continue;
                    float* o = (float*)WS.rivers[WS.river_slot].mapped + 4u * RIVER_STRIDE * WS.nrivers;
                    memset(o, 0, 64);
                    o[0] = (float)ns;
                    memcpy(o + 4, cw[r].color, 12), o[7] = fmaxf(cw[r].clarity, 1.0f);
                    memcpy(o + 8, cw[r].scatter, 12), o[11] = fminf(fmaxf(cw[r].shore_foam, 0.0f), 1.0f);
                    for (uint32_t k = 0; k < ns; k++) {
                        float* q = o + 16 + 8 * k;
                        memcpy(q, rs[k].p, 12), q[3] = rs[k].width;
                        q[4] = rs[k].dir[0], q[5] = rs[k].dir[1], q[6] = rs[k].speed, q[7] = rs[k].depth;
                    }
                    // the camera under it (B12h): over its surface's height where the camera stands on it
                    float rz, cur[2], dep;
                    if (river_at(rs, ns, fr->camera_pos[0], fr->camera_pos[1], &rz, cur, &dep) && fr->camera_pos[2] < rz + 5.0f && under < 0.0f)
                        under = rz, memcpy(under_col, cw[r].color, 12), under_clarity = fmaxf(cw[r].clarity, 1.0f);
                    WS.river_n[WS.nrivers++] = ns;
                    continue;
                }
                if (WS.nlakes >= 4) continue;
                float* L = lakes[WS.nlakes++];
                L[0] = tr[r].location[0], L[1] = tr[r].location[1], L[2] = tr[r].location[2];
                L[3] = 50.0f * fabsf(tr[r].scale[0]), L[4] = 50.0f * fabsf(tr[r].scale[1]);
                L[5] = cw[r].wave_height;
                L[6] = cw[r].wave_length > 1.0f ? 6.2831853f / cw[r].wave_length : 0.0f;
                L[7] = L[6] * cw[r].wave_speed;
                float* K = lake_look[WS.nlakes - 1];
                memcpy(K, cw[r].color, 12), K[3] = fmaxf(cw[r].clarity, 1.0f);
                if (fabsf(fr->camera_pos[0] - L[0]) < L[3] && fabsf(fr->camera_pos[1] - L[1]) < L[4] && fr->camera_pos[2] < L[2] + L[5] + 5.0f &&
                    fr->camera_pos[2] > L[2] - cw[r].depth && under < 0.0f) // (B12h: the camera in the lake)
                    under = L[2], memcpy(under_col, cw[r].color, 12), under_clarity = K[3];
                memcpy(K + 4, cw[r].scatter, 12), K[7] = fminf(fmaxf(cw[r].shore_foam, 0.0f), 1.0f);
            }
        }
    }
    fr->water[0][1] = WS.time;
    // the lakes
    for (uint32_t i = 0; i < WS.nlakes; i++) {
        memcpy(fr->water[29 + 2 * i], lakes[i], 16);
        memcpy(fr->water[40 + i], lake_look[i], 16), memcpy(fr->water[44 + i], lake_look[i] + 4, 16);
        memcpy(fr->water[30 + 2 * i], lakes[i] + 4, 16);
    }
    fr->water[37][0] = (float)WS.nlakes;
    fr->water[63][0] = (float)WS.nexcl; // (2026-11-10) the ocean's exclusions: water[64 + 2i] center, half sizes; [65 + 2i] the yaw's cos, sin
    for (uint32_t i = 0; i < WS.nexcl; i++) memcpy(fr->water[64 + 2 * i], WS.excl[i], 16), fr->water[65 + 2 * i][0] = WS.excl[i][4], fr->water[65 + 2 * i][1] = WS.excl[i][5];
    // (B12h) under water: the ocean's surface where the camera is, else a lake's or a river's
    float oz;
    if (ocean && water_ocean_height(fr->camera_pos[0], fr->camera_pos[1], &oz) && fr->camera_pos[2] < oz + 5.0f)
        under = oz, memcpy(under_col, ocean->color, 12), under_clarity = fmaxf(ocean->clarity, 1.0f);
    memset(fr->water[50], 0, 32);
    if (under > -0.5f && !getenv("REALENGINE_NO_UNDERWATER")) {
        fr->water[50][0] = under, fr->water[50][1] = 1.0f, fr->water[50][2] = under_clarity;
        memcpy(fr->water[51], under_col, 12);
    }
    // the rain
    // (the snow's cover stays and melts after the snow stops: the map keeps running while a Rain actor exists)
    // (2026-11-14) the map keeps running after the rain stops while the open ground or something that moved in it is still
    // wet: the surfaces dry over their Dry Time (it went dry at once with the intensity set to 0)
    static CRain s_eff; // (Q27.6) the regional weather: the Rain actor's look with the storms' rain and wind where the camera is
    if (rain && rain->regional) {
        wfield_configure(true, rain->storminess, rain->weather_seed, rain->wind_speed, rain->wind_dir);
        wfield_update(fminf(dt, 0.25f), fr->camera_pos[0], fr->camera_pos[1]);
        wfield_sample(fr->camera_pos[0], fr->camera_pos[1], &WS.here);
        s_eff = *rain;
        s_eff.intensity = WS.here.rain;
        s_eff.wind_speed = sqrtf(WS.here.wind[0] * WS.here.wind[0] + WS.here.wind[1] * WS.here.wind[1]);
        s_eff.wind_dir = atan2f(WS.here.wind[1], WS.here.wind[0]) * 57.29578f;
        rain = &s_eff;
    } else if (wfield_on()) wfield_configure(false, 0.0f, 0u, 0.0f, 0.0f);
    fr->water[53][3] = 0.0f, fr->water[54][0] = fr->water[54][1] = 0.0f;
    bool still_wet = WS.open_wet > 0.002f;
    for (int k = 0; k < WET_MAX && !still_wet; k++) still_wet = WS.wet[k].e && WS.wet[k].wet > 0.002f;
    // (the lightning's flash: a quick decay, flickering twice)
    WS.flash = WS.flash_hold * (0.75f + 0.25f * sinf(WS.time * 47.0f));
    WS.flash_hold = fmaxf(WS.flash_hold - dt * 3.5f, 0.0f);
    if (rain && (rain->intensity > 0.0f || still_wet || wfield_on() || WS.persist_on || (rain->snow && (WS.snow_cover > 0.0f || rain->start_cover > 0.0f))) && !getenv("REALENGINE_NO_RAIN")) { // (the env: measuring without it)
        uint32_t n = rain->resolution < 128 ? 128 : rain->resolution > MAX_RAIN_N ? MAX_RAIN_N : rain->resolution;
        n &= ~7u;
        VkDeviceSize bytes = sizeof(float) * 4 * (VkDeviceSize)n * n;
        bool fresh = WS.n != n || !WS.surface.buffer;
        if (fresh) { // (the surface: two vec4 a cell: the static first surface, then what moves over it: its speed and top)
            if (buf(g_vk, &WS.surface, bytes * 2, false) && buf(g_vk, &WS.water, bytes, false) && buf(g_vk, &WS.flux, bytes, false)) {
                VkCommandBuffer cmd = gpu_begin_once(g_vk);
                vkCmdFillBuffer(cmd, WS.surface.buffer, 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(cmd, WS.water.buffer, 0, VK_WHOLE_SIZE, 0);
                vkCmdFillBuffer(cmd, WS.flux.buffer, 0, VK_WHOLE_SIZE, 0);
                gpu_end_once(g_vk, cmd);
                WS.n = n;
            }
        }
        if (WS.n == n) {
            WS.rain = true;
            float cell = fmaxf(rain->area, 8.0f) * 100.0f / (float)n;
            // (2026-11-10) the window around the camera in the slanted grid (water.glsl rain_slant / rain_u: the same)
            float wr0 = rain->wind_dir * 0.017453293f, fall = (rain->snow ? 80.0f : 600.0f) * clampf(rain->fall_speed, 0.25f, 3.0f); // (as water.glsl rain_slant)
            float sx = cosf(wr0) * rain->wind_speed * 100.0f / fall, sy = sinf(wr0) * rain->wind_speed * 100.0f / fall, sl = sqrtf(sx * sx + sy * sy);
            if (sl > 0.7f) sx *= 0.7f / sl, sy *= 0.7f / sl; // (as water.glsl rain_slant: 0.7 at most, 2026-11-15)
            // (the window's middle: the ground under the camera, not the camera itself. The grid is slanted (u = xy +
            // slant x z): centered at the camera's own height, a camera 30 m up in a strong wind had its window 45 m
            // off, half of it, and the ground it looked at went dry. The ground's height: the rain map's middle cell,
            // read back from the frame that last used this slot; until then the camera's feet)
            {
                uint32_t gs = g_vk->frame % RE_FRAMES_IN_FLIGHT; // (this slot's last frame is done: vkctx waited for it)
                if (WS.ground_asked[gs] && WS.ground_rb[gs].mapped) {
                    const float* g = WS.ground_rb[gs].mapped;
                    uint32_t k;
                    memcpy(&k, &g[1], 4);
                    if (k == WS.ground_key[gs] && g[2] > 0.5f && g[0] > -1e8f) {
                        float z = fminf(g[0], fr->camera_pos[2]);
                        WS.ground_z = WS.ground_ok ? WS.ground_z + (z - WS.ground_z) * 0.5f : z; // (damped: a slope can move the middle cell)
                        WS.ground_ok = true;
                    }
                    WS.ground_asked[gs] = false;
                }
            }
            float zr = WS.ground_ok ? fminf(WS.ground_z, fr->camera_pos[2]) : fr->camera_pos[2] - 170.0f;
            float ux = fr->camera_pos[0] + sx * zr, uy = fr->camera_pos[1] + sy * zr;
            WS.first[0] = (int32_t)floorf(ux / cell) - (int32_t)n / 2;
            WS.first[1] = (int32_t)floorf(uy / cell) - (int32_t)n / 2;
            wet_update(fr, w, rain, dt, sx, sy, cell, n);
            // (2026-11-14, "a chuva ainda some se a câmera subir muito: a chuva não pode sumir no chão independente da
            // altura") the drops live in a box around the camera (50 x 20 m) and the splashes within 18 m of it: from 30
            // m up neither reached the ground. Over 8 m above the ground under it, half the drops fall in a second box
            // standing on the ground where the camera looks (its side grows with the height, up to 300 m) and the
            // splashes go there too, drawn at least a couple of pixels across
            {
                float gz = WS.ground_ok ? WS.ground_z : fr->camera_pos[2] - 170.0f, h = fr->camera_pos[2] - gz;
                float amt = clampf(rain->far_rain, 0.0f, 1.0f) * clampf((h - 800.0f) / 1200.0f, 0.0f, 1.0f);
                if (amt > 0.0f && !rain->snow) {
                    const float* fw = fr->camera_fwd;
                    float hl = sqrtf(fw[0] * fw[0] + fw[1] * fw[1]), d;
                    if (fw[2] < -0.05f) d = h / -fw[2] * hl; // (where the view meets the ground, along it)
                    else d = h;
                    d = fminf(d, fmaxf(h * 3.0f, 6000.0f));
                    float fx = fr->camera_pos[0], fy = fr->camera_pos[1];
                    if (hl > 1e-3f) fx += fw[0] / hl * d, fy += fw[1] / hl * d;
                    fr->water[81][0] = fx, fr->water[81][1] = fy, fr->water[81][2] = gz;
                    fr->water[81][3] = clampf(h * 1.5f, 5000.0f, 30000.0f);
                    fr->water[83][2] = amt;
                }
            }
            // the look: the drops' tint and streak, their width, the crowns' size; the gusts (the drops' wind now and the
            // way it carried them so far: water[84]); the lightning's flash (water[85].x)
            fr->water[82][0] = clampf(rain->drop_color[0], 0.0f, 4.0f), fr->water[82][1] = clampf(rain->drop_color[1], 0.0f, 4.0f);
            fr->water[82][2] = clampf(rain->drop_color[2], 0.0f, 4.0f), fr->water[82][3] = clampf(rain->streak_length, 0.25f, 4.0f);
            fr->water[83][0] = clampf(rain->drop_width, 0.25f, 4.0f), fr->water[83][1] = clampf(rain->splash_size, 0.25f, 4.0f);
            {
                float g = clampf(rain->gusts, 0.0f, 1.0f), t = WS.time;
                float sp = rain->wind_speed * (1.0f + g * (0.55f * sinf(t * 0.41f) + 0.35f * sinf(t * 1.37f + 1.7f) + 0.1f * sinf(t * 3.1f + 0.4f)));
                float dr = (rain->wind_dir + g * 25.0f * sinf(t * 0.23f + 0.9f)) * 0.017453293f;
                float wx = cosf(dr) * fmaxf(sp, 0.0f) * 100.0f, wy = sinf(dr) * fmaxf(sp, 0.0f) * 100.0f;
                WS.gust_drift[0] = fmodf(WS.gust_drift[0] + wx * dt, 1e6f), WS.gust_drift[1] = fmodf(WS.gust_drift[1] + wy * dt, 1e6f);
                fr->water[84][0] = wx, fr->water[84][1] = wy, fr->water[84][2] = WS.gust_drift[0], fr->water[84][3] = WS.gust_drift[1];
            }
            fr->water[85][0] = WS.flash * clampf(rain->lightning, 0.0f, 1.0f);
            // (2026-11-15) the shading (water.glsl rain_shade, rain.vert): water[86] darkening, film, its roughness, the
            // style; [87] the puddles' tint, clarity; [88] their edge, the surface splashes, the wall drops, their size
            // (cm); [89] the drops' light, opacity, the fall direction's speed (cm/s)
            fr->water[86][0] = clampf(rain->darkening, 0.0f, 2.0f), fr->water[86][1] = clampf(rain->film, 0.0f, 2.0f);
            fr->water[86][2] = clampf(rain->film_roughness, 0.005f, 0.5f), fr->water[86][3] = (float)(rain->shading_style < RAIN_SHADE_COUNT ? rain->shading_style : 0);
            for (int k = 0; k < 3; k++) fr->water[87][k] = clampf(rain->puddle_color[k], 0.0f, 1.0f);
            fr->water[87][3] = clampf(rain->puddle_clarity, 0.0f, 1.0f);
            fr->water[88][0] = clampf(rain->puddle_edge, 0.0f, 1.0f), fr->water[88][1] = clampf(rain->surface_splashes, 0.0f, 2.0f);
            fr->water[88][2] = clampf(rain->wall_drops, 0.0f, 2.0f), fr->water[88][3] = clampf(rain->wall_drop_size, 0.2f, 4.0f);
            fr->water[89][0] = clampf(rain->drop_light, 0.0f, 4.0f), fr->water[89][1] = clampf(rain->drop_opacity, 0.0f, 2.0f);
            fr->water[89][2] = fmaxf(9.65f - 10.3f * expf(-0.6f * rain->drop_size), 0.8f) * 100.0f * clampf(rain->fall_speed, 0.25f, 3.0f); // (the median drop's terminal speed: Atlas et al. 1973)
            // the haze: rain's extinction from its rate (about 0.29 R^0.67 per km, R in mm/h: ~2 km seen at 20 mm/h,
            // ~600 m at 100 mm/h) times the Rain Mist
            WS.mist = !rain->snow && rain->intensity > 0.0f ? 0.00029f * powf(rain->intensity, 0.67f) * clampf(rain->mist, 0.0f, 3.0f) : 0.0f;
            // (2026-11-15, "uma câmera real gravando na chuva com água escorrendo pelo visor ... dependendo do ângulo ... inércia
            // ... mexer a câmera rápido para tirar a chuva") the water on the lens (lens_water.c): drops arrive out under the
            // sky by the rain's rate, more looking into the rain (into the wind, up) than down at the ground; under a roof or
            // with the rain stopped they dry. Packed into this slot's buffer for composite.frag: water[90] on, blur,
            // refraction, aspect; water[91] its address
            lens_frame(fr, rain, dt);
            float key[4] = { rain->start_water, rain->area, (float)n, rain->micro_relief };
            WS.reset = fresh || memcmp(key, WS.reset_key, sizeof key) != 0; // (the place's water made again)
            memcpy(WS.reset_key, key, sizeof key);
            WS.rows_count = n / 8u, WS.rows_first = (WS.rows_first + WS.rows_count) % n;
            float wr = rain->wind_dir * 0.017453293f;
            fr->water[0][0] = rain->intensity;
            fr->water[0][2] = cosf(wr) * rain->wind_speed, fr->water[0][3] = sinf(wr) * rain->wind_speed;
            fr->water[1][0] = cell, fr->water[1][1] = (float)n, fr->water[1][2] = (float)WS.first[0], fr->water[1][3] = (float)WS.first[1];
            fr->water[2][0] = rain->wetness, fr->water[2][1] = rain->puddles, fr->water[2][2] = rain->streaks, fr->water[2][3] = rain->ripples;
            // (2026-11-16) as many drops as the Marshall-Palmer spectrum puts in the air (the 0.5 mm and up: e^(-0.5 L) / L, L =
            // 4.1 R^-0.21): it grows as the square root of the rate, not the rate (a drizzle has many fine drops, a storm
            // fewer and bigger ones); 48 000 at 50 mm/h. Snow: by the rate, as before
            float rm = 0.0f;
            if (rain->intensity > 0.05f) {
                float L = 4.1f * powf(rain->intensity, -0.21f), L50 = 4.1f * powf(50.0f, -0.21f);
                rm = rain->snow ? rain->intensity / 50.0f * 0.5f : (expf(-0.5f * L) / L) / (expf(-0.5f * L50) / L50);
            }
            WS.drops = (uint32_t)clampf(48000.0f * rm * (rain->drops > 0.0f ? 1.0f : 0.0f) * clampf(rain->drop_amount, 0.0f, 4.0f), 0.0f, (float)MAX_DROPS);
            WS.splashes = (uint32_t)clampf(rain->intensity / 50.0f * 2500.0f * rain->splashes, 0.0f, (float)MAX_SPLASHES);
            fr->water[3][0] = rain->drops, fr->water[3][1] = rain->drop_size, fr->water[3][2] = rain->splashes, fr->water[3][3] = (float)WS.drops;
            // the simulation: 2 steps a frame of half the frame (stable: dt * sqrt(g d) < a cell)
            fr->water[4][0] = clampf(rain->flow, 0.0f, 1.0f), fr->water[4][1] = fmaxf(rain->water_gain, 0.0f);
            fr->water[4][2] = (rain->infiltration + rain->evaporation) * 0.1f / 3600.0f * fmaxf(rain->water_gain, 1.0f);
            fr->water[4][3] = fminf(dt, 1.0f / 30.0f) * 0.5f;
            fr->water[5][0] = 1.0f / fmaxf(rain->wet_time, 0.1f), fr->water[5][1] = 1.0f / fmaxf(rain->dry_time, 0.1f);
            fr->water[5][2] = rain->micro_relief;
            fr->water[5][3] = (WS.rt ? 1.0f : 0.0f) + (WS.reset ? 2.0f : 0.0f);
            fr->water[6][0] = (float)WS.rows_first, fr->water[6][1] = (float)WS.rows_count;
            // (from 50 m over the camera down past the ground under it: a camera 300 m up still reaches it)
            fr->water[6][2] = fr->camera_pos[2] + 5000.0f, fr->water[6][3] = fmaxf(30000.0f, fr->camera_pos[2] + 5000.0f - zr + 10000.0f);
            fr->water[38][0] = rain->start_water * 0.1f;
            // snow: on (1), the flakes' size (mm), the cover's growth and melt (1/s), the steepest surface's normal z
            fr->water[39][0] = rain->snow ? 1.0f : 0.0f, fr->water[39][1] = rain->flake_size;
            fr->water[39][2] = 1.0f / fmaxf(rain->cover_time, 1.0f);
            fr->water[39][3] = cosf(fminf(fmaxf(rain->cover_angle, 1.0f), 89.0f) * 0.017453293f);
            fr->water[38][1] = rain->melt_time > 0.0f ? 1.0f / rain->melt_time : 0.0f;
            // the cover in the open (as each cell's on the GPU): far away and for the places entering the window
            if (fresh || WS.snow_key != rain->start_cover) WS.snow_cover = clampf(rain->start_cover, 0.0f, 1.0f), WS.snow_key = rain->start_cover;
            if (rain->snow && rain->intensity > 0.0f) WS.snow_cover += dt * fr->water[39][2] * clampf(rain->intensity / 10.0f, 0.1f, 3.0f);
            else WS.snow_cover -= dt * fr->water[38][1];
            WS.snow_cover = clampf(WS.snow_cover, 0.0f, 1.0f);
            // the open ground's wetness, past the simulated window (water.glsl rain_shade): a flat open cell's (water_sim
            // pass 2): its standing water (the rain times the gain, less what soaks in and dries) and its wetness, a film
            // of 0.3 mm already wet
            if (fresh) WS.open_wet = rain->intensity > 0.05f && !rain->snow ? 0.3f : 0.0f, WS.open_water = rain->start_water * 0.1f;
            {
                float rdt = fminf(dt, 1.0f / 30.0f), rn = rain->snow ? 0.0f : rain->intensity * 0.1f / 3600.0f * fmaxf(rain->water_gain, 0.0f);
                WS.open_water = fmaxf(WS.open_water + rdt * (rn - fr->water[4][2]), 0.0f);
                if (rain->intensity > 0.05f && !rain->snow) WS.open_wet += rdt * fr->water[5][0];
                else WS.open_wet -= rdt * fr->water[5][1];
                float film = WS.open_water / 0.03f; // (smoothstep(0, 0.03 cm))
                film = film >= 1.0f ? 1.0f : film * film * (3.0f - 2.0f * film);
                WS.open_wet = clampf(fmaxf(WS.open_wet, film), 0.0f, 1.0f);
            }
            fr->water[38][2] = WS.snow_cover, fr->water[38][3] = clampf(rain->start_cover, 0.0f, 1.0f);
            // (2026-11-15, "quando algo molhado entrar em um lugar seco molhar o chão") the same map, in the rain, keeps the
            // wet marks: the prints of wet feet (water.glsl rain_marks) and the drips of wet actors where it is dry, drying
            // over Wet Marks Dry Time (water[49].x 2; .w the drying rate)
            bool snow_prints = rain->snow || WS.snow_cover > 0.0f, wet_marks = !snow_prints && rain->wet_marks > 0.0f;
            float marks = clampf(rain->wet_marks, 0.0f, 2.0f);
            bool raining = rain->intensity > 0.05f;
            memset(fr->water[92], 0, sizeof(float) * 4 * 4);
            if ((snow_prints || wet_marks) && buf(g_vk, &WS.trail, sizeof(float) * 2 * TRAIL_N * TRAIL_N, false)) { // the feet's prints
                WS.trail_on = true;
                fr->water[48][2] = floorf(fr->camera_pos[0] / TRAIL_CELL) - TRAIL_N / 2, fr->water[48][3] = floorf(fr->camera_pos[1] / TRAIL_CELL) - TRAIL_N / 2;
                uint32_t lo = (uint32_t)WS.trail.addr, hi = (uint32_t)(WS.trail.addr >> 32);
                memcpy(&fr->water[48][0], &lo, 4), memcpy(&fr->water[48][1], &hi, 4);
                fr->water[49][0] = 1.0f, fr->water[49][2] = fminf(dt, 0.1f);
                fr->water[49][3] = rain->snow ? fr->water[39][2] * clampf(rain->intensity / 10.0f, 0.1f, 3.0f) : 0.0f; // (as the cover grows)
                if (wet_marks) fr->water[49][0] = 2.0f, fr->water[49][3] = 1.0f / fmaxf(rain->wet_marks_dry_time, 1.0f);
                uint32_t ns = 0;
                for (int k = 0; k < 16; k++) WS.feet[k].seen = false;
                for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                    const EcsArchetype* a = &w->archetypes[ai];
                    if (!a->count || !(a->mask & COMP_BIT(COMP_CHARACTER)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
                    const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
                    const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
                    const Entity* ents = ecs_arch_entities(a);
                    for (uint32_t r = 0; r < a->count; r++) {
                        int k = -1, fr_k = -1;
                        for (int j = 0; j < 16; j++) {
                            if (WS.feet[j].e == ents[r] && WS.feet[j].e) k = j;
                            if (!WS.feet[j].e && fr_k < 0) fr_k = j;
                        }
                        const float* pos = tr[r].location;
                        if (k < 0) { // (a new walker: from here)
                            if (fr_k < 0) continue;
                            k = fr_k, WS.feet[k].e = ents[r], WS.feet[k].last[0] = pos[0], WS.feet[k].last[1] = pos[1], WS.feet[k].acc = 0.0f;
                        }
                        WS.feet[k].seen = true;
                        float dx = pos[0] - WS.feet[k].last[0], dy = pos[1] - WS.feet[k].last[1], m = sqrtf(dx * dx + dy * dy);
                        WS.feet[k].last[0] = pos[0], WS.feet[k].last[1] = pos[1];
                        if (wet_marks) { // its feet's water: soaked out in the rain, as wet as it is, drying a little each step
                            float own = 0.0f;
                            int ex = 0;
                            for (int j = 0; j < WET_MAX; j++)
                                if (WS.wet[j].e == ents[r] && WS.wet[j].e) own = WS.wet[j].wet, ex = WS.wet[j].exposed;
                            WS.feet[k].wet = fmaxf(WS.feet[k].wet, raining && ex == 1 ? 1.0f : own * 0.8f);
                        }
                        if (m > 200.0f || !ch[r].on_ground) { WS.feet[k].acc = 0.0f; continue; } // (a teleport, a jump)
                        if (m > 0.05f) WS.feet[k].heading = atan2f(dy, dx);
                        WS.feet[k].acc += m;
                        // a print each step, the feet 11 cm either side of the path (a step falls where the body is now)
                        while (WS.feet[k].acc >= TRAIL_STRIDE && ns < 6) {
                            WS.feet[k].acc -= TRAIL_STRIDE;
                            float h = WS.feet[k].heading, side = (WS.feet[k].side ^= 1u) ? 11.0f : -11.0f;
                            float st = 1.0f;
                            if (wet_marks) {
                                st = fminf(WS.feet[k].wet * marks, 1.0f);
                                WS.feet[k].wet *= 0.9f; // (about 20 prints and the feet are dry)
                                if (st < 0.04f) continue;
                            }
                            float* s = fr->water[50 + ns++];
                            s[0] = pos[0] - sinf(h) * side, s[1] = pos[1] + cosf(h) * side, s[2] = h, s[3] = st;
                        }
                    }
                }
                for (int k = 0; k < 16; k++)
                    if (!WS.feet[k].seen) WS.feet[k].e = 0, WS.feet[k].wet = 0.0f; // (gone)
                // (2026-11-15) a wet actor where the rain doesn't fall on it (under a roof, or the rain stopped) drips: spots
                // on the floor around it (negative strength: a drip's splat). (The drops seen falling off it were taken away:
                // "o escoamento na parte de baixo do modelo fica muito forte e feio, pode tirar ele")
                float ad = clampf(rain->actor_drips, 0.0f, 2.0f);
                if (wet_marks && ad > 0.0f) {
                    for (int k = 0; k < WET_MAX; k++) {
                        if (!WS.wet[k].e || !WS.wet[k].seen || WS.wet[k].wet < 0.15f || (raining && WS.wet[k].exposed == 1)) { WS.drip_acc[k] = 0.0f; continue; }
                        const CTransform* t = ecs_get(w, WS.wet[k].e, COMP_TRANSFORM);
                        if (!t) continue;
                        const CCharacter* chr = ecs_get(w, WS.wet[k].e, COMP_CHARACTER);
                        float rad = chr ? fmaxf(chr->radius, 20.0f) : 30.0f;
                        // (2026-11-15, "as goteiras dos personagens em locais fechados tão muito intensas") clothes drip hard
                        // for the first seconds out of the rain, then hardly at all while they stay wet: ~2 drips a second
                        // soaked, fading over ~20 s to a tenth
                        float rate = WS.wet[k].wet * WS.wet[k].wet * 2.0f * ad * (0.1f + 0.9f * expf(-WS.wet[k].out_t / 20.0f));
                        WS.drip_acc[k] += rate * dt;
                        while (WS.drip_acc[k] >= 1.0f && ns < 6) {
                            WS.drip_acc[k] -= 1.0f;
                            WS.mark_rng = WS.mark_rng * 1664525u + 1013904223u;
                            float u = (float)(WS.mark_rng >> 8) * (1.0f / 16777216.0f);
                            WS.mark_rng = WS.mark_rng * 1664525u + 1013904223u;
                            float v = (float)(WS.mark_rng >> 8) * (1.0f / 16777216.0f);
                            float rr = rad * sqrtf(u) * 0.9f, an = v * 6.2831853f;
                            float* s = fr->water[50 + ns++];
                            s[0] = t->location[0] + cosf(an) * rr, s[1] = t->location[1] + sinf(an) * rr, s[2] = v * 6.28f;
                            s[3] = -fminf((0.45f + 0.55f * WS.wet[k].wet) * marks, 1.0f);
                        }
                        if (WS.drip_acc[k] > 3.0f) WS.drip_acc[k] = 3.0f;
                    }
                }
                fr->water[49][1] = (float)ns;
                WS.stats.footprints += ns;
            }
            // (2026-11-10, "os pingos que saírem dele ter um ângulo de movimento") the 3 nearest things moving under the
            // rain: water[57 + 2k] = center x, y, z, radius; water[58 + 2k] = velocity (cm/s), half height; water[56] =
            // their count, this frame's dt, the drops' Fall Speed
            fr->water[56][1] = fmaxf(dt, 1e-4f), fr->water[56][2] = clampf(rain->fall_speed, 0.25f, 3.0f);
            fr->water[56][3] = WS.open_wet; // (water.glsl rain_shade: the ground past the window)
            if (!rain->snow) {
                for (int k = 0; k < 32; k++) WS.movers[k].seen = false;
                float best[3][8], bd[3] = { 1e30f, 1e30f, 1e30f };
                uint32_t nb = 0;
                const uint64_t any = COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_VEHICLE) | COMP_BIT(COMP_RIGID_BODY);
                for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                    const EcsArchetype* a = &w->archetypes[ai];
                    if (!a->count || !(a->mask & any) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
                    const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
                    const CCharacter* ch = (a->mask & COMP_BIT(COMP_CHARACTER)) ? ecs_arch_column(a, COMP_CHARACTER) : NULL;
                    const Entity* ents = ecs_arch_entities(a);
                    for (uint32_t r = 0; r < a->count; r++) {
                        if ((tr[r].flags & TRANSFORM_MOBILITY_MASK) != MOBILITY_MOVABLE && !ch) continue;
                        const float* pos = tr[r].location;
                        int k = -1, fk = -1;
                        for (int j = 0; j < 32; j++) {
                            if (WS.movers[j].e == ents[r] && WS.movers[j].e) k = j;
                            if (!WS.movers[j].e && fk < 0) fk = j;
                        }
                        if (k < 0) {
                            if (fk < 0) continue;
                            k = fk, WS.movers[k].e = ents[r], memcpy(WS.movers[k].last, pos, 12);
                        }
                        WS.movers[k].seen = true;
                        float v[3] = { (pos[0] - WS.movers[k].last[0]) / fmaxf(dt, 1e-4f), (pos[1] - WS.movers[k].last[1]) / fmaxf(dt, 1e-4f), (pos[2] - WS.movers[k].last[2]) / fmaxf(dt, 1e-4f) };
                        memcpy(WS.movers[k].last, pos, 12);
                        float sp2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                        if (sp2 < 100.0f || sp2 > 4e6f) continue; // (still, or a teleport: > 20 m/s)
                        float sc = fmaxf(fabsf(tr[r].scale[0]), fmaxf(fabsf(tr[r].scale[1]), fabsf(tr[r].scale[2])));
                        float rad = ch ? fmaxf(ch[r].radius, 20.0f) * 1.8f : (a->mask & COMP_BIT(COMP_VEHICLE)) ? 260.0f : 60.0f * sc;
                        float hh = ch ? fmaxf(ch[r].half_height, 40.0f) * 1.15f : rad;
                        float cz = ch ? pos[2] : pos[2] + hh * 0.5f;
                        float dx = pos[0] - fr->camera_pos[0], dy = pos[1] - fr->camera_pos[1], d2 = dx * dx + dy * dy;
                        if (d2 > 3000.0f * 3000.0f) continue;
                        int slot = nb < 3 ? (int)nb++ : -1;
                        if (slot < 0) // (the farthest of the three makes room)
                            for (int j = 0; j < 3; j++)
                                if (d2 < bd[j] && (slot < 0 || bd[j] > bd[slot])) slot = j;
                        if (slot < 0) continue;
                        bd[slot] = d2;
                        float m[8] = { pos[0], pos[1], cz, rad, v[0], v[1], v[2], hh };
                        memcpy(best[slot], m, sizeof m);
                    }
                }
                for (int k = 0; k < 32; k++)
                    if (!WS.movers[k].seen) WS.movers[k].e = 0; // (gone)
                fr->water[56][0] = (float)nb;
                for (uint32_t k = 0; k < nb; k++) memcpy(fr->water[57 + 2 * k], best[k], 32);
            }
            if (rain->snow) { // (flakes: in a smaller box around the camera, more of them; no splashes)
                WS.splashes = 0, WS.drops = (uint32_t)fminf((float)WS.drops * 1.5f, (float)MAX_DROPS);
                fr->water[3][3] = (float)WS.drops;
            }
            fr->water_addr[0] = WS.surface.addr, fr->water_addr[1] = WS.water.addr, fr->water_addr[2] = WS.flux.addr, fr->water_addr[7] = WS.rings.addr;
            // (Q27.6) the persistent wet map: what the window keeps of a place after it leaves (its water and wetness,
            // 16 m cells), evolving where the camera isn't by the rain there (the storms', or the actor's everywhere)
            bool pnew = !WS.persist.buffer;
            if (buf(g_vk, &WS.persist, sizeof(float) * 4 * PERSIST_N * PERSIST_N, false)) {
                if (pnew || fresh) {
                    VkCommandBuffer cmd = gpu_begin_once(g_vk);
                    vkCmdFillBuffer(cmd, WS.persist.buffer, 0, VK_WHOLE_SIZE, 0);
                    gpu_end_once(g_vk, cmd);
                }
                WS.persist_on = true;
                WS.persist_first[0] = (int32_t)floorf(fr->camera_pos[0] / PERSIST_CELL) - PERSIST_N / 2;
                WS.persist_first[1] = (int32_t)floorf(fr->camera_pos[1] / PERSIST_CELL) - PERSIST_N / 2;
                uint32_t lo = (uint32_t)WS.persist.addr, hi = (uint32_t)(WS.persist.addr >> 32);
                memcpy(&fr->water[52][0], &lo, 4), memcpy(&fr->water[52][1], &hi, 4);
                fr->water[52][2] = (float)WS.persist_first[0], fr->water[52][3] = (float)WS.persist_first[1];
                fr->water[53][0] = PERSIST_CELL, fr->water[53][1] = (float)PERSIST_N, fr->water[53][2] = WS.persist_skip, fr->water[53][3] = 1.0f;
                WS.persist_skip = 0.0f;
                float st[WFIELD_MAX_STORMS][4];
                uint32_t ns = wfield_pack(st);
                fr->water[54][0] = (float)ns, fr->water[54][1] = wfield_on() ? 1.0f : 0.0f;
                fr->water[54][2] = fmaxf(rain->evaporation, 0.0f) * 0.1f / 3600.0f; // (the puddles' own drying: the real rate)
                for (uint32_t k = 0; k < ns; k++) memcpy(fr->water[13 + k], st[k], 16);
                // the probe this slot asked for last time
                uint32_t gs = g_vk->frame % RE_FRAMES_IN_FLIGHT;
                if (WS.probe_asked[gs] && WS.persist_rb[gs].mapped) {
                    const float* g = WS.persist_rb[gs].mapped;
                    uint32_t k;
                    memcpy(&k, &g[3], 4);
                    if (k == WS.probe_key[gs]) WS.probe_val[0] = g[0], WS.probe_val[1] = g[1], WS.probe_ok = true;
                    WS.probe_asked[gs] = false;
                }
            }
            if (WS.reset) WS.rows_first = 0, WS.rows_count = n, fr->water[6][0] = 0.0f, fr->water[6][1] = (float)n; // (the whole surface, once)
            WS.rain_key[0] = rain->intensity;
        }
    }
    // the ocean
    if (ocean && getenv("REALENGINE_NO_OCEAN")) ocean = NULL; // (measuring without it)
    static CWater s_sea;
    if (ocean && wfield_on()) { // (Q27.6) a storm's gust front over the sea: a rougher sea (the spectrum made again by the half m/s)
        WeatherSample s;
        wfield_sample(fr->camera_pos[0], fr->camera_pos[1], &s);
        s_sea = *ocean;
        s_sea.wind_speed = floorf((ocean->wind_speed + s.gust) * 2.0f + 0.5f) * 0.5f;
        ocean = &s_sea;
    }
    if (ocean) {
        float key[6] = { ocean->wind_speed, ocean->wind_dir, ocean->swell, 0, 0, 0 };
        if (memcmp(key, WS.spec_key, sizeof key) != 0 || !WS.ntop) make_spectrum(ocean->wind_speed, ocean->wind_dir, ocean->swell), memcpy(WS.spec_key, key, sizeof key);
        WS.ocean_on = true, WS.ocean_level = ocean_z;
        fr->water[7][0] = ocean_z, fr->water[7][1] = ocean->choppiness, fr->water[7][2] = ocean->foam, fr->water[7][3] = 1.0f + fminf(fmaxf(ocean->shore_foam, 0.0f), 1.0f); // (w: on, 1 + its shore foam)
        memcpy(fr->water[8], ocean->color, 12), fr->water[8][3] = ocean->clarity;
        memcpy(fr->water[9], ocean->scatter, 12), fr->water[9][3] = ocean->interaction;
        fr->water[10][0] = k_cascade[0], fr->water[10][1] = k_cascade[1], fr->water[10][2] = k_cascade[2], fr->water[10][3] = WS.time;
        fr->water[37][1] = 0.0f, fr->water[37][2] = ocean->swell, fr->water[37][3] = ocean->wind_dir * 0.017453293f;
        uint32_t slot = g_vk ? g_vk->frame : 0; // (its last frame is done: vkctx_begin_frame waited)
        if (WS.h0_dirty & (1u << slot)) {
            memcpy(WS.h0[slot].mapped, WS.h0_cpu, sizeof(float) * 4 * 3 * OCEAN_N * OCEAN_N);
            WS.h0_dirty &= ~(1u << slot);
        }
        WS.h0_slot = slot;
        fr->water_addr[4] = WS.h0[slot].addr, fr->water_addr[5] = WS.fft.addr, fr->water_addr[6] = WS.ocean.addr;
    } else fr->water[37][1] = -1.0f;
    // the ripples (any water): a grid around the camera, the spheres of what moves in it
    WS.ripples = ocean || WS.nlakes;
    if (WS.ripples) {
        fr->water[11][0] = RIPPLE_CELL, fr->water[11][1] = (float)RIPPLE_N;
        fr->water[11][2] = floorf(fr->camera_pos[0] / RIPPLE_CELL) - RIPPLE_N / 2, fr->water[11][3] = floorf(fr->camera_pos[1] / RIPPLE_CELL) - RIPPLE_N / 2;
        fr->water[12][0] = fminf(fmaxf(dt, 1.0f / 240.0f), 1.0f / 30.0f), fr->water[12][1] = 0.985f, fr->water[12][2] = 600.0f;
        uint32_t ns = 0;
        float lvl = ocean ? ocean_z : WS.nlakes ? lakes[0][2] : 0.0f;
        for (uint32_t ai = 0; ai < w->archetype_count && ns < 16; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            bool mover = (a->mask & (COMP_BIT(COMP_VEHICLE) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_RIGID_BODY))) != 0;
            if (!a->count || !mover || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            float rad = (a->mask & COMP_BIT(COMP_VEHICLE)) ? 220.0f : (a->mask & COMP_BIT(COMP_CHARACTER)) ? 40.0f : 0.0f;
            for (uint32_t r = 0; r < a->count && ns < 16; r++) {
                float rr = rad > 0.0f ? rad : 50.0f * fmaxf(fabsf(tr[r].scale[0]), fabsf(tr[r].scale[1]));
                if (fabsf(tr[r].location[2] - lvl) > rr * 1.5f) continue;
                float* s = fr->water[13 + ns++];
                s[0] = tr[r].location[0], s[1] = tr[r].location[1], s[2] = tr[r].location[2] - (a->mask & COMP_BIT(COMP_CHARACTER) ? 60.0f : 0.0f), s[3] = rr;
            }
        }
        fr->water[12][3] = (float)ns;
        if (!ocean) fr->water[9][3] = 1.0f, fr->water[7][0] = lvl;
        fr->water_addr[3] = WS.ripple.addr;
    }
    WS.stats.rain = WS.rain, WS.stats.ocean = WS.ocean_on;
    WS.stats.wet_followed = 0, WS.stats.wet_max = 0.0f;
    for (int k = 0; k < WET_MAX; k++)
        if (WS.wet[k].e && WS.wet[k].seen) WS.stats.wet_followed++, WS.stats.wet_max = fmaxf(WS.stats.wet_max, WS.wet[k].wet);
    WS.stats.drops = WS.drops, WS.stats.splashes = WS.splashes, WS.stats.cells = WS.rain ? WS.n * WS.n : 0, WS.stats.lakes = WS.nlakes;
    WS.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
}

static void dispatch(VkCommandBuffer cmd, Push p, uint32_t pass, uint32_t gx, uint32_t gy, uint32_t gz) {
    p.pass = pass;
    vkCmdPushConstants(cmd, WS.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, gx, gy, gz);
}

void water_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr) {
    if (!WS.rain && !WS.ocean_on && !WS.ripples) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VS = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, WS.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, WS.pipe);
    gpu_barrier(cmd, VS | FS | CS, RW, CS, RW); // (last frame's draws read what this writes)
    Push p = *push;
    if (WS.rain) {
        uint32_t n = WS.n, g = (n + 7) / 8;
        dispatch(cmd, p, 0, g, (WS.rows_count + 7) / 8, 1); // the surface's rows
        dispatch(cmd, p, 4, 128 / 8, 128 / 8, 1);           // the rings baked (W_RIP_N)
        gpu_barrier(cmd, CS, RW, CS, RW);
        for (int step = 0; step < 2; step++) { // two half steps of the shallow water
            dispatch(cmd, p, 1, g, g, 1);
            gpu_barrier(cmd, CS, RW, CS, RW);
            dispatch(cmd, p, 2, g, g, 1);
            gpu_barrier(cmd, CS, RW, CS, RW);
        }
        static int s_no_persist = -1; // (REALENGINE_NO_PERSIST=1: without it, the measure)
        if (s_no_persist < 0) s_no_persist = getenv("REALENGINE_NO_PERSIST") != NULL;
        if (WS.persist_on && !s_no_persist) { // (Q27.6) the persistent wet map (~0.02 ms: 65 536 cells)
            dispatch(cmd, p, 15, PERSIST_N / 8, PERSIST_N / 8, 1);
            gpu_barrier(cmd, CS, RW, CS, RW);
            uint32_t ps = g_vk->frame % RE_FRAMES_IN_FLIGHT;
            int32_t cx = (int32_t)floorf(WS.probe_xy[0] / PERSIST_CELL), cy = (int32_t)floorf(WS.probe_xy[1] / PERSIST_CELL);
            int32_t dx = cx - WS.persist_first[0], dy = cy - WS.persist_first[1], N = PERSIST_N;
            if (WS.probe_on && dx >= 0 && dy >= 0 && dx < N && dy < N && buf(g_vk, &WS.persist_rb[ps], 16, true)) {
                uint32_t ix = (uint32_t)(((cx % N) + N) % N), iy = (uint32_t)(((cy % N) + N) % N);
                VkBufferCopy c = { (VkDeviceSize)(iy * (uint32_t)N + ix) * 16u, 0, 16 };
                gpu_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                vkCmdCopyBuffer(cmd, WS.persist.buffer, WS.persist_rb[ps].buffer, 1, &c);
                gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, CS, RW);
                WS.probe_key[ps] = (uint32_t)(cx & 0x7FFF) | ((uint32_t)(cy & 0x7FFF) << 15) | 0x80000000u;
                WS.probe_asked[ps] = true;
            }
        }
        // the window's middle cell (its first surface: the ground under the camera) into this slot's host buffer
        uint32_t gs = g_vk->frame % RE_FRAMES_IN_FLIGHT;
        if (buf(g_vk, &WS.ground_rb[gs], 16, true)) {
            int32_t cx = WS.first[0] + (int32_t)n / 2, cy = WS.first[1] + (int32_t)n / 2, in = (int32_t)n;
            int32_t mx = ((cx % in) + in) % in, my = ((cy % in) + in) % in;
            VkBufferCopy c = { (VkDeviceSize)((uint32_t)my * n + (uint32_t)mx) * 16u, 0, 16 };
            gpu_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdCopyBuffer(cmd, WS.surface.buffer, WS.ground_rb[gs].buffer, 1, &c);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, CS, RW);
            WS.ground_key[gs] = (uint32_t)(cx & 0xFFFF) | ((uint32_t)(cy & 0xFFFF) << 16);
            WS.ground_asked[gs] = true;
        }
        // (2026-11-14) the cells over the camera and the followed actors (water.c wet_update): open or under something
        uint32_t nq = WS.wet_req_n[gs];
        if (nq && buf(g_vk, &WS.wet_rb[gs], 16u * (WET_MAX + 1u), true) && WS.wet_rb[gs].mapped) {
            VkBufferCopy cs[WET_MAX + 1];
            uint32_t nc = 0;
            float* m = WS.wet_rb[gs].mapped;
            for (uint32_t i = 0; i < nq; i++) {
                m[4 * i] = -1e9f; // (outside the window: open)
                if (WS.wet_req_in[gs][i]) cs[nc++] = (VkBufferCopy){ (VkDeviceSize)WS.wet_req_cell[gs][i] * 16u, (VkDeviceSize)i * 16u, 16 };
            }
            if (nc) {
                gpu_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                vkCmdCopyBuffer(cmd, WS.surface.buffer, WS.wet_rb[gs].buffer, nc, cs);
                gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, CS, RW);
            }
        }
    }
    if (WS.trail_on) { // the snow's trail (~0.03 ms: 512^2 cells, a few feet)
        dispatch(cmd, p, 14, TRAIL_N / 8, TRAIL_N / 8, 1);
        gpu_barrier(cmd, CS, RW, CS, RW);
    }
    if (WS.ripples) {
        dispatch(cmd, p, 3, RIPPLE_N / 8, RIPPLE_N / 8, 1);
        gpu_barrier(cmd, CS, RW, CS, RW);
    }
    if (WS.ocean_on) {
        p.instances = WS.h0[WS.h0_slot].addr;
        dispatch(cmd, p, 10, OCEAN_N / 8, OCEAN_N / 8, 3); // the spectrum at this time, its 3 cascades
        gpu_barrier(cmd, CS, RW, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, WS.fft_pipe);
        dispatch(cmd, p, 11, OCEAN_N, 6, 1); // the rows (6 fields: 3 cascades x 2), then the columns
        gpu_barrier(cmd, CS, RW, CS, RW);
        dispatch(cmd, p, 12, OCEAN_N, 6, 1);
        gpu_barrier(cmd, CS, RW, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, WS.pipe);
        dispatch(cmd, p, 13, OCEAN_N / 8, OCEAN_N / 8, 3);
    }
    (void)fr;
    gpu_barrier(cmd, CS, RW, VS | FS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

uint32_t water_draw_surfaces(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe) {
    if ((!WS.ocean_on && !WS.nlakes && !WS.nrivers) || !pipe) return 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p = *push;
    uint32_t draws = 0;
    for (uint32_t i = 0; i < WS.nlakes; i++) {
        p.pass = i;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 6u * 128u * 128u, 1, 0, 0), draws++;
    }
    // (B12c) the rivers: ribbons along their samples (pc.instances: pc.vertices is the meshes' the rays' hits read)
    for (uint32_t i = 0; i < (getenv("REALENGINE_NO_RIVERS") ? 0u : WS.nrivers); i++) {
        p.pass = 200u + i;
        p.instances = WS.rivers[WS.river_slot].addr;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 6u * RIVER_ACROSS * (WS.river_n[i] - 1u), 1, 0, 0), draws++;
    }
    p.instances = push->instances;
    if (WS.ocean_on) {
        p.pass = 100;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 6u * 200u * 256u, 1, 0, 0), draws++;
    }
    return draws;
}

uint32_t water_draw_rain(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe) {
    if (!WS.rain || !pipe || !(WS.drops + WS.splashes)) return 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    Push p = *push;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmd, 6u * (WS.drops + WS.splashes), 1, 0, 0);
    return 1;
}

// --- The self-test: the spectrum's waves for a wind (the significant height of a fully developed sea grows with the
// wind squared: ~2.1 m at 10 m/s, Pierson-Moskowitz), and the shallow water's mass on the CPU ---
int water_selftest(void) {
    int fails = 0;
    static float h0[4 * 3 * OCEAN_N * OCEAN_N];
    float* h0_was = WS.h0_cpu;
    WS.h0_cpu = h0;
    float hs[2];
    float winds[2] = { 5.0f, 12.0f };
    for (int k = 0; k < 2; k++) {
        make_spectrum(winds[k], 0.0f, 1.0f);
        double var = 0.0;
        for (size_t i = 0; i < (size_t)3 * OCEAN_N * OCEAN_N; i++) var += (double)h0[4 * i] * h0[4 * i] + (double)h0[4 * i + 1] * h0[4 * i + 1];
        hs[k] = 4.0f * sqrtf(2.0f * (float)var); // (Hs = 4 sigma; the height's variance: both halves, h0 and conj(h0(-k)))
        printf("[RealEngine] water: ocean at %.0f m/s: significant wave height %.2f m, %u big waves on the CPU\n", winds[k], hs[k], WS.ntop);
    }
    // (a fully developed sea: Hs ~ 0.21 U^2 / g: 0.5 m at 5 m/s, 3.1 m at 12 m/s; the cascades keep waves under 400 m)
    if (!(hs[0] > 0.2f && hs[0] < 1.0f && hs[1] > 1.5f && hs[1] < 4.5f)) printf("[RealEngine] FAIL: water: the ocean's spectrum\n"), fails++;
    WS.h0_cpu = h0_was;
    // the CPU heights move and stay within a few Hs
    WS.ocean_on = true, WS.ocean_level = 0.0f, WS.time = 3.0f;
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 200; i++) {
        float z;
        water_ocean_height((float)i * 137.0f, (float)i * 71.0f, &z);
        mn = fminf(mn, z), mx = fmaxf(mx, z);
    }
    printf("[RealEngine] water: the ocean under the boats: %.0f to %.0f cm\n", mn, mx);
    if (!(mx - mn > 30.0f && mx - mn < 2000.0f)) printf("[RealEngine] FAIL: water: the ocean's height for the boats\n"), fails++;
    { // (2026-11-10) a Water Body Exclusion (20 x 10 m turned 90 degrees at 1000, 0): no ocean inside it, ocean outside
        float q[6] = { 1000.0f, 0.0f, 1000.0f, 500.0f, 0.0f, 1.0f }, z1, z2, z3;
        memcpy(WS.excl[0], q, sizeof q), WS.nexcl = 1;
        bool in = water_ocean_height(1000.0f, 900.0f, &z1); // (turned: its 20 m along Y)
        bool out1 = water_ocean_height(1000.0f, 1200.0f, &z2), out2 = water_ocean_height(1700.0f, 0.0f, &z3);
        WS.nexcl = 0;
        printf("[RealEngine] water: Exclusion: inside %s, outside %s / %s\n", in ? "ocean" : "none", out1 ? "ocean" : "none", out2 ? "ocean" : "none");
        if (in || !out1 || !out2) printf("[RealEngine] FAIL: water: the Water Body Exclusion\n"), fails++;
    }
    { // (2026-11-14) the Rain's fields by name and its presets (Set / Get Rain Property, Set Rain Preset)
        CRain r;
        memset(&r, 0, sizeof r);
        const RainProp* a = rain_prop_find("windspeed");
        const RainProp* b = rain_prop_find("Muffle Under Roof");
        rain_apply_preset(&r, RAIN_PRESET_STORM);
        bool ok = a && a->offset == offsetof(CRain, wind_speed) && b && b->offset == offsetof(CRain, muffle) && !rain_prop_find("nope") &&
                  r.intensity == 100.0f && r.thunder > 0.0f && r.preset == RAIN_PRESET_STORM;
        printf("[RealEngine] water: Rain properties %u, presets %s\n", g_rain_prop_count, ok ? "ok" : "wrong");
        if (!ok) printf("[RealEngine] FAIL: water: the Rain's properties by name\n"), fails++;
    }
    { // (2026-11-15) the shading styles: their defaults, the Stylized preset shades stylized, a realistic preset brings the
      // shading back, and the shading fields by name (they don't make the Preset Custom)
        CRain r;
        memset(&r, 0, sizeof r);
        rain_apply_shading_style(&r, RAIN_SHADE_CINEMATIC);
        bool ok = r.shading_style == RAIN_SHADE_CINEMATIC && r.film > 1.0f && r.drop_light > 1.0f && r.mist > 1.0f;
        rain_apply_preset(&r, RAIN_PRESET_STYLIZED);
        ok = ok && r.shading_style == RAIN_SHADE_STYLIZED && r.puddle_edge == 0.0f;
        rain_apply_preset(&r, RAIN_PRESET_STORM);
        ok = ok && r.shading_style == RAIN_SHADE_REALISTIC && r.darkening == 1.0f;
        const RainProp* lw = rain_prop_find("lens water");
        const RainProp* fm = rain_prop_find("Film");
        ok = ok && lw && lw->offset == offsetof(CRain, lens_water) && fm && fm->offset == offsetof(CRain, film) && rain_prop_find("Rain Mist");
        printf("[RealEngine] water: Rain shading styles %s\n", ok ? "ok" : "wrong");
        if (!ok) printf("[RealEngine] FAIL: water: the Rain's shading styles\n"), fails++;
    }
    fails += lens_selftest(); // (2026-11-15) the water on the lens
    WS.ocean_on = false, WS.ntop = 0, WS.time = 0.0f;
    printf("[RealEngine] water self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
