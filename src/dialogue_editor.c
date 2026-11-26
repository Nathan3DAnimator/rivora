// The Dialogue editor (dialogue.h): the conversation as a graph on a canvas (the Behavior Tree's and Blueprint's moves:
// pan with the right or middle button, zoom with the wheel, drag from a node's right pin to wire it, onto nothing: a new
// node from the menu), the selected node's Details on the right (texts in the three languages, conditions, actions) and
// a preview that runs the conversation inside the editor on a made-up player (its tags, items, health), showing the
// node it is on. Also the Dialogue Speaker component's Details.
#include "editor_docs.h"
#include "dialogue.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include "graph_style.h"

#include "blueprint.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "graph_view.h"
#include "graph_layout.h"
#include "i18n.h"
#include "platform.h"
#include "ui_icons.h"
#include "ui_tips.h"

#define NODE_W 230.0f

static struct {
    Dialogue* d;
    int sel, drag, wire_from;
    GraphView view;
    bool unsaved, previewing;
    DlgPreview pv;
    int lang;                  // the texts edited (the Details' tab) and the preview's language
    char status[120];
    char new_tag[DLG_NAME], new_item[DLG_NAME];
} E = { .sel = -1, .drag = -1, .wire_from = -1 };

static const uint32_t g_kind_colors[DLG_KIND_COUNT] = { TK_GREEN_D, TK_BLUE_D, TK_ORANGE, TK_AMBER, TK_MAGENTA, TK_RED };
static const char* const g_lang_names[DLG_LANGS] = { "English", "Português", "Español" };
// the kinds as the editor names them (the file keeps g_dlg_kind_names)
static const char* const g_kind_labels[DLG_KIND_COUNT] = { "Start", "Line", "Choice", "Option", "Jump to Node", "End" };

void dlg_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    Dialogue* d = dlg_load(vpath);
    if (!d) return;
    E.d = d, E.sel = -1, E.drag = -1, E.wire_from = -1, E.unsaved = false, E.previewing = false, E.status[0] = 0;
    E.view = (GraphView){ { 80.0f, 260.0f }, 1.0f, false };
}

bool dlg_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_GRAPH, TR("Dialogue"), ok)) return false;
    Dialogue* d = NULL;
    if (ui_menu_item(ICON_GRAPH, TR("Dialogue"), NULL, false, true)) d = dlg_create(folder_vpath, "DLG_New", false);
    igSetItemTooltip("%s", TR("A conversation: lines, the player's choices, conditions and actions. Give it to an NPC's Dialogue Speaker."));
    if (ui_menu_item(ICON_GRAPH, TR("Dialogue (Merchant Example)"), NULL, false, true)) d = dlg_create(folder_vpath, "DLG_Merchant", true);
    igSetItemTooltip("%s", TR("A merchant who greets, sells a potion for a coin (hidden without one), tells who he is once and says goodbye."));
    igEndMenu();
    if (d) {
        char vp[700];
        snprintf(vp, sizeof vp, "%s/%s.rdlg", folder_vpath, d->name);
        dlg_widget_asset(); // (WBP_Dialogue next to it in Content/UI)
        dlg_editor_open(vp);
        if (!E.d) E.d = d;
        made = true;
    }
    return made;
}

// --- The canvas ---

static int body_lines(const DlgNode* n); // (below)
// (2026-11-23) Organize Nodes (Q, the menu): the dialogue from its Start to the right, each answer under the other
// (graph_layout.c); a Jump's dashed wire left out (it goes back)
static bool organize_dialogue(Dialogue* d) {
    static GlNode nodes[DLG_MAX_NODES];
    static GlEdge edges[DLG_MAX_NODES * DLG_MAX_LINKS];
    int ne = 0;
    float fs = igGetFontSize();
    for (uint32_t i = 0; i < d->nnodes; i++) {
        nodes[i] = (GlNode){ d->nodes[i].x, d->nodes[i].y, NODE_W, fs + 8.0f + (float)body_lines(&d->nodes[i]) * (fs + 3.0f) + 8.0f };
        for (uint32_t k = 0; k < d->nodes[i].nlinks; k++) {
            int t = d->nodes[i].links[k];
            if (t > 0 && t < (int)d->nnodes) edges[ne++] = (GlEdge){ (int)i, t };
        }
    }
    if (!graph_layout(nodes, (int)d->nnodes, edges, ne, NULL, GL_TREE_RIGHT, 90.0f, 24.0f)) return false;
    for (uint32_t i = 0; i < d->nnodes; i++) d->nodes[i].x = roundf(nodes[i].x), d->nodes[i].y = roundf(nodes[i].y);
    return true;
}

