// The API reference (a floating window: every engine function, searchable, by category, its signature, what it does,
// an example in Real, C#, Python and C) and the Editor Utilities (the classes whose parent is EditorUtility —
// scripts and Blueprints — their Custom Events as the editor's buttons, run on the level, undoable).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "blueprint.h"
#include "theme.h"
#include "bp_internal.h"
#include "editor.h"
#include "i18n.h"
#include "script.h"
#include "script_ide.h"
#include "ui_icons.h"

extern struct ImFont* g_code_font;

static struct {
    bool api_open, util_open;
    char find[64];
    char sel[64];
    int lang;
    char status[160];
} T;

void script_api_reference_open(const char* name) {
    T.api_open = true;
    if (name) snprintf(T.sel, sizeof T.sel, "%s", name), snprintf(T.find, sizeof T.find, "%s", name);
}

static void snake(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        bool up = isupper(c), dig = isdigit(c);
        if (i && o && out[o - 1] != '_' && ((up && (islower((unsigned char)in[i - 1]) || isdigit((unsigned char)in[i - 1]) ||
                                                    (in[i + 1] && islower((unsigned char)in[i + 1])))) ||
                                            (dig && isalpha((unsigned char)in[i - 1]))))
            out[o++] = '_';
        out[o++] = (char)tolower(c);
    }
    out[o] = 0;
}

// An example call in a language: its arguments' values by their types
static void example(const ScriptApi* a, int lang, char* out, size_t cap) {
    char name[80];
    if (lang == SL_PYTHON) snake(a->name, name, sizeof name);
    else snprintf(name, sizeof name, "%s", a->name);
    const char* p = strchr(a->sig, '(');
    char args[200] = "";
    size_t o = 0;
    bool first = true;
    for (const char* s = p ? p + 1 : ""; *s && *s != ')';) {
        while (*s == ' ' || *s == ',') s++;
        const char* e = s;
        while (*e && *e != ',' && *e != ')') e++;
        char ty[32] = "";
        sscanf(s, "%31s", ty);
        const char* v = !strcmp(ty, "string") ? "\"Name\"" : !strcmp(ty, "float") ? (lang == SL_CSHARP || lang == SL_C ? "1.0f" : "1.0")
                      : !strcmp(ty, "int") ? "1" : !strcmp(ty, "bool") ? (lang == SL_PYTHON ? "True" : "true")
                      : !strcmp(ty, "Vector") ? (lang == SL_CSHARP ? "new Vector(0, 0, 100)" : "Vector(0, 0, 100)")
                      : !strcmp(ty, "Rotator") ? "Rotator(0, 90, 0)" : !strcmp(ty, "Color") ? "Color(1, 0.5, 0, 1)"
                      : !strcmp(ty, "Actor") ? (lang == SL_PYTHON ? "self" : lang == SL_CSHARP ? "this" : "other") : "value";
        if (e > s && o < sizeof args - 40) o += (size_t)snprintf(args + o, sizeof args - o, "%s%s", first ? "" : ", ", v), first = false;
        s = e;
    }
    char ret[16] = "";
    sscanf(a->sig, "%15s", ret);
    bool has_ret = strcmp(ret, "void") != 0;
    switch (lang) {
    case SL_CSHARP: snprintf(out, cap, "%s%s(%s);", has_ret ? "var result = " : "", name, args); break;
    case SL_C: snprintf(out, cap, "%s%s(%s);", has_ret ? (strcmp(ret, "float") ? "int result = " : "float result = ") : "", name, args); break;
    case SL_PYTHON: snprintf(out, cap, "%s%s(%s)", has_ret ? "result = " : "", name, args); break;
    default: snprintf(out, cap, "%s%s(%s)", has_ret ? "var result = " : "", name, args); break;
    }
}

