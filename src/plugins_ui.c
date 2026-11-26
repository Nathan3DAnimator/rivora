// Edit > Plugins (B14c), as Unreal's Plugins browser: a search, every plugin found (the project's and the engine's) with
// its check box, version, author, description, where it is and its state, the nodes it adds; New Plugin... makes a
// plugin's folder from the template (a C file with one node, plugin.json, build.bat for Visual Studio's compiler).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "theme.h"
#include "platform.h"
#include "platform_files.h"
#include "plugins.h"
#include "blueprint.h"
#include "project.h"
#include "ui_icons.h"
#include "ui_tips.h"

static bool contains_ci(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (; *hay; hay++) {
        size_t k = 0;
        while (needle[k] && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

bool plugin_create(const char* project_dir, const char* name, char* err, size_t cap) { return plugin_create_ex(project_dir, name, false, err, cap); }

// (2026-11-25, item 13) cpp: the source in C++ (Source/<Name>.cpp over plugin_api.hpp, built with /std:c++17)
bool plugin_create_ex(const char* project_dir, const char* name, bool cpp, char* err, size_t cap) {
    size_t n = strlen(name);
    bool valid = n >= 2 && n <= 40 && isalpha((unsigned char)name[0]);
    for (size_t i = 0; i < n && valid; i++) valid = isalnum((unsigned char)name[i]) || name[i] == '_';
    if (!valid) return snprintf(err, cap, "%s", TR("A plugin's name: letters, digits and _, starting with a letter (2 to 40)")), false;
    char dir[900], src[1000], path[1100];
    snprintf(dir, sizeof dir, "%s/Plugins/%s", project_dir, name);
    snprintf(path, sizeof path, "%s/plugin.json", dir);
    FILE* probe = re_fopen(path, "rb");
    if (probe) return fclose(probe), snprintf(err, cap, "%s", TR("A plugin of that name already exists")), false;
    snprintf(src, sizeof src, "%s/Source", dir);
    if (!re_make_dirs(src)) return snprintf(err, cap, "%s %s", TR("Couldn't make the folder"), src), false;
    char text[4096];
    snprintf(text, sizeof text,
             "{\n  \"FileVersion\": 1,\n  \"FriendlyName\": \"%s\",\n  \"VersionName\": \"1.0\",\n  \"Description\": \"\",\n"
             "  \"Category\": \"Other\",\n  \"Icon\": \"PLUGIN\",\n  \"CreatedBy\": \"\",\n  \"EnabledByDefault\": true,\n  \"Module\": \"%s\"\n}\n", name, name);
    if (!write_text(path, text)) return snprintf(err, cap, "%s %s", TR("Couldn't write"), path), false;
    snprintf(path, sizeof path, "%s/%s.%s", src, name, cpp ? "cpp" : "c");
    if (cpp)
        snprintf(text, sizeof text,
                 "// %s: a Rivora Engine plugin in C++ (plugin_api.hpp). build.bat makes %s.dll next to plugin.json; the editor loads\n"
                 "// it at its next start (Edit > Plugins). Each api.node is a node (Plugins category) and a script function.\n"
                 "#include \"plugin_api.hpp\"\n\n"
                 "// A pure node: Remap (Value from In Min..In Max to Out Min..Out Max)\n"
                 "static void remap(const RePluginValue* in, RePluginValue* out, void*) {\n"
                 "    const float v = in[0].v[0], a = in[1].v[0], b = in[2].v[0], c = in[3].v[0], d = in[4].v[0];\n"
                 "    out[0].v[0] = b != a ? c + (v - a) / (b - a) * (d - c) : c;\n"
                 "}\n\n"
                 "static int start(rivora::Api& api) {\n"
                 "    api.node(\"%s Remap\", \"Math\", \"Value from one range to another.\",\n"
                 "             \"i:float:Value,i:float:In Min,i:float:In Max,i:float:Out Min,i:float:Out Max,o:float:Return Value\", true, remap);\n"
                 "    api.log(\"loaded\");\n"
                 "    return 0;\n"
                 "}\n\n"
                 "RE_PLUGIN_ENTRY(start)\n",
                 name, name, name);
    else
    snprintf(text, sizeof text,
             "// %s: a Rivora Engine plugin (plugin_api.h). build.bat makes %s.dll next to plugin.json; the editor loads it at\n"
             "// its next start (Edit > Plugins). Each add_function is a Blueprint node (Plugins category) and a script function.\n"
             "#include \"plugin_api.h\"\n\n"
             "static const RePluginApi* g_api;\n\n"
             "// A pure node: Remap (Value from In Min..In Max to Out Min..Out Max)\n"
             "static void remap(const RePluginValue* in, RePluginValue* out, void* user) {\n"
             "    (void)user;\n"
             "    float v = in[0].v[0], a = in[1].v[0], b = in[2].v[0], c = in[3].v[0], d = in[4].v[0];\n"
             "    out[0].v[0] = b != a ? c + (v - a) / (b - a) * (d - c) : c;\n"
             "}\n\n"
             "RE_PLUGIN_EXPORT int re_plugin_init(const RePluginApi* api) {\n"
             "    g_api = api;\n"
             "    api->add_function(api->plugin, \"%s Remap\", \"Math\", \"Value from one range to another.\",\n"
             "                      \"i:float:Value,i:float:In Min,i:float:In Max,i:float:Out Min,i:float:Out Max,o:float:Return Value\", true, remap, 0);\n"
             "    api->log(api->plugin, \"loaded\");\n"
             "    return 0;\n"
             "}\n\n"
             "RE_PLUGIN_EXPORT void re_plugin_shutdown(void) {}\n",
             name, name, name);
    if (!write_text(path, text)) return snprintf(err, cap, "%s %s", TR("Couldn't write"), path), false;
    // the API's header next to the source (the plugin builds on its own)
    char exe[800], hdr[900], dst[1100];
    if (re_exe_dir(exe, sizeof exe)) {
        snprintf(hdr, sizeof hdr, "%s/SDK/plugin_api.h", exe);
        snprintf(dst, sizeof dst, "%s/plugin_api.h", src);
        if (!re_copy_file(hdr, dst)) return snprintf(err, cap, "%s %s", TR("Couldn't copy"), hdr), false;
        snprintf(hdr, sizeof hdr, "%s/SDK/plugin_api.hpp", exe);
        snprintf(dst, sizeof dst, "%s/plugin_api.hpp", src);
        if (cpp && !re_copy_file(hdr, dst)) return snprintf(err, cap, "%s %s", TR("Couldn't copy"), hdr), false;
    }
    snprintf(path, sizeof path, "%s/build.bat", dir);
    char cl[400];
    if (cpp) snprintf(cl, sizeof cl, "cl /nologo /O2 /LD /std:c++17 /EHsc /W3 Source\\%s.cpp /Fe:%s.dll /Fo:Source\\ /link /NOLOGO", name, name);
    else snprintf(cl, sizeof cl, "cl /nologo /O2 /LD /std:c11 /W3 Source\\%s.c /Fe:%s.dll /Fo:Source\\ /link /NOLOGO", name, name);
    snprintf(text, sizeof text,
             "@echo off\r\n"
             "rem Builds %s.dll with Visual Studio's C compiler (2019 or newer, the \"Desktop development with C++\" workload).\r\n"
             "setlocal\r\n"
             "cd /d \"%%~dp0\"\r\n"
             "where cl >nul 2>nul\r\n"
             "if not errorlevel 1 goto build\r\n"
             "pushd \"%%ProgramFiles(x86)%%\\Microsoft Visual Studio\\Installer\"\r\n"
             "for /f \"usebackq delims=\" %%%%i in (`.\\vswhere.exe -latest -products * -property installationPath`) do set \"VSDIR=%%%%i\"\r\n"
             "popd\r\n"
             "if not defined VSDIR (echo Visual Studio with C++ wasn't found. & exit /b 1)\r\n"
             "call \"%%VSDIR%%\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul 2>nul\r\n"
             ":build\r\n"
             "%s\r\n"
             "if errorlevel 1 exit /b 1\r\n"
             "del /q %s.exp %s.lib >nul 2>nul\r\n"
             "echo %s.dll built. Restart the editor to load it.\r\n",
             name, cl, name, name, name);
    if (!write_text(path, text)) return snprintf(err, cap, "%s %s", TR("Couldn't write"), path), false;
    return true;
}

// (2026-11-25, item 18) The categories as Unreal's Plugins browser shows them (the side bar): each its icon and color; a
// plugin.json's Category matched by name (case and spaces ignored), anything else under Other
typedef struct PlugCat { const char* name; UiIcon icon; int token; } PlugCat;
static const PlugCat g_cats[] = {
    { "Artist", ICON_BRUSH, TK_ORANGE },        { "Level Design", ICON_LANDSCAPE, TK_GREEN }, { "Animation", ICON_ANIMATION, TK_PINK },
    { "Gameplay", ICON_GAMEPAD, TK_CYAN },      { "Productivity", ICON_CHECK, TK_BLUE_L },   { "Rendering", ICON_VM_LIT, TK_AMBER },
    { "Audio", ICON_SOUND, TK_VIOLET },         { "AI", ICON_BRAIN, TK_TEAL },               { "UI", ICON_WIDGET, TK_BLUE },
    { "Online", ICON_NETWORK, TK_LIME },        { "Other", ICON_PLUGIN, TK_ACCENT },
};
#define NCATS ((int)(sizeof g_cats / sizeof g_cats[0]))

static bool same_word(const char* a, const char* b) {
    for (;; a++, b++) {
        while (*a == ' ' || *a == '&' || *a == '/') a++;
        while (*b == ' ' || *b == '&' || *b == '/') b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        if (!*a) return true;
    }
}
static int cat_of(const PluginInfo* p) {
    for (int c = 0; c < NCATS - 1; c++)
        if (same_word(p->category, g_cats[c].name) || (c == 5 && (contains_ci(p->category, "render") || contains_ci(p->category, "debug"))) ||
            (c == 0 && contains_ci(p->category, "art")))
            return c;
    return NCATS - 1;
}
// plugin.json's Icon: one of the engine's icons by its name ("BRUSH", "camera"); none or unknown: its category's
UiIcon plugin_icon(const PluginInfo* p) {
    if (p->icon[0])
        for (int k = 0; k < ICON_COUNT; k++) {
            const char* n = ui_icon_name((UiIcon)k);
            size_t j = 0;
            while (n[j] && p->icon[j] && tolower((unsigned char)n[j]) == tolower((unsigned char)p->icon[j])) j++;
            if (!n[j] && !p->icon[j]) return (UiIcon)k;
        }
    return g_cats[cat_of(p)].icon;
}
static uint32_t plugin_tint(const PluginInfo* p) { return TH(g_cats[cat_of(p)].token); }

enum { PV_ALL = -3, PV_BUILTIN = -2, PV_PROJECT = -1 }; // the side bar's first rows (then a category's index)

static bool plugin_shown(const PluginInfo* p, int view, const char* search) {
    if (p->unloaded) return false; // (2026-11-25) a closed project's
    if (view == PV_BUILTIN && !p->builtin) return false;
    if (view == PV_PROJECT && !p->project) return false;
    if (view >= 0 && cat_of(p) != view) return false;
    return contains_ci(p->friendly, search) || contains_ci(p->description, search) || contains_ci(p->category, search) || contains_ci(p->name, search);
}

static bool side_row(const char* id, UiIcon icon, uint32_t col, const char* label, uint32_t count, bool sel) {
    ImVec2 p0 = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x, h = igGetFrameHeight() + 4.0f;
    bool hit = igSelectable_Bool(id, sel, 0, (ImVec2){ w, h });
    ui_tip_item(icon, label, TR("Shows the plugins of this group."), NULL);
    ImDrawList* dl = igGetWindowDrawList();
    float isz = h - 8.0f;
    ui_icon(dl, icon, p0.x + 4.0f, p0.y + 4.0f, isz, col);
    char n[16];
    snprintf(n, sizeof n, "%u", count);
    ImVec2 ts = igCalcTextSize(n, NULL, false, -1);
    float ty = p0.y + (h - igGetTextLineHeight()) * 0.5f;
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p0.x + isz + 12.0f, ty }, TH(sel ? TK_TEXT_STRONG : TK_TEXT), label, NULL);
    ImDrawList_AddRectFilled(dl, (ImVec2){ p0.x + w - ts.x - 14.0f, ty - 1.0f }, (ImVec2){ p0.x + w - 4.0f, ty + ts.y + 1.0f }, TH(TK_BG_CONTROL), 8.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p0.x + w - ts.x - 9.0f, ty }, TH(TK_TEXT_DIM), n, NULL);
    return hit;
}

