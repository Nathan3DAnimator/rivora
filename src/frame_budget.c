// The frame budget (frame_budget.h, notas-dev/orcamento.md)
#include "frame_budget.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ability.h"
#include "ai.h"
#include "anim.h"
#include "blueprint.h"
#include "cloth.h"
#include "traffic.h"
#include "crowd.h"
#include "debug_viewer.h"
#include "dialogue.h"
#include "game_rules.h"
#include "health.h"
#include "hud.h"
#include "interaction.h"
#include "inventory.h"
#include "particles.h"
#include "physics.h"
#include "quest.h"
#include "render.h"
#include "vehicle.h"

static const char* const g_names[FB_SLICES] = {
    "Geometry and materials", "Direct light and shadows", "GI and reflections", "Volumes, water and sky",
    "Camera, color and post", "Reconstruction and UI",
    "Render (CPU)", "Blueprints", "Physics", "Animation", "AI", "Particles (CPU)", "Game modules",
};
// R11's split of the 10 ms (geometry 2 + materials 1, light and shadows 1.5, GI and reflections 3, volumes 1, post 0.8,
// reconstruction and slack 0.7); the CPU's own 10 ms by system (the frame is the slower of the two)
static const double g_goal10[FB_SLICES] = { 3.0, 1.5, 3.0, 1.0, 0.8, 0.7, 2.0, 1.5, 1.5, 1.0, 1.0, 0.5, 0.5 };

const char* fb_slice_name(int s) { return s >= 0 && s < FB_SLICES ? g_names[s] : ""; }
bool fb_slice_gpu(int s) { return s >= 0 && s < FB_GPU_COUNT; }
double fb_goal(int s, double budget_ms) { return s >= 0 && s < FB_SLICES ? g_goal10[s] * budget_ms / 10.0 : 0.0; }

void fb_sample(double o[FB_SLICES]) {
    memset(o, 0, sizeof(double) * FB_SLICES);
    const RenderStats* r = render_stats();
    // the main pass's parts are timed with the detail on (fb_begin, the Debug Viewer); without it, the whole scene and
    // main pass count as geometry
    bool detail = r->gpu_meshes_ms > 0.0 || r->gpu_cull_ms > 0.0;
    if (detail) {
        double upload = r->gpu_scene_ms - r->gpu_atmosphere_ms - r->gpu_weather_ms;
        o[FB_GEOMETRY] = (upload > 0.0 ? upload : 0.0) + r->gpu_meshes_ms;
        // (the AO is post's, the upscale the reconstruction's: both inside the sky's interval)
        o[FB_VOLUMES] = r->gpu_atmosphere_ms + r->gpu_weather_ms +
                        fmax(r->gpu_sky_ms - r->gpu_gtao_ms - r->gpu_sss_ms - r->gpu_upscale_ms - r->gpu_denoise_ms, 0.0);
    } else o[FB_GEOMETRY] = fmax(r->gpu_scene_ms + r->gpu_main_ms - r->gpu_upscale_ms - r->gpu_denoise_ms - r->gpu_sss_ms, 0.0);
    o[FB_SHADOWS] = r->gpu_shadow_ms;
    o[FB_GI] = r->gpu_gi_ms + r->gpu_denoise_ms; // (the unified denoiser, A9.1d: the GI's, the reflections' and the shadows' filter)
    o[FB_POST] = r->gpu_mask_ms + r->gpu_exposure_ms + r->gpu_post_ms + r->gpu_gtao_ms + r->gpu_sss_ms; // (the screen space AO, A4a; the skin, A9.3a)
    o[FB_RECON] = r->gpu_composite_ms + r->gpu_upscale_ms; // (TSR or DLSS / FSR / XeSS, then the composite and the UI)
    o[FB_CPU_RENDER] = r->cpu_ms;
    o[FB_CPU_BLUEPRINTS] = bp_stats()->ms_last_frame;
    o[FB_CPU_PHYSICS] = physics_stats()->ms + vehicle_stats()->ms + cloth_stats()->cpu_ms;
    o[FB_CPU_ANIMATION] = anim_stats()->ms;
    o[FB_CPU_AI] = ai_stats()->ms + crowd_stats()->sim_ms + traffic_stats()->ms; // (Q27.2: the traffic too)
    o[FB_CPU_PARTICLES] = particles_stats()->cpu_ms;
    o[FB_CPU_GAMEPLAY] = health_stats()->update_ms + inv_stats()->tick_ms + interaction_stats()->tick_ms + dlg_stats()->tick_ms +
                         quest_stats()->tick_ms + rules_stats()->tick_ms + abil_stats()->tick_ms + hud_stats()->tick_ms;
}

