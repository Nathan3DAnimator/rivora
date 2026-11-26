// The Sequencer window, laid out and used as Unreal's (sequencer.h, notas-dev/sequencer.md; 2026-11-10, "faz o
// sequencer de cinematics ser igual o da Unreal engine, tanto na aparência quanto na funcionalidade"):
//   - toolbar: Save, + Track (Actor To Sequencer: any actor of the level; the master tracks), Auto Key (the red dot:
//     moving a bound actor keys it at the playhead), Create Camera (a Camera Actor where the viewport looks, bound, its
//     cut), Lock to Camera Cuts, the display rate (fps), the snap to frames, seconds or frames shown;
//   - left, the tree: a search; the Camera Cuts; each bound actor as a group (its + Track, a right click rebinds or
//     removes it), its tracks inside (each with Unreal's previous key / add key / next key), the Transform opening into its
//     nine channels with their value at the playhead (editing it keys it there); the master tracks (Event, Audio, Fade);
//     an actor dragged from the Outliner binds it;
//   - right, the timeline: the ruler (frames or seconds; a click or drag moves the playhead), the playback range (its
//     green start and red end dragged on the ruler), each track's sections (a Transform's span, each animation clip, each
//     camera cut with its camera's name, each sound, the visible spans) and keys (their shape by interpolation: Auto a
//     circle, User a rounded square, Linear a diamond, Constant a square); keys selected by a click (Ctrl / Shift: add), a
//     box (drag on the empty), a group row's key (every key of the actor at that time); dragged along the frames
//     together; a right click sets their interpolation; Del deletes, Ctrl+C / Ctrl+V copy them to the playhead; a double
//     click keys a row there; the wheel scrolls, Ctrl+wheel zooms at the mouse, the middle button pans;
//   - the transport, under the tree: to the start, previous key, play backwards, play, next key, to the end, loop;
//     Space plays, Left / Right step a frame, Home / End jump;
//   - bottom: Details (the selected keys: time, values, interpolation; none: the sequence's range and rate) and the
//     Curve Editor (the selected track's channels as curves: keys dragged in time and value, User tangents' handles
//     dragged, F frames them);
//   - the preview: moving the playhead evaluates the sequence into the level (the actors' state before it is kept and
//     put back when the window closes or play starts: Unreal's Restore State).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "editor_docs.h"
#include "anim.h"
#include "theme.h"
#include "audio.h"
#include "audio_internal.h"
#include "components.h"
#include "editor.h"
#include "i18n.h"
#include "sequencer.h"
#include "terror.h"
#include "sequencer_ui.h"
#include "ui_icons.h"
#include "graph_style.h"

#define SAVED_MAX 64
#define SEL_MAX 1024
#define CLIP_MAX 256
#define ROWS_MAX 512
#define GROUPS_MAX 64

typedef struct Saved {
    Entity e;
    CTransform t;
    float light, fov;
} Saved;
typedef struct KeyRef { uint16_t track, key; } KeyRef;

enum { ROW_GROUP = 0, ROW_TRACK, ROW_CHANNEL };
typedef struct Row {
    uint8_t kind;
    int16_t track; // ROW_TRACK / ROW_CHANNEL: the track; ROW_GROUP: its first track
    int8_t ch;     // ROW_CHANNEL: the channel
    int8_t depth;
    char actor[SEQ_NAME]; // ROW_GROUP: the bound actor
} Row;

enum { DRAG_NONE = 0, DRAG_KEYS, DRAG_HEAD, DRAG_BOX, DRAG_START, DRAG_END, DRAG_PAN, DRAG_CV_KEY, DRAG_CV_TAN, DRAG_CV_PAN };

static struct {
    Sequence* s;
    float t, last_eval;
    bool playing, loop, lock_cam, unsaved, previewed, auto_key, show_frames, snap;
    int dir; // 1 forward, -1 backwards
    int sel_track;
    KeyRef sel[SEL_MAX];
    uint32_t nsel;
    float zoom, view0, scroll; // px per second, the seconds at the timeline's left, the rows scrolled (px)
    int drag;
    float drag_x0, drag_y0, drag_t0;
    float drag_orig[SEL_MAX];
    struct { uint32_t hash; bool closed; } groups[GROUPS_MAX];
    uint32_t ngroups;
    bool track_open[SEQ_MAX_TRACKS];
    char search[64];
    struct { uint16_t track; SeqKey k; } clip[CLIP_MAX];
    uint32_t nclip;
    float clip_t0;
    int tab; // 0 Details, 1 Curves
    bool cv_ch[9];
    float cv_t0, cv_t1, cv_v0, cv_v1;
    bool cv_fit;
    int cv_track, cv_key, cv_c, cv_side;
    CTransform written[SEQ_MAX_TRACKS]; // (Auto Key: what the preview put on each Transform track's actor)
    bool have_written[SEQ_MAX_TRACKS];
    Saved saved[SAVED_MAX];
    uint32_t nsaved;
    char status[160];
    uint32_t gen;
    Row rows[ROWS_MAX];
    uint32_t nrows;
} U = { .zoom = 120.0f, .sel_track = -1, .dir = 1, .snap = true, .show_frames = true, .cv_ch = { 1, 1, 1, 1, 1, 1, 1, 1, 1 }, .cv_fit = true };

typedef struct SeqSnap SeqSnap;
static void undo_clear(void);
static SeqSnap* snap_take(const Sequence* s);
#define UNDO_MAX 48
static struct {
    SeqSnap* undo[UNDO_MAX];
    SeqSnap* redo[UNDO_MAX];
    int nundo, nredo;
    SeqSnap* last;     // the state after the last change (what an undo of the next change goes back to)
    bool pressed_push; // (one press of the mouse, one step: a drag's frames are one change)
    bool focused;
} UD;

static const char* const g_ch_names[9] = { "Location X", "Location Y", "Location Z", "Rotation X (Pitch)", "Rotation Y (Yaw)",
                                           "Rotation Z (Roll)", "Scale X", "Scale Y", "Scale Z" };
static const uint32_t g_ch_cols[9] = { TK_RED, TK_GREEN, TK_BLUE, TK_RED, TK_GREEN, TK_BLUE, TK_RED, TK_GREEN, TK_BLUE };

// --- State before the preview (Unreal's Restore State) ---

static void save_state(World* w, Entity e) {
    if (!e) return;
    for (uint32_t i = 0; i < U.nsaved; i++)
        if (U.saved[i].e == e) return;
    if (U.nsaved >= SAVED_MAX) return;
    Saved* s = &U.saved[U.nsaved++];
    memset(s, 0, sizeof *s);
    s->e = e;
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) s->t = *t;
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    s->light = l ? l->intensity : 0.0f;
    const CCamera* c = ecs_get(w, e, COMP_CAMERA);
    s->fov = c ? c->fov_deg : 0.0f;
}

static void restore(World* w) {
    for (uint32_t i = 0; i < U.nsaved; i++) {
        Saved* s = &U.saved[i];
        if (!ecs_alive(w, s->e)) continue;
        CTransform* t = ecs_get(w, s->e, COMP_TRANSFORM);
        if (t) *t = s->t;
        CLight* l = ecs_get(w, s->e, COMP_LIGHT);
        if (l) l->intensity = s->light;
        CCamera* c = ecs_get(w, s->e, COMP_CAMERA);
        if (c) c->fov_deg = s->fov;
        ecs_touch(w, s->e);
    }
    U.nsaved = 0;
    U.previewed = false;
    memset(U.have_written, 0, sizeof U.have_written);
}

void seq_editor_restore(Editor* ed) {
    if (U.previewed) restore(&ed->world);
    terror_seq_clear(); // (Q13: the preview's Horror Filter tracks let go)
    U.playing = false;
}

void seq_editor_open(const char* vpath) {
    Sequence* s = seq_load(vpath);
    if (!s) return;
    U.gen = seq_generation();
    U.s = s, U.t = s->start, U.last_eval = -1.0f, U.playing = false, U.sel_track = -1, U.nsel = 0, U.unsaved = false;
    undo_clear();
    UD.last = snap_take(s);
    U.view0 = s->start - 14.0f / U.zoom, U.scroll = 0.0f, U.cv_fit = true, U.drag = DRAG_NONE; // (a margin: the keys at the start whole)
    memset(U.have_written, 0, sizeof U.have_written);
    U.status[0] = 0;
}

bool seq_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8);
    if (!ui_begin_menu(ICON_CAMERA, TR("Cinematics"), ok)) return false;
    bool made = false;
    if (ui_menu_item(ICON_CAMERA, TR("Level Sequence"), NULL, false, true)) {
        Sequence* s = seq_create(folder_vpath, "LS_New");
        if (s) seq_editor_open(s->vpath), made = true;
    }
    igEndMenu();
    return made;
}

// --- Helpers ---

static const char* actor_name(World* w, Entity e) {
    const CName* n = e ? ecs_get(w, e, COMP_NAME) : NULL;
    return n ? n->text : "";
}
static float snapt(float t) { return U.snap && U.s->fps > 0.0f ? roundf(t * U.s->fps) / U.s->fps : t; }
static bool master(const SeqTrack* trk) { return !trk->actor[0]; }
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static bool* group_closed(const char* actor) {
    uint32_t h = hash_str(actor);
    for (uint32_t i = 0; i < U.ngroups; i++)
        if (U.groups[i].hash == h) return &U.groups[i].closed;
    if (U.ngroups >= GROUPS_MAX) return NULL;
    U.groups[U.ngroups].hash = h, U.groups[U.ngroups].closed = false;
    return &U.groups[U.ngroups++].closed;
}
static bool key_selected(int trk, int k) {
    for (uint32_t i = 0; i < U.nsel; i++)
        if (U.sel[i].track == trk && U.sel[i].key == k) return true;
    return false;
}
static void select_key(int trk, int k, bool add) {
    if (!add) U.nsel = 0;
    if (key_selected(trk, k) || U.nsel >= SEL_MAX) return;
    U.sel[U.nsel].track = (uint16_t)trk, U.sel[U.nsel].key = (uint16_t)k, U.nsel++;
    U.sel_track = trk;
}
static void unselect_key(int trk, int k) {
    for (uint32_t i = 0; i < U.nsel; i++)
        if (U.sel[i].track == trk && U.sel[i].key == k) {
            U.sel[i] = U.sel[--U.nsel];
            return;
        }
}
// --- Undo (Ctrl+Z / Ctrl+Y in the window: the sequence's own, as Unreal's transactions) ---
struct SeqSnap {
    uint32_t ntracks;
    float length, start, fps;
    SeqTrack tracks[SEQ_MAX_TRACKS]; // (keys: copies of their own)
};
static void snap_free(SeqSnap* p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->ntracks; i++) free(p->tracks[i].keys);
    free(p);
}
static SeqSnap* snap_take(const Sequence* s) {
    SeqSnap* p = calloc(1, sizeof *p); // (an edit, not a frame)
    if (!p) return NULL;
    p->ntracks = s->ntracks, p->length = s->length, p->start = s->start, p->fps = s->fps;
    for (uint32_t i = 0; i < s->ntracks; i++) {
        p->tracks[i] = s->tracks[i];
        p->tracks[i].keys = s->tracks[i].nkeys ? malloc(sizeof(SeqKey) * s->tracks[i].nkeys) : NULL;
        if (p->tracks[i].keys) memcpy(p->tracks[i].keys, s->tracks[i].keys, sizeof(SeqKey) * s->tracks[i].nkeys);
        p->tracks[i].cap = s->tracks[i].nkeys;
    }
    return p;
}
static void snap_apply(Sequence* s, const SeqSnap* p) {
    for (uint32_t i = 0; i < s->ntracks; i++) free(s->tracks[i].keys);
    s->ntracks = p->ntracks, s->length = p->length, s->start = p->start, s->fps = p->fps;
    for (uint32_t i = 0; i < p->ntracks; i++) {
        s->tracks[i] = p->tracks[i];
        s->tracks[i].keys = p->tracks[i].nkeys ? malloc(sizeof(SeqKey) * p->tracks[i].nkeys) : NULL;
        if (s->tracks[i].keys) memcpy(s->tracks[i].keys, p->tracks[i].keys, sizeof(SeqKey) * p->tracks[i].nkeys);
        s->tracks[i].cap = p->tracks[i].nkeys;
    }
}
static void undo_clear(void) {
    for (int i = 0; i < UD.nundo; i++) snap_free(UD.undo[i]);
    for (int i = 0; i < UD.nredo; i++) snap_free(UD.redo[i]);
    snap_free(UD.last);
    UD.nundo = UD.nredo = 0, UD.last = NULL;
}
static void changed(void) {
    U.unsaved = true, U.last_eval = -1.0f;
    if (!U.s) return;
    if (!(igIsMouseDown_Nil(0) && UD.pressed_push) && UD.last) { // a new step (a drag's later frames join its first)
        if (UD.nundo == UNDO_MAX) snap_free(UD.undo[0]), memmove(UD.undo, UD.undo + 1, sizeof(UD.undo[0]) * (UNDO_MAX - 1)), UD.nundo--;
        UD.undo[UD.nundo++] = UD.last, UD.last = NULL;
        for (int i = 0; i < UD.nredo; i++) snap_free(UD.redo[i]);
        UD.nredo = 0;
        if (igIsMouseDown_Nil(0)) UD.pressed_push = true;
    }
    snap_free(UD.last);
    UD.last = snap_take(U.s);
}
static void undo_step(bool redo) {
    SeqSnap** from = redo ? UD.redo : UD.undo;
    int* nfrom = redo ? &UD.nredo : &UD.nundo;
    if (!*nfrom) return;
    SeqSnap* cur = snap_take(U.s);
    SeqSnap* to = from[--*nfrom];
    snap_apply(U.s, to);
    snap_free(to);
    if (cur) {
        SeqSnap** other = redo ? UD.undo : UD.redo;
        int* nother = redo ? &UD.nundo : &UD.nredo;
        if (*nother < UNDO_MAX) other[(*nother)++] = cur;
        else snap_free(cur);
    }
    snap_free(UD.last);
    UD.last = snap_take(U.s);
    U.nsel = 0, U.sel_track = -1, U.unsaved = true, U.last_eval = -1.0f;
    memset(U.have_written, 0, sizeof U.have_written);
    snprintf(U.status, sizeof U.status, "%s", redo ? TR("Redone.") : TR("Undone."));
}
bool seq_editor_focused(void) { return U.s && UD.focused; }

