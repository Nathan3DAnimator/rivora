// Timelines: the data, the curves, the .rbp's part and the Timeline editor (bp_timeline.h, notas-dev/blueprints.md)
#include "bp_timeline.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "io_json.h"


const char* const g_tl_track_type_names[TLT_COUNT] = { "Float", "Vector", "Linear Color", "Event" };
const char* const g_tl_interp_names[TLI_COUNT] = { "Auto", "Linear", "Constant" };

static BpTimelines* tls_of(BpAsset* a, bool make) {
    if (!a->tl && make) a->tl = calloc(1, sizeof *a->tl); // (an edit: the class gets its first timeline)
    return a->tl;
}

BpTimeline* bp_timeline_find(BpAsset* a, const char* name) {
    for (int depth = 0; a && depth < 8; depth++, a = bp_find_name(BPA_CLASS, a->parent)) {
        BpTimelines* s = a->tl;
        for (uint32_t i = 0; s && i < s->n; i++)
            if (!strcmp(s->t[i].name, name)) return &s->t[i];
        if (!a->parent[0]) break;
    }
    return NULL;
}

BpTlTrack* bp_timeline_add_track(BpTimeline* tl, uint8_t type) {
    if (tl->ntracks >= BP_TL_TRACKS || type >= TLT_COUNT) return NULL;
    BpTlTrack* tr = &tl->tracks[tl->ntracks++];
    memset(tr, 0, sizeof *tr);
    tr->type = type;
    static const char* base[TLT_COUNT] = { "NewTrack", "NewVector", "NewColor", "NewEvent" };
    for (int k = 0; k < 100; k++) { // a name not used yet
        char nm[BP_NAME];
        snprintf(nm, sizeof nm, k ? "%s_%d" : "%s", base[type], k);
        bool used = false;
        for (uint8_t i = 0; i + 1 < tl->ntracks; i++) used |= !strcmp(tl->tracks[i].name, nm);
        if (!used) {
            snprintf(tr->name, sizeof tr->name, "%s", nm);
            break;
        }
    }
    return tr;
}

BpTimeline* bp_timeline_add(BpAsset* a) {
    BpTimelines* s = tls_of(a, true);
    if (!s || s->n >= BP_TL_MAX) return NULL;
    BpTimeline* tl = &s->t[s->n];
    memset(tl, 0, sizeof *tl);
    for (int k = 0; k < 100; k++) {
        char nm[BP_NAME];
        snprintf(nm, sizeof nm, "Timeline_%d", k);
        if (!bp_timeline_find(a, nm)) {
            snprintf(tl->name, sizeof tl->name, "%s", nm);
            break;
        }
    }
    tl->length = 1.0f, tl->rate = 1.0f;
    BpTlTrack* tr = bp_timeline_add_track(tl, TLT_FLOAT);
    snprintf(tr->name, sizeof tr->name, "Alpha");
    float v0[4] = { 0 }, v1[4] = { 1, 1, 1, 1 };
    bp_timeline_add_key(tr, 0.0f, v0);
    bp_timeline_add_key(tr, 1.0f, v1);
    s->n++;
    a->dirty = true;
    return tl;
}

bool bp_timeline_remove(BpAsset* a, const char* name) {
    BpTimelines* s = a->tl;
    for (uint32_t i = 0; s && i < s->n; i++)
        if (!strcmp(s->t[i].name, name)) {
            memmove(&s->t[i], &s->t[i + 1], sizeof s->t[0] * (s->n - i - 1));
            s->n--;
            a->dirty = true;
            return true;
        }
    return false;
}

float bp_timeline_length(const BpTimeline* tl) {
    if (!tl->use_last_key) return tl->length > 0.0f ? tl->length : 0.0f;
    float m = 0.0f;
    for (uint8_t i = 0; i < tl->ntracks; i++)
        if (tl->tracks[i].nkeys) m = fmaxf(m, tl->tracks[i].keys[tl->tracks[i].nkeys - 1].t);
    return m;
}

static int key_cmp(const void* x, const void* y) {
    float a = ((const BpTlKey*)x)->t, b = ((const BpTlKey*)y)->t;
    return a < b ? -1 : a > b;
}
void bp_timeline_sort(BpTlTrack* tr) { qsort(tr->keys, tr->nkeys, sizeof tr->keys[0], key_cmp); }

int bp_timeline_add_key(BpTlTrack* tr, float t, const float v[4]) {
    if (tr->nkeys >= BP_TL_KEYS) return -1;
    BpTlKey* k = &tr->keys[tr->nkeys++];
    memset(k, 0, sizeof *k);
    k->t = t;
    if (v) memcpy(k->v, v, sizeof k->v);
    if (tr->type == TLT_COLOR && !v) k->v[3] = 1.0f;
    bp_timeline_sort(tr);
    for (int i = 0; i < tr->nkeys; i++)
        if (tr->keys[i].t == t) return i;
    return -1;
}

