// UMG's test (realengine --umg-test, ctest "umg"): a Widget Blueprint made through the API the editor uses (a panel
// with a title, a Text bound to an integer, a Progress Bar bound to a float, a Button whose OnClicked event sets the
// title and the bar), an actor whose BeginPlay creates it and adds it to the viewport; played, then checked: the
// widget on the screen, the bindings shown, a click on the Button running its event.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "platform_files.h"
#include "ui_widget.h"

static bool g_fail;
static Entity g_spawner;

static int pin_of_name(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] umg-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}

static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin_of_name(g, a, 1, ap), b, (uint8_t)pin_of_name(g, b, 0, bpn))) {
        printf("[RealEngine] umg-test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn);
        g_fail = true;
    }
}

static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float slot, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, slot * 300.0f, 0.0f, text, NULL);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}

static void var(BpAsset* a, const char* name, uint8_t type, float def) {
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, BP_NAME, "%s", name);
    v->type = type, v->def.v[0] = def;
}

static int add(UiTree* t, uint8_t kind, int parent, const char* name) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_fail = true, 0;
    snprintf(t->w[i].name, UI_NAME, "%s", name);
    return i;
}

bool umg_test_setup(Editor* ed) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/UmgTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    const char* F = "/Content/UmgTest";
    BpAsset* wb = bp_create(BPA_CLASS, F, "WBP_TestHUD", "User Widget");
    BpAsset* sp = bp_create(BPA_CLASS, F, "BP_TestHUDSpawner", "Actor");
    if (!wb || !sp) return printf("[RealEngine] FAIL: umg-test: the assets could not be made\n"), false;
    var(wb, "Score", BPT_INT, 7.0f);
    var(wb, "Health", BPT_FLOAT, 0.75f);
    // the designer: a panel at the top left (a title, the score, the health), a Button at the bottom center
    wb->ui = calloc(1, sizeof(UiTree));
    if (!wb->ui) return false;
    UiTree* t = wb->ui;
    ui_tree_default(t);
    int panel = add(t, UIW_BORDER, 0, "Panel");
    t->w[panel].size[0] = 460, t->w[panel].size[1] = 190, t->w[panel].padding = 14;
    int box = add(t, UIW_VBOX, panel, "Lines");
    t->w[box].padding = 10;
    int title = add(t, UIW_TEXT, box, "Title");
    snprintf(t->w[title].text, sizeof t->w[title].text, "Rivora Engine HUD");
    t->w[title].font_size = 34, t->w[title].size[1] = 44;
    int score = add(t, UIW_TEXT, box, "ScoreText");
    snprintf(t->w[score].bind_text, UI_NAME, "Score");
    t->w[score].size[1] = 34, t->w[score].color[0] = 1.0f, t->w[score].color[1] = 0.8f, t->w[score].color[2] = 0.3f;
    int bar = add(t, UIW_PROGRESS, box, "HealthBar");
    snprintf(t->w[bar].bind_percent, UI_NAME, "Health");
    t->w[bar].size[1] = 26;
    int button = add(t, UIW_BUTTON, 0, "StartButton");
    t->w[button].anchor[0] = t->w[button].anchor[2] = 0.5f, t->w[button].anchor[1] = t->w[button].anchor[3] = 1.0f;
    t->w[button].align[0] = 0.5f, t->w[button].align[1] = 1.0f;
    t->w[button].pos[0] = 0, t->w[button].pos[1] = -60, t->w[button].size[0] = 260, t->w[button].size[1] = 70;
    t->w[button].bg[0] = 0.85f, t->w[button].bg[1] = 0.42f, t->w[button].bg[2] = 0.1f, t->w[button].bg[3] = 0.95f;
    int label = add(t, UIW_TEXT, button, "StartLabel");
    snprintf(t->w[label].text, sizeof t->w[label].text, "Start");
    t->w[label].font_size = 30;
    // UMG 5.8 (sugestões #59 / #61): an options panel at the top right (a Slider, a Check Box, a Combo Box, an Editable
    // Text, a Spin Box, two Fill buttons in a row), a Scroll Box of lines, a Uniform Grid; a Widget Animation that
    // pulses the title (its scale, looping) played by the widget's Construct (BeginPlay)
    int opts = add(t, UIW_BORDER, 0, "OptionsPanel");
    t->w[opts].anchor[0] = t->w[opts].anchor[2] = 1.0f, t->w[opts].align[0] = 1.0f;
    t->w[opts].pos[0] = -40, t->w[opts].pos[1] = 40, t->w[opts].size[0] = 420, t->w[opts].size[1] = 420, t->w[opts].padding = 16;
    int ob = add(t, UIW_VBOX, opts, "OptionsBox");
    t->w[ob].padding = 8;
    int vol = add(t, UIW_SLIDER, ob, "Volume");
    t->w[vol].value = 0.3f;
    int fsc = add(t, UIW_CHECKBOX, ob, "FullscreenCheck");
    t->w[fsc].h_align = UI_ALIGN_START;
    int q = add(t, UIW_COMBO, ob, "Quality");
    snprintf(t->w[q].text, sizeof t->w[q].text, "Low|Medium|High|Epic");
    t->w[q].value = 2;
    int nm = add(t, UIW_EDIT, ob, "PlayerName");
    snprintf(t->w[nm].text, sizeof t->w[nm].text, "Player");
    int cnt = add(t, UIW_SPIN, ob, "Count");
    t->w[cnt].value = 5;
    int row = add(t, UIW_HBOX, ob, "Buttons");
    t->w[row].size[1] = 50, t->w[row].padding = 8;
    for (int b = 0; b < 2; b++) {
        int bt = add(t, UIW_BUTTON, row, b ? "CancelButton" : "OkButton");
        t->w[bt].size_rule = 1, t->w[bt].fill = 1.0f; // (two halves of the row)
        int bl = add(t, UIW_TEXT, bt, b ? "CancelLabel" : "OkLabel");
        snprintf(t->w[bl].text, sizeof t->w[bl].text, b ? "Cancel" : "OK");
    }
    int sb = add(t, UIW_SCROLL, 0, "Log");
    t->w[sb].anchor[1] = t->w[sb].anchor[3] = 0.5f, t->w[sb].pos[1] = -100, t->w[sb].size[0] = 360, t->w[sb].size[1] = 200;
    t->w[sb].bg[3] = 0.5f;
    for (int ln = 0; ln < 8; ln++) {
        char lname[UI_NAME];
        snprintf(lname, sizeof lname, "LogLine%d", ln);
        int li = add(t, UIW_TEXT, sb, lname);
        snprintf(t->w[li].text, sizeof t->w[li].text, "Line %d of the log", ln + 1);
        t->w[li].size[1] = 34;
    }
    int grid = add(t, UIW_GRID, 0, "Slots");
    t->w[grid].anchor[0] = t->w[grid].anchor[2] = 1.0f, t->w[grid].anchor[1] = t->w[grid].anchor[3] = 1.0f;
    t->w[grid].align[0] = t->w[grid].align[1] = 1.0f, t->w[grid].pos[0] = -40, t->w[grid].pos[1] = -40;
    t->w[grid].size[0] = 300, t->w[grid].size[1] = 150, t->w[grid].columns = 4;
    for (int s = 0; s < 8; s++) {
        char sname[UI_NAME];
        snprintf(sname, sizeof sname, "Slot%d", s);
        int si = add(t, UIW_BORDER, grid, sname);
        t->w[si].slot_pad[0] = t->w[si].slot_pad[1] = t->w[si].slot_pad[2] = t->w[si].slot_pad[3] = 4;
        t->w[si].bg[0] = 0.2f + 0.08f * s, t->w[si].bg[1] = 0.3f, t->w[si].bg[2] = 0.5f, t->w[si].bg[3] = 0.9f;
    }
    // B7: a Rich Text Block in a font of the system (its bold and italic from the family's own files), a List View given
    // 1000 quests when play starts (only the rows on the screen drawn)
    int rich = add(t, UIW_RICH_TEXT, 0, "Story");
    t->w[rich].anchor[1] = t->w[rich].anchor[3] = 1.0f, t->w[rich].align[1] = 1.0f, t->w[rich].pos[0] = 40, t->w[rich].pos[1] = -40;
    t->w[rich].size[0] = 640, t->w[rich].size[1] = 90, t->w[rich].outline = 1.0f;
    snprintf(t->w[rich].font, sizeof t->w[rich].font, "C:/Windows/Fonts/segoeui.ttf");
    snprintf(t->w[rich].text, sizeof t->w[rich].text, "The <b>dragon</b> waits in the <color=#FF6020>red mountain</color>; <i>hurry</i>, <size=36>hero</size>!");
    int quests = add(t, UIW_LIST_VIEW, 0, "Quests");
    t->w[quests].anchor[1] = t->w[quests].anchor[3] = 0.5f, t->w[quests].align[1] = 0.5f, t->w[quests].pos[0] = 40, t->w[quests].pos[1] = 160;
    t->w[quests].size[0] = 300, t->w[quests].size[1] = 240;
    int thr = add(t, UIW_CIRCLE_THROBBER, 0, "Loading");
    t->w[thr].anchor[0] = t->w[thr].anchor[2] = 0.5f, t->w[thr].anchor[1] = t->w[thr].anchor[3] = 0.5f, t->w[thr].align[0] = t->w[thr].align[1] = 0.5f;
    t->w[thr].pos[0] = t->w[thr].pos[1] = 0;
    UiWAnim* an = &t->anims[t->nanims++];
    memset(an, 0, sizeof *an);
    snprintf(an->name, UI_NAME, "Pulse");
    an->length = 0.8f;
    for (int axis = 0; axis < 2; axis++) {
        UiWTrack* trk = &an->tracks[an->ntracks++];
        snprintf(trk->widget, UI_NAME, "Title");
        trk->prop = (uint8_t)(axis ? UIP_SCALE_Y : UIP_SCALE_X);
        ui_track_key(trk, 0.0f, 1.0f), ui_track_key(trk, 0.4f, 1.3f), ui_track_key(trk, 0.8f, 1.0f);
    }
    // its graph: OnClicked_StartButton -> Set Text (Title) "Clicked!" -> Set Percent (HealthBar) 0.25
    BpGraph* g = &wb->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t ev = node(wb, g, BPK_EVENT_CUSTOM, 0, "OnClicked_StartButton");
    uint32_t st = node(wb, g, BPK_WIDGET_SET_TEXT, 1, "Title");
    uint32_t pc = node(wb, g, BPK_WIDGET_SET_PERCENT, 2, "HealthBar");
    snprintf(bp_node(g, st)->pins[pin_of_name(g, st, 0, "In Text")].slit, sizeof bp_node(g, st)->pins[0].slit, "Clicked!");
    bp_node(g, pc)->pins[pin_of_name(g, pc, 0, "In Percent")].lit.v[0] = 0.25f;
    link(g, ev, "", st, "");
    link(g, st, "", pc, "");
    // Construct (BeginPlay) -> Play Animation (Pulse, forever); OnClicked also sets the Slider's value
    uint32_t cbp = node(wb, g, BPK_EVENT_BEGIN_PLAY, 0, NULL);
    uint32_t pa = node(wb, g, BPK_PLAY_WIDGET_ANIM, 1, "Pulse");
    bp_node(g, pa)->pins[pin_of_name(g, pa, 0, "Num Loops To Play")].lit.v[0] = 0.0f; // (0: forever)
    link(g, cbp, "", pa, "");
    uint32_t sv = node(wb, g, BPK_WIDGET_SET_VALUE, 3, "Volume");
    bp_node(g, sv)->pins[pin_of_name(g, sv, 0, "In Value")].lit.v[0] = 0.9f;
    link(g, pc, "", sv, "");
    // the spawner: BeginPlay -> Create Widget (WBP_TestHUD) -> Add to Viewport -> Set Show Mouse Cursor
    BpGraph* sg = &sp->graphs[0];
    while (sg->nnodes) bp_remove_node(sg, sg->nodes[0].id);
    uint32_t beg = node(sp, sg, BPK_EVENT_BEGIN_PLAY, 0, NULL);
    uint32_t cw = node(sp, sg, BPK_CREATE_WIDGET, 1, NULL);
    uint32_t av = node(sp, sg, BPK_ADD_TO_VIEWPORT, 2, NULL);
    uint32_t sm = node(sp, sg, BPK_SHOW_MOUSE, 3, NULL);
    snprintf(bp_node(sg, cw)->pins[pin_of_name(sg, cw, 0, "Class")].slit, sizeof bp_node(sg, cw)->pins[0].slit, "WBP_TestHUD"); // (a class literal: its name)
    bp_node(sg, sm)->pins[pin_of_name(sg, sm, 0, "Show")].lit.v[0] = 1.0f;
    link(sg, beg, "", cw, "");
    link(sg, cw, "", av, "");
    link(sg, cw, "Return Value", av, "Target");
    link(sg, av, "", sm, "");
    if (g_fail || !bp_save(wb) || !bp_save(sp) || !bp_compile(wb) || !bp_compile(sp)) {
        printf("[RealEngine] FAIL: umg-test: the Blueprints didn't compile: %s %s\n", wb->error, sp->error);
        return false;
    }
    printf("[RealEngine] umg-test: the designer tree has %u widgets, %u animations\n", t->n, t->nanims);
    g_spawner = bp_spawn(&ed->world, sp, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
    editor_play(ed, 0.0);
    { // (B7) the List View's 1000 items (Set Text of a joined array: past the widget's own text)
        static char items[16000];
        size_t o = 0;
        for (int i = 0; i < 1000; i++) o += (size_t)snprintf(items + o, sizeof items - o, "%sQuest %d", i ? "|" : "", i + 1);
        if (!ui_test_set_text("Quests", items)) printf("[RealEngine] FAIL: umg-test: the List View isn't on the screen\n");
    }
    return g_spawner != ENTITY_NONE;
}

