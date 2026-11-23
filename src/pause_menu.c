// The pause menu (game_settings.h, notas-dev/pausa-opcoes.md): the two Widget Blueprints the engine makes in Content/UI
// when they are missing — WBP_PauseMenu (Resume, Options, Main Menu, Quit Game) and WBP_Options (tabs Graphics, Audio,
// Controls, Language; Apply, Reset to Defaults, Back) — made through the editor's own API: a UMG tree whose controls are
// bound to the Game User Settings by name (no node needed to read or write them) and an Event Graph of ordinary nodes
// (Close Pause Menu, Create Widget, Set Widget Visible, Apply Settings...), so a project copies and changes them like any
// Blueprint. Then the menu opened and closed: by Esc in the game, the Play's key, a gamepad's Start (pause_menu_key).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "bp_internal.h"
#include "game_settings.h"
#include "i18n.h"
#include "input.h"
#include "platform_files.h"
#include "project.h"
#include "save_progress.h"
#include "ui_widget.h"

#define MENU_VPATH "/Content/UI/WBP_PauseMenu.rbp"
#define OPTIONS_VPATH "/Content/UI/WBP_Options.rbp"

static struct {
    Entity menu;
    bool open, mouse_before, paused_before;
} P;

// --- Making the widgets ---

static bool g_bad;

static int W(UiTree* t, uint8_t kind, int parent, const char* name) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_bad = true, 0;
    if (name) snprintf(t->w[i].name, UI_NAME, "%s", name);
    return i;
}
static void stretch(UiWidget* w) {
    w->anchor[0] = w->anchor[1] = 0.0f, w->anchor[2] = w->anchor[3] = 1.0f;
    w->pos[0] = w->pos[1] = w->size[0] = w->size[1] = 0.0f;
}
static void centered(UiWidget* w, float wd, float ht) {
    w->anchor[0] = w->anchor[1] = w->anchor[2] = w->anchor[3] = 0.5f;
    w->align[0] = w->align[1] = 0.5f;
    w->pos[0] = w->pos[1] = 0.0f;
    w->size[0] = wd, w->size[1] = ht;
}
static void rgba(float* c, float r, float g, float b, float a) { c[0] = r, c[1] = g, c[2] = b, c[3] = a; }

static int text(UiTree* t, int parent, const char* name, const char* s, float font, float jx) {
    int i = W(t, UIW_TEXT, parent, name);
    UiWidget* w = &t->w[i];
    snprintf(w->text, sizeof w->text, "%s", s);
    w->font_size = font, w->align[0] = jx, w->align[1] = 0.5f, w->size[1] = font * 1.6f;
    return i;
}
static int button(UiTree* t, int parent, const char* name, const char* label, float h, float font) {
    int b = W(t, UIW_BUTTON, parent, name);
    t->w[b].size[1] = h, t->w[b].size[0] = 220.0f, t->w[b].radius = 8.0f;
    rgba(t->w[b].hover, 0.20f, 0.42f, 0.78f, 0.95f);
    rgba(t->w[b].press, 0.12f, 0.30f, 0.62f, 1.0f);
    char tn[UI_NAME];
    snprintf(tn, sizeof tn, "%sLabel", name);
    int l = text(t, b, tn, label, font, 0.5f);
    t->w[l].bold = 1;
    return b;
}
// a row: the label, the control bound to the setting, its value (none for a check box / a combo)
static int row(UiTree* t, int parent, const char* label, uint8_t kind, const char* setting, bool value) {
    int r = W(t, UIW_HBOX, parent, NULL);
    t->w[r].size[1] = 54.0f, t->w[r].padding = 14.0f, t->w[r].bg[3] = 0.0f;
    int l = text(t, r, NULL, label, 24.0f, 0.0f);
    t->w[l].size_rule = 1;
    int c = W(t, kind, r, NULL);
    t->w[c].size[0] = kind == UIW_CHECKBOX ? 54.0f : 420.0f;
    if (kind == UIW_CHECKBOX) t->w[c].v_align = UI_ALIGN_CENTER, t->w[c].size[1] = 38.0f;
    snprintf(t->w[c].bind_setting, UI_NAME, "%s", setting);
    if (kind == UIW_CHECKBOX) { // (its row keeps the value column's width: the check boxes line up with the rest)
        int sp = W(t, UIW_SPACER, r, NULL);
        t->w[sp].size[0] = 366.0f + 110.0f + 14.0f;
    }
    if (value) {
        int v = text(t, r, NULL, "", 22.0f, 1.0f);
        t->w[v].size[0] = 110.0f;
        snprintf(t->w[v].bind_setting, UI_NAME, "%s", setting);
    } else if (kind != UIW_CHECKBOX) {
        int sp = W(t, UIW_SPACER, r, NULL);
        t->w[sp].size[0] = 110.0f;
    }
    return c;
}

