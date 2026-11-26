// The dock test's second part (dock_test.h; 2026-11-25, item 9): the asset editors as Unreal's.
// A Graph (ABP_Mannequin) and a Material open: both tabs of one Editors window. Then three windows of their own out of
// the editor (the Editors window, the Material dragged out of it, the test window): each owned by the editor's main
// window and presented with vsync; the main window raised (a click on it) leaves them above it; the frame rate with
// them measured against the one before. A project switch: no document, no window of its own left, no validation
// error. The editor closes with two windows of their own up: none left after (dock_test_after_shutdown).
#include "dock_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "editor.h"
#include "editor_docs.h"
#include "editor_host.h"
#include "mat_editor.h"
#include "material.h"
#include "platform_files.h"
#include "project.h"
#include "ui_viewports.h"
#include "vkctx.h"

enum { D_OPEN, D_TABS, D_FPS0, D_FLOAT, D_OWNER, D_Z, D_MIN, D_FPS, D_SWITCH, D_SWITCHED, D_REOPEN, D_REOPENED, D_END };

static struct {
    int state, frames, tries, fails;
    char mat[512], proj[700];
    double dt_sum;
    int dt_n;
    float fps0, fps;
    int floating, errors;
    bool test_open; // (the dock test's own window: open, floating out of the editor)
    GLFWwindow* main;
} D;

