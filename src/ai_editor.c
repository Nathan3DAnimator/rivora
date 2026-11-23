// The AI editors (ai_editor.h): Unreal's Behavior Tree Editor (the Blackboard on the left, the tree top-down on a
// canvas, Details on the right) and the Real Brain's (its Behaviors, their Considerations with the curves drawn, the
// scores live while playing). Also the AI component's Details.
#include "ai_editor.h"
#include "theme.h"
#include "content.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "ai.h"
#include "graph_view.h"
#include "graph_menu.h"
#include "components.h"
#include "i18n.h"
#include "ui_icons.h"
#include "ui_tips.h"

static struct {
    BehaviorTree* tree;
    Brain* brain;
    int sel, drag, behavior;
    float pan[2];
    GraphView view;
    int sub, sub_i;      // the selected node's part: 0 the node, 1 a decorator, 2 a service (its index)
    int wire_from;       // a wire being dragged from this node (-1: none)
    bool wire_up;        // ... from its top bar (a new parent for it)
    bool bb_mode;        // the Blackboard mode (Unreal's toolbar: Behavior Tree / Blackboard)
    bool unsaved;
    char status[120];
} E = { .sel = -1, .drag = -1, .wire_from = -1 };

static void auto_arrange(BehaviorTree* t);
static bool overlapping(const BehaviorTree* t);

void ai_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    const char* dot = strrchr(vpath, '.');
    if (!dot) return;
    E.tree = NULL, E.brain = NULL, E.sel = -1, E.behavior = 0, E.unsaved = false, E.status[0] = 0;
    E.pan[0] = 400, E.pan[1] = 40;
    E.view = (GraphView){ { 500.0f, 40.0f }, 1.0f, false }, E.sub = 0, E.wire_from = -1;
    if (!strcmp(dot, ".rbt")) E.tree = ai_tree_load(vpath);
    if (E.tree && overlapping(E.tree)) auto_arrange(E.tree); // (the first trees were laid out for smaller nodes)
    else if (!strcmp(dot, ".rbrain")) E.brain = ai_brain_load(vpath);
}

bool ai_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_GRAPH, TR("Artificial Intelligence"), ok)) return false;
    if (ui_menu_item(ICON_GRAPH, TR("Behavior Tree"), NULL, false, true)) {
        BehaviorTree* t = ai_tree_create(folder_vpath, "BT_New");
        if (t) E.tree = t, E.brain = NULL, E.sel = -1, made = true;
    }
    if (ui_menu_item(ICON_SLIDERS, TR("Real Brain (utility AI)"), NULL, false, true)) {
        Brain* b = ai_brain_create(folder_vpath, "Brain_New");
        if (b) E.brain = b, E.tree = NULL, made = true;
    }
    igEndMenu();
    return made;
}

// --- The Blackboard (both editors) ---

static bool blackboard(BbKey* keys, uint32_t* n) {
    bool ch = false;
    igSeparatorText(TR("Blackboard"));
    for (uint32_t i = 0; i < *n; i++) {
        igPushID_Int((int)i);
        igSetNextItemWidth(igGetContentRegionAvail().x * 0.55f);
        ch |= igInputText("##bb_key", keys[i].name, AI_NAME, 0, NULL, NULL);
        igSameLine(0, 4);
        igSetNextItemWidth(igGetContentRegionAvail().x - igGetFrameHeight() - 4);
        int t = keys[i].type;
        if (igCombo_Str_arr("##bb_type", &t, g_bb_type_names, BB_TYPE_COUNT, -1)) keys[i].type = (uint8_t)t, ch = true;
        igSameLine(0, 4);
        if (igSmallButton("x")) {
            memmove(&keys[i], &keys[i + 1], sizeof(BbKey) * (*n - i - 1));
            (*n)--, ch = true;
            igPopID();
            break;
        }
        igPopID();
    }
    if (*n < AI_MAX_KEYS && igButton(TR("New Key"), (ImVec2){ 0, 0 })) {
        snprintf(keys[*n].name, AI_NAME, "Key%u", *n);
        keys[(*n)++].type = BB_FLOAT, ch = true;
    }
    igTextDisabled("%s", TR("Perception writes TargetActor, CanSeeTarget, LastKnownLocation, DistanceToTarget, HeardNoise, NoiseLocation, SelfLocation."));
    return ch;
}

static bool key_combo(const char* label, char* key, const BbKey* keys, uint32_t n) {
    bool ch = false;
    if (igBeginCombo(label, key[0] ? key : "-", 0)) {
        for (uint32_t i = 0; i < n; i++)
            if (igSelectable_Bool(keys[i].name, !strcmp(key, keys[i].name), 0, (ImVec2){ 0, 0 })) snprintf(key, AI_NAME, "%s", keys[i].name), ch = true;
        igEndCombo();
    }
    return ch;
}

// --- The Behavior Tree Editor (Unreal's: top-down, decorators in blue over a node, services in green, the order of the
// children numbered left to right; wires dragged from a node's bottom bar to another's top) ---

static uint32_t node_color(uint8_t kind) {
    if (kind == BT_ROOT) return TH(TK_N6);
    if (kind == BT_SELECTOR || kind == BT_SEQUENCE) return TH(TK_N10); // composites: grey
    return TH(TK_VIOLET);                                                   // tasks: purple
}
static bool is_composite(uint8_t kind) { return kind == BT_ROOT || kind == BT_SELECTOR || kind == BT_SEQUENCE; }

// The node goes; its children stay, not wired (Unreal's)
static void delete_node(BehaviorTree* t, int n) {
    if (n <= 0 || n >= (int)t->nnodes) return;
    int map[AI_MAX_NODES];
    uint32_t k = 0;
    for (uint32_t i = 0; i < t->nnodes; i++) map[i] = (int)i == n ? -1 : (int)k++;
    k = 0;
    for (uint32_t i = 0; i < t->nnodes; i++) {
        if ((int)i == n) continue;
        BtNode nd = t->nodes[i];
        if (nd.parent == n) nd.parent = BT_DETACHED;
        else if (nd.parent >= 0) nd.parent = (int16_t)map[nd.parent];
        t->nodes[k++] = nd;
    }
    t->nnodes = k;
}

static bool is_ancestor(const BehaviorTree* t, int a, int n) { // a above n
    for (int g = 0; n >= 0 && g < AI_MAX_NODES; g++) {
        if (n == a) return true;
        n = t->nodes[n].parent;
    }
    return false;
}

// The wire from a parent's bottom to a child: child under parent (the root takes one child: the old one comes loose)
static bool wire_under(BehaviorTree* t, int parent, int child) {
    if (parent < 0 || child <= 0 || parent == child || !is_composite(t->nodes[parent].kind) || is_ancestor(t, child, parent)) return false;
    if (parent == 0)
        for (uint32_t i = 1; i < t->nnodes; i++)
            if (t->nodes[i].parent == 0 || t->nodes[i].parent == -1) t->nodes[i].parent = BT_DETACHED;
    t->nodes[child].parent = (int16_t)parent;
    return true;
}

// The children's run order (Unreal's numbers on the nodes): depth first, each parent's children left to right
static void run_order(const BehaviorTree* t, int n, int* order, int* next) {
    int kids[AI_MAX_NODES], nk = 0;
    for (uint32_t i = 1; i < t->nnodes; i++) {
        int p = t->nodes[i].parent == -1 ? 0 : t->nodes[i].parent;
        if (p != n) continue;
        int k = nk++;
        while (k > 0 && t->nodes[kids[k - 1]].x > t->nodes[i].x) kids[k] = kids[k - 1], k--;
        kids[k] = (int)i;
    }
    for (int i = 0; i < nk; i++) {
        order[kids[i]] = (*next)++;
        run_order(t, kids[i], order, next);
    }
}

// Auto Arrange: each subtree as wide as its children, the children centered under their parent, 210 units apart
static float arrange(BehaviorTree* t, int n, float left, float y) {
    int kids[AI_MAX_NODES], nk = 0;
    for (uint32_t i = 1; i < t->nnodes; i++) {
        int p = t->nodes[i].parent == -1 ? 0 : t->nodes[i].parent;
        if (p != n) continue;
        int k = nk++;
        while (k > 0 && t->nodes[kids[k - 1]].x > t->nodes[i].x) kids[k] = kids[k - 1], k--;
        kids[k] = (int)i;
    }
    float h = 90.0f + 26.0f * (float)(t->nodes[n].ndecos + t->nodes[n].nsvcs);
    float x = left;
    for (int i = 0; i < nk; i++) x = arrange(t, kids[i], x, y + h);
    float width = nk ? x - left : 210.0f;
    t->nodes[n].x = left + width * 0.5f, t->nodes[n].y = y;
    return left + width;
}
static void auto_arrange(BehaviorTree* t) {
    float right = arrange(t, 0, -400.0f, 0.0f);
    for (uint32_t i = 1; i < t->nnodes; i++) // the loose ones in a row under the rest
        if (t->nodes[i].parent == BT_DETACHED) t->nodes[i].y = 900.0f, t->nodes[i].x = right += 220.0f;
}
static bool overlapping(const BehaviorTree* t) {
    for (uint32_t i = 0; i < t->nnodes; i++)
        for (uint32_t k = i + 1; k < t->nnodes; k++)
            if (fabsf(t->nodes[i].x - t->nodes[k].x) < 195.0f && fabsf(t->nodes[i].y - t->nodes[k].y) < 60.0f) return true;
    return false;
}

