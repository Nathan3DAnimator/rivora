// (Q25b) Mixed reality on a passthrough headset (Meta Quest 3 / 3S / Pro; notas-dev/xr.md), over the VR of xr.h and the
// VR pawn (vr_pawn.h). Everything is the developer's: Project Settings > VR > Mixed Reality, the console variables
// vr.Passthrough* / vr.MR*, the Blueprint nodes (Set Passthrough Enabled / Opacity / Style / Window, Get Room Planes,
// Line Trace Real World, Event On Room Loaded).
//   * Passthrough (XR_FB_passthrough when the runtime has it, else an ALPHA_BLEND environment): the headset's cameras behind
//     the game. The eye's image carries an alpha (composite.frag, shaders/mr.glsl): where nothing virtual is drawn (the
//     level's sky gone: Background), under a Passthrough Window (an actor, or the room's real windows and doors), where a
//     real thing is nearer than the virtual one, the room shows. Its opacity, its style (color, black & white, one color,
//     brightness / contrast / saturation) and edges' color, on and off while playing; the virtual things' own opacity.
//   * The room (XR_FB_scene, _spatial_entity, _query, _container; XR_META_spatial_entity_mesh): the headset's Space Setup
//     loaded (asked for when there is none): the floor, ceiling, walls, tables, couches, beds, windows, doors with their
//     labels, and the room's mesh. Each plane and volume becomes an "MR ..." actor that collides and is never drawn (the
//     real one is seen): the Characters walk on the real floor, the physics lands on the real table; the virtual things'
//     shadows fall on the real surfaces; line traces reach every real thing (the mesh too). The level is turned to the
//     room's walls (axis-aligned: the collision boxes exact).
//   * The real depth (XR_META_environment_depth): the real things nearer than the virtual ones hide them (a soft edge; the
//     hands left out when asked); without it the room's planes and volumes hide what is behind them.
//   * The simulated headset (--vr-sim): a stand-in room (5 x 4 m, a table, a couch, a window, a door), its mesh and a real
//     depth made from it, the passthrough's look and style drawn in the window: the whole path testable without a headset.
// The room is in the play space (the real one: cm, engine axes, the floor at z 0) and follows the VR pawn into the world.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "xr.h"

typedef enum MrLabel {
    MR_LABEL_FLOOR = 0, MR_LABEL_CEILING, MR_LABEL_WALL, MR_LABEL_TABLE, MR_LABEL_COUCH, MR_LABEL_BED, MR_LABEL_STORAGE,
    MR_LABEL_SCREEN, MR_LABEL_WINDOW, MR_LABEL_DOOR, MR_LABEL_LAMP, MR_LABEL_PLANT, MR_LABEL_WALL_ART, MR_LABEL_INVISIBLE_WALL,
    MR_LABEL_GLOBAL_MESH, MR_LABEL_OTHER, MR_LABEL_COUNT
} MrLabel;
const char* mr_label_name(int label);      // "Floor", "Wall", "Table"... (the Blueprints' Label)
int mr_label_from_runtime(const char* s);  // the runtime's labels ("WALL_FACE", "TABLE,DESK": the first known)

#define MR_MAX_ANCHORS 128
#define MR_MAX_POLY 32
typedef struct MrAnchor {
    int label;
    char labels[64];                  // as the runtime gave them
    float pos[3], ax[3], ay[3], az[3]; // its frame (play space, cm): ax, ay a plane's axes, az its normal (into the room)
    bool plane, volume;
    float rect[4];                    // the plane: x, y, width, height (cm, along ax / ay)
    float box_min[3], box_max[3];     // the volume (cm, along ax / ay / az)
    uint32_t npoly;
    float poly[MR_MAX_POLY][2];       // the plane's outline (cm, along ax / ay)
    uint8_t uuid[16];
} MrAnchor;
typedef struct MrRoom {
    uint32_t n;
    MrAnchor a[MR_MAX_ANCHORS];
    float* mesh_v;                    // the room's mesh (play space, cm: xyz), mesh_nv vertices; mesh_i: mesh_ni indices
    uint32_t* mesh_i;
    uint32_t mesh_nv, mesh_ni;
    uint64_t serial;                  // a new one each time the room changes
    bool simulated;
} MrRoom;
const MrRoom* mr_room(void);

typedef struct MrStatus {
    bool passthrough_ext, passthrough_supported, passthrough_color, blend_alpha; // what the runtime has
    bool passthrough_running;         // the real world is seen now
    bool scene_ext, mesh_ext, capture_ext, depth_ext, depth_supported, hand_removal_supported;
    bool room_loading, room_loaded, room_none, depth_running;
    uint32_t depth_w, depth_h;
    uint64_t depth_frames;            // real depth images received
    double tick_ms, render_ms;        // the last frame's CPU (mr_tick, mr_render)
    char note[160];                   // the last thing worth saying (an error, why something is off)
} MrStatus;
const MrStatus* mr_status(void);

// The game (game_main, VR): once the pawn plays; each frame after the pawn's late tick (settings, the passthrough's state,
// the room's events and actors); before the eyes (the renderer's numbers: render_mr_frame); at the end
void mr_begin_play(World* w);
void mr_tick(World* w, float dt);
struct RenderView;
void mr_render(World* w, struct RenderView* view, const XrEye eyes[2]); // (also the exposure on the view; eyes: the play space's)
void mr_end_play(World* w);
bool mr_on(void);              // the passthrough asked for and running (or simulated)

// Blueprints and the console (they write Project Settings > VR > Mixed Reality: read every frame)
void mr_set_passthrough(bool on);
bool mr_passthrough_active(void);
void mr_set_window(World* w, Entity e, bool on); // an actor's bounds show the real room through the virtual (16 at most)
bool mr_load_room(void);                         // the Space Setup loaded again (false: no scene here)
bool mr_request_room_setup(void);                // the headset's Space Setup opened (false: not here)
uint32_t mr_room_actors(Entity* out, uint32_t cap); // the room's actors (planes and volumes)
bool mr_actor_info(World* w, Entity e, int* label, float center[3], float normal[3], float size[3]); // false: not one
// A world segment against the real room (its planes, volumes and mesh): the nearest hit, its normal, label and actor
bool mr_line_trace(const float a[3], const float b[3], float hit[3], float normal[3], int* label, Entity* actor);

// (xr.c) the hooks into the OpenXR session (xr_internal.h)
int mr_selftest(void); // --mr-test: the simulated room, actors, traces, alignment, windows, the renderer's numbers; failures
