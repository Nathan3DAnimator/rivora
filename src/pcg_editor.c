// The PCG Graph editor (Q15, pcg.h; notas-dev/pcg.md): Unreal's PCG Graph editor, simpler. The graph on a canvas in
// the engine's graph style (Órbita: cards with the family's band and icon, pins by shape, wires in a gradient), the
// selected node's settings on the right (each number can be bound to a graph parameter), the graph's parameters, the
// points each node made in the last generation on its card. Every change is the components' at once (they regenerate:
// no save needed to see it); Ctrl+S saves. D marks a node to show its points in the viewport. Also here: the PCG
// component's Details, the viewport's points and boxes, Content Browser > Add > PCG Graph.
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "editor_docs.h"
#include "asset_picker.h"
#include "blueprint.h"
#include "content.h"
#include "graph_style.h"
#include "graph_view.h"
#include "graph_layout.h"
#include "i18n.h"
#include "pcg.h"
#include "theme.h"
#include "ui_icons.h"
#include "ui_tips.h"

static struct {
    bool open, unsaved;
    char vpath[600], status[160];
    PcgGraph g;
    GraphView view;
    int sel, drag;
    int wire_node, wire_pin; // dragging a wire from an output
    float menu_g[2];
    int menu_from, menu_from_pin, menu_node;
} E = { .sel = -1, .drag = -1, .wire_node = -1, .menu_from = -1, .menu_node = -1 };

static const int g_fam_color[] = { TK_N10, TK_GREEN_D, TK_AMBER, TK_BLUE_D, TK_ORANGE, TK_MAGENTA };
static const int g_fam_icon[] = { ICON_GRAPH, ICON_LANDSCAPE, ICON_FILTER, ICON_MOVE, ICON_FOLIAGE, ICON_GRAPH };
static const char* const g_fam_names[] = { "Input / Output", "Sources", "Filters", "Points", "Spawners", "Graph" };

void pcg_editor_open(const char* vpath) {
    snprintf(E.vpath, sizeof E.vpath, "%s", vpath);
    memset(&E.g, 0, sizeof E.g); // (read on the first draw: an editor opened from the command line opens before the project)
    E.open = true, E.unsaved = false, E.status[0] = 0, E.sel = -1, E.drag = -1, E.wire_node = -1;
    if (E.view.zoom <= 0.0f) E.view.zoom = 1.0f;
    E.view.pan[0] = 420.0f, E.view.pan[1] = 300.0f;
}

static void changed(void) {
    E.unsaved = true;
    E.g.version++;
    pcg_graph_edited(&E.g); // (the components using it regenerate)
}

// --- The canvas ---
#define NODE_W 210.0f

static void add_menu(float gx, float gy, int from, int from_pin) {
    for (int f = PCGF_FAM_SOURCE; f <= PCGF_FAM_GRAPH; f++) {
        if (!ui_begin_menu(g_fam_icon[f], TR(g_fam_names[f]), true)) continue;
        for (int t = 0; t < PCGN_TYPE_COUNT; t++) {
            const PcgNodeInfo* ni = pcg_node_info(t);
            if (ni->family != f || (from >= 0 && !ni->nin)) continue;
            if (!ui_menu_item(g_fam_icon[f], TR(ni->name), NULL, false, E.g.nnodes < PCG_MAX_NODES)) continue;
            ui_tip_last_item(ni->name);
            int k = pcg_graph_add(&E.g, t, gx, gy);
            if (k >= 0) {
                if (from >= 0) pcg_graph_link(&E.g, from, from_pin, k, 0);
                E.sel = k;
                changed();
            }
        }
        igEndMenu();
    }
}

static void pin_pos(ImVec2 a, ImVec2 b, float title, float row, int pin, bool out, ImVec2* p) {
    *p = (ImVec2){ out ? b.x : a.x, a.y + title + row * ((float)pin + 0.5f) + 2.0f };
}

// (2026-11-23) Organize Nodes (Q, the menus): the graph in columns toward its Output (graph_layout.c)
static bool pcg_organize(PcgGraph* g) {
    static GlNode nodes[PCG_MAX_NODES];
    static GlEdge edges[PCG_MAX_LINKS];
    float fs = igGetFontSize(), title = fs + 10.0f, row = fs + 6.0f;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const PcgNodeInfo* ni = pcg_node_info(g->nodes[i].type);
        int rows = ni->nin > ni->nout ? ni->nin : ni->nout;
        nodes[i] = (GlNode){ g->nodes[i].pos[0], g->nodes[i].pos[1], NODE_W, title + row * (float)(rows > 0 ? rows : 1) + row + 6.0f };
    }
    for (uint32_t l = 0; l < g->nlinks; l++) edges[l] = (GlEdge){ g->links[l].from, g->links[l].to };
    if (!graph_layout(nodes, (int)g->nnodes, edges, (int)g->nlinks, NULL, GL_FLOW_RIGHT, 70.0f, 24.0f)) return false;
    for (uint32_t i = 0; i < g->nnodes; i++) g->nodes[i].pos[0] = roundf(nodes[i].x), g->nodes[i].pos[1] = roundf(nodes[i].y);
    return true;
}

