// The audio mixer (audio.h): the device's thread mixes the voices into the buses (submixes with effects, the reverb
// send, the master with its limiter); the game thread starts and stops voices through a queue and publishes their
// parameters every frame (spatialization, attenuation, classes, mixes, concurrency, virtualization).
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <intrin.h>
typedef CRITICAL_SECTION AuLock;
#define au_lock_init(l) InitializeCriticalSection(l)
#define au_lock_free(l) DeleteCriticalSection(l)
#define au_enter(l) EnterCriticalSection(l)
#define au_leave(l) LeaveCriticalSection(l)
#define au_popcnt(x) __popcnt(x)
#else // Android: AAudio (notas-dev/android.md); the lock recursive as a critical section
#include <aaudio/AAudio.h>
#include <pthread.h>
#include "re_sync.h"
typedef pthread_mutex_t AuLock;
static void au_lock_init(AuLock* l) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(l, &a);
    pthread_mutexattr_destroy(&a);
}
#define au_lock_free(l) pthread_mutex_destroy(l)
#define au_enter(l) pthread_mutex_lock(l)
#define au_leave(l) pthread_mutex_unlock(l)
#define au_popcnt(x) __builtin_popcount(x)
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"
#include "metasound.h"
#include "audio_internal.h"
#include "platform.h"
#include "platform_files.h"

#define PI_F 3.14159265358979f
#define AU_VOICES 256        // mixer voices (real + virtual)
#define AU_SOUNDS 256        // active sounds (a cue's may hold several voices)
#define AU_SOUND_VOICES AU_PLAN_WAVES
#define AU_CMDS 1024
#define AU_ITD 64            // the binaural delay line (samples: > 0.66 ms at 96 kHz)
#define AU_MIXES 16          // Sound Mixes active at once (pushed, passive, the base)
#define SPEED_OF_SOUND 34300.0f // cm/s
#define LFE_BLEED 0.25f
#ifdef _WIN32 // the share of a 3D sound sent to the subwoofer (its low pass: 120 Hz at the master)

// The GUIDs (named apart from the SDK's: no uuid.lib needed)
static const GUID s_clsid_enum = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID s_iid_enum = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID s_iid_client = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID s_iid_render = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID s_float_sub = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID s_pcm_sub = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
#endif

void audio_waves_root(const char* root);
void au_fx_prepare(AuFxState* s, int kind);

// --- What the game thread hands the mixer ---
enum { CMD_START = 1, CMD_STOP };
typedef struct Cmd {
    uint32_t op, slot, gen, delay;  // delay: output frames before it sounds
    const SoundWave* w;
    double pos;                     // source frames
    uint32_t loop;
} Cmd;
typedef struct VoiceParams {        // written by the game thread every frame, read by the mixer every block
    float gain, pitch, az, el, spatial, spread, lpf, hpf, send;
    uint32_t bus;
    uint8_t binaural, virt, paused, pad;
} VoiceParams;
typedef struct BusDesc {
    uint32_t parent;
    float volume;
    AuEffect fx[AU_BUS_FX];
    uint32_t nfx;
    uint8_t used, pad[3];
} BusDesc;

// --- The mixer's own voice ---
typedef struct MixVoice {
    const SoundWave* w;
    const int16_t* pcm;
    uint32_t ch, frames, loop_s, loop_e, gen, delay;
    double pos;
    uint8_t state, loop, fading_out, started; // state 0 free, 1 playing
    float g[2][AU_MAX_CH]; // per emitter: each output channel's gain now (ramped to the target over a block)
    float lp[2], hp[2];   // the source channels' one-pole filters
    float send;
    float itd_buf[2][AU_ITD];
    uint32_t itd_pos;
    float itd[2][2];      // per emitter: the left and right ears' delays now (samples)
    float hs_a[2][2];     // per emitter, per ear: the head shadow's alpha now
    float hs_x1[2][2], hs_y1[2][2];
} MixVoice;

static struct {
    bool on, quit, silent, manual, com;
    ReThread* thread;
    AuLock lock;                    // the mixing (the device's thread, a test's audio_render_test)
    uint32_t rate, period;          // frames per second, per device period
    // the queue (one writer: the game thread; one reader: the mixer)
    Cmd cmds[AU_CMDS];
    volatile int32_t head, tail;
    VoiceParams params[AU_VOICES];
    volatile int32_t ended[AU_VOICES];   // the generation of the voice the mixer let go of
    volatile int32_t played[AU_VOICES];  // its position (source frames)
    volatile int32_t blocks;             // blocks mixed (audio_forget_wave waits on it)
    // buses: the game thread's descriptions, copied by the mixer when their version changes
    BusDesc bus_game[AU_MAX_BUSES], bus_mix[AU_MAX_BUSES];
    volatile int32_t bus_version, bus_lock;
    int32_t bus_seen;
    uint32_t bus_order[AU_MAX_BUSES], nbus_order, bus_order_mix[AU_MAX_BUSES], nbus_order_mix;
    uint32_t bus_hash[AU_MAX_BUSES];
    AuFxState* fx[AU_MAX_BUSES][AU_BUS_FX];
    AuLimiter* limiter;
    float master_volume, user_volume; // master: the user's (audio_set_master_volume) times the project's
    // the mixer's state
    MixVoice v[AU_VOICES];
    MsState ms[AU_VOICES];               // a MetaSound voice's graph state (B8; the mixer's)
    float msp[AU_VOICES][MS_MAX_PARAMS]; // its inputs as the game thread sets them (audio_set_param)
    float bus[AU_MAX_BUSES][AU_MAX_CH][AU_BLOCK];
    float tmp[2][AU_BLOCK];
    // the output's speakers (the device's channel mask, in its order): their roles, their angles (radians, + right),
    // the horizontal ones sorted by angle (VBAP's pairs); the LFE's low pass
    uint32_t nch, mask;
    uint8_t role[AU_MAX_CH];
    float spk_az[AU_MAX_CH];
    int vb_n, vb_idx[AU_MAX_CH], lfe;
    AuBiquad lfe_lp;
    char layout[24];
    // measures
    double mix_acc, mix_max, mix_max_shown, mix_mean, stat_t0, frames_acc;
    volatile int32_t peak_bits[AU_MAX_CH], lim_bits;
    char device[96];
    volatile int32_t nreal, nvirt;
} M;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// --- The queue ---
static bool push_cmd(const Cmd* c) {
    int32_t h = re_atomic_load(&M.head), t = re_atomic_load(&M.tail);
    if (h - t >= AU_CMDS) return false; // (full: dropped; 1024 starts in one frame)
    M.cmds[h % AU_CMDS] = *c;
    re_atomic_store(&M.head, h + 1);
    return true;
}

// --- Mixing ---
static void voice_free(MixVoice* v, uint32_t slot) {
    v->state = 0, v->w = NULL, v->pcm = NULL;
    re_atomic_store(&M.ended[slot], (int32_t)v->gen);
}

static void run_cmds(void) {
    int32_t t = re_atomic_load(&M.tail), h = re_atomic_load(&M.head);
    for (; t != h; t++) {
        const Cmd* c = &M.cmds[t % AU_CMDS];
        if (c->slot >= AU_VOICES) continue;
        MixVoice* v = &M.v[c->slot];
        if (c->op == CMD_START) {
            memset(v, 0, sizeof *v);
            v->w = c->w, v->pcm = c->w->pcm, v->ch = c->w->h.channels, v->frames = c->w->h.frames;
            v->loop = (uint8_t)c->loop;
            v->loop_s = c->w->h.loop_start, v->loop_e = c->w->h.loop_end ? c->w->h.loop_end : c->w->h.frames;
            v->pos = c->pos, v->delay = c->delay, v->gen = c->gen, v->state = 1;
            if (c->w->ms) ms_reset(&M.ms[c->slot], c->w->ms); // (B8: a MetaSound's graph from its start)
            if (!v->pcm || !v->frames) voice_free(v, c->slot);
        } else if (c->op == CMD_STOP && v->state && v->gen == c->gen) {
            v->fading_out = 1; // (ramped to silence over this block, then let go)
        }
    }
    re_atomic_store(&M.tail, t);
}

// One source frame of channel c at a position (looped or past the end: silence)
static inline float smp(const MixVoice* v, int64_t i, uint32_t c) {
    if (v->loop) {
        int64_t span = (int64_t)v->loop_e - v->loop_s;
        if (i >= (int64_t)v->loop_e && span > 0) i = v->loop_s + (i - v->loop_s) % span;
    }
    if (i < 0 || i >= (int64_t)v->frames) return 0.0f;
    return v->pcm[(size_t)i * v->ch + c] * (1.0f / 32768.0f);
}

// The voice's source resampled (cubic Hermite) into tmp (n frames); false: it ended in this block (the rest silent)
static bool fetch(MixVoice* v, float step, uint32_t n) {
    uint32_t ch = v->ch;
    for (uint32_t i = 0; i < n; i++) {
        int64_t ip = (int64_t)v->pos;
        float f = (float)(v->pos - (double)ip);
        for (uint32_t c = 0; c < ch; c++) {
            float xm, x0, x1, x2;
            if (ip >= 1 && ip + 2 < (int64_t)v->frames && (!v->loop || ip + 2 < (int64_t)v->loop_e)) { // (the fast path)
                const int16_t* p = v->pcm + (size_t)(ip - 1) * ch + c;
                xm = p[0] * (1.0f / 32768.0f), x0 = p[ch] * (1.0f / 32768.0f), x1 = p[2 * ch] * (1.0f / 32768.0f), x2 = p[3 * ch] * (1.0f / 32768.0f);
            } else {
                xm = smp(v, ip - 1, c), x0 = smp(v, ip, c), x1 = smp(v, ip + 1, c), x2 = smp(v, ip + 2, c);
            }
            float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
            M.tmp[c][i] = ((c3 * f + c2) * f + c1) * f + x0;
        }
        v->pos += step;
        if (v->loop && v->pos >= v->loop_e) v->pos -= (double)(v->loop_e - v->loop_s);
        if (v->pos >= v->frames) {
            for (uint32_t k = i + 1; k < n; k++)
                for (uint32_t c = 0; c < ch; c++) M.tmp[c][k] = 0.0f;
            return false;
        }
    }
    return true;
}