bool umg_test_check(Editor* ed) {
    World* w = &ed->world;
    const UiStats* s = ui_stats();
    const char* score = ui_test_text("ScoreText");
    bool shown = s->shown == 1 && score && !strcmp(score, "7");
    bool clicked = ui_test_click(w, "StartButton");
    const char* title = ui_test_text("Title");
    bool event = clicked && title && !strcmp(title, "Clicked!");
    printf("[RealEngine] UMG: %u widget on the screen (%u widgets), score \"%s\", the click: title \"%s\"; drawn in %.3f ms\n", s->shown,
           s->widgets, score ? score : "-", title ? title : "-", s->draw_ms);
    if (!shown || !event) printf("[RealEngine] FAIL: UMG: the widget should be shown with its binding, its Button's event should run\n");
    // 5.8's: the click's Set Value on the Slider, the Combo Box's option, the Pulse animation playing on the title
    const UiWidget* vol = ui_test_widget("Volume");
    const UiWidget* q = ui_test_widget("Quality");
    const UiWidget* ti = ui_test_widget("Title");
    bool value = vol && fabsf(vol->value - 0.9f) < 1e-4f;
    bool combo = q && (int)q->value == 2;
    bool pulse = ti && ti->scale[0] > 1.0f + 1e-4f && ti->scale[0] <= 1.3f + 1e-4f;
    printf("[RealEngine] UMG 5.8: slider %.2f, combo index %d, title scale %.3f (Pulse)\n", vol ? vol->value : -1.0f, q ? (int)q->value : -1,
           ti ? ti->scale[0] : -1.0f);
    if (!value || !combo || !pulse) printf("[RealEngine] FAIL: UMG 5.8: Set Value, the Combo Box or the Widget Animation\n");
    // B7: the fonts (the regular, its bold and its italic: three files of the family), the rich text's runs, the list's rows
    bool fonts = s->fonts >= 3, rich = s->rich_runs >= 8, list = s->list_items == 1000 && s->list_rows > 0 && s->list_rows <= 20;
    printf("[RealEngine] UMG B7: %u fonts, %u rich text runs, the List View: %u items, %u rows drawn\n", s->fonts, s->rich_runs, s->list_items, s->list_rows);
    if (!fonts || !rich || !list) printf("[RealEngine] FAIL: UMG B7: the fonts, the Rich Text or the List View\n");
    return shown && event && value && combo && pulse && fonts && rich && list;
}
