// Audio in the world (audio_world.h; notas-dev/audio.md): Unreal's Audio Components (an Ambient Sound actor, a
// Blueprint's component) played when the game starts and moving with their entity; the listener (the player's camera);
// Audio Volumes (their reverb where the listener is, their inside / outside levels); occlusion by the level's collision
// boxes and landscapes (a ray from the sound to the listener).
#include "audio_world.h"

#include <math.h>
#include <string.h>

#include "audio_internal.h"
#include "bp_internal.h"
#include "components.h"
#include <stdio.h>
#include "metasound.h"
#include "vehicle.h"
#include "water.h"
#include <stdlib.h>
#include "blueprint.h"

#define DEG2RAD 0.017453292519943295f
#define MAX_VOLUMES 64

typedef struct Vol {
    float inv[3][3], center[3], half[3]; // the box: world -> its axes (rows), its center, its half extents
    const CAudioVolume* v;
    Entity e;
} Vol;

static struct {
    World* w;
    Vol vols[MAX_VOLUMES];
    uint32_t nvols;
    int listener_vol;          // the listener's volume (-1: none)
    float lpos[3], last_lpos[3];
    bool have_last, playing;
} AW;

// Unreal's rotator (pitch about Y, yaw about Z, roll about X; degrees) as the box's axes
static void axes_of(const float rot[3], float m[3][3]) {
    float sp = sinf(rot[0] * DEG2RAD), cp = cosf(rot[0] * DEG2RAD);
    float sy = sinf(rot[1] * DEG2RAD), cy = cosf(rot[1] * DEG2RAD);
    float sr = sinf(rot[2] * DEG2RAD), cr = cosf(rot[2] * DEG2RAD);
    // rows: forward (X), right (Y), up (Z) in world space
    m[0][0] = cp * cy, m[0][1] = cp * sy, m[0][2] = sp;
    m[1][0] = sr * sp * cy - cr * sy, m[1][1] = sr * sp * sy + cr * cy, m[1][2] = -sr * cp;
    m[2][0] = -(cr * sp * cy + sr * sy), m[2][1] = cy * sr - cr * sp * sy, m[2][2] = cr * cp;
}

static bool vol_inside(const Vol* v, const float p[3]) {
    float d[3] = { p[0] - v->center[0], p[1] - v->center[1], p[2] - v->center[2] };
    for (int k = 0; k < 3; k++) {
        float l = v->inv[k][0] * d[0] + v->inv[k][1] * d[1] + v->inv[k][2] * d[2];
        if (fabsf(l) > v->half[k]) return false;
    }
    return true;
}
static int vol_at(const float p[3]) { // the highest-priority enabled volume holding p
    int best = -1;
    for (uint32_t i = 0; i < AW.nvols; i++)
        if (vol_inside(&AW.vols[i], p) && (best < 0 || AW.vols[i].v->priority > AW.vols[best].v->priority)) best = (int)i;
    return best;
}

// --- The mixer's hooks ---
static bool hook_pos(uint32_t entity, int bone, float pos[3]) {
    (void)bone;
    if (!AW.w || !ecs_alive(AW.w, (Entity)entity)) return false;
    const CTransform* t = ecs_get(AW.w, (Entity)entity, COMP_TRANSFORM);
    if (!t) return false;
    memcpy(pos, t->location, sizeof t->location);
    return true;
}
static float hook_occlusion(const float from[3], const float to[3]) {
    if (!AW.playing) return 0.0f;
    float f = bp_trace_boxes(from, to, 0.0f);
    return f < 0.98f ? 1.0f : 0.0f;
}
static void hook_zone(const float src[3], float* gain, float* lpf) {
    if (!AW.nvols) return;
    int sv = vol_at(src), lv = AW.listener_vol;
    if (sv == lv) return;
    if (lv >= 0) { // inside a volume, the sound outside it: its exterior settings
        const CAudioVolume* v = AW.vols[lv].v;
        *gain *= v->exterior_volume;
        if (v->exterior_lpf > 0.0f) *lpf = fminf(*lpf, v->exterior_lpf);
    }
    if (sv >= 0) { // the sound inside a volume the listener isn't in: its interior settings
        const CAudioVolume* v = AW.vols[sv].v;
        *gain *= v->interior_volume;
        if (v->interior_lpf > 0.0f) *lpf = fminf(*lpf, v->interior_lpf);
    }
}

