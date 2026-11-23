// The Game Rules' example widgets (game_rules.h, notas-dev/regras-de-jogo.md): WBP_MatchHUD (the clock, the score, the
// lives, the round, the countdown) and WBP_MatchEnd (Victory or Defeat, why, the score, Play Again, Main Menu). Made in
// Content/UI when missing, as ordinary Widget Blueprints: their graphs use the Game Rules' nodes (Event Tick -> Get Time
// Remaining / Get Score / Get Lives / Get Match Phase / Get Match Result -> Set Text / Set Widget Visible), so the user
// opens, changes and copies them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "game_rules.h"
#include "platform_files.h"
#include "ui_widget.h"

static bool g_bad;

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    g_bad = true;
    return 0;
}
static void L(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!a || !b || !bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) g_bad = true;
}
static uint32_t N(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, x * 300.0f, y * 150.0f, text, NULL);
    if (!n) g_bad = true;
    return n ? n->id : 0;
}
static void lit_f(BpGraph* g, uint32_t id, const char* pn, float v) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, pn);
    if (n) n->pins[p].lit.v[0] = v;
}
static void lit_s(BpGraph* g, uint32_t id, const char* pn, const char* s) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, pn);
    if (n) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}
static void chain(BpGraph* g, const uint32_t* ids, int n) {
    for (int i = 0; i + 1 < n; i++) L(g, ids[i], "", ids[i + 1], "");
}

// a widget: anchored at (ax, ay) of the screen, its pivot (px, py), at pos, of size; a text's font (0: not a text)
static int W(UiTree* t, uint8_t kind, int parent, const char* name, float ax, float ay, float px, float py, float x, float y, float wd, float ht) {
    int i = ui_tree_add(t, kind, parent);
    if (i < 0) return g_bad = true, 0;
    UiWidget* u = &t->w[i];
    snprintf(u->name, UI_NAME, "%s", name);
    u->anchor[0] = u->anchor[2] = ax, u->anchor[1] = u->anchor[3] = ay;
    u->align[0] = px, u->align[1] = py;
    u->pos[0] = x, u->pos[1] = y, u->size[0] = wd, u->size[1] = ht;
    return i;
}
static int T(UiTree* t, const char* name, const char* text, float font, float ax, float ay, float px, float py, float x, float y, float wd, float ht) {
    int i = W(t, UIW_TEXT, 0, name, ax, ay, px, py, x, y, wd, ht);
    UiWidget* u = &t->w[i];
    snprintf(u->text, sizeof u->text, "%s", text);
    u->font_size = font, u->bold = font >= 30.0f, u->shadow = 1;
    u->color[0] = u->color[1] = u->color[2] = u->color[3] = 1.0f;
    return i;
}
static void rgba(float* c, float r, float g, float b, float a) { c[0] = r, c[1] = g, c[2] = b, c[3] = a; }

static BpAsset* begin(const char* name) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    BpAsset* a = bp_create(BPA_CLASS, "/Content/UI", name, "User Widget");
    if (!a) return NULL;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return NULL;
    ui_tree_default(a->ui);
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    g_bad = false;
    return a;
}
static uint32_t finish(BpAsset* a) {
    if (g_bad) printf("[RealEngine] Game Rules: %s was made with errors\n", a->name);
    if (!bp_compile(a)) printf("[RealEngine] Game Rules: %s: %s\n", a->name, a->error);
    return bp_save(a) ? a->hash : 0;
}

