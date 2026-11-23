// The modular windows' UI test (--dock-test, ctest dock_targets, area editor; notas-dev/temas-e-janelas.md). The mouse is
// the test's own (ImGui's input queue): a window is dragged by its title bar onto each target of the level editor's
// layout (a panel's center and its 4 sides, the dock space's 4 outer edges, the gaps between panels, across and down)
// and must end up docked there; then the same window, out of the editor as a window of its own, maximized, is closed
// by its X, by the system (WM_CLOSE: Alt+F4, the taskbar) and by a close meant for it that reached the editor's main
// window: each time the window closes and the engine keeps running.
#include "dock_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "editor.h"
#include "editor_dock.h"
#include "theme.h"

enum { T_CENTER, T_LEFT, T_RIGHT, T_UP, T_DOWN, T_OUT_LEFT, T_OUT_RIGHT, T_OUT_UP, T_OUT_DOWN, T_GAP_X, T_GAP_Y, T_DROPS,
       T_DBL = T_DROPS, T_MAX_CLICK, T_MAX_WMCLOSE, T_MAX_MAINCLOSE, T_COUNT };
static const char* const g_names[T_COUNT] = { "center of Details", "left of Details", "right of Details", "top of Details",
                                              "bottom of Details", "outer left edge", "outer right edge", "outer top edge",
                                              "outer bottom edge", "gap between the viewport and the right column",
                                              "gap between the Outliner and Details",
                                              "double-click on the title: maximized, then back",
                                              "maximized window closed by its X",
                                              "maximized window closed by the system (WM_CLOSE)",
                                              "a close meant for it, reaching the main window" };

static struct {
    bool on, done, ok, viewports;
    int stage, step, wait, fails, tries;
    double until;
    bool open;          // the test window's
    ImVec2 grab, to;    // the drag: from the title bar to the target
    float before[4];    // the target node's rectangle before the drop
    float split[4];     // a gap: the split's rectangle
    ImGuiDockNode* gap_c0;
    GLFWwindow* main_win;
} T;

static const char* WIN = "Dock Test###re_docktest";

void dock_test_start(GLFWwindow* main_window, bool viewports) {
    memset(&T, 0, sizeof T);
    T.on = true, T.open = true, T.main_win = main_window, T.viewports = viewports;
    T.wait = 40; // (the editor's first frames: the layout, the fades)
}
bool dock_test_running(void) { return T.on && !T.done; }
bool dock_test_finished(bool* ok) {
    if (ok) *ok = T.ok;
    return T.done;
}

static ImGuiWindow* test_win(void) { return igFindWindowByName(WIN); }

static ImGuiID g_mouse_viewport; // (the viewport under the test's mouse: a window of its own's, else the editor's)
static void mouse(ImVec2 p, int down) {
    ImGuiIO* io = igGetIO_Nil();
    ImGuiIO_AddMouseSourceEvent(io, ImGuiMouseSource_Mouse);
    ImGuiIO_AddMouseViewportEvent(io, g_mouse_viewport ? g_mouse_viewport : igGetMainViewport()->ID);
    ImGuiIO_AddMousePosEvent(io, p.x, p.y);
    if (down >= 0) ImGuiIO_AddMouseButtonEvent(io, 0, down != 0);
}

static ImGuiDockNode* node_of(DockPanel p) {
    ImGuiWindow* w = igFindWindowByName(dock_panel_id(p));
    return w ? w->DockNode : NULL;
}

