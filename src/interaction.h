// Interaction (notas-dev/interacao.md; módulo 5 do R7): made from scratch, modular, every game's "press E to use".
//   * Interactable (CInteractable, components.h; Details > Add, or a Blueprint's components): its prompt ("Open {0}"),
//     reach, view angle, key or Enhanced Input action, hold for X s, once or again, enabled, priority, a condition by
//     Gameplay Tag (an item tagged so in the instigator's Inventory, or the tag on the instigator)
//   * Interactor (CInteractor): on the player's pawn (the possessed Character gets one when play starts); every frame
//     the best Interactable: the camera's ray first, then how near and how centered, its priority; not through walls;
//     the focused actor outlined or glowing (the renderer's selection mask) and the prompt on the HUD
//   * events on the Interactable's actor: On Interact (Instigator), On Focus Begin / End, On Hold Progress; nodes Interact,
//     Get Focused Interactable, Set Interaction Enabled, Set Prompt Text, Get Interaction Prompt, Set / Is Device Active
//   * the ready-made actors (CDevice): Door (swings at a hinge or slides; locked by a key's tag), Chest (opens its lid,
//     gives its items), Lever and Button (switch another actor: a door opens, a light goes off, an actor hides; or a
//     Custom Event on it with On). A simple animation of the moving part (robust: no physics to settle)
//   * WBP_InteractPrompt (UMG, /Content/UI): the key (the keyboard's or the gamepad's, whichever was used last), the text
//     and the hold's bar
// Data only and no allocation: the components hold everything; the walk is one pass over the Interactables.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"

enum { IA_EV_INTERACT = 0, IA_EV_FOCUS_BEGIN, IA_EV_FOCUS_END, IA_EV_HOLD, IA_EV_COUNT };
typedef struct InteractHooks {
    // On the Interactable's actor: the event (a: the instigator; progress: the hold's, 0-1). It may run game code
    void (*event)(World* w, Entity e, int ev, Entity a, float progress);
    // A Custom Event on an actor with its bool input (a Lever's / Button's target): false when it has none
    bool (*call)(World* w, Entity e, const char* event, bool on);
    bool (*key)(int key); // a GLFW key held (NULL: none)
} InteractHooks;
void interaction_set_hooks(const InteractHooks* h); // NULL: none

// Play (bp_vm.c): before the collision boxes are made (the moving parts become Movable), after the BeginPlay (the
// player's Interactor and its prompt); every frame after the movement; the end
void interaction_begin_play(World* w);
void interaction_after_begin_play(World* w, Entity player);
void interaction_tick(World* w, float dt, Entity player);
void interaction_end_play(World* w);

// What the nodes do. interactor: who uses (its Interactor; none: the instigator only); target: the Interactable (none:
// the interactor's focused one). False: disabled, used, the condition not met, out of reach
bool interaction_interact(World* w, Entity interactor, Entity target);
Entity interaction_focused(World* w, Entity interactor); // ENTITY_NONE: none
bool interaction_set_enabled(World* w, Entity target, bool enabled);
bool interaction_set_prompt(World* w, Entity target, const char* text);
bool interaction_can_interact(World* w, Entity interactor, Entity target); // the condition met (and enabled, not used)
// The prompt as the HUD shows it: the text ("Open Door", "Locked (Red Key)"), the key's label ("E", "X" on a gamepad),
// the hold (0-1). False: nothing focused
bool interaction_prompt(World* w, Entity interactor, char* text, uint32_t tcap, char* key, uint32_t kcap, float* progress, bool* can);
bool interaction_device_set(World* w, Entity device, bool active, Entity instigator); // open / close, on / off
bool interaction_device_active(World* w, Entity device);

// The renderer's highlight (editor_viewport.c, game_main.c): the focused actor of the player's Interactor and how
// (IAR_OUTLINE / IAR_GLOW); ENTITY_NONE: none
Entity interaction_highlight(World* w, uint32_t* mode);
Entity interaction_highlight_part(World* w); // (its moving part: a chest's lid; ENTITY_NONE: none)

// A ready-made actor set up on e (it has a Static Mesh, an Interactable and a Device): its mesh, size, defaults; a
// chest gets its lid (a second actor, made here; its name: name + "_Lid"). The editor's Place Actors
void interaction_setup_device(World* w, Entity e, int kind);
extern const char* const g_device_names[DEV_KIND_COUNT];   // "Door", "Treasure Chest", "Lever", "Button"
extern const char* const g_device_motions[DEV_MOTION_COUNT]; // "Swing (Hinge)", "Slide", "Tilt"

// WBP_InteractPrompt (the example widget, /Content/UI): made when missing; its hash (0: failed)
uint32_t interaction_prompt_asset(void);

typedef struct InteractStats {
    uint32_t interactables, interactors, devices, candidates, sight_traces; // the last frame's
    uint32_t interactions, refused, focus_changes;                         // since play started
    double tick_ms;                                                        // the last frame's walk
} InteractStats;
const InteractStats* interaction_stats(void);
void interaction_register_debug(void); // its Debug Viewer list ("Interaction")

// (Q25, vr_pawn.h) VR: the player's Interactor looks along a hand's ray (o, d: engine space) instead of the camera, and
// use (the hand's trigger) is its key; off: the camera and the keys again. Set every frame by the VR pawn
void interaction_vr_pointer(bool on, const float o[3], const float d[3], bool use);

// The tests: the gamepad as the last device used (1), the keyboard (0), or as it really is (-1)
void interaction_test_gamepad(int on);

// The self-test (--interaction-test, before the play part in interaction_test.c): focus by view and reach, priority,
// the condition by item tag and by Gameplay Tag, once only, hold, the door's swing and slide, the lever's switch, the
// chest's items; 200 Interactables timed. Failures
int interaction_selftest(void);

// --- The editor's Details (interaction_ui.c: the level's, a Blueprint's components; w NULL: no actor to pick) ---
bool ia_ui_interactable(CInteractable* c);
bool ia_ui_interactor(CInteractor* r);
bool ia_ui_device(World* w, Entity self, CDevice* d);
