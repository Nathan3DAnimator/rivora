// Health and damage (health.h, notas-dev/saude-dano.md)
#include "health.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "bp_internal.h"
#include "debug_viewer.h"
#include "gameplay_tags.h"
#include "hud.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

#define HEALTH_KILLS_MAX 256u  // destroys a frame (the rest wait for the next)
#define HEALTH_RADIAL_MAX 2048u // actors one radial damage reaches

static struct {
    HealthHooks hooks;
    HealthStats st;
    HealthNumber num[HEALTH_NUMBERS_MAX];
    uint32_t next_num, live_nums;
    double damage_ms;   // this frame's (until the update)
    uint32_t damages;   // this frame's
    double profile_until; // the Debug Viewer collected lately: the damage calls are timed
    Entity kill[HEALTH_KILLS_MAX];
    Entity radial[HEALTH_RADIAL_MAX];
    float radial_d[HEALTH_RADIAL_MAX];
} H;

static float (*g_damage_scale)(World* w, Entity target, Entity instigator);
void health_set_damage_scale(float (*fn)(World* w, Entity target, Entity instigator)) { g_damage_scale = fn; }

void health_set_hooks(const HealthHooks* h) {
    if (h) H.hooks = *h;
    else memset(&H.hooks, 0, sizeof H.hooks);
}

static CHealth* health_of(World* w, Entity e) { return w && e && ecs_alive(w, e) ? ecs_get(w, e, COMP_HEALTH) : NULL; }

static void fire(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b) {
    if (H.hooks.event) H.hooks.event(w, e, ev, amount, type ? type : "", a, b);
}

float health_multiplier(const CHealth* h, const char* type) {
    float s = 1.0f;
    size_t best = 0;
    for (int k = 0; h && type && type[0] && k < HEALTH_MAX_MULT; k++) {
        const char* t = h->mult[k].tag;
        if (!t[0] || !gt_matches(type, t, false)) continue;
        size_t l = strnlen(t, HEALTH_TAG_LEN);
        if (l > best) best = l, s = h->mult[k].scale; // (the most specific: "Damage.Fire" before "Damage")
    }
    return s;
}

void health_push_number(const float pos[3], float amount, uint32_t kind, bool critical) {
    if (!pos || !(amount > 0.0f)) return;
    HealthNumber* n = &H.num[H.next_num++ % HEALTH_NUMBERS_MAX];
    memcpy(n->pos, pos, 12);
    // (a little apart, so a stream of hits reads: by the slot)
    float jx = (float)((int)(H.next_num * 7u % 9u) - 4) * 9.0f;
    n->pos[0] += jx;
    n->amount = amount, n->age = 0.0f, n->kind = kind, n->critical = critical;
    H.live_nums = HEALTH_NUMBERS_MAX;
}

static void number(World* w, Entity e, float amount, uint32_t kind) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t || !(amount > 0.0f)) return;
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    float p[3] = { t->location[0], t->location[1], t->location[2] };
    p[2] += c ? c->half_height + 25.0f : 100.0f * fmaxf(t->scale[2], 0.5f) * 0.5f + 40.0f; // (over the head / the top)
    health_push_number(p, amount, kind, false);
}

