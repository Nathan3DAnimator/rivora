// (Q25, notas-dev/xr.md) The VR pawn: what Unreal's VR Template does, for the possessed pawn whenever the game runs on a
// headset (xr.h), set up by Project Settings > VR and changed by Blueprints:
//   * the play space: its origin on the floor under the pawn (a Character: its feet) turned by the pawn's yaw; the eyes
//     and the hands placed from it (seated: raised to the seated height)
//   * locomotion: teleport (the left stick pushed aims an arc from the hand, released goes; the head lands on the spot),
//     smooth (the left stick walks the Character where the head looks), or both (the right stick's push teleports);
//     turning: snap (45 degrees around the head) or smooth by the right stick; the comfort vignette while moving / turning
//   * hands: the controllers' (the fingers curled by the trigger and the grip) or the tracked hands' joints, drawn as small
//     boxes; a laser from the hand (aim pose) that points at widgets (the VR menu panel: the trigger clicks) and at
//     Interactables (the player's Interactor looks along it, the trigger uses: interaction_vr_pointer)
//   * grabbing (an Interactable set Grabbable: IA_GRAB): the grip takes what is under the hand, it follows the hand
//     (where it was taken, or snapped: IA_GRAB_SNAP); let go it is thrown with the hand's speed (its physics again), or
//     snapped into a Grab Socket near it (IA_GRAB_SOCKET; its Required Tag: the Gameplay Tag what it takes must have)
//   * the real hands (tracked: no controller): the fist grabs, the pinch is the trigger, the index aims the laser; the index
//     tip touching a door / lever / button uses it (both kinds of hands); drag locomotion: a hand closed in the air (nothing
//     to grab) holds the world, the play space moves under it (both hands: their mean; let go: a short glide), the walls stop it
//   * physical hands (optional): the drawn hand stops at the fixed level (walls, floors, tables), pushes bodies with physics;
//     what it holds stops at walls too; the head into a wall fades the view to black or pushes the player back; the pawn's
//     own body hidden (only the hands); each hand's mesh replaceable (Project Settings > VR > Hand Meshes)
// Data only, no allocation: the visuals are a few entities made when play starts.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

void vr_pawn_begin_play(World* w); // the game started on a headset: the settings from the project, the visuals made
void vr_pawn_end_play(World* w);
bool vr_pawn_active(void);
// Every frame, after xr_begin_frame (the hands' poses for this frame): the sticks (locomotion, turning, teleport's aim)
// before the Blueprints and the movement; then, after the physics, what follows the hands (held actors, hands, laser,
// arc), the Interactor's ray and the menu panel's pointer
void vr_pawn_tick(World* w, float dt);
void vr_pawn_late_tick(World* w, float dt);
// The play space in the world: its origin (cm, on the floor) and its yaw (degrees); a play-space point into the world
void vr_pawn_origin(float pos[3], float* yaw_deg);
void vr_pawn_to_world(const float local[3], float out[3]);
void vr_pawn_dir_to_world(const float local[3], float out[3]);
float vr_pawn_vignette(void); // the comfort vignette now (0..1)
float vr_pawn_fade(void);     // the head inside a wall: the view's fade to black now (0..1)

// The VR menu panel (xr_panel_*): shown while a widget wants the mouse (the pause menu, a menu), placed 1.2 m before the
// head when it opens; the laser on it moves the UI's pointer (uv 0..1), the trigger is its button
bool vr_pawn_ui_pointer(float uv[2], bool* down);
void vr_pawn_set_menu(bool shown); // the panel's state this frame (game_main: ui_mouse_shown())

// Blueprints' nodes (Unreal's names where the VR Template has them)
void vr_pawn_teleport(World* w, const float location[3], float yaw_deg, bool keep_yaw); // the head lands over location
void vr_pawn_set_locomotion(int locomotion, int turn);  // VR_LOCO_*, VR_TURN_* (project.h); < 0: kept
void vr_pawn_set_vignette(float strength);               // the comfort vignette's most (0: off)
void vr_pawn_set_hand_collision(int physical, int objects, int push); // Physical Hands, with objects, Hands Push Bodies; < 0: kept
void vr_pawn_turn_play_space(World* w, float delta_deg);   // (Q25b) the play space turned about the head (mixed reality: the level to the room)
bool vr_pawn_grab(World* w, int hand, Entity target);     // takes target into hand (it need not be Grabbable)
void vr_pawn_release(World* w, int hand);
Entity vr_pawn_held(int hand);                           // ENTITY_NONE: empty
bool vr_pawn_laser(int hand, float hit[3], Entity* actor); // the laser's hit (false: nothing within 10 m)

typedef struct VrStats {
    uint32_t teleports, snap_turns, grabs, throws, socketed, ui_clicks, interacts, drags, pokes; // since play started
    uint32_t pushes, hand_blocked, head_blocked, distance_grabs, rescues; // (frames a hand pushed a body / stopped at a wall / the head met a wall)
    float last_throw_speed;    // cm/s
    double tick_ms;            // the last frame's two ticks
} VrStats;
const VrStats* vr_pawn_stats(void);

// The self-test (--vr-test, the simulated headset; xr test area): the play space, snap and smooth turns about the head,
// the teleport's arc onto a floor and refused into a wall, smooth walking, grab / follow / throw with the hand's speed,
// the socket's snap, the laser on an Interactable (focus, the trigger uses it) and on the menu panel (uv, click), the
// tracked hand's pinch and fist, game.ini's keys, the foveation's rings. Failures
int vr_pawn_selftest(void);
// The played test (--vr-script): the packaged VR template on the simulated headset, a cube grabbed, held, thrown (true: done)
bool vr_pawn_script(World* w, float dt);
