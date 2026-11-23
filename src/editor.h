// The editor: Unreal's layout (menu bar, toolbar, Viewport, Outliner, Details, Content Drawer, status bar), its
// shortcuts and Play in Editor over the ECS world.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"
#include "render.h"
#include "level.h"

struct GLFWwindow;
struct VkCtx;

// ImGui's packed color (a C++ macro that cimgui.h doesn't have)
#ifndef IM_COL32
#define IM_COL32(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#endif

// A free camera, Unreal's axes (Z up, X forward at yaw 0, Y to the right), centimeters and degrees.
typedef LevelCamera EdCamera; // level.h: saved in the level

typedef enum EdTool { TOOL_SELECT, TOOL_MOVE, TOOL_ROTATE, TOOL_SCALE } EdTool;
// (2026-11-23) The gizmo's axes (the Tlender's Transform Orientation) and where it turns and scales (its Pivot Point)
enum { GIZMO_ORIENT_WORLD, GIZMO_ORIENT_LOCAL, GIZMO_ORIENT_VIEW, GIZMO_ORIENT_COUNT };
enum { GIZMO_PIVOT_ACTIVE, GIZMO_PIVOT_MEDIAN, GIZMO_PIVOT_BOUNDS, GIZMO_PIVOT_INDIVIDUAL, GIZMO_PIVOT_COUNT };

typedef struct FrameStats {
    double fps;       // averaged over the last half second
    double frame_ms;  // frame to frame
    double cpu_ms;    // the frame's CPU work (not the wait for the GPU / swapchain)
    double gpu_ms;    // GPU timestamps of the frame
} FrameStats;


#define RE_UNDO_MAX 100
#define EXPOSURE_COMP_DEFAULT 1.0f // stops over the physical camera (Unreal's auto exposure bias is 1 too)
#define RE_LOG_LINES 64

