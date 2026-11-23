// Abilities and effects (ability.h, notas-dev/habilidades.md): the definitions read from their assets, the Ability
// Systems and their active effects in fixed SoA arrays, the rules (costs, cooldowns, charges, casts, tags, stacking,
// periods, modifiers), the events, the saves' records, the Debug Viewer, WBP_AbilityBar and the self-test.
#include "ability.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "debug_viewer.h"
#include "gameplay_tags.h"
#include "health.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

const char* const g_abil_fail_names[ABIL_FAIL_COUNT] = { "", "Not Granted", "Active", "Blocked", "Cooldown", "Cost", "Dead" };

#define NAMES_MAX 256   // attribute names known
#define TAGS_MAX 512    // tag names known
#define QUEUE_MAX 2048  // events waiting for the end of the operation / the frame
#define DUE_MAX 16384   // periodic runs in a frame
#define LATER_MAX 1024  // casts finished, cancels, keys in a frame
enum { G_IDLE = 0, G_CASTING, G_ACTIVE };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool same_loose(const char* a, const char* b) { // case, spaces and '_' ignored ("Max Mana" is "MaxMana")
    for (;;) {
        while (*a == ' ' || *a == '_') a++;
        while (*b == ' ' || *b == '_') b++;
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
        if (!*a) return true;
        a++, b++;
    }
}
static uint32_t lhash(const char* s) { // (FNV-1a of the lowercase name)
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (uint32_t)lower((unsigned char)*s)) * 16777619u;
    return h ? h : 1u;
}

// --- Definitions ----------------------------------------------------------------------------------------------------------
static struct {
    AbilityDef ab[ABIL_MAX_DEFS];
    EffectDef fx[ABIL_MAX_DEFS];
    uint32_t nab, nfx;
    bool ready;
    char attr[NAMES_MAX][32]; // [0]: none
    uint32_t nattr;
    char tag[TAGS_MAX][GT_TAG_LEN];
    uint32_t ntag;
    uint16_t speed, damage, defense; // the attributes the engine reads
} D;

static uint16_t attr_id(const char* name) { // interned for good (the ids stay across plays); 0: none
    char n[32];
    size_t k = 0;
    for (const char* p = name ? name : ""; *p && k + 1 < sizeof n; p++)
        if (!isspace((unsigned char)*p)) n[k++] = *p;
    n[k] = 0;
    if (!n[0] || !_stricmp(n, "None")) return 0;
    if (!D.nattr) D.nattr = 1;
    for (uint32_t i = 1; i < D.nattr; i++)
        if (same_loose(D.attr[i], n)) return (uint16_t)i;
    if (D.nattr >= NAMES_MAX) return 0;
    snprintf(D.attr[D.nattr], sizeof D.attr[0], "%s", n);
    return (uint16_t)D.nattr++;
}
const char* abil_attr_name(uint16_t id) { return id && id < D.nattr ? D.attr[id] : ""; }
static uint16_t tag_id(const char* tag) {
    char t[GT_TAG_LEN];
    if (!gt_clean(tag, t, sizeof t)) return 0;
    if (!D.ntag) D.ntag = 1;
    for (uint32_t i = 1; i < D.ntag; i++)
        if (gt_matches(D.tag[i], t, true)) return (uint16_t)i;
    if (D.ntag >= TAGS_MAX) return 0;
    snprintf(D.tag[D.ntag], GT_TAG_LEN, "%s", t);
    return (uint16_t)D.ntag++;
}
const char* abil_tag_name(uint16_t id) { return id && id < D.ntag ? D.tag[id] : ""; }
static void builtins(void) {
    if (!D.speed) D.speed = attr_id("Speed"), D.damage = attr_id("Damage"), D.defense = attr_id("Defense");
}
static uint8_t parse_tags(const char* s, uint16_t* out) { // "A.B, C.D" (commas, spaces, ';' or '|')
    uint8_t n = 0;
    while (s && *s && n < ABIL_DEF_TAGS) {
        while (*s == ',' || *s == ';' || *s == ' ' || *s == '|') s++;
        size_t k = 0;
        while (s[k] && s[k] != ',' && s[k] != ';' && s[k] != ' ' && s[k] != '|') k++;
        if (!k) break;
        char raw[64];
        size_t c = k < sizeof raw - 1 ? k : sizeof raw - 1;
        memcpy(raw, s, c), raw[c] = 0;
        uint16_t id = tag_id(raw);
        if (id) out[n++] = id;
        s += k;
    }
    return n;
}
// "Speed x1.5, Defense +10, Mana -5, Damage =2, Armor /2": an attribute, an operation, a number
static uint8_t parse_mods(const char* s, EffectMod* out) {
    uint8_t n = 0;
    while (s && *s && n < ABIL_EFFECT_MODS) {
        while (*s == ',' || *s == ';' || isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char name[32];
        size_t k = 0;
        while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '.') && k + 1 < sizeof name) name[k++] = *s++;
        name[k] = 0;
        while (*s == ' ') s++;
        char op = *s;
        uint8_t o = GE_OP_ADD;
        bool neg = false, inv = false;
        if (op == 'x' || op == 'X' || op == '*') o = GE_OP_MUL, s++;
        else if (op == '/') o = GE_OP_MUL, inv = true, s++;
        else if (op == '=') o = GE_OP_OVERRIDE, s++;
        else if (op == '+') s++;
        else if (op == '-') neg = true, s++;
        char* end = NULL;
        float v = strtof(s, &end);
        bool ok = end && end != s && name[0];
        if (end) s = end;
        while (*s && *s != ',' && *s != ';') s++; // (the rest of a bad one skipped)
        if (!ok) continue;
        if (neg) v = -v;
        if (inv) v = v != 0.0f ? 1.0f / v : 1.0f;
        uint16_t id = attr_id(name);
        if (!id) continue;
        out[n].attr = id, out[n].op = o, out[n].value = v;
        n++;
    }
    return n;
}

bool abil_is_ability_class(const BpAsset* a) { return a && a->kind == BPA_CLASS && bp_native_of(a) == BPN_GAMEPLAY_ABILITY; }
bool abil_is_effect_asset(const BpAsset* a) { return a && a->kind == BPA_DATA && same_loose(a->parent, "Gameplay Effect"); }

// A class's (or its parents') variable by its loose name: the class defaults of a Gameplay Ability
static const BpVar* cvar(const BpAsset* a, const char* name) {
    for (int depth = 0; a && depth < 8; depth++) {
        for (uint32_t i = 0; i < a->nvars; i++)
            if (same_loose(a->vars[i].name, name)) return &a->vars[i];
        a = a->kind == BPA_CLASS ? bp_find_name(BPA_CLASS, a->parent) : NULL;
    }
    return NULL;
}
static const char* cstr(const BpAsset* a, const char* name, const char* def) {
    const BpVar* v = cvar(a, name);
    return v && bp_type_stringish(v->type) ? v->sdef : def;
}
static float cnum(const BpAsset* a, const char* name, float def) {
    const BpVar* v = cvar(a, name);
    return v && !bp_type_stringish(v->type) ? v->def.v[0] : def;
}
static bool has_activate(const BpAsset* a) {
    for (int depth = 0; a && depth < 8; depth++) {
        for (uint32_t g = 0; g < a->ngraphs; g++)
            for (uint32_t k = 0; k < a->graphs[g].nnodes; k++)
                if (a->graphs[g].nodes[k].kind == BPK_EVENT_ABIL_ACTIVATE) return true;
        a = bp_find_name(BPA_CLASS, a->parent);
    }
    return false;
}

static void add_field(BpAsset* a, const char* name, uint8_t type, float v, const char* s, const char* cat, const char* tip) {
    if (bp_var_index(a, name) >= 0) return;
    int k = bp_add_var(a, name, type);
    if (k < 0) return;
    BpVar* x = &a->vars[k];
    x->def.v[0] = v;
    if (s) snprintf(x->sdef, sizeof x->sdef, "%s", s);
    snprintf(x->category, sizeof x->category, "%s", cat);
    if (tip) snprintf(x->tooltip, sizeof x->tooltip, "%s", tip);
}

void abil_class_defaults(BpAsset* a) {
    if (!a || a->kind != BPA_CLASS) return;
    add_field(a, "Display Name", BPT_TEXT, 0, "New Ability", "Ability", NULL);
    add_field(a, "Description", BPT_TEXT, 0, "", "Ability", NULL);
    add_field(a, "Icon", BPT_STRING, 0, "", "Ability", "A texture: /Content/...png");
    add_field(a, "Cost Attribute", BPT_NAME, 0, "Mana", "Ability", "What it spends: Mana, Stamina...");
    add_field(a, "Cost", BPT_FLOAT, 0.0f, NULL, "Ability", NULL);
    add_field(a, "Cooldown", BPT_FLOAT, 1.0f, NULL, "Ability", "Seconds until a charge comes back");
    add_field(a, "Charges", BPT_INT, 1.0f, NULL, "Ability", NULL);
    add_field(a, "Cast Time", BPT_FLOAT, 0.0f, NULL, "Ability", "Seconds before it acts (0: at once)");
    add_field(a, "Ability Tags", BPT_STRING, 0, "Ability", "Ability", "What it is: Ability.Attack.Fire");
    add_field(a, "Activation Owned Tags", BPT_STRING, 0, "", "Ability", "Tags the owner has while it runs");
    add_field(a, "Blocked By Tags", BPT_STRING, 0, "State.Stunned", "Ability", "The owner with one: it can't start");
    add_field(a, "Cancel Abilities With Tags", BPT_STRING, 0, "", "Ability", "Running abilities with them stop");
    add_field(a, "Self Effect", BPT_NAME, 0, "", "Ability", "A Gameplay Effect on the owner when it acts");
    // its Event Graph: Event On Activate Ability -> End Ability (the user's work goes between)
    BpGraph* g = a->ngraphs ? &a->graphs[0] : NULL;
    if (!g) return;
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    BpNode* ev = bp_add_node(a, g, BPK_EVENT_ABIL_ACTIVATE, 0, 0, NULL, NULL);
    uint32_t evid = ev ? ev->id : 0;
    BpNode* en = bp_add_node(a, g, BPK_ABIL_END, 420, 0, NULL, NULL);
    if (evid && en) bp_link(g, evid, 0, en->id, 0);
}

void abil_effect_fields(BpAsset* a) {
    if (!a) return;
    add_field(a, "Display Name", BPT_TEXT, 0, "New Effect", "Display", NULL);
    add_field(a, "Description", BPT_TEXT, 0, "", "Display", NULL);
    add_field(a, "Icon", BPT_STRING, 0, "", "Display", "A texture: /Content/...png");
    add_field(a, "Is Debuff", BPT_BOOL, 0.0f, NULL, "Display", "Shown red in the lists");
    add_field(a, "Duration Policy", BPT_NAME, 0, "Has Duration", "Rules", "Instant, Has Duration or Infinite");
    add_field(a, "Duration", BPT_FLOAT, 5.0f, NULL, "Rules", "Seconds (Has Duration)");
    add_field(a, "Period", BPT_FLOAT, 0.0f, NULL, "Rules", "Seconds between its runs (0: not periodic)");
    add_field(a, "Modifiers", BPT_STRING, 0, "Speed x1.5", "Rules", "Speed x1.5, Defense +10, Mana =50");
    add_field(a, "Damage", BPT_FLOAT, 0.0f, NULL, "Rules", "A period's damage (Apply Damage)");
    add_field(a, "Damage Type", BPT_NAME, 0, "Damage", "Rules", NULL);
    add_field(a, "Heal", BPT_FLOAT, 0.0f, NULL, "Rules", "A period's healing (Heal)");
    add_field(a, "Max Stacks", BPT_INT, 1.0f, NULL, "Stacking", "0: each its own; 1: one; N: up to N stacks");
    add_field(a, "Refresh Duration", BPT_BOOL, 1.0f, NULL, "Stacking", "A new stack restarts the time");
    add_field(a, "Granted Tags", BPT_STRING, 0, "", "Tags", "The target has them while active");
    add_field(a, "Required Tags", BPT_STRING, 0, "", "Tags", "Applied only to a target with all of them");
}

