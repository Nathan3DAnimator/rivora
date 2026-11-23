// The packaged game (Build > Package Game in the editor): the level, its baked lighting and the game's systems, in a
// window of their own with nothing of the editor (no UI, no actor icons, no selection, no grid). Reads game.ini next
// to the executable (written by the package):
//   title=...             the window's title
//   level=Content/X.rlevel the start level (relative to the executable)
//   scalability=0..4      Low, Medium, High, Epic, Cinematic (Engine Scalability: MSAA, GI; Cinematic: the Quality mode)
//   path_tracing=0|1      the Quality mode's path tracer as the game's view (denoised; Lit without ray tracing)
//   fullscreen=1          borderless, the monitor's size; 0: a window
//   vsync=0
//   vr=1                  (Q25) Start in VR: the game on the headset (OpenXR; none connected: the monitor); --vr / --vr-sim too
//   vr_refresh=90         72, 90, 120 (0: the headset's own); vr_tracking=0 floor | 1 seated; vr_quality=0..4 (4: Quest's preset)
//   vr_foveation=0..3     fixed foveated rendering; vr_foveation_dynamic=1, vr_spacewarp=1, vr_render_scale=1.0
// Controls (until the Blueprints bring the game's own): the level's auto-activated camera, else a spectator at the
// Player Start (mouse to look, WASD and Q/E to move, Shift faster, the wheel sets the speed). Esc quits, F11 toggles
// fullscreen. Command line: --level <path>, --windowed, --bench <s> [--min-fps <n>] (prints the measure and exits),
// --validation, --scalability <0-3>.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <math.h>
#include <stdio.h>
#include "rpak.h"
#include "world_partition.h"
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <vulkan/vulkan.h>
#include <cimgui_impl.h>

#include "blueprint.h"
#include "physics.h"
#include "spline.h"
#include "anim.h"
#include "particles.h"
#include "decal.h"
#include "ikrig.h"
#include "vehicle.h"
#include "procgen.h"
#include "pcg.h"
#include "audio.h"
#include "audio_world.h"
#include "script.h"
#include "ai.h"
#include "dialogue.h"
#include "quest.h"
#include "navmesh.h"
#include "health.h"
#include "save_progress.h"
#include "interaction.h"
#include "hud.h"
#include "input.h"
#include "project.h"
#include "crashlog.h"
#include "debug_viewer.h"
#include "trace.h"
#include "plugins.h"
#include "material.h"
#include "gi.h"
#include "sequencer.h"
#include "gi_bake.h"
#include "level.h"
#include "level_preload.h" // (2026-11-23)
#include "mem.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "scene_query.h" // (2026-11-19) traces
#include "debug_draw.h"
#include "post.h"
#include "systems.h"
#include "vkctx.h"
#include "brand.h"
#include "traffic.h"
#include "crowd.h"
#include "ui_widget.h"
#include "landscape.h"
#include "mobile.h"
#include "foliage.h"
#include "proto.h"
#include "i18n.h"
#include "game_settings.h"
#include "upscale.h"
#include "online.h"
#include "xr.h"
#include "vr_pawn.h"
#include "xr_mr.h" // (Q25b) mixed reality
#include "ar.h"
#include "ar_arcore.h"
uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)
#include "render_gpu.h"
#include "sublevels.h"
#include "theme.h"
#include "runtime_lighting.h" // (Q16: Build Lighting in the game)
#include "cvars.h"

typedef struct GameConfig {
    char title[128];
    char level[1024];
    int scalability;
    bool fullscreen, vsync;
    int width, height; // the window (not fullscreen); 0: 3/4 of the monitor
    bool shipping;     // Package Game's Shipping: no Debug Viewer (F10)
    bool hdr;          // (A9.4) HDR10 output when the display has it (Project Settings > HDR Output; REALENGINE_HDR=1)
    float hdr_paper, hdr_peak; // its paper white and peak (nits)
    bool path_tracing; // the Quality mode's path tracer as the view (sugestão #45; Lit without ray tracing)
    bool ar;           // ARCore's session (android/package_apk.ps1 -AR writes ar=1: the APK carries ARCore)
    bool ar_occlusion; // the real things hide the virtual ones behind them (ARCore's depth; ar_occlusion=0 turns it off)
    XrConfig vr;       // (Q25) Start in VR and the headset's frame (xr.h; Project Settings > VR)
} GameConfig;

// (Q25) VR: the menu panel's pixels (the UI laid out for it; xr.c's quad swapchain), the VR Quality presets
#define VR_PANEL_W 1280
#define VR_PANEL_H 800
// Low..Epic: the flat game's presets; Quest: made for the Quest 3 / 3S's Adreno 740 at 90-120 Hz, both eyes: the indirect
// light from the lightmaps and the baked volume (RealGI's bake: nothing traced, no surface cache), MSAA 4x (no screen-space
// history: TSR, SSR, GTAO off), the sun's virtual shadows small, the LODs and the impostors early
static void vr_quality_apply(int q, int* msaa, GiSettings* gi) {
    if (q >= 0 && q < VR_QUALITY_QUEST) {
        render_scalability(q, msaa, gi);
        gi->ssr = false; // (a screen-space reflection differs between the eyes: it shimmers in stereo)
        return;
    }
    render_scalability(1, msaa, gi);
    gi->rt_shadows = gi->reflections = false, gi->surface_cache = false, gi->restir_gi = false;
    gi->probes = true; // (the baked volume sampled; with lightmaps the static meshes read theirs)
    gi->gtao = gi->ssr = gi->contact_shadows = false, gi->sss_samples = 0;
    gi->screen_clouds = gi->clouds_half = false, gi->volumetric_fog = false;
    gi->vsm_levels = 5, gi->vsm_res = 1024, gi->vsm_taps = 4, gi->vsm_full_budget = 1;
    gi->lsm_lights = 2, gi->lsm_res = 512;
    gi->lod_error_px = 3.0f, gi->tessellation = false, gi->tess_max = 1;
    gi->far_res = 512, gi->impostor_res = 512, gi->impostor_px = 40.0f;
    gi->tsr_scale = 0.0f;
}

static void trim(char* s) {
    char* e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

// game.ini next to the executable; what it lacks keeps the defaults
static void load_config(GameConfig* c, const char* exe_dir) {
    memset(c, 0, sizeof *c); // (what game.ini lacks: zero, the defaults below)
    snprintf(c->title, sizeof c->title, "Real Engine Game");
    c->ar_occlusion = true;
    xr_config_defaults(&c->vr);
    snprintf(c->level, sizeof c->level, "%s/Content/Game.rlevel", exe_dir);
    c->scalability = 2;
    c->fullscreen = true;
    c->vsync = false;
    char path[1100], line[1200];
    snprintf(path, sizeof path, "%s/game.ini", exe_dir);
    FILE* f = re_fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "title")) snprintf(c->title, sizeof c->title, "%s", v);
        else if (!strcmp(k, "level")) snprintf(c->level, sizeof c->level, "%s/%s", exe_dir, v);
        else if (!strcmp(k, "scalability")) c->scalability = atoi(v);
        else if (!strcmp(k, "fullscreen")) c->fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "vsync")) c->vsync = atoi(v) != 0;
        else if (!strcmp(k, "width")) c->width = atoi(v);
        else if (!strcmp(k, "height")) c->height = atoi(v);
        else if (!strcmp(k, "shipping")) c->shipping = atoi(v) != 0;
        else if (!strcmp(k, "hdr")) c->hdr = atoi(v) != 0;
        else if (!strcmp(k, "hdr_paper")) c->hdr_paper = (float)atof(v);
        else if (!strcmp(k, "hdr_peak")) c->hdr_peak = (float)atof(v);
        else if (!strcmp(k, "path_tracing")) c->path_tracing = atoi(v) != 0;
        else if (!strcmp(k, "ar")) c->ar = atoi(v) != 0;
        else if (!strcmp(k, "ar_occlusion")) c->ar_occlusion = atoi(v) != 0;
        else if (!strncmp(k, "vr", 2)) xr_config_key(&c->vr, k, v); // (Q25: vr, vr_refresh, vr_tracking, vr_quality...)
    }
    fclose(f);
}

static struct {
    GLFWwindow* window;
    bool fullscreen, exclusive; // exclusive: the monitor's own mode (the options' Fullscreen)
    int win_x, win_y, win_w, win_h; // the window before fullscreen
    double wheel;
} G;

static void ui_check(VkResult r) { (void)r; }

// Dear ImGui for the Debug Viewer (F10), set up the first time it opens: a game that never opens it pays nothing
// (2026-11-23) Open Level of a level still preloading: the loading screen's frames while it finishes
static VkCtx* g_preload_vk;
static void game_preload_pump(float progress) {
    if (g_preload_vk) brand_splash_frame(g_preload_vk, progress, TR("Loading the level..."));
}
static bool ui_init(VkCtx* vk) {
    static bool ready;
    if (ready) return true;
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->IniFilename = NULL;
#ifdef _WIN32
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
#else
    const char* font = "/system/fonts/Roboto-Regular.ttf";
#endif
    if (re_file_stat(font, NULL, NULL, NULL)) ImFontAtlas_AddFontFromFileTTF(io->Fonts, font, 16.0f, NULL, NULL);
    else ImFontAtlas_AddFontDefault(io->Fonts, NULL);
    theme_apply_imgui(1.0f); // (the tokens: the same look as the editor's)
    ImGui_ImplGlfw_InitForVulkan(G.window, true); // (chains the game's scroll callback)
    static VkFormat color_format;
    color_format = vk->hdr || vk->want_hdr ? render_hdr_ui_format() : vk->color_format; // (A9.4 HDR10: the UI's own target)
    ImGui_ImplVulkan_InitInfo init;
    memset(&init, 0, sizeof init);
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = vk->instance;
    init.PhysicalDevice = vk->gpu;
    init.Device = vk->device;
    init.QueueFamily = vk->queue_family;
    init.Queue = vk->queue;
    init.PipelineCache = vk->pipeline_cache;
    init.DescriptorPoolSize = 16;
    init.MinImageCount = vk->min_image_count >= 2 ? vk->min_image_count : 2;
    init.ImageCount = vk->image_count >= init.MinImageCount ? vk->image_count : init.MinImageCount;
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init.CheckVkResultFn = ui_check;
    vkDeviceWaitIdle(vk->device);
    ready = ImGui_ImplVulkan_Init(&init);
    return ready;
}

static void on_scroll(GLFWwindow* w, double dx, double dy) {
    (void)w;
    (void)dx;
    G.wheel += dy;
}