static void canvas(float w, float h, const uint32_t* pts, const float* ms) {
    GraphView* v = &E.view;
    PcgGraph* g = &E.g;
    igBeginChild_Str("pcg_canvas", (ImVec2){ w, h }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    igInvisibleButton("pcg_bg", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, its nodes drawn with their own names)
    bool hovered = igIsItemHovered(0);
    ImGuiIO* io = igGetIO_Nil();
    gv_input(v, o, hovered || v->panning);
    gv_grid(v, dl, o, sz, TH(TK_N6));
    float z = v->zoom, fs = igGetFontSize() * fmaxf(z, 0.6f);
    const ImGuiStyle* st = igGetStyle();
    igPushFont(NULL, roundf((st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f) * fmaxf(z, 0.6f))); // (whole pixels: a new size per zoom step bakes glyphs and waits on the GPU)
    float bw = NODE_W * z, title = fs + 10.0f * z, row = fs + 6.0f * z, pr = 5.5f * z;
    ImVec2 p0[PCG_MAX_NODES], p1[PCG_MAX_NODES];
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const PcgNodeInfo* ni = pcg_node_info(g->nodes[i].type);
        int rows = ni->nin > ni->nout ? ni->nin : ni->nout;
        p0[i] = gv_screen(v, o, g->nodes[i].pos[0], g->nodes[i].pos[1]);
        p1[i] = (ImVec2){ p0[i].x + bw, p0[i].y + title + row * (float)(rows > 0 ? rows : 1) + row + 6.0f * z };
    }
    // the wires
    const uint32_t wcol[2] = { TH(TK_CYAN), TH(TK_LIME) };
    for (uint32_t i = 0; i < g->nlinks; i++) {
        const PcgLink* l = &g->links[i];
        ImVec2 a, b;
        pin_pos(p0[l->from], p1[l->from], title, row, l->from_pin, true, &a);
        pin_pos(p0[l->to], p1[l->to], title, row, l->to_pin, false, &b);
        bool hi = l->from == E.sel || l->to == E.sel;
        graph_wire2(dl, a, b, hi ? TH(TK_AMBER) : wcol[0], hi ? TH(TK_AMBER) : wcol[1], z, false, -1);
    }
    // the nodes
    int hover = -1, hover_in = -1, hover_out = -1;
    ImVec2 m = io->MousePos;
    char line[160];
    bool orbit = graph_orbit();
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const PcgNode* n = &g->nodes[i];
        const PcgNodeInfo* ni = pcg_node_info(n->type);
        ImVec2 a = p0[i], b = p1[i];
        if (b.x < o.x - 40 || b.y < o.y - 40 || a.x > o.x + sz.x + 40 || a.y > o.y + sz.y + 40) continue;
        bool in = hovered && m.x >= a.x - pr * 2 && m.x < b.x + pr * 2 && m.y >= a.y && m.y < b.y;
        if (in) hover = (int)i;
        for (int p = 0; p < ni->nin && in; p++) {
            ImVec2 c;
            pin_pos(a, b, title, row, p, false, &c);
            if (fabsf(m.x - c.x) < pr * 2.2f && fabsf(m.y - c.y) < row * 0.5f) hover_in = p;
        }
        for (int p = 0; p < ni->nout && in; p++) {
            ImVec2 c;
            pin_pos(a, b, title, row, p, true, &c);
            if (fabsf(m.x - c.x) < pr * 2.2f && fabsf(m.y - c.y) < row * 0.5f) hover_out = p;
        }
        bool err = false;
        if (orbit) graph_card(dl, a, b, title, TH(g_fam_color[ni->family]), g_fam_icon[ni->family], (int)i == E.sel, (int)i == hover, err, z);
        else {
            ImDrawList_AddRectFilled(dl, a, b, THA(TK_N4, 240), 6 * z, 0);
            ImDrawList_AddRectFilled(dl, a, (ImVec2){ b.x, a.y + title }, TH(g_fam_color[ni->family]), 6 * z, ImDrawFlags_RoundCornersTop);
            ImDrawList_AddRect(dl, a, b, (int)i == E.sel ? TH(TK_ACCENT) : TH(TK_N10), 6 * z, (int)i == E.sel ? 2.5f : 1.2f, 0);
        }
        float tx = orbit ? graph_card_title_x(a, title, g_fam_icon[ni->family], z) : a.x + 8 * z;
        ImDrawList_AddText_Vec2(dl, (ImVec2){ tx, a.y + 5 * z }, TH(TK_NODE_TITLE), TR(ni->name), NULL);
        if (n->flags & PCGNF_DEBUG) ImDrawList_AddText_Vec2(dl, (ImVec2){ b.x - 18 * z, a.y + 5 * z }, TH(TK_CYAN_L), "D", NULL); // i18n: none (a letter)
        if (n->flags & PCGNF_DISABLED) ImDrawList_AddLine(dl, a, b, THA(TK_RED, 160), 2.0f * z);
        for (int p = 0; p < ni->nin; p++) {
            ImVec2 c;
            pin_pos(a, b, title, row, p, false, &c);
            bool linked = false;
            for (uint32_t k = 0; k < g->nlinks; k++) linked |= g->links[k].to == i && g->links[k].to_pin == p;
            graph_pin(dl, c, GP_ARRAY, TH(TK_LIME), 0, linked, z);
            if (ni->nin > 1) ImDrawList_AddText_Vec2(dl, (ImVec2){ c.x + 10 * z, c.y - fs * 0.5f }, TH(TK_N13), TR(ni->in_names[p]), NULL);
        }
        for (int p = 0; p < ni->nout; p++) {
            ImVec2 c;
            pin_pos(a, b, title, row, p, true, &c);
            bool linked = false;
            for (uint32_t k = 0; k < g->nlinks; k++) linked |= g->links[k].from == i && g->links[k].from_pin == p;
            graph_pin(dl, c, GP_ARRAY, (int)i == hover && hover_out == p ? TH(TK_AMBER) : TH(TK_CYAN), 0, linked, z);
            if (ni->nout > 1) {
                const char* t = TR(ni->out_names[p]);
                ImDrawList_AddText_Vec2(dl, (ImVec2){ c.x - 10 * z - igCalcTextSize(t, NULL, false, -1).x, c.y - fs * 0.5f }, TH(TK_N13), t, NULL);
            }
        }
        // the last generation's points (and time) on its foot
        line[0] = 0;
        if (pts && pts[i]) snprintf(line, sizeof line, "%u %s  %.2f ms", pts[i], TR("points"), ms ? ms[i] : 0.0f);
        else if (n->type == PCGN_MESH_SPAWNER || n->type == PCGN_NESTED || n->type == PCGN_SUBGRAPH || n->type == PCGN_ACTOR_SPAWNER) {
            const char* s = n->s[0][0] ? n->s[0] : "-";
            const char* sl = strrchr(s, '/');
            snprintf(line, sizeof line, "%s", sl ? sl + 1 : s);
        }
        if (line[0]) {
            ImVec4 clip = { a.x + 6 * z, b.y - row - 2 * z, b.x - 6 * z, b.y };
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 8 * z, b.y - row - 2 * z }, TH(TK_N12), line, NULL, 0.0f, &clip);
        }
    }
    // clicks: an output pin starts a wire; an input pin with wires lifts them (re-routed from their source); a node is
    // selected and dragged
    if (igIsItemActivated() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (hover >= 0 && hover_out >= 0) E.wire_node = hover, E.wire_pin = hover_out;
        else if (hover >= 0 && hover_in >= 0) {
            int src = -1, sp = 0;
            for (uint32_t k = 0; k < g->nlinks; k++)
                if (g->links[k].to == hover && g->links[k].to_pin == hover_in) src = g->links[k].from, sp = g->links[k].from_pin, g->links[k--] = g->links[--g->nlinks];
            if (src >= 0) E.wire_node = src, E.wire_pin = sp, changed();
        } else E.sel = hover, E.drag = hover;
    }
    if (igIsItemActive() && igIsMouseDown_Nil(ImGuiMouseButton_Left) && E.drag >= 0 && E.wire_node < 0 && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        g->nodes[E.drag].pos[0] += io->MouseDelta.x / z, g->nodes[E.drag].pos[1] += io->MouseDelta.y / z;
        E.unsaved = true; // (moving a node changes nothing it makes)
    }
    { // (2026-11-23) Blender's edge pan while a node or a wire is dragged
        float es[2];
        bool node = igIsItemActive() && E.drag >= 0 && E.drag < (int)g->nnodes && E.wire_node < 0;
        if ((node || E.wire_node >= 0) && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(o, sz, es)) {
            v->pan[0] -= es[0] / z, v->pan[1] -= es[1] / z;
            if (node) g->nodes[E.drag].pos[0] += es[0] / z, g->nodes[E.drag].pos[1] += es[1] / z;
        }
    }
    if (E.wire_node >= 0) {
        ImVec2 a;
        pin_pos(p0[E.wire_node], p1[E.wire_node], title, row, E.wire_pin, true, &a);
        graph_wire2(dl, a, io->MousePos, TH(TK_AMBER), TH(TK_AMBER), z, false, -1);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            if (hover >= 0 && hover != E.wire_node) {
                const PcgNodeInfo* hi = pcg_node_info(g->nodes[hover].type);
                int pin = hover_in >= 0 ? hover_in : 0;
                if (hi->nin && pcg_graph_link(g, E.wire_node, E.wire_pin, hover, pin)) changed();
                else snprintf(E.status, sizeof E.status, "%s", TR("Not wired: that node has no input, or it would make a loop."));
            } else if (hover < 0) {
                gv_graph(v, o, io->MousePos, &E.menu_g[0], &E.menu_g[1]);
                E.menu_from = E.wire_node, E.menu_from_pin = E.wire_pin;
                igOpenPopup_Str("pcg_add", 0);
            }
            E.wire_node = -1;
        }
    }
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) E.drag = -1;
    if (hovered && gv_right_click()) {
        gv_graph(v, o, io->MousePos, &E.menu_g[0], &E.menu_g[1]);
        E.menu_node = hover;
        if (hover >= 0) E.sel = hover, igOpenPopup_Str("pcg_node", 0);
        else E.menu_from = -1, igOpenPopup_Str("pcg_add", 0);
    }
    igPopFont();
    if (igBeginPopup("pcg_add", 0)) {
        igTextDisabled("%s", E.menu_from >= 0 ? TR("Add and wire") : TR("Add Node"));
        add_menu(E.menu_g[0], E.menu_g[1], E.menu_from, E.menu_from_pin);
        if (E.menu_from < 0) {
            igSeparator();
            if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, g->nnodes > 1) && pcg_organize(g)) changed();
        }
        igEndPopup();
    }
    if (igBeginPopup("pcg_node", 0)) {
        PcgNode* n = E.menu_node >= 0 && E.menu_node < (int)g->nnodes ? &g->nodes[E.menu_node] : NULL;
        if (n) {
            bool io_node = n->type == PCGN_INPUT || n->type == PCGN_OUTPUT;
            if (ui_menu_item(ICON_EYE, TR("Debug Points"), "D", (n->flags & PCGNF_DEBUG) != 0, true)) n->flags ^= PCGNF_DEBUG, changed();
            if (!io_node && ui_menu_item(ICON_CROSS, TR("Disable Node"), NULL, (n->flags & PCGNF_DISABLED) != 0, true)) n->flags ^= PCGNF_DISABLED, changed();
            if (ui_menu_item(ICON_CROSS, TR("Break Links"), NULL, false, true)) {
                for (uint32_t k = 0; k < g->nlinks; k++)
                    if (g->links[k].from == E.menu_node || g->links[k].to == E.menu_node) g->links[k--] = g->links[--g->nlinks];
                changed();
            }
            if (!io_node && ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, g->nnodes < PCG_MAX_NODES)) {
                PcgNode copy = *n;
                int c = pcg_graph_add(g, n->type, n->pos[0] + 40.0f, n->pos[1] + 60.0f);
                if (c >= 0) copy.id = g->nodes[c].id, copy.pos[0] += 40.0f, copy.pos[1] += 60.0f, g->nodes[c] = copy, E.sel = c, changed();
            }
            if (!io_node && ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, true)) pcg_graph_remove(g, E.menu_node), E.sel = -1, changed();
        }
        igEndPopup();
    }
    bool keys = igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput;
    if (keys && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false) && pcg_organize(g)) changed(); // (2026-11-23)
    if (keys && E.sel >= 0 && E.sel < (int)g->nnodes) {
        PcgNode* n = &g->nodes[E.sel];
        bool io_node = n->type == PCGN_INPUT || n->type == PCGN_OUTPUT;
        if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && !io_node) pcg_graph_remove(g, E.sel), E.sel = -1, changed();
        else if (igIsKeyPressed_Bool(ImGuiKey_D, false) && !io->KeyCtrl) n->flags ^= PCGNF_DEBUG, changed();
    }
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, o.y + sz.y - igGetFontSize() - 6 }, TH(TK_N12),
                            TR("Drag from an output to an input to wire (on nothing: a new node) | right click: add / node menu | D: show its points | right or middle drag: pan | wheel: zoom"), NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 12, o.y + 8 }, TH(TK_N13), "PCG", NULL); // i18n: none (the PCG's name)
    igEndChild();
}

