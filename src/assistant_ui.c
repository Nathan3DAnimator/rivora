// The Assistant's window (assistant.h, notas-dev/assistente.md): Help > Assistant (F1 outside the Code Editor). A
// question (answers while typing; Enter fetches the base the first time), the answers (articles, functions, controls),
// the chosen one read (Markdown: headings, steps, points, code with Copy, its actions as buttons). "Show me where"
// outlines the controls on the screen (ItemAdd's labels: imgui_hooks.c) for a few seconds.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "assistant.h"
#include "theme.h"
#include "assistant_ui.h"
#include "assistant_draw.h"
#include "script_ide.h"
#include "async.h"
#include "crashlog.h"
#include "i18n.h"
#include "platform.h"
#include "script.h"
#include "ui_icons.h"

extern int GREHighlight;
extern char g_re_hl_label[4][128];
extern int g_re_hl_nlabel;
extern float g_re_hl_rect[8][4];
extern int g_re_hl_nrect, g_re_hl_frame;
extern struct ImFont* g_code_font;

#define MAX_HITS 24

static struct {
    bool open, focus_input, asked_fetch;
    char q[256], last_q[256];
    AsHit hits[MAX_HITS];
    int nhits, sel, ndocs_seen;
    bool weak, searched;
    int history[16], nhist; // (the articles opened by links: Back)
    double hl_until;
    char hl_path[256];
    AssistantActionFn action;
    char unanswered_sent[256];
    // how a recipe shows (one at a time): the user's choice (kept in editor.ini), the question's when it says one
    int variant, asked_variant;
    char want[64]; // an article asked for before the base arrived (Help links, --show assistant:<id>)
} U = { .variant = 0, .asked_variant = -1 };

