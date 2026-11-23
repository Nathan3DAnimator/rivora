// The Debug Viewer (notas-dev/debug-viewer.md): what weighs in the scene, in the editor and in the packaged game (F10).
// One list per system with its cost in front; inside, the scene's objects with each one's cost (the heavy ones in red).
// It measures only while open: closed, nothing is timed, read back or collected (render_set_detail, bp_profile off).
//
// A new big system (foliage, a simulation...) adds its own list: dv_register(title, collect). Its collect function is
// called a few times a second while the viewer is open; it fills the list with dv_add (any number of items: the viewer
// keeps the heaviest DV_MAX_ITEMS and sums the rest) and sets its total (l->ms) and an optional note.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define DV_MAX_ITEMS 256

typedef struct DvItem {
    char name[64];
    char info[48]; // what it is / why it costs (triangles, LOD, rays...)
    float ms;
    Entity e;      // ENTITY_NONE: not an actor (a pass, a subsystem)
} DvItem;

typedef struct DvList {
    char title[48];
    char note[160];  // under the title: how it is measured, or why it is empty
    double ms;       // the list's total (GPU or CPU, see unit)
    bool gpu;        // the total is GPU time (else CPU)
    bool estimated;  // the items are a measured total split by a cost model (triangles, screen coverage...)
    DvItem items[DV_MAX_ITEMS];
    uint32_t n;
    uint32_t rest;   // the items past the heaviest DV_MAX_ITEMS
    double rest_ms;
} DvList;

typedef struct DvContext {
    World* world;
    bool editor;       // the editor (clicking an item selects its actor) or the game
    bool playing;      // PIE or the game: the Blueprints run
    double frame_ms, cpu_ms, gpu_ms;
    float camera[3], fwd[3];
    float fov_deg, view_w, view_h;
} DvContext;

typedef void (*DvCollectFn)(DvList* l, const DvContext* c);

// A list of a system (its title is shown as given: the built-in lists translate theirs)
void dv_register(const char* title, DvCollectFn collect);
// An item into the list being collected
void dv_add(DvList* l, const char* name, const char* info, float ms, Entity e);

// The frame rate's color: 10 fps strong red, 30 orange, 60 dark green, 100 light green, 200+ gold
void dv_fps_color(double fps, float out[3]);
bool dv_open(void);
void dv_set_open(bool open);
// Once per frame (inside an ImGui frame) while open: collects (a few times a second) and draws the window. Returns the
// actor clicked (ENTITY_NONE: none).
Entity dv_frame(const DvContext* c);
// The lists as last collected, printed (--bench with --debug-viewer)
void dv_dump(void);
// The object cost view (sugestão #65, editor_cost.c): measuring on while it is on (the viewer closed too); true when
// the lists were collected anew this call (a few times a second). The lists as collected (their English titles).
bool dv_costs(const DvContext* c, bool on);
int dv_list_count(void);
const DvList* dv_list_at(int i, const char** title);