static void plugin_card(uint32_t i, float w) {
    const PluginInfo* p = plugin_at(i);
    igPushID_Int((int)i);
    float pad = 10.0f, isz = 44.0f, line = igGetTextLineHeightWithSpacing();
    float h = pad * 2.0f + line * 5.6f;
    ImVec2 p0 = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    bool hover = igIsMouseHoveringRect(p0, (ImVec2){ p0.x + w, p0.y + h }, true);
    uint32_t tint = plugin_tint(p);
    ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p0.x + w, p0.y + h }, TH(hover ? TK_BG_HOVER : TK_BG_RAISED), 6.0f, 0);
    ImDrawList_AddRect(dl, p0, (ImVec2){ p0.x + w, p0.y + h }, p->enabled ? THA(g_cats[cat_of(p)].token, 170) : TH(TK_BORDER), 6.0f, 1.0f, 0);
    // the icon in its family's color, on a rounded tile
    ImVec2 ip = { p0.x + pad, p0.y + pad };
    ImDrawList_AddRectFilled(dl, ip, (ImVec2){ ip.x + isz, ip.y + isz }, THA(g_cats[cat_of(p)].token, 46), 8.0f, 0);
    ui_icon(dl, plugin_icon(p), ip.x + 6.0f, ip.y + 6.0f, isz - 12.0f, p->enabled ? tint : TH(TK_TEXT_DIM));
    float tx = ip.x + isz + pad;
    igSetCursorScreenPos((ImVec2){ tx, p0.y + pad - 2.0f });
    igTextColored(THV(TK_TEXT_STRONG), "%s", TR(p->friendly));
    igSetCursorScreenPos((ImVec2){ tx, p0.y + pad + line - 2.0f });
    igTextColored(THV(TK_TEXT_DIM), "%s %s%s%s", TR("Version"), p->version, p->author[0] ? "  |  " : "", p->author);
    // Enabled at the top right
    char en_label[64];
    snprintf(en_label, sizeof en_label, "%s##en", TR("Enabled"));
    ImVec2 ls = igCalcTextSize(TR("Enabled"), NULL, false, -1);
    igSetCursorScreenPos((ImVec2){ p0.x + w - ls.x - igGetFrameHeight() - pad - 6.0f, p0.y + pad - 2.0f });
    bool en = p->enabled;
    if (igCheckbox(en_label, &en)) plugin_set_enabled(i, en);
    // the description, wrapped in the card
    igSetCursorScreenPos((ImVec2){ tx, p0.y + pad + line * 2.0f });
    float desc_bottom = p0.y + h - pad - line - 2.0f; // (whole lines only: the state's row stays clear)
    igPushClipRect((ImVec2){ tx, p0.y + pad + line * 2.0f }, (ImVec2){ p0.x + w - pad, desc_bottom }, true);
    igPushTextWrapPos(igGetCursorPosX() + (p0.x + w - tx - pad));
    igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_TEXT));
    if (p->description[0]) igTextWrapped("%s", TR(p->description));
    igPopStyleColor(1);
    igPopTextWrapPos();
    igPopClipRect();
    // the state at the bottom: where it is from, what it adds, restart / error
    igSetCursorScreenPos((ImVec2){ p0.x + pad, p0.y + h - pad - igGetTextLineHeight() });
    const char* where = p->builtin ? TR("Built-in") : p->project ? TR("Project") : TR("Engine");
    igTextColored(THV(TK_TEXT_DIM), "%s  |  %s", where, TR(g_cats[cat_of(p)].name));
    igSameLine(0, 14.0f);
    if (p->faulted || (p->error[0] && !p->loaded)) {
        igTextColored(THV(TK_ERROR), "%s: %s", TR("Not working"), p->error);
        if (igIsItemHovered(0)) igSetTooltip("%s", p->error);
    } else if (!p->builtin && p->enabled != p->enabled_at_start)
        igTextColored(THV(TK_WARN), "%s", TR("Restart needed"));
    else if (p->builtin && !strcmp(p->name, PLUGIN_GAMEPLAY)) {
        uint32_t nodes = 0;
        for (uint32_t k = 0; k < bp_node_kind_count(); k++) nodes += bp_kind_gameplay(k);
        if (p->enabled) igTextColored(THV(TK_OK), TR("On: %u nodes and 13 components"), nodes);
        else igTextDisabled("%s", TR("Off: its nodes, components, actors and assets are hidden"));
    } else if (p->builtin)
        igTextColored(p->enabled ? THV(TK_OK) : THV(TK_TEXT_DISABLED), "%s", p->enabled ? TR("On (Tools > Plugins)") : TR("Off"));
    else if (p->loaded)
        igTextColored(THV(TK_OK), TR("Loaded: %u nodes, %u ticks, %u menu items (%.1f ms)"), p->nfuncs, p->nticks, p->nmenus, p->load_ms);
    else
        igTextDisabled("%s", p->enabled_at_start ? TR("Not loaded") : TR("Disabled"));
    if (p->nfuncs && igIsItemHovered(0)) { // (the nodes it adds)
        igBeginTooltip();
        for (uint32_t k = 0; k < plugin_fn_count(); k++) {
            const PluginFn* f = plugin_fn_at(k);
            if (f->plugin == i) igBulletText("%s  (%s)  %s", f->name, f->pure ? TR("pure") : TR("callable"), f->pins);
        }
        igEndTooltip();
    }
    igSetCursorScreenPos(p0);
    igDummy((ImVec2){ w, h });
    igPopID();
}

