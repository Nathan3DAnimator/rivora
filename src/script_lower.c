// A script's class lowered into a Blueprint graph (script_internal.h): every statement a chain of exec nodes, every
// expression pure nodes wired into them (constants folded on the way), the engine's functions their nodes. The
// Blueprints' compiler then makes the bytecode; its errors come back to the script's lines through the nodes' comments.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "script_internal.h"

typedef struct Val {
    uint8_t ty;              // STy (| ST_ARRAY)
    bool lit, none;          // a literal (no pin): num / str / vec; none: null
    double num;
    const char* str;
    float vec[4];
    uint32_t node;
    uint8_t pin;
    bool ok;                 // false: an error already said
} Val;
typedef struct Pend { uint32_t node; uint8_t pin; } Pend;
typedef struct Local {
    const char* name;
    uint8_t ty;
    int var;                 // a class variable (-1: a pin)
    uint32_t node;
    uint8_t pin;
    int depth;
    bool readonly;           // a loop's index / element
} Local;
typedef struct Loop {
    int kind;                // 0 while (a flag), 1 a For / For Each with Break
    uint32_t node;           // the For's (its Break input)
    uint8_t brk_pin;
    int flag;                // the while's break flag (a variable)
} Loop;

typedef struct Low {
    SCtx* c;
    SClass* cls;
    BpAsset* a;
    BpGraph* g;
    Pend pend[64];
    int npend;
    float x, y;
    SMethod* m;
    Local loc[96];
    int nloc, depth;
    Loop loops[16];
    int nloops;
    uint32_t entry;          // a function's entry node (its parameters' pins); an event's node
    uint8_t param_pin[S_MAX_PARAMS];
    int ret_graph;           // the method's function graph (-1: an event)
    int impure;              // exec nodes made (an expression with side effects)
    uint32_t nodes;
    int nflags;
} Low;

// --- Nodes and pins ---
static BpNode* node_of(Low* L, uint32_t id) { return bp_node(L->g, id); }

static BpNode* add(Low* L, uint16_t kind, int line, int col, const char* text, const char* text2) {
    BpNode* n = bp_add_node(L->a, L->g, kind, L->x, L->y, text, text2);
    if (!n) {
        s_diag(L->c, line, col, true, "The graph is full (too much code in one method)");
        return NULL;
    }
    snprintf(n->comment, sizeof n->comment, "L%d:C%d", line, col); // (the compiler's errors back to the line)
    L->x += 240.0f, L->nodes++;
    return n;
}

static int pin_find(const BpNode* n, bool out, bool exec, int k) { // the k-th (exec or data) input / output
    for (int i = 0; i < n->npins; i++) {
        bool is_exec = n->pins[i].type == BPT_EXEC;
        if ((n->pins[i].out != 0) == out && is_exec == exec && k-- == 0) return i;
    }
    return -1;
}
static int pin_named(const BpNode* n, bool out, const char* name) {
    for (int i = 0; i < n->npins; i++)
        if ((n->pins[i].out != 0) == out && !_stricmp(n->pins[i].name, name)) return i;
    return -1;
}

static SCtx* g_sc; // (the compile being lowered: its structures' table)
static uint8_t sty_of_pin(const BpPin* p) {
    uint8_t t;
    switch (p->type) {
    case BPT_STRUCT: t = s_struct_type(g_sc, p->ref); break;
    case BPT_BOOL: t = ST_BOOL; break;
    case BPT_INT: case BPT_BYTE: case BPT_ENUM: t = ST_INT; break;
    case BPT_FLOAT: t = ST_FLOAT; break;
    case BPT_VECTOR: t = ST_VECTOR; break;
    case BPT_ROTATOR: t = ST_ROTATOR; break;
    case BPT_STRING: case BPT_NAME: case BPT_TEXT: t = ST_STRING; break;
    case BPT_ACTOR: t = ST_ACTOR; break;
    case BPT_COLOR: t = ST_COLOR; break;
    case BPT_TRANSFORM: t = ST_TRANSFORM; break;
    case BPT_INT64: t = ST_INT64; break;
    case BPT_OBJECT: t = ST_OBJECT; break;
    case BPT_CLASS: t = ST_CLASS; break;
    default: t = ST_UNKNOWN; break;
    }
    return (uint8_t)(t | (p->cont == BPC_ARRAY ? ST_ARRAY : 0));
}
static uint8_t bpt_of(uint8_t st) {
    if (ST_IS_STRUCT(st)) return BPT_STRUCT;
    switch (st & 0x7F) {
    case ST_BOOL: return BPT_BOOL;
    case ST_INT: return BPT_INT;
    case ST_FLOAT: return BPT_FLOAT;
    case ST_STRING: return BPT_STRING;
    case ST_VECTOR: return BPT_VECTOR;
    case ST_ROTATOR: return BPT_ROTATOR;
    case ST_ACTOR: return BPT_ACTOR;
    case ST_COLOR: return BPT_COLOR;
    case ST_TRANSFORM: return BPT_TRANSFORM;
    case ST_INT64: return BPT_INT64;
    case ST_OBJECT: return BPT_OBJECT;
    case ST_CLASS: return BPT_CLASS;
    default: return BPT_FLOAT;
    }
}
static const char* tyname(uint8_t t) {
    static char buf[4][24];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "%s%s", s_ty_name(g_sc, t), t & ST_ARRAY ? "[]" : "");
    return buf[k];
}
// a variable (or a parameter) of a structure names it
static void var_ref(BpVar* v, uint8_t ty) {
    if (v && ST_IS_STRUCT(ty)) snprintf(v->ref, sizeof v->ref, "%s", s_ty_name(g_sc, ty));
}

static Val vpin(uint32_t node, uint8_t pin, uint8_t ty) { return (Val){ ty, false, false, 0, NULL, { 0 }, node, (uint8_t)pin, true }; }
static Val vnum(double x, uint8_t ty) { return (Val){ ty, true, false, x, NULL, { 0 }, 0, 0, true }; }
static Val vbad(void) { return (Val){ ST_UNKNOWN, true, false, 0, NULL, { 0 }, 0, 0, false }; }
static Val out_val(BpNode* n, int k) { // a node's k-th data output as a value
    int pi = n ? pin_find(n, true, false, k) : -1;
    if (pi < 0) return vbad();
    return vpin(n->id, (uint8_t)pi, sty_of_pin(&n->pins[pi]));
}

// A value into a node's input pin: a wire, or the literal written in the pin
static bool feed(Low* L, Val v, BpNode* n, int pi, int line, int col) {
    if (!v.ok || !n || pi < 0) return false;
    BpPin* p = &n->pins[pi];
    if (!v.lit) {
        bool wild = p->type == BPT_WILDCARD && p->wslot == 1;
        if (bp_link(L->g, v.node, v.pin, n->id, (uint8_t)pi)) {
            // a wildcard takes the wired type now (the class isn't registered yet: bp_link can't refresh its node), so
            // the node's outputs are typed for what reads them next (xs[i] + 1)
            BpNode* from = wild ? node_of(L, v.node) : NULL;
            if (from && from->pins[v.pin].type != BPT_WILDCARD && n->wtype == 0) {
                n->wtype = from->pins[v.pin].type;
                snprintf(n->wref, sizeof n->wref, "%s", from->pins[v.pin].ref);
                bp_refresh_pins(L->a, L->g, n);
            }
            return true;
        }
        BpNode* from = node_of(L, v.node);
        s_diag(L->c, line, col, true, "Type mismatch: %s expected, this is %s", g_bp_type_names[p->type],
               from ? g_bp_type_names[from->pins[v.pin].type] : "?");
        return false;
    }
    if (v.none) return true; // (null: the pin's default, none)
    uint8_t t = v.ty & 0x7F;
    if (p->type == BPT_WILDCARD && p->wslot == 1 && !(v.ty & ST_ARRAY)) { // a literal into a wildcard (an array literal's 3 into Add's Item): the node takes its type
        n->wtype = bpt_of(t);
        bp_refresh_pins(L->a, L->g, n);
        p = &n->pins[pi];
    }
    switch (p->type) {
    case BPT_STRING: case BPT_NAME: case BPT_TEXT:
        if (t == ST_STRING) snprintf(p->slit, sizeof p->slit, "%s", v.str ? v.str : "");
        else if (t == ST_INT) snprintf(p->slit, sizeof p->slit, "%lld", (long long)v.num);
        else if (t == ST_BOOL) snprintf(p->slit, sizeof p->slit, "%s", v.num != 0 ? "true" : "false");
        else snprintf(p->slit, sizeof p->slit, "%g", v.num);
        return true;
    case BPT_BOOL: case BPT_INT: case BPT_FLOAT: case BPT_BYTE: case BPT_INT64:
        if (t == ST_STRING && p->type == BPT_INT && p->ref[0]) { // (2026-11-25, item 13) an enum's entry by its name: "Injured"
            int ix = bp_builtin_enum_index(p->ref, v.str);
            if (ix >= 0) return p->lit.v[0] = (float)ix, true;
            int ne = 0;
            const char* const* en = bp_builtin_enum(p->ref, &ne);
            char opts[400] = "";
            for (int k = 0; en && k < ne; k++) snprintf(opts + strlen(opts), sizeof opts - strlen(opts), "%s\"%s\"", k ? ", " : "", en[k]);
            s_diag(L->c, line, col, true, "\"%s\" isn't one of %s", v.str ? v.str : "", opts);
            return false;
        }
        if (t == ST_STRING) break;
        if (t == ST_VECTOR) break;
        if (p->type == BPT_INT64) {
            int64_t x = (int64_t)v.num;
            memcpy(p->lit.v, &x, 8);
        } else p->lit.v[0] = (float)(p->type == BPT_INT || p->type == BPT_BYTE ? floor(v.num) : v.num);
        return true;
    case BPT_VECTOR: case BPT_ROTATOR: case BPT_COLOR:
        if (t == ST_VECTOR || t == ST_ROTATOR || t == ST_COLOR) {
            memcpy(p->lit.v, v.vec, sizeof p->lit.v);
            return true;
        }
        if (t == ST_INT || t == ST_FLOAT) { // (a number for a vector: every component)
            for (int k = 0; k < 3; k++) p->lit.v[k] = (float)v.num;
            if (p->type == BPT_COLOR) p->lit.v[3] = 1.0f;
            return true;
        }
        break;
    default: break;
    }
    s_diag(L->c, line, col, true, "Type mismatch: %s expected, this is %s", g_bp_type_names[p->type], tyname(v.ty));
    return false;
}

// The node joins the exec chain: what was waiting goes into its exec input; its first exec output waits now
static void chain(Low* L, BpNode* n) {
    if (!n) return;
    int in = pin_find(n, false, true, 0);
    for (int i = 0; i < L->npend && in >= 0; i++) bp_link(L->g, L->pend[i].node, L->pend[i].pin, n->id, (uint8_t)in);
    int out = pin_find(n, true, true, 0);
    L->npend = 0;
    if (out >= 0) L->pend[L->npend++] = (Pend){ n->id, (uint8_t)out };
    L->impure++;
}
static void wait_on(Low* L, BpNode* n, int pin) {
    L->npend = 0;
    if (n && pin >= 0) L->pend[L->npend++] = (Pend){ n->id, (uint8_t)pin };
}

// --- Names ---
static void norm(const char* s, char* out, size_t cap) { // lower case without '_' (snake_case meets PascalCase)
    size_t o = 0;
    for (; *s && o + 1 < cap; s++)
        if (*s != '_') out[o++] = (char)tolower((unsigned char)*s);
    out[o] = 0;
}
static bool same(const char* a, const char* b) {
    char x[64], y[64];
    norm(a, x, sizeof x), norm(b, y, sizeof y);
    return !strcmp(x, y);
}

