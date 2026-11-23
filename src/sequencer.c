// Sequencer: the Level Sequence assets and their players (sequencer.h, notas-dev/sequencer.md)
#include "render.h"
#include "sequencer.h"
#include "terror.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "audio.h"
#include "blueprint.h"
#include "components.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"

#define SEQ_CACHE 64

typedef struct Player {
    Sequence* s;
    float t, rate;
    bool loop, active;
    Entity bind[SEQ_MAX_TRACKS]; // the tracks' actors (resolved at Play: no search by name in a frame)
    int cam_key;                  // the Camera Cuts key last looked up, its camera
    Entity cam;
} Player;

static struct {
    char root[520];
    Sequence* cache[SEQ_CACHE];
    uint32_t ncache;
    Player players[SEQ_PLAYERS];
    SeqStats stats;
} Q;

static const char* const g_track_names[SEQT_COUNT] = { "Transform", "Visibility", "Light Intensity", "Field Of View",
                                                         "Animation", "Camera Cuts", "Event", "Audio", "Fade", "Horror Filter" };
#define SEQ_AUDIO_OWNER 0x5E0A0D10u // (the sequences' sounds: stopped with them)
const char* seq_track_type_name(int type) { return type >= 0 && type < SEQT_COUNT ? g_track_names[type] : "?"; }
const char* seq_interp_name(int interp) {
    static const char* const n[SEQI_COUNT] = { "Auto", "User", "Linear", "Constant" };
    return interp >= 0 && interp < SEQI_COUNT ? n[interp] : "?";
}
int seq_track_channels(int type) {
    return type == SEQT_TRANSFORM ? 9 : (type == SEQT_LIGHT_INTENSITY || type == SEQT_FOV || type == SEQT_FADE || type == SEQT_HORROR_FILTER) ? 1 : 0;
}

void seq_init(const char* content_root) {
    snprintf(Q.root, sizeof Q.root, "%s", content_root ? content_root : "Content");
    seq_reload_all();
}

static void seq_free(Sequence* s) {
    for (uint32_t i = 0; i < s->ntracks; i++) free(s->tracks[i].keys);
    free(s);
}

static uint32_t g_generation;
uint32_t seq_generation(void) { return g_generation; }

void seq_reload_all(void) {
    g_generation++; // (the Sequencer window drops its pointer)
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++) Q.players[i].active = false;
    for (uint32_t i = 0; i < Q.ncache; i++) seq_free(Q.cache[i]);
    Q.ncache = 0;
}

// --- Files ---

static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = n > 0 ? malloc((size_t)n + 1) : NULL; // (a load: not in a frame)
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *len = (size_t)n;
    return b;
}

