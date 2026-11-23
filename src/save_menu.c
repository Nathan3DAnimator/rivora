// WBP_SaveSlots (save_progress.h, notas-dev/salvar-progresso.md): the Widget Blueprint the engine makes in Content/UI when
// it is missing, through the editor's own API (a UMG tree and an Event Graph of ordinary nodes, so a project copies and
// changes it like any Blueprint). Its widgets are bound to the slots by name (UiWidget.bind_setting "Save.*", read each
// time the list changes; a click on a bound Button does its action):
//   Save.Title, Save.Status, Save.Empty (shown without slots), Save.Continue, Save.NewGame, Save.New (Save mode only),
//   Save.Back; per row N (0 the newest): Save.N.Row (collapsed without a slot), Save.N.Thumb (its picture), Save.N.Name,
//   Save.N.Info ("Level  -  2026-11-08 21:45  -  1:02:03"), Save.N.Select (Load / Overwrite), Save.N.Action (its label),
//   Save.N.Delete
// The pause menu (pause_menu.c) gets the Save Game and Load Game buttons that open it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "game_settings.h"
#include "i18n.h"
#include "level.h"
#include "platform_files.h"
#include "project.h"
#include "save_progress.h"
#include "ui_widget.h"

#define SLOTS_VPATH "/Content/UI/WBP_SaveSlots.rbp"
#define PAUSE_VPATH "/Content/UI/WBP_PauseMenu.rbp"
#define ROWS 8

static struct {
    Entity menu;
    bool save_mode, mouse_before;
    SaveSlotInfo list[ROWS];
    uint32_t n, gen_seen;
    char status[96];
    uint32_t status_gen; // (each message: the bound Status text read again)
} M;

// --- Making the widget (pause_menu.c's way) ---

static bool g_bad;
static int W(UiTree* t, uint8_t kind, int parent, const char* name) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_bad = true, 0;
    if (name) snprintf(t->w[i].name, UI_NAME, "%s", name);
    return i;
}
static void rgba(float* c, float r, float g, float b, float a) { c[0] = r, c[1] = g, c[2] = b, c[3] = a; }
static int text(UiTree* t, int parent, const char* name, const char* s, float font, float jx, const char* bind) {
    int i = W(t, UIW_TEXT, parent, name);
    UiWidget* w = &t->w[i];
    snprintf(w->text, sizeof w->text, "%s", s);
    w->font_size = font, w->align[0] = jx, w->align[1] = 0.5f, w->size[1] = font * 1.6f;
    if (bind) snprintf(w->bind_setting, UI_NAME, "%s", bind);
    return i;
}
static int button(UiTree* t, int parent, const char* name, const char* label, float wd, float h, float font, const char* bind, const char* label_bind) {
    int b = W(t, UIW_BUTTON, parent, name);
    t->w[b].size[0] = wd, t->w[b].size[1] = h, t->w[b].radius = 8.0f;
    rgba(t->w[b].hover, 0.20f, 0.42f, 0.78f, 0.95f);
    rgba(t->w[b].press, 0.12f, 0.30f, 0.62f, 1.0f);
    if (bind) snprintf(t->w[b].bind_setting, UI_NAME, "%s", bind);
    char tn[UI_NAME];
    snprintf(tn, sizeof tn, "%sLabel", name);
    int l = text(t, b, tn, label, font, 0.5f, label_bind);
    t->w[l].bold = 1;
    return b;
}
static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    g_bad = true;
    return 0;
}
static uint32_t N(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* txt) {
    BpNode* n = bp_add_node(a, g, kind, x * 320.0f, y * 160.0f, txt, NULL);
    if (!n) g_bad = true;
    return n ? n->id : 0;
}
static void L(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) g_bad = true;
}

