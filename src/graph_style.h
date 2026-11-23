// The graph editors' look (2026-11-10, Editor Preferences > Graph Style): the nodes' style and the wires' style, chosen
// apart (any node style with any wire style), each from a gallery of previews. The Blueprint editor draws its nodes and
// wires through it; the Material, Animation, Behavior Tree, Sound Cue and Dialogue graphs their wires, cards and
// background. Kept in editor.ini.
// (Q5, 2026-11-17, notas-dev/icones-e-tooltips.md) the engine's own look, "Órbita", is the default: nodes as cards
// (8 px corners, a header band in the family's color with its icon, a halo when selected, a red outline and badge on an
// error), pins by family in color AND shape (a value a circle, an Array a bar of three, a Set a double ring, a Map two
// linked circles, a Class a hollow ring), wires in a gradient from the output's color to the input's, a background of
// dots (the bigger grid fading in when zoomed in). The 5 styles before it stay as alternatives.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct ImDrawList;
typedef struct ImVec2_c ImVec2; // (cimgui's)

enum { GS_NODE_DEFAULT = 0, GS_NODE_FLAT, GS_NODE_MINIMAL, GS_NODE_GLASS, GS_NODE_NEON, GS_NODE_ORBIT, GS_NODE_COUNT };
enum { GS_LINK_CURVE = 0, GS_LINK_STRAIGHT, GS_LINK_CIRCUIT, GS_LINK_GLOW, GS_LINK_FLOW, GS_LINK_GRADIENT, GS_LINK_COUNT };
// The defaults (a new editor.ini; an old one that kept the first style moves to these: graph_set_styles_ini)
#define GS_NODE_DEFAULT_STYLE GS_NODE_ORBIT
#define GS_LINK_DEFAULT_STYLE GS_LINK_GRADIENT
// A pin's shape (its family): the Órbita style draws each its own way; the older styles keep theirs
enum { GP_VALUE = 0, GP_EXEC, GP_ARRAY, GP_SET, GP_MAP, GP_CLASS, GP_SOFT, GP_DELEGATE };
// A node's family (its header's color token and its icon)
enum { GF_EVENT = 0, GF_FUNCTION, GF_PURE, GF_LATENT, GF_MACRO, GF_FLOW, GF_VARIABLE, GF_CAST, GF_ENTRY, GF_COUNT };

int graph_node_style(void);
int graph_link_style(void);
void graph_set_styles(int node_style, int link_style);
// editor.ini's values: "graph_node_style" (before Q5: 0 was the only default, moved to Órbita) or "graph_style2"
void graph_set_styles_ini(int node_style, int link_style, bool v2);
bool graph_orbit(void); // the nodes in the Órbita style (the cards, the pins' shapes, the dots)
const char* graph_node_style_name(int s); // English (TR() it)
const char* graph_link_style_name(int s);

// A wire from a (an output) to b (an input) in the current link style (or `style` >= 0): col its color, z the zoom,
// exec an execution wire (a little thicker)
void graph_wire(struct ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t col, float z, bool exec, int style);
// The mouse's distance to that wire (px)
float graph_wire_dist(ImVec2 a, ImVec2 b, ImVec2 m, float z, int style);
// A node's frame (its body and its header band of header_h px in header_col; its border in border_col, border_w px) in
// a node style other than the default (the editors keep their own default drawing)
void graph_node_frame(struct ImDrawList* dl, ImVec2 p0, ImVec2 p1, float header_h, float r, uint32_t header_col, uint32_t border_col,
                      float border_w, float z, int style);
// A wire in a gradient from ca (the output's color) to cb (the input's): the Órbita wire; another style draws graph_wire
// in ca
void graph_wire2(struct ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t ca, uint32_t cb, float z, bool exec, int style);
// A pin of a shape (GP_*) centered at c: col its color (a Map's key), col2 a Map's value; linked filled, else hollow
void graph_pin(struct ImDrawList* dl, ImVec2 c, int shape, uint32_t col, uint32_t col2, bool linked, float z);
// The canvas's background at o (size): offx/offy the pan in px (pan * zoom), z the zoom. Órbita: dots (the bigger grid
// fading in past zoom 1); the others: Unreal-like lines. bg 0: the theme's graph background
void graph_background(struct ImDrawList* dl, ImVec2 o, ImVec2 size, float offx, float offy, float z, uint32_t bg);
// The header color (token) and icon (UiIcon) of a family
uint32_t graph_family_color(int fam);
int graph_family_icon(int fam);
// A node of the graphs other than the Blueprint's (Material, Behavior Tree, Sound Cue, Dialogue, Anim states, the
// particles' emitters) as a card in the current style: its body, its header band (header_h px, 0: none) in header_col
// with the family's icon (icon < 0: none), its border by its state (sel, hov, err). The title is the caller's
void graph_card(struct ImDrawList* dl, ImVec2 p0, ImVec2 p1, float header_h, uint32_t header_col, int icon, bool sel, bool hov,
                bool err, float z);
// The error's badge (a red disc with !) at a node's top right corner
void graph_error_badge(struct ImDrawList* dl, ImVec2 corner, float z);
// Where a card's title goes after its icon (x of the text)
float graph_card_title_x(ImVec2 p0, float header_h, int icon, float z);
// A small graph (two nodes and their wires) in the given styles inside [p0, p1]: the preferences' gallery
void graph_style_preview(struct ImDrawList* dl, ImVec2 p0, ImVec2 p1, int node_style, int link_style);
