// The scripts' front end (script_internal.h): one lexer for the four languages, two parser families — indentation
// (Real, Python) and braces (C#, C) — into one tree. Errors carry their line and column; the parser goes on after one
// (to the next line / statement) so the IDE sees them all.
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "script_internal.h"

const char* const g_sty_names[ST_COUNT] = { "?", "void", "bool", "int", "float", "string", "Vector", "Rotator", "Actor",
                                            "Color", "Transform", "int64", "Object", "Class" };

// --- The arena ---
void* sa_alloc(SArena* a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->nblocks || a->used + n > a->cap) {
        size_t cap = n > 65536 ? n : 65536;
        if (a->nblocks >= 256) return NULL;
        char* b = calloc(1, cap); // (a compile's memory: freed at once by sa_free)
        if (!b) return NULL;
        a->blocks[a->nblocks++] = b, a->used = 0, a->cap = cap;
    }
    void* p = a->blocks[a->nblocks - 1] + a->used;
    a->used += n;
    return p;
}
const char* sa_str(SArena* a, const char* s, size_t n) {
    char* p = sa_alloc(a, n + 1);
    if (!p) return "";
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
void sa_free(SArena* a) {
    for (int i = 0; i < a->nblocks; i++) free(a->blocks[i]);
    memset(a, 0, sizeof *a);
}

void s_diag(SCtx* c, int line, int col, bool error, const char* fmt, ...) {
    if (!c->ndiags || *c->ndiags >= SCRIPT_MAX_DIAGS) return;
    for (uint32_t i = 0; i < *c->ndiags; i++) // (one message per place: the parser's recovery repeats them)
        if (c->diags[i].line == line && c->diags[i].col == col) return;
    ScriptDiag* d = &c->diags[(*c->ndiags)++];
    d->line = line, d->col = col, d->error = error;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->text, sizeof d->text, fmt, ap);
    va_end(ap);
}

void s_sym(SCtx* c, ScriptSymKind kind, const char* name, const char* type, int line, int col, int from, int to) {
    if (c->nsyms == c->cap_syms) {
        uint32_t cap = c->cap_syms ? c->cap_syms * 2 : 64;
        ScriptSym* n = realloc(c->syms, cap * sizeof *n); // (handed to the ScriptFile: freed with its next compile)
        if (!n) return;
        c->syms = n, c->cap_syms = cap;
    }
    ScriptSym* s = &c->syms[c->nsyms++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->type, sizeof s->type, "%s", type ? type : "");
    s->kind = (uint8_t)kind, s->line = line, s->col = col, s->scope_from = from, s->scope_to = to;
}

uint8_t s_struct_type(SCtx* c, const char* name) {
    if (!c || !name || !name[0]) return ST_UNKNOWN;
    for (int i = 0; i < c->nsnames; i++)
        if (!strcmp(c->snames[i], name)) return (uint8_t)(ST_STRUCT + i);
    if (c->nsnames >= S_MAX_STRUCTS || !bp_find_name(BPA_STRUCT, name)) return ST_UNKNOWN;
    c->snames[c->nsnames] = sa_str(&c->arena, name, strlen(name));
    return (uint8_t)(ST_STRUCT + c->nsnames++);
}

const char* s_ty_name(SCtx* c, uint8_t ty) {
    uint8_t t = ty & 0x7F;
    if (ST_IS_STRUCT(t)) return c && t - ST_STRUCT < c->nsnames ? c->snames[t - ST_STRUCT] : "struct";
    return t < ST_COUNT ? g_sty_names[t] : "?";
}

