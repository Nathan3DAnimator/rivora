// Camera Shakes (camshake.h; notas-dev/camera-primeira-pessoa.md): .rshake JSON files read once into a small table by
// their path's hash, read again when the file's time changed; the built-in presets; the oscillation (sine, Perlin noise,
// curves) as a pure function of the time and a seed.
#include "camshake.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

const char* const g_camshake_wave_names[CSW_COUNT] = { "Sine", "Perlin Noise", "Curve" };
const char* const g_camshake_channel_names[CS_CHANNELS] = { "Location X (forward)", "Location Y (right)", "Location Z (up)",
                                                            "Pitch", "Yaw", "Roll", "Field of View" };

// --- the oscillation ---

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return x;
}
static float hash_unit(uint32_t x) { return (float)(mix32(x) & 0xFFFFFFu) / 16777215.0f; } // 0..1

// 1D gradient (Perlin) noise, -1..1 about: a random slope at each whole step, a quintic fade between
static float perlin1(float x, uint32_t seed) {
    float fl = floorf(x), f = x - fl;
    uint32_t i = (uint32_t)(int32_t)fl;
    float g0 = hash_unit(seed ^ (i * 0x9E3779B1u)) * 2.0f - 1.0f, g1 = hash_unit(seed ^ ((i + 1u) * 0x9E3779B1u)) * 2.0f - 1.0f;
    float u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float a = g0 * f, b = g1 * (f - 1.0f);
    return (a + (b - a) * u) * 2.0f;
}

// A curve's value at t: linear between its keys, looping over its last key's time
static float curve_at(const CamShakeChannel* c, float t) {
    uint32_t n = c->nkeys < CS_MAX_KEYS ? c->nkeys : CS_MAX_KEYS;
    if (!n) return 0.0f;
    float period = c->keys[n - 1][0];
    if (period > 1e-4f && t > period) t = fmodf(t, period);
    if (t <= c->keys[0][0]) return c->keys[0][1];
    for (uint32_t k = 1; k < n; k++)
        if (t <= c->keys[k][0]) {
            float t0 = c->keys[k - 1][0], t1 = c->keys[k][0], s = t1 > t0 ? (t - t0) / (t1 - t0) : 1.0f;
            return c->keys[k - 1][1] + (c->keys[k][1] - c->keys[k - 1][1]) * s;
        }
    return c->keys[n - 1][1];
}

void camshake_sample(const CamShake* s, float t, uint32_t seed, float out[CS_CHANNELS]) {
    for (int c = 0; c < CS_CHANNELS; c++) {
        const CamShakeChannel* ch = &s->ch[c];
        float v = 0.0f;
        if (ch->amplitude != 0.0f) {
            uint32_t cs = mix32(seed + (uint32_t)c * 7919u + 1u);
            switch (ch->wave) {
            case CSW_PERLIN: v = perlin1(t * fmaxf(ch->frequency, 0.0f) + hash_unit(cs) * 64.0f, cs); break;
            case CSW_CURVE: v = curve_at(ch, t); break;
            default: v = sinf(6.28318531f * fmaxf(ch->frequency, 0.0f) * t + (ch->random_offset ? hash_unit(cs) * 6.28318531f : 0.0f)); break;
            }
            v *= ch->amplitude;
        }
        out[c] = v;
    }
}

static float smooth01(float x) { return x <= 0.0f ? 0.0f : x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x); }

float camshake_envelope(const CamShake* s, float t, float stop_t) {
    float e = s->blend_in > 1e-4f ? smooth01(t / s->blend_in) : 1.0f;
    if (s->duration > 0.0f) e *= s->blend_out > 1e-4f ? smooth01((s->duration - t) / s->blend_out) : (t < s->duration ? 1.0f : 0.0f);
    if (stop_t >= 0.0f) e *= s->blend_out > 1e-4f ? 1.0f - smooth01((t - stop_t) / s->blend_out) : 0.0f;
    return e;
}

bool camshake_finished(const CamShake* s, float t, float stop_t) {
    if (s->duration > 0.0f && t >= s->duration) return true;
    return stop_t >= 0.0f && t >= stop_t + fmaxf(s->blend_out, 0.0f);
}

