// HUD modular (hud.h, notas-dev/hud.md): the data and the run time of the HUD's pieces. No ImGui here (re_render); the
// drawing is hud_widget.c's.
#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "debug_viewer.h"
#include "dialogue.h"
#include "health.h"
#include "platform.h"
#include "platform_files.h"
#include "sequencer.h"
#include "ui_widget.h"

#define HUD_BARS_MAX 64
#define HUD_SOURCES_MAX 8
#define HIT_LIFE 1.5f

const char* const g_hud_scale_modes[HUD_SCALE_COUNT] = { "Screen Height", "Shortest Side", "Fit 1920 x 1080", "Fixed Pixels" };

static struct {
    HudMarker mk[HUD_MARKERS_MAX];
    uint32_t next_id;
    HudMessage msg[HUD_MSG_MAX];
    uint32_t serial;
    uint32_t max_shown;        // the feed's Max Shown (the last feed drawn; 5)
    float default_life;        // the feed's Duration (3 s)
    HudHit hit[HUD_HITS_MAX];
    uint32_t next_hit;
    HudBarState bar[HUD_BARS_MAX];
    float spread_target, spread;
    bool visible, photo, seq_hidden, shown;
    HudView view;
    World* w;
    Entity player, layout_e, layout_widget;
    CHudLayout layout;
    bool has_layout;
    HudAttributeFn src[HUD_SOURCES_MAX];
    uint32_t nsrc;
    HudStats st;
    bool numbers_drawn;
    uint32_t frame;
    float dt;
    // the frame's drawing (hud_widget.c), moved to the stats at the next frame
    double draw_acc;
    uint32_t vtx_acc, idx_acc, pieces_acc;
    double kind_acc[8];
    uint32_t kind_vtx_acc[8];
} H = { .max_shown = 5, .default_life = 3.0f, .visible = true, .shown = true };

HudStats* hud_stats(void) { return &H.st; }

// --- Markers ---
static void marker_place(World* w, HudMarker* m) {
    if (!m->actor) return;
    const CTransform* t = w && ecs_alive(w, m->actor) ? ecs_get(w, m->actor, COMP_TRANSFORM) : NULL;
    if (t) m->pos[0] = t->location[0], m->pos[1] = t->location[1], m->pos[2] = t->location[2] + m->offset_z;
}

uint32_t hud_add_marker(World* w, Entity actor, const float pos[3], const char* label, const float color[4], float offset_z, uint32_t flags) {
    if (!actor && !pos) return 0;
    if (!w) w = H.w;
    if (actor && w && !ecs_alive(w, actor)) return 0;
    const uint32_t kind = flags & (HUD_MK_COMPASS | HUD_MK_SCREEN);
    HudMarker* m = NULL;
    for (uint32_t i = 0; i < HUD_MARKERS_MAX && actor; i++) // the same actor's of the same kind: replaced
        if (H.mk[i].id && H.mk[i].actor == actor && (H.mk[i].flags & (HUD_MK_COMPASS | HUD_MK_SCREEN)) == kind) m = &H.mk[i];
    for (uint32_t i = 0; i < HUD_MARKERS_MAX && !m; i++)
        if (!H.mk[i].id) m = &H.mk[i];
    if (!m) return 0;
    uint32_t id = m->id ? m->id : ++H.next_id;
    if (!id) id = ++H.next_id; // (wrapped)
    memset(m, 0, sizeof *m);
    m->id = id, m->actor = actor, m->flags = flags, m->offset_z = offset_z;
    if (pos) memcpy(m->pos, pos, 12);
    snprintf(m->label, sizeof m->label, "%s", label ? label : "");
    if (color && color[3] > 0.0f) memcpy(m->color, color, 16);
    else m->color[0] = 1.0f, m->color[1] = 0.82f, m->color[2] = 0.25f, m->color[3] = 1.0f; // (a warm gold: Unreal's objective)
    marker_place(w, m);
    return id;
}

uint32_t hud_remove_marker(Entity actor, uint32_t kinds) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < HUD_MARKERS_MAX; i++)
        if (H.mk[i].id && H.mk[i].actor == actor && (!kinds || (H.mk[i].flags & kinds))) memset(&H.mk[i], 0, sizeof H.mk[i]), n++;
    return n;
}
bool hud_remove_marker_id(uint32_t id) {
    for (uint32_t i = 0; id && i < HUD_MARKERS_MAX; i++)
        if (H.mk[i].id == id) return memset(&H.mk[i], 0, sizeof H.mk[i]), true;
    return false;
}
void hud_clear_markers(void) { memset(H.mk, 0, sizeof H.mk); }
const HudMarker* hud_markers(uint32_t* count) {
    if (count) *count = HUD_MARKERS_MAX;
    return H.mk;
}
uint32_t hud_marker_count(uint32_t kinds) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < HUD_MARKERS_MAX; i++) n += H.mk[i].id && (!kinds || (H.mk[i].flags & kinds));
    return n;
}

// --- Messages ---
// "+12 Gold" -> 12, "Gold" (a sign, digits, a space, the rest); false: not a count
static bool split_count(const char* s, float* num, const char** rest) {
    const char* p = s;
    if (*p != '+' && *p != '-') return false;
    p++;
    if (*p < '0' || *p > '9') return false;
    char* e = NULL;
    double v = strtod(p, &e);
    if (!e || *e != ' ' || !e[1]) return false;
    *num = (float)(s[0] == '-' ? -v : v), *rest = e + 1;
    return true;
}
static void msg_build(HudMessage* m, const char* rest, float num, bool numeric) {
    if (numeric) snprintf(m->text, sizeof m->text, "%s%g %s", num < 0.0f ? "-" : "+", fabs((double)num), rest);
    else if (m->count > 1) {
        char base[HUD_MSG_TEXT];
        snprintf(base, sizeof base, "%s", rest);
        snprintf(m->text, sizeof m->text, "%s x%d", base, (int)m->count);
    } else snprintf(m->text, sizeof m->text, "%s", rest);
}

// (the base text of a message: the part without its "+N" or " xN")
static bool msg_same(const HudMessage* m, const char* text, bool numeric, const char* rest) {
    float n2;
    const char* r2;
    bool num2 = split_count(m->text, &n2, &r2);
    if (numeric) return num2 && !strcmp(r2, rest);
    if (num2) return false;
    if (m->count <= 1) return !strcmp(m->text, text);
    size_t l = strlen(text);
    return !strncmp(m->text, text, l) && m->text[l] == ' ' && m->text[l + 1] == 'x';
}

