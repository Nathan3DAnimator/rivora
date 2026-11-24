// Tools > AI Connection (MCP) (mcp.h, notas-dev/mcp.md): the server's switch and port, what it is doing, and each
// client's setup ready to copy (Claude Code's command, Claude Desktop's JSON with the stdio bridge, a URL client's).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "editor.h"
#include "i18n.h"
#include "mcp.h"
#include "theme.h"

static struct {
    bool show;
    bool enabled;
    int port;
    bool loaded;
} U = { false, false, MCP_DEFAULT_PORT, false };

bool* mcp_ui_show_flag(void) { return &U.show; }
bool mcp_pref_enabled(void) { return U.enabled; }

void mcp_write_prefs(FILE* f) { fprintf(f, "mcp_enabled=%d\nmcp_port=%d\n", U.enabled ? 1 : 0, U.port); }

bool mcp_read_pref(const char* key, const char* value) {
    if (!strcmp(key, "mcp_enabled")) return U.enabled = atoi(value) != 0, true;
    if (!strcmp(key, "mcp_port")) {
        int p = atoi(value);
        U.port = p >= 1024 && p <= 65535 ? p : MCP_DEFAULT_PORT;
        return true;
    }
    return false;
}

void mcp_apply_prefs(bool force_on, int port_override) {
    if (port_override >= 1024 && port_override <= 65535) U.port = port_override;
    if (force_on || U.enabled) mcp_server_start(U.port);
}

void mcp_pref_set(bool enabled, int port) {
    U.enabled = enabled;
    if (port >= 1024 && port <= 65535) U.port = port;
    if (enabled) mcp_server_start(U.port);
    else mcp_server_stop();
    editor_prefs_changed();
}

// A block of text to copy: shown wrapped, a Copy button beside its title
static void copy_block(const char* title, const char* id, const char* text) {
    igSeparatorText(title);
    igPushID_Str(id);
    igPushStyleColor_U32(ImGuiCol_ChildBg, TH(TK_N3));
    float h = igGetTextLineHeightWithSpacing() * (float)(1 + (strlen(text) / 70)) + 12.0f;
    igBeginChild_Str("code", (ImVec2){ -1.0f, h }, ImGuiChildFlags_Borders, 0);
    igPushTextWrapPos(0.0f);
    igTextUnformatted(text, NULL);
    igPopTextWrapPos();
    igEndChild();
    igPopStyleColor(1);
    if (igButton(TR("Copy"), (ImVec2){ 0, 0 })) igSetClipboardText(text);
    igPopID();
}

void mcp_ui_draw(struct Editor* ed) {
    (void)ed;
    if (!U.show) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 36.0f, igGetFontSize() * 34.0f }, ImGuiCond_FirstUseEver);
    char title[96];
    snprintf(title, sizeof title, "%s###mcp_window", TR("AI Connection (MCP)"));
    if (!igBegin(title, &U.show, 0)) {
        igEnd();
        return;
    }
    igTextWrapped("%s", TR("Lets an AI assistant (Claude, Cursor, any MCP client) see and change this editor: place and edit actors, run scripts, take pictures of the viewport, play and read the docs. It only accepts programs on this computer."));
    igSpacing();
    bool on = U.enabled;
    if (igCheckbox(TR("Enable MCP Server"), &on)) mcp_pref_set(on, U.port);
    int port = U.port;
    igSetNextItemWidth(igGetFontSize() * 8.0f);
    if (igInputInt(TR("Port"), &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && port >= 1024 && port <= 65535) mcp_pref_set(U.enabled, port);
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/mcp", U.port);
    if (mcp_server_running()) {
        igTextColored(THV(TK_GREEN), "%s %s", TR("Running at"), url);
        const char* cl = mcp_server_client();
        if (cl[0]) igTextDisabled(TR("Last client: %s, %u calls answered"), cl, mcp_server_calls());
        else igTextDisabled("%s", TR("No client connected yet."));
    } else if (mcp_server_error()[0]) igTextColored(THV(TK_ERROR), "%s", mcp_server_error());
    else igTextDisabled("%s", TR("Off."));

    // the clients' setups
    char text[1400];
    snprintf(text, sizeof text, "claude mcp add --transport http rivora %s", url);
    copy_block(TR("Claude Code (terminal)"), "cc", text);
    wchar_t wexe[MAX_PATH];
    char exe[MAX_PATH * 3] = "", esc[MAX_PATH * 6] = "";
    GetModuleFileNameW(NULL, wexe, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, wexe, -1, exe, (int)sizeof exe, NULL, NULL);
    char* sl = strrchr(exe, '\\');
    if (sl) snprintf(sl + 1, sizeof exe - (size_t)(sl + 1 - exe), "rivora_mcp.exe");
    for (size_t i = 0, o = 0; exe[i] && o + 2 < sizeof esc; i++) { // (JSON: each \ doubled)
        if (exe[i] == '\\') esc[o++] = '\\';
        esc[o++] = exe[i], esc[o] = 0;
    }
    if (U.port == MCP_DEFAULT_PORT)
        snprintf(text, sizeof text, "{\n  \"mcpServers\": {\n    \"rivora\": {\n      \"command\": \"%s\"\n    }\n  }\n}", esc);
    else
        snprintf(text, sizeof text, "{\n  \"mcpServers\": {\n    \"rivora\": {\n      \"command\": \"%s\",\n      \"args\": [\"--port\", \"%d\"]\n    }\n  }\n}", esc, U.port);
    copy_block(TR("Claude Desktop (claude_desktop_config.json)"), "cd", text);
    igTextDisabled("%s", TR("rivora_mcp.exe opens the editor when it isn't running and turns the server on."));
    snprintf(text, sizeof text, "{\n  \"mcpServers\": {\n    \"rivora\": {\n      \"url\": \"%s\"\n    }\n  }\n}", url);
    copy_block(TR("Cursor, VS Code and other clients (URL)"), "url", text);
    igSpacing();
    igTextWrapped("%s", TR("Tip: ask the AI to call get_started first. Every change it makes can be undone with Ctrl+Z; nothing is saved until it (or you) saves the level."));
    igEnd();
}