float health_apply_damage(World* w, Entity target, float amount, const char* type, Entity instigator, Entity causer) {
    double t0 = H.profile_until > 0.0 ? re_time_seconds() : 0.0;
    CHealth* h = health_of(w, target);
    if (!h || !(amount > 0.0f) || (h->flags & (HEALTH_DEAD | HEALTH_INVINCIBLE))) return 0.0f;
    float dmg = amount * health_multiplier(h, type);
    if (g_damage_scale) { // (ability.h: the Damage and Defense attributes; no game code runs in it)
        dmg *= g_damage_scale(w, target, instigator);
        h = health_of(w, target);
        if (!h) return 0.0f;
    }
    if (!(dmg > 0.0f)) return 0.0f; // (immune: x0)
    bool had_shield = h->shield > 0.0f;
    float absorbed = had_shield ? fminf(h->shield, dmg) : 0.0f;
    h->shield -= absorbed;
    float lost = fmaxf(fminf(h->health, dmg - absorbed), 0.0f);
    h->health -= lost;
    h->since_damage = 0.0f;
    h->last_instigator = instigator;
    bool broke = had_shield && h->shield <= 0.0f, died = h->health <= 0.0f;
    if (broke) h->shield = 0.0f;
    if (died) h->health = 0.0f, h->flags |= HEALTH_DEAD, h->flags &= ~HEALTH_DESTROYING;
    uint32_t death = h->death;
    bool numbers = (h->flags & HEALTH_NUMBERS) != 0;
    float taken = absorbed + lost;
    H.st.damages++, H.damages++;
    if (died) H.st.deaths++;
    if (numbers) number(w, target, taken, lost > 0.0f ? HEALTH_NUM_DAMAGE : HEALTH_NUM_SHIELD);
    if (t0 > 0.0) H.damage_ms += (re_time_seconds() - t0) * 1000.0;
    hud_on_damage(w, target, instigator, causer, taken); // (the HUD's damage direction: the player's hits, hud.h)
    ai_report_damage(w, target, instigator, taken); // (Q24: the damage sense: a hit agent knows who hit it, ai.h)
    // the events (game code: h may have moved, the actor may be gone): Damaged, Shield Broken, then Death if still dead
    fire(w, target, HEALTH_EV_DAMAGED, taken, type, instigator, causer);
    if (broke && ecs_alive(w, target)) fire(w, target, HEALTH_EV_SHIELD_BROKEN, 0.0f, type, instigator, ENTITY_NONE);
    if (died) {
        if (death == HEALTH_DEATH_RAGDOLL && ecs_alive(w, target) && H.hooks.ragdoll) H.hooks.ragdoll(w, target, true);
        h = health_of(w, target);
        if (h && (h->flags & HEALTH_DEAD)) fire(w, target, HEALTH_EV_DEATH, 0.0f, type, instigator, causer);
    }
    return taken;
}

uint32_t health_apply_radial(World* w, const float origin[3], float radius, float base, float min_damage, float falloff, const char* type,
                             Entity instigator, Entity causer) {
    if (!w || !origin || !(radius > 0.0f) || !(base > 0.0f || min_damage > 0.0f)) return 0;
    uint32_t n = 0;
    const uint64_t need = COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_TRANSFORM);
    float r2 = radius * radius;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // the actors in reach first (the damage's events may move them)
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & need) != need || !a->count) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const CHealth* hc = ecs_arch_column(a, COMP_HEALTH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && n < HEALTH_RADIAL_MAX; i++) {
            if (hc[i].flags & HEALTH_DEAD) continue;
            float d[3] = { t[i].location[0] - origin[0], t[i].location[1] - origin[1], t[i].location[2] - origin[2] };
            float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dd > r2) continue;
            H.radial[n] = ents[i], H.radial_d[n] = sqrtf(dd);
            n++;
        }
    }
    H.st.radial++;
    uint32_t hits = 0;
    for (uint32_t k = 0; k < n; k++) {
        float f = falloff > 0.0f ? powf(fmaxf(1.0f - H.radial_d[k] / radius, 0.0f), falloff) : 1.0f;
        float dmg = min_damage + (base - min_damage) * f;
        if (health_apply_damage(w, H.radial[k], dmg, type, instigator, causer) > 0.0f) hits++;
    }
    return hits;
}

float health_heal(World* w, Entity target, float amount, Entity healer) {
    CHealth* h = health_of(w, target);
    if (!h || !(amount > 0.0f) || (h->flags & HEALTH_DEAD)) return 0.0f;
    float healed = fminf(amount, h->max_health - h->health);
    if (!(healed > 0.0f)) return 0.0f;
    h->health += healed;
    bool numbers = (h->flags & HEALTH_NUMBERS) != 0;
    H.st.heals++;
    if (numbers) number(w, target, healed, HEALTH_NUM_HEAL);
    fire(w, target, HEALTH_EV_HEALED, healed, "", healer, ENTITY_NONE);
    return healed;
}

bool health_revive(World* w, Entity target, float health) {
    CHealth* h = health_of(w, target);
    if (!h) return false;
    bool was_dead = (h->flags & HEALTH_DEAD) != 0;
    h->flags &= ~(HEALTH_DEAD | HEALTH_DESTROYING);
    h->health = health > 0.0f ? fminf(health, h->max_health) : h->max_health;
    h->shield = h->max_shield;
    h->since_damage = 0.0f;
    if (was_dead && h->death == HEALTH_DEATH_RAGDOLL && H.hooks.ragdoll) H.hooks.ragdoll(w, target, false);
    return true;
}

