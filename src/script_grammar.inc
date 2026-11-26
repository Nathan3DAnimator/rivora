// The grammars (included by script_parse.c): expressions (one precedence climb for the four languages), the statements
// and the class of the indentation languages (Real, Python) and of the brace languages (C#, C).

// --- Expressions ---
static bool is_type_name(P* p, Tok* t) {
    return t->kind == TK_IDENT && (s_type_named(t->s) != ST_UNKNOWN || s_struct_type(p->c, t->s) != ST_UNKNOWN || s_asset_ref_named(t->s));
}

static Expr* parse_unary(P* p);
static Expr* parse_primary(P* p) {
    Tok* t = cur(p);
    Expr* e = NULL;
    switch (t->kind) {
    case TK_INT: p->i++, e = mk(p, EX_INT, t), e->num = t->num; return e;
    case TK_FLOAT: p->i++, e = mk(p, EX_FLOAT, t), e->num = t->num; return e;
    case TK_STRING: p->i++, e = mk(p, EX_STRING, t), e->s = t->s; return e;
    case TK_FSTRING: p->i++; return parse_interp(p, t);
    case TK_IDENT: {
        const char* s = t->s;
        if (!strcmp(s, "true") || !strcmp(s, "True") || !strcmp(s, "false") || !strcmp(s, "False")) {
            p->i++, e = mk(p, EX_BOOL, t), e->num = (s[0] == 't' || s[0] == 'T');
            return e;
        }
        if (!strcmp(s, "null") || !strcmp(s, "None") || !strcmp(s, "nullptr") || !strcmp(s, "NULL") || !strcmp(s, "nil"))
            return p->i++, mk(p, EX_NONE, t);
        if (!strcmp(s, "self") || !strcmp(s, "this")) return p->i++, mk(p, EX_SELF, t);
        if (p->c->lang == SL_CPP && (!strcmp(s, "static_cast") || !strcmp(s, "float") || !strcmp(s, "int") || !strcmp(s, "bool")) &&
            is_op(ahead(p, 1), "<") && !strcmp(s, "static_cast")) { // (C++) static_cast<int>(x) -> int(x)
            p->i += 2;
            Tok* ty = cur(p);
            if (ty->kind == TK_IDENT) p->i++;
            expect_op(p, ">");
            e = mk(p, EX_CALL, ty);
            e->s = ty->kind == TK_IDENT ? ty->s : "float";
            expect_op(p, "(");
            e->args = sa_alloc(&p->c->arena, sizeof(Expr*));
            if (e->args) e->args[0] = parse_expr(p), e->nargs = 1;
            expect_op(p, ")");
            return e;
        }
        if (!strcmp(s, "new") && p->c->lang == SL_CSHARP) { // new Vector3(1, 2, 3): its constructor
            p->i++;
            e = mk(p, EX_CALL, t);
            Expr* callee = mk(p, EX_NAME, cur(p));
            callee->s = expect_ident(p, "a type");
            if (accept_op(p, "<")) // (new List<int>(): an empty array)
                while (cur(p)->kind != TK_EOF && !accept_op(p, ">")) p->i++;
            e->s = callee->s;
            Vec args = { 0 };
            if (expect_op(p, "("))
                while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) {
                    vpush(p, &args, parse_expr(p));
                    if (!accept_op(p, ",") && !is_op(cur(p), ")")) {
                        expect_op(p, ")");
                        break;
                    }
                }
            e->args = (Expr**)args.v, e->nargs = args.n;
            return e;
        }
        p->i++;
        e = mk(p, EX_NAME, t);
        e->s = s;
        return e;
    }
    case TK_OP:
        if (is_op(t, "(")) {
            // C's / C#'s cast: (float)x -> float(x)
            if ((p->c->lang == SL_C || p->c->lang == SL_CSHARP || p->c->lang == SL_CPP) && is_type_name(p, ahead(p, 1)) && is_op(ahead(p, 2), ")") &&
                (ahead(p, 3)->kind == TK_IDENT || ahead(p, 3)->kind == TK_INT || ahead(p, 3)->kind == TK_FLOAT || is_op(ahead(p, 3), "(") ||
                 is_op(ahead(p, 3), "-"))) {
                Tok* ty = ahead(p, 1);
                p->i += 3;
                e = mk(p, EX_CALL, ty);
                e->s = ty->s;
                e->args = sa_alloc(&p->c->arena, sizeof(Expr*));
                if (e->args) e->args[0] = parse_unary(p), e->nargs = 1; // ((int)a.b: the cast takes a.b, as C reads it)
                return e;
            }
            p->i++;
            e = parse_expr(p);
            expect_op(p, ")");
            return e;
        }
        if (is_op(t, "[") || is_op(t, "{")) { // [1, 2, 3]: an array
            const char* close = is_op(t, "[") ? "]" : "}";
            p->i++;
            e = mk(p, EX_LIST, t);
            Vec items = { 0 };
            while (!accept_op(p, close) && cur(p)->kind != TK_EOF) {
                while (cur(p)->kind == TK_NEWLINE || cur(p)->kind == TK_INDENT || cur(p)->kind == TK_DEDENT) p->i++;
                if (accept_op(p, close)) break;
                vpush(p, &items, parse_expr(p));
                if (!accept_op(p, ",") && !is_op(cur(p), close)) {
                    expect_op(p, close);
                    break;
                }
            }
            e->args = (Expr**)items.v, e->nargs = items.n;
            return e;
        }
        break;
    default: break;
    }
    s_diag(p->c, t->line, t->col, true, "Expected a value here, not %s", tok_text(t));
    if (t->kind != TK_EOF && t->kind != TK_NEWLINE && !is_op(t, ";") && !is_op(t, "}") && t->kind != TK_DEDENT) p->i++;
    return mk(p, EX_INT, t);
}

static Expr* parse_postfix(P* p) {
    Expr* e = parse_primary(p);
    for (;;) {
        Tok* t = cur(p);
        if (is_op(t, "(")) {
            p->i++;
            Expr* call = mk(p, EX_CALL, t);
            call->line = e->line, call->col = e->col;
            if (e->k == EX_NAME) call->s = e->s; // f(x)
            else if (e->k == EX_MEMBER) call->s = e->s, call->a = e->a; // obj.f(x)
            else s_diag(p->c, t->line, t->col, true, "This can't be called");
            Vec args = { 0 };
            while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) {
                // (Python's / C#'s named arguments "volume=0.5": the value only, in order)
                if (cur(p)->kind == TK_IDENT && (is_op(ahead(p, 1), "=") || (is_op(ahead(p, 1), ":") && p->c->lang == SL_CSHARP))) p->i += 2;
                vpush(p, &args, parse_expr(p));
                if (!accept_op(p, ",") && !is_op(cur(p), ")")) {
                    expect_op(p, ")");
                    break;
                }
            }
            call->args = (Expr**)args.v, call->nargs = args.n;
            e = call;
        } else if (is_op(t, ".") || is_op(t, "->") || is_op(t, "::")) {
            p->i++;
            Expr* m = mk(p, EX_MEMBER, cur(p));
            m->a = e;
            m->s = expect_ident(p, "a member's name");
            if (!m->s) m->s = "?";
            // (Engine.PlaySound2D / RealEngine::X: the engine's namespace is the plain call)
            if (e->k == EX_NAME && (!strcmp(e->s, "Engine") || !strcmp(e->s, "RealEngine") || !strcmp(e->s, "real") ||
                                    !strcmp(e->s, "Mathf") || !strcmp(e->s, "Math") || !strcmp(e->s, "math")))
                m->a = NULL, m->k = EX_NAME;
            e = m;
        } else if (is_op(t, "[")) {
            p->i++;
            Expr* x = mk(p, EX_INDEX, t);
            x->a = e, x->b = parse_expr(p);
            expect_op(p, "]");
            e = x;
        } else break;
    }
    return e;
}

static Expr* parse_unary(P* p) {
    Tok* t = cur(p);
    if (is_op(t, "-") || is_op(t, "+") || is_op(t, "!") || is_op(t, "~") || is_id(t, "not")) {
        p->i++;
        Expr* e = mk(p, EX_UNARY, t);
        snprintf(e->op, sizeof e->op, "%s", is_op(t, "!") || is_id(t, "not") ? "not" : t->op);
        e->a = parse_unary(p);
        return e;
    }
    if (is_op(t, "++") || is_op(t, "--")) { // (a prefix increment as an expression: not supported, as a statement it is)
        s_diag(p->c, t->line, t->col, true, "'%s' works as a statement of its own (x++;)", t->op);
        p->i++;
    }
    Expr* e = parse_postfix(p);
    if (is_op(cur(p), "**")) { // Python's power: right associative
        Tok* o = cur(p);
        p->i++;
        Expr* b = mk(p, EX_BINARY, o);
        snprintf(b->op, sizeof b->op, "**");
        b->a = e, b->b = parse_unary(p);
        return b;
    }
    return e;
}

