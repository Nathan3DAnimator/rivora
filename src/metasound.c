// MetaSounds (metasound.h, notas-dev/audio.md "MetaSounds"): the assets, the graph sorted into a program, the block
// renderer the mixer thread runs (no allocation) and the engine's own sounds (an engine, the rain, the wind).
#include "metasound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "platform.h"
#include "platform_files.h"

const char* const g_ms_kind_names[MSN_KIND_COUNT] = { "Output", "Input", "Oscillator", "Noise", "Impulses", "Filter", "ADSR Envelope",
                                                       "Multiply", "Add", "Map Range", "Smooth", "Distortion" };
static const char* const in_out[] = { "Signal", "Volume", NULL };
static const char* const in_none[] = { "Default", NULL }; // (an Input: its value until the game sets it)
static const char* const in_osc[] = { "Frequency", "Amplitude", "Offset", "Pulse Width", NULL };
static const char* const in_noise[] = { "Amplitude", NULL };
static const char* const in_imp[] = { "Rate", "Amplitude", "Randomness", NULL };
static const char* const in_filter[] = { "Signal", "Cutoff", "Resonance", NULL };
static const char* const in_adsr[] = { "Gate", "Attack", "Decay", "Sustain", "Release", NULL };
static const char* const in_ab[] = { "A", "B", NULL };
static const char* const in_map[] = { "Value", "In Min", "In Max", "Out Min", "Out Max", NULL };
static const char* const in_smooth[] = { "Value", "Time", NULL };
static const char* const in_dist[] = { "Signal", "Drive", NULL };
const char* const* const g_ms_in_names[MSN_KIND_COUNT] = { in_out, in_none, in_osc, in_noise, in_imp, in_filter, in_adsr, in_ab, in_ab, in_map, in_smooth, in_dist };
static const char* const m_osc[] = { "Sine", "Saw", "Square", "Triangle", NULL };
static const char* const m_noise[] = { "White", "Pink", "Brown", NULL };
static const char* const m_filter[] = { "Low Pass", "High Pass", "Band Pass", NULL };
const char* const* const g_ms_mode_names[MSN_KIND_COUNT] = { NULL, NULL, m_osc, m_noise, NULL, m_filter, NULL, NULL, NULL, NULL, NULL, NULL };

#define MAX_MS 64
static struct {
    char root[700];
    MsAsset* a[MAX_MS];
    uint32_t n;
    uint32_t engine, rain, wind, rain_styled, thunder;
} S;

static int ins_of(int kind) {
    int n = 0;
    while (g_ms_in_names[kind][n]) n++;
    return n;
}

void ms_node_defaults(MsNode* n, int kind) {
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind, n->nin = (uint8_t)ins_of(kind);
    for (int i = 0; i < MS_MAX_IN; i++) n->in[i] = (MsIn){ -1, 0.0f };
    switch (kind) {
    case MSN_OUTPUT: n->in[1].value = 1.0f; break;
    case MSN_INPUT: snprintf(n->name, sizeof n->name, "Input"); break;
    case MSN_OSC: n->in[0].value = 220.0f, n->in[1].value = 0.5f, n->in[3].value = 0.5f; break;
    case MSN_NOISE: n->in[0].value = 0.3f; break;
    case MSN_IMPULSES: n->in[0].value = 20.0f, n->in[1].value = 0.8f, n->in[2].value = 1.0f; break;
    case MSN_FILTER: n->in[1].value = 1000.0f, n->in[2].value = 0.707f; break;
    case MSN_ADSR: n->in[0].value = 1.0f, n->in[1].value = 0.01f, n->in[2].value = 0.1f, n->in[3].value = 0.7f, n->in[4].value = 0.3f; break;
    case MSN_MUL: n->in[1].value = 1.0f; break;
    case MSN_MAP: n->in[2].value = 1.0f, n->in[4].value = 1.0f; break;
    case MSN_SMOOTH: n->in[1].value = 0.05f; break;
    case MSN_DISTORT: n->in[1].value = 2.0f; break;
    default: break;
    }
}

