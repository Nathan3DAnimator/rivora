// Real Engine editor: the window, Vulkan, Dear ImGui and the frame loop with its measures (ms and FPS).
// Command line: --vsync, --validation, --bench <s> (measures the frame for s seconds, prints it and exits),
// --min-fps <n> (with --bench: exit code 1 below n fps; the editor_fps test).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

#include "loc_ui.h"
#include "editor_docs.h"
#include "ui_viewports.h" // (item 9)
#include "delight.h"
#include "theme.h"
#include "editor_dock.h"
#include "dock_test.h"
#include "ui_sweep.h" // (item 21) --ui-sweep
#include "picker_test.h"
#include "asset_viewport.h"
#include "bp_editor.h"
#include "anim_editor.h"
#include "ai_editor.h"
#include "sequencer_ui.h"
#include "dialogue.h"
#include "quest.h"
#include "hud.h"
#include "demo_play.h"
#include "mat_editor.h"
#include "debug_viewer.h"
#include "anim.h"
#include "blueprint.h"
#include "content.h"
#include "post.h"
#include "movie_render.h"
#include "source_control.h"
#include "trace.h"
#include "plugins.h"
#include "cvars.h"
#include "editor.h"
#include "mcp.h"
#include "rig_detect.h"
#include "loc.h" // (2026-11-24) --loc-test
#include "level_preload.h"
#include "frame_budget.h"
#include "upscale.h"
#include "save_progress.h"
#include "graph_menu.h"
#include "particles.h"
#include "ikrig.h"
#include "vehicle.h"
#include "gi_fallback.h"
#include "weather_field.h"
#include "water.h"
#include "cloth.h"
#include "ar.h"
#include "ar_arcore.h"
#include "cloth_mesh.h"
#include "softbody.h"
#include "ui_widget.h"
#include "mobile.h"
#include "vr_pawn.h"
#include "xr_mr.h" // (Q25b) --mr-test
#include "procgen.h"
#include "phys_test.h"
#include "spline.h" // (Q14) --spline-test
#include "terror_test.h"
#include "pcg.h"
#include "pcg_test.h"
#include "udim_test.h" // (Q18)
#include "emissive_test.h" // (2026-11-23)
#include "mat_first_test.h" // (2026-11-24) the materials' color before any bake
#include "farview.h" // (Q21)
#include "farview_test.h" // (Q21)
#include "ai_senses_test.h" // (Q24)
#include "ingame_test.h" // (Q16)
#include "ikrig_editor.h"
#include "vehicle_editor.h"
#include "project.h"
#include "assistant.h" // (2026-11-25: its base on a worker)
#include "script.h"
#include "gi_bake.h"
#include "gi_cache.h"
#include "vsm.h"
#include "lsm.h"
#include "online.h"
int net_selftest(void); // (net_test.c)
bool net_session_begin(World* w, bool host, const char* join_addr, double child_seconds, const NetSim* emu); // (net_session_test.c)
void net_session_frame(World* w);
void net_session_set_advanced(bool on); // (--net-adv)
bool net_session_check(World* w);
int steam_selftest(void);
#include "tess.h"
#include "render.h"
#include "scene_query.h" // (2026-11-19) traces
#include "debug_draw.h"
#include "decal.h"
#include "crashlog.h"
#include "services.h"
#include "mem.h"
#include "platform.h"
#include "platform_files.h" // (re_make_dirs: the C4013 warning)
#include "editor_look.h"
#include "particle_editor.h"
#include "updater.h"
#include "vkctx.h"
#include "brand.h"
#include "asset_viewer.h"
#include "i18n.h"
#include "ui_tips.h"
#include "ui_icons.h"
#include "material.h"
#include "gameplay_tags.h"
#include "bp_debug.h"
#include "async.h"
#include "assistant_ui.h"
#include "mesh_build.h" // (2026-11-24: --skin-lods-test)
void selection_test_scene(Editor* ed); // (2026-11-25, item 17: selection_test.c)
void selection_test_tick(Editor* ed, VkCtx* vk);
bool scal_mem_test_frame(Editor* ed, VkCtx* vk, double dt, bool* ok); // (2026-11-25, bug-fixer: scal_mem_test.c)
bool place_hitch_test_frame(Editor* ed, const char* name, double max_ms, double dt, double now, bool* ok); // (place_hitch_test.c)
bool selection_test_check(Editor* ed);
void groom_skin_test_tick(Editor* ed, VkCtx* vk); // (2026-11-25, item 16: groom_skin_test.c)
bool groom_skin_test_check(Editor* ed);

static VkQueryPool ui_q; // the UI pass's two timestamps per frame slot (the HUD's GPU cost: hud.h)
static bool ui_q_used[RE_FRAMES_IN_FLIGHT];

static void check_vk_result(VkResult r) { vk_check(r, "ImGui Vulkan"); }

// (Q5) the standard tooltip's icon (ui_tips.c links without the icons in the game)
static void tip_icon(ImDrawList* dl, int icon, float x, float y, float size, uint32_t col) { ui_icon(dl, (UiIcon)icon, x, y, size, col); }

static void glfw_error(int code, const char* msg) { fprintf(stderr, "[GLFW] %d: %s\n", code, msg); }

static Editor* g_editor;

// Files dropped from Explorer: imported into the Content Browser's folder (the drawer opens to show them)
static void on_drop(GLFWwindow* w, int count, const char** paths) {
    (void)w;
    content_import(count, paths);
    if (g_editor && !g_editor->content_docked) g_editor->drawer_open = true;
}

// A project's load on the loading screen (editor.c's steps) and its own picture (brand.h)
static VkCtx* g_vk;
// The hang watchdog's test (--hang-test): one frame frozen in a function of a known name; the report written meanwhile
// must exist and name it (the stack read from the frozen main thread, with the .pdb's symbols)
static volatile int g_hang_test_done;
static __declspec(noinline) void hang_test_freeze(double seconds) {
    re_sleep_seconds(seconds);
    g_hang_test_done++; // (after the call: no tail call, the frame stays on the stack)
}
static bool hang_test_check(void) {
    const char* path = crash_last_hang_report();
    char text[16384] = "";
    FILE* f = path[0] ? re_fopen(path, "rb") : NULL;
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    bool ok = n > 0 && strstr(text, "hang_test_freeze") != NULL;
    printf("[RealEngine] %s: %s\n", ok ? "Hang watchdog: OK, the report names the frozen function" : "FAIL: hang watchdog", path[0] ? path : "(no report)");
    if (path[0]) re_remove_file(path); // (the test's: not left among the real ones)
    return ok;
}

// (2026-11-19) a cold pipeline cache (the first start after an update): the loading screen counts the main pass's
// shaders compiled on the background threads (render_prewarm) instead of the editor standing still
static void wait_shaders_splash(VkCtx* vk, float from) {
    uint32_t done, total;
    double t0 = re_time_seconds();
    while (render_prewarm_progress(&done, &total)) {
        char s[200];
        snprintf(s, sizeof s, TR("Compiling the shaders: %u of %u (the first time after an update)..."), done, total);
        brand_splash_frame(vk, from + (0.98f - from) * (float)done / (float)(total ? total : 1), s);
        re_sleep_seconds(0.016);
    }
    if (re_time_seconds() - t0 > 0.5) printf("[RealEngine] The loading screen waited %.1f s for the shaders.\n", re_time_seconds() - t0);
}
static void splash_progress(float p, const char* status) {
    if (g_vk) brand_splash_frame(g_vk, p, status);
}
static void splash_picture(const char* path) {
    if (g_vk && !brand_set_splash(g_vk, path) && path && path[0]) printf("[RealEngine] Loading screen: could not read %s\n", path);
}
static bool crash_kind_env(void) { return getenv("REALENGINE_CRASH_TEST") != NULL; }

// --source-radius <cm>: every local light of the level a sphere/disk of that radius (area shadows: the shadow split's
// filter, render.c; the test scenes have none)
static void set_source_radius(World* w, float cm) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_LIGHT)) || !a->count) continue;
        CLight* l = ecs_arch_column(a, COMP_LIGHT);
        for (uint32_t i = 0; i < a->count; i++)
            if (l[i].type == LIGHT_POINT || l[i].type == LIGHT_SPOT) l[i].source_radius = cm, l[i].cast_shadows = 1;
    }
    ecs_touch_all(w);
}

// Back to the window: the files may have changed outside (Explorer, the Tlender's export)
static void on_focus(GLFWwindow* w, int focused) {
    (void)w;
    if (focused) content_mark_dirty();
}

// The editor's look: the current theme's tokens (theme.c; Editor Preferences > Appearance). Compact like a production
// tool; the windows fade in and out, the hovered colors ease in (the patched imgui, [Real Engine] comments: the
// durations are tokens too). Tooltips and descriptions wait half a second, beside the item.
static float g_theme_dpi = 1.0f;
static void apply_theme(float scale) {
    g_theme_dpi = scale;
    theme_apply_imgui(scale);
}

// The frame measures: averages over half a second for the status bar, and timed runs (--bench, Tools > Measure)
typedef struct Bench {
    double seconds, until, warm_until;
    uint64_t frames;
    double frame_sum, cpu_sum, gpu_sum, worst;
    double gpu_min, main_min, gi_min; // the least of the frames: ~the cost on a GPU shared with another program
    double skin_sum, skin_worst; // (2026-11-24) the render's dynamic set and skins on the CPU (render_skin_stats' ms)
    uint64_t skin_bends;         // (2026-11-24) the skins bent over the measure
    bool active, cli;
} Bench;

// (2026-11-25, bug-fixer) A self-test that returns before the editor starts: its crash leaves a report (the suite's
// ikphys_selftest of 03:45 died after its IK lines with none: a binary built in the middle of an edit, never seen again)
// and its lines reach the log as they're printed (a buffered pipe lost the ones before the fault)
static void selftest_guard(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    crash_install(&(CrashConfig){ NULL, false, true });
}