static uint8_t policy_of(const char* s) {
    if (!s || !s[0]) return GE_DURATION;
    if (same_loose(s, "Instant")) return GE_INSTANT;
    if (same_loose(s, "Infinite")) return GE_INFINITE;
    return GE_DURATION;
}

static void read_ability(const BpAsset* a, AbilityDef* d) {
    memset(d, 0, sizeof *d);
    d->hash = a->hash;
    snprintf(d->name, sizeof d->name, "%s", a->name);
    d->name_hash = lhash(a->name);
    snprintf(d->display, sizeof d->display, "%s", cstr(a, "Display Name", a->name));
    if (!d->display[0]) snprintf(d->display, sizeof d->display, "%s", a->name);
    snprintf(d->description, sizeof d->description, "%s", cstr(a, "Description", ""));
    snprintf(d->icon, sizeof d->icon, "%s", cstr(a, "Icon", ""));
    d->cost_attr = attr_id(cstr(a, "Cost Attribute", ""));
    d->cost = fmaxf(cnum(a, "Cost", 0.0f), 0.0f);
    d->cooldown = fmaxf(cnum(a, "Cooldown", 0.0f), 0.0f);
    int ch = (int)cnum(a, "Charges", 1.0f);
    d->charges = ch < 1 ? 1 : ch > 99 ? 99 : ch;
    d->cast_time = fmaxf(cnum(a, "Cast Time", 0.0f), 0.0f);
    d->ntags = parse_tags(cstr(a, "Ability Tags", ""), d->tags);
    d->nowned = parse_tags(cstr(a, "Activation Owned Tags", ""), d->owned);
    d->nblock = parse_tags(cstr(a, "Blocked By Tags", ""), d->block);
    d->ncancel = parse_tags(cstr(a, "Cancel Abilities With Tags", ""), d->cancel);
    const char* se = cstr(a, "Self Effect", "");
    const BpAsset* fx = se[0] && _stricmp(se, "None") ? bp_find_name(BPA_DATA, se) : NULL;
    d->self_effect = fx ? fx->hash : 0;
    d->has_graph = has_activate(a);
}
static void read_effect(const BpAsset* a, EffectDef* d) {
    memset(d, 0, sizeof *d);
    d->hash = a->hash;
    snprintf(d->name, sizeof d->name, "%s", a->name);
    d->name_hash = lhash(a->name);
    snprintf(d->display, sizeof d->display, "%s", cstr(a, "Display Name", a->name));
    if (!d->display[0]) snprintf(d->display, sizeof d->display, "%s", a->name);
    snprintf(d->description, sizeof d->description, "%s", cstr(a, "Description", ""));
    snprintf(d->icon, sizeof d->icon, "%s", cstr(a, "Icon", ""));
    d->policy = policy_of(cstr(a, "Duration Policy", "Has Duration"));
    d->duration = fmaxf(cnum(a, "Duration", 0.0f), 0.0f);
    d->period = fmaxf(cnum(a, "Period", 0.0f), 0.0f);
    if (d->policy == GE_INSTANT) d->period = 0.0f;
    if (d->period > 0.0f && d->period < 0.01f) d->period = 0.01f; // (at most 100 runs a second)
    d->damage = fmaxf(cnum(a, "Damage", 0.0f), 0.0f);
    d->heal = fmaxf(cnum(a, "Heal", 0.0f), 0.0f);
    snprintf(d->damage_type, sizeof d->damage_type, "%s", cstr(a, "Damage Type", ""));
    d->nmods = parse_mods(cstr(a, "Modifiers", ""), d->mods);
    int ms = (int)cnum(a, "Max Stacks", 1.0f);
    d->max_stacks = ms < 0 ? 0 : ms > 999 ? 999 : ms;
    d->refresh = cnum(a, "Refresh Duration", 1.0f) != 0.0f;
    d->debuff = cnum(a, "Is Debuff", 0.0f) != 0.0f;
    d->ngranted = parse_tags(cstr(a, "Granted Tags", ""), d->granted);
    d->nrequired = parse_tags(cstr(a, "Required Tags", ""), d->required);
}

void abil_defs_refresh(void) {
    builtins();
    bp_find_name(BPA_DATA, "\x01"); // (the Content folder scanned once: every asset known)
    D.nab = D.nfx = 0;
    for (uint32_t i = 0; i < bp_asset_count(); i++) { // the effects first (an ability's Self Effect names one)
        const BpAsset* a = bp_asset_at(i);
        if (abil_is_effect_asset(a) && D.nfx < ABIL_MAX_DEFS) read_effect(a, &D.fx[D.nfx++]);
    }
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        const BpAsset* a = bp_asset_at(i);
        if (abil_is_ability_class(a) && D.nab < ABIL_MAX_DEFS) read_ability(a, &D.ab[D.nab++]);
    }
    D.ready = true;
}
static void ready(void) {
    if (!D.ready) abil_defs_refresh();
}
uint32_t abil_ability_count(void) { return ready(), D.nab; }
const AbilityDef* abil_ability_at(uint32_t i) { return ready(), i < D.nab ? &D.ab[i] : NULL; }
uint32_t abil_effect_count(void) { return ready(), D.nfx; }
const EffectDef* abil_effect_at(uint32_t i) { return ready(), i < D.nfx ? &D.fx[i] : NULL; }
const AbilityDef* abil_ability_by_hash(uint32_t hash) {
    ready();
    for (uint32_t i = 0; hash && i < D.nab; i++)
        if (D.ab[i].hash == hash) return &D.ab[i];
    return NULL;
}
const EffectDef* abil_effect_by_hash(uint32_t hash) {
    ready();
    for (uint32_t i = 0; hash && i < D.nfx; i++)
        if (D.fx[i].hash == hash) return &D.fx[i];
    return NULL;
}
const AbilityDef* abil_ability_by_name(const char* name) {
    if (!name || !name[0] || !_stricmp(name, "None")) return NULL;
    ready();
    uint32_t h = lhash(name);
    for (uint32_t i = 0; i < D.nab; i++)
        if (D.ab[i].name_hash == h && !_stricmp(D.ab[i].name, name)) return &D.ab[i];
    for (uint32_t i = 0; i < D.nab; i++)
        if (!_stricmp(D.ab[i].display, name)) return &D.ab[i];
    return NULL;
}
const EffectDef* abil_effect_by_name(const char* name) {
    if (!name || !name[0] || !_stricmp(name, "None")) return NULL;
    ready();
    uint32_t h = lhash(name);
    for (uint32_t i = 0; i < D.nfx; i++)
        if (D.fx[i].name_hash == h && !_stricmp(D.fx[i].name, name)) return &D.fx[i];
    for (uint32_t i = 0; i < D.nfx; i++)
        if (!_stricmp(D.fx[i].display, name)) return &D.fx[i];
    return NULL;
}

// --- The systems and the effects (SoA, fixed) ----------------------------------------------------------------------------
#define NS ABIL_MAX_SYSTEMS
#define NA ABIL_MAX_ATTRS
#define NG ABIL_MAX_GRANTED
#define NT 8 // tags a system's effects and abilities grant at once (counted)
static struct {
    uint32_t n;                           // high water
    uint16_t freel[NS];
    uint32_t nfree;
    Entity ent[NS];
    uint32_t flags[NS];
    int32_t first[NS];                    // its first effect (-1: none)
    uint8_t dirty[NS];
    uint8_t nattr[NS];
    uint16_t aid[NS][NA];
    int8_t amax[NS][NA];                  // the index of MaxX for X (-1: none)
    float base[NS][NA], cur[NS][NA];
    uint8_t ngrant[NS];
    uint16_t gdef[NS][NG];                // D.ab's index + 1
    uint8_t gstate[NS][NG], gcharges[NS][NG];
    float grecharge[NS][NG], gcast[NS][NG];
    Entity gobj[NS][NG];                  // its object (the class's instance: Event On Activate Ability)
    uint16_t tid[NS][NT], tcnt[NS][NT];   // granted tags and how many grant each (bit 15: the actor had it before)
} S;
#define NE ABIL_MAX_EFFECTS
static struct {
    uint32_t n;
    uint16_t sys[NE], def[NE], stacks[NE];
    float left[NE], period_left[NE];      // left: +inf for Infinite
    Entity src[NE];
    int32_t next[NE], prev[NE];
} F;
typedef struct AbEv { Entity to, e; uint16_t kind; float a, b; char s[48]; } AbEv;
typedef struct Due { Entity target, src; uint16_t sys, def, stacks; } Due;
typedef struct Later { uint16_t sys, slot, what; } Later; // what: 0 the cast is done, 1 cancel (interrupted), 2 a key
static struct {
    bool live, in_tick, flushing;
    World* w;
    AbEv q[QUEUE_MAX];
    uint32_t nq;
    Due due[DUE_MAX];
    uint32_t ndue;
    Later later[LATER_MAX];
    uint32_t nlater;
    uint32_t keys_prev; // the player's keys 1..6 last frame
    AbilStats st;
    AbilTestHook hook;
    double profile_until;
} A;

void abil_set_test_hook(AbilTestHook h) { A.hook = h; }

static void push_ev(Entity to, uint16_t kind, const char* s, float a, float b, Entity e) {
    if (!to || A.nq >= QUEUE_MAX) return;
    AbEv* v = &A.q[A.nq++];
    v->to = to, v->kind = kind, v->a = a, v->b = b, v->e = e;
    snprintf(v->s, sizeof v->s, "%s", s ? s : "");
}
// The events waiting, sent once the operation (or the frame) is done: game code may run there and call back in
static void flush(World* w) {
    if (A.flushing || A.in_tick || !A.nq) return;
    A.flushing = true;
    for (uint32_t k = 0; k < A.nq; k++) {
        AbEv v = A.q[k];
        A.st.events++;
        if (A.hook) {
            A.hook(v.to, v.kind, v.s, v.a, v.b, v.e);
            continue;
        }
        if (!w || !ecs_alive(w, v.to)) continue;
        BpValue args[3];
        memset(args, 0, sizeof args);
        uint32_t n = 0;
        switch (v.kind) {
        case BPK_EVENT_ABIL_ACTIVATE: memcpy(&args[0].v[0], &v.e, sizeof v.e), n = 1; break;
        case BPK_EVENT_ABIL_ACTIVATED: case BPK_EVENT_GE_REMOVED: bp_make_string(&args[0], v.s), n = 1; break;
        case BPK_EVENT_ABIL_ENDED: bp_make_string(&args[0], v.s), args[1].v[0] = v.a, n = 2; break;
        case BPK_EVENT_GE_APPLIED: bp_make_string(&args[0], v.s), args[1].v[0] = v.a, memcpy(&args[2].v[0], &v.e, sizeof v.e), n = 3; break;
        case BPK_EVENT_ATTR_CHANGED: bp_make_string(&args[0], v.s), args[1].v[0] = v.a, args[2].v[0] = v.b, n = 3; break;
        default: break;
        }
        bp_fire_actor_event(w, v.to, v.kind, args, n);
    }
    A.nq = 0;
    A.flushing = false;
}

