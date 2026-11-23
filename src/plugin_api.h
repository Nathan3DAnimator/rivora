// Real Engine plugin API (B14c, notas-dev/editor.md "Plugins"). The one header a plugin includes: plain C, no other
// engine header. A plugin is a folder with plugin.json and a DLL (Plugins/<Name>/ in the project, or next to the engine);
// the engine loads the enabled ones at startup and calls re_plugin_init with this table. What it can add:
//  - Blueprint nodes (and functions the scripts in Real, C#, Python and C call by name): pure (a value, no exec pins) or
//    callable (exec in and out), with up to 8 inputs and 4 outputs of float, int, bool, vector, rotator or color;
//  - a per frame tick (the editor and the game), menu items (the editor's Tools > Plugins), log lines and a status text.
// The table only grows: a plugin built against an older version keeps loading (it checks api->version for the newer).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RE_PLUGIN_API_VERSION 1

#if defined(_WIN32)
#define RE_PLUGIN_EXPORT __declspec(dllexport)
#else
#define RE_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// A pin's value: float, int and bool in v[0] (bool: 0 or 1), vector / rotator (pitch, yaw, roll) / color in v[0..3]
typedef struct RePluginValue { float v[4]; } RePluginValue;

// A node's function: in[] the inputs in the order of the pins, out[] the outputs (zeroed before the call). Called on the
// game thread, in the middle of a Blueprint: keep it short (no waits, no allocation per call).
typedef void (*RePluginFn)(const RePluginValue* in, RePluginValue* out, void* user);

typedef struct RePluginApi {
    uint32_t version;   // RE_PLUGIN_API_VERSION of the engine
    uint32_t size;      // sizeof(RePluginApi) of the engine
    const char* plugin; // this plugin's name (from its plugin.json)

    // The Output Log (and the console): one line, prefixed by the plugin's name
    void (*log)(const char* plugin, const char* text);
    // A Blueprint node. name: its title and the scripts' function ("Smooth Step" is SmoothStep in a script; up to 47
    // characters, unique among the plugins). category: the palette's ("Math|Interpolation"; under "Plugins|"). pins:
    // the inputs and outputs as the engine writes them, "i:float:A,i:vector:B,o:float:Return Value" (types float, int,
    // bool, vector, rotator, color). pure: no exec pins (a value); else Exec in and out. False: a bad spec or a name taken
    bool (*add_function)(const char* plugin, const char* name, const char* category, const char* tooltip, const char* pins,
                         bool pure, RePluginFn fn, void* user);
    // Called every frame (dt seconds; playing: in Play or the game, false: the editor's level)
    bool (*add_tick)(const char* plugin, void (*fn)(float dt, bool playing, void* user), void* user);
    // An item in the editor's Tools > Plugins menu (nothing in the game)
    bool (*add_menu_item)(const char* plugin, const char* label, void (*fn)(void* user), void* user);
    // The editor's status bar (the game: the log)
    void (*status)(const char* text);
    // The engine's clock (seconds) and the actors in the world now
    double (*time_seconds)(void);
    uint32_t (*actor_count)(void);
    bool (*in_editor)(void); // the editor (true) or a packaged game (false)
} RePluginApi;

// What the DLL exports:
//   RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api);   // 0: loaded; else the plugin is reported as failed
//   RE_PLUGIN_EXPORT void re_plugin_shutdown(void);                 // (optional) at exit
typedef int (*RePluginInitFn)(const RePluginApi* api);
typedef void (*RePluginShutdownFn)(void);