uint8_t s_type_named(const char* n) {
    static const struct { const char* name; uint8_t t; } map[] = {
        { "void", ST_VOID }, { "None", ST_VOID }, { "bool", ST_BOOL }, { "boolean", ST_BOOL }, { "Bool", ST_BOOL },
        { "int", ST_INT }, { "Int", ST_INT }, { "int32", ST_INT }, { "Int32", ST_INT }, { "short", ST_INT }, { "uint", ST_INT },
        { "uint32_t", ST_INT }, { "int32_t", ST_INT }, { "byte", ST_INT }, { "char", ST_INT },
        { "long", ST_INT64 }, { "int64", ST_INT64 }, { "Int64", ST_INT64 }, { "int64_t", ST_INT64 },
        { "float", ST_FLOAT }, { "Float", ST_FLOAT }, { "double", ST_FLOAT }, { "Double", ST_FLOAT }, { "Single", ST_FLOAT },
        { "string", ST_STRING }, { "String", ST_STRING }, { "str", ST_STRING }, { "text", ST_STRING }, { "Text", ST_STRING },
        { "Name", ST_STRING },
        { "Vector", ST_VECTOR }, { "Vector3", ST_VECTOR }, { "vec3", ST_VECTOR }, { "FVector", ST_VECTOR },
        { "Rotator", ST_ROTATOR }, { "FRotator", ST_ROTATOR }, { "Actor", ST_ACTOR }, { "AActor", ST_ACTOR },
        { "Pawn", ST_ACTOR }, { "Character", ST_ACTOR }, { "GameObject", ST_ACTOR },
        { "Color", ST_COLOR }, { "LinearColor", ST_COLOR }, { "FLinearColor", ST_COLOR },
        { "Transform", ST_TRANSFORM }, { "FTransform", ST_TRANSFORM }, { "Object", ST_OBJECT }, { "object", ST_OBJECT },
        { "Class", ST_CLASS }, { "Type", ST_CLASS },
    };
    if (!n) return ST_UNKNOWN;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].name, n)) return map[i].t;
    return ST_UNKNOWN;
}

// --- The lexer ---
typedef struct Lex {
    SCtx* c;
    const char* s;
    size_t n, i;
    int line, col;
    Tok* t;
    int nt, cap;
    int indent[64], nindent, depth; // (Real / Python: the indentation stack; brackets open: no NEWLINE)
    bool indent_lang;
} Lex;

static Tok* push(Lex* L, uint8_t kind, int line, int col) {
    if (L->nt == L->cap) {
        int cap = L->cap ? L->cap * 2 : 1024;
        Tok* t = sa_alloc(&L->c->arena, (size_t)cap * sizeof *t);
        if (!t) return &L->t[L->nt - 1];
        if (L->nt) memcpy(t, L->t, (size_t)L->nt * sizeof *t);
        L->t = t, L->cap = cap;
    }
    Tok* t = &L->t[L->nt++];
    memset(t, 0, sizeof *t);
    t->kind = kind, t->line = line, t->col = col;
    return t;
}

static char peekc(Lex* L, size_t k) { return L->i + k < L->n ? L->s[L->i + k] : 0; }
static void adv(Lex* L, size_t k) {
    for (size_t j = 0; j < k && L->i < L->n; j++) {
        if (L->s[L->i] == '\n') L->line++, L->col = 1;
        else L->col++;
        L->i++;
    }
}

// A string's body (the quotes already met): escapes decoded; interp: braces kept for the parser
static const char* read_string(Lex* L, char q, bool triple, bool raw) {
    char buf[2048];
    size_t o = 0;
    while (L->i < L->n) {
        char ch = peekc(L, 0);
        if (triple && ch == q && peekc(L, 1) == q && peekc(L, 2) == q) {
            adv(L, 3);
            return sa_str(&L->c->arena, buf, o);
        }
        if (!triple && ch == q) {
            adv(L, 1);
            return sa_str(&L->c->arena, buf, o);
        }
        if (!triple && ch == '\n') break;
        if (ch == '\\' && !raw) {
            char e = peekc(L, 1);
            adv(L, 2);
            char r = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? 0 : e;
            if (o < sizeof buf - 1) buf[o++] = r;
            continue;
        }
        if (o < sizeof buf - 1) buf[o++] = ch;
        adv(L, 1);
    }
    s_diag(L->c, L->line, L->col, true, "This text has no closing quote");
    return sa_str(&L->c->arena, buf, o);
}

static void indentation(Lex* L) { // (at a line's start, Real / Python): INDENT / DEDENT
    int w = 0;
    size_t j = L->i;
    while (j < L->n && (L->s[j] == ' ' || L->s[j] == '\t')) w += L->s[j] == '\t' ? 4 : 1, j++;
    // a blank or comment line: no indentation change
    if (j >= L->n || L->s[j] == '\n' || L->s[j] == '\r' || L->s[j] == '#' || (L->s[j] == '/' && j + 1 < L->n && L->s[j + 1] == '/')) return;
    int top = L->nindent ? L->indent[L->nindent - 1] : 0;
    if (w > top && L->nindent < 64) L->indent[L->nindent++] = w, push(L, TK_INDENT, L->line, w + 1);
    else
        while (w < top) {
            L->nindent--;
            push(L, TK_DEDENT, L->line, w + 1);
            top = L->nindent ? L->indent[L->nindent - 1] : 0;
            if (w > top) {
                s_diag(L->c, L->line, w + 1, true, "The indentation doesn't match any block above");
                break;
            }
        }
}