static void node_title(const DlgNode* n, char* out, size_t cap) {
    switch (n->kind) {
    case DLG_LINE: snprintf(out, cap, "%s: %s", TR("Line"), !n->speaker[0] ? TR("NPC") : n->speaker); break;
    default: snprintf(out, cap, "%s", TR(g_kind_labels[n->kind % DLG_KIND_COUNT])); break;
    }
}
static int body_lines(const DlgNode* n) {
    int l = n->kind == DLG_START || n->kind == DLG_END ? 0 : 2;
    return l + n->nconds + n->nacts;
}
static void cond_words(const Dialogue* d, const DlgCond* c, char* out, size_t cap) {
    const char* who = c->who == DLG_WHO_SPEAKER ? TR("Speaker") : TR("Player");
    const char* nt = c->invert ? TR("not ") : "";
    switch (c->kind) {
    case DLC_HAS_TAG: snprintf(out, cap, "%s%s %s %s", nt, who, TR("has tag"), c->key); break;
    case DLC_HAS_ITEM: snprintf(out, cap, "%s%s %s %d %s", nt, who, TR("has"), c->value >= 1.0f ? (int)c->value : 1, c->key); break;
    case DLC_VARIABLE: snprintf(out, cap, "%s%s.%s %s %g", nt, who, c->key, g_dlg_op_names[c->op % DLG_OP_COUNT], c->value); break;
    case DLC_HEALTH: snprintf(out, cap, "%s%s %s %s %g%%", nt, who, TR("health"), g_dlg_op_names[c->op % DLG_OP_COUNT], c->value); break;
    case DLC_SAID: {
        int k = (int)c->value;
        snprintf(out, cap, "%s%s #%d", nt, TR("said before"), k);
        if (d && k >= 0 && k < (int)d->nnodes && d->nodes[k].text[0][0]) snprintf(out + strlen(out), cap - strlen(out), " \"%.20s\"", d->nodes[k].text[0]);
        break;
    }
    default: snprintf(out, cap, "-"); break;
    }
}
static void act_words(const DlgAct* a, char* out, size_t cap) {
    const char* who = a->who == DLG_WHO_SPEAKER ? TR("Speaker") : TR("Player");
    switch (a->kind) {
    case DLA_GIVE_ITEM: snprintf(out, cap, "%s %d %s -> %s", TR("Give"), a->value >= 1.0f ? (int)a->value : 1, a->key, who); break;
    case DLA_TAKE_ITEM: snprintf(out, cap, "%s %d %s <- %s", TR("Take"), a->value >= 1.0f ? (int)a->value : 1, a->key, who); break;
    case DLA_ADD_TAG: snprintf(out, cap, "+ %s (%s)", a->key, who); break;
    case DLA_REMOVE_TAG: snprintf(out, cap, "- %s (%s)", a->key, who); break;
    case DLA_EVENT: snprintf(out, cap, "%s.%s(%s%s%s)", who, a->key, a->arg[0], a->arg[1][0] ? ", " : "", a->arg[1]); break;
    case DLA_SET_VAR: snprintf(out, cap, "%s.%s = %s", who, a->key, a->arg[0]); break;
    case DLA_OPEN_SHOP: snprintf(out, cap, "%s %s", TR("Open Shop"), a->key); break;
    default: snprintf(out, cap, "-"); break;
    }
}

static void add_menu_items(Dialogue* d, float gx, float gy, int link_from, bool* ch) {
    for (int k = DLG_LINE; k < DLG_KIND_COUNT; k++) {
        if (link_from >= 0 && d->nodes[link_from].kind != DLG_CHOICE && k == DLG_OPTION) continue; // (Options hang under a Choice)
        if (link_from >= 0 && d->nodes[link_from].kind == DLG_CHOICE && k != DLG_OPTION && k != DLG_LINE) continue;
        if (!ui_menu_item(ICON_PLUS, TR(g_kind_labels[k]), NULL, false, d->nnodes < DLG_MAX_NODES)) continue;
        int n = dlg_add_node(d, (uint8_t)k, gx, gy);
        if (n >= 0) {
            if (link_from >= 0) dlg_link(d, link_from, n);
            if (k == DLG_OPTION) snprintf(d->nodes[n].text[0], DLG_TEXT, "%s", "...");
            E.sel = n, *ch = true;
        }
    }
}

