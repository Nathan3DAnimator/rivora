// HUD modular (notas-dev/hud.md; módulo 10 do R7): the game's HUD made from reusable pieces, each a native UMG widget
// (ui_widget.h UIW_ATTR_BAR...) set up in the Designer and driven by Blueprints and scripts, made from scratch:
//   * Attribute Bar: an actor's attribute (Health, Shield or any other: a Blueprint's float variable, or a source another
//     module registers, hud_add_attribute_source) as a bar; its color, the loss smoothed, the chunk lost that lingers
//     and then drains, the gain in its own color, the numbers, segments, low pulse
//   * Compass: a strip with the cardinal points turning with the view and the compass markers (with their distance)
//   * Message Feed: a queue of messages ("+1 Coin", "Quest Complete") shown a few at a time, for a while, stacking the
//     same one ("+1 Coin" twice: "+2 Coin")
//   * Damage Numbers: the Health's floating numbers (health.h, one buffer: the damage, the shield, the heal in green and
//     the critical ones) drawn by the widget with its style; with no such widget on the screen they are drawn as before
//   * Crosshair: four lines and a dot opening with the spread (Set Crosshair Spread, or the player's speed)
//   * Screen Markers: an icon and a label over an actor; out of the view, an arrow at the screen's edge pointing to it
//   * Damage Direction: arcs around the middle toward who hurt the player, fading
//   * Minimap: the view from above around the player (the markers, the actors with Health), turning with the view
//   * HUD Layout (the component CHudLayout, on the player or any actor): the HUD widget (WBP_GameHUD unless another),
//     its scale by the resolution, the safe area (Android), hidden in the photo mode and while a Sequence has the camera,
//     the pieces shown; WBP_GameHUD (made when missing in /Content/UI) has every piece placed
// hud.c holds the data and the run time (no ImGui, re_render); hud_widget.c draws (the editor and the game).
// Nothing allocated: fixed arrays (256 markers, 32 messages, 16 hits, 64 bar states).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"

// --- Markers (compass, screen, minimap): an actor followed (or a fixed place); one per actor and kind ---
#define HUD_MARKERS_MAX 256
#define HUD_LABEL 32
enum {
    HUD_MK_COMPASS = 1u,   // on the Compass (and the Minimap)
    HUD_MK_SCREEN = 2u,    // over the actor on the screen
    HUD_MK_OFFSCREEN = 4u, // out of the view: at the screen's edge with an arrow (Screen markers)
    HUD_MK_DISTANCE = 8u,  // its distance shown ("35 m")
    HUD_MK_MINIMAP = 16u,  // on the Minimap
};
typedef struct HudMarker {
    Entity actor;          // ENTITY_NONE: the fixed place pos
    float pos[3];          // where it is (the actor's, read every frame; + offset_z over it)
    float offset_z;        // cm over the actor's origin
    float color[4];
    char label[HUD_LABEL];
    uint32_t flags;        // HUD_MK_*
    uint32_t id;           // (0: free)
    float dist;            // (the frame's) cm from the view
} HudMarker;
// A marker on the actor (or at pos when actor is ENTITY_NONE; NULL: none): the same actor's marker of the same kind
// (Compass / Screen) is replaced. Its id (0: full or nothing given)
uint32_t hud_add_marker(World* w, Entity actor, const float pos[3], const char* label, const float color[4], float offset_z, uint32_t flags);
// The actor's markers of these kinds (HUD_MK_COMPASS | HUD_MK_SCREEN...; 0: every kind) removed; how many
uint32_t hud_remove_marker(Entity actor, uint32_t kinds);
bool hud_remove_marker_id(uint32_t id);
void hud_clear_markers(void);
const HudMarker* hud_markers(uint32_t* count); // (every slot: id 0 is free)
uint32_t hud_marker_count(uint32_t kinds);      // the live ones of these kinds (0: any)