// The variants an article's code comes in (its fences' tags): Blueprint, Real, C#, Python, C
enum { V_BLUEPRINT, V_REAL, V_CS, V_PY, V_C, V_COUNT };
static const char* const g_vtag[V_COUNT] = { "blueprint", "real", "cs", "py", "c" };
static const char* const g_vname[V_COUNT] = { "Blueprint", "Rivora Script", "C#", "Python", "C" };
static int tag_variant(const char* tag) { // ("```py", "```cs"...: -1 another tag, or none)
    for (int v = 0; v < V_COUNT; v++) {
        size_t n = strlen(g_vtag[v]);
        if (!strncmp(tag, g_vtag[v], n) && (tag[n] == 0 || tag[n] == '\n' || tag[n] == '\r' || tag[n] == ' ')) return v;
    }
    return -1;
}
// the variant a question asks for: its words ("em python", "com blueprint", "c#") or its code (def, self., {, ;)
static int detect_variant(const char* q) {
    char s[300];
    size_t n = 0;
    s[n++] = ' ';
    for (const char* c = q; *c && n + 2 < sizeof s; c++) s[n++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    s[n++] = ' ', s[n] = 0;
    if (strstr(s, "python") || strstr(s, " py ") || strstr(s, "def ") || strstr(s, "self.") || strstr(s, "dataclass")) return V_PY;
    if (strstr(s, "c#") || strstr(s, "csharp") || strstr(s, "c sharp") || strstr(s, "unity") || strstr(s, "public ") || strstr(s, " cs "))
        return V_CS;
    if (strstr(s, "blueprint") || strstr(s, " bp ") || strstr(s, " bps ") || strstr(s, " nos ") || strstr(s, " n\xc3\xb3s ") ||
        strstr(s, " node") || strstr(s, "visual"))
        return V_BLUEPRINT;
    if (strstr(s, "linguagem real") || strstr(s, "real language") || strstr(s, ".real") || strstr(s, "realscript") || strstr(s, " extends "))
        return V_REAL;
    if (strstr(s, " em c ") || strstr(s, " in c ") || strstr(s, "linguagem c ") || strstr(s, "#include") || strstr(s, "typedef")) return V_C;
    return -1;
}
void assistant_ui_write_prefs(FILE* f) { fprintf(f, "assistant_variant=%d\n", U.variant); }
bool assistant_ui_read_pref(const char* key, const char* v) {
    if (strcmp(key, "assistant_variant")) return false;
    int x = atoi(v);
    if (x >= 0 && x < V_COUNT) U.variant = x;
    return true;
}
void editor_prefs_changed(void);

void assistant_ui_set_action(AssistantActionFn fn) { U.action = fn; }
bool assistant_ui_is_open(void) { return U.open; }

static int lang(void) { return i18n_language() == LANG_PT_BR ? 1 : i18n_language() == LANG_ES ? 2 : 0; } // (2026-11-25: Spanish)

static void search(void) {
    assistant_relang(); // (2026-11-25: the editor's language changed since the index was built)
    U.asked_variant = detect_variant(U.q);
    U.nhits = assistant_search(U.q, U.hits, MAX_HITS, &U.weak);
    U.sel = U.nhits ? 0 : -1;
    U.searched = U.q[0] != 0;
    snprintf(U.last_q, sizeof U.last_q, "%s", U.q);
    U.ndocs_seen = assistant_doc_count();
}

void assistant_ui_open(const char* query) {
    assistant_init();
    U.open = true, U.focus_input = true;
    if (query && query[0]) {
        snprintf(U.q, sizeof U.q, "%s", query);
        search();
        if (assistant_state() == AS_NONE || assistant_state() == AS_FAILED) assistant_fetch(false);
    }
}

void assistant_ui_show_article(const char* id) {
    assistant_init();
    int d = assistant_find_article(id);
    U.open = true;
    if (d < 0) {
        snprintf(U.want, sizeof U.want, "%s", id);
        if (assistant_state() == AS_NONE || assistant_state() == AS_FAILED) assistant_fetch(false);
        return;
    }
    U.want[0] = 0;
    U.hits[0] = (AsHit){ d, 1.0f };
    U.nhits = 1, U.sel = 0;
}

// "Show me where": the path's labels (English, as the editor writes them: TR gives what is on the screen)
static void show_where(const char* path) {
    g_re_hl_nlabel = 0;
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    U.hl_path[0] = 0;
    for (char* tok = strtok(p, ">"); tok && g_re_hl_nlabel < 4; tok = strtok(NULL, ">")) {
        while (*tok == ' ') tok++;
        const char* shown = TR(tok);
        snprintf(g_re_hl_label[g_re_hl_nlabel++], sizeof g_re_hl_label[0], "%s", shown);
        size_t n = strlen(U.hl_path);
        snprintf(U.hl_path + n, sizeof U.hl_path - n, "%s%s", n ? "  >  " : "", shown);
    }
    U.hl_until = igGetTime() + 8.0;
    GREHighlight = 1;
}

static void run_action(const char* act) {
    if (!strncmp(act, "show:", 5)) show_where(act + 5);
    else if (!strncmp(act, "article:", 8)) {
        if (U.sel >= 0 && U.nhist < 16) U.history[U.nhist++] = U.hits[U.sel].doc;
        assistant_ui_show_article(act + 8);
    } else if (!strncmp(act, "api:", 4)) script_api_reference_open(act + 4);
    else if (!strncmp(act, "url:", 4)) crash_open_file(act + 4); // (the browser: the user clicked it)
    else if (U.action) U.action(act);
}

// the outlines where the path's controls are (pulsing; the menus' items as they open)
static void draw_highlight(void) {
    if (!GREHighlight) return;
    double now = igGetTime();
    if (now > U.hl_until) {
        extern void (*g_ui_sweep_item_hook)(const char*, float, float, float, float); // (imgui_hooks.c)
        if (!g_ui_sweep_item_hook) GREHighlight = 0; // (item 21: the UI sweep keeps every item offered, the whole frame)
        g_re_hl_nlabel = 0;
        return;
    }
    ImDrawList* dl = igGetForegroundDrawList_ViewportPtr(igGetMainViewport());
    float a = 0.55f + 0.45f * (float)sin(now * 6.0);
    uint32_t col = igGetColorU32_Vec4((ImVec4){ 1.0f, 0.72f, 0.18f, a });
    if (g_re_hl_frame >= igGetFrameCount() - 1)
        for (int i = 0; i < g_re_hl_nrect; i++) {
            const float* r = g_re_hl_rect[i];
            ImDrawList_AddRect(dl, (ImVec2){ r[0] - 3, r[1] - 3 }, (ImVec2){ r[2] + 3, r[3] + 3 }, col, 4.0f, 2.5f, 0);
        }
    // the path, as a hint at the top
    const ImGuiViewport* vp = igGetMainViewport();
    char hint[300];
    snprintf(hint, sizeof hint, "%s  %s", TR("Here:"), U.hl_path);
    ImVec2 ts = igCalcTextSize(hint, NULL, false, -1);
    float x = vp->WorkPos.x + (vp->WorkSize.x - ts.x) * 0.5f, y = vp->WorkPos.y + igGetFontSize() * 2.2f;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x - 10, y - 6 }, (ImVec2){ x + ts.x + 10, y + ts.y + 6 }, igGetColorU32_Vec4(THVA(TK_N4, 0.94f)), 6.0f, 0);
    ImDrawList_AddRect(dl, (ImVec2){ x - 10, y - 6 }, (ImVec2){ x + ts.x + 10, y + ts.y + 6 }, col, 6.0f, 1.5f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x, y }, igGetColorU32_Vec4(THVA(TK_N15, 0.95f)), hint, NULL);
}