void plugins_window(bool* open) {
    if (!*open) return;
    igSetNextWindowSize((ImVec2){ 1080, 680 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Plugins"), open, 0)) {
        igEnd();
        return;
    }
    static char search[64], newname[48], msg[300];
    static bool msg_err;
    static int view = PV_ALL;
    // the top bar: the search, New Plugin, Open Folder; the restart's warning
    igSetNextItemWidth(320);
    igInputTextWithHint("##psearch", TR("Search plugins"), search, sizeof search, 0, NULL, NULL);
    igSameLine(0, -1);
    if (ui_icon_button("newp", ICON_PLUS, TR("New Plugin..."), TH(TK_ACCENT), false, NULL)) igOpenPopup_Str("newplugin", 0);
    igSameLine(0, -1);
    if (ui_icon_button("openp", ICON_OPEN, TR("Open Folder"), TH(TK_TEXT), false, NULL)) {
        char pr[800], dir[900];
        project_root(pr, sizeof pr);
        snprintf(dir, sizeof dir, "%s/Plugins", pr);
        re_make_dirs(dir);
        re_show_in_explorer(dir);
    }
    if (igBeginPopup("newplugin", 0)) {
        igTextUnformatted(TR("A plugin's folder in <project>/Plugins: plugin.json, Source/<Name>.c with one node, build.bat."), NULL);
        igSetNextItemWidth(220);
        igInputTextWithHint("##pname", TR("Name (letters, digits, _)"), newname, sizeof newname, 0, NULL, NULL);
        static bool cpp;
        igSameLine(0, -1);
        igCheckbox(TR("C++"), &cpp);
        igSameLine(0, -1);
        if (igButton(TR("Create"), (ImVec2){ 0, 0 })) {
            char pr[800], err[300] = "";
            project_root(pr, sizeof pr);
            if (plugin_create_ex(pr, newname, cpp, err, sizeof err))
                snprintf(msg, sizeof msg, TR("Created %s/Plugins/%s: run its build.bat, then restart the editor."), pr, newname), msg_err = false;
            else snprintf(msg, sizeof msg, "%s", err), msg_err = true;
            igCloseCurrentPopup();
        }
        igEndPopup();
    }
    bool restart = false;
    for (uint32_t i = 0; i < plugin_count(); i++) restart |= !plugin_at(i)->builtin && plugin_at(i)->enabled != plugin_at(i)->enabled_at_start;
    if (restart) {
        igSameLine(0, 16.0f);
        igTextColored(THV(TK_WARN), "%s", TR("Restart the editor for the plugins' changes to take effect."));
    }
    if (msg[0]) igTextColored(msg_err ? THV(TK_ERROR) : THV(TK_OK), "%s", msg);
    // the side bar: All, Built-in, Project, then every category with plugins, each with its count
    uint32_t n_all = 0, n_builtin = 0, n_project = 0, n_cat[NCATS] = { 0 };
    for (uint32_t i = 0; i < plugin_count(); i++) {
        const PluginInfo* p = plugin_at(i);
        if (p->unloaded) continue;
        n_all++, n_builtin += p->builtin, n_project += p->project, n_cat[cat_of(p)]++;
    }
    if (igBeginChild_Str("pcats", (ImVec2){ 210, 0 }, ImGuiChildFlags_Borders, 0)) {
        if (side_row("##all", ICON_PLUGIN, TH(TK_ACCENT), TR("All Plugins"), n_all, view == PV_ALL)) view = PV_ALL;
        if (side_row("##bi", ICON_PROJECT, TH(TK_ACCENT2), TR("Built-in"), n_builtin, view == PV_BUILTIN)) view = PV_BUILTIN;
        if (side_row("##pr", ICON_FOLDER, TH(TK_TEXT), TR("Project"), n_project, view == PV_PROJECT)) view = PV_PROJECT;
        igSeparatorText(TR("Categories"));
        for (int c = 0; c < NCATS; c++) {
            if (!n_cat[c]) continue;
            igPushID_Int(c);
            if (side_row("##cat", g_cats[c].icon, TH(g_cats[c].token), TR(g_cats[c].name), n_cat[c], view == c)) view = c;
            igPopID();
        }
    }
    igEndChild();
    igSameLine(0, -1);
    // the cards, two or three in a row by the width
    if (igBeginChild_Str("plist", (ImVec2){ 0, 0 }, 0, 0)) {
        if (!plugin_count()) igTextWrapped("%s", TR("No plugins found. A plugin is a folder with plugin.json and its DLL in <project>/Plugins or next to the engine (Plugins). New Plugin... makes one."));
        float avail = igGetContentRegionAvail().x, gap = 10.0f;
        int cols = avail > 1100.0f ? 3 : avail > 640.0f ? 2 : 1;
        float cw = (avail - gap * (float)(cols - 1)) / (float)cols;
        int col = 0;
        uint32_t shown = 0;
        for (uint32_t i = 0; i < plugin_count(); i++) {
            if (!plugin_shown(plugin_at(i), view, search)) continue;
            if (col) igSameLine(0, gap);
            plugin_card(i, cw);
            col = (col + 1) % cols;
            shown++;
            if (!col) igDummy((ImVec2){ 1, gap * 0.4f });
        }
        if (plugin_count() && !shown) igTextDisabled("%s", TR("No plugin matches the search."));
    }
    igEndChild();
    igEnd();
}
