// UMG (notas-dev/umg.md): Widget Blueprints, the game's UI as Unreal 5's makes it (sugestões #59 / #61). A Blueprint
// Class whose parent is "User Widget" has a designer tree (UiTree, stored in its .rbp) besides its Event Graph:
//   * widgets, Unreal's palette: Common (Border, Button, Check Box, Image, Progress Bar, Slider, Text, Spacer, Throbber,
//     Circular Throbber), Input (Combo Box, Editable Text, Spin Box), Panel (Canvas Panel, Horizontal / Vertical Box,
//     Overlay, Scroll Box, Size Box, Uniform Grid, Wrap Box); sizes in pixels at 1080p (scaled to the screen's height)
//   * every widget: its slot in its parent (a Canvas's anchors / offsets / alignment; a box's size rule Auto / Fill,
//     padding and alignment), Visibility (Visible, Collapsed, Hidden, Not Hit-Testable), Is Enabled, Render Opacity,
//     Render Transform (translation, scale, angle), Tool Tip; a style (corner radius, outline, hover / press colors)
//   * a Text's text and a Progress Bar's percent can be bound to a variable of the widget (read every frame)
//   * events: a Button's OnClicked / OnHovered / OnUnhovered / OnPressed / OnReleased, a Slider's or Spin Box's
//     OnValueChanged, a Check Box's OnCheckStateChanged, an Editable Text's OnTextChanged / OnTextCommitted, a Combo Box's
//     OnSelectionChanged: the widget's Custom Event "<Event>_<its name>" (the Designer makes it)
//   * Widget Animations (Unreal's UMG Timeline): tracks of a widget's opacity, translation, scale, angle or color, keys
//     in seconds; nodes Play Animation / Stop Animation
//   * nodes: Create Widget, Add to Viewport, Remove from Parent, Set Text / Percent / Visible / Color / Is Enabled /
//     Value, Get Value, Get Text (the widget by its name, in Details), Play / Stop Animation, Set Show Mouse Cursor
// ui_widget.c holds the data and the run time (no ImGui); ui_widget_draw.c draws it (the game, PIE, the designer).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

enum {
    UIW_CANVAS = 0, UIW_TEXT, UIW_IMAGE, UIW_BUTTON, UIW_PROGRESS, UIW_VBOX, UIW_HBOX, UIW_BORDER,
    // (5.8's other widgets, after the first ones: the files keep their numbers)
    UIW_SCROLL, UIW_SLIDER, UIW_CHECKBOX, UIW_EDIT, UIW_COMBO, UIW_SPIN, UIW_SPACER, UIW_SIZEBOX, UIW_OVERLAY, UIW_GRID,
    UIW_WRAP, UIW_THROBBER, UIW_CIRCLE_THROBBER,
    // Touch (notas-dev/android.md): a virtual joystick (its text: "Move" walks, "Look" turns the camera), a touch button
    // (its text: the key it holds - "Space", "E", "Left Mouse Button"...; its hint: the label shown), a look area (a drag
    // inside it turns the camera). Several fingers at once on a touch screen; the mouse on the PC
    UIW_JOYSTICK, UIW_TOUCH_BUTTON, UIW_TOUCH_AREA,
    // Inventory (inventory.h, notas-dev/inventario.md): the slots of an actor's Inventory (its text: the actor's name; "":
    // the player's), icon and count, dragged and dropped, a tip with the description; orient 1: the equipment slots
    UIW_INVENTORY,
    // Unreal's Input Key Selector (the options' Controls): a button showing a key (its text); clicked (or Enter / a gamepad's
    // A) it waits for the next key, mouse button or gamepad button (Esc cancels): OnKeySelected
    UIW_KEY_SELECTOR,
    // Abilities (ability.h, notas-dev/habilidades.md): an Ability Slot (value: the slot, 0 the first; its text: the actor's
    // name, "": the player's) with the icon, the cooldown's sweep, the charges, the key, clicked activates; the Active
    // Effects list (its text: the actor) with the stacks and the time left
    UIW_ABILITY_SLOT, UIW_EFFECT_LIST,
    // The HUD's pieces (hud.h, notas-dev/hud.md): an actor's attribute as a bar (text: the actor's name, "": the player;
    // hint: the attribute), the compass, the message feed, the floating damage numbers, the crosshair, the markers on the
    // screen and at its edge, the damage's direction, the minimap. Keep them together (hud_is_kind: ATTR_BAR..MINIMAP)
    UIW_ATTR_BAR, UIW_COMPASS, UIW_MESSAGES, UIW_DAMAGE_NUMBERS, UIW_CROSSHAIR, UIW_SCREEN_MARKERS, UIW_DAMAGE_DIR, UIW_MINIMAP,
    // B7 (notas-dev/umg.md "Fontes, Rich Text e listas"): Unreal's Rich Text Block (its text with <b>, <i>, <color=#RRGGBB>,
    // <size=N>, <outline> marks), List View and Tile View (their items: the text, '|' between them, or Set Text's of any
    // length; only the rows on the screen are drawn; value: the selected item, -1 none; OnItemClicked / OnSelectionChanged)
    UIW_RICH_TEXT, UIW_LIST_VIEW, UIW_TILE_VIEW,
    UIW_KIND_COUNT
};
// Visibility (Unreal's ESlateVisibility)
enum { UI_VIS_VISIBLE = 0, UI_VIS_COLLAPSED, UI_VIS_HIDDEN, UI_VIS_NOT_HIT, UI_VIS_SELF_NOT_HIT, UI_VIS_COUNT };
// A slot's alignment in a box, an overlay, a grid, a button
enum { UI_ALIGN_FILL = 0, UI_ALIGN_START, UI_ALIGN_CENTER, UI_ALIGN_END, UI_ALIGN_COUNT };
#define UI_MAX_WIDGETS 128
#define UI_NAME 48
#define UI_REF_HEIGHT 1080.0f // the designer's pixels: at this screen height, 1 px is 1 px

