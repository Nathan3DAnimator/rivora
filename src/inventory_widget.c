// UMG's Inventory Grid (ui_widget.h UIW_INVENTORY, inventory.h; notas-dev/inventario.md): an actor's slots drawn as a
// grid (icon or a tile with the name's initials, the count), or its equipment slots; dragged and dropped (move, merge,
// swap, onto another grid: a chest; onto the equipment: equip; out of every grid: drop into the world), right click
// uses, double click equips / unequips, a tip under the mouse with the description. The input of a frame is read against
// the grids the frame before drew (their rectangles): nothing allocated, a few dozen rectangles.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "loc.h" // (2026-11-24) the items' names in the culture
#include "blueprint.h"
#include "brand.h"
#include "components.h"
#include "i18n.h"
#include "inventory.h"
#include "platform.h"
#include "ui_widget.h"

#define COL(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define MAX_GRIDS 16

typedef struct Grid { Entity owner; bool equip; float x0, y0, cell, sx, sy; int cols, n; } Grid; // (sx, sy: a cell to the next)
static struct {
    Grid g[2][MAX_GRIDS];
    int ng[2], cur;             // this frame's grids (drawn), the last frame's (the input's)
    World* w;
    float mouse[2];
    bool has_mouse;
    // a press on a slot: a drag once the mouse moved
    bool pending, dragging;
    Entity d_owner;
    bool d_equip;
    int d_slot;
    float d_start[2];
    // under the mouse (the tip)
    Entity h_owner;
    bool h_equip;
    int h_slot;
    double h_since;
    float k;
} W;

// The tests' mouse (inventory_test.c): used instead of the real one while on; a click is once
static struct { bool on, down, clicked, rclicked; float xy[2]; } T;
void inv_widget_test_input(bool on, float x, float y, bool down, bool clicked, bool rclicked) {
    T.on = on, T.xy[0] = x, T.xy[1] = y, T.down = down, T.clicked = clicked, T.rclicked = rclicked;
}
bool inv_widget_cell(Entity owner, bool equip, int slot, float xy[2]) { // (the grids the last frame drew)
    for (int i = 0; i < W.ng[W.cur]; i++) {
        const Grid* g = &W.g[W.cur][i];
        if (g->owner != owner || g->equip != equip || slot < 0 || slot >= g->n) continue;
        xy[0] = g->x0 + (float)(slot % g->cols) * g->sx + g->cell * 0.5f, xy[1] = g->y0 + (float)(slot / g->cols) * g->sy + g->cell * 0.5f;
        return true;
    }
    return false;
}

static ImU32 colf(const float c[4], float a) {
    float x[4];
    for (int i = 0; i < 4; i++) x[i] = fminf(fmaxf(c[i], 0.0f), 1.0f);
    return COL((int)(x[0] * 255), (int)(x[1] * 255), (int)(x[2] * 255), (int)(x[3] * a * 255));
}

// the grid's cell under (x, y): -1 none
static int cell_at(const Grid* g, float x, float y) {
    if (g->cols < 1 || g->cell <= 0.0f) return -1;
    float u = (x - g->x0) / g->sx, v = (y - g->y0) / g->sy;
    if (u < 0.0f || v < 0.0f) return -1;
    int c = (int)u, r = (int)v;
    if (c >= g->cols || (u - c) * g->sx > g->cell || (v - r) * g->sy > g->cell) return -1;
    int i = r * g->cols + c;
    return i < g->n ? i : -1;
}
static const Grid* hit(float x, float y, int* cell) {
    int p = W.cur ^ 1;
    for (int i = W.ng[p] - 1; i >= 0; i--) {
        int c = cell_at(&W.g[p][i], x, y);
        if (c >= 0) {
            *cell = c;
            return &W.g[p][i];
        }
    }
    *cell = -1;
    return NULL;
}
static uint32_t item_at(Entity owner, bool equip, int slot, int* count) {
    uint32_t it = 0;
    int n = 0;
    if (equip) it = inv_equipped(W.w, owner, slot), n = it ? 1 : 0;
    else inv_get_slot(W.w, owner, slot, &it, &n);
    if (count) *count = n;
    return it;
}

bool inv_widget_busy(void) { return W.pending || W.dragging; }