static bool sys_ok(int s) { return s >= 0 && (uint32_t)s < S.n && S.ent[s] != ENTITY_NONE; }
static int sys_find(World* w, Entity e) {
    if (!A.live || !w || !e || !ecs_alive(w, e)) return -1;
    const CAbilitySystem* c = ecs_get(w, e, COMP_ABILITY_SYSTEM);
    if (!c || !c->handle || c->handle > S.n || S.ent[c->handle - 1] != e) return -1;
    return (int)c->handle - 1;
}
static int attr_idx(int s, uint16_t id) {
    for (int i = 0; id && i < S.nattr[s]; i++)
        if (S.aid[s][i] == id) return i;
    return -1;
}
static void link_max(int s) { // X -> MaxX (by name)
    for (int i = 0; i < S.nattr[s]; i++) {
        S.amax[s][i] = -1;
        char mx[40];
        snprintf(mx, sizeof mx, "Max%s", abil_attr_name(S.aid[s][i]));
        for (int j = 0; j < S.nattr[s]; j++)
            if (j != i && same_loose(abil_attr_name(S.aid[s][j]), mx)) S.amax[s][i] = (int8_t)j;
    }
}
static float attr_default(uint16_t id) { return id == D.damage ? 1.0f : 0.0f; }
static int attr_make(World* w, int s, uint16_t id, float base) { // its index (made with base when missing); -1: full
    int i = attr_idx(s, id);
    if (i >= 0 || !id) return i;
    if (S.nattr[s] >= NA) return -1;
    i = S.nattr[s]++;
    S.aid[s][i] = id;
    if (id == D.speed && w) { // (a Character's: its Max Walk Speed)
        const CCharacter* ch = ecs_get(w, S.ent[s], COMP_CHARACTER);
        if (ch) base = ch->max_walk_speed;
    }
    S.base[s][i] = S.cur[s][i] = base;
    link_max(s);
    return i;
}
static float clamp_attr(int s, int i, float v) {
    int m = S.amax[s][i];
    if (m >= 0) v = fminf(fmaxf(v, 0.0f), S.cur[s][m]);
    if (S.aid[s][i] == D.speed) v = fmaxf(v, 0.0f);
    return v;
}

// The current values: (base + the adds) x the multipliers, or the last override; X within 0..MaxX. Changes: events, the Character
static void recompute(World* w, int s) {
    float add[NA], mul[NA], ovr[NA];
    bool has_ovr[NA];
    int n = S.nattr[s];
    for (int i = 0; i < n; i++) add[i] = 0.0f, mul[i] = 1.0f, has_ovr[i] = false, ovr[i] = 0.0f;
    for (int32_t f = S.first[s]; f >= 0; f = F.next[f]) {
        const EffectDef* d = &D.fx[F.def[f]];
        if (d->policy == GE_INSTANT || d->period > 0.0f) continue; // (those change the base)
        float st = (float)F.stacks[f];
        for (int m = 0; m < d->nmods; m++) {
            int i = attr_idx(s, d->mods[m].attr);
            if (i < 0) continue;
            if (d->mods[m].op == GE_OP_ADD) add[i] += d->mods[m].value * st;
            else if (d->mods[m].op == GE_OP_MUL) mul[i] *= st == 1.0f ? d->mods[m].value : powf(d->mods[m].value, st);
            else has_ovr[i] = true, ovr[i] = d->mods[m].value;
        }
    }
    float nv[NA];
    for (int i = 0; i < n; i++) nv[i] = has_ovr[i] ? ovr[i] : (S.base[s][i] + add[i]) * mul[i];
    for (int pass = 0; pass < 2; pass++) // (the Max ones first: a MaxX may be modified too)
        for (int i = 0; i < n; i++) {
            bool is_max = false;
            for (int j = 0; j < n; j++) is_max |= S.amax[s][j] == i;
            if ((pass == 0) != is_max) continue;
            float v = nv[i];
            int m = S.amax[s][i];
            if (m >= 0) v = fminf(fmaxf(v, 0.0f), S.cur[s][m]), S.base[s][i] = fminf(fmaxf(S.base[s][i], 0.0f), S.cur[s][m]);
            if (S.aid[s][i] == D.speed) v = fmaxf(v, 0.0f);
            float old = S.cur[s][i];
            if (v == old) continue;
            S.cur[s][i] = v;
            push_ev(S.ent[s], BPK_EVENT_ATTR_CHANGED, abil_attr_name(S.aid[s][i]), v, old, ENTITY_NONE);
            if (S.aid[s][i] == D.speed && w) {
                CCharacter* ch = ecs_get(w, S.ent[s], COMP_CHARACTER);
                if (ch) ch->max_walk_speed = v;
            }
        }
    S.dirty[s] = 0;
}

// The tags a system grants (counted: two effects with State.Burning, removed when the last goes)
static void tag_grant(World* w, int s, uint16_t id) {
    if (!id) return;
    int fr = -1;
    for (int k = 0; k < NT; k++) {
        if (S.tid[s][k] == id && (S.tcnt[s][k] & 0x7FFF)) {
            S.tcnt[s][k]++;
            return;
        }
        if (fr < 0 && !(S.tcnt[s][k] & 0x7FFF)) fr = k;
    }
    if (fr < 0) return;
    bool had = gt_has(w, S.ent[s], abil_tag_name(id), true);
    S.tid[s][fr] = id, S.tcnt[s][fr] = (uint16_t)(1u | (had ? 0x8000u : 0u));
    if (!had) gt_add(w, S.ent[s], abil_tag_name(id));
}
static void tag_ungrant(World* w, int s, uint16_t id) {
    for (int k = 0; id && k < NT; k++) {
        if (S.tid[s][k] != id || !(S.tcnt[s][k] & 0x7FFF)) continue;
        S.tcnt[s][k]--;
        if (!(S.tcnt[s][k] & 0x7FFF)) {
            if (!(S.tcnt[s][k] & 0x8000) && w && ecs_alive(w, S.ent[s])) gt_remove(w, S.ent[s], abil_tag_name(id));
            S.tcnt[s][k] = 0, S.tid[s][k] = 0;
        }
        return;
    }
}
static bool has_any(World* w, Entity e, const uint16_t* tags, int n) {
    for (int k = 0; k < n; k++)
        if (gt_has(w, e, abil_tag_name(tags[k]), false)) return true;
    return false;
}

// --- Effects: alloc, the system's list, removal (no game code: the events wait in the queue) ---
static int32_t fx_alloc(int s, uint16_t def) {
    if (F.n >= NE) return -1;
    int32_t f = (int32_t)F.n++;
    F.sys[f] = (uint16_t)s, F.def[f] = def, F.stacks[f] = 1;
    F.left[f] = INFINITY, F.period_left[f] = 0.0f, F.src[f] = ENTITY_NONE;
    F.prev[f] = -1, F.next[f] = S.first[s];
    if (S.first[s] >= 0) F.prev[S.first[s]] = f;
    S.first[s] = f;
    return f;
}
static void fx_unlink(int32_t f) {
    int s = F.sys[f];
    if (F.prev[f] >= 0) F.next[F.prev[f]] = F.next[f];
    else S.first[s] = F.next[f];
    if (F.next[f] >= 0) F.prev[F.next[f]] = F.prev[f];
}
static void fx_move(int32_t from, int32_t to) { // (the last one into a freed place: its neighbours told)
    F.sys[to] = F.sys[from], F.def[to] = F.def[from], F.stacks[to] = F.stacks[from];
    F.left[to] = F.left[from], F.period_left[to] = F.period_left[from], F.src[to] = F.src[from];
    F.prev[to] = F.prev[from], F.next[to] = F.next[from];
    if (F.prev[to] >= 0) F.next[F.prev[to]] = to;
    else S.first[F.sys[to]] = to;
    if (F.next[to] >= 0) F.prev[F.next[to]] = to;
}
static void fx_free(World* w, int32_t f, bool event) {
    int s = F.sys[f];
    const EffectDef* d = &D.fx[F.def[f]];
    fx_unlink(f);
    for (int k = 0; k < d->ngranted; k++) tag_ungrant(w, s, d->granted[k]);
    int32_t last = (int32_t)F.n - 1;
    if (f != last) fx_move(last, f);
    F.n--;
    S.dirty[s] = 1;
    A.st.removed++;
    if (event) push_ev(S.ent[s], BPK_EVENT_GE_REMOVED, d->name, 0, 0, ENTITY_NONE);
}

// The modifiers onto the base values (an Instant effect, a period's run), times its stacks
static void mods_to_base(World* w, int s, const EffectDef* d, int stacks) {
    for (int m = 0; m < d->nmods; m++) {
        int i = attr_make(w, s, d->mods[m].attr, attr_default(d->mods[m].attr));
        if (i < 0) continue;
        float v = S.base[s][i];
        for (int k = 0; k < stacks; k++) {
            if (d->mods[m].op == GE_OP_ADD) v += d->mods[m].value;
            else if (d->mods[m].op == GE_OP_MUL) v *= d->mods[m].value;
            else v = d->mods[m].value;
        }
        S.base[s][i] = clamp_attr(s, i, v);
    }
    S.dirty[s] = 1;
}
// An application's or a period's run: the modifiers onto the base (periodic / instant), the Damage and Heal (health.h)
static void execute(World* w, int s, const EffectDef* d, int stacks, Entity target, Entity src) {
    A.st.periodic++;
    if (d->policy == GE_INSTANT || d->period > 0.0f) mods_to_base(w, s, d, stacks);
    if (d->damage > 0.0f) health_apply_damage(w, target, d->damage * (float)stacks, d->damage_type, src ? src : target, src ? src : target);
    if (d->heal > 0.0f && ecs_alive(w, target)) health_heal(w, target, d->heal * (float)stacks, src ? src : target);
}

// --- The systems: made from the component, freed when the actor goes ---
static void sys_free(World* w, int s) {
    while (S.first[s] >= 0) fx_free(w, S.first[s], false);
    for (int g = 0; g < S.ngrant[s]; g++)
        if (S.gobj[s][g] && w && ecs_alive(w, S.gobj[s][g])) bp_destroy_actor(w, S.gobj[s][g], true);
    S.ent[s] = ENTITY_NONE;
    S.ngrant[s] = S.nattr[s] = 0;
    if (A.nlater) // (its waiting work dropped)
        for (uint32_t k = 0; k < A.nlater; k++)
            if (A.later[k].sys == s) A.later[k].what = 0xFFFF;
    if (S.nfree < NS) S.freel[S.nfree++] = (uint16_t)s;
}
static int sys_alloc(void) {
    if (S.nfree) return S.freel[--S.nfree];
    return S.n < NS ? (int)S.n++ : -1;
}
static void parse_attrs(World* w, int s, const char* t) { // "Mana 100, MaxMana 100" (also "Mana=100", "Mana: 100")
    while (t && *t) {
        while (*t == ',' || *t == ';' || isspace((unsigned char)*t)) t++;
        char name[32];
        size_t k = 0;
        while (*t && (isalnum((unsigned char)*t) || *t == '_' || *t == '.') && k + 1 < sizeof name) name[k++] = *t++;
        name[k] = 0;
        while (*t == ' ' || *t == '=' || *t == ':') t++;
        char* end = NULL;
        float v = strtof(t, &end);
        if (end && end != t) t = end;
        else v = 0.0f;
        while (*t && *t != ',' && *t != ';') t++;
        uint16_t id = attr_id(name);
        if (!id) continue;
        int i = attr_make(w, s, id, v);
        if (i >= 0) S.base[s][i] = S.cur[s][i] = v;
    }
}
static int give(World* w, int s, uint32_t hash);
static int sys_get(World* w, Entity e, bool create) {
    int s = sys_find(w, e);
    if (s >= 0 || !A.live || !w || !e || !ecs_alive(w, e)) return s;
    CAbilitySystem* c = ecs_get(w, e, COMP_ABILITY_SYSTEM);
    if (!c) {
        if (!create || !ecs_add(w, e, COMP_ABILITY_SYSTEM)) return -1;
        c = ecs_get(w, e, COMP_ABILITY_SYSTEM);
        if (!c) return -1;
        memset(c, 0, sizeof *c); // (one made by an effect or Give Ability: empty)
    }
    s = sys_alloc();
    if (s < 0) return -1;
    CAbilitySystem cfg = *c;
    c->handle = (uint32_t)s + 1;
    S.ent[s] = e, S.flags[s] = cfg.flags, S.first[s] = -1, S.dirty[s] = 0;
    S.nattr[s] = S.ngrant[s] = 0;
    memset(S.tid[s], 0, sizeof S.tid[s]), memset(S.tcnt[s], 0, sizeof S.tcnt[s]);
    parse_attrs(w, s, cfg.attributes);
    if (ecs_get(w, e, COMP_CHARACTER)) attr_make(w, s, D.speed, 0.0f); // (its Speed: the Max Walk Speed)
    for (int k = 0; k < ABIL_START_ABILITIES; k++)
        if (cfg.ability[k]) give(w, s, cfg.ability[k]);
    for (int k = 0; k < ABIL_START_EFFECTS; k++)
        if (cfg.effect[k]) abil_apply_effect(w, e, cfg.effect[k], e);
    return s;
}

