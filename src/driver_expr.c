// (Q19, notas-dev/importar-tlender.md) Driver expressions: Blender's "simple expressions" (the subset of Python it
// evaluates without Python), copied from the Tlender's rig_expr.c: compiled once to a stack bytecode by a recursive-descent
// parser with Python's precedence, then run each evaluation with no text work. Conditionals and and / or jump, so the
// branch not taken can't fail (`1 / x if x else 0`). The import bakes drivers with it; the Skeletal Meshes keep the
// expressions of the drivers of their morph targets and run them per frame (anim.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "driver_expr.h"

enum {
    OP_PUSH = 1, OP_VAR, OP_FRAME,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FLOORDIV, OP_MOD, OP_POW, OP_NEG, OP_NOT,
    OP_CMP,      // arg: comparison (CMP_*), + 8 keeps the right operand under the result (chains)
    OP_ANDV,     // x y -> y if x else x (the parts of a chained comparison)
    OP_JZ,       // pops; jumps by arg when zero
    OP_JMP,
    OP_AND_JMP,  // top falsy: jump (keeping it), else pop
    OP_OR_JMP,   // top truthy: jump (keeping it), else pop
    OP_CALL,     // arg: function * 16 + argument count
};
enum { CMP_LT = 0, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE };

typedef enum Fn {
    FN_MIN = 0, FN_MAX, FN_ABS, FN_FABS, FN_FLOOR, FN_CEIL, FN_TRUNC, FN_INT, FN_ROUND, FN_SQRT, FN_POW, FN_EXP, FN_LOG,
    FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_ATAN2, FN_RADIANS, FN_DEGREES, FN_FMOD, FN_SIGN, FN_CLAMP,
    FN_LERP, FN_SMOOTHSTEP,
    // the rest of Python's math module and builtins in Blender's driver namespace (full Python expressions)
    FN_ACOSH, FN_ASINH, FN_ATANH, FN_CBRT, FN_COPYSIGN, FN_COSH, FN_ERF, FN_ERFC, FN_EXP2, FN_EXPM1, FN_HYPOT, FN_ISFINITE,
    FN_ISINF, FN_ISNAN, FN_LDEXP, FN_LOG10, FN_LOG1P, FN_LOG2, FN_REMAINDER, FN_SINH, FN_TANH, FN_FMA, FN_FLOAT, FN_BOOL,
    FN_GAMMA, FN_LGAMMA, FN_FACTORIAL, FN_COUNT
} Fn;
static const struct {
    const char* name;
    int min_args, max_args;
} FUNCS[FN_COUNT] = {
    { "min", 1, 15 },  { "max", 1, 15 },   { "abs", 1, 1 },     { "fabs", 1, 1 },    { "floor", 1, 1 }, { "ceil", 1, 1 },
    { "trunc", 1, 1 }, { "int", 1, 1 },    { "round", 1, 2 },   { "sqrt", 1, 1 },    { "pow", 2, 2 },   { "exp", 1, 1 },
    { "log", 1, 2 },   { "sin", 1, 1 },    { "cos", 1, 1 },     { "tan", 1, 1 },     { "asin", 1, 1 },  { "acos", 1, 1 },
    { "atan", 1, 1 },  { "atan2", 2, 2 },  { "radians", 1, 1 }, { "degrees", 1, 1 }, { "fmod", 2, 2 },  { "sign", 1, 1 },
    { "clamp", 1, 3 }, { "lerp", 3, 3 },   { "smoothstep", 3, 3 },
    { "acosh", 1, 1 }, { "asinh", 1, 1 },  { "atanh", 1, 1 },   { "cbrt", 1, 1 },    { "copysign", 2, 2 }, { "cosh", 1, 1 },
    { "erf", 1, 1 },   { "erfc", 1, 1 },   { "exp2", 1, 1 },    { "expm1", 1, 1 },   { "hypot", 1, 15 },  { "isfinite", 1, 1 },
    { "isinf", 1, 1 }, { "isnan", 1, 1 },  { "ldexp", 2, 2 },   { "log10", 1, 1 },   { "log1p", 1, 1 },   { "log2", 1, 1 },
    { "remainder", 2, 2 }, { "sinh", 1, 1 }, { "tanh", 1, 1 }, { "fma", 3, 3 },     { "float", 0, 1 },   { "bool", 0, 1 },
    { "gamma", 1, 1 }, { "lgamma", 1, 1 }, { "factorial", 1, 1 },
};