Tok* s_lex(SCtx* c, const char* src, size_t len, int* ntok) {
    Lex L;
    memset(&L, 0, sizeof L);
    L.c = c, L.s = src, L.n = len, L.line = 1, L.col = 1;
    L.indent_lang = c->lang == SL_REAL || c->lang == SL_PYTHON;
    bool line_start = true;
    static const char* const ops3[] = { "..=", "**=", "<<=", ">>=", "===" };
    static const char* const ops2[] = { "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "->", "..", "++",
                                        "--", "::", "**", "=>", "<<", ">>", "//" };
    while (L.i < L.n) {
        if (line_start && L.indent_lang && !L.depth) indentation(&L);
        line_start = false;
        char ch = peekc(&L, 0);
        if (ch == '\n') {
            if (L.indent_lang && !L.depth && L.nt && L.t[L.nt - 1].kind != TK_NEWLINE && L.t[L.nt - 1].kind != TK_INDENT &&
                L.t[L.nt - 1].kind != TK_DEDENT)
                push(&L, TK_NEWLINE, L.line, L.col);
            adv(&L, 1);
            line_start = true;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            adv(&L, 1);
            continue;
        }
        // comments (and C's preprocessor lines, its "parent:" hint)
        bool hash_comment = ch == '#' && (c->lang == SL_REAL || c->lang == SL_PYTHON);
        bool pre = ch == '#' && (c->lang == SL_C || c->lang == SL_CSHARP);
        if (hash_comment || pre || (ch == '/' && peekc(&L, 1) == '/')) {
            size_t s0 = L.i;
            while (L.i < L.n && peekc(&L, 0) != '\n') adv(&L, 1);
            const char* p = strstr(sa_str(&c->arena, L.s + s0, L.i - s0), "parent:");
            if (p) { // "// parent: Character" (C's class parent): an identifier token pair the parser reads
                p += 7;
                while (*p == ' ') p++;
                size_t k = 0;
                while (isalnum((unsigned char)p[k]) || p[k] == '_' || p[k] == ' ') k++;
                while (k && p[k - 1] == ' ') k--;
                if (k) {
                    Tok* t = push(&L, TK_IDENT, L.line, 1);
                    t->s = "__parent__";
                    t = push(&L, TK_IDENT, L.line, 1);
                    t->s = sa_str(&c->arena, p, k);
                }
            }
            continue;
        }
        if (ch == '/' && peekc(&L, 1) == '*') {
            adv(&L, 2);
            while (L.i < L.n && !(peekc(&L, 0) == '*' && peekc(&L, 1) == '/')) adv(&L, 1);
            adv(&L, 2);
            continue;
        }
        int line = L.line, col = L.col;
        // strings: "..", '..' (Python), f"..", $"..", @"..", """.."""
        bool fpre = (ch == 'f' || ch == 'F') && c->lang == SL_PYTHON && (peekc(&L, 1) == '"' || peekc(&L, 1) == '\'');
        bool dpre = ch == '$' && c->lang == SL_CSHARP && peekc(&L, 1) == '"';
        bool atpre = ch == '@' && c->lang == SL_CSHARP && peekc(&L, 1) == '"';
        bool rpre = (ch == 'r' || ch == 'R') && c->lang == SL_PYTHON && (peekc(&L, 1) == '"' || peekc(&L, 1) == '\'');
        if (ch == '"' || (ch == '\'' && c->lang == SL_PYTHON) || fpre || dpre || atpre || rpre) {
            if (fpre || dpre || atpre || rpre) adv(&L, 1);
            char q = peekc(&L, 0);
            bool triple = peekc(&L, 1) == q && peekc(&L, 2) == q;
            adv(&L, triple ? 3 : 1);
            const char* s = read_string(&L, q, triple, atpre || rpre);
            bool interp = fpre || dpre || (c->lang == SL_REAL && strchr(s, '{'));
            Tok* t = push(&L, interp ? TK_FSTRING : TK_STRING, line, col);
            t->s = s;
            continue;
        }
        if (ch == '\'' && (c->lang == SL_C || c->lang == SL_CSHARP)) { // a char: its code
            adv(&L, 1);
            int v = peekc(&L, 0);
            if (v == '\\') adv(&L, 1), v = peekc(&L, 0) == 'n' ? '\n' : peekc(&L, 0);
            adv(&L, 1);
            if (peekc(&L, 0) == '\'') adv(&L, 1);
            Tok* t = push(&L, TK_INT, line, col);
            t->num = v;
            continue;
        }
        if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)peekc(&L, 1)))) {
            size_t s0 = L.i;
            bool flt = false;
            if (ch == '0' && (peekc(&L, 1) == 'x' || peekc(&L, 1) == 'X')) {
                adv(&L, 2);
                while (isxdigit((unsigned char)peekc(&L, 0))) adv(&L, 1);
            } else {
                while (isdigit((unsigned char)peekc(&L, 0)) || peekc(&L, 0) == '_') adv(&L, 1);
                if (peekc(&L, 0) == '.' && peekc(&L, 1) != '.') { // (not a range "0..10")
                    flt = true;
                    adv(&L, 1);
                    while (isdigit((unsigned char)peekc(&L, 0))) adv(&L, 1);
                }
                if (peekc(&L, 0) == 'e' || peekc(&L, 0) == 'E') {
                    flt = true;
                    adv(&L, 1);
                    if (peekc(&L, 0) == '+' || peekc(&L, 0) == '-') adv(&L, 1);
                    while (isdigit((unsigned char)peekc(&L, 0))) adv(&L, 1);
                }
            }
            char num[64];
            size_t k = 0;
            for (size_t j = s0; j < L.i && k < 63; j++)
                if (L.s[j] != '_') num[k++] = L.s[j];
            num[k] = 0;
            char suf = peekc(&L, 0);
            if (suf == 'f' || suf == 'F' || suf == 'd' || suf == 'D') flt = true, adv(&L, 1);
            else if (suf == 'L' || suf == 'l' || suf == 'u' || suf == 'U') adv(&L, 1);
            Tok* t = push(&L, flt ? TK_FLOAT : TK_INT, line, col);
            t->num = strtod(num, NULL);
            if (num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) t->num = (double)strtoull(num, NULL, 16);
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80) {
            size_t s0 = L.i;
            while (isalnum((unsigned char)peekc(&L, 0)) || peekc(&L, 0) == '_' || (unsigned char)peekc(&L, 0) >= 0x80) adv(&L, 1);
            Tok* t = push(&L, TK_IDENT, line, col);
            t->s = sa_str(&c->arena, L.s + s0, L.i - s0);
            continue;
        }
        // operators
        Tok* t = push(&L, TK_OP, line, col);
        size_t k = 1;
        for (size_t o = 0; o < sizeof ops3 / sizeof ops3[0] && k == 1; o++)
            if (!strncmp(L.s + L.i, ops3[o], 3)) k = 3;
        for (size_t o = 0; o < sizeof ops2 / sizeof ops2[0] && k == 1; o++)
            if (!strncmp(L.s + L.i, ops2[o], 2) && !(ops2[o][0] == '/' && ops2[o][1] == '/' && c->lang != SL_PYTHON)) k = 2;
        memcpy(t->op, L.s + L.i, k);
        if (strchr("([{", ch)) L.depth++;
        else if (strchr(")]}", ch) && L.depth > 0) L.depth--;
        if (!strchr("+-*/%=<>!&|^~()[]{},.:;?@$", ch)) s_diag(c, line, col, true, "Unexpected character '%c'", ch);
        adv(&L, k);
    }
    if (L.indent_lang) {
        if (L.nt && L.t[L.nt - 1].kind != TK_NEWLINE) push(&L, TK_NEWLINE, L.line, L.col);
        while (L.nindent) L.nindent--, push(&L, TK_DEDENT, L.line, 1);
    }
    push(&L, TK_EOF, L.line, L.col);
    *ntok = L.nt;
    return L.t;
}