// The frame's input (ui_draw_viewport, before the trees): what the last frame's grids were under the mouse
void inv_widget_frame(World* w, const float mouse[2], bool down, bool clicked, bool rclicked, bool dclicked) {
    float tm[2];
    if (T.on) { // (the test's mouse)
        tm[0] = T.xy[0], tm[1] = T.xy[1], mouse = tm, down = T.down, clicked = T.clicked, rclicked = T.rclicked, dclicked = false;
        T.clicked = T.rclicked = false;
    }
    W.cur ^= 1;
    W.ng[W.cur] = 0;
    W.w = w;
    W.has_mouse = mouse != NULL;
    if (mouse) W.mouse[0] = mouse[0], W.mouse[1] = mouse[1];
    if (!w || !mouse) {
        W.pending = W.dragging = false;
        return;
    }
    int cell;
    const Grid* g = hit(mouse[0], mouse[1], &cell);
    Entity ho = g ? g->owner : ENTITY_NONE;
    if (ho != W.h_owner || (g && g->equip) != W.h_equip || cell != W.h_slot) W.h_since = re_time_seconds();
    W.h_owner = ho, W.h_equip = g && g->equip, W.h_slot = cell;
    if (clicked && g && item_at(g->owner, g->equip, cell, NULL)) {
        W.pending = true, W.dragging = false;
        W.d_owner = g->owner, W.d_equip = g->equip, W.d_slot = cell;
        W.d_start[0] = mouse[0], W.d_start[1] = mouse[1];
    }
    if (W.pending && down && !W.dragging && fabsf(mouse[0] - W.d_start[0]) + fabsf(mouse[1] - W.d_start[1]) > 6.0f) W.dragging = true;
    if (!down && W.pending) { // released: where it went
        if (W.dragging) {
            int count = 0;
            uint32_t item = item_at(W.d_owner, W.d_equip, W.d_slot, &count);
            if (g && !W.d_equip && !g->equip) { // slot -> slot: move / merge / swap, or into another inventory
                if (g->owner == W.d_owner) inv_move(w, W.d_owner, W.d_slot, cell);
                else inv_transfer(w, W.d_owner, W.d_slot, g->owner, cell);
            } else if (g && !W.d_equip && g->equip && g->owner == W.d_owner) inv_equip_slot(w, W.d_owner, W.d_slot);
            else if (g && W.d_equip && !g->equip && g->owner == W.d_owner) inv_unequip(w, W.d_owner, W.d_slot);
            else if (!g && item) { // out of every grid: into the world
                if (W.d_equip && inv_unequip(w, W.d_owner, W.d_slot)) count = 1;
                inv_drop(w, W.d_owner, item, count);
            }
        }
        W.pending = W.dragging = false;
    }
    if (g && rclicked && !g->equip) inv_use_slot(w, g->owner, cell);
    if (g && dclicked) {
        if (g->equip) inv_unequip(w, g->owner, cell);
        else inv_equip_slot(w, g->owner, cell);
        W.pending = W.dragging = false;
    }
}

static Entity owner_of(World* w, const char* name) { // the widget's actor: by its name, else the player
    if (!w) return ENTITY_NONE;
    if (!name || !name[0]) return bp_player(w);
    static char last[64];
    static Entity cached;
    if (cached && ecs_alive(w, cached) && !strcmp(last, name)) return cached;
    cached = ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count && !cached; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_INVENTORY);
        if ((a->mask & need) != need) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* e = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!_stricmp(n[i].text, name)) {
                cached = e[i];
                break;
            }
    }
    snprintf(last, sizeof last, "%s", name);
    return cached;
}

static void initials(const char* s, char out[4]) {
    int n = 0;
    bool start = true;
    for (; *s && n < 2; s++) {
        if (*s == ' ' || *s == '_' || *s == '-') start = true;
        else if (start) out[n++] = (char)((*s >= 'a' && *s <= 'z') ? *s - 32 : *s), start = false;
    }
    out[n] = 0;
}

// An item's picture in a rectangle: its icon, else a tile of its color with its initials
static void item_tile(ImDrawList* dl, const ItemDef* d, ImVec2 a, ImVec2 b, float k, float al) {
    float aspect = 1.0f;
    void* tex = d->icon[0] ? brand_ui_image(ui_content_root(), d->icon, &aspect) : NULL;
    if (tex) {
        ImTextureRef_c tr = { NULL, (ImTextureID)(uintptr_t)tex };
        ImDrawList_AddImageRounded(dl, tr, a, b, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 }, COL(255, 255, 255, (int)(255 * al)), 4.0f * k, 0);
        return;
    }
    float c[4] = { d->color[0] * 0.7f, d->color[1] * 0.7f, d->color[2] * 0.7f, 0.9f };
    ImDrawList_AddRectFilled(dl, a, b, colf(c, al), 5.0f * k, 0);
    char ini[4];
    initials(d->display, ini);
    ImFont* font = igGetFont();
    float size = (b.y - a.y) * 0.42f;
    ImVec2 ts = ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, ini, NULL, NULL);
    ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ (a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f }, COL(255, 255, 255, (int)(235 * al)), ini, NULL, 0.0f, NULL);
}