// The keys' times change: each track sorted again, the selection following its keys (by their old times)
static void resort(void) {
    for (uint32_t ti = 0; ti < U.s->ntracks; ti++) {
        SeqTrack* trk = &U.s->tracks[ti];
        if (trk->nkeys < 2) continue;
        // a mark on the selected keys (name's last byte would be lost: use a side table)
        static uint8_t mark[4096];
        uint32_t n = trk->nkeys < 4096 ? trk->nkeys : 4096;
        memset(mark, 0, n);
        for (uint32_t i = 0; i < U.nsel; i++)
            if (U.sel[i].track == ti && U.sel[i].key < n) mark[U.sel[i].key] = 1;
        // insertion sort carrying the marks (few keys per track)
        for (uint32_t i = 1; i < n; i++) {
            SeqKey k = trk->keys[i];
            uint8_t m = mark[i];
            uint32_t j = i;
            while (j > 0 && trk->keys[j - 1].t > k.t) trk->keys[j] = trk->keys[j - 1], mark[j] = mark[j - 1], j--;
            trk->keys[j] = k, mark[j] = m;
        }
        uint32_t keep = 0;
        for (uint32_t i = 0; i < U.nsel; i++)
            if (U.sel[i].track != ti) U.sel[keep++] = U.sel[i];
        U.nsel = keep;
        for (uint32_t k = 0; k < n && U.nsel < SEL_MAX; k++)
            if (mark[k]) U.sel[U.nsel].track = (uint16_t)ti, U.sel[U.nsel].key = (uint16_t)k, U.nsel++;
    }
}

// A key with the actor's state now (the track's kind)
static void capture(World* w, const SeqTrack* trk, SeqKey* k) {
    Entity e = seq_find_actor(w, trk->actor);
    switch (trk->type) {
    case SEQT_TRANSFORM: {
        const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t) memcpy(&k->v[0], t->location, 12), memcpy(&k->v[3], t->rotation, 12), memcpy(&k->v[6], t->scale, 12);
        else k->v[6] = k->v[7] = k->v[8] = 1.0f;
        break;
    }
    case SEQT_VISIBILITY: {
        const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        k->v[0] = t && (t->flags & TRANSFORM_HIDDEN_GAME) ? 0.0f : 1.0f;
        break;
    }
    case SEQT_LIGHT_INTENSITY: {
        const CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
        k->v[0] = l ? l->intensity : 1000.0f;
        break;
    }
    case SEQT_FOV: {
        const CCamera* c = e ? ecs_get(w, e, COMP_CAMERA) : NULL;
        k->v[0] = c ? c->fov_deg : 90.0f;
        break;
    }
    case SEQT_FADE: k->v[0] = trk->nkeys ? seq_channel_at(trk, 0, U.t) : 1.0f; break;
    case SEQT_HORROR_FILTER: // (Q13) the filter before it (the first: Hallucination), at full
        k->v[0] = trk->nkeys ? seq_channel_at(trk, 0, U.t) : 1.0f;
        if (!k->name[0]) snprintf(k->name, sizeof k->name, "%s", trk->nkeys ? trk->keys[0].name : terror_filter_name(TF_HALLUCINATION));
        break;
    default: break;
    }
}

static int add_key_at(Editor* ed, int ti, float t) {
    SeqTrack* trk = &U.s->tracks[ti];
    SeqKey k;
    memset(&k, 0, sizeof k);
    k.t = snapt(t);
    capture(&ed->world, trk, &k);
    if (trk->type == SEQT_CAMERA_CUTS) { // the selected camera, else the first camera of the level
        Entity c = ed->selected && ecs_get(&ed->world, ed->selected, COMP_CAMERA) ? ed->selected : ENTITY_NONE;
        for (uint32_t ai = 0; !c && ai < ed->world.archetype_count; ai++) {
            const EcsArchetype* a = &ed->world.archetypes[ai];
            if ((a->mask & COMP_BIT(COMP_CAMERA)) && (a->mask & COMP_BIT(COMP_NAME)) && a->count) c = ecs_arch_entities(a)[0];
        }
        snprintf(k.name, sizeof k.name, "%s", actor_name(&ed->world, c));
    }
    if (trk->type == SEQT_EVENT) snprintf(k.name, sizeof k.name, "SequenceEvent");
    if ((trk->type == SEQT_ANIMATION || trk->type == SEQT_AUDIO) && trk->nkeys) {
        int before = 0;
        for (uint32_t i = 0; i < trk->nkeys; i++)
            if (trk->keys[i].t <= k.t) before = (int)i;
        snprintf(k.name, sizeof k.name, "%s", trk->keys[before].name);
        if (trk->type == SEQT_AUDIO) k.v[0] = trk->keys[before].v[0], k.v[1] = trk->keys[before].v[1];
    }
    if (trk->type == SEQT_AUDIO && k.v[0] <= 0.0f) k.v[0] = k.v[1] = 1.0f;
    for (uint32_t i = 0; i < trk->nkeys; i++) // (a key already there keeps its interpolation)
        if (fabsf(trk->keys[i].t - k.t) < 1e-4f) k.interp = trk->keys[i].interp, memcpy(k.tan, trk->keys[i].tan, sizeof k.tan);
    int at = seq_add_key(trk, &k);
    if (at >= 0) select_key(ti, at, false);
    changed();
    return at;
}

static int add_track(Editor* ed, SeqTrackType type, const char* actor) {
    int ti = seq_add_track(U.s, type, actor ? actor : "");
    if (ti < 0) {
        snprintf(U.status, sizeof U.status, "%s", TR("The sequence has as many tracks as it can hold."));
        return -1;
    }
    bool keyed = type == SEQT_TRANSFORM || type == SEQT_VISIBILITY || type == SEQT_LIGHT_INTENSITY || type == SEQT_FOV || type == SEQT_FADE || type == SEQT_HORROR_FILTER;
    if (keyed) add_key_at(ed, ti, U.t); // (its first key: as it is)
    U.sel_track = ti;
    changed();
    return ti;
}

static int find_track(const char* actor, int type) {
    for (uint32_t i = 0; i < U.s->ntracks; i++)
        if (U.s->tracks[i].type == type && !strcmp(U.s->tracks[i].actor, actor)) return (int)i;
    return -1;
}

// Actor To Sequencer: bound with its Transform (and what it has: a camera's FOV, a light's intensity)
static void bind_actor(Editor* ed, Entity e) {
    const char* n = actor_name(&ed->world, e);
    if (!n[0]) {
        snprintf(U.status, sizeof U.status, "%s", TR("The actor needs a name to be bound."));
        return;
    }
    if (find_track(n, SEQT_TRANSFORM) < 0) add_track(ed, SEQT_TRANSFORM, n);
    snprintf(U.status, sizeof U.status, TR("%s added to the sequence."), n);
}

// --- The preview ---

static void preview(Editor* ed) {
    if (ed->playing || !U.s) return;
    if (!U.previewed) { // the state before: kept once
        for (uint32_t i = 0; i < U.s->ntracks; i++) save_state(&ed->world, seq_find_actor(&ed->world, U.s->tracks[i].actor));
        U.previewed = true;
    }
    seq_evaluate(&ed->world, U.s, U.last_eval, U.t, false);
    U.last_eval = U.t;
    for (uint32_t i = 0; i < U.s->ntracks; i++) { // (Auto Key: what the preview put there; a difference later is the user's)
        const SeqTrack* trk = &U.s->tracks[i];
        Entity e = trk->type == SEQT_TRANSFORM && trk->nkeys ? seq_find_actor(&ed->world, trk->actor) : ENTITY_NONE;
        const CTransform* x = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
        U.have_written[i] = x != NULL;
        if (x) U.written[i] = *x;
    }
    if (U.lock_cam) { // the viewport looks through the Camera Cuts' camera
        Entity c = seq_camera_at(&ed->world, U.s, U.t);
        const CTransform* x = c ? ecs_get(&ed->world, c, COMP_TRANSFORM) : NULL;
        if (x) memcpy(ed->cam.pos, x->location, 12), ed->cam.pitch = x->rotation[0], ed->cam.yaw = x->rotation[1];
    }
}
static void set_time(Editor* ed, float t) {
    float lo = 0.0f, hi = U.s->length;
    U.t = snapt(t < lo ? lo : t > hi ? hi : t);
    preview(ed);
}

// Auto Key (Unreal's red dot): a bound actor moved in the viewport (its transform no longer the preview's) is keyed at
// the playhead
static void auto_key(Editor* ed) {
    if (!U.auto_key || U.playing || ed->playing) return;
    for (uint32_t i = 0; i < U.s->ntracks; i++) {
        SeqTrack* trk = &U.s->tracks[i];
        if (trk->type != SEQT_TRANSFORM || !U.have_written[i]) continue;
        Entity e = seq_find_actor(&ed->world, trk->actor);
        const CTransform* x = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
        if (!x) continue;
        float d = 0.0f;
        for (int c = 0; c < 3; c++)
            d = fmaxf(d, fmaxf(fabsf(x->location[c] - U.written[i].location[c]), fmaxf(fabsf(x->rotation[c] - U.written[i].rotation[c]), fabsf(x->scale[c] - U.written[i].scale[c]) * 100.0f)));
        if (d < 1e-3f) continue;
        add_key_at(ed, (int)i, U.t);
        U.written[i] = *x;
        snprintf(U.status, sizeof U.status, TR("Auto Key: %s keyed at frame %d."), trk->actor, (int)roundf(U.t * U.s->fps));
    }
}

// --- Copy / paste / delete ---

static void copy_keys(void) {
    U.nclip = 0, U.clip_t0 = 1e30f;
    for (uint32_t i = 0; i < U.nsel && U.nclip < CLIP_MAX; i++) {
        const SeqTrack* trk = &U.s->tracks[U.sel[i].track];
        if (U.sel[i].key >= trk->nkeys) continue;
        U.clip[U.nclip].track = U.sel[i].track, U.clip[U.nclip].k = trk->keys[U.sel[i].key];
        U.clip_t0 = fminf(U.clip_t0, trk->keys[U.sel[i].key].t);
        U.nclip++;
    }
    snprintf(U.status, sizeof U.status, TR("%u keys copied."), U.nclip);
}
static void paste_keys(void) {
    if (!U.nclip) return;
    U.nsel = 0;
    for (uint32_t i = 0; i < U.nclip; i++) {
        if (U.clip[i].track >= U.s->ntracks) continue;
        SeqKey k = U.clip[i].k;
        k.t = snapt(U.t + (k.t - U.clip_t0));
        int at = seq_add_key(&U.s->tracks[U.clip[i].track], &k);
        if (at >= 0) select_key(U.clip[i].track, at, true);
    }
    resort();
    changed();
}
static void delete_selected(void) {
    // by track, highest key first (the indices below stay)
    for (uint32_t ti = 0; ti < U.s->ntracks; ti++)
        for (int k = (int)U.s->tracks[ti].nkeys - 1; k >= 0; k--)
            if (key_selected((int)ti, k)) seq_remove_key(&U.s->tracks[ti], k);
    U.nsel = 0;
    changed();
}
static void set_interp_selected(int m) {
    for (uint32_t i = 0; i < U.nsel; i++) {
        SeqTrack* trk = &U.s->tracks[U.sel[i].track];
        if (U.sel[i].key >= trk->nkeys) continue;
        SeqKey* k = &trk->keys[U.sel[i].key];
        if (m == SEQI_USER && k->interp != SEQI_USER) // (User from what it was: its Auto slopes, nothing jumps)
            for (int c = 0; c < seq_track_channels(trk->type); c++) {
                float e = 1e-3f, a = seq_channel_at(trk, c, k->t + e), b = seq_channel_at(trk, c, k->t - e);
                k->tan[c] = (a - b) / (2.0f * e);
            }
        k->interp = (uint8_t)m;
    }
    changed();
}
static void interp_menu(void) {
    igSeparatorText(TR("Interpolation"));
    for (int m = 0; m < SEQI_COUNT; m++)
        if (ui_menu_item(ICON_CURVE, TR(seq_interp_name(m)), NULL, false, U.nsel > 0)) set_interp_selected(m);
    igSeparator();
    if (ui_menu_item(ICON_COPY, TR("Copy"), "Ctrl+C", false, U.nsel > 0)) copy_keys();
    if (ui_menu_item(ICON_PASTE, TR("Paste"), "Ctrl+V", false, U.nclip > 0)) paste_keys();
    if (ui_menu_item(ICON_DELETE, TR("Delete"), "Del", false, U.nsel > 0)) delete_selected();
}