void camshake_defaults(CamShake* s) {
    memset(s, 0, sizeof *s);
    s->duration = 0.5f, s->blend_in = 0.05f, s->blend_out = 0.2f, s->scale = 1.0f;
    for (int c = 0; c < CS_CHANNELS; c++) s->ch[c].frequency = 10.0f, s->ch[c].wave = CSW_PERLIN;
    s->ch[CS_PITCH].amplitude = 1.0f, s->ch[CS_YAW].amplitude = 1.0f;
}

// --- the presets ---

CamShakePreset g_camshake_presets[CAMSHAKE_PRESETS];
static void ch(CamShake* s, int c, uint32_t wave, float amp, float hz) { s->ch[c].wave = wave, s->ch[c].amplitude = amp, s->ch[c].frequency = hz; }
static void keys(CamShake* s, int c, float amp, const float (*k)[2], uint32_t n) {
    s->ch[c].wave = CSW_CURVE, s->ch[c].amplitude = amp, s->ch[c].nkeys = n;
    memcpy(s->ch[c].keys, k, sizeof(float) * 2 * n);
}
const CamShakePreset* camshake_presets(void) {
    static bool made;
    if (made) return g_camshake_presets;
    made = true;
    CamShake* s;
    // Explosion: a hard, short jolt (noise in every channel) and a quick FOV punch
    s = &g_camshake_presets[0].s, g_camshake_presets[0].name = "Explosion";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 1.0f, s->blend_in = 0.02f, s->blend_out = 0.7f;
    ch(s, CS_LOC_X, CSW_PERLIN, 5, 22), ch(s, CS_LOC_Y, CSW_PERLIN, 7, 24), ch(s, CS_LOC_Z, CSW_PERLIN, 9, 20);
    ch(s, CS_PITCH, CSW_PERLIN, 2.5f, 17), ch(s, CS_YAW, CSW_PERLIN, 1.6f, 17), ch(s, CS_ROLL, CSW_PERLIN, 3.0f, 14);
    keys(s, CS_FOV, 4.0f, (const float[][2]){ { 0, 0 }, { 0.05f, 1 }, { 0.4f, 0 } }, 3);
    // Earthquake: long, low, the ground rolling
    s = &g_camshake_presets[1].s, g_camshake_presets[1].name = "Earthquake";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 6.0f, s->blend_in = 1.0f, s->blend_out = 2.0f;
    ch(s, CS_LOC_X, CSW_PERLIN, 2, 6), ch(s, CS_LOC_Y, CSW_PERLIN, 3, 7), ch(s, CS_LOC_Z, CSW_PERLIN, 3, 8);
    ch(s, CS_PITCH, CSW_PERLIN, 0.6f, 5), ch(s, CS_YAW, CSW_PERLIN, 0.4f, 4), ch(s, CS_ROLL, CSW_PERLIN, 0.9f, 4);
    // Handheld: a held camera's slow drift (until stopped)
    s = &g_camshake_presets[2].s, g_camshake_presets[2].name = "Handheld";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.0f, s->blend_in = 0.5f, s->blend_out = 0.5f;
    ch(s, CS_LOC_Y, CSW_PERLIN, 0.4f, 0.5f), ch(s, CS_LOC_Z, CSW_PERLIN, 0.4f, 0.6f);
    ch(s, CS_PITCH, CSW_PERLIN, 0.35f, 0.6f), ch(s, CS_YAW, CSW_PERLIN, 0.4f, 0.5f), ch(s, CS_ROLL, CSW_PERLIN, 0.25f, 0.4f);
    // Hit: a blow taken (the head snapped back, a twist)
    s = &g_camshake_presets[3].s, g_camshake_presets[3].name = "Hit";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.35f, s->blend_in = 0.0f, s->blend_out = 0.25f;
    keys(s, CS_PITCH, 3.0f, (const float[][2]){ { 0, 0 }, { 0.04f, 1 }, { 0.35f, 0 } }, 3);
    keys(s, CS_LOC_X, 3.0f, (const float[][2]){ { 0, 0 }, { 0.04f, -1 }, { 0.35f, 0 } }, 3);
    ch(s, CS_ROLL, CSW_PERLIN, 2.0f, 18);
    // Gunshot: the kick of a shot (back and up, a buzz)
    s = &g_camshake_presets[4].s, g_camshake_presets[4].name = "Gunshot";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.18f, s->blend_in = 0.0f, s->blend_out = 0.12f;
    keys(s, CS_LOC_X, 2.0f, (const float[][2]){ { 0, 0 }, { 0.02f, -1 }, { 0.18f, 0 } }, 3);
    keys(s, CS_PITCH, 1.2f, (const float[][2]){ { 0, 0 }, { 0.03f, 1 }, { 0.18f, 0 } }, 3);
    ch(s, CS_ROLL, CSW_PERLIN, 0.4f, 30);
    // Footstep: a heavy step felt (a stomp nearby)
    s = &g_camshake_presets[5].s, g_camshake_presets[5].name = "Footstep";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.25f, s->blend_in = 0.02f, s->blend_out = 0.2f;
    ch(s, CS_LOC_Z, CSW_SINE, 0.8f, 6), ch(s, CS_PITCH, CSW_SINE, 0.3f, 6);
    // Heartbeat: the view pulsing at 70 beats a minute (fear), until stopped
    s = &g_camshake_presets[6].s, g_camshake_presets[6].name = "Heartbeat";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.0f, s->blend_in = 0.4f, s->blend_out = 0.8f;
    keys(s, CS_FOV, 1.5f, (const float[][2]){ { 0, 0 }, { 0.07f, 1 }, { 0.16f, 0.15f }, { 0.25f, 0.7f }, { 0.4f, 0 }, { 0.857f, 0 } }, 6);
    keys(s, CS_PITCH, 0.15f, (const float[][2]){ { 0, 0 }, { 0.07f, 1 }, { 0.16f, 0 }, { 0.25f, 0.6f }, { 0.4f, 0 }, { 0.857f, 0 } }, 6);
    // Dread: a slow unsettling sway (horror), until stopped
    s = &g_camshake_presets[7].s, g_camshake_presets[7].name = "Dread";
    camshake_defaults(s), memset(s->ch, 0, sizeof s->ch);
    s->duration = 0.0f, s->blend_in = 2.0f, s->blend_out = 2.0f;
    ch(s, CS_PITCH, CSW_PERLIN, 0.5f, 0.2f), ch(s, CS_YAW, CSW_PERLIN, 0.6f, 0.18f), ch(s, CS_ROLL, CSW_PERLIN, 1.2f, 0.15f);
    ch(s, CS_FOV, CSW_PERLIN, 1.5f, 0.1f);
    return g_camshake_presets;
}