int main(int argc, char** argv) {
    services_early_init(); // crash reports, the log file (logs/realengine.log next to the reports)
    bool validation = false, vsync = false;
    Bench bench = { 0 };
    double min_fps = 0.0;
    double budget_ms = 0.0; // --budget <ms>: the measure fails when the frame is past it (frame_budget.h)
    bool budget_strict = false; // --budget-strict: each slice too
    int upscale_mode = -1;      // --upscale-mode: the render's share as the upscalers name it (upscale.h)
    float dyn_res_fps = 0.0f;   // --dyn-res <fps>: the dynamic resolution on
    int dyn_res_min = -1;       // --dyn-res-min <percent>: its floor (else the project's)
    bool check_gpu_marks = false;
    double max_scene_lum = -1.0; // --max-scene-lum: the probe's brightest point limit (the spec_leak test)
    double max_flicker = 0.0; // %: the flicker probe's limit (render_flicker)
    double max_pulses = 0.0;  // (Q20) %: the back-and-forth flicker's limit (render_flicker_pulses: a slow camera, --cam-move)
    double max_ghost = 0.0;   // (Q20) %: the trail's limit behind moving actors (render_ghosting)
    double max_skin_ms = 0.0; // (2026-11-24) --max-skin-ms: the render's dynamic set and skins, CPU ms a frame on average
    int skin_still = -1;      // (2026-11-24) --skin-still N: the scene's Skeletal Meshes stand with no animation but N (the Unamed case)
    int skin_copies = 0;      // (2026-11-24) --skin-copies N: the level's first Skeletal Mesh Actor copied N times (after --import)
    float cam_near_skin = 0.0f; // (2026-11-24) --cam-near-skin
    bool no_skins = false;      // (2026-11-25) --no-skins: the level's Skeletal Mesh Actors removed (the scene's own cost, measured)
    bool groom_skin_test = false; // (2026-11-25, item 16) --groom-skin-test: the Tlender's fur on a skin (groom_skin_test.c)
    bool select_all = false;      // (2026-11-25, item 17) --select-all: every actor selected after the load (the outline's photos)
    const char* select_name = NULL; // (2026-11-25, item 17) --select-name N: the actors named N (prefix) selected after the load
    bool no_spill = false;    // --no-spill: the main pass's shaders keep everything in registers (render_shader_spill)
    bool cache_all = false;   // --cache-all: fail when a mesh within the surface cache's reach got no chart
    const char* gi_mode = NULL;
    const char *bake_mode = NULL, *open_path = NULL, *save_path = NULL, *package_dir = NULL;
    bool package_android = false, package_play = false, package_quest = false, make_upload_key = false;
    bool no_ao = false, manual_exposure = false, no_cache = false;
    const char* view_name = NULL;
    float aerial_scale = -1.0f;
    uint32_t stress_objects = 0, stress_lights = 0, stress_textures = 0;
    float stress_fly = 1500.0f; // --stress-fly: the texture stress's camera speed (cm/s; 0: still)
    bool play_eject = false;
    bool game_view_cli = false; // --game-view
    int shot_every = 0;          // --shot-every N: with --shot a.png, a.png's frames a_000.png ... every N frames of the measure
    int shot_skip = 0;           // --shot-skip N: ... after the measure's first N frames (the GI's history settled: flicker hunts)
    float cam_move[3] = { 0, 0, 0 }; // --cam-move x,y,z: the editor camera's speed during the measure (cm/s; the shadows' tests)
    float source_radius = -1.0f; // --source-radius: the local lights' (set_source_radius)
    float stress_cast = 0.0f;
    uint32_t shadow_lights = 0; // --shadow-lights
    const char* scene_name = NULL;
    const char* project_path = NULL; // the projects (project.h)
    const char* new_project = NULL;
    const char* theme_cli = NULL; // --theme: a theme by its name (theme.h), only for this run
    bool layout_default = false; // (Q26) --layout-default: the whole default layout (every panel of the mockups)
    bool icon_sheet = false;      // --icon-sheet (Q5): every icon drawn over the editor (ui_icon_sheet_draw)
    const char* shot_path = NULL; // --shot: the window into a PNG near the end of --bench (vkctx_request_shot)
    bool shot_asked = false;
    int template_id = 0;
    bool show_project_browser = false, force_intro = false;
    const char* browser_open = NULL; // --browser-open: the Project Browser shown, then this project opened as by its click
    int browser_frames = 0, hang_frames = 0;
    bool hang_test = false;
    const char* place_hitch_name = NULL; // (2026-11-25) --place-hitch-test "<class>" <ms> (place_hitch_test.c)
    double place_hitch_ms = 0.0;
    bool scal_mem_test = false;
    bool vr_preview_now = false; // (2026-11-25) --vr-preview: Play > VR Preview once the project is open (child_process_cleanup) // (2026-11-25) --scalability-mem-test: Medium <-> Cinematic, the memory kept (scal_mem_test.c)
    float splash_hold = 0.0f;
    bool list_actors = false, vehicle_mesh_test = false;
    float add_floor = 0.0f;
    const char* bp_menu_test = NULL;
    const char* sun_spec = NULL;
    int scalability_cli = -1;
    int quit_after = 0; bool quit_test = false; // (2026-11-20) --quit-after N: the window closed as by its X after N frames, nothing automated (the test
                        // editor_close: the user's own shutdown, preferences and layout written; APPDATA a test folder)
    const char* cvar_cli[16]; // (Q8 leftover) --cvar name=value, as the packaged game's: the project's p.* for a measure
    int ncvar_cli = 0;
    const char* splash_file = NULL;
    const char* show_window = NULL;
    bool char_test = false, constraint_test = false, nav_test = false, morph_test = false, health_test = false, inv_test = false, save_test = false, ia_test = false, dlg_test = false, qst_test = false, rules_test = false;
    const char* ui_sweep_dir = NULL; // (item 21: --ui-sweep <dir>)
    bool abil_test = false, hud_test = false, levels_test = false, dock_test = false, picker_test = false, avp_test = false, artist_test = false, psw_test = false;
    bool mcp_flag = false, mcp_test = false; // (2026-11-24) --mcp: the MCP server on (mcp.h); --mcp-test: its test
    int mcp_port = 0;
    int pause_test = 0; // --pause-test 1, --pause-test-reload 2 (pause_test.c)
    const char* cine_test = NULL;
    const char* trace_path = NULL; // (B14b) --trace FILE
    bool plugin_test = false; // (B14c) --plugin-test
    bool lut_test = false;    // (A9.4) --lut-test
    bool ssr_test = false;    // (A4c) --ssr-test
    bool cine_cam_test = false; // (A9.4) --cine-cam-test
    const char* trace_open = NULL; // (B14b) --trace-open FILE
    bool bp_test = false, umg_test = false, seq_test = false, audio_test = false, script_test = false, ide_demo = false, async_test = false, assistant_test = false;
    bool cs_test = false, sk_move_test = false;
    bool bp_dbg_test = false; // --bp-debug-test: the Blueprint debugger (bp_debug_test.c)
    const char* view2_arg = NULL;
    bool ed_bp_idle = false;
    bool ed_bp_walkview = false;
    bool play = false;
    bool trace_test = false;
    bool vr_hand_test = false; // (2026-11-20) --vr-hand-test: the VR base hand exported, imported, solved (vr_hand_editor.c)
    uint32_t trace_bench_n = 0; // (2026-11-19) --trace-test, --trace-bench N (trace_test.c)
    bool net_host_test = false;     // --net-host-test, --net-join-test: online with two processes (net_session_test.c)
    const char* net_join_test = NULL;
    const char* pie_client = NULL; // (B13)
    int pie_window = 0, pie_players = 0, pie_max_seen = 0;
    NetSim net_emu = { 0 };
    const char* bp_package = NULL;
    const char* ingame_test = NULL, *ingame_package = NULL; // (Q16) --ingame-test loading|progressive [--ingame-package DIR]
    const char* open_asset = NULL;
    const char* cli_lang = NULL; // (2026-11-25, item 12) --lang pt_BR / es_ES / en_US: the interface's language for this run
    bool cli_project_settings = false; // --project-settings: Edit > Project Settings open (the screenshots)
    const char* import_tlend = NULL;
    bool import_folder = false; // (2026-11-23) --import-folder: a folder from Explorer, all of it (content_import_folder)
    bool mat_live_test = false; // (2026-11-24, #180) --mat-live-test
    bool mat_param_test = false; // (2026-11-24) --mat-param-test (the affine fold)
    bool class_split_check = false; // (2026-11-24) --class-split-check: the bench's main pass has plain and RE_EXT runs
    bool rig_fit_test = false; // (2026-11-23) --rig-fit-test: after --import, the rig found and the physics fitted to the skin
    bool mesh_socket_test = false; // (2026-11-23) --mesh-socket-test: after --import, a Static Mesh's sockets and material
    double import_budget_ms = 0.0; // (Q19) --import-budget: the import's time limit (the test fails past it)
    const char* reimport = NULL; // --reimport: a Skeletal Mesh read again from its source (Content Browser > Reimport)
    bool open_viewport = false;
    const char* browse = NULL;
    bool browse_hover = false;
    const char* mm_editor = NULL;
    const char* blend_space = NULL;
    const char* transition_rule = NULL;
    bool bt_blackboard = false;
    const char* sheet_anim = NULL, *sheet_png = NULL;
    float sheet_yaw = 90.0f;
    int sky_preset = -1;
    float cam_pitch = -1000.0f;
    const char* cam_spec = NULL;
    const char* ortho_name = NULL; // --ortho top | bottom | left | right | front | back: the main viewport's projection
    const char* package_icon = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--validation")) validation = true;
        else if (!strcmp(argv[i], "--vsync")) vsync = true;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) { bench.seconds = atof(argv[++i]); bench.cli = true; _putenv("REALENGINE_NO_UPDATE_CHECK=1"); crash_set_dialog(false); }
        else if (!strcmp(argv[i], "--min-fps") && i + 1 < argc) min_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--budget-strict")) budget_strict = true;
        else if (!strcmp(argv[i], "--upscaler") && i + 1 < argc) { // upscale.h UPS_*: tsr, fsr, dlss, xess or 0-3 (a measure, a test)
            const char* u = argv[++i];
            render_set_upscaler_override(!_stricmp(u, "tsr") ? 0 : !_stricmp(u, "fsr") ? 1 : !_stricmp(u, "dlss") ? 2 : !_stricmp(u, "xess") ? 3 : atoi(u));
        }
        else if (!strcmp(argv[i], "--render-style") && i + 1 < argc) { // render.h RENDER_STYLE_* (a measure, a test)
            render_set_style_override(atoi(argv[++i]));
            printf("[RealEngine] Render style: %s\n", render_style_name(atoi(argv[i])));
        }
        else if (!strcmp(argv[i], "--upscale-mode") && i + 1 < argc) upscale_mode = atoi(argv[++i]); // upscale.h UPM_* (the render's share)
        else if (!strcmp(argv[i], "--dyn-res") && i + 1 < argc) dyn_res_fps = (float)atof(argv[++i]); // the dynamic resolution's target
        else if (!strcmp(argv[i], "--dyn-res-min") && i + 1 < argc) dyn_res_min = atoi(argv[++i]); // its floor (Dynamic Resolution Min)
        else if (!strcmp(argv[i], "--upscale-test")) { // (upscale.h: the modes, the fallback, the dynamic resolution simulated)
            int r = upscale_selftest() ? 0 : 1;
            return r;
        }
        else if (!strcmp(argv[i], "--max-scene-lum") && i + 1 < argc) max_scene_lum = atof(argv[++i]);
        else if (!strcmp(argv[i], "--check-gpu-marks")) check_gpu_marks = true; // the GPU crash markers (test gpu_marks)
        else if (!strcmp(argv[i], "--max-flicker") && i + 1 < argc) max_flicker = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-pulses") && i + 1 < argc) max_pulses = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-ghost") && i + 1 < argc) max_ghost = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-skin-ms") && i + 1 < argc) max_skin_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--skin-still") && i + 1 < argc) skin_still = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skin-copies") && i + 1 < argc) skin_copies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-skins")) no_skins = true;
        else if (!strcmp(argv[i], "--groom-skin-test")) groom_skin_test = true;
        else if (!strcmp(argv[i], "--select-all")) select_all = true;
        else if (!strcmp(argv[i], "--select-name") && i + 1 < argc) select_name = argv[++i];
        else if (!strcmp(argv[i], "--cam-near-skin") && i + 1 < argc) cam_near_skin = (float)atof(argv[++i]); // (2026-11-24) the camera D cm from the first Skeletal Mesh Actor
        else if (!strcmp(argv[i], "--cache-all")) cache_all = true; // every mesh within the surface cache's reach has a chart
        else if (!strcmp(argv[i], "--gi") && i + 1 < argc) gi_mode = argv[++i]; // off | probes | full (measures)
        // the baked lighting's tests: build it first (the measure waits for it), open / save a level, no ray tracing
        else if (!strcmp(argv[i], "--bake") && i + 1 < argc) bake_mode = argv[++i]; // preview | production
        else if (!strcmp(argv[i], "--open") && i + 1 < argc) open_path = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_path = argv[++i];
        else if (!strcmp(argv[i], "--package") && i + 1 < argc) package_dir = argv[++i]; // Build > Package Game into it
        else if (!strcmp(argv[i], "--package-android") && i + 1 < argc) package_dir = argv[++i], package_android = true; // Build > Package for Android
        else if (!strcmp(argv[i], "--package-play") && i + 1 < argc) package_dir = argv[++i], package_android = package_play = true; // ... for Google Play (AAB)
        else if (!strcmp(argv[i], "--package-quest") && i + 1 < argc) package_dir = argv[++i], package_android = package_quest = true; // (Q25) ... for Meta Quest (VR)
        else if (!strcmp(argv[i], "--android-upload-key")) make_upload_key = true; // Project Settings > Android > Create Upload Key
        else if (!strcmp(argv[i], "--package-icon") && i + 1 < argc) package_icon = argv[++i]; // the packaged game's icon (the test)
        else if (!strcmp(argv[i], "--no-rt")) _putenv("REALENGINE_NO_RT=1");
        else if (!strcmp(argv[i], "--gpu-passes")) _putenv("REALENGINE_GPU_PASSES=1"); // each pass's GPU ms at the measure's end (vkctx_pass_timing)
        else if (!strcmp(argv[i], "--shader-stats")) _putenv("REALENGINE_SHADER_STATS=1"); // the main pass's registers (vkctx.c)
        else if (!strcmp(argv[i], "--no-spill")) no_spill = true, _putenv("REALENGINE_SHADER_STATS=1"); // FAIL: the main pass spills
        else if (!strcmp(argv[i], "--no-ao")) no_ao = true;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = true; // the GI without the surface cache (the probes: comparisons)
        else if (!strcmp(argv[i], "--manual-exposure")) manual_exposure = true;
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) view_name = argv[++i]; // pt | gi: the comparisons
        else if (!strcmp(argv[i], "--aerial-scale") && i + 1 < argc) aerial_scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--stress-objects") && i + 1 < argc) stress_objects = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--stress-lights") && i + 1 < argc) stress_lights = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--stress-textures") && i + 1 < argc) stress_textures = (uint32_t)atol(argv[++i]); // the texture streaming's
        else if (!strcmp(argv[i], "--stress-fly") && i + 1 < argc) stress_fly = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-skip") && i + 1 < argc) shot_skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cam-move") && i + 1 < argc) sscanf(argv[++i], "%f,%f,%f", &cam_move[0], &cam_move[1], &cam_move[2]);
        else if (!strcmp(argv[i], "--scene") && i + 1 < argc) scene_name = argv[++i]; // house: the GI tests' scene
        else if (!strcmp(argv[i], "--constraint-test")) constraint_test = true; // Physics Constraints (constraint_test.c)
        else if (!strcmp(argv[i], "--morph-test")) morph_test = true; // morph targets at run time (morph_test.c)
        else if (!strcmp(argv[i], "--health-test")) health_test = true; // Health and damage: the self-test, then played (health_test.c)
        else if (!strcmp(argv[i], "--inventory-test")) inv_test = true; // Inventory and Items (inventory_test.c)
        else if (!strcmp(argv[i], "--save-test")) save_test = true; // Save and load the progress: the format, then played (save_test.c)
        else if (!strcmp(argv[i], "--interaction-test")) ia_test = true; // Interaction (interaction_test.c)
        else if (!strcmp(argv[i], "--dialogue-test")) dlg_test = true; // Dialogue (dialogue_test.c)
        else if (!strcmp(argv[i], "--quest-test")) qst_test = true; // Quests and objectives (quest_test.c)
        else if (!strcmp(argv[i], "--rules-test")) rules_test = true; // Game Rules (rules_test.c)
        else if (!strcmp(argv[i], "--ability-test")) abil_test = true; // Abilities and effects (ability_test.c)
        else if (!strcmp(argv[i], "--hud-test")) hud_test = true; // HUD modular (hud_test.c)
        else if (!strcmp(argv[i], "--icon-sheet")) icon_sheet = true; // (Q5) every icon in a window over the editor: --shot makes the PNG (test icon_sheet)
        else if (!strcmp(argv[i], "--artist-test")) artist_test = true; // (Q23) the Texture viewer's .hdr, its thumbnail and the viewer following the file, an .exr's reason (artist_test.c)
        else if (!strcmp(argv[i], "--artist-selftest")) return artist_selftest() ? 0 : 1; // (Q23) the .hdr decoder, the textures' kinds, the vertex count
        else if (!strcmp(argv[i], "--mcp")) mcp_flag = true; // (2026-11-24) the MCP server on (rivora_mcp.exe opens the editor with it)
        else if (!strcmp(argv[i], "--mcp-port") && i + 1 < argc) mcp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mcp-test")) mcp_test = mcp_flag = true, mcp_port = mcp_port ? mcp_port : 47871; // (mcp_test.c: a port of its own)
        else if (!strcmp(argv[i], "--project-switch-test")) psw_test = true; // (2026-11-25) editors open across a project switch: closed, the prompt (project_switch_test.c)
        else if (!strcmp(argv[i], "--asset-viewport-test")) avp_test = true; // (Q7) every asset editor's viewport in every view mode, a socket, the cost (asset_viewport_test.c)
        else if (!strcmp(argv[i], "--ui-sweep") && i + 1 < argc) ui_sweep_dir = argv[++i]; // (item 21) every menu, button and window by injected input (ui_sweep.c)
        else if (!strcmp(argv[i], "--dock-test")) dock_test = true; // the modular windows: every docking target, a maximized window closed (dock_test.c)
        else if (!strcmp(argv[i], "--picker-test")) picker_test = true; // (Q6) the asset picker and Create from the selection (picker_test.c)
        else if (!strcmp(argv[i], "--levels-test")) levels_test = true; // sublevels and Lighting Levels (levels_test.c)
        else if (!strcmp(argv[i], "--char-physics-test")) char_test = true; // the Character against physics and platforms (char_test.c)
        else if (!strcmp(argv[i], "--nav-test")) nav_test = true; // the NavMesh: stairs, ramp, a Nav Link, a corridor (nav_test.c)
        else if (!strcmp(argv[i], "--bp-test")) bp_test = true; // Blueprints: made, played, checked (bp_test.c)
        else if (!strcmp(argv[i], "--cs-test")) cs_test = true; // the Construction Script in the editor (bp_test.c)
        else if (!strcmp(argv[i], "--sk-move-test")) sk_move_test = true; // a skeletal mesh moved as the gizmo does (anim.c)
        else if (!strcmp(argv[i], "--source-radius") && i + 1 < argc) source_radius = (float)atof(argv[++i]); // area lights (the shadow filter's test)
        else if (!strcmp(argv[i], "--pause-test")) pause_test = 1; // the pause menu and the options in PIE (pause_test.c)
        else if (!strcmp(argv[i], "--pause-test-reload")) pause_test = 2; // the options saved by --pause-test read back (pause_test.c)
        else if (!strcmp(argv[i], "--umg-test")) umg_test = true; // a Widget Blueprint: made, shown, clicked (umg_test.c)
        else if (!strcmp(argv[i], "--seq-test")) seq_test = true; // a Level Sequence played by a Blueprint in PIE (seq_test.c)
        else if (!strcmp(argv[i], "--cine-test") && i + 1 < argc) cine_test = argv[++i]; // (B9) Take Recorder + Movie Render Queue into this folder (movie_render.c)
        else if (!strcmp(argv[i], "--audio-test")) audio_test = true; // sounds in PIE (audio_test.c)
        else if (!strcmp(argv[i], "--vr-hand-test")) vr_hand_test = true;
        else if (!strcmp(argv[i], "--trace-test")) trace_test = true; // (2026-11-19) the trace nodes in PIE (trace_test.c)
        else if (!strcmp(argv[i], "--trace-bench") && i + 1 < argc) trace_bench_n = (uint32_t)atol(argv[++i]); // N traces a frame while playing
        else if (!strcmp(argv[i], "--script-test")) script_test = true; // the four languages' classes in PIE (script_test.c)
        else if (!strcmp(argv[i], "--ide-demo")) ide_demo = true; // the Code Editor shown with a door script (a picture)
        else if (!strcmp(argv[i], "--assistant-test")) assistant_test = true; // the Assistant's base and answers (assistant_test.c)
        else if (!strcmp(argv[i], "--async-test") && i + 1 < argc) async_test = true, async_test_set_source(argv[++i]); // background work while drawing (async_test.c)
        else if (!strcmp(argv[i], "--view2") && i + 1 < argc) view2_arg = argv[++i]; // Viewport 2 open: persp | top | front | side [:mode]
        else if (!strcmp(argv[i], "--bp-idle")) bp_test = true, ed_bp_idle = true; // the same, the Character standing (flicker)
        else if (!strcmp(argv[i], "--bp-walkview")) bp_test = true, ed_bp_walkview = true; // it walks, seen from a still camera
        else if (!strcmp(argv[i], "--play")) play = true; // Play in Editor right away (the game's view: the flicker tests)
        else if (!strcmp(argv[i], "--play-eject")) play = play_eject = true; // ... ejected (F8): the game runs, seen from --cam
        else if (!strcmp(argv[i], "--game-view")) game_view_cli = true; // the viewport's Game View (G: no icons; screenshots)
        else if (!strcmp(argv[i], "--debug-viewer")) dv_set_open(true); // the Debug Viewer open (its test, screenshots)
        else if (!strcmp(argv[i], "--bp-package") && i + 1 < argc) bp_package = argv[++i]; // with --bp-test: the game too
        else if (!strcmp(argv[i], "--ingame-test") && i + 1 < argc) ingame_test = argv[++i]; // (Q16, ingame_test.c)
        else if (!strcmp(argv[i], "--ingame-package") && i + 1 < argc) ingame_package = argv[++i];
        else if (!strcmp(argv[i], "--open-asset") && i + 1 < argc) open_asset = argv[++i]; // a Blueprint's editor ("/Content/...")
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) cli_lang = argv[++i];
        else if (!strcmp(argv[i], "--project-settings")) cli_project_settings = true;
        else if (!strcmp(argv[i], "--import-folder") && i + 1 < argc) import_tlend = argv[++i], import_folder = true;
        else if (!strcmp(argv[i], "--mesh-socket-test")) mesh_socket_test = true;
        else if (!strcmp(argv[i], "--rig-fit-test")) rig_fit_test = true;
        else if (!strcmp(argv[i], "--mat-live-test")) mat_live_test = true;
        else if (!strcmp(argv[i], "--mat-param-test")) mat_param_test = true;
        else if (!strcmp(argv[i], "--class-split-check")) class_split_check = true;
        else if ((!strcmp(argv[i], "--import-tlend") || !strcmp(argv[i], "--import")) && i + 1 < argc) import_tlend = argv[++i]; // a Tlender scene (or an OBJ, a glTF) into /Content/Imported
        else if (!strcmp(argv[i], "--import-budget") && i + 1 < argc) import_budget_ms = atof(argv[++i]); // (Q19) ms
        else if (!strcmp(argv[i], "--open-viewport") && i + 1 < argc) open_asset = argv[++i], open_viewport = true; // ... on its Viewport
        else if (!strcmp(argv[i], "--browse") && i + 1 < argc) browse = argv[++i]; // the Content Drawer on an asset (screenshots)
        else if (!strcmp(argv[i], "--hover")) browse_hover = true; // ... drawn as under the pointer (an animation plays)
        else if (!strcmp(argv[i], "--anim-sheet") && i + 2 < argc) sheet_anim = argv[++i], sheet_png = argv[++i]; // an animation's frames in a PNG
        else if (!strcmp(argv[i], "--sheet-yaw") && i + 1 < argc) sheet_yaw = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--mm-editor") && i + 1 < argc) mm_editor = argv[++i]; // an ABP's Motion Matching editor (Query Debugger)
        else if (!strcmp(argv[i], "--blend-space") && i + 1 < argc) blend_space = argv[++i]; // an ABP's first blend space in its editor
        else if (!strcmp(argv[i], "--transition-rule") && i + 1 < argc) transition_rule = argv[++i]; // an ABP's first transition's rule window
        else if (!strcmp(argv[i], "--bt-blackboard")) bt_blackboard = true; // with --open-asset of a .rbt: its Blackboard mode
        else if (!strcmp(argv[i], "--sky") && i + 1 < argc) sky_preset = atoi(argv[++i]); // a sky preset (editor.h)
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) cam_pitch = (float)atof(argv[++i]); // the camera's (screenshots)
        else if (!strcmp(argv[i], "--cam") && i + 1 < argc) cam_spec = argv[++i]; // x,y,z,yaw,pitch (screenshots)
        else if (!strcmp(argv[i], "--ortho") && i + 1 < argc) ortho_name = argv[++i]; // an orthographic view (screenshots, the test)
        else if (!strcmp(argv[i], "--shadow-lights") && i + 1 < argc) shadow_lights = (uint32_t)atol(argv[++i]); // the local shadow maps' measures
        else if (!strcmp(argv[i], "--stress-cast") && i + 1 < argc) stress_cast = (float)atof(argv[++i]); // the share casting shadows
        else if (!strcmp(argv[i], "--project") && i + 1 < argc) project_path = argv[++i]; // a .rproject opened
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i]; // the window into a PNG (the engine's own screenshot)
        else if (!strcmp(argv[i], "--anim-selftest")) return anim_selftest() ? 0 : 1; // the blend spaces' and IK's math
        else if (!strcmp(argv[i], "--ar-test")) return ar_selftest() ? 1 : 0; // AR's light and color matching (ar.c)
        else if (!strcmp(argv[i], "--arcore-test")) return arcore_selftest() ? 1 : 0; // ARCore's simulated session (ar_arcore.c)
        else if (!strcmp(argv[i], "--cloth-mesh-test")) return cloth_mesh_selftest() ? 1 : 0; // a mesh's cloth graph (cloth_mesh.c)
        else if (!strcmp(argv[i], "--softbody-test")) return soft_selftest() ? 1 : 0; // (Q9) a soft body's lattice from a mesh (softbody.c)
        else if (!strcmp(argv[i], "--delight-test")) return delight_selftest() ? 1 : 0; // (A9.5e) the baked light out of a lit checker
        else if (!strcmp(argv[i], "--water-test")) return water_selftest() ? 1 : 0; // the ocean's spectrum and its heights for the boats (water.c)
        else if (!strcmp(argv[i], "--graph-layout-test")) { // (2026-11-23) Organize Nodes' layout
            extern int graph_layout_selftest(void);
            return graph_layout_selftest();
        } else if (!strcmp(argv[i], "--mat-autolink-test")) { // (2026-11-23) texture names to maps, a drop on a Principled BSDF
            extern int mat_autolink_selftest(void);
            return mat_autolink_selftest();
        } else if (!strcmp(argv[i], "--sq-test")) { // (2026-11-19) the scene queries: exact shapes, multi, 200 000 traces a batch
            extern int sq_selftest(void);
            return sq_selftest();
        } else if (!strcmp(argv[i], "--tags-test")) return gt_selftest() ? 1 : 0; // Gameplay Tags: matching, add / remove (gameplay_tags.c)
        else if (!strcmp(argv[i], "--bp-debug-test")) bp_dbg_test = true; // the Blueprint debugger: stop, inspect, step, continue (bp_debug_test.c)
        else if (!strcmp(argv[i], "--udim-test")) { // (Q18) UDIM: names, import, codecs, versions (no GPU)
            extern int udim_selftest(void);
            return udim_selftest() ? 1 : 0;
        } else if (!strcmp(argv[i], "--udim-import-test")) { // (Q18) an OBJ naming a UDIM tile: the material reads one .rudim
            extern int udim_import_selftest(void);
            return udim_import_selftest() ? 1 : 0;
        } else if (!strcmp(argv[i], "--skin-lods-test") && i + 1 < argc) { // (2026-11-24, item 4) a skin's LODs made from its file, no GPU
            MeshBuilt mb;
            if (!rmesh_read(argv[i + 1], &mb, 1u << 26, 1u << 28)) return printf("[RealEngine] FAIL: skin lods: %s unreadable\n", argv[i + 1]), 1;
            uint32_t t0 = mb.lods.index_count[0] / 3u;
            uint32_t small_isl = 0, isl = mesh_islands(&mb, 64u, &small_isl); // (2026-11-25: loose pieces lock its simplification)
            printf("[RealEngine] skin lods: %u vertices, %u triangles, %u islands by position (%u under 64 triangles)\n", mb.nv, t0, isl, small_isl);
            { // (2026-11-25) its cluster LOD DAG: the cut's triangles at a few errors (cm), what the GPU would draw
                MeshDagCluster* dc;
                uint32_t *dp, *di, dni;
                uint32_t nd = mesh_built_dag(&mb, &dc, &dp, &di, &dni);
                static const float cut_err[8] = { 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 100.0f, 1000.0f };
                printf("[RealEngine] skin lods: DAG %u clusters, its cut:", nd);
                for (int q = 0; q < 8 && nd; q++) {
                    uint64_t tri = 0;
                    for (uint32_t c = 0; c < nd; c++)
                        if (dc[c].self_err <= cut_err[q] && dc[c].parent_err > cut_err[q]) tri += dc[c].count / 3u;
                    printf(" %.1f cm %llu,", cut_err[q], (unsigned long long)tri);
                }
                printf("\n");
                if (nd) free(dc), free(dp), free(di);
            }
            bool made = mesh_skinned_lods(&mb);
            printf("[RealEngine] skin lods: %s: %s, %u LODs:", argv[i + 1], made ? "made" : "NOT made", mb.lods.count);
            for (uint32_t l = 0; l < mb.lods.count; l++) printf(" %u (err %.2f cm)", mb.lods.index_count[l] / 3u, mb.lods.error[l]);
            printf(" from %u triangles\n", t0);
            for (uint32_t l = 1; l < mb.lods.count; l++) { // (2026-11-25) the real one-sided error: LOD 0's vertices to the LOD's surface
                float mx = 0.0f, sum = 0.0f;
                double d = mesh_lod_true_error(&mb, l, &mx, &sum);
                printf("[RealEngine] skin lods: LOD %u measured: max %.2f cm, mean %.2f cm, 99%% under %.2f cm\n", l, mx, sum, (float)d);
            }
            mesh_built_free(&mb);
            return made ? 0 : 1;
} else if (!strcmp(argv[i], "--mat-pins-test")) return mat_pins_selftest() ? 1 : 0; // the inputs greyed "no effect yet" are the ones not compiled
        else if (!strcmp(argv[i], "--vehicle-mesh-test")) vehicle_mesh_test = true; // a vehicle from an "imported" car's meshes (vehicle_build.inc)
        else if (!strcmp(argv[i], "--update-parse-test")) return update_parse_selftest(); // (2026-11-25) the list and the feed, offline
        else if (!strcmp(argv[i], "--update-test")) return update_selftest(); // the real latest release: API path, retries, signature (updater.c)
        else if (!strcmp(argv[i], "--net-test")) return net_selftest(); // the network protocol under emulated lag and loss (net_test.c)
        else if (!strcmp(argv[i], "--steam-test")) return steam_selftest(); // Steam read-only with the App ID 480 (net_session_test.c)
        else if (!strcmp(argv[i], "--net-host-test")) net_host_test = true; // hosts and starts a client (net_session_test.c)
        else if (!strcmp(argv[i], "--net-adv")) net_session_set_advanced(true); // ... the advanced sessions' steps (password, filters, bans)
        else if (!strcmp(argv[i], "--net-join-test") && i + 1 < argc) net_join_test = argv[++i]; // joins that address (the host starts it)
        else if (!strcmp(argv[i], "--pie-client") && i + 1 < argc) pie_client = argv[++i]; // (B13) another player of the editor's Play: joins it
        else if (!strcmp(argv[i], "--pie-window") && i + 1 < argc) pie_window = atoi(argv[++i]); // (its window's place: 1-3)
        else if (!strcmp(argv[i], "--pie-players") && i + 1 < argc) pie_players = atoi(argv[++i]); // (B13) Play with that many players (a test)
        else if (!strcmp(argv[i], "--net-emu") && i + 1 < argc) { // latency ms, jitter ms, loss % (the network emulation)
            float l = 0, j = 0, p = 0;
            sscanf(argv[++i], "%f,%f,%f", &l, &j, &p);
            net_emu = (NetSim){ l, j, p * 0.01f, 0.0f };
        }
        else if (!strcmp(argv[i], "--new-project") && i + 1 < argc) new_project = argv[++i]; // folder\Name, from --template
        else if (!strcmp(argv[i], "--render-mode") && i + 1 < argc) // performance | quality (sugestão #45): the new project's
            project_set_new_render_mode(!strcmp(argv[++i], "quality") ? PROJECT_RENDER_QUALITY : PROJECT_RENDER_PERFORMANCE);
        else if (!strcmp(argv[i], "--template") && i + 1 < argc) template_id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--project-browser")) show_project_browser = true;
        else if (!strcmp(argv[i], "--browser-open") && i + 1 < argc) browser_open = argv[++i], show_project_browser = true;
        else if (!strcmp(argv[i], "--scalability-mem-test")) scal_mem_test = true;
        else if (!strcmp(argv[i], "--vr-preview")) vr_preview_now = true;
        else if (!strcmp(argv[i], "--place-hitch-test") && i + 2 < argc) place_hitch_name = argv[++i], place_hitch_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--hang-test")) hang_test = true; // a frame frozen 7.5 s: the watchdog's report (crashlog.h)
        else if (!strcmp(argv[i], "--no-intro")) g_brand_intro_on = false; // straight to the loading screen
        else if (!strcmp(argv[i], "--intro")) force_intro = true;
        else if (!strcmp(argv[i], "--scalability") && i + 1 < argc) scalability_cli = atoi(argv[++i]); // 0 Low .. 3 Epic (measures)
        else if (!strcmp(argv[i], "--quit-after") && i + 1 < argc) quit_after = atoi(argv[++i]), quit_test = quit_after > 0;
        else if (!strcmp(argv[i], "--cvar") && i + 1 < argc && ncvar_cli < 16) cvar_cli[ncvar_cli++] = argv[++i]; // (a measure's p.Rate=60)
        else if (!strcmp(argv[i], "--sun") && i + 1 < argc) sun_spec = argv[++i]; // pitch,yaw of the first directional light (not saved)
        else if (!strcmp(argv[i], "--add-floor") && i + 1 < argc) add_floor = (float)atof(argv[++i]); // a floor (templates') of N scale, not saved
        else if (!strcmp(argv[i], "--bp-menu-test") && i + 1 < argc) bp_menu_test = argv[++i]; // the graph menu over a Blueprint (after the editor is up)
        else if (!strcmp(argv[i], "--particles-test")) return particles_selftest() ? 1 : 0; // the particles' births, stacks, templates
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i]; // (B14b) every frame recorded into this .json
        else if (!strcmp(argv[i], "--trace-open") && i + 1 < argc) trace_open = argv[++i]; // (B14b) the Trace window with this file (a test)
        else if (!strcmp(argv[i], "--cine-cam-test")) cine_cam_test = true; // (A9.4) a Cine Camera's lens and exposure in Play
        else if (!strcmp(argv[i], "--ssr-test")) ssr_test = true; // (A4c) the default level's floor a mirror
        else if (!strcmp(argv[i], "--lut-test")) lut_test = true; // (A9.4) an inverting Color Grading LUT in an unbound volume
        else if (!strcmp(argv[i], "--plugin-test")) plugin_test = true; // (B14c) the example plugin's nodes in a script, in Play
        else if (!strcmp(argv[i], "--plugin-new-test") && i + 1 < argc) { // (B14c) New Plugin's template built and loaded
            extern int plugins_template_test(const char* dir);
            return plugins_template_test(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sc-test")) return sc_selftest() ? 1 : 0; // (B14) Git: a temporary repository's states, a submit, a change
        else if (!strcmp(argv[i], "--procgen-test")) return procgen_selftest() ? 1 : 0; // the noise far out, the biomes, a facility's rooms
        else if (!strcmp(argv[i], "--touch-test")) return ui_touch_selftest() ? 1 : 0;
        else if (!strcmp(argv[i], "--vr-test")) return vr_pawn_selftest() ? 1 : 0; // (Q25) the VR pawn with the simulated headset (vr_pawn.c)
        else if (!strcmp(argv[i], "--mr-test")) return mr_selftest() ? 1 : 0; // (Q25b) mixed reality's stand-in room, actors, traces (xr_mr.c)
        else if (!strcmp(argv[i], "--mobile-test")) return mobile_selftest() ? 1 : 0; // the device tiers, the dynamic resolution // the touch controls with fingers made up
        else if (!strcmp(argv[i], "--vehicle-test")) return vehicle_selftest() ? 1 : 0; // a car, a boat, an airplane, a helicopter and a train on a flat floor
        else if (!strcmp(argv[i], "--weather-field-test")) return wfield_selftest() ? 1 : 0; // (Q27.6)
        else if (!strcmp(argv[i], "--ik-test") && i + 1 < argc && argv[i + 1][0] != '-') return selftest_guard(), ikphys_selftest_named(argv[i + 1]) ? 1 : 0; // (Q10) ragdoll_limits, ragdoll_active, dyn_bones
        else if (!strcmp(argv[i], "--spline-test") && i + 1 < argc && argv[i + 1][0] != '-') return spline_selftest_named(argv[i + 1]) ? 0 : 1; // (Q14) spline_eval, spline_rail_river, cable_hang
        else if (!strcmp(argv[i], "--fpcam-test") && i + 1 < argc && argv[i + 1][0] != '-') return fpcam_selftest_named(argv[i + 1]) ? 0 : 1; // (Q12) fp_bob_sync, camshake_determinism, fp_autofocus, fp_land_recoil
        else if (!strcmp(argv[i], "--rig-test")) return rig_detect_selftest() ? 0 : 1; // (2026-11-23) Mixamo, Rigify, UE, no names, CC
        else if (!strcmp(argv[i], "--preload-test") && i + 1 < argc) return level_preload_selftest(argv[i + 1]) ? 0 : 1; // (2026-11-23)
        else if (!strcmp(argv[i], "--loc-test") && i + 1 < argc) return loc_selftest(argv[i + 1]) ? 0 : 1; // (2026-11-24, loc.h)
        else if (!strcmp(argv[i], "--i18n-dump") && i + 1 < argc) { // (2026-11-25, item 12) the tables' texts, for tools/i18n_audit.py
            extern int i18n_dump(const char* path);
            return i18n_dump(argv[i + 1]);
        }
        else if (!strcmp(argv[i], "--ik-test")) return selftest_guard(), (ikphys_selftest() + ikrig_editor_selftest()) ? 1 : 0; // IK chains, limits, a ragdoll falling
        else if (!strcmp(argv[i], "--graph-menu-test")) return gm_selftest() ? 1 : 0; // the graph menus' recents and recommendations
        else if (!strcmp(argv[i], "--pcg-test")) return pcg_selftest() ? 1 : 0; // (Q15) PCG: determinism, cells, 100k points, the files (pcg_test.c)
        else if (!strcmp(argv[i], "--reimport") && i + 1 < argc) reimport = argv[++i]; // "/Content/.../SK_x.rskm": Reimport (in the background)
        else if (!strcmp(argv[i], "--list-actors")) list_actors = true; // the level's actors (name, place, size) on the log
        else if (!strcmp(argv[i], "--splash-file") && i + 1 < argc) splash_file = argv[++i]; // a picture or video for it (tests)
        else if (!strcmp(argv[i], "--splash-hold") && i + 1 < argc) splash_hold = (float)atof(argv[++i]), force_intro = true; // the loading screen held (screenshots)           // the intro even with a test's flags (screenshots)
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme_cli = argv[++i]; // a theme by name for this run (screenshots, tests)
        else if (!strcmp(argv[i], "--layout-default")) layout_default = true; // (Q26) the default layout with all its panels (screenshots, tests)
        else if (!strcmp(argv[i], "--look-test") && i + 1 < argc) { // (Q26, the test look_migration) %APPDATA% in this folder: a 0.43 editor.ini
            const char* d = argv[++i];                                 // on the old default theme and its last layout, then the new look's migration
            char p[900];
            snprintf(p, sizeof p, "%s\\RealEngine\\Layouts", d);
            re_make_dirs(p);
            snprintf(p, sizeof p, "%s\\RealEngine\\editor.ini", d);
            FILE* f = re_fopen(p, "wb");
            if (f) fputs("language=pt\ntheme=\xc3\x93rbita Escuro\nshow_stats=0\n", f), fclose(f);
            snprintf(p, sizeof p, "%s\\RealEngine\\Layouts\\Last.rlayout", d);
            f = re_fopen(p, "wb");
            if (f) fputs("; a 0.43 layout\n[RealEngine][Panels]\nplace=0\noutliner=1\ndetails=1\ncontent=0\nlevels=1\nlog=0\n\n", f), fclose(f);
            snprintf(p, sizeof p, "%s\\RealEngine\\Layouts\\Antes do Azul Real.rlayout", d);
            re_remove_file(p);
            _putenv_s("APPDATA", d);
            g_look_test = true;
        }
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show_window = argv[++i]; // prefs | settings | games | assistant:<article> (screenshots)
    }

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // REALENGINE_BACKGROUND=1 (the test suites while someone uses the PC): the window as big as a maximized one, but
    // left of every monitor, never focused: nothing pops over what the user does (the frames still render and measure)
    const char* bgenv = getenv("REALENGINE_BACKGROUND");
    bool background = bgenv && bgenv[0] == '1';
    int bw = 1600, bh = 900;
    // REALENGINE_MONITOR=N (the visible suites, tools/suite.ps1 -Visivel: the user's 2nd monitor): the background mode's
    // window (the primary's size: the same measures, never focused) shown at that display's corner instead
    int mon_x = 0, mon_y = 0;
    bool on_monitor = !background && re_test_monitor(&mon_x, &mon_y, NULL, NULL);
    if (background || (on_monitor && !pie_client)) {
        int wx, wy;
        GLFWmonitor* pm = glfwGetPrimaryMonitor();
        if (pm) glfwGetMonitorWorkarea(pm, &wx, &wy, &bw, &bh);
        bh -= 23; // (the title bar a maximized window loses: the same client size the measures had)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    } else if (!pie_client) glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE); // like Unreal: the editor opens maximized
    if (pie_client && !background) bw = 1280, bh = 720; // (B13: another player's window, beside the editor)
    // (2026-11-23, the user: "the opening like Blender's, a transparent background") the intro plays from here in a
    // window of its own over the desktop (brand.c) while the engine starts; the main window is shown once it ends
    bool automated = bench.cli || pause_test || bp_test || char_test || constraint_test || nav_test || morph_test || health_test || inv_test || save_test || ia_test || dlg_test || qst_test || rules_test || abil_test || hud_test || levels_test || dock_test || ui_sweep_dir || picker_test || avp_test || psw_test || artist_test || bp_dbg_test || char_test || constraint_test || nav_test || morph_test || health_test || inv_test || save_test || ia_test || dlg_test || umg_test || seq_test || cine_test || pie_client || audio_test || script_test || trace_test || vr_hand_test || async_test || assistant_test || mcp_test || package_dir || import_tlend || crash_kind_env();
    if (automated && !force_intro) g_brand_intro_on = false;
    int imx = 0, imy = 0, imw = 0, imh = 0;
    if (on_monitor) re_test_monitor(&imx, &imy, &imw, &imh);
    bool intro_hidden = !pie_client && brand_intro_start(background, imx, imy, imw, imh) && !background && !on_monitor;
    if (intro_hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(bw, bh, pie_client ? "Rivora Engine - Client" : "Rivora Engine", NULL, NULL);
    if (!window) return 1;
    if (intro_hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    // (Q2) the hints stay for every window created after: the windows dragged out of the editor (ImGui's viewports) were
    // born maximized, kept at their size by ImGui, and the X in the screen's corner was the main window's: it closed the
    // engine (the user's report, 17/11). Back to the defaults for them.
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);
    if (pie_client && !background) glfwSetWindowPos(window, mon_x + 60 + 120 * pie_window, mon_y + 60 + 90 * pie_window);
    if (on_monitor && !pie_client) {
        glfwSetWindowPos(window, mon_x + 8, mon_y + 31); // (the title bar on the display)
        glfwShowWindow(window);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE), glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE), glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
    }
    if (background) {
        glfwSetWindowPos(window, GetSystemMetrics(SM_XVIRTUALSCREEN) - bw - 200, GetSystemMetrics(SM_YVIRTUALSCREEN) + 40);
        glfwShowWindow(window);
    }

    static VkCtx vk; // big (swapchain arrays): not on the stack
    g_vk = &vk;
    vk.vsync = vsync;
    if (!vkctx_init(&vk, window, validation)) {
        MessageBoxA(NULL, "Rivora Engine needs a GPU with Vulkan 1.3 (dynamic rendering, synchronization2, timeline "
                          "semaphores). Update the graphics driver.", "Rivora Engine", MB_ICONERROR);
        return 1;
    }
    // Dear ImGui (through cimgui), rendering with dynamic rendering into the swapchain's format
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float dpi = xscale > 0.5f ? xscale : 1.0f;
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->IniFilename = NULL;
    io->ConfigWindowsMoveFromTitleBarOnly = true;
    io->ConfigWindowsResizeFromEdges = true; // (every edge and corner resizes a window, as in Unreal)
    // docking (the asset editors' tabs dragged out, docked again) and multi-viewport (a window dragged out of the editor
    // becomes a window of its own: another monitor)
    io->ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    // (REALENGINE_MONITOR too: the window wider than the 2nd display, ImGui's windows of their own born past its edge
    // rendered into swapchain images never acquired, 2 validation errors in debug_viewer)
    if ((background || on_monitor) && !dock_test) io->ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable; // (the dock test: its window of its own goes offscreen too) // (the background mode: no window of its own pops up on the screen)
    io->ConfigDragClickToInputText = true; // (Blender's fields: a click types the value, a drag changes it; typed values pass the drag's limits)
    { // the fonts: Inter, Sora and JetBrains Mono (OFL, embedded: theme.c), the code's the monospaced one
        extern struct ImFont* g_code_font;
        theme_init(); // (the themes: the preferences pick one later, editor_load_prefs)
        ui_tips_editor(tip_icon, &g_font_bold); // (Q5: the standard tooltip's icon, bold name and F1 line)
        { // (2026-11-25, item 12) the descriptions in Spanish
            extern const UiTipEs g_ui_tips_es[];
            extern const int g_ui_tip_es_count;
            ui_tips_set_es(g_ui_tips_es, g_ui_tip_es_count);
        }
        theme_load_fonts(15.0f); // (compact, like a production tool's 9 pt)
        g_code_font = g_font_mono;
    }
    apply_theme(dpi);

    ImGui_ImplGlfw_InitForVulkan(window, true);
    static VkFormat color_format;
    color_format = vk.color_format;
    ImGui_ImplVulkan_InitInfo init;
    memset(&init, 0, sizeof init);
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = vk.instance;
    init.PhysicalDevice = vk.gpu;
    init.Device = vk.device;
    init.QueueFamily = vk.queue_family;
    init.Queue = vk.queue;
    init.PipelineCache = vk.pipeline_cache;
    init.DescriptorPoolSize = UI_TEXTURES_MAX; // (the thumbnails, the materials, the previews: asset_viewer.h)
    init.MinImageCount = vk.min_image_count >= 2 ? vk.min_image_count : 2;
    init.ImageCount = vk.image_count >= init.MinImageCount ? vk.image_count : init.MinImageCount;
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT; // (the windows out of the editor: the same format, the same colors)
    init.PipelineInfoForViewports.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoForViewports.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoForViewports.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init);
    // (2026-11-25, item 9) the windows dragged out of the editor: owned by this one (above it, no taskbar button of their
    // own, closed with it), presented in mailbox (measured: FIFO held the whole editor at the refresh rate), in SDR (ui_viewports.h)
    ui_viewports_install(window, vk.instance, vk.gpu, vk.device, vk.queue_family, init.MinImageCount, UIVP_PRESENT_MAILBOX);
    vkctx_pipeline_cache_save(&vk); // what the driver compiled for this start (a crash later keeps it)
    uint32_t imgui_min_images = init.MinImageCount;

    // the intro (what the engine is, 3.4 s: a click skips it), then the loading screen while the engine starts
    brand_init(&vk);
    progress_set_vk(&vk); // (a save's picture of the screen: save_progress.h)
    if (automated) g_editor_prefs_readonly = true; // (a test never changes the user's preferences: language, preset...)
    g_brand_splash_on = !automated || force_intro;
    brand_run_intro(&vk, window);
    if (intro_hidden) { // (the intro over: the editor's window, maximized, in front)
        glfwShowWindow(window);
        glfwMaximizeWindow(window);
        glfwFocusWindow(window);
    }
    if (splash_file && !brand_set_splash(&vk, splash_file)) printf("[RealEngine] FAIL: the loading screen could not read %s\n", splash_file);
    brand_splash_begin();
    for (double h0 = re_time_seconds(); re_time_seconds() - h0 < splash_hold;) // (--splash-hold: the bar crawling)
        brand_splash_frame(&vk, (float)((re_time_seconds() - h0) / splash_hold) * 0.6f, TR("Starting the renderer (the first time: compiling the shaders)..."));
    brand_splash_frame(&vk, 0.04f, TR("Starting the renderer (the first time: compiling the shaders)..."));
    re_arena_init(&g_frame_arena, (size_t)256 << 20);
    render_set_wait_pump(glfwPollEvents); // (the pipelines compiled on threads: the window answers meanwhile)
    if (!render_init(&vk)) {
        MessageBoxA(NULL, "Rivora Engine could not create its renderer (GPU memory or features).", "Rivora Engine", MB_ICONERROR);
        return 1;
    }
    brand_splash_frame(&vk, 0.45f, TR("Starting the editor..."));


    static Editor ed;
    if (!editor_init(&ed, window, &vk)) return 1;
    sq_register_debug(); // (the Debug Viewer's Scene Queries)
    render_prewarm(&vk, ed.msaa); // (2026-11-19: a cold cache compiled behind the intro and the Project Browser)
    g_load_progress = splash_progress; // a project's load shows on the loading screen
    g_load_splash = splash_picture;
    brand_splash_frame(&vk, project_path ? 0.55f : 1.0f, TR("Ready."));
    g_editor = &ed;
    if (mcp_flag || !automated) mcp_apply_prefs(mcp_flag, mcp_port); // (a test never starts it from the user's preferences)
    if (theme_cli && !theme_select_name(theme_cli)) printf("[RealEngine] FAIL: no theme named %s\n", theme_cli);
    if (layout_default) dock_layout_default(&ed); // (Q26: every panel of the default layout, an automated run too)
    services_init(&ed);
    int exit_code = 0;
    if (bench.cli) project_recent_disable();
    ed.show_browser = (argc == 1 && ed.browser_on_start) || show_project_browser; // Unreal's, before the editor
    if (show_window) {
        ed.show_prefs = !strcmp(show_window, "prefs") || !strcmp(show_window, "prefs:graph") || !strcmp(show_window, "prefs:appearance");
        if (!strcmp(show_window, "prefs:graph")) project_ui_pref_page(5); // (Graph Style: its gallery, a screenshot)
        if (!strcmp(show_window, "prefs:appearance")) project_ui_pref_page(6); // (Appearance: the themes, the Theme Editor)
        ed.show_project_settings = !strcmp(show_window, "settings");
        ed.show_place = !strcmp(show_window, "place");
        ed.show_package = !strcmp(show_window, "package");
        if (!strcmp(show_window, "games")) ed.show_browser = true, project_ui_show_games();
        if (!strncmp(show_window, "assistant:", 10)) assistant_ui_show_article(show_window + 10);
        // (2026-11-25, the tester's photos) loc: Tools > Localization Dashboard; plugins: Edit > Plugins (bp-pin / bp-var:
        // after the project opens, below)
        if (!strcmp(show_window, "loc")) *loc_ui_show_flag() = true;
        if (!strcmp(show_window, "plugins")) editor_show_plugins();
    }
    if (project_path && !editor_open_project(&ed, project_path, -1)) {
        printf("[RealEngine] FAIL: could not open the project %s\n", project_path);
        exit_code = 1;
    }
    // (2026-11-25, the tester's photos) --show bp-pin:<vpath>[:<pin>]: the Blueprint's Event Graph with a struct pin's menu
    // open (Split Struct Pin); bp-var:<vpath>:<var>: the variable in the Details with its value's asset picker open (a
    // Level variable: the levels). After the project opened (its switch closes the editors); with --bench and --shot
    if (show_window && (!strncmp(show_window, "bp-pin:", 7) || !strncmp(show_window, "bp-var:", 7))) {
        char vp[600], subb[200], *sub;
        snprintf(vp, sizeof vp, "%s", show_window + 7);
        sub = strlen(vp) > 9 ? strchr(vp + 9, ':') : NULL; // (after "/Content/")
        if (sub) *sub++ = 0, snprintf(subb, sizeof subb, "%s", sub), sub = subb; // (its own: ".rbp" may go where it was)
        size_t vn = strlen(vp); // (the vpath with or without its ".rbp": /Content/BP_Hero or /Content/BP_Hero.rbp)
        const char* dot = strrchr(vp, '.');
        if ((!dot || strchr(dot, '/')) && vn + 4 < sizeof vp) memcpy(vp + vn, ".rbp", 5);
        if (show_window[3] == 'p') bp_editor_show(vp, sub ? sub : "", NULL); // ("": its first splittable pin)
        else bp_editor_show(vp, NULL, sub ? sub : "");
    }
    if (!ed.show_browser) plugins_init(NULL, true); // (B14c) no project opened: the engine's plugins (a project opened later adds its own)
    if (!ed.show_browser) wait_shaders_splash(&vk, 0.6f); // (the Project Browser: they go on compiling behind it)
    // (2026-11-25, bug-fixer) the gamepads asked once here, behind the splash: GLFW starts its joysticks (DirectInput, XInput)
    // on the first ask, ~110 ms, which was the Play's first frame (the pause menu's Start button: play_hitch 188 ms)
    glfwJoystickPresent(GLFW_JOYSTICK_1);
    brand_splash_end(&vk);
    if (new_project) {
        char folder[600];
        snprintf(folder, sizeof folder, "%s", new_project);
        char* s = strrchr(folder, '\\');
        char* f = strrchr(folder, '/');
        if (f > s) s = f;
        const char* name = s ? s + 1 : folder;
        if (s) *s = 0;
        if (!s || !project_create(&ed, folder, name, template_id)) {
            printf("[RealEngine] FAIL: could not create the project %s\n", new_project);
            exit_code = 1;
        } else {
            printf("[RealEngine] Project %s created (template %d).\n", new_project, template_id);
        }
        // (an import, a save or a package goes into it: opened now, not between frames; game_first_run packages a blank
        // project, not the build's Content that the other tests fill; the texture stress writes its 401 assets there)
        bool heavy = scene_name && (!strcmp(scene_name, "heavy") || !strcmp(scene_name, "rocks") || !strcmp(scene_name, "rocks_tess") || !strcmp(scene_name, "tess_seams") || !strcmp(scene_name, "glass") || !strcmp(scene_name, "masked_lobes") || !strcmp(scene_name, "haunted") || !strcmp(scene_name, "metro") || !strcmp(scene_name, "cloth_mesh") || !strcmp(scene_name, "sofa") || !strcmp(scene_name, "softbody") || !strcmp(scene_name, "softbody_rigid") || !strcmp(scene_name, "physics_shapes") || !strcmp(scene_name, "lightfn") || !strcmp(scene_name, "interiors")); // (Q4: lightfn, interiors) (their assets into the project, never the build's Content)
        heavy = heavy || (scene_name && !strncmp(scene_name, "udim", 4)); // (Q18: its assets into the project)
        heavy = heavy || (scene_name && !strncmp(scene_name, "vista", 5)); // (Q21: its assets into the project)
        heavy = heavy || (scene_name && !strncmp(scene_name, "emit", 4)); // (2026-11-23: its material into the project)
    heavy = heavy || (scene_name && !strcmp(scene_name, "firstframe")); // (2026-11-24: its materials into the project)
        if ((import_tlend || save_path || package_dir || stress_textures || heavy || morph_test || inv_test || save_test || ia_test || dlg_test || qst_test || rules_test || abil_test || hud_test || levels_test || picker_test || vr_hand_test) && ed.pending_project[0]) {
            char path[600];
            snprintf(path, sizeof path, "%s", ed.pending_project);
            ed.pending_project[0] = 0;
            if (!editor_open_project(&ed, path, ed.pending_tpl)) exit_code = 1;
        }
    }
    if (gi_mode) {
        ed.gi.probes = strcmp(gi_mode, "off") != 0;
        bool full = strcmp(gi_mode, "full") == 0; // also: "shadows", "reflections" (one ray traced feature each)
        ed.gi.reflections = full || strcmp(gi_mode, "reflections") == 0;
        ed.gi.rt_shadows = full || strcmp(gi_mode, "shadows") == 0;
    }
    // (only for this run: a test's --scalability 3 was saved and every later test ran on Epic)
    if (scalability_cli >= 0) ed.saved_scalability = ed.scalability, editor_set_scalability(&ed, scalability_cli, 0.0);
    for (int i = 0; i < ncvar_cli; i++) { // (the project's values in memory, for this run; Play restores what changes while playing)
        char line[160], out[200];
        snprintf(line, sizeof line, "%s", cvar_cli[i]);
        char* eq = strchr(line, '=');
        if (eq) *eq = ' ';
        bool ok = cvar_exec(line, out, sizeof out);
        printf("[RealEngine] Console: %s%s\n", out, ok ? "" : " (FAIL)");
    }
    // --upscale-mode: the render's share as an upscaler's mode names it, over the preset's (upscale.h; a measure)
    float ups_top = ed.gi.tsr_scale;
    if (upscale_mode >= 0 && ed.gi.tsr_scale > 0.0f) ed.gi.tsr_scale = ups_top = upscale_mode_scale(upscale_mode, ed.gi.tsr_scale);
    if (upscale_mode >= 0) printf("[RealEngine] Upscaling: %s, the render at %.0f%%\n", upscale_mode_name(upscale_mode), ed.gi.tsr_scale * 100.0f);
    UpsDynamic ups_dyn = { 0 };
    if (no_ao) ed.gi.contact_ao = false;
    if (no_cache) ed.gi.surface_cache = false;
    if (manual_exposure) ed.auto_exposure = false;
    if (view_name) {
        static const struct { const char* name; ViewMode mode; } views[] = {
            { "pt", VIEW_PATH_TRACING }, { "gi", VIEW_GI_ONLY }, { "random", VIEW_SOLID_RANDOM }, { "normals", VIEW_NORMALS },
            { "lod", VIEW_LOD }, { "tris", VIEW_TRIANGLES }, { "mobility", VIEW_MOBILITY }, { "cache", VIEW_SURFACE_CACHE },
            { "gisrc", VIEW_GI_SOURCE }, { "density", VIEW_TEXEL_DENSITY }, { "lights", VIEW_LIGHT_COMPLEXITY },
            { "cost", VIEW_SHADER_COST }, { "objcost", VIEW_OBJECT_COST }, { "unlit", VIEW_UNLIT }, { "solid", VIEW_SOLID }, { "texture", VIEW_TEXTURE },
        };
        ed.view_mode = VIEW_LIT;
        for (size_t k = 0; k < sizeof views / sizeof views[0]; k++)
            if (!strcmp(view_name, views[k].name)) ed.view_mode = views[k].mode;
    }
    if (aerial_scale >= 0.0f) ed.aerial_scale = aerial_scale;
    if (game_view_cli) ed.game_view = true;
    if (scene_name && !strcmp(scene_name, "firstframe")) mat_first_scene(&ed); // (2026-11-24: mat_first_test.c)
    else if (scene_name && !strcmp(scene_name, "selection")) selection_test_scene(&ed); // (2026-11-25, item 17: selection_test.c)
    else if (scene_name) editor_test_scene(&ed, scene_name);
    if (sky_preset >= 0) editor_sky_preset(&ed, sky_preset, 0.0);
    if (cam_pitch > -999.0f) ed.cam.pitch = cam_pitch;
    if (add_floor > 0.0f) editor_add_test_floor(&ed, add_floor);
    if (sun_spec) // (the tests: a sun of their own over a project's level)
        for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) {
            Entity e = ecs_entity_at(&ed.world, s2);
            const CLight* li = e ? ecs_get(&ed.world, e, COMP_LIGHT) : NULL;
            CTransform* t = li && li->type == LIGHT_DIRECTIONAL ? ecs_get(&ed.world, e, COMP_TRANSFORM) : NULL;
            if (!t) continue;
            sscanf(sun_spec, "%f,%f", &t->rotation[0], &t->rotation[1]);
            t->rotation[2] = 0.0f;
            break;
        }
    if (cam_spec) sscanf(cam_spec, "%f,%f,%f,%f,%f", &ed.cam.pos[0], &ed.cam.pos[1], &ed.cam.pos[2], &ed.cam.yaw, &ed.cam.pitch);
    else if (bench.cli && project_path) // the project's camera, to repeat a view with --cam
        printf("[RealEngine] Camera: --cam %.0f,%.0f,%.0f,%.1f,%.1f\n", ed.cam.pos[0], ed.cam.pos[1], ed.cam.pos[2], ed.cam.yaw, ed.cam.pitch);
    if (shadow_lights) editor_shadow_lights(&ed, shadow_lights);
    if (stress_objects || stress_lights) {
        double s0 = re_time_seconds();
        editor_stress_scene(&ed, stress_objects, stress_lights, stress_cast);
        printf("[RealEngine] Stress: %u objects, %u lights in %.2f s\n", stress_objects, stress_lights, re_time_seconds() - s0);
    }
    if (stress_textures && !(new_project && exit_code)) // (never into the build's Content: the other tests package it)
        editor_stress_textures(&ed, stress_textures);
    if (open_path && !editor_open_level(&ed, open_path, re_time_seconds())) {
        printf("[RealEngine] FAIL: could not open %s\n", open_path);
        exit_code = 1;
    }
    if (open_path && cam_spec) // the level brings its editor camera: the command line's wins (screenshots)
        sscanf(cam_spec, "%f,%f,%f,%f,%f", &ed.cam.pos[0], &ed.cam.pos[1], &ed.cam.pos[2], &ed.cam.yaw, &ed.cam.pitch);
    if (ortho_name) { // (from the perspective camera: what it looks at)
        static const char* const names[VP_PROJ_COUNT] = { "perspective", "top", "bottom", "left", "right", "front", "back" };
        for (int k = 0; k < VP_PROJ_COUNT; k++)
            if (!strcmp(ortho_name, names[k])) viewport_set_projection(&ed, k);
    }
    double bake_t0 = 0.0;
    if (bake_mode) {
        char why[160];
        if (gi_bake_start(&vk, &ed.world, strcmp(bake_mode, "production") ? BAKE_PREVIEW : BAKE_PRODUCTION, why, sizeof why)) {
            bake_t0 = re_time_seconds();
        } else {
            printf("[RealEngine] FAIL: Build Lighting: %s\n", why);
            exit_code = 1;
        }
    }
    if (bp_menu_test && bp_editor_menu_selftest(bp_menu_test)) exit_code = 1;
    if (import_tlend) { // (Q19: the time it took, against --import-budget)
        double t0 = re_time_seconds();
        if (import_folder ? !content_import_folder(import_tlend, "/Content/Imported", true, true) : !editor_import_tlend(&ed, import_tlend, "/Content/Imported", true)) exit_code = 1;
        double ms = (re_time_seconds() - t0) * 1000.0;
        printf("[RealEngine] Import took %.1f ms (the assets written, the materials compiled and baked, the meshes loaded)\n", ms);
        if (import_budget_ms > 0.0 && ms > import_budget_ms) printf("[RealEngine] FAIL: the import took %.1f ms, over its budget of %.0f ms\n", ms, import_budget_ms), exit_code = 1;
    }
    if (no_skins) { // (2026-11-25, item 4) the level without its Skeletal Mesh Actors: what the scene costs alone
        uint32_t gone = 0;
        for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) {
            Entity e = ecs_entity_at(&ed.world, s2);
            if (e && ecs_get(&ed.world, e, COMP_SKELETAL_MESH)) bp_destroy_actor(&ed.world, e, false), gone++;
        }
        printf("[RealEngine] No skins: %u Skeletal Mesh Actors removed\n", gone);
    }    if (skin_copies > 0) { // (2026-11-24, the Unamed scene's case) the level's first Skeletal Mesh Actor copied on a grid
        CSkeletalMesh src = { 0 };
        CTransform at = { 0 };
        for (uint32_t s2 = 1; s2 < ed.world.slot_count && !src.mesh; s2++) {
            Entity e = ecs_entity_at(&ed.world, s2);
            const CSkeletalMesh* c = e ? ecs_get(&ed.world, e, COMP_SKELETAL_MESH) : NULL;
            const CTransform* t = c && c->mesh ? ecs_get(&ed.world, e, COMP_TRANSFORM) : NULL;
            if (t) src = *c, at = *t;
        }
        // (2026-11-25) spaced as the level's own: the median distance to the nearest neighbor among its Skeletal Mesh Actors
        // (one alone, the tests' rig: 80 cm), the grid beside them (at 80 cm the Unamed's capsules, a few m wide, overlapped)
        float sp = 80.0f, ymax = at.location[1];
        {
            enum { NS = 2048 };
            static float px[NS], py[NS];
            uint32_t np = 0;
            for (uint32_t s2 = 1; s2 < ed.world.slot_count && np < NS; s2++) {
                Entity e = ecs_entity_at(&ed.world, s2);
                const CSkeletalMesh* c = e ? ecs_get(&ed.world, e, COMP_SKELETAL_MESH) : NULL;
                const CTransform* t = c && c->mesh ? ecs_get(&ed.world, e, COMP_TRANSFORM) : NULL;
                if (!t) continue;
                px[np] = t->location[0], py[np] = t->location[1], np++;
                if (t->location[1] > ymax) ymax = t->location[1];
            }
            float nn[64];
            uint32_t nnn = 0;
            for (uint32_t a = 0; a < np && nnn < 64; a += np / 64u + 1u) {
                float best = 1e30f;
                for (uint32_t b = 0; b < np; b++) {
                    float dx = px[a] - px[b], dy = py[a] - py[b], d = dx * dx + dy * dy;
                    if (b != a && d > 1.0f && d < best) best = d;
                }
                if (best < 1e29f) nn[nnn++] = sqrtf(best);
            }
            for (uint32_t a = 1; a < nnn; a++) // (insertion sort: 64 at most)
                for (uint32_t b = a; b > 0 && nn[b] < nn[b - 1]; b--) { float x = nn[b]; nn[b] = nn[b - 1], nn[b - 1] = x; }
            if (nnn) sp = fmaxf(80.0f, nn[nnn / 2]);
        }
        uint32_t side = (uint32_t)ceilf(sqrtf((float)skin_copies));
        printf("[RealEngine] Skin copies spaced %.0f cm\n", sp);
        for (int i = 0; i < skin_copies && src.mesh; i++) {
            Entity e = ecs_create(&ed.world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
            if (e == ENTITY_NONE) break;
            snprintf(((CName*)ecs_get(&ed.world, e, COMP_NAME))->text, RE_NAME_MAX, "SkinCopy%d", i);
            CTransform* t = ecs_get(&ed.world, e, COMP_TRANSFORM);
            *t = at;
            t->location[0] = at.location[0] + ((float)((uint32_t)i % side) - (float)side * 0.5f) * sp;
            t->location[1] = (sp > 80.0f ? ymax + sp : at.location[1] + 300.0f) + (float)((uint32_t)i / side) * sp;
            t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
            CSkeletalMesh* c = ecs_get(&ed.world, e, COMP_SKELETAL_MESH);
            *c = src;
            c->inst = 0; // (its own animation instance)
        }
        printf("[RealEngine] Skin copies: %d of the first Skeletal Mesh Actor\n", src.mesh ? skin_copies : 0);
    }
    if (cam_near_skin > 0.0f) { // (2026-11-24, item 4: its LODs seen near and far) the camera D cm out from the crowd's edge:
        // the Skeletal Mesh Actor farthest from their mean, looked at from outside (out of the level's walls around it)
        double cx = 0.0, cy = 0.0;
        uint32_t nsk = 0, best = 0;
        float bd = -1.0f;
        for (int pass = 0; pass < 2; pass++)
            for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) {
                Entity e = ecs_entity_at(&ed.world, s2);
                const CSkeletalMesh* c = e ? ecs_get(&ed.world, e, COMP_SKELETAL_MESH) : NULL;
                const CTransform* t = c && c->mesh ? ecs_get(&ed.world, e, COMP_TRANSFORM) : NULL;
                if (!t) continue;
                if (pass == 0) { cx += t->location[0], cy += t->location[1], nsk++; continue; }
                float dx = t->location[0] - (float)(cx / nsk), dy = t->location[1] - (float)(cy / nsk), d = dx * dx + dy * dy;
                if (d > bd) bd = d, best = s2;
            }
        const CTransform* t = best ? ecs_get(&ed.world, ecs_entity_at(&ed.world, best), COMP_TRANSFORM) : NULL;
        if (t) {
            float dx = t->location[0] - (float)(cx / nsk), dy = t->location[1] - (float)(cy / nsk), l = sqrtf(dx * dx + dy * dy);
            if (l < 1.0f) dx = -1.0f, dy = 0.0f, l = 1.0f;
            ed.cam.pos[0] = t->location[0] + dx / l * cam_near_skin, ed.cam.pos[1] = t->location[1] + dy / l * cam_near_skin;
            ed.cam.pos[2] = t->location[2] + 60.0f;
            ed.cam.yaw = atan2f(-dy, -dx) * 57.29578f, ed.cam.pitch = -5.0f;
            printf("[RealEngine] Camera %.0f cm from the Skeletal Mesh Actor at %.0f %.0f %.0f (the crowd's edge, %u of them)\n", cam_near_skin,
                   t->location[0], t->location[1], t->location[2], nsk);
        }
    }    if (skin_still >= 0) { // (2026-11-24, the Unamed scene's case: Skeletal Mesh Actors standing, no animation, no Anim Blueprint;
        // the first skin_still animated ones keep theirs: a pose that isn't the bind's still bent)
        int kept = 0;
        for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) {
            Entity e = ecs_entity_at(&ed.world, s2);
            CSkeletalMesh* c = e ? ecs_get(&ed.world, e, COMP_SKELETAL_MESH) : NULL;
            if (c && kept < skin_still && (c->anim || c->anim_class)) kept++;
            else if (c) c->anim = 0, c->anim_class = 0;
        }
    }
    if (mesh_socket_test && !editor_mesh_socket_test(&ed)) exit_code = 1; // (2026-11-23)
    if (rig_fit_test && !editor_rig_fit_test(&ed)) exit_code = 1; // (2026-11-23)
    if (mat_live_test && !mat_live_selftest()) exit_code = 1; // (2026-11-24, #180)
    if (mat_param_test && !mat_param_selftest()) exit_code = 1; // (2026-11-24)
    if (reimport && !content_reimport(reimport)) exit_code = 1;
    // --save without --bake: the level as it is after --open / --stress / --import (the game's tests write their level so)
    if (save_path && !bake_mode && !editor_save_level(&ed, save_path, re_time_seconds())) exit_code = 1;
    if (package_dir) { // the level must have a file (--open or --save): no dialog in a test
        if (package_icon) snprintf(project()->icon, sizeof project()->icon, "%s", package_icon); // (a picture or a .ico from anywhere)
        if (make_upload_key && !editor_android_upload_key(&ed, re_time_seconds())) printf("[RealEngine] FAIL: %s\n", ed.status_msg);
        if (!ed.level_path[0] || !(package_quest ? editor_package_quest(&ed, package_dir, false, false, re_time_seconds()) : package_android ? editor_package_android_as(&ed, package_dir, false, false, package_play, re_time_seconds())
                                                   : editor_package_game(&ed, package_dir, false, re_time_seconds()))) {
            printf("[RealEngine] FAIL: Package Game into %s\n", package_dir);
            exit_code = 1;
        } else {
            printf("[RealEngine] %s\n", ed.status_msg);
        }
    }
    if (vehicle_mesh_test && vehicle_mesh_selftest(content_root())) exit_code = 1;
    if (list_actors)
        for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) {
            Entity e = ecs_entity_at(&ed.world, s2);
            const CName* nm = e ? ecs_get(&ed.world, e, COMP_NAME) : NULL;
            const CTransform* t = e ? ecs_get(&ed.world, e, COMP_TRANSFORM) : NULL;
            const CAttach* at = e ? ecs_get(&ed.world, e, COMP_ATTACH) : NULL;
            const CName* on = at ? ecs_get(&ed.world, at->owner, COMP_NAME) : NULL;
            if (nm && t)
                printf("[RealEngine] actor %s: at %.0f %.0f %.0f rot %.0f %.0f %.0f scale %.2f %.2f %.2f%s%s\n", nm->text, t->location[0], t->location[1],
                       t->location[2], t->rotation[0], t->rotation[1], t->rotation[2], t->scale[0], t->scale[1], t->scale[2],
                       at ? " part of " : "", at ? (on ? on->text : "(none)") : "");
            const CLight* li = (nm && t) ? ecs_get(&ed.world, e, COMP_LIGHT) : NULL;
            if (li)
                printf("[RealEngine]   light: type %u, intensity %g, cast shadows %u, source radius %g\n", (unsigned)li->type, li->intensity,
                       li->cast_shadows, li->source_radius);
        }
    // (2026-11-25, the tester's block of item 8) the Content Browser's table (content_open_asset): every kind in its own
    // editor, as a double-click opens it. This list had its own, a few kinds short: a Sound Wave (and every audio asset), a
    // script, an Input Action, a Physical Material, a Camera Shake fell to the Blueprint editor, nothing opened
    if (cli_lang) { // (an unknown code is said, not ignored: the tester's block of item 12)
        int l = i18n_language_from_code(cli_lang);
        if (l >= 0) i18n_set_language(l);
        else printf("[RealEngine] --lang: unknown language code \"%s\" (use en, pt or es; en_US, pt_BR, es_ES)\n", cli_lang);
    }
    if (cli_project_settings) ed.show_project_settings = true;
    if (getenv("REALENGINE_I18N_MISSING")) i18n_record_missing(getenv("REALENGINE_I18N_MISSING")); // (the audit's runtime half)
    if (open_asset && open_viewport) bp_editor_open_viewport(open_asset);
    else if (open_asset) { // (item 9) several, separated by ';': tabs of the Editors window (the screenshots)
        char list[2048];
        snprintf(list, sizeof list, "%s", open_asset);
        for (char* a = strtok(list, ";"); a; a = strtok(NULL, ";")) content_open_asset(a);
    }
    if (browse) content_browse_to(browse), ed.content_docked = true; // docked: it stays open without the focus
    if (browse && browse_hover) content_test_hover(browse);
    if (mm_editor) anim_mm_editor_open_asset(mm_editor, true);
    if (blend_space) anim_blend_space_open_asset(blend_space);
    if (transition_rule) anim_transition_open_asset(transition_rule);
    if (bt_blackboard) ai_editor_blackboard_mode(true);
    if (sheet_anim) printf(asset_anim_sheet(sheet_anim, sheet_png, 8, sheet_yaw) ? "[RealEngine] Animation sheet: %s\n" : "[RealEngine] FAIL: animation sheet %s\n", sheet_png);
    // (a new project opens between frames and its open stops play: then play starts once it is open)
    bool play_after_open = play && ed.pending_project[0];
    if (play && !play_after_open && pie_players > 1) editor_pie_set_players(pie_players), editor_play_multi(&ed, re_time_seconds()); // (B13)
    else if (play && !play_after_open) editor_play(&ed, re_time_seconds());
    if (pie_client && ed.playing && !online_join_address(&ed.world, pie_client)) printf("[RealEngine] FAIL: the client couldn't join %s\n", pie_client); // (B13)
    if (play && play_eject && !play_after_open) {
        editor_set_possessed(&ed, false);
        if (cam_spec) sscanf(cam_spec, "%f,%f,%f,%f,%f", &ed.cam.pos[0], &ed.cam.pos[1], &ed.cam.pos[2], &ed.cam.yaw, &ed.cam.pitch);
    }
    if (ed_bp_idle) ed.bp_test = 2;
    if (ed_bp_walkview) ed.bp_test = 3;
    if (scene_name && !strncmp(scene_name, "house", 5)) ed.bp_test_house = true;
    if (cs_test && !bp_test_construct_editor(&ed)) exit_code = 1;
    if (sk_move_test && !anim_test_editor_move(&ed.world)) exit_code = 1;
    if (source_radius >= 0.0f) set_source_radius(&ed.world, source_radius);
    if (char_test && !char_test_setup(&ed)) exit_code = 1;
    if (bp_dbg_test && !bp_debug_test_setup(&ed)) exit_code = 1;
    if (nav_test && !nav_test_setup(&ed)) exit_code = 1;
    if (constraint_test && !constraint_test_setup(&ed)) exit_code = 1;
    if (morph_test && !morph_test_setup(&ed)) exit_code = 1;
    if (health_test && !health_test_setup(&ed)) exit_code = 1;
    if (inv_test && !inventory_test_setup(&ed)) exit_code = 1;
    if (save_test && !save_test_setup(&ed)) exit_code = 1;
    if (ia_test && !interaction_test_setup(&ed)) exit_code = 1;
    if (dlg_test && !dialogue_test_setup(&ed)) exit_code = 1;
    if (qst_test && !quest_test_setup(&ed)) exit_code = 1;
    if (rules_test && !rules_test_setup(&ed)) exit_code = 1;
    if (abil_test && !ability_test_setup(&ed)) exit_code = 1;
    if (hud_test && !hud_test_setup(&ed)) exit_code = 1;
    if (levels_test && !levels_test_setup(&ed)) exit_code = 1;
    if (picker_test && !picker_test_setup(&ed)) exit_code = 1;
    if (dock_test) dock_test_start(window, (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0);
    if (ui_sweep_dir) ui_sweep_start(ui_sweep_dir);
    if (avp_test) avp_test_start();
    if (psw_test) psw_test_start();
    if (artist_test) artist_test_start();
    if (pause_test && !pause_test_setup(&ed, pause_test == 2)) exit_code = 1;
    if (ingame_test && !ingame_test_setup(&ed, ingame_test, ingame_package)) exit_code = 1; // (Q16)
    if (bp_test && !bp_test_setup(&ed, bp_package)) {
        printf("[RealEngine] FAIL: bp-test setup\n");
        exit_code = 1;
    }
    if ((net_host_test || net_join_test) && !net_session_begin(&ed.world, net_host_test, net_join_test, bench.seconds > 6.0 ? bench.seconds - 3.0 : 4.0, &net_emu))
        exit_code = 1; // (the online test: hosting and starting the client, or joining)
    if (view2_arg) { // Viewport 2 from the start (the tests): its projection, its mode after a colon (lit, unlit, normals, pt)
        ed.show_view2 = true;
        ed.v2_proj = !strncmp(view2_arg, "top", 3) ? 1 : !strncmp(view2_arg, "front", 5) ? 2 : !strncmp(view2_arg, "side", 4) ? 3 : 0;
        const char* m = strchr(view2_arg, ':');
        ed.v2_mode = !m ? 0 : !strcmp(m + 1, "unlit") ? 1 : !strcmp(m + 1, "normals") ? 2 : !strcmp(m + 1, "pt") ? 3 : 0;
    }
    if (umg_test && !umg_test_setup(&ed)) {
        printf("[RealEngine] FAIL: umg-test setup\n");
        exit_code = 1;
    }
    if (seq_test && !seq_test_setup(&ed)) {
        printf("[RealEngine] FAIL: seq-test setup\n");
        exit_code = 1;
    }
    if (cine_test && !cine_test_setup(&ed, cine_test)) {
        printf("[RealEngine] FAIL: cine-test setup\n");
        exit_code = 1;
    }
    if (ide_demo) script_ide_demo_setup(&ed);
    if (cine_cam_test) { // (A9.4) the player's view through a Cine Camera: 50 mm on Super 35, Sunny 16, 6 blades, focus 6 m
        Entity ce = ecs_create(&ed.world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA));
        CCamera* cc = ce ? ecs_get(&ed.world, ce, COMP_CAMERA) : NULL;
        CTransform* ct = ce ? ecs_get(&ed.world, ce, COMP_TRANSFORM) : NULL;
        if (cc && ct) {
            memcpy(ct->location, ed.cam.pos, sizeof ct->location);
            cc->auto_activate = 1, cc->cine = CINE_LENS | CINE_EXPOSURE | CINE_SHUTTER_BLUR;
            cc->focal_mm = 50.0f, cc->sensor_mm = 24.89f, cc->aperture = 16.0f, cc->iso = 100.0f, cc->shutter_s = 1.0f / 125.0f;
            cc->focus_cm = 600.0f, cc->blades = 6;
            cc->fov_deg = 2.0f * atanf(cc->sensor_mm / (2.0f * cc->focal_mm)) * (180.0f / 3.14159265f);
            editor_play(&ed, 0.0);
        } else printf("[RealEngine] FAIL: cine-cam-test: no camera\n"), exit_code = 1;
    }
    if (ssr_test) {
        extern bool editor_ssr_test_setup(Editor * ed);
        if (!editor_ssr_test_setup(&ed)) printf("[RealEngine] FAIL: ssr-test setup\n"), exit_code = 1;
    }
    if (lut_test) { // (A9.4) a 17^3 .cube that inverts the colors, in an unbound Post Process Volume
        char dir[1024], path[1200];
        snprintf(dir, sizeof dir, "%s/LUTTest", content_root());
        re_make_dirs(dir);
        snprintf(path, sizeof path, "%s/Invert.cube", dir);
        FILE* lf = re_fopen(path, "wb");
        if (lf) {
            fprintf(lf, "TITLE \"Invert\"\nLUT_3D_SIZE 17\n");
            for (int b = 0; b < 17; b++)
                for (int g = 0; g < 17; g++)
                    for (int r = 0; r < 17; r++) fprintf(lf, "%.6f %.6f %.6f\n", 1.0 - r / 16.0, 1.0 - g / 16.0, 1.0 - b / 16.0);
            fclose(lf);
        }
        Entity pv = ecs_create(&ed.world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_POST_PROCESS));
        CPostProcess* pp = pv ? ecs_get(&ed.world, pv, COMP_POST_PROCESS) : NULL;
        if (pp) post_volume_defaults(pp), snprintf(pp->lut, sizeof pp->lut, "/Content/LUTTest/Invert.cube");
        else printf("[RealEngine] FAIL: lut-test: no volume\n"), exit_code = 1;
    }
    if (plugin_test) {
        extern bool plugins_test_setup(Editor * ed);
        if (!plugins_test_setup(&ed)) printf("[RealEngine] FAIL: plugin-test setup\n"), exit_code = 1;
    }
    if (vr_hand_test) {
        extern bool vr_hand_test_run(Editor * ed);
        if (!vr_hand_test_run(&ed)) exit_code = 1;
    }
    if (trace_test) {
        extern bool trace_test_setup(Editor * ed);
        if (!trace_test_setup(&ed)) printf("[RealEngine] FAIL: trace-test setup\n"), exit_code = 1;
        else editor_play(&ed, 0.0);
    }
    if (script_test && !script_test_setup(&ed)) {
        printf("[RealEngine] FAIL: script-test setup\n");
        exit_code = 1;
    }
    if (audio_test && !audio_test_setup(&ed)) {
        printf("[RealEngine] FAIL: audio-test setup\n");
        exit_code = 1;
    }
    const char* crash_kind = getenv("REALENGINE_CRASH_TEST"); // tests of the reports: "acesso", "abort", "gpu"...
    if (crash_kind) crash_test(crash_kind);
    glfwSetDropCallback(window, on_drop);
    glfwSetWindowFocusCallback(window, on_focus); // after ImGui's: GLFW keeps one callback, ImGui chains to the previous
    printf("[RealEngine] Editor pronto (%ux%u, %s).\n", vk.extent.width, vk.extent.height,
           vk.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "sem vsync");
    fflush(stdout);

    // the hang watchdog (crashlog.h): the main thread stopped 6 s goes to the log and to crashes/ (not in the tests: a
    // long step of theirs is no hang; REALENGINE_WATCHDOG=<s> turns it on there)
    const char* wd = getenv("REALENGINE_WATCHDOG");
    if (wd || !automated || hang_test) crash_watchdog_start(hang_test ? 3.0 : wd ? atof(wd) : 6.0);
    double prev = re_time_seconds();
    double win_t0 = prev, win_frame = 0, win_cpu = 0, win_gpu = 0;
    bool bench_offscreen = false; // (the visible suites' fps measure, off the screens while it measures)
    uint64_t win_n = 0;
    if (bench.cli) {
        bench.active = true;
        bench.warm_until = prev + 1.0; // the first second: swapchain, fonts, caches
        bench.until = bench.warm_until + bench.seconds;
    }

    if (trace_path && !trace_start(trace_path)) printf("[RealEngine] FAIL: the trace could not start\n"), exit_code = 1;
    if (trace_open) { // (the test: the Trace window shows the file, drawn every frame of the bench)
        extern void editor_show_trace(void);
        editor_show_trace();
        trace_window_load(trace_open);
    }
    while (!ed.quit && !(glfwWindowShouldClose(window) && !dock_route_main_close(window) && editor_request_quit(&ed))) {
        { // (2026-11-25, bug-fixer) REALENGINE_SLOW_FRAME_MS=ms: every frame at least that long (a test's busy machine, made
          // the same every run: the blueprints_slow test, 4 fps)
            static double slow_ms = -1.0;
            if (slow_ms < 0.0) slow_ms = getenv("REALENGINE_SLOW_FRAME_MS") ? atof(getenv("REALENGINE_SLOW_FRAME_MS")) : 0.0;
            if (slow_ms > 0.0) re_sleep_seconds(slow_ms / 1000.0);
        }
        double t0 = re_time_seconds();
        dd_frame_begin(t0), sq_frame_end(); // (2026-11-19) this frame's debug lines from empty; last frame's async traces joined, their counts
        crash_heartbeat(); // (the hang watchdog)
        if (hang_test && ++hang_frames == 30) hang_test_freeze(7.5);
        if (quit_after > 0 && --quit_after == 0) glfwSetWindowShouldClose(window, GLFW_TRUE), ed.dirty = false; // (by the X, "Don't Save")
        ed.measuring = bench.active; // (the editor's background sleep and frame limit off: editor.c)
        glfwPollEvents();
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw == 0 || fbh == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEvents();
            prev = re_time_seconds();
            continue;
        }
        if (browser_open && ++browser_frames == 90) editor_open_project_later(&ed, browser_open, -1);
        if (ed.pending_project[0]) { // the Project Browser's choice: opened between frames, the loading screen meanwhile
            char path[600];
            snprintf(path, sizeof path, "%s", ed.pending_project);
            int tpl = ed.pending_tpl;
            ed.pending_project[0] = 0;
            brand_splash_begin();
            brand_splash_frame(&vk, 0.02f, TR("Opening the project..."));
            if (!editor_open_project(&ed, path, tpl)) project_ui_open_failed(path);
            wait_shaders_splash(&vk, 0.6f);
            brand_splash_end(&vk);
            if (play_after_open) play_after_open = false, editor_play(&ed, re_time_seconds()); // (--play with --new-project)
            t0 = re_time_seconds(); // (the open's time isn't a frame's)
            // (2026-11-23) the measure waits the open's first second as it waits the editor's: --new-project --play's open
            // (5 s) outlived the start's warm-up, so the window began at the first frame after it and counted the level's
            // first frames (528 + 53 ms: the uploads, the first BLAS and the RE_EXT pipelines), 0.58 of demo_fighting's 6 s
            if (bench.active && bench.frames == 0) bench.warm_until = t0 + 1.0, bench.until = bench.warm_until + bench.seconds;
        }
        double dt = t0 - prev;
        prev = t0;
        { // (2026-11-19) the start's longest frame (a cold pipeline cache's compile: render.c's batches) once, at 300
            static double start_worst;
            static uint32_t start_frames;
            if (dt > start_worst && start_frames > 0) start_worst = dt;
            if (++start_frames == 300) {
                printf("[RealEngine] Editor start: longest frame %.0f ms; %u pipelines, %.0f ms compiling (pipeline cache: %s)\n",
                       start_worst * 1000.0, vk.pipelines, vk.pipelines_ms, vk.pipeline_cache_off ? "off" : vk.pipeline_cache_loaded ? "warm" : "cold");
                const char* mx = getenv("REALENGINE_MAX_START_FRAME_MS"); // (the test start_pipelines)
                if (mx && start_worst * 1000.0 > atof(mx)) printf("[RealEngine] FAIL: a start frame took %.0f ms > %s\n", start_worst * 1000.0, mx), exit_code = 1;
                fflush(stdout);
            }
        }
        // --shot-every: the game's clock steps 1/60 s a frame (a frame written to a PNG takes ~1 s: the sequence is a 60 fps video)
        static double sim_t = 0.0;
        if (sim_t == 0.0) sim_t = t0;
        if (shot_every > 0) dt = 1.0 / 60.0;
        { // (2026-11-23) RE_FIXED_DT=s: the game's clock steps s a frame (a slow machine's long frames, made the same every run)
            static double fixed_dt = -1.0;
            if (fixed_dt < 0.0) fixed_dt = getenv("RE_FIXED_DT") ? atof(getenv("RE_FIXED_DT")) : 0.0;
            if (fixed_dt > 0.0) dt = fixed_dt;
        }
        { // (B9) the Movie Render Queue: its fixed clock (each frame a temporal sample, whatever it took)
            double mdt = movie_render_tick(&ed, t0);
            if (mdt > 0.0) dt = mdt;
        }
        if (cine_test) cine_test_tick(&ed, dt);
        sim_t += dt;
        if (net_host_test || net_join_test) net_session_frame(&ed.world);
        if (pie_players > 1 && online_player_count() > pie_max_seen) pie_max_seen = online_player_count(); // (B13: the test's count)

        re_arena_reset(&g_frame_arena);
        ImGui_ImplVulkan_NewFrame();
        TRACE("ImGui: new frame (input)", ImGui_ImplGlfw_NewFrame());
        TRACE("Dock layout (between frames)", dock_between_frames(&ed)); // (Q2: a layout loaded, the last one on open: ImGui's docking between frames)
        if (dock_test) dock_test_input(&ed); // (the test's mouse: ImGui's input queue, before the frame)
        if (ui_sweep_dir) { // (item 21) the sweep's mouse and keys, before the frame
            bool sweep_ok = true;
            TRACE("UI sweep (the robot)", ui_sweep_input(&ed));
            if (ui_sweep_finished(&sweep_ok)) ed.quit = true; // (its findings are a report, not the test's result: the run itself must end)
        }
        if (theme_style_dirty()) apply_theme(g_theme_dpi); // (a theme picked or edited: ImGui's style from the new tokens, before the frame)
        igNewFrame();
        if (scene_name && !strcmp(scene_name, "firstframe")) mat_first_tick(&vk); // (2026-11-24)
        if (scene_name && !strcmp(scene_name, "selection")) selection_test_tick(&ed, &vk); // (2026-11-25, item 17)
        if (vr_preview_now && project_file()[0] && !ed.pending_project[0]) { // (2026-11-25) once the project is open: the
            vr_preview_now = false;                                          // game's executable as a child, ended with the editor
            editor_vr_preview(&ed, t0);
        }
        { // (2026-11-25, bug-fixer) the Assistant's base built on a worker once the editor settled (the project open, its
          // plugins in: the scripts' API table they change is built here first, on this thread)
            static uint32_t as_frames;
            if (as_frames < 31 && !ed.pending_project[0] && !ed.show_browser && ++as_frames == 30) script_api_count(), assistant_init_async();
        }
        if (place_hitch_name) { // (2026-11-25, bug-fixer) a class placed, its longest frame after
            static bool phdone;
            bool ph_ok = true;
            if (!phdone && place_hitch_test_frame(&ed, place_hitch_name, place_hitch_ms, dt, t0, &ph_ok)) phdone = true, ed.quit = true, exit_code = ph_ok ? exit_code : 1;
        }
        if (scal_mem_test) { // (2026-11-25, bug-fixer) Medium first, then the rounds
            static bool started;
            bool smt_ok = true;
            if (!started) started = true, editor_set_scalability(&ed, 1, 0.0);
            else if (scal_mem_test_frame(&ed, &vk, dt, &smt_ok)) ed.quit = true, exit_code = smt_ok ? exit_code : 1;
        }
        if (groom_skin_test) groom_skin_test_tick(&ed, &vk); // (2026-11-25, item 16)
        if (select_all) { static uint32_t sa_frames; if (++sa_frames == 30) editor_select_all(&ed); } // (2026-11-25, item 17: --select-all, after the level's load)
        if (select_name) { static uint32_t sn_frames; if (++sn_frames == 30) for (uint32_t s2 = 1; s2 < ed.world.slot_count; s2++) { Entity e = ecs_entity_at(&ed.world, s2); const CName* nm = e ? ecs_get(&ed.world, e, COMP_NAME) : NULL; if (nm && !strncmp(nm->text, select_name, strlen(select_name)) && !ecs_get(&ed.world, e, COMP_ATTACH)) { editor_select(&ed, e, SEL_ADD); printf("[RealEngine] Selected %s\n", nm->text); } } }
        if (scene_name && (!strncmp(scene_name, "mm_", 3))) editor_mm_startstop_tick(&ed, dt); // (the tests' script)
        if (scene_name && !strcmp(scene_name, "vehicles")) editor_vehicles_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "vehicle_crash")) editor_vehicle_crash_tick(&ed, dt); // (Q27.4)
        if (scene_name && !strcmp(scene_name, "weather_regional")) editor_weather_regional_tick(&ed, dt); // (Q27.6)
        if (scene_name && !strcmp(scene_name, "city_night")) editor_city_night_tick(&ed, dt); // (Q27.8)
        if (scene_name && !strcmp(scene_name, "city_routine")) editor_city_routine_tick(&ed, dt); // (Q27.10)
        if (scene_name && !strcmp(scene_name, "loco_styles")) editor_mm_styles_tick(&ed, dt); // (Q27.11)
        if (scene_name && !strcmp(scene_name, "warp")) editor_warp_tick(&ed, dt); // (2026-11-23)
        if (scene_name && !strcmp(scene_name, "traversal")) editor_traversal_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "anim_warping")) editor_anim_warping_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "combo")) editor_combo_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "cover")) editor_cover_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "house_ghost")) editor_ghost_tick(&ed, dt); // (Q20: the ghosting probe's mover)
        if (scene_name && !strncmp(scene_name, "phys_", 5) && ed.playing) phys_test_tick(&ed, dt); // (Q8, phys_test.c)
        if (scene_name && !strncmp(scene_name, "terror_", 7)) terror_test_tick(&ed, &vk, (float)dt); // (Q13, terror_test.c)
        if (scene_name && !strncmp(scene_name, "pcg_", 4)) pcg_test_tick(&ed, (float)dt); // (Q15, pcg_test.c)
        if (scene_name && !strcmp(scene_name, "ai_senses")) ai_senses_test_tick(&ed, (float)dt); // (Q24, ai_senses_test.c)
        if (scene_name && !strcmp(scene_name, "active_ragdoll")) editor_active_ragdoll_tick(&ed, dt);
        if (scene_name && (!strcmp(scene_name, "socket_offset") || !strcmp(scene_name, "cloth_on_body") || !strcmp(scene_name, "ragdoll_q10") || !strcmp(scene_name, "getup_q10"))) editor_q10_tick(&ed, dt); // (Q10)
        if (scene_name && !strcmp(scene_name, "fps_camera")) editor_q12_tick(&ed, dt); // (Q12)
        if (scene_name && !strcmp(scene_name, "splines")) editor_splines_tick(&ed, dt); // (Q14)
        if (scene_name && !strcmp(scene_name, "hair_dynamic")) editor_hair_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "skin_churn")) editor_skin_churn_tick(&ed);
        if (scene_name && !strcmp(scene_name, "emit_intensity")) emissive_test_tick(&ed, dt); // (2026-11-25: the Emission Intensity)
        if (scene_name && (!strcmp(scene_name, "snow") || !strcmp(scene_name, "rain") || !strcmp(scene_name, "cloth_mesh"))) editor_snow_tick(&ed, dt);
        if (scene_name && (!strcmp(scene_name, "sofa") || !strcmp(scene_name, "softbody") || !strcmp(scene_name, "softbody_rigid"))) editor_soft_tick(&ed, dt); // (Q9)
        if (scene_name && !strcmp(scene_name, "procgen")) editor_procgen_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "facility_leak")) editor_facility_leak_tick(&ed); // (2026-11-25, item 19)
        if (scene_name && (!strcmp(scene_name, "wp") || !strcmp(scene_name, "wp_fast"))) editor_wp_tick(&ed, dt);
        if (scene_name && !strcmp(scene_name, "landscape_paint")) editor_landscape_paint_tick(&ed, dt);
        TRACE("Editor Frame (UI, game)", editor_frame(&ed, dt, shot_every > 0 ? sim_t : t0));
        if (trace_bench_n) { // (2026-11-19) N traces a frame while playing (trace_test.c)
            extern void trace_bench_frame(Editor * ed, uint32_t n);
            trace_bench_frame(&ed, trace_bench_n);
        }
        if (picker_test) { // (Q6: after the editor's frame, which drew the picker)
            picker_test_frame(&ed);
            bool picker_ok;
            if (picker_test_finished(&picker_ok)) ed.quit = true, exit_code = picker_ok ? exit_code : 1;
        }
        if (icon_sheet) ui_icon_sheet_draw(); // (Q5: the icon sheet over everything)
        mcp_server_frame(&ed, t0); // (2026-11-24) the AI's queued calls (mcp.h)
        if (psw_test) {
            bool psw_ok;
            if (psw_test_frame(&ed, &psw_ok)) ed.quit = true, exit_code = psw_ok ? exit_code : 1;
        }
        if (mcp_test) {
            bool mcp_ok;
            if (mcp_test_frame(&ed, &mcp_ok)) ed.quit = true, exit_code = mcp_ok ? exit_code : 1;
        }
        if (avp_test) {
            bool avp_ok;
            if (avp_test_frame(&ed, &avp_ok)) ed.quit = true, exit_code = avp_ok ? exit_code : 1;
        }
        if (artist_test) { // (Q23)
            bool art_ok;
            if (artist_test_frame(&ed, &art_ok)) ed.quit = true, exit_code = art_ok ? exit_code : 1;
        }
        if (dock_test) {
            dock_test_frame(&ed);
            bool dock_ok;
            if (dock_test_finished(&dock_ok)) ed.quit = true, exit_code = dock_ok ? exit_code : 1;
        }
        ui_tips_frame(); // the hovered widget's description (ui_tips.h)
        brand_splash_fade();
        igRender();
        // ImGui's texture changes (the font atlas, when new glyphs show up) uploaded here, the GPU idle: its backend
        // would submit and wait on the queue in the middle of the frame's recording, which, with a heavy previous frame
        // still running, crashed the NVIDIA driver in one of its threads (notas-dev/escala.md). Rare: at startup and
        // on new glyphs. A texture waiting to be destroyed needs no wait (the backend keeps it until it's unused).
        ImDrawData* dd = igGetDrawData();
        if (dd && dd->Textures) {
            bool idle = false;
            for (int i = 0; i < dd->Textures->Size; i++) {
                ImTextureData* t = dd->Textures->Data[i];
                if (t->Status == ImTextureStatus_OK) continue;
                if (t->Status != ImTextureStatus_WantDestroy && !idle) {
                    vkDeviceWaitIdle(vk.device);
                    idle = true;
                }
                ImGui_ImplVulkan_UpdateTexture(t);
            }
        }

        double w0 = re_time_seconds();
        VkCommandBuffer cmd = vkctx_begin_frame(&vk); // waits for the frame slot and the swapchain image
        double w1 = re_time_seconds();
        trace_span("Wait for GPU (frame slot)", w0, w1);
        if (cmd) {
            if (vk.min_image_count != imgui_min_images) {
                imgui_min_images = vk.min_image_count;
                ImGui_ImplVulkan_SetMinImageCount(imgui_min_images);
            }
            // The scene's passes, then the swapchain: the scene into the viewport's rectangle, the UI over it (on NVIDIA drivers
            // before 617, after the last frame: vkctx.c; render_scene waits itself after its CPU work)
            double rs0 = trace_t0();
            bool scene = render_scene(&vk, cmd, &ed.world, &ed.render_view);
            cmd = vkctx_cmd(&vk); // (A9.7c: a frame split for async compute goes on in its second part)
            trace_scope("Render (record scene)", rs0);
            if (scene && ed.v2_on) render_view2(&vk, cmd, &ed.v2); // Viewport 2: the same frame, its own camera
            render_preview_record(&vk, cmd); // the asset editors' previews asked for while the UI was built
            vkctx_wait_submitted(&vk);
            // the UI pass timed (two timestamps of our own: the HUD's GPU cost is in it, hud.h); the slot's last result
            // first (its fence was waited by vkctx_begin_frame)
            if (!ui_q && vk.timestamps) {
                VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 2 * RE_FRAMES_IN_FLIGHT;
                if (vkCreateQueryPool(vk.device, &qi, NULL, &ui_q) != VK_SUCCESS) ui_q = VK_NULL_HANDLE;
            }
            uint32_t uf = vk.frame % RE_FRAMES_IN_FLIGHT;
            if (ui_q && ui_q_used[uf]) {
                uint64_t ut[2];
                if (vkGetQueryPoolResults(vk.device, ui_q, uf * 2, 2, sizeof ut, ut, sizeof ut[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ut[1] >= ut[0])
                    hud_set_ui_gpu_ms((double)(ut[1] - ut[0]) * vk.timestamp_ms);
            }
            if (ui_q) vkCmdResetQueryPool(cmd, ui_q, uf * 2, 2);
            vkctx_begin_rendering(&vk);
            if (scene) render_composite(&vk, cmd, &ed.render_view), render_debug_draw(&vk, cmd, &ed.render_view);
            gpu_mark(cmd, "UI do editor (ImGui)");
            if (ui_q) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ui_q, uf * 2);
            ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd, VK_NULL_HANDLE);
            if (ui_q) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ui_q, uf * 2 + 1), ui_q_used[uf] = true;
            TRACE("Submit and Present", vkctx_end_frame(&vk));
        }
        if (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) { // the windows dragged out of the editor: their own swapchains
            igUpdatePlatformWindows();
            igRenderPlatformWindowsDefault(NULL, NULL);
        }
        double t1 = re_time_seconds();
        double cpu_ms = ((t1 - t0) - (w1 - w0)) * 1000.0;
        if (g_trace_on) { // (B14b) the frame and the GPU's passes (their timestamps, the frame before last)
            const char* gn[16];
            double gm[16];
            uint32_t ng = trace_gpu_passes(gn, gm, 16);
            gn[ng] = "GPU Editor UI", gm[ng++] = hud_ui_gpu_ms();
            trace_frame(t0, t1, gn, gm, ng);
        }

        // Status bar: half-second averages
        win_frame += dt * 1000.0;
        win_cpu += cpu_ms;
        win_gpu += vk.gpu_ms;
        win_n++;
        if (t1 - win_t0 >= 0.5) {
            ed.stats.fps = (double)win_n / (t1 - win_t0);
            ed.stats.frame_ms = win_frame / (double)win_n;
            ed.stats.cpu_ms = win_cpu / (double)win_n;
            ed.stats.gpu_ms = win_gpu / (double)win_n;
            win_t0 = t1;
            win_frame = win_cpu = win_gpu = 0;
            win_n = 0;
        }

        // Timed runs (--bake: after the bake, which is then saved with --save)
        if (bake_t0 > 0.0) {
            if (gi_baking()) {
                bench.warm_until = t1 + 0.5;
                bench.until = bench.warm_until + bench.seconds;
            } else {
                printf("[RealEngine] Build Lighting (%s): %.2f s\n", bake_mode, t1 - bake_t0);
                bake_t0 = 0.0;
                if (save_path && !editor_save_level(&ed, save_path, t1)) exit_code = 1;
            }
        }
        if (!bench.active && ed.bench_request > 0.0) {
            memset(&bench, 0, sizeof bench);
            bench.active = true;
            bench.seconds = ed.bench_request;
            bench.warm_until = t1;
            bench.until = t1 + bench.seconds;
        }
        // the measure starts once the Masked shadow rays' pipelines are in (made on a thread: before, the rays are opaque
        // and the frame up to twice as cheap; a measure taking its window across the swap varied 22-38 ms)
        // (0.75 s after them: the unified denoiser's history, 64 frames, starts when its pipelines are in)
        if (bench.active && bench.frames == 0 && render_pipelines_pending()) bench.warm_until = t1 + 0.75, bench.until = bench.warm_until + bench.seconds;
        // (Q22) and once the project's materials are compiled (the editor's start bakes every one in the background, a
        // slice a frame: up to ~0.2 s frames for ~1.5 s that are no scene's cost; at most 8 s waited)
        else if (bench.active && bench.frames == 0 && mat_compiling() && t1 - win_t0 < 8.0) bench.warm_until = t1 + 0.1, bench.until = bench.warm_until + bench.seconds;
        if (seq_test) seq_test_frame(&ed); // (the Camera Cuts' view, counted)
        if (async_test) async_test_frame(&ed, dt);
        if (assistant_test) assistant_test_frame(&ed);
        if (morph_test) morph_test_frame(&ed, &vk);
        if (bp_dbg_test) bp_debug_test_frame(&ed); // (also while paused at a breakpoint)
        // the measures wait for the background work (async.h: an import, the materials, the meshes read): a test measures
        // the scene, not its loading (the async test's own measure waits for its tasks too)
        if (bench.active && bench.frames == 0 &&
            (!async_idle() || async_test_running() || (assistant_test && !assistant_test_done()) || (morph_test && !morph_test_done()) || (save_test && !save_test_done()) ||
             (abil_test && !ability_test_done())))
            bench.warm_until = t1 + 0.25, bench.until = bench.warm_until + bench.seconds;
        if (scene_name && !strncmp(scene_name, "udim", 4)) udim_test_frame(&ed, bench.active && t0 >= bench.warm_until, t0); // (Q18: the road's flight)
        if (scene_name && !strncmp(scene_name, "vista", 5)) { // (Q21) the lookout found; the measure waits for the trees, impostors and far shadow
            farview_test_frame(&ed, &vk, bench.active && t0 >= bench.warm_until && bench.frames > 0);
            if (bench.active && bench.frames == 0 && !farview_test_ready(&ed)) bench.warm_until = t1 + 0.25, bench.until = bench.warm_until + bench.seconds;
        }
        if (stress_textures && bench.active) { // (the stress flies along its row: the cubes climb as it nears, go down as it leaves)
            static double fly0 = 0.0;
            if (fly0 == 0.0) fly0 = t0;
            float span = (float)stress_textures * 250.0f, x = fmodf((float)((t0 - fly0) * stress_fly), 2.0f * span);
            ed.cam.pos[0] = x < span ? x : 2.0f * span - x;
        }
        { // the per-pass GPU times (--gpu-passes): from the measure's first frame
            static bool pt_reset;
            if (bench.active && t0 >= bench.warm_until && !pt_reset) pt_reset = true, vkctx_pass_stats_reset();
        }
        if (bench.active && t0 >= bench.warm_until && bench.frames > 0) { // (a still bench: nothing moves)
            static double mv0 = 0.0;
            double dt_mv = mv0 > 0.0 ? (shot_every > 0 ? 1.0 / 60.0 : t0 - mv0) : 0.0;
            mv0 = t0;
            for (int k = 0; k < 3; k++) ed.cam.pos[k] += cam_move[k] * (float)dt_mv;
        }
        { // (2026-11-23, the site's screenshots) --open-asset with --shot: the asset's editor brought to the front a second
          // before the picture (a level opening with the project focuses the viewport: the floating editor went behind it)
            static bool refocused;
            const char* rext = open_asset ? strrchr(open_asset, '.') : NULL;
            if (shot_path && rext && !refocused && bench.active && t1 >= bench.until - 1.0) {
                refocused = true;
                if (!strcmp(rext, ".rbp")) bp_editor_open(open_asset);
                else if (!strcmp(rext, ".rmat") || !strcmp(rext, ".rmi")) mat_editor_open(open_asset);
            }
        }
        if (shot_path && shot_every > 0 && bench.active && t0 >= bench.warm_until && bench.frames > (uint64_t)shot_skip && bench.frames % (uint64_t)shot_every == 0) {
            static char seq[600];
            const char* dot = strrchr(shot_path, '.');
            int stem = dot ? (int)(dot - shot_path) : (int)strlen(shot_path);
            snprintf(seq, sizeof seq, "%.*s_%03llu.png", stem, shot_path, (unsigned long long)(bench.frames / (uint64_t)shot_every));
            vkctx_request_shot(&vk, seq);
        } else if (shot_path && shot_every <= 0 && !shot_asked && bench.active && t1 >= bench.until - 0.25) vkctx_request_shot(&vk, shot_path), shot_asked = true;
        if (dyn_res_fps > 0.0f && ups_top > 0.0f) { // --dyn-res: the share follows the GPU against the target (upscale.h)
            float before = ed.gi.tsr_scale;
            ed.gi.tsr_scale = upscale_dynamic(&ups_dyn, vk.gpu_ms, dyn_res_fps, upscale_dynamic_floor(ups_top, dyn_res_min >= 0 ? dyn_res_min : project()->dyn_res_min), ups_top);
            if (ed.gi.tsr_scale != before) printf("[RealEngine] Dynamic resolution: %.0f%% -> %.0f%% (GPU %.2f ms, target %.0f fps)\n", before * 100.0f,
                                                  ed.gi.tsr_scale * 100.0f, vk.gpu_ms, dyn_res_fps);
        } else if (upscale_mode < 0 && ed.gi.tsr_scale > 0.0f) {
            // (2026-11-10, "os upscalings devem funcionar dentro da viewport do editor") the project's Upscaling Mode and
            // Dynamic Resolution (Project Settings > Rendering) in the editor's viewport too, as the game does (game_main.c):
            // the share the scalability set (its preset's) is noticed when it changes, the mode's over it, the dynamic
            // resolution under it. The upscaler itself (TSR, FSR, DLSS, XeSS) was already the project's here
            static float preset = 0.0f, written = -1.0f;
            static UpsDynamic dyn;
            if (ed.gi.tsr_scale != written) preset = ed.gi.tsr_scale, memset(&dyn, 0, sizeof dyn); // (the scalability changed it)
            const ProjectSettings* ps = project();
            float top = upscale_mode_scale(ps->upscale_mode, preset);
            float sc = ps->dyn_res ? upscale_dynamic(&dyn, vk.gpu_ms, (float)(ps->dyn_res_fps > 0 ? ps->dyn_res_fps : 100), upscale_dynamic_floor(top, ps->dyn_res_min), top) : top;
            ed.gi.tsr_scale = written = sc;
        }
        // (2026-11-23) the visible suites (REALENGINE_MONITOR): the fps measure taken hidden. A window shown on a monitor
        // costs 9-12% of the GPU in Windows' composition, the same in mailbox and immediate and in a window inside the
        // display (rain Epic 173 vs 199 fps, many_lights 69 vs 78), and the targets were set hidden: the test runs on the
        // monitor, its measure off the screens (the same size: the same swapchain), a quarter second to settle first
        if (on_monitor && min_fps > 0.0 && bench.active && bench.frames == 0 && t0 >= bench.warm_until && !bench_offscreen) {
            re_test_window_offscreen(window, true), bench_offscreen = true;
            printf("[RealEngine] Visible suite: the fps measure off the screens\n"), fflush(stdout);
            bench.warm_until = t0 + 0.25, bench.until = bench.warm_until + bench.seconds;
        }
        if (bench.active && t0 >= bench.warm_until) {
            if (bench.frames == 0) { // starts counting here: the whole window, however late the first frame (a pipeline compiled, a
                // busy GPU: a warm-up past the deadline measured one frame, and --bp-test checked a game 0.1 s old)
                bench.frame_sum = 0.0, bench.warm_until = t0, bench.until = t0 + bench.seconds, render_flicker_reset(), vsm_stats_reset(), lsm_stats_reset(), gi_probe_check(true);
                if (budget_ms > 0.0) fb_begin();
            }
            bench.frames++;
            bench.cpu_sum += cpu_ms;
            bench.gpu_sum += vk.gpu_ms;
            {
                uint32_t sk_n, sk_b, sk_v;
                double sk_ms;
                render_skin_stats(&sk_n, &sk_b, &sk_v, &sk_ms);
                bench.skin_sum += sk_ms, bench.skin_bends += sk_b;
                if (sk_ms > bench.skin_worst) bench.skin_worst = sk_ms;
            }
            if (budget_ms > 0.0) fb_add_frame(cpu_ms, vk.gpu_ms);
            { // (the passes' times are the frame before last's: their minimum still is one frame's)
                const RenderStats* rm = render_stats();
                if (vk.gpu_ms > 0.0 && (bench.gpu_min <= 0.0 || vk.gpu_ms < bench.gpu_min)) bench.gpu_min = vk.gpu_ms;
                if (rm->gpu_main_ms > 0.0 && (bench.main_min <= 0.0 || rm->gpu_main_ms < bench.main_min)) bench.main_min = rm->gpu_main_ms;
                if (rm->gpu_gi_ms > 0.0 && (bench.gi_min <= 0.0 || rm->gpu_gi_ms < bench.gi_min)) bench.gi_min = rm->gpu_gi_ms;
            }
            if (dt * 1000.0 > bench.worst && bench.frames > 1) bench.worst = dt * 1000.0;
            // (2026-11-25, bug-fixer) --bp-test checks a game that ran its bench's seconds of game time, not of the wall's: a
            // frame's dt counts at most 0.1 s, so under 10 fps (a busy suite) 2 s of wall were 0.4 s of game and its 1 s
            // Timeline hadn't reached its Ping ("the Timeline didn't play", Pings 0, Done 0). At most 60 s more waited
            const bool bp_clock_short = bp_test && !net_host_test && !net_join_test && ed.playing && bp_game_time() < bench.seconds &&
                                        t1 < bench.until + 60.0;
            // (2026-11-25, bug-fixer) --pause-test: its steps (by the game's seconds) finished before the check, at most 30 s
            // more; under load (a 850 ms Options frame, another program's cores) the bench ended at step 5
            const bool pause_short = pause_test == 1 && !pause_test_done() && t1 < bench.until + 30.0;
            if (t1 >= bench.until && !bp_clock_short && !pause_short) {
                double secs = t1 - bench.warm_until;
                double fps = (double)bench.frames / secs;
                const char* pm = vk.present_mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                                 : vk.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo (vsync)";
                char line[320];
                snprintf(line, sizeof line,
                         "Measure: %.0f fps (%.3f ms/frame, worst %.3f ms), CPU %.3f ms, GPU %.3f ms - %llu frames in "
                         "%.2f s, %ux%u, %s, %s",
                         fps, 1000.0 / fps, bench.worst, bench.cpu_sum / bench.frames, bench.gpu_sum / bench.frames,
                         (unsigned long long)bench.frames, secs, vk.extent.width, vk.extent.height, pm,
                         vk.gpu_props.deviceName);
                editor_status(&ed, t1, "%s", line);
                const RenderStats* rs = render_stats();
                if (decal_stats()->total) printf("[RealEngine] Decals: %u of %u drawn\n", decal_stats()->drawn, decal_stats()->total);
                if (rs->gpu_denoise_ms > 0.0) printf("[RealEngine] Denoiser: %.3f ms (the last frame's)\n", rs->gpu_denoise_ms);
                if (rs->gpu_sss_ms > 0.0) printf("[RealEngine] Subsurface (pele): %.3f ms (the last frame's)\n", rs->gpu_sss_ms);
                { // (Q21) the distant view
                    const FarviewStats* fv = farview_stats();
                    if (fv->shadow_on || fv->impostors)
                        printf("[RealEngine] Distant view: far shadow %s (%u x %u, %.1f m a texel, horizon %.3f ms, %u passes), %u impostors of %u candidates "
                               "(%u x %u, %.0f MB, switch %.0f px, bakes %.1f ms), fog sky %.2f\n",
                               fv->shadow_on ? "on" : "off", fv->res, fv->res, fv->texel_cm * 0.01f, fv->shadow_gpu_ms, fv->shadow_runs, fv->impostors,
                               fv->candidates, fv->atlas, fv->atlas, (double)fv->bytes / 1048576.0, fv->switch_px, fv->bake_ms_total, fv->fog_sky);
                }
                if (tess_stats()->on) { // (Q17)
                    const TessStats* tst = tess_stats();
                    printf("[RealEngine] Geometry Tessellation: %.2f M micro triangles from %u base ones, dicing %.3f ms, %.1f px per edge x %.2f (budget), "
                           "most %.0f, %u fallbacks%s%s (the last frame's)\n",
                           (double)tst->micro / 1e6, tst->base, tst->gpu_ms, tst->px, tst->scale, tst->max_factor, tst->fallback,
                           tst->subdiv ? "" : ", displaced only", tst->scale > 1.01f ? ", scaled by the budget" : "");
                }
                printf("[RealEngine] Render: GPU scene %.3f, GI %.3f, shadow %.3f, main %.3f, mask %.3f, exposure %.3f, post %.3f, composite %.3f ms; "
                       "CPU %.3f ms; %u/%u meshes, %u draws, %u lights, MSAA %ux; exposure EV100 %.1f (scene mean %.0f nits)\n",
                       rs->gpu_scene_ms, rs->gpu_gi_ms, rs->gpu_shadow_ms, rs->gpu_main_ms, rs->gpu_mask_ms, rs->gpu_exposure_ms, rs->gpu_post_ms, rs->gpu_composite_ms,
                       rs->cpu_ms, rs->visible, rs->instances, rs->draws, rs->lights, rs->msaa,
                       rs->auto_exposure > 0.0f ? log2f(exp2f(ed.exposure_comp) / (1.2f * rs->auto_exposure)) : 0.0f, exp2f(rs->avg_log_lum));
                printf("[RealEngine] GPU minimum: %.3f ms a frame (main %.3f, GI %.3f): the least frame, ~its cost alone on the GPU\n",
                       bench.gpu_min, bench.main_min, bench.gi_min);
                { // (2026-11-24) the main pass's runs: plain, and the RE_EXT ones (only the meshes an RE_EXT material draws)
                    uint32_t rp = 0, rx = 0, mx = 0;
                    render_ext_runs(&rp, &rx, &mx);
                    printf("[RealEngine] Main pass runs: %u plain, %u RE_EXT (%u meshes with an RE_EXT material)\n", rp, rx, mx);
                    if (class_split_check) {
                        bool cs_ok = rx > 0 && rp > 0;
                        if (!cs_ok) printf("[RealEngine] FAIL: class split: %u plain runs, %u RE_EXT (wanted both: the other meshes stay plain)\n", rp, rx), exit_code = 1;
                        else printf("[RealEngine] class split: ok\n");
                    }
                }
                if (vkctx_pass_timing_on()) { // (--gpu-passes) each marked pass's mean over the measure, the slowest first
                    const GpuPassStat* ps;
                    uint32_t nf = 0, np = vkctx_pass_stats(&ps, &nf), ord[160];
                    double tot = 0.0;
                    if (np > 160) np = 160;
                    for (uint32_t k = 0; k < np; k++) ord[k] = k, tot += nf ? ps[k].ms / nf : 0.0;
                    for (uint32_t a = 1; a < np; a++)
                        for (uint32_t b = a; b > 0 && ps[ord[b]].ms > ps[ord[b - 1]].ms; b--) {
                            uint32_t t = ord[b];
                            ord[b] = ord[b - 1], ord[b - 1] = t;
                        }
                    printf("[RealEngine] GPU passes (%u frames, %.3f ms a frame):", nf, tot);
                    for (uint32_t k = 0; k < np; k++)
                        if (nf && ps[ord[k]].ms / nf >= 0.005) printf(" %s %.3f;", ps[ord[k]].name, ps[ord[k]].ms / nf);
                    printf("\n");
                }
                if (budget_ms > 0.0 && !fb_check(budget_ms, scene_name ? scene_name : "", budget_strict)) exit_code = 1;
                const ClothStats* cst = cloth_stats();
                if (cst->cloths)
                    printf("[RealEngine] Cloth: %u cloths, %u vertices, %u colliders, %u dispatches, CPU %.3f ms\n", cst->cloths, cst->vertices,
                           cst->colliders, cst->dispatches, cst->cpu_ms);
                const LsmStats* lst = lsm_stats();
                if (lst->frames && lst->atlas && (lst->full_total || lst->lights))
                    printf("[RealEngine] Local shadows: %d lights mapped of %d in view, atlas %d, %llu faces whole + %llu dynamic in %llu frames; "
                           "GPU %.3f ms a drawing frame (%llu, worst %.3f), CPU %.3f ms a frame\n", lst->lights, lst->candidates, lst->atlas,
                           (unsigned long long)lst->full_total, (unsigned long long)lst->dyn_total, (unsigned long long)lst->frames,
                           lst->gpu_frames ? lst->gpu_sum / (double)lst->gpu_frames : 0.0, (unsigned long long)lst->gpu_frames, lst->gpu_max,
                           lst->cpu_sum / (double)lst->frames);
                const WaterStats* wst = water_stats();
                if (wst->rain || wst->ocean || wst->lakes)
                    printf("[RealEngine] Water: rain %d (%u drops, %u splashes, %u cells), ocean %d, %u lakes, %u footprints, %u wet actors (max %.2f), CPU %.3f ms\n", wst->rain,
                           wst->drops, wst->splashes, wst->cells, wst->ocean, wst->lakes, wst->footprints, wst->wet_followed, (double)wst->wet_max, wst->cpu_ms);
                printf("[RealEngine] Scene: %u slots, %u drawn (%.1f M triangles), TLAS %u instances, %u builds, %u refits; "
                       "occlusion %s (GPU %.3f ms without, %.3f with)\n",
                       rs->instances, rs->visible, (double)rs->triangles / 1e6, gi_stats()->tlas_instances, gi_stats()->tlas_builds,
                       gi_stats()->tlas_refits, rs->occlusion ? "on" : "off", rs->occlusion_ms[0], rs->occlusion_ms[1]);
                printf("[RealEngine] Lights: %u (local in the grid: %u items of %u)\n", rs->lights, rs->grid_items, rs->grid_capacity);
                printf("[RealEngine] Material textures: %u, %.0f MB\n", mat_stats()->textures, (double)mat_stats()->tex_bytes / 1048576.0);
                { // the sun's virtual shadow maps (vsm.c): what was drawn again, per frame on average
                    const VsmStats* vs = vsm_stats();
                    double nf = vs->frames ? (double)vs->frames : 1.0;
                    {
                        uint32_t sk_n, sk_b, sk_v;
                        double sk_ms;
                        render_skin_stats(&sk_n, &sk_b, &sk_v, &sk_ms);
                        uint32_t sk_bind, sk_noroom;
                        render_skin_bind_stats(&sk_bind, &sk_noroom);
                        if (sk_n)
                            printf("[RealEngine] Skins: %u, bent last frame %u (%u vertices), CPU %.3f ms, GPU %.4f ms\n", sk_n, sk_b, sk_v, sk_ms,
                                   rs->gpu_skin_ms);
                        uint32_t sk_rigid = render_skin_rigid_count(); // (2026-11-24, item 4)
                        if (sk_n || sk_bind || sk_rigid) // (2026-11-24) the dynamic set's CPU on the render's thread over the measure
                            printf("[RealEngine] Skins at their bind: %u (%u without room), with their own vertices %u (%llu bends over the "
                                   "measure); dynamic set CPU %.3f ms a frame (worst %.3f ms)\n", sk_bind, sk_noroom, sk_n,
                                   (unsigned long long)bench.skin_bends, bench.skin_sum / (double)bench.frames, bench.skin_worst);
                        if (sk_rigid) printf("[RealEngine] Rigid skins: %u (one bone holds every vertex: drawn as the mesh moved by it)\n", sk_rigid);
                        if (max_skin_ms > 0.0 && bench.skin_sum / (double)bench.frames > max_skin_ms) {
                            printf("[RealEngine] FAIL: the dynamic set and skins took %.3f ms of CPU a frame > %.3f ms\n", bench.skin_sum / (double)bench.frames,
                                   max_skin_ms);
                            exit_code = 1;
                        }
                        { // (2026-11-24, item 4) the dormant dynamic instances
                            uint32_t dn, ds, dw;
                            render_dormant_stats(&dn, &ds, &dw);
                            if (dn || ds) printf("[RealEngine] Dormant dynamic instances: %u (slept %u, woke %u since the start)\n", dn, ds, dw);
                        }
                        uint32_t mo_m, mo_j, mo_v;
                        render_morph_stats(&mo_m, &mo_j, &mo_v);
                        if (mo_m) printf("[RealEngine] Morph targets: %u meshes, %u skins bent with weights last frame (%u vertices)\n", mo_m, mo_j, mo_v);
                        const ParticleStats* ps = particles_stats();
                        if (ps->emitters)
                            printf("[RealEngine] Particles: %u emitters, %u slots, drawn %u sprites + %u meshes, CPU %.3f ms\n", ps->emitters,
                                   ps->capacity, ps->sprites, ps->meshes, ps->cpu_ms);
                    }
                    if (vs->levels) printf("[RealEngine] Virtual shadows: %d levels of %d, %.2f drawn whole + %.2f rectangles a frame\n", vs->levels, vs->res, (double)vs->full_total / nf, (double)vs->partial_total / nf);
                }
                if (dv_open()) dv_dump(); // the Debug Viewer's lists (its test)
                if (anim_stats()->meshes)
                    printf("[RealEngine] Animation: %u skeletal meshes (%u sampled last frame, %u parts), %.3f ms\n", anim_stats()->meshes,
                           anim_stats()->sampled, anim_stats()->parts, anim_stats()->ms);
                const GiCacheStats* cs = gi_cache_stats();
                printf("[RealEngine] Surface cache: %u/%u charts, %u texels (%.0f cm x%.2f, coarser past %.0f m), %u per frame (+%u of moving actors), %u reallocations; flicker %.3f%%, %.2f%% of the view\n",
                       cs->charts, cs->candidates, cs->texels, (double)cs->texel_cm, (double)cs->coarse, (double)cs->falloff_cm / 100.0,
                       cs->per_frame, cs->dynamic, cs->reallocations, render_flicker() * 100.0,
                       render_flicker_spots() * 100.0);
                printf("[RealEngine] Flicker back and forth: %.2f%% of the view (what moves smoothly doesn't count)\n", render_flicker_pulses() * 100.0);
                { // (Q20) the trail behind moving actors (render_ghosting): only with one in the view
                    double gbg = 0.0, gsm = 0.0, gh = render_ghosting(&gbg, &gsm);
                    printf("[RealEngine] Ghosting: %.3f%% behind moving actors (%.0f points a frame), background %.3f%%\n", gh * 100.0, gsm, gbg * 100.0);
                }
                uint32_t probe_collisions = gi_probe_collisions(); // (two cells of the grid in one probe slot: gi.c)
                gi_probe_check(false);
                const char* fmap = getenv("REALENGINE_FLICKER_MAP"); // a debugging aid: where it flickers (PGM, 64 x 32)
                if (fmap) {
                    uint32_t nf;
                    const uint32_t* m = render_flicker_map(&nf);
                    FILE* pf = fopen(fmap, "wb");
                    if (pf) {
                        fprintf(pf, "P5 64 32 255\n");
                        for (int k = 0; k < 64 * 32; k++) fputc(nf ? (int)(m[k] * 255u / nf) : 0, pf);
                        fclose(pf);
                    }
                }
                bench.active = false;
                if (bench_offscreen) re_test_window_offscreen(window, false), bench_offscreen = false; // (measured: back on the monitor)
                ed.bench_request = 0.0;
                if (bench.cli) {
                    if (bp_test && !net_host_test && !net_join_test && !bp_test_check(&ed)) exit_code = 1;
                    if (char_test && !char_test_check(&ed)) exit_code = 1;
                    if (bp_dbg_test && !bp_debug_test_check(&ed)) exit_code = 1;
                    if (nav_test && !nav_test_check(&ed)) exit_code = 1;
                    if (constraint_test && !constraint_test_check(&ed)) exit_code = 1;
                    if (morph_test && !morph_test_check(&ed)) exit_code = 1;
                    if (health_test && !health_test_check(&ed)) exit_code = 1;
                    if (save_test && !save_test_check(&ed)) exit_code = 1;
                    if (inv_test && !inventory_test_check(&ed)) exit_code = 1;
                    if (ia_test && !interaction_test_check(&ed)) exit_code = 1;
                    if (dlg_test && !dialogue_test_check(&ed)) exit_code = 1;
                    if (qst_test && !quest_test_check(&ed)) exit_code = 1;
                    if (rules_test && !rules_test_check(&ed)) exit_code = 1;
                    if (abil_test && !ability_test_check(&ed)) exit_code = 1;
                    if (hud_test && !hud_test_check(&ed)) exit_code = 1;
                    if (levels_test && !levels_test_check(&ed)) exit_code = 1;
                    if (pause_test && !pause_test_check(&ed, pause_test == 2)) exit_code = 1;
                    if ((net_host_test || net_join_test) && !net_session_check(&ed.world)) exit_code = 1;
                    if (umg_test && !umg_test_check(&ed)) exit_code = 1;
                    if (seq_test && !seq_test_check(&ed)) exit_code = 1;
                    if (cine_test && !cine_test_check(&ed)) exit_code = 1;
                    if (pie_players > 1) { // (B13) Play with several players: the others' windows joined this editor's session
                        printf("[RealEngine] PIE multiplayer: %d of %d players in the session, %d client windows\n", pie_max_seen, pie_players, editor_pie_clients());
                        if (pie_max_seen < pie_players) printf("[RealEngine] FAIL: PIE multiplayer: the other players didn't join\n"), exit_code = 1;
                    }
                    if (audio_test && !audio_test_check(&ed)) exit_code = 1;
                    if (script_test && !script_test_check(&ed)) exit_code = 1;
                    if (trace_test) {
                        extern bool trace_test_check(Editor * ed);
                        if (!trace_test_check(&ed)) exit_code = 1;
                    }
                    if (trace_bench_n) {
                        extern void trace_bench_report(void);
                        trace_bench_report();
                    }
                    if (cine_cam_test) { // the exposure in use is the camera's: 1 / (1.2 x 2^EV100), EV100 = log2(16^2 x 125) = 14.97
                        float ex = render_stats()->auto_exposure;
                        float comp = ed.render_view.exposure_comp + (ed.render_view.post ? ed.render_view.post->v[PP_EXPOSURE_COMP] : 0.0f);
                        float ev = ex > 0.0f ? -log2f(1.2f * ex) + comp : 0.0f; // (the compensation over it, as Unreal's manual exposure)
                        printf("[RealEngine] Cine camera: EV100 %.2f (want 14.97), field of view %.1f deg\n", ev, ed.render_view.fov_deg);
                        if (fabsf(ev - 14.97f) > 0.1f || fabsf(ed.render_view.fov_deg - 28.0f) > 0.5f) printf("[RealEngine] FAIL: Cine camera\n"), exit_code = 1;
                    }
                    if (plugin_test) {
                        extern bool plugins_test_check(Editor * ed);
                        if (!plugins_test_check(&ed)) exit_code = 1;
                    }
                    if (async_test && !async_test_check(&ed)) exit_code = 1;
                    if (assistant_test && !assistant_test_check(&ed)) exit_code = 1;
                    if (g_look_test && !look_test_check(&ed)) exit_code = 1; // (Q26)
                    { const char* dr = demo_play_report(&ed.world); if (dr[0]) printf("[RealEngine] %s\n", dr); } // (a demo's game mode: its state)
                    if (!demo_play_check(&ed.world)) exit_code = 1;
                    if (stress_textures && !editor_stress_textures_check(&ed)) exit_code = 1;
                    if (ed.show_view2) { // Viewport 2: drawn every frame, at its window's size
                        printf("[RealEngine] Viewport 2: %ux%u, mode %u, %u frames drawn\n", ed.v2.w, ed.v2.h, ed.v2.mode, render_view2_frames());
                        if (!render_view2_frames()) printf("[RealEngine] FAIL: Viewport 2 drew nothing\n"), exit_code = 1;
                    }
                    if (open_asset && render_preview_stats()->drawn_frames) { // the asset editors' previews on the GPU (render_preview.c)
                        const PreviewStats* ps = render_preview_stats();
                        printf("[RealEngine] Previews: %u views, %llu triangles, GPU %.3f ms, CPU %.3f ms, %llu frames drawn, %u pictures made\n", ps->views,
                               (unsigned long long)ps->triangles, ps->gpu_ms, ps->last_cpu_ms, (unsigned long long)ps->drawn_frames, ps->made);
                    } else if (open_asset && (strstr(open_asset, ".rskm") || strstr(open_asset, ".rmesh") || open_viewport))
                        printf("[RealEngine] FAIL: the asset's preview drew nothing on the GPU\n"), exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "physics", 7) && !editor_physics_check(&ed, !strcmp(scene_name, "physics"))) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "physics_shapes") && !editor_shapes_check(&ed)) exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "phys_", 5) && !phys_test_check(&ed)) exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "terror_", 7) && !terror_test_check(&ed)) exit_code = 1; // (Q13)
                    if (scene_name && !strncmp(scene_name, "pcg_", 4) && !pcg_test_check(&ed)) exit_code = 1; // (Q15)
                    if (scene_name && !strncmp(scene_name, "udim", 4) && !udim_test_check(&ed, shot_path)) exit_code = 1; // (Q18)
                    if (scene_name && !strncmp(scene_name, "emit", 4) && !emissive_test_check(&ed, shot_path)) exit_code = 1; // (2026-11-23)
                    if (scene_name && !strcmp(scene_name, "facility_leak") && // (2026-11-25, item 19)
                        !facility_leak_check(&ed, shot_path, getenv("REALENGINE_FACILITY_BLOTCH_MAX") ? atof(getenv("REALENGINE_FACILITY_BLOTCH_MAX")) : 0.0,
                                             getenv("REALENGINE_FACILITY_MEAN_MIN") ? atof(getenv("REALENGINE_FACILITY_MEAN_MIN")) : 0.0))
                        exit_code = 1;
    if (scene_name && !strcmp(scene_name, "firstframe") && !mat_first_check(&ed)) exit_code = 1; // (2026-11-24)
                    if (scene_name && !strcmp(scene_name, "selection") && !selection_test_check(&ed)) exit_code = 1; // (2026-11-25, item 17)
                    if (groom_skin_test && !groom_skin_test_check(&ed)) exit_code = 1; // (2026-11-25, item 16)
                    if (scene_name && !strncmp(scene_name, "vista", 5) && !farview_test_check(&ed)) exit_code = 1; // (Q21)
                    if (scene_name && !strcmp(scene_name, "ai_senses") && !ai_senses_test_check(&ed)) exit_code = 1; // (Q24)
                    if (scene_name && !strcmp(scene_name, "river") && !editor_river_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "ai") && !editor_ai_check(&ed)) exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "ai_squad", 8) && !editor_ai_squad_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "landscape") && !editor_landscape_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "landscape_hm") && !editor_landscape_hm_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "proto") && !editor_proto_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "packs") && !editor_packs_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "autorig") && !editor_autorig_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "footik") && !editor_footik_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "ragdoll") && !editor_ragdoll_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "active_ragdoll") && !editor_active_ragdoll_check(&ed)) exit_code = 1;
                    if (scene_name && (!strcmp(scene_name, "socket_offset") || !strcmp(scene_name, "cloth_on_body") || !strcmp(scene_name, "ragdoll_q10") || !strcmp(scene_name, "getup_q10")) && !editor_q10_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "fps_camera") && !editor_q12_check(&ed)) exit_code = 1; // (Q12)
                    if (scene_name && !strcmp(scene_name, "vehicles") && !editor_vehicles_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "toon") && !editor_toon_check(&ed)) exit_code = 1; // (Q11)
                    if (scene_name && !strcmp(scene_name, "splines") && !editor_splines_check(&ed)) exit_code = 1; // (Q14)
                    if (scene_name && (!strcmp(scene_name, "sofa") || !strcmp(scene_name, "softbody") || !strcmp(scene_name, "softbody_rigid")) && !editor_soft_check(&ed, scene_name))
                        exit_code = 1; // (Q9: the soft bodies' volume, the box on the cushion, the sofa's cost)
                    if (scene_name && (!strcmp(scene_name, "procgen") || !strcmp(scene_name, "facility")) && !editor_procgen_check(&ed, scene_name[0] == 'f')) exit_code = 1;
                    if (scene_name && (!strcmp(scene_name, "wp") || !strcmp(scene_name, "wp_fast")) && !editor_wp_check(&ed)) exit_code = 1;
                    if (scene_name && (!strncmp(scene_name, "mm_", 3)) && !editor_mm_startstop_check(&ed)) exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "crowd", 5) && !editor_crowd_check(&ed)) exit_code = 1;
                    if (scene_name && !strncmp(scene_name, "panic", 5) && !editor_panic_check(&ed)) exit_code = 1; // (Q27.1)
                    if (scene_name && !strcmp(scene_name, "traffic") && !editor_traffic_check(&ed)) exit_code = 1; // (Q27.2)
                    if (scene_name && !strcmp(scene_name, "hair_crowd") && !editor_hair_crowd_check(&ed)) exit_code = 1; // (Q27.3)
                    if (scene_name && !strcmp(scene_name, "vehicle_crash") && !editor_vehicle_crash_check(&ed)) exit_code = 1; // (Q27.4)
                    if (scene_name && !strcmp(scene_name, "weather_regional") && !editor_weather_regional_check(&ed)) exit_code = 1; // (Q27.6)
                    if (scene_name && !strcmp(scene_name, "city_night") && !editor_city_night_check(&ed)) exit_code = 1; // (Q27.8)
                    if (scene_name && !strcmp(scene_name, "npc_variety") && !editor_npc_variety_check(&ed)) exit_code = 1; // (Q27.9)
                    if (scene_name && !strcmp(scene_name, "city_routine") && !editor_city_routine_check(&ed)) exit_code = 1; // (Q27.10)
                    if (scene_name && !strcmp(scene_name, "loco_styles") && !editor_mm_styles_check(&ed)) exit_code = 1; // (Q27.11)
                    if (scene_name && !strcmp(scene_name, "warp") && !editor_warp_check(&ed)) exit_code = 1; // (2026-11-23)
                    if (scene_name && !strcmp(scene_name, "traversal") && !editor_traversal_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "anim_warping") && !editor_anim_warping_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "combo") && !editor_combo_check(&ed)) exit_code = 1;
                    if (scene_name && !strcmp(scene_name, "cover") && !editor_cover_check(&ed)) exit_code = 1;
                    if (max_flicker > 0.0 && render_flicker() * 100.0 > max_flicker) {
                        printf("[RealEngine] FAIL: flicker %.3f%% > %.3f%%\n", render_flicker() * 100.0, max_flicker);
                        exit_code = 1;
                    }
                    if (max_pulses > 0.0 && render_flicker_pulses() * 100.0 > max_pulses) {
                        printf("[RealEngine] FAIL: flicker back and forth %.3f%% > %.3f%%\n", render_flicker_pulses() * 100.0, max_pulses);
                        exit_code = 1;
                    }
                    if (max_ghost > 0.0) {
                        double gsm = 0.0, gh = render_ghosting(NULL, &gsm) * 100.0;
                        if (gsm < 4.0) { // (no mover seen: the measure would pass on nothing)
                            printf("[RealEngine] FAIL: ghosting: no moving actor in the view (%.1f points a frame)\n", gsm);
                            exit_code = 1;
                        } else if (gh > max_ghost) {
                            printf("[RealEngine] FAIL: ghosting %.3f%% > %.3f%%\n", gh, max_ghost);
                            exit_code = 1;
                        }
                    }
                    if (probe_collisions) { // any measure: the probes near the floor reset each other and the actors flickered
                        printf("[RealEngine] FAIL: GI probes: %u updates found another cell of the grid in their slot\n", probe_collisions);
                        exit_code = 1;
                    }
                    if (cache_all && gi_cache_stats()->charts < gi_cache_stats()->candidates) { // (left on the probes: they flicker)
                        printf("[RealEngine] FAIL: surface cache: %u of %u meshes within reach got a chart\n", gi_cache_stats()->charts,
                               gi_cache_stats()->candidates);
                        exit_code = 1;
                    }
                    // (registers: the main pass, notas-dev/render.md). 16 bytes in mesh_rt and mesh_rt_t since the shadow split's
                    // second output, measured free (house Epic: main 1.82 vs 1.84 ms); the 96 of 0.15 cost 10%
                    // (2026-11-20) 32 in each since the mirrors' fog segment (weather.glsl fog_segment): house Epic main 1.643 vs
                    // 1.674 ms, metro High the same fps interleaved (405); its loop at the shader's end, its state in two registers
                    if (no_spill && render_shader_spill() > 64) {
                        printf("[RealEngine] FAIL: the main pass's shaders (mesh, mesh_rt, mesh_rt_t) spill %lld bytes to local memory\n",
                               (long long)render_shader_spill());
                        exit_code = 1;
                    } else if (no_spill && render_shader_spill() < 0)
                        printf("[RealEngine] Shader spill: unknown on this GPU (no statistic): not checked\n");
                    {
                        double lmax, lmean;
                        render_probe_lum(&lmax, &lmean);
                        printf("[RealEngine] Scene luminance (probe 64x32): brightest %.4g, mean %.4g\n", lmax, lmean);
                        const GiFallbackStats* gfs = gi_fallback_stats(); // (Q27.12)
                        if (gfs->ready) {
                            printf("[RealEngine] GI fallback: built %u times, %u boxes, mean sky visibility %.3f, %.2f ms a build on its thread\n", gfs->builds,
                                   gfs->boxes, gfs->mean_vis, gfs->build_ms);
                            printf("[RealEngine] GI fallback bounce: %u local lights, %u wall faces lit, %u sweeps, %.2f ms, light mean %.4g, brightest %.4g\n",
                                   gfs->fine_lights, gfs->fine_faces, gfs->fine_iters, gfs->fine_ms, gfs->fine_mean, gfs->fine_max);
                        }
                        if (max_scene_lum >= 0.0 && lmax > max_scene_lum) {
                            printf("[RealEngine] FAIL: the scene's brightest point %.4g > %.4g\n", lmax, max_scene_lum);
                            exit_code = 1;
                        }
                    }
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
    }
    if (hang_test && !hang_test_check()) exit_code = 1;
    if (check_gpu_marks) { // every frame done: its marks all written, the last pass finished, nothing reported stuck
        vkDeviceWaitIdle(vk.device);
        const char *started = NULL, *finished = NULL;
        uint32_t marks = 0;
        char full[1024], where[256];
        bool have = vkctx_gpu_marks_last(&started, &finished, &marks);
        vkctx_gpu_diagnosis(full, sizeof full, where, sizeof where);
        printf("[RealEngine] Marcadores da GPU: %s, %u passes no ultimo quadro, iniciado \"%s\", terminado \"%s\"\n",
               have ? "sim" : "nao", marks, started ? started : "-", finished ? finished : "-");
        if (!have || marks < 20 || !started || !finished || strcmp(started, "quadro terminado") || strcmp(finished, "fim do quadro") || where[0]) {
            printf("[RealEngine] FAIL: GPU markers (%s)\n%s", where, full);
            exit_code = 1;
        }
    }

    if (trace_path && trace_finish_check()) exit_code = 1; // (B14b) the file written and read back
    if (trace_open) {
        uint32_t ev = 0, fr = 0;
        bool ok = trace_window_loaded(&ev, &fr);
        printf("[RealEngine] Trace window: %s, %u events, %u frames\n", ok ? "opened" : "FAIL: not opened", ev, fr);
        if (!ok || fr < 10) exit_code = 1;
    }
    online_shutdown(); // (the session closed politely: the others see the player leave at once)
    services_on_exit(&ed);
    vkDeviceWaitIdle(vk.device);
    dock_layout_autosave(&ed); // (2026-11-20, crashes #143-146: the layout is ImGui's settings: saved before its context goes)
    editor_docs_finish_quit(&ed); // (2026-11-25) the asset editors closed while ImGui and the assets live (editor_docs.h)
    brand_shutdown(&vk); // (its textures are the UI's: before the UI goes)
    asset_viewer_shutdown(&vk);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    if (dock_test && !dock_test_after_shutdown()) exit_code = 1; // (item 9) no window of its own outlives the editor
    igDestroyContext(NULL);
    mcp_server_stop();
    editor_shutdown(&ed);
    if (quit_test) { // (the test editor_close: no crash on the way here, the layout written while ImGui lived)
        char lp[900] = "";
        FILE* lf = re_config_dir(lp, 700) ? (strcat(lp, "\\Layouts\\Last.rlayout"), fopen(lp, "rb")) : NULL;
        if (lf) fclose(lf), printf("[RealEngine] Editor closed cleanly (layout saved)\n");
        else printf("[RealEngine] FAIL: the editor closed without saving the layout (%s)\n", lp), exit_code = 1;
    }
    render_shutdown(&vk);
    re_arena_free(&g_frame_arena);
    if (ui_q) vkDestroyQueryPool(vk.device, ui_q, NULL), ui_q = VK_NULL_HANDLE;
    vkctx_shutdown(&vk);
    glfwDestroyWindow(window);
    glfwTerminate();
    fflush(stdout);
    return exit_code;
}