// --- Tokens ---------------------------------------------------------------------------------------------------------
typedef enum Tok {
    T_END = 0, T_NUM, T_NAME, T_LPAREN, T_RPAREN, T_COMMA, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_DSLASH, T_PERCENT,
    T_POW, T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE, T_BAD
} Tok;

typedef struct Parser {
    const char* src;
    const char* p;
    Tok tok;
    const char* tok_start;
    int tok_len;
    double num;
    const char* const* vars;
    int var_count;
    DriverExpr* out;
    int depth, max_depth;
    double exact[RE_EXPR_CODE]; // each number pushed, as written (the code keeps it in float precision but in comparisons)
    char* err;
    int err_cap;
    bool failed;
} Parser;

static void fail(Parser* ps, const char* fmt, const char* what) {
    if (ps->failed) return;
    ps->failed = true;
    int col = (int)(ps->tok_start - ps->src) + 1;
    char msg[160];
    snprintf(msg, sizeof msg, fmt, what ? what : "");
    if (ps->err) snprintf(ps->err, (size_t)ps->err_cap, TR("%s (column %d)"), msg, col);
}

static void next(Parser* ps) {
    const char* p = ps->p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    ps->tok_start = p;
    if (!*p) {
        ps->tok = T_END;
        ps->tok_len = 0;
        ps->p = p;
        return;
    }
    char c = *p;
    if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
        char* end = NULL;
        ps->num = strtod(p, &end);
        ps->tok = end > p ? T_NUM : T_BAD;
        if (end > p && ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') || *end == '_')) ps->tok = T_BAD;
        p = end > p ? end : p + 1;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
        ps->tok = T_NAME;
    } else {
        p++;
        switch (c) {
        case '(': ps->tok = T_LPAREN; break;
        case ')': ps->tok = T_RPAREN; break;
        case ',': ps->tok = T_COMMA; break;
        case '+': ps->tok = T_PLUS; break;
        case '-': ps->tok = T_MINUS; break;
        case '%': ps->tok = T_PERCENT; break;
        case '*':
            ps->tok = *p == '*' ? T_POW : T_STAR;
            if (*p == '*') p++;
            break;
        case '/':
            ps->tok = *p == '/' ? T_DSLASH : T_SLASH;
            if (*p == '/') p++;
            break;
        case '<':
            ps->tok = *p == '=' ? T_LE : T_LT;
            if (*p == '=') p++;
            break;
        case '>':
            ps->tok = *p == '=' ? T_GE : T_GT;
            if (*p == '=') p++;
            break;
        case '=':
            ps->tok = *p == '=' ? T_EQ : T_BAD;
            if (*p == '=') p++;
            break;
        case '!':
            ps->tok = *p == '=' ? T_NE : T_BAD;
            if (*p == '=') p++;
            break;
        default: ps->tok = T_BAD; break;
        }
    }
    ps->tok_len = (int)(p - ps->tok_start);
    ps->p = p;
}

static bool is_word(const Parser* ps, const char* w) {
    return ps->tok == T_NAME && (int)strlen(w) == ps->tok_len && strncmp(ps->tok_start, w, (size_t)ps->tok_len) == 0;
}

static void emit(Parser* ps, int op, double arg, int stack_delta) {
    if (ps->failed) return;
    DriverExpr* e = ps->out;
    if (e->count >= RE_EXPR_CODE) {
        fail(ps, TR("Expression too long"), NULL);
        return;
    }
    e->op[e->count] = (uint8_t)op;
    e->arg[e->count] = arg;
    e->count++;
    ps->depth += stack_delta;
    if (ps->depth > ps->max_depth) ps->max_depth = ps->depth;
}

static int here(const Parser* ps) { return ps->out->count; }
static void patch(Parser* ps, int at) { // the jump at `at` lands here
    if (!ps->failed) ps->out->arg[at] = (float)(here(ps) - at);
}

