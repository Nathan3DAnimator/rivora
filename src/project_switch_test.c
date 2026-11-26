// The test project_switch (--project-switch-test; 2026-11-25, bug-fixer, editor_docs.h): a Blueprint, a material, an
// image and the other asset editors open in project A, the Blueprint changed; a switch to B asked: the save prompt up
// (Cancel: A stays, the editors too); asked again, Don't Save: B open with no editor left open; the same assets opened
// in B and drawn; back to A (nothing unsaved: no prompt), none open, opened again; the editor closes with them open.
// Before: bp_shutdown freed the Blueprints under their open windows (a use after free), every editor stayed open on the
// old project's assets, and no prompt.
// Also (the same day): a Widget Blueprint in the UMG Designer (its Blueprint forgotten after the switch), the image's
// thumbnail (B's own, not A's at the same path) and a plugin of B's (Plugins/SwitchPlugin, the example plugin's DLL):
// loaded in B, gone back in A (its nodes, tick and menu item went on in every later project before)
#include "editor_docs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "plugins.h"
#include "content.h"
#include "editor.h"
#include "platform_files.h"
#include "project.h"

enum { S_WAIT_A, S_OPEN, S_DRAW, S_ASK, S_CANCEL, S_ASK2, S_WAIT_B, S_OPEN_B, S_DRAW_B, S_BACK, S_WAIT_A2, S_OPEN_A2, S_DRAW_A2, S_DONE };

static struct {
    bool running, ok;
    int state, frames, total;
    char a[600], b[600];
    char assets[16][512];
    int nassets, opened;
    bool designer_a; // the Designer showed the Widget Blueprint in A
    char b_root[600];
} T;

#define FAIL(...) (printf("[RealEngine] FAIL: project switch: " __VA_ARGS__), printf("\n"), T.ok = false)

void psw_test_start(void) {
    memset(&T, 0, sizeof T);
    T.running = T.ok = true;
}

static bool exists(const char* vpath) {
    char p[1100];
    snprintf(p, sizeof p, "%s%s", content_root(), vpath + 8);
    return re_file_stat(p, NULL, NULL, NULL);
}

// One asset per editor, those of every project (anim_init, the engine's materials and examples): opened by the Content
// Browser's double click
static void open_assets(void) {
    static const char* const fixed[] = { "/Content/Characters/Mannequin/ABP_Mannequin.rbp", "/Content/Materials/Examples/Textures/T_RoomsAtlas.png",
                                         "/Content/Characters/Mannequin/SK_Mannequin.rskm", "/Content/AI/BT_Chase.rbt",
                                         "/Content/Input/IA_Jump.rinputaction", "/Content/Audio/MetaSounds/MS_Wind.rmsnd" };
    T.nassets = 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (exists(fixed[i])) snprintf(T.assets[T.nassets++], 512, "%s", fixed[i]);
    static char found[64][512];
    if (exists("/Content/WBP_SwitchTest.rbp")) snprintf(T.assets[T.nassets++], 512, "%s", "/Content/WBP_SwitchTest.rbp");
    const char* finds[] = { ".rmat", ".rpcg" };
    for (int f = 0; f < 2; f++) {
        int n = content_search(finds[f], found, 64);
        for (int i = 0; i < n; i++)
            if (strstr(found[i], finds[f])) {
                snprintf(T.assets[T.nassets++], 512, "%s", found[i]);
                break;
            }
    }
    for (int i = 0; i < T.nassets; i++) content_open_asset(T.assets[i]);
}

static bool copy_file(const char* from, const char* to) {
    FILE* f = fopen(from, "rb");
    if (!f) return false;
    FILE* o = fopen(to, "wb");
    if (!o) return fclose(f), false;
    static char buf[1 << 16];
    bool ok = true;
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) ok &= fwrite(buf, 1, n, o) == n;
    fclose(f), fclose(o);
    return ok;
}

