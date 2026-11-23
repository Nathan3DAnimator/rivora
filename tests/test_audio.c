// The audio mixer's tests (notas-dev/audio.md): the silent device driven by the test (audio_render_test), the real
// mixing: a 2D sine at its level, a 3D source on the right louder on the right (panning and binaural), behind a
// distance quieter, the limiter holding loud sums under its ceiling, the reverb's tail, a Sound Cue's random and
// concatenated waves, the concurrency's limit, the virtual voices, the fades; the mixer's cost per block.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "audio_internal.h"
#include "metasound.h"
#include "platform.h"
#include "platform_files.h"

static int g_fail;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static char g_root[600];

static uint32_t make_sine(const char* name, float hz, float seconds, float amp, uint32_t channels) {
    uint32_t rate = 48000, n = (uint32_t)(rate * seconds);
    int16_t* pcm = malloc(sizeof(int16_t) * n * channels);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < channels; c++) pcm[i * channels + c] = (int16_t)(sinf(2.0f * 3.14159265f * hz * i / rate) * amp * 32767.0f);
    char path[800], vp[200];
    snprintf(path, sizeof path, "%s/%s.rsound", g_root, name);
    bool ok = audio_wave_write(path, pcm, n, channels, rate, false);
    free(pcm);
    CHECK(ok, "write %s", name);
    snprintf(vp, sizeof vp, "/Content/%s.rsound", name);
    audio_rescan();
    return audio_vpath_hash(vp);
}

// RMS of each side over n frames mixed now (after skipping some: the ramps)
static void measure(uint32_t skip, uint32_t n, float out_rms[2], float* peak) {
    static float buf[2 * 48000];
    audio_update(1.0f / 60.0f);
    audio_render_test(buf, skip);
    audio_update(1.0f / 60.0f);
    audio_render_test(buf, n);
    double s[2] = { 0, 0 };
    float pk = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        s[0] += buf[2 * i] * buf[2 * i], s[1] += buf[2 * i + 1] * buf[2 * i + 1];
        pk = fmaxf(pk, fmaxf(fabsf(buf[2 * i]), fabsf(buf[2 * i + 1])));
    }
    out_rms[0] = (float)sqrt(s[0] / n), out_rms[1] = (float)sqrt(s[1] / n);
    if (peak) *peak = pk;
}

