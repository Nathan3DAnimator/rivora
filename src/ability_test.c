// Abilities and effects played (--ability-test, the test ability; notas-dev/habilidades.md): first abil_selftest (the
// rules without the VM, 1000 actors with 5 effects each timed), then in a new project, in the editor's play:
//   * GA_Fireball (a Gameplay Ability: Mana 30, cooldown 1.5 s, blocked by State.Stunned): its graph's Event On Activate
//     Ability applies GE_Poison (10 a second, stacks up to 3, State.Poisoned) to BP_AbilDummy, then End Ability;
//     GA_Haste (Mana 10, cooldown 5 s, Self Effect GE_Haste: Speed x1.5); GE_Stun (State.Stunned 0.6 s)
//   * BP_AbilHero (the player's Character, an Ability System with both, Show Ability Bar): its Custom Event TryTwice
//     tries the Fireball twice (the second: Cooldown); its events count into variables
//   * the level saved to a file (Load Progress opens it again)
//   * step by step: stunned, the Fireball blocked; the stun over and the cooldown too: WBP_AbilityBar's first slot
//     clicked casts it again (the poison stacks); the haste walks the Character 1.5x faster; the poison ticks through
//     the Health; Save Progress, everything changed, Load Progress: the Mana, the effects with their time, the stacks and
//     the cooldown back
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ability.h"
#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "gameplay_tags.h"
#include "health.h"
#include "platform.h"
#include "platform_files.h"
#include "save_progress.h"
#include "ui_widget.h"

static bool g_ok = true, g_done;
static int g_state, g_wait = 20;
static double g_state_t;
static Entity g_hero, g_dummy;
static uint32_t g_fire, g_haste, g_loads;
static float g_act0, g_mana_s, g_rem_s, g_cd_s, g_hp_s;
static int g_stacks_s;
static double g_tick_ms, g_op_us;
static float g_counts[4]; // the hero's Activated, Ended, Applied, AttrChanges before the load (a new instance after it)

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: ability-test: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: ability-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = pin(g, a, 1, ap), pb = pin(g, b, 0, bpn);
    if (!bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb)) printf("[RealEngine] FAIL: ability-test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 300.0f, floorf(x / 6.0f) * 190.0f, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
static void slit(BpGraph* g, uint32_t id, const char* name, const char* s) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}
static void var(BpAsset* a, const char* name, uint8_t type) {
    if (bp_add_var(a, name, type) < 0) g_ok = false;
}
static void field(BpAsset* a, const char* name, float v, const char* s) { // a class default / a Data Asset's value
    int k = bp_var_index(a, name);
    if (k < 0) {
        printf("[RealEngine] FAIL: ability-test: %s has no \"%s\"\n", a->name, name);
        g_ok = false;
        return;
    }
    a->vars[k].def.v[0] = v;
    if (s) snprintf(a->vars[k].sdef, sizeof a->vars[k].sdef, "%s", s);
}
// on event ev: Var = Var + 1 (an integer counter); returns the Set node
static uint32_t count_on(BpAsset* a, BpGraph* g, uint32_t ev, float x, const char* varname) {
    uint32_t get = node(a, g, BPK_VAR_GET, x, varname), add = node(a, g, BPK_IADD, x + 1, NULL), set = node(a, g, BPK_VAR_SET, x + 2, varname);
    link(g, get, varname, add, "A");
    BpNode* n = bp_node(g, add);
    if (n) n->pins[pin(g, add, 0, "B")].lit = (BpValue){ { 1, 0, 0, 0 } };
    link(g, add, "", set, varname);
    link(g, ev, "", set, "");
    return set;
}
static bool done_class(BpAsset* a) {
    if (!g_ok || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: ability-test: %s didn't compile: %s\n", a->name, a->error);
        return false;
    }
    return true;
}

static BpAsset* make_effect(const char* F, const char* name, const char* policy, float duration, float period, const char* mods, float damage,
                            int stacks, const char* granted, bool debuff) {
    BpAsset* a = bp_create(BPA_DATA, F, name, "Gameplay Effect");
    if (!a) return NULL;
    field(a, "Duration Policy", 0, policy), field(a, "Duration", duration, NULL), field(a, "Period", period, NULL);
    field(a, "Modifiers", 0, mods), field(a, "Damage", damage, NULL), field(a, "Damage Type", 0, "Damage.Poison");
    field(a, "Max Stacks", (float)stacks, NULL), field(a, "Granted Tags", 0, granted), field(a, "Is Debuff", debuff ? 1.0f : 0.0f, NULL);
    field(a, "Display Name", 0, name + 3);
    return bp_save(a) ? a : NULL;
}