// --- The output's layout ---
// A channel mask (WASAPI's: FL FR FC LFE BL BR FLC FRC BC SL SR in bit order) into the mixer's channels: each set bit a
// channel, its role and angle. 5.1's surrounds (back or side bits) at +-110 degrees; 7.1's sides at +-90, backs at +-150
static void set_layout(uint32_t nch, uint32_t mask) {
    static const uint32_t bits[] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x200, 0x400 };
    static const uint8_t roles[] = { AU_SPK_FL, AU_SPK_FR, AU_SPK_FC, AU_SPK_LFE, AU_SPK_BL, AU_SPK_BR, AU_SPK_SL, AU_SPK_SR };
    if (nch < 1 || nch > AU_MAX_CH) nch = 2, mask = 0x3;
    if (!mask || (uint32_t)au_popcnt(mask) != nch) mask = nch == 8 ? 0x63F : nch == 6 ? 0x3F : nch == 4 ? 0x33 : nch == 1 ? 0x4 : 0x3;
    bool sides = (mask & 0x600) != 0, backs = (mask & 0x30) != 0, seven = sides && backs;
    uint32_t c = 0;
    for (int b = 0; b < 8 && c < nch; b++) {
        if (!(mask & bits[b])) continue;
        uint8_t r = roles[b];
        float deg = 0.0f;
        switch (r) {
        case AU_SPK_FL: deg = nch == 4 ? -45.0f : -30.0f; break;
        case AU_SPK_FR: deg = nch == 4 ? 45.0f : 30.0f; break;
        case AU_SPK_FC: deg = 0.0f; break;
        case AU_SPK_BL: deg = seven ? -150.0f : nch == 4 ? -135.0f : -110.0f; break;
        case AU_SPK_BR: deg = seven ? 150.0f : nch == 4 ? 135.0f : 110.0f; break;
        case AU_SPK_SL: deg = seven ? -90.0f : -110.0f; break;
        case AU_SPK_SR: deg = seven ? 90.0f : 110.0f; break;
        default: break;
        }
        M.role[c] = r, M.spk_az[c] = deg * PI_F / 180.0f;
        c++;
    }
    for (; c < nch; c++) M.role[c] = AU_SPK_OTHER, M.spk_az[c] = 0.0f; // (a bit we don't know: silent)
    M.nch = nch, M.mask = mask, M.lfe = -1, M.vb_n = 0;
    for (uint32_t k = 0; k < nch; k++) {
        if (M.role[k] == AU_SPK_LFE) M.lfe = (int)k;
        else if (M.role[k] != AU_SPK_OTHER) M.vb_idx[M.vb_n++] = (int)k;
    }
    for (int i = 1; i < M.vb_n; i++) // (by angle)
        for (int j = i; j > 0 && M.spk_az[M.vb_idx[j]] < M.spk_az[M.vb_idx[j - 1]]; j--) {
            int t = M.vb_idx[j];
            M.vb_idx[j] = M.vb_idx[j - 1], M.vb_idx[j - 1] = t;
        }
    memset(&M.lfe_lp, 0, sizeof M.lfe_lp);
    au_biquad_lowpass(&M.lfe_lp, 120.0f, 0.7071f, (float)(M.rate ? M.rate : 48000));
    snprintf(M.layout, sizeof M.layout, "%s", nch == 2 ? "Stereo" : nch == 6 ? "5.1" : nch == 8 ? "7.1" : nch == 4 ? "Quad" : nch == 1 ? "Mono" : "Multichannel");
}

// Vector-base amplitude panning (Pulkki) on the horizontal speakers: the pair around the angle, gains from the inverse
// of their directions, power normalized; up or down (elevation), toward every speaker evenly
static void vbap(float az, float el, float* g) {
    memset(g, 0, sizeof(float) * AU_MAX_CH);
    int n = M.vb_n;
    if (n <= 1) {
        if (n == 1) g[M.vb_idx[0]] = 1.0f;
        return;
    }
    for (int i = 0; i < n; i++) {
        int ia = M.vb_idx[i], ib = M.vb_idx[(i + 1) % n];
        float a = M.spk_az[ia], b = M.spk_az[ib] + (i == n - 1 ? 2.0f * PI_F : 0.0f), x = az;
        while (x < a) x += 2.0f * PI_F;
        if (x > b) continue;
        float l1x = cosf(a), l1y = sinf(a), l2x = cosf(b), l2y = sinf(b), px = cosf(x), py = sinf(x);
        float det = l1x * l2y - l1y * l2x;
        if (fabsf(det) < 1e-6f) continue;
        float g1 = fmaxf((px * l2y - py * l2x) / det, 0.0f), g2 = fmaxf((l1x * py - l1y * px) / det, 0.0f);
        float nrm = sqrtf(g1 * g1 + g2 * g2);
        if (nrm < 1e-6f) continue;
        g[ia] += g1 / nrm, g[ib] += g2 / nrm;
        break;
    }
    float ce = fabsf(cosf(el)), u = 1.0f / sqrtf((float)n), pw = 0.0f;
    for (int i = 0; i < n; i++) {
        int c = M.vb_idx[i];
        g[c] = g[c] * ce + (1.0f - ce) * u;
        pw += g[c] * g[c];
    }
    pw = pw > 1e-9f ? 1.0f / sqrtf(pw) : 0.0f;
    for (int i = 0; i < n; i++) g[M.vb_idx[i]] *= pw;
}

// The head shadow (Brown & Duda's spherical head): a one-pole one-zero shelf per ear whose alpha falls as the source
// turns away from that ear; the interaural delay (Woodworth). theta: the angle between the source and the ear's axis
static float bd_alpha(float theta) {
    const float amin = 0.1f, tmin = 150.0f * PI_F / 180.0f;
    return (1.0f + amin * 0.5f) + (1.0f - amin * 0.5f) * cosf(theta / tmin * PI_F);
}

static void mix_voice(uint32_t slot, uint32_t n) {
    MixVoice* v = &M.v[slot];
    const VoiceParams* P = &M.params[slot];
    if (P->paused && !v->fading_out) return;
    uint32_t off = 0;
    if (v->delay) { // not started yet
        if (v->delay >= n) {
            v->delay -= n;
            return;
        }
        off = v->delay, v->delay = 0;
    }
    uint32_t m = n - off;
    float step = (float)v->w->h.sample_rate / (float)M.rate * clampf(P->pitch, 0.01f, 8.0f);
    if (P->virt && !v->fading_out && v->w->ms) { // (a MetaSound virtual: nothing rendered, its graph waits)
        memset(v->g, 0, sizeof v->g), v->send = 0.0f;
        return;
    }
    if (P->virt && !v->fading_out) { // virtual: time runs, nothing heard (it comes back from silence: ramped in)
        v->pos += (double)step * m;
        if (v->loop) {
            double span = (double)(v->loop_e - v->loop_s);
            while (span > 0 && v->pos >= v->loop_e) v->pos -= span;
        }
        if (v->pos >= v->frames) voice_free(v, slot);
        memset(v->g, 0, sizeof v->g), v->send = 0.0f;
        return;
    }
    bool more = true;
    if (v->w->ms) ms_render(&M.ms[slot], M.msp[slot], M.tmp[0], m, (float)M.rate, clampf(P->pitch, 0.01f, 8.0f)); // (B8: the graph's block)
    else more = fetch(v, step, m);
    // the source's filters (air absorption, occlusion, the class's low pass; a high pass)
    for (uint32_t c = 0; c < v->ch; c++) {
        if (P->lpf < 19000.0f) {
            float a = 1.0f - expf(-2.0f * PI_F * clampf(P->lpf, 20.0f, 20000.0f) / M.rate), y = v->lp[c];
            for (uint32_t i = 0; i < m; i++) y += a * (M.tmp[c][i] - y), M.tmp[c][i] = y;
            v->lp[c] = fabsf(y) < 1e-20f ? 0.0f : y;
        }
        if (P->hpf > 20.0f) {
            float a = 1.0f - expf(-2.0f * PI_F * clampf(P->hpf, 20.0f, 20000.0f) / M.rate), y = v->hp[c];
            for (uint32_t i = 0; i < m; i++) y += a * (M.tmp[c][i] - y), M.tmp[c][i] -= y;
            v->hp[c] = fabsf(y) < 1e-20f ? 0.0f : y;
        }
    }
    float gain = v->fading_out ? 0.0f : clampf(P->gain, 0.0f, 16.0f);
    float spatial = clampf(P->spatial, 0.0f, 1.0f);
    uint32_t bi = P->bus < AU_MAX_BUSES ? P->bus : 0, nch = M.nch;
    float* out[AU_MAX_CH];
    for (uint32_t c = 0; c < nch; c++) out[c] = M.bus[bi][c] + off;
    float* rv[2] = { M.bus[AU_BUS_REVERB][0] + off, M.bus[AU_BUS_REVERB][1] + off };
    float send_to = v->fading_out ? 0.0f : clampf(P->send, 0.0f, 4.0f) * gain;
    float inv = 1.0f / (float)m;
    int nem = (v->ch == 2 && spatial <= 0.0f) ? 0 : (int)v->ch; // 0: a 2D stereo sound (straight to the two sides)
    if (nem == 0) { // (the front pair: FL and FR are the first two channels of every layout but mono)
        int cr = nch > 1 ? 1 : 0;
        float g0l = v->g[0][0], g0r = v->g[0][cr], g1l = gain, g1r = gain;
        for (uint32_t i = 0; i < m; i++) {
            float t = (float)(i + 1) * inv, gl = g0l + (g1l - g0l) * t, gr = g0r + (g1r - g0r) * t;
            out[0][i] += M.tmp[0][i] * gl, out[cr][i] += M.tmp[1][i] * gr;
        }
        v->g[0][0] = g1l, v->g[0][cr] = g1r;
    } else if (nch > 2) { // surround: VBAP between the speakers around it, the 2D part on the front pair, the LFE's bleed
        for (int e = 0; e < nem; e++) {
            float az = P->az + (nem == 2 ? (e ? P->spread : -P->spread) : 0.0f);
            float g3[AU_MAX_CH], tg[AU_MAX_CH];
            vbap(az, P->el, g3);
            for (uint32_t c = 0; c < nch; c++) {
                float g2 = M.role[c] == AU_SPK_FL || M.role[c] == AU_SPK_FR ? 0.7071f : 0.0f;
                tg[c] = gain * (spatial * g3[c] + (1.0f - spatial) * g2);
            }
            if (M.lfe >= 0) tg[M.lfe] = gain * LFE_BLEED * spatial;
            const float* src = M.tmp[nem == 2 ? e : 0];
            for (uint32_t c = 0; c < nch; c++) {
                float g0 = v->g[e][c], g1 = tg[c];
                if (g0 == 0.0f && g1 == 0.0f) continue;
                float* o = out[c];
                for (uint32_t i = 0; i < m; i++) o[i] += src[i] * (g0 + (g1 - g0) * (float)(i + 1) * inv);
                v->g[e][c] = g1;
            }
        }
    } else {
        for (int e = 0; e < nem; e++) {
            float az = P->az + (nem == 2 ? (e ? P->spread : -P->spread) : 0.0f);
            float saz = sinf(az), caz = cosf(az), cel = cosf(P->el);
            float lat = clampf(saz * cel, -1.0f, 1.0f); // the source toward the right ear (-1 left .. 1 right)
            // equal power panning (the 2D sound: the middle), blended by how spatial it is
            float pan = lat * spatial, a = (pan + 1.0f) * PI_F * 0.25f;
            float tl = cosf(a) * gain, tr = sinf(a) * gain;
            float* src = M.tmp[nem == 2 ? e : 0];
            if (nem == 1 && v->ch == 2) src = M.tmp[0]; // (unreached: a stereo source has two emitters)
            if (!P->binaural || spatial <= 0.0f || nch != 2) { // (binaural: headphones, a stereo output)
                float g0l = v->g[e][0], g0r = v->g[e][1];
                for (uint32_t i = 0; i < m; i++) {
                    float t = (float)(i + 1) * inv, x = src[i];
                    out[0][i] += x * (g0l + (tl - g0l) * t), out[1][i] += x * (g0r + (tr - g0r) * t);
                }
                v->g[e][0] = tl, v->g[e][1] = tr;
                continue;
            }
            // binaural: equal gains (the head's shadow and delay make the side), its delay and shadow per ear
            float g = gain * 0.7071f;
            float th_l = acosf(clampf(-lat, -1.0f, 1.0f)), th_r = acosf(clampf(lat, -1.0f, 1.0f)); // angle to each ear's axis
            float al_t = 1.0f + (bd_alpha(th_l) - 1.0f) * spatial, ar_t = 1.0f + (bd_alpha(th_r) - 1.0f) * spatial;
            float back = caz < 0.0f ? -caz * 0.35f * spatial : 0.0f;          // behind: a little darker (no pinna)
            al_t -= back, ar_t -= back;
            float lt = asinf(fabsf(lat)), itd = 8.75f / SPEED_OF_SOUND * (lt + sinf(lt)) * (float)M.rate * spatial;
            if (itd > AU_ITD - 3) itd = AU_ITD - 3;
            float dl_t = lat > 0.0f ? itd : 0.0f, dr_t = lat < 0.0f ? itd : 0.0f; // (the far ear later)
            float w0 = 2.0f * SPEED_OF_SOUND / 8.75f, K = 2.0f * (float)M.rate, den = w0 + K;
            float g0l = v->g[e][0], g0r = v->g[e][1];
            float d0l = v->itd[e][0], d0r = v->itd[e][1], a0l = v->hs_a[e][0], a0r = v->hs_a[e][1];
            if (!v->started) d0l = dl_t, d0r = dr_t, a0l = al_t, a0r = ar_t;
            float* buf = v->itd_buf[e];
            uint32_t p = v->itd_pos;
            for (uint32_t i = 0; i < m; i++) {
                float t = (float)(i + 1) * inv;
                buf[p] = src[i];
                float ys[2];
                for (int ear = 0; ear < 2; ear++) {
                    float d = ear ? d0r + (dr_t - d0r) * t : d0l + (dl_t - d0l) * t;
                    float al = ear ? a0r + (ar_t - a0r) * t : a0l + (al_t - a0l) * t;
                    int di = (int)d;
                    float fr = d - (float)di;
                    float x0 = buf[(p + AU_ITD - (uint32_t)di) % AU_ITD], x1 = buf[(p + AU_ITD - (uint32_t)di - 1) % AU_ITD];
                    float x = x0 + (x1 - x0) * fr;
                    // H(s) = (al s / w0 + 1) / (s / w0 + 1), bilinear
                    float b0 = (w0 + al * K) / den, b1 = (w0 - al * K) / den, a1 = (w0 - K) / den;
                    float y = b0 * x + b1 * v->hs_x1[e][ear] - a1 * v->hs_y1[e][ear];
                    v->hs_x1[e][ear] = x, v->hs_y1[e][ear] = fabsf(y) < 1e-20f ? 0.0f : y;
                    ys[ear] = y;
                }
                out[0][i] += ys[0] * (g0l + (g - g0l) * t), out[1][i] += ys[1] * (g0r + (g - g0r) * t);
                p = (p + 1) % AU_ITD;
            }
            if (e == nem - 1) v->itd_pos = p;
            v->g[e][0] = v->g[e][1] = g;
            v->itd[e][0] = dl_t, v->itd[e][1] = dr_t, v->hs_a[e][0] = al_t, v->hs_a[e][1] = ar_t;
            (void)tl, (void)tr;
        }
        // (two emitters share the delay line's position: the second wrote its own buffer from the same start)
    }
    // the reverb send (both sides' mean: the reverb is diffuse)
    if (send_to > 0.0f || v->send > 0.0f) {
        float s0 = v->send;
        for (uint32_t i = 0; i < m; i++) {
            float t = (float)(i + 1) * inv, s = (s0 + (send_to - s0) * t) * 0.5f;
            float x = v->ch == 2 ? (M.tmp[0][i] + M.tmp[1][i]) * 0.5f : M.tmp[0][i];
            rv[0][i] += x * s, rv[1][i] += x * s;
        }
        v->send = send_to;
    }
    v->started = 1;
    re_atomic_store(&M.played[slot], (int32_t)v->pos);
    if (v->fading_out || !more) voice_free(v, slot);
}

