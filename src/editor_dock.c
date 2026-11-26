// The level editor's modular windows (editor_dock.h, Q2, notas-dev/temas-e-janelas.md). ImGui's docking does the
// docking itself; this file builds the default layout (Place Actors on the left, the viewport in the middle, Outliner
// with Levels beside it and Details on the right, the docked Content Browser under the viewport), shows every target
// while a window is dragged (ImGui shows only the hovered panel's) plus the gaps between panels as targets of their own,
// puts a panel that opens without a place back beside the viewport, and keeps the layouts (.rlayout: the panels shown
// and ImGui's docking, as text).
#include "editor_dock.h"
#include "editor_host.h" // (item 9)

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "editor_look.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "theme.h"
#include "ui_icons.h"

extern bool g_editor_prefs_readonly; // (an automated run: never the user's layout, never written)

static const char* const g_ids[DP_COUNT] = { "###re_viewport", "###re_place", "###re_outliner", "###re_details", "###Content Browser", "###re_levels",
                                             "###re_modes", "###output_log", "###re_platforms" };
const char* dock_panel_id(DockPanel p) { return p < DP_COUNT ? g_ids[p] : ""; }

static ImGuiID g_dock;           // the dock space (a fixed ID: the layouts' files name it)
static bool g_build_default = true, g_restored;
static char* g_pending;          // a layout's text, applied before the next frame
static ImVec2 g_ds_pos, g_ds_size;
static bool g_seen[DP_COUNT], g_redock[DP_COUNT];
static char g_last_project[700];
// a drag: the window, and the gap target under the mouse (applied when it is let go)
static ImGuiWindow* g_drag_win;
static ImGuiDockNode* g_gap_node;
static ImGuiDir g_gap_dir;
static int g_front_outliner;

unsigned dock_space_id(void) { return g_dock; }

static ImGuiID dock_id(void) {
    if (!g_dock) g_dock = igImHashStr("RealEngine.LevelEditor.Dock", 0, 0);
    return g_dock;
}

// (Q26) The default of 0.44, the user's mockups: Modes and Place Actors on the left, the viewport in the middle, the
// Outliner (Levels beside it) over Details on the right; under them all a row with the Content Browser (the Output Log
// beside it) and Platform Support at the right end
static void build_default(void) {
    ImGuiID id = dock_id();
    igDockBuilderRemoveNode(id);
    igDockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodePos(id, g_ds_pos);
    igDockBuilderSetNodeSize(id, g_ds_size);
    ImGuiID top, bottom, bleft, bright, left, rest, right, center, rtop, rbot;
    igDockBuilderSplitNode(id, ImGuiDir_Down, 0.33f, &bottom, &top); // (the central node stays in the top part)
    igDockBuilderSplitNode(bottom, ImGuiDir_Right, 0.155f, &bright, &bleft);
    igDockBuilderSplitNode(top, ImGuiDir_Left, 0.145f, &left, &rest);
    igDockBuilderSplitNode(rest, ImGuiDir_Right, 0.235f, &right, &center);
    igDockBuilderSplitNode(right, ImGuiDir_Up, 0.6f, &rtop, &rbot);
    igDockBuilderDockWindow(g_ids[DP_MODES], left);
    igDockBuilderDockWindow(g_ids[DP_PLACE], left);
    igDockBuilderDockWindow(g_ids[DP_LEVELS], rtop); // (Levels beside the Outliner: a tab of the same panel)
    igDockBuilderDockWindow(g_ids[DP_OUTLINER], rtop);
    igDockBuilderDockWindow(g_ids[DP_DETAILS], rbot);
    igDockBuilderDockWindow(g_ids[DP_CONTENT], bleft);
    igDockBuilderDockWindow(g_ids[DP_LOG], bleft);
    igDockBuilderDockWindow(g_ids[DP_PLATFORMS], bright);
    (void)center; // (the viewport: over the central node, kept empty: ImGui leaves a hole there and the scene shows)
    igDockBuilderFinish(id);
    g_front_outliner = 8; // (the Outliner's, Place Actors' and the Content Browser's tabs in front once their tab bars exist)
    g_build_default = false;
}

void dock_host(Editor* ed, float x, float y, float w, float h) {
    (void)ed;
    g_ds_pos = (ImVec2){ x, y }, g_ds_size = (ImVec2){ w, h };
    ImGuiID id = dock_id();
    ImGuiDockNode* n = igDockBuilderGetNode(id);
    if (g_build_default || !n) build_default();
    igSetCursorScreenPos(g_ds_pos);
    igDockSpace(id, g_ds_size, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode, NULL);
}

