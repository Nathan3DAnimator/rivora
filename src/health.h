// Health and damage (notas-dev/saude-dano.md): the Health Component of Unreal's games as a component of its own
// (CHealth, components.h), reusable by any actor and any game. Data only: the functions here change the components and
// tell the actor through hooks (bp_vm.c sets them while playing: its Event AnyDamage, On Healed, On Death, On Shield
// Broken; the destroy with End Play; the ragdoll of its Skeletal Mesh). Nothing allocated: the floating numbers and the
// frame's lists are fixed arrays.
//   * damage: Amount x the most specific multiplier whose Gameplay Tag matches the damage type ("Damage" covers
//     "Damage.Fire.Burning"; none: x1), taken by the shield first, then the health. Dead or Invincible: nothing.
//   * regeneration: Regen Delay seconds after the last damage, Regen Rate health and Shield Regen Rate shield a second.
//   * death (health 0): the On Death event; then, by Death: only that, the actor destroyed after Destroy Delay, or its
//     Skeletal Mesh's ragdoll (a Physics Asset) and the destroy after the delay when it is above 0. Revive brings it back.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"

enum { HEALTH_EV_DAMAGED = 0, HEALTH_EV_HEALED, HEALTH_EV_DEATH, HEALTH_EV_SHIELD_BROKEN, HEALTH_EV_COUNT };
typedef struct HealthHooks {
    // On the actor: Damaged (amount taken, type, a: instigator, b: causer), Healed (amount, a: healer), Death (type, a:
    // killer, b: causer), Shield Broken (a: instigator). It may run game code: the components can move meanwhile
    void (*event)(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b);
    void (*destroy)(World* w, Entity e);          // NULL: ecs_destroy
    bool (*ragdoll)(World* w, Entity e, bool on); // the actor's Skeletal Mesh simulating (false: none, or no Physics Asset)
} HealthHooks;
void health_set_hooks(const HealthHooks* h); // NULL: none
// The damage scaled from outside before the multipliers (ability.c: the instigator's Damage and the target's Defense
// attributes); NULL: none
void health_set_damage_scale(float (*fn)(World* w, Entity target, Entity instigator));

// The damage taken (shield + health, after the multiplier); 0: no Health, dead, invincible, immune
float health_apply_damage(World* w, Entity target, float amount, const char* type, Entity instigator, Entity causer);
// Unreal's Apply Radial Damage with Falloff: every Health within radius of origin takes min + (base - min) x (1 -
// distance / radius) ^ falloff (falloff 0: base everywhere). The actors damaged
uint32_t health_apply_radial(World* w, const float origin[3], float radius, float base, float min_damage, float falloff, const char* type,
                             Entity instigator, Entity causer);
float health_heal(World* w, Entity target, float amount, Entity healer); // the health given (dead: 0, Revive first)
bool health_revive(World* w, Entity target, float health);               // health <= 0: full; the shield full; the ragdoll off
bool health_set_max(World* w, Entity target, float max_health, bool fill); // fill: the health to it (else clamped)
bool health_set_shield(World* w, Entity target, float shield, float max_shield); // max_shield <= 0: kept
bool health_set_invincible(World* w, Entity target, bool on);
bool health_set_multiplier(World* w, Entity target, const char* type, float scale); // replaces the type's (false: full)
float health_multiplier(const CHealth* h, const char* type);
// Getters (no Health: 0, not dead)
float health_get(World* w, Entity e);
float health_get_max(World* w, Entity e);
float health_percent(World* w, Entity e);
float health_shield(World* w, Entity e);
float health_shield_max(World* w, Entity e);
bool health_is_dead(World* w, Entity e);

// Every frame while playing (bp_tick): regeneration, the deaths' timers and destroys, the numbers' ages
void health_update(World* w, float dt);
void health_begin_play(World* w);
void health_end_play(void);

// The floating damage numbers (Health > Damage Numbers): the last HEALTH_NUMBERS_MAX, oldest replaced (ui_widget_draw.c
// draws them over the view)
#define HEALTH_NUMBERS_MAX 64
#define HEALTH_NUMBER_LIFE 1.1f
enum { HEALTH_NUM_DAMAGE = 0, HEALTH_NUM_SHIELD, HEALTH_NUM_HEAL };
typedef struct HealthNumber {
    float pos[3];  // where it started (over the actor)
    float amount;
    float age;     // seconds; > HEALTH_NUMBER_LIFE (or amount 0): none
    uint32_t kind; // HEALTH_NUM_*
    uint32_t critical; // a critical hit (Show Damage Number): bigger, its own color, "!"
} HealthNumber;
const HealthNumber* health_numbers(uint32_t* count);
bool health_numbers_active(void); // one is showing (the game opens its ImGui frame for them)
// A number at pos (the HUD's Show Damage Number, hud.h; the Health's own over the actor): the same buffer, drawn by the
// HUD's Damage Numbers widget (or over the view when none is shown)
void health_push_number(const float pos[3], float amount, uint32_t kind, bool critical);

typedef struct HealthStats {
    uint32_t components, dead;                 // the last update's
    uint32_t damages, heals, deaths, destroyed, radial; // since play started
    uint32_t damages_frame;                    // the last frame's
    double update_ms;                          // the last update
    double damage_ms;                          // the last frame's damage and healing (measured while the Debug Viewer is open)
} HealthStats;
const HealthStats* health_stats(void);
void health_register_debug(void); // its Debug Viewer list ("Health and Damage")

// WBP_HealthBar (the example widget, /Content/UI): a health bar, a shield bar and "75 / 100" of the player's pawn. Made
// when missing; its hash (0: failed)
uint32_t health_bar_asset(void);

// The self-test (--health-test, before the play part in health_test.c): types and multipliers, the shield, the
// regeneration, death and its events, the destroy, healing, revive, radial damage, invincible; 1000 actors damaged a
// frame, timed. Failures
int health_selftest(void);