// Previous / next key of a track (or of every track: -1) from the playhead
static float neighbor_key(int ti, int dir) {
    float best = dir > 0 ? 1e30f : -1e30f;
    for (uint32_t i = 0; i < U.s->ntracks; i++) {
        if (ti >= 0 && (int)i != ti) continue;
        const SeqTrack* trk = &U.s->tracks[i];
        for (uint32_t k = 0; k < trk->nkeys; k++) {
            float t = trk->keys[k].t;
            if (dir > 0 && t > U.t + 1e-4f && t < best) best = t;
            if (dir < 0 && t < U.t - 1e-4f && t > best) best = t;
        }
    }
    return dir > 0 ? (best < 1e29f ? best : U.t) : (best > -1e29f ? best : U.t);
}

// --- Drawing helpers ---

static void key_shape(ImDrawList* dl, float x, float y, float r, int interp, uint32_t col, bool sel) {
    bool orbit = graph_orbit(); // (Q5: the Orbit look: a selected key in the accent with its halo)
    uint32_t fill = sel ? (orbit ? TH(TK_ACCENT) : TH(TK_AMBER)) : col, edge = sel ? TH(TK_N15) : TH(TK_N2);
    if (sel && orbit) ImDrawList_AddCircleFilled(dl, (ImVec2){ x, y }, r * 1.9f, THA(TK_ACCENT, 60), 16);
    switch (interp) {
    case SEQI_LINEAR:
        ImDrawList_AddQuadFilled(dl, (ImVec2){ x, y - r }, (ImVec2){ x + r, y }, (ImVec2){ x, y + r }, (ImVec2){ x - r, y }, fill);
        ImDrawList_AddQuad(dl, (ImVec2){ x, y - r }, (ImVec2){ x + r, y }, (ImVec2){ x, y + r }, (ImVec2){ x - r, y }, edge, 1.0f);
        break;
    case SEQI_CONSTANT:
        ImDrawList_AddRectFilled(dl, (ImVec2){ x - r * 0.8f, y - r * 0.8f }, (ImVec2){ x + r * 0.8f, y + r * 0.8f }, fill, 0.0f, 0);
        ImDrawList_AddRect(dl, (ImVec2){ x - r * 0.8f, y - r * 0.8f }, (ImVec2){ x + r * 0.8f, y + r * 0.8f }, edge, 0.0f, 1.0f, 0);
        break;
    case SEQI_USER:
        ImDrawList_AddRectFilled(dl, (ImVec2){ x - r * 0.85f, y - r * 0.85f }, (ImVec2){ x + r * 0.85f, y + r * 0.85f }, fill, r * 0.45f, 0);
        ImDrawList_AddRect(dl, (ImVec2){ x - r * 0.85f, y - r * 0.85f }, (ImVec2){ x + r * 0.85f, y + r * 0.85f }, edge, r * 0.45f, 1.0f, 0);
        break;
    default:
        ImDrawList_AddCircleFilled(dl, (ImVec2){ x, y }, r * 0.85f, fill, 12);
        ImDrawList_AddCircle(dl, (ImVec2){ x, y }, r * 0.85f, edge, 12, 1.0f);
        break;
    }
}

static uint32_t section_color(int type) {
    switch (type) {
    case SEQT_TRANSFORM: return TH(TK_BLUE_D);      // blue-grey
    case SEQT_VISIBILITY: return TH(TK_GREEN_D);
    case SEQT_LIGHT_INTENSITY: return TH(TK_AMBER); // amber
    case SEQT_FOV: return TH(TK_PINK);
    case SEQT_ANIMATION: return TH(TK_LIME);       // green
    case SEQT_CAMERA_CUTS: return TH(TK_MAGENTA);     // purple
    case SEQT_EVENT: return TH(TK_ORANGE);           // orange
    case SEQT_AUDIO: return TH(TK_BLUE);           // teal
    case SEQT_FADE: return TH(TK_N10);
    case SEQT_HORROR_FILTER: return TH(TK_RED); // (Q13)
    default: return TH(TK_N10);
    }
}
static UiIcon track_icon(int type) {
    switch (type) {
    case SEQT_TRANSFORM: return ICON_MOVE;
    case SEQT_VISIBILITY: return ICON_EYE;
    case SEQT_LIGHT_INTENSITY: return ICON_BULB;
    case SEQT_FOV: case SEQT_CAMERA_CUTS: return ICON_CAMERA;
    case SEQT_ANIMATION: return ICON_CHARACTER;
    case SEQT_EVENT: return ICON_EVENT;
    case SEQT_AUDIO: return ICON_SOUND;
    case SEQT_FADE: return ICON_EXPOSURE;
    case SEQT_HORROR_FILTER: return ICON_FILTER; // (Q13)
    default: return ICON_FILE;
    }
}

// A transport button drawn as Unreal's: 0 to start, 1 previous key, 2 play backwards, 3 play, 4 pause, 5 next key,
// 6 to end, 7 loop
static bool transport(const char* id, int kind, bool active, const char* tip) {
    float s = igGetFrameHeight();
    ImVec2 p = igGetCursorScreenPos();
    bool clicked = igInvisibleButton(id, (ImVec2){ s * 1.2f, s }, 0);
    bool hov = igIsItemHovered(0);
    if (tip) igSetItemTooltip("%s", tip);
    ImDrawList* dl = igGetWindowDrawList();
    if (hov || active) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + s * 1.2f, p.y + s }, active ? TH(TK_BLUE_D) : TH(TK_N8), 3.0f, 0);
    uint32_t c = active ? TH(TK_BLUE_L) : TH(TK_N14);
    float cx = p.x + s * 0.6f, cy = p.y + s * 0.5f, r = s * 0.26f;
    switch (kind) {
    case 0: case 6: { // |<  >|
        float dx = kind == 0 ? -1.0f : 1.0f;
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx + dx * r - 1.5f, cy - r }, (ImVec2){ cx + dx * r + 1.5f, cy + r }, c, 0, 0);
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx + dx * r, cy }, (ImVec2){ cx - dx * r * 0.6f, cy - r }, (ImVec2){ cx - dx * r * 0.6f, cy + r }, c);
        break;
    }
    case 1: case 5: { // |< key >|: a diamond and an arrow
        float dx = kind == 1 ? -1.0f : 1.0f;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx + dx * r, cy }, (ImVec2){ cx, cy - r }, (ImVec2){ cx, cy + r }, c);
        ImDrawList_AddQuadFilled(dl, (ImVec2){ cx - dx * r * 0.55f, cy - r * 0.45f }, (ImVec2){ cx - dx * r * 0.1f, cy }, (ImVec2){ cx - dx * r * 0.55f, cy + r * 0.45f },
                                 (ImVec2){ cx - dx * r, cy }, c);
        break;
    }
    case 2: ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - r, cy }, (ImVec2){ cx + r * 0.7f, cy - r }, (ImVec2){ cx + r * 0.7f, cy + r }, c); break;
    case 3: ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx + r, cy }, (ImVec2){ cx - r * 0.7f, cy - r }, (ImVec2){ cx - r * 0.7f, cy + r }, c); break;
    case 4:
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx - r * 0.75f, cy - r }, (ImVec2){ cx - r * 0.2f, cy + r }, c, 0, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx + r * 0.2f, cy - r }, (ImVec2){ cx + r * 0.75f, cy + r }, c, 0, 0);
        break;
    case 7: // loop: a circle with an arrow head
        ImDrawList_PathArcTo(dl, (ImVec2){ cx, cy }, r, 0.6f, 5.9f, 16);
        ImDrawList_PathStroke(dl, c, 1.8f, 0);
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx + r * 0.85f, cy - r * 0.55f }, (ImVec2){ cx + r * 1.45f, cy - r * 0.05f }, (ImVec2){ cx + r * 0.45f, cy + r * 0.25f }, c);
        break;
    }
    return clicked;
}

static void fmt_time(char* out, size_t cap, float t) {
    if (U.show_frames) snprintf(out, cap, "%04d", (int)roundf(t * U.s->fps));
    else snprintf(out, cap, "%.2f s", t);
}

// --- The tree's rows ---

static bool track_matches(const SeqTrack* trk) {
    if (!U.search[0]) return true;
    const char* hay[2] = { trk->actor, seq_track_type_name(trk->type) };
    for (int i = 0; i < 2; i++)
        for (const char* h = hay[i]; *h; h++) {
            size_t k = 0;
            while (U.search[k] && h[k] && ((h[k] | 32) == (U.search[k] | 32))) k++;
            if (!U.search[k]) return true;
        }
    return false;
}
static void push_row(uint8_t kind, int track, int ch, int depth, const char* actor) {
    if (U.nrows >= ROWS_MAX) return;
    Row* r = &U.rows[U.nrows++];
    r->kind = kind, r->track = (int16_t)track, r->ch = (int8_t)ch, r->depth = (int8_t)depth;
    snprintf(r->actor, sizeof r->actor, "%s", actor ? actor : "");
}
static void push_track(int ti, int depth) {
    const SeqTrack* trk = &U.s->tracks[ti];
    push_row(ROW_TRACK, ti, -1, depth, trk->actor);
    if (trk->type == SEQT_TRANSFORM && U.track_open[ti])
        for (int c = 0; c < 9; c++) push_row(ROW_CHANNEL, ti, c, depth + 1, trk->actor);
}
// Camera Cuts first, then each bound actor as a group with its tracks, then the other master tracks (Unreal's order)
static void build_rows(void) {
    U.nrows = 0;
    for (uint32_t i = 0; i < U.s->ntracks; i++)
        if (U.s->tracks[i].type == SEQT_CAMERA_CUTS && track_matches(&U.s->tracks[i])) push_track((int)i, 0);
    for (uint32_t i = 0; i < U.s->ntracks; i++) {
        const SeqTrack* trk = &U.s->tracks[i];
        if (master(trk)) continue;
        bool first = true;
        for (uint32_t j = 0; j < i && first; j++)
            if (!strcmp(U.s->tracks[j].actor, trk->actor)) first = false;
        if (!first) continue;
        bool any = false;
        for (uint32_t j = i; j < U.s->ntracks && !any; j++) any = !strcmp(U.s->tracks[j].actor, trk->actor) && track_matches(&U.s->tracks[j]);
        if (!any) continue;
        push_row(ROW_GROUP, (int)i, -1, 0, trk->actor);
        bool* closed = group_closed(trk->actor);
        if (closed && *closed) continue;
        for (uint32_t j = i; j < U.s->ntracks; j++)
            if (!strcmp(U.s->tracks[j].actor, trk->actor) && track_matches(&U.s->tracks[j])) push_track((int)j, 1);
    }
    for (uint32_t i = 0; i < U.s->ntracks; i++)
        if (master(&U.s->tracks[i]) && U.s->tracks[i].type != SEQT_CAMERA_CUTS && track_matches(&U.s->tracks[i])) push_track((int)i, 0);
}