static BtNode* add_bt_node(BehaviorTree* t, uint8_t kind, int parent, float x, float y) {
    if (t->nnodes >= AI_MAX_NODES) return NULL;
    BtNode* n = &t->nodes[t->nnodes];
    memset(n, 0, sizeof *n);
    n->kind = kind, n->parent = BT_DETACHED, n->x = x, n->y = y;
    if (kind == BT_WAIT) n->value = 1.0f;
    if (kind == BT_MOVE_TO) n->value = 60.0f;
    if (kind == BT_FIND_RANDOM) n->value = 600.0f;
    E.sel = (int)t->nnodes++;
    E.sub = 0;
    if (parent >= 0) wire_under(t, parent, E.sel);
    return n;
}

static bool add_decorator(BtNode* n, uint8_t kind) {
    if (n->ndecos >= BT_MAX_DECOS || n->kind == BT_ROOT) return false;
    BtDeco* d = &n->decos[n->ndecos++];
    memset(d, 0, sizeof *d);
    d->kind = kind, d->aborts = BTA_BOTH;
    if (kind == BTD_COOLDOWN || kind == BTD_TIME_LIMIT) d->value = 5.0f;
    if (kind == BTD_LOOP) d->value = 3.0f;
    E.sub = 1, E.sub_i = n->ndecos - 1;
    return true;
}
static bool add_service(BtNode* n, uint8_t kind) {
    if (n->nsvcs >= BT_MAX_SVCS || n->kind == BT_ROOT) return false;
    BtService* s = &n->svcs[n->nsvcs++];
    memset(s, 0, sizeof *s);
    s->kind = kind, s->value = kind == BTS_SPEED ? 0.5f : 0.0f;
    E.sub = 2, E.sub_i = n->nsvcs - 1;
    return true;
}

static const char* deco_line(const BtDeco* d, char* buf, size_t cap) {
    static const char* const ops[5] = { "is set", "is not set", ">", "<", "==" };
    switch (d->kind) {
    case BTD_BLACKBOARD:
        if (d->op >= BB_GREATER) snprintf(buf, cap, "%s%s %s %.2g", d->inverse ? "NOT " : "", d->key[0] ? d->key : "?", ops[d->op % 5], d->value);
        else snprintf(buf, cap, "%s%s %s", d->inverse ? "NOT " : "", d->key[0] ? d->key : "?", ops[d->op % 5]);
        break;
    case BTD_COOLDOWN: snprintf(buf, cap, "Cooldown %.2g s", d->value); break;
    case BTD_LOOP: snprintf(buf, cap, d->value > 0 ? "Loop %.0f x" : "Loop forever", d->value); break;
    case BTD_TIME_LIMIT: snprintf(buf, cap, "Time Limit %.2g s", d->value); break;
    default: snprintf(buf, cap, "%s", g_btd_names[d->kind % BTD_KIND_COUNT]); break;
    }
    return buf;
}

// The menus' recents and recommendations (graph_menu.h): a kind by its name; under the root only a composite
static int bt_kind_named(const char* name) {
    for (int k = BT_SELECTOR; k < BT_KIND_COUNT; k++)
        if (!strcmp(g_bt_kind_names[k], name)) return k;
    return -1;
}
typedef struct BtAccept { int parent; const char* shown[2]; } BtAccept;
static bool bt_accept(const char* item, void* user) {
    const BtAccept* u = user;
    int k = bt_kind_named(item);
    if (k < 0 || (u->parent == 0 && k > BT_SEQUENCE)) return false;
    return (!u->shown[0] || strcmp(u->shown[0], item)) && (!u->shown[1] || strcmp(u->shown[1], item));
}

static bool bb_node_menu(BehaviorTree* t, int target, float gx, float gy, int parent) {
    bool ch = false;
    // what is being done: the node the new one goes under
    const char* ctx = parent >= 0 && parent < (int)t->nnodes && t->nodes[parent].kind != BT_ROOT ? g_bt_kind_names[t->nodes[parent].kind] : "";
    BtAccept u = { parent, { NULL, NULL } };
    const char* items[8];
    int n = gm_recent(GM_BEHAVIOR_TREE, items, 8), ns = 0;
    for (int i = 0; i < n && ns < GM_RECENT_SHOWN; i++) {
        if (!bt_accept(items[i], &u)) continue;
        if (!ns) igSeparatorText(gm_title_recent());
        igPushID_Str("btr"), igPushID_Int(ns);
        if (ui_menu_item(ICON_FUNCTION, TR(items[i]), NULL, false, t->nnodes < AI_MAX_NODES))
            gm_note(GM_BEHAVIOR_TREE, ctx, items[i]), add_bt_node(t, (uint8_t)bt_kind_named(items[i]), parent, gx, gy), ch = true;
        igPopID(), igPopID();
        u.shown[ns++] = items[i];
    }
    n = gm_recommend(GM_BEHAVIOR_TREE, ctx, bt_accept, &u, items, GM_RECOMMEND_SHOWN);
    for (int i = 0; i < n && !ch; i++) {
        if (!i) igSeparatorText(gm_title_recommended());
        igPushID_Str("btm"), igPushID_Int(i);
        if (ui_menu_item(ICON_FUNCTION, TR(items[i]), NULL, false, t->nnodes < AI_MAX_NODES))
            gm_note(GM_BEHAVIOR_TREE, ctx, items[i]), add_bt_node(t, (uint8_t)bt_kind_named(items[i]), parent, gx, gy), ch = true;
        igPopID(), igPopID();
    }
    if (ch) return true;
    igSeparatorText(TR("Composites"));
    for (int k = BT_SELECTOR; k <= BT_SEQUENCE; k++)
        if (ui_menu_item(ICON_GRAPH, TR(g_bt_kind_names[k]), NULL, false, t->nnodes < AI_MAX_NODES))
            gm_note(GM_BEHAVIOR_TREE, ctx, g_bt_kind_names[k]), add_bt_node(t, (uint8_t)k, parent, gx, gy), ch = true;
    if (parent != 0) {
        igSeparatorText(TR("Tasks"));
        for (int k = BT_MOVE_TO; k < BT_KIND_COUNT; k++)
            if (ui_menu_item(ICON_FUNCTION, TR(g_bt_kind_names[k]), NULL, false, t->nnodes < AI_MAX_NODES))
                gm_note(GM_BEHAVIOR_TREE, ctx, g_bt_kind_names[k]), add_bt_node(t, (uint8_t)k, parent, gx, gy), ch = true;
    }
    (void)target;
    return ch;
}

// (2026-11-23) Organize Nodes (Q, the canvas's menu): the tree's own Auto Arrange (its children left to right in the
// order they run)
static bool bt_organize(BehaviorTree* t) {
    auto_arrange(t);
    return true;
}

