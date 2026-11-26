// UMG's data and run time (ui_widget.h, notas-dev/umg.md): the designer tree, its JSON, the Widget Animations, the
// live widgets of Play (created, shown, bound, animated, their events). No ImGui here (re_render: the game too).
#include "loc.h" // (2026-11-24) ui_text_shown
#include "ui_widget.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "platform_files.h"
#include "io_json.h"
#include "audio.h"
#include "game_settings.h"
#include "save_progress.h"
#include "hud.h"

const char* const g_ui_kind_names[UIW_KIND_COUNT] = { "Canvas Panel", "Text", "Image", "Button", "Progress Bar", "Vertical Box",
                                                      "Horizontal Box", "Border", "Scroll Box", "Slider", "Check Box",
                                                      "Editable Text", "Combo Box (String)", "Spin Box", "Spacer", "Size Box",
                                                      "Overlay", "Uniform Grid Panel", "Wrap Box", "Throbber", "Circular Throbber",
                                                      "Virtual Joystick", "Touch Button", "Touch Look Area", "Inventory Grid", "Input Key Selector",
                                                      "Ability Slot", "Active Effects",
                                                      "Attribute Bar", "Compass", "Message Feed", "Damage Numbers", "Crosshair",
                                                      "Screen Markers", "Damage Direction", "Minimap", "Rich Text Block", "List View",
                                                      "Tile View" };
const char* const g_ui_prop_names[UIP_COUNT] = { "Render Opacity", "Translation X", "Translation Y", "Scale X", "Scale Y", "Angle",
                                                 "Color R", "Color G", "Color B", "Color A" };

#define UI_MAX_LIVE 32

static struct {
    char root[600];
    UiLive live[UI_MAX_LIVE];
    uint32_t nlive, order;
    bool mouse;
    UiStats stats;
    UiLive* creating; // (a widget whose Construct runs inside bp_spawn: its nodes find it before it knows its entity)
} U;

void ui_widgets_init(const char* content_root) { snprintf(U.root, sizeof U.root, "%s", content_root ? content_root : ""); }
const char* ui_content_root(void) { return U.root; }

// WBP_TouchControls (notas-dev/android.md): a movement joystick at the bottom left, a look area over the right half, Jump
// / Interact / Sprint at the bottom right, Pause at the top right: the keys the templates' Characters already use
uint32_t ui_touch_controls_asset(void) {
    const char* vp = "/Content/UI/WBP_TouchControls.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", U.root);
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_TouchControls", "User Widget");
    if (!a) return 0;
    a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    int look = ui_tree_add(t, UIW_TOUCH_AREA, 0);
    if (look >= 0) snprintf(t->w[look].name, UI_NAME, "LookArea");
    int joy = ui_tree_add(t, UIW_JOYSTICK, 0);
    if (joy >= 0) snprintf(t->w[joy].name, UI_NAME, "MoveStick");
    static const struct { const char* name; const char* key; const char* label; float x, y, size; float anchor[2]; } b[] = {
        { "JumpButton", "Space", "Jump", -90, -90, 170, { 1, 1 } },
        { "InteractButton", "E", "Use", -290, -70, 130, { 1, 1 } },
        { "SprintButton", "Shift", "Run", -110, -290, 130, { 1, 1 } },
        { "PauseButton", "Escape", "II", -40, 40, 90, { 1, 0 } },
    };
    for (int i = 0; i < 4; i++) {
        int k = ui_tree_add(t, UIW_TOUCH_BUTTON, 0);
        if (k < 0) break;
        UiWidget* w = &t->w[k];
        snprintf(w->name, UI_NAME, "%s", b[i].name);
        snprintf(w->text, sizeof w->text, "%s", b[i].key), snprintf(w->hint, sizeof w->hint, "%s", b[i].label);
        w->anchor[0] = w->anchor[2] = b[i].anchor[0], w->anchor[1] = w->anchor[3] = b[i].anchor[1];
        w->align[0] = b[i].anchor[0], w->align[1] = b[i].anchor[1];
        w->pos[0] = b[i].x, w->pos[1] = b[i].y, w->size[0] = w->size[1] = b[i].size;
        if (b[i].size < 120) w->font_size = 22.0f;
    }
    bp_compile(a);
    if (!bp_save(a)) return 0;
    return a->hash;
}

bool ui_is_container(uint8_t kind) {
    switch (kind) {
    case UIW_CANVAS: case UIW_VBOX: case UIW_HBOX: case UIW_BUTTON: case UIW_BORDER: case UIW_SCROLL: case UIW_SIZEBOX:
    case UIW_OVERLAY: case UIW_GRID: case UIW_WRAP:
        return true;
    default:
        return false;
    }
}

// The fields a widget made before 5.8's options had (an older file, a new widget) start as Unreal's defaults
static void widget_defaults_58(UiWidget* w) {
    w->vis_mode = UI_VIS_VISIBLE, w->enabled = 1;
    w->h_align = w->v_align = UI_ALIGN_FILL;
    w->fill = 1.0f;
    w->opacity = 1.0f;
    w->scale[0] = w->scale[1] = 1.0f;
    w->vmin = 0.0f, w->vmax = 1.0f, w->vstep = 0.0f;
    w->columns = 2;
}

void ui_tree_default(UiTree* t) {
    memset(t, 0, sizeof *t);
    UiWidget* r = &t->w[t->n++];
    snprintf(r->name, UI_NAME, "CanvasPanel");
    r->kind = UIW_CANVAS, r->visible = 1, r->parent = -1;
    r->anchor[2] = r->anchor[3] = 1.0f;
    widget_defaults_58(r);
}

int ui_tree_find(const UiTree* t, const char* name) {
    for (uint32_t i = 0; name && name[0] && i < t->n; i++)
        if (!strcmp(t->w[i].name, name)) return (int)i;
    return -1;
}

