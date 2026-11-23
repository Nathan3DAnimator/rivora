// The scripts' IDE (script_ide.h): documents in tabs, a text buffer edited by hand (ImGui draws it: no InputText), the
// language's colors, live errors, completion, signature help, go to definition, find / replace; the API reference.
#include "script_ide.h"

#include "theme.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "script.h"
#include "bp_debug.h"
#include "ui_icons.h"
#include "ui_tips.h"

struct ImFont* g_code_font; // (main.c: Consolas)
bool g_script_ide_demo;      // (--ide-demo: the windows placed for a picture, the code focused)

#define MAX_DOCS 12
#define MAX_UNDO 200

typedef struct Undo { char* text; size_t len; int cl, cc; } Undo;
typedef struct Doc {
    char vpath[512], path[600], name[64];
    ScriptLang lang;
    char* text;
    size_t len, cap;
    int* ls;               // each line's start (offsets)
    int nl, capl;
    uint8_t* lstate;       // a line's start inside a block comment (C, C#)
    int cl, cc, al, ac;    // the cursor (line, column in bytes); the selection's anchor (al < 0: none)
    int want_col;          // (up / down keep the column)
    bool dirty, edited, scroll_to;
    double edit_t;         // the last edit (the live check waits a pause)
    Undo undo[MAX_UNDO];
    int nundo, uptr;
    double undo_t;
    int goto_line;
    ScriptFile* sf;        // its script (the registry keeps the pointer)
} Doc;

static struct {
    Doc* d[MAX_DOCS];
    int n, cur;
    bool open, focused;
    // completion
    bool comp;
    int comp_sel, comp_n;
    char comp_word[64];
    struct { char name[64]; char detail[160]; const char* doc; uint8_t kind; } items[64];
    // find
    bool find_open;
    char find[128], repl[128];
    // the API reference
    bool api_open;
    char api_find[64];
    int api_sel;
    // editor utilities
    bool util_open;
    char status[200];
} I = { .cur = -1 };

// --- The buffer ---
static void relines(Doc* d) {
    d->nl = 0;
    int start = 0;
    for (size_t i = 0; i <= d->len; i++) {
        if (i == d->len || d->text[i] == '\n') {
            if (d->nl == d->capl) {
                int cap = d->capl ? d->capl * 2 : 256;
                int* n = realloc(d->ls, (size_t)cap * sizeof *n);
                uint8_t* s = realloc(d->lstate, (size_t)cap);
                if (!n || !s) {
                    if (n) d->ls = n;
                    if (s) d->lstate = s;
                    return;
                }
                d->ls = n, d->lstate = s, d->capl = cap;
            }
            d->ls[d->nl++] = start;
            start = (int)i + 1;
        }
    }
    // block comments' state at each line's start (C, C#)
    bool in = false;
    for (int l = 0; l < d->nl; l++) {
        d->lstate[l] = in;
        int e = l + 1 < d->nl ? d->ls[l + 1] - 1 : (int)d->len;
        bool str = false;
        for (int i = d->ls[l]; i < e; i++) {
            char c = d->text[i];
            if (in) {
                if (c == '*' && i + 1 < e && d->text[i + 1] == '/') in = false, i++;
                continue;
            }
            if (c == '"') str = !str;
            if (str) continue;
            if (c == '/' && i + 1 < e && d->text[i + 1] == '/') break;
            if ((d->lang == SL_C || d->lang == SL_CSHARP) && c == '/' && i + 1 < e && d->text[i + 1] == '*') in = true, i++;
        }
    }
}
static int line_len(Doc* d, int l) {
    int e = l + 1 < d->nl ? d->ls[l + 1] - 1 : (int)d->len;
    return e - d->ls[l];
}
static int off_of(Doc* d, int l, int c) {
    if (l < 0) l = 0;
    if (l >= d->nl) l = d->nl - 1;
    int ll = line_len(d, l);
    return d->ls[l] + (c < 0 ? 0 : c > ll ? ll : c);
}
static void pos_of(Doc* d, int off, int* l, int* c) {
    int lo = 0, hi = d->nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (d->ls[mid] <= off) lo = mid;
        else hi = mid - 1;
    }
    *l = lo, *c = off - d->ls[lo];
}
static bool reserve(Doc* d, size_t n) {
    if (n + 1 <= d->cap) return true;
    size_t cap = d->cap ? d->cap : 4096;
    while (cap < n + 1) cap *= 2;
    char* t = realloc(d->text, cap);
    if (!t) return false;
    d->text = t, d->cap = cap;
    return true;
}
static void snapshot(Doc* d, bool coalesce) { // an undo step (typing within a moment: one step)
    double now = re_time_seconds();
    if (coalesce && d->uptr > 0 && now - d->undo_t < 0.8) {
        d->undo_t = now;
        return;
    }
    d->undo_t = now;
    for (int i = d->uptr; i < d->nundo; i++) free(d->undo[i].text);
    d->nundo = d->uptr;
    if (d->nundo == MAX_UNDO) {
        free(d->undo[0].text);
        memmove(d->undo, d->undo + 1, sizeof(Undo) * (MAX_UNDO - 1));
        d->nundo--;
    }
    Undo* u = &d->undo[d->nundo++];
    u->text = malloc(d->len + 1); // (an edit, not a frame)
    if (u->text) memcpy(u->text, d->text, d->len), u->text[d->len] = 0;
    u->len = d->len, u->cl = d->cl, u->cc = d->cc;
    d->uptr = d->nundo;
}
static void edited(Doc* d) {
    relines(d);
    d->dirty = d->edited = true, d->edit_t = re_time_seconds(), d->scroll_to = true;
}
static void replace_range(Doc* d, int a, int b, const char* s, size_t n) { // [a, b) by s
    if (a > b) { int t = a; a = b, b = t; }
    if (!reserve(d, d->len - (size_t)(b - a) + n)) return;
    memmove(d->text + a + n, d->text + b, d->len - (size_t)b);
    memcpy(d->text + a, s, n);
    d->len = d->len - (size_t)(b - a) + n;
    d->text[d->len] = 0;
    edited(d);
    pos_of(d, a + (int)n, &d->cl, &d->cc);
    d->al = -1;
}
static bool has_sel(Doc* d) { return d->al >= 0 && (d->al != d->cl || d->ac != d->cc); }
static void sel_range(Doc* d, int* a, int* b) {
    *a = off_of(d, d->cl, d->cc), *b = d->al >= 0 ? off_of(d, d->al, d->ac) : *a;
    if (*a > *b) { int t = *a; *a = *b, *b = t; }
}
static void insert_text(Doc* d, const char* s, size_t n, bool coalesce) {
    snapshot(d, coalesce);
    int a, b;
    sel_range(d, &a, &b);
    replace_range(d, a, b, s, n);
}

