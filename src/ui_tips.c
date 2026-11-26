// Descriptions: the hovered widget's text, shown beside it after a moment (ui_tips.h)
#include "ui_tips.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"

extern const UiTip g_ui_tips[];
extern const int g_ui_tip_count;
extern const UiTip g_ui_tips_q5[]; // (Q5, ui_tips_table_q5.c: the controls that had none)
extern const int g_ui_tip_q5_count;
extern const UiTip g_ui_tips_q8[]; // (Q8, ui_tips_table_q8.c: the rigid bodies', joints' and Physical Materials')
extern const int g_ui_tip_q8_count;
extern const UiTip g_ui_tips_q11[]; // (Q11, ui_tips_table_q11.c: the toon's, the outline's, the toon lights')
extern const int g_ui_tip_q11_count;
extern const UiTip g_ui_tips_q12[]; // (Q12, ui_tips_table_q12.c: the First Person Camera's and the Camera Shakes')
extern const int g_ui_tip_q12_count;
extern const UiTip g_ui_tips_q13[]; // (Q13, ui_tips_table_q13.c: the horror kit's)
extern const int g_ui_tip_q13_count;
extern const UiTip g_ui_tips_q14[]; // (Q14, ui_tips_table_q14.c: the splines' and cables')
extern const int g_ui_tip_q14_count;
extern const UiTip g_ui_tips_q15[]; // (Q15, ui_tips_table_q15.c: the PCG's)
extern const int g_ui_tip_q15_count;
extern const UiTip g_ui_tips_q16[]; // (Q16, ui_tips_table_q16.c: everything editable in the game)
extern const int g_ui_tip_q16_count;
extern const UiTip g_ui_tips_q18[]; // (Q18, ui_tips_table_q18.c: UDIM)
extern const int g_ui_tip_q18_count;
extern const UiTip g_ui_tips_q19[]; // (Q19, ui_tips_table_q19.c: the Tlender imported whole, the import report)
extern const int g_ui_tip_q19_count;
extern const UiTip g_ui_tips_q20[]; // (Q20, ui_tips_table_q20.c: the temporal filters' stability options)
extern const int g_ui_tip_q20_count;
extern const UiTip g_ui_tips_q17[]; // (Q17, ui_tips_table_q17.c: Nanite Tessellation)
extern const int g_ui_tip_q17_count;
extern const UiTip g_ui_tips_q21[]; // (Q21, ui_tips_table_q21.c: the distant view)
extern const int g_ui_tip_q21_count;
extern const UiTip g_ui_tips_q25[]; // (Q25, ui_tips_table_q25.c: VR)
extern const int g_ui_tip_q25_count;
extern const UiTip g_ui_tips_q23[]; // (Q23, ui_tips_table_q23.c: the artist tools' fixes)
extern const int g_ui_tip_q23_count;
extern const UiTip g_ui_tips_look[]; // (Q26, ui_tips_table_look.c: the Azul Real look, Platform Support)
extern const int g_ui_tip_look_count;