static void to_disk(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", Q.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}

typedef struct ListWalk { char (*out)[300]; uint32_t cap, n; char rel[300]; int depth; } ListWalk;
static void list_dir(ListWalk* lw);
static void list_entry(void* user, const ReDirEntry* e) {
    ListWalk* lw = user;
    if (lw->n >= lw->cap) return;
    if (e->is_dir) {
        if (lw->depth > 10) return;
        char keep[300];
        snprintf(keep, sizeof keep, "%s", lw->rel);
        snprintf(lw->rel, sizeof lw->rel, "%s/%s", keep, e->name);
        lw->depth++;
        list_dir(lw);
        lw->depth--;
        snprintf(lw->rel, sizeof lw->rel, "%s", keep);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (dot && !_stricmp(dot, ".rseq")) snprintf(lw->out[lw->n++], 300, "/Content%s/%s", lw->rel, e->name);
}
static void list_dir(ListWalk* lw) {
    char dir[900];
    snprintf(dir, sizeof dir, "%s%s", Q.root, lw->rel);
    re_dir_walk(dir, list_entry, lw);
}

uint32_t seq_list(char (*out)[300], uint32_t cap) {
    ListWalk lw = { out, cap, 0, "", 0 };
    list_dir(&lw);
    return lw.n;
}

static void name_of(const char* vpath, char* out, size_t cap) {
    const char* b = strrchr(vpath, '/');
    snprintf(out, cap, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static bool parse(Sequence* s, const char* text, size_t len) {
    JsonDoc doc;
    char err[128];
    if (!json_parse(text, len, &doc, err, sizeof err)) return false;
    const JsonValue* r = &doc.root;
    int version = json_get_int(r, "version", 1);
    s->length = (float)json_get_num(r, "length", 5.0);
    s->start = (float)json_get_num(r, "start", 0.0);
    s->fps = (float)json_get_num(r, "fps", 30.0);
    const JsonValue* tracks = json_get(r, "tracks");
    for (size_t i = 0; i < json_len(tracks) && s->ntracks < SEQ_MAX_TRACKS; i++) {
        const JsonValue* jt = json_at(tracks, i);
        int type = json_get_int(jt, "type", -1);
        if (type < 0 || type >= SEQT_COUNT) continue;
        SeqTrack* tr = &s->tracks[s->ntracks++];
        memset(tr, 0, sizeof *tr);
        tr->type = (uint8_t)type;
        tr->loop_anim = json_get_bool(jt, "loop", false);
        snprintf(tr->actor, sizeof tr->actor, "%s", json_get_str(jt, "actor", ""));
        const JsonValue* keys = json_get(jt, "keys");
        for (size_t k = 0; k < json_len(keys); k++) {
            const JsonValue* jk = json_at(keys, k);
            SeqKey key;
            memset(&key, 0, sizeof key);
            key.t = (float)json_get_num(jk, "t", 0.0);
            json_get_floats(jk, "v", key.v, 9);
            snprintf(key.name, sizeof key.name, "%s", json_get_str(jk, "name", ""));
            key.interp = (uint8_t)json_get_int(jk, "i", SEQI_AUTO);
            if (key.interp >= SEQI_COUNT) key.interp = SEQI_AUTO;
            // (version 1: Light Intensity, Field Of View and Fade went linear between keys: they open as they played)
            if (version < 2 && (type == SEQT_LIGHT_INTENSITY || type == SEQT_FOV || type == SEQT_FADE)) key.interp = SEQI_LINEAR;
            if (key.interp == SEQI_USER) json_get_floats(jk, "tan", key.tan, 9);
            seq_add_key(tr, &key);
        }
    }
    json_free(&doc);
    return true;
}

Sequence* seq_load(const char* key) {
    if (!key || !key[0]) return NULL;
    bool is_path = key[0] == '/';
    for (uint32_t i = 0; i < Q.ncache; i++)
        if (is_path ? !_stricmp(Q.cache[i]->vpath, key) : !_stricmp(Q.cache[i]->name, key)) return Q.cache[i];
    char vpath[300];
    if (is_path) snprintf(vpath, sizeof vpath, "%s", key);
    else { // by name: the first file of that name under Content
        static char found[256][300];
        uint32_t n = seq_list(found, 256);
        vpath[0] = 0;
        for (uint32_t i = 0; i < n && !vpath[0]; i++) {
            char nm[SEQ_NAME];
            name_of(found[i], nm, sizeof nm);
            if (!_stricmp(nm, key)) snprintf(vpath, sizeof vpath, "%s", found[i]);
        }
        if (!vpath[0]) return NULL;
    }
    if (Q.ncache >= SEQ_CACHE) return NULL;
    Sequence* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->vpath, sizeof s->vpath, "%s", vpath);
    to_disk(vpath, s->path, sizeof s->path);
    name_of(vpath, s->name, sizeof s->name);
    size_t len = 0;
    char* text = read_all(s->path, &len);
    if (!text || !parse(s, text, len)) {
        free(text);
        seq_free(s);
        return NULL;
    }
    free(text);
    Q.cache[Q.ncache++] = s;
    return s;
}

bool seq_save(Sequence* s) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "format", "RealEngine Level Sequence");
    jw_kint(&w, "version", 2); // (2: the keys' interpolation and tangents, the range's start)
    jw_kfloat(&w, "length", s->length);
    if (s->start > 0.0f) jw_kfloat(&w, "start", s->start);
    jw_kfloat(&w, "fps", s->fps);
    jw_key(&w, "tracks");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < s->ntracks; i++) {
        const SeqTrack* tr = &s->tracks[i];
        jw_obj_begin(&w);
        jw_kint(&w, "type", tr->type);
        if (tr->actor[0]) jw_kstr(&w, "actor", tr->actor);
        if (tr->loop_anim) jw_kbool(&w, "loop", true);
        jw_key(&w, "keys");
        jw_arr_begin(&w);
        for (uint32_t k = 0; k < tr->nkeys; k++) {
            const SeqKey* key = &tr->keys[k];
            jw_obj_begin(&w);
            jw_kfloat(&w, "t", key->t);
            jw_kfloats(&w, "v", key->v, tr->type == SEQT_TRANSFORM ? 9 : 1);
            if (key->name[0]) jw_kstr(&w, "name", key->name);
            if (key->interp != SEQI_AUTO) jw_kint(&w, "i", key->interp);
            if (key->interp == SEQI_USER) jw_kfloats(&w, "tan", key->tan, tr->type == SEQT_TRANSFORM ? 9 : 1);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = !w.fail;
    FILE* f = ok ? re_fopen(s->path, "wb") : NULL;
    if (f) {
        ok = fwrite(w.buf, 1, w.len, f) == w.len;
        fclose(f);
    } else ok = false;
    jw_free(&w);
    return ok;
}

Sequence* seq_create(const char* folder_vpath, const char* name) {
    char vpath[300], disk[600];
    for (int n = 0; n < 1000; n++) {
        if (n) snprintf(vpath, sizeof vpath, "%s/%s_%d.rseq", folder_vpath, name, n);
        else snprintf(vpath, sizeof vpath, "%s/%s.rseq", folder_vpath, name);
        to_disk(vpath, disk, sizeof disk);
        if (!re_file_stat(disk, NULL, NULL, NULL)) break;
    }
    if (Q.ncache >= SEQ_CACHE) return NULL;
    Sequence* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->vpath, sizeof s->vpath, "%s", vpath);
    snprintf(s->path, sizeof s->path, "%s", disk);
    name_of(vpath, s->name, sizeof s->name);
    s->length = 5.0f, s->fps = 30.0f;
    seq_add_track(s, SEQT_CAMERA_CUTS, ""); // (Unreal's new sequence starts with its Camera Cuts track)
    if (!seq_save(s)) {
        seq_free(s);
        return NULL;
    }
    Q.cache[Q.ncache++] = s;
    return s;
}

// --- Editing ---

int seq_add_track(Sequence* s, SeqTrackType type, const char* actor) {
    if (s->ntracks >= SEQ_MAX_TRACKS) return -1;
    SeqTrack* tr = &s->tracks[s->ntracks];
    memset(tr, 0, sizeof *tr);
    tr->type = (uint8_t)type;
    snprintf(tr->actor, sizeof tr->actor, "%s", actor ? actor : "");
    return (int)s->ntracks++;
}

void seq_remove_track(Sequence* s, int i) {
    if (i < 0 || (uint32_t)i >= s->ntracks) return;
    free(s->tracks[i].keys);
    memmove(&s->tracks[i], &s->tracks[i + 1], (s->ntracks - (uint32_t)i - 1) * sizeof s->tracks[0]);
    s->ntracks--;
}

int seq_add_key(SeqTrack* tr, const SeqKey* k) {
    for (uint32_t i = 0; i < tr->nkeys; i++)
        if (fabsf(tr->keys[i].t - k->t) < 1e-4f) {
            tr->keys[i] = *k;
            return (int)i;
        }
    if (tr->nkeys >= tr->cap) {
        uint32_t c = tr->cap ? tr->cap * 2 : 8;
        SeqKey* nk = realloc(tr->keys, c * sizeof *nk); // (an edit or a load, not a frame)
        if (!nk) return -1;
        tr->keys = nk, tr->cap = c;
    }
    uint32_t at = tr->nkeys;
    while (at > 0 && tr->keys[at - 1].t > k->t) at--;
    memmove(&tr->keys[at + 1], &tr->keys[at], (tr->nkeys - at) * sizeof *k);
    tr->keys[at] = *k;
    tr->nkeys++;
    return (int)at;
}

void seq_remove_key(SeqTrack* tr, int i) {
    if (i < 0 || (uint32_t)i >= tr->nkeys) return;
    memmove(&tr->keys[i], &tr->keys[i + 1], (tr->nkeys - (uint32_t)i - 1) * sizeof tr->keys[0]);
    tr->nkeys--;
}

static int key_cmp(const void* a, const void* b) {
    float x = ((const SeqKey*)a)->t, y = ((const SeqKey*)b)->t;
    return x < y ? -1 : x > y ? 1 : 0;
}
void seq_sort_keys(SeqTrack* tr) {
    if (tr->nkeys > 1) qsort(tr->keys, tr->nkeys, sizeof tr->keys[0], key_cmp);
}

// --- Evaluation ---

Entity seq_find_actor(World* w, const char* name) {
    if (!name || !name[0]) return ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || !a->count) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ents[i];
    }
    return ENTITY_NONE;
}