bool health_set_max(World* w, Entity target, float max_health, bool fill) {
    CHealth* h = health_of(w, target);
    if (!h) return false;
    h->max_health = fmaxf(max_health, 0.0f);
    if (fill && !(h->flags & HEALTH_DEAD)) h->health = h->max_health;
    else h->health = fminf(h->health, h->max_health);
    return true;
}

bool health_set_shield(World* w, Entity target, float shield, float max_shield) {
    CHealth* h = health_of(w, target);
    if (!h) return false;
    if (max_shield > 0.0f) h->max_shield = max_shield;
    h->shield = fminf(fmaxf(shield, 0.0f), h->max_shield);
    return true;
}

bool health_set_invincible(World* w, Entity target, bool on) {
    CHealth* h = health_of(w, target);
    if (!h) return false;
    h->flags = on ? (h->flags | HEALTH_INVINCIBLE) : (h->flags & ~HEALTH_INVINCIBLE);
    return true;
}

bool health_set_multiplier(World* w, Entity target, const char* type, float scale) {
    CHealth* h = health_of(w, target);
    char t[HEALTH_TAG_LEN];
    if (!h || !gt_clean(type, t, sizeof t)) return false;
    int free_slot = -1;
    for (int k = 0; k < HEALTH_MAX_MULT; k++) {
        if (h->mult[k].tag[0] && gt_matches(h->mult[k].tag, t, true)) {
            h->mult[k].scale = scale;
            return true;
        }
        if (!h->mult[k].tag[0] && free_slot < 0) free_slot = k;
    }
    if (free_slot < 0) return false;
    snprintf(h->mult[free_slot].tag, HEALTH_TAG_LEN, "%s", t);
    h->mult[free_slot].scale = scale;
    return true;
}

float health_get(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h ? h->health : 0.0f;
}
float health_get_max(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h ? h->max_health : 0.0f;
}
float health_percent(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h && h->max_health > 0.0f ? fminf(fmaxf(h->health / h->max_health, 0.0f), 1.0f) : 0.0f;
}
float health_shield(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h ? h->shield : 0.0f;
}
float health_shield_max(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h ? h->max_shield : 0.0f;
}
bool health_is_dead(World* w, Entity e) {
    const CHealth* h = health_of(w, e);
    return h && (h->flags & HEALTH_DEAD);
}

void health_update(World* w, float dt) {
    double t0 = re_time_seconds();
    uint32_t comps = 0, dead = 0, nkill = 0;
    const uint64_t need = COMP_BIT(COMP_HEALTH);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & need) != need || !a->count) continue;
        CHealth* h = ecs_arch_column(a, COMP_HEALTH);
        const Entity* ents = ecs_arch_entities(a);
        comps += a->count;
        for (uint32_t i = 0; i < a->count; i++) {
            CHealth* c = &h[i];
            c->since_damage += dt;
            if (c->flags & HEALTH_DEAD) {
                dead++;
                bool removes = c->death == HEALTH_DEATH_DESTROY || (c->death == HEALTH_DEATH_RAGDOLL && c->destroy_delay > 0.0f);
                if (removes && !(c->flags & HEALTH_DESTROYING) && c->since_damage >= c->destroy_delay && nkill < HEALTH_KILLS_MAX)
                    c->flags |= HEALTH_DESTROYING, H.kill[nkill++] = ents[i];
                continue;
            }
            if (c->since_damage < c->regen_delay) continue;
            if (c->regen_rate > 0.0f && c->health < c->max_health) c->health = fminf(c->health + c->regen_rate * dt, c->max_health);
            if (c->shield_regen_rate > 0.0f && c->shield < c->max_shield) c->shield = fminf(c->shield + c->shield_regen_rate * dt, c->max_shield);
        }
    }
    for (uint32_t k = 0; k < nkill; k++) { // (after the walk: a destroy moves the archetypes' rows)
        if (!ecs_alive(w, H.kill[k])) continue;
        if (H.hooks.destroy) H.hooks.destroy(w, H.kill[k]);
        else ecs_destroy(w, H.kill[k]);
        H.st.destroyed++;
    }
    if (H.live_nums) { // the floating numbers' ages
        uint32_t live = 0;
        for (uint32_t k = 0; k < HEALTH_NUMBERS_MAX; k++)
            if (H.num[k].amount > 0.0f && H.num[k].age <= HEALTH_NUMBER_LIFE) H.num[k].age += dt, live++;
        H.live_nums = live;
    }
    H.st.components = comps, H.st.dead = dead;
    H.st.damages_frame = H.damages;
    H.st.damage_ms = H.damage_ms;
    H.damages = 0, H.damage_ms = 0.0;
    H.st.update_ms = (re_time_seconds() - t0) * 1000.0;
    if (H.profile_until > 0.0 && t0 > H.profile_until) H.profile_until = 0.0; // (the viewer closed: no more timing)
}