static bool tree_canvas(BehaviorTree* t, float w, float h) {
    bool ch = false;
    GraphView* v = &E.view;
    igBeginChild_Str("bt_canvas", (ImVec2){ w, h }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    igInvisibleButton("bt_bg", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool hovered = igIsItemHovered(0);
    ImGuiIO* io = igGetIO_Nil();
    gv_input(v, o, hovered || v->panning);
    gv_grid(v, dl, o, sz, TH(TK_N6)); // (after the input: the grid at this frame's zoom, like the nodes)
    float z = v->zoom, fs = igGetFontSize() * fmaxf(z, 0.6f);
    // the dragged node (2026-11-23: applied before drawing; after it the nodes and their wires showed a frame behind the mouse)
    if (igIsItemActive() && igIsMouseDown_Nil(ImGuiMouseButton_Left) && E.drag > 0 && E.wire_from < 0 &&
        (io->MouseDelta.x != 0.0f || io->MouseDelta.y != 0.0f)) {
        t->nodes[E.drag].x += io->MouseDelta.x / z, t->nodes[E.drag].y += io->MouseDelta.y / z;
        ch = true;
    }
    { // (2026-11-23) Blender's edge pan while a node or a wire is dragged
        float es[2];
        bool node = igIsItemActive() && E.drag > 0 && E.drag < (int)t->nnodes && E.wire_from < 0;
        if ((node || E.wire_from >= 0) && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(o, sz, es)) {
            v->pan[0] -= es[0] / z, v->pan[1] -= es[1] / z;
            if (node) t->nodes[E.drag].x += es[0] / z, t->nodes[E.drag].y += es[1] / z;
        }
    }
    const ImGuiStyle* st = igGetStyle();
    igPushFont(NULL, roundf((st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f) * fmaxf(z, 0.6f))); // (whole pixels: a new size per zoom step bakes glyphs and waits on the GPU)
    float bw = 190.0f * z, row = fs + 8.0f * z, strip = 9.0f * z, title = fs * 2.2f + 8.0f * z;
    // each node's box: strip (in), decorators, the body, services, strip (out)
    ImVec2 p0[AI_MAX_NODES], p1[AI_MAX_NODES];
    float body_y[AI_MAX_NODES];
    for (uint32_t i = 0; i < t->nnodes; i++) {
        const BtNode* n = &t->nodes[i];
        ImVec2 c = gv_screen(v, o, n->x, n->y);
        float hh = strip + n->ndecos * row + title + n->nsvcs * row + strip;
        p0[i] = (ImVec2){ c.x - bw * 0.5f, c.y }, p1[i] = (ImVec2){ c.x + bw * 0.5f, c.y + hh };
        body_y[i] = c.y + strip + n->ndecos * row;
    }
    int order[AI_MAX_NODES], next = 1;
    for (uint32_t i = 0; i < t->nnodes; i++) order[i] = 0;
    run_order(t, 0, order, &next);
    // the wires: from a parent's bottom bar to each child's top (Unreal's square elbows)
    for (uint32_t i = 1; i < t->nnodes; i++) {
        if (t->nodes[i].parent == BT_DETACHED) continue;
        int p = t->nodes[i].parent < 0 ? 0 : t->nodes[i].parent;
        ImVec2 a = { (p0[p].x + p1[p].x) * 0.5f, p1[p].y }, b = { (p0[i].x + p1[i].x) * 0.5f, p0[i].y };
        float my = (a.y + b.y) * 0.5f;
        ImU32 col = (int)i == E.sel ? TH(TK_AMBER) : TH(TK_N14);
        ImDrawList_AddLine(dl, a, (ImVec2){ a.x, my }, col, 2.0f * z);
        ImDrawList_AddLine(dl, (ImVec2){ a.x, my }, (ImVec2){ b.x, my }, col, 2.0f * z);
        ImDrawList_AddLine(dl, (ImVec2){ b.x, my }, b, col, 2.0f * z);
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ b.x - 5 * z, b.y - 7 * z }, (ImVec2){ b.x + 5 * z, b.y - 7 * z }, b, col);
    }
    // the nodes; what is under the mouse (the node, a decorator / service in it, its bars)
    int hover = -1, hover_sub = 0, hover_i = 0, hover_bar = 0; // bar: 1 top, 2 bottom
    for (uint32_t i = 0; i < t->nnodes; i++) {
        const BtNode* n = &t->nodes[i];
        ImVec2 a = p0[i], b = p1[i];
        if (b.x < o.x || b.y < o.y || a.x > o.x + sz.x || a.y > o.y + sz.y) continue;
        bool sel = (int)i == E.sel;
        ImVec2 m = io->MousePos;
        bool in = m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y && hovered;
        if (in) {
            hover = (int)i, hover_sub = 0;
            if (m.y < a.y + strip) hover_bar = 1;
            else if (m.y > b.y - strip) hover_bar = 2;
            else if (m.y < body_y[i]) hover_sub = 1, hover_i = (int)((m.y - a.y - strip) / row);
            else if (m.y >= body_y[i] + title) hover_sub = 2, hover_i = (int)((m.y - body_y[i] - title) / row);
        }
        if (graph_orbit()) // (Q5) a card of the engine's look: its halo when selected
            graph_card(dl, a, b, 0.0f, 0, -1, sel && E.sub == 0, (int)i == hover, n->parent == BT_DETACHED && n->kind != BT_ROOT, z);
        else {
            for (int s = 3; s >= 1; s--) // the shadow
                ImDrawList_AddRectFilled(dl, (ImVec2){ a.x - s * z, a.y - s * z + 3 * z }, (ImVec2){ b.x + s * z, b.y + s * z + 3 * z }, THA(TK_N0, 30), 6 * z, 0);
            ImDrawList_AddRectFilled(dl, a, b, TH(TK_N4), 6 * z, 0);
        }
        // the bars: in (not the root), out (composites)
        if (n->kind != BT_ROOT) ImDrawList_AddRectFilled(dl, a, (ImVec2){ b.x, a.y + strip }, in && hover_bar == 1 ? TH(TK_N11) : TH(TK_N8), 6 * z, ImDrawFlags_RoundCornersTop);
        if (is_composite(n->kind)) ImDrawList_AddRectFilled(dl, (ImVec2){ a.x, b.y - strip }, b, in && hover_bar == 2 ? TH(TK_N11) : TH(TK_N9), 6 * z, ImDrawFlags_RoundCornersBottom);
        char line[160];
        // decorators (blue)
        for (uint32_t d = 0; d < n->ndecos; d++) {
            float y = a.y + strip + d * row;
            bool ds = sel && E.sub == 1 && E.sub_i == (int)d;
            ImDrawList_AddRectFilled(dl, (ImVec2){ a.x + 4 * z, y + 2 * z }, (ImVec2){ b.x - 4 * z, y + row - 1 * z }, ds ? TH(TK_BLUE) : TH(TK_BLUE_D), 3 * z, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 10 * z, y + (row - fs) * 0.5f }, TH(TK_N15), deco_line(&n->decos[d], line, sizeof line), NULL);
            if (n->decos[d].kind == BTD_BLACKBOARD && n->decos[d].aborts != BTA_NONE) // the abort mark (Unreal's)
                ImDrawList_AddText_Vec2(dl, (ImVec2){ b.x - 22 * z, y + (row - fs) * 0.5f }, TH(TK_AMBER_L), n->decos[d].aborts == BTA_BOTH ? "<>" : n->decos[d].aborts == BTA_SELF ? "|" : "<", NULL);
        }
        // the body: icon, name, what it does
        ImU32 body = node_color(n->kind);
        ImDrawList_AddRectFilled(dl, (ImVec2){ a.x + 2 * z, body_y[i] }, (ImVec2){ b.x - 2 * z, body_y[i] + title }, body, 4 * z, 0);
        float is = fs * 1.1f;
        ui_icon(dl, n->kind == BT_ROOT ? ICON_BEHAVIOR_TREE : is_composite(n->kind) ? ICON_FLOW : ICON_FUNCTION, a.x + 8 * z, body_y[i] + 5 * z, is, TH(TK_N15));
        const char* name = n->kind == BT_ROOT ? "ROOT" : TR(g_bt_kind_names[n->kind]);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 12 * z + is, body_y[i] + 4 * z }, TH(TK_N15), name, NULL);
        snprintf(line, sizeof line, "%s", n->key[0] ? n->key : n->text[0] ? n->text : n->comment);
        if (n->kind == BT_WAIT) snprintf(line, sizeof line, "%.2g s%s", n->value, n->value2 > 0 ? " +/-" : "");
        if (n->kind == BT_SELECTOR) snprintf(line, sizeof line, "%s", n->comment[0] ? n->comment : TR("the first child that works"));
        if (n->kind == BT_SEQUENCE) snprintf(line, sizeof line, "%s", n->comment[0] ? n->comment : TR("each child in turn"));
        ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 12 * z + is, body_y[i] + 4 * z + fs }, TH(TK_PINK_L), line, NULL);
        // services (green)
        for (uint32_t s = 0; s < n->nsvcs; s++) {
            float y = body_y[i] + title + s * row;
            bool ss = sel && E.sub == 2 && E.sub_i == (int)s;
            const BtService* sv = &n->svcs[s];
            if (sv->kind == BTS_SPEED) snprintf(line, sizeof line, "%s %.2f", TR(g_bts_names[sv->kind]), sv->value);
            else snprintf(line, sizeof line, "%s %s", TR(g_bts_names[sv->kind % BTS_KIND_COUNT]), sv->key);
            ImDrawList_AddRectFilled(dl, (ImVec2){ a.x + 4 * z, y + 2 * z }, (ImVec2){ b.x - 4 * z, y + row - 1 * z }, ss ? TH(TK_GREEN) : TH(TK_GREEN_D), 3 * z, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ a.x + 10 * z, y + (row - fs) * 0.5f }, TH(TK_N15), line, NULL);
        }
        ImU32 border = sel && E.sub == 0 ? TH(TK_ACCENT) : (int)i == hover ? TH(TK_N13) : n->parent == BT_DETACHED ? TH(TK_ORANGE) : TH(TK_N10);
        if (!graph_orbit()) ImDrawList_AddRect(dl, a, b, border, 6 * z, sel && E.sub == 0 ? 2.5f : 1.2f, 0);
        if (order[i]) { // the run order
            ImVec2 c = { b.x - 2 * z, a.y + 2 * z };
            ImDrawList_AddCircleFilled(dl, c, 9 * z, TH(TK_N5), 16);
            ImDrawList_AddCircle(dl, c, 9 * z, TH(TK_N13), 16, 1.0f);
            snprintf(line, sizeof line, "%d", order[i]);
            ImVec2 ts = igCalcTextSize(line, NULL, false, -1);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ c.x - ts.x * 0.5f, c.y - ts.y * 0.5f }, TH(TK_N15), line, NULL);
        }
    }
    // clicks: a node / its decorator or service selected; a bar: a wire; drags move the node
    if (igIsItemActivated() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (hover >= 0 && hover_bar == 2 && is_composite(t->nodes[hover].kind)) E.wire_from = hover, E.wire_up = false;
        else if (hover > 0 && hover_bar == 1) {
            if (io->KeyAlt) t->nodes[hover].parent = BT_DETACHED, ch = true; // Alt + click: the wire breaks
            else E.wire_from = hover, E.wire_up = true;
        } else {
            E.sel = hover, E.drag = hover, E.sub = hover >= 0 ? hover_sub : 0, E.sub_i = hover_i;
            if (E.sel >= 0 && E.sub == 1 && E.sub_i >= t->nodes[E.sel].ndecos) E.sub = 0;
            if (E.sel >= 0 && E.sub == 2 && E.sub_i >= t->nodes[E.sel].nsvcs) E.sub = 0;
        }
    }
    static float menu_g[2];
    static int menu_parent = -1, menu_node = -1;
    if (E.wire_from >= 0) { // the wire being dragged
        int f = E.wire_from;
        ImVec2 a = E.wire_up ? (ImVec2){ (p0[f].x + p1[f].x) * 0.5f, p0[f].y } : (ImVec2){ (p0[f].x + p1[f].x) * 0.5f, p1[f].y };
        ImDrawList_AddLine(dl, a, io->MousePos, TH(TK_AMBER), 2.5f * z);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            if (!E.wire_up) { // from a parent: onto a node (its child now) or onto nothing (a new child, from the menu)
                if (hover > 0) ch |= wire_under(t, f, hover);
                else if (hover < 0) {
                    gv_graph(v, o, io->MousePos, &menu_g[0], &menu_g[1]);
                    menu_parent = f;
                    igOpenPopup_Str("bt_add", 0);
                }
            } else { // from a child's top: onto a composite (its new parent) or nothing (loose)
                if (hover >= 0 && hover != f) ch |= wire_under(t, hover, f);
                else if (hover < 0) t->nodes[f].parent = BT_DETACHED, ch = true;
            }
            E.wire_from = -1;
        }
    }
    if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) E.drag = -1;
    if (hovered && gv_right_click()) {
        gv_graph(v, o, io->MousePos, &menu_g[0], &menu_g[1]);
        menu_node = hover;
        if (hover >= 0) E.sel = hover, E.sub = 0, igOpenPopup_Str("bt_node_menu", 0);
        else menu_parent = -1, igOpenPopup_Str("bt_add", 0);
    }
    igPopFont();
    if (igBeginPopup("bt_add", 0)) {
        if (menu_parent < 0 && ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, t->nnodes > 1)) ch |= bt_organize(t), igCloseCurrentPopup();
        ch |= bb_node_menu(t, -1, menu_g[0], menu_g[1], menu_parent);
        igEndPopup();
    }
    if (igBeginPopup("bt_node_menu", 0)) {
        BtNode* n = menu_node >= 0 && menu_node < (int)t->nnodes ? &t->nodes[menu_node] : NULL;
        if (n && n->kind != BT_ROOT) {
            if (ui_begin_menu(ICON_PLUS, TR("Add Decorator..."), n->ndecos < BT_MAX_DECOS)) {
                for (int k = BTD_BLACKBOARD; k < BTD_KIND_COUNT; k++)
                    if (ui_menu_item(ICON_FILTER, TR(g_btd_names[k]), NULL, false, true)) ch |= add_decorator(n, (uint8_t)k);
                igEndMenu();
            }
            if (ui_begin_menu(ICON_PLUS, TR("Add Service..."), n->nsvcs < BT_MAX_SVCS)) {
                for (int k = BTS_FOCUS; k < BTS_KIND_COUNT; k++)
                    if (ui_menu_item(ICON_REFRESH, TR(g_bts_names[k]), NULL, false, true)) ch |= add_service(n, (uint8_t)k);
                igEndMenu();
            }
            if (is_composite(n->kind) && ui_begin_menu(ICON_PLUS, TR("Add Child"), true)) {
                ch |= bb_node_menu(t, menu_node, n->x, n->y + 160.0f, menu_node);
                igEndMenu();
            }
            igSeparator();
            if (n->parent != BT_DETACHED && ui_menu_item(ICON_CROSS, TR("Break Node Link"), "Alt+Click", false, true)) n->parent = BT_DETACHED, ch = true;
            if (ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, true)) delete_node(t, menu_node), E.sel = -1, ch = true;
        } else if (n) {
            if (ui_begin_menu(ICON_PLUS, TR("Add Child"), true)) {
                ch |= bb_node_menu(t, 0, n->x, n->y + 160.0f, 0);
                igEndMenu();
            }
        }
        igEndPopup();
    }
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false))
        ch |= bt_organize(t); // (2026-11-23)
    // Delete: the selected decorator / service, or the node
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Delete, false) && E.sel > 0 &&
        E.sel < (int)t->nnodes) {
        BtNode* n = &t->nodes[E.sel];
        if (E.sub == 1 && E.sub_i < n->ndecos) memmove(&n->decos[E.sub_i], &n->decos[E.sub_i + 1], sizeof(BtDeco) * (n->ndecos - E.sub_i - 1)), n->ndecos--;
        else if (E.sub == 2 && E.sub_i < n->nsvcs) memmove(&n->svcs[E.sub_i], &n->svcs[E.sub_i + 1], sizeof(BtService) * (n->nsvcs - E.sub_i - 1)), n->nsvcs--;
        else delete_node(t, E.sel), E.sel = -1;
        E.sub = 0, ch = true;
    }
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Home, false))
        v->pan[0] = sz.x * 0.5f / v->zoom, v->pan[1] = 40.0f / v->zoom;
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, o.y + sz.y - igGetFontSize() - 6 }, TH(TK_N12),
                            TR("Drag from a node's bottom bar to wire a child (on nothing: a new one) | right click: add / decorators / services | right or middle drag: pan | wheel: zoom | Home"), NULL);
    char zl[32];
    snprintf(zl, sizeof zl, "Zoom %.0f%%", z * 100.0f);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + sz.x - igCalcTextSize(zl, NULL, false, -1).x - 12, o.y + 8 }, TH(TK_N12), zl, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 12, o.y + 8 }, TH(TK_N13), "BEHAVIOR TREE", NULL);
    igEndChild();
    return ch;
}