// --- the files ---

#define CS_MAX 128
typedef struct Entry {
    uint32_t hash;
    int64_t mtime;
    bool ok;
    CamShake s;
    char vpath[256];
} Entry;
static Entry g_cs[CS_MAX];
static uint32_t g_ncs;

uint32_t camshake_vpath_hash(const char* vpath) { // (the same as the materials' and Blueprints')
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) {
        char chr = *c == '\\' ? '/' : *c;
        h = (h ^ (uint8_t)tolower((unsigned char)chr)) * 16777619u;
    }
    return h ? h : 1u;
}

bool camshake_is_path(const char* vpath) {
    const char* d = vpath ? strrchr(vpath, '.') : NULL;
    return d && !strcmp(d, ".rshake");
}

static bool to_abs(const char* vpath, char* out, size_t cap) {
    const char* root = ui_content_root();
    if (!root || !root[0] || strncmp(vpath, "/Content", 8)) return false;
    snprintf(out, cap, "%s%s", root, vpath + 8);
    return true;
}

bool camshake_load(const char* vpath, CamShake* s) {
    camshake_defaults(s);
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL; // (a load, not a frame)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc doc;
    char err[160];
    if (!ok || !json_parse(t, (size_t)n, &doc, err, sizeof err)) {
        free(t);
        return false;
    }
    const JsonValue* r = &doc.root;
    s->duration = (float)json_get_num(r, "duration", s->duration);
    s->blend_in = (float)json_get_num(r, "blend_in", s->blend_in);
    s->blend_out = (float)json_get_num(r, "blend_out", s->blend_out);
    s->scale = (float)json_get_num(r, "scale", s->scale);
    s->single_instance = (uint32_t)json_get_int(r, "single_instance", 0);
    const JsonValue* chs = json_get(r, "channels");
    for (int c = 0; c < CS_CHANNELS; c++) {
        CamShakeChannel* k = &s->ch[c];
        const JsonValue* o = json_at(chs, (size_t)c);
        if (!o) {
            k->amplitude = 0.0f;
            continue;
        }
        k->amplitude = (float)json_get_num(o, "amplitude", 0.0);
        k->frequency = (float)json_get_num(o, "frequency", 10.0);
        k->wave = (uint32_t)json_get_int(o, "wave", CSW_PERLIN) % CSW_COUNT;
        k->random_offset = (uint32_t)json_get_int(o, "random_offset", 0);
        float kv[CS_MAX_KEYS * 2];
        int nk = json_get_floats(o, "keys", kv, CS_MAX_KEYS * 2);
        k->nkeys = nk > 0 ? (uint32_t)nk / 2u : 0u;
        for (uint32_t i = 0; i < k->nkeys; i++) k->keys[i][0] = kv[i * 2], k->keys[i][1] = kv[i * 2 + 1];
    }
    json_free(&doc);
    free(t);
    return true;
}