// Borderless fullscreen on the window's monitor (no mode change: alt-tab is instant), or back to the window
static void set_fullscreen(bool on) {
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = m ? glfwGetVideoMode(m) : NULL;
    if (!mode) return;
    if (on && !G.fullscreen) {
        glfwGetWindowPos(G.window, &G.win_x, &G.win_y);
        glfwGetWindowSize(G.window, &G.win_w, &G.win_h);
        glfwSetWindowAttrib(G.window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(G.window, NULL, 0, 0, mode->width, mode->height, GLFW_DONT_CARE);
    } else if (!on && G.fullscreen) {
        glfwSetWindowAttrib(G.window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(G.window, NULL, G.win_x, G.win_y, G.win_w, G.win_h, GLFW_DONT_CARE);
    }
    G.fullscreen = on;
}

// The options' window modes (game_settings.h): Fullscreen (the monitor in the chosen resolution), Windowed Fullscreen
// (borderless, the monitor's size: the default), Windowed (the chosen size, centered)
static void apply_window(int mode, int w, int h) {
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    const GLFWvidmode* md = m ? glfwGetVideoMode(m) : NULL;
    if (!md) return;
    if (w < 320 || h < 200) w = md->width, h = md->height;
    if (mode == GUS_WINDOW_FULLSCREEN) {
        if (!G.fullscreen) glfwGetWindowPos(G.window, &G.win_x, &G.win_y), glfwGetWindowSize(G.window, &G.win_w, &G.win_h);
        glfwSetWindowMonitor(G.window, m, 0, 0, w, h, GLFW_DONT_CARE);
        G.fullscreen = G.exclusive = true;
        return;
    }
    if (G.exclusive) { // (out of the monitor's mode first)
        glfwSetWindowMonitor(G.window, NULL, G.win_x, G.win_y, G.win_w > 0 ? G.win_w : w, G.win_h > 0 ? G.win_h : h, GLFW_DONT_CARE);
        G.exclusive = false, G.fullscreen = false;
        glfwSetWindowAttrib(G.window, GLFW_DECORATED, GLFW_TRUE);
    }
    if (mode == GUS_WINDOW_WINDOWED_FULLSCREEN) {
        set_fullscreen(true);
        return;
    }
    set_fullscreen(false);
    if (w > md->width) w = md->width;
    if (h > md->height) h = md->height;
    glfwSetWindowSize(G.window, w, h);
    glfwSetWindowPos(G.window, (md->width - w) / 2, (md->height - h) / 2);
}

// The options applied (game_settings.h): the preset (only when it changed: a phone's, VR's adjustments stay), the
// resolution scale, the frame limit; display: the window and VSync
static struct {
    RenderView* view;
    VkCtx* vk;
    int scalability, res_scale, frame_limit;
    bool windowed_test; // (REALENGINE_BACKGROUND: the window stays where the tests put it)
    int upscale_mode;   // upscale.h UPM_*
    bool dyn_res;       // the dynamic resolution (upscale_dynamic each frame, under the share the mode gives)
    float preset_scale, top_scale; // the preset's render share; the mode's times the resolution scale (the most)
    UpsDynamic dyn;
    int upscaler;       // (4.4a) the options' Upscaler as last applied (-1: the project's; a change only: --upscaler stays)
} GV = { NULL, NULL, -1, 100, 0, false, 0, false, 0.75f, 0.75f, { 0 } };
static void game_apply_video(const GameUserSettings* s, bool display) {
    if (GV.view && (s->scalability != GV.scalability || s->res_scale != GV.res_scale || s->upscale_mode != GV.upscale_mode ||
                    s->dyn_res != GV.dyn_res)) {
        if (s->scalability != GV.scalability) render_scalability(s->scalability, &GV.view->msaa, &GV.view->gi), GV.preset_scale = GV.view->gi.tsr_scale;
        // the render's share: the upscaling mode's (Preset: the preset's), times the resolution scale (upscale.h)
        GV.top_scale = upscale_mode_scale(s->upscale_mode, GV.preset_scale) * (float)s->res_scale / 100.0f;
        if (GV.preset_scale > 0.0f) GV.view->gi.tsr_scale = GV.top_scale; // (0: TSR off by the preset; stays off)
        GV.scalability = s->scalability, GV.res_scale = s->res_scale, GV.upscale_mode = s->upscale_mode, GV.dyn_res = s->dyn_res;
        memset(&GV.dyn, 0, sizeof GV.dyn);
    }
    GV.frame_limit = s->frame_limit;
    if (s->upscaler != GV.upscaler) GV.upscaler = s->upscaler, render_set_upscaler_override(s->upscaler); // (4.4a: the options' Upscaler)
    if (display && !GV.windowed_test) apply_window(s->window_mode, s->res_w, s->res_h);
    if (display && GV.vk && GV.vk->vsync != s->vsync) GV.vk->vsync = s->vsync, GV.vk->swapchain_dirty = true;
}

static bool key(int k) { return glfwGetKey(G.window, k) == GLFW_PRESS; }

// The spectator: mouse look, WASD forward/sideways, Q/E world down/up (Unreal's), Shift faster
static void fly(LevelCamera* cam, double dt, double* last_x, double* last_y) {
    double cx, cy;
    glfwGetCursorPos(G.window, &cx, &cy);
    cam->yaw += (float)(cx - *last_x) * 0.15f;
    cam->pitch -= (float)(cy - *last_y) * 0.15f;
    cam->pitch = fminf(fmaxf(cam->pitch, -89.0f), 89.0f);
    cam->yaw = fmodf(cam->yaw, 360.0f);
    *last_x = cx;
    *last_y = cy;
    if (G.wheel != 0.0) cam->speed = fminf(fmaxf(cam->speed * powf(1.25f, (float)G.wheel), 10.0f), 20000.0f);
    G.wheel = 0.0;
    float f[3], r[3], u[3];
    level_camera_basis(cam, f, r, u);
    float mx = (float)(key(GLFW_KEY_D) - key(GLFW_KEY_A));
    float mz = (float)(key(GLFW_KEY_W) - key(GLFW_KEY_S));
    float my = (float)(key(GLFW_KEY_E) - key(GLFW_KEY_Q));
    float step = cam->speed * (float)dt * (key(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
    for (int k = 0; k < 3; k++) cam->pos[k] += (f[k] * mz + r[k] * mx) * step;
    cam->pos[2] += my * step;
}

// The possessed Blueprint pawn's input: W/S A/D and the mouse as Unreal's axes, every key as a Key Event
static void game_input(World* w, bool look_on, double* last_x, double* last_y) {
    static uint8_t s_down[512];
    float move[2] = { 0, 0 }, look[2] = { 0, 0 };
    if (bp_player(w) != ENTITY_NONE) {
        const ProjectSettings* ps = project(); // Project Settings > Input
        const GameUserSettings* us = gus_applied(); // (the player's options: the movement keys, sensitivity, Invert Y)
        move[0] = (float)(key(gus_remapped_code("IA_Move", ps->key_forward)) - key(gus_remapped_code("IA_Move", ps->key_back)));
        move[1] = (float)(key(gus_remapped_code("IA_Move", ps->key_right)) - key(gus_remapped_code("IA_Move", ps->key_left)));
        double cx, cy;
        glfwGetCursorPos(G.window, &cx, &cy);
        float sens = ps->mouse_sensitivity * (us->sensitivity > 0.0f ? us->sensitivity : 1.0f);
        if (look_on)
            look[0] = (float)(cx - *last_x) * sens,
            look[1] = (float)(*last_y - cy) * sens * ((ps->invert_mouse_y != us->invert_y) ? -1.0f : 1.0f);
        *last_x = cx, *last_y = cy;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST && k < 500; k++) {
            uint8_t d = key(k);
            if (d != s_down[k]) bp_key_event(w, k, d);
            s_down[k] = d;
        }
        for (int b = 0; b < 2; b++) {
#ifdef __ANDROID__
            break; // (a touch screen's fingers: the touch widgets, not the mouse buttons)
#endif
            uint8_t d = glfwGetMouseButton(G.window, b) == GLFW_PRESS;
            if (d != s_down[500 + b]) bp_key_event(w, 500 + b, d);
            s_down[500 + b] = d;
        }
    }
    bp_set_input(move, look);
    { // Event Touch, Get Mouse Position
        double mx, my;
        glfwGetCursorPos(G.window, &mx, &my);
        float m[2] = { (float)mx, (float)my };
#ifdef __ANDROID__
        bp_touch_update(w, NULL, false);
#else
        bp_touch_update(w, m, glfwGetMouseButton(G.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
#endif
    }
}

static void fail(const char* what) {
#ifdef _WIN32
    MessageBoxA(NULL, what, "Real Engine", MB_ICONERROR);
#else
    fprintf(stderr, "[RealEngine] erro: %s\n", what);
#endif
}

int main(int argc, char** argv) {
    theme_init(); // (the editor's default theme: the Debug Viewer and the drawn widgets' helpers read its tokens)
    double launched = re_time_seconds(); // the start's measure: launch to the first frame on screen
    CrashConfig crash = { NULL, true, true };
    crash_install(&crash);
    char exe_dir[1024];
    if (!re_exe_dir(exe_dir, sizeof exe_dir)) snprintf(exe_dir, sizeof exe_dir, ".");
    { // (B15a) a Shipping package: its Content in one file, read as if it were the folder
        char pk[1100], croot[1100];
        snprintf(pk, sizeof pk, "%s/Content.rpak", exe_dir), snprintf(croot, sizeof croot, "%s/Content", exe_dir);
        if (re_file_stat(pk, NULL, NULL, NULL)) rpak_mount(pk, croot);
    }
    GameConfig cfg;
    load_config(&cfg, exe_dir);
    // (Q25) --project <file>: the game straight from a project's folder (the editor's VR Preview): its settings and Content
    const char* proj_arg = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--project")) proj_arg = argv[i + 1];
    char content_dir[1100];
    snprintf(content_dir, sizeof content_dir, "%s/Content", exe_dir);
    if (proj_arg && project_load(proj_arg)) {
        char root[1024];
        project_root(root, sizeof root);
        snprintf(content_dir, sizeof content_dir, "%s/Content", root);
        const ProjectVr* pv = &project()->vr; // (no game.ini: the project's VR settings)
        cfg.vr.refresh_hz = pv->refresh_hz, cfg.vr.tracking = pv->tracking, cfg.vr.quality = pv->quality, cfg.vr.foveation = pv->foveation;
        cfg.vr.foveation_dynamic = pv->foveation_dynamic, cfg.vr.spacewarp = pv->spacewarp;
        cfg.vr.render_scale = pv->render_scale > 0.0f ? pv->render_scale : 1.0f;
    } else {
        char pp[1100];
        snprintf(pp, sizeof pp, "%s/Game.rproject", exe_dir);
        project_load(pp); // the project's settings (its gravity, input...): packaged next to the game
    }
    bool validation = false;
    const char* shot_path = NULL;
    const char* trace_path = NULL; // (B14b) --trace FILE: every frame recorded
    const char* cvar_args[32]; // (Q16) --cvar name=value: a console variable set once the view exists
    int ncvar_args = 0;
    const char* ar_image = NULL;
    bool ar_mode = false, ar_sim = false;
    bool vr_script = false;
    int vr_mode = getenv("REALENGINE_VR") ? atoi(getenv("REALENGINE_VR")) : cfg.vr.start ? 1 : 0; // (Q25: game.ini's vr=1, Start in VR)
    double bench_s = 0.0, min_fps = 0.0, max_start_ms = 0.0, max_bake_frame_ms = 0.0;
    bool net_server = false;     // --server: a dedicated server
    char net_connect_to[128] = ""; // --connect <address>: joins at once
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--level") && i + 1 < argc) snprintf(cfg.level, sizeof cfg.level, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--windowed")) cfg.fullscreen = false;
        else if (!strcmp(argv[i], "--validation")) validation = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i]; // the window near the measure's end into a PNG
        else if (!strcmp(argv[i], "--ar-image") && i + 1 < argc) ar_image = argv[++i]; // AR's light matching from a picture (the PC's preview)
        else if (!strcmp(argv[i], "--ar")) ar_mode = true; // AR on a phone: its rotation sensor turns the camera (3-DoF)
        else if (!strcmp(argv[i], "--ar-sim")) ar_sim = true; // ARCore simulated (with --ar-image): the picture behind, a floor receiving the shadows
        else if (!strcmp(argv[i], "--vr")) vr_mode = 1;     // VR on the headset (OpenXR); none: the monitor
        else if (!strcmp(argv[i], "--vr-sim")) vr_mode = 2; // VR without a headset: both eyes side by side (the tests)
        else if (!strcmp(argv[i], "--project") && i + 1 < argc) i++; // (Q25: read above)
        else if (!strcmp(argv[i], "--vr-script")) vr_script = true; // (Q25) the played VR test (vr_pawn_script)
        else if (!strcmp(argv[i], "--vr-ini") && i + 1 < argc) { // (Q25) a game.ini VR key for this run: --vr-ini vr_quality=4
            char kv[96];
            snprintf(kv, sizeof kv, "%s", argv[++i]);
            char* eq = strchr(kv, '=');
            if (eq) *eq = 0, xr_config_key(&cfg.vr, kv, eq + 1);
        }
        else if (!strcmp(argv[i], "--scalability") && i + 1 < argc) cfg.scalability = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--path-tracing")) cfg.path_tracing = true; // (game.ini's path_tracing=1: the Quality mode's test)
        else if (!strcmp(argv[i], "--server")) net_server = true; // a dedicated server (no local player; the port: Project Settings)
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc) snprintf(net_connect_to, sizeof net_connect_to, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_s = atof(argv[++i]), crash_set_dialog(false);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--cvar") && i + 1 < argc && ncvar_args < 32) cvar_args[ncvar_args++] = argv[++i];
        else if (!strcmp(argv[i], "--min-fps") && i + 1 < argc) min_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-start-ms") && i + 1 < argc) max_start_ms = atof(argv[++i]); // the first-run test
        else if (!strcmp(argv[i], "--max-bake-frame-ms") && i + 1 < argc) max_bake_frame_ms = atof(argv[++i]); // (Q16) a hitch while the game bakes
    }

    if (!glfwInit()) return 1;
    { // the player's options (GameUserSettings.ini: %LOCALAPPDATA%/<game>/Saved/Config): over game.ini's; a measure (--bench) reads
      // none (the same frame every run) unless REALENGINE_SETTINGS_FILE names one
        char sp[1200];
        gus_default_path(project()->game_name[0] ? project()->game_name : cfg.title, true, sp, sizeof sp);
        gus_set_path(sp);
        GameUserSettings* b = gus_base(); // (Set to Defaults: what the package chose)
        b->scalability = cfg.scalability, b->vsync = cfg.vsync, b->res_w = cfg.width, b->res_h = cfg.height;
        b->window_mode = cfg.fullscreen ? GUS_WINDOW_WINDOWED_FULLSCREEN : GUS_WINDOW_WINDOWED;
        gus_reset();
        if ((bench_s <= 0.0 || getenv("REALENGINE_SETTINGS_FILE")) && gus_load()) {
            const GameUserSettings* s = gus();
            cfg.scalability = s->scalability, cfg.vsync = s->vsync;
            cfg.fullscreen = s->window_mode != GUS_WINDOW_WINDOWED;
            if (s->res_w >= 320 && s->res_h >= 200) cfg.width = s->res_w, cfg.height = s->res_h;
            printf("[RealEngine] Game User Settings: %s (preset %d, VSync %d, window mode %d)\n", gus_path(), s->scalability, s->vsync, s->window_mode);
        }
        gus_set_host(&(GusHost){ game_apply_video, true });
        i18n_set_language(gus_language()); // (the loading screen already in the player's language)
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : NULL;
    int ww = mode ? mode->width * 3 / 4 : 1600, wh = mode ? mode->height * 3 / 4 : 900;
    if (cfg.width >= 320 && cfg.height >= 200) // (Package Game's resolution, kept inside the monitor)
        ww = mode && cfg.width > mode->width ? mode->width : cfg.width, wh = mode && cfg.height > mode->height ? mode->height : cfg.height;
    // REALENGINE_BACKGROUND=1 (the test suites): windowed, left of every monitor, never focused (main.c's same mode)
    const char* bgenv = getenv("REALENGINE_BACKGROUND");
    bool background = bgenv && bgenv[0] == '1';
    // REALENGINE_MONITOR=N (the visible suites: the user's 2nd monitor): the background mode's window, shown on that display
    int mon_x = 0, mon_y = 0;
    bool on_monitor = !background && re_test_monitor(&mon_x, &mon_y, NULL, NULL);
    if (background || on_monitor) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE), glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE), glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        if (mode) ww = mode->width, wh = mode->height; // (the fullscreen size the game's measures were taken at)
    }
    G.window = glfwCreateWindow(ww, wh, cfg.title, NULL, NULL);
    if (!G.window) return 1;
    if (background) {
#ifdef _WIN32
        glfwSetWindowPos(G.window, GetSystemMetrics(SM_XVIRTUALSCREEN) - ww - 200, GetSystemMetrics(SM_YVIRTUALSCREEN) + 40);
#endif
        glfwShowWindow(G.window);
        GV.windowed_test = true;
    } else if (on_monitor) {
        glfwSetWindowPos(G.window, mon_x + 8, mon_y + 31);
        glfwShowWindow(G.window);
        GV.windowed_test = true;
    } else if (cfg.fullscreen) {
        if (gus()->window_mode == GUS_WINDOW_FULLSCREEN) apply_window(GUS_WINDOW_FULLSCREEN, cfg.width, cfg.height);
        else set_fullscreen(true);
    }
    glfwSetScrollCallback(G.window, on_scroll);

    static VkCtx vk; // big (swapchain arrays): not on the stack
    g_preload_vk = &vk;
