// Pause and options (notas-dev/pausa-opcoes.md): the R7 module 3, made to be reused by any game.
//   * the game's pause, Unreal's Set Game Paused: the world, its physics, animation, particles, AI, movement and sounds
//     stop; the widgets (UMG), the UI sounds and the actors marked Tick When Paused go on
//   * Game User Settings, Unreal's UGameUserSettings: graphics (the scalability preset, resolution, window mode, VSync,
//     the frame rate limit, the resolution scale), audio (the volume of the Sound Classes Master, Music, SFX, Voice),
//     controls (the Enhanced Input keys remapped, the look sensitivity, Invert Y) and the language; kept in
//     GameUserSettings.ini (the packaged game: %LOCALAPPDATA%/<Game>/Saved/Config; the editor's Play: the project's
//     Saved/Config) and applied when the game opens
//   * the settings by name (the nodes Get / Set Game User Setting, a widget bound to a setting in its Details): see
//     gus_get / gus_set for the names
//   * the pause menu (pause_menu.c): WBP_PauseMenu and WBP_Options made in Content/UI when missing, opened by Esc in the
//     game (Project Settings > Pause Menu: the Play's key, P by default) and by a gamepad's Start
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

// Unreal's EWindowMode
enum { GUS_WINDOW_FULLSCREEN = 0, GUS_WINDOW_WINDOWED_FULLSCREEN, GUS_WINDOW_WINDOWED, GUS_WINDOW_COUNT };
// the Sound Classes the options set (the engine's SC_Master, SC_Music, SC_SFX, SC_Voice: audio.h AUDIO_CLASS_*)
enum { GUS_VOL_MASTER = 0, GUS_VOL_MUSIC, GUS_VOL_SFX, GUS_VOL_VOICE, GUS_VOL_COUNT };
#define GUS_MAX_REMAPS 64
#define GUS_NAME 48

// A key of an Input Mapping Context's mapping changed by the player: the action's mapping of key "from" reads "to"
typedef struct GusRemap { char action[GUS_NAME], from[GUS_NAME], to[GUS_NAME]; } GusRemap;

typedef struct GameUserSettings {
    int scalability;          // 0 Low, 1 Medium, 2 High, 3 Epic (4 Cinematic)
    int res_w, res_h;         // the window / the exclusive fullscreen mode (0: the monitor's)
    int window_mode;          // GUS_WINDOW_*
    bool vsync;
    int frame_limit;          // fps (0: none)
    int res_scale;            // the share of the preset's render resolution, % (50-100)
    float volume[GUS_VOL_COUNT]; // 0-1
    float sensitivity;        // the look's, times the project's (0.1-4)
    bool invert_y;
    int language;             // Language (i18n.h); -1: the system's
    char culture[16];         // (2026-11-24, loc.h) the project's localization culture ("": the native); the Language option picks it when the project has cultures
    bool auto_save;           // the game saves by itself (save_progress.h: the timed auto save, the level change; not the Checkpoints)
    int upscale_mode;         // upscale.h UPM_*: the render's share (Preset, Native, Quality, Balanced, Performance, Ultra Performance)
    bool dyn_res;             // the dynamic resolution: the share follows the GPU against the frame rate target (Project Settings)
    int upscaler;             // (4.4a) upscale.h UPS_*: TSR, FSR, DLSS, XeSS where installed; -1: the project's (Project Settings)
    uint32_t nremap;
    GusRemap remap[GUS_MAX_REMAPS];
} GameUserSettings;

GameUserSettings* gus(void);          // the settings as set (what Apply Settings applies)
const GameUserSettings* gus_applied(void); // as last applied
void gus_defaults(GameUserSettings* s); // the project's (Project Settings: the packaged game's rendering and window)
GameUserSettings* gus_base(void);       // what Set to Defaults goes back to (the host may change it: game.ini's)
void gus_reset(void);                   // the settings back to gus_base (not applied)
// The file: set by the host (the game: %LOCALAPPDATA%/<game>/Saved/Config; the editor: the project's Saved/Config; the
// tests: their own), REALENGINE_SETTINGS_FILE over every one
void gus_set_path(const char* path);
const char* gus_path(void);
void gus_default_path(const char* game_name, bool packaged, char* out, size_t cap);
bool gus_load(void);  // false: no file (the defaults stand)
bool gus_save(void);
uint32_t gus_serial(void); // changes with each change (the widgets bound to a setting read it again)