static void gather_volumes(World* w) {
    AW.nvols = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_AUDIO_VOLUME)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CAudioVolume* v = ecs_arch_column(a, COMP_AUDIO_VOLUME);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && AW.nvols < MAX_VOLUMES; i++) {
            if (!v[i].enabled) continue;
            Vol* o = &AW.vols[AW.nvols++];
            axes_of(t[i].rotation, o->inv);
            memcpy(o->center, t[i].location, 12);
            for (int k = 0; k < 3; k++) o->half[k] = fabsf(t[i].scale[k]) * 100.0f;
            o->v = &v[i], o->e = ents[i];
        }
    }
}

static AudioHandle play_component(World* w, Entity e, CAudio* c, float start_time) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    AudioPlay p;
    memset(&p, 0, sizeof p);
    p.sound = c->sound, p.volume = c->volume, p.pitch = c->pitch, p.fade_in = c->fade_in;
    p.start_time = start_time >= 0.0f ? start_time : c->start_time;
    p.spatial = c->spatialize != 0, p.follow = e, p.bone = -1;
    if (t) memcpy(p.pos, t->location, 12);
    p.attenuation = c->attenuation, p.sound_class = c->sound_class, p.concurrency = c->concurrency;
    p.owner = at ? at->owner : e, p.ui = c->ui != 0;
    return audio_play(&p);
}

void audio_world_begin_play(World* w) {
    AW.w = w, AW.playing = true, AW.have_last = false;
    AudioWorldHooks h = { hook_pos, hook_occlusion, hook_zone };
    audio_set_world_hooks(&h);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_AUDIO))) continue;
        CAudio* c = ecs_arch_column(a, COMP_AUDIO);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            c[i].handle = 0;
            if (c[i].auto_activate && c[i].sound) c[i].handle = play_component(w, ents[i], &c[i], -1.0f);
        }
    }
}