typedef struct OpInfo { const char* tok; const char* norm; int prec; } OpInfo;
static const OpInfo g_binops[] = {
    { "or", "or", 1 }, { "||", "or", 1 }, { "and", "and", 2 }, { "&&", "and", 2 },
    { "==", "==", 3 }, { "!=", "!=", 3 }, { "is", "==", 3 }, { "<", "<", 4 }, { ">", ">", 4 }, { "<=", "<=", 4 }, { ">=", ">=", 4 },
    { "+", "+", 5 }, { "-", "-", 5 }, { "*", "*", 6 }, { "/", "/", 6 }, { "%", "%", 6 }, { "//", "//", 6 },
    { "&", "&", 3 }, { "|", "|", 1 }, { "^", "^", 3 },
};
static const OpInfo* binop_at(P* p) {
    Tok* t = cur(p);
    for (size_t i = 0; i < sizeof g_binops / sizeof g_binops[0]; i++) {
        bool word = isalpha((unsigned char)g_binops[i].tok[0]);
        if (word ? is_id(t, g_binops[i].tok) : is_op(t, g_binops[i].tok)) return &g_binops[i];
    }
    return NULL;
}

static Expr* parse_binary(P* p, int min_prec) {
    Expr* lhs = parse_unary(p);
    for (;;) {
        const OpInfo* o = binop_at(p);
        if (!o || o->prec < min_prec) break;
        Tok* t = cur(p);
        p->i++;
        if (!strcmp(o->tok, "is") && accept_id(p, "not")) { // Python's "is not"
            Expr* e = mk(p, EX_BINARY, t);
            snprintf(e->op, sizeof e->op, "!=");
            e->a = lhs, e->b = parse_binary(p, o->prec + 1);
            lhs = e;
            continue;
        }
        Expr* e = mk(p, EX_BINARY, t);
        snprintf(e->op, sizeof e->op, "%s", o->norm);
        e->a = lhs, e->b = parse_binary(p, o->prec + 1);
        lhs = e;
    }
    return lhs;
}

static Expr* parse_expr(P* p) {
    Expr* e = parse_binary(p, 1);
    Tok* t = cur(p);
    if (is_op(t, "?") && p->c->lang != SL_PYTHON) { // c ? a : b
        p->i++;
        Expr* x = mk(p, EX_TERNARY, t);
        x->a = e, x->b = parse_expr(p);
        expect_op(p, ":");
        x->c = parse_expr(p);
        return x;
    }
    if (is_id(t, "if") && p->c->lang == SL_PYTHON) { // a if c else b
        p->i++;
        Expr* x = mk(p, EX_TERNARY, t);
        x->a = parse_binary(p, 1), x->b = e;
        if (!accept_id(p, "else")) s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected 'else' here");
        x->c = parse_expr(p);
        return x;
    }
    return e;
}

// (2026-11-24) the last type parsed was an asset reference (Level, Sound...): its class (the field takes it), else NULL
static const char* g_pt_asset;
// A type as written: Real "float", "int[]", "array<int>"; Python "float", "list[int]"; C# "float", "List<int>", "int[]"
static uint8_t parse_type(P* p) {
    Tok* t = cur(p);
    g_pt_asset = NULL;
    if (t->kind != TK_IDENT) {
        s_diag(p->c, t->line, t->col, true, "Expected a type here");
        return ST_UNKNOWN;
    }
    p->i++;
    uint8_t ty = s_type_named(t->s), arr = 0;
    bool container = !strcmp(t->s, "List") || !strcmp(t->s, "list") || !strcmp(t->s, "array") || !strcmp(t->s, "Array") ||
                     !strcmp(t->s, "TArray") || !strcmp(t->s, "IList");
    if (accept_op(p, "<") || (p->c->lang == SL_PYTHON && accept_op(p, "["))) {
        uint8_t inner = parse_type(p);
        if (!accept_op(p, ">")) accept_op(p, "]");
        if (container) ty = inner, arr = ST_ARRAY;
    }
    while (is_op(cur(p), "[") && is_op(ahead(p, 1), "]")) p->i += 2, arr = ST_ARRAY;
    if (accept_op(p, "?")) {} // (C#'s nullable)
    if (accept_op(p, "*")) ty = ST_ACTOR; // (C's pointer: an actor's handle)
    if (accept_op(p, "&") || accept_op(p, "&&")) {} // (C++'s reference: the value; a parameter by reference is read only here)
    if (ty == ST_UNKNOWN && !container && (g_pt_asset = s_asset_ref_named(t->s)) != NULL) ty = ST_STRING; // (an asset reference: its name)
    if (ty == ST_UNKNOWN && !container) ty = s_struct_type(p->c, t->s); // a structure (the script's or the project's)
    if (ty == ST_UNKNOWN && !container) {
        // a class of the project (an actor): its reference
        ty = ST_ACTOR;
    }
    return (uint8_t)(ty | arr);
}

static void add_local(P* p, const char* name, uint8_t type, int line, int col) {
    s_sym(p->c, SYM_LOCAL, name, s_ty_name(p->c, type), line, col, line, p->m ? p->m->end_line : 0);
}