static int field_index(Low* L, const char* name) {
    for (int i = 0; i < L->cls->nfields; i++)
        if (!strcmp(L->cls->fields[i].name, name)) return bp_var_index(L->a, name);
    return -1;
}
static uint8_t field_type(Low* L, const char* name) { // (its declared type; unknown: float)
    for (int i = 0; i < L->cls->nfields; i++)
        if (!strcmp(L->cls->fields[i].name, name)) return L->cls->fields[i].type & 0x7F ? (uint8_t)(L->cls->fields[i].type & 0x7F) : ST_FLOAT;
    return ST_FLOAT;
}
static Local* local_find(Low* L, const char* name) {
    for (int i = L->nloc - 1; i >= 0; i--)
        if (!strcmp(L->loc[i].name, name)) return &L->loc[i];
    return NULL;
}
static SMethod* method_find(Low* L, const char* name) {
    for (int i = 0; i < L->cls->nmethods; i++)
        if (!strcmp(L->cls->methods[i].name, name)) return &L->cls->methods[i];
    for (int i = 0; i < L->cls->nmethods; i++)
        if (same(L->cls->methods[i].name, name)) return &L->cls->methods[i];
    return NULL;
}

// The engine's events by a method's name (any language's spelling)
enum { EV_NONE = 0, EV_BEGIN, EV_TICK, EV_END, EV_OVERLAP_BEGIN, EV_OVERLAP_END, EV_DAMAGED, EV_HEALED, EV_DEATH, EV_SHIELD_BROKEN, EV_INV_CHANGED, EV_ITEM_USED, EV_ITEM_EQUIPPED, EV_ITEM_UNEQUIPPED, EV_GAME_SAVED, EV_GAME_LOADED, EV_CHECKPOINT, EV_IA_INTERACT, EV_IA_FOCUS_BEGIN, EV_IA_FOCUS_END, EV_IA_HOLD, EV_DLG_LINE, EV_DLG_CHOICE, EV_DLG_END, EV_QST_STARTED, EV_QST_OBJ_UPDATED, EV_QST_OBJ_COMPLETED, EV_QST_COMPLETED, EV_QST_FAILED, EV_GR_PHASE, EV_GR_SCORE, EV_GR_WON, EV_GR_LOST, EV_GR_ROUND_START, EV_GR_ROUND_END, EV_GR_RESPAWNED, EV_GR_TIMER, EV_ABIL_ACTIVATE, EV_ABIL_ACTIVATED, EV_ABIL_ENDED, EV_GE_APPLIED, EV_GE_REMOVED, EV_ATTR_CHANGED };
static int engine_event(const char* name) {
    char n[64];
    norm(name, n, sizeof n);
    if (!strcmp(n, "beginplay") || !strcmp(n, "start") || !strcmp(n, "awake") || !strcmp(n, "ready") || !strcmp(n, "ctor")) return EV_BEGIN;
    if (!strcmp(n, "tick") || !strcmp(n, "update") || !strcmp(n, "process")) return EV_TICK;
    if (!strcmp(n, "endplay") || !strcmp(n, "ondestroy")) return EV_END;
    if (!strcmp(n, "onbeginoverlap") || !strcmp(n, "onactorbeginoverlap") || !strcmp(n, "ontriggerenter") || !strcmp(n, "beginoverlap")) return EV_OVERLAP_BEGIN;
    if (!strcmp(n, "onendoverlap") || !strcmp(n, "onactorendoverlap") || !strcmp(n, "ontriggerexit") || !strcmp(n, "endoverlap")) return EV_OVERLAP_END;
    // the Health's events (health.h): Event AnyDamage, On Healed, On Death, On Shield Broken (only the On... names: a
    // script's own TakeDamage function stays a function)
    if (!strcmp(n, "onanydamage") || !strcmp(n, "receiveanydamage") || !strcmp(n, "ondamaged") || !strcmp(n, "ontakedamage")) return EV_DAMAGED;
    if (!strcmp(n, "onhealed")) return EV_HEALED;
    if (!strcmp(n, "ondeath") || !strcmp(n, "ondied")) return EV_DEATH;
    if (!strcmp(n, "onshieldbroken")) return EV_SHIELD_BROKEN;
    // the inventory's (inventory.h): its events' values as the parameters (Item, Slot Name)
    if (!strcmp(n, "oninventorychanged") || !strcmp(n, "inventorychanged")) return EV_INV_CHANGED;
    if (!strcmp(n, "onitemused") || !strcmp(n, "itemused")) return EV_ITEM_USED;
    if (!strcmp(n, "onitemequipped") || !strcmp(n, "itemequipped")) return EV_ITEM_EQUIPPED;
    if (!strcmp(n, "onitemunequipped") || !strcmp(n, "itemunequipped")) return EV_ITEM_UNEQUIPPED;
    // the saves' events (save_progress.h): On Game Saved, On Game Loaded, On Checkpoint Reached
    if (!strcmp(n, "ongamesaved") || !strcmp(n, "onsaved")) return EV_GAME_SAVED;
    if (!strcmp(n, "ongameloaded") || !strcmp(n, "onloaded")) return EV_GAME_LOADED;
    if (!strcmp(n, "oncheckpointreached") || !strcmp(n, "oncheckpoint")) return EV_CHECKPOINT;
    // the Interactable's (interaction.h): the instigator (and the hold's progress) as the parameters
    if (!strcmp(n, "oninteract") || !strcmp(n, "oninteracted")) return EV_IA_INTERACT;
    if (!strcmp(n, "onfocusbegin") || !strcmp(n, "onfocused")) return EV_IA_FOCUS_BEGIN;
    if (!strcmp(n, "onfocusend") || !strcmp(n, "onunfocused")) return EV_IA_FOCUS_END;
    if (!strcmp(n, "onholdprogress")) return EV_IA_HOLD;
    // the conversations' (dialogue.h): Speaker, Speaker Name, Text, Line / Speaker, Option, Text / Speaker, Dialogue
    if (!strcmp(n, "ondialogueline")) return EV_DLG_LINE;
    if (!strcmp(n, "ondialoguechoice")) return EV_DLG_CHOICE;
    if (!strcmp(n, "ondialogueend")) return EV_DLG_END;
    // the quests' (quest.h): Quest, Quest Log / Quest, Objective, Index, Progress, Count / Quest, Objective, Index
    if (!strcmp(n, "onqueststarted")) return EV_QST_STARTED;
    if (!strcmp(n, "onobjectiveupdated")) return EV_QST_OBJ_UPDATED;
    if (!strcmp(n, "onobjectivecompleted")) return EV_QST_OBJ_COMPLETED;
    if (!strcmp(n, "onquestcompleted")) return EV_QST_COMPLETED;
    if (!strcmp(n, "onquestfailed")) return EV_QST_FAILED;
    // the Game Rules' (game_rules.h): to every class that has them
    if (!strcmp(n, "onmatchphasechanged") || !strcmp(n, "onphasechanged")) return EV_GR_PHASE;
    if (!strcmp(n, "onscorechanged")) return EV_GR_SCORE;
    if (!strcmp(n, "onmatchwon") || !strcmp(n, "onvictory")) return EV_GR_WON;
    if (!strcmp(n, "onmatchlost") || !strcmp(n, "ondefeat")) return EV_GR_LOST;
    if (!strcmp(n, "onroundstarted") || !strcmp(n, "onroundstart")) return EV_GR_ROUND_START;
    if (!strcmp(n, "onroundended") || !strcmp(n, "onroundend")) return EV_GR_ROUND_END;
    if (!strcmp(n, "onplayerrespawned") || !strcmp(n, "onrespawned")) return EV_GR_RESPAWNED;
    if (!strcmp(n, "ontimerexpired") || !strcmp(n, "ontimeup")) return EV_GR_TIMER;
    // the abilities' and effects' (ability.h): a Gameplay Ability's ActivateAbility (Owner); on the owner: Ability /
    // Ability, Was Cancelled / Effect, Stacks, Source / Effect / Attribute, New Value, Old Value
    if (!strcmp(n, "onactivateability") || !strcmp(n, "activateability")) return EV_ABIL_ACTIVATE;
    if (!strcmp(n, "onabilityactivated")) return EV_ABIL_ACTIVATED;
    if (!strcmp(n, "onabilityended")) return EV_ABIL_ENDED;
    if (!strcmp(n, "oneffectapplied")) return EV_GE_APPLIED;
    if (!strcmp(n, "oneffectremoved")) return EV_GE_REMOVED;
    if (!strcmp(n, "onattributechanged")) return EV_ATTR_CHANGED;
    return EV_NONE;
}
static bool is_function(const SMethod* m) { return m->nparams > 0 || (m->ret != ST_VOID && m->ret != ST_UNKNOWN); }

// --- Expressions ---
static Val eval(Low* L, Expr* e);
// The node's actor "Target" (self, or obj in obj.F()): -1 when it has none. A pin named Target of another type is an
// argument like the others (Find Look at Rotation's Target is a vector)
static int target_pin(const BpNode* n) {
    int t = pin_named(n, false, "Target");
    return t >= 0 && n->pins[t].type == BPT_ACTOR ? t : -1;
}
static BpNode* add(Low* L, uint16_t kind, int line, int col, const char* text, const char* text2);

// A structure's field (Break <S>: its output by name, any spelling); -1 in *idx: none
static Val struct_get(Low* L, Val o, const char* field, int line, int col, bool* found) {
    *found = false;
    const char* sname = s_ty_name(L->c, o.ty);
    BpNode* b = add(L, BPK_BREAK_STRUCT, line, col, sname, NULL);
    if (!b) return vbad();
    feed(L, o, b, pin_find(b, false, false, 0), line, col);
    for (int k = 0;; k++) {
        int pi = pin_find(b, true, false, k);
        if (pi < 0) break;
        if (same(b->pins[pi].name, field)) {
            *found = true;
            return out_val(b, k);
        }
    }
    return vbad();
}
// The structure o with field set to v (Break, then Make with the others wired through)
static Val struct_with(Low* L, Val o, const char* field, Val v, int line, int col, bool* found) {
    *found = false;
    const char* sname = s_ty_name(L->c, o.ty);
    BpNode* b = add(L, BPK_BREAK_STRUCT, line, col, sname, NULL);
    BpNode* mk = add(L, BPK_MAKE_STRUCT, line, col, sname, NULL);
    if (!b || !mk) return vbad();
    feed(L, o, b, pin_find(b, false, false, 0), line, col);
    for (int k = 0;; k++) {
        int po = pin_find(b, true, false, k), pi = pin_find(mk, false, false, k);
        if (po < 0 || pi < 0) break;
        if (same(b->pins[po].name, field)) *found = true, feed(L, v, mk, pi, line, col);
        else feed(L, out_val(b, k), mk, pi, line, col);
    }
    return out_val(mk, 0);
}
static int new_local_var(Low* L, const char* name, uint8_t ty, int line, int col);
static Val make_struct(Low* L, const char* nm, Expr** args, int nargs, int line, int col);
static Val get_var(Low* L, int vi, int line, int col);
static Val call_node(Low* L, uint16_t kind, Val* args, int nargs, int line, int col, const char* text);