typedef struct Editor {
    struct GLFWwindow* window;
    // Landscape sculpting (landscape.h): the tool (LAND_TOOL_*, 0: off), the brush, a stroke going on
    int land_tool;
    float land_radius, land_strength;
    bool land_stroke;
    // Unreal's Landscape mode's Manage > New: from noise or from a heightmap file (its path; its size as the resolution)
    int land_new;
    char land_hm[600];
    bool land_hm_res;
    // The Foliage mode (foliage.h, Shift+3): painting with the brush (Shift or Erase: erasing), a stroke going on
    bool foliage_mode, fol_erase, fol_stroke;
    float fol_radius, fol_density;
    struct VkCtx* vk;
    World world;
    WorldSnapshot snapshot;
    Entity selected; // the primary selection (Details, the gizmo's pivot): the last one picked
    // Every selected actor (selected is the last of them); sel_mark: per entity slot, for a quick test. Code that sets
    // selected alone gets a selection of one (editor_sync_selection, every frame)
    Entity* sel;
    uint32_t nsel;
    uint8_t* sel_mark;
    CTransform* sel_start; // each one's transform when a gizmo drag began
    // The box selection (viewport or Outliner): dragging from the press point
    bool marquee, ol_marquee;
    int ol_press_row, ol_hover_row; // the Outliner's drag: the row pressed, the row under the mouse
    Entity ol_anchor;               // Shift+click selects the rows from it
    float marquee_p0[2];
    // The clipboard (Ctrl+C / Ctrl+X, Ctrl+V): the copied actors' component masks and bytes
    uint8_t* clip;
    size_t clip_len;
    uint32_t clip_count;
    // Shift+D's grab: the selection follows the mouse (left click keeps it, right click or Esc puts it back)
    bool grab, grab_started;
    float grab_p0[3];
    EdTool tool;
    EdCamera cam;
    // The main viewport's projection (Unreal's: Alt+G Perspective, Alt+J Top, Alt+H Front, Alt+K Left; with Shift the
    // opposite side): VP_PROJ_*; an orthographic one looks at vp_target with vp_ortho cm of half height
    int vp_proj;
    float vp_ortho, vp_target[3];

    // Play in Editor
    bool playing, paused, possessed, mouse_released;
    EdCamera cam_before_play; // the editor's camera, back when play ends
    EdCamera pie_cam;         // the player's view while possessed
    double play_time;
    double snapshot_ms, restore_ms; // the last snapshot taken / restored
    uint64_t snapshot_bytes;

    // Layout
    bool show_outliner, show_details, drawer_open, game_view, fullscreen;
    bool realtime;   // (the viewport's menu, Ctrl+R) animations and particles move in the editor without Play
    bool simulating; // (Simulate, Alt+S) the game runs without the player: the editor's camera, the selection, the gizmo
    float right_width;      // Outliner + Details column, pixels
    bool show_place;        // Unreal's Place Actors panel (on the left)
    float left_width;       // its width, pixels
    int place_tab;          // its category
    float outliner_frac;    // the Outliner's share of that column's height
    float drawer_frac;      // the Content Drawer's height over the viewport's
    int windowed_rect[4];   // x, y, w, h before F11
    float vp_rect[4];       // the viewport's x, y, w, h this frame (screen pixels)
    bool vp_hovered;
    RenderView render_view;  // what main.c renders this frame (filled by viewport_draw)
    uint32_t view_mode;      // ViewMode
    bool show_grid, show_stats, show_icons;
    // Viewport 2 (editor_view2.c): shown, its camera and projection (V2P_*), its mode (VIEW2_*), the orthographic views'
    // middle and half height, what render_view2 draws this frame (v2_on), its picture registered with the UI
    bool show_view2, v2_on;
    View2 v2;
    EdCamera cam2;
    int v2_proj, v2_mode;
    float v2_ortho, v2_target[3];
    uint32_t v2_gen;
    void* v2_tex;
    bool show_ai; // the AI overlay while playing (Unreal's gameplay debugger, the ' key): paths, sight, what each agent does
    bool show_nav; // the NavMesh in the viewport (Unreal's P): its polygons, tiles and Nav Links (navmesh.h)
    // Editor Preferences (project_ui.c; %APPDATA%/RealEngine/editor.ini)
    float ui_scale, mouse_sens, cam_fov;
    bool invert_y, less_cpu_in_background, play_possessed, browser_on_start, confirm_quit;
    int autosave_minutes, frame_limit, thumb_size;
    double autosave_at;
    bool show_prefs, show_project_settings, show_browser, quit_asking;
    bool show_package; // the Package Game window (project_ui.c, sugestão #37)
    char pending_project[600]; // the Project Browser's open, done between frames (main.c)
    int pending_tpl;
    int msaa;
    // The physical camera (render.h's RenderView): manual EV100 or auto between two, a compensation in stops
    float exposure_ev100, exposure_comp, auto_min_ev100, auto_max_ev100;
    bool auto_exposure;
    float aerial_scale; // the aerial perspective's distance scale (1: physical)
    GiSettings gi;          // Settings > Engine Scalability sets it; Settings > Global Illumination tweaks it
    // Snapping (the viewport toolbar): move in cm, rotate in degrees, scale steps
    bool snap_move, snap_rotate, snap_scale;
    float snap_move_step, snap_rotate_step, snap_scale_step;
    // The gizmo being dragged: GIZMO_NONE or the handle; the transform when it began
    int gizmo_hot, gizmo_active;
    CTransform gizmo_start;
    float gizmo_t0, gizmo_angle0, gizmo_angle; // gizmo_angle: the rotation shown (the pie)
    int gizmo_orient;                          // GIZMO_ORIENT_*: World, Local (the active actor's axes; Ctrl+` toggles), View
    int gizmo_pivot;                           // GIZMO_PIVOT_*: where rotate and scale turn (the active actor, the median...)
    bool gizmo_only_loc;                       // Only Locations: rotate and scale move the actors, never turn or size them
    float gizmo_size;                          // the gizmo's size on screen, px at 100% (Editor Preferences > Viewport)
    float gizmo_p0[3];
    bool flying;            // right mouse held in the viewport: WASD flies, the mouse looks
    double last_cursor[2];

    // Outliner
    char filter[64];
    // the Outliner's rows, made again only when the world's change log moved or the filter changed
    Entity* ol_rows;
    uint8_t* ol_depth;          // per row: its folder depth (the indent)
    uint32_t ol_nrows, ol_cap, ol_changes, ol_revision;
    uint32_t ol_folder_sel;     // the folder row selected (its id; 0: none)
    char ol_filter[64];
    bool ol_valid;
    Entity renaming;
    char rename_buf[RE_NAME_MAX];
    bool rename_focus;

    // Content Browser (content.c): a drawer over the viewport (Ctrl+Space) or docked under it
    bool content_docked;
    float content_docked_frac; // the docked panel's share of the viewport column's height

    // Undo / Redo: whole-world blobs (ecs_serialize), taken before each change; RE_UNDO_MAX steps
    struct UndoStep { uint8_t* data; size_t size; Entity selected; ReVmBuf mem; void* levels; } undo[RE_UNDO_MAX], redo[RE_UNDO_MAX]; // (levels: the Levels panel's list then, editor_levels.c)
    int undo_count, redo_count;
    struct UndoStep pending; // taken when a field starts being edited, kept if the edit ends with a change

    // The level file (.rlevel): its path ("" until saved) and whether it changed since
    char level_path[1024];
    bool dirty;
    char title_shown[256];

    // Output Log: the last status lines
    char log[RE_LOG_LINES][160];
    int log_count;
    bool show_log;

    FrameStats stats;
    char status_msg[160];
    double status_until;
    bool show_about;
    bool show_cloth; // Window > Cloth Simulation (cloth.h)
    bool show_open_asset;   // File > Open Asset (Ctrl+P)
    bool quit;
    int scalability; // 0 Low, 1 Medium, 2 High, 3 Epic (Unreal's Engine Scalability)
    int saved_scalability; // the preference written to editor.ini (-1: the current one; a test's --scalability is not kept)
    double bench_request; // Tools > Measure: seconds of a timed run main.c makes (the result in the status bar)
    bool measuring;       // a timed run going on (main.c): Use Less CPU in Background and the frame limit don't pace it
    int bp_test;          // --bp-test: 1 the possessed Character walks forward by itself, 2 stands still (bp_test.c)
    bool bp_test_house;   // --bp-test with --scene house: the Character walks through the house (bp_test.c)
    void (*test_step)(struct Editor* ed, double dt); // a test's step each frame while playing, before the Blueprints (char_test.c)
} Editor;