// --- Abilities ---
static int grant_of(int s, uint32_t hash) {
    for (int g = 0; g < S.ngrant[s]; g++)
        if (D.ab[S.gdef[s][g] - 1].hash == hash) return g;
    return -1;
}
static int give(World* w, int s, uint32_t hash) {
    (void)w;
    const AbilityDef* d = abil_ability_by_hash(hash);
    if (!d) return -1;
    int g = grant_of(s, hash);
    if (g >= 0) return g;
    if (S.ngrant[s] >= NG) return -1;
    g = S.ngrant[s]++;
    S.gdef[s][g] = (uint16_t)(d - D.ab) + 1;
    S.gstate[s][g] = G_IDLE, S.gcharges[s][g] = (uint8_t)d->charges;
    S.grecharge[s][g] = S.gcast[s][g] = 0.0f, S.gobj[s][g] = ENTITY_NONE;
    if (d->cost_attr) attr_make(w, s, d->cost_attr, 0.0f);
    return g;
}
static bool end_grant(World* w, int s, int g, bool cancelled) {
    if (S.gstate[s][g] == G_IDLE) return false;
    const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
    S.gstate[s][g] = G_IDLE, S.gcast[s][g] = 0.0f;
    for (int k = 0; k < d->nowned; k++) tag_ungrant(w, s, d->owned[k]);
    push_ev(S.ent[s], BPK_EVENT_ABIL_ENDED, d->name, cancelled ? 1.0f : 0.0f, 0, ENTITY_NONE);
    if (S.gobj[s][g]) push_ev(S.gobj[s][g], BPK_EVENT_ABIL_ENDED, d->name, cancelled ? 1.0f : 0.0f, 0, ENTITY_NONE);
    return true;
}
static Entity ability_object(World* w, int s, int g) { // the class's object (spawned the first time: its BeginPlay runs)
    if (S.gobj[s][g] && ecs_alive(w, S.gobj[s][g])) return S.gobj[s][g];
    BpAsset* c = bp_find_hash(D.ab[S.gdef[s][g] - 1].hash);
    if (!c || (!c->compiled && !bp_compile(c))) return ENTITY_NONE;
    static const float zero[3] = { 0, 0, 0 };
    S.gobj[s][g] = bp_spawn(w, c, zero, zero);
    return S.gobj[s][g];
}
static void act_now(World* w, int s, int g) { // the cast is over (or there was none): it acts
    const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
    S.gstate[s][g] = G_ACTIVE, S.gcast[s][g] = 0.0f;
    A.st.activations++;
    Entity owner = S.ent[s];
    if (d->self_effect) abil_apply_effect(w, owner, d->self_effect, owner);
    if (!sys_ok(s) || S.ent[s] != owner) return; // (the effect's events can't run yet: nothing moved; safety)
    push_ev(owner, BPK_EVENT_ABIL_ACTIVATED, d->name, 0, 0, ENTITY_NONE);
    Entity obj = d->has_graph && !A.hook ? ability_object(w, s, g) : ENTITY_NONE;
    if (obj) push_ev(obj, BPK_EVENT_ABIL_ACTIVATE, d->name, 0, 0, owner);
    else end_grant(w, s, g, false); // (no graph: done once it acted)
}
static int try_grant(World* w, int s, int g) {
    const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
    Entity e = S.ent[s];
    if (S.gstate[s][g] != G_IDLE) return ABIL_FAIL_ACTIVE;
    if (health_is_dead(w, e)) return ABIL_FAIL_DEAD;
    if (has_any(w, e, d->block, d->nblock)) return ABIL_FAIL_BLOCKED;
    if (!S.gcharges[s][g]) return ABIL_FAIL_COOLDOWN;
    int ci = d->cost_attr && d->cost > 0.0f ? attr_idx(s, d->cost_attr) : -1;
    if (d->cost_attr && d->cost > 0.0f && (ci < 0 || S.cur[s][ci] < d->cost)) return ABIL_FAIL_COST;
    // commit: the cost, a charge, the running ones it cancels, its tags
    if (ci >= 0) S.base[s][ci] = clamp_attr(s, ci, S.base[s][ci] - d->cost), recompute(w, s);
    if (d->cooldown > 0.0f) {
        S.gcharges[s][g]--;
        if (S.grecharge[s][g] <= 0.0f) S.grecharge[s][g] = d->cooldown;
    }
    for (int o = 0; o < S.ngrant[s] && d->ncancel; o++) {
        if (o == g || S.gstate[s][o] == G_IDLE) continue;
        const AbilityDef* od = &D.ab[S.gdef[s][o] - 1];
        bool hit = false;
        for (int a = 0; a < od->ntags && !hit; a++)
            for (int b = 0; b < d->ncancel && !hit; b++) hit = gt_matches(abil_tag_name(od->tags[a]), abil_tag_name(d->cancel[b]), false);
        if (hit) end_grant(w, s, o, true);
    }
    for (int k = 0; k < d->nowned; k++) tag_grant(w, s, d->owned[k]);
    if (d->cast_time > 0.0f) {
        S.gstate[s][g] = G_CASTING, S.gcast[s][g] = d->cast_time; // (it acts when the cast is over: abil_tick)
        return ABIL_OK;
    }
    act_now(w, s, g);
    return ABIL_OK;
}

int abil_give(World* w, Entity e, uint32_t ability) {
    int s = sys_get(w, e, true);
    int g = s >= 0 ? give(w, s, ability) : -1;
    flush(w);
    return g;
}
bool abil_remove(World* w, Entity e, uint32_t ability) {
    int s = sys_find(w, e), g = s >= 0 ? grant_of(s, ability) : -1;
    if (g < 0) return false;
    end_grant(w, s, g, true);
    Entity obj = S.gobj[s][g];
    for (int k = g; k + 1 < S.ngrant[s]; k++) { // (the later ones move down a slot)
        S.gdef[s][k] = S.gdef[s][k + 1], S.gstate[s][k] = S.gstate[s][k + 1], S.gcharges[s][k] = S.gcharges[s][k + 1];
        S.grecharge[s][k] = S.grecharge[s][k + 1], S.gcast[s][k] = S.gcast[s][k + 1], S.gobj[s][k] = S.gobj[s][k + 1];
    }
    S.ngrant[s]--;
    flush(w);
    if (obj && ecs_alive(w, obj)) bp_destroy_actor(w, obj, true);
    return true;
}
int abil_try_activate(World* w, Entity e, uint32_t ability) {
    double t0 = re_time_seconds();
    int s = sys_find(w, e), g = s >= 0 ? grant_of(s, ability) : -1;
    int r = g < 0 ? ABIL_FAIL_NOT_GRANTED : try_grant(w, s, g);
    if (r != ABIL_OK) A.st.failures++;
    A.st.op_us = (re_time_seconds() - t0) * 1e6;
    flush(w);
    return r;
}
int abil_try_activate_tag(World* w, Entity e, const char* tag, uint32_t* which) {
    int s = sys_find(w, e), r = ABIL_FAIL_NOT_GRANTED;
    if (which) *which = 0;
    for (int g = 0; s >= 0 && g < S.ngrant[s]; g++) {
        const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
        bool hit = false;
        for (int k = 0; k < d->ntags && !hit; k++) hit = gt_matches(abil_tag_name(d->tags[k]), tag, false);
        if (!hit) continue;
        r = try_grant(w, s, g);
        if (which) *which = d->hash;
        if (r == ABIL_OK) break;
    }
    if (r != ABIL_OK) A.st.failures++;
    flush(w);
    return r;
}
static bool ability_matches(const AbilityDef* d, const char* q) {
    if (!q || !q[0]) return true;
    if (!_stricmp(d->name, q) || !_stricmp(d->display, q)) return true;
    for (int k = 0; k < d->ntags; k++)
        if (gt_matches(abil_tag_name(d->tags[k]), q, false)) return true;
    return false;
}
int abil_cancel(World* w, Entity e, const char* q) {
    int s = sys_find(w, e), n = 0;
    for (int g = 0; s >= 0 && g < S.ngrant[s]; g++)
        if (S.gstate[s][g] != G_IDLE && ability_matches(&D.ab[S.gdef[s][g] - 1], q)) n += end_grant(w, s, g, true);
    flush(w);
    return n;
}
static bool find_object(World* w, Entity obj, int* ps, int* pg) {
    const CBlueprint* b = A.live && w && obj && ecs_alive(w, obj) ? ecs_get(w, obj, COMP_BLUEPRINT) : NULL;
    if (!b || !abil_ability_by_hash(b->asset_hash)) return false; // (not a Gameplay Ability's object: no walk)
    for (uint32_t s = 0; obj && w && s < S.n; s++)
        for (int g = 0; S.ent[s] && g < S.ngrant[s]; g++)
            if (S.gobj[s][g] == obj) {
                *ps = (int)s, *pg = g;
                return true;
            }
    return false;
}
bool abil_end(World* w, Entity obj) {
    int s, g;
    if (!find_object(w, obj, &s, &g)) return false;
    bool r = end_grant(w, s, g, false);
    flush(w);
    return r;
}
Entity abil_owner_of(World* w, Entity obj) {
    int s, g;
    return find_object(w, obj, &s, &g) ? S.ent[s] : ENTITY_NONE;
}
bool abil_is_active(World* w, Entity e, uint32_t ability) {
    int s = sys_find(w, e), g = s >= 0 ? grant_of(s, ability) : -1;
    return g >= 0 && S.gstate[s][g] != G_IDLE;
}
float abil_cooldown_remaining(World* w, Entity e, uint32_t ability, float* duration, int* charges, int* max_charges) {
    int s = sys_find(w, e), g = s >= 0 ? grant_of(s, ability) : -1;
    const AbilityDef* d = g >= 0 ? &D.ab[S.gdef[s][g] - 1] : NULL;
    if (duration) *duration = d ? d->cooldown : 0.0f;
    if (charges) *charges = d ? S.gcharges[s][g] : 0;
    if (max_charges) *max_charges = d ? d->charges : 0;
    return d && S.gcharges[s][g] < d->charges ? fmaxf(S.grecharge[s][g], 0.0f) : 0.0f;
}
bool abil_on_cooldown(World* w, Entity e, uint32_t ability) {
    int s = sys_find(w, e), g = s >= 0 ? grant_of(s, ability) : -1;
    return g >= 0 && !S.gcharges[s][g];
}

