// The project (notas-dev/projetos.md): a folder with <Name>.rproject (JSON: Unreal's Project Settings) and Content/.
// The editor opens one from the Project Browser (project_ui.c); the packaged game reads its copy (Game.rproject).
// Without one (the tests, the folder next to the executable) the defaults stand.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

enum { PROJECT_RENDER_PERFORMANCE = 0, PROJECT_RENDER_QUALITY };

enum { PKG_WINDOWS = 0, PKG_ANDROID_APK, PKG_ANDROID_PLAY, PKG_PLATFORM_COUNT };

// (Q25) VR (xr.h, vr_pawn.h, notas-dev/xr.md): what the packaged game does on a headset and how the player's pawn
// moves in it (the Unreal VR Template's choices). Its game.ini keys: vr, vr_refresh, vr_tracking, vr_quality, vr_foveation,
// vr_foveation_dynamic, vr_spacewarp, vr_render_scale (the pawn's read from the packaged Game.rproject)
enum { VR_TRACK_FLOOR = 0, VR_TRACK_SEATED };
enum { VR_LOCO_TELEPORT = 0, VR_LOCO_SMOOTH, VR_LOCO_BOTH };
enum { VR_TURN_SNAP = 0, VR_TURN_SMOOTH, VR_TURN_NONE };
enum { VR_HEAD_NONE = 0, VR_HEAD_FADE, VR_HEAD_PUSH };
enum { VR_MIRROR_BOTH = 0, VR_MIRROR_ONE_EYE, VR_MIRROR_SPECTATOR };
enum { VR_GESTURE_FIST = 0, VR_GESTURE_PINCH, VR_GESTURE_ANY_TO_INDEX, VR_GESTURE_FIST_OR_TOUCH, VR_GESTURE_MODES };
enum { VR_QUALITY_LOW = 0, VR_QUALITY_MEDIUM, VR_QUALITY_HIGH, VR_QUALITY_EPIC, VR_QUALITY_QUEST, VR_QUALITY_COUNT };
// (Q25b) mixed reality (xr_mr.h): what replaces the level's sky, the passthrough's style, the occlusion, the light
enum { MR_BG_ROOM = 0, MR_BG_LEVEL };
enum { MR_STYLE_COLOR = 0, MR_STYLE_MONO, MR_STYLE_TINT, MR_STYLE_ADJUSTED, MR_STYLE_COUNT };
enum { MR_OCC_NONE = 0, MR_OCC_ROOM, MR_OCC_DEPTH };
enum { MR_LIGHT_ROOM = 0, MR_LIGHT_LEVEL };
typedef struct ProjectVr {
    bool start_in_vr;        // the packaged game opens on the headset (none connected: the monitor)
    bool pc, quest;          // the platforms: PC VR (OpenXR: Quest Link / Air Link, SteamVR), Meta Quest standalone (its APK)
    int refresh_hz;          // 72, 90, 120 (0: the headset's own)
    int tracking;            // VR_TRACK_FLOOR (standing, room scale), VR_TRACK_SEATED (local: the seated height added)
    float seated_height;     // cm the seated player's eyes are raised to
    int locomotion;          // VR_LOCO_*
    int turn;                // VR_TURN_*
    float snap_angle;        // degrees a snap turn
    float turn_speed;        // degrees a second (smooth)
    float move_speed;        // cm a second (smooth)
    float teleport_range;    // cm the teleport's arc reaches
    float vignette;          // the comfort vignette while moving or turning (0: none, 1: strong)
    bool hands;              // the hands shown (posed by the trigger and the grip; tracked hands follow the fingers)
    bool hand_tracking;      // the headset's hand tracking when it has it (XR_EXT_hand_tracking)
    bool laser;              // a laser from the hand points at widgets and Interactables
    bool drag_locomotion;    // a hand closed in the air (the fist, the grip) holds the world: the player pulls along
    bool physical_hands;     // the drawn hands stop at walls, floors and tables (where the real hand is, the level allowing)
    bool hand_objects;       // ... and at what moves: bodies with physics, Interactables (Physical Hands on; off: the level only)
    bool hand_push;          // the hands push the bodies with physics they touch (by how deep the real hand goes in, or its speed)
    bool held_collide;       // what a hand holds stops at walls too
    int head_collision;      // VR_HEAD_*: the head into a wall: nothing, the view fades to black, or the player pushed back
    bool hide_body;          // the pawn's own meshes (its body, a mannequin) hidden in VR: only the hands seen
    char hand_mesh[2][128];  // a Static Mesh for each hand ("/Content/...rmesh"; "": the engine's hands), its scale and rotation
    float hand_mesh_scale;
    float hand_mesh_rot[3];
    int dominant_hand;       // 0 right, 1 left (left-handed: the sticks' roles and the laser's hand swapped)
    int move_direction;      // smooth walking toward 0 the head, 1 the moving hand's pointing
    float teleport_fade;     // seconds of the blink to black at a teleport (0: none)
    bool turn_fade;          // a short blink at each snap turn
    float grab_reach;        // cm from the hand a Grabbable is taken
    bool distance_grab;      // the grip on what the laser points at (Grabbable) pulls it into the hand
    float throw_scale;       // the throw's speed times this
    float haptics;           // the controllers' vibration strength (0: off)
    float laser_length;      // cm
    float menu_distance, menu_width; // cm: the menu panel before the head, its width
    int msaa;                // the VR view's MSAA (1, 2, 4, 8; 0: 4)
    bool safety_net;         // the pawn fallen out of the level (or falling 2 s) is put back where it last stood
    int mirror_mode;         // VR_MIRROR_*: what the PC's window shows (recording, streaming)
    int grab_gesture, drag_gesture; // VR_GESTURE_*: what a tracked hand does to grab, to drag the world (controllers: the grip)
    float spectator_fov;     // degrees across the Spectator Camera's view
    float spectator_smooth;  // how slowly it follows the head (0: as the head; 1: very smooth)
    int headroom_fps;        // the frame rate the GPU's two eyes are kept able to reach (150 at 120 Hz: room to spare; 0: the display's)
    int quality;             // VR_QUALITY_* (Quest: the preset made for the Quest's Adreno)
    int foveation;           // fixed foveated rendering: 0 off, 1 low, 2 medium, 3 high
    bool foveation_dynamic;  // deeper only while the GPU is short of its time
    bool spacewarp;          // Application SpaceWarp (Quest): half the frames rendered, the rest made by the headset
    float render_scale;      // the eyes' resolution over the headset's recommended (0.5..1.5)
    // (Q25b) Mixed Reality (xr_mr.h; Quest 3 / 3S / Pro, PC by Link with Developer Runtime Features)
    bool mr_enabled;         // the passthrough's, the room's and the real depth's extensions asked for (off: pure VR)
    bool mr_passthrough;     // the real world seen (Set Passthrough Enabled changes it while playing)
    int mr_background;       // MR_BG_*: the room in the level's sky's place, or the level's sky kept (only windows show it)
    float mr_opacity;        // the passthrough's opacity (0..1)
    int mr_style;            // MR_STYLE_*: color, black & white, one color (tint), adjusted (brightness, contrast, saturation)
    float mr_brightness;     // -100..100
    float mr_contrast, mr_saturation; // 1: as the cameras see
    float mr_tint[3];        // the one color's (linear)
    float mr_edge[4];        // the edges drawn over the passthrough (a = 0: none)
    float mr_virtual_opacity; // the virtual things over it (1 solid; lower: the room through them)
    bool mr_scene;           // the room (the headset's Space Setup) loaded at the start
    bool mr_scene_actors;    // its planes and volumes as actors that collide (never drawn)
    bool mr_room_mesh;       // the room's mesh too (line traces on any real thing)
    bool mr_ask_scan;        // no room yet: the headset's Space Setup opened
    bool mr_align;           // the level turned to the room's walls (its X along the longest wall)
    int mr_occlusion;        // MR_OCC_*: none, the room's planes and volumes, the real depth (the room without it)
    float mr_occlusion_soft; // cm of the soft edge
    bool mr_hand_removal;    // the real depth without the hands
    float mr_shadows;        // the virtual things' shadows on the real surfaces (0: none .. 1)
    int mr_light;            // MR_LIGHT_*: a room's light (the sun from above, warm, the sky low), or the level's
    float mr_ev100;          // the exposure while the room is seen (EV100; 0: automatic, the virtual pixels metered: the room's black left out)
    bool mr_windows_real;    // the room's real windows and doors show through the virtual walls
    bool mr_hide_floor;      // the level's floor planes not drawn (still colliding) while the room is seen
    bool mr_depth_flip;      // the real depth's rows from the bottom (a runtime's convention)
    bool mr_unpremultiplied; // the eyes' alpha given unpremultiplied (the compositor's other convention: the edges' dark ring)
} ProjectVr;