static Doc* doc_open(const char* vpath) {
    for (int i = 0; i < I.n; i++)
        if (!strcmp(I.d[i]->vpath, vpath)) return I.d[i];
    if (I.n >= MAX_DOCS) return NULL;
    ScriptFile* sf = script_load(vpath);
    Doc* d = calloc(1, sizeof *d); // (a document opened)
    if (!d) return NULL;
    snprintf(d->vpath, sizeof d->vpath, "%s", vpath);
    snprintf(d->path, sizeof d->path, "%s", sf ? sf->path : vpath);
    snprintf(d->name, sizeof d->name, "%s", strrchr(vpath, '/') ? strrchr(vpath, '/') + 1 : vpath);
    d->lang = script_lang_of(vpath);
    d->al = -1;
    FILE* f = re_fopen(d->path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && reserve(d, (size_t)n)) {
            d->len = fread(d->text, 1, (size_t)n, f);
            d->text[d->len] = 0;
        }
        fclose(f);
    }
    if (!reserve(d, 16)) {
        free(d);
        return NULL;
    }
    // (the editor's own: tabs as 4 spaces, no carriage returns)
    size_t o = 0;
    char* src = malloc(d->len + 1);
    if (src) {
        memcpy(src, d->text, d->len);
        size_t n = d->len;
        for (size_t i = 0; i < n; i++) {
            if (src[i] == '\r') continue;
            if (src[i] == '\t') {
                reserve(d, o + 4 + (n - i));
                for (int k = 0; k < 4; k++) d->text[o++] = ' ';
                continue;
            }
            reserve(d, o + 1 + (n - i));
            d->text[o++] = src[i];
        }
        free(src);
        d->len = o;
        d->text[o] = 0;
    }
    relines(d);
    I.d[I.n++] = d;
    return d;
}

static bool doc_save(Doc* d) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", d->path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(d->text, 1, d->len, f) == d->len;
    ok = fclose(f) == 0 && ok;
    ok = ok && re_replace_file(tmp, d->path);
    if (ok) {
        d->dirty = false;
        ScriptFile* sf = script_load(d->vpath); // (its class compiled again from the file now)
        snprintf(I.status, sizeof I.status, "%s %s: %s (%.2f ms, %u nodes, %u ops)", TR("Saved"), d->name,
                 sf && sf->ok ? TR("compiled") : TR("has errors"), sf ? sf->compile_ms : 0.0, sf ? sf->nodes : 0, sf ? sf->ops : 0);
        content_mark_dirty();
    }
    return ok;
}

void script_ide_open_at(const char* vpath, int line) {
    if (content_defer_open(vpath)) return;
    Doc* d = doc_open(vpath);
    if (!d) return;
    I.open = true;
    for (int i = 0; i < I.n; i++)
        if (I.d[i] == d) I.cur = i;
    if (line > 0) d->cl = line - 1 < d->nl ? line - 1 : d->nl - 1, d->cc = 0, d->al = -1, d->scroll_to = true;
}
void script_ide_open(const char* vpath) { script_ide_open_at(vpath, 0); }
bool script_ide_focused(void) { return I.focused; }

// --- Colors ---
enum { C_TEXT = 0, C_KEYWORD, C_TYPE, C_STRING, C_NUMBER, C_COMMENT, C_API, C_MEMBER, C_OP, C_COUNT };
static const uint32_t g_col[C_COUNT] = { TK_N14, TK_BLUE, TK_TEAL, TK_ORANGE, TK_LIME_L, TK_LIME,
                                          TK_AMBER_L, TK_BLUE_L, TK_N13 };
static bool in_list(const char* w, size_t n, const char* const* l, uint32_t cnt) {
    for (uint32_t i = 0; i < cnt; i++)
        if (strlen(l[i]) == n && !strncmp(l[i], w, n)) return true;
    return false;
}
static ScriptFile* doc_file(Doc* d) { return d->sf ? d->sf : (d->sf = script_load(d->vpath)); } // (the registry's: no disk read per call)
static bool is_member_name(Doc* d, const char* w, size_t n) {
    ScriptFile* sf = doc_file(d);
    for (uint32_t i = 0; sf && i < sf->nsyms; i++)
        if ((sf->syms[i].kind == SYM_FIELD || sf->syms[i].kind == SYM_METHOD || sf->syms[i].kind == SYM_EVENT) && strlen(sf->syms[i].name) == n &&
            !strncmp(sf->syms[i].name, w, n))
            return true;
    return false;
}

// The line's tokens' colors (a color per byte)
static void colorize(Doc* d, int l, uint8_t* col) {
    const char* s = d->text + d->ls[l];
    int n = line_len(d, l);
    uint32_t nk, nt;
    const char* const* kw = script_keywords(d->lang, &nk);
    const char* const* ty = script_builtin_types(d->lang, &nt);
    bool block = d->lstate[l];
    for (int i = 0; i < n;) {
        if (block) {
            int j = i;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) j++;
            if (j < n) j += 2, block = false;
            memset(col + i, C_COMMENT, (size_t)(j - i));
            i = j;
            continue;
        }
        char c = s[i];
        bool hash = c == '#' && (d->lang == SL_PYTHON || d->lang == SL_REAL || d->lang == SL_C || d->lang == SL_CSHARP);
        if ((c == '/' && i + 1 < n && s[i + 1] == '/') || hash) {
            memset(col + i, (d->lang == SL_C || d->lang == SL_CSHARP) && hash ? C_KEYWORD : C_COMMENT, (size_t)(n - i));
            return;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*' && (d->lang == SL_C || d->lang == SL_CSHARP)) {
            block = true;
            continue;
        }
        if (c == '"' || (c == '\'' && d->lang == SL_PYTHON)) {
            int j = i + 1;
            while (j < n && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            j = j < n ? j + 1 : n;
            memset(col + i, C_STRING, (size_t)(j - i));
            for (int k = i + 1; k < j - 1; k++) // the interpolations' braces
                if (s[k] == '{' || s[k] == '}') col[k] = C_OP;
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            memset(col + i, C_NUMBER, (size_t)(j - i));
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            size_t w = (size_t)(j - i);
            uint8_t k = C_TEXT;
            if (in_list(s + i, w, kw, nk)) k = C_KEYWORD;
            else if (in_list(s + i, w, ty, nt)) k = C_TYPE;
            else {
                char name[64];
                snprintf(name, sizeof name, "%.*s", (int)(w < 63 ? w : 63), s + i);
                if (j < n && s[j] == '(' && script_api_find(name)) k = C_API;
                else if (is_member_name(d, s + i, w)) k = C_MEMBER;
            }
            memset(col + i, k, w);
            i = j;
            continue;
        }
        col[i++] = strchr("+-*/%=<>!&|^~?:", c) ? C_OP : C_TEXT;
    }
}

