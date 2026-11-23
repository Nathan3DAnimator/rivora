// Cinematics (movie_render.h; notas-dev/sequencer.md "Movie Render Queue e Take Recorder"): the Movie Render Queue
// (a sequence played in PIE on a fixed clock, each output frame the mean of its temporal samples grabbed from the
// viewport, written as PNG), the Take Recorder (actors' transforms keyed into a new Level Sequence while playing), the
// Window > Cinematics panel and --cine-test.
#include "movie_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "sequencer.h"
#include "vkctx.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

// --- Movie Render Queue ---
enum { PH_IDLE = 0, PH_WARMUP, PH_RENDER };
static struct {
    MovieJob job;
    MovieStats st;
    int phase, warm, sub;
    uint32_t total, frame;
    float len;
    bool waiting, game_view_was;
    float* acc;              // the samples' sum (RGB floats)
    uint32_t w, h;
    uint8_t* prev;           // the last frame written (the motion between frames)
    double t0;
    float lum[1024], diff[1024]; // each frame's mean (0-255) and its mean difference from the one before
} M;

const MovieStats* movie_render_stats(void) { return &M.st; }

bool movie_render_start(Editor* ed, const MovieJob* job) {
    if (M.phase != PH_IDLE || !job || !job->sequence[0]) return false;
    Sequence* s = seq_load(job->sequence);
    if (!s || s->length <= 0.0f) {
        snprintf(M.st.status, sizeof M.st.status, "%s", TR("Movie Render Queue: no such sequence (or it has no length)."));
        return false;
    }
    memset(&M.st, 0, sizeof M.st);
    M.job = *job;
    if (M.job.fps <= 0) M.job.fps = 24;
    if (M.job.samples <= 0) M.job.samples = 1;
    if (M.job.warmup < 0) M.job.warmup = 0;
    M.len = s->length;
    M.total = (uint32_t)ceilf(s->length * (float)M.job.fps - 1e-4f);
    if (M.job.max_frames > 0 && (uint32_t)M.job.max_frames < M.total) M.total = (uint32_t)M.job.max_frames;
    if (M.total > 1024) M.total = 1024; // (the stats' arrays; a longer one: render it in parts)
    if (!M.job.out_dir[0]) snprintf(M.job.out_dir, sizeof M.job.out_dir, "%s/../Saved/MovieRenders/%s", content_root(), s->name);
    re_make_dirs(M.job.out_dir);
    snprintf(M.st.out_dir, sizeof M.st.out_dir, "%s", M.job.out_dir);
    M.frame = 0, M.sub = 0, M.warm = 0, M.waiting = false;
    M.st.active = true, M.st.frames = M.total;
    M.game_view_was = ed->game_view;
    ed->game_view = true; // (the icons, the grid: hidden)
    if (!ed->playing) editor_play(ed, re_time_seconds());
    M.phase = PH_WARMUP;
    M.t0 = re_time_seconds();
    printf("[RealEngine] Movie Render Queue: %s, %u frames at %d fps x %d temporal samples -> %s\n", s->name, M.total, M.job.fps,
           M.job.samples, M.job.out_dir);
    return true;
}

static void finish(Editor* ed, bool ok) {
    seq_stop(&ed->world, NULL);
    if (ed->playing) editor_stop(ed, re_time_seconds());
    ed->game_view = M.game_view_was;
    free(M.acc), M.acc = NULL;
    free(M.prev), M.prev = NULL;
    M.phase = PH_IDLE;
    M.st.active = false, M.st.done = true, M.st.ok = ok && M.st.written == M.total;
    M.st.ms = (re_time_seconds() - M.t0) * 1000.0;
    snprintf(M.st.status, sizeof M.st.status, TR("Rendered %u of %u frames in %.1f s: %s"), M.st.written, M.total, M.st.ms / 1000.0, M.job.out_dir);
    printf("[RealEngine] Movie Render Queue: %u of %u frames written, %u samples, %.1f s\n", M.st.written, M.total, M.st.samples_grabbed, M.st.ms / 1000.0);
}

void movie_render_cancel(Editor* ed) {
    if (M.phase != PH_IDLE) finish(ed, false);
}