// --- the program: every node after its inputs (a depth-first sort from the Output; a cycle is an error) ---
static bool visit(MsAsset* a, int i, uint8_t* mark) {
    if (mark[i] == 2) return true;
    if (mark[i] == 1) {
        snprintf(a->error, sizeof a->error, "a loop through node %d (%s)", i, g_ms_kind_names[a->n[i].kind]);
        return false;
    }
    mark[i] = 1;
    for (int k = 0; k < a->n[i].nin; k++) {
        int s = a->n[i].in[k].node;
        if (s < 0) continue;
        if (s >= (int)a->nnodes || s == 0) {
            snprintf(a->error, sizeof a->error, "node %d reads a node that isn't there (%d)", i, s);
            return false;
        }
        if (!visit(a, s, mark)) return false;
    }
    mark[i] = 2;
    a->order[a->norder++] = (uint8_t)i;
    return true;
}
bool ms_compile(MsAsset* a) {
    a->error[0] = 0, a->norder = 0, a->nparams = 0;
    if (!a->nnodes || a->n[0].kind != MSN_OUTPUT) {
        snprintf(a->error, sizeof a->error, "no Output (node 0)");
        return false;
    }
    for (uint32_t i = 0; i < a->nnodes; i++)
        if (a->n[i].kind == MSN_INPUT && a->nparams < MS_MAX_PARAMS) a->param_node[a->nparams++] = (uint8_t)i;
    uint8_t mark[MS_MAX_NODES] = { 0 };
    bool ok = visit(a, 0, mark);
    // the wave the mixer plays: mono, at its rate, endless (the graph says when it is silent; Stop ends it)
    memset(&a->wave, 0, sizeof a->wave);
    snprintf(a->wave.name, sizeof a->wave.name, "%s", a->name);
    a->wave.hash = a->hash;
    a->wave.h.channels = 1, a->wave.h.sample_rate = 48000, a->wave.h.frames = 0x7FFFFFFF, a->wave.h.flags = SND_LOOPING;
    a->wave.h.volume = a->volume > 0.0f ? a->volume : 1.0f, a->wave.h.pitch = 1.0f, a->wave.h.priority = 1.0f;
    a->wave.h.sound_class = a->sound_class, a->wave.h.attenuation = a->attenuation;
    static const int16_t silence[4];
    a->wave.pcm = silence; // (never read: the voice renders the graph)
    a->wave.length = 1e9;
    a->wave.ms = ok ? a : NULL;
    return ok;
}

int ms_param_index(const MsAsset* a, const char* name) {
    for (uint32_t i = 0; a && name && i < a->nparams; i++)
        if (!_stricmp(a->n[a->param_node[i]].name, name)) return (int)i;
    return -1;
}

