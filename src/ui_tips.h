// Descriptions (Unreal's tooltips): every button, menu item, check box, field and tab of the editor says what it does.
// The patched ImGui reports the hovered labeled widget (ItemAdd, RE_LABEL: imgui_hooks.c keeps it in g_ui_hover); after a
// moment still over it, ui_tips_frame() shows its description beside it (under it, or right of a menu), never over it.
// The texts: ui_tips_table.c, keyed by the widget's English label (the msgid), in each language.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct UiHover {
    char label[160];
    uint32_t id;
    int frame, tooltip_frame;          // ImGui's frame count when it was hovered / when a widget showed its own tooltip
    int window_flags;
    float x0, y0, x1, y1;              // the item
    float win_x0, win_x1;              // its window's x span (a menu's description goes beside it)
    float vp_x0, vp_y0, vp_x1, vp_y1;  // its viewport's work area
    // (Q5) the standard tooltip's parts, given by the widget (ui_tip_item) while hovered: its icon (-1: none), its
    // name (else the label's visible part), its description (else the table's) and its shortcut (else parsed from them)
    int icon, info_frame;
    char name[96], desc[512], shortcut[40];
} UiHover;
extern UiHover g_ui_hover;

typedef struct UiTip {
    const char* label; // the widget's label in English (the msgid), without its ## part; or its ## id
    const char* text[2]; // English, Português (Brasil)
} UiTip;

// Once per editor frame, after the UI and before igRender. (Q5) The standard tooltip: the icon, the control's NAME in bold,
// its shortcut on the right, the description under them and, in the editor, "F1: more in the Assistant"
void ui_tips_frame(void);
// (Q5) The editor's parts (the game links ui_tips.c without the icons): the icons' drawer, the bold font, the F1 line
struct ImDrawList;
struct ImFont;
typedef void (*UiTipIconFn)(struct ImDrawList* dl, int icon, float x, float y, float size, uint32_t col);
void ui_tips_editor(UiTipIconFn icon_fn, struct ImFont* const* bold); // (bold: the address of the font, reloaded with the scale)
// (Q5) The last item's tooltip parts (any NULL / -1: found as usual): an icon's button, a menu item, a field with its own
// text (in place of igSetItemTooltip, so the tooltip keeps the standard look). Cheap: copies only while hovered
void ui_tip_item(int icon, const char* name, const char* desc, const char* shortcut);
#define ui_item_tip(desc) ui_tip_item(-1, NULL, (desc), NULL)
// The name of the control whose tooltip shows now (the Assistant's F1 searches it), or NULL
const char* ui_tip_hovered_name(void);
// (Q5) A Details row's label (label_cell, PL): a field after it in its row without a description of its own takes the
// row's (its name and text). Recorded every frame (a pointer and a rectangle: no copy)
void ui_tip_row(const char* label);
// A label drawn as text (a Details row's name): its description when hovered, like a widget's. After the item.
void ui_tip_last_item(const char* label);
// Unreal's Details row: the property's name in a left column (with its description), the field filling the right one.
// Wrap a field's label: PL(TR("Intensity")) returns the hidden label to pass on. A field after SameLine or in a table
// keeps ImGui's layout (the label returned as it is).
const char* ui_prop_label(const char* label);
#define PL(label) ui_prop_label(label)
// The description of a label (English or translated, "##id" too), or NULL
const char* ui_tip_find(const char* label);
// For the tests: the table and its size
const UiTip* ui_tips_table(int* count);
// (2026-11-25, item 12) The Spanish descriptions (ui_tips_table_es.c, the editor's: label -> text), given at startup
typedef struct UiTipEs { const char* label; const char* text; } UiTipEs;
void ui_tips_set_es(const UiTipEs* t, int count);
const char* ui_tip_es(const char* label); // its Spanish text (NULL: none)