bool dock_panel_begin(DockPanel p, const char* title, bool* open, int extra_flags) {
    char name[160];
    snprintf(name, sizeof name, "%s%s", title, g_ids[p]);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | extra_flags;
    if (p == DP_VIEWPORT) { // the viewport: over the dock space's central node (kept empty: the scene shows through the
        // hole ImGui leaves there, and nothing docks over it); the panels around it move it and size it
        ImGuiDockNode* c = igDockBuilderGetCentralNode(dock_id());
        ImVec2 pos = c ? c->Pos : g_ds_pos, size = c ? c->Size : g_ds_size;
        igSetNextWindowPos(pos, ImGuiCond_Always, (ImVec2){ 0, 0 });
        igSetNextWindowSize((ImVec2){ fmaxf(size.x, 1.0f), fmaxf(size.y, 1.0f) }, ImGuiCond_Always);
        igSetNextWindowViewport(igGetMainViewport()->ID);
        fl |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0, 0 });
        igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
        igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
        bool shown = igBegin(name, NULL, fl);
        igPopStyleVar(3);
        return shown;
    } else {
        igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ TMV(TM_SPACE_PAD), TMV(TM_SPACE_PAD) });
        igSetNextWindowSize((ImVec2){ igGetFontSize() * 22, igGetFontSize() * 26 }, ImGuiCond_FirstUseEver);
    }
    bool shown = igBegin(name, open, fl);
    igPopStyleVar(1);
    ImGuiWindow* win = igGetCurrentWindow();
    if (p != DP_VIEWPORT && win->Appearing && !win->DockNode && (win->DockId != 0 || !g_seen[p])) g_redock[p] = true;
    g_seen[p] = true;
    return shown;
}

void dock_panel_end(void) { igEnd(); }

// --- Targets while a window is dragged --------------------------------------------------------------------------------
// ImGui's own rectangles for a node's 5 targets (DockNodeCalcDropRectsAndTestMousePos, imgui.cpp): the same places
static void drop_rect(ImVec2 p0, ImVec2 p1, ImGuiDir dir, bool outer, ImVec2* a, ImVec2* b) {
    float fs = igGetFontSize(), w = p1.x - p0.x, h = p1.y - p0.y, smin = fminf(w, h);
    float hs_c = fminf(fs * 1.5f, fmaxf(fs * 0.5f, smin / 8.0f)), hs_w, hs_h, ox, oy;
    if (outer) hs_w = floorf(hs_c * 1.5f), hs_h = floorf(hs_c * 0.8f), ox = floorf(w * 0.5f - hs_h), oy = floorf(h * 0.5f - hs_h);
    else hs_w = floorf(hs_c), hs_h = floorf(hs_c * 0.9f), ox = oy = floorf(hs_w * 2.4f);
    ImVec2 c = { floorf((p0.x + p1.x) * 0.5f), floorf((p0.y + p1.y) * 0.5f) };
    switch (dir) {
    case ImGuiDir_Up: *a = (ImVec2){ c.x - hs_w, c.y - oy - hs_h }, *b = (ImVec2){ c.x + hs_w, c.y - oy + hs_h }; break;
    case ImGuiDir_Down: *a = (ImVec2){ c.x - hs_w, c.y + oy - hs_h }, *b = (ImVec2){ c.x + hs_w, c.y + oy + hs_h }; break;
    case ImGuiDir_Left: *a = (ImVec2){ c.x - ox - hs_h, c.y - hs_w }, *b = (ImVec2){ c.x - ox + hs_h, c.y + hs_w }; break;
    case ImGuiDir_Right: *a = (ImVec2){ c.x + ox - hs_h, c.y - hs_w }, *b = (ImVec2){ c.x + ox + hs_h, c.y + hs_w }; break;
    default: *a = (ImVec2){ c.x - hs_w, c.y - hs_w }, *b = (ImVec2){ c.x + hs_w, c.y + hs_w }; break;
    }
}

static void target(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hot) {
    float r = TMV(TM_RADIUS_FRAME);
    ImDrawList_AddRectFilled(dl, a, b, THA(TK_DOCK_TARGET, hot ? 200 : 70), r, 0);
    ImDrawList_AddRect(dl, a, b, THA(TK_DOCK_TARGET, hot ? 255 : 150), r, 1.5f, 0);
}

static bool in_rect(ImVec2 m, ImVec2 a, ImVec2 b) { return m.x >= a.x && m.y >= a.y && m.x <= b.x && m.y <= b.y; }