// The key before t (or -1), keys by time
static int key_before(const SeqTrack* tr, float t) {
    int lo = 0, hi = (int)tr->nkeys - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tr->keys[mid].t <= t) at = mid, lo = mid + 1;
        else hi = mid - 1;
    }
    return at;
}

// A key's slope on channel c (units per second): its own (User), else Unreal's Auto (Catmull-Rom from the neighbors,
// flat at the ends)
static float slope(const SeqTrack* tr, int i, int c) {
    const SeqKey* k = tr->keys;
    int n = (int)tr->nkeys;
    if (k[i].interp == SEQI_USER) return k[i].tan[c];
    if (i <= 0 || i >= n - 1) return 0.0f;
    float dt = k[i + 1].t - k[i - 1].t;
    return dt > 1e-6f ? (k[i + 1].v[c] - k[i - 1].v[c]) / dt : 0.0f;
}

// A channel between keys by the left key's interpolation: Constant holds it, Linear a straight line, Auto / User a
// cubic Hermite (each end's slope: its key's own or Auto's)
static float channel(const SeqTrack* tr, int i, int c, float t) {
    const SeqKey* k = tr->keys;
    int n = (int)tr->nkeys;
    if (i < 0) return k[0].v[c];
    if (i >= n - 1) return k[n - 1].v[c];
    float t0 = k[i].t, t1 = k[i + 1].t, dt = t1 - t0;
    if (dt <= 1e-6f) return k[i + 1].v[c];
    float p0 = k[i].v[c], p1 = k[i + 1].v[c], u = (t - t0) / dt;
    if (k[i].interp == SEQI_CONSTANT) return p0;
    if (k[i].interp == SEQI_LINEAR) return p0 + (p1 - p0) * u;
    float m0 = slope(tr, i, c) * dt, m1 = slope(tr, i + 1, c) * dt;
    float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
}
static float cubic(const SeqTrack* tr, int i, int c, float t) { return channel(tr, i, c, t); }
static float linear(const SeqTrack* tr, int i, float t) { return channel(tr, i, 0, t); }