// --- B8: the world's own sounds (MetaSounds): engines, rain, wind ---
#define AUTO_MAX 32
enum { AUTO_ENGINE = 0, AUTO_RAIN, AUTO_WIND, AUTO_THUNDER };
typedef struct AutoSnd { uint32_t e; AudioHandle h; uint8_t kind, seen; uint32_t sound; } AutoSnd;
static AutoSnd g_auto[AUTO_MAX];
static uint32_t g_nauto;
static AudioAutoStats g_auto_st;
const AudioAutoStats* audio_auto_stats(void) { return &g_auto_st; }
static int auto_slot(uint32_t e, int kind) {
    for (uint32_t i = 0; i < g_nauto; i++)
        if (g_auto[i].e == e && g_auto[i].kind == kind) return (int)i;
    if (g_nauto == AUTO_MAX) return -1;
    g_auto[g_nauto] = (AutoSnd){ e, 0, (uint8_t)kind, 0, 0 };
    return (int)g_nauto++;
}
static void auto_play(int k, uint32_t sound, bool spatial, const float pos[3], uint32_t owner, const char* p, float v, const char* p2, float v2) {
    if (k < 0 || !sound) return;
    g_auto[k].seen = 1;
    if (!g_auto[k].h || !audio_is_playing(g_auto[k].h)) {
        AudioPlay ap = { 0 };
        ap.sound = sound, ap.volume = 1.0f, ap.pitch = 1.0f, ap.fade_in = 0.4f, ap.spatial = spatial, ap.owner = owner;
        if (spatial) memcpy(ap.pos, pos, 12), ap.follow = owner;
        g_auto[k].h = audio_play(&ap);
        g_auto[k].sound = sound;
    } else if (spatial) audio_set_pos(g_auto[k].h, pos);
    if (p) audio_set_param(g_auto[k].h, p, v);
    if (p2) audio_set_param(g_auto[k].h, p2, v2);
}
static bool has_audio(World* w, Entity e) { // (an actor with its own Audio Component, or one of its components: its sound is its own)
    if (ecs_mask(w, e) & COMP_BIT(COMP_AUDIO)) return true;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_AUDIO)) || !(a->mask & COMP_BIT(COMP_ATTACH)) || !a->count) continue;
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        for (uint32_t i = 0; i < a->count; i++)
            if (at[i].owner == e) return true;
    }
    return false;
}
// (2026-11-14) thunder (the Rain's Thunder): strikes at random (about every 12 s at 1), each a flash of lightning
// (water_strike_lightning) and, after the light's distance in sound (0.4 to 5 s), a rumble: a crack, then rolling away
// over 3 to 7 s with a second swell; its MetaSound's Rumble driven here a frame at a time
static struct { float next, wait, t, len, peak; bool rolling; uint32_t seed; } g_thunder;
static float g_aw_dt = 1.0f / 60.0f; // (this update's dt: audio_world_update)
static float audio_world_dt(void) { return g_aw_dt; }
static float th_rand(void) { g_thunder.seed = g_thunder.seed * 1664525u + 1013904223u; return (float)(g_thunder.seed >> 8) / 16777216.0f; }
static void thunder_tick(World* w, uint32_t e, const CRain* r) {
    (void)w;
    float dt = audio_world_dt();
    if (!g_thunder.seed) g_thunder.seed = 0x9E3779B9u, g_thunder.next = 3.0f + 6.0f * th_rand();
    if (r->thunder <= 0.0f || r->intensity <= 0.05f || r->snow) { g_thunder.rolling = false; return; }
    int k = auto_slot(e, AUTO_THUNDER);
    float level = 0.0f;
    if (g_thunder.rolling) {
        if (g_thunder.wait > 0.0f) g_thunder.wait -= dt; // (the sound on its way)
        else {
            g_thunder.t += dt;
            float x = g_thunder.t / g_thunder.len;
            if (x >= 1.0f) g_thunder.rolling = false;
            else {
                float att = fminf(g_thunder.t / 0.08f, 1.0f), roll = expf(-3.2f * x) + 0.45f * expf(-powf((x - 0.35f) * 6.0f, 2.0f));
                level = g_thunder.peak * att * roll * (0.85f + 0.15f * sinf(g_thunder.t * 9.0f));
            }
        }
    } else if ((g_thunder.next -= dt) <= 0.0f) { // a strike: the flash now, its sound by its distance
        float d = th_rand(); // (0 near: loud, sharp, at once; 1 far: soft, long, late)
        water_strike_lightning((1.0f - 0.7f * d) * (0.6f + 0.4f * th_rand()));
        g_thunder.rolling = true, g_thunder.t = 0.0f, g_thunder.wait = 0.4f + 4.6f * d;
        g_thunder.len = 3.0f + 4.0f * d, g_thunder.peak = 1.0f - 0.6f * d;
        g_thunder.next = (6.0f + 18.0f * th_rand()) / fmaxf(r->thunder, 0.05f);
    }
    if (level > 0.001f || (k >= 0 && g_auto[k].h)) {
        auto_play(k, ms_thunder_sound(), false, NULL, e, "Rumble", level, "Volume", fminf(fmaxf(r->thunder_volume, 0.0f), 2.0f));
    }
}
static void auto_sounds(World* w) {
    static int off = -1;
    if (off < 0) off = getenv("REALENGINE_NO_AUTO_SOUNDS") != NULL;
    if (off) return;
    for (uint32_t i = 0; i < g_nauto; i++) g_auto[i].seen = 0;
    memset(&g_auto_st, 0, sizeof g_auto_st);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count) continue;
        const Entity* ents = ecs_arch_entities(a);
        if (a->mask & COMP_BIT(COMP_VEHICLE))
            for (uint32_t i = 0; i < a->count; i++) {
                VehInfo vi;
                if (!vehicle_info(w, ents[i], &vi) || !vi.engine_on || has_audio(w, ents[i])) continue;
                auto_play(auto_slot(ents[i], AUTO_ENGINE), ms_engine_sound(), true, vi.pos, ents[i], "RPM", vi.rpm, "Load", vi.throttle);
                g_auto_st.engines++, g_auto_st.rpm = vi.rpm, g_auto_st.load = vi.throttle;
            }
        if (a->mask & COMP_BIT(COMP_RAIN)) {
            const CRain* r = ecs_arch_column(a, COMP_RAIN);
            const CTransform* t = (a->mask & COMP_BIT(COMP_TRANSFORM)) ? ecs_arch_column(a, COMP_TRANSFORM) : NULL;
            for (uint32_t i = 0; i < a->count; i++) {
                if (t && (t[i].flags & TRANSFORM_HIDDEN_GAME)) continue;
                if (has_audio(w, ents[i])) continue;
                if (r[i].intensity > 0.05f && r[i].sound_volume > 0.0f) {
                    // (2026-11-14) the style (what it falls on), the volume, muffled with a roof over the camera, or a sound of
                    // the project's own (looped: its volume follows the intensity)
                    float shel = water_rain_shelter() * fminf(fmaxf(r[i].muffle, 0.0f), 1.0f), vol = fminf(fmaxf(r[i].sound_volume, 0.0f), 2.0f);
                    int k = auto_slot(ents[i], AUTO_RAIN);
                    if (r[i].custom_sound) {
                        if (k >= 0 && g_auto[k].h && g_auto[k].sound != r[i].custom_sound) audio_stop(g_auto[k].h, 0.3f), g_auto[k].h = 0;
                        auto_play(k, r[i].custom_sound, false, NULL, ents[i], NULL, 0.0f, NULL, 0.0f);
                        if (k >= 0 && g_auto[k].h) audio_set_volume(g_auto[k].h, vol * fminf(r[i].intensity / 30.0f + 0.2f, 1.5f) * (1.0f - 0.6f * shel));
                    } else {
                        static const float st[RAIN_SOUND_COUNT][4] = { // tone (Hz), drop pitch (Hz), drop rate (x), drop level
                            { 7000.0f, 2600.0f, 1.0f, 1.0f }, { 4500.0f, 1700.0f, 1.6f, 1.3f }, { 9000.0f, 4300.0f, 2.2f, 1.8f },
                            { 5500.0f, 3300.0f, 0.8f, 0.7f }, { 3000.0f, 900.0f, 1.4f, 1.6f },  { 2200.0f, 1500.0f, 0.4f, 0.15f } };
                        const float* q = st[r[i].sound_style < RAIN_SOUND_COUNT ? r[i].sound_style : 0];
                        uint32_t snd = ms_rain_styled_sound();
                        if (k >= 0 && g_auto[k].h && g_auto[k].sound != snd) audio_stop(g_auto[k].h, 0.3f), g_auto[k].h = 0;
                        auto_play(k, snd, false, NULL, ents[i], "Intensity", r[i].intensity, "Tone", q[0]);
                        if (k >= 0 && g_auto[k].h) {
                            audio_set_param(g_auto[k].h, "Drop Pitch", q[1]), audio_set_param(g_auto[k].h, "Drop Rate", q[2]);
                            audio_set_param(g_auto[k].h, "Drop Level", q[3]), audio_set_param(g_auto[k].h, "Muffle", shel);
                            audio_set_param(g_auto[k].h, "Volume", 0.8f * vol);
                        }
                    }
                    g_auto_st.rain++, g_auto_st.intensity = r[i].intensity;
                }
                thunder_tick(w, ents[i], &r[i]);
                if (r[i].wind_speed > 1.0f) {
                    auto_play(auto_slot(ents[i], AUTO_WIND), ms_wind_sound(), false, NULL, ents[i], "Speed", r[i].wind_speed, NULL, 0.0f);
                    g_auto_st.wind++, g_auto_st.wind_speed = r[i].wind_speed;
                }
            }
        }
    }
    for (uint32_t i = 0; i < g_nauto;) { // (gone, stopped or quiet: faded out, its slot freed)
        if (!g_auto[i].seen) {
            if (g_auto[i].h) audio_stop(g_auto[i].h, 0.4f);
            g_auto[i] = g_auto[--g_nauto];
        } else i++;
    }
}

