// The Blueprint Editor (bp_editor.h): Unreal's, with its shortcuts. The graph: drag a wire from a pin and let go on
// nothing for the nodes that fit it (linked when placed), right click for every node, B/S/D + click for Branch,
// Sequence and Delay, Alt + click on a pin breaks its wires, Delete, Ctrl+D, Ctrl+A, Home frames the graph, F7 compiles,
// Ctrl+S saves. Variables are dragged from My Blueprint (Ctrl: Get, Alt: Set).
#include <ctype.h>
#include "editor_docs.h"
#include "graph_layout.h"
#include "graph_view.h" // (2026-11-23: gv_edge_scroll)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include "graph_style.h"

#include "theme.h"
#include "ui_number.h"
#include "bp_editor.h"
#include "editor_host.h" // (item 9)
#include "asset_picker.h"
#include "graph_menu.h"
#include "bp_internal.h"
#include "plugins.h"
#include "ui_widget.h"
#include "sequencer.h"
#include "bp_preview.h"
#include "bp_timeline.h"
#include "anim.h"
#include "particles.h"
#include "material.h"
#include "asset_viewer.h"
#include "anim_editor.h"
#include "audio_editor.h"
#include "audio_internal.h"
#include "ai.h"
#include "input.h"
#include "components.h"
#include "health.h"
#include "hud.h"
#include "interaction.h"
#include "content.h"
#include "platform_files.h"
#include "editor.h"
#include "editor_dock.h"
#include "i18n.h"
#include "gameplay_tags.h"
#include "post.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "inventory.h"
#include "dialogue.h"
#include "quest.h"
#include "game_rules.h"
#include "ability.h"
#include "bp_debug.h"
#include "script_ide.h"
#include <GLFW/glfw3.h>

#define MAX_WIN 8
#define WANT_ANIM_GRAPH 1000 // (Win.want_tab: the AnimGraph tab)
#define MAX_SEL 512
#define MAX_MENU 1024

enum { DET_CLASS = 0, DET_VAR, DET_NODE, DET_FUNC, DET_COMPONENT, DET_DEFAULTS, DET_SCS };
enum { MODE_NONE = 0, MODE_MOVE, MODE_BOX, MODE_WIRE, MODE_PAN };

typedef struct Win {
    BpAsset* a;
    bool open, focus;
    uint8_t placed; // (Q26) frames it was put over the viewport (the layout settles in the first ones)
    bool host;      // (item 9) docks into the Editors window on its next frame (editor_host.h)
    uint32_t graph;
    uint32_t sel[MAX_SEL];
    uint32_t nsel;
    float move_start[MAX_SEL][2]; // (a node drag: the selection's places when it began, the mouse's then)
    ImVec2 move_press;
    int mode;
    uint32_t wire_node;
    uint8_t wire_pin;
    ImVec2 press;
    float menu_g[2];         // where the action menu places its node (graph units)
    uint32_t from_node;      // the pin the menu was opened from (from_pin < 0: none)
    int from_pin;
    uint32_t last_placed;    // the last node placed in this graph (the menu's recommendations follow it; 0: none)
    uint32_t last_graph;
    char search[64];
    int details, var, func;
    bool frame;
    char status[200];
    bool status_bad;
    // the Viewport tab (the class's look, bp_preview.c) and the Components panel
    BpPreviewCam cam;
    bool vp_grid, vp_helpers;
    int vp_mode; // the Viewport's view mode (PREVIEW_, render.h)
    int comp;     // the component in Details (DET_COMPONENT)
    int scs;      // the component tree's node selected (DET_SCS; -1 none)
    int gizmo;    // 0 move (W), 1 rotate (E), 2 scale (R)
    BpGizmo giz;
    bool giz_held, vp_clicked;
    int want_tab; // a tab to bring forward: 0 Viewport, 1 + graph, WANT_ANIM_GRAPH; -1 none
    int want_hold; // frames it is asked for (a new window: its tabs dock first)
    ImGuiID dock; // (the dock space the tabs' windows go in)
    bool on_viewport;
    bool on_anim; // the AnimGraph tab is the current one (My Blueprint marks its row)
    uint32_t closed; // the tabs closed (Unreal's: x on the tab; reopened from My Blueprint or the Window menu): bit 0 Viewport, 1 the Anim Graph, 2 + graph
    uint32_t center_node; // a node to bring to the middle of the graph (the debugger's pause, its call stack); 0: none
    // (2026-11-16) the Timeline editor open on a Timeline node (bp_timeline.h): the node's graph and id
    bool tl_open;
    uint32_t tl_graph, tl_node;
} Win;

typedef struct MenuEntry {
    char label[96];
    char cat[40];
    uint16_t kind;
    char text[BP_NAME], text2[BP_NAME];
    int8_t group;  // 0 fits the wire, 1 recently used, 2 the rest (the menu's order)
    int8_t recent; // its place among the recent ones (-1: none)
} MenuEntry;

typedef struct Geo {
    ImVec2 pos, size;
    ImVec2 pin[BP_MAX_PINS];
    float lit_w[BP_MAX_PINS];
    float label_x[BP_MAX_PINS];
    float header;
} Geo;

static Win g_win[MAX_WIN];
static MenuEntry g_menu[MAX_MENU];
static uint32_t g_nmenu;
static bool g_focused;
static Geo* g_geo;
static uint32_t g_geo_cap;

static const char* const g_keys[] = { "Space", "E", "F", "Q", "R", "1", "2", "3", "Enter", "Left Shift", "Left Mouse Button",
                                      "Right Mouse Button", "Tab", "Up", "Down", "Left", "Right" };
static const char* const g_axes[] = { "MoveForward", "MoveRight", "Turn", "LookUp" };

bool bp_editor_focused(void) { return g_focused; }

static ImU32 type_color(uint8_t t) { // the pins by family (theme tokens: numbers in blues, text in magentas...)
    switch (t) {
    case BPT_EXEC: return TH(TK_PIN_EXEC);
    case BPT_BOOL: return TH(TK_PIN_BOOL);
    case BPT_INT: return TH(TK_PIN_INT);
    case BPT_FLOAT: return TH(TK_PIN_FLOAT);
    case BPT_VECTOR: return TH(TK_PIN_VECTOR);
    case BPT_ROTATOR: return TH(TK_PIN_ROTATOR);
    case BPT_STRING: return TH(TK_PIN_STRING);
    case BPT_ACTOR: return TH(TK_PIN_OBJECT);
    case BPT_STRUCT: return TH(TK_PIN_STRUCT);
    case BPT_ENUM: return TH(TK_PIN_ENUM);
    case BPT_BYTE: return TH(TK_PIN_BYTE);
    case BPT_INT64: return TH(TK_PIN_INT64);
    case BPT_NAME: return TH(TK_PIN_NAME);
    case BPT_TEXT: return TH(TK_PIN_TEXT);
    case BPT_COLOR: return TH(TK_PIN_COLOR);
    case BPT_TRANSFORM: return TH(TK_PIN_TRANSFORM);
    case BPT_CLASS: return TH(TK_PIN_CLASS);
    case BPT_OBJECT: return TH(TK_PIN_OBJECT);
    default: return TH(TK_PIN_WILDCARD);
    }
}

// "Float", "Array of Float", "Map of Name to Integer"
static void type_label(uint8_t type, const char* ref, uint8_t cont, uint8_t vtype, char* out, size_t cap) {
    const char* tn = (type == BPT_STRUCT || type == BPT_ENUM || ((type == BPT_ACTOR || type == BPT_CLASS || type == BPT_NAME) && ref && ref[0])) ? ref : g_bp_type_names[type];
    switch (cont) {
    case BPC_ARRAY: snprintf(out, cap, "Array of %s", tn); break;
    case BPC_SET: snprintf(out, cap, "Set of %s", tn); break;
    case BPC_MAP: snprintf(out, cap, "Map of %s to %s", tn, g_bp_type_names[vtype < BPT_COUNT ? vtype : 0]); break;
    default: snprintf(out, cap, "%s", tn); break;
    }
}

// --- Recently used nodes (the action menu shows them before the rest, Unreal's way) ---
typedef struct Recent { uint16_t kind; char text[BP_NAME], text2[BP_NAME]; } Recent;
static Recent g_recent[16];
static uint32_t g_nrecent;
static void recent_add(uint16_t kind, const char* text, const char* text2) {
    uint32_t at = g_nrecent < 16 ? g_nrecent : 15;
    for (uint32_t i = 0; i < g_nrecent; i++)
        if (g_recent[i].kind == kind && !strcmp(g_recent[i].text, text) && !strcmp(g_recent[i].text2, text2)) at = i;
    if (at == g_nrecent) g_nrecent++;
    memmove(&g_recent[1], &g_recent[0], sizeof(Recent) * at);
    g_recent[0].kind = kind;
    snprintf(g_recent[0].text, BP_NAME, "%s", text);
    snprintf(g_recent[0].text2, BP_NAME, "%s", text2);
}
static int recent_rank(uint16_t kind, const char* text, const char* text2) {
    for (uint32_t i = 0; i < g_nrecent; i++)
        if (g_recent[i].kind == kind && !strcmp(g_recent[i].text, text) && !strcmp(g_recent[i].text2, text2)) return (int)i;
    return -1;
}

static bool node_latent(const BpNode* n); // (Q5, below)
static ImU32 header_color(const BpNode* n) {
    const BpNodeDesc* d = bp_node_desc(n->kind);
    if (n->kind <= BPK_EVENT_INTERFACE || n->kind == BPK_EVENT_END_PLAY || n->kind == BPK_EVENT_BEGIN_OVERLAP || n->kind == BPK_EVENT_END_OVERLAP ||
        n->kind == BPK_EVENT_OVERLAPPING || n->kind == BPK_EVENT_DISPATCH || n->kind == BPK_EVENT_TOUCH)
        return TH(TK_NODE_EVENT);
    if (n->kind >= BPK_EVENT_ANY_DAMAGE && n->kind <= BPK_EVENT_SHIELD_BROKEN) return TH(TK_NODE_EVENT); // (the Health's events)
    if (n->kind >= BPK_EVENT_GAME_SAVED && n->kind <= BPK_EVENT_CHECKPOINT) return TH(TK_NODE_EVENT); // (the saves' events)
    if (n->kind >= BPK_EVENT_IA_INTERACT && n->kind <= BPK_EVENT_IA_HOLD) return TH(TK_NODE_EVENT); // (the Interactable's events)
    if (n->kind >= BPK_EVENT_MATCH_PHASE && n->kind <= BPK_EVENT_TIMER_EXPIRED) return TH(TK_NODE_EVENT); // (the Game Rules' events)
    if (n->kind == BPK_FUNC_ENTRY || n->kind == BPK_FUNC_RETURN) return TH(TK_NODE_ENTRY);
    if (n->kind == BPK_MACRO || n->kind == BPK_TUNNEL_IN || n->kind == BPK_TUNNEL_OUT) return TH(TK_NODE_MACRO);
    if (n->kind == BPK_CAST) return TH(TK_NODE_CAST);
    if (graph_orbit() && node_latent(n)) return TH(TK_NODE_LATENT); // (Q5: latent in violet, its clock on the header)
    if (!strncmp(d->category, "Flow Control", 12)) return TH(TK_NODE_FLOW);
    if (d->pure) return TH(TK_NODE_PURE);
    return TH(TK_NODE_FUNCTION);
}