// --- Markdown (the articles' subset) ---
typedef struct Link { char label[64], act[160]; } Link;

// a line's text without the Markdown marks: **bold**, `code`, [label](action) (the actions collected)
static void plain(const char* s, char* out, size_t cap, Link* links, int* nlinks, int max_links) {
    size_t n = 0;
    while (*s && n + 1 < cap) {
        if (s[0] == '*' && s[1] == '*') {
            s += 2;
            continue;
        }
        if (*s == '`') {
            s++;
            continue;
        }
        if (*s == '[') {
            const char* close = strchr(s, ']');
            if (close && close[1] == '(') {
                const char* end = strchr(close, ')');
                if (end) {
                    size_t ln = (size_t)(close - s - 1);
                    if (links && *nlinks < max_links) {
                        Link* L = &links[(*nlinks)++];
                        snprintf(L->label, sizeof L->label, "%.*s", (int)ln, s + 1);
                        snprintf(L->act, sizeof L->act, "%.*s", (int)(end - close - 2), close + 2);
                    }
                    for (size_t i = 0; i < ln && n + 1 < cap; i++) out[n++] = s[1 + i];
                    s = end + 1;
                    continue;
                }
            }
        }
        out[n++] = *s++;
    }
    out[n] = 0;
    // (2026-11-25, the tester's block of item 13) "Blueprint" -> "Graph" / "Grafo" on screen, as every TR: the visible
    // text and the links' labels; never the actions ("show:" paths are the editor's English labels)
    const char* t = i18n_terms(out);
    if (t != out) snprintf(out, cap, "%s", t);
    if (links)
        for (int i = 0; i < *nlinks; i++) {
            const char* lt = i18n_terms(links[i].label);
            if (lt != links[i].label) snprintf(links[i].label, sizeof links[i].label, "%s", lt);
        }
}

static void link_buttons(Link* links, int n, int* uid) {
    for (int i = 0; i < n; i++) {
        char id[220];
        snprintf(id, sizeof id, "%s##lk%d", links[i].label, (*uid)++);
        if (i) igSameLine(0, 6);
        bool show = !strncmp(links[i].act, "show:", 5);
        if (show) igPushStyleColor_Vec4(ImGuiCol_Button, THV(TK_AMBER_D));
        if (igSmallButton(id)) run_action(links[i].act);
        if (show) igPopStyleColor(1);
        if (igIsItemHovered(0)) igSetTooltip("%s", links[i].act);
    }
}