// ImGui's target rectangles (the centers): the same formula as editor_dock.c's
static ImVec2 target_point(ImVec2 p0, ImVec2 sz, ImGuiDir dir, bool outer) {
    float fs = igGetFontSize(), smin = fminf(sz.x, sz.y);
    float hs_c = fminf(fs * 1.5f, fmaxf(fs * 0.5f, smin / 8.0f)), hs_w, hs_h, ox, oy;
    if (outer) hs_w = floorf(hs_c * 1.5f), hs_h = floorf(hs_c * 0.8f), ox = floorf(sz.x * 0.5f - hs_h), oy = floorf(sz.y * 0.5f - hs_h);
    else hs_w = floorf(hs_c), hs_h = floorf(hs_c * 0.9f), ox = oy = floorf(hs_w * 2.4f);
    ImVec2 c = { floorf(p0.x + sz.x * 0.5f), floorf(p0.y + sz.y * 0.5f) };
    switch (dir) {
    case ImGuiDir_Up: return (ImVec2){ c.x, c.y - oy };
    case ImGuiDir_Down: return (ImVec2){ c.x, c.y + oy };
    case ImGuiDir_Left: return (ImVec2){ c.x - ox, c.y };
    case ImGuiDir_Right: return (ImVec2){ c.x + ox, c.y };
    default: return c;
    }
}

// The stage's target point (from the layout as it is now)
static bool plan(int s) {
    ImGuiDockNode* det = node_of(DP_DETAILS);
    ImGuiDockNode* root = igDockBuilderGetNode(dock_space_id());
    if (!det || !root) return false;
    memcpy(T.before, (float[4]){ det->Pos.x, det->Pos.y, det->Size.x, det->Size.y }, sizeof T.before);
    static const ImGuiDir dirs[5] = { ImGuiDir_None, ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down };
    if (s <= T_DOWN) T.to = target_point(det->Pos, det->Size, dirs[s], false);
    else if (s <= T_OUT_DOWN) {
        memcpy(T.before, (float[4]){ root->Pos.x, root->Pos.y, root->Size.x, root->Size.y }, sizeof T.before);
        T.to = target_point(root->Pos, root->Size, dirs[s - T_OUT_LEFT + 1], true);
    } else { // a gap: the split above the right column (X) or the one between the Outliner and Details (Y)
        ImGuiDockNode* n = s == T_GAP_X ? det->ParentNode ? det->ParentNode->ParentNode : NULL : det->ParentNode;
        if (!n || !n->ChildNodes[0] || !n->ChildNodes[1]) return false;
        ImGuiAxis want = s == T_GAP_X ? ImGuiAxis_X : ImGuiAxis_Y;
        while (n && n->SplitAxis != want) n = n->ParentNode;
        if (!n) return false;
        ImGuiDockNode *c0 = n->ChildNodes[0], *c1 = n->ChildNodes[1];
        T.gap_c0 = c0;
        memcpy(T.split, (float[4]){ n->Pos.x, n->Pos.y, n->Size.x, n->Size.y }, sizeof T.split);
        if (want == ImGuiAxis_X) T.to = (ImVec2){ (c0->Pos.x + c0->Size.x + c1->Pos.x) * 0.5f, n->Pos.y + n->Size.y * 0.5f };
        else T.to = (ImVec2){ n->Pos.x + n->Size.x * 0.5f, (c0->Pos.y + c0->Size.y + c1->Pos.y) * 0.5f };
    }
    return true;
}