static void node_title(const BpNode* n, char* out, size_t cap) {
    const BpNodeDesc* d = bp_node_desc(n->kind);
    switch (n->kind) {
    case BPK_VAR_GET: snprintf(out, cap, "%s", n->text); break;
    case BPK_VAR_SET: snprintf(out, cap, TR("Set %s"), n->text); break;
    case BPK_TIMELINE: snprintf(out, cap, "%s", n->text[0] ? n->text : "Timeline"); break; // (Unreal: the timeline's name)
    case BPK_EVENT_KEY: snprintf(out, cap, "%s", n->text); break;
    case BPK_DISPATCH_CALL: snprintf(out, cap, TR("Call %s"), n->text); break;
    case BPK_DISPATCH_BIND: snprintf(out, cap, TR("Bind Event to %s"), n->text); break;
    case BPK_DISPATCH_UNBIND: snprintf(out, cap, TR("Unbind Event from %s"), n->text); break;
    case BPK_DISPATCH_UNBIND_ALL: snprintf(out, cap, TR("Unbind all Events from %s"), n->text); break;
    case BPK_EVENT_DISPATCH: snprintf(out, cap, TR("Event %s"), n->text); break;
    case BPK_EVENT_BEGIN_OVERLAP: case BPK_EVENT_END_OVERLAP: case BPK_EVENT_OVERLAPPING:
        if (n->text[0]) snprintf(out, cap, "%s (%s)", n->kind == BPK_EVENT_BEGIN_OVERLAP ? "OnActorBeginOverlap" : n->kind == BPK_EVENT_END_OVERLAP ? "OnActorEndOverlap" : "OnActorOverlapping", n->text);
        else snprintf(out, cap, "%s", TR(d->name));
        break;
    case BPK_EVENT_CUSTOM:
        if (n->net & 3) { // (Unreal's subtitle: where it runs)
            static const char* const nm[4] = { "", "Multicast", "Run on Server", "Run on owning Client" };
            snprintf(out, cap, "%s (%s)", n->text, TR(nm[n->net & 3]));
        } else
            snprintf(out, cap, "%s", n->text);
        break;
    case BPK_GET_COMPONENT: snprintf(out, cap, "%s", n->text); break;
    case BPK_CALL_CUSTOM: snprintf(out, cap, TR("Call %s"), n->text); break;
    case BPK_CAST: snprintf(out, cap, TR("Cast To %s"), n->text); break;
    case BPK_PLUGIN_PURE: case BPK_PLUGIN_CALL: snprintf(out, cap, "%s", n->text); break; // (B14c: the plugin's node's own name)
    case BPK_SPAWN:
    case BPK_SPAWN_ASYNC: {
        const char* cls = "";
        for (int p = 0; p < n->npins; p++)
            if (n->pins[p].type == BPT_CLASS && !n->pins[p].out) cls = n->pins[p].slit;
        if (cls[0]) snprintf(out, cap, TR("Spawn Actor %s%s"), cls, n->kind == BPK_SPAWN_ASYNC ? TR(" (Async)") : "");
        else snprintf(out, cap, "%s", TR(d->name));
        break;
    }
    case BPK_MACRO: snprintf(out, cap, "%s", n->text); break;
    case BPK_TUNNEL_IN: snprintf(out, cap, TR("Inputs")); break;
    case BPK_TUNNEL_OUT: snprintf(out, cap, TR("Outputs")); break;
    case BPK_EVENT_INPUT_ACTION: snprintf(out, cap, TR("EnhancedInputAction %s"), n->text[0] ? n->text : "?"); break;
    case BPK_ADD_MAPPING_CONTEXT: snprintf(out, cap, TR("Add Mapping Context %s"), n->text[0] ? n->text : "?"); break;
    case BPK_REMOVE_MAPPING_CONTEXT: snprintf(out, cap, TR("Remove Mapping Context %s"), n->text[0] ? n->text : "?"); break;
    case BPK_GET_ACTION_VALUE: snprintf(out, cap, "%s", n->text[0] ? n->text : "Get Action Value"); break;
    case BPK_SET_TIMER: snprintf(out, cap, TR("Set Timer by Event %s"), n->text[0] ? n->text : "?"); break;
    case BPK_NET_RPC_SERVER: snprintf(out, cap, TR("Run on Server: %s"), n->text[0] ? n->text : "?"); break;
    case BPK_NET_RPC_MULTICAST: snprintf(out, cap, TR("Multicast: %s"), n->text[0] ? n->text : "?"); break;
    case BPK_NET_RPC_OWNER: snprintf(out, cap, TR("Run on Owning Client: %s"), n->text[0] ? n->text : "?"); break;
    case BPK_NET_REPLICATE_VAR: snprintf(out, cap, TR("Replicate %s"), n->text[0] ? n->text : "?"); break;
    case BPK_CLEAR_TIMER: snprintf(out, cap, TR("Clear Timer %s"), n->text[0] ? n->text : "?"); break;
    case BPK_CALL_FUNCTION:
    case BPK_FUNC_ENTRY:
    case BPK_AXIS: snprintf(out, cap, "%s", n->text); break;
    case BPK_KEY_DOWN: snprintf(out, cap, TR("Is %s Down"), n->text); break;
    case BPK_MAKE_STRUCT: snprintf(out, cap, TR("Make %s"), n->text); break;
    case BPK_BREAK_STRUCT: snprintf(out, cap, TR("Break %s"), n->text); break;
    case BPK_SWITCH_ENUM: snprintf(out, cap, TR("Switch on %s"), n->text); break;
    case BPK_ENUM_LITERAL: snprintf(out, cap, "%s::%s", n->text, n->text2); break;
    case BPK_DATA_GET: snprintf(out, cap, "%s.%s", n->text, n->text2); break;
    case BPK_EVENT_INTERFACE: snprintf(out, cap, TR("Event %s"), n->text); break;
    case BPK_CALL_INTERFACE: snprintf(out, cap, TR("%s (Message)"), n->text2[0] ? n->text2 : n->text); break;
    case BPK_DOES_IMPLEMENT: snprintf(out, cap, TR("Does Implement %s"), n->text); break;
    case BPK_MAT_SCALAR:
    case BPK_MAT_VECTOR: snprintf(out, cap, TR("Set %s (%s)"), n->text[0] ? n->text : "?", n->kind == BPK_MAT_SCALAR ? TR("Scalar") : TR("Vector")); break;
    case BPK_PLAY_MONTAGE: snprintf(out, cap, TR("Play Montage %s"), n->text[0] ? n->text : "?"); break;
    case BPK_PP_SET_FLOAT: snprintf(out, cap, TR("Set %s (Post Process)"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_TEXT: snprintf(out, cap, TR("Set Text %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_PERCENT: snprintf(out, cap, TR("Set Percent %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_VISIBLE: snprintf(out, cap, TR("Set Visible %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_COLOR: snprintf(out, cap, TR("Set Color %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_VALUE: snprintf(out, cap, TR("Set Value %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_GET_VALUE: snprintf(out, cap, TR("Get Value %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_GET_TEXT: snprintf(out, cap, TR("Get Text %s"), n->text[0] ? n->text : "?"); break;
    case BPK_WIDGET_SET_ENABLED: snprintf(out, cap, TR("Set Is Enabled %s"), n->text[0] ? n->text : "?"); break;
    case BPK_PLAY_WIDGET_ANIM: snprintf(out, cap, TR("Play Animation %s"), n->text[0] ? n->text : "?"); break;
    case BPK_STOP_WIDGET_ANIM: snprintf(out, cap, TR("Stop Animation %s"), n->text[0] ? n->text : "?"); break;
    case BPK_PLAY_SOUND_2D: case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_2D: case BPK_SPAWN_SOUND_AT: case BPK_SPAWN_SOUND_ATTACHED:
    case BPK_AUDIO_SET_SOUND: case BPK_PUSH_SOUND_MIX: case BPK_POP_SOUND_MIX: case BPK_SET_BASE_SOUND_MIX: case BPK_SET_CLASS_VOLUME:
    case BPK_PS_SPAWN: case BPK_SPAWN_DECAL:
        snprintf(out, cap, "%s %s", TR(bp_node_desc(n->kind)->name), n->text[0] ? n->text : "?"); // (the asset's name, as Unreal shows it)
        break;
    case BPK_PLAY_SEQUENCE:
    case BPK_STOP_SEQUENCE:
    case BPK_IS_SEQUENCE_PLAYING: { // (the sequence's name, as Unreal shows its asset)
        const char* b = strrchr(n->text, '/');
        char nm[64];
        snprintf(nm, sizeof nm, "%s", b ? b + 1 : n->text);
        char* dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        snprintf(out, cap, "%s %s", n->kind == BPK_PLAY_SEQUENCE ? "Play" : n->kind == BPK_STOP_SEQUENCE ? "Stop" : "Is Playing",
                 nm[0] ? nm : (n->kind == BPK_STOP_SEQUENCE ? "All Sequences" : "?"));
        break;
    }
    case BPK_BB_GET_FLOAT: case BPK_BB_GET_BOOL: case BPK_BB_GET_VECTOR: case BPK_BB_GET_ACTOR:
        snprintf(out, cap, TR("Get BB %s"), n->text[0] ? n->text : "?");
        break;
    case BPK_BB_SET_FLOAT: case BPK_BB_SET_BOOL: case BPK_BB_SET_VECTOR: case BPK_BB_SET_ACTOR:
        snprintf(out, cap, TR("Set BB %s"), n->text[0] ? n->text : "?");
        break;
    case BPK_RUN_BT: snprintf(out, cap, TR("Run Behavior Tree %s"), n->text[0] ? n->text : "?"); break;
    case BPK_PLAY_ANIMATION: snprintf(out, cap, TR("Play Animation %s"), n->text[0] ? n->text : "?"); break;
    default: snprintf(out, cap, "%s", TR(d->name)); break;
    }
}

static bool str_has(const char* hay, const char* needle) {
    // every word of needle in hay, any case (Unreal's search)
    char word[64];
    const char* s = needle;
    while (*s) {
        while (*s == ' ') s++;
        size_t n = 0;
        while (s[n] && s[n] != ' ' && n < sizeof word - 1) word[n] = (char)tolower((unsigned char)s[n]), n++;
        word[n] = 0;
        s += n;
        if (!n) break;
        bool found = false;
        for (const char* h = hay; *h && !found; h++) {
            size_t k = 0;
            while (word[k] && h[k] && tolower((unsigned char)h[k]) == word[k]) k++;
            found = !word[k];
        }
        if (!found) return false;
    }
    return true;
}

// --- Windows ---

void bp_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    BpAsset* a = bp_load(vpath);
    if (!a) return;
    int free_i = -1;
    for (int i = 0; i < MAX_WIN; i++) {
        if (g_win[i].open && g_win[i].a == a) {
            g_win[i].focus = true;
            return;
        }
        if (!g_win[i].open && free_i < 0) free_i = i;
    }
    if (free_i < 0) free_i = 0; // the oldest gives way
    Win* w = &g_win[free_i];
    memset(w, 0, sizeof *w);
    w->a = a;
    w->open = w->focus = w->host = true;
    w->var = w->func = -1;
    if (getenv("REALENGINE_OPEN_TIMELINE")) // (the screenshots: its first Timeline node's editor open)
        for (uint32_t i = 0; a->ngraphs && i < a->graphs[0].nnodes; i++)
            if (a->graphs[0].nodes[i].kind == BPK_TIMELINE) {
                w->tl_open = true, w->tl_graph = 0, w->tl_node = a->graphs[0].nodes[i].id;
                break;
            }
    w->from_pin = -1;
    w->frame = a->ngraphs && a->graphs[0].nnodes && a->graphs[0].pan[0] == 0.0f && a->graphs[0].pan[1] == 0.0f;
    w->vp_grid = w->vp_helpers = true;
    w->comp = -1;
    w->scs = -1;
    // Unreal's: a class with no script yet opens on its Viewport, one with a script on its Event Graph
    bool scripted = false;
    for (uint32_t g = 0; g < a->ngraphs; g++) scripted |= a->graphs[g].nnodes > 3;
    w->want_tab = a->kind == BPA_CLASS && (bp_native_of(a) == BPN_USER_WIDGET || (!scripted && bp_preview_has_look(a))) ? 0 : 1; // (a Widget Blueprint: its Designer)
    if (a->kind == BPA_CLASS && bp_is_child_of(a, "Anim Instance")) w->want_tab = WANT_ANIM_GRAPH; // (Unreal opens an Animation Blueprint on its AnimGraph)
    if (getenv("REALENGINE_BP_MENU")) w->want_tab = 1; // (the test bp_graph_menu: on its Event Graph)
    w->want_hold = 2; // (asked again while the new tabs dock: the first frame's focus goes before they are in the dock)
}

static bool unique_asset_name(const char* folder, const char* base, BpAssetKind kind, char* out, size_t cap);

// Child Blueprint Class (the Content Browser's right click on a class): a new class in its folder whose parent it is
bool bp_editor_create_child(const char* vpath, bool open) {
    BpAsset* p = bp_load(vpath);
    if (!p || p->kind != BPA_CLASS) return false;
    const char* slash = strrchr(vpath, '/');
    if (!slash || strncmp(vpath, "/Content", 8)) return false;
    char folder[600], base[BP_NAME + 8], name[BP_NAME];
    snprintf(folder, sizeof folder, "%.*s", (int)(slash - vpath), vpath);
    snprintf(base, sizeof base, "%s_Child", p->name);
    if (!unique_asset_name(folder, base, BPA_CLASS, name, sizeof name)) return false;
    BpAsset* a = bp_create(BPA_CLASS, folder, name, p->name);
    if (!a || !bp_save(a)) return false;
    char child[700];
    snprintf(child, sizeof child, "%s/%s.%s", folder, name, g_bp_asset_ext[BPA_CLASS]);
    if (open) bp_editor_open(child);
    return true;
}

static void set_status(Win* w, bool bad, const char* fmt, const char* arg) {
    snprintf(w->status, sizeof w->status, fmt, arg);
    w->status_bad = bad;
}

static bool compile(Win* w) {
    BpAsset* a = w->a;
    bool ok = bp_compile(a);
    if (ok) {
        char buf[96];
        snprintf(buf, sizeof buf, TR("Compiled: %u ops, %u values per actor"), a->ncode, a->nslots);
        set_status(w, false, "%s", buf);
    } else {
        set_status(w, true, "%s", a->error);
        // the node with the error: selected and in view
        for (uint32_t g = 0; g < a->ngraphs; g++) {
            BpNode* n = a->error_node ? bp_node(&a->graphs[g], a->error_node) : NULL;
            if (!n) continue;
            w->graph = g;
            w->sel[0] = n->id;
            w->nsel = 1;
            w->details = DET_NODE;
            a->graphs[g].pan[0] = -n->x + 300.0f;
            a->graphs[g].pan[1] = -n->y + 200.0f;
            break;
        }
    }
    return ok;
}

static void save(Win* w, Editor* ed) {
    BpAsset* a = w->a;
    if (a->kind == BPA_CLASS) compile(w);
    else bp_refresh_all(); // the classes that use it get its new fields / entries
    if (!bp_save(a)) {
        set_status(w, true, TR("Could not save %s."), a->path);
        return;
    }
    if (a->kind == BPA_CLASS && ed && !ed->playing) {
        editor_undo_push(ed);
        bp_apply_defaults(&ed->world, a);
    }
    if (!w->status_bad) {
        char buf[160];
        snprintf(buf, sizeof buf, TR("Saved %s."), a->name);
        set_status(w, false, "%s", buf);
    }
}

void bp_editor_save_all(Editor* ed) {
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].open && g_win[i].a->dirty) save(&g_win[i], ed);
}

static bool unique_asset_name(const char* folder, const char* base, BpAssetKind kind, char* out, size_t cap) {
    const char* root = content_root();
    for (int n = 0; n < 1000; n++) {
        if (n) snprintf(out, cap, "%s%d", base, n + 1);
        else snprintf(out, cap, "%s", base);
        char disk[1024];
        snprintf(disk, sizeof disk, "%s%s/%s.%s", root, folder + 8, out, g_bp_asset_ext[kind]);
        FILE* f = fopen(disk, "rb");
        if (!f) return true;
        fclose(f);
    }
    return false;
}

static void create_asset(const char* folder, BpAssetKind kind, const char* parent) {
    static const char* const bases[BPA_COUNT] = { "BP_NewBlueprint", "S_NewStructure", "E_NewEnum", "BPI_NewInterface", "DA_NewDataAsset",
                                                  "BPML_NewMacroLibrary" };
    char name[BP_NAME];
    const char* base = bases[kind];
    if (kind == BPA_CLASS && parent) {
        if (!strcmp(parent, "Character")) base = "BP_Character";
        else if (!strcmp(parent, "Pawn")) base = "BP_Pawn";
        else if (!strcmp(parent, "Game Mode")) base = "BP_GameMode";
        else if (!strcmp(parent, "Actor")) base = "BP_Actor";
        else if (!strcmp(parent, "User Widget")) base = "WBP_NewWidget";
        else if (!strcmp(parent, "Anim Instance")) base = "ABP_New";
    }
    if (kind == BPA_DATA && parent && !strcmp(parent, "Item Definition")) base = "DA_NewItem"; // (inventory.h)
    if (kind == BPA_DATA && parent && !strcmp(parent, "Gameplay Effect")) base = "GE_NewEffect"; // (ability.h)
    if (kind == BPA_CLASS && parent && !strcmp(parent, "Gameplay Ability")) base = "GA_NewAbility";
    if (!unique_asset_name(folder, base, kind, name, sizeof name)) return;
    BpAsset* a = bp_create(kind, folder, name, parent);
    if (!a) return;
    char vpath[600];
    snprintf(vpath, sizeof vpath, "%s/%s.%s", folder, name, g_bp_asset_ext[kind]);
    bp_editor_open(vpath);
}

static char s_pick_folder[600];
// The Pick Parent Class window's answer: a native class (#class/<name>) or a Blueprint of the project
static void parent_picked(const char* ref, uint32_t user, const char* ctx) {
    (void)user;
    if (!ref[0]) return;
    const char* folder = ctx && ctx[0] ? ctx : s_pick_folder;
    if (!strncmp(ref, "#class/", 7)) create_asset(folder, BPA_CLASS, ref + 7);
    else {
        BpAsset* p = bp_load(ref);
        if (p && p->kind == BPA_CLASS) create_asset(folder, BPA_CLASS, p->name);
    }
    content_mark_dirty();
}

bool bp_editor_add_menu(const char* folder, int part) {
    bool made = false, ok = !strncmp(folder, "/Content", 8);
    // Unreal's Pick Parent Class: the common classes, each with its icon and what it is for
    static const UiIcon icons[BPN_COUNT] = { ICON_BLUEPRINT, ICON_PAWN, ICON_CHARACTER, ICON_GAMEMODE, ICON_GAMEPAD, ICON_PAWN,
                                             ICON_GAMEMODE, ICON_PLAYER, ICON_PANEL, ICON_GLOBE, ICON_COMPONENT, ICON_COMPONENT, ICON_SAVE, ICON_CHARACTER,
                                             ICON_LEVEL, ICON_PANEL, ICON_GRAPH };
    static const char* const tips[BPN_COUNT] = {
        "An object that can be placed or spawned in the world.",
        "An actor that can be possessed and receive input from a controller.",
        "A pawn that can walk around: a capsule, Character Movement, a camera.",
        "The rules of the game: who plays, what spawns, how it is won.",
        "Possesses a pawn for the player: input, the camera, the HUD.",
        "Possesses a pawn for the AI: Behavior Trees, perception, moving on the navigation mesh.",
        "What everyone knows about the game while it runs: the score, the time, the phase.",
        "What a player has while the game runs: the name, the score, the team.",
        "Draws on the screen over the game: health, ammunition, messages.",
        "Lives the whole run, across levels: settings and what must not be lost.",
        "A behavior any actor can have (its graph runs on the actor; Get Owner).",
        "A component with a place: hangs in an actor's component tree.",
        "The values a game saves and loads.",
        "An Animation Blueprint: its Event Graph sets variables, its Anim Graph turns them into a pose.",
        "A level's script (each level has its own: Blueprints > Open Level Blueprint).",
        "A Widget Blueprint: a screen of buttons, texts, images and bars (UMG), shown with Create Widget > Add to Viewport.",
        "A Gameplay Ability: a spell, a dash, a heal. Its class defaults set the cost, cooldown, charges, cast time and tags; Event On Activate Ability does it, End Ability ends it.",
    };
    // Unreal's Pick Parent Class: the common ones, then all classes
    static const int common[] = { BPN_ACTOR, BPN_PAWN, BPN_CHARACTER, BPN_PLAYER_CONTROLLER, BPN_GAME_MODE, BPN_ACTOR_COMPONENT, BPN_SCENE_COMPONENT };
    if (part == 2) { // User Interface > Widget Blueprint
        if (ui_menu_item(ICON_PANEL, TR("Widget Blueprint"), NULL, false, ok)) create_asset(folder, BPA_CLASS, "User Widget"), made = true;
        igSetItemTooltip("%s", TR(tips[BPN_USER_WIDGET]));
        if (ui_menu_item(ICON_PANEL, TR("Inventory Widget (WBP_Inventory)"), NULL, false, ok) && inv_widget_asset()) // (inventory.c: Content/UI)
            bp_editor_open("/Content/UI/WBP_Inventory.rbp"), made = true;
        igSetItemTooltip("%s", TR("The ready-made inventory screen: the player's slots and equipment, drag and drop, tips. Copy it into your game and change it."));
        if (ui_menu_item(ICON_PANEL, TR("Ability Bar (WBP_AbilityBar)"), NULL, false, ok) && abil_bar_asset()) // (ability.c: Content/UI)
            bp_editor_open("/Content/UI/WBP_AbilityBar.rbp"), made = true;
        igSetItemTooltip("%s", TR("The ready-made ability bar: the player's abilities with their icons, cooldowns, charges and keys, and the active effects with their time."));
        return made;
    }
    if (part == 3) { // Miscellaneous > Data Asset
        if (ui_menu_item(ICON_DATA, TR("Data Asset"), NULL, false, ok)) create_asset(folder, BPA_DATA, NULL), made = true;
        if (ui_menu_item(ICON_DATA, TR("Item Definition"), NULL, false, ok)) create_asset(folder, BPA_DATA, "Item Definition"), made = true;
        igSetItemTooltip("%s", TR("An item for the Inventory: its name, description, icon, mesh, stack size, weight, tags, equipment slot and value."));
        if (ui_menu_item(ICON_GRAPH, TR("Gameplay Ability"), NULL, false, ok)) create_asset(folder, BPA_CLASS, "Gameplay Ability"), made = true;
        igSetItemTooltip("%s", TR("A spell, a dash, a heal: its cost, cooldown, charges, cast time and tags in its class defaults; Event On Activate Ability does it."));
        if (ui_menu_item(ICON_DATA, TR("Gameplay Effect"), NULL, false, ok)) create_asset(folder, BPA_DATA, "Gameplay Effect"), made = true;
        igSetItemTooltip("%s", TR("A buff or a debuff: instant, for a time or forever; modifiers (Speed x1.5), damage or healing every period, stacks, tags."));
        return made;
    }
    // (Q6) Unreal's Pick Parent Class: one item opens the asset picker with the common classes first, then every native
    // class and the project's Blueprint classes (their thumbnails, a search) instead of a menu listing them all by name
    if (part == 0 && ui_menu_item(ICON_BLUEPRINT, TR("Blueprint Class..."), NULL, false, ok)) {
        snprintf(s_pick_folder, sizeof s_pick_folder, "%s", folder);
        AssetPickerSpec ps = { .title = TR("Pick Parent Class"), .hint = TR("The new Blueprint inherits the class you pick: its components, variables and graphs."),
                               .kinds = AK(ASSET_BLUEPRINT), .on_pick = parent_picked, .ctx = folder };
        asset_picker_open(&ps);
        for (int n = 0; n < BPN_COUNT; n++) {
            bool is_common = false;
            for (size_t i = 0; i < sizeof common / sizeof common[0]; i++) is_common |= common[i] == n;
            char ref[80];
            snprintf(ref, sizeof ref, "#class/%s", g_bp_native_names[n]);
            asset_picker_add(g_bp_native_names[n], ref, ASSET_BLUEPRINT, icons[n], is_common ? 0 : 1, (uint32_t)n + 1);
            asset_picker_add_tip(TR(tips[n]));
        }
    }
    if (part == 0) igSetItemTooltip("%s", TR("Pick the parent class (Actor, Pawn, Character, a Blueprint of yours...): the new Blueprint inherits it."));
    if (part == 1) { // (inside the Content Browser's Blueprint submenu)
        if (ui_menu_item(ICON_STRUCT, TR("Structure"), NULL, false, ok)) create_asset(folder, BPA_STRUCT, NULL), made = true;
        if (ui_menu_item(ICON_ENUM, TR("Enumeration"), NULL, false, ok)) create_asset(folder, BPA_ENUM, NULL), made = true;
        if (ui_menu_item(ICON_INTERFACE, TR("Blueprint Interface"), NULL, false, ok)) create_asset(folder, BPA_INTERFACE, NULL), made = true;
        if (ui_menu_item(ICON_GRAPH, TR("Blueprint Macro Library"), NULL, false, ok)) create_asset(folder, BPA_MACROLIB, NULL), made = true;
    }
    return made;
}

// --- Selection ---

static bool is_sel(const Win* w, uint32_t id) {
    for (uint32_t i = 0; i < w->nsel; i++)
        if (w->sel[i] == id) return true;
    return false;
}
static void sel_add(Win* w, uint32_t id) {
    if (!is_sel(w, id) && w->nsel < MAX_SEL) w->sel[w->nsel++] = id;
}
static void sel_remove(Win* w, uint32_t id) {
    for (uint32_t i = 0; i < w->nsel; i++)
        if (w->sel[i] == id) {
            w->sel[i] = w->sel[--w->nsel];
            return;
        }
}

static void touch(Win* w) {
    w->a->dirty = true;
    w->a->compiled = false;
}

static bool deletable(const Win* w, const BpNode* n) { return !(n->kind == BPK_FUNC_ENTRY && w->graph > 0); }

// (2026-11-24, item 14) The graph's undo (Ctrl+Z / Ctrl+Y over the Blueprint editor's graph): its nodes and links as
// they were before a change of its structure (Split / Recombine Struct Pin, Delete, Organize Nodes); 32 steps
typedef struct GraphStep { const BpAsset* a; int graph; BpNode* nodes; uint32_t nnodes, next_id; BpLink* links; uint32_t nlinks; } GraphStep;
#define GSTEP_MAX 32
static GraphStep g_gundo[GSTEP_MAX], g_gredo[GSTEP_MAX];
static int g_ngundo, g_ngredo;
static void gstep_free(GraphStep* s) {
    free(s->nodes), free(s->links);
    memset(s, 0, sizeof *s);
}
static bool gstep_take(const BpAsset* a, int gi, GraphStep* s) {
    const BpGraph* g = &a->graphs[gi];
    memset(s, 0, sizeof *s);
    s->a = a, s->graph = gi, s->nnodes = g->nnodes, s->nlinks = g->nlinks, s->next_id = g->next_id;
    s->nodes = malloc(sizeof(BpNode) * (g->nnodes ? g->nnodes : 1));
    s->links = malloc(sizeof(BpLink) * (g->nlinks ? g->nlinks : 1));
    if (!s->nodes || !s->links) return gstep_free(s), false;
    memcpy(s->nodes, g->nodes, sizeof(BpNode) * g->nnodes);
    memcpy(s->links, g->links, sizeof(BpLink) * g->nlinks);
    return true;
}
static bool gstep_apply(BpAsset* a, const GraphStep* s) {
    BpGraph* g = &a->graphs[s->graph];
    if (g->cap_nodes < s->nnodes) {
        BpNode* nn = realloc(g->nodes, sizeof(BpNode) * s->nnodes);
        if (!nn) return false;
        g->nodes = nn, g->cap_nodes = s->nnodes;
    }
    if (g->cap_links < s->nlinks) {
        BpLink* nl = realloc(g->links, sizeof(BpLink) * s->nlinks);
        if (!nl) return false;
        g->links = nl, g->cap_links = s->nlinks;
    }
    memcpy(g->nodes, s->nodes, sizeof(BpNode) * s->nnodes);
    memcpy(g->links, s->links, sizeof(BpLink) * s->nlinks);
    g->nnodes = s->nnodes, g->nlinks = s->nlinks, g->next_id = s->next_id;
    a->dirty = true, a->compiled = false;
    return true;
}
static void gstep_push(GraphStep* st, int* n, GraphStep* s) {
    if (*n == GSTEP_MAX) {
        gstep_free(&st[0]);
        memmove(&st[0], &st[1], sizeof st[0] * (GSTEP_MAX - 1));
        (*n)--;
    }
    st[(*n)++] = *s;
}
void bp_graph_undo_push(BpAsset* a, int gi) {
    GraphStep s;
    if (!a || gi < 0 || gi >= (int)a->ngraphs || !gstep_take(a, gi, &s)) return;
    gstep_push(g_gundo, &g_ngundo, &s);
    while (g_ngredo) gstep_free(&g_gredo[--g_ngredo]);
}
// The last step of this asset (the steps of other assets stay); false: none
bool bp_graph_undo(BpAsset* a, bool redo) {
    GraphStep* from = redo ? g_gredo : g_gundo;
    int* nf = redo ? &g_ngredo : &g_ngundo;
    for (int i = *nf - 1; i >= 0; i--) {
        if (from[i].a != a) continue;
        GraphStep cur;
        if (!gstep_take(a, from[i].graph, &cur)) return false;
        bool ok = gstep_apply(a, &from[i]);
        gstep_free(&from[i]);
        memmove(&from[i], &from[i + 1], sizeof from[0] * (size_t)(*nf - i - 1));
        (*nf)--;
        if (redo) gstep_push(g_gundo, &g_ngundo, &cur);
        else gstep_push(g_gredo, &g_ngredo, &cur);
        return ok;
    }
    return false;
}

static void delete_selected(Win* w) {
    BpGraph* g = &w->a->graphs[w->graph];
    bp_graph_undo_push(w->a, w->graph); // (2026-11-24: Ctrl+Z brings them back)
    for (uint32_t i = 0; i < w->nsel; i++) {
        BpNode* n = bp_node(g, w->sel[i]);
        if (n && deletable(w, n)) bp_remove_node(g, w->sel[i]);
    }
    w->nsel = 0;
    w->details = DET_CLASS;
    touch(w);
}

// Copies of the selected nodes 40 units lower right, with the wires between them; the copies selected
static void duplicate_selected(Win* w) {
    BpAsset* a = w->a;
    BpGraph* g = &a->graphs[w->graph];
    uint32_t from[MAX_SEL], to[MAX_SEL], n = 0;
    for (uint32_t i = 0; i < w->nsel; i++) {
        BpNode* s = bp_node(g, w->sel[i]);
        if (!s || !deletable(w, s) || s->kind == BPK_EVENT_BEGIN_PLAY || s->kind == BPK_EVENT_TICK) continue;
        BpNode copy = *s;
        BpNode* d = bp_add_node(a, g, s->kind, s->x + 40.0f, s->y + 40.0f, s->text, s->text2);
        if (!d) break;
        uint32_t id = d->id;
        *d = copy;
        d->id = id;
        d->x += 40.0f, d->y += 40.0f;
        from[n] = copy.id, to[n++] = id;
    }
    uint32_t nl = g->nlinks;
    for (uint32_t l = 0; l < nl; l++) {
        BpLink k = g->links[l];
        int fi = -1, ti = -1;
        for (uint32_t i = 0; i < n; i++) {
            if (from[i] == k.from_node) fi = (int)i;
            if (from[i] == k.to_node) ti = (int)i;
        }
        if (fi >= 0 && ti >= 0) bp_link(g, to[fi], k.from_pin, to[ti], k.to_pin);
    }
    w->nsel = 0;
    for (uint32_t i = 0; i < n; i++) sel_add(w, to[i]);
    touch(w);
}

// --- The action menu ---

static void menu_add(const char* label, const char* cat, uint16_t kind, const char* text, const char* text2) {
    if (g_nmenu >= MAX_MENU) return;
    MenuEntry* e = &g_menu[g_nmenu++];
    snprintf(e->label, sizeof e->label, "%s", label);
    snprintf(e->cat, sizeof e->cat, "%s", cat);
    e->kind = kind;
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    snprintf(e->text2, sizeof e->text2, "%s", text2 ? text2 : "");
}

// Category paths in tree order: '|' (a sub-box) before any letter, so "Game|Save" stays inside "Game" and isn't put
// after "Gameplay Tags" (the box "Game" drawn twice: ImGui's "conflicting ID" warning)
static int cat_cmp(const char* a, const char* b) {
    for (;; a++, b++) {
        unsigned ca = *a == '|' ? 1u : (unsigned char)*a, cb = *b == '|' ? 1u : (unsigned char)*b;
        if (ca != cb || !ca) return (int)ca - (int)cb;
    }
}

static int menu_cmp(const void* x, const void* y) {
    const MenuEntry* a = x;
    const MenuEntry* b = y;
    if (a->group != b->group) return a->group - b->group;
    if (a->group == 1) return a->recent - b->recent;
    int c = cat_cmp(a->cat, b->cat);
    return c ? c : strcmp(a->label, b->label);
}

// A node made in a scratch graph: whether it has a pin a wire from `from` can take (its index, or -1)
static int fits(BpAsset* a, const MenuEntry* e, const BpPin* from) {
    BpGraph tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.next_id = 1;
    bool dirty = a->dirty, compiled = a->compiled;
    BpNode* n = bp_add_node(a, &tmp, e->kind, 0, 0, e->text, e->text2);
    int found = -1;
    for (uint8_t p = 0; n && p < n->npins && found < 0; p++) {
        const BpPin* q = &n->pins[p];
        if (from->out ? bp_types_compatible(from, q) : bp_types_compatible(q, from)) found = p;
    }
    free(tmp.nodes);
    free(tmp.links);
    a->dirty = dirty, a->compiled = compiled;
    return found;
}

static bool graph_has(const BpAsset* a, uint16_t kind) {
    for (uint32_t i = 0; i < a->graphs[0].nnodes; i++)
        if (a->graphs[0].nodes[i].kind == kind) return true;
    return false;
}

// --- What a graph supports and what the menu recommends (graph_menu.h) ---
// The kind of graph open: the Event Graph, a function, a macro, the Construction Script
static GraphMenuKind graph_kind(const Win* w) {
    const BpGraph* g = &w->a->graphs[w->graph];
    if (w->graph == 0) return GM_BP_EVENT;
    if (g->gkind == BPG_MACRO) return GM_BP_MACRO;
    if (!strcmp(g->name, "ConstructionScript")) return GM_BP_CONSTRUCTION;
    return GM_BP_FUNCTION;
}

// Per node kind (found once, by a node made aside): an event (an exec out, no exec in: a graph's entry) and latent (it
// waits: Delay, AI Move To...). A function and the Construction Script take neither; a macro no events
static uint8_t g_kind_class[BPN_KIND_COUNT]; // 1 known, 2 event, 4 latent
static uint8_t kind_class(BpAsset* a, uint16_t kind) {
    if (kind >= BPN_KIND_COUNT) return 1;
    if (g_kind_class[kind]) return g_kind_class[kind];
    uint8_t c = 1;
    const BpNodeDesc* d = bp_node_desc(kind);
    if (d && d->tooltip && (strstr(d->tooltip, "latent") || strstr(d->tooltip, "Latent"))) c |= 4;
    BpGraph tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.next_id = 1;
    bool dirty = a->dirty, compiled = a->compiled;
    BpNode* n = bp_add_node(a, &tmp, kind, 0, 0, "", "");
    if (n) {
        bool exec_in = false, exec_out = false;
        for (uint8_t p = 0; p < n->npins; p++)
            if (n->pins[p].type == BPT_EXEC) {
                if (n->pins[p].out) exec_out = true;
                else exec_in = true;
            }
        if (exec_out && !exec_in && kind != BPK_FUNC_ENTRY && kind != BPK_TUNNEL_IN) c |= 2;
    }
    free(tmp.nodes);
    free(tmp.links);
    a->dirty = dirty, a->compiled = compiled;
    return g_kind_class[kind] = c;
}

// Whether the open graph takes this node kind (Unreal's: an event only in the Event Graph, nothing latent in a
// function or the Construction Script, an Animation Blueprint's own nodes only in one)
static bool graph_supports(Win* w, uint16_t kind) {
    GraphMenuKind gk = graph_kind(w);
    uint8_t c = kind_class(w->a, kind);
    if ((c & 2) && gk != GM_BP_EVENT) return false;
    if ((c & 4) && (gk == GM_BP_FUNCTION || gk == GM_BP_CONSTRUCTION)) return false;
    if ((kind == BPK_EVENT_UPDATE_ANIM || kind == BPK_TRY_GET_PAWN_OWNER) && !bp_is_child_of(w->a, "Anim Instance")) return false;
    return true;
}

// A node's name as the menu and the recommendations know it ("Cast To BP_Enemy", "Print String")
static void node_key(const BpNode* n, char* out, size_t cap) {
    const BpNodeDesc* d = bp_node_desc(n->kind);
    if (n->kind == BPK_CAST) snprintf(out, cap, "Cast To %s", n->text);
    else if (n->kind == BPK_CALL_FUNCTION) snprintf(out, cap, "Call %s", n->text);
    else if (n->kind == BPK_EVENT_KEY) snprintf(out, cap, "Key Event: %s", n->text);
    else if (n->kind == BPK_EVENT_INPUT_ACTION) snprintf(out, cap, "EnhancedInputAction %s", n->text);
    else if (n->kind == BPK_PLUGIN_PURE || n->kind == BPK_PLUGIN_CALL) snprintf(out, cap, "%s", n->text);
    else snprintf(out, cap, "%s", d ? d->name : "");
}

// What the user is doing: the node the wire comes from, else the selected one, else the last placed here
static void menu_context(Win* w, char* out, size_t cap) {
    out[0] = 0;
    BpGraph* g = &w->a->graphs[w->graph];
    const BpNode* n = w->from_pin >= 0 ? bp_node(g, w->from_node) : NULL;
    if (!n && w->nsel) n = bp_node(g, w->sel[w->nsel - 1]);
    if (!n && w->last_placed && w->last_graph == w->graph) n = bp_node(g, w->last_placed);
    if (n) node_key(n, out, cap);
}

static void build_menu(Win* w) {
    BpAsset* a = w->a;
    g_nmenu = 0;
    char label[96];
    for (uint32_t k = 0; k < bp_node_kind_count(); k++) {
        const BpNodeDesc* d = bp_node_desc(k);
        switch (k) {
        case BPK_VAR_GET: case BPK_VAR_SET: case BPK_CALL_FUNCTION: case BPK_FUNC_ENTRY: case BPK_EVENT_KEY: case BPK_AXIS:
        case BPK_KEY_DOWN: case BPK_CAST: case BPK_CALL_CUSTOM: case BPK_EVENT_INTERFACE: case BPK_CALL_INTERFACE:
        case BPK_DOES_IMPLEMENT: case BPK_MAKE_STRUCT: case BPK_BREAK_STRUCT: case BPK_SWITCH_ENUM: case BPK_ENUM_LITERAL:
        case BPK_DATA_GET: case BPK_EVENT_CUSTOM: case BPK_MACRO: case BPK_TUNNEL_IN: case BPK_TUNNEL_OUT: case BPK_SET_TIMER:
        case BPK_CLEAR_TIMER: case BPK_EVENT_INPUT_ACTION: case BPK_ADD_MAPPING_CONTEXT: case BPK_REMOVE_MAPPING_CONTEXT:
        case BPK_GET_ACTION_VALUE: case BPK_DISPATCH_CALL: case BPK_DISPATCH_BIND: case BPK_DISPATCH_UNBIND: case BPK_DISPATCH_UNBIND_ALL:
        case BPK_EVENT_DISPATCH: case BPK_PLUGIN_PURE: case BPK_PLUGIN_CALL: case BPK_GET_COMPONENT:
            continue; // offered by name below
        case BPK_FUNC_RETURN:
            if (w->graph == 0 || a->graphs[w->graph].gkind == BPG_MACRO) continue;
            break;
        case BPK_EVENT_BEGIN_PLAY:
        case BPK_EVENT_TICK:
        case BPK_EVENT_END_PLAY:
            if (w->graph != 0 || graph_has(a, (uint16_t)k)) continue;
            break;
        default: break;
        }
        if (!graph_supports(w, (uint16_t)k)) continue; // (only what this graph takes)
        if (bp_kind_gameplay(k) && !plugins_gameplay_enabled()) continue; // (the Gameplay Framework plugin off: Edit > Plugins)
        menu_add(d->name, d->category, (uint16_t)k, NULL, NULL);
    }
    for (uint32_t i = 0; i < plugin_fn_count(); i++) { // (B14c) the plugins' nodes, each by its name in its category
        const PluginFn* f = plugin_fn_at(i);
        uint16_t k = f->pure ? BPK_PLUGIN_PURE : BPK_PLUGIN_CALL;
        if (graph_supports(w, k)) menu_add(f->name, f->category, k, f->name, NULL);
    }
    if (w->graph == 0) {
        char name[BP_NAME] = "CustomEvent";
        for (int n = 2; n < 100; n++) {
            bool taken = false;
            for (uint32_t i = 0; i < a->graphs[0].nnodes; i++)
                taken |= a->graphs[0].nodes[i].kind == BPK_EVENT_CUSTOM && !strcmp(a->graphs[0].nodes[i].text, name);
            if (!taken) break;
            snprintf(name, sizeof name, "CustomEvent%d", n);
        }
        menu_add("Add Custom Event...", "Events", BPK_EVENT_CUSTOM, name, NULL);
        for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
            snprintf(label, sizeof label, "Key Event: %s", g_keys[i]);
            menu_add(label, "Input|Keyboard Events", BPK_EVENT_KEY, g_keys[i], NULL);
        }
    }
    for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        snprintf(label, sizeof label, "Is %s Down", g_keys[i]);
        menu_add(label, "Input|Keys", BPK_KEY_DOWN, g_keys[i], NULL);
    }
    // Enhanced Input: each action's event and value, each context added or removed
    for (uint32_t i = 0; i < input_action_count(); i++) {
        const InputAction* ia = input_action_at(i);
        if (w->graph == 0) {
            snprintf(label, sizeof label, "EnhancedInputAction %s", ia->name);
            menu_add(label, "Input|Enhanced Action Events", BPK_EVENT_INPUT_ACTION, ia->name, NULL);
        }
        snprintf(label, sizeof label, "Get %s (Action Value)", ia->name);
        menu_add(label, "Input|Enhanced Action Values", BPK_GET_ACTION_VALUE, ia->name, NULL);
    }
    for (uint32_t i = 0; i < input_context_count(); i++) {
        const InputContext* ic = input_context_at(i);
        snprintf(label, sizeof label, "Add Mapping Context %s", ic->name);
        menu_add(label, "Input|Enhanced Input", BPK_ADD_MAPPING_CONTEXT, ic->name, NULL);
        snprintf(label, sizeof label, "Remove Mapping Context %s", ic->name);
        menu_add(label, "Input|Enhanced Input", BPK_REMOVE_MAPPING_CONTEXT, ic->name, NULL);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(label, sizeof label, "Get %s", g_axes[i]);
        menu_add(label, "Input|Axis Values", BPK_AXIS, g_axes[i], NULL);
    }
    // this class's (and its parents') variables, functions and custom events
    for (const BpAsset* c = a; c; c = bp_find_name(BPA_CLASS, c->parent)) {
        for (uint32_t i = 0; i < c->nscs && c->kind == BPA_CLASS; i++) { // (its components: Get <name>)
            snprintf(label, sizeof label, "Get %s", c->scs[i].name);
            menu_add(label, "Components", BPK_GET_COMPONENT, c->scs[i].name, NULL);
        }
        for (uint32_t i = 0; i < c->nvars; i++) {
            snprintf(label, sizeof label, "Get %s", c->vars[i].name);
            menu_add(label, "Variables", BPK_VAR_GET, c->vars[i].name, NULL);
            snprintf(label, sizeof label, "Set %s", c->vars[i].name);
            menu_add(label, "Variables", BPK_VAR_SET, c->vars[i].name, NULL);
        }
        for (uint32_t g = c->kind == BPA_CLASS ? 1 : 0; g < c->ngraphs; g++) {
            if (c->graphs[g].gkind == BPG_MACRO) {
                if (c->graphs[g].name[0] && &c->graphs[g] != &a->graphs[w->graph]) menu_add(c->graphs[g].name, "Macros", BPK_MACRO, c->graphs[g].name, NULL);
                continue;
            }
            if (c->kind != BPA_CLASS) continue;
            if (c->graphs[g].gkind == BPG_DISPATCHER) { // Unreal's Event Dispatcher actions (of this class or a parent: self)
                static const uint16_t dk[5] = { BPK_DISPATCH_CALL, BPK_DISPATCH_BIND, BPK_DISPATCH_UNBIND, BPK_DISPATCH_UNBIND_ALL, BPK_EVENT_DISPATCH };
                static const char* const df[5] = { "Call %s", "Bind Event to %s", "Unbind Event from %s", "Unbind all Events from %s", "Event %s" };
                for (int k = 0; k < 5; k++) {
                    snprintf(label, sizeof label, df[k], c->graphs[g].name);
                    menu_add(label, "Event Dispatchers", dk[k], c->graphs[g].name, NULL);
                }
                continue;
            }
            if (c->kind != BPA_CLASS) continue;
            snprintf(label, sizeof label, "Call %s", c->graphs[g].name);
            menu_add(label, "Functions", BPK_CALL_FUNCTION, c->graphs[g].name, NULL);
        }
        for (uint32_t i = 0; c->ngraphs && c->kind == BPA_CLASS && i < c->graphs[0].nnodes; i++)
            if (c->graphs[0].nodes[i].kind == BPK_EVENT_CUSTOM) {
                snprintf(label, sizeof label, "Call %s", c->graphs[0].nodes[i].text);
                menu_add(label, "Events", BPK_CALL_CUSTOM, c->graphs[0].nodes[i].text, NULL);
                snprintf(label, sizeof label, "Set Timer by Event %s", c->graphs[0].nodes[i].text);
                menu_add(label, "Utilities|Time", BPK_SET_TIMER, c->graphs[0].nodes[i].text, NULL);
                snprintf(label, sizeof label, "Clear Timer %s", c->graphs[0].nodes[i].text);
                menu_add(label, "Utilities|Time", BPK_CLEAR_TIMER, c->graphs[0].nodes[i].text, NULL);
            }
        if (c == bp_find_name(BPA_CLASS, c->parent)) break;
    }
    // the project's assets: classes (cast, spawn), structures, enumerations, interfaces, data assets
    menu_add("Cast To Character", "Utilities|Casting", BPK_CAST, "Character", NULL);
    menu_add("Cast To Pawn", "Utilities|Casting", BPK_CAST, "Pawn", NULL);
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        BpAsset* o = bp_asset_at(i);
        switch (o->kind) {
        case BPA_CLASS:
            snprintf(label, sizeof label, "Cast To %s", o->name);
            menu_add(label, "Utilities|Casting", BPK_CAST, o->name, NULL);
            snprintf(label, sizeof label, "Spawn Actor %s", o->name);
            menu_add(label, "Game", BPK_SPAWN, o->name, NULL); // (place() puts the class in the Class pin)
            break;
        case BPA_MACROLIB:
            for (uint32_t g = 0; g < o->ngraphs; g++)
                if (o->graphs[g].gkind == BPG_MACRO) {
                    snprintf(label, sizeof label, "%s", o->graphs[g].name);
                    menu_add(label, "Macros", BPK_MACRO, o->graphs[g].name, o->name);
                }
            break;
        case BPA_STRUCT:
            snprintf(label, sizeof label, "Make %s", o->name);
            menu_add(label, "Structures", BPK_MAKE_STRUCT, o->name, NULL);
            snprintf(label, sizeof label, "Break %s", o->name);
            menu_add(label, "Structures", BPK_BREAK_STRUCT, o->name, NULL);
            break;
        case BPA_ENUM:
            snprintf(label, sizeof label, "Switch on %s", o->name);
            menu_add(label, "Flow Control", BPK_SWITCH_ENUM, o->name, NULL);
            for (uint32_t k = 0; k < o->nentries; k++) {
                snprintf(label, sizeof label, "%s::%s", o->name, o->entries[k]);
                menu_add(label, "Enumerations", BPK_ENUM_LITERAL, o->name, o->entries[k]);
            }
            break;
        case BPA_INTERFACE: {
            bool mine = false;
            for (uint32_t k = 0; k < a->ninterfaces; k++) mine |= !strcmp(a->interfaces[k], o->name);
            for (uint32_t k = 0; k < o->ngraphs; k++) {
                const BpGraph* fg = &o->graphs[k];
                snprintf(label, sizeof label, "%s (Message)", fg->name);
                menu_add(label, "Interfaces", BPK_CALL_INTERFACE, o->name, fg->name);
                if (mine && w->graph == 0 && !fg->nresults) { // (with outputs: implemented as a function, My Blueprint)
                    snprintf(label, sizeof label, "Event %s", fg->name);
                    menu_add(label, "Interfaces", BPK_EVENT_INTERFACE, fg->name, o->name);
                }
            }
            snprintf(label, sizeof label, "Does Implement %s", o->name);
            menu_add(label, "Interfaces", BPK_DOES_IMPLEMENT, o->name, NULL);
            break;
        }
        case BPA_DATA:
            for (uint32_t k = 0; k < o->nvars; k++) {
                snprintf(label, sizeof label, "%s.%s", o->name, o->vars[k].name);
                menu_add(label, "Data", BPK_DATA_GET, o->name, o->vars[k].name);
            }
            break;
        default: break;
        }
    }
    // the order: from a pin, what takes its wire first (Unreal's Context Sensitive); then the recently used; then the rest
    BpNode* f = w->from_pin >= 0 ? bp_node(&a->graphs[w->graph], w->from_node) : NULL;
    if (f && w->from_pin >= f->npins) f = NULL;
    for (uint32_t i = 0; i < g_nmenu; i++) {
        MenuEntry* e = &g_menu[i];
        e->recent = (int8_t)recent_rank(e->kind, e->text, e->text2);
        e->group = f && fits(a, e, &f->pins[w->from_pin]) >= 0 ? 0 : 2;
    }
    qsort(g_menu, g_nmenu, sizeof g_menu[0], menu_cmp);
}

static void place(Win* w, const MenuEntry* e) {
    BpAsset* a = w->a;
    BpGraph* g = &a->graphs[w->graph];
    recent_add(e->kind, e->text, e->text2);
    { // the recents and the pairs learned (graph_menu.h): this one after what was being done
        char ctx[96];
        menu_context(w, ctx, sizeof ctx);
        gm_note(graph_kind(w), ctx, e->label);
    }
    bool spawn = e->kind == BPK_SPAWN || e->kind == BPK_SPAWN_ASYNC;
    BpNode* n = bp_add_node(a, g, e->kind, w->menu_g[0], w->menu_g[1], spawn ? NULL : e->text, e->text2);
    if (!n) return;
    if (e->kind == BPK_TIMELINE && !n->text[0]) { // (2026-11-16) a new Timeline of the class, opened in its editor
        BpTimeline* tl = bp_timeline_add(a);
        if (tl) {
            snprintf(n->text, sizeof n->text, "%s", tl->name);
            bp_refresh_pins(a, g, n);
            w->tl_open = true, w->tl_graph = w->graph, w->tl_node = n->id;
        }
    }
    if (spawn && e->text[0]) { // "Spawn Actor <Class>": the class in its Class pin, the Return Value of it
        for (uint8_t p = 0; p < n->npins; p++)
            if (n->pins[p].type == BPT_CLASS) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", e->text);
        bp_refresh_pins(a, g, n);
    }
    if (e->kind == BPK_TUNNEL_IN || e->kind == BPK_TUNNEL_OUT || (e->kind == BPK_FUNC_RETURN && !n->text[0]))
        snprintf(n->text, sizeof n->text, "%s", g->name), bp_refresh_pins(a, g, n);
    uint32_t id = n->id;
    if (w->from_pin >= 0) {
        BpNode* f = bp_node(g, w->from_node);
        if (f && w->from_pin < f->npins) {
            int p = fits(a, e, &f->pins[w->from_pin]);
            n = bp_node(g, id);
            if (p >= 0 && n) {
                if (!f->pins[w->from_pin].out) n->x -= 220.0f; // to the left of the input it feeds
                bp_link(g, w->from_node, (uint8_t)w->from_pin, id, (uint8_t)p);
            }
        }
    }
    w->nsel = 0;
    sel_add(w, id);
    w->last_placed = id, w->last_graph = w->graph;
    w->details = DET_NODE;
    touch(w);
}

// The menu's entry for a recent or recommended name: its label, a pattern's start ("Cast To*"), or its kind's name
static MenuEntry* entry_named(const char* item) {
    size_t l = strlen(item);
    bool prefix = l && item[l - 1] == '*';
    for (uint32_t i = 0; i < g_nmenu; i++)
        if (prefix ? !strncmp(g_menu[i].label, item, l - 1) : !strcmp(g_menu[i].label, item)) return &g_menu[i];
    return NULL;
}

typedef struct RecUser { Win* w; MenuEntry* shown[4]; int nshown; } RecUser;
// A recommendation the graph has, not shown above, and (from a wire) one that connects to it
static bool rec_accept(const char* item, void* user) {
    RecUser* u = user;
    MenuEntry* e = entry_named(item);
    if (!e) return false;
    for (int i = 0; i < u->nshown; i++)
        if (u->shown[i] == e) return false;
    if (u->w->from_pin >= 0 && e->group != 0) return false;
    return true;
}

// The two sections on top (no search typed): the last 2 used in this kind of graph, and 2 recommended that connect
// with what is being done. Returns true when one was placed
static bool menu_top(Win* w) {
    RecUser u = { w, { 0 }, 0 };
    const char* items[8];
    int n = gm_recent(graph_kind(w), items, 8), shown = 0;
    bool placed = false;
    for (int i = 0; i < n && shown < GM_RECENT_SHOWN && !placed; i++) {
        MenuEntry* e = entry_named(items[i]);
        if (!e || (w->from_pin >= 0 && e->group != 0)) continue; // (not in this graph, or doesn't take the wire)
        if (!shown) {
            igPushStyleColor_U32(ImGuiCol_Text, TH(TK_OK));
            igSeparatorText(gm_title_recent());
            igPopStyleColor(1);
        }
        igPushID_Str("recent"), igPushID_Int(shown);
        if (igSelectable_Bool(TR(e->label), false, 0, (ImVec2){ 0, 0 })) place(w, e), placed = true;
        igPopID(), igPopID();
        u.shown[u.nshown++] = e, shown++;
    }
    char ctx[96];
    menu_context(w, ctx, sizeof ctx);
    n = placed ? 0 : gm_recommend(graph_kind(w), ctx, rec_accept, &u, items, GM_RECOMMEND_SHOWN);
    for (int i = 0; i < n && !placed; i++) {
        MenuEntry* e = entry_named(items[i]);
        if (!e) continue;
        if (!i) {
            igPushStyleColor_U32(ImGuiCol_Text, TH(TK_INFO));
            igSeparatorText(gm_title_recommended());
            igPopStyleColor(1);
        }
        igPushID_Str("recommended"), igPushID_Int(i);
        if (igSelectable_Bool(TR(e->label), false, 0, (ImVec2){ 0, 0 })) place(w, e), placed = true;
        if (ctx[0]) igSetItemTooltip(TR("Recommended after %s"), ctx);
        igPopID(), igPopID();
    }
    return placed;
}

// The graph's menus keep the editor's font size: the canvas pushes one scaled by the zoom. Its base is kept before
// that push, since ImGui 1.92 copies every pushed size into style.FontSizeBase (reading it inside the canvas gave the
// zoomed size back: the menus shrank with the zoom)
static float g_font_base = 13.0f;
static bool popup_begin(const char* id) {
    if (!igBeginPopup(id, 0)) return false;
    igPushFont(NULL, g_font_base);
    return true;
}
static void popup_end(void) {
    igPopFont();
    igEndPopup();
}

static void action_menu(Win* w) {
    if (!popup_begin("actions")) return;
    if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
    igSetNextItemWidth(400.0f);
    igInputTextWithHint("##search", TR("Search"), w->search, sizeof w->search, 0, NULL, NULL);
    bool enter = igIsKeyPressed_Bool(ImGuiKey_Enter, false);
    igTextDisabled("%s", w->from_pin >= 0 ? TR("What fits the wire, the recent and recommended ones first") : TR("This graph's actions: the recent and recommended ones first"));
    igBeginChild_Str("list", (ImVec2){ 400.0f, 460.0f }, 0, 0);
    if (!w->search[0] && menu_top(w)) { // (the recent and recommended ones, on top)
        igEndChild();
        igCloseCurrentPopup();
        popup_end();
        return;
    }
    // the sections (Compatible, Recently Used, All Actions), each a tree of categories in boxes that open and close
    // (Unreal's): "Utilities|Time" inside "Utilities"; a search opens them all, showing only the matches inside
    int shown = 0, group = -1, npath = 0;
    char path[8][48];
    bool open[8], pushed[8];
    for (uint32_t i = 0; i < g_nmenu; i++) {
        MenuEntry* e = &g_menu[i];
        char hay[400]; // (2026-11-25, item 12) found by its English name and category, and by the editor's language's
        int hl = snprintf(hay, sizeof hay, "%s %s %s", e->label, e->cat, TR(e->label));
        for (const char* c = e->cat; *c && hl > 0 && hl < (int)sizeof hay - 2;) {
            const char* bar = strchr(c, '|');
            char part[64];
            snprintf(part, sizeof part, "%.*s", (int)(bar ? (size_t)(bar - c) : strlen(c)), c);
            hl += snprintf(hay + hl, sizeof hay - (size_t)hl, " %s", TR(part));
            c = bar ? bar + 1 : c + strlen(c);
        }
        if (w->search[0] && !str_has(hay, w->search)) continue;
        if (e->group != group) { // the sections: Compatible, Recently Used, All Actions
            while (npath > 0)
                if (pushed[--npath]) igTreePop();
            group = e->group;
            igPushStyleColor_U32(ImGuiCol_Text, group == 0 ? TH(TK_WARN) : group == 1 ? TH(TK_OK) : TH(TK_N14));
            igSeparatorText(group == 0 ? TR("Compatible") : group == 1 ? TR("Recently Used") : TR("All Actions"));
            igPopStyleColor(1);
        }
        char parts[8][48];
        int np = 0;
        if (group != 1) { // (the recent ones without categories)
            const char* c = e->cat;
            while (*c && np < 8) {
                const char* bar = strchr(c, '|');
                size_t l = bar ? (size_t)(bar - c) : strlen(c);
                snprintf(parts[np++], sizeof parts[0], "%.*s", (int)(l < 47 ? l : 47), c);
                c = bar ? bar + 1 : c + l;
            }
        }
        int common = 0;
        while (common < npath && common < np && !strcmp(path[common], parts[common])) common++;
        while (npath > common)
            if (pushed[--npath]) igTreePop();
        while (npath < np) {
            bool parent = npath == 0 || open[npath - 1];
            bool o = false;
            if (parent) {
                if (w->search[0]) igSetNextItemOpen(true, ImGuiCond_Always);
                else if (group == 0) igSetNextItemOpen(true, ImGuiCond_Appearing); // (what fits the wire: open)
                char lab[80];
                snprintf(lab, sizeof lab, "%s##g%d_%s", TR(parts[npath]), group, parts[npath]); // (the id stays English)
                igPushStyleColor_U32(ImGuiCol_Header, npath ? THEME_NONE : TH(TK_N7));
                igPushStyleColor_U32(ImGuiCol_Text, npath ? TH(TK_N14) : TH(TK_N15));
                o = igTreeNodeEx_Str(lab, ImGuiTreeNodeFlags_SpanAvailWidth | (npath ? 0 : ImGuiTreeNodeFlags_Framed));
                igPopStyleColor(2);
            }
            snprintf(path[npath], sizeof path[0], "%s", parts[npath]);
            open[npath] = o, pushed[npath] = o;
            npath++;
        }
        if (np && !open[np - 1]) continue; // (inside a closed box)
        igPushID_Int((int)i);
        bool first = shown == 0 && w->search[0];
        if (igSelectable_Bool(TR(e->label), first, 0, (ImVec2){ 0, 0 }) || (enter && shown == 0)) {
            place(w, e);
            igCloseCurrentPopup();
            igPopID();
            break;
        }
        const BpNodeDesc* d = bp_node_desc(e->kind);
        if (d && d->tooltip) igSetItemTooltip("%s", TR(d->tooltip));
        igPopID();
        shown++;
    }
    while (npath > 0)
        if (pushed[--npath]) igTreePop();
    if (!shown) igTextDisabled("%s", TR("No results."));
    igEndChild();
    popup_end();
}

static void open_actions(Win* w, float gx, float gy, uint32_t from_node, int from_pin) {
    w->menu_g[0] = gx, w->menu_g[1] = gy;
    w->from_node = from_node;
    w->from_pin = from_pin;
    w->search[0] = 0;
    build_menu(w);
    igOpenPopup_Str("actions", 0);
}

// --- The graph ---

static float lit_width(const BpPin* p, float z) {
    if (p->cont) return 0.0f;
    switch (p->type) {
    case BPT_BOOL: return igGetFontSize() + 6.0f * z;
    case BPT_INT:
    case BPT_BYTE:
    case BPT_FLOAT: return 58.0f * z;
    case BPT_INT64: return 90.0f * z;
    case BPT_ENUM: return 96.0f * z;
    case BPT_VECTOR:
    case BPT_ROTATOR: return 150.0f * z;
    case BPT_COLOR: return igGetFontSize() * 2.0f + 6.0f * z;
    case BPT_STRING:
    case BPT_NAME:
    case BPT_TEXT: return 120.0f * z;
    case BPT_CLASS: return 130.0f * z;
    case BPT_ACTOR: return igCalcTextSize("self", NULL, false, -1).x + 4.0f * z;
    default: return 0.0f;
    }
}

// Unreal's variable getters: no header, the name inside a pill with the value's pin
static bool compact(const BpNode* n) { return n->kind == BPK_VAR_GET; }

// The icon in front of the title (Unreal's: events, functions, casts, macros); ICON_COUNT: none
// (Q5) a latent node (it finishes later: Delay, Timeline, an async spawn, a node with a Completed / Finished pin)
static bool node_latent(const BpNode* n) {
    if (n->kind == BPK_DELAY || n->kind == BPK_RETRIGGER_DELAY || n->kind == BPK_TIMELINE || n->kind == BPK_SPAWN_ASYNC) return true;
    for (uint8_t p = 0; p < n->npins; p++)
        if (n->pins[p].out && n->pins[p].type == BPT_EXEC &&
            (!strcmp(n->pins[p].name, "Completed") || !strcmp(n->pins[p].name, "On Completed") || !strcmp(n->pins[p].name, "Finished") ||
             !strcmp(n->pins[p].name, "On Finished")))
            return true;
    return false;
}

static UiIcon header_icon(const BpNode* n) {
    const BpNodeDesc* d = bp_node_desc(n->kind);
    if (n->kind <= BPK_EVENT_INTERFACE || n->kind == BPK_EVENT_TOUCH) return ICON_EVENT;
    if (n->kind >= BPK_EVENT_ANY_DAMAGE && n->kind <= BPK_EVENT_SHIELD_BROKEN) return ICON_EVENT;
    if (n->kind >= BPK_EVENT_GAME_SAVED && n->kind <= BPK_EVENT_CHECKPOINT) return ICON_EVENT;
    if (n->kind == BPK_FUNC_ENTRY || n->kind == BPK_FUNC_RETURN || n->kind == BPK_CALL_FUNCTION || n->kind == BPK_CALL_CUSTOM)
        return ICON_FUNCTION;
    if (n->kind == BPK_CAST) return ICON_BLUEPRINT;
    if (n->kind == BPK_CALL_INTERFACE || n->kind == BPK_DOES_IMPLEMENT) return ICON_INTERFACE;
    if (n->kind == BPK_VAR_SET) return ICON_VARIABLE;
    if (graph_orbit()) { // (Q5) every family its icon: latent the clock, macro, flow, pure
        if (node_latent(n)) return ICON_TIMER;
        if (n->kind == BPK_MACRO || n->kind == BPK_TUNNEL_IN || n->kind == BPK_TUNNEL_OUT) return ICON_MACRO;
        if (!strncmp(d->category, "Flow Control", 12)) return ICON_FLOW;
        if (d->pure) return ICON_PURE;
    }
    if (!strncmp(d->category, "Flow Control", 12)) return ICON_COUNT;
    return ICON_FUNCTION;
}

static void node_geo(const BpGraph* g, const BpNode* n, ImVec2 o, const float pan[2], float z, Geo* out) {
    if (n->kind == BPK_REROUTE) { // a knot: its two pins side by side
        out->header = 0.0f;
        out->pos = (ImVec2){ o.x + (n->x + pan[0]) * z, o.y + (n->y + pan[1]) * z };
        out->size = (ImVec2){ 30.0f * z, 16.0f * z };
        for (uint8_t p = 0; p < n->npins; p++) {
            out->lit_w[p] = 0.0f;
            out->pin[p] = (ImVec2){ out->pos.x + (n->pins[p].out ? out->size.x - 7.0f * z : 7.0f * z), out->pos.y + out->size.y * 0.5f };
            out->label_x[p] = out->pos.x;
        }
        return;
    }
    float fs = igGetFontSize();
    float row = fs + 9.0f * z, header = compact(n) ? 0.0f : fs + 12.0f * z;
    char title[128];
    node_title(n, title, sizeof title);
    float tw = igCalcTextSize(title, NULL, false, -1).x + (header_icon(n) != ICON_COUNT ? fs + 4.0f * z : 0.0f) + 34.0f * z;
    for (uint8_t p = 0; p < n->npins; p++)
        if (n->pins[p].delegate && n->pins[p].out) { tw += 16.0f * z; break; } // (room for the delegate in the header)
    if (compact(n)) { // the name and the pin on one row
        out->header = 0.0f;
        out->pos = (ImVec2){ o.x + (n->x + pan[0]) * z, o.y + (n->y + pan[1]) * z };
        out->size = (ImVec2){ igCalcTextSize(title, NULL, false, -1).x + 40.0f * z, row + 4.0f * z };
        for (uint8_t p = 0; p < n->npins; p++) {
            out->lit_w[p] = 0.0f;
            out->pin[p] = (ImVec2){ out->pos.x + out->size.x - 10.0f * z, out->pos.y + out->size.y * 0.5f };
            out->label_x[p] = out->pos.x;
        }
        return;
    }
    float in_w = 0, out_w = 0;
    int nin = 0, nout = 0;
    for (uint8_t p = 0; p < n->npins; p++) {
        const BpPin* q = &n->pins[p];
        float lw = q->name[0] ? igCalcTextSize(TR(q->name), NULL, false, -1).x : 0.0f;
        out->lit_w[p] = 0.0f;
        if (q->delegate && q->out && header > 0) continue; // (in the header, as in Unreal)
        if (q->out) {
            out_w = fmaxf(out_w, lw + 18.0f * z);
            nout++;
        } else {
            if (q->type != BPT_EXEC && !bp_pin_linked(g, n->id, p) && z >= 0.55f) out->lit_w[p] = lit_width(q, z);
            in_w = fmaxf(in_w, 18.0f * z + lw + (lw > 0 ? 6.0f * z : 0) + out->lit_w[p]);
            nin++;
        }
    }
    float w = fmaxf(tw, in_w + out_w + 24.0f * z);
    int rows = nin > nout ? nin : nout;
    out->header = header;
    out->pos = (ImVec2){ o.x + (n->x + pan[0]) * z, o.y + (n->y + pan[1]) * z };
    out->size = (ImVec2){ w, header + rows * row + 6.0f * z };
    int ii = 0, oi = 0;
    for (uint8_t p = 0; p < n->npins; p++) {
        const BpPin* q = &n->pins[p];
        if (q->delegate && q->out && header > 0) { // the event's delegate: the red square at the header's right
            out->pin[p] = (ImVec2){ out->pos.x + w - 11.0f * z, out->pos.y + header * 0.5f };
            out->label_x[p] = out->pos.x;
            continue;
        }
        int r = q->out ? oi++ : ii++;
        float y = out->pos.y + header + r * row + row * 0.5f;
        out->pin[p] = (ImVec2){ q->out ? out->pos.x + w : out->pos.x, y };
        float lw = q->name[0] ? igCalcTextSize(TR(q->name), NULL, false, -1).x : 0.0f;
        out->label_x[p] = q->out ? out->pos.x + w - 12.0f * z - lw : out->pos.x + 14.0f * z;
    }
}

static void wire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float z, bool exec) {
    graph_wire(dl, a, b, col, z, exec, -1); // (Editor Preferences > Graph Style: the wires' style)
}
static void wire2(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 ca, ImU32 cb, float z, bool exec) {
    graph_wire2(dl, a, b, ca, cb, z, exec, -1); // (Q5: the Gradient style goes from the output's color to the input's)
}

// The mouse's distance to a wire (the same curve as wire(): a few points along it)
static float wire_dist(ImVec2 a, ImVec2 b, ImVec2 m, float z) {
    if (graph_link_style() != GS_LINK_CURVE) return graph_wire_dist(a, b, m, z, -1); // (its own shape)
    float dx = fmaxf(fabsf(b.x - a.x) * 0.5f, 40.0f * z), best = 1e30f;
    ImVec2 c1 = { a.x + dx, a.y }, c2 = { b.x - dx, b.y };
    for (int i = 0; i <= 24; i++) {
        float t = (float)i / 24.0f, u = 1.0f - t;
        float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        float x = w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x - m.x, y = w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y - m.y;
        best = fminf(best, x * x + y * y);
    }
    return sqrtf(best);
}

// A wire from (node, pin) into the node `to`: its first pin that takes it, an unlinked one of the same type first
// (a wire let go on a node's body, a node made by a shortcut while dragging)
static bool link_into(BpGraph* g, uint32_t node, uint8_t pin, uint32_t to) {
    BpNode* f = bp_node(g, node);
    BpNode* t = bp_node(g, to);
    if (!f || !t || pin >= f->npins || node == to) return false;
    for (int pass = 0; pass < 2; pass++)
        for (uint8_t p = 0; p < t->npins; p++) {
            if (t->pins[p].out == f->pins[pin].out) continue;
            if (!pass && (t->pins[p].type != f->pins[pin].type || bp_pin_linked(g, to, p))) continue;
            if (bp_link(g, node, pin, to, p)) return true;
        }
    return false;
}

static ImU32 scale_rgb(ImU32 c, float k, uint32_t alpha) {
    uint32_t r = (uint32_t)fminf((float)(c & 0xFF) * k, 255.0f), g = (uint32_t)fminf((float)((c >> 8) & 0xFF) * k, 255.0f);
    uint32_t b = (uint32_t)fminf((float)((c >> 16) & 0xFF) * k, 255.0f);
    return r | (g << 8) | (b << 16) | (alpha << 24);
}

// A node's header: rounded on top, its color bright on the left fading darker to the right (Unreal's gloss)
static void header_fill(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 col) {
    ImDrawList_PathClear(dl);
    ImDrawList_PathLineTo(dl, (ImVec2){ p0.x, p1.y });
    ImDrawList_PathArcToFast(dl, (ImVec2){ p0.x + r, p0.y + r }, r, 6, 9);
    ImDrawList_PathArcToFast(dl, (ImVec2){ p1.x - r, p0.y + r }, r, 9, 12);
    ImDrawList_PathLineTo(dl, (ImVec2){ p1.x, p1.y });
    int n = dl->_Path.Size;
    ImVec2 uv = igGetFontTexUvWhitePixel();
    ImDrawList_PrimReserve(dl, (n - 2) * 3, n);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    float w = fmaxf(p1.x - p0.x, 1.0f);
    for (int i = 0; i < n; i++) {
        ImVec2 q = dl->_Path.Data[i];
        float t = (q.x - p0.x) / w, k = 1.0f - 0.55f * t, top = 1.0f + 0.25f * (1.0f - (q.y - p0.y) / fmaxf(p1.y - p0.y, 1.0f));
        ImDrawList_PrimWriteVtx(dl, q, uv, scale_rgb(col, k * top, (uint32_t)(250.0f - 60.0f * t)));
    }
    for (int i = 1; i + 1 < n; i++) {
        ImDrawList_PrimWriteIdx(dl, base);
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + i));
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + i + 1));
    }
    ImDrawList_PathClear(dl);
}