// B's own plugin: the example plugin's DLL under another name, on by default
static bool install_plugin(const char* bdir) {
    char exe[600], dir[800], from[900], to[900];
    if (!re_exe_dir(exe, sizeof exe)) return false;
    snprintf(dir, sizeof dir, "%s/SwitchB/Plugins/SwitchPlugin", bdir);
    re_make_dirs(dir);
    snprintf(from, sizeof from, "%s/Plugins/ExamplePlugin/ExamplePlugin.dll", exe);
    snprintf(to, sizeof to, "%s/ExamplePlugin.dll", dir);
    if (!copy_file(from, to)) return false;
    snprintf(to, sizeof to, "%s/plugin.json", dir);
    FILE* f = fopen(to, "wb");
    if (!f) return false;
    fprintf(f, "{ \"FileVersion\": 1, \"FriendlyName\": \"Switch Plugin\", \"Category\": \"Other\", \"EnabledByDefault\": true, \"Module\": \"ExamplePlugin\" }\n");
    fclose(f);
    return true;
}

static bool plugin_loaded(const char* name) {
    for (uint32_t i = 0; i < plugin_count(); i++)
        if (!strcmp(plugin_at(i)->name, name) && plugin_at(i)->loaded && !plugin_at(i)->unloaded) return true;
    return false;
}

static bool has_menu(const char* label) {
    for (uint32_t i = 0; i < plugin_menu_count(); i++)
        if (!strcmp(plugin_menu_label(i), label)) return true;
    return false;
}

#define PNG "/Content/Materials/Examples/Textures/T_RoomsAtlas.png"

static bool is_project(const char* f) { return !_stricmp(project_file(), f); }