static bool deco_details(BehaviorTree* t, BtDeco* d) {
    bool ch = false;
    igSeparatorText(TR(g_btd_names[d->kind % BTD_KIND_COUNT]));
    if (d->kind == BTD_BLACKBOARD) {
        static const char* const ops[5] = { "Is Set", "Is Not Set", ">", "<", "==" };
        static const char* const aborts[4] = { "None", "Self", "Lower Priority", "Both" };
        ch |= key_combo(PL(TR("Blackboard Key")), d->key, t->keys, t->nkeys);
        int op = d->op;
        if (igCombo_Str_arr(PL(TR("Key Query")), &op, ops, 5, -1)) d->op = (uint8_t)op, ch = true;
        if (d->op >= BB_GREATER) ch |= igDragFloat(PL(TR("Value")), &d->value, 0.1f, 0, 0, "%.2f", 0);
        int ab = d->aborts;
        if (igCombo_Str_arr(PL(TR("Observer aborts")), &ab, aborts, 4, -1)) d->aborts = (uint8_t)ab, ch = true;
        ch |= igCheckbox(PL(TR("Inverse Condition")), &d->inverse);
        igTextDisabled("%s", TR("Self: the branch stops when the condition fails. Lower Priority: this branch takes over when it holds."));
    } else if (d->kind == BTD_COOLDOWN) ch |= igDragFloat(PL(TR("Cool Down Time")), &d->value, 0.05f, 0.0f, 0.0f, "%.2f s", 0);
    else if (d->kind == BTD_LOOP) ch |= igDragFloat(PL(TR("Number of Loops (0: forever)")), &d->value, 1.0f, 0.0f, 0.0f, "%.0f", 0);
    else if (d->kind == BTD_TIME_LIMIT) ch |= igDragFloat(PL(TR("Time Limit")), &d->value, 0.05f, 0.0f, 0.0f, "%.2f s", 0);
    else if (d->kind == BTD_FORCE_SUCCESS) igTextDisabled("%s", TR("The node counts as a success, whatever happened."));
    return ch;
}