// The code in [a, c) becomes [b, c) followed by [a, b) (relative jumps stay valid inside each block).
static void rotate_code(Parser* ps, int a, int b, int c) {
    if (ps->failed || a >= b || b >= c) return;
    DriverExpr* e = ps->out;
    uint8_t op[RE_EXPR_CODE];
    double arg[RE_EXPR_CODE];
    int n = c - a, k = 0;
    for (int i = b; i < c; i++, k++) op[k] = e->op[i], arg[k] = e->arg[i];
    for (int i = a; i < b; i++, k++) op[k] = e->op[i], arg[k] = e->arg[i];
    memcpy(e->op + a, op, (size_t)n);
    memcpy(e->arg + a, arg, sizeof arg[0] * (size_t)n);
}

static void parse_ternary(Parser* ps);

static void expect(Parser* ps, Tok t, const char* what) {
    if (ps->tok != t) {
        char got[32];
        snprintf(got, sizeof got, "%.*s", ps->tok_len > 20 ? 20 : ps->tok_len, ps->tok_start);
        if (ps->tok == T_END) fail(ps, TR("Expected '%s' at the end of the expression"), what);
        else {
            char msg[96];
            snprintf(msg, sizeof msg, TR("Expected '%s', found '%s'"), what, got);
            fail(ps, "%s", msg);
        }
        return;
    }
    next(ps);
}

// The end of a `self` reference's accessors after its name (".location[0]", "['prop']", '.constraints["IK"].influence');
// out: the reference without spaces ("self" + accessors).
static const char* self_ref_end(const char* p, char* out, int cap) {
    int n = 0;
    if (cap > 4) memcpy(out, "self", 4), n = 4;
    for (;;) {
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '.' && ((q[1] >= 'a' && q[1] <= 'z') || (q[1] >= 'A' && q[1] <= 'Z') || q[1] == '_')) {
            if (n + 1 < cap) out[n++] = '.';
            q++;
            while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_') {
                if (n + 1 < cap) out[n++] = *q;
                q++;
            }
            p = q;
        } else if (*q == '[') {
            int depth = 0;
            char quote = 0;
            do {
                char c = *q;
                if (!c) break;
                if (quote) {
                    if (c == '\\' && q[1]) {
                        if (n + 2 < cap) out[n++] = c, out[n++] = q[1];
                        q += 2;
                        continue;
                    }
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '[') {
                    depth++;
                } else if (c == ']') {
                    depth--;
                } else if (c == ' ' || c == '\t') {
                    q++;
                    continue;
                }
                if (n + 1 < cap) out[n++] = c;
                q++;
            } while (depth > 0);
            p = q;
        } else {
            break;
        }
    }
    out[n < cap ? n : cap - 1] = '\0';
    return p;
}

int driver_expr_self_refs(const char* text, char (*out)[RE_DRIVER_VAR_LEN], int cap) {
    int n = 0;
    for (const char* p = text ? text : ""; *p;) {
        bool start = p == text || !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9') ||
                                    p[-1] == '_' || p[-1] == '.');
        if (start && strncmp(p, "self", 4) == 0 &&
            !((p[4] >= 'a' && p[4] <= 'z') || (p[4] >= 'A' && p[4] <= 'Z') || (p[4] >= '0' && p[4] <= '9') || p[4] == '_')) {
            char ref[256];
            p = self_ref_end(p + 4, ref, (int)sizeof ref);
            bool seen = false;
            for (int i = 0; i < n && !seen; i++) seen = strcmp(out[i], ref) == 0;
            if (!seen && n < cap) snprintf(out[n++], RE_DRIVER_VAR_LEN, "%s", ref);
            continue;
        }
        if (*p == '"' || *p == '\'') { // strings are not references
            char q = *p++;
            while (*p && *p != q) p += *p == '\\' && p[1] ? 2 : 1;
            if (*p) p++;
            continue;
        }
        p++;
    }
    return n;
}