typedef struct UiWidget {
    char name[UI_NAME];
    uint8_t kind, visible;   // visible 0: collapsed (Set Widget Visible), whatever vis_mode says
    int16_t parent;          // -1: the root
    float anchor[4];         // min x, min y, max x, max y (0-1 of the parent); min == max: a point, else stretched
    float pos[2], size[2];   // px: the offset from the anchor and the size (stretched: the margins left/top, right/bottom)
    float align[2];          // the pivot (0-1 of its size) put on the anchor point; a Text's justification x / y
    float color[4], bg[4];   // text / fill / bar / tint; background (button, border, the bar's back, a field's box)
    char text[256];          // a Text's (a dialogue's line: 200 bytes); an Editable Text's; a Combo Box's options separated by '|'
    float font_size, percent, padding; // padding: a box's spacing, a Button's / Border's inner padding
    char image[256];         // an Image's texture ("/Content/...")
    char bind_text[UI_NAME], bind_percent[UI_NAME]; // the widget's variables they show ("": none)
    // --- 5.8's options (sugestão #59) ---
    uint8_t vis_mode, enabled, h_align, v_align, size_rule, bold, shadow, wrap, checked, columns, orient, rounded;
    float fill;              // the Fill size rule's weight
    float slot_pad[4];       // the slot's padding: left, top, right, bottom
    float opacity;           // Render Opacity
    float xlate[2], scale[2], angle; // Render Transform (px, factor, degrees)
    float radius, border_w;  // the corners (px) and the outline's width
    float border_col[4], hover[4], press[4]; // the outline; a Button's (Check Box's...) colors hovered / pressed (alpha 0: from bg)
    float value, vmin, vmax, vstep; // a Slider's / Spin Box's value and range; a Combo Box's selected index
    char hint[64];           // an Editable Text's hint text
    char tooltip[96];        // shown beside it after a moment under the mouse
    float scroll;            // a Scroll Box's offset (px, run time)
    // its sounds (audio.h, a Sound Wave's or Cue's hash; 0: none), played as UI sounds (heard while paused): Unreal's
    // Hovered Sound, Pressed Sound, the click (released on it), a value changed (checked, slid, chosen)
    uint32_t snd[4];
    // a Game User Setting it shows and changes (game_settings.h: "MasterVolume", "Scalability", "Key:IA_Jump"...): a
    // Slider's / Spin Box's value, a Check Box's state, a Combo Box's options and choice, a Text's value as text, a Key
    // Selector's key ("": none). Changed: set and applied at once (the window's settings: Apply Settings)
    char bind_setting[UI_NAME];
    // B7: its font (a TTF / OTF: "/Content/Fonts/Name.ttf", or a file's path; "": the engine's) - bold and italic from its
    // family's files (Name-Bold.ttf, Name-Italic.ttf, Name-BoldItalic.ttf; Windows' nameb / namei / namez) when there,
    // else made from the regular one -, italic, and an outline (px at 1080p) in its color
    char font[96];
    uint8_t italic;
    float outline, outline_col[4];
} UiWidget;
enum { UI_SND_HOVERED = 0, UI_SND_PRESSED, UI_SND_CLICKED, UI_SND_CHANGED, UI_SND_COUNT };