// The gap between a split's two children: a bar on it; dropped there, the window gets a node of its own between them
// (the second child split on its outer side: as tall or wide as the whole split)
static void gap_targets(ImDrawList* dl, ImGuiDockNode* n, ImVec2 m) {
    if (!n || !n->ChildNodes[0] || !n->ChildNodes[1]) return;
    ImGuiDockNode *c0 = n->ChildNodes[0], *c1 = n->ChildNodes[1];
    if (c0->IsVisible && c1->IsVisible) {
        float fs = igGetFontSize(), len = fs * 2.2f, th = fs * 0.45f;
        ImVec2 a, b;
        ImGuiDir dir;
        if (n->SplitAxis == ImGuiAxis_X) {
            float x = (c0->Pos.x + c0->Size.x + c1->Pos.x) * 0.5f, y = n->Pos.y + n->Size.y * 0.5f;
            a = (ImVec2){ x - th, y - len }, b = (ImVec2){ x + th, y + len }, dir = ImGuiDir_Left;
        } else {
            float y = (c0->Pos.y + c0->Size.y + c1->Pos.y) * 0.5f, x = n->Pos.x + n->Size.x * 0.5f;
            a = (ImVec2){ x - len, y - th }, b = (ImVec2){ x + len, y + th }, dir = ImGuiDir_Up;
        }
        ImVec2 ea = { a.x - 4, a.y - 4 }, eb = { b.x + 4, b.y + 4 };
        bool hot = in_rect(m, ea, eb);
        if (hot) {
            g_gap_node = c1, g_gap_dir = dir;
            // the preview: where the new node goes
            ImVec2 pa = c1->Pos, pb = { c1->Pos.x + c1->Size.x, c1->Pos.y + c1->Size.y };
            if (dir == ImGuiDir_Left) pb.x = pa.x + c1->Size.x * 0.3f;
            else pb.y = pa.y + c1->Size.y * 0.3f;
            ImDrawList_AddRectFilled(dl, pa, pb, THA(TK_DOCK_PREVIEW, 70), 0, 0);
        }
        target(dl, a, b, hot);
    }
    gap_targets(dl, c0, m);
    gap_targets(dl, c1, m);
}

static void node_targets(ImDrawList* dl, ImGuiDockNode* n, ImGuiDockNode* hovered) {
    if (!n) return;
    if (n->ChildNodes[0] || n->ChildNodes[1]) {
        node_targets(dl, n->ChildNodes[0], hovered);
        node_targets(dl, n->ChildNodes[1], hovered);
        return;
    }
    if (!n->IsVisible || n == hovered) return; // (the hovered one: ImGui draws its own, the live ones)
    ImVec2 p0 = n->Pos, p1 = { n->Pos.x + n->Size.x, n->Pos.y + n->Size.y };
    bool center_ok = !(n->MergedFlags & ImGuiDockNodeFlags_NoDockingOverMe);
    for (int d = -1; d < 4; d++) {
        if (d == ImGuiDir_None && !center_ok) continue;
        ImVec2 a, b;
        drop_rect(p0, p1, (ImGuiDir)d, false, &a, &b);
        target(dl, a, b, false);
    }
}