// --- The parsers ---
typedef struct P {
    SCtx* c;
    Tok* t;
    int n, i;
    SClass* cls;
    SMethod* m;          // the method being read (its locals' scopes)
} P;

static Tok* cur(P* p) { return &p->t[p->i < p->n ? p->i : p->n - 1]; }
static Tok* ahead(P* p, int k) { return &p->t[p->i + k < p->n ? p->i + k : p->n - 1]; }
static bool is_op(Tok* t, const char* op) { return t->kind == TK_OP && !strcmp(t->op, op); }
static bool is_id(Tok* t, const char* s) { return t->kind == TK_IDENT && !strcmp(t->s, s); }
static bool accept_op(P* p, const char* op) {
    if (!is_op(cur(p), op)) return false;
    p->i++;
    return true;
}
static bool accept_id(P* p, const char* s) {
    if (!is_id(cur(p), s)) return false;
    p->i++;
    return true;
}
static const char* tok_text(Tok* t) {
    switch (t->kind) {
    case TK_EOF: return "the end of the file";
    case TK_NEWLINE: return "the end of the line";
    case TK_INDENT: return "an indented block";
    case TK_DEDENT: return "the end of the block";
    case TK_OP: return t->op;
    case TK_IDENT: return t->s;
    default: return "a value";
    }
}
static bool expect_op(P* p, const char* op) {
    if (accept_op(p, op)) return true;
    s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected '%s' here, not %s", op, tok_text(cur(p)));
    return false;
}
static const char* expect_ident(P* p, const char* what) {
    if (cur(p)->kind == TK_IDENT) return p->t[p->i++].s;
    s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected %s here, not %s", what, tok_text(cur(p)));
    return NULL;
}

