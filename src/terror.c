// The horror kit (Q13): the Tension Director, the filter stack, the letterbox, the dips, the scare rigs, the lights'
// flicker and Rim Lights (terror.h, notas-dev/terror.md). The game thread's; nothing allocated.
#include "terror.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_internal.h"
#include "blueprint.h"
#include "components.h"
#include "debug_viewer.h"
#include "platform.h"
#include "post.h"
#include "render.h"

#define T_DEG2RAD 0.017453292f

static const char* const g_filter_names[TF_COUNT] = { "Hallucination", "Black & White Blown Out", "White Flash", "TV Static",
                                                      "VHS", "Chromatic Aberration", "Distortion" };
static const char* const g_rig_names[TR_COUNT] = { "Dolly Zoom", "Whip Pan", "Dutch Angle" };
static const char* const g_flicker_names[FLICKER_COUNT] = { "None", "Candle", "Faulty Bulb", "Strobe", "Lightning", "Pulse" };

// The channels' defaults: the value at tension 0, 1/3, 2/3, 1 (what the analysis measured: the grain and the vignette
// grow, the color drains, the bars close toward 2.2:1, the lens narrows, the heart beats, the lamps fail, the room's
// sound gets quieter and duller)
const TensionChannelInfo g_tension_channels[TENSION_CHANNELS] = {
    [TC_GRAIN] = { "Grain Curve", { 0.0f, 0.15f, 0.35f, 0.7f }, 0.0f, 2.0f },
    [TC_VIGNETTE] = { "Vignette Curve", { 0.0f, 0.1f, 0.25f, 0.45f }, 0.0f, 1.0f },
    [TC_SATURATION] = { "Saturation Curve", { 1.0f, 0.9f, 0.7f, 0.45f }, 0.0f, 2.0f },
    [TC_LETTERBOX] = { "Letterbox Curve", { 0.0f, 0.0f, 0.04f, 0.095f }, 0.0f, 0.3f },
    [TC_FOV] = { "FOV Curve", { 1.0f, 0.98f, 0.94f, 0.88f }, 0.5f, 1.5f },
    [TC_PULSE] = { "Pulse Curve", { 0.0f, 0.0f, 0.3f, 0.8f }, 0.0f, 1.0f },
    [TC_FLICKER] = { "Flicker Curve", { 0.0f, 0.1f, 0.4f, 0.9f }, 0.0f, 1.0f },
    [TC_VOLUME] = { "Volume Curve", { 1.0f, 0.85f, 0.6f, 0.35f }, 0.0f, 2.0f },
    [TC_LOWPASS] = { "Low-Pass Curve (Hz)", { 20000.0f, 12000.0f, 5000.0f, 1500.0f }, 200.0f, 20000.0f },
};

typedef struct Env { float peak, t, in, hold, out; bool on; } Env; // an event's envelope (a trigger, a dip)
static float env_value(const Env* e) {
    if (!e->on) return 0.0f;
    float t = e->t;
    if (t < e->in) return e->peak * (e->in > 0.0f ? t / e->in : 1.0f);
    t -= e->in;
    if (t < e->hold) return e->peak;
    t -= e->hold;
    return t < e->out ? e->peak * (1.0f - t / e->out) : 0.0f;
}
static void env_step(Env* e, float dt) {
    if (!e->on) return;
    e->t += dt;
    if (e->t >= e->in + e->hold + e->out) e->on = false;
}

#define FL_SLOTS 64 // the flickering lights with a sound: each one's last event (its strike, its pop) and a pending sound
typedef struct FlSlot { Entity e; long long last; float delay; uint32_t sound; float pos[3]; bool pending; } FlSlot;
#define RIM_SLOTS 32 // the Rim Lights' targets found by name (the last one, while it lives)
typedef struct RimSlot { Entity light, target; } RimSlot;

static struct {
    float tension, target, speed;       // speed < 0: the volume's rise / fall rates
    Env ev[TF_COUNT];
    float held[TF_COUNT], held_target[TF_COUNT], held_speed[TF_COUNT];
    float seq[TF_COUNT], seq_lb;
    float lb_aspect, lb_bar, lb_speed, view_aspect; // the letterbox: the asked aspect, the bar now
    Env dip;
    float dip_color[3];
    struct { int rig; float intensity, t, dur; bool on; } rig[4];
    double clock;
    bool frozen;
    float pulse_phase, pulse;
    float cam[3], cam_fwd[3];
    bool have_cam;
    uint32_t snd_class;                 // the Sound Class driven (its own volume and low-pass kept to put back)
    float snd_base_vol, snd_base_lpf;
    FlSlot fl[FL_SLOTS];
    RimSlot rim[RIM_SLOTS];
    TerrorStats stats;
    bool playing; // (Play's frames: the flicker runs; the editor shows each light as set)
} T = { .view_aspect = 16.0f / 9.0f };