// a node of the graph and a wire by the pins' names (umg_test.c's way)
static uint32_t N(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* txt) {
    BpNode* n = bp_add_node(a, g, kind, x * 320.0f, y * 160.0f, txt, NULL);
    if (!n) g_bad = true;
    return n ? n->id : 0;
}
static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    g_bad = true;
    return 0;
}
static void L(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) g_bad = true;
}
static void chain(BpGraph* g, const uint32_t* ids, int n) {
    for (int i = 0; i + 1 < n; i++) L(g, ids[i], "", ids[i + 1], "");
}
static BpGraph* empty_graph(BpAsset* a) {
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    return g;
}

// "IA_Jump" -> "Jump", "IA_MoveForward" -> "Move Forward"
static void action_label(const char* action, char* out, size_t cap) {
    const char* s = !_strnicmp(action, "IA_", 3) ? action + 3 : action;
    size_t o = 0;
    for (const char* c = s; *c && o + 2 < cap; c++) {
        if (c != s && *c >= 'A' && *c <= 'Z' && c[-1] >= 'a' && c[-1] <= 'z') out[o++] = ' ';
        out[o++] = *c == '_' ? ' ' : *c;
    }
    out[o] = 0;
}

static BpAsset* make_options(void) {
    BpAsset* a = bp_create(BPA_CLASS, "/Content/UI", "WBP_Options", "User Widget");
    if (!a || !(a->ui = calloc(1, sizeof(UiTree)))) return NULL;
    UiTree* t = a->ui;
    ui_tree_default(t);
    g_bad = false;
    int dim = W(t, UIW_BORDER, 0, "Dim");
    stretch(&t->w[dim]), rgba(t->w[dim].bg, 0, 0, 0, 0.35f), t->w[dim].radius = 0.0f;
    int panel = W(t, UIW_BORDER, 0, "Panel");
    centered(&t->w[panel], 1160.0f, 820.0f);
    rgba(t->w[panel].bg, 0.045f, 0.05f, 0.06f, 0.96f), t->w[panel].radius = 14.0f, t->w[panel].padding = 30.0f;
    t->w[panel].border_w = 1.0f, rgba(t->w[panel].border_col, 1, 1, 1, 0.08f);
    int rows = W(t, UIW_VBOX, panel, "Rows");
    t->w[rows].padding = 14.0f, t->w[rows].bg[3] = 0.0f;
    int title = text(t, rows, "Title", "Options", 40.0f, 0.0f);
    t->w[title].bold = 1;
    int tabs = W(t, UIW_HBOX, rows, "Tabs");
    t->w[tabs].size[1] = 58.0f, t->w[tabs].padding = 10.0f, t->w[tabs].bg[3] = 0.0f;
    static const char* const tab_names[4] = { "GraphicsTab", "AudioTab", "ControlsTab", "LanguageTab" };
    static const char* const tab_labels[4] = { "Graphics", "Audio", "Controls", "Language" };
    static const char* const page_names[4] = { "GraphicsPage", "AudioPage", "ControlsPage", "LanguagePage" };
    for (int k = 0; k < 4; k++) {
        int b = button(t, tabs, tab_names[k], tab_labels[k], 58.0f, 24.0f);
        t->w[b].size_rule = 1;
    }
    int pages = W(t, UIW_OVERLAY, rows, "Pages");
    t->w[pages].size_rule = 1;
    int pg[4];
    for (int k = 0; k < 4; k++) {
        pg[k] = W(t, k == 2 ? UIW_VBOX : UIW_VBOX, pages, page_names[k]);
        t->w[pg[k]].padding = 8.0f, t->w[pg[k]].bg[3] = 0.0f, t->w[pg[k]].visible = k == 0;
    }
    // Graphics
    row(t, pg[0], "Quality Preset", UIW_COMBO, "Scalability", false);
    row(t, pg[0], "Resolution", UIW_COMBO, "Resolution", false);
    row(t, pg[0], "Window Mode", UIW_COMBO, "WindowMode", false);
    row(t, pg[0], "VSync", UIW_CHECKBOX, "VSync", false);
    row(t, pg[0], "Frame Rate Limit", UIW_COMBO, "FrameRateLimit", false);
    row(t, pg[0], "Resolution Scale", UIW_SLIDER, "ResolutionScale", true);
    row(t, pg[0], "Upscaler", UIW_COMBO, "Upscaler", false); // (4.4a: TSR, FSR, DLSS, XeSS where installed)
    row(t, pg[0], "Upscaling", UIW_COMBO, "UpscalingMode", false); // (upscale.h: Quality, Balanced, Performance...)
    row(t, pg[0], "Dynamic Resolution", UIW_CHECKBOX, "DynamicResolution", false);
    // Audio
    row(t, pg[1], "Master Volume", UIW_SLIDER, "MasterVolume", true);
    row(t, pg[1], "Music", UIW_SLIDER, "MusicVolume", true);
    row(t, pg[1], "Effects", UIW_SLIDER, "SFXVolume", true);
    row(t, pg[1], "Voice", UIW_SLIDER, "VoiceVolume", true);
    // Controls: the look, then a key selector per remappable mapping of the project's Input Mapping Contexts
    row(t, pg[2], "Mouse Sensitivity", UIW_SLIDER, "MouseSensitivity", true);
    row(t, pg[2], "Invert Y", UIW_CHECKBOX, "InvertY", false);
    {
        int h = W(t, UIW_HBOX, pg[2], "KeysHeader");
        t->w[h].size[1] = 40.0f, t->w[h].padding = 14.0f, t->w[h].bg[3] = 0.0f;
        int l = text(t, h, NULL, "Action", 20.0f, 0.0f);
        t->w[l].size_rule = 1, rgba(t->w[l].color, 0.6f, 0.62f, 0.68f, 1.0f);
        l = text(t, h, NULL, "Keyboard", 20.0f, 0.5f);
        t->w[l].size[0] = 280.0f, rgba(t->w[l].color, 0.6f, 0.62f, 0.68f, 1.0f);
        l = text(t, h, NULL, "Gamepad", 20.0f, 0.5f);
        t->w[l].size[0] = 280.0f, rgba(t->w[l].color, 0.6f, 0.62f, 0.68f, 1.0f);
    }
    int list = W(t, UIW_SCROLL, pg[2], "KeyList");
    t->w[list].size_rule = 1, t->w[list].padding = 6.0f;
    t->w[list].size[1] = 300.0f;
    int nrows = 0;
    char seen[16][2][GUS_NAME];
    for (uint32_t c = 0; c < input_context_count() && nrows < 10; c++) {
        const InputContext* ic = input_context_at(c);
        for (uint32_t m = 0; ic && m < ic->nmaps && nrows < 10 && t->n + 4 < UI_MAX_WIDGETS - 8; m++) {
            const InputMapping* mp = &ic->maps[m];
            if (strstr(mp->key, "Axis") || !_strnicmp(mp->key, "Gamepad", 7) || !_stricmp(mp->key, "Mouse X") || !_stricmp(mp->key, "Mouse Y")) continue;
            bool dup = false;
            for (int k = 0; k < nrows && !dup; k++) dup = !_stricmp(seen[k][0], mp->action) && !_stricmp(seen[k][1], mp->key);
            if (dup) continue;
            int same = 0; // (an action on several keys: W / A / S / D for IA_Move): each its own row, named by the key
            for (uint32_t q = 0; q < ic->nmaps; q++)
                same += !_stricmp(ic->maps[q].action, mp->action) && !strstr(ic->maps[q].key, "Axis") && _strnicmp(ic->maps[q].key, "Gamepad", 7);
            bool first = true;
            for (int k = 0; k < nrows; k++) first &= _stricmp(seen[k][0], mp->action) != 0;
            snprintf(seen[nrows][0], GUS_NAME, "%s", mp->action), snprintf(seen[nrows][1], GUS_NAME, "%s", mp->key);
            int r = W(t, UIW_HBOX, list, NULL);
            t->w[r].size[1] = 50.0f, t->w[r].padding = 14.0f, t->w[r].bg[3] = 0.0f;
            char lab[96], an[64];
            action_label(mp->action, an, sizeof an);
            if (same > 1) snprintf(lab, sizeof lab, "%s (%s)", an, mp->key);
            else snprintf(lab, sizeof lab, "%s", an);
            int l = text(t, r, NULL, lab, 22.0f, 0.0f);
            t->w[l].size_rule = 1;
            int ks = W(t, UIW_KEY_SELECTOR, r, NULL);
            t->w[ks].size[0] = 280.0f;
            snprintf(t->w[ks].bind_setting, UI_NAME, "Key:%s:%s", mp->action, mp->key);
            const char* gp = gus_action_default_key(mp->action, true);
            if (first && gp[0]) {
                int kg = W(t, UIW_KEY_SELECTOR, r, NULL);
                t->w[kg].size[0] = 280.0f;
                snprintf(t->w[kg].bind_setting, UI_NAME, "Key:%s:Gamepad", mp->action);
            } else {
                int sp = W(t, UIW_SPACER, r, NULL);
                t->w[sp].size[0] = 280.0f;
            }
            nrows++;
        }
    }
    // Language
    row(t, pg[3], "Language", UIW_COMBO, "Language", false);
    row(t, pg[3], "Auto Save", UIW_CHECKBOX, "AutoSave", false); // (save_progress.h: the timed and level change auto saves; here, not in Controls: its keys need the room)
    // the footer: Reset to Defaults on the left, Apply and Back on the right
    int foot = W(t, UIW_HBOX, rows, "Footer");
    t->w[foot].size[1] = 60.0f, t->w[foot].padding = 14.0f, t->w[foot].bg[3] = 0.0f;
    int rb = button(t, foot, "ResetButton", "Reset to Defaults", 60.0f, 22.0f);
    t->w[rb].size[0] = 280.0f;
    int sp = W(t, UIW_SPACER, foot, NULL);
    t->w[sp].size_rule = 1;
    int ab = button(t, foot, "ApplyButton", "Apply", 60.0f, 24.0f);
    t->w[ab].size[0] = 200.0f;
    int bb = button(t, foot, "BackButton", "Back", 60.0f, 24.0f);
    t->w[bb].size[0] = 200.0f;
    rgba(t->w[bb].bg, 0.15f, 0.42f, 0.85f, 0.95f);
    // the Event Graph: each tab shows its page (Set Widget Visible on the four), Apply / Reset / Back, and OnBack (Esc,
    // a gamepad's B: the pause menu sends it) as Back
    BpGraph* g = empty_graph(a);
    for (int k = 0; k < 4; k++) {
        char ev[UI_NAME + 16];
        snprintf(ev, sizeof ev, "OnClicked_%s", tab_names[k]);
        uint32_t ids[5];
        ids[0] = N(a, g, BPK_EVENT_CUSTOM, 0, (float)k * 1.4f, ev);
        for (int p = 0; p < 4; p++) {
            ids[p + 1] = N(a, g, BPK_WIDGET_SET_VISIBLE, (float)(p + 1), (float)k * 1.4f, page_names[p]);
            bp_node(g, ids[p + 1])->pins[pin(g, ids[p + 1], 0, "Visible")].lit.v[0] = p == k ? 1.0f : 0.0f;
        }
        chain(g, ids, 5);
    }
    uint32_t apply[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 6, "OnClicked_ApplyButton"), N(a, g, BPK_APPLY_GUS, 1, 6, NULL) };
    chain(g, apply, 2);
    uint32_t reset[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 7, "OnClicked_ResetButton"), N(a, g, BPK_DEFAULTS_GUS, 1, 7, NULL) };
    chain(g, reset, 2);
    for (int k = 0; k < 2; k++) {
        uint32_t back[3] = { N(a, g, BPK_EVENT_CUSTOM, 0, 8.0f + k, k ? "OnBack" : "OnClicked_BackButton"), N(a, g, BPK_APPLY_GUS, 1, 8.0f + k, NULL),
                             N(a, g, BPK_REMOVE_FROM_PARENT, 2, 8.0f + k, NULL) };
        chain(g, back, 3);
    }
    if (g_bad) printf("[RealEngine] Pause menu: WBP_Options was made with errors (%u widgets)\n", t->n);
    if (!bp_compile(a)) printf("[RealEngine] Pause menu: WBP_Options: %s\n", a->error);
    bp_save(a);
    return a;
}