static BpAsset* make_slots(void) {
    BpAsset* a = bp_create(BPA_CLASS, "/Content/UI", "WBP_SaveSlots", "User Widget");
    if (!a || !(a->ui = calloc(1, sizeof(UiTree)))) return NULL; // (an edit, not a frame)
    UiTree* t = a->ui;
    ui_tree_default(t);
    g_bad = false;
    int dim = W(t, UIW_BORDER, 0, "Dim");
    UiWidget* d = &t->w[dim];
    d->anchor[0] = d->anchor[1] = 0.0f, d->anchor[2] = d->anchor[3] = 1.0f, d->radius = 0.0f;
    rgba(d->bg, 0, 0, 0, 0.6f);
    int panel = W(t, UIW_BORDER, 0, "Panel");
    UiWidget* p = &t->w[panel];
    p->anchor[0] = p->anchor[1] = p->anchor[2] = p->anchor[3] = 0.5f, p->align[0] = p->align[1] = 0.5f;
    p->size[0] = 1180.0f, p->size[1] = 900.0f, p->radius = 14.0f, p->padding = 28.0f, p->border_w = 1.0f;
    rgba(p->bg, 0.045f, 0.05f, 0.06f, 0.96f), rgba(p->border_col, 1, 1, 1, 0.08f);
    int col = W(t, UIW_VBOX, panel, "Items");
    t->w[col].padding = 14.0f, t->w[col].bg[3] = 0.0f;
    int title = text(t, col, "Title", "Load Game", 42.0f, 0.0f, "Save.Title");
    t->w[title].bold = 1;
    int top = W(t, UIW_HBOX, col, "TopButtons");
    t->w[top].size[1] = 64.0f, t->w[top].padding = 12.0f, t->w[top].bg[3] = 0.0f;
    button(t, top, "ContinueButton", "Continue", 230.0f, 64.0f, 24.0f, "Save.Continue", NULL);
    button(t, top, "NewSaveButton", "New Save", 230.0f, 64.0f, 24.0f, "Save.New", NULL);
    button(t, top, "NewGameButton", "New Game", 230.0f, 64.0f, 24.0f, "Save.NewGame", NULL);
    int sp = W(t, UIW_SPACER, top, NULL);
    t->w[sp].size_rule = 1;
    int back = button(t, top, "BackButton", "Back", 200.0f, 64.0f, 24.0f, "Save.Back", NULL);
    rgba(t->w[back].bg, 0.15f, 0.42f, 0.85f, 0.95f);
    int status = text(t, col, "Status", "", 20.0f, 0.0f, "Save.Status");
    rgba(t->w[status].color, 0.62f, 0.85f, 0.62f, 1.0f);
    int empty = text(t, col, "Empty", "No saved games yet.", 24.0f, 0.5f, "Save.Empty");
    rgba(t->w[empty].color, 0.6f, 0.62f, 0.68f, 1.0f);
    int list = W(t, UIW_SCROLL, col, "SlotList");
    t->w[list].size_rule = 1, t->w[list].padding = 10.0f, t->w[list].size[1] = 600.0f;
    for (int r = 0; r < ROWS; r++) {
        char nm[UI_NAME], bd[UI_NAME];
#define B(fmt) (snprintf(bd, sizeof bd, fmt, r), bd)
#define NM(fmt) (snprintf(nm, sizeof nm, fmt, r), nm)
        int row = W(t, UIW_HBOX, list, NM("Row%d"));
        t->w[row].size[1] = 118.0f, t->w[row].padding = 16.0f, t->w[row].radius = 8.0f;
        rgba(t->w[row].bg, 1, 1, 1, 0.035f);
        snprintf(t->w[row].bind_setting, UI_NAME, "%s", B("Save.%d.Row"));
        int img = W(t, UIW_IMAGE, row, NM("Thumb%d"));
        t->w[img].size[0] = 192.0f, t->w[img].size[1] = 108.0f, t->w[img].radius = 4.0f, t->w[img].v_align = UI_ALIGN_CENTER;
        rgba(t->w[img].color, 1, 1, 1, 1);
        snprintf(t->w[img].bind_setting, UI_NAME, "%s", B("Save.%d.Thumb"));
        int info = W(t, UIW_VBOX, row, NM("Info%d"));
        t->w[info].size_rule = 1, t->w[info].padding = 6.0f, t->w[info].bg[3] = 0.0f;
        int name = text(t, info, NM("Name%d"), "Slot", 26.0f, 0.0f, B("Save.%d.Name"));
        t->w[name].bold = 1;
        int det = text(t, info, NM("Details%d"), "", 19.0f, 0.0f, B("Save.%d.Info"));
        rgba(t->w[det].color, 0.62f, 0.64f, 0.70f, 1.0f);
        int sel = button(t, row, NM("Select%d"), "Load", 170.0f, 60.0f, 22.0f, B("Save.%d.Select"), NULL);
        t->w[sel].v_align = UI_ALIGN_CENTER;
        snprintf(t->w[sel + 1].bind_setting, UI_NAME, "%s", B("Save.%d.Action")); // (its label: Load / Overwrite)
        int del = button(t, row, NM("Delete%d"), "Delete", 150.0f, 60.0f, 22.0f, B("Save.%d.Delete"), NULL);
        t->w[del].v_align = UI_ALIGN_CENTER;
        rgba(t->w[del].bg, 0.42f, 0.12f, 0.12f, 0.9f), rgba(t->w[del].hover, 0.62f, 0.18f, 0.18f, 0.95f);
#undef B
#undef NM
    }
    // the Event Graph: OnBack (Esc, a gamepad's B) closes it, as Back does
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t ev = N(a, g, BPK_EVENT_CUSTOM, 0, 0, "OnBack"), rm = N(a, g, BPK_REMOVE_FROM_PARENT, 1, 0, NULL);
    L(g, ev, "", rm, "");
    if (g_bad) printf("[RealEngine] Save Game: WBP_SaveSlots was made with errors (%u widgets)\n", t->n);
    if (!bp_compile(a)) printf("[RealEngine] Save Game: WBP_SaveSlots: %s\n", a->error);
    bp_save(a);
    return a;
}