static void draw_pin(ImDrawList* dl, ImVec2 c, const BpPin* p, bool linked, float z) {
    ImU32 col = type_color(p->type);
    if (graph_orbit()) { // (Q5) the family by its shape too: value a circle, Array a bar, Set a double ring, Map two circles
        int shape = p->delegate ? GP_DELEGATE : p->cont == BPC_ARRAY ? GP_ARRAY : p->cont == BPC_SET ? GP_SET : p->cont == BPC_MAP ? GP_MAP
                    : p->type == BPT_EXEC ? GP_EXEC : p->type == BPT_CLASS ? GP_CLASS : GP_VALUE;
        graph_pin(dl, c, shape, p->delegate ? TH(TK_PIN_DELEGATE) : col, type_color(p->vtype), linked, z);
        return;
    }
    float r = 5.0f * z;
    if (p->cont == BPC_ARRAY || p->cont == BPC_SET) { // Unreal's: a grid of squares (an array), braces-like ring (a set)
        float s = r * 0.62f, g = s * 1.25f;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                if (p->cont == BPC_SET && x == 0 && y == 0) continue;
                ImVec2 q0 = { c.x + x * g - s * 0.45f, c.y + y * g - s * 0.45f }, q1 = { q0.x + s * 0.9f, q0.y + s * 0.9f };
                if (linked || (x == 0 && y == 0)) ImDrawList_AddRectFilled(dl, q0, q1, col, 0, 0);
                else ImDrawList_AddRect(dl, q0, q1, col, 0, 1.0f * z, 0);
            }
        return;
    }
    if (p->cont == BPC_MAP) { // the key's color and the value's
        ImU32 vc = type_color(p->vtype);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ c.x - r * 0.55f, c.y }, r * 0.7f, col, 12);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ c.x + r * 0.55f, c.y }, r * 0.7f, vc, 12);
        if (!linked) ImDrawList_AddCircle(dl, c, r * 1.25f, col, 16, 1.0f * z);
        return;
    }
    if (p->delegate) { // Unreal's delegate: a small red square, filled when wired
        float h = 4.5f * z;
        ImU32 red = TH(TK_RED);
        if (linked) ImDrawList_AddRectFilled(dl, (ImVec2){ c.x - h, c.y - h }, (ImVec2){ c.x + h, c.y + h }, red, 1.0f * z, 0);
        else ImDrawList_AddRect(dl, (ImVec2){ c.x - h, c.y - h }, (ImVec2){ c.x + h, c.y + h }, red, 1.0f * z, 1.6f * z, 0);
        return;
    }
    if (p->type == BPT_EXEC) {
        r = 6.0f * z;
        ImVec2 pts[5] = { { c.x - r, c.y - r }, { c.x + r * 0.15f, c.y - r }, { c.x + r * 1.1f, c.y }, { c.x + r * 0.15f, c.y + r }, { c.x - r, c.y + r } };
        if (linked) ImDrawList_AddConvexPolyFilled(dl, pts, 5, col);
        else ImDrawList_AddPolyline(dl, pts, 5, col, 1.6f * z, ImDrawFlags_Closed);
        return;
    }
    if (linked) ImDrawList_AddCircleFilled(dl, c, r, col, 16);
    else {
        ImDrawList_AddCircleFilled(dl, c, r, TH(TK_N1), 16);
        ImDrawList_AddCircle(dl, c, r, col, 16, 1.7f * z);
    }
    // Unreal's little arrow beside a value pin: the way the value flows
    float ax = c.x + r + 2.5f * z;
    ImDrawList_AddTriangleFilled(dl, (ImVec2){ ax, c.y - 2.6f * z }, (ImVec2){ ax + 3.0f * z, c.y }, (ImVec2){ ax, c.y + 2.6f * z }, col);
}

static bool g_dbg_trace; // (the window drawn now plays: the debugger's traces and values show)
static void dbg_node_marks(Win* w, ImDrawList* dl, const BpNode* n, ImVec2 p0, ImVec2 p1, float r, float z);
static void organize_nodes(Win* w); // (2026-11-23, below)
static void dbg_wire(Win* w, ImDrawList* dl, const BpNode* to, int to_pin, ImVec2 a0, ImVec2 b0, float z);
static void dbg_pin_menu(Win* w, const BpNode* n, int pin);
static void dbg_node_menu(Win* w, const BpNode* sn);
static void dbg_toggle_selected(Win* w);
static void dbg_toolbar(Win* w, Editor* ed);
static void dbg_frame(Editor* ed);
static uint16_t g_lit_kind; // (the node whose pin lit_widget draws: a tag node's name pins pick from the project's tags)
static bool tag_node(uint16_t k) {
    return k == BPK_TAG_ADD || k == BPK_TAG_REMOVE || k == BPK_TAG_HAS || k == BPK_TAG_HAS_EXACT || k == BPK_TAG_MATCHES ||
           k == BPK_GET_ALL_WITH_TAG || k == BPK_GET_ALL_WITH_GTAG;
}
static bool lit_widget(Win* w, BpPin* p, float width) {
    bool ch = false;
    if (p->type == BPT_NAME && tag_node(g_lit_kind)) { // (Unreal's tag drop-down: the project's tree with a search)
        ch = gt_tag_picker("##l", p->slit, sizeof p->slit, width);
        if (ch) touch(w);
        return ch;
    }
    igSetNextItemWidth(width);
    switch (p->type) {
    case BPT_BOOL: {
        bool b = p->lit.v[0] != 0.0f;
        if (igCheckbox("##pin_bool", &b)) p->lit.v[0] = b ? 1.0f : 0.0f, ch = true;
        break;
    }
    case BPT_INT: {
        extern const char* const* bp_builtin_enum(const char* name, int* count);
        int ne = 0;
        const char* const* en = bp_builtin_enum(p->ref, &ne); // (2026-11-19: Trace Channel, Draw Debug Type)
        if (en) {
            int cur = (int)p->lit.v[0];
            if (igBeginCombo("##l", cur >= 0 && cur < ne ? TR(en[cur]) : "?", 0)) {
                for (int k = 0; k < ne; k++)
                    if (igSelectable_Bool(TR(en[k]), k == cur, 0, (ImVec2){ 0, 0 })) p->lit.v[0] = (float)k, ch = true;
                igEndCombo();
            }
            break;
        }
        if (igDragFloat("##l", &p->lit.v[0], 0.2f, 0, 0, "%.0f", 0)) p->lit.v[0] = roundf(p->lit.v[0]), ch = true;
        break;
    }
    case BPT_BYTE:
        if (ui_num_float("##l", &p->lit.v[0], 0.2f, 0, 255, 0, 255, "%.0f", 0)) p->lit.v[0] = roundf(p->lit.v[0]), ch = true; // (a byte: 0..255, the type's own limit)
        break;
    case BPT_INT64: {
        double d;
        memcpy(&d, p->lit.v, 8);
        long long x = (long long)d;
        if (igDragScalar("##l", ImGuiDataType_S64, &x, 0.2f, NULL, NULL, "%lld", 0)) d = (double)x, memcpy(p->lit.v, &d, 8), ch = true;
        break;
    }
    case BPT_FLOAT: {
        float lo = 0.0f, hi = 0.0f; // (2026-11-24) a pin with a range (the load speeds): clamped, the range in its tooltip
        if (bp_pin_range(g_lit_kind, p->name, &lo, &hi)) {
            ch = ui_num_float("##l", &p->lit.v[0], 0.005f, lo, hi, lo, hi, "%.3g", 0); // (a real limit: held typed too)
            if (igIsItemHovered(0)) igSetTooltip(TR("Min %g, max %g"), lo, hi);
        } else
            ch = igDragFloat("##l", &p->lit.v[0], 0.05f, 0, 0, "%.3g", 0);
        break;
    }
    case BPT_VECTOR:
    case BPT_ROTATOR: ch = igDragFloat3("##l", p->lit.v, 0.5f, 0, 0, "%.3g", 0); break;
    case BPT_COLOR: ch = igColorEdit4("##l", p->lit.v, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
    case BPT_STRING:
    case BPT_NAME:
    case BPT_TEXT: ch = igInputText("##l", p->slit, sizeof p->slit, 0, NULL, NULL); break;
    case BPT_CLASS:
        if (igBeginCombo("##l", p->slit[0] ? p->slit : TR("Select Class"), 0)) {
            if (igSelectable_Bool(TR("None"), !p->slit[0], 0, (ImVec2){ 0, 0 })) p->slit[0] = 0, ch = true;
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                const BpAsset* c = bp_asset_at(i);
                if (c->kind == BPA_CLASS && igSelectable_Bool(c->name, !strcmp(p->slit, c->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(p->slit, sizeof p->slit, "%s", c->name), ch = true;
            }
            igEndCombo();
        }
        break;
    case BPT_ENUM: {
        const BpAsset* e = bp_find_name(BPA_ENUM, p->ref);
        uint32_t cur = (uint32_t)p->lit.v[0];
        if (igBeginCombo("##l", e && cur < e->nentries ? e->entries[cur] : "?", 0)) {
            for (uint32_t k = 0; e && k < e->nentries; k++)
                if (igSelectable_Bool(e->entries[k], k == cur, 0, (ImVec2){ 0, 0 })) p->lit.v[0] = (float)k, ch = true;
            igEndCombo();
        }
        break;
    }
    case BPT_ACTOR: igTextDisabled("self"); break;
    default: break;
    }
    if (ch) touch(w);
    return ch;
}

static void frame_all(Win* w, BpGraph* g, ImVec2 size) {
    if (!g->nnodes) {
        g->pan[0] = size.x * 0.4f, g->pan[1] = size.y * 0.4f, g->zoom = 1.0f;
        return;
    }
    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    for (uint32_t i = 0; i < g->nnodes; i++) {
        mn[0] = fminf(mn[0], g->nodes[i].x), mn[1] = fminf(mn[1], g->nodes[i].y);
        mx[0] = fmaxf(mx[0], g->nodes[i].x + 260.0f), mx[1] = fmaxf(mx[1], g->nodes[i].y + 140.0f);
    }
    float z = fminf(size.x / (mx[0] - mn[0] + 80.0f), size.y / (mx[1] - mn[1] + 80.0f));
    g->zoom = fminf(fmaxf(z, 0.5f), 1.0f); // readable: a big graph is framed at half size, centered
    g->pan[0] = size.x * 0.5f / g->zoom - (mn[0] + mx[0]) * 0.5f;
    g->pan[1] = size.y * 0.5f / g->zoom - (mn[1] + mx[1]) * 0.5f;
    (void)w;
}

// (2026-11-25, the tester's photos) --show bp-pin:<vpath>[:<pin>] / bp-var:<vpath>:<var> (bp_editor_show)
static struct {
    BpAsset* a;
    char pin[BP_NAME], var[BP_NAME];
    bool pin_pending, var_pending, place;
    int frames;
    ImVec2 at; // (the menu's place: the pin's)
} g_show;

static void draw_graph(Win* w, float height) {
    BpAsset* a = w->a;
    if (w->graph >= a->ngraphs) w->graph = 0;
    BpGraph* g = &a->graphs[w->graph];
    ImGuiIO* io = igGetIO_Nil();
    ImVec2 size = { igGetContentRegionAvail().x, height };
    if (size.x < 50 || size.y < 50) return;
    igBeginChild_Str("canvas", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImVec2 o = igGetCursorScreenPos();
    if (w->frame) {
        frame_all(w, g, size);
        w->frame = false;
    }
    if (g->zoom < 0.3f || g->zoom > 2.0f) g->zoom = 1.0f;
    // the wheel's zoom first: the whole frame (grid, font, nodes, wires) at the new zoom and pan, never half at the
    // old one (the nodes and wires flickered while zooming)
    if (igIsWindowHovered(0) && io->MouseWheel != 0.0f) {
        ImVec2 mw = io->MousePos;
        float oz = g->zoom, gx = (mw.x - o.x) / oz - g->pan[0], gy = (mw.y - o.y) / oz - g->pan[1];
        float nz = fminf(fmaxf(oz * powf(1.12f, io->MouseWheel), 0.3f), 2.0f);
        g->pan[0] = (mw.x - o.x) / nz - gx;
        g->pan[1] = (mw.y - o.y) / nz - gy;
        g->zoom = nz;
    }
    float z = g->zoom;
    ImDrawList* dl = igGetWindowDrawList();
    // the background (Q5, graph_style.c): Orbit's dots (the bigger grid fading in when zoomed in); the older styles' lines
    graph_background(dl, o, size, g->pan[0] * z, g->pan[1] * z, z, graph_orbit() ? 0 : TH(TK_N6));
    // the watermark (Unreal's big BLUEPRINT in the corner)
    {
        igPushFont(NULL, 42.0f);
        const char* wm = g->gkind == BPG_MACRO ? "MACRO" : a->kind == BPA_INTERFACE ? "INTERFACE" : a->kind == BPA_CLASS && w->graph > 0 ? "FUNCTION" : "BLUEPRINT";
        ImVec2 ts = igCalcTextSize(wm, NULL, false, -1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + size.x - ts.x - 22, o.y + size.y - ts.y - 30 }, THA(TK_N15, 22), wm, NULL);
        igPopFont();
    }
    const ImGuiStyle* st = igGetStyle();
    g_font_base = st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f; // (before the zoom's push: popup_begin)
    igPushFont(NULL, roundf(g_font_base * z)); // (2026-11-23: whole pixels: each new size baked glyphs and waited on the GPU)
    // the background takes what the nodes don't
    igSetCursorScreenPos(o);
    igSetNextItemAllowOverlap();
    igInvisibleButton("bg", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool bg_hover = igIsItemHovered(0);
    ImVec2 m = io->MousePos;
    float gm[2] = { (m.x - o.x) / z - g->pan[0], (m.y - o.y) / z - g->pan[1] };
    if (igIsItemActivated() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        uint16_t quick = igIsKeyDown_Nil(ImGuiKey_B) ? BPK_BRANCH : igIsKeyDown_Nil(ImGuiKey_S) ? BPK_SEQUENCE
                       : igIsKeyDown_Nil(ImGuiKey_D) ? BPK_DELAY : BPN_KIND_COUNT;
        if (quick != BPN_KIND_COUNT) { // Unreal's B / S / D + click
            BpNode* n = bp_add_node(a, g, quick, gm[0], gm[1], NULL, NULL);
            w->nsel = 0;
            if (n) sel_add(w, n->id);
            touch(w);
        } else {
            w->mode = MODE_BOX;
            w->press = m;
            if (!io->KeyCtrl && !io->KeyShift) w->nsel = 0;
            w->details = DET_CLASS;
        }
    }
    if (bg_hover && igIsMouseReleased_Nil(ImGuiMouseButton_Right) && fabsf(igGetMouseDragDelta(ImGuiMouseButton_Right, 0).x) < 3.0f &&
        fabsf(igGetMouseDragDelta(ImGuiMouseButton_Right, 0).y) < 3.0f)
        open_actions(w, gm[0], gm[1], 0, -1);
    { // (the test bp_graph_menu: the menu opened once, as a right click would; with 2, the first recommendation placed)
        static int s_test_menu = -1;
        if (s_test_menu < 0) s_test_menu = getenv("REALENGINE_BP_MENU") ? atoi(getenv("REALENGINE_BP_MENU")) : 0;
        if (s_test_menu > 0) {
            open_actions(w, gm[0], gm[1], 0, -1);
            char ctx[96];
            menu_context(w, ctx, sizeof ctx);
            RecUser u = { w, { 0 }, 0 };
            const char* items[2];
            int n = gm_recommend(graph_kind(w), ctx, rec_accept, &u, items, 2);
            printf("[RealEngine] Graph menu: %u actions for graph %u (kind %d), context \"%s\", recommended: %s, %s\n", g_nmenu, w->graph,
                   (int)graph_kind(w), ctx, n > 0 ? items[0] : "-", n > 1 ? items[1] : "-");
            if (s_test_menu == 2 && n > 0) {
                MenuEntry* e = entry_named(items[0]);
                if (e) place(w, e), printf("[RealEngine] Graph menu: placed %s\n", e->label);
            }
            s_test_menu = 0;
        }
    }
    if ((igIsItemActive() || w->mode == MODE_PAN) && (igIsMouseDragging(ImGuiMouseButton_Right, 3.0f) || igIsMouseDragging(ImGuiMouseButton_Middle, 3.0f))) {
        g->pan[0] += io->MouseDelta.x / z, g->pan[1] += io->MouseDelta.y / z;
        w->mode = MODE_PAN;
    }
    if (w->mode == MODE_PAN && !igIsMouseDown_Nil(ImGuiMouseButton_Right) && !igIsMouseDown_Nil(ImGuiMouseButton_Middle)) w->mode = MODE_NONE;
    // a variable dropped from My Blueprint: Ctrl Get, Alt Set, else ask
    static int drop_var = -1;
    static float drop_g[2];
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* mp = igAcceptDragDropPayload("RE_BP_MACRO", 0);
        if (mp) {
            BpNode* n = bp_add_node(a, g, BPK_MACRO, gm[0], gm[1], (const char*)mp->Data, NULL);
            w->nsel = 0;
            if (n) sel_add(w, n->id);
            touch(w);
        }
        // a function (a call), a component (Get <it>) dropped from My Blueprint / Components
        static const struct { const char* type; uint16_t kind; } drops[2] = { { "RE_BP_FUNC", BPK_CALL_FUNCTION }, { "RE_BP_COMP", BPK_GET_COMPONENT } };
        for (int d = 0; d < 2; d++) {
            const ImGuiPayload* dp = igAcceptDragDropPayload(drops[d].type, 0);
            if (!dp) continue;
            BpNode* n = bp_add_node(a, g, drops[d].kind, gm[0], gm[1], (const char*)dp->Data, NULL);
            w->nsel = 0;
            if (n) sel_add(w, n->id);
            touch(w);
        }
        const ImGuiPayload* pl = igAcceptDragDropPayload("RE_BP_VAR", 0);
        if (pl) {
            int v = *(const int*)pl->Data;
            if (v >= 0 && (uint32_t)v < a->nvars) {
                if (io->KeyCtrl || io->KeyAlt) {
                    BpNode* n = bp_add_node(a, g, io->KeyAlt ? BPK_VAR_SET : BPK_VAR_GET, gm[0], gm[1], a->vars[v].name, NULL);
                    w->nsel = 0;
                    if (n) sel_add(w, n->id);
                    touch(w);
                } else {
                    drop_var = v, drop_g[0] = gm[0], drop_g[1] = gm[1];
                    igOpenPopup_Str("var_drop", 0);
                }
            }
        }
        igEndDragDropTarget();
    }
    if (popup_begin("var_drop")) {
        if (drop_var >= 0 && (uint32_t)drop_var < a->nvars) {
            char l[96];
            snprintf(l, sizeof l, "Get %s", a->vars[drop_var].name);
            if (ui_menu_item(ICON_VARIABLE, l, "Ctrl", false, true)) bp_add_node(a, g, BPK_VAR_GET, drop_g[0], drop_g[1], a->vars[drop_var].name, NULL), touch(w);
            snprintf(l, sizeof l, "Set %s", a->vars[drop_var].name);
            if (ui_menu_item(ICON_VARIABLE, l, "Alt", false, true)) bp_add_node(a, g, BPK_VAR_SET, drop_g[0], drop_g[1], a->vars[drop_var].name, NULL), touch(w);
        }
        popup_end();
    }

    // geometry first (the wires need every pin)
    if (g->nnodes > g_geo_cap) {
        uint32_t cap = g->nnodes * 2;
        Geo* ng = realloc(g_geo, sizeof(Geo) * cap); // the editor's UI, not a frame of the game
        if (!ng) {
            igPopFont();
            igEndChild();
            return;
        }
        g_geo = ng;
        g_geo_cap = cap;
    }
    if (w->center_node) { // (the debugger: the paused node, a frame of the call stack, in the middle)
        const BpNode* cn = bp_node(g, w->center_node);
        if (cn) g->pan[0] = size.x * 0.5f / z - cn->x - 100.0f, g->pan[1] = size.y * 0.5f / z - cn->y - 40.0f;
        w->center_node = 0;
    }
    // a node drag (begun last frame below): the selection where the mouse is now, before the geometry, so the nodes
    // and their wires are drawn under the cursor this very frame (moved while drawing, they showed a frame behind; and
    // by the mouse's deltas after the drag threshold, they trailed it by those first pixels)
    { // (2026-11-23) Blender's edge pan: a node or a wire dragged near the edge scrolls the view (the node stays under the mouse)
        float es[2];
        if ((w->mode == MODE_MOVE || w->mode == MODE_WIRE) && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(o, size, es)) {
            g->pan[0] -= es[0] / z, g->pan[1] -= es[1] / z;
            w->move_press.x -= es[0], w->move_press.y -= es[1];
        }
    }
    if (w->mode == MODE_MOVE && igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
        float dx = (io->MousePos.x - w->move_press.x) / z, dy = (io->MousePos.y - w->move_press.y) / z;
        for (uint32_t s = 0; s < w->nsel; s++) {
            BpNode* sn = bp_node(g, w->sel[s]);
            if (sn) sn->x = w->move_start[s][0] + dx, sn->y = w->move_start[s][1] + dy;
        }
    }
    for (uint32_t i = 0; i < g->nnodes; i++) node_geo(g, &g->nodes[i], o, g->pan, z, &g_geo[i]);
    ImDrawList_ChannelsSplit(dl, 2);
    ImDrawList_ChannelsSetCurrent(dl, 0);
    int near_link = -1;
    float near_d = 1e30f;
    for (uint32_t l = 0; l < g->nlinks; l++) {
        const BpLink* k = &g->links[l];
        int fi = -1, ti = -1;
        for (uint32_t i = 0; i < g->nnodes && (fi < 0 || ti < 0); i++) {
            if (g->nodes[i].id == k->from_node) fi = (int)i;
            if (g->nodes[i].id == k->to_node) ti = (int)i;
        }
        if (fi < 0 || ti < 0 || k->from_pin >= g->nodes[fi].npins || k->to_pin >= g->nodes[ti].npins) continue;
        const BpPin* p = &g->nodes[fi].pins[k->from_pin];
        {
            const BpPin* tp = &g->nodes[ti].pins[k->to_pin];
            ImU32 ca = p->delegate ? TH(TK_PIN_DELEGATE) : type_color(p->type), cb = tp->delegate ? TH(TK_PIN_DELEGATE) : type_color(tp->type);
            wire2(dl, g_geo[fi].pin[k->from_pin], g_geo[ti].pin[k->to_pin], ca, cb, z, p->type == BPT_EXEC);
        }
        if (p->type == BPT_EXEC && g_dbg_trace) dbg_wire(w, dl, &g->nodes[ti], k->to_pin, g_geo[fi].pin[k->from_pin], g_geo[ti].pin[k->to_pin], z); // (the debugger: it ran lately)
        if (bg_hover && (w->mode == MODE_NONE || w->mode == MODE_BOX)) { // the wire under the mouse (a double click puts a Reroute on it)
            float d = wire_dist(g_geo[fi].pin[k->from_pin], g_geo[ti].pin[k->to_pin], m, z);
            if (d < 6.0f * fmaxf(z, 0.6f) && d < near_d) near_d = d, near_link = (int)l;
        }
    }
    if (near_link >= 0) {
        const BpLink* k = &g->links[near_link];
        BpNode* fn = bp_node(g, k->from_node);
        int fi = fn ? (int)(fn - g->nodes) : -1;
        BpNode* tn = bp_node(g, k->to_node);
        int ti = tn ? (int)(tn - g->nodes) : -1;
        if (fi >= 0 && ti >= 0) { // highlighted, Unreal's way
            ImU32 c = type_color(fn->pins[k->from_pin].type);
            wire(dl, g_geo[fi].pin[k->from_pin], g_geo[ti].pin[k->to_pin], scale_rgb(c, 1.6f, 255), z * 1.4f, fn->pins[k->from_pin].type == BPT_EXEC);
        }
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            BpLink old = *k;
            BpNode* rr = bp_add_node(a, g, BPK_REROUTE, gm[0], gm[1], NULL, NULL);
            if (rr) {
                uint32_t rid = rr->id;
                g->links[near_link] = g->links[--g->nlinks]; // (the wire through the knot instead)
                bp_link(g, old.from_node, old.from_pin, rid, 0);
                bp_link(g, rid, 1, old.to_node, old.to_pin);
                w->nsel = 0, sel_add(w, rid);
                w->mode = MODE_NONE;
                touch(w);
            }
        }
    }
    ImDrawList_ChannelsSetCurrent(dl, 1);
    uint32_t hover_node = 0, hover_body = 0; // (body: a node under the mouse, a wire let go there finds its pin)
    int hover_pin = -1;
    uint32_t deferred_open_node = 0;
    int deferred_open_pin = -1;
    bool node_menu = false;
    static uint32_t pin_menu_node;
    static int pin_menu_pin = -1;
    bool pin_menu = false;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        BpNode* n = &g->nodes[i];
        Geo* ge = &g_geo[i];
        ImVec2 p0 = ge->pos, p1 = { ge->pos.x + ge->size.x, ge->pos.y + ge->size.y };
        if (p1.x < o.x || p1.y < o.y || p0.x > o.x + size.x || p0.y > o.y + size.y) continue; // off the view
        bool sel = is_sel(w, n->id), err = a->error[0] && a->error_node == n->id;
        bool cpt = compact(n), hov = igIsMouseHoveringRect(p0, p1, true) && igIsWindowHovered(0);
        if (igIsMouseHoveringRect(p0, p1, true) && igIsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) hover_body = n->id;
        float r = (cpt ? ge->size.y * 0.5f : 7.0f * z);
        // a soft shadow: three widening layers
        for (int s = 3; s >= 1; s--) {
            float e = (float)s * 1.6f * z;
            ImDrawList_AddRectFilled(dl, (ImVec2){ p0.x - e + 2 * z, p0.y - e + 4 * z }, (ImVec2){ p1.x + e + 2 * z, p1.y + e + 4 * z },
                                     THA(TK_N0, 34), r + e, 0);
        }
        char title[128];
        node_title(n, title, sizeof title);
        float fs = igGetFontSize();
        if (n->kind == BPK_REROUTE) { // a knot: a small dark pill tinted with what it carries
            ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N4, 230), ge->size.y * 0.5f, 0);
            if (n->npins) ImDrawList_AddRectFilled(dl, p0, p1, scale_rgb(type_color(n->pins[0].type), 0.5f, 50), ge->size.y * 0.5f, 0);
        } else         if (cpt) { // a getter: the pill tinted with its type's color
            ImU32 tc = n->npins ? type_color(n->pins[0].type) : TH(TK_N14);
            ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N4, 240), r, 0);
            ImDrawList_AddRectFilled(dl, p0, p1, scale_rgb(tc, 0.55f, 60), r, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ p0.x + 12 * z, p0.y + (ge->size.y - fs) * 0.5f }, TH(TK_N15), title, NULL);
        } else {
            if (graph_node_style() != GS_NODE_DEFAULT) { // (Editor Preferences > Graph Style: the nodes' style; its border too)
                ImU32 bc = err ? TH(TK_RED) : sel ? TH(TK_AMBER) : hov ? TH(TK_N11) : TH(TK_N8);
                if (graph_orbit()) bc = err ? TH(TK_ERROR) : sel ? TH(TK_ACCENT) : hov ? TH(TK_TEXT_DIM) : TH(TK_BORDER_STRONG); // (Q5: the halo)
                graph_node_frame(dl, p0, p1, ge->header, r, header_color(n), bc, (sel || err ? 2.4f : 1.0f) * z, z, graph_node_style());
            } else {
                ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N3, 238), r, 0);
                header_fill(dl, p0, (ImVec2){ p1.x, p0.y + ge->header }, r, header_color(n));
                ImDrawList_AddLine(dl, (ImVec2){ p0.x + 1, p0.y + ge->header }, (ImVec2){ p1.x - 1, p0.y + ge->header }, THA(TK_N0, 140), 1.0f);
            }
            float tx = p0.x + 10 * z;
            UiIcon hi = header_icon(n);
            if (hi != ICON_COUNT) {
                float is = fs * 0.95f;
                ImU32 ic = TH(TK_NODE_TITLE);
                ui_icon(dl, hi, tx - 2 * z, p0.y + (ge->header - is) * 0.5f, is, ic);
                tx += is + 3 * z;
            }
            ImDrawList_AddText_Vec2(dl, (ImVec2){ tx + 1, p0.y + (ge->header - fs) * 0.5f + 1 }, THA(TK_N0, 160), title, NULL);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ tx, p0.y + (ge->header - fs) * 0.5f }, TH(TK_NODE_TITLE), title, NULL);
            if (err && graph_orbit()) graph_error_badge(dl, (ImVec2){ p1.x, p0.y }, z); // (Q5: the error's badge)
        }
        ImU32 border = err ? TH(TK_RED) : sel ? TH(TK_AMBER) : hov ? TH(TK_N11) : TH(TK_N8);
        if (graph_node_style() == GS_NODE_DEFAULT || cpt || n->kind == BPK_REROUTE) ImDrawList_AddRect(dl, p0, p1, border, r, (sel || err ? 2.4f : 1.0f) * z, 0);
        dbg_node_marks(w, dl, n, p0, p1, r, z); // (the debugger: the breakpoint's dot, the paused node)
        if (n->comment[0]) // the comment bubble above it
            ImDrawList_AddText_Vec2(dl, (ImVec2){ p0.x, p0.y - fs - 4 * z }, THA(TK_N14, 200), n->comment, NULL);
        igPushID_Int((int)n->id);
        igSetCursorScreenPos(p0);
        igSetNextItemAllowOverlap();
        igInvisibleButton("node", ge->size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
        if (igIsItemActivated() && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            if (io->KeyCtrl) sel ? sel_remove(w, n->id) : sel_add(w, n->id);
            else if (io->KeyShift) sel_add(w, n->id);
            else if (!sel) w->nsel = 0, sel_add(w, n->id);
            w->details = DET_NODE;
            if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) && n->kind == BPK_CALL_FUNCTION) // into the function
                for (uint32_t k = 1; k < a->ngraphs; k++)
                    if (!strcmp(a->graphs[k].name, n->text)) w->graph = k, w->frame = true;
            if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) && n->kind == BPK_TIMELINE) { // (2026-11-16) its editor
                if (!bp_timeline_find(a, n->text)) { // (a node without one: made)
                    BpTimeline* tl = bp_timeline_add(a);
                    if (tl) snprintf(n->text, sizeof n->text, "%s", tl->name), bp_refresh_pins(a, g, n);
                }
                w->tl_open = true, w->tl_graph = w->graph, w->tl_node = n->id;
            }
        }
        if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, 2.0f) && w->mode != MODE_WIRE && w->mode != MODE_MOVE) {
            w->mode = MODE_MOVE; // (the drag begins: where each was and where the mouse was pressed; moved at the top)
            w->move_press = io->MouseClickedPos[0];
            for (uint32_t s = 0; s < w->nsel; s++) {
                BpNode* sn = bp_node(g, w->sel[s]);
                w->move_start[s][0] = sn ? sn->x : 0.0f, w->move_start[s][1] = sn ? sn->y : 0.0f;
            }
            float dx = (io->MousePos.x - w->move_press.x) / z, dy = (io->MousePos.y - w->move_press.y) / z;
            for (uint32_t s = 0; s < w->nsel; s++) {
                BpNode* sn = bp_node(g, w->sel[s]);
                if (sn) sn->x = w->move_start[s][0] + dx, sn->y = w->move_start[s][1] + dy;
            }
            a->dirty = true;
        }
        if (igIsItemClicked(ImGuiMouseButton_Right)) {
            if (!sel) w->nsel = 0, sel_add(w, n->id);
            node_menu = true;
        }
        if (igIsItemHovered(0) && bp_node_desc(n->kind)->tooltip) igSetItemTooltip("%s", bp_node_desc(n->kind)->tooltip);
        for (uint8_t p = 0; p < n->npins; p++) {
            BpPin* q = &n->pins[p];
            bool linked = bp_pin_linked(g, n->id, p);
            draw_pin(dl, ge->pin[p], q, linked, z);
            if (q->name[0] && !cpt && !(q->delegate && q->out && ge->header > 0))
                ImDrawList_AddText_Vec2(dl, (ImVec2){ ge->label_x[p], ge->pin[p].y - fs * 0.5f }, TH(TK_N14), TR(q->name), NULL);
            igPushID_Int(p);
            float pr = 8.0f * z;
            igSetCursorScreenPos((ImVec2){ ge->pin[p].x - pr, ge->pin[p].y - pr });
            igInvisibleButton("pin", (ImVec2){ pr * 2, pr * 2 }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
            if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                hover_node = n->id, hover_pin = p;
                ImDrawList_AddCircle(dl, ge->pin[p], pr, THA(TK_N15, 120), 12, 1.0f);
                char tl[BP_NAME * 2 + 32], val[256];
                type_label(q->type, q->ref, q->cont, q->vtype, tl, sizeof tl);
                if (g_dbg_trace && q->type != BPT_EXEC && bp_debug_pin_text(a, w->graph, n->id, p, ENTITY_NONE, val, sizeof val)) // (the debugger: its value now)
                    igSetItemTooltip("%s (%s)\n= %s", TR(q->name[0] ? q->name : bp_node_desc(n->kind)->name), tl, val);
                else
                    igSetItemTooltip("%s (%s)%s", TR(q->name[0] ? q->name : bp_node_desc(n->kind)->name), tl,
                                     bp_type_splittable(q->type) && !q->cont ? TR("  -  right click: Split") : "");
            }
            if (igIsItemClicked(ImGuiMouseButton_Right)) pin_menu = true, pin_menu_node = n->id, pin_menu_pin = p;
            if (igIsItemActivated()) {
                if (io->KeyAlt) {
                    bp_unlink_pin(g, n->id, p);
                    touch(w);
                } else {
                    w->mode = MODE_WIRE;
                    w->wire_node = n->id;
                    w->wire_pin = p;
                }
            }
            if (ge->lit_w[p] > 0.0f) { // the value typed in an unconnected input
                float lw = q->name[0] ? igCalcTextSize(TR(q->name), NULL, false, -1).x + 6.0f * z : 0.0f;
                igSetCursorScreenPos((ImVec2){ ge->label_x[p] + lw, ge->pin[p].y - igGetFrameHeight() * 0.5f });
                g_lit_kind = n->kind;
                if (q->asset) snprintf(q->slit, sizeof q->slit, "%s", n->text); // (the asset pin: the node's text)
                bool lch = lit_widget(w, q, ge->lit_w[p]);
                if (lch && q->asset) snprintf(n->text, sizeof n->text, "%s", q->slit), touch(w);
                if (lch && q->type == BPT_CLASS && (n->kind == BPK_SPAWN || n->kind == BPK_SPAWN_ASYNC)) {
                    bp_refresh_pins(a, g, n); // (the class's Expose on Spawn variables become its pins)
                    touch(w);
                    igPopID();
                    break;
                }
            }
            igPopID();
        }
        igPopID();
    }
    // the wire being dragged; let go on a pin: linked, on nothing: the nodes that fit it
    if (w->mode == MODE_WIRE) {
        BpNode* f = bp_node(g, w->wire_node);
        if (!f || w->wire_pin >= f->npins) w->mode = MODE_NONE;
        else {
            int fi = (int)(f - g->nodes);
            const BpPin* fp = &f->pins[w->wire_pin];
            ImVec2 a0 = g_geo[fi].pin[w->wire_pin];
            ImU32 wc = fp->delegate ? TH(TK_PIN_DELEGATE) : type_color(fp->type);
            if (fp->out) wire(dl, a0, m, wc, z, fp->type == BPT_EXEC);
            else wire(dl, m, a0, wc, z, fp->type == BPT_EXEC);
            // B / S / D while dragging (Unreal's): Branch, Sequence or Delay at the mouse, the wire into it
            uint16_t quick = BPN_KIND_COUNT;
            if (!io->WantTextInput && !io->KeyCtrl && igIsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                quick = igIsKeyPressed_Bool(ImGuiKey_B, false)   ? BPK_BRANCH
                        : igIsKeyPressed_Bool(ImGuiKey_S, false) ? BPK_SEQUENCE
                        : igIsKeyPressed_Bool(ImGuiKey_D, false) ? BPK_DELAY
                                                                 : BPN_KIND_COUNT;
            if (quick != BPN_KIND_COUNT) {
                BpNode* qn = bp_add_node(a, g, quick, gm[0], gm[1] - 20.0f, NULL, NULL);
                if (qn) {
                    uint32_t qid = qn->id;
                    link_into(g, w->wire_node, w->wire_pin, qid);
                    w->nsel = 0, sel_add(w, qid);
                    touch(w);
                }
                w->mode = MODE_NONE;
            } else if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
                if (hover_pin >= 0 && hover_node != w->wire_node) {
                    BpNode* t = bp_node(g, hover_node);
                    if (t && !bp_link(g, w->wire_node, w->wire_pin, hover_node, (uint8_t)hover_pin)) {
                        char buf[160];
                        snprintf(buf, sizeof buf, "%s -> %s", g_bp_type_names[fp->type], g_bp_type_names[t->pins[hover_pin].type]);
                        set_status(w, true, TR("These pins don't connect: %s."), buf);
                    } else touch(w);
                } else if (hover_pin < 0 && hover_body && hover_body != w->wire_node) { // on a node's body: its pin that fits
                    if (link_into(g, w->wire_node, w->wire_pin, hover_body)) touch(w);
                    else set_status(w, true, TR("No pin of that node takes this wire."), "");
                } else if (hover_pin < 0) {
                    deferred_open_node = w->wire_node;
                    deferred_open_pin = w->wire_pin;
                }
                w->mode = MODE_NONE;
            }
        }
    }
    ImDrawList_ChannelsMerge(dl);
    if (deferred_open_pin >= 0) open_actions(w, gm[0], gm[1], deferred_open_node, deferred_open_pin);
    // the box selection
    if (w->mode == MODE_BOX) {
        ImVec2 b0 = { fminf(w->press.x, m.x), fminf(w->press.y, m.y) }, b1 = { fmaxf(w->press.x, m.x), fmaxf(w->press.y, m.y) };
        ImDrawList_AddRectFilled(dl, b0, b1, THA(TK_AMBER, 30), 0, 0);
        ImDrawList_AddRect(dl, b0, b1, THA(TK_AMBER, 200), 0, 1.0f, 0);
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            for (uint32_t i = 0; i < g->nnodes; i++) {
                const Geo* ge = &g_geo[i];
                if (ge->pos.x < b1.x && ge->pos.x + ge->size.x > b0.x && ge->pos.y < b1.y && ge->pos.y + ge->size.y > b0.y) sel_add(w, g->nodes[i].id);
            }
            if (w->nsel) w->details = DET_NODE;
            w->mode = MODE_NONE;
        }
    }
    if (w->mode == MODE_MOVE && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) w->mode = MODE_NONE;
    if (g_show.pin_pending && g_show.a == a && w->graph == 0 && ++g_show.frames == 400) // (none in 400 frames: said once)
        printf("[RealEngine] --show bp-pin: %s's Event Graph has no splittable pin%s%s\n", a->name, g_show.pin[0] ? " named " : "", g_show.pin);
    if (g_show.pin_pending && g_show.a == a && w->graph == 0 && g_show.frames > 10) // (--show bp-pin: the layout settled first)
        for (uint32_t i = 0; i < g->nnodes && g_show.pin_pending; i++)
            for (int p = 0; p < g->nodes[i].npins; p++) {
                const BpPin* q = &g->nodes[i].pins[p];
                if (q->sub2 || q->cont || !bp_type_splittable(q->type) || (g_show.pin[0] && _stricmp(q->name, g_show.pin))) continue;
                pin_menu = true, pin_menu_node = g->nodes[i].id, pin_menu_pin = p, g_show.pin_pending = false;
                w->want_tab = -1, w->want_hold = 0; // (no tab brought forward again: its focus would close the menu)
                g_show.at = g_geo[i].pin[p], g_show.place = true; // (at the pin, not where the real mouse is: another monitor)
                printf("[RealEngine] --show bp-pin: the menu of %s's pin %s\n", bp_node_desc(g->nodes[i].kind)->name, q->name);
                break;
            }
    if (pin_menu) igOpenPopup_Str("pin_menu", 0);
    if (g_show.place && pin_menu) igSetNextWindowPos(g_show.at, ImGuiCond_Always, (ImVec2){ 0, 0 }), g_show.place = false;
    if (popup_begin("pin_menu")) { // Unreal's pin actions
        BpNode* n = bp_node(g, pin_menu_node);
        if (n && pin_menu_pin >= 0 && pin_menu_pin < n->npins) {
            BpPin* q = &n->pins[pin_menu_pin];
            igTextDisabled("%s", TR(q->name[0] ? q->name : bp_node_desc(n->kind)->name));
            igSeparator();
            // (2026-11-24) a part split again too (Transform -> Location -> X); each one a step of Ctrl+Z
            if (!q->sub2 && !q->cont && bp_type_splittable(q->type) && ui_menu_item(ICON_STRUCT, TR("Split Struct Pin"), NULL, false, true)) {
                bp_graph_undo_push(a, w->graph);
                bp_split_pin(a, g, n, (uint8_t)pin_menu_pin, true);
                touch(w);
            }
            if (q->sub && ui_menu_item(ICON_STRUCT, TR("Recombine Struct Pin"), NULL, false, true)) {
                bp_graph_undo_push(a, w->graph);
                bp_split_pin(a, g, n, (uint8_t)pin_menu_pin, false);
                touch(w);
            }
            dbg_pin_menu(w, n, pin_menu_pin); // (the debugger's Watch)
            if (bp_pin_linked(g, n->id, (uint8_t)pin_menu_pin) && ui_menu_item(ICON_CUT, TR("Break Link(s)"), "Alt+Click", false, true)) {
                bp_unlink_pin(g, n->id, (uint8_t)pin_menu_pin);
                touch(w);
            }
            if (q->type != BPT_EXEC && q->type != BPT_WILDCARD && a->kind == BPA_CLASS && a->nvars < 64 &&
                ui_menu_item(ICON_VARIABLE, TR("Promote to Variable"), NULL, false, true)) {
                BpVar* v = &a->vars[a->nvars];
                memset(v, 0, sizeof *v);
                snprintf(v->name, sizeof v->name, "%s", q->name[0] ? q->name : "NewVar");
                for (char* c = v->name; *c; c++)
                    if (*c == ' ') *c = '_';
                for (int k = 2; k < 100 && bp_var_index(a, v->name) >= 0; k++) snprintf(v->name, sizeof v->name, "%s%d", q->name[0] ? q->name : "NewVar", k);
                v->type = q->type, v->cont = q->cont, v->vtype = q->vtype;
                snprintf(v->ref, sizeof v->ref, "%s", q->ref);
                if (!q->out) v->def = q->lit, snprintf(v->sdef, sizeof v->sdef, "%s", q->slit);
                if (v->type == BPT_TRANSFORM) v->def3[1].v[0] = v->def3[1].v[1] = v->def3[1].v[2] = 1.0f;
                a->nvars++;
                uint32_t nid = n->id;
                float gx = n->x + (q->out ? 260.0f : -200.0f), gy = n->y;
                BpNode* vn = bp_add_node(a, g, q->out ? BPK_VAR_SET : BPK_VAR_GET, gx, gy, v->name, NULL);
                n = bp_node(g, nid);
                if (vn && n) {
                    if (q->out) bp_link(g, nid, (uint8_t)pin_menu_pin, vn->id, 1);
                    else bp_link(g, vn->id, 0, nid, (uint8_t)pin_menu_pin);
                }
                touch(w);
            }
        }
        popup_end();
    }
    if (node_menu) igOpenPopup_Str("node_menu", 0);
    if (popup_begin("node_menu")) {
        BpNode* sn = w->nsel ? bp_node(g, w->sel[w->nsel - 1]) : NULL;
        if (a->kind == BPA_CLASS) dbg_node_menu(w, sn); // (Add / Remove / Disable Breakpoint)
        sn = w->nsel ? bp_node(g, w->sel[w->nsel - 1]) : NULL;
        bool grows = sn && (sn->kind == BPK_SEQUENCE || sn->kind == BPK_ARR_MAKE || sn->kind == BPK_APPEND || sn->kind == BPK_SWITCH_INT);
        if (grows && sn->npins < BP_MAX_PINS && ui_menu_item(ICON_PLUS, TR("Add pin"), NULL, false, true)) {
            sn->extra++;
            bp_refresh_pins(a, g, sn);
            touch(w);
        }
        if (grows && sn->extra && ui_menu_item(ICON_CROSS, TR("Remove pin"), NULL, false, true)) {
            sn->extra--;
            bp_refresh_pins(a, g, sn);
            touch(w);
        }
        if (sn && sn->wtype && ui_menu_item(ICON_REFRESH, TR("Reset wildcard type"), NULL, false, true)) {
            for (uint8_t p = 0; p < sn->npins; p++)
                if (sn->pins[p].wslot) bp_unlink_pin(g, sn->id, p);
            sn->wtype = sn->wtype2 = 0, sn->wref[0] = 0;
            bp_refresh_pins(a, g, sn);
            touch(w);
        }
        if (sn && sn->kind == BPK_MACRO && ui_menu_item(ICON_MACRO, TR("Open Macro"), NULL, false, true))
            for (uint32_t k = 1; k < a->ngraphs; k++)
                if (a->graphs[k].gkind == BPG_MACRO && !strcmp(a->graphs[k].name, sn->text)) w->graph = k, w->want_tab = 1 + (int)k, w->frame = true;
        if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, true)) delete_selected(w);
        if (ui_menu_item(ICON_DUPLICATE, TR("Duplicate"), "Ctrl+D", false, true)) duplicate_selected(w);
        if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, true)) organize_nodes(w);
        if (ui_menu_item(ICON_CUT, TR("Break All Pin Links"), NULL, false, true)) {
            for (uint32_t s = 0; s < w->nsel; s++) {
                BpNode* n = bp_node(g, w->sel[s]);
                for (uint8_t p = 0; n && p < n->npins; p++) bp_unlink_pin(g, n->id, p);
            }
            touch(w);
        }
        if (ui_menu_item(ICON_REFRESH, TR("Refresh Nodes"), NULL, false, true)) {
            for (uint32_t s = 0; s < w->nsel; s++) {
                BpNode* n = bp_node(g, w->sel[s]);
                if (n) bp_refresh_pins(a, g, n);
            }
            touch(w);
        }
        popup_end();
    }
    action_menu(w);
    // the graph's keys (Unreal's) while the mouse is over it and nothing is being typed
    if (igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io->WantTextInput) {
        if ((igIsKeyPressed_Bool(ImGuiKey_Delete, false) || igIsKeyPressed_Bool(ImGuiKey_Backspace, false)) && w->nsel) delete_selected(w);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_D) && w->nsel) duplicate_selected(w);
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_A)) {
            w->nsel = 0;
            for (uint32_t i = 0; i < g->nnodes; i++) sel_add(w, g->nodes[i].id);
        }
        if (igIsKeyPressed_Bool(ImGuiKey_Home, false)) w->frame = true;
        if (!io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false)) organize_nodes(w); // (2026-11-23)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Z) && bp_graph_undo(w->a, false)) w->nsel = 0; // (2026-11-24)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_Y) && bp_graph_undo(w->a, true)) w->nsel = 0;
    }
    // the hints and the graph's name
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, o.y + size.y - igGetFontSize() - 8 }, TH(TK_N11),
                            TR("Right click: add a node  |  drag from a pin: what fits  |  B/S/D + click  |  Alt + click: break  |  Q: organize  |  Home: frame"), NULL);
    igPopFont();
    // the breadcrumb (class > graph) and the zoom, like Unreal's
    char crumb[160];
    snprintf(crumb, sizeof crumb, "%s  >  %s", a->name, w->graph == 0 ? "Event Graph" : g->name);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 12, o.y + 10 }, THA(TK_N14, 170), crumb, NULL);
    char zl[32];
    int zs = (int)lroundf(logf(z) / logf(1.12f));
    if (zs == 0) snprintf(zl, sizeof zl, "Zoom 1:1");
    else snprintf(zl, sizeof zl, "Zoom %+d", zs);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + size.x - igCalcTextSize(zl, NULL, false, -1).x - 14, o.y + 10 }, THA(TK_N13, 150), zl, NULL);
    igEndChild();
}