// --- Messages (the Message Feed) ---
#define HUD_MSG_MAX 32
#define HUD_MSG_TEXT 96
typedef struct HudMessage {
    char text[HUD_MSG_TEXT]; // as shown ("+2 Coin")
    float color[4];
    float life, age;         // seconds it shows; its age (< 0: waiting in the queue)
    int32_t count;           // "+N rest" summed (the number) / the same text stacked (x2): how many times
    uint32_t serial;         // (the order it came in)
} HudMessage;
// A message to the feed: shown when there is room (the feed's Max Shown), for duration s (<= 0: the feed's); the same
// text already showing stacks ("+1 Coin" + "+1 Coin" = "+2 Coin", "Saved" twice: "Saved x2") and lives on. color NULL:
// white. False: the queue is full
bool hud_message(const char* text, float duration, const float color[4]);
const HudMessage* hud_messages(uint32_t* count); // (every slot: text "" is free)
uint32_t hud_message_count(bool shown_only);
void hud_clear_messages(void);

// --- Damage numbers (health.h's buffer: the same numbers the Health makes) ---
// A number at pos: heal in green ("+25"), critical bigger in its color with "!"; shield: the shield's blue
void hud_damage_number(const float pos[3], float amount, bool critical, bool heal);

// --- Crosshair ---
void hud_set_spread(float spread); // 0..1: how open (it eases there); the Crosshair widget adds the player's speed
float hud_spread(void);            // as shown now (eased)

// --- The damage's direction (the player hit: health.c calls it for every damage) ---
#define HUD_HITS_MAX 16
typedef struct HudHit { float from[3]; float age, amount; } HudHit;
void hud_on_damage(World* w, Entity victim, Entity instigator, Entity causer, float amount);
const HudHit* hud_hits(uint32_t* count); // (age > 1.5 s: gone)

// --- Visibility ---
void hud_set_visible(bool visible); // Set HUD Visible: the HUD Layout's widget (and every HUD piece) shown or hidden
bool hud_visible(void);             // what Set HUD Visible said
bool hud_shown(void);               // shown now (not hidden by Set HUD Visible, the photo mode or a Sequence's camera)
void hud_set_photo_mode(bool on);   // the photo mode: the HUD hides (HUD Layout's Hide in Photo Mode)
bool hud_photo_mode(void);

// --- Attribute bars ---
// A source of attributes (a module's: stamina, mana...): true when it knows the actor's attribute (its value and max)
typedef bool (*HudAttributeFn)(World* w, Entity e, const char* attribute, float* value, float* max);
void hud_add_attribute_source(HudAttributeFn fn); // (up to 8)
// The actor's attribute: "Health", "Shield" (its Health), a registered source's, a Blueprint float variable (its max: the
// variable "Max<Name>" or "<Name>Max", else the bar's Max). False: none
bool hud_attribute(World* w, Entity e, const char* attribute, float* value, float* max);
// Bind Attribute Bar: the Attribute Bar named bar in the widget (ENTITY_NONE: any shown widget that has it) shows the
// actor's attribute (actor ENTITY_NONE: the player). False: no such bar
bool hud_bind_bar(World* w, Entity widget, const char* bar, Entity actor, const char* attribute);

// --- Play (bp_vm.c) ---
void hud_begin_play(World* w);
void hud_after_begin_play(World* w, Entity player); // the HUD Layouts' widgets made and shown (after BeginPlay)
void hud_tick(World* w, float dt, Entity player);   // the markers' places, the messages' ages, the spread, the hiding
void hud_end_play(void);
Entity hud_player(void);                            // (the player the HUD follows; the last hud_tick's)
Entity hud_layout_widget(void);                     // the HUD Layout's widget (ENTITY_NONE: none)

// --- The view (editor_viewport.c, game_main.c: before the widgets are drawn) ---
void hud_set_view(const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg);
typedef struct HudView { float pos[3], fwd[3], right[3], up[3], fov_deg; bool set; } HudView;
const HudView* hud_view(void);
// The point on the screen rect (x0..x1, y0..y1): false behind the view (xy: where it points, mirrored)
bool hud_project(const float p[3], float x0, float y0, float x1, float y1, float xy[2], float* depth);