uint32_t save_menu_asset(void) {
    BpAsset* a = bp_load(SLOTS_VPATH);
    if (!a && ui_content_root()[0]) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
        re_make_dirs(dir);
        a = make_slots();
    }
    return a ? a->hash : 0u;
}

// WBP_PauseMenu: Save Game and Load Game after Options (Resume, Options, Save Game, Load Game, Main Menu, Quit Game) (a menu made before this module gets them; one already having a
// SaveButton is left as it is)
void save_menu_pause_buttons(void) {
    BpAsset* a = bp_load(PAUSE_VPATH);
    if (!a || !a->ui || ui_tree_find(a->ui, "SaveButton") >= 0 || ui_tree_find(a->ui, "LoadGameButton") >= 0) return;
    UiTree* t = a->ui;
    int items = ui_tree_find(t, "Items"), resume = ui_tree_find(t, "ResumeButton");
    if (items < 0 || resume < 0 || t->n + 4 > UI_MAX_WIDGETS) return;
    g_bad = false;
    const char* names[2] = { "SaveButton", "LoadGameButton" };
    const char* labels[2] = { "Save Game", "Load Game" };
    for (int k = 0; k < 2; k++) {
        int b = button(t, items, names[k], labels[k], t->w[resume].size[0], t->w[resume].size[1], 28.0f, NULL, NULL);
        UiWidget keep = t->w[b];
        memcpy(t->w[b].bg, t->w[resume].bg, sizeof keep.bg), memcpy(t->w[b].hover, t->w[resume].hover, sizeof keep.hover);
        memcpy(t->w[b].press, t->w[resume].press, sizeof keep.press), t->w[b].radius = t->w[resume].radius;
        int lb = ui_tree_find(t, k ? "LoadGameButtonLabel" : "SaveButtonLabel"), rl = ui_tree_find(t, "ResumeButtonLabel");
        if (lb >= 0 && rl >= 0) t->w[lb].font_size = t->w[rl].font_size, t->w[lb].size[1] = t->w[rl].size[1];
        // up to just after Options (and Save Game): the boxes stack in their order
        for (int step = 0; step < 16; step++) {
            int prev = -1;
            for (uint32_t i = 0; i < t->n; i++)
                if (t->w[i].parent == items && (int)i != b) {
                    if ((int)i > b) break;
                    prev = (int)i;
                }
            const char* stop = k ? "SaveButton" : (ui_tree_find(t, "OptionsButton") >= 0 ? "OptionsButton" : "ResumeButton");
            if (prev < 0 || !strcmp(t->w[prev].name, stop)) break;
            int was = b;
            if (!ui_tree_move(t, b, -1)) break;
            b = ui_tree_find(t, names[k]);
            if (b == was) break;
        }
    }
    int panel = ui_tree_find(t, "Panel");
    if (panel >= 0) t->w[panel].size[1] += 2.0f * (t->w[resume].size[1] + 18.0f); // (room for the two)
    BpGraph* g = &a->graphs[0];
    for (int k = 0; k < 2; k++) {
        char ev[UI_NAME + 16];
        snprintf(ev, sizeof ev, "OnClicked_%s", names[k]);
        uint32_t e = N(a, g, BPK_EVENT_CUSTOM, 0, 5.0f + (float)k, ev), op = N(a, g, BPK_OPEN_SAVE_MENU, 1, 5.0f + (float)k, NULL);
        BpNode* on = bp_node(g, op);
        if (on) on->pins[pin(g, op, 0, "Save Mode")].lit.v[0] = k ? 0.0f : 1.0f;
        L(g, e, "", op, "");
    }
    if (g_bad) printf("[RealEngine] Save Game: the pause menu's Save / Load buttons were added with errors\n");
    if (!bp_compile(a)) printf("[RealEngine] Save Game: WBP_PauseMenu: %s\n", a->error);
    bp_save(a);
}

