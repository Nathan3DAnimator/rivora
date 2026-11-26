// The plugins' tests (B14c, notas-dev/editor.md "Plugins").
// --plugin-test (ctest "plugins", REALENGINE_PLUGINS_ENABLE=ExamplePlugin): the example plugin loaded; a Real script
// calls its nodes (pure, callable with state, vectors) in Play; its tick counted the frames; a crash in one of its
// functions disables it and the editor goes on.
// --plugin-new-test DIR (ctest "plugin_template"): New Plugin's template made in DIR, built by its build.bat with Visual
// Studio's compiler, loaded and called (its Remap node).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "content.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "plugin_kit.h"
#include "plugins.h"
#include "script.h"

static Entity g_actor;
static bool g_fail;
static size_t g_kit_live0;

static const char* const g_src =
    "class PluginUser extends Actor\n"
    "    var smooth = 0.0\n"
    "    var spring = 0.0\n"
    "    var total = 0\n"
    "    var box = Vector(0, 0, 0)\n"
    "\n"
    "    event BeginPlay()\n"
    "        smooth = SmoothStep(0.0, 10.0, 5.0)\n"
    "        spring = SpringToward(0.0, 10.0, 1000.0, 1.0)\n"
    "        CountCalls(3)\n"
    "        total = CountCalls(4)\n"
    "        box = RandomPointInBox(Vector(100, 200, 300), Vector(10, 10, 10), 7)\n";

bool plugins_test_setup(Editor* ed) {
    if (!plugin_count()) {
        printf("[RealEngine] FAIL: plugin-test: no plugins found next to the engine\n");
        return false;
    }
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/PluginTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/PluginUser.real", dir);
    FILE* f = re_fopen(path, "wb");
    if (!f) return printf("[RealEngine] FAIL: plugin-test: can't write %s\n", path), false;
    fputs(g_src, f);
    fclose(f);
    ScriptFile* sf = script_load("/Content/PluginTest/PluginUser.real");
    if (!sf || !sf->ok || !sf->cls) {
        printf("[RealEngine] FAIL: plugin-test: the script calling the plugin's nodes didn't compile\n");
        for (uint32_t i = 0; sf && i < sf->ndiags; i++) printf("[RealEngine]   (%d:%d) %s\n", sf->diags[i].line, sf->diags[i].col, sf->diags[i].text);
        return false;
    }
    printf("[RealEngine] Plugin script: %u nodes, %u ops\n", sf->nodes, sf->ops);
    // (2026-11-25, item 18) the editor's built-in plugins: each one on, its self-test, off with none of its memory left
    g_kit_live0 = kit_live_bytes();
    if (kit_selftest(ed)) g_fail = true;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < plugin_count(); i++) listed += plugin_at(i)->builtin;
    printf("[RealEngine] Plugin kit: %u built-in plugins listed in Edit > Plugins (%u found in all)\n", listed, plugin_count());
    if (listed < kit_count() + 1) printf("[RealEngine] FAIL: plugin kit: not every built-in plugin is listed\n"), g_fail = true;
    if (getenv("REALENGINE_KIT_WINDOWS")) // (the frames draw every plugin's window, its overlay and tick: on through Play)
        for (uint32_t i = 0; i < kit_count(); i++) {
            for (uint32_t k = 0; k < plugin_count(); k++)
                if (plugin_at(k)->builtin && !strcmp(plugin_at(k)->name, kit_at(i)->name)) plugin_set_enabled(k, true);
            kit_open(i, true);
        }
    float at[3] = { ed->cam.pos[0] + 400.0f, ed->cam.pos[1], 100.0f };
    g_actor = bp_spawn(&ed->world, sf->cls, at, (float[3]){ 0, 0, 0 });
    extern void editor_show_plugins(void);
    editor_show_plugins(); // (the window drawn through the measure)
    editor_play(ed, 0.0);
    return true;
}