static bool tree_details(BehaviorTree* t) {
    if (E.sel < 0 || E.sel >= (int)t->nnodes) {
        igTextDisabled("%s", TR("Click a node, a decorator (blue) or a service (green). Right click the canvas to add nodes."));
        return false;
    }
    bool ch = false;
    BtNode* n = &t->nodes[E.sel];
    if (E.sub == 1 && E.sub_i < n->ndecos) return deco_details(t, &n->decos[E.sub_i]);
    if (E.sub == 2 && E.sub_i < n->nsvcs) {
        BtService* s = &n->svcs[E.sub_i];
        igSeparatorText(TR(g_bts_names[s->kind % BTS_KIND_COUNT]));
        if (s->kind == BTS_FOCUS) ch |= key_combo(PL(TR("Focus Key")), s->key, t->keys, t->nkeys);
        if (s->kind == BTS_SPEED) ch |= igSliderFloat(PL(TR("Speed (walk-run)")), &s->value, 0.1f, 1.0f, "%.2f", 0);
        return ch;
    }
    igSeparatorText(n->kind == BT_ROOT ? "ROOT" : TR(g_bt_kind_names[n->kind]));
    if (n->kind == BT_ROOT) {
        igTextDisabled("%s", TR("The root runs its one child again and again."));
        return false;
    }
    if (n->parent == BT_DETACHED) igTextColored(THV(TK_WARN), "%s", TR("Not wired: drag from a composite's bottom bar to it."));
    switch (n->kind) {
    case BT_MOVE_TO:
        ch |= key_combo(PL(TR("Blackboard Key")), n->key, t->keys, t->nkeys);
        ch |= igDragFloat(PL(TR("Acceptable Radius")), &n->value, 1.0f, 0.0f, 0.0f, "%.0f cm", 0);
        break;
    case BT_WAIT:
        ch |= igDragFloat(PL(TR("Wait Time")), &n->value, 0.05f, 0.0f, 0.0f, "%.2f s", 0);
        ch |= igDragFloat(PL(TR("Random Deviation")), &n->value2, 0.05f, 0.0f, 0.0f, "%.2f s", 0);
        break;
    case BT_ROTATE_TO: ch |= key_combo(PL(TR("Blackboard Key")), n->key, t->keys, t->nkeys); break;
    case BT_FIND_RANDOM:
        ch |= key_combo(PL(TR("Result Key")), n->key, t->keys, t->nkeys);
        ch |= igDragFloat(PL(TR("Radius")), &n->value, 5.0f, 0.0f, 0.0f, "%.0f cm", 0);
        break;
    case BT_SET_KEY:
        ch |= key_combo(PL(TR("Blackboard Key")), n->key, t->keys, t->nkeys);
        ch |= igDragFloat(PL(TR("Value")), &n->value, 0.1f, 0.0f, 0.0f, "%.2f", 0);
        break;
    case BT_CLEAR_KEY: ch |= key_combo(PL(TR("Blackboard Key")), n->key, t->keys, t->nkeys); break;
    case BT_RUN_EVENT: ch |= igInputText(PL(TR("Event")), n->text, AI_NAME, 0, NULL, NULL); break;
    case BT_PLAY_MONTAGE: ch |= igInputText(PL(TR("Montage")), n->text, AI_NAME, 0, NULL, NULL); break;
    case BT_PREDICT:
        ch |= key_combo(PL(TR("Result Key")), n->key, t->keys, t->nkeys);
        ch |= igDragFloat(PL(TR("Max Time Ahead")), &n->value, 0.05f, 0.0f, 10.0f, "%.2f s", 0);
        igTextDisabled("%s", TR("Where the target will be when this agent can reach it (0 s: the agent's Prediction)."));
        break;
    case BT_SURROUND:
        ch |= key_combo(PL(TR("Result Key")), n->key, t->keys, t->nkeys);
        ch |= igDragFloat(PL(TR("Radius")), &n->value, 5.0f, 0.0f, 1e5f, "%.0f cm", 0);
        igTextDisabled("%s", TR("Its place in the ring its squad closes around the target (0 cm: the agent's Surround Radius)."));
        break;
    default: break;
    }
    igSeparatorText(TR("Decorators and Services"));
    for (uint32_t d = 0; d < n->ndecos; d++) {
        char line[160];
        igPushID_Int((int)d);
        if (igSelectable_Bool(deco_line(&n->decos[d], line, sizeof line), false, 0, (ImVec2){ 0, 0 })) E.sub = 1, E.sub_i = (int)d;
        igPopID();
    }
    for (uint32_t s = 0; s < n->nsvcs; s++) {
        igPushID_Int(100 + (int)s);
        if (igSelectable_Bool(TR(g_bts_names[n->svcs[s].kind % BTS_KIND_COUNT]), false, 0, (ImVec2){ 0, 0 })) E.sub = 2, E.sub_i = (int)s;
        igPopID();
    }
    if (igButton(TR("+ Decorator"), (ImVec2){ 0, 0 })) igOpenPopup_Str("dd_add", 0);
    igSameLine(0, 6);
    if (igButton(TR("+ Service"), (ImVec2){ 0, 0 })) igOpenPopup_Str("ds_add", 0);
    if (igBeginPopup("dd_add", 0)) {
        for (int k = BTD_BLACKBOARD; k < BTD_KIND_COUNT; k++)
            if (ui_menu_item(ICON_FILTER, TR(g_btd_names[k]), NULL, false, n->ndecos < BT_MAX_DECOS)) ch |= add_decorator(n, (uint8_t)k);
        igEndPopup();
    }
    if (igBeginPopup("ds_add", 0)) {
        for (int k = BTS_FOCUS; k < BTS_KIND_COUNT; k++)
            if (ui_menu_item(ICON_REFRESH, TR(g_bts_names[k]), NULL, false, n->nsvcs < BT_MAX_SVCS)) ch |= add_service(n, (uint8_t)k);
        igEndPopup();
    }
    igSeparatorText(TR("Comment"));
    ch |= igInputText("##comment", n->comment, sizeof n->comment, 0, NULL, NULL);
    if (igButton(TR("Delete Node"), (ImVec2){ 0, 0 })) {
        delete_node(t, E.sel);
        E.sel = -1, ch = true;
    }
    return ch;
}

// --- The Brain Editor ---

static void curve_plot(const Consideration* k, float w, float h) {
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    igDummy((ImVec2){ w, h });
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, TH(TK_N4), 3.0f, 0);
    ImVec2 prev = { 0, 0 };
    for (int i = 0; i <= 40; i++) {
        float x = (float)i / 40.0f, y = ai_curve(k->curve, x, k->param);
        if (k->invert) y = 1.0f - y;
        ImVec2 q = { p.x + x * w, p.y + h - y * (h - 4) - 2 };
        if (i) ImDrawList_AddLine(dl, prev, q, TH(TK_AMBER_L), 2.0f);
        prev = q;
    }
}

// --- Making the Brain easy: ready behaviors, considerations said in words, the score tried before playing ---

// The considerations' presets (Unreal-free words: what the curve means)
typedef struct ConsPreset { const char* name; uint8_t curve; bool invert; float lo, hi, param; } ConsPreset;
static const ConsPreset g_cons_presets[] = {
    { "Closer is better", CURVE_INVERSE, false, 0, 2000, 0.5f },
    { "Farther is better", CURVE_LINEAR, false, 0, 2000, 0.5f },
    { "Only when true", CURVE_STEP, false, 0, 1, 0.5f },
    { "Only when false", CURVE_STEP, true, 0, 1, 0.5f },
    { "Grows with time", CURVE_SMOOTH, false, 0, 10, 0.5f },
    { "Fades with time", CURVE_SMOOTH, true, 0, 10, 0.5f },
    { "Best in the middle", CURVE_BELL, false, 0, 2000, 0.25f },
    { "Under a threshold", CURVE_STEP, true, 0, 2000, 0.5f },
};
#define NPRESETS (sizeof g_cons_presets / sizeof g_cons_presets[0])

static void cons_words(const Consideration* k, char* out, size_t cap) {
    const char* in = TR(g_brain_input_names[k->input % BRAIN_IN_COUNT]);
    switch (k->curve) {
    case CURVE_STEP:
        if (k->hi - k->lo <= 1.0f) snprintf(out, cap, k->invert ? TR("%s is false") : TR("%s is true"), in);
        else snprintf(out, cap, k->invert ? TR("%s under %.0f") : TR("%s over %.0f"), in, k->lo + (k->hi - k->lo) * k->param);
        break;
    case CURVE_INVERSE: snprintf(out, cap, k->invert ? TR("%s: farther is better (%.0f-%.0f)") : TR("%s: closer is better (%.0f-%.0f)"), in, k->lo, k->hi); break;
    case CURVE_BELL: snprintf(out, cap, TR("%s: best around %.0f"), in, (k->lo + k->hi) * 0.5f); break;
    default: snprintf(out, cap, k->invert ? TR("%s: less is better (%.0f-%.0f)") : TR("%s: more is better (%.0f-%.0f)"), in, k->lo, k->hi); break;
    }
}