static void report(const char* what, bool ok) {
    printf("[RealEngine] dock test: %-48s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) D.fails++;
    fflush(stdout);
}

// A root window whose name holds id (its ### part)
static ImGuiWindow* find_window(const char* id) {
    ImGuiContext* g = igGetCurrentContext();
    for (int i = 0; i < g->Windows.Size; i++) {
        ImGuiWindow* w = g->Windows.Data[i];
        const char* h = strstr(w->Name, id); // (a document's own window, docked or not: not one of its children, "<name>/<child>")
        if (h && w->Active && !strchr(h, '/')) return w;
    }
    return NULL;
}

// A window out of the editor: offscreen to the left of the main window in the background (as the main window), else
// over the main window's top edge (out of its client area: a window of its own)
static void float_out(ImGuiWindow* w, int k) {
    int mx, my;
    glfwGetWindowPos(D.main, &mx, &my);
    ImVec2 p;
    if (getenv("REALENGINE_BACKGROUND")) p = (ImVec2){ (float)mx - 900.0f - 40.0f * k, (float)my + 80.0f + 60.0f * k };
    else {
        const ImGuiViewport* mv = igGetMainViewport();
        p = (ImVec2){ mv->Pos.x + 120.0f + 420.0f * k, mv->Pos.y - 30.0f };
    }
    igSetWindowPos_WindowPtr(w, p, ImGuiCond_Always);
    igSetWindowSize_WindowPtr(w, (ImVec2){ k == 0 ? 800.0f : 400.0f, k == 0 ? 520.0f : 300.0f }, ImGuiCond_Always);
}

static bool above_main(HWND h) { // h before the main window in the z-order (the next ones are below it)
    HWND main = (HWND)ui_viewports_main_hwnd();
    for (HWND x = GetWindow(h, GW_HWNDNEXT); x; x = GetWindow(x, GW_HWNDNEXT))
        if (x == main) return true;
    return false;
}

static void measure(void) {
    D.dt_sum += igGetIO_Nil()->DeltaTime, D.dt_n++;
}
static float fps_of(void) { return D.dt_n && D.dt_sum > 0.0 ? (float)(D.dt_n / D.dt_sum) : 0.0f; }

// Each frame once the first part is done (before igNewFrame): true when finished (*ok: no failure); *test_window:
// whether the dock test's window should be open
bool dock_docs_step(Editor* ed, GLFWwindow* main_window, bool* test_window, bool* ok) {
    D.main = main_window;
    D.frames++;
    if (D.frames > 6000) {
        printf("[RealEngine] dock test: stuck at the editors' step %d\n", D.state);
        D.fails++, D.state = D_END;
    }
    ImGuiWindow *bp = find_window("###bpwin"), *mat = find_window("###mat_editor"), *host = find_window("###rivora_editors");
    switch (D.state) {
    case D_OPEN: { // a Graph and a Material, opened as the Content Browser's double click opens them
        *test_window = false;
        static char found[64][512];
        int n = content_search(".rmat", found, 64);
        for (int pass = 0; pass < 2 && !D.mat[0]; pass++) // (a plain material first: M_..., not a Light Function's)
            for (int i = 0; i < n && !D.mat[0]; i++)
                if (strstr(found[i], ".rmat") && (pass || strstr(found[i], "/M_"))) snprintf(D.mat, sizeof D.mat, "%s", found[i]);
        content_open_asset("/Content/Characters/Mannequin/ABP_Mannequin.rbp");
        if (D.mat[0]) content_open_asset(D.mat);
        D.state = D_TABS, D.frames = 0;
        return false;
    }
    case D_TABS: { // one window, a tab of each kind
        if (D.frames < 40 || (mat_editor_open_count() == 0 && D.frames < 600)) return false; // (the material read and baked)
        if (getenv("REALENGINE_DOCK_DEBUG")) {
            ImGuiContext* g = igGetCurrentContext();
            for (int i = 0; i < g->Windows.Size; i++) {
                ImGuiWindow* w = g->Windows.Data[i];
                if (!strstr(w->Name, "###")) continue;
                ImGuiDockNode* r = w->DockNode ? igDockNodeGetRootNode(w->DockNode) : NULL;
                printf("[RealEngine] dock test debug: %s active %d child %d node %08x root %08x\n", w->Name, w->Active,
                       (w->Flags & ImGuiWindowFlags_ChildWindow) != 0, w->DockNode ? w->DockNode->ID : 0, r ? r->ID : 0);
            }
            printf("[RealEngine] dock test debug: Editors dock space %08x, %d materials open\n", edhost_dockspace(), mat_editor_open_count());
        }
        ImGuiDockNode* nb = bp ? bp->DockNode : NULL;
        ImGuiDockNode* nm = mat ? mat->DockNode : NULL;
        ImGuiDockNode* root = nb ? igDockNodeGetRootNode(nb) : NULL;
        bool ok2 = bp && mat && nb && nb == nm && root && root->ID == edhost_dockspace() && edhost_docs() >= 2;
        if (!ok2)
            printf("[RealEngine] dock test:   Graph %s, Material %s (%s), same tab bar %d, in the Editors window %d (%d documents)\n", bp ? "open" : "missing",
                   mat ? "open" : "missing", D.mat[0] ? D.mat : "no .rmat found", nb && nb == nm, root && root->ID == edhost_dockspace(), edhost_docs());
        report("one window, tabs of two kinds (Graph + Material)", ok2);
        D.state = D_FPS0, D.frames = 0, D.dt_sum = 0, D.dt_n = 0;
        return false;
    }
    case D_FPS0: // the frame rate with no window of its own
        if (D.frames > 30) measure();
        if (D.dt_n < 240) return false;
        D.fps0 = fps_of();
        D.state = D_FLOAT, D.frames = 0, D.tries = 0;
        return false;
    case D_FLOAT: // three windows of their own: the Editors window, the Material out of it, the test window
        *test_window = true;
        if (D.frames == 1) {
            if (mat && mat->DockNode) igDockContextQueueUndockWindow(igGetCurrentContext(), mat);
            return false;
        }
        if (D.frames == 4) {
            if (host) float_out(host, 0);
            if (mat) float_out(mat, 1);
            ImGuiWindow* t = igFindWindowByName("Dock Test###re_docktest");
            if (t) {
                if (t->DockNode) igDockContextQueueUndockWindow(igGetCurrentContext(), t);
                else float_out(t, 2);
            }
            return false;
        }
        if (D.frames == 8) {
            ImGuiWindow* t = igFindWindowByName("Dock Test###re_docktest");
            if (t) float_out(t, 2);
        }
        if (ui_viewports_count() < 3 && ++D.tries < 180) return false;
        D.floating = ui_viewports_count();
        if (D.floating < 3) printf("[RealEngine] dock test:   only %d windows of their own\n", D.floating);
        report("three windows of their own out of the editor", D.floating >= 3);
        D.state = D_OWNER, D.frames = 0;
        return false;
    case D_OWNER: {
        if (D.frames < 6) return false;
        int owned = 0, mode = 0, n = ui_viewports_count();
        for (int i = 0; i < n; i++) {
            owned += ui_viewports_owner(i) == ui_viewports_main_hwnd();
            mode += ui_viewports_present_mode(i) == ui_viewports_wanted_present();
        }
        if (owned != n || mode != n) printf("[RealEngine] dock test:   %d of %d owned by the editor, %d in the present mode asked\n", owned, n, mode);
        report("each owned by the editor's window (GW_OWNER)", n > 0 && owned == n);
        report("each presented in the mode asked (mailbox)", n > 0 && mode == n);
        D.state = D_Z, D.frames = 0;
        return false;
    }
    case D_Z: { // the editor's window raised as a click raises it (no activation: the user's focus untouched)
        HWND main = (HWND)ui_viewports_main_hwnd();
        if (D.frames == 1) SetWindowPos(main, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (D.frames < 4) return false;
        int n = ui_viewports_count(), above = 0;
        for (int i = 0; i < n; i++) above += above_main((HWND)ui_viewports_hwnd(i));
        if (above != n) printf("[RealEngine] dock test:   %d of %d still above the editor's window\n", above, n);
        report("the editor's window raised: they stay above it", n > 0 && above == n);
        D.state = D_MIN, D.frames = 0;
        return false;
    }
    case D_MIN: { // (2026-11-25, bug-fixer) the editor minimized: they go with it; restored: they come back. In one frame
        // (minimized, the editor's loop waits in glfwWaitEvents and no frame runs); neither activates (the user's focus)
        HWND main = (HWND)ui_viewports_main_hwnd();
        int n = ui_viewports_count();
        if (D.frames == 1) {
            bool zoomed = IsZoomed(main) != 0;
            ShowWindow(main, SW_SHOWMINNOACTIVE);
            int gone = 0;
            for (int i = 0; i < n; i++) {
                HWND h = (HWND)ui_viewports_hwnd(i);
                gone += h && (!IsWindowVisible(h) || IsIconic(h));
            }
            bool iconic = IsIconic(main) != 0;
            ShowWindow(main, zoomed ? SW_RESTORE : SW_SHOWNOACTIVATE); // (back as it was: maximized again, else unactivated)
            if (!iconic || gone != n) printf("[RealEngine] dock test:   the editor %s, %d of %d windows of their own hidden with it\n", iconic ? "minimized" : "not minimized", gone, n);
            report("the editor minimized: its windows of their own go with it", n > 0 && iconic && gone == n);
            return false;
        }
        if (D.frames < 6) return false;
        int back = 0;
        for (int i = 0; i < n; i++) {
            HWND h = (HWND)ui_viewports_hwnd(i);
            back += h && IsWindowVisible(h) && !IsIconic(h);
        }
        if (back != n || IsIconic(main)) printf("[RealEngine] dock test:   restored: %d of %d windows of their own back\n", back, n);
        report("the editor restored: they come back", n > 0 && back == n && !IsIconic(main));
        D.state = D_FPS, D.frames = 0, D.dt_sum = 0, D.dt_n = 0;
        return false;
    }
    case D_FPS:
        if (D.frames > 30) measure();
        if (D.dt_n < 240) return false;
        D.fps = fps_of();
        printf("[RealEngine] dock test: frame rate %.0f fps with no window of its own, %.0f fps with %d (%.2f -> %.2f ms)\n", D.fps0, D.fps,
               D.floating, D.fps0 > 0 ? 1000.0f / D.fps0 : 0.0f, D.fps > 0 ? 1000.0f / D.fps : 0.0f);
        fflush(stdout);
        D.state = D_SWITCH, D.frames = 0;
        return false;
    case D_SWITCH: { // another project: the documents and their windows go
        *test_window = false;
        char tmp[MAX_PATH] = { 0 };
        GetTempPathA(sizeof tmp, tmp);
        char dir[700];
        snprintf(dir, sizeof dir, "%sRivoraDockTest\\DockSwitch\\Content", tmp);
        re_make_dirs(dir);
        snprintf(D.proj, sizeof D.proj, "%sRivoraDockTest\\DockSwitch\\DockSwitch.rproject", tmp);
        if (!project_save_copy(D.proj)) {
            report("project switch: no window left, no validation error", false);
            D.state = D_END;
            return false;
        }
        D.errors = g_vk_validation_errors; // (the errors before: the 2nd monitor's, main.c's REALENGINE_MONITOR note)
        editor_open_project_later(ed, D.proj, -1);
        D.state = D_SWITCHED, D.frames = 0;
        return false;
    }
    case D_SWITCHED: {
        *test_window = false;
        if (D.frames < 150) return false; // (the windows' fade-out)
        char a[700], b[700];
        snprintf(a, sizeof a, "%s", project_file()), snprintf(b, sizeof b, "%s", D.proj);
        for (char* c = a; *c; c++)
            if (*c == '/') *c = '\\';
        for (char* c = b; *c; c++)
            if (*c == '/') *c = '\\';
        bool switched = !_stricmp(a, b);
        int docs = editor_docs_open(), wins = ui_viewports_count();
        bool ok2 = switched && docs == 0 && wins == 0 && edhost_docs() == 0 && g_vk_validation_errors == D.errors;
        if (!ok2)
            printf("[RealEngine] dock test:   switched %d, %d documents, %d windows of their own, %d in the Editors window, %d validation errors\n", switched, docs,
                   wins, edhost_docs(), g_vk_validation_errors - D.errors);
        report("project switch: no window left, no validation error", ok2);
        D.state = D_REOPEN, D.frames = 0;
        return false;
    }
    case D_REOPEN: { // a material of the new project, out of the editor with the test window: the editor closes on them
        char path[900];
        snprintf(path, sizeof path, "%s\\M_DockQuit.rmat", content_root());
        mat_create(path, false, NULL);
        mat_editor_open("/Content/M_DockQuit.rmat");
        *test_window = true;
        D.state = D_REOPENED, D.frames = 0, D.tries = 0;
        return false;
    }
    case D_REOPENED:
        *test_window = true;
        if (D.frames == 20) {
            if (host) float_out(host, 0);
            ImGuiWindow* t = igFindWindowByName("Dock Test###re_docktest");
            if (t && t->DockNode) igDockContextQueueUndockWindow(igGetCurrentContext(), t);
        }
        if (D.frames == 24) {
            ImGuiWindow* t = igFindWindowByName("Dock Test###re_docktest");
            if (t) float_out(t, 2);
        }
        if (D.frames < 28 || (ui_viewports_count() < 2 && ++D.tries < 180)) return false;
        printf("[RealEngine] dock test: the editor closes with %d windows of their own up (%d materials open)\n", ui_viewports_count(), mat_editor_open_count());
        D.state = D_END;
        return false;
    default:
        *ok = D.fails == 0;
        return true;
    }
}

// After ImGui's and GLFW's shutdown: the windows of this thread still there (the windows of their own: none)
static BOOL CALLBACK count_cb(HWND h, LPARAM l) {
    if (IsWindow(h) && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == (HWND)ui_viewports_main_hwnd()) (*(int*)l)++;
    return TRUE;
}
bool dock_test_after_shutdown(void) {
    int n = 0;
    EnumThreadWindows(GetCurrentThreadId(), count_cb, (LPARAM)&n);
    printf(n ? "[RealEngine] FAIL: dock test: %d windows of their own left after the editor closed\n"
             : "[RealEngine] dock test: the editor closed: %d windows of their own left\n", n);
    fflush(stdout);
    return n == 0;
}