// a frame's mean of its samples into a PNG (and its brightness, its change from the frame before)
static void write_frame(void) {
    size_t n = (size_t)M.w * M.h * 3;
    uint8_t* rgb = malloc(n); // (an offline render: a frame's allocation is fine)
    if (!rgb) return;
    const float k = 1.0f / (float)M.job.samples;
    double sum = 0.0, dsum = 0.0;
    for (size_t i = 0; i < n; i++) {
        float v = M.acc[i] * k;
        rgb[i] = (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f);
        sum += rgb[i];
        if (M.prev) dsum += abs((int)rgb[i] - (int)M.prev[i]);
    }
    char path[900];
    snprintf(path, sizeof path, "%s/frame_%04u.png", M.job.out_dir, M.frame);
    if (vkctx_png_write_rgb(path, rgb, M.w, M.h)) M.st.written++;
    if (M.frame < 1024) M.lum[M.frame] = (float)(sum / (double)n), M.diff[M.frame] = M.prev ? (float)(dsum / (double)n) : 0.0f;
    free(M.prev);
    M.prev = rgb;
}

double movie_render_tick(Editor* ed, double now) {
    (void)now;
    if (M.phase == PH_IDLE) return 0.0;
    VkCtx* vk = ed->vk;
    const double step = 1.0 / ((double)M.job.fps * (double)M.job.samples);
    // the last frame's grab: one more sample of the frame (the samples' mean: motion blur, anti-aliasing)
    if (M.waiting && vk->grab_ready) {
        vk->grab_ready = false, M.waiting = false;
        if (!M.acc || M.w != vk->grab_w || M.h != vk->grab_h) {
            free(M.acc), free(M.prev), M.prev = NULL;
            M.w = vk->grab_w, M.h = vk->grab_h;
            M.acc = calloc((size_t)M.w * M.h * 3, sizeof(float));
            if (!M.acc) {
                finish(ed, false);
                return 0.0;
            }
        }
        size_t n = (size_t)M.w * M.h * 3;
        for (size_t i = 0; i < n; i++) M.acc[i] += vk->grab_px[i];
        M.st.samples_grabbed++;
        if (++M.sub == M.job.samples) {
            write_frame();
            memset(M.acc, 0, n * sizeof(float));
            M.sub = 0, M.frame++;
        }
    }
    if (M.phase == PH_WARMUP && ++M.warm > M.job.warmup) { // (the GI and the streaming settled: the sequence from its start)
        if (!seq_play(&ed->world, M.job.sequence, false, 1.0f)) {
            finish(ed, false);
            return 0.0;
        }
        M.phase = PH_RENDER;
    }
    if (M.phase == PH_RENDER && M.frame >= M.total) {
        finish(ed, true);
        return 0.0;
    }
    if (M.phase == PH_RENDER) { // this frame's picture: grabbed at its end (the viewport, in the window's pixels)
        float r[4] = { ed->vp_rect[0], ed->vp_rect[1], ed->vp_rect[2], ed->vp_rect[3] };
        const ImGuiViewport* mv = igGetMainViewport();
        if (mv) r[0] -= mv->Pos.x, r[1] -= mv->Pos.y; // (the multi-viewport's screen pixels into the window's)
        vkctx_request_grab(vk, r);
        M.waiting = true;
        return step;
    }
    return 1.0 / 60.0; // (the warm-up on the same fixed clock)
}

// --- Take Recorder ---
#define TAKE_SOURCES 16
static struct {
    bool on;
    Sequence* s;
    int fps;
    double t, next;
    uint32_t n;
    Entity e[TAKE_SOURCES];
    int track[TAKE_SOURCES];
    char last[300];
    uint32_t keys;
} T;

bool take_recording(void) { return T.on; }
const char* take_last(void) { return T.last; }

static void take_add(World* w, Entity e) {
    if (!ecs_alive(w, e) || T.n >= TAKE_SOURCES) return;
    for (uint32_t i = 0; i < T.n; i++)
        if (T.e[i] == e) return;
    const CName* nm = ecs_get(w, e, COMP_NAME);
    if (!nm || !nm->text[0] || !ecs_get(w, e, COMP_TRANSFORM)) return; // (a binding is by the actor's name)
    int tr = seq_add_track(T.s, SEQT_TRANSFORM, nm->text);
    if (tr < 0) return;
    T.e[T.n] = e, T.track[T.n++] = tr;
}