// An array literal's element type: the items' (ints with a float: float); a bad item: unknown
static uint8_t list_elem(Low* L, Expr* e) {
    uint8_t t = ST_UNKNOWN;
    for (int i = 0; i < e->nargs; i++) {
        Expr* it = e->args[i];
        uint8_t k = it->k == EX_INT ? ST_INT : it->k == EX_FLOAT ? ST_FLOAT : it->k == EX_BOOL ? ST_BOOL
                  : it->k == EX_STRING || it->k == EX_INTERP ? ST_STRING : ST_UNKNOWN;
        if (k == ST_UNKNOWN) { // (an expression: its type from a throwaway evaluation would add nodes; the first one decides)
            Local* lc = it->k == EX_NAME ? local_find(L, it->s) : NULL;
            int fi = it->k == EX_NAME && !lc ? field_index(L, it->s) : -1;
            k = lc ? (uint8_t)(lc->ty & 0x7F) : fi >= 0 ? field_type(L, it->s) : ST_FLOAT;
            if (it->k == EX_CALL && it->s && (!strcmp(it->s, "Vector") || !strcmp(it->s, "FVector") || !strcmp(it->s, "vector"))) k = ST_VECTOR;
        }
        if (t == ST_UNKNOWN || (t == ST_INT && k == ST_FLOAT)) t = k;
    }
    return t == ST_UNKNOWN ? ST_FLOAT : t;
}

// The array variable vi made the literal's items: cleared, then each one added (in the line's order)
static void list_into(Low* L, int vi, Expr* e) {
    Val a[1] = { get_var(L, vi, e->line, e->col) };
    call_node(L, BPK_ARR_CLEAR, a, 1, e->line, e->col, NULL);
    for (int i = 0; i < e->nargs; i++) {
        Val it = eval(L, e->args[i]);
        if (!it.ok) continue;
        if (it.lit && (it.ty & 0x7F) == ST_INT && L->a->vars[vi].type == BPT_FLOAT) it.ty = ST_FLOAT; // (float[] xs = {1, 2})
        Val b2[2] = { get_var(L, vi, e->line, e->col), it };
        call_node(L, BPK_ARR_ADD, b2, 2, e->args[i]->line, e->args[i]->col, NULL);
    }
}

static Val literal_vec(float x, float y, float z, float w, uint8_t ty) {
    Val v = vnum(0, ty);
    v.vec[0] = x, v.vec[1] = y, v.vec[2] = z, v.vec[3] = w;
    return v;
}

static Val call_node(Low* L, uint16_t kind, Val* args, int nargs, int line, int col, const char* text) {
    BpNode* n = add(L, kind, line, col, text, NULL);
    if (!n) return vbad();
    for (int k = 0; k < nargs; k++) {
        int pi = pin_find(n, false, false, k);
        if (pi < 0) {
            s_diag(L->c, line, col, true, "Too many arguments (%d expected)", k);
            break;
        }
        feed(L, args[k], n, pi, line, col);
    }
    if (!bp_node_desc(kind)->pure) chain(L, n);
    return out_val(n, 0);
}

static bool numeric(uint8_t t) { return t == ST_INT || t == ST_FLOAT || t == ST_INT64 || t == ST_BOOL; }

static Val binop(Low* L, const char* op, Val a, Val b, int line, int col) {
    if (!a.ok || !b.ok) return vbad();
    uint8_t ta = a.ty & 0x7F, tb = b.ty & 0x7F;
    // constants folded
    if (a.lit && b.lit && !a.none && !b.none && numeric(ta) && numeric(tb)) {
        double x = a.num, y = b.num, r = 0;
        bool ints = ta == ST_INT && tb == ST_INT, cmp = false;
        if (!strcmp(op, "+")) r = x + y;
        else if (!strcmp(op, "-")) r = x - y;
        else if (!strcmp(op, "*")) r = x * y;
        else if (!strcmp(op, "/")) r = y != 0 ? (ints ? (double)((long long)x / (long long)y) : x / y) : 0;
        else if (!strcmp(op, "%")) r = y != 0 ? (ints ? (double)((long long)x % (long long)y) : fmod(x, y)) : 0;
        else if (!strcmp(op, "//")) r = y != 0 ? floor(x / y) : 0;
        else if (!strcmp(op, "**")) r = pow(x, y);
        else if (!strcmp(op, "<")) r = x < y, cmp = true;
        else if (!strcmp(op, ">")) r = x > y, cmp = true;
        else if (!strcmp(op, "<=")) r = x <= y, cmp = true;
        else if (!strcmp(op, ">=")) r = x >= y, cmp = true;
        else if (!strcmp(op, "==")) r = x == y, cmp = true;
        else if (!strcmp(op, "!=")) r = x != y, cmp = true;
        else if (!strcmp(op, "and")) r = x != 0 && y != 0, cmp = true;
        else if (!strcmp(op, "or")) r = x != 0 || y != 0, cmp = true;
        return vnum(r, cmp ? ST_BOOL : ints && strcmp(op, "**") ? ST_INT : ST_FLOAT);
    }
    if (a.lit && b.lit && ta == ST_STRING && tb == ST_STRING && !strcmp(op, "+")) {
        size_t la = strlen(a.str ? a.str : ""), lb = strlen(b.str ? b.str : "");
        char* s = sa_alloc(&L->c->arena, la + lb + 1);
        if (s) memcpy(s, a.str ? a.str : "", la), memcpy(s + la, b.str ? b.str : "", lb + 1);
        Val v = vnum(0, ST_STRING);
        v.str = s;
        return v;
    }
    Val ab[2] = { a, b };
    // strings
    if ((ta == ST_STRING || tb == ST_STRING) && !strcmp(op, "+")) return call_node(L, BPK_APPEND, ab, 2, line, col, NULL);
    if (ta == ST_STRING && tb == ST_STRING && (!strcmp(op, "==") || !strcmp(op, "!="))) {
        Val r = call_node(L, BPK_STR_EQUAL, ab, 2, line, col, NULL);
        return !strcmp(op, "!=") ? call_node(L, BPK_NOT, &r, 1, line, col, NULL) : r;
    }
    // booleans
    if (!strcmp(op, "and")) return call_node(L, BPK_AND, ab, 2, line, col, NULL);
    if (!strcmp(op, "or")) return call_node(L, BPK_OR, ab, 2, line, col, NULL);
    if (ta == ST_BOOL && tb == ST_BOOL && (!strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "^"))) {
        Val r = call_node(L, BPK_XOR, ab, 2, line, col, NULL);
        return !strcmp(op, "==") ? call_node(L, BPK_NOT, &r, 1, line, col, NULL) : r;
    }
    // actors / objects
    if ((ta == ST_ACTOR || ta == ST_OBJECT || a.none) && (tb == ST_ACTOR || tb == ST_OBJECT || b.none) && (!strcmp(op, "==") || !strcmp(op, "!="))) {
        Val x = a.none ? b : a;
        Val r;
        if (a.none || b.none) { // (x == null: not Is Valid)
            r = call_node(L, BPK_IS_VALID, &x, 1, line, col, NULL);
            return !strcmp(op, "==") ? call_node(L, BPK_NOT, &r, 1, line, col, NULL) : r;
        }
        r = call_node(L, BPK_OBJ_EQUAL, ab, 2, line, col, NULL);
        return !strcmp(op, "!=") ? call_node(L, BPK_NOT, &r, 1, line, col, NULL) : r;
    }
    // vectors
    if (ta == ST_VECTOR || tb == ST_VECTOR) {
        if (ta == ST_VECTOR && tb == ST_VECTOR) {
            if (!strcmp(op, "+")) return call_node(L, BPK_VADD, ab, 2, line, col, NULL);
            if (!strcmp(op, "-")) return call_node(L, BPK_VSUB, ab, 2, line, col, NULL);
            if (!strcmp(op, "*")) return call_node(L, BPK_VMUL, ab, 2, line, col, NULL);
            if (!strcmp(op, "==") || !strcmp(op, "!=")) {
                Val r = call_node(L, BPK_VEQUAL, ab, 2, line, col, NULL);
                return !strcmp(op, "!=") ? call_node(L, BPK_NOT, &r, 1, line, col, NULL) : r;
            }
        } else if (ta == ST_VECTOR && numeric(tb)) {
            if (!strcmp(op, "*")) return call_node(L, BPK_VMULS, ab, 2, line, col, NULL);
            if (!strcmp(op, "/")) return call_node(L, BPK_VDIVS, ab, 2, line, col, NULL);
        } else if (numeric(ta) && tb == ST_VECTOR && !strcmp(op, "*")) {
            Val ba[2] = { b, a };
            return call_node(L, BPK_VMULS, ba, 2, line, col, NULL);
        }
        s_diag(L->c, line, col, true, "'%s' doesn't apply to %s and %s", op, tyname(a.ty), tyname(b.ty));
        return vbad();
    }
    if (!numeric(ta) || !numeric(tb)) {
        s_diag(L->c, line, col, true, "'%s' doesn't apply to %s and %s", op, tyname(a.ty), tyname(b.ty));
        return vbad();
    }
    bool ints = (ta == ST_INT || ta == ST_BOOL) && (tb == ST_INT || tb == ST_BOOL);
    if (ta == ST_INT64 || tb == ST_INT64) {
        if (!strcmp(op, "+")) return call_node(L, BPK_INT64_ADD, ab, 2, line, col, NULL);
        if (!strcmp(op, "-")) return call_node(L, BPK_INT64_SUB, ab, 2, line, col, NULL);
        if (!strcmp(op, "*")) return call_node(L, BPK_INT64_MUL, ab, 2, line, col, NULL);
        if (!strcmp(op, "/")) return call_node(L, BPK_INT64_DIV, ab, 2, line, col, NULL);
        ints = false; // (compared as floats)
    }
    static const struct { const char* op; uint16_t f, i; } t[] = {
        { "+", BPK_ADD, BPK_IADD }, { "-", BPK_SUB, BPK_ISUB }, { "*", BPK_MUL, BPK_IMUL }, { "/", BPK_DIV, BPK_IDIV },
        { "%", BPK_FMOD, BPK_IMOD }, { "<", BPK_LESS, BPK_ILESS }, { ">", BPK_GREATER, BPK_IGREATER }, { "<=", BPK_FLE, BPK_ILE },
        { ">=", BPK_FGE, BPK_IGE }, { "==", BPK_EQUAL, BPK_IEQUAL }, { "!=", BPK_FNOTEQUAL, BPK_INOTEQUAL },
    };
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++)
        if (!strcmp(op, t[k].op)) return call_node(L, ints ? t[k].i : t[k].f, ab, 2, line, col, NULL);
    if (!strcmp(op, "**")) return call_node(L, BPK_POW, ab, 2, line, col, NULL);
    if (!strcmp(op, "//")) {
        Val d = call_node(L, BPK_DIV, ab, 2, line, col, NULL);
        return call_node(L, BPK_FLOOR, &d, 1, line, col, NULL);
    }
    s_diag(L->c, line, col, true, "The operator '%s' isn't supported", op);
    return vbad();
}