// --- Completion ---
static int word_start(Doc* d, int l, int c) {
    const char* s = d->text + d->ls[l];
    while (c > 0 && (isalnum((unsigned char)s[c - 1]) || s[c - 1] == '_')) c--;
    return c;
}
static bool starts_ci(const char* s, const char* pre) {
    for (; *pre; s++, pre++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return false;
    return true;
}
static bool contains_ci(const char* s, const char* pre) {
    size_t n = strlen(pre);
    for (; *s; s++)
        if (!_strnicmp(s, pre, n)) return true;
    return false;
}
static void snake(const char* in, char* out, size_t cap) { // PlaySound2D -> play_sound_2d (Python's spelling)
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        bool up = isupper(c), dig = isdigit(c);
        if (i && o && out[o - 1] != '_' && ((up && (islower((unsigned char)in[i - 1]) || isdigit((unsigned char)in[i - 1]) ||
                                                    (in[i + 1] && islower((unsigned char)in[i + 1])))) ||
                                            (dig && isalpha((unsigned char)in[i - 1]))))
            out[o++] = '_';
        out[o++] = (char)tolower(c);
    }
    out[o] = 0;
}
static void comp_add(const char* name, const char* detail, const char* doc, uint8_t kind) {
    if (I.comp_n >= 64) return;
    for (int i = 0; i < I.comp_n; i++)
        if (!strcmp(I.items[i].name, name)) return;
    snprintf(I.items[I.comp_n].name, sizeof I.items[0].name, "%s", name);
    snprintf(I.items[I.comp_n].detail, sizeof I.items[0].detail, "%s", detail ? detail : "");
    I.items[I.comp_n].doc = doc, I.items[I.comp_n].kind = kind;
    I.comp_n++;
}
static void comp_build(Doc* d) {
    I.comp_n = 0;
    const char* w = I.comp_word;
    ScriptFile* sf = doc_file(d);
    int line = d->cl + 1;
    // the class's: locals and parameters in scope, fields, methods
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; sf && i < sf->nsyms; i++) {
            const ScriptSym* s = &sf->syms[i];
            if (s->kind == SYM_CLASS) continue;
            bool scoped = s->kind == SYM_LOCAL || s->kind == SYM_PARAM;
            if (scoped && (line < s->scope_from || (s->scope_to && line > s->scope_to))) continue;
            bool hit = pass == 0 ? starts_ci(s->name, w) : contains_ci(s->name, w);
            if (hit) {
                char det[64];
                snprintf(det, sizeof det, "%s  %s", s->type, s->kind == SYM_FIELD ? "field" : s->kind == SYM_LOCAL ? "local" : s->kind == SYM_PARAM ? "parameter" : "method");
                comp_add(s->name, det, NULL, s->kind);
            }
        }
    // the language's keywords and types
    uint32_t nk, nt;
    const char* const* kw = script_keywords(d->lang, &nk);
    const char* const* ty = script_builtin_types(d->lang, &nt);
    for (uint32_t i = 0; i < nk; i++)
        if (w[0] && starts_ci(kw[i], w)) comp_add(kw[i], "keyword", NULL, SYM_KEYWORD);
    for (uint32_t i = 0; i < nt; i++)
        if (w[0] && starts_ci(ty[i], w)) comp_add(ty[i], "type", NULL, SYM_KEYWORD);
    static const char* const builtins[] = { "print", "Vector", "Rotator", "Color", "len", "str", "int", "float", "abs", "min", "max",
                                            "clamp", "sqrt", "pow", "floor", "ceil", "round", "sin", "cos", "tan", "atan2", "lerp", "random", "randint" };
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (w[0] && starts_ci(builtins[i], w)) comp_add(builtins[i], "built-in", NULL, SYM_API);
    // the engine's API (Python: snake_case), the prefix first, then what contains it
    for (int pass = 0; pass < 2 && I.comp_n < 64; pass++)
        for (uint32_t i = 0; i < script_api_count() && I.comp_n < 64; i++) {
            const ScriptApi* a = script_api_at(i);
            char nm[80];
            if (d->lang == SL_PYTHON) snake(a->name, nm, sizeof nm);
            else snprintf(nm, sizeof nm, "%s", a->name);
            bool hit = pass == 0 ? starts_ci(nm, w) || starts_ci(a->name, w) : (strlen(w) >= 2 && contains_ci(nm, w));
            if (hit) comp_add(nm, a->sig, a->doc, SYM_API);
        }
    if (I.comp_sel >= I.comp_n) I.comp_sel = I.comp_n ? I.comp_n - 1 : 0;
}
static void comp_accept(Doc* d) {
    if (I.comp_sel < 0 || I.comp_sel >= I.comp_n) return;
    int ws = word_start(d, d->cl, d->cc);
    snapshot(d, false);
    const char* nm = I.items[I.comp_sel].name;
    bool call = I.items[I.comp_sel].kind == SYM_API || I.items[I.comp_sel].kind == SYM_METHOD || I.items[I.comp_sel].kind == SYM_EVENT;
    char ins[96];
    snprintf(ins, sizeof ins, "%s%s", nm, call ? "()" : "");
    replace_range(d, off_of(d, d->cl, ws), off_of(d, d->cl, d->cc), ins, strlen(ins));
    if (call) d->cc--; // (inside the parentheses)
    I.comp = false;
}

// The call being typed around the cursor: its function's name (its signature shown) and which argument
static bool call_at_cursor(Doc* d, char* name, size_t cap, int* arg) {
    int off = off_of(d, d->cl, d->cc), depth = 0;
    *arg = 0;
    for (int i = off - 1; i >= 0 && i > off - 400; i--) {
        char c = d->text[i];
        if (c == '\n' && (d->lang == SL_PYTHON || d->lang == SL_REAL) && depth == 0) return false;
        if (c == ')') depth++;
        else if (c == '(') {
            if (depth == 0) {
                int e = i;
                while (e > 0 && d->text[e - 1] == ' ') e--;
                int s = e;
                while (s > 0 && (isalnum((unsigned char)d->text[s - 1]) || d->text[s - 1] == '_')) s--;
                if (s == e) return false;
                snprintf(name, cap, "%.*s", e - s, d->text + s);
                return true;
            }
            depth--;
        } else if (c == ',' && depth == 0) (*arg)++;
    }
    return false;
}

// --- Keyboard ---
static void move_to(Doc* d, int l, int c, bool shift) {
    if (shift && d->al < 0) d->al = d->cl, d->ac = d->cc;
    if (!shift) d->al = -1;
    d->cl = l < 0 ? 0 : l >= d->nl ? d->nl - 1 : l;
    int ll = line_len(d, d->cl);
    d->cc = c < 0 ? 0 : c > ll ? ll : c;
    d->scroll_to = true;
}
static int indent_of(Doc* d, int l) {
    const char* s = d->text + d->ls[l];
    int n = line_len(d, l), k = 0;
    while (k < n && s[k] == ' ') k++;
    return k;
}
static void toggle_comment(Doc* d) {
    int a = d->cl, b = d->al >= 0 ? d->al : d->cl;
    if (a > b) { int t = a; a = b, b = t; }
    const char* mark = d->lang == SL_PYTHON || d->lang == SL_REAL ? "# " : "// ";
    size_t ml = strlen(mark);
    bool all = true;
    for (int l = a; l <= b; l++) {
        int k = indent_of(d, l);
        all &= line_len(d, l) == k || !strncmp(d->text + d->ls[l] + k, mark, ml - 1);
    }
    snapshot(d, false);
    for (int l = b; l >= a; l--) {
        int k = indent_of(d, l), o = d->ls[l] + k;
        if (line_len(d, l) == k) continue;
        if (all) {
            size_t rm = !strncmp(d->text + o, mark, ml) ? ml : ml - 1;
            replace_range(d, o, o + (int)rm, "", 0);
        } else replace_range(d, o, o, mark, ml);
    }
    d->al = -1;
}
static void indent_lines(Doc* d, bool out) {
    int a = d->cl, b = d->al >= 0 ? d->al : d->cl;
    if (a > b) { int t = a; a = b, b = t; }
    snapshot(d, false);
    int sl = d->al, sc = d->ac, cl = d->cl, cc = d->cc;
    for (int l = b; l >= a; l--) {
        int o = d->ls[l];
        if (out) {
            int k = indent_of(d, l);
            replace_range(d, o, o + (k < 4 ? k : 4), "", 0);
        } else replace_range(d, o, o, "    ", 4);
    }
    d->al = sl, d->ac = sc < 0 ? 0 : sc, d->cl = cl, d->cc = cc < 0 ? 0 : cc;
}