void health_begin_play(World* w) {
    (void)w;
    memset(&H.st, 0, sizeof H.st);
    memset(H.num, 0, sizeof H.num);
    H.next_num = H.live_nums = 0, H.damages = 0, H.damage_ms = 0.0;
}

void health_end_play(void) {
    memset(H.num, 0, sizeof H.num);
    H.live_nums = 0;
    memset(&H.hooks, 0, sizeof H.hooks);
}

const HealthNumber* health_numbers(uint32_t* count) {
    if (count) *count = HEALTH_NUMBERS_MAX;
    return H.num;
}
bool health_numbers_active(void) { return H.live_nums != 0; }
const HealthStats* health_stats(void) { return &H.st; }

// --- The Debug Viewer: the update and the damage (timed while it is open), each actor's health ---
static void collect_health(DvList* l, const DvContext* c) {
    H.profile_until = re_time_seconds() + 1.0; // (the damage calls timed while the viewer collects)
    l->gpu = false, l->estimated = true;
    l->ms = H.st.update_ms + H.st.damage_ms;
    snprintf(l->note, sizeof l->note, "%u Health components (%u dead); last frame %u damages; since play %u damages, %u heals, %u deaths",
             H.st.components, H.st.dead, H.st.damages_frame, H.st.damages, H.st.heals, H.st.deaths);
    char info[48];
    snprintf(info, sizeof info, "regeneration, deaths, %u destroyed", H.st.destroyed);
    dv_add(l, "Update", info, (float)H.st.update_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u calls", H.st.damages_frame);
    dv_add(l, "Damage and healing", info, (float)H.st.damage_ms, ENTITY_NONE);
    World* w = c ? c->world : NULL;
    if (!w || !H.st.components) return;
    float each = (float)(l->ms / (double)H.st.components);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_HEALTH)) || !a->count) continue;
        const CHealth* h = ecs_arch_column(a, COMP_HEALTH);
        const CName* nm = (a->mask & COMP_BIT(COMP_NAME)) ? ecs_arch_column(a, COMP_NAME) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (h[i].flags & HEALTH_DEAD) snprintf(info, sizeof info, "dead");
            else if (h[i].max_shield > 0.0f)
                snprintf(info, sizeof info, "%.0f / %.0f, shield %.0f / %.0f", h[i].health, h[i].max_health, h[i].shield, h[i].max_shield);
            else snprintf(info, sizeof info, "%.0f / %.0f", h[i].health, h[i].max_health);
            dv_add(l, nm ? nm[i].text : "Actor", info, each, ents[i]);
        }
    }
}
void health_register_debug(void) { dv_register("Health and Damage", collect_health); }

// --- WBP_HealthBar: the example widget ---
static int find_pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    return -1;
}
static bool wire(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = find_pin(g, a, 1, ap), pb = find_pin(g, b, 0, bpn);
    return pa >= 0 && pb >= 0 && bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb);
}
static uint32_t add_node(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, x, y, text, NULL);
    return n ? n->id : 0;
}