// --- The selected node's settings ---
static uint32_t g_field_id;
static const char* base_name(const char* name, char* out, size_t cap) { // ("Mesh 2" -> "Mesh": the tip)
    size_t l = strlen(name);
    snprintf(out, cap, "%s", name);
    if (l > 2 && name[l - 2] == ' ' && name[l - 1] >= '1' && name[l - 1] <= '9') out[l - 2] = 0;
    return out;
}

static bool prop_ui(PcgNode* n, const PcgProp* p) {
    PcgGraph* g = &E.g;
    bool ch = false;
    char tip[48], lab[96];
    base_name(p->name, tip, sizeof tip);
    igPushID_Str(p->name);
    igTableNextRow(0, 0);
    igTableNextColumn();
    igAlignTextToFramePadding();
    snprintf(lab, sizeof lab, "%s%s", TR(tip), strcmp(tip, p->name) ? p->name + strlen(tip) : "");
    igTextUnformatted(lab, NULL);
    ui_tip_last_item(tip);
    igTableNextColumn();
    float* f = &n->f[p->slot];
    char* s = n->s[p->slot];
    bool bindable = p->kind <= PP_ENUM && g->nparams;
    igSetNextItemWidth(bindable ? -igGetFrameHeight() - 4.0f : -1.0f);
    switch (p->kind) {
    case PP_FLOAT: ch = igDragFloat("##v", f, fmaxf((p->mx - p->mn) * 0.002f, 0.001f), p->mn, p->mx, p->fmt ? p->fmt : "%.3f", 0); break;
    case PP_INT: {
        int iv = (int)*f;
        if (igDragInt("##v", &iv, 0.1f, (int)p->mn, (int)p->mx, "%d", 0)) *f = (float)iv, ch = true;
        break;
    }
    case PP_BOOL: {
        bool b = *f != 0.0f;
        if (igCheckbox("##v", &b)) *f = b ? 1.0f : 0.0f, ch = true;
        break;
    }
    case PP_ENUM: {
        const char* names[8];
        for (int k = 0; k < p->nenums && k < 8; k++) names[k] = TR(p->enums[k]);
        int iv = (int)*f;
        if (igCombo_Str_arr("##v", &iv, names, p->nenums, -1)) *f = (float)iv, ch = true;
        break;
    }
    case PP_VEC3: ch = igDragFloat3("##v", f, 1.0f, p->mn, p->mx, p->fmt ? p->fmt : "%.2f", 0); break;
    case PP_COLOR: ch = igColorEdit3("##v", f, ImGuiColorEditFlags_Float); break;
    case PP_MESH: case PP_MATERIAL: case PP_CLASS: case PP_GRAPH: {
        uint64_t kinds = p->kind == PP_MESH ? AK(ASSET_STATIC_MESH) : p->kind == PP_MATERIAL ? AK(ASSET_MATERIAL) | AK(ASSET_MATERIAL_INSTANCE)
                         : p->kind == PP_CLASS ? AK(ASSET_BLUEPRINT) : AK(ASSET_PCG_GRAPH);
        int kind = p->kind == PP_MESH ? ASSET_STATIC_MESH : p->kind == PP_MATERIAL ? ASSET_MATERIAL : p->kind == PP_CLASS ? ASSET_BLUEPRINT : ASSET_PCG_GRAPH;
        const char* sl = strrchr(s, '/');
        char out[PCG_STR];
        int r = asset_field("##asset", s[0] ? (sl ? sl + 1 : s) : TR("None"), s, kind, kinds, p->kind != PP_MESH, out, sizeof out);
        if (r == ASSET_FIELD_OPENED && p->kind == PP_MESH) {
            static const char* const shapes[] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
            for (int k = 0; k < 5; k++) asset_picker_add(TR(shapes[k]), shapes[k], ASSET_BASIC_SHAPE, -1, 0, 0);
        }
        if (r == ASSET_FIELD_PICKED) snprintf(s, PCG_STR, "%s", out), ch = true;
        break;
    }
    default: ch = igInputText("##v", s, PCG_STR, 0, NULL, NULL); break;
    }
    if (bindable) { // a graph parameter drives it
        igSameLine(0, 4);
        int b = n->bind[p->slot];
        bool bound = b && b <= (int)g->nparams;
        if (bound) igPushStyleColor_Vec4(ImGuiCol_Button, THV(TK_ACCENT));
        if (igButton("P##bind", (ImVec2){ igGetFrameHeight(), 0 })) igOpenPopup_Str("bind", 0); // i18n: none (a letter)
        if (bound) igPopStyleColor(1);
        ui_tip_last_item("Bind to Parameter");
        if (igBeginPopup("bind", 0)) {
            if (igSelectable_Bool(TR("(none)"), !bound, 0, (ImVec2){ 0, 0 })) n->bind[p->slot] = 0, ch = true;
            for (uint32_t k = 0; k < g->nparams; k++)
                if (igSelectable_Bool(g->params[k].name, b == (int)k + 1, 0, (ImVec2){ 0, 0 })) n->bind[p->slot] = (uint8_t)(k + 1), ch = true;
            igEndPopup();
        }
    }
    igPopID();
    (void)g_field_id;
    return ch;
}