// A window of its own (dragged out of the editor) maximized by the system (Win+Up, the taskbar's menu): ImGui kept its
// old size, so the X at the screen's corner was the main window's and closed the engine (the user's report, 17/11).
// Maximized, it fills its monitor's work area: its own X is the one in the corner.
// (only a window the system maximized after it was seen not maximized, and not while it is dragged: GLFW reports some
// new windows of the viewports as maximized from the start)
static struct { ImGuiID id; bool seen_normal, maximized; } g_max[16];
static void dock_maximized_windows(void) {
    ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
    ImGuiContext* g = igGetCurrentContext();
    for (int i = 0; i < pio->Viewports.Size; i++) {
        ImGuiViewportP* v = (ImGuiViewportP*)pio->Viewports.Data[i];
        if (&v->_ImGuiViewport == igGetMainViewport() || !v->_ImGuiViewport.PlatformHandle || !v->Window) continue;
        GLFWwindow* gw = (GLFWwindow*)v->_ImGuiViewport.PlatformHandle;
        int slot = -1, free_slot = -1;
        for (int k = 0; k < 16; k++) {
            if (g_max[k].id == v->_ImGuiViewport.ID) slot = k;
            if (!g_max[k].id && free_slot < 0) free_slot = k;
        }
        if (slot < 0) {
            if (free_slot < 0) continue;
            slot = free_slot, g_max[slot].id = v->_ImGuiViewport.ID, g_max[slot].seen_normal = false, g_max[slot].maximized = false;
        }
        bool zoomed = glfwGetWindowAttrib(gw, GLFW_MAXIMIZED) != 0;
        bool moving = g->MovingWindow && g->MovingWindow->RootWindowDockTree == v->Window->RootWindowDockTree;
        if (!zoomed || moving) {
            g_max[slot].seen_normal = !zoomed, g_max[slot].maximized = false;
            continue;
        }
        if (!g_max[slot].seen_normal && !g_max[slot].maximized) continue;
        g_max[slot].maximized = true;
        ImVec2 c = { v->_ImGuiViewport.Pos.x + v->_ImGuiViewport.Size.x * 0.5f, v->_ImGuiViewport.Pos.y + v->_ImGuiViewport.Size.y * 0.5f };
        const ImGuiPlatformMonitor* best = NULL;
        float bd = 3.4e38f;
        for (int m = 0; m < pio->Monitors.Size; m++) { // the monitor it is on (the nearest one's center)
            const ImGuiPlatformMonitor* mon = &pio->Monitors.Data[m];
            float dx = mon->MainPos.x + mon->MainSize.x * 0.5f - c.x, dy = mon->MainPos.y + mon->MainSize.y * 0.5f - c.y, d = dx * dx + dy * dy;
            if (c.x >= mon->MainPos.x && c.y >= mon->MainPos.y && c.x < mon->MainPos.x + mon->MainSize.x && c.y < mon->MainPos.y + mon->MainSize.y) d = -1.0f;
            if (d < bd) bd = d, best = mon;
        }
        if (!best) continue;
        ImGuiWindow* w = v->Window;
        if (fabsf(w->Pos.x - best->WorkPos.x) > 0.5f || fabsf(w->Pos.y - best->WorkPos.y) > 0.5f) igSetWindowPos_WindowPtr(w, best->WorkPos, ImGuiCond_Always);
        if (fabsf(w->Size.x - best->WorkSize.x) > 0.5f || fabsf(w->Size.y - best->WorkSize.y) > 0.5f) igSetWindowSize_WindowPtr(w, best->WorkSize, ImGuiCond_Always);
    }
    for (int k = 0; k < 16; k++) { // (the viewports gone: their slots free)
        bool alive = false;
        for (int i = 0; i < pio->Viewports.Size && g_max[k].id; i++) alive |= pio->Viewports.Data[i]->ID == g_max[k].id;
        if (!alive) g_max[k].id = 0;
    }
}

// A double-click on a floating window's title bar (or a floating node's tab bar) maximizes it, another one puts it back
// (the user's request, 19/11: like a window of the system). Out of the editor (a window of its own): its monitor's work
// area; over the editor: the dock space's area (the menu and the toolbar stay). The editor's own window is the system's
// (its title bar does it already). A window maximized by the system (Win+Up) comes back the same way.
static struct { ImGuiID id; ImVec2 pos, size; } g_zoom[16];
static bool same_rect(ImVec2 p, ImVec2 s, ImVec2 tp, ImVec2 ts) {
    return fabsf(p.x - tp.x) < 2.0f && fabsf(p.y - tp.y) < 2.0f && fabsf(s.x - ts.x) < 2.0f && fabsf(s.y - ts.y) < 2.0f;
}
static void dock_title_double_click(void) {
    ImGuiContext* g = igGetCurrentContext();
    ImGuiIO* io = igGetIO_Nil();
    if (!io->MouseDoubleClicked[0] || !g->HoveredWindow) return;
    ImGuiWindow* w = g->HoveredWindow->RootWindowDockTree;
    if (!w || (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)))
        return;
    if (w->DockNodeAsHost && w->DockNodeAsHost->ID == dock_id()) return; // (the editor itself)
    float th = igGetFrameHeight();
    ImVec2 m = io->MousePos;
    if (m.y < w->Pos.y || m.y > w->Pos.y + th || m.x > w->Pos.x + w->Size.x - th * 1.2f) return; // (the title bar, not its X)
    ImGuiViewportP* v = w->Viewport;
    ImVec2 tp = g_ds_pos, ts = g_ds_size;
    GLFWwindow* gw = NULL;
    if (v && &v->_ImGuiViewport != igGetMainViewport()) { // a window of its own: its monitor's work area
        ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
        int mi = v->PlatformMonitor;
        if (mi < 0 || mi >= pio->Monitors.Size) mi = 0;
        if (pio->Monitors.Size == 0) return;
        tp = pio->Monitors.Data[mi].WorkPos, ts = pio->Monitors.Data[mi].WorkSize;
        gw = (GLFWwindow*)v->_ImGuiViewport.PlatformHandle;
    }
    if (ts.x < 64.0f || ts.y < 64.0f) return;
    int slot = -1, free_slot = -1;
    for (int k = 0; k < 16; k++) {
        if (g_zoom[k].id == w->ID) slot = k;
        if (!g_zoom[k].id && free_slot < 0) free_slot = k;
    }
    bool sys_max = gw && glfwGetWindowAttrib(gw, GLFW_MAXIMIZED);
    ImVec2 np, ns;
    if (same_rect(w->Pos, w->Size, tp, ts) || sys_max) { // maximized: back to where it was (or a window in the middle of it)
        if (slot >= 0) np = g_zoom[slot].pos, ns = g_zoom[slot].size, g_zoom[slot].id = 0;
        else ns = (ImVec2){ ts.x * 0.6f, ts.y * 0.6f }, np = (ImVec2){ tp.x + ts.x * 0.2f, tp.y + ts.y * 0.2f };
        if (sys_max) glfwRestoreWindow(gw);
    } else { // maximized, its place kept
        if (slot < 0) slot = free_slot;
        if (slot < 0) slot = 0;
        g_zoom[slot].id = w->ID, g_zoom[slot].pos = w->Pos, g_zoom[slot].size = w->Size;
        np = tp, ns = ts;
    }
    if (g->MovingWindow == w || (g->MovingWindow && g->MovingWindow->RootWindowDockTree == w)) g->MovingWindow = NULL; // (the second click's drag would pull it back under the mouse)
    igClearActiveID();
    igSetWindowCollapsed_WindowPtr(w, false, ImGuiCond_Always);
    igSetWindowPos_WindowPtr(w, np, ImGuiCond_Always);
    igSetWindowSize_WindowPtr(w, ns, ImGuiCond_Always);
}
bool dock_window_zoomed(const char* name) { // (the dock test's: maximized by the double-click, its old place kept)
    ImGuiWindow* w = igFindWindowByName(name);
    if (!w) return false;
    w = w->RootWindowDockTree;
    for (int k = 0; k < 16; k++)
        if (g_zoom[k].id == w->ID) return true;
    return false;
}

