// Audio's small assets (audio_internal.h): Sound Attenuation, Sound Class, Sound Mix, Sound Concurrency, Submix and
// Sound Cue, JSON files in Content found by the hash of their vpath; the engine's own Sound Classes (/Engine/Audio:
// Master, Music, SFX, Voice, UI, Ambient) and default concurrency; a cue's graph evaluated into the waves it plays.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metasound.h"
#include "audio_internal.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"

#define MAX_ASSETS 1024

const char* const g_aua_ext[AUA_KIND_COUNT] = { "rsatt", "rsclass", "rsmix", "rsconc", "rsubmix", "rscue" };
const char* const g_aua_names[AUA_KIND_COUNT] = { "Sound Attenuation", "Sound Class", "Sound Mix", "Sound Concurrency", "Submix", "Sound Cue" };
const char* const g_cue_kind_names[CUE_KIND_COUNT] = { "Output", "Wave Player", "Random", "Modulator", "Mixer", "Concatenator", "Looping", "Delay" };
static const char* const g_builtin_class_names[AU_SC_COUNT] = { "SC_Master", "SC_Music", "SC_SFX", "SC_Voice", "SC_UI", "SC_Ambient" };

static struct {
    char root[512];
    AuAsset* a[MAX_ASSETS];
    uint32_t n;
    bool scanned, builtins;
    AuProject project;
    uint32_t rng;
} S;

#define TAB 4096
static uint32_t g_tab_key[TAB], g_tab_idx[TAB], g_tab_n = ~0u;
static AuProject g_default_project = { 64, 0, 1.0f, 0, 0, 0, 0 };

uint32_t au_builtin_class(int which) {
    char vp[64];
    snprintf(vp, sizeof vp, "/Engine/Audio/%s", g_builtin_class_names[which]);
    return audio_vpath_hash(vp);
}
uint32_t au_builtin_concurrency(void) { return audio_vpath_hash("/Engine/Audio/SCon_Default"); }

void au_asset_defaults(AuAsset* a, int kind) {
    memset(&a->u, 0, sizeof a->u);
    a->kind = kind;
    switch (kind) {
    case AUA_ATTENUATION: au_attenuation_defaults(&a->u.att); break;
    case AUA_CLASS: a->u.cls.volume = a->u.cls.pitch = 1.0f, a->u.cls.lpf_hz = 0.0f, a->u.cls.passive_threshold = 0.05f; break;
    case AUA_MIX:
        a->u.mix.fade_in = 0.2f, a->u.mix.fade_out = 0.5f;
        break;
    case AUA_CONCURRENCY:
        a->u.conc.max_count = 16, a->u.conc.rule = AU_RES_STOP_FARTHEST_THEN_OLDEST, a->u.conc.volume_scale = 1.0f;
        break;
    case AUA_SUBMIX: a->u.sub.volume = 1.0f; break;
    case AUA_CUE: {
        AuCue* c = &a->u.cue;
        c->volume = c->pitch = c->priority = 1.0f;
        c->nnodes = 1;
        c->n[0].kind = CUE_OUTPUT, c->n[0].x = 400.0f, c->n[0].y = 100.0f;
        for (int k = 0; k < AU_CUE_IN; k++) c->n[0].in[k] = -1;
        break;
    }
    default: break;
    }
}

static void add_builtins(void) {
    if (S.builtins) return;
    S.builtins = true;
    static AuAsset cls[AU_SC_COUNT], conc;
    for (int k = 0; k < AU_SC_COUNT && S.n < MAX_ASSETS; k++) {
        AuAsset* a = &cls[k];
        au_asset_defaults(a, AUA_CLASS);
        a->builtin = true;
        snprintf(a->name, sizeof a->name, "%s", g_builtin_class_names[k]);
        a->hash = au_builtin_class(k);
        a->u.cls.hash = a->hash;
        a->u.cls.parent = k ? au_builtin_class(AU_SC_MASTER) : 0;
        if (k == AU_SC_UI) a->u.cls.ui = 1, a->u.cls.always_play = 1;
        if (k == AU_SC_VOICE) a->u.cls.always_play = 1;
        S.a[S.n++] = a;
    }
    au_asset_defaults(&conc, AUA_CONCURRENCY);
    conc.builtin = true;
    snprintf(conc.name, sizeof conc.name, "SCon_Default");
    conc.hash = conc.u.conc.hash = au_builtin_concurrency();
    if (S.n < MAX_ASSETS) S.a[S.n++] = &conc;
    g_default_project.default_class = au_builtin_class(AU_SC_SFX);
    g_default_project.default_concurrency = au_builtin_concurrency();
    if (!S.project.max_channels) S.project = g_default_project;
}