static void details(const uint32_t* pts, const float* ms) {
    PcgGraph* g = &E.g;
    bool ch = false;
    if (E.sel >= 0 && E.sel < (int)g->nnodes) {
        PcgNode* n = &g->nodes[E.sel];
        const PcgNodeInfo* ni = pcg_node_info(n->type);
        igTextColored(THV(TK_ACCENT), "%s", TR(ni->name));
        igPushTextWrapPos(0.0f);
        igTextDisabled("%s", TR(ni->desc));
        igPopTextWrapPos();
        if (pts) igTextDisabled(TR("Last generation: %u points, %.2f ms"), pts[E.sel], ms ? ms[E.sel] : 0.0f);
        bool dbg = (n->flags & PCGNF_DEBUG) != 0;
        if (igCheckbox(TR("Debug Points"), &dbg)) n->flags ^= PCGNF_DEBUG, ch = true;
        if (igBeginTable("pcg_props", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 8.0f, 0);
            igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0); // i18n: none (a column id)
            int entry = 0;
            for (int i = 0; i < ni->nprops; i++) {
                const PcgProp* p = &ni->props[i];
                size_t l = strlen(p->name);
                int e = l > 2 && p->name[l - 2] == ' ' && p->name[l - 1] >= '1' && p->name[l - 1] <= '4' ? p->name[l - 1] - '0' : 0;
                if (n->type == PCGN_MESH_SPAWNER && e && e != entry) { // (the spawner's entries: a band each)
                    entry = e;
                    igTableNextRow(0, 0);
                    igTableNextColumn();
                    igTextColored(THV(TK_N13), TR("Entry %d"), e);
                    igTableNextColumn();
                }
                ch |= prop_ui(n, p);
            }
            igEndTable();
        }
    } else
        igTextDisabled("%s", TR("Select a node to see its settings."));
    igSeparator();
    igTextColored(THV(TK_ACCENT), "%s", TR("Graph Parameters"));
    ui_tip_last_item("Graph Parameters");
    for (uint32_t k = 0; k < g->nparams; k++) {
        PcgParam* p = &g->params[k];
        igPushID_Int((int)k);
        igSetNextItemWidth(igGetFontSize() * 7.0f);
        ch |= igInputText("##pname", p->name, sizeof p->name, 0, NULL, NULL);
        ui_tip_last_item("Parameter Name");
        igSameLine(0, 4);
        igSetNextItemWidth(igGetFontSize() * 6.0f);
        ch |= igDragFloat("##pval", &p->value, fmaxf((p->mx - p->mn) * 0.002f, 0.0005f), p->mn, p->mx, "%.4g", 0);
        ui_tip_last_item("Parameter Value");
        igSameLine(0, 4);
        if (igSmallButton("x##pdel")) {
            for (uint32_t i = 0; i < g->nnodes; i++)
                for (int s = 0; s < PCG_NODE_F; s++) {
                    if (g->nodes[i].bind[s] == k + 1) g->nodes[i].bind[s] = 0;
                    else if (g->nodes[i].bind[s] > k + 1) g->nodes[i].bind[s]--;
                }
            memmove(&g->params[k], &g->params[k + 1], sizeof(PcgParam) * (g->nparams - k - 1));
            g->nparams--, ch = true;
            igPopID();
            break;
        }
        ui_tip_last_item("Remove Parameter");
        igPopID();
    }
    if (g->nparams < PCG_MAX_PARAMS && igSmallButton(TR("+ Parameter"))) {
        PcgParam* p = &g->params[g->nparams++];
        snprintf(p->name, sizeof p->name, "Param%u", g->nparams);
        p->value = 1.0f, p->mn = 0.0f, p->mx = 10.0f, ch = true;
    }
    if (ch) changed();
}