bool hud_message(const char* text, float duration, const float color[4]) {
    if (!text || !text[0]) return false;
    float num = 0.0f;
    const char* rest = text;
    bool numeric = split_count(text, &num, &rest);
    float life = duration > 0.0f ? duration : H.default_life;
    // the same one showing (or waiting): it stacks
    for (uint32_t i = 0; i < HUD_MSG_MAX; i++) {
        HudMessage* m = &H.msg[i];
        if (!m->text[0] || m->age > m->life || !msg_same(m, text, numeric, rest)) continue;
        float have = 0.0f;
        const char* r2;
        if (numeric) split_count(m->text, &have, &r2);
        m->count++;
        msg_build(m, rest, have + num, numeric);
        if (m->age > 0.25f) m->age = 0.25f; // (lives on, without sliding in again)
        if (life > m->life) m->life = life;
        if (color && color[3] > 0.0f) memcpy(m->color, color, 16);
        H.st.stacked++;
        return true;
    }
    HudMessage* m = NULL;
    for (uint32_t i = 0; i < HUD_MSG_MAX && !m; i++)
        if (!H.msg[i].text[0]) m = &H.msg[i];
    if (!m) return false;
    memset(m, 0, sizeof *m);
    m->count = 1, m->life = life, m->age = -1.0f, m->serial = ++H.serial; // (waits for room in the feed)
    msg_build(m, text, num, false);
    if (numeric) msg_build(m, rest, num, true);
    if (color && color[3] > 0.0f) memcpy(m->color, color, 16);
    else m->color[0] = m->color[1] = m->color[2] = m->color[3] = 1.0f;
    return true;
}
const HudMessage* hud_messages(uint32_t* count) {
    if (count) *count = HUD_MSG_MAX;
    return H.msg;
}
uint32_t hud_message_count(bool shown_only) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < HUD_MSG_MAX; i++) n += H.msg[i].text[0] && (!shown_only || H.msg[i].age >= 0.0f);
    return n;
}
void hud_clear_messages(void) { memset(H.msg, 0, sizeof H.msg); }

static void messages_tick(float dt) {
    uint32_t shown = 0;
    for (uint32_t i = 0; i < HUD_MSG_MAX; i++) {
        HudMessage* m = &H.msg[i];
        if (!m->text[0] || m->age < 0.0f) continue;
        m->age += dt;
        if (m->age > m->life + 0.35f) memset(m, 0, sizeof *m); // (its fade out done)
        else shown++;
    }
    while (shown < H.max_shown) { // the oldest waiting comes in
        HudMessage* first = NULL;
        for (uint32_t i = 0; i < HUD_MSG_MAX; i++)
            if (H.msg[i].text[0] && H.msg[i].age < 0.0f && (!first || H.msg[i].serial < first->serial)) first = &H.msg[i];
        if (!first) break;
        first->age = 0.0f, shown++, H.st.shown_messages++;
    }
}

// --- Damage numbers: the Health's buffer ---
void hud_damage_number(const float pos[3], float amount, bool critical, bool heal) {
    if (!pos || !(fabsf(amount) > 0.0f)) return;
    health_push_number(pos, fabsf(amount), heal ? HEALTH_NUM_HEAL : HEALTH_NUM_DAMAGE, critical);
}

// --- Crosshair ---
void hud_set_spread(float spread) { H.spread_target = fminf(fmaxf(spread, 0.0f), 1.0f); }
float hud_spread(void) { return H.spread; }

// --- The damage's direction ---
static bool actor_pos(World* w, Entity e, float p[3]) {
    const CTransform* t = w && e && ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (t) memcpy(p, t->location, 12);
    return t != NULL;
}
void hud_on_damage(World* w, Entity victim, Entity instigator, Entity causer, float amount) {
    if (!w || !victim || victim != H.player || !(amount > 0.0f)) return;
    float from[3];
    if (!actor_pos(w, causer != victim ? causer : ENTITY_NONE, from) && !actor_pos(w, instigator != victim ? instigator : ENTITY_NONE, from)) return;
    HudHit* h = &H.hit[H.next_hit++ % HUD_HITS_MAX];
    memcpy(h->from, from, 12);
    h->age = 0.0f, h->amount = amount;
}
const HudHit* hud_hits(uint32_t* count) {
    if (count) *count = HUD_HITS_MAX;
    return H.hit;
}

// --- Visibility ---
void hud_set_visible(bool visible) { H.visible = visible; }
bool hud_visible(void) { return H.visible; }
bool hud_shown(void) { return H.shown; }
void hud_set_photo_mode(bool on) { H.photo = on; }
bool hud_photo_mode(void) { return H.photo; }

// --- Attributes ---
void hud_add_attribute_source(HudAttributeFn fn) {
    for (uint32_t i = 0; i < H.nsrc; i++)
        if (H.src[i] == fn) return;
    if (fn && H.nsrc < HUD_SOURCES_MAX) H.src[H.nsrc++] = fn;
}
static bool var_float(World* w, Entity e, const char* name, float* v) {
    BpValue x;
    if (!bp_instance_var(w, e, name, &x)) return false;
    *v = x.v[0];
    return true;
}
bool hud_attribute(World* w, Entity e, const char* attribute, float* value, float* max) {
    if (!w || !e || !ecs_alive(w, e) || !attribute) return false;
    const char* a = attribute[0] ? attribute : "Health";
    float v = 0.0f, m = 0.0f;
    if (!_stricmp(a, "Health") && ecs_get(w, e, COMP_HEALTH)) v = health_get(w, e), m = health_get_max(w, e);
    else if (!_stricmp(a, "Shield") && ecs_get(w, e, COMP_HEALTH)) v = health_shield(w, e), m = health_shield_max(w, e);
    else {
        bool found = false;
        for (uint32_t i = 0; i < H.nsrc && !found; i++) found = H.src[i](w, e, a, &v, &m);
        if (!found) { // a Blueprint's float variable; its max: Max<Name> / <Name>Max / Max <Name>
            if (!var_float(w, e, a, &v)) return false;
            char mx[96];
            snprintf(mx, sizeof mx, "Max%s", a);
            if (!var_float(w, e, mx, &m)) {
                snprintf(mx, sizeof mx, "%sMax", a);
                if (!var_float(w, e, mx, &m)) snprintf(mx, sizeof mx, "Max %s", a), var_float(w, e, mx, &m);
            }
        }
    }
    if (value) *value = v;
    if (max) *max = m;
    return true;
}

