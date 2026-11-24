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

bool plugin_create(const char* project_dir, const char* name, char* err, size_t cap) {
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
             "  \"Category\": \"Other\",\n  \"CreatedBy\": \"\",\n  \"EnabledByDefault\": true,\n  \"Module\": \"%s\"\n}\n", name, name);
    if (!write_text(path, text)) return snprintf(err, cap, "%s %s", TR("Couldn't write"), path), false;
    snprintf(path, sizeof path, "%s/%s.c", src, name);
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
    }
    snprintf(path, sizeof path, "%s/build.bat", dir);
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
             "cl /nologo /O2 /LD /std:c11 /W3 Source\\%s.c /Fe:%s.dll /Fo:Source\\ /link /NOLOGO\r\n"
             "if errorlevel 1 exit /b 1\r\n"
             "del /q %s.exp %s.lib >nul 2>nul\r\n"
             "echo %s.dll built. Restart the editor to load it.\r\n",
             name, name, name, name, name, name);
    if (!write_text(path, text)) return snprintf(err, cap, "%s %s", TR("Couldn't write"), path), false;
    return true;
}

void plugins_window(bool* open) {
    if (!*open) return;
    igSetNextWindowSize((ImVec2){ 760, 560 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Plugins"), open, 0)) {
        igEnd();
        return;
    }
    static char search[64], newname[48], msg[300];
    static bool msg_err;
    igSetNextItemWidth(260);
    igInputTextWithHint("##psearch", TR("Search"), search, sizeof search, 0, NULL, NULL);
    igSameLine(0, -1);
    if (igButton(TR("New Plugin..."), (ImVec2){ 0, 0 })) igOpenPopup_Str("newplugin", 0);
    if (igBeginPopup("newplugin", 0)) {
        igTextUnformatted(TR("A plugin's folder in <project>/Plugins: plugin.json, Source/<Name>.c with one node, build.bat."), NULL);
        igSetNextItemWidth(220);
        igInputTextWithHint("##pname", TR("Name (letters, digits, _)"), newname, sizeof newname, 0, NULL, NULL);
        igSameLine(0, -1);
        if (igButton(TR("Create"), (ImVec2){ 0, 0 })) {
            char pr[800], err[300] = "";
            project_root(pr, sizeof pr);
            if (plugin_create(pr, newname, err, sizeof err))
                snprintf(msg, sizeof msg, TR("Created %s/Plugins/%s: run its build.bat, then restart the editor."), pr, newname), msg_err = false;
            else snprintf(msg, sizeof msg, "%s", err), msg_err = true;
            igCloseCurrentPopup();
        }
        igEndPopup();
    }
    bool restart = false;
    for (uint32_t i = 0; i < plugin_count(); i++) restart |= plugin_at(i)->enabled != plugin_at(i)->enabled_at_start;
    if (restart) igTextColored(THV(TK_WARN), "%s", TR("Restart the editor for the plugins' changes to take effect."));
    if (msg[0]) igTextColored(msg_err ? THV(TK_ERROR) : THV(TK_OK), "%s", msg);
    igSeparator();
    if (!plugin_count()) igTextWrapped("%s", TR("No plugins found. A plugin is a folder with plugin.json and its DLL in <project>/Plugins or next to the engine (Plugins). New Plugin... makes one."));
    if (igBeginChild_Str("plist", (ImVec2){ 0, 0 }, 0, 0)) {
        for (uint32_t i = 0; i < plugin_count(); i++) {
            const PluginInfo* p = plugin_at(i);
            if (!contains_ci(p->friendly, search) && !contains_ci(p->description, search) && !contains_ci(p->category, search)) continue;
            igPushID_Int((int)i);
            bool en = p->enabled;
            if (igCheckbox("##en", &en)) plugin_set_enabled(i, en);
            igSameLine(0, -1);
            igText("%s", p->friendly);
            igSameLine(0, -1);
            igTextDisabled("%s %s  |  %s  |  %s", TR("Version"), p->version, p->category, p->project ? TR("Project") : TR("Engine"));
            if (p->author[0]) igTextDisabled("%s %s", TR("By"), p->author);
            if (p->description[0]) igTextWrapped("%s", p->description);
            if (p->builtin) { // (part of the engine: its nodes are the engine's own, counted by their kind)
                uint32_t nodes = 0;
                for (uint32_t k = 0; k < bp_node_kind_count(); k++) nodes += bp_kind_gameplay(k);
                if (p->enabled) igTextColored(THV(TK_OK), TR("Built into the engine, on: %u nodes and 13 components (on and off at once)"), nodes);
                else igTextDisabled("%s", TR("Built into the engine, off: its nodes, components, actors and assets are hidden (on and off at once)"));
            } else if (p->loaded && !p->faulted)
                igTextColored(THV(TK_OK), TR("Loaded: %u nodes, %u ticks, %u menu items (%.1f ms)"), p->nfuncs, p->nticks, p->nmenus, p->load_ms);
            else if (p->error[0]) igTextColored(THV(TK_ERROR), "%s: %s", TR("Not working"), p->error);
            else igTextDisabled("%s", p->enabled_at_start ? TR("Not loaded") : TR("Disabled"));
            if (p->nfuncs && igTreeNode_Str(TR("Nodes"))) {
                for (uint32_t k = 0; k < plugin_fn_count(); k++) {
                    const PluginFn* f = plugin_fn_at(k);
                    if (f->plugin != i) continue;
                    igBulletText("%s  (%s)", f->name, f->pure ? TR("pure") : TR("callable"));
                    if (igIsItemHovered(0)) igSetTooltip("%s\n%s\n%s", f->category, f->tooltip, f->pins);
                }
                igTreePop();
            }
            igSeparator();
            igPopID();
        }
    }
    igEndChild();
    igEnd();
}