void pcg_editor_draw(void) {
    if (!E.open) return;
    if (!E.g.nnodes && !E.unsaved) { // (not read yet, or the project wasn't set when it opened)
        pcg_graph_remember(E.vpath);
        const PcgGraph* c = pcg_graph_get(pcg_vpath_hash(E.vpath));
        if (c) E.g = *c;
        else pcg_graph_load(E.vpath, &E.g);
    }
    char title[300];
    const char* nm = strrchr(E.vpath, '/');
    snprintf(title, sizeof title, "%s%s - %s###pcg_editor", nm ? nm + 1 : E.vpath, E.unsaved ? "*" : "", TR("PCG Graph"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.78f, mv->Size.y * 0.74f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.11f, mv->Pos.y + mv->Size.y * 0.12f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = pcg_graph_save(E.vpath, &E.g);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        igSameLine(0, 8);
        if (igButton(TR("Regenerate"), (ImVec2){ 0, 0 })) changed();
        igSameLine(0, 10);
        igTextDisabled("%s", TR("Points from the ground, a volume or splines, filtered, moved and spawned as meshes, actors or other graphs."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        const uint32_t* pts = NULL;
        const float* ms = NULL;
        pcg_last_nodes(E.g.hash, &pts, &ms);
        ImVec2 avail = igGetContentRegionAvail();
        static float right_w;
        float right_def = igGetFontSize() * 24.0f;
        if (right_w <= 0.0f) right_w = right_def;
        float right = fminf(right_w, fmaxf(avail.x - 200.0f, 160.0f));
        canvas(avail.x - right - UI_SPLIT, avail.y, pts, ms);
        igSameLine(0, 0);
        ui_splitter("##pcg_split", true, &right_w, -1.0f, 160.0f, avail.x - 200.0f, avail.y, right_def);
        igSameLine(0, 0);
        igBeginChild_Str("pcg_right", (ImVec2){ right, avail.y }, ImGuiChildFlags_Borders, 0);
        details(pts, ms);
        igEndChild();
    }
    igEnd();
    if (!open) E.open = false;
}

bool pcg_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_FOLIAGE, TR("PCG"), ok)) return false;
    static const char* const ex[] = { "", "PCG_Forest", "PCG_Rocks", "PCG_Fence" };
    static const char* const labels[] = { "PCG Graph", "PCG Graph: Forest", "PCG Graph: Rocks", "PCG Graph: Fence" };
    for (int i = 0; i < 4; i++) {
        if (!ui_menu_item(ICON_FOLIAGE, TR(labels[i]), NULL, false, true)) continue;
        char src[300], vp[600];
        const PcgGraph* from = NULL;
        if (ex[i][0]) snprintf(src, sizeof src, "/Content/PCG/Examples/%s.rpcg", ex[i]), from = pcg_graph_find(src);
        if (pcg_graph_create(folder_vpath, i ? ex[i] : "PCG_NewGraph", from, vp, sizeof vp)) {
            pcg_editor_open(vp);
            made = true;
        }
    }
    igEndMenu();
    return made;
}

