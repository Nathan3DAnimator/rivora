// The UI sweep (ui_sweep.h; 2026-11-25, item 21). How it works:
// - ImGui's ItemAdd offers every labeled item drawn (imgui_hooks.c, GREHighlight on): its label as shown, its rectangle,
//   its root window, disabled or not. Two buffers: the items of the frame being drawn, the last whole frame's.
// - A control is a path of clicks from a clean editor: ["File"], ["File", "Save All"], [right click on the viewport,
//   "Add Actor", ...]. Each episode resets (Escape, the modals' Cancel), replays its path by the injected mouse (an item
//   found by its window and label in the last frame), then watches some frames: the frame time, the validation errors,
//   the asserts, the texts without translation it brought. A menu or a popup that opened adds its items as new
//   controls; a window that opened adds its buttons and tabs (one level). Then the cleanup closes what it opened (the
//   windows' X, the asset editors' documents, the default layout back).
// - Then Play: some seconds of the game (the HUD), the pause menu opened and each of its buttons clicked (the options).
#include "ui_sweep.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "async.h"
#include "content.h"
#include "debug_viewer.h"
#include "editor.h"
#include "editor_docs.h"
#include "editor_dock.h"
#include "game_settings.h"
#include "i18n.h"
#include "material.h"
#include "platform.h"
#include "project.h"
#include "ui_widget.h"
#include "vkctx.h"

extern int GREHighlight;
extern void (*g_ui_sweep_item_hook)(const char* label, float x0, float y0, float x1, float y1); // (imgui_hooks.c)
extern bool (*g_ui_sweep_assert_hook)(const char* expr, const char* file, int line);           // (crashlog.c)

// --- The items of a frame -------------------------------------------------------------------------------------------
#define IT_MAX 6144
typedef struct SwItem {
    char label[96], win[64];
    float r[4];
    int wflags;
    bool disabled;
} SwItem;
static SwItem g_it[2][IT_MAX];
static int g_nit[2], g_fill; // g_it[g_fill]: this frame's; g_it[!g_fill]: the last whole frame's

// --- The controls to go through --------------------------------------------------------------------------------------
enum { K_CLICK = 0, K_RCLICK };
enum { P_VIEWPORT = 0, P_OUTLINER, P_CONTENT, P_GRAPH, P_COUNT };
static const char* const g_place[P_COUNT] = { "viewport", "Outliner", "Content Browser", "graph" };
#define STEPS 5
typedef struct Step {
    char win[64], label[96];
    unsigned char kind, place;
    bool popup; // (in a popup: found by its label in whatever popup is up, a right click menu's id varies)
} Step;
typedef struct Target {
    Step s[STEPS];
    unsigned char n;
    bool in_window; // its last step is inside a window (not a menu): popups it opens aren't followed
} Target;
#define Q_MAX 4096
static Target g_q[Q_MAX];
static int g_qn, g_qi;
#define SEEN_CAP 16384
static unsigned g_seen[SEEN_CAP];

enum { S_WAIT, S_SEED, S_RESET, S_STEP, S_OBSERVE, S_CLEAN, S_PLAY, S_PLAY_MENU, S_PLAY_CLICK, S_PLAY_END, S_DONE };
static struct {
    bool on, done, ok;
    int state, frames, sub, step, tries;
    char dir[600], bp[512];
    FILE* out;
    int count, fails, skipped, unreached, children, by_api;
    Target cur;
    double t0, budget;
    int max_episodes;
    // per episode
    int vk0, miss0, asserts, undo0, scal0;
    bool vsync0;
    bool retried;
    int scroll_step, sib_i, graph_spot; // (an item out of sight: the window scrolled a step at a time)
    char assert_text[256];
    float worst_ms;
    size_t mem0;
    char base_wins[64][64], ep_wins[64][64]; // (the editor's windows at the start; those before this control)
    int nbase, nep;
    bool toggled, closing;
    int replay_n; // (the toggle-off replay: its first steps, the menu part) // (a window a menu item toggled on: the same path again to toggle it off)
    char problems[1024];
    // Play
    char buttons[64][64];
    int nbuttons, button;
    double play_t;
    Editor* ed;
} W;

static unsigned hash_str(const char* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}
static bool seen_add(const char* name) { // false: seen already
    unsigned h = hash_str(name);
    for (unsigned k = h & (SEEN_CAP - 1), n = 0; n < SEEN_CAP; k = (k + 1) & (SEEN_CAP - 1), n++) {
        if (g_seen[k] == h) return false;
        if (!g_seen[k]) return g_seen[k] = h, true;
    }
    return false;
}

static void target_name(const Target* t, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < t->n; i++) {
        size_t l = strlen(out);
        if (t->s[i].kind == K_RCLICK) snprintf(out + l, cap - l, "%s[right click: %s]", i ? " > " : "", g_place[t->s[i].place]);
        else snprintf(out + l, cap - l, "%s%s", i ? " > " : "", t->s[i].label);
    }
}

// The destructive or run-ending controls, never clicked (lowercase parts of a label in en, pt, es)
static const char* const g_never[] = {
    "delete", "apagar", "excluir", "eliminar", "borrar", "remove", "remover", "quitar", "exit", "sair", "salir", "quit",
    "close project", "fechar projeto", "cerrar proyecto", "new project", "novo projeto", "nuevo proyecto", "open project",
    "abrir projeto", "abrir proyecto", "recent", "publish", "publicar", "send", "enviar", "report", "relat", "informe",
    "push", "commit", "submit", "source control", "controle de vers", "control de vers", "git", "lfs", "package",
    "empacotar", "empaquetar", "url", "website", "site", "github", "donat", "doar", "donar", "doação", "update", "atualiz",
    "actualiz", "restart", "reinici", "language", "idioma", "full screen", "fullscreen", "tela cheia", "pantalla completa",
    "steam", "host", "hospedar", "join", "server", "servidor", "play", "jogar", "jugar", "simulat", "simular",
    "standalone", "launch", "preview vr", "vr preview", "prévia em vr", "previa en vr", "vista previa en vr", "novo processo", "new process", "record", "gravar", "grabar", "movie render", "render queue",
    "crash", "travar", "close all", "fechar todos", "cerrar todos", "log out", "sign", "connect", "conectar", "mcp",
    "build all", "compilar tudo", "compilar todo", "android", "quest", "device", "aparelho", "dispositivo", "dlss",
    "fsr", "xess", "download", "baixar", "descargar", "clear", "limpar", "limpiar", "reset", "redefinir", "restablecer",
};
static bool never_click(const char* label) {
    char l[96];
    size_t n = strlen(label);
    if (n >= sizeof l) n = sizeof l - 1;
    for (size_t i = 0; i < n; i++) l[i] = (char)tolower((unsigned char)label[i]);
    l[n] = 0;
    for (size_t i = 0; i < sizeof g_never / sizeof g_never[0]; i++)
        if (strstr(l, g_never[i])) return true;
    return false;
}