// --- Structures ---
static void skip_newlines(P* p);
static void sync_line(P* p);
static void end_line(P* p);
static void sync_semi(P* p);
static bool skip_modifiers(P* p);
static bool skip_attributes(P* p);
static bool g_save_attr; // the next field is marked SaveGame (@savegame, [SaveGame]: save_progress.h)
static SStruct* new_struct(P* p, Tok* at, const char* name) {
    if (!name) return NULL;
    if (p->cls->nstructs >= S_MAX_SSTRUCTS) {
        s_diag(p->c, at->line, at->col, true, "Too many structures in one script (%d at most)", S_MAX_SSTRUCTS);
        return NULL;
    }
    SStruct* s = &p->cls->structs[p->cls->nstructs++];
    memset(s, 0, sizeof *s);
    s->name = name, s->line = at->line, s->col = at->col;
    return s;
}
static SField* struct_field(P* p, SStruct* s, Tok* at, const char* name) {
    if (!s || !name) return NULL;
    if (s->nfields >= S_MAX_SFIELDS) {
        s_diag(p->c, at->line, at->col, true, "Too many fields in one structure (%d at most)", S_MAX_SFIELDS);
        return NULL;
    }
    SField* f = &s->fields[s->nfields++];
    memset(f, 0, sizeof *f);
    f->name = name, f->line = at->line, f->col = at->col;
    return f;
}
// { [public] T a [= v], b; ... } (C#, C): the '{' taken here, the '}' too
static void brace_struct_body(P* p, SStruct* s) {
    if (!expect_op(p, "{")) return;
    while (!accept_op(p, "}") && cur(p)->kind != TK_EOF) {
        skip_attributes(p);
        skip_modifiers(p);
        Tok* t = cur(p);
        if (t->kind != TK_IDENT) {
            s_diag(p->c, t->line, t->col, true, "Expected a field here, not %s", tok_text(t));
            sync_semi(p);
            continue;
        }
        uint8_t ty = parse_type(p);
        for (;;) {
            Tok* nt = cur(p);
            const char* nm = expect_ident(p, "the field's name");
            if (is_op(cur(p), "(")) { // (a method: structures hold data only)
                s_diag(p->c, nt->line, nt->col, true, "A structure holds fields only (methods go in the class)");
                int depth = 0;
                while (cur(p)->kind != TK_EOF) {
                    if (is_op(cur(p), "{")) depth++;
                    if (is_op(cur(p), "}") && --depth <= 0) {
                        p->i++;
                        break;
                    }
                    if (is_op(cur(p), ";") && depth == 0) {
                        p->i++;
                        break;
                    }
                    p->i++;
                }
                break;
            }
            SField* f = struct_field(p, s, nt, nm);
            if (f) f->type = ty;
            if (accept_op(p, "=")) {
                Expr* v = parse_expr(p);
                if (f) f->init = v;
            }
            if (!accept_op(p, ",")) {
                if (!expect_op(p, ";")) sync_semi(p);
                break;
            }
        }
    }
}
// NEWLINE INDENT (var) a [: T] [= v] ... DEDENT (Real's struct, Python's @dataclass class)
static void indent_struct_body(P* p, SStruct* s) {
    accept_op(p, ":");
    skip_newlines(p);
    if (cur(p)->kind != TK_INDENT) {
        s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the structure's fields, indented");
        return;
    }
    p->i++;
    while (cur(p)->kind != TK_DEDENT && cur(p)->kind != TK_EOF) {
        Tok* t = cur(p);
        if (t->kind == TK_NEWLINE) {
            p->i++;
            continue;
        }
        if (accept_id(p, "pass")) continue;
        if (is_id(t, "var") || is_id(t, "let") || is_id(t, "const")) p->i++;
        Tok* nt = cur(p);
        if (nt->kind != TK_IDENT || is_id(nt, "def") || is_id(nt, "func") || is_id(nt, "event")) {
            s_diag(p->c, nt->line, nt->col, true, "A structure holds fields only ('name: type = value')");
            sync_line(p);
            continue;
        }
        const char* nm = expect_ident(p, "the field's name");
        SField* f = struct_field(p, s, nt, nm);
        uint8_t ty = ST_UNKNOWN;
        if (accept_op(p, ":")) ty = parse_type(p);
        Expr* init = accept_op(p, "=") ? parse_expr(p) : NULL;
        if (ty == ST_UNKNOWN && init) // (its type from its value: 0 int, 0.0 float, "" string, true bool)
            ty = init->k == EX_INT ? ST_INT : init->k == EX_FLOAT ? ST_FLOAT : init->k == EX_BOOL ? ST_BOOL : init->k == EX_STRING ? ST_STRING : ST_FLOAT;
        if (f) f->type = ty ? ty : ST_FLOAT, f->init = init;
        end_line(p);
    }
    if (cur(p)->kind == TK_DEDENT) p->i++;
}
// Real's "struct Name" / Python's "@dataclass" + "class Name:" at the cursor (the file's top or the class's body)
static bool indent_struct(P* p) {
    Tok* t = cur(p);
    if (is_id(t, "struct") && ahead(p, 1)->kind == TK_IDENT) {
        p->i++;
        Tok* nt = cur(p);
        SStruct* s = new_struct(p, nt, expect_ident(p, "the structure's name"));
        indent_struct_body(p, s);
        return true;
    }
    if (is_op(t, "@") && is_id(ahead(p, 1), "dataclass")) {
        sync_line(p);
        skip_newlines(p);
        if (!accept_id(p, "class")) {
            s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the dataclass's 'class' here");
            return true;
        }
        Tok* nt = cur(p);
        SStruct* s = new_struct(p, nt, expect_ident(p, "the structure's name"));
        if (accept_op(p, "(")) while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) p->i++;
        indent_struct_body(p, s);
        return true;
    }
    return false;
}
// The structures' names known before anything is read (a field typed by one declared further down)
static void prescan_structs(P* p) {
    SCtx* c = p->c;
    for (int k = 0; k + 1 < p->n; k++) {
        const char* name = NULL;
        Tok* t = &p->t[k];
        if (t->kind == TK_IDENT && !strcmp(t->s, "struct") && p->t[k + 1].kind == TK_IDENT && !is_op(&p->t[k + 2], ";") &&
            (is_op(&p->t[k + 2], "{") || p->t[k + 2].kind == TK_NEWLINE || is_op(&p->t[k + 2], ":")))
            name = p->t[k + 1].s; // struct Name { / struct Name NEWLINE
        else if (is_op(t, "@") && p->t[k + 1].kind == TK_IDENT && !strcmp(p->t[k + 1].s, "dataclass")) {
            for (int j = k + 2; j + 1 < p->n && j < k + 8; j++)
                if (p->t[j].kind == TK_IDENT && !strcmp(p->t[j].s, "class") && p->t[j + 1].kind == TK_IDENT) {
                    name = p->t[j + 1].s;
                    break;
                }
        } else if (t->kind == TK_IDENT && !strcmp(t->s, "typedef") && p->t[k + 1].kind == TK_IDENT && !strcmp(p->t[k + 1].s, "struct")) {
            int j = k + 2, depth = 0; // typedef struct [Tag] { ... } Name;
            while (j < p->n && !is_op(&p->t[j], "{")) j++;
            for (; j < p->n; j++) {
                if (is_op(&p->t[j], "{")) depth++;
                if (is_op(&p->t[j], "}") && --depth == 0) break;
            }
            if (j + 1 < p->n && p->t[j + 1].kind == TK_IDENT) name = p->t[j + 1].s;
        }
        if (!name || c->nsnames >= S_MAX_STRUCTS) continue;
        bool have = false;
        for (int i = 0; i < c->nsnames; i++) have |= !strcmp(c->snames[i], name);
        if (!have) c->snames[c->nsnames++] = name;
    }
    c->ndeclared = c->nsnames;
}

// --- Indentation languages (Real, Python) ---
static void skip_newlines(P* p) {
    while (cur(p)->kind == TK_NEWLINE) p->i++;
}
static void sync_line(P* p) { // (recovery: to the next line)
    while (cur(p)->kind != TK_NEWLINE && cur(p)->kind != TK_EOF && cur(p)->kind != TK_DEDENT) p->i++;
    if (cur(p)->kind == TK_NEWLINE) p->i++;
}
static void end_line(P* p) {
    accept_op(p, ";");
    if (cur(p)->kind == TK_NEWLINE) {
        p->i++;
        return;
    }
    if (cur(p)->kind == TK_EOF || cur(p)->kind == TK_DEDENT) return;
    s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the end of the line, not %s", tok_text(cur(p)));
    sync_line(p);
}

// switch / match: read as a chain of if / else if (the subject compared with each case's values; "default" / "_" / "else"
// the last else). Each case's condition: subject == v1 or subject == v2 ... (Python's "1 | 2" and Real's "1, 2" too)
typedef struct Case { Expr* cond; Stmt** body; int nbody; bool dflt; Tok* at; } Case;
static Expr* case_or(P* p, Tok* at, Expr* subject, Expr* pat, Expr* acc) {
    if (pat && pat->k == EX_BINARY && !strcmp(pat->op, "|") && p->c->lang == SL_PYTHON) // (alternatives)
        return case_or(p, at, subject, pat->b, case_or(p, at, subject, pat->a, acc));
    Expr* eq = mk(p, EX_BINARY, at);
    snprintf(eq->op, sizeof eq->op, "==");
    eq->a = subject, eq->b = pat;
    if (!acc) return eq;
    Expr* o = mk(p, EX_BINARY, at);
    snprintf(o->op, sizeof o->op, "or");
    o->a = acc, o->b = eq;
    return o;
}
static Stmt* case_chain(P* p, Case* cs, int n) {
    Stmt* first = NULL;
    Stmt* last = NULL; // (the chain's last if: its else gets the next one)
    Case* dflt = NULL;
    for (int i = 0; i < n; i++) {
        if (cs[i].dflt) {
            dflt = &cs[i];
            continue;
        }
        Stmt* s = mks(p, SS_IF, cs[i].at);
        s->e = cs[i].cond, s->body = cs[i].body, s->nbody = cs[i].nbody;
        if (!first) first = s;
        else if (last) {
            last->els = sa_alloc(&p->c->arena, sizeof(Stmt*));
            if (last->els) last->els[0] = s, last->nels = 1;
        }
        last = s;
    }
    if (dflt) {
        if (!first) { // (only a default: its body as a block)
            Stmt* blk = mks(p, SS_BLOCK, dflt->at);
            blk->body = dflt->body, blk->nbody = dflt->nbody;
            return blk;
        }
        last->els = dflt->body, last->nels = dflt->nbody;
    }
    return first ? first : mks(p, SS_PASS, n ? cs[0].at : cur(p));
}