// The built-in functions every language has (Vector, print, len, str, abs, min...); handled: true
static bool builtin(Low* L, Expr* e, Val* out) {
    const char* nm = e->s;
    char n[64];
    norm(nm, n, sizeof n);
    Val a[4];
    int na = e->nargs < 4 ? e->nargs : 4;
    int line = e->line, col = e->col;
#define ARGS() for (int k = 0; k < na; k++) a[k] = eval(L, e->args[k])
    if (!strcmp(n, "vector") || !strcmp(n, "vector3") || !strcmp(n, "vec3") || !strcmp(n, "fvector") || !strcmp(n, "makevector")) {
        ARGS();
        bool all = true;
        for (int k = 0; k < na; k++) all &= a[k].lit && numeric(a[k].ty & 0x7F);
        if (all) { *out = literal_vec(na > 0 ? (float)a[0].num : 0, na > 1 ? (float)a[1].num : 0, na > 2 ? (float)a[2].num : 0, 0, ST_VECTOR); return true; }
        *out = call_node(L, BPK_MAKE_VECTOR, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "rotator") || !strcmp(n, "frotator") || !strcmp(n, "makerotator")) {
        ARGS();
        bool all = true;
        for (int k = 0; k < na; k++) all &= a[k].lit;
        if (all) { *out = literal_vec(na > 0 ? (float)a[0].num : 0, na > 1 ? (float)a[1].num : 0, na > 2 ? (float)a[2].num : 0, 0, ST_ROTATOR); return true; }
        *out = call_node(L, BPK_MAKE_ROTATOR, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "color") || !strcmp(n, "linearcolor") || !strcmp(n, "makelinearcolor")) {
        ARGS();
        bool all = true;
        for (int k = 0; k < na; k++) all &= a[k].lit;
        if (all) { *out = literal_vec(na > 0 ? (float)a[0].num : 0, na > 1 ? (float)a[1].num : 0, na > 2 ? (float)a[2].num : 0, na > 3 ? (float)a[3].num : 1, ST_COLOR); return true; }
        *out = call_node(L, BPK_MAKE_COLOR, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "print") || !strcmp(n, "printstring") || !strcmp(n, "debuglog") || !strcmp(n, "log") || !strcmp(n, "printf") ||
        !strcmp(n, "printline") || !strcmp(n, "writeline") || !strcmp(n, "puts")) {
        ARGS();
        Val s = na ? a[0] : vnum(0, ST_STRING);
        for (int k = 1; k < na; k++) { // print(a, b): joined by a space (Python's)
            Val sp = vnum(0, ST_STRING);
            sp.str = " ";
            s = binop(L, "+", binop(L, "+", s, sp, line, col), a[k], line, col);
        }
        BpNode* pn = add(L, BPK_PRINT, line, col, NULL, NULL);
        if (pn) feed(L, s, pn, pin_find(pn, false, false, 0), line, col), chain(L, pn);
        *out = vnum(0, ST_VOID);
        return true;
    }
    static const struct { const char* name; uint16_t f, i; int nargs; uint8_t ret; } math[] = {
        { "abs", BPK_FABS, BPK_IABS, 1, 0 }, { "min", BPK_FMIN, BPK_IMIN, 2, 0 }, { "max", BPK_FMAX, BPK_IMAX, 2, 0 },
        { "clamp", BPK_CLAMP, BPK_ICLAMP, 3, 0 }, { "sqrt", BPK_SQRT, BPK_SQRT, 1, ST_FLOAT }, { "pow", BPK_POW, BPK_POW, 2, ST_FLOAT },
        { "floor", BPK_FLOOR, BPK_FLOOR, 1, ST_INT }, { "ceil", BPK_CEIL, BPK_CEIL, 1, ST_INT }, { "round", BPK_ROUND, BPK_ROUND, 1, ST_INT },
        { "sin", BPK_SIN, BPK_SIN, 1, ST_FLOAT }, { "cos", BPK_COS, BPK_COS, 1, ST_FLOAT }, { "tan", BPK_TAN, BPK_TAN, 1, ST_FLOAT },
        { "atan2", BPK_ATAN2, BPK_ATAN2, 2, ST_FLOAT }, { "lerp", BPK_LERP, BPK_LERP, 3, ST_FLOAT },
        { "fmod", BPK_FMOD, BPK_FMOD, 2, ST_FLOAT },
    };
    for (size_t k = 0; k < sizeof math / sizeof math[0]; k++)
        if (!strcmp(n, math[k].name)) {
            ARGS();
            bool ints = true;
            for (int j = 0; j < na; j++) ints &= (a[j].ty & 0x7F) == ST_INT;
            if (na == 1 && a[0].lit && numeric(a[0].ty & 0x7F)) { // (folded)
                double x = a[0].num, r = 0;
                if (!strcmp(n, "abs")) r = fabs(x);
                else if (!strcmp(n, "sqrt")) r = sqrt(x);
                else if (!strcmp(n, "floor")) r = floor(x);
                else if (!strcmp(n, "ceil")) r = ceil(x);
                else if (!strcmp(n, "round")) r = floor(x + 0.5);
                else if (!strcmp(n, "sin")) r = sin(x * 3.14159265358979 / 180.0);
                else if (!strcmp(n, "cos")) r = cos(x * 3.14159265358979 / 180.0);
                else if (!strcmp(n, "tan")) r = tan(x * 3.14159265358979 / 180.0);
                *out = vnum(r, math[k].ret ? math[k].ret : a[0].ty);
                return true;
            }
            *out = call_node(L, ints ? math[k].i : math[k].f, a, na, line, col, NULL);
            return true;
        }
    if (!strcmp(n, "len") || !strcmp(n, "length") || !strcmp(n, "count")) {
        ARGS();
        if (na == 1 && (a[0].ty & ST_ARRAY)) *out = call_node(L, BPK_ARR_LEN, a, 1, line, col, NULL);
        else if (na == 1 && a[0].lit && (a[0].ty & 0x7F) == ST_STRING) *out = vnum((double)strlen(a[0].str ? a[0].str : ""), ST_INT);
        else *out = call_node(L, BPK_STR_LEN, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "str") || !strcmp(n, "tostring") || !strcmp(n, "string")) {
        ARGS();
        if (na == 1 && (a[0].ty & 0x7F) == ST_STRING) { *out = a[0]; return true; }
        *out = call_node(L, BPK_TO_STRING, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "int") || !strcmp(n, "toint") || !strcmp(n, "truncate") || !strcmp(n, "int32")) {
        ARGS();
        if (na == 1 && a[0].lit && numeric(a[0].ty & 0x7F)) { *out = vnum(trunc(a[0].num), ST_INT); return true; }
        *out = call_node(L, (a[0].ty & 0x7F) == ST_STRING ? BPK_STRING_TO_INT : BPK_FLOAT_TO_INT, a, 1, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "float") || !strcmp(n, "tofloat") || !strcmp(n, "double")) {
        ARGS();
        if (na == 1 && a[0].lit && numeric(a[0].ty & 0x7F)) { *out = vnum(a[0].num, ST_FLOAT); return true; }
        *out = call_node(L, (a[0].ty & 0x7F) == ST_STRING ? BPK_STRING_TO_FLOAT : BPK_INT_TO_FLOAT, a, 1, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "random") || !strcmp(n, "randf") || !strcmp(n, "randomfloat") || !strcmp(n, "uniform") || !strcmp(n, "randomrange")) {
        ARGS();
        if (na == 2) *out = call_node(L, (a[0].ty & 0x7F) == ST_INT && (a[1].ty & 0x7F) == ST_INT && strcmp(n, "uniform") ? BPK_RAND_INT_RANGE : BPK_RANDOM, a, 2, line, col, NULL);
        else *out = call_node(L, BPK_RAND_FLOAT, a, 0, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "randint") || !strcmp(n, "randomint")) {
        ARGS();
        *out = call_node(L, na == 2 ? BPK_RAND_INT_RANGE : BPK_RAND_INT, a, na, line, col, NULL);
        return true;
    }
    if (!strcmp(n, "bool")) {
        ARGS();
        Val z = vnum(0, (a[0].ty & 0x7F) == ST_INT ? ST_INT : ST_FLOAT);
        *out = binop(L, "!=", a[0], z, line, col);
        return true;
    }
    return false;
#undef ARGS
}

static Val get_var(Low* L, int vi, int line, int col) {
    if (vi < 0) return vbad();
    BpNode* n = add(L, BPK_VAR_GET, line, col, L->a->vars[vi].name, NULL);
    return out_val(n, 0);
}
static void set_var(Low* L, int vi, Val v, int line, int col) {
    if (vi < 0 || !v.ok) return;
    BpNode* n = add(L, BPK_VAR_SET, line, col, L->a->vars[vi].name, NULL);
    if (!n) return;
    feed(L, v, n, pin_find(n, false, false, 0), line, col);
    chain(L, n);
}

static Val eval_name(Low* L, Expr* e) {
    Local* lc = local_find(L, e->s);
    if (lc) {
        if (lc->var >= 0) return get_var(L, lc->var, e->line, e->col);
        return vpin(lc->node, lc->pin, lc->ty);
    }
    int fi = field_index(L, e->s);
    if (fi >= 0) return get_var(L, fi, e->line, e->col);
    // a zero-argument engine value used without parentheses (Python-like properties): DeltaSeconds...
    const ScriptApi* api = s_api_find(e->s);
    if (api && api->pure) {
        Val none[1];
        return call_node(L, api->kind, none, 0, e->line, e->col, NULL);
    }
    s_diag(L->c, e->line, e->col, true, "'%s' isn't declared here", e->s);
    return vbad();
}