int ui_tree_add(UiTree* t, uint8_t kind, int parent) {
    if (t->n >= UI_MAX_WIDGETS || kind >= UIW_KIND_COUNT || parent < 0 || parent >= (int)t->n) return -1;
    UiWidget* w = &t->w[t->n];
    memset(w, 0, sizeof *w);
    static const char* const base[UIW_KIND_COUNT] = { "CanvasPanel", "Text", "Image", "Button", "ProgressBar", "VerticalBox",
                                                      "HorizontalBox", "Border", "ScrollBox", "Slider", "CheckBox", "EditableText",
                                                      "ComboBox", "SpinBox", "Spacer", "SizeBox", "Overlay", "UniformGrid", "WrapBox",
                                                      "Throbber", "CircularThrobber", "VirtualJoystick", "TouchButton", "TouchLookArea",
                                                      "InventoryGrid", "InputKeySelector", "AbilitySlot", "EffectList", "AttributeBar", "Compass", "MessageFeed",
                                                      "DamageNumbers", "Crosshair", "ScreenMarkers", "DamageDirection", "Minimap" };
    for (int k = 1; k < 1000; k++) { // Text_1, Text_2...
        snprintf(w->name, UI_NAME, "%s_%d", base[kind], k);
        if (ui_tree_find(t, w->name) < 0) break;
    }
    w->kind = kind, w->visible = 1, w->parent = (int16_t)parent;
    widget_defaults_58(w);
    w->pos[0] = w->pos[1] = 40.0f;
    static const float sizes[UIW_KIND_COUNT][2] = { { 600, 400 }, { 300, 40 }, { 128, 128 }, { 220, 60 }, { 400, 28 }, { 300, 300 },
                                                    { 400, 80 }, { 300, 200 }, { 360, 400 }, { 300, 32 }, { 36, 36 }, { 320, 44 },
                                                    { 320, 44 }, { 160, 40 }, { 40, 40 }, { 200, 200 }, { 300, 200 }, { 400, 300 },
                                                    { 400, 200 }, { 120, 24 }, { 64, 64 }, { 300, 300 }, { 150, 150 }, { 900, 900 },
                                                    { 560, 470 }, { 280, 50 }, { 72, 72 }, { 260, 160 }, { 420, 26 }, { 720, 52 }, { 460, 270 }, { 0, 0 },
                                                    { 120, 120 }, { 0, 0 }, { 440, 440 }, { 220, 220 } };
    w->size[0] = sizes[kind][0], w->size[1] = sizes[kind][1];
    w->color[0] = w->color[1] = w->color[2] = w->color[3] = 1.0f;
    w->font_size = 24.0f, w->padding = 4.0f, w->percent = 0.5f;
    // Unreal 5's look: dark fields, a blue accent, rounded corners
    static const float accent[4] = { 0.15f, 0.55f, 1.0f, 1.0f }, field[4] = { 0.06f, 0.06f, 0.07f, 0.92f };
    switch (kind) {
    case UIW_TEXT: snprintf(w->text, sizeof w->text, "Text Block"); break;
    case UIW_BUTTON:
        w->bg[0] = 0.12f, w->bg[1] = 0.12f, w->bg[2] = 0.13f, w->bg[3] = 0.9f;
        w->radius = 6.0f, w->border_w = 1.0f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.12f;
        break;
    case UIW_BORDER: w->bg[0] = w->bg[1] = w->bg[2] = 0.05f, w->bg[3] = 0.6f, w->radius = 4.0f; break;
    case UIW_PROGRESS: memcpy(w->color, accent, 16), w->bg[0] = w->bg[1] = w->bg[2] = 0.08f, w->bg[3] = 0.8f, w->radius = 3.0f; break;
    case UIW_SCROLL: w->bg[3] = 0.0f; break;
    case UIW_SLIDER: memcpy(w->color, accent, 16), memcpy(w->bg, field, 16), w->value = 0.5f, w->radius = 3.0f; break;
    case UIW_CHECKBOX: memcpy(w->color, accent, 16), memcpy(w->bg, field, 16), w->radius = 4.0f, w->border_w = 1.0f, w->border_col[3] = 0.25f, w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f; break;
    case UIW_EDIT:
    case UIW_COMBO:
    case UIW_SPIN:
        memcpy(w->bg, field, 16), w->radius = 4.0f, w->border_w = 1.0f, w->font_size = 22.0f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.18f;
        if (kind == UIW_EDIT) snprintf(w->hint, sizeof w->hint, "Type here...");
        if (kind == UIW_COMBO) snprintf(w->text, sizeof w->text, "Option 1|Option 2|Option 3");
        if (kind == UIW_SPIN) w->vmin = 0.0f, w->vmax = 100.0f, w->vstep = 1.0f, w->value = 0.0f;
        break;
    case UIW_OVERLAY: case UIW_SIZEBOX: case UIW_GRID: case UIW_WRAP: case UIW_SPACER: w->bg[3] = 0.0f; break;
    case UIW_THROBBER: case UIW_CIRCLE_THROBBER: memcpy(w->color, accent, 16); break;
    case UIW_RICH_TEXT: // (B7: Unreal's Rich Text Block)
        snprintf(w->text, sizeof w->text, "<b>Rich</b> text with <color=#FFB020>colors</color>, <i>italic</i> and <size=34>sizes</size>");
        w->size[0] = 520.0f, w->size[1] = 60.0f, w->wrap = 1, w->shadow = 1;
        break;
    case UIW_LIST_VIEW: case UIW_TILE_VIEW: // (B7: rows / tiles of items, the selected one in the accent)
        snprintf(w->text, sizeof w->text, "Item 1|Item 2|Item 3|Item 4|Item 5|Item 6|Item 7|Item 8");
        w->size[0] = kind == UIW_TILE_VIEW ? 420.0f : 320.0f, w->size[1] = 260.0f, w->font_size = 20.0f, w->value = -1.0f, w->vmin = -1.0f;
        w->vmax = 1e9f, w->bg[0] = w->bg[1] = w->bg[2] = 0.06f, w->bg[3] = 0.7f, w->radius = 4.0f, w->padding = 4.0f;
        memcpy(w->hover, accent, 16), w->hover[3] = 0.35f, w->columns = kind == UIW_TILE_VIEW ? 4 : 1;
        break;
    case UIW_JOYSTICK: // (bottom left; its knob the accent)
        snprintf(w->text, sizeof w->text, "Move"), memcpy(w->color, accent, 16);
        w->bg[0] = w->bg[1] = w->bg[2] = 1.0f, w->bg[3] = 0.16f, w->border_w = 2.0f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.35f;
        w->anchor[0] = w->anchor[2] = 0.0f, w->anchor[1] = w->anchor[3] = 1.0f, w->align[0] = 0.0f, w->align[1] = 1.0f, w->pos[0] = 80.0f, w->pos[1] = -80.0f;
        break;
    case UIW_TOUCH_BUTTON: // (bottom right)
        snprintf(w->text, sizeof w->text, "Space"), snprintf(w->hint, sizeof w->hint, "Jump");
        w->bg[0] = w->bg[1] = w->bg[2] = 1.0f, w->bg[3] = 0.18f, w->border_w = 2.0f, w->font_size = 26.0f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.4f;
        memcpy(w->press, accent, 16), w->press[3] = 0.6f;
        w->anchor[0] = w->anchor[2] = 1.0f, w->anchor[1] = w->anchor[3] = 1.0f, w->align[0] = 1.0f, w->align[1] = 1.0f, w->pos[0] = -90.0f, w->pos[1] = -90.0f;
        break;
    case UIW_ABILITY_SLOT: // (a dark square, the slot's number in its value; the key and the charges in white)
        w->size[0] = w->size[1] = 84.0f, w->radius = 8.0f, w->border_w = 1.0f, w->font_size = 16.0f;
        w->bg[0] = w->bg[1] = w->bg[2] = 0.06f, w->bg[3] = 0.85f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.22f;
        break;
    case UIW_EFFECT_LIST: // (rows of a dark strip, the buff's color on the left)
        w->size[0] = 380.0f, w->size[1] = 300.0f, w->font_size = 18.0f, w->padding = 4.0f, w->radius = 4.0f;
        w->bg[0] = w->bg[1] = w->bg[2] = 0.03f, w->bg[3] = 0.6f;
        break;
    case UIW_INVENTORY: // (the slots dark, the accent where the mouse is; 5 columns)
        w->columns = 5, w->font_size = 18.0f, w->radius = 6.0f, w->padding = 6.0f, w->border_w = 1.0f;
        w->bg[0] = w->bg[1] = w->bg[2] = 0.09f, w->bg[3] = 0.9f, memcpy(w->hover, accent, 16);
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.1f;
        break;
    case UIW_KEY_SELECTOR: // (a field like the Combo Box's: the key in the middle)
        memcpy(w->bg, field, 16), w->radius = 4.0f, w->border_w = 1.0f, w->font_size = 22.0f;
        w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.18f;
        snprintf(w->text, sizeof w->text, "Space"), snprintf(w->hint, sizeof w->hint, "None");
        break;
    case UIW_TOUCH_AREA: // (the right half of the screen, under the buttons)
        snprintf(w->text, sizeof w->text, "Look"), w->bg[3] = 0.0f, w->value = 0.25f; // value: degrees per pixel
        w->anchor[0] = 0.5f, w->anchor[1] = 0.0f, w->anchor[2] = 1.0f, w->anchor[3] = 1.0f, w->pos[0] = w->pos[1] = 0.0f, w->size[0] = w->size[1] = 0.0f;
        break;
    default:
        if (hud_is_kind(kind)) hud_widget_defaults(w); // (hud.c: its look and place)
        break;
    }
    return (int)t->n++;
}