static Stmt* istmt(P* p);
static Stmt** iblock(P* p, int* n) { // NEWLINE INDENT stmts DEDENT (a ':' before it, optional in Real)
    accept_op(p, ":");
    Vec v = { 0 };
    if (cur(p)->kind != TK_NEWLINE) { // a one-line body: "if x: return"
        Stmt* s = istmt(p);
        if (s) vpush(p, &v, s);
        *n = v.n;
        return (Stmt**)v.v;
    }
    skip_newlines(p);
    if (cur(p)->kind != TK_INDENT) {
        s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected an indented block here");
        *n = 0;
        return NULL;
    }
    p->i++;
    while (cur(p)->kind != TK_DEDENT && cur(p)->kind != TK_EOF) {
        if (cur(p)->kind == TK_NEWLINE) {
            p->i++;
            continue;
        }
        int before = p->i;
        Stmt* s = istmt(p);
        if (s) vpush(p, &v, s);
        if (p->i == before) p->i++; // (no progress: skip a token)
    }
    if (cur(p)->kind == TK_DEDENT) p->i++;
    *n = v.n;
    return (Stmt**)v.v;
}

static Stmt* simple_stmt(P* p, Tok* t) { // an assignment or an expression (the line's end not taken)
    if ((is_op(cur(p), "++") || is_op(cur(p), "--")) && p->c->lang != SL_PYTHON && p->c->lang != SL_REAL) { // (++i; as i++;)
        Tok* o = cur(p);
        p->i++;
        Stmt* s = mks(p, SS_ASSIGN, t);
        snprintf(s->op, sizeof s->op, "%s", is_op(o, "++") ? "+=" : "-=");
        s->target = parse_expr(p), s->e = mk(p, EX_INT, o), s->e->num = 1;
        return s;
    }
    Expr* lhs = parse_expr(p);
    Tok* o = cur(p);
    static const char* const aops[] = { "=", "+=", "-=", "*=", "/=", "%=" };
    for (size_t k = 0; k < sizeof aops / sizeof aops[0]; k++)
        if (is_op(o, aops[k])) {
            p->i++;
            Stmt* s = mks(p, SS_ASSIGN, t);
            snprintf(s->op, sizeof s->op, "%s", aops[k]);
            s->target = lhs, s->e = parse_expr(p);
            return s;
        }
    if (is_op(o, "++") || is_op(o, "--")) {
        p->i++;
        Stmt* s = mks(p, SS_ASSIGN, t);
        snprintf(s->op, sizeof s->op, "%s", is_op(o, "++") ? "+=" : "-=");
        s->target = lhs, s->e = mk(p, EX_INT, o), s->e->num = 1;
        return s;
    }
    Stmt* s = mks(p, SS_EXPR, t);
    s->e = lhs;
    return s;
}

static Stmt* istmt(P* p) {
    Tok* t = cur(p);
    if (t->kind == TK_INDENT) { // (an unexpected block: read as one)
        s_diag(p->c, t->line, t->col, true, "Unexpected indentation");
        int n;
        Stmt* b = mks(p, SS_BLOCK, t);
        b->body = iblock(p, &n), b->nbody = n;
        return b;
    }
    if (accept_id(p, "pass")) return end_line(p), mks(p, SS_PASS, t);
    if (accept_id(p, "break")) return end_line(p), mks(p, SS_BREAK, t);
    if (accept_id(p, "continue")) return end_line(p), mks(p, SS_CONTINUE, t);
    if (accept_id(p, "return")) {
        Stmt* s = mks(p, SS_RETURN, t);
        if (cur(p)->kind != TK_NEWLINE && cur(p)->kind != TK_DEDENT && cur(p)->kind != TK_EOF) s->e = parse_expr(p);
        end_line(p);
        return s;
    }
    if (accept_id(p, "if")) {
        Stmt* s = mks(p, SS_IF, t);
        s->e = parse_expr(p);
        int n;
        s->body = iblock(p, &n), s->nbody = n;
        skip_newlines(p);
        Tok* e = cur(p);
        if (is_id(e, "elif") || (is_id(e, "else") && is_id(ahead(p, 1), "if"))) {
            if (is_id(e, "else")) p->i++;
            p->t[p->i].s = "if"; // (an elif: an if in the else)
            s->els = sa_alloc(&p->c->arena, sizeof(Stmt*));
            if (s->els) s->els[0] = istmt(p), s->nels = 1;
        } else if (accept_id(p, "else")) {
            s->els = iblock(p, &n), s->nels = n;
        }
        return s;
    }
    if (accept_id(p, "while")) {
        Stmt* s = mks(p, SS_WHILE, t);
        s->e = parse_expr(p);
        int n;
        s->body = iblock(p, &n), s->nbody = n;
        return s;
    }
    if (is_id(t, "match") && cur(p)->kind == TK_IDENT && ahead(p, 1)->kind != TK_NEWLINE && !is_op(ahead(p, 1), "=") &&
        !is_op(ahead(p, 1), "(") && !is_op(ahead(p, 1), ".")) { // match x: (Python 3.10, Real); "match = ..." stays a name
        p->i++;
        Expr* subject = parse_expr(p);
        if (subject && subject->k != EX_NAME && subject->k != EX_MEMBER && subject->k != EX_INT && subject->k != EX_STRING)
            s_diag(p->c, t->line, t->col, false, "The subject is computed again for each case: read it into a variable first");
        accept_op(p, ":");
        skip_newlines(p);
        if (cur(p)->kind != TK_INDENT) {
            s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the cases, indented, under the match");
            return NULL;
        }
        p->i++;
        Vec cv = { 0 };
        Case cs[64];
        int nc = 0;
        while (cur(p)->kind != TK_DEDENT && cur(p)->kind != TK_EOF) {
            if (cur(p)->kind == TK_NEWLINE) {
                p->i++;
                continue;
            }
            Tok* ct = cur(p);
            bool dflt = false;
            Expr* cond = NULL;
            if (accept_id(p, "case")) {
                if (is_id(cur(p), "_") && (is_op(ahead(p, 1), ":") || ahead(p, 1)->kind == TK_NEWLINE)) p->i++, dflt = true;
                else
                    do cond = case_or(p, ct, subject, parse_expr(p), cond);
                    while (accept_op(p, ","));
            } else if (accept_id(p, "else") || accept_id(p, "default")) {
                dflt = true;
            } else {
                s_diag(p->c, ct->line, ct->col, true, "Expected 'case' here");
                sync_line(p);
                continue;
            }
            int n;
            Stmt** body = iblock(p, &n);
            if (nc < 64) cs[nc++] = (Case){ cond, body, n, dflt, ct };
            else s_diag(p->c, ct->line, ct->col, true, "Too many cases (64)");
        }
        (void)cv;
        if (cur(p)->kind == TK_DEDENT) p->i++;
        return case_chain(p, cs, nc);
    }
    if (accept_id(p, "for")) {
        Stmt* s = mks(p, SS_FOR_EACH, t);
        Tok* vt = cur(p);
        s->name = expect_ident(p, "the loop's variable");
        if (accept_op(p, ":")) parse_type(p);
        if (!accept_id(p, "in")) s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected 'in' here");
        if (is_id(cur(p), "range") && is_op(ahead(p, 1), "(")) { // Python's range(a[, b[, step]])
            Tok* rt = cur(p);
            p->i += 2;
            Expr* a = parse_expr(p);
            Expr* b = NULL;
            Expr* st = NULL;
            if (accept_op(p, ",")) b = parse_expr(p);
            if (accept_op(p, ",")) st = parse_expr(p);
            expect_op(p, ")");
            s->k = SS_FOR_RANGE;
            if (b) s->lo = a, s->hi = b;
            else s->lo = mk(p, EX_INT, rt), s->hi = a;
            s->step = st;
        } else {
            Expr* a = parse_expr(p);
            if (accept_op(p, "..") || (is_op(cur(p), "..=") && (s->inclusive = true, p->i++, true))) { // Real's a..b / a..=b
                s->k = SS_FOR_RANGE;
                s->lo = a, s->hi = parse_expr(p);
                if (accept_id(p, "step")) s->step = parse_expr(p);
            } else s->lo = a;
        }
        if (s->name) add_local(p, s->name, s->k == SS_FOR_RANGE ? ST_INT : ST_UNKNOWN, vt->line, vt->col);
        int n;
        s->body = iblock(p, &n), s->nbody = n;
        return s;
    }
    // Real's "var x: T = v" / "let"; Python's annotated "x: T = v"
    bool decl = (p->c->lang == SL_REAL && (is_id(t, "var") || is_id(t, "let") || is_id(t, "const"))) ||
                (p->c->lang == SL_PYTHON && t->kind == TK_IDENT && is_op(ahead(p, 1), ":") && ahead(p, 2)->kind == TK_IDENT);
    if (decl) {
        if (p->c->lang == SL_REAL) p->i++;
        Stmt* s = mks(p, SS_VAR, t);
        Tok* nt = cur(p);
        s->name = expect_ident(p, "the variable's name");
        if (accept_op(p, ":")) s->type = parse_type(p);
        if (accept_op(p, "=")) s->e = parse_expr(p);
        if (s->name) add_local(p, s->name, s->type, nt->line, nt->col);
        end_line(p);
        return s;
    }
    Stmt* s = simple_stmt(p, t);
    end_line(p);
    return s;
}