// RMS of each of nch channels over n frames mixed now (after skipping some)
static void measure_n(uint32_t nch, uint32_t skip, uint32_t n, float out_rms[8]) {
    static float buf[8 * 48000];
    audio_update(1.0f / 60.0f);
    for (uint32_t done = 0; done < skip;) { // (skipped in chunks: the buffer holds a second)
        uint32_t k = skip - done > 24000 ? 24000 : skip - done;
        audio_render_test(buf, k);
        done += k;
    }
    audio_update(1.0f / 60.0f);
    audio_render_test(buf, n);
    for (uint32_t c = 0; c < nch; c++) {
        double s = 0.0;
        for (uint32_t i = 0; i < n; i++) s += buf[i * nch + c] * buf[i * nch + c];
        out_rms[c] = (float)sqrt(s / n);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    _putenv("REALENGINE_NO_AUDIO=1"); // (the silent device: nothing heard, the test drives the mixer)
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", getenv("TEMP") ? getenv("TEMP") : ".");
    snprintf(g_root, sizeof g_root, "%s/re_audio_test", tmp);
    for (char* c = g_root; *c; c++)
        if (*c == '\\') *c = '/';
    re_remove_tree(g_root);
    re_make_dirs(g_root);
    CHECK(audio_init(g_root), "audio_init");
    float rms[2], pk;
    float fwd[3] = { 1, 0, 0 }, up[3] = { 0, 0, 1 }, zero[3] = { 0, 0, 0 };
    audio_set_listener(zero, fwd, up, NULL);

    // 1. a 2D mono sine: both sides at -3 dB (equal power) of its RMS (0.5 * 0.707 = 0.354 -> 0.25)
    uint32_t sine = make_sine("Sine440", 440.0f, 2.0f, 0.5f, 1);
    AudioHandle h = audio_play_2d(sine, 1.0f, 1.0f);
    CHECK(h != 0, "play 2D");
    measure(2048, 9600, rms, &pk);
    CHECK(fabsf(rms[0] - 0.25f) < 0.02f && fabsf(rms[1] - 0.25f) < 0.02f, "2D level %.3f %.3f (0.25)", rms[0], rms[1]);
    CHECK(audio_is_playing(h), "playing");
    audio_stop(h, 0.0f);
    measure(512, 4800, rms, NULL);
    CHECK(rms[0] < 1e-4f && rms[1] < 1e-4f, "stopped %.5f", rms[0]);

    // 2. 3D, panning: a source 3 m on the right (+Y) is louder on the right; on the left, the other way
    AuProject pr = { 64, 0, 1.0f, 0, 0, 0, 0 };
    audio_set_project(&pr);
    float right[3] = { 0, 300, 0 }, left[3] = { 0, -300, 0 };
    h = audio_play_at(sine, right, 1.0f, 1.0f);
    measure(2048, 9600, rms, NULL);
    CHECK(rms[1] > rms[0] * 4.0f, "panned right %.3f %.3f", rms[0], rms[1]);
    audio_set_pos(h, left);
    measure(4096, 9600, rms, NULL);
    CHECK(rms[0] > rms[1] * 4.0f, "panned left %.3f %.3f", rms[0], rms[1]);
    // far away: quieter (Natural Sound, 4 m inside, 36 m of falloff)
    float far[3] = { 2000, 0, 0 };
    audio_set_pos(h, far);
    float near_l = rms[0];
    measure(4096, 9600, rms, NULL);
    CHECK(rms[0] < near_l * 0.3f && rms[0] > 0.0f, "far %.4f (near %.3f)", rms[0], near_l);
    audio_stop(h, 0.0f);

    // 3. binaural: on the right, the right ear louder and earlier; in front, both the same
    pr.binaural = 1;
    audio_set_project(&pr);
    // (the head's shadow is a high frequency's: at 440 Hz only ~2 dB, as in life; at 4 kHz much more)
    uint32_t hi = make_sine("Sine4k", 4000.0f, 2.0f, 0.5f, 1);
    h = audio_play_at(hi, right, 1.0f, 1.0f);
    measure(2048, 9600, rms, NULL);
    CHECK(rms[1] > rms[0] * 2.0f, "binaural right %.3f %.3f", rms[0], rms[1]);
    float front[3] = { 300, 0, 0 };
    audio_set_pos(h, front);
    measure(4096, 9600, rms, NULL);
    CHECK(fabsf(rms[0] - rms[1]) < 0.01f, "binaural front %.3f %.3f", rms[0], rms[1]);
    audio_stop(h, 0.0f);
    pr.binaural = 0;
    audio_set_project(&pr);

    // 4. the limiter: 8 loud sines at once stay under -1 dBFS
    uint32_t loud = make_sine("Loud", 220.0f, 2.0f, 0.95f, 2);
    AudioHandle many[8];
    for (int k = 0; k < 8; k++) many[k] = audio_play_2d(loud, 1.0f, 1.0f);
    measure(4096, 9600, rms, &pk);
    CHECK(pk <= 0.8913f + 1e-4f && pk > 0.5f, "limited peak %.4f (<= 0.891)", pk);
    AudioStats st;
    audio_stats(&st);
    CHECK(st.limiter_db < -3.0f, "limiter reduction %.1f dB", st.limiter_db);
    for (int k = 0; k < 8; k++) audio_stop(many[k], 0.0f);
    measure(512, 2400, rms, NULL);

    // 5. the reverb: a short 3D blip leaves a tail after it ends
    uint32_t blip = make_sine("Blip", 1000.0f, 0.05f, 0.8f, 1);
    float side[3] = { 800, 0, 0 };
    h = audio_play_at(blip, side, 1.0f, 1.0f);
    measure(0, 4800, rms, NULL);   // the blip (0.05 s) and its start
    measure(0, 9600, rms, NULL);   // 0.1 - 0.3 s: only the reverb
    CHECK(rms[0] > 1e-4f, "reverb tail %.5f", rms[0]);
    measure(48000, 4800, rms, NULL); // ~1.4 s later: decayed
    measure(48000, 4800, rms, NULL);
    CHECK(rms[0] < 2e-3f, "reverb decayed %.5f", rms[0]);

    // 6. a Sound Cue: Random of two waves (no repeats: they alternate), then a Concatenator of two blips plays both
    uint32_t a440 = make_sine("A440", 440.0f, 0.3f, 0.5f, 1), a880 = make_sine("A880", 880.0f, 0.3f, 0.5f, 1);
    AuAsset* cue = au_asset_create(AUA_CUE, "/Content/SCue_Random.rscue");
    CHECK(cue != NULL, "cue made");
    if (cue) {
        AuCue* c = &cue->u.cue;
        c->nnodes = 4;
        c->n[0].nin = 1, c->n[0].in[0] = 1;
        c->n[1] = (AuCueNode){ CUE_RANDOM, 2, 1, 0, { 2, 3, -1, -1, -1, -1, -1, -1 }, 0, 0, 0, { 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, -1 };
        c->n[2] = (AuCueNode){ CUE_WAVE, 0, 0, 0, { -1, -1, -1, -1, -1, -1, -1, -1 }, 0, 0, a440, { 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, -1 };
        c->n[3] = (AuCueNode){ CUE_WAVE, 0, 0, 0, { -1, -1, -1, -1, -1, -1, -1, -1 }, 0, 0, a880, { 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, -1 };
        CHECK(au_asset_save(cue), "cue saved");
        AuPlan p1, p2;
        CHECK(au_plan(cue->hash, &p1) && au_plan(cue->hash, &p2), "cue plans");
        CHECK(p1.n == 1 && p2.n == 1 && p1.wave[0] != p2.wave[0], "no repeats");
        c->n[1].kind = CUE_CONCAT;
        CHECK(au_plan(cue->hash, &p1) && p1.n == 2 && fabsf(p1.delay[1] - 0.3f) < 0.01f, "concatenated: %u waves, second at %.3f s", p1.n, p1.n > 1 ? p1.delay[1] : -1.0f);
    }

    // 7. concurrency: at most 2 of a group (stop oldest)
    AuAsset* conc = au_asset_create(AUA_CONCURRENCY, "/Content/SCon_Two.rsconc");
    if (conc) {
        conc->u.conc.max_count = 2, conc->u.conc.rule = AU_RES_STOP_OLDEST;
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = sine, p.concurrency = conc->hash, p.bone = -1;
        AudioHandle c1 = audio_play(&p), c2 = audio_play(&p), c3 = audio_play(&p);
        for (int k = 0; k < 6; k++) measure(256, 256, rms, NULL);
        CHECK(c3 && !audio_is_playing(c1) && audio_is_playing(c2) && audio_is_playing(c3), "concurrency: oldest stopped");
        p.concurrency = 0;
        audio_stop(c2, 0.0f), audio_stop(c3, 0.0f);
    }

    // 8. many voices: 200 playing, the project's 64 real, the rest virtual; the mixer's cost
    pr.max_channels = 64;
    audio_set_project(&pr);
    // (one sound 200 times: the project's default concurrency allows 16 of a sound, as Unreal's; a group without a
    // limit here)
    AuAsset* unl = au_asset_create(AUA_CONCURRENCY, "/Content/SCon_Unlimited.rsconc");
    if (unl) unl->u.conc.max_count = 0;
    AudioHandle lots[200];
    for (int k = 0; k < 200; k++) {
        AudioPlay lp;
        memset(&lp, 0, sizeof lp);
        lp.sound = sine, lp.pitch = 1.0f + 0.001f * k, lp.spatial = true, lp.bone = -1, lp.concurrency = unl ? unl->hash : 0;
        lp.pos[0] = (float)(k % 20) * 100.0f, lp.pos[1] = (float)(k / 20) * 100.0f;
        lots[k] = audio_play(&lp);
    }
    double t0 = re_time_seconds();
    for (int k = 0; k < 20; k++) measure(0, 480, rms, NULL);
    double ms = (re_time_seconds() - t0) * 1000.0 / 40.0; // per 10 ms period (two renders per measure)
    audio_stats(&st);
    printf("[RealEngine] Audio: 200 voices (%u real, %u virtual): %.3f ms per 10 ms period, peak %.2f\n", st.real, st.virt, ms, pk);
    CHECK(st.real <= 64 && st.virt >= 100, "virtualized: %u real, %u virtual", st.real, st.virt);
    CHECK(ms < 1.5, "mixer cost %.3f ms per period", ms);
    for (int k = 0; k < 200; k++) audio_stop(lots[k], 0.0f);
    for (int k = 0; k < 10; k++) measure(0, 24000, rms, NULL); // (5 s: the reverb's tail gone)

    // 9. a fade in: quiet at first, full after
    h = audio_play_2d(sine, 1.0f, 1.0f);
    audio_stop(h, 0.0f);
    AudioPlay fp;
    memset(&fp, 0, sizeof fp);
    fp.sound = sine, fp.fade_in = 0.5f, fp.bone = -1;
    h = audio_play(&fp);
    measure(0, 480, rms, NULL);
    float first = rms[0];
    audio_stats(&st);
    for (int k = 0; k < 40; k++) measure(0, 480, rms, NULL); // (40 game frames of 1/60 s > 0.5 s)
    CHECK(first < rms[0] * 0.5f && rms[0] > 0.2f, "fade in %.3f -> %.3f", first, rms[0]);
    audio_stop(h, 0.0f);
    for (int k = 0; k < 4; k++) measure(0, 4800, rms, NULL);

    // 10. MetaSounds (B8, metasound.h): the engine's own (made by audio_init) rendered offline - the engine's pitch with
    // its RPM, the rain and the wind louder with their inputs -, their cost, then the engine through the mixer with its
    // input set while it plays (Set Float Parameter)
    {
        const MsAsset* eng = ms_find(ms_engine_sound());
        const MsAsset* rain = ms_find(ms_rain_sound());
        const MsAsset* wind = ms_find(ms_wind_sound());
        CHECK(eng && rain && wind, "MetaSounds made: engine %d, rain %d, wind %d", eng != NULL, rain != NULL, wind != NULL);
        if (eng && rain && wind) {
            static MsState st;
            static float buf[256];
            float prm[MS_MAX_PARAMS];
            double zc[2], re[2];
            for (int k = 0; k < 2; k++) {
                memset(prm, 0, sizeof prm);
                ms_reset(&st, eng);
                prm[ms_param_index(eng, "RPM")] = k ? 6000.0f : 1000.0f, prm[ms_param_index(eng, "Load")] = 0.5f;
                uint32_t cross = 0, total = 0;
                double ss = 0.0;
                float prev = 0.0f;
                for (int b = 0; b < 375; b++) { // (2 s; the first 0.4 s the glides settle)
                    ms_render(&st, prm, buf, 256, 48000.0f, 1.0f);
                    if (b < 75) continue;
                    for (int i = 0; i < 256; i++) cross += (buf[i] >= 0.0f) != (prev >= 0.0f), prev = buf[i], ss += (double)buf[i] * buf[i], total++;
                }
                zc[k] = cross / (total / 48000.0), re[k] = sqrt(ss / total);
            }
            printf("[RealEngine] MetaSound engine: 1000 RPM %.0f crossings/s (rms %.3f), 6000 RPM %.0f crossings/s (rms %.3f)\n", zc[0], re[0], zc[1], re[1]);
            CHECK(re[0] > 0.02 && re[1] > 0.02 && zc[1] > zc[0] * 2.0, "engine: its pitch follows the RPM");
            const MsAsset* two[2] = { rain, wind };
            const char* pn[2] = { "Intensity", "Speed" };
            const float lo[2] = { 2.0f, 2.0f }, hi[2] = { 60.0f, 20.0f };
            for (int s = 0; s < 2; s++) {
                double r2[2];
                for (int k = 0; k < 2; k++) {
                    memset(prm, 0, sizeof prm);
                    ms_reset(&st, two[s]);
                    prm[ms_param_index(two[s], pn[s])] = k ? hi[s] : lo[s];
                    double ss = 0.0;
                    uint32_t total = 0;
                    for (int b = 0; b < 750; b++) { // (4 s; the first 3 s its slow glide settles)
                        ms_render(&st, prm, buf, 256, 48000.0f, 1.0f);
                        if (b < 560) continue;
                        for (int i = 0; i < 256; i++) ss += (double)buf[i] * buf[i], total++;
                    }
                    r2[k] = sqrt(ss / total);
                }
                printf("[RealEngine] MetaSound %s: %s %.0f rms %.4f, %.0f rms %.4f\n", two[s]->name, pn[s], lo[s], r2[0], hi[s], r2[1]);
                CHECK(r2[1] > r2[0] * 2.0 && r2[1] > 0.01, "%s louder with its %s", two[s]->name, pn[s]);
            }
            ms_reset(&st, eng);
            double t1 = re_time_seconds();
            for (int b = 0; b < 1875; b++) ms_render(&st, NULL, buf, 256, 48000.0f, 1.0f); // (10 s)
            double us = (re_time_seconds() - t1) * 1e6 / 1000.0; // (per 10 ms of sound)
            printf("[RealEngine] MetaSound engine: %u nodes, %.1f us per 10 ms of sound\n", eng->norder, us);
            CHECK(us < 200.0, "MetaSound cost %.1f us per 10 ms", us);
            h = audio_play_2d(ms_engine_sound(), 1.0f, 1.0f);
            bool set = h && audio_set_param(h, "RPM", 3000.0f), none = h && audio_set_param(h, "NoSuchInput", 1.0f);
            measure(4800, 9600, rms, NULL);
            CHECK(h && set && !none && rms[0] > 0.01f, "the engine through the mixer: %.4f (its RPM set %d)", rms[0], set);
            audio_stop(h, 0.0f);
            measure(0, 4800, rms, NULL);
        }
    }
    audio_shutdown();

    // 10. surround: the silent device as 5.1, then 7.1; a source around the listener lands on the speakers around it
    // (VBAP), a 2D sound on the front pair only, the subwoofer gets the lows of 3D sounds, the reverb reaches the backs
    uint32_t low = make_sine("Low60", 60.0f, 2.0f, 0.5f, 1);
    for (int layout = 0; layout < 2; layout++) {
        uint32_t nch = layout ? 8u : 6u;
        _putenv(layout ? "REALENGINE_AUDIO_CHANNELS=8" : "REALENGINE_AUDIO_CHANNELS=6");
        CHECK(audio_init(g_root), "audio_init %u channels", nch);
        AudioStats ss;
        audio_stats(&ss);
        CHECK(ss.channels == nch, "the silent device's channels %u (%u)", ss.channels, nch);
        audio_set_listener(zero, fwd, up, NULL);
        // the channels: FL FR FC LFE BL BR (SL SR)
        enum { FL = 0, FR, FC, LFE, BL, BR, SL, SR };
        struct { float deg; int want; } at[] = { { 0.0f, FC }, { 30.0f, FR }, { -30.0f, FL }, { layout ? -150.0f : -110.0f, BL },
                                                 { layout ? 150.0f : 110.0f, BR }, { 90.0f, layout ? SR : -1 }, { -90.0f, layout ? SL : -1 } };
        for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
            if (at[k].want < 0) continue;
            float p[3] = { cosf(at[k].deg * 3.14159265f / 180.0f) * 300.0f, sinf(at[k].deg * 3.14159265f / 180.0f) * 300.0f, 0.0f };
            AudioHandle s = audio_play_at(sine, p, 1.0f, 1.0f);
            float r[8];
            measure_n(nch, 4096, 9600, r);
            int best = 0;
            for (uint32_t c = 1; c < nch; c++)
                if (r[c] > r[best]) best = (int)c;
            CHECK(best == at[k].want && r[best] > 0.1f, "%s at %.0f deg: loudest channel %d (want %d), %.3f", layout ? "7.1" : "5.1", at[k].deg, best, at[k].want, r[best]);
            audio_stop(s, 0.0f);
            measure_n(nch, 48000, 480, r); // (its reverb gone)
            measure_n(nch, 48000, 480, r);
        }
        // a 2D stereo sound: the front pair, nothing behind
        AudioHandle s2 = audio_play_2d(loud, 0.3f, 1.0f);
        float r[8];
        measure_n(nch, 4096, 9600, r);
        CHECK(r[FL] > 0.05f && r[FR] > 0.05f && r[BL] < 1e-3f && r[BR] < 1e-3f && r[FC] < 1e-3f, "%u ch 2D: front %.3f %.3f, center %.4f, back %.4f %.4f",
              nch, r[FL], r[FR], r[FC], r[BL], r[BR]);
        audio_stop(s2, 0.0f);
        measure_n(nch, 48000, 480, r);
        // the subwoofer: a 60 Hz 3D sound bleeds into it
        float pl[3] = { 300.0f, 0.0f, 0.0f };
        AudioHandle s3 = audio_play_at(low, pl, 1.0f, 1.0f);
        measure_n(nch, 4096, 9600, r);
        CHECK(r[LFE] > 0.03f, "%u ch LFE %.4f", nch, r[LFE]);
        audio_stop(s3, 0.0f);
        measure_n(nch, 48000, 480, r);
        measure_n(nch, 48000, 480, r);
        // the reverb of a blip in front reaches the backs (a diffuse field around the room)
        float pf[3] = { 800.0f, 0.0f, 0.0f };
        audio_play_at(blip, pf, 1.0f, 1.0f);
        measure_n(nch, 0, 4800, r);
        measure_n(nch, 0, 9600, r);
        CHECK(r[BL] > 1e-4f && r[BR] > 1e-4f, "%u ch reverb behind %.5f %.5f", nch, r[BL], r[BR]);
        double t1 = re_time_seconds();
        for (int k = 0; k < 20; k++) measure_n(nch, 0, 480, r);
        printf("[RealEngine] Audio %s: %.3f ms per 10 ms period (reverb tail)\n", layout ? "7.1" : "5.1", (re_time_seconds() - t1) * 1000.0 / 40.0);
        audio_shutdown();
    }
    _putenv("REALENGINE_AUDIO_CHANNELS=");
    re_remove_tree(g_root);
    if (g_fail) printf("[RealEngine] FAIL: %d audio checks\n", g_fail);
    else printf("[RealEngine] Audio: all checks passed\n");
    return g_fail ? 1 : 0;
}