float seq_channel_at(const SeqTrack* tr, int c, float t) {
    if (!tr->nkeys || c < 0 || c > 8) return 0.0f;
    return channel(tr, key_before(tr, t), c, t);
}

static void evaluate(World* w, const Sequence* s, float from, float t, bool events, const Entity* bind) {
    for (uint32_t ti = 0; ti < s->ntracks; ti++) {
        const SeqTrack* tr = &s->tracks[ti];
        if (!tr->nkeys || tr->type == SEQT_CAMERA_CUTS) continue;
        Entity e = bind ? bind[ti] : tr->actor[0] ? seq_find_actor(w, tr->actor) : ENTITY_NONE;
        if (e && !ecs_alive(w, e)) continue;
        int i = key_before(tr, t);
        switch (tr->type) {
        case SEQT_TRANSFORM: {
            CTransform* x = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            if (!x) break;
            for (int c = 0; c < 3; c++) x->location[c] = cubic(tr, i, c, t);
            for (int c = 0; c < 3; c++) x->rotation[c] = cubic(tr, i, 3 + c, t);
            for (int c = 0; c < 3; c++) x->scale[c] = cubic(tr, i, 6 + c, t);
            ecs_touch(w, e);
            break;
        }
        case SEQT_VISIBILITY: { // stepped: the key before t holds
            CTransform* x = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            if (!x) break;
            bool vis = tr->keys[i < 0 ? 0 : i].v[0] != 0.0f;
            uint32_t f = vis ? x->flags & ~TRANSFORM_HIDDEN_GAME : x->flags | TRANSFORM_HIDDEN_GAME;
            if (f != x->flags) x->flags = f, ecs_touch(w, e);
            break;
        }
        case SEQT_LIGHT_INTENSITY: {
            CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
            if (l) l->intensity = linear(tr, i, t), ecs_touch(w, e);
            break;
        }
        case SEQT_FOV: {
            CCamera* c = e ? ecs_get(w, e, COMP_CAMERA) : NULL;
            if (c) c->fov_deg = linear(tr, i, t);
            break;
        }
        case SEQT_ANIMATION: // the keys crossed start their clips (playing forward only: a scrub back doesn't)
            if (!events || !e) break;
            for (uint32_t k = 0; k < tr->nkeys; k++)
                if (tr->keys[k].t > from && tr->keys[k].t <= t && tr->keys[k].name[0]) anim_play(w, e, tr->keys[k].name, tr->loop_anim);
            break;
        case SEQT_AUDIO: // the keys crossed play their sounds (at the bound actor, following it; else 2D)
            if (!events) break;
            for (uint32_t k = 0; k < tr->nkeys; k++) {
                if (!(tr->keys[k].t > from && tr->keys[k].t <= t) || !tr->keys[k].name[0]) continue;
                AudioPlay p;
                memset(&p, 0, sizeof p);
                p.sound = audio_sound_by_name(tr->keys[k].name), p.bone = -1, p.owner = SEQ_AUDIO_OWNER;
                p.volume = tr->keys[k].v[0] > 0.0f ? tr->keys[k].v[0] : 1.0f, p.pitch = tr->keys[k].v[1] > 0.0f ? tr->keys[k].v[1] : 1.0f;
                const CTransform* at = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
                if (at) p.spatial = true, p.follow = e, memcpy(p.pos, at->location, 12);
                audio_play(&p);
            }
            break;
        case SEQT_FADE: { // (B9) the picture to the key's color by the amount between the keys
            int c = i < 0 ? 0 : i;
            render_set_fade(linear(tr, i, t), &tr->keys[c].v[1]);
            break;
        }
        case SEQT_HORROR_FILTER: { // (Q13) the key's filter at the curve's intensity; the track's other filters off
            int c = i < 0 ? 0 : i;
            for (uint32_t k = 0; k < tr->nkeys; k++)
                if (strcmp(tr->keys[k].name, tr->keys[c].name)) {
                    if (!_stricmp(tr->keys[k].name, "Letterbox")) terror_seq_letterbox(0.0f);
                    else terror_seq_filter(terror_filter_by_name(tr->keys[k].name), 0.0f);
                }
            float v = linear(tr, i, t);
            if (!_stricmp(tr->keys[c].name, "Letterbox")) terror_seq_letterbox(v);
            else terror_seq_filter(terror_filter_by_name(tr->keys[c].name), v);
            break;
        }
        case SEQT_EVENT: // the bound actor's custom event; without a binding, every Blueprint actor that has it
            if (!events) break;
            for (uint32_t k = 0; k < tr->nkeys; k++) {
                if (!(tr->keys[k].t > from && tr->keys[k].t <= t) || !tr->keys[k].name[0]) continue;
                Q.stats.events_fired++;
                if (e) {
                    bp_call_event(w, e, tr->keys[k].name);
                    continue;
                }
                for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                    const EcsArchetype* a = &w->archetypes[ai];
                    if (!(a->mask & COMP_BIT(COMP_BLUEPRINT))) continue;
                    const Entity* ents = ecs_arch_entities(a);
                    for (uint32_t r = 0; r < a->count; r++) bp_call_event(w, ents[r], tr->keys[k].name);
                }
            }
            break;
        default: break;
        }
    }
}