// --- the renderer (the mixer thread) ---
void ms_reset(MsState* st, const MsAsset* a) {
    memset(st, 0, sizeof *st);
    st->a = a;
    for (int i = 0; i < MS_MAX_NODES; i++) st->s[i].rng = 0x9E3779B9u * (uint32_t)(i + 1) ^ (uint32_t)(uintptr_t)st;
    for (uint32_t p = 0; a && p < a->nparams; p++) st->pv[p] = a->n[a->param_node[p]].in[0].value; // (the input's default)
}
static inline float rnd(uint32_t* s) { // 0..1
    *s ^= *s << 13, *s ^= *s >> 17, *s ^= *s << 5;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

// each node's output for the block (n <= 256): the program's scratch (the mixer renders one voice at a time)
static float g_buf[MS_MAX_NODES][256];

static inline float in_at(const MsNode* nd, int k, uint32_t i) { return nd->in[k].node >= 0 ? g_buf[nd->in[k].node][i] : nd->in[k].value; }

void ms_render(MsState* st, const float* params, float* out, uint32_t n, float rate, float pitch) {
    const MsAsset* a = st->a;
    if (!a || !a->norder || n > 256) {
        memset(out, 0, n * sizeof *out);
        return;
    }
    const float inv_rate = 1.0f / rate;
    for (uint32_t oi = 0; oi < a->norder; oi++) {
        int ni = a->order[oi];
        const MsNode* nd = &a->n[ni];
        MsNodeState* s = &st->s[ni];
        float* y = g_buf[ni];
        switch (nd->kind) {
        case MSN_INPUT: { // the parameter's value, glided over the block from the last one (no zipper noise)
            int p = 0;
            while (p < (int)a->nparams && a->param_node[p] != ni) p++;
            float to = p < (int)a->nparams && params ? params[p] : nd->in[0].value, from = p < MS_MAX_PARAMS ? st->pv[p] : to;
            for (uint32_t i = 0; i < n; i++) y[i] = from + (to - from) * (float)(i + 1) / (float)n;
            if (p < MS_MAX_PARAMS) st->pv[p] = to;
            break;
        }
        case MSN_OSC:
            for (uint32_t i = 0; i < n; i++) {
                float f = in_at(nd, 0, i) * pitch, amp = in_at(nd, 1, i), off = in_at(nd, 2, i), pw = in_at(nd, 3, i);
                float ph = (float)s->phase, v;
                switch (nd->mode) {
                case 1: v = 2.0f * ph - 1.0f; break;                         // saw
                case 2: v = ph < (pw > 0.01f && pw < 0.99f ? pw : 0.5f) ? 1.0f : -1.0f; break; // square
                case 3: v = 4.0f * fabsf(ph - 0.5f) - 1.0f; break;          // triangle
                default: v = sinf(6.2831853f * ph); break;                    // sine
                }
                y[i] = off + amp * v;
                s->phase += (double)(f * inv_rate);
                s->phase -= floor(s->phase);
            }
            break;
        case MSN_NOISE:
            for (uint32_t i = 0; i < n; i++) {
                float w = rnd(&s->rng) * 2.0f - 1.0f, v = w;
                if (nd->mode == 1) { // pink (Paul Kellet's economy filter)
                    s->z1 = 0.99765f * s->z1 + w * 0.0990460f, s->z2 = 0.96300f * s->z2 + w * 0.2965164f, s->env = 0.57000f * s->env + w * 1.0526913f;
                    v = (s->z1 + s->z2 + s->env + w * 0.1848f) * 0.25f;
                } else if (nd->mode == 2) { // brown: integrated, kept from wandering off
                    s->y = (s->y + 0.02f * w) * 0.998f;
                    v = s->y * 3.5f;
                }
                y[i] = v * in_at(nd, 0, i);
            }
            break;
        case MSN_IMPULSES: // a click when a random draw says so (Rate a second on average), each its own height
            for (uint32_t i = 0; i < n; i++) {
                float rate_hz = fmaxf(in_at(nd, 0, i), 0.0f), amp = in_at(nd, 1, i), rv = fminf(fmaxf(in_at(nd, 2, i), 0.0f), 1.0f);
                y[i] = rnd(&s->rng) < rate_hz * inv_rate ? amp * (1.0f - rv * rnd(&s->rng)) * (rnd(&s->rng) < 0.5f ? -1.0f : 1.0f) : 0.0f;
            }
            break;
        case MSN_FILTER: { // a biquad (RBJ), its coefficients once a block from the block's first cutoff and Q
            float fc = fminf(fmaxf(in_at(nd, 1, 0), 10.0f), rate * 0.45f), q = fmaxf(in_at(nd, 2, 0), 0.1f);
            float w0 = 6.2831853f * fc * inv_rate, cw = cosf(w0), al = sinf(w0) / (2.0f * q), a0 = 1.0f + al;
            float b0, b1, b2;
            if (nd->mode == 1) b0 = (1.0f + cw) * 0.5f, b1 = -(1.0f + cw), b2 = b0;
            else if (nd->mode == 2) b0 = al, b1 = 0.0f, b2 = -al;
            else b0 = (1.0f - cw) * 0.5f, b1 = 1.0f - cw, b2 = b0;
            b0 /= a0, b1 /= a0, b2 /= a0;
            float a1 = -2.0f * cw / a0, a2 = (1.0f - al) / a0;
            for (uint32_t i = 0; i < n; i++) { // (transposed direct form II)
                float x = in_at(nd, 0, i), o = b0 * x + s->z1;
                s->z1 = b1 * x - a1 * o + s->z2, s->z2 = b2 * x - a2 * o;
                y[i] = o;
            }
            break;
        }
        case MSN_ADSR:
            for (uint32_t i = 0; i < n; i++) {
                bool g = in_at(nd, 0, i) >= 0.5f;
                if (g && !s->gate) s->stage = 1; // attack
                if (!g && s->gate) s->stage = 4; // release
                s->gate = g;
                float at = fmaxf(in_at(nd, 1, i), 1e-4f), dc = fmaxf(in_at(nd, 2, i), 1e-4f), su = in_at(nd, 3, i), rl = fmaxf(in_at(nd, 4, i), 1e-4f);
                if (s->stage == 1 && (s->env += inv_rate / at) >= 1.0f) s->env = 1.0f, s->stage = 2;
                else if (s->stage == 2 && (s->env -= inv_rate / dc * (1.0f - su)) <= su) s->env = su, s->stage = 3;
                else if (s->stage == 4 && (s->env -= inv_rate / rl) <= 0.0f) s->env = 0.0f, s->stage = 0;
                y[i] = s->env;
            }
            break;
        case MSN_MUL: for (uint32_t i = 0; i < n; i++) y[i] = in_at(nd, 0, i) * in_at(nd, 1, i); break;
        case MSN_ADD: for (uint32_t i = 0; i < n; i++) y[i] = in_at(nd, 0, i) + in_at(nd, 1, i); break;
        case MSN_MAP:
            for (uint32_t i = 0; i < n; i++) {
                float v = in_at(nd, 0, i), a0 = in_at(nd, 1, i), a1 = in_at(nd, 2, i), b0 = in_at(nd, 3, i), b1 = in_at(nd, 4, i);
                float t = a1 != a0 ? fminf(fmaxf((v - a0) / (a1 - a0), 0.0f), 1.0f) : 0.0f;
                y[i] = b0 + (b1 - b0) * t;
            }
            break;
        case MSN_SMOOTH:
            for (uint32_t i = 0; i < n; i++) {
                float k = 1.0f - expf(-inv_rate / fmaxf(in_at(nd, 1, i), 1e-4f));
                s->y += (in_at(nd, 0, i) - s->y) * k;
                y[i] = s->y;
            }
            break;
        case MSN_DISTORT:
            for (uint32_t i = 0; i < n; i++) {
                float d = fmaxf(in_at(nd, 1, i), 0.01f);
                y[i] = tanhf(in_at(nd, 0, i) * d) / tanhf(d);
            }
            break;
        case MSN_OUTPUT:
            for (uint32_t i = 0; i < n; i++) out[i] = fminf(fmaxf(in_at(nd, 0, i) * in_at(nd, 1, i), -1.0f), 1.0f);
            break;
        default: memset(y, 0, n * sizeof *y); break;
        }
    }
}

// --- the assets: JSON ---
static void write_ms(const MsAsset* a, JsonWriter* w) {
    jw_obj_begin(w);
    jw_kint(w, "version", 1);
    jw_kfloat(w, "volume", a->volume);
    jw_kint(w, "sound_class", a->sound_class), jw_kint(w, "attenuation", a->attenuation);
    jw_key(w, "nodes");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < a->nnodes; i++) {
        const MsNode* n = &a->n[i];
        jw_obj_begin(w);
        jw_kstr(w, "kind", g_ms_kind_names[n->kind]);
        if (n->mode) jw_kint(w, "mode", n->mode);
        if (n->name[0]) jw_kstr(w, "name", n->name);
        jw_kfloat(w, "x", n->x), jw_kfloat(w, "y", n->y);
        jw_key(w, "in");
        jw_arr_begin(w);
        for (int k = 0; k < n->nin; k++) {
            if (n->in[k].node >= 0) jw_int(w, -1000 - n->in[k].node); // (a node: -1000 - its index)
            else jw_float(w, n->in[k].value);
        }
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}
static void read_ms(MsAsset* a, const JsonValue* r) {
    a->volume = (float)json_get_num(r, "volume", 1.0);
    a->sound_class = (uint32_t)json_get_num(r, "sound_class", 0), a->attenuation = (uint32_t)json_get_num(r, "attenuation", 0);
    const JsonValue* ns = json_get(r, "nodes");
    a->nnodes = 0;
    for (size_t i = 0; i < json_len(ns) && a->nnodes < MS_MAX_NODES; i++) {
        const JsonValue* o = json_at(ns, i);
        const char* k = json_get_str(o, "kind", "");
        int kind = MSN_KIND_COUNT;
        for (int q = 0; q < MSN_KIND_COUNT; q++)
            if (!strcmp(k, g_ms_kind_names[q])) kind = q;
        if (kind == MSN_KIND_COUNT) kind = MSN_ADD; // (an unknown kind: passes A + B, nothing breaks)
        MsNode* n = &a->n[a->nnodes++];
        ms_node_defaults(n, kind);
        n->mode = (uint8_t)json_get_int(o, "mode", 0);
        snprintf(n->name, sizeof n->name, "%s", json_get_str(o, "name", n->name));
        n->x = (float)json_get_num(o, "x", 0.0), n->y = (float)json_get_num(o, "y", 0.0);
        const JsonValue* in = json_get(o, "in");
        for (size_t q = 0; q < json_len(in) && q < n->nin; q++) {
            double v = json_num(json_at(in, q), 0.0);
            if (v <= -1000.0 && v == floor(v)) n->in[q].node = (int8_t)(-1000.0 - v), n->in[q].value = 0.0f;
            else n->in[q].value = (float)v;
        }
    }
}

static void name_of(MsAsset* a, const char* vpath) {
    const char* b = strrchr(vpath, '/');
    snprintf(a->name, sizeof a->name, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(a->name, '.');
    if (dot) *dot = 0;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content/", 9)) snprintf(out, cap, "%s/%s", S.root, vpath + 9);
    else snprintf(out, cap, "%s", vpath);
}

bool ms_save(MsAsset* a) {
    if (!a || !a->path[0]) return false;
    JsonWriter w;
    jw_init(&w);
    write_ms(a, &w);
    bool ok = !w.fail;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", a->path);
    FILE* f = ok ? re_fopen(tmp, "wb") : NULL;
    ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && re_replace_file(tmp, a->path);
    jw_free(&w);
    return ok;
}

static MsAsset* load_disk(const char* disk, const char* vpath) {
    FILE* f = re_fopen(disk, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // (a load, not a frame)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc doc;
    char err[128];
    ok = ok && json_parse(t, (size_t)n, &doc, err, sizeof err);
    free(t);
    if (!ok) return NULL;
    MsAsset* a = calloc(1, sizeof *a); // (kept: the mixer's voices point at it)
    if (a) {
        snprintf(a->path, sizeof a->path, "%s", disk);
        a->hash = audio_vpath_hash(vpath);
        name_of(a, vpath);
        read_ms(a, &doc.root);
        if (!ms_compile(a)) printf("[RealEngine] Sound Graph %s: %s\n", a->name, a->error);
    }
    json_free(&doc);
    return a;
}

typedef struct WalkCtx { char dir[700], vdir[700]; } WalkCtx;
static void walk(const char* dir, const char* vdir);
static void walk_one(void* user, const ReDirEntry* e) {
    WalkCtx* c = user;
    char p[900], v[900];
    snprintf(p, sizeof p, "%s/%s", c->dir, e->name), snprintf(v, sizeof v, "%s/%s", c->vdir, e->name);
    if (e->is_dir) {
        if (e->name[0] != '.') walk(p, v);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rmsnd") || S.n >= MAX_MS) return;
    uint32_t h = audio_vpath_hash(v);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.a[i]->hash == h) return;
    MsAsset* a = load_disk(p, v);
    if (a) S.a[S.n++] = a;
}
static void walk(const char* dir, const char* vdir) {
    WalkCtx c;
    snprintf(c.dir, sizeof c.dir, "%s", dir), snprintf(c.vdir, sizeof c.vdir, "%s", vdir);
    re_dir_walk(dir, walk_one, &c);
}

MsAsset* ms_create(const char* vpath) {
    if (S.n >= MAX_MS) return NULL;
    MsAsset* a = calloc(1, sizeof *a);
    if (!a) return NULL;
    disk_of(vpath, a->path, sizeof a->path);
    a->hash = audio_vpath_hash(vpath);
    name_of(a, vpath);
    a->volume = 1.0f, a->nnodes = 2;
    ms_node_defaults(&a->n[0], MSN_OUTPUT);
    ms_node_defaults(&a->n[1], MSN_OSC);
    a->n[0].in[0].node = 1, a->n[1].x = -260.0f;
    ms_compile(a);
    char dir[700];
    snprintf(dir, sizeof dir, "%s", a->path);
    char* cut = strrchr(dir, '/');
    if (cut) *cut = 0, re_make_dirs(dir);
    if (!ms_save(a)) {
        free(a);
        return NULL;
    }
    S.a[S.n++] = a;
    return a;
}

// --- the engine's own: made when missing (Content/Audio/MetaSounds) ---
static int add(MsAsset* a, int kind, int mode) {
    MsNode* n = &a->n[a->nnodes];
    ms_node_defaults(n, kind);
    n->mode = (uint8_t)mode, n->x = -200.0f * (float)(a->nnodes % 5) - 200.0f, n->y = 90.0f * (float)(a->nnodes / 5);
    return (int)a->nnodes++;
}
static void set(MsAsset* a, int node, int in, float v) { a->n[node].in[in] = (MsIn){ -1, v }; }
static void wire(MsAsset* a, int from, int node, int in) { a->n[node].in[in] = (MsIn){ (int8_t)from, 0.0f }; }
static int input(MsAsset* a, const char* name, float def) {
    int i = add(a, MSN_INPUT, 0);
    snprintf(a->n[i].name, sizeof a->n[i].name, "%s", name);
    a->n[i].in[0].value = def;
    return i;
}
static uint32_t make(const char* rel, void (*build)(MsAsset* a)) {
    char vpath[300], disk[900];
    snprintf(vpath, sizeof vpath, "/Content/Audio/MetaSounds/%s", rel);
    uint32_t h = audio_vpath_hash(vpath);
    for (uint32_t i = 0; i < S.n; i++)
        if (S.a[i]->hash == h) return h;
    disk_of(vpath, disk, sizeof disk);
    if (re_file_stat(disk, NULL, NULL, NULL)) return h; // (on disk: the walk reads it)
    MsAsset* a = ms_create(vpath);
    if (!a) return 0;
    a->nnodes = 1; // (the Output only)
    ms_node_defaults(&a->n[0], MSN_OUTPUT);
    build(a);
    if (!ms_compile(a)) printf("[RealEngine] Sound Graph %s: %s\n", a->name, a->error);
    ms_save(a);
    return h;
}
// An engine: the firing (RPM / 60 x cylinders / 2) as a saw and its octave below, a square an octave up for the rasp,
// all through a low pass that opens with the load; a filtered noise for the intake; the load drives a soft distortion
static void build_engine(MsAsset* a) {
    int rpm = input(a, "RPM", 900.0f), load = input(a, "Load", 0.2f);
    int rs = add(a, MSN_SMOOTH, 0);
    wire(a, rpm, rs, 0), set(a, rs, 1, 0.08f);
    int fire = add(a, MSN_MAP, 0); // (a 4-cylinder: 2 firings a turn; 600-8000 RPM -> 20-266 Hz)
    wire(a, rs, fire, 0), set(a, fire, 1, 600.0f), set(a, fire, 2, 8000.0f), set(a, fire, 3, 20.0f), set(a, fire, 4, 266.7f);
    int o1 = add(a, MSN_OSC, 1);
    wire(a, fire, o1, 0), set(a, o1, 1, 0.5f);
    int half = add(a, MSN_MUL, 0);
    wire(a, fire, half, 0), set(a, half, 1, 0.5f);
    int o2 = add(a, MSN_OSC, 0);
    wire(a, half, o2, 0), set(a, o2, 1, 0.45f);
    int dbl = add(a, MSN_MUL, 0);
    wire(a, fire, dbl, 0), set(a, dbl, 1, 2.0f);
    int o3 = add(a, MSN_OSC, 2);
    wire(a, dbl, o3, 0), set(a, o3, 1, 0.15f), set(a, o3, 3, 0.3f);
    int s12 = add(a, MSN_ADD, 0);
    wire(a, o1, s12, 0), wire(a, o2, s12, 1);
    int s123 = add(a, MSN_ADD, 0);
    wire(a, s12, s123, 0), wire(a, o3, s123, 1);
    int ls = add(a, MSN_SMOOTH, 0);
    wire(a, load, ls, 0), set(a, ls, 1, 0.12f);
    int cut = add(a, MSN_MAP, 0); // (the load opens the low pass: 300 Hz idle to 2.4 kHz full)
    wire(a, ls, cut, 0), set(a, cut, 1, 0.0f), set(a, cut, 2, 1.0f), set(a, cut, 3, 300.0f), set(a, cut, 4, 2400.0f);
    int lp = add(a, MSN_FILTER, 0);
    wire(a, s123, lp, 0), wire(a, cut, lp, 1), set(a, lp, 2, 1.4f);
    int nz = add(a, MSN_NOISE, 1);
    set(a, nz, 0, 0.25f);
    int bp = add(a, MSN_FILTER, 2);
    wire(a, nz, bp, 0), wire(a, dbl, bp, 1), set(a, bp, 2, 2.0f);
    int mix = add(a, MSN_ADD, 0);
    wire(a, lp, mix, 0), wire(a, bp, mix, 1);
    int drive = add(a, MSN_MAP, 0);
    wire(a, ls, drive, 0), set(a, drive, 1, 0.0f), set(a, drive, 2, 1.0f), set(a, drive, 3, 1.2f), set(a, drive, 4, 3.5f);
    int dist = add(a, MSN_DISTORT, 0);
    wire(a, mix, dist, 0), wire(a, drive, dist, 1);
    int vol = add(a, MSN_MAP, 0); // (louder with the load)
    wire(a, ls, vol, 0), set(a, vol, 1, 0.0f), set(a, vol, 2, 1.0f), set(a, vol, 3, 0.35f), set(a, vol, 4, 0.8f);
    wire(a, dist, 0, 0), wire(a, vol, 0, 1);
}
// The rain: a pink noise band (the hiss of many drops far) and drops (impulses through a band pass), both with the
// intensity (mm/h: 1 drizzle .. 100 storm)
static void build_rain(MsAsset* a) {
    int in = input(a, "Intensity", 20.0f);
    int sm = add(a, MSN_SMOOTH, 0);
    wire(a, in, sm, 0), set(a, sm, 1, 0.5f);
    int amt = add(a, MSN_MAP, 0);
    wire(a, sm, amt, 0), set(a, amt, 1, 0.0f), set(a, amt, 2, 80.0f), set(a, amt, 3, 0.0f), set(a, amt, 4, 1.0f);
    int nz = add(a, MSN_NOISE, 1);
    wire(a, amt, nz, 0);
    int hp = add(a, MSN_FILTER, 1);
    wire(a, nz, hp, 0), set(a, hp, 1, 900.0f), set(a, hp, 2, 0.6f);
    int lp = add(a, MSN_FILTER, 0);
    wire(a, hp, lp, 0), set(a, lp, 1, 7000.0f), set(a, lp, 2, 0.7f);
    int rate = add(a, MSN_MAP, 0); // (drops a second near: 5 to 300)
    wire(a, sm, rate, 0), set(a, rate, 1, 0.0f), set(a, rate, 2, 80.0f), set(a, rate, 3, 5.0f), set(a, rate, 4, 300.0f);
    int imp = add(a, MSN_IMPULSES, 0);
    wire(a, rate, imp, 0), set(a, imp, 1, 0.9f), set(a, imp, 2, 0.8f);
    int drop = add(a, MSN_FILTER, 2);
    wire(a, imp, drop, 0), set(a, drop, 1, 2600.0f), set(a, drop, 2, 3.0f);
    int mix = add(a, MSN_ADD, 0);
    wire(a, lp, mix, 0), wire(a, drop, mix, 1);
    wire(a, mix, 0, 0), set(a, 0, 1, 0.8f);
}
// (2026-11-14, "sobre o som da chuva também ter opções") the rain with its style's knobs: Tone (the hiss's top, Hz),
// Drop Pitch (the drops' band, Hz), Drop Rate (x), Drop Level, Muffle (0 open .. 1 under a thick roof: a low pass down to
// 500 Hz) and Volume. A new asset (MS_RainStyled): a project's MS_Rain saved before keeps its own inputs
static void build_rain_styled(MsAsset* a) {
    int in = input(a, "Intensity", 20.0f), tone = input(a, "Tone", 7000.0f), dp = input(a, "Drop Pitch", 2600.0f);
    int dr = input(a, "Drop Rate", 1.0f), dl = input(a, "Drop Level", 1.0f), mu = input(a, "Muffle", 0.0f), vo = input(a, "Volume", 0.8f);
    int sm = add(a, MSN_SMOOTH, 0);
    wire(a, in, sm, 0), set(a, sm, 1, 0.5f);
    int amt = add(a, MSN_MAP, 0);
    wire(a, sm, amt, 0), set(a, amt, 1, 0.0f), set(a, amt, 2, 80.0f), set(a, amt, 3, 0.0f), set(a, amt, 4, 1.0f);
    int nz = add(a, MSN_NOISE, 1);
    wire(a, amt, nz, 0);
    int hp = add(a, MSN_FILTER, 1);
    wire(a, nz, hp, 0), set(a, hp, 1, 900.0f), set(a, hp, 2, 0.6f);
    int lp = add(a, MSN_FILTER, 0);
    wire(a, hp, lp, 0), wire(a, tone, lp, 1), set(a, lp, 2, 0.7f);
    int rate = add(a, MSN_MAP, 0); // (drops a second near: 5 to 300, x the style's rate)
    wire(a, sm, rate, 0), set(a, rate, 1, 0.0f), set(a, rate, 2, 80.0f), set(a, rate, 3, 5.0f), set(a, rate, 4, 300.0f);
    int rr = add(a, MSN_MUL, 0);
    wire(a, rate, rr, 0), wire(a, dr, rr, 1);
    int imp = add(a, MSN_IMPULSES, 0);
    wire(a, rr, imp, 0), set(a, imp, 1, 0.9f), set(a, imp, 2, 0.8f);
    int drop = add(a, MSN_FILTER, 2);
    wire(a, imp, drop, 0), wire(a, dp, drop, 1), set(a, drop, 2, 3.0f);
    int dg = add(a, MSN_MUL, 0);
    wire(a, drop, dg, 0), wire(a, dl, dg, 1);
    int mix = add(a, MSN_ADD, 0);
    wire(a, lp, mix, 0), wire(a, dg, mix, 1);
    int mc = add(a, MSN_MAP, 0); // (the muffle: the low pass from 16 kHz open down to 500 Hz)
    wire(a, mu, mc, 0), set(a, mc, 1, 0.0f), set(a, mc, 2, 1.0f), set(a, mc, 3, 16000.0f), set(a, mc, 4, 500.0f);
    int ml = add(a, MSN_FILTER, 0);
    wire(a, mix, ml, 0), wire(a, mc, ml, 1), set(a, ml, 2, 0.7f);
    wire(a, ml, 0, 0), wire(a, vo, 0, 1);
}
// (2026-11-14) thunder: Rumble (0..1, the envelope audio_world.c drives: a strike, then rolling away) through a deep brown
// noise and a crackle (impulses through a band pass) at the start, saturated a little; Volume
static void build_thunder(MsAsset* a) {
    int ru = input(a, "Rumble", 0.0f), vo = input(a, "Volume", 1.0f);
    int sm = add(a, MSN_SMOOTH, 0);
    wire(a, ru, sm, 0), set(a, sm, 1, 0.04f);
    int nz = add(a, MSN_NOISE, 2);
    wire(a, sm, nz, 0);
    int lp = add(a, MSN_FILTER, 0);
    wire(a, nz, lp, 0), set(a, lp, 1, 140.0f), set(a, lp, 2, 0.9f);
    int lg = add(a, MSN_MUL, 0); // (the rumble is low: a gain back)
    wire(a, lp, lg, 0), set(a, lg, 1, 3.0f);
    int sq = add(a, MSN_MUL, 0); // (the crackle only near the top of the envelope: rumble squared)
    wire(a, sm, sq, 0), wire(a, sm, sq, 1);
    int rate = add(a, MSN_MAP, 0);
    wire(a, sq, rate, 0), set(a, rate, 1, 0.0f), set(a, rate, 2, 1.0f), set(a, rate, 3, 0.0f), set(a, rate, 4, 60.0f);
    int imp = add(a, MSN_IMPULSES, 0);
    wire(a, rate, imp, 0), wire(a, sq, imp, 1), set(a, imp, 2, 1.0f);
    int bp = add(a, MSN_FILTER, 2);
    wire(a, imp, bp, 0), set(a, bp, 1, 700.0f), set(a, bp, 2, 1.2f);
    int mix = add(a, MSN_ADD, 0);
    wire(a, lg, mix, 0), wire(a, bp, mix, 1);
    int ds = add(a, MSN_DISTORT, 0);
    wire(a, mix, ds, 0), set(a, ds, 1, 1.6f);
    wire(a, ds, 0, 0), wire(a, vo, 0, 1);
}
// The wind: a brown noise through a band pass whose center wanders slowly (gusts), louder and higher with the speed (m/s)
static void build_wind(MsAsset* a) {
    int in = input(a, "Speed", 8.0f);
    int sm = add(a, MSN_SMOOTH, 0);
    wire(a, in, sm, 0), set(a, sm, 1, 1.0f);
    int nz = add(a, MSN_NOISE, 2);
    set(a, nz, 0, 1.0f);
    int lfo = add(a, MSN_OSC, 0); // (gusts: a slow wander of the band)
    set(a, lfo, 0, 0.13f), set(a, lfo, 1, 0.35f), set(a, lfo, 2, 1.0f);
    int base = add(a, MSN_MAP, 0);
    wire(a, sm, base, 0), set(a, base, 1, 0.0f), set(a, base, 2, 25.0f), set(a, base, 3, 250.0f), set(a, base, 4, 1100.0f);
    int fc = add(a, MSN_MUL, 0);
    wire(a, base, fc, 0), wire(a, lfo, fc, 1);
    int bp = add(a, MSN_FILTER, 2);
    wire(a, nz, bp, 0), wire(a, fc, bp, 1), set(a, bp, 2, 1.6f);
    int vol = add(a, MSN_MAP, 0);
    wire(a, sm, vol, 0), set(a, vol, 1, 0.0f), set(a, vol, 2, 25.0f), set(a, vol, 3, 0.0f), set(a, vol, 4, 1.0f);
    int g = add(a, MSN_MUL, 0);
    wire(a, bp, g, 0), wire(a, lfo, g, 1);
    wire(a, g, 0, 0), wire(a, vol, 0, 1);
}

void ms_init(const char* content_root) {
    for (uint32_t i = 0; i < S.n; i++) free(S.a[i]); // (another project: the old ones go; no voice plays them now)
    S.n = 0;
    snprintf(S.root, sizeof S.root, "%s", content_root ? content_root : "");
    if (!S.root[0]) return;
    walk(S.root, "/Content");
    S.engine = make("MS_Engine.rmsnd", build_engine);
    S.rain = make("MS_Rain.rmsnd", build_rain);
    S.rain_styled = make("MS_RainStyled.rmsnd", build_rain_styled);
    S.thunder = make("MS_Thunder.rmsnd", build_thunder);
    S.wind = make("MS_Wind.rmsnd", build_wind);
    walk(S.root, "/Content"); // (what make() found on disk without loading)
}

const MsAsset* ms_find(uint32_t hash) {
    for (uint32_t i = 0; hash && i < S.n; i++)
        if (S.a[i]->hash == hash) return S.a[i]->wave.ms ? S.a[i] : NULL;
    return NULL;
}
MsAsset* ms_find_edit(uint32_t hash) {
    for (uint32_t i = 0; hash && i < S.n; i++)
        if (S.a[i]->hash == hash) return S.a[i];
    return NULL;
}
uint32_t ms_count(void) { return S.n; }
MsAsset* ms_at(uint32_t i) { return i < S.n ? S.a[i] : NULL; }
uint32_t ms_by_name(const char* name) {
    for (uint32_t i = 0; name && i < S.n; i++)
        if (!_stricmp(S.a[i]->name, name)) return S.a[i]->hash;
    return 0;
}
uint32_t ms_engine_sound(void) { return S.engine; }
uint32_t ms_rain_sound(void) { return S.rain; }
uint32_t ms_rain_styled_sound(void) { return S.rain_styled; }
uint32_t ms_thunder_sound(void) { return S.thunder; }
uint32_t ms_wind_sound(void) { return S.wind; }
