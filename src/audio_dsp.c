// The mixer's effects (audio_dsp.h): biquads (RBJ's cookbook), a feedback delay network reverb, a 4-band EQ, a
// compressor, a delay, a filter and the master's look-ahead limiter. All on blocks of 2 to 8 channels, in place.
#include "audio_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979f

static float db_to_lin(float db) { return powf(10.0f, db * 0.05f); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// --- Biquads ---
static void bq_set(AuBiquad* f, double b0, double b1, double b2, double a0, double a1, double a2) {
    f->b0 = (float)(b0 / a0), f->b1 = (float)(b1 / a0), f->b2 = (float)(b2 / a0);
    f->a1 = (float)(a1 / a0), f->a2 = (float)(a2 / a0);
}
void au_biquad_lowpass(AuBiquad* f, float hz, float q, float rate) {
    double w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w), al = sin(w) / (2.0 * (q > 0.05f ? q : 0.7071f));
    bq_set(f, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
void au_biquad_highpass(AuBiquad* f, float hz, float q, float rate) {
    double w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w), al = sin(w) / (2.0 * (q > 0.05f ? q : 0.7071f));
    bq_set(f, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
void au_biquad_bandpass(AuBiquad* f, float hz, float q, float rate) {
    double w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w), al = sin(w) / (2.0 * (q > 0.05f ? q : 0.7071f));
    bq_set(f, al, 0, -al, 1 + al, -2 * c, 1 - al);
}
void au_biquad_peak(AuBiquad* f, float hz, float q, float db, float rate) {
    double A = pow(10.0, db / 40.0), w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w);
    double al = sin(w) / (2.0 * (q > 0.05f ? q : 0.7071f));
    bq_set(f, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
void au_biquad_lowshelf(AuBiquad* f, float hz, float db, float rate) {
    double A = pow(10.0, db / 40.0), w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w), s = sin(w);
    double al = s / 2.0 * sqrt(2.0), sa = 2.0 * sqrt(A) * al;
    bq_set(f, A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
           (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
void au_biquad_highshelf(AuBiquad* f, float hz, float db, float rate) {
    double A = pow(10.0, db / 40.0), w = 2.0 * PI_F * clampf(hz, 10.0f, rate * 0.49f) / rate, c = cos(w), s = sin(w);
    double al = s / 2.0 * sqrt(2.0), sa = 2.0 * sqrt(A) * al;
    bq_set(f, A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
           (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}
void au_biquad_run(AuBiquad* f, float* const* ch, int nch, uint32_t n) { // (transposed direct form II)
    for (int c = 0; c < nch && c < AU_MAX_CH; c++) {
        float z1 = f->z1[c], z2 = f->z2[c], *x = ch[c];
        for (uint32_t i = 0; i < n; i++) {
            float in = x[i], out = f->b0 * in + z1;
            z1 = f->b1 * in - f->a1 * out + z2;
            z2 = f->b2 * in - f->a2 * out;
            x[i] = out;
        }
        // (denormals: a silent tail decays into them; flushed)
        f->z1[c] = fabsf(z1) < 1e-20f ? 0.0f : z1, f->z2[c] = fabsf(z2) < 1e-20f ? 0.0f : z2;
    }
}

// --- The reverb: 8 delay lines (Jot's feedback delay network: a Householder matrix), each with a low pass in its loop
// (the high frequencies decay faster), pre-delay, early reflections from taps of the pre-delay, input diffusion
// (allpasses); the lengths scaled by the room's size ---
#define FDN 8
#define FDN_MAX 8192   // samples per line (~170 ms at 48 kHz: the biggest room)
#define PRE_MAX 9600   // pre-delay up to 200 ms
#define AP_N 4
#define AP_MAX 1024
static const float g_fdn_ms[FDN] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.3f }; // (mutually prime-ish)
static const float g_ap_ms[AP_N] = { 4.77f, 3.59f, 12.73f, 9.31f };
static const float g_er_ms[8] = { 7.1f, 11.3f, 17.9f, 23.3f, 31.7f, 41.9f, 53.1f, 61.7f };
static const float g_er_gain[8] = { 0.84f, -0.72f, 0.63f, -0.55f, 0.47f, -0.39f, 0.31f, -0.25f };

typedef struct Reverb {
    float line[FDN][FDN_MAX];
    uint32_t len[FDN], pos[FDN];
    float lp[FDN], g[FDN], damp[FDN];
    float pre[PRE_MAX];
    uint32_t pre_pos;
    float ap[AP_N][AP_MAX];
    uint32_t ap_len[AP_N], ap_pos[AP_N];
    float key[AU_FX_PARAMS]; // the parameters the coefficients were made for
} Reverb;

static void reverb_setup(Reverb* v, const float* p, float rate) {
    if (!memcmp(v->key, p, sizeof v->key)) return;
    float decay = clampf(p[0], 0.1f, 20.0f), hf = clampf(p[1], 0.05f, 1.0f), size = clampf(p[2], 0.05f, 1.0f);
    for (int k = 0; k < FDN; k++) {
        uint32_t l = (uint32_t)(g_fdn_ms[k] * (0.35f + 1.65f * size) * rate * 0.001f);
        v->len[k] = l < 16 ? 16 : l >= FDN_MAX ? FDN_MAX - 1 : l;
        if (v->pos[k] >= v->len[k]) v->pos[k] = 0;
        float t = (float)v->len[k] / rate;
        v->g[k] = powf(10.0f, -3.0f * t / decay);                   // -60 dB after decay seconds
        float g_hf = powf(10.0f, -3.0f * t / (decay * hf));         // the highs' shorter decay
        float r = g_hf / v->g[k];                                   // the loop's low pass gain at Nyquist (relative)
        v->damp[k] = clampf(1.0f - r, 0.0f, 0.95f);                 // (one pole: more damping, darker)
    }
    for (int k = 0; k < AP_N; k++) {
        uint32_t l = (uint32_t)(g_ap_ms[k] * rate * 0.001f);
        v->ap_len[k] = l < 8 ? 8 : l >= AP_MAX ? AP_MAX - 1 : l;
        if (v->ap_pos[k] >= v->ap_len[k]) v->ap_pos[k] = 0;
    }
    memcpy(v->key, p, sizeof v->key);
}

static void reverb_run(Reverb* v, const float* p, float rate, float* const* ch, const uint8_t* roles, int nch, uint32_t n) {
    reverb_setup(v, p, rate);
    uint32_t pre = (uint32_t)(clampf(p[3], 0.0f, 199.0f) * rate * 0.001f);
    float wet = clampf(p[4], 0.0f, 4.0f), dry = clampf(p[5], 0.0f, 4.0f), early = clampf(p[6], 0.0f, 2.0f);
    float diff = clampf(p[7], 0.0f, 0.8f);
    uint32_t er_tap[8];
    for (int k = 0; k < 8; k++) er_tap[k] = (uint32_t)(g_er_ms[k] * (0.35f + 1.65f * clampf(p[2], 0.05f, 1.0f)) * rate * 0.001f) % PRE_MAX;
    int nin = 0;
    for (int c = 0; c < nch; c++) nin += roles[c] != AU_SPK_LFE;
    float in_scale = nin ? 1.0f / (float)nin : 0.0f;
    bool surround = false;
    for (int c = 0; c < nch; c++) surround |= roles[c] >= AU_SPK_BL && roles[c] <= AU_SPK_SR;
    for (uint32_t i = 0; i < n; i++) {
        float in = 0.0f;
        for (int c = 0; c < nch; c++)
            if (roles[c] != AU_SPK_LFE) in += ch[c][i];
        in *= in_scale;
        v->pre[v->pre_pos] = in;
        uint32_t rp = (v->pre_pos + PRE_MAX - pre) % PRE_MAX;
        float x = v->pre[rp];
        // early reflections: taps after the pre-delay, alternating sides
        float el = 0.0f, er = 0.0f;
        for (int k = 0; k < 8; k++) {
            float t = v->pre[(rp + PRE_MAX - er_tap[k]) % PRE_MAX] * g_er_gain[k];
            if (k & 1) er += t;
            else el += t;
        }
        v->pre_pos = (v->pre_pos + 1) % PRE_MAX;
        // input diffusion (Schroeder allpasses)
        for (int k = 0; k < AP_N; k++) {
            float* b = v->ap[k];
            float d = b[v->ap_pos[k]], y = d - diff * x;
            b[v->ap_pos[k]] = x + diff * y;
            v->ap_pos[k] = (v->ap_pos[k] + 1) % v->ap_len[k];
            x = y;
        }
        // the network: the lines' outputs, mixed by Householder (I - 2/N 11^T), damped, fed back with the input
        float o[FDN], sum = 0.0f;
        for (int k = 0; k < FDN; k++) {
            o[k] = v->line[k][v->pos[k]];
            sum += o[k];
        }
        sum *= 2.0f / FDN;
        float yl = 0.0f, yr = 0.0f, ylb = 0.0f, yrb = 0.0f; // (front: lines 0-3; back: 4-7; left: even, right: odd)
        for (int k = 0; k < FDN; k++) {
            float fb = (o[k] - sum) * v->g[k];
            v->lp[k] += (1.0f - v->damp[k]) * (fb - v->lp[k]);
            float nv = v->lp[k] + x * 0.35f;
            v->line[k][v->pos[k]] = fabsf(nv) < 1e-20f ? 0.0f : nv;
            v->pos[k] = v->pos[k] + 1 >= v->len[k] ? 0 : v->pos[k] + 1;
            float* side = k < 4 ? ((k & 1) ? &yr : &yl) : ((k & 1) ? &yrb : &ylb);
            *side += o[k];
        }
        // each speaker its own lines (a diffuse field: uncorrelated around the room); stereo: all of them
        for (int c = 0; c < nch; c++) {
            float y;
            switch (roles[c]) {
            case AU_SPK_FL: y = (surround ? yl * 1.41f : yl + ylb) * 0.35f + el * early * 0.25f; break;
            case AU_SPK_FR: y = (surround ? yr * 1.41f : yr + yrb) * 0.35f + er * early * 0.25f; break;
            case AU_SPK_BL: y = ylb * 1.41f * 0.35f + el * early * 0.18f; break;
            case AU_SPK_BR: y = yrb * 1.41f * 0.35f + er * early * 0.18f; break;
            case AU_SPK_SL: y = (yl - ylb) * 0.35f + el * early * 0.2f; break;
            case AU_SPK_SR: y = (yr - yrb) * 0.35f + er * early * 0.2f; break;
            case AU_SPK_FC: y = (el + er) * early * 0.12f; break; // (the center: little reverb, Unreal's too)
            default: y = 0.0f; break;                          // (the LFE: none)
            }
            ch[c][i] = ch[c][i] * dry + y * wet;
        }
    }
}

// --- The delay ---
#define DLY_MAX 96000 // 2 s at 48 kHz
typedef struct Delay {
    float buf[AU_MAX_CH][DLY_MAX];
    uint32_t pos;
    float lp[AU_MAX_CH];
} Delay;

static void delay_run(Delay* d, const float* p, float rate, float* const* ch, int nch, uint32_t n) {
    uint32_t t = (uint32_t)(clampf(p[0], 1.0f, 1990.0f) * rate * 0.001f);
    if (t >= DLY_MAX) t = DLY_MAX - 1;
    float fb = clampf(p[1], 0.0f, 0.95f), wet = clampf(p[2], 0.0f, 2.0f), dry = clampf(p[3], 0.0f, 2.0f);
    float a = 1.0f - expf(-2.0f * PI_F * clampf(p[4] > 0 ? p[4] : 20000.0f, 50.0f, 20000.0f) / rate);
    bool ping = p[5] > 0.5f && nch >= 2; // (ping-pong: between the front pair; the others echo in place)
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rp = (d->pos + DLY_MAX - t) % DLY_MAX;
        float y[AU_MAX_CH];
        for (int c = 0; c < nch; c++) y[c] = d->buf[c][rp], d->lp[c] += a * (y[c] - d->lp[c]);
        for (int c = 0; c < nch; c++) {
            int src = ping && c < 2 ? 1 - c : c;
            float f = d->lp[src] * fb;
            float x = ping && c == 1 ? 0.0f : ch[c][i];
            d->buf[c][d->pos] = x + (fabsf(f) < 1e-20f ? 0.0f : f);
            ch[c][i] = ch[c][i] * dry + y[c] * wet;
        }
        d->pos = (d->pos + 1) % DLY_MAX;
    }
}

// --- The compressor: a peak detector (attack / release), a soft knee, makeup; look-ahead by a short delay ---
#define CMP_LA 960
typedef struct Comp {
    float env, gain;
    float la[AU_MAX_CH][CMP_LA];
    uint32_t pos;
} Comp;

static void comp_run(Comp* c, const float* p, float rate, float* const* ch, int nch, uint32_t n) {
    float th = clampf(p[0], -60.0f, 0.0f), ratio = clampf(p[1], 1.0f, 50.0f);
    float at = expf(-1.0f / (clampf(p[2], 0.05f, 500.0f) * 0.001f * rate)), rel = expf(-1.0f / (clampf(p[3], 1.0f, 5000.0f) * 0.001f * rate));
    float makeup = db_to_lin(clampf(p[4], -24.0f, 24.0f)), knee = clampf(p[5], 0.0f, 24.0f);
    uint32_t la = (uint32_t)(clampf(p[6], 0.0f, 19.0f) * rate * 0.001f);
    if (la >= CMP_LA) la = CMP_LA - 1;
    for (uint32_t i = 0; i < n; i++) {
        float x = 0.0f;
        for (int k = 0; k < nch; k++) x = fmaxf(x, fabsf(ch[k][i]));
        c->env = x > c->env ? at * c->env + (1.0f - at) * x : rel * c->env + (1.0f - rel) * x;
        float lvl = 20.0f * log10f(c->env + 1e-9f), over = lvl - th, red;
        if (knee > 0.0f && fabsf(over) < knee * 0.5f) red = (1.0f / ratio - 1.0f) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
        else red = over > 0.0f ? (1.0f / ratio - 1.0f) * over : 0.0f;
        c->gain = db_to_lin(red);
        uint32_t rp = (c->pos + CMP_LA - la) % CMP_LA;
        for (int k = 0; k < nch; k++) {
            c->la[k][c->pos] = ch[k][i];
            ch[k][i] = c->la[k][rp] * c->gain * makeup;
        }
        c->pos = (c->pos + 1) % CMP_LA;
    }
}

// --- The effect slot ---
struct AuFxState {
    float rate;
    int kind;
    float key[AU_FX_PARAMS];
    AuBiquad bq[4];
    union { Reverb* reverb; Delay* delay; Comp* comp; } u;
    Reverb* reverb;
    Delay* delay;
    Comp* comp;
};

AuFxState* au_fx_create(float rate) {
    AuFxState* s = calloc(1, sizeof *s); // (at init: the effect's lines made when the kind is first set, off the mixer)
    if (s) s->rate = rate;
    return s;
}
void au_fx_destroy(AuFxState* s) {
    if (!s) return;
    free(s->reverb), free(s->delay), free(s->comp), free(s);
}
// The big states made before the mixer needs them (the game thread, when a bus's effects are set)
void au_fx_prepare(AuFxState* s, int kind);
void au_fx_prepare(AuFxState* s, int kind) {
    if (!s) return;
    if (kind == AUFX_REVERB && !s->reverb) s->reverb = calloc(1, sizeof(Reverb));
    if (kind == AUFX_DELAY && !s->delay) s->delay = calloc(1, sizeof(Delay));
    if (kind == AUFX_COMPRESSOR && !s->comp) s->comp = calloc(1, sizeof(Comp));
}

void au_fx_process(AuFxState* s, const AuEffect* e, float* const* ch, const uint8_t* roles, int nch, uint32_t n) {
    if (!s || !e || e->kind == AUFX_NONE) return;
    if (s->kind != e->kind) { // a new kind: its state from silence
        s->kind = e->kind;
        memset(s->key, 0xFF, sizeof s->key);
        memset(s->bq, 0, sizeof s->bq);
        if (s->reverb && e->kind == AUFX_REVERB) memset(s->reverb, 0, sizeof *s->reverb);
        if (s->delay && e->kind == AUFX_DELAY) memset(s->delay, 0, sizeof *s->delay);
        if (s->comp && e->kind == AUFX_COMPRESSOR) memset(s->comp, 0, sizeof *s->comp), s->comp->gain = 1.0f;
    }
    const float* p = e->p;
    switch (e->kind) {
    case AUFX_REVERB:
        if (s->reverb) reverb_run(s->reverb, p, s->rate, ch, roles, nch, n);
        break;
    case AUFX_DELAY:
        if (s->delay) delay_run(s->delay, p, s->rate, ch, nch, n);
        break;
    case AUFX_COMPRESSOR:
        if (s->comp) comp_run(s->comp, p, s->rate, ch, nch, n);
        break;
    case AUFX_EQ: {
        if (memcmp(s->key, p, sizeof s->key)) { // (the coefficients made again only when a parameter changed)
            float z[4][2][AU_MAX_CH];
            for (int k = 0; k < 4; k++) memcpy(z[k][0], s->bq[k].z1, sizeof z[k][0]), memcpy(z[k][1], s->bq[k].z2, sizeof z[k][1]); // (the states kept: no click)
            au_biquad_lowshelf(&s->bq[0], p[0] > 0 ? p[0] : 120.0f, p[1], s->rate);
            au_biquad_peak(&s->bq[1], p[2] > 0 ? p[2] : 1000.0f, p[4] > 0 ? p[4] : 1.0f, p[3], s->rate);
            au_biquad_highshelf(&s->bq[2], p[5] > 0 ? p[5] : 8000.0f, p[6], s->rate);
            for (int k = 0; k < 4; k++) memcpy(s->bq[k].z1, z[k][0], sizeof z[k][0]), memcpy(s->bq[k].z2, z[k][1], sizeof z[k][1]);
            memcpy(s->key, p, sizeof s->key);
        }
        for (int k = 0; k < 3; k++) au_biquad_run(&s->bq[k], ch, nch, n);
        float g = db_to_lin(clampf(p[7], -24.0f, 24.0f));
        if (g != 1.0f)
            for (int c = 0; c < nch; c++)
                for (uint32_t i = 0; i < n; i++) ch[c][i] *= g;
        break;
    }
    case AUFX_FILTER:
        if (memcmp(s->key, p, sizeof s->key)) {
            float z[2][AU_MAX_CH];
            memcpy(z[0], s->bq[0].z1, sizeof z[0]), memcpy(z[1], s->bq[0].z2, sizeof z[1]);
            int t = (int)p[0];
            float hz = p[1] > 0 ? p[1] : 1000.0f, q = p[2] > 0 ? p[2] : 0.7071f;
            if (t == 1) au_biquad_highpass(&s->bq[0], hz, q, s->rate);
            else if (t == 2) au_biquad_bandpass(&s->bq[0], hz, q, s->rate);
            else au_biquad_lowpass(&s->bq[0], hz, q, s->rate);
            memcpy(s->bq[0].z1, z[0], sizeof z[0]), memcpy(s->bq[0].z2, z[1], sizeof z[1]);
            memcpy(s->key, p, sizeof s->key);
        }
        au_biquad_run(&s->bq[0], ch, nch, n);
        break;
    default: break;
    }
}

void au_effect_defaults(AuEffect* e, int kind) {
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind;
    switch (kind) {
    case AUFX_REVERB: { const float d[8] = { 1.8f, 0.6f, 0.5f, 20.0f, 0.35f, 1.0f, 0.6f, 0.6f }; memcpy(e->p, d, sizeof d); break; }
    case AUFX_EQ: { const float d[8] = { 120.0f, 0.0f, 1000.0f, 0.0f, 1.0f, 8000.0f, 0.0f, 0.0f }; memcpy(e->p, d, sizeof d); break; }
    case AUFX_COMPRESSOR: { const float d[8] = { -18.0f, 4.0f, 5.0f, 120.0f, 0.0f, 6.0f, 2.0f, 0.0f }; memcpy(e->p, d, sizeof d); break; }
    case AUFX_DELAY: { const float d[8] = { 350.0f, 0.35f, 0.4f, 1.0f, 6000.0f, 0.0f, 0.0f, 0.0f }; memcpy(e->p, d, sizeof d); break; }
    case AUFX_FILTER: { const float d[8] = { 0.0f, 2000.0f, 0.7071f, 0, 0, 0, 0, 0 }; memcpy(e->p, d, sizeof d); break; }
    default: break;
    }
}

// --- The limiter: the signal delayed by the look-ahead; the gain follows the coming peaks down within it (no overs)
// and recovers slowly (no pumping) ---
#define LIM_LA 128
struct AuLimiter {
    float rate, ceiling, gain, rel;
    float buf[AU_MAX_CH][LIM_LA], peak[LIM_LA];
    uint32_t pos, la;
};
AuLimiter* au_limiter_create(float rate, float ceiling_db) {
    AuLimiter* L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->rate = rate, L->ceiling = db_to_lin(ceiling_db), L->gain = 1.0f;
    L->rel = expf(-1.0f / (0.12f * rate));
    L->la = (uint32_t)(0.0015f * rate);
    if (L->la >= LIM_LA) L->la = LIM_LA - 1;
    if (L->la < 8) L->la = 8;
    return L;
}
void au_limiter_destroy(AuLimiter* L) { free(L); }
float au_limiter_process(AuLimiter* L, float* const* ch, int nch, uint32_t n) {
    float minred = 1.0f;
    for (uint32_t i = 0; i < n; i++) {
        float x = 0.0f;
        for (int c = 0; c < nch; c++) x = fmaxf(x, fabsf(ch[c][i])), L->buf[c][L->pos] = ch[c][i];
        L->peak[L->pos] = x;
        // the gain the loudest sample within the look-ahead needs
        float need = 1.0f;
        for (uint32_t k = 0; k < L->la; k++) {
            float pk = L->peak[(L->pos + LIM_LA - k) % LIM_LA];
            if (pk * need > L->ceiling) need = L->ceiling / pk;
        }
        if (need < L->gain) L->gain += (need - L->gain) * 0.5f; // (fast down: reached within the look-ahead)
        else L->gain = need + (L->gain - need) * L->rel;        // (slow up)
        if (L->gain > 1.0f) L->gain = 1.0f;
        uint32_t rp = (L->pos + LIM_LA - L->la) % LIM_LA;
        for (int c = 0; c < nch; c++) ch[c][i] = clampf(L->buf[c][rp] * L->gain, -L->ceiling, L->ceiling); // (the last guard)
        L->pos = (L->pos + 1) % LIM_LA;
        if (L->gain < minred) minred = L->gain;
    }
    return 20.0f * log10f(minred);
}