// --- The component's Details ---
static void row_label(const char* label) {
    igTableNextRow(0, 0);
    igTableNextColumn();
    igAlignTextToFramePadding();
    igTextUnformatted(TR(label), NULL);
    ui_tip_last_item(label);
    igTableNextColumn();
    igSetNextItemWidth(-1);
}
static bool flag_row(const char* label, uint32_t* flags, uint32_t bit) {
    row_label(label);
    igPushID_Str(label);
    bool b = (*flags & bit) != 0, ch = igCheckbox("##f", &b);
    igPopID();
    if (ch) *flags = b ? *flags | bit : *flags & ~bit;
    return ch;
}

bool pcg_details(World* w, Entity e) {
    CProcWorld* c = ecs_get(w, e, COMP_PROC_WORLD);
    if (!c || c->mode != PROCGEN_PCG) return false;
    if (!igCollapsingHeader_TreeNodeFlags(TR("PCG"), ImGuiTreeNodeFlags_DefaultOpen)) return false;
    ui_tip_last_item("PCG");
    bool ch = false;
    if (!igBeginTable("sec_pcg", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) return false;
    igTableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.5f, 0);
    igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f, 0); // i18n: none (a column id)
    row_label("Graph");
    const char* vp = pcg_graph_vpath(c->pcg.graph);
    const PcgGraph* g = pcg_graph_get(c->pcg.graph);
    vp = pcg_graph_vpath(c->pcg.graph);
    const char* sl = strrchr(vp, '/');
    char out[600];
    if (asset_field("##pcg_graph", vp[0] ? (sl ? sl + 1 : vp) : TR("None"), vp, ASSET_PCG_GRAPH, AK(ASSET_PCG_GRAPH), true, out, sizeof out) == ASSET_FIELD_PICKED) {
        if (out[0]) pcg_graph_remember(out), c->pcg.graph = pcg_vpath_hash(out);
        else c->pcg.graph = 0;
        ch = true;
    }
    if (g) {
        row_label("");
        if (igSmallButton(TR("Edit Graph"))) pcg_editor_open(g->vpath);
    }
    row_label("Seed");
    int seed = (int)c->pcg.seed;
    if (igDragInt("##pcg_seed", &seed, 1.0f, 0, INT_MAX, "%d", 0)) c->pcg.seed = (uint32_t)seed, ch = true;
    row_label("Volume Extent");
    ch |= igDragFloat3("##pcg_ext", c->pcg.extent, 10.0f, 0.0f, 1e7f, "%.0f cm", 0);
    row_label("Generation Trigger");
    {
        const char* t[PCG_TRIGGER_COUNT] = { TR(g_pcg_trigger_names[0]), TR(g_pcg_trigger_names[1]) };
        int v = (int)c->pcg.trigger;
        if (igCombo_Str_arr("##pcg_trig", &v, t, PCG_TRIGGER_COUNT, -1)) c->pcg.trigger = (uint32_t)v, ch = true;
    }
    ch |= flag_row("Generate On Change", &c->pcg.flags, PCGC_ON_CHANGE);
    ch |= flag_row("Partitioned", &c->pcg.flags, PCGC_PARTITIONED);
    if (c->pcg.flags & PCGC_PARTITIONED) {
        row_label("Cell Size");
        ch |= igDragFloat("##pcg_cell", &c->pcg.cell_size, 100.0f, 0.0f, 1e6f, c->pcg.cell_size > 0.0f ? "%.0f cm" : TR("World Partition's"), 0);
        row_label("Loading Range");
        ch |= igDragFloat("##pcg_range", &c->pcg.load_range, 100.0f, 0.0f, 1e7f, c->pcg.load_range > 0.0f ? "%.0f cm" : TR("World Partition's"), 0);
    }
    row_label("Spawn Budget");
    ch |= igDragFloat("##pcg_budget", &c->pcg.spawn_budget, 0.05f, 0.0f, 20.0f, c->pcg.spawn_budget > 0.0f ? "%.2f ms" : TR("World Partition's"), 0);
    ch |= flag_row("Show Points", &c->pcg.flags, PCGC_SHOW_POINTS);
    for (uint32_t k = 0; g && k < g->nparams; k++) { // the graph's parameters, set on this actor
        const PcgParam* p = &g->params[k];
        int o = -1;
        for (uint32_t i = 0; i < c->pcg.noverrides && i < 8; i++)
            if (!_stricmp(c->pcg.override_name[i], p->name)) o = (int)i;
        igTableNextRow(0, 0);
        igTableNextColumn();
        igAlignTextToFramePadding();
        igTextUnformatted(p->name, NULL); // (the graph's own names)
        ui_tip_last_item("Graph Parameters");
        igTableNextColumn();
        igPushID_Int((int)k);
        float v = o >= 0 ? c->pcg.override_value[o] : p->value;
        igSetNextItemWidth(o >= 0 ? -igGetFrameHeight() - 4.0f : -1.0f);
        if (igDragFloat("##ov", &v, fmaxf((p->mx - p->mn) * 0.002f, 0.0005f), p->mn, p->mx, "%.4g", 0)) {
            pcg_set_parameter(w, e, p->name, v);
            ch = true;
        }
        if (o >= 0) {
            igSameLine(0, 4);
            if (igButton("x##reset", (ImVec2){ igGetFrameHeight(), 0 })) {
                for (uint32_t i = (uint32_t)o; i + 1 < c->pcg.noverrides; i++)
                    memcpy(c->pcg.override_name[i], c->pcg.override_name[i + 1], 24), c->pcg.override_value[i] = c->pcg.override_value[i + 1];
                c->pcg.noverrides--, ch = true;
            }
            ui_tip_last_item("Reset to Default");
        }
        igPopID();
    }
    row_label("");
    if (igSmallButton(TR("Generate"))) pcg_generate(w, e), ch = true;
    igSameLine(0, 6);
    if (igSmallButton(TR("Cleanup"))) pcg_cleanup(w, e), ch = true;
    const PcgStats* st = pcg_stats();
    row_label("");
    igTextDisabled(TR("%u entities; last generation %.2f ms"), pcg_generated_count(w, e), st->last_gen_ms);
    igEndTable();
    return ch;
}