static BpAsset* make_menu(void) {
    BpAsset* a = bp_create(BPA_CLASS, "/Content/UI", "WBP_PauseMenu", "User Widget");
    if (!a || !(a->ui = calloc(1, sizeof(UiTree)))) return NULL;
    UiTree* t = a->ui;
    ui_tree_default(t);
    g_bad = false;
    int dim = W(t, UIW_BORDER, 0, "Dim");
    stretch(&t->w[dim]), rgba(t->w[dim].bg, 0, 0, 0, 0.55f), t->w[dim].radius = 0.0f;
    int panel = W(t, UIW_BORDER, 0, "Panel");
    centered(&t->w[panel], 520.0f, 560.0f);
    rgba(t->w[panel].bg, 0.045f, 0.05f, 0.06f, 0.95f), t->w[panel].radius = 14.0f, t->w[panel].padding = 30.0f;
    t->w[panel].border_w = 1.0f, rgba(t->w[panel].border_col, 1, 1, 1, 0.08f);
    int box = W(t, UIW_VBOX, panel, "Items");
    t->w[box].padding = 18.0f, t->w[box].bg[3] = 0.0f;
    int title = text(t, box, "Title", "Paused", 46.0f, 0.5f);
    t->w[title].bold = 1, t->w[title].size[1] = 80.0f;
    button(t, box, "ResumeButton", "Resume", 74.0f, 28.0f);
    button(t, box, "OptionsButton", "Options", 74.0f, 28.0f);
    button(t, box, "MainMenuButton", "Main Menu", 74.0f, 28.0f);
    button(t, box, "QuitButton", "Quit Game", 74.0f, 28.0f);
    BpGraph* g = empty_graph(a);
    uint32_t resume[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 0, "OnClicked_ResumeButton"), N(a, g, BPK_CLOSE_PAUSE_MENU, 1, 0, NULL) };
    chain(g, resume, 2);
    uint32_t opt[3] = { N(a, g, BPK_EVENT_CUSTOM, 0, 1, "OnClicked_OptionsButton"), N(a, g, BPK_CREATE_WIDGET, 1, 1, NULL),
                        N(a, g, BPK_ADD_TO_VIEWPORT, 2, 1, NULL) };
    snprintf(bp_node(g, opt[1])->pins[pin(g, opt[1], 0, "Class")].slit, sizeof bp_node(g, opt[1])->pins[0].slit, "WBP_Options");
    bp_node(g, opt[2])->pins[pin(g, opt[2], 0, "ZOrder")].lit.v[0] = 110.0f; // (over the pause menu)
    chain(g, opt, 3);
    L(g, opt[1], "Return Value", opt[2], "Target");
    uint32_t mm[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 2, "OnClicked_MainMenuButton"), N(a, g, BPK_MAIN_MENU, 1, 2, NULL) };
    chain(g, mm, 2);
    uint32_t quit[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 3, "OnClicked_QuitButton"), N(a, g, BPK_QUIT_GAME, 1, 3, NULL) };
    chain(g, quit, 2);
    if (g_bad) printf("[RealEngine] Pause menu: WBP_PauseMenu was made with errors\n");
    if (!bp_compile(a)) printf("[RealEngine] Pause menu: WBP_PauseMenu: %s\n", a->error);
    bp_save(a);
    return a;
}