// --- Effects ---
static bool effect_matches(const EffectDef* d, const char* q) {
    if (!q || !q[0]) return false;
    if (!_stricmp(d->name, q) || !_stricmp(d->display, q)) return true;
    for (int k = 0; k < d->ngranted; k++)
        if (gt_matches(abil_tag_name(d->granted[k]), q, false)) return true;
    return false;
}
int abil_apply_effect(World* w, Entity target, uint32_t effect, Entity source) {
    double t0 = re_time_seconds();
    const EffectDef* d = abil_effect_by_hash(effect);
    int s = d ? sys_get(w, target, true) : -1;
    if (s < 0) return 0;
    for (int k = 0; k < d->nrequired; k++)
        if (!gt_has(w, target, abil_tag_name(d->required[k]), false)) {
            flush(w);
            return 0;
        }
    uint16_t di = (uint16_t)(d - D.fx);
    for (int m = 0; m < d->nmods; m++) attr_make(w, s, d->mods[m].attr, attr_default(d->mods[m].attr));
    int stacks = 1;
    A.st.applied++;
    if (d->policy == GE_INSTANT) {
        execute(w, s, d, 1, target, source);
        s = sys_find(w, target); // (the Health's events may have run game code)
        if (s >= 0) recompute(w, s), push_ev(target, BPK_EVENT_GE_APPLIED, d->name, 1.0f, 0, source);
        A.st.op_us = (re_time_seconds() - t0) * 1e6;
        flush(w);
        return 1;
    }
    int32_t f = -1;
    bool fresh = false;
    if (d->max_stacks >= 1)
        for (int32_t x = S.first[s]; x >= 0; x = F.next[x])
            if (F.def[x] == di) {
                f = x;
                break;
            }
    if (f >= 0) { // stacked onto the one there
        if (F.stacks[f] < d->max_stacks) F.stacks[f]++;
        if (d->refresh && d->policy == GE_DURATION) F.left[f] = d->duration;
        F.src[f] = source;
        stacks = F.stacks[f];
    } else {
        f = fx_alloc(s, di);
        if (f < 0) {
            flush(w);
            return 0;
        }
        F.left[f] = d->policy == GE_DURATION ? d->duration : INFINITY;
        F.period_left[f] = d->period;
        F.src[f] = source;
        fresh = true;
        for (int k = 0; k < d->ngranted; k++) tag_grant(w, s, d->granted[k]);
    }
    S.dirty[s] = 1;
    push_ev(target, BPK_EVENT_GE_APPLIED, d->name, (float)stacks, 0, source);
    // a periodic one runs at once when it starts (not on a new stack); one with Damage / Heal and no period: each time
    if ((d->period > 0.0f && fresh) || (d->period <= 0.0f && (d->damage > 0.0f || d->heal > 0.0f))) execute(w, s, d, 1, target, source);
    s = sys_find(w, target);
    if (s >= 0) recompute(w, s);
    A.st.op_us = (re_time_seconds() - t0) * 1e6;
    flush(w);
    return stacks;
}
int abil_remove_effect(World* w, Entity e, const char* q, int stacks) {
    int s = sys_find(w, e), n = 0;
    for (int32_t f = s >= 0 ? S.first[s] : -1; f >= 0;) {
        int32_t next = F.next[f];
        const EffectDef* d = &D.fx[F.def[f]];
        if (effect_matches(d, q)) {
            if (stacks > 0 && F.stacks[f] > stacks) F.stacks[f] = (uint16_t)(F.stacks[f] - stacks), n += stacks, S.dirty[s] = 1;
            else {
                n += F.stacks[f];
                if (next == (int32_t)F.n - 1) next = f; // (the last moves into f's place)
                fx_free(w, f, true);
            }
        }
        f = next;
        if (f >= (int32_t)F.n) break;
    }
    if (s >= 0 && S.dirty[s]) recompute(w, s);
    flush(w);
    return n;
}
int abil_has_effect(World* w, Entity e, const char* q, float* remaining) {
    int s = sys_find(w, e), n = 0;
    float rem = 0.0f;
    for (int32_t f = s >= 0 ? S.first[s] : -1; f >= 0; f = F.next[f])
        if (effect_matches(&D.fx[F.def[f]], q)) n += F.stacks[f], rem = isinf(F.left[f]) || rem < 0.0f ? -1.0f : fmaxf(rem, F.left[f]);
    if (remaining) *remaining = n ? rem : 0.0f;
    return n;
}
bool abil_get_attr(World* w, Entity e, const char* name, float* current, float* base) {
    builtins();
    int s = sys_find(w, e), i = s >= 0 ? attr_idx(s, attr_id(name)) : -1;
    if (current) *current = i >= 0 ? S.cur[s][i] : 0.0f;
    if (base) *base = i >= 0 ? S.base[s][i] : 0.0f;
    return i >= 0;
}
bool abil_set_attr(World* w, Entity e, const char* name, float base) {
    builtins();
    int s = sys_get(w, e, true);
    uint16_t id = attr_id(name);
    int i = s >= 0 ? attr_make(w, s, id, base) : -1;
    if (i < 0) return false;
    S.base[s][i] = clamp_attr(s, i, base);
    recompute(w, s);
    flush(w);
    return true;
}
void abil_clear(World* w, Entity e) {
    int s = sys_find(w, e);
    if (s < 0) return;
    while (S.first[s] >= 0) fx_free(w, S.first[s], false);
    for (int g = 0; g < S.ngrant[s]; g++) end_grant(w, s, g, true);
    S.ngrant[s] = S.nattr[s] = 0;
    A.nq = 0; // (no events of a clear)
}

// --- The UI ---
int abil_slot_count(World* w, Entity e) {
    int s = sys_find(w, e);
    return s >= 0 ? S.ngrant[s] : 0;
}
bool abil_slot_info(World* w, Entity e, int slot, AbilSlotInfo* o) {
    int s = sys_find(w, e);
    memset(o, 0, sizeof *o);
    if (s < 0 || slot < 0 || slot >= S.ngrant[s]) return false;
    const AbilityDef* d = &D.ab[S.gdef[s][slot] - 1];
    o->def = d;
    o->charges = S.gcharges[s][slot], o->max_charges = d->charges;
    o->cooldown = d->cooldown;
    o->cooldown_left = o->charges < d->charges ? fmaxf(S.grecharge[s][slot], 0.0f) : 0.0f;
    o->casting = S.gstate[s][slot] == G_CASTING, o->active = S.gstate[s][slot] == G_ACTIVE;
    o->cast_time = d->cast_time, o->cast_left = S.gcast[s][slot];
    int ci = d->cost_attr && d->cost > 0.0f ? attr_idx(s, d->cost_attr) : -1;
    o->affordable = !(d->cost_attr && d->cost > 0.0f) || (ci >= 0 && S.cur[s][ci] >= d->cost);
    o->key = (S.flags[s] & ABIL_KEYS) && slot < 6 ? '1' + slot : 0;
    return true;
}
uint32_t abil_effects_of(World* w, Entity e, AbilEffectInfo* out, uint32_t cap) {
    int s = sys_find(w, e);
    uint32_t n = 0;
    for (int32_t f = s >= 0 ? S.first[s] : -1; f >= 0 && n < cap; f = F.next[f]) {
        const EffectDef* d = &D.fx[F.def[f]];
        out[n].def = d, out[n].stacks = F.stacks[f];
        out[n].remaining = isinf(F.left[f]) ? -1.0f : F.left[f], out[n].duration = d->duration;
        n++;
    }
    return n;
}

// --- Play ---
static float damage_scale(World* w, Entity target, Entity inst) { // the instigator's Damage, the target's Defense
    float k = 1.0f;
    int si = inst ? sys_find(w, inst) : -1, i;
    if (si >= 0 && (i = attr_idx(si, D.damage)) >= 0) k *= fmaxf(S.cur[si][i], 0.0f);
    int st = sys_find(w, target);
    if (st >= 0 && (i = attr_idx(st, D.defense)) >= 0) {
        float def = S.cur[st][i];
        k *= def >= 0.0f ? 100.0f / (100.0f + def) : 2.0f - 100.0f / (100.0f - def);
    }
    return k;
}

void abil_begin_play(World* w) {
    abil_defs_refresh();
    memset(&A.st, 0, sizeof A.st);
    S.n = 0, S.nfree = 0, F.n = 0, A.nq = A.ndue = A.nlater = 0, A.keys_prev = 0;
    A.live = true, A.w = w, A.in_tick = A.flushing = false;
    health_set_damage_scale(damage_scale);
    // the components' systems (collected first: making one may move archetypes)
    static Entity list[NS];
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_ABILITY_SYSTEM)) || !a->count) continue;
        CAbilitySystem* c = ecs_arch_column(a, COMP_ABILITY_SYSTEM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < NS; i++) c[i].handle = 0, list[n++] = ents[i];
    }
    for (uint32_t k = 0; k < n; k++) sys_get(w, list[k], false);
    A.nq = 0; // (the starting effects' events: before any BeginPlay, nobody listens)
}

void abil_after_begin_play(World* w, Entity player) {
    int s = sys_find(w, player);
    if (s < 0 || !(S.flags[s] & ABIL_SHOW_BAR)) return;
    BpAsset* bar = bp_find_hash(abil_bar_asset());
    Entity we = bar && (bar->compiled || bp_compile(bar)) ? ui_create_widget(w, bar) : ENTITY_NONE;
    if (we) ui_add_to_viewport(we, 0);
}

