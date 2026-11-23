// ExamplePlugin: the Real Engine's example plugin (plugin_api.h; notas-dev/editor.md "Plugins"). Built with the engine
// into Plugins/ExamplePlugin/ next to it, disabled by default (Edit > Plugins turns it on). What it shows:
//  - pure nodes (Smooth Step; Spring Toward with four inputs; Random Point in Box with vectors);
//  - a callable node with state (Count Calls: exec in and out, a running total);
//  - a tick (Example Ticks counts the frames it saw) and a menu item (Tools > Plugins > Example: Say Hello).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "plugin_api.h"

static const RePluginApi* g_api;
static int32_t g_total;
static uint32_t g_ticks;

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static void smooth_step(const RePluginValue* in, RePluginValue* out, void* user) {
    (void)user;
    float e0 = in[0].v[0], e1 = in[1].v[0], x = in[2].v[0];
    float t = e1 != e0 ? clampf((x - e0) / (e1 - e0), 0.0f, 1.0f) : 0.0f;
    out[0].v[0] = t * t * (3.0f - 2.0f * t);
}

// Current toward Target at Speed (1/s) over Delta Time, without overshooting (frame rate independent)
static void spring_toward(const RePluginValue* in, RePluginValue* out, void* user) {
    (void)user;
    float cur = in[0].v[0], target = in[1].v[0], speed = in[2].v[0], dt = in[3].v[0];
    out[0].v[0] = target + (cur - target) * expf(-fmaxf(speed, 0.0f) * fmaxf(dt, 0.0f));
}

static void random_in_box(const RePluginValue* in, RePluginValue* out, void* user) {
    (void)user;
    uint32_t s = (uint32_t)(int32_t)in[2].v[0] * 2654435761u + 12345u; // (Seed: the same seed, the same point)
    for (int k = 0; k < 3; k++) {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        float u = (float)(s & 0xFFFFFF) / 16777215.0f * 2.0f - 1.0f;
        out[0].v[k] = in[0].v[k] + u * in[1].v[k];
    }
}

static void count_calls(const RePluginValue* in, RePluginValue* out, void* user) {
    (void)user;
    g_total += (int32_t)in[0].v[0];
    out[0].v[0] = (float)g_total;
}

static void example_ticks(const RePluginValue* in, RePluginValue* out, void* user) {
    (void)in, (void)user;
    out[0].v[0] = (float)g_ticks;
}

static void crash_test(const RePluginValue* in, RePluginValue* out, void* user) { // (the engine's test of its crash guard)
    (void)in, (void)user;
    volatile int* p = NULL;
    out[0].v[0] = (float)*p;
}

static void on_tick(float dt, bool playing, void* user) {
    (void)dt, (void)playing, (void)user;
    g_ticks++;
}

static void say_hello(void* user) {
    (void)user;
    g_api->status("Hello from the Example Plugin!");
}

RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {
    if (api->version < 1) return 1;
    g_api = api;
    const char* me = api->plugin;
    api->add_function(me, "Smooth Step", "Math", "0 below Edge 0, 1 above Edge 1, an S curve between (Hermite).",
                      "i:float:Edge 0,i:float:Edge 1,i:float:X,o:float:Return Value", true, smooth_step, NULL);
    api->add_function(me, "Spring Toward", "Math", "Current moved toward Target at Speed per second over Delta Time: smooth, never overshooting, the same at any frame rate.",
                      "i:float:Current,i:float:Target,i:float:Speed,i:float:Delta Time,o:float:Return Value", true, spring_toward, NULL);
    api->add_function(me, "Random Point in Box", "Math", "A point inside the box (Center, Extent: half its size); the same Seed gives the same point.",
                      "i:vector:Center,i:vector:Extent,i:int:Seed,o:vector:Return Value", true, random_in_box, NULL);
    api->add_function(me, "Count Calls", "Utilities", "Adds Add to the plugin's running total and returns it (a callable node with state).",
                      "i:int:Add,o:int:Total", false, count_calls, NULL);
    api->add_function(me, "Example Ticks", "Utilities", "The frames the plugin's tick has seen.", "o:int:Return Value", true, example_ticks, NULL);
    if (getenv("REALENGINE_PLUGIN_CRASH_TEST"))
        api->add_function(me, "Crash Test", "Utilities", "(the engine's test) reads address 0.", "o:float:Return Value", true, crash_test, NULL);
    api->add_tick(me, on_tick, NULL);
    api->add_menu_item(me, "Example: Say Hello", say_hello, NULL);
    api->log(me, api->in_editor() ? "ready (editor)" : "ready (game)");
    return 0;
}

RE_PLUGIN_EXPORT void re_plugin_shutdown(void) { g_api = NULL; }