static bool close_to(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static bool check(int s) {
    ImGuiWindow* w = test_win();
    ImGuiDockNode* n = w ? w->DockNode : NULL;
    if (!n) return false;
    ImGuiDockNode* det = node_of(DP_DETAILS);
    float x = n->Pos.x, y = n->Pos.y, ww = n->Size.x, hh = n->Size.y, tol = 12.0f;
    const float* b = T.before;
    switch (s) {
    case T_CENTER: return det == n; // a tab of Details
    case T_LEFT: return close_to(x, b[0], tol) && ww < b[2] && close_to(hh, b[3], tol);
    case T_RIGHT: return close_to(x + ww, b[0] + b[2], tol) && ww < b[2] && close_to(hh, b[3], tol);
    case T_UP: return close_to(y, b[1], tol) && hh < b[3] && close_to(ww, b[2], tol);
    case T_DOWN: return close_to(y + hh, b[1] + b[3], tol) && hh < b[3] && close_to(ww, b[2], tol);
    case T_OUT_LEFT: return close_to(x, b[0], tol) && close_to(hh, b[3], tol);
    case T_OUT_RIGHT: return close_to(x + ww, b[0] + b[2], tol) && close_to(hh, b[3], tol);
    case T_OUT_UP: return close_to(y, b[1], tol) && close_to(ww, b[2], tol);
    case T_OUT_DOWN: return close_to(y + hh, b[1] + b[3], tol) && close_to(ww, b[2], tol);
    case T_GAP_X: return x > T.split[0] + 20 && x + ww < T.split[0] + T.split[2] + 1 && close_to(hh, T.split[3], tol) && det && det->Pos.x > x;
    case T_GAP_Y: return y > T.split[1] + 20 && close_to(ww, T.split[2], tol) && det && det->Pos.y > y;
    default: return false;
    }
}

static void report(int s, bool ok) {
    printf("[RealEngine] dock test: %-48s %s\n", g_names[s], ok ? "ok" : "FAIL");
    if (!ok) T.fails++;
    fflush(stdout);
}

// Before the frame (ImGui's input queue): the drag's steps, the closes
void dock_test_input(Editor* ed) {
    if (!dock_test_running()) return;
    if (T.wait > 0) {
        T.wait--;
        return;
    }
    if (igGetTime() < T.until) return; // (a wait in seconds: a window's fade-out, whatever the frame rate)
    ImGuiWindow* w = test_win();
    if (T.stage < T_DROPS) {
        switch (T.step) {
        case 0: // the layout back to its default, the test window floating over the viewport
            dock_layout_default(ed);
            T.open = true;
            T.step = 1, T.wait = 4;
            return;
        case 1: {
            if (!w) return;
            if (w->DockNode) { // (docked by the last stage: out first, placed after)
                igDockContextQueueUndockWindow(igGetCurrentContext(), w);
                T.wait = 3;
                return;
            }
            float vr[4];
            if (!dock_panel_rect(DP_VIEWPORT, vr)) {
                if (++T.tries == 120) printf("[RealEngine] dock test: no viewport node\n"), fflush(stdout);
                return;
            }
            igSetWindowPos_WindowPtr(w, (ImVec2){ vr[0] + 60, vr[1] + 80 }, ImGuiCond_Always);
            igSetWindowSize_WindowPtr(w, (ImVec2){ 320, 200 }, ImGuiCond_Always);
            T.step = 2, T.wait = 3;
            return;
        }
        case 2:
            if (!w || w->DockNode) {
                if (++T.tries == 120) printf("[RealEngine] dock test: stage %d waits for the window to float (%s)\n", T.stage, w ? "docked" : "no window"), fflush(stdout);
                return;
            }
            T.tries = 0;
            if (!plan(T.stage)) {
                report(T.stage, false);
                T.stage++, T.step = 0;
                return;
            }
            T.grab = (ImVec2){ w->Pos.x + 50, w->Pos.y + igGetFrameHeight() * 0.5f };
            if (getenv("REALENGINE_DOCK_DEBUG")) printf("[RealEngine] dock test debug: stage %d grab %.0f,%.0f (window %.0f,%.0f %.0fx%.0f, hovered %s)\n", T.stage, T.grab.x, T.grab.y, w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, igGetCurrentContext()->HoveredWindow ? igGetCurrentContext()->HoveredWindow->Name : "-"), fflush(stdout);
            mouse(T.grab, 0);
            T.step = 3;
            return;
        case 3:
            mouse(T.grab, 1);
            T.step = 4;
            return;
        default: { // 4..13: moving; 14..16: held over the target; 17: let go; 22: checked
            int k = T.step - 4;
            if (k < 10) {
                float t = (float)(k + 1) / 10.0f;
                mouse((ImVec2){ T.grab.x + (T.to.x - T.grab.x) * t, T.grab.y + (T.to.y - T.grab.y) * t }, -1);
            } else if (k < 13) {
                mouse(T.to, -1);
                if (k == 12 && getenv("REALENGINE_DOCK_DEBUG")) {
                    ImGuiContext* g = igGetCurrentContext();
                    printf("[RealEngine] dock test debug: stage %d moving %s, window at %.0f,%.0f, mouse %.0f,%.0f\n", T.stage,
                           g->MovingWindow ? g->MovingWindow->Name : "(none)", w ? w->Pos.x : 0, w ? w->Pos.y : 0, igGetIO_Nil()->MousePos.x, igGetIO_Nil()->MousePos.y);
                    fflush(stdout);
                }
            } else if (k == 13) {
                mouse(T.to, 0);
            } else if (k == 18) {
                bool ok = check(T.stage);
                if (!ok && w) {
                    ImGuiDockNode* n = w->DockNode;
                    printf("[RealEngine] dock test: the window at %s (target %.0f,%.0f; before %.0f,%.0f %.0fx%.0f)\n", n ? "a node" : "no node",
                           T.to.x, T.to.y, T.before[0], T.before[1], T.before[2], T.before[3]);
                    if (n) printf("[RealEngine] dock test:   its node %.0f,%.0f %.0fx%.0f\n", n->Pos.x, n->Pos.y, n->Size.x, n->Size.y);
                }
                report(T.stage, ok);
                T.stage++, T.step = 0;
                return;
            }
            T.step++;
            return;
        }
        }
    }
    // (19/11) the double-click on the title bar: the window maximized over the editor, another one puts it back
    if (T.stage == T_DBL) {
        switch (T.step) {
        case 0: dock_layout_default(ed), T.open = true, T.step = 1, T.wait = 4; return;
        case 1: {
            if (!w) return;
            if (w->DockNode) {
                igDockContextQueueUndockWindow(igGetCurrentContext(), w);
                T.wait = 3;
                return;
            }
            float vr[4];
            if (!dock_panel_rect(DP_VIEWPORT, vr)) return;
            igSetWindowPos_WindowPtr(w, (ImVec2){ vr[0] + 60, vr[1] + 80 }, ImGuiCond_Always);
            igSetWindowSize_WindowPtr(w, (ImVec2){ 320, 200 }, ImGuiCond_Always);
            T.step = 2, T.wait = 3;
            return;
        }
        case 2: case 12: // the title bar, two clicks
            if (!w) return;
            if (T.step == 2) memcpy(T.before, (float[4]){ w->Pos.x, w->Pos.y, w->Size.x, w->Size.y }, sizeof T.before);
            T.grab = (ImVec2){ w->Pos.x + 50, w->Pos.y + igGetFrameHeight() * 0.5f };
            mouse(T.grab, 0), T.step++;
            return;
        case 3: case 5: case 13: case 15: mouse(T.grab, 1), T.step++; return;
        case 4: case 14: mouse(T.grab, 0), T.step++; return;
        case 6: case 16: mouse(T.grab, 0), T.step++, T.wait = 4; return;
        case 7: { // maximized: much bigger, its old place kept
            bool ok = w && dock_window_zoomed(WIN) && w->Size.x > T.before[2] * 2.0f && w->Size.y > T.before[3] * 2.0f;
            if (!ok) printf("[RealEngine] dock test:   after the double-click %.0fx%.0f (was %.0fx%.0f), zoomed %d\n", w ? w->Size.x : 0, w ? w->Size.y : 0,
                            T.before[2], T.before[3], (int)dock_window_zoomed(WIN));
            T.tries = !ok; // (the first half's result: reported with the second)
            T.step = 12;
            return;
        }
        case 17: { // back where it was
            bool ok = w && !dock_window_zoomed(WIN) && close_to(w->Pos.x, T.before[0], 2) && close_to(w->Pos.y, T.before[1], 2) &&
                      close_to(w->Size.x, T.before[2], 2) && close_to(w->Size.y, T.before[3], 2);
            if (!ok && w) printf("[RealEngine] dock test:   after the second double-click %.0f,%.0f %.0fx%.0f (was %.0f,%.0f %.0fx%.0f)\n", w->Pos.x, w->Pos.y,
                                 w->Size.x, w->Size.y, T.before[0], T.before[1], T.before[2], T.before[3]);
            report(T_DBL, ok && !T.tries);
            T.stage++, T.step = 0, T.wait = 3, T.tries = 0;
            return;
        }
        default: T.step++; return;
        }
    }
    // the closes: the window out of the editor (a window of its own), maximized, closed three ways
    if (T.stage >= T_COUNT) {
        T.done = true;
        T.ok = T.fails == 0;
        printf(T.ok ? "[RealEngine] Dock test OK: %d targets, the maximized window closed 3 ways, the engine alive\n"
                    : "[RealEngine] FAIL: dock test, %d failure(s)\n", T.ok ? T_DROPS : T.fails);
        dock_layout_default(ed);
        return;
    }
    if (!T.viewports) { // (multi-viewport off: no window of its own to close)
        printf("[RealEngine] dock test: %-48s skipped (no multi-viewport)\n", g_names[T.stage]);
        T.stage++;
        return;
    }
    switch (T.step) {
    case 0:
        T.open = true;
        T.step = 1, T.wait = 3;
        return;
    case 1: { // out of the editor: to the left of the main window (offscreen like it when running in the background)
        if (!w) return;
        if (w->DockNode) {
            igDockContextQueueUndockWindow(igGetCurrentContext(), w);
            T.wait = 2;
            return;
        }
        int mx, my;
        glfwGetWindowPos(T.main_win, &mx, &my);
        if (getenv("REALENGINE_BACKGROUND")) // (offscreen, left of the main window, offscreen too)
            igSetWindowPos_WindowPtr(w, (ImVec2){ (float)mx - 700.0f, (float)my + 100.0f }, ImGuiCond_Always);
        else { // (on the screen: over the main window's top edge, out of its client area: a window of its own)
            const ImGuiViewport* mv = igGetMainViewport();
            igSetWindowPos_WindowPtr(w, (ImVec2){ mv->Pos.x + 300.0f, mv->Pos.y - 20.0f }, ImGuiCond_Always);
        }
        igSetWindowSize_WindowPtr(w, (ImVec2){ 500, 300 }, ImGuiCond_Always);
        T.step = 2, T.wait = 4;
        return;
    }
    case 2: { // its own OS window: maximized (in the background: as big as a screen, offscreen)
        if (!w || !w->Viewport || w->Viewport == (ImGuiViewportP*)igGetMainViewport() || !w->Viewport->_ImGuiViewport.PlatformHandle) {
            if (++T.tries < 60) return; // (wait for it)
            printf("[RealEngine] dock test:   the window never got a window of its own\n");
            report(T.stage, false), T.stage++, T.step = 0, T.tries = 0;
            return;
        }
        GLFWwindow* gw = (GLFWwindow*)w->Viewport->_ImGuiViewport.PlatformHandle;
        if (T.stage == T_MAX_CLICK && glfwGetWindowAttrib(gw, GLFW_MAXIMIZED)) { // (the report's cause: born maximized)
            printf("[RealEngine] dock test:   a window of its own was born maximized (the main window's GLFW hint)\n");
            T.fails++;
        }
        if (getenv("REALENGINE_BACKGROUND")) {
            int mx, my, sw = 1920, sh = 1080;
            glfwGetWindowPos(T.main_win, &mx, &my);
            const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
            if (vm) sw = vm->width, sh = vm->height;
            igSetWindowPos_WindowPtr(w, (ImVec2){ (float)mx - (float)sw - 50.0f, (float)my }, ImGuiCond_Always);
            igSetWindowSize_WindowPtr(w, (ImVec2){ (float)sw, (float)sh - 40.0f }, ImGuiCond_Always);
        } else {
            glfwMaximizeWindow(gw);
        }
        T.step = 3, T.wait = 6, T.tries = 0;
        return;
    }
    case 3: { // the close
        if (!w || !w->Viewport) return;
        GLFWwindow* gw = (GLFWwindow*)w->Viewport->_ImGuiViewport.PlatformHandle;
        if (getenv("REALENGINE_DOCK_DEBUG")) {
            int gx = 0, gy = 0, gw2 = 0, gh = 0;
            if (gw) glfwGetWindowPos(gw, &gx, &gy), glfwGetWindowSize(gw, &gw2, &gh);
            ImGuiViewport* v = &w->Viewport->_ImGuiViewport;
            printf("[RealEngine] dock test debug: window %.0f,%.0f %.0fx%.0f; viewport %.0f,%.0f %.0fx%.0f; OS %d,%d %dx%d maximized %d\n", w->Pos.x, w->Pos.y,
                   w->Size.x, w->Size.y, v->Pos.x, v->Pos.y, v->Size.x, v->Size.y, gx, gy, gw2, gh, gw ? glfwGetWindowAttrib(gw, GLFW_MAXIMIZED) : -1);
            fflush(stdout);
        }
        if (!getenv("REALENGINE_BACKGROUND") && T.stage == T_MAX_CLICK && w->Size.x < 900.0f) { // (on the screen: maximized for real)
            printf("[RealEngine] dock test:   maximized, but the window kept %.0fx%.0f: the X in the corner is the main window's\n", w->Size.x, w->Size.y);
            T.fails++;
        }
        if (T.stage == T_MAX_CLICK) { // its X (the title bar's close button, top right)
            const ImGuiStyle* st = igGetStyle();
            float fs = igGetFontSize();
            ImVec2 x = { w->Pos.x + w->Size.x - st->FramePadding.x - fs * 0.5f, w->Pos.y + st->FramePadding.y + fs * 0.5f };
            g_mouse_viewport = w->Viewport->_ImGuiViewport.ID;
            mouse(x, 0);
            T.grab = x;
            T.step = 10;
            return;
        }
#ifdef _WIN32
        HWND hw = gw ? glfwGetWin32Window(gw) : NULL;
        if (T.stage == T_MAX_WMCLOSE && hw) PostMessageA(hw, WM_CLOSE, 0, 0);
        if (T.stage == T_MAX_MAINCLOSE) { // a close for it that reached the main window (it was in front: Alt+F4 on it)
            dock_route_test_foreground(hw); // (as if it were in front: the test never steals the user's focus)
            PostMessageA(glfwGetWin32Window(T.main_win), WM_CLOSE, 0, 0);
        }
#else
        (void)gw;
#endif
        T.step = 20, T.until = igGetTime() + 0.6;
        return;
    }
    case 10: mouse(T.grab, 1), T.step = 11; return;
    case 11: mouse(T.grab, 0), g_mouse_viewport = 0, T.step = 20, T.until = igGetTime() + 0.6; return; // (the window fades out, then closes)
    case 20: { // closed, the engine running
        bool closed = !T.open;
        bool alive = !glfwWindowShouldClose(T.main_win) && !ed->quit;
        if (!closed || !alive) printf("[RealEngine] dock test:   window %s, main window %s\n", closed ? "closed" : "still open", alive ? "alive" : "closing");
        report(T.stage, closed && alive);
        glfwSetWindowShouldClose(T.main_win, GLFW_FALSE);
        dock_route_test_foreground(NULL);
        T.stage++, T.step = 0, T.wait = 4;
        return;
    }
    default: return;
    }
}

// In the frame: the test window (a plain dockable window)
void dock_test_frame(Editor* ed) {
    (void)ed;
    if (!T.on || T.done) return;
    if (!T.open) return;
    igSetNextWindowSize((ImVec2){ 320, 200 }, ImGuiCond_FirstUseEver);
    if (igBegin(WIN, &T.open, ImGuiWindowFlags_NoCollapse)) igTextUnformatted("Drag me", NULL);
    igEnd();
}