static void keyboard(Doc* d) {
    ImGuiIO* io = igGetIO_Nil();
    bool ctrl = io->KeyCtrl, shift = io->KeyShift;
    if (igIsKeyPressed_Bool(ImGuiKey_F9, false) && d->sf && d->sf->cls) { // the Blueprint debugger: a breakpoint on the cursor's line
        bool had = bp_debug_line_has(d->sf->cls, d->cl + 1);
        if (!bp_debug_toggle_line(d->sf->cls, d->cl + 1) && !had)
            snprintf(I.status, sizeof I.status, "%s", TR("No code runs on this line: no breakpoint."));
        return;
    }
    // completion's keys first
    if (I.comp && I.comp_n) {
        if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true)) { I.comp_sel = (I.comp_sel + 1) % I.comp_n; return; }
        if (igIsKeyPressed_Bool(ImGuiKey_UpArrow, true)) { I.comp_sel = (I.comp_sel + I.comp_n - 1) % I.comp_n; return; }
        if (igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_Tab, false)) { comp_accept(d); return; }
        if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) { I.comp = false; return; }
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_S, false)) doc_save(d);
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_F, false)) I.find_open = true;
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_G, false)) d->goto_line = 1;
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_Space, false)) {
        int ws = word_start(d, d->cl, d->cc);
        snprintf(I.comp_word, sizeof I.comp_word, "%.*s", d->cc - ws, d->text + d->ls[d->cl] + ws);
        I.comp = true, I.comp_sel = 0;
        comp_build(d);
        return;
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_A, false)) { d->al = 0, d->ac = 0, d->cl = d->nl - 1, d->cc = line_len(d, d->nl - 1); return; }
    if (ctrl && (igIsKeyPressed_Bool(ImGuiKey_C, false) || igIsKeyPressed_Bool(ImGuiKey_X, false))) {
        int a, b;
        sel_range(d, &a, &b);
        if (a == b) a = d->ls[d->cl], b = off_of(d, d->cl, line_len(d, d->cl)) + (d->cl + 1 < d->nl ? 1 : 0); // (no selection: the line)
        char* t = malloc((size_t)(b - a) + 1);
        if (t) {
            memcpy(t, d->text + a, (size_t)(b - a));
            t[b - a] = 0;
            igSetClipboardText(t);
            free(t);
        }
        if (igIsKeyPressed_Bool(ImGuiKey_X, false)) {
            snapshot(d, false);
            replace_range(d, a, b, "", 0);
        }
        return;
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_V, false)) {
        const char* t = igGetClipboardText();
        if (t) {
            size_t n = strlen(t), o = 0;
            char* c = malloc(n * 4 + 1);
            if (c) {
                for (size_t i = 0; i < n; i++) {
                    if (t[i] == '\r') continue;
                    if (t[i] == '\t') { memcpy(c + o, "    ", 4), o += 4; continue; }
                    c[o++] = t[i];
                }
                insert_text(d, c, o, false);
                free(c);
            }
        }
        return;
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_Z, true)) {
        if (d->uptr > 0) {
            if (d->uptr == d->nundo) snapshot(d, false), d->uptr--; // (the present kept for redo)
            Undo* u = &d->undo[--d->uptr];
            reserve(d, u->len);
            memcpy(d->text, u->text, u->len), d->len = u->len, d->text[d->len] = 0;
            edited(d);
            move_to(d, u->cl, u->cc, false);
        }
        return;
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_Y, true)) {
        if (d->uptr + 1 < d->nundo) {
            Undo* u = &d->undo[++d->uptr];
            reserve(d, u->len);
            memcpy(d->text, u->text, u->len), d->len = u->len, d->text[d->len] = 0;
            edited(d);
            move_to(d, u->cl, u->cc, false);
        }
        return;
    }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_Slash, false)) { toggle_comment(d); return; }
    if (ctrl && igIsKeyPressed_Bool(ImGuiKey_D, false)) { // the line duplicated
        int a = d->ls[d->cl], n = line_len(d, d->cl);
        char* t = malloc((size_t)n + 2);
        if (t) {
            t[0] = '\n';
            memcpy(t + 1, d->text + a, (size_t)n);
            snapshot(d, false);
            int cl = d->cl, cc = d->cc;
            replace_range(d, a + n, a + n, t, (size_t)n + 1);
            d->cl = cl + 1, d->cc = cc;
            free(t);
        }
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F12, false)) { // go to the definition (the class's), else the API's page
        int ws = word_start(d, d->cl, d->cc), we = d->cc;
        const char* s = d->text + d->ls[d->cl];
        while (we < line_len(d, d->cl) && (isalnum((unsigned char)s[we]) || s[we] == '_')) we++;
        char w[64];
        snprintf(w, sizeof w, "%.*s", we - ws, s + ws);
        ScriptFile* sf = doc_file(d);
        for (uint32_t i = 0; sf && i < sf->nsyms; i++)
            if (!strcmp(sf->syms[i].name, w) && sf->syms[i].line > 0) {
                move_to(d, sf->syms[i].line - 1, sf->syms[i].col - 1, false);
                return;
            }
        if (script_api_find(w)) script_api_reference_open(script_api_find(w)->name);
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_F1, false)) { script_api_reference_open(NULL); return; }
    // moves
    int l = d->cl, c = d->cc;
    if (igIsKeyPressed_Bool(ImGuiKey_LeftArrow, true)) {
        if (has_sel(d) && !shift) { int a, b; sel_range(d, &a, &b); pos_of(d, a, &l, &c); move_to(d, l, c, false); return; }
        if (ctrl) { // a word
            const char* s = d->text + d->ls[l];
            while (c > 0 && s[c - 1] == ' ') c--;
            while (c > 0 && (isalnum((unsigned char)s[c - 1]) || s[c - 1] == '_')) c--;
            if (c == d->cc && c > 0) c--;
        } else if (c > 0) c--;
        else if (l > 0) l--, c = line_len(d, l);
        move_to(d, l, c, shift), d->want_col = d->cc, I.comp = false;
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_RightArrow, true)) {
        if (has_sel(d) && !shift) { int a, b; sel_range(d, &a, &b); pos_of(d, b, &l, &c); move_to(d, l, c, false); return; }
        int ll = line_len(d, l);
        if (ctrl) {
            const char* s = d->text + d->ls[l];
            while (c < ll && (isalnum((unsigned char)s[c]) || s[c] == '_')) c++;
            while (c < ll && s[c] == ' ') c++;
            if (c == d->cc && c < ll) c++;
        } else if (c < ll) c++;
        else if (l + 1 < d->nl) l++, c = 0;
        move_to(d, l, c, shift), d->want_col = d->cc, I.comp = false;
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_UpArrow, true)) { move_to(d, l - 1, d->want_col > c ? d->want_col : c, shift); return; }
    if (igIsKeyPressed_Bool(ImGuiKey_DownArrow, true)) { move_to(d, l + 1, d->want_col > c ? d->want_col : c, shift); return; }
    if (igIsKeyPressed_Bool(ImGuiKey_PageUp, true)) { move_to(d, l - 30, c, shift); return; }
    if (igIsKeyPressed_Bool(ImGuiKey_PageDown, true)) { move_to(d, l + 30, c, shift); return; }
    if (igIsKeyPressed_Bool(ImGuiKey_Home, true)) {
        int k = indent_of(d, l);
        move_to(d, ctrl ? 0 : l, ctrl ? 0 : (c == k ? 0 : k), shift), d->want_col = d->cc;
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_End, true)) {
        int ll2 = ctrl ? d->nl - 1 : l;
        move_to(d, ll2, line_len(d, ll2), shift), d->want_col = d->cc;
        return;
    }
    // edits
    if (igIsKeyPressed_Bool(ImGuiKey_Tab, true)) {
        if (has_sel(d) && d->al != d->cl) indent_lines(d, shift);
        else if (shift) indent_lines(d, true);
        else insert_text(d, "    ", 4 - (size_t)(d->cc % 4), true);
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Backspace, true)) {
        snapshot(d, true);
        int a, b;
        sel_range(d, &a, &b);
        if (a == b) {
            if (!a) return;
            int k = indent_of(d, l);
            a = (c > 0 && c <= k && c % 4 == 0 && line_len(d, l) >= c) ? b - 4 : b - 1; // (an indentation's step at once)
            if (a < d->ls[l] && c > 0) a = b - 1;
        }
        replace_range(d, a, b, "", 0);
        if (I.comp) {
            int ws = word_start(d, d->cl, d->cc);
            snprintf(I.comp_word, sizeof I.comp_word, "%.*s", d->cc - ws, d->text + d->ls[d->cl] + ws);
            if (!I.comp_word[0]) I.comp = false;
            else comp_build(d);
        }
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Delete, true)) {
        snapshot(d, true);
        int a, b;
        sel_range(d, &a, &b);
        if (a == b) b = a + 1 <= (int)d->len ? a + 1 : a;
        replace_range(d, a, b, "", 0);
        return;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Enter, true) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, true)) { // auto indent
        int k = indent_of(d, l);
        const char* s = d->text + d->ls[l];
        int e = c;
        while (e > 0 && s[e - 1] == ' ') e--;
        char last = e > 0 ? s[e - 1] : 0;
        bool opener = last == '{' || last == ':' || last == '(' || last == '[';
        // Real: a line opening a block (class, func, event, if, while, for, else) without ':'
        if (d->lang == SL_REAL && !opener) {
            int k2 = k;
            const char* w = s + k2;
            static const char* const heads[] = { "class ", "func ", "fn ", "event ", "if ", "elif ", "else", "while ", "for " };
            for (size_t h = 0; h < sizeof heads / sizeof heads[0]; h++)
                if (!strncmp(w, heads[h], strlen(heads[h]))) opener = true;
        }
        char buf[160];
        int n = 0;
        buf[n++] = '\n';
        for (int i = 0; i < k + (opener ? 4 : 0) && n < 150; i++) buf[n++] = ' ';
        insert_text(d, buf, (size_t)n, false);
        if (last == '{' && c < line_len(d, l) && s[c] == '}') { // {|} -> the brace on its own line
            int cl = d->cl, cc = d->cc;
            char b2[160];
            int m = 0;
            b2[m++] = '\n';
            for (int i = 0; i < k && m < 150; i++) b2[m++] = ' ';
            replace_range(d, off_of(d, cl, cc), off_of(d, cl, cc), b2, (size_t)m);
            d->cl = cl, d->cc = cc;
        }
        I.comp = false;
        return;
    }
    // typed characters
    for (int i = 0; i < io->InputQueueCharacters.Size; i++) {
        unsigned int ch = io->InputQueueCharacters.Data[i];
        if (ch < 32 || ch == 127) continue;
        char u[4];
        int n = 0;
        if (ch < 0x80) u[n++] = (char)ch;
        else if (ch < 0x800) u[n++] = (char)(0xC0 | (ch >> 6)), u[n++] = (char)(0x80 | (ch & 63));
        else u[n++] = (char)(0xE0 | (ch >> 12)), u[n++] = (char)(0x80 | ((ch >> 6) & 63)), u[n++] = (char)(0x80 | (ch & 63));
        // closing pairs: ( [ { " typed with their closer
        static const char pairs[][2] = { { '(', ')' }, { '[', ']' }, { '{', '}' }, { '"', '"' } };
        const char* s = d->text + d->ls[d->cl];
        if ((ch == ')' || ch == ']' || ch == '}' || ch == '"') && d->cc < line_len(d, d->cl) && s[d->cc] == (char)ch && !has_sel(d)) {
            move_to(d, d->cl, d->cc + 1, false); // (over its own closer)
            continue;
        }
        bool paired = false;
        for (int p = 0; p < 4; p++)
            if ((char)ch == pairs[p][0] && !has_sel(d)) {
                char two[2] = { pairs[p][0], pairs[p][1] };
                insert_text(d, two, 2, true);
                d->cc--;
                paired = true;
            }
        if (!paired) insert_text(d, u, (size_t)n, true);
        // completion: while a word is typed, after a '.'
        if (isalnum(ch) || ch == '_' || ch == '.') {
            int ws = word_start(d, d->cl, d->cc);
            snprintf(I.comp_word, sizeof I.comp_word, "%.*s", d->cc - ws, d->text + d->ls[d->cl] + ws);
            if (I.comp_word[0] || ch == '.') {
                I.comp = true, I.comp_sel = 0;
                comp_build(d);
                if (!I.comp_n) I.comp = false;
            }
        } else I.comp = false;
    }
}