// A call: a method of the class, a built-in, the engine's API (obj.F(...): obj is its Target)
static Val eval_call(Low* L, Expr* e) {
    int line = e->line, col = e->col;
    const char* nm = e->s ? e->s : "";
    bool on_self = !e->a || e->a->k == EX_SELF;
    uint8_t sty = !e->a ? s_struct_type(L->c, nm) : ST_UNKNOWN;
    if (sty != ST_UNKNOWN) return make_struct(L, nm, e->args, e->nargs, line, col); // a structure's value
    // arrays' methods: arr.Add(x), arr.append(x), arr.Contains(x), arr.Remove(x), arr.Clear(), arr.Count()
    if (!on_self) {
        Val obj = eval(L, e->a);
        if (obj.ok && (obj.ty & ST_ARRAY)) {
            char n[32];
            norm(nm, n, sizeof n);
            Val a[3] = { obj };
            for (int k = 0; k < e->nargs && k < 2; k++) a[k + 1] = eval(L, e->args[k]);
            uint16_t kind = !strcmp(n, "add") || !strcmp(n, "append") || !strcmp(n, "push") || !strcmp(n, "pushback") ? BPK_ARR_ADD
                          : !strcmp(n, "contains") || !strcmp(n, "includes") ? BPK_ARR_CONTAINS
                          : !strcmp(n, "remove") ? BPK_ARR_REMOVE : !strcmp(n, "removeat") || !strcmp(n, "pop") ? BPK_ARR_REMOVE_INDEX
                          : !strcmp(n, "clear") ? BPK_ARR_CLEAR : !strcmp(n, "count") || !strcmp(n, "length") || !strcmp(n, "size") || !strcmp(n, "len") ? BPK_ARR_LEN
                          : !strcmp(n, "insert") ? BPK_ARR_INSERT : !strcmp(n, "indexof") || !strcmp(n, "find") || !strcmp(n, "index") ? BPK_ARR_FIND
                          : !strcmp(n, "shuffle") ? BPK_ARR_SHUFFLE : 0;
            if (!kind) {
                s_diag(L->c, line, col, true, "Arrays have no '%s' (Add, Remove, RemoveAt, Contains, IndexOf, Insert, Clear, Count, Shuffle)", nm);
                return vbad();
            }
            if (kind == BPK_ARR_INSERT && e->nargs >= 2) { Val t = a[1]; a[1] = a[2], a[2] = t; } // (Insert(index, item): the node's Item, Index)
            return call_node(L, kind, a, 1 + (e->nargs < 2 ? e->nargs : 2), line, col, NULL);
        }
        if (obj.ok && ((obj.ty & 0x7F) == ST_VECTOR || (obj.ty & 0x7F) == ST_STRING)) { // v.Length(), v.Normalize(), s.ToUpper()...
            char n[32];
            norm(nm, n, sizeof n);
            uint16_t kind = !strcmp(n, "length") || !strcmp(n, "magnitude") || !strcmp(n, "size") ? ((obj.ty & 0x7F) == ST_STRING ? BPK_STR_LEN : BPK_VLEN)
                          : !strcmp(n, "normalize") || !strcmp(n, "normalized") || !strcmp(n, "getsafenormal") ? BPK_NORMALIZE
                          : !strcmp(n, "dot") ? BPK_DOT : !strcmp(n, "cross") ? BPK_CROSS : !strcmp(n, "distance") ? BPK_VDIST
                          : !strcmp(n, "toupper") || !strcmp(n, "upper") ? BPK_STR_UPPER : !strcmp(n, "tolower") || !strcmp(n, "lower") ? BPK_STR_LOWER
                          : !strcmp(n, "contains") ? BPK_STR_CONTAINS : !strcmp(n, "replace") ? BPK_STR_REPLACE
                          : !strcmp(n, "substring") ? BPK_SUBSTRING : 0;
            if (kind) {
                Val a[3] = { obj };
                for (int k = 0; k < e->nargs && k < 2; k++) a[k + 1] = eval(L, e->args[k]);
                return call_node(L, kind, a, 1 + (e->nargs < 2 ? e->nargs : 2), line, col, NULL);
            }
        }
        if (!obj.ok) return vbad();
        // an engine function on another actor: obj.SetActorLocation(v) -> Target = obj
        const ScriptApi* api = s_api_find(nm);
        if (api) {
            BpNode* n = add(L, api->kind, line, col, api->fixed[0] ? api->fixed : NULL, NULL);
            if (!n) return vbad();
            int target = target_pin(n);
            if (target < 0) target = pin_find(n, false, false, 0);
            feed(L, obj, n, target, line, col);
            int k = 0;
            for (int i = 0; i < n->npins && k < e->nargs; i++) {
                if (n->pins[i].out || n->pins[i].type == BPT_EXEC || i == target) continue;
                feed(L, eval(L, e->args[k++]), n, i, line, col);
            }
            if (!bp_node_desc(api->kind)->pure) chain(L, n);
            return out_val(n, 0);
        }
        s_diag(L->c, line, col, true, "%s has no '%s'", tyname(obj.ty), nm);
        return vbad();
    }
    // the class's own methods
    SMethod* m = method_find(L, nm);
    if (m && !engine_event(m->name)) {
        if (!is_function(m)) {
            if (e->nargs) s_diag(L->c, line, col, true, "'%s' takes no arguments", m->name);
            BpNode* n = add(L, BPK_CALL_CUSTOM, line, col, m->name, NULL);
            chain(L, n);
            return vnum(0, ST_VOID);
        }
        BpNode* n = add(L, BPK_CALL_FUNCTION, line, col, m->name, NULL);
        if (!n) return vbad();
        if (e->nargs != m->nparams) s_diag(L->c, line, col, true, "'%s' takes %d argument%s, not %d", m->name, m->nparams, m->nparams == 1 ? "" : "s", e->nargs);
        for (int k = 0; k < e->nargs && k < m->nparams; k++) feed(L, eval(L, e->args[k]), n, pin_find(n, false, false, k), line, col);
        chain(L, n);
        return out_val(n, 0);
    }
    Val b;
    if (builtin(L, e, &b)) return b;
    const ScriptApi* api = s_api_find(nm);
    if (!api) {
        s_diag(L->c, line, col, true, "'%s' isn't a method of the class nor a function of the engine", nm);
        for (int k = 0; k < e->nargs; k++) eval(L, e->args[k]);
        return vbad();
    }
    // the node's text (an asset, a widget, an event): its first argument, a text literal
    const char* text = NULL;
    int first = 0;
    if (api->text_arg) {
        if (e->nargs && e->args[0]->k == EX_STRING) text = e->args[0]->s, first = 1;
        else s_diag(L->c, line, col, true, "%s's first argument is a name written in quotes (an asset, a widget, an event)", api->name);
    } else if (api->fixed[0])
        text = api->fixed; // (a plugin's function: its node by name)
    BpNode* n = add(L, api->kind, line, col, text, NULL);
    if (!n) return vbad();
    int target = target_pin(n);
    int k = first;
    for (int i = 0; i < n->npins && k < e->nargs; i++) {
        if (n->pins[i].out || n->pins[i].type == BPT_EXEC || i == target) continue; // (Target: self)
        feed(L, eval(L, e->args[k++]), n, i, line, col);
    }
    if (k < e->nargs) s_diag(L->c, line, col, true, "%s takes %d argument%s", api->name, k, k == 1 ? "" : "s");
    if (!bp_node_desc(api->kind)->pure) chain(L, n);
    Val r = out_val(n, 0);
    if (!r.ok) r = vnum(0, ST_VOID);
    return r;
}

static Val eval(Low* L, Expr* e) {
    if (!e) return vbad();
    switch (e->k) {
    case EX_INT: return vnum(e->num, ST_INT);
    case EX_FLOAT: return vnum(e->num, ST_FLOAT);
    case EX_BOOL: return vnum(e->num, ST_BOOL);
    case EX_STRING: {
        Val v = vnum(0, ST_STRING);
        v.str = e->s;
        return v;
    }
    case EX_NONE: {
        Val v = vnum(0, ST_ACTOR);
        v.none = true;
        return v;
    }
    case EX_SELF: {
        BpNode* n = add(L, BPK_SELF, e->line, e->col, NULL, NULL);
        return out_val(n, 0);
    }
    case EX_NAME: return eval_name(L, e);
    case EX_MEMBER: {
        if (!e->a || e->a->k == EX_SELF) { // self.x
            int fi = field_index(L, e->s);
            if (fi >= 0) return get_var(L, fi, e->line, e->col);
            Expr nm = *e;
            nm.k = EX_NAME;
            return eval_name(L, &nm);
        }
        Val o = eval(L, e->a);
        if (!o.ok) return vbad();
        uint8_t t = o.ty & 0x7F;
        char n[16];
        norm(e->s, n, sizeof n);
        if (t == ST_VECTOR || t == ST_ROTATOR || t == ST_COLOR) {
            static const char* const vn[] = { "x", "y", "z" }, *const rn[] = { "pitch", "yaw", "roll" }, *const cn[] = { "r", "g", "b", "a" };
            const char* const* names = t == ST_VECTOR ? vn : t == ST_ROTATOR ? rn : cn;
            int cnt = t == ST_COLOR ? 4 : 3;
            for (int k = 0; k < cnt; k++)
                if (!strcmp(n, names[k])) {
                    if (o.lit) return vnum(o.vec[k], ST_FLOAT);
                    BpNode* b = add(L, t == ST_VECTOR ? BPK_BREAK_VECTOR : t == ST_ROTATOR ? BPK_BREAK_ROTATOR : BPK_BREAK_COLOR, e->line, e->col, NULL, NULL);
                    if (b) feed(L, o, b, pin_find(b, false, false, 0), e->line, e->col);
                    return out_val(b, k);
                }
        }
        if (ST_IS_STRUCT(o.ty) && !(o.ty & ST_ARRAY)) { // hit.damage: Break <S>
            bool found;
            Val f = struct_get(L, o, e->s, e->line, e->col, &found);
            if (found) return f;
        }
        if ((o.ty & ST_ARRAY) && (!strcmp(n, "count") || !strcmp(n, "length") || !strcmp(n, "size"))) return call_node(L, BPK_ARR_LEN, &o, 1, e->line, e->col, NULL);
        if (t == ST_STRING && (!strcmp(n, "length") || !strcmp(n, "count"))) return call_node(L, BPK_STR_LEN, &o, 1, e->line, e->col, NULL);
        if (t == ST_VECTOR && (!strcmp(n, "length") || !strcmp(n, "magnitude"))) return call_node(L, BPK_VLEN, &o, 1, e->line, e->col, NULL);
        if (t == ST_VECTOR && (!strcmp(n, "normalized"))) return call_node(L, BPK_NORMALIZE, &o, 1, e->line, e->col, NULL);
        s_diag(L->c, e->line, e->col, true, "%s has no '%s'%s", tyname(o.ty), e->s, t == ST_ACTOR ? " (another actor's variables: call its functions or use interfaces)" : "");
        return vbad();
    }
    case EX_CALL: return eval_call(L, e);
    case EX_UNARY: {
        Val a = eval(L, e->a);
        if (!a.ok) return a;
        uint8_t t = a.ty & 0x7F;
        if (!strcmp(e->op, "not")) {
            if (a.lit) return vnum(a.num == 0, ST_BOOL);
            return call_node(L, BPK_NOT, &a, 1, e->line, e->col, NULL);
        }
        if (!strcmp(e->op, "+")) return a;
        if (!strcmp(e->op, "-")) {
            if (a.lit && numeric(t)) return vnum(-a.num, t == ST_BOOL ? ST_INT : t);
            if (a.lit && t == ST_VECTOR) return literal_vec(-a.vec[0], -a.vec[1], -a.vec[2], 0, ST_VECTOR);
            if (t == ST_VECTOR) return binop(L, "*", a, vnum(-1, ST_FLOAT), e->line, e->col);
            return binop(L, "-", vnum(0, t == ST_INT ? ST_INT : ST_FLOAT), a, e->line, e->col);
        }
        s_diag(L->c, e->line, e->col, true, "The operator '%s' isn't supported", e->op);
        return vbad();
    }
    case EX_BINARY: {
        Val a = eval(L, e->a);
        Val b = eval(L, e->b);
        return binop(L, e->op, a, b, e->line, e->col);
    }
    case EX_TERNARY: {
        Val c = eval(L, e->a);
        Val x = eval(L, e->b);
        Val y = eval(L, e->c);
        if (!c.ok || !x.ok || !y.ok) return vbad();
        if (c.lit) return c.num != 0 ? x : y;
        uint8_t t = x.ty & 0x7F;
        if (t == ST_FLOAT || t == ST_INT) {
            Val ab[3] = { c, x, y };
            return call_node(L, BPK_SELECT, ab, 3, e->line, e->col, NULL);
        }
        // others: Select (wildcard: Pick A, A, B)
        Val ab[3] = { c, x, y };
        return call_node(L, BPK_SELECTW, ab, 3, e->line, e->col, NULL);
    }
    case EX_INTERP: {
        Val acc = vnum(0, ST_STRING);
        acc.str = "";
        for (int k = 0; k < e->nargs; k++) acc = binop(L, "+", acc, eval(L, e->args[k]), e->line, e->col);
        return acc;
    }
    case EX_INDEX: {
        Val a[2] = { eval(L, e->a), eval(L, e->b) };
        if (!a[0].ok || !a[1].ok) return vbad();
        if (!(a[0].ty & ST_ARRAY)) {
            s_diag(L->c, e->line, e->col, true, "Only arrays take [index]");
            return vbad();
        }
        return call_node(L, BPK_ARR_GET, a, 2, e->line, e->col, NULL);
    }
    case EX_LIST: { // [1, 2, 3]: a hidden local array, cleared and filled each time this line runs
        if (!e->nargs) {
            s_diag(L->c, e->line, e->col, true, "An empty [] needs its type: declare the array (var a: int[] = [])");
            return vbad();
        }
        uint8_t elem = list_elem(L, e);
        char nm[32];
        snprintf(nm, sizeof nm, "lit%d_%d", e->line, e->col);
        int vi = new_local_var(L, nm, (uint8_t)(ST_ARRAY | elem), e->line, e->col);
        if (vi < 0) return vbad();
        list_into(L, vi, e);
        return get_var(L, vi, e->line, e->col);
    }
    default:
        s_diag(L->c, e->line, e->col, true, "This expression isn't supported");
        return vbad();
    }
}

// --- Statements ---
static void lower_block(Low* L, Stmt** b, int n);

