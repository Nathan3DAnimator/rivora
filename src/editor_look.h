// The editor's look of 0.44 (notas-dev/temas-e-janelas.md, "Q26: o visual Azul Real"): the user's two mockups made
// real. The menu bar's brand (the logo, the name, the version) and its readouts (FPS / frame ms, the target platforms
// PC | Android | VR with their readiness), the status bar's right side (Ready, the pipelines still compiling, the
// derived data cache's size, the version), the Platform Support panel (what each target has and lacks, the devices
// connected), the window's own title bar in the theme's color, and the one-time offer of the new default look to
// the users of the old one. Nothing here is decoration: every readout is measured, every button does what it says.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct Editor;

// The target platforms (the menu bar's PC | Android | VR): the one Play and the Platforms menu act for
enum { LOOK_PC, LOOK_ANDROID, LOOK_VR, LOOK_TARGET_COUNT };
int look_target(void);
void look_set_target(int t);
const char* look_target_name(int t); // "PC", "Android", "VR"
// A target's readiness: what is missing (0: ready; checked every few seconds, a few file lookups)
int look_target_missing(int t);

// The main menu bar: its height (taller than a frame: the brand), the brand before the menus, the readouts after
float look_menu_bar_height(void);
void look_menu_bar_push(void); // (before igBeginMainMenuBar) its padding; look_menu_bar_pop after igEndMainMenuBar
void look_menu_bar_pop(void);
void look_menu_brand(struct Editor* ed);
void look_menu_readouts(struct Editor* ed);

// The status bar's right side: Ready (or what runs), Shader Compile: N, Derived Data: X GB, the version pill
void look_status_right(struct Editor* ed, double now);
// The status bar's left buttons after the Content Drawer and the Output Log: Statistics, Profiler, Device Manager
void look_status_left(struct Editor* ed);

// The Platform Support panel (Window > Platform Support; docked under Details by default, it may be closed)
bool* look_platforms_flag(void);
bool* look_modes_flag(void); // (the Modes panel: Selection, Landscape, Foliage and the mode's tools; beside Place Actors)
void look_platforms_panel(struct Editor* ed, double now);

// A primary button (the theme's accent: "+ Add" in the Content Browser and Details, a panel's main action)
bool look_primary_button(const char* id, int icon, const char* label, const char* tip);
// A pill: rounded text on a color (the version), clicked: true
bool look_pill(const char* id, const char* text, uint32_t bg_tok, uint32_t text_tok, const char* tip);

// Every frame (after the windows): the title bar's color following the theme, the offer of the new look, the caches
void look_frame(struct Editor* ed, double now);

// The preferences (editor.ini): look=<version of the default look the user has seen>, target=<platform>
bool look_read_pref(const char* key, const char* v);
void look_write_prefs(FILE* f);
// After editor.ini was read: an existing user still on the old default theme gets the new look (and can go back);
// one on another theme is offered it. had_file: there was an editor.ini; readonly: an automated run (nothing changes)
void look_after_prefs(struct Editor* ed, bool had_file, bool readonly);
// (editor_dock.c) the layout to start with: false when the new look replaces the saved one this run
bool look_keep_saved_layout(void);
void look_previous_layout(const char* text); // the layout the new look replaced (kept as a user layout to go back to)
// (the test look_migration: --look-test) the migration done in an automated run; its check at the end
extern bool g_look_test;
bool look_test_check(struct Editor* ed);
