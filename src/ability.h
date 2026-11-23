// Abilities and effects (notas-dev/habilidades.md; R7 módulo 9): Unreal's Gameplay Ability System, the light version,
// on the Gameplay Tags (gameplay_tags.h) and the Health (health.h). Made from scratch.
//   * a Gameplay Ability is a Blueprint Class whose parent is "Gameplay Ability": its class defaults (variables in the
//     category Ability: Display Name, Icon, Cost Attribute + Cost, Cooldown, Charges, Cast Time, Ability Tags,
//     Activation Owned Tags, Blocked By Tags, Cancel Abilities With Tags, Self Effect) are the rules; its Event On
//     Activate Ability does it and End Ability ends it (an object of the class is spawned for the owner the first time)
//   * a Gameplay Effect is a Data Asset of the ready-made type "Gameplay Effect": Instant (changes the base values once),
//     Has Duration or Infinite (changes the current values while active); Period > 0 runs it every Period (Damage / Heal
//     through the Health, its modifiers onto the base: a regeneration); modifiers "Speed x1.5, Defense +10, Mana =50";
//     stacking (Max Stacks, Refresh Duration), Granted Tags while active, Required Tags to apply. Buffs and debuffs
//   * the Ability System component (CAbilitySystem, components.h): its abilities, attributes (base and current), active
//     effects and their time, cooldowns and charges. The run's state lives here in fixed SoA arrays (systems; effects in
//     one array, a list per system): no malloc in the frame. Speed is the Character's Max Walk Speed, Damage multiplies
//     the damage it deals (1), Defense divides the damage it takes (x 100 / (100 + Defense)); X is held in 0..MaxX when
//     the system has an attribute MaxX
//   * the nodes (bp_vm.c, the scripts get them by themselves) and the events On Ability Activated / Ended, On Effect
//     Applied / Removed, On Attribute Changed (on the owner, once the frame's work is done)
//   * saved by Save Progress (save_progress.c: the chunk ABIL), shown by WBP_AbilityBar (ability_widget.c)
// An ability or an effect is named by its asset's name ("GA_Fireball", "GE_Poison") or its Display Name.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define ABIL_MAX_ATTRS 16       // attributes one Ability System has
#define ABIL_MAX_GRANTED 8      // abilities one Ability System holds
#define ABIL_MAX_SYSTEMS 4096   // Ability Systems alive at once while playing
#define ABIL_MAX_EFFECTS 16384  // active effects at once (every system's)
#define ABIL_MAX_DEFS 512       // abilities (and effects) known
#define ABIL_DEF_TAGS 4         // tags in each of a definition's lists
#define ABIL_EFFECT_MODS 4      // an effect's modifiers
#define ABIL_PLAYER_KEY 0xFFFFFFFFu // a save record's key for the player (save_progress.c's)

enum { GE_INSTANT = 0, GE_DURATION, GE_INFINITE };
enum { GE_OP_ADD = 0, GE_OP_MUL, GE_OP_OVERRIDE };
// Try Activate Ability's results (Failure Reason: g_abil_fail_names)
enum { ABIL_OK = 0, ABIL_FAIL_NOT_GRANTED, ABIL_FAIL_ACTIVE, ABIL_FAIL_BLOCKED, ABIL_FAIL_COOLDOWN, ABIL_FAIL_COST, ABIL_FAIL_DEAD, ABIL_FAIL_COUNT };
extern const char* const g_abil_fail_names[ABIL_FAIL_COUNT]; // "", "Not Granted", "Active", "Blocked", "Cooldown", "Cost", "Dead"

typedef struct AbilityDef {
    uint32_t hash, name_hash;  // the class's (its vpath's), its name's
    char name[48];             // the class's name: "GA_Fireball"
    char display[64], description[96], icon[160];
    uint16_t cost_attr;        // an attribute's id (0: none)
    float cost, cooldown, cast_time;
    int32_t charges;           // >= 1: uses before it waits for its cooldown (one comes back each Cooldown)
    uint16_t tags[ABIL_DEF_TAGS], owned[ABIL_DEF_TAGS], block[ABIL_DEF_TAGS], cancel[ABIL_DEF_TAGS]; // tag ids
    uint8_t ntags, nowned, nblock, ncancel;
    uint32_t self_effect;      // a Gameplay Effect applied to the owner when it activates (0: none)
    bool has_graph;            // its Event On Activate Ability (none: it ends once activated)
} AbilityDef;

typedef struct EffectMod { uint16_t attr; uint8_t op; float value; } EffectMod;
typedef struct EffectDef {
    uint32_t hash, name_hash;
    char name[48], display[64], description[96], icon[160];
    uint8_t policy;            // GE_INSTANT, GE_DURATION, GE_INFINITE
    float duration, period;    // seconds (period 0: not periodic)
    float damage, heal;        // a period's (or the application's) through the Health
    char damage_type[40];
    uint8_t nmods;
    EffectMod mods[ABIL_EFFECT_MODS];
    int32_t max_stacks;        // 0: each application its own; >= 1: one with up to this many stacks
    bool refresh, debuff;      // a new stack restarts the duration; shown red (a debuff) in the lists
    uint16_t granted[ABIL_DEF_TAGS], required[ABIL_DEF_TAGS];
    uint8_t ngranted, nrequired;
} EffectDef;

