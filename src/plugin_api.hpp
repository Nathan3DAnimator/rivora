// Rivora Engine plugin API for C++ (2026-11-25, item 13): the C API of plugin_api.h in a small class, for a plugin written
// in C++ (Edit > Plugins > New Plugin... > C++). Nothing more than plugin_api.h can do: the same nodes, ticks, menu items;
// the DLL still exports re_plugin_init (RE_PLUGIN_ENTRY writes it). Header only, no engine library to link.
//
//   #include "plugin_api.hpp"
//   static void twice(const RePluginValue* in, RePluginValue* out, void*) { out[0].v[0] = in[0].v[0] * 2.0f; }
//   static int start(rivora::Api& api) {
//       api.node("Double It", "Math", "The value times 2.", "i:float:Value,o:float:Return Value", true, twice);
//       return 0;
//   }
//   RE_PLUGIN_ENTRY(start)
#pragma once

extern "C" {
#include "plugin_api.h"
}

namespace rivora {

// A pin's value: a number (x), or a vector / rotator / color (x, y, z, w)
struct Value {
    RePluginValue v;
    float x() const { return v.v[0]; }
    float y() const { return v.v[1]; }
    float z() const { return v.v[2]; }
    float w() const { return v.v[3]; }
};

class Api {
public:
    explicit Api(const RePluginApi* api) : a_(api) {}
    const RePluginApi* raw() const { return a_; }
    const char* plugin() const { return a_->plugin; }
    // the Output Log, one line prefixed by the plugin's name
    void log(const char* text) const { a_->log(a_->plugin, text); }
    // a Blueprint node (and the scripts' function): see plugin_api.h's add_function
    bool node(const char* name, const char* category, const char* tooltip, const char* pins, bool pure, RePluginFn fn,
              void* user = nullptr) const {
        return a_->add_function(a_->plugin, name, category, tooltip, pins, pure, fn, user);
    }
    bool tick(void (*fn)(float dt, bool playing, void* user), void* user = nullptr) const { return a_->add_tick(a_->plugin, fn, user); }
    bool menu(const char* label, void (*fn)(void* user), void* user = nullptr) const { return a_->add_menu_item(a_->plugin, label, fn, user); }
    void status(const char* text) const { a_->status(text); }
    double time() const { return a_->time_seconds(); }
    unsigned actors() const { return a_->actor_count(); }
    bool in_editor() const { return a_->in_editor(); }

private:
    const RePluginApi* a_;
};

} // namespace rivora

// The DLL's entry: start(rivora::Api&) returns 0 when the plugin loaded
#define RE_PLUGIN_ENTRY(start)                                                                                        \
    extern "C" RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {                                          \
        rivora::Api a(api);                                                                                            \
        return start(a);                                                                                               \
    }                                                                                                                  \
    extern "C" RE_PLUGIN_EXPORT void re_plugin_shutdown(void) {}