bool take_record_start(Editor* ed, int fps) {
    if (T.on || !ed->playing) return false;
    char dir[800];
    snprintf(dir, sizeof dir, "%s/Cinematics/Takes", content_root());
    re_make_dirs(dir); // (the takes' folder, made on the first)
    T.s = seq_create("/Content/Cinematics/Takes", "Take");
    if (!T.s) return false;
    T.fps = fps > 0 ? fps : 30, T.t = 0.0, T.next = 0.0, T.n = 0, T.keys = 0;
    T.s->fps = (float)T.fps;
    take_add(&ed->world, bp_player(&ed->world));
    for (uint32_t i = 0; i < ed->nsel; i++) take_add(&ed->world, ed->sel[i]);
    if (ed->selected) take_add(&ed->world, ed->selected);
    if (!T.n) {
        printf("[RealEngine] Take Recorder: nothing to record (no player pawn, no named actor selected)\n");
        return false;
    }
    T.on = true;
    printf("[RealEngine] Take Recorder: recording %u actors into %s\n", T.n, T.s->name);
    return true;
}

void take_record_tick(Editor* ed, double dt) {
    if (!T.on) return;
    if (!ed->playing) {
        take_record_stop(ed);
        return;
    }
    T.t += dt;
    while (T.next <= T.t) { // (a key at each of the take's frames: their times exact, whatever the frame rate)
        for (uint32_t i = 0; i < T.n; i++) {
            const CTransform* x = ecs_alive(&ed->world, T.e[i]) ? ecs_get(&ed->world, T.e[i], COMP_TRANSFORM) : NULL;
            if (!x) continue;
            SeqKey k;
            memset(&k, 0, sizeof k);
            k.t = (float)T.next;
            memcpy(k.v, x->location, 12), memcpy(k.v + 3, x->rotation, 12), memcpy(k.v + 6, x->scale, 12);
            if (seq_add_key(&T.s->tracks[T.track[i]], &k) >= 0) T.keys++;
        }
        T.next += 1.0 / (double)T.fps;
    }
}

bool take_record_stop(Editor* ed) {
    (void)ed;
    if (!T.on) return false;
    T.on = false;
    T.s->length = (float)T.t;
    bool ok = seq_save(T.s);
    snprintf(T.last, sizeof T.last, "%s", T.s->vpath);
    printf("[RealEngine] Take Recorder: %s, %.2f s, %u actors, %u keys%s\n", T.s->name, T.t, T.n, T.keys, ok ? "" : " (NOT SAVED)");
    return ok;
}

// --- Window > Cinematics ---
void cinematics_window(Editor* ed, bool* open) {
    if (!*open) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 30.0f, igGetFontSize() * 26.0f }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Cinematics"), open, 0)) {
        igEnd();
        return;
    }
    static MovieJob job = { "", "", 24, 4, 120, 0 }; // (2 s of warm-up: the exposure adapts, the GI settles)
    igSeparatorText(TR("Movie Render Queue"));
    {
        static char list[64][300];
        static uint32_t n;
        static double scanned;
        if (re_time_seconds() - scanned > 2.0) n = seq_list(list, 64), scanned = re_time_seconds();
        if (igBeginCombo(TR("Sequence"), job.sequence[0] ? job.sequence : "-", 0)) {
            for (uint32_t i = 0; i < n; i++)
                if (igSelectable_Bool(list[i], !strcmp(list[i], job.sequence), 0, (ImVec2){ 0, 0 })) snprintf(job.sequence, sizeof job.sequence, "%s", list[i]);
            igEndCombo();
        }
    }
    igSliderInt(TR("Frame Rate"), &job.fps, 12, 120, "%d fps", 0);
    igSliderInt(TR("Temporal Samples"), &job.samples, 1, 32, "%d", 0);
    igSetItemTooltip("%s", TR("Each frame is the mean of this many renders spread over its time: motion blur and smooth edges."));
    igSliderInt(TR("Warm Up Frames"), &job.warmup, 0, 240, "%d", 0);
    igInputTextWithHint(TR("Output Folder"), TR("<project>/Saved/MovieRenders/<sequence>"), job.out_dir, sizeof job.out_dir, 0, NULL, NULL);
    const MovieStats* st = movie_render_stats();
    if (st->active) {
        igText(TR("Rendering: %u of %u frames"), st->written, st->frames);
        if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) movie_render_cancel(ed);
    } else {
        igBeginDisabled(!job.sequence[0]);
        if (igButton(TR("Render (PNG)"), (ImVec2){ 0, 0 })) movie_render_start(ed, &job);
        igEndDisabled();
        igTextDisabled("%s", TR("The picture is the viewport's: make it the size you want the frames."));
    }
    if (st->status[0]) igTextWrapped("%s", st->status);
    igSeparatorText(TR("Take Recorder"));
    static int take_fps = 30;
    igSliderInt(TR("Take Frame Rate"), &take_fps, 10, 120, "%d fps", 0);
    if (!ed->playing) igTextDisabled("%s", TR("Play first (Alt+P): it records the player's pawn and the selected actors."));
    else if (!take_recording()) {
        if (igButton(TR("Record"), (ImVec2){ 0, 0 })) take_record_start(ed, take_fps);
    } else if (igButton(TR("Stop Recording"), (ImVec2){ 0, 0 }))
        take_record_stop(ed);
    if (take_last()[0]) igText(TR("Last take: %s"), take_last());
    igEnd();
}