void dock_frame_end(Editor* ed) {
    (void)ed;
    ImGuiContext* g = igGetCurrentContext();
    ImGuiIO* io = igGetIO_Nil();
    // a drop on a gap target (the mouse let go last frame, over it)
    if (g_drag_win && !g->MovingWindow && g_gap_node && !io->MouseDown[0]) {
        ImGuiDockNode* root = igDockBuilderGetNode(dock_id());
        if (root && root->HostWindow && igDockContextFindNodeByID(g, g_gap_node->ID) == g_gap_node)
            igDockContextQueueDock(g, root->HostWindow, g_gap_node, g_drag_win, g_gap_dir, 0.3f, true);
    }
    g_drag_win = NULL, g_gap_node = NULL;
    if (g_front_outliner > 0) { // (counted from the frame their tab bars exist: a window that appears later, Levels,
        // takes the focus and its tab, so a few frames over)
        static const DockPanel front[3] = { DP_OUTLINER, DP_PLACE, DP_CONTENT };
        static int s_waited;
        bool outl = false;
        for (int k = 0; k < 3; k++) {
            ImGuiWindow* ow = igFindWindowByName(g_ids[front[k]]);
            if (ow && ow->DockNode && ow->DockNode->TabBar) ow->DockNode->TabBar->NextSelectedTabId = ow->TabId, outl |= k == 0;
        }
        if (outl || ++s_waited > 240) g_front_outliner--, s_waited = 0;
        // (the window that appeared last, Levels, keeps the focus, and ImGui gives the focused window's tab back to it
        // every frame: the focus cleared once the fronts are set)
        if (!g_front_outliner) igSetWindowFocus_Str(NULL);
    }
    // panels opened without a place: beside the viewport (Place Actors left, the Content Browser under, the rest right)
    ImGuiDockNode* central = igDockBuilderGetCentralNode(dock_id());
    ImGuiDockNode* root = igDockBuilderGetNode(dock_id());
    for (int p = 0; p < DP_COUNT; p++) {
        if (!g_redock[p]) continue;
        g_redock[p] = false;
        ImGuiWindow* w = igFindWindowByName(g_ids[p]);
        if (!w || w->DockNode || !central || !root || !root->HostWindow) continue;
        ImGuiDir dir = p == DP_PLACE || p == DP_MODES ? ImGuiDir_Left : p == DP_CONTENT || p == DP_LOG ? ImGuiDir_Down : ImGuiDir_Right;
        igDockContextQueueDock(g, root->HostWindow, central, w, dir, p == DP_CONTENT || p == DP_LOG ? 0.33f : 0.2f, false);
    }
    dock_maximized_windows();
    dock_title_double_click();
    // while a window is dragged: every target shows
    ImGuiWindow* mw = g->MovingWindow;
    if (!mw || (mw->Flags & ImGuiWindowFlags_NoDocking) || !root || !io->MouseDown[0]) return;
    if (mw->DockNodeAsHost && mw->DockNodeAsHost->ID == dock_id()) return; // (the editor's own host)
    ImDrawList* dl = igGetForegroundDrawList_ViewportPtr(igGetMainViewport());
    ImVec2 m = io->MousePos;
    ImGuiDockNode* hovered = NULL;
    for (int pass = 0; pass < 1; pass++) { // the leaf under the mouse (ImGui shows its live targets there)
        ImGuiDockNode* stack[64];
        int ns = 0;
        stack[ns++] = root;
        while (ns) {
            ImGuiDockNode* n = stack[--ns];
            if (!n->IsVisible) continue;
            if (n->ChildNodes[0] || n->ChildNodes[1]) {
                if (n->ChildNodes[0] && ns < 63) stack[ns++] = n->ChildNodes[0];
                if (n->ChildNodes[1] && ns < 63) stack[ns++] = n->ChildNodes[1];
            } else if (in_rect(m, n->Pos, (ImVec2){ n->Pos.x + n->Size.x, n->Pos.y + n->Size.y })) {
                hovered = n;
            }
        }
    }
    node_targets(dl, root, hovered);
    // the outer edges (the whole dock space's sides): ImGui's outer targets, shown all the time while dragging
    ImVec2 r0 = root->Pos, r1 = { root->Pos.x + root->Size.x, root->Pos.y + root->Size.y };
    if (!hovered)
        for (int d = 0; d < 4; d++) {
            ImVec2 a, b;
            drop_rect(r0, r1, (ImGuiDir)d, true, &a, &b);
            target(dl, a, b, false);
        }
    gap_targets(dl, root, m);
    g_drag_win = mw;
}