void seq_evaluate(World* w, const Sequence* s, float from, float t, bool events) { evaluate(w, s, from, t, events, NULL); }

Entity seq_camera_at(World* w, const Sequence* s, float t) {
    for (uint32_t ti = 0; ti < s->ntracks; ti++) {
        const SeqTrack* tr = &s->tracks[ti];
        if (tr->type != SEQT_CAMERA_CUTS || !tr->nkeys) continue;
        int i = key_before(tr, t);
        return seq_find_actor(w, tr->keys[i < 0 ? 0 : i].name);
    }
    return ENTITY_NONE;
}

// --- Players ---

bool seq_play(World* w, const char* key, bool loop, float rate) {
    Sequence* s = seq_load(key);
    if (!s) return false;
    Player* p = NULL;
    for (uint32_t i = 0; i < SEQ_PLAYERS && !p; i++)
        if (Q.players[i].active && Q.players[i].s == s) p = &Q.players[i]; // (playing already: from the start again)
    for (uint32_t i = 0; i < SEQ_PLAYERS && !p; i++)
        if (!Q.players[i].active) p = &Q.players[i];
    if (!p) return false;
    p->s = s, p->t = s->start, p->loop = loop, p->rate = rate > 0.0f ? rate : 1.0f, p->active = true;
    for (uint32_t i = 0; i < s->ntracks; i++) p->bind[i] = s->tracks[i].actor[0] ? seq_find_actor(w, s->tracks[i].actor) : ENTITY_NONE;
    p->cam_key = -2, p->cam = ENTITY_NONE;
    evaluate(w, s, s->start - 1e-4f, s->start, true, p->bind); // the first frame as the sequence starts (keys at its range's start fire)
    return true;
}