uint32_t health_bar_asset(void) {
    const char* vp = "/Content/UI/WBP_HealthBar.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_HealthBar", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    // the panel at the bottom left: a dark frame, the shield's thin bar over the health's, the numbers on the health
    static const struct { uint8_t kind; const char* name; float x, y, w, h; float color[4], bg[4]; } wd[] = {
        { UIW_BORDER, "Frame", 32, -32, 436, 78, { 1, 1, 1, 1 }, { 0.02f, 0.02f, 0.025f, 0.62f } },
        { UIW_PROGRESS, "ShieldBar", 50, -82, 400, 12, { 0.25f, 0.62f, 1.0f, 1.0f }, { 0.08f, 0.1f, 0.14f, 0.9f } },
        { UIW_PROGRESS, "HealthBar", 50, -46, 400, 28, { 0.86f, 0.16f, 0.13f, 1.0f }, { 0.12f, 0.05f, 0.05f, 0.9f } },
        { UIW_TEXT, "HealthText", 62, -46, 380, 28, { 1, 1, 1, 1 }, { 0, 0, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof wd / sizeof wd[0]; i++) {
        int k = ui_tree_add(t, wd[i].kind, 0);
        if (k < 0) return 0;
        UiWidget* u = &t->w[k];
        snprintf(u->name, UI_NAME, "%s", wd[i].name);
        u->anchor[0] = u->anchor[2] = 0.0f, u->anchor[1] = u->anchor[3] = 1.0f; // (bottom left)
        u->align[0] = 0.0f, u->align[1] = 1.0f;
        u->pos[0] = wd[i].x, u->pos[1] = wd[i].y, u->size[0] = wd[i].w, u->size[1] = wd[i].h;
        memcpy(u->color, wd[i].color, sizeof u->color);
        memcpy(u->bg, wd[i].bg, sizeof u->bg);
        u->radius = wd[i].kind == UIW_TEXT ? 0.0f : 4.0f;
        if (wd[i].kind == UIW_PROGRESS) u->percent = 1.0f;
        if (wd[i].kind == UIW_TEXT) snprintf(u->text, sizeof u->text, "100 / 100"), u->font_size = 20.0f, u->bold = 1, u->shadow = 1, u->align[1] = 0.5f;
        if (wd[i].kind == UIW_TEXT) u->pos[1] = wd[i].y - wd[i].h * 0.5f; // (the pivot at its middle: the text centered on the bar)
    }
    // Event Tick: the player's pawn's health and shield into the bars, its numbers into the text
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t tick = add_node(a, g, BPK_EVENT_TICK, 0, 0, NULL);
    uint32_t pl = add_node(a, g, BPK_GET_PLAYER, 0, 220, NULL);
    uint32_t hp = add_node(a, g, BPK_GET_HEALTH_PERCENT, 300, 220, NULL);
    uint32_t sh = add_node(a, g, BPK_GET_SHIELD, 300, 340, NULL);
    uint32_t gh = add_node(a, g, BPK_GET_HEALTH, 300, 460, NULL);
    uint32_t gm = add_node(a, g, BPK_GET_MAX_HEALTH, 300, 560, NULL);
    uint32_t th = add_node(a, g, BPK_FLOAT_TO_INT, 560, 460, NULL);
    uint32_t tm = add_node(a, g, BPK_FLOAT_TO_INT, 560, 560, NULL);
    uint32_t fmt = add_node(a, g, BPK_FORMAT_TEXT, 760, 460, NULL);
    uint32_t has = add_node(a, g, BPK_GREATER, 560, 340, NULL);
    uint32_t sp1 = add_node(a, g, BPK_WIDGET_SET_PERCENT, 320, 0, "HealthBar");
    uint32_t sp2 = add_node(a, g, BPK_WIDGET_SET_PERCENT, 620, 0, "ShieldBar");
    uint32_t vis = add_node(a, g, BPK_WIDGET_SET_VISIBLE, 920, 0, "ShieldBar");
    uint32_t txt = add_node(a, g, BPK_WIDGET_SET_TEXT, 1220, 0, "HealthText");
    BpNode* fn = bp_node(g, fmt);
    int fp = find_pin(g, fmt, 0, "Format");
    if (fn && fp >= 0) snprintf(fn->pins[fp].slit, sizeof fn->pins[fp].slit, "{0} / {1}");
    bool ok = tick && pl && hp && sh && gh && gm && th && tm && fmt && has && sp1 && sp2 && vis && txt;
    ok = ok && wire(g, tick, "", sp1, "") && wire(g, sp1, "", sp2, "") && wire(g, sp2, "", vis, "") && wire(g, vis, "", txt, "");
    ok = ok && wire(g, pl, "Player", hp, "Target") && wire(g, pl, "Player", sh, "Target") && wire(g, pl, "Player", gh, "Target") &&
         wire(g, pl, "Player", gm, "Target");
    ok = ok && wire(g, hp, "Return Value", sp1, "In Percent") && wire(g, sh, "Percent", sp2, "In Percent") && wire(g, sh, "Max Shield", has, "A") &&
         wire(g, has, "", vis, "Visible");
    ok = ok && wire(g, gh, "Return Value", th, "") && wire(g, gm, "Return Value", tm, "") && wire(g, th, "", fmt, "{0}") &&
         wire(g, tm, "", fmt, "{1}") && wire(g, fmt, "", txt, "In Text");
    if (!ok) printf("[RealEngine] WBP_HealthBar: its graph could not be made\n");
    bp_compile(a);
    if (!bp_save(a)) return 0;
    return a->hash;
}

// --- The self-test ---
static struct { uint32_t n[HEALTH_EV_COUNT]; float amount[HEALTH_EV_COUNT]; char type[HEALTH_EV_COUNT][GT_TAG_LEN]; Entity a[HEALTH_EV_COUNT]; } g_rec;
static void rec_event(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b) {
    (void)w, (void)e, (void)b;
    g_rec.n[ev]++, g_rec.amount[ev] = amount, g_rec.a[ev] = a;
    snprintf(g_rec.type[ev], GT_TAG_LEN, "%s", type);
}

static Entity actor(World* w, float x, float max_health) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_HEALTH));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x;
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    h->max_health = h->health = max_health;
    return e;
}