static void copy_buses(void) {
    int32_t ver = re_atomic_load(&M.bus_version);
    if (ver == M.bus_seen) return;
    if (!re_atomic_cas(&M.bus_lock, 0, 1)) return; // (the game thread writing: next block)
    memcpy(M.bus_mix, M.bus_game, sizeof M.bus_mix);
    memcpy(M.bus_order_mix, M.bus_order, sizeof M.bus_order_mix);
    M.nbus_order_mix = M.nbus_order;
    M.bus_seen = ver;
    re_atomic_store(&M.bus_lock, 0);
}

static void store_float(volatile int32_t* p, float f) {
    int32_t b;
    memcpy(&b, &f, 4);
    re_atomic_store(p, b);
}
static float load_float(volatile int32_t* p) {
    int32_t b = re_atomic_load(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

// n frames (<= AU_BLOCK) of the master into out (interleaved in the output's channels)
static void mix_block(float* out, uint32_t n) {
    run_cmds();
    copy_buses();
    memset(M.bus, 0, sizeof M.bus);
    int32_t real = 0, virt = 0;
    for (uint32_t s = 0; s < AU_VOICES; s++) {
        if (!M.v[s].state) continue;
        if (M.params[s].virt) virt++;
        else real++;
        mix_voice(s, n);
    }
    re_atomic_store(&M.nreal, real), re_atomic_store(&M.nvirt, virt);
    // the buses, children before parents, each into its parent; the master last
    uint32_t nch = M.nch;
    for (uint32_t k = 0; k < M.nbus_order_mix; k++) {
        uint32_t b = M.bus_order_mix[k];
        const BusDesc* d = &M.bus_mix[b];
        if (!d->used) continue;
        float* ch[AU_MAX_CH];
        for (uint32_t c = 0; c < nch; c++) ch[c] = M.bus[b][c];
        for (uint32_t f = 0; f < d->nfx && f < AU_BUS_FX; f++) au_fx_process(M.fx[b][f], &d->fx[f], ch, M.role, (int)nch, n);
        if (b == AU_BUS_MASTER) continue;
        uint32_t p = d->parent < AU_MAX_BUSES ? d->parent : 0;
        for (uint32_t c = 0; c < nch; c++)
            for (uint32_t i = 0; i < n; i++) M.bus[p][c][i] += M.bus[b][c][i] * d->volume;
    }
    float mv = M.master_volume * M.bus_mix[0].volume;
    float* mc[AU_MAX_CH];
    for (uint32_t c = 0; c < nch; c++) {
        mc[c] = M.bus[0][c];
        for (uint32_t i = 0; i < n; i++) mc[c][i] *= mv;
    }
    if (M.lfe >= 0) au_biquad_run(&M.lfe_lp, &mc[M.lfe], 1, n); // (the subwoofer: the lows only)
    float red = M.limiter ? au_limiter_process(M.limiter, mc, (int)nch, n) : 0.0f;
    for (uint32_t c = 0; c < nch; c++) {
        float pk = load_float(&M.peak_bits[c]) * 0.97f;
        const float* x = mc[c];
        for (uint32_t i = 0; i < n; i++) {
            out[i * nch + c] = x[i];
            if (fabsf(x[i]) > pk) pk = fabsf(x[i]);
        }
        store_float(&M.peak_bits[c], pk);
    }
    store_float(&M.lim_bits, red);
    re_atomic_add(&M.blocks, 1);
}

// frames of the master (any count), measured
static void mix(float* out, uint32_t frames) {
    double t0 = re_time_seconds();
    au_enter(&M.lock);
    for (uint32_t done = 0; done < frames;) {
        uint32_t n = frames - done > AU_BLOCK ? AU_BLOCK : frames - done;
        mix_block(out + (size_t)M.nch * done, n);
        done += n;
    }
    au_leave(&M.lock);
    double ms = (re_time_seconds() - t0) * 1000.0;
    M.mix_acc += ms, M.frames_acc += frames; // (the mean per device period: the share of its time the mixer takes)
    if (ms > M.mix_max) M.mix_max = ms;
    double now = re_time_seconds();
    if (now - M.stat_t0 > 1.0) {
        M.mix_mean = M.frames_acc > 0.0 ? M.mix_acc / M.frames_acc * M.period : 0.0;
        M.mix_max_shown = M.mix_max;
        M.mix_acc = 0.0, M.frames_acc = 0.0, M.mix_max = 0.0, M.stat_t0 = now;
    }
}

#ifdef _WIN32
// --- The device ---
typedef struct Device {
    IMMDeviceEnumerator* en;
    IMMDevice* dev;
    IAudioClient* client;
    IAudioRenderClient* render;
    HANDLE event;
    UINT32 frames;
    uint32_t channels, is_float, bits;
    WCHAR id[256];
} Device;

static void device_close(Device* d) {
    if (d->client) IAudioClient_Stop(d->client);
    if (d->render) IAudioRenderClient_Release(d->render);
    if (d->client) IAudioClient_Release(d->client);
    if (d->dev) IMMDevice_Release(d->dev);
    if (d->event) CloseHandle(d->event);
    IMMDeviceEnumerator* en = d->en;
    memset(d, 0, sizeof *d);
    d->en = en;
}

static bool device_open(Device* d) {
    if (!d->en && FAILED(CoCreateInstance(&s_clsid_enum, NULL, CLSCTX_ALL, &s_iid_enum, (void**)&d->en))) return false;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(d->en, eRender, eConsole, &d->dev))) return false;
    LPWSTR id = NULL;
    if (SUCCEEDED(IMMDevice_GetId(d->dev, &id)) && id) {
        lstrcpynW(d->id, id, 256);
        CoTaskMemFree(id);
    }
    if (FAILED(IMMDevice_Activate(d->dev, &s_iid_client, CLSCTX_ALL, NULL, (void**)&d->client))) return false;
    WAVEFORMATEX* mixf = NULL;
    if (FAILED(IAudioClient_GetMixFormat(d->client, &mixf)) || !mixf) return false;
    uint32_t rate = mixf->nSamplesPerSec, dch = mixf->nChannels, dmask = 0;
    if (mixf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixf->cbSize >= 22) dmask = ((WAVEFORMATEXTENSIBLE*)mixf)->dwChannelMask;
    CoTaskMemFree(mixf);
    // float at the device's rate in its own speakers (5.1, 7.1: each channel mixed here, VBAP); more than 8 or an
    // unknown layout: stereo (Windows converts)
    bool native = dch >= 1 && dch <= AU_MAX_CH && dmask && (uint32_t)__popcnt(dmask) == dch && !(dmask & ~0x63Fu);
    if (getenv("REALENGINE_AUDIO_STEREO")) native = false; // (a measure's override: the old stereo output)
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    for (int attempt = native ? 0 : 1; attempt < 2; attempt++) {
        uint32_t ch = attempt ? 2 : dch, mask = attempt ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : dmask;
        WAVEFORMATEXTENSIBLE f;
        memset(&f, 0, sizeof f);
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = (WORD)ch, f.Format.nSamplesPerSec = rate, f.Format.wBitsPerSample = 32;
        f.Format.nBlockAlign = (WORD)(4 * ch), f.Format.nAvgBytesPerSec = rate * 4 * ch, f.Format.cbSize = 22;
        f.Samples.wValidBitsPerSample = 32, f.dwChannelMask = mask;
        f.SubFormat = s_float_sub;
        if (SUCCEEDED(IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, (WAVEFORMATEX*)&f, NULL))) {
            au_enter(&M.lock); // (the mixer isn't mixing: this is its thread; a test's render waits)
            M.rate = rate;
            set_layout(ch, mask);
            au_leave(&M.lock);
            d->channels = ch;
            break;
        }
        if (attempt == 1) return false;
        IAudioClient_Release(d->client), d->client = NULL; // (initialized once at most: a fresh client for stereo)
        if (FAILED(IMMDevice_Activate(d->dev, &s_iid_client, CLSCTX_ALL, NULL, (void**)&d->client))) return false;
    }
    d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!d->event || FAILED(IAudioClient_SetEventHandle(d->client, d->event))) return false;
    if (FAILED(IAudioClient_GetBufferSize(d->client, &d->frames))) return false;
    if (FAILED(IAudioClient_GetService(d->client, &s_iid_render, (void**)&d->render))) return false;
    REFERENCE_TIME per = 0, minp = 0;
    IAudioClient_GetDevicePeriod(d->client, &per, &minp);
    d->is_float = 1, d->bits = 32;
    M.rate = rate;
    M.period = per > 0 ? (uint32_t)((uint64_t)per * rate / 10000000u) : rate / 100;
    if (FAILED(IAudioClient_Start(d->client))) return false;
    return true;
}