// The grid in its rectangle (the Designer: a preview with a few made-up items)
void inv_widget_draw(ImDrawList* dl, const UiWidget* x, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    World* w = designer ? NULL : W.w;
    Entity owner = owner_of(w, x->text);
    bool equip = x->orient == 1;
    int n = equip ? INV_EQ_COUNT : designer ? 20 : inv_slot_count(w, owner);
    if (n <= 0) {
        if (!designer) return;
        n = 20;
    }
    int cols = x->columns < 1 ? 1 : x->columns, rows = (n + cols - 1) / cols;
    ImFont* font = igGetFont();
    float fs = fmaxf(x->font_size * k, 8.0f), gap = x->padding * k, footer = (!equip && !x->checked) ? fs * 1.7f : 0.0f;
    float label = equip ? fs * 0.9f : 0.0f; // (the equipment: the slot's name over each)
    float cw = (x1 - x0 - gap * (cols - 1)) / cols, ch = (y1 - y0 - footer - gap * (rows - 1)) / rows - label;
    float cell = fmaxf(fminf(cw, ch), 4.0f);
    float sx = cell + gap, sy = cell + gap + label; // (the equipment's rows hold their label above)
    if (W.ng[W.cur] < MAX_GRIDS && !designer) {
        Grid* g = &W.g[W.cur][W.ng[W.cur]++];
        g->owner = owner, g->equip = equip, g->x0 = x0, g->y0 = y0 + label, g->cell = cell, g->sx = sx, g->sy = sy;
        g->cols = cols, g->n = n;
    }
    W.k = k;
    const float hover_c[4] = { x->hover[0], x->hover[1], x->hover[2], x->hover[3] > 0.0f ? x->hover[3] : 1.0f };
    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        ImVec2 a = { x0 + c * sx, y0 + label + r * sy };
        ImVec2 b = { a.x + cell, a.y + cell };
        bool hot = !designer && W.h_owner == owner && W.h_equip == equip && W.h_slot == i && owner;
        bool src = W.dragging && W.d_owner == owner && W.d_equip == equip && W.d_slot == i;
        ImDrawList_AddRectFilled(dl, a, b, colf(x->bg, al), x->radius * k, 0);
        if (x->border_w > 0.0f)
            ImDrawList_AddRect(dl, a, b, hot ? colf(hover_c, al) : colf(x->border_col, al), x->radius * k, fmaxf(x->border_w * k * (hot ? 2.0f : 1.0f), 1.0f), 0);
        if (equip) {
            const char* nm = TR(g_inv_equip_labels[i]);
            ImDrawList_AddText_FontPtr(dl, font, fs * 0.8f, (ImVec2){ a.x, a.y - label }, COL(170, 170, 178, (int)(220 * al)), nm, NULL, 0.0f, NULL);
        }
        int count = 0;
        uint32_t it = designer ? 0 : item_at(owner, equip, i, &count);
        const ItemDef* d = inv_item_by_hash(it);
        if (designer && i < 3 && !equip) { // (the preview: three made-up stacks)
            static ItemDef fake[3];
            static const char* const nm[3] = { "Health Potion", "Iron Sword", "Arrow" };
            static const float colr[3][3] = { { 0.9f, 0.2f, 0.25f }, { 0.6f, 0.65f, 0.75f }, { 0.75f, 0.55f, 0.3f } };
            snprintf(fake[i].display, sizeof fake[i].display, "%s", nm[i]);
            memcpy(fake[i].color, colr[i], 12);
            fake[i].icon[0] = 0;
            d = &fake[i], count = i == 0 ? 3 : i == 2 ? 24 : 1;
        }
        if (!d) continue;
        float pad = cell * 0.1f;
        item_tile(dl, d, (ImVec2){ a.x + pad, a.y + pad }, (ImVec2){ b.x - pad, b.y - pad }, k, al * (src ? 0.35f : 1.0f));
        if (count > 1) {
            char t[16];
            snprintf(t, sizeof t, "%d", count);
            ImVec2 ts = ImFont_CalcTextSizeA(font, fs, 1e9f, 0.0f, t, NULL, NULL);
            ImVec2 p = { b.x - ts.x - 4.0f * k, b.y - ts.y - 2.0f * k };
            ImDrawList_AddText_FontPtr(dl, font, fs, (ImVec2){ p.x + 1.0f * k, p.y + 1.0f * k }, COL(0, 0, 0, (int)(200 * al)), t, NULL, 0.0f, NULL);
            ImDrawList_AddText_FontPtr(dl, font, fs, p, colf(x->color, al), t, NULL, 0.0f, NULL);
        }
    }
    if (footer > 0.0f) { // the weight: carried / most
        float mx = 0.0f, wt = designer ? 4.5f : inv_weight(w, owner, &mx);
        if (designer) mx = 30.0f;
        char t[64];
        if (mx > 0.0f) snprintf(t, sizeof t, "%s  %.1f / %.0f kg", TR("Weight"), wt, mx);
        else snprintf(t, sizeof t, "%s  %.1f kg", TR("Weight"), wt);
        float fy = y0 + rows * sy + fs * 0.25f;
        ImDrawList_AddText_FontPtr(dl, font, fs, (ImVec2){ x0, fy }, colf(x->color, al * 0.85f), t, NULL, 0.0f, NULL);
        if (mx > 0.0f) { // (a thin bar: red near the limit)
            float bx0 = x0 + fs * 9.0f, bx1 = fminf(x1, x0 + cols * sx - gap), by = fy + fs * 0.35f, u = fminf(wt / mx, 1.0f);
            if (bx1 > bx0 + 10.0f) {
                ImDrawList_AddRectFilled(dl, (ImVec2){ bx0, by }, (ImVec2){ bx1, by + fs * 0.4f }, COL(255, 255, 255, (int)(30 * al)), fs * 0.2f, 0);
                ImU32 bc = u > 0.9f ? COL(230, 70, 60, (int)(230 * al)) : colf(hover_c, al * 0.9f);
                ImDrawList_AddRectFilled(dl, (ImVec2){ bx0, by }, (ImVec2){ bx0 + (bx1 - bx0) * u, by + fs * 0.4f }, bc, fs * 0.2f, 0);
            }
        }
    }
}