// The tracks an actor can have (+ Track on its group)
static void actor_track_menu(Editor* ed, const char* actor) {
    Entity e = seq_find_actor(&ed->world, actor);
    static const SeqTrackType bound[6] = { SEQT_TRANSFORM, SEQT_VISIBILITY, SEQT_LIGHT_INTENSITY, SEQT_FOV, SEQT_ANIMATION, SEQT_AUDIO };
    for (int i = 0; i < 6; i++) {
        bool can = find_track(actor, bound[i]) < 0 && (bound[i] != SEQT_LIGHT_INTENSITY || (e && ecs_get(&ed->world, e, COMP_LIGHT))) &&
                   (bound[i] != SEQT_FOV || (e && ecs_get(&ed->world, e, COMP_CAMERA))) &&
                   (bound[i] != SEQT_ANIMATION || (e && ecs_get(&ed->world, e, COMP_SKELETAL_MESH)));
        if (ui_menu_item(track_icon(bound[i]), TR(seq_track_type_name(bound[i])), NULL, false, can)) add_track(ed, bound[i], actor);
    }
    if (ui_menu_item(ICON_EVENT, TR("Event"), NULL, false, true)) add_track(ed, SEQT_EVENT, actor);
}

// One row of the tree (left): its label, expander, buttons; true when it asked for its track's removal
static void tree_row(Editor* ed, const Row* r, float x, float y, float w, float h, int ri, int* remove_track, char* remove_group) {
    ImDrawList* dl = igGetWindowDrawList();
    World* world = &ed->world;
    const SeqTrack* trk = &U.s->tracks[r->track];
    bool sel = r->kind != ROW_GROUP && r->track == U.sel_track;
    uint32_t bg = r->kind == ROW_GROUP ? TH(TK_N7) : sel ? TH(TK_BLUE_D) : (ri & 1) ? TH(TK_N5) : TH(TK_N4);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h - 1 }, bg, 0, 0);
    float fs = igGetFontSize(), ix = x + 6 + r->depth * fs * 1.1f, cy = y + h * 0.5f;
    igPushID_Int(ri);
    // the expander
    bool expandable = r->kind == ROW_GROUP || (r->kind == ROW_TRACK && trk->type == SEQT_TRANSFORM);
    if (expandable) {
        bool* closed = r->kind == ROW_GROUP ? group_closed(r->actor) : NULL;
        bool open = r->kind == ROW_GROUP ? !(closed && *closed) : U.track_open[r->track];
        igSetCursorScreenPos((ImVec2){ ix - 2, y });
        if (igInvisibleButton("##exp", (ImVec2){ fs, h - 1 }, 0)) {
            if (r->kind == ROW_GROUP && closed) *closed = !*closed;
            else U.track_open[r->track] = !U.track_open[r->track];
        }
        float a = fs * 0.28f;
        if (open) ImDrawList_AddTriangleFilled(dl, (ImVec2){ ix + fs * 0.5f - a, cy - a * 0.6f }, (ImVec2){ ix + fs * 0.5f + a, cy - a * 0.6f }, (ImVec2){ ix + fs * 0.5f, cy + a * 0.7f }, TH(TK_N13));
        else ImDrawList_AddTriangleFilled(dl, (ImVec2){ ix + fs * 0.5f - a * 0.6f, cy - a }, (ImVec2){ ix + fs * 0.5f - a * 0.6f, cy + a }, (ImVec2){ ix + fs * 0.5f + a * 0.7f, cy }, TH(TK_N13));
    }
    ix += fs * 1.1f;
    // the label
    char lab[160];
    uint32_t tc = TH(TK_N14);
    UiIcon icon;
    if (r->kind == ROW_GROUP) {
        bool missing = !seq_find_actor(world, r->actor);
        snprintf(lab, sizeof lab, "%s", r->actor);
        tc = missing ? TH(TK_RED) : TH(TK_N15);
        Entity e = seq_find_actor(world, r->actor);
        icon = e && ecs_get(world, e, COMP_CAMERA) ? ICON_CAMERA : e && ecs_get(world, e, COMP_LIGHT) ? ICON_BULB : e && ecs_get(world, e, COMP_SKELETAL_MESH) ? ICON_CHARACTER : ICON_ACTOR;
    } else if (r->kind == ROW_CHANNEL) {
        snprintf(lab, sizeof lab, "%s", TR(g_ch_names[r->ch]));
        tc = TH(TK_N13);
        icon = ICON_COUNT;
    } else {
        snprintf(lab, sizeof lab, "%s", TR(seq_track_type_name(trk->type)));
        icon = track_icon(trk->type);
    }
    if (icon != ICON_COUNT) ui_icon(dl, icon, ix, cy - fs * 0.5f, fs, r->kind == ROW_GROUP ? TH(TK_BLUE_L) : section_color(trk->type) | 0xFF404040u), ix += fs * 1.3f;
    else ImDrawList_AddCircleFilled(dl, (ImVec2){ ix + fs * 0.3f, cy }, fs * 0.22f, TH(g_ch_cols[r->ch]), 8), ix += fs * 0.9f;
    ImDrawList_PushClipRect(dl, (ImVec2){ x, y }, (ImVec2){ x + w - fs * 6.0f, y + h }, true);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ ix, cy - fs * 0.5f }, tc, lab, NULL);
    ImDrawList_PopClipRect(dl);
    // the row itself: a click selects the track, a right click its menu
    igSetCursorScreenPos((ImVec2){ ix, y });
    float bw = w - (ix - x) - fs * 6.0f;
    if (igInvisibleButton("##row", (ImVec2){ bw > 4 ? bw : 4, h - 1 }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight)) {
        if (r->kind != ROW_GROUP) U.sel_track = r->track;
        else { // the group: its actor selected in the level (Unreal selects the bound actor)
            Entity e = seq_find_actor(world, r->actor);
            if (e) editor_select(ed, e, SEL_REPLACE);
        }
    }
    if (r->kind == ROW_GROUP && !seq_find_actor(world, r->actor)) igSetItemTooltip("%s", TR("No actor of that name in this level (right click: bind it to the selected actor)."));
    if (igBeginPopupContextItem("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
        if (r->kind == ROW_GROUP) {
            igSeparatorText(r->actor);
            const char* selname = actor_name(world, ed->selected);
            char rb[96];
            snprintf(rb, sizeof rb, "%s %s", TR("Rebind to"), selname[0] ? selname : TR("(select an actor)"));
            if (ui_menu_item(ICON_LINK, rb, NULL, false, selname[0] != 0)) { // every track of the group follows
                char old[SEQ_NAME];
                snprintf(old, sizeof old, "%s", r->actor);
                for (uint32_t i = 0; i < U.s->ntracks; i++)
                    if (!strcmp(U.s->tracks[i].actor, old)) snprintf(U.s->tracks[i].actor, sizeof U.s->tracks[i].actor, "%s", selname);
                changed();
            }
            if (ui_menu_item(ICON_CROSS, TR("Remove from the Sequence"), NULL, false, true)) snprintf(remove_group, SEQ_NAME, "%s", r->actor);
        } else {
            if (r->kind == ROW_TRACK && trk->type == SEQT_ANIMATION) {
                bool lp = trk->loop_anim;
                if (ui_menu_item(ICON_REFRESH, TR("Loop the Clip"), NULL, lp, true)) U.s->tracks[r->track].loop_anim = !lp, changed();
            }
            if (ui_menu_item(ICON_DELETE, TR("Delete Track"), NULL, false, true)) *remove_track = r->track;
        }
        igEndPopup();
    }
    // the right side: a group's + Track; a track's previous / add / next key; a channel's value at the playhead
    float bx = x + w - fs * 5.8f;
    if (r->kind == ROW_GROUP) {
        igSetCursorScreenPos((ImVec2){ bx + fs * 0.6f, y + (h - igGetFrameHeight()) * 0.5f });
        igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 4, 1 });
        if (igSmallButton(TR("+ Track"))) igOpenPopup_Str("##grp_add", 0);
        igPopStyleVar(1);
        if (igBeginPopup("##grp_add", 0)) {
            actor_track_menu(ed, r->actor);
            igEndPopup();
        }
    } else if (r->kind == ROW_TRACK) {
        float s = fs * 1.4f;
        for (int b = 0; b < 3; b++) {
            igSetCursorScreenPos((ImVec2){ bx + b * (s + 2), y + (h - s) * 0.5f });
            igPushID_Int(b);
            bool click = igInvisibleButton("##kb", (ImVec2){ s, s }, 0);
            bool hov = igIsItemHovered(0);
            igSetItemTooltip("%s", b == 0 ? TR("Previous key") : b == 1 ? TR("Add a key at the playhead (S)") : TR("Next key"));
            float kx = bx + b * (s + 2) + s * 0.5f, ky = y + h * 0.5f, rr = s * 0.28f;
            uint32_t c = hov ? TH(TK_N15) : TH(TK_N12);
            if (b == 1) key_shape(dl, kx, ky, rr * 1.2f, SEQI_LINEAR, c, false);
            else {
                float d = b == 0 ? -1.0f : 1.0f;
                ImDrawList_AddTriangleFilled(dl, (ImVec2){ kx + d * rr, ky }, (ImVec2){ kx - d * rr * 0.6f, ky - rr }, (ImVec2){ kx - d * rr * 0.6f, ky + rr }, c);
            }
            if (click) {
                U.sel_track = r->track;
                if (b == 1) add_key_at(ed, r->track, U.t);
                else set_time(ed, neighbor_key(r->track, b == 0 ? -1 : 1)), U.playing = false;
            }
            igPopID();
        }
    } else { // a channel: its value now (typed or dragged: a key there with it)
        float v = seq_channel_at(trk, r->ch, U.t);
        igSetCursorScreenPos((ImVec2){ bx, y + (h - igGetFrameHeight()) * 0.5f });
        igSetNextItemWidth(fs * 5.6f);
        igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2){ 3, 1 });
        if (igDragFloat("##chv", &v, r->ch >= 6 ? 0.01f : 0.5f, 0, 0, r->ch >= 6 ? "%.3f" : "%.1f", 0)) {
            int at = -1;
            for (uint32_t k = 0; k < trk->nkeys && at < 0; k++)
                if (fabsf(trk->keys[k].t - U.t) < 1e-4f) at = (int)k;
            if (at < 0) {
                SeqKey nk;
                memset(&nk, 0, sizeof nk);
                nk.t = snapt(U.t);
                for (int c = 0; c < 9; c++) nk.v[c] = seq_channel_at(trk, c, U.t);
                at = seq_add_key(&U.s->tracks[r->track], &nk);
            }
            if (at >= 0) U.s->tracks[r->track].keys[at].v[r->ch] = v, changed(), preview(ed);
        }
        igPopStyleVar(1);
    }
    igPopID();
}

// --- The timeline (right) ---

static float t2x(float t, float x0) { return x0 + (t - U.view0) * U.zoom; }
static float x2t(float x, float x0) { return U.view0 + (x - x0) / U.zoom; }

