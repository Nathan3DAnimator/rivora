// The Material Editor (mat_editor.h): Blender's / the Tlender's shader nodes on a canvas (header colors by category,
// socket colors by type, links), Shift+A / right click adds a node by category, Details edits the selected node, the
// baked textures are previewed; a Material Instance shows its parent's parameters with their overrides.
#include "mat_editor.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include "graph_style.h"
#include "graph_layout.h"
#include "graph_view.h" // (2026-11-23: gv_edge_scroll)
#include "tex_settings.h"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <cimgui_impl.h>

#include "asset_viewer.h"
#include "asset_viewport.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "graph_menu.h"
#include "material.h"
#include "udim.h" // (Q18) a UDIM image's tiles under the field
#include "platform.h"
#include "platform_files.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "toon_ui.h" // (Q11)

// The menu's recents and recommendations (graph_menu.h): a node's definition by its English name
static const MatNodeDef* mat_def_named(const MatNodeDef** defs, int nd, const char* name) {
    for (int k = 0; k < nd; k++)
        if (!strcmp(defs[k]->name, name)) return defs[k];
    return NULL;
}
typedef struct MatAccept { const MatNodeDef** defs; int nd; const char* shown[2]; } MatAccept;
static bool mat_accept(const char* item, void* user) {
    const MatAccept* u = user;
    return mat_def_named(u->defs, u->nd, item) && (!u->shown[0] || strcmp(u->shown[0], item)) && (!u->shown[1] || strcmp(u->shown[1], item));
}

static struct {
    Material* m;
    bool open;
    int sel;              // the selected node (-1: none)
    int drag_node;        // moving a node
    uint64_t msel[4];     // more nodes selected (Ctrl / Shift + click, a box on the empty canvas): they move and delete together
    bool box;
    ImVec2 box_from;
    int link_from, link_sock; // dragging a link from an output
    int link_to, link_to_sock; // ... or detaching from an input
    int link_in, link_in_sock; // (2026-11-23) dragging a link backwards from a free input (Tlender, Blueprints)
    bool link_detached;   // the link was picked from an input: dropped on nothing it is gone (no menu)
    ImVec2 link_press;    // where the drag began (a release near it: a click, no menu)
    bool add_menu;
    float add_x, add_y;   // where the node goes (graph units)
    // (2026-11-23) a link dropped on the empty canvas: the add menu lists only the nodes that take it and links the new
    // one (Tlender's link search, Unreal's context menu): from an output (add_from/add_sock) or an input (add_in/add_in_sock)
    int add_from, add_sock, add_in, add_in_sock;
    ImVec2 move_press;    // a node drag: the mouse when it began, the nodes' places then (applied before drawing: no frame behind)
    float move_start[MAT_MAX_NODES][2];
    char status[200];
    double dirty_at;      // a pending bake (edits batched: one bake per 0.12 s while dragging)
    bool unsaved;
    VkDescriptorSet preview[3]; // base, normal, emission (the preview sphere's glow)
    VkImageView preview_view[3];
} E = { .sel = -1, .drag_node = -1, .link_from = -1, .link_to = -1, .link_in = -1, .add_from = -1, .add_in = -1 };

static const char* L(const char* en, const char* pt) { return i18n_language() == LANG_PT_BR && pt ? pt : en; }

static void touch(void) {
    E.unsaved = true;
    if (E.dirty_at <= 0.0) E.dirty_at = igGetTime() + 0.12;
}

static bool vpath_to_path(const char* vpath, char* out, size_t cap) {
    if (strncmp(vpath, "/Content", 8)) return false;
    snprintf(out, cap, "%s%s", content_root(), vpath + 8);
    for (char* c = out; *c; c++)
        if (*c == '/') *c = '\\';
    return true;
}

void mat_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    char path[600];
    if (!vpath_to_path(vpath, path, sizeof path)) return;
    Material* m = mat_load(path);
    if (!m) return;
    E.m = m;
    E.open = true;
    E.sel = -1;
    E.unsaved = false;
    E.status[0] = 0;
}

static void unique_path(const char* folder_vpath, const char* base, const char* ext, char* out, size_t cap) {
    char dir[600];
    vpath_to_path(folder_vpath, dir, sizeof dir);
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(out, cap, "%s\\%s_%d.%s", dir, base, i, ext);
        else snprintf(out, cap, "%s\\%s.%s", dir, base, ext);
        FILE* f = re_fopen(out, "rb");
        if (!f) return;
        fclose(f);
    }
}

bool mat_editor_add_menu(const char* folder_vpath, int part) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    char path[700];
    if (part == 0 && ui_menu_item(ICON_IMAGE, TR("Material"), NULL, false, ok)) {
        unique_path(folder_vpath, "M_NewMaterial", "rmat", path, sizeof path);
        Material* m = mat_create(path, false, NULL);
        if (m) {
            E.m = m, E.open = true, E.sel = -1, made = true;
        }
    }
    // (Q6) Material Instance is no longer listed here with every material: it is made from a material's right click
    // (Create > Material Instance); the advanced Material submenu has the Material itself
    if (part == 1 && ui_menu_item(ICON_IMAGE, TR("Material"), NULL, false, ok)) {
        unique_path(folder_vpath, "M_NewMaterial", "rmat", path, sizeof path);
        Material* m = mat_create(path, false, NULL);
        if (m) E.m = m, E.open = true, E.sel = -1, made = true;
    }
    if (part == 1) igSetItemTooltip("%s", TR("A Material Instance is made from its material: right click the material > Create > Material Instance."));
    return made;
}

// --- The canvas ---

typedef struct Geo {
    ImVec2 p0, p1;               // the node's box
    ImVec2 in[MAT_NODE_MAX_IN], out[MAT_NODE_MAX_OUT];
    bool in_shown[MAT_NODE_MAX_IN];
} Geo;

static ImVec2 V2(float x, float y) { return (ImVec2){ x, y }; }

// The node search: every word typed is in the name, case aside ("tex img" finds Image Texture)
static bool find_match(const char* name, const char* q) {
    if (!name) return false;
    char w[64];
    while (*q) {
        while (*q == ' ') q++;
        int n = 0;
        while (q[n] && q[n] != ' ' && n < 63) w[n] = q[n], n++;
        if (!n) break;
        w[n] = 0, q += n;
        bool hit = false;
        for (const char* h = name; *h && !hit; h++) {
            int k = 0;
            while (k < n && h[k] && (h[k] | 32) == (w[k] | 32)) k++;
            hit = k == n;
        }
        if (!hit) return false;
    }
    return true;
}

// A link's two ends fit: a shader only into a shader (mat_graph_link's rule; a reroute knot takes both)
static bool sock_fits(int out_type, int in_type) { return (out_type == MSOCK_SHADER) == (in_type == MSOCK_SHADER); }
// The socket of a new node a dropped link goes into: the same type first, else the first that fits (-1: none)
static int best_in(const MatNodeDef* d, int out_type) {
    if (d->type == MAT_NODE_REROUTE) return d->nin ? 0 : -1;
    for (int k = 0; k < d->nin; k++)
        if ((int)d->in[k].type == out_type) return k;
    for (int k = 0; k < d->nin; k++)
        if (sock_fits(out_type, d->in[k].type)) return k;
    return -1;
}
static int best_out(const MatNodeDef* d, int in_type) {
    if (d->type == MAT_NODE_REROUTE) return d->nout ? 0 : -1;
    for (int k = 0; k < d->nout; k++)
        if ((int)d->out[k].type == in_type) return k;
    for (int k = 0; k < d->nout; k++)
        if (sock_fits(d->out[k].type, in_type)) return k;
    return -1;
}
// The menu's context: the dragged output's or input's type (-1: none, every node)
static int link_type_out(const MatGraph* g) {
    const MatNodeDef* d = E.add_from >= 0 && E.add_from < g->node_count ? mat_node_def(g->nodes[E.add_from].type) : NULL;
    return d && E.add_sock < d->nout ? (int)d->out[E.add_sock].type : -1;
}
static int link_type_in(const MatGraph* g) {
    const MatNodeDef* d = E.add_in >= 0 && E.add_in < g->node_count ? mat_node_def(g->nodes[E.add_in].type) : NULL;
    return d && E.add_in_sock < d->nin ? (int)d->in[E.add_in_sock].type : -1;
}
static bool def_fits(const MatGraph* g, const MatNodeDef* d) {
    int to = link_type_out(g), ti = link_type_in(g);
    if (d->type == NODE_OUTPUT_MATERIAL) return false;
    if (to >= 0) return best_in(d, to) >= 0;
    if (ti >= 0) return best_out(d, ti) >= 0;
    return true;
}
// A node picked in the menu: made where the menu opened and, after a dropped link, linked to it (its input on the
// left of the drop, or its output on the right)
static int add_picked(MatGraph* g, const MatNodeDef* pick) {
    int to = link_type_out(g), ti = link_type_in(g);
    float x = E.add_x, y = E.add_y;
    if (to >= 0) x += 20.0f, y -= 40.0f;
    else if (ti >= 0) x -= (float)pick->width + 20.0f, y -= 40.0f;
    int idx = mat_graph_add(g, pick->type, x, y);
    if (idx >= 0) {
        if (to >= 0) mat_graph_link(g, E.add_from, E.add_sock, idx, best_in(pick, to));
        else if (ti >= 0) mat_graph_link(g, idx, best_out(pick, ti), E.add_in, E.add_in_sock);
        memset(E.msel, 0, sizeof E.msel);
        E.sel = idx;
        touch();
    }
    E.add_from = E.add_in = -1;
    return idx;
}

static void node_geo(const MatNode* n, const MatNodeDef* d, ImVec2 o, float z, Geo* g) {
    float row = 20.0f * z, head = 24.0f * z, w = d->width * z;
    g->p0 = V2(o.x + n->x * z, o.y + n->y * z);
    float y = g->p0.y + head + 6.0f * z;
    for (int i = 0; i < d->nout; i++, y += row) g->out[i] = V2(g->p0.x + w, y + row * 0.5f);
    if (n->type == NODE_RGB || n->type == NODE_VALUE || n->type == NODE_TEX_IMAGE || n->type == NODE_COLOR_RAMP) y += row; // its value
    for (int i = 0; i < d->nin; i++) {
        // Principled's many inputs: the ones a Tlender material links show, plus the main ones
        bool shown = true; // (every input is a pin, the Principled BSDF's too: any of them takes a link)
        g->in_shown[i] = shown;
        if (!shown) {
            g->in[i] = V2(g->p0.x, y);
            continue;
        }
        g->in[i] = V2(g->p0.x, y + row * 0.5f);
        y += row;
    }
    g->p1 = V2(g->p0.x + w, y + 6.0f * z);
}

static void bezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float z) {
    graph_wire(dl, a, b, col, z, false, -1); // (Editor Preferences > Graph Style)
}

static void value_text(const MatNode* n, const MatNodeDef* d, int i, char* out, size_t cap) {
    const MatSockDef* s = &d->in[i];
    if (s->type == MSOCK_FLOAT) snprintf(out, cap, "%.3g", n->in[i][0]);
    else if (s->type == MSOCK_VECTOR && !(s->flags & MSF_HIDE_VALUE)) snprintf(out, cap, "%.2g %.2g %.2g", n->in[i][0], n->in[i][1], n->in[i][2]);
    else out[0] = 0;
}

#include "mat_editor_test.inc" // (2026-11-23) the editor's own test by injected mouse and keys