static bool canvas(Dialogue* d, float w, float h) {
    bool ch = false;
    GraphView* v = &E.view;
    igBeginChild_Str("dlg_canvas", (ImVec2){ w, h }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    igInvisibleButton("dlg_bg", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool hovered = igIsItemHovered(0);
    ImGuiIO* io = igGetIO_Nil();
    gv_input(v, o, hovered || v->panning);
    gv_grid(v, dl, o, sz, TH(TK_N6));
    float z = v->zoom, fs = igGetFontSize() * fmaxf(z, 0.6f);
    // the dragged node (2026-11-23: applied before drawing; after it the nodes and their wires showed a frame behind the mouse)
    if (igIsItemActive() && igIsMouseDown_Nil(ImGuiMouseButton_Left) && E.drag >= 0 && E.wire_from < 0 && (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        d->nodes[E.drag].x += io->MouseDelta.x / z, d->nodes[E.drag].y += io->MouseDelta.y / z;
        ch = true;
    }
    { // (2026-11-23) Blender's edge pan while a node or a wire is dragged
        float es[2];
        bool node = igIsItemActive() && E.drag >= 0 && E.drag < (int)d->nnodes && E.wire_from < 0;
        if ((node || E.wire_from >= 0) && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(o, sz, es)) {
            v->pan[0] -= es[0] / z, v->pan[1] -= es[1] / z;
            if (node) d->nodes[E.drag].x += es[0] / z, d->nodes[E.drag].y += es[1] / z;
        }
    }
    const ImGuiStyle* st = igGetStyle();
    igPushFont(NULL, roundf((st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f) * fmaxf(z, 0.6f))); // (whole pixels: a new size per zoom step bakes glyphs and waits on the GPU)
    float bw = NODE_W * z, title = fs + 8.0f * z, row = fs + 3.0f * z, pr = 6.0f * z;
    ImVec2 p0[DLG_MAX_NODES], p1[DLG_MAX_NODES];
    for (uint32_t i = 0; i < d->nnodes; i++) {
        ImVec2 c = gv_screen(v, o, d->nodes[i].x, d->nodes[i].y);
        float hh = title + body_lines(&d->nodes[i]) * row + 8.0f * z;
        p0[i] = c, p1[i] = (ImVec2){ c.x + bw, c.y + hh };
    }
    int shown = E.previewing ? E.pv.node : -1;
    // the wires: from a node's right pin to the next one's left; a Jump's to its target dashed
    for (uint32_t i = 0; i < d->nnodes; i++) {
        const DlgNode* n = &d->nodes[i];
        ImVec2 a = { p1[i].x, p0[i].y + title * 0.5f };
        for (uint32_t k = 0; k < n->nlinks; k++) {
            int t = n->links[k];
            if (t <= 0 || t >= (int)d->nnodes) continue;
            ImVec2 b = { p0[t].x, p0[t].y + title * 0.5f };
            ImU32 col = (int)i == E.sel || t == E.sel ? TH(TK_AMBER) : TH(TK_N14);
            graph_wire(dl, a, b, col, z, false, -1); // (Editor Preferences > Graph Style)
            if (n->nlinks > 1 && n->kind != DLG_CHOICE) { // (the order they are tried in)
                char num[8];
                snprintf(num, sizeof num, "%u", k + 1);
                ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 6 * z, a.y - fs - 2 * z + k * 2.0f }, TH(TK_N13), num, NULL);
            }
        }
        if (n->kind == DLG_JUMP && n->jump > 0 && n->jump < (int)d->nnodes) {
            ImVec2 b = { p0[n->jump].x + bw * 0.5f, p0[n->jump].y };
            ImVec2 s0 = { (p0[i].x + p1[i].x) * 0.5f, p1[i].y };
            for (int q = 0; q < 24; q += 2) { // dashed
                float t0 = q / 24.0f, t1 = (q + 1) / 24.0f;
                ImDrawList_AddLine(dl, (ImVec2){ s0.x + (b.x - s0.x) * t0, s0.y + (b.y - s0.y) * t0 }, (ImVec2){ s0.x + (b.x - s0.x) * t1, s0.y + (b.y - s0.y) * t1 },
                                   TH(TK_MAGENTA_L), 1.6f * z);
            }
            ImDrawList_AddCircleFilled(dl, b, 4 * z, TH(TK_MAGENTA_L), 12);
        }
    }
    // the nodes
    int hover = -1, hover_pin = -1;
    char line[256];
    int lang = E.lang;
    for (uint32_t i = 0; i < d->nnodes; i++) {
        const DlgNode* n = &d->nodes[i];
        ImVec2 a = p0[i], b = p1[i];
        if (b.x < o.x - 20 || b.y < o.y || a.x > o.x + sz.x + 20 || a.y > o.y + sz.y) continue;
        ImVec2 m = io->MousePos;
        ImVec2 pin = { b.x, a.y + title * 0.5f };
        bool on_pin = hovered && n->kind != DLG_END && n->kind != DLG_JUMP && fabsf(m.x - pin.x) < pr * 1.8f && fabsf(m.y - pin.y) < pr * 1.8f;
        bool in = hovered && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
        if (on_pin) hover = (int)i, hover_pin = 1;
        else if (in) hover = (int)i, hover_pin = 0;
        bool orbit = graph_orbit();
        if (orbit) // (Q5) a card with its kind's band and icon
            graph_card(dl, a, b, title, TH(g_kind_colors[n->kind % DLG_KIND_COUNT]), n->kind == DLG_OPTION ? ICON_FLOW : n->kind == DLG_END ? ICON_STOP
                       : n->kind == DLG_START ? ICON_PLAY : ICON_DIALOGUE, (int)i == E.sel, (int)i == hover, false, z);
        else {
            for (int s = 3; s >= 1; s--)
                ImDrawList_AddRectFilled(dl, (ImVec2){ a.x - s * z, a.y - s * z + 3 * z }, (ImVec2){ b.x + s * z, b.y + s * z + 3 * z }, THA(TK_N0, 30), 6 * z, 0);
            ImDrawList_AddRectFilled(dl, a, b, THA(TK_N4, 240), 6 * z, 0);
            ImDrawList_AddRectFilled(dl, a, (ImVec2){ b.x, a.y + title }, TH(g_kind_colors[n->kind % DLG_KIND_COUNT]), 6 * z, ImDrawFlags_RoundCornersTop);
        }
        node_title(n, line, sizeof line);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ orbit ? graph_card_title_x(a, title, 0, z) : a.x + 8 * z, a.y + 4 * z }, TH(TK_NODE_TITLE), line, NULL);
        snprintf(line, sizeof line, "#%u", i);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ b.x - igCalcTextSize(line, NULL, false, -1).x - 8 * z, a.y + 4 * z }, THA(TK_NODE_TITLE, 176), line, NULL);
        float y = a.y + title + 4 * z;
        if (n->kind != DLG_START && n->kind != DLG_END) {
            const char* t = n->kind == DLG_JUMP ? "" : dlg_node_text(d, n, lang);
            if (n->kind == DLG_JUMP) {
                int j = n->jump;
                snprintf(line, sizeof line, "-> #%d %s", j, j > 0 && j < (int)d->nnodes ? TR(g_kind_labels[d->nodes[j].kind]) : TR("(none)"));
                t = line;
            }
            ImVec4 clip = { a.x + 6 * z, y, b.x - 6 * z, y + row * 2.0f };
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 8 * z, y }, n->kind == DLG_OPTION ? TH(TK_CYAN_L) : TH(TK_N14), t[0] ? t : TR("(no text)"), NULL,
                                       bw - 16 * z, &clip);
            y += row * 2.0f;
        }
        for (uint32_t k = 0; k < n->nconds; k++, y += row) {
            cond_words(d, &n->conds[k], line, sizeof line);
            ImVec4 clip = { a.x + 6 * z, y, b.x - 6 * z, y + row };
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 8 * z, y }, TH(TK_AMBER_L), "?", NULL, 0.0f, &clip);
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 20 * z, y }, TH(TK_AMBER_L), line, NULL, 0.0f, &clip);
        }
        for (uint32_t k = 0; k < n->nacts; k++, y += row) {
            act_words(&n->acts[k], line, sizeof line);
            ImVec4 clip = { a.x + 6 * z, y, b.x - 6 * z, y + row };
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 8 * z, y }, TH(TK_LIME), "!", NULL, 0.0f, &clip);
            ImDrawList_AddText_FontPtr(dl, NULL, 0.0f, (ImVec2){ a.x + 20 * z, y }, TH(TK_LIME_L), line, NULL, 0.0f, &clip);
        }
        if (n->kind != DLG_START) ImDrawList_AddCircleFilled(dl, (ImVec2){ a.x, a.y + title * 0.5f }, pr, TH(TK_N14), 12); // in
        if (n->kind != DLG_END && n->kind != DLG_JUMP) {
            ImDrawList_AddCircleFilled(dl, pin, pr, (int)i == hover && hover_pin ? TH(TK_AMBER) : TH(TK_N14), 12); // out
        }
        bool sel = (int)i == E.sel, live = (int)i == shown;
        ImU32 border = live ? TH(TK_GREEN) : sel ? TH(TK_ACCENT) : (int)i == hover ? TH(TK_N13) : TH(TK_N10);
        if (!orbit || live) ImDrawList_AddRect(dl, a, b, border, (orbit ? 8 : 6) * z, live || sel ? 2.5f : 1.2f, 0);
        if (n->once) ImDrawList_AddText_Vec2(dl, (ImVec2){ b.x - 46 * z, b.y - fs - 2 * z }, TH(TK_ORANGE_L), TR("once"), NULL);
    }
    // clicks: a node selected and dragged; its right pin: a wire
    if (igIsItemActivated() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (hover >= 0 && hover_pin == 1) E.wire_from = hover;
        else E.sel = hover, E.drag = hover;
    }
    static float menu_g[2];
    static int menu_from = -1, menu_node = -1;
    if (E.wire_from >= 0) {
        int f = E.wire_from;
        ImVec2 a = { p1[f].x, p0[f].y + title * 0.5f };
        ImDrawList_AddLine(dl, a, io->MousePos, TH(TK_AMBER), 2.5f * z);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            if (hover >= 0 && hover != f) {
                if (dlg_link(d, f, hover)) ch = true;
                else snprintf(E.status, sizeof E.status, "%s", TR("Not wired: an End and a Jump have no next, and a node has 8 links at most."));
            } else if (hover < 0) {
                gv_graph(v, o, io->MousePos, &menu_g[0], &menu_g[1]);
                menu_from = f;
                igOpenPopup_Str("dlg_add", 0);
            }
            E.wire_from = -1;
        }
    }
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) E.drag = -1;
    if (hovered && gv_right_click()) {
        gv_graph(v, o, io->MousePos, &menu_g[0], &menu_g[1]);
        menu_node = hover;
        if (hover >= 0) E.sel = hover, igOpenPopup_Str("dlg_node", 0);
        else menu_from = -1, igOpenPopup_Str("dlg_add", 0);
    }
    igPopFont();
    if (igBeginPopup("dlg_add", 0)) {
        igTextDisabled("%s", menu_from >= 0 ? TR("Add and wire") : TR("Add Node"));
        add_menu_items(d, menu_g[0], menu_g[1], menu_from, &ch);
        igEndPopup();
    }
    if (igBeginPopup("dlg_node", 0)) {
        DlgNode* n = menu_node >= 0 && menu_node < (int)d->nnodes ? &d->nodes[menu_node] : NULL;
        if (n) {
            if (n->kind != DLG_END && n->kind != DLG_JUMP && ui_begin_menu(ICON_PLUS, TR("Add Next"), n->nlinks < DLG_MAX_LINKS)) {
                add_menu_items(d, n->x + NODE_W + 80.0f, n->y + n->nlinks * 120.0f, menu_node, &ch);
                igEndMenu();
            }
            if (n->nlinks && ui_menu_item(ICON_CROSS, TR("Break Links"), NULL, false, true)) n->nlinks = 0, ch = true;
            if (menu_node > 0 && ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, d->nnodes < DLG_MAX_NODES)) {
                int c = dlg_add_node(d, n->kind, n->x + 40.0f, n->y + 60.0f);
                if (c >= 0) {
                    DlgNode copy = d->nodes[menu_node];
                    copy.x += 40.0f, copy.y += 60.0f, copy.nlinks = 0;
                    d->nodes[c] = copy, E.sel = c, ch = true;
                }
            }
            if (menu_node > 0 && ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, true)) dlg_remove_node(d, menu_node), E.sel = -1, ch = true;
            if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, d->nnodes > 1)) ch |= organize_dialogue(d);
        }
        igEndPopup();
    }
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Delete, false) && E.sel > 0) {
        dlg_remove_node(d, E.sel), E.sel = -1, ch = true;
    }
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false))
        ch |= organize_dialogue(d); // (2026-11-23)
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, o.y + sz.y - igGetFontSize() - 6 }, TH(TK_N12),
                            TR("Drag from a node's right pin to wire it (on nothing: a new node) | right click: add / delete | right or middle drag: pan | wheel: zoom"), NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 12, o.y + 8 }, TH(TK_N13), TR("DIALOGUE"), NULL);
    igEndChild();
    return ch;
}