void audio_set_project(const AuProject* p) {
    add_builtins();
    S.project = p ? *p : g_default_project;
    if (!S.project.max_channels) S.project.max_channels = 64;
    if (!S.project.default_class) S.project.default_class = au_builtin_class(AU_SC_SFX);
    if (!S.project.default_concurrency) S.project.default_concurrency = au_builtin_concurrency();
    if (S.project.master_volume <= 0.0f) S.project.master_volume = 1.0f;
    if (S.project.base_mix) audio_set_base_mix(S.project.base_mix);
}
const AuProject* au_project(void) {
    add_builtins();
    return &S.project;
}

void au_assets_root(const char* root) {
    snprintf(S.root, sizeof S.root, "%s", root ? root : "");
    for (size_t i = 0; S.root[i]; i++)
        if (S.root[i] == '\\') S.root[i] = '/';
    size_t l = strlen(S.root);
    if (l && S.root[l - 1] == '/') S.root[l - 1] = 0;
    // the project's assets forgotten (the engine's stay)
    uint32_t keep = 0;
    for (uint32_t i = 0; i < S.n; i++) {
        if (S.a[i]->builtin) S.a[keep++] = S.a[i];
        else free(S.a[i]);
    }
    S.n = keep, S.scanned = false;
    g_tab_n = ~0u;
    add_builtins();
}