// (2026-11-23, the user: "several textures dropped on a Principled: each into the output its name fits, organized")
// A node's height in the graph (node_geo's rows at zoom 1)
static float node_h(int type) {
    const MatNodeDef* d = mat_node_def(type);
    if (!d) return 120.0f;
    bool value = type == NODE_RGB || type == NODE_VALUE || type == NODE_TEX_IMAGE || type == NODE_COLOR_RAMP;
    return 24.0f + 6.0f + 20.0f * (float)(d->nout + d->nin + (value ? 1 : 0)) + 6.0f;
}
// Textures dropped: onto a Principled BSDF (pbr >= 0) each into the input its name says (mat_map_slot) - color, metallic,
// roughness (gloss through an Invert), an ORM through a Separate Color, opacity (the material cut by it), a normal
// through a Normal Map (DirectX's flipped), emission - in a column left of it in the inputs' order, the helpers between;
// the ones without an input (AO, height, unknown) under them. Else a column of Image Textures where they fell. The count
int mat_editor_drop_textures(MatGraph* g, Material* m, char (*paths)[512], int n, int pbr, float x, float y) {
    static const int ORDER[] = { MAT_MAP_BASE, MAT_MAP_METAL, MAT_MAP_ORM, MAT_MAP_ROUGH, MAT_MAP_GLOSS, MAT_MAP_ALPHA, MAT_MAP_NORMAL,
                                 MAT_MAP_NORMAL_DX, MAT_MAP_EMIT, MAT_MAP_AO, MAT_MAP_HEIGHT, MAT_MAP_NONE };
    int slot[64], made = 0;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) slot[i] = mat_map_slot(paths[i], NULL, 0);
    float tx = pbr >= 0 ? g->nodes[pbr].x - 600.0f : x, hx = pbr >= 0 ? g->nodes[pbr].x - 250.0f : x, cy = pbr >= 0 ? g->nodes[pbr].y : y;
    for (size_t o = 0; o < sizeof ORDER / sizeof ORDER[0]; o++)
        for (int i = 0; i < n; i++) {
            if (slot[i] != ORDER[o] && !(ORDER[o] == MAT_MAP_NONE && slot[i] == MAT_MAP_NONE)) continue;
            if (pbr < 0) tx = x;
            int k = mat_graph_add(g, NODE_TEX_IMAGE, tx, cy);
            if (k < 0) return made;
            snprintf(g->nodes[k].image, sizeof g->nodes[k].image, "%s", paths[i]);
            int s = slot[i];
            tex_settings_to_node(paths[i], &g->nodes[k]); // (its color space, filter and wrap: its own settings, else its name's)
            made++;
            float h = node_h(NODE_TEX_IMAGE);
            if (pbr >= 0) {
                if (s == MAT_MAP_BASE) mat_graph_link(g, k, 0, pbr, 0);
                else if (s == MAT_MAP_METAL) mat_graph_link(g, k, 0, pbr, 1);
                else if (s == MAT_MAP_ROUGH) mat_graph_link(g, k, 0, pbr, 2);
                else if (s == MAT_MAP_ALPHA) {
                    mat_graph_link(g, k, 0, pbr, 4); // (a grayscale map: its color)
                    if (m && m->blend_mode == MAT_BLEND_OPAQUE) m->blend_mode = MAT_BLEND_MASKED, m->two_sided = true;
                } else if (s == MAT_MAP_EMIT) {
                    mat_graph_link(g, k, 0, pbr, 26);
                    if (g->nodes[pbr].in[27][0] < 1.0f) g->nodes[pbr].in[27][0] = 1.0f;
                } else if (s == MAT_MAP_GLOSS) {
                    int v = mat_graph_add(g, NODE_INVERT, hx, cy);
                    if (v >= 0) mat_graph_link(g, k, 0, v, 1), mat_graph_link(g, v, 0, pbr, 2);
                } else if (s == MAT_MAP_ORM) {
                    int v = mat_graph_add(g, NODE_SEPARATE_COLOR, hx, cy);
                    if (v >= 0) mat_graph_link(g, k, 0, v, 0), mat_graph_link(g, v, 1, pbr, 2), mat_graph_link(g, v, 2, pbr, 1);
                } else if (s == MAT_MAP_NORMAL || s == MAT_MAP_NORMAL_DX) {
                    int v = mat_graph_add(g, NODE_NORMAL_MAP, hx, cy);
                    if (v >= 0) {
                        g->nodes[v].prop[1] = s == MAT_MAP_NORMAL_DX ? 1 : 0; // (DirectX: its green flipped)
                        mat_graph_link(g, k, 0, v, 1), mat_graph_link(g, v, 0, pbr, 5);
                    }
                }
            }
            cy += h + 24.0f;
        }
    return made;
}