static void push(const Target* t) {
    if (g_qn >= Q_MAX) return;
    char name[600];
    target_name(t, name, sizeof name);
    const char* only = getenv("REALENGINE_SWEEP_ONLY"); // (a part of the sweep: the paths with this in them, and the starts)
    if (only && only[0] && t->n > 1 && !strstr(name, only)) return;
    if (!seen_add(name)) return;
    g_q[g_qn++] = *t;
}

// --- The hook: each labeled item drawn ------------------------------------------------------------------------------
void ui_sweep_item(const char* label, float x0, float y0, float x1, float y1) {
    if (!W.on || !label) return;
    ImGuiContext* g = igGetCurrentContext();
    if (!(g->LastItemData.StatusFlags & ImGuiItemStatusFlags_Visible) || x1 - x0 < 2.0f || y1 - y0 < 2.0f) return;
    ImGuiWindow* w = g->CurrentWindow;
    ImGuiWindow* root = w ? w->RootWindow : NULL;
    if (!root || (root->Flags & ImGuiWindowFlags_Tooltip)) return;
    while (*label == ' ') label++; // (a menu item's room for its icon)
    const char* cut = strstr(label, "##");
    size_t n = cut ? (size_t)(cut - label) : strlen(label);
    while (n && label[n - 1] == ' ') n--;
    static char named[96];
    if (!n && cut) { // an icon button with no text ("##tb_save"): its id names it ("#tb_save")
        const char* id = cut;
        while (*id == '#') id++;
        if (strlen(id) < 3 || strlen(id) > 60 || strchr(id, '%')) return;
        snprintf(named, sizeof named, "#%s", id);
        label = named, n = strlen(named);
    }
    bool letters = false;
    for (size_t i = 0; i < n && !letters; i++) letters = isalpha((unsigned char)label[i]) || (unsigned char)label[i] >= 0xC0;
    if (!letters || n >= 96) return; // (an icon alone, a value: not a control with a name)
    int k = g_nit[g_fill];
    if (k >= IT_MAX) return;
    SwItem* it = &g_it[g_fill][k];
    memcpy(it->label, label, n), it->label[n] = 0;
    snprintf(it->win, sizeof it->win, "%s", root->Name);
    it->r[0] = x0, it->r[1] = y0, it->r[2] = x1, it->r[3] = y1;
    it->wflags = (int)root->Flags;
    it->disabled = (g->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
    g_nit[g_fill] = k + 1;
}

bool ui_sweep_assert(const char* expr, const char* file, int line) {
    if (!W.on || W.done) return false;
    W.asserts++;
    const char* f = file ? file : "?";
    for (const char* c = f; *c; c++)
        if (*c == '/' || *c == '\\') f = c + 1;
    snprintf(W.assert_text, sizeof W.assert_text, "IM_ASSERT(%s) %s:%d", expr ? expr : "?", f, line);
    return true;
}

// --- Input --------------------------------------------------------------------------------------------------------------
static void mouse_to(float x, float y) {
    ImGuiIO* io = igGetIO_Nil();
    ImGuiIO_AddMouseSourceEvent(io, ImGuiMouseSource_Mouse);
    ImGuiIO_AddMouseViewportEvent(io, igGetMainViewport()->ID);
    ImGuiIO_AddMousePosEvent(io, x, y);
}
static void button(int b, bool down) { ImGuiIO_AddMouseButtonEvent(igGetIO_Nil(), b, down); }
static void key(ImGuiKey k, bool down) { ImGuiIO_AddKeyEvent(igGetIO_Nil(), k, down); }

static bool is_popup(int flags);
static bool once_in_window(const SwItem* li, int n, int at) { // (its label the only one of the kind in its window)
    for (int i = 0; i < n; i++)
        if (i != at && !strcmp(li[i].win, li[at].win) && !strcmp(li[i].label, li[at].label)) return false;
    return true;
}
static int popup_count(void) { // the popups up in the last frame (distinct windows with items)
    const SwItem* it = g_it[!g_fill];
    int n = 0;
    const char* last = "";
    for (int i = 0; i < g_nit[!g_fill]; i++)
        if (is_popup(it[i].wflags) && strcmp(it[i].win, last)) last = it[i].win, n++;
    return n;
}
static const SwItem* find_item_ex(const char* win, const char* label, bool popup) {
    const SwItem* last = g_it[!g_fill];
    for (int i = 0; i < g_nit[!g_fill]; i++)
        if (!strcmp(last[i].label, label) && (popup ? is_popup(last[i].wflags) : !win[0] || !strcmp(last[i].win, win))) return &last[i];
    return NULL;
}
static const SwItem* find_item(const char* win, const char* label) { return find_item_ex(win, label, false); }

static bool place_point(int p, float* x, float* y) {
    float r[4];
    switch (p) {
    case P_VIEWPORT:
        if (!dock_panel_rect(DP_VIEWPORT, r)) return false;
        *x = r[0] + r[2] * 0.5f, *y = r[1] + r[3] * 0.55f;
        return true;
    case P_OUTLINER: { // the first actor's row (the same each time: a fixed spot hit a level's row or the empty area)
        const SwItem* it = g_it[!g_fill];
        for (int i = 0; i < g_nit[!g_fill]; i++)
            if (strstr(it[i].win, "###re_outliner") && it[i].label[0] != '#') {
                *x = (it[i].r[0] + it[i].r[2]) * 0.5f, *y = (it[i].r[1] + it[i].r[3]) * 0.5f;
                return true;
            }
        if (!dock_panel_rect(DP_OUTLINER, r)) return false;
        *x = r[0] + r[2] * 0.3f, *y = r[1] + fminf(90.0f, r[3] * 0.5f);
        return true;
    }
    case P_CONTENT: // (its empty corner, bottom right: the assets fill it from the top left; on one, the asset's menu came)
        if (!dock_panel_rect(DP_CONTENT, r)) return false;
        *x = r[0] + r[2] - 24.0f, *y = r[1] + r[3] - 18.0f;
        return true;
    case P_GRAPH: { // the open Blueprint's window: a spot of its canvas
        ImGuiContext* g = igGetCurrentContext();
        for (int i = 0; i < g->Windows.Size; i++) {
            ImGuiWindow* w = g->Windows.Data[i];
            if (w->Active && !w->Hidden && (!w->DockIsActive || w->DockTabIsVisible) && strstr(w->Name, "###bpg") &&
                !strchr(strstr(w->Name, "###bpg"), '/')) { // (the graph whose tab shows: a hidden tab's spot is another panel)
                // (its canvas: a second try a little higher, the first spot fell on a node or a comment)
                // (spots of the canvas tried in turn: an item only a node's own menu has, Break All Links, needs a linked node)
                static const float spots[6][2] = { { 0.5f, 0.7f }, { 0.35f, 0.45f }, { 0.25f, 0.3f }, { 0.6f, 0.4f }, { 0.45f, 0.55f }, { 0.3f, 0.6f } };
                const float* sp = spots[W.graph_spot % 6];
                *x = w->Pos.x + w->Size.x * sp[0], *y = w->Pos.y + w->Size.y * sp[1];
                return true;
            }
        }
        return false;
    }
    default: return false;
    }
}

// --- The windows open now --------------------------------------------------------------------------------------------
static bool is_popup(int flags) { return (flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)) != 0; }
static bool in_base(const char* name) {
    for (int i = 0; i < W.nbase; i++)
        if (!strcmp(W.base_wins[i], name)) return true;
    return false;
}
static bool in_ep(const char* name) {
    for (int i = 0; i < W.nep; i++)
        if (!strcmp(W.ep_wins[i], name)) return true;
    return false;
}
static void ep_windows(void) {
    ImGuiContext* g = igGetCurrentContext();
    W.nep = 0;
    for (int i = 0; i < g->Windows.Size && W.nep < 64; i++) {
        ImGuiWindow* w = g->Windows.Data[i];
        if (w->Active && w->RootWindow == w && !is_popup((int)w->Flags)) snprintf(W.ep_wins[W.nep++], 64, "%s", w->Name);
    }
}
static void base_windows(void) {
    ImGuiContext* g = igGetCurrentContext();
    W.nbase = 0;
    for (int i = 0; i < g->Windows.Size && W.nbase < 64; i++) {
        ImGuiWindow* w = g->Windows.Data[i];
        if (w->Active && w->RootWindow == w && !is_popup((int)w->Flags)) snprintf(W.base_wins[W.nbase++], 64, "%s", w->Name);
    }
}