#ifdef __ANDROID__
    cfg.vsync = true; // (a phone: the display's pace, never a frame drawn to be dropped; mobile.c paces it)
#endif
    vk.vsync = cfg.vsync;
    bool vr = false;
    xr_set_config(&cfg.vr); // (Q25: the refresh, the play space, the render scale, the foveation, before the runtime starts)
    if (vr_mode && xr_init(vr_mode == 2)) { // (before Vulkan: the runtime's extensions and its GPU)
        vr = true;
        render_want_foveation(cfg.vr.foveation > 0); // (the rate image's pipelines)
        if (xr_info()->headset) g_vk_pick_gpu = xr_vulkan_gpu;
        vk.vsync = false; // (the headset paces the frames: xrWaitFrame)
    }
    if (!vkctx_init(&vk, G.window, validation)) {
        fail("This game needs a GPU with Vulkan 1.3. Update the graphics driver.");
        return 1;
    }
    // the engine's intro (Project Settings: Show the Engine Intro) and the loading screen (the project's picture or
    // video, else the engine's) while the shaders and the level load; the tests go straight in
    bool presented = bench_s <= 0.0 && ui_init(&vk) && brand_init(&vk);
    progress_set_vk(&vk); // (a save's picture of the screen: save_progress.h)
    if (presented) {
        g_brand_intro_on = project()->show_intro;
        brand_run_intro(&vk, G.window);
        const char* sp = project()->splash;
        if (sp[0] && !strncmp(sp, "/Content/", 9)) {
            char path[1200];
            snprintf(path, sizeof path, "%s%s", content_dir, sp + 8);
            brand_set_splash(&vk, path);
        }
        brand_splash_begin();
        brand_splash_frame(&vk, 0.05f, TR("Loading the shaders..."));
    } else
        g_brand_splash_on = g_brand_intro_on = false;
    re_arena_init(&g_frame_arena, (size_t)256 << 20);
    render_set_wait_pump(glfwPollEvents); // (the pipelines compiled on threads: the window answers meanwhile)
    if (!render_init(&vk)) {
        fail("The game could not create its renderer (GPU memory or features).");
        return 1;
    }
    if (vr && !xr_start_session(vk.instance, vk.gpu, vk.device, vk.queue_family, vk.color_format)) vr = false;
    brand_splash_frame(&vk, 0.55f, TR("Loading the level..."));
    vkctx_pipeline_cache_save(&vk); // what the driver compiled for this start (a crash later keeps it)
    if (project()->pause_menu) gus_warm(); // (Q22: the Options' Upscaler choices asked while loading)

    static World world;
    if (!ecs_init(&world)) return 1;
    LevelCamera saved = { { 0, 0, 170 }, 0, 0, 90, 600 };
    if (!level_load(&world, cfg.level, &saved)) {
        char msg[1300];
        snprintf(msg, sizeof msg, "Could not open the level %s.", cfg.level);
        fail(msg);
        return 1;
    }
    brand_splash_frame(&vk, 0.75f, TR("Loading the lighting..."));
    char lp[1100];
    level_rlight_path(cfg.level, lp, sizeof lp);
    if (!gi_lightmaps_load(&vk, lp)) gi_lightmaps_clear(&vk); // no baked lighting: the dynamic GI
    gi_reset_probes();
    char content[1100];
    snprintf(content, sizeof content, "%s", content_dir);
    plugins_init(NULL, false); // (B14c) the plugins packaged with the game (Plugins/<Name>): their nodes before the Blueprints compile
    plugins_set_world(&world);
    bp_init(content); // the level's Blueprints (packaged next to it)
    input_init(content, false); // the Input Actions and Mapping Contexts (before the Blueprints are read: their events' value pins)
    ui_widgets_init(content); // the widgets' pictures
    landscape_init(content); // the landscapes' heights (their chunk meshes are packaged)
    procgen_init(content);
    pcg_init(content, false); // (Q15: the PCG Graphs, packaged)
    foliage_init(content);   // the painted plants (Content/Foliage, packaged)
    proto_init(content); // the prototyping shapes' grid material
    anim_init(content, false); // the skeletal meshes and animations
    particles_set_root(content);
    ikrig_set_root(content);
    vehicle_set_root(content);
    ai_init(content, false); // the Behavior Trees and Brains
    dlg_init(content); // the Dialogues (dialogue.h)
    quest_init(content); // the Quests (quest.h)
    bp_components_rebuild(&world, NULL); // their component trees as the classes are
    mat_set_root(content); // its materials
    seq_init(content); // its Level Sequences (Play Sequence)
    script_init(content); // its scripts: their classes (the level's actors of them)
    audio_init(content); // the mixer (its own thread) and the project's sounds
    project_apply_audio(); // (Project Settings > Audio)
    brand_splash_frame(&vk, 0.95f, TR("Starting the game..."));
    brand_splash_end(&vk);
    // (A9.4) HDR10 after the splash (drawn in SDR): the swapchain made again in A2B10G10R10 / ST.2084 when the display has it;
    // the UI then goes through its own target (the SDR swapchain's format: ImGui's pipeline stays)
    if ((cfg.hdr || getenv("REALENGINE_HDR")) && !vr) {
        if (vk.color_format != render_hdr_ui_format()) printf("[RealEngine] HDR10 output: off (the swapchain isn't B8G8R8A8)\n");
        else {
            vk.want_hdr = true;
            vkctx_recreate_swapchain(&vk);
            render_hdr_set(cfg.hdr_paper, cfg.hdr_peak);
            printf("[RealEngine] HDR10 output: %s\n", vk.hdr ? "on (ST.2084, BT.2020, 10 bits)" : vk.hdr_possible ? "unavailable on this display" : "unavailable (no VK_EXT_swapchain_colorspace)");
            if (!vk.hdr) vk.want_hdr = false;
        }
    }

    // The player's view: the auto-activated camera, else the spectator at the Player Start, else the saved view
    LevelCamera cam = saved;
    cam.speed = cam.speed > 0.0f ? cam.speed : 600.0f;
    if (cam.fov_deg <= 0.0f) cam.fov_deg = 90.0f;
    level_player_start(&world, &cam);
    glfwSetInputMode(G.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(G.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double last_x, last_y;
    glfwGetCursorPos(G.window, &last_x, &last_y);
    { // the level's streaming and its Level Blueprint
        const char* base = strrchr(cfg.level, '/');
        const char* base2 = strrchr(cfg.level, '\\');
        if (base2 > base) base = base2;
        char lname[128];
        snprintf(lname, sizeof lname, "%s", base ? base + 1 : cfg.level);
        char* dot = strrchr(lname, '.');
        if (dot) *dot = 0;
        level_stream_init(content, lname);
        bp_set_level_hooks(&(BpLevelHooks){ level_stream_request, level_stream_done, level_open_request, level_stream_persistent });
        level_bp_spawn(&world, cfg.level);
        static SublevelSet subs; // (Window > Levels: the sublevels Initially Loaded in, the Lighting Levels added)
        sublevels_read(cfg.level, &subs);
        sublevels_begin_game(&vk, &world, cfg.level, &subs);
        if (subs.n) bp_components_rebuild(&world, NULL);
    }
    wp_begin(&world, cfg.level, content, cam.pos); // (B1: what is out of range leaves before its BeginPlay)
    bp_begin_play(&world);
    physics_begin_play(&world); // the rigid bodies
    spline_begin_play(&world); // (Q14)
    vehicle_begin_play(&world); // the vehicles, their rails and waters
    procgen_begin_play(&world);
    pcg_begin_play(&world);
    anim_begin_play(&world); // the skeletal meshes' instances
    crowd_begin_play(&world); // the crowds (ai_begin_play built the grid, else they build it)
    traffic_begin_play(&world); // (Q27.2) the roads' cars
    ai_begin_play(&world); // the AI agents and the navigation grid
    project_begin_play(&world); // the Game Mode's classes and the Default Pawn (Project Settings)
    if (vr) vr_pawn_begin_play(&world); // (Q25) the possessed pawn in VR: the play space, locomotion, hands, laser
    if (vr) mr_begin_play(&world); // (Q25b) mixed reality: the passthrough, the room's actors
    audio_world_begin_play(&world); // the auto-activated Audio Components
    // online (Project Settings > Online; no network emulation in the game): Steam when set, a dedicated server or a join
    online_apply_project();
    { OnlineSettings os = *online_settings(); os.sim = (NetSim){ 0 }; online_configure(&os); }
    online_init();
    if (net_server) online_host_dedicated(&world, 0);
    else if (net_connect_to[0]) online_join_address(&world, net_connect_to);

    RenderView view;
    memset(&view, 0, sizeof view);
    view.view_mode = VIEW_LIT;
    view.selected = ENTITY_NONE;
    view.exposure_ev100 = 14.0f;
    view.exposure_comp = 1.0f; // the editor's default (EXPOSURE_COMP_DEFAULT)
    view.auto_min_ev100 = -4.0f;
    view.auto_max_ev100 = 18.0f;
    view.auto_exposure = true;
    view.aerial_scale = 1.0f;
    view.gi.contact_ao = true;
    render_scalability(cfg.scalability, &view.msaa, &view.gi);
    GV.view = &view, GV.vk = &vk, GV.scalability = cfg.scalability, GV.res_scale = 100;
    GV.preset_scale = GV.top_scale = view.gi.tsr_scale, GV.upscale_mode = UPM_PRESET, GV.dyn_res = false, GV.upscaler = -1;
    { // phones and tablets (or REALENGINE_MOBILE=1): the device's profile (mobile.h)
        const ProjectSettings* ps = project();
        MobileConfig mc = { ps->android_quality, ps->android_target_fps, ps->android_dynamic_res, ps->android_content_scale };
        if (!vr && mobile_init(&vk, &mc)) mobile_apply(&view.gi, &view.msaa); // (a headset paces itself: xrWaitFrame)
    }
    if (cfg.path_tracing && gi_available()) view.view_mode = VIEW_PATH_TRACING;
    gus_apply(false); // the options: the volumes, the keys, the resolution scale, the frame limit (the window: made so above)
    cvar_bind_view(&view.gi, &view.msaa); // (Q16) the console variables' r.* on this view (Set Console Variable)
    for (int i = 0; i < ncvar_args; i++) { // (--cvar r.GI.Probes=0)
        char line[160], out[200];
        snprintf(line, sizeof line, "%s", cvar_args[i]);
        char* eq = strchr(line, '=');
        if (eq) *eq = ' ';
        bool ok = cvar_exec(line, out, sizeof out);
        printf("[RealEngine] Console: %s%s\n", out, ok ? "" : " (FAIL)");
    }
    // VR (xr.h): each eye a frame of its own: what keeps a screen-space history is off (it would mix the eyes); MSAA
    static GpuImage sim_eye[2];
    static GpuImage spec_img; // (Q25) the VR Spectator Camera's picture (the eyes' size)
    uint32_t spec_w = 0, spec_h = 0;
    double vr_last_pair_gpu_ms = 0.0; // (Q25) the last VR frame's two eyes on the GPU
    // AR (ar.h): the camera's picture's light and color onto the level (on the PC a picture stands for the camera)
    static ArLight ar_light, ar_now;
    bool ar_on = false;
    if (ar_image) {
        uint32_t iw, ih;
        uint8_t* px = mat_load_image_rgba(ar_image, &iw, &ih);
        if (px) {
            ar_estimate(px, iw, ih, 70.0f, &ar_now), ar_light = ar_now, ar_on = true;
            arcore_set_occlusion(cfg.ar_occlusion && !getenv("REALENGINE_NO_AR_OCCLUSION"));
            if (ar_sim && arcore_simulate(px, iw, ih, 70.0f)) printf("[RealEngine] ARCore: simulated (%s behind the level, a floor at its origin)\n", ar_image);
            printf("[RealEngine] AR: %s: %.0f K, EV100 %.1f, main light %.2f (%.2f, %.2f, %.2f)\n", ar_image, ar_now.kelvin, ar_now.ev100,
                   ar_now.strength, ar_now.dir_view[0], ar_now.dir_view[1], ar_now.dir_view[2]);
            free(px);
        }
    }
    // ARCore (ar_arcore.h): Project Settings > Android > Augmented Reality, or the simulated session
    bool arcore_wanted = arcore_view()->simulated;
#ifdef __ANDROID__
    if (project()->android_ar || cfg.ar) {
        arcore_wanted = true;
        arcore_set_occlusion(cfg.ar_occlusion);
        if (!arcore_start()) printf("[RealEngine] ARCore: %s\n", arcore_state_name(arcore_view()->state));
    }
#endif
    ArcoreState ars = arcore_view()->state;
    // (AR shows the real floor: the level's ground planes hidden, an AR level has none, the real one takes the shadows;
    // kept where ARCore can't run: the game plays as a plain one)
    if (arcore_wanted && (ars == ARCORE_TRACKING || ars == ARCORE_SEARCHING || ars == ARCORE_NO_PERMISSION))
        for (uint32_t ai = 0; ai < world.archetype_count; ai++) {
            EcsArchetype* a = &world.archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
            CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t i = 0; i < a->count; i++)
                if (sm[i].mesh == SHAPE_PLANE) tr[i].flags |= TRANSFORM_HIDDEN_GAME;
        }
    if (vr) {
        vr_quality_apply(cfg.vr.quality, &view.msaa, &view.gi); // (Q25: Project Settings > VR > VR Quality; Quest: its own preset)
        view.gi.tsr_scale = 0.0f, view.gi.screen_clouds = false;
        { // (Q25) Project Settings > VR > VR MSAA (no screen history in VR: MSAA is its anti-aliasing)
            int m = project()->vr.msaa;
            view.msaa = m == 1 || m == 2 || m == 8 ? m : 4;
        }
        view.foveation = cfg.vr.foveation;
        printf("[RealEngine] VR: %s, %u x %u per eye, MSAA %dx\n", xr_info()->sim ? "simulated (both eyes in the window)" : xr_info()->system_name,
               xr_info()->eye_w, xr_info()->eye_h, view.msaa);
    }
    printf("[RealEngine] Game: %s, %u actors, %ux%u, scalability %d (MSAA %dx).\n", cfg.level, world.alive, vk.extent.width,
           vk.extent.height, cfg.scalability, view.msaa);
    fflush(stdout);

    int exit_code = 0;
    double prev = re_time_seconds(), bench_from = prev + 1.0, bench_until = bench_from + bench_s;
    uint64_t frames = 0;
    double cpu_sum = 0.0, gpu_sum = 0.0, worst = 0.0, vr_pair_sum = 0.0;
    bool f11 = false, f10 = false;
    double last_cpu_ms = 0.0, last_frame_ms = 0.0;
    uint64_t frames_seen = 0, frames_drawn = 0, frames_early = 0;
    if (trace_path && !trace_start(trace_path)) printf("[RealEngine] FAIL: the trace could not start\n"), exit_code = 1;
    int vr_fov_level = cfg.vr.foveation_dynamic && cfg.vr.foveation > 1 ? 1 : cfg.vr.foveation; // (Q25: the dynamic foveation's start)
    double vr_fov_calm = 0.0;
    const bool vr_share = !getenv("REALENGINE_VR_SHARE") || atoi(getenv("REALENGINE_VR_SHARE")) != 0;
    while (!glfwWindowShouldClose(G.window)) {
        double t0 = re_time_seconds();
        dd_frame_begin(t0), sq_frame_end(); // (2026-11-19) this frame's debug lines from empty; last frame's async traces joined, their counts
        glfwPollEvents();
        // (Q25) VR: the headset's frame first (xrWaitFrame paces the loop; the head's and the hands' poses of the frame shown)
        XrEye eyes[2];
        bool vr_frame = vr && xr_begin_frame(eyes);
        if (vr_script && vr_frame && vr_pawn_script(&world, (float)(t0 - prev > 0.1 ? 0.1 : t0 - prev))) vr_script = false; // (--vr-script)
        { // Esc: the pause menu (Project Settings > Pause Menu; its Options: back), else quits as before; a gamepad's Start / B
            static bool esc;
            if (key(GLFW_KEY_ESCAPE) != esc) {
                esc = !esc;
                if (esc && !project()->pause_menu) break;
                if (esc && plugins_gameplay_enabled()) pause_menu_key(&world, false); // (the Gameplay Framework's)
            }
            pause_menu_poll_gamepad(&world);
            if (game_quit_requested()) break; // (Quit Game)
        }
        if (key(GLFW_KEY_F11) != f11) {
            f11 = !f11;
            if (f11) set_fullscreen(!G.fullscreen);
        }
        if (!cfg.shipping && key(GLFW_KEY_F10) != f10) { // the Debug Viewer (not in a Shipping package): the cursor free while it is open (the look waits)
            f10 = !f10;
            if (f10 && ui_init(&vk)) {
                dv_set_open(!dv_open());
                glfwSetInputMode(G.window, GLFW_CURSOR, dv_open() ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                glfwGetCursorPos(G.window, &last_x, &last_y);
            }
        }
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(G.window, &fbw, &fbh);
        static bool s_away; // in the background (a phone's home button, minimized): nothing drawn, the audio device let go
        if (fbw == 0 || fbh == 0 || glfwGetWindowAttrib(G.window, GLFW_ICONIFIED)) {
            if (!s_away) s_away = true, audio_device_suspend(true);
            glfwWaitEvents();
            prev = re_time_seconds();
            continue;
        }
        if (s_away) s_away = false, audio_device_suspend(false);
        double dt = t0 - prev;
        prev = t0;
        re_arena_reset(&g_frame_arena);

        // the game's frame: its systems, then the player's view. Paused (Set Game Paused, the pause menu): the world waits;
        // the widgets, the UI sounds and the actors with Tick When Paused go on (bp_tick)
        // (Q16) a Build Lighting behind the loading screen: the world waits as paused, its Blueprints too, until it ends
        const bool loading = rtl_blocking();
        const bool frozen = game_paused() || loading;
        if (!frozen) TRACE("Systems (ECS)", systems_update(&world, (float)dt));
        // (the widgets' Set Show Mouse Cursor: the cursor out, the look stops)
        static bool s_ui_mouse;
        if (ui_mouse_shown() != s_ui_mouse && !dv_open()) {
            s_ui_mouse = ui_mouse_shown();
            glfwSetInputMode(G.window, GLFW_CURSOR, s_ui_mouse ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(G.window, &last_x, &last_y);
        }
#ifdef __ANDROID__
        const bool mouse_look = false; // (a touch screen turns the camera with the touch widgets: the look area, a joystick)
#else
        const bool mouse_look = true;
#endif
        game_input(&world, mouse_look && bench_s <= 0.0 && frames_seen > 0 && !dv_open() && !ui_mouse_shown(), &last_x, &last_y);
        TRACE("Online (receive)", online_pre_tick(&world, re_time_seconds())); // the network: what came, then its events
        bp_online_events(&world);
        TRACE("NavMesh", navmesh_update(&world, true, re_time_seconds())); // the NavMesh (its tiles built on a thread of its own)
        if (!frozen) TRACE("AI", ai_update(&world, (float)(dt > 0.1 ? 0.1 : dt))); // the AIs set their pawns' input before they move
        if (vr && !frozen) TRACE("VR Pawn", vr_pawn_tick(&world, (float)(dt > 0.1 ? 0.1 : dt))); // (Q25: the sticks before the movement)
        if (!loading) TRACE("Blueprints", bp_tick(&world, (float)(dt > 0.1 ? 0.1 : dt)));
        if (!frozen) {
            TRACE("Physics", physics_update(&world, (float)(dt > 0.1 ? 0.1 : dt))); // after the Blueprints: their impulses count this frame
            TRACE("Vehicles", vehicle_update(&world, (float)(dt > 0.1 ? 0.1 : dt))); // the vehicles (after the physics: the Blueprints' inputs count)
        }
        if (vr) TRACE("VR Pawn (hands)", vr_pawn_late_tick(&world, (float)(dt > 0.1 ? 0.1 : dt))); // (Q25: held actors, hands, laser)
        if (vr) TRACE("Mixed Reality", mr_tick(&world, (float)(dt > 0.1 ? 0.1 : dt))); // (Q25b: the passthrough, the room, its actors)
        TRACE("Online (send)", online_post_tick(&world, re_time_seconds())); // the others' actors as the server has them, what this machine sends
        if (!frozen) {
            anim_set_view_point(view.pos); // (the Look At nodes turn toward the camera)
            TRACE("Animation", anim_update(&world, (float)(dt > 0.1 ? 0.1 : dt), true)); // the poses (after the Characters moved)
            TRACE("Crowds", crowd_update(&world, (float)(dt > 0.1 ? 0.1 : dt), true, view.pos)); // the crowds think and move
            TRACE("Traffic", traffic_update(&world, (float)(dt > 0.1 ? 0.1 : dt), view.pos)); // (Q27.2) the roads' cars
        }
        TRACE("Plugins", plugins_tick(frozen ? 0.0f : (float)dt, true)); // (B14c) the plugins' ticks
        TRACE("Splines", spline_update(&world, frozen ? 0.0f : (float)dt, true)); // (Q14) the splines' pieces, the cables
        TRACE("Particles", particles_update(&world, frozen ? 0.0f : (float)dt, true)); // the particle systems (paused: they wait)
        decals_update(&world, frozen ? 0.0f : (float)dt, true); // the decals' life spans (M6)
        landscape_update(&world, &vk);
        {
            Entity pl = bp_player(&world);
            const CTransform* pt = pl ? ecs_get(&world, pl, COMP_TRANSFORM) : NULL;
            float focus[3];
            if (pt) memcpy(focus, pt->location, 12);
            else memcpy(focus, cam.pos, 12);
            procgen_update(&world, focus, true);
            TRACE("PCG", pcg_update(&world, focus, true)); // (Q15: the PCG components, generated and streamed)
        }
        foliage_update(&world);
        proto_update(&world);
        level_stream_update(&world, true);
        level_preload_update(); // (2026-11-23) the preloads' component trees, within the speed's budget
        wp_update(&world, view.pos, true); // (B1) the World Partition's cells around the camera
        if (level_open_pending()) { // Open Level: the other level in place of this one
            char go[700];
            snprintf(go, sizeof go, "%s", level_open_pending());
            level_open_clear();
            wp_end(&world);
            bp_end_play(&world);
            physics_end_play(&world);
            spline_end_play(&world); // (Q14)
            vehicle_end_play(&world);
            procgen_end_play(&world);
            pcg_end_play(&world);
            anim_end_play(&world);
            ai_end_play(&world);
            crowd_end_play(&world);
            traffic_end_play(&world);
            audio_world_end_play(&world);
            // (2026-11-23) a preloaded level swaps in at once (its loading screen only when it hasn't finished)
            double swap_ms = 0.0;
            bool pre = level_preload_state(go, NULL) != PRE_NONE;
            if (pre && presented) brand_splash_begin();
            bool took = pre && level_preload_take(go, &world, &saved, presented ? game_preload_pump : NULL, &swap_ms);
            if (pre && presented) brand_splash_end(&vk);
            if (took) printf("[RealEngine] Open Level: %s was preloaded, swapped in in %.2f ms\n", go, swap_ms);
            if (took || level_load(&world, go, &saved)) {
                bp_components_rebuild(&world, NULL);
                level_rlight_path(go, lp, sizeof lp);
                if (!gi_lightmaps_load(&vk, lp)) gi_lightmaps_clear(&vk);
                gi_reset_probes();
                level_stream_init(content, go);
                level_bp_spawn(&world, go);
                static SublevelSet subs2;
                sublevels_read(go, &subs2);
                sublevels_begin_game(&vk, &world, go, &subs2);
                if (subs2.n) bp_components_rebuild(&world, NULL);
                cam = saved;
                level_player_start(&world, &cam);
                wp_begin(&world, go, content, cam.pos);
                bp_begin_play(&world);
                physics_begin_play(&world);
                spline_begin_play(&world); // (Q14)
                vehicle_begin_play(&world); // the vehicles, their rails and waters
                procgen_begin_play(&world);
                pcg_begin_play(&world);
                anim_begin_play(&world);
                ai_begin_play(&world);
                crowd_begin_play(&world);
                traffic_begin_play(&world);
                project_begin_play(&world);
                if (vr) mr_end_play(&world), vr_pawn_end_play(&world), vr_pawn_begin_play(&world), mr_begin_play(&world); // (Q25: the new level's pawn; Q25b its room's actors)
                audio_world_begin_play(&world);
                online_level_loaded(&world, go); // (in a session: the clients follow)
            }
        }
        TRACE("Build Lighting (game)", rtl_frame(&vk, &world)); // (Q16) a bake a Blueprint asked for: started, its end noticed
        LevelCamera fixed;
        float fixed_roll = 0.0f, fixed_fov = 90.0f; // (Q12: a First Person Camera's lean and FOV, the shakes')
        bool fixed_cam = bp_player_camera_ex(&world, fixed.pos, &fixed.yaw, &fixed.pitch, &fixed_roll, &fixed_fov);
        if (fixed_cam) fixed.fov_deg = fixed_fov, frames_seen++;
        else fixed_cam = level_player_camera(&world, &fixed);
        // (no look on the first frame: disabling the cursor moves it; none in a measure: the same view every run)
        if (!fixed_cam && bench_s <= 0.0 && frames_seen++ > 0 && !dv_open() && !ui_mouse_shown()) fly(&cam, dt, &last_x, &last_y); // (a menu's cursor: the view stays)
        else glfwGetCursorPos(G.window, &last_x, &last_y);
        const LevelCamera* c = fixed_cam ? &fixed : &cam;
        level_camera_basis(c, view.fwd, view.right, view.up);
        if (fixed_cam && c == &fixed && fixed_roll != 0.0f) level_roll_basis(fixed_roll, view.fwd, view.right, view.up);
        memcpy(view.pos, c->pos, sizeof view.pos);
        view.fov_deg = c->fov_deg > 0.0f ? c->fov_deg : 90.0f;
        if (vr) { // (Q25) the head in the world: the listener, the culling's and the streaming's point, the HUD's view
            float hp[3], hf[3], hu[3], r[3];
            xr_head(hp, hf, hu);
            vr_pawn_to_world(hp, view.pos), vr_pawn_dir_to_world(hf, view.fwd), vr_pawn_dir_to_world(hu, view.up);
            r[0] = view.up[1] * view.fwd[2] - view.up[2] * view.fwd[1], r[1] = view.up[2] * view.fwd[0] - view.up[0] * view.fwd[2];
            r[2] = view.up[0] * view.fwd[1] - view.up[1] * view.fwd[0];
            memcpy(view.right, r, 12);
            // the VR menu panel: shown while a widget wants the mouse, 1.2 m before the head where it opened (the play space)
            static bool menu_was;
            bool menu = ui_mouse_shown() || dv_open();
            if (menu && !menu_was) {
                float f[3] = { hf[0], hf[1], 0.0f }, l = sqrtf(f[0] * f[0] + f[1] * f[1]);
                if (l < 1e-3f) f[0] = 1.0f, f[1] = 0.0f, l = 1.0f;
                f[0] /= l, f[1] /= l;
                const ProjectVr* pv = &project()->vr; // (Project Settings > VR > Menu Distance, Menu Width)
                float md = pv->menu_distance > 20.0f ? pv->menu_distance : 120.0f, mw = pv->menu_width > 10.0f ? pv->menu_width : 128.0f;
                float pp[3] = { hp[0] + f[0] * md, hp[1] + f[1] * md, hp[2] - 10.0f };
                xr_panel_place(pp, f, mw, mw * (float)VR_PANEL_H / (float)VR_PANEL_W, true);
            } else if (!menu && menu_was) {
                float z[3] = { 0 }, f[3] = { 1, 0, 0 };
                xr_panel_place(z, f, 0.0f, 0.0f, false);
            }
            menu_was = menu;
            vr_pawn_set_menu(menu);
        }
        view.highlight[0] = interaction_highlight(&world, &view.highlight_mode); // (interaction.h: the Interactable in focus)
        view.highlight[1] = view.highlight[0] ? interaction_highlight_part(&world) : ENTITY_NONE;
        audio_pause_game(frozen); // (the game's sounds wait with it; the UI's go on)
        TRACE("Audio", audio_world_update(&world, (float)(dt > 0.1 ? 0.1 : dt), true, view.pos, view.fwd, view.up)); // the listener: the player's camera
        bool ar_tracked = false;
        if (arcore_wanted) { // ARCore: the phone's camera is the game's; the picture behind, the real floor's shadows, its light
            if (arcore_view()->simulated) arcore_sim_camera(view.pos, view.fwd, view.right, view.up, view.fov_deg);
            const ArcoreView* av = arcore_update((uint32_t)fbw, (uint32_t)fbh);
            ar_tracked = av->state == ARCORE_TRACKING;
            if (ar_tracked && !av->simulated) {
                memcpy(view.pos, av->pos, 12), memcpy(view.fwd, av->fwd, 12), memcpy(view.right, av->right, 12), memcpy(view.up, av->up, 12);
                view.fov_deg = av->hfov_deg;
            }
            static float ar_tris[4096 * 3];
            uint32_t nv = ar_tracked ? arcore_floor_triangles(ar_tris, 4096) : 0;
            render_ar_frame(av->y != NULL, av->y, av->uv, av->img_w, av->img_h, av->img_serial, av->uv_corners, ar_tris, nv, av->simulated ? 0.6f : 0.25f + 0.5f * ar_light.strength);
            render_ar_depth(ar_tracked && av->y ? av->depth : NULL, av->depth_w, av->depth_h, av->depth_serial, av->depth_corners);
            if (ar_tracked && (av->frame % 8u) == 0u) arcore_sync_planes(&world);
            // the light: the picture's (exposure, white balance, ambient) a few times a second, the main light's
            // direction from ARCore's HDR estimate when it has one (the picture's brightest region otherwise)
            if (av->y && (av->frame % 10u) == 0u) {
                static uint8_t small[96 * 72 * 4];
                for (uint32_t yy = 0; yy < 72; yy++)
                    for (uint32_t xx = 0; xx < 96; xx++) {
                        uint32_t sx = xx * av->img_w / 96u, sy = yy * av->img_h / 72u;
                        float Y = av->y[sy * av->img_w + sx];
                        const uint8_t* c = av->uv + 2u * ((sy / 2u) * (av->img_w / 2u) + sx / 2u);
                        float U = c[0] - 128.0f, V = c[1] - 128.0f;
                        float rgb[3] = { Y + 1.402f * V, Y - 0.344136f * U - 0.714136f * V, Y + 1.772f * U };
                        for (int k = 0; k < 3; k++) small[4 * (yy * 96 + xx) + k] = (uint8_t)fminf(fmaxf(rgb[k], 0.0f), 255.0f);
                        small[4 * (yy * 96 + xx) + 3] = 255;
                    }
                ar_estimate(small, 96, 72, av->hfov_deg > 1.0f ? av->hfov_deg : 70.0f, &ar_now);
                if (av->light_valid) {
                    const float* d = av->light_dir; // (toward the light: into the camera's view, x right, y up, z forward)
                    ar_now.dir_view[0] = d[0] * view.right[0] + d[1] * view.right[1] + d[2] * view.right[2];
                    ar_now.dir_view[1] = d[0] * view.up[0] + d[1] * view.up[1] + d[2] * view.up[2];
                    ar_now.dir_view[2] = d[0] * view.fwd[0] + d[1] * view.fwd[1] + d[2] * view.fwd[2];
                    float li = 0.2126f * av->light_rgb[0] + 0.7152f * av->light_rgb[1] + 0.0722f * av->light_rgb[2];
                    float amb = 0.2126f * av->sh[0][0] + 0.7152f * av->sh[0][1] + 0.0722f * av->sh[0][2];
                    ar_now.strength = li > 0.0f ? fminf(li / (li + amb * 3.0f + 1e-4f), 1.0f) : ar_now.strength;
                }
                ar_on = true;
            }
        }
        if (ar_mode && !ar_tracked) { // a phone's rotation turns the camera around where it stands (no tracking: 3-DoF)
            float df[3], dr[3], du[3];
            if (ar_device_rotation(df, dr, du)) memcpy(view.fwd, df, 12), memcpy(view.right, dr, 12), memcpy(view.up, du, 12);
        }
        // the light matched, the simulated AR's too (blended: a camera's exposure changing doesn't flicker the scene). The
        // exposure from the picture, never metered: the picture is drawn to show the camera's colors whatever the
        // exposure, so metering it only fed the exposure back (a dark picture ran it to the maximum: the things white)
        if (ar_on) {
            ar_blend(&ar_light, &ar_now, 0.1f);
            view.exposure_ev100 = ar_apply(&world, &ar_light, view.fwd, view.right, view.up), view.auto_exposure = false;
        }
        view.x = view.y = 0.0f;
        view.w = (float)fbw;
        view.h = (float)fbh;
        static PostSettings post; // (the Post Process Volumes at the camera)
        post_fill_view(&world, view.pos, bp_player(&world), true, &post, &view);

        // the Debug Viewer's window (ImGui only while it is open; closing it turns its measures off)
        bool ui = dv_open() || ((ui_any_shown() || health_numbers_active() || quest_markers_shown() || rtl_blocking()) && ui_init(&vk)); // (the widgets, the damage numbers, Q16's loading screen: ImGui's draw lists too)
        if (ui) {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (vr) { // (Q25) the UI on the VR menu panel: its size, the laser as the mouse, the trigger its button
                ImGuiIO* vio = igGetIO_Nil();
                vio->DisplaySize = (ImVec2){ (float)VR_PANEL_W, (float)VR_PANEL_H }, vio->DisplayFramebufferScale = (ImVec2){ 1.0f, 1.0f };
                float uv[2];
                bool down = false;
                if (vr_pawn_ui_pointer(uv, &down)) {
                    ImGuiIO_AddMousePosEvent(vio, uv[0] * (float)VR_PANEL_W, uv[1] * (float)VR_PANEL_H);
                    ImGuiIO_AddMouseButtonEvent(vio, 0, down);
                } else
                    ImGuiIO_AddMousePosEvent(vio, -1e6f, -1e6f);
            }
            igNewFrame();
            DvContext dc = { &world, false, true, last_frame_ms, last_cpu_ms, vk.gpu_ms, { 0 }, { 0 }, view.fov_deg, view.w, view.h };
            memcpy(dc.camera, view.pos, sizeof dc.camera);
            memcpy(dc.fwd, view.fwd, sizeof dc.fwd);
            dv_frame(&dc);
            hud_set_view(view.pos, view.fwd, view.right, view.up, view.fov_deg); // (the HUD's markers, compass, numbers: hud.h)
            if (ui_any_shown()) { // UMG: under the Debug Viewer's window
                ImGuiIO* io = igGetIO_Nil();
                float mp[2] = { io->MousePos.x, io->MousePos.y };
                ui_draw_viewport(&world, igGetBackgroundDrawList(NULL), 0.0f, 0.0f, vr ? (float)VR_PANEL_W : (float)fbw, vr ? (float)VR_PANEL_H : (float)fbh,
                                 ui_mouse_shown() || vr ? mp : NULL,
                                 igIsMouseDown_Nil(ImGuiMouseButton_Left), igIsMouseClicked_Bool(ImGuiMouseButton_Left, false));
            }
            ui_draw_damage_numbers(igGetBackgroundDrawList(NULL), view.pos, view.fwd, view.right, view.up, view.fov_deg, 0.0f, 0.0f, (float)fbw, (float)fbh);
            quest_draw_markers(igGetBackgroundDrawList(NULL), view.pos, view.fwd, view.right, view.up, view.fov_deg, 0.0f, 0.0f, (float)fbw, (float)fbh); // (quest_hud.c)
            if (rtl_blocking()) { // (Q16) Build Lighting (Async) in Loading Screen mode: the project's loading screen over the level
                static double s_load_t0;
                if (s_load_t0 <= 0.0) s_load_t0 = re_time_seconds();
                brand_splash_draw(re_time_seconds() - s_load_t0, rtl_progress(), TR("Building the lighting..."));
            }
            igRender();
            ImDrawData* dd = igGetDrawData(); // its textures (the font atlas) with the GPU idle, as the editor does
            if (dd && dd->Textures) {
                bool idle = false;
                for (int i = 0; i < dd->Textures->Size; i++) {
                    ImTextureData* t = dd->Textures->Data[i];
                    if (t->Status == ImTextureStatus_OK) continue;
                    if (t->Status != ImTextureStatus_WantDestroy && !idle) vkDeviceWaitIdle(vk.device), idle = true;
                    ImGui_ImplVulkan_UpdateTexture(t);
                }
            }
            if (!dv_open()) glfwSetInputMode(G.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); // closed by its button
        } else {
            dv_frame(NULL);
        }

        double w0 = re_time_seconds();
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vr_frame) { // (Q25) VR: both eyes in one frame of the game: the first pays the shared work (its frame never presented),
            // the second only its view and shows both in the window (the Quest: no window); the menu panel's UI into its quad
            view.camera_blur = false;
            view.vr_vignette = vr_pawn_vignette();
            view.vr_fade = vr_pawn_fade();
            view.foveation = vr_fov_level;
            mr_render(&world, &view, eyes); // (Q25b) mixed reality: the passthrough's alpha, the room's boxes, the real depth, the exposure
#ifdef __ANDROID__
            const bool mirror = false; // (the Quest's window is never seen: nothing presented)
#else
            const bool mirror = true;
#endif
            // (Q25) Project Settings > VR > Window Mirror (vr.MirrorMode): both eyes, one eye filling the window, or the
            // Spectator Camera (a steady view of its own: recording, streaming)
            const int mmode = project()->vr.mirror_mode;
            const bool spect = mirror && mmode == VR_MIRROR_SPECTATOR, mirror_eyes = mirror && !spect;
            VkImage eye_img[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
            bool headset = false;
            double vr_gpu = 0.0;
            for (int e = 0; e < 2; e++) {
                RenderView ev = view;
                const XrEye* x = &eyes[e];
                ev.stereo_second = e == 1 && vr_share; // (REALENGINE_VR_SHARE=0: each eye pays the whole frame, the measure's other half)
                // (the eye the window shows (One Eye) without the foveation: its rings were a "glass circle" on the PC's
                // screen, the recording's; the other eye keeps it, the dynamic foveation pays the difference)
                if (mirror_eyes && mmode == VR_MIRROR_ONE_EYE && e == (project()->vr.dominant_hand ? 0 : 1)) ev.foveation = 0;
                ev.vr_eye = e; // (Q25b: mixed reality's real depth layer)
                vr_pawn_to_world(x->pos, ev.pos);
                vr_pawn_dir_to_world(x->fwd, ev.fwd), vr_pawn_dir_to_world(x->right, ev.right), vr_pawn_dir_to_world(x->up, ev.up);
                ev.fov_deg = 2.0f * atanf(x->tan_right) * 57.29578f;
                ev.x = ev.y = 0.0f, ev.w = (float)x->w, ev.h = (float)x->h;
                const bool last = e == 1;
                cmd = last && mirror_eyes ? vkctx_begin_frame(&vk) : vkctx_begin_frame_offscreen(&vk);
                if (!cmd) break;
                vr_gpu += vk.gpu_ms; // (this slot's last frame: an eye's)
                bool scene = render_scene(&vk, cmd, &world, &ev);
                cmd = vkctx_cmd(&vk); // (A9.7c: the eye's frame split for async compute)
                vkctx_wait_submitted(&vk);
                VkImage img;
                VkImageView iv;
                uint32_t iw, ih;
                VkImageLayout from = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // (an OpenXR image after its wait)
                headset = xr_eye_target((uint32_t)e, &img, &iv, &iw, &ih);
                if (!headset) { // (simulated: images of our own)
                    if (!sim_eye[e].image)
                        gpu_image_create(&vk, &sim_eye[e], x->w, x->h, vk.color_format,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                    img = sim_eye[e].image, iv = sim_eye[e].view, iw = x->w, ih = x->h, from = VK_IMAGE_LAYOUT_UNDEFINED;
                }
                eye_img[e] = img;
                vkctx_begin_rendering_on(cmd, img, iv, iw, ih, from);
                if (scene) render_composite(&vk, cmd, &ev);
                // (an eye shown in the window waits in TRANSFER_SRC for the mirror; the Quest's goes back to what OpenXR wants)
                vkctx_end_rendering_on(cmd, img, mirror_eyes || !headset ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (last && ui && headset) { // the menu panel: the UI into the quad's image (cleared to transparent), as OpenXR wants it after
                    VkImage pimg;
                    VkImageView pview;
                    if (xr_panel_target(VR_PANEL_W, VR_PANEL_H, &pimg, &pview)) {
                        vkctx_begin_rendering_on(cmd, pimg, pview, VR_PANEL_W, VR_PANEL_H, VK_IMAGE_LAYOUT_UNDEFINED);
                        gpu_mark(cmd, "UI do jogo (painel VR)");
                        ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE);
                        vkctx_end_rendering_on(cmd, pimg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                        xr_panel_done();
                    }
                }
                if (last && mirror_eyes) { // the window: both eyes side by side, or the dominant eye's middle filling it, once
                    if (mmode == VR_MIRROR_ONE_EYE) {
                        int de = project()->vr.dominant_hand ? 0 : 1;
                        vkctx_mirror_crop(&vk, eye_img[de] ? eye_img[de] : img, iw, ih, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1.25f);
                    } else
                        vkctx_mirror_eyes(&vk, eye_img[0] ? eye_img[0] : img, img, iw, ih, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    if (headset) // (both back to what OpenXR wants at release)
                        for (int k = 0; k < 2; k++) {
                            if (!eye_img[k]) continue;
                            VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                            b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT, b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                            b.image = eye_img[k], b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                            VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                            di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
                            vkCmdPipelineBarrier2(cmd, &di);
                        }
                    vkctx_resume_rendering(&vk); // (vkctx_end_frame ends it)
                    if (ui && !headset) gpu_mark(cmd, "UI do jogo (ImGui)"), ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE); // (simulated: over the mirror)
                }
                vkctx_end_frame(&vk);
            }
            // both eyes released after their frames (the mirror read the first one in the second's)
            if (headset) xr_eye_done(0), xr_eye_done(1);
            if (spect && cmd) { // (Q25) the Spectator Camera: the head's place and look, smoothed, no roll, its own field of view;
                // drawn at the eyes' size (the renderer's targets kept: no reallocation) and its middle cropped into the window
                static float sp_pos[3], sp_yaw, sp_pitch;
                static bool sp_ok;
                float hp[3], hf[3], hu[3], H[3], F[3];
                xr_head(hp, hf, hu), vr_pawn_to_world(hp, H), vr_pawn_dir_to_world(hf, F);
                float yaw = atan2f(F[1], F[0]) * 57.29578f, pitch = asinf(fmaxf(fminf(F[2], 1.0f), -1.0f)) * 57.29578f;
                float sm = fminf(fmaxf(project()->vr.spectator_smooth, 0.0f), 0.95f), k = fminf((float)dt * (1.0f - sm) * 30.0f, 1.0f);
                if (!sp_ok) memcpy(sp_pos, H, 12), sp_yaw = yaw, sp_pitch = pitch, sp_ok = true;
                float dy = fmodf(yaw - sp_yaw + 540.0f, 360.0f) - 180.0f;
                for (int q = 0; q < 3; q++) sp_pos[q] += (H[q] - sp_pos[q]) * k;
                sp_yaw += dy * k, sp_pitch += (pitch - sp_pitch) * k;
                RenderView sv = view;
                sv.stereo_second = vr_share, sv.foveation = 0, sv.vr_vignette = 0.0f;
                float cy = cosf(sp_yaw * 0.0174533f), sy = sinf(sp_yaw * 0.0174533f), cp = cosf(sp_pitch * 0.0174533f), spp = sinf(sp_pitch * 0.0174533f);
                memcpy(sv.pos, sp_pos, 12);
                sv.fwd[0] = cp * cy, sv.fwd[1] = cp * sy, sv.fwd[2] = spp;
                sv.right[0] = -sy, sv.right[1] = cy, sv.right[2] = 0.0f;
                sv.up[0] = sv.fwd[1] * sv.right[2] - sv.fwd[2] * sv.right[1], sv.up[1] = sv.fwd[2] * sv.right[0] - sv.fwd[0] * sv.right[2];
                sv.up[2] = sv.fwd[0] * sv.right[1] - sv.fwd[1] * sv.right[0];
                sv.fov_deg = project()->vr.spectator_fov > 30.0f ? project()->vr.spectator_fov : 90.0f;
                sv.x = sv.y = 0.0f, sv.w = (float)eyes[0].w, sv.h = (float)eyes[0].h;
                cmd = vkctx_begin_frame(&vk);
                if (cmd) {
                    bool scene = render_scene(&vk, cmd, &world, &sv);
                    cmd = vkctx_cmd(&vk);
                    vkctx_wait_submitted(&vk);
                    if (!spec_img.image || spec_w != eyes[0].w || spec_h != eyes[0].h) {
                        if (spec_img.image) vkDeviceWaitIdle(vk.device), gpu_image_destroy(&vk, &spec_img);
                        gpu_image_create(&vk, &spec_img, eyes[0].w, eyes[0].h, vk.color_format,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                        spec_w = eyes[0].w, spec_h = eyes[0].h;
                    }
                    vkctx_begin_rendering_on(cmd, spec_img.image, spec_img.view, spec_w, spec_h, VK_IMAGE_LAYOUT_UNDEFINED);
                    if (scene) render_composite(&vk, cmd, &sv);
                    vkctx_end_rendering_on(cmd, spec_img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    vkctx_mirror_crop(&vk, spec_img.image, spec_w, spec_h, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1.0f);
                    vkctx_resume_rendering(&vk);
                    if (ui && !headset) gpu_mark(cmd, "UI do jogo (ImGui)"), ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE);
                    vkctx_end_frame(&vk);
                }
            }
            xr_end_frame();
            // (Q25) the dynamic foveation: deeper while the GPU's pair of eyes runs past 90% of the display's period, back
            // after 1 s under 70% (the Project Settings' level is the deepest)
            if (cfg.vr.foveation_dynamic && cfg.vr.foveation > 0 && vr_gpu > 0.0) {
                // the budget: the display's period, or tighter (Project Settings > VR > Headroom Target: 150 fps at a 120 Hz
                // display keeps the two eyes in 6.7 ms, room for a heavier moment, the PC's other work, the Link's encoder)
                double budget = xr_frame_period_ms();
                int hr = project()->vr.headroom_fps;
                if (hr > 0 && 1000.0 / hr < budget) budget = 1000.0 / hr;
                if (vr_gpu > budget * 0.9 && vr_fov_level < cfg.vr.foveation) vr_fov_level++, vr_fov_calm = 0.0;
                else if (vr_gpu < budget * 0.7) {
                    if ((vr_fov_calm += dt) > 1.0 && vr_fov_level > 1) vr_fov_level--, vr_fov_calm = 0.0;
                } else
                    vr_fov_calm = 0.0;
            }
            vr_last_pair_gpu_ms = vr_gpu;
        } else
            cmd = vr ? VK_NULL_HANDLE : vkctx_begin_frame(&vk); // waits for the frame slot and the swapchain image
        double w1 = re_time_seconds();
        trace_span("Wait for GPU (frame slot)", w0, w1);
        if (cmd && !vr) {
            double rs0 = trace_t0();
            bool scene = render_scene(&vk, cmd, &world, &view);
            cmd = vkctx_cmd(&vk); // (A9.7c: a frame split for async compute goes on in its second part)
            trace_scope("Render (record scene)", rs0);
            vkctx_wait_submitted(&vk);
            if (vk.hdr) { // (A9.4) HDR10: the UI into its own target first (cleared even without one), laid over by the composite
                render_hdr_ui_begin(&vk, cmd);
                if (ui) gpu_mark(cmd, "UI do jogo (ImGui, HDR10)"), ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE);
                render_hdr_ui_end(&vk, cmd);
            }
            vkctx_begin_rendering(&vk);
            if (scene) render_composite(&vk, cmd, &view), render_debug_draw(&vk, cmd, &view);
            if (ui && !vk.hdr) gpu_mark(cmd, "UI do jogo (ImGui)"), ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE);
            TRACE("Submit and Present", vkctx_end_frame(&vk));
        }
        if (vr && !cmd) re_sleep_seconds(0.005); // (the headset off: no spinning)
        if (GV.frame_limit > 0 && bench_s <= 0.0) { // (the options' Frame Rate Limit)
            double left = 1.0 / GV.frame_limit - (re_time_seconds() - t0);
            if (left > 0.0005) re_sleep_seconds(left);
        }
        double t1 = re_time_seconds();
        last_cpu_ms = ((t1 - t0) - (w1 - w0)) * 1000.0;
        if (g_trace_on) { // (B14b) the frame and the GPU's passes (their timestamps, the frame before last)
            const char* gn[16];
            double gm[16];
            trace_frame(t0, t1, gn, gm, trace_gpu_passes(gn, gm, 16));
        }
        if (cmd) mobile_frame_end(&view.gi, t0, last_cpu_ms, vk.gpu_ms); // (a phone: the resolution, the heat, the pace)
        else if (GV.dyn_res && GV.top_scale > 0.0f && GV.preset_scale > 0.0f) // the dynamic resolution (upscale.h): down to upscale_dynamic_floor
            view.gi.tsr_scale = upscale_dynamic(&GV.dyn, vk.gpu_ms, (float)(project()->dyn_res_fps > 0 ? project()->dyn_res_fps : 100),
                                                upscale_dynamic_floor(GV.top_scale, project()->dyn_res_min), GV.top_scale);
        last_frame_ms = dt * 1000.0;
        if (cmd && ++frames_drawn == 1) { // the start: what a player waits for (a cold one compiles every shader)
            double start_ms = (t1 - launched) * 1000.0;
            printf("[RealEngine] Start: first frame %.0f ms after launch; %u pipelines in %.0f ms (pipeline cache: %s, %zu KB)\n",
                   start_ms, vk.pipelines, vk.pipelines_ms, vk.pipeline_cache_loaded ? "warm" : "cold",
                   vk.pipeline_cache_loaded / 1024);
            fflush(stdout);
            if (max_start_ms > 0.0 && start_ms > max_start_ms) {
                printf("[RealEngine] FAIL: the first frame took %.0f ms > %.0f\n", start_ms, max_start_ms);
                exit_code = 1;
            }
        }
        if (cmd && t1 - launched < 2.0) frames_early++; // the first 2 s: a driver still optimizing shows here

        { // (Q16) a Build Lighting in the game: its frames measured (a hitch: the worst one), the measure waits for its end
            static bool s_was;
            static uint32_t s_n;
            static double s_sum, s_worst, s_t0;
            bool busy = rtl_busy();
            if (busy && !s_was) s_n = 0, s_sum = s_worst = 0.0, s_t0 = t1;
            if (busy && s_was && s_n++ > 0) { // (the frame that started it: its CPU start reported apart)
                s_sum += dt * 1000.0;
                if (dt * 1000.0 > s_worst) s_worst = dt * 1000.0;
            }
            if (!busy && s_was) {
                GiBakeStats st;
                gi_bake_stats(&st);
                printf("[RealEngine] Frames while baking: %u in %.2f s, mean %.3f ms, worst %.3f ms; the bake's GPU %.3f ms a frame (%.3f at most), "
                       "its start %.1f ms (CPU)\n",
                       s_n, t1 - s_t0, s_n ? s_sum / s_n : 0.0, s_worst, st.gpu_avg_ms, st.gpu_max_ms, st.start_ms);
                if (max_bake_frame_ms > 0.0 && s_worst > max_bake_frame_ms) {
                    printf("[RealEngine] FAIL: a frame of %.1f ms while baking > %.1f\n", s_worst, max_bake_frame_ms);
                    exit_code = 1;
                }
            }
            s_was = busy;
            if (busy && bench_s > 0.0) bench_from = t1 + 0.5, bench_until = bench_from + bench_s, frames = 0, cpu_sum = gpu_sum = worst = 0.0;
        }
        // --bench: the same measure as the editor's (after a second of warm-up)
        if (bench_s > 0.0 && t0 >= bench_from) {
            double cpu_ms = ((t1 - t0) - (w1 - w0)) * 1000.0;
            if (frames == 0) bench_from = t0, bench_until = t0 + bench_s; // the whole window, however late the first frame
            if (shot_path && t1 >= bench_until - 0.25) vkctx_request_shot(&vk, shot_path), shot_path = NULL;
            frames++;
            cpu_sum += cpu_ms;
            gpu_sum += vk.gpu_ms;
            vr_pair_sum += vr_last_pair_gpu_ms;
            if (frames > 1 && dt * 1000.0 > worst) worst = dt * 1000.0;
            if (t1 >= bench_until) {
                double secs = t1 - bench_from, fps = (double)frames / secs;
                printf("[RealEngine] Measure: %.0f fps (%.3f ms/frame, worst %.3f ms), CPU %.3f ms, GPU %.3f ms - %llu frames in "
                       "%.2f s, %ux%u, %s\n",
                       fps, 1000.0 / fps, worst, cpu_sum / frames, gpu_sum / frames, (unsigned long long)frames, secs,
                       vk.extent.width, vk.extent.height, vk.gpu_props.deviceName);
                printf("[RealEngine] First 2 s after launch: %llu frames\n", (unsigned long long)frames_early);
                if (vr) // (Q25) a VR frame is both eyes: their GPU summed (the timestamps of each eye's frame), the foveation's level
                    printf("[RealEngine] VR measure: %.0f frames/s (both eyes), GPU %.3f ms the pair (%.0f fps of GPU: the headroom), foveation %d, shared work %s, %u x %u per eye at %.0f Hz\n", fps,
                           vr_pair_sum / frames, vr_pair_sum > 0.0 ? 1000.0 / (vr_pair_sum / frames) : 0.0, vr_fov_level, vr_share ? "on" : "off", xr_info()->eye_w,
                           xr_info()->eye_h, 1000.0 / xr_frame_period_ms());
                if (vr && mr_on()) { // (Q25b) mixed reality: what runs, the room, the CPU of its two steps
                    const MrStatus* ms = mr_status();
                    printf("[RealEngine] MR measure: passthrough %s, room %u (mesh %u triangles%s), real depth %s (%u x %u, %llu images), CPU tick %.3f ms, render %.3f ms\n",
                           xr_info()->sim ? "simulated" : ms->passthrough_running ? "on" : "off", mr_room()->n, mr_room()->mesh_ni / 3u,
                           mr_room()->simulated ? ", stand-in" : "", ms->depth_running ? "headset" : xr_info()->sim && project()->vr.mr_occlusion == MR_OCC_DEPTH ? "simulated" : "off",
                           ms->depth_w, ms->depth_h, (unsigned long long)ms->depth_frames, ms->tick_ms, ms->render_ms);
                }
                if (vr) {
                    const VrStats* vs = vr_pawn_stats();
                    printf("[RealEngine] VR pawn: %u teleports, %u snap turns, %u grabs, %u throws, %u socketed, %u drags, %u pokes, %u UI clicks\n", vs->teleports,
                           vs->snap_turns, vs->grabs, vs->throws, vs->socketed, vs->drags, vs->pokes, vs->ui_clicks);
                }
                const RenderStats* rs = render_stats();
                printf("[RealEngine] Render: GPU scene %.3f, GI %.3f, shadow %.3f, main %.3f, mask %.3f, exposure %.3f, post %.3f, composite %.3f ms; "
                       "CPU %.3f ms; %u/%u meshes, %u lights, MSAA %ux\n",
                       rs->gpu_scene_ms, rs->gpu_gi_ms, rs->gpu_shadow_ms, rs->gpu_main_ms, rs->gpu_mask_ms, rs->gpu_exposure_ms,
                       rs->gpu_post_ms, rs->gpu_composite_ms, rs->cpu_ms, rs->visible, rs->instances, rs->lights, rs->msaa);
                if (min_fps > 0.0 && fps < min_fps) {
                    printf("[RealEngine] FAIL: %.0f fps < %.0f\n", fps, min_fps);
                    exit_code = 1;
                }
                if (g_vk_validation_errors > 0) {
                    printf("[RealEngine] FAIL: %d Vulkan validation errors\n", g_vk_validation_errors);
                    exit_code = 1;
                }
                break;
            }
        }
    }

    if (trace_path && trace_finish_check()) exit_code = 1; // (B14b) the file written and read back
    plugins_shutdown();
    online_end_play(&world);
    online_shutdown();
    audio_world_end_play(&world);
    audio_shutdown();
    wp_end(&world);
    bp_end_play(&world);
    physics_end_play(&world);
    spline_end_play(&world); // (Q14)
    vehicle_end_play(&world);
    procgen_end_play(&world);
    pcg_end_play(&world);
    anim_end_play(&world);
    ai_end_play(&world);
    bp_shutdown();
    vkDeviceWaitIdle(vk.device);
    brand_shutdown(&vk);
    if (igGetCurrentContext()) { // the Debug Viewer was opened
        vkDeviceWaitIdle(vk.device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        igDestroyContext(NULL);
    }
    for (int e = 0; e < 2; e++)
        if (sim_eye[e].image) vkDeviceWaitIdle(vk.device), gpu_image_destroy(&vk, &sim_eye[e]);
    mr_end_play(&world); // (Q25b)
    vr_pawn_end_play(&world);
    if (spec_img.image) vkDeviceWaitIdle(vk.device), gpu_image_destroy(&vk, &spec_img);
    xr_shutdown();
    render_shutdown(&vk);
    ecs_shutdown(&world);
    re_arena_free(&g_frame_arena);
    vkctx_shutdown(&vk);
    glfwDestroyWindow(G.window);
    glfwTerminate();
    fflush(stdout);
    return exit_code;
}