uint32_t pause_menu_assets(void) {
    const char* root = ui_content_root();
    BpAsset* opt = bp_load(OPTIONS_VPATH);
    BpAsset* menu = bp_load(MENU_VPATH);
    if ((!opt || !menu) && root[0]) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/UI", root);
        re_make_dirs(dir);
        if (!opt) opt = make_options(); // (first: the menu's Create Widget names it)
        if (!menu) menu = make_menu();
    }
    if (opt && root[0] && opt->ui) { // (4.4a) an Options made before the Upscaler gets its row at the Graphics page's end
        bool has = false;
        for (uint32_t i = 0; i < opt->ui->n && !has; i++) has = !strcmp(opt->ui->w[i].bind_setting, "Upscaler");
        int pg = has ? -1 : ui_tree_find(opt->ui, "GraphicsPage");
        if (pg >= 0) {
            row(opt->ui, pg, "Upscaler", UIW_COMBO, "Upscaler", false);
            if (!bp_compile(opt)) printf("[RealEngine] Pause menu: WBP_Options: %s\n", opt->error);
            bp_save(opt);
        }
    }
    if (menu && root[0] && menu->ui && ui_tree_find(menu->ui, "SaveButton") < 0) {
        save_menu_asset();         // WBP_SaveSlots (save_menu.c), which its Save Game / Load Game buttons open
        save_menu_pause_buttons(); // (a menu made before them gets them; one changed by the project keeps its own)
    }
    return menu ? menu->hash : 0u;
}

