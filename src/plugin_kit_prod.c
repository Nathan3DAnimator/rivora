// (2026-11-25, item 18) The kit's Productivity plugins (plugin_kit.h): Camera Bookmarks, Actor Notes, Selection Sets,
// Command Palette, Asset References, Unused Assets.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_kit.h"

#include "async.h"
#include "blueprint.h"
#include "content.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "theme.h"
#include "ui_icons.h"

static bool ci_has(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (; *hay; hay++) {
        size_t k = 0;
        while (needle[k] && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}
static const char* level_key(Editor* ed) { return ed->level_path[0] ? ed->level_path : "(unsaved)"; }
static void ensure_dir_of(const char* path) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = 0, re_make_dirs(dir);
}
static Entity actor_named(World* w, const char* name) {
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        if (e && !strcmp(kit_actor_name(w, e), name)) return e;
    }
    return ENTITY_NONE;
}

// ---------------------------------------------------------------------------------------------------------------
// Camera Bookmarks: ten views of each level (Saved/CameraBookmarks.txt); Ctrl+Shift+0..9 keeps the view, a click goes back
#define BM_SLOTS 10
typedef struct Bookmark { bool set; EdCamera cam; char name[32]; } Bookmark;
static Bookmark g_bm[BM_SLOTS];
static char g_bm_level[1024] = "\x01"; // (the level whose bookmarks are loaded)

static void bm_path(char* out, size_t cap) { kit_project_file(out, cap, "Saved/CameraBookmarks.txt"); }
static void bm_load(Editor* ed) {
    memset(g_bm, 0, sizeof g_bm);
    snprintf(g_bm_level, sizeof g_bm_level, "%s", level_key(ed));
    char path[900];
    bm_path(path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        char* bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        if (strcmp(line, g_bm_level)) continue;
        int slot = -1, used = 0;
        EdCamera c = ed->cam;
        if (sscanf(bar + 1, "%d %f %f %f %f %f %f %n", &slot, &c.pos[0], &c.pos[1], &c.pos[2], &c.yaw, &c.pitch, &c.fov_deg, &used) < 7 || slot < 0 || slot >= BM_SLOTS) continue;
        g_bm[slot].set = true, g_bm[slot].cam = c;
        snprintf(g_bm[slot].name, sizeof g_bm[slot].name, "%s", bar + 1 + used);
        g_bm[slot].name[strcspn(g_bm[slot].name, "\r\n")] = 0;
    }
    fclose(f);
}
static void bm_save(void) { // (the other levels' lines kept)
    char path[900], tmp[920];
    bm_path(path, sizeof path);
    ensure_dir_of(path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* in = re_fopen(path, "rb");
    FILE* out = re_fopen(tmp, "wb");
    if (!out) {
        if (in) fclose(in);
        return;
    }
    char line[1400];
    size_t kl = strlen(g_bm_level);
    while (in && fgets(line, sizeof line, in))
        if (strncmp(line, g_bm_level, kl) || line[kl] != '|') fputs(line, out);
    if (in) fclose(in);
    for (int i = 0; i < BM_SLOTS; i++)
        if (g_bm[i].set)
            fprintf(out, "%s|%d %.3f %.3f %.3f %.3f %.3f %.3f %s\n", g_bm_level, i, g_bm[i].cam.pos[0], g_bm[i].cam.pos[1], g_bm[i].cam.pos[2], g_bm[i].cam.yaw,
                    g_bm[i].cam.pitch, g_bm[i].cam.fov_deg, g_bm[i].name);
    fclose(out);
    remove(path);
    rename(tmp, path);
}
static void bm_set(Editor* ed, int i) {
    g_bm[i].set = true, g_bm[i].cam = ed->cam;
    if (!g_bm[i].name[0]) snprintf(g_bm[i].name, sizeof g_bm[i].name, "View %d", i);
    bm_save();
}
static void bm_go(Editor* ed, int i) {
    if (!g_bm[i].set) return;
    float fov = ed->cam.fov_deg, speed = ed->cam.speed;
    ed->cam = g_bm[i].cam;
    ed->cam.speed = speed;
    if (g_bm[i].cam.fov_deg <= 1.0f) ed->cam.fov_deg = fov;
}
static void bm_tick(Editor* ed, double dt) {
    (void)dt;
    if (strcmp(g_bm_level, level_key(ed))) bm_load(ed);
    if (igGetIO_Nil()->WantTextInput || ed->playing) return;
    for (int i = 0; i < BM_SLOTS; i++)
        if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | (ImGuiKey)(ImGuiKey_0 + i))) bm_set(ed, i), editor_status(ed, re_time_seconds(), TR("Camera bookmark %d kept."), i);
}
static void bm_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("Ten camera views per level. Ctrl+Shift+0..9 (or Keep) remembers the view; Go flies back to it."));
    for (int i = 0; i < BM_SLOTS; i++) {
        igPushID_Int(i);
        igText("%d", i);
        igSameLine(28, -1);
        igSetNextItemWidth(160);
        if (igInputTextWithHint("##n", TR("Bookmark name"), g_bm[i].name, sizeof g_bm[i].name, 0, NULL, NULL) && g_bm[i].set) bm_save();
        igSameLine(0, -1);
        if (igButton(TR("Keep View"), (ImVec2){ 0, 0 })) bm_set(ed, i), editor_status(ed, now, TR("Camera bookmark %d kept."), i);
        igSameLine(0, -1);
        igBeginDisabled(!g_bm[i].set);
        if (igButton(TR("Go"), (ImVec2){ 0, 0 })) bm_go(ed, i);
        igSameLine(0, -1);
        if (igButton(TR("Forget"), (ImVec2){ 0, 0 })) g_bm[i].set = false, g_bm[i].name[0] = 0, bm_save();
        igEndDisabled();
        igPopID();
    }
}
static bool bm_test(Editor* ed, char* msg, size_t cap) {
    char path[900];
    bm_path(path, sizeof path);
    EdCamera keep = ed->cam;
    char keep_level[1024];
    snprintf(keep_level, sizeof keep_level, "%s", ed->level_path);
    snprintf(ed->level_path, sizeof ed->level_path, "%s", "/kit_selftest_level.rlevel");
    bm_load(ed);
    ed->cam.pos[0] = 1234.0f, ed->cam.yaw = 77.0f;
    bm_set(ed, 3);
    ed->cam.pos[0] = 0, ed->cam.yaw = 0;
    bm_load(ed); // (read back from the file)
    bm_go(ed, 3);
    bool ok = g_bm[3].set && ed->cam.pos[0] == 1234.0f && ed->cam.yaw == 77.0f;
    snprintf(msg, cap, "a view kept in slot 3, read back from the file and gone to: x %.0f, yaw %.0f", ed->cam.pos[0], ed->cam.yaw);
    g_bm[3].set = false, bm_save();
    snprintf(ed->level_path, sizeof ed->level_path, "%s", keep_level);
    ed->cam = keep;
    g_bm_level[0] = 1, g_bm_level[1] = 0;
    return ok;
}
static void bm_off(Editor* ed) { (void)ed, g_bm_level[0] = 1, g_bm_level[1] = 0; }