bool char_test_setup(Editor* ed); // --char-physics-test (char_test.c)
bool nav_test_setup(Editor* ed);  // --nav-test (nav_test.c)
bool nav_test_check(Editor* ed);
bool char_test_check(Editor* ed);
bool constraint_test_setup(Editor* ed); // --constraint-test (constraint_test.c)
bool pause_test_setup(Editor* ed, bool reload); // --pause-test / --pause-test-reload (pause_test.c)
bool pause_test_check(Editor* ed, bool reload);
extern double g_pie_tick_ms; // the Play's game step last frame, ms (editor.c: the pause's measure)
bool constraint_test_check(Editor* ed);
struct VkCtx;
bool health_test_setup(Editor* ed); // --health-test (health_test.c): the Health's self-test, then its nodes and events played
bool health_test_check(Editor* ed);
bool save_test_setup(Editor* ed); // --save-test (save_test.c): the progress's format self-test, then saved, changed, loaded, checked
bool save_test_check(Editor* ed);
bool save_test_done(void);
bool morph_test_setup(Editor* ed); // --morph-test (morph_test.c): morph targets set at run time, read back from the GPU
void morph_test_frame(Editor* ed, struct VkCtx* vk);
bool morph_test_done(void);
bool morph_test_check(Editor* ed);
bool inventory_test_setup(Editor* ed); // --inventory-test (inventory_test.c): stacks, weight, equipment, pickups, saves
bool inventory_test_check(Editor* ed);
bool interaction_test_setup(Editor* ed); // --interaction-test (interaction_test.c): focus, prompt, doors, keys, hold, lever, chest
bool interaction_test_check(Editor* ed);
bool dialogue_test_setup(Editor* ed); // --dialogue-test (dialogue_test.c): a conversation played, its rules, the widget, saves
bool dialogue_test_check(Editor* ed);
bool quest_test_setup(Editor* ed); // --quest-test (quest_test.c): a quest line played (every objective kind), the tracker, markers, saves, 200 quests
bool quest_test_check(Editor* ed);
bool rules_test_setup(Editor* ed); // --rules-test (rules_test.c): the phases, win by points, lose by time, respawn with lives
bool rules_test_check(Editor* ed);
bool ability_test_setup(Editor* ed); // --ability-test (ability_test.c): the rules, then an ability bar played: costs, cooldowns, tags, effects, saves
bool ability_test_check(Editor* ed);
bool ability_test_done(void);
bool hud_test_setup(Editor* ed); // --hud-test (hud_test.c): WBP_GameHUD, the bars, markers, feed, numbers, hiding, the cost
bool hud_test_check(Editor* ed);