static SMethod* new_method(P* p, Tok* at, const char* name) {
    if (p->cls->nmethods >= S_MAX_METHODS) {
        s_diag(p->c, at->line, at->col, true, "Too many methods in one class (%d at most)", S_MAX_METHODS);
        return NULL;
    }
    SMethod* m = &p->cls->methods[p->cls->nmethods++];
    memset(m, 0, sizeof *m);
    m->name = name, m->line = at->line, m->col = at->col, m->ret = ST_VOID;
    return m;
}
static SField* new_field(P* p, Tok* at, const char* name) {
    for (int i = 0; i < p->cls->nfields; i++)
        if (!strcmp(p->cls->fields[i].name, name)) return &p->cls->fields[i];
    if (p->cls->nfields >= S_MAX_FIELDS) {
        s_diag(p->c, at->line, at->col, true, "Too many fields in one class (%d at most)", S_MAX_FIELDS);
        return NULL;
    }
    SField* f = &p->cls->fields[p->cls->nfields++];
    memset(f, 0, sizeof *f);
    f->name = name, f->line = at->line, f->col = at->col;
    return f;
}

// The method's last line (its locals' scope): the block's end, found by indentation before parsing it
static int block_end_line(P* p) {
    int depth = 0;
    for (int k = p->i; k < p->n; k++) {
        if (p->t[k].kind == TK_INDENT) depth++;
        else if (p->t[k].kind == TK_DEDENT && --depth <= 0) return p->t[k].line;
        else if (p->t[k].kind == TK_OP && p->t[k].op[0] == '{') depth++;
        else if (p->t[k].kind == TK_OP && p->t[k].op[0] == '}' && --depth <= 0) return p->t[k].line;
    }
    return p->t[p->n - 1].line;
}

static void iparams(P* p, SMethod* m) {
    expect_op(p, "(");
    while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) {
        Tok* nt = cur(p);
        const char* nm = expect_ident(p, "a parameter's name");
        if (!nm) break;
        uint8_t ty = ST_UNKNOWN;
        if (accept_op(p, ":")) ty = parse_type(p);
        if (accept_op(p, "=")) parse_expr(p); // (a default: the engine's call fills it)
        bool self = p->c->lang == SL_PYTHON && m->nparams == 0 && !strcmp(nm, "self") && !m->nparams;
        if (!self) {
            if (m->nparams < S_MAX_PARAMS) m->params[m->nparams++] = (SParam){ nm, ty, nt->line, nt->col };
            else s_diag(p->c, nt->line, nt->col, true, "Too many parameters (%d at most)", S_MAX_PARAMS);
            s_sym(p->c, SYM_PARAM, nm, s_ty_name(p->c, ty), nt->line, nt->col, m->line, m->end_line);
        }
        if (!accept_op(p, ",") && !is_op(cur(p), ")")) {
            expect_op(p, ")");
            break;
        }
    }
    if (accept_op(p, "->")) m->ret = parse_type(p);
}

static void sym_method(P* p, SMethod* m, bool event) {
    char sig[32];
    snprintf(sig, sizeof sig, "%s (%d)", s_ty_name(p->c, m->ret), m->nparams);
    s_sym(p->c, event ? SYM_EVENT : SYM_METHOD, m->name, sig, m->line, m->col, 0, 0);
}

// Python's __init__: "self.x = v" lines are the class's fields (the rest: a warning)
static void python_init(P* p, SMethod* m) {
    for (int i = 0; i < m->nbody; i++) {
        Stmt* s = m->body[i];
        if (s->k == SS_ASSIGN && !strcmp(s->op, "=") && s->target && s->target->k == EX_MEMBER && s->target->a && s->target->a->k == EX_SELF) {
            SField* f = new_field(p, &(Tok){ .line = s->line, .col = s->col }, s->target->s);
            if (f) f->init = s->e;
            continue;
        }
        if (s->k == SS_VAR && s->name) { // (an annotated self field "self.x: int = 0" reads as a local here)
            SField* f = new_field(p, &(Tok){ .line = s->line, .col = s->col }, s->name);
            if (f) f->init = s->e, f->type = s->type;
            continue;
        }
        if (s->k == SS_PASS || (s->k == SS_EXPR && s->e && s->e->k == EX_CALL && s->e->s && !strcmp(s->e->s, "__init__"))) continue;
        s_diag(p->c, s->line, s->col, false, "Only 'self.x = value' runs in __init__ (use begin_play for the rest)");
    }
    p->cls->nmethods--; // (__init__ itself: no method)
}

static void parse_indent_class(P* p) {
    SClass* cls = p->cls;
    skip_newlines(p);
    // Python's imports
    while (is_id(cur(p), "import") || is_id(cur(p), "from")) sync_line(p), skip_newlines(p);
    while (indent_struct(p)) skip_newlines(p); // (the structures before the class)
    bool has_class = is_id(cur(p), "class");
    if (has_class) {
        Tok* ct = cur(p);
        p->i++;
        cls->name = expect_ident(p, "the class's name");
        cls->line = ct->line;
        if (accept_op(p, "(")) { // Python: class Door(Actor)
            if (cur(p)->kind == TK_IDENT) cls->parent = p->t[p->i++].s;
            while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) p->i++;
        } else if (accept_id(p, "extends") || accept_op(p, ":") || accept_op(p, "<")) {
            if (cur(p)->kind == TK_IDENT) cls->parent = p->t[p->i++].s;
        }
        accept_op(p, ":");
        skip_newlines(p);
        if (cur(p)->kind != TK_INDENT) {
            s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the class's indented members here");
            return;
        }
        p->i++;
    }
    bool editable = false;
    while (cur(p)->kind != TK_EOF && !(has_class && cur(p)->kind == TK_DEDENT)) {
        Tok* t = cur(p);
        if (t->kind == TK_NEWLINE || t->kind == TK_DEDENT) {
            p->i++;
            continue;
        }
        if (indent_struct(p)) continue; // (a structure in the class's body: the script's, like one outside)
        if (is_op(t, "@")) { // @editable (Real), decorators (Python: ignored)
            p->i++;
            Tok* d = cur(p);
            if (d->kind == TK_IDENT && (!strcmp(d->s, "editable") || !strcmp(d->s, "export") || !strcmp(d->s, "property"))) editable = true;
            if (d->kind == TK_IDENT && (!_stricmp(d->s, "savegame") || !_stricmp(d->s, "save_game") || !_stricmp(d->s, "save"))) g_save_attr = true;
            sync_line(p);
            continue;
        }
        if (is_id(t, "event") || is_id(t, "func") || is_id(t, "fn") || is_id(t, "def") || is_id(t, "function")) {
            bool ev = is_id(t, "event");
            p->i++;
            Tok* nt = cur(p);
            const char* nm = expect_ident(p, "the method's name");
            if (!nm) {
                sync_line(p);
                continue;
            }
            SMethod* m = new_method(p, nt, nm);
            if (!m) {
                sync_line(p);
                continue;
            }
            m->end_line = block_end_line(p);
            p->m = m;
            iparams(p, m);
            int n;
            m->body = iblock(p, &n), m->nbody = n;
            p->m = NULL;
            if (p->c->lang == SL_PYTHON && !strcmp(nm, "__init__")) python_init(p, m);
            else sym_method(p, m, ev);
            continue;
        }
        bool var = p->c->lang == SL_REAL ? (is_id(t, "var") || is_id(t, "let") || is_id(t, "const"))
                                         : (t->kind == TK_IDENT && (is_op(ahead(p, 1), ":") || is_op(ahead(p, 1), "=")));
        if (var) {
            if (p->c->lang == SL_REAL) p->i++;
            Tok* nt = cur(p);
            const char* nm = expect_ident(p, "the field's name");
            SField* f = nm ? new_field(p, nt, nm) : NULL;
            uint8_t ty = ST_UNKNOWN;
            if (accept_op(p, ":")) ty = parse_type(p);
            Expr* init = accept_op(p, "=") ? parse_expr(p) : NULL;
            if (f) f->type = ty, f->init = init, f->editable = editable, f->save_game = g_save_attr, f->asset_ref = ty == ST_STRING ? g_pt_asset : NULL;
            editable = false, g_save_attr = false;
            end_line(p);
            continue;
        }
        if (!has_class && (is_id(t, "class"))) {
            s_diag(p->c, t->line, t->col, true, "One class per script");
            sync_line(p);
            continue;
        }
        s_diag(p->c, t->line, t->col, true, "Expected a field ('%s') or a method ('%s') here, not %s", p->c->lang == SL_REAL ? "var" : "name = value",
               p->c->lang == SL_REAL ? "func / event" : "def", tok_text(t));
        sync_line(p);
    }
}