// A track's sections and keys on its row; hit test of the keys under the mouse (returned in *hit_key)
static void track_lane(ImDrawList* dl, const Row* r, float x0, float x1, float y, float h, int* hit_track, int* hit_key, bool* hit_group, float* hit_t) {
    const SeqTrack* trk = &U.s->tracks[r->track];
    float cy = y + h * 0.5f, kr = h * 0.27f, fs = igGetFontSize();
    ImVec2 m = igGetIO_Nil()->MousePos;
    bool in_row = m.y >= y && m.y < y + h && m.x >= x0 && m.x < x1;
    if (r->kind == ROW_GROUP) { // every key of the actor's tracks, small (a click selects them all at that time)
        for (uint32_t ti = 0; ti < U.s->ntracks; ti++) {
            const SeqTrack* t2 = &U.s->tracks[ti];
            if (strcmp(t2->actor, r->actor)) continue;
            for (uint32_t k = 0; k < t2->nkeys; k++) {
                float kx = t2x(t2->keys[k].t, x0);
                if (kx < x0 - 8 || kx > x1 + 8) continue;
                key_shape(dl, kx, cy, kr * 0.75f, SEQI_LINEAR, TH(TK_N12), key_selected((int)ti, (int)k));
                if (in_row && fabsf(m.x - kx) <= kr + 2) *hit_group = true, *hit_t = t2->keys[k].t, *hit_track = (int)ti;
            }
        }
        return;
    }
    uint32_t sc = section_color(trk->type);
    float sy0 = y + 3, sy1 = y + h - 4;
    if (r->kind == ROW_TRACK && trk->nkeys) { // the sections
        switch (trk->type) {
        case SEQT_ANIMATION: case SEQT_CAMERA_CUTS: case SEQT_AUDIO:
            for (uint32_t k = 0; k < trk->nkeys; k++) {
                float a = trk->keys[k].t, b = k + 1 < trk->nkeys ? trk->keys[k + 1].t : U.s->length;
                if (trk->type == SEQT_ANIMATION) {
                    const AnimSeq* q = anim_seq_name(trk->keys[k].name);
                    if (q && q->length > 0.0f && !trk->loop_anim) b = fminf(b, a + q->length);
                }
                float xa = fmaxf(t2x(a, x0), x0), xb = fminf(t2x(b, x0), x1);
                if (xb <= xa) continue;
                ImDrawList_AddRectFilled(dl, (ImVec2){ xa, sy0 }, (ImVec2){ xb - 1, sy1 }, sc, 3.0f, 0);
                ImDrawList_AddRectFilled(dl, (ImVec2){ xa, sy0 }, (ImVec2){ xb - 1, sy0 + 3 }, sc | 0xFF404040u, 3.0f, 0);
                ImDrawList_PushClipRect(dl, (ImVec2){ xa + 2, sy0 }, (ImVec2){ xb - 3, sy1 }, true);
                const char* nm = trk->keys[k].name[0] ? trk->keys[k].name : TR("(none)");
                ImDrawList_AddText_Vec2(dl, (ImVec2){ xa + kr * 2 + 4, cy - fs * 0.5f }, TH(TK_N15), nm, NULL);
                ImDrawList_PopClipRect(dl);
            }
            break;
        case SEQT_VISIBILITY: // the visible spans
            for (uint32_t k = 0; k < trk->nkeys; k++) {
                if (trk->keys[k].v[0] == 0.0f) continue;
                float a = t2x(trk->keys[k].t, x0), b = t2x(k + 1 < trk->nkeys ? trk->keys[k + 1].t : U.s->length, x0);
                ImDrawList_AddRectFilled(dl, (ImVec2){ fmaxf(a, x0), sy0 }, (ImVec2){ fminf(b, x1) - 1, sy1 }, sc, 3.0f, 0);
            }
            break;
        case SEQT_EVENT:
            break;
        default: { // a curve's span, first key to the end of the range (it holds after its last key)
            float a = fmaxf(t2x(trk->keys[0].t, x0), x0), b = fminf(t2x(U.s->length, x0), x1);
            if (b > a) ImDrawList_AddRectFilled(dl, (ImVec2){ a, sy0 }, (ImVec2){ b, sy1 }, sc, 3.0f, 0);
            break;
        }
        }
    }
    // the keys
    for (uint32_t k = 0; k < trk->nkeys; k++) {
        float kx = t2x(trk->keys[k].t, x0);
        if (kx < x0 - 8 || kx > x1 + 8) continue;
        bool ks = key_selected(r->track, (int)k);
        int shape = (trk->type == SEQT_TRANSFORM || seq_track_channels(trk->type)) ? trk->keys[k].interp : SEQI_CONSTANT;
        key_shape(dl, kx, cy, r->kind == ROW_CHANNEL ? kr * 0.8f : kr, shape, TH(TK_N14), ks);
        if (trk->type == SEQT_EVENT && r->kind == ROW_TRACK)
            ImDrawList_AddText_Vec2(dl, (ImVec2){ kx + kr + 3, cy - fs * 0.5f }, TH(TK_N14), trk->keys[k].name, NULL);
        if (in_row && fabsf(m.x - kx) <= kr + 2) *hit_track = r->track, *hit_key = (int)k;
    }
}

static void timeline(Editor* ed, float w, float h, float tree_w) {
    World* world = &ed->world;
    ImGuiIO* io = igGetIO_Nil();
    float fs = igGetFontSize(), row = igGetFrameHeight() + 6.0f, ruler = igGetFrameHeight() + 6.0f;
    ImVec2 o = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    float x0 = o.x + tree_w + 4, x1 = o.x + w, y0 = o.y, yb = o.y + h;
    // backgrounds: the tree, the lanes, the ruler
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + tree_w, yb }, TH(TK_N4), 0, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, yb }, TH(TK_N3), 0, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y0 + ruler }, TH(TK_N5), 0, 0);
    // the tree's head: the search (Unreal's "Search Tracks")
    igSetCursorScreenPos((ImVec2){ o.x + 4, y0 + 3 });
    igSetNextItemWidth(tree_w - 8);
    igInputTextWithHint("##seq_search", TR("Search Tracks"), U.search, sizeof U.search, 0, NULL, NULL);
    // the ruler: frames (or seconds) with a step that keeps the labels apart
    float len = U.s->length, fps = U.s->fps > 0.0f ? U.s->fps : 30.0f;
    static const float steps_f[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600 };
    float px_frame = U.zoom / fps, minor = 1.0f;
    for (size_t i = 0; i < sizeof steps_f / sizeof steps_f[0]; i++)
        if (steps_f[i] * px_frame >= 9.0f) { minor = steps_f[i]; break; }
    float major = minor * 5.0f;
    float f_first = floorf(U.view0 * fps / minor) * minor;
    ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, yb }, true);
    for (float f = f_first;; f += minor) {
        float x = t2x(f / fps, x0);
        if (x > x1) break;
        if (x < x0) continue;
        bool maj = fmodf(f, major) < 0.5f;
        ImDrawList_AddLine(dl, (ImVec2){ x, y0 + ruler * (maj ? 0.45f : 0.75f) }, (ImVec2){ x, y0 + ruler }, TH(TK_N11), 1.0f);
        if (maj) {
            ImDrawList_AddLine(dl, (ImVec2){ x, y0 + ruler }, (ImVec2){ x, yb }, TH(TK_N6), 1.0f);
            char lab[24];
            fmt_time(lab, sizeof lab, f / fps);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 3, y0 + 2 }, TH(TK_N13), lab, NULL);
        }
    }
    // the playback range: outside it shaded; its start green, its end red (dragged on the ruler)
    float rs = t2x(U.s->start, x0), re = t2x(len, x0);
    if (rs > x0) ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 + ruler }, (ImVec2){ fminf(rs, x1), yb }, THA(TK_N0, 96), 0, 0);
    if (re < x1) ImDrawList_AddRectFilled(dl, (ImVec2){ fmaxf(re, x0), y0 + ruler }, (ImVec2){ x1, yb }, THA(TK_N0, 96), 0, 0);
    ImDrawList_AddLine(dl, (ImVec2){ rs, y0 }, (ImVec2){ rs, yb }, TH(TK_GREEN), 2.0f);
    ImDrawList_AddLine(dl, (ImVec2){ re, y0 }, (ImVec2){ re, yb }, TH(TK_RED), 2.0f);
    ImDrawList_PopClipRect(dl);
    // the rows
    build_rows();
    float lanes_y0 = y0 + ruler, lanes_h = yb - lanes_y0;
    float total = U.nrows * row;
    if (U.scroll > total - lanes_h) U.scroll = total - lanes_h;
    if (U.scroll < 0.0f) U.scroll = 0.0f;
    int remove_track = -1;
    char remove_group[SEQ_NAME] = "";
    int hit_track = -1, hit_key = -1;
    bool hit_group = false;
    float hit_t = 0.0f;
    int hover_row = -1;
    ImDrawList_PushClipRect(dl, (ImVec2){ o.x, lanes_y0 }, (ImVec2){ x1, yb }, true);
    for (uint32_t ri = 0; ri < U.nrows; ri++) {
        float ry = lanes_y0 + ri * row - U.scroll;
        if (ry + row < lanes_y0 || ry > yb) continue;
        const Row* r = &U.rows[ri];
        if (r->kind == ROW_GROUP) ImDrawList_AddRectFilled(dl, (ImVec2){ x0, ry }, (ImVec2){ x1, ry + row - 1 }, TH(TK_N5), 0, 0);
        else if (r->track == U.sel_track) ImDrawList_AddRectFilled(dl, (ImVec2){ x0, ry }, (ImVec2){ x1, ry + row - 1 }, TH(TK_N4), 0, 0);
        ImDrawList_AddLine(dl, (ImVec2){ x0, ry + row - 1 }, (ImVec2){ x1, ry + row - 1 }, TH(TK_N5), 1.0f);
        tree_row(ed, r, o.x, ry, tree_w, row, (int)ri, &remove_track, remove_group);
        ImDrawList_PushClipRect(dl, (ImVec2){ x0, fmaxf(ry, lanes_y0) }, (ImVec2){ x1, fminf(ry + row, yb) }, true);
        track_lane(dl, r, x0, x1, ry, row, &hit_track, &hit_key, &hit_group, &hit_t);
        ImDrawList_PopClipRect(dl);
        if (io->MousePos.y >= ry && io->MousePos.y < ry + row) hover_row = (int)ri;
    }
    ImDrawList_PopClipRect(dl);
    if (!U.nrows) {
        const char* hint = TR("Add an actor with + Track > Actor To Sequencer, drag one from the Outliner here, or Create Camera.");
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 10, lanes_y0 + 10 }, TH(TK_N11), hint, NULL);
    }
    // an actor dragged from the Outliner onto the tree: bound
    igSetCursorScreenPos((ImVec2){ o.x, lanes_y0 });
    igDummy((ImVec2){ tree_w, lanes_h > 1 ? lanes_h : 1 });
    if (igBeginDragDropTarget()) {
        const ImGuiPayload* pl = igAcceptDragDropPayload("RE_OL_ACTORS", 0);
        if (pl && pl->DataSize == (int)sizeof(Entity)) bind_actor(ed, *(const Entity*)pl->Data);
        igEndDragDropTarget();
    }
    // the lanes' mouse: the ruler, the range's ends, the keys, the box, the pan, the zoom
    bool over_lanes = io->MousePos.x >= x0 && io->MousePos.x < x1 && io->MousePos.y >= y0 && io->MousePos.y < yb && igIsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    bool on_ruler = over_lanes && io->MousePos.y < lanes_y0;
    float mt = x2t(io->MousePos.x, x0);
    if (over_lanes && !igIsAnyItemActive() && U.drag == DRAG_NONE) {
        if (on_ruler && igIsMouseClicked_Bool(0, false)) {
            if (fabsf(io->MousePos.x - rs) < 5) U.drag = DRAG_START;
            else if (fabsf(io->MousePos.x - re) < 5) U.drag = DRAG_END;
            else U.drag = DRAG_HEAD, U.playing = false;
        } else if (!on_ruler && igIsMouseClicked_Bool(0, false)) {
            bool add = io->KeyCtrl || io->KeyShift;
            if (hit_key >= 0) {
                if (add && key_selected(hit_track, hit_key)) unselect_key(hit_track, hit_key);
                else if (!key_selected(hit_track, hit_key)) select_key(hit_track, hit_key, add);
                U.sel_track = hit_track;
                U.drag = DRAG_KEYS;
            } else if (hit_group) { // every key of the actor at that time
                if (!add) U.nsel = 0;
                const char* actor = U.s->tracks[hit_track].actor;
                for (uint32_t ti = 0; ti < U.s->ntracks; ti++)
                    if (!strcmp(U.s->tracks[ti].actor, actor))
                        for (uint32_t k = 0; k < U.s->tracks[ti].nkeys; k++)
                            if (fabsf(U.s->tracks[ti].keys[k].t - hit_t) < 1e-4f) select_key((int)ti, (int)k, true);
                U.drag = DRAG_KEYS;
            } else {
                if (!add) U.nsel = 0;
                if (hover_row >= 0 && U.rows[hover_row].kind != ROW_GROUP) U.sel_track = U.rows[hover_row].track;
                U.drag = DRAG_BOX;
            }
            U.drag_x0 = io->MousePos.x, U.drag_y0 = io->MousePos.y, U.drag_t0 = mt;
            for (uint32_t i = 0; i < U.nsel; i++) U.drag_orig[i] = U.s->tracks[U.sel[i].track].keys[U.sel[i].key].t;
        } else if (!on_ruler && igIsMouseDoubleClicked_Nil(0) && hover_row >= 0 && U.rows[hover_row].kind != ROW_GROUP && hit_key < 0) {
            add_key_at(ed, U.rows[hover_row].track, mt);
        } else if (igIsMouseClicked_Bool(2, false)) U.drag = DRAG_PAN, U.drag_x0 = io->MousePos.x, U.drag_t0 = U.view0;
        else if (!on_ruler && igIsMouseClicked_Bool(1, false) && hit_key >= 0) {
            if (!key_selected(hit_track, hit_key)) select_key(hit_track, hit_key, false);
            igOpenPopup_Str("##key_ctx", 0);
        }
        if (io->MouseWheel != 0.0f) {
            if (io->KeyCtrl) { // zoom at the mouse
                float before = mt;
                U.zoom *= io->MouseWheel > 0 ? 1.15f : 1.0f / 1.15f;
                U.zoom = U.zoom < 4.0f ? 4.0f : U.zoom > 4000.0f ? 4000.0f : U.zoom;
                U.view0 = before - (io->MousePos.x - x0) / U.zoom;
            } else if (io->KeyShift) U.view0 -= io->MouseWheel * 60.0f / U.zoom;
            else U.scroll -= io->MouseWheel * row * 2.0f;
            if (U.view0 < -1.0f) U.view0 = -1.0f;
        }
    }
    if (igBeginPopup("##key_ctx", 0)) {
        interp_menu();
        igEndPopup();
    }
    // the drags
    if (U.drag != DRAG_NONE) {
        if (!igIsMouseDown_Nil(0) && !igIsMouseDown_Nil(2)) {
            if (U.drag == DRAG_KEYS) resort(), changed(), preview(ed);
            U.drag = DRAG_NONE;
        } else {
            switch (U.drag) {
            case DRAG_HEAD: set_time(ed, mt); break;
            case DRAG_START: U.s->start = snapt(fminf(fmaxf(mt, 0.0f), U.s->length - 1.0f / fps)), changed(); break;
            case DRAG_END: U.s->length = snapt(fmaxf(mt, U.s->start + 1.0f / fps)), changed(); break;
            case DRAG_PAN: U.view0 = U.drag_t0 - (io->MousePos.x - U.drag_x0) / U.zoom; break;
            case DRAG_KEYS: {
                float d = snapt(mt) - snapt(U.drag_t0);
                bool moved = false;
                for (uint32_t i = 0; i < U.nsel; i++) {
                    SeqKey* k = &U.s->tracks[U.sel[i].track].keys[U.sel[i].key];
                    float nt = fmaxf(U.drag_orig[i] + d, 0.0f);
                    if (nt != k->t) k->t = nt, moved = true;
                }
                if (moved) changed(), preview(ed);
                break;
            }
            case DRAG_BOX: {
                float bx0 = fminf(U.drag_x0, io->MousePos.x), bx1 = fmaxf(U.drag_x0, io->MousePos.x);
                float by0 = fminf(U.drag_y0, io->MousePos.y), by1 = fmaxf(U.drag_y0, io->MousePos.y);
                ImDrawList_AddRectFilled(dl, (ImVec2){ bx0, by0 }, (ImVec2){ bx1, by1 }, THA(TK_BLUE, 48), 0, 0);
                ImDrawList_AddRect(dl, (ImVec2){ bx0, by0 }, (ImVec2){ bx1, by1 }, THA(TK_BLUE, 192), 0, 1.0f, 0);
                if (!(io->KeyCtrl || io->KeyShift)) U.nsel = 0;
                for (uint32_t ri = 0; ri < U.nrows; ri++) {
                    const Row* r = &U.rows[ri];
                    if (r->kind == ROW_GROUP) continue;
                    float cy = lanes_y0 + ri * row - U.scroll + row * 0.5f;
                    if (cy < by0 || cy > by1) continue;
                    const SeqTrack* trk = &U.s->tracks[r->track];
                    for (uint32_t k = 0; k < trk->nkeys; k++) {
                        float kx = t2x(trk->keys[k].t, x0);
                        if (kx >= bx0 && kx <= bx1) select_key(r->track, (int)k, true);
                    }
                }
                break;
            }
            default: break;
            }
        }
    }
    // the playhead (Unreal's: a red line, its handle on the ruler with the frame)
    float hx = t2x(U.t, x0);
    if (hx >= x0 && hx <= x1) {
        ImDrawList_AddLine(dl, (ImVec2){ hx, y0 }, (ImVec2){ hx, yb }, TH(TK_RED), 1.5f);
        char lab[24];
        fmt_time(lab, sizeof lab, U.t);
        ImVec2 ts = igCalcTextSize(lab, NULL, false, -1);
        ImDrawList_AddRectFilled(dl, (ImVec2){ hx - ts.x * 0.5f - 4, y0 + 1 }, (ImVec2){ hx + ts.x * 0.5f + 4, y0 + ruler - 2 }, TH(TK_RED), 3.0f, 0);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ hx - ts.x * 0.5f, y0 + (ruler - fs) * 0.5f - 1 }, TH(TK_N15), lab, NULL);
    }
    if (remove_track >= 0) {
        seq_remove_track(U.s, remove_track), U.nsel = 0, U.sel_track = -1, changed();
        memset(U.have_written, 0, sizeof U.have_written);
    }
    if (remove_group[0]) {
        for (int i = (int)U.s->ntracks - 1; i >= 0; i--)
            if (!strcmp(U.s->tracks[i].actor, remove_group)) seq_remove_track(U.s, i);
        U.nsel = 0, U.sel_track = -1, changed();
        memset(U.have_written, 0, sizeof U.have_written);
    }
    (void)world;
    igSetCursorScreenPos((ImVec2){ o.x, yb });
}