bool editor_init(Editor* ed, struct GLFWwindow* window, struct VkCtx* vk);
// Unreal's Simulate (Alt+S): Play with the player not possessed (no pawn input): physics, AI, Blueprints, animations
// and particles run under the editor's camera; Stop (Esc) brings the level back as it was. Again: stops
void editor_simulate(Editor* ed, double now);
// A class of the Add menus / Place Actors dragged into the viewport (payload RE_PAYLOAD_PLACE): placed at that point
#define RE_PAYLOAD_PLACE "RE_PLACE"
typedef struct PlacePayload { int index; } PlacePayload;
void editor_place_drop(Editor* ed, int index, const float at[3]);
void editor_shutdown(Editor* ed);
// Builds the UI for this frame (between igNewFrame and igRender) and runs the systems of the playing world.
void editor_frame(Editor* ed, double dt, double now);
void editor_status(Editor* ed, double now, const char* fmt, ...);

// Play in Editor (Alt+P / Esc / F8 / Shift+F1 / Pause; the toolbar's buttons)
void editor_play(Editor* ed, double now);
void editor_stop(Editor* ed, double now);
int editor_pie_clients(void);          // (B13) the other players' windows this Play opened
void editor_pie_set_players(int n);    // (B13) Play's Number of Players (1-4)
void editor_play_multi(Editor* ed, double now); // (B13) Play with them (one: editor_play)
void editor_set_possessed(Editor* ed, bool possessed);