static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", S.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(S.root);
    if (rl && !strncmp(disk, S.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}
static int kind_of(const char* path) {
    const char* dot = strrchr(path, '.');
    for (int k = 0; dot && k < AUA_KIND_COUNT; k++)
        if (!_stricmp(dot + 1, g_aua_ext[k])) return k;
    return -1;
}

// --- JSON ---
static uint32_t jh(const JsonValue* o, const char* key) { return (uint32_t)json_get_num(o, key, 0.0); }
static void read_asset(AuAsset* a, const JsonValue* r) {
    au_asset_defaults(a, a->kind);
    switch (a->kind) {
    case AUA_ATTENUATION: {
        AuAttenuation* t = &a->u.att;
        t->spatialize = (uint8_t)json_get_bool(r, "spatialize", true);
        t->falloff = (uint8_t)json_get_int(r, "falloff", t->falloff);
        t->method = (uint8_t)json_get_int(r, "method", t->method);
        t->inner_radius = (float)json_get_num(r, "inner_radius", t->inner_radius);
        t->falloff_distance = (float)json_get_num(r, "falloff_distance", t->falloff_distance);
        t->db_at_max = (float)json_get_num(r, "db_at_max", t->db_at_max);
        t->non_spatial_radius = (float)json_get_num(r, "non_spatial_radius", t->non_spatial_radius);
        t->spread = (float)json_get_num(r, "spread", t->spread);
        t->air_absorption = (uint8_t)json_get_bool(r, "air_absorption", t->air_absorption);
        t->lpf_near_hz = (float)json_get_num(r, "lpf_near_hz", t->lpf_near_hz);
        t->lpf_far_hz = (float)json_get_num(r, "lpf_far_hz", t->lpf_far_hz);
        t->absorb_min = (float)json_get_num(r, "absorb_min", t->absorb_min);
        t->absorb_max = (float)json_get_num(r, "absorb_max", t->absorb_max);
        t->occlusion = (uint8_t)json_get_bool(r, "occlusion", t->occlusion);
        t->occlusion_lpf_hz = (float)json_get_num(r, "occlusion_lpf_hz", t->occlusion_lpf_hz);
        t->occlusion_volume = (float)json_get_num(r, "occlusion_volume", t->occlusion_volume);
        t->occlusion_interp = (float)json_get_num(r, "occlusion_interp", t->occlusion_interp);
        t->reverb_send = (uint8_t)json_get_bool(r, "reverb_send", t->reverb_send);
        t->reverb_wet_min = (float)json_get_num(r, "reverb_wet_min", t->reverb_wet_min);
        t->reverb_wet_max = (float)json_get_num(r, "reverb_wet_max", t->reverb_wet_max);
        t->reverb_dist_min = (float)json_get_num(r, "reverb_dist_min", t->reverb_dist_min);
        t->reverb_dist_max = (float)json_get_num(r, "reverb_dist_max", t->reverb_dist_max);
        t->doppler = (float)json_get_num(r, "doppler", t->doppler);
        t->priority_falloff = (float)json_get_num(r, "priority_falloff", t->priority_falloff);
        break;
    }
    case AUA_CLASS: {
        AuClass* c = &a->u.cls;
        c->parent = jh(r, "parent");
        c->volume = (float)json_get_num(r, "volume", 1.0);
        c->pitch = (float)json_get_num(r, "pitch", 1.0);
        c->lpf_hz = (float)json_get_num(r, "lpf_hz", 0.0);
        c->submix = jh(r, "submix");
        c->ui = (uint8_t)json_get_bool(r, "ui", false);
        c->always_play = (uint8_t)json_get_bool(r, "always_play", false);
        c->passive_mix = jh(r, "passive_mix");
        c->passive_threshold = (float)json_get_num(r, "passive_threshold", 0.05);
        break;
    }
    case AUA_MIX: {
        AuMix* m = &a->u.mix;
        m->fade_in = (float)json_get_num(r, "fade_in", m->fade_in);
        m->fade_out = (float)json_get_num(r, "fade_out", m->fade_out);
        m->duration = (float)json_get_num(r, "duration", 0.0);
        const JsonValue* adj = json_get(r, "adjust");
        for (size_t i = 0; i < json_len(adj) && m->nadj < AU_MIX_ADJUST; i++) {
            const JsonValue* o = json_at(adj, i);
            m->adj[m->nadj].sound_class = jh(o, "class");
            m->adj[m->nadj].volume = (float)json_get_num(o, "volume", 1.0);
            m->adj[m->nadj].pitch = (float)json_get_num(o, "pitch", 1.0);
            m->adj[m->nadj].lpf_hz = (float)json_get_num(o, "lpf_hz", 0.0);
            m->adj[m->nadj].children = (uint8_t)json_get_bool(o, "children", true);
            m->nadj++;
        }
        break;
    }
    case AUA_CONCURRENCY: {
        AuConcurrency* c = &a->u.conc;
        c->max_count = (uint32_t)json_get_int(r, "max_count", 16);
        c->rule = (uint8_t)json_get_int(r, "rule", c->rule);
        c->per_owner = (uint8_t)json_get_bool(r, "per_owner", false);
        c->retrigger_time = (float)json_get_num(r, "retrigger_time", 0.0);
        c->volume_scale = (float)json_get_num(r, "volume_scale", 1.0);
        break;
    }
    case AUA_SUBMIX: {
        AuSubmix* s = &a->u.sub;
        s->parent = jh(r, "parent");
        s->volume = (float)json_get_num(r, "volume", 1.0);
        const JsonValue* fx = json_get(r, "effects");
        for (size_t i = 0; i < json_len(fx) && s->nfx < AU_BUS_FX; i++) {
            const JsonValue* o = json_at(fx, i);
            int kind = json_get_int(o, "kind", AUFX_NONE);
            if (kind <= AUFX_NONE || kind >= AUFX_COUNT) continue;
            au_effect_defaults(&s->fx[s->nfx], kind);
            json_get_floats(o, "p", s->fx[s->nfx].p, AU_FX_PARAMS);
            s->nfx++;
        }
        break;
    }
    case AUA_CUE: {
        AuCue* c = &a->u.cue;
        c->volume = (float)json_get_num(r, "volume", 1.0);
        c->pitch = (float)json_get_num(r, "pitch", 1.0);
        c->priority = (float)json_get_num(r, "priority", 1.0);
        c->attenuation = jh(r, "attenuation"), c->sound_class = jh(r, "class"), c->concurrency = jh(r, "concurrency");
        const JsonValue* ns = json_get(r, "nodes");
        if (json_len(ns)) c->nnodes = 0;
        for (size_t i = 0; i < json_len(ns) && c->nnodes < AU_CUE_NODES; i++) {
            const JsonValue* o = json_at(ns, i);
            AuCueNode* n = &c->n[c->nnodes++];
            memset(n, 0, sizeof *n);
            n->kind = (uint8_t)json_get_int(o, "kind", CUE_WAVE);
            n->flag = (uint8_t)json_get_int(o, "flag", 0);
            n->x = (float)json_get_num(o, "x", 0.0), n->y = (float)json_get_num(o, "y", 0.0);
            n->wave = jh(o, "wave");
            json_get_floats(o, "p", n->p, 4);
            for (int k = 0; k < AU_CUE_IN; k++) n->in[k] = -1, n->weight[k] = 1.0f;
            const JsonValue* in = json_get(o, "in");
            for (size_t k = 0; k < json_len(in) && k < AU_CUE_IN; k++) n->in[k] = (int16_t)json_int(json_at(in, k), -1);
            n->nin = (uint8_t)(json_len(in) < AU_CUE_IN ? json_len(in) : AU_CUE_IN);
            json_get_floats(o, "weight", n->weight, AU_CUE_IN);
            n->last = -1;
        }
        if (!c->nnodes || c->n[0].kind != CUE_OUTPUT) { // (a broken file: an empty cue)
            au_asset_defaults(a, AUA_CUE);
        }
        break;
    }
    default: break;
    }
}

static void write_asset(const AuAsset* a, JsonWriter* w) {
    jw_obj_begin(w);
    jw_kstr(w, "type", g_aua_names[a->kind]);
    jw_kint(w, "version", 1);
    switch (a->kind) {
    case AUA_ATTENUATION: {
        const AuAttenuation* t = &a->u.att;
        jw_kbool(w, "spatialize", t->spatialize), jw_kint(w, "falloff", t->falloff), jw_kint(w, "method", t->method);
        jw_kfloat(w, "inner_radius", t->inner_radius), jw_kfloat(w, "falloff_distance", t->falloff_distance);
        jw_kfloat(w, "db_at_max", t->db_at_max), jw_kfloat(w, "non_spatial_radius", t->non_spatial_radius);
        jw_kfloat(w, "spread", t->spread), jw_kbool(w, "air_absorption", t->air_absorption);
        jw_kfloat(w, "lpf_near_hz", t->lpf_near_hz), jw_kfloat(w, "lpf_far_hz", t->lpf_far_hz);
        jw_kfloat(w, "absorb_min", t->absorb_min), jw_kfloat(w, "absorb_max", t->absorb_max);
        jw_kbool(w, "occlusion", t->occlusion), jw_kfloat(w, "occlusion_lpf_hz", t->occlusion_lpf_hz);
        jw_kfloat(w, "occlusion_volume", t->occlusion_volume), jw_kfloat(w, "occlusion_interp", t->occlusion_interp);
        jw_kbool(w, "reverb_send", t->reverb_send), jw_kfloat(w, "reverb_wet_min", t->reverb_wet_min);
        jw_kfloat(w, "reverb_wet_max", t->reverb_wet_max), jw_kfloat(w, "reverb_dist_min", t->reverb_dist_min);
        jw_kfloat(w, "reverb_dist_max", t->reverb_dist_max), jw_kfloat(w, "doppler", t->doppler);
        jw_kfloat(w, "priority_falloff", t->priority_falloff);
        break;
    }
    case AUA_CLASS: {
        const AuClass* c = &a->u.cls;
        jw_kint(w, "parent", c->parent), jw_kfloat(w, "volume", c->volume), jw_kfloat(w, "pitch", c->pitch);
        jw_kfloat(w, "lpf_hz", c->lpf_hz), jw_kint(w, "submix", c->submix), jw_kbool(w, "ui", c->ui);
        jw_kbool(w, "always_play", c->always_play), jw_kint(w, "passive_mix", c->passive_mix);
        jw_kfloat(w, "passive_threshold", c->passive_threshold);
        break;
    }
    case AUA_MIX: {
        const AuMix* m = &a->u.mix;
        jw_kfloat(w, "fade_in", m->fade_in), jw_kfloat(w, "fade_out", m->fade_out), jw_kfloat(w, "duration", m->duration);
        jw_key(w, "adjust");
        jw_arr_begin(w);
        for (uint32_t i = 0; i < m->nadj; i++) {
            jw_obj_begin(w);
            jw_kint(w, "class", m->adj[i].sound_class), jw_kfloat(w, "volume", m->adj[i].volume);
            jw_kfloat(w, "pitch", m->adj[i].pitch), jw_kfloat(w, "lpf_hz", m->adj[i].lpf_hz);
            jw_kbool(w, "children", m->adj[i].children);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        break;
    }
    case AUA_CONCURRENCY: {
        const AuConcurrency* c = &a->u.conc;
        jw_kint(w, "max_count", c->max_count), jw_kint(w, "rule", c->rule), jw_kbool(w, "per_owner", c->per_owner);
        jw_kfloat(w, "retrigger_time", c->retrigger_time), jw_kfloat(w, "volume_scale", c->volume_scale);
        break;
    }
    case AUA_SUBMIX: {
        const AuSubmix* s = &a->u.sub;
        jw_kint(w, "parent", s->parent), jw_kfloat(w, "volume", s->volume);
        jw_key(w, "effects");
        jw_arr_begin(w);
        for (uint32_t i = 0; i < s->nfx; i++) {
            jw_obj_begin(w);
            jw_kint(w, "kind", s->fx[i].kind);
            jw_kfloats(w, "p", s->fx[i].p, AU_FX_PARAMS);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        break;
    }
    case AUA_CUE: {
        const AuCue* c = &a->u.cue;
        jw_kfloat(w, "volume", c->volume), jw_kfloat(w, "pitch", c->pitch), jw_kfloat(w, "priority", c->priority);
        jw_kint(w, "attenuation", c->attenuation), jw_kint(w, "class", c->sound_class), jw_kint(w, "concurrency", c->concurrency);
        jw_key(w, "nodes");
        jw_arr_begin(w);
        for (uint32_t i = 0; i < c->nnodes; i++) {
            const AuCueNode* n = &c->n[i];
            jw_obj_begin(w);
            jw_kint(w, "kind", n->kind), jw_kint(w, "flag", n->flag);
            jw_kfloat(w, "x", n->x), jw_kfloat(w, "y", n->y);
            if (n->wave) jw_kint(w, "wave", n->wave);
            jw_kfloats(w, "p", n->p, 4);
            jw_key(w, "in");
            jw_arr_begin(w);
            for (int k = 0; k < n->nin; k++) jw_int(w, n->in[k]);
            jw_arr_end(w);
            jw_kfloats(w, "weight", n->weight, n->nin ? n->nin : 1);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        break;
    }
    default: break;
    }
    jw_obj_end(w);
}

static void fix_hashes(AuAsset* a) { // (each struct carries its own hash: the mixer's lookups compare it)
    switch (a->kind) {
    case AUA_ATTENUATION: a->u.att.hash = a->hash; break;
    case AUA_CLASS: a->u.cls.hash = a->hash; break;
    case AUA_MIX: a->u.mix.hash = a->hash; break;
    case AUA_CONCURRENCY: a->u.conc.hash = a->hash; break;
    case AUA_SUBMIX: a->u.sub.hash = a->hash; break;
    default: break;
    }
}

static bool load_file(AuAsset* a) {
    FILE* f = re_fopen(a->path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // (a load, not a frame)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(t);
        return false;
    }
    t[n] = 0;
    JsonDoc doc;
    char err[128];
    ok = json_parse(t, (size_t)n, &doc, err, sizeof err);
    free(t);
    if (!ok) return false;
    read_asset(a, &doc.root);
    json_free(&doc);
    fix_hashes(a);
    return true;
}

AuAsset* au_asset_load(const char* vpath) {
    add_builtins();
    uint32_t h = audio_vpath_hash(vpath);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.a[i]->hash == h) return S.a[i];
    int kind = kind_of(vpath);
    if (kind < 0 || S.n >= MAX_ASSETS) return NULL;
    AuAsset* a = calloc(1, sizeof *a); // (a load, not a frame; kept: the mixer's lookups point at them)
    if (!a) return NULL;
    a->kind = kind, a->hash = h;
    disk_of(vpath, a->path, sizeof a->path);
    const char* b = strrchr(vpath, '/');
    snprintf(a->name, sizeof a->name, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(a->name, '.');
    if (dot) *dot = 0;
    if (!load_file(a)) {
        free(a);
        return NULL;
    }
    S.a[S.n++] = a;
    return a;
}

bool au_asset_save(AuAsset* a) {
    if (!a || a->builtin || !a->path[0]) return false;
    fix_hashes(a);
    JsonWriter w;
    jw_init(&w);
    write_asset(a, &w);
    bool ok = !w.fail;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", a->path);
    FILE* f = ok ? re_fopen(tmp, "wb") : NULL;
    ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && re_replace_file(tmp, a->path);
    jw_free(&w);
    if (ok) a->dirty = false;
    return ok;
}

AuAsset* au_asset_create(int kind, const char* vpath) {
    if (kind < 0 || kind >= AUA_KIND_COUNT || S.n >= MAX_ASSETS) return NULL;
    AuAsset* a = calloc(1, sizeof *a);
    if (!a) return NULL;
    au_asset_defaults(a, kind);
    a->hash = audio_vpath_hash(vpath);
    disk_of(vpath, a->path, sizeof a->path);
    const char* b = strrchr(vpath, '/');
    snprintf(a->name, sizeof a->name, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(a->name, '.');
    if (dot) *dot = 0;
    if (!au_asset_save(a)) {
        free(a);
        return NULL;
    }
    for (uint32_t i = 0; i < S.n; i++) // (one made again over an old one: replaced)
        if (S.a[i]->hash == a->hash) {
            *S.a[i] = *a;
            free(a);
            return S.a[i];
        }
    S.a[S.n++] = a;
    return a;
}

static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        scan_dir(path, 1);
        return;
    }
    if (kind_of(e->name) < 0) return;
    vpath_of(path, vpath, sizeof vpath);
    au_asset_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth > 12) return;
    re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    add_builtins();
    if (S.scanned || !S.root[0]) return;
    S.scanned = true;
    scan_dir(S.root, 0);
}

// A table of the hashes (the mixer's lookups run per sound per frame: no list walk)
static AuAsset* lookup(uint32_t hash) {
    if (g_tab_n != S.n) {
        memset(g_tab_key, 0, sizeof g_tab_key);
        for (uint32_t i = 0; i < S.n; i++) {
            uint32_t s = (S.a[i]->hash * 2654435761u) & (TAB - 1);
            while (g_tab_key[s]) s = (s + 1) & (TAB - 1);
            g_tab_key[s] = S.a[i]->hash, g_tab_idx[s] = i;
        }
        g_tab_n = S.n;
    }
    for (uint32_t s = (hash * 2654435761u) & (TAB - 1); g_tab_key[s]; s = (s + 1) & (TAB - 1))
        if (g_tab_key[s] == hash) return S.a[g_tab_idx[s]];
    return NULL;
}

AuAsset* au_asset(uint32_t hash) {
    if (!hash) return NULL;
    scan();
    AuAsset* f = lookup(hash);
    if (f) return f;
    static double last; // (a miss: walked again at most every 2 s: an import or a new asset)
    double now = re_time_seconds();
    if (now - last < 2.0) return NULL;
    last = now;
    S.scanned = false;
    scan();
    for (uint32_t i = 0; i < S.n; i++)
        if (S.a[i]->hash == hash) return S.a[i];
    return NULL;
}
uint32_t au_asset_count(void) {
    scan();
    return S.n;
}
AuAsset* au_asset_at(uint32_t i) { return i < S.n ? S.a[i] : NULL; }

static const void* typed(uint32_t hash, int kind) {
    AuAsset* a = hash ? au_asset(hash) : NULL;
    return a && a->kind == kind ? (const void*)&a->u : NULL;
}
const AuAttenuation* au_attenuation(uint32_t h) { return typed(h, AUA_ATTENUATION); }
const AuClass* au_class(uint32_t h) { return typed(h, AUA_CLASS); }
const AuMix* au_mix(uint32_t h) { return typed(h, AUA_MIX); }
const AuConcurrency* au_concurrency(uint32_t h) { return typed(h, AUA_CONCURRENCY); }
const AuSubmix* au_submix(uint32_t h) { return typed(h, AUA_SUBMIX); }

const char* au_sound_name(uint32_t hash) {
    if (!hash) return "";
    SoundWave* w = audio_wave(hash);
    if (w) return w->name;
    AuAsset* a = au_asset(hash);
    return a ? a->name : "";
}

// --- A cue played: its graph walked from the Output, each node choosing (Random), scaling (Modulator), delaying
// (Delay), mixing (Mixer: all its inputs) or chaining (Concatenator: each after the one before) ---
static float frand(void) {
    S.rng = S.rng * 1664525u + 1013904223u;
    return (float)(S.rng >> 8) / 16777216.0f;
}
// the node's waves into the plan; returns its length (seconds, at its pitch; the longest of what it started)
static float eval(AuCue* c, int ni, float vol, float pitch, float delay, uint8_t loop, AuPlan* out, int depth) {
    if (ni < 0 || ni >= (int)c->nnodes || depth > 32) return 0.0f;
    AuCueNode* n = &c->n[ni];
    switch (n->kind) {
    case CUE_OUTPUT:
    case CUE_LOOPING: return eval(c, n->nin ? n->in[0] : -1, vol, pitch, delay, (uint8_t)(loop || n->kind == CUE_LOOPING), out, depth + 1);
    case CUE_WAVE: {
        SoundWave* w = audio_wave(n->wave);
        if (!w || out->n >= AU_PLAN_WAVES) return 0.0f;
        uint32_t k = out->n++;
        out->wave[k] = w, out->volume[k] = vol, out->pitch[k] = pitch, out->delay[k] = delay;
        out->loop[k] = (uint8_t)(loop || n->flag);
        return (float)w->length / (pitch > 0.01f ? pitch : 1.0f);
    }
    case CUE_MODULATOR: {
        float v = n->p[0] + (n->p[1] - n->p[0]) * frand(), p = n->p[2] + (n->p[3] - n->p[2]) * frand();
        if (n->p[1] <= 0.0f) v = 1.0f; // (unset: unchanged)
        if (n->p[3] <= 0.0f) p = 1.0f;
        return eval(c, n->nin ? n->in[0] : -1, vol * v, pitch * p, delay, loop, out, depth + 1);
    }
    case CUE_DELAY: {
        float d = n->p[0] + (fmaxf(n->p[1], n->p[0]) - n->p[0]) * frand();
        return d + eval(c, n->nin ? n->in[0] : -1, vol, pitch, delay + d, loop, out, depth + 1);
    }
    case CUE_RANDOM: {
        if (!n->nin) return 0.0f;
        float sum = 0.0f;
        for (int k = 0; k < n->nin; k++)
            if (!(n->flag && k == n->last && n->nin > 1)) sum += fmaxf(n->weight[k], 0.0f);
        float r = frand() * sum;
        int pick = 0;
        for (int k = 0; k < n->nin; k++) {
            if (n->flag && k == n->last && n->nin > 1) continue; // (no repeats: the last pick left out)
            pick = k;
            r -= fmaxf(n->weight[k], 0.0f);
            if (r <= 0.0f) break;
        }
        n->last = (int8_t)pick;
        return eval(c, n->in[pick], vol, pitch, delay, loop, out, depth + 1);
    }
    case CUE_MIXER: {
        float len = 0.0f;
        for (int k = 0; k < n->nin; k++) len = fmaxf(len, eval(c, n->in[k], vol * n->weight[k], pitch, delay, loop, out, depth + 1));
        return len;
    }
    case CUE_CONCAT: {
        float at = 0.0f;
        for (int k = 0; k < n->nin; k++) at += eval(c, n->in[k], vol, pitch, delay + at, loop, out, depth + 1);
        return at;
    }
    default: return 0.0f;
    }
}

bool au_plan(uint32_t sound, AuPlan* out) {
    memset(out, 0, sizeof *out);
    out->volume_all = out->pitch_all = out->priority = 1.0f;
    SoundWave* w = audio_wave(sound);
    if (w) {
        out->n = 1;
        out->wave[0] = w, out->volume[0] = out->pitch[0] = 1.0f;
        out->loop[0] = (w->h.flags & SND_LOOPING) != 0;
        out->attenuation = w->h.attenuation, out->sound_class = w->h.sound_class, out->concurrency = w->h.concurrency;
        out->priority = w->h.priority > 0.0f ? w->h.priority : 1.0f;
        return true;
    }
    const MsAsset* ms = ms_find(sound); // (B8: a MetaSound: one voice rendering its graph)
    if (ms) {
        out->n = 1;
        out->wave[0] = (SoundWave*)&ms->wave, out->volume[0] = out->pitch[0] = 1.0f, out->loop[0] = 1;
        out->attenuation = ms->attenuation, out->sound_class = ms->sound_class;
        return true;
    }
    AuAsset* a = au_asset(sound);
    if (!a || a->kind != AUA_CUE) return false;
    AuCue* c = &a->u.cue;
    if (!S.rng) S.rng = (uint32_t)(re_time_seconds() * 1000003.0) | 1u;
    eval(c, 0, 1.0f, 1.0f, 0.0f, 0, out, 0);
    out->volume_all = c->volume, out->pitch_all = c->pitch, out->priority = c->priority;
    out->attenuation = c->attenuation, out->sound_class = c->sound_class, out->concurrency = c->concurrency;
    if (out->n) { // (the cue's settings, else its first wave's)
        if (!out->attenuation) out->attenuation = out->wave[0]->h.attenuation;
        if (!out->sound_class) out->sound_class = out->wave[0]->h.sound_class;
        if (!out->concurrency) out->concurrency = out->wave[0]->h.concurrency;
    }
    return out->n > 0;
}

uint32_t audio_sound_by_name(const char* name) {
    if (!name || !name[0]) return 0;
    for (uint32_t i = 0, n = audio_wave_count(); i < n; i++)
        if (!_stricmp(audio_wave_at(i)->name, name)) return audio_wave_at(i)->hash;
    for (uint32_t i = 0, n = au_asset_count(); i < n; i++)
        if (S.a[i]->kind == AUA_CUE && !_stricmp(S.a[i]->name, name)) return S.a[i]->hash;
    return ms_by_name(name); // (B8: a MetaSound)
}

uint32_t audio_asset_by_name(int kind, const char* name) {
    if (kind < 0) return audio_sound_by_name(name);
    if (!name || !name[0]) return 0;
    for (uint32_t i = 0, n = au_asset_count(); i < n; i++)
        if (S.a[i]->kind == kind && !_stricmp(S.a[i]->name, name)) return S.a[i]->hash;
    return 0;
}