const KitPlugin kit_cam_bookmarks = { "CameraBookmarks", "Camera Bookmarks", "Productivity", "CAMERA",
    "Ten camera views for each level, named: Ctrl+Shift+0..9 keeps the current view and one click flies back to it. Kept in Saved/CameraBookmarks.txt.",
    true, bm_window, NULL, bm_tick, bm_off, bm_test };

// ---------------------------------------------------------------------------------------------------------------
// Actor Notes: a TODO on an actor (Config/ActorNotes.txt, by the level and the actor's name: shared through Git), shown
// over it in the viewport
#define NOTE_MAX 256
typedef struct Note { char level[256], actor[RE_NAME_MAX], text[160]; bool done; } Note;
static Note* g_notes; // (NOTE_MAX of them once read)
static int g_nnotes = -1;
static char g_note_text[160];
static bool g_note_overlay = true, g_note_hide_done;

static void notes_path(char* out, size_t cap) { kit_project_file(out, cap, "Config/ActorNotes.txt"); }
static void notes_load(void) {
    if (!g_notes && !(g_notes = kit_alloc(sizeof(Note) * NOTE_MAX))) return;
    g_nnotes = 0;
    char path[900];
    notes_path(path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    char line[700];
    while (g_nnotes < NOTE_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* a = strchr(line, '|');
        char* b = a ? strchr(a + 1, '|') : NULL;
        char* c = b ? strchr(b + 1, '|') : NULL;
        if (!c || line[0] == ';') continue;
        *a = *b = *c = 0;
        Note* n = &g_notes[g_nnotes++];
        snprintf(n->level, sizeof n->level, "%s", line);
        snprintf(n->actor, sizeof n->actor, "%s", a + 1);
        n->done = b[1] == '1';
        snprintf(n->text, sizeof n->text, "%s", c + 1);
    }
    fclose(f);
}
static void notes_save(void) {
    char path[900];
    notes_path(path, sizeof path);
    ensure_dir_of(path);
    FILE* f = re_fopen(path, "wb");
    if (!f) return;
    fputs("; Tools > Plugins > Actor Notes: level|actor|done|text\n", f);
    for (int i = 0; i < g_nnotes; i++) fprintf(f, "%s|%s|%d|%s\n", g_notes[i].level, g_notes[i].actor, g_notes[i].done, g_notes[i].text);
    fclose(f);
}
static bool note_add(Editor* ed, Entity e, const char* text) {
    if (g_nnotes < 0) notes_load();
    if (!g_notes || g_nnotes >= NOTE_MAX || !text[0] || !e) return false;
    Note* n = &g_notes[g_nnotes++];
    snprintf(n->level, sizeof n->level, "%s", level_key(ed));
    snprintf(n->actor, sizeof n->actor, "%s", kit_actor_name(&ed->world, e));
    snprintf(n->text, sizeof n->text, "%s", text);
    for (char* c = n->text; *c; c++)
        if (*c == '|' || *c == '\n') *c = ' ';
    n->done = false;
    notes_save();
    return true;
}
static void notes_window(Editor* ed, double now) {
    (void)now;
    if (g_nnotes < 0) notes_load();
    igBeginDisabled(ed->selected == ENTITY_NONE);
    igSetNextItemWidth(-90);
    bool enter = igInputTextWithHint("##note", TR("A note on the selected actor"), g_note_text, sizeof g_note_text, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSameLine(0, -1);
    if ((igButton(TR("Add Note"), (ImVec2){ -1, 0 }) || enter) && note_add(ed, ed->selected, g_note_text)) g_note_text[0] = 0;
    igEndDisabled();
    igCheckbox(TR("Show over the Actors"), &g_note_overlay);
    igSameLine(0, -1);
    igCheckbox(TR("Hide Done"), &g_note_hide_done);
    igSeparator();
    int del = -1, shown = 0;
    for (int i = 0; i < g_nnotes; i++) {
        Note* n = &g_notes[i];
        if (strcmp(n->level, level_key(ed)) || (g_note_hide_done && n->done)) continue;
        igPushID_Int(i);
        if (igCheckbox("##done", &n->done)) notes_save();
        igSameLine(0, -1);
        char l[260];
        snprintf(l, sizeof l, "%s: %s", n->actor, n->text);
        if (n->done) igPushStyleColor_Vec4(ImGuiCol_Text, THV(TK_TEXT_DISABLED));
        if (igSelectable_Bool(l, false, 0, (ImVec2){ igGetContentRegionAvail().x - 30.0f, 0 })) {
            Entity e = actor_named(&ed->world, n->actor);
            if (e) editor_select(ed, e, SEL_REPLACE), viewport_focus(ed, e);
        }
        if (n->done) igPopStyleColor(1);
        igSameLine(0, -1);
        if (igSmallButton("x")) del = i;
        igPopID();
        shown++;
    }
    if (!shown) igTextDisabled("%s", TR("No notes in this level."));
    if (del >= 0) memmove(&g_notes[del], &g_notes[del + 1], sizeof(Note) * (size_t)(g_nnotes - del - 1)), g_nnotes--, notes_save();
}
static void notes_overlay(Editor* ed, struct ImDrawList* dl) {
    if (!g_note_overlay || ed->playing) return;
    if (g_nnotes < 0) notes_load();
    for (int i = 0; i < g_nnotes; i++) {
        const Note* n = &g_notes[i];
        if (n->done || strcmp(n->level, level_key(ed))) continue;
        Entity e = actor_named(&ed->world, n->actor);
        const CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
        if (t) kit_label(dl, ed, (float[3]){ t->location[0], t->location[1], t->location[2] + 60.0f }, TH(TK_AMBER_L), n->text);
    }
}
static void notes_off(Editor* ed) {
    (void)ed;
    kit_free(g_notes, sizeof(Note) * NOTE_MAX);
    g_notes = NULL, g_nnotes = -1;
}
static bool notes_test(Editor* ed, char* msg, size_t cap) {
    char path[900], bak[920];
    notes_path(path, sizeof path);
    snprintf(bak, sizeof bak, "%s.kitbak", path);
    rename(path, bak); // (the project's notes kept aside)
    Entity e = kit_spawn_test_cube(ed, "NoteCrate", 0, 0, 0);
    notes_load();
    bool added = note_add(ed, e, "fix the lid's UVs");
    notes_off(ed);
    notes_load(); // (read back)
    bool read = g_nnotes == 1 && !strcmp(g_notes[0].actor, "NoteCrate") && !strcmp(g_notes[0].text, "fix the lid's UVs");
    snprintf(msg, cap, "a note on NoteCrate saved and read back");
    notes_off(ed);
    remove(path);
    rename(bak, path);
    kit_destroy(ed, e);
    return added && read;
}

const KitPlugin kit_actor_notes = { "ActorNotes", "Actor Notes", "Productivity", "COMMENT",
    "TODO notes on actors, shown over them in the viewport and listed with a done box; kept in Config/ActorNotes.txt so the team sees them through Git.",
    false, notes_window, notes_overlay, NULL, notes_off, notes_test };

// ---------------------------------------------------------------------------------------------------------------
// Selection Sets: the selection kept by name (the actors' names, per level; Saved/SelectionSets.txt) and picked again
#define SS_MAX 24
#define SS_ACTORS 256
typedef struct SelSet { char name[40]; uint32_t n; char actor[SS_ACTORS][RE_NAME_MAX]; } SelSet;
static SelSet* g_ss; // (SS_MAX once used)
static int g_nss;
static char g_ss_name[40] = "Set";

static bool ss_keep(Editor* ed, const char* name) {
    if (!g_ss && !(g_ss = kit_alloc(sizeof(SelSet) * SS_MAX))) return false;
    if (g_nss >= SS_MAX || !ed->nsel) return false;
    SelSet* s = &g_ss[g_nss++];
    snprintf(s->name, sizeof s->name, "%s", name[0] ? name : "Set");
    s->n = 0;
    for (uint32_t i = 0; i < ed->nsel && s->n < SS_ACTORS; i++) snprintf(s->actor[s->n++], RE_NAME_MAX, "%s", kit_actor_name(&ed->world, ed->sel[i]));
    return true;
}
static uint32_t ss_pick(Editor* ed, int i, bool add) {
    if (!add) editor_select_none(ed);
    uint32_t n = 0;
    for (uint32_t k = 0; k < g_ss[i].n; k++) {
        Entity e = actor_named(&ed->world, g_ss[i].actor[k]);
        if (e) editor_select(ed, e, SEL_ADD), n++;
    }
    return n;
}
static void ss_window(Editor* ed, double now) {
    igSetNextItemWidth(-130);
    igInputText("##ssname", g_ss_name, sizeof g_ss_name, 0, NULL, NULL);
    igSameLine(0, -1);
    igBeginDisabled(!ed->nsel || g_nss >= SS_MAX);
    if (igButton(TR("Keep Selection"), (ImVec2){ -1, 0 })) ss_keep(ed, g_ss_name);
    igEndDisabled();
    int del = -1;
    for (int i = 0; i < g_nss; i++) {
        igPushID_Int(i);
        char l[80];
        snprintf(l, sizeof l, "%s  (%u)", g_ss[i].name, g_ss[i].n);
        if (igSelectable_Bool(l, false, 0, (ImVec2){ igGetContentRegionAvail().x - 90.0f, 0 }))
            editor_status(ed, now, TR("%u actors selected."), ss_pick(ed, i, igGetIO_Nil()->KeyShift));
        igSameLine(0, -1);
        if (igSmallButton(TR("Add"))) ss_pick(ed, i, true);
        igSameLine(0, -1);
        if (igSmallButton("x")) del = i;
        igPopID();
    }
    if (!g_nss) igTextDisabled("%s", TR("Select actors and keep them as a set; click a set to select them again (Shift: add)."));
    if (del >= 0) memmove(&g_ss[del], &g_ss[del + 1], sizeof(SelSet) * (size_t)(g_nss - del - 1)), g_nss--;
}
static void ss_off(Editor* ed) {
    (void)ed;
    kit_free(g_ss, sizeof(SelSet) * SS_MAX);
    g_ss = NULL, g_nss = 0;
}
static bool ss_test(Editor* ed, char* msg, size_t cap) {
    Entity a = kit_spawn_test_cube(ed, "SetA", 0, 0, 0), b = kit_spawn_test_cube(ed, "SetB", 100, 0, 0), c = kit_spawn_test_cube(ed, "SetC", 200, 0, 0);
    editor_select(ed, a, SEL_REPLACE), editor_select(ed, c, SEL_ADD);
    int keep_n = g_nss;
    bool kept = ss_keep(ed, "Corners");
    editor_select(ed, b, SEL_REPLACE);
    uint32_t n = kept ? ss_pick(ed, g_nss - 1, false) : 0;
    bool ok = n == 2 && editor_is_selected(ed, a) && editor_is_selected(ed, c) && !editor_is_selected(ed, b);
    snprintf(msg, cap, "a set of 2 kept and picked again");
    g_nss = keep_n;
    kit_destroy(ed, a), kit_destroy(ed, b), kit_destroy(ed, c);
    return ok;
}

const KitPlugin kit_selection_sets = { "SelectionSets", "Selection Sets", "Productivity", "GROUP",
    "Keeps the current selection under a name (the lights of a room, a set of props) and selects it again in one click; Shift adds it to the selection.",
    true, ss_window, NULL, NULL, ss_off, ss_test };

// ---------------------------------------------------------------------------------------------------------------
// Command Palette (Ctrl+Shift+P): every command by its name: the editor's actions, the plugins' windows, placing a class
typedef struct Cmd { char label[96]; int kind, arg; } Cmd;
enum { CMD_SAVE, CMD_PLAY, CMD_STOP, CMD_FOCUS, CMD_ALL, CMD_NONE, CMD_UNDO, CMD_REDO, CMD_GRID, CMD_GAME, CMD_ICONS, CMD_OUTLINER, CMD_DETAILS, CMD_EDITOR_N,
       CMD_PLUGIN = 100, CMD_PLACE = 200 };
static const char* const g_cmd_names[CMD_EDITOR_N] = { "Save Level", "Play", "Stop", "Focus Selected", "Select All", "Select None", "Undo", "Redo",
                                                       "Toggle Grid", "Toggle Game View", "Toggle Icons", "Toggle Outliner", "Toggle Details" };
static bool g_pal_open;
static char g_pal_find[64];
static int g_pal_sel;

static uint32_t cmd_list(Editor* ed, const char* find, Cmd* out, uint32_t cap) {
    (void)ed;
    uint32_t n = 0;
    for (int i = 0; i < CMD_EDITOR_N && n < cap; i++)
        if (ci_has(g_cmd_names[i], find) || ci_has(TR(g_cmd_names[i]), find)) snprintf(out[n].label, sizeof out[n].label, "%s", TR(g_cmd_names[i])), out[n].kind = i, out[n++].arg = 0;
    for (uint32_t i = 0; i < kit_count() && n < cap; i++) {
        const KitPlugin* k = kit_at(i);
        if (!kit_on(i) || !k->window || (!ci_has(k->friendly, find) && !ci_has(TR(k->friendly), find))) continue;
        snprintf(out[n].label, sizeof out[n].label, "%s: %s", TR("Plugin"), TR(k->friendly)), out[n].kind = CMD_PLUGIN, out[n++].arg = (int)i;
    }
    for (int i = 0; i < editor_place_count() && n < cap && find[0]; i++) {
        const char* cat;
        const char* nm = editor_place_name(i, &cat);
        if (!ci_has(nm, find) && !ci_has(TR(nm), find)) continue;
        snprintf(out[n].label, sizeof out[n].label, "%s: %s", TR("Place"), TR(nm)), out[n].kind = CMD_PLACE, out[n++].arg = i;
    }
    return n;
}
static void cmd_run(Editor* ed, const Cmd* c, double now) {
    switch (c->kind) {
    case CMD_SAVE: editor_save_level(ed, ed->level_path, now); break;
    case CMD_PLAY: if (!ed->playing) editor_play(ed, now); break;
    case CMD_STOP: if (ed->playing) editor_stop(ed, now); break;
    case CMD_FOCUS: if (ed->selected) viewport_focus(ed, ed->selected); break;
    case CMD_ALL: editor_select_all(ed); break;
    case CMD_NONE: editor_select_none(ed); break;
    case CMD_UNDO: editor_undo(ed, now); break;
    case CMD_REDO: editor_redo(ed, now); break;
    case CMD_GRID: ed->show_grid = !ed->show_grid; break;
    case CMD_GAME: ed->game_view = !ed->game_view; break;
    case CMD_ICONS: ed->show_icons = !ed->show_icons; break;
    case CMD_OUTLINER: ed->show_outliner = !ed->show_outliner; break;
    case CMD_DETAILS: ed->show_details = !ed->show_details; break;
    case CMD_PLUGIN: kit_open((uint32_t)c->arg, true); break;
    case CMD_PLACE: editor_place_spawn(ed, c->arg, now); break;
    }
}
static void palette_tick(Editor* ed, double dt) {
    (void)dt;
    if (igIsKeyChordPressed_Nil(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P)) g_pal_open = true, g_pal_find[0] = 0, g_pal_sel = 0;
    if (!g_pal_open) return;
    const ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowPos((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 80.0f }, ImGuiCond_Always, (ImVec2){ 0.5f, 0.0f });
    igSetNextWindowSize((ImVec2){ 560, 0 }, ImGuiCond_Always);
    if (!igBegin("###kitpalette", &g_pal_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
        igEnd();
        return;
    }
    if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
    igSetNextItemWidth(-1);
    igInputTextWithHint("##pal", TR("Type a command (Esc closes)"), g_pal_find, sizeof g_pal_find, 0, NULL, NULL);
    static Cmd cmds[48];
    uint32_t n = cmd_list(ed, g_pal_find, cmds, 48);
    if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true)) g_pal_sel++;
    if (igIsKeyPressed_Bool(ImGuiKey_UpArrow, true)) g_pal_sel--;
    g_pal_sel = n ? (g_pal_sel < 0 ? 0 : g_pal_sel >= (int)n ? (int)n - 1 : g_pal_sel) : 0;
    bool run = igIsKeyPressed_Bool(ImGuiKey_Enter, false) && n;
    for (uint32_t i = 0; i < n && i < 14; i++)
        if (igSelectable_Bool(cmds[i].label, (int)i == g_pal_sel, 0, (ImVec2){ 0, 0 })) g_pal_sel = (int)i, run = true;
    if (!n) igTextDisabled("%s", TR("No command matches."));
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) g_pal_open = false;
    igEnd();
    if (run) g_pal_open = false, cmd_run(ed, &cmds[g_pal_sel], re_time_seconds());
}
static void palette_window(Editor* ed, double now) {
    (void)ed, (void)now;
    igTextWrapped("%s", TR("Ctrl+Shift+P opens the palette anywhere: type a part of a command's name, arrows to pick, Enter to run. It finds the editor's actions, the plugins' windows and every class to place."));
    if (igButton(TR("Open the Palette"), (ImVec2){ -1, 0 })) g_pal_open = true, g_pal_find[0] = 0;
}
static void palette_off(Editor* ed) { (void)ed, g_pal_open = false; }
static bool palette_test(Editor* ed, char* msg, size_t cap) {
    Cmd c[48];
    uint32_t n = cmd_list(ed, "grid", c, 48);
    bool found = false;
    for (uint32_t i = 0; i < n; i++)
        if (c[i].kind == CMD_GRID) {
            bool g = ed->show_grid;
            cmd_run(ed, &c[i], 0.0);
            found = ed->show_grid != g;
            ed->show_grid = g;
        }
    uint32_t nl = cmd_list(ed, "point light", c, 48);
    bool place = false;
    for (uint32_t i = 0; i < nl; i++) place |= c[i].kind == CMD_PLACE;
    snprintf(msg, cap, "\"grid\": %u commands (Toggle Grid ran), \"point light\": %u (a class to place)", n, nl);
    return found && place;
}