// --- Drawing ---
static ScriptDiag* diag_at(ScriptFile* sf, int line) {
    for (uint32_t i = 0; sf && i < sf->ndiags; i++)
        if (sf->diags[i].line == line) return &sf->diags[i];
    return NULL;
}

static void draw_code(Doc* d, float h) {
    ImGuiIO* io = igGetIO_Nil();
    if (g_code_font) igPushFont(g_code_font, 0.0f);
    float lh = igGetTextLineHeightWithSpacing(), cw = igCalcTextSize("M", NULL, false, -1).x;
    float gutter = cw * 6.0f;
    igBeginChild_Str("code", (ImVec2){ 0, h }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    bool focused = igIsWindowFocused(0);
    if (g_script_ide_demo) focused = true;
    I.focused = focused;
    ImVec2 o = igGetCursorScreenPos(), avail = igGetContentRegionAvail();
    ImDrawList* dl = igGetWindowDrawList();
    float sy = igGetScrollY(), sx = igGetScrollX();
    // the scroll area
    int maxc = 80;
    for (int l = 0; l < d->nl; l++) maxc = line_len(d, l) > maxc ? line_len(d, l) : maxc;
    igDummy((ImVec2){ gutter + cw * (float)(maxc + 4), lh * (float)(d->nl + 2) });
    bool hovered = igIsWindowHovered(0);
    if (focused) keyboard(d);
    // the mouse: click places the cursor, drag selects, double click selects the word
    if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && io->MousePos.x < o.x + igGetScrollX() + gutter && d->sf && d->sf->cls) {
        int l = (int)((io->MousePos.y - o.y) / lh); // the gutter: the line's breakpoint on / off (Unreal's and Visual Studio's)
        bool had = l >= 0 && bp_debug_line_has(d->sf->cls, l + 1);
        if (l >= 0 && l < d->nl && !bp_debug_toggle_line(d->sf->cls, l + 1) && !had)
            snprintf(I.status, sizeof I.status, "%s", TR("No code runs on this line: no breakpoint."));
    } else if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        ImVec2 m = io->MousePos;
        int l = (int)((m.y - o.y) / lh), c = (int)((m.x - o.x - gutter) / cw + 0.5f);
        move_to(d, l, c, io->KeyShift);
        if (!io->KeyShift) d->al = d->cl, d->ac = d->cc;
        d->want_col = d->cc, I.comp = false;
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            int ws = word_start(d, d->cl, d->cc), we = d->cc;
            const char* s = d->text + d->ls[d->cl];
            while (we < line_len(d, d->cl) && (isalnum((unsigned char)s[we]) || s[we] == '_')) we++;
            d->al = d->cl, d->ac = ws, d->cc = we;
        }
        if (io->KeyCtrl) { // Ctrl+click: go to the definition
            igSetWindowFocus_Nil();
        }
    } else if (focused && igIsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        ImVec2 m = io->MousePos;
        int l = (int)((m.y - o.y) / lh), c = (int)((m.x - o.x - gutter) / cw + 0.5f);
        int al = d->al, ac = d->ac;
        move_to(d, l, c, true);
        if (al >= 0) d->al = al, d->ac = ac;
    }
    // the cursor kept in view
    if (d->scroll_to) {
        float cy = (float)d->cl * lh;
        if (cy < sy) igSetScrollY_Float(cy);
        else if (cy + lh * 2 > sy + avail.y) igSetScrollY_Float(cy + lh * 2 - avail.y);
        float cx = gutter + (float)d->cc * cw;
        if (cx < sx + gutter) igSetScrollX_Float(cx - gutter);
        else if (cx > sx + avail.x - cw * 2) igSetScrollX_Float(cx - avail.x + cw * 2);
        d->scroll_to = false;
    }
    ScriptFile* sf = doc_file(d);
    int first = (int)(sy / lh), last = (int)((sy + avail.y) / lh) + 1;
    if (last > d->nl) last = d->nl;
    int sa = -1, sb = -1;
    if (has_sel(d)) sel_range(d, &sa, &sb);
    static uint8_t colbuf[4096];
    for (int l = first < 0 ? 0 : first; l < last; l++) {
        float y = o.y + (float)l * lh, x0 = o.x + gutter;
        // the gutter: the line's number, its error / warning
        ScriptDiag* dg = diag_at(sf, l + 1);
        char num[16];
        snprintf(num, sizeof num, "%5d", l + 1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + sx, y }, l == d->cl ? TH(TK_N14) : TH(TK_N11), num, NULL);
        if (sf && sf->cls && bp_debug_line_has(sf->cls, l + 1)) // (the debugger: its breakpoint, the paused line)
            ImDrawList_AddCircleFilled(dl, (ImVec2){ o.x + sx + cw * 0.5f, y + lh * 0.5f }, lh * 0.32f, TH(TK_RED), 12);
        if (sf && sf->cls && bp_debug_paused_line(sf->cls) == l + 1) {
            ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y }, (ImVec2){ x0 + cw * (float)(maxc + 4), y + lh }, THA(TK_AMBER, 80), 0, 0);
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ o.x + sx + cw * 4.6f, y + 2 }, (ImVec2){ o.x + sx + cw * 5.8f, y + lh * 0.5f },
                                         (ImVec2){ o.x + sx + cw * 4.6f, y + lh - 2 }, TH(TK_AMBER));
        }
        if (dg) ImDrawList_AddCircleFilled(dl, (ImVec2){ o.x + sx + cw * 5.6f, y + lh * 0.5f }, cw * 0.32f, dg->error ? TH(TK_RED) : TH(TK_AMBER), 10);
        if (l == d->cl) ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y }, (ImVec2){ x0 + cw * (float)(maxc + 4), y + lh }, THA(TK_N15, 24), 0, 0);
        int n = line_len(d, l), base = d->ls[l];
        // the selection
        if (sa >= 0 && sb > base && sa <= base + n) {
            int a = sa > base ? sa - base : 0, b = sb < base + n ? sb - base : n + 1;
            ImDrawList_AddRectFilled(dl, (ImVec2){ x0 + (float)a * cw, y }, (ImVec2){ x0 + (float)b * cw, y + lh }, THA(TK_BLUE_D, 128), 0, 0);
        }
        // the text, colored
        if (n > (int)sizeof colbuf) n = (int)sizeof colbuf;
        colorize(d, l, colbuf);
        const char* s = d->text + base;
        for (int i = 0; i < n;) {
            int j = i;
            while (j < n && colbuf[j] == colbuf[i]) j++;
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + (float)i * cw, y }, TH(g_col[colbuf[i]]), s + i, s + j);
            i = j;
        }
        // the error's squiggle under its word
        if (dg) {
            int c0 = dg->col - 1 < n ? (dg->col > 0 ? dg->col - 1 : 0) : 0, c1 = c0;
            while (c1 < n && (isalnum((unsigned char)s[c1]) || s[c1] == '_' || s[c1] == '.' || s[c1] == '"')) c1++;
            if (c1 == c0) c1 = c0 + 1 < n ? n : c0 + 1;
            uint32_t col = dg->error ? TH(TK_RED) : TH(TK_AMBER);
            for (float x = x0 + (float)c0 * cw; x < x0 + (float)c1 * cw; x += 4.0f)
                ImDrawList_AddLine(dl, (ImVec2){ x, y + lh - 2 }, (ImVec2){ x + 2, y + lh }, col, 1.0f), ImDrawList_AddLine(dl, (ImVec2){ x + 2, y + lh }, (ImVec2){ x + 4, y + lh - 2 }, col, 1.0f);
            ImVec2 m = io->MousePos;
            if (hovered && m.y >= y && m.y < y + lh && m.x >= x0 + (float)c0 * cw && m.x < x0 + (float)c1 * cw + cw)
                igSetTooltip("%s: %s", dg->error ? TR("Error") : TR("Warning"), dg->text);
        }
    }
    // the cursor (blinking)
    if (focused && fmod(igGetTime(), 1.0) < 0.6) {
        float x = o.x + gutter + (float)d->cc * cw, y = o.y + (float)d->cl * lh;
        ImDrawList_AddLine(dl, (ImVec2){ x, y + 1 }, (ImVec2){ x, y + lh - 1 }, TH(TK_N15), 1.5f);
    }
    // hover: an engine function's signature
    if (hovered && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
        ImVec2 m = io->MousePos;
        int l = (int)((m.y - o.y) / lh), c = (int)((m.x - o.x - gutter) / cw);
        if (l >= 0 && l < d->nl && c >= 0 && c < line_len(d, l)) {
            const char* s = d->text + d->ls[l];
            if (isalnum((unsigned char)s[c]) || s[c] == '_') {
                int a = c, b = c;
                while (a > 0 && (isalnum((unsigned char)s[a - 1]) || s[a - 1] == '_')) a--;
                while (b < line_len(d, l) && (isalnum((unsigned char)s[b]) || s[b] == '_')) b++;
                char w[64];
                snprintf(w, sizeof w, "%.*s", b - a, s + a);
                const ScriptApi* api = !diag_at(sf, l + 1) ? script_api_find(w) : NULL;
                if (api) igSetTooltip("%s\n\n%s\n(%s)", api->sig, api->doc ? api->doc : "", api->category ? api->category : "");
            }
        }
    }
    // completion's list, under the cursor
    if (I.comp && I.comp_n && focused) {
        float x = o.x + gutter + (float)word_start(d, d->cl, d->cc) * cw, y = o.y + (float)(d->cl + 1) * lh;
        igSetNextWindowPos((ImVec2){ x, y }, ImGuiCond_Always, (ImVec2){ 0, 0 });
        igSetNextWindowSize((ImVec2){ cw * 70.0f, 0 }, ImGuiCond_Always);
        if (igBegin("##completion", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            int shown = I.comp_n < 12 ? I.comp_n : 12, from = I.comp_sel >= shown ? I.comp_sel - shown + 1 : 0;
            for (int i = from; i < from + shown && i < I.comp_n; i++) {
                static const uint32_t kc[] = { TK_BLUE_L, TK_TEAL, TK_TEAL, TK_N14, TK_N14, TK_N14, TK_AMBER_L, TK_BLUE };
                igPushStyleColor_U32(ImGuiCol_Text, TH(kc[I.items[i].kind < 8 ? I.items[i].kind : 3]));
                if (igSelectable_Bool(I.items[i].name, i == I.comp_sel, 0, (ImVec2){ cw * 28.0f, 0 })) I.comp_sel = i, comp_accept(d);
                igPopStyleColor(1);
                igSameLine(cw * 29.0f, -1);
                igTextDisabled("%s", I.items[i].detail);
            }
            if (I.comp_sel < I.comp_n && I.items[I.comp_sel].doc) {
                igSeparator();
                igPushTextWrapPos(cw * 68.0f);
                igTextWrapped("%s", I.items[I.comp_sel].doc);
                igPopTextWrapPos();
            }
        }
        igEnd();
    } else if (focused) { // the signature of the call being typed
        char fn[64];
        int arg;
        if (call_at_cursor(d, fn, sizeof fn, &arg)) {
            const ScriptApi* api = script_api_find(fn);
            if (api) {
                float x = o.x + gutter + (float)d->cc * cw, y = o.y + (float)d->cl * lh - lh * 1.6f;
                igSetNextWindowPos((ImVec2){ x, y }, ImGuiCond_Always, (ImVec2){ 0, 0 });
                if (igBegin("##signature", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs)) {
                    // the argument being typed in bold color
                    const char* p = strchr(api->sig, '(');
                    igTextUnformatted(api->sig, p ? p + 1 : NULL);
                    int k = 0;
                    const char* s = p ? p + 1 : "";
                    while (*s && *s != ')') {
                        const char* e = s;
                        while (*e && *e != ',' && *e != ')') e++;
                        igSameLine(0, 0);
                        igPushStyleColor_U32(ImGuiCol_Text, k == arg ? TH(TK_WARN) : TH(TK_N13));
                        igTextUnformatted(s, e);
                        igPopStyleColor(1);
                        if (*e == ',') igSameLine(0, 0), igTextUnformatted(",", NULL), e++;
                        s = e, k++;
                    }
                    igSameLine(0, 0);
                    igTextUnformatted(")", NULL);
                }
                igEnd();
            }
        }
    }
    igEndChild();
    if (g_code_font) igPopFont();
}