// --- The Details ---

static bool name_field(const char* label, char* s, size_t cap, const char* hint) {
    return igInputTextWithHint(PL(label), hint ? hint : "", s, cap, 0, NULL, NULL);
}

static bool cond_ui(Dialogue* d, DlgCond* c, int k) {
    bool ch = false;
    igPushID_Int(k);
    int kind = c->kind;
    const char* kinds[DLC_KIND_COUNT];
    for (int i = 0; i < DLC_KIND_COUNT; i++) kinds[i] = TR(g_dlg_cond_names[i]);
    if (igCombo_Str_arr(PL(TR("Condition")), &kind, kinds, DLC_KIND_COUNT, -1)) c->kind = (uint8_t)kind, ch = true;
    if (c->kind != DLC_SAID) {
        int who = c->who;
        const char* whos[2] = { TR("Player"), TR("Speaker") };
        if (igCombo_Str_arr(PL(TR("Checked On")), &who, whos, 2, -1)) c->who = (uint8_t)who, ch = true;
    }
    switch (c->kind) {
    case DLC_HAS_TAG: ch |= name_field(TR("Gameplay Tag"), c->key, sizeof c->key, "Quest.Started"); break;
    case DLC_HAS_ITEM: {
        ch |= name_field(TR("Item"), c->key, sizeof c->key, "DA_Coin");
        int n = c->value >= 1.0f ? (int)c->value : 1;
        if (igDragInt(PL(TR("At Least")), &n, 0.1f, 1, 9999, "%d", 0)) c->value = (float)n, ch = true;
        break;
    }
    case DLC_VARIABLE:
    case DLC_HEALTH: {
        if (c->kind == DLC_VARIABLE) ch |= name_field(TR("Variable"), c->key, sizeof c->key, "Reputation");
        int op = c->op;
        if (igCombo_Str_arr(PL(TR("Is")), &op, g_dlg_op_names, DLG_OP_COUNT, -1)) c->op = (uint8_t)op, ch = true;
        ch |= igDragFloat(PL(c->kind == DLC_HEALTH ? TR("Percent") : TR("Value")), &c->value, 0.1f, -1e6f, 1e6f, "%g", 0);
        break;
    }
    case DLC_SAID: {
        int n = (int)c->value;
        char cur[96];
        snprintf(cur, sizeof cur, "#%d %.40s", n, n >= 0 && n < (int)d->nnodes ? d->nodes[n].text[0] : "");
        if (igBeginCombo(PL(TR("Node")), cur, 0)) {
            for (uint32_t i = 1; i < d->nnodes; i++) {
                char lab[96];
                snprintf(lab, sizeof lab, "#%u %s %.40s##%u", i, TR(g_kind_labels[d->nodes[i].kind]), d->nodes[i].text[0], i);
                if (igSelectable_Bool(lab, (int)i == n, 0, (ImVec2){ 0, 0 })) c->value = (float)i, ch = true;
            }
            igEndCombo();
        }
        break;
    }
    default: break;
    }
    ch |= igCheckbox(PL(TR("Not")), &c->invert);
    igPopID();
    return ch;
}