static void parse_atom(Parser* ps) {
    if (ps->failed) return;
    if (ps->tok == T_NUM) {
        // numbers in float precision, as Tlender's drivers always computed them (a comparison takes the exact one)
        if (ps->out->count < RE_EXPR_CODE) ps->exact[ps->out->count] = ps->num;
        emit(ps, OP_PUSH, (float)ps->num, 1);
        next(ps);
        return;
    }
    if (ps->tok == T_LPAREN) {
        next(ps);
        parse_ternary(ps);
        expect(ps, T_RPAREN, ")");
        return;
    }
    if (ps->tok == T_NAME) {
        char name[64];
        const char* name_at = ps->tok_start;
        snprintf(name, sizeof name, "%.*s", ps->tok_len > 63 ? 63 : ps->tok_len, ps->tok_start);
        if (strcmp(name, "and") == 0 || strcmp(name, "or") == 0 || strcmp(name, "if") == 0 || strcmp(name, "else") == 0 ||
            strcmp(name, "not") == 0) {
            fail(ps, TR("Unexpected symbol '%s'"), name);
            return;
        }
        if (strcmp(name, "self") == 0) { // Use Self: the reference is a variable named by its text (DVARF_SELF)
            char ref[256];
            ps->p = self_ref_end(ps->p, ref, (int)sizeof ref);
            int v = -1;
            for (int i = 0; i < ps->var_count && v < 0; i++)
                if (ps->vars[i] && strcmp(ps->vars[i], ref) == 0) v = i;
            if (v < 0) {
                ps->tok_start = name_at;
                fail(ps, TR("Unknown variable '%s'"), ref);
                return;
            }
            next(ps);
            emit(ps, OP_VAR, (float)v, 1);
            return;
        }
        next(ps);
        if (ps->tok == T_LPAREN) { // a call
            int fn = -1;
            for (int i = 0; i < FN_COUNT && fn < 0; i++)
                if (strcmp(FUNCS[i].name, name) == 0) fn = i;
            if (fn < 0) {
                ps->tok_start = name_at;
                fail(ps, TR("Unknown function '%s'"), name);
                return;
            }
            next(ps);
            int argc = 0;
            if (ps->tok != T_RPAREN) {
                for (;;) {
                    parse_ternary(ps);
                    argc++;
                    if (ps->failed) return;
                    if (ps->tok != T_COMMA) break;
                    next(ps);
                }
            }
            expect(ps, T_RPAREN, ")");
            if (ps->failed) return;
            if (argc < FUNCS[fn].min_args || argc > FUNCS[fn].max_args) {
                ps->tok_start = name_at;
                char msg[96];
                if (FUNCS[fn].min_args == FUNCS[fn].max_args)
                    snprintf(msg, sizeof msg, TR("'%s' takes %d argument(s)"), name, FUNCS[fn].min_args);
                else
                    snprintf(msg, sizeof msg, TR("'%s' takes %d to %d arguments"), name, FUNCS[fn].min_args,
                             FUNCS[fn].max_args);
                fail(ps, "%s", msg);
                return;
            }
            emit(ps, OP_CALL, (float)(fn * 16 + argc), 1 - argc);
            return;
        }
        if (strcmp(name, "pi") == 0) emit(ps, OP_PUSH, 3.14159265358979323846, 1);
        else if (strcmp(name, "e") == 0) emit(ps, OP_PUSH, 2.718281828459045, 1);
        else if (strcmp(name, "tau") == 0) emit(ps, OP_PUSH, 6.283185307179586, 1);
        else if (strcmp(name, "inf") == 0) emit(ps, OP_PUSH, INFINITY, 1);
        else if (strcmp(name, "nan") == 0) emit(ps, OP_PUSH, NAN, 1);
        else if (strcmp(name, "True") == 0) emit(ps, OP_PUSH, 1.0f, 1);
        else if (strcmp(name, "False") == 0) emit(ps, OP_PUSH, 0.0f, 1);
        else if (strcmp(name, "frame") == 0) emit(ps, OP_FRAME, 0.0f, 1);
        else {
            int v = -1;
            for (int i = 0; i < ps->var_count && v < 0; i++)
                if (ps->vars[i] && strcmp(ps->vars[i], name) == 0) v = i;
            if (v < 0) {
                ps->tok_start = name_at; // the column of the name, not of what follows
                fail(ps, TR("Unknown variable '%s'"), name);
                return;
            }
            emit(ps, OP_VAR, (float)v, 1);
        }
        return;
    }
    if (ps->tok == T_END) fail(ps, TR("Incomplete expression"), NULL);
    else {
        char got[32];
        snprintf(got, sizeof got, "%.*s", ps->tok_len > 20 ? 20 : ps->tok_len, ps->tok_start);
        fail(ps, TR("Unexpected symbol '%s'"), got);
    }
}