// --- Widget Animations (Unreal's UMG Timeline) ---
enum { UIP_OPACITY = 0, UIP_X, UIP_Y, UIP_SCALE_X, UIP_SCALE_Y, UIP_ANGLE, UIP_R, UIP_G, UIP_B, UIP_A, UIP_COUNT };
#define UI_MAX_ANIMS 8
#define UI_MAX_TRACKS 24
#define UI_MAX_KEYS 12
typedef struct UiWTrack {
    char widget[UI_NAME];
    uint8_t prop, nkeys;     // UIP_*, its keys (sorted by time)
    float t[UI_MAX_KEYS], v[UI_MAX_KEYS];
} UiWTrack;
typedef struct UiWAnim {
    char name[UI_NAME];
    float length;            // seconds (the last key's at least)
    uint8_t ntracks;
    UiWTrack tracks[UI_MAX_TRACKS];
    // its sound keys (a sound played when the animation passes its time; Unreal's Audio track in UMG)
    uint8_t nsnd;
    float snd_t[8];
    uint32_t snd[8];
} UiWAnim;

typedef struct UiTree {
    uint32_t n;
    UiWidget w[UI_MAX_WIDGETS]; // [0]: the root Canvas Panel
    uint32_t nanims;
    UiWAnim anims[UI_MAX_ANIMS];
} UiTree;

extern const char* const g_ui_kind_names[UIW_KIND_COUNT];
extern const char* const g_ui_prop_names[UIP_COUNT];

void ui_widgets_init(const char* content_root); // the Content folder (an Image's vpath -> its file)
const char* ui_content_root(void);
void ui_tree_default(UiTree* t);
int ui_tree_add(UiTree* t, uint8_t kind, int parent); // its index (-1: full); a unique name, sensible defaults
void ui_tree_remove(UiTree* t, int i);                 // with its children
bool ui_tree_move(UiTree* t, int i, int dir);          // among its siblings (-1 up, +1 down): the order boxes stack in
int ui_tree_find(const UiTree* t, const char* name);
bool ui_is_container(uint8_t kind);
float ui_prop_get(const UiWidget* w, int prop);
void ui_prop_set(UiWidget* w, int prop, float v);
void ui_anim_apply(UiTree* t, const UiWAnim* a, float time); // every track at that time (the keys interpolated)
int ui_anim_find(const UiTree* t, const char* name);
void ui_track_key(UiWTrack* tr, float time, float value); // a key there (replaced when one is already there)
struct JsonWriter;
struct JsonValue;
void ui_tree_write(const UiTree* t, struct JsonWriter* w);
void ui_tree_read(UiTree* t, const struct JsonValue* v);
void ui_anims_write(const UiTree* t, struct JsonWriter* w);
void ui_anims_read(UiTree* t, const struct JsonValue* v);