// The curve between keys i and i+1 at u (0..1): cubic Hermite with auto (Catmull-Rom) tangents, like Unreal's Auto
static float hermite(const BpTlTrack* tr, int i, int c, float u) {
    const BpTlKey *k0 = &tr->keys[i], *k1 = &tr->keys[i + 1];
    float dt = fmaxf(k1->t - k0->t, 1e-6f);
    float p0 = k0->v[c], p1 = k1->v[c];
    // the tangents (per second): the neighbours' slope; flat at the ends
    float m0 = i > 0 ? (p1 - tr->keys[i - 1].v[c]) / fmaxf(k1->t - tr->keys[i - 1].t, 1e-6f) : 0.0f;
    float m1 = i + 2 < tr->nkeys ? (tr->keys[i + 2].v[c] - p0) / fmaxf(tr->keys[i + 2].t - k0->t, 1e-6f) : 0.0f;
    float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 * dt + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1 * dt;
}

void bp_timeline_eval(const BpTlTrack* tr, float t, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f, out[3] = tr->type == TLT_COLOR ? 1.0f : 0.0f;
    if (!tr->nkeys || tr->type == TLT_EVENT) return;
    int nc = tr->type == TLT_FLOAT ? 1 : tr->type == TLT_VECTOR ? 3 : 4;
    if (t <= tr->keys[0].t) {
        memcpy(out, tr->keys[0].v, sizeof(float) * (size_t)nc);
        return;
    }
    if (t >= tr->keys[tr->nkeys - 1].t) {
        memcpy(out, tr->keys[tr->nkeys - 1].v, sizeof(float) * (size_t)nc);
        return;
    }
    int lo = 0, hi = tr->nkeys - 1; // (the key before t: a binary search)
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (tr->keys[mid].t <= t) lo = mid;
        else hi = mid;
    }
    const BpTlKey *k0 = &tr->keys[lo], *k1 = &tr->keys[lo + 1];
    float u = (t - k0->t) / fmaxf(k1->t - k0->t, 1e-6f);
    for (int c = 0; c < nc; c++)
        out[c] = k0->interp == TLI_CONSTANT ? k0->v[c] : k0->interp == TLI_LINEAR ? k0->v[c] + (k1->v[c] - k0->v[c]) * u : hermite(tr, lo, c, u);
}