typedef struct ProjectSettings {
    // Project - Description
    char name[64], company[64], version[16], description[256];
    // Project - Maps & Modes ("/Content/..." paths; Blueprint class names, "" for the engine's)
    char editor_startup_map[256], game_default_map[256];
    char game_mode[64], default_pawn[64], player_controller[64], hud[64], game_state[64], player_state[64], game_instance[64];
    // Engine - Physics
    float gravity_z;          // cm/s² (positive: down; Unreal's -980 as a magnitude)
    int max_substeps;         // the physics' fixed 120 Hz steps per frame at most
    int solver_substeps;      // (Q8) the solver's substeps in each 120 Hz step (4: 480 Hz; more: stiffer stacks and chains)
    int physics_rate;         // (Q8 leftover, 2026-11-19) the fixed steps a second (Hz): Physics Quality Epic 120, High 90,
                              // Medium and Low 60 (notas-dev/fisica.md "20 mil corpos")
    // Engine - Input (GLFW key codes) and the mouse
    int key_forward, key_back, key_left, key_right, key_jump;
    float mouse_sensitivity;  // degrees per pixel
    bool invert_mouse_y;
    char default_imc[48];     // Enhanced Input: the Input Mapping Context every play starts with ("": none)
    // Engine - Rendering (the packaged game's defaults)
    int game_scalability;     // 0 Low .. 3 Epic, 4 Cinematic
    // the Rendering Mode (sugestão #45), chosen when the project is made and changed here: Performance (250+ fps, the
    // base for VR and weaker PCs) or Quality (Cinematic scalability, ~90-120 fps; the path tracer playable in the game)
    int render_mode;          // PROJECT_RENDER_PERFORMANCE or PROJECT_RENDER_QUALITY
    bool game_path_tracing;   // Quality: the game drawn by the path tracer (denoised, reprojected; needs ray tracing)
    bool ray_tracing, static_lighting, auto_exposure;
    // upscaling (upscale.h, notas-dev/upscaling.md): the upscaler (UPS_*: TSR, or FSR / DLSS / XeSS when their runtime is
    // there), its mode (UPM_*: the render's share), the dynamic resolution and its frame rate target
    int upscaler, upscale_mode;
    int render_style;         // render.h RENDER_STYLE_* (A9.0): Stylized, Realistic (the default), Photorealistic, Video-realistic
    // (2026-11-10) the Stylized style under the user's control (Project Settings > Rendering > Stylized): the light in
    // bands (how many, how soft), the colors (saturation, the shadows' tint), the outline (width px, 0 off; color; how
    // far apart in depth an edge is), the animations' own frame rate (0 smooth; 12: stop-motion, "on twos")
    int stylized_bands;
    float stylized_softness, stylized_saturation, stylized_shadow_tint[3];
    float stylized_outline, stylized_outline_color[3], stylized_outline_depth;
    int stylized_anim_fps;
    // (2026-11-10) the picture's noise: the film grain's amount (0 none, 1 the style's, up to 2) and the denoiser of the
    // traced light (0 Automatic: Photorealistic and Video-realistic; 1 On in every style; 2 Off)
    float film_grain;
    // (2026-11-10, "opções extras a todos os estilos") the shadows' denoise (0 Low, 1 Medium: as before, 2 High, 3 Epic:
    // wider and longer, a little softer contact), and the Video-realistic camera's lens: its distortion, fringes,
    // halation and vignette as multipliers of the style's own (1: as the style is, 0: none)
    int shadow_denoise;
    float video_distortion, video_fringe, video_halation, video_vignette;
    int image_denoise;
    bool async_compute;       // (A9.7c) the particles' simulation on the GPU's compute queue beside the graphics one; off by default
    bool vrs;                 // variable rate shading (A9.7b, render.c): coarser shading where last frame showed little detail; off by default
    // (Q20, notas-dev/gi-estavel.md) the temporal filters' stability: the TSR drops the history a moving actor uncovers
    // (last frame's depth, tsr.comp); the denoiser's short histories take their converged neighbors (ReBLUR's history
    // fix, denoise.comp). On by default
    bool tsr_disocclusion, den_history_fix;
    bool dyn_res;
    int dyn_res_fps;
    int dyn_res_min; // (2026-11-16) Dynamic Resolution Min, % of the output (0: Auto: 70% of the mode, never under 50%)
    float exposure_compensation;
    // Platforms - Windows (the packaged game's window)
    char window_title[64];
    int resolution_w, resolution_h;
    bool fullscreen, vsync, mouse_capture;
    bool hdr_output;            // (A9.4) the game in HDR10 when the display has it (game.ini hdr=1)
    float hdr_paper, hdr_peak;  // its paper white and peak (nits: 200, 1000)
    // Packaging
    char package_dir[512];
    bool include_all_assets;  // every asset of Content, not only what the level uses
    // Packaging (sugestão #37: the Package Game window has all of it): the executable's name ("": the project's), its
    // icon (a .ico, .png, .jpg, .bmp or .tga under Content: "/Content/..."; "": the engine's), the copyright in its
    // version, Shipping (no Debug Viewer, no measuring flags in the game)
    char game_name[64], icon[256], copyright[96];
    bool shipping;
    // (2026-11-10, "deixar explícito a plataforma") Package Game's target: PKG_WINDOWS, PKG_ANDROID_APK, PKG_ANDROID_PLAY;
    // -1 until chosen (the window then picks Android for a project set up for it)
    int package_platform;
    // Platforms - Loading Screen (brand.h): the picture or video while the project / the game loads ("/Content/..."; "":
    // the engine's), and the engine's intro before it
    char splash[256];
    bool show_intro;
    // Online (online.h, notas-dev/online.md): Steam (its App ID: 480, Spacewar, while testing; the game ships
    // steam_api64.dll from the Steamworks SDK next to its .exe), saves in Steam Cloud, the UDP port, players, the tick
    // rate, the interpolation delay, the relevancy distance and the bandwidth cap per client; the network emulation
    // the editor's Play applies (latency, jitter, loss)
    bool online_steam, steam_cloud;
    uint32_t steam_app_id;
    int net_port, net_max_players, net_tick_rate, net_budget_kbps;
    float net_interp_ms, net_relevancy_m;
    float emu_latency_ms, emu_jitter_ms, emu_loss;
    // Engine - Audio (audio.h): the voices mixed at once (the rest virtual), binaural spatialization (the head's delay and
    // shadow: headphones), the master volume
    int audio_max_channels;
    bool audio_binaural;
    float audio_master_volume;
    // Platforms - Android (notas-dev/android.md): the package name (com.company.game), the orientation (0 landscape,
    // 1 portrait, 2 either), the version code (raised by each store upload), the release keystore ("": a debug key
    // made once), its alias and the file holding its password; the touch controls (WBP_TouchControls) added on touch
    // screens when the game starts
    char android_package[96], android_keystore[512], android_key_alias[64], android_key_pass_file[512];
    int android_orientation, android_version_code;
    bool android_touch_controls;
    // Augmented Reality (ARCore, src/ar_arcore.c): the APK carries ARCore and the game starts its session (the camera's
    // picture behind the level, the phone tracked, the real floor receiving the shadows); AR optional: any phone runs it
    bool android_ar;
    bool android_ar_occlusion; // the real things hide the virtual ones behind them (ARCore's Depth API, where the phone has it)
    // Running smoothly (src/mobile.c): the quality (0 automatic by the device, 1 Low, 2 Medium, 3 High), the frame rate
    // (30/60/90/120), the dynamic resolution, the window's scale (0: by the quality)
    int android_quality, android_target_fps;
    bool android_dynamic_res;
    float android_content_scale;
    // Google Play: the version code raised on each bundle (the store refuses one it has seen), the adaptive icon's
    // background ("#RRGGBB"), the privacy policy's address (the store asks it of online games; a reminder here)
    bool android_auto_version;
    char android_icon_bg[16], android_privacy_url[256];
    // Pause and options (game_settings.h, notas-dev/pausa-opcoes.md): the pause menu (its Widget Blueprint: "" is the
    // engine's WBP_PauseMenu, made in Content/UI), opened by Esc in the game and by the Play's key (Esc stops the Play);
    // the Main Menu Map its Main Menu button opens ("" : the Game Default Map)
    bool pause_menu;
    char pause_menu_widget[64], pie_pause_key[24], main_menu_map[256];
    // Save Game (save_progress.h, notas-dev/salvar-progresso.md): the progress's Save Game class ("": none), the timed auto save
    // (seconds, 0: off) into how many slots in turn, an auto save when a level opens, the picture of the screen in each save
    char save_game_class[64];
    float autosave_interval;
    int autosave_slots;
    bool autosave_on_level, save_thumbnail;
    // (Q25) VR (xr.h, vr_pawn.h, notas-dev/xr.md): Project Settings > Platforms > VR
    ProjectVr vr;
} ProjectSettings;