void abil_tick(World* w, float dt, Entity player) {
    if (!A.live || A.w != w) return;
    double t0 = re_time_seconds();
    A.in_tick = true;
    A.ndue = A.nlater = 0;
    // the systems whose actor went: freed (their effects and objects with them)
    for (uint32_t s = 0; s < S.n; s++)
        if (S.ent[s] && !ecs_alive(w, S.ent[s])) sys_free(w, (int)s);
    // the effects' time: the periods due (run below), the expired removed (the last into its place: i again)
    for (uint32_t i = 0; i < F.n;) {
        const EffectDef* d = &D.fx[F.def[i]];
        if (d->period > 0.0f) {
            F.period_left[i] -= dt;
            for (int k = 0; F.period_left[i] <= 0.0f && k < 8; k++) {
                F.period_left[i] += d->period;
                if (A.ndue < DUE_MAX) {
                    Due* u = &A.due[A.ndue++];
                    u->sys = F.sys[i], u->def = F.def[i], u->stacks = F.stacks[i], u->target = S.ent[F.sys[i]], u->src = F.src[i];
                }
            }
        }
        if (d->policy == GE_DURATION && (F.left[i] -= dt) <= 0.0f) {
            fx_free(w, (int32_t)i, true);
            continue;
        }
        i++;
    }
    // the abilities: charges back, casts going on (interrupted by a blocking tag), the player's keys
    uint32_t keys = 0;
    int ps = sys_find(w, player);
    if (ps >= 0 && (S.flags[ps] & ABIL_KEYS))
        for (int k = 0; k < 6 && k < S.ngrant[ps]; k++)
            if (bp_key_held('1' + k)) keys |= 1u << k;
    for (uint32_t s = 0; s < S.n; s++) {
        if (!S.ent[s]) continue;
        for (int g = 0; g < S.ngrant[s]; g++) {
            const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
            if (S.gcharges[s][g] < d->charges && d->cooldown > 0.0f && (S.grecharge[s][g] -= dt) <= 0.0f) {
                S.gcharges[s][g]++;
                S.grecharge[s][g] = S.gcharges[s][g] < d->charges ? S.grecharge[s][g] + d->cooldown : 0.0f;
            }
            if (S.gstate[s][g] != G_CASTING || A.nlater >= LATER_MAX) continue;
            if (has_any(w, S.ent[s], d->block, d->nblock)) A.later[A.nlater++] = (Later){ (uint16_t)s, (uint16_t)g, 1 };
            else if ((S.gcast[s][g] -= dt) <= 0.0f) A.later[A.nlater++] = (Later){ (uint16_t)s, (uint16_t)g, 0 };
        }
    }
    uint32_t pressed = keys & ~A.keys_prev;
    A.keys_prev = keys;
    for (int k = 0; k < 6 && ps >= 0; k++)
        if ((pressed >> k & 1u) && A.nlater < LATER_MAX) A.later[A.nlater++] = (Later){ (uint16_t)ps, (uint16_t)k, 2 };
    // the work that runs game code (the Health's events, an effect applied): after the walks
    for (uint32_t k = 0; k < A.ndue; k++) {
        const Due* u = &A.due[k];
        if (!sys_ok(u->sys) || S.ent[u->sys] != u->target || !ecs_alive(w, u->target)) continue;
        execute(w, u->sys, &D.fx[u->def], u->stacks, u->target, u->src);
    }
    for (uint32_t k = 0; k < A.nlater; k++) {
        Later l = A.later[k];
        if (l.what == 0xFFFF || !sys_ok(l.sys) || l.slot >= S.ngrant[l.sys]) continue;
        if (l.what == 0 && S.gstate[l.sys][l.slot] == G_CASTING) act_now(w, l.sys, l.slot);
        else if (l.what == 1 && S.gstate[l.sys][l.slot] == G_CASTING) end_grant(w, l.sys, l.slot, true);
        else if (l.what == 2 && try_grant(w, l.sys, l.slot) != ABIL_OK) A.st.failures++;
    }
    uint32_t nsys = 0, nab = 0, nact = 0;
    for (uint32_t s = 0; s < S.n; s++) {
        if (!S.ent[s]) continue;
        if (S.dirty[s]) recompute(w, (int)s);
        nsys++, nab += S.ngrant[s];
        for (int g = 0; g < S.ngrant[s]; g++) nact += S.gstate[s][g] != G_IDLE;
    }
    A.st.systems = nsys, A.st.abilities = nab, A.st.active = nact, A.st.effects = F.n;
    A.in_tick = false;
    A.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
    flush(w);
    if (A.profile_until > 0.0 && t0 > A.profile_until) A.profile_until = 0.0;
}

void abil_end_play(void) {
    A.live = false, A.w = NULL, A.nq = 0;
    S.n = 0, S.nfree = 0, F.n = 0;
    health_set_damage_scale(NULL);
}

const AbilStats* abil_stats(void) { return &A.st; }

// --- Saves: per actor its key, its attributes (by name), active effects and abilities with their cooldowns ---
typedef struct { uint8_t* p; uint32_t n, cap; bool bad; } Wb;
static void wput(Wb* b, const void* d, uint32_t n) {
    if (b->bad || b->n + n > b->cap) {
        b->bad = true;
        return;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void wu32(Wb* b, uint32_t v) { wput(b, &v, 4); }
static void wf(Wb* b, float v) { wput(b, &v, 4); }
#define ABIL_SAVE_VERSION 1u
uint32_t abil_save_write(World* w, const Entity* ents, const uint32_t* keys, uint32_t n, uint8_t* out, uint32_t cap) {
    Wb b = { out, 0, cap, false };
    wu32(&b, ABIL_SAVE_VERSION);
    uint32_t count_at = b.n, count = 0;
    wu32(&b, 0);
    for (uint32_t k = 0; k < n; k++) {
        int s = sys_find(w, ents[k]);
        if (s < 0) continue;
        wu32(&b, keys[k]);
        uint32_t size_at = b.n;
        wu32(&b, 0);
        uint32_t start = b.n;
        wu32(&b, S.nattr[s]);
        for (int i = 0; i < S.nattr[s]; i++) {
            char nm[32];
            memset(nm, 0, sizeof nm);
            snprintf(nm, sizeof nm, "%s", abil_attr_name(S.aid[s][i]));
            wput(&b, nm, 32), wf(&b, S.base[s][i]);
        }
        uint32_t ne = 0;
        for (int32_t f = S.first[s]; f >= 0; f = F.next[f]) ne++;
        wu32(&b, ne);
        for (int32_t f = S.first[s]; f >= 0; f = F.next[f])
            wu32(&b, D.fx[F.def[f]].hash), wf(&b, isinf(F.left[f]) ? -1.0f : F.left[f]), wf(&b, F.period_left[f]), wu32(&b, F.stacks[f]);
        wu32(&b, S.ngrant[s]);
        for (int g = 0; g < S.ngrant[s]; g++)
            wu32(&b, D.ab[S.gdef[s][g] - 1].hash), wu32(&b, S.gcharges[s][g]), wf(&b, S.grecharge[s][g]);
        if (!b.bad) {
            uint32_t sz = b.n - start;
            memcpy(b.p + size_at, &sz, 4);
        }
        count++;
    }
    if (b.bad) return 0;
    memcpy(b.p + count_at, &count, 4);
    return b.n;
}

typedef struct { const uint8_t* p; uint32_t n, at; bool bad; } Rb;
static bool rget(Rb* r, void* o, uint32_t n) {
    if (r->bad || n > r->n - r->at) {
        r->bad = true;
        if (o) memset(o, 0, n);
        return false;
    }
    if (o) memcpy(o, r->p + r->at, n);
    r->at += n;
    return true;
}
static uint32_t ru32(Rb* r) {
    uint32_t v = 0;
    rget(r, &v, 4);
    return v;
}
static float rf(Rb* r) {
    float v = 0;
    rget(r, &v, 4);
    return isfinite(v) ? v : 0.0f;
}
static void apply_one(World* w, Entity e, Rb* r) {
    int s = sys_get(w, e, true);
    if (s < 0) return;
    while (S.first[s] >= 0) fx_free(w, S.first[s], false);
    for (int g = 0; g < S.ngrant[s]; g++) end_grant(w, s, g, true);
    uint32_t na = ru32(r);
    for (uint32_t i = 0; i < na && !r->bad; i++) {
        char nm[33];
        rget(r, nm, 32), nm[32] = 0;
        float v = rf(r);
        uint16_t id = attr_id(nm);
        int k = id && !r->bad ? attr_make(w, s, id, v) : -1;
        if (k >= 0) S.base[s][k] = v;
    }
    uint32_t ne = ru32(r);
    for (uint32_t i = 0; i < ne && !r->bad; i++) {
        uint32_t h = ru32(r);
        float left = rf(r), pl = rf(r);
        uint32_t st = ru32(r);
        const EffectDef* d = abil_effect_by_hash(h);
        if (!d || r->bad || d->policy == GE_INSTANT) continue; // (an effect no longer in the project: dropped)
        int32_t f = fx_alloc(s, (uint16_t)(d - D.fx));
        if (f < 0) break;
        F.left[f] = d->policy == GE_DURATION ? fmaxf(left, 0.001f) : INFINITY;
        F.period_left[f] = pl, F.stacks[f] = (uint16_t)(st < 1 ? 1 : st > 999 ? 999 : st), F.src[f] = e;
        for (int k = 0; k < d->ngranted; k++) tag_grant(w, s, d->granted[k]);
    }
    uint32_t ng = ru32(r);
    for (uint32_t i = 0; i < ng && !r->bad; i++) {
        uint32_t h = ru32(r), ch = ru32(r);
        float rc = rf(r);
        int g = r->bad ? -1 : give(w, s, h);
        if (g < 0) continue;
        const AbilityDef* d = &D.ab[S.gdef[s][g] - 1];
        S.gcharges[s][g] = (uint8_t)(ch > (uint32_t)d->charges ? (uint32_t)d->charges : ch), S.grecharge[s][g] = fmaxf(rc, 0.0f);
    }
    recompute(w, s);
}
void abil_save_read(World* w, const uint8_t* d, uint32_t n, Entity (*find)(World* w, uint32_t key), Entity player, bool player_only) {
    if (!A.live || !w || !d) return;
    Rb r = { d, n, 0, false };
    uint32_t ver = ru32(&r), count = ru32(&r);
    if (r.bad || ver > ABIL_SAVE_VERSION) return; // (a newer engine's records: left as the level has them)
    for (uint32_t i = 0; i < count && !r.bad; i++) {
        uint32_t key = ru32(&r), size = ru32(&r);
        if (r.bad || size > r.n - r.at) break;
        Rb one = { r.p + r.at, size, 0, false };
        r.at += size;
        Entity e = key == ABIL_PLAYER_KEY ? (player_only ? player : ENTITY_NONE) : (!player_only && find ? find(w, key) : ENTITY_NONE);
        if (e && ecs_alive(w, e)) apply_one(w, e, &one);
    }
    flush(w);
}

// --- The Debug Viewer: the tick (effects' time, periods, casts, cooldowns), each system ---
static void collect(DvList* l, const DvContext* c) {
    A.profile_until = re_time_seconds() + 1.0;
    l->gpu = false, l->estimated = true;
    l->ms = A.st.tick_ms;
    snprintf(l->note, sizeof l->note, "%u Ability Systems, %u active effects, %u abilities (%u running); since play: %u applied, %u periodic runs, %u activations, %u failed",
             A.st.systems, A.st.effects, A.st.abilities, A.st.active, A.st.applied, A.st.periodic, A.st.activations, A.st.failures);
    char info[64];
    snprintf(info, sizeof info, "%u effects, %u systems, last operation %.2f us", A.st.effects, A.st.systems, A.st.op_us);
    dv_add(l, "Update", info, (float)A.st.tick_ms, ENTITY_NONE);
    World* w = c ? c->world : NULL;
    if (!w || !A.live || !A.st.systems) return;
    float each = (float)(l->ms / (double)(A.st.effects + A.st.systems));
    for (uint32_t s = 0; s < S.n; s++) {
        if (!S.ent[s] || !ecs_alive(w, S.ent[s])) continue;
        const CName* nm = ecs_get(w, S.ent[s], COMP_NAME);
        uint32_t ne = 0;
        for (int32_t f = S.first[s]; f >= 0; f = F.next[f]) ne++;
        int pos = snprintf(info, sizeof info, "%u effects, %u abilities", ne, S.ngrant[s]);
        for (int i = 0; i < S.nattr[s] && i < 2 && pos > 0 && pos < (int)sizeof info; i++)
            pos += snprintf(info + pos, sizeof info - (size_t)pos, "; %s %.0f", abil_attr_name(S.aid[s][i]), S.cur[s][i]);
        dv_add(l, nm ? nm->text : "Actor", info, each * (float)(ne + 1), S.ent[s]);
    }
}
void abil_register_debug(void) { dv_register("Abilities and Effects", collect); }

// --- WBP_AbilityBar: six Ability Slots at the bottom, the Active Effects at the top left ---
uint32_t abil_bar_asset(void) {
    const char* vp = "/Content/UI/WBP_AbilityBar.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_AbilityBar", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    const float slot = 84.0f, gap = 12.0f, n = 6.0f, wbar = n * slot + (n - 1.0f) * gap + 2.0f * 18.0f;
    int f = ui_tree_add(t, UIW_BORDER, 0);
    if (f < 0) return 0;
    UiWidget* u = &t->w[f];
    snprintf(u->name, UI_NAME, "Frame");
    u->anchor[0] = u->anchor[2] = 0.5f, u->anchor[1] = u->anchor[3] = 1.0f, u->align[0] = 0.5f, u->align[1] = 1.0f;
    u->pos[0] = 0.0f, u->pos[1] = -24.0f, u->size[0] = wbar, u->size[1] = slot + 36.0f;
    u->bg[0] = 0.02f, u->bg[1] = 0.02f, u->bg[2] = 0.025f, u->bg[3] = 0.55f, u->radius = 10.0f;
    for (int i = 0; i < 6; i++) {
        int k = ui_tree_add(t, UIW_ABILITY_SLOT, 0);
        if (k < 0) return 0;
        u = &t->w[k];
        snprintf(u->name, UI_NAME, "Ability%d", i + 1);
        u->anchor[0] = u->anchor[2] = 0.5f, u->anchor[1] = u->anchor[3] = 1.0f, u->align[0] = 0.0f, u->align[1] = 1.0f;
        u->pos[0] = -wbar * 0.5f + 18.0f + (float)i * (slot + gap), u->pos[1] = -42.0f, u->size[0] = u->size[1] = slot;
        u->value = (float)i;
    }
    int e = ui_tree_add(t, UIW_EFFECT_LIST, 0);
    if (e < 0) return 0;
    u = &t->w[e];
    snprintf(u->name, UI_NAME, "ActiveEffects");
    u->anchor[0] = u->anchor[2] = 0.0f, u->anchor[1] = u->anchor[3] = 0.0f, u->align[0] = u->align[1] = 0.0f;
    u->pos[0] = 32.0f, u->pos[1] = 32.0f, u->size[0] = 380.0f, u->size[1] = 360.0f;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id); // (the slots read the player's Ability System themselves)
    bp_compile(a);
    if (!bp_save(a)) return 0;
    return a->hash;
}

// --- The self-test: the rules on a world without the VM ---
static AbilityDef* test_ability(const char* name, const char* cost_attr, float cost, float cooldown, int charges, float cast) {
    builtins();
    if (D.nab >= ABIL_MAX_DEFS) return NULL;
    AbilityDef* d = &D.ab[D.nab++];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", name), snprintf(d->display, sizeof d->display, "%s", name);
    d->hash = lhash(name) ^ 0xA5A5u, d->name_hash = lhash(name);
    d->cost_attr = attr_id(cost_attr), d->cost = cost, d->cooldown = cooldown, d->charges = charges, d->cast_time = cast;
    return d;
}
static EffectDef* test_effect(const char* name, uint8_t policy, float duration, float period, const char* mods) {
    builtins();
    if (D.nfx >= ABIL_MAX_DEFS) return NULL;
    EffectDef* d = &D.fx[D.nfx++];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", name), snprintf(d->display, sizeof d->display, "%s", name);
    d->hash = lhash(name) ^ 0x5A5Au, d->name_hash = lhash(name);
    d->policy = policy, d->duration = duration, d->period = period, d->max_stacks = 1, d->refresh = true;
    d->nmods = parse_mods(mods, d->mods);
    return d;
}
static struct { uint32_t n[8]; char last[8][48]; float a[8]; } g_ev;
static int ev_slot(uint16_t kind) {
    switch (kind) {
    case BPK_EVENT_ABIL_ACTIVATED: return 0;
    case BPK_EVENT_ABIL_ENDED: return 1;
    case BPK_EVENT_GE_APPLIED: return 2;
    case BPK_EVENT_GE_REMOVED: return 3;
    case BPK_EVENT_ATTR_CHANGED: return 4;
    default: return 5;
    }
}
static void rec(Entity owner, uint16_t kind, const char* s, float a, float b, Entity e) {
    (void)owner, (void)b, (void)e;
    int k = ev_slot(kind);
    g_ev.n[k]++, g_ev.a[k] = a;
    snprintf(g_ev.last[k], sizeof g_ev.last[k], "%s", s);
}
static Entity test_actor(World* w, bool character, const char* attrs) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ABILITY_SYSTEM) | COMP_BIT(COMP_HEALTH) |
                                 (character ? COMP_BIT(COMP_CHARACTER) : 0));
    if (!e) return e;
    CAbilitySystem* c = ecs_get(w, e, COMP_ABILITY_SYSTEM);
    memset(c, 0, sizeof *c);
    snprintf(c->attributes, sizeof c->attributes, "%s", attrs);
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    ecs_component_defaults(COMP_HEALTH, h);
    if (character) ((CCharacter*)ecs_get(w, e, COMP_CHARACTER))->max_walk_speed = 600.0f;
    return e;
}
static void step(World* w, float seconds) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) abil_tick(w, 1.0f / 60.0f, ENTITY_NONE), health_update(w, 1.0f / 60.0f);
}