const KitPlugin kit_command_palette = { "CommandPalette", "Command Palette", "Productivity", "SEARCH",
    "Ctrl+Shift+P anywhere: type a part of a name and run it: save, play, focus, the views, any plugin's window, any class to place. Arrows and Enter.",
    true, palette_window, NULL, palette_tick, palette_off, palette_test };

// ---------------------------------------------------------------------------------------------------------------
// The references between the project's files (shared by Asset References and Unused Assets): a worker reads every file
// of Content once and finds in it the other assets' hashes (what the components and the binary assets keep) and names
// (what the text assets and the levels' paths keep): an edge from that file to each asset found
typedef struct RefIndex {
    uint32_t nassets;
    KitAsset* assets;   // the snapshot scanned
    uint32_t* edges;    // pairs (from, to): asset indices
    uint32_t nedges, cap;
    double ms;
    uint64_t bytes;
} RefIndex;
static RefIndex* g_refs;     // the last scan (kit memory)
static uint32_t g_refs_task;  // the scan running
static uint8_t g_refs_owners; // 1 Asset References, 2 Unused Assets: freed when neither is on

static uint32_t fnv_lower(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)tolower((unsigned char)s[i])) * 16777619u;
    return h;
}
static bool tok_char(uint8_t c) { return isalnum(c) || c == '_' || c == '-'; }