bool camshake_save(const char* vpath, const CamShake* s) {
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "type", "CameraShake");
    jw_kfloat(&w, "duration", s->duration), jw_kfloat(&w, "blend_in", s->blend_in), jw_kfloat(&w, "blend_out", s->blend_out);
    jw_kfloat(&w, "scale", s->scale), jw_kint(&w, "single_instance", s->single_instance);
    jw_key(&w, "channels");
    jw_arr_begin(&w);
    for (int c = 0; c < CS_CHANNELS; c++) {
        const CamShakeChannel* k = &s->ch[c];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", g_camshake_channel_names[c]); // (read by its place; the name for a reader)
        jw_kfloat(&w, "amplitude", k->amplitude), jw_kfloat(&w, "frequency", k->frequency);
        jw_kint(&w, "wave", k->wave), jw_kint(&w, "random_offset", k->random_offset);
        uint32_t nk = k->nkeys < CS_MAX_KEYS ? k->nkeys : CS_MAX_KEYS;
        if (nk) jw_kfloats(&w, "keys", &k->keys[0][0], (int)nk * 2);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = false;
    if (!w.fail) {
        FILE* f = re_fopen(path, "wb");
        if (f) ok = fwrite(w.buf, 1, w.len, f) == w.len, ok = fclose(f) == 0 && ok;
    }
    jw_free(&w);
    camshake_reload();
    return ok;
}

bool camshake_create(const char* folder_vpath, const char* name, const CamShake* from, char* out_vpath, uint32_t cap) {
    char vp[600], abs[1100];
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vp, sizeof vp, "%s/%s%d.rshake", folder_vpath, name, i + 1);
        else snprintf(vp, sizeof vp, "%s/%s.rshake", folder_vpath, name);
        if (!to_abs(vp, abs, sizeof abs)) return false;
        if (!re_file_stat(abs, NULL, NULL, NULL)) break;
    }
    CamShake s;
    if (from) s = *from;
    else camshake_defaults(&s);
    if (!camshake_save(vp, &s)) return false;
    camshake_remember(vp);
    if (out_vpath) snprintf(out_vpath, cap, "%s", vp);
    return true;
}

static bool g_scanned_all; // (camshake_find looked through the Content's .rshake files)
void camshake_reload(void) {
    for (uint32_t i = 0; i < g_ncs; i++) g_cs[i].mtime = -1; // (each read again when asked for)
    g_scanned_all = false;
}

static Entry* find(uint32_t hash) {
    for (uint32_t i = 0; i < g_ncs; i++)
        if (g_cs[i].hash == hash) return &g_cs[i];
    return NULL;
}