static BpAsset* make_fireball(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "GA_Fireball", "Gameplay Ability");
    if (!a) return NULL;
    field(a, "Display Name", 0, "Fireball"), field(a, "Cost", 30, NULL), field(a, "Cooldown", 1.5f, NULL);
    field(a, "Ability Tags", 0, "Ability.Attack.Fire");
    // Event On Activate Ability -> Apply Gameplay Effect (the dummy, GE_Poison, the owner) -> End Ability
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t ev = node(a, g, BPK_EVENT_ABIL_ACTIVATE, 0, NULL), find = node(a, g, BPK_GET_ACTOR_OF_CLASS, 1, NULL);
    uint32_t ap = node(a, g, BPK_GE_APPLY, 2, NULL), end = node(a, g, BPK_ABIL_END, 3, NULL);
    slit(g, find, "Actor Class", "BP_AbilDummy"), slit(g, ap, "Effect", "GE_Poison");
    link(g, ev, "", ap, ""), link(g, ap, "", end, "");
    link(g, find, "Return Value", ap, "Target"), link(g, ev, "Owner", ap, "Source");
    return done_class(a) ? a : NULL;
}
static BpAsset* make_haste(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "GA_Haste", "Gameplay Ability");
    if (!a) return NULL;
    field(a, "Display Name", 0, "Haste"), field(a, "Cost", 10, NULL), field(a, "Cooldown", 5.0f, NULL);
    field(a, "Ability Tags", 0, "Ability.Buff"), field(a, "Self Effect", 0, "GE_Haste");
    return done_class(a) ? a : NULL; // (its graph: On Activate Ability -> End Ability, as made)
}
static BpAsset* make_hero(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_AbilHero", "Character");
    if (!a) return NULL;
    var(a, "FirstOk", BPT_BOOL), var(a, "SecondReason", BPT_NAME), var(a, "Activated", BPT_INT), var(a, "Ended", BPT_INT);
    var(a, "Applied", BPT_INT), var(a, "AttrChanges", BPT_INT), var(a, "LastAbility", BPT_NAME);
    a->comp_mask |= COMP_BIT(COMP_ABILITY_SYSTEM);
    CAbilitySystem* c = (CAbilitySystem*)a->comp_data[COMP_ABILITY_SYSTEM];
    memset(c, 0, sizeof *c);
    ecs_component_defaults(COMP_ABILITY_SYSTEM, c);
    c->ability[0] = g_fire, c->ability[1] = g_haste, c->flags = ABIL_KEYS | ABIL_SHOW_BAR;
    CCharacter* ch = (CCharacter*)a->comp_data[COMP_CHARACTER];
    ch->max_walk_speed = 300.0f, ch->gravity_scale = 0.0f;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    // the Custom Event TryTwice (the test calls it): Try Activate Ability (Fireball) twice: Success, then the Failure Reason (Cooldown)
    uint32_t bp = node(a, g, BPK_EVENT_CUSTOM, 0, "TryTwice"), t1 = node(a, g, BPK_ABIL_TRY, 1, NULL), s1 = node(a, g, BPK_VAR_SET, 2, "FirstOk");
    uint32_t t2 = node(a, g, BPK_ABIL_TRY, 3, NULL), s2 = node(a, g, BPK_VAR_SET, 4, "SecondReason");
    slit(g, t1, "Ability", "GA_Fireball"), slit(g, t2, "Ability", "GA_Fireball");
    link(g, bp, "", t1, ""), link(g, t1, "", s1, ""), link(g, t1, "Success", s1, "FirstOk");
    link(g, s1, "", t2, ""), link(g, t2, "", s2, ""), link(g, t2, "Failure Reason", s2, "SecondReason");
    uint32_t ea = node(a, g, BPK_EVENT_ABIL_ACTIVATED, 6, NULL);
    uint32_t sa = count_on(a, g, ea, 7, "Activated"), sl = node(a, g, BPK_VAR_SET, 10, "LastAbility");
    link(g, sa, "", sl, ""), link(g, ea, "Ability", sl, "LastAbility");
    count_on(a, g, node(a, g, BPK_EVENT_ABIL_ENDED, 12, NULL), 13, "Ended");
    count_on(a, g, node(a, g, BPK_EVENT_GE_APPLIED, 18, NULL), 19, "Applied");
    count_on(a, g, node(a, g, BPK_EVENT_ATTR_CHANGED, 24, NULL), 25, "AttrChanges");
    return done_class(a) ? a : NULL;
}
static BpAsset* make_dummy(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_AbilDummy", "Actor");
    if (!a) return NULL;
    var(a, "Applied", BPT_INT), var(a, "Removed", BPT_INT);
    a->comp_mask |= COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_SAVEABLE);
    ecs_component_defaults(COMP_HEALTH, a->comp_data[COMP_HEALTH]);
    ecs_component_defaults(COMP_SAVEABLE, a->comp_data[COMP_SAVEABLE]);
    ((CSaveable*)a->comp_data[COMP_SAVEABLE])->id = progress_new_id();
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    count_on(a, g, node(a, g, BPK_EVENT_GE_APPLIED, 0, NULL), 1, "Applied");
    count_on(a, g, node(a, g, BPK_EVENT_GE_REMOVED, 6, NULL), 7, "Removed");
    return done_class(a) ? a : NULL;
}