static bool default_changed(Device* d) {
    IMMDevice* now = NULL;
    if (!d->en || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(d->en, eRender, eConsole, &now)) || !now) return false;
    LPWSTR id = NULL;
    bool changed = false;
    if (SUCCEEDED(IMMDevice_GetId(now, &id)) && id) {
        changed = lstrcmpW(id, d->id) != 0;
        CoTaskMemFree(id);
    }
    IMMDevice_Release(now);
    return changed;
}

#endif

static float g_dev_buf[AU_MAX_CH * 8192];

#ifdef _WIN32
static int mixer_thread(void* arg) {
    (void)arg;
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    Device d;
    memset(&d, 0, sizeof d);
    double next_check = 0.0, silent_t = re_time_seconds();
    bool have = false;
    while (!M.quit) {
        if (!M.silent && !have) {
            have = device_open(&d);
            if (!have) device_close(&d);
            else {
                snprintf(M.device, sizeof M.device, "WASAPI %u Hz, %s, %u frames", M.rate, M.layout, d.frames);
                printf("[RealEngine] Audio: device open: %s\n", M.device);
                // the limiter and the effects at the device's rate (made once: a rate change remakes them)
            }
        }
        if (!have) { // the silent device: the same mixing paced by the clock, nothing heard
            if (!M.silent) snprintf(M.device, sizeof M.device, "(none)");
            double now = re_time_seconds();
            uint32_t due = (uint32_t)((now - silent_t) * M.rate);
            if (due > 4096) due = 4096, silent_t = now - 4096.0 / M.rate;
            if (due >= M.period && !M.manual) {
                mix(g_dev_buf, due);
                silent_t += (double)due / M.rate;
            } else if (M.manual) silent_t = now;
            Sleep(M.silent ? 5 : 250); // (no device: tried again every 250 ms)
            continue;
        }
        DWORD wr = WaitForSingleObject(d.event, 200);
        (void)wr;
        UINT32 pad = 0;
        HRESULT hr = IAudioClient_GetCurrentPadding(d.client, &pad);
        double now = re_time_seconds();
        if (SUCCEEDED(hr) && now > next_check) { // the default device changed (headphones in): followed
            next_check = now + 2.0;
            if (default_changed(&d)) hr = AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (FAILED(hr)) { // lost (unplugged, changed): opened again
            device_close(&d);
            have = false;
            silent_t = re_time_seconds();
            continue;
        }
        UINT32 avail = d.frames - pad;
        if (avail > 8192) avail = 8192;
        if (!avail) continue;
        BYTE* data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(d.render, avail, &data)) || !data) continue;
        mix((float*)data, avail);
        IAudioRenderClient_ReleaseBuffer(d.render, avail, 0);
    }
    device_close(&d);
    if (d.en) IMMDeviceEnumerator_Release(d.en);
    if (av) AvRevertMmThreadCharacteristics(av);
    if (com) CoUninitialize();
    return 0;
}

#else
// Android: an AAudio stream in float stereo at the device's rate, low latency; its callback mixes (the thread the system
// gives it is the mixer's). Lost (headphones out, another app): opened again; no device: the silent pacing, as Windows'
static volatile int32_t g_aa_lost, g_aa_suspended;
static aaudio_data_callback_result_t aa_data(AAudioStream* st, void* user, void* audio, int32_t frames) {
    (void)st, (void)user;
    if (M.quit) return AAUDIO_CALLBACK_RESULT_STOP;
    mix((float*)audio, (uint32_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void aa_error(AAudioStream* st, void* user, aaudio_result_t err) {
    (void)st, (void)user, (void)err;
    re_atomic_store(&g_aa_lost, 1);
}
static AAudioStream* aa_open(void) {
    AAudioStreamBuilder* b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, aa_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, aa_error, NULL);
    AAudioStream* st = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !st) return NULL;
    au_enter(&M.lock);
    M.rate = (uint32_t)AAudioStream_getSampleRate(st);
    set_layout(2u, 0u);
    int32_t burst = AAudioStream_getFramesPerBurst(st);
    M.period = burst > 0 ? (uint32_t)burst : M.rate / 100;
    au_leave(&M.lock);
    if (AAudioStream_requestStart(st) != AAUDIO_OK) {
        AAudioStream_close(st);
        return NULL;
    }
    return st;
}
static int mixer_thread(void* arg) {
    (void)arg;
    AAudioStream* st = NULL;
    double silent_t = re_time_seconds();
    while (!M.quit) {
        if (st && re_atomic_load(&g_aa_suspended)) { // (in the background: the stream closed, nothing mixed)
            AAudioStream_requestStop(st);
            AAudioStream_close(st);
            st = NULL;
            printf("[RealEngine] Audio: device let go (in the background)\n");
        }
        if (re_atomic_load(&g_aa_suspended)) {
            re_sleep_ms(100);
            continue;
        }
        if (!M.silent && !st) {
            st = aa_open();
            if (st) {
                re_atomic_store(&g_aa_lost, 0);
                snprintf(M.device, sizeof M.device, "AAudio %u Hz, %s, %u frames", M.rate, M.layout, M.period);
                printf("[RealEngine] Audio: device open: %s\n", M.device);
            }
        }
        if (st) {
            if (re_atomic_load(&g_aa_lost)) { // (unplugged, taken: opened again)
                AAudioStream_requestStop(st);
                AAudioStream_close(st);
                st = NULL, silent_t = re_time_seconds();
                continue;
            }
            re_sleep_ms(50); // (the callback mixes; this thread only watches)
            continue;
        }
        if (!M.silent) snprintf(M.device, sizeof M.device, "(none)");
        double now = re_time_seconds();
        uint32_t due = (uint32_t)((now - silent_t) * M.rate);
        if (due > 4096) due = 4096, silent_t = now - 4096.0 / M.rate;
        if (due >= M.period && !M.manual) {
            mix(g_dev_buf, due);
            silent_t += (double)due / M.rate;
        } else if (M.manual) silent_t = now;
        re_sleep_ms(M.silent ? 5 : 250);
    }
    if (st) AAudioStream_requestStop(st), AAudioStream_close(st);
    return 0;
}
#endif

// --- The game thread's side ---
typedef struct ActiveSound {
    uint32_t gen;
    uint8_t used, stopping, paused, spatial, sequential;
    AudioPlay p;
    AuPlan plan;
    uint32_t nv;
    uint32_t vslot[AU_SOUND_VOICES], vgen[AU_SOUND_VOICES];
    uint8_t vdone[AU_SOUND_VOICES];
    float fade, fade_to, fade_rate;  // the fade's gain now, its target, per second
    float pos[3], prev_pos[3], vel[3];
    bool have_prev;
    float occl, occl_to;
    double occl_next, started;
    float audible, dist;             // the last frame's gain (virtualization, concurrency), distance (cm)
    float duck;                      // the concurrency's ducking
    uint32_t conc, group, cls, att;  // resolved (group: the concurrency's group: the asset, per sound for the project's default)
    uint32_t order;                  // its start's order (the oldest)
} ActiveSound;

typedef struct ActiveMix { const AuMix* mix; uint32_t hash; float fade; int dir; double t0; uint8_t passive, base; } ActiveMix;

static struct {
    ActiveSound s[AU_SOUNDS];
    uint8_t vused[AU_VOICES];
    uint32_t vgen[AU_VOICES], vowner[AU_VOICES];
    uint32_t order;
    float lpos[3], lfwd[3], lup[3], lright[3], lvel[3];
    bool game_paused;
    AudioWorldHooks hooks;
    AudioHandle finished[256];
    uint32_t nfinished;
    ActiveMix mixes[AU_MIXES];
    uint32_t nmixes;
    // retrigger: the last start of a concurrency group
    uint32_t rt_hash[64];
    double rt_time[64];
#ifdef _WIN32
    BOOL(WINAPI* prefetch)(HANDLE, ULONG_PTR, void*, ULONG);
#else
    void* prefetch; // (none: the mapped file is paged in by the reads)
#endif
    double prefetch_t;
} G;

void audio_set_world_hooks(const AudioWorldHooks* h) { G.hooks = h ? *h : (AudioWorldHooks){ 0 }; }

static void publish_buses(void);

bool audio_init(const char* content_root) {
    if (M.on) {
        audio_set_content_root(content_root);
        return true;
    }
    memset(&M, 0, sizeof M);
    memset(&G, 0, sizeof G);
    audio_waves_root(content_root);
    au_assets_root(content_root);
    ms_init(content_root); // (B8: the MetaSounds, the engine's own made when missing)
    au_lock_init(&M.lock);
    const char* bg = getenv("REALENGINE_BACKGROUND");
    const char* na = getenv("REALENGINE_NO_AUDIO");
    M.silent = (bg && bg[0] && bg[0] != '0') || (na && na[0] && na[0] != '0'); // (the tests: nothing heard on the PC)
    const char* forced = getenv("REALENGINE_AUDIO_DEVICE"); // (a measure in the background on the real device)
    if (forced && forced[0] == '1' && !(na && na[0] && na[0] != '0')) M.silent = false;
    M.rate = 48000, M.period = 480;
    { // the silent device's speakers (REALENGINE_AUDIO_CHANNELS: 6 for 5.1, 8 for 7.1; the tests' surround), else stereo
        const char* ec = getenv("REALENGINE_AUDIO_CHANNELS");
        set_layout(ec && atoi(ec) > 0 ? (uint32_t)atoi(ec) : 2u, 0u);
    }
    M.master_volume = M.user_volume = 1.0f;
    // the device's rate isn't known before its thread opens it: the effects are made at 48 kHz then remade if needed
#ifdef _WIN32
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) G.prefetch = (BOOL(WINAPI*)(HANDLE, ULONG_PTR, void*, ULONG))(void*)GetProcAddress(k32, "PrefetchVirtualMemory");
#endif
    G.lfwd[0] = 1.0f, G.lup[2] = 1.0f, G.lright[1] = 1.0f;
    M.on = true;
#ifdef _WIN32
    if (!M.silent) { // (the device's rate: probed here so the effects are made at it)
        bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
        IMMDeviceEnumerator* en = NULL;
        IMMDevice* dev = NULL;
        IAudioClient* cl = NULL;
        WAVEFORMATEX* mf = NULL;
        if (SUCCEEDED(CoCreateInstance(&s_clsid_enum, NULL, CLSCTX_ALL, &s_iid_enum, (void**)&en)) &&
            SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
            SUCCEEDED(IMMDevice_Activate(dev, &s_iid_client, CLSCTX_ALL, NULL, (void**)&cl)) && SUCCEEDED(IAudioClient_GetMixFormat(cl, &mf)) && mf) {
            M.rate = mf->nSamplesPerSec, M.period = M.rate / 100;
            CoTaskMemFree(mf);
        }
        if (cl) IAudioClient_Release(cl);
        if (dev) IMMDevice_Release(dev);
        if (en) IMMDeviceEnumerator_Release(en);
        if (com) CoUninitialize();
    }
#endif
    M.limiter = au_limiter_create((float)M.rate, -1.0f);
    for (int b = 0; b < AU_MAX_BUSES; b++)
        for (int f = 0; f < AU_BUS_FX; f++) M.fx[b][f] = au_fx_create((float)M.rate);
    publish_buses();
    M.thread = re_thread_start(mixer_thread, NULL);
#ifdef _WIN32
    printf("[RealEngine] Audio: %s mixer at %u Hz (%u voices)\n", M.silent ? "silent" : "WASAPI", M.rate, AU_VOICES);
#else
    printf("[RealEngine] Audio: %s mixer at %u Hz (%u voices)\n", M.silent ? "silent" : "AAudio", M.rate, AU_VOICES);
#endif
    return M.thread != NULL;
}