void ui_tree_remove(UiTree* t, int i) {
    if (i <= 0 || i >= (int)t->n) return; // (the root stays)
    for (int c = (int)t->n - 1; c > i; c--)
        if (t->w[c].parent == i) ui_tree_remove(t, c);
    memmove(&t->w[i], &t->w[i + 1], sizeof t->w[0] * (t->n - (uint32_t)i - 1));
    t->n--;
    for (uint32_t c = 0; c < t->n; c++)
        if (t->w[c].parent > i) t->w[c].parent--;
}

// The subtree of i: i and every descendant (they follow it in the array: a parent comes before its children)
static int subtree_end(const UiTree* t, int i) {
    int e = i + 1;
    while (e < (int)t->n) {
        int p = t->w[e].parent;
        while (p > i) p = t->w[p].parent;
        if (p != i) break;
        e++;
    }
    return e;
}

bool ui_tree_move(UiTree* t, int i, int dir) {
    if (i <= 0 || i >= (int)t->n) return false;
    int parent = t->w[i].parent, j = -1;
    if (dir < 0) { // the previous sibling
        for (int k = i - 1; k > parent; k--)
            if (t->w[k].parent == parent) { j = k; break; }
    } else
        for (int k = subtree_end(t, i); k < (int)t->n; k++)
            if (t->w[k].parent == parent) { j = k; break; }
    if (j < 0) return false;
    int a = dir < 0 ? j : i, b = dir < 0 ? i : j; // swap the subtrees [a, ae) and [b, be): a before b
    int ae = subtree_end(t, a), be = subtree_end(t, b);
    int na = ae - a, nb = be - b;
    static UiWidget tmp[UI_MAX_WIDGETS]; // (an edit, not a frame)
    memcpy(tmp, &t->w[a], sizeof(UiWidget) * (size_t)(be - a));
    // new order: b's subtree, then a's; indices remapped
    int map[UI_MAX_WIDGETS];
    for (int k = 0; k < (int)t->n; k++) map[k] = k;
    for (int k = 0; k < nb; k++) map[b + k] = a + k;
    for (int k = 0; k < na; k++) map[a + k] = a + nb + k;
    for (int k = 0; k < nb; k++) t->w[a + k] = tmp[b - a + k];
    for (int k = 0; k < na; k++) t->w[a + nb + k] = tmp[k];
    for (int k = 0; k < (int)t->n; k++)
        if (t->w[k].parent >= 0) t->w[k].parent = (int16_t)map[t->w[k].parent];
    return true;
}

// --- Properties the animations drive ---

float ui_prop_get(const UiWidget* w, int prop) {
    switch (prop) {
    case UIP_OPACITY: return w->opacity;
    case UIP_X: return w->xlate[0];
    case UIP_Y: return w->xlate[1];
    case UIP_SCALE_X: return w->scale[0];
    case UIP_SCALE_Y: return w->scale[1];
    case UIP_ANGLE: return w->angle;
    case UIP_R: case UIP_G: case UIP_B: case UIP_A: return w->color[prop - UIP_R];
    default: return 0.0f;
    }
}
void ui_prop_set(UiWidget* w, int prop, float v) {
    switch (prop) {
    case UIP_OPACITY: w->opacity = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; break;
    case UIP_X: w->xlate[0] = v; break;
    case UIP_Y: w->xlate[1] = v; break;
    case UIP_SCALE_X: w->scale[0] = v; break;
    case UIP_SCALE_Y: w->scale[1] = v; break;
    case UIP_ANGLE: w->angle = v; break;
    case UIP_R: case UIP_G: case UIP_B: case UIP_A: w->color[prop - UIP_R] = v; break;
    default: break;
    }
}

int ui_anim_find(const UiTree* t, const char* name) {
    for (uint32_t i = 0; name && name[0] && i < t->nanims; i++)
        if (!strcmp(t->anims[i].name, name)) return (int)i;
    return -1;
}

void ui_track_key(UiWTrack* tr, float time, float value) {
    for (int k = 0; k < tr->nkeys; k++)
        if (fabsf(tr->t[k] - time) < 1e-3f) {
            tr->v[k] = value;
            return;
        }
    if (tr->nkeys >= UI_MAX_KEYS) return;
    int k = tr->nkeys++;
    while (k > 0 && tr->t[k - 1] > time) tr->t[k] = tr->t[k - 1], tr->v[k] = tr->v[k - 1], k--;
    tr->t[k] = time, tr->v[k] = value;
}