// Places a basic shape as a Static Mesh actor (dropped from the Content Browser, Add menus); selects it.
Entity editor_spawn_shape(Editor* ed, int shape, const float location[3], double now);
// (2026-11-10) a Camera Actor at a place and a view (the Sequencer's Create Camera): selected, undoable
Entity editor_spawn_camera(Editor* ed, const float location[3], float pitch, float yaw, double now);
// The selection (several actors; ed->selected is the primary one, the last picked)
#define ED_MAX_SEL (1u << 18)
typedef enum SelMode { SEL_REPLACE, SEL_ADD, SEL_TOGGLE, SEL_REMOVE } SelMode;
bool editor_is_selected(const Editor* ed, Entity e);
void editor_select(Editor* ed, Entity e, SelMode mode);
void editor_select_none(Editor* ed);
void editor_select_all(Editor* ed);
void editor_sync_selection(Editor* ed); // selected set alone -> a selection of one; dead actors out
void editor_grab_begin(Editor* ed); // editor_viewport.c
Entity viewport_pick_ray(Editor* ed, const float o[3], const float d[3]); // the actor whose meshes' boxes a ray meets first
void view2_window(Editor* ed, double dt); // editor_view2.c: Window > Viewport 2
// Window > Levels (editor_levels.c, sublevels.h): sublevels and stackable Lighting Levels
void levels_frame(Editor* ed, double now);      // the new actors into the current level; the panel
bool* levels_show_flag(void);
void levels_on_open(Editor* ed);                // a level opened: its sublevels in, its lighting summed
void levels_on_new(Editor* ed);
void levels_resync(Editor* ed);                 // the world replaced (undo): what is there isn't new
bool levels_save(Editor* ed, const char* path, size_t* bytes); // the persistent level, the changed sublevels, the list
bool levels_lighting_summed(void);              // the lightmaps shown are a sum (the level's .rlight is written by the bake)
bool levels_locked(Editor* ed, Entity e);       // in a locked sublevel: not selected
bool levels_in_current(Editor* ed, Entity e);   // (2026-11-17) in the current level (true: no sublevels)
// (2026-11-17, Q3) the Outliner by level: its header (the level picked, "All Levels"), what it lists, when to list again
void levels_outliner_header(Editor* ed);
bool levels_outliner_shows(Editor* ed, Entity e);
uint32_t levels_outliner_revision(void);
int levels_outliner_level(void);          // -2 all, -1 the persistent level, else a sublevel's slot
void levels_outliner_set_level(int level);
bool levels_panel_focused(void);          // (Del there removes the picked sublevel, not the selected actors)
// the undo steps keep the list (a removed or reordered sublevel comes back with Ctrl+Z)
void* levels_undo_take(void);
void levels_undo_free(void* p);
void levels_undo_apply(Editor* ed, const void* p);
bool levels_build_lighting(Editor* ed, int quality, double now); // true: it bakes (one bake per Lighting Level)
bool levels_bake_finished(Editor* ed, double now);
void levels_play_begin(Editor* ed);
void levels_play_open(Editor* ed, const char* path);
void levels_play_end(Editor* ed);
bool editor_bake_start(Editor* ed, int quality, double now); // (editor.c) Build Lighting's bake started, its card shown
void editor_undo_clear(Editor* ed);
struct SublevelSet;
struct SublevelSet* levels_set(void); // (levels_test.c, --levels-test) the open level's sublevels
bool levels_create(Editor* ed, const char* name, double now);
void levels_assign_now(Editor* ed);
void levels_set_current(Editor* ed, int slot);
void levels_move_selected(Editor* ed, int slot, double now);
void levels_remove(Editor* ed, int slot, double now); // (the test: as the dialog's Remove)
void levels_move_slot(Editor* ed, int from, int to);  // (the test: a card dragged)
bool levels_baking(void);
bool levels_test_setup(Editor* ed); // --levels-test (levels_test.c): sublevels saved, opened, moved, hidden; Lighting Levels baked and summed
bool levels_test_check(Editor* ed);
// Sky presets (Quick Add > Visual Effects > Sky Preset): the sun, the sky light, the clouds and the fog set together
#define SKY_PRESET_COUNT 7
typedef struct SkyPreset {
    const char* name;
    float sun_pitch, sun_yaw, sun_lux, sun_color[3];
    float coverage, cloud_density, detail, cloud_shadow;
    float fog_density, fog_falloff, fog_albedo[3], fog_anisotropy, fog_emissive[3];
} SkyPreset;
extern const SkyPreset g_sky_presets[SKY_PRESET_COUNT];
void editor_sky_preset(Editor* ed, int preset, double now);
void editor_add_test_floor(Editor* ed, float size); // --add-floor: the templates' floor, in memory only
void editor_test_scene(Editor* ed, const char* name); // the GI tests' scenes (--scene house)
// --scene physics / physics_stress with --play --bench: the bodies resting on the floor (and the box broken)
bool editor_physics_check(Editor* ed, bool expect_fracture);
bool editor_river_check(Editor* ed); // (B12c, B12f: --scene river --play)
bool editor_shapes_check(Editor* ed); // --scene physics_shapes: each shape at rest as it should
bool editor_ai_check(Editor* ed); // --scene ai: the agents and the navigation grid
bool editor_ai_squad_check(Editor* ed);
bool editor_landscape_check(Editor* ed);
bool editor_landscape_hm_check(Editor* ed); // --scene landscape_hm: a heightmap imported, the Foliage brush
bool editor_proto_check(Editor* ed);
bool editor_packs_check(Editor* ed);
bool editor_mm_startstop_check(Editor* ed); // --scene mm_startstop: Motion Matching's made starts and stops played, the feet in place
void editor_mm_startstop_tick(Editor* ed, double dt); // (its script: the Character's input)
bool editor_autorig_check(Editor* ed); // --scene autorig: a T-posed statue rigged onto the Mannequin, walking // --scene packs: the animation packs made, their characters playing // --scene proto: the blockout's boxes, a Character on the platform, a path up the stairs // --scene landscape: its chunks and plants, a Character standing on it // --scene ai_squad: shared knowledge, prediction, the squad's ring
bool editor_footik_check(Editor* ed); // --scene footik: the left foot on the step
void editor_landscape_paint_tick(Editor* ed, double dt); // --scene landscape_paint: the layers painted (B11)
void editor_wp_tick(Editor* ed, double dt);               // --scene wp: the player carried east over 4 km, the Data Layers switched
bool editor_wp_check(Editor* ed);                         // ... the cells, the HLOD, the layers, no hitch
void editor_procgen_tick(Editor* ed, double dt);          // --scene procgen: the player carried east
bool editor_procgen_check(Editor* ed, bool facility);     // ... the chunks, the rebases, the rooms
void editor_snow_tick(Editor* ed, double dt);     // --scene snow: the Walker leaving prints
void editor_vehicles_tick(Editor* ed, double dt); // --scene vehicles: the script driving them
void editor_ghost_tick(Editor* ed, double dt);    // (Q20) --scene house_ghost: the pillar sweeping across the view
bool editor_vehicles_check(Editor* ed);          // ... each did its part
void editor_soft_tick(Editor* ed, double dt);    // (Q9) --scene sofa / softbody / softbody_rigid: their script (playing)
bool editor_soft_check(Editor* ed, const char* scene); // ... the soft bodies' volume, the box's sink, the sofa's cost
bool editor_ragdoll_check(Editor* ed);
void editor_hair_tick(Editor* ed, double dt); // --scene hair_dynamic (2026-11-16)
void editor_skin_churn_tick(Editor* ed);       // --scene skin_churn (2026-11-16, crashes #135-137)
void editor_active_ragdoll_tick(Editor* ed, double dt); // --scene active_ragdoll (2026-11-16)
// (Q10, editor_q10.c) --scene socket_offset, cloth_on_body, ragdoll_q10: made, moved each frame, checked at the end
void editor_q10_scene(Editor* ed, const char* name);
void editor_q10_tick(Editor* ed, double dt);
bool editor_q10_check(Editor* ed);
// (Q11, editor_toon.c) --scene toon: the Toon shading model and the Auto Adaptive Outline, checked at the end
void editor_toon_scene(Editor* ed);
bool editor_toon_check(Editor* ed);
// (Q14) --scene splines (editor_spline.inc): the road, the pipe, the fence, the cables; Play turns the arm; the check
void editor_splines_tick(Editor* ed, double dt);
bool editor_splines_check(Editor* ed);
// (Q12, fpcam_test.c) --scene fps_camera (the First Person template's player in Play: autofocus, bob on the steps, lean,
// shakes, recoil, cost) and --fpcam-test <name> (fp_bob_sync, camshake_determinism, fp_autofocus, fp_land_recoil)
void editor_q12_scene(Editor* ed, const char* name);
void editor_q12_tick(Editor* ed, double dt);
bool editor_q12_check(Editor* ed);
bool fpcam_selftest_named(const char* name);
bool editor_active_ragdoll_check(Editor* ed); // --scene ragdoll: the ragdolls at rest, the arms hanging, the hands on their goals
bool editor_crowd_check(Editor* ed);  // --scene crowdN: the crowd closed in on the player
void editor_mm_styles_tick(Editor* ed, double dt); // (Q27.11) --scene loco_styles: walking, the style switched every 2 s
bool editor_mm_styles_check(Editor* ed); // (Q27.11) no pop, the tired one bows, the layer's cost
void editor_warp_tick(Editor* ed, double dt); // (2026-11-23) --scene warp: root motion, a warp to a target, a paired montage
bool editor_warp_check(Editor* ed);
void editor_traversal_tick(Editor* ed, double dt); // (2026-11-23) --scene traversal: Vault, Mantle, Climb, None
bool editor_traversal_check(Editor* ed);
void editor_anim_warping_tick(Editor* ed, double dt); // (2026-11-23) --scene anim_warping: Orientation and Stride Warping
bool editor_anim_warping_check(Editor* ed);
void editor_combo_tick(Editor* ed, double dt); // (2026-11-23) --scene combo: a montage's Combo and Hit windows
bool editor_combo_check(Editor* ed);
void editor_cover_tick(Editor* ed, double dt); // (2026-11-23) --scene cover: in, crouch, along to the end, peek, out
bool editor_cover_check(Editor* ed);
bool editor_mesh_socket_test(Editor* ed);
bool editor_rig_fit_test(Editor* ed); // (2026-11-23) the rig found, the physics fitted to the skin (humanoid.glb) // (2026-11-23) a Static Mesh's sockets and its saved material (after --import)
void editor_city_routine_tick(Editor* ed, double dt); // (Q27.10) --scene city_routine: the clock, the shop, the crime
bool editor_city_routine_check(Editor* ed); // (Q27.10) the shop by its hours, the witness calls the police
bool editor_npc_variety_check(Editor* ed); // (Q27.9) --scene npc_variety: 200 agents, nearly all looking different
void editor_city_night_tick(Editor* ed, double dt); // (Q27.8) --scene city_night: the headlight by the eye, then off to the side
bool editor_city_night_check(Editor* ed); // (Q27.8) the retroreflective sign sends the headlight back
void editor_weather_regional_tick(Editor* ed, double dt); // (Q27.6) --scene weather_regional: the camera here, at the storm, back
bool editor_weather_regional_check(Editor* ed); // (Q27.6) dry here, raining there, the water kept 10 minutes
void editor_vehicle_crash_tick(Editor* ed, double dt); // (Q27.4) --scene vehicle_crash: full throttle
bool editor_vehicle_crash_check(Editor* ed); // (Q27.4) the crashes: no overlap, dents, power lost, bent wheels, the cost
bool editor_hair_crowd_check(Editor* ed); // (Q27.3) --scene hair_crowd: the budget, the simulated ones
bool editor_traffic_check(Editor* ed); // (Q27.2) --scene traffic: queues at the reds, no overlaps, flow, the cost
bool editor_panic_check(Editor* ed); // (Q27.1) --scene panicN: the frames after a Crowd Alarm, the wave, the promotion budget
// A Tlender .tlend into the project: assets in folder_vpath ("/Content/Name"), its objects placed in the level
bool editor_import_tlend(Editor* ed, const char* src, const char* folder_vpath, bool place); // place: actors in the level too
// The texture streaming's stress (--stress-textures): n cubes in a row, each with its own 4096 Material Instance
void editor_stress_textures(Editor* ed, uint32_t n);
bool editor_stress_textures_check(Editor* ed);
void editor_shadow_lights(Editor* ed, uint32_t n); // --shadow-lights: n point lights casting shadows inside the scene's meshes
void editor_stress_scene(Editor* ed, uint32_t objects, uint32_t lights, float cast); // the scale tests (cast: the share of
                                                                                    // lights with Cast Shadows)