// --- Details ---

static void spawn_nodes_refresh(void);
static bool stristr_ascii(const char* h, const char* n) { // (the type search: case-insensitive)
    for (; *h; h++) {
        size_t k = 0;
        while (n[k] && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)n[k])) k++;
        if (!n[k]) return true;
    }
    return false;
}

// (2026-11-10, "para todo tipo de propriedade ter um tipo de variável, e deixe elas organizadas") Unreal's asset
// references as variable types: a Name holding the asset's name (what the nodes' asset pins take: Play Sequence's
// Sequence, Play Animation's Animation, Play Sound's Sound, Spawn System's System...), its class in ref; the default
// picked from the project's assets of that class
// (2026-11-24, item 11) each with the Content kinds the asset picker lists for it (Unreal's soft object references:
// Level = World, Level Sequence...); the value stays the asset's name, what the nodes' name and asset pins take
typedef struct AssetRefType { const char* label; uint64_t kinds; } AssetRefType;
static const AssetRefType g_asset_refs[] = {
    { "Level Sequence", AK(ASSET_LEVEL_SEQUENCE) }, { "Animation Sequence", AK(ASSET_ANIMATION) }, { "Anim Montage", AK(ASSET_ANIM_MONTAGE) },
    { "Sound", AK(ASSET_SOUND_WAVE) | AK(ASSET_SOUND_CUE) | AK(ASSET_METASOUND) }, { "Sound Mix", AK(ASSET_SOUND_MIX) }, { "Sound Class", AK(ASSET_SOUND_CLASS) },
    { "Material", AK(ASSET_MATERIAL) | AK(ASSET_MATERIAL_INSTANCE) }, { "Texture", AK(ASSET_TEXTURE_SOURCE) }, { "Static Mesh", AK(ASSET_STATIC_MESH) },
    { "Skeletal Mesh", AK(ASSET_SKELETAL_MESH) }, { "Particle System", AK(ASSET_PARTICLE_SYSTEM) }, { "Behavior Tree", AK(ASSET_BEHAVIOR_TREE) },
    { "Data Asset", AK(ASSET_DATA_ASSET) }, { "Dialogue", AK(ASSET_DIALOGUE) }, { "Quest", AK(ASSET_QUEST) }, { "Input Action", AK(ASSET_INPUT_ACTION) },
    { "Input Mapping Context", AK(ASSET_INPUT_CONTEXT) }, { "Level", AK(ASSET_LEVEL) }, { "Vehicle", AK(ASSET_VEHICLE) },
    { "Physics Asset", AK(ASSET_PHYSICS_ASSET) }, { "IK Rig", AK(ASSET_IK_RIG) },
    // (2026-11-24) the rest the Content knows
    { "Camera Shake", AK(ASSET_CAMERA_SHAKE) }, { "PCG Graph", AK(ASSET_PCG_GRAPH) }, { "Physical Material", AK(ASSET_PHYS_MATERIAL) },
    { "Sound Attenuation", AK(ASSET_SOUND_ATTENUATION) }, { "Sound Concurrency", AK(ASSET_SOUND_CONCURRENCY) }, { "Submix", AK(ASSET_SUBMIX) },
    { "AI Brain", AK(ASSET_BRAIN) },
};
#define NASSET_REFS (int)(sizeof g_asset_refs / sizeof g_asset_refs[0])
static int asset_ref_index(uint8_t type, const char* ref) {
    if (type != BPT_NAME || !ref || !ref[0]) return -1;
    for (int i = 0; i < NASSET_REFS; i++)
        if (!strcmp(g_asset_refs[i].label, ref)) return i;
    return -1;
}
// The asset reference's value: one of the project's assets of its class (by name, as the nodes keep it), picked in the
// asset picker filtered by its kinds (2026-11-24: Unreal's asset field, thumbnail and search; it was a list of names)
static bool asset_ref_pick(const char* label, int ar, char* s, size_t scap) {
    if (g_show.var_pending && ++g_show.frames > 10) asset_field_open_next(), g_show.var_pending = false; // (--show bp-var)
    int kind = 0;
    while (kind < 63 && !(g_asset_refs[ar].kinds & (1ull << kind))) kind++;
    char out[512] = "";
    if (asset_field(label, s[0] ? s : "None", NULL, kind, g_asset_refs[ar].kinds, true, out, sizeof out) != ASSET_FIELD_PICKED) return false;
    const char* b = strrchr(out, '/');
    snprintf(s, scap, "%s", b ? b + 1 : out);
    char* dot = strrchr(s, '.');
    if (dot) *dot = 0;
    return true;
}

static bool type_combo_ex(const char* label, uint8_t* type, char* ref, bool exec) {
    bool ch = false;
    char shown[BP_NAME + 32];
    int ar = asset_ref_index(*type, ref);
    if (*type == BPT_ACTOR && ref[0]) snprintf(shown, sizeof shown, "%s (%s)", ref, TR("Object Reference"));
    else if (*type == BPT_CLASS && ref[0]) snprintf(shown, sizeof shown, "%s (%s)", ref, TR("Class Reference"));
    else if (ar >= 0) snprintf(shown, sizeof shown, "%s (%s)", TR(g_asset_refs[ar].label), TR("Asset"));
    else if ((*type == BPT_STRUCT || *type == BPT_ENUM) && ref[0]) snprintf(shown, sizeof shown, "%s", ref);
    else snprintf(shown, sizeof shown, "%s", g_bp_type_names[*type]);
    if (igBeginCombo(label, shown, ImGuiComboFlags_HeightLargest)) {
        // (organized as Unreal's: a search, then the basic types, structures, enumerations, assets, actors, classes)
        static char search[64];
        if (igIsWindowAppearing()) search[0] = 0, igSetKeyboardFocusHere(0);
        igSetNextItemWidth(-1);
        igInputTextWithHint("##type_search", TR("Search"), search, sizeof search, 0, NULL, NULL);
#define TYPE_MATCH(name) (!search[0] || stristr_ascii((name), search))
        igSeparatorText(TR("Basic"));
        for (int t = exec ? BPT_EXEC : BPT_BOOL; t < BPT_COUNT; t++) {
            if (t == BPT_STRUCT || t == BPT_ENUM || t == BPT_WILDCARD || !TYPE_MATCH(g_bp_type_names[t])) continue;
            igPushStyleColor_U32(ImGuiCol_Text, type_color((uint8_t)t));
            if (igSelectable_Bool(g_bp_type_names[t], *type == t && !ref[0], 0, (ImVec2){ 0, 0 })) *type = (uint8_t)t, ref[0] = 0, ch = true;
            igPopStyleColor(1);
        }
        for (int pass = 0; pass < 2; pass++) { // the project's structures, then its enumerations
            bool head = false;
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                BpAsset* s = bp_asset_at(i);
                if (s->kind != (pass ? BPA_ENUM : BPA_STRUCT) || !TYPE_MATCH(s->name)) continue;
                if (!head) igSeparatorText(pass ? TR("Enumerations") : TR("Structures")), head = true;
                uint8_t t = pass ? BPT_ENUM : BPT_STRUCT;
                igPushStyleColor_U32(ImGuiCol_Text, type_color(t));
                if (igSelectable_Bool(s->name, *type == t && !strcmp(ref, s->name), 0, (ImVec2){ 0, 0 })) {
                    *type = t;
                    snprintf(ref, BP_NAME, "%s", s->name);
                    ch = true;
                }
                igPopStyleColor(1);
            }
        }
        { // the assets (a Name of the asset's class)
            bool head = false;
            for (int i = 0; i < NASSET_REFS; i++) {
                if (!TYPE_MATCH(g_asset_refs[i].label) && !TYPE_MATCH(TR(g_asset_refs[i].label))) continue;
                if (!head) igSeparatorText(TR("Asset References")), head = true;
                igPushStyleColor_U32(ImGuiCol_Text, TH(TK_WARN));
                if (igSelectable_Bool(TR(g_asset_refs[i].label), ar == i, 0, (ImVec2){ 0, 0 })) {
                    *type = BPT_NAME;
                    snprintf(ref, BP_NAME, "%s", g_asset_refs[i].label);
                    ch = true;
                }
                igPopStyleColor(1);
            }
        }
        // the project's Blueprint classes: a reference to an actor of one, or to the class itself
        for (int k = 0; k < 2; k++) {
            uint8_t t = k ? BPT_CLASS : BPT_ACTOR;
            bool head = false;
            igPushStyleColor_U32(ImGuiCol_Text, type_color(t));
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                BpAsset* s = bp_asset_at(i);
                if (s->kind != BPA_CLASS || !TYPE_MATCH(s->name)) continue;
                if (!head) {
                    igPushStyleColor_U32(ImGuiCol_Text, TH(TK_N13));
                    igSeparatorText(k ? TR("Class References") : TR("Object References"));
                    igPopStyleColor(1);
                    head = true;
                }
                char lbl[BP_NAME + 32];
                snprintf(lbl, sizeof lbl, "%s (%s)", s->name, k ? TR("Class Reference") : TR("Object Reference"));
                if (igSelectable_Bool(lbl, *type == t && !strcmp(ref, s->name), 0, (ImVec2){ 0, 0 })) {
                    *type = t;
                    snprintf(ref, BP_NAME, "%s", s->name);
                    ch = true;
                }
            }
            igPopStyleColor(1);
        }
#undef TYPE_MATCH
        if (ch) igCloseCurrentPopup();
        igEndCombo();
    }
    return ch;
}
static bool type_combo(const char* label, uint8_t* type, char* ref) { return type_combo_ex(label, type, ref, false); }

// Unreal's variable type: the type, then the container (Single, Array, Set, Map: the value's type beside it)
static bool type_picker(const char* id, BpVar* v, bool exec, float width) {
    static const char* const conts[BPC_COUNT] = { "Single", "Array", "Set", "Map" };
    bool ch = false;
    igPushID_Str(id);
    float cw = igGetFontSize() * 4.2f;
    igSetNextItemWidth(fmaxf(width - cw - 4.0f, 60.0f));
    ch |= type_combo_ex("##t", &v->type, v->ref, exec);
    if (v->type != BPT_EXEC) {
        igSameLine(0, 4);
        igSetNextItemWidth(cw);
        int c = v->cont;
        if (igCombo_Str_arr("##var_container", &c, conts, BPC_COUNT, -1)) {
            v->cont = (uint8_t)c, ch = true;
            if (c == BPC_MAP && !v->vtype) v->vtype = BPT_INT;
        }
        if (v->cont == BPC_MAP) {
            char vref[BP_NAME] = "";
            igSetNextItemWidth(width);
            igTextDisabled("%s", TR("Value type"));
            igSameLine(0, 4);
            igSetNextItemWidth(fmaxf(width - igGetFontSize() * 5.0f, 60.0f));
            ch |= type_combo("##vt", &v->vtype, vref);
        }
    } else v->cont = 0;
    igPopID();
    return ch;
}