uint32_t rules_hud_asset(void) {
    BpAsset* a = bp_load("/Content/UI/WBP_MatchHUD.rbp");
    if (a) return a->hash;
    if (!ui_content_root()[0] || !(a = begin("WBP_MatchHUD"))) return 0;
    UiTree* t = a->ui;
    // the top: a dark band with the clock in the middle, the score at the left, the lives at the right, the round under it
    int band = W(t, UIW_BORDER, 0, "Band", 0.5f, 0.0f, 0.5f, 0.0f, 0, 16, 640, 92);
    rgba(t->w[band].bg, 0.02f, 0.02f, 0.025f, 0.55f), t->w[band].radius = 10.0f;
    T(t, "TimeText", "5:00", 46, 0.5f, 0.0f, 0.5f, 0.0f, 0, 22, 200, 60);
    T(t, "ScoreLabel", "Score", 18, 0.5f, 0.0f, 0.5f, 0.0f, -230, 26, 140, 24);
    T(t, "ScoreValue", "0", 34, 0.5f, 0.0f, 0.5f, 0.0f, -230, 50, 140, 44);
    T(t, "LivesLabel", "Lives", 18, 0.5f, 0.0f, 0.5f, 0.0f, 230, 26, 140, 24);
    T(t, "LivesValue", "3", 34, 0.5f, 0.0f, 0.5f, 0.0f, 230, 50, 140, 44);
    T(t, "RoundLabel", "Round", 16, 0.5f, 0.0f, 1.0f, 0.0f, -6, 112, 120, 24);
    T(t, "RoundValue", "1 / 1", 16, 0.5f, 0.0f, 0.0f, 0.0f, 6, 112, 120, 24);
    int cd = T(t, "CountdownText", "3", 150, 0.5f, 0.5f, 0.5f, 0.5f, 0, -40, 400, 220);
    rgba(t->w[cd].color, 1.0f, 0.86f, 0.35f, 1.0f);
    int st = T(t, "StatusText", "Round Over", 56, 0.5f, 0.5f, 0.5f, 0.5f, 0, -140, 900, 90);
    rgba(t->w[st].color, 0.85f, 0.92f, 1.0f, 1.0f);
    // Event Tick: the Game Rules' values into the texts, the countdown and the round's end shown only in their phases
    BpGraph* g = &a->graphs[0];
    uint32_t tick = N(a, g, BPK_EVENT_TICK, 0, 0, NULL);
    uint32_t tm = N(a, g, BPK_GR_TIME, 0, 2, NULL), sc = N(a, g, BPK_GR_GET_SCORE, 0, 3, NULL), lv = N(a, g, BPK_GR_GET_LIVES, 0, 4, NULL);
    uint32_t ph = N(a, g, BPK_GR_GET_PHASE, 0, 5, NULL);
    uint32_t s_time = N(a, g, BPK_WIDGET_SET_TEXT, 1, 0, "TimeText"), s_score = N(a, g, BPK_WIDGET_SET_TEXT, 2, 0, "ScoreValue");
    uint32_t s_lives = N(a, g, BPK_WIDGET_SET_TEXT, 3, 0, "LivesValue");
    uint32_t v_ll = N(a, g, BPK_WIDGET_SET_VISIBLE, 4, 0, "LivesLabel"), v_lv = N(a, g, BPK_WIDGET_SET_VISIBLE, 5, 0, "LivesValue");
    uint32_t not_inf = N(a, g, BPK_NOT, 2, 4, NULL);
    uint32_t fr = N(a, g, BPK_FORMAT_TEXT, 2, 6, NULL), s_round = N(a, g, BPK_WIDGET_SET_TEXT, 6, 0, "RoundValue");
    uint32_t many = N(a, g, BPK_IGREATER, 2, 7, NULL);
    uint32_t v_rl = N(a, g, BPK_WIDGET_SET_VISIBLE, 7, 0, "RoundLabel"), v_rv = N(a, g, BPK_WIDGET_SET_VISIBLE, 8, 0, "RoundValue");
    uint32_t ceil_n = N(a, g, BPK_CEIL, 2, 8, NULL), s_cd = N(a, g, BPK_WIDGET_SET_TEXT, 9, 0, "CountdownText");
    uint32_t is_cd = N(a, g, BPK_IEQUAL, 2, 9, NULL), v_cd = N(a, g, BPK_WIDGET_SET_VISIBLE, 10, 0, "CountdownText");
    uint32_t s_st = N(a, g, BPK_WIDGET_SET_TEXT, 11, 0, "StatusText");
    uint32_t is_ro = N(a, g, BPK_IEQUAL, 2, 10, NULL), v_st = N(a, g, BPK_WIDGET_SET_VISIBLE, 12, 0, "StatusText");
    uint32_t ex[] = { tick, s_time, s_score, s_lives, v_ll, v_lv, s_round, v_rl, v_rv, s_cd, v_cd, s_st, v_st };
    chain(g, ex, (int)(sizeof ex / sizeof ex[0]));
    L(g, tm, "Text", s_time, "In Text");
    L(g, sc, "Return Value", s_score, "In Text");
    L(g, lv, "Return Value", s_lives, "In Text");
    L(g, lv, "Infinite", not_inf, "A"), L(g, not_inf, "", v_ll, "Visible"), L(g, not_inf, "", v_lv, "Visible");
    lit_s(g, fr, "Format", "{0} / {1}");
    L(g, ph, "Round", fr, "{0}"), L(g, ph, "Rounds", fr, "{1}"), L(g, fr, "", s_round, "In Text");
    L(g, ph, "Rounds", many, "A"), lit_f(g, many, "B", 1.0f), L(g, many, "", v_rl, "Visible"), L(g, many, "", v_rv, "Visible");
    L(g, ph, "Phase Time Left", ceil_n, "A"), L(g, ceil_n, "", s_cd, "In Text");
    L(g, ph, "Return Value", is_cd, "A"), lit_f(g, is_cd, "B", (float)MATCH_COUNTDOWN), L(g, is_cd, "", v_cd, "Visible");
    L(g, ph, "Phase Name", s_st, "In Text");
    L(g, ph, "Return Value", is_ro, "A"), lit_f(g, is_ro, "B", (float)MATCH_ROUND_OVER), L(g, is_ro, "", v_st, "Visible");
    return finish(a);
}

