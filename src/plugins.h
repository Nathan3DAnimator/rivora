// Plugins (B14c, notas-dev/editor.md "Plugins"), as Unreal's: native DLLs that add Blueprint nodes, a tick and menu
// items through plugin_api.h. Found in <project>/Plugins/<Name>/plugin.json and <engine>/Plugins/<Name>/plugin.json;
// enabled per project (Edit > Plugins; <project>/Config/Plugins.ini; a change counts at the next start, as Unreal's).
// A plugin's crash in one of its functions (an access violation) disables it instead of taking the editor down.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "plugin_api.h"

struct World;

#define PLUGINS_MAX 96
#define PLUGIN_FN_MAX 256

typedef struct PluginInfo {
    char name[48], friendly[64], version[24], author[64], category[48];
    char icon[48];          // (2026-11-25) plugin.json's Icon: one of the engine's icons by name ("brush", "camera"...); "": its category's
    char description[300];
    char dir[700];          // its folder
    char module[64];        // the DLL's name (no extension)
    bool project;           // the project's (else the engine's)
    bool enabled;           // to load (Config/Plugins.ini, else its EnabledByDefault)
    bool enabled_at_start;  // as it was when loaded (a change waits for a restart)
    bool loaded;            // its DLL loaded and its init returned 0
    bool faulted;           // one of its functions crashed: its nodes return zeros from then on
    bool unloaded;          // (2026-11-25) a project's plugin whose project was closed: nothing of it registered, its slot
                            // free for the next (its index kept: the built-ins keep theirs). Its DLL stays mapped
    bool builtin;           // part of the engine (no DLL): on and off at once, no restart (the Gameplay Framework, the editor's kit)
    bool builtin_default;   // a built-in's state when Plugins.ini doesn't say
    char error[200];        // why it isn't loaded
    uint32_t nfuncs, nticks, nmenus;
    double load_ms;
    void* dll;
    RePluginShutdownFn shutdown;
} PluginInfo;

typedef struct PluginFn {
    char name[48];          // the node's title (and, without spaces, the scripts' function)
    char category[80];      // "Plugins|<its category>"
    char tooltip[240];
    char pins[300];         // its pins (plugin_api.h's spec)
    bool pure;
    uint8_t nin, nout;
    uint8_t plugin;         // PluginInfo index
    RePluginFn fn;
    void* user;
} PluginFn;

// At startup (the editor: before the Blueprints and scripts compile; the game: before its level): every plugin found
// under the folders (project_dir may be NULL), the enabled ones loaded
void plugins_init(const char* project_dir, bool editor);
void plugins_shutdown(void);
void plugins_set_world(struct World* w);              // (the actors' count for the plugins)
void plugins_set_status(void (*fn)(const char* text)); // (the editor's status bar)
void plugins_tick(float dt, bool playing);            // each frame: their ticks

// (2026-11-10) the built-in Gameplay Framework (the game modules: Health, Inventory, Interaction, Pause Menu and
// Options, Save Progress, Dialogue, Quests, Game Rules, Abilities, HUD): Edit > Plugins turns it off and on for the
// project. Off: its nodes, components, actors and assets leave the menus, and Play makes nothing on its own (the
// player's Interactor, the HUD, the pause menu's key)
#define PLUGIN_GAMEPLAY "GameplayFramework"
bool plugins_gameplay_enabled(void);
// (2026-11-25, item 18) Another built-in plugin (the editor's kit, plugin_kit.h): listed with the others, on and off at
// once, its state in Config/Plugins.ini (else on_default). Its index (-1: full); called again with the same name: the same
int plugins_add_builtin(const char* name, const char* friendly, const char* category, const char* icon, const char* author,
                        const char* description, bool on_default);
bool plugins_builtin_on(int index); // a built-in plugin's state now (false: no such index)

uint32_t plugin_count(void);
const PluginInfo* plugin_at(uint32_t i);
void plugin_set_enabled(uint32_t i, bool enabled);    // saved in Config/Plugins.ini (counts at the next start)
uint32_t plugin_fn_count(void);
const PluginFn* plugin_fn_at(uint32_t i);
int plugin_fn_find(const char* name);                 // -1: none (a node's text)
// A node's call: in[nin] -> out[nout] (zeroed first). False: no such function, or its plugin faulted (outputs zero)
bool plugin_fn_call(int fn, const float (*in)[4], uint32_t nin, float (*out)[4], uint32_t nout);
// The editor's Tools > Plugins items (the label, run on a click)
uint32_t plugin_menu_count(void);
const char* plugin_menu_label(uint32_t i);
void plugin_menu_run(uint32_t i);

// Edit > Plugins (plugins_ui.c): the list as Unreal's (enable, details, the nodes each adds), New Plugin...
void plugins_window(bool* open);
// (--plugin-test) the example plugin loaded, its nodes in Blueprints and scripts; 0: passed
int plugins_selftest_report(void);
// A new plugin's folder from the template (Source/<Name>.c, plugin.json, build.bat); false with the reason in err
bool plugin_create(const char* project_dir, const char* name, char* err, size_t err_cap);
bool plugin_create_ex(const char* project_dir, const char* name, bool cpp, char* err, size_t err_cap); // (2026-11-25) cpp: in C++ (plugin_api.hpp)
// A node's pins (the engine's spec: exec first when callable, the inputs, Exec out, the outputs); "" when unknown
void plugin_fn_spec(int fn, bool exec, char* out, size_t cap);
uint32_t plugins_load_folder(const char* root, bool project); // the plugins under root not yet known, loaded (the tests)