static void markdown(const char* text, int show) {
    const char* p = text;
    int uid = 0;
    char line[1024], txt[1024];
    float wrap = igGetContentRegionAvail().x;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = e ? e + 1 : p + n;
        if (!strncmp(line, "```", 3)) { // code: until the closing fence
            char lang_[48];
            snprintf(lang_, sizeof lang_, "%s", line + 3);
            int var = tag_variant(lang_);
            if (var >= 0 && var != show) { // (another variant: skipped)
                while (*p && strncmp(p, "```", 3)) {
                    const char* e2 = strchr(p, '\n');
                    p = e2 ? e2 + 1 : p + strlen(p);
                }
                if (*p) {
                    const char* e3 = strchr(p, '\n');
                    p = e3 ? e3 + 1 : p + strlen(p);
                }
                continue;
            }
            char code[4096];
            size_t cn = 0;
            while (*p && strncmp(p, "```", 3)) {
                const char* e2 = strchr(p, '\n');
                size_t n2 = e2 ? (size_t)(e2 - p + 1) : strlen(p);
                if (cn + n2 < sizeof code) memcpy(code + cn, p, n2), cn += n2;
                p += n2;
            }
            code[cn] = 0;
            if (*p) {
                const char* e3 = strchr(p, '\n');
                p = e3 ? e3 + 1 : p + strlen(p);
            }
            // a picture, not text: the Blueprint's node graph, the diagrams (assistant_draw.c)
            bool diagram = !strncmp(lang_, "diagram ", 8);
            if (diagram && strncmp(lang_ + 8, "layout", 6)) { // (its boxes are text on screen: the terms line by line,
                // "Blueprint" -> "Grafo"; a layout's boxes are "Show me where" paths in the editor's English: as they are)
                char shown[4096];
                size_t sn = 0;
                for (const char* c = code; *c && sn + 1 < sizeof shown;) {
                    const char* ce = strchr(c, '\n');
                    size_t cl = ce ? (size_t)(ce - c) : strlen(c);
                    char one[1024];
                    snprintf(one, sizeof one, "%.*s", (int)(cl < sizeof one ? cl : sizeof one - 1), c);
                    sn += (size_t)snprintf(shown + sn, sizeof shown - sn, "%s%s", i18n_terms(one), ce ? "\n" : "");
                    if (sn >= sizeof shown) sn = sizeof shown - 1;
                    c = ce ? ce + 1 : c + cl;
                }
                shown[sn] = 0;
                if (assistant_draw_diagram(lang_ + 8, shown, uid++, show_where)) continue;
            } else if (var == V_BLUEPRINT && assistant_draw_diagram("blueprint", code, uid++, show_where)) continue;
            igPushStyleColor_Vec4(ImGuiCol_ChildBg, THV(TK_N3));
            float lines = 1.0f;
            for (const char* c = code; *c; c++) lines += *c == '\n';
            char cid[32];
            snprintf(cid, sizeof cid, "##code%d", uid++);
            igBeginChild_Str(cid, (ImVec2){ 0, igGetTextLineHeightWithSpacing() * (lines < 18 ? lines : 18) + 8.0f + igGetStyle()->ScrollbarSize },
                             ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar); // (long lines: scrolled, never cut)
            if (g_code_font) igPushFont(g_code_font, 0.0f);
            igTextUnformatted(code, NULL);
            if (g_code_font) igPopFont();
            igEndChild();
            igPopStyleColor(1);
            char bid[48];
            snprintf(bid, sizeof bid, "%s##cp%d", TR("Copy"), uid++);
            if (igSmallButton(bid)) igSetClipboardText(code);
            if (lang_[0] && var < 0) {
                igSameLine(0, 8);
                igTextDisabled("%s", lang_);
            }
            continue;
        }
        Link links[8];
        int nl = 0;
        if (!line[0]) {
            igSpacing();
            continue;
        }
        if (line[0] == '#') { // a heading
            const char* h = line;
            while (*h == '#') h++;
            while (*h == ' ') h++;
            bool vhead = false; // ("## Python" over its code: the selector says it)
            for (int v = 0; v < V_COUNT; v++) vhead |= !strcmp(h, g_vname[v]);
            if (vhead) continue;
            plain(h, txt, sizeof txt, links, &nl, 8);
            igSpacing();
            igTextColored(THV(TK_WARN), "%s", txt);
            continue;
        }
        const char* body = line;
        char bullet[8] = "";
        if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') body = line + 2, snprintf(bullet, sizeof bullet, "%s", "\xE2\x80\xA2");
        else {
            const char* d = line;
            while (*d >= '0' && *d <= '9') d++;
            if (d > line && d[0] == '.' && d[1] == ' ') snprintf(bullet, sizeof bullet, "%.*s.", (int)(d - line), line), body = d + 2;
        }
        plain(body, txt, sizeof txt, links, &nl, 8);
        if (bullet[0]) {
            igTextDisabled("%s", bullet);
            igSameLine(igGetFontSize() * 1.8f, 0);
        }
        igPushTextWrapPos(igGetCursorPosX() + wrap - (bullet[0] ? igGetFontSize() * 1.8f : 0.0f));
        igTextUnformatted(txt, NULL);
        igPopTextWrapPos();
        if (nl) {
            if (bullet[0]) igSetCursorPosX(igGetFontSize() * 1.8f + igGetStyle()->WindowPadding.x);
            link_buttons(links, nl, &uid);
        }
    }
}