// "Chase runs when Sees Target is true and Distance to Target: closer is better; priority 1.2"
static void behavior_words(const BrainBehavior* x, char* out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "%s: %s", x->name, TR(g_brain_action_names[x->action % BRAIN_ACT_COUNT]));
    for (uint32_t c = 0; c < x->ncons && n < cap; c++) {
        char w[160];
        cons_words(&x->cons[c], w, sizeof w);
        n += (size_t)snprintf(out + n, cap - n, c ? TR(" and %s") : TR(" when %s"), w);
    }
    if (!x->ncons && n < cap) n += (size_t)snprintf(out + n, cap - n, "%s", TR(" always (a fallback)"));
    if (n < cap) snprintf(out + n, cap - n, TR("; priority %.2g"), x->weight);
}

// Ready behaviors (what most games need), each with its considerations set up
static bool add_template(Brain* b, int k) {
    if (b->nbehaviors >= 16) return false;
    BrainBehavior* x = &b->behaviors[b->nbehaviors];
    memset(x, 0, sizeof *x);
    x->weight = 1.0f, x->speed = 0.5f;
    switch (k) {
    case 0: snprintf(x->name, AI_NAME, "Rest"), x->action = BRAIN_ACT_IDLE, x->weight = 0.15f; break;
    case 1:
        snprintf(x->name, AI_NAME, "Wander"), x->action = BRAIN_ACT_WANDER, x->param = 800.0f, x->speed = 0.35f, x->weight = 0.4f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_STEP, "", 0, 1, 0.5f, true };
        break;
    case 2:
        snprintf(x->name, AI_NAME, "Chase"), x->action = BRAIN_ACT_CHASE, x->speed = 1.0f, x->weight = 1.2f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_INVERSE, "", 0, 3000, 0.5f, false };
        break;
    case 3:
        snprintf(x->name, AI_NAME, "Flee"), x->action = BRAIN_ACT_FLEE, x->param = 1500.0f, x->speed = 1.0f, x->weight = 1.5f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_STEP, "", 0, 2000, 0.2f, true };
        break;
    case 4:
        snprintf(x->name, AI_NAME, "Investigate Noise"), x->action = BRAIN_ACT_INVESTIGATE, x->speed = 0.6f, x->weight = 0.9f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_HEARD_NOISE, CURVE_STEP, "", 0, 1, 0.5f, false };
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, true };
        break;
    case 5:
        snprintf(x->name, AI_NAME, "Search Last Seen"), x->action = BRAIN_ACT_INVESTIGATE, x->speed = 0.7f, x->weight = 0.8f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, true };
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_TIME_SINCE_SEEN, CURVE_SMOOTH, "", 0, 15, 0.5f, true };
        break;
    case 6:
        snprintf(x->name, AI_NAME, "Patrol"), x->action = BRAIN_ACT_PATROL, x->param = 1200.0f, x->speed = 0.4f, x->weight = 0.5f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_HAS_TARGET, CURVE_STEP, "", 0, 1, 0.5f, true };
        break;
    case 7:
        snprintf(x->name, AI_NAME, "Attack"), x->action = BRAIN_ACT_ATTACK, x->param = 150.0f, x->param2 = 1.0f, x->weight = 2.0f;
        snprintf(x->text, AI_NAME, "Attack");
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_STEP, "", 0, 1000, 0.15f, true };
        break;
    case 8:
        snprintf(x->name, AI_NAME, "Keep Distance"), x->action = BRAIN_ACT_KEEP_DISTANCE, x->param = 600.0f, x->param2 = 1200.0f, x->weight = 1.1f;
        x->cons[x->ncons++] = (Consideration){ BRAIN_IN_SEES_TARGET, CURVE_STEP, "", 0, 1, 0.5f, false };
        break;
    default: snprintf(x->name, AI_NAME, "Behavior%u", b->nbehaviors); break;
    }
    E.behavior = (int)b->nbehaviors++;
    return true;
}
static const char* const g_templates[] = { "Rest (idle)", "Wander when there is no target", "Chase what it sees",
                                           "Flee when it comes close", "Investigate a noise", "Search where it was last seen",
                                           "Patrol when there is no target", "Attack in range", "Keep distance (ranged)", "Empty" };