static Expr* mk(P* p, uint8_t k, Tok* at) {
    Expr* e = sa_alloc(&p->c->arena, sizeof *e);
    if (!e) return NULL;
    e->k = k, e->line = at->line, e->col = at->col;
    return e;
}
static Stmt* mks(P* p, uint8_t k, Tok* at) {
    Stmt* s = sa_alloc(&p->c->arena, sizeof *s);
    if (!s) return NULL;
    s->k = k, s->line = at->line, s->col = at->col;
    return s;
}
// A growing list in the arena
typedef struct Vec { void** v; int n, cap; } Vec;
static void vpush(P* p, Vec* v, void* x) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        void** nv = sa_alloc(&p->c->arena, (size_t)cap * sizeof *nv);
        if (!nv) return;
        if (v->n) memcpy(nv, v->v, (size_t)v->n * sizeof *nv);
        v->v = nv, v->cap = cap;
    }
    v->v[v->n++] = x;
}

static Expr* parse_expr(P* p);

// A ':' inside an interpolation that starts a format ("{hp:.1f}", "{n:5}"), not a "::" or a ternary's
static bool c_is_format_colon(const char* s) {
    char n = s[1];
    return s[1] != ':' && (n == '.' || isdigit((unsigned char)n) || n == '<' || n == '>' || n == '^' || n == '+' || n == '#' ||
                           n == ',' || n == 'x' || n == 'X' || n == 'f' || n == 'd' || n == 'N' || n == 'F');
}

// "Hello {name}, {hp:.0f}" into the pieces: literals and expressions (each parsed in the file's language)
static Expr* parse_interp(P* p, Tok* t) {
    Expr* e = mk(p, EX_INTERP, t);
    Vec parts = { 0 };
    const char* s = t->s;
    size_t i = 0, n = strlen(s);
    char lit[1024];
    size_t o = 0;
    while (i <= n) {
        if (i == n || (s[i] == '{' && s[i + 1] != '{')) {
            if (o) {
                Expr* l = mk(p, EX_STRING, t);
                l->s = sa_str(&p->c->arena, lit, o);
                vpush(p, &parts, l);
                o = 0;
            }
            if (i == n) break;
            size_t j = i + 1;
            int depth = 1;
            while (j < n && depth) depth += s[j] == '{' ? 1 : s[j] == '}' ? -1 : 0, j++;
            size_t end = j - 1, colon = end;
            for (size_t k = i + 1; k < end; k++)
                if (s[k] == ':' && (c_is_format_colon(s + k))) { colon = k; break; }
            // (the expression's own tokens, parsed with the same language)
            int nt = 0;
            Tok* sub = s_lex(p->c, s + i + 1, colon - i - 1, &nt);
            for (int k = 0; k < nt; k++) sub[k].line = t->line, sub[k].col = t->col + (int)i + 1;
            P q = *p;
            q.t = sub, q.n = nt, q.i = 0;
            Expr* x = parse_expr(&q);
            if (x) vpush(p, &parts, x);
            i = j;
            continue;
        }
        if (s[i] == '{' && s[i + 1] == '{') i++; // ("{{": a brace)
        if (s[i] == '}' && s[i + 1] == '}') i++;
        if (o < sizeof lit - 1) lit[o++] = s[i];
        i++;
    }
    e->args = (Expr**)parts.v, e->nargs = parts.n;
    return e;
}

#include "script_grammar.inc"