bool psw_test_frame(Editor* ed, bool* ok) {
    if (!T.running) return false;
    T.frames++;
    if (++T.total > 4000) {
        FAIL("stuck at stage %d", T.state);
        T.state = S_DONE;
    }
    switch (T.state) {
    case S_WAIT_A: // (--new-project: A opens between the first frames)
        if (project_file()[0] && T.frames > 10) {
            snprintf(T.a, sizeof T.a, "%s", project_file());
            char dir[600];
            snprintf(dir, sizeof dir, "%s", T.a);
            char* s = strrchr(dir, '\\'), *s2 = strrchr(dir, '/');
            if (s2 > s) s = s2;
            if (s) *s = 0;
            s = strrchr(dir, '\\'), s2 = strrchr(dir, '/');
            if (s2 > s) s = s2;
            if (s) *s = 0; // (the folder that holds A's folder)
            char bdir[700];
            snprintf(bdir, sizeof bdir, "%s/SwitchB/Content", dir);
            re_make_dirs(bdir);
            snprintf(T.b, sizeof T.b, "%s/SwitchB/SwitchB.rproject", dir);
            snprintf(T.b_root, sizeof T.b_root, "%s/SwitchB/Content", dir);
            if (!copy_file(T.a, T.b)) FAIL("could not write %s", T.b), T.state = S_DONE;
            else if (!install_plugin(dir)) FAIL("could not put the plugin in B"), T.state = S_DONE;
            else T.state = S_OPEN, T.frames = 0;
        }
        break;
    case S_OPEN:
        if (!bp_create(BPA_CLASS, "/Content", "WBP_SwitchTest", "User Widget")) FAIL("could not make a Widget Blueprint");
        open_assets();
        asset_viewer_test_thumb(PNG, true); // (its thumbnail asked for, as the Content Browser does)
        T.state = S_DRAW, T.frames = 0;
        break;
    case S_DRAW:
        if (T.frames < 20) break;
        T.opened = editor_docs_open();
        T.designer_a = ui_designer_bound() != NULL;
        if (!T.designer_a) FAIL("the UMG Designer never showed the Widget Blueprint in A");
        if (plugin_fn_find("Count Calls") >= 0) FAIL("A has the plugin's nodes before B was opened");
        printf("[RealEngine] Project switch: %d assets, %d editors open in A\n", T.nassets, T.opened);
        if (T.opened < 4) FAIL("only %d editors opened of %d assets", T.opened, T.nassets);
        {
            BpAsset* a = bp_load("/Content/Characters/Mannequin/ABP_Mannequin.rbp");
            if (a) a->dirty = true; // (an unsaved change: the prompt lists it)
            else FAIL("no ABP_Mannequin to change");
        }
        T.state = S_ASK, T.frames = 0;
        break;
    case S_ASK: // the switch asked: the prompt first, nothing switched yet
        editor_open_project_later(ed, T.b, -1);
        if (!editor_docs_prompt_up() || ed->pending_project[0]) FAIL("the switch went ahead with unsaved changes and no prompt");
        T.state = S_CANCEL, T.frames = 0;
        break;
    case S_CANCEL:
        if (T.frames == 3) editor_docs_test_answer(3); // Cancel
        if (T.frames < 30) break;
        if (!is_project(T.a)) FAIL("Cancel switched the project anyway (%s)", project_file());
        else if (editor_docs_open() != T.opened) FAIL("Cancel closed the editors (%d of %d open)", editor_docs_open(), T.opened);
        else printf("[RealEngine] Project switch: Cancel kept A and its %d editors\n", T.opened);
        if (!is_project(T.a)) { T.state = S_WAIT_B, T.frames = 0; break; } // (switched already: on to B's checks)
        editor_open_project_later(ed, T.b, -1);
        T.state = S_ASK2, T.frames = 0;
        break;
    case S_ASK2:
        if (T.frames == 3) {
            char names[1024];
            int n = editor_docs_unsaved(names, sizeof names);
            if (n < 1 || !strstr(names, "ABP_Mannequin")) FAIL("the prompt doesn't list the changed Blueprint (%d: %s)", n, names);
            editor_docs_test_answer(2); // Don't Save
            T.state = S_WAIT_B, T.frames = 0;
        }
        break;
    case S_WAIT_B:
        if (!is_project(T.b) || T.frames < 20) break;
        {
            int n = editor_docs_open();
            if (n) FAIL("%d editor windows still open after switching to B (the old project's assets)", n);
            else printf("[RealEngine] Project switch: B open, 0 editors left open\n");
            if (ui_designer_bound()) FAIL("the UMG Designer still holds A's Widget Blueprint (freed) in B");
            const char* th = asset_viewer_test_thumb(PNG, false);
            if (th && _strnicmp(th, content_root(), strlen(content_root()))) FAIL("B's image shows A's thumbnail (%s)", th);
            else printf("[RealEngine] Project switch: the thumbnail is B's own (%s)\n", th ? "kept apart" : "none yet");
            if (!plugin_loaded("SwitchPlugin") || plugin_fn_find("Count Calls") < 0) FAIL("B's plugin didn't load");
            else printf("[RealEngine] Project switch: B's plugin loaded (its nodes in)\n");
        }
        T.state = S_OPEN_B, T.frames = 0;
        break;
    case S_OPEN_B:
        open_assets();
        T.state = S_DRAW_B, T.frames = 0;
        break;
    case S_DRAW_B:
        if (T.frames < 30) break;
        printf("[RealEngine] Project switch: %d editors open in B\n", editor_docs_open());
        if (editor_docs_open() < 4) FAIL("only %d editors reopened in B", editor_docs_open());
        T.state = S_BACK, T.frames = 0;
        break;
    case S_BACK: // (nothing unsaved: straight back, no prompt)
        editor_open_project_later(ed, T.a, -1);
        if (editor_docs_prompt_up()) FAIL("a prompt with nothing unsaved"), editor_docs_test_answer(2);
        T.state = S_WAIT_A2, T.frames = 0;
        break;
    case S_WAIT_A2:
        if (!is_project(T.a) || T.frames < 20) break;
        if (editor_docs_open()) FAIL("%d editor windows still open after switching back to A", editor_docs_open());
        else printf("[RealEngine] Project switch: back in A, 0 editors left open\n");
        if (plugin_loaded("SwitchPlugin") || plugin_fn_find("Count Calls") >= 0 || has_menu("Example: Say Hello"))
            FAIL("B's plugin still loaded in A (its nodes %s, its menu item %s)", plugin_fn_find("Count Calls") >= 0 ? "in" : "out", has_menu("Example: Say Hello") ? "in" : "out");
        else printf("[RealEngine] Project switch: B's plugin unloaded in A\n");
        T.state = S_OPEN_A2, T.frames = 0;
        break;
    case S_OPEN_A2:
        open_assets();
        T.state = S_DRAW_A2, T.frames = 0;
        break;
    case S_DRAW_A2:
        if (T.frames < 30) break;
        if (editor_docs_open() < 4) FAIL("only %d editors reopened in A", editor_docs_open());
        T.state = S_DONE; // (the editor closes with them open: editor_docs_finish_quit before ImGui's shutdown)
        break;
    default: break;
    }
    if (T.state != S_DONE) return false;
    T.running = false;
    *ok = T.ok;
    printf("[RealEngine] Project switch test: %s\n", T.ok ? "OK" : "FAIL");
    return true;
}