uint32_t rules_end_asset(void) {
    BpAsset* a = bp_load("/Content/UI/WBP_MatchEnd.rbp");
    if (a) return a->hash;
    if (!ui_content_root()[0] || !(a = begin("WBP_MatchEnd"))) return 0;
    UiTree* t = a->ui;
    int dim = W(t, UIW_BORDER, 0, "Dim", 0, 0, 0, 0, 0, 0, 0, 0);
    t->w[dim].anchor[0] = t->w[dim].anchor[1] = 0.0f, t->w[dim].anchor[2] = t->w[dim].anchor[3] = 1.0f;
    rgba(t->w[dim].bg, 0, 0, 0, 0.6f), t->w[dim].radius = 0.0f;
    int panel = W(t, UIW_BORDER, 0, "Panel", 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 620, 470);
    rgba(t->w[panel].bg, 0.045f, 0.05f, 0.06f, 0.95f), t->w[panel].radius = 14.0f;
    t->w[panel].border_w = 1.0f, rgba(t->w[panel].border_col, 1, 1, 1, 0.08f);
    int vt = T(t, "VictoryText", "Victory", 72, 0.5f, 0.5f, 0.5f, 0.5f, 0, -150, 560, 100);
    rgba(t->w[vt].color, 1.0f, 0.82f, 0.30f, 1.0f);
    int dt = T(t, "DefeatText", "Defeat", 72, 0.5f, 0.5f, 0.5f, 0.5f, 0, -150, 560, 100);
    rgba(t->w[dt].color, 0.95f, 0.30f, 0.26f, 1.0f);
    int rs = T(t, "ReasonText", "Score Target Reached", 26, 0.5f, 0.5f, 0.5f, 0.5f, 0, -78, 560, 40);
    rgba(t->w[rs].color, 0.80f, 0.84f, 0.90f, 1.0f);
    T(t, "ScoreLabel", "Score", 22, 0.5f, 0.5f, 1.0f, 0.5f, -10, -10, 200, 40);
    T(t, "ScoreValue", "0", 40, 0.5f, 0.5f, 0.0f, 0.5f, 10, -10, 200, 56);
    static const struct { const char* name; const char* label; float y; } bt[] = { { "PlayAgainButton", "Play Again", 80 }, { "MenuButton", "Main Menu", 160 } };
    for (int k = 0; k < 2; k++) {
        int b = W(t, UIW_BUTTON, 0, bt[k].name, 0.5f, 0.5f, 0.5f, 0.5f, 0, bt[k].y, 300, 62);
        t->w[b].radius = 8.0f;
        rgba(t->w[b].hover, 0.20f, 0.42f, 0.78f, 0.95f), rgba(t->w[b].press, 0.12f, 0.30f, 0.62f, 1.0f);
        char tn[UI_NAME];
        snprintf(tn, sizeof tn, "%sLabel", bt[k].name);
        int l = ui_tree_add(t, UIW_TEXT, b);
        if (l < 0) {
            g_bad = true;
            continue;
        }
        UiWidget* u = &t->w[l];
        snprintf(u->name, UI_NAME, "%s", tn), snprintf(u->text, sizeof u->text, "%s", bt[k].label);
        u->font_size = 26.0f, u->bold = 1, u->align[0] = u->align[1] = 0.5f;
        u->color[0] = u->color[1] = u->color[2] = u->color[3] = 1.0f;
    }
    // Event Tick: Get Match Result -> Victory or Defeat shown, the reason, the score; the buttons: Restart Match, Main Menu
    BpGraph* g = &a->graphs[0];
    uint32_t tick = N(a, g, BPK_EVENT_TICK, 0, 0, NULL), res = N(a, g, BPK_GR_RESULT, 0, 2, NULL), sc = N(a, g, BPK_GR_GET_SCORE, 0, 3, NULL);
    uint32_t v_won = N(a, g, BPK_WIDGET_SET_VISIBLE, 1, 0, "VictoryText"), v_lost = N(a, g, BPK_WIDGET_SET_VISIBLE, 2, 0, "DefeatText");
    uint32_t not_won = N(a, g, BPK_NOT, 1, 2, NULL);
    uint32_t s_why = N(a, g, BPK_WIDGET_SET_TEXT, 3, 0, "ReasonText"), s_sc = N(a, g, BPK_WIDGET_SET_TEXT, 4, 0, "ScoreValue");
    uint32_t ex[] = { tick, v_won, v_lost, s_why, s_sc };
    chain(g, ex, 5);
    L(g, res, "Won", v_won, "Visible"), L(g, res, "Won", not_won, "A"), L(g, not_won, "", v_lost, "Visible");
    L(g, res, "Reason", s_why, "In Text"), L(g, sc, "Return Value", s_sc, "In Text");
    uint32_t again[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 5, "OnClicked_PlayAgainButton"), N(a, g, BPK_GR_RESTART, 1, 5, NULL) };
    chain(g, again, 2);
    uint32_t menu[2] = { N(a, g, BPK_EVENT_CUSTOM, 0, 6, "OnClicked_MenuButton"), N(a, g, BPK_MAIN_MENU, 1, 6, NULL) };
    chain(g, menu, 2);
    return finish(a);
}
