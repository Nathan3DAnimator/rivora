// (2026-11-25, item 18) The editor's built-in plugins (notas-dev/editor.md "Plugins embutidos"): small tools for the
// artist, the level designer, the animator, gameplay, productivity, rendering, audio, AI, UI and online, each a plugin
// of Edit > Plugins (plugins_add_builtin: on and off at once, Config/Plugins.ini), its window under Tools > Plugins.
// They live in the editor (no DLL): what they do needs the editor's world, selection, camera and Content. Off: nothing
// of theirs runs, their memory is freed (kit_live_bytes counts it), what they changed in the world stays as an edit.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "editor.h"

struct ImDrawList;

typedef struct KitPlugin {
    const char* name;        // its id (Plugins.ini), e.g. "BatchRename"
    const char* friendly;    // its name in English (TR'd when shown)
    const char* category;    // plugins_ui's: Artist, Level Design, Animation, Gameplay, Productivity, Rendering, Audio, AI, UI, Online
    const char* icon;        // ui_icon_name ("BRUSH")
    const char* description; // English (TR'd)
    bool on_default;
    void (*window)(Editor* ed, double now);              // its window's contents (the kit opens and closes it)
    void (*overlay)(Editor* ed, struct ImDrawList* dl);  // over the viewport each frame while on (clipped to it); NULL: none
    void (*tick)(Editor* ed, double dt);                 // each frame while on; NULL: none
    void (*off)(Editor* ed);                             // turned off: its memory freed, its effects on the editor undone
    bool (*selftest)(Editor* ed, char* msg, size_t cap); // what it does, checked on a few actors it makes (--plugin-test)
} KitPlugin;

void kit_init(void);                            // registers them (plugins_add_builtin); before plugins_init reads the project
uint32_t kit_count(void);
const KitPlugin* kit_at(uint32_t i);
bool kit_on(uint32_t i);
void kit_open(uint32_t i, bool open);           // its window
bool kit_window_open(const char* name);         // a plugin's window shown now (by its id)
uint32_t kit_menu_count(void);                  // the ones on now (Tools > Plugins)
void kit_menu(Editor* ed);                      // Tools > Plugins: a submenu per category, an item per plugin on
void kit_frame(Editor* ed, double dt, double now);
float kit_time_scale(void);                     // the Debug Cheats' Slow Motion while playing (1: off)
// (the tests) every plugin turned on and off, its self-test run; the failures' count
int kit_selftest(Editor* ed);

// The plugins' memory (counted: off, a plugin must leave none)
void* kit_alloc(size_t bytes);
void* kit_realloc(void* p, size_t old_bytes, size_t bytes);
void kit_free(void* p, size_t bytes);
size_t kit_live_bytes(void);
void kit_account(long long bytes); // memory a worker made, adopted (+) or given back (-) on the main thread

// Helpers the plugins share (plugin_kit.c)
const char* kit_actor_name(World* w, Entity e);              // its label ("" when none)
bool kit_world_to_screen(Editor* ed, const float p[3], float out[2]); // the viewport's pixel (false: behind the camera)
bool kit_ground_below(Editor* ed, const float from[3], Entity ignore, float hit[3], float normal[3]); // the surface under a point
Entity kit_spawn_test_cube(Editor* ed, const char* name, float x, float y, float z); // (self-tests) a cube, named
void kit_destroy(Editor* ed, Entity e);                      // (self-tests) gone, out of the selection
bool kit_project_file(char* out, size_t cap, const char* rel); // <project>/rel (false: no project, then <exe>/rel)
// A panel of text over the viewport's top right corner (under the ones drawn before it this frame): its title in the
// accent token's color, then the lines of text
void kit_hud(struct ImDrawList* dl, Editor* ed, int accent_token, const char* title, const char* text);
// The project's assets (the Content folder walked, cached: read again with refresh, or when a second has passed and
// refresh is asked): vpath "/Content/..." and its hash (bp_vpath_hash: what the components keep), kind, size
typedef struct KitAsset { char vpath[240]; uint32_t hash; int kind; uint64_t size; } KitAsset;
uint32_t kit_assets(const KitAsset** out, bool refresh);
const char* kit_asset_by_hash(uint32_t hash); // its vpath ("" when unknown)
const char* kit_file_name(const char* vpath); // the name without folder and extension (a static buffer)
// A label at a world point (a dot and the text beside it); false: behind the camera
bool kit_label(struct ImDrawList* dl, Editor* ed, const float p[3], uint32_t col, const char* text);