static bool act_ui(DlgAct* a, int k) {
    bool ch = false;
    igPushID_Int(100 + k);
    int kind = a->kind;
    const char* kinds[DLA_KIND_COUNT];
    for (int i = 0; i < DLA_KIND_COUNT; i++) kinds[i] = TR(g_dlg_act_names[i]);
    if (igCombo_Str_arr(PL(TR("Action")), &kind, kinds, DLA_KIND_COUNT, -1)) a->kind = (uint8_t)kind, ch = true;
    if (a->kind != DLA_OPEN_SHOP) {
        int who = a->who;
        const char* whos[2] = { TR("Player"), TR("Speaker") };
        if (igCombo_Str_arr(PL(TR("Applies To")), &who, whos, 2, -1)) a->who = (uint8_t)who, ch = true;
    }
    switch (a->kind) {
    case DLA_GIVE_ITEM:
    case DLA_TAKE_ITEM: {
        ch |= name_field(TR("Item"), a->key, sizeof a->key, "DA_Potion");
        int n = a->value >= 1.0f ? (int)a->value : 1;
        if (igDragInt(PL(TR("Count")), &n, 0.1f, 1, 9999, "%d", 0)) a->value = (float)n, ch = true;
        break;
    }
    case DLA_ADD_TAG:
    case DLA_REMOVE_TAG: ch |= name_field(TR("Gameplay Tag"), a->key, sizeof a->key, "Quest.Started"); break;
    case DLA_EVENT:
        ch |= name_field(TR("Custom Event"), a->key, sizeof a->key, "OnQuestAccepted");
        ch |= name_field(TR("Input 1"), a->arg[0], sizeof a->arg[0], TR("(text, a number, true)"));
        ch |= name_field(TR("Input 2"), a->arg[1], sizeof a->arg[1], NULL);
        break;
    case DLA_SET_VAR:
        ch |= name_field(TR("Variable"), a->key, sizeof a->key, "Reputation");
        ch |= name_field(TR("Value"), a->arg[0], sizeof a->arg[0], "10");
        break;
    case DLA_OPEN_SHOP:
        ch |= name_field(TR("Shop Widget"), a->key, sizeof a->key, "WBP_Shop");
        igTextDisabled("%s", TR("The widget goes on the screen; the speaker's Custom Event OpenShop runs too."));
        break;
    default: break;
    }
    igPopID();
    return ch;
}