// "Cube" -> "Cube", or "Cube2", "Cube3"... when the name is taken (Unreal's labels)
void editor_unique_name(Editor* ed, const char* base, char* out, size_t cap);
// A file imported in the background (async.h: a card at the bottom right); its actors placed when it is done
void editor_import_async(Editor* ed, const char* src, const char* folder_vpath, bool place);

// The viewport (editor_viewport.c): the camera, the grid and the actors, picking and flying.
void viewport_draw(Editor* ed, float x, float y, float w, float h, double dt);
void viewport_focus(Editor* ed, Entity e);
enum { VP_PROJ_PERSPECTIVE = 0, VP_PROJ_TOP, VP_PROJ_BOTTOM, VP_PROJ_LEFT, VP_PROJ_RIGHT, VP_PROJ_FRONT, VP_PROJ_BACK, VP_PROJ_COUNT };
extern const char* const g_vp_proj_names[VP_PROJ_COUNT];
// The main viewport's projection: an orthographic one starts looking at what the perspective camera looked at
void viewport_set_projection(Editor* ed, int proj);
// The Construction Scripts in the editor (editor_construct.c, sugestão #64): run on the Blueprint actors that changed
// (each frame, not playing); the authored data put back before saving, playing or keeping an undo step; forgotten when
// the world is replaced (undo, another level, play ended); this frame's runs in ms
// The object cost view (editor_cost.c, sugestão #65): each frame (its lists a few times a second while View Mode >
// Object Cost is on, the panel over the viewport); the object the viewport's mouse (or middle) points at
struct DvContext;
void editor_cost_frame(Editor* ed, const struct DvContext* c);
void editor_cost_set_hover(Entity e);
bool editor_cost_on(void);
void editor_construct_update(Editor* ed);
void editor_construct_restore(Editor* ed);
void editor_construct_forget(void);
double editor_construct_ms(void);
// Unreal's rotation matrix: axes[0] forward (X), axes[1] right (Y), axes[2] up (Z) of a pitch/yaw/roll in degrees
// (render.c), and back (Unreal's FMatrix::Rotator).
void rotator_axes(const float rot_deg[3], float axes[3][3]);
void axes_rotator(const float axes[3][3], float rot_deg[3]);
// Called before a change the user can undo (a gizmo drag, a Details edit): the undo history keeps the world as it was
void editor_undo_push(Editor* ed);
void editor_undo_drop(Editor* ed); // (2026-11-23) the last step dropped unapplied (a cancelled gizmo drag)
void editor_undo(Editor* ed, double now);
void editor_redo(Editor* ed, double now);