// (2026-11-23) --mat-autolink-test: the names (Substance, Quixel, Poly Haven, Unreal's) to their maps, and textures
// dropped on a Principled BSDF linked by them (no window, no GPU). 0: passed
int mat_autolink_selftest(void) {
    static const struct { const char* name; int slot; const char* key; } N[] = {
        { "T_Wall_BaseColor.png", MAT_MAP_BASE, "twall" }, { "rock_nor_gl_2k.jpg", MAT_MAP_NORMAL, "rock" },
        { "rock_nor_dx_4k.exr", MAT_MAP_NORMAL_DX, "rock" }, { "WoodFloor_Roughness.tga", MAT_MAP_ROUGH, "woodfloor" },
        { "WoodRoughness.png", MAT_MAP_ROUGH, "wood" }, { "Chair_ORM.png", MAT_MAP_ORM, "chair" }, { "metal_plate_metallic.png", MAT_MAP_METAL, "metalplate" },
        { "Leaf_Opacity.png", MAT_MAP_ALPHA, "leaf" }, { "Screen_Emissive.png", MAT_MAP_EMIT, "screen" }, { "Brick_AO.png", MAT_MAP_AO, "brick" },
        { "Sand_Height_8K.png", MAT_MAP_HEIGHT, "sand" }, { "Car_Albedo.png", MAT_MAP_BASE, "car" }, { "Car_Gloss.png", MAT_MAP_GLOSS, "car" },
        { "/Content/Tex/Skin_N.png", MAT_MAP_NORMAL, "skin" }, { "/Content/Tex/Skin_D.png", MAT_MAP_BASE, "skin" },
        { "Grass Diffuse 1024px.jpg", MAT_MAP_BASE, "grass" }, { "Logo.png", MAT_MAP_NONE, "logo" }, { "MetalDoor.png", MAT_MAP_NONE, "metaldoor" },
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) {
        char key[96];
        int s = mat_map_slot(N[i].name, key, sizeof key);
        if (s != N[i].slot || strcmp(key, N[i].key)) printf("[RealEngine] FAIL: autolink: %s -> map %d key '%s' (wanted %d '%s')\n", N[i].name, s, key, N[i].slot, N[i].key), fails++;
    }
    static MatGraph g;
    mat_graph_default(&g);
    int pbr = -1;
    for (int k = 0; k < g.node_count; k++)
        if (g.nodes[k].type == NODE_BSDF_PRINCIPLED) pbr = k;
    static char paths[8][512];
    static const char* const drop[8] = { "/Content/T/Wall_Normal_DX.png", "/Content/T/Wall_BaseColor.png", "/Content/T/Wall_ORM.png", "/Content/T/Wall_Emissive.png",
                                        "/Content/T/Wall_Opacity.png", "/Content/T/Wall_AO.png", "/Content/T/Wall_Height.png", "/Content/T/Notes.png" };
    for (int i = 0; i < 8; i++) snprintf(paths[i], 512, "%s", drop[i]);
    Material m;
    memset(&m, 0, sizeof m);
    int made = pbr >= 0 ? mat_editor_drop_textures(&g, &m, paths, 8, pbr, 0, 0) : 0;
    // what feeds each Principled input, through its helper
    int feed[32];
    for (int k = 0; k < 32; k++) feed[k] = -1;
    for (int l = 0; l < g.link_count; l++)
        if (g.links[l].to == pbr && g.links[l].to_sock < 32) feed[g.links[l].to_sock] = g.links[l].from;
    const char* why = NULL;
    if (made != 8) why = "8 Image Textures";
    else if (feed[0] < 0 || g.nodes[feed[0]].type != NODE_TEX_IMAGE || !strstr(g.nodes[feed[0]].image, "BaseColor")) why = "the color into Base Color";
    else if (feed[5] < 0 || g.nodes[feed[5]].type != NODE_NORMAL_MAP || g.nodes[feed[5]].prop[1] != 1) why = "the DirectX normal through a Normal Map";
    else if (feed[2] < 0 || feed[2] != feed[1] || g.nodes[feed[2]].type != NODE_SEPARATE_COLOR) why = "the ORM's roughness and metallic through a Separate Color";
    else if (feed[26] < 0 || g.nodes[pbr].in[27][0] < 1.0f) why = "the emission";
    else if (feed[4] < 0 || m.blend_mode != MAT_BLEND_MASKED) why = "the opacity (Masked)";
    else {
        float last_y = -1e30f; // the column in the inputs' order: color, ORM, opacity, normal, emission, AO, height, the rest
        static const char* const order[8] = { "BaseColor", "ORM", "Opacity", "Normal", "Emissive", "AO", "Height", "Notes" };
        for (int o = 0; o < 8 && !why; o++)
            for (int k = 0; k < g.node_count; k++)
                if (g.nodes[k].type == NODE_TEX_IMAGE && strstr(g.nodes[k].image, order[o])) {
                    if (g.nodes[k].y <= last_y) why = "the column in the inputs' order";
                    last_y = g.nodes[k].y;
                }
    }
    if (why) printf("[RealEngine] FAIL: autolink: dropped on a Principled BSDF: %s\n", why), fails++;
    // (2026-11-23) the textures' own settings: by their names, then saved beside one and read back
    static const struct { const char* name; int color; } TS[] = { { "Wall_Roughness.png", TEXC_LINEAR }, { "Wall_Normal.png", TEXC_NORMAL }, { "Wall_BaseColor.png", TEXC_SRGB },
                                                                   { "Wall_ORM.png", TEXC_LINEAR }, { "Wall_Emissive.png", TEXC_SRGB }, { "Logo.png", TEXC_SRGB } };
    for (size_t i = 0; i < sizeof TS / sizeof TS[0]; i++) {
        TexSettings ts;
        tex_settings_get(TS[i].name, &ts);
        if (ts.color != TS[i].color || ts.saved) printf("[RealEngine] FAIL: texture settings: %s by its name: color %d (wanted %d)\n", TS[i].name, ts.color, TS[i].color), fails++;
    }
    {
        char tmp[600];
        const char* td = getenv("TEMP");
        int n = snprintf(tmp, sizeof tmp - 40, "%s/", td && td[0] ? td : ".");
        snprintf(tmp + n, sizeof tmp - (size_t)n, "re_texset_Roughness.png");
        TexSettings a1 = { TEXC_SRGB, TEXZ_NONE, 1, 3, 1024, false }, b1;
        bool saved = tex_settings_save(tmp, &a1);
        tex_settings_get(tmp, &b1);
        if (!saved || !b1.saved || b1.color != TEXC_SRGB || b1.compression != TEXZ_NONE || b1.filter != 1 || b1.wrap != 3 || b1.max_size != 1024)
            printf("[RealEngine] FAIL: texture settings: saved and read back differ\n"), fails++;
        char side[700];
        snprintf(side, sizeof side, "%.*s.re_texset_Roughness.png.rtex", (int)n, tmp);
        remove(side);
    }
    printf("[RealEngine] autolink: %d names, a drop of 8 on a Principled BSDF (%d links): %s\n", (int)(sizeof N / sizeof N[0]), g.link_count, fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

// (2026-11-23) Organize Nodes (Q, the canvas's menu): the selected ones (two or more), else all of them, in columns
// toward the Material Output (graph_layout.c); undone with Ctrl+Z
static bool organize_nodes(MatGraph* g) {
    static GlNode nodes[MAT_MAX_NODES];
    static GlEdge edges[MAT_MAX_LINKS];
    static bool only[MAT_MAX_NODES];
    int nsel = 0;
    for (int i = 0; i < g->node_count && i < 256; i++) nsel += (E.msel[i >> 6] >> (i & 63) & 1ull) || i == E.sel;
    for (int i = 0; i < g->node_count; i++) {
        const MatNodeDef* d = mat_node_def(g->nodes[i].type);
        nodes[i] = (GlNode){ g->nodes[i].x, g->nodes[i].y, d ? (float)d->width : 160.0f, node_h(g->nodes[i].type) };
        only[i] = nsel < 2 || (i < 256 && ((E.msel[i >> 6] >> (i & 63) & 1ull) || i == E.sel));
    }
    for (int l = 0; l < g->link_count; l++) edges[l] = (GlEdge){ g->links[l].from, g->links[l].to };
    if (!graph_layout(nodes, g->node_count, edges, g->link_count, only, GL_FLOW_RIGHT, 70.0f, 28.0f)) return false;
    for (int i = 0; i < g->node_count; i++) g->nodes[i].x = roundf(nodes[i].x), g->nodes[i].y = roundf(nodes[i].y);
    snprintf(E.status, sizeof E.status, "%s", TR("Nodes organized (Ctrl+Z undoes it)."));
    touch();
    return true;
}

static void draw_canvas(ImVec2 origin, ImVec2 size) {
    static const Material* shown;
    if (shown != E.m) shown = E.m, memset(E.msel, 0, sizeof E.msel), E.box = false; // (another material: nothing selected)
    MatGraph* g = E.m->graph;
    ImDrawList* dl = igGetWindowDrawList();
    ImGuiIO* io = igGetIO_Nil();
    float z = g->view[2] > 0.1f ? g->view[2] : 1.0f;
    ImVec2 o = V2(origin.x + g->view[0] + size.x * 0.5f, origin.y + g->view[1] + size.y * 0.5f);
    ImDrawList_AddRectFilled(dl, origin, V2(origin.x + size.x, origin.y + size.y), TH(TK_N4), 0, 0);
    if (graph_orbit()) graph_background(dl, origin, size, o.x - origin.x, o.y - origin.y, z, 0); // (Q5: the dots)
    // the grid
    float step = 25.0f * z;
    if (step > 6.0f && !graph_orbit()) {
        for (float x = fmodf(o.x - origin.x, step); x < size.x; x += step)
            ImDrawList_AddLine(dl, V2(origin.x + x, origin.y), V2(origin.x + x, origin.y + size.y), TH(TK_N6), 1);
        for (float y = fmodf(o.y - origin.y, step); y < size.y; y += step)
            ImDrawList_AddLine(dl, V2(origin.x, origin.y + y), V2(origin.x + size.x, origin.y + y), TH(TK_N6), 1);
    }
    igSetCursorScreenPos(origin);
    igInvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    bool hovered = igIsItemHovered(0);
    ImVec2 mouse = io->MousePos;
    // (2026-11-16, bug #124 "não consigo arrastar as texturas para dentro do editor de materiais") a texture dragged from
    // the Content Browser onto the graph: an Image Texture node with it where it falls (Unreal's Texture Sample); a
    // material: a note in the status (it goes in Detail Material or a layer)
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* p = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (p && p->DataSize == (int)sizeof(AssetPayload)) {
            const AssetPayload* ap = p->Data;
            if (ap->kind == ASSET_TEXTURE_SOURCE) {
                // (2026-11-23) every texture dragged with it; onto a Principled BSDF: linked by their names, organized
                static char paths[64][512];
                int np = content_drag_paths(ap->vpath, paths, 64), nt = 0;
                for (int i = 0; i < np; i++)
                    if (content_kind_of(paths[i]) == ASSET_TEXTURE_SOURCE) memmove(paths[nt++], paths[i], 512);
                int pbr = -1;
                for (int i = 0; i < g->node_count && pbr < 0; i++) {
                    const MatNodeDef* d = mat_node_def(g->nodes[i].type);
                    Geo ge;
                    if (!d || g->nodes[i].type != NODE_BSDF_PRINCIPLED) continue;
                    node_geo(&g->nodes[i], d, o, z, &ge);
                    if (mouse.x >= ge.p0.x && mouse.x <= ge.p1.x && mouse.y >= ge.p0.y && mouse.y <= ge.p1.y) pbr = i;
                }
                int first = g->node_count;
                int made = mat_editor_drop_textures(g, E.m, paths, nt, pbr, (mouse.x - o.x) / z, (mouse.y - o.y) / z);
                if (made) {
                    memset(E.msel, 0, sizeof E.msel);
                    for (int i = first; i < g->node_count && i < 256; i++) E.msel[i >> 6] |= 1ull << (i & 63);
                    E.sel = g->node_count - 1;
                    if (pbr >= 0) snprintf(E.status, sizeof E.status, TR("%d texture(s) linked by their names."), made);
                    touch();
                }
            } else
                snprintf(E.status, sizeof E.status, "%s", TR("Only textures go onto the graph (they become an Image Texture node)."));
        }
        igEndDragDropTarget();
    }
    // pan (middle or right drag), zoom (wheel) around the mouse
    if (hovered && (igIsMouseDragging(ImGuiMouseButton_Middle, 2) || (igIsMouseDragging(ImGuiMouseButton_Right, 4) && !E.add_menu))) {
        g->view[0] += io->MouseDelta.x;
        g->view[1] += io->MouseDelta.y;
    }
    if (hovered && io->MouseWheel != 0.0f) {
        float nz = fminf(fmaxf(z * (io->MouseWheel > 0 ? 1.12f : 1.0f / 1.12f), 0.3f), 2.5f);
        float gx = (mouse.x - o.x) / z, gy = (mouse.y - o.y) / z;
        g->view[2] = nz;
        g->view[0] += (gx * z - gx * nz);
        g->view[1] += (gy * z - gy * nz);
        z = nz;
        o = V2(origin.x + g->view[0] + size.x * 0.5f, origin.y + g->view[1] + size.y * 0.5f);
    }
    // a node drag: the selection placed from where it began and the mouse now, before anything is drawn (2026-11-23: moved
    // after drawing, the nodes and their wires showed a frame behind the mouse; the Blueprint graph's fix)
    { // (2026-11-23) Blender's edge pan: a node or a link dragged near the edge scrolls the view (the node stays under the mouse)
        float es[2];
        bool dragging = (E.drag_node >= 0 && E.drag_node < g->node_count) || E.link_from >= 0 || E.link_in >= 0;
        if (dragging && igIsMouseDown_Nil(ImGuiMouseButton_Left) && gv_edge_scroll(origin, size, es)) {
            g->view[0] -= es[0], g->view[1] -= es[1];
            o.x -= es[0], o.y -= es[1];
            E.move_press.x -= es[0], E.move_press.y -= es[1];
        }
    }
    if (E.drag_node >= 0 && E.drag_node < g->node_count) {
        if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            bool group = E.msel[E.drag_node >> 6] >> (E.drag_node & 63) & 1ull;
            float dx = (mouse.x - E.move_press.x) / z, dy = (mouse.y - E.move_press.y) / z;
            for (int i = 0; i < g->node_count; i++) // (the selection moves together)
                if (i == E.drag_node || (group && (E.msel[i >> 6] >> (i & 63) & 1ull))) {
                    float nx = E.move_start[i][0] + dx, ny = E.move_start[i][1] + dy;
                    if (g->nodes[i].x != nx || g->nodes[i].y != ny) E.unsaved = true;
                    g->nodes[i].x = nx, g->nodes[i].y = ny;
                }
        } else E.drag_node = -1;
    }
    static Geo geo[MAT_MAX_NODES];
    for (int i = 0; i < g->node_count; i++) {
        const MatNodeDef* d = mat_node_def(g->nodes[i].type);
        if (d) node_geo(&g->nodes[i], d, o, z, &geo[i]);
    }
    ImDrawList_PushClipRect(dl, origin, V2(origin.x + size.x, origin.y + size.y), true);
    // the text follows the zoom like the boxes (as in the Blueprint graph): never larger or smaller than its node
    const ImGuiStyle* st = igGetStyle();
    igPushFont(NULL, roundf((st->FontSizeBase > 0 ? st->FontSizeBase : 13.0f) * z)); // (2026-11-23: whole pixels: each new size baked glyphs and waited on the GPU, a hitch per zoom step)
    // the links
    for (int i = 0; i < g->link_count; i++) {
        const MatLink* l = &g->links[i];
        const MatNodeDef* fd = mat_node_def(g->nodes[l->from].type);
        if (!fd || (E.link_to == l->to && E.link_to_sock == l->to_sock)) continue;
        const MatNodeDef* td = mat_node_def(g->nodes[l->to].type); // (Q5: the wire from the output's color to the input's)
        ImU32 ca = mat_socket_color(fd->out[l->from_sock].type), cb = td ? mat_socket_color(td->in[l->to_sock].type) : ca;
        graph_wire2(dl, geo[l->from].out[l->from_sock], geo[l->to].in[l->to_sock], ca, cb, z, false, -1);
    }
    // the nodes
    int hover_node = -1, hover_out = -1, hover_out_node = -1, hover_in = -1, hover_in_node = -1; // (a pin's node apart: a node drawn after, under the pin, took hover_node and the link left the wrong node)
    float r = 5.5f * z;
    for (int i = 0; i < g->node_count; i++) {
        MatNode* n = &g->nodes[i];
        const MatNodeDef* d = mat_node_def(n->type);
        if (!d) continue;
        Geo* q = &geo[i];
        bool sel = i == E.sel || (E.msel[i >> 6] >> (i & 63) & 1ull);
        bool orbit = graph_orbit();
        if (orbit) // (Q5) a card: the category's band and the Material family's icon
            graph_card(dl, q->p0, q->p1, 24.0f * z, mat_category_color(d->category), z > 0.45f ? ICON_MATERIAL : -1, sel, false, false, z);
        else {
            ImDrawList_AddRectFilled(dl, q->p0, q->p1, THA(TK_N7, 240), 5.0f * z, 0);
            ImDrawList_AddRectFilled(dl, q->p0, V2(q->p1.x, q->p0.y + 24.0f * z), mat_category_color(d->category), 5.0f * z,
                                     ImDrawFlags_RoundCornersTop);
            ImDrawList_AddRect(dl, q->p0, q->p1, sel ? TH(TK_ACCENT) : TH(TK_N2), 5.0f * z, sel ? 2.0f : 1.0f, 0);
        }
        const char* title = n->label[0] ? n->label : L(d->name, d->label);
        char t[120];
        if (n->param[0]) snprintf(t, sizeof t, "%s  [%s]", title, n->param);
        else snprintf(t, sizeof t, "%s", title);
        if (mat_node_unused_inputs(g, i, NULL, 0) > 0 && z > 0.45f) { // an input set that does nothing yet: "!" in the title
            ImVec2 bc = V2(q->p1.x - 12.0f * z, q->p0.y + 12.0f * z);
            ImDrawList_AddCircleFilled(dl, bc, 7.0f * z, TH(TK_AMBER), 12);
            ImVec2 es = igCalcTextSize("!", NULL, false, -1);
            ImDrawList_AddText_Vec2(dl, V2(bc.x - es.x * 0.5f, bc.y - es.y * 0.5f), TH(TK_N3), "!", NULL);
        }
        if (z > 0.45f) ImDrawList_AddText_Vec2(dl, V2(orbit ? graph_card_title_x(q->p0, 24.0f * z, 0, z) : q->p0.x + 8 * z, q->p0.y + 4 * z), TH(TK_NODE_TITLE), t, NULL);
        if (mouse.x >= q->p0.x && mouse.x <= q->p1.x && mouse.y >= q->p0.y && mouse.y <= q->p1.y) hover_node = i;
        for (int k = 0; k < d->nout; k++) {
            ImVec2 c = q->out[k];
            if (orbit) graph_pin(dl, c, GP_VALUE, mat_socket_color(d->out[k].type), 0, true, z); // (Q5)
            else ImDrawList_AddCircleFilled(dl, c, r, mat_socket_color(d->out[k].type), 12);
            if (z > 0.45f) {
                const char* lb = L(d->out[k].name, d->out[k].label);
                ImVec2 ts = igCalcTextSize(lb, NULL, false, -1);
                ImDrawList_AddText_Vec2(dl, V2(c.x - ts.x - 10 * z, c.y - ts.y * 0.5f), TH(TK_N14), lb, NULL);
            }
            if (fabsf(mouse.x - c.x) < r * 2 && fabsf(mouse.y - c.y) < r * 2) hover_out = k, hover_out_node = hover_node = i;
        }
        if ((n->type == NODE_RGB || n->type == NODE_VALUE || n->type == NODE_TEX_IMAGE || n->type == NODE_COLOR_RAMP) && z > 0.45f) {
            float y = q->p0.y + 30.0f * z + 20.0f * z * (float)d->nout;
            if (n->type == NODE_RGB)
                ImDrawList_AddRectFilled(dl, V2(q->p0.x + 8 * z, y), V2(q->p1.x - 8 * z, y + 14 * z),
                                         igColorConvertFloat4ToU32((ImVec4){ n->value[0], n->value[1], n->value[2], 1 }), 3, 0);
            else if (n->type == NODE_COLOR_RAMP) {
                float x0 = q->p0.x + 8 * z, x1 = q->p1.x - 8 * z;
                for (int s = 0; s < 32; s++) {
                    float c[4];
                    mat_ramp_eval(n, (s + 0.5f) / 32.0f, c);
                    ImDrawList_AddRectFilled(dl, V2(x0 + (x1 - x0) * s / 32.0f, y), V2(x0 + (x1 - x0) * (s + 1) / 32.0f, y + 14 * z),
                                             igColorConvertFloat4ToU32((ImVec4){ c[0], c[1], c[2], 1 }), 0, 0);
                }
            } else {
                char v[96];
                if (n->type == NODE_VALUE) snprintf(v, sizeof v, "%.3g", n->value[0]);
                else {
                    const char* b = strrchr(n->image, '/');
                    const char* b2 = strrchr(n->image, '\\');
                    if (b2 > b) b = b2;
                    snprintf(v, sizeof v, "%s", n->image[0] ? (b ? b + 1 : n->image) : L("(no image)", "(sem imagem)"));
                }
                ImDrawList_AddText_Vec2(dl, V2(q->p0.x + 10 * z, y), TH(TK_N14), v, NULL);
            }
        }
        for (int k = 0; k < d->nin; k++) {
            if (!q->in_shown[k]) continue;
            ImVec2 c = q->in[k];
            bool linked = mat_graph_input_link(g, i, k) >= 0;
            ImDrawList_AddCircleFilled(dl, c, r, mat_socket_color(d->in[k].type), 12);
            if (z > 0.45f) {
                char lb[140], val[48];
                value_text(n, d, k, val, sizeof val);
                if (d->in[k].type == MSOCK_COLOR && !linked)
                    ImDrawList_AddRectFilled(dl, V2(q->p1.x - 26 * z, c.y - 6 * z), V2(q->p1.x - 8 * z, c.y + 6 * z),
                                             igColorConvertFloat4ToU32((ImVec4){ n->in[k][0], n->in[k][1], n->in[k][2], 1 }), 2, 0);
                snprintf(lb, sizeof lb, "%s%s%s", L(d->in[k].name, d->in[k].label), !linked && val[0] ? ": " : "", !linked ? val : "");
                ImVec2 ts = igCalcTextSize(lb, NULL, false, -1);
                bool effect = mat_input_has_effect(n->type, k) || (!linked && mat_input_const_only(n->type, k)); // (greyed: no effect yet, notas-dev/materiais.md)
                // (2026-11-10) the Principled's extra lobes (IOR, subsurface, specular, anisotropy, coat) are drawn by the
                // opaque and the masked runs (Masked, Two Sided), not by the OIT pass of a Translucent one: greyed there;
                // a Masked one keeps its cut over the Transmission (material_gpu.inc)
                if (n->type == NODE_BSDF_PRINCIPLED && (k == 3 || (k >= 7 && k <= 9) || (k >= 12 && k <= 15) || (k >= 18 && k <= 21)) &&
                    E.m->blend_mode == MAT_BLEND_TRANSLUCENT)
                    effect = false;
                if (n->type == NODE_BSDF_PRINCIPLED && k == 17 && E.m->blend_mode == MAT_BLEND_MASKED) effect = false;
                ImDrawList_AddText_Vec2(dl, V2(c.x + 10 * z, c.y - ts.y * 0.5f), effect ? TH(TK_N14) : TH(TK_N11), lb, NULL);
            }
            if (fabsf(mouse.x - c.x) < r * 2 && fabsf(mouse.y - c.y) < r * 2) hover_in = k, hover_in_node = i;
        }
    }
    // Unreal's shortcuts: a key held + a click on the canvas makes its node there; pressed while dragging a link, the
    // node is made at the mouse with the link in its first input
    int quick = -1, quick_op = -1;
    {
        static const struct { ImGuiKey key; int type, op; } keys[] = {
            { ImGuiKey_1, NODE_VALUE, -1 }, { ImGuiKey_3, NODE_RGB, -1 }, { ImGuiKey_T, NODE_TEX_IMAGE, -1 },
            { ImGuiKey_M, NODE_MATH, 2 /* Multiply */ }, { ImGuiKey_A, NODE_MATH, 0 /* Add */ }, { ImGuiKey_L, NODE_MIX, -1 },
            { ImGuiKey_U, NODE_TEX_COORD, -1 },
        };
        bool focus = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io->WantTextInput && !io->KeyCtrl && !io->KeyShift;
        for (int k = 0; focus && k < (int)(sizeof keys / sizeof keys[0]); k++) {
            bool now = E.link_from >= 0 ? igIsKeyPressed_Bool(keys[k].key, false)
                                        : igIsKeyDown_Nil(keys[k].key) && hovered && hover_node < 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false);
            if (now) quick = keys[k].type, quick_op = keys[k].op;
        }
    }
    if (quick >= 0) {
        int idx = mat_graph_add(g, quick, (mouse.x - o.x) / z, (mouse.y - o.y) / z);
        if (idx >= 0) {
            if (quick_op >= 0) g->nodes[idx].prop[0] = quick_op;
            if (E.link_from >= 0) mat_graph_link(g, E.link_from, E.link_sock, idx, 0), E.link_from = -1;
            memset(E.msel, 0, sizeof E.msel);
            E.sel = idx;
            touch();
        }
    }
    // dragging a link
    if (E.link_from >= 0) {
        const MatNodeDef* fd = mat_node_def(g->nodes[E.link_from].type);
        bezier(dl, geo[E.link_from].out[E.link_sock], mouse, fd ? mat_socket_color(fd->out[E.link_sock].type) : TH(TK_N15), z);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            float moved = fabsf(mouse.x - E.link_press.x) + fabsf(mouse.y - E.link_press.y);
            if (hover_in >= 0 && mat_graph_link(g, E.link_from, E.link_sock, hover_in_node, hover_in)) touch();
            else if (hover_node < 0 && hover_in < 0 && !E.link_detached && moved > 8.0f) { // on nothing: the menu of what takes it
                E.add_from = E.link_from, E.add_sock = E.link_sock, E.add_in = -1;
                E.add_x = (mouse.x - o.x) / z, E.add_y = (mouse.y - o.y) / z;
                igOpenPopup_Str("mat_add", 0);
            } else if (hover_node >= 0 && hover_node != E.link_from) { // dropped on a node's body: its first free input of the same kind
                const MatNodeDef* td = mat_node_def(g->nodes[hover_node].type);
                int best = -1;
                for (int k = 0; td && fd && k < td->nin && best < 0; k++)
                    if (geo[hover_node].in_shown[k] && mat_graph_input_link(g, hover_node, k) < 0 && td->in[k].type == fd->out[E.link_sock].type) best = k;
                for (int k = 0; td && k < td->nin && best < 0; k++) // (else the first free one)
                    if (geo[hover_node].in_shown[k] && mat_graph_input_link(g, hover_node, k) < 0) best = k;
                if (best >= 0 && mat_graph_link(g, E.link_from, E.link_sock, hover_node, best)) touch();
            }
            E.link_from = -1;
        }
    }
    // dragging a link backwards from a free input: dropped on an output it links, on a node's body its first output that
    // fits, on nothing the menu of the nodes that can feed it
    if (E.link_in >= 0 && E.link_in < g->node_count) {
        const MatNodeDef* td = mat_node_def(g->nodes[E.link_in].type);
        int it = td && E.link_in_sock < td->nin ? (int)td->in[E.link_in_sock].type : (int)MSOCK_FLOAT;
        bezier(dl, mouse, geo[E.link_in].in[E.link_in_sock], mat_socket_color((MatSockType)it), z);
        if (igIsMouseReleased_Nil(ImGuiMouseButton_Left)) {
            float moved = fabsf(mouse.x - E.link_press.x) + fabsf(mouse.y - E.link_press.y);
            if (hover_out >= 0 && hover_out_node != E.link_in) {
                if (mat_graph_link(g, hover_out_node, hover_out, E.link_in, E.link_in_sock)) touch();
            } else if (hover_node >= 0 && hover_node != E.link_in) {
                const MatNodeDef* fd = mat_node_def(g->nodes[hover_node].type);
                int k = fd ? best_out(fd, it) : -1;
                if (k >= 0 && mat_graph_link(g, hover_node, k, E.link_in, E.link_in_sock)) touch();
            } else if (hover_node < 0 && moved > 8.0f) {
                E.add_in = E.link_in, E.add_in_sock = E.link_in_sock, E.add_from = -1;
                E.add_x = (mouse.x - o.x) / z, E.add_y = (mouse.y - o.y) / z;
                igOpenPopup_Str("mat_add", 0);
            }
            E.link_in = -1;
        }
    }
    igPopFont();
    ImDrawList_PopClipRect(dl);
    // mouse actions
    if (hovered && quick < 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        E.link_press = mouse;
        if (hover_out >= 0) {
            E.link_from = hover_out_node;
            E.link_sock = hover_out;
            E.link_detached = false;
        } else if (hover_in >= 0 && mat_graph_input_link(g, hover_in_node, hover_in) >= 0) { // detach: grab its link
            const MatLink* l = &g->links[mat_graph_input_link(g, hover_in_node, hover_in)];
            E.link_from = l->from;
            E.link_sock = l->from_sock;
            E.link_detached = true;
            mat_graph_unlink_input(g, hover_in_node, hover_in);
            touch();
        } else if (hover_in >= 0) { // a free input: a link drawn backwards from it
            E.link_in = hover_in_node, E.link_in_sock = hover_in;
        } else if (hover_node >= 0) { // Ctrl toggles it, Shift adds it, a click on an unselected one selects it alone
            uint64_t bit = 1ull << (hover_node & 63), *w = &E.msel[hover_node >> 6];
            if (io->KeyCtrl) *w ^= bit;
            else if (io->KeyShift || (*w & bit)) *w |= bit;
            else memset(E.msel, 0, sizeof E.msel), *w |= bit;
            E.sel = hover_node;
            E.drag_node = hover_node;
            E.move_press = mouse;
            for (int i = 0; i < g->node_count; i++) E.move_start[i][0] = g->nodes[i].x, E.move_start[i][1] = g->nodes[i].y;
        } else { // the empty canvas: a selection box
            if (!io->KeyCtrl && !io->KeyShift) memset(E.msel, 0, sizeof E.msel);
            E.sel = -1, E.box = true, E.box_from = mouse;
        }
    }
    if (E.box) { // the box: drawn while dragged; released, the nodes it touches join the selection
        ImVec2 b0 = V2(fminf(E.box_from.x, mouse.x), fminf(E.box_from.y, mouse.y)), b1 = V2(fmaxf(E.box_from.x, mouse.x), fmaxf(E.box_from.y, mouse.y));
        ImDrawList_AddRectFilled(dl, b0, b1, THA(TK_AMBER, 30), 0.0f, 0);
        ImDrawList_AddRect(dl, b0, b1, THA(TK_AMBER, 200), 0.0f, 1.0f, 0);
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            for (int i = 0; i < g->node_count; i++)
                if (geo[i].p0.x < b1.x && geo[i].p1.x > b0.x && geo[i].p0.y < b1.y && geo[i].p1.y > b0.y) {
                    E.msel[i >> 6] |= 1ull << (i & 63);
                    if (E.sel < 0) E.sel = i;
                }
            E.box = false;
        }
    }
    bool want_menu = (hovered && igIsMouseReleased_Nil(ImGuiMouseButton_Right) && !igIsMouseDragPastThreshold(ImGuiMouseButton_Right, 4)) ||
                     (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io->KeyShift && igIsKeyPressed_Bool(ImGuiKey_A, false));
    if (want_menu) {
        E.add_from = E.add_in = -1; // (no link: every node)
        E.add_x = (mouse.x - o.x) / z;
        E.add_y = (mouse.y - o.y) / z;
        igOpenPopup_Str("mat_add", 0);
    }
    static char find[64];
    if (igBeginPopup("mat_add", 0)) {
        if (!E.add_menu) find[0] = 0; // (opened now: an empty search)
        E.add_menu = true;
        const MatNodeDef* defs[NODE_TYPE_COUNT + 1];
        int nd = mat_node_defs(defs, NODE_TYPE_COUNT + 1);
        const MatNodeDef* pick = NULL;
        // the search, as in the Blueprints: typed at once, by the name in English or Portuguese
        if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
        igSetNextItemWidth(260.0f);
        bool enter = igInputTextWithHint("##find", TR("Search"), find, sizeof find, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        if (find[0]) {
            int shown_n = 0;
            for (int k = 0; k < nd && !pick; k++) {
                if (!def_fits(g, defs[k])) continue; // (the Material Output never; after a dropped link, what takes it)
                if (!find_match(defs[k]->name, find) && !find_match(defs[k]->label, find)) continue;
                char lb[160];
                snprintf(lb, sizeof lb, "%s##%d", L(defs[k]->name, defs[k]->label), k);
                if (ui_menu_item(ICON_MATERIAL, lb, mat_category_name(defs[k]->category), false, true) || (enter && !shown_n)) pick = defs[k];
                shown_n++;
            }
            if (!shown_n) igTextDisabled("%s", TR("Nothing found."));
            if (pick) {
                gm_note(GM_MATERIAL, "", pick->name);
                add_picked(g, pick);
                igCloseCurrentPopup();
            }
            igEndPopup();
            goto menu_done;
        }
        igSeparator();
        // the last 2 used in materials and 2 recommended after the selected node (graph_menu.h), then the categories
        const MatNodeDef* ctx_def = E.sel >= 0 && E.sel < g->node_count ? mat_node_def(g->nodes[E.sel].type) : NULL;
        const char* ctx = ctx_def ? ctx_def->name : "";
        const char* items[8];
        MatAccept u = { defs, nd, { NULL, NULL } };
        int n = gm_recent(GM_MATERIAL, items, 8), ns = 0;
        for (int i = 0; i < n && ns < GM_RECENT_SHOWN; i++) {
            const MatNodeDef* d = mat_def_named(defs, nd, items[i]);
            if (!d || !def_fits(g, d)) continue;
            if (!ns) igSeparatorText(gm_title_recent());
            igPushID_Str("mr"), igPushID_Int(ns);
            if (ui_menu_item(ICON_MATERIAL, L(d->name, d->label), NULL, false, true)) pick = d;
            igPopID(), igPopID();
            u.shown[ns++] = items[i];
        }
        n = gm_recommend(GM_MATERIAL, ctx, mat_accept, &u, items, GM_RECOMMEND_SHOWN);
        for (int i = 0; i < n; i++) {
            const MatNodeDef* d = mat_def_named(defs, nd, items[i]);
            if (d && !def_fits(g, d)) continue;
            if (!i) igSeparatorText(gm_title_recommended());
            igPushID_Str("mm"), igPushID_Int(i);
            if (d && ui_menu_item(ICON_MATERIAL, L(d->name, d->label), NULL, false, true)) pick = d;
            igPopID(), igPopID();
        }
        if (ns || n) igSeparator();
        static const char* const pt_cat[MCAT_COUNT] = { "Entrada", "Shader", "Textura", "Cor", "Vetor", "Conversor", "Saída" };
        for (int cat = 0; cat < MCAT_COUNT && !pick; cat++) {
            bool any = false; // (after a dropped link: only the categories with a node that takes it)
            for (int k = 0; k < nd && !any; k++) any = defs[k]->category == cat && def_fits(g, defs[k]);
            if (!any || !ui_begin_menu(ICON_FOLDER, L(mat_category_name(cat), pt_cat[cat]), true)) continue;
            for (int k = 0; k < nd; k++) {
                if (defs[k]->category != cat || !def_fits(g, defs[k])) continue;
                if (ui_menu_item(ICON_MATERIAL, L(defs[k]->name, defs[k]->label), NULL, false, true)) pick = defs[k];
            }
            igEndMenu();
        }
        if (pick) {
            gm_note(GM_MATERIAL, ctx, pick->name);
            add_picked(g, pick);
            igCloseCurrentPopup();
        }
        if (E.add_from < 0 && E.add_in < 0) { // (the canvas's own menu: Organize Nodes)
            igSeparator();
            if (ui_menu_item(ICON_SORT, TR("Organize Nodes"), "Q", false, g->node_count > 1)) organize_nodes(g), igCloseCurrentPopup();
        }
        igEndPopup();
    } else E.add_menu = false;
menu_done:
    if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io->WantTextInput && !io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Q, false))
        organize_nodes(g);
    if (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && E.sel >= 0 && !io->WantTextInput &&
        (igIsKeyPressed_Bool(ImGuiKey_Delete, false) || igIsKeyPressed_Bool(ImGuiKey_X, false))) {
        E.msel[E.sel >> 6] |= 1ull << (E.sel & 63);
        for (int i = g->node_count - 1; i >= 0; i--) // (every selected node, from the last: the indices below stay)
            if ((E.msel[i >> 6] >> (i & 63) & 1ull) && (g->nodes[i].type != NODE_OUTPUT_MATERIAL || g->node_count > 1)) mat_graph_remove(g, i);
        memset(E.msel, 0, sizeof E.msel);
        E.sel = -1;
        touch();
    }
    mated_test_frame(g, geo, origin, size);
}