int abil_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: abilities: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.02f)
    World w;
    if (!ecs_init(&w)) return 1;
    abil_defs_refresh();
    uint32_t nab0 = D.nab, nfx0 = D.nfx;
    AbilityDef* fire = test_ability("T_Fireball", "Mana", 30, 2.0f, 1, 0);
    AbilityDef* dash = test_ability("T_Dash", "Stamina", 10, 1.0f, 2, 0);
    AbilityDef* cast = test_ability("T_Channel", "", 0, 0.0f, 1, 0.5f);
    EffectDef* poison = test_effect("T_Poison", GE_DURATION, 3.5f, 1.0f, "");
    EffectDef* haste = test_effect("T_Haste", GE_DURATION, 2.0f, 0.0f, "Speed x1.5");
    EffectDef* armor = test_effect("T_Armor", GE_INFINITE, 0.0f, 0.0f, "Defense +100");
    EffectDef* drain = test_effect("T_Drain", GE_INSTANT, 0.0f, 0.0f, "Mana -25");
    EffectDef* regen = test_effect("T_Regen", GE_INFINITE, 0.0f, 0.5f, "Mana +5");
    EffectDef* stun = test_effect("T_Stun", GE_DURATION, 1.0f, 0.0f, "");
    EffectDef* fury = test_effect("T_Fury", GE_DURATION, 4.0f, 0.0f, "Damage +0.5");
    if (!fire || !dash || !cast || !poison || !haste || !armor || !drain || !regen || !stun || !fury) return 1;
    poison->damage = 10.0f, snprintf(poison->damage_type, sizeof poison->damage_type, "Damage.Poison"), poison->max_stacks = 3, poison->debuff = true;
    poison->ngranted = parse_tags("State.Poisoned", poison->granted);
    stun->ngranted = parse_tags("State.Stunned", stun->granted), stun->debuff = true;
    fire->nblock = parse_tags("State.Stunned", fire->block), fire->ntags = parse_tags("Ability.Attack.Fire", fire->tags);
    fire->nowned = parse_tags("State.Casting", fire->owned);
    cast->nblock = parse_tags("State.Stunned", cast->block), cast->ntags = parse_tags("Ability.Channel", cast->tags);
    dash->ncancel = parse_tags("Ability.Channel", dash->cancel);
    fury->max_stacks = 2;
    abil_set_test_hook(rec);
    memset(&g_ev, 0, sizeof g_ev);
    A.live = true, A.w = &w, S.n = 0, S.nfree = 0, F.n = 0, A.nq = 0;
    health_set_damage_scale(damage_scale);
    Entity e = test_actor(&w, true, "Mana 100, MaxMana 100, Stamina 30");
    Entity foe = test_actor(&w, false, "");
    CHealth* fh = ecs_get(&w, foe, COMP_HEALTH);
    fh->max_health = fh->health = 1000.0f; // (it lives through the poison)
    CHECK(abil_give(&w, e, fire->hash) == 0 && abil_give(&w, e, dash->hash) == 1 && abil_give(&w, e, cast->hash) == 2 && abil_give(&w, e, fire->hash) == 0,
          "Give Ability: slots 0, 1, 2 (the same twice: its slot)");
    float mana = 0, base = 0, spd = 0;
    CHECK(abil_get_attr(&w, e, "Mana", &mana, &base) && NEAR(mana, 100) && abil_get_attr(&w, e, "Speed", &spd, NULL) && NEAR(spd, 600),
          "the starting attributes (Mana 100, the Character's Speed 600)");
    // the cost and the cooldown
    CHECK(abil_try_activate(&w, e, fire->hash) == ABIL_OK, "Fireball activates");
    abil_get_attr(&w, e, "Mana", &mana, NULL);
    CHECK(NEAR(mana, 70), "the cost paid: Mana 70 (%.1f)", mana);
    CHECK(g_ev.n[0] == 1 && g_ev.n[1] == 1 && !strcmp(g_ev.last[0], "T_Fireball"), "On Ability Activated and Ended (no graph: it ends at once)");
    CHECK(abil_on_cooldown(&w, e, fire->hash) && abil_try_activate(&w, e, fire->hash) == ABIL_FAIL_COOLDOWN, "on cooldown: refused");
    float dur = 0;
    int ch = 0, mx = 0;
    CHECK(NEAR(abil_cooldown_remaining(&w, e, fire->hash, &dur, &ch, &mx), 2.0f) && NEAR(dur, 2.0f) && ch == 0 && mx == 1, "Get Cooldown Remaining 2 s");
    step(&w, 1.0f);
    CHECK(fabsf(abil_cooldown_remaining(&w, e, fire->hash, NULL, NULL, NULL) - 1.0f) < 0.05f, "1 s later: 1 s left");
    step(&w, 1.05f);
    CHECK(!abil_on_cooldown(&w, e, fire->hash) && abil_try_activate(&w, e, fire->hash) == ABIL_OK, "the cooldown over: activates again");
    abil_get_attr(&w, e, "Mana", &mana, NULL);
    CHECK(NEAR(mana, 40) && abil_try_activate(&w, e, fire->hash) == ABIL_FAIL_COOLDOWN, "Mana 40");
    step(&w, 2.05f);
    CHECK(abil_try_activate(&w, e, fire->hash) == ABIL_OK, "a third time (Mana 10)");
    step(&w, 2.05f);
    CHECK(abil_try_activate(&w, e, fire->hash) == ABIL_FAIL_COST, "Mana 10 < 30: refused for its cost");
    // charges: two dashes, then the cooldown brings one back at a time
    CHECK(abil_try_activate(&w, e, dash->hash) == ABIL_OK && abil_try_activate(&w, e, dash->hash) == ABIL_OK &&
              abil_try_activate(&w, e, dash->hash) == ABIL_FAIL_COOLDOWN, "two charges: two dashes, the third refused");
    step(&w, 1.05f);
    abil_cooldown_remaining(&w, e, dash->hash, NULL, &ch, NULL);
    CHECK(ch == 1 && abil_try_activate(&w, e, dash->hash) == ABIL_OK, "one charge back after 1 s: a third dash (Stamina 10 - 10 = 0)");
    step(&w, 2.1f);
    abil_cooldown_remaining(&w, e, dash->hash, NULL, &ch, NULL);
    CHECK(ch == 2 && abil_try_activate(&w, e, dash->hash) == ABIL_FAIL_COST, "both charges back, but Stamina 0: refused for its cost");
    // blocked by a tag: a stun (Granted Tags State.Stunned) blocks the Fireball; a cast is interrupted by it
    abil_set_attr(&w, e, "Mana", 100);
    CHECK(abil_apply_effect(&w, e, stun->hash, foe) == 1 && gt_has(&w, e, "State.Stunned", true), "the stun grants State.Stunned");
    CHECK(abil_try_activate(&w, e, fire->hash) == ABIL_FAIL_BLOCKED, "blocked by State.Stunned");
    step(&w, 1.05f);
    CHECK(!gt_has(&w, e, "State.Stunned", true) && abil_try_activate(&w, e, fire->hash) == ABIL_OK, "the stun over: the tag gone, it activates");
    CHECK(abil_try_activate(&w, e, cast->hash) == ABIL_OK && abil_is_active(&w, e, cast->hash), "a 0.5 s cast starts");
    uint32_t act0 = g_ev.n[0];
    abil_apply_effect(&w, e, stun->hash, foe);
    step(&w, 0.1f);
    CHECK(!abil_is_active(&w, e, cast->hash) && g_ev.n[0] == act0, "the cast interrupted by the stun (never acted)");
    step(&w, 1.05f);
    CHECK(abil_try_activate(&w, e, cast->hash) == ABIL_OK, "a cast again");
    step(&w, 0.6f);
    CHECK(g_ev.n[0] == act0 + 1 && !abil_is_active(&w, e, cast->hash), "after its cast time it acted (and ended)");
    // the periodic damage through the Health: 10 at once, then each second (3.5 s: 4 runs), stacking up to 3
    float h0 = health_get(&w, foe);
    CHECK(abil_apply_effect(&w, foe, poison->hash, e) == 1 && gt_has(&w, foe, "State.Poisoned", true), "poisoned (its tag)");
    CHECK(h0 == 1000.0f && NEAR(health_get(&w, foe), h0 - 10), "the first run at once: 10 (%.1f)", h0 - health_get(&w, foe));
    step(&w, 1.02f);
    CHECK(NEAR(health_get(&w, foe), h0 - 20), "a second later: 20");
    CHECK(abil_apply_effect(&w, foe, poison->hash, e) == 2 && abil_apply_effect(&w, foe, poison->hash, e) == 3 && abil_apply_effect(&w, foe, poison->hash, e) == 3,
          "stacks: 2, 3, then 3 (Max Stacks)");
    float rem = 0;
    CHECK(abil_has_effect(&w, foe, "T_Poison", &rem) == 3 && NEAR(rem, 3.5f), "Has Effect: 3 stacks, its duration renewed (%.2f)", rem);
    CHECK(abil_has_effect(&w, foe, "State.Poisoned", NULL) == 3, "found by its granted tag");
    float hb = health_get(&w, foe);
    step(&w, 1.0f);
    CHECK(NEAR(health_get(&w, foe), hb - 30), "3 stacks: 30 a second (%.1f)", hb - health_get(&w, foe));
    step(&w, 3.0f);
    CHECK(abil_has_effect(&w, foe, "T_Poison", NULL) == 0 && !gt_has(&w, foe, "State.Poisoned", true), "expired: gone with its tag");
    CHECK(g_ev.n[3] >= 1, "On Effect Removed");
    // modifiers: haste x1.5 on the Character's speed, back when it ends
    CHECK(abil_apply_effect(&w, e, haste->hash, e) == 1, "haste");
    CCharacter* chr = ecs_get(&w, e, COMP_CHARACTER);
    CHECK(chr && NEAR(chr->max_walk_speed, 900.0f), "Speed x1.5: the Character walks 900 (%.0f)", chr ? chr->max_walk_speed : -1.0f);
    step(&w, 2.05f);
    chr = ecs_get(&w, e, COMP_CHARACTER);
    CHECK(chr && NEAR(chr->max_walk_speed, 600.0f), "haste over: 600 again (%.0f)", chr ? chr->max_walk_speed : -1.0f);
    CHECK(g_ev.n[4] >= 2, "On Attribute Changed");
    // Defense halves the damage taken (x 100 / (100 + 100)); Damage x1.5 on the instigator (stacked twice: x2)
    abil_apply_effect(&w, foe, armor->hash, e);
    hb = health_get(&w, foe);
    health_apply_damage(&w, foe, 20, "Damage.Physical", e, e);
    CHECK(NEAR(health_get(&w, foe), hb - 10), "Defense 100: 20 -> 10 (%.1f)", hb - health_get(&w, foe));
    abil_apply_effect(&w, e, fury->hash, e), abil_apply_effect(&w, e, fury->hash, e);
    float dmg = 0;
    abil_get_attr(&w, e, "Damage", &dmg, NULL);
    hb = health_get(&w, foe);
    health_apply_damage(&w, foe, 20, "Damage.Physical", e, e);
    CHECK(NEAR(dmg, 2.0f) && NEAR(health_get(&w, foe), hb - 20), "Damage 1 + 0.5 x2 stacks = 2: 20 x 2 / 2 = 20 (%.2f, %.1f)", dmg, hb - health_get(&w, foe));
    CHECK(abil_remove_effect(&w, foe, "T_Armor", 0) == 1, "Remove Effect");
    // instant: the base changes once; periodic Infinite: a regeneration on the base, held under MaxMana
    abil_set_attr(&w, e, "Mana", 50);
    abil_apply_effect(&w, e, drain->hash, foe);
    abil_get_attr(&w, e, "Mana", &mana, &base);
    CHECK(NEAR(mana, 25) && NEAR(base, 25) && abil_has_effect(&w, e, "T_Drain", NULL) == 0, "Instant: Mana 50 - 25 on the base, not kept active");
    abil_apply_effect(&w, e, regen->hash, e);
    step(&w, 1.02f);
    abil_get_attr(&w, e, "Mana", &mana, NULL);
    CHECK(NEAR(mana, 40), "regeneration: +5 at once, then every 0.5 s: 25 + 15 = 40 (%.1f)", mana);
    abil_set_attr(&w, e, "Mana", 99);
    step(&w, 1.0f);
    abil_get_attr(&w, e, "Mana", &mana, NULL);
    CHECK(NEAR(mana, 100), "held under MaxMana (%.1f)", mana);
    // cancel by tag: the dash cancels the channel (Cancel Abilities With Tags)
    abil_remove_effect(&w, e, "T_Regen", 0);
    abil_set_attr(&w, e, "Stamina", 100);
    step(&w, 2.0f);
    CHECK(abil_try_activate(&w, e, cast->hash) == ABIL_OK && abil_try_activate(&w, e, dash->hash) == ABIL_OK && !abil_is_active(&w, e, cast->hash),
          "the dash cancels the channel");
    uint32_t which = 0;
    step(&w, 1.0f);
    CHECK(abil_try_activate_tag(&w, e, "Ability.Attack", &which) == ABIL_OK && which == fire->hash, "Try Activate by Tag (Ability.Attack finds the Fireball)");
    // the save's record: the attributes, the effects with their time, the abilities with their cooldowns back as they were
    abil_apply_effect(&w, e, haste->hash, e);
    abil_set_attr(&w, e, "Mana", 42);
    step(&w, 0.5f);
    float rem0 = 0, cd0 = abil_cooldown_remaining(&w, e, fire->hash, NULL, NULL, NULL);
    abil_has_effect(&w, e, "T_Haste", &rem0);
    static uint8_t buf[8192];
    uint32_t key = ABIL_PLAYER_KEY;
    uint32_t len = abil_save_write(&w, &e, &key, 1, buf, sizeof buf);
    abil_clear(&w, e);
    CHECK(len > 16 && abil_slot_count(&w, e) == 0 && !abil_has_effect(&w, e, "T_Haste", NULL), "a record written (%u bytes), the system cleared", len);
    abil_save_read(&w, buf, len, NULL, e, true);
    float rem1 = 0, cd1 = abil_cooldown_remaining(&w, e, fire->hash, NULL, NULL, NULL);
    abil_get_attr(&w, e, "Mana", &mana, NULL);
    chr = ecs_get(&w, e, COMP_CHARACTER);
    CHECK(NEAR(mana, 42) && abil_has_effect(&w, e, "T_Haste", &rem1) == 1 && NEAR(rem1, rem0) && abil_slot_count(&w, e) == 3 && NEAR(cd1, cd0) && cd1 > 0.5f &&
              chr && NEAR(chr->max_walk_speed, 900.0f),
          "read back: Mana 42, the haste with %.2f s (%.2f), the Fireball's cooldown %.2f (%.2f), Speed 900", rem1, rem0, cd1, cd0);
    ecs_shutdown(&w);
    // 1000 actors with 5 effects each (a periodic damage, a speed buff, an armor, a regeneration, a stun), timed
    if (!ecs_init(&w)) return fails + 1;
    S.n = 0, S.nfree = 0, F.n = 0, A.nq = 0;
    memset(&A.st, 0, sizeof A.st);
    enum { N = 1000, FRAMES = 600 };
    static Entity many[N];
    EffectDef* burn = test_effect("T_Burn", GE_INFINITE, 0.0f, 0.5f, "");
    if (!burn) return fails + 1;
    burn->damage = 0.5f;
    stun->duration = 1e6f;
    double a0 = re_time_seconds();
    for (int i = 0; i < N; i++) {
        many[i] = test_actor(&w, true, "Mana 50, MaxMana 100");
        CHealth* h = ecs_get(&w, many[i], COMP_HEALTH);
        h->max_health = h->health = 1e6f;
        abil_apply_effect(&w, many[i], burn->hash, many[i]);
        abil_apply_effect(&w, many[i], haste->hash, many[i]);
        abil_apply_effect(&w, many[i], armor->hash, many[i]);
        abil_apply_effect(&w, many[i], regen->hash, many[i]);
        abil_apply_effect(&w, many[i], stun->hash, many[i]);
    }
    double apply_ms = (re_time_seconds() - a0) * 1000.0;
    haste->duration = 1e6f;
    for (int i = 0; i < N; i++) abil_apply_effect(&w, many[i], haste->hash, many[i]); // (renewed: kept the whole run)
    double t0 = re_time_seconds(), worst = 0.0;
    for (int f = 0; f < FRAMES; f++) {
        double f0 = re_time_seconds();
        abil_tick(&w, 1.0f / 60.0f, ENTITY_NONE);
        double ms = (re_time_seconds() - f0) * 1000.0;
        if (ms > worst) worst = ms;
    }
    double ms = (re_time_seconds() - t0) * 1000.0 / FRAMES;
    const AbilStats* st = abil_stats();
    CCharacter* c0 = ecs_get(&w, many[0], COMP_CHARACTER);
    printf("[RealEngine] abilities: 1000 actors x 5 effects: %.4f ms a frame (worst %.4f; %u effects, %u periodic runs); applying the 5000: %.2f ms (%.2f us each)\n",
           ms, worst, st->effects, st->periodic, apply_ms, apply_ms * 1000.0 / (N * 5));
    CHECK(st->effects == N * 5, "5000 active effects (%u)", st->effects);
    CHECK(c0 && NEAR(c0->max_walk_speed, 900.0f), "the speed buff on each Character (%.0f)", c0 ? c0->max_walk_speed : -1.0f);
    CHECK(ms < 0.5, "1000 actors x 5 effects in %.4f ms a frame (the budget: 0.5 ms)", ms);
    ecs_shutdown(&w);
    // the test's definitions out of the tables
    D.nab = nab0, D.nfx = nfx0;
    abil_end_play();
    abil_set_test_hook(NULL);
#undef NEAR
#undef CHECK
    printf("[RealEngine] abilities: self-test %s\n", fails ? "FAILED" : "ok");
    return fails;
}