static void find_bar(Doc* d) {
    if (!I.find_open) return;
    igSetNextItemWidth(igGetFontSize() * 14.0f);
    bool next = igInputTextWithHint("##find", TR("Find"), I.find, sizeof I.find, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSameLine(0, 4);
    next |= igButton(TR("Next"), (ImVec2){ 0, 0 });
    igSameLine(0, 10);
    igSetNextItemWidth(igGetFontSize() * 14.0f);
    igInputTextWithHint("##repl", TR("Replace with"), I.repl, sizeof I.repl, 0, NULL, NULL);
    igSameLine(0, 4);
    bool one = igButton(TR("Replace"), (ImVec2){ 0, 0 });
    igSameLine(0, 4);
    bool all = igButton(TR("Replace All"), (ImVec2){ 0, 0 });
    igSameLine(0, 4);
    if (igSmallButton("x")) I.find_open = false;
    size_t fl = strlen(I.find);
    if (!fl) return;
    if (all) {
        snapshot(d, false);
        int count = 0;
        for (size_t i = 0; i + fl <= d->len;) {
            if (!strncmp(d->text + i, I.find, fl)) {
                replace_range(d, (int)i, (int)(i + fl), I.repl, strlen(I.repl));
                i += strlen(I.repl), count++;
            } else i++;
        }
        snprintf(I.status, sizeof I.status, TR("%d replaced."), count);
        return;
    }
    if (one && has_sel(d)) insert_text(d, I.repl, strlen(I.repl), false), next = true;
    if (next) {
        int from = off_of(d, d->cl, d->cc);
        const char* hit = strstr(d->text + from, I.find);
        if (!hit) hit = strstr(d->text, I.find);
        if (hit) {
            int a = (int)(hit - d->text);
            int l, c;
            pos_of(d, a, &l, &c);
            d->al = l, d->ac = c;
            pos_of(d, a + (int)fl, &d->cl, &d->cc);
            d->scroll_to = true;
        }
    }
}

// --- The window ---
void script_ide_draw(struct Editor* ed, double now, float dt) {
    (void)now, (void)dt, (void)ed;
    I.focused = false;
    script_api_reference_draw();
    editor_utilities_draw(ed);
    if (!I.open || !I.n) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 64.0f, igGetFontSize() * 42.0f }, ImGuiCond_FirstUseEver);
    if (g_script_ide_demo) {
        const ImGuiViewport* vp = igGetMainViewport();
        igSetNextWindowPos((ImVec2){ vp->WorkPos.x + 24, vp->WorkPos.y + 60 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
        igSetNextWindowSize((ImVec2){ vp->WorkSize.x * 0.56f, vp->WorkSize.y * 0.86f }, ImGuiCond_Always);
    }
    bool open = true;
    char title[96];
    snprintf(title, sizeof title, "%s###script_ide", TR("Code Editor"));
    if (!igBegin(title, &open, ImGuiWindowFlags_MenuBar)) {
        igEnd();
        if (!open) I.open = false;
        return;
    }
    Doc* d = I.cur >= 0 && I.cur < I.n ? I.d[I.cur] : I.d[0];
    // the menu: file, edit, help
    if (igBeginMenuBar()) {
        if (igBeginMenu(TR("File"), true)) {
            if (ui_menu_item(ICON_SAVE, TR("Save"), "Ctrl+S", false, true)) doc_save(d);
            if (ui_menu_item(ICON_SAVE, TR("Save All"), NULL, false, true))
                for (int i = 0; i < I.n; i++)
                    if (I.d[i]->dirty) doc_save(I.d[i]);
            if (ui_menu_item(ICON_OPEN, TR("Show in Explorer"), NULL, false, true)) {
                char cmd[700];
                snprintf(cmd, sizeof cmd, "%s", d->path);
                for (char* c = cmd; *c; c++)
                    if (*c == '/') *c = '\\';
                re_show_in_explorer(cmd);
            }
            igEndMenu();
        }
        if (igBeginMenu(TR("Edit"), true)) {
            if (ui_menu_item(ICON_SEARCH, TR("Find / Replace"), "Ctrl+F", false, true)) I.find_open = true;
            if (ui_menu_item(ICON_ARROW, TR("Go to Line"), "Ctrl+G", false, true)) d->goto_line = 1;
            if (ui_menu_item(ICON_COMMENT, TR("Toggle Comment"), "Ctrl+/", false, true)) toggle_comment(d);
            if (ui_menu_item(ICON_SCRIPT, TR("Complete"), "Ctrl+Space", false, true)) I.comp = true, I.comp_word[0] = 0, comp_build(d);
            igEndMenu();
        }
        if (igBeginMenu(TR("Help"), true)) {
            if (ui_menu_item(ICON_HELP, TR("API Reference"), "F1", false, true)) script_api_reference_open(NULL);
            if (ui_menu_item(ICON_SETTINGS, TR("Editor Utilities"), NULL, false, true)) editor_utilities_open();
            igEndMenu();
        }
        igEndMenuBar();
    }
    // the tabs
    if (igBeginTabBar("##docs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < I.n; i++) {
            bool keep = true;
            char tab[96];
            snprintf(tab, sizeof tab, "%s%s###doc%d", I.d[i]->name, I.d[i]->dirty ? " *" : "", i);
            ImGuiTabItemFlags fl = i == I.cur && I.d[i]->scroll_to ? ImGuiTabItemFlags_SetSelected : 0;
            if (igBeginTabItem(tab, &keep, fl)) {
                I.cur = i;
                igEndTabItem();
            }
            if (!keep) { // closed (saved first: no work lost)
                if (I.d[i]->dirty) doc_save(I.d[i]);
                for (int k = 0; k < I.d[i]->nundo; k++) free(I.d[i]->undo[k].text);
                free(I.d[i]->text), free(I.d[i]->ls), free(I.d[i]->lstate), free(I.d[i]);
                memmove(&I.d[i], &I.d[i + 1], sizeof(Doc*) * (size_t)(I.n - i - 1));
                I.n--, i--;
                if (I.cur >= I.n) I.cur = I.n - 1;
            }
        }
        igEndTabBar();
    }
    if (!I.n) {
        igEnd();
        I.open = false;
        return;
    }
    d = I.d[I.cur >= 0 ? I.cur : 0];
    // the live check: half a second after the typing stopped, the text compiled (nothing written, the class kept)
    ScriptFile* sf = doc_file(d);
    if (d->edited && re_time_seconds() - d->edit_t > 0.5 && sf) {
        script_compile_text(sf, d->text, d->len, false);
        d->edited = false;
    }
    find_bar(d);
    if (d->goto_line) {
        static int target = 1;
        if (d->goto_line == 1) igOpenPopup_Str("goto", 0), target = d->cl + 1, d->goto_line = 2;
        if (igBeginPopup("goto", 0)) {
            igSetKeyboardFocusHere(0);
            if (igInputInt(TR("Line"), &target, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
                move_to(d, target - 1, 0, false);
                igCloseCurrentPopup(), d->goto_line = 0;
            }
            igEndPopup();
        } else d->goto_line = 0;
    }
    // the code, then the problems
    ImVec2 av = igGetContentRegionAvail();
    uint32_t nerr = 0, nwarn = 0;
    for (uint32_t i = 0; sf && i < sf->ndiags; i++) sf->diags[i].error ? nerr++ : nwarn++;
    float probs = (nerr + nwarn) ? igGetFontSize() * (float)((nerr + nwarn) < 5 ? (nerr + nwarn) + 2 : 7) * 1.25f : 0.0f;
    draw_code(d, av.y - probs - igGetFrameHeightWithSpacing());
    if (nerr + nwarn) {
        igBeginChild_Str("problems", (ImVec2){ 0, probs }, ImGuiChildFlags_Borders, 0);
        for (uint32_t i = 0; i < sf->ndiags; i++) {
            ScriptDiag* g = &sf->diags[i];
            igPushID_Int((int)i);
            igPushStyleColor_U32(ImGuiCol_Text, g->error ? TH(TK_ERROR) : TH(TK_WARN));
            char row[260];
            snprintf(row, sizeof row, "%s  %s:%d:%d  %s", g->error ? "x" : "!", d->name, g->line, g->col, g->text);
            if (igSelectable_Bool(row, false, 0, (ImVec2){ 0, 0 })) move_to(d, g->line - 1, g->col - 1, false), igSetWindowFocus_Str("code");
            igPopStyleColor(1);
            igPopID();
        }
        igEndChild();
    }
    // the status line
    igTextDisabled("%s  |  %s %d, %s %d  |  %s  |  %u %s, %u %s  |  %.2f ms, %u nodes, %u ops%s%s", g_script_lang_names[d->lang], TR("Line"),
                   d->cl + 1, TR("Col"), d->cc + 1, sf && sf->ok ? TR("compiles") : TR("errors"), nerr, TR("errors"), nwarn, TR("warnings"),
                   sf ? sf->compile_ms : 0.0, sf ? sf->nodes : 0, sf ? sf->ops : 0, I.status[0] ? "  |  " : "", I.status);
    igEnd();
    if (!open) {
        for (int i = 0; i < I.n; i++)
            if (I.d[i]->dirty) doc_save(I.d[i]);
        I.open = false;
    }
}

bool script_ide_add_menu(const char* folder) {
    bool ok = !strncmp(folder, "/Content", 8);
    if (!ui_begin_menu(ICON_FUNCTION, TR("Script"), ok)) return false;
    bool made = false;
    for (int l = 0; l < SL_COUNT; l++) {
        char label[48];
        snprintf(label, sizeof label, "%s %s", g_script_lang_names[l], TR("Script"));
        if (!ui_menu_item(ICON_FUNCTION, label, NULL, false, true)) continue;
        char vp[700];
        for (int k = 0; k < 1000; k++) {
            char nm[48];
            snprintf(nm, sizeof nm, k ? "NewScript%d" : "NewScript", k + 1);
            if (script_create(folder, nm, (ScriptLang)l, "Actor", vp, sizeof vp)) {
                content_mark_dirty();
                script_ide_open(vp);
                made = true;
                break;
            }
        }
    }
    igSeparator();
    if (ui_menu_item(ICON_FUNCTION, TR("Editor Utility (Real)"), NULL, false, true)) {
        char vp[700];
        for (int k = 0; k < 1000; k++) {
            char nm[48];
            snprintf(nm, sizeof nm, k ? "EUW_Tool%d" : "EUW_Tool", k + 1);
            if (script_create(folder, nm, SL_REAL, "EditorUtility", vp, sizeof vp)) {
                content_mark_dirty();
                script_ide_open(vp);
                made = true;
                break;
            }
        }
    }
    igEndMenu();
    return made;
}

// --ide-demo: a document opened at a line and column, a word typed there with its completion open
void script_ide_demo(const char* vpath, int line, int col, const char* word) {
    g_script_ide_demo = true;
    script_ide_open_at(vpath, line);
    Doc* d = I.cur >= 0 && I.cur < I.n ? I.d[I.cur] : NULL;
    if (!d) return;
    d->cc = col - 1;
    if (word && word[0]) {
        snprintf(I.comp_word, sizeof I.comp_word, "%s", word);
        I.comp = true, I.comp_sel = 0;
        comp_build(d);
    }
}
