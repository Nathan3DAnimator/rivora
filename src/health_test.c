// Health and damage played (--health-test, the test health; notas-dev/saude-dano.md): first health_selftest (the rules,
// 1000 actors timed), then in the editor's play, away from the default level (y 2000):
//   * BP_HealthTarget (a Blueprint with the Health in its class defaults: 100, a shield of 20, Damage.Fire x2): its
//     BeginPlay hits itself with Apply Damage 15 Damage.Fire (30: the shield 20, broken, the health 10) and Heals 5; its
//     Event AnyDamage, On Death, On Healed and On Shield Broken count into variables;
//   * BP_HealthBomber: after 0.3 s Apply Radial Damage 200 (Falloff 0) at itself, 600 cm: the target dies (its Killer: the
//     bomber), a crate with Health dies and is destroyed after 0.3 s, an invincible wall takes nothing; 0.3 s later it
//     Revives the target to 50 and reads Get Health Percent and Is Dead;
//   * the player's Character with Health 60 / 80 and Show Health Bar: WBP_HealthBar shows 0.75 and "60 / 80".
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "health.h"
#include "platform_files.h"
#include "ui_widget.h"

static bool g_fail;
static Entity g_target, g_bomber, g_crate, g_wall, g_player;

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: health-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = pin(g, a, 1, ap), pb = pin(g, b, 0, bpn);
    if (!bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb)) printf("[RealEngine] FAIL: health-test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn), g_fail = true;
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 300.0f, floorf(x / 6.0f) * 190.0f, text, NULL);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}
static void lit(BpGraph* g, uint32_t id, const char* name, float x) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) n->pins[p].lit = (BpValue){ { x, 0, 0, 0 } };
}
static void slit(BpGraph* g, uint32_t id, const char* name, const char* s) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}
static void var(BpAsset* a, const char* name, uint8_t type) {
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, BP_NAME, "%s", name);
    v->type = type;
}
// on event ev (its exec "" ): Var = Var + (the event's output, or 1 for an integer counter); returns the Set node
static uint32_t accumulate(BpAsset* a, BpGraph* g, uint32_t ev, float x, const char* varname, const char* out, bool integer) {
    uint32_t get = node(a, g, BPK_VAR_GET, x, varname), add = node(a, g, integer ? BPK_IADD : BPK_ADD, x + 1, NULL);
    uint32_t set = node(a, g, BPK_VAR_SET, x + 2, varname);
    link(g, get, varname, add, "A");
    if (integer) lit(g, add, "B", 1.0f);
    else link(g, ev, out, add, "B");
    link(g, add, "", set, varname);
    link(g, ev, "", set, "");
    return set;
}

static BpAsset* make_target(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_HealthTarget", "Actor");
    if (!a) return NULL;
    var(a, "DamageTotal", BPT_FLOAT), var(a, "HealedTotal", BPT_FLOAT), var(a, "Deaths", BPT_INT), var(a, "ShieldBreaks", BPT_INT);
    var(a, "LastType", BPT_NAME), var(a, "Killer", BPT_ACTOR);
    // the Health in the class's defaults (BpAsset.comp_data: the actors spawned get it)
    a->comp_mask |= COMP_BIT(COMP_HEALTH);
    CHealth* h = (CHealth*)a->comp_data[COMP_HEALTH];
    memset(h, 0, sizeof *h);
    ecs_component_defaults(COMP_HEALTH, h);
    h->max_shield = h->shield = 20.0f;
    snprintf(h->mult[0].tag, HEALTH_TAG_LEN, "Damage.Fire"), h->mult[0].scale = 2.0f;
    h->death = HEALTH_DEATH_EVENT;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL), self = node(a, g, BPK_SELF, 1, NULL);
    uint32_t ad = node(a, g, BPK_APPLY_DAMAGE, 2, NULL), heal = node(a, g, BPK_HEAL, 3, NULL);
    lit(g, ad, "Amount", 15.0f), slit(g, ad, "Damage Type", "Damage.Fire"), lit(g, heal, "Amount", 5.0f);
    link(g, bp, "", ad, ""), link(g, ad, "", heal, "");
    link(g, self, "Self", ad, "Instigator"), link(g, self, "Self", heal, "Healer");
    uint32_t evd = node(a, g, BPK_EVENT_ANY_DAMAGE, 6, NULL);
    uint32_t sd = accumulate(a, g, evd, 7, "DamageTotal", "Damage", false);
    uint32_t st = node(a, g, BPK_VAR_SET, 10, "LastType");
    link(g, sd, "", st, ""), link(g, evd, "Damage Type", st, "LastType");
    uint32_t evk = node(a, g, BPK_EVENT_DEATH, 12, NULL);
    uint32_t sk = accumulate(a, g, evk, 13, "Deaths", NULL, true);
    uint32_t skl = node(a, g, BPK_VAR_SET, 16, "Killer");
    link(g, sk, "", skl, ""), link(g, evk, "Killer", skl, "Killer");
    uint32_t evh = node(a, g, BPK_EVENT_HEALED, 18, NULL);
    accumulate(a, g, evh, 19, "HealedTotal", "Amount", false);
    uint32_t evs = node(a, g, BPK_EVENT_SHIELD_BROKEN, 24, NULL);
    accumulate(a, g, evs, 25, "ShieldBreaks", NULL, true);
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: health-test: BP_HealthTarget didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