static Entity actor_by_name(World* w, const char* name) {
    for (uint32_t ai = 0; w && name && name[0] && ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || !a->count) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* es = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return es[i];
    }
    return ENTITY_NONE;
}

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (s && *s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// a bar's state: the actor bound (Bind Attribute Bar, or its Actor field by name; "": the player)
typedef struct BarExt { Entity actor; uint32_t name_hash; bool bound; } BarExt;
static BarExt g_bar_ext[HUD_BARS_MAX];

static HudBarState* bar_slot(Entity owner, int widget, bool make) {
    HudBarState* free_s = NULL;
    for (int i = 0; i < HUD_BARS_MAX; i++) {
        HudBarState* s = &H.bar[i];
        if (s->used && s->owner == owner && s->widget == widget) return s;
        if (!s->used && !free_s) free_s = s;
    }
    if (!make) return NULL;
    if (!free_s) { // full: the one not drawn the longest
        free_s = &H.bar[0];
        for (int i = 1; i < HUD_BARS_MAX; i++)
            if (H.bar[i].frame < free_s->frame) free_s = &H.bar[i];
    }
    memset(free_s, 0, sizeof *free_s);
    memset(&g_bar_ext[free_s - H.bar], 0, sizeof g_bar_ext[0]);
    free_s->owner = owner, free_s->widget = (int16_t)widget, free_s->used = 1, free_s->shown = free_s->trail = free_s->gain = -1.0f;
    return free_s;
}

HudBarState* hud_bar_state(Entity owner, int widget, const UiWidget* w, float dt) {
    HudBarState* s = bar_slot(owner, widget, true);
    BarExt* x = &g_bar_ext[s - H.bar];
    if (s->frame == H.frame && s->shown >= 0.0f) return s; // (once a frame)
    s->frame = H.frame;
    World* world = H.w;
    // the actor: bound, or by its name, or the player
    uint32_t nh = fnv(w->text);
    if (!x->bound && (x->name_hash != nh || !x->actor || (world && !ecs_alive(world, x->actor))))
        x->name_hash = nh, x->actor = w->text[0] ? actor_by_name(world, w->text) : ENTITY_NONE;
    Entity e = x->actor ? x->actor : (!x->bound && !w->text[0] ? H.player : ENTITY_NONE);
    float v = 0.0f, m = 0.0f;
    bool have = world && hud_attribute(world, e, w->hint, &v, &m);
    if (have && !(m > 0.0f)) m = w->vmax > 0.0f ? w->vmax : 100.0f;
    float f = have ? fminf(fmaxf(v / m, 0.0f), 1.0f) : -1.0f;
    s->value = v, s->max = m;
    if (f < 0.0f) { // nothing to show: the bar keeps the designer's percent and is hidden in play
        s->shown = s->trail = s->gain = -2.0f;
        return s;
    }
    if (s->shown < 0.0f) s->shown = s->trail = s->gain = s->target = f; // (the first frame: no animation)
    if (f < s->target - 1e-5f) s->timer = w->vmin > 0.0f ? w->vmin : 0.0f; // a new loss: the chunk lost waits, then drains
    s->target = f;
    float tau = w->value > 0.0f ? w->value : 0.0001f;
    s->shown += (f - s->shown) * (1.0f - expf(-dt / tau)); // (the fill follows the value)
    s->gain = f > s->shown ? f : s->shown;                  // (a gain shows at once in its color; the fill catches up)
    if (s->trail <= s->shown) s->trail = s->shown, s->timer = 0.0f;
    else if (s->timer > 0.0f) s->timer -= dt;
    else s->trail = fmaxf(s->trail - (w->padding > 0.0f ? w->padding : 0.8f) * dt, s->shown);
    (void)widget;
    return s;
}

bool hud_bind_bar(World* w, Entity widget, const char* bar, Entity actor, const char* attribute) {
    if (!bar || !bar[0]) return false;
    Entity owner = widget;
    UiWidget* x = ui_live_widget(widget, bar, &owner);
    if (!x || x->kind != UIW_ATTR_BAR) return false;
    if (attribute && attribute[0]) snprintf(x->hint, sizeof x->hint, "%s", attribute);
    const CName* nm = actor && w && ecs_alive(w, actor) ? ecs_get(w, actor, COMP_NAME) : NULL;
    snprintf(x->text, sizeof x->text, "%s", nm ? nm->text : "");
    int idx = ui_live_widget_index(owner, x);
    HudBarState* s = bar_slot(owner, idx, true);
    BarExt* e = &g_bar_ext[s - H.bar];
    e->actor = actor, e->bound = actor != ENTITY_NONE, e->name_hash = fnv(x->text);
    s->shown = s->trail = s->gain = -1.0f; // (starts where the new actor is)
    return true;
}

// --- Play ---
void hud_begin_play(World* w) {
    H.w = w;
    hud_clear_markers(), hud_clear_messages();
    memset(H.hit, 0, sizeof H.hit), memset(H.bar, 0, sizeof H.bar), memset(g_bar_ext, 0, sizeof g_bar_ext);
    for (int i = 0; i < HUD_HITS_MAX; i++) H.hit[i].age = 99.0f;
    H.next_hit = 0, H.serial = 0, H.next_id = 0;
    H.spread = H.spread_target = 0.0f;
    H.visible = true, H.photo = false, H.seq_hidden = false, H.shown = true;
    H.player = H.layout_e = H.layout_widget = ENTITY_NONE;
    H.has_layout = false;
    memset(&H.st, 0, sizeof H.st);
}

void hud_after_begin_play(World* w, Entity player) {
    H.w = w, H.player = player;
    // the HUD Layout: the player's first, else the level's first
    Entity best = ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_HUD_LAYOUT)) || !a->count) continue;
        const Entity* es = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!best || es[i] == player) best = es[i];
    }
    if (!best) return;
    CHudLayout* c = ecs_get(w, best, COMP_HUD_LAYOUT);
    if (!c || !(c->flags & HUD_LAYOUT_VISIBLE)) return;
    BpAsset* cls = bp_find_hash(c->widget ? c->widget : hud_layout_asset());
    if (!cls || !cls->ui || !(cls->compiled || bp_compile(cls))) {
        printf("[RealEngine] HUD Layout: its widget %s could not be made\n", cls ? cls->name : "(missing)");
        return;
    }
    Entity we = ui_create_widget(w, cls);
    if (!we) return;
    ui_add_to_viewport(we, c->zorder);
    H.layout_e = best, H.layout_widget = we, H.layout = *c, H.has_layout = true;
    c->widget_entity = we;
}