// --- The widgets (ui_widget.c / ui_widget_draw.c) ---
bool hud_is_kind(uint8_t kind);        // one of the HUD pieces
struct UiWidget;
void hud_widget_defaults(struct UiWidget* w); // a new piece's look (ui_tree_add)
// An Attribute Bar's state now (smoothed, the trail): its fractions for drawing; created on first use
typedef struct HudBarState { Entity owner; int16_t widget; uint8_t used, pad; float shown, trail, gain, timer, value, max, target; uint32_t frame; } HudBarState;
HudBarState* hud_bar_state(Entity owner, int widget, const struct UiWidget* w, float dt);
// The HUD layout's rectangle and scale for its widget (ui_widget_draw.c): the safe area cut, the scale (k: UMG's)
bool hud_layout_rect(Entity live, float rect[4], float* k);
bool hud_piece_hidden(Entity live, uint8_t kind); // the HUD Layout hides this kind of piece
void hud_numbers_drawn(void);     // (a Damage Numbers widget drew this frame)
void hud_feed_config(uint32_t max_shown, float life); // (a Message Feed drawn: its Max Shown and Duration for the queue)
World* hud_world(void);           // (the playing world, the last hud_tick's; NULL: not playing)
float hud_dt(void);
bool hud_numbers_take_drawn(void); // (ui_draw_damage_numbers: the widget drew them already; cleared)

// The compass's cardinal labels in the game's language (8: N, NE, E... / N, NE, L... / N, NE, E...)
const char* hud_cardinal(int i);
// The yaw (degrees, 0 = north = +X, clockwise as seen from above: east 90) of a horizontal direction
float hud_bearing(const float d[3]);

// --- The HUD Layout component ---
uint32_t hud_layout_asset(void); // WBP_GameHUD in /Content/UI (made when missing); its hash (0: failed)
extern const char* const g_hud_scale_modes[HUD_SCALE_COUNT];

typedef struct HudStats {
    uint32_t markers, messages, queued, numbers, bars, hits; // the last frame's
    uint32_t shown_messages, stacked;                       // since play started
    double tick_ms;                                         // the last hud_tick
    double draw_ms;                                         // the last frame's HUD pieces drawn (CPU)
    uint32_t draw_vtx, draw_idx;                            // their vertices / indices the last frame (the GPU's work)
    uint32_t pieces;                                        // drawn the last frame
    double ui_gpu_ms;                                       // the whole UI pass on the GPU (the editor's; the HUD is part of it)
    double kind_ms[8];                                      // the last frame's drawing by kind of piece (UIW_ATTR_BAR + i)
    uint32_t kind_vtx[8];
} HudStats;
HudStats* hud_stats(void);
void hud_draw_account(uint8_t kind, double ms, uint32_t vtx, uint32_t idx); // (hud_widget.c: a piece drawn)
void hud_frame_begin(void);
void hud_set_ui_gpu_ms(double ms); // (main.c: the UI pass's GPU timestamps, a frame late)       // (ui_draw_viewport: the frame's draw counters to the stats)
double hud_ui_gpu_ms(void);   // (B14b: the trace's GPU track)
void hud_register_debug(void);    // its Debug Viewer list ("HUD")

// The self-test (--hud-test, before the play part in hud_test.c): markers (replace, remove, full), the feed (queue,
// stacking, "+N" summed, life), the bar's smoothing and trail, the bearing and the cardinal points, the projection and
// the edge, the damage's direction, the attributes. Failures
int hud_selftest(void);

// --- The drawing (hud_widget.c) ---
struct ImDrawList;
// A HUD piece into its rectangle (ui_widget_draw.c's draw_tree; owner: the live widget, ENTITY_NONE in the Designer)
void hud_widget_draw(struct ImDrawList* dl, const struct UiWidget* w, Entity owner, int index, float x0, float y0, float x1, float y1, float k,
                     float alpha, bool designer, double now);
// The Health's numbers over the view in their own colors, unless a Damage Numbers widget drew them this frame
void hud_draw_numbers_default(struct ImDrawList* dl, float x0, float y0, float x1, float y1);

// --- The editor's Details (hud_widget.c) ---
bool hud_ui_layout(CHudLayout* c);              // the HUD Layout component
bool hud_widget_details(struct UiWidget* w);    // a HUD piece's own section in the Designer