// Python: ** binds tighter than a unary minus on its left and is right associative; its right side may be unary.
static void parse_unary(Parser* ps);
static void parse_power(Parser* ps) {
    parse_atom(ps);
    if (ps->tok == T_POW) {
        next(ps);
        parse_unary(ps);
        emit(ps, OP_POW, 0, -1);
    }
}

static void parse_unary(Parser* ps) {
    if (ps->tok == T_MINUS) {
        next(ps);
        parse_unary(ps);
        emit(ps, OP_NEG, 0, 0);
    } else if (ps->tok == T_PLUS) {
        next(ps);
        parse_unary(ps);
    } else {
        parse_power(ps);
    }
}

static void parse_term(Parser* ps) {
    parse_unary(ps);
    while (!ps->failed && (ps->tok == T_STAR || ps->tok == T_SLASH || ps->tok == T_DSLASH || ps->tok == T_PERCENT)) {
        Tok t = ps->tok;
        next(ps);
        parse_unary(ps);
        emit(ps, t == T_STAR ? OP_MUL : t == T_SLASH ? OP_DIV : t == T_DSLASH ? OP_FLOORDIV : OP_MOD, 0, -1);
    }
}

static void parse_arith(Parser* ps) {
    parse_term(ps);
    while (!ps->failed && (ps->tok == T_PLUS || ps->tok == T_MINUS)) {
        Tok t = ps->tok;
        next(ps);
        parse_term(ps);
        emit(ps, t == T_PLUS ? OP_ADD : OP_SUB, 0, -1);
    }
}

static int cmp_kind(Tok t) {
    switch (t) {
    case T_LT: return CMP_LT;
    case T_LE: return CMP_LE;
    case T_GT: return CMP_GT;
    case T_GE: return CMP_GE;
    case T_EQ: return CMP_EQ;
    case T_NE: return CMP_NE;
    default: return -1;
    }
}

// a < b < c is (a < b) and (b < c), with b evaluated once.
// A comparison's operand that is a plain number (or its negation) compares as Python does, in double precision: a
// float32 variable of 0.1 is greater than the literal 0.1
static void exact_operand(Parser* ps, int start) {
    DriverExpr* e = ps->out;
    int n = e->count - start;
    if (ps->failed || start < 0 || start >= e->count || e->op[start] != OP_PUSH) return;
    if (n == 1 || (n == 2 && e->op[start + 1] == OP_NEG)) e->arg[start] = ps->exact[start];
}

static void parse_compare(Parser* ps) {
    int s0 = here(ps);
    parse_arith(ps);
    int links = 0;
    if (!ps->failed && cmp_kind(ps->tok) >= 0) exact_operand(ps, s0);
    while (!ps->failed && cmp_kind(ps->tok) >= 0) {
        int k = cmp_kind(ps->tok);
        next(ps);
        int s1 = here(ps);
        parse_arith(ps);
        exact_operand(ps, s1);
        bool more = cmp_kind(ps->tok) >= 0;
        emit(ps, OP_CMP, (float)(k + (more ? 8 : 0)), more ? 0 : -1);
        links++;
        if (!more && links > 1)
            for (int i = 1; i < links; i++) emit(ps, OP_ANDV, 0, -1);
    }
}

static void parse_not(Parser* ps) {
    if (is_word(ps, "not")) {
        next(ps);
        parse_not(ps);
        emit(ps, OP_NOT, 0, 0);
        return;
    }
    parse_compare(ps);
}