// The host applies what is its own: the render's preset and scale, the window, VSync, the frame limit (display: the
// window / VSync changed); the rest (volumes, keys, sensitivity, the language's texts) the module applies itself
typedef struct GusHost {
    void (*apply_video)(const GameUserSettings* s, bool display);
    bool global_language;     // the language is the whole program's (the game); the editor's Play: only its widgets'
} GusHost;
void gus_set_host(const GusHost* h);
void gus_warm(void); // (Q22) the slow first answers (the Upscaler's choices) asked now, while loading, not in a frame
void gus_apply(bool save);    // everything (Unreal's Apply Settings), then saved
void gus_apply_live(void);    // what applies at once (the volumes, the controls, the language, the preset)

// --- By name (Get / Set Game User Setting; a widget's "Game User Setting" in its Details). Names: Scalability,
// Resolution (text "1920x1080"), WindowMode, VSync, FrameRateLimit, ResolutionScale, MasterVolume, MusicVolume,
// SFXVolume, VoiceVolume, MouseSensitivity, InvertY, Language (text "pt_BR"), AutoSave, UpscalingMode, DynamicResolution,
// Key:<Action>:<Key> (the key now; set:
// the new key's name) ---
bool gus_get(const char* name, float* value, char* text, size_t cap);
bool gus_set(const char* name, float value, const char* text); // false: no such setting
// A setting's choices for a Combo Box ("Low|Medium|High|Epic"; the value is the index), "" when it has none
bool gus_options(const char* name, char* out, size_t cap);
int gus_option_index(const char* name);             // the choice set now (-1: not a choice)
bool gus_set_option(const char* name, int index);
bool gus_value_text(const char* name, char* out, size_t cap); // as shown ("75%", "High", "Space")
bool gus_slider_range(const char* name, float* vmin, float* vmax, float* step);
uint32_t gus_setting_count(void);                   // the Details' list
const char* gus_setting_name(uint32_t i);

// --- The controls: the keys remapped (input.c reads them for every mapping), the actions a player may remap ---
const char* gus_remapped(const char* action, const char* key); // the key to read in place of key (key itself: none)
bool gus_remap(const char* action, const char* from, const char* to); // to "" or == from: back to the default
// The key the action is on now: its first keyboard / mouse mapping's (gamepad: its first gamepad button's), remapped
const char* gus_action_key(const char* action, bool gamepad);
const char* gus_action_default_key(const char* action, bool gamepad); // before any remap
uint32_t gus_remap_hits(void); // (the tests: the mappings read through a remap)
// A GLFW key code through the action's remap (the engine's WASD movement reads IA_Move's keys: the player's)
int gus_remapped_code(const char* action, int glfw_key);

int gus_language(void); // the language the game's texts are in (i18n.h Language)
const char* gus_tr(const char* text); // a widget's text in the game's language (its translation, or itself)

// --- The game's pause (Set Game Paused) ---
bool game_paused(void);
void game_set_paused(bool paused);
// Quit Game / Return to Main Menu: the host reads them (the game ends; the editor's Play stops)
void game_request_quit(void);
bool game_quit_requested(void);
void game_quit_clear(void);

// --- The pause menu (pause_menu.c) ---
// WBP_PauseMenu and WBP_Options made in Content/UI when missing (the editor's Play and the packaging make them); the
// pause menu's hash (0: no Content folder)
uint32_t pause_menu_assets(void);
bool pause_menu_open(World* w);   // shown, the game paused, the cursor out
void pause_menu_close(World* w);  // gone (and its Options), the game on, the cursor as it was
bool pause_menu_is_open(void);
// Esc in the game, the Play's key, a gamepad's Start or B: a sub menu's OnBack first (the Options' back to the pause
// menu), else the menu opened / closed. back: B (closes only)
void pause_menu_key(World* w, bool back);
void pause_menu_end_play(void);
void pause_menu_poll_gamepad(World* w); // once a frame while playing: Start opens / closes, B goes back