// Levels (.rlevel: a header with the editor camera, then the world's blob). Path "" asks with the Save dialog.
bool editor_save_level(Editor* ed, const char* path, double now);
// Unreal's Save All (Ctrl+Shift+S): the level and every changed asset
void editor_save_all(Editor* ed, double now);
bool editor_open_level(Editor* ed, const char* path, double now);
// Build > Package Game into out (a folder; the game gets its own folder in it); show: open it in Explorer after
bool editor_package_game(Editor* ed, const char* out, bool show, double now);
bool editor_package_game_async(Editor* ed, const char* out, bool show, double now);
// Build > Package for Android (notas-dev/android.md): the package, then an APK beside it (async: on a worker, a card)
bool editor_package_android(Editor* ed, const char* out, bool show, bool async, double now);
// play: the Android App Bundle for Google Play (and the APK of the same module beside it), signed with the upload key;
// the version code raised after it (notas-dev/android.md "Google Play")
bool editor_package_android_as(Editor* ed, const char* out, bool show, bool async, bool play, double now);
bool editor_package_quest(Editor* ed, const char* out, bool show, bool async, double now); // (Q25) Build > Package for Meta Quest
void editor_vr_preview(Editor* ed, double now); // (Q25) Play > VR Preview
bool editor_android_upload_key(Editor* ed, double now); // <project>/Build/Android/upload.keystore, set in the project // (a card at the bottom right: the copies on a worker)