// --- The menu at run time ---

static void refresh(void) {
    M.n = progress_list(M.list, ROWS, false);
}

static BpAsset* slots_class(void) {
    uint32_t h = save_menu_asset();
    return h ? bp_find_hash(h) : NULL;
}

bool save_menu_is_open(void) { return M.menu != ENTITY_NONE && ui_is_shown(M.menu); }

bool save_menu_open(World* w, bool save_mode) {
    BpAsset* c = slots_class();
    if (!c || !w) return false;
    if (!c->compiled && !bp_compile(c)) return false;
    if (!save_menu_is_open()) M.mouse_before = ui_mouse_shown();
    if (M.menu == ENTITY_NONE || !ecs_alive(w, M.menu)) M.menu = ui_create_widget(w, c); // (made once a play; shown again after)
    if (M.menu == ENTITY_NONE) return false;
    M.save_mode = save_mode, M.status[0] = 0;
    refresh();
    ui_add_to_viewport(M.menu, 120); // (over the pause menu and its Options)
    ui_show_mouse(true);
    return true;
}

void save_menu_close(World* w) {
    (void)w;
    if (!save_menu_is_open()) return;
    ui_remove_from_parent(M.menu);
    if (!M.mouse_before && !pause_menu_is_open()) ui_show_mouse(false);
}

static void say(const char* s) { snprintf(M.status, sizeof M.status, "%s", s ? TR(s) : ""), M.status_gen++; }

uint32_t save_menu_serial(void) {
    if (M.gen_seen != progress_stats()->saves + progress_stats()->loads * 7u) { // (a save elsewhere: an auto save, a Checkpoint)
        M.gen_seen = progress_stats()->saves + progress_stats()->loads * 7u;
        refresh();
    }
    return 1u + M.gen_seen * 31u + M.n * 3u + (M.save_mode ? 0x8000u : 0u) + (uint32_t)strlen(M.status) * 977u + M.status_gen * 7919u;
}

// "Save.3.Name" -> 3, "Name"; "Save.Title" -> -1, "Title"
static int parse(const char* bind, const char** what) {
    const char* s = bind + 5;
    if (*s >= '0' && *s <= '9') {
        int r = atoi(s);
        const char* dot = strchr(s, '.');
        *what = dot ? dot + 1 : "";
        return r;
    }
    *what = s;
    return -1;
}

bool save_menu_bind(void* widget) {
    UiWidget* x = widget;
    if (!x || strncmp(x->bind_setting, "Save.", 5)) return false;
    const char* what;
    int r = parse(x->bind_setting, &what);
    const SaveSlotInfo* si = r >= 0 && (uint32_t)r < M.n ? &M.list[r] : NULL;
    if (r >= 0) {
        if (!strcmp(what, "Row")) x->visible = si != NULL;
        else if (!si) return true;
        else if (!strcmp(what, "Name")) snprintf(x->text, sizeof x->text, "%s", si->display[0] ? si->display : si->slot);
        else if (!strcmp(what, "Info")) {
            char when[40], pt[32];
            progress_format_date(si->time, when, sizeof when), progress_format_duration(si->play_time, pt, sizeof pt);
            const char* kind = (si->flags & SAVE_FLAG_CHECKPOINT) ? TR("Checkpoint") : (si->flags & SAVE_FLAG_AUTO) ? TR("Auto Save") : TR("Saved");
            snprintf(x->text, sizeof x->text, "%s  -  %s  -  %s %s  -  %s", si->level, when, TR("Played"), pt, kind);
        } else if (!strcmp(what, "Action")) snprintf(x->text, sizeof x->text, "%s", TR(M.save_mode ? "Overwrite" : "Load"));
        return true;
    }
    if (!strcmp(what, "Title")) snprintf(x->text, sizeof x->text, "%s", TR(M.save_mode ? "Save Game" : "Load Game"));
    else if (!strcmp(what, "Status")) snprintf(x->text, sizeof x->text, "%s", M.status);
    else if (!strcmp(what, "Empty")) x->visible = M.n == 0;
    else if (!strcmp(what, "Continue")) x->visible = M.n > 0 && !M.save_mode;
    else if (!strcmp(what, "New")) x->visible = M.save_mode;
    else if (!strcmp(what, "NewGame")) x->visible = !M.save_mode;
    return true;
}