bool plugins_test_check(Editor* ed) {
    World* w = &ed->world;
    BpValue smooth = { { 0 } }, spring = { { 0 } }, total = { { 0 } }, box = { { 0 } };
    bool got = bp_instance_var(w, g_actor, "smooth", &smooth);
    bp_instance_var(w, g_actor, "spring", &spring);
    bp_instance_var(w, g_actor, "total", &total);
    bp_instance_var(w, g_actor, "box", &box);
    bool in_box = fabsf(box.v[0] - 100.0f) <= 10.0f && fabsf(box.v[1] - 200.0f) <= 10.0f && fabsf(box.v[2] - 300.0f) <= 10.0f &&
                  (box.v[0] != 100.0f || box.v[1] != 200.0f);
    float ticks[1][4];
    plugin_fn_call(plugin_fn_find("Example Ticks"), NULL, 0, ticks, 1);
    bool good = got && smooth.v[0] == 0.5f && fabsf(spring.v[0] - 10.0f) < 1e-3f && total.v[0] == 7.0f && in_box && ticks[0][0] > 10.0f;
    printf("[RealEngine] Plugin nodes in Play: Smooth Step %.3f, Spring Toward %.3f, Count Calls %.0f, Random Point in Box (%.1f, %.1f, %.1f), ticks %.0f: %s\n",
           smooth.v[0], spring.v[0], total.v[0], box.v[0], box.v[1], box.v[2], ticks[0][0], good ? "ok" : "WRONG");
    if (!good) g_fail = true;
    if (plugins_selftest_report()) g_fail = true; // (last: its crash test disables the plugin)
    if (getenv("REALENGINE_KIT_WINDOWS")) { // every built-in plugin off again: what their windows made in Play freed
        for (uint32_t k = 0; k < plugin_count(); k++)
            for (uint32_t i = 0; i < kit_count(); i++)
                if (plugin_at(k)->builtin && !strcmp(plugin_at(k)->name, kit_at(i)->name)) plugin_set_enabled(k, false);
        kit_frame(ed, 0.0, 0.0);
        size_t left = kit_live_bytes() - g_kit_live0;
        printf("[RealEngine] Plugin kit: every window drawn in Play, all off: %zu bytes left\n", left);
        if (left) printf("[RealEngine] FAIL: plugin kit: memory left after turning them off\n"), g_fail = true;
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/PluginTest", content_root());
    re_remove_tree(dir); // (the build's Content without it: compiled at the next start, before the plugins load, it would complain)
    if (g_fail) printf("[RealEngine] FAIL: Plugins\n");
    else printf("[RealEngine] Plugins OK\n");
    return !g_fail;
}

int plugins_template_test(const char* dir) {
    char err[300] = "", root[900], cmd[1400], log[1000], dll[1000];
    re_make_dirs(dir);
    snprintf(root, sizeof root, "%s/Plugins/TestPlugin", dir);
    re_remove_tree(root);
    if (!plugin_create(dir, "TestPlugin", err, sizeof err)) {
        printf("[RealEngine] FAIL: plugin template: %s\n", err);
        return 1;
    }
    double t0 = re_time_seconds();
    char bat[1000];
    snprintf(bat, sizeof bat, "%s\\build.bat", root);
    for (char* c = bat; *c; c++) // (only the path's slashes: "cmd /c" keeps its own)
        if (*c == '/') *c = '\\';
    snprintf(cmd, sizeof cmd, "cmd /d /c \"\"%s\" < NUL\"", bat);
    snprintf(log, sizeof log, "%s/build.log", root);
    int rc = re_run_command(cmd, log);
    snprintf(dll, sizeof dll, "%s/TestPlugin.dll", root);
    FILE* f = re_fopen(dll, "rb");
    bool built = f != NULL;
    if (f) fclose(f);
    printf("[RealEngine] Plugin template: build.bat %s (exit %d) in %.1f s\n", built ? "made TestPlugin.dll" : "FAILED", rc, re_time_seconds() - t0);
    if (!built) {
        printf("[RealEngine] FAIL: plugin template didn't build (see %s)\n", log);
        return 1;
    }
    plugins_init(NULL, true);
    char plugs[1000];
    snprintf(plugs, sizeof plugs, "%s/Plugins", dir);
    plugins_load_folder(plugs, true);
    int fi = plugin_fn_find("TestPlugin Remap");
    float in[5][4] = { { 5 }, { 0 }, { 10 }, { 0 }, { 100 } }, out[1][4];
    bool ok = plugin_fn_call(fi, (const float(*)[4])in, 5, out, 1) && fabsf(out[0][0] - 50.0f) < 1e-4f;
    for (uint32_t i = 0; i < plugin_count(); i++)
        if (plugin_at(i)->error[0]) printf("[RealEngine] Plugin %s: %s\n", plugin_at(i)->name, plugin_at(i)->error);
    const PluginInfo* tp = NULL; // (2026-11-25, item 18) the template's plugin.json has its Category and Icon
    for (uint32_t i = 0; i < plugin_count(); i++)
        if (!strcmp(plugin_at(i)->name, "TestPlugin")) tp = plugin_at(i);
    bool meta = tp && !strcmp(tp->category, "Other") && !strcmp(tp->icon, "PLUGIN");
    printf("[RealEngine] Plugin template: Category \"%s\", Icon \"%s\": %s\n", tp ? tp->category : "", tp ? tp->icon : "", meta ? "ok" : "WRONG");
    ok = ok && meta;
    printf("[RealEngine] Plugin template: loaded, TestPlugin Remap(5, 0..10 -> 0..100) = %.2f: %s\n", out[0][0], ok ? "ok" : "WRONG");
    // (2026-11-25, item 13) the C++ template (plugin_api.hpp): made, built, loaded, called
    char rootc[900];
    snprintf(rootc, sizeof rootc, "%s/Plugins/TestPluginCpp", dir);
    re_remove_tree(rootc);
    bool cpp_ok = plugin_create_ex(dir, "TestPluginCpp", true, err, sizeof err);
    if (cpp_ok) {
        snprintf(bat, sizeof bat, "%s\\build.bat", rootc);
        for (char* c = bat; *c; c++)
            if (*c == '/') *c = '\\';
        snprintf(cmd, sizeof cmd, "cmd /d /c \"\"%s\" < NUL\"", bat);
        snprintf(log, sizeof log, "%s/build.log", rootc);
        double c0 = re_time_seconds();
        re_run_command(cmd, log);
        snprintf(plugs, sizeof plugs, "%s/Plugins", dir);
        plugins_load_folder(plugs, true);
        int fc = plugin_fn_find("TestPluginCpp Remap");
        float outc[1][4];
        cpp_ok = plugin_fn_call(fc, (const float(*)[4])in, 5, outc, 1) && fabsf(outc[0][0] - 50.0f) < 1e-4f;
        printf("[RealEngine] Plugin template C++: built in %.1f s, TestPluginCpp Remap(5) = %.2f: %s\n", re_time_seconds() - c0, outc[0][0], cpp_ok ? "ok" : "WRONG");
    } else printf("[RealEngine] Plugin template C++: %s\n", err);
    ok = ok && cpp_ok;
    if (!ok) printf("[RealEngine] FAIL: plugin template\n");
    return ok ? 0 : 1;
}