// the tables as one (built on first use)
#define TIP_MAX 4096 // (Q11: 1934 with its table; 2560 until item 12: 2515 then)
static UiTip g_all[TIP_MAX];
static int g_nall;
static void tables_join(void) {
    if (g_nall) return;
    for (int i = 0; i < g_ui_tip_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips[i];
    for (int i = 0; i < g_ui_tip_q5_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q5[i];
    for (int i = 0; i < g_ui_tip_q8_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q8[i];
    for (int i = 0; i < g_ui_tip_q11_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q11[i];
    for (int i = 0; i < g_ui_tip_q12_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q12[i];
    for (int i = 0; i < g_ui_tip_q13_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q13[i];
    for (int i = 0; i < g_ui_tip_q14_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q14[i];
    for (int i = 0; i < g_ui_tip_q15_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q15[i];
    for (int i = 0; i < g_ui_tip_q16_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q16[i];
    for (int i = 0; i < g_ui_tip_q18_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q18[i];
    for (int i = 0; i < g_ui_tip_q19_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q19[i];
    for (int i = 0; i < g_ui_tip_q20_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q20[i];
    for (int i = 0; i < g_ui_tip_q17_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q17[i];
    for (int i = 0; i < g_ui_tip_q21_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q21[i];
    for (int i = 0; i < g_ui_tip_q25_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q25[i];
    for (int i = 0; i < g_ui_tip_q23_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_q23[i];
    for (int i = 0; i < g_ui_tip_look_count && g_nall < TIP_MAX; i++) g_all[g_nall++] = g_ui_tips_look[i];
}

const UiTip* ui_tips_table(int* count) {
    tables_join();
    *count = g_nall;
    return g_all;
}

// label -> tip: open addressing over the static table, built on first use
#define TIP_CAP 8192 // (4096 until item 12: only the first 2048 of 2452 descriptions were found)
static int16_t g_index[TIP_CAP]; // tip + 1, 0 = empty
static bool g_ready;

static uint32_t hash_n(const char* s, size_t n) {
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static const UiTip* find_n(const char* s, size_t n) {
    if (!g_ready) {
        tables_join();
        for (int i = 0; i < g_nall && i < TIP_CAP / 2; i++) {
            uint32_t k = hash_n(g_all[i].label, strlen(g_all[i].label)) & (TIP_CAP - 1);
            while (g_index[k]) k = (k + 1) & (TIP_CAP - 1);
            g_index[k] = (int16_t)(i + 1);
        }
        g_ready = true;
    }
    uint32_t k = hash_n(s, n) & (TIP_CAP - 1);
    while (g_index[k]) {
        const UiTip* t = &g_all[g_index[k] - 1];
        if (strncmp(t->label, s, n) == 0 && t->label[n] == '\0') return t;
        k = (k + 1) & (TIP_CAP - 1);
    }
    return NULL;
}

// (2026-11-25, item 12) the Spanish ones: their own index
static const UiTipEs* g_es;
static int g_nes;
static int16_t g_index_es[TIP_CAP];
void ui_tips_set_es(const UiTipEs* t, int count) {
    g_es = t, g_nes = count;
    memset(g_index_es, 0, sizeof g_index_es);
    for (int i = 0; i < count && i < TIP_CAP / 2; i++) {
        uint32_t k = hash_n(t[i].label, strlen(t[i].label)) & (TIP_CAP - 1);
        while (g_index_es[k]) k = (k + 1) & (TIP_CAP - 1);
        g_index_es[k] = (int16_t)(i + 1);
    }
}
const char* ui_tip_es(const char* label) {
    if (!g_es || !label) return NULL;
    uint32_t k = hash_n(label, strlen(label)) & (TIP_CAP - 1);
    while (g_index_es[k]) {
        const UiTipEs* t = &g_es[g_index_es[k] - 1];
        if (!strcmp(t->label, label)) return t->text;
        k = (k + 1) & (TIP_CAP - 1);
    }
    return NULL;
}

static const char* tip_text(const UiTip* t) {
    int lang = i18n_language();
    const char* s = lang == LANG_PT_BR ? t->text[1] : lang == LANG_ES ? ui_tip_es(t->label) : NULL;
    return i18n_terms(s && s[0] ? s : t->text[0]); // (2026-11-25, item 13: "Graph" / "Grafo" on screen)
}

const char* ui_tip_find(const char* label) {
    if (!label) return NULL;
    // the visible part: the menus' leading spaces (their icon's room) and the ##id off
    const char* id = strstr(label, "##");
    size_t n = id ? (size_t)(id - label) : strlen(label);
    while (n && *label == ' ') label++, n--;
    while (n && label[n - 1] == ' ') n--;
    if (n) {
        char vis[160];
        if (n >= sizeof vis) n = sizeof vis - 1;
        memcpy(vis, label, n);
        vis[n] = '\0';
        const char* m = i18n_msgid(vis); // the English msgid of a translated label
        const UiTip* t = find_n(m, strlen(m));
        if (!t && m != vis) t = find_n(vis, n);
        if (t) return tip_text(t);
    }
    if (id) { // an icon's button: its id ("##bp_save", "###Exposure"); the table keeps it bare ("bp_save") or as "##bp_save"
        const char* hashes = id;
        while (*id == '#') id++;
        const UiTip* t = find_n(id, strlen(id));
        if (!t && id - hashes >= 2) t = find_n(id - 2, strlen(id - 2));
        if (t) return tip_text(t);
    }
    return NULL;
}

// --- (Q5) The standard tooltip (notas-dev/icones-e-tooltips.md) ---

static UiTipIconFn g_icon_fn;
static ImFont* const* g_bold;
static bool g_editor;
void ui_tips_editor(UiTipIconFn icon_fn, ImFont* const* bold) { g_icon_fn = icon_fn, g_bold = bold, g_editor = true; }

// the rows of this frame (label_cell, ui_prop_label): a field without its own description takes its row's
typedef struct TipRow { const char* label; float y0, y1, x1, wx0, wx1; } TipRow;
static TipRow g_rows[2][256];
static int g_nrows[2], g_rows_frame;

void ui_tip_row(const char* label) {
    if (!label || !label[0]) return;
    int f = igGetFrameCount();
    if (f != g_rows_frame) { // (a new frame: last frame's rows kept for the lookup at its end)
        g_rows_frame = f;
        int b = f & 1;
        g_nrows[b] = 0;
    }
    int b = f & 1;
    if (g_nrows[b] >= 256) return;
    ImVec2 a = igGetItemRectMin(), c = igGetItemRectMax(), wp = igGetWindowPos(), ws = igGetWindowSize();
    TipRow* r = &g_rows[b][g_nrows[b]++];
    r->label = label, r->y0 = a.y, r->y1 = c.y, r->x1 = c.x, r->wx0 = wp.x, r->wx1 = wp.x + ws.x;
}

static const char* row_of(const UiHover* h) {
    if (g_rows_frame != igGetFrameCount()) return NULL;
    int b = igGetFrameCount() & 1;
    float cy = (h->y0 + h->y1) * 0.5f;
    for (int i = g_nrows[b] - 1; i >= 0; i--) {
        const TipRow* r = &g_rows[b][i];
        if (r->wx0 != h->win_x0 || r->wx1 != h->win_x1) continue; // (the same window)
        if (cy >= r->y0 - 3.0f && cy <= r->y1 + 3.0f && r->x1 <= h->x0 + 2.0f) return r->label;
    }
    return NULL;
}

// "(Ctrl+S)", "(F7)", "(W)", "Ctrl+Shift+P": the first shortcut in a text, into out; its span in the text
static bool key_token(const char* s, int* n) {
    static const char* const words[] = { "Ctrl", "Shift", "Alt", "Del", "Delete", "Esc", "Space", "Tab", "Enter", "Home", "End",
                                         "Backspace", "PgUp", "PgDn", "Up", "Down", "Left", "Right", "Wheel", "Cmd", NULL };
    for (int i = 0; words[i]; i++) {
        size_t k = strlen(words[i]);
        if (!strncmp(s, words[i], k) && !((s[k] >= 'a' && s[k] <= 'z'))) return *n = (int)k, true;
    }
    if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') return *n = (s[2] >= '0' && s[2] <= '9') ? 3 : 2, true;
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9') || s[0] == '+' || s[0] == '-' || s[0] == '=') &&
        !((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z')))
        return *n = 1, true;
    return false;
}
static bool parse_shortcut(const char* s, char* out, size_t cap, const char** at, const char** end) {
    for (const char* p = s; p && *p; p++) {
        if (*p != '(') continue;
        const char* q = p + 1;
        int parts = 0, n;
        bool mod = false;
        while (key_token(q, &n)) {
            mod |= !strncmp(q, "Ctrl", 4) || !strncmp(q, "Shift", 5) || !strncmp(q, "Alt", 3);
            q += n, parts++;
            if (*q == '+' && q[1] && q[1] != ')') q++;
            else break;
        }
        if (parts && *q == ')' && (mod || parts == 1)) {
            size_t len = (size_t)(q - p - 1);
            if (len >= cap) len = cap - 1;
            memcpy(out, p + 1, len), out[len] = 0;
            if (at) *at = p;
            if (end) *end = q + 1;
            return true;
        }
    }
    return false;
}

static ImGuiWindow* last_item_window(void) { return igGetCurrentWindow(); }

void ui_tip_item(int icon, const char* name, const char* desc, const char* shortcut) {
    if (!igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    UiHover* h = &g_ui_hover;
    int f = igGetFrameCount();
    ImVec2 a = igGetItemRectMin(), b = igGetItemRectMax();
    if (h->frame != f || h->x0 != a.x || h->y0 != a.y) { // (no hover reported for this item: one made like ui_tip_last_item's)
        ImGuiWindow* w = last_item_window();
        const ImGuiViewport* vp = igGetWindowViewport();
        uint32_t id = igGetItemID();
        if (!id) id = 2166136261u ^ (uint32_t)(a.x * 13.0f) ^ (uint32_t)(a.y * 7.0f);
        if (h->id != id) h->label[0] = 0;
        h->id = id, h->frame = f, h->window_flags = (int)w->Flags;
        h->x0 = a.x, h->y0 = a.y, h->x1 = b.x, h->y1 = b.y;
        h->win_x0 = w->Pos.x, h->win_x1 = w->Pos.x + w->Size.x;
        h->vp_x0 = vp->WorkPos.x, h->vp_y0 = vp->WorkPos.y, h->vp_x1 = vp->WorkPos.x + vp->WorkSize.x, h->vp_y1 = vp->WorkPos.y + vp->WorkSize.y;
    }
    if (h->info_frame != f) h->icon = -1, h->name[0] = h->desc[0] = h->shortcut[0] = 0;
    h->info_frame = f;
    if (icon >= 0) h->icon = icon;
    if (name && name[0]) snprintf(h->name, sizeof h->name, "%s", name);
    if (desc && desc[0]) snprintf(h->desc, sizeof h->desc, "%s", desc);
    if (shortcut && shortcut[0]) snprintf(h->shortcut, sizeof h->shortcut, "%s", shortcut);
}

// The visible name of a label: no "##id", no menu's leading spaces, no "(Ctrl+O)" (that goes to the shortcut)
static void visible_name(const char* label, char* out, size_t cap, char* sc, size_t sc_cap) {
    const char* id = strstr(label, "##");
    size_t n = id ? (size_t)(id - label) : strlen(label);
    while (n && *label == ' ') label++, n--;
    while (n && label[n - 1] == ' ') n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, label, n), out[n] = 0;
    const char *at, *end;
    char k[40];
    if (parse_shortcut(out, k, sizeof k, &at, &end) && !end[0]) {
        if (!sc[0]) snprintf(sc, sc_cap, "%s", k);
        size_t m = (size_t)(at - out);
        while (m && out[m - 1] == ' ') m--;
        out[m] = 0;
    }
}

static struct { uint32_t id; char name[96], shortcut[40]; const char* text; int icon; double since; } g_shown;

const char* ui_tip_hovered_name(void) {
    const UiHover* h = &g_ui_hover;
    if (h->frame < igGetFrameCount() - 1 || !h->id || h->id != g_shown.id || !g_shown.text) return NULL;
    return g_shown.name[0] ? g_shown.name : NULL;
}

void ui_tips_frame(void) {
    UiHover* h = &g_ui_hover;
    int frame = igGetFrameCount();
    double now = igGetTime();
    if (h->frame != frame || !h->id) {
        g_shown.id = 0;
        return;
    }
    bool info = h->info_frame == frame;
    if (h->id != g_shown.id) { // a new widget under the mouse: its parts looked up once
        g_shown.id = h->id;
        g_shown.since = now;
        g_shown.icon = info ? h->icon : -1;
        g_shown.shortcut[0] = 0;
        if (info && h->shortcut[0]) snprintf(g_shown.shortcut, sizeof g_shown.shortcut, "%s", h->shortcut);
        g_shown.text = ui_tip_find(h->label);
        if (!g_shown.text && info && h->desc[0]) g_shown.text = h->desc;
        char nm[96] = "";
        if (info && h->name[0]) snprintf(nm, sizeof nm, "%s", h->name);
        visible_name(nm[0] ? nm : h->label, g_shown.name, sizeof g_shown.name, g_shown.shortcut, sizeof g_shown.shortcut);
        if (!g_shown.text) { // a field in a Details row: its row's
            const char* row = row_of(h);
            if (row) {
                g_shown.text = ui_tip_find(row);
                if (g_shown.text && !g_shown.name[0]) snprintf(g_shown.name, sizeof g_shown.name, "%s", tr(row));
            }
        }
        if (g_shown.text == h->desc) { // (a copy: the hover's buffer changes)
            static char keep[512];
            snprintf(keep, sizeof keep, "%s", h->desc);
            g_shown.text = keep;
        }
        if (g_shown.text && !g_shown.shortcut[0]) parse_shortcut(g_shown.text, g_shown.shortcut, sizeof g_shown.shortcut, NULL, NULL);
        if (!g_shown.text && g_shown.name[0] && info && (h->name[0] || h->icon >= 0)) g_shown.text = ""; // (a name alone: still the card)
    } else if (info && h->desc[0] && g_shown.text && !g_shown.text[0]) {
        static char keep2[512];
        snprintf(keep2, sizeof keep2, "%s", h->desc);
        g_shown.text = keep2;
    }
    const char* text = g_shown.text;
    ImGuiIO* io = igGetIO_Nil();
    if (io->MouseDown[0] || io->MouseDown[1] || io->MouseDown[2] || igIsAnyItemActive()) g_shown.since = now; // (a press restarts it)
    if (!text || now - g_shown.since < 0.55 || h->tooltip_frame == frame) return;
    // beside the widget: right of a menu (left of it near the screen's edge), else under it (above it near the bottom)
    float fs = igGetFontSize(), gap = 4.0f;
    ImVec2 pos, pivot;
    if (h->window_flags & (ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Popup)) {
        bool left = h->win_x1 + fs * 24.0f > h->vp_x1;
        pos = (ImVec2){ left ? h->win_x0 - gap : h->win_x1 + gap, h->y0 };
        pivot = (ImVec2){ left ? 1.0f : 0.0f, 0.0f };
    } else {
        bool above = h->y1 + fs * 8.0f > h->vp_y1;
        float x = io->MousePos.x;
        if (x > h->vp_x1 - fs * 22.0f) x = h->vp_x1 - fs * 22.0f;
        if (x < h->vp_x0) x = h->vp_x0;
        pos = (ImVec2){ x, above ? h->y0 - gap : h->y1 + gap };
        pivot = (ImVec2){ 0.0f, above ? 1.0f : 0.0f };
    }
    igSetNextWindowPos(pos, ImGuiCond_Always, pivot);
    if (!igBeginTooltip()) return;
    // the card: [icon] NAME .............. [shortcut]
    //           the description, wrapped
    //           F1: more in the Assistant
    ImDrawList* dl = igGetWindowDrawList();
    float wrap = fs * 24.0f, is = fs * 1.25f;
    bool head = g_shown.name[0] || g_shown.icon >= 0;
    if (head) {
        ImVec2 p = igGetCursorScreenPos();
        float x = p.x;
        if (g_shown.icon >= 0 && g_icon_fn) {
            g_icon_fn(dl, g_shown.icon, x, p.y + (fs * 1.25f - is) * 0.5f, is, igGetColorU32_Col(ImGuiCol_Text, 1.0f));
            x += is + fs * 0.45f;
        }
        igSetCursorScreenPos((ImVec2){ x, p.y + (is - fs) * 0.5f });
        ImFont* bf = g_bold ? *g_bold : NULL;
        if (bf) igPushFont(bf, 0.0f);
        igTextUnformatted(g_shown.name[0] ? g_shown.name : "", NULL);
        if (bf) igPopFont();
        if (g_shown.shortcut[0]) { // a key cap on the right
            ImVec2 ts = igCalcTextSize(g_shown.shortcut, NULL, false, -1);
            igSameLine(0, fs * 1.2f);
            ImVec2 k = igGetCursorScreenPos();
            float kw = ts.x + fs * 0.7f;
            ImDrawList_AddRectFilled(dl, (ImVec2){ k.x, k.y - 1.0f }, (ImVec2){ k.x + kw, k.y + fs + 3.0f }, igGetColorU32_Col(ImGuiCol_FrameBg, 1.0f), 4.0f, 0);
            ImDrawList_AddRect(dl, (ImVec2){ k.x, k.y - 1.0f }, (ImVec2){ k.x + kw, k.y + fs + 3.0f }, igGetColorU32_Col(ImGuiCol_Border, 1.0f), 4.0f, 1.0f, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ k.x + fs * 0.35f, k.y + 1.0f }, igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f), g_shown.shortcut, NULL);
            igDummy((ImVec2){ kw, fs });
        }
        igSetCursorScreenPos((ImVec2){ p.x, fmaxf(igGetCursorScreenPos().y, p.y + is + 2.0f) });
    }
    if (text[0]) {
        if (head) igDummy((ImVec2){ 0.0f, 1.0f });
        igPushTextWrapPos(igGetCursorPosX() + wrap);
        igTextUnformatted(text, NULL);
        igPopTextWrapPos();
    }
    if (g_editor && head && g_shown.name[0]) {
        igPushStyleColor_U32(ImGuiCol_Text, igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f));
        igTextUnformatted(tr("F1: more in the Assistant"), NULL);
        igPopStyleColor(1);
    }
    igEndTooltip();
}

void ui_tip_last_item(const char* label) {
    ui_tip_row(label); // (Q5: the fields after it in its row take its description)
    if (!igIsItemHovered(0)) return;
    UiHover* h = &g_ui_hover;
    ImVec2 a = igGetItemRectMin(), b = igGetItemRectMax(), wp = igGetWindowPos(), ws = igGetWindowSize();
    const ImGuiViewport* vp = igGetWindowViewport();
    uint32_t id = 2166136261u; // (a text has no ImGui id: its label's hash and place)
    for (const char* c = label; *c; c++) id = (id ^ (uint8_t)*c) * 16777619u;
    id ^= (uint32_t)(a.y * 7.0f);
    if (h->id != id || h->frame != igGetFrameCount() - 1) snprintf(h->label, sizeof h->label, "%s", label);
    h->id = id ? id : 1;
    h->frame = igGetFrameCount();
    h->window_flags = 0;
    h->x0 = a.x, h->y0 = a.y, h->x1 = b.x, h->y1 = b.y;
    h->win_x0 = wp.x, h->win_x1 = wp.x + ws.x;
    h->vp_x0 = vp->WorkPos.x, h->vp_y0 = vp->WorkPos.y, h->vp_x1 = vp->WorkPos.x + vp->WorkSize.x, h->vp_y1 = vp->WorkPos.y + vp->WorkSize.y;
}

const char* ui_prop_label(const char* label) {
    ImGuiWindow* w = igGetCurrentWindow();
    if (!label || w->SkipItems || w->DC.IsSameLine || igGetCurrentTable()) return label; // (in a row of its own only)
    const char* end = strstr(label, "##");
    size_t n = end ? (size_t)(end - label) : strlen(label);
    float avail = igGetContentRegionAvail().x;
    if (!n || avail < igGetFontSize() * 10.0f) return label;
    float x0 = igGetCursorPosX(), col = avail * 0.4f;
    igAlignTextToFramePadding();
    ImVec2 p = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_PushClipRect(dl, p, (ImVec2){ p.x + col - 6.0f, p.y + igGetFrameHeight() }, true); // (a long name cut at the column)
    igTextUnformatted(label, label + n);
    ImDrawList_PopClipRect(dl);
    ui_tip_last_item(label);
    igSameLine(x0 + col, 0.0f);
    igSetNextItemWidth(-FLT_MIN);
    static char ring[16][192];
    static unsigned k;
    char* b = ring[k++ & 15];
    snprintf(b, sizeof ring[0], "##%s", label); // (hidden: the name is drawn; the id stays unique)
    return b;
}