// --- Bottom: Details and the Curve Editor ---

static void details(Editor* ed) {
    if (!U.nsel) { // the sequence itself
        igSeparatorText(TR("Level Sequence"));
        float st = U.s->start, en = U.s->length, fps = U.s->fps;
        igSetNextItemWidth(igGetFontSize() * 8);
        if (igDragFloat(TR("Start"), &st, 1.0f / fps, 0.0f, en, "%.3f s", 0)) U.s->start = fminf(fmaxf(st, 0.0f), en), changed();
        igSameLine(0, 12);
        igSetNextItemWidth(igGetFontSize() * 8);
        if (igDragFloat(TR("End"), &en, 1.0f / fps, st + 1.0f / fps, 36000.0f, "%.3f s", 0)) U.s->length = fmaxf(en, st + 1.0f / fps), changed();
        igTextDisabled(TR("%d frames at %.0f fps; %u tracks. Select keys to edit them."), (int)roundf((U.s->length - U.s->start) * fps), fps, U.s->ntracks);
        return;
    }
    if (U.nsel > 1) {
        igSeparatorText(TR("Keys"));
        igText(TR("%u keys selected"), U.nsel);
        if (igButton(TR("Interpolation..."), (ImVec2){ 0, 0 })) igOpenPopup_Str("##multi_interp", 0);
        if (igBeginPopup("##multi_interp", 0)) {
            interp_menu();
            igEndPopup();
        }
        return;
    }
    SeqTrack* trk = &U.s->tracks[U.sel[0].track];
    if (U.sel[0].key >= trk->nkeys) return;
    SeqKey* k = &trk->keys[U.sel[0].key];
    char head[160];
    snprintf(head, sizeof head, "%s%s%s", trk->actor, trk->actor[0] ? " - " : "", TR(seq_track_type_name(trk->type)));
    igSeparatorText(head);
    bool ch = false;
    float frame = roundf(k->t * U.s->fps);
    igSetNextItemWidth(igGetFontSize() * 7);
    if (igDragFloat(TR("Frame"), &frame, 0.2f, 0.0f, U.s->length * U.s->fps, "%.0f", 0)) k->t = fmaxf(frame, 0.0f) / U.s->fps, ch = true;
    if (seq_track_channels(trk->type)) {
        igSameLine(0, 14);
        int m = k->interp;
        const char* names[SEQI_COUNT];
        for (int i = 0; i < SEQI_COUNT; i++) names[i] = TR(seq_interp_name(i));
        igSetNextItemWidth(igGetFontSize() * 7);
        if (igCombo_Str_arr(TR("Interpolation"), &m, names, SEQI_COUNT, -1)) set_interp_selected(m), ch = true;
    }
    switch (trk->type) {
    case SEQT_TRANSFORM:
        ch |= igDragFloat3(TR("Location"), &k->v[0], 1.0f, 0, 0, "%.1f", 0);
        ch |= igDragFloat3(TR("Rotation"), &k->v[3], 0.5f, 0, 0, "%.1f°", 0);
        ch |= igDragFloat3(TR("Scale"), &k->v[6], 0.01f, 0, 0, "%.3f", 0);
        if (igButton(TR("Set From the Actor"), (ImVec2){ 0, 0 })) capture(&ed->world, trk, k), ch = true;
        break;
    case SEQT_VISIBILITY: {
        bool v = k->v[0] != 0.0f;
        if (igCheckbox(TR("Visible"), &v)) k->v[0] = v ? 1.0f : 0.0f, ch = true;
        break;
    }
    case SEQT_LIGHT_INTENSITY: ch |= igDragFloat(TR("Intensity"), &k->v[0], 10.0f, 0.0f, 1e7f, "%.1f", 0); break;
    case SEQT_FOV: ch |= igDragFloat(TR("Field Of View"), &k->v[0], 0.5f, 5.0f, 170.0f, "%.1f°", 0); break;
    case SEQT_FADE:
        ch |= igSliderFloat(TR("Fade Amount"), &k->v[0], 0.0f, 1.0f, "%.2f", 0);
        ch |= igColorEdit3(TR("Fade Color"), &k->v[1], 0);
        break;
    case SEQT_HORROR_FILTER: { // (Q13) which filter (or the letterbox) and how strong from this key on
        if (igBeginCombo(TR("Key Filter"), k->name[0] ? TR(k->name) : TR("None"), 0)) {
            for (int fi = 0; fi <= TF_COUNT; fi++) {
                const char* fn = fi < TF_COUNT ? terror_filter_name(fi) : "Letterbox";
                if (igSelectable_Bool(TR(fn), !strcmp(k->name, fn), 0, (ImVec2){ 0, 0 })) snprintf(k->name, sizeof k->name, "%s", fn), ch = true;
            }
            igEndCombo();
        }
        ch |= igSliderFloat(TR("Filter Intensity"), &k->v[0], 0.0f, 1.0f, "%.2f", 0);
        break;
    }
    case SEQT_AUDIO: {
        if (igBeginCombo(TR("Sound"), k->name[0] ? k->name : TR("None"), 0)) {
            for (uint32_t i = 0, n = audio_wave_count(); i < n; i++)
                if (igSelectable_Bool(audio_wave_at(i)->name, !strcmp(k->name, audio_wave_at(i)->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(k->name, sizeof k->name, "%s", audio_wave_at(i)->name), ch = true;
            for (uint32_t i = 0, n = au_asset_count(); i < n; i++) {
                AuAsset* a = au_asset_at(i);
                if (a->kind == AUA_CUE && igSelectable_Bool(a->name, !strcmp(k->name, a->name), 0, (ImVec2){ 0, 0 }))
                    snprintf(k->name, sizeof k->name, "%s", a->name), ch = true;
            }
            igEndCombo();
        }
        if (k->v[0] <= 0.0f) k->v[0] = 1.0f;
        if (k->v[1] <= 0.0f) k->v[1] = 1.0f;
        ch |= igDragFloat(TR("Volume Multiplier"), &k->v[0], 0.01f, 0.0f, 4.0f, "%.2f", 0);
        ch |= igDragFloat(TR("Pitch Multiplier"), &k->v[1], 0.01f, 0.1f, 4.0f, "%.2f", 0);
        if (k->name[0] && igButton(TR("Preview"), (ImVec2){ 0, 0 })) audio_play_2d(audio_sound_by_name(k->name), k->v[0], k->v[1]);
        break;
    }
    case SEQT_ANIMATION:
        if (igBeginCombo(TR("Animation Sequence"), k->name[0] ? k->name : TR("None"), 0)) {
            for (uint32_t i = 0, n = anim_seq_count(); i < n; i++) {
                const AnimSeq* q = anim_seq_at(i);
                if (!q) continue;
                if (igSelectable_Bool(q->name, !strcmp(k->name, q->name), 0, (ImVec2){ 0, 0 })) snprintf(k->name, sizeof k->name, "%s", q->name), ch = true;
            }
            igEndCombo();
        }
        break;
    case SEQT_CAMERA_CUTS: {
        if (igBeginCombo(TR("Camera Actor"), k->name[0] ? k->name : TR("None"), 0)) {
            World* w = &ed->world;
            for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                const EcsArchetype* a = &w->archetypes[ai];
                if (!(a->mask & COMP_BIT(COMP_CAMERA)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
                const CName* nm = ecs_arch_column(a, COMP_NAME);
                for (uint32_t r = 0; r < a->count; r++)
                    if (igSelectable_Bool(nm[r].text, !strcmp(k->name, nm[r].text), 0, (ImVec2){ 0, 0 })) snprintf(k->name, sizeof k->name, "%s", nm[r].text), ch = true;
            }
            igEndCombo();
        }
        break;
    }
    case SEQT_EVENT:
        ch |= igInputText(TR("Event Name"), k->name, sizeof k->name, 0, NULL, NULL);
        igTextDisabled("%s", TR("A Custom Event of the bound actor's Blueprint (none bound: every Blueprint actor that has it)."));
        break;
    default: break;
    }
    if (ch) resort(), changed(), preview(ed);
}

// Unreal's Curve Editor: the selected track's channels as curves over time
static void curve_editor(Editor* ed, float w, float h) {
    if (U.sel_track < 0 || (uint32_t)U.sel_track >= U.s->ntracks || !seq_track_channels(U.s->tracks[U.sel_track].type)) {
        igTextDisabled("%s", TR("Select a Transform, Light Intensity, Field Of View or Fade track to see its curves."));
        return;
    }
    SeqTrack* trk = &U.s->tracks[U.sel_track];
    int nch = seq_track_channels(trk->type);
    ImGuiIO* io = igGetIO_Nil();
    float fs = igGetFontSize(), list_w = nch > 1 ? fs * 10.0f : 0.0f;
    ImVec2 o = igGetCursorScreenPos();
    // the channels' list (Transform: which of the nine are drawn)
    if (nch > 1) {
        igBeginChild_Str("##cv_list", (ImVec2){ list_w, h }, 0, 0);
        for (int c = 0; c < 9; c++) {
            igPushStyleColor_U32(ImGuiCol_Text, theme_mix(TH(g_ch_cols[c]), TH(TK_TEXT_STRONG), 0.25f));
            if (igCheckbox(TR(g_ch_names[c]), &U.cv_ch[c])) U.cv_fit = true;
            igPopStyleColor(1);
        }
        igEndChild();
        igSameLine(0, 4);
    }
    ImVec2 g0 = igGetCursorScreenPos();
    float gw = w - list_w - 8, gh = h;
    if (gw < 40 || gh < 40) return;
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, g0, (ImVec2){ g0.x + gw, g0.y + gh }, TH(TK_N3), 0, 0);
    // the view: framed on the keys (F, or a new track)
    if (U.cv_fit || U.cv_track != U.sel_track) {
        float vmin = 1e30f, vmax = -1e30f, tmin = U.s->start, tmax = U.s->length;
        for (int c = 0; c < nch; c++) {
            if (nch > 1 && !U.cv_ch[c]) continue;
            for (float t = tmin; t <= tmax; t += (tmax - tmin) / 64.0f + 1e-4f) {
                float v = seq_channel_at(trk, c, t);
                vmin = fminf(vmin, v), vmax = fmaxf(vmax, v);
            }
        }
        if (vmin > vmax) vmin = 0, vmax = 1;
        float pad = fmaxf((vmax - vmin) * 0.12f, 1e-3f + fabsf(vmax) * 0.05f + 0.5f * (vmax == vmin));
        U.cv_v0 = vmin - pad, U.cv_v1 = vmax + pad, U.cv_t0 = tmin - (tmax - tmin) * 0.03f, U.cv_t1 = tmax + (tmax - tmin) * 0.03f;
        U.cv_fit = false, U.cv_track = U.sel_track;
    }
#define CX(t) (g0.x + ((t) - U.cv_t0) / (U.cv_t1 - U.cv_t0) * gw)
#define CY(v) (g0.y + gh - ((v) - U.cv_v0) / (U.cv_v1 - U.cv_v0) * gh)
    ImDrawList_PushClipRect(dl, g0, (ImVec2){ g0.x + gw, g0.y + gh }, true);
    // the grid: values and frames
    float vr = U.cv_v1 - U.cv_v0, vstep = powf(10.0f, floorf(log10f(vr / 4.0f)));
    for (float v = ceilf(U.cv_v0 / vstep) * vstep; v <= U.cv_v1; v += vstep) {
        float y = CY(v);
        ImDrawList_AddLine(dl, (ImVec2){ g0.x, y }, (ImVec2){ g0.x + gw, y }, TH(TK_N6), 1.0f);
        char lab[32];
        snprintf(lab, sizeof lab, "%.3g", v);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ g0.x + 3, y - fs - 1 }, TH(TK_N11), lab, NULL);
    }
    float tr_ = U.cv_t1 - U.cv_t0, tstep = powf(10.0f, floorf(log10f(tr_ / 6.0f)));
    for (float t = ceilf(U.cv_t0 / tstep) * tstep; t <= U.cv_t1; t += tstep)
        ImDrawList_AddLine(dl, (ImVec2){ CX(t), g0.y }, (ImVec2){ CX(t), g0.y + gh }, TH(TK_N5), 1.0f);
    ImDrawList_AddLine(dl, (ImVec2){ CX(U.t), g0.y }, (ImVec2){ CX(U.t), g0.y + gh }, TH(TK_RED), 1.5f);
    // the curves (a point per 2 px) and their keys
    int hit_k = -1, hit_c = -1, hit_side = 0;
    for (int c = 0; c < nch; c++) {
        if (nch > 1 && !U.cv_ch[c]) continue;
        uint32_t col = nch > 1 ? TH(g_ch_cols[c]) : TH(TK_AMBER);
        ImVec2 prev = { 0, 0 };
        for (float x = 0; x <= gw; x += 2.0f) {
            float t = U.cv_t0 + x / gw * (U.cv_t1 - U.cv_t0);
            ImVec2 p = { g0.x + x, CY(seq_channel_at(trk, c, t)) };
            if (x > 0) ImDrawList_AddLine(dl, prev, p, col, 1.6f);
            prev = p;
        }
        for (uint32_t k = 0; k < trk->nkeys; k++) {
            float kx = CX(trk->keys[k].t), ky = CY(trk->keys[k].v[c]);
            bool ks = key_selected(U.sel_track, (int)k);
            if (ks && (trk->keys[k].interp == SEQI_USER || trk->keys[k].interp == SEQI_AUTO)) { // the tangent's handles
                float e = 1e-3f, slope = trk->keys[k].interp == SEQI_USER ? trk->keys[k].tan[c] : (seq_channel_at(trk, c, trk->keys[k].t + e) - seq_channel_at(trk, c, trk->keys[k].t - e)) / (2 * e);
                float dx = 40.0f, dt = dx / gw * (U.cv_t1 - U.cv_t0), dy = CY(trk->keys[k].v[c] + slope * dt) - ky;
                float l = sqrtf(dx * dx + dy * dy), hx = dx / l * 40.0f, hy = dy / l * 40.0f;
                for (int sd = -1; sd <= 1; sd += 2) {
                    ImVec2 hp = { kx + hx * sd, ky + hy * sd };
                    ImDrawList_AddLine(dl, (ImVec2){ kx, ky }, hp, TH(TK_N13), 1.0f);
                    ImDrawList_AddCircleFilled(dl, hp, 3.5f, TH(TK_N14), 8);
                    if (fabsf(io->MousePos.x - hp.x) < 6 && fabsf(io->MousePos.y - hp.y) < 6) hit_k = (int)k, hit_c = c, hit_side = sd;
                }
            }
            key_shape(dl, kx, ky, 5.0f, trk->keys[k].interp, col | 0xFF404040u, ks);
            if (hit_side == 0 && fabsf(io->MousePos.x - kx) < 6 && fabsf(io->MousePos.y - ky) < 6) hit_k = (int)k, hit_c = c;
        }
    }
    ImDrawList_PopClipRect(dl);
    // the mouse
    igSetCursorScreenPos(g0);
    igInvisibleButton("##cv", (ImVec2){ gw, gh }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    bool hov = igIsItemHovered(0);
    float mt = U.cv_t0 + (io->MousePos.x - g0.x) / gw * (U.cv_t1 - U.cv_t0);
    float mv = U.cv_v0 + (g0.y + gh - io->MousePos.y) / gh * (U.cv_v1 - U.cv_v0);
    if (hov && igIsMouseClicked_Bool(0, false)) {
        if (hit_k >= 0) {
            if (!key_selected(U.sel_track, hit_k)) select_key(U.sel_track, hit_k, io->KeyCtrl || io->KeyShift);
            U.cv_key = hit_k, U.cv_c = hit_c, U.cv_side = hit_side;
            U.drag = hit_side ? DRAG_CV_TAN : DRAG_CV_KEY;
            if (hit_side && trk->keys[hit_k].interp != SEQI_USER) { // a handle grabbed: the key goes User (from its Auto slope)
                U.nsel = 0, select_key(U.sel_track, hit_k, false), set_interp_selected(SEQI_USER);
            }
        } else if (!(io->KeyCtrl || io->KeyShift)) U.nsel = 0;
    }
    if (hov && igIsMouseClicked_Bool(1, false) && hit_k >= 0) {
        if (!key_selected(U.sel_track, hit_k)) select_key(U.sel_track, hit_k, false);
        igOpenPopup_Str("##cv_ctx", 0);
    }
    if (igBeginPopup("##cv_ctx", 0)) {
        interp_menu();
        igEndPopup();
    }
    if (hov && igIsMouseClicked_Bool(2, false)) U.drag = DRAG_CV_PAN, U.drag_x0 = io->MousePos.x, U.drag_y0 = io->MousePos.y;
    if (hov && io->MouseWheel != 0.0f) { // zoom at the mouse (Ctrl: the values only)
        float f = io->MouseWheel > 0 ? 1.0f / 1.15f : 1.15f;
        if (!io->KeyCtrl) U.cv_t0 = mt + (U.cv_t0 - mt) * f, U.cv_t1 = mt + (U.cv_t1 - mt) * f;
        U.cv_v0 = mv + (U.cv_v0 - mv) * f, U.cv_v1 = mv + (U.cv_v1 - mv) * f;
    }
    if (hov && igIsKeyPressed_Bool(ImGuiKey_F, false)) U.cv_fit = true;
    if (U.drag == DRAG_CV_KEY || U.drag == DRAG_CV_TAN || U.drag == DRAG_CV_PAN) {
        if (!igIsMouseDown_Nil(0) && !igIsMouseDown_Nil(2)) {
            if (U.drag == DRAG_CV_KEY) resort();
            U.drag = DRAG_NONE;
        } else if (U.drag == DRAG_CV_PAN) {
            float dt = -(io->MousePos.x - U.drag_x0) / gw * (U.cv_t1 - U.cv_t0), dv = (io->MousePos.y - U.drag_y0) / gh * (U.cv_v1 - U.cv_v0);
            U.cv_t0 += dt, U.cv_t1 += dt, U.cv_v0 += dv, U.cv_v1 += dv;
            U.drag_x0 = io->MousePos.x, U.drag_y0 = io->MousePos.y;
        } else if (U.cv_key >= 0 && (uint32_t)U.cv_key < trk->nkeys) {
            SeqKey* k = &trk->keys[U.cv_key];
            if (U.drag == DRAG_CV_KEY) {
                k->v[U.cv_c] = mv;
                if (!io->KeyShift) k->t = fmaxf(snapt(mt), 0.0f); // (Shift: only the value)
            } else { // the tangent: the slope toward the mouse (the left handle mirrored)
                float dt = mt - k->t, dv = mv - k->v[U.cv_c];
                if (fabsf(dt) > 1e-5f) k->tan[U.cv_c] = dv / dt;
            }
            changed(), preview(ed);
        }
    }
#undef CX
#undef CY
    igSetCursorScreenPos((ImVec2){ o.x, o.y + h });
}

// --- The window ---

static void toolbar(Editor* ed, bool focused) {
    if (ui_icon_button("seq_save", ICON_SAVE, TR("Save"), TH(TK_N14), false, TR("Saves the Level Sequence (Ctrl+S).")) ||
        (focused && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
        bool ok = seq_save(U.s);
        snprintf(U.status, sizeof U.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
        if (ok) U.unsaved = false;
    }
    igSameLine(0, 6);
    if (ui_icon_button("seq_add", ICON_PLUS, TR("Track"), TH(TK_GREEN), false, TR("Adds an actor to the sequence or a master track.")))
        igOpenPopup_Str("seq_add_menu", 0);
    if (igBeginPopup("seq_add_menu", 0)) {
        if (ui_begin_menu(ICON_ACTOR, TR("Actor To Sequencer"), true)) {
            const char* sn = actor_name(&ed->world, ed->selected);
            if (sn[0]) {
                char lab[96];
                snprintf(lab, sizeof lab, "%s (%s)", sn, TR("selected"));
                if (ui_menu_item(ICON_SELECT, lab, NULL, false, true)) bind_actor(ed, ed->selected);
                igSeparator();
            }
            World* w = &ed->world;
            int shown = 0;
            for (uint32_t ai = 0; ai < w->archetype_count && shown < 200; ai++) {
                const EcsArchetype* a = &w->archetypes[ai];
                if (!a->count || !(a->mask & COMP_BIT(COMP_NAME)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
                const CName* nm = ecs_arch_column(a, COMP_NAME);
                const Entity* ents = ecs_arch_entities(a);
                for (uint32_t r = 0; r < a->count && shown < 200; r++, shown++)
                    if (nm[r].text[0] && ui_menu_item(ICON_ACTOR, nm[r].text, NULL, false, find_track(nm[r].text, SEQT_TRANSFORM) < 0)) bind_actor(ed, ents[r]);
            }
            igEndMenu();
        }
        igSeparatorText(TR("Master Tracks"));
        if (ui_menu_item(ICON_CAMERA, TR("Camera Cuts"), NULL, false, find_track("", SEQT_CAMERA_CUTS) < 0)) add_track(ed, SEQT_CAMERA_CUTS, "");
        if (ui_menu_item(ICON_EVENT, TR("Event"), NULL, false, true)) add_track(ed, SEQT_EVENT, "");
        if (ui_menu_item(ICON_SOUND, TR("Audio"), NULL, false, true)) add_track(ed, SEQT_AUDIO, "");
        if (ui_menu_item(ICON_EXPOSURE, TR("Fade"), NULL, false, find_track("", SEQT_FADE) < 0)) add_track(ed, SEQT_FADE, "");
        if (ui_menu_item(ICON_FILTER, TR("Horror Filter"), NULL, false, true)) add_track(ed, SEQT_HORROR_FILTER, ""); // (Q13: one track a filter at once)
        igEndPopup();
    }
    igSameLine(0, 10);
    { // Auto Key: Unreal's red dot
        float s = igGetFrameHeight();
        ImVec2 p = igGetCursorScreenPos();
        if (igInvisibleButton("seq_autokey", (ImVec2){ s, s }, 0)) U.auto_key = !U.auto_key;
        igSetItemTooltip("%s", TR("Auto Key: moving a bound actor in the viewport keys it at the playhead."));
        ImDrawList* dl = igGetWindowDrawList();
        if (U.auto_key || igIsItemHovered(0)) ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + s, p.y + s }, U.auto_key ? TH(TK_RED_D) : TH(TK_N8), 3.0f, 0);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ p.x + s * 0.5f, p.y + s * 0.5f }, s * 0.26f, U.auto_key ? TH(TK_RED) : TH(TK_RED), 16);
    }
    igSameLine(0, 6);
    if (ui_icon_button("seq_cam", ICON_CAMERA, TR("Create Camera"), TH(TK_N14), false, TR("A Camera Actor where the viewport looks, bound to the sequence, its cut at the playhead."))) {
        float yaw = ed->cam.yaw, pitch = ed->cam.pitch;
        Entity c = editor_spawn_camera(ed, ed->cam.pos, pitch, yaw, igGetTime());
        const char* cn = actor_name(&ed->world, c);
        if (c && cn[0]) {
            add_track(ed, SEQT_TRANSFORM, cn);
            add_track(ed, SEQT_FOV, cn);
            int cuts = find_track("", SEQT_CAMERA_CUTS);
            if (cuts < 0) cuts = add_track(ed, SEQT_CAMERA_CUTS, "");
            if (cuts >= 0) {
                int at = add_key_at(ed, cuts, U.t);
                if (at >= 0) snprintf(U.s->tracks[cuts].keys[at].name, SEQ_NAME, "%s", cn);
            }
            U.lock_cam = true;
            snprintf(U.status, sizeof U.status, TR("%s made: the viewport looks through it (Lock to Camera Cuts)."), cn);
        }
    }
    igSameLine(0, 6);
    igCheckbox(TR("Lock to Camera Cuts"), &U.lock_cam);
    igSetItemTooltip("%s", TR("The viewport looks through the Camera Cuts' camera while the playhead moves."));
    igSameLine(0, 14);
    { // the display rate
        static const float rates[] = { 24, 25, 30, 48, 50, 60, 120 };
        char cur[16];
        snprintf(cur, sizeof cur, "%.0f fps", U.s->fps);
        igSetNextItemWidth(igGetFontSize() * 5.5f);
        if (igBeginCombo("##fps", cur, 0)) {
            for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
                char l[16];
                snprintf(l, sizeof l, "%.0f fps", rates[i]);
                if (igSelectable_Bool(l, U.s->fps == rates[i], 0, (ImVec2){ 0, 0 })) U.s->fps = rates[i], changed();
            }
            igEndCombo();
        }
        igSetItemTooltip("%s", TR("The display rate: the frames the keys snap to."));
    }
    igSameLine(0, 6);
    igCheckbox(TR("Snap"), &U.snap);
    igSetItemTooltip("%s", TR("Keys and the playhead snap to the frames."));
    igSameLine(0, 6);
    igCheckbox(TR("Frames"), &U.show_frames);
    igSetItemTooltip("%s", TR("Time shown in frames (off: seconds)."));
    if (U.status[0]) igSameLine(0, 14), igTextDisabled("%s", U.status);
}

static void transport_bar(Editor* ed, bool focused) {
    igBeginDisabled(ed->playing);
    if (transport("tp0", 0, false, TR("To the start (Home)")) || (focused && igIsKeyPressed_Bool(ImGuiKey_Home, false))) U.playing = false, U.last_eval = -1.0f, set_time(ed, U.s->start);
    igSameLine(0, 1);
    if (transport("tp1", 1, false, TR("Previous key"))) U.playing = false, set_time(ed, neighbor_key(-1, -1));
    igSameLine(0, 1);
    if (transport("tp2", 2, U.playing && U.dir < 0, TR("Play backwards"))) {
        if (U.playing && U.dir < 0) U.playing = false;
        else U.playing = true, U.dir = -1;
    }
    igSameLine(0, 1);
    if (transport("tp3", U.playing && U.dir > 0 ? 4 : 3, U.playing && U.dir > 0, TR("Play / Pause (Space)")) || (focused && igIsKeyPressed_Bool(ImGuiKey_Space, false) && !igGetIO_Nil()->WantTextInput)) {
        if (U.playing && U.dir > 0) U.playing = false;
        else {
            U.playing = true, U.dir = 1;
            if (U.t >= U.s->length) U.t = U.s->start, U.last_eval = -1.0f;
        }
    }
    igSameLine(0, 1);
    if (transport("tp5", 5, false, TR("Next key"))) U.playing = false, set_time(ed, neighbor_key(-1, 1));
    igSameLine(0, 1);
    if (transport("tp6", 6, false, TR("To the end (End)")) || (focused && igIsKeyPressed_Bool(ImGuiKey_End, false))) U.playing = false, set_time(ed, U.s->length);
    igSameLine(0, 1);
    if (transport("tp7", 7, U.loop, TR("Loop"))) U.loop = !U.loop;
    igSameLine(0, 8);
    float v = U.show_frames ? roundf(U.t * U.s->fps) : U.t;
    igSetNextItemWidth(igGetFontSize() * 4.5f);
    if (igDragFloat("##now", &v, U.show_frames ? 0.2f : 1.0f / U.s->fps, 0.0f, U.show_frames ? U.s->length * U.s->fps : U.s->length, U.show_frames ? "%04.0f" : "%.2f s", 0))
        U.playing = false, set_time(ed, U.show_frames ? v / U.s->fps : v);
    igSetItemTooltip("%s", TR("The current time (Left / Right: a frame)"));
    igEndDisabled();
    if (focused && !igGetIO_Nil()->WantTextInput) { // frames by the arrows
        if (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) U.playing = false, set_time(ed, U.t + 1.0f / U.s->fps);
        if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true)) U.playing = false, set_time(ed, U.t - 1.0f / U.s->fps);
    }
}