// --- Details ---

// (Q18) A UDIM image's tiles under the field (a .rudim or a <UDIM> pattern); a tile of a set (<name>.1001.png with
// others beside it) offers to read the whole set (Unreal's Texture Sample on a UDIM texture: the tile from the UV).
// The disk is read when the path changes, not every frame
static bool udim_info(MatNode* n) {
    static char last[MAT_PATH];
    static int tiles, lo, hi, offer;
    static char pat[MAT_PATH], what[24];
    if (strcmp(last, n->image)) {
        snprintf(last, sizeof last, "%s", n->image);
        tiles = lo = hi = offer = 0, pat[0] = what[0] = 0;
        char full[MAT_PATH * 2];
        if (!strncmp(n->image, "/Content", 8)) snprintf(full, sizeof full, "%s%s", mat_root(), n->image + 8);
        else snprintf(full, sizeof full, "%s", n->image);
        int u[UDIM_MAX_TILES];
        size_t fl = strlen(full);
        if (fl > 6 && !_stricmp(full + fl - 6, ".rudim")) {
            UdimFile* f = calloc(1, sizeof *f);
            if (f && udim_open(full, f, NULL, 0)) {
                tiles = (int)f->count, lo = (int)f->tiles[0].udim, hi = (int)f->tiles[f->count - 1].udim;
                snprintf(what, sizeof what, "%s %u", f->format == UDIM_FMT_BC5 ? "BC5" : f->format == UDIM_FMT_BC7 ? "BC7" : "RGBA8", f->tiles[0].width);
                udim_close(f);
            }
            free(f);
        } else if (udim_is_path(full)) {
            tiles = udim_find_tiles(full, u, UDIM_MAX_TILES);
            if (tiles) lo = u[0], hi = u[tiles - 1];
        } else if (udim_detect(full, NULL, 0) && udim_pattern_of(n->image, pat, sizeof pat, NULL))
            offer = 1;
    }
    if (tiles) {
        igTextDisabled(TR("UDIM: %d tiles (%d - %d) %s"), tiles, lo, hi, what);
        igSetItemTooltip("%s", TR("Each tile of the UV (1001 at 0..1, 1002 to its right, 1011 above) reads its own image; the material is baked per tile and each tile streams on its own (only the tiles seen stay resident)."));
    } else if (udim_is_path(n->image))
        igTextColored(THV(TK_WARN), "%s", TR("UDIM: no tile found"));
    if (offer && igButton(TR("Use as UDIM"), (ImVec2){ 0, 0 })) {
        snprintf(n->image, sizeof n->image, "%s", pat);
        return true;
    }
    return false;
}