// --- Brace languages (C#, C) ---
static void sync_semi(P* p) { // (recovery: past the next ';' or '}' at this depth)
    int depth = 0;
    while (cur(p)->kind != TK_EOF) {
        if (is_op(cur(p), "{")) depth++;
        if (is_op(cur(p), "}")) {
            if (!depth) return;
            depth--;
        }
        if (is_op(cur(p), ";") && !depth) {
            p->i++;
            return;
        }
        p->i++;
    }
}

static bool skip_modifiers(P* p) { // public, private, static, readonly, const, override, virtual, unsafe...
    static const char* const mods[] = { "public", "private", "protected", "internal", "static", "readonly", "const", "override",
                                        "virtual", "sealed", "abstract", "partial", "unsafe", "extern", "inline", "volatile",
                                        "async", "new", "constexpr", "mutable", "explicit", "friend", "final" };
    bool any = false;
    for (;;) {
        bool hit = false;
        for (size_t k = 0; k < sizeof mods / sizeof mods[0]; k++)
            if (is_id(cur(p), mods[k])) p->i++, hit = any = true;
        if (!hit) return any;
    }
}

// [SerializeField] / [Editable] / [UPROPERTY(...)]: true when it marks the field editable ([SaveGame]: g_save_attr)
static bool skip_attributes(P* p) {
    bool ed = false;
    while (is_op(cur(p), "[") && p->c->lang == SL_CSHARP) {
        int depth = 0;
        do {
            if (is_op(cur(p), "[")) depth++;
            if (is_op(cur(p), "]")) depth--;
            if (cur(p)->kind == TK_IDENT && (!strcmp(cur(p)->s, "SerializeField") || !strcmp(cur(p)->s, "Editable") || !strcmp(cur(p)->s, "EditAnywhere")))
                ed = true;
            if (cur(p)->kind == TK_IDENT && !strcmp(cur(p)->s, "SaveGame")) g_save_attr = true;
            p->i++;
        } while (depth > 0 && cur(p)->kind != TK_EOF);
    }
    return ed;
}

// IDENT ('<' ... '>')? ('[' ']')* IDENT: a declaration starts here (a type then a name)
static bool looks_like_decl(P* p) {
    int k = p->i;
    if (p->t[k].kind != TK_IDENT) return false;
    if (!strcmp(p->t[k].s, "var")) return p->t[k + 1].kind == TK_IDENT;
    k++;
    if (is_op(&p->t[k], "<")) {
        int depth = 0;
        while (k < p->n) {
            if (is_op(&p->t[k], "<")) depth++;
            if (is_op(&p->t[k], ">")) {
                if (--depth == 0) {
                    k++;
                    break;
                }
            }
            if (is_op(&p->t[k], ";") || is_op(&p->t[k], "(")) return false;
            k++;
        }
    }
    while (is_op(&p->t[k], "[") && is_op(&p->t[k + 1], "]")) k += 2;
    if (is_op(&p->t[k], "*")) k++;
    return p->t[k].kind == TK_IDENT;
}

static Stmt* bstmt(P* p);
// A continue of this loop's own (not inside a nested loop): do ... while can't have one yet
static bool loose_continue(Stmt** b, int n) {
    for (int i = 0; i < n; i++) {
        const Stmt* s = b[i];
        if (!s) continue;
        if (s->k == SS_CONTINUE) return true;
        if ((s->k == SS_IF || s->k == SS_BLOCK) && (loose_continue(s->body, s->nbody) || loose_continue(s->els, s->nels))) return true;
    }
    return false;
}

static Stmt** bblock(P* p, int* n) { // '{' stmts '}' or one statement
    Vec v = { 0 };
    if (!accept_op(p, "{")) {
        Stmt* s = bstmt(p);
        if (s) vpush(p, &v, s);
        *n = v.n;
        return (Stmt**)v.v;
    }
    while (!accept_op(p, "}") && cur(p)->kind != TK_EOF) {
        int before = p->i;
        Stmt* s = bstmt(p);
        if (s) vpush(p, &v, s);
        if (p->i == before) p->i++;
    }
    *n = v.n;
    return (Stmt**)v.v;
}

static Stmt* bdecl(P* p, Tok* t) { // T x = v; / var x = v;  (the ';' not taken)
    uint8_t ty = ST_UNKNOWN;
    if (!accept_id(p, "var")) ty = parse_type(p);
    Stmt* s = mks(p, SS_VAR, t);
    Tok* nt = cur(p);
    s->name = expect_ident(p, "the variable's name");
    if (is_op(cur(p), "[") && ty != ST_UNKNOWN && !(ty & ST_ARRAY)) { // C's int xs[] / int xs[3]: an array (its size: the items')
        p->i++;
        if (!is_op(cur(p), "]")) parse_expr(p);
        expect_op(p, "]");
        ty |= ST_ARRAY;
    }
    s->type = ty;
    if (accept_op(p, "=")) s->e = parse_expr(p);
    if (s->name) add_local(p, s->name, ty, nt->line, nt->col);
    return s;
}