// --- Layouts ----------------------------------------------------------------------------------------------------------
static bool* panel_flag(Editor* ed, int p) {
    switch (p) {
    case DP_PLACE: return &ed->show_place;
    case DP_OUTLINER: return &ed->show_outliner;
    case DP_DETAILS: return &ed->show_details;
    case DP_CONTENT: return &ed->content_docked;
    case DP_LEVELS: return levels_show_flag();
    case DP_MODES: return look_modes_flag();
    case DP_LOG: return &ed->show_log;
    case DP_PLATFORMS: return look_platforms_flag();
    default: return NULL;
    }
}
static const char* const g_flag_names[DP_COUNT] = { "viewport", "place", "outliner", "details", "content", "levels", "modes", "log", "platforms" };

static bool layouts_dir(bool project, char* out, size_t cap) {
    if (project) {
        if (!project_file()[0]) return false;
        char root[600];
        project_root(root, sizeof root);
        snprintf(out, cap, "%s/Config/Layouts", root);
    } else {
        char d[512];
        if (!re_config_dir(d, sizeof d)) return false;
        snprintf(out, cap, "%s\\Layouts", d);
    }
    return true;
}

static char* layout_text(Editor* ed, bool docs) {
    size_t n = 0;
    const char* ini = igSaveIniSettingsToMemory(&n);
    size_t cap = n + 1024 + (docs ? 16384 : 0);
    char* t = malloc(cap);
    if (!t) return NULL;
    int w = snprintf(t, cap, "; Real Engine layout (Window > Layout): the panels shown, then the docking (ImGui)\n[RealEngine][Panels]\n");
    for (int p = 1; p < DP_COUNT; p++) w += snprintf(t + w, cap - (size_t)w, "%s=%d\n", g_flag_names[p], *panel_flag(ed, p) ? 1 : 0);
    w += snprintf(t + w, cap - (size_t)w, "\n");
    if (docs) w += (int)edhost_layout_write(t + w, cap - (size_t)w - n - 1); // (item 9) the asset editors open
    memcpy(t + w, ini, n);
    t[w + n] = 0;
    return t;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

static char* read_text(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (8 << 20) ? malloc((size_t)n + 1) : NULL;
    if (t) {
        size_t r = fread(t, 1, (size_t)n, f);
        t[r] = 0;
    }
    fclose(f);
    return t;
}

bool dock_layout_save(Editor* ed, const char* name, bool project) {
    char dir[700], path[900];
    if (!name || !name[0] || !layouts_dir(project, dir, sizeof dir)) return false;
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/%s.rlayout", dir, name);
    char* t = layout_text(ed, false); // (a named layout: the panels and docking only, no project's documents)
    bool ok = t && write_text(path, t);
    free(t);
    return ok;
}

bool dock_layout_load_file(Editor* ed, const char* path) {
    (void)ed;
    char* t = read_text(path);
    if (!t) return false;
    if (!strstr(t, "[RealEngine][Panels]")) {
        free(t);
        return false;
    }
    free(g_pending);
    g_pending = t;
    return true;
}

void dock_layout_default(Editor* ed) { // (Q26: every panel of the mockups' layout shown)
    ed->show_outliner = ed->show_details = true;
    ed->show_place = true;
    ed->content_docked = true;
    ed->drawer_open = false;
    ed->show_log = true;
    *levels_show_flag() = true;
    *look_modes_flag() = true;
    *look_platforms_flag() = true;
    g_build_default = true;
}

void dock_between_frames(Editor* ed) {
    if (!g_restored) { // the editor opens: the project's last layout, else the user's, else the default (an automated
        // run: the default's tree with only the Outliner, Levels and Details shown, as the tests always had it)
        g_restored = true;
        *levels_show_flag() = true; // (Levels beside the Outliner by default)
        snprintf(g_last_project, sizeof g_last_project, "%s", project_file());
        if (!g_editor_prefs_readonly || g_look_test) { // (the look test reads its own folder's)
            char dir[700], path[900];
            bool got = false;
            if (layouts_dir(true, dir, sizeof dir)) {
                char root[600];
                project_root(root, sizeof root);
                snprintf(path, sizeof path, "%s/Saved/Layout.rlayout", root);
                got = dock_layout_load_file(ed, path);
            }
            if (!got && layouts_dir(false, dir, sizeof dir)) {
                snprintf(path, sizeof path, "%s/Last.rlayout", dir);
                got = dock_layout_load_file(ed, path);
            }
            if (got && !look_keep_saved_layout()) { // (Q26) the new look replaces it once: kept as a user layout to go back to
                look_previous_layout(g_pending);
                free(g_pending), g_pending = NULL;
                got = false;
            }
            if (!got) dock_layout_default(ed); // (a new user: the whole default)
        }
    } else if (strcmp(g_last_project, project_file()) != 0) { // another project: its layout (this one's kept)
        snprintf(g_last_project, sizeof g_last_project, "%s", project_file());
        char root[600], path[900];
        project_root(root, sizeof root);
        snprintf(path, sizeof path, "%s/Saved/Layout.rlayout", root);
        if (!g_editor_prefs_readonly) dock_layout_load_file(ed, path);
    }
    if (!g_pending) return;
    char* t = g_pending;
    g_pending = NULL;
    for (int p = 1; p < DP_COUNT; p++) { // the panels shown
        char key[48];
        snprintf(key, sizeof key, "\n%s=", g_flag_names[p]);
        const char* s = strstr(t, key);
        if (s) *panel_flag(ed, p) = atoi(s + strlen(key)) != 0;
        else if (p == DP_MODES || p == DP_PLATFORMS) *panel_flag(ed, p) = false; // (a layout from before 0.44: they stay closed)
    }
    igLoadIniSettingsFromMemory(t, 0); // (ImGui's docking: the nodes rebuilt, the windows put back in them)
    if (!g_editor_prefs_readonly && project_file()[0]) edhost_layout_read(t); // (item 9) the editors the project closed with
    ImGuiDockNode* n = igDockBuilderGetNode(dock_id());
    g_build_default = !n || (!n->ChildNodes[0] && !n->Windows.Size);
    free(t);
}

void dock_layout_autosave(Editor* ed) {
    // (2026-11-20, crashes #143-146) ImGui's settings: never without its context (main.c saves before destroying it)
    if (g_editor_prefs_readonly || !igGetCurrentContext()) return;
    char dir[700], path[900];
    char* t = layout_text(ed, project_file()[0] != 0); // (the project's own: its documents too, item 9)
    if (!t) return;
    if (project_file()[0]) {
        char root[600];
        project_root(root, sizeof root);
        snprintf(dir, sizeof dir, "%s/Saved", root);
        re_make_dirs(dir);
        snprintf(path, sizeof path, "%s/Layout.rlayout", dir);
        write_text(path, t);
    }
    if (layouts_dir(false, dir, sizeof dir)) {
        re_make_dirs(dir);
        snprintf(path, sizeof path, "%s/Last.rlayout", dir);
        write_text(path, t);
    }
    free(t);
}

// --- Window > Layout --------------------------------------------------------------------------------------------------
static bool g_open_save;
typedef struct LayoutList { char name[32][64]; bool project[32]; int n; bool want_project; } LayoutList;
static void list_cb(void* user, const char* name) {
    LayoutList* l = user;
    size_t n = strlen(name);
    if (l->n >= 32 || n < 9 || strcmp(name + n - 8, ".rlayout") != 0 || !strcmp(name, "Last.rlayout")) return;
    snprintf(l->name[l->n], sizeof l->name[0], "%.*s", (int)(n - 8), name);
    l->project[l->n++] = l->want_project;
}

void dock_layout_menu(Editor* ed) {
    if (!ui_begin_menu(ICON_PANEL, TR("Layout"), true)) return;
    if (ui_menu_item(ICON_SAVE, TR("Save Layout As..."), NULL, false, true)) g_open_save = true;
    if (ui_menu_item(ICON_UNDO, TR("Default Layout"), NULL, false, true)) dock_layout_default(ed);
    igSeparator();
    LayoutList l = { 0 };
    char dir[700];
    for (int pr = 1; pr >= 0; pr--)
        if (layouts_dir(pr == 1, dir, sizeof dir)) l.want_project = pr == 1, re_list_dir(dir, list_cb, &l);
    if (!l.n) igTextDisabled("%s", TR("No saved layouts yet"));
    for (int i = 0; i < l.n; i++) {
        char label[160];
        snprintf(label, sizeof label, "%s  (%s)", l.name[i], l.project[i] ? TR("project") : TR("user"));
        if (ui_menu_item(ICON_PANEL, label, NULL, false, true) && layouts_dir(l.project[i], dir, sizeof dir)) {
            char path[900];
            snprintf(path, sizeof path, "%s/%s.rlayout", dir, l.name[i]);
            if (!dock_layout_load_file(ed, path)) editor_status(ed, igGetTime(), TR("Could not read the layout %s."), l.name[i]);
        }
    }
    igEndMenu();
}

void dock_layout_popups(Editor* ed) {
    static char name[64] = "Meu layout";
    static bool for_project = true;
    char title[96];
    snprintf(title, sizeof title, "%s###re_save_layout", TR("Save Layout As"));
    if (g_open_save) igOpenPopup_Str(title, 0), g_open_save = false;
    if (igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        igSetNextItemWidth(igGetFontSize() * 16);
        igInputText(TR("Name"), name, sizeof name, 0, NULL, NULL);
        bool has_project = project_file()[0] != 0;
        if (!has_project) for_project = false;
        igBeginDisabled(!has_project);
        if (igRadioButton_Bool(TR("This project"), for_project)) for_project = true;
        igEndDisabled();
        igSameLine(0, -1);
        if (igRadioButton_Bool(TR("All my projects (user)"), !for_project)) for_project = false;
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) && name[0]) {
            if (dock_layout_save(ed, name, for_project)) editor_status(ed, igGetTime(), TR("Layout %s saved."), name);
            else editor_status(ed, igGetTime(), TR("Could not save the layout %s."), name);
            igCloseCurrentPopup();
        }
        igSameLine(0, -1);
        if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) igCloseCurrentPopup();
        igEndPopup();
    }
}