// A track's value at a time: its keys eased (Unreal's default cubic "auto" tangents, here smoothstep between keys)
static float track_eval(const UiWTrack* tr, float time) {
    if (!tr->nkeys) return 0.0f;
    if (time <= tr->t[0]) return tr->v[0];
    for (int k = 1; k < tr->nkeys; k++)
        if (time <= tr->t[k]) {
            float span = tr->t[k] - tr->t[k - 1], u = span > 1e-6f ? (time - tr->t[k - 1]) / span : 1.0f;
            u = u * u * (3.0f - 2.0f * u);
            return tr->v[k - 1] + (tr->v[k] - tr->v[k - 1]) * u;
        }
    return tr->v[tr->nkeys - 1];
}

void ui_anim_apply(UiTree* t, const UiWAnim* a, float time) {
    for (int i = 0; a && i < a->ntracks; i++) {
        int w = ui_tree_find(t, a->tracks[i].widget);
        if (w >= 0 && a->tracks[i].nkeys) ui_prop_set(&t->w[w], a->tracks[i].prop, track_eval(&a->tracks[i], time));
    }
}

// --- JSON ---

void ui_tree_write(const UiTree* t, JsonWriter* w) {
    jw_arr_begin(w);
    for (uint32_t i = 0; i < t->n; i++) {
        const UiWidget* x = &t->w[i];
        jw_obj_begin(w);
        jw_kstr(w, "name", x->name);
        jw_kint(w, "kind", x->kind);
        jw_kint(w, "parent", x->parent);
        jw_kbool(w, "visible", x->visible != 0);
        jw_kfloats(w, "anchor", x->anchor, 4);
        jw_kfloats(w, "pos", x->pos, 2);
        jw_kfloats(w, "size", x->size, 2);
        jw_kfloats(w, "align", x->align, 2);
        jw_kfloats(w, "color", x->color, 4);
        jw_kfloats(w, "bg", x->bg, 4);
        jw_kstr(w, "text", x->text);
        jw_kfloat(w, "font_size", x->font_size);
        jw_kfloat(w, "percent", x->percent);
        jw_kfloat(w, "padding", x->padding);
        jw_kstr(w, "image", x->image);
        jw_kstr(w, "bind_text", x->bind_text);
        jw_kstr(w, "bind_percent", x->bind_percent);
        // 5.8's options
        int flags[12] = { x->vis_mode, x->enabled, x->h_align, x->v_align, x->size_rule, x->bold, x->shadow, x->wrap, x->checked, x->columns, x->orient, x->rounded };
        jw_key(w, "flags");
        jw_arr_begin(w);
        for (int k = 0; k < 12; k++) jw_int(w, flags[k]);
        jw_arr_end(w);
        jw_kfloat(w, "fill", x->fill);
        jw_kfloats(w, "slot_pad", x->slot_pad, 4);
        jw_kfloat(w, "opacity", x->opacity);
        jw_kfloats(w, "xlate", x->xlate, 2);
        jw_kfloats(w, "scale", x->scale, 2);
        jw_kfloat(w, "angle", x->angle);
        jw_kfloat(w, "radius", x->radius);
        jw_kfloat(w, "border_w", x->border_w);
        jw_kfloats(w, "border_col", x->border_col, 4);
        jw_kfloats(w, "hover", x->hover, 4);
        jw_kfloats(w, "press", x->press, 4);
        float val[4] = { x->value, x->vmin, x->vmax, x->vstep };
        jw_kfloats(w, "value", val, 4);
        jw_kstr(w, "hint", x->hint);
        jw_kstr(w, "tooltip", x->tooltip);
        if (x->bind_setting[0]) jw_kstr(w, "bind_setting", x->bind_setting);
        if (x->font[0]) jw_kstr(w, "font", x->font);
        if (x->italic) jw_kint(w, "italic", x->italic);
        if (x->outline > 0.0f) jw_kfloat(w, "outline", x->outline), jw_kfloats(w, "outline_col", x->outline_col, 4);
        if (x->snd[0] || x->snd[1] || x->snd[2] || x->snd[3]) { // (its sounds: hashes)
            jw_key(w, "sounds");
            jw_arr_begin(w);
            for (int k = 0; k < UI_SND_COUNT; k++) jw_int(w, x->snd[k]);
            jw_arr_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

void ui_tree_read(UiTree* t, const JsonValue* v) {
    ui_tree_default(t);
    if (!json_len(v)) return;
    t->n = 0;
    for (size_t i = 0; i < json_len(v) && t->n < UI_MAX_WIDGETS; i++) {
        const JsonValue* o = json_at(v, i);
        UiWidget* x = &t->w[t->n++];
        memset(x, 0, sizeof *x);
        widget_defaults_58(x);
        snprintf(x->name, UI_NAME, "%s", json_get_str(o, "name", "Widget"));
        x->kind = (uint8_t)json_get_int(o, "kind", UIW_CANVAS);
        if (x->kind >= UIW_KIND_COUNT) x->kind = UIW_BORDER;
        x->parent = (int16_t)json_get_int(o, "parent", -1);
        if (i == 0) x->parent = -1, x->kind = UIW_CANVAS;
        else if (x->parent < 0 || x->parent >= (int)i) x->parent = 0;
        x->visible = json_get_bool(o, "visible", true);
        json_get_floats(o, "anchor", x->anchor, 4);
        json_get_floats(o, "pos", x->pos, 2);
        json_get_floats(o, "size", x->size, 2);
        json_get_floats(o, "align", x->align, 2);
        json_get_floats(o, "color", x->color, 4);
        json_get_floats(o, "bg", x->bg, 4);
        snprintf(x->text, sizeof x->text, "%s", json_get_str(o, "text", ""));
        x->font_size = (float)json_get_num(o, "font_size", 24.0);
        x->percent = (float)json_get_num(o, "percent", 0.5);
        x->padding = (float)json_get_num(o, "padding", 4.0);
        snprintf(x->image, sizeof x->image, "%s", json_get_str(o, "image", ""));
        snprintf(x->bind_text, UI_NAME, "%s", json_get_str(o, "bind_text", ""));
        snprintf(x->bind_percent, UI_NAME, "%s", json_get_str(o, "bind_percent", ""));
        const JsonValue* fl = json_get(o, "flags");
        if (fl) { // (5.8's options; a file from before keeps the defaults; a Button then centers its label as it did)
            uint8_t* dst[12] = { &x->vis_mode, &x->enabled, &x->h_align, &x->v_align, &x->size_rule, &x->bold, &x->shadow, &x->wrap,
                                 &x->checked, &x->columns, &x->orient, &x->rounded };
            for (size_t k = 0; k < 12 && k < json_len(fl); k++) *dst[k] = (uint8_t)json_num(json_at(fl, k), 0.0);
        } else if (x->kind == UIW_BUTTON) {
            x->radius = 6.0f, x->border_w = 1.0f;
            x->border_col[0] = x->border_col[1] = x->border_col[2] = 1.0f, x->border_col[3] = 0.12f;
        } else if (x->kind == UIW_BORDER) x->radius = 4.0f;
        else if (x->kind == UIW_PROGRESS) x->radius = 3.0f;
        else if (x->kind == UIW_TEXT) x->shadow = 1; // (the old Text drew a shadow)
        x->fill = (float)json_get_num(o, "fill", x->fill);
        json_get_floats(o, "slot_pad", x->slot_pad, 4);
        x->opacity = (float)json_get_num(o, "opacity", x->opacity);
        json_get_floats(o, "xlate", x->xlate, 2);
        json_get_floats(o, "scale", x->scale, 2);
        x->angle = (float)json_get_num(o, "angle", 0.0);
        x->radius = (float)json_get_num(o, "radius", x->radius);
        x->border_w = (float)json_get_num(o, "border_w", x->border_w);
        json_get_floats(o, "border_col", x->border_col, 4);
        json_get_floats(o, "hover", x->hover, 4);
        json_get_floats(o, "press", x->press, 4);
        float val[4] = { x->value, x->vmin, x->vmax, x->vstep };
        json_get_floats(o, "value", val, 4);
        x->value = val[0], x->vmin = val[1], x->vmax = val[2], x->vstep = val[3];
        snprintf(x->hint, sizeof x->hint, "%s", json_get_str(o, "hint", ""));
        snprintf(x->tooltip, sizeof x->tooltip, "%s", json_get_str(o, "tooltip", ""));
        snprintf(x->font, sizeof x->font, "%s", json_get_str(o, "font", ""));
        x->italic = (uint8_t)json_get_int(o, "italic", 0);
        x->outline = (float)json_get_num(o, "outline", 0.0);
        json_get_floats(o, "outline_col", x->outline_col, 4);
        snprintf(x->bind_setting, UI_NAME, "%s", json_get_str(o, "bind_setting", ""));
        const JsonValue* sn = json_get(o, "sounds");
        for (int k = 0; k < UI_SND_COUNT; k++) x->snd[k] = k < (int)json_len(sn) ? (uint32_t)json_num(json_at(sn, (size_t)k), 0.0) : 0u;
        if (x->columns < 1) x->columns = 1;
    }
}

void ui_anims_write(const UiTree* t, JsonWriter* w) {
    jw_arr_begin(w);
    for (uint32_t i = 0; i < t->nanims; i++) {
        const UiWAnim* a = &t->anims[i];
        jw_obj_begin(w);
        jw_kstr(w, "name", a->name);
        jw_kfloat(w, "length", a->length);
        jw_key(w, "tracks");
        jw_arr_begin(w);
        for (int k = 0; k < a->ntracks; k++) {
            const UiWTrack* tr = &a->tracks[k];
            jw_obj_begin(w);
            jw_kstr(w, "widget", tr->widget);
            jw_kint(w, "prop", tr->prop);
            jw_kfloats(w, "t", tr->t, tr->nkeys);
            jw_kfloats(w, "v", tr->v, tr->nkeys);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        if (a->nsnd) {
            jw_key(w, "sounds");
            jw_arr_begin(w);
            for (int k = 0; k < a->nsnd; k++) {
                jw_obj_begin(w);
                jw_kfloat(w, "t", a->snd_t[k]);
                jw_kint(w, "sound", a->snd[k]);
                jw_obj_end(w);
            }
            jw_arr_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

void ui_anims_read(UiTree* t, const JsonValue* v) {
    t->nanims = 0;
    for (size_t i = 0; v && i < json_len(v) && t->nanims < UI_MAX_ANIMS; i++) {
        const JsonValue* o = json_at(v, i);
        UiWAnim* a = &t->anims[t->nanims++];
        memset(a, 0, sizeof *a);
        snprintf(a->name, UI_NAME, "%s", json_get_str(o, "name", "Animation"));
        a->length = (float)json_get_num(o, "length", 1.0);
        const JsonValue* trs = json_get(o, "tracks");
        for (size_t k = 0; k < json_len(trs) && a->ntracks < UI_MAX_TRACKS; k++) {
            const JsonValue* to = json_at(trs, k);
            UiWTrack* tr = &a->tracks[a->ntracks++];
            memset(tr, 0, sizeof *tr);
            snprintf(tr->widget, UI_NAME, "%s", json_get_str(to, "widget", ""));
            tr->prop = (uint8_t)json_get_int(to, "prop", 0);
            if (tr->prop >= UIP_COUNT) tr->prop = 0;
            const JsonValue* ts = json_get(to, "t");
            tr->nkeys = (uint8_t)(json_len(ts) < UI_MAX_KEYS ? json_len(ts) : UI_MAX_KEYS);
            json_get_floats(to, "t", tr->t, tr->nkeys);
            json_get_floats(to, "v", tr->v, tr->nkeys);
        }
        const JsonValue* ss = json_get(o, "sounds");
        for (size_t k = 0; k < json_len(ss) && a->nsnd < 8; k++) {
            a->snd_t[a->nsnd] = (float)json_get_num(json_at(ss, k), "t", 0.0);
            a->snd[a->nsnd++] = (uint32_t)json_get_num(json_at(ss, k), "sound", 0.0);
        }
    }
}

// --- Run time ---

void ui_begin_play(World* w) {
    (void)w;
    U.nlive = 0, U.order = 0, U.mouse = false;
}
static void lists_free(void);
void ui_end_play(void) {
    U.nlive = 0, U.mouse = false;
    lists_free(); // (B7: the long lists of items)
}

static UiLive* live_of(Entity e) {
    for (uint32_t i = 0; i < U.nlive; i++)
        if (U.live[i].e == e) return &U.live[i];
    if (U.creating && U.creating->e == ENTITY_NONE && e != ENTITY_NONE) return U.creating->e = e, U.creating; // (its Construct)
    return NULL;
}

Entity ui_create_widget(World* w, BpAsset* cls) {
    if (!cls || bp_native_of(cls) != BPN_USER_WIDGET) return ENTITY_NONE;
    if (U.nlive >= UI_MAX_LIVE) { // (the oldest hidden one makes room)
        uint32_t k = 0;
        while (k < U.nlive && U.live[k].shown) k++;
        if (k == U.nlive) return ENTITY_NONE;
        U.live[k] = U.live[--U.nlive];
    }
    UiLive* l = &U.live[U.nlive];
    memset(l, 0, sizeof *l);
    l->cls = cls;
    if (cls->ui) l->t = *cls->ui;
    else ui_tree_default(&l->t);
    l->hot = l->focus = l->open_combo = l->nav = l->capture = -1;
    l->anim = -1;
    l->bind_serial = UINT32_MAX; // (its settings read on its first frame)
    U.nlive++;
    l->e = ENTITY_NONE;
    U.creating = l;
    // its instance: the class's entity (no place in the world), its BeginPlay (Unreal's Construct) now
    l->e = bp_spawn(w, cls, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
    U.creating = NULL;
    if (l->e == ENTITY_NONE) U.nlive--;
    return l->e;
}

void ui_add_to_viewport(Entity e, int z) {
    UiLive* l = live_of(e);
    if (l) l->shown = true, l->z = z, l->order = ++U.order;
}
void ui_remove_from_parent(Entity e) {
    UiLive* l = live_of(e);
    if (l) l->shown = false;
}
bool ui_is_shown(Entity e) {
    UiLive* l = live_of(e);
    return l && l->shown;
}

static UiWidget* widget_of(Entity e, const char* name) {
    UiLive* l = live_of(e);
    int i = l ? ui_tree_find(&l->t, name) : -1;
    return i >= 0 ? &l->t.w[i] : NULL;
}
// B7: a List View's / Tile View's items longer than its text (thousands of them): kept here by its live widget, freed
// when play ends (a load, not a frame)
typedef struct UiListStore { Entity e; int16_t w; char* s; } UiListStore;
static UiListStore g_lists[64];
static uint32_t g_nlists;
static UiListStore* list_store(Entity e, int w, bool make) {
    for (uint32_t i = 0; i < g_nlists; i++)
        if (g_lists[i].e == e && g_lists[i].w == w) return &g_lists[i];
    if (!make || g_nlists == 64) return NULL;
    g_lists[g_nlists] = (UiListStore){ e, (int16_t)w, NULL };
    return &g_lists[g_nlists++];
}
const char* ui_list_items(Entity e, int widget, const UiWidget* w) {
    UiListStore* s = list_store(e, widget, false);
    return s && s->s ? s->s : w->text;
}
static void lists_free(void) {
    for (uint32_t i = 0; i < g_nlists; i++) free(g_lists[i].s);
    g_nlists = 0;
}

void ui_set_text(Entity e, const char* widget, const char* text) {
    UiWidget* x = widget_of(e, widget);
    if (!x) return;
    if (x->kind == UIW_LIST_VIEW || x->kind == UIW_TILE_VIEW) { // (B7: the items; any length)
        const char* s = text ? text : "";
        UiListStore* ls = list_store(e, (int)(x - live_of(e)->t.w), strlen(s) >= sizeof x->text);
        if (ls) {
            free(ls->s);
            ls->s = strlen(s) >= sizeof x->text ? _strdup(s) : NULL;
        }
        snprintf(x->text, sizeof x->text, "%s", s);
        x->value = -1.0f, x->scroll = 0.0f; // (new items: nothing selected, back to the top)
        return;
    }
    snprintf(x->text, sizeof x->text, "%s", text ? text : "");
    x->bind_text[0] = 0; // (set: no longer bound)
    if (x->kind == UIW_BUTTON) // a Button: its first Text child
        for (uint32_t i = 0; i < live_of(e)->t.n; i++) {
            UiWidget* c = &live_of(e)->t.w[i];
            if (c->parent == x - live_of(e)->t.w && c->kind == UIW_TEXT) {
                snprintf(c->text, sizeof c->text, "%s", text ? text : "");
                c->bind_text[0] = 0;
                break;
            }
        }
}
void ui_set_percent(Entity e, const char* widget, float percent) {
    UiWidget* x = widget_of(e, widget);
    if (x) x->percent = percent < 0.0f ? 0.0f : percent > 1.0f ? 1.0f : percent, x->bind_percent[0] = 0;
}
void ui_set_visible(Entity e, const char* widget, bool visible) {
    UiWidget* x = widget_of(e, widget);
    if (x) x->visible = visible;
}
void ui_set_color(Entity e, const char* widget, const float color[4]) {
    UiWidget* x = widget_of(e, widget);
    if (x) memcpy(x->color, color, 16);
}
void ui_set_enabled(Entity e, const char* widget, bool enabled) {
    UiWidget* x = widget_of(e, widget);
    if (x) x->enabled = enabled;
}

// A Combo Box's options: the k-th of its text (separated by '|') into out
static bool combo_option(const char* text, int k, char* out, size_t cap) {
    const char* s = text;
    for (int i = 0; i < k && s; i++) {
        s = strchr(s, '|');
        if (s) s++;
    }
    if (!s || !*s) return out[0] = 0, false;
    const char* e = strchr(s, '|');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n), out[n] = 0;
    return true;
}
static int combo_count(const char* text) {
    if (!text[0]) return 0;
    int n = 1;
    for (const char* s = text; *s; s++) n += *s == '|';
    return n;
}

void ui_set_value(Entity e, const char* widget, float value) {
    UiWidget* x = widget_of(e, widget);
    if (!x) return;
    switch (x->kind) {
    case UIW_CHECKBOX: x->checked = value != 0.0f; break;
    case UIW_PROGRESS: x->percent = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value, x->bind_percent[0] = 0; break;
    case UIW_COMBO: {
        int n = combo_count(x->text), k = (int)value;
        x->value = (float)(k < 0 ? 0 : k >= n ? (n ? n - 1 : 0) : k);
        break;
    }
    default: x->value = value < x->vmin ? x->vmin : value > x->vmax && x->vmax > x->vmin ? x->vmax : value; break;
    }
}
float ui_get_value(Entity e, const char* widget) {
    const UiWidget* x = widget_of(e, widget);
    if (!x) return 0.0f;
    return x->kind == UIW_CHECKBOX ? (float)x->checked : x->kind == UIW_PROGRESS ? x->percent : x->value;
}
// (2026-11-24) the text a widget shows now (the localized one when the culture has it): the localization test
const char* ui_text_shown(Entity e, const char* widget) {
    UiLive* l = live_of(e);
    for (uint32_t i = 0; l && i < l->t.n; i++)
        if (!strcmp(l->t.w[i].name, widget)) {
            const char* t = loc_widget_text(l->cls, &l->t.w[i], (int)i);
            return t ? t : l->t.w[i].text;
        }
    return NULL;
}

const char* ui_get_text(Entity e, const char* widget) {
    const UiWidget* x = widget_of(e, widget);
    if (!x) return "";
    if (x->kind == UIW_COMBO) {
        static char opt[128];
        combo_option(x->text, (int)x->value, opt, sizeof opt);
        return opt;
    }
    if (x->kind == UIW_LIST_VIEW || x->kind == UIW_TILE_VIEW) { // (B7: the selected item; "": none)
        static char item[256];
        item[0] = 0;
        if (x->value >= 0.0f) combo_option(ui_list_items(e, (int)(x - live_of(e)->t.w), x), (int)x->value, item, sizeof item);
        return item;
    }
    return x->text;
}

void ui_play_anim(Entity e, const char* anim, int loops, float speed, bool reverse) {
    UiLive* l = live_of(e);
    int a = l ? ui_anim_find(&l->t, anim) : -1;
    if (a < 0) return;
    l->anim = (int8_t)a;
    l->anim_loops = loops < 0 ? 1 : loops;
    l->anim_speed = speed > 0.0f ? speed : 1.0f;
    l->anim_reverse = reverse;
    l->anim_t = reverse ? l->t.anims[a].length : 0.0f;
    ui_anim_apply(&l->t, &l->t.anims[a], l->anim_t);
}
void ui_stop_anim(Entity e, const char* anim) {
    UiLive* l = live_of(e);
    if (!l || l->anim < 0) return;
    if (!anim || !anim[0] || !strcmp(l->t.anims[l->anim].name, anim)) l->anim = -1; // (Unreal's: it stays where it was)
}

void ui_show_mouse(bool show) { U.mouse = show; }
bool ui_mouse_shown(void) { return U.mouse; }
bool ui_any_shown(void) {
    for (uint32_t i = 0; i < U.nlive; i++)
        if (U.live[i].shown) return true;
    return false;
}

// A widget bound to a Game User Setting shows it now (game_settings.h)
void ui_bind_read(UiWidget* x) {
    const char* s = x->bind_setting;
    if (save_menu_bind(x)) return; // (WBP_SaveSlots' rows: "Save.*", save_menu.c)
    float v, a, b, st;
    switch (x->kind) {
    case UIW_SLIDER:
    case UIW_SPIN:
        if (gus_slider_range(s, &a, &b, &st)) x->vmin = a, x->vmax = b, x->vstep = st;
        if (gus_get(s, &v, NULL, 0)) x->value = v;
        break;
    case UIW_CHECKBOX:
        if (gus_get(s, &v, NULL, 0)) x->checked = v != 0.0f;
        break;
    case UIW_COMBO:
        if (gus_options(s, x->text, sizeof x->text)) x->value = (float)gus_option_index(s);
        break;
    case UIW_KEY_SELECTOR: gus_get(s, NULL, x->text, sizeof x->text); break;
    case UIW_PROGRESS:
        if (gus_get(s, &v, NULL, 0)) x->percent = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        break;
    default: gus_value_text(s, x->text, sizeof x->text); break; // (a Text: "75%", "High", "Space")
    }
}

// The widget changed by the player: its setting set and applied (what needs the window waits for Apply Settings)
static void bind_write(UiWidget* x, const char* event) {
    const char* s = x->bind_setting;
    bool set = false;
    if (!strcmp(event, "OnValueChanged")) set = gus_set(s, x->value, NULL);
    else if (!strcmp(event, "OnCheckStateChanged")) set = gus_set(s, x->checked ? 1.0f : 0.0f, NULL);
    else if (!strcmp(event, "OnSelectionChanged")) set = gus_set_option(s, (int)x->value);
    else if (!strcmp(event, "OnKeySelected")) set = gus_set(s, 0.0f, x->text);
    if (set) gus_apply_live();
}

void ui_hide_class(uint32_t cls_hash) {
    for (uint32_t i = 0; i < U.nlive; i++)
        if (U.live[i].cls && U.live[i].cls->hash == cls_hash) U.live[i].shown = false, U.live[i].capture = -1;
}

bool ui_capturing_key(void) {
    for (uint32_t i = 0; i < U.nlive; i++)
        if (U.live[i].shown && U.live[i].capture >= 0) return true;
    return false;
}

bool ui_back(World* w, Entity skip) {
    UiLive* front = NULL;
    for (uint32_t i = 0; i < U.nlive; i++) { // the front one: the highest ZOrder, the last added
        UiLive* l = &U.live[i];
        if (!l->shown || l->e == skip || !ecs_alive(w, l->e)) continue;
        if (!front || l->z > front->z || (l->z == front->z && l->order > front->order)) front = l;
    }
    return front && bp_call_event(w, front->e, "OnBack");
}

static int live_cmp(const void* a, const void* b) {
    const UiLive *x = *(const UiLive* const*)a, *y = *(const UiLive* const*)b;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

// A live widget's animation one step on (its loops, its direction); applied to its tree
static void anim_step(UiLive* l, float dt) {
    if (l->anim < 0 || l->anim >= (int)l->t.nanims) return;
    const UiWAnim* a = &l->t.anims[l->anim];
    float len = a->length > 1e-3f ? a->length : 1e-3f;
    float t0 = l->anim_t;
    l->anim_t += (l->anim_reverse ? -dt : dt) * l->anim_speed;
    for (int k = 0; k < a->nsnd; k++) { // its sound keys passed (either way)
        float st = a->snd_t[k];
        bool hit = l->anim_reverse ? (st < t0 && st >= l->anim_t) : (st > t0 && st <= l->anim_t) || (t0 <= 0.0f && st <= 0.0f && l->anim_t > 0.0f);
        if (hit && a->snd[k]) {
            AudioPlay p;
            memset(&p, 0, sizeof p);
            p.sound = a->snd[k], p.ui = true, p.bone = -1, p.volume = 1.0f, p.pitch = 1.0f;
            audio_play(&p);
        }
    }
    bool done = false;
    if (l->anim_t > len || l->anim_t < 0.0f) {
        if (l->anim_loops == 1) done = true, l->anim_t = l->anim_t > len ? len : 0.0f;
        else {
            if (l->anim_loops > 1) l->anim_loops--;
            l->anim_t = l->anim_reverse ? l->anim_t + len : l->anim_t - len;
        }
    }
    ui_anim_apply(&l->t, a, l->anim_t);
    if (done) l->anim = -1;
}

uint32_t ui_live_list(World* w, UiLive** out, uint32_t cap, float dt) {
    uint32_t n = 0, widgets = 0;
    for (uint32_t i = 0; i < U.nlive; i++) {
        UiLive* l = &U.live[i];
        if (!ecs_alive(w, l->e)) { // (destroyed: gone)
            U.live[i--] = U.live[--U.nlive];
            continue;
        }
        if (!l->shown || l->hud_hidden || n >= cap) continue;
        uint32_t serial = gus_serial() + save_menu_serial() * 0x10001u; // (and the save slots' list: save_menu.c)
        if (l->bind_serial != serial) { // the Game User Settings it shows: read again when one changed
            l->bind_serial = serial;
            for (uint32_t k = 0; k < l->t.n; k++)
                if (l->t.w[k].bind_setting[0] && (int)k != l->pressed - 1 && (int)k != l->capture) ui_bind_read(&l->t.w[k]);
        }
        // the bindings: the widget's variables, read now
        for (uint32_t k = 0; k < l->t.n; k++) {
            UiWidget* x = &l->t.w[k];
            if (x->bind_text[0]) bp_instance_var_text(w, l->e, x->bind_text, x->text, sizeof x->text);
            BpValue v;
            if (x->bind_percent[0] && bp_instance_var(w, l->e, x->bind_percent, &v))
                x->percent = v.v[0] < 0.0f ? 0.0f : v.v[0] > 1.0f ? 1.0f : v.v[0];
        }
        anim_step(l, dt);
        widgets += l->t.n;
        out[n++] = l;
    }
    qsort(out, n, sizeof *out, live_cmp);
    U.stats.live = U.nlive, U.stats.shown = n, U.stats.widgets = widgets;
    return n;
}

static void (*g_native_event)(World* w, Entity e, const char* event, const char* widget);
void ui_set_native_events(void (*fn)(World* w, Entity e, const char* event, const char* widget)) { g_native_event = fn; }

void ui_event(World* w, UiLive* l, int widget, const char* event) {
    if (!l || widget < 0 || widget >= (int)l->t.n) return;
    { // the widget's sound for the event (a UI sound: heard while the game is paused)
        int s = !strcmp(event, "OnHovered") ? UI_SND_HOVERED : !strcmp(event, "OnPressed") ? UI_SND_PRESSED
              : !strcmp(event, "OnClicked") ? UI_SND_CLICKED
              : (!strcmp(event, "OnValueChanged") || !strcmp(event, "OnCheckStateChanged") || !strcmp(event, "OnSelectionChanged")) ? UI_SND_CHANGED : -1;
        uint32_t snd = s >= 0 ? l->t.w[widget].snd[s] : 0u;
        if (snd) {
            AudioPlay p;
            memset(&p, 0, sizeof p);
            p.sound = snd, p.ui = true, p.bone = -1, p.volume = 1.0f, p.pitch = 1.0f;
            audio_play(&p);
        }
    }
    if (l->t.w[widget].bind_setting[0] && !save_menu_event(w, l->e, &l->t.w[widget], event)) // (the slots' buttons: save_menu.c)
        bind_write(&l->t.w[widget], event); // (a widget bound to a Game User Setting)
    if (g_native_event) g_native_event(w, l->e, event, l->t.w[widget].name); // (a native game mode's: the RPG demo's shop)
    char ev[UI_NAME + 32];
    snprintf(ev, sizeof ev, "%s_%s", event, l->t.w[widget].name);
    bp_call_event(w, l->e, ev);
}

void ui_clicked(World* w, UiLive* l, int widget) { ui_event(w, l, widget, "OnClicked"); }

bool ui_test_set_text(const char* widget, const char* text) {
    for (uint32_t i = 0; i < U.nlive; i++)
        if (ui_tree_find(&U.live[i].t, widget) >= 0) {
            ui_set_text(U.live[i].e, widget, text);
            return true;
        }
    return false;
}

bool ui_test_click(World* w, const char* widget) {
    for (uint32_t i = 0; i < U.nlive; i++) {
        int k = ui_tree_find(&U.live[i].t, widget);
        if (k >= 0) {
            ui_clicked(w, &U.live[i], k);
            return true;
        }
    }
    return false;
}

const char* ui_test_text(const char* widget) {
    for (uint32_t i = 0; i < U.nlive; i++) {
        int k = ui_tree_find(&U.live[i].t, widget);
        if (k >= 0) return U.live[i].t.w[k].text;
    }
    return NULL;
}

const UiWidget* ui_test_widget(const char* widget) {
    for (uint32_t i = 0; i < U.nlive; i++) {
        int k = ui_tree_find(&U.live[i].t, widget);
        if (k >= 0) return &U.live[i].t.w[k];
    }
    return NULL;
}

UiStats* ui_stats(void) { return &U.stats; }

bool ui_test_shown(const char* widget) {
    for (uint32_t i = 0; i < U.nlive; i++) {
        int k = U.live[i].shown && !U.live[i].hud_hidden ? ui_tree_find(&U.live[i].t, widget) : -1;
        for (int p = k; p >= 0; p = U.live[i].t.w[p].parent) // (it and its parents: none collapsed)
            if (!U.live[i].t.w[p].visible || U.live[i].t.w[p].vis_mode == UI_VIS_COLLAPSED) k = -1;
        if (k >= 0) return true;
    }
    return false;
}

// (2026-11-25, item 21: the UI sweep in Play) the names of the buttons shown now, of every live widget tree
int ui_test_buttons(char (*names)[64], int cap) {
    int n = 0;
    for (uint32_t i = 0; i < U.nlive && n < cap; i++) {
        const UiLive* l = &U.live[i];
        if (!l->shown || l->hud_hidden) continue;
        for (uint32_t k = 0; k < l->t.n && n < cap; k++) {
            const UiWidget* x = &l->t.w[k];
            if (x->kind != UIW_BUTTON || !x->name[0] || !x->enabled) continue;
            bool vis = true;
            for (int p = (int)k; p >= 0 && vis; p = l->t.w[p].parent)
                vis = l->t.w[p].visible && l->t.w[p].vis_mode != UI_VIS_COLLAPSED;
            if (vis) snprintf(names[n++], 64, "%s", x->name);
        }
    }
    return n;
}

bool ui_test_bound(World* w, const char* setting, float value, const char* text) {
    for (uint32_t i = 0; i < U.nlive; i++) {
        UiLive* l = &U.live[i];
        if (!l->shown) continue;
        for (uint32_t k = 0; k < l->t.n; k++) {
            UiWidget* x = &l->t.w[k];
            if (strcmp(x->bind_setting, setting) || x->kind == UIW_TEXT) continue;
            const char* ev = NULL;
            switch (x->kind) {
            case UIW_SLIDER: case UIW_SPIN: x->value = value, ev = "OnValueChanged"; break;
            case UIW_CHECKBOX: x->checked = value != 0.0f, ev = "OnCheckStateChanged"; break;
            case UIW_COMBO: x->value = value, ev = "OnSelectionChanged"; break;
            case UIW_KEY_SELECTOR: snprintf(x->text, sizeof x->text, "%s", text ? text : ""), ev = "OnKeySelected"; break;
            default: break;
            }
            if (!ev) continue;
            ui_event(w, l, (int)k, ev);
            return true;
        }
    }
    return false;
}

// --- The HUD (hud.h) ---
UiWidget* ui_live_widget(Entity e, const char* name, Entity* owner) {
    for (uint32_t i = 0; name && name[0] && i < U.nlive; i++) {
        UiLive* l = &U.live[i];
        if (e ? l->e != e : !l->shown) continue;
        int k = ui_tree_find(&l->t, name);
        if (k < 0) continue;
        if (owner) *owner = l->e;
        return &l->t.w[k];
    }
    return NULL;
}
int ui_live_widget_index(Entity e, const UiWidget* x) {
    UiLive* l = live_of(e);
    return l && x >= l->t.w && x < l->t.w + l->t.n ? (int)(x - l->t.w) : -1;
}
void ui_set_live_hidden(Entity e, bool hidden) {
    UiLive* l = live_of(e);
    if (l) l->hud_hidden = hidden;
}