static Stmt* bstmt(P* p) {
    Tok* t = cur(p);
    if (is_op(t, "{")) {
        Stmt* b = mks(p, SS_BLOCK, t);
        int n;
        b->body = bblock(p, &n), b->nbody = n;
        return b;
    }
    if (accept_op(p, ";")) return mks(p, SS_PASS, t);
    if (accept_id(p, "break")) return expect_op(p, ";"), mks(p, SS_BREAK, t);
    if (accept_id(p, "continue")) return expect_op(p, ";"), mks(p, SS_CONTINUE, t);
    if (accept_id(p, "return")) {
        Stmt* s = mks(p, SS_RETURN, t);
        if (!is_op(cur(p), ";")) s->e = parse_expr(p);
        expect_op(p, ";");
        return s;
    }
    if (accept_id(p, "if")) {
        Stmt* s = mks(p, SS_IF, t);
        expect_op(p, "(");
        s->e = parse_expr(p);
        expect_op(p, ")");
        int n;
        s->body = bblock(p, &n), s->nbody = n;
        if (accept_id(p, "else")) s->els = bblock(p, &n), s->nels = n;
        return s;
    }
    if (accept_id(p, "while")) {
        Stmt* s = mks(p, SS_WHILE, t);
        expect_op(p, "(");
        s->e = parse_expr(p);
        expect_op(p, ")");
        int n;
        s->body = bblock(p, &n), s->nbody = n;
        return s;
    }
    if (accept_id(p, "foreach")) { // C#: foreach (var x in arr)
        Stmt* s = mks(p, SS_FOR_EACH, t);
        expect_op(p, "(");
        if (!accept_id(p, "var")) parse_type(p);
        Tok* nt = cur(p);
        s->name = expect_ident(p, "the loop's variable");
        if (!accept_id(p, "in")) s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected 'in' here");
        s->lo = parse_expr(p);
        expect_op(p, ")");
        if (s->name) add_local(p, s->name, ST_UNKNOWN, nt->line, nt->col);
        int n;
        s->body = bblock(p, &n), s->nbody = n;
        return s;
    }
    if (accept_id(p, "for")) { // for (init; cond; post): a range when it reads as one
        if (p->c->lang == SL_CPP && is_op(cur(p), "(")) { // (C++) for (auto x : xs) / for (const T& x : xs): a for each
            int k = p->i + 1, depth = 0;
            bool range = false;
            for (; k < p->n && !is_op(&p->t[k], ";"); k++) {
                if (is_op(&p->t[k], "(") || is_op(&p->t[k], "<")) depth++;
                if (is_op(&p->t[k], ")") || is_op(&p->t[k], ">")) {
                    if (!depth) break;
                    depth--;
                }
                if (!depth && is_op(&p->t[k], ":")) {
                    range = true;
                    break;
                }
            }
            if (range) {
                Stmt* s = mks(p, SS_FOR_EACH, t);
                expect_op(p, "(");
                skip_modifiers(p);
                if (!accept_id(p, "var")) parse_type(p);
                accept_op(p, "&");
                Tok* nt = cur(p);
                s->name = expect_ident(p, "the loop's variable");
                expect_op(p, ":");
                s->lo = parse_expr(p);
                expect_op(p, ")");
                if (s->name) add_local(p, s->name, ST_UNKNOWN, nt->line, nt->col);
                int n;
                s->body = bblock(p, &n), s->nbody = n;
                return s;
            }
        }
        Stmt* s = mks(p, SS_FOR_C, t);
        expect_op(p, "(");
        Tok* it = cur(p);
        if (!is_op(cur(p), ";")) s->init = looks_like_decl(p) ? bdecl(p, it) : simple_stmt(p, it);
        expect_op(p, ";");
        if (!is_op(cur(p), ";")) s->e = parse_expr(p);
        expect_op(p, ";");
        Tok* pt = cur(p);
        if (!is_op(cur(p), ")")) s->post = simple_stmt(p, pt);
        expect_op(p, ")");
        int n;
        s->body = bblock(p, &n), s->nbody = n;
        // for (int i = a; i < b; i++) / i <= b / i += k: a range loop
        const char* v = s->init ? (s->init->k == SS_VAR ? s->init->name : s->init->target && s->init->target->k == EX_NAME ? s->init->target->s : NULL) : NULL;
        Expr* c = s->e;
        if (v && c && c->k == EX_BINARY && (!strcmp(c->op, "<") || !strcmp(c->op, "<=")) && c->a->k == EX_NAME && !strcmp(c->a->s, v) &&
            s->post && s->post->k == SS_ASSIGN && !strcmp(s->post->op, "+=") && s->post->target->k == EX_NAME && !strcmp(s->post->target->s, v)) {
            s->k = SS_FOR_RANGE, s->name = v, s->lo = s->init->e, s->hi = c->b, s->inclusive = !strcmp(c->op, "<=");
            s->step = s->post->e && !(s->post->e->k == EX_INT && s->post->e->num == 1) ? s->post->e : NULL;
        }
        return s;
    }
    if (accept_id(p, "do")) { // (Q24) do { body } while (cond);  ->  while (true) { body; if (!(cond)) break; }
        Stmt* s = mks(p, SS_WHILE, t);
        s->e = mk(p, EX_BOOL, t), s->e->num = 1;
        int n;
        Stmt** body = bblock(p, &n);
        Tok* wt = cur(p);
        if (!accept_id(p, "while")) {
            s_diag(p->c, wt->line, wt->col, true, "Expected 'while (...);' after the do block");
            sync_semi(p);
            return NULL;
        }
        expect_op(p, "(");
        Expr* cond = parse_expr(p);
        expect_op(p, ")");
        expect_op(p, ";");
        if (loose_continue(body, n)) // (it would skip the condition: the lowering puts it at the body's end)
            s_diag(p->c, t->line, t->col, true, "continue inside do ... while isn't supported: use an if around the rest");
        Stmt* chk = mks(p, SS_IF, wt);
        Expr* nc = mk(p, EX_UNARY, wt);
        snprintf(nc->op, sizeof nc->op, "not");
        nc->a = cond, chk->e = nc;
        chk->body = sa_alloc(&p->c->arena, sizeof(Stmt*));
        chk->body[0] = mks(p, SS_BREAK, wt), chk->nbody = 1;
        s->body = sa_alloc(&p->c->arena, (size_t)(n + 1) * sizeof(Stmt*));
        for (int i = 0; i < n; i++) s->body[i] = body[i];
        s->body[n] = chk, s->nbody = n + 1;
        return s;
    }
    if (is_id(t, "switch")) {
        p->i++;
        expect_op(p, "(");
        Expr* subject = parse_expr(p);
        expect_op(p, ")");
        if (subject && subject->k != EX_NAME && subject->k != EX_MEMBER)
            s_diag(p->c, t->line, t->col, false, "The subject is computed again for each case: read it into a variable first");
        if (!expect_op(p, "{")) return NULL;
        Case cs[64];
        int nc = 0;
        Expr* pending = NULL; // (case a: case b: one body)
        bool pending_dflt = false;
        Tok* pending_at = t;
        while (!is_op(cur(p), "}") && cur(p)->kind != TK_EOF) {
            Tok* ct = cur(p);
            if (accept_id(p, "case")) {
                pending = case_or(p, ct, subject, parse_expr(p), pending);
                expect_op(p, ":");
                pending_at = pending_at == t ? ct : pending_at;
                continue;
            }
            if (accept_id(p, "default")) {
                expect_op(p, ":");
                pending_dflt = true;
                pending_at = pending_at == t ? ct : pending_at;
                continue;
            }
            if (!pending && !pending_dflt) {
                s_diag(p->c, ct->line, ct->col, true, "Expected 'case' or 'default' here");
                sync_semi(p);
                continue;
            }
            // the body: statements up to the next case / default / '}'; its last break dropped (the chain ends there)
            Vec bv = { 0 };
            bool ended = false;
            while (!is_op(cur(p), "}") && !is_id(cur(p), "case") && !is_id(cur(p), "default") && cur(p)->kind != TK_EOF) {
                int before = p->i;
                Stmt* s = bstmt(p);
                if (s && s->k == SS_BREAK) ended = true;
                else if (s) vpush(p, &bv, s), ended = s->k == SS_RETURN || s->k == SS_CONTINUE;
                if (p->i == before) p->i++;
            }
            if (!ended && !is_op(cur(p), "}"))
                s_diag(p->c, ct->line, ct->col, false, "This case falls through: only its own lines run (end it with break)");
            if (pending_dflt && pending) pending_dflt = false, s_diag(p->c, ct->line, ct->col, false, "default with other cases: read as those cases");
            if (nc < 64) cs[nc++] = (Case){ pending, (Stmt**)bv.v, bv.n, pending_dflt, pending_at };
            pending = NULL, pending_dflt = false, pending_at = t;
        }
        expect_op(p, "}");
        return case_chain(p, cs, nc);
    }
    if (looks_like_decl(p) && !is_id(t, "return")) {
        Stmt* s = bdecl(p, t);
        expect_op(p, ";");
        return s;
    }
    Stmt* s = simple_stmt(p, t);
    if (!expect_op(p, ";")) sync_semi(p);
    return s;
}