void audio_set_content_root(const char* content_root) {
    audio_stop_all(false);
    audio_waves_root(content_root);
    au_assets_root(content_root);
    ms_init(content_root);
    publish_buses();
}

void audio_shutdown(void) {
    if (!M.on) return;
    M.quit = true;
    re_thread_join(M.thread);
    au_limiter_destroy(M.limiter);
    for (int b = 0; b < AU_MAX_BUSES; b++)
        for (int f = 0; f < AU_BUS_FX; f++) au_fx_destroy(M.fx[b][f]);
    au_lock_free(&M.lock);
    M.on = false;
}

void audio_stats(AudioStats* o) {
    memset(o, 0, sizeof *o);
    if (!M.on) return;
    o->mix_ms = M.mix_mean, o->mix_ms_max = M.mix_max_shown;
    o->period_ms = (double)M.period * 1000.0 / M.rate;
    o->rate = M.rate;
    o->real = (uint32_t)re_atomic_load(&M.nreal), o->virt = (uint32_t)re_atomic_load(&M.nvirt);
    for (uint32_t i = 0; i < AU_SOUNDS; i++) o->voices += G.s[i].used;
    o->channels = M.nch;
    snprintf(o->layout, sizeof o->layout, "%s", M.layout);
    for (uint32_t c = 0; c < M.nch && c < 8; c++) o->peak[c] = load_float(&M.peak_bits[c]);
    o->limiter_db = load_float(&M.lim_bits);
    o->silent = M.silent;
    snprintf(o->device, sizeof o->device, "%s", M.silent ? "silent (REALENGINE_BACKGROUND)" : M.device);
}

void audio_set_master_volume(float v) { M.user_volume = clampf(v, 0.0f, 1.0f); }
float audio_master_volume(void) { return M.user_volume; }

void audio_render_test(float* out, uint32_t frames) {
    if (!M.on || !M.silent) {
        memset(out, 0, sizeof(float) * 2 * frames);
        return;
    }
    M.manual = true; // (the clock's pacing stops: the test drives it)
    mix(out, frames);
}

// --- Buses ---
static void bus_order_build(void) { // children before parents (depth from the master, deepest first)
    uint32_t depth[AU_MAX_BUSES] = { 0 };
    for (uint32_t b = 0; b < AU_MAX_BUSES; b++) {
        uint32_t d = 0, p = b;
        while (p != AU_BUS_MASTER && d < AU_MAX_BUSES) p = M.bus_game[p].parent, d++;
        depth[b] = d;
    }
    M.nbus_order = 0;
    for (int d = AU_MAX_BUSES; d >= 0; d--)
        for (uint32_t b = 0; b < AU_MAX_BUSES; b++)
            if (M.bus_game[b].used && depth[b] == (uint32_t)d) M.bus_order[M.nbus_order++] = b;
}

static void bus_commit(void) {
    while (!re_atomic_cas(&M.bus_lock, 0, 1)) re_cpu_relax();
    bus_order_build();
    re_atomic_add(&M.bus_version, 1);
    re_atomic_store(&M.bus_lock, 0);
}

static void publish_buses(void) {
    while (!re_atomic_cas(&M.bus_lock, 0, 1)) re_cpu_relax();
    memset(M.bus_game, 0, sizeof M.bus_game);
    memset(M.bus_hash, 0, sizeof M.bus_hash);
    M.bus_game[AU_BUS_MASTER] = (BusDesc){ 0, 1.0f, { { 0 } }, 0, 1, { 0 } };
    M.bus_game[AU_BUS_REVERB] = (BusDesc){ AU_BUS_MASTER, 1.0f, { { 0 } }, 1, 1, { 0 } };
    au_effect_defaults(&M.bus_game[AU_BUS_REVERB].fx[0], AUFX_REVERB);
    M.bus_game[AU_BUS_REVERB].fx[0].p[5] = 0.0f; // (the send: wet only)
    M.bus_game[AU_BUS_REVERB].fx[0].p[4] = 1.0f;
    au_fx_prepare(M.fx[AU_BUS_REVERB][0], AUFX_REVERB);
    re_atomic_store(&M.bus_lock, 0);
    bus_commit();
}

uint32_t au_bus_of_submix(uint32_t submix) {
    if (!submix) return AU_BUS_MASTER;
    for (uint32_t b = 2; b < AU_MAX_BUSES; b++)
        if (M.bus_hash[b] == submix && M.bus_game[b].used) return b;
    const AuSubmix* s = au_submix(submix);
    if (!s) return AU_BUS_MASTER;
    uint32_t parent = s->parent && s->parent != submix ? au_bus_of_submix(s->parent) : AU_BUS_MASTER; // (made first)
    uint32_t b = 2;
    while (b < AU_MAX_BUSES && M.bus_game[b].used) b++;
    if (b == AU_MAX_BUSES) return AU_BUS_MASTER; // (full: to the master)
    for (uint32_t f = 0; f < s->nfx && f < AU_BUS_FX; f++) au_fx_prepare(M.fx[b][f], s->fx[f].kind); // (off the mixer)
    while (!re_atomic_cas(&M.bus_lock, 0, 1)) re_cpu_relax();
    BusDesc* d = &M.bus_game[b];
    d->parent = parent, d->volume = s->volume, d->nfx = s->nfx > AU_BUS_FX ? AU_BUS_FX : s->nfx, d->used = 1;
    memcpy(d->fx, s->fx, sizeof d->fx);
    M.bus_hash[b] = submix;
    re_atomic_store(&M.bus_lock, 0);
    bus_commit();
    return b;
}

// Audio Volumes' reverb: the reverb send's effect blended toward it
static AuEffect g_rev_from, g_rev_to;
static float g_rev_t = 1.0f, g_rev_rate;
void au_set_reverb(const AuEffect* reverb, float blend) {
    AuEffect def;
    au_effect_defaults(&def, AUFX_REVERB);
    const AuEffect* to = reverb && reverb->kind == AUFX_REVERB ? reverb : &def;
    if (!memcmp(to->p, g_rev_to.p, sizeof to->p) && g_rev_to.kind == AUFX_REVERB) return;
    g_rev_from = M.bus_game[AU_BUS_REVERB].fx[0];
    g_rev_to = *to;
    g_rev_t = blend > 0.0f ? 0.0f : 1.0f, g_rev_rate = blend > 0.0f ? 1.0f / blend : 0.0f;
}
static void reverb_step(float dt) {
    if (g_rev_to.kind != AUFX_REVERB) return;
    if (g_rev_t >= 1.0f && !memcmp(M.bus_game[AU_BUS_REVERB].fx[0].p, g_rev_to.p, sizeof g_rev_to.p)) return;
    g_rev_t = g_rev_rate > 0.0f ? clampf(g_rev_t + dt * g_rev_rate, 0.0f, 1.0f) : 1.0f;
    while (!re_atomic_cas(&M.bus_lock, 0, 1)) re_cpu_relax();
    AuEffect* e = &M.bus_game[AU_BUS_REVERB].fx[0];
    e->kind = AUFX_REVERB;
    for (int k = 0; k < AU_FX_PARAMS; k++) e->p[k] = g_rev_from.p[k] + (g_rev_to.p[k] - g_rev_from.p[k]) * g_rev_t;
    e->p[5] = 0.0f; // (wet only)
    re_atomic_store(&M.bus_lock, 0);
    re_atomic_add(&M.bus_version, 1);
}