static bool value_edit(const char* label, uint8_t type, const char* ref, BpValue* v, char* s, size_t scap);
bool bp_value_edit(const char* label, unsigned char type, const char* ref, struct BpValue* v, char* s, size_t scap) { return value_edit(label, type, ref, v, s, scap); }
static bool value_edit(const char* label, uint8_t type, const char* ref, BpValue* v, char* s, size_t scap) {
    switch (type) {
    case BPT_BYTE:
        if (ui_num_float(label, &v->v[0], 0.2f, 0, 255, 0, 255, "%.0f", 0)) { // (a byte: the type's own limit)
            v->v[0] = roundf(v->v[0]);
            return true;
        }
        return false;
    case BPT_INT64: {
        double d;
        memcpy(&d, v->v, 8);
        long long x = (long long)d;
        if (igDragScalar(label, ImGuiDataType_S64, &x, 0.2f, NULL, NULL, "%lld", 0)) {
            d = (double)x;
            memcpy(v->v, &d, 8);
            return true;
        }
        return false;
    }
    case BPT_COLOR: return igColorEdit4(label, v->v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    case BPT_NAME: {
        int ar = asset_ref_index(type, ref);
        if (ar >= 0) return s && asset_ref_pick(label, ar, s, scap); // (an asset reference: the project's assets of its class)
        return s && igInputText(label, s, scap, 0, NULL, NULL);
    }
    case BPT_TEXT: return s && igInputText(label, s, scap, 0, NULL, NULL);
    case BPT_CLASS: {
        bool ch = false;
        if (s && igBeginCombo(label, s[0] ? s : TR("None"), 0)) {
            if (igSelectable_Bool(TR("None"), !s[0], 0, (ImVec2){ 0, 0 })) s[0] = 0, ch = true;
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                const BpAsset* c = bp_asset_at(i);
                if (c->kind == BPA_CLASS && (!ref || !ref[0] || bp_is_child_of(c, ref)) && igSelectable_Bool(c->name, !strcmp(s, c->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(s, scap, "%s", c->name), ch = true;
            }
            igEndCombo();
        }
        return ch;
    }
    case BPT_BOOL: {
        bool b = v->v[0] != 0.0f;
        if (igCheckbox(label, &b)) {
            v->v[0] = b ? 1.0f : 0.0f;
            return true;
        }
        return false;
    }
    case BPT_INT:
        if (igDragFloat(label, &v->v[0], 0.2f, 0, 0, "%.0f", 0)) {
            v->v[0] = roundf(v->v[0]);
            return true;
        }
        return false;
    case BPT_FLOAT: return igDragFloat(label, &v->v[0], 0.05f, 0, 0, "%.4g", 0);
    case BPT_VECTOR:
    case BPT_ROTATOR: return igDragFloat3(label, v->v, 0.5f, 0, 0, "%.4g", 0);
    case BPT_STRING: return s && igInputText(label, s, scap, 0, NULL, NULL);
    case BPT_ENUM: {
        const BpAsset* e = bp_find_name(BPA_ENUM, ref);
        uint32_t cur = (uint32_t)v->v[0];
        bool ch = false;
        if (igBeginCombo(label, e && cur < e->nentries ? e->entries[cur] : "?", 0)) {
            for (uint32_t k = 0; e && k < e->nentries; k++)
                if (igSelectable_Bool(e->entries[k], k == cur, 0, (ImVec2){ 0, 0 })) v->v[0] = (float)k, ch = true;
            igEndCombo();
        }
        return ch;
    }
    default: igTextDisabled("%s", type == BPT_ACTOR ? TR("None") : TR("(the fields' defaults)")); return false;
    }
}

// A name typed in place of another: every node that named it follows
static void rename_refs(BpAsset* a, uint16_t k1, uint16_t k2, const char* from, const char* to) {
    for (uint32_t g = 0; g < a->ngraphs; g++)
        for (uint32_t i = 0; i < a->graphs[g].nnodes; i++) {
            BpNode* n = &a->graphs[g].nodes[i];
            if ((n->kind == k1 || n->kind == k2) && !strcmp(n->text, from)) {
                snprintf(n->text, sizeof n->text, "%s", to);
                bp_refresh_pins(a, &a->graphs[g], n);
            }
        }
}

static bool name_combo(const char* label, char* text, BpAssetKind kind, const char* extra1, const char* extra2) {
    bool ch = false;
    if (igBeginCombo(label, text[0] ? text : "-", 0)) {
        if (extra1 && igSelectable_Bool(extra1, !strcmp(text, extra1), 0, (ImVec2){ 0, 0 })) snprintf(text, BP_NAME, "%s", extra1), ch = true;
        if (extra2 && igSelectable_Bool(extra2, !strcmp(text, extra2), 0, (ImVec2){ 0, 0 })) snprintf(text, BP_NAME, "%s", extra2), ch = true;
        for (uint32_t i = 0; i < bp_asset_count(); i++) {
            BpAsset* o = bp_asset_at(i);
            if (o->kind == kind && igSelectable_Bool(o->name, !strcmp(text, o->name), 0, (ImVec2){ 0, 0 }))
                snprintf(text, BP_NAME, "%s", o->name), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

static bool list_combo(const char* label, char* text, const char* const* items, uint32_t n, size_t stride) {
    bool ch = false;
    if (igBeginCombo(label, text[0] ? text : "-", 0)) {
        for (uint32_t i = 0; i < n; i++) {
            const char* it = stride ? (const char*)items + i * stride : items[i];
            if (igSelectable_Bool(it, !strcmp(text, it), 0, (ImVec2){ 0, 0 })) snprintf(text, BP_NAME, "%s", it), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

static void details_node(Win* w) {
    BpAsset* a = w->a;
    BpGraph* g = &a->graphs[w->graph];
    BpNode* n = w->nsel ? bp_node(g, w->sel[w->nsel - 1]) : NULL;
    if (!n) {
        w->details = DET_CLASS;
        return;
    }
    const BpNodeDesc* d = bp_node_desc(n->kind);
    igSeparatorText(d->name);
    igTextWrapped("%s", d->tooltip);
    igSpacing();
    bool ch = false;
    char old[BP_NAME];
    snprintf(old, sizeof old, "%s", n->text);
    switch (n->kind) {
    case BPK_EVENT_BEGIN_OVERLAP: case BPK_EVENT_END_OVERLAP: case BPK_EVENT_OVERLAPPING: // Unreal's OnActorBeginOverlap (Actor) of the Level Blueprint
        ch = igInputTextWithHint(PL(TR("Overlapped Actor")), TR("(this actor)"), n->text, sizeof n->text, 0, NULL, NULL);
        igTextDisabled("%s", TR("Empty: this actor's overlaps. A name (Level Blueprint): that actor's in the level, e.g. a Trigger Box."));
        break;
    case BPK_EVENT_KEY:
    case BPK_KEY_DOWN:
        ch = list_combo(PL(TR("Key")), n->text, g_keys, sizeof g_keys / sizeof g_keys[0], 0);
        ch |= igInputText(PL(TR("Key name")), n->text, sizeof n->text, 0, NULL, NULL);
        if (bp_key_code(n->text) < 0) igTextColored(THV(TK_ERROR), "%s", TR("Unknown key"));
        break;
    case BPK_AXIS: ch = list_combo(PL(TR("Axis")), n->text, g_axes, 4, 0); break;
    case BPK_BB_GET_FLOAT: case BPK_BB_GET_BOOL: case BPK_BB_GET_VECTOR: case BPK_BB_GET_ACTOR:
    case BPK_BB_SET_FLOAT: case BPK_BB_SET_BOOL: case BPK_BB_SET_VECTOR: case BPK_BB_SET_ACTOR: {
        ch |= igInputText(PL(TR("Key")), n->text, sizeof n->text, 0, NULL, NULL);
        static const char* const keys[] = { "TargetActor", "CanSeeTarget", "LastKnownLocation", "DistanceToTarget", "HeardNoise",
                                            "NoiseLocation", "SelfLocation", "Destination" };
        ch |= list_combo(PL(TR("Perception keys")), n->text, keys, sizeof keys / sizeof keys[0], 0);
        break;
    }
    case BPK_RUN_BT:
        if (igBeginCombo(PL(TR("Behavior Tree")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < ai_tree_count(); i++)
                if (igSelectable_Bool(ai_tree_at(i)->name, !strcmp(n->text, ai_tree_at(i)->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", ai_tree_at(i)->name), ch = true;
            igEndCombo();
        }
        break;
    case BPK_PLAY_MONTAGE:
    case BPK_PLAY_ANIMATION: { // the asset by its name
        bool mont = n->kind == BPK_PLAY_MONTAGE;
        if (igBeginCombo(mont ? TR("Montage") : TR("Animation"), n->text[0] ? n->text : "-", 0)) {
            uint32_t cnt = mont ? anim_montage_count() : anim_seq_count();
            for (uint32_t i = 0; i < cnt; i++) {
                const char* nm = mont ? anim_montage_at(i)->name : anim_seq_at(i)->name;
                if (asset_selectable(nm, !strcmp(n->text, nm), mont ? NULL : anim_seq_at(i)->path, ASSET_ANIMATION)) snprintf(n->text, sizeof n->text, "%s", nm), ch = true;
            }
            igEndCombo();
        }
        break;
    }
    case BPK_PLAY_SOUND_2D: case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_2D: case BPK_SPAWN_SOUND_AT: case BPK_SPAWN_SOUND_ATTACHED:
    case BPK_AUDIO_SET_SOUND: case BPK_PUSH_SOUND_MIX: case BPK_POP_SOUND_MIX: case BPK_SET_BASE_SOUND_MIX: case BPK_SET_CLASS_VOLUME: {
        int kind = n->kind == BPK_SET_CLASS_VOLUME ? AUA_CLASS : n->kind >= BPK_PUSH_SOUND_MIX ? AUA_MIX : -1;
        uint32_t h = audio_asset_by_name(kind, n->text);
        const char* label = kind == AUA_CLASS ? "Sound Class" : kind == AUA_MIX ? "Sound Mix" : "Sound";
        if (audio_pick(PL(TR(label)), &h, kind)) { // (the node keeps the name: found when it runs)
            snprintf(n->text, sizeof n->text, "%s", h ? au_sound_name(h) : "");
            ch = true;
        }
        if (n->kind == BPK_SPAWN_SOUND_ATTACHED)
            ch |= igInputTextWithHint(PL(TR("Attach Point Name")), TR("(a bone; none: the actor)"), n->text2, sizeof n->text2, 0, NULL, NULL);
        break;
    }
    case BPK_SPAWN_DECAL: { // the Decal Material (its name: found when it runs)
        if (igBeginCombo(PL(TR("Decal Material")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < mat_count(); i++) {
                const Material* m = mat_at(i);
                if (m && igSelectable_Bool(m->name, !strcmp(n->text, m->name), 0, (ImVec2){ 0, 0 })) snprintf(n->text, sizeof n->text, "%s", m->name), ch = true;
            }
            igEndCombo();
        }
        break;
    }
    case BPK_PS_SPAWN: { // the Particle System (its name: found when it runs)
        if (igBeginCombo(PL(TR("Particle System")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < ps_asset_count(); i++) {
                const ParticleSystemAsset* pa = ps_asset_at(i);
                if (pa && igSelectable_Bool(pa->name, !strcmp(n->text, pa->name), 0, (ImVec2){ 0, 0 })) snprintf(n->text, sizeof n->text, "%s", pa->name), ch = true;
            }
            if (!ps_asset_count()) igTextDisabled("%s", TR("No Particle System in the project (Content Browser > Add > Visual Effects)."));
            igEndCombo();
        }
        break;
    }
    case BPK_PLAY_SEQUENCE:
    case BPK_STOP_SEQUENCE:
    case BPK_IS_SEQUENCE_PLAYING: { // a Level Sequence of the project (the Sequencer's .rseq)
        static char list[256][300];
        static uint32_t nlist;
        const char* b = strrchr(n->text, '/');
        (void)b;
        if (igBeginCombo(PL(TR("Level Sequence")), n->text[0] ? n->text : "-", 0)) { // (the node keeps the name: seq_load finds it)
            if (igIsWindowAppearing()) nlist = seq_list(list, 256);
            if (n->kind == BPK_STOP_SEQUENCE && igSelectable_Bool(TR("All Sequences"), !n->text[0], 0, (ImVec2){ 0, 0 })) n->text[0] = 0, ch = true;
            for (uint32_t i = 0; i < nlist; i++) {
                const char* nb = strrchr(list[i], '/');
                char nm[BP_NAME];
                snprintf(nm, sizeof nm, "%s", nb ? nb + 1 : list[i]);
                char* dot = strrchr(nm, '.');
                if (dot) *dot = 0;
                if (igSelectable_Bool(nm, !strcmp(n->text, nm), 0, (ImVec2){ 0, 0 })) snprintf(n->text, sizeof n->text, "%s", nm), ch = true;
                igSetItemTooltip("%s", list[i]);
            }
            if (!nlist) igTextDisabled("%s", TR("No Level Sequence in the project (Content Browser > Add > Cinematics)."));
            igEndCombo();
        }
        break;
    }
    case BPK_PLAY_WIDGET_ANIM:
    case BPK_STOP_WIDGET_ANIM: { // a Widget Animation of this Widget Blueprint (or of the first that has the name)
        const BpAsset* src = a->ui ? a : NULL;
        for (uint32_t i = 0; !src && i < bp_asset_count(); i++)
            if (bp_asset_at(i)->ui && ui_anim_find(bp_asset_at(i)->ui, n->text) >= 0) src = bp_asset_at(i);
        if (ui_anim_combo(TR("Animation"), n->text, sizeof n->text, src)) ch = true;
        if (!a->ui) ch |= igInputText(PL(TR("Animation Name")), n->text, sizeof n->text, 0, NULL, NULL);
        break;
    }
    case BPK_WIDGET_SET_VALUE:
    case BPK_WIDGET_GET_VALUE:
    case BPK_WIDGET_GET_TEXT:
    case BPK_WIDGET_SET_ENABLED:
    case BPK_WIDGET_SET_TEXT:
    case BPK_WIDGET_SET_PERCENT:
    case BPK_WIDGET_SET_VISIBLE:
    case BPK_WIDGET_SET_COLOR: { // the widget of this Widget Blueprint's tree (or of the class the Target is)
        const uint64_t lists = (1ull << UIW_LIST_VIEW) | (1ull << UIW_TILE_VIEW); // (B7: their items / the selected one)
        const uint64_t valued = (1ull << UIW_SLIDER) | (1ull << UIW_SPIN) | (1ull << UIW_CHECKBOX) | (1ull << UIW_COMBO) | (1ull << UIW_PROGRESS) | lists;
        uint64_t kinds = n->kind == BPK_WIDGET_SET_TEXT ? (1ull << UIW_TEXT) | (1ull << UIW_BUTTON) | (1ull << UIW_EDIT) | (1ull << UIW_RICH_TEXT) | lists
                         : n->kind == BPK_WIDGET_SET_PERCENT ? 1ull << UIW_PROGRESS
                         : n->kind == BPK_WIDGET_SET_VALUE || n->kind == BPK_WIDGET_GET_VALUE ? valued
                         : n->kind == BPK_WIDGET_GET_TEXT ? (1ull << UIW_TEXT) | (1ull << UIW_EDIT) | (1ull << UIW_COMBO) | (1ull << UIW_RICH_TEXT) | lists : 0ull;
        const BpAsset* src = a->ui ? a : NULL;
        for (uint32_t i = 0; !src && i < bp_asset_count(); i++) // (from another Blueprint: the first Widget Blueprint that has the name)
            if (bp_asset_at(i)->ui && ui_tree_find(bp_asset_at(i)->ui, n->text) >= 0) src = bp_asset_at(i);
        if (ui_widget_combo(TR("Widget"), n->text, sizeof n->text, src, kinds)) ch = true;
        if (!a->ui) ch |= igInputText(PL(TR("Widget Name")), n->text, sizeof n->text, 0, NULL, NULL);
        break;
    }
    case BPK_PP_SET_FLOAT:
        if (igBeginCombo(PL(TR("Setting")), n->text[0] ? TR(n->text) : "-", 0)) {
            const char* grp = "";
            for (int f = 0; f < PP_FIELD_COUNT; f++) {
                if (strcmp(grp, g_post_fields[f].group)) igSeparatorText(TR(grp = g_post_fields[f].group));
                if (igSelectable_Bool(TR(g_post_fields[f].name), !strcmp(n->text, g_post_fields[f].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", g_post_fields[f].name), ch = true;
            }
            igEndCombo();
        }
        break;
    case BPK_MAT_SCALAR:
    case BPK_MAT_VECTOR:
        ch = igInputText(PL(TR("Parameter Name")), n->text, sizeof n->text, 0, NULL, NULL);
        igTextDisabled("%s", TR("The name of a Value / RGB node of the actor's material (its Parameter Name)."));
        break;
    case BPK_EVENT_CUSTOM: {
        BpGraph* sg = NULL; // its inputs (a signature graph of its name, made with the first input)
        for (uint32_t i = 0; i < a->ngraphs && !sg; i++)
            if (a->graphs[i].gkind == BPG_EVENT && !strcmp(a->graphs[i].name, old)) sg = &a->graphs[i];
        if (igInputText(PL(TR("Name")), n->text, sizeof n->text, 0, NULL, NULL)) {
            rename_refs(a, BPK_CALL_CUSTOM, BPK_CALL_CUSTOM, old, n->text);
            if (sg) snprintf(sg->name, sizeof sg->name, "%s", n->text);
            ch = true;
        }
        { // Unreal's Replicates: Not Replicated, Multicast, Run on Server, Run on owning Client; Reliable
            static const char* const modes[4] = { "Not Replicated", "Multicast", "Run on Server", "Run on owning Client" };
            const char* ml[4] = { TR(modes[0]), TR(modes[1]), TR(modes[2]), TR(modes[3]) };
            int m = n->net & 3;
            if (igCombo_Str_arr(PL(TR("Replicates")), &m, ml, 4, -1)) n->net = (uint8_t)((n->net & ~3) | m), ch = true;
            igSetItemTooltip("%s", TR("Multicast: the server runs it on every machine. Run on Server: a client asks the server. Run on owning Client: the server runs it on the owner's machine. Calling the event sends it; its first Float, Vector and Text travel (Get RPC Float / Vector / Text there)."));
            bool rel = (n->net & BP_NET_RELIABLE) != 0;
            if (m) {
                if (igCheckbox(PL(TR("Reliable")), &rel)) n->net = (uint8_t)(rel ? n->net | BP_NET_RELIABLE : n->net & ~BP_NET_RELIABLE), ch = true;
                igSetItemTooltip("%s", TR("Always arrives (resent when lost): actions. Off: may be dropped under loss (effects that repeat often)."));
            }
        }
        igSeparatorText(TR("Inputs"));
        bool sig = false;
        for (uint8_t i = 0; sg && i < sg->nparams; i++) {
            igPushID_Int(300 + i);
            igSetNextItemWidth(110);
            if (igInputText("##disp_param", sg->params[i].name, sizeof sg->params[i].name, 0, NULL, NULL)) sig = true;
            igSameLine(0, 4);
            sig |= type_picker("##t", &sg->params[i], false, fmaxf(igGetContentRegionAvail().x - 30.0f, 120.0f));
            igSameLine(0, 4);
            if (igSmallButton("x")) {
                memmove(&sg->params[i], &sg->params[i + 1], sizeof(BpVar) * (size_t)(sg->nparams - i - 1));
                sg->nparams--;
                sig = true;
            }
            igPopID();
        }
        if ((!sg || sg->nparams < 6) && n->text[0] && igSmallButton(TR("+ Add"))) {
            if (!sg) sg = bp_add_graph(a, n->text, BPG_EVENT);
            if (sg) {
                BpVar* v = &sg->params[sg->nparams++];
                memset(v, 0, sizeof *v);
                snprintf(v->name, sizeof v->name, "Param%u", sg->nparams);
                v->type = BPT_FLOAT;
                sig = true;
            }
        }
        if (sig) bp_refresh_all(), ch = true; // (the event and its calls, here and in other classes, take the inputs)
        break;
    }
    case BPK_VAR_GET:
    case BPK_VAR_SET: {
        if (igBeginCombo(PL(TR("Variable")), n->text, 0)) {
            for (uint32_t i = 0; i < a->nvars; i++)
                if (igSelectable_Bool(a->vars[i].name, !strcmp(n->text, a->vars[i].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", a->vars[i].name), ch = true;
            igEndCombo();
        }
        break;
    }
    case BPK_CAST: ch = name_combo(PL(TR("Class")), n->text, BPA_CLASS, "Character", "Pawn"); break;
    case BPK_MACRO: {
        if (igBeginCombo(PL(TR("Macro")), n->text, 0)) {
            for (const BpAsset* c = a; c; c = bp_find_name(BPA_CLASS, c->parent)) {
                for (uint32_t i = 1; i < c->ngraphs; i++)
                    if (c->graphs[i].gkind == BPG_MACRO && igSelectable_Bool(c->graphs[i].name, !strcmp(n->text, c->graphs[i].name), 0, (ImVec2){ 0, 0 }))
                        snprintf(n->text, sizeof n->text, "%s", c->graphs[i].name), n->text2[0] = 0, ch = true;
                if (c == bp_find_name(BPA_CLASS, c->parent)) break;
            }
            for (uint32_t k = 0; k < bp_asset_count(); k++) {
                const BpAsset* l = bp_asset_at(k);
                for (uint32_t i = 0; l->kind == BPA_MACROLIB && i < l->ngraphs; i++) {
                    char lbl[BP_NAME * 2 + 4];
                    snprintf(lbl, sizeof lbl, "%s.%s", l->name, l->graphs[i].name);
                    if (igSelectable_Bool(lbl, !strcmp(n->text, l->graphs[i].name) && !strcmp(n->text2, l->name), 0, (ImVec2){ 0, 0 }))
                        snprintf(n->text, sizeof n->text, "%s", l->graphs[i].name), snprintf(n->text2, sizeof n->text2, "%s", l->name), ch = true;
                }
            }
            igEndCombo();
        }
        break;
    }
    case BPK_EVENT_INPUT_ACTION:
    case BPK_GET_ACTION_VALUE:
        if (igBeginCombo(PL(TR("Input Action")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < input_action_count(); i++)
                if (igSelectable_Bool(input_action_at(i)->name, !strcmp(n->text, input_action_at(i)->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", input_action_at(i)->name), ch = true;
            igEndCombo();
        }
        break;
    case BPK_ADD_MAPPING_CONTEXT:
    case BPK_REMOVE_MAPPING_CONTEXT:
        if (igBeginCombo(PL(TR("Mapping Context")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < input_context_count(); i++)
                if (igSelectable_Bool(input_context_at(i)->name, !strcmp(n->text, input_context_at(i)->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", input_context_at(i)->name), ch = true;
            igEndCombo();
        }
        break;
    case BPK_NET_REPLICATE_VAR: // (a variable of this Blueprint: numbers, vectors, texts)
        if (igBeginCombo(PL(TR("Variable")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < a->nvars; i++)
                if (igSelectable_Bool(a->vars[i].name, !strcmp(n->text, a->vars[i].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", a->vars[i].name), ch = true;
            igEndCombo();
        }
        break;
    case BPK_NET_RPC_SERVER:
    case BPK_NET_RPC_MULTICAST:
    case BPK_NET_RPC_OWNER:
    case BPK_SET_TIMER:
    case BPK_CLEAR_TIMER:
        if (igBeginCombo(PL(TR("Event")), n->text[0] ? n->text : "-", 0)) {
            for (uint32_t i = 0; i < a->graphs[0].nnodes; i++) {
                const BpNode* e = &a->graphs[0].nodes[i];
                if (e->kind == BPK_EVENT_CUSTOM && igSelectable_Bool(e->text, !strcmp(n->text, e->text), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", e->text), ch = true;
            }
            igEndCombo();
        }
        break;
    case BPK_CALL_FUNCTION:
        if (igBeginCombo(PL(TR("Function")), n->text, 0)) {
            for (uint32_t i = 1; i < a->ngraphs; i++)
                if (igSelectable_Bool(a->graphs[i].name, !strcmp(n->text, a->graphs[i].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", a->graphs[i].name), ch = true;
            igEndCombo();
        }
        break;
    case BPK_CALL_CUSTOM: {
        // Target Class: empty, this Blueprint's events (run here); a class, its events run on the Target actor
        if (igBeginCombo(PL(TR("Target Class")), n->text2[0] ? n->text2 : TR("(Self)"), 0)) {
            if (igSelectable_Bool(TR("(Self)"), !n->text2[0], 0, (ImVec2){ 0, 0 })) n->text2[0] = 0, ch = true;
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                const BpAsset* c = bp_asset_at(i);
                if (c->kind == BPA_CLASS && c != a && igSelectable_Bool(c->name, !strcmp(n->text2, c->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text2, sizeof n->text2, "%s", c->name), ch = true;
            }
            igEndCombo();
        }
        const BpAsset* ec = n->text2[0] ? bp_find_name(BPA_CLASS, n->text2) : a;
        if (igBeginCombo(PL(TR("Event")), n->text, 0)) {
            for (uint32_t i = 0; ec && ec->ngraphs && i < ec->graphs[0].nnodes; i++) {
                const BpNode* e = &ec->graphs[0].nodes[i];
                if (e->kind == BPK_EVENT_CUSTOM && igSelectable_Bool(e->text, !strcmp(n->text, e->text), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text, sizeof n->text, "%s", e->text), ch = true;
            }
            igEndCombo();
        }
        break;
    }
    case BPK_CALL_INTERFACE:
    case BPK_DOES_IMPLEMENT:
    case BPK_EVENT_INTERFACE: {
        if (n->kind != BPK_EVENT_INTERFACE) ch = name_combo(PL(TR("Interface")), n->text, BPA_INTERFACE, NULL, NULL);
        const BpAsset* in = bp_find_name(BPA_INTERFACE, n->text);
        if (n->kind == BPK_CALL_INTERFACE && in) ch |= list_combo(PL(TR("Function")), n->text2, (const char* const*)in->entries, in->nentries, BP_NAME);
        if (n->kind == BPK_EVENT_INTERFACE) ch |= igInputText(PL(TR("Function")), n->text, sizeof n->text, 0, NULL, NULL);
        break;
    }
    case BPK_MAKE_STRUCT:
    case BPK_BREAK_STRUCT: ch = name_combo(PL(TR("Structure")), n->text, BPA_STRUCT, NULL, NULL); break;
    case BPK_SWITCH_ENUM:
    case BPK_ENUM_LITERAL: {
        ch = name_combo(PL(TR("Enumeration")), n->text, BPA_ENUM, NULL, NULL);
        const BpAsset* e = bp_find_name(BPA_ENUM, n->text);
        if (n->kind == BPK_ENUM_LITERAL && e) ch |= list_combo(PL(TR("Entry")), n->text2, (const char* const*)e->entries, e->nentries, BP_NAME);
        break;
    }
    case BPK_DATA_GET: {
        ch = name_combo(PL(TR("Data Asset")), n->text, BPA_DATA, NULL, NULL);
        const BpAsset* da = bp_find_name(BPA_DATA, n->text);
        if (da && igBeginCombo(PL(TR("Value")), n->text2, 0)) {
            for (uint32_t i = 0; i < da->nvars; i++)
                if (igSelectable_Bool(da->vars[i].name, !strcmp(n->text2, da->vars[i].name), 0, (ImVec2){ 0, 0 }))
                    snprintf(n->text2, sizeof n->text2, "%s", da->vars[i].name), ch = true;
            igEndCombo();
        }
        break;
    }
    default: break;
    }
    if (ch) {
        bp_refresh_pins(a, g, n);
        touch(w);
    }
    if (igInputText(PL(TR("Comment")), n->comment, sizeof n->comment, 0, NULL, NULL)) touch(w);
    // the inputs' values (also typed on the node)
    bool any = false;
    for (uint8_t p = 0; p < n->npins; p++) {
        BpPin* q = &n->pins[p];
        if (q->out || q->type == BPT_EXEC || bp_pin_linked(g, n->id, p) || q->type == BPT_ACTOR || q->type == BPT_STRUCT || q->cont ||
            q->type == BPT_TRANSFORM || q->type == BPT_WILDCARD || q->type == BPT_OBJECT)
            continue;
        if (!any) igSeparatorText(TR("Inputs")), any = true;
        igPushID_Int(p);
        if (value_edit(q->name[0] ? q->name : "Value", q->type, q->ref, &q->lit, q->slit, sizeof q->slit)) touch(w);
        igPopID();
    }
}

static void details_var(Win* w) {
    BpAsset* a = w->a;
    if (w->var < 0 || (uint32_t)w->var >= a->nvars) {
        w->details = DET_CLASS;
        return;
    }
    BpVar* v = &a->vars[w->var];
    igSeparatorText(TR("Variable"));
    char old[BP_NAME];
    snprintf(old, sizeof old, "%s", v->name);
    if (igInputText(PL(TR("Variable Name")), v->name, sizeof v->name, 0, NULL, NULL)) {
        rename_refs(a, BPK_VAR_GET, BPK_VAR_SET, old, v->name);
        touch(w);
    }
    igAlignTextToFramePadding();
    igTextUnformatted(TR("Variable Type"), NULL);
    igSameLine(igGetFontSize() * 8.0f, 0);
    if (type_picker("vt", v, false, igGetContentRegionAvail().x)) {
        memset(&v->def, 0, sizeof v->def);
        v->nadef = 0;
        if (v->type == BPT_TRANSFORM) memset(v->def3, 0, sizeof v->def3), v->def3[1].v[0] = v->def3[1].v[1] = v->def3[1].v[2] = 1.0f;
        if (v->type == BPT_COLOR) v->def.v[3] = 1.0f;
        rename_refs(a, BPK_VAR_GET, BPK_VAR_SET, v->name, v->name); // their pins take the new type
        touch(w);
    }
    if (igCheckbox(PL(TR("Instance Editable")), &v->editable)) touch(w);
    if (igCheckbox(PL(TR("Expose on Spawn")), &v->expose_spawn)) touch(w), spawn_nodes_refresh(); // (Spawn Actor from Class: a pin)
    if (v->expose_spawn && !bp_var_overridable(v)) igTextDisabled("%s", TR("(only single values: not containers, structures, actors or transforms)"));
    if (igCheckbox(PL(TR("SaveGame")), &v->save_game)) touch(w); // (save_progress.h: a Saveable actor's saves keep it)
    { // Unreal's Replication: None, Replicated (the server sends it to every client), RepNotify (and runs OnRep_<Name>)
        static const char* const reps[3] = { "None", "Replicated", "RepNotify" };
        const char* rl[3] = { TR(reps[0]), TR(reps[1]), TR(reps[2]) };
        int r = v->rep <= BP_REP_NOTIFY ? v->rep : 0;
        if (igCombo_Str_arr(PL(TR("Replication")), &r, rl, 3, -1)) {
            v->rep = (uint8_t)r;
            if (r == BP_REP_NOTIFY) { // (its OnRep_ event made in the Event Graph, as Unreal makes its function)
                char ev[BP_NAME + 8];
                snprintf(ev, sizeof ev, "OnRep_%s", v->name);
                bp_add_custom_event(a, ev);
            }
            touch(w);
        }
        igSetItemTooltip("%s", TR("Replicated: setting it on the server sends it to every client. RepNotify: also runs the Custom Event OnRep_<Name> wherever it arrives (and on the server)."));
    }
    if (igInputText(PL(TR("Category")), v->category, sizeof v->category, 0, NULL, NULL)) touch(w);
    if (igInputText(PL(TR("Tooltip")), v->tooltip, sizeof v->tooltip, 0, NULL, NULL)) touch(w);
    igSeparatorText(TR("Default Value"));
    if (v->cont == BPC_ARRAY || v->cont == BPC_SET) { // the elements (one-register types)
        if (bp_type_width(v->type, v->ref) != 1 || bp_type_stringish(v->type)) igTextDisabled("%s", TR("(starts empty: fill it in the graph)"));
        else {
            for (uint32_t i = 0; i < v->nadef; i++) {
                igPushID_Int((int)i);
                char lbl[16];
                snprintf(lbl, sizeof lbl, "[%u]", i);
                if (value_edit(lbl, v->type, v->ref, &v->adef[i], NULL, 0)) touch(w);
                igSameLine(0, 4);
                if (igSmallButton("x")) {
                    memmove(&v->adef[i], &v->adef[i + 1], sizeof(BpValue) * (v->nadef - i - 1));
                    v->nadef--;
                    touch(w);
                }
                igPopID();
            }
            if (v->nadef < BP_MAX_ADEF && igSmallButton(TR("+ Add Element"))) memset(&v->adef[v->nadef++], 0, sizeof(BpValue)), touch(w);
            igSameLine(0, 8);
            igTextDisabled(TR("%u elements"), v->nadef);
        }
    } else if (v->cont == BPC_MAP) igTextDisabled("%s", TR("(starts empty: Add (Map) in the graph)"));
    else if (v->type == BPT_TRANSFORM) {
        if (igDragFloat3(PL(TR("Location")), v->def.v, 0.5f, 0, 0, "%.3g", 0)) touch(w);
        if (igDragFloat3(PL(TR("Rotation")), v->def3[0].v, 0.5f, 0, 0, "%.3g", 0)) touch(w);
        if (igDragFloat3(PL(TR("Scale")), v->def3[1].v, 0.01f, 0, 0, "%.3g", 0)) touch(w);
    } else if (value_edit(v->name, v->type, v->ref, &v->def, v->sdef, sizeof v->sdef)) touch(w);
}

static void details_func(Win* w) {
    BpAsset* a = w->a;
    if (w->func < (a->kind == BPA_CLASS ? 1 : 0) || (uint32_t)w->func >= a->ngraphs) {
        w->details = DET_CLASS;
        return;
    }
    BpGraph* g = &a->graphs[w->func];
    bool macro = g->gkind == BPG_MACRO;
    bool disp = g->gkind == BPG_DISPATCHER;
    igSeparatorText(macro ? TR("Macro") : disp ? TR("Event Dispatcher") : TR("Function"));
    if (g->iface[0]) igTextDisabled(TR("Implements %s (its signature)"), g->iface);
    char old[BP_NAME];
    snprintf(old, sizeof old, "%s", g->name);
    bool sig = false;
    if (igInputText(PL(TR("Name")), g->name, sizeof g->name, 0, NULL, NULL)) {
        rename_refs(a, macro ? BPK_MACRO : BPK_CALL_FUNCTION, macro ? BPK_MACRO : BPK_CALL_FUNCTION, old, g->name);
        for (uint32_t i = 0; i < g->nnodes; i++)
            if (g->nodes[i].kind == BPK_FUNC_ENTRY || g->nodes[i].kind == BPK_FUNC_RETURN || g->nodes[i].kind == BPK_TUNNEL_IN ||
                g->nodes[i].kind == BPK_TUNNEL_OUT)
                snprintf(g->nodes[i].text, BP_NAME, "%s", g->name);
        touch(w);
    }
    if (g->iface[0]) return; // (an interface's signature is the interface's)
    for (int side = 0; side < (disp ? 1 : 2); side++) { // (a dispatcher: its inputs only)
        uint8_t* count = side ? &g->nresults : &g->nparams;
        BpVar* list = side ? g->results : g->params;
        uint8_t max = side ? 4 : 6;
        igSeparatorText(side ? TR("Outputs") : TR("Inputs"));
        for (uint8_t i = 0; i < *count; i++) {
            igPushID_Int(side * 16 + i);
            igSetNextItemWidth(110);
            if (igInputText("##n", list[i].name, sizeof list[i].name, 0, NULL, NULL)) sig = true;
            igSameLine(0, 4);
            sig |= type_picker("##t", &list[i], macro, fmaxf(igGetContentRegionAvail().x - 30.0f, 120.0f));
            igSameLine(0, 4);
            if (igSmallButton("x")) {
                memmove(&list[i], &list[i + 1], sizeof(BpVar) * (size_t)(*count - i - 1));
                (*count)--;
                sig = true;
            }
            igPopID();
        }
        igPushID_Int(100 + side);
        if (*count < max && igSmallButton(TR("+ Add"))) {
            BpVar* v = &list[(*count)++];
            memset(v, 0, sizeof *v);
            snprintf(v->name, sizeof v->name, side ? "Result%u" : "Param%u", *count);
            v->type = BPT_FLOAT;
            sig = true;
        }
        igPopID();
    }
    if (sig) { // the entry, return and call nodes take the new signature (a macro: its tunnels and its nodes)
        rename_refs(a, macro ? BPK_MACRO : BPK_CALL_FUNCTION, macro ? BPK_MACRO : BPK_CALL_FUNCTION, g->name, g->name);
        for (uint32_t i = 0; i < g->nnodes; i++) bp_refresh_pins(a, g, &g->nodes[i]);
        if (a->kind == BPA_MACROLIB || a->kind == BPA_INTERFACE) bp_refresh_all();
        if (disp) bp_refresh_all(); // (its Call and Event nodes, in any class)
        touch(w);
    }
}

static bool would_loop(const BpAsset* a, const char* parent) {
    for (const BpAsset* p = bp_find_name(BPA_CLASS, parent); p; p = bp_find_name(BPA_CLASS, p->parent)) {
        if (p == a) return true;
        if (p == bp_find_name(BPA_CLASS, p->parent)) break;
    }
    return false;
}

// The class's settings (Unreal's Class Settings): its parent and the interfaces it implements
static void details_settings(Win* w) {
    BpAsset* a = w->a;
    bool ch = false;
    igSeparatorText(TR("Class Settings"));
    if (igBeginCombo(PL(TR("Parent Class")), a->parent, 0)) {
        for (int n = 0; n < BPN_COUNT; n++)
            if (igSelectable_Bool(g_bp_native_names[n], !strcmp(a->parent, g_bp_native_names[n]), 0, (ImVec2){ 0, 0 }))
                snprintf(a->parent, sizeof a->parent, "%s", g_bp_native_names[n]), ch = true;
        for (uint32_t i = 0; i < bp_asset_count(); i++) {
            BpAsset* p = bp_asset_at(i);
            if (p->kind != BPA_CLASS || p == a || would_loop(a, p->name)) continue;
            if (igSelectable_Bool(p->name, !strcmp(a->parent, p->name), 0, (ImVec2){ 0, 0 })) snprintf(a->parent, sizeof a->parent, "%s", p->name), ch = true;
        }
        igEndCombo();
    }
    igTextDisabled(TR("Native class: %s"), g_bp_native_names[bp_native_of(a)]);
    igSeparatorText(TR("Interfaces"));
    for (uint32_t i = 0; i < a->ninterfaces; i++) {
        igPushID_Int((int)i);
        igBulletText("%s", a->interfaces[i]);
        igSameLine(0, 8);
        if (igSmallButton("x")) {
            memmove(a->interfaces[i], a->interfaces[i + 1], BP_NAME * (size_t)(a->ninterfaces - i - 1));
            a->ninterfaces--;
            ch = true;
        }
        igPopID();
    }
    char add[BP_NAME] = "";
    if (a->ninterfaces < 8 && name_combo(PL(TR("Implement")), add, BPA_INTERFACE, NULL, NULL) && add[0]) {
        snprintf(a->interfaces[a->ninterfaces++], BP_NAME, "%s", add);
        ch = true;
    }
    if (ch) touch(w);
}

// The components of a class as Unreal names them (a Character's root is its capsule)
static const char* comp_label(const BpAsset* a, int c) {
    bool chr = bp_native_of(a) == BPN_CHARACTER;
    switch (c) {
    case COMP_TRANSFORM: return chr ? "CapsuleComponent" : "DefaultSceneRoot";
    case COMP_STATIC_MESH: return chr ? "Mesh" : "StaticMesh";
    case COMP_CHARACTER: return "CharacterMovement";
    case COMP_ROTATING: return "RotatingMovement";
    case COMP_LIGHT: {
        const CLight* l = (const CLight*)a->comp_data[COMP_LIGHT];
        return l->type == LIGHT_SPOT ? "SpotLight" : "PointLight";
    }
    case COMP_HEALTH: return "HealthComponent";
    case COMP_SAVEABLE: return "SaveableComponent";
    default: return g_components[c].name;
    }
}

static UiIcon comp_icon(const BpAsset* a, int c) {
    switch (c) {
    case COMP_TRANSFORM: return bp_native_of(a) == BPN_CHARACTER ? ICON_CAPSULE : ICON_ACTOR;
    case COMP_STATIC_MESH: {
        const CStaticMesh* m = (const CStaticMesh*)a->comp_data[COMP_STATIC_MESH];
        return m->mesh < SHAPE_COUNT ? (UiIcon)(ICON_CUBE + m->mesh) : ICON_CUBE;
    }
    case COMP_CHARACTER: return ICON_MOVEMENT;
    case COMP_ROTATING: return ICON_UPDATE;
    case COMP_LIGHT: return ((const CLight*)a->comp_data[COMP_LIGHT])->type == LIGHT_SPOT ? ICON_SPOT : ICON_BULB;
    default: return ICON_COMPONENT;
    }
}

static bool flag_box(const char* label, uint32_t* v) {
    bool b = *v != 0;
    if (!igCheckbox(label, &b)) return false;
    *v = b;
    return true;
}

// One component's properties; true when something changed
static bool component_props(BpAsset* a, int c) {
    bool ch = false;
    switch (c) {
    case COMP_TRANSFORM: {
        CTransform* t = (CTransform*)a->comp_data[COMP_TRANSFORM];
        ch |= igDragFloat3(PL(TR("Scale")), t->scale, 0.01f, 0.01f, 1000.0f, "%.3f", 0);
        break;
    }
    case COMP_STATIC_MESH: {
        CStaticMesh* m = (CStaticMesh*)a->comp_data[COMP_STATIC_MESH];
        if (igBeginCombo(PL(TR("Mesh")), m->mesh < SHAPE_COUNT ? g_shape_names[m->mesh] : "?", 0)) {
            for (uint32_t s = 0; s < SHAPE_COUNT; s++)
                if (asset_selectable_mesh(g_shape_names[s], m->mesh == s, render_mesh_id(s))) m->mesh = s, ch = true;
            igEndCombo();
        }
        ch |= igColorEdit3(PL(TR("Base Color")), m->base_color, 0);
        ch |= igSliderFloat(PL(TR("Metallic")), &m->metallic, 0, 1, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Roughness")), &m->roughness, 0, 1, "%.2f", 0);
        ch |= igColorEdit3(PL(TR("Emissive")), m->emissive, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        bool cs = m->cast_shadow != 0;
        if (igCheckbox(PL(TR("Cast Shadow")), &cs)) m->cast_shadow = cs, ch = true;
        break;
    }
    case COMP_CHARACTER: {
        CCharacter* cc = (CCharacter*)a->comp_data[COMP_CHARACTER];
        ch |= igDragFloat(PL(TR("Max Walk Speed")), &cc->max_walk_speed, 5, 0, 5000, "%.0f cm/s", 0);
        ch |= igDragFloat(PL(TR("Jump Z Velocity")), &cc->jump_z_velocity, 5, 0, 5000, "%.0f cm/s", 0);
        ch |= igDragFloat(PL(TR("Gravity Scale")), &cc->gravity_scale, 0.01f, 0, 10, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Capsule Radius")), &cc->radius, 0.5f, 5, 500, "%.0f cm", 0);
        ch |= igDragFloat(PL(TR("Capsule Half Height")), &cc->half_height, 0.5f, 5, 500, "%.0f cm", 0);
        ch |= igDragFloat(PL(TR("Camera Distance")), &cc->camera_distance, 5, 0, 3000, "%.0f cm", 0);
        bool ap = cc->auto_possess != 0;
        if (igCheckbox(PL(TR("Auto Possess Player")), &ap)) cc->auto_possess = ap, ch = true;
        bool cy = cc->use_controller_yaw != 0; // (Q12)
        if (igCheckbox(PL(TR("Use Controller Rotation Yaw")), &cy)) cc->use_controller_yaw = cy, ch = true;
        break;
    }
    case COMP_LIGHT: {
        CLight* l = (CLight*)a->comp_data[COMP_LIGHT];
        if (igBeginCombo(PL(TR("Type")), l->type < LIGHT_TYPE_COUNT ? g_light_names[l->type] : "?", 0)) {
            for (uint32_t k = LIGHT_POINT; k <= LIGHT_SPOT; k++)
                if (igSelectable_Bool(g_light_names[k], l->type == k, 0, (ImVec2){ 0, 0 })) l->type = k, ch = true;
            igEndCombo();
        }
        ch |= igColorEdit3(PL(TR("Light Color")), l->color, 0);
        ch |= igDragFloat(PL(TR("Intensity")), &l->intensity, 1, 0, 1e6f, "%.1f cd", 0);
        ch |= igDragFloat(PL(TR("Attenuation Radius")), &l->attenuation_radius, 10, 10, 1e5f, "%.0f cm", 0);
        break;
    }
    case COMP_ROTATING:
        ch |= igDragFloat3(PL(TR("Rotation Rate")), ((CRotating*)a->comp_data[COMP_ROTATING])->rate, 1, -3600, 3600, "%.0f", 0);
        break;
    case COMP_PROJECTILE: {
        CProjectile* q = (CProjectile*)a->comp_data[COMP_PROJECTILE];
        ch |= igDragFloat(PL(TR("Initial Speed")), &q->initial_speed, 10, 0, 1e6f, "%.0f cm/s", 0);
        ch |= igDragFloat(PL(TR("Max Speed")), &q->max_speed, 10, 0, 1e6f, "%.0f cm/s", 0);
        ch |= igDragFloat3(PL(TR("Velocity")), q->velocity, 0.01f, -1e5f, 1e5f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Projectile Gravity Scale")), &q->gravity_scale, 0.01f, -10, 10, "%.2f", 0);
        ch |= flag_box(PL(TR("Should Bounce")), &q->should_bounce);
        ch |= igSliderFloat(PL(TR("Bounciness")), &q->bounciness, 0, 1, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Friction")), &q->friction, 0, 1, "%.2f", 0);
        ch |= flag_box(PL(TR("Rotation Follows Velocity")), &q->rotation_follows_velocity);
        ch |= flag_box(PL(TR("Auto Activate")), &q->active);
        break;
    }
    case COMP_INVENTORY: ch |= inv_ui_inventory((CInventory*)a->comp_data[COMP_INVENTORY]); break; // (inventory_ui.c)
    case COMP_INTERACTABLE: ch |= ia_ui_interactable((CInteractable*)a->comp_data[COMP_INTERACTABLE]); break; // (interaction_ui.c)
    case COMP_INTERACTOR: ch |= ia_ui_interactor((CInteractor*)a->comp_data[COMP_INTERACTOR]); break;
    case COMP_DEVICE: ch |= ia_ui_device(NULL, ENTITY_NONE, (CDevice*)a->comp_data[COMP_DEVICE]); break;
    case COMP_DIALOGUE_SPEAKER: ch |= dlg_ui_speaker((CDialogueSpeaker*)a->comp_data[COMP_DIALOGUE_SPEAKER]); break; // (dialogue_editor.c)
    case COMP_QUEST_LOG: ch |= quest_ui_log((CQuestLog*)a->comp_data[COMP_QUEST_LOG]); break; // (quest_editor.c)
    case COMP_GAME_RULES: ch |= rules_ui_component((CGameRules*)a->comp_data[COMP_GAME_RULES]); break; // (rules_ui.c)
    case COMP_ABILITY_SYSTEM: ch |= abil_ui_system((CAbilitySystem*)a->comp_data[COMP_ABILITY_SYSTEM]); break; // (ability_ui.c)
    case COMP_HUD_LAYOUT: ch |= hud_ui_layout((CHudLayout*)a->comp_data[COMP_HUD_LAYOUT]); break; // (hud_widget.c)
    case COMP_FLOATING: {
        CFloating* f = (CFloating*)a->comp_data[COMP_FLOATING];
        ch |= igDragFloat(PL(TR("Max Speed")), &f->max_speed, 10, 0, 1e5f, "%.0f cm/s", 0);
        ch |= igDragFloat(PL(TR("Acceleration")), &f->acceleration, 10, 0, 1e6f, "%.0f cm/s2", 0);
        ch |= igDragFloat(PL(TR("Deceleration")), &f->deceleration, 10, 0, 1e6f, "%.0f cm/s2", 0);
        break;
    }
    case COMP_SAVEABLE: { // (save_progress.h: what the progress's saves keep of the class's actors)
        CSaveable* sv = (CSaveable*)a->comp_data[COMP_SAVEABLE];
        static const struct { uint32_t bit; const char* label; } sf[] = {
            { SAVEABLE_TRANSFORM, "Save Transform" }, { SAVEABLE_HEALTH, "Save Health" }, { SAVEABLE_INVENTORY, "Save Inventory" },
            { SAVEABLE_VARIABLES, "Save SaveGame Variables" }, { SAVEABLE_ALL_VARIABLES, "Save All Variables" },
            { SAVEABLE_DESTROYED, "Remember Destroyed" }, { SAVEABLE_HIDDEN, "Save Hidden In Game" } };
        for (size_t k = 0; k < sizeof sf / sizeof sf[0]; k++) {
            bool on = (sv->flags & sf[k].bit) != 0;
            if (igCheckbox(PL(TR(sf[k].label)), &on)) sv->flags = on ? (sv->flags | sf[k].bit) : (sv->flags & ~sf[k].bit), ch = true;
        }
        break;
    }
    case COMP_HEALTH: { // (health.h: the same fields as the level's Details)
        CHealth* h = (CHealth*)a->comp_data[COMP_HEALTH];
        ch |= igDragFloat(PL(TR("Max Health")), &h->max_health, 1, 0, 1e7f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Health")), &h->health, 1, 0, 1e7f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Max Shield")), &h->max_shield, 1, 0, 1e7f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Shield")), &h->shield, 1, 0, 1e7f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Regen Delay")), &h->regen_delay, 0.05f, 0, 600, "%.2f s", 0);
        ch |= igDragFloat(PL(TR("Regen Rate")), &h->regen_rate, 0.5f, 0, 1e6f, "%.1f /s", 0);
        ch |= igDragFloat(PL(TR("Shield Regen Rate")), &h->shield_regen_rate, 0.5f, 0, 1e6f, "%.1f /s", 0);
        static const char* const deaths[HEALTH_DEATH_COUNT] = { "Only the Event", "Destroy Actor", "Ragdoll" };
        const char* dn[HEALTH_DEATH_COUNT];
        for (int k = 0; k < HEALTH_DEATH_COUNT; k++) dn[k] = TR(deaths[k]);
        int d = (int)(h->death < HEALTH_DEATH_COUNT ? h->death : 0);
        if (igCombo_Str_arr(PL(TR("On Death")), &d, dn, HEALTH_DEATH_COUNT, -1)) h->death = (uint32_t)d, ch = true;
        if (h->death != HEALTH_DEATH_EVENT) ch |= igDragFloat(PL(TR("Destroy Delay")), &h->destroy_delay, 0.05f, 0, 600, "%.2f s", 0);
        static const struct { uint32_t bit; const char* label; } fl[] = { { HEALTH_INVINCIBLE, "Invincible" }, { HEALTH_NUMBERS, "Damage Numbers" }, { HEALTH_SHOW_BAR, "Show Health Bar" } };
        for (size_t k = 0; k < sizeof fl / sizeof fl[0]; k++) {
            bool on = (h->flags & fl[k].bit) != 0;
            if (igCheckbox(PL(TR(fl[k].label)), &on)) h->flags = on ? (h->flags | fl[k].bit) : (h->flags & ~fl[k].bit), ch = true;
        }
        for (int k = 0; k < HEALTH_MAX_MULT; k++) {
            igPushID_Int(k);
            igSetNextItemWidth(igGetContentRegionAvail().x * 0.55f);
            ch |= igInputTextWithHint("##mt", "Damage.Fire", h->mult[k].tag, HEALTH_TAG_LEN, 0, NULL, NULL);
            igSameLine(0, 4);
            ch |= igDragFloat(PL(TR("Damage Multiplier")), &h->mult[k].scale, 0.01f, 0, 100, "x%.2f", 0);
            igPopID();
        }
        break;
    }
    default: break;
    }
    return ch;
}

// --- The component tree (Unreal's Simple Construction Script, bp_components.c) ---

static UiIcon scs_icon(uint32_t kind, const uint8_t* data) {
    switch (kind) {
    case SCS_STATIC_MESH: {
        uint32_t m = ((const CStaticMesh*)data)->mesh;
        return m < SHAPE_COUNT ? (UiIcon)(ICON_CUBE + m) : ICON_CUBE;
    }
    case SCS_POINT_LIGHT: return ICON_BULB;
    case SCS_SPOT_LIGHT: return ICON_SPOT;
    case SCS_RECT_LIGHT: return ICON_RECT;
    case SCS_CAMERA: return ICON_CAMERA;
    case SCS_SPRING_ARM: return ICON_ANGLE;
    case SCS_BOX: return ICON_CUBE;
    case SCS_SPHERE: return ICON_SPHERE;
    case SCS_CAPSULE: return ICON_CAPSULE;
    case SCS_ARROW: return ICON_ARROW;
    case SCS_BLUEPRINT: return ICON_BLUEPRINT;
    case SCS_SKELETAL_MESH: return ICON_CHARACTER;
    case SCS_AUDIO: return ICON_SOUND;
    default: return ICON_COMPONENT;
    }
}

static void select_scs(Win* w, int i) {
    w->scs = i;
    w->details = i >= 0 ? DET_SCS : DET_DEFAULTS;
    w->giz.dragging = false;
}

static void add_scs(Win* w, uint32_t kind) {
    BpAsset* a = w->a;
    int parent = w->details == DET_SCS && w->scs >= 0 && w->scs < (int)a->nscs ? w->scs : BP_SCS_ROOT; // under the selection
    int i = bp_scs_add(a, kind, parent, NULL);
    if (i < 0) {
        set_status(w, true, TR("A class holds up to %s components."), "32");
        return;
    }
    select_scs(w, i);
    touch(w);
}

static bool mesh_props(CStaticMesh* m) {
    bool ch = false;
    if (igBeginCombo(PL(TR("Mesh")), m->mesh < SHAPE_COUNT ? g_shape_names[m->mesh] : "?", 0)) {
        for (uint32_t s = 0; s < SHAPE_COUNT; s++)
            if (asset_selectable_mesh(g_shape_names[s], m->mesh == s, render_mesh_id(s))) m->mesh = s, ch = true;
        igEndCombo();
    }
    ch |= igColorEdit3(PL(TR("Base Color")), m->base_color, 0);
    ch |= igSliderFloat(PL(TR("Metallic")), &m->metallic, 0, 1, "%.2f", 0);
    ch |= igSliderFloat(PL(TR("Roughness")), &m->roughness, 0, 1, "%.2f", 0);
    ch |= igColorEdit3(PL(TR("Emissive")), m->emissive, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    ch |= flag_box(PL(TR("Cast Shadow")), &m->cast_shadow);
    return ch;
}

static void details_scs(Win* w) {
    BpAsset* a = w->a;
    if (w->scs < 0 || w->scs >= (int)a->nscs) {
        w->details = DET_DEFAULTS;
        return;
    }
    BpScsNode* n = &a->scs[w->scs];
    bool ch = false;
    ui_icon_item(scs_icon(n->kind, n->data), igGetFontSize(), TH(TK_N14));
    igSameLine(0, 6);
    igSetNextItemWidth(-1);
    ch |= igInputText("##scs_name", n->name, sizeof n->name, 0, NULL, NULL);
    igTextDisabled("%s %s", TR(g_scs_names[n->kind]), TR("Component"));
    // the parent: the root or an earlier component (parents come first)
    const char* pname = n->parent >= 0 ? a->scs[n->parent].name : TR("Root");
    if (igBeginCombo(PL(TR("Parent")), pname, 0)) {
        if (igSelectable_Bool(TR("Root"), n->parent < 0, 0, (ImVec2){ 0, 0 })) n->parent = BP_SCS_ROOT, ch = true;
        for (int j = 0; j < w->scs; j++)
            if (igSelectable_Bool(a->scs[j].name, n->parent == j, 0, (ImVec2){ 0, 0 })) n->parent = (int16_t)j, ch = true;
        igEndCombo();
    }
    igSeparatorText(TR("Transform"));
    ch |= igDragFloat3(PL(TR("Location")), n->location, 0.5f, -1e6f, 1e6f, "%.1f", 0);
    ch |= igDragFloat3(PL(TR("Rotation")), n->rotation, 0.5f, -360.0f, 360.0f, "%.1f", 0);
    ch |= igDragFloat3(PL(TR("Scale")), n->scale, 0.01f, -1000.0f, 1000.0f, "%.3f", 0);
    igSeparatorText(TR(g_scs_names[n->kind]));
    switch (n->kind) {
    case SCS_STATIC_MESH: ch |= mesh_props((CStaticMesh*)n->data); break;
    case SCS_POINT_LIGHT: case SCS_SPOT_LIGHT: case SCS_RECT_LIGHT: {
        CLight* l = (CLight*)n->data;
        ch |= igColorEdit3(PL(TR("Light Color")), l->color, 0);
        ch |= igDragFloat(PL(TR("Intensity")), &l->intensity, 1, 0, 1e6f, "%.1f cd", 0);
        ch |= igDragFloat(PL(TR("Attenuation Radius")), &l->attenuation_radius, 10, 10, 1e5f, "%.0f cm", 0);
        if (n->kind == SCS_SPOT_LIGHT) {
            ch |= igSliderFloat(PL(TR("Inner Cone Angle")), &l->inner_cone, 0, 80, "%.1f", 0);
            ch |= igSliderFloat(PL(TR("Outer Cone Angle")), &l->outer_cone, 1, 80, "%.1f", 0);
        }
        if (n->kind == SCS_RECT_LIGHT) {
            ch |= igDragFloat(PL(TR("Source Width")), &l->source_width, 1, 1, 1e4f, "%.0f cm", 0);
            ch |= igDragFloat(PL(TR("Source Height")), &l->source_height, 1, 1, 1e4f, "%.0f cm", 0);
        }
        ch |= flag_box(PL(TR("Cast Shadows")), &l->cast_shadows);
        break;
    }
    case SCS_CAMERA: { // the Camera Actor's settings, the same here (Unreal's Camera Component)
        CCamera* cam = (CCamera*)n->data;
        ch |= igSliderFloat(PL(TR("Field of View")), &cam->fov_deg, 5, 170, "%.0f", 0);
        ch |= flag_box(PL(TR("Auto Activate for Player")), &cam->auto_activate);
        uint32_t cb = !cam->no_camera_blur;
        if (flag_box(PL(TR("Camera Motion Blur")), &cb)) cam->no_camera_blur = !cb, ch = true;
        ch |= editor_camera_post_ui(cam);
        ch |= fpcam_ui(cam); // (Q12, fpcam_ui.c)
        break;
    }
    case SCS_SPRING_ARM: {
        CSpringArm* s = (CSpringArm*)n->data;
        ch |= igDragFloat(PL(TR("Target Arm Length")), &s->target_arm_length, 1, 0, 1e5f, "%.0f cm", 0);
        ch |= igDragFloat3(PL(TR("Socket Offset")), s->socket_offset, 0.5f, -1e4f, 1e4f, "%.1f", 0);
        ch |= igDragFloat3(PL(TR("Target Offset")), s->target_offset, 0.5f, -1e4f, 1e4f, "%.1f", 0);
        ch |= flag_box(PL(TR("Use Pawn Control Rotation")), &s->use_pawn_control_rotation);
        ch |= flag_box(PL(TR("Do Collision Test")), &s->do_collision_test);
        ch |= igDragFloat(PL(TR("Probe Size")), &s->probe_size, 0.5f, 0, 200, "%.0f cm", 0);
        ch |= flag_box(PL(TR("Enable Camera Lag")), &s->enable_camera_lag);
        ch |= igDragFloat(PL(TR("Camera Lag Speed")), &s->camera_lag_speed, 0.1f, 0, 100, "%.1f", 0);
        break;
    }
    case SCS_BOX: case SCS_SPHERE: case SCS_CAPSULE: {
        CCollision* q = (CCollision*)n->data;
        if (n->kind == SCS_BOX) ch |= igDragFloat3(PL(TR("Box Extent")), q->extent, 0.5f, 0, 1e5f, "%.1f", 0);
        else if (n->kind == SCS_SPHERE) ch |= igDragFloat(PL(TR("Sphere Radius")), &q->extent[0], 0.5f, 0, 1e5f, "%.1f cm", 0);
        else {
            ch |= igDragFloat(PL(TR("Capsule Radius")), &q->extent[0], 0.5f, 0, 1e5f, "%.1f cm", 0);
            ch |= igDragFloat(PL(TR("Capsule Half Height")), &q->extent[1], 0.5f, 0, 1e5f, "%.1f cm", 0);
        }
        ch |= flag_box(PL(TR("Generate Overlap Events")), &q->generate_overlap_events);
        ch |= flag_box(PL(TR("Block All")), &q->block);
        break;
    }
    case SCS_SKELETAL_MESH: ch |= anim_component_details((CSkeletalMesh*)n->data); break;
    case SCS_AUDIO: ch |= audio_component_details((CAudio*)n->data); break;
    case SCS_BLUEPRINT: {
        const BpAsset* c = bp_find_hash(((const CBlueprint*)n->data)->asset_hash);
        igText("%s: %s", TR("Class"), c ? c->name : "?");
        break;
    }
    case SCS_ARROW: {
        CArrow* ar = (CArrow*)n->data;
        ch |= igColorEdit3(PL(TR("Arrow Color")), ar->color, 0);
        ch |= igDragFloat(PL(TR("Arrow Length")), &ar->length, 1, 1, 1e4f, "%.0f cm", 0);
        break;
    }
    default: break;
    }
    if (ch) touch(w);
}

static void remove_scs(Win* w, int i) {
    bp_scs_remove(w->a, i);
    select_scs(w, -1);
    touch(w);
}

static void add_actor_component(Win* w, ComponentId c);

// The actor's own components a class can add (Unreal's movement components and the root's mesh / light)
static const ComponentId g_addable[] = { COMP_STATIC_MESH, COMP_LIGHT, COMP_ROTATING, COMP_CHARACTER, COMP_PROJECTILE, COMP_FLOATING };

static void add_component_menu(Win* w) {
    BpAsset* a = w->a;
    // Unreal's + Add: the scene components, by group (they hang under the selected component)
    static const struct { const char* group; uint32_t kinds[4]; uint32_t n; } groups[] = {
        { "Common", { SCS_STATIC_MESH, SCS_SKELETAL_MESH, SCS_SCENE, SCS_ARROW }, 4 },
        { "Audio", { SCS_AUDIO }, 1 },
        { "Lights", { SCS_POINT_LIGHT, SCS_SPOT_LIGHT, SCS_RECT_LIGHT }, 3 },
        { "Camera", { SCS_CAMERA, SCS_SPRING_ARM }, 2 },
        { "Collision", { SCS_BOX, SCS_SPHERE, SCS_CAPSULE }, 3 },
    };
    for (size_t gi = 0; gi < sizeof groups / sizeof groups[0]; gi++) {
        igSeparatorText(TR(groups[gi].group));
        for (uint32_t k = 0; k < groups[gi].n; k++) {
            uint32_t kind = groups[gi].kinds[k];
            uint8_t blank[BP_SCS_DATA] = { 0 };
            if (ui_menu_item(scs_icon(kind, blank), TR(g_scs_names[kind]), NULL, false, a->nscs < BP_MAX_SCS)) add_scs(w, kind);
        }
    }
    // the project's Actor / Scene Component Blueprints
    bool sep = false;
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        BpAsset* c = bp_asset_at(i);
        BpNative nat = c->kind == BPA_CLASS ? bp_native_of(c) : BPN_ACTOR;
        if (c->kind != BPA_CLASS || (nat != BPN_ACTOR_COMPONENT && nat != BPN_SCENE_COMPONENT) || c == a) continue;
        if (!sep) igSeparatorText(TR("Blueprint Components")), sep = true;
        if (ui_menu_item(ICON_BLUEPRINT, c->name, NULL, false, a->nscs < BP_MAX_SCS)) {
            int parent = w->details == DET_SCS && w->scs >= 0 ? w->scs : BP_SCS_ROOT;
            int j = bp_scs_add(a, SCS_BLUEPRINT, parent, c->name);
            if (j >= 0) {
                ((CBlueprint*)a->scs[j].data)->asset_hash = c->hash;
                select_scs(w, j);
                touch(w);
            }
        }
    }
    igSeparatorText(TR("Movement"));
    for (size_t i = 0; i < sizeof g_addable / sizeof g_addable[0]; i++) {
        ComponentId c = g_addable[i];
        if (c == COMP_STATIC_MESH || c == COMP_LIGHT) continue; // (the root's: below)
        bool has = (a->comp_mask & COMP_BIT(c)) != 0;
        UiIcon icon = c == COMP_ROTATING ? ICON_UPDATE : ICON_MOVEMENT;
        if (!ui_menu_item(icon, TR(g_components[c].name), NULL, has, !has)) continue;
        add_actor_component(w, c);
    }
    if (plugins_gameplay_enabled()) { // (the Gameplay Framework plugin's components: Edit > Plugins)
    igSeparatorText(TR("Gameplay")); // (health.h: Unreal's Health Component of its games)
    bool has_health = (a->comp_mask & COMP_BIT(COMP_HEALTH)) != 0;
    if (ui_menu_item(ICON_MOVEMENT, TR(g_components[COMP_HEALTH].name), NULL, has_health, !has_health)) add_actor_component(w, COMP_HEALTH);
    { // (save_progress.h: the actor's place, Health, Inventory, SaveGame variables and destruction in the saves)
        bool has = (a->comp_mask & COMP_BIT(COMP_SAVEABLE)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("Saveable"), NULL, has, !has)) add_actor_component(w, COMP_SAVEABLE);
    }
    { // (inventory.h: the actor carries items)
        bool has = (a->comp_mask & COMP_BIT(COMP_INVENTORY)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("Inventory"), NULL, has, !has)) add_actor_component(w, COMP_INVENTORY);
    }
    static const int ia[] = { COMP_INTERACTABLE, COMP_INTERACTOR, COMP_DEVICE }; // (interaction.h: used, who uses, a door / chest / lever / button)
    for (int k = 0; k < 3; k++) {
        bool has = (a->comp_mask & COMP_BIT(ia[k])) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR(g_components[ia[k]].name), NULL, has, !has)) add_actor_component(w, (ComponentId)ia[k]);
    }
    { // (dialogue.h: the NPC one talks to)
        bool has = (a->comp_mask & COMP_BIT(COMP_DIALOGUE_SPEAKER)) != 0;
        if (ui_menu_item(ICON_GRAPH, TR("Dialogue Speaker"), NULL, has, !has)) add_actor_component(w, COMP_DIALOGUE_SPEAKER);
    }
    { // (quest.h: the actor's quests: the player's, or a Game State's)
        bool has = (a->comp_mask & COMP_BIT(COMP_QUEST_LOG)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("Quest Log"), NULL, has, !has)) add_actor_component(w, COMP_QUEST_LOG);
    }
    { // (game_rules.h: the match's rules, on a Game Mode class)
        bool has = (a->comp_mask & COMP_BIT(COMP_GAME_RULES)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("Game Rules"), NULL, has, !has)) add_actor_component(w, COMP_GAME_RULES);
    }
    { // (ability.h: abilities, attributes, Gameplay Effects)
        bool has = (a->comp_mask & COMP_BIT(COMP_ABILITY_SYSTEM)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("Ability System"), NULL, has, !has)) add_actor_component(w, COMP_ABILITY_SYSTEM);
    }
    { // (hud.h: the game's HUD, WBP_GameHUD, on the player)
        bool has = (a->comp_mask & COMP_BIT(COMP_HUD_LAYOUT)) != 0;
        if (ui_menu_item(ICON_COMPONENT, TR("HUD Layout"), NULL, has, !has)) add_actor_component(w, COMP_HUD_LAYOUT);
    }
    }
    igSeparatorText(TR("Root"));
    for (int c = COMP_STATIC_MESH; c <= COMP_LIGHT; c++) {
        if (c != COMP_STATIC_MESH && c != COMP_LIGHT) continue;
        bool has = (a->comp_mask & COMP_BIT(c)) != 0;
        char label[80];
        snprintf(label, sizeof label, "%s (%s)", TR(g_components[c].name), TR("Root"));
        if (ui_menu_item(c == COMP_STATIC_MESH ? ICON_CUBE : ICON_BULB, label, NULL, has, !has)) add_actor_component(w, (ComponentId)c);
    }
}

static void add_actor_component(Win* w, ComponentId c) {
    BpAsset* a = w->a;
    {
        a->comp_mask |= COMP_BIT(c);
        memset(a->comp_data[c], 0, sizeof a->comp_data[c]);
        if (c == COMP_LIGHT) {
            CLight* l = (CLight*)a->comp_data[c];
            l->type = LIGHT_POINT, l->color[0] = l->color[1] = l->color[2] = 1.0f, l->intensity = 800.0f;
            l->attenuation_radius = 1000.0f, l->outer_cone = 44.0f, l->inner_cone = 0.0f, l->cast_shadows = 1;
        } else if (c == COMP_STATIC_MESH) {
            CStaticMesh* m = (CStaticMesh*)a->comp_data[c];
            m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.7f, m->roughness = 0.5f, m->cast_shadow = 1;
        } else if (c == COMP_ROTATING) {
            ((CRotating*)a->comp_data[c])->rate[1] = 90.0f;
        } else if (c == COMP_CHARACTER) {
            CCharacter* cc = (CCharacter*)a->comp_data[c];
            cc->max_walk_speed = 600, cc->jump_z_velocity = 520, cc->gravity_scale = 1, cc->radius = 34, cc->half_height = 88;
            cc->camera_distance = 350, cc->auto_possess = 1;
        } else
            ecs_component_defaults(c, a->comp_data[c]);
        if (c == COMP_HEALTH) health_bar_asset(); // (the example widget, WBP_HealthBar, into Content/UI when missing)
        if (c == COMP_INTERACTABLE || c == COMP_INTERACTOR) interaction_prompt_asset(); // (WBP_InteractPrompt)
        if (c == COMP_DEVICE && !(a->comp_mask & COMP_BIT(COMP_INTERACTABLE))) { // (a device is used through its Interactable)
            a->comp_mask |= COMP_BIT(COMP_INTERACTABLE);
            memset(a->comp_data[COMP_INTERACTABLE], 0, sizeof a->comp_data[COMP_INTERACTABLE]);
            ecs_component_defaults(COMP_INTERACTABLE, a->comp_data[COMP_INTERACTABLE]);
        }
        if (c == COMP_DIALOGUE_SPEAKER) dlg_widget_asset(); // (WBP_Dialogue into Content/UI when missing)
        if (c == COMP_QUEST_LOG) quest_tracker_asset(); // (WBP_QuestTracker into Content/UI when missing)
        if (c == COMP_GAME_RULES) rules_hud_asset(), rules_end_asset(); // (WBP_MatchHUD, WBP_MatchEnd)
        if (c == COMP_ABILITY_SYSTEM) abil_bar_asset(); // (WBP_AbilityBar into Content/UI when missing)
        if (c == COMP_HUD_LAYOUT) hud_layout_asset(); // (WBP_GameHUD into Content/UI when missing)
        w->comp = (int)c;
        w->details = DET_COMPONENT;
        touch(w);
    }
}

static void remove_component(Win* w, int c) {
    w->a->comp_mask &= ~COMP_BIT(c);
    if (w->comp == c) w->comp = -1, w->details = DET_DEFAULTS;
    touch(w);
}

// One component in Details: its name with its icon, its properties
static void details_component(Win* w) {
    BpAsset* a = w->a;
    if (w->comp < 0 || !(a->comp_mask & COMP_BIT(w->comp))) {
        w->details = DET_DEFAULTS;
        return;
    }
    ui_icon_item(comp_icon(a, w->comp), igGetFontSize(), TH(TK_N14));
    igSameLine(0, 6);
    igText("%s", comp_label(a, w->comp));
    igTextDisabled("%s", TR(g_components[w->comp].name));
    igSeparator();
    if (component_props(a, w->comp)) touch(w);
}

// Class Defaults: every component's properties
static void details_defaults(Win* w) {
    BpAsset* a = w->a;
    static const int order[] = { COMP_TRANSFORM, COMP_STATIC_MESH, COMP_CHARACTER, COMP_LIGHT, COMP_ROTATING, COMP_PROJECTILE, COMP_FLOATING, COMP_HEALTH, COMP_SAVEABLE,
                                 COMP_INTERACTABLE, COMP_INTERACTOR, COMP_DEVICE, COMP_QUEST_LOG, COMP_GAME_RULES, COMP_ABILITY_SYSTEM, COMP_HUD_LAYOUT };
    igSeparatorText(TR("Class Defaults"));
    bool ch = false;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        int c = order[i];
        if (!(a->comp_mask & COMP_BIT(c))) continue;
        igPushID_Int(c);
        if (igCollapsingHeader_TreeNodeFlags(comp_label(a, c), ImGuiTreeNodeFlags_DefaultOpen)) ch |= component_props(a, c);
        igPopID();
    }
    if (ch) touch(w);
}

// --- The panels (Unreal's look: a dark band with the panel's name) ---

static void panel_header(const char* label) {
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float wdt = igGetContentRegionAvail().x, h = igGetFrameHeight();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + wdt, p.y + h }, TH(TK_N4), 3.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 8, p.y + (h - igGetFontSize()) * 0.5f }, TH(TK_N14), label, NULL);
    igDummy((ImVec2){ wdt, h });
}

// A row with an icon (Components, My Blueprint): true when clicked
// (2026-11-10) Expose on Spawn changed on a class: the Spawn Actor nodes of every loaded Blueprint take its pins again
static void spawn_nodes_refresh(void) {
    for (uint32_t k = 0; k < bp_asset_count(); k++) {
        BpAsset* o = bp_asset_at(k);
        for (uint32_t gi = 0; o && gi < o->ngraphs; gi++)
            for (uint32_t ni = 0; ni < o->graphs[gi].nnodes; ni++) {
                BpNode* n = &o->graphs[gi].nodes[ni];
                if (n->kind == BPK_SPAWN || n->kind == BPK_SPAWN_ASYNC) bp_refresh_pins(o, &o->graphs[gi], n);
            }
    }
}

static bool icon_row(const char* id, UiIcon icon, uint32_t icol, const char* label, const char* right, bool selected, float indent) {
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetTextLineHeight() + 6.0f;
    igSetNextItemAllowOverlap(); // (a value can be edited on the row: My Blueprint's variables)
    bool clicked = igSelectable_Bool(id, selected, ImGuiSelectableFlags_AllowDoubleClick, (ImVec2){ 0, h });
    ImDrawList* dl = igGetWindowDrawList();
    float x = p.x + 4.0f + indent, is = igGetFontSize();
    ui_icon(dl, icon, x, p.y + (h - is) * 0.5f, is, icol);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x + is + 6.0f, p.y + (h - igGetFontSize()) * 0.5f }, TH(TK_N14), label, NULL);
    if (right) {
        float rw = igCalcTextSize(right, NULL, false, -1).x;
        ImDrawList_AddText_Vec2(dl, (ImVec2){ igGetItemRectMax().x - rw - 6.0f, p.y + (h - igGetFontSize()) * 0.5f }, TH(TK_N12), right, NULL);
    }
    return clicked;
}

// Unreal's Components panel: + Add, the class itself (Self: its defaults), the root, the rest under it
static void components_panel(Win* w) {
    BpAsset* a = w->a;
    panel_header(TR("Components"));
    igPushStyleColor_U32(ImGuiCol_Button, TH(TK_GREEN_D));
    igPushStyleColor_U32(ImGuiCol_ButtonHovered, TH(TK_GREEN));
    if (ui_icon_button("bp_add_comp", ICON_PLUS, TR("Add"), TH(TK_N15), false, TR("Add a component"))) igOpenPopup_Str("add_comp", 0);
    igPopStyleColor(2);
    if (igBeginPopup("add_comp", 0)) {
        add_component_menu(w);
        igEndPopup();
    }
    char self[BP_NAME + 16];
    snprintf(self, sizeof self, "%s (%s)", a->name, TR("Self"));
    if (icon_row("##self", ICON_BLUEPRINT, TH(TK_BLUE), self, NULL, w->details == DET_DEFAULTS, 0.0f)) w->details = DET_DEFAULTS;
    static const int order[] = { COMP_TRANSFORM, COMP_STATIC_MESH, COMP_LIGHT, -1, COMP_CHARACTER, COMP_ROTATING, COMP_PROJECTILE, COMP_FLOATING, COMP_HEALTH, COMP_SAVEABLE,
                                 COMP_INTERACTABLE, COMP_INTERACTOR, COMP_DEVICE, COMP_QUEST_LOG, COMP_GAME_RULES, COMP_ABILITY_SYSTEM, COMP_HUD_LAYOUT };
    float step = igGetFontSize() + 4.0f;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        int c = order[i];
        if (c < 0) { // the component tree under the root, each node indented by its depth
            for (uint32_t j = 0; j < a->nscs; j++) {
                const BpScsNode* n = &a->scs[j];
                int depth = 2;
                for (int p = n->parent; p >= 0 && depth < 12; p = a->scs[p].parent) depth++;
                igPushID_Int(1000 + (int)j);
                uint32_t icol = n->kind >= SCS_POINT_LIGHT && n->kind <= SCS_RECT_LIGHT ? TH(TK_AMBER_L)
                                : n->kind >= SCS_BOX && n->kind <= SCS_CAPSULE ? TH(TK_ORANGE) : TH(TK_N14);
                if (icon_row("##scs", scs_icon(n->kind, n->data), icol, n->name, NULL, w->details == DET_SCS && w->scs == (int)j, step * (float)depth))
                    select_scs(w, (int)j);
                if (igBeginDragDropSource(0)) { // dropped on the graph: Get <component>
                    igSetDragDropPayload("RE_BP_COMP", n->name, BP_NAME, ImGuiCond_Once);
                    igText(TR("Get %s"), n->name);
                    igEndDragDropSource();
                }
                if (igBeginPopupContextItem("scs_menu", ImGuiPopupFlags_MouseButtonRight)) {
                    if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, true)) remove_scs(w, (int)j);
                    igEndPopup();
                }
                igPopID();
            }
            continue;
        }
        if (!(a->comp_mask & COMP_BIT(c))) continue;
        igPushID_Int(c);
        bool movement = c == COMP_CHARACTER || c == COMP_ROTATING || c == COMP_PROJECTILE || c == COMP_FLOATING || c == COMP_HEALTH || c == COMP_SAVEABLE ||
                        c == COMP_INTERACTABLE || c == COMP_INTERACTOR || c == COMP_DEVICE || c == COMP_QUEST_LOG || c == COMP_GAME_RULES || c == COMP_HUD_LAYOUT; // Unreal lists them apart
        float indent = c == COMP_TRANSFORM || movement ? step : step * 2.0f;
        uint32_t icol = c == COMP_LIGHT ? TH(TK_AMBER_L) : movement ? TH(TK_N13) : TH(TK_N14);
        if (icon_row("##comp", comp_icon(a, c), icol, comp_label(a, c), c == COMP_TRANSFORM ? TR("Root") : NULL,
                     w->details == DET_COMPONENT && w->comp == c, indent)) {
            w->comp = c;
            w->details = DET_COMPONENT;
        }
        if (c != COMP_TRANSFORM && igBeginPopupContextItem("comp_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, true)) remove_component(w, c);
            igEndPopup();
        }
        igPopID();
    }
}

// --- My Blueprint ---

// A section of My Blueprint (Unreal's GRAPHS, FUNCTIONS, VARIABLES...): open or closed, with its + on the right
static bool mybp_section(const char* label, const char* add_id, bool* add) {
    float right = igGetCursorPosX() + igGetContentRegionAvail().x;
    bool open = igCollapsingHeader_TreeNodeFlags(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    if (add_id) {
        igSameLine(right - igGetFrameHeight() + 2.0f, 0);
        igPushStyleColor_U32(ImGuiCol_Button, THEME_NONE);
        *add = ui_icon_button(add_id, ICON_PLUS, NULL, TH(TK_GREEN), false, NULL);
        igPopStyleColor(1);
    }
    return open;
}

static void open_graph(Win* w, uint32_t g) {
    w->graph = g;
    w->want_tab = 1 + (int)g;
    w->closed &= ~(1u << (2 + g)); // (a closed tab opens again)
}

// A variable deleted with its Get / Set nodes (and their wires) in every graph, as Unreal does
static void delete_var(Win* w, int vi) {
    BpAsset* a = w->a;
    if (vi < 0 || (uint32_t)vi >= a->nvars) return;
    char name[BP_NAME];
    snprintf(name, sizeof name, "%s", a->vars[vi].name);
    for (uint32_t g = 0; g < a->ngraphs; g++)
        for (uint32_t k = a->graphs[g].nnodes; k-- > 0;) {
            const BpNode* n = &a->graphs[g].nodes[k];
            if ((n->kind == BPK_VAR_GET || n->kind == BPK_VAR_SET) && !strcmp(n->text, name)) bp_remove_node(&a->graphs[g], n->id);
        }
    memmove(&a->vars[vi], &a->vars[vi + 1], sizeof(BpVar) * (a->nvars - (uint32_t)vi - 1));
    a->nvars--;
    w->nsel = 0;
    w->details = DET_CLASS;
    touch(w);
}

static void my_blueprint(Win* w) {
    BpAsset* a = w->a;
    panel_header(TR("My Blueprint"));
    bool add = false;
    if (mybp_section(TR("Graphs"), NULL, NULL)) {
        bool other = w->on_viewport || w->on_anim;
        if (anim_graph_of(a, false) || bp_is_child_of(a, "Anim Instance")) // an Animation Blueprint (Unreal's: AnimGraph next to the EventGraph)
            if (icon_row("##ag", ICON_GRAPH, TH(TK_AMBER_L), "AnimGraph", NULL, w->on_anim, 4.0f)) {
                w->closed &= ~2u; // (a closed tab opens again)
                w->want_tab = WANT_ANIM_GRAPH;
            }
        if (icon_row("##eg", ICON_GRAPH, TH(TK_N14), "EventGraph", NULL, !other && w->graph == 0, 4.0f)) open_graph(w, 0);
        for (uint32_t i = 1; i < a->ngraphs; i++) // the Construction Script (Unreal's: under Graphs)
            if (a->graphs[i].gkind == BPG_FUNCTION && !strcmp(a->graphs[i].name, "ConstructionScript")) {
                igPushID_Int(3000 + (int)i);
                if (icon_row("##cs", ICON_GRAPH, TH(TK_N14), "ConstructionScript", NULL, !other && w->graph == i, 4.0f)) open_graph(w, i);
                igPopID();
            }
    }
    bool fopen = mybp_section(TR("Functions"), "bp_add_fn", &add);
    if (add && a->ngraphs < 16) {
        char name[BP_NAME] = "NewFunction";
        for (int n = 2; n < 100; n++) {
            bool taken = false;
            for (uint32_t i = 1; i < a->ngraphs; i++) taken |= !strcmp(a->graphs[i].name, name);
            if (!taken) break;
            snprintf(name, sizeof name, "NewFunction%d", n);
        }
        bp_add_function(a, name);
        open_graph(w, a->ngraphs - 1);
        w->func = (int)w->graph;
        w->details = DET_FUNC;
        w->frame = true;
        touch(w);
    }
    for (uint32_t i = 1; fopen && i < a->ngraphs; i++) {
        if (a->graphs[i].gkind != BPG_FUNCTION || !strcmp(a->graphs[i].name, "ConstructionScript")) continue; // (under Graphs)
        igPushID_Int((int)i);
        if (icon_row("##fn", a->graphs[i].iface[0] ? ICON_INTERFACE : ICON_FUNCTION, a->graphs[i].iface[0] ? TH(TK_CYAN) : TH(TK_BLUE_L),
                     a->graphs[i].name, a->graphs[i].iface[0] ? a->graphs[i].iface : NULL, !w->on_viewport && !w->on_anim && w->graph == i, 4.0f)) {
            open_graph(w, i);
            w->func = (int)i;
            w->details = DET_FUNC;
        }
        if (!a->graphs[i].iface[0] && igBeginDragDropSource(0)) { // dropped on the graph: a call to it
            igSetDragDropPayload("RE_BP_FUNC", a->graphs[i].name, BP_NAME, ImGuiCond_Once);
            igText("%s", a->graphs[i].name);
            igEndDragDropSource();
        }
        if (igBeginPopupContextItem("fn_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (ui_menu_item(ICON_DELETE, TR("Delete"), NULL, false, true)) {
                BpGraph* g = &a->graphs[i];
                free(g->nodes);
                free(g->links);
                memmove(&a->graphs[i], &a->graphs[i + 1], sizeof(BpGraph) * (a->ngraphs - i - 1));
                a->ngraphs--;
                w->graph = 0;
                w->details = DET_CLASS;
                touch(w);
                igEndPopup();
                igPopID();
                break;
            }
            igEndPopup();
        }
        igPopID();
    }
    // macros (Unreal's: graphs whose nodes take a macro node's place when compiled)
    add = false;
    bool mopen = mybp_section(TR("Macros"), "bp_add_macro", &add);
    if (add && a->ngraphs < 16) {
        char name[BP_NAME] = "NewMacro";
        for (int n = 2; n < 100; n++) {
            bool taken = false;
            for (uint32_t i = 1; i < a->ngraphs; i++) taken |= !strcmp(a->graphs[i].name, name);
            if (!taken) break;
            snprintf(name, sizeof name, "NewMacro%d", n);
        }
        if (bp_add_graph(a, name, BPG_MACRO)) {
            open_graph(w, a->ngraphs - 1);
            w->func = (int)w->graph;
            w->details = DET_FUNC;
            w->frame = true;
            touch(w);
        }
    }
    for (uint32_t i = 1; mopen && i < a->ngraphs; i++) {
        if (a->graphs[i].gkind != BPG_MACRO) continue;
        igPushID_Int(1000 + (int)i);
        if (icon_row("##mc", ICON_GRAPH, TH(TK_N13), a->graphs[i].name, NULL, !w->on_viewport && !w->on_anim && w->graph == i, 4.0f)) {
            open_graph(w, i);
            w->func = (int)i;
            w->details = DET_FUNC;
        }
        if (igBeginDragDropSource(0)) { // dropped on the graph: a macro node
            igSetDragDropPayload("RE_BP_MACRO", a->graphs[i].name, BP_NAME, ImGuiCond_Once);
            igText("%s", a->graphs[i].name);
            igEndDragDropSource();
        }
        if (igBeginPopupContextItem("mc_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (ui_menu_item(ICON_DELETE, TR("Delete"), NULL, false, true)) {
                BpGraph* g = &a->graphs[i];
                free(g->nodes);
                free(g->links);
                memmove(&a->graphs[i], &a->graphs[i + 1], sizeof(BpGraph) * (a->ngraphs - i - 1));
                a->ngraphs--;
                w->graph = 0;
                w->details = DET_CLASS;
                touch(w);
                igEndPopup();
                igPopID();
                break;
            }
            igEndPopup();
        }
        igPopID();
    }
    add = false;
    bool dopen = mybp_section(TR("Event Dispatchers"), "bp_add_dispatcher", &add);
    if (add && a->ngraphs < 16) {
        char name[BP_NAME] = "NewEventDispatcher";
        for (int k = 2; k < 100; k++) {
            bool taken = false;
            for (uint32_t i = 1; i < a->ngraphs; i++) taken |= !strcmp(a->graphs[i].name, name);
            if (!taken) break;
            snprintf(name, sizeof name, "NewEventDispatcher%d", k);
        }
        if (bp_add_graph(a, name, BPG_DISPATCHER)) w->func = (int)a->ngraphs - 1, w->details = DET_FUNC, touch(w);
    }
    for (uint32_t i = 1; dopen && i < a->ngraphs; i++) {
        if (a->graphs[i].gkind != BPG_DISPATCHER) continue;
        igPushID_Int(2000 + (int)i);
        if (icon_row("##ed", ICON_EVENT, TH(TK_RED), a->graphs[i].name, NULL, w->details == DET_FUNC && w->func == (int)i, 4.0f))
            w->func = (int)i, w->details = DET_FUNC;
        if (igBeginPopupContextItem("ed_menu", ImGuiPopupFlags_MouseButtonRight)) { // its nodes into the open graph, or delete it
            static const uint16_t dk[5] = { BPK_DISPATCH_CALL, BPK_DISPATCH_BIND, BPK_DISPATCH_UNBIND, BPK_DISPATCH_UNBIND_ALL, BPK_EVENT_DISPATCH };
            static const char* const dl[5] = { "Call", "Bind", "Unbind", "Unbind all", "Event" };
            for (int k = 0; k < 5; k++)
                if (ui_menu_item(k == 4 ? ICON_EVENT : ICON_FUNCTION, TR(dl[k]), NULL, false, true)) {
                    BpGraph* g = &a->graphs[w->graph < a->ngraphs ? w->graph : 0];
                    bp_add_node(a, g, dk[k], -g->pan[0] + 200.0f, -g->pan[1] + 200.0f, a->graphs[i].name, NULL);
                    touch(w);
                }
            igSeparator();
            if (ui_menu_item(ICON_DELETE, TR("Delete"), NULL, false, true)) {
                memmove(&a->graphs[i], &a->graphs[i + 1], sizeof(BpGraph) * (a->ngraphs - i - 1));
                a->ngraphs--;
                w->details = DET_CLASS;
                touch(w);
                igEndPopup();
                igPopID();
                break;
            }
            igEndPopup();
        }
        igPopID();
    }
    add = false;
    bool vopen = mybp_section(TR("Variables"), "bp_add_var", &add);
    if (add && a->nvars < 64) {
        BpVar* v = &a->vars[a->nvars];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "NewVar");
        for (int n = 2; n < 100 && bp_var_index(a, v->name) >= 0 && bp_var_index(a, v->name) != (int)a->nvars; n++)
            snprintf(v->name, sizeof v->name, "NewVar%d", n);
        v->type = BPT_BOOL;
        w->var = (int)a->nvars++;
        w->details = DET_VAR;
        touch(w);
    }
    int del_var = -1;
    for (uint32_t i = 0; vopen && i < a->nvars; i++) {
        BpVar* v = &a->vars[i];
        igPushID_Int((int)i);
        // the default value edited on the row itself (simple types); the rest show their type
        bool inline_edit = !v->cont && v->type != BPT_STRUCT && v->type != BPT_TRANSFORM && v->type != BPT_ACTOR && v->type != BPT_OBJECT &&
                           v->type != BPT_ENUM;
        char tl[BP_NAME * 2 + 32];
        type_label(v->type, v->ref, v->cont, v->vtype, tl, sizeof tl);
        float row_x = igGetCursorPosX(), row_w = igGetContentRegionAvail().x;
        // (2026-11-10, Unreal's My Blueprint) the row's right: the type as a colored pill (a click changes the type there)
        // and the eye (Instance Editable); the value edited on the row only when there is room for it too
        float fs = igGetFontSize(), eye_w = fs * 1.5f, pill_w = fminf(fmaxf(igCalcTextSize(tl, NULL, false, -1).x + fs, fs * 4.0f), row_w * 0.42f);
        inline_edit = inline_edit && row_w > fs * 22.0f;
        if (icon_row("##var", ICON_VARIABLE, type_color(v->type), v->name, NULL, w->details == DET_VAR && w->var == (int)i, 4.0f)) {
            w->var = (int)i;
            w->details = DET_VAR;
        }
        ImVec2 rmin0 = igGetItemRectMin(), rmax0 = igGetItemRectMax();
        if (igIsItemHovered(0)) igSetItemTooltip("%s%s%s", tl, v->tooltip[0] ? "\n" : "", v->tooltip);
        // the row's own drag and menu, before the value edited on it (they attach to the last item: the value's field
        // took them, and a simple variable neither dragged into the graph nor showed Delete)
        if (igBeginDragDropSource(0)) {
            int idx = (int)i;
            igSetDragDropPayload("RE_BP_VAR", &idx, sizeof idx, ImGuiCond_Once);
            igText(TR("%s (Ctrl: Get, Alt: Set)"), v->name);
            igEndDragDropSource();
        }
        if (igBeginPopupContextItem("var_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (ui_menu_item(ICON_DELETE, TR("Delete"), "Delete", false, true)) del_var = (int)i;
            igEndPopup();
        }
        // Unreal's Delete on the selected variable (the panel focused, no text being typed)
        if (w->details == DET_VAR && w->var == (int)i && igIsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !igGetIO_Nil()->WantTextInput &&
            igIsKeyPressed_Bool(ImGuiKey_Delete, false))
            del_var = (int)i;
        { // the pill and the eye, over the row's right end
            ImDrawList* dl = igGetWindowDrawList();
            float h = rmax0.y - rmin0.y, x1 = rmax0.x - 4.0f;
            ImVec2 back = igGetCursorScreenPos();
            igSetCursorScreenPos((ImVec2){ x1 - eye_w, rmin0.y });
            if (igInvisibleButton("##eye", (ImVec2){ eye_w, h }, 0)) v->editable = !v->editable, touch(w);
            igSetItemTooltip("%s", v->editable ? TR("Instance Editable: shown in the actor's Details (click: private)")
                                               : TR("Private: not shown in the actor's Details (click: Instance Editable)"));
            ui_icon(dl, v->editable ? ICON_EYE : ICON_EYE_CLOSED, x1 - eye_w + (eye_w - fs) * 0.5f, rmin0.y + (h - fs) * 0.5f, fs,
                    v->editable ? TH(TK_AMBER_L) : TH(TK_N11));
            float px0 = x1 - eye_w - 4.0f - pill_w;
            igSetCursorScreenPos((ImVec2){ px0, rmin0.y + 2.0f });
            if (igInvisibleButton("##pill", (ImVec2){ pill_w, h - 4.0f }, 0)) igOpenPopup_Str("var_type_pick", 0);
            bool hov = igIsItemHovered(0);
            uint32_t tc = type_color(v->type);
            ImDrawList_AddRectFilled(dl, (ImVec2){ px0, rmin0.y + 2.0f }, (ImVec2){ px0 + pill_w, rmax0.y - 2.0f }, (tc & 0x00FFFFFFu) | (hov ? THA(TK_N0, 112) : THA(TK_N0, 72)), (h - 4.0f) * 0.5f, 0);
            ImDrawList_PushClipRect(dl, (ImVec2){ px0 + 4.0f, rmin0.y }, (ImVec2){ px0 + pill_w - 4.0f, rmax0.y }, true);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ px0 + fs * 0.5f, rmin0.y + (h - fs) * 0.5f }, TH(TK_N15), tl, NULL);
            ImDrawList_PopClipRect(dl);
            igSetItemTooltip("%s", TR("The variable's type: click to change it"));
            if (igBeginPopup("var_type_pick", 0)) {
                igTextDisabled("%s", TR("Variable Type"));
                if (type_picker("vtp", v, false, fs * 18.0f)) {
                    memset(&v->def, 0, sizeof v->def);
                    v->nadef = 0, v->sdef[0] = 0;
                    if (v->type == BPT_TRANSFORM) memset(v->def3, 0, sizeof v->def3), v->def3[1].v[0] = v->def3[1].v[1] = v->def3[1].v[2] = 1.0f;
                    if (v->type == BPT_COLOR) v->def.v[3] = 1.0f;
                    rename_refs(a, BPK_VAR_GET, BPK_VAR_SET, v->name, v->name); // (their pins take the new type)
                    touch(w);
                }
                igEndPopup();
            }
            if (inline_edit) {
                float vx = row_x + row_w * 0.45f, vw = px0 - (igGetWindowPos().x + vx) - 6.0f;
                if (vw > fs * 4.0f) {
                    igSetCursorPos((ImVec2){ vx, igGetCursorPosY() });
                    igSetCursorScreenPos((ImVec2){ igGetCursorScreenPos().x, rmin0.y + 1.0f });
                    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 4.0f, 1.0f });
                    igSetNextItemWidth(vw);
                    if (value_edit("##inl", v->type, v->ref, &v->def, v->sdef, sizeof v->sdef)) touch(w);
                    igPopStyleVar(1);
                }
            }
            igSetCursorScreenPos(back);
            igDummy((ImVec2){ 0, 0 });
        }
        igPopID();
    }
    if (del_var >= 0) delete_var(w, del_var);
    if (mybp_section(TR("Interfaces"), NULL, NULL)) {
        for (uint32_t i = 0; i < a->ninterfaces; i++) {
            igPushID_Int((int)i);
            if (icon_row("##if", ICON_INTERFACE, TH(TK_CYAN), a->interfaces[i], NULL, false, 4.0f)) w->details = DET_CLASS;
            // its functions: double click implements one (an event without outputs, a function graph with them)
            const BpAsset* ia = bp_find_name(BPA_INTERFACE, a->interfaces[i]);
            for (uint32_t f = 0; ia && f < ia->ngraphs; f++) {
                const BpGraph* fg = &ia->graphs[f];
                bool done = false;
                for (uint32_t k = 1; k < a->ngraphs; k++) done |= !strcmp(a->graphs[k].name, fg->name);
                for (uint32_t k = 0; !done && a->ngraphs && k < a->graphs[0].nnodes; k++)
                    done |= a->graphs[0].nodes[k].kind == BPK_EVENT_INTERFACE && !strcmp(a->graphs[0].nodes[k].text, fg->name);
                igPushID_Int((int)f);
                char lbl[BP_NAME + 32];
                snprintf(lbl, sizeof lbl, "%s%s", fg->name, done ? "" : TR("  (double click: implement)"));
                if (icon_row("##iff", fg->nresults ? ICON_FUNCTION : ICON_EVENT, done ? TH(TK_GREEN) : TH(TK_N12), lbl, NULL, false, 22.0f) &&
                    igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) && !done) {
                    if (fg->nresults && a->ngraphs < 16) { // a function graph with the interface's signature
                        BpGraph* g = bp_add_graph(a, fg->name, BPG_FUNCTION);
                        if (g) {
                            g->nparams = fg->nparams, g->nresults = fg->nresults;
                            memcpy(g->params, fg->params, sizeof g->params);
                            memcpy(g->results, fg->results, sizeof g->results);
                            snprintf(g->iface, sizeof g->iface, "%s", ia->name);
                            for (uint32_t k = 0; k < g->nnodes; k++) bp_refresh_pins(a, g, &g->nodes[k]);
                            open_graph(w, a->ngraphs - 1);
                            w->frame = true;
                        }
                    } else { // an event in the Event Graph
                        BpGraph* eg = &a->graphs[0];
                        float y = 0;
                        for (uint32_t k = 0; k < eg->nnodes; k++) y = fmaxf(y, eg->nodes[k].y + 200.0f);
                        BpNode* n = bp_add_node(a, eg, BPK_EVENT_INTERFACE, 0, y, fg->name, ia->name);
                        open_graph(w, 0);
                        if (n) w->nsel = 0, sel_add(w, n->id), w->details = DET_NODE, eg->pan[0] = 200, eg->pan[1] = 200 - y;
                    }
                    touch(w);
                }
                igPopID();
            }
            igPopID();
        }
        if (!a->ninterfaces) igTextDisabled("  %s", TR("Class Settings > Implement"));
    }
}

// A Macro Library's or an Interface's left panel: its graphs (macros / functions), + to add one
static void graphs_panel(Win* w) {
    BpAsset* a = w->a;
    bool lib = a->kind == BPA_MACROLIB;
    panel_header(TR("My Blueprint"));
    bool add = false;
    bool open = mybp_section(lib ? TR("Macros") : TR("Functions"), "bp_add_g", &add);
    if (add && a->ngraphs < 16) {
        char name[BP_NAME];
        snprintf(name, sizeof name, lib ? "NewMacro" : "NewFunction");
        for (int n = 2; n < 100; n++) {
            bool taken = false;
            for (uint32_t i = 0; i < a->ngraphs; i++) taken |= !strcmp(a->graphs[i].name, name);
            if (!taken) break;
            snprintf(name, sizeof name, lib ? "NewMacro%d" : "NewFunction%d", n);
        }
        if (bp_add_graph(a, name, lib ? BPG_MACRO : BPG_FUNCTION)) {
            w->graph = a->ngraphs - 1, w->func = (int)w->graph, w->details = DET_FUNC, w->frame = true;
            if (!lib) {
                a->nentries = 0;
                for (uint32_t i = 0; i < a->ngraphs && a->nentries < 32; i++) snprintf(a->entries[a->nentries++], BP_NAME, "%s", a->graphs[i].name);
            }
            touch(w);
        }
    }
    for (uint32_t i = 0; open && i < a->ngraphs; i++) {
        igPushID_Int((int)i);
        if (icon_row("##g", lib ? ICON_GRAPH : ICON_FUNCTION, lib ? TH(TK_N13) : TH(TK_CYAN), a->graphs[i].name, NULL, w->graph == i, 4.0f))
            w->graph = i, w->func = (int)i, w->details = DET_FUNC;
        if (igBeginPopupContextItem("g_menu", ImGuiPopupFlags_MouseButtonRight)) {
            if (a->ngraphs > 1 && ui_menu_item(ICON_DELETE, TR("Delete"), NULL, false, true)) {
                free(a->graphs[i].nodes), free(a->graphs[i].links);
                memmove(&a->graphs[i], &a->graphs[i + 1], sizeof(BpGraph) * (a->ngraphs - i - 1));
                a->ngraphs--;
                w->graph = 0, w->details = DET_CLASS;
                touch(w);
            }
            igEndPopup();
        }
        igPopID();
    }
}

// --- The Viewport tab: the class's look (bp_preview.c), orbited like Unreal's ---

static void viewport_tab(Win* w, ImVec2 size) {
    BpAsset* a = w->a;
    ImGuiIO* io = igGetIO_Nil();
    igBeginChild_Str("vp", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImVec2 o = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    if (anim_graph_of(a, false)) { // an Animation Blueprint: its Skeletal Mesh played by its graph (Unreal's ABP Viewport)
        ImVec2 av = igGetContentRegionAvail();
        anim_bp_preview(a, o.x, o.y, av.x, av.y, false);
        igEndChild();
        return;
    }
    igSetNextItemAllowOverlap();
    igInvisibleButton("vp_in", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    BpPreviewCam* c = &w->cam;
    if (!c->framed) bp_preview_frame(a, c);
    if (igIsItemActivated()) w->vp_clicked = !w->giz_held && igIsMouseDown_Nil(ImGuiMouseButton_Left);
    if (igIsItemActive() && !w->giz_held && !w->giz.dragging) {
        ImVec2 d = io->MouseDelta;
        if (igIsMouseDown_Nil(ImGuiMouseButton_Left) || igIsMouseDown_Nil(ImGuiMouseButton_Right)) { // orbit
            c->yaw += d.x * 0.35f;
            c->pitch = fminf(fmaxf(c->pitch - d.y * 0.35f, -89.0f), 89.0f);
        } else if (igIsMouseDown_Nil(ImGuiMouseButton_Middle)) { // pan
            float sy = sinf(c->yaw * 0.017453293f), cy = cosf(c->yaw * 0.017453293f), sp = sinf(c->pitch * 0.017453293f), cp = cosf(c->pitch * 0.017453293f);
            float k = c->dist * 0.0016f;
            c->target[0] -= -sy * d.x * k + (-sp * cy) * -d.y * k;
            c->target[1] -= cy * d.x * k + (-sp * sy) * -d.y * k;
            c->target[2] -= cp * -d.y * k;
        }
    }
    bool hovered = igIsItemHovered(0);
    if (hovered && io->MouseWheel != 0.0f) c->dist = fminf(fmaxf(c->dist * powf(0.88f, io->MouseWheel), 5.0f), 1e6f);
    if (hovered && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_F, false)) bp_preview_frame(a, c);
    // Unreal's keys: W move, E rotate, R scale, Delete the selected component
    if (hovered && !io->WantTextInput && !io->KeyCtrl) {
        if (igIsKeyPressed_Bool(ImGuiKey_W, false)) w->gizmo = 0;
        if (igIsKeyPressed_Bool(ImGuiKey_E, false)) w->gizmo = 1;
        if (igIsKeyPressed_Bool(ImGuiKey_R, false)) w->gizmo = 2;
        if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && w->details == DET_SCS && w->scs >= 0) remove_scs(w, w->scs);
    }
    // a click (not a drag, not on the gizmo) selects the component under the mouse
    if (w->vp_clicked && igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
        w->vp_clicked = false;
        ImVec2 dd = igGetMouseDragDelta(ImGuiMouseButton_Left, -1.0f);
        if (dd.x * dd.x + dd.y * dd.y < 9.0f) {
            int hit = bp_preview_pick(o.x, o.y, o.x + size.x, o.y + size.y, a, c, io->MousePos.x, io->MousePos.y);
            if (hit >= 0) select_scs(w, hit);
            else if (hit == BP_SCS_ROOT) {
                w->scs = -1;
                w->comp = COMP_TRANSFORM;
                w->details = DET_COMPONENT;
            } else select_scs(w, -1);
        }
    }
    unsigned flags = (w->vp_grid ? BP_PREVIEW_GRID : 0) | (w->vp_helpers ? BP_PREVIEW_HELPERS : 0);
    int sel = w->details == DET_SCS && w->scs >= 0 ? w->scs : w->details == DET_COMPONENT ? BP_SCS_ROOT : BP_PREVIEW_NONE;
    BpPreviewGpu gp = { BP_PV_KEY_BP_VIEWPORT + (unsigned)(w - g_win), w->vp_mode, (float)igGetTime() };
    bool drawn = bp_preview_draw_gpu(dl, o.x, o.y, o.x + size.x, o.y + size.y, a, c, flags, sel, &gp);
    w->giz_held = false;
    if (drawn && w->details == DET_SCS && w->scs >= 0) {
        bool moved = false;
        ImDrawList_PushClipRect(dl, o, (ImVec2){ o.x + size.x, o.y + size.y }, true);
        w->giz_held = bp_preview_gizmo(dl, o.x, o.y, o.x + size.x, o.y + size.y, a, c, w->scs, w->gizmo, &w->giz, &moved);
        ImDrawList_PopClipRect(dl);
        if (moved) touch(w);
    }
    if (!drawn) {
        ImDrawList_AddRectFilledMultiColor(dl, o, (ImVec2){ o.x + size.x, o.y + size.y }, TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_BOTTOM), TH(TK_PREVIEW_BOTTOM));
        const char* msg = TR("This class has no components to show. Add one in Components.");
        ImVec2 ts = igCalcTextSize(msg, NULL, false, -1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + (size.x - ts.x) * 0.5f, o.y + size.y * 0.5f }, TH(TK_N12), msg, NULL);
    }
    // the viewport's bar (Unreal's: the view on the left, what it shows)
    igSetCursorScreenPos((ImVec2){ o.x + 8, o.y + 8 });
    ui_icon_button("vp_persp", ICON_PERSPECTIVE, TR("Perspective"), TH(TK_N14), false, TR("Orbit: drag | Pan: middle button | Zoom: wheel | F: frame"));
    igSameLine(0, 4);
    bp_preview_mode_button("vp_mode", &w->vp_mode);
    igSameLine(0, 4);
    if (ui_icon_button("vp_grid", ICON_GRID, NULL, TH(TK_N14), w->vp_grid, TR("Grid"))) w->vp_grid = !w->vp_grid;
    igSameLine(0, 4);
    if (ui_icon_button("vp_help", ICON_CAPSULE, NULL, TH(TK_N14), w->vp_helpers, TR("Show the capsule, arrow and camera"))) w->vp_helpers = !w->vp_helpers;
    igSameLine(0, 4);
    if (ui_icon_button("vp_frame", ICON_FOCUS, NULL, TH(TK_N14), false, TR("Frame (F)"))) bp_preview_frame(a, c);
    igSameLine(0, 14);
    if (ui_icon_button("vp_move", ICON_MOVE, NULL, TH(TK_N14), w->gizmo == 0, TR("Move (W)"))) w->gizmo = 0;
    igSameLine(0, 2);
    if (ui_icon_button("vp_rot", ICON_ROTATE, NULL, TH(TK_N14), w->gizmo == 1, TR("Rotate (E)"))) w->gizmo = 1;
    igSameLine(0, 2);
    if (ui_icon_button("vp_scale", ICON_SCALE, NULL, TH(TK_N14), w->gizmo == 2, TR("Scale (R) | Ctrl: snap"))) w->gizmo = 2;
    // the class at the bottom left, as Unreal names what the viewport shows
    char cls[BP_NAME * 2 + 8];
    snprintf(cls, sizeof cls, "%s  (%s)", a->name, a->parent);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 12, o.y + size.y - igGetFontSize() - 10 }, THA(TK_N14, 192), cls, NULL);
    igEndChild();
}

// --- The other assets: Unreal's Structure, Enumeration and Data Asset editors ---

// A member's buttons at the end of its row (Unreal's): move up, move down, delete. -1 up, 1 down, 2 delete, 0 none
static int member_buttons(uint32_t i, uint32_t n) {
    int r = 0;
    igBeginDisabled(i == 0);
    if (igArrowButton("##move_up", ImGuiDir_Up)) r = -1;
    igEndDisabled();
    igSameLine(0, 2);
    igBeginDisabled(i + 1 >= n);
    if (igArrowButton("##move_down", ImGuiDir_Down)) r = 1;
    igEndDisabled();
    igSameLine(0, 2);
    if (ui_icon_button("member_delete", ICON_DELETE, NULL, TH(TK_N13), false, NULL)) r = 2;
    return r;
}
static float member_buttons_width(void) { return igGetFrameHeight() * 3.0f + 6.0f; }

// A pin's dot in its type's color before a type picker (Unreal's variable type icon)
static void type_dot(uint8_t type) {
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFrameHeight();
    ImDrawList_AddCircleFilled(igGetWindowDrawList(), (ImVec2){ p.x + h * 0.3f, p.y + h * 0.5f }, h * 0.2f, type_color(type), 12);
    igDummy((ImVec2){ h * 0.6f, h });
    igSameLine(0, 2);
}

// The editor's header line: the description box and, on its right, the add button (Unreal's "+ Add Variable")
static bool header_add(Win* w, const char* add_id, const char* add_label, bool can_add) {
    BpAsset* a = w->a;
    float bw = igGetFrameHeight() * 1.3f + igCalcTextSize(add_label, NULL, false, -1).x + 12.0f;
    igSetNextItemWidth(igGetContentRegionAvail().x - bw - 8.0f);
    if (igInputTextWithHint("##description", TR("Description"), a->description, sizeof a->description, 0, NULL, NULL)) touch(w);
    igSameLine(0, 8);
    igBeginDisabled(!can_add);
    bool add = ui_icon_button(add_id, ICON_PLUS, add_label, TH(TK_GREEN), false, NULL);
    igEndDisabled();
    return add;
}

static bool members_table(const char* id, int cols) {
    return igBeginTable(id, cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY,
                        (ImVec2){ 0, igGetContentRegionAvail().y }, 0);
}

// Unreal's Structure editor: the Structure tab (the members: name, type and container, order, delete; opened, a
// member's tooltip) and the Default Values tab (what each field of a new variable of it starts with)
static void struct_editor(Win* w) {
    BpAsset* a = w->a;
    if (!igBeginTabBar("##struct_tabs", 0)) return;
    if (igBeginTabItem(TR("Structure"), NULL, 0)) {
        igSpacing();
        if (header_add(w, "struct_add_variable", TR("Add Variable"), a->nfields < BP_MAX_FIELDS)) {
            BpVar* v = &a->fields[a->nfields++];
            memset(v, 0, sizeof *v);
            snprintf(v->name, sizeof v->name, "MemberVar_%u", a->nfields - 1);
            v->type = BPT_BOOL;
            v->def3[1].v[0] = v->def3[1].v[1] = v->def3[1].v[2] = 1.0f;
            touch(w);
        }
        igSpacing();
        if (members_table("##members", 3)) {
            igTableSetupScrollFreeze(0, 1);
            igTableSetupColumn(TR("Variable Name"), ImGuiTableColumnFlags_WidthStretch, 0.42f, 0);
            igTableSetupColumn(TR("Variable Type"), ImGuiTableColumnFlags_WidthStretch, 0.58f, 0);
            igTableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, member_buttons_width(), 0);
            igTableHeadersRow();
            for (uint32_t i = 0; i < a->nfields; i++) {
                BpVar* v = &a->fields[i];
                igPushID_Int((int)i);
                igTableNextRow(0, 0);
                igTableNextColumn();
                bool open = igTreeNodeEx_Str("##member", ImGuiTreeNodeFlags_NoTreePushOnOpen);
                igSameLine(0, 2);
                igSetNextItemWidth(-1);
                if (igInputText("##member_name", v->name, sizeof v->name, 0, NULL, NULL)) touch(w);
                igTableNextColumn();
                type_dot(v->type);
                if (type_picker("member_type", v, false, igGetContentRegionAvail().x)) touch(w);
                igTableNextColumn();
                int act = member_buttons(i, a->nfields);
                if (open) { // the member's settings under it (Unreal's opened row)
                    igTableNextRow(0, 0);
                    igTableNextColumn();
                    igIndent(igGetFrameHeight());
                    igAlignTextToFramePadding();
                    igTextDisabled("%s", TR("Tooltip"));
                    igUnindent(igGetFrameHeight());
                    igTableNextColumn();
                    igSetNextItemWidth(-1);
                    if (igInputText("##member_tooltip", v->tooltip, sizeof v->tooltip, 0, NULL, NULL)) touch(w);
                }
                igPopID();
                if (act == -1 || act == 1) {
                    BpVar t = a->fields[i];
                    a->fields[i] = a->fields[i + act];
                    a->fields[i + act] = t;
                    touch(w);
                } else if (act == 2) {
                    memmove(&a->fields[i], &a->fields[i + 1], sizeof(BpVar) * (a->nfields - i - 1));
                    a->nfields--;
                    touch(w);
                    break;
                }
            }
            igEndTable();
        }
        igEndTabItem();
    }
    if (igBeginTabItem(TR("Default Values"), NULL, 0)) {
        igSpacing();
        if (members_table("##defaults", 2)) {
            igTableSetupColumn(TR("Name"), ImGuiTableColumnFlags_WidthStretch, 0.4f, 0);
            igTableSetupColumn(TR("Value"), ImGuiTableColumnFlags_WidthStretch, 0.6f, 0);
            for (uint32_t i = 0; i < a->nfields; i++) {
                BpVar* v = &a->fields[i];
                igPushID_Int((int)i);
                igTableNextRow(0, 0);
                igTableNextColumn();
                igAlignTextToFramePadding();
                igTextUnformatted(v->name, NULL);
                if (v->tooltip[0]) igSetItemTooltip("%s", v->tooltip);
                igTableNextColumn();
                igSetNextItemWidth(-1);
                if (v->cont) igTextDisabled("%s", TR("(empty)"));
                else if (v->type == BPT_TRANSFORM) {
                    bool ch = igDragFloat3("##location", v->def.v, 0.5f, 0, 0, "%.4g", 0);
                    igSetNextItemWidth(-1);
                    ch |= igDragFloat3("##rotation", v->def3[0].v, 0.5f, 0, 0, "%.4g", 0);
                    igSetNextItemWidth(-1);
                    ch |= igDragFloat3("##scale", v->def3[1].v, 0.01f, 0, 0, "%.4g", 0);
                    if (ch) touch(w);
                } else if (value_edit("##default_value", v->type, v->ref, &v->def, v->sdef, sizeof v->sdef)) touch(w);
                igPopID();
            }
            igEndTable();
        }
        igEndTabItem();
    }
    igEndTabBar();
}

// Unreal's Enumeration editor: the description, then the Enumerators (Display Name, Description, order, delete). The
// order is each entry's value (Switch on it, a variable of it).
static void enum_editor(Win* w) {
    BpAsset* a = w->a;
    igSpacing();
    if (header_add(w, "enum_add_enumerator", TR("Add Enumerator"), a->nentries < 32)) {
        snprintf(a->entries[a->nentries], BP_NAME, "NewEnumerator%u", a->nentries);
        a->entry_desc[a->nentries][0] = 0;
        a->nentries++;
        touch(w);
    }
    igSpacing();
    if (!members_table("##enumerators", 4)) return;
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 1.6f, 0);
    igTableSetupColumn(TR("Display Name"), ImGuiTableColumnFlags_WidthStretch, 0.4f, 0);
    igTableSetupColumn(TR("Description"), ImGuiTableColumnFlags_WidthStretch, 0.6f, 0);
    igTableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, member_buttons_width(), 0);
    igTableHeadersRow();
    for (uint32_t i = 0; i < a->nentries; i++) {
        igPushID_Int((int)i);
        igTableNextRow(0, 0);
        igTableNextColumn();
        igAlignTextToFramePadding();
        igTextDisabled("%u", i);
        igTableNextColumn();
        igSetNextItemWidth(-1);
        if (igInputText("##display_name", a->entries[i], BP_NAME, 0, NULL, NULL)) touch(w);
        igTableNextColumn();
        igSetNextItemWidth(-1);
        if (igInputText("##entry_description", a->entry_desc[i], sizeof a->entry_desc[i], 0, NULL, NULL)) touch(w);
        igTableNextColumn();
        int act = member_buttons(i, a->nentries);
        igPopID();
        if (act == -1 || act == 1) {
            char t[BP_NAME], d[64];
            memcpy(t, a->entries[i], sizeof t), memcpy(d, a->entry_desc[i], sizeof d);
            memcpy(a->entries[i], a->entries[i + act], sizeof t), memcpy(a->entry_desc[i], a->entry_desc[i + act], sizeof d);
            memcpy(a->entries[i + act], t, sizeof t), memcpy(a->entry_desc[i + act], d, sizeof d);
            touch(w);
        } else if (act == 2) {
            memmove(a->entries[i], a->entries[i + 1], BP_NAME * (size_t)(a->nentries - i - 1));
            memmove(a->entry_desc[i], a->entry_desc[i + 1], sizeof a->entry_desc[0] * (size_t)(a->nentries - i - 1));
            a->nentries--;
            touch(w);
            break;
        }
    }
    igEndTable();
}

// A Data Asset: its values as Unreal's Details show a Primary Data Asset (name, type, value), + Add Variable
static void data_editor(Win* w) {
    BpAsset* a = w->a;
    igSpacing();
    if (header_add(w, "data_add_variable", TR("Add Variable"), a->nvars < 64)) {
        BpVar* v = &a->vars[a->nvars++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "Value%u", a->nvars);
        v->type = BPT_FLOAT;
        touch(w);
    }
    igSpacing();
    if (!members_table("##values", 4)) return;
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn(TR("Name"), ImGuiTableColumnFlags_WidthStretch, 0.3f, 0);
    igTableSetupColumn(TR("Type"), ImGuiTableColumnFlags_WidthStretch, 0.3f, 0);
    igTableSetupColumn(TR("Value"), ImGuiTableColumnFlags_WidthStretch, 0.4f, 0);
    igTableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, member_buttons_width(), 0);
    igTableHeadersRow();
    for (uint32_t i = 0; i < a->nvars; i++) {
        BpVar* v = &a->vars[i];
        igPushID_Int((int)i);
        igTableNextRow(0, 0);
        igTableNextColumn();
        igSetNextItemWidth(-1);
        if (igInputText("##member_name", v->name, sizeof v->name, 0, NULL, NULL)) touch(w);
        igTableNextColumn();
        type_dot(v->type);
        igSetNextItemWidth(-1);
        if (type_combo("##member_type", &v->type, v->ref)) touch(w);
        igTableNextColumn();
        igSetNextItemWidth(-1);
        if (value_edit("##default_value", v->type, v->ref, &v->def, v->sdef, sizeof v->sdef)) touch(w);
        igTableNextColumn();
        int act = member_buttons(i, a->nvars);
        igPopID();
        if (act == -1 || act == 1) {
            BpVar t = a->vars[i];
            a->vars[i] = a->vars[i + act];
            a->vars[i + act] = t;
            touch(w);
        } else if (act == 2) {
            memmove(&a->vars[i], &a->vars[i + 1], sizeof(BpVar) * (a->nvars - i - 1));
            a->nvars--;
            touch(w);
            break;
        }
    }
    igEndTable();
}

static void asset_form(Win* w) {
    switch (w->a->kind) {
    case BPA_STRUCT: struct_editor(w); break;
    case BPA_ENUM: enum_editor(w); break;
    case BPA_DATA: data_editor(w); break;
    default: break;
    }
}

// --- The window ---

// The asset's virtual path ("/Content/...") from its file
static bool asset_vpath(const BpAsset* a, char* out, size_t cap) {
    const char* root = content_root();
    size_t n = strlen(root);
    if (strncmp(a->path, root, n) != 0) return false;
    snprintf(out, cap, "/Content%s", a->path + n);
    for (char* c = out; *c; c++)
        if (*c == '\\') *c = '/';
    return true;
}

// Unreal's toolbar: Compile (with its state: a check, a question mark, a cross), Save, Browse | Class Settings, Class
// Defaults | Play; the parent class on the right
static void toolbar(Win* w, Editor* ed) {
    BpAsset* a = w->a;
    float right = igGetCursorPosX() + igGetContentRegionAvail().x;
    bool compiled_ok = a->compiled && !a->error[0];
    uint32_t ccol = a->error[0] ? TH(TK_RED) : compiled_ok ? TH(TK_GREEN) : TH(TK_AMBER);
    if (a->kind == BPA_CLASS) {
        if (ui_icon_button("bp_compile", ICON_BAKE, TR("Compile"), TH(TK_N14), false, TR("Compile the graphs (F7)"))) compile(w);
        // the state's badge on the icon's corner
        ImVec2 m = igGetItemRectMin();
        float h = igGetFrameHeight(), bs = h * 0.42f;
        ImVec2 bc = { m.x + h * 0.5f + h * 0.38f, m.y + h * 0.66f };
        ImDrawList* dl = igGetWindowDrawList();
        ImDrawList_AddCircleFilled(dl, bc, bs * 0.62f, ccol, 16);
        if (a->error[0]) ui_icon(dl, ICON_CROSS, bc.x - bs * 0.5f, bc.y - bs * 0.5f, bs, TH(TK_N15));
        else if (compiled_ok) ui_icon(dl, ICON_CHECK, bc.x - bs * 0.5f, bc.y - bs * 0.5f, bs, TH(TK_N15));
        else ImDrawList_AddText_Vec2(dl, (ImVec2){ bc.x - igCalcTextSize("?", NULL, false, -1).x * 0.5f, bc.y - igGetFontSize() * 0.5f }, TH(TK_N5), "?", NULL);
        igSameLine(0, 4);
    }
    if (ui_icon_button("bp_save", ICON_SAVE, TR("Save"), TH(TK_N14), false, TR("Save the asset (Ctrl+S)"))) save(w, ed);
    igSameLine(0, 4);
    if (ui_icon_button("bp_browse", ICON_SEARCH, TR("Browse"), TH(TK_N14), false, TR("Show it in the Content Browser"))) {
        char vp[700];
        if (asset_vpath(a, vp, sizeof vp)) {
            content_browse_to(vp);
            if (!ed->content_docked) ed->drawer_open = true;
        }
    }
    if (a->kind == BPA_CLASS) {
        igSameLine(0, 18);
        if (ui_icon_button("bp_settings", ICON_SETTINGS, TR("Class Settings"), TH(TK_N14), w->details == DET_CLASS, TR("Parent class and interfaces")))
            w->details = DET_CLASS;
        igSameLine(0, 4);
        if (ui_icon_button("bp_defaults", ICON_SLIDERS, TR("Class Defaults"), TH(TK_N14), w->details == DET_DEFAULTS, TR("The values its actors start with")))
            w->details = DET_DEFAULTS;
        igSameLine(0, 18);
        if (ui_icon_button("bp_play", ICON_GAMEPAD, TR("Play"), TH(TK_GREEN), false, TR("Play in Editor (Alt+P)")) && !ed->playing) {
            bp_editor_save_all(ed);
            editor_play(ed, igGetTime());
        }
        dbg_toolbar(w, ed); // (the debugger: the instance to debug, the steps while paused)
        // the tabs' menu (Unreal's Window menu of an asset editor): the closed tabs come back from it
        igSameLine(0, 8);
        if (ui_icon_button("bp_tabs_menu", ICON_MENU, NULL, TH(TK_N14), false, TR("Window: the tabs (closed ones open again)"))) igOpenPopup_Str("bp_tabs", 0);
        if (igBeginPopup("bp_tabs", 0)) {
            bool on = !(w->closed & 1u);
            if (ui_menu_item_ptr(ICON_VM_LIT, TR("Viewport"), NULL, &on, true)) w->closed = on ? w->closed & ~1u : w->closed | 1u, w->want_tab = on ? 0 : -1;
            if (anim_graph_of(a, false) || bp_is_child_of(a, "Anim Instance")) {
                on = !(w->closed & 2u);
                if (ui_menu_item_ptr(ICON_ANIM_BP, TR("AnimGraph"), NULL, &on, true)) w->closed = on ? w->closed & ~2u : w->closed | 2u;
            }
            for (uint32_t g = 0; g < a->ngraphs && g < 30; g++) {
                if (a->graphs[g].gkind == BPG_DISPATCHER || a->graphs[g].gkind == BPG_EVENT) continue;
                on = !(w->closed & (1u << (2 + g)));
                if (ui_menu_item_ptr(ICON_GRAPH, g == 0 ? TR("Event Graph") : a->graphs[g].name, NULL, &on, true)) {
                    if (on) open_graph(w, g);
                    else w->closed |= 1u << (2 + g);
                }
            }
            igSeparator();
            if (ui_menu_item(ICON_WINDOW, TR("Open all tabs"), NULL, false, true)) w->closed = 0;
            igEndPopup();
        }
        char par[BP_NAME + 40];
        snprintf(par, sizeof par, TR("Parent class: %s"), a->parent);
        float pw = igCalcTextSize(par, NULL, false, -1).x;
        igSameLine(right - pw - 8.0f, 0);
        igAlignTextToFramePadding();
        igTextDisabled("%s", par);
    } else {
        igSameLine(0, 14);
        igAlignTextToFramePadding();
        if (w->status[0]) igTextColored(w->status_bad ? THV(TK_ERROR) : THV(TK_OK), "%s", w->status);
        else {
            char vp[700];
            igTextDisabled("%s", asset_vpath(a, vp, sizeof vp) ? vp : a->path);
        }
    }
}

// Under the graph: Unreal's Compiler Results, one line
static void compiler_bar(Win* w) {
    BpAsset* a = w->a;
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float wd = igGetContentRegionAvail().x, h = igGetFrameHeight();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + wd, p.y + h }, TH(TK_N4), 3.0f, 0);
    float is = igGetFontSize();
    UiIcon icon = a->error[0] ? ICON_CROSS : a->compiled ? ICON_CHECK : ICON_INFO;
    uint32_t col = a->error[0] ? TH(TK_RED) : a->compiled ? TH(TK_GREEN) : TH(TK_AMBER);
    ui_icon(dl, icon, p.x + 6, p.y + (h - is) * 0.5f, is, col);
    char line[320];
    if (w->status[0]) snprintf(line, sizeof line, "%s", w->status);
    else if (a->error[0]) snprintf(line, sizeof line, TR("Error: %s"), a->error);
    else if (a->compiled) snprintf(line, sizeof line, TR("Compiled: %u ops, %u values per actor, %u Delays"), a->ncode, a->nslots, a->nlatent);
    else snprintf(line, sizeof line, "%s", TR("Not compiled yet (F7). Play compiles what changed."));
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + is + 14, p.y + (h - igGetFontSize()) * 0.5f },
                            w->status[0] && !w->status_bad ? TH(TK_GREEN_L) : a->error[0] || w->status_bad ? TH(TK_RED_L) : TH(TK_N13), line, NULL);
    igDummy((ImVec2){ wd, h });
}

// The tabs as windows in the editor's dock space (Unreal's): Viewport (a Widget Blueprint's Designer), the AnimGraph, the
// graphs. A tab dragged out floats (with multi-viewport: its own OS window, another monitor); x closes it (the Window
// menu or My Blueprint opens it again). The graph shown in the left and right panels: the one last focused
static bool tab_begin(Win* w, const char* label, const char* id, uint32_t bit, bool front) {
    char name[200];
    snprintf(name, sizeof name, "%s###%s%u", label, id, w->a->hash);
    igSetNextWindowDockID(w->dock, ImGuiCond_FirstUseEver);
    if (front) igSetNextWindowFocus();
    bool open = true;
    igPushStyleColor_U32(ImGuiCol_WindowBg, TH(TK_N3));
    bool shown = igBegin(name, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleColor(1);
    if (!open && bit < 32) w->closed |= 1u << bit;
    if (!shown) igEnd();
    return shown;
}

static void tab_windows(Win* w) {
    BpAsset* a = w->a;
    if (!w->dock || !w->open) return;
    w->on_viewport = false;
    if (!(w->closed & 1u)) {
        bool widget = bp_native_of(a) == BPN_USER_WIDGET;
        if (tab_begin(w, widget ? TR("Designer") : TR("Viewport"), "bpv", 0, w->want_tab == 0)) {
            ImVec2 av = igGetContentRegionAvail();
            if (widget) {
                if (ui_designer_tab(a, av.x, av.y)) touch(w);
            } else {
                w->on_viewport = true;
                viewport_tab(w, av);
            }
            igEnd();
        }
    }
    bool anim_vis = false, anim_foc = false, graph_foc = false;
    if ((anim_graph_of(a, false) || bp_is_child_of(a, "Anim Instance")) && !(w->closed & 2u)) {
        if (tab_begin(w, TR("AnimGraph"), "bpa", 1, w->want_tab == WANT_ANIM_GRAPH)) {
            ImVec2 av = igGetContentRegionAvail();
            if (anim_graph_tab(a, av.x, av.y)) touch(w);
            anim_vis = true;
            anim_foc = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || w->want_tab == WANT_ANIM_GRAPH;
            igEnd();
        }
    }
    uint32_t focused = w->graph;
    for (uint32_t g = 0; g < a->ngraphs; g++) {
        if (a->graphs[g].gkind == BPG_DISPATCHER || a->graphs[g].gkind == BPG_EVENT) continue;
        if (g < 30 && (w->closed & (1u << (2 + g)))) continue;
        char id[16];
        snprintf(id, sizeof id, "bpg%u_", g);
        if (!tab_begin(w, g == 0 ? TR("Event Graph") : !strcmp(a->graphs[g].name, "ConstructionScript") ? TR("Construction Script") : a->graphs[g].name, id, g < 30 ? 2 + g : 99, w->want_tab == 1 + (int)g)) continue;
        // (each graph window draws its own graph; the panels follow the focused one)
        uint32_t keep = w->graph;
        w->graph = g;
        draw_graph(w, igGetContentRegionAvail().y);
        if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || w->want_tab == 1 + (int)g) focused = g, graph_foc = true;
        else w->graph = keep;
        igEnd();
    }
    w->graph = focused < a->ngraphs ? focused : 0;
    w->on_anim = anim_foc || (!graph_foc && w->on_anim && anim_vis); // (My Blueprint's rows do not take it away)
    if (w->want_hold > 0) {
        w->want_hold--;
        return;
    }
    w->want_tab = -1;
}

// (2026-11-16) the Timeline editor of the node double-clicked: a change refreshes the node's pins (its tracks)
static void timeline_window(Win* w) {
    if (!w->tl_open || w->tl_graph >= w->a->ngraphs) return;
    BpGraph* g = &w->a->graphs[w->tl_graph];
    BpNode* n = bp_node(g, w->tl_node);
    if (!n || n->kind != BPK_TIMELINE) {
        w->tl_open = false;
        return;
    }
    if (bp_timeline_editor(w->a, n->text, &w->tl_open)) {
        bp_refresh_pins(w->a, g, n);
        w->a->compiled = false;
        touch(w);
    }
}

static void draw_window(Win* w, Editor* ed) {
    BpAsset* a = w->a;
    char title[160];
    snprintf(title, sizeof title, "%s - %s###bpwin%u", a->name, TR(g_bp_asset_names[a->kind]), a->hash); // (the tab: translated, "Graph Class")
    const ImGuiViewport* mv = igGetMainViewport(); // a class: most of the screen, centered (Unreal opens it as a tab)
    ImVec2 ws = a->kind == BPA_CLASS || a->kind == BPA_INTERFACE || a->kind == BPA_MACROLIB ? (ImVec2){ mv->Size.x * 0.8f, mv->Size.y * 0.8f }
                                                                              : (ImVec2){ fminf(980.0f, mv->Size.x * 0.6f), fminf(620.0f, mv->Size.y * 0.6f) };
    float vr[4]; // (Q26, the mockups) a class opens over the viewport, the level editor's panels around it, like a tab
    if (w->host) w->placed = 4; // (item 9: going into the Editors window, which takes that place; a SetNextWindowPos undocks)
    if ((a->kind == BPA_CLASS || a->kind == BPA_INTERFACE || a->kind == BPA_MACROLIB) && w->placed < 4 && dock_panel_rect(DP_VIEWPORT, vr) && vr[2] > 600.0f && vr[3] > 400.0f) {
        w->placed++; // (its first frames: the docked panels around the viewport take their places meanwhile)
        igSetNextWindowSize((ImVec2){ vr[2], vr[3] }, ImGuiCond_Always);
        igSetNextWindowPos((ImVec2){ vr[0], vr[1] }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    } else {
        igSetNextWindowSize(ws, ImGuiCond_FirstUseEver);
        igSetNextWindowPos((ImVec2){ mv->Pos.x + (mv->Size.x - ws.x) * 0.5f, mv->Pos.y + (mv->Size.y - ws.y) * 0.5f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    }
    edhost_doc(&w->host); // (item 9) a tab of the Editors window, beside the other editors (any kind)
    if (w->focus) igSetNextWindowFocus(), w->focus = false;
    bool open = true;
    igPushStyleColor_U32(ImGuiCol_WindowBg, TH(TK_N3));
    bool shown = igBegin(title, &open, ImGuiWindowFlags_NoCollapse | (a->dirty ? ImGuiWindowFlags_UnsavedDocument : 0));
    igPopStyleColor(1);
    if (!shown) {
        igEnd();
        if (!open) w->open = false;
        return;
    }
    if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_focused = true;
    toolbar(w, ed);
    bool keys = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !igGetIO_Nil()->WantTextInput;
    if (keys && igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiKey_S)) save(w, ed);
    if (keys && igIsKeyPressed_Bool(ImGuiKey_F7, false) && a->kind == BPA_CLASS) compile(w);
    if (keys && igIsKeyPressed_Bool(ImGuiKey_F9, false) && a->kind == BPA_CLASS) dbg_toggle_selected(w); // (a breakpoint on the selected nodes)
    g_dbg_trace = ed->playing && a->kind == BPA_CLASS; // (the debugger: the wires that ran, the pins' values)
    if (g_dbg_trace) bp_debug_watch(a, true);

    if (a->kind == BPA_INTERFACE || a->kind == BPA_MACROLIB) { // Unreal's: the graphs on the left, one open, its Details
        ImVec2 av = igGetContentRegionAvail();
        static float glw, grw; // (the bars between the panels drag them: Unreal's)
        float lw_def = fmaxf(220.0f, av.x * 0.2f), rw_def = fmaxf(300.0f, av.x * 0.26f);
        if (glw <= 0.0f) glw = lw_def;
        if (grw <= 0.0f) grw = rw_def;
        float lw = glw, rw = grw;
        igPushStyleColor_U32(ImGuiCol_ChildBg, TH(TK_N6));
        igBeginChild_Str("gl", (ImVec2){ lw, av.y }, ImGuiChildFlags_Borders, 0);
        graphs_panel(w);
        igEndChild();
        igPopStyleColor(1);
        igSameLine(0, 0);
        ui_splitter("##gl_split", true, &glw, 1.0f, 140.0f, av.x - rw - 240.0f, av.y, lw_def);
        igSameLine(0, 0);
        igBeginChild_Str("gm", (ImVec2){ av.x - lw - rw - 2.0f * UI_SPLIT, av.y }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (a->ngraphs) draw_graph(w, igGetContentRegionAvail().y);
        igEndChild();
        igSameLine(0, 0);
        ui_splitter("##gr_split", true, &grw, -1.0f, 180.0f, av.x - lw - 240.0f, av.y, rw_def);
        igSameLine(0, 0);
        igPushStyleColor_U32(ImGuiCol_ChildBg, TH(TK_N6));
        igBeginChild_Str("gd", (ImVec2){ rw, av.y }, ImGuiChildFlags_Borders, 0);
        panel_header(TR("Details"));
        igPushItemWidth(-120.0f);
        if (w->details == DET_NODE) details_node(w);
        else if (a->ngraphs) {
            if ((uint32_t)w->func >= a->ngraphs) w->func = (int)w->graph;
            w->details = DET_FUNC;
            details_func(w);
            if (a->kind == BPA_INTERFACE) { // (the names other code lists)
                a->nentries = 0;
                for (uint32_t i = 0; i < a->ngraphs && a->nentries < 32; i++) snprintf(a->entries[a->nentries++], BP_NAME, "%s", a->graphs[i].name);
            }
        }
        igPopItemWidth();
        igEndChild();
        igPopStyleColor(1);
        igEnd();
        if (!open) {
            if (a->dirty) save(w, ed);
            w->open = false;
        }
        return;
    }
    if (a->kind != BPA_CLASS) {
        asset_form(w);
        igEnd();
        if (!open) {
            if (a->dirty) save(w, ed);
            w->open = false;
        }
        return;
    }
    ImVec2 avail = igGetContentRegionAvail();
    // (2026-11-10) the columns' widths and the Components' height: the bars between them drag them (double-click: back)
    static float left_w, right_w, comp_hh;
    float left_def = fmaxf(240.0f, avail.x * 0.17f), right_def = fmaxf(300.0f, avail.x * 0.21f);
    if (left_w <= 0.0f) left_w = left_def;
    if (right_w <= 0.0f) right_w = right_def;
    left_w = fminf(left_w, fmaxf(avail.x - right_w - 300.0f, 160.0f));
    right_w = fminf(right_w, fmaxf(avail.x - left_w - 300.0f, 180.0f));
    float left = left_w, right = right_w;
    igPushStyleColor_U32(ImGuiCol_ChildBg, TH(TK_N6));
    // the left column: Components over My Blueprint
    igBeginChild_Str("left", (ImVec2){ left, avail.y }, 0, 0);
    // (a Widget Blueprint has no components, Unreal's way: its Designer's Hierarchy holds the widgets)
    if (bp_native_of(w->a) != BPN_USER_WIDGET) {
        float comp_def = fminf(fmaxf(avail.y * 0.3f, 150.0f), avail.y * 0.5f);
        if (comp_hh <= 0.0f) comp_hh = comp_def;
        float comp_h = fminf(fmaxf(comp_hh, 80.0f), avail.y - 120.0f);
        igBeginChild_Str("components", (ImVec2){ 0, comp_h }, ImGuiChildFlags_Borders, 0);
        components_panel(w);
        igEndChild();
        ui_splitter("##comp_split", false, &comp_hh, 1.0f, 80.0f, avail.y - 120.0f, left, comp_def);
    }
    igBeginChild_Str("mybp", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, 0);
    my_blueprint(w);
    igEndChild();
    igEndChild();
    igPopStyleColor(1);
    igSameLine(0, 0);
    ui_splitter("##left_split", true, &left_w, 1.0f, 160.0f, avail.x - right - 300.0f, avail.y, left_def);
    igSameLine(0, 0);
    // the middle: the tabs (Viewport, the Event Graph, the functions) and the compiler's line
    float mid = avail.x - left - right - 2.0f * UI_SPLIT;
    igBeginChild_Str("middle", (ImVec2){ mid, avail.y }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (w->want_tab == 0) w->closed &= ~1u;
    ImVec2 tb = igGetCursorScreenPos();
    igSetCursorScreenPos(tb);
    { // a dock space: each tab is a window docked in it (dragged out: a window of its own, another monitor; back: docked)
        float body = igGetContentRegionAvail().y - igGetFrameHeight() - igGetStyle()->ItemSpacing.y - 2.0f; // the compiler bar under it
        char dn[48];
        snprintf(dn, sizeof dn, "bpdock%u", a->hash);
        w->dock = igGetID_Str(dn);
        igDockSpace(w->dock, (ImVec2){ igGetContentRegionAvail().x, body }, ImGuiDockNodeFlags_None, NULL);
    }
    compiler_bar(w);
    igEndChild();
    igSameLine(0, 0);
    ui_splitter("##right_split", true, &right_w, -1.0f, 180.0f, avail.x - left - 300.0f, avail.y, right_def);
    igSameLine(0, 0);
    // the right: Details
    igPushStyleColor_U32(ImGuiCol_ChildBg, TH(TK_N6));
    igBeginChild_Str("details", (ImVec2){ right, avail.y }, ImGuiChildFlags_Borders, 0);
    panel_header(TR("Details"));
    igPushItemWidth(-120.0f);
    switch (w->details) {
    case DET_NODE: details_node(w); break;
    case DET_VAR: details_var(w); break;
    case DET_FUNC: details_func(w); break;
    case DET_COMPONENT: details_component(w); break;
    case DET_SCS: details_scs(w); break;
    default: break;
    }
    if (w->details == DET_DEFAULTS) details_defaults(w);
    if (w->details == DET_CLASS) details_settings(w);
    igPopItemWidth();
    igEndChild();
    igPopStyleColor(1);
    igEnd();
    tab_windows(w);
    timeline_window(w); // (2026-11-16) a Timeline node's editor
    if (!open) {
        if (a->dirty) save(w, ed);
        w->open = false;
    }
}

// The graph menu's test (--bp-menu-test): a Blueprint's Event Graph offers its events and the recommendations after
// Event BeginPlay; a function offers no event and nothing latent; a recommendation placed is noted. 0 = ok
int bp_editor_menu_selftest(const char* vpath) {
    BpAsset* a = bp_load(vpath);
    char made_dir[1024] = "";
    if (!a && !strncmp(vpath, "/Content/", 9) && strrchr(vpath, '/') > vpath + 8) { // (missing: an Actor class made here, removed after)
        char folder[600], name[128];
        const char* sl = strrchr(vpath, '/');
        snprintf(folder, sizeof folder, "%.*s", (int)(sl - vpath), vpath);
        snprintf(name, sizeof name, "%s", sl + 1);
        char* dot = strrchr(name, '.');
        if (dot) *dot = 0;
        snprintf(made_dir, sizeof made_dir, "%s%s", content_root(), folder + 8);
        re_make_dirs(made_dir);
        a = bp_create(BPA_CLASS, folder, name, "Actor");
    }
    if (!a || !a->ngraphs) return printf("[RealEngine] FAIL: bp-menu: %s not loaded\n", vpath), 1;
    static Win tw; // (a window's state without its window)
    memset(&tw, 0, sizeof tw);
    tw.a = a, tw.graph = 0, tw.from_pin = -1;
    int fails = 0;
    build_menu(&tw);
    uint32_t evs = 0;
    for (uint32_t i = 0; i < g_nmenu; i++) evs += (kind_class(a, g_menu[i].kind) & 2) != 0;
    bool ev = evs > 0, delay = entry_named("Delay") != NULL; // (BeginPlay and Tick only when missing: Unreal's)
    uint32_t n_event = g_nmenu;
    if (!ev || !delay) printf("[RealEngine] FAIL: bp-menu: the Event Graph lacks its events or Delay\n"), fails++;
    // the context: a BeginPlay selected; its recommendations are the graph's own
    BpGraph* g = &a->graphs[0];
    uint32_t begin = 0;
    for (uint32_t i = 0; i < g->nnodes && !begin; i++)
        if (g->nodes[i].kind == BPK_EVENT_BEGIN_PLAY) begin = g->nodes[i].id;
    if (!begin) {
        BpNode* n = bp_add_node(a, g, BPK_EVENT_BEGIN_PLAY, 0, 0, "", "");
        begin = n ? n->id : 0;
    }
    tw.sel[0] = begin, tw.nsel = begin ? 1 : 0;
    char ctx[96];
    menu_context(&tw, ctx, sizeof ctx);
    RecUser u = { &tw, { 0 }, 0 };
    const char* items[2];
    int n = gm_recommend(graph_kind(&tw), ctx, rec_accept, &u, items, 2);
    printf("[RealEngine] bp-menu: Event Graph %u actions, context \"%s\", recommended %s, %s\n", n_event, ctx, n > 0 ? items[0] : "-", n > 1 ? items[1] : "-");
    if (strcmp(ctx, "Event BeginPlay") || n < 2) printf("[RealEngine] FAIL: bp-menu: no recommendations after BeginPlay\n"), fails++;
    uint32_t before = g->nnodes;
    MenuEntry* e = n ? entry_named(items[0]) : NULL;
    if (e) place(&tw, e);
    if (g->nnodes != before + 1 || tw.last_placed == 0) printf("[RealEngine] FAIL: bp-menu: the recommendation wasn't placed\n"), fails++;
    const char* rec[1];
    if (!e || gm_recent(GM_BP_EVENT, rec, 1) != 1 || strcmp(rec[0], e->label)) printf("[RealEngine] FAIL: bp-menu: not the first recent\n"), fails++;
    // a function: no events, nothing latent (Unreal's rules)
    uint32_t fg = UINT32_MAX;
    for (uint32_t i = 1; i < a->ngraphs && fg == UINT32_MAX; i++)
        if (a->graphs[i].gkind == BPG_FUNCTION && strcmp(a->graphs[i].name, "ConstructionScript")) fg = i;
    if (fg == UINT32_MAX && bp_add_graph(a, "MenuTestFunction", BPG_FUNCTION)) fg = a->ngraphs - 1;
    if (fg != UINT32_MAX) {
        tw.graph = fg, tw.nsel = 0, tw.last_placed = 0;
        build_menu(&tw);
        uint32_t events = 0, latent = 0;
        for (uint32_t i = 0; i < g_nmenu; i++) {
            uint8_t c = kind_class(a, g_menu[i].kind);
            events += (c & 2) != 0, latent += (c & 4) != 0;
        }
        printf("[RealEngine] bp-menu: function %u actions (%u events, %u latent)\n", g_nmenu, events, latent);
        if (events || latent || entry_named("Delay") || g_nmenu >= n_event) printf("[RealEngine] FAIL: bp-menu: the function offers events or latent nodes\n"), fails++;
    } else
        printf("[RealEngine] FAIL: bp-menu: no function graph\n"), fails++;
    a->dirty = false; // (nothing saved)
    printf("[RealEngine] bp-menu test: %s\n", fails ? "FAIL" : "ok");
    if (made_dir[0]) re_remove_tree(made_dir);
    return fails;
}


// --- The Blueprint debugger (bp_debug.h; Unreal's Blueprint Debugger): the graph's marks, the toolbar, the window ---

typedef struct DbgWatch { BpAsset* owner; uint32_t node; uint16_t graph; uint8_t pin; } DbgWatch;
static DbgWatch g_watch[32];
static uint32_t g_nwatch;
static bool g_dbg_open;     // the Blueprint Debugger window
static bool g_dbg_was_paused, g_dbg_recapture;

static int watch_index(const BpAsset* a, uint32_t graph, uint32_t node, int pin) {
    for (uint32_t i = 0; i < g_nwatch; i++)
        if (g_watch[i].owner == a && g_watch[i].graph == graph && g_watch[i].node == node && g_watch[i].pin == pin) return (int)i;
    return -1;
}

// A node's marks: the breakpoint's red dot (hollow: disabled) at its top left; paused there: a yellow outline and arrow
static void dbg_node_marks(Win* w, ImDrawList* dl, const BpNode* n, ImVec2 p0, ImVec2 p1, float r, float z) {
    bool en = false;
    if (bp_debug_has(w->a, w->graph, n->id, &en)) {
        ImVec2 c = { p0.x - 1.0f * z, p0.y - 1.0f * z };
        float rr = 8.0f * z;
        ImDrawList_AddCircleFilled(dl, c, rr + 1.5f * z, THA(TK_N0, 200), 16);
        if (en) ImDrawList_AddCircleFilled(dl, c, rr, TH(TK_RED), 16);
        else ImDrawList_AddCircle(dl, c, rr, TH(TK_RED), 16, 2.0f * z);
    }
    BpDbgFrame f;
    if (bp_debug_paused() && bp_debug_stack(&f, 1) && f.owner == w->a && f.graph == w->graph && f.node_id == n->id) {
        float e = 3.0f * z;
        ImDrawList_AddRect(dl, (ImVec2){ p0.x - e, p0.y - e }, (ImVec2){ p1.x + e, p1.y + e }, TH(TK_AMBER), r + e, 3.0f * z, 0);
        float ay = p0.y + 12.0f * z, ax = p0.x - 8.0f * z;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ ax - 16 * z, ay - 9 * z }, (ImVec2){ ax, ay }, (ImVec2){ ax - 16 * z, ay + 9 * z }, TH(TK_AMBER));
    }
}

// An exec wire into node's input pin that ran lately: drawn again bright, fading over 0.7 s (Unreal's execution flow)
static int exec_ord_of(const BpNode* n, int pin) {
    int k = 0;
    for (int i = 0; i < pin && i < n->npins; i++) k += !n->pins[i].out && n->pins[i].type == BPT_EXEC;
    return k > 3 ? 3 : k;
}
static void dbg_wire(Win* w, ImDrawList* dl, const BpNode* to, int to_pin, ImVec2 a0, ImVec2 b0, float z) {
    float age = bp_debug_node_age(w->a, w->graph, to->id, exec_ord_of(to, to_pin));
    if (age < 0.0f || age > 0.7f) return;
    float f = 1.0f - age / 0.7f;
    float pulse = 0.75f + 0.25f * sinf((float)igGetTime() * 18.0f);
    wire(dl, a0, b0, THA(TK_AMBER_L, (int)(255.0f * f * pulse)), z * (1.0f + 1.6f * f), true);
}

static void dbg_toggle_selected(Win* w) {
    BpGraph* g = &w->a->graphs[w->graph];
    for (uint32_t s = 0; s < w->nsel; s++) {
        const BpNode* n = bp_node(g, w->sel[s]);
        if (n && !bp_node_desc(n->kind)->pure && n->kind != BPK_REROUTE && n->kind != BPK_VAR_GET) bp_debug_toggle(w->a, w->graph, n->id);
    }
}

// The node menu's debugging part (Unreal's: Add / Remove / Disable Breakpoint)
static void dbg_node_menu(Win* w, const BpNode* sn) {
    if (!sn || bp_node_desc(sn->kind)->pure) return;
    igSeparatorText(TR("Debug"));
    bool en = false, has = bp_debug_has(w->a, w->graph, sn->id, &en);
    if (ui_menu_item(ICON_BREAKPOINT, has ? TR("Remove Breakpoint") : TR("Add Breakpoint"), "F9", false, true)) bp_debug_toggle(w->a, w->graph, sn->id);
    if (has && ui_menu_item(ICON_BREAKPOINT, en ? TR("Disable Breakpoint") : TR("Enable Breakpoint"), NULL, false, true)) bp_debug_enable(w->a, w->graph, sn->id, !en);
}
static void dbg_pin_menu(Win* w, const BpNode* n, int pin) {
    if (n->pins[pin].type == BPT_EXEC) return;
    int wi = watch_index(w->a, w->graph, n->id, pin);
    if (wi >= 0 && ui_menu_item(ICON_EYE_CLOSED, TR("Stop Watching This Value"), NULL, false, true)) g_watch[wi] = g_watch[--g_nwatch];
    else if (wi < 0 && g_nwatch < 32 && ui_menu_item(ICON_EYE, TR("Watch This Value"), NULL, false, true)) {
        g_watch[g_nwatch++] = (DbgWatch){ w->a, n->id, (uint16_t)w->graph, (uint8_t)pin };
        g_dbg_open = true;
    }
}

// The toolbar's part: the instance to debug (Unreal's Debug Object) while playing, the debugger's window, the steps
static void dbg_toolbar(Win* w, Editor* ed) {
    BpAsset* a = w->a;
    igSameLine(0, 18);
    if (ui_icon_button("bp_debugger", ICON_BUG, TR("Debug"), bp_debug_paused() ? TH(TK_AMBER) : TH(TK_N14), g_dbg_open,
                       TR("The Blueprint Debugger: breakpoints (F9), the call stack, the watched values")))
        g_dbg_open = !g_dbg_open;
    if (!ed->playing) return;
    igSameLine(0, 6);
    Entity cur = bp_debug_object(a), list[64];
    uint32_t n = bp_debug_instances(a, list, 64);
    char label[96];
    if (cur && ecs_alive(&ed->world, cur)) {
        const CName* nm = ecs_get(&ed->world, cur, COMP_NAME);
        snprintf(label, sizeof label, "%s", nm ? nm->text : "?");
    } else
        snprintf(label, sizeof label, n ? TR("No debug object selected") : TR("No instances playing"));
    igSetNextItemWidth(igGetFontSize() * 13.0f);
    if (igBeginCombo("##dbgobj", label, 0)) {
        if (igSelectable_Bool(TR("No debug object selected"), !cur, 0, (ImVec2){ 0, 0 })) bp_debug_set_object(a, ENTITY_NONE);
        for (uint32_t i = 0; i < n && i < 64; i++) {
            const CName* nm = ecs_get(&ed->world, list[i], COMP_NAME);
            igPushID_Int((int)i);
            if (igSelectable_Bool(nm ? nm->text : "?", list[i] == cur, 0, (ImVec2){ 0, 0 })) bp_debug_set_object(a, list[i]);
            igPopID();
        }
        igEndCombo();
    }
    igSetItemTooltip("%s", TR("Debug Object: the instance whose values show and whose breakpoints stop (none: any). Selecting an actor of this class in the level picks it."));
    // (Unreal's: an actor of the class selected in the Outliner becomes the debug object)
    Entity sel = ed->nsel ? ed->sel[ed->nsel - 1] : ENTITY_NONE;
    for (uint32_t i = 0; sel && sel != cur && i < n && i < 64; i++)
        if (list[i] == sel) bp_debug_set_object(a, sel);
    if (bp_debug_paused()) {
        igSameLine(0, 10);
        if (igButton(TR("Resume"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_CONTINUE);
        igSetItemTooltip("%s", TR("Resume the game until the next breakpoint (F5)"));
        igSameLine(0, 4);
        if (igButton(TR("Step Over"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_OVER);
        igSetItemTooltip("%s", TR("Run this node and stop at the next one in this graph (F10)"));
        igSameLine(0, 4);
        if (igButton(TR("Step Into"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_INTO);
        igSetItemTooltip("%s", TR("Stop at the next node, inside a function or another actor's event too (F11)"));
        igSameLine(0, 4);
        if (igButton(TR("Step Out"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_OUT);
        igSetItemTooltip("%s", TR("Run to the end of this function and stop after it (Shift+F11)"));
    }
}

// The editor's window of an asset brought to a node of it, centered (the call stack's frames, a breakpoint hit)
static void dbg_focus(const BpAsset* owner, uint32_t graph, uint32_t node, int line) {
    char vp[700];
    if (!owner || !asset_vpath(owner, vp, sizeof vp)) return;
    if (owner->generated) { // (a script's class: its Code Editor at the line)
        if (line > 0) script_ide_open_at(vp, line);
        return;
    }
    bp_editor_open(vp);
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].open && g_win[i].a == owner) {
            Win* w = &g_win[i];
            w->focus = true;
            if (graph < owner->ngraphs && owner->graphs[graph].gkind != BPG_DISPATCHER && owner->graphs[graph].gkind != BPG_EVENT) {
                open_graph(w, graph);
                w->want_tab = 1 + (int)graph, w->want_hold = 2;
                w->center_node = node;
            }
        }
}

static void dbg_window(Editor* ed) {
    igSetNextWindowSize((ImVec2){ 520, 560 }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Blueprint Debugger"), &g_dbg_open, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        return;
    }
    BpDbgFrame fr[16];
    uint32_t nf = bp_debug_stack(fr, 16);
    if (bp_debug_paused()) {
        igTextColored(THV(TK_WARN), "%s", TR("Paused at a breakpoint: the world waits."));
        if (igButton(TR("Resume"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_CONTINUE);
        igSameLine(0, 4);
        if (igButton(TR("Step Over"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_OVER);
        igSameLine(0, 4);
        if (igButton(TR("Step Into"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_INTO);
        igSameLine(0, 4);
        if (igButton(TR("Step Out"), (ImVec2){ 0, 0 })) bp_debug_command(BPD_STEP_OUT);
        igTextDisabled("%s", TR("F5 Resume, F10 Step Over, F11 Step Into, Shift+F11 Step Out"));
    } else
        igTextDisabled("%s", ed->playing ? TR("Running. A node with a breakpoint (F9 on it) stops the game there.") : TR("Not playing. Breakpoints stop the Play (Alt+P) at their node."));
    if (igCollapsingHeader_TreeNodeFlags(TR("Call Stack"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!nf) igTextDisabled("%s", TR("(not paused)"));
        for (uint32_t i = 0; i < nf; i++) {
            igPushID_Int((int)i);
            char row[160];
            snprintf(row, sizeof row, "%s%s", i == 0 ? "> " : "   ", fr[i].what);
            if (igSelectable_Bool(row, i == 0, 0, (ImVec2){ 0, 0 })) dbg_focus(fr[i].owner, fr[i].graph, fr[i].node_id, fr[i].line);
            if (fr[i].line > 0 && igIsItemHovered(0)) igSetTooltip(TR("Line %d of the script"), fr[i].line);
            igPopID();
        }
    }
    if (igCollapsingHeader_TreeNodeFlags(TR("Watches"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!g_nwatch) igTextDisabled("%s", TR("Right click a pin > Watch This Value."));
        if (g_nwatch && igBeginTable("watch", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            for (uint32_t i = 0; i < g_nwatch; i++) {
                DbgWatch* q = &g_watch[i];
                BpNode* n = q->graph < q->owner->ngraphs ? bp_node(&q->owner->graphs[q->graph], q->node) : NULL;
                if (!n || q->pin >= n->npins) continue;
                igPushID_Int((int)i);
                igTableNextRow(0, 0);
                igTableNextColumn();
                char title[128];
                node_title(n, title, sizeof title);
                igText("%s.%s", title, n->pins[q->pin].name[0] ? n->pins[q->pin].name : TR("Return Value"));
                igTableNextColumn();
                char val[256];
                if (bp_debug_pin_text(q->owner, q->graph, q->node, q->pin, ENTITY_NONE, val, sizeof val)) igTextWrapped("%s", val);
                else igTextDisabled("%s", TR("(no value)"));
                igTableNextColumn();
                bool del = igSmallButton("x");
                igPopID();
                if (del) {
                    g_watch[i] = g_watch[--g_nwatch];
                    break;
                }
            }
            igEndTable();
        }
    }
    Entity who = bp_debug_paused_actor();
    if (igCollapsingHeader_TreeNodeFlags(TR("Variables"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!who) igTextDisabled("%s", TR("(paused: the instance's variables)"));
        else if (igBeginTable("vars", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
            char name[64], val[256];
            for (uint32_t i = 0; i < 256; i++) {
                name[0] = 0;
                bool ok = bp_debug_var_text(who, i, name, sizeof name, val, sizeof val);
                if (!name[0]) break;
                igTableNextRow(0, 0);
                igTableNextColumn();
                igText("%s", name);
                igTableNextColumn();
                if (ok) igTextWrapped("%s", val);
                else igTextDisabled("-");
            }
            igEndTable();
        }
    }
    if (igCollapsingHeader_TreeNodeFlags(TR("Breakpoints"), ImGuiTreeNodeFlags_DefaultOpen)) {
        BpDbgBreak bl[64];
        uint32_t nb = bp_debug_breakpoints(bl, 64);
        if (!nb) igTextDisabled("%s", TR("None. Select a node and press F9 (or right click > Add Breakpoint)."));
        for (uint32_t i = 0; i < nb && i < 64; i++) {
            igPushID_Int((int)i);
            bool en = bl[i].enabled;
            if (igCheckbox("##en", &en)) bp_debug_enable((BpAsset*)bl[i].owner, bl[i].graph, bl[i].node_id, en);
            igSameLine(0, -1);
            BpNode* n = bl[i].graph < bl[i].owner->ngraphs ? bp_node((BpGraph*)&bl[i].owner->graphs[bl[i].graph], bl[i].node_id) : NULL;
            char title[128] = "?", row[256];
            if (n) node_title(n, title, sizeof title);
            snprintf(row, sizeof row, "%s > %s > %s  (%u)", bl[i].owner->name, bl[i].owner->graphs[bl[i].graph].name, title, bl[i].hits);
            if (igSelectable_Bool(row, false, 0, (ImVec2){ igGetContentRegionAvail().x - igGetFrameHeight() - 6, 0 }))
                dbg_focus(bl[i].owner, bl[i].graph, bl[i].node_id, n && n->comment[0] == 'L' ? atoi(n->comment + 1) : 0);
            igSameLine(0, -1);
            bool del = igSmallButton("x");
            igPopID();
            if (del) {
                bp_debug_toggle((BpAsset*)bl[i].owner, bl[i].graph, bl[i].node_id);
                break;
            }
        }
        if (nb && igButton(TR("Delete All Breakpoints"), (ImVec2){ 0, 0 })) bp_debug_clear(NULL);
    }
    igEnd();
}

// Every frame (bp_editor_draw): a pause just begun brings its node into view and frees the mouse; the debugger's keys
static void dbg_frame(Editor* ed) {
    bool paused = bp_debug_paused();
    if (paused && !g_dbg_was_paused) {
        BpDbgFrame f;
        if (bp_debug_stack(&f, 1)) dbg_focus(f.owner, f.graph, f.node_id, f.line);
        g_dbg_open = true;
        if (ed->possessed && !ed->mouse_released) { // (the cursor back to the editor; captured again on Resume)
            ed->mouse_released = true, g_dbg_recapture = true;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
    if (!paused && g_dbg_was_paused && g_dbg_recapture && ed->playing && ed->possessed) {
        g_dbg_recapture = false;
        ed->mouse_released = false;
        glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(ed->window, &ed->last_cursor[0], &ed->last_cursor[1]);
    }
    if (!ed->playing) g_dbg_recapture = false;
    g_dbg_was_paused = paused;
    if (paused && !igGetIO_Nil()->WantTextInput) { // (Unreal's keys while paused; F10 / F11 are the Debug Viewer's and full screen's otherwise)
        bool shift = igGetIO_Nil()->KeyShift;
        if (igIsKeyPressed_Bool(ImGuiKey_F5, false)) bp_debug_command(BPD_CONTINUE);
        if (igIsKeyPressed_Bool(ImGuiKey_F10, false)) bp_debug_command(BPD_STEP_OVER);
        if (igIsKeyPressed_Bool(ImGuiKey_F11, false)) bp_debug_command(shift ? BPD_STEP_OUT : BPD_STEP_INTO);
    }
    if (g_dbg_open) dbg_window(ed);
}


void bp_editor_draw(Editor* ed, double now) {
    (void)now;
    g_focused = false;
    if (g_show.pin_pending) // (--show bp-pin: its Event Graph kept in front until the menu opens; the tabs dock first)
        for (int i = 0; i < MAX_WIN; i++)
            if (g_win[i].open && g_win[i].a == g_show.a) g_win[i].want_tab = 1, g_win[i].want_hold = 1;
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].open) draw_window(&g_win[i], ed);
        else if (g_win[i].a && ed->playing) bp_debug_watch(g_win[i].a, false); // (closed: its traces off)
    dbg_frame(ed); // the Blueprint debugger: a pause brought into view, its keys, its window
}

void bp_editor_open_viewport(const char* vpath) {
    bp_editor_open(vpath);
    BpAsset* a = bp_load(vpath);
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].open && g_win[i].a == a) g_win[i].want_tab = 0;
}

// (2026-11-23) Organize Nodes (Q, the node's menu): the selected nodes (two or more), else the whole graph, in columns
// toward where their wires go - the events' chains to the right, the pure nodes just left of what they feed
// (graph_layout.c); Ctrl+Z undoes it
static void organize_nodes(Win* w) {
    BpGraph* g = &w->a->graphs[w->graph];
    if (g->nnodes < 2) return;
    bp_graph_undo_push(w->a, w->graph); // (2026-11-24: its Ctrl+Z)
    GlNode* nodes = malloc(sizeof(GlNode) * g->nnodes);
    GlEdge* edges = malloc(sizeof(GlEdge) * (g->nlinks + 1));
    bool* only = malloc(sizeof(bool) * g->nnodes);
    if (!nodes || !edges || !only) {
        free(nodes), free(edges), free(only);
        return;
    }
    const float pan0[2] = { 0, 0 };
    bool some = w->nsel >= 2;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        Geo ge;
        node_geo(g, &g->nodes[i], (ImVec2){ 0, 0 }, pan0, 1.0f, &ge);
        nodes[i] = (GlNode){ g->nodes[i].x, g->nodes[i].y, ge.size.x, ge.size.y };
        only[i] = !some || is_sel(w, g->nodes[i].id);
    }
    int ne = 0;
    for (uint32_t l = 0; l < g->nlinks; l++) {
        int a = -1, b = -1;
        for (uint32_t i = 0; i < g->nnodes && (a < 0 || b < 0); i++) {
            if (g->nodes[i].id == g->links[l].from_node) a = (int)i;
            if (g->nodes[i].id == g->links[l].to_node) b = (int)i;
        }
        if (a >= 0 && b >= 0) edges[ne++] = (GlEdge){ a, b };
    }
    if (graph_layout(nodes, (int)g->nnodes, edges, ne, only, GL_FLOW_RIGHT, 80.0f, 36.0f)) {
        for (uint32_t i = 0; i < g->nnodes; i++) g->nodes[i].x = roundf(nodes[i].x), g->nodes[i].y = roundf(nodes[i].y);
        touch(w);
    }
    free(nodes), free(edges), free(only);
}


// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
// (2026-11-25, item 9) the Blueprints open, for the layout (editor_host.h: reopened with the project)
void bp_editor_doc_paths(void (*cb)(const char* path, void* user), void* user) {
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].open && g_win[i].a && g_win[i].a->path[0]) cb(g_win[i].a->path, user);
}

int bp_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        Win* w = &g_win[i];
        if (op == DOCS_CLOSE) { // (a closed window's asset too: bp_editor_draw watches it while playing)
            if (w->a && ed && ed->playing) bp_debug_watch(w->a, false);
            n += w->open;
            memset(w, 0, sizeof *w);
            continue;
        }
        if (!w->open || !w->a) continue;
        if (op == DOCS_OPEN) n++;
        else if (op == DOCS_UNSAVED && w->a->dirty) n++, editor_docs_add_name(names, cap, w->a->name);
        else if (op == DOCS_SAVE && w->a->dirty) {
            save(w, ed);
            n += w->a->dirty; // (still unsaved: its status says why)
        }
    }
    if (op == DOCS_CLOSE) g_focused = false;
    return n;
}

// (2026-11-25, the tester's photos) A Blueprint open for a screenshot: pin: its Event Graph with the menu of its first
// splittable pin (or the one named) open (Split Struct Pin); var: that variable in the Details with its value's asset
// picker open (a Level variable's: the levels). What isn't there is said on the console
void bp_editor_show(const char* vpath, const char* pin, const char* var) {
    bp_editor_open(vpath);
    BpAsset* a = bp_load(vpath);
    if (!a) {
        printf("[RealEngine] --show: no Blueprint %s\n", vpath);
        return;
    }
    memset(&g_show, 0, sizeof g_show);
    g_show.a = a;
    for (int i = 0; i < MAX_WIN; i++) {
        Win* w = &g_win[i];
        if (!w->open || w->a != a) continue;
        if (pin) {
            snprintf(g_show.pin, sizeof g_show.pin, "%s", pin);
            g_show.pin_pending = true, w->want_tab = 1, w->want_hold = 4; // (its Event Graph)
        }
        if (var) {
            int vi = bp_var_index(a, var);
            if (vi < 0) printf("[RealEngine] --show: %s has no variable %s\n", a->name, var);
            else snprintf(g_show.var, sizeof g_show.var, "%s", var), g_show.var_pending = true, w->var = vi, w->details = DET_VAR;
        }
    }
}