static void parse_brace_members(P* p, bool c_file) {
    for (;;) {
        Tok* t = cur(p);
        if (t->kind == TK_EOF || (!c_file && is_op(t, "}"))) return;
        if (is_id(t, "__parent__")) { // "// parent: Character"
            p->i++;
            if (cur(p)->kind == TK_IDENT) p->cls->parent = p->t[p->i++].s;
            continue;
        }
        if (is_id(t, "struct") && ahead(p, 1)->kind == TK_IDENT && is_op(ahead(p, 2), "{")) { // struct Name { ... } [;]
            p->i++;
            Tok* nt = cur(p);
            SStruct* s = new_struct(p, nt, expect_ident(p, "the structure's name"));
            brace_struct_body(p, s);
            accept_op(p, ";");
            continue;
        }
        if (is_id(t, "typedef") && is_id(ahead(p, 1), "struct")) { // typedef struct [Tag] { ... } Name;
            p->i += 2;
            if (cur(p)->kind == TK_IDENT) p->i++;
            SStruct tmp;
            memset(&tmp, 0, sizeof tmp);
            Tok* at = cur(p);
            SStruct* s = p->cls->nstructs < S_MAX_SSTRUCTS ? &p->cls->structs[p->cls->nstructs] : &tmp;
            memset(s, 0, sizeof *s);
            brace_struct_body(p, s);
            Tok* nt = cur(p);
            const char* nm = expect_ident(p, "the structure's name");
            if (nm && s != &tmp) s->name = nm, s->line = nt->line, s->col = nt->col, p->cls->nstructs++;
            else if (s == &tmp) s_diag(p->c, at->line, at->col, true, "Too many structures in one script (%d at most)", S_MAX_SSTRUCTS);
            if (!expect_op(p, ";")) sync_semi(p);
            continue;
        }
        if (is_id(t, "using") || is_id(t, "typedef") || is_id(t, "enum")) {
            if (!is_id(t, "using")) s_diag(p->c, t->line, t->col, false, "%s is skipped (not supported in scripts yet)", t->s);
            sync_semi(p);
            continue;
        }
        if (p->c->lang == SL_CPP && (is_id(t, "public") || is_id(t, "private") || is_id(t, "protected")) && is_op(ahead(p, 1), ":")) {
            p->i += 2; // (C++'s access labels: every member is the class's)
            continue;
        }
        g_save_attr = false; // ([SaveGame] on this declaration only)
        bool ed = skip_attributes(p);
        skip_modifiers(p);
        t = cur(p);
        if (t->kind != TK_IDENT) {
            s_diag(p->c, t->line, t->col, true, "Expected a field or a method here, not %s", tok_text(t));
            p->i++;
            continue;
        }
        // a constructor: Door() { } -> its body runs at BeginPlay's start (a warning)
        bool ctor = !c_file && p->cls->name && !strcmp(t->s, p->cls->name) && is_op(ahead(p, 1), "(");
        uint8_t ty = ST_VOID;
        if (!ctor) {
            if (is_id(t, "void")) p->i++;
            else ty = parse_type(p);
        }
        Tok* nt = cur(p);
        const char* nm = ctor ? (p->i++, "__ctor__") : expect_ident(p, "a name");
        if (!nm) {
            sync_semi(p);
            continue;
        }
        if (is_op(cur(p), "(")) { // a method
            SMethod* m = new_method(p, nt, nm);
            if (!m) {
                sync_semi(p);
                continue;
            }
            m->ret = ty;
            m->end_line = block_end_line(p);
            p->m = m;
            expect_op(p, "(");
            while (!accept_op(p, ")") && cur(p)->kind != TK_EOF) {
                if (is_id(cur(p), "void") && is_op(ahead(p, 1), ")")) {
                    p->i++;
                    continue;
                }
                skip_modifiers(p);
                Tok* pt0 = cur(p);
                uint8_t pty = parse_type(p);
                Tok* pn = cur(p);
                const char* pnm = expect_ident(p, "the parameter's name");
                if (accept_op(p, "=")) parse_expr(p);
                if (pnm && m->nparams < S_MAX_PARAMS) {
                    m->params[m->nparams++] = (SParam){ pnm, pty, pn->line, pn->col };
                    s_sym(p->c, SYM_PARAM, pnm, s_ty_name(p->c, pty), pn->line, pn->col, m->line, m->end_line);
                }
                (void)pt0;
                if (!accept_op(p, ",") && !is_op(cur(p), ")")) {
                    expect_op(p, ")");
                    break;
                }
            }
            while (is_id(cur(p), "const") || is_id(cur(p), "override") || is_id(cur(p), "final") || is_id(cur(p), "noexcept")) p->i++; // (C++)
            if (is_op(cur(p), "=") && p->c->lang == SL_CPP) { // (= default; = 0;: nothing to run)
                sync_semi(p);
                p->cls->nmethods--;
                p->m = NULL;
                continue;
            }
            if (accept_op(p, ";")) { // (a prototype: C)
                p->cls->nmethods--;
                p->m = NULL;
                continue;
            }
            int n;
            if (!is_op(cur(p), "{")) s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected the method's body '{' here");
            m->body = bblock(p, &n), m->nbody = n;
            p->m = NULL;
            if (ctor) s_diag(p->c, nt->line, nt->col, false, "The constructor runs at BeginPlay's start");
            sym_method(p, m, false);
            continue;
        }
        // field(s): T a = 1, b;
        for (;;) {
            SField* f = new_field(p, nt, nm);
            if (f) f->type = ty, f->editable = ed || c_file || p->c->lang == SL_CPP, f->save_game = g_save_attr, f->asset_ref = (ty & 0x7F) == ST_STRING ? g_pt_asset : NULL;
            if (accept_op(p, "=")) {
                Expr* v = parse_expr(p);
                if (f) f->init = v;
            }
            if (!accept_op(p, ",")) break;
            nt = cur(p);
            nm = expect_ident(p, "a name");
            if (!nm) break;
        }
        if (!expect_op(p, ";")) sync_semi(p);
    }
}

static void parse_brace_file(P* p) {
    if (p->c->lang == SL_C) {
        parse_brace_members(p, true);
        return;
    }
    // C#: usings, a namespace (file-scoped or a block), one class
    while (cur(p)->kind != TK_EOF) {
        if (is_id(cur(p), "using")) {
            sync_semi(p);
            continue;
        }
        if (accept_id(p, "namespace")) {
            while (cur(p)->kind == TK_IDENT || is_op(cur(p), ".")) p->i++;
            if (!accept_op(p, ";")) accept_op(p, "{");
            continue;
        }
        skip_attributes(p);
        skip_modifiers(p);
        if (is_id(cur(p), "struct") && ahead(p, 1)->kind == TK_IDENT) { // (a structure beside the class)
            p->i++;
            Tok* nt = cur(p);
            SStruct* s = new_struct(p, nt, expect_ident(p, "the structure's name"));
            brace_struct_body(p, s);
            accept_op(p, ";");
            continue;
        }
        if (accept_id(p, "class")) {
            Tok* ct = cur(p);
            p->cls->name = expect_ident(p, "the class's name");
            p->cls->line = ct->line;
            if (accept_op(p, ":") && cur(p)->kind == TK_IDENT) {
                if (is_id(cur(p), "public") || is_id(cur(p), "protected") || is_id(cur(p), "private") || is_id(cur(p), "virtual")) p->i++; // (C++)
                p->cls->parent = p->t[p->i++].s;
                while (accept_op(p, ",")) p->i++; // (interfaces)
            }
            if (expect_op(p, "{")) {
                parse_brace_members(p, false);
                expect_op(p, "}");
                accept_op(p, ";"); // (C++'s class ends with "};")
            }
            accept_op(p, "}"); // (the namespace's)
            while (is_id(cur(p), "public") || is_id(cur(p), "internal") || is_id(cur(p), "struct")) { // (structures after the class)
                skip_modifiers(p);
                if (!is_id(cur(p), "struct") || ahead(p, 1)->kind != TK_IDENT) break;
                p->i++;
                Tok* nt = cur(p);
                SStruct* s = new_struct(p, nt, expect_ident(p, "the structure's name"));
                brace_struct_body(p, s);
                accept_op(p, ";");
            }
            accept_op(p, "}"); // (a namespace's after them)
            if (cur(p)->kind != TK_EOF) s_diag(p->c, cur(p)->line, cur(p)->col, false, "One class per script: what follows is skipped");
            return;
        }
        if (cur(p)->kind == TK_EOF) break;
        s_diag(p->c, cur(p)->line, cur(p)->col, true, "Expected 'class' here, not %s", tok_text(cur(p)));
        sync_semi(p);
    }
}

SClass* s_parse(SCtx* c, Tok* t, int n) {
    SClass* cls = sa_alloc(&c->arena, sizeof *cls);
    if (!cls) return NULL;
    P p = { c, t, n, 0, cls, NULL };
    prescan_structs(&p);
    if (c->lang == SL_REAL || c->lang == SL_PYTHON) {
        parse_indent_class(&p);
        for (;;) { // (structures after the class)
            while (cur(&p)->kind == TK_NEWLINE || cur(&p)->kind == TK_DEDENT) p.i++;
            if (!indent_struct(&p)) break;
        }
    } else
        parse_brace_file(&p);
    if (!cls->name || !cls->name[0]) cls->name = c->file_name;
    if (!cls->parent) cls->parent = "Actor";
    if (!strcmp(cls->parent, "MonoBehaviour") || !strcmp(cls->parent, "object") || !strcmp(cls->parent, "AActor")) cls->parent = "Actor";
    if (!strcmp(cls->parent, "GameplayAbility") || !strcmp(cls->parent, "UGameplayAbility")) cls->parent = "Gameplay Ability"; // (ability.h)
    s_sym(c, SYM_CLASS, cls->name, cls->parent, cls->line, 1, 0, 0);
    for (int i = 0; i < cls->nfields; i++)
        s_sym(c, SYM_FIELD, cls->fields[i].name, s_ty_name(c, cls->fields[i].type), cls->fields[i].line, cls->fields[i].col, 0, 0);
    return cls;
}