static size_t private_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm) ? pm.PrivateUsage : 0;
}

// The processes this one started that still run (a VR Preview, a Play in its own process, a package's tools): their
// names into out (may be NULL), killed when kill; how many
static int child_processes(bool kill, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD me = GetCurrentProcessId();
    int n = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ParentProcessID != me) continue;
        char name[260];
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof name, NULL, NULL);
        if (!_stricmp(name, "conhost.exe")) continue; // (the console's host of a test run: not one it started)
        // (the GPU vendor's own updater the DLSS DLL starts when it loads: it ends by itself; never killed)
        if (!_stricmp(name, "nvngx_update.exe")) continue;
        n++;
        if (out && cap) {
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "%s%s", l ? ", " : "", name);
        }
        if (kill) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) TerminateProcess(h, 1), CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return n;
}

static void problem(const char* fmt, const char* a) {
    size_t l = strlen(W.problems);
    char one[400];
    snprintf(one, sizeof one, fmt, a ? a : "");
    snprintf(W.problems + l, sizeof W.problems - l, "%s%s", l ? "; " : "", one);
}

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fputc('\\', f), fputc(c, f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void mark_running(const char* name) { // (a crash: this file names the control)
    char p[700];
    snprintf(p, sizeof p, "%s/ui_sweep_running.txt", W.dir);
    FILE* f = fopen(p, "wb");
    if (f) fprintf(f, "%s\n", name), fclose(f);
}

static void write_result(const char* name, const char* result, const char* where) {
    if (!W.out) return;
    fputs("{\"control\":", W.out);
    json_str(W.out, name);
    fputs(",\"result\":", W.out);
    json_str(W.out, result);
    fputs(",\"where\":", W.out);
    json_str(W.out, where);
    fprintf(W.out, ",\"worst_ms\":%.1f,\"problems\":", W.worst_ms);
    json_str(W.out, W.problems);
    fputs(",\"log\":", W.out);
    json_str(W.out, W.ed && W.ed->log_count ? W.ed->log[W.ed->log_count - 1] : "");
    fputs("}\n", W.out);
    fflush(W.out);
}

// --- The episodes ----------------------------------------------------------------------------------------------------
static void begin_episode(void) {
    W.vk0 = g_vk_validation_errors, W.miss0 = i18n_missing_count(), W.asserts = 0, W.assert_text[0] = 0;
    W.worst_ms = 0.0f, W.problems[0] = 0, W.mem0 = private_bytes();
    W.undo0 = W.ed ? W.ed->undo_count : 0;
    W.scal0 = W.ed ? W.ed->scalability : 1;
    W.vsync0 = W.ed && W.ed->vk ? W.ed->vk->vsync : false;
    W.toggled = W.closing = false;
    ep_windows();
    W.retried = false;
    char name[600];
    target_name(&W.cur, name, sizeof name);
    mark_running(name);
}

static void end_episode(const char* where) {
    char name[600];
    target_name(&W.cur, name, sizeof name);
    if (g_vk_validation_errors > W.vk0) {
        char t[400];
        snprintf(t, sizeof t, "%d validation error(s): %.300s", g_vk_validation_errors - W.vk0, g_vk_last_validation);
        problem("%s", t);
    }
    if (W.asserts) problem("%s", W.assert_text);
    for (int i = W.miss0, shown = 0; i < i18n_missing_count() && shown < 4; i++) // (an ImGui id passed through TR: not a text)
        if (strncmp(i18n_missing_at(i), "##", 2)) problem("no translation: \"%s\"", i18n_missing_at(i)), shown++;
    if (W.worst_ms > 400.0f) {
        char t[64];
        snprintf(t, sizeof t, "a frame of %.0f ms", W.worst_ms);
        problem("%s", t);
    }
    bool fail = W.problems[0] != 0;
    W.count++, W.fails += fail;
    write_result(name, fail ? "fail" : "ok", where);
}

// The editor's documents closed, the layout's panels back, the windows the control opened closed by their X
// A popup left up: a dialog's Cancel / Close / No / OK clicked; a menu: closed by ImGui
static int close_popup(void) {
    const SwItem* it = g_it[!g_fill];
    const char* answers[] = { TR("Cancel"), TR("Close"), TR("No"), TR("Don't Save"), TR("OK") };
    bool any = false;
    for (int i = 0; i < g_nit[!g_fill]; i++) {
        if (!is_popup(it[i].wflags)) continue;
        any = true;
        for (size_t a = 0; a < sizeof answers / sizeof answers[0]; a++)
            if (!strcmp(it[i].label, answers[a])) {
                mouse_to((it[i].r[0] + it[i].r[2]) * 0.5f, (it[i].r[1] + it[i].r[3]) * 0.5f);
                return 1;
            }
    }
    // (the menus and right click menus: the editor's keyboard navigation is off, Escape doesn't close them; ImGui does)
    if (any && igGetCurrentContext()->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true);
    return 0;
}

static int close_new_windows(void) {
    ImGuiContext* g = igGetCurrentContext();
    for (int i = 0; i < g->Windows.Size; i++) {
        ImGuiWindow* w = g->Windows.Data[i];
        if (!w->Active || w->RootWindow != w || is_popup((int)w->Flags) || in_base(w->Name) || w->DockIsActive) continue;
        if (!w->HasCloseButton || (w->Flags & ImGuiWindowFlags_NoTitleBar)) continue;
        const ImGuiStyle* st = igGetStyle();
        float fs = igGetFontSize();
        float x = w->Pos.x + w->Size.x - st->FramePadding.x - fs * 0.5f, y = w->Pos.y + st->FramePadding.y + fs * 0.5f;
        mouse_to(x, y);
        return 1;
    }
    return 0;
}

static void seed(Editor* ed) {
    (void)ed;
    base_windows();
    if (getenv("REALENGINE_SWEEP_DEBUG")) // (what it sees: every item of the editor at the start)
        for (int i = 0; i < g_nit[!g_fill]; i++)
            printf("[RealEngine] UI sweep debug item: %s | %s%s\n", g_it[!g_fill][i].win, g_it[!g_fill][i].label, g_it[!g_fill][i].disabled ? " (disabled)" : "");
    const SwItem* it = g_it[!g_fill];
    for (int i = 0; i < g_nit[!g_fill]; i++) { // the menu bar, the toolbar and status bar (the editor's own window)
        if (it[i].disabled || never_click(it[i].label)) continue;
        if (strcmp(it[i].win, "##MainMenuBar") && strcmp(it[i].win, "##editor")) continue;
        Target t;
        memset(&t, 0, sizeof t);
        t.n = 1;
        snprintf(t.s[0].win, 64, "%s", it[i].win), snprintf(t.s[0].label, 96, "%s", it[i].label);
        push(&t);
    }
    for (int p = 0; p < P_COUNT; p++) { // the right click menus
        Target t;
        memset(&t, 0, sizeof t);
        t.n = 1, t.s[0].kind = K_RCLICK, t.s[0].place = (unsigned char)p;
        push(&t);
    }
    static char found[16][512]; // (a graph to right click in: a Blueprint of the project's)
    int n = content_search(".rbp", found, 16);
    for (int i = 0; i < n && !W.bp[0]; i++)
        if (strstr(found[i], ".rbp")) snprintf(W.bp, sizeof W.bp, "%s", found[i]);
}

// After a control: the new popups' items (a menu chain) and a new window's buttons and tabs become controls
static void discover(void) {
    if (W.cur.n >= STEPS) return;
    const SwItem* it = g_it[!g_fill];
    static char wins[32][64];
    static int per[32];
    int nwins = 0;
    for (int i = 0; i < g_nit[!g_fill]; i++) {
        bool popup = is_popup(it[i].wflags);
        if (popup && W.cur.in_window) continue; // (a combo's list inside a window: not followed)
        if (!popup && (in_ep(it[i].win) || W.cur.in_window)) continue; // (a window this control opened: its items, one level)
        if (!strcmp(it[i].win, "##MainMenuBar")) continue;
        bool parent = false; // (the menus the path went through, still open around the new one: not again)
        for (int k = 0; k < W.cur.n && !parent; k++) parent = !strcmp(W.cur.s[k].win, it[i].win);
        if (parent) continue;
        if (it[i].disabled || never_click(it[i].label)) continue;
        int w = 0; // (48 controls a window at most: its own count, the windows' items interleaved in the list)
        while (w < nwins && strcmp(wins[w], it[i].win)) w++;
        if (w == nwins) {
            if (nwins == 32) continue;
            snprintf(wins[nwins], 64, "%s", it[i].win), per[nwins++] = 0;
        }
        if (++per[w] > 48) continue;
        Target t = W.cur;
        Step* s = &t.s[t.n++];
        memset(s, 0, sizeof *s);
        snprintf(s->win, 64, "%s", it[i].win), snprintf(s->label, 96, "%s", it[i].label);
        s->popup = popup;
        t.in_window = !popup;
        push(&t);
    }
}

void ui_sweep_start(const char* out_dir) {
    memset(&W, 0, sizeof W);
    W.on = true, W.ok = true;
    snprintf(W.dir, sizeof W.dir, "%s", out_dir && out_dir[0] ? out_dir : ".");
    CreateDirectoryA(W.dir, NULL);
    char p[700];
    snprintf(p, sizeof p, "%s/ui_sweep.jsonl", W.dir);
    W.out = fopen(p, "wb");
    const char* e = getenv("REALENGINE_SWEEP_MAX");
    W.max_episodes = e ? atoi(e) : 1500;
    e = getenv("REALENGINE_SWEEP_SECONDS");
    W.budget = e ? atof(e) : 600.0; // (a template project: ~870 controls, ~6 min)
    if (!getenv("REALENGINE_I18N_MISSING")) { // (the texts without translation: recorded for each control)
        snprintf(p, sizeof p, "%s/i18n_missing.txt", W.dir);
        i18n_record_missing(p);
    }
    g_ui_sweep_item_hook = ui_sweep_item, g_ui_sweep_assert_hook = ui_sweep_assert;
    g_platform_synthetic_input = true; // (no dialog of the system, no Explorer, no browser: platform_win32.c, crashlog.c)
}
bool ui_sweep_active(void) { return W.on && !W.done; }
bool ui_sweep_finished(bool* ok) {
    if (ok) *ok = W.ok;
    return W.done;
}

static void finish(void) {
    W.done = true;
    W.ok = W.fails == 0;
    if (W.out) fclose(W.out), W.out = NULL;
    char p[700];
    snprintf(p, sizeof p, "%s/ui_sweep_running.txt", W.dir);
    remove(p);
    printf("[RealEngine] UI sweep: %d controls, %d failed, %d skipped (time), %d not reached, %d asset(s) opened by the API, %.0f s; report "
           "%s/ui_sweep.jsonl\n",
           W.count, W.fails, W.skipped, W.unreached, W.by_api, re_time_seconds() - W.t0, W.dir);
    printf(W.ok ? "[RealEngine] UI sweep OK\n" : "[RealEngine] UI sweep: %d control(s) with problems (tools/ui_sweep_report.py)\n", W.fails);
    { // (the tests after it: no process of its left on the GPU; each control's were closed, and a last look here)
        char names[400];
        int left = child_processes(true, names, sizeof names);
        if (left) printf("[RealEngine] FAIL: UI sweep: %d process(es) it started still running at the end: %s (closed now)\n", left, names);
        else printf("[RealEngine] UI sweep: no process of its left running (%d control(s) started one: closed after each)\n", W.children);
    }
    fflush(stdout);
}

void ui_sweep_input(Editor* ed) {
    if (!W.on || W.done) return;
    W.ed = ed;
    g_fill ^= 1, g_nit[g_fill] = 0; // (the frame that ended: g_it[!g_fill])
    GREHighlight = 1;
    float ms = igGetIO_Nil()->DeltaTime * 1000.0f;
    if (ms > W.worst_ms && W.state >= S_STEP) W.worst_ms = ms;
    W.frames++;
    switch (W.state) {
    case S_WAIT: // the editor's first frames (the layout, the fades, the project's first bakes)
        // (and the start's background work done: the materials, the meshes; the first control took its hitch: Edit, 1.6 s)
        if (W.frames < 150 || ((!async_idle() || mat_compiling()) && W.frames < 1800)) return;
        W.t0 = re_time_seconds();
        if (!project_file()[0]) { // (--new-project failed, its folder left from a run before: never the engine's own Content)
            printf("[RealEngine] FAIL: UI sweep: no project open (did --new-project fail? its folder must not exist yet)\n");
            fflush(stdout);
            W.fails++, W.state = S_DONE, W.frames = 1000;
            return;
        }
        ed->less_cpu_in_background = false; // (its windows are never focused: 30 ms asleep a frame otherwise; prefs not saved)
        dock_layout_default(ed);
        W.state = S_SEED, W.frames = 0;
        return;
    case S_SEED:
        if (W.frames < 10) return;
        seed(ed);
        printf("[RealEngine] UI sweep: %d controls to start from (menus, toolbar, right click menus)\n", g_qn);
        fflush(stdout);
        W.state = S_RESET, W.frames = 0, W.sub = 0;
        return;
    case S_RESET: { // a clean editor: Escape twice, a modal's Cancel, then the next control
        if (W.sub < 4) { key(ImGuiKey_Escape, (W.sub & 1) == 0), W.sub++; return; } // (twice: a menu bar left active too)
        if (W.frames < 5) return;
        if (g_qi >= g_qn || W.count >= W.max_episodes || re_time_seconds() - W.t0 > W.budget) {
            if (g_qi < g_qn) W.skipped = g_qn - g_qi;
            W.state = S_PLAY, W.frames = 0, W.sub = 0;
            return;
        }
        // (the windows the menus toggle, never up at the start: a flag left on made the next control's click close them
        // instead of opening them: Editor Preferences..., Project Settings..., the modal Open Asset)
        ed->show_open_asset = ed->show_prefs = ed->show_project_settings = false;
        ed->less_cpu_in_background = false; // (Preferences > Performance's box clicked by a control: 30 ms asleep a frame again)
        if (igGetCurrentContext()->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true); // (a menu left up: from scratch)
        W.cur = g_q[g_qi++];
        begin_episode();
        if (W.cur.s[0].kind == K_RCLICK && W.cur.s[0].place == P_GRAPH && W.bp[0]) content_open_asset(W.bp); // (its graph open)
        W.state = S_STEP, W.step = 0, W.sub = 0, W.frames = 0, W.tries = 0, W.scroll_step = 0, W.sib_i = 0, W.graph_spot = 0;
        return;
    }
    case S_STEP: {
        if (W.cur.s[0].kind == K_RCLICK && W.cur.s[0].place == P_GRAPH && W.step == 0 && W.frames < 40) {
            if (W.frames == 39) ep_windows(); // (the Blueprint's own windows, opened for it: not the control's)
            return;
        }
        if (W.step >= (W.closing ? W.replay_n : W.cur.n)) {
            W.state = S_OBSERVE, W.frames = 0;
            return;
        }
        const Step* s = &W.cur.s[W.step];
        static float x, y;
        if (W.sub == 0) { // where: the item in the last frame (a menu opening: some frames), or the place
            if (s->kind == K_RCLICK) {
                if (!place_point(s->place, &x, &y)) {
                    if (++W.tries < 40) return;
                    end_episode("not reached"), W.count--, W.state = S_CLEAN, W.frames = 0, W.sub = 0;
                    return;
                }
            } else {
                const SwItem* it = find_item_ex(s->win, s->label, s->popup);
                // (Open Asset's row as a step on the way, not the control itself: the asset opened by its path at once, a row
                // half hidden in the list didn't take the click; and when the row isn't up)
                if (strstr(s->win, "###open_asset") && (W.step < W.cur.n - 1 || (!it && W.tries >= 10))) { // (Open Asset's row not up: the asset opened
                    // as that row opens it, by the Content Browser's path; the result says so)
                    static char found[8][512];
                    int n = content_search(s->label, found, 8);
                    for (int i = 0; i < n; i++) {
                        const char* nm = strrchr(found[i], '/');
                        if (nm && !strcmp(nm + 1, s->label)) {
                            if (igGetCurrentContext()->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true);
                            ed->show_open_asset = false;
                            content_open_asset(found[i]);
                            W.by_api++;
                            W.step++, W.sub = 0, W.tries = 0, W.scroll_step = 0, W.sib_i = 0, W.frames = 0;
                            return;
                        }
                    }
                }
                if (!it) {
                    if (++W.tries < (s->popup ? 25 : 30)) return; // (a window's rows can come some frames late: the Debug Viewer measures once open)
                    // not in sight: the window's state kept from an earlier control (a header left open pushed it down, a list
                    // scrolled): its scrolling regions gone through from the top to the bottom, 2 frames a step
                    if (!s->popup && W.scroll_step <= 12) {
                        if (((W.tries - 30) & 1) == 0) {
                            float f = (float)W.scroll_step++ / 12.0f;
                            ImGuiContext* g = igGetCurrentContext();
                            for (int i = 0; i < g->Windows.Size; i++) {
                                ImGuiWindow* w = g->Windows.Data[i];
                                if (w->Active && w->RootWindow && !strcmp(w->RootWindow->Name, s->win) && w->ScrollMax.y > 0.0f)
                                    igSetScrollY_WindowPtr(w, w->ScrollMax.y * f);
                            }
                        }
                        return;
                    }
                    // still not there: another page of the window shows (a mode, a tab, a section a control left chosen:
                    // Behavior Tree > Blackboard, Sound Wave > Sound). The window's other controls clicked one at a time,
                    // the item looked for after each (24 at most: a node, a variable selected first; never the ones never clicked)
                    if (!s->popup && W.sib_i < 24) {
                        int phase = (W.tries - 30) % 8; // (a click: move, down, up, then 5 frames to look)
                        static float sx, sy;
                        if (phase == 0) {
                            const SwItem* li = g_it[!g_fill];
                            int k = 0, picked = -1;
                            for (int i = 0; i < g_nit[!g_fill] && picked < 0; i++)
                                if (!strcmp(li[i].win, s->win) && !li[i].disabled && !never_click(li[i].label) && strcmp(li[i].label, s->label) &&
                                    once_in_window(li, g_nit[!g_fill], i) && k++ == W.sib_i) // (a label repeated, "#src": the rows' fields, not a page)
                                    picked = i;
                            W.sib_i++;
                            if (picked < 0) { W.sib_i = 24; return; }
                            sx = (li[picked].r[0] + li[picked].r[2]) * 0.5f, sy = (li[picked].r[1] + li[picked].r[3]) * 0.5f;
                            mouse_to(sx, sy);
                        } else if (phase == 1) button(0, true);
                        else if (phase == 2) button(0, false);
                        else if (phase == 3 && igGetCurrentContext()->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true); // (a combo it opened)
                        return;
                    }
                    if (W.step == 1 && W.cur.s[0].kind == K_RCLICK && W.cur.s[0].place == P_GRAPH && W.graph_spot < 5) { // (another spot)
                        if (igGetCurrentContext()->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true);
                        W.graph_spot++, W.step = 0, W.tries = 0, W.scroll_step = 0, W.sib_i = 0, W.sub = 0;
                        return;
                    }
                    if (W.step > 0 && !W.retried) { // (the menu that holds it closed by the click: that one once more)
                        W.retried = true, W.step--, W.tries = 0, W.scroll_step = 0, W.sib_i = 0;
                        return;
                    }
                    W.state = S_CLEAN, W.frames = 0, W.sub = 0; // (not reached this time: noted, not a failure)
                    if (W.closing) { W.closing = false; return; } // (the toggle-off replay: nothing to note)
                    W.unreached++;
                    {
                        char name[600], where[600], wins[400] = "";
                        target_name(&W.cur, name, sizeof name);
                        for (int i = 0, n = 0; i < g_nit[!g_fill] && n < 6; i++) // (the windows up then: why it wasn't found)
                            if (is_popup(g_it[!g_fill][i].wflags) && !strstr(wins, g_it[!g_fill][i].win)) {
                                size_t l = strlen(wins);
                                snprintf(wins + l, sizeof wins - l, "%s%s", l ? "," : "", g_it[!g_fill][i].win), n++;
                            }
                        char state[300] = "";
                        { // (the window it looked in: up or not, collapsed, its tab, its scroll, the labels it has now)
                            ImGuiWindow* tw = igFindWindowByName(s->win);
                            if (tw) {
                                int k = snprintf(state, sizeof state, "window active %d collapsed %d tab %d scroll %.0f/%.0f; has:", tw->Active,
                                                 tw->Collapsed, tw->DockIsActive ? tw->DockTabIsVisible : -1, tw->Scroll.y, tw->ScrollMax.y);
                                for (int i = 0, n = 0; i < g_nit[!g_fill] && n < 5 && k > 0 && k < (int)sizeof state; i++)
                                    if (!strcmp(g_it[!g_fill][i].win, s->win))
                                        k += snprintf(state + k, sizeof state - (size_t)k, " %.20s,", g_it[!g_fill][i].label), n++;
                            } else snprintf(state, sizeof state, "window not there");
                        }
                        snprintf(where, sizeof where, "step %d: %.40s in %.40s; popups up: %.60s; %s", W.step + 1, s->label, s->win, wins, state);
                        W.problems[0] = 0;
                        write_result(name, "unreached", where);
                    }
                    return;
                }
                if (it->disabled) {
                    W.state = S_CLEAN, W.frames = 0, W.sub = 0;
                    return;
                }
                x = (it->r[0] + it->r[2]) * 0.5f, y = (it->r[1] + it->r[3]) * 0.5f;
            }
            mouse_to(x, y), W.sub = 1;
            return;
        }
        int b = s->kind == K_RCLICK ? 1 : 0;
        static int popups_before;
        if (W.sub == 1) { popups_before = popup_count(), mouse_to(x, y), W.sub = 10; return; } // (frames hovering: menus open on hover)
        if (W.sub == 10) { W.sub = s->kind == K_RCLICK && s->place == P_GRAPH ? 20 : 2; return; }
        // (a graph's canvas: a left click first, its window focused; reopened, its right click opened no menu otherwise)
        if (W.sub == 20) { button(0, true), W.sub = 21; return; }
        if (W.sub == 21) { button(0, false), W.sub = 22; return; }
        if (W.sub == 22) { W.sub = 2; return; }
        if (W.sub == 2 && s->kind == K_CLICK && popup_count() > popups_before) { // a menu the hover opened (a menu bar left
            // active, a submenu): a click would close it
            W.sub = 4, W.frames = 0;
            return;
        }
        if (W.sub == 2) { button(b, true), W.sub = 3; return; }
        if (W.sub == 3) { button(b, false), W.sub = 4, W.frames = 0; return; }
        if (W.frames < 3) return;
        W.step++, W.sub = 0, W.tries = 0, W.scroll_step = 0, W.sib_i = 0;
        return;
    }
    case S_OBSERVE: // some frames of what it did
        if (W.frames < 9) return;
        if (W.frames < 40 && !W.closing) { // a window it opened with nothing drawn in it yet (a fade, a first layout): wait
            ImGuiContext* g = igGetCurrentContext();
            for (int i = 0; i < g->Windows.Size; i++) {
                ImGuiWindow* w = g->Windows.Data[i];
                if (!w->Active || w->RootWindow != w || is_popup((int)w->Flags) || in_ep(w->Name)) continue;
                bool items = false;
                for (int k = 0; k < g_nit[!g_fill] && !items; k++) items = !strcmp(g_it[!g_fill][k].win, w->Name);
                if (!items) return;
            }
        }
        if (W.closing) { // (the path again to toggle its window off: no result)
            W.closing = false, W.state = S_CLEAN, W.frames = 0, W.sub = 0;
            return;
        }
        { // a menu bar's item clicked and no menu up: the hover had opened it and the click closed it (the bar left
            // active by the last control): clicked once more
            const Step* last = &W.cur.s[W.cur.n - 1];
            bool any_popup = false;
            for (int i = 0; i < g_nit[!g_fill] && !any_popup; i++) any_popup = is_popup(g_it[!g_fill][i].wflags);
            if (last->kind == K_CLICK && !strcmp(last->win, "##MainMenuBar") && !any_popup && !W.retried) {
                W.retried = true, W.step = W.cur.n - 1, W.sub = 0, W.state = S_STEP, W.frames = 0;
                return;
            }
        }
        {
            int before = g_qn;
            discover();
            if (getenv("REALENGINE_SWEEP_DEBUG")) {
                char name[600];
                target_name(&W.cur, name, sizeof name);
                char wins[900] = "";
                ImGuiContext* g = igGetCurrentContext();
                for (int i = 0; i < g->Windows.Size; i++) {
                    ImGuiWindow* w = g->Windows.Data[i];
                    if (w->Active && w->RootWindow == w && !in_ep(w->Name)) {
                        size_t l = strlen(wins);
                        snprintf(wins + l, sizeof wins - l, "%s%.40s (dock %d tab %d collapsed %d skip %d at %.0f,%.0f %.0fx%.0f)", l ? ", " : "", w->Name,
                                 w->DockIsActive, w->DockTabIsVisible, w->Collapsed, w->SkipItems, w->Pos.x, w->Pos.y, w->Size.x, w->Size.y);
                    }
                }
                printf("[RealEngine] UI sweep debug: %s: %d items, %d popups, %d new controls; new windows: %s\n", name, g_nit[!g_fill], popup_count(),
                       g_qn - before, wins);
            }
        }
        end_episode(W.cur.in_window ? "window" : "menu");
        W.state = S_CLEAN, W.frames = 0, W.sub = 0;
        return;
    case S_CLEAN: { // Escape, the documents closed, the windows' X, the default layout
        if (W.sub < 2) { key(ImGuiKey_Escape, W.sub == 0), W.sub++; return; }
        if (W.sub < 4) { W.sub++; return; }
        if (W.sub == 4) {
            if (editor_docs_open()) editor_docs_discard(), editor_docs_finish(ed);
            for (int k = 0; k < 12 && ed->undo_count > W.undo0; k++) editor_undo(ed, re_time_seconds()); // (an actor it added: gone)
            if (dv_open()) dv_set_open(false); // (the Debug Viewer left open covered the menu bar: the next control's menus never opened)
            // (Open Asset is a modal the editor opens again while its flag is on: closed by ImGui alone it came back and,
            // modal, blocked the menu bar for the next controls: Edit's menu "never opened")
            ed->show_open_asset = false;
            { // a process it started (a VR Preview, a Play of its own): closed before the next control (it took the GPU)
                char names[300];
                if (child_processes(true, names, sizeof names)) {
                    W.children++;
                    printf("[RealEngine] UI sweep: closed what this control started: %s\n", names);
                }
            }
            // (a setting it left: the scalability back to what it was; the Cinematic preset kept its MSAA 8x targets, ~+300
            // MB that the next controls would have been charged with: a setting that stays is no leak)
            if (ed->scalability != W.scal0) editor_set_scalability(ed, W.scal0, re_time_seconds()), W.mem0 = (size_t)1 << 60; // (no memory check)
            // (VSync turned on by a control: offscreen it held every control after it at 60 fps, the sweep twice as long)
            if (ed->vk && ed->vk->vsync != W.vsync0) editor_set_vsync(ed, W.vsync0, re_time_seconds());
            W.sub = 5, W.frames = 0;
            return;
        }
        if (W.sub >= 5 && W.sub < 5 + 3 * 6) { // up to 6 windows closed by their X (move, press, release)
            int k = (W.sub - 5) % 3;
            if (k == 0 && !close_popup() && !close_new_windows()) { W.sub = 5 + 3 * 6; return; }
            if (k == 1) button(0, true);
            if (k == 2) button(0, false);
            W.sub++;
            return;
        }
        if (W.sub == 5 + 3 * 6 && !W.toggled) { // a window a menu item toggled on (Window > ...): the same path toggles it off
            // its menu part (the leading clicks on the menus, the toolbar, the status bar): what opened the window, also
            // when the control then clicked inside it (Editor Preferences > a tab: the window stayed, and the next control's
            // "Editor Preferences..." toggled it off)
            int m = 0;
            while (m < W.cur.n && W.cur.s[m].kind == K_CLICK &&
                   (W.cur.s[m].popup || !strcmp(W.cur.s[m].win, "##MainMenuBar") || !strcmp(W.cur.s[m].win, "##editor")))
                m++;
            bool menu_path = m > 0, opened = false;
            ImGuiContext* g = igGetCurrentContext();
            for (int i = 0; i < g->Windows.Size && !opened; i++) {
                ImGuiWindow* w = g->Windows.Data[i];
                opened = w->Active && w->RootWindow == w && !is_popup((int)w->Flags) && !in_ep(w->Name);
            }
            W.toggled = true;
            if (menu_path && opened) {
                if (g->OpenPopupStack.Size > 0) igClosePopupToLevel(0, true);
                W.closing = true, W.replay_n = m, W.state = S_STEP, W.step = 0, W.sub = 0, W.frames = 0, W.tries = 0;
                return;
            }
        }
        if (W.sub == 5 + 3 * 6) { // (the panels as they were: only when one of them went or a window stayed)
            ImGuiContext* g = igGetCurrentContext();
            int shown = 0;
            bool extra = false;
            for (int i = 0; i < g->Windows.Size; i++) {
                ImGuiWindow* w = g->Windows.Data[i];
                if (!w->Active || w->RootWindow != w || is_popup((int)w->Flags)) continue;
                if (in_base(w->Name)) shown++;
                else extra = true;
            }
            if (shown < W.nbase || extra) dock_layout_default(ed);
            mouse_to(-100.0f, -100.0f);
            W.sub++, W.frames = 0;
            return;
        }
        if (W.frames < 2) return;
        if (private_bytes() > W.mem0 + ((size_t)256 << 20)) { // (kept after its windows closed: noted on the control)
            char t[64];
            snprintf(t, sizeof t, "+%u MB kept after closing", (unsigned)((private_bytes() - W.mem0) >> 20));
            W.problems[0] = 0, problem("%s", t);
            char name[600];
            target_name(&W.cur, name, sizeof name);
            write_result(name, "fail", "cleanup"), W.fails++;
        }
        W.state = S_RESET, W.frames = 0, W.sub = 0;
        return;
    }
    case S_PLAY: // Play: the game some seconds (the HUD), then the pause menu
        memset(&W.cur, 0, sizeof W.cur), W.cur.n = 1;
        snprintf(W.cur.s[0].label, 96, "Play (PIE)");
        begin_episode();
        editor_play(ed, re_time_seconds());
        W.play_t = re_time_seconds();
        W.state = S_PLAY_MENU, W.frames = 0;
        return;
    case S_PLAY_MENU:
        if (re_time_seconds() - W.play_t < 3.0) return;
        if (!ed->playing) problem("%s", "Play stopped by itself");
        end_episode("play");
        memset(&W.cur, 0, sizeof W.cur), W.cur.n = 1;
        snprintf(W.cur.s[0].label, 96, "Play > pause menu");
        begin_episode();
        pause_menu_key(&ed->world, false);
        W.state = S_PLAY_CLICK, W.frames = 0, W.button = -1;
        return;
    case S_PLAY_CLICK:
        if (W.frames < 20) return;
        if (W.button < 0) { // the menu's buttons, once
            if (!pause_menu_is_open()) problem("%s", "the pause menu didn't open (Project Settings > Pause Menu)");
            end_episode("play");
            W.nbuttons = ui_test_buttons(W.buttons, 64);
            W.button = 0;
        } else {
            end_episode("play");
            char now[64][64]; // (a page it opened, the Options: its buttons too)
            int n = ui_test_buttons(now, 64);
            for (int i = 0; i < n && W.nbuttons < 64; i++) {
                bool known = false;
                for (int k = 0; k < W.nbuttons && !known; k++) known = !strcmp(W.buttons[k], now[i]);
                if (!known) snprintf(W.buttons[W.nbuttons++], 64, "%s", now[i]);
            }
        }
        while (W.button < W.nbuttons && never_click(W.buttons[W.button])) W.button++;
        if (W.button >= W.nbuttons || !ed->playing) {
            W.state = S_PLAY_END, W.frames = 0;
            return;
        }
        memset(&W.cur, 0, sizeof W.cur), W.cur.n = 2;
        snprintf(W.cur.s[0].label, 96, "Play > pause menu");
        snprintf(W.cur.s[1].label, 96, "%s", W.buttons[W.button]);
        begin_episode();
        if (!pause_menu_is_open()) pause_menu_key(&ed->world, false); // (a button that closed it: open again)
        ui_test_click(&ed->world, W.buttons[W.button++]);
        W.frames = 0;
        return;
    case S_PLAY_END:
        if (W.frames < 10) return;
        if (ed->playing) editor_stop(ed, re_time_seconds());
        W.state = S_DONE, W.frames = 0;
        return;
    case S_DONE:
        if (W.frames < 20) return;
        GREHighlight = 0;
        finish();
        return;
    default: return;
    }
}