static float num(World* w, Entity e, const char* name) {
    BpValue v;
    return bp_instance_var(w, e, name, &v) ? v.v[0] : -12345.0f;
}
static float attr(World* w, Entity e, const char* name) {
    float v = -1.0f;
    abil_get_attr(w, e, name, &v, NULL);
    return v;
}
static float speed(World* w, Entity e) {
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    return c ? c->max_walk_speed : -1.0f;
}
static void next(int s) { g_state = s, g_state_t = re_time_seconds(); }
static bool waited(double s) { return re_time_seconds() - g_state_t >= s; }
static void fail_out(const char* what) {
    printf("[RealEngine] FAIL: ability-test: timed out waiting for %s (step %d)\n", what, g_state);
    g_ok = false, g_done = true;
}

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done || g_wait-- > 0) return;
    const EffectDef* stun = abil_effect_by_name("GE_Stun");
    switch (g_state) {
    case 0: { // the BeginPlay's two tries, the poison the Fireball's graph applied; stunned: the Fireball blocked
        g_hero = bp_player(w);
        BpAsset* dc = bp_find_name(BPA_CLASS, "BP_AbilDummy");
        for (uint32_t ai = 0; dc && ai < w->archetype_count && !g_dummy; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_BLUEPRINT))) continue;
            const CBlueprint* b = ecs_arch_column(a, COMP_BLUEPRINT);
            for (uint32_t i = 0; i < a->count; i++)
                if (b[i].asset_hash == dc->hash) g_dummy = ecs_arch_entities(a)[i];
        }
        char reason[32] = "";
        CHECK(g_hero && g_dummy, "the hero (the player) and the dummy");
        if (!g_ok) {
            g_done = true;
            return;
        }
        CHECK(fabsf(attr(w, g_hero, "Mana") - 100.0f) < 0.01f && fabsf(speed(w, g_hero) - 300.0f) < 0.5f, "the Starting Attributes: Mana %.1f, Speed %.0f",
              attr(w, g_hero, "Mana"), speed(w, g_hero));
        CHECK(bp_call_event_args(w, g_hero, "TryTwice", NULL, 0), "the hero's TryTwice");
        bp_instance_var_text(w, g_hero, "SecondReason", reason, sizeof reason);
        CHECK(abil_slot_count(w, g_hero) == 2, "the hero's two Starting Abilities (%d)", abil_slot_count(w, g_hero));
        CHECK(num(w, g_hero, "FirstOk") == 1.0f, "BeginPlay's Try Activate Ability: Success");
        CHECK(!strcmp(reason, "Cooldown"), "the second try at once: Failure Reason \"%s\" (Cooldown)", reason);
        CHECK(fabsf(attr(w, g_hero, "Mana") - 70.0f) < 0.01f, "the cost paid: Mana %.1f (70)", attr(w, g_hero, "Mana"));
        CHECK(abil_has_effect(w, g_dummy, "GE_Poison", NULL) == 1 && gt_has(w, g_dummy, "State.Poisoned", true),
              "the Fireball's graph (Event On Activate Ability) poisoned the dummy");
        CHECK(fabsf(health_get(w, g_dummy) - 90.0f) < 0.01f, "the poison's first 10 through the Health (%.1f)", health_get(w, g_dummy));
        CHECK(num(w, g_hero, "Activated") >= 1.0f && num(w, g_hero, "Ended") >= 1.0f, "On Ability Activated / Ended (%.0f / %.0f)",
              num(w, g_hero, "Activated"), num(w, g_hero, "Ended"));
        CHECK(ui_test_widget("Ability1") && ui_test_widget("ActiveEffects"), "WBP_AbilityBar on the screen (Show Ability Bar)");
        CHECK(stun && abil_apply_effect(w, g_hero, stun->hash, g_dummy) == 1 && gt_has(w, g_hero, "State.Stunned", true), "the hero stunned");
        CHECK(abil_try_activate(w, g_hero, g_fire) == ABIL_FAIL_BLOCKED, "stunned: the Fireball blocked by State.Stunned");
        next(1);
        break;
    }
    case 1: { // the stun and the cooldown over: the bar's first slot clicked
        if (gt_has(w, g_hero, "State.Stunned", true) || abil_on_cooldown(w, g_hero, g_fire)) {
            if (waited(4.0)) fail_out("the stun and the cooldown to end");
            break;
        }
        float xy[2];
        CHECK(abil_widget_slot_center(g_hero, 0, xy), "WBP_AbilityBar's first slot not drawn");
        if (!g_ok) {
            g_done = true;
            return;
        }
        g_act0 = num(w, g_hero, "Activated");
        abil_widget_test_click(true, xy[0], xy[1]);
        next(2);
        break;
    }
    case 2: { // the click cast it: the second stack; the haste on the Character
        if (num(w, g_hero, "Activated") <= g_act0) {
            if (waited(1.5)) fail_out("the click on the ability bar");
            break;
        }
        abil_widget_test_click(false, 0, 0);
        char last[32] = "";
        bp_instance_var_text(w, g_hero, "LastAbility", last, sizeof last);
        CHECK(!strcmp(last, "GA_Fireball"), "the click activated \"%s\" (GA_Fireball)", last);
        CHECK(fabsf(attr(w, g_hero, "Mana") - 40.0f) < 0.01f, "Mana %.1f (40)", attr(w, g_hero, "Mana"));
        CHECK(abil_has_effect(w, g_dummy, "State.Poisoned", NULL) == 2, "the poison stacked: %d (2)", abil_has_effect(w, g_dummy, "GE_Poison", NULL));
        CHECK(abil_try_activate(w, g_hero, g_haste) == ABIL_OK, "the haste");
        CHECK(fabsf(speed(w, g_hero) - 450.0f) < 0.5f, "Speed x1.5: the Character walks %.0f (450)", speed(w, g_hero));
        next(3);
        break;
    }
    case 3: { // the poison's ticks, then Save Progress
        if (!waited(1.2)) break;
        CHECK(health_get(w, g_dummy) <= 70.01f, "the poison every second (2 stacks): health %.1f (70 at most)", health_get(w, g_dummy));
        CHECK(num(w, g_dummy, "Applied") >= 2.0f, "the dummy's On Effect Applied (%.0f)", num(w, g_dummy, "Applied"));
        g_mana_s = attr(w, g_hero, "Mana");
        abil_has_effect(w, g_hero, "GE_Haste", &g_rem_s);
        g_cd_s = abil_cooldown_remaining(w, g_hero, g_haste, NULL, NULL, NULL);
        g_stacks_s = abil_has_effect(w, g_dummy, "GE_Poison", NULL);
        g_hp_s = health_get(w, g_dummy);
        g_tick_ms = abil_stats()->tick_ms, g_op_us = abil_stats()->op_us; // (before the load: its play starts the stats again)
        CHECK(progress_save(w, "AbilTest", "Abilities", 0), "Save Progress");
        next(4);
        break;
    }
    case 4: { // the file there: everything changed, then Load Progress
        SaveSlotInfo si;
        if (!progress_slot_info("AbilTest", &si)) {
            if (waited(3.0)) fail_out("the save's file");
            break;
        }
        abil_remove_effect(w, g_hero, "GE_Haste", 0);
        abil_set_attr(w, g_hero, "Mana", 5.0f);
        abil_remove_effect(w, g_dummy, "State.Poisoned", 0);
        CHECK(fabsf(speed(w, g_hero) - 300.0f) < 0.5f && !abil_has_effect(w, g_dummy, "GE_Poison", NULL), "changed: the haste and the poison gone");
        CHECK(num(w, g_dummy, "Removed") >= 1.0f, "the dummy's On Effect Removed (%.0f)", num(w, g_dummy, "Removed"));
        static const char* const cn[4] = { "Activated", "Ended", "Applied", "AttrChanges" };
        for (int k = 0; k < 4; k++) g_counts[k] = num(w, g_hero, cn[k]);
        g_loads = progress_stats()->loads;
        CHECK(progress_load(w, "AbilTest"), "Load Progress");
        next(5);
        break;
    }
    case 5: { // the level opened again, the load's last stage (the player's record): everything back
        if (progress_stats()->loads == g_loads) {
            if (waited(8.0)) fail_out("the load");
            break;
        }
        g_hero = bp_player(w), g_dummy = ENTITY_NONE; // (the level's actors made again)
        BpAsset* dc = bp_find_name(BPA_CLASS, "BP_AbilDummy");
        for (uint32_t ai = 0; dc && ai < w->archetype_count && !g_dummy; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_BLUEPRINT))) continue;
            const CBlueprint* b = ecs_arch_column(a, COMP_BLUEPRINT);
            for (uint32_t i = 0; i < a->count; i++)
                if (b[i].asset_hash == dc->hash) g_dummy = ecs_arch_entities(a)[i];
        }
        float rem = 0.0f, cd = abil_cooldown_remaining(w, g_hero, g_haste, NULL, NULL, NULL);
        int st = abil_has_effect(w, g_hero, "GE_Haste", &rem);
        printf("[RealEngine] ability-test: loaded: Mana %.1f (%.1f), haste %d with %.2f s (%.2f), its cooldown %.2f (%.2f), poison %d (%d), speed %.0f\n",
               attr(w, g_hero, "Mana"), g_mana_s, st, rem, g_rem_s, cd, g_cd_s, abil_has_effect(w, g_dummy, "GE_Poison", NULL), g_stacks_s, speed(w, g_hero));
        CHECK(fabsf(attr(w, g_hero, "Mana") - g_mana_s) < 0.01f, "the Mana back (%.1f)", attr(w, g_hero, "Mana"));
        CHECK(st == 1 && fabsf(rem - g_rem_s) < 0.6f, "the haste back with its time (%.2f, %.2f)", rem, g_rem_s);
        CHECK(fabsf(speed(w, g_hero) - 450.0f) < 0.5f, "and the speed with it (%.0f)", speed(w, g_hero));
        CHECK(fabsf(cd - g_cd_s) < 0.6f && cd > 1.0f, "the haste's cooldown back (%.2f, %.2f)", cd, g_cd_s);
        CHECK(abil_has_effect(w, g_dummy, "GE_Poison", NULL) == g_stacks_s && gt_has(w, g_dummy, "State.Poisoned", true),
              "the dummy's poison back with its stacks and its tag");
        CHECK(fabsf(health_get(w, g_dummy) - g_hp_s) < 10.01f, "the dummy's health back (%.1f, %.1f)", health_get(w, g_dummy), g_hp_s);
        g_done = true;
        break;
    }
    default: g_done = true; break;
    }
}