void audio_world_end_play(World* w) {
    (void)w;
    g_nauto = 0; // (B8: their sounds stop with the rest)
    AW.playing = false;
    audio_stop_all(true); // (the game's sounds; the editor's previews go on)
    audio_clear_mixes();
    au_set_reverb(NULL, 0.0f);
    audio_pause_game(false);
}

void audio_world_update(World* w, float dt, bool playing, const float pos[3], const float fwd[3], const float up[3]) {
    AW.w = w;
    g_aw_dt = dt > 0.0f && dt < 0.25f ? dt : 1.0f / 60.0f;
    if (!AW.playing && playing) audio_world_begin_play(w); // (the game's own start: game_main)
    float vel[3] = { 0, 0, 0 };
    if (AW.have_last && dt > 1e-4f)
        for (int k = 0; k < 3; k++) vel[k] = (pos[k] - AW.last_lpos[k]) / dt;
    if (AW.have_last && (vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) > 4e8f) memset(vel, 0, sizeof vel); // (a teleport)
    memcpy(AW.last_lpos, pos, 12), AW.have_last = true;
    audio_set_listener(pos, fwd, up, vel);
    if (playing) {
        gather_volumes(w);
        AW.listener_vol = vol_at(pos);
        // the reverb where the listener is (blended in over the volume's fade time); none: the engine's room
        if (AW.listener_vol >= 0 && AW.vols[AW.listener_vol].v->reverb_on) {
            const CAudioVolume* v = AW.vols[AW.listener_vol].v;
            AuEffect e;
            au_effect_defaults(&e, AUFX_REVERB);
            memcpy(e.p, v->reverb, sizeof e.p);
            au_set_reverb(&e, v->fade_time);
        } else au_set_reverb(NULL, 1.0f);
        // the components' handles: forgotten once their sound ended (Is Playing reads false); Unreal's On Audio
        // Finished: the owner's Custom Events "OnAudioFinished" and "OnAudioFinished_<component's name>"
        Entity done[64], owner[64];
        uint32_t ndone = 0;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_AUDIO))) continue;
            CAudio* c = ecs_arch_column(a, COMP_AUDIO);
            const Entity* ents = ecs_arch_entities(a);
            const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
            for (uint32_t i = 0; i < a->count; i++)
                if (c[i].handle && !audio_is_playing(c[i].handle)) {
                    c[i].handle = 0;
                    if (ndone < 64) done[ndone] = ents[i], owner[ndone++] = at ? at[i].owner : ents[i];
                }
        }
        for (uint32_t k = 0; k < ndone; k++) { // (after the walk: an event may add or remove components)
            const CName* nm = ecs_get(w, done[k], COMP_NAME);
            char ev[96];
            bp_call_event(w, owner[k], "OnAudioFinished");
            if (nm && nm->text[0]) {
                snprintf(ev, sizeof ev, "OnAudioFinished_%s", nm->text);
                bp_call_event(w, owner[k], ev);
            }
        }
        auto_sounds(w); // (B8: the engines, the rain, the wind)
    } else AW.nvols = 0, AW.listener_vol = -1;
    audio_update(dt);
}