// --- Run time (Play) ---
struct BpAsset;
void ui_begin_play(World* w);
void ui_end_play(void);
Entity ui_create_widget(World* w, struct BpAsset* cls);
void ui_add_to_viewport(Entity e, int z);
void ui_remove_from_parent(Entity e);
bool ui_is_shown(Entity e); // on the screen (Add to Viewport, not removed since)
void ui_set_text(Entity e, const char* widget, const char* text);
void ui_set_percent(Entity e, const char* widget, float percent);
void ui_set_visible(Entity e, const char* widget, bool visible);
void ui_set_color(Entity e, const char* widget, const float color[4]);
void ui_set_enabled(Entity e, const char* widget, bool enabled);
void ui_set_value(Entity e, const char* widget, float value); // a Slider's / Spin Box's value, a Check Box's state, a Combo Box's index, a Progress Bar's percent
float ui_get_value(Entity e, const char* widget);
const char* ui_get_text(Entity e, const char* widget);        // a Text's / Editable Text's text, a Combo Box's selected option
const char* ui_text_shown(Entity e, const char* widget);     // (2026-11-24) what it shows now: localized (loc.h) or as it is
void ui_play_anim(Entity e, const char* anim, int loops, float speed, bool reverse); // loops 0: forever
void ui_stop_anim(Entity e, const char* anim);
void ui_show_mouse(bool show);
bool ui_any_shown(void); // a widget is on the screen (the game opens its ImGui frame for it)
bool ui_mouse_shown(void);
// The widgets on the screen, back to front (their trees as they are now: bindings read, animations applied)
typedef struct UiLive {
    Entity e;
    struct BpAsset* cls;
    UiTree t;
    bool shown;
    int z;
    uint32_t order;
    uint8_t pressed;       // a widget held (its index + 1)
    int16_t hot, focus, open_combo; // under the mouse; the Editable Text typed into; the Combo Box opened (-1: none)
    int8_t anim;           // the animation playing (-1: none)
    bool anim_reverse;
    int anim_loops;        // left (0: forever)
    float anim_t, anim_speed;
    uint32_t bind_serial;  // the settings' serial its bound widgets were read at (game_settings.h)
    int16_t nav;           // the widget the keyboard / gamepad is on (-1: none)
    int16_t capture;       // the Key Selector waiting for a key (-1: none)
    bool hud_hidden;       // hidden by the HUD (hud.h: Set HUD Visible, the photo mode, a Sequence's camera)
} UiLive;
uint32_t ui_live_list(World* w, UiLive** out, uint32_t cap, float dt); // bindings refreshed, animations advanced; by ZOrder
void ui_clicked(World* w, UiLive* l, int widget);           // a Button's click: its OnClicked event
void ui_event(World* w, UiLive* l, int widget, const char* event); // "<event>_<widget name>"
// The tests: a click on a widget by name (the first live widget that has it)
bool ui_test_click(World* w, const char* widget);
// A native game mode's widget events (demo_play.c: the RPG's shop): every event, before the Blueprint's (NULL: none)
void ui_set_native_events(void (*fn)(World* w, Entity e, const char* event, const char* widget));
// The touch controls (WBP_TouchControls, made when missing: Content/UI): their Widget Blueprint's hash (0: failed)
uint32_t ui_touch_controls_asset(void);
// The touch joysticks' and look areas' movement this frame (taken: the next starts at 0; bp_set_input adds it)
void ui_touch_axes(float move[2], float look[2]);
bool ui_touch_held(int id); // a finger a touch widget (joystick, touch button, look area) holds
int ui_touch_selftest(void); // the joystick, a button, the look area with fingers made up (failures; --touch-test)
const char* ui_test_text(const char* widget); // a live widget's text as shown (NULL: none)
const UiWidget* ui_test_widget(const char* widget); // a live widget as it is now (NULL: none)
typedef struct UiStats {
    uint32_t live, shown, widgets;
    double draw_ms;
    uint32_t fonts, list_items, list_rows, rich_runs; // (B7: the fonts loaded; the last list drawn: its items and the rows drawn; the rich texts' runs)
} UiStats;
// B7: a List View's / Tile View's items (Set Text's when longer than the widget's text: kept apart; else its text)
const char* ui_list_items(Entity e, int widget, const UiWidget* w);
bool ui_test_set_text(const char* widget, const char* text); // (the tests: a live widget's Set Text by its name)
UiStats* ui_stats(void);
// Pause and options (game_settings.h): a class's shown widgets hidden (the pause menu's Options with it); the front
// widget's OnBack event (Esc, a gamepad's B; skip: the pause menu itself) — false when none has one; a Key Selector waiting
void ui_hide_class(uint32_t cls_hash);
void ui_bind_read(UiWidget* x); // a widget bound to a Game User Setting: its value / options / text now
bool ui_back(World* w, Entity skip);
bool ui_capturing_key(void);
// The tests: the keyboard / gamepad on the front widget (dir: 0 up, 1 down, 2 left, 3 right, 4 accept, 5 back), a key
// given to a waiting Key Selector (its name)
void ui_test_nav(int dir);
void ui_test_key(const char* key);
bool ui_test_shown(const char* widget); // a widget of a shown tree (and itself not collapsed)
int ui_test_buttons(char (*names)[64], int cap); // (item 21, the UI sweep) the buttons shown now, by name
// The player changes the shown widget bound to a setting: a Slider's / Spin Box's value, a Check Box (value != 0), a
// Combo Box's choice (value: its index), a Key Selector's key (text); its event sent as the mouse's would (false: none)
bool ui_test_bound(World* w, const char* setting, float value, const char* text);

// --- Drawing (ui_widget_draw.c: the editor and the game) ---
struct ImDrawList;
// The live widgets over the rectangle (the game's view); mouse: its position, the left button down / clicked this frame
void ui_draw_viewport(World* w, struct ImDrawList* dl, float x0, float y0, float x1, float y1, const float mouse[2], bool down, bool clicked);
// The Health's floating damage numbers (health.h) over the view: the camera (place, axes, horizontal field of view) and
// the rectangle it is drawn in. Nothing when none is showing
void ui_draw_damage_numbers(struct ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg,
                            float x0, float y0, float x1, float y1);
// The Widget Blueprint's Designer tab (bp_editor.c): true when the tree changed
bool ui_designer_tab(struct BpAsset* a, float width, float height);
// A node's widget (Set Text ...): a combo of the tree's widgets of these kinds (mask of 1 << UIW_*; 0: any)
bool ui_widget_combo(const char* label, char* name, size_t cap, const struct BpAsset* a, uint64_t kinds); // (kinds: a bit per UIW_, 0 any)
// A node's Widget Animation (Play Animation ...): a combo of the tree's animations
bool ui_anim_combo(const char* label, char* name, size_t cap, const struct BpAsset* a);
// The HUD (hud.h): a live widget by name in the widget e (ENTITY_NONE: any shown one that has it; owner: whose it is);
// its index in its tree; a live widget hidden or shown by the HUD (not Remove from Parent: it comes back as it was)
UiWidget* ui_live_widget(Entity e, const char* name, Entity* owner);
int ui_live_widget_index(Entity e, const UiWidget* x);
void ui_set_live_hidden(Entity e, bool hidden);