// --- The menu at run time ---

static BpAsset* menu_class(void) {
    const ProjectSettings* ps = project();
    BpAsset* c = ps->pause_menu_widget[0] ? bp_find_name(BPA_CLASS, ps->pause_menu_widget) : NULL;
    if (c && bp_native_of(c) == BPN_USER_WIDGET) return c;
    uint32_t h = pause_menu_assets();
    return h ? bp_find_hash(h) : NULL;
}

bool pause_menu_is_open(void) { return P.open; }

bool pause_menu_open(World* w) {
    if (P.open) return true;
    BpAsset* c = menu_class();
    if (!c) return false;
    P.mouse_before = ui_mouse_shown();
    P.paused_before = game_paused();
    game_set_paused(true); // (first: its Construct sees the game paused)
    if (P.menu == ENTITY_NONE || !ecs_alive(w, P.menu)) P.menu = ui_create_widget(w, c); // (made once a play; shown again after)
    if (P.menu == ENTITY_NONE) {
        game_set_paused(P.paused_before);
        return false;
    }
    ui_add_to_viewport(P.menu, 100);
    ui_show_mouse(true);
    P.open = true;
    return true;
}

void pause_menu_close(World* w) {
    (void)w;
    if (!P.open) return;
    BpAsset* opt = bp_load(OPTIONS_VPATH);
    if (opt) ui_hide_class(opt->hash); // (its Options with it)
    if (P.menu != ENTITY_NONE) ui_remove_from_parent(P.menu);
    ui_show_mouse(P.mouse_before);
    game_set_paused(P.paused_before);
    P.open = false;
}

void pause_menu_key(World* w, bool back) {
    if (ui_capturing_key()) return; // (a key selector waits for a key: Esc cancels it there)
    if (P.open || back) {
        if (ui_back(w, P.menu)) return; // the front widget's OnBack (the Options: back to the menu)
        if (P.open) pause_menu_close(w);
        return;
    }
    if (project()->pause_menu) pause_menu_open(w);
}

void pause_menu_poll_gamepad(World* w) {
    static unsigned char prev[2];
    GLFWgamepadstate gp;
    unsigned char now[2] = { 0, 0 };
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp))
        now[0] = gp.buttons[GLFW_GAMEPAD_BUTTON_START], now[1] = gp.buttons[GLFW_GAMEPAD_BUTTON_B];
    if (now[0] && !prev[0]) pause_menu_key(w, false);
    else if (now[1] && !prev[1] && P.open) pause_menu_key(w, true);
    prev[0] = now[0], prev[1] = now[1];
}

void pause_menu_end_play(void) {
    memset(&P, 0, sizeof P);
    game_set_paused(false);
}