static const char* kind_label(const AsDoc* d) {
    if (d->src == AS_SRC_API) return TR("Function");
    if (d->src == AS_SRC_CONTROL) return TR("Control");
    switch (d->kind) {
    case AS_KIND_RECIPE: return TR("Recipe");
    case AS_KIND_ERROR: return TR("Error");
    case AS_KIND_CONCEPT: return TR("Concept");
    default: return TR("How to");
    }
}

static void doc_view(const AsDoc* d) {
    int l = lang();
    if (U.nhist && igSmallButton(TR("< Back"))) {
        int prev = U.history[--U.nhist];
        U.hits[0] = (AsHit){ prev, 1.0f }, U.nhits = U.nhits ? U.nhits : 1, U.sel = 0;
        for (int i = 0; i < U.nhits; i++)
            if (U.hits[i].doc == prev) U.sel = i;
        return;
    }
    igTextColored(THV(TK_N15), "%s", d->title[l][0] ? d->title[l] : d->title[0]);
    igSameLine(0, 10);
    igTextDisabled("%s", kind_label(d));
    igSeparator();
    if (d->src == AS_SRC_ARTICLE) {
        const char* body = d->body[l][0] ? d->body[l] : d->body[0];
        int have = 0; // the variants it has
        for (const char* f = strstr(body, "```"); f; f = strstr(f + 3, "```")) {
            int v = tag_variant(f + 3);
            if (v >= 0) have |= 1 << v;
            const char* close = strstr(f + 3, "```");
            if (!close) break;
            f = close;
        }
        int show = -1;
        if (have) { // the question's, else the user's choice, else the first it has
            show = U.asked_variant >= 0 && (have >> U.asked_variant & 1) ? U.asked_variant : (have >> U.variant & 1) ? U.variant : -1;
            for (int v = 0; show < 0 && v < V_COUNT; v++)
                if (have >> v & 1) show = v;
            igAlignTextToFramePadding();
            igTextDisabled("%s", TR("Show as:"));
            for (int v = 0; v < V_COUNT; v++) {
                if (!(have >> v & 1)) continue;
                igSameLine(0, 6);
                bool on = v == show;
                if (on) igPushStyleColor_Vec4(ImGuiCol_Button, igGetStyle()->Colors[ImGuiCol_ButtonActive]);
                char bid[48];
                snprintf(bid, sizeof bid, "%s", v == V_BLUEPRINT ? TR("Blueprint") : g_vname[v]);
                igPushID_Int(1000 + v); // (the id apart: "C#" followed by "##" would read as "###")
                if (igSmallButton(bid)) U.variant = v, U.asked_variant = -1, editor_prefs_changed();
                igPopID();
                if (on) igPopStyleColor(1);
            }
            igSetItemTooltip("%s", TR("Nodes or code, in the language you choose (kept for the next answers)."));
            igSeparator();
        }
        markdown(body, show);
    } else if (d->src == AS_SRC_API) {
        if (g_code_font) igPushFont(g_code_font, 0.0f);
        igTextWrapped("%s", d->extra);
        if (g_code_font) igPopFont();
        igSpacing();
        igTextWrapped("%s", d->body[0]);
        igSpacing();
        if (igButton(TR("Open in the API Reference"), (ImVec2){ 0, 0 })) script_api_reference_open(d->id);
    } else { // a control: what it does, where it is
        igTextWrapped("%s", d->body[l][0] ? d->body[l] : d->body[0]);
        igSpacing();
        if (igButton(TR("Show me where"), (ImVec2){ 0, 0 })) show_where(d->id);
        igSameLine(0, 8);
        igTextDisabled("%s", TR("(visible now, or when its panel or menu opens)"));
    }
}