int health_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: health: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)
    World w;
    if (!ecs_init(&w)) return 1;
    HealthHooks hooks = { rec_event, NULL, NULL };
    health_set_hooks(&hooks);
    health_begin_play(&w);
    memset(&g_rec, 0, sizeof g_rec);
    Entity src = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM)); // (the instigator)
    // the damage types: the most specific multiplier
    Entity e = actor(&w, 0, 100);
    CHECK(health_set_multiplier(&w, e, "Damage.Fire", 2.0f) && health_set_multiplier(&w, e, "Damage", 0.5f), "multipliers set");
    CHECK(NEAR(health_apply_damage(&w, e, 10, "Damage.Fire.Burning", src, src), 20.0f), "Damage.Fire.Burning x2 (Damage.Fire is more specific than Damage)");
    CHECK(NEAR(health_apply_damage(&w, e, 10, "Damage.Physical", src, src), 5.0f), "Damage.Physical x0.5 (under Damage)");
    CHECK(NEAR(health_apply_damage(&w, e, 10, "", src, src), 10.0f), "no type: x1");
    CHECK(NEAR(health_apply_damage(&w, e, 10, "Fire", src, src), 10.0f), "\"Fire\" is not \"Damage.Fire\"");
    CHECK(NEAR(health_get(&w, e), 55.0f), "health 55 (%.2f)", health_get(&w, e));
    CHECK(g_rec.n[HEALTH_EV_DAMAGED] == 4 && g_rec.a[HEALTH_EV_DAMAGED] == src && NEAR(g_rec.amount[HEALTH_EV_DAMAGED], 10.0f), "Damaged events (4, the instigator, the amount)");
    CHECK(health_set_multiplier(&w, e, "Damage.Poison", 0.0f) && health_apply_damage(&w, e, 50, "Damage.Poison", src, src) == 0.0f, "immune (x0)");
    CHECK(!health_set_multiplier(&w, e, "Damage.Ice", 3.0f), "three multipliers: the fourth refused");
    CHECK(health_set_multiplier(&w, e, "Damage.Fire", 3.0f) && NEAR(health_multiplier(ecs_get(&w, e, COMP_HEALTH), "Damage.Fire"), 3.0f), "a type's multiplier replaced");
    // the shield takes it first; broken once
    Entity s = actor(&w, 0, 100);
    CHECK(health_set_shield(&w, s, 50, 50), "shield set");
    memset(&g_rec, 0, sizeof g_rec);
    CHECK(NEAR(health_apply_damage(&w, s, 30, "Damage.Physical", src, src), 30.0f) && NEAR(health_shield(&w, s), 20.0f) && NEAR(health_get(&w, s), 100.0f), "the shield absorbs");
    CHECK(g_rec.n[HEALTH_EV_SHIELD_BROKEN] == 0, "not broken yet");
    CHECK(NEAR(health_apply_damage(&w, s, 30, "Damage.Physical", src, src), 30.0f) && NEAR(health_shield(&w, s), 0.0f) && NEAR(health_get(&w, s), 90.0f), "the rest to the health");
    CHECK(g_rec.n[HEALTH_EV_SHIELD_BROKEN] == 1 && g_rec.a[HEALTH_EV_SHIELD_BROKEN] == src, "Shield Broken once");
    health_apply_damage(&w, s, 5, NULL, src, src);
    CHECK(g_rec.n[HEALTH_EV_SHIELD_BROKEN] == 1, "no second Shield Broken without a shield");
    // regeneration: after its delay, at its rates
    CHealth* sh = ecs_get(&w, s, COMP_HEALTH);
    sh->regen_delay = 1.0f, sh->regen_rate = 10.0f, sh->shield_regen_rate = 20.0f;
    for (int i = 0; i < 9; i++) health_update(&w, 0.1f); // 0.9 s
    CHECK(NEAR(health_get(&w, s), 85.0f) && NEAR(health_shield(&w, s), 0.0f), "nothing before the delay (%.2f)", health_get(&w, s));
    for (int i = 0; i < 11; i++) health_update(&w, 0.1f); // 2.0 s: 1.0 s of it regenerating
    CHECK(fabsf(health_get(&w, s) - 95.0f) < 1.01f && fabsf(health_shield(&w, s) - 20.0f) < 2.01f, "regenerated (%.2f, shield %.2f)", health_get(&w, s), health_shield(&w, s));
    health_apply_damage(&w, s, 1, NULL, src, src);
    float before = health_get(&w, s);
    health_update(&w, 0.5f);
    CHECK(NEAR(health_get(&w, s), before), "a new damage waits the delay again");
    // healing (clamped), death and its event, no damage nor heal on the dead, revive
    Entity d = actor(&w, 0, 100);
    memset(&g_rec, 0, sizeof g_rec);
    health_apply_damage(&w, d, 50, NULL, src, src);
    CHECK(NEAR(health_heal(&w, d, 30, src), 30.0f) && NEAR(health_get(&w, d), 80.0f), "healed 30");
    CHECK(NEAR(health_heal(&w, d, 50, src), 20.0f) && NEAR(health_get(&w, d), 100.0f), "healed up to the max (20)");
    CHECK(g_rec.n[HEALTH_EV_HEALED] == 2 && NEAR(g_rec.amount[HEALTH_EV_HEALED], 20.0f), "On Healed (2, its amount)");
    CHECK(NEAR(health_apply_damage(&w, d, 1000, "Damage.Fall", src, src), 100.0f), "the damage taken is what it had");
    CHECK(health_is_dead(&w, d) && g_rec.n[HEALTH_EV_DEATH] == 1 && g_rec.a[HEALTH_EV_DEATH] == src && !strcmp(g_rec.type[HEALTH_EV_DEATH], "Damage.Fall"),
          "dead; On Death with the killer and the type");
    CHECK(health_apply_damage(&w, d, 10, NULL, src, src) == 0.0f && health_heal(&w, d, 10, src) == 0.0f && g_rec.n[HEALTH_EV_DEATH] == 1, "the dead take nothing");
    CHECK(health_revive(&w, d, 0) && !health_is_dead(&w, d) && NEAR(health_get(&w, d), 100.0f), "revived full");
    CHECK(health_revive(&w, d, 40) && NEAR(health_get(&w, d), 40.0f) && NEAR(health_percent(&w, d), 0.4f), "revived to 40 (Get Health Percent 0.4)");
    CHECK(health_set_max(&w, d, 200, false) && NEAR(health_get(&w, d), 40.0f) && health_set_max(&w, d, 30, false) && NEAR(health_get(&w, d), 30.0f) &&
          health_set_max(&w, d, 150, true) && NEAR(health_get(&w, d), 150.0f), "Set Max Health (kept, clamped, filled)");
    // the destroy after its delay
    Entity k = actor(&w, 0, 10);
    CHealth* kh = ecs_get(&w, k, COMP_HEALTH);
    kh->death = HEALTH_DEATH_DESTROY, kh->destroy_delay = 0.5f;
    health_apply_damage(&w, k, 20, NULL, src, src);
    health_update(&w, 0.3f);
    CHECK(ecs_alive(&w, k), "kept until its delay");
    health_update(&w, 0.3f);
    CHECK(!ecs_alive(&w, k), "destroyed after its delay");
    // invincible
    Entity v = actor(&w, 0, 100);
    health_set_invincible(&w, v, true);
    CHECK(health_apply_damage(&w, v, 50, NULL, src, src) == 0.0f && NEAR(health_get(&w, v), 100.0f), "invincible");
    health_set_invincible(&w, v, false);
    // radial damage: full at the middle, half at half the radius (falloff 1), nothing outside; the minimum at the edge
    Entity r0 = actor(&w, 10000, 1000), r1 = actor(&w, 10250, 1000), r2 = actor(&w, 10600, 1000);
    const float o[3] = { 10000, 0, 0 };
    CHECK(health_apply_radial(&w, o, 500, 100, 0, 1, "Damage.Explosion", src, src) == 2, "two in reach");
    CHECK(NEAR(health_get(&w, r0), 900.0f) && NEAR(health_get(&w, r1), 950.0f) && NEAR(health_get(&w, r2), 1000.0f), "falloff (%.1f %.1f %.1f)",
          health_get(&w, r0), health_get(&w, r1), health_get(&w, r2));
    CHECK(health_apply_radial(&w, o, 500, 100, 20, 0, "Damage.Explosion", src, src) == 2 && NEAR(health_get(&w, r1), 850.0f), "falloff 0: full everywhere");
    CHECK(health_apply_radial(&w, o, 500, 100, 20, 2, NULL, src, src) == 2 && NEAR(health_get(&w, r1), 810.0f), "falloff 2 with a minimum of 20 (%.1f)", health_get(&w, r1));
    // 1000 actors damaged every frame (the measure: notas-dev/saude-dano.md)
    ecs_shutdown(&w);
    if (!ecs_init(&w)) return fails + 1;
    enum { N = 1000, FRAMES = 600 };
    static Entity many[N];
    for (int i = 0; i < N; i++) {
        many[i] = actor(&w, (float)(i % 40) * 200.0f, 100.0f);
        CHealth* h = ecs_get(&w, many[i], COMP_HEALTH);
        h->max_shield = h->shield = 25.0f, h->regen_delay = 0.0f, h->regen_rate = 120.0f, h->shield_regen_rate = 10.0f; // (kept alive: 2 a frame back)
        health_set_multiplier(&w, many[i], "Damage.Fire", 1.5f);
    }
    memset(&g_rec, 0, sizeof g_rec);
    double t0 = re_time_seconds(), upd = 0.0;
    float total = 0.0f;
    for (int f = 0; f < FRAMES; f++) {
        for (int i = 0; i < N; i++) total += health_apply_damage(&w, many[i], 1.0f, (i & 1) ? "Damage.Fire" : "Damage.Physical", src, src);
        double u0 = re_time_seconds();
        health_update(&w, 1.0f / 60.0f);
        upd += re_time_seconds() - u0;
    }
    double ms = (re_time_seconds() - t0) * 1000.0 / FRAMES;
    uint32_t events = g_rec.n[HEALTH_EV_DAMAGED];
    double radial0 = re_time_seconds();
    uint32_t rh = 0;
    for (int f = 0; f < 100; f++) rh += health_apply_radial(&w, (float[3]){ 4000, 0, 0 }, 3000, 1, 0, 1, "Damage.Explosion", src, src);
    double radial_ms = (re_time_seconds() - radial0) * 1000.0 / 100.0;
    printf("[RealEngine] health: 1000 actors damaged a frame: %.4f ms a frame (update %.4f ms; %u events, %.0f taken); radial over %u actors: %.4f ms\n", ms,
           upd * 1000.0 / FRAMES, events, total, rh / 100u, radial_ms);
    CHECK(events == (uint32_t)N * FRAMES, "every damage an event (%u)", events);
    CHECK(rh == 100u * 725u, "the radial damage hit the 725 actors inside 3000 cm every time (the 50 on its edge take 0) (%u)", rh);
    CHECK(ms < 1.0, "1000 actors in %.3f ms a frame (the budget: 1 ms)", ms);
    CHECK(health_stats()->components == N, "the update saw the %d components (%u)", N, health_stats()->components);
    ecs_shutdown(&w);
    health_set_hooks(NULL);
#undef NEAR
#undef CHECK
    printf("[RealEngine] health: self-test %s\n", fails ? "FAILED" : "ok");
    return fails;
}
