// --seq-test (sugestão #45, notas-dev/sequencer.md): a Level Sequence made here (a cube's Transform keys, Camera Cuts to
// a camera actor, an Event that a Blueprint's Custom Event answers) played by a Blueprint's BeginPlay -> Play Sequence
// in Play in Editor; the check: the cube at its last key, the event answered, the sequence over, the Camera Cuts' view.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "anim.h"
#include "content.h"
#include "editor.h"
#include "platform_files.h"
#include "sequencer.h"
#include "sequencer_ui.h"

static bool g_fail;
static Entity g_actor, g_cube, g_cam, g_man;
static uint32_t g_view_frames; // frames the Camera Cuts gave the view (seq_test_frame)
static uint32_t g_man_clip_frames; // (Q23) frames the Animation track's clip drove the Mannequin (its Anim BP off)

static int pin_of_name(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] seq-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
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

static Entity named(World* w, uint64_t mask, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, mask | COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE;
    return e;
}

bool seq_test_setup(Editor* ed) {
    World* w = &ed->world;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/SeqTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    seq_reload_all();
    g_cube = named(w, COMP_BIT(COMP_STATIC_MESH), "SeqCube", 0, 0, 100);
    CStaticMesh* m = ecs_get(w, g_cube, COMP_STATIC_MESH);
    if (m) m->mesh = SHAPE_CUBE, m->base_color[0] = 0.9f, m->base_color[1] = 0.3f, m->base_color[2] = 0.1f, m->roughness = 0.5f;
    g_cam = named(w, COMP_BIT(COMP_CAMERA), "SeqCam", -600, 0, 250);
    CTransform* ct = ecs_get(w, g_cam, COMP_TRANSFORM);
    if (ct) ct->rotation[0] = -15.0f, ct->rotation[1] = 0.0f;
    CCamera* cc = ecs_get(w, g_cam, COMP_CAMERA);
    if (cc) cc->fov_deg = 70.0f;
    // the sequence: 2.5 s; the cube from x 0 to 400 in 2 s; the view from SeqCam; SeqPing at 1 s
    Sequence* s = seq_create("/Content/SeqTest", "LS_Test");
    if (!s) return printf("[RealEngine] FAIL: seq-test: could not make the Level Sequence\n"), false;
    s->length = 2.5f;
    SeqKey k;
    memset(&k, 0, sizeof k);
    snprintf(k.name, sizeof k.name, "SeqCam");
    seq_add_key(&s->tracks[0], &k); // (Camera Cuts: the new sequence's first track)
    int tt = seq_add_track(s, SEQT_TRANSFORM, "SeqCube");
    for (int i = 0; i < 2; i++) {
        memset(&k, 0, sizeof k);
        k.t = i ? 2.0f : 0.0f;
        k.v[0] = i ? 400.0f : 0.0f, k.v[2] = 100.0f, k.v[4] = i ? 90.0f : 0.0f;
        k.v[6] = k.v[7] = k.v[8] = 1.0f;
        seq_add_key(&s->tracks[tt], &k);
    }
    { // (Q23) a Mannequin with its Anim BP: an Animation track plays a clip at 0.5 s; when the sequence ends the Anim BP
      // drives it again (before, it stayed in the clip's Animation mode for good)
        g_man = named(w, COMP_BIT(COMP_SKELETAL_MESH), "SeqMan", 0, 300, 0);
        CSkeletalMesh* sk = g_man != ENTITY_NONE ? ecs_get(w, g_man, COMP_SKELETAL_MESH) : NULL;
        const SkelMesh* mm = anim_mesh_name("SK_Mannequin");
        const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
        if (sk && mm && abp) {
            sk->mesh = mm->hash, sk->anim_class = abp->hash, sk->play_rate = 1.0f, sk->loop = 1;
            int ta = seq_add_track(s, SEQT_ANIMATION, "SeqMan");
            memset(&k, 0, sizeof k);
            k.t = 0.5f;
            snprintf(k.name, sizeof k.name, "MM_Idle");
            if (ta >= 0) seq_add_key(&s->tracks[ta], &k), s->tracks[ta].loop_anim = true;
        } else
            g_man = ENTITY_NONE, printf("[RealEngine] seq-test: no Mannequin here: the Animation track's end not checked\n");
    }
    int te = seq_add_track(s, SEQT_EVENT, "");
    memset(&k, 0, sizeof k);
    k.t = 1.0f;
    snprintf(k.name, sizeof k.name, "SeqPing");
    seq_add_key(&s->tracks[te], &k);
    { // (2026-11-10) the keys' interpolations (Unreal's): SeqCam's Field Of View 70 Constant at 0, 50 Linear at 0.5,
      // 90 User (slope 0) at 1, 90 Auto at 2.5: a step, a line, a flat-ended cubic; saved and read back
        int tf = seq_add_track(s, SEQT_FOV, "SeqCam");
        static const float kt[4] = { 0.0f, 0.5f, 1.0f, 2.5f }, kv[4] = { 70.0f, 50.0f, 90.0f, 90.0f };
        static const uint8_t ki[4] = { SEQI_CONSTANT, SEQI_LINEAR, SEQI_USER, SEQI_AUTO };
        for (int i = 0; i < 4 && tf >= 0; i++) {
            memset(&k, 0, sizeof k);
            k.t = kt[i], k.v[0] = kv[i], k.interp = ki[i];
            seq_add_key(&s->tracks[tf], &k);
        }
    }
    if (!seq_save(s)) return printf("[RealEngine] FAIL: seq-test: could not save LS_Test\n"), false;
    seq_reload_all(); // (read back from its file: the format round trip)
    if (!seq_editor_selftest()) return printf("[RealEngine] FAIL: seq-test: the Sequencer's undo\n"), false;
    {
        Sequence* r = seq_load("LS_Test");
        const SeqTrack* f = NULL;
        for (uint32_t i = 0; r && i < r->ntracks; i++)
            if (r->tracks[i].type == SEQT_FOV) f = &r->tracks[i];
        bool ok = f && f->nkeys == 4 && f->keys[0].interp == SEQI_CONSTANT && f->keys[1].interp == SEQI_LINEAR && f->keys[2].interp == SEQI_USER &&
                  f->keys[3].interp == SEQI_AUTO;
        float a1 = ok ? seq_channel_at(f, 0, 0.25f) : -1.0f, a2 = ok ? seq_channel_at(f, 0, 0.75f) : -1.0f;
        float a3 = ok ? seq_channel_at(f, 0, 1.75f) : -1.0f, a4 = ok ? seq_channel_at(f, 0, 0.5f) : -1.0f;
        printf("[RealEngine] seq-test: interpolations Constant %.1f, Linear %.1f, User->Auto %.1f, key %.1f\n", a1, a2, a3, a4);
        if (!ok || fabsf(a1 - 70.0f) > 0.01f || fabsf(a2 - 70.0f) > 0.01f || fabsf(a3 - 90.0f) > 0.01f || fabsf(a4 - 50.0f) > 0.01f)
            return printf("[RealEngine] FAIL: seq-test: the keys' interpolations\n"), false;
    }
    // the Blueprint: BeginPlay -> Play Sequence (LS_Test); Custom Event SeqPing -> Pings = 1
    // (a name of its own each run: the Blueprints' registry keeps a deleted file's asset, and a class of the same path
    // read at startup from the last run's file would be the one the actor runs)
    char cls[48];
    snprintf(cls, sizeof cls, "BP_SeqTest_%u", (unsigned)(re_unix_time() % 1000000));
    BpAsset* a = bp_create(BPA_CLASS, "/Content/SeqTest", cls, "Actor");
    if (!a) return printf("[RealEngine] FAIL: seq-test: no Blueprint\n"), false;
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, BP_NAME, "Pings");
    v->type = BPT_INT;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t beg = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL);
    uint32_t ps = node(a, g, BPK_PLAY_SEQUENCE, 1, "LS_Test");
    link(g, beg, "", ps, "");
    uint32_t ev = node(a, g, BPK_EVENT_CUSTOM, 0, "SeqPing");
    uint32_t set = node(a, g, BPK_VAR_SET, 1, "Pings");
    bp_node(g, set)->pins[pin_of_name(g, set, 0, "Pings")].lit.v[0] = 1.0f;
    link(g, ev, "", set, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) return printf("[RealEngine] FAIL: seq-test: the Blueprint didn't compile: %s\n", a->error), false;
    g_actor = bp_spawn(w, a, (float[3]){ 0, -400, 0 }, (float[3]){ 0, 0, 0 });
    editor_play(ed, 0.0);
    return g_actor != ENTITY_NONE;
}