// --- --cine-test: a cube falls in PIE and is recorded (a take); the take gets a camera cut and a fade from black; it is
// rendered at 24 fps with 4 temporal samples; the check: every frame written, the first dark (the fade), the cube moving ---
static struct { int phase; double t; Entity cube, cam; char out[700]; uint32_t take_keys; bool fail; } C;

static Entity named(World* w, uint64_t mask, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | mask);
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    return e;
}

bool cine_test_setup(Editor* ed, const char* out_dir) {
    memset(&C, 0, sizeof C);
    snprintf(C.out, sizeof C.out, "%s", out_dir && out_dir[0] ? out_dir : "movie_test");
    re_remove_tree(C.out);
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    Entity fl = named(w, COMP_BIT(COMP_STATIC_MESH), "Floor", 0, 0, 0);
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    CStaticMesh* fm = ecs_get(w, fl, COMP_STATIC_MESH);
    fm->mesh = SHAPE_PLANE, fm->base_color[0] = fm->base_color[1] = fm->base_color[2] = 0.6f;
    C.cube = named(w, COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_RIGID_BODY), "TakeCube", 0, 0, 450);
    CStaticMesh* cm = ecs_get(w, C.cube, COMP_STATIC_MESH);
    cm->mesh = SHAPE_CUBE, cm->base_color[0] = 0.9f, cm->base_color[1] = 0.35f, cm->base_color[2] = 0.1f;
    ((CTransform*)ecs_get(w, C.cube, COMP_TRANSFORM))->flags = MOBILITY_MOVABLE;
    ((CRigidBody*)ecs_get(w, C.cube, COMP_RIGID_BODY))->simulate = 1;
    C.cam = named(w, COMP_BIT(COMP_CAMERA), "MovieCam", -800, 0, 220);
    CTransform* ct = ecs_get(w, C.cam, COMP_TRANSFORM);
    ct->rotation[0] = -6.0f, ct->rotation[1] = 0.0f;
    Entity sun = named(w, COMP_BIT(COMP_LIGHT), "Sun", 0, 0, 800);
    CTransform* st = ecs_get(w, sun, COMP_TRANSFORM);
    st->rotation[0] = -45.0f, st->rotation[1] = 30.0f;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->type = LIGHT_DIRECTIONAL;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->intensity = 60000.0f;
    Entity sky = named(w, COMP_BIT(COMP_LIGHT), "Sky", 0, -300, 400);
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->type = LIGHT_SKY;
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->intensity = 1.0f; // (a Sky Light's intensity scales the sky: 1)
    ed->cam.pos[0] = -800.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 220.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = -6.0f;
    ed->dirty = true;
    editor_play(ed, re_time_seconds());
    editor_select(ed, C.cube, SEL_REPLACE);
    return ed->playing;
}