static void base_status(void) {
    AsState st = assistant_state();
    char kb[32];
    snprintf(kb, sizeof kb, "%u KB", (assistant_base_bytes() + 1023u) / 1024u);
    switch (st) {
    case AS_FETCHING:
        igTextDisabled("%s", TR("Fetching the articles from GitHub... (the card at the bottom right)"));
        break;
    case AS_ONLINE:
        igTextDisabled(TR("Online: %d articles from GitHub (%s), nothing installed."), assistant_article_count(), kb);
        igSameLine(0, 8);
        if (igSmallButton(TR("Download for offline use"))) assistant_fetch(true);
        break;
    case AS_OFFLINE:
        igTextDisabled(TR("Offline: %d articles on this computer (%s, version %s)."), assistant_article_count(), kb, assistant_version());
        igSameLine(0, 8);
        if (igSmallButton(TR("Update"))) assistant_fetch(true);
        igSameLine(0, 4);
        if (igSmallButton(TR("Remove"))) assistant_remove_offline();
        break;
    case AS_FAILED:
        igTextColored(THV(TK_WARN), "%s", assistant_error());
        igSameLine(0, 8);
        if (igSmallButton(TR("Try again"))) assistant_fetch(false);
        break;
    default:
        igTextDisabled("%s", TR("The articles come from GitHub with your first question (nothing installed), or:"));
        igSameLine(0, 8);
        if (igSmallButton(TR("Download for offline use"))) assistant_fetch(true);
        break;
    }
    igSetItemTooltip("%s", TR("Without the articles the Assistant still answers from the engine's functions and the controls' descriptions."));
}

// (the questions it couldn't answer: kept here, sent only when the user says so)
static void note_unanswered(const char* q) {
    char dir[512], p[600];
    if (!re_config_dir(dir, sizeof dir)) return;
    snprintf(p, sizeof p, "%s\\assistant_unanswered.txt", dir);
    FILE* f = re_fopen(p, "a");
    if (f) fprintf(f, "%s\n", q), fclose(f);
}