// A structure's value from its fields in order (Hit(5, 2.0), C's { 5, 2.0f }): Make <S>, the rest their defaults
static Val make_struct(Low* L, const char* nm, Expr** args, int nargs, int line, int col) {
    BpNode* mk = add(L, BPK_MAKE_STRUCT, line, col, nm, NULL);
    if (!mk) return vbad();
    for (int k = 0; k < nargs; k++) {
        int pi = pin_find(mk, false, false, k);
        if (pi < 0) {
            s_diag(L->c, line, col, true, "%s has %d fields", nm, k);
            break;
        }
        feed(L, eval(L, args[k]), mk, pi, args[k]->line, args[k]->col);
    }
    return out_val(mk, 0);
}

static int new_local_var(Low* L, const char* name, uint8_t ty, int line, int col) {
    char vn[BP_NAME];
    snprintf(vn, sizeof vn, "%.18s_%.20s", L->m ? L->m->name : "init", name); // (per method: a function's and an event's apart)
    int vi = bp_var_index(L->a, vn);
    if (vi < 0) vi = bp_add_var(L->a, vn, bpt_of(ty)), var_ref(vi >= 0 ? &L->a->vars[vi] : NULL, ty);
    if (vi < 0) s_diag(L->c, line, col, true, "Too many variables in one class (64 at most, fields and locals)");
    else if (ty & ST_ARRAY) L->a->vars[vi].cont = BPC_ARRAY;
    return vi;
}

static void assign(Low* L, Expr* target, Val v, int line, int col) {
    if (!target) return;
    if (target->k == EX_NAME || (target->k == EX_MEMBER && (!target->a || target->a->k == EX_SELF))) {
        Local* lc = target->k == EX_NAME ? local_find(L, target->s) : NULL;
        if (lc) {
            if (lc->readonly || lc->var < 0) {
                s_diag(L->c, line, col, true, "'%s' can't be changed here (a loop's variable or a parameter: copy it to a local)", lc->name);
                return;
            }
            set_var(L, lc->var, v, line, col);
            return;
        }
        int fi = field_index(L, target->s);
        if (fi >= 0) {
            set_var(L, fi, v, line, col);
            return;
        }
        if (target->k == EX_NAME && (L->c->lang == SL_PYTHON || L->c->lang == SL_REAL)) { // (a first assignment: a new local)
            uint8_t ty = v.ty & 0x7F ? v.ty : ST_FLOAT;
            int vi = new_local_var(L, target->s, ty, line, col);
            if (vi >= 0 && L->nloc < 96) L->loc[L->nloc++] = (Local){ target->s, ty, vi, 0, 0, L->depth, false };
            s_sym(L->c, SYM_LOCAL, target->s, tyname(ty), line, col, line, L->m ? L->m->end_line : 0);
            set_var(L, vi, v, line, col);
            return;
        }
        s_diag(L->c, line, col, true, "'%s' isn't declared here", target->s);
        return;
    }
    if (target->k == EX_MEMBER) { // v.x = 3: the vector made again
        Expr* base = target->a;
        Val o = eval(L, base);
        uint8_t t = o.ty & 0x7F;
        if (o.ok && ST_IS_STRUCT(o.ty) && !(o.ty & ST_ARRAY)) { // hit.damage = 3: the structure made again with it
            bool found;
            Val nv = struct_with(L, o, target->s, v, line, col, &found);
            if (!found) {
                s_diag(L->c, line, col, true, "%s has no '%s'", tyname(o.ty), target->s);
                return;
            }
            assign(L, base, nv, line, col);
            return;
        }
        if (o.ok && (t == ST_VECTOR || t == ST_ROTATOR)) {
            static const char* const vn[] = { "x", "y", "z" }, *const rn[] = { "pitch", "yaw", "roll" };
            char n[16];
            norm(target->s, n, sizeof n);
            Val parts[3];
            BpNode* b = add(L, t == ST_VECTOR ? BPK_BREAK_VECTOR : BPK_BREAK_ROTATOR, line, col, NULL, NULL);
            if (b) feed(L, o, b, pin_find(b, false, false, 0), line, col);
            int hit = -1;
            for (int k = 0; k < 3; k++) {
                parts[k] = out_val(b, k);
                if (!strcmp(n, (t == ST_VECTOR ? vn : rn)[k])) hit = k;
            }
            if (hit < 0) {
                s_diag(L->c, line, col, true, "%s has no '%s'", tyname(o.ty), target->s);
                return;
            }
            parts[hit] = v;
            Val nv = call_node(L, t == ST_VECTOR ? BPK_MAKE_VECTOR : BPK_MAKE_ROTATOR, parts, 3, line, col, NULL);
            assign(L, base, nv, line, col);
            return;
        }
        if (o.ok) s_diag(L->c, line, col, true, "Only this class's variables (and a vector's x / y / z) can be set this way");
        return;
    }
    if (target->k == EX_INDEX) { // arr[i] = v
        Val a[4] = { eval(L, target->a), eval(L, target->b), v, vnum(1, ST_BOOL) };
        if (!a[0].ok || !a[1].ok) return;
        call_node(L, BPK_ARR_SET, a, 4, line, col, NULL);
        return;
    }
    s_diag(L->c, line, col, true, "This can't be assigned");
}

static void lower_stmt(Low* L, Stmt* s) {
    if (!s) return;
    L->y = 160.0f * (float)L->depth;
    switch (s->k) {
    case SS_PASS: return;
    case SS_BLOCK:
        L->depth++;
        lower_block(L, s->body, s->nbody);
        L->depth--;
        return;
    case SS_EXPR: {
        int before = L->impure;
        Val v = eval(L, s->e);
        if (v.ok && L->impure == before) s_diag(L->c, s->line, s->col, false, "This line computes a value and does nothing with it");
        return;
    }
    case SS_VAR: {
        if (s->e && s->e->k == EX_LIST && ST_IS_STRUCT(s->type) && !(s->type & ST_ARRAY)) { // C's Hit h = { 5, 2.0f }
            int vi = new_local_var(L, s->name, s->type, s->line, s->col);
            if (vi < 0) return;
            if (L->nloc < 96) L->loc[L->nloc++] = (Local){ s->name, s->type, vi, 0, 0, L->depth, false };
            set_var(L, vi, make_struct(L, s_ty_name(L->c, s->type), s->e->args, s->e->nargs, s->line, s->col), s->line, s->col);
            return;
        }
        if (s->e && s->e->k == EX_LIST) { // var a = [1, 2] / var a: float[] = []: the items added to the array itself
            uint8_t ty = (s->type & ST_ARRAY) ? s->type : (uint8_t)(ST_ARRAY | (s->e->nargs ? list_elem(L, s->e) : ST_FLOAT));
            if (!s->e->nargs && !(s->type & ST_ARRAY)) s_diag(L->c, s->line, s->col, true, "An empty [] needs its type: var %s: int[] = []", s->name);
            int vi = new_local_var(L, s->name, ty, s->line, s->col);
            if (vi < 0) return;
            if (L->nloc < 96) L->loc[L->nloc++] = (Local){ s->name, ty, vi, 0, 0, L->depth, false };
            list_into(L, vi, s->e);
            return;
        }
        Val v = s->e ? eval(L, s->e) : vnum(0, s->type ? s->type : ST_FLOAT);
        uint8_t ty = s->type ? s->type : (v.ok ? v.ty : ST_FLOAT);
        if ((ty & 0x7F) == ST_UNKNOWN || (ty & 0x7F) == ST_VOID) {
            if (v.ok && (v.ty & 0x7F) == ST_VOID) s_diag(L->c, s->line, s->col, true, "This gives no value");
            ty = ST_FLOAT;
        }
        int vi = new_local_var(L, s->name, ty, s->line, s->col);
        if (vi < 0) return;
        if (L->nloc < 96) L->loc[L->nloc++] = (Local){ s->name, ty, vi, 0, 0, L->depth, false };
        if (s->e || (!(ty & ST_ARRAY) && !ST_IS_STRUCT(ty))) set_var(L, vi, v, s->line, s->col); // (declared without a value: its default, set each run)
        return;
    }
    case SS_ASSIGN: {
        if (s->e && s->e->k == EX_LIST && !strcmp(s->op, "=") && s->target && s->target->k == EX_NAME) {
            Local* lc = local_find(L, s->target->s);
            int vi = lc && !lc->readonly ? lc->var : !lc ? field_index(L, s->target->s) : -1;
            if (vi >= 0 && (L->a->vars[vi].cont == BPC_ARRAY)) {
                list_into(L, vi, s->e);
                return;
            }
        }
        Val v = eval(L, s->e);
        if (strcmp(s->op, "=")) { // x += v: x = x + v
            char op[2] = { s->op[0], 0 };
            Val cur = eval(L, s->target);
            v = binop(L, op, cur, v, s->line, s->col);
        }
        assign(L, s->target, v, s->line, s->col);
        return;
    }
    case SS_IF: {
        Val c = eval(L, s->e);
        if (!c.ok) return;
        if (c.lit) { // (a constant condition: one branch)
            L->depth++;
            if (c.num != 0) lower_block(L, s->body, s->nbody);
            else lower_block(L, s->els, s->nels);
            L->depth--;
            return;
        }
        BpNode* b = add(L, BPK_BRANCH, s->line, s->col, NULL, NULL);
        if (!b) return;
        feed(L, c, b, pin_find(b, false, false, 0), s->line, s->col);
        chain(L, b);
        wait_on(L, b, pin_find(b, true, true, 0));
        L->depth++;
        lower_block(L, s->body, s->nbody);
        Pend then_p[64];
        int nthen = L->npend;
        memcpy(then_p, L->pend, sizeof(Pend) * (size_t)nthen);
        wait_on(L, b, pin_find(b, true, true, 1));
        lower_block(L, s->els, s->nels);
        L->depth--;
        for (int i = 0; i < nthen && L->npend < 64; i++) L->pend[L->npend++] = then_p[i];
        return;
    }
    case SS_WHILE: {
        bool has_break = false;
        for (int i = 0; i < s->nbody; i++) has_break |= s->body[i] && s->body[i]->k == SS_BREAK; // (a direct break; nested ones: flagged too)
        int flag = -1;
        char fn[24];
        snprintf(fn, sizeof fn, "break_%d", L->nflags++);
        flag = new_local_var(L, fn, ST_BOOL, s->line, s->col);
        if (flag >= 0) set_var(L, flag, vnum(0, ST_BOOL), s->line, s->col);
        int before = L->impure;
        BpNode* w = NULL;
        Val c = eval(L, s->e);
        if (L->impure != before)
            s_diag(L->c, s->line, s->col, true, "A loop's condition can't call functions with side effects (it's read every turn): read them into a variable in the loop");
        if (flag >= 0) {
            Val f = get_var(L, flag, s->line, s->col);
            Val nf = call_node(L, BPK_NOT, &f, 1, s->line, s->col, NULL);
            c = binop(L, "and", c, nf, s->line, s->col);
        }
        w = add(L, BPK_WHILE, s->line, s->col, NULL, NULL);
        if (!w) return;
        feed(L, c, w, pin_find(w, false, false, 0), s->line, s->col);
        chain(L, w);
        if (L->nloops < 16) L->loops[L->nloops++] = (Loop){ 0, w->id, 0, flag };
        wait_on(L, w, pin_named(w, true, "Loop Body"));
        L->depth++;
        lower_block(L, s->body, s->nbody);
        L->depth--;
        L->nloops--;
        wait_on(L, w, pin_named(w, true, "Completed"));
        (void)has_break;
        return;
    }
    case SS_FOR_RANGE:
    case SS_FOR_EACH: {
        bool each = s->k == SS_FOR_EACH;
        Val lo = eval(L, s->lo), hi = each ? vbad() : eval(L, s->hi);
        if (!lo.ok || (!each && !hi.ok)) return;
        if (!each && s->step) { // a step: a while loop with its own counter
            Val st = eval(L, s->step);
            int vi = new_local_var(L, s->name, ST_INT, s->line, s->col);
            if (vi < 0) return;
            set_var(L, vi, lo, s->line, s->col);
            if (L->nloc < 96) L->loc[L->nloc++] = (Local){ s->name, ST_INT, vi, 0, 0, L->depth, false };
            Val c = binop(L, s->inclusive ? "<=" : "<", get_var(L, vi, s->line, s->col), hi, s->line, s->col);
            BpNode* w = add(L, BPK_WHILE, s->line, s->col, NULL, NULL);
            if (!w) return;
            feed(L, c, w, pin_find(w, false, false, 0), s->line, s->col);
            chain(L, w);
            if (L->nloops < 16) L->loops[L->nloops++] = (Loop){ 0, w->id, 0, -1 };
            wait_on(L, w, pin_named(w, true, "Loop Body"));
            L->depth++;
            lower_block(L, s->body, s->nbody);
            set_var(L, vi, binop(L, "+", get_var(L, vi, s->line, s->col), st, s->line, s->col), s->line, s->col);
            L->depth--;
            L->nloops--;
            wait_on(L, w, pin_named(w, true, "Completed"));
            return;
        }
        BpNode* f = add(L, each ? BPK_FOREACH_BREAK : BPK_FOR_LOOP_BREAK, s->line, s->col, NULL, NULL);
        if (!f) return;
        if (each) {
            if (!(lo.ty & ST_ARRAY)) s_diag(L->c, s->line, s->col, true, "for ... in needs an array or a range (a..b, range(n))");
            feed(L, lo, f, pin_named(f, false, "Array"), s->line, s->col);
        } else {
            feed(L, lo, f, pin_named(f, false, "First Index"), s->line, s->col);
            Val last = s->inclusive ? hi : binop(L, "-", hi, vnum(1, ST_INT), s->line, s->col); // (exclusive: Last = hi - 1)
            feed(L, last, f, pin_named(f, false, "Last Index"), s->line, s->col);
        }
        chain(L, f);
        int brk = pin_named(f, false, "Break");
        if (L->nloops < 16) L->loops[L->nloops++] = (Loop){ 1, f->id, (uint8_t)(brk < 0 ? 0 : brk), -1 };
        int ip = each ? pin_named(f, true, "Array Element") : pin_named(f, true, "Index");
        if (L->nloc < 96 && ip >= 0)
            L->loc[L->nloc++] = (Local){ s->name, each ? (uint8_t)(lo.ty & 0x7F) : ST_INT, -1, f->id, (uint8_t)ip, L->depth, true };
        if (each && L->nloc < 96) { // (its element's type: the array's)
            Local* el = &L->loc[L->nloc - 1];
            el->ty = sty_of_pin(&f->pins[ip]) ? sty_of_pin(&f->pins[ip]) : (uint8_t)(lo.ty & 0x7F);
        }
        int nloc0 = L->nloc;
        wait_on(L, f, pin_named(f, true, "Loop Body"));
        L->depth++;
        lower_block(L, s->body, s->nbody);
        L->depth--;
        L->nloc = nloc0 - 1; // (the loop's variable leaves its scope)
        L->nloops--;
        wait_on(L, f, pin_named(f, true, "Completed"));
        return;
    }
    case SS_FOR_C: { // init; while (cond) { body; post }
        lower_stmt(L, s->init);
        for (int i = 0; i < s->nbody; i++)
            if (s->body[i] && s->body[i]->k == SS_CONTINUE)
                s_diag(L->c, s->body[i]->line, s->body[i]->col, true, "continue in this for loop would skip its step: use a for over a range");
        Stmt w = *s;
        w.k = SS_WHILE;
        Stmt** body = sa_alloc(&L->c->arena, sizeof(Stmt*) * (size_t)(s->nbody + 1));
        if (!body) return;
        memcpy(body, s->body, sizeof(Stmt*) * (size_t)s->nbody);
        body[s->nbody] = s->post;
        w.body = body, w.nbody = s->nbody + (s->post ? 1 : 0);
        if (!w.e) w.e = &(Expr){ .k = EX_BOOL, .num = 1, .line = s->line, .col = s->col };
        lower_stmt(L, &w);
        return;
    }
    case SS_BREAK: {
        if (!L->nloops) {
            s_diag(L->c, s->line, s->col, true, "break outside a loop");
            return;
        }
        Loop* lp = &L->loops[L->nloops - 1];
        if (lp->kind == 1) { // For with Break: its Break input
            for (int i = 0; i < L->npend; i++) bp_link(L->g, L->pend[i].node, L->pend[i].pin, lp->node, lp->brk_pin);
        } else if (lp->flag >= 0) set_var(L, lp->flag, vnum(1, ST_BOOL), s->line, s->col);
        else s_diag(L->c, s->line, s->col, true, "break in a loop with a step isn't supported: use while");
        L->npend = 0; // (nothing after it runs)
        return;
    }
    case SS_CONTINUE:
        if (!L->nloops) s_diag(L->c, s->line, s->col, true, "continue outside a loop");
        L->npend = 0; // (the turn ends here: the loop goes on)
        return;
    case SS_RETURN: {
        if (L->ret_graph < 0) { // an event: it ends here
            if (s->e) s_diag(L->c, s->line, s->col, true, "An event returns no value");
            L->npend = 0;
            return;
        }
        BpNode* r = add(L, BPK_FUNC_RETURN, s->line, s->col, L->g->name, NULL);
        if (!r) return;
        if (s->e) {
            Val v = eval(L, s->e);
            int pi = pin_find(r, false, false, 0);
            if (pi < 0) s_diag(L->c, s->line, s->col, true, "'%s' returns no value", L->m->name);
            else feed(L, v, r, pi, s->line, s->col);
        } else if (pin_find(r, false, false, 0) >= 0) s_diag(L->c, s->line, s->col, true, "'%s' must return a %s", L->m->name, tyname(L->m->ret));
        chain(L, r);
        L->npend = 0;
        return;
    }
    default: return;
    }
}