// Projects (project.h): open a .rproject (tpl >= 0: a new one, its level built from that template and saved);
// every system goes to its Content
bool editor_open_project(Editor* ed, const char* rproject, int tpl);
// The Project Browser's open: done by main.c between frames (the loading screen draws its own frames meanwhile)
void editor_open_project_later(Editor* ed, const char* rproject, int tpl);
// A load's progress (main.c: the loading screen; NULL: nothing to show) and the project's own splash (its path)
extern void (*g_load_progress)(float progress, const char* status);
extern void (*g_load_splash)(const char* path);
void editor_build_template(Editor* ed, int tpl);
// The windows of project_ui.c: the Project Browser (before the editor, File > Open Project), Edit > Project Settings,
// Edit > Editor Preferences. project_ui_frame draws the open ones; true: the browser covers the editor this frame.
bool project_ui_frame(Editor* ed, double now);
bool project_create(Editor* ed, const char* folder, const char* name, int tpl); // folder/name/name.rproject
// The actor's own picture (Outliner, viewport icons): an imported Static Mesh's model, a Skeletal Mesh in its rest pose
// (its T pose); false when it has none (or not yet): the caller draws the type's icon
bool editor_actor_thumb(World* w, Entity e, struct ImDrawList* dl, float x0, float y0, float x1, float y1);
void project_save_thumbnail(Editor* ed); // <project>/Saved/Thumbnail.rthumb: the level's look (on save)
void project_ui_browser_refresh(void);
void project_ui_show_games(void); // the browser on the templates
void project_ui_pref_page(int page);  // Editor Preferences opened on that page (5: Graph Style)
void project_ui_open_failed(const char* rproject); // a project the browser asked for didn't open (its message, the recent list)
// The settings the preference windows change (editor.c's)
void editor_set_vsync(Editor* ed, bool on, double now);
void editor_set_scalability(Editor* ed, int level, double now);
void editor_prefs_changed(void); // editor.ini written at the end of the frame
// Closing the editor: false when it asks first (Confirm before quitting with unsaved changes: a dialog)
bool editor_request_quit(Editor* ed);

// Preferences in %APPDATA%\RealEngine\editor.ini (language, VSync, scalability, stats)
void editor_load_prefs(Editor* ed);
void editor_save_prefs(Editor* ed);
extern bool g_editor_prefs_readonly; // an automated run (the tests): editor.ini never written

// A camera's Post Process Settings in the Details (the Camera Actor's and a Blueprint's Camera Component). True: changed
struct CCamera;
bool editor_camera_post_ui(struct CCamera* c);
bool fpcam_ui(struct CCamera* c); // (Q12, fpcam_ui.c) the First Person Camera's Details under the camera's