bool ability_test_setup(Editor* ed) {
    g_done = true; // (until it plays: a failure here never holds the measure back)
    if (abil_selftest()) {
        printf("[RealEngine] FAIL: ability-test: the self-test failed\n");
        return false;
    }
    World* w = &ed->world;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/AbilTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    const char* F = "/Content/AbilTest";
    bool fx = make_effect(F, "GE_Poison", "Has Duration", 8.0f, 1.0f, "", 10.0f, 3, "State.Poisoned", true) &&
              make_effect(F, "GE_Haste", "Has Duration", 20.0f, 0.0f, "Speed x1.5", 0.0f, 1, "State.Hasted", false) &&
              make_effect(F, "GE_Stun", "Has Duration", 0.6f, 0.0f, "", 0.0f, 1, "State.Stunned", true);
    BpAsset* dummy = fx ? make_dummy(F) : NULL; // (first: the Fireball's graph names its class)
    BpAsset* fire = dummy ? make_fireball(F) : NULL;
    BpAsset* haste = fire ? make_haste(F) : NULL;
    g_fire = fire ? fire->hash : 0, g_haste = haste ? haste->hash : 0;
    BpAsset* hero = haste ? make_hero(F) : NULL;
    if (!hero || !abil_bar_asset()) {
        printf("[RealEngine] FAIL: ability-test: the assets (effects, abilities, BP_AbilHero, BP_AbilDummy, WBP_AbilityBar)\n");
        return false;
    }
    abil_defs_refresh();
    const AbilityDef* fd = abil_ability_by_name("GA_Fireball");
    const EffectDef* pd = abil_effect_by_name("GE_Poison");
    CHECK(fd && fd->has_graph && fd->cost == 30.0f && fd->nblock == 1 && fd->ntags == 1, "GA_Fireball's class defaults read wrong");
    CHECK(pd && pd->period == 1.0f && pd->damage == 10.0f && pd->max_stacks == 3 && pd->ngranted == 1 && pd->debuff, "GE_Poison's fields read wrong");
    const AbilityDef* hd = abil_ability_by_name("Haste");
    CHECK(hd && hd->self_effect && hd->has_graph, "GA_Haste (by its Display Name) and its Self Effect");
    const float Y = 3000.0f;
    Entity he = bp_spawn(w, hero, (float[3]){ 0, Y, 120 }, (float[3]){ 0, 0, 0 });
    Entity de = bp_spawn(w, dummy, (float[3]){ 400, Y, 50 }, (float[3]){ 0, 0, 0 });
    if (!he || !de) {
        printf("[RealEngine] FAIL: ability-test: the actors\n");
        return false;
    }
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/L_AbilTest.rlevel", dir);
    if (!editor_save_level(ed, lp, 0.0)) return printf("[RealEngine] FAIL: ability-test: the level could not be saved in %s\n", lp), false;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = Y - 900.0f, ed->cam.pos[2] = 300.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
    ed->test_step = step;
    g_state_t = re_time_seconds();
    g_done = !g_ok;
    editor_play(ed, 0.0);
    return g_ok;
}