static bool brain_editor(Brain* b, float w, float h) {
    bool ch = false;
    float list_w = igGetFontSize() * 13.0f;
    // the behaviors with their live scores
    igBeginChild_Str("br_list", (ImVec2){ list_w, h }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Behaviors"));
    static AiDebug dbg[64];
    uint32_t nd = ai_debug(dbg, 64);
    const AiDebug* live = NULL;
    for (uint32_t i = 0; i < nd && !live; i++)
        if (dbg[i].nscores == b->nbehaviors) live = &dbg[i];
    float top = 1e-6f;
    for (uint32_t i = 0; live && i < live->nscores; i++) top = fmaxf(top, live->scores[i]);
    for (uint32_t i = 0; i < b->nbehaviors; i++) {
        igPushID_Int((int)i);
        ImVec2 p = igGetCursorScreenPos();
        if (igSelectable_Bool(b->behaviors[i].name, E.behavior == (int)i, 0, (ImVec2){ 0, igGetTextLineHeight() * 1.6f })) E.behavior = (int)i;
        if (igIsItemHovered(0)) {
            char words[512];
            behavior_words(&b->behaviors[i], words, sizeof words);
            igSetItemTooltip("%s", words);
        }
        if (live) { // the score as a bar under its name (the running one in green)
            float f = live->scores[i] / top;
            ImDrawList_AddRectFilled(igGetWindowDrawList(), (ImVec2){ p.x, p.y + igGetTextLineHeight() * 1.25f },
                                     (ImVec2){ p.x + (list_w - 20.0f) * f, p.y + igGetTextLineHeight() * 1.5f },
                                     live->active == i ? TH(TK_GREEN) : TH(TK_N11), 2.0f, 0);
        }
        igPopID();
    }
    if (b->nbehaviors < 16 && igButton(TR("+ Add Behavior"), (ImVec2){ -1, 0 })) igOpenPopup_Str("br_tpl", 0);
    if (igBeginPopup("br_tpl", 0)) {
        igTextDisabled("%s", TR("Ready behaviors (their considerations set up):"));
        for (int k = 0; k < (int)(sizeof g_templates / sizeof g_templates[0]); k++)
            if (ui_menu_item(ICON_NEW, TR(g_templates[k]), NULL, false, true)) ch |= add_template(b, k);
        igEndPopup();
    }
    igSpacing();
    ch |= igSliderFloat(PL(TR("Momentum")), &b->momentum, 0.0f, 1.0f, "%.2f", 0);
    ch |= igDragFloat(PL(TR("Think Every")), &b->think_interval, 0.01f, 0.05f, 5.0f, "%.2f s", 0);
    if (live) igTextColored(THV(TK_OK), TR("Live: %s"), live->what);
    else igTextDisabled("%s", TR("Play (Alt+P) to see the scores live."));
    igEndChild();
    igSameLine(0, 4);
    igBeginChild_Str("br_detail", (ImVec2){ w - list_w - 4.0f, h }, ImGuiChildFlags_Borders, 0);
    if (E.behavior >= 0 && E.behavior < (int)b->nbehaviors) {
        BrainBehavior* x = &b->behaviors[E.behavior];
        { // in words, and the score it would have (the test values below)
            char words[512];
            behavior_words(x, words, sizeof words);
            igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_INFO));
            igTextWrapped("%s", words);
            igPopStyleColor(1);
        }
        static float test[16][4] = { { 0 } };
        float predicted = x->weight;
        for (uint32_t c = 0; c < x->ncons; c++) {
            const Consideration* k = &x->cons[c];
            float t01 = k->hi != k->lo ? (test[E.behavior & 15][c] - k->lo) / (k->hi - k->lo) : 0.0f;
            float y = ai_curve(k->curve, t01, k->param);
            predicted *= k->invert ? 1.0f - y : y;
        }
        igTextColored(THV(TK_OK), TR("Score with the test values: %.3f (the highest score runs)"), predicted);
        igPushItemWidth(igGetFontSize() * 14.0f);
        ch |= igInputText(PL(TR("Name")), x->name, AI_NAME, 0, NULL, NULL);
        int act = x->action;
        if (igCombo_Str_arr(PL(TR("Action")), &act, g_brain_action_names, BRAIN_ACT_COUNT, -1)) x->action = (uint8_t)act, ch = true;
        ch |= igSliderFloat(PL(TR("Weight (priority)")), &x->weight, 0.0f, 3.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Speed (walk-run)")), &x->speed, 0.1f, 1.0f, "%.2f", 0);
        switch (x->action) {
        case BRAIN_ACT_WANDER: case BRAIN_ACT_PATROL: ch |= igDragFloat(PL(TR("Radius")), &x->param, 5.0f, 50.0f, 1e5f, "%.0f cm", 0); break;
        case BRAIN_ACT_FLEE: ch |= igDragFloat(PL(TR("Flee Distance")), &x->param, 5.0f, 100.0f, 1e5f, "%.0f cm", 0); break;
        case BRAIN_ACT_KEEP_DISTANCE:
            ch |= igDragFloat(PL(TR("Min Distance")), &x->param, 5.0f, 0.0f, 1e5f, "%.0f cm", 0);
            ch |= igDragFloat(PL(TR("Max Distance")), &x->param2, 5.0f, 0.0f, 1e5f, "%.0f cm", 0);
            break;
        case BRAIN_ACT_ATTACK:
            ch |= igDragFloat(PL(TR("Attack Range")), &x->param, 1.0f, 10.0f, 1e4f, "%.0f cm", 0);
            ch |= igDragFloat(PL(TR("Cooldown")), &x->param2, 0.05f, 0.0f, 60.0f, "%.2f s", 0);
            ch |= igInputText(PL(TR("Event")), x->text, AI_NAME, 0, NULL, NULL);
            break;
        case BRAIN_ACT_RUN_TREE:
            if (igBeginCombo(PL(TR("Behavior Tree")), x->text[0] ? x->text : "-", 0)) {
                for (uint32_t i = 0; i < ai_tree_count(); i++)
                    if (igSelectable_Bool(ai_tree_at(i)->name, !strcmp(x->text, ai_tree_at(i)->name), 0, (ImVec2){ 0, 0 }))
                        snprintf(x->text, AI_NAME, "%s", ai_tree_at(i)->name), ch = true;
                igEndCombo();
            }
            break;
        case BRAIN_ACT_EVENT: ch |= igInputText(PL(TR("Event")), x->text, AI_NAME, 0, NULL, NULL); break;
        case BRAIN_ACT_INTERCEPT: igTextDisabled("%s", TR("Runs to where the target will be (the agent's Prediction): the shortcut that cuts it off.")); break;
        case BRAIN_ACT_SURROUND: igTextDisabled("%s", TR("Takes its place in the ring its squad closes around the target, facing it.")); break;
        default: break;
        }
        igPopItemWidth();
        igSeparatorText(TR("Considerations (score = weight x each curve)"));
        for (uint32_t c = 0; c < x->ncons; c++) {
            Consideration* k = &x->cons[c];
            igPushID_Int((int)c);
            igBeginGroup();
            igPushItemWidth(igGetFontSize() * 11.0f);
            int in = k->input, cv = k->curve;
            char words[200];
            cons_words(k, words, sizeof words);
            igTextColored(THV(TK_WARN), "%s", words);
            if (igCombo_Str_arr(PL(TR("Input")), &in, g_brain_input_names, BRAIN_IN_COUNT, -1)) k->input = (uint8_t)in, ch = true;
            if (igBeginCombo(PL(TR("Quick setup")), TR("choose what it means..."), 0)) {
                for (uint32_t p = 0; p < NPRESETS; p++)
                    if (igSelectable_Bool(TR(g_cons_presets[p].name), false, 0, (ImVec2){ 0, 0 })) {
                        const ConsPreset* q = &g_cons_presets[p];
                        bool flag = k->input == BRAIN_IN_SEES_TARGET || k->input == BRAIN_IN_HEARD_NOISE || k->input == BRAIN_IN_HAS_TARGET;
                        k->curve = q->curve, k->invert = q->invert, k->param = q->param;
                        k->lo = q->lo, k->hi = flag ? 1.0f : k->input == BRAIN_IN_TIME_SINCE_SEEN || k->input == BRAIN_IN_TIME_IN_BEHAVIOR ? 10.0f
                                                    : k->input == BRAIN_IN_RANDOM ? 1.0f : q->hi;
                        ch = true;
                    }
                igEndCombo();
            }
            if (k->input == BRAIN_IN_KEY_FLOAT) ch |= key_combo(PL(TR("Key")), k->key, b->keys, b->nkeys);
            if (k->input == BRAIN_IN_BP_VAR) ch |= igInputText(PL(TR("Variable")), k->key, AI_NAME, 0, NULL, NULL);
            ch |= igDragFloat2(PL(TR("Range")), &k->lo, 0.1f, -1e6f, 1e6f, "%.1f", 0);
            if (igCombo_Str_arr(PL(TR("Curve")), &cv, g_curve_names, CURVE_COUNT, -1)) k->curve = (uint8_t)cv, ch = true;
            if (k->curve == CURVE_STEP || k->curve == CURVE_BELL) ch |= igSliderFloat(PL(TR("Threshold / Width")), &k->param, 0.0f, 1.0f, "%.2f", 0);
            ch |= igCheckbox(PL(TR("Invert")), &k->invert);
            igDragFloat(PL(TR("Test value")), &test[E.behavior & 15][c], (k->hi - k->lo) * 0.005f + 0.001f, 0, 0, "%.2f", 0);
            igPopItemWidth();
            igEndGroup();
            igSameLine(0, 12);
            ImVec2 cp = igGetCursorScreenPos();
            float cw = igGetFontSize() * 8.0f, chh = igGetFontSize() * 5.0f;
            curve_plot(k, cw, chh);
            { // the test value on the curve
                float t01 = k->hi != k->lo ? fminf(fmaxf((test[E.behavior & 15][c] - k->lo) / (k->hi - k->lo), 0.0f), 1.0f) : 0.0f;
                float y = ai_curve(k->curve, t01, k->param);
                if (k->invert) y = 1.0f - y;
                ImDrawList_AddCircleFilled(igGetWindowDrawList(), (ImVec2){ cp.x + t01 * cw, cp.y + chh - y * (chh - 4) - 2 }, 4.0f, TH(TK_AMBER), 12);
            }
            igSameLine(0, 8);
            if (igSmallButton("x")) {
                memmove(k, k + 1, sizeof *k * (x->ncons - c - 1));
                x->ncons--, ch = true;
                igPopID();
                break;
            }
            igSeparator();
            igPopID();
        }
        if (x->ncons < 4 && igButton(TR("Add Consideration"), (ImVec2){ 0, 0 })) {
            x->cons[x->ncons++] = (Consideration){ BRAIN_IN_DISTANCE, CURVE_INVERSE, "", 0, 2000, 0.5f, false };
            ch = true;
        }
        igSameLine(0, 12);
        if (b->nbehaviors > 1 && igButton(TR("Delete Behavior"), (ImVec2){ 0, 0 })) {
            memmove(x, x + 1, sizeof *x * (b->nbehaviors - (uint32_t)E.behavior - 1));
            b->nbehaviors--, E.behavior = 0, ch = true;
        }
    }
    igSpacing();
    ch |= blackboard(b->keys, &b->nkeys);
    igEndChild();
    return ch;
}