typedef struct Scan { const char* abs; const char* vdir; int depth; } Scan;
static void scan_dir(const char* abs_dir, const char* vdir, int depth);
static void scan_one(void* user, const ReDirEntry* d) {
    const Scan* s = user;
    if (g_ncs >= CS_MAX) return;
    char a[1100], v[600];
    snprintf(a, sizeof a, "%s/%s", s->abs, d->name);
    snprintf(v, sizeof v, "%s/%s", s->vdir, d->name);
    if (d->is_dir) scan_dir(a, v, s->depth + 1);
    else if (camshake_is_path(v) && !find(camshake_vpath_hash(v))) {
        Entry* e = &g_cs[g_ncs++];
        memset(e, 0, sizeof *e);
        e->hash = camshake_vpath_hash(v), e->mtime = -1;
        snprintf(e->vpath, sizeof e->vpath, "%s", v);
    }
}
static void scan_dir(const char* abs_dir, const char* vdir, int depth) {
    if (depth > 8 || g_ncs >= CS_MAX) return;
    Scan s = { abs_dir, vdir, depth };
    re_dir_walk(abs_dir, scan_one, &s);
}
static void scan_content(void) {
    const char* root = ui_content_root();
    if (root && root[0]) scan_dir(root, "/Content", 0);
}

static const CamShake* entry_shake(Entry* e) {
    if (e->mtime < 0) {
        char path[1100];
        int64_t mt = 0;
        e->ok = to_abs(e->vpath, path, sizeof path) && re_file_stat(path, NULL, &mt, NULL) && camshake_load(e->vpath, &e->s);
        e->mtime = e->ok ? mt : 0;
    }
    return e->ok ? &e->s : NULL;
}

static uint32_t g_scanned_for; // (a hash not found: the Content looked through once for it)
const CamShake* camshake_get(uint32_t hash) {
    if (!hash) return NULL;
    Entry* e = find(hash);
    if (!e && g_scanned_for != hash) {
        g_scanned_for = hash;
        scan_content();
        e = find(hash);
    }
    return e ? entry_shake(e) : NULL;
}

const char* camshake_vpath(uint32_t hash) {
    const Entry* e = hash ? find(hash) : NULL;
    return e ? e->vpath : "";
}

void camshake_remember(const char* vpath) {
    if (find(camshake_vpath_hash(vpath)) || g_ncs >= CS_MAX) return;
    Entry* e = &g_cs[g_ncs++];
    memset(e, 0, sizeof *e);
    e->hash = camshake_vpath_hash(vpath), e->mtime = -1;
    snprintf(e->vpath, sizeof e->vpath, "%s", vpath);
}

static bool same_name(const char* a, const char* b, size_t n) { // (case-insensitive, b of length n)
    for (size_t i = 0; i < n; i++)
        if (!a[i] || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return a[n] == 0;
}

const CamShake* camshake_find(const char* name) {
    if (!name || !name[0]) return NULL;
    if (name[0] == '/') { // a path
        camshake_remember(name);
        return camshake_get(camshake_vpath_hash(name));
    }
    // an asset of that name (its file's name without the folder and the extension)
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < g_ncs; i++) {
            const char* base = strrchr(g_cs[i].vpath, '/');
            base = base ? base + 1 : g_cs[i].vpath;
            const char* dot = strrchr(base, '.');
            if (dot && same_name(name, base, (size_t)(dot - base))) {
                const CamShake* s = entry_shake(&g_cs[i]);
                if (s) return s;
            }
        }
        if (pass || g_scanned_all) break; // (the Content looked through once: new ones come by camshake_remember, a reload)
        g_scanned_all = true;
        scan_content();
    }
    // a built-in preset ("Explosion", "CS_Explosion")
    const char* p = !strncmp(name, "CS_", 3) || !strncmp(name, "cs_", 3) ? name + 3 : name;
    const CamShakePreset* ps = camshake_presets();
    for (int i = 0; i < CAMSHAKE_PRESETS; i++)
        if (same_name(p, ps[i].name, strlen(ps[i].name))) return &ps[i].s;
    return NULL;
}