// --- The main window's close ---------------------------------------------------------------------------------------------
static void* g_test_fg;
void dock_route_test_foreground(void* hwnd) { g_test_fg = hwnd; }

bool dock_route_main_close(GLFWwindow* main_window) {
    if (!glfwWindowShouldClose(main_window)) return false;
#ifdef _WIN32
    HWND fg = g_test_fg ? (HWND)g_test_fg : GetForegroundWindow(), mh = glfwGetWin32Window(main_window);
    if (!fg || fg == mh) return false;
    ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
    for (int i = 0; i < pio->Viewports.Size; i++) {
        ImGuiViewport* v = pio->Viewports.Data[i];
        if (v == igGetMainViewport() || (HWND)v->PlatformHandleRaw != fg) continue;
        v->PlatformRequestClose = true; // (its windows close, ImGui's way: as its own X / Alt+F4 would)
        glfwSetWindowShouldClose(main_window, GLFW_FALSE);
        printf("[RealEngine] A close for a window of its own (in front) reached the main window: that window closes, the engine stays.\n");
        return true;
    }
#endif
    return false;
}

// --- The tests ----------------------------------------------------------------------------------------------------------
bool dock_panel_rect(DockPanel p, float r[4]) {
    ImGuiWindow* w = igFindWindowByName(g_ids[p]);
    ImGuiDockNode* n = p == DP_VIEWPORT ? igDockBuilderGetCentralNode(dock_id()) : w ? w->DockNode : NULL; // (the viewport: the central node)
    if (!n) return false;
    r[0] = n->Pos.x, r[1] = n->Pos.y, r[2] = n->Size.x, r[3] = n->Size.y;
    return true;
}
bool dock_space_rect(float r[4]) {
    ImGuiDockNode* n = igDockBuilderGetNode(dock_id());
    if (!n) return false;
    r[0] = n->Pos.x, r[1] = n->Pos.y, r[2] = n->Size.x, r[3] = n->Size.y;
    return true;
}