static void parse_and(Parser* ps) {
    parse_not(ps);
    while (!ps->failed && is_word(ps, "and")) {
        next(ps);
        int j = here(ps);
        emit(ps, OP_AND_JMP, 0, -1); // falls through with the left side popped
        parse_not(ps);
        patch(ps, j);
    }
}

static void parse_or(Parser* ps) {
    parse_and(ps);
    while (!ps->failed && is_word(ps, "or")) {
        next(ps);
        int j = here(ps);
        emit(ps, OP_OR_JMP, 0, -1);
        parse_and(ps);
        patch(ps, j);
    }
}

// a if c else b: c runs first, so the code of a moves after it.
static void parse_ternary(Parser* ps) {
    int start = here(ps);
    parse_or(ps);
    if (ps->failed || !is_word(ps, "if")) return;
    next(ps);
    int mid = here(ps);
    parse_or(ps);
    if (ps->failed) return;
    int end = here(ps);
    rotate_code(ps, start, mid, end); // now: [c][a]
    int a_start = start + (end - mid);
    // insert the JZ between c and a: shift a by one
    if (ps->out->count >= RE_EXPR_CODE) {
        fail(ps, TR("Expression too long"), NULL);
        return;
    }
    DriverExpr* e = ps->out;
    memmove(e->op + a_start + 1, e->op + a_start, (size_t)(e->count - a_start));
    memmove(e->arg + a_start + 1, e->arg + a_start, sizeof e->arg[0] * (size_t)(e->count - a_start));
    e->op[a_start] = OP_JZ;
    e->count++;
    ps->depth -= 2; // c popped by JZ; a's value replaced by b's on the other branch
    int jmp = here(ps);
    emit(ps, OP_JMP, 0, 0);
    e->arg[a_start] = (float)(here(ps) - a_start);
    if (!is_word(ps, "else")) {
        fail(ps, TR("Expected 'else'"), NULL);
        return;
    }
    next(ps);
    parse_ternary(ps);
    patch(ps, jmp);
}

bool driver_expr_compile(const char* text, const char* const* vars, int var_count, DriverExpr* out, char* err,
                         int err_cap) {
    Parser ps = { 0 };
    ps.src = ps.p = text ? text : "";
    ps.vars = vars;
    ps.var_count = var_count;
    ps.out = out;
    ps.err = err;
    ps.err_cap = err_cap;
    memset(out, 0, sizeof *out);
    if (err && err_cap) err[0] = '\0';
    next(&ps);
    if (ps.tok == T_END) {
        fail(&ps, TR("Empty expression"), NULL);
        return false;
    }
    parse_ternary(&ps);
    if (!ps.failed && ps.tok != T_END) {
        char got[32];
        snprintf(got, sizeof got, "%.*s", ps.tok_len > 20 ? 20 : ps.tok_len, ps.tok_start);
        fail(&ps, TR("Unexpected symbol '%s'"), got);
    }
    if (ps.failed) {
        out->count = 0;
        return false;
    }
    out->stack = ps.max_depth;
    return true;
}

// --- Evaluation -----------------------------------------------------------------------------------------------------
static double py_round(double x, int digits) {
    double m = pow(10.0, digits), y = x * m, r = floor(y + 0.5);
    if (r - y == 0.5 && fmod(r, 2.0) != 0.0) r -= 1.0; // half to even, as Python
    return r / m;
}

