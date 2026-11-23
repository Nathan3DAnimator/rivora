// --audio-test (notas-dev/audio.md): the audio in the editor's Play, as a game uses it. Made here: two Sound Waves (a
// looping hum, a short blip), an Ambient Sound actor with the hum, an Audio Volume around the listener (its reverb), a
// Blueprint with an Audio Component playing the blip (its On Audio Finished event answered), BeginPlay -> Spawn Sound 2D
// (its handle kept) and Play Sound at Location. The check while playing: the hum heard from its side (real, at its
// distance), the blip's component finished, the spawned sound's handle, the mixer's cost; then Play stops and every game
// sound with it.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_internal.h"
#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "platform_files.h"

static bool g_fail;
static Entity g_actor, g_ambient;
static uint32_t g_hum, g_blip;

static int pin_of_name(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] audio-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin_of_name(g, a, 1, ap), b, (uint8_t)pin_of_name(g, b, 0, bpn))) g_fail = true;
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float slot, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, slot * 300.0f, 0.0f, text, NULL);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}

static uint32_t make_wave(const char* name, float hz, float seconds, bool loop) {
    enum { RATE = 48000 };
    uint32_t n = (uint32_t)(RATE * seconds);
    static int16_t pcm[RATE * 2];
    for (uint32_t i = 0; i < n && i < RATE * 2; i++) {
        float env = loop ? 1.0f : fminf(1.0f, (n - i) / 2400.0f); // (the blip ends softly)
        pcm[i] = (int16_t)(sinf(6.2831853f * hz * i / RATE) * 0.4f * env * 32767.0f);
    }
    char path[1100], vp[200];
    snprintf(path, sizeof path, "%s/AudioTest/%s.rsound", content_root(), name);
    snprintf(vp, sizeof vp, "/Content/AudioTest/%s.rsound", name);
    if (!audio_wave_write(path, pcm, n, 1, RATE, false)) return g_fail = true, 0;
    audio_rescan();
    SoundWave* w = audio_wave_load(vp);
    if (!w) return g_fail = true, 0;
    if (loop) w->h.flags |= SND_LOOPING, audio_wave_save(w);
    return w->hash;
}

static Entity named(World* w, uint64_t mask, const char* name, float x, float y, float z, float s) {
    Entity e = ecs_create(w, mask | COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = s;
    t->flags = MOBILITY_MOVABLE;
    return e;
}

bool audio_test_setup(Editor* ed) {
    World* w = &ed->world;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/AudioTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    g_hum = make_wave("AT_Hum", 220.0f, 1.0f, true);
    g_blip = make_wave("AT_Blip", 880.0f, 0.3f, false);
    // the hum on the listener's right (Play starts at the editor's view: the camera looks down +X)
    float pos[3] = { ed->cam.pos[0], ed->cam.pos[1] + 300.0f, ed->cam.pos[2] };
    g_ambient = named(w, COMP_BIT(COMP_AUDIO), "AT_Ambient", pos[0], pos[1], pos[2], 1.0f);
    CAudio* au = ecs_get(w, g_ambient, COMP_AUDIO);
    if (au) au->sound = g_hum;
    Entity vol = named(w, COMP_BIT(COMP_AUDIO_VOLUME), "AT_Room", ed->cam.pos[0], ed->cam.pos[1], ed->cam.pos[2], 20.0f);
    (void)vol;
    // the Blueprint: an Audio Component (the blip, auto-activated); OnAudioFinished -> Finished = 1; BeginPlay ->
    // Spawn Sound 2D (the blip) -> Handle; -> Play Sound at Location (the blip)
    char cls[48];
    snprintf(cls, sizeof cls, "BP_AudioTest_%u", (unsigned)(re_unix_time() % 1000000));
    BpAsset* a = bp_create(BPA_CLASS, "/Content/AudioTest", cls, "Actor");
    if (!a) return printf("[RealEngine] FAIL: audio-test: no Blueprint\n"), false;
    int ci = bp_scs_add(a, SCS_AUDIO, BP_SCS_ROOT, "Blip");
    if (ci < 0) return printf("[RealEngine] FAIL: audio-test: no Audio Component\n"), false;
    CAudio* bc = (CAudio*)a->scs[ci].data;
    bc->sound = g_blip, bc->auto_activate = 1, bc->spatialize = 1, bc->volume = bc->pitch = 1.0f;
    for (int k = 0; k < 2; k++) {
        BpVar* v = &a->vars[a->nvars++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, BP_NAME, k ? "Handle" : "Finished");
        v->type = BPT_INT;
    }
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t beg = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL);
    uint32_t sp = node(a, g, BPK_SPAWN_SOUND_2D, 1, "AT_Blip");
    uint32_t set = node(a, g, BPK_VAR_SET, 2, "Handle");
    uint32_t at = node(a, g, BPK_PLAY_SOUND_AT, 3, "AT_Blip");
    link(g, beg, "", sp, "");
    link(g, sp, "", set, "");
    link(g, sp, "Sound", set, "Handle");
    link(g, set, "", at, "");
    uint32_t ev = node(a, g, BPK_EVENT_CUSTOM, 0, "OnAudioFinished_Blip");
    uint32_t fin = node(a, g, BPK_VAR_SET, 1, "Finished");
    bp_node(g, fin)->pins[pin_of_name(g, fin, 0, "Finished")].lit.v[0] = 1.0f;
    link(g, ev, "", fin, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) return printf("[RealEngine] FAIL: audio-test: the Blueprint didn't compile: %s\n", a->error), false;
    g_actor = bp_spawn(w, a, (float[3]){ ed->cam.pos[0] + 200.0f, ed->cam.pos[1], ed->cam.pos[2] }, (float[3]){ 0, 0, 0 });
    editor_play(ed, 0.0);
    return g_actor != ENTITY_NONE && !g_fail;
}