// --- The components' calls (Blueprints) ---
static CAudio* comp_of(World* w, Entity e) {
    CAudio* c = ecs_alive(w, e) ? ecs_get(w, e, COMP_AUDIO) : NULL;
    if (c) return c;
    // an actor: its first Audio Component
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_AUDIO)) || !(a->mask & COMP_BIT(COMP_ATTACH))) continue;
        CAudio* cs = ecs_arch_column(a, COMP_AUDIO);
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        for (uint32_t i = 0; i < a->count; i++)
            if (at[i].owner == e) return &cs[i];
    }
    return NULL;
}
static Entity comp_entity(World* w, Entity e) {
    if (ecs_alive(w, e) && ecs_get(w, e, COMP_AUDIO)) return e;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_AUDIO)) || !(a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (at[i].owner == e) return ents[i];
    }
    return ENTITY_NONE;
}

void audio_component_play(World* w, Entity e, float start_time) {
    CAudio* c = comp_of(w, e);
    if (!c) return;
    if (c->handle) audio_stop(c->handle, 0.0f);
    c->handle = play_component(w, comp_entity(w, e), c, start_time);
}
void audio_component_stop(World* w, Entity e, float fade_out) {
    CAudio* c = comp_of(w, e);
    if (c && c->handle) audio_stop(c->handle, fade_out), c->handle = 0;
}
void audio_component_fade_in(World* w, Entity e, float seconds, float volume, float start_time) {
    CAudio* c = comp_of(w, e);
    if (!c) return;
    float f = c->fade_in;
    c->fade_in = seconds;
    audio_component_play(w, e, start_time);
    c->fade_in = f;
    if (c->handle) audio_fade(c->handle, seconds, volume);
}
void audio_component_set_paused(World* w, Entity e, bool paused) {
    CAudio* c = comp_of(w, e);
    if (c && c->handle) audio_set_paused(c->handle, paused);
}
void audio_component_set_volume(World* w, Entity e, float v) {
    CAudio* c = comp_of(w, e);
    if (!c) return;
    c->volume = v;
    if (c->handle) audio_set_volume(c->handle, v);
}
void audio_component_set_pitch(World* w, Entity e, float p) {
    CAudio* c = comp_of(w, e);
    if (!c) return;
    c->pitch = p;
    if (c->handle) audio_set_pitch(c->handle, p);
}
void audio_component_set_sound(World* w, Entity e, uint32_t sound) {
    CAudio* c = comp_of(w, e);
    if (!c) return;
    bool was = c->handle && audio_is_playing(c->handle);
    c->sound = sound;
    if (was) audio_component_play(w, e, 0.0f); // (Unreal's Set Sound: a playing component plays the new one)
}
bool audio_component_is_playing(World* w, Entity e) {
    CAudio* c = comp_of(w, e);
    return c && c->handle && audio_is_playing(c->handle);
}
bool audio_component_set_param(World* w, Entity e, const char* name, float value) {
    CAudio* c = comp_of(w, e);
    return c && c->handle && audio_set_param(c->handle, name, value);
}

AudioHandle audio_component_handle(World* w, Entity e) {
    CAudio* c = comp_of(w, e);
    return c ? c->handle : 0;
}