static double call(int fn, const double* a, int n, bool* ok) {
    switch (fn) {
    case FN_MIN: {
        double v = a[0];
        for (int i = 1; i < n; i++) v = a[i] < v ? a[i] : v;
        return v;
    }
    case FN_MAX: {
        double v = a[0];
        for (int i = 1; i < n; i++) v = a[i] > v ? a[i] : v;
        return v;
    }
    case FN_ABS:
    case FN_FABS: return fabs(a[0]);
    case FN_FLOOR: return floor(a[0]);
    case FN_CEIL: return ceil(a[0]);
    case FN_TRUNC:
    case FN_INT: return trunc(a[0]);
    case FN_ROUND: return py_round(a[0], n > 1 ? (int)a[1] : 0);
    case FN_SQRT:
        if (a[0] < 0) *ok = false;
        return a[0] < 0 ? 0.0 : sqrt(a[0]);
    case FN_POW:
        if ((a[0] < 0 && a[1] != floor(a[1])) || (a[0] == 0 && a[1] < 0)) {
            *ok = false;
            return 0.0;
        }
        return pow(a[0], a[1]);
    case FN_EXP: return exp(a[0]);
    case FN_LOG: {
        if (a[0] <= 0 || (n > 1 && (a[1] <= 0 || a[1] == 1))) {
            *ok = false;
            return 0.0;
        }
        return n > 1 ? log(a[0]) / log(a[1]) : log(a[0]);
    }
    case FN_SIN: return sin(a[0]);
    case FN_COS: return cos(a[0]);
    case FN_TAN: return tan(a[0]);
    case FN_ASIN:
    case FN_ACOS:
        if (a[0] < -1 || a[0] > 1) {
            *ok = false;
            return 0.0;
        }
        return fn == FN_ASIN ? asin(a[0]) : acos(a[0]);
    case FN_ATAN: return atan(a[0]);
    case FN_ATAN2: return atan2(a[0], a[1]);
    case FN_RADIANS: return a[0] * (3.14159265358979323846 / 180.0);
    case FN_DEGREES: return a[0] * (180.0 / 3.14159265358979323846);
    case FN_FMOD:
        if (a[1] == 0) {
            *ok = false;
            return 0.0;
        }
        return fmod(a[0], a[1]);
    case FN_SIGN: return a[0] > 0 ? 1.0 : a[0] < 0 ? -1.0 : 0.0;
    case FN_CLAMP: {
        double lo = n > 1 ? a[1] : 0.0, hi = n > 2 ? a[2] : 1.0;
        return a[0] < lo ? lo : a[0] > hi ? hi : a[0];
    }
    case FN_LERP: return a[0] + (a[1] - a[0]) * a[2];
    case FN_SMOOTHSTEP: {
        if (a[1] == a[0]) return a[2] < a[0] ? 0.0 : 1.0;
        double t = (a[2] - a[0]) / (a[1] - a[0]);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        return t * t * (3.0 - 2.0 * t);
    }
    case FN_ACOSH:
        if (a[0] < 1) *ok = false;
        return a[0] < 1 ? 0.0 : acosh(a[0]);
    case FN_ASINH: return asinh(a[0]);
    case FN_ATANH:
        if (a[0] <= -1 || a[0] >= 1) *ok = false;
        return a[0] <= -1 || a[0] >= 1 ? 0.0 : atanh(a[0]);
    case FN_CBRT: return cbrt(a[0]);
    case FN_COPYSIGN: return copysign(a[0], a[1]);
    case FN_COSH: return cosh(a[0]);
    case FN_ERF: return erf(a[0]);
    case FN_ERFC: return erfc(a[0]);
    case FN_EXP2: return exp2(a[0]);
    case FN_EXPM1: return expm1(a[0]);
    case FN_HYPOT: {
        double v = 0.0;
        for (int i = 0; i < n; i++) v = hypot(v, a[i]);
        return v;
    }
    case FN_ISFINITE: return isfinite(a[0]) ? 1.0 : 0.0;
    case FN_ISINF: return isinf(a[0]) ? 1.0 : 0.0;
    case FN_ISNAN: return isnan(a[0]) ? 1.0 : 0.0;
    case FN_LDEXP: return ldexp(a[0], (int)a[1]);
    case FN_LOG10:
    case FN_LOG1P:
    case FN_LOG2:
        if (fn == FN_LOG1P ? a[0] <= -1 : a[0] <= 0) {
            *ok = false;
            return 0.0;
        }
        return fn == FN_LOG10 ? log10(a[0]) : fn == FN_LOG2 ? log2(a[0]) : log1p(a[0]);
    case FN_REMAINDER:
        if (a[1] == 0) {
            *ok = false;
            return 0.0;
        }
        return remainder(a[0], a[1]);
    case FN_SINH: return sinh(a[0]);
    case FN_TANH: return tanh(a[0]);
    case FN_FMA: return a[0] * a[1] + a[2];
    case FN_FLOAT: return n ? a[0] : 0.0;
    case FN_BOOL: return n && a[0] != 0.0 ? 1.0 : 0.0;
    case FN_GAMMA:
        if (a[0] <= 0 && a[0] == floor(a[0])) {
            *ok = false;
            return 0.0;
        }
        return tgamma(a[0]);
    case FN_LGAMMA: return lgamma(a[0]);
    case FN_FACTORIAL: {
        if (a[0] < 0 || a[0] != floor(a[0]) || a[0] > 170) {
            *ok = false;
            return 0.0;
        }
        double v = 1.0;
        for (int i = 2; i <= (int)a[0]; i++) v *= i;
        return v;
    }
    default: return 0.0;
    }
}