void script_api_reference_draw(void) {
    if (!T.api_open) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 58.0f, igGetFontSize() * 36.0f }, ImGuiCond_FirstUseEver);
    extern bool g_script_ide_demo;
    if (g_script_ide_demo) {
        const ImGuiViewport* vp = igGetMainViewport();
        igSetNextWindowPos((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.58f, vp->WorkPos.y + 60 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
        igSetNextWindowSize((ImVec2){ vp->WorkSize.x * 0.41f, vp->WorkSize.y * 0.62f }, ImGuiCond_Always);
    }
    char title[96];
    snprintf(title, sizeof title, "%s###api_reference", TR("API Reference"));
    if (!igBegin(title, &T.api_open, 0)) {
        igEnd();
        return;
    }
    // the search and the language of the examples
    igSetNextItemWidth(igGetFontSize() * 18.0f);
    igInputTextWithHint("##find", TR("Search the engine's functions"), T.find, sizeof T.find, 0, NULL, NULL);
    igSameLine(0, 12);
    for (int l = 0; l < SL_COUNT; l++) {
        if (l) igSameLine(0, 4);
        if (igRadioButton_Bool(g_script_lang_names[l], T.lang == l)) T.lang = l;
    }
    igTextDisabled("%u %s", script_api_count(), TR("functions: every Blueprint node is one (Target: self, or obj.Function())"));
    igSeparator();
    ImVec2 av = igGetContentRegionAvail();
    igBeginChild_Str("list", (ImVec2){ av.x * 0.38f, 0 }, ImGuiChildFlags_Borders, 0);
    const char* cat = NULL;
    const ScriptApi* sel = NULL;
    // by category (in the table's order), filtered by the search (name, category, description)
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < script_api_count(); i++) {
            const ScriptApi* a = script_api_at(i);
            if (!strcmp(a->name, T.sel)) sel = a;
            if (pass == 1) continue;
            if (T.find[0]) {
                char lf[64], ln[64], lc[64];
                size_t k;
                for (k = 0; T.find[k] && k < 63; k++) lf[k] = (char)tolower((unsigned char)T.find[k]);
                lf[k] = 0;
                for (k = 0; a->name[k] && k < 63; k++) ln[k] = (char)tolower((unsigned char)a->name[k]);
                ln[k] = 0;
                for (k = 0; a->category && a->category[k] && k < 63; k++) lc[k] = (char)tolower((unsigned char)a->category[k]);
                lc[k] = 0;
                if (!strstr(ln, lf) && !strstr(lc, lf)) continue;
            }
            if (!cat || !a->category || strcmp(cat, a->category)) {
                cat = a->category ? a->category : "";
                igSeparatorText(cat);
            }
            if (igSelectable_Bool(a->name, !strcmp(a->name, T.sel), 0, (ImVec2){ 0, 0 })) snprintf(T.sel, sizeof T.sel, "%s", a->name);
        }
    igEndChild();
    igSameLine(0, 8);
    igBeginChild_Str("page", (ImVec2){ 0, 0 }, 0, 0);
    if (sel) {
        igPushFont(g_code_font ? g_code_font : NULL, 0.0f);
        igTextWrapped("%s", sel->sig);
        igPopFont();
        igTextDisabled("%s  |  %s", sel->category ? sel->category : "", sel->pure ? TR("pure (a value, no side effect)") : TR("an action (runs in order)"));
        igSeparator();
        igTextWrapped("%s", sel->doc ? sel->doc : "");
        if (sel->text_arg) igTextDisabled("%s", TR("Its first argument is the name of an asset, a widget or an event, written in quotes."));
        igSeparatorText(TR("Example"));
        char ex[320];
        example(sel, T.lang, ex, sizeof ex);
        igPushFont(g_code_font ? g_code_font : NULL, 0.0f);
        igPushStyleColor_U32(ImGuiCol_Text, TH(TK_WARN));
        igTextWrapped("%s", ex);
        igPopStyleColor(1);
        igPopFont();
        if (igSmallButton(TR("Copy"))) igSetClipboardText(ex);
        igSeparatorText(TR("In every language"));
        for (int l = 0; l < SL_COUNT; l++) {
            example(sel, l, ex, sizeof ex);
            igTextDisabled("%-7s", g_script_lang_names[l]);
            igSameLine(igGetFontSize() * 5.0f, -1);
            igPushFont(g_code_font ? g_code_font : NULL, 0.0f);
            igTextUnformatted(ex, NULL);
            igPopFont();
        }
    } else igTextDisabled("%s", TR("Choose a function on the left (F1 in the Code Editor: this window; F12 on a name: its page)."));
    igEndChild();
    igEnd();
}

// --- Editor Utilities ---
void editor_utilities_open(void) { T.util_open = true; }

static Editor* g_ed;
static uint32_t sel_count(void) { return g_ed ? g_ed->nsel : 0; }
static Entity sel_at(uint32_t i) { return g_ed && i < g_ed->nsel ? g_ed->sel[i] : ENTITY_NONE; }
static void sel_set(Entity e, bool add) {
    if (g_ed) editor_select(g_ed, e, add ? SEL_ADD : SEL_REPLACE);
}

static bool is_utility(const BpAsset* a) {
    return a && a->kind == BPA_CLASS && (!strcmp(a->parent, "EditorUtility") || !strcmp(a->parent, "Editor Utility") ||
                                         !strcmp(a->parent, "EditorUtilityWidget") || !strcmp(a->parent, "Editor Utility Blueprint"));
}

void editor_utilities_bind(Editor* ed) { // (the selection's nodes read this editor's)
    g_ed = ed;
    BpEditorHooks h = { sel_count, sel_at, sel_set };
    bp_set_editor_hooks(&h);
}

void editor_utilities_draw(Editor* ed) {
    if (g_ed != ed) editor_utilities_bind(ed);
    if (!T.util_open) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 26.0f, igGetFontSize() * 24.0f }, ImGuiCond_FirstUseEver);
    char title[96];
    snprintf(title, sizeof title, "%s###editor_utilities", TR("Editor Utilities"));
    if (!igBegin(title, &T.util_open, 0)) {
        igEnd();
        return;
    }
    igTextWrapped("%s", TR("Classes whose parent is EditorUtility (a script or a Blueprint): each of their Custom Events is a tool, run on the level (Ctrl+Z undoes it)."));
    igSeparator();
    bool any = false;
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        BpAsset* a = bp_asset_at(i);
        if (!is_utility(a)) continue;
        any = true;
        igSeparatorText(a->name);
        if (!a->compiled && !bp_compile(a)) {
            igTextColored(THV(TK_ERROR), "%s", a->error);
            continue;
        }
        for (uint32_t k = 0; k < a->nentry; k++) {
            if (a->entries_code[k].kind != BPK_EVENT_CUSTOM) continue;
            igPushID_Ptr(&a->entries_code[k]);
            if (ui_icon_button("run", ICON_FORWARD, a->entries_code[k].name, TH(TK_N14), false, TR("Runs this tool on the level."))) {
                if (ed->playing) snprintf(T.status, sizeof T.status, "%s", TR("Stop playing first: the tools change the level."));
                else {
                    editor_undo_push(ed);
                    bool ok = bp_run_editor_event(&ed->world, a, a->entries_code[k].name);
                    ed->dirty = true;
                    snprintf(T.status, sizeof T.status, "%s %s.%s", ok ? TR("Ran") : TR("Could not run"), a->name, a->entries_code[k].name);
                }
            }
            igPopID();
        }
    }
    if (!any) igTextDisabled("%s", TR("None yet: Content Browser > Add > Script > Editor Utility."));
    if (T.status[0]) igSeparator(), igTextDisabled("%s", T.status);
    igEnd();
}