// Each frame while it plays: the Camera Cuts give the view between 0 and 2.5 s
void seq_test_frame(Editor* ed) {
    float pos[3], yaw, pitch;
    if (ed->playing && seq_view(&ed->world, pos, &yaw, &pitch, NULL) && fabsf(pos[0] + 600.0f) < 1.0f) g_view_frames++;
    if (ed->playing && g_man != ENTITY_NONE && seq_is_playing("LS_Test") && !anim_uses_blueprint(&ed->world, g_man)) g_man_clip_frames++;
}

bool seq_test_check(Editor* ed) {
    World* w = &ed->world;
    const CTransform* t = ecs_get(w, g_cube, COMP_TRANSFORM);
    BpValue pings = { { 0 } };
    bool got = bp_instance_var(w, g_actor, "Pings", &pings);
    bool moved = t && fabsf(t->location[0] - 400.0f) < 0.5f && fabsf(t->rotation[1] - 90.0f) < 0.5f;
    bool event = got && pings.v[0] == 1.0f;
    bool over = !seq_is_playing("LS_Test");
    bp_instance_var(w, g_actor, "Pings", &pings);
    printf("[RealEngine] Sequencer: cube at x %.1f yaw %.1f, Pings %.0f (%u events fired), %s, Camera Cuts' view %u frames, tick %.3f ms\n",
           t ? t->location[0] : -1.0f, t ? t->rotation[1] : -1.0f, pings.v[0], seq_stats()->events_fired, over ? "over" : "still playing",
           g_view_frames, seq_stats()->tick_ms);
    if (!moved || !event || !over || g_view_frames < 10) {
        printf("[RealEngine] FAIL: Sequencer: the Transform track, the Event, the end or the Camera Cuts\n");
        return false;
    }
    if (g_man != ENTITY_NONE) { // (Q23)
        bool back = anim_uses_blueprint(w, g_man);
        printf("[RealEngine] Sequencer: the Animation track drove the Mannequin %u frames; at the end its Anim BP %s (%u restored)\n", g_man_clip_frames,
               back ? "drives it again" : "is OFF", seq_stats()->anim_restored);
        if (g_man_clip_frames < 10 || !back) {
            printf("[RealEngine] FAIL: Sequencer: the Animation track's clip, or the Anim BP back at the end\n");
            return false;
        }
    }
    printf("[RealEngine] Sequencer OK\n");
    return true;
}