// --- Voices ---
typedef struct Rank { float key; uint32_t s, k; } Rank;
static int rank_cmp(const void* a, const void* b) {
    float x = ((const Rank*)a)->key, y = ((const Rank*)b)->key;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void update_sound(uint32_t i, float dt, double now, uint32_t* passive, uint32_t* npassive_, Rank* ranks, uint32_t* nrank_);
static AudioHandle handle_of(uint32_t i) { return (G.s[i].gen << 9) | (i + 1); }
static ActiveSound* sound_of(AudioHandle h) {
    uint32_t i = (h & 511u) - 1u;
    if (!h || i >= AU_SOUNDS || !G.s[i].used || G.s[i].gen != h >> 9) return NULL;
    return &G.s[i];
}

static int voice_alloc(void) {
    for (uint32_t s = 0; s < AU_VOICES; s++)
        if (!G.vused[s]) {
            G.vused[s] = 1;
            G.vgen[s] = (G.vgen[s] + 1) & 0x7FFFFFFF;
            if (!G.vgen[s]) G.vgen[s] = 1;
            return (int)s;
        }
    return -1;
}

static void sound_release(ActiveSound* a, bool finished) {
    for (uint32_t k = 0; k < a->nv; k++)
        if (!a->vdone[k]) {
            Cmd c = { CMD_STOP, a->vslot[k], a->vgen[k], 0, NULL, 0, 0 };
            push_cmd(&c);
            a->vdone[k] = 2; // (stopping: the slot freed when the mixer lets go)
        }
    if (finished && G.nfinished < 256) G.finished[G.nfinished++] = handle_of((uint32_t)(a - G.s));
    a->used = 0;
}

// A class's chain: the volume, pitch, low pass (with the mixes), its submix and flags
typedef struct ClassEval { float volume, pitch, lpf; uint32_t submix; bool ui, always; } ClassEval;
static float mix_volume_for(uint32_t cls, const AuClass* chain[], int nchain, float* pitch, float* lpf) {
    float v = 1.0f;
    for (uint32_t m = 0; m < G.nmixes; m++) {
        const ActiveMix* am = &G.mixes[m];
        if (!am->mix || am->fade <= 0.0f) continue;
        for (uint32_t k = 0; k < am->mix->nadj; k++) {
            uint32_t target = am->mix->adj[k].sound_class;
            bool hit = target == cls;
            for (int c = 1; c < nchain && !hit && am->mix->adj[k].children; c++) hit = chain[c]->hash == target;
            if (!hit) continue;
            v *= 1.0f + (am->mix->adj[k].volume - 1.0f) * am->fade;
            *pitch *= 1.0f + (am->mix->adj[k].pitch - 1.0f) * am->fade;
            if (am->mix->adj[k].lpf_hz > 0.0f) *lpf = fminf(*lpf, 20000.0f + (am->mix->adj[k].lpf_hz - 20000.0f) * am->fade);
        }
    }
    return v;
}
// The player's volumes of the engine's classes (the game's options: audio_set_user_class_volume), by their hashes
static float g_user_cls[AU_SC_COUNT] = { 1, 1, 1, 1, 1, 1 };
static uint32_t g_user_cls_hash[AU_SC_COUNT];
static bool g_user_cls_on; // (one of them under 1: looked for in the chains)
void audio_set_user_class_volume(int which, float v) {
    if (which < 0 || which >= AU_SC_COUNT) return;
    if (!g_user_cls_hash[0])
        for (int k = 0; k < AU_SC_COUNT; k++) g_user_cls_hash[k] = au_builtin_class(k);
    g_user_cls[which] = clampf(v, 0.0f, 1.0f);
    g_user_cls_on = false;
    for (int k = 0; k < AU_SC_COUNT; k++) g_user_cls_on |= g_user_cls[k] < 1.0f;
}
float audio_user_class_volume(int which) { return which >= 0 && which < AU_SC_COUNT ? g_user_cls[which] : 1.0f; }
static ClassEval class_eval(uint32_t cls) {
    ClassEval e = { 1.0f, 1.0f, 20000.0f, 0, false, false };
    const AuClass* chain[8];
    int n = 0;
    for (const AuClass* c = au_class(cls); c && n < 8; c = c->parent && c->parent != c->hash ? au_class(c->parent) : NULL) chain[n++] = c;
    for (int k = 0; k < n; k++) {
        e.volume *= chain[k]->volume, e.pitch *= chain[k]->pitch;
        if (g_user_cls_on)
            for (int u = 0; u < AU_SC_COUNT; u++)
                if (chain[k]->hash == g_user_cls_hash[u]) e.volume *= g_user_cls[u];
        if (chain[k]->lpf_hz > 0.0f) e.lpf = fminf(e.lpf, chain[k]->lpf_hz);
        if (!e.submix) e.submix = chain[k]->submix;
        e.ui |= chain[k]->ui != 0, e.always |= chain[k]->always_play != 0;
    }
    if (n) e.volume *= mix_volume_for(cls, chain, n, &e.pitch, &e.lpf);
    return e;
}

static float falloff_gain(const AuAttenuation* a, float d) {
    if (d <= a->inner_radius) return 1.0f;
    float span = a->falloff_distance > 1.0f ? a->falloff_distance : 1.0f;
    float t = clampf((d - a->inner_radius) / span, 0.0f, 1.0f);
    switch (a->falloff) {
    case AU_FALLOFF_LINEAR: return 1.0f - t;
    case AU_FALLOFF_LOG: return clampf(1.0f - log10f(1.0f + 9.0f * t), 0.0f, 1.0f);      // (falls fast near)
    case AU_FALLOFF_LOG_REVERSE: return clampf(log10f(10.0f - 9.0f * t), 0.0f, 1.0f);    // (holds, then falls)
    case AU_FALLOFF_INVERSE: return t >= 1.0f ? 0.0f : clampf((1.0f - t) / (1.0f + 9.0f * t), 0.0f, 1.0f); // (1/r like)
    default: // Natural Sound: dB falling linearly to db_at_max at the end (a real source's -6 dB per doubling, bounded)
        return t >= 1.0f ? 0.0f : powf(10.0f, (a->db_at_max < 0.0f ? a->db_at_max : -60.0f) * t / 20.0f);
    }
}

void au_attenuation_defaults(AuAttenuation* a) {
    memset(a, 0, sizeof *a);
    a->spatialize = 1, a->falloff = AU_FALLOFF_NATURAL, a->method = AU_SPAT_PROJECT;
    a->inner_radius = 400.0f, a->falloff_distance = 3600.0f, a->db_at_max = -60.0f, a->non_spatial_radius = 50.0f;
    a->spread = 200.0f;
    a->air_absorption = 1, a->lpf_near_hz = 20000.0f, a->lpf_far_hz = 4000.0f, a->absorb_min = 3000.0f, a->absorb_max = 20000.0f;
    a->occlusion = 0, a->occlusion_lpf_hz = 1200.0f, a->occlusion_volume = 0.5f, a->occlusion_interp = 0.1f;
    a->reverb_send = 1, a->reverb_wet_min = 0.15f, a->reverb_wet_max = 0.6f, a->reverb_dist_min = 400.0f, a->reverb_dist_max = 6000.0f;
    a->doppler = 0.0f, a->priority_falloff = 0.5f;
}

// The concurrency's resolution for a new sound of the group; false: it doesn't play
static bool concurrency_admit(const AudioPlay* p, uint32_t conc_hash, uint32_t group, float new_dist) {
    const AuConcurrency* c = au_concurrency(conc_hash);
    if (!c) return true;
    double now = re_time_seconds();
    if (c->retrigger_time > 0.0f) {
        int slot = -1;
        for (int k = 0; k < 64; k++)
            if (G.rt_hash[k] == group) slot = k;
        if (slot >= 0 && now - G.rt_time[slot] < c->retrigger_time) return false;
        if (slot < 0)
            for (int k = 0; k < 64 && slot < 0; k++)
                if (!G.rt_hash[k] || now - G.rt_time[k] > 10.0) slot = k;
        if (slot >= 0) G.rt_hash[slot] = group, G.rt_time[slot] = now;
    }
    if (!c->max_count) return true;
    ActiveSound* members[AU_SOUNDS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < AU_SOUNDS; i++) {
        ActiveSound* a = &G.s[i];
        if (!a->used || a->stopping || a->group != group) continue;
        if (c->per_owner && a->p.owner != p->owner) continue;
        members[n++] = a;
    }
    if (n < c->max_count) return true;
    ActiveSound* victim = NULL;
    switch (c->rule) {
    case AU_RES_PREVENT_NEW: return false;
    case AU_RES_STOP_OLDEST:
        for (uint32_t k = 0; k < n; k++)
            if (!victim || members[k]->order < victim->order) victim = members[k];
        break;
    case AU_RES_STOP_FARTHEST_THEN_PREVENT:
    case AU_RES_STOP_FARTHEST_THEN_OLDEST:
        for (uint32_t k = 0; k < n; k++)
            if (!victim || members[k]->dist > victim->dist || (members[k]->dist == victim->dist && members[k]->order < victim->order)) victim = members[k];
        if (victim && victim->dist <= new_dist && c->rule == AU_RES_STOP_FARTHEST_THEN_PREVENT) return false;
        break;
    case AU_RES_STOP_LOWEST_PRIORITY:
    case AU_RES_STOP_LOWEST_PRIORITY_THEN_PREVENT:
        for (uint32_t k = 0; k < n; k++)
            if (!victim || members[k]->plan.priority < victim->plan.priority) victim = members[k];
        if (c->rule == AU_RES_STOP_LOWEST_PRIORITY_THEN_PREVENT && victim && victim->plan.priority >= 1.0f) return false;
        break;
    default: // quietest
        for (uint32_t k = 0; k < n; k++)
            if (!victim || members[k]->audible < victim->audible) victim = members[k];
        break;
    }
    if (victim) {
        victim->stopping = 1, victim->fade_to = 0.0f, victim->fade_rate = 1.0f / 0.05f; // (a short fade: no click)
    }
    return true;
}

AudioHandle audio_play(const AudioPlay* p) {
    if (!M.on || !p || !p->sound) return 0;
    AuPlan plan;
    if (!au_plan(p->sound, &plan) || !plan.n) return 0;
    uint32_t idx = AU_SOUNDS;
    for (uint32_t i = 0; i < AU_SOUNDS && idx == AU_SOUNDS; i++)
        if (!G.s[i].used) idx = i;
    if (idx == AU_SOUNDS) return 0; // (256 sounds at once: the new one dropped)
    const AuProject* pr = au_project();
    uint32_t conc = p->concurrency ? p->concurrency : plan.concurrency ? plan.concurrency : pr->default_concurrency;
    float d0 = 0.0f;
    if (p->spatial) {
        float d[3] = { p->pos[0] - G.lpos[0], p->pos[1] - G.lpos[1], p->pos[2] - G.lpos[2] };
        d0 = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    // the group: an asset set on the sound (or its call) groups every sound using it; the project's default groups
    // each sound with itself (Unreal's)
    bool own = p->concurrency || plan.concurrency;
    uint32_t group = own ? conc : conc ^ (p->sound * 2654435761u);
    if (conc && !concurrency_admit(p, conc, group, d0)) return 0;
    ActiveSound* a = &G.s[idx];
    uint32_t gen = (a->gen + 1) & 0x7FFF; // (15 bits: a handle under 2^24, exact in a Blueprint's float)
    memset(a, 0, sizeof *a);
    a->gen = gen ? gen : 1, a->used = 1;
    a->p = *p;
    if (a->p.volume <= 0.0f) a->p.volume = 1.0f;
    if (a->p.pitch <= 0.0f) a->p.pitch = 1.0f;
    a->plan = plan, a->conc = conc, a->group = group;
    a->cls = p->sound_class ? p->sound_class : plan.sound_class ? plan.sound_class : pr->default_class;
    a->att = p->attenuation ? p->attenuation : plan.attenuation;
    a->spatial = p->spatial;
    memcpy(a->pos, p->pos, sizeof a->pos);
    a->fade = p->fade_in > 0.0f ? 0.0f : 1.0f, a->fade_to = 1.0f, a->fade_rate = p->fade_in > 0.0f ? 1.0f / p->fade_in : 0.0f;
    a->started = re_time_seconds(), a->order = ++G.order, a->duck = 1.0f, a->dist = d0;
    a->occl = a->occl_to = 0.0f;
    // its waves: started together (a Concatenator's each after the one before, sample-accurate by its delay)
    double at = 0.0;
    for (uint32_t k = 0; k < plan.n; k++) {
        int s = voice_alloc();
        if (s < 0) break;
        const SoundWave* w = plan.wave[k];
        float pitch = plan.pitch[k] * plan.pitch_all * w->h.pitch * a->p.pitch;
        double start = k == 0 ? p->start_time : 0.0;
        double delay_s = plan.sequential ? at : plan.delay[k];
        if (plan.sequential) at += (w->length - start) / (pitch > 0.01f ? pitch : 1.0f) + plan.delay[k];
        uint32_t loop = plan.loop[k] || (w->h.flags & SND_LOOPING);
        Cmd c = { CMD_START, (uint32_t)s, G.vgen[s], (uint32_t)(delay_s * M.rate), w, start * w->h.sample_rate, loop };
        if (c.pos >= w->h.frames) c.pos = loop ? fmod(c.pos, (double)w->h.frames) : (double)w->h.frames - 1;
        G.vowner[s] = idx;
        if (w->ms) // (B8: a MetaSound's inputs at their defaults)
            for (uint32_t q = 0; q < MS_MAX_PARAMS; q++) M.msp[s][q] = q < w->ms->nparams ? w->ms->n[w->ms->param_node[q]].in[0].value : 0.0f;
        re_atomic_store(&M.ended[s], -1);
        memset(&M.params[s], 0, sizeof M.params[s]); // (silent until its first update: audio_update publishes it)
        M.params[s].virt = 1;
        push_cmd(&c);
        a->vslot[a->nv] = (uint32_t)s, a->vgen[a->nv] = G.vgen[s], a->vdone[a->nv] = 0;
        a->nv++;
    }
    if (!a->nv) {
        a->used = 0;
        return 0;
    }
    uint32_t np = 0, passive[16];
    update_sound(idx, 0.0f, re_time_seconds(), passive, &np, NULL, NULL); // (its parameters now: no frame late)
    for (uint32_t k = 0; k < a->nv; k++) M.params[a->vslot[k]].virt = 0;
    return handle_of(idx);
}

AudioHandle audio_play_2d(uint32_t sound, float volume, float pitch) {
    AudioPlay p;
    memset(&p, 0, sizeof p);
    p.sound = sound, p.volume = volume, p.pitch = pitch, p.bone = -1;
    return audio_play(&p);
}
AudioHandle audio_play_at(uint32_t sound, const float pos[3], float volume, float pitch) {
    AudioPlay p;
    memset(&p, 0, sizeof p);
    p.sound = sound, p.volume = volume, p.pitch = pitch, p.spatial = true, p.bone = -1;
    memcpy(p.pos, pos, sizeof p.pos);
    return audio_play(&p);
}

void audio_stop(AudioHandle h, float fade) {
    ActiveSound* a = sound_of(h);
    if (!a) return;
    if (fade > 0.0f) {
        a->stopping = 1, a->fade_to = 0.0f, a->fade_rate = a->fade / fade;
        if (a->fade_rate <= 0.0f) a->fade_rate = 1.0f / fade;
    } else sound_release(a, true);
}
void audio_fade(AudioHandle h, float seconds, float to) {
    ActiveSound* a = sound_of(h);
    if (!a) return;
    a->fade_to = clampf(to, 0.0f, 4.0f);
    a->fade_rate = seconds > 0.0f ? fabsf(a->fade_to - a->fade) / seconds : 0.0f;
    if (seconds <= 0.0f) a->fade = a->fade_to;
}
void audio_set_paused(AudioHandle h, bool paused) {
    ActiveSound* a = sound_of(h);
    if (a) a->paused = paused;
}
bool audio_is_playing(AudioHandle h) { return sound_of(h) != NULL; }
void audio_set_volume(AudioHandle h, float v) {
    ActiveSound* a = sound_of(h);
    if (a) a->p.volume = clampf(v, 0.0f, 16.0f);
}
void audio_set_pitch(AudioHandle h, float p) {
    ActiveSound* a = sound_of(h);
    if (a) a->p.pitch = clampf(p, 0.01f, 8.0f);
}
bool audio_set_param(AudioHandle h, const char* name, float value) {
    ActiveSound* a = sound_of(h);
    bool any = false;
    for (uint32_t k = 0; a && k < a->nv; k++) {
        const SoundWave* w = a->plan.wave[k];
        int p = w && w->ms ? ms_param_index(w->ms, name) : -1;
        if (p >= 0) M.msp[a->vslot[k]][p] = value, any = true; // (a float: the mixer reads it whole)
    }
    return any;
}

void audio_set_pos(AudioHandle h, const float pos[3]) {
    ActiveSound* a = sound_of(h);
    if (a) memcpy(a->pos, pos, sizeof a->pos);
}
float audio_playback_time(AudioHandle h) {
    ActiveSound* a = sound_of(h);
    if (!a || !a->nv) return 0.0f;
    const SoundWave* w = a->plan.wave[0];
    int32_t f = re_atomic_load(&M.played[a->vslot[0]]);
    return w && w->h.sample_rate ? (float)f / (float)w->h.sample_rate : 0.0f;
}
void audio_stop_owner(uint32_t owner, float fade) {
    for (uint32_t i = 0; i < AU_SOUNDS; i++)
        if (G.s[i].used && G.s[i].p.owner == owner) audio_stop(handle_of(i), fade);
}
void audio_stop_all(bool keep_previews) {
    for (uint32_t i = 0; i < AU_SOUNDS; i++)
        if (G.s[i].used && (!keep_previews || !G.s[i].p.preview)) sound_release(&G.s[i], false);
    G.nmixes = 0;
    G.game_paused = false;
}
void audio_pause_game(bool paused) { G.game_paused = paused; }
void audio_device_suspend(bool suspended) {
#ifdef __ANDROID__
    re_atomic_store(&g_aa_suspended, suspended ? 1 : 0);
#else
    (void)suspended; // (Windows: the device stays; the game's window minimized draws nothing)
#endif
}

void audio_set_listener(const float pos[3], const float fwd[3], const float up[3], const float vel[3]) {
    memcpy(G.lpos, pos, sizeof G.lpos);
    float f[3] = { fwd[0], fwd[1], fwd[2] }, u[3] = { up[0], up[1], up[2] };
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) f[0] = 1, f[1] = f[2] = 0, fl = 1;
    for (int k = 0; k < 3; k++) f[k] /= fl;
    // right = up x forward (Unreal's: yaw 0 looks down +X, +Y on its right), up made orthogonal
    float r[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) r[0] = 0, r[1] = 1, r[2] = 0, rl = 1;
    for (int k = 0; k < 3; k++) r[k] /= rl;
    float uu[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    memcpy(G.lfwd, f, sizeof f), memcpy(G.lright, r, sizeof r), memcpy(G.lup, uu, sizeof uu);
    if (vel) memcpy(G.lvel, vel, sizeof G.lvel);
    else memset(G.lvel, 0, sizeof G.lvel);
}

uint32_t audio_finished(AudioHandle* out, uint32_t cap) {
    uint32_t n = G.nfinished < cap ? G.nfinished : cap;
    memcpy(out, G.finished, n * sizeof *out);
    memmove(G.finished, G.finished + n, (G.nfinished - n) * sizeof *G.finished);
    G.nfinished -= n;
    return n;
}

void audio_forget_wave(const SoundWave* w);
void audio_forget_wave(const SoundWave* w) { // (a reimport: its voices stopped, the mixer done with its samples)
    if (!M.on) return;
    bool any = false;
    for (uint32_t i = 0; i < AU_SOUNDS; i++) {
        ActiveSound* a = &G.s[i];
        if (!a->used) continue;
        for (uint32_t k = 0; k < a->plan.n; k++)
            if (a->plan.wave[k] == w) {
                sound_release(a, true);
                any = true;
                break;
            }
    }
    if (!any) return;
    int32_t b0 = re_atomic_load(&M.blocks);
    double t0 = re_time_seconds();
    while (re_atomic_load(&M.blocks) - b0 < 2 && re_time_seconds() - t0 < 0.5) re_sleep_seconds(0.001); // (two blocks: the stops ran)
    if (M.manual) { // (a test drives the mixer: run the queue here)
        float tmp[AU_MAX_CH * AU_BLOCK];
        mix(tmp, AU_BLOCK);
    }
}

// --- Mixes ---
static ActiveMix* mix_find(uint32_t hash, bool passive) {
    for (uint32_t m = 0; m < G.nmixes; m++)
        if (G.mixes[m].hash == hash && G.mixes[m].passive == passive && !G.mixes[m].base) return &G.mixes[m];
    return NULL;
}
void audio_push_mix(uint32_t mix_hash);
void audio_pop_mix(uint32_t mix_hash);
void audio_clear_mixes(void);
void audio_set_base_mix(uint32_t mix_hash);
void audio_push_mix(uint32_t h) {
    const AuMix* m = au_mix(h);
    if (!m) return;
    ActiveMix* am = mix_find(h, false);
    if (!am && G.nmixes < AU_MIXES) am = &G.mixes[G.nmixes++], memset(am, 0, sizeof *am);
    if (!am) return;
    am->mix = m, am->hash = h, am->dir = 1, am->t0 = re_time_seconds();
    if (m->fade_in <= 0.0f) am->fade = 1.0f;
}
void audio_pop_mix(uint32_t h) {
    ActiveMix* am = mix_find(h, false);
    if (am) am->dir = -1;
}
void audio_clear_mixes(void) {
    for (uint32_t m = 0; m < G.nmixes; m++)
        if (!G.mixes[m].base) G.mixes[m].dir = -1;
}
void audio_set_base_mix(uint32_t h) {
    for (uint32_t m = 0; m < G.nmixes; m++)
        if (G.mixes[m].base) G.mixes[m].dir = -1, G.mixes[m].base = 0, G.mixes[m].passive = 1; // (faded out)
    const AuMix* mx = au_mix(h);
    if (!mx || G.nmixes >= AU_MIXES) return;
    ActiveMix* am = &G.mixes[G.nmixes++];
    memset(am, 0, sizeof *am);
    am->mix = mx, am->hash = h, am->dir = 1, am->base = 1, am->fade = mx->fade_in > 0.0f ? 0.0f : 1.0f, am->t0 = re_time_seconds();
}

static void mixes_step(float dt, const uint32_t* passive, uint32_t npassive) {
    double now = re_time_seconds();
    // passive mixes: on while a sound of their class plays loud enough, off otherwise
    for (uint32_t k = 0; k < npassive; k++) {
        ActiveMix* am = mix_find(passive[k], true);
        if (!am && G.nmixes < AU_MIXES) {
            const AuMix* m = au_mix(passive[k]);
            if (m) {
                am = &G.mixes[G.nmixes++];
                memset(am, 0, sizeof *am);
                am->mix = m, am->hash = passive[k], am->passive = 1;
            }
        }
        if (am) am->dir = 1, am->t0 = now;
    }
    for (uint32_t m = 0; m < G.nmixes;) {
        ActiveMix* am = &G.mixes[m];
        if (am->passive && am->dir > 0) {
            bool still = false;
            for (uint32_t k = 0; k < npassive; k++) still |= passive[k] == am->hash;
            if (!still) am->dir = -1;
        }
        if (!am->passive && !am->base && am->mix && am->mix->duration > 0.0f && am->dir > 0 && now - am->t0 > am->mix->duration) am->dir = -1;
        float fi = am->mix ? am->mix->fade_in : 0.0f, fo = am->mix ? am->mix->fade_out : 0.0f;
        if (am->dir > 0) am->fade = fi > 0.0f ? fminf(1.0f, am->fade + dt / fi) : 1.0f;
        else am->fade = fo > 0.0f ? fmaxf(0.0f, am->fade - dt / fo) : 0.0f;
        if (am->dir < 0 && am->fade <= 0.0f) {
            G.mixes[m] = G.mixes[--G.nmixes];
            continue;
        }
        m++;
    }
}

// --- The frame's update ---

// One sound's parameters for this frame (its voices'); ranks: their keys for the virtualization (NULL: none)
static void update_sound(uint32_t i, float dt, double now, uint32_t* passive, uint32_t* npassive_, Rank* ranks, uint32_t* nrank_) {
    ActiveSound* a = &G.s[i];
    const AuProject* pr = au_project();
    uint32_t npassive = *npassive_, nrank = nrank_ ? *nrank_ : 0;
    // the fade
    if (a->fade != a->fade_to) {
        float st = a->fade_rate > 0.0f ? a->fade_rate * dt : 1e9f;
        a->fade = a->fade < a->fade_to ? fminf(a->fade_to, a->fade + st) : fmaxf(a->fade_to, a->fade - st);
    }
    if (a->stopping && a->fade <= 0.0f) {
        sound_release(a, true);
        return;
    }
    // where it is (following its entity)
    if (a->p.follow && G.hooks.entity_pos) {
        if (!G.hooks.entity_pos(a->p.follow, a->p.bone, a->pos)) { // (the actor is gone: so is its sound)
            a->stopping = 1, a->fade_to = 0.0f, a->fade_rate = 1.0f / 0.05f;
        }
    }
    if (a->have_prev)
        for (int k = 0; k < 3; k++) a->vel[k] = a->vel[k] * 0.7f + 0.3f * (a->pos[k] - a->prev_pos[k]) / dt;
    memcpy(a->prev_pos, a->pos, sizeof a->pos);
    a->have_prev = true;
    ClassEval ce = class_eval(a->cls);
    bool paused = a->paused || (G.game_paused && !a->p.ui && !a->p.preview && !ce.ui && !(a->plan.wave[0]->h.flags & SND_UI));
    float gain = a->p.volume * a->plan.volume_all * ce.volume * a->fade * a->duck;
    float pitch = a->p.pitch * a->plan.pitch_all * ce.pitch;
    float lpf = ce.lpf, send = 0.0f, spatial = 0.0f, az = 0.0f, el = 0.0f, spread = 0.0f;
    uint8_t binaural = 0;
    // 3D: the attenuation (the sound's, the project's default, the engine's)
    AuAttenuation defatt;
    const AuAttenuation* at = a->spatial ? au_attenuation(a->att) : NULL;
    if (a->spatial && !at) at = au_attenuation(pr->default_attenuation);
    if (a->spatial && !at) au_attenuation_defaults(&defatt), at = &defatt;
    a->dist = 0.0f;
    if (at && at->spatialize) {
        float d[3] = { a->pos[0] - G.lpos[0], a->pos[1] - G.lpos[1], a->pos[2] - G.lpos[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        a->dist = dist;
        gain *= falloff_gain(at, dist);
        float x = d[0] * G.lfwd[0] + d[1] * G.lfwd[1] + d[2] * G.lfwd[2];
        float y = d[0] * G.lright[0] + d[1] * G.lright[1] + d[2] * G.lright[2];
        float z = d[0] * G.lup[0] + d[1] * G.lup[1] + d[2] * G.lup[2];
        az = atan2f(y, x), el = dist > 1e-3f ? asinf(clampf(z / dist, -1.0f, 1.0f)) : 0.0f;
        spatial = at->non_spatial_radius > 0.0f ? clampf(dist / at->non_spatial_radius, 0.0f, 1.0f) : 1.0f;
        spread = dist > 1.0f ? atanf(at->spread * 0.5f / dist) : 0.0f;
        binaural = at->method == AU_SPAT_BINAURAL || (at->method == AU_SPAT_PROJECT && pr->binaural);
        if (at->air_absorption && at->absorb_max > at->absorb_min) { // (on a log scale of frequency: even to the ear)
            float t = clampf((dist - at->absorb_min) / (at->absorb_max - at->absorb_min), 0.0f, 1.0f);
            float hz = expf(logf(fmaxf(at->lpf_near_hz, 20.0f)) + (logf(fmaxf(at->lpf_far_hz, 20.0f)) - logf(fmaxf(at->lpf_near_hz, 20.0f))) * t);
            lpf = fminf(lpf, hz);
        }
        if (at->occlusion && G.hooks.occlusion) { // a ray to the listener ~10 times a second, smoothed
            if (now >= a->occl_next) {
                a->occl_to = G.hooks.occlusion(a->pos, G.lpos);
                a->occl_next = now + 0.1 + 0.02 * (i % 5);
            }
            float r = at->occlusion_interp > 0.0f ? dt / at->occlusion_interp : 1.0f;
            a->occl += (a->occl_to - a->occl) * clampf(r, 0.0f, 1.0f);
            gain *= 1.0f + (at->occlusion_volume - 1.0f) * a->occl;
            if (a->occl > 0.001f) lpf = fminf(lpf, expf(logf(20000.0f) + (logf(fmaxf(at->occlusion_lpf_hz, 20.0f)) - logf(20000.0f)) * a->occl));
        }
        if (at->reverb_send) {
            float t = at->reverb_dist_max > at->reverb_dist_min ? clampf((dist - at->reverb_dist_min) / (at->reverb_dist_max - at->reverb_dist_min), 0.0f, 1.0f) : 0.0f;
            send = at->reverb_wet_min + (at->reverb_wet_max - at->reverb_wet_min) * t;
        }
        if (at->doppler > 0.0f && dist > 1.0f) { // the speeds along the line between them
            float u[3] = { d[0] / dist, d[1] / dist, d[2] / dist };
            float vs = a->vel[0] * u[0] + a->vel[1] * u[1] + a->vel[2] * u[2];          // the source away (+)
            float vl = G.lvel[0] * u[0] + G.lvel[1] * u[1] + G.lvel[2] * u[2];          // the listener toward it (+)
            float dop = (SPEED_OF_SOUND + clampf(vl, -SPEED_OF_SOUND * 0.5f, SPEED_OF_SOUND * 0.5f)) /
                        (SPEED_OF_SOUND + clampf(vs, -SPEED_OF_SOUND * 0.5f, SPEED_OF_SOUND * 0.5f));
            pitch *= 1.0f + (clampf(dop, 0.5f, 2.0f) - 1.0f) * at->doppler;
        }
    }
    if (a->spatial && G.hooks.zone) G.hooks.zone(a->pos, &gain, &lpf); // (Audio Volumes: inside / outside)
    a->audible = gain;
    // the class's passive mix (ducking): pushed while this plays above its threshold
    const AuClass* cl = au_class(a->cls);
    if (cl && cl->passive_mix && gain >= cl->passive_threshold && npassive < 16) passive[npassive++] = cl->passive_mix;
    uint32_t bus = au_bus_of_submix(ce.submix);
    float prio = a->plan.priority * (at ? 1.0f - at->priority_falloff * (1.0f - clampf(gain, 0.0f, 1.0f)) : 1.0f);
    for (uint32_t k = 0; k < a->nv; k++) {
        if (a->vdone[k]) continue;
        uint32_t s = a->vslot[k];
        VoiceParams* P = &M.params[s];
        const SoundWave* w = a->plan.wave[k];
        P->gain = gain * a->plan.volume[k] * w->h.volume;
        P->pitch = pitch * a->plan.pitch[k] * w->h.pitch;
        P->az = az, P->el = el, P->spatial = spatial, P->spread = spread;
        P->lpf = lpf, P->hpf = 0.0f, P->send = send, P->bus = bus, P->binaural = binaural, P->paused = paused;
        float key = P->gain * prio + (ce.always ? 100.0f : 0.0f) + (a->p.preview ? 1000.0f : 0.0f);
        if (paused) key = -1.0f;
        if (ranks) ranks[nrank++] = (Rank){ key, s, k };
    }
    // streaming: the next 2 seconds of a long sound paged in ahead of the mixer (4 times a second)
#ifdef _WIN32
    if (G.prefetch && now - G.prefetch_t > 0.25)
        for (uint32_t k = 0; k < a->nv; k++) {
            const SoundWave* w = a->plan.wave[k];
            if (!(w->h.flags & SND_STREAM) || a->vdone[k]) continue;
            int32_t f = re_atomic_load(&M.played[a->vslot[k]]);
            if (f < 0) f = 0;
            size_t bytes = (size_t)w->h.sample_rate * 2 * w->h.channels * 2, off = (size_t)f * 2 * w->h.channels;
            size_t total = (size_t)w->h.frames * 2 * w->h.channels;
            if (off >= total) continue;
            if (off + bytes > total) bytes = total - off;
            struct { void* va; SIZE_T n; } r = { (void*)((const uint8_t*)w->pcm + off), bytes };
            G.prefetch(GetCurrentProcess(), 1, &r, 0);
        }
#endif
    *npassive_ = npassive;
    if (nrank_) *nrank_ = nrank;
}

void audio_update(float dt) {
    if (!M.on) return;
    if (dt <= 0.0f || dt > 0.5f) dt = 1.0f / 60.0f;
    const AuProject* pr = au_project();
    M.master_volume = M.user_volume * clampf(pr->master_volume, 0.0f, 1.0f);
    // the mixer's let-go voices: their slots free, their sounds finished when all are
    for (uint32_t s = 0; s < AU_VOICES; s++)
        if (G.vused[s] && re_atomic_load(&M.ended[s]) == (int32_t)G.vgen[s]) {
            G.vused[s] = 0;
            uint32_t o = G.vowner[s];
            if (o < AU_SOUNDS && G.s[o].used)
                for (uint32_t k = 0; k < G.s[o].nv; k++)
                    if (G.s[o].vslot[k] == s && G.s[o].vgen[k] == G.vgen[s]) G.s[o].vdone[k] = 1;
        }
    uint32_t passive[16], npassive = 0;
    Rank ranks[AU_VOICES];
    uint32_t nrank = 0;
    double now = re_time_seconds();
    // concurrency ducking: each sound's newer siblings in its group
    for (uint32_t i = 0; i < AU_SOUNDS; i++) G.s[i].duck = 1.0f;
    for (uint32_t i = 0; i < AU_SOUNDS; i++) {
        ActiveSound* a = &G.s[i];
        if (!a->used || !a->conc) continue;
        const AuConcurrency* c = au_concurrency(a->conc);
        if (!c || c->volume_scale >= 1.0f || c->volume_scale <= 0.0f) continue;
        for (uint32_t j = 0; j < AU_SOUNDS; j++)
            if (G.s[j].used && G.s[j].group == a->group && G.s[j].order > a->order) a->duck *= c->volume_scale;
    }
    for (uint32_t i = 0; i < AU_SOUNDS; i++) {
        ActiveSound* a = &G.s[i];
        if (!a->used) continue;
        bool all_done = true;
        for (uint32_t k = 0; k < a->nv; k++) all_done &= a->vdone[k] != 0;
        if (all_done) {
            sound_release(a, true);
            continue;
        }
        update_sound(i, dt, now, passive, &npassive, ranks, &nrank);
    }
    if (G.prefetch && now - G.prefetch_t > 0.25) G.prefetch_t = now;
    // virtualization: the loudest (by priority) are real, up to the project's channels; silent ones always virtual
    qsort(ranks, nrank, sizeof *ranks, rank_cmp);
    uint32_t maxr = pr->max_channels ? pr->max_channels : 64;
    for (uint32_t r = 0; r < nrank; r++) M.params[ranks[r].s].virt = (r >= maxr || ranks[r].key < 1e-4f) && ranks[r].key > -0.5f;
    mixes_step(dt, passive, npassive);
    reverb_step(dt);
}

uint32_t audio_list(AudioInfo* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < AU_SOUNDS && n < cap; i++) {
        const ActiveSound* a = &G.s[i];
        if (!a->used) continue;
        bool virt = true;
        for (uint32_t k = 0; k < a->nv; k++) virt &= M.params[a->vslot[k]].virt != 0;
        out[n++] = (AudioInfo){ a->p.sound, a->p.owner, a->audible, a->dist, virt, a->paused != 0 };
    }
    return n;
}