static BpAsset* make_bomber(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_HealthBomber", "Actor");
    if (!a) return NULL;
    var(a, "Hits", BPT_INT), var(a, "Pct", BPT_FLOAT), var(a, "Dead", BPT_BOOL);
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL), self = node(a, g, BPK_SELF, 1, NULL);
    uint32_t d1 = node(a, g, BPK_DELAY, 2, NULL), loc = node(a, g, BPK_GET_LOCATION, 3, NULL);
    uint32_t rad = node(a, g, BPK_APPLY_RADIAL_DAMAGE, 4, NULL), sh = node(a, g, BPK_VAR_SET, 5, "Hits");
    lit(g, d1, "Duration", 0.3f);
    lit(g, rad, "Base Damage", 200.0f), lit(g, rad, "Damage Radius", 600.0f), lit(g, rad, "Falloff", 0.0f);
    slit(g, rad, "Damage Type", "Damage.Explosion");
    link(g, bp, "", d1, ""), link(g, d1, "Completed", rad, ""), link(g, rad, "", sh, ""), link(g, rad, "Hit Count", sh, "Hits");
    link(g, self, "Self", loc, "Target"), link(g, loc, "", rad, "Origin");
    link(g, self, "Self", rad, "Instigator"), link(g, self, "Self", rad, "Damage Causer");
    uint32_t d2 = node(a, g, BPK_DELAY, 6, NULL), find = node(a, g, BPK_GET_ACTOR_OF_CLASS, 7, NULL);
    uint32_t rev = node(a, g, BPK_REVIVE, 8, NULL), pct = node(a, g, BPK_GET_HEALTH_PERCENT, 9, NULL), dead = node(a, g, BPK_IS_DEAD, 10, NULL);
    uint32_t sp = node(a, g, BPK_VAR_SET, 11, "Pct"), sdd = node(a, g, BPK_VAR_SET, 12, "Dead");
    lit(g, d2, "Duration", 0.3f), lit(g, rev, "Health", 50.0f);
    slit(g, find, "Actor Class", "BP_HealthTarget");
    link(g, sh, "", d2, ""), link(g, d2, "Completed", rev, ""), link(g, rev, "", sp, ""), link(g, sp, "", sdd, "");
    link(g, find, "Return Value", rev, "Target"), link(g, find, "Return Value", pct, "Target"), link(g, find, "Return Value", dead, "Target");
    link(g, pct, "Return Value", sp, "Pct"), link(g, dead, "Return Value", sdd, "Dead");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: health-test: BP_HealthBomber didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

static Entity health_actor(Editor* ed, const float at[3], const char* name) {
    World* w = &ed->world;
    Entity e = editor_spawn_shape(ed, SHAPE_CUBE, at, 0.0);
    if (!e || !ecs_add(w, e, COMP_HEALTH)) return ENTITY_NONE;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, sizeof(((CName*)0)->text), "%s", name);
    return e;
}

bool health_test_setup(Editor* ed) {
    if (health_selftest()) {
        printf("[RealEngine] FAIL: health-test: the self-test failed\n");
        return false;
    }
    World* w = &ed->world;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/HealthTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    const char* F = "/Content/HealthTest";
    BpAsset* target = make_target(F);
    BpAsset* bomber = make_bomber(F);
    if (!target || !bomber || !health_bar_asset()) {
        printf("[RealEngine] FAIL: health-test: the assets (BP_HealthTarget, BP_HealthBomber, WBP_HealthBar)\n");
        return false;
    }
    const float Y = 2000.0f;
    g_bomber = bp_spawn(w, bomber, (float[3]){ 0, Y, 50 }, (float[3]){ 0, 0, 0 });
    g_target = bp_spawn(w, target, (float[3]){ 300, Y, 50 }, (float[3]){ 0, 0, 0 });
    g_crate = health_actor(ed, (float[3]){ -300, Y, 50 }, "Crate");
    g_wall = health_actor(ed, (float[3]){ 0, Y + 300, 50 }, "Wall");
    CHealth* ch = g_crate ? ecs_get(w, g_crate, COMP_HEALTH) : NULL;
    if (ch) ch->death = HEALTH_DEATH_DESTROY, ch->destroy_delay = 0.3f, ch->flags |= HEALTH_NUMBERS;
    CHealth* wh = g_wall ? ecs_get(w, g_wall, COMP_HEALTH) : NULL;
    if (wh) wh->flags |= HEALTH_INVINCIBLE;
    // the player: a Character with Health 60 / 80 and Show Health Bar
    g_player = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_HEALTH));
    if (g_player) {
        CTransform* t = ecs_get(w, g_player, COMP_TRANSFORM);
        t->location[0] = 0, t->location[1] = Y + 1500.0f, t->location[2] = 120.0f;
        t->flags = MOBILITY_MOVABLE;
        CCharacter* c = ecs_get(w, g_player, COMP_CHARACTER);
        c->max_walk_speed = 300.0f, c->jump_z_velocity = 420.0f, c->gravity_scale = 0.0f, c->radius = 34.0f, c->half_height = 88.0f;
        c->camera_distance = 350.0f, c->auto_possess = 1;
        CHealth* ph = ecs_get(w, g_player, COMP_HEALTH);
        ph->max_health = 80.0f, ph->health = 60.0f, ph->flags |= HEALTH_SHOW_BAR;
        snprintf(((CName*)ecs_get(w, g_player, COMP_NAME))->text, sizeof(((CName*)0)->text), "HealthPlayer");
    }
    if (!g_bomber || !g_target || !g_crate || !g_wall || !g_player) {
        printf("[RealEngine] FAIL: health-test: the actors\n");
        return false;
    }
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = Y - 900.0f, ed->cam.pos[2] = 300.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
    editor_play(ed, 0.0);
    return true;
}