const char* terror_filter_name(int f) { return f >= 0 && f < TF_COUNT ? g_filter_names[f] : "?"; }
const char* terror_rig_name(int r) { return r >= 0 && r < TR_COUNT ? g_rig_names[r] : "?"; }
const char* light_flicker_name(int f) { return f >= 0 && f < FLICKER_COUNT ? g_flicker_names[f] : "?"; }

// a name compared without case, spaces, '&' or '-' ("black & white blown out" = "BlackWhiteBlownOut")
static bool same_name(const char* a, const char* b) {
    for (;;) {
        while (*a == ' ' || *a == '&' || *a == '-' || *a == '_') a++;
        while (*b == ' ' || *b == '&' || *b == '-' || *b == '_') b++;
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
        a++, b++;
    }
}
int terror_filter_by_name(const char* s) {
    if (!s) return -1;
    for (int f = 0; f < TF_COUNT; f++)
        if (same_name(s, g_filter_names[f])) return f;
    static const struct { const char* n; int f; } alias[] = { { "BW", TF_BW_BLOWN }, { "Black & White", TF_BW_BLOWN }, { "Flash", TF_WHITE_FLASH },
                                                              { "Static", TF_TV_STATIC }, { "Chromatic", TF_CHROMATIC }, { "Alucinacao", TF_HALLUCINATION } };
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (same_name(s, alias[i].n)) return alias[i].f;
    return -1;
}
int terror_rig_by_name(const char* s) {
    if (!s) return -1;
    for (int r = 0; r < TR_COUNT; r++)
        if (same_name(s, g_rig_names[r])) return r;
    if (same_name(s, "Dutch") || same_name(s, "Dutch Tilt")) return TR_DUTCH;
    if (same_name(s, "Whip") || same_name(s, "Vertigo")) return same_name(s, "Whip") ? TR_WHIP_PAN : TR_DOLLY_ZOOM;
    return -1;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// --- the Tension Director ---
void terror_set_tension(float value, float blend_time) {
    T.target = clamp01(value);
    T.speed = blend_time < 0.0f ? -1.0f : blend_time <= 1e-4f ? 1e9f : fabsf(T.target - T.tension) / blend_time;
}
void terror_add_tension(float delta, float blend_time) { terror_set_tension(T.target + delta, blend_time); }
float terror_tension(void) { return T.tension; }
float terror_tension_target(void) { return T.target; }

// The curves in use: a volume's Horror group with its own (TERROR_PPV_TENSION), else the defaults
static const float (*curves(void))[4] {
    float w;
    const CPostProcess* v = post_terror_volume(&w);
    if (v && (v->terror_flags & TERROR_PPV_TENSION)) return (const float(*)[4])v->tension_curve;
    static float def[TENSION_CHANNELS][4];
    static bool made;
    if (!made) {
        for (int c = 0; c < TENSION_CHANNELS; c++) memcpy(def[c], g_tension_channels[c].def, sizeof def[c]);
        made = true;
    }
    return (const float(*)[4])def;
}
// through the four points (Catmull-Rom, the ends' tangents one-sided), held in the channel's range
float terror_tension_channel(int c) {
    if (c < 0 || c >= TENSION_CHANNELS) return 0.0f;
    const float* p = curves()[c];
    float x = clamp01(T.tension) * 3.0f;
    int i = x >= 3.0f ? 2 : (int)x;
    float t = x - (float)i;
    float p0 = i > 0 ? p[i - 1] : 2.0f * p[0] - p[1], p1 = p[i], p2 = p[i + 1], p3 = i + 2 < 4 ? p[i + 2] : 2.0f * p[3] - p[2];
    float v = 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
    const TensionChannelInfo* ci = &g_tension_channels[c];
    float lo = fminf(fminf(p[0], p[3]), fminf(p[1], p[2])), hi = fmaxf(fmaxf(p[0], p[3]), fmaxf(p[1], p[2])); // (no overshoot)
    v = fminf(fmaxf(v, lo), hi);
    return fminf(fmaxf(v, ci->lo), ci->hi);
}

// --- the filters ---
void terror_filter_trigger(int f, float intensity, float fade_in, float hold, float fade_out) {
    if (f < 0 || f >= TF_COUNT) return;
    Env* e = &T.ev[f];
    e->peak = clamp01(intensity), e->in = fmaxf(fade_in, 0.0f), e->hold = fmaxf(hold, 0.0f), e->out = fmaxf(fade_out, 0.0f);
    e->t = 0.0f, e->on = true;
}
void terror_filter_set(int f, float intensity, float fade) {
    if (f < 0 || f >= TF_COUNT) return;
    T.held_target[f] = clamp01(intensity);
    T.held_speed[f] = fade <= 1e-4f ? 1e9f : fmaxf(fabsf(T.held_target[f] - T.held[f]) / fade, 1e-4f);
}
void terror_filter_clear(float fade) {
    for (int f = 0; f < TF_COUNT; f++) {
        terror_filter_set(f, 0.0f, fade);
        if (T.ev[f].on) { // (an event under way: faded out from where it is)
            float v = env_value(&T.ev[f]);
            T.ev[f].peak = v, T.ev[f].in = 0.0f, T.ev[f].hold = 0.0f, T.ev[f].out = fmaxf(fade, 1e-3f), T.ev[f].t = 0.0f;
        }
    }
}
float terror_filter_value(int f) {
    if (f < 0 || f >= TF_COUNT) return 0.0f;
    return clamp01(fmaxf(fmaxf(env_value(&T.ev[f]), T.held[f]), T.seq[f]));
}
void terror_seq_filter(int f, float intensity) {
    if (f >= 0 && f < TF_COUNT) T.seq[f] = clamp01(intensity);
}
void terror_seq_letterbox(float amount) { T.seq_lb = clamp01(amount); }
void terror_seq_clear(void) { memset(T.seq, 0, sizeof T.seq), T.seq_lb = 0.0f; }

// --- the letterbox, the dips, the rigs ---
void terror_set_letterbox(float aspect, float blend_time) {
    T.lb_aspect = aspect > 0.5f ? aspect : 0.0f;
    float target = T.lb_aspect > T.view_aspect ? 0.5f * (1.0f - T.view_aspect / T.lb_aspect) : 0.0f;
    T.lb_speed = blend_time <= 1e-4f ? 1e9f : fmaxf(fabsf(target - T.lb_bar) / blend_time, 1e-4f);
}
float terror_letterbox(void) {
    float seq = T.seq_lb > 0.0f && T.view_aspect < 2.39f ? T.seq_lb * 0.5f * (1.0f - T.view_aspect / 2.39f) : 0.0f; // (the Sequencer's: x the 2.39:1 bars)
    return fmaxf(fmaxf(T.lb_bar, seq), T.tension > 0.0f ? terror_tension_channel(TC_LETTERBOX) : 0.0f);
}
void terror_dip(const float color[3], float fade_in, float hold, float fade_out) {
    for (int k = 0; k < 3; k++) T.dip_color[k] = color ? clamp01(color[k]) : 0.0f;
    T.dip.peak = 1.0f, T.dip.in = fmaxf(fade_in, 0.0f), T.dip.hold = fmaxf(hold, 0.0f), T.dip.out = fmaxf(fade_out, 0.0f);
    T.dip.t = 0.0f, T.dip.on = true;
}
void terror_scare_rig(int rig, float intensity, float duration) {
    if (rig < 0 || rig >= TR_COUNT) return;
    int slot = 0;
    for (int i = 0; i < 4; i++)
        if (!T.rig[i].on || T.rig[i].rig == rig) {
            slot = i;
            break;
        }
    T.rig[slot].rig = rig, T.rig[slot].intensity = intensity, T.rig[slot].t = 0.0f;
    T.rig[slot].dur = duration > 0.05f ? duration : rig == TR_WHIP_PAN ? 0.45f : rig == TR_DOLLY_ZOOM ? 2.5f : 3.0f;
    T.rig[slot].on = true;
}
void terror_set_time_frozen(bool frozen, double t) {
    T.frozen = frozen;
    if (frozen) T.clock = t;
}
double terror_time(void) { return T.clock; }

void terror_reset(void) {
    if (T.snd_class) { // the Sound Class as it was
        AuAsset* c = au_asset(T.snd_class);
        if (c && c->kind == AUA_CLASS) c->u.cls.volume = T.snd_base_vol, c->u.cls.lpf_hz = T.snd_base_lpf;
    }
    bool frozen = T.frozen;
    double clock = T.clock;
    memset(&T, 0, sizeof T);
    T.view_aspect = 16.0f / 9.0f;
    T.frozen = frozen, T.clock = frozen ? clock : 0.0;
}

// --- the lights' flicker (deterministic: the clock and the light's entity, no state) ---
static uint32_t hash_u(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
    return x;
}
static float hash_f(uint32_t a, uint32_t b) { return (float)(hash_u(a * 0x9E3779B9u ^ hash_u(b)) & 0xFFFFFFu) * (1.0f / 16777216.0f); }
static float vnoise(uint32_t seed, double t) { // a smooth 1D value noise in 0..1
    double fl = floor(t);
    float f = (float)(t - fl), s = f * f * (3.0f - 2.0f * f);
    uint32_t i = (uint32_t)(long long)fl;
    return hash_f(seed, i) * (1.0f - s) + hash_f(seed, i + 1u) * s;
}
static uint32_t light_seed(Entity e) { return hash_u((uint32_t)e * 2654435761u + 0x51u); }

// The lightning: in each slot of 7 s / speed a strike with 65% chance, at a random time in it: two or three flashes
// over ~0.45 s (the main one, its returns), then the dark. *event: the slot's index of the last strike started (-1)
static float lightning(uint32_t seed, double t, long long* event) {
    double slot_len = 7.0;
    long long s = (long long)floor(t / slot_len);
    float best = 0.0f;
    if (event) *event = -1;
    for (long long k = s - 1; k <= s; k++) { // (a strike late in the last slot may still be flashing)
        if (hash_f(seed ^ 0xA5u, (uint32_t)k) > 0.65f) continue;
        double t0 = ((double)k + 0.1 + 0.6 * hash_f(seed ^ 0x3Cu, (uint32_t)k)) * slot_len, d = t - t0;
        if (d < 0.0) continue;
        if (event) *event = k;
        int n = 2 + (hash_f(seed ^ 0x77u, (uint32_t)k) > 0.5f);
        for (int j = 0; j < n; j++) {
            double tj = j * (0.12 + 0.08 * hash_f(seed ^ (uint32_t)j, (uint32_t)k)), dj = d - tj;
            if (dj < 0.0) continue;
            float amp = j == 0 ? 1.0f : 0.55f + 0.4f * hash_f(seed ^ 0x11u ^ (uint32_t)j, (uint32_t)k);
            best = fmaxf(best, amp * expf(-(float)dj * 14.0f));
        }
    }
    return best;
}
// The faulty bulb: on, then a burst (in 35% of 1.7 s slots) of 25 Hz stutter and drops; *event: the burst's slot
static float faulty(uint32_t seed, double t, long long* event) {
    double slot_len = 1.7;
    long long s = (long long)floor(t / slot_len);
    if (event) *event = -1;
    if (hash_f(seed ^ 0xB1u, (uint32_t)s) > 0.35f) return 1.0f;
    double t0 = ((double)s + 0.2 * hash_f(seed ^ 0x2Eu, (uint32_t)s)) * slot_len, len = 0.15 + 0.5 * hash_f(seed ^ 0x9u, (uint32_t)s);
    if (t < t0 || t > t0 + len) return 1.0f;
    if (event) *event = s;
    uint32_t step = (uint32_t)(long long)floor(t * 25.0);
    float r = hash_f(seed ^ 0x5Du, step);
    return r < 0.45f ? 0.0f : r < 0.7f ? 0.35f : 1.0f;
}

bool terror_light_animated(const CLight* l) { return l && (l->flicker != FLICKER_NONE || l->flicker_tension > 0.0f); }

static float flicker_at(const CLight* l, Entity e, double now, long long* event) {
    if (event) *event = -1;
    float amount = l->flicker_amount > 0.0f ? clamp01(l->flicker_amount) : 1.0f;
    double t = now * (l->flicker_speed > 0.0f ? (double)l->flicker_speed : 1.0);
    uint32_t seed = light_seed(e);
    float v = 1.0f;
    switch (l->flicker) {
    case FLICKER_CANDLE: { // a flame: slow sway, quick tremble, a rare gutter
        float n = 0.55f * vnoise(seed, t * 2.3) + 0.3f * vnoise(seed ^ 0x1Fu, t * 9.0) + 0.15f * vnoise(seed ^ 0x2Fu, t * 23.0);
        float gutter = vnoise(seed ^ 0x3Fu, t * 0.4) > 0.86f ? 0.5f : 1.0f;
        v = 1.0f - amount * 0.45f * (1.0f - n * gutter);
        break;
    }
    case FLICKER_FAULTY: v = 1.0f - amount * (1.0f - faulty(seed, t, event)); break;
    case FLICKER_STROBE: { // 8 flashes a second, each 15% of its period
        double ph = t * 8.0;
        if (event) *event = (long long)floor(ph);
        v = ph - floor(ph) < 0.15 ? 1.0f : 1.0f - amount;
        break;
    }
    case FLICKER_LIGHTNING: v = 1.0f - amount + amount * lightning(seed, t, event); break;
    case FLICKER_PULSE: v = 1.0f - amount * 0.5f * (1.0f - cosf((float)fmod(t * 0.5, 1.0) * 6.2831853f)); break;
    default: break;
    }
    // the Tension Director's Light Flicker: a faulty bulb's stutter as deep as its curve x this light's share
    if (l->flicker_tension > 0.0f && T.tension > 0.0f) {
        float depth = clamp01(terror_tension_channel(TC_FLICKER) * l->flicker_tension);
        if (depth > 0.0f) v *= 1.0f - depth * (1.0f - faulty(seed ^ 0xC0FFEEu, now * (0.6 + 1.4 * T.tension), NULL));
    }
    return v;
}
float terror_light_factor(const CLight* l, Entity e) {
    if (!T.playing || !terror_light_animated(l)) return 1.0f; // (the editor shows the light as it is set: a lightning's isn't dark)
    return flicker_at(l, e, T.clock, NULL);
}

// --- per frame ---
static void step_held(float dt) {
    for (int f = 0; f < TF_COUNT; f++) {
        float d = T.held_target[f] - T.held[f], s = T.held_speed[f] * dt;
        T.held[f] = fabsf(d) <= s ? T.held_target[f] : T.held[f] + (d > 0.0f ? s : -s);
        env_step(&T.ev[f], dt);
    }
}

static Entity find_named(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}

// A Rim Light behind its target as the camera sees it: away from the camera (level), up, to a side; aimed at its chest
static void place_rim(World* w, Entity le, CLight* l, CTransform* lt) {
    Entity target = ENTITY_NONE;
    if (l->rim == RIM_PLAYER) target = bp_player(w);
    else if (l->rim == RIM_ACTOR && l->rim_target[0]) {
        RimSlot* s = NULL;
        for (int i = 0; i < RIM_SLOTS && !s; i++)
            if (T.rim[i].light == le) s = &T.rim[i];
        for (int i = 0; i < RIM_SLOTS && !s; i++)
            if (!T.rim[i].light) s = &T.rim[i], s->light = le, s->target = ENTITY_NONE;
        if (s && (!s->target || !ecs_alive(w, s->target))) s->target = find_named(w, l->rim_target);
        target = s ? s->target : find_named(w, l->rim_target);
    }
    const CTransform* tt = target && ecs_alive(w, target) ? ecs_get(w, target, COMP_TRANSFORM) : NULL;
    if (!tt || !T.have_cam) return;
    float away[2] = { tt->location[0] - T.cam[0], tt->location[1] - T.cam[1] }, len = sqrtf(away[0] * away[0] + away[1] * away[1]);
    if (len < 1.0f) away[0] = T.cam_fwd[0], away[1] = T.cam_fwd[1], len = sqrtf(away[0] * away[0] + away[1] * away[1]) + 1e-6f;
    away[0] /= len, away[1] /= len;
    float dist = l->rim_distance > 0.0f ? l->rim_distance : 250.0f, h = l->rim_height != 0.0f ? l->rim_height : 120.0f;
    float side[2] = { -away[1], away[0] };
    float p[3] = { tt->location[0] + away[0] * dist + side[0] * l->rim_side, tt->location[1] + away[1] * dist + side[1] * l->rim_side,
                   tt->location[2] + h };
    float aim[3] = { tt->location[0] - p[0], tt->location[1] - p[1], tt->location[2] + 40.0f - p[2] };
    float al = sqrtf(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
    if (al < 1e-3f) return;
    memcpy(lt->location, p, sizeof p);
    lt->rotation[0] = asinf(fminf(fmaxf(aim[2] / al, -1.0f), 1.0f)) / T_DEG2RAD;
    lt->rotation[1] = atan2f(aim[1], aim[0]) / T_DEG2RAD;
    lt->rotation[2] = 0.0f;
    lt->flags = (lt->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE; // (compared every frame by the renderer: no touch)
}

static FlSlot* fl_slot(Entity e) {
    for (int i = 0; i < FL_SLOTS; i++)
        if (T.fl[i].e == e) return &T.fl[i];
    for (int i = 0; i < FL_SLOTS; i++)
        if (!T.fl[i].e) {
            memset(&T.fl[i], 0, sizeof T.fl[i]);
            T.fl[i].e = e, T.fl[i].last = -1;
            return &T.fl[i];
        }
    return NULL;
}

void terror_tick(World* w, float dt, bool playing) {
    if (!playing) {
        if (T.tension != 0.0f || T.target != 0.0f || T.lb_bar != 0.0f) terror_reset();
        return;
    }
    double t0 = re_time_seconds();
    T.playing = true;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    if (!T.frozen) T.clock += dt;
    // the tension toward its target
    float d = T.target - T.tension, speed = T.speed;
    if (speed < 0.0f) {
        float wv;
        const CPostProcess* v = post_terror_volume(&wv);
        float rise = v && v->tension_rise > 0.0f ? v->tension_rise : 0.5f, fall = v && v->tension_fall > 0.0f ? v->tension_fall : 0.25f;
        speed = d > 0.0f ? rise : fall;
    }
    T.tension = fabsf(d) <= speed * dt ? T.target : T.tension + (d > 0.0f ? speed : -speed) * dt;
    step_held(dt);
    env_step(&T.dip, dt);
    { // the letterbox's bar toward the asked aspect
        float target = T.lb_aspect > T.view_aspect ? 0.5f * (1.0f - T.view_aspect / T.lb_aspect) : 0.0f, e = target - T.lb_bar, s = T.lb_speed * dt;
        T.lb_bar = fabsf(e) <= s ? target : T.lb_bar + (e > 0.0f ? s : -s);
    }
    for (int i = 0; i < 4; i++)
        if (T.rig[i].on && (T.rig[i].t += dt) >= T.rig[i].dur) T.rig[i].on = false;
    // the heart: 60 bpm calm .. 150 at full tension, a beat's two bumps (lub-dub)
    float amp = T.tension > 0.0f ? terror_tension_channel(TC_PULSE) : 0.0f;
    T.pulse_phase += dt * (60.0f + 90.0f * T.tension) / 60.0f;
    T.pulse_phase -= floorf(T.pulse_phase);
    float a = (T.pulse_phase) * 12.0f, b = (T.pulse_phase - 0.22f) * 14.0f;
    T.pulse = amp * fmaxf(expf(-a * a), 0.6f * expf(-b * b));
    // the tension's sound: a Sound Class's volume and low-pass, a MetaSound input on the playing Audio Components
    float wv;
    const CPostProcess* vol = post_terror_volume(&wv);
    uint32_t cls = vol && vol->tension_sound_class[0] ? audio_asset_by_name(AUA_CLASS, vol->tension_sound_class) : 0u;
    if (cls != T.snd_class) {
        if (T.snd_class) {
            AuAsset* c = au_asset(T.snd_class);
            if (c && c->kind == AUA_CLASS) c->u.cls.volume = T.snd_base_vol, c->u.cls.lpf_hz = T.snd_base_lpf;
        }
        T.snd_class = cls;
        AuAsset* c = cls ? au_asset(cls) : NULL;
        if (c && c->kind == AUA_CLASS) T.snd_base_vol = c->u.cls.volume, T.snd_base_lpf = c->u.cls.lpf_hz > 0.0f ? c->u.cls.lpf_hz : 20000.0f;
        else T.snd_class = 0;
    }
    if (T.snd_class) {
        AuAsset* c = au_asset(T.snd_class);
        if (c && c->kind == AUA_CLASS)
            c->u.cls.volume = T.snd_base_vol * terror_tension_channel(TC_VOLUME), c->u.cls.lpf_hz = fminf(T.snd_base_lpf, terror_tension_channel(TC_LOWPASS));
    }
    uint32_t nflick = 0, nrim = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (vol && vol->tension_param[0] && (ar->mask & COMP_BIT(COMP_AUDIO))) {
            const CAudio* au = ecs_arch_column(ar, COMP_AUDIO);
            for (uint32_t i = 0; i < ar->count; i++)
                if (au[i].handle) audio_set_param(au[i].handle, vol->tension_param, T.tension);
        }
        if (!(ar->mask & COMP_BIT(COMP_LIGHT)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CLight* ls = ecs_arch_column(ar, COMP_LIGHT);
        CTransform* ts = ecs_arch_column(ar, COMP_TRANSFORM);
        const Entity* es = ecs_arch_entities(ar);
        for (uint32_t i = 0; i < ar->count; i++) {
            CLight* l = &ls[i];
            if (l->rim) place_rim(w, es[i], l, &ts[i]), nrim++;
            if (!terror_light_animated(l)) continue;
            nflick++;
            if (!l->flicker_sound) continue;
            long long ev;
            flicker_at(l, es[i], T.clock, &ev);
            FlSlot* s = fl_slot(es[i]);
            if (!s) continue;
            if (ev >= 0 && ev != s->last) { // a new strike / pop / flash: its sound now, or after its delay (the thunder)
                s->last = ev, s->pending = true, s->delay = fmaxf(l->flicker_sound_delay, 0.0f), s->sound = l->flicker_sound;
                memcpy(s->pos, ts[i].location, sizeof s->pos);
            }
        }
    }
    for (int i = 0; i < FL_SLOTS; i++) {
        FlSlot* s = &T.fl[i];
        if (!s->e || !s->pending || (s->delay -= dt) > 0.0f) continue;
        s->pending = false;
        audio_play_at(s->sound, s->pos, 1.0f, 1.0f);
        T.stats.sounds++;
    }
    uint32_t on = 0;
    for (int f = 0; f < TF_COUNT; f++) on += terror_filter_value(f) > 0.0f;
    T.stats.tension = T.tension, T.stats.letterbox = terror_letterbox(), T.stats.dip = env_value(&T.dip);
    T.stats.filters_on = on, T.stats.flicker_lights = nflick, T.stats.rim_lights = nrim;
    T.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
}

const TerrorStats* terror_stats(void) { return &T.stats; }

// The Debug Viewer's list: the kit's frame on the CPU (the tension, the fades, the sounds, the Rim Lights moved), what is on
static void collect(DvList* l, const DvContext* c) {
    (void)c;
    char info[48];
    l->ms = T.stats.cpu_ms;
    snprintf(l->note, sizeof l->note, "tension %.2f -> %.2f, %u filters on, letterbox %.1f%%, dip %.2f; the filters' GPU time is in the composite's",
             T.tension, T.target, T.stats.filters_on, terror_letterbox() * 100.0f, T.stats.dip);
    for (int f = 0; f < TF_COUNT; f++) {
        float v = terror_filter_value(f);
        if (v <= 0.0f) continue;
        snprintf(info, sizeof info, "%.0f%%", v * 100.0f);
        dv_add(l, g_filter_names[f], info, 0.0f, ENTITY_NONE);
    }
    snprintf(info, sizeof info, "%u lights, %u sounds", T.stats.flicker_lights, T.stats.sounds);
    dv_add(l, "Light Flicker", info, 0.0f, ENTITY_NONE);
    snprintf(info, sizeof info, "%u lights", T.stats.rim_lights);
    dv_add(l, "Rim Lights", info, 0.0f, ENTITY_NONE);
}
void terror_register_debug(void) { dv_register("Horror Kit", collect); }

// a vector turned about a unit axis by an angle (Rodrigues)
static void turn(float v[3], const float k[3], float ang) {
    float c = cosf(ang), s = sinf(ang), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float x[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + k[i] * d * (1.0f - c);
}
static float ease(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

void terror_view(World* w, RenderView* rv, bool playing) {
    (void)w;
    memset(rv->terror, 0, sizeof rv->terror);
    if (rv->w > 1.0f && rv->h > 1.0f) T.view_aspect = rv->w / rv->h;
    float vw;
    const CPostProcess* vol = post_terror_volume(&vw);
    bool expo = vol && (vol->terror_flags & TERROR_PPV_EXPOSURE) && vw > 0.0f;
    if (expo) { // the horror exposure: the lit part metered, the EV floor, the toe
        rv->terror[5][0] = 1.0f;
        rv->terror[5][1] = vol->terror_meter > 0.0f ? fminf(fmaxf(vol->terror_meter, 0.5f), 0.995f) : 0.95f;
        rv->terror[3][2] = clamp01(vol->terror_toe) * vw;
        rv->terror_ev_floor = vol->terror_ev_floor;
    }
    if (playing) {
        memcpy(T.cam, rv->pos, sizeof T.cam), memcpy(T.cam_fwd, rv->fwd, sizeof T.cam_fwd), T.have_cam = true;
        // the tension's lens and the rigs on the player's view
        float fov_k = T.tension > 0.0f ? terror_tension_channel(TC_FOV) : 1.0f;
        for (int i = 0; i < 4; i++) {
            if (!T.rig[i].on) continue;
            float x = T.rig[i].t / T.rig[i].dur, k = T.rig[i].intensity;
            if (T.rig[i].rig == TR_DOLLY_ZOOM) { // the lens widens while the camera pulls in: the subject (3 m) the same size, the room stretching behind
                float s = x < 0.75f ? ease(x / 0.75f) : 1.0f - ease((x - 0.75f) / 0.25f);
                float f0 = rv->fov_deg * fov_k, f1 = fminf(f0 * (1.0f + 0.6f * k * s), 150.0f);
                float d0 = 300.0f, d1 = d0 * tanf(f0 * 0.5f * T_DEG2RAD) / tanf(f1 * 0.5f * T_DEG2RAD);
                for (int c = 0; c < 3; c++) rv->pos[c] += rv->fwd[c] * (d0 - d1);
                fov_k *= f1 / fmaxf(f0, 1.0f);
            } else if (T.rig[i].rig == TR_WHIP_PAN) { // a snap aside and back (the camera's motion blur smears it)
                float s = x < 0.3f ? ease(x / 0.3f) : 1.0f - ease((x - 0.3f) / 0.7f);
                const float z[3] = { 0, 0, 1 };
                float ang = 55.0f * k * s * T_DEG2RAD;
                turn(rv->fwd, z, ang), turn(rv->right, z, ang), turn(rv->up, z, ang);
            } else { // the Dutch angle: the horizon tilted, held, let go
                float s = x < 0.25f ? ease(x / 0.25f) : x < 0.75f ? 1.0f : 1.0f - ease((x - 0.75f) / 0.25f);
                float ang = 18.0f * k * s * T_DEG2RAD;
                turn(rv->right, rv->fwd, ang), turn(rv->up, rv->fwd, ang);
            }
        }
        rv->fov_deg = fminf(fmaxf(rv->fov_deg * fov_k, 5.0f), 160.0f);
    }
    for (int f = 0; f < TF_COUNT; f++) rv->terror[f / 4][f % 4] = terror_filter_value(f);
    rv->terror[1][3] = terror_letterbox();
    if (T.tension > 0.0f) {
        rv->terror[2][0] = terror_tension_channel(TC_GRAIN);
        rv->terror[2][1] = terror_tension_channel(TC_VIGNETTE);
        rv->terror[2][2] = terror_tension_channel(TC_SATURATION);
    } else
        rv->terror[2][2] = 1.0f;
    rv->terror[2][3] = T.pulse;
    rv->terror[3][0] = (float)fmod(T.clock, 1000.0);
    rv->terror[3][1] = (float)fmod(floor(T.clock * 24.0), 65536.0); // (the noise's step: 24 a second, film's)
    float dip = env_value(&T.dip);
    rv->terror[4][0] = T.dip_color[0], rv->terror[4][1] = T.dip_color[1], rv->terror[4][2] = T.dip_color[2], rv->terror[4][3] = dip;
    rv->terror[6][0] = 1.0f, rv->terror[6][1] = 0.06f, rv->terror[6][2] = 0.04f, rv->terror[6][3] = 4.0f; // (the hallucination's red, 4 levels)
    bool any = rv->terror[1][3] > 0.0f || dip > 0.0f || rv->terror[2][0] > 0.0f || rv->terror[2][1] > 0.0f || rv->terror[2][2] != 1.0f ||
               rv->terror[2][3] > 0.0f || rv->terror[3][2] > 0.0f;
    for (int f = 0; f < TF_COUNT; f++) any |= rv->terror[f / 4][f % 4] > 0.0f;
    rv->terror[3][3] = any ? 1.0f : 0.0f;
}