const uint8_t* save_menu_thumb(const char* bind, char* key, size_t cap, uint32_t* w, uint32_t* h, uint32_t* gen) {
    const char* what;
    if (!bind || strncmp(bind, "Save.", 5)) return NULL;
    int r = parse(bind, &what);
    if (r < 0 || (uint32_t)r >= M.n || strcmp(what, "Thumb")) return NULL;
    snprintf(key, cap, "save:%s", M.list[r].slot);
    return progress_thumb(M.list[r].slot, w, h, gen);
}

// The level New Game opens: the Game Default Map from the main menu, else this level again
static void new_game_level(char* out, size_t cap) {
    const ProjectSettings* ps = project();
    const char* cur = level_stream_persistent();
    char mm[128] = "";
    const char* b = strrchr(ps->main_menu_map, '/');
    snprintf(mm, sizeof mm, "%s", b ? b + 1 : ps->main_menu_map);
    char* dot = strrchr(mm, '.');
    if (dot) *dot = 0;
    out[0] = 0;
    if (mm[0] && !_stricmp(mm, cur) && ps->game_default_map[0]) {
        b = strrchr(ps->game_default_map, '/');
        snprintf(out, cap, "%s", b ? b + 1 : ps->game_default_map);
        dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }
}

static void leave(World* w) { // (a load or a new game: the menus go, the game goes on)
    save_menu_close(w);
    pause_menu_close(w);
}

bool save_menu_event(World* w, Entity live, void* widget, const char* event) {
    UiWidget* x = widget;
    (void)live;
    if (!x || strncmp(x->bind_setting, "Save.", 5)) return false;
    if (strcmp(event, "OnClicked")) return true;
    const char* what;
    int r = parse(x->bind_setting, &what);
    const SaveSlotInfo* si = r >= 0 && (uint32_t)r < M.n ? &M.list[r] : NULL;
    if (r >= 0 && !si) return true;
    if (si && !strcmp(what, "Select")) {
        if (M.save_mode) say(progress_save(w, si->slot, si->display, 0) ? "Saved." : "Could not save.");
        else if (progress_load(w, si->slot)) leave(w);
        else say("This save is damaged and can't be loaded.");
    } else if (si && !strcmp(what, "Delete")) {
        say(progress_slot_delete(si->slot) ? "Deleted." : "Could not delete.");
    } else if (!strcmp(what, "New")) { // the first free "Save_N"
        char slot[32], display[64];
        int k = 1;
        for (; k < 1000; k++) {
            snprintf(slot, sizeof slot, "Save_%d", k);
            if (!progress_slot_exists(slot)) break;
        }
        snprintf(display, sizeof display, "%s %d", TR("Save"), k);
        say(progress_save(w, slot, display, 0) ? "Saved." : "Could not save.");
    } else if (!strcmp(what, "Continue")) {
        char slot[SAVE_SLOT_NAME];
        if (progress_latest(slot, sizeof slot) && progress_load(w, slot)) leave(w);
        else say("This save is damaged and can't be loaded.");
    } else if (!strcmp(what, "NewGame")) {
        char lv[128];
        new_game_level(lv, sizeof lv);
        progress_new_game(w, lv);
        leave(w);
    } else if (!strcmp(what, "Back")) save_menu_close(w);
    refresh();
    return true;
}