// --- Definitions (read again when play starts: every Gameplay Ability class and Gameplay Effect asset) ---
struct BpAsset;
void abil_defs_refresh(void);
uint32_t abil_ability_count(void);
const AbilityDef* abil_ability_at(uint32_t i);
const AbilityDef* abil_ability_by_hash(uint32_t hash);
const AbilityDef* abil_ability_by_name(const char* name);
uint32_t abil_effect_count(void);
const EffectDef* abil_effect_at(uint32_t i);
const EffectDef* abil_effect_by_hash(uint32_t hash);
const EffectDef* abil_effect_by_name(const char* name);
const char* abil_attr_name(uint16_t id); // "" unknown
const char* abil_tag_name(uint16_t id);
bool abil_is_ability_class(const struct BpAsset* a);
bool abil_is_effect_asset(const struct BpAsset* a);
void abil_class_defaults(struct BpAsset* a); // a new Gameplay Ability: its variables and its graph (blueprint.c bp_create)
void abil_effect_fields(struct BpAsset* a);  // a new Gameplay Effect's fields

// --- Play (bp_vm.c) ---
void abil_begin_play(World* w);                       // the systems made from the components (before any BeginPlay)
void abil_after_begin_play(World* w, Entity player);  // WBP_AbilityBar for the player with Show Ability Bar
void abil_tick(World* w, float dt, Entity player);    // effects' time and periods, casts, cooldowns, the keys, the events
void abil_end_play(void);

// --- Operations (the nodes'): e is the actor with the Ability System (made when an effect or ability needs it) ---
int abil_give(World* w, Entity e, uint32_t ability);            // its slot (-1: no such ability / full); already: its slot
bool abil_remove(World* w, Entity e, uint32_t ability);         // cancelled first when active
int abil_try_activate(World* w, Entity e, uint32_t ability);    // ABIL_OK or ABIL_FAIL_*
int abil_try_activate_tag(World* w, Entity e, const char* tag, uint32_t* which); // the first granted with the tag
int abil_cancel(World* w, Entity e, const char* name_or_tag);   // active ones cancelled ("": all)
bool abil_end(World* w, Entity ability_object);                 // End Ability (its graph's self)
Entity abil_owner_of(World* w, Entity ability_object);          // ENTITY_NONE: not an ability's object
bool abil_is_active(World* w, Entity e, uint32_t ability);      // active or casting
float abil_cooldown_remaining(World* w, Entity e, uint32_t ability, float* duration, int* charges, int* max_charges);
bool abil_on_cooldown(World* w, Entity e, uint32_t ability);    // no charge left
int abil_apply_effect(World* w, Entity target, uint32_t effect, Entity source); // its stacks now (instant: 1); 0: not applied
int abil_remove_effect(World* w, Entity e, const char* name_or_tag, int stacks); // stacks removed (stacks <= 0: whole)
int abil_has_effect(World* w, Entity e, const char* name_or_tag, float* remaining); // its stacks (0: none); remaining <0: infinite
bool abil_get_attr(World* w, Entity e, const char* name, float* current, float* base);
bool abil_set_attr(World* w, Entity e, const char* name, float base); // the base (made when missing)
void abil_clear(World* w, Entity e);                            // no effects, abilities, attributes (the tests)

// --- The UI (ability_widget.c) ---
typedef struct AbilSlotInfo {
    const AbilityDef* def;
    float cooldown_left, cooldown; // the next charge's wait (0: full)
    float cast_left, cast_time;
    int charges, max_charges;
    bool active, casting, affordable;
    int key;                       // a GLFW key when the player's keys activate it (0: none)
} AbilSlotInfo;
bool abil_slot_info(World* w, Entity e, int slot, AbilSlotInfo* out);
int abil_slot_count(World* w, Entity e);
typedef struct AbilEffectInfo { const EffectDef* def; float remaining, duration; int stacks; } AbilEffectInfo; // remaining < 0: infinite
uint32_t abil_effects_of(World* w, Entity e, AbilEffectInfo* out, uint32_t cap);

// --- Saves (save_progress.c's chunk ABIL): the actors' attributes, active effects and cooldowns by their keys ---
uint32_t abil_save_write(World* w, const Entity* ents, const uint32_t* keys, uint32_t n, uint8_t* out, uint32_t cap);
// The records whose keys find finds (the level's actors), or only the player's (player_only: onto player)
void abil_save_read(World* w, const uint8_t* d, uint32_t n, Entity (*find)(World* w, uint32_t key), Entity player, bool player_only);

// --- Widgets, the Debug Viewer, the tests ---
uint32_t abil_bar_asset(void); // WBP_AbilityBar (Content/UI, made when missing): its hash
struct ImDrawList;
struct UiWidget;
void abil_widget_frame(World* w, const float mouse[2], bool clicked); // the slots the last frame drew: a click activates
void abil_widget_draw(struct ImDrawList* dl, const struct UiWidget* x, float x0, float y0, float x1, float y1, float k, float alpha, bool designer);
bool abil_widget_slot_center(Entity owner, int slot, float xy[2]); // (the tests' clicks)
void abil_widget_test_click(bool on, float x, float y);
typedef struct AbilStats {
    uint32_t systems, effects, abilities, active; // alive now
    uint32_t applied, removed, periodic, activations, failures, events; // since play started
    double tick_ms;                               // the last abil_tick
    double op_us;                                 // the last operation's (an apply, an activation)
} AbilStats;
const AbilStats* abil_stats(void);
void abil_register_debug(void); // its Debug Viewer list ("Abilities and Effects")
int abil_selftest(void);        // the rules without the VM (--ability-test, before the play part); failures
// The events the self-test sees (no VM): kind = BPK_EVENT_*, s: the name, a / b: the numbers, e: an actor
typedef void (*AbilTestHook)(Entity owner, uint16_t kind, const char* s, float a, float b, Entity e);
void abil_set_test_hook(AbilTestHook h);
// The editor's Details (ability_ui.c)
struct CAbilitySystem;
bool abil_ui_system(struct CAbilitySystem* c);