static void lower_block(Low* L, Stmt** b, int n) {
    int nloc = L->nloc;
    for (int i = 0; i < n; i++) {
        if (!L->npend && b[i] && b[i]->k != SS_PASS) { // (after a return / break: unreachable)
            s_diag(L->c, b[i]->line, b[i]->col, false, "This is never reached");
            break;
        }
        lower_stmt(L, b[i]);
    }
    L->nloc = nloc; // (the block's locals leave their scope)
}

// The type a method's return statements give (a function without a declared type: inferred from its first return)
static uint8_t infer_return(Low* L, Stmt** b, int n) {
    for (int i = 0; i < n; i++) {
        Stmt* s = b[i];
        if (!s) continue;
        if (s->k == SS_RETURN && s->e) {
            Expr* e = s->e;
            switch (e->k) {
            case EX_INT: return ST_INT;
            case EX_FLOAT: return ST_FLOAT;
            case EX_BOOL: return ST_BOOL;
            case EX_STRING: case EX_INTERP: return ST_STRING;
            case EX_BINARY:
                if (!strcmp(e->op, "==") || !strcmp(e->op, "!=") || !strcmp(e->op, "<") || !strcmp(e->op, ">") || !strcmp(e->op, "<=") ||
                    !strcmp(e->op, ">=") || !strcmp(e->op, "and") || !strcmp(e->op, "or"))
                    return ST_BOOL;
                return ST_FLOAT;
            case EX_NAME: {
                int fi = field_index(L, e->s);
                if (fi >= 0) return L->cls->fields[0].type ? L->cls->fields[0].type : ST_FLOAT;
                return ST_FLOAT;
            }
            default: return ST_FLOAT;
            }
        }
        uint8_t r = infer_return(L, s->body, s->nbody);
        if (r != ST_VOID) return r;
        r = infer_return(L, s->els, s->nels);
        if (r != ST_VOID) return r;
    }
    return ST_VOID;
}