static bool details(Dialogue* d) {
    bool ch = false;
    if (E.sel < 0 || E.sel >= (int)d->nnodes) { // the asset's own settings
        igSeparatorText(TR("Dialogue"));
        ch |= igDragFloat(PL(TR("Letters per Second")), &d->chars_per_second, 1.0f, 0.0f, 400.0f, d->chars_per_second > 0 ? "%.0f" : TR("all at once"), 0);
        bool p = d->pause != 0;
        if (igCheckbox(PL(TR("Pause the Game")), &p)) d->pause = p, ch = true;
        ch |= name_field(TR("Player Name"), d->player_name, sizeof d->player_name, TR("You"));
        igTextWrapped("%s", TR("Pick a node to edit it. Lines follow the first link whose node passes its conditions; a Choice shows each Option that passes."));
        return ch;
    }
    DlgNode* n = &d->nodes[E.sel];
    igSeparatorText(TR(g_kind_labels[n->kind % DLG_KIND_COUNT]));
    if (n->kind == DLG_LINE) ch |= name_field(TR("Speaker"), n->speaker, sizeof n->speaker, TR("(the NPC; Player: the player)"));
    if (n->kind == DLG_LINE || n->kind == DLG_CHOICE || n->kind == DLG_OPTION) {
        if (igBeginTabBar("langs", 0)) {
            for (int l = 0; l < DLG_LANGS; l++) {
                if (!igBeginTabItem(g_lang_names[l], NULL, 0)) continue;
                E.lang = l;
                const char* hint = l == 0 ? TR("The text (English: the msgid)") : TR("(empty: the English text, or its translation in the engine's table)");
                igSetNextItemWidth(-1);
                ch |= igInputTextMultiline("##text", n->text[l], DLG_TEXT, (ImVec2){ -1, igGetFontSize() * 4.5f }, 0, NULL, NULL);
                if (!n->text[l][0]) igTextDisabled("%s", hint);
                igEndTabItem();
            }
            igEndTabBar();
        }
    }
    if (n->kind == DLG_CHOICE && !n->text[0][0]) igTextDisabled("%s", TR("No text: the last line (the question) stays on the screen."));
    if (n->kind == DLG_LINE) {
        bool wait = n->wait != 0;
        if (igCheckbox(PL(TR("Wait for the Player")), &wait)) n->wait = wait, ch = true;
        if (!wait) ch |= igDragFloat(PL(TR("Duration")), &n->duration, 0.05f, 0.0f, 60.0f, n->duration > 0 ? "%.2f s" : TR("the voice's / 2 s"), 0);
        ch |= name_field(TR("Voice"), n->voice, sizeof n->voice, "/Content/Audio/Hello.rsound");
        ch |= name_field(TR("Animation"), n->anim, sizeof n->anim, TR("(an Anim Montage or Sequence)"));
    }
    if (n->kind == DLG_OPTION) {
        bool once = n->once != 0;
        if (igCheckbox(PL(TR("Choose Only Once")), &once)) n->once = once, ch = true;
    }
    if (n->kind == DLG_JUMP) {
        char cur[96];
        snprintf(cur, sizeof cur, "#%d", n->jump);
        if (igBeginCombo(PL(TR("Jump To")), cur, 0)) {
            for (uint32_t i = 1; i < d->nnodes; i++) {
                if ((int)i == E.sel) continue;
                char lab[96];
                snprintf(lab, sizeof lab, "#%u %s %.40s##%u", i, TR(g_kind_labels[d->nodes[i].kind]), d->nodes[i].text[0], i);
                if (igSelectable_Bool(lab, (int)i == n->jump, 0, (ImVec2){ 0, 0 })) n->jump = (int16_t)i, ch = true;
            }
            igEndCombo();
        }
    }
    if (n->kind != DLG_START) {
        igSeparatorText(TR("Conditions"));
        for (uint32_t k = 0; k < n->nconds; k++) {
            ch |= cond_ui(d, &n->conds[k], (int)k);
            igPushID_Int(200 + (int)k);
            if (igSmallButton(TR("Remove"))) {
                memmove(&n->conds[k], &n->conds[k + 1], sizeof(DlgCond) * (n->nconds - k - 1)), n->nconds--, ch = true;
                igPopID();
                break;
            }
            igPopID();
            igSeparator();
        }
        if (n->nconds < DLG_MAX_CONDS && igSmallButton(TR("+ Condition"))) {
            memset(&n->conds[n->nconds], 0, sizeof(DlgCond));
            n->conds[n->nconds].kind = DLC_HAS_TAG, n->conds[n->nconds].value = 1.0f, n->nconds++, ch = true;
        }
    }
    igSeparatorText(TR("Actions"));
    for (uint32_t k = 0; k < n->nacts; k++) {
        ch |= act_ui(&n->acts[k], (int)k);
        igPushID_Int(300 + (int)k);
        if (igSmallButton(TR("Remove"))) {
            memmove(&n->acts[k], &n->acts[k + 1], sizeof(DlgAct) * (n->nacts - k - 1)), n->nacts--, ch = true;
            igPopID();
            break;
        }
        igPopID();
        igSeparator();
    }
    if (n->nacts < DLG_MAX_ACTS && igSmallButton(TR("+ Action"))) {
        memset(&n->acts[n->nacts], 0, sizeof(DlgAct));
        n->acts[n->nacts].kind = DLA_GIVE_ITEM, n->acts[n->nacts].value = 1.0f, n->nacts++, ch = true;
    }
    igSeparatorText(TR("Comment"));
    igSetNextItemWidth(-1);
    ch |= igInputText("##comment", n->comment, sizeof n->comment, 0, NULL, NULL);
    return ch;
}