// --- Unreal's Blackboard mode of the Behavior Tree editor (the toolbar's Behavior Tree / Blackboard): the keys on the
// left, the selected key's details on the right (name, type) and where the tree uses it (tasks, decorators, services) ---
static bool blackboard_mode(BehaviorTree* t, float w, float h) {
    static int sel = 0;
    bool ch = false;
    float left = igGetFontSize() * 20.0f;
    igBeginChild_Str("bbm_keys", (ImVec2){ left, h }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Blackboard"));
    for (uint32_t i = 0; i < t->nkeys; i++) {
        igPushID_Int((int)i);
        char row[AI_NAME + 40];
        snprintf(row, sizeof row, "%s##bbk", t->keys[i].name);
        if (igSelectable_Bool(row, sel == (int)i, 0, (ImVec2){ 0, 0 })) sel = (int)i;
        igSameLine(left - igCalcTextSize(g_bb_type_names[t->keys[i].type], NULL, false, -1).x - 24.0f, 0);
        igTextDisabled("%s", g_bb_type_names[t->keys[i].type]);
        igPopID();
    }
    if (t->nkeys < AI_MAX_KEYS && igButton(TR("New Key"), (ImVec2){ -FLT_MIN, 0 })) {
        snprintf(t->keys[t->nkeys].name, AI_NAME, "Key%u", t->nkeys);
        t->keys[t->nkeys].type = BB_FLOAT;
        sel = (int)t->nkeys++, ch = true;
    }
    igEndChild();
    igSameLine(0, 4);
    igBeginChild_Str("bbm_details", (ImVec2){ w - left - 4.0f, h }, ImGuiChildFlags_Borders, 0);
    igSeparatorText(TR("Blackboard Details"));
    if (sel >= 0 && sel < (int)t->nkeys) {
        BbKey* k = &t->keys[sel];
        char old[AI_NAME];
        snprintf(old, sizeof old, "%s", k->name);
        if (igInputText(PL(TR("Entry Name")), k->name, AI_NAME, 0, NULL, NULL)) { // the tree follows the new name
            for (uint32_t i = 0; i < t->nnodes; i++) {
                BtNode* nd = &t->nodes[i];
                if (!strcmp(nd->key, old)) snprintf(nd->key, AI_NAME, "%s", k->name);
                for (uint32_t d = 0; d < nd->ndecos; d++)
                    if (!strcmp(nd->decos[d].key, old)) snprintf(nd->decos[d].key, AI_NAME, "%s", k->name);
                for (uint32_t s = 0; s < nd->nsvcs; s++)
                    if (!strcmp(nd->svcs[s].key, old)) snprintf(nd->svcs[s].key, AI_NAME, "%s", k->name);
            }
            ch = true;
        }
        int ty = k->type;
        if (igCombo_Str_arr(PL(TR("Key Type")), &ty, g_bb_type_names, BB_TYPE_COUNT, -1)) k->type = (uint8_t)ty, ch = true;
        igSeparatorText(TR("Used by"));
        int uses = 0;
        for (uint32_t i = 0; i < t->nnodes; i++) {
            const BtNode* nd = &t->nodes[i];
            const char* what = TR(g_bt_kind_names[nd->kind]);
            if (!strcmp(nd->key, k->name)) igBulletText("%s", what), uses++;
            for (uint32_t d = 0; d < nd->ndecos; d++)
                if (!strcmp(nd->decos[d].key, k->name)) igBulletText("%s > %s", what, TR(g_btd_names[nd->decos[d].kind])), uses++;
            for (uint32_t s = 0; s < nd->nsvcs; s++)
                if (!strcmp(nd->svcs[s].key, k->name)) igBulletText("%s > %s", what, TR(g_bts_names[nd->svcs[s].kind])), uses++;
        }
        if (!uses) igTextDisabled("%s", TR("Not used by the tree yet."));
        igSpacing();
        if (igButton(TR("Delete Key"), (ImVec2){ 0, 0 })) {
            memmove(&t->keys[sel], &t->keys[sel + 1], sizeof(BbKey) * (t->nkeys - (uint32_t)sel - 1));
            t->nkeys--, sel = sel > 0 ? sel - 1 : 0, ch = true;
        }
    } else igTextDisabled("%s", TR("Select a key."));
    igTextDisabled("%s", TR("Perception writes TargetActor, CanSeeTarget, LastKnownLocation, DistanceToTarget, HeardNoise, NoiseLocation, SelfLocation."));
    igEndChild();
    return ch;
}

void ai_editor_draw(struct Editor* ed, double now) {
    (void)ed, (void)now;
    if (!E.tree && !E.brain) return;
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###ai_editor2", E.tree ? E.tree->name : E.brain->name, E.unsaved ? "*" : "",
             E.tree ? TR("Behavior Tree") : TR("Real Brain"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.75f, mv->Size.y * 0.75f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.125f, mv->Pos.y + mv->Size.y * 0.1f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = E.tree ? ai_tree_save(E.tree) : ai_brain_save(E.brain);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        if (E.tree) {
            igSameLine(0, 6);
            if (igButton(TR("Auto Arrange"), (ImVec2){ 0, 0 })) auto_arrange(E.tree), E.unsaved = true;
            igSameLine(0, 14); // the modes
            if (ui_icon_button("bt_mode_tree", ICON_GRAPH, TR("Behavior Tree"), TH(TK_N14), !E.bb_mode, NULL)) E.bb_mode = false;
            igSameLine(0, 2);
            if (ui_icon_button("bt_mode_bb", ICON_DATA, TR("Blackboard"), TH(TK_N14), E.bb_mode, NULL)) E.bb_mode = true;
        }
        igSameLine(0, 10);
        igTextDisabled("%s", E.tree ? TR("Behavior Tree: Selectors try their children until one works, Sequences run them all.")
                                    : TR("Utility AI: every behavior scores its considerations; the best one runs."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        ImVec2 avail = igGetContentRegionAvail();
        if (E.tree && E.bb_mode) E.unsaved |= blackboard_mode(E.tree, avail.x, avail.y);
        else if (E.tree) {
            float left = igGetFontSize() * 15.0f, right = igGetFontSize() * 18.0f;
            igBeginChild_Str("bt_left", (ImVec2){ left, avail.y }, ImGuiChildFlags_Borders, 0);
            E.unsaved |= blackboard(E.tree->keys, &E.tree->nkeys);
            igEndChild();
            igSameLine(0, 4);
            E.unsaved |= tree_canvas(E.tree, avail.x - left - right - 8.0f, avail.y);
            igSameLine(0, 4);
            igBeginChild_Str("bt_right", (ImVec2){ right, avail.y }, ImGuiChildFlags_Borders, 0);
            igPushItemWidth(-igGetFontSize() * 7.0f);
            E.unsaved |= tree_details(E.tree);
            igPopItemWidth();
            igEndChild();
        } else
            E.unsaved |= brain_editor(E.brain, avail.x, avail.y);
    }
    igEnd();
    if (!open) E.tree = NULL, E.brain = NULL;
}

bool ai_component_details(CAIAgent* a) {
    bool ch = false;
    static const char* const modes[3] = { "None", "Behavior Tree", "Real Brain" };
    int m = (int)a->mode;
    if (igCombo_Str_arr(PL(TR("AI")), &m, modes, 3, -1)) a->mode = (uint32_t)m, a->asset = 0, ch = true;
    if (a->mode == AI_MODE_TREE) {
        const BehaviorTree* cur = ai_tree(a->asset);
        if (igBeginCombo(PL(TR("Behavior Tree")), cur ? cur->name : "-", 0)) {
            for (uint32_t i = 0; i < ai_tree_count(); i++)
                if (igSelectable_Bool(ai_tree_at(i)->name, ai_tree_at(i) == cur, 0, (ImVec2){ 0, 0 })) a->asset = ai_tree_at(i)->hash, ch = true;
            igEndCombo();
        }
    } else if (a->mode == AI_MODE_BRAIN) {
        const Brain* cur = ai_brain(a->asset);
        if (igBeginCombo(PL(TR("Brain")), cur ? cur->name : "-", 0)) {
            for (uint32_t i = 0; i < ai_brain_count(); i++)
                if (igSelectable_Bool(ai_brain_at(i)->name, ai_brain_at(i) == cur, 0, (ImVec2){ 0, 0 })) a->asset = ai_brain_at(i)->hash, ch = true;
            igEndCombo();
        }
    }
    igSeparatorText(TR("AI Perception"));
    ch |= igDragFloat(PL(TR("Sight Radius")), &a->sight_radius, 10.0f, 0.0f, 1e5f, "%.0f cm", 0);
    ch |= igSliderFloat(PL(TR("Peripheral Vision Angle")), &a->sight_angle, 10.0f, 360.0f, "%.0f", 0);
    ch |= igDragFloat(PL(TR("Hearing Range")), &a->hearing_radius, 10.0f, 0.0f, 1e5f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("Max Age")), &a->lose_sight_time, 0.1f, 0.0f, 120.0f, "%.1f s", 0);
    int team = (int)a->team;
    if (igSliderInt(PL(TR("Team")), &team, 0, 8, "%d", 0)) a->team = (uint32_t)team, ch = true;
    igTextDisabled("%s", TR("Team 0 ignores everyone; other teams (and the player) are targets."));
    igSeparatorText(TR("Difficulty and Teamwork"));
    ch |= igDragFloat(PL(TR("Prediction")), &a->prediction, 0.05f, 0.0f, 10.0f, "%.2f s", 0);
    ch |= igDragFloat(PL(TR("Reaction Time")), &a->reaction, 0.01f, 0.0f, 5.0f, "%.2f s", 0);
    ch |= igSliderFloat(PL(TR("Coordination")), &a->coordination, 0.0f, 1.0f, "%.2f", 0);
    ch |= igDragFloat(PL(TR("Share Radius")), &a->share_radius, 10.0f, 0.0f, 1e5f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("Surround Radius")), &a->surround_radius, 5.0f, 50.0f, 1e4f, "%.0f cm", 0);
    if (igButton(TR("Easy"), (ImVec2){ 0, 0 })) a->prediction = 0.0f, a->reaction = 0.8f, a->coordination = 0.0f, a->share_radius = 0.0f, ch = true;
    igSameLine(0, 4);
    if (igButton(TR("Normal"), (ImVec2){ 0, 0 })) a->prediction = 0.8f, a->reaction = 0.2f, a->coordination = 0.6f, a->share_radius = 2500.0f, ch = true;
    igSameLine(0, 4);
    if (igButton(TR("Hard"), (ImVec2){ 0, 0 })) a->prediction = 1.5f, a->reaction = 0.05f, a->coordination = 0.85f, a->share_radius = 6000.0f, ch = true;
    igTextDisabled("%s", TR("Prediction aims ahead of a moving target; Coordination sends part of a squad round to cut it off."));
    return ch;
}

void ai_editor_blackboard_mode(bool on) { E.bb_mode = on; }