void hud_tick(World* w, float dt, Entity player) {
    double t0 = re_time_seconds();
    H.w = w, H.player = player, H.dt = dt, H.frame++;
    // the layout's settings now (a Blueprint may change the component)
    const CHudLayout* c = H.layout_e && ecs_alive(w, H.layout_e) ? ecs_get(w, H.layout_e, COMP_HUD_LAYOUT) : NULL;
    if (c) H.layout = *c;
    float sp[3];
    float yaw, pitch, fov;
    H.seq_hidden = seq_view(w, sp, &yaw, &pitch, &fov);
    bool hide = !H.visible || (H.photo && (!H.has_layout || (H.layout.flags & HUD_LAYOUT_HIDE_PHOTO))) ||
                (H.seq_hidden && (!H.has_layout || (H.layout.flags & HUD_LAYOUT_HIDE_SEQUENCE)));
    H.shown = !hide;
    if (H.layout_widget) ui_set_live_hidden(H.layout_widget, hide);
    // the markers where their actors are (gone: removed)
    uint32_t nm = 0;
    for (uint32_t i = 0; i < HUD_MARKERS_MAX; i++) {
        HudMarker* m = &H.mk[i];
        if (!m->id) continue;
        if (m->actor && !ecs_alive(w, m->actor)) {
            memset(m, 0, sizeof *m);
            continue;
        }
        marker_place(w, m);
        if (H.view.set) { // (its distance from the view: the labels, the Debug Viewer)
            float d[3] = { m->pos[0] - H.view.pos[0], m->pos[1] - H.view.pos[1], m->pos[2] - H.view.pos[2] };
            m->dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
        nm++;
    }
    messages_tick(dt);
    for (int i = 0; i < HUD_HITS_MAX; i++) H.hit[i].age += dt;
    // the spread: Set Crosshair Spread's, opened by the player's speed (the HUD Layout's Speed Opens Crosshair)
    float target = H.spread_target;
    const CCharacter* ch = player && ecs_alive(w, player) ? ecs_get(w, player, COMP_CHARACTER) : NULL;
    if (ch && (!H.has_layout || (H.layout.flags & HUD_LAYOUT_SPEED_SPREAD))) {
        float v = sqrtf(ch->velocity[0] * ch->velocity[0] + ch->velocity[1] * ch->velocity[1]);
        float vmax = ch->max_walk_speed > 1.0f ? ch->max_walk_speed : 600.0f;
        target = fmaxf(target, fminf(v / vmax, 1.0f) * 0.55f + (fabsf(ch->velocity[2]) > 50.0f ? 0.3f : 0.0f));
    }
    target = fminf(target, 1.0f);
    float k = 1.0f - expf(-dt / (target > H.spread ? 0.05f : 0.18f)); // (opens fast, closes slowly)
    H.spread += (target - H.spread) * k;
    // the stats
    uint32_t nmsg = 0, queued = 0, hits = 0, bars = 0, nums = 0;
    for (uint32_t i = 0; i < HUD_MSG_MAX; i++)
        if (H.msg[i].text[0]) { if (H.msg[i].age < 0.0f) queued++; else nmsg++; }
    for (int i = 0; i < HUD_HITS_MAX; i++) hits += H.hit[i].age < HIT_LIFE;
    for (int i = 0; i < HUD_BARS_MAX; i++) bars += H.bar[i].used && H.frame - H.bar[i].frame < 3;
    uint32_t hn = 0;
    const HealthNumber* num = health_numbers(&hn);
    for (uint32_t i = 0; i < hn; i++) nums += num[i].amount > 0.0f && num[i].age <= HEALTH_NUMBER_LIFE;
    H.st.markers = nm, H.st.messages = nmsg, H.st.queued = queued, H.st.hits = hits, H.st.bars = bars, H.st.numbers = nums;
    H.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

void hud_end_play(void) {
    hud_clear_markers(), hud_clear_messages();
    H.layout_widget = H.layout_e = H.player = ENTITY_NONE;
    H.has_layout = false, H.photo = false, H.visible = true, H.shown = true;
    H.w = NULL;
}
Entity hud_player(void) { return H.player; }
Entity hud_layout_widget(void) { return H.layout_widget; }

// --- The view ---
void hud_set_view(const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg) {
    memcpy(H.view.pos, pos, 12), memcpy(H.view.fwd, fwd, 12), memcpy(H.view.right, right, 12), memcpy(H.view.up, up, 12);
    H.view.fov_deg = fov_deg, H.view.set = true;
}
const HudView* hud_view(void) { return &H.view; }

bool hud_project(const float p[3], float x0, float y0, float x1, float y1, float xy[2], float* depth) {
    const HudView* v = &H.view;
    float d[3] = { p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2] };
    float qz = d[0] * v->fwd[0] + d[1] * v->fwd[1] + d[2] * v->fwd[2];
    float qx = d[0] * v->right[0] + d[1] * v->right[1] + d[2] * v->right[2], qy = d[0] * v->up[0] + d[1] * v->up[1] + d[2] * v->up[2];
    float focal = (x1 - x0) * 0.5f / tanf(fmaxf(v->fov_deg, 1.0f) * 0.5f * 0.017453292f);
    float z = fabsf(qz) > 1.0f ? fabsf(qz) : 1.0f;
    xy[0] = (x0 + x1) * 0.5f + qx / z * focal * (qz < 0.0f ? -1.0f : 1.0f);
    xy[1] = (y0 + y1) * 0.5f - qy / z * focal * (qz < 0.0f ? -1.0f : 1.0f);
    if (qz < 0.0f) xy[0] = (x0 + x1) - xy[0], xy[1] = (y0 + y1) - xy[1]; // (behind: the direction it is in, from the middle)
    if (depth) *depth = qz;
    return qz > 1.0f;
}

// east: the view's right when facing north (+X); the engine's handedness read from the view (Unreal's: +Y)
static float east_sign(void) {
    const HudView* v = &H.view;
    if (!v->set) return 1.0f;
    float c = v->fwd[0] * v->right[1] - v->fwd[1] * v->right[0];
    return c < 0.0f ? -1.0f : 1.0f;
}
float hud_bearing(const float d[3]) {
    float a = atan2f(d[1] * east_sign(), d[0]) * 57.29578f;
    return a < 0.0f ? a + 360.0f : a;
}
const char* hud_cardinal(int i) {
    static const char* const t[3][8] = { { "N", "NE", "E", "SE", "S", "SW", "W", "NW" },
                                         { "N", "NE", "L", "SE", "S", "SO", "O", "NO" },
                                         { "N", "NE", "E", "SE", "S", "SO", "O", "NO" } };
    int l = dlg_language();
    return t[l >= 0 && l < 3 ? l : 0][i & 7];
}

// --- The layout's rectangle ---
bool hud_layout_rect(Entity live, float rect[4], float* k) {
    if (!live || live != H.layout_widget || !H.has_layout) return false;
    const CHudLayout* c = &H.layout;
    float w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (w <= 1.0f || h <= 1.0f) return false;
    float m[4] = { c->safe[0], c->safe[1], c->safe[2], c->safe[3] };
#ifdef __ANDROID__
    if (c->flags & HUD_LAYOUT_SAFE_AREA) { // (the notch and the rounded corners: the sides of a landscape phone)
        m[0] = fmaxf(m[0], 4.0f), m[2] = fmaxf(m[2], 4.0f), m[1] = fmaxf(m[1], 2.0f), m[3] = fmaxf(m[3], 3.0f);
    }
#endif
    rect[0] += w * m[0] * 0.01f, rect[2] -= w * m[2] * 0.01f, rect[1] += h * m[1] * 0.01f, rect[3] -= h * m[3] * 0.01f;
    float sw = rect[2] - rect[0], sh = rect[3] - rect[1];
    float kk = c->scale_mode == HUD_SCALE_SHORT_SIDE ? fminf(sw, sh) / UI_REF_HEIGHT
             : c->scale_mode == HUD_SCALE_FIT        ? fminf(sw / 1920.0f, sh / UI_REF_HEIGHT)
             : c->scale_mode == HUD_SCALE_FIXED      ? 1.0f
                                                     : sh / UI_REF_HEIGHT;
    kk *= c->scale > 0.0f ? c->scale : 1.0f;
    if (c->min_scale > 0.0f) kk = fmaxf(kk, c->min_scale);
    if (c->max_scale > 0.0f) kk = fminf(kk, c->max_scale);
    *k = kk;
    return true;
}

bool hud_piece_hidden(Entity live, uint8_t kind) {
    if (!H.shown) return true; // (Set HUD Visible, the photo mode, a Sequence's camera: every piece)
    if (!live || live != H.layout_widget || !H.has_layout) return false;
    uint32_t bit = kind == UIW_ATTR_BAR ? HUD_PIECE_BARS : kind == UIW_COMPASS ? HUD_PIECE_COMPASS : kind == UIW_MESSAGES ? HUD_PIECE_MESSAGES
                 : kind == UIW_DAMAGE_NUMBERS ? HUD_PIECE_NUMBERS : kind == UIW_CROSSHAIR ? HUD_PIECE_CROSSHAIR
                 : kind == UIW_SCREEN_MARKERS ? HUD_PIECE_MARKERS : kind == UIW_DAMAGE_DIR ? HUD_PIECE_DAMAGE_DIR
                 : kind == UIW_MINIMAP ? HUD_PIECE_MINIMAP : 0u;
    return bit && !(H.layout.pieces & bit);
}

void hud_numbers_drawn(void) { H.numbers_drawn = true; }
bool hud_numbers_take_drawn(void) {
    bool d = H.numbers_drawn;
    H.numbers_drawn = false;
    return d;
}
void hud_feed_config(uint32_t max_shown, float life) { // (the feed widget drawn: its settings for the queue)
    if (max_shown >= 1 && max_shown <= HUD_MSG_MAX) H.max_shown = max_shown;
    if (life > 0.05f) H.default_life = life;
}
World* hud_world(void) { return H.w; }
float hud_dt(void) { return H.dt; }

void hud_set_ui_gpu_ms(double ms) { H.st.ui_gpu_ms = ms; }
double hud_ui_gpu_ms(void) { return H.st.ui_gpu_ms; }
void hud_draw_account(uint8_t kind, double ms, uint32_t vtx, uint32_t idx) {
    H.draw_acc += ms, H.vtx_acc += vtx, H.idx_acc += idx, H.pieces_acc++;
    int k = kind >= UIW_ATTR_BAR && kind <= UIW_MINIMAP ? kind - UIW_ATTR_BAR : 3; // (the default numbers: the Damage Numbers')
    H.kind_acc[k] += ms, H.kind_vtx_acc[k] += vtx;
}
void hud_frame_begin(void) {
    if (H.pieces_acc || H.draw_acc > 0.0) {
        H.st.draw_ms = H.draw_acc, H.st.draw_vtx = H.vtx_acc, H.st.draw_idx = H.idx_acc, H.st.pieces = H.pieces_acc;
    } else H.st.draw_ms = 0.0, H.st.draw_vtx = H.st.draw_idx = H.st.pieces = 0;
    memcpy(H.st.kind_ms, H.kind_acc, sizeof H.kind_acc), memcpy(H.st.kind_vtx, H.kind_vtx_acc, sizeof H.kind_vtx_acc);
    H.draw_acc = 0.0, H.vtx_acc = H.idx_acc = H.pieces_acc = 0;
    memset(H.kind_acc, 0, sizeof H.kind_acc), memset(H.kind_vtx_acc, 0, sizeof H.kind_vtx_acc);
}

bool hud_is_kind(uint8_t kind) { return kind >= UIW_ATTR_BAR && kind <= UIW_MINIMAP; }

void hud_widget_defaults(UiWidget* w) {
    static const float white[4] = { 1, 1, 1, 1 }, dark[4] = { 0.02f, 0.02f, 0.03f, 0.55f };
    memcpy(w->color, white, 16), memcpy(w->bg, dark, 16);
    w->radius = 0.0f, w->border_w = 0.0f;
    switch (w->kind) {
    case UIW_ATTR_BAR: // a red health bar: the loss lingers light, the gain green
        w->color[0] = 0.86f, w->color[1] = 0.18f, w->color[2] = 0.16f;
        w->hover[0] = 1.0f, w->hover[1] = 0.86f, w->hover[2] = 0.62f, w->hover[3] = 0.95f;
        w->press[0] = 0.45f, w->press[1] = 1.0f, w->press[2] = 0.55f, w->press[3] = 0.9f;
        snprintf(w->hint, sizeof w->hint, "Health");
        w->value = 0.12f, w->vmin = 0.55f, w->vmax = 100.0f, w->vstep = 0.25f, w->padding = 0.8f;
        w->checked = 1, w->bold = 1, w->font_size = 18.0f, w->radius = 3.0f, w->percent = 0.75f;
        w->border_w = 1.0f, w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.15f;
        break;
    case UIW_COMPASS:
        w->value = 120.0f, w->checked = 1, w->font_size = 20.0f, w->bold = 1, w->radius = 4.0f;
        w->hover[0] = 1.0f, w->hover[1] = 0.82f, w->hover[2] = 0.25f, w->hover[3] = 1.0f;
        break;
    case UIW_MESSAGES:
        w->columns = 5, w->value = 3.0f, w->font_size = 24.0f, w->bold = 1, w->shadow = 1, w->align[0] = 1.0f, w->radius = 4.0f;
        w->bg[3] = 0.4f;
        break;
    case UIW_DAMAGE_NUMBERS: // damage yellow, heal green, critical orange, shield blue
        w->color[0] = 1.0f, w->color[1] = 0.86f, w->color[2] = 0.32f;
        w->hover[0] = 0.4f, w->hover[1] = 1.0f, w->hover[2] = 0.5f, w->hover[3] = 1.0f;
        w->press[0] = 1.0f, w->press[1] = 0.42f, w->press[2] = 0.12f, w->press[3] = 1.0f;
        w->bg[0] = 0.45f, w->bg[1] = 0.78f, w->bg[2] = 1.0f, w->bg[3] = 1.0f;
        w->font_size = 34.0f, w->value = 70.0f, w->bold = 1;
        w->anchor[0] = w->anchor[1] = 0.0f, w->anchor[2] = w->anchor[3] = 1.0f, w->pos[0] = w->pos[1] = w->size[0] = w->size[1] = 0.0f;
        break;
    case UIW_CROSSHAIR:
        w->value = 6.0f, w->vmin = 12.0f, w->vmax = 36.0f, w->border_w = 2.0f, w->checked = 1, w->bold = 1;
        w->anchor[0] = w->anchor[2] = 0.5f, w->anchor[1] = w->anchor[3] = 0.5f, w->align[0] = w->align[1] = 0.5f, w->pos[0] = w->pos[1] = 0.0f;
        w->size[0] = w->size[1] = 120.0f;
        break;
    case UIW_SCREEN_MARKERS:
        w->value = 48.0f, w->checked = 1, w->bold = 1, w->font_size = 18.0f;
        w->anchor[0] = w->anchor[1] = 0.0f, w->anchor[2] = w->anchor[3] = 1.0f, w->pos[0] = w->pos[1] = w->size[0] = w->size[1] = 0.0f;
        break;
    case UIW_DAMAGE_DIR:
        w->color[0] = 0.95f, w->color[1] = 0.12f, w->color[2] = 0.1f, w->color[3] = 0.85f;
        w->value = 1.2f, w->vmin = 170.0f, w->border_w = 10.0f;
        w->anchor[0] = w->anchor[2] = 0.5f, w->anchor[1] = w->anchor[3] = 0.5f, w->align[0] = w->align[1] = 0.5f, w->pos[0] = w->pos[1] = 0.0f;
        w->size[0] = w->size[1] = 440.0f;
        break;
    case UIW_MINIMAP:
        w->value = 3000.0f, w->checked = 1, w->rounded = 1, w->font_size = 16.0f;
        w->color[0] = 0.35f, w->color[1] = 0.75f, w->color[2] = 1.0f;
        w->hover[0] = 1.0f, w->hover[1] = 0.82f, w->hover[2] = 0.25f, w->hover[3] = 1.0f;
        w->press[0] = 1.0f, w->press[1] = 0.25f, w->press[2] = 0.2f, w->press[3] = 1.0f; // (the actors with Health)
        w->bg[3] = 0.6f, w->border_w = 2.0f, w->border_col[0] = w->border_col[1] = w->border_col[2] = 1.0f, w->border_col[3] = 0.3f;
        w->anchor[0] = w->anchor[2] = 1.0f, w->anchor[1] = w->anchor[3] = 0.0f, w->align[0] = 1.0f, w->align[1] = 0.0f;
        w->pos[0] = -30.0f, w->pos[1] = 30.0f, w->size[0] = w->size[1] = 220.0f;
        break;
    default: break;
    }
}

// --- WBP_GameHUD: every piece placed ---
uint32_t hud_layout_asset(void) {
    const char* vp = "/Content/UI/WBP_GameHUD.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_GameHUD", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    static const struct { uint8_t kind; const char* name; float ax, ay, x, y, w, h, alx, aly; } pc[] = {
        // (the full-screen layers first: under the rest)
        { UIW_SCREEN_MARKERS, "ScreenMarkers", -1, -1, 0, 0, 0, 0, 0, 0 },
        { UIW_DAMAGE_NUMBERS, "DamageNumbers", -1, -1, 0, 0, 0, 0, 0, 0 },
        { UIW_DAMAGE_DIR, "DamageDirection", 0.5f, 0.5f, 0, 0, 440, 440, 0.5f, 0.5f },
        { UIW_CROSSHAIR, "Crosshair", 0.5f, 0.5f, 0, 0, 120, 120, 0.5f, 0.5f },
        { UIW_COMPASS, "Compass", 0.5f, 0.0f, 0, 28, 720, 52, 0.5f, 0.0f },
        { UIW_MINIMAP, "Minimap", 1.0f, 0.0f, -32, 32, 220, 220, 1.0f, 0.0f },
        { UIW_MESSAGES, "MessageFeed", 1.0f, 0.0f, -32, 290, 460, 270, 1.0f, 0.0f },
        { UIW_ATTR_BAR, "HealthBar", 0.0f, 1.0f, 40, -64, 420, 26, 0.0f, 1.0f },
        { UIW_ATTR_BAR, "ShieldBar", 0.0f, 1.0f, 40, -96, 420, 12, 0.0f, 1.0f },
        { UIW_ATTR_BAR, "StaminaBar", 0.0f, 1.0f, 40, -40, 300, 8, 0.0f, 1.0f },
    };
    for (size_t i = 0; i < sizeof pc / sizeof pc[0]; i++) {
        int k = ui_tree_add(t, pc[i].kind, 0);
        if (k < 0) return 0;
        UiWidget* u = &t->w[k];
        snprintf(u->name, UI_NAME, "%s", pc[i].name);
        if (pc[i].ax >= 0.0f) {
            u->anchor[0] = u->anchor[2] = pc[i].ax, u->anchor[1] = u->anchor[3] = pc[i].ay;
            u->pos[0] = pc[i].x, u->pos[1] = pc[i].y, u->size[0] = pc[i].w, u->size[1] = pc[i].h;
            u->align[0] = pc[i].alx, u->align[1] = pc[i].aly;
        }
        if (!strcmp(pc[i].name, "ShieldBar")) {
            snprintf(u->hint, sizeof u->hint, "Shield");
            u->color[0] = 0.3f, u->color[1] = 0.65f, u->color[2] = 1.0f, u->checked = 0, u->vmin = 0.3f;
        } else if (!strcmp(pc[i].name, "StaminaBar")) {
            snprintf(u->hint, sizeof u->hint, "Stamina");
            u->color[0] = 0.95f, u->color[1] = 0.82f, u->color[2] = 0.25f, u->checked = 0, u->vmin = 0.0f, u->vstep = 0.0f, u->value = 0.08f;
        } else if (!strcmp(pc[i].name, "HealthBar")) u->columns = 10;
    }
    bp_compile(a);
    if (!bp_save(a)) return 0;
    return a->hash;
}

// --- The Debug Viewer ---
static void collect(DvList* l, const DvContext* ctx) {
    (void)ctx;
    l->gpu = false, l->estimated = false;
    l->ms = H.st.tick_ms + H.st.draw_ms;
    snprintf(l->note, sizeof l->note, "%u markers, %u messages (+%u waiting), %u damage numbers, %u bars, %u hits; drawn %u pieces, %u vertices, %u indices",
             H.st.markers, H.st.messages, H.st.queued, H.st.numbers, H.st.bars, H.st.hits, H.st.pieces, H.st.draw_vtx, H.st.draw_idx);
    char info[64];
    snprintf(info, sizeof info, "markers, messages, spread, hiding");
    dv_add(l, "Update", info, (float)H.st.tick_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u pieces, %u vertices; the UI pass %.3f ms on the GPU", H.st.pieces, H.st.draw_vtx, H.st.ui_gpu_ms);
    dv_add(l, "Draw (CPU)", info, (float)H.st.draw_ms, ENTITY_NONE);
    for (int k = 0; k < 8; k++)
        if (H.st.kind_ms[k] > 0.0) {
            snprintf(info, sizeof info, "%u vertices", H.st.kind_vtx[k]);
            dv_add(l, g_ui_kind_names[UIW_ATTR_BAR + k], info, (float)H.st.kind_ms[k], ENTITY_NONE);
        }
    float each = H.st.markers ? (float)(H.st.draw_ms * 0.5 / H.st.markers) : 0.0f; // (≈: half the drawing goes to the markers)
    for (uint32_t i = 0; i < HUD_MARKERS_MAX; i++) {
        const HudMarker* m = &H.mk[i];
        if (!m->id) continue;
        snprintf(info, sizeof info, "%s%s%s, %.0f m", (m->flags & HUD_MK_COMPASS) ? "compass" : "", (m->flags & HUD_MK_SCREEN) ? "screen" : "",
                 (m->flags & HUD_MK_OFFSCREEN) ? " + edge" : "", m->dist / 100.0f);
        dv_add(l, m->label[0] ? m->label : "Marker", info, each, m->actor);
    }
}
void hud_register_debug(void) { dv_register("HUD", collect); }

// --- The self-test ---
int hud_selftest(void) {
    int fails = 0;
#define HCHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: hud: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    World* w = calloc(1, sizeof(World));
    if (!w || !ecs_init(w)) return free(w), 1;
    hud_begin_play(w);
    // markers: one per actor and kind, replaced; removed by kind; a fixed place; full at 256
    Entity a = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM)), b = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    CTransform* ta = ecs_get(w, a, COMP_TRANSFORM);
    ta->location[0] = 1000.0f, ta->location[1] = 0.0f;
    snprintf(((CName*)ecs_get(w, a, COMP_NAME))->text, RE_NAME_MAX, "Goal");
    uint32_t m1 = hud_add_marker(w, a, NULL, "Goal", NULL, 50.0f, HUD_MK_COMPASS | HUD_MK_DISTANCE);
    uint32_t m2 = hud_add_marker(w, a, NULL, "Goal 2", NULL, 50.0f, HUD_MK_COMPASS);
    uint32_t m3 = hud_add_marker(w, a, NULL, "Over", NULL, 120.0f, HUD_MK_SCREEN | HUD_MK_OFFSCREEN);
    HCHECK(m1 && m1 == m2 && m3 && m3 != m1, "the same actor's compass marker is replaced, the screen one apart (%u %u %u)", m1, m2, m3);
    HCHECK(hud_marker_count(HUD_MK_COMPASS) == 1 && hud_marker_count(0) == 2, "markers %u compass, %u in all (1, 2)", hud_marker_count(HUD_MK_COMPASS), hud_marker_count(0));
    const HudMarker* mk = hud_markers(NULL);
    bool placed = false;
    for (int i = 0; i < HUD_MARKERS_MAX; i++)
        if (mk[i].id == m3) placed = fabsf(mk[i].pos[0] - 1000.0f) < 0.01f && fabsf(mk[i].pos[2] - 120.0f) < 0.01f;
    HCHECK(placed, "the screen marker is not over its actor (+120 cm)");
    uint32_t fixed = hud_add_marker(w, ENTITY_NONE, (float[3]){ 0, 500, 0 }, "Camp", (float[4]){ 0, 1, 0, 1 }, 0.0f, HUD_MK_COMPASS);
    HCHECK(fixed && hud_marker_count(HUD_MK_COMPASS) == 2, "a fixed place's marker");
    HCHECK(hud_remove_marker(a, HUD_MK_SCREEN) == 1 && hud_marker_count(0) == 2, "removing the screen marker only");
    HCHECK(hud_remove_marker_id(fixed) && hud_marker_count(0) == 1, "removing by id");
    uint32_t got = 0;
    for (int i = 0; i < 300; i++) got += hud_add_marker(w, ENTITY_NONE, (float[3]){ (float)i, 0, 0 }, "x", NULL, 0, HUD_MK_SCREEN) != 0;
    HCHECK(got == HUD_MARKERS_MAX - 1, "the markers fill at %d (%u added)", HUD_MARKERS_MAX, got);
    hud_clear_markers();
    // a destroyed actor's marker goes
    hud_add_marker(w, b, NULL, "B", NULL, 0, HUD_MK_COMPASS);
    ecs_destroy(w, b);
    hud_tick(w, 0.016f, ENTITY_NONE);
    HCHECK(hud_marker_count(0) == 0, "a destroyed actor's marker stays");
    // the feed: the queue (5 shown), "+N" summed, the same text stacked, the life
    hud_feed_config(5, 3.0f);
    for (int i = 0; i < 7; i++) {
        char s[32];
        snprintf(s, sizeof s, "Message %d", i);
        hud_message(s, 0.0f, NULL);
    }
    hud_tick(w, 0.016f, ENTITY_NONE);
    HCHECK(hud_message_count(true) == 5 && hud_message_count(false) == 7, "the feed shows %u of %u (5 of 7)", hud_message_count(true), hud_message_count(false));
    hud_message("+1 Coin", 0.0f, NULL), hud_message("+1 Coin", 0.0f, NULL), hud_message("+3 Coin", 0.0f, NULL);
    hud_message("Saved", 0.0f, NULL), hud_message("Saved", 0.0f, NULL);
    const HudMessage* ms = hud_messages(NULL);
    bool coin = false, saved = false;
    for (int i = 0; i < HUD_MSG_MAX; i++) coin |= !strcmp(ms[i].text, "+5 Coin"), saved |= !strcmp(ms[i].text, "Saved x2");
    HCHECK(coin, "\"+1 Coin\" twice and \"+3 Coin\" are not \"+5 Coin\"");
    HCHECK(saved, "\"Saved\" twice is not \"Saved x2\"");
    HCHECK(hud_message_count(false) == 9, "%u messages (9: the stacked ones are one)", hud_message_count(false));
    for (int i = 0; i < 400; i++) hud_tick(w, 0.05f, ENTITY_NONE); // (20 s: every one shown in turn and gone)
    HCHECK(hud_message_count(false) == 0, "%u messages left after 20 s", hud_message_count(false));
    HCHECK(H.st.shown_messages == 9, "%u messages shown (9)", H.st.shown_messages);
    // the bar: a loss follows quickly, the trail waits and drains; a gain jumps the gain's color
    Entity p = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_HEALTH));
    ecs_component_defaults(COMP_HEALTH, ecs_get(w, p, COMP_HEALTH));
    CHealth* hp = ecs_get(w, p, COMP_HEALTH);
    hp->max_health = hp->health = 100.0f, hp->max_shield = hp->shield = 0.0f, hp->flags &= ~HEALTH_NUMBERS;
    H.player = p;
    UiWidget bar;
    memset(&bar, 0, sizeof bar);
    bar.kind = UIW_ATTR_BAR;
    hud_widget_defaults(&bar);
    H.frame++;
    HudBarState* s = hud_bar_state(ENTITY_NONE, 1, &bar, 0.016f);
    HCHECK(s && fabsf(s->shown - 1.0f) < 1e-3f && fabsf(s->value - 100.0f) < 1e-3f, "the bar starts full (%.2f, %.0f)", s ? s->shown : -1, s ? s->value : -1);
    hp->health = 60.0f;
    for (int i = 0; i < 20; i++) H.frame++, s = hud_bar_state(ENTITY_NONE, 1, &bar, 0.016f); // 0.32 s
    HCHECK(fabsf(s->shown - 0.6f) < 0.03f && s->trail > 0.95f, "after 0.3 s the fill is at %.2f (0.6), the trail at %.2f (still 1)", s->shown, s->trail);
    for (int i = 0; i < 60; i++) H.frame++, s = hud_bar_state(ENTITY_NONE, 1, &bar, 0.016f); // 1.3 s
    HCHECK(fabsf(s->trail - 0.6f) < 0.02f, "after 1.3 s the trail is at %.2f (drained to 0.6)", s->trail);
    hp->health = 90.0f;
    H.frame++, s = hud_bar_state(ENTITY_NONE, 1, &bar, 0.016f);
    HCHECK(fabsf(s->gain - 0.9f) < 1e-3f && s->shown < 0.7f, "a heal: the gain at %.2f (0.9), the fill at %.2f catching up", s->gain, s->shown);
    // attributes: Shield, an unknown one, a registered source
    float v, mx;
    HCHECK(hud_attribute(w, p, "Health", &v, &mx) && fabsf(v - 90.0f) < 1e-3f && fabsf(mx - 100.0f) < 1e-3f, "the Health attribute");
    HCHECK(!hud_attribute(w, p, "Stamina", &v, &mx), "an unknown attribute found");
    // the bearing: north +X, east the view's right
    hud_set_view((float[3]){ 0, 0, 0 }, (float[3]){ 1, 0, 0 }, (float[3]){ 0, 1, 0 }, (float[3]){ 0, 0, 1 }, 90.0f);
    HCHECK(fabsf(hud_bearing((float[3]){ 1, 0, 0 })) < 0.01f && fabsf(hud_bearing((float[3]){ 0, 1, 0 }) - 90.0f) < 0.01f &&
           fabsf(hud_bearing((float[3]){ -1, 0, 0 }) - 180.0f) < 0.01f && fabsf(hud_bearing((float[3]){ 0, -1, 0 }) - 270.0f) < 0.01f,
           "the bearings of +X, +Y, -X, -Y are not 0, 90, 180, 270");
    // the projection: ahead in the middle, to the right on the right, behind flagged
    float xy[2], dz;
    HCHECK(hud_project((float[3]){ 500, 0, 0 }, 0, 0, 1920, 1080, xy, &dz) && fabsf(xy[0] - 960) < 0.5f && fabsf(xy[1] - 540) < 0.5f, "ahead is not the middle");
    HCHECK(hud_project((float[3]){ 500, 500, 0 }, 0, 0, 1920, 1080, xy, &dz) && fabsf(xy[0] - 1920) < 1.0f, "45 degrees right at 90 fov is not the edge (%.0f)", xy[0]);
    HCHECK(!hud_project((float[3]){ -500, 100, 0 }, 0, 0, 1920, 1080, xy, &dz) && xy[0] > 960, "behind and to the right is not flagged, pointing right (%.0f)", xy[0]);
    // the damage's direction: only the player's, from the causer
    Entity foe = ecs_create(w, COMP_BIT(COMP_TRANSFORM));
    ((CTransform*)ecs_get(w, foe, COMP_TRANSFORM))->location[1] = 800.0f;
    hud_on_damage(w, p, foe, ENTITY_NONE, 10.0f);
    hud_on_damage(w, foe, p, ENTITY_NONE, 10.0f);
    uint32_t nh = 0, live_hits = 0;
    const HudHit* hh = hud_hits(&nh);
    for (uint32_t i = 0; i < nh; i++) live_hits += hh[i].age < HIT_LIFE;
    HCHECK(live_hits == 1 && fabsf(hh[0].from[1] - 800.0f) < 1e-3f, "%u hits recorded (1, from the foe)", live_hits);
    // visibility: Set HUD Visible, the photo mode
    hud_set_visible(false), hud_tick(w, 0.016f, p);
    HCHECK(!hud_shown() && hud_piece_hidden(ENTITY_NONE, UIW_COMPASS), "Set HUD Visible false does not hide");
    hud_set_visible(true), hud_set_photo_mode(true), hud_tick(w, 0.016f, p);
    HCHECK(!hud_shown(), "the photo mode does not hide");
    hud_set_photo_mode(false), hud_tick(w, 0.016f, p);
    HCHECK(hud_shown(), "the HUD does not come back");
    // the spread eases toward Set Crosshair Spread's
    hud_set_spread(1.0f);
    for (int i = 0; i < 30; i++) hud_tick(w, 0.016f, p);
    HCHECK(hud_spread() > 0.95f, "the spread is at %.2f after 0.5 s (1)", hud_spread());
    hud_set_spread(0.0f);
    for (int i = 0; i < 90; i++) hud_tick(w, 0.016f, p);
    HCHECK(hud_spread() < 0.05f, "the spread closes to %.2f after 1.4 s (0)", hud_spread());
    // 100 markers and 300 messages a frame through the tick, timed
    for (int i = 0; i < 100; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM));
        ((CTransform*)ecs_get(w, e, COMP_TRANSFORM))->location[0] = (float)i * 30.0f;
        hud_add_marker(w, e, NULL, "M", NULL, 100, HUD_MK_SCREEN | HUD_MK_OFFSCREEN | HUD_MK_DISTANCE);
    }
    double t0 = re_time_seconds();
    for (int f = 0; f < 200; f++) hud_tick(w, 0.016f, p);
    double per = (re_time_seconds() - t0) * 1000.0 / 200.0;
    printf("[RealEngine] HUD self-test: tick with 100 markers %.4f ms a frame\n", per);
    HCHECK(per < 0.05, "the tick costs %.4f ms with 100 markers (0.05 at most)", per);
    hud_end_play();
    ecs_shutdown(w);
    free(w);
    return fails;
}