void cine_test_tick(Editor* ed, double dt) {
    C.t += dt;
    switch (C.phase) {
    case 0: // playing: the cube falls; recorded from 0.2 s for 1.3 s
        if (C.t >= 0.2 && !take_recording()) {
            editor_select(ed, C.cube, SEL_REPLACE);
            if (!take_record_start(ed, 30)) printf("[RealEngine] FAIL: cine-test: the Take Recorder didn't start\n"), C.fail = true, C.phase = 9;
        }
        if (C.t >= 1.5 && take_recording()) {
            take_record_stop(ed);
            C.phase = 1;
        }
        break;
    case 1: { // the take: a camera cut to MovieCam and a fade from black; the cube's physics off (the take moves it)
        editor_stop(ed, re_time_seconds());
        Sequence* s = seq_load(take_last());
        if (!s) {
            printf("[RealEngine] FAIL: cine-test: the take wasn't saved\n"), C.fail = true, C.phase = 9;
            break;
        }
        for (uint32_t i = 0; i < s->ntracks; i++) C.take_keys += s->tracks[i].nkeys;
        int cut = -1; // (a new sequence has its Camera Cuts track already)
        for (uint32_t i = 0; i < s->ntracks && cut < 0; i++)
            if (s->tracks[i].type == SEQT_CAMERA_CUTS) cut = (int)i;
        if (cut < 0) cut = seq_add_track(s, SEQT_CAMERA_CUTS, "");
        int fade = seq_add_track(s, SEQT_FADE, "");
        SeqKey k;
        memset(&k, 0, sizeof k);
        snprintf(k.name, sizeof k.name, "MovieCam");
        if (cut >= 0) seq_add_key(&s->tracks[cut], &k);
        memset(&k, 0, sizeof k), k.v[0] = 1.0f; // (black at the start)
        if (fade >= 0) seq_add_key(&s->tracks[fade], &k);
        k.t = 0.4f, k.v[0] = 0.0f;
        if (fade >= 0) seq_add_key(&s->tracks[fade], &k);
        seq_save(s);
        Entity cube = seq_find_actor(&ed->world, "TakeCube");
        CRigidBody* rb = cube ? ecs_get(&ed->world, cube, COMP_RIGID_BODY) : NULL;
        if (rb) rb->simulate = 0;
        MovieJob j;
        memset(&j, 0, sizeof j);
        snprintf(j.sequence, sizeof j.sequence, "%s", take_last());
        snprintf(j.out_dir, sizeof j.out_dir, "%s", C.out);
        j.fps = 24, j.samples = 4, j.warmup = 120;
        if (!movie_render_start(ed, &j)) printf("[RealEngine] FAIL: cine-test: the Movie Render Queue didn't start\n"), C.fail = true, C.phase = 9;
        else C.phase = 2;
        break;
    }
    case 2:
        if (movie_render_stats()->done) C.phase = 3;
        break;
    default: break;
    }
}

bool cine_test_check(Editor* ed) {
    (void)ed;
    const MovieStats* st = movie_render_stats();
    uint32_t last = st->written ? st->written - 1 : 0;
    float motion = 0.0f;
    for (uint32_t i = 6; i < st->written && i < 1024; i++) motion = fmaxf(motion, M.diff[i]);
    printf("[RealEngine] Cinematics: the take %u keys; the movie %u of %u frames (%u samples, %.1f s), brightness first %.1f last %.1f, motion %.2f\n",
           C.take_keys, st->written, st->frames, st->samples_grabbed, st->ms / 1000.0, st->written ? M.lum[0] : 0.0f, st->written ? M.lum[last] : 0.0f, motion);
    bool ok = !C.fail && C.phase == 3 && st->ok && st->frames >= 30 && C.take_keys >= 30;
    if (ok && !(M.lum[0] < M.lum[last] * 0.5f)) printf("[RealEngine] FAIL: cine-test: the fade from black\n"), ok = false;
    if (ok && motion < 0.05f) printf("[RealEngine] FAIL: cine-test: the cube didn't move in the frames\n"), ok = false;
    if (!ok && !C.fail) printf("[RealEngine] FAIL: cine-test: the take or the movie\n");
    return ok;
}