// --- The viewport ---
void pcg_draw_debug(ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg, float x0,
                    float y0, float x1, float y1, World* w, Entity selected) {
    float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f, cx = x0 + hw, cy = y0 + hh;
    float focal = hw / tanf(fmaxf(fov_deg, 1.0f) * 0.5f * 0.017453292f);
    ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
    // the points (the components with Show Points, the nodes marked Debug), at most 80 000 drawn
    uint32_t drawn = 0, n, cur = 0;
    const PcgDebugPoint* d;
    while ((d = pcg_debug_next(&cur, &n)) != NULL && drawn < 80000) {
        for (uint32_t i = 0; i < n && drawn < 80000; i++) {
            float q[3] = { d[i].p[0] - pos[0], d[i].p[1] - pos[1], d[i].p[2] - pos[2] };
            float zz = q[0] * fwd[0] + q[1] * fwd[1] + q[2] * fwd[2];
            if (zz < 10.0f || zz > 40000.0f) continue;
            float sx = cx + (q[0] * right[0] + q[1] * right[1] + q[2] * right[2]) / zz * focal, sy = cy - (q[0] * up[0] + q[1] * up[1] + q[2] * up[2]) / zz * focal;
            if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
            float r = fmaxf(1.5f, fminf(5.0f, 2400.0f / zz));
            ImDrawList_AddRectFilled(dl, (ImVec2){ sx - r, sy - r }, (ImVec2){ sx + r, sy + r }, d[i].color, 0.0f, 0);
            drawn++;
        }
    }
    // the selected PCG volume's box
    float mn[3], mx[3];
    if (selected && pcg_component_bounds(w, selected, mn, mx)) {
        float c8[8][3];
        for (int i = 0; i < 8; i++) c8[i][0] = (i & 1) ? mx[0] : mn[0], c8[i][1] = (i & 2) ? mx[1] : mn[1], c8[i][2] = (i & 4) ? mx[2] : mn[2];
        static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (int k = 0; k < 12; k++) {
            float s[2][2];
            bool ok = true;
            for (int j = 0; j < 2; j++) {
                const float* p = c8[edges[k][j]];
                float q[3] = { p[0] - pos[0], p[1] - pos[1], p[2] - pos[2] };
                float zz = q[0] * fwd[0] + q[1] * fwd[1] + q[2] * fwd[2];
                if (zz < 10.0f) ok = false;
                else s[j][0] = cx + (q[0] * right[0] + q[1] * right[1] + q[2] * right[2]) / zz * focal, s[j][1] = cy - (q[0] * up[0] + q[1] * up[1] + q[2] * up[2]) / zz * focal;
            }
            if (ok) ImDrawList_AddLine(dl, (ImVec2){ s[0][0], s[0][1] }, (ImVec2){ s[1][0], s[1][1] }, TH(TK_LIME), 1.5f);
        }
    }
    ImDrawList_PopClipRect(dl);
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
static const char* pcg_editor_docs_base(const char* vpath) { const char* b = strrchr(vpath, '/'); return b ? b + 1 : vpath; }
int pcg_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    (void)ed;
    if (!E.open) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) return E.open = false, E.unsaved = false, 1;
    if (!E.unsaved) return 0;
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, pcg_editor_docs_base(E.vpath)), 1;
    bool ok = pcg_graph_save(E.vpath, &E.g);
    if (ok) E.unsaved = false;
    return !ok;
}