void seq_editor_draw(Editor* ed, double now, float dt) {
    (void)now;
    if (U.s && U.gen != seq_generation()) U.s = NULL, U.nsaved = 0, U.previewed = false; // (a project switched: its sequences went)
    if (!U.s) return;
    char title[200];
    snprintf(title, sizeof title, "%s%s - %s###sequencer", U.s->name, U.unsaved ? "*" : "", TR("Sequencer"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.72f, mv->Size.y * 0.42f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.14f, mv->Pos.y + mv->Size.y * 0.54f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true;
    igPushStyleColor_U32(ImGuiCol_WindowBg, TH(TK_N3));
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        bool focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        UD.focused = focused;
        if (!igIsMouseDown_Nil(0)) UD.pressed_push = false;
        auto_key(ed); // (before the preview overwrites what was moved)
        toolbar(ed, focused);
        // the preview playing (forward or backwards, inside the playback range)
        if (U.playing && !ed->playing) {
            float nt = U.t + dt * (float)U.dir;
            if (nt > U.s->length || nt < U.s->start) {
                if (U.loop) nt = U.dir > 0 ? U.s->start : U.s->length, U.last_eval = -1.0f;
                else nt = U.dir > 0 ? U.s->length : U.s->start, U.playing = false;
            }
            U.t = nt;
            if (U.dir < 0) U.last_eval = -1.0f; // (backwards: no events)
            preview(ed);
        }
        // the keyboard: S keys, Del deletes, Ctrl+C / Ctrl+V
        ImGuiIO* io = igGetIO_Nil();
        if (focused && !io->WantTextInput) {
            if (igIsKeyPressed_Bool(ImGuiKey_S, false) && !io->KeyCtrl && U.sel_track >= 0) add_key_at(ed, U.sel_track, U.t);
            if (igIsKeyPressed_Bool(ImGuiKey_Delete, false) && U.nsel) delete_selected();
            if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_C, false)) copy_keys();
            if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_V, false)) paste_keys();
            if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Z, false)) undo_step(io->KeyShift), preview(ed);
            if (io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_Y, false)) undo_step(true), preview(ed);
        }
        ImVec2 avail = igGetContentRegionAvail();
        float bottom = fmaxf(avail.y * 0.36f, igGetFrameHeightWithSpacing() * 5.0f);
        float tree_w = fminf(fmaxf(igGetFontSize() * 22.0f, avail.x * 0.24f), avail.x * 0.45f);
        // the transport under the tree's head (Unreal puts it at the tree's bottom; here above it: always in sight)
        transport_bar(ed, focused);
        avail = igGetContentRegionAvail();
        timeline(ed, avail.x, fmaxf(avail.y - bottom, 80.0f), tree_w);
        igSetCursorScreenPos((ImVec2){ igGetCursorScreenPos().x, igGetCursorScreenPos().y + 4 });
        if (igBeginTabBar("##seq_bottom", 0)) {
            if (igBeginTabItem(TR("Details"), NULL, 0)) {
                U.tab = 0;
                igBeginChild_Str("##seq_details", (ImVec2){ 0, 0 }, 0, 0);
                details(ed);
                igEndChild();
                igEndTabItem();
            }
            // (REALENGINE_SEQ_SHOT=curves: the screenshots' Curve Editor, the first curve track and its keys selected)
            static int shot_once = -1;
            if (shot_once < 0) shot_once = getenv("REALENGINE_SEQ_SHOT") && !strcmp(getenv("REALENGINE_SEQ_SHOT"), "curves");
            ImGuiTabItemFlags cf = 0;
            if (shot_once == 1) {
                for (uint32_t i = 0; i < U.s->ntracks && U.sel_track < 0; i++)
                    if (seq_track_channels(U.s->tracks[i].type)) {
                        U.sel_track = (int)i;
                        for (uint32_t k = 0; k < U.s->tracks[i].nkeys; k++) select_key((int)i, (int)k, true);
                    }
                cf = ImGuiTabItemFlags_SetSelected, shot_once = 2;
            }
            if (igBeginTabItem(TR("Curve Editor"), NULL, cf)) {
                U.tab = 1;
                ImVec2 a = igGetContentRegionAvail();
                curve_editor(ed, a.x, a.y);
                igEndTabItem();
            }
            igEndTabBar();
        }
    }
    igEnd();
    igPopStyleColor(1);
    if (!open) {
        seq_editor_restore(ed);
        undo_clear();
        U.s = NULL, UD.focused = false;
    }
}