void assistant_ui_draw(void) {
    draw_highlight();
    if (!U.open) return;
    // the base arrived (or changed): the question asked again
    if (U.searched && assistant_doc_count() != U.ndocs_seen) search();
    if (U.want[0] && assistant_find_article(U.want) >= 0) assistant_ui_show_article(U.want);
    float fs = igGetFontSize();
    // the first time: a good part of the screen (the pictures and the graphs whole), never smaller than 56 x 36 lines
    const ImGuiViewport* mvp = igGetMainViewport();
    float ww = mvp->WorkSize.x * 0.6f, wh = mvp->WorkSize.y * 0.72f;
    igSetNextWindowSize((ImVec2){ ww > fs * 56.0f ? ww : fs * 56.0f, wh > fs * 36.0f ? wh : fs * 36.0f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mvp->WorkPos.x + mvp->WorkSize.x * 0.5f, mvp->WorkPos.y + mvp->WorkSize.y * 0.5f }, ImGuiCond_FirstUseEver,
                       (ImVec2){ 0.5f, 0.5f });
    char title[96];
    snprintf(title, sizeof title, "%s###assistant", TR("Assistant"));
    if (!igBegin(title, &U.open, ImGuiWindowFlags_NoCollapse)) {
        igEnd();
        return;
    }
    // the question
    igSetNextItemWidth(-fs * 7.0f);
    if (U.focus_input) igSetKeyboardFocusHere(0), U.focus_input = false;
    bool enter = igInputTextWithHint("##asq", TR("Ask anything: how to make a character jump twice, package the game..."), U.q, sizeof U.q,
                                     ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    bool changed = strcmp(U.q, U.last_q) != 0;
    igSameLine(0, 6);
    bool ask = igButton(TR("Search"), (ImVec2){ -1, 0 }) || enter;
    if (changed || ask) search(), U.nhist = 0;
    if (ask && U.q[0] && (assistant_state() == AS_NONE || assistant_state() == AS_FAILED)) assistant_fetch(false); // (online: the first question)
    base_status();
    igSeparator();
    // the answers (left), the chosen one (right)
    float lw = fs * 17.0f;
    igBeginChild_Str("##as_hits", (ImVec2){ lw, 0 }, ImGuiChildFlags_Borders, 0);
    if (!U.searched) { // no question yet: the base's articles, to browse
        igTextDisabled("%s", TR("Articles"));
        int shown = 0;
        for (int i = 0; i < assistant_doc_count() && shown < 60; i++) {
            const AsDoc* d = assistant_doc(i);
            if (d->src != AS_SRC_ARTICLE || d->kind == AS_KIND_ERROR) continue;
            char row[200];
            snprintf(row, sizeof row, "%s##art%d", d->title[lang()][0] ? d->title[lang()] : d->title[0], i);
            if (igSelectable_Bool(row, false, 0, (ImVec2){ 0, 0 })) {
                U.hits[0] = (AsHit){ i, 1.0f }, U.nhits = 1, U.sel = 0, U.searched = true, U.nhist = 0;
                snprintf(U.last_q, sizeof U.last_q, "%s", U.q);
            }
            shown++;
        }
        if (!shown) igTextWrapped("%s", TR("Ask a question above: the engine's functions and the controls answer now; the articles come with it."));
    }
    for (int i = 0; i < U.nhits; i++) {
        const AsDoc* d = assistant_doc(U.hits[i].doc);
        if (!d) continue;
        char row[220];
        // the title cut to its column (the kind beside it)
        const char* tt = d->title[lang()][0] ? d->title[lang()] : d->title[0];
        float room = lw - fs * 5.6f;
        char cut[160];
        snprintf(cut, sizeof cut, "%s", tt);
        if (igCalcTextSize(cut, NULL, false, -1).x > room) {
            size_t n = strlen(cut);
            while (n > 1 && igCalcTextSize(cut, NULL, false, -1).x > room - igCalcTextSize("...", NULL, false, -1).x) {
                cut[--n] = 0;
                while (n > 1 && (cut[n - 1] & 0xC0) == 0x80) cut[--n] = 0; // (a whole UTF-8 character)
            }
            snprintf(cut + n, sizeof cut - n, "...");
        }
        snprintf(row, sizeof row, "%s##hit%d", cut, i);
        if (igSelectable_Bool(row, U.sel == i, 0, (ImVec2){ 0, 0 })) U.sel = i, U.nhist = 0;
        if (strcmp(cut, tt) && igIsItemHovered(0)) igSetTooltip("%s", tt);
        igSameLine(lw - fs * 4.6f, 0);
        igTextDisabled("%s", kind_label(d));
    }
    if (U.searched && U.weak) { // nothing good: say so, offer to send it
        igSpacing();
        igTextWrapped("%s", TR("No good answer for this yet."));
        if (strcmp(U.unanswered_sent, U.q) && igSmallButton(TR("Send it as a suggestion"))) {
            note_unanswered(U.q);
            snprintf(U.unanswered_sent, sizeof U.unanswered_sent, "%s", U.q);
            if (U.action) {
                char act[400];
                snprintf(act, sizeof act, "suggest:%s", U.q);
                U.action(act);
            }
        }
        igSetItemTooltip("%s", TR("Opens Send a Suggestion with your question: the article that answers it can be written."));
    }
    igEndChild();
    igSameLine(0, 6);
    igBeginChild_Str("##as_doc", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, 0);
    if (U.sel >= 0 && U.sel < U.nhits && assistant_doc(U.hits[U.sel].doc)) doc_view(assistant_doc(U.hits[U.sel].doc));
    else igTextDisabled("%s", TR("The answer shows here."));
    igEndChild();
    igEnd();
}