static float fvar(World* w, Entity e, const char* name) {
    BpValue v;
    return bp_instance_var(w, e, name, &v) ? v.v[0] : -12345.0f;
}

bool health_test_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: health-test: "), printf(__VA_ARGS__), printf("\n"), ok = false; } } while (0)
    char type[64] = "";
    bp_instance_var_text(w, g_target, "LastType", type, sizeof type);
    BpValue kv;
    Entity killer = ENTITY_NONE;
    if (bp_instance_var(w, g_target, "Killer", &kv)) memcpy(&killer, &kv.v[0], sizeof killer);
    float dmg = fvar(w, g_target, "DamageTotal"), healed = fvar(w, g_target, "HealedTotal");
    int deaths = (int)fvar(w, g_target, "Deaths"), breaks = (int)fvar(w, g_target, "ShieldBreaks");
    int hits = (int)fvar(w, g_bomber, "Hits");
    float pct = fvar(w, g_bomber, "Pct"), dead = fvar(w, g_bomber, "Dead");
    const HealthStats* s = health_stats();
    uint32_t nn = 0, made = 0;
    const HealthNumber* num = health_numbers(&nn);
    for (uint32_t i = 0; i < nn; i++) made += num[i].amount > 0.0f;
    const UiWidget* bar = ui_test_widget("HealthBar");
    const char* text = ui_test_text("HealthText");
    printf("[RealEngine] health-test: target damage %.1f (last %s), healed %.1f, %d deaths (killer %s), %d shield breaks, health %.1f; "
           "radial hits %d; revived %.2f (dead %.0f); crate %s; wall %.1f; %u numbers; bar %.2f \"%s\"; %u damages, %u destroyed\n",
           dmg, type, healed, deaths, killer == g_bomber ? "the bomber" : "?", breaks, health_get(w, g_target), hits, pct, dead,
           ecs_alive(w, g_crate) ? "alive" : "destroyed", health_get(w, g_wall), made, bar ? bar->percent : -1.0f, text ? text : "(none)",
           s->damages, s->destroyed);
    CHECK(fabsf(dmg - 125.0f) < 0.01f, "Event AnyDamage: 30 (15 Damage.Fire x2) + 95 (the radial, what was left) = %.2f", dmg);
    CHECK(!strcmp(type, "Damage.Explosion"), "the last Damage Type \"%s\"", type);
    CHECK(fabsf(healed - 5.0f) < 0.01f, "On Healed: 5 (%.2f)", healed);
    CHECK(breaks == 1, "On Shield Broken once (%d)", breaks);
    CHECK(deaths == 1 && killer == g_bomber, "On Death once, the bomber the killer (%d)", deaths);
    CHECK(hits == 2, "Apply Radial Damage: the target and the crate (the wall invincible, the player away): %d", hits);
    CHECK(fabsf(pct - 0.5f) < 0.01f && dead == 0.0f && fabsf(health_get(w, g_target) - 50.0f) < 0.01f, "Revive to 50: Get Health Percent 0.5, Is Dead false");
    CHECK(!ecs_alive(w, g_crate) && s->destroyed >= 1, "the crate destroyed after its delay");
    CHECK(ecs_alive(w, g_wall) && fabsf(health_get(w, g_wall) - 100.0f) < 0.01f, "the invincible wall untouched");
    CHECK(made >= 1, "the crate's damage numbers");
    CHECK(bp_player(w) == g_player, "the player is the Character with Health");
    CHECK(bar && fabsf(bar->percent - 0.75f) < 0.01f, "WBP_HealthBar's bar at 60 / 80 (%.2f)", bar ? bar->percent : -1.0f);
    CHECK(text && !strcmp(text, "60 / 80"), "WBP_HealthBar's text \"%s\"", text ? text : "(none)");
#undef CHECK
    editor_stop(ed, 0.0);
    printf("[RealEngine] Health test: %s\n", ok ? "OK" : "FAILED");
    return ok;
}