// --- the .rbp ---
void bp_timelines_write(struct JsonWriter* w, const BpAsset* a) {
    const BpTimelines* s = a->tl;
    if (!s || !s->n) return;
    jw_key(w, "timelines");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < s->n; i++) {
        const BpTimeline* tl = &s->t[i];
        jw_obj_begin(w);
        jw_kstr(w, "name", tl->name);
        jw_kfloat(w, "length", tl->length);
        jw_kfloat(w, "rate", tl->rate);
        jw_kbool(w, "loop", tl->loop), jw_kbool(w, "autoplay", tl->autoplay), jw_kbool(w, "use_last_key", tl->use_last_key);
        jw_key(w, "tracks");
        jw_arr_begin(w);
        for (uint8_t k = 0; k < tl->ntracks; k++) {
            const BpTlTrack* tr = &tl->tracks[k];
            jw_obj_begin(w);
            jw_kstr(w, "name", tr->name);
            jw_kint(w, "type", tr->type);
            jw_key(w, "keys");
            jw_arr_begin(w);
            for (uint8_t j = 0; j < tr->nkeys; j++) { // [t, v0, v1, v2, v3, interp]
                const BpTlKey* key = &tr->keys[j];
                float f[6] = { key->t, key->v[0], key->v[1], key->v[2], key->v[3], (float)key->interp };
                jw_arr_begin(w);
                for (int q = 0; q < 6; q++) jw_float(w, f[q]);
                jw_arr_end(w);
            }
            jw_arr_end(w);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

void bp_timelines_read(const struct JsonValue* root, BpAsset* a) {
    const JsonValue* arr = json_get(root, "timelines");
    if (!json_len(arr)) return;
    BpTimelines* s = tls_of(a, true);
    if (!s) return;
    s->n = 0;
    for (size_t i = 0; i < json_len(arr) && s->n < BP_TL_MAX; i++) {
        const JsonValue* o = json_at(arr, i);
        BpTimeline* tl = &s->t[s->n++];
        memset(tl, 0, sizeof *tl);
        snprintf(tl->name, sizeof tl->name, "%s", json_get_str(o, "name", "Timeline"));
        tl->length = (float)json_get_num(o, "length", 1.0);
        tl->rate = (float)json_get_num(o, "rate", 1.0);
        tl->loop = json_get_bool(o, "loop", false), tl->autoplay = json_get_bool(o, "autoplay", false);
        tl->use_last_key = json_get_bool(o, "use_last_key", false);
        const JsonValue* trs = json_get(o, "tracks");
        for (size_t k = 0; k < json_len(trs) && tl->ntracks < BP_TL_TRACKS; k++) {
            const JsonValue* t = json_at(trs, k);
            BpTlTrack* tr = &tl->tracks[tl->ntracks++];
            memset(tr, 0, sizeof *tr);
            snprintf(tr->name, sizeof tr->name, "%s", json_get_str(t, "name", "Track"));
            tr->type = (uint8_t)json_get_int(t, "type", TLT_FLOAT);
            if (tr->type >= TLT_COUNT) tr->type = TLT_FLOAT;
            const JsonValue* ks = json_get(t, "keys");
            for (size_t j = 0; j < json_len(ks) && tr->nkeys < BP_TL_KEYS; j++) {
                const JsonValue* kv = json_at(ks, j);
                BpTlKey* key = &tr->keys[tr->nkeys++];
                key->t = (float)json_num(json_at(kv, 0), 0.0);
                for (int q = 0; q < 4; q++) key->v[q] = (float)json_num(json_at(kv, (size_t)q + 1), 0.0);
                int ip = (int)json_num(json_at(kv, 5), 0.0);
                key->interp = (uint8_t)(ip >= 0 && ip < TLI_COUNT ? ip : 0);
            }
            bp_timeline_sort(tr);
        }
    }
}

void bp_timelines_free(BpAsset* a) {
    free(a->tl);
    a->tl = NULL;
}

// --- self-test (the test bp_timeline: bp_vm's play is in bp_test) ---
int bp_timeline_selftest(void) {
    int fail = 0;
    BpTlTrack tr;
    memset(&tr, 0, sizeof tr);
    tr.type = TLT_FLOAT;
    float a[4] = { 0 }, b[4] = { 10 }, c[4] = { 0 };
    bp_timeline_add_key(&tr, 2.0f, c), bp_timeline_add_key(&tr, 0.0f, a), bp_timeline_add_key(&tr, 1.0f, b);
    float o[4];
    if (tr.keys[0].t != 0.0f || tr.keys[2].t != 2.0f) fail++, printf("[RealEngine] timeline: keys not in order\n");
    bp_timeline_eval(&tr, 1.0f, o);
    if (fabsf(o[0] - 10.0f) > 1e-4f) fail++, printf("[RealEngine] timeline: at a key %g (want 10)\n", o[0]);
    tr.keys[0].interp = TLI_LINEAR;
    bp_timeline_eval(&tr, 0.25f, o);
    if (fabsf(o[0] - 2.5f) > 1e-4f) fail++, printf("[RealEngine] timeline: linear %g (want 2.5)\n", o[0]);
    tr.keys[0].interp = TLI_CONSTANT;
    bp_timeline_eval(&tr, 0.9f, o);
    if (fabsf(o[0]) > 1e-4f) fail++, printf("[RealEngine] timeline: constant %g (want 0)\n", o[0]);
    tr.keys[0].interp = TLI_AUTO;
    bp_timeline_eval(&tr, 0.5f, o);
    if (!(o[0] > 4.0f && o[0] < 9.0f)) fail++, printf("[RealEngine] timeline: auto %g (want between)\n", o[0]);
    bp_timeline_eval(&tr, -1.0f, o);
    float o2[4];
    bp_timeline_eval(&tr, 5.0f, o2);
    if (o[0] != 0.0f || o2[0] != 0.0f) fail++, printf("[RealEngine] timeline: outside the keys %g %g\n", o[0], o2[0]);
    // speed: a million evaluations of a 64-key curve
    BpTlTrack big;
    memset(&big, 0, sizeof big);
    big.type = TLT_VECTOR;
    for (int i = 0; i < BP_TL_KEYS; i++) {
        float v[4] = { (float)i, (float)(i % 3), (float)(i * i % 7), 0 };
        bp_timeline_add_key(&big, (float)i * 0.1f, v);
    }
    double t0 = (double)clock();
    float acc = 0.0f;
    for (int i = 0; i < 1000000; i++) {
        bp_timeline_eval(&big, (float)(i % 6400) * 0.001f, o);
        acc += o[0];
    }
    double ms = ((double)clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("[RealEngine] timeline: 1M evaluations of a 64-key vector track in %.1f ms (%.0f ns each) %s\n", ms, ms, acc > 0 ? "" : " ");
    printf(fail ? "[RealEngine] timeline selftest: %d FAILED\n" : "[RealEngine] timeline selftest: ok\n", fail);
    return fail;
}