bool audio_test_check(Editor* ed) {
    World* w = &ed->world;
    AudioInfo info[64];
    uint32_t n = audio_list(info, 64);
    const AudioInfo* hum = NULL;
    for (uint32_t i = 0; i < n; i++)
        if (info[i].sound == g_hum) hum = &info[i];
    BpValue fin = { { 0 } }, hd = { { 0 } };
    bp_instance_var(w, g_actor, "Finished", &fin);
    bp_instance_var(w, g_actor, "Handle", &hd);
    AudioStats st;
    audio_stats(&st);
    printf("[RealEngine] Audio: %u sounds playing (%u real, %u virtual); the hum %s at %.0f cm, %.1f dB; the blip's component %s; "
           "Spawn Sound 2D's handle %.0f; mixer %.3f ms of %.1f ms (worst %.3f); %s\n",
           n, st.real, st.virt, hum ? (hum->virt ? "virtual" : "heard") : "missing", hum ? hum->dist : -1.0f,
           hum ? 20.0 * log10(fmax(hum->audible, 1e-6)) : -999.0, fin.v[0] == 1.0f ? "finished (event answered)" : "not finished",
           hd.v[0], st.mix_ms, st.period_ms, st.mix_ms_max, st.device);
    // (the listener: the Play's view, the Player Start's if the level has one)
    const CTransform* at = ecs_get(w, g_ambient, COMP_TRANSFORM);
    float d[3] = { at ? at->location[0] - ed->render_view.pos[0] : 0, at ? at->location[1] - ed->render_view.pos[1] : 0,
                   at ? at->location[2] - ed->render_view.pos[2] : 0 };
    float want = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    bool ok = hum && !hum->virt && hum->audible > 1e-3f && fabsf(hum->dist - want) < 30.0f && fin.v[0] == 1.0f && hd.v[0] > 0.0f &&
              st.mix_ms < 1.0;
    // Play stops: every game sound with it
    editor_stop(ed, 0.0);
    audio_update(1.0f / 60.0f);
    uint32_t left = audio_list(info, 64);
    if (left) printf("[RealEngine] Audio: %u sounds left after Play stopped\n", left), ok = false;
    if (!ok) {
        printf("[RealEngine] FAIL: Audio: the Ambient Sound, the component's On Audio Finished, Spawn Sound 2D, the cost or the stop\n");
        return false;
    }
    printf("[RealEngine] Audio OK\n");
    return true;
}