ProjectSettings* project(void);                  // the open project's (the defaults until one is loaded)
void project_defaults(ProjectSettings* p);
bool project_load(const char* rproject_path);
bool project_save(const char* rproject_path);    // NULL: where it was loaded from
bool project_save_copy(const char* path);         // a copy elsewhere (the packaged game's Game.rproject)
const char* project_file(void);                  // "" without one
// The project's folder (where Content is): the .rproject's folder, else the executable's
void project_root(char* out, size_t cap);

// The recent projects (%APPDATA%/RealEngine/projects.txt), newest first
void project_recent_add(const char* rproject_path);
int project_recent(char (*out)[600], int cap);
void project_recent_remove(const char* rproject_path);
void project_recent_disable(void); // the automated runs (--bench) leave the user's list alone

// The templates of a new project (the Project Browser's): what its first level and classes are
enum { PROJECT_TEMPLATE_BLANK = 0, PROJECT_TEMPLATE_THIRD_PERSON, PROJECT_TEMPLATE_FIRST_PERSON, PROJECT_TEMPLATE_TOP_DOWN,
       PROJECT_TEMPLATE_PHYSICS, PROJECT_TEMPLATE_RPG, PROJECT_TEMPLATE_HORROR, PROJECT_TEMPLATE_FIGHTING, PROJECT_TEMPLATE_AR,
       PROJECT_TEMPLATE_VR, PROJECT_TEMPLATE_COUNT }; // (Q25: Virtual Reality)

// Play starts (after bp_begin_play): the Game Mode's classes made (Player Controller, HUD, Game State, Player State,
// Game Instance) and, when no pawn was possessed, the Default Pawn at the Player Start
void project_begin_play(World* w);
void project_set_new_render_mode(int mode); // --render-mode: the next project_create's (a test: no Project Browser)
// The audio settings to the mixer (audio.h): the project opened, the settings changed, the game started
void project_apply_audio(void);