static bool image_field(MatNode* n) {
    bool ch = false;
    igSetNextItemWidth(-igGetFontSize() * 2.5f);
    ch |= igInputText("##img", n->image, sizeof n->image, 0, NULL, NULL);
    // an image dragged from the Content Browser
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* p = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        if (p && p->DataSize == (int)sizeof(AssetPayload)) {
            const AssetPayload* ap = p->Data;
            snprintf(n->image, sizeof n->image, "%s", ap->vpath);
            ch = true;
        }
        igEndDragDropTarget();
    }
    igSameLine(0, 4);
    if (igButton("...", (ImVec2){ 0, 0 })) {
        char path[1024] = "";
        extern struct GLFWwindow* g_mat_editor_window;
        if (platform_file_dialog_ext(g_mat_editor_window, false, TR("Images"), "png;jpg;jpeg;tga;bmp;tif;tiff;rudim", path, sizeof path)) {
            snprintf(n->image, sizeof n->image, "%s", path);
            ch = true;
        }
    }
    ch |= udim_info(n);
    return ch;
}

// (2026-11-23, "o painel de detalhes está confuso") Unreal's Details: a search on top, two tabs - Material (the asset's
// settings in folding sections, the ones in use open) and Node (the selected node: its values, where each input comes
// from with a click to it) - that follow the selection
static char g_dfind[64];
static bool sect(const char* title, const char* keys, bool open) {
    if (g_dfind[0]) {
        if (!find_match(title, g_dfind) && !find_match(keys, g_dfind)) return false;
        igSetNextItemOpen(true, ImGuiCond_Always); // (found: shown open)
    }
    return igCollapsingHeader_TreeNodeFlags(title, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

static void details_material(Material* m) {
    MatGraph* g = m->graph;
    if (sect(TR("Material"), "domain blend mode opacity mask clip two sided shading model eye iris glow texture resolution emissive light emission dominio mistura opacidade recorte dois lados modelo olho iris brilho resolucao emissao luz emissiva", true)) {
        { // (2026-11-17, Q4) Unreal's Material Domain
            int dm = m->domain;
            const char* dm_names[MAT_DOMAIN_COUNT] = { TR("Surface"), TR("Light Function") };
            if (igCombo_Str_arr(PL(TR("Material Domain")), &dm, dm_names, MAT_DOMAIN_COUNT, -1)) m->domain = (uint8_t)dm, touch();
            igSetItemTooltip("%s", TR("Surface: an actor's material. Light Function: a pattern a light projects (Emission, else Base Color; a Time node animates it): a projector's slide, blinds, clouds passing, a flickering bulb. Set it in a Spot, Point or Directional light's Light Function."));
        }
        int bm = m->blend_mode;
        static const char* const blend_names[MAT_BLEND_COUNT] = { "Opaque", "Masked", "Translucent" };
        if (igCombo_Str_arr(PL(TR("Blend Mode")), &bm, blend_names, MAT_BLEND_COUNT, -1)) m->blend_mode = (uint8_t)bm, touch();
        igSetItemTooltip("%s", TR("Masked: the pixels whose Alpha (the Principled BSDF's) is under the clip value are cut (hair, leaves, fences). Translucent: the Alpha is the opacity (glass, water, smoke), drawn over the scene in any order; no shadow."));
        if (m->blend_mode == MAT_BLEND_MASKED &&
            igSliderFloat(PL(TR("Opacity Mask Clip Value")), &m->opacity_clip, 0.0f, 1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp))
            touch();
        int sm = m->shading_model; // (A9.3c: Unreal's Shading Model)
        const char* sm_names[MAT_SHADING_COUNT] = { TR("Default Lit"), TR("Eye"), TR("Toon") }; // (Q11: Toon)
        if (igCombo_Str_arr(PL(TR("Shading Model")), &sm, sm_names, MAT_SHADING_COUNT, -1)) {
            m->shading_model = (uint8_t)sm;
            if (m->eye_iris_radius <= 0.0f) m->eye_iris_radius = 0.25f, m->eye_iris_depth = 0.25f;
            touch();
        }
        igSetItemTooltip("%s", TR("Eye: a clear, wet cornea over the iris; the iris seen through it, refracted, and lit like a dish (the light pools on the far side). The iris at the UV's center; the eye looks along its actor's forward (+X)."));
        if (m->shading_model == MAT_SHADING_EYE) {
            if (igSliderFloat(PL(TR("Iris Radius")), &m->eye_iris_radius, 0.02f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) touch();
            igSetItemTooltip("%s", TR("The iris's radius in UV units, around the UV's center (0.5, 0.5)."));
            if (igSliderFloat(PL(TR("Iris Depth")), &m->eye_iris_depth, 0.0f, 1.0f, "%.2f cm", ImGuiSliderFlags_AlwaysClamp)) touch();
            igSetItemTooltip("%s", TR("How far the cornea's dome stands over the iris at its center: the iris shifts as the view turns."));
            // (Q13) the eyes that shine in the dark: the light sent back toward its source
            if (igSliderFloat(PL(TR("Eye Glow")), &m->eye_glow, 0.0f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                if (m->eye_glow_color[0] + m->eye_glow_color[1] + m->eye_glow_color[2] <= 0.0f)
                    m->eye_glow_color[0] = 0.85f, m->eye_glow_color[1] = 1.0f, m->eye_glow_color[2] = 0.7f; // (a cat's: green-gold)
                touch();
            }
            igSetItemTooltip("%s", TR("The iris sends the light back where it came from: with a flashlight beside the camera the eyes shine in the dark, like an animal's or an animatronic's. 0: none."));
            if (m->eye_glow > 0.0f && igColorEdit3(PL(TR("Eye Glow Color")), m->eye_glow_color, ImGuiColorEditFlags_Float)) touch();
        }
        if (igCheckbox(PL(TR("Two Sided")), &m->two_sided)) touch();
        igSetItemTooltip("%s", TR("The back faces are drawn too, lit from their side (hair cards, leaves, thin cloth)."));
        int res = m->resolution;
        static const int sizes[] = { 256, 512, 1024, 2048, 4096 };
        const char* names[] = { "256", "512", "1024", "2048", "4096" };
        int cur = 2;
        for (int i = 0; i < 5; i++)
            if (sizes[i] == res) cur = i;
        if (igCombo_Str_arr(PL(TR("Texture Resolution")), &cur, names, 5, -1)) m->resolution = sizes[cur], touch();
        igSetItemTooltip("%s", TR("The size the material is baked at (its textures in the game): an imported material takes its largest image's, up to 4096."));
        // (2026-11-23) its glow lighting the scene (lights.c: a point light per actor; the GI's rays see it too)
        if (igSliderFloat(PL(TR("Emissive Light")), &m->emissive_light, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
        igSetItemTooltip("%s", TR("How much its glow (the Emission) lights what is around it: each actor with it lights like a lamp and bounces in the GI. 0: it glows but lights nothing."));
        if (m->emissive_light > 0.0f && igCheckbox(PL(TR("Emissive Shadows")), &m->emissive_shadows)) touch();
        igSetItemTooltip("%s", TR("Its glow's light casts ray traced shadows (what stands in the way blocks it). Costs about 1 ms at 1440p in a scene without other shadowed local lights; needs ray tracing."));
        if (m->avg_emis[0] + m->avg_emis[1] + m->avg_emis[2] > 0.0f)
            igTextDisabled(TR("Mean emission: %.0f nits"), (0.2126f * m->avg_emis[0] + 0.7152f * m->avg_emis[1] + 0.0722f * m->avg_emis[2]) * 100.0f);
        if (m->vcol == MAT_VCOL_OTHER)
            igTextColored(THV(TK_WARN), "%s", TR("Color Attribute: only multiplying the Base Color has an effect (directly or as a Mix Multiply's A)."));
    }
    if ((m->blend_mode == MAT_BLEND_TRANSLUCENT || m->transmissive) &&
        sect(TR("Refraction"), "refraction thickness dispersion glass refracao espessura dispersao vidro", true)) { // (2026-11-16) glass
        if (igSliderFloat(PL(TR("Refraction")), &m->refraction, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
        igSetItemTooltip("%s", TR("How much the glass bends what is seen through it, by its IOR (the Principled BSDF's) and its normal: 0 off, 1 real."));
        if (igSliderFloat(PL(TR("Refraction Thickness")), &m->refr_thickness, 0.0f, 100.0f, "%.1f cm", ImGuiSliderFlags_Logarithmic)) touch();
        igSetItemTooltip("%s", TR("How far the view travels inside it (cm): a window pane ~1, a glass or a bottle ~5, a lens or a crystal block more."));
        if (igSliderFloat(PL(TR("Dispersion")), &m->dispersion, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
        igSetItemTooltip("%s", TR("The colors split where it bends the most, as a prism or cut crystal (0: none)."));
    }
    { // (2026-11-17, Q4) Transmission: the Principled BSDF's weight here, its mode, the absorption, the colored shadow
        int pn = -1;
        for (int i = 0; g && i < g->node_count && pn < 0; i++)
            if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) pn = i;
        bool used = pn >= 0 && (mat_graph_input_link(g, pn, 17) >= 0 || g->nodes[pn].in[17][0] > 0.0f);
        if (pn >= 0 && m->blend_mode != MAT_BLEND_MASKED &&
            sect(TR("Transmission"), "transmission mode absorption colored shadow thin refractive transmissao absorcao sombra colorida fino", used)) {
            if (mat_graph_input_link(g, pn, 17) < 0) {
                float tw = g->nodes[pn].in[17][0];
                if (igSliderFloat(PL(TR("Transmission")), &tw, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) g->nodes[pn].in[17][0] = tw, touch();
                igSetItemTooltip("%s", TR("How much light goes through the surface (the Principled BSDF's Transmission Weight). Link a Value node with a Parameter Name to its pin to change it in Material Instances and by Blueprint."));
            } else
                igTextDisabled("%s", TR("Transmission: from its node (a Value with a Parameter Name: Material Instances and Blueprints set it)"));
            int tm = m->transmission_mode;
            const char* tm_names[MAT_TRANS_COUNT] = { TR("Refractive"), TR("Thin") };
            if (igCombo_Str_arr(PL(TR("Transmission Mode")), &tm, tm_names, MAT_TRANS_COUNT, -1)) m->transmission_mode = (uint8_t)tm, touch();
            igSetItemTooltip("%s", TR("Refractive: glass and water, see-through, bent by the IOR, rough and absorbing. Thin: a leaf, a curtain, paper, a lampshade, an ear: opaque, a light behind it shows through in its color."));
            if (m->transmission_mode == MAT_TRANS_REFRACTIVE) {
                if (igColorEdit3(PL(TR("Absorption Color")), m->absorption, 0)) touch();
                igSetItemTooltip("%s", TR("The color the light keeps after crossing Absorption Distance inside it (white: clear glass; amber, green bottle glass, blue water)."));
                if (igSliderFloat(PL(TR("Absorption Distance")), &m->absorption_dist, 0.1f, 1000.0f, "%.1f cm", ImGuiSliderFlags_Logarithmic)) touch();
                igSetItemTooltip("%s", TR("How far inside it the light reaches the Absorption Color (cm): thicker parts get deeper."));
                if (igCheckbox(PL(TR("Colored Shadow")), &m->colored_shadow)) touch();
                igSetItemTooltip("%s", TR("The sun's light through it takes its color on what is behind (stained glass, a bottle). Needs ray traced shadows."));
            }
        }
    }
    if (m->shading_model != MAT_SHADING_EYE && m->shading_model != MAT_SHADING_TOON && // (Q27.8) the city at night's surfaces
        sect(TR("Surface Effects"), "retroreflection metallic flakes paint car reflector retrorrefletor flocos metalicos pintura carro efeitos superficie",
             m->retro > 0.0f || m->flakes > 0.0f)) {
        if (igSliderFloat(PL(TR("Retroreflection")), &m->retro, 0.0f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            if (m->retro_color[0] + m->retro_color[1] + m->retro_color[2] <= 0.0f) m->retro_color[0] = m->retro_color[1] = m->retro_color[2] = 1.0f;
            touch();
        }
        igSetItemTooltip("%s", TR("Sends the light back toward where it came from: a road sign, a cat's eye on the road, a vest's stripes or a reflector shine in the headlights. 0: none."));
        if (m->retro > 0.0f && igColorEdit3(PL(TR("Retroreflection Color")), m->retro_color, ImGuiColorEditFlags_Float)) touch();
        if (igSliderFloat(PL(TR("Metallic Flakes")), &m->flakes, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            if (m->flake_size <= 0.0f) m->flake_size = 0.04f, m->flake_spread = 0.35f;
            touch();
        }
        igSetItemTooltip("%s", TR("Tiny metal flakes under the coat that glint one by one in the light: a car's metallic paint. 0: none."));
        if (m->flakes > 0.0f) {
            if (igSliderFloat(PL(TR("Paint Flake Size")), &m->flake_size, 0.005f, 1.0f, "%.3f cm", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) touch();
            igSetItemTooltip("%s", TR("Each flake's size: smaller ones glint finer and blend into a soft shine farther away."));
            if (igSliderFloat(PL(TR("Flake Spread")), &m->flake_spread, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
            igSetItemTooltip("%s", TR("How far the flakes tilt from the surface: more, they glint over a wider angle around the highlight."));
        }
    }
    // (Q11) the Toon shading model's values (when Toon) and the outline (any shading model)
    if (sect(TR("Toon and Outline"), "toon bands ramp outline contorno faixas rampa sombra", m->shading_model == MAT_SHADING_TOON))
        if (toon_ui_draw(&m->toon, m->shading_model == MAT_SHADING_TOON)) touch();
    if (sect(TR("World Aligned"), "world aligned tile size grid alinhado ao mundo tamanho grade", m->world_aligned || m->world_grid)) {
        if (igCheckbox(PL(TR("World Aligned")), &m->world_aligned)) {
            if (m->world_tile <= 1.0f) m->world_tile = 200.0f;
            touch();
        }
        igSetItemTooltip("%s", TR("The texture laid by the world position: walls and floors of any size tile at one size, without stretching."));
        if (m->world_aligned && igDragFloat(PL(TR("Tile Size")), &m->world_tile, 5.0f, 10.0f, 10000.0f, "%.0f cm", 0)) touch();
        if (igCheckbox(PL(TR("World Aligned Grid")), &m->world_grid)) touch();
        igSetItemTooltip("%s", TR("Grid lines in world space over the base color."));
    }
    // (Q17) Nanite Tessellation: Unreal's Enable Tessellation and Displacement Scaling (Magnitude, Center)
    if (sect(TR("Geometry Tessellation"), "nanite tessellation displacement magnitude center tesselacao deslocamento", m->tessellation)) {
        if (igCheckbox(PL(TR("Enable Tessellation")), &m->tessellation)) {
            if (m->disp_magnitude == 0.0f) m->disp_magnitude = 5.0f, m->disp_center = 0.5f;
            touch();
        }
        igSetItemTooltip("%s", TR("The Displacement moves the surface for real: the mesh's clusters are subdivided on the GPU by their size on screen and pushed along the normal by the height (the silhouette and the shadows change, not just the shading). Needs a mesh of 16 k triangles or more (its cluster LOD); smaller ones move their vertices only."));
        if (m->tessellation) {
            if (igDragFloat(PL(TR("Displacement Magnitude")), &m->disp_magnitude, 0.1f, -1000.0f, 1000.0f, "%.1f cm", 0)) touch();
            igSetItemTooltip("%s", TR("How far the height's whole range moves the surface (cm of the mesh: the actor's scale scales it)."));
            if (igSliderFloat(PL(TR("Displacement Center")), &m->disp_center, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
            igSetItemTooltip("%s", TR("The height that stays in place (Blender's Midlevel): below it the surface sinks, above it it rises."));
            if (!m->dirty && !m->tess_active)
                igTextColored(THV(TK_WARN), "%s", TR("Connect a height to the Material Output's Displacement (an opaque material, not animated)."));
        }
    }
    // M5: the detail normal (another material's normal, tiled finer: pores, scratches, fabric weave up close)
    if (sect(TR("Detail Normal"), "detail normal material tiling strength detalhe", m->detail_path[0] != 0)) {
        igSetNextItemWidth(-igGetFontSize() * 2.5f);
        if (igInputTextWithHint(PL(TR("Detail Material")), TR("drag a material here"), m->detail_path, sizeof m->detail_path, 0, NULL, NULL)) touch();
        if (igBeginDragDropTarget()) {
            const ImGuiPayload* p = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
            if (p && p->DataSize == (int)sizeof(AssetPayload)) {
                const AssetPayload* ap = p->Data;
                if (strstr(ap->vpath, ".rmat")) {
                    if (!m->detail_path[0] && m->detail_strength <= 0.0f) m->detail_strength = 1.0f; // (a new one: full strength)
                    snprintf(m->detail_path, sizeof m->detail_path, "%s", ap->vpath), touch();
                }
            }
            igEndDragDropTarget();
        }
        igSetItemTooltip("%s", TR("A material whose normal is laid over this one's, repeated Tiling times: the fine detail seen up close. Empty: none."));
        if (m->detail_path[0]) {
            igSameLine(0, 4);
            if (igButton("x##detail", (ImVec2){ 0, 0 })) m->detail_path[0] = 0, touch();
            if (m->detail_tiling <= 0.01f) m->detail_tiling = 8.0f;
            if (igDragFloat(PL(TR("Detail Tiling")), &m->detail_tiling, 0.1f, 1.0f, 256.0f, "%.1fx", 0)) touch();
            if (igSliderFloat(PL(TR("Detail Strength")), &m->detail_strength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) touch();
        }
    }
    // (B11) a landscape's painted layers: three materials blended over this one where the Landscape's Paint tool put them
    if (sect(TR("Landscape Layers"), "landscape layers paint layer camadas pintura terreno", m->layer_path[0][0] || m->layer_path[1][0] || m->layer_path[2][0])) {
        for (int k = 0; k < 3; k++) {
            igPushID_Int(k + 700);
            char lab[48];
            snprintf(lab, sizeof lab, "%s %d", TR("Layer"), k + 1);
            igSetNextItemWidth(-igGetFontSize() * 2.5f);
            if (igInputTextWithHint(PL(lab), TR("drag a material here"), m->layer_path[k], sizeof m->layer_path[k], 0, NULL, NULL)) touch();
            if (igBeginDragDropTarget()) {
                const ImGuiPayload* p = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
                if (p && p->DataSize == (int)sizeof(AssetPayload) && strstr(((const AssetPayload*)p->Data)->vpath, ".rmat"))
                    snprintf(m->layer_path[k], sizeof m->layer_path[k], "%s", ((const AssetPayload*)p->Data)->vpath), touch();
                igEndDragDropTarget();
            }
            if (m->layer_path[k][0]) {
                igSameLine(0, 4);
                if (igButton("x", (ImVec2){ 0, 0 })) m->layer_path[k][0] = 0, touch();
                if (m->layer_tile[k] < 1.0f) m->layer_tile[k] = 400.0f;
                if (igDragFloat(PL(TR("Tile")), &m->layer_tile[k], 5.0f, 10.0f, 1e5f, "%.0f cm", 0)) touch();
            }
            igPopID();
        }
        igTextDisabled("%s", TR("On a Landscape, its Paint tool puts these where you brush (the rest stays this material)."));
    }
    if (sect(TR("Animation (a Time node)"), "animation time frames frame resolution animacao tempo quadros", m->animated)) {
        static const int fsizes[5] = { 128, 256, 512, 1024, 2048 };
        static const char* const fnames[5] = { "128", "256", "512", "1024", "2048" };
        int fcur = 2;
        for (int i = 0; i < 5; i++)
            if (fsizes[i] == m->anim_res) fcur = i;
        if (igSliderInt(PL(TR("Frames")), &m->anim_frames, 2, MAT_ANIM_MAX, "%d", ImGuiSliderFlags_AlwaysClamp)) touch();
        if (igCombo_Str_arr(PL(TR("Frame Resolution")), &fcur, fnames, 5, -1)) m->anim_res = fsizes[fcur], touch();
        igTextDisabled("%s", m->animated ? TR("The loop is baked into these frames; the game blends two of them (6 texture reads).")
                                         : TR("Add a Time node (Input) to animate the material."));
    }
}

static void details_node(MatGraph* g) {
    MatNode* n = &g->nodes[E.sel];
    const MatNodeDef* d = mat_node_def(n->type);
    if (!d) return;
    // the node: its name in its category's color, what it is, its label
    igTextColored(igColorConvertU32ToFloat4(mat_category_color(d->category)), "%s", L(d->name, d->label));
    igSameLine(0, 8);
    igTextDisabled("%s  |  %s", mat_category_name(d->category), d->idname);
    bool ch = false;
    igInputTextWithHint(PL(TR("Label")), TR("the title shown on the node"), n->label, sizeof n->label, 0, NULL, NULL);
    if (n->type == NODE_RGB || n->type == NODE_VALUE) {
        if (n->type == NODE_RGB) ch |= igColorEdit4(PL(TR("Color")), n->value, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        else ch |= igDragFloat(PL(TR("Value")), &n->value[0], 0.01f, 0, 0, "%.3f", 0);
        ch |= igInputTextWithHint(PL(TR("Parameter Name")), TR("empty: not a parameter"), n->param, sizeof n->param, 0, NULL, NULL);
        igSetItemTooltip("%s", TR("A name makes it a parameter: Material Instances and Blueprints change it."));
    }
    if (n->type == NODE_TEX_IMAGE || n->type == NODE_RE_INTERIOR) { // (Q4: the Interior Mapping's atlas)
        igSeparatorText(TR("Image"));
        ch |= image_field(n);
    }
    if (d->nprops > 0) igSeparatorText(TR("Settings"));
    for (int p = 0; p < d->nprops; p++) {
        const MatPropDef* pd = &d->props[p];
        igPushID_Int(100 + p);
        if (!pd->items) {
            bool b = n->prop[p] != 0;
            if (igCheckbox(pd->label, &b)) n->prop[p] = b, ch = true;
        } else if (igCombo_Str_arr(pd->label, &n->prop[p], (const char* const*)pd->items, pd->count, -1)) {
            ch = true;
        }
        igPopID();
    }
    if (n->type == NODE_COLOR_RAMP) {
        igSeparatorText(TR("Color Ramp"));
        for (int s = 0; s < n->ramp_count; s++) {
            igPushID_Int(200 + s);
            igSetNextItemWidth(igGetFontSize() * 4);
            ch |= igDragFloat("##pos", &n->ramp[s][0], 0.005f, 0.0f, 1.0f, "%.3f", 0);
            igSameLine(0, 4);
            ch |= igColorEdit4("##c", &n->ramp[s][1], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
            igSameLine(0, 4);
            if (n->ramp_count > 1 && igSmallButton("x")) {
                memmove(n->ramp[s], n->ramp[s + 1], sizeof(n->ramp[0]) * (size_t)(n->ramp_count - s - 1));
                n->ramp_count--, ch = true;
            }
            igPopID();
        }
        if (n->ramp_count < RAMP_MAX_STOPS && igSmallButton(TR("+ Stop"))) {
            float c[4];
            mat_ramp_eval(n, 0.5f, c);
            float st[5] = { 0.5f, c[0], c[1], c[2], c[3] };
            memcpy(n->ramp[n->ramp_count++], st, sizeof st);
            ch = true;
        }
        // kept sorted by position (the evaluation walks them in order)
        for (int a = 1; a < n->ramp_count; a++)
            for (int b = a; b > 0 && n->ramp[b][0] < n->ramp[b - 1][0]; b--) {
                float t[5];
                memcpy(t, n->ramp[b], sizeof t);
                memcpy(n->ramp[b], n->ramp[b - 1], sizeof t);
                memcpy(n->ramp[b - 1], t, sizeof t);
            }
    }
    if (d->nin > 0) igSeparatorText(TR("Inputs"));
    {
        char unused[512];
        int nu = mat_node_unused_inputs(g, E.sel, unused, sizeof unused);
        if (nu > 0) { // (the value is kept: it counts once the feature exists; notas-dev/materiais.md "Pinos sem efeito")
            igPushTextWrapPos(0.0f);
            igTextColored(THV(TK_WARN), TR("Set but without effect yet in this version: %s. The value is kept."), unused);
            igPopTextWrapPos();
        }
    }
    int go = -1; // (a linked input's source clicked: selected after the loop)
    for (int i = 0; i < d->nin; i++) {
        const MatSockDef* s = &d->in[i];
        const char* lb = L(s->name, s->label);
        if (g_dfind[0] && !find_match(s->name, g_dfind) && !find_match(lb, g_dfind)) continue;
        igPushID_Int(i);
        bool effect = mat_input_has_effect(n->type, i) || (mat_input_const_only(n->type, i) && mat_graph_input_link(g, E.sel, i) < 0);
        char lb_buf[160];
        if (!effect) snprintf(lb_buf, sizeof lb_buf, "%s (%s)", lb, TR("no effect yet")), lb = lb_buf;
        if (!effect) igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_N12));
        int li = mat_graph_input_link(g, E.sel, i);
        if (li >= 0) { // linked: where it comes from, a click goes there
            const MatLink* l = &g->links[li];
            const MatNodeDef* fd = mat_node_def(g->nodes[l->from].type);
            const MatNode* fn = &g->nodes[l->from];
            char from[200];
            snprintf(from, sizeof from, "%s  <-  %s%s%s##from%d", lb, fn->label[0] ? fn->label : fd ? L(fd->name, fd->label) : "?",
                     fd && fd->nout > 1 ? " : " : "", fd && fd->nout > 1 ? L(fd->out[l->from_sock].name, fd->out[l->from_sock].label) : "", i);
            if (igSelectable_Bool(from, false, 0, (ImVec2){ 0, 0 })) go = l->from;
            igSetItemTooltip("%s", TR("Linked: its value comes from that node. A click selects it; drag the link off the pin on the graph to free this input."));
        } else if (s->type == MSOCK_SHADER || (s->flags & MSF_HIDE_VALUE)) {
            igTextDisabled("%s: %s", lb, TR("link a node here"));
        } else if (s->type == MSOCK_FLOAT) {
            if (s->flags & MSF_FACTOR) ch |= igSliderFloat(lb, &n->in[i][0], s->min, s->max, "%.3f", 0);
            else ch |= igDragFloat(lb, &n->in[i][0], 0.01f, s->min, s->max, "%.3f", 0);
        } else if (s->type == MSOCK_COLOR) {
            ch |= igColorEdit4(lb, n->in[i], ImGuiColorEditFlags_Float);
        } else {
            ch |= igDragFloat3(lb, n->in[i], 0.01f, 0, 0, "%.3f", 0);
        }
        if (!effect) {
            igPopStyleColor(1);
            if (igIsItemHovered(ImGuiHoveredFlags_None))
                igSetTooltip("%s", TR("This input does not change the look yet (the renderer does not read it): its value is saved and will count once the feature exists."));
        }
        igPopID();
    }
    if (go >= 0) memset(E.msel, 0, sizeof E.msel), E.sel = go;
    if (ch) touch();
}

static void details(void) {
    Material* m = E.m;
    MatGraph* g = m->graph;
    bool node = E.sel >= 0 && E.sel < g->node_count;
    static int shown = -2; // (the selection the tabs last followed: a new one picks its tab)
    bool follow = shown != E.sel;
    shown = E.sel;
    igSetNextItemWidth(-1.0f);
    igInputTextWithHint("##dfind", TR("Search details"), g_dfind, sizeof g_dfind, 0, NULL, NULL);
    if (igBeginTabBar("##dtabs", 0)) {
        if (igBeginTabItem(TR("Material"), NULL, follow && !node ? ImGuiTabItemFlags_SetSelected : 0)) {
            details_material(m);
            igEndTabItem();
        }
        const MatNodeDef* d = node ? mat_node_def(g->nodes[E.sel].type) : NULL;
        char tab[160];
        if (d) snprintf(tab, sizeof tab, "%s: %s###node_tab", TR("Node"), g->nodes[E.sel].label[0] ? g->nodes[E.sel].label : L(d->name, d->label));
        else snprintf(tab, sizeof tab, "%s###node_tab", TR("Node"));
        if (igBeginTabItem(tab, NULL, follow && node ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (node) details_node(g);
            else {
                igPushTextWrapPos(0.0f);
                igTextDisabled("%s", TR("Click a node on the graph to edit it here. Shift+A or a right click adds one; a link dropped on empty space opens the nodes that take it."));
                igPopTextWrapPos();
            }
            igEndTabItem();
        }
        igEndTabBar();
    }
}

static void instance_panel(void) {
    Material* m = E.m;
    igText("%s: %s", TR("Parent"), m->parent ? m->parent->name : m->parent_path);
    if (!m->parent) {
        igTextColored(THV(TK_ERROR), "%s", TR("The parent material was not found."));
        return;
    }
    igSeparatorText(TR("Parameter Values"));
    if (!m->parent->nparams) igTextDisabled("%s", TR("The parent has no parameter: name an RGB or Value node in it."));
    bool ch = false;
    for (int i = 0; i < m->parent->nparams; i++) {
        const MatParam* pp = &m->parent->params[i];
        int k = -1;
        for (int j = 0; j < m->nparams; j++)
            if (!strcmp(m->params[j].name, pp->name)) k = j;
        igPushID_Int(i);
        bool over = k >= 0;
        if (igCheckbox("##ov", &over)) {
            if (over && m->nparams < MAT_MAX_PARAMS) {
                m->params[m->nparams] = *pp;
                m->nparams++;
            } else if (!over && k >= 0) {
                m->params[k] = m->params[--m->nparams];
            }
            ch = true;
            k = -1;
            for (int j = 0; j < m->nparams; j++)
                if (!strcmp(m->params[j].name, pp->name)) k = j;
        }
        igSetItemTooltip("%s", TR("Override the parent's value"));
        igSameLine(0, 6);
        float* v = k >= 0 ? m->params[k].value : (float*)pp->value;
        if (k < 0) igBeginDisabled(true);
        if (pp->kind) ch |= igColorEdit4(pp->name, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        else ch |= igDragFloat(pp->name, v, 0.01f, 0, 0, "%.3f", 0);
        if (k < 0) igEndDisabled();
        igPopID();
    }
    // (Q11) its own toon and outline values over its parent's
    igSeparatorText(TR("Toon and Outline"));
    if (igCheckbox(PL(TR("Override Toon and Outline")), &m->toon_own)) {
        if (m->toon_own) m->toon = m->parent->toon; // (starts from the parent's)
        ch = true;
    }
    if (m->toon_own) ch |= toon_ui_draw(&m->toon, m->parent->shading_model == MAT_SHADING_TOON);
    if (ch) {
        E.unsaved = true;
        mat_mark_dirty(m);
    }
}

// Unreal's preview: a sphere with the material (its baked base color, lit by a key light and the sky, a highlight from
// its mean roughness), turned by dragging; an animated material plays its frames
// (2026-11-23) The preview's ImGui textures of an old bake freed a few frames later: freed at once, a frame in flight
// still drew them (VUID-vkFreeDescriptorSets-pDescriptorSets-00309 while editing); waiting for the GPU instead (as the
// thumbnails do) stalled the editor at every bake, one per 0.12 s while dragging a value
#define TEX_GRAVE 64
static struct { VkDescriptorSet set; int at; } g_grave[TEX_GRAVE];
static int g_ngrave;
static void tex_grave_drain(bool all) {
    int now = igGetFrameCount(), k = 0;
    for (int i = 0; i < g_ngrave; i++)
        if (all || now - g_grave[i].at > 4) ImGui_ImplVulkan_RemoveTexture(g_grave[i].set);
        else g_grave[k++] = g_grave[i];
    g_ngrave = k;
}
static void tex_retire(VkDescriptorSet set) {
    if (!set) return;
    if (g_ngrave == TEX_GRAVE) { // (64 bakes within 4 frames: never; the oldest go)
        ImGui_ImplVulkan_RemoveTexture(g_grave[0].set);
        memmove(g_grave, g_grave + 1, sizeof g_grave[0] * (TEX_GRAVE - 1));
        g_ngrave--;
    }
    g_grave[g_ngrave].set = set, g_grave[g_ngrave++].at = igGetFrameCount();
}

static ImTextureID frame_tex(int which, uint32_t frame) {
    static VkDescriptorSet sets[2][MAT_ANIM_MAX];
    static VkImageView views[2][MAT_ANIM_MAX];
    VkImageView v = mat_preview_frame_view(E.m, which, frame);
    uint32_t k = frame % MAT_ANIM_MAX;
    VkDescriptorSet* set = &sets[which ? 1 : 0][k];
    VkImageView* view = &views[which ? 1 : 0][k];
    if (v != *view) {
        tex_retire(*set);
        *set = v ? ImGui_ImplVulkan_AddTexture(v, VK_IMAGE_LAYOUT_GENERAL) : VK_NULL_HANDLE;
        *view = v;
    }
    return (ImTextureID)(uintptr_t)*set;
}

// A lit sphere with a material's baked base color (and its glow, and a highlight from its roughness) in the square at o
// (size), turned by yaw / pitch; nu x nv quads (40 x 20 at most). The Material Editor's preview and the Content
// Browser's thumbnails (asset_viewer.c).
void mat_sphere_draw(ImDrawList* dl, ImVec2 o, float size, const Material* m, ImTextureID tex, ImTextureID emis, float yaw, float pitch,
                     int nu, int nv) {
    if (!tex || !m) return;
    float cy = cosf(yaw * 0.0174533f), sy = sinf(yaw * 0.0174533f), cp = cosf(pitch * 0.0174533f), sp = sinf(pitch * 0.0174533f);
    float L[3] = { -0.45f, -0.55f, 0.7f }, ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; k++) L[k] /= ll;
    float rough = 0.5f; // (the highlight's size: from the material's Principled roughness when it is a constant)
    for (int i = 0; m->graph && i < m->graph->node_count; i++)
        if (m->graph->nodes[i].type == NODE_BSDF_PRINCIPLED) rough = m->graph->nodes[i].in[2][0];
    float shin = 2.0f / fmaxf(rough * rough * rough * rough, 1e-3f) - 2.0f;
    if (nu > 40) nu = 40;
    if (nv > 20) nv = 20;
    ImVec2 pts[41 * 21];
    ImU32 cols[41 * 21], spec[41 * 21];
    float depth[41 * 21];
    float r = size * 0.42f, cx = o.x + size * 0.5f, cyy = o.y + size * 0.5f;
    for (int j = 0; j <= nv; j++)
        for (int i = 0; i <= nu; i++) {
            float lon = (float)i / nu * 6.2831853f, lat = (float)j / nv * 3.14159265f;
            float n[3] = { sinf(lat) * cosf(lon), sinf(lat) * sinf(lon), cosf(lat) };
            float a[3] = { n[0] * cy - n[1] * sy, n[0] * sy + n[1] * cy, n[2] }; // turned (yaw about z, pitch about x)
            float b[3] = { a[0], a[1] * cp - a[2] * sp, a[1] * sp + a[2] * cp };
            int k = j * (nu + 1) + i;
            pts[k] = (ImVec2){ cx + b[0] * r, cyy - b[2] * r };
            depth[k] = b[1]; // < 0: toward the viewer (it looks along +y)
            float nl = fmaxf(b[0] * L[0] + b[1] * L[1] + b[2] * L[2], 0.0f);
            float sky = 0.35f + 0.25f * b[2], lit = fminf(nl * 0.95f + sky, 1.0f);
            uint32_t g = (uint32_t)(lit * 255.0f);
            cols[k] = 0xFF000000u | g << 16 | g << 8 | g;
            float h[3] = { L[0], L[1] - 1.0f, L[2] }, hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            float nh = fmaxf((b[0] * h[0] + b[1] * h[1] + b[2] * h[2]) / hl, 0.0f);
            float s = nl > 0.0f ? powf(nh, shin) * (shin + 8.0f) / 60.0f : 0.0f;
            spec[k] = ((uint32_t)(fminf(s, 1.0f) * 200.0f) << 24) | 0x00FFFFFFu;
        }
    ImTextureRef_c ref = { NULL, tex };
    ImDrawList_PushTexture(dl, ref);
    for (int pass = 0; pass < 3; pass++) { // the textured, lit surface; its glow (emissive); the highlight (white, by alpha)
        if (pass == 1) {
            ImDrawList_PopTexture(dl);
            if (!emis) continue;
            ImDrawList_PushTexture(dl, (ImTextureRef_c){ NULL, emis });
        }
        if (pass == 2 && emis) ImDrawList_PopTexture(dl);
        ImVec2 white = igGetFontTexUvWhitePixel();
        for (int j = 0; j < nv; j++)
            for (int i = 0; i < nu; i++) {
                int q[4] = { j * (nu + 1) + i, j * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i };
                if (depth[q[0]] + depth[q[1]] + depth[q[2]] + depth[q[3]] > 0.2f) continue; // the far side
                ImDrawList_PrimReserve(dl, 6, 4);
                ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
                for (int c = 0; c < 4; c++) {
                    int ii = c == 1 || c == 2 ? i + 1 : i, jj = c >= 2 ? j + 1 : j;
                    ImVec2 uv = pass == 2 ? white : (ImVec2){ (float)ii / nu, (float)jj / nv };
                    ImDrawList_PrimWriteVtx(dl, pts[q[c]], uv, pass == 2 ? spec[q[c]] : pass == 1 ? TH(TK_N1) : cols[q[c]]);
                }
                ImDrawIdx id[6] = { 0, 1, 2, 0, 2, 3 };
                for (int c = 0; c < 6; c++) ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + id[c]));
            }
    }
}

// (Q7) the asset viewport: the material on a preview mesh (Unreal's sphere, cube, cylinder, plane or any Static Mesh)
// drawn by the GPU with its baked textures, in any view mode, the light and the camera of every asset editor
static AssetViewport g_mat_vp;
static uint32_t g_mat_mesh = SHAPE_SPHERE;
static void preview_view(float size) {
    ImVec2 o = igGetCursorScreenPos();
    float h = size * 0.9f;
    PreviewDraw d;
    static const float grey[3] = { 0.7f, 0.7f, 0.7f };
    avp_mesh_draw(&d, g_mat_mesh, E.m->hash, grey, 0.5f, 0.0f);
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.draws = &d, sc.ndraws = 1, sc.sel_bone = sc.sel_body = sc.sel_constraint = -1;
    avp_draw(&g_mat_vp, BP_PV_KEY_MATERIAL, o.x, o.y, size, h, &sc, NULL);
    igSetCursorScreenPos((ImVec2){ o.x, o.y + h + 4.0f });
    static const char* const shapes[SHAPE_COUNT] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
    uint32_t slot = render_mesh_slot(g_mat_mesh);
    igSetNextItemWidth(size * 0.6f);
    if (igBeginCombo(TR("Preview Mesh"), g_mat_mesh < SHAPE_COUNT ? TR(shapes[g_mat_mesh]) : render_mesh_name(slot), 0)) {
        for (uint32_t k = 0; k < SHAPE_COUNT; k++)
            if (igSelectable_Bool(TR(shapes[k]), g_mat_mesh == k, 0, (ImVec2){ 0, 0 })) g_mat_mesh = k, g_mat_vp.cam.framed = false;
        for (uint32_t sl = SHAPE_COUNT; sl < render_mesh_count(); sl++)
            if (asset_selectable_mesh(render_mesh_name(sl), slot == sl, render_mesh_id(sl))) g_mat_mesh = render_mesh_id(sl), g_mat_vp.cam.framed = false;
        igEndCombo();
    }
}

static void preview_sphere(float size) {
    static float yaw = -30.0f, pitch = 15.0f;
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos();
    igInvisibleButton("mat_sphere", (ImVec2){ size, size }, ImGuiButtonFlags_MouseButtonLeft); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    if (igIsItemActive()) {
        ImGuiIO* io = igGetIO_Nil();
        yaw += io->MouseDelta.x * 0.6f;
        pitch = fminf(fmaxf(pitch + io->MouseDelta.y * 0.6f, -85.0f), 85.0f);
    }
    igSetItemTooltip("%s", TR("Drag to turn it"));
    // the backdrop: a dark checker (alpha and dark materials read against it)
    float cs = size / 8.0f;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            ImDrawList_AddRectFilled(dl, (ImVec2){ o.x + x * cs, o.y + y * cs }, (ImVec2){ o.x + (x + 1) * cs, o.y + (y + 1) * cs },
                                     (x + y) & 1 ? TH(TK_N6) : TH(TK_N5), 0, 0);
    ImTextureID tex = (ImTextureID)(uintptr_t)E.preview[0], emis = (ImTextureID)(uintptr_t)E.preview[2];
    if (E.m->animated) { // the loop's frame now
        uint32_t nf = (uint32_t)(E.m->anim_frames > 1 ? E.m->anim_frames : 16);
        uint32_t fr = (uint32_t)(fmod(igGetTime() / (E.m->anim_period > 0.05f ? E.m->anim_period : 2.0f), 1.0) * nf) % nf;
        ImTextureID ft = frame_tex(0, fr), fe = frame_tex(2, fr);
        if (ft) tex = ft;
        if (fe) emis = fe;
    }
    mat_sphere_draw(dl, o, size, E.m, tex, emis, yaw, pitch, 40, 20);
}

static void preview(void) {
    for (int k = 0; k < 3; k++) {
        VkImageView v = mat_preview_view(E.m, k);
        if (v != E.preview_view[k]) { // a new bake: the old textures are gone
            tex_retire(E.preview[k]);
            E.preview[k] = v ? ImGui_ImplVulkan_AddTexture(v, VK_IMAGE_LAYOUT_GENERAL) : VK_NULL_HANDLE;
            E.preview_view[k] = v;
        }
    }
    float pv = fminf(igGetContentRegionAvail().x, igGetFontSize() * 19.0f);
    if (render_preview_available() && !getenv("REALENGINE_PREVIEW_CPU")) preview_view(pv); // (Q7)
    else preview_sphere(pv);
    if (E.m->animated) igTextDisabled(TR("Animated: %d frames of %.2f s (blended while it plays)"), E.m->anim_frames, E.m->anim_period);
    float s = igGetFontSize() * 4.5f;
    for (int k = 0; k < 2; k++) {
        if (!E.preview[k]) continue;
        if (k) igSameLine(0, 6);
        ImTextureRef_c ref = { NULL, (ImTextureID)(uintptr_t)E.preview[k] };
        igImage(ref, (ImVec2){ s, s }, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 });
        igSetItemTooltip("%s", k ? TR("Normal (xy), roughness, metallic") : TR("Base color"));
    }
}

struct GLFWwindow* g_mat_editor_window;

void mat_editor_draw(struct Editor* ed, double now) {
    (void)now;
    g_mat_editor_window = ed->window;
    mated_test_setup();
    tex_grave_drain(false);
    if (!E.open || !E.m) return;
    if (E.dirty_at > 0.0 && igGetTime() >= E.dirty_at) { // the batched edits: one bake
        mat_mark_dirty(E.m);
        E.dirty_at = 0.0;
    }
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 70, igGetFontSize() * 40 }, ImGuiCond_FirstUseEver);
    mated_test_window();
    char title[200];
    snprintf(title, sizeof title, "%s%s - %s###mat_editor", E.m->name, E.unsaved ? "*" : "",
             E.m->instance ? TR("Material Instance") : TR("Material Editor"));
    bool open = true;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            if (E.dirty_at > 0.0) mat_mark_dirty(E.m), E.dirty_at = 0.0;
            snprintf(E.status, sizeof E.status, "%s", mat_save(E.m) ? TR("Saved.") : TR("Could not save."));
            E.unsaved = false;
        }
        igSameLine(0, 10);
        if (E.m->error[0]) igTextColored(THV(TK_ERROR), "%s", E.m->error);
        else if (!E.m->instance)
            igTextDisabled(TR("%u instructions, %d parameters, baked %dx%d in %.1f ms: in game a pixel reads 2-3 textures"),
                           E.m->ninstr, E.m->nparams, E.m->resolution, E.m->resolution, E.m->bake_ms);
        if (E.status[0]) {
            igSameLine(0, 10);
            igTextDisabled("%s", E.status);
        }
        ImVec2 avail = igGetContentRegionAvail();
        static float side_w; // (the Details' width: the bar between it and the graph drags it, double-click: back)
        float side_def = igGetFontSize() * 20.0f;
        if (side_w <= 0.0f) side_w = side_def;
        float side = fminf(side_w, fmaxf(avail.x - 200.0f, 160.0f));
        if (E.m->instance) {
            igBeginChild_Str("mi", (ImVec2){ 0, 0 }, 0, 0);
            preview();
            instance_panel();
            igEndChild();
        } else {
            ImVec2 p = igGetCursorScreenPos();
            float cw = avail.x - side - UI_SPLIT;
            igBeginChild_Str("canvas", (ImVec2){ cw, avail.y }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            draw_canvas(p, (ImVec2){ cw, avail.y });
            igEndChild();
            igSameLine(0, 0);
            ui_splitter("##mat_split", true, &side_w, -1.0f, 160.0f, avail.x - 200.0f, avail.y, side_def);
            igSameLine(0, 0);
            igBeginChild_Str("details", (ImVec2){ side, avail.y }, ImGuiChildFlags_Borders, 0);
            preview();
            igSeparator();
            details();
            igEndChild();
        }
    }
    igEnd();
    if (!open) E.open = false;
}

bool mat_editor_focused(void) { return E.open && igIsWindowFocused(ImGuiFocusedFlags_AnyWindow) && E.m; }

void mat_editor_close(void) { E.open = false; }