float driver_expr_eval(const DriverExpr* e, const float* vars, float frame, bool* ok_out) {
    double st[RE_EXPR_CODE + 2];
    int sp = 0;
    bool ok = true;
    for (int pc = 0; pc < e->count; pc++) {
        int op = e->op[pc];
        double arg = e->arg[pc];
        switch (op) {
        case OP_PUSH: st[sp++] = arg; break;
        case OP_VAR: st[sp++] = vars ? vars[(int)arg] : 0.0; break;
        case OP_FRAME: st[sp++] = frame; break;
        case OP_ADD: sp--, st[sp - 1] += st[sp]; break;
        case OP_SUB: sp--, st[sp - 1] -= st[sp]; break;
        case OP_MUL: sp--, st[sp - 1] *= st[sp]; break;
        case OP_DIV:
        case OP_FLOORDIV:
        case OP_MOD: {
            double b = st[--sp], a = st[sp - 1];
            if (b == 0.0) {
                ok = false;
                st[sp - 1] = 0.0;
                break;
            }
            if (op == OP_DIV) st[sp - 1] = a / b;
            else if (op == OP_FLOORDIV) st[sp - 1] = floor(a / b);
            else st[sp - 1] = a - b * floor(a / b); // Python's sign follows the divisor
            break;
        }
        case OP_POW: {
            double b = st[--sp], a = st[sp - 1];
            if ((a < 0 && b != floor(b)) || (a == 0 && b < 0)) {
                ok = false;
                st[sp - 1] = 0.0;
            } else {
                st[sp - 1] = pow(a, b);
            }
            break;
        }
        case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
        case OP_NOT: st[sp - 1] = st[sp - 1] == 0.0 ? 1.0 : 0.0; break;
        case OP_CMP: {
            int k = (int)arg & 7;
            bool keep = (int)arg >= 8;
            double b = st[--sp], a = st[sp - 1];
            bool r = k == CMP_LT ? a < b : k == CMP_LE ? a <= b : k == CMP_GT ? a > b : k == CMP_GE ? a >= b
                   : k == CMP_EQ ? a == b : a != b;
            st[sp - 1] = r ? 1.0 : 0.0;
            if (keep) st[sp++] = b;
            break;
        }
        case OP_ANDV: {
            double y = st[--sp], x = st[sp - 1];
            st[sp - 1] = x != 0.0 ? y : x;
            break;
        }
        case OP_JZ:
            if (st[--sp] == 0.0) pc += (int)arg - 1;
            break;
        case OP_JMP: pc += (int)arg - 1; break;
        case OP_AND_JMP:
            if (st[sp - 1] == 0.0) pc += (int)arg - 1;
            else sp--;
            break;
        case OP_OR_JMP:
            if (st[sp - 1] != 0.0) pc += (int)arg - 1;
            else sp--;
            break;
        case OP_CALL: {
            int fn = (int)arg / 16, n = (int)arg % 16;
            double a[16];
            sp -= n;
            for (int i = 0; i < n; i++) a[i] = st[sp + i];
            st[sp++] = call(fn, a, n, &ok);
            break;
        }
        default: break;
        }
    }
    double r = sp > 0 ? st[sp - 1] : 0.0;
    if (r != r || r > 3.0e38 || r < -3.0e38) {
        ok = false;
        r = 0.0;
    }
    if (ok_out) *ok_out = ok;
    return (float)r;
}