bool s_lower(SCtx* c, SClass* cls, struct BpAsset* a) {
    Low* L = calloc(1, sizeof *L); // (a compile's state)
    if (!L) return false;
    L->c = c, L->cls = cls, L->a = a;
    g_sc = c;
    uint32_t errors0 = *c->ndiags;
    // the fields: the class's variables, their literal defaults
    for (int i = 0; i < cls->nfields; i++) {
        SField* f = &cls->fields[i];
        L->g = &a->graphs[0];
        Val v = f->init ? (f->init->k == EX_CALL || f->init->k == EX_UNARY || f->init->k == EX_INT || f->init->k == EX_FLOAT ||
                           f->init->k == EX_BOOL || f->init->k == EX_STRING ? eval(L, f->init) : vnum(0, ST_UNKNOWN))
                         : vnum(0, f->type ? f->type : ST_FLOAT);
        uint8_t ty = f->type ? f->type : (v.ok && (v.ty & 0x7F) != ST_UNKNOWN && (v.ty & 0x7F) != ST_VOID ? v.ty : ST_FLOAT);
        f->type = ty;
        int vi = bp_add_var(a, f->name, bpt_of(ty));
        if (vi < 0) {
            s_diag(c, f->line, f->col, true, "Too many variables in one class (64 at most)");
            continue;
        }
        BpVar* bv = &a->vars[vi];
        var_ref(bv, ty);
        bv->editable = f->editable;
        bv->save_game = f->save_game; // (@savegame, [SaveGame]: save_progress.h)
        if (f->asset_ref) bv->type = BPT_NAME, snprintf(bv->ref, sizeof bv->ref, "%s", f->asset_ref); // (2026-11-24: Level, Sound...: a typed asset reference)
        if (ty & ST_ARRAY) bv->cont = BPC_ARRAY;
        if (v.ok && v.lit && !v.none) {
            if ((v.ty & 0x7F) == ST_STRING) snprintf(bv->sdef, sizeof bv->sdef, "%s", v.str ? v.str : "");
            else if ((v.ty & 0x7F) == ST_VECTOR || (v.ty & 0x7F) == ST_ROTATOR || (v.ty & 0x7F) == ST_COLOR) memcpy(bv->def.v, v.vec, sizeof bv->def.v);
            else bv->def.v[0] = (float)v.num;
            f->init = NULL; // (its default: nothing to run)
        }
    }
    // the graphs first (the calls between the methods find them): functions, then the events
    int func_graph[S_MAX_METHODS];
    for (int i = 0; i < cls->nmethods; i++) {
        SMethod* m = &cls->methods[i];
        func_graph[i] = -1;
        if (engine_event(m->name) || !is_function(m)) continue;
        if (m->ret == ST_UNKNOWN) m->ret = infer_return(L, m->body, m->nbody);
        BpGraph* g = bp_add_graph(a, m->name, BPG_FUNCTION);
        if (!g) {
            s_diag(c, m->line, m->col, true, "Too many functions in one class (15 at most)");
            continue;
        }
        for (int k = 0; k < m->nparams; k++) {
            uint8_t pt = m->params[k].type ? m->params[k].type : ST_FLOAT;
            m->params[k].type = pt;
            snprintf(g->params[k].name, BP_NAME, "%s", m->params[k].name);
            g->params[k].type = bpt_of(pt);
            var_ref(&g->params[k], pt);
            if (pt & ST_ARRAY) g->params[k].cont = BPC_ARRAY;
        }
        g->nparams = (uint8_t)m->nparams;
        if (m->ret != ST_VOID) {
            g->nresults = 1;
            snprintf(g->results[0].name, BP_NAME, "Return Value");
            g->results[0].type = bpt_of(m->ret);
            var_ref(&g->results[0], m->ret);
        }
        for (uint32_t k = 0; k < g->nnodes; k++) bp_refresh_pins(a, g, &g->nodes[k]);
        func_graph[i] = (int)(g - a->graphs);
    }
    // the initializers that aren't constants: at BeginPlay's start
    SMethod* begin = NULL;
    for (int i = 0; i < cls->nmethods; i++)
        if (engine_event(cls->methods[i].name) == EV_BEGIN && strcmp(cls->methods[i].name, "__ctor__")) begin = &cls->methods[i];
    bool need_init = false;
    for (int i = 0; i < cls->nfields; i++) need_init |= cls->fields[i].init != NULL;
    SMethod* ctor = NULL;
    for (int i = 0; i < cls->nmethods; i++)
        if (!strcmp(cls->methods[i].name, "__ctor__")) ctor = &cls->methods[i];
    uint32_t begin_node = 0;
    if (begin || need_init || ctor) {
        L->g = &a->graphs[0];
        L->x = 0, L->y = 0;
        BpNode* ev = add(L, BPK_EVENT_BEGIN_PLAY, begin ? begin->line : 1, 1, NULL, NULL);
        begin_node = ev ? ev->id : 0;
        wait_on(L, ev, ev ? pin_find(ev, true, true, 0) : -1);
        L->m = ctor, L->ret_graph = -1;
        for (int i = 0; i < cls->nfields; i++)
            if (cls->fields[i].init) {
                Val v = eval(L, cls->fields[i].init);
                int vi = bp_var_index(a, cls->fields[i].name);
                set_var(L, vi, v, cls->fields[i].line, cls->fields[i].col);
            }
        if (ctor) lower_block(L, ctor->body, ctor->nbody);
    }
    Pend begin_pend[64];
    int nbegin = L->npend;
    memcpy(begin_pend, L->pend, sizeof(Pend) * (size_t)L->npend);
    // each method's body
    for (int i = 0; i < cls->nmethods; i++) {
        SMethod* m = &cls->methods[i];
        if (!strcmp(m->name, "__ctor__")) continue;
        L->m = m, L->nloc = 0, L->depth = 0, L->nloops = 0, L->x = 0, L->y = (float)(i + 1) * 600.0f;
        int ev = engine_event(m->name);
        if (func_graph[i] >= 0) {
            L->g = &a->graphs[func_graph[i]];
            L->ret_graph = func_graph[i];
            BpNode* entry = &L->g->nodes[0];
            L->entry = entry->id;
            wait_on(L, entry, pin_find(entry, true, true, 0));
            for (int k = 0; k < m->nparams && k < S_MAX_PARAMS; k++) {
                int pi = pin_find(entry, true, false, k);
                if (pi >= 0 && L->nloc < 96) L->loc[L->nloc++] = (Local){ m->params[k].name, m->params[k].type, -1, entry->id, (uint8_t)pi, 0, true };
            }
            lower_block(L, m->body, m->nbody);
            if (L->npend) { // (the end reached: the default Return node)
                BpNode* ret = NULL;
                for (uint32_t k = 0; k < L->g->nnodes; k++)
                    if (L->g->nodes[k].kind == BPK_FUNC_RETURN) ret = &L->g->nodes[k];
                if (m->ret != ST_VOID) s_diag(c, m->end_line, 1, true, "'%s' must return a %s on every path", m->name, tyname(m->ret));
                if (ret) chain(L, ret);
            }
            continue;
        }
        L->g = &a->graphs[0];
        L->ret_graph = -1;
        if (ev == EV_BEGIN) { // (after the initializers)
            L->npend = nbegin;
            memcpy(L->pend, begin_pend, sizeof(Pend) * (size_t)nbegin);
            if (!begin_node) continue;
        } else {
            uint16_t kind = ev == EV_TICK ? BPK_EVENT_TICK : ev == EV_END ? BPK_EVENT_END_PLAY : ev == EV_OVERLAP_BEGIN ? BPK_EVENT_BEGIN_OVERLAP
                          : ev == EV_OVERLAP_END ? BPK_EVENT_END_OVERLAP : ev == EV_DAMAGED ? BPK_EVENT_ANY_DAMAGE : ev == EV_HEALED ? BPK_EVENT_HEALED
                          : ev == EV_DEATH ? BPK_EVENT_DEATH : ev == EV_SHIELD_BROKEN ? BPK_EVENT_SHIELD_BROKEN 
                          : ev == EV_INV_CHANGED ? BPK_EVENT_INV_CHANGED
                          : ev == EV_ITEM_USED ? BPK_EVENT_ITEM_USED : ev == EV_ITEM_EQUIPPED ? BPK_EVENT_ITEM_EQUIPPED
                          : ev == EV_ITEM_UNEQUIPPED ? BPK_EVENT_ITEM_UNEQUIPPED : ev == EV_GAME_SAVED ? BPK_EVENT_GAME_SAVED
                          : ev == EV_GAME_LOADED ? BPK_EVENT_GAME_LOADED : ev == EV_CHECKPOINT ? BPK_EVENT_CHECKPOINT 
                          : ev == EV_IA_INTERACT ? BPK_EVENT_IA_INTERACT
                          : ev == EV_IA_FOCUS_BEGIN ? BPK_EVENT_IA_FOCUS_BEGIN : ev == EV_IA_FOCUS_END ? BPK_EVENT_IA_FOCUS_END
                          : ev == EV_IA_HOLD ? BPK_EVENT_IA_HOLD 
                          : ev == EV_DLG_LINE ? BPK_EVENT_DLG_LINE
                          : ev == EV_DLG_CHOICE ? BPK_EVENT_DLG_CHOICE : ev == EV_DLG_END ? BPK_EVENT_DLG_END
                          : ev == EV_QST_STARTED ? BPK_EVENT_QST_STARTED : ev == EV_QST_OBJ_UPDATED ? BPK_EVENT_QST_OBJ_UPDATED
                          : ev == EV_QST_OBJ_COMPLETED ? BPK_EVENT_QST_OBJ_COMPLETED : ev == EV_QST_COMPLETED ? BPK_EVENT_QST_COMPLETED
                          : ev == EV_QST_FAILED ? BPK_EVENT_QST_FAILED
                          : ev == EV_GR_PHASE ? BPK_EVENT_MATCH_PHASE : ev == EV_GR_SCORE ? BPK_EVENT_SCORE_CHANGED
                          : ev == EV_GR_WON ? BPK_EVENT_MATCH_WON : ev == EV_GR_LOST ? BPK_EVENT_MATCH_LOST
                          : ev == EV_GR_ROUND_START ? BPK_EVENT_ROUND_STARTED : ev == EV_GR_ROUND_END ? BPK_EVENT_ROUND_ENDED
                          : ev == EV_GR_RESPAWNED ? BPK_EVENT_PLAYER_RESPAWNED : ev == EV_GR_TIMER ? BPK_EVENT_TIMER_EXPIRED
                          : ev == EV_ABIL_ACTIVATE ? BPK_EVENT_ABIL_ACTIVATE : ev == EV_ABIL_ACTIVATED ? BPK_EVENT_ABIL_ACTIVATED
                          : ev == EV_ABIL_ENDED ? BPK_EVENT_ABIL_ENDED : ev == EV_GE_APPLIED ? BPK_EVENT_GE_APPLIED
                          : ev == EV_GE_REMOVED ? BPK_EVENT_GE_REMOVED : ev == EV_ATTR_CHANGED ? BPK_EVENT_ATTR_CHANGED : BPK_EVENT_CUSTOM;
            BpNode* n = add(L, kind, m->line, m->col, kind == BPK_EVENT_CUSTOM ? m->name : NULL, NULL);
            if (!n) continue;
            wait_on(L, n, pin_find(n, true, true, 0));
            if (m->nparams && kind == BPK_EVENT_CUSTOM) s_diag(c, m->line, m->col, true, "Custom Events take no parameters");
            for (int k = 0; k < m->nparams; k++) { // (Tick's Delta Seconds, an overlap's Other Actor)
                int pi = pin_find(n, true, false, k);
                if (pi < 0) {
                    s_diag(c, m->params[k].line, m->params[k].col, true, "%s gives %d value%s", m->name, k, k == 1 ? "" : "s");
                    break;
                }
                if (L->nloc < 96) L->loc[L->nloc++] = (Local){ m->params[k].name, sty_of_pin(&n->pins[pi]), -1, n->id, (uint8_t)pi, 0, true };
            }
        }
        lower_block(L, m->body, m->nbody);
    }
    c->syms = c->syms; // (the locals found while lowering: added to the symbols)
    bool ok = true;
    for (uint32_t i = errors0; i < *c->ndiags; i++) ok &= !c->diags[i].error;
    free(L);
    return ok;
}

// The script's structures as Blueprint structures (generated: the script is their file), in the registry before the
// class is lowered (Make / Break read their fields). One of the project's by the same name: an error. Changed: every
// class's pins made again (bp_refresh_all).
bool s_make_structs(SCtx* c, SClass* cls, const char* vpath, const char* disk) {
    g_sc = c;
    bool ok = true, changed = false;
    for (int i = 0; i < cls->nstructs; i++) {
        SStruct* s = &cls->structs[i];
        if (!s->name) continue;
        BpAsset* old = bp_find_name(BPA_STRUCT, s->name);
        char sv[700];
        snprintf(sv, sizeof sv, "%s#struct:%s", vpath, s->name);
        uint32_t hash = bp_vpath_hash(sv);
        if (old && old->hash != hash) {
            s_diag(c, s->line, s->col, true, "A structure named '%s' already exists in the project (%s)", s->name, old->path);
            ok = false;
            continue;
        }
        BpAsset* a = calloc(1, sizeof *a);
        if (!a) return false;
        a->kind = BPA_STRUCT;
        a->generated = true;
        a->hash = hash;
        snprintf(a->name, sizeof a->name, "%s", s->name);
        snprintf(a->path, sizeof a->path, "%s", disk);
        for (int k = 0; k < s->nfields && a->nfields < BP_MAX_FIELDS; k++) {
            SField* f = &s->fields[k];
            uint8_t ty = f->type ? f->type : ST_FLOAT;
            if (ty & ST_ARRAY) {
                s_diag(c, f->line, f->col, true, "A structure's field can't be an array yet");
                ok = false;
                continue;
            }
            if (ST_IS_STRUCT(ty) && !strcmp(s_ty_name(c, ty), s->name)) {
                s_diag(c, f->line, f->col, true, "A structure can't hold itself");
                ok = false;
                continue;
            }
            BpVar* v = &a->fields[a->nfields++];
            snprintf(v->name, sizeof v->name, "%s", f->name);
            v->type = bpt_of(ty);
            var_ref(v, ty);
            Expr* in = f->init;
            if (in && (in->k == EX_INT || in->k == EX_FLOAT || in->k == EX_BOOL)) v->def.v[0] = (float)in->num;
            else if (in && in->k == EX_STRING && in->s) snprintf(v->sdef, sizeof v->sdef, "%s", in->s);
            else if (in && in->k == EX_UNARY && !strcmp(in->op, "-") && in->a && (in->a->k == EX_INT || in->a->k == EX_FLOAT)) v->def.v[0] = (float)-in->a->num;
            else if (in) s_diag(c, f->line, f->col, false, "Only a literal is a field's default here (the rest: zero)");
            s_sym(c, SYM_FIELD, f->name, s_ty_name(c, ty), f->line, f->col, 0, 0);
        }
        bool same_layout = old && old->nfields == a->nfields;
        for (uint32_t k = 0; same_layout && k < a->nfields; k++)
            same_layout = !strcmp(old->fields[k].name, a->fields[k].name) && old->fields[k].type == a->fields[k].type &&
                          !strcmp(old->fields[k].ref, a->fields[k].ref);
        changed |= old && !same_layout;
        bp_adopt(a);
    }
    if (changed) bp_refresh_all(); // (the Blueprints using one: their Make / Break pins follow)
    return ok;
}