static struct {
    double sum[FB_SLICES], peak[FB_SLICES], gpu, cpu;
    uint32_t frames;
    FbReport rep;
} F;

void fb_begin(void) {
    memset(&F, 0, sizeof F);
    render_set_detail(true); // (the main pass's parts: a few timestamps more)
}

void fb_add_frame(double cpu_ms, double gpu_ms) {
    double s[FB_SLICES];
    fb_sample(s);
    for (int k = 0; k < FB_SLICES; k++) {
        F.sum[k] += s[k];
        if (s[k] > F.peak[k]) F.peak[k] = s[k];
    }
    F.gpu += gpu_ms, F.cpu += cpu_ms;
    F.frames++;
}

const FbReport* fb_result(double budget_ms) {
    FbReport* r = &F.rep;
    memset(r, 0, sizeof *r);
    r->frames = F.frames, r->budget_ms = budget_ms;
    double n = F.frames ? (double)F.frames : 1.0;
    for (int k = 0; k < FB_SLICES; k++) r->mean[k] = F.sum[k] / n, r->peak[k] = F.peak[k], r->goal[k] = fb_goal(k, budget_ms);
    r->gpu_mean = F.gpu / n, r->cpu_mean = F.cpu / n;
    return r;
}

bool fb_check(double budget_ms, const char* scene, bool strict) {
    const FbReport* r = fb_result(budget_ms);
    bool ok = r->frames > 0;
    printf("[RealEngine] Budget %s: %.1f ms%s (%u frames): GPU %.3f ms, CPU %.3f ms\n", scene && scene[0] ? scene : "play", budget_ms,
           strict ? ", each slice" : "", r->frames, r->gpu_mean, r->cpu_mean);
    for (int k = 0; k < FB_SLICES; k++)
        printf("[RealEngine] Budget   %-26s %s %6.3f ms (worst %6.3f) of %.2f%s\n", g_names[k], k < FB_GPU_COUNT ? "GPU" : "CPU", r->mean[k],
               r->peak[k], r->goal[k], r->mean[k] > r->goal[k] ? "  OVER" : "");
    if (r->gpu_mean > budget_ms) printf("[RealEngine] FAIL: the frame's GPU %.3f ms is past the budget of %.1f ms\n", r->gpu_mean, budget_ms), ok = false;
    if (r->cpu_mean > budget_ms) printf("[RealEngine] FAIL: the frame's CPU %.3f ms is past the budget of %.1f ms\n", r->cpu_mean, budget_ms), ok = false;
    for (int k = 0; strict && k < FB_SLICES; k++)
        if (r->mean[k] > r->goal[k]) {
            printf("[RealEngine] FAIL: the slice '%s' takes %.3f ms, past its %.2f ms\n", g_names[k], r->mean[k], r->goal[k]);
            ok = false;
        }
    if (!r->frames) printf("[RealEngine] FAIL: the budget measured no frame\n");
    return ok;
}

// --- The Debug Viewer's list: each slice with its time and its goal (for 10 ms) ---
static void collect(DvList* l, const DvContext* c) {
    (void)c;
    double s[FB_SLICES];
    fb_sample(s);
    double gpu = 0.0;
    for (int k = 0; k < FB_SLICES; k++) {
        char info[48];
        snprintf(info, sizeof info, "%s, goal %.2f ms%s", k < FB_GPU_COUNT ? "GPU" : "CPU", fb_goal(k, 10.0), s[k] > fb_goal(k, 10.0) ? " (over)" : "");
        dv_add(l, g_names[k], info, (float)s[k], ENTITY_NONE);
        if (k < FB_GPU_COUNT) gpu += s[k];
    }
    l->gpu = true, l->ms = gpu;
    snprintf(l->note, sizeof l->note, "The 10 ms of the Epic preset at 1440p (R11): GPU %.2f of 10, each slice against its goal", gpu);
}
void fb_register_debug(void) { dv_register("Frame Budget", collect); }