// (Q23) The actors an Animation track played on go back to their Animation Blueprint when the sequence ends or is
// stopped (the Animation section's Restore State): anim_play had switched them to the Animation mode, and a Character
// came out of a cinematic frozen in its last clip with nothing to give it its locomotion back. One without an Animation
// Blueprint keeps the last frame (Keep State), as before.
static void restore_anim(World* w, const Player* p) {
    for (uint32_t i = 0; w && i < p->s->ntracks; i++) {
        Entity e = p->bind[i];
        if (p->s->tracks[i].type != SEQT_ANIMATION || e == ENTITY_NONE || !ecs_alive(w, e)) continue;
        const CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
        if (c && c->anim_class && !anim_uses_blueprint(w, e)) anim_use_blueprint(w, e), Q.stats.anim_restored++;
    }
}

static bool matches(const Player* p, const char* key) {
    return !key || !key[0] || !_stricmp(p->s->vpath, key) || !_stricmp(p->s->name, key);
}

void seq_stop(World* w, const char* key) {
    audio_stop_owner(SEQ_AUDIO_OWNER, 0.15f); // (the sequences' sounds end with them)
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++)
        if (Q.players[i].active && matches(&Q.players[i], key)) Q.players[i].active = false, restore_anim(w, &Q.players[i]);
}

bool seq_is_playing(const char* key) {
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++)
        if (Q.players[i].active && matches(&Q.players[i], key)) return true;
    return false;
}

void seq_tick(World* w, float dt) {
    double t0 = re_time_seconds();
    uint32_t playing = 0;
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++) {
        Player* p = &Q.players[i];
        if (!p->active) continue;
        float from = p->t, to = p->t + dt * p->rate, len = p->s->length > 0.0f ? p->s->length : 0.0f;
        if (to >= len) {
            if (p->loop && len > 0.0f) { // the end, then from the start (the keys of both stretches fire)
                evaluate(w, p->s, from, len, true, p->bind);
                float st = p->s->start < len ? p->s->start : 0.0f; // (back to the range's start)
                to = st + fmodf(to - len, len - st > 1e-4f ? len - st : len);
                from = st - 1e-4f;
            } else {
                evaluate(w, p->s, from, len, true, p->bind); // it holds its last frame (Unreal's "Keep State")
                p->t = len;
                p->active = false;
                restore_anim(w, p); // (Q23) but the Characters' Animation Blueprints come back
                continue;
            }
        }
        p->t = to;
        evaluate(w, p->s, from, to, true, p->bind);
        playing++;
    }
    Q.stats.playing = playing;
    Q.stats.sequences = Q.ncache;
    Q.stats.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

void seq_end_play(void) {
    render_set_fade(0.0f, NULL); // (B9: a Fade left on goes with the play)
    terror_seq_clear(); // (Q13: and the Horror Filter tracks' filters)
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++) Q.players[i].active = false;
    Q.stats.playing = 0;
}

bool seq_view(World* w, float pos[3], float* yaw, float* pitch, float* fov_deg) {
    for (uint32_t i = 0; i < SEQ_PLAYERS; i++) {
        Player* p = &Q.players[i];
        if (!p->active) continue;
        const SeqTrack* cuts = NULL;
        for (uint32_t ti = 0; ti < p->s->ntracks && !cuts; ti++)
            if (p->s->tracks[ti].type == SEQT_CAMERA_CUTS && p->s->tracks[ti].nkeys) cuts = &p->s->tracks[ti];
        if (!cuts) continue;
        int k = key_before(cuts, p->t);
        if (k < 0) k = 0;
        if (k != p->cam_key) p->cam_key = k, p->cam = seq_find_actor(w, cuts->keys[k].name); // (a search per cut only)
        Entity c = p->cam;
        if (c && !ecs_alive(w, c)) continue;
        const CTransform* x = c ? ecs_get(w, c, COMP_TRANSFORM) : NULL;
        if (!x) continue;
        memcpy(pos, x->location, sizeof x->location);
        *pitch = x->rotation[0], *yaw = x->rotation[1];
        const CCamera* cc = ecs_get(w, c, COMP_CAMERA);
        if (fov_deg) *fov_deg = cc ? cc->fov_deg : 90.0f;
        return true;
    }
    return false;
}

const SeqStats* seq_stats(void) { return &Q.stats; }