bool ability_test_done(void) { return g_done; }

bool ability_test_check(Editor* ed) {
    if (!g_done) printf("[RealEngine] FAIL: ability-test: stopped at step %d\n", g_state), g_ok = false;
    World* w = &ed->world;
    const AbilStats* st = abil_stats();
    printf("[RealEngine] Ability cost: abil_tick %.4f ms a frame (%u systems, %u effects), an operation %.2f us; since play %u applied, %u periodic, %u activations, %u failed, %u events; hero: %.0f activated, %.0f ended, %.0f applied, %.0f attribute changes\n",
           g_tick_ms, st->systems, st->effects, g_op_us, st->applied, st->periodic, st->activations, st->failures, st->events, g_counts[0], g_counts[1], g_counts[2],
           g_counts[3]);
    CHECK(g_tick_ms < 0.2, "abil_tick costs %.4f ms a frame (0.2 at most)", g_tick_ms);
    CHECK(g_counts[0] >= 3.0f && g_counts[1] >= 3.0f, "the hero's events: 3 activations and ends at least (%.0f, %.0f)", g_counts[0], g_counts[1]);
    CHECK(g_counts[2] >= 2.0f && g_counts[3] >= 3.0f, "On Effect Applied and On Attribute Changed on the hero (%.0f, %.0f)", g_counts[2], g_counts[3]);
    editor_stop(ed, 0.0);
    progress_slot_delete("AbilTest");
    printf("[RealEngine] Ability test: %s\n", g_ok ? "OK" : "FAILED");
    return g_ok;
}