bool seq_editor_is_open(void) { return U.s != NULL; }

// (2026-11-10) the undo's check (seq_test.c): a sequence of its own, a key added then moved, undone twice, redone once
bool seq_editor_selftest(void) {
    Sequence* keep = U.s;
    static Sequence t;
    memset(&t, 0, sizeof t);
    t.length = 2.0f, t.fps = 30.0f;
    U.s = &t;
    undo_clear();
    UD.last = snap_take(&t);
    int ti = seq_add_track(&t, SEQT_FOV, "Cam");
    changed();
    SeqKey k;
    memset(&k, 0, sizeof k);
    k.t = 1.0f, k.v[0] = 60.0f;
    seq_add_key(&t.tracks[ti], &k);
    changed();
    t.tracks[ti].keys[0].v[0] = 80.0f;
    changed();
    undo_step(false); // back to 60
    bool a = t.ntracks == 1 && t.tracks[0].nkeys == 1 && t.tracks[0].keys[0].v[0] == 60.0f;
    undo_step(false); // the key gone
    bool b = t.ntracks == 1 && t.tracks[0].nkeys == 0;
    undo_step(true); // the key back
    bool c = t.ntracks == 1 && t.tracks[0].nkeys == 1 && t.tracks[0].keys[0].v[0] == 60.0f;
    undo_clear();
    for (uint32_t i = 0; i < t.ntracks; i++) free(t.tracks[i].keys);
    U.s = keep, U.unsaved = false, U.status[0] = 0;
    if (keep) UD.last = snap_take(keep);
    printf("[RealEngine] seq-test: Sequencer undo %s / %s / redo %s\n", a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL");
    return a && b && c;
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
int seq_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    if (!U.s) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) { // (as its window's x: the actors it moved back where they were)
        if (ed) seq_editor_restore(ed);
        undo_clear();
        U.s = NULL, U.playing = false, U.unsaved = false, UD.focused = false;
        return 1;
    }
    if (!U.unsaved) return 0;
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, U.s->name), 1;
    bool ok = seq_save(U.s);
    if (ok) U.unsaved = false;
    return !ok;
}