// --- The preview ---

static void preview(Dialogue* d, float h) {
    DlgPreview* p = &E.pv;
    igBeginChild_Str("dlg_preview", (ImVec2){ 0, h }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Preview"));
    if (!E.previewing) {
        if (igButton(TR("Play the Conversation"), (ImVec2){ -1, 0 })) {
            p->lang = E.lang, p->health = p->health > 0.0f ? p->health : 100.0f;
            dlg_preview_start(p, d), E.previewing = true;
        }
        igTextDisabled("%s", TR("The made-up player (the conditions read it, the actions change it):"));
    } else {
        p->d = d;
        dlg_preview_tick(p, igGetIO_Nil()->DeltaTime);
        int sn = dlg_shown_node(p);
        if (p->node < 0) igTextDisabled("%s", TR("(the conversation ended)"));
        else if (sn >= 0) {
            const DlgNode* n = &d->nodes[sn];
            const char* who = !n->speaker[0] ? TR("NPC") : n->speaker;
            igTextColored(THV(TK_WARN), "%s", who);
            const char* t = dlg_node_text(d, n, p->lang);
            uint32_t want = p->shown >= 1e8f ? UINT32_MAX : (uint32_t)p->shown, b = 0;
            while (t[b] && want) { // (the letters shown so far)
                b++;
                while (((uint8_t)t[b] & 0xC0u) == 0x80u) b++;
                want--;
            }
            char buf[DLG_TEXT];
            snprintf(buf, sizeof buf, "%.*s", (int)b, t);
            igTextWrapped("%s", buf);
            if (p->choosing) {
                for (int i = 0; i < p->nopts; i++) {
                    char lab[DLG_TEXT + 16];
                    snprintf(lab, sizeof lab, "%d. %s##opt%d", i + 1, dlg_node_text(d, &d->nodes[p->opts[i]], p->lang), i);
                    if (igButton(lab, (ImVec2){ -1, 0 })) dlg_preview_choose(p, i);
                }
            } else if (igButton(TR("Continue"), (ImVec2){ -1, 0 }))
                dlg_preview_skip(p);
        }
        if (igButton(TR("Restart"), (ImVec2){ 0, 0 })) dlg_preview_start(p, d);
        igSameLine(0, 6);
        if (igButton(TR("Stop"), (ImVec2){ 0, 0 })) E.previewing = false;
    }
    int lang = p->lang;
    if (igCombo_Str_arr(PL(TR("Language")), &lang, g_lang_names, DLG_LANGS, -1)) p->lang = lang, E.lang = lang;
    igDragFloat(PL(TR("Health")), &p->health, 1.0f, 0.0f, 100.0f, "%.0f%%", 0);
    // the made-up player's tags and items
    for (int i = 0; i < 8; i++) {
        if (!p->tags[i][0]) continue;
        igPushID_Int(500 + i);
        if (igSmallButton("x")) p->tags[i][0] = 0;
        igSameLine(0, 4), igText("%s %s", TR("Tag"), p->tags[i]);
        igPopID();
    }
    for (int i = 0; i < 8; i++) {
        if (!p->items[i][0]) continue;
        igPushID_Int(600 + i);
        if (igSmallButton("x")) p->items[i][0] = 0;
        igSameLine(0, 4), igText("%d x %s", p->counts[i], p->items[i]);
        igPopID();
    }
    igSetNextItemWidth(igGetContentRegionAvail().x * 0.6f);
    igInputTextWithHint("##ntag", "Quest.Started", E.new_tag, sizeof E.new_tag, 0, NULL, NULL);
    igSameLine(0, 4);
    if (igSmallButton(TR("+ Tag")) && E.new_tag[0])
        for (int i = 0; i < 8; i++)
            if (!p->tags[i][0]) {
                snprintf(p->tags[i], DLG_NAME, "%s", E.new_tag), E.new_tag[0] = 0;
                break;
            }
    igSetNextItemWidth(igGetContentRegionAvail().x * 0.6f);
    igInputTextWithHint("##nitem", "DA_Coin", E.new_item, sizeof E.new_item, 0, NULL, NULL); // i18n: none (an asset name)
    igSameLine(0, 4);
    if (igSmallButton(TR("+ Item")) && E.new_item[0]) {
        for (int i = 0; i < 8; i++)
            if (p->items[i][0] && !_stricmp(p->items[i], E.new_item)) {
                p->counts[i]++, E.new_item[0] = 0;
                break;
            }
        for (int i = 0; i < 8 && E.new_item[0]; i++)
            if (!p->items[i][0]) {
                snprintf(p->items[i], DLG_NAME, "%s", E.new_item), p->counts[i] = 1, E.new_item[0] = 0;
                break;
            }
    }
    if (p->nlog) {
        igSeparatorText(TR("What happened"));
        for (int i = 0; i < p->nlog; i++) igTextDisabled("%s", p->log[i]);
    }
    igEndChild();
}

void dlg_editor_draw(struct Editor* ed) {
    (void)ed;
    if (!E.d) return;
    Dialogue* d = E.d;
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###dlg_editor", d->name, E.unsaved ? "*" : "", TR("Dialogue"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.78f, mv->Size.y * 0.78f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.11f, mv->Pos.y + mv->Size.y * 0.09f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true, ch = false;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = dlg_save(d);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        igSameLine(0, 10);
        igTextDisabled("%s", TR("Lines, the player's choices, conditions and actions. Start Dialogue on an NPC with a Dialogue Speaker runs it."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        ImVec2 avail = igGetContentRegionAvail();
        static float right_w; // (the bar between the graph and the right drags it; double-click: back)
        float right_def = igGetFontSize() * 22.0f;
        if (right_w <= 0.0f) right_w = right_def;
        float right = fminf(right_w, fmaxf(avail.x - 200.0f, 160.0f));
        ch |= canvas(d, avail.x - right - UI_SPLIT, avail.y);
        igSameLine(0, 0);
        ui_splitter("##dlg_split", true, &right_w, -1.0f, 160.0f, avail.x - 200.0f, avail.y, right_def);
        igSameLine(0, 0);
        igBeginChild_Str("dlg_right", (ImVec2){ right, avail.y }, 0, 0);
        float ph = avail.y * 0.42f;
        igBeginChild_Str("dlg_details", (ImVec2){ 0, avail.y - ph - 6.0f }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-igGetFontSize() * 7.5f);
        ch |= details(d);
        igPopItemWidth();
        igEndChild();
        igPushItemWidth(-igGetFontSize() * 6.0f);
        preview(d, ph);
        igPopItemWidth();
        igEndChild();
    }
    igEnd();
    if (ch) E.unsaved = true, d->version++;
    if (!open) E.d = NULL, E.previewing = false;
}

void dlg_editor_test_preview(void) {
    if (!E.d) return;
    E.pv.lang = E.lang, E.pv.health = 100.0f;
    dlg_preview_start(&E.pv, E.d), E.previewing = true;
}

// --- The Dialogue Speaker's Details ---

bool dlg_ui_speaker(CDialogueSpeaker* s) {
    bool ch = false;
    Dialogue* cur = dlg_find(s->dialogue);
    if (igBeginCombo(PL(TR("Dialogue")), cur ? cur->name : s->dialogue ? "?" : TR("None"), 0)) {
        if (igSelectable_Bool(TR("None"), !s->dialogue, 0, (ImVec2){ 0, 0 })) s->dialogue = 0, ch = true;
        for (uint32_t i = 0; i < dlg_count(); i++) {
            Dialogue* d = dlg_at(i);
            if (igSelectable_Bool(d->name, d->hash == s->dialogue, 0, (ImVec2){ 0, 0 })) s->dialogue = d->hash, ch = true;
        }
        if (!dlg_count()) igTextDisabled("%s", TR("No Dialogue yet: Content Browser > Add > Dialogue."));
        igEndCombo();
    }
    if (cur && igSmallButton(TR("Edit Dialogue"))) {
        char vp[700];
        snprintf(vp, sizeof vp, "%s", cur->path);
        const char* root = content_root();
        size_t rl = strlen(root);
        if (rl && !strncmp(cur->path, root, rl)) snprintf(vp, sizeof vp, "/Content%s", cur->path + rl);
        dlg_editor_open(vp);
    }
    ch |= igInputTextWithHint(PL(TR("Display Name")), TR("(the actor's name)"), s->display_name, sizeof s->display_name, 0, NULL, NULL);
    int pause = (int)s->pause;
    const char* pauses[3] = { TR("As the Dialogue says"), TR("Pause"), TR("Don't Pause") };
    if (igCombo_Str_arr(PL(TR("Pause the Game")), &pause, pauses, 3, -1)) s->pause = (uint32_t)pause, ch = true;
    bool face = (s->flags & DLG_SPK_FACE) != 0, cam = (s->flags & DLG_SPK_CAMERA) != 0;
    if (igCheckbox(PL(TR("Face the Player")), &face)) s->flags = face ? s->flags | DLG_SPK_FACE : s->flags & ~DLG_SPK_FACE, ch = true;
    if (igCheckbox(PL(TR("Conversation Camera")), &cam)) s->flags = cam ? s->flags | DLG_SPK_CAMERA : s->flags & ~DLG_SPK_CAMERA, ch = true;
    if (cam) {
        ch |= igDragFloat(PL(TR("Camera Distance")), &s->cam_distance, 1.0f, 50.0f, 2000.0f, "%.0f cm", 0);
        ch |= igDragFloat(PL(TR("Camera Height")), &s->cam_height, 1.0f, -200.0f, 400.0f, "%.0f cm", 0);
        ch |= igDragFloat(PL(TR("Camera Angle")), &s->cam_angle, 0.5f, -180.0f, 180.0f, "%.0f deg", 0);
    }
    static char wname[64];
    static uint32_t shown = UINT32_MAX;
    static const CDialogueSpeaker* shown_for;
    if (shown != s->widget || shown_for != s) { // (the field keeps what is typed until Enter)
        BpAsset* wb = s->widget ? bp_find_hash(s->widget) : NULL;
        snprintf(wname, sizeof wname, "%s", wb ? wb->name : "");
        shown = s->widget, shown_for = s;
    }
    if (igInputTextWithHint(PL(TR("Widget")), "WBP_Dialogue", wname, sizeof wname, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) { // i18n: none (an asset name)
        BpAsset* f = wname[0] ? bp_find_name(BPA_CLASS, wname) : NULL;
        s->widget = f ? f->hash : 0u, ch = true;
    }
    igSetItemTooltip("%s", TR("A Widget Blueprint of yours with the same widgets' names (Speaker, Line, Option1..6, Continue); empty: WBP_Dialogue."));
    return ch;
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
int dlg_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    (void)ed;
    if (!E.d) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) return E.d = NULL, E.previewing = false, E.unsaved = false, 1;
    if (!E.unsaved) return 0;
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, E.d->name), 1;
    bool ok = dlg_save(E.d);
    if (ok) E.unsaved = false;
    return !ok;
}