typedef struct { uint32_t key; uint32_t asset; } Slot;
static uint32_t slot_find(const Slot* t, uint32_t mask, uint32_t key) {
    for (uint32_t i = (key * 2654435761u) & mask;; i = (i + 1) & mask) {
        if (!t[i].key) return UINT32_MAX;
        if (t[i].key == key) return t[i].asset;
    }
}
static void slot_put(Slot* t, uint32_t mask, uint32_t key, uint32_t asset) {
    if (!key) return;
    for (uint32_t i = (key * 2654435761u) & mask;; i = (i + 1) & mask) {
        if (!t[i].key) { t[i].key = key, t[i].asset = asset; return; }
        if (t[i].key == key) { t[i].asset = (UINT32_MAX - 1u); return; } // (two assets of one key: neither counted by it)
    }
}

// (any thread) the scan of a snapshot into r (its own memory: malloc, adopted by the main thread)
static void refs_scan(RefIndex* r, AsyncTask* t) {
    double t0 = re_time_seconds();
    uint32_t na = r->nassets, cap = 1;
    while (cap < na * 4 + 16) cap <<= 1;
    Slot* by_hash = calloc(cap, sizeof(Slot));
    Slot* by_name = calloc(cap, sizeof(Slot));
    uint32_t* seen = calloc(na ? na : 1, sizeof(uint32_t)); // (the file that last found each: one edge per pair)
    if (!by_hash || !by_name || !seen) {
        free(by_hash), free(by_name), free(seen);
        return;
    }
    for (uint32_t i = 0; i < na; i++) {
        slot_put(by_hash, cap - 1, r->assets[i].hash, i);
        const char* sl = strrchr(r->assets[i].vpath, '/'); // (the name without folder and extension: no static buffer on a worker)
        const char* nm = sl ? sl + 1 : r->assets[i].vpath;
        const char* dot = strrchr(nm, '.');
        size_t nl = dot && dot != nm ? (size_t)(dot - nm) : strlen(nm);
        if (nl >= 3) slot_put(by_name, cap - 1, fnv_lower(nm, nl), i);
    }
    const char* root = content_root();
    for (uint32_t f = 0; f < na; f++) {
        if (t && async_cancelled(t)) break;
        if (t && !(f & 31)) async_progress_n(t, (int)f, (int)na);
        const KitAsset* a = &r->assets[f];
        if (a->size > (64ull << 20) || a->kind == ASSET_TEXTURE_SOURCE || a->kind == ASSET_SOUND_SOURCE) continue; // (pictures and sounds name nothing)
        char disk[1024];
        snprintf(disk, sizeof disk, "%s%s", root, a->vpath + 8); // ("/Content" -> the folder)
        FILE* fp = re_fopen(disk, "rb");
        if (!fp) continue;
        uint8_t* buf = malloc((size_t)a->size + 4);
        size_t n = buf ? fread(buf, 1, (size_t)a->size, fp) : 0;
        fclose(fp);
        if (!buf) continue;
        r->bytes += n;
        uint32_t stamp = f + 1;
        for (size_t i = 0; i + 4 <= n || i < n; i++) {
            uint32_t to = UINT32_MAX;
            if (i + 4 <= n) {
                uint32_t v = (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24;
                if (v) to = slot_find(by_hash, cap - 1, v);
            }
            if (to >= na && tok_char(buf[i]) && (i == 0 || !tok_char(buf[i - 1]))) { // a word starting here
                size_t j = i;
                while (j < n && tok_char(buf[j])) j++;
                if (j - i >= 3) to = slot_find(by_name, cap - 1, fnv_lower((const char*)buf + i, j - i));
            }
            if (to >= na || to == f || seen[to] == stamp) continue;
            seen[to] = stamp;
            if (r->nedges == r->cap) {
                uint32_t nc = r->cap ? r->cap * 2 : 1024;
                uint32_t* e = realloc(r->edges, sizeof(uint32_t) * 2 * nc);
                if (!e) break;
                r->edges = e, r->cap = nc;
            }
            r->edges[r->nedges * 2] = f, r->edges[r->nedges * 2 + 1] = to, r->nedges++;
        }
        free(buf);
    }
    free(by_hash), free(by_name), free(seen);
    r->ms = (re_time_seconds() - t0) * 1000.0;
}
static size_t refs_bytes(const RefIndex* r) { return sizeof *r + sizeof(KitAsset) * r->nassets + sizeof(uint32_t) * 2 * r->cap; }
static void refs_free(void) {
    if (!g_refs) return;
    size_t b = refs_bytes(g_refs);
    free(g_refs->assets), free(g_refs->edges), free(g_refs);
    kit_account(-(long long)b);
    g_refs = NULL;
}
static RefIndex* refs_snapshot(void) {
    const KitAsset* as;
    uint32_t na = kit_assets(&as, true);
    RefIndex* r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->assets = malloc(sizeof(KitAsset) * (na ? na : 1));
    if (!r->assets) return free(r), NULL;
    memcpy(r->assets, as, sizeof(KitAsset) * na);
    r->nassets = na;
    return r;
}
static void refs_work(AsyncTask* t, void* arg) { refs_scan(arg, t); }
static void refs_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    RefIndex* r = arg;
    g_refs_task = 0;
    if (!ok || !g_refs_owners) {
        free(r->assets), free(r->edges), free(r);
        return;
    }
    refs_free();
    g_refs = r;
    kit_account((long long)refs_bytes(r));
}
static void refs_start(uint8_t owner) {
    g_refs_owners |= owner;
    if (g_refs_task) return;
    RefIndex* r = refs_snapshot();
    if (!r) return;
    g_refs_task = async_run(TR("Finding the assets' references"), refs_work, refs_done, r, ASYNC_CANCELLABLE | ASYNC_QUIET);
}
static void refs_release(uint8_t owner) {
    g_refs_owners &= (uint8_t)~owner;
    if (!g_refs_owners) {
        if (g_refs_task) async_cancel(g_refs_task);
        refs_free();
    }
}
// the actors of the open level holding a hash in one of their components
static uint32_t level_users(Editor* ed, uint32_t hash, Entity* out, uint32_t cap) {
    World* w = &ed->world;
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count && hash; s++) {
        Entity e = ecs_entity_at(w, s);
        uint64_t m = e ? ecs_mask(w, e) : 0;
        bool has = false;
        for (int c = 0; c < COMP_COUNT && m && !has; c++) {
            if (!(m & COMP_BIT(c))) continue;
            const uint8_t* p = ecs_get(w, e, (ComponentId)c);
            for (uint32_t k = 0; p && k + 4 <= g_components[c].size && !has; k += 4) has = !memcmp(p + k, &hash, 4);
        }
        if (has) {
            if (n < cap && out) out[n] = e;
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Asset References: what uses an asset (the other assets, the open level's actors) and what it uses
static char g_ar_find[64];
static int g_ar_pick = -1; // an index in the scan's snapshot

static void ar_window(Editor* ed, double now) {
    (void)now;
    if (!g_refs && !g_refs_task) refs_start(1);
    igSetNextItemWidth(-110);
    igInputTextWithHint("##arfind", TR("Search an asset"), g_ar_find, sizeof g_ar_find, 0, NULL, NULL);
    igSameLine(0, -1);
    igBeginDisabled(g_refs_task != 0);
    if (igButton(g_refs_task ? TR("Reading...") : TR("Read Again"), (ImVec2){ -1, 0 })) refs_start(1);
    igEndDisabled();
    if (!g_refs) {
        igTextDisabled("%s", TR("Reading the project's files..."));
        return;
    }
    igTextDisabled(TR("%u assets, %u references (%.0f ms, %.1f MB read)"), g_refs->nassets, g_refs->nedges, g_refs->ms, (double)g_refs->bytes / (1024.0 * 1024.0));
    if (igBeginChild_Str("arlist", (ImVec2){ 0, 150 }, ImGuiChildFlags_Borders, 0)) {
        uint32_t shown = 0;
        for (uint32_t i = 0; i < g_refs->nassets && shown < 300; i++) {
            const KitAsset* a = &g_refs->assets[i];
            if (!ci_has(a->vpath, g_ar_find)) continue;
            igPushID_Int((int)i);
            if (igSelectable_Bool(a->vpath + 9, g_ar_pick == (int)i, 0, (ImVec2){ 0, 0 })) g_ar_pick = (int)i;
            igPopID();
            shown++;
        }
    }
    igEndChild();
    if (g_ar_pick < 0 || (uint32_t)g_ar_pick >= g_refs->nassets) return;
    const KitAsset* a = &g_refs->assets[g_ar_pick];
    igSeparatorText(kit_file_name(a->vpath));
    uint32_t users = 0, uses = 0;
    if (igBeginChild_Str("arrefs", (ImVec2){ 0, 0 }, 0, 0)) {
        igTextColored(THV(TK_ACCENT), "%s", TR("Used by"));
        for (uint32_t k = 0; k < g_refs->nedges; k++)
            if (g_refs->edges[k * 2 + 1] == (uint32_t)g_ar_pick) {
                const char* vp = g_refs->assets[g_refs->edges[k * 2]].vpath;
                igPushID_Int((int)k);
                if (igSelectable_Bool(vp + 9, false, 0, (ImVec2){ 0, 0 })) content_browse_to(vp);
                igPopID();
                users++;
            }
        Entity acts[64];
        uint32_t na = level_users(ed, a->hash, acts, 64);
        for (uint32_t k = 0; k < na && k < 64; k++) {
            igPushID_Int((int)(100000 + k));
            char l[120];
            snprintf(l, sizeof l, "%s (%s)", kit_actor_name(&ed->world, acts[k]), TR("actor in this level"));
            if (igSelectable_Bool(l, false, 0, (ImVec2){ 0, 0 })) editor_select(ed, acts[k], SEL_REPLACE), viewport_focus(ed, acts[k]);
            igPopID();
        }
        if (!users && !na) igTextDisabled("%s", TR("Nothing uses it."));
        igTextColored(THV(TK_ACCENT2), "%s", TR("Uses"));
        for (uint32_t k = 0; k < g_refs->nedges; k++)
            if (g_refs->edges[k * 2] == (uint32_t)g_ar_pick) igBulletText("%s", g_refs->assets[g_refs->edges[k * 2 + 1]].vpath + 9), uses++;
        if (!uses) igTextDisabled("%s", TR("It uses no other asset."));
    }
    igEndChild();
}
static void ar_off(Editor* ed) { (void)ed, g_ar_pick = -1, g_ar_find[0] = 0, refs_release(1); }

// (the self-tests' small project) Content/KitRefTest: a material, a mesh pointing at it by hash, a Blueprint naming the
// mesh, and a sound nothing uses
static bool refs_fixture(char* dir, size_t cap) {
    snprintf(dir, cap, "%s/KitRefTest", content_root());
    re_remove_tree(dir);
    if (!re_make_dirs(dir)) return false;
    char p[1100];
    uint32_t mat = bp_vpath_hash("/Content/KitRefTest/M_KitRock.rmat");
    FILE* f;
    snprintf(p, sizeof p, "%s/M_KitRock.rmat", dir);
    if (!(f = re_fopen(p, "wb"))) return false;
    fputs("{ \"nodes\": [] }\n", f), fclose(f);
    snprintf(p, sizeof p, "%s/SM_KitRock.rmesh", dir);
    if (!(f = re_fopen(p, "wb"))) return false;
    uint8_t bin[64] = { 'R', 'M', 'S', 'H' };
    memcpy(bin + 20, &mat, 4);
    fwrite(bin, 1, sizeof bin, f), fclose(f);
    snprintf(p, sizeof p, "%s/BP_KitRock.rbp", dir);
    if (!(f = re_fopen(p, "wb"))) return false;
    fputs("{ \"components\": [ { \"mesh\": \"/Content/KitRefTest/SM_KitRock.rmesh\" } ] }\n", f), fclose(f);
    snprintf(p, sizeof p, "%s/S_KitUnused.rsound", dir);
    if (!(f = re_fopen(p, "wb"))) return false;
    fwrite(bin, 1, 16, f), fclose(f);
    return true;
}
static int refs_find(const RefIndex* r, const char* vpath) {
    for (uint32_t i = 0; i < r->nassets; i++)
        if (!strcmp(r->assets[i].vpath, vpath)) return (int)i;
    return -1;
}
static bool refs_edge(const RefIndex* r, int from, int to) {
    for (uint32_t k = 0; k < r->nedges; k++)
        if ((int)r->edges[k * 2] == from && (int)r->edges[k * 2 + 1] == to) return true;
    return false;
}
static bool ar_test(Editor* ed, char* msg, size_t cap) {
    (void)ed;
    char dir[900];
    if (!refs_fixture(dir, sizeof dir)) return false;
    RefIndex* r = refs_snapshot();
    if (!r) return false;
    refs_scan(r, NULL);
    int mat = refs_find(r, "/Content/KitRefTest/M_KitRock.rmat"), mesh = refs_find(r, "/Content/KitRefTest/SM_KitRock.rmesh");
    int bp = refs_find(r, "/Content/KitRefTest/BP_KitRock.rbp");
    bool by_hash = refs_edge(r, mesh, mat), by_name = refs_edge(r, bp, mesh), back = !refs_edge(r, mat, mesh);
    snprintf(msg, cap, "%u files read in %.1f ms: the mesh uses the material (by hash) %s, the Blueprint the mesh (by path) %s", r->nassets, r->ms,
             by_hash ? "yes" : "NO", by_name ? "yes" : "NO");
    free(r->assets), free(r->edges), free(r);
    re_remove_tree(dir);
    kit_assets(NULL, true);
    return mat >= 0 && by_hash && by_name && back;
}

const KitPlugin kit_asset_usage = { "AssetReferences", "Asset References", "Productivity", "LINK",
    "Who uses an asset and what it uses (Unreal's Reference Viewer as a list): the other assets that point at it, the open level's actors that hold it. Read in the background.",
    false, ar_window, NULL, NULL, ar_off, ar_test };

// ---------------------------------------------------------------------------------------------------------------
// Unused Assets: the assets nothing points at (no other asset, no saved level, not the open level); deleted only after
// a confirmation, to the Recycle Bin
static bool unused_kind(int kind) {
    return kind != ASSET_LEVEL && kind != ASSET_FOLDER && kind != ASSET_OTHER && kind != ASSET_BASIC_SHAPE;
}
static uint32_t unused_list(Editor* ed, const RefIndex* r, uint8_t* is_unused) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->nassets; i++) is_unused[i] = unused_kind(r->assets[i].kind);
    for (uint32_t k = 0; k < r->nedges; k++) is_unused[r->edges[k * 2 + 1]] = 0;
    for (uint32_t i = 0; i < r->nassets; i++)
        if (is_unused[i] && ed && level_users(ed, r->assets[i].hash, NULL, 0)) is_unused[i] = 0;
    for (uint32_t i = 0; i < r->nassets; i++) n += is_unused[i];
    return n;
}
static uint8_t* g_un;      // per asset of the scan: unused (kit memory)
static uint8_t* g_un_pick; // ticked for deletion
static uint32_t g_un_n, g_un_for; // the scan they belong to (its asset count)
static const RefIndex* g_un_scan;
static void un_free(void) {
    kit_free(g_un, g_un_for), kit_free(g_un_pick, g_un_for);
    g_un = g_un_pick = NULL, g_un_for = 0, g_un_scan = NULL;
}
static void un_window(Editor* ed, double now) {
    if (!g_refs && !g_refs_task) refs_start(2);
    igTextWrapped("%s", TR("The assets nothing points at: no other asset, no level, not the open level. Check before deleting: an asset loaded only by name from a script or a Data Table shows here too."));
    igBeginDisabled(g_refs_task != 0);
    if (igButton(g_refs_task ? TR("Reading...") : TR("Read Again"), (ImVec2){ 0, 0 })) refs_start(2), un_free();
    igEndDisabled();
    if (!g_refs) return;
    if (g_un_scan != g_refs) {
        un_free();
        g_un_for = g_refs->nassets ? g_refs->nassets : 1;
        g_un = kit_alloc(g_un_for), g_un_pick = kit_alloc(g_un_for);
        g_un_scan = g_refs;
        if (g_un) g_un_n = unused_list(ed, g_refs, g_un);
    }
    if (!g_un || !g_un_pick) return;
    uint32_t picked = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < g_refs->nassets; i++)
        if (g_un[i] && g_un_pick[i]) picked++, bytes += g_refs->assets[i].size;
    igText(TR("%u unused assets"), g_un_n);
    igSameLine(0, -1);
    if (igSmallButton(TR("Tick All"))) memcpy(g_un_pick, g_un, g_refs->nassets);
    igSameLine(0, -1);
    if (igSmallButton(TR("Tick None"))) memset(g_un_pick, 0, g_refs->nassets);
    if (igBeginChild_Str("unlist", (ImVec2){ 0, -igGetFrameHeightWithSpacing() * 1.4f }, ImGuiChildFlags_Borders, 0)) {
        for (uint32_t i = 0; i < g_refs->nassets; i++) {
            if (!g_un[i]) continue;
            igPushID_Int((int)i);
            bool t = g_un_pick[i] != 0;
            if (igCheckbox("##t", &t)) g_un_pick[i] = t;
            igSameLine(0, -1);
            if (igSelectable_Bool(g_refs->assets[i].vpath + 9, false, 0, (ImVec2){ 0, 0 })) content_browse_to(g_refs->assets[i].vpath);
            igSameLine(igGetContentRegionAvail().x - 40.0f, -1);
            igTextDisabled("%.0f KB", (double)g_refs->assets[i].size / 1024.0);
            igPopID();
        }
    }
    igEndChild();
    igBeginDisabled(!picked);
    char l[120];
    snprintf(l, sizeof l, TR("Delete %u Ticked (%.1f MB)..."), picked, (double)bytes / (1024.0 * 1024.0));
    if (igButton(l, (ImVec2){ -1, 0 })) igOpenPopup_Str(TR("Delete Unused Assets?"), 0);
    igEndDisabled();
    if (igBeginPopupModal(TR("Delete Unused Assets?"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        igText(TR("%u assets go to the Recycle Bin (it can bring them back)."), picked);
        igTextColored(THV(TK_WARN), "%s", TR("An asset a script loads by its name isn't seen as used: check the list."));
        if (igButton(TR("Delete"), (ImVec2){ 120, 0 })) {
            uint32_t done = 0;
            for (uint32_t i = 0; i < g_refs->nassets; i++) {
                if (!g_un[i] || !g_un_pick[i]) continue;
                char disk[1024];
                snprintf(disk, sizeof disk, "%s%s", content_root(), g_refs->assets[i].vpath + 8);
                if (re_trash_path(disk)) done++, g_un[i] = g_un_pick[i] = 0;
            }
            content_mark_dirty();
            editor_status(ed, now, TR("%u unused assets sent to the Recycle Bin."), done);
            g_un_n -= done;
            igCloseCurrentPopup();
        }
        igSameLine(0, -1);
        if (igButton(TR("Cancel"), (ImVec2){ 120, 0 })) igCloseCurrentPopup();
        igEndPopup();
    }
}
static void un_off(Editor* ed) { (void)ed, un_free(), refs_release(2); }
static bool un_test(Editor* ed, char* msg, size_t cap) {
    char dir[900];
    if (!refs_fixture(dir, sizeof dir)) return false;
    RefIndex* r = refs_snapshot();
    if (!r) return false;
    refs_scan(r, NULL);
    uint8_t* un = calloc(r->nassets ? r->nassets : 1, 1);
    if (!un) return false;
    unused_list(ed, r, un);
    int snd = refs_find(r, "/Content/KitRefTest/S_KitUnused.rsound"), mat = refs_find(r, "/Content/KitRefTest/M_KitRock.rmat");
    int mesh = refs_find(r, "/Content/KitRefTest/SM_KitRock.rmesh");
    bool ok = snd >= 0 && un[snd] && mat >= 0 && !un[mat] && mesh >= 0 && !un[mesh];
    snprintf(msg, cap, "the sound nothing uses listed, the material and the mesh in use kept out (nothing deleted)");
    free(un), free(r->assets), free(r->edges), free(r);
    re_remove_tree(dir);
    kit_assets(NULL, true);
    return ok;
}

const KitPlugin kit_unused_assets = { "UnusedAssets", "Unused Assets", "Productivity", "DELETE",
    "Lists the assets nothing points at (no asset, no level, not the open level), with their size; the ticked ones go to the Recycle Bin only after you confirm.",
    false, un_window, NULL, NULL, un_off, un_test };