// Over everything (after the trees): the dragged item at the mouse, the tip of the one under it
void inv_widget_overlay(ImDrawList* dl) {
    if (!W.w || !W.has_mouse) return;
    float k = W.k > 0.0f ? W.k : 1.0f;
    ImFont* font = igGetFont();
    if (W.dragging) {
        int count = 0;
        const ItemDef* d = inv_item_by_hash(item_at(W.d_owner, W.d_equip, W.d_slot, &count));
        if (d) {
            float s = 64.0f * k;
            ImVec2 a = { W.mouse[0] - s * 0.5f, W.mouse[1] - s * 0.5f }, b = { a.x + s, a.y + s };
            item_tile(dl, d, a, b, k, 0.85f);
        }
        return;
    }
    if (!W.h_owner || W.h_slot < 0 || re_time_seconds() - W.h_since < 0.25) return;
    int count = 0;
    const ItemDef* d = inv_item_by_hash(item_at(W.h_owner, W.h_equip, W.h_slot, &count));
    if (!d) return;
    char info[160];
    int o = snprintf(info, sizeof info, "%s %.1f kg  -  %s %d", TR("Weight"), d->weight, TR("Value"), d->value);
    if (d->equip >= 0 && o > 0 && o < (int)sizeof info) snprintf(info + o, sizeof info - (size_t)o, "  -  %s", TR(g_inv_equip_labels[d->equip]));
    float fs = 19.0f * k, wrap = 380.0f * k;
    ImVec2 t1 = ImFont_CalcTextSizeA(font, fs * 1.1f, 1e9f, wrap, loc_item(d, false), NULL, NULL);
    ImVec2 t2 = loc_item(d, true)[0] ? ImFont_CalcTextSizeA(font, fs, 1e9f, wrap, loc_item(d, true), NULL, NULL) : (ImVec2){ 0, 0 };
    ImVec2 t3 = ImFont_CalcTextSizeA(font, fs * 0.85f, 1e9f, wrap, info, NULL, NULL);
    float wd = fmaxf(fmaxf(t1.x, t2.x), t3.x) + 20.0f * k, ht = t1.y + t2.y + t3.y + 26.0f * k;
    ImVec2 a = { W.mouse[0] + 18.0f * k, W.mouse[1] + 18.0f * k }, b = { a.x + wd, a.y + ht };
    ImDrawList_AddRectFilled(dl, a, b, COL(18, 18, 22, 240), 6.0f * k, 0);
    ImDrawList_AddRect(dl, a, b, COL(255, 255, 255, 50), 6.0f * k, 1.0f, 0);
    float y = a.y + 8.0f * k;
    ImDrawList_AddText_FontPtr(dl, font, fs * 1.1f, (ImVec2){ a.x + 10.0f * k, y }, COL(245, 245, 245, 255), loc_item(d, false), NULL, wrap, NULL);
    ImDrawList_AddText_FontPtr(dl, font, fs * 1.1f, (ImVec2){ a.x + 10.7f * k, y }, COL(245, 245, 245, 255), loc_item(d, false), NULL, wrap, NULL);
    y += t1.y + 4.0f * k;
    if (loc_item(d, true)[0]) {
        ImDrawList_AddText_FontPtr(dl, font, fs, (ImVec2){ a.x + 10.0f * k, y }, COL(205, 205, 210, 255), loc_item(d, true), NULL, wrap, NULL);
        y += t2.y + 4.0f * k;
    }
    ImDrawList_AddText_FontPtr(dl, font, fs * 0.85f, (ImVec2){ a.x + 10.0f * k, y }, COL(150, 175, 220, 255), info, NULL, wrap, NULL);
}
