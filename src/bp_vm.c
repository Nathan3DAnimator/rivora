// Blueprints: the compiler (graphs -> register bytecode), the VM and the runtime (instances, events, latent actions,
// the Character's movement and camera). Nothing is allocated while a Blueprint runs: an instance's registers come from
// an arena reserved when play starts (notas-dev/blueprints.md).
#include "crowd.h" // (Q27.1) Crowd Alarm
#include "weather_field.h" // (Q27.6) Add Storm, Get Rain At
#include "city.h"          // (Q27.10) the city's clock and events
#include "sublevels.h"
// (2026-11-25, item 13) an enum pin's value (its entry's index) as the name the engine's own functions take
#define ENUM_NAME(e, X) bp_builtin_enum_entry((e), (int)(X)->v[0])
#include "loc.h" // (2026-11-24) the game's localization
#include "level_preload.h" // (2026-11-23) Preload Level, the load speeds
#include "bp_timeline.h"
#include "audio.h"
#include "world_partition.h"
#include "audio_internal.h"
#include "audio_world.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "groom.h"
#include "plugins.h"
#include "gameplay_tags.h"
#include "bp_debug.h"
#include "bp_fiber.h"
#include "health.h"
#include "game_settings.h"
#include "bp_jit.h"
#include "ui_widget.h"
#include "terror.h"
#include "spline.h" // (Q14) splines and cables
#include "pcg.h"
#include "runtime_lighting.h" // (Q16)
#include "props.h"
#include "cvars.h"
#include "sequencer.h"
#include "demo_play.h"
#include "online.h"
#include "steam.h"
#include "landscape.h"
#include "scene_query.h" // (2026-11-19) the traces: Line / Sphere / Box / Capsule Trace
#include "debug_draw.h"
#include "components.h"
#include "mem.h"
#include "platform.h"
#include "material.h"
#include "physics.h"
#include "physmat.h"
#include "toon_actor.h" // (Q11)
#include "post.h"
#include "anim.h"
#include "particles.h"
#include "decal.h"
#include "ikrig.h"
#include "vehicle.h"
#include "cloth.h"
#include "xr.h"
#include "vr_pawn.h"
#include "xr_mr.h" // (Q25b) mixed reality
#include "ar_arcore.h"
#include "water.h"
#include "ai.h"
#include "input.h"
#include "project.h"
#include "render.h"
#include "inventory.h"
#include "save_progress.h"
#include "hud.h"
#include "interaction.h"
#include "fpcam.h"
#include "camshake.h"
#include "dialogue.h"
#include "quest.h"
#include "game_rules.h"
#include "ability.h"

#define DEG2RAD 0.017453292519943295f

static BpEditorHooks g_ed_hooks; // (the editor's selection for the Editor Utilities' nodes)
static inline Entity ent_of(const BpValue* v) {
    Entity e;
    memcpy(&e, &v->v[0], sizeof e);
    return e;
}
static inline void set_ent(BpValue* v, Entity e) {
    memset(v, 0, sizeof *v);
    memcpy(&v->v[0], &e, sizeof e);
}

// --- Compiler ---

typedef struct GraphInfo {
    BpAsset* owner;
    BpGraph* g;
    uint16_t* out_reg;  // per node: its first output register
    int32_t* exec_pc;   // per node: where its code starts (-1: not yet)
    uint16_t res_reg;   // a function's results
    int32_t entry_pc;   // a function's start
    int32_t entry_node; // its Function Entry node
    uint16_t* state;    // per node: its state register (Gate, Do Once, FlipFlop, the loops' Break; 0: none yet)
    bool expanded;      // g is a copy with its macros expanded (freed after compiling)
    uint16_t gidx;      // its index in owner's graphs (the debugger's)
    uint16_t* dbg;      // per node: its debugger node (an index + 1 of the asset's dbg_nodes; 0: not yet)
} GraphInfo;

typedef struct Fix {
    uint32_t op;
    uint8_t field; // 0: a, 1: b
    uint8_t kind;  // 0: a function's entry (gi), 1: a custom event (name)
    uint16_t gi;
    char name[BP_NAME];
} Fix;

typedef struct Cc {
    BpAsset* a;
    BpAsset* chain[8]; // the root parent first, the asset compiled last
    uint32_t nchain;
    struct { const BpVar* v; uint16_t slot; } vars[256];
    uint32_t nvars;
    GraphInfo graphs[8 * 16];
    uint32_t ngraphs;
    GraphInfo* cur;
    Fix fix[512];
    uint32_t nfix;
    uint32_t nregs, cap_init;
    int depth;
    bool fail;
    // the events' owners (a child's event replaces its parent's) and the same class's repeated BeginPlay / Tick /
    // EndPlay: all of them run, in order (they used to replace each other: a second Tick stopped the walking)
    const BpAsset* entry_owner[64];
    uint32_t dup_pc[64][4];
    uint8_t ndup[64];
    uint16_t cur_src; // the node the ops emitted now come from (the debugger's map: BpAsset.op_src)
} Cc;

static void cc_error(Cc* cc, const BpNode* n, const char* fmt, ...) {
    if (cc->fail) return;
    cc->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cc->a->error, sizeof cc->a->error, fmt, ap);
    va_end(ap);
    cc->a->error_node = n ? n->id : 0;
}

static uint32_t emit(Cc* cc, uint16_t op, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    BpAsset* as = cc->a;
    if (as->ncode == as->cap_code) {
        uint32_t cap = as->cap_code ? as->cap_code * 2 : 256;
        BpOp* nc = realloc(as->code, sizeof(BpOp) * cap); // compiling, not a frame
        if (!nc || cap > 0x7FFF) {
            cc_error(cc, NULL, "The Blueprint is too big (over 32767 ops)");
            return as->ncode ? as->ncode - 1 : 0;
        }
        as->code = nc;
        as->cap_code = cap;
    }
    as->code[as->ncode] = (BpOp){ op, a, b, c, d };
    if (as->ncode >= as->cap_src) { // (the debugger's map: the op's node)
        uint32_t cap = as->cap_code > as->cap_src ? as->cap_code : as->cap_src + 256;
        uint16_t* ns = realloc(as->op_src, sizeof(uint16_t) * cap); // compiling, not a frame
        if (ns) as->op_src = ns, as->cap_src = cap;
    }
    if (as->ncode < as->cap_src) as->op_src[as->ncode] = cc->cur_src;
    return as->ncode++;
}

// The debugger's node of n (bp_debug.h): made the first time one of its ops is emitted; 0 when none can be
static uint16_t dbg_node(Cc* cc, const BpNode* n) {
    GraphInfo* gi = cc->cur;
    if (!gi || !gi->dbg || n < gi->g->nodes || n >= gi->g->nodes + gi->g->nnodes) return 0;
    uint32_t ni = (uint32_t)(n - gi->g->nodes);
    if (gi->dbg[ni]) return gi->dbg[ni];
    BpAsset* a = cc->a;
    if (a->ndbg_nodes >= 0xFFFEu) return 0;
    if (a->ndbg_nodes == a->cap_dbg_nodes) {
        uint32_t cap = a->cap_dbg_nodes ? a->cap_dbg_nodes * 2 : 64;
        BpDbgNode* nd = realloc(a->dbg_nodes, sizeof(BpDbgNode) * cap);
        if (!nd) return 0;
        a->dbg_nodes = nd, a->cap_dbg_nodes = cap;
    }
    BpDbgNode* d = &a->dbg_nodes[a->ndbg_nodes++];
    d->owner = gi->owner, d->node_id = n->id, d->graph = gi->gidx, d->kind = n->kind, d->out_reg = gi->out_reg[ni];
    d->line = 0;
    if (gi->owner->generated && n->comment[0] == 'L') d->line = atoi(n->comment + 1); // (a script's node: "L<line>:C<column>")
    return gi->dbg[ni] = (uint16_t)a->ndbg_nodes;
}
// Where an executable node starts (its input ord's code): a breakpoint's op
static void dbg_site(Cc* cc, uint16_t node, uint32_t pc, int ord) {
    BpAsset* a = cc->a;
    if (!node || cc->fail) return;
    if (a->ndbg_sites == a->cap_dbg_sites) {
        uint32_t cap = a->cap_dbg_sites ? a->cap_dbg_sites * 2 : 64;
        BpDbgSite* ns = realloc(a->dbg_sites, sizeof(BpDbgSite) * cap);
        if (!ns) return;
        a->dbg_sites = ns, a->cap_dbg_sites = cap;
    }
    a->dbg_sites[a->ndbg_sites++] = (BpDbgSite){ pc, (uint16_t)(node - 1), (uint8_t)ord };
}
static uint16_t here(Cc* cc) { return (uint16_t)cc->a->ncode; }

static uint16_t konst(Cc* cc, const BpValue* v) {
    BpAsset* as = cc->a;
    for (uint32_t i = as->nconsts > 64 ? as->nconsts - 64 : 0; i < as->nconsts; i++)
        if (!memcmp(&as->consts[i], v, sizeof *v)) return (uint16_t)(BP_K | i);
    if (as->nconsts == as->cap_consts) {
        uint32_t cap = as->cap_consts ? as->cap_consts * 2 : 64;
        BpValue* nk = realloc(as->consts, sizeof(BpValue) * cap);
        if (!nk || cap > 0x7FFF) {
            cc_error(cc, NULL, "Too many constants");
            return BP_K;
        }
        as->consts = nk;
        as->cap_consts = cap;
    }
    as->consts[as->nconsts] = *v;
    return (uint16_t)(BP_K | as->nconsts++);
}
static uint16_t konst_f(Cc* cc, float x) { return konst(cc, &(BpValue){ { x, 0, 0, 0 } }); }

static uint16_t string_index(Cc* cc, const char* s) {
    BpAsset* as = cc->a;
    for (uint32_t i = 0; i < as->nstrings; i++)
        if (!strcmp(as->strings[i], s)) return (uint16_t)i;
    if (as->nstrings >= 128) {
        cc_error(cc, NULL, "Too many different texts (128)");
        return 0;
    }
    snprintf(as->strings[as->nstrings], sizeof as->strings[0], "%s", s);
    return (uint16_t)as->nstrings++;
}

static uint32_t type_width(uint8_t type, const char* ref) { return bp_type_width(type, ref); }
// A pin's registers; a split pin's part: its whole's (the parts are read from / assembled into it)
static uint32_t whole_width(const BpPin* p) { return p->sub ? bp_type_width(p->ptype, p->pref) : bp_pin_width(p); }

static uint16_t alloc_regs(Cc* cc, uint32_t n) {
    uint32_t r = cc->nregs;
    cc->nregs += n;
    if (cc->nregs >= 0x7FFF) {
        cc_error(cc, NULL, "Too many values (over 32767 registers)");
        cc->nregs = r;
        return 0;
    }
    if (cc->nregs > cc->cap_init) {
        uint32_t cap = cc->cap_init ? cc->cap_init : 64;
        while (cap < cc->nregs) cap *= 2;
        BpValue* ni = realloc(cc->a->init, sizeof(BpValue) * cap);
        if (!ni) {
            cc_error(cc, NULL, "Out of memory");
            return 0;
        }
        memset(ni + cc->cap_init, 0, sizeof(BpValue) * (cap - cc->cap_init));
        cc->a->init = ni;
        cc->cap_init = cap;
    }
    return (uint16_t)r;
}

// A register that holds a container: each instance makes its own (with the variable's defaults)
static void arr_slot(Cc* cc, uint16_t slot, uint8_t cont, uint8_t type, const char* ref, uint8_t vtype, const BpVar* def) {
    BpAsset* as = cc->a;
    if (as->narr == as->cap_arr) {
        uint32_t cap = as->cap_arr ? as->cap_arr * 2 : 16;
        BpArrSlot* na = realloc(as->arr, sizeof(BpArrSlot) * cap); // compiling, not a frame
        if (!na) {
            cc_error(cc, NULL, "Out of memory");
            return;
        }
        as->arr = na, as->cap_arr = cap;
    }
    uint32_t w = type_width(type, ref), w2 = cont == BPC_MAP ? type_width(vtype, "") : 0;
    as->arr[as->narr++] = (BpArrSlot){ slot, cont, (uint8_t)w, (uint8_t)w2, type, def };
}

// n consecutive constants (never shared with others: konst merges equal ones)
static uint16_t konst_block(Cc* cc, const BpValue* v, uint32_t n) {
    uint32_t base = cc->a->nconsts;
    for (uint32_t i = 0; i < n; i++) konst(cc, &(BpValue){ { 0, 0, 0, -1.0f - (float)(base + i) } }); // unique markers
    for (uint32_t i = 0; i < n && base + i < cc->a->nconsts; i++) cc->a->consts[base + i] = v[i];
    return (uint16_t)(BP_K | base);
}

static uint16_t konst_u32(Cc* cc, uint32_t x) {
    BpValue v = { { 0 } };
    memcpy(&v.v[0], &x, 4);
    v.v[1] = 1.0f; // (a class / asset hash: not zero, not a float)
    return konst(cc, &v);
}

static int find_var(Cc* cc, const char* name) {
    for (int i = (int)cc->nvars - 1; i >= 0; i--)
        if (!strcmp(cc->vars[i].v->name, name)) return i;
    return -1;
}

// The k-th input / output, exec or data (a split pin counts once: its first part)
static int pin_of(const BpNode* n, int out, int exec, int k) {
    for (int i = 0; i < n->npins; i++) {
        if (n->pins[i].sub > 1) continue;
        if (n->pins[i].out == out && (n->pins[i].type == BPT_EXEC) == (exec != 0) && k-- == 0) return i;
    }
    return -1;
}
// The ordinal of exec input pin p (Gate: Enter 0, Open 1, ...)
static int exec_in_ordinal(const BpNode* n, int p) {
    int k = 0;
    for (int i = 0; i < p && i < n->npins; i++)
        if (!n->pins[i].out && n->pins[i].type == BPT_EXEC) k++;
    return k;
}

static int node_index(const GraphInfo* gi, const BpNode* n) { return (int)(n - gi->g->nodes); }

static uint16_t out_reg(GraphInfo* gi, const BpNode* n, int p) {
    uint32_t r = gi->out_reg[node_index(gi, n)];
    for (int i = 0; i < p; i++)
        if (n->pins[i].out && n->pins[i].type != BPT_EXEC && n->pins[i].sub <= 1) r += whole_width(&n->pins[i]);
    return (uint16_t)r;
}
static uint32_t outs_width(const BpNode* n) {
    uint32_t w = 0;
    for (int p = 0; p < n->npins; p++)
        if (n->pins[p].out && n->pins[p].type != BPT_EXEC && n->pins[p].sub <= 1) w += whole_width(&n->pins[p]);
    return w;
}

static const BpLink* link_to(const BpGraph* g, uint32_t node, int pin) {
    for (uint32_t i = 0; i < g->nlinks; i++)
        if (g->links[i].to_node == node && g->links[i].to_pin == pin) return &g->links[i];
    return NULL;
}
static const BpLink* link_from(const BpGraph* g, uint32_t node, int pin) {
    for (uint32_t i = 0; i < g->nlinks; i++)
        if (g->links[i].from_node == node && g->links[i].from_pin == pin) return &g->links[i];
    return NULL;
}

static void emit_pure(Cc* cc, BpNode* n);
static void compile_exec(Cc* cc, BpNode* n, int in_pin);

// (2026-11-10) the Custom Event wired into n's delegate input (Unreal's red square): its name, NULL when none is wired
static const char* delegate_event(Cc* cc, const BpNode* n) {
    const BpGraph* g = cc->cur ? cc->cur->g : NULL;
    for (int p = 0; g && p < n->npins; p++) {
        if (!n->pins[p].delegate || n->pins[p].out) continue;
        const BpLink* l = link_to(g, n->id, p);
        if (!l) return NULL;
        for (uint32_t i = 0; i < g->nnodes; i++)
            if (g->nodes[i].id == l->from_node && g->nodes[i].kind == BPK_EVENT_CUSTOM) return g->nodes[i].text;
        return NULL;
    }
    return NULL;
}

// The Custom Event node of that name in the asset's graphs (its Replicates: BpNode.net), NULL: none
static const BpNode* custom_event_node(const BpAsset* a, const char* name) {
    for (uint32_t g = 0; a && name && name[0] && g < a->ngraphs; g++)
        for (uint32_t i = 0; i < a->graphs[g].nnodes; i++)
            if (a->graphs[g].nodes[i].kind == BPK_EVENT_CUSTOM && !strcmp(a->graphs[g].nodes[i].text, name)) return &a->graphs[g].nodes[i];
    return NULL;
}

// The operand a whole output pin's value is read from (pure nodes are evaluated here, every time: Unreal's rule)
static uint16_t whole_src(Cc* cc, BpNode* f, int fp) {
    switch (f->kind) {
    case BPK_SELF: return 0;
    case BPK_VAR_GET:
    case BPK_VAR_SET: {
        int v = find_var(cc, f->text);
        if (v < 0) {
            cc_error(cc, f, "The variable \"%s\" doesn't exist", f->text);
            return 0;
        }
        return cc->vars[v].slot;
    }
    case BPK_ENUM_LITERAL: {
        const BpAsset* e = bp_find_name(BPA_ENUM, f->text);
        for (uint32_t k = 0; e && k < e->nentries; k++)
            if (!strcmp(e->entries[k], f->text2)) return konst_f(cc, (float)k);
        return konst_f(cc, 0.0f);
    }
    case BPK_DATA_GET: { // a Data Asset is read only: its value is a constant
        const BpAsset* d = bp_find_name(BPA_DATA, f->text);
        int v = d ? bp_var_index(d, f->text2) : -1;
        if (v < 0) {
            cc_error(cc, f, "The Data Asset \"%s\" has no value \"%s\"", f->text, f->text2);
            return 0;
        }
        if (bp_type_stringish(d->vars[v].type)) return konst_f(cc, (float)string_index(cc, d->vars[v].sdef));
        if (d->vars[v].type == BPT_TRANSFORM) {
            BpValue t[3] = { d->vars[v].def, d->vars[v].def3[0], d->vars[v].def3[1] };
            return konst_block(cc, t, 3);
        }
        return konst(cc, &d->vars[v].def);
    }
    default: break;
    }
    if (bp_node_desc(f->kind)->pure) emit_pure(cc, f);
    return out_reg(cc->cur, f, fp);
}

// An output pin's operand; a split part: read from its whole (a field's register, or a component copied out)
static uint16_t src_op(Cc* cc, BpNode* f, int fp) {
    const BpPin* p = &f->pins[fp];
    if (!p->sub) return whole_src(cc, f, fp);
    int first = fp;
    while (first > 0 && f->pins[first].sub > 1) first--;
    uint16_t base = whole_src(cc, f, first);
    if (p->ptype == BPT_TRANSFORM || p->ptype == BPT_STRUCT) {
        // (2026-11-24) the part's registers: the parts before it (a part split again counts once, by its own width)
        uint32_t off = 0, inner = 0;
        for (int i = first; i < fp; i++) {
            const BpPin* q = &f->pins[i];
            if (q->part != p->part) off += q->sub2 > 1 ? 0 : q->sub2 ? type_width(q->ptype2, q->pref2) : bp_pin_width(q);
            else if (q->sub2) inner += bp_pin_width(q);
        }
        if (!p->sub2) return (uint16_t)(base + off);
        if (p->ptype2 == BPT_TRANSFORM || p->ptype2 == BPT_STRUCT) return (uint16_t)(base + off + inner); // (a field of a field)
        uint16_t t = alloc_regs(cc, 1); // (a component of a vector / rotator / color part)
        emit(cc, OP_COMP, t, (uint16_t)(base + off), (uint16_t)(p->sub2 - 1), 0);
        return t;
    }
    uint16_t t = alloc_regs(cc, 1);
    emit(cc, OP_COMP, t, base, (uint16_t)p->part, 0);
    return t;
}

// A value of one type for a pin of another: the conversion Unreal's wire adds (none needed: the same operand)
static uint16_t convert(Cc* cc, uint16_t src, uint8_t from, uint8_t to) {
    if (from == to || from == BPT_WILDCARD || to == BPT_WILDCARD) return src;
    if (bp_type_stringish(to)) {
        if (bp_type_stringish(from)) return src;
        uint16_t t = alloc_regs(cc, 1);
        emit(cc, OP_TOSTR, t, src, from, 0);
        return t;
    }
    bool need = from == BPT_INT64 || to == BPT_INT64 || (to == BPT_BYTE && from != BPT_BYTE) || (to == BPT_INT && from == BPT_FLOAT) ||
                (to == BPT_COLOR && from != BPT_COLOR) || (to == BPT_BOOL && from != BPT_BOOL);
    if (!need) return src;
    uint16_t t = alloc_regs(cc, 1);
    emit(cc, OP_CONV, t, src, (uint16_t)(from | to << 8), 0);
    return t;
}

// The operand of one input pin: the wire's source (converted) or the literal typed in it (an actor left empty: self)
static uint16_t in_single(Cc* cc, BpNode* n, int p, uint8_t* src_type) {
    const BpPin* pin = &n->pins[p];
    const BpLink* l = link_to(cc->cur->g, n->id, p);
    if (src_type) *src_type = pin->type;
    if (l) {
        BpNode* f = bp_node(cc->cur->g, l->from_node);
        if (f && l->from_pin < f->npins) {
            uint8_t ft = f->pins[l->from_pin].type;
            uint16_t s = src_op(cc, f, l->from_pin);
            if (pin->cont || f->pins[l->from_pin].cont) return s;
            s = convert(cc, s, ft, pin->type);
            if (src_type) *src_type = bp_type_stringish(pin->type) ? BPT_STRING : ft;
            return s;
        }
    }
    if (pin->cont) return konst_f(cc, 0.0f); // no container: the natives see none (an empty one)
    if (pin->type == BPT_WILDCARD) {
        cc_error(cc, n, "\"%s\": connect its wildcard pin (its type comes from the wire)", bp_node_desc(n->kind)->name);
        return 0;
    }
    switch (pin->type) {
    case BPT_ACTOR: return 0;
    case BPT_OBJECT: return konst_f(cc, 0.0f);
    case BPT_STRING: case BPT_NAME: case BPT_TEXT: return konst_f(cc, (float)string_index(cc, pin->slit));
    case BPT_CLASS: {
        const BpAsset* c = pin->slit[0] ? bp_find_name(BPA_CLASS, pin->slit) : NULL;
        if (pin->slit[0] && !c) cc_error(cc, n, "No Blueprint Class \"%s\"", pin->slit);
        return c ? konst_u32(cc, c->hash) : konst_f(cc, 0.0f);
    }
    case BPT_TRANSFORM: {
        BpValue t[3] = { { { 0 } }, { { 0 } }, { { 1, 1, 1, 0 } } };
        return konst_block(cc, t, 3);
    }
    case BPT_STRUCT: {
        uint32_t w = type_width(pin->type, pin->ref);
        BpValue z[64] = { { { 0 } } };
        return konst_block(cc, z, w < 64 ? w : 64);
    }
    default: return konst(cc, &pin->lit);
    }
}

// The operand of input pin p; a split pin's parts are assembled into its whole first
static uint16_t in_op(Cc* cc, BpNode* n, int p, uint8_t* src_type) {
    const BpPin* pin = &n->pins[p];
    if (pin->sub != 1) return in_single(cc, n, p, src_type);
    uint32_t w = type_width(pin->ptype, pin->pref);
    uint16_t t = alloc_regs(cc, w);
    if (src_type) *src_type = pin->ptype;
    if (pin->ptype == BPT_TRANSFORM || pin->ptype == BPT_STRUCT) {
        uint32_t off = 0;
        for (int i = p; i < n->npins && n->pins[i].sub && (i == p || n->pins[i].sub > 1);) {
            const BpPin* q = &n->pins[i];
            if (q->sub2) { // (2026-11-24) a part split again: its pins assembled into the part, the part into the whole
                uint32_t pw = type_width(q->ptype2, q->pref2);
                bool vecish_part = q->ptype2 == BPT_VECTOR || q->ptype2 == BPT_ROTATOR || q->ptype2 == BPT_COLOR;
                if (vecish_part) emit(cc, OP_MOVN, (uint16_t)(t + off), konst(cc, &(BpValue){ { 0, 0, 0, 0 } }), 1, 0);
                uint32_t inner = 0;
                uint8_t part = q->part;
                for (; i < n->npins && n->pins[i].sub2 && n->pins[i].part == part && n->pins[i].sub > (i == p ? 0 : 1); i++) {
                    if (vecish_part) emit(cc, OP_SETCOMP, (uint16_t)(t + off), in_single(cc, n, i, NULL), (uint16_t)(n->pins[i].sub2 - 1), 0);
                    else {
                        uint32_t iw = bp_pin_width(&n->pins[i]);
                        emit(cc, OP_MOVN, (uint16_t)(t + off + inner), in_single(cc, n, i, NULL), (uint16_t)iw, 0);
                        inner += iw;
                    }
                }
                off += pw;
                continue;
            }
            uint32_t pw = bp_pin_width(q);
            emit(cc, OP_MOVN, (uint16_t)(t + off), in_single(cc, n, i, NULL), (uint16_t)pw, 0);
            off += pw;
            i++;
        }
    } else {
        emit(cc, OP_MOVN, t, konst(cc, &(BpValue){ { 0, 0, 0, 0 } }), 1, 0);
        for (int i = p; i < n->npins && n->pins[i].sub && (i == p || n->pins[i].sub > 1); i++)
            emit(cc, OP_SETCOMP, t, in_single(cc, n, i, NULL), (uint16_t)n->pins[i].part, 0);
    }
    return t;
}

static int data_inputs(Cc* cc, BpNode* n, uint16_t* ops, uint8_t* types, int max) {
    int k = 0;
    for (int i = 0; i < n->npins && k < max; i++)
        if (!n->pins[i].out && n->pins[i].type != BPT_EXEC && n->pins[i].sub <= 1 && !n->pins[i].asset) { // (the asset pin: asset_name_op)
            ops[k] = in_op(cc, n, i, types ? &types[k] : NULL);
            k++;
        }
    return k;
}

// (2026-11-10) the node's asset pin (bp_asset_pin): wired, its value staged in the last parameter slot (the native
// reads it there: name_at) and the name operand says so; else the text's constant as before. Called right before the
// node's own op (after its inputs: nothing else stages in between)
#define BP_NAME_STAGED 0xFFFFu
static int asset_pin_of(const BpNode* n) {
    for (int i = 0; i < n->npins; i++)
        if (n->pins[i].asset) return i;
    return -1;
}
static bool asset_linked(Cc* cc, const BpNode* n) {
    int p = asset_pin_of(n);
    return p >= 0 && link_to(cc->cur->g, n->id, p) != NULL;
}
static uint16_t asset_name_op(Cc* cc, BpNode* n) {
    if (!asset_linked(cc, n)) return string_index(cc, n->text);
    uint16_t r = in_op(cc, n, asset_pin_of(n), NULL);
    emit(cc, OP_NPARAM, BP_MAX_NPARAM - 1, r, 0, 0);
    return BP_NAME_STAGED;
}
// ... a node that takes its asset's name as a register (the sounds, the systems, the decals): the pin's own when wired
static uint16_t asset_name_reg(Cc* cc, BpNode* n) {
    return asset_linked(cc, n) ? in_op(cc, n, asset_pin_of(n), NULL) : konst_f(cc, (float)string_index(cc, n->text));
}

// A value moved into a register: a container is copied (each register owns its own), the rest moved
static void emit_move(Cc* cc, uint16_t dst, uint16_t src, uint32_t w, uint8_t cont) {
    if (cont) emit(cc, OP_ACOPY, dst, src, 0, 0);
    else if (w) emit(cc, OP_MOVN, dst, src, (uint16_t)w, 0);
}

// A native with more than three inputs: the rest go first (OP_NPARAM)
static void emit_native(Cc* cc, BpNode* n, const uint16_t* in, int nin, uint16_t d) {
    if (n->kind == BPK_PLUGIN_PURE || n->kind == BPK_PLUGIN_CALL) { // (B14c) every input staged, the function by its name
        if (plugin_fn_find(n->text) < 0) cc_error(cc, n, "The plugin node \"%s\" isn't loaded (Edit > Plugins)", n->text);
        for (int i = 0; i < nin && i < BP_MAX_NPARAM; i++) emit(cc, OP_NPARAM, (uint16_t)i, in[i], 0, 0);
        emit(cc, (uint16_t)(OP_NATIVE + n->kind), string_index(cc, n->text), (uint16_t)(nin < BP_MAX_NPARAM ? nin : BP_MAX_NPARAM), 0, d);
        return;
    }
    for (int i = 3; i < nin && i - 3 < BP_MAX_NPARAM; i++) emit(cc, OP_NPARAM, (uint16_t)(i - 3), in[i], 0, 0);
    emit(cc, (uint16_t)(OP_NATIVE + n->kind), nin > 0 ? in[0] : 0, nin > 1 ? in[1] : 0, nin > 2 ? in[2] : 0, d);
}

static void emit_pure_ops(Cc* cc, BpNode* n);
static void emit_pure(Cc* cc, BpNode* n) { // (the debugger: its ops are its node's)
    uint16_t prev = cc->cur_src;
    cc->cur_src = dbg_node(cc, n);
    emit_pure_ops(cc, n);
    cc->cur_src = prev;
}
static void emit_pure_ops(Cc* cc, BpNode* n) {
    if (++cc->depth > 64) {
        cc_error(cc, n, "The pure nodes make a loop");
        cc->depth--;
        return;
    }
    uint16_t in[BP_MAX_PINS] = { 0 };
    uint8_t types[BP_MAX_PINS] = { 0 };
    int nin = data_inputs(cc, n, in, types, BP_MAX_PINS);
    int first_out = pin_of(n, 1, 0, 0);
    uint16_t d = first_out >= 0 ? out_reg(cc->cur, n, first_out) : 0;
    switch (n->kind) {
    case BPK_AXIS: {
        int ax = bp_axis_index(n->text);
        if (ax < 0) cc_error(cc, n, "Unknown input axis \"%s\" (MoveForward, MoveRight, Turn, LookUp)", n->text);
        emit(cc, OP_NATIVE + BPK_AXIS, (uint16_t)(ax < 0 ? 0 : ax), 0, 0, d);
        break;
    }
    case BPK_KEY_DOWN: {
        int key = bp_key_code(n->text);
        if (key < 0) cc_error(cc, n, "Unknown key \"%s\"", n->text);
        emit(cc, OP_NATIVE + BPK_KEY_DOWN, (uint16_t)(key < 0 ? 0 : key), 0, 0, d);
        break;
    }
    case BPK_DOES_IMPLEMENT: emit(cc, OP_NATIVE + n->kind, in[0], string_index(cc, n->text), 0, d); break;
    case BPK_WIDGET_GET_VALUE:
    case BPK_WIDGET_GET_TEXT:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the widget (Details)");
        emit(cc, OP_NATIVE + n->kind, in[0], 0, asset_name_op(cc, n), d);
        break;
    case BPK_IS_SEQUENCE_PLAYING: emit(cc, OP_NATIVE + n->kind, 0, 0, asset_name_op(cc, n), d); break;
    case BPK_BB_GET_FLOAT: case BPK_BB_GET_BOOL: case BPK_BB_GET_VECTOR: case BPK_BB_GET_ACTOR:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Blackboard key (Details)");
        emit(cc, OP_NATIVE + n->kind, in[0], 0, asset_name_op(cc, n), d);
        break;
    case BPK_MAKE_STRUCT: {
        uint16_t r = d;
        for (int i = 0, k = 0; i < n->npins; i++)
            if (!n->pins[i].out && n->pins[i].type != BPT_EXEC && n->pins[i].sub <= 1) {
                uint32_t w = whole_width(&n->pins[i]);
                emit_move(cc, r, in[k++], w, n->pins[i].cont);
                r = (uint16_t)(r + w);
            }
        break;
    }
    case BPK_BREAK_STRUCT:
    case BPK_BREAK_TRANSFORM: { // the whole copied into the outputs, field by field
        uint32_t w = outs_width(n);
        if (w) emit(cc, OP_MOVN, d, in[0], (uint16_t)w, 0);
        break;
    }
    case BPK_MAKE_TRANSFORM: // location, rotation, scale: three registers in a row
        emit(cc, OP_MOVN, d, in[0], 1, 0);
        emit(cc, OP_MOVN, (uint16_t)(d + 1), in[1], 1, 0);
        emit(cc, OP_MOVN, (uint16_t)(d + 2), in[2], 1, 0);
        break;
    case BPK_TO_STRING: emit(cc, OP_TOSTR, d, in[0], types[0], 0); break;
    case BPK_SELECTW: { // any width: Pick A ? A : B
        uint32_t w = whole_width(&n->pins[first_out]);
        uint32_t jz = emit(cc, OP_JZ, in[0], 0, 0, 0);
        emit(cc, OP_MOVN, d, in[1], (uint16_t)w, 0);
        uint32_t jmp = emit(cc, OP_JMP, 0, 0, 0, 0);
        cc->a->code[jz].b = here(cc);
        emit(cc, OP_MOVN, d, in[2], (uint16_t)w, 0);
        cc->a->code[jmp].a = here(cc);
        break;
    }
    case BPK_ARR_MAKE: // emptied, then each input added
        emit(cc, OP_NATIVE + BPK_ARR_CLEAR, d, 0, 0, 0);
        for (int i = 0; i < nin; i++) emit(cc, OP_NATIVE + BPK_ARR_ADD, d, in[i], 0, 0);
        break;
    case BPK_GET_LEVEL_NAME: emit(cc, OP_NATIVE + n->kind, 0, 0, 0, d); break;
    case BPK_GET_COMPONENT:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the component (drag it from Components)");
        emit(cc, OP_NATIVE + n->kind, 0, 0, string_index(cc, n->text), d);
        break;
    case BPK_GET_ACTION_VALUE:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Input Action (Details)");
        emit(cc, OP_NATIVE + n->kind, 0, 0, string_index(cc, n->text), d);
        break;
    case BPK_APPEND: // two at a time
        emit(cc, OP_NATIVE + BPK_APPEND, in[0], nin > 1 ? in[1] : konst_f(cc, (float)string_index(cc, "")), 0, d);
        for (int i = 2; i < nin; i++) emit(cc, OP_NATIVE + BPK_APPEND, d, in[i], 0, d);
        break;
    default: emit_native(cc, n, in, nin, d); break;
    }
    cc->depth--;
}

// Follows exec output k of n: the next node's code (at the input pin the wire reaches), or the end of the chain
static void chain(Cc* cc, BpNode* n, int k) {
    int p = pin_of(n, 1, 1, k);
    const BpLink* l = p >= 0 ? link_from(cc->cur->g, n->id, p) : NULL;
    BpNode* t = l ? bp_node(cc->cur->g, l->to_node) : NULL;
    if (t) compile_exec(cc, t, l->to_pin);
    else emit(cc, OP_RET, 0, 0, 0, 0);
}

static GraphInfo* find_function(Cc* cc, const char* name) {
    for (int i = (int)cc->ngraphs - 1; i >= 0; i--) // the child's override the parent's
        if (cc->graphs[i].entry_node >= 0 && !strcmp(cc->graphs[i].g->name, name)) return &cc->graphs[i];
    return NULL;
}

static void add_fix(Cc* cc, uint32_t op, uint8_t field, uint8_t kind, uint16_t gi, const char* name) {
    if (cc->nfix >= 512) {
        cc_error(cc, NULL, "Too many calls");
        return;
    }
    Fix* f = &cc->fix[cc->nfix++];
    f->op = op, f->field = field, f->kind = kind, f->gi = gi;
    snprintf(f->name, sizeof f->name, "%s", name ? name : "");
}

// A state register of a node (Gate's, Do N's ...) with its value when an instance starts
// (2026-11-16) a Timeline node's slot in the class's table (its code's places, its registers): one per node
static int tl_slot(Cc* cc, BpNode* n) {
    BpAsset* a = cc->a;
    uint16_t ni = (uint16_t)node_index(cc->cur, n);
    for (int i = 0; i < a->ntlc; i++)
        if (a->tlc[i].node == ni && a->tlc[i].graph == (uint8_t)cc->cur->gidx) return i;
    const BpTimeline* tl = bp_timeline_find(a, n->text);
    if (!tl) {
        cc_error(cc, n, "No such Timeline (double-click the node to make one)");
        return -1;
    }
    if (a->ntlc >= 8) {
        cc_error(cc, n, "Too many Timelines (8)");
        return -1;
    }
    struct BpTlCode* t = &a->tlc[a->ntlc];
    memset(t, 0, sizeof *t);
    t->tl = tl, t->node = ni, t->graph = (uint8_t)cc->cur->gidx;
    t->pc_update = t->pc_finished = -1;
    for (int k = 0; k < 6; k++) t->pc_event[k] = -1;
    return a->ntlc++;
}
static uint16_t state_reg(Cc* cc, float init) {
    uint16_t r = alloc_regs(cc, 1);
    if (!cc->fail) cc->a->init[r].v[0] = init;
    return r;
}
// A bool input's literal (Start Closed): its value when it is not wired
static float lit_of(Cc* cc, BpNode* n, const char* name) {
    for (int i = 0; i < n->npins; i++)
        if (!n->pins[i].out && !strcmp(n->pins[i].name, name)) return link_to(cc->cur->g, n->id, i) ? 0.0f : n->pins[i].lit.v[0];
    return 0.0f;
}


// (2026-11-10) Expose on Spawn: Spawn Actor's pins for the class's exposed variables (build_pins), each staged by its
// name's hash before the OP_SPAWN that gives them to the new actor
static void spawn_args(Cc* cc, BpNode* n) {
    for (int p = 0; p < n->npins; p++) {
        if (n->pins[p].out || !n->pins[p].spawnarg) continue;
        uint16_t val = in_op(cc, n, p, NULL);
        emit(cc, OP_SPAWNARG, val, konst_u32(cc, bp_name_hash(n->pins[p].name)), 0, bp_type_stringish(n->pins[p].type) ? 1 : 0);
    }
}

static void compile_exec(Cc* cc, BpNode* n, int in_pin) {
    GraphInfo* gi = cc->cur;
    int ni = node_index(gi, n);
    int ord = in_pin >= 0 && in_pin < n->npins ? exec_in_ordinal(n, in_pin) : 0;
    if (ord > 7) ord = 7;
    int32_t* cached = &gi->exec_pc[ni * 8 + ord];
    if (*cached >= 0) { // reached again: one copy of its code
        emit(cc, OP_JMP, (uint16_t)*cached, 0, 0, 0);
        return;
    }
    if (++cc->depth > 2000) {
        cc_error(cc, n, "The execution chain is too long");
        cc->depth--;
        return;
    }
    *cached = here(cc);
    uint16_t dbg_prev = cc->cur_src; // (the debugger: its ops are its own, its start a breakpoint's place)
    cc->cur_src = dbg_node(cc, n);
    dbg_site(cc, cc->cur_src, *cached, ord);
    const BpNodeDesc* desc = bp_node_desc(n->kind);
    uint16_t in[BP_MAX_PINS] = { 0 };
    // the node's state (Gate, Do N, FlipFlop, Do Once, the loops' Break): one register per node, shared by its inputs
    uint16_t* st = &gi->state[ni];
    switch (n->kind) {
    case BPK_BRANCH: {
        data_inputs(cc, n, in, NULL, 1);
        uint32_t jz = emit(cc, OP_JZ, in[0], 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[jz].b = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_SEQUENCE: {
        int outs = 0;
        while (pin_of(n, 1, 1, outs) >= 0) outs++;
        for (int k = 0; k + 1 < outs; k++) {
            uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
            chain(cc, n, k);
            cc->a->code[push].a = here(cc);
        }
        chain(cc, n, outs - 1);
        break;
    }
    case BPK_FOR_LOOP:
    case BPK_FOR_LOOP_BREAK: {
        if (n->kind == BPK_FOR_LOOP_BREAK && !*st) *st = state_reg(cc, 0.0f);
        if (n->kind == BPK_FOR_LOOP_BREAK && ord == 1) { // Break
            emit(cc, OP_MOVN, *st, konst_f(cc, 1.0f), 1, 0);
            emit(cc, OP_RET, 0, 0, 0, 0);
            break;
        }
        uint16_t idx = out_reg(gi, n, pin_of(n, 1, 0, 0));
        int pf = pin_of(n, 0, 0, 0), pl = pin_of(n, 0, 0, 1);
        emit(cc, OP_MOVN, idx, in_op(cc, n, pf, NULL), 1, 0);
        if (*st) emit(cc, OP_MOVN, *st, konst_f(cc, 0.0f), 1, 0);
        uint16_t loop = here(cc);
        uint32_t jb = *st ? emit(cc, OP_JNZ, *st, 0, 0, 0) : 0;
        uint32_t jgt = emit(cc, OP_JGT, idx, in_op(cc, n, pl, NULL), 0, 0);
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[push].a = here(cc);
        emit(cc, OP_INC, idx, 0, 0, 0);
        emit(cc, OP_JMP, loop, 0, 0, 0);
        cc->a->code[jgt].c = here(cc);
        if (*st) cc->a->code[jb].b = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_FOREACH:
    case BPK_FOREACH_BREAK: {
        if (n->kind == BPK_FOREACH_BREAK && !*st) *st = state_reg(cc, 0.0f);
        if (n->kind == BPK_FOREACH_BREAK && ord == 1) {
            emit(cc, OP_MOVN, *st, konst_f(cc, 1.0f), 1, 0);
            emit(cc, OP_RET, 0, 0, 0, 0);
            break;
        }
        uint16_t arr = in_op(cc, n, pin_of(n, 0, 0, 0), NULL);
        uint16_t elem = out_reg(gi, n, pin_of(n, 1, 0, 0)), idx = out_reg(gi, n, pin_of(n, 1, 0, 1)), len = alloc_regs(cc, 1);
        emit(cc, OP_MOVN, idx, konst_f(cc, 0.0f), 1, 0);
        if (*st) emit(cc, OP_MOVN, *st, konst_f(cc, 0.0f), 1, 0);
        uint16_t loop = here(cc);
        uint32_t jb = *st ? emit(cc, OP_JNZ, *st, 0, 0, 0) : 0;
        emit(cc, OP_ALEN, len, arr, 0, 0);
        uint32_t jge = emit(cc, OP_JGE, idx, len, 0, 0);
        emit(cc, OP_AGET, elem, arr, idx, 0);
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[push].a = here(cc);
        emit(cc, OP_INC, idx, 0, 0, 0);
        emit(cc, OP_JMP, loop, 0, 0, 0);
        cc->a->code[jge].c = here(cc);
        if (*st) cc->a->code[jb].b = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_WHILE: {
        uint16_t loop = here(cc);
        uint16_t c = in_op(cc, n, pin_of(n, 0, 0, 0), NULL); // (evaluated again each time round)
        uint32_t jz = emit(cc, OP_JZ, c, 0, 0, 0);
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[push].a = here(cc);
        emit(cc, OP_JMP, loop, 0, 0, 0);
        cc->a->code[jz].b = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_TIMELINE: { // (2026-11-16) an input: the timeline's state changes; its outputs run from the tick (bp_tick)
        int sl = tl_slot(cc, n);
        if (sl < 0) break;
        uint16_t nt = 0;
        if (ord == 5) {
            for (int p = 0; p < n->npins; p++)
                if (!n->pins[p].out && n->pins[p].type == BPT_FLOAT) nt = in_op(cc, n, p, NULL);
        }
        emit(cc, OP_TIMELINE, (uint16_t)sl, (uint16_t)ord, nt, 0);
        emit(cc, OP_RET, 0, 0, 0, 0);
        break;
    }
    case BPK_DELAY:
    case BPK_RETRIGGER_DELAY: {
        data_inputs(cc, n, in, NULL, 1);
        if (cc->a->nlatent >= 255) cc_error(cc, n, "Too many Delays (255)");
        uint32_t lat = n->kind == BPK_DELAY ? emit(cc, OP_LATENT, (uint16_t)cc->a->nlatent++, in[0], 0, 0)
                                            : emit(cc, OP_LATENT2, (uint16_t)cc->a->nlatent++, in[0], 0, LAT_RETRIGGER);
        cc->a->code[lat].c = here(cc);
        chain(cc, n, 0);
        break;
    }
    case BPK_DO_ONCE: {
        if (!*st) *st = state_reg(cc, lit_of(cc, n, "Start Closed"));
        if (ord == 1) { // Reset
            emit(cc, OP_MOVN, *st, konst_f(cc, 0.0f), 1, 0);
            emit(cc, OP_RET, 0, 0, 0, 0);
            break;
        }
        uint32_t jnz = emit(cc, OP_JNZ, *st, 0, 0, 0);
        emit(cc, OP_MOVN, *st, konst_f(cc, 1.0f), 1, 0);
        chain(cc, n, 0);
        cc->a->code[jnz].b = here(cc);
        emit(cc, OP_RET, 0, 0, 0, 0);
        break;
    }
    case BPK_FLIPFLOP: {
        if (!*st) *st = state_reg(cc, 0.0f);
        uint16_t isa = out_reg(gi, n, pin_of(n, 1, 0, 0));
        uint32_t jnz = emit(cc, OP_JNZ, *st, 0, 0, 0);
        emit(cc, OP_MOVN, *st, konst_f(cc, 1.0f), 1, 0);
        emit(cc, OP_MOVN, isa, konst_f(cc, 1.0f), 1, 0);
        chain(cc, n, 0);
        cc->a->code[jnz].b = here(cc);
        emit(cc, OP_MOVN, *st, konst_f(cc, 0.0f), 1, 0);
        emit(cc, OP_MOVN, isa, konst_f(cc, 0.0f), 1, 0);
        chain(cc, n, 1);
        break;
    }
    case BPK_GATE: { // the register: open
        if (!*st) *st = state_reg(cc, lit_of(cc, n, "Start Closed") != 0.0f ? 0.0f : 1.0f);
        if (ord == 0) {
            uint32_t jz = emit(cc, OP_JZ, *st, 0, 0, 0);
            chain(cc, n, 0);
            cc->a->code[jz].b = here(cc);
        } else if (ord == 1 || ord == 2) emit(cc, OP_MOVN, *st, konst_f(cc, ord == 1 ? 1.0f : 0.0f), 1, 0);
        else emit(cc, OP_NATIVE + BPK_NOT, *st, 0, 0, *st);
        emit(cc, OP_RET, 0, 0, 0, 0);
        break;
    }
    case BPK_DO_N: {
        uint16_t counter = out_reg(gi, n, pin_of(n, 1, 0, 0));
        if (ord == 1) { // Reset
            emit(cc, OP_MOVN, counter, konst_f(cc, 0.0f), 1, 0);
            emit(cc, OP_RET, 0, 0, 0, 0);
            break;
        }
        uint32_t jge = emit(cc, OP_JGE, counter, in_op(cc, n, pin_of(n, 0, 0, 0), NULL), 0, 0);
        emit(cc, OP_INC, counter, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[jge].c = here(cc);
        emit(cc, OP_RET, 0, 0, 0, 0);
        break;
    }
    case BPK_ISVALID_EXEC: {
        data_inputs(cc, n, in, NULL, 1);
        uint16_t t = alloc_regs(cc, 1);
        emit(cc, OP_NATIVE + BPK_IS_VALID, in[0], 0, 0, t);
        uint32_t jz = emit(cc, OP_JZ, t, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[jz].b = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_PRINT:
        data_inputs(cc, n, in, NULL, 1);
        emit(cc, OP_PRINT, in[0], 0, 0, 0);
        chain(cc, n, 0);
        break;
    case BPK_VAR_SET: {
        int v = find_var(cc, n->text);
        if (v < 0) {
            cc_error(cc, n, "The variable \"%s\" doesn't exist", n->text);
            break;
        }
        data_inputs(cc, n, in, NULL, 1);
        emit_move(cc, cc->vars[v].slot, in[0], bp_var_width(cc->vars[v].v), cc->vars[v].v->cont);
        if (cc->vars[v].v->rep) // (Unreal's Replicated / RepNotify: the server sends it; RepNotify runs OnRep_<Name> here too)
            emit(cc, OP_NATIVE + BPK_NET_REPLICATE_VAR, 0, cc->vars[v].v->rep == BP_REP_NOTIFY ? 1 : 0, 0, string_index(cc, n->text));
        chain(cc, n, 0);
        break;
    }
    case BPK_CAST: {
        data_inputs(cc, n, in, NULL, 1);
        uint32_t c = emit(cc, OP_CAST, in[0], string_index(cc, n->text), out_reg(gi, n, pin_of(n, 1, 0, 0)), 0);
        chain(cc, n, 0);
        cc->a->code[c].d = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_SPAWN: {
        data_inputs(cc, n, in, NULL, 2);
        spawn_args(cc, n);
        emit(cc, OP_SPAWN, in[1], in[0], out_reg(gi, n, pin_of(n, 1, 0, 0)), 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_SPAWN_ASYNC: { // on at once; Completed when the class is ready and the actor spawned
        if (cc->a->nlatent >= 255) cc_error(cc, n, "Too many latent actions (255)");
        uint16_t slot = (uint16_t)cc->a->nlatent++;
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        uint32_t lat = emit(cc, OP_LATENT2, slot, in_op(cc, n, pin_of(n, 0, 0, 0), NULL), 0, LAT_SPAWN);
        cc->a->code[push].a = here(cc);
        chain(cc, n, 0);
        cc->a->code[lat].c = here(cc);
        data_inputs(cc, n, in, NULL, 2);
        spawn_args(cc, n);
        emit(cc, OP_SPAWN, in[1], in[0], out_reg(gi, n, pin_of(n, 1, 0, 0)), 0);
        chain(cc, n, 1);
        break;
    }
    case BPK_BUILD_LIGHTING: { // (Q16) the request now (Started), the first pin at once; Completed once the bake ended
        if (cc->a->nlatent >= 255) cc_error(cc, n, "Too many latent actions (255)");
        int nin = data_inputs(cc, n, in, NULL, BP_MAX_PINS);
        int po = pin_of(n, 1, 0, 0);
        emit_native(cc, n, in, nin, po >= 0 ? out_reg(gi, n, po) : 0);
        uint16_t slot = (uint16_t)cc->a->nlatent++;
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        uint32_t lat = emit(cc, OP_LATENT2, slot, 0, 0, LAT_LIGHTING);
        cc->a->code[push].a = here(cc);
        chain(cc, n, 0);
        cc->a->code[lat].c = here(cc);
        chain(cc, n, 1);
        break;
    }
    case BPK_LOAD_STREAM:
    case BPK_UNLOAD_STREAM: {
        if (cc->a->nlatent >= 255) cc_error(cc, n, "Too many latent actions (255)");
        data_inputs(cc, n, in, NULL, 3);
        if (n->kind == BPK_LOAD_STREAM) emit(cc, OP_NPARAM, 0, in[1], 0, 0);
        // (2026-11-24) Loaded / Failed / Progress: Completed then Loaded on success, Completed then Failed on failure (a
        // node saved before them has only Completed: as before)
        int po = n->kind == BPK_LOAD_STREAM ? pin_of(n, 1, 0, 0) : -1;
        bool ext = n->kind == BPK_LOAD_STREAM && pin_of(n, 1, 1, 1) >= 0;
        uint32_t info = ext ? emit(cc, OP_LATENT_INFO, po >= 0 ? out_reg(gi, n, po) : 0xFFFF, 0, 0, 0) : 0;
        uint32_t lat = emit(cc, OP_LATENT2, (uint16_t)cc->a->nlatent++, in[0], 0, n->kind == BPK_LOAD_STREAM ? LAT_STREAM_LOAD : LAT_STREAM_UNLOAD);
        cc->a->code[lat].c = here(cc);
        if (!ext) {
            chain(cc, n, 0);
            break;
        }
        uint32_t p1 = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[p1].a = here(cc);
        chain(cc, n, 1);
        cc->a->code[info].b = here(cc);
        uint32_t p2 = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        chain(cc, n, 0);
        cc->a->code[p2].a = here(cc);
        chain(cc, n, 2);
        break;
    }
    case BPK_PRELOAD_LEVEL: { // (2026-11-24) started, the first output at once; Loaded / Failed when it ends, Progress meanwhile
        if (cc->a->nlatent >= 255) cc_error(cc, n, "Too many latent actions (255)");
        int nin = data_inputs(cc, n, in, NULL, BP_MAX_PINS);
        int po = pin_of(n, 1, 0, 0);
        uint16_t preg = po >= 0 ? out_reg(gi, n, po) : 0xFFFF;
        emit_native(cc, n, in, nin, po >= 0 ? preg : 0);
        uint32_t info = emit(cc, OP_LATENT_INFO, preg, 0, 0, 0);
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        uint32_t lat = emit(cc, OP_LATENT2, (uint16_t)cc->a->nlatent++, in[0], 0, LAT_PRELOAD);
        cc->a->code[push].a = here(cc);
        chain(cc, n, 0);
        cc->a->code[lat].c = here(cc);
        chain(cc, n, 1);
        cc->a->code[info].b = here(cc);
        chain(cc, n, 2);
        break;
    }
    case BPK_DISPATCH_CALL: { // the inputs staged, then every bound event runs with them
        uint16_t target = in_op(cc, n, pin_of(n, 0, 0, 0), NULL);
        uint16_t off = 0;
        for (int k = 1;; k++) {
            int pi = pin_of(n, 0, 0, k);
            if (pi < 0) break;
            uint32_t w = whole_width(&n->pins[pi]);
            uint8_t t = n->pins[pi].sub ? n->pins[pi].ptype : n->pins[pi].type;
            emit(cc, OP_ARG, off, in_op(cc, n, pi, NULL), (uint16_t)w, bp_type_stringish(t) || n->pins[pi].cont ? 1 : 0);
            off = (uint16_t)(off + w);
        }
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Event Dispatcher (Details)");
        emit(cc, OP_DISPATCH, target, string_index(cc, n->text), 0, 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_DISPATCH_BIND: case BPK_DISPATCH_UNBIND: case BPK_DISPATCH_UNBIND_ALL: {
        uint16_t target = in_op(cc, n, pin_of(n, 0, 0, 0), NULL);
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Event Dispatcher (Details)");
        const char* ev = n->kind == BPK_DISPATCH_UNBIND_ALL ? NULL : delegate_event(cc, n); // (the red square: that Custom Event)
        emit(cc, OP_DBIND, target, string_index(cc, n->text), (uint16_t)(n->kind - BPK_DISPATCH_BIND), ev ? (uint16_t)(string_index(cc, ev) + 1) : 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_CALL_INTERFACE: { // the arguments staged, the call, the results taken back
        const char* func = n->text2[0] ? n->text2 : n->text;
        uint16_t target = in_op(cc, n, pin_of(n, 0, 0, 0), NULL);
        uint16_t off = 0;
        for (int k = 1;; k++) {
            int pi = pin_of(n, 0, 0, k);
            if (pi < 0) break;
            uint32_t w = whole_width(&n->pins[pi]);
            uint8_t t = n->pins[pi].sub ? n->pins[pi].ptype : n->pins[pi].type;
            emit(cc, OP_ARG, off, in_op(cc, n, pi, NULL), (uint16_t)w, bp_type_stringish(t) || n->pins[pi].cont ? 1 : 0);
            off = (uint16_t)(off + w);
        }
        emit(cc, OP_CALLIFACE, target, string_index(cc, func), 0, 0);
        uint16_t roff = 0;
        for (int k = 0;; k++) {
            int po = pin_of(n, 1, 0, k);
            if (po < 0) break;
            uint32_t w = whole_width(&n->pins[po]);
            emit(cc, OP_TAKERES, out_reg(gi, n, po), (uint16_t)w, roff, 0);
            roff = (uint16_t)(roff + w);
        }
        chain(cc, n, 0);
        break;
    }
    case BPK_CALL_CUSTOM: {
        // the inputs staged (a Custom Event's: its GETARGS reads them), then the event: here a jump to it and back, on
        // another class's Target its instance runs it
        int first = n->text2[0] ? 1 : 0;
        uint16_t off = 0;
        for (int k = first;; k++) {
            int pi = pin_of(n, 0, 0, k);
            if (pi < 0) break;
            uint32_t w = whole_width(&n->pins[pi]);
            uint8_t t = n->pins[pi].sub ? n->pins[pi].ptype : n->pins[pi].type;
            emit(cc, OP_ARG, off, in_op(cc, n, pi, NULL), (uint16_t)w, bp_type_stringish(t) || n->pins[pi].cont ? 1 : 0);
            off = (uint16_t)(off + w);
        }
        if (first) {
            if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the event (Details)");
            emit(cc, OP_CALLEV, in_op(cc, n, pin_of(n, 0, 0, 0), NULL), string_index(cc, n->text), 0, 0);
            chain(cc, n, 0);
            break;
        }
        const BpNode* ev = custom_event_node(cc->a, n->text);
        if (ev && (ev->net & 3)) { // (Unreal's replicated event: the call goes through the network; the first Float, Vector and Text travel)
            uint16_t f = konst_f(cc, 0.0f), vec = konst_f(cc, 0.0f), txt = konst_f(cc, (float)string_index(cc, ""));
            bool hf = false, hv = false, ht = false;
            for (int k = 0;; k++) {
                int pi = pin_of(n, 0, 0, k);
                if (pi < 0) break;
                uint8_t t = n->pins[pi].type;
                if (t == BPT_FLOAT && !hf) f = in_op(cc, n, pi, NULL), hf = true;
                else if (t == BPT_VECTOR && !hv) vec = in_op(cc, n, pi, NULL), hv = true;
                else if (bp_type_stringish(t) && !ht) txt = in_op(cc, n, pi, NULL), ht = true;
            }
            emit(cc, OP_NPARAM, 0, txt, 0, 0);
            emit(cc, OP_NPARAM, 1, konst_f(cc, (ev->net & BP_NET_RELIABLE) ? 1.0f : 0.0f), 0, 0);
            int kk = (ev->net & 3) == BP_NET_MULTICAST ? BPK_NET_RPC_MULTICAST : (ev->net & 3) == BP_NET_SERVER ? BPK_NET_RPC_SERVER : BPK_NET_RPC_OWNER;
            emit(cc, OP_NATIVE + kk, 0, f, vec, string_index(cc, n->text));
            chain(cc, n, 0);
            break;
        }
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        uint32_t jmp = emit(cc, OP_JMP, 0, 0, 0, 0);
        add_fix(cc, jmp, 0, 1, 0, n->text);
        cc->a->code[push].a = here(cc);
        chain(cc, n, 0);
        break;
    }
    case BPK_ADD_MAPPING_CONTEXT:
    case BPK_REMOVE_MAPPING_CONTEXT:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Input Mapping Context (Details)");
        data_inputs(cc, n, in, NULL, 1);
        emit(cc, OP_NATIVE + n->kind, n->kind == BPK_ADD_MAPPING_CONTEXT ? in[0] : 0, 0, asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_PLAY_WIDGET_ANIM: // Target, loops, speed; the animation's name in d
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the animation (Details)");
        data_inputs(cc, n, in, NULL, 3);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], in[2], asset_name_op(cc, n));
        chain(cc, n, 0);
        break;
    case BPK_STOP_WIDGET_ANIM:
        data_inputs(cc, n, in, NULL, 1);
        emit(cc, OP_NATIVE + n->kind, in[0], 0, asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_NET_RPC_SERVER: // Target, Float, Vector; Text, Reliable after them; the event's name in d
    case BPK_NET_RPC_MULTICAST:
    case BPK_NET_RPC_OWNER: {
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Custom Event (Details)");
        int nin = data_inputs(cc, n, in, NULL, 5);
        for (int i = 3; i < nin; i++) emit(cc, OP_NPARAM, (uint16_t)(i - 3), in[i], 0, 0);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], in[2], string_index(cc, n->text));
        chain(cc, n, 0);
        break;
    }
    case BPK_NET_REPLICATE_VAR: // Target; the variable's name in d
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the variable (Details)");
        data_inputs(cc, n, in, NULL, 1);
        emit(cc, OP_NATIVE + n->kind, in[0], 0, 0, string_index(cc, n->text));
        chain(cc, n, 0);
        break;
    case BPK_PLAY_SOUND_2D: case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_2D: case BPK_SPAWN_SOUND_AT: case BPK_SPAWN_SOUND_ATTACHED:
    case BPK_AUDIO_SET_SOUND: case BPK_PUSH_SOUND_MIX: case BPK_POP_SOUND_MIX: case BPK_SET_BASE_SOUND_MIX: case BPK_SET_CLASS_VOLUME: {
        // the asset (Details: its name) as a constant before the inputs; Spawn Sound Attached's bone: a string, last
        if (!n->text[0] && n->kind != BPK_AUDIO_SET_SOUND && !asset_linked(cc, n))
            cc_error(cc, n, n->kind >= BPK_PUSH_SOUND_MIX ? (n->kind == BPK_SET_CLASS_VOLUME ? "Choose the Sound Class (Details)" : "Choose the Sound Mix (Details)") : "Choose the sound (Details)");
        uint16_t ops[BP_MAX_PINS + 2];
        ops[0] = asset_name_reg(cc, n); // (its name: found when it runs, by audio_asset_by_name; the Sound pin's when wired)
        int nin = data_inputs(cc, n, ops + 1, NULL, BP_MAX_PINS);
        if (n->kind == BPK_SPAWN_SOUND_ATTACHED) ops[1 + nin++] = konst_f(cc, (float)string_index(cc, n->text2));
        int po = pin_of(n, 1, 0, 0);
        emit_native(cc, n, ops, nin + 1, po >= 0 ? out_reg(gi, n, po) : 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_PS_SPAWN: case BPK_SPAWN_DECAL: { // the system's / the material's name (Details) as a constant before the inputs
        if (!n->text[0]) cc_error(cc, n, n->kind == BPK_SPAWN_DECAL ? "Choose the Decal Material (Details)" : "Choose the Particle System (Details)");
        uint16_t ops[BP_MAX_PINS + 2];
        ops[0] = asset_name_reg(cc, n);
        int nin = data_inputs(cc, n, ops + 1, NULL, BP_MAX_PINS);
        int po = pin_of(n, 1, 0, 0);
        emit_native(cc, n, ops, nin + 1, po >= 0 ? out_reg(gi, n, po) : 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_PLAY_SEQUENCE: // loop, rate; the sequence in c
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Level Sequence (Details)");
        data_inputs(cc, n, in, NULL, 2);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_STOP_SEQUENCE:
        emit(cc, OP_NATIVE + n->kind, 0, 0, asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_WIDGET_SET_VALUE:
    case BPK_WIDGET_SET_ENABLED:
    case BPK_WIDGET_SET_TEXT:
    case BPK_WIDGET_SET_PERCENT:
    case BPK_WIDGET_SET_VISIBLE:
    case BPK_WIDGET_SET_COLOR:
        if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the widget (Details)");
        data_inputs(cc, n, in, NULL, 2);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_PP_SET_FLOAT:
        if (post_field_index(n->text) < 0) cc_error(cc, n, "Choose the Post Process setting (Details)");
        // fallthrough
    case BPK_MAT_SCALAR:
    case BPK_MAT_VECTOR: // (the name: the third operand)
    // (2026-11-10) the actor's named actions: the name in the third operand too (they fell into the default before,
    // which left it at the class's first text)
    case BPK_PLAY_MONTAGE: case BPK_PLAY_ANIMATION: case BPK_RUN_BT:
    case BPK_BB_SET_FLOAT: case BPK_BB_SET_BOOL: case BPK_BB_SET_VECTOR: case BPK_BB_SET_ACTOR:
        if (n->kind != BPK_MAT_SCALAR && n->kind != BPK_MAT_VECTOR && n->kind != BPK_PP_SET_FLOAT && !n->text[0] && !asset_linked(cc, n))
            cc_error(cc, n, "Choose the %s (Details or its pin)", bp_asset_pin(n->kind));
        data_inputs(cc, n, in, NULL, 2);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], asset_name_op(cc, n), 0);
        chain(cc, n, 0);
        break;
    case BPK_SET_TIMER:
    case BPK_CLEAR_TIMER: {
        const char* ev = delegate_event(cc, n); // (the red square wired: that Custom Event; else the one in Details)
        if (!ev && !n->text[0]) cc_error(cc, n, "Choose the Custom Event (Details) or wire one's red square to Event");
        data_inputs(cc, n, in, NULL, 2);
        emit(cc, OP_NATIVE + n->kind, in[0], in[1], string_index(cc, ev ? ev : n->text), 0);
        chain(cc, n, 0);
        break;
    }
    case BPK_CALL_FUNCTION: {
        GraphInfo* fg = find_function(cc, n->text);
        if (!fg) {
            cc_error(cc, n, "The function \"%s\" doesn't exist", n->text);
            break;
        }
        BpNode* entry = &fg->g->nodes[fg->entry_node];
        // the arguments into the function's entry outputs
        for (int k = 0;; k++) {
            int pi = pin_of(n, 0, 0, k), pe = pin_of(entry, 1, 0, k);
            if (pi < 0 || pe < 0) break;
            emit_move(cc, out_reg(fg, entry, pe), in_op(cc, n, pi, NULL), whole_width(&n->pins[pi]), n->pins[pi].cont);
        }
        uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
        uint32_t jmp = emit(cc, OP_JMP, 0, 0, 0, 0);
        add_fix(cc, jmp, 0, 0, (uint16_t)(fg - cc->graphs), NULL);
        cc->a->code[push].a = here(cc);
        uint16_t res = fg->res_reg;
        for (int k = 0;; k++) { // the results out of it
            int po = pin_of(n, 1, 0, k);
            if (po < 0) break;
            uint16_t w = (uint16_t)whole_width(&n->pins[po]);
            emit_move(cc, out_reg(gi, n, po), res, w, n->pins[po].cont);
            res = (uint16_t)(res + w);
        }
        chain(cc, n, 0);
        break;
    }
    case BPK_FUNC_RETURN: {
        uint16_t res = gi->res_reg;
        for (int k = 0;; k++) {
            int pi = pin_of(n, 0, 0, k);
            if (pi < 0) break;
            uint16_t w = (uint16_t)whole_width(&n->pins[pi]);
            emit_move(cc, res, in_op(cc, n, pi, NULL), w, n->pins[pi].cont);
            res = (uint16_t)(res + w);
        }
        emit(cc, OP_RET, 0, 0, 0, 0);
        break;
    }
    case BPK_SWITCH_ENUM:
    case BPK_SWITCH_INT: {
        data_inputs(cc, n, in, NULL, 1);
        int outs = 0;
        while (pin_of(n, 1, 1, outs) >= 0) outs++;
        int cases = n->kind == BPK_SWITCH_INT ? outs - 1 : outs; // (Switch on Int: the last is Default)
        uint32_t jumps[32];
        for (int k = 0; k < cases && k < 32; k++) jumps[k] = emit(cc, OP_JEQ, in[0], konst_f(cc, (float)k), 0, 0);
        if (n->kind == BPK_SWITCH_INT) chain(cc, n, outs - 1);
        else emit(cc, OP_RET, 0, 0, 0, 0);
        for (int k = 0; k < cases && k < 32; k++) {
            cc->a->code[jumps[k]].c = here(cc);
            chain(cc, n, k);
        }
        break;
    }
    case BPK_MACRO:
    case BPK_TUNNEL_IN:
    case BPK_TUNNEL_OUT:
        cc_error(cc, n, "A macro node outside a macro (or a macro that could not be expanded)");
        break;
    default:
        if (desc->pure || n->kind <= BPK_EVENT_INTERFACE || n->kind == BPK_FUNC_ENTRY || n->kind == BPK_EVENT_END_PLAY || n->kind == BPK_EVENT_TOUCH ||
            n->kind == BPK_EVENT_BEGIN_OVERLAP || n->kind == BPK_EVENT_END_OVERLAP || n->kind == BPK_EVENT_OVERLAPPING || n->kind == BPK_EVENT_DISPATCH) {
            cc_error(cc, n, "\"%s\" can't be executed", desc->name);
            break;
        }
        { // a native action: its inputs, its outputs' registers, then on
            int nin = data_inputs(cc, n, in, NULL, BP_MAX_PINS);
            int po = pin_of(n, 1, 0, 0);
            emit_native(cc, n, in, nin, po >= 0 ? out_reg(gi, n, po) : 0);
            chain(cc, n, 0);
        }
        break;
    }
    cc->cur_src = dbg_prev;
    cc->depth--;
}

static void add_entry(Cc* cc, const BpNode* n, const char* name, uint16_t kind, uint32_t pc) {
    BpAsset* as = cc->a;
    const BpAsset* owner = cc->cur ? cc->cur->owner : NULL;
    bool once = kind == BPK_EVENT_BEGIN_PLAY || kind == BPK_EVENT_TICK || kind == BPK_EVENT_END_PLAY;
    for (uint32_t i = 0; i < as->nentry; i++)
        if (as->entries_code[i].kind == kind && !strcmp(as->entries_code[i].name, name)) {
            if (once && cc->entry_owner[i] == owner) { // the same class's second one: it runs after the first
                if (cc->ndup[i] < 4) cc->dup_pc[i][cc->ndup[i]++] = pc;
                else cc_error(cc, n, "Too many \"%s\" events (5)", name);
                return;
            }
            as->entries_code[i].pc = pc; // a child's event replaces its parent's
            cc->entry_owner[i] = owner, cc->ndup[i] = 0;
            return;
        }
    if (as->nentry >= 64) {
        cc_error(cc, n, "Too many events (64)");
        return;
    }
    cc->entry_owner[as->nentry] = owner, cc->ndup[as->nentry] = 0;
    BpEntry* e = &as->entries_code[as->nentry++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->pc = pc;
    e->kind = kind;
}

// --- Macros: a copy of the graph with each macro node replaced by the macro's nodes (Unreal expands them too) ---

static void clone_add_link(BpGraph* g, uint32_t fn, uint8_t fp, uint32_t tn, uint8_t tp) {
    if (g->nlinks == g->cap_links) {
        uint32_t c = g->cap_links ? g->cap_links * 2 : 64;
        BpLink* nl = realloc(g->links, sizeof(BpLink) * c);
        if (!nl) return;
        g->links = nl, g->cap_links = c;
    }
    g->links[g->nlinks++] = (BpLink){ fn, tn, fp, tp };
}
static BpNode* clone_add_node(BpGraph* g, const BpNode* src) {
    if (g->nnodes == g->cap_nodes) {
        uint32_t c = g->cap_nodes ? g->cap_nodes * 2 : 32;
        BpNode* nn = realloc(g->nodes, sizeof(BpNode) * c);
        if (!nn) return NULL;
        g->nodes = nn, g->cap_nodes = c;
    }
    BpNode* n = &g->nodes[g->nnodes++];
    *n = *src;
    n->id = g->next_id++;
    return n;
}
static int pin_named(const BpNode* n, int out, const char* name) {
    for (int i = 0; i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    return -1;
}

static bool has_macro(const BpGraph* g) {
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == BPK_MACRO || g->nodes[i].kind == BPK_REROUTE) return true; // (reroutes joined in the copy too)
    return false;
}

static BpGraph* expand_macros(Cc* cc, const BpAsset* owner, const BpGraph* src) {
    BpGraph* g = calloc(1, sizeof *g); // compiling, not a frame
    if (!g) return NULL;
    *g = *src;
    g->nodes = malloc(sizeof(BpNode) * (src->nnodes + 1));
    g->links = malloc(sizeof(BpLink) * (src->nlinks + 1));
    if (!g->nodes || !g->links) {
        free(g->nodes), free(g->links), free(g);
        return NULL;
    }
    memcpy(g->nodes, src->nodes, sizeof(BpNode) * src->nnodes);
    memcpy(g->links, src->links, sizeof(BpLink) * src->nlinks);
    g->cap_nodes = src->nnodes + 1, g->cap_links = src->nlinks + 1;
    if (g->next_id < 1000000u) g->next_id = 1000000u; // the copies' ids apart from the graph's own
    for (int round = 0; round < 64 && !cc->fail; round++) {
        uint32_t mi = UINT32_MAX;
        for (uint32_t i = 0; i < g->nnodes && mi == UINT32_MAX; i++)
            if (g->nodes[i].kind == BPK_MACRO) mi = i;
        if (mi == UINT32_MAX) break;
        BpNode m = g->nodes[mi];
        const BpGraph* mg = bp_find_macro(owner, m.text, m.text2);
        if (!mg) {
            cc_error(cc, &m, "The macro \"%s\" doesn't exist", m.text);
            break;
        }
        uint32_t* idmap = calloc(mg->nnodes + 1, sizeof *idmap);
        if (!idmap) break;
        uint32_t tin = 0, tout = 0;
        for (uint32_t i = 0; i < mg->nnodes; i++) {
            const BpNode* q = &mg->nodes[i];
            if (q->kind == BPK_TUNNEL_IN) tin = q->id;
            else if (q->kind == BPK_TUNNEL_OUT) tout = q->id;
            else {
                BpNode* c = clone_add_node(g, q);
                if (c) idmap[i] = c->id;
            }
        }
        // (the nodes array may have moved: the macro node is found again by id)
        uint32_t nl0 = g->nlinks;
        for (uint32_t li = 0; li < mg->nlinks; li++) {
            const BpLink* l = &mg->links[li];
            const BpNode* fq = NULL;
            const BpNode* tq = NULL;
            uint32_t fmap = 0, tmap = 0;
            for (uint32_t i = 0; i < mg->nnodes; i++) {
                if (mg->nodes[i].id == l->from_node) fq = &mg->nodes[i], fmap = idmap[i];
                if (mg->nodes[i].id == l->to_node) tq = &mg->nodes[i], tmap = idmap[i];
            }
            if (!fq || !tq) continue;
            bool from_in = fq->id == tin, to_out = tq->id == tout;
            if (!from_in && !to_out) {
                clone_add_link(g, fmap, l->from_pin, tmap, l->to_pin);
                continue;
            }
            int mip = from_in ? pin_named(&m, 0, fq->pins[l->from_pin].name) : -1; // the macro node's input
            int mop = to_out ? pin_named(&m, 1, tq->pins[l->to_pin].name) : -1;   // ... and output
            if (from_in && to_out) { // straight through: every wire in to every wire out
                for (uint32_t a = 0; a < nl0; a++)
                    if (g->links[a].to_node == m.id && g->links[a].to_pin == mip)
                        for (uint32_t b = 0; b < nl0; b++)
                            if (g->links[b].from_node == m.id && g->links[b].from_pin == mop)
                                clone_add_link(g, g->links[a].from_node, g->links[a].from_pin, g->links[b].to_node, g->links[b].to_pin);
            } else if (from_in) { // what fed the macro's input now feeds the node inside
                bool wired = false;
                for (uint32_t a = 0; a < nl0; a++)
                    if (g->links[a].to_node == m.id && g->links[a].to_pin == mip) {
                        clone_add_link(g, g->links[a].from_node, g->links[a].from_pin, tmap, l->to_pin);
                        wired = true;
                    }
                if (!wired && mip >= 0) // its literal
                    for (uint32_t i = 0; i < g->nnodes; i++)
                        if (g->nodes[i].id == tmap && l->to_pin < g->nodes[i].npins) {
                            g->nodes[i].pins[l->to_pin].lit = m.pins[mip].lit;
                            memcpy(g->nodes[i].pins[l->to_pin].slit, m.pins[mip].slit, sizeof m.pins[mip].slit);
                        }
            } else { // what the node inside gives goes where the macro's output went
                for (uint32_t b = 0; b < nl0; b++)
                    if (g->links[b].from_node == m.id && g->links[b].from_pin == mop)
                        clone_add_link(g, fmap, l->from_pin, g->links[b].to_node, g->links[b].to_pin);
            }
        }
        free(idmap);
        // the macro node and its wires go
        for (uint32_t i = 0; i < g->nlinks;) {
            if (g->links[i].from_node == m.id || g->links[i].to_node == m.id) g->links[i] = g->links[--g->nlinks];
            else i++;
        }
        for (uint32_t i = 0; i < g->nnodes; i++)
            if (g->nodes[i].id == m.id) {
                g->nodes[i] = g->nodes[--g->nnodes];
                break;
            }
        if (round == 63) cc_error(cc, NULL, "The macros go too deep (a macro inside itself?)");
    }
    // Reroute nodes: every wire into a knot goes on to every wire out of it, and the knot leaves (knots into knots: rounds)
    for (uint32_t round = 0; round < 4096; round++) {
        uint32_t k = UINT32_MAX;
        for (uint32_t i = 0; i < g->nnodes && k == UINT32_MAX; i++)
            if (g->nodes[i].kind == BPK_REROUTE) k = i;
        if (k == UINT32_MAX) break;
        uint32_t id = g->nodes[k].id, nl0 = g->nlinks;
        for (uint32_t a = 0; a < nl0; a++)
            if (g->links[a].to_node == id)
                for (uint32_t b = 0; b < nl0; b++)
                    if (g->links[b].from_node == id) clone_add_link(g, g->links[a].from_node, g->links[a].from_pin, g->links[b].to_node, g->links[b].to_pin);
        for (uint32_t i = 0; i < g->nlinks;)
            if (g->links[i].from_node == id || g->links[i].to_node == id) g->links[i] = g->links[--g->nlinks];
            else i++;
        g->nodes[k] = g->nodes[--g->nnodes];
    }
    return g;
}

uint32_t g_bp_compiles; // (every compile: the editor's Construction Scripts run again with the new code)
static void warn_recompile_playing(const BpAsset* a); // (below the VM's state)
static BpOp dbg_hit(uint32_t ii, uint32_t pc, const uint32_t* stack, uint32_t sp); // (the debugger, at the end)
bool bp_compile(BpAsset* a) {
    if (bp_debug_paused()) return a->compiled; // (paused at a breakpoint: its code is running; compiled after the Play)
    g_bp_compiles++;
    bp_jit_free(a), a->jit_failed = false; // (its native code is the old bytecode's)
    if (a->kind != BPA_CLASS) {
        a->compiled = true;
        return true;
    }
    warn_recompile_playing(a);
    Cc* cc = calloc(1, sizeof *cc); // compiling, not a frame
    if (!cc) return false;
    cc->a = a;
    bp_debug_forget(a); // (its breakpoints' ops are the old code's: put back after)
    a->ndbg_nodes = a->ndbg_sites = a->ndbg_vars = 0;
    a->ncode = a->nconsts = a->nstrings = a->nentry = a->nlatent = a->narr = 0;
    a->ntlc = 0;
    a->error[0] = 0;
    a->error_node = 0;
    a->compiled = false;
    // the parents (the root first)
    BpAsset* chain_rev[8];
    uint32_t nrev = 0;
    for (BpAsset* p = a; p && nrev < 8; p = bp_find_name(BPA_CLASS, p->parent)) {
        bool loop = false;
        for (uint32_t i = 0; i < nrev; i++) loop |= chain_rev[i] == p;
        if (loop) {
            cc_error(cc, NULL, "The class's parents make a loop");
            break;
        }
        chain_rev[nrev++] = p;
    }
    for (uint32_t i = 0; i < nrev; i++) cc->chain[i] = chain_rev[nrev - 1 - i];
    cc->nchain = nrev;
    alloc_regs(cc, 1); // self
    // the variables
    for (uint32_t c = 0; c < cc->nchain; c++) {
        BpAsset* as = cc->chain[c];
        for (uint32_t i = 0; i < as->nvars && cc->nvars < 256; i++) {
            const BpVar* v = &as->vars[i];
            uint32_t w = bp_var_width(v);
            uint16_t slot = alloc_regs(cc, w);
            cc->vars[cc->nvars].v = v;
            cc->vars[cc->nvars++].slot = slot;
            if (as == a) a->var_slot[i] = slot;
            if (cc->fail) break;
            if (v->cont) arr_slot(cc, slot, v->cont, v->type, v->ref, v->vtype, v);
            else if (bp_type_stringish(v->type)) a->init[slot].v[0] = (float)string_index(cc, v->sdef);
            else if (v->type == BPT_TRANSFORM) a->init[slot] = v->def, a->init[slot + 1] = v->def3[0], a->init[slot + 2] = v->def3[1];
            else if (v->type == BPT_CLASS) {
                const BpAsset* k = v->sdef[0] ? bp_find_name(BPA_CLASS, v->sdef) : NULL;
                if (k) memcpy(&a->init[slot].v[0], &k->hash, 4), a->init[slot].v[1] = 1.0f;
            } else if (v->type == BPT_STRUCT) { // its fields start at the structure's Default Values (its editor)
                const BpAsset* s = bp_find_name(BPA_STRUCT, v->ref);
                uint32_t off = 0;
                for (uint32_t f = 0; s && f < s->nfields; f++) {
                    const BpVar* fv = &s->fields[f];
                    uint32_t fw = bp_var_width(fv);
                    if (!fv->cont && off + fw <= w) {
                        if (bp_type_stringish(fv->type)) a->init[slot + off].v[0] = (float)string_index(cc, fv->sdef);
                        else if (fv->type == BPT_TRANSFORM) a->init[slot + off] = fv->def, a->init[slot + off + 1] = fv->def3[0], a->init[slot + off + 2] = fv->def3[1];
                        else if (fv->type != BPT_STRUCT && fv->type != BPT_ACTOR && fv->type != BPT_OBJECT && fv->type != BPT_CLASS) a->init[slot + off] = fv->def;
                    }
                    off += fw;
                }
            } else if (v->type != BPT_ACTOR && v->type != BPT_OBJECT) a->init[slot] = v->def;
        }
    }
    // every graph's registers (calls between them need each other's); macros expanded in copies first
    for (uint32_t c = 0; c < cc->nchain && !cc->fail; c++) {
        BpAsset* as = cc->chain[c];
        for (uint32_t g = 0; g < as->ngraphs; g++) {
            if (as->graphs[g].gkind == BPG_MACRO || as->graphs[g].gkind == BPG_DISPATCHER || as->graphs[g].gkind == BPG_EVENT) continue; // (macros: only where they are used; dispatchers, events: a signature)
            GraphInfo* gi = &cc->graphs[cc->ngraphs++];
            gi->owner = as;
            gi->g = &as->graphs[g];
            gi->gidx = (uint16_t)g;
            if (has_macro(gi->g)) {
                BpGraph* x = expand_macros(cc, as, gi->g);
                if (x) gi->g = x, gi->expanded = true;
            }
            gi->entry_pc = -1;
            gi->entry_node = -1;
            uint32_t nn = gi->g->nnodes ? gi->g->nnodes : 1;
            gi->out_reg = calloc(nn, sizeof *gi->out_reg);
            gi->exec_pc = malloc(nn * 8 * sizeof *gi->exec_pc);
            gi->state = calloc(nn, sizeof *gi->state);
            gi->dbg = calloc(nn, sizeof *gi->dbg);
            if (!gi->out_reg || !gi->exec_pc || !gi->state) {
                cc_error(cc, NULL, "Out of memory");
                continue;
            }
            for (uint32_t i = 0; i < nn * 8; i++) gi->exec_pc[i] = -1;
            for (uint32_t i = 0; i < gi->g->nnodes; i++) {
                BpNode* n = &gi->g->nodes[i];
                gi->out_reg[i] = alloc_regs(cc, outs_width(n));
                for (int p = 0; p < n->npins; p++) // the outputs that hold containers (each instance makes its own)
                    if (n->pins[p].out && n->pins[p].cont && !n->pins[p].sub)
                        arr_slot(cc, out_reg(gi, n, p), n->pins[p].cont, n->pins[p].type, n->pins[p].ref, n->pins[p].vtype, NULL);
                if (n->kind == BPK_FUNC_ENTRY && g > 0) gi->entry_node = (int32_t)i;
            }
            uint32_t rw = 0;
            for (uint8_t i = 0; i < gi->g->nresults; i++) rw += bp_var_width(&gi->g->results[i]);
            gi->res_reg = alloc_regs(cc, rw ? rw : 1);
            for (uint8_t i = 0, off = 0; i < gi->g->nresults; off = (uint8_t)(off + bp_var_width(&gi->g->results[i])), i++)
                if (gi->g->results[i].cont)
                    arr_slot(cc, (uint16_t)(gi->res_reg + off), gi->g->results[i].cont, gi->g->results[i].type, gi->g->results[i].ref,
                             gi->g->results[i].vtype, NULL);
        }
    }
    // the functions, then the events
    for (int pass = 0; pass < 2 && !cc->fail; pass++)
        for (uint32_t i = 0; i < cc->ngraphs && !cc->fail; i++) {
            GraphInfo* gi = &cc->graphs[i];
            cc->cur = gi;
            if (pass == 0) {
                if (gi->entry_node < 0) continue;
                gi->entry_pc = here(cc);
                cc->cur_src = dbg_node(cc, &gi->g->nodes[gi->entry_node]);
                chain(cc, &gi->g->nodes[gi->entry_node], 0);
                dbg_site(cc, cc->cur_src, (uint32_t)gi->entry_pc, 0); // (after its first node's: a step shows that one)
                cc->cur_src = 0;
                if (gi->owner == a && !strcmp(gi->g->name, "ConstructionScript")) // (also an event: run when an actor is made)
                    add_entry(cc, &gi->g->nodes[gi->entry_node], "ConstructionScript", BPK_FUNC_ENTRY, gi->entry_pc);
                continue;
            }
            // a function that implements an interface function (with outputs): its entry for the callers
            const char* iface = gi->entry_node >= 0 ? (gi->g->iface[0] ? gi->g->iface : bp_interface_of(a, gi->g->name)) : NULL;
            if (iface && gi->owner == a) {
                BpNode* en = &gi->g->nodes[gi->entry_node];
                uint32_t pc = here(cc), w = outs_width(en), rw = 0;
                for (uint8_t r = 0; r < gi->g->nresults; r++) rw += bp_var_width(&gi->g->results[r]);
                if (w) emit(cc, OP_GETARGS, gi->out_reg[gi->entry_node], (uint16_t)w, 0, 0);
                uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
                emit(cc, OP_JMP, (uint16_t)gi->entry_pc, 0, 0, 0);
                cc->a->code[push].a = here(cc);
                if (rw) emit(cc, OP_PUTRES, gi->res_reg, (uint16_t)rw, 0, 0);
                emit(cc, OP_RET, 0, 0, 0, 0);
                add_entry(cc, en, gi->g->name, BPK_EVENT_INTERFACE, pc);
            }
            for (uint32_t k = 0; k < gi->g->nnodes && !cc->fail; k++) {
                BpNode* n = &gi->g->nodes[k];
                uint32_t pc = here(cc);
                // (the debugger: an event's ops are its node's, its start a breakpoint's place, after its first node's)
                cc->cur_src = dbg_node(cc, n); // (a node that emits nothing here gets no site)
                switch (n->kind) {
                case BPK_TIMELINE: { // (2026-11-16) its outputs' code: Update, Finished, each Event track; its registers
                    int sl = tl_slot(cc, n);
                    if (sl < 0) break;
                    struct BpTlCode* t = &cc->a->tlc[sl];
                    int pd = pin_of(n, 1, 0, 0);
                    if (pd >= 0) t->reg_dir = out_reg(gi, n, pd);
                    for (uint8_t j = 0, nd = 1, ne = 2; j < t->tl->ntracks && j < 6; j++) {
                        if (t->tl->tracks[j].type == TLT_EVENT) {
                            t->pc_event[j] = (int32_t)here(cc);
                            chain(cc, n, ne++);
                        } else {
                            int pp = pin_of(n, 1, 0, nd++);
                            if (pp >= 0) t->reg_track[j] = out_reg(gi, n, pp);
                        }
                    }
                    t->pc_update = (int32_t)here(cc);
                    chain(cc, n, 0);
                    t->pc_finished = (int32_t)here(cc);
                    chain(cc, n, 1);
                    break;
                }
                case BPK_EVENT_BEGIN_PLAY:
                    chain(cc, n, 0);
                    add_entry(cc, n, "BeginPlay", BPK_EVENT_BEGIN_PLAY, pc);
                    break;
                case BPK_EVENT_END_PLAY:
                    chain(cc, n, 0);
                    add_entry(cc, n, "EndPlay", BPK_EVENT_END_PLAY, pc);
                    break;
                case BPK_EVENT_DISPATCH: { // the call's arguments, then the chain
                    uint32_t w = outs_width(n);
                    if (w) emit(cc, OP_GETARGS, gi->out_reg[k], (uint16_t)w, 0, 0);
                    chain(cc, n, 0);
                    add_entry(cc, n, n->text, BPK_EVENT_DISPATCH, pc);
                    break;
                }
                // the network's and Steam's events (online.c through bp_online_events): their outputs, then the chain
                case BPK_EVENT_NET_PLAYER_JOINED: case BPK_EVENT_NET_PLAYER_LEFT: case BPK_EVENT_NET_SESSION_CREATED:
                case BPK_EVENT_NET_SESSIONS_FOUND: case BPK_EVENT_NET_JOINED: case BPK_EVENT_NET_ERROR: case BPK_EVENT_NET_CHAT:
                case BPK_EVENT_STEAM_LEADERBOARD: case BPK_EVENT_STEAM_OVERLAY: case BPK_EVENT_NET_SESSION_UPDATED:
                case BPK_EVENT_NET_SESSION_STATE: case BPK_EVENT_NET_INVITE: case BPK_EVENT_NET_JOIN_REFUSED:
                case BPK_EVENT_GAME_SAVED: case BPK_EVENT_GAME_LOADED: case BPK_EVENT_CHECKPOINT: // (save_progress.c)
                case BPK_EVENT_INV_CHANGED: case BPK_EVENT_ITEM_USED: case BPK_EVENT_ITEM_EQUIPPED: case BPK_EVENT_ITEM_UNEQUIPPED: // (inventory.c)
                case BPK_EVENT_IA_INTERACT: case BPK_EVENT_IA_FOCUS_BEGIN: case BPK_EVENT_IA_FOCUS_END: case BPK_EVENT_IA_HOLD: // (interaction.c)
                case BPK_EVENT_DLG_LINE: case BPK_EVENT_DLG_CHOICE: case BPK_EVENT_DLG_END: // (dialogue.c through dlg_event)
                case BPK_EVENT_QST_STARTED: case BPK_EVENT_QST_OBJ_UPDATED: case BPK_EVENT_QST_OBJ_COMPLETED: case BPK_EVENT_QST_COMPLETED:
                case BPK_EVENT_QST_FAILED: // (quest.c through quest_event_hook)
                case BPK_EVENT_MATCH_PHASE: case BPK_EVENT_SCORE_CHANGED: case BPK_EVENT_MATCH_WON: case BPK_EVENT_MATCH_LOST: // (game_rules.c)
                case BPK_EVENT_ROUND_STARTED: case BPK_EVENT_ROUND_ENDED: case BPK_EVENT_PLAYER_RESPAWNED: case BPK_EVENT_TIMER_EXPIRED:
                case BPK_EVENT_ABIL_ACTIVATE: case BPK_EVENT_ABIL_ACTIVATED: case BPK_EVENT_ABIL_ENDED: case BPK_EVENT_GE_APPLIED:
                case BPK_EVENT_GE_REMOVED: case BPK_EVENT_ATTR_CHANGED: // (ability.c through bp_fire_actor_event)
                case BPK_EVENT_VR_GRABBED: case BPK_EVENT_VR_RELEASED: case BPK_EVENT_VR_TELEPORTED: // (Q25: vr_pawn.c)
                case BPK_EVENT_MR_ROOM_LOADED: // (Q25b: xr_mr.c)
                case BPK_EVENT_MONTAGE_WINDOW: case BPK_EVENT_MONTAGE_HIT: // (2026-11-23: anim_combo.inc)
                case BPK_EVENT_LIGHTING_BUILT: { // (Q16: runtime_lighting.c)
                    uint32_t w = outs_width(n);
                    if (w) emit(cc, OP_GETARGS, gi->out_reg[k], (uint16_t)w, 0, 0);
                    chain(cc, n, 0);
                    add_entry(cc, n, "", n->kind, pc);
                    break;
                }
                // the actor's Health events (health.h through health_event): their outputs, then the chain
                case BPK_EVENT_ANY_DAMAGE: case BPK_EVENT_HEALED: case BPK_EVENT_DEATH: case BPK_EVENT_SHIELD_BROKEN: {
                    uint32_t w = outs_width(n);
                    if (w) emit(cc, OP_GETARGS, gi->out_reg[k], (uint16_t)w, 0, 0);
                    chain(cc, n, 0);
                    add_entry(cc, n, "", n->kind, pc);
                    break;
                }
                case BPK_EVENT_BEGIN_OVERLAP: case BPK_EVENT_END_OVERLAP: case BPK_EVENT_OVERLAPPING: { // Other Actor, then the chain
                    int po = pin_of(n, 1, 0, 0);
                    if (po >= 0) emit(cc, OP_GETARGS, out_reg(gi, n, po), 1, 0, 0);
                    chain(cc, n, 0);
                    add_entry(cc, n, n->text, n->kind, pc); // (a name: the Level Blueprint's, for that actor)
                    break;
                }
                case BPK_EVENT_TICK:
                case BPK_EVENT_UPDATE_ANIM: // (an Animation Blueprint's frame: its Tick)
                    emit(cc, OP_DELTA, out_reg(gi, n, pin_of(n, 1, 0, 0)), 0, 0, 0);
                    chain(cc, n, 0);
                    add_entry(cc, n, "Tick", BPK_EVENT_TICK, pc);
                    break;
                case BPK_EVENT_TOUCH: { // two entries (Pressed, Released): each loads the finger's place and index, then its chain
                    int po = pin_of(n, 1, 0, 0);
                    if (po >= 0) emit(cc, OP_NATIVE + BPK_EVENT_TOUCH, 0, 0, 0, out_reg(gi, n, po));
                    chain(cc, n, 0);
                    add_entry(cc, n, "Touch", BPK_EVENT_TOUCH, pc);
                    pc = here(cc);
                    if (po >= 0) emit(cc, OP_NATIVE + BPK_EVENT_TOUCH, 0, 0, 0, out_reg(gi, n, po));
                    chain(cc, n, 1);
                    add_entry(cc, n, "Touch", BPK_EVENT_TOUCH | 0x100, pc);
                    break;
                }
                case BPK_EVENT_KEY:
                    if (bp_key_code(n->text) < 0) cc_error(cc, n, "Unknown key \"%s\"", n->text);
                    chain(cc, n, 0);
                    add_entry(cc, n, n->text, BPK_EVENT_KEY, pc);
                    pc = here(cc);
                    chain(cc, n, 1);
                    add_entry(cc, n, n->text, BPK_EVENT_KEY | 0x100, pc);
                    break;
                case BPK_EVENT_INPUT_ACTION: // five entries (one per event): each loads the action's value, then its chain
                    if (!n->text[0] && !asset_linked(cc, n)) cc_error(cc, n, "Choose the Input Action (Details)");
                    for (int ev = 0; ev < 5 && !cc->fail; ev++) {
                        uint32_t epc = here(cc);
                        int po = pin_of(n, 1, 0, 0);
                        if (po >= 0) emit(cc, OP_NATIVE + BPK_GET_ACTION_VALUE, 0, 1, string_index(cc, n->text), out_reg(gi, n, po));
                        chain(cc, n, ev);
                        add_entry(cc, n, n->text, (uint16_t)(BPK_EVENT_INPUT_ACTION | (ev + 1) << 12), epc);
                    }
                    break;
                case BPK_EVENT_CUSTOM:
                case BPK_EVENT_INTERFACE: {
                    if (!n->text[0]) cc_error(cc, n, "The event has no name");
                    uint32_t w = outs_width(n);
                    if (w) emit(cc, OP_GETARGS, gi->out_reg[k], (uint16_t)w, 0, 0); // the call's arguments (an interface's, a Custom Event's inputs)
                    chain(cc, n, 0);
                    add_entry(cc, n, n->kind == BPK_EVENT_INTERFACE && n->text2[0] && !bp_interface_function(n->text2, n->text) ? n->text2 : n->text,
                              n->kind, pc);
                    break;
                }
                default: break;
                }
                if (cc->cur_src && here(cc) > pc) dbg_site(cc, cc->cur_src, pc, 0);
                cc->cur_src = 0;
            }
        }
    // a repeated BeginPlay / Tick / EndPlay: its entry becomes a stub calling each one in turn (a Delay in one stops
    // the rest for that run, like the chain after a Sequence's pin)
    for (uint32_t i = 0; i < a->nentry && !cc->fail; i++) {
        if (!cc->ndup[i]) continue;
        uint32_t stub = here(cc);
        for (int k = -1; k < (int)cc->ndup[i]; k++) {
            uint32_t push = emit(cc, OP_PUSHRET, 0, 0, 0, 0);
            emit(cc, OP_JMP, (uint16_t)(k < 0 ? a->entries_code[i].pc : cc->dup_pc[i][k]), 0, 0, 0);
            a->code[push].a = (uint16_t)here(cc);
        }
        emit(cc, OP_RET, 0, 0, 0, 0);
        a->entries_code[i].pc = stub;
    }
    for (uint32_t i = 0; i < cc->nfix && !cc->fail; i++) {
        Fix* f = &cc->fix[i];
        int32_t pc = -1;
        if (f->kind == 0) pc = cc->graphs[f->gi].entry_pc;
        else
            for (uint32_t e = 0; e < a->nentry; e++)
                if (a->entries_code[e].kind == BPK_EVENT_CUSTOM && !strcmp(a->entries_code[e].name, f->name)) pc = (int32_t)a->entries_code[e].pc;
        if (pc < 0) cc_error(cc, NULL, "\"Call %s\": no such event", f->name);
        else if (f->field == 0) a->code[f->op].a = (uint16_t)pc;
        else a->code[f->op].b = (uint16_t)pc;
    }
    emit(cc, OP_END, 0, 0, 0, 0);
    // the variables' registers for the debugger's Watch (the parents' first, like the slots)
    free(a->dbg_vars);
    a->dbg_vars = cc->nvars ? malloc(sizeof(BpDbgVar) * cc->nvars) : NULL;
    for (uint32_t i = 0; a->dbg_vars && i < cc->nvars; i++) {
        const BpAsset* ow = NULL;
        for (uint32_t c = 0; c < cc->nchain && !ow; c++)
            if (cc->vars[i].v >= cc->chain[c]->vars && cc->vars[i].v < cc->chain[c]->vars + cc->chain[c]->nvars) ow = cc->chain[c];
        if (ow) a->dbg_vars[a->ndbg_vars++] = (BpDbgVar){ ow, (uint16_t)(cc->vars[i].v - ow->vars), cc->vars[i].slot };
    }
    for (uint32_t i = 0; i < cc->ngraphs; i++) {
        free(cc->graphs[i].dbg);
        free(cc->graphs[i].out_reg);
        free(cc->graphs[i].exec_pc);
        free(cc->graphs[i].state);
        if (cc->graphs[i].expanded) {
            free(cc->graphs[i].g->nodes), free(cc->graphs[i].g->links);
            free(cc->graphs[i].g);
        }
    }
    a->nslots = cc->nregs;
    bool ok = !cc->fail;
    free(cc);
    a->compiled = ok;
    if (ok) bp_debug_compiled(a); // (its breakpoints into the new code)
    return ok;
}

// --- Keys and axes ---

int bp_key_code(const char* s) {
    if (!s || !s[0]) return -1;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    static const struct { const char* n; int k; } names[] = {
        { "Space", 32 }, { "SpaceBar", 32 }, { "Escape", 256 }, { "Enter", 257 }, { "Tab", 258 }, { "BackSpace", 259 },
        { "Right", 262 }, { "Left", 263 }, { "Down", 264 }, { "Up", 265 }, { "Left Shift", 340 }, { "LeftShift", 340 },
        { "Left Ctrl", 341 }, { "LeftControl", 341 }, { "Left Alt", 342 }, { "LeftAlt", 342 },
        { "Left Mouse Button", 500 }, { "LeftMouseButton", 500 }, { "Right Mouse Button", 501 }, { "RightMouseButton", 501 },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(names[i].n, s)) return names[i].k;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return 289 + n;
    }
    return -1;
}

int bp_axis_index(const char* s) {
    static const char* const n[] = { "MoveForward", "MoveRight", "Turn", "LookUp" };
    for (int i = 0; i < 4; i++)
        if (s && !_stricmp(s, n[i])) return i;
    return -1;
}

// --- Runtime ---

typedef struct Inst {
    Entity e;
    BpAsset* cls;
    BpValue* r;
    float* lat_t;
    uint32_t* lat_pc; // resume pc + 1 (0: not waiting)
    struct TlState { float t, dir; uint8_t playing, pad[3]; }* tls; // (2026-11-16) its Timelines' times (cls->ntlc)
    int32_t tick;     // Tick's pc (-1: none)
    uint32_t alive;
    float prof_ms;    // the Debug Viewer's measure (bp_profile): its events this frame
    uint32_t prof_ops;
    uint32_t paused_tick; // ticks (its Delays, timers, keys) while the game is paused: Tick When Paused, a widget
} Inst;

// A collision box (mn, mx first: bp_world_boxes copies those 6 floats). moving: it moved since play began (an actor with
// physics, a Movable platform): out of the grid, in the big list, its box refreshed every frame (track_boxes); d its
// last move (a Character standing on it goes along); e its entity (a body a Character pushes)
// owner: the actor a component's box belongs to (ENTITY_NONE: e itself; a destroyed actor's boxes go with it, box_sweep);
// dyaw, piv: the yaw it turned by in its last move and its entity's place after it (a turning platform turns its rider)
typedef struct Box { float mn[3], mx[3]; uint32_t moving; Entity e; float d[3]; Entity owner; float dyaw, piv[3]; } Box;
// A box that may move: its entity and its shape in the entity's space (center, half extents); loc, yaw: the entity's
// place and yaw when last read (its moves: the carry of whoever stands on it)
typedef struct BoxTrack { uint32_t box; Entity e; float c[3], h[3]; float loc[3], yaw; } BoxTrack;
typedef struct BpTimer { uint32_t inst, pc; float period, t; bool loop; } BpTimer;
// (2026-11-24) info: the node has Loaded / Failed / Progress (fail_pc: where Failed goes on, prog_reg: its Progress
// register, 0xFFFF none); name: a Preload Level's level
typedef struct BpStream { uint32_t inst, req; uint16_t slot, fail_pc, prog_reg; bool info; char name[96]; } BpStream;
#define BP_REQ_LIGHTING 0xFFFFFFF0u // (Q16) a Build Lighting (Async) waiting for the bake (not a streamed level's request)
#define BP_REQ_PRELOAD 0xFFFFFFF1u  // (2026-11-24) a Preload Level waiting for its level (level_preload_state)

#define MAX_INST (1u << 16)
#define GRID_BUCKETS 65536u
#define GRID_CELL 1000.0f

static struct {
    bool playing, mem_ready;
    bool editor_cs;   // the editor runs a Construction Script (bp_construct_in_editor): only its own actor changes
    Entity cs_actor;
    World* w;
    ReArena mem;
    ReVmBuf insts;
    uint32_t ninst;
    float dt;
    double time;
    float axes[4];
    uint8_t keys[512];
    // the pointer (Get Mouse Position) and the fingers (Event Touch, bp_touch_update): this frame's, the event's
    float cursor[2];
    bool cursor_on;
    struct { int id; float x, y; uint8_t state; } fingers[10]; // (state 1: seen once, waiting a frame; 2: Pressed fired)
    float touch_xy[2];
    int touch_finger;
    Entity player;
    struct { char text[160]; double t; } msg[16];
    uint32_t nmsg;
    BpStats stats;
    bool profile;        // the Debug Viewer is open: each instance's time measured (bp_profile)
    double prof_char_ms; // the Characters' movement this frame
    uint32_t ops;
    uint64_t rng;
    int depth;
    // the collision boxes (the actors with a mesh when play started) in a grid of 10 m cells; big ones apart
    ReVmBuf boxes, cells, refs, big, track;
    uint32_t nboxes, nbig, ntrack;
    uint32_t play_frames; // (2026-11-19) the play's ticks (scene_query.c refreshes its world once a frame)
    uint32_t box_moves, pushes; // (the run's: tracked boxes refreshed after moving, bodies pushed by a Character; the tests)
    bool boxes_dirty; // the boxes made again at the next tick (procgen.c's facilities, the origin moved: bp_boxes_changed)
    bool boxes_sweep; // an actor was destroyed while playing: its boxes emptied at the next tick (a picked coin left an invisible wall)
    bool boxes_built; // (build_boxes ran this play: a spawned Character before any box makes them)
    // the actors spawned while playing: their boxes added at the next tick, into a grid of their own (chains per cell:
    // xhead the first ref + 1, xref the box and the next ref + 1) without making the level's again
    Entity spawned[4096];
    uint32_t nspawned;
    ReVmBuf xhead, xref;
    uint32_t nxref, spawn_boxes; // (spawn_boxes: the run's boxes added that way; the tests)
    uint32_t platform_turns;     // (the run's: frames a Character was turned by the platform under it; the tests)
    double box_ms;               // (the last add / sweep / rebuild of the boxes: the tests' measure)
    bool characters;
    // the strings made while playing, the containers, the staged arguments, the natives' extra inputs (bp_vm runtime)
    ReVmBuf sdata, soff, sht, arrs, adata;
    uint32_t nstr, narr;
    size_t sused, aused;
    BpValue stage[64];
    BpValue np[BP_MAX_NPARAM];
    // (Expose on Spawn) the values the next spawn gives its actor before its Construction Script and BeginPlay
    struct { uint32_t hash; BpValue v; bool str; char text[128]; } sp[16];
    uint32_t nsp;
    const BpAsset* sp_cls;
    BpTimer timers[256];
    uint32_t ntimers;
    BpLevelHooks hooks;
    BpStream streams[64];
    uint32_t nstreams;
    struct { uint16_t prog_reg, fail_pc; bool set; } li; // (2026-11-24) OP_LATENT_INFO's, for the next OP_LATENT2
    // the runs going on (the debugger's call stack, bp_debug.h): each depth's instance and the op it is at when it calls
    // deeper (a native, another actor's event, an interface)
    struct { uint32_t inst; const BpOp* op; } fr[4];
} V;
static bool g_dbg_block; // (paused at a breakpoint: the runs asked meanwhile from outside the VM's fiber don't run)

void bp_set_level_hooks(const BpLevelHooks* h) { V.hooks = h ? *h : (BpLevelHooks){ 0 }; }

static Inst* insts(void) { return (Inst*)V.insts.base; }

// (2026-11-24, the MCP server's run_script / play: mcp_tools.c) a sink that also gets every message (main thread)
static void (*g_msg_sink)(const char* text, void* user);
static void* g_msg_sink_user;
void bp_set_message_sink(void (*fn)(const char* text, void* user), void* user) { g_msg_sink = fn, g_msg_sink_user = user; }

static void message(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_msg_sink) g_msg_sink(buf, g_msg_sink_user);
    uint32_t i = V.nmsg++ % 16;
    snprintf(V.msg[i].text, sizeof V.msg[i].text, "%s", buf);
    V.msg[i].t = V.time;
    printf("[Blueprint] %s\n", buf);
}

uint32_t bp_messages(const char** lines, float* ages, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < 16 && k < V.nmsg && n < max; k++) {
        uint32_t i = (V.nmsg - 1 - k) % 16;
        float age = (float)(V.time - V.msg[i].t);
        if (age > 5.0f) continue;
        lines[n] = V.msg[i].text;
        ages[n++] = age;
    }
    return n;
}

const BpStats* bp_stats(void) { return &V.stats; }

static Inst* inst_of(World* w, Entity e); // (below)
static int32_t find_entry(const BpAsset* a, uint16_t kind, const char* name) {
    for (uint32_t i = 0; i < a->nentry; i++)
        if (a->entries_code[i].kind == kind && (!name || !strcmp(a->entries_code[i].name, name))) return (int32_t)a->entries_code[i].pc;
    return -1;
}

static Entity first_with(ComponentId c) {
    for (uint32_t ai = 0; ai < V.w->archetype_count; ai++) {
        const EcsArchetype* a = &V.w->archetypes[ai];
        if (a->count && (a->mask & COMP_BIT(c))) return ecs_arch_entities(a)[0];
    }
    return ENTITY_NONE;
}

static Entity sun(void) {
    for (uint32_t ai = 0; ai < V.w->archetype_count; ai++) {
        const EcsArchetype* a = &V.w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_LIGHT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CLight* l = ecs_arch_column(a, COMP_LIGHT);
        for (uint32_t i = 0; i < a->count; i++)
            if (l[i].type == LIGHT_DIRECTIONAL) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}

static float rnd(void) {
    V.rng ^= V.rng << 13;
    V.rng ^= V.rng >> 7;
    V.rng ^= V.rng << 17;
    return (float)(V.rng >> 40) / (float)(1u << 24);
}

static void axes_of(const CTransform* t, float fwd[3], float right[3]) {
    CTransform u = *t;
    u.scale[0] = u.scale[1] = u.scale[2] = 1.0f;
    float rows[3][4];
    render_transform_rows(&u, rows);
    for (int k = 0; k < 3; k++) fwd[k] = rows[k][0], right[k] = rows[k][1];
}

static const char* actor_name(Entity e) {
    const CName* n = ecs_alive(V.w, e) ? ecs_get(V.w, e, COMP_NAME) : NULL;
    return n ? n->text : "None";
}

static BpAsset* class_of(Entity e) {
    const CBlueprint* b = ecs_alive(V.w, e) ? ecs_get(V.w, e, COMP_BLUEPRINT) : NULL;
    return b ? bp_find_hash(b->asset_hash) : NULL;
}

static bool implements(const BpAsset* c, const char* iface) {
    for (int depth = 0; c && depth < 8; depth++) {
        for (uint32_t i = 0; i < c->ninterfaces; i++)
            if (!strcmp(c->interfaces[i], iface)) return true;
        c = bp_find_name(BPA_CLASS, c->parent);
    }
    return false;
}

static void run(uint32_t ii, uint32_t pc);
static void tl_tick(uint32_t ii, uint32_t k, float dt); // (2026-11-16) a Timeline playing (below)
static uint32_t inst_index_of(Entity e);
// --- Strings made while playing (interned: an equal text is the same string) and the containers ---
// A string value: v[0] its index, v[1] 0 for the class's constant texts, 1 for the strings made while playing.
// A container value: v[0] its handle (1 + its index; 0 none). Both live until play stops (no malloc in a frame).

#define SHT (1u << 20) // the strings' hash table
typedef struct Arr { uint32_t count, cap; size_t off; uint8_t w, w2, cont, type, vtype; } Arr;

static uint32_t str_hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static const char* str_at(uint32_t id) { return id && id <= V.nstr ? (const char*)V.sdata.base + ((uint32_t*)V.soff.base)[id] : ""; }
static uint32_t str_intern(const char* s) {
    if (!s[0] || !V.sht.base) return 0;
    uint32_t* ht = (uint32_t*)V.sht.base;
    for (uint32_t i = str_hash(s) & (SHT - 1);; i = (i + 1) & (SHT - 1)) {
        if (!ht[i]) {
            size_t len = strlen(s) + 1;
            if (V.nstr + 1 >= SHT / 2 || !re_vm_ensure(&V.sdata, V.sused + len) || !re_vm_ensure(&V.soff, (size_t)(V.nstr + 2) * 4u)) return 0;
            memcpy(V.sdata.base + V.sused, s, len);
            ((uint32_t*)V.soff.base)[V.nstr + 1] = (uint32_t)V.sused;
            V.sused += len;
            ht[i] = ++V.nstr;
            return V.nstr;
        }
        if (!strcmp(str_at(ht[i]), s)) return ht[i];
    }
}
static const char* str_of(const Inst* in, const BpValue* v) {
    if (v->v[1] == 1.0f) return str_at((uint32_t)v->v[0]);
    return in ? in->cls->strings[(uint32_t)v->v[0] & 127] : "";
}
// A node's name operand (asset_name_op): the class's text, or the wired asset pin's value staged in the last slot
#define name_at(in, op) ((op) == BP_NAME_STAGED ? str_of((in), &V.np[BP_MAX_NPARAM - 1]) : (in)->cls->strings[(op) & 127])
static void set_str(BpValue* d, const char* s) {
    memset(d, 0, sizeof *d);
    d->v[0] = (float)str_intern(s);
    d->v[1] = 1.0f;
}
static void glob_str(const Inst* in, BpValue* v) { // a class's constant text made a play string (it leaves the class)
    if (v->v[1] != 1.0f) set_str(v, str_of(in, v));
}
static uint32_t u32_of(const BpValue* v) {
    uint32_t x;
    memcpy(&x, &v->v[0], 4);
    return v->v[1] == 1.0f ? x : 0u;
}
static void set_u32(BpValue* d, uint32_t x) {
    memset(d, 0, sizeof *d);
    memcpy(&d->v[0], &x, 4);
    d->v[1] = x ? 1.0f : 0.0f;
}
static int64_t i64_of(const BpValue* v) {
    double d;
    memcpy(&d, v->v, 8);
    return (int64_t)d;
}
static void set_i64(BpValue* v, int64_t x) {
    double d = (double)x;
    memset(v, 0, sizeof *v);
    memcpy(v->v, &d, 8);
}

static Arr* arr_get(const BpValue* v) {
    uint32_t h = (uint32_t)v->v[0];
    return h && h <= V.narr ? &((Arr*)V.arrs.base)[h - 1] : NULL;
}
static uint32_t arr_new(uint8_t cont, uint8_t w, uint8_t w2, uint8_t type, uint8_t vtype) {
    if (!re_vm_ensure(&V.arrs, (size_t)(V.narr + 1) * sizeof(Arr))) return 0;
    Arr* a = &((Arr*)V.arrs.base)[V.narr++];
    *a = (Arr){ 0, 0, 0, w ? w : 1, w2, cont, type, vtype };
    return V.narr;
}
static BpValue* arr_at(const Arr* a, uint32_t i) { return (BpValue*)V.adata.base + a->off + (size_t)i * (a->w + a->w2); }
static bool arr_reserve(Arr* a, uint32_t n) {
    if (n <= a->cap) return true;
    uint32_t cap = a->cap ? a->cap * 2 : 4;
    while (cap < n) cap *= 2;
    size_t stride = (size_t)(a->w + a->w2), need = (size_t)cap * stride;
    if (!re_vm_ensure(&V.adata, (V.aused + need) * sizeof(BpValue))) return false;
    BpValue* dst = (BpValue*)V.adata.base + V.aused;
    if (a->count) memcpy(dst, arr_at(a, 0), (size_t)a->count * stride * sizeof(BpValue));
    a->off = V.aused, a->cap = cap; // (the old block stays until play stops)
    V.aused += need;
    return true;
}
static bool elem_eq(const Inst* in, const Arr* a, const BpValue* x, const BpValue* y, uint32_t w, uint8_t type) {
    if (bp_type_stringish(type)) return !strcmp(str_of(in, x), str_of(in, y));
    return !memcmp(x, y, (size_t)w * sizeof(BpValue));
    (void)a;
}
static int32_t arr_find(const Inst* in, const Arr* a, const BpValue* item) {
    for (uint32_t i = 0; i < a->count; i++)
        if (elem_eq(in, a, arr_at(a, i), item, a->w, a->type)) return (int32_t)i;
    return -1;
}
// Puts a value in (a play string when it is a text) at element i, key part (0) or value part (1)
static void arr_put(const Inst* in, Arr* a, uint32_t i, const BpValue* v, int part) {
    BpValue* d = arr_at(a, i) + (part ? a->w : 0);
    uint32_t w = part ? a->w2 : a->w;
    memcpy(d, v, (size_t)w * sizeof(BpValue));
    if (bp_type_stringish(part ? a->vtype : a->type)) glob_str(in, d);
}
static int32_t arr_push(const Inst* in, Arr* a, const BpValue* v) {
    if (!arr_reserve(a, a->count + 1)) return -1;
    memset(arr_at(a, a->count), 0, (size_t)(a->w + a->w2) * sizeof(BpValue));
    arr_put(in, a, a->count, v, 0);
    return (int32_t)a->count++;
}
static void arr_copy(Arr* dst, const Arr* src) {
    dst->count = 0;
    if (!src || !arr_reserve(dst, src->count)) return;
    if (src->w + src->w2 != dst->w + dst->w2) return;
    memcpy(arr_at(dst, 0), arr_at(src, 0), (size_t)src->count * (src->w + src->w2) * sizeof(BpValue));
    dst->count = src->count;
}

// A value written as text (Print String, To String, the wires into a string pin)
static void to_text(const Inst* in, const BpValue* v, uint8_t type, char* out, size_t cap) {
    switch (type) {
    case BPT_BOOL: snprintf(out, cap, "%s", v->v[0] != 0.0f ? "true" : "false"); break;
    case BPT_INT: case BPT_ENUM: case BPT_BYTE: snprintf(out, cap, "%d", (int)v->v[0]); break;
    case BPT_INT64: snprintf(out, cap, "%lld", (long long)i64_of(v)); break;
    case BPT_VECTOR: snprintf(out, cap, "X=%.3f Y=%.3f Z=%.3f", v->v[0], v->v[1], v->v[2]); break;
    case BPT_ROTATOR: snprintf(out, cap, "P=%.3f Y=%.3f R=%.3f", v->v[0], v->v[1], v->v[2]); break;
    case BPT_COLOR: snprintf(out, cap, "(R=%.3f,G=%.3f,B=%.3f,A=%.3f)", v->v[0], v->v[1], v->v[2], v->v[3]); break;
    case BPT_TRANSFORM:
        snprintf(out, cap, "Translation: X=%.3f Y=%.3f Z=%.3f Rotation: P=%.3f Y=%.3f R=%.3f Scale X=%.3f Y=%.3f Z=%.3f", v[0].v[0], v[0].v[1],
                 v[0].v[2], v[1].v[0], v[1].v[1], v[1].v[2], v[2].v[0], v[2].v[1], v[2].v[2]);
        break;
    case BPT_ACTOR: snprintf(out, cap, "%s", actor_name(ent_of(v))); break;
    case BPT_CLASS: {
        const BpAsset* c = bp_find_hash(u32_of(v));
        snprintf(out, cap, "%s", c && u32_of(v) ? c->name : "None");
        break;
    }
    case BPT_STRING: case BPT_NAME: case BPT_TEXT: snprintf(out, cap, "%s", str_of(in, v)); break;
    default: snprintf(out, cap, "%.6g", v->v[0]); break;
    }
}

static void conv(const BpValue* s, BpValue* d, uint8_t from, uint8_t to) {
    double x = from == BPT_INT64 ? (double)i64_of(s) : (double)s->v[0];
    BpValue r = *s;
    switch (to) {
    case BPT_INT64: set_i64(&r, (int64_t)x); break;
    case BPT_BYTE: r = (BpValue){ { (float)((int64_t)x & 255) } }; break;
    case BPT_INT: r = (BpValue){ { (float)(int64_t)x } }; break;
    case BPT_BOOL: r = (BpValue){ { x != 0.0 ? 1.0f : 0.0f } }; break;
    case BPT_COLOR: r = *s, r.v[3] = from == BPT_COLOR ? s->v[3] : 1.0f; break;
    default: r = (BpValue){ { (float)x } }; break;
    }
    *d = r;
}

// --- Rotations and transforms (Unreal's: pitch, yaw, roll in degrees; render.c's axes) ---
static void rot_vec(const float rot[3], const float v[3], float o[3]) {
    float m[3][3];
    rotator_axes(rot, m);
    for (int k = 0; k < 3; k++) o[k] = v[0] * m[0][k] + v[1] * m[1][k] + v[2] * m[2][k];
}
static void unrot_vec(const float rot[3], const float v[3], float o[3]) {
    float m[3][3];
    rotator_axes(rot, m);
    for (int k = 0; k < 3; k++) o[k] = v[0] * m[k][0] + v[1] * m[k][1] + v[2] * m[k][2];
}
static void combine_rot(const float a[3], const float b[3], float o[3]) { // a, then b
    float ma[3][3], c[3][3];
    rotator_axes(a, ma);
    for (int i = 0; i < 3; i++) rot_vec(b, ma[i], c[i]);
    axes_rotator(c, o);
}
static float norm_angle(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}
static void dir_rot(const float d[3], float o[3]) {
    o[0] = atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) / DEG2RAD;
    o[1] = atan2f(d[1], d[0]) / DEG2RAD;
    o[2] = 0.0f;
}
static void xf_point(const BpValue* t, const float p[3], float o[3], bool dir) {
    float s[3] = { p[0] * t[2].v[0], p[1] * t[2].v[1], p[2] * t[2].v[2] };
    rot_vec(t[1].v, s, o);
    if (!dir) for (int k = 0; k < 3; k++) o[k] += t[0].v[k];
}
static void xf_inv_point(const BpValue* t, const float p[3], float o[3]) {
    float d[3] = { p[0] - t[0].v[0], p[1] - t[0].v[1], p[2] - t[0].v[2] }, u[3];
    unrot_vec(t[1].v, d, u);
    for (int k = 0; k < 3; k++) o[k] = t[2].v[k] != 0.0f ? u[k] / t[2].v[k] : 0.0f;
}
static float interp_to(float cur, float target, float dt, float speed) {
    if (speed <= 0.0f) return target;
    float d = target - cur;
    if (fabsf(d) < 1e-6f) return target;
    float k = fminf(fmaxf(dt * speed, 0.0f), 1.0f);
    return cur + d * k;
}

// The containers' and the newer nodes' work: true when handled (D written by itself when it has outputs)
// The Game Rules' Player input: unconnected it reads self; self that is no player (a widget, a Game Mode) means the local one
static Entity gr_player(World* w, const Inst* in, const BpValue* A) {
    Entity e = ent_of(A);
    if (e && e == in->e && !rules_slot(w, e) && !ecs_get(w, e, COMP_CHARACTER)) return ENTITY_NONE;
    return e;
}
static bool abil_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: ability.h)
static bool terror_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: terror.h, Q13)
static bool spline_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: spline.h, Q14)
static bool pcg_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: pcg.h, Q15)
static bool ingame_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: Q16)
static bool mr_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: Q25b)
static bool trace_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D); // (the end: 2026-11-19)
static bool native_more(Inst* in, const BpOp* o, BpValue* r, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    uint32_t kind = o->op - OP_NATIVE;
    const BpValue* N0 = &V.np[0];
    const BpValue* N1 = &V.np[1];
    BpValue res = { { 0 } };
    char buf[512];
    switch (kind) {
    // integers (floats holding whole numbers)
    case BPK_IADD: res.v[0] = A->v[0] + B->v[0]; break;
    case BPK_ISUB: res.v[0] = A->v[0] - B->v[0]; break;
    case BPK_IMUL: res.v[0] = A->v[0] * B->v[0]; break;
    case BPK_IDIV: res.v[0] = (int)B->v[0] ? (float)((int)A->v[0] / (int)B->v[0]) : 0.0f; break;
    case BPK_IMOD: res.v[0] = (int)B->v[0] ? (float)((int)A->v[0] % (int)B->v[0]) : 0.0f; break;
    case BPK_IGREATER: res.v[0] = (float)(A->v[0] > B->v[0]); break;
    case BPK_ILESS: res.v[0] = (float)(A->v[0] < B->v[0]); break;
    case BPK_IGE: case BPK_FGE: res.v[0] = (float)(A->v[0] >= B->v[0]); break;
    case BPK_ILE: case BPK_FLE: res.v[0] = (float)(A->v[0] <= B->v[0]); break;
    case BPK_IEQUAL: res.v[0] = (float)(A->v[0] == B->v[0]); break;
    case BPK_INOTEQUAL: case BPK_FNOTEQUAL: res.v[0] = (float)(A->v[0] != B->v[0]); break;
    case BPK_IMAX: case BPK_FMAX: res.v[0] = fmaxf(A->v[0], B->v[0]); break;
    case BPK_IMIN: case BPK_FMIN: res.v[0] = fminf(A->v[0], B->v[0]); break;
    case BPK_IABS: case BPK_FABS: res.v[0] = fabsf(A->v[0]); break;
    case BPK_ICLAMP: res.v[0] = fminf(fmaxf(A->v[0], B->v[0]), C->v[0]); break;
    case BPK_POW: res.v[0] = powf(A->v[0], B->v[0]); break;
    case BPK_SQRT: res.v[0] = A->v[0] > 0.0f ? sqrtf(A->v[0]) : 0.0f; break;
    case BPK_FLOOR: res.v[0] = floorf(A->v[0]); break;
    case BPK_CEIL: res.v[0] = ceilf(A->v[0]); break;
    case BPK_ROUND: res.v[0] = roundf(A->v[0]); break;
    case BPK_FRAC: res.v[0] = A->v[0] - floorf(A->v[0]); break;
    case BPK_FMOD: res.v[0] = B->v[0] != 0.0f ? fmodf(A->v[0], B->v[0]) : 0.0f; break;
    case BPK_TAN: res.v[0] = tanf(A->v[0] * DEG2RAD); break;
    case BPK_ATAN2: res.v[0] = atan2f(A->v[0], B->v[0]) / DEG2RAD; break;
    case BPK_MAP_RANGE: {
        float t = N0->v[0] == 0.0f && B->v[0] == C->v[0] ? 0.0f : (C->v[0] != B->v[0] ? (A->v[0] - B->v[0]) / (C->v[0] - B->v[0]) : 0.0f);
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        res.v[0] = N0->v[0] + (N1->v[0] - N0->v[0]) * t;
        break;
    }
    case BPK_FINTERP: res.v[0] = interp_to(A->v[0], B->v[0], C->v[0], N0->v[0]); break;
    case BPK_NEARLY_EQUAL: res.v[0] = (float)(fabsf(A->v[0] - B->v[0]) <= C->v[0]); break;
    case BPK_XOR: res.v[0] = (float)((A->v[0] != 0.0f) != (B->v[0] != 0.0f)); break;
    // vectors and rotators
    case BPK_VMUL: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] * B->v[i]; break;
    case BPK_VDIVS: for (int i = 0; i < 3; i++) res.v[i] = B->v[0] != 0.0f ? A->v[i] / B->v[0] : 0.0f; break;
    case BPK_CROSS:
        res.v[0] = A->v[1] * B->v[2] - A->v[2] * B->v[1], res.v[1] = A->v[2] * B->v[0] - A->v[0] * B->v[2];
        res.v[2] = A->v[0] * B->v[1] - A->v[1] * B->v[0];
        break;
    case BPK_VDIST: {
        float d[3] = { A->v[0] - B->v[0], A->v[1] - B->v[1], A->v[2] - B->v[2] };
        res.v[0] = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        break;
    }
    case BPK_VLERP: case BPK_LERP_COLOR: for (int i = 0; i < 4; i++) res.v[i] = A->v[i] + (B->v[i] - A->v[i]) * C->v[0]; break;
    case BPK_VINTERP: for (int i = 0; i < 3; i++) res.v[i] = interp_to(A->v[i], B->v[i], C->v[0], N0->v[0]); break;
    case BPK_ROTATE_VECTOR: rot_vec(B->v, A->v, res.v); break;
    case BPK_UNROTATE_VECTOR: unrot_vec(B->v, A->v, res.v); break;
    case BPK_VEQUAL:
        res.v[0] = (float)(fabsf(A->v[0] - B->v[0]) <= C->v[0] && fabsf(A->v[1] - B->v[1]) <= C->v[0] && fabsf(A->v[2] - B->v[2]) <= C->v[0]);
        break;
    case BPK_ROT_FORWARD: case BPK_ROT_RIGHT: case BPK_ROT_UP: {
        float m[3][3];
        rotator_axes(A->v, m);
        memcpy(res.v, m[kind - BPK_ROT_FORWARD], 12);
        break;
    }
    case BPK_LOOK_AT: {
        float d[3] = { B->v[0] - A->v[0], B->v[1] - A->v[1], B->v[2] - A->v[2] };
        dir_rot(d, res.v);
        break;
    }
    case BPK_ROT_FROM_X: dir_rot(A->v, res.v); break;
    case BPK_COMBINE_ROT: combine_rot(A->v, B->v, res.v); break;
    case BPK_RLERP: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] + norm_angle(B->v[i] - A->v[i]) * C->v[0]; break;
    case BPK_RINTERP:
        for (int i = 0; i < 3; i++) res.v[i] = interp_to(0.0f, norm_angle(B->v[i] - A->v[i]), C->v[0], N0->v[0]) + A->v[i];
        break;
    case BPK_DELTA_ROT: for (int i = 0; i < 3; i++) res.v[i] = norm_angle(A->v[i] - B->v[i]); break;
    // random
    case BPK_RAND_BOOL: res.v[0] = (float)(rnd() < 0.5f); break;
    case BPK_RAND_BOOL_W: res.v[0] = (float)(rnd() < A->v[0]); break;
    case BPK_RAND_INT: res.v[0] = A->v[0] >= 1.0f ? floorf(rnd() * floorf(A->v[0])) : 0.0f; break;
    case BPK_RAND_INT_RANGE: {
        float lo = floorf(fminf(A->v[0], B->v[0])), hi = floorf(fmaxf(A->v[0], B->v[0]));
        res.v[0] = fminf(lo + floorf(rnd() * (hi - lo + 1.0f)), hi);
        break;
    }
    case BPK_RAND_FLOAT: res.v[0] = rnd(); break;
    case BPK_RAND_UNIT_VEC: case BPK_RAND_CONE: {
        float z = rnd() * 2.0f - 1.0f, a = rnd() * 6.2831853f;
        if (kind == BPK_RAND_CONE) z = 1.0f - rnd() * (1.0f - cosf(fminf(fmaxf(B->v[0], 0.0f), 180.0f) * DEG2RAD));
        float s = sqrtf(fmaxf(1.0f - z * z, 0.0f)), l[3] = { s * cosf(a), s * sinf(a), z };
        if (kind == BPK_RAND_UNIT_VEC) {
            memcpy(res.v, l, 12);
            break;
        }
        float rot[3]; // the cone around its direction: the sample's +Z turned onto it
        dir_rot(A->v, rot);
        rot[0] -= 90.0f;
        rot_vec(rot, l, res.v);
        break;
    }
    case BPK_RAND_ROT: res.v[0] = rnd() * 360.0f - 180.0f, res.v[1] = rnd() * 360.0f - 180.0f, res.v[2] = A->v[0] != 0.0f ? rnd() * 360.0f - 180.0f : 0.0f; break;
    case BPK_RAND_POINT_BOX: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] + (rnd() * 2.0f - 1.0f) * B->v[i]; break;
    case BPK_RAND_INT64: {
        int64_t lo = i64_of(A), hi = i64_of(B);
        if (hi < lo) { int64_t t = lo; lo = hi, hi = t; }
        uint64_t span = (uint64_t)(hi - lo) + 1u, x = ((uint64_t)(rnd() * 16777216.0f) << 24) ^ (uint64_t)(rnd() * 16777216.0f);
        set_i64(&res, lo + (int64_t)(span ? x % span : 0));
        break;
    }
    case BPK_INT64_ADD: set_i64(&res, i64_of(A) + i64_of(B)); break;
    case BPK_INT64_SUB: set_i64(&res, i64_of(A) - i64_of(B)); break;
    case BPK_INT64_MUL: set_i64(&res, i64_of(A) * i64_of(B)); break;
    case BPK_INT64_DIV: set_i64(&res, i64_of(B) ? i64_of(A) / i64_of(B) : 0); break;
    // strings
    case BPK_STRING_TO_INT: res.v[0] = (float)atoi(str_of(in, A)); break;
    case BPK_STRING_TO_FLOAT: res.v[0] = (float)atof(str_of(in, A)); break;
    case BPK_APPEND: // (more than two: one after the other, emit_pure)
        snprintf(buf, sizeof buf, "%s%s", str_of(in, A), str_of(in, B));
        set_str(&res, buf);
        break;
    case BPK_STR_LEN: res.v[0] = (float)strlen(str_of(in, A)); break;
    case BPK_FORMAT_TEXT: { // {0}..{3}: B, C and the two staged ones; {{ a brace; nothing allocated (buf, then the strings' table)
        const char* f = str_of(in, A);
        const char* args[4] = { str_of(in, B), str_of(in, C), str_of(in, N0), str_of(in, N1) };
        size_t n = 0;
        for (const char* p = f; *p && n + 1 < sizeof buf; p++) {
            if (p[0] == '{' && p[1] == '{') { buf[n++] = '{', p++; continue; }
            if (p[0] == '}' && p[1] == '}') { buf[n++] = '}', p++; continue; }
            if (p[0] == '{' && p[1] >= '0' && p[1] <= '3' && p[2] == '}') {
                for (const char* q = args[p[1] - '0']; *q && n + 1 < sizeof buf; q++) buf[n++] = *q;
                p += 2;
                continue;
            }
            buf[n++] = *p;
        }
        buf[n] = 0;
        set_str(&res, buf);
        break;
    }
    case BPK_SUBSTRING: {
        const char* s = str_of(in, A);
        int len = (int)strlen(s), st = (int)B->v[0], n = (int)C->v[0];
        if (st < 0) st = 0;
        if (st > len) st = len;
        if (n < 0 || st + n > len) n = len - st;
        snprintf(buf, sizeof buf, "%.*s", n, s + st);
        set_str(&res, buf);
        break;
    }
    case BPK_STR_CONTAINS: {
        const char* h = str_of(in, A);
        const char* q = str_of(in, B);
        size_t ql = strlen(q);
        bool found = ql == 0;
        for (const char* p = h; *p && !found; p++) found = !_strnicmp(p, q, ql);
        res.v[0] = (float)found;
        break;
    }
    case BPK_STR_UPPER: case BPK_STR_LOWER: {
        snprintf(buf, sizeof buf, "%s", str_of(in, A));
        for (char* p = buf; *p; p++) *p = (char)(kind == BPK_STR_UPPER ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
        set_str(&res, buf);
        break;
    }
    case BPK_STR_EQUAL: res.v[0] = (float)!strcmp(str_of(in, A), str_of(in, B)); break;
    case BPK_STR_REPLACE: {
        const char* s = str_of(in, A);
        const char* from = str_of(in, B);
        const char* to = str_of(in, C);
        size_t fl = strlen(from), n = 0;
        while (*s && n + 1 < sizeof buf) {
            if (fl && !strncmp(s, from, fl)) {
                for (const char* t = to; *t && n + 1 < sizeof buf; t++) buf[n++] = *t;
                s += fl;
            } else buf[n++] = *s++;
        }
        buf[n] = 0;
        set_str(&res, buf);
        break;
    }
    case BPK_JOIN: {
        const Arr* a = arr_get(A);
        size_t n = 0;
        buf[0] = 0;
        for (uint32_t i = 0; a && i < a->count; i++) {
            n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0, "%s%s", i ? str_of(in, B) : "", str_of(in, arr_at(a, i)));
            if (n >= sizeof buf) break;
        }
        set_str(&res, buf);
        break;
    }
    // colors
    case BPK_MAKE_COLOR: res.v[0] = A->v[0], res.v[1] = B->v[0], res.v[2] = C->v[0], res.v[3] = N0->v[0]; break;
    case BPK_BREAK_COLOR:
        for (int i = 0; i < 4; i++) D[i] = (BpValue){ { A->v[i] } };
        return true;
    case BPK_HSV_TO_RGB: {
        float h = fmodf(A->v[0], 360.0f) / 60.0f, s = B->v[0], v = C->v[0];
        if (h < 0) h += 6.0f;
        float c = v * s, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = v - c, rgb[3] = { 0 };
        int seg = (int)h % 6;
        float t[6][3] = { { c, x, 0 }, { x, c, 0 }, { 0, c, x }, { 0, x, c }, { x, 0, c }, { c, 0, x } };
        for (int i = 0; i < 3; i++) rgb[i] = t[seg][i] + m;
        res.v[0] = rgb[0], res.v[1] = rgb[1], res.v[2] = rgb[2], res.v[3] = N0->v[0];
        break;
    }
    // transforms (3 registers: location, rotation, scale)
    case BPK_GET_TRANSFORM: {
        Entity e = ent_of(A);
        const CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        memset(D, 0, 3 * sizeof *D);
        if (t) memcpy(D[0].v, t->location, 12), memcpy(D[1].v, t->rotation, 12), memcpy(D[2].v, t->scale, 12);
        return true;
    }
    case BPK_SET_TRANSFORM: {
        Entity e = ent_of(A);
        CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t) memcpy(t->location, B[0].v, 12), memcpy(t->rotation, B[1].v, 12), memcpy(t->scale, B[2].v, 12), ecs_touch(w, e);
        return true;
    }
    case BPK_TRANSFORM_LOC: xf_point(A, B->v, res.v, false); break;
    case BPK_TRANSFORM_DIR: xf_point(A, B->v, res.v, true); break;
    case BPK_INV_TRANSFORM_LOC: xf_inv_point(A, B->v, res.v); break;
    case BPK_COMPOSE: {
        BpValue t[3];
        xf_point(B, A[0].v, t[0].v, false);
        combine_rot(A[1].v, B[1].v, t[1].v);
        for (int i = 0; i < 3; i++) t[2].v[i] = A[2].v[i] * B[2].v[i];
        t[0].v[3] = t[1].v[3] = t[2].v[3] = 0.0f;
        memcpy(D, t, sizeof t);
        return true;
    }
    case BPK_INVERT_TRANSFORM: {
        BpValue t[3] = { { { 0 } } };
        float m[3][3], inv[3][3];
        rotator_axes(A[1].v, m);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) inv[i][j] = m[j][i];
        axes_rotator(inv, t[1].v);
        for (int i = 0; i < 3; i++) t[2].v[i] = A[2].v[i] != 0.0f ? 1.0f / A[2].v[i] : 0.0f;
        float u[3];
        unrot_vec(A[1].v, A[0].v, u);
        for (int i = 0; i < 3; i++) t[0].v[i] = -u[i] * t[2].v[i];
        memcpy(D, t, sizeof t);
        return true;
    }
    // classes and actors
    case BPK_GET_CLASS: {
        const BpAsset* c = class_of(ent_of(A));
        set_u32(&res, c ? c->hash : 0u);
        break;
    }
    case BPK_CLASS_EQUAL: res.v[0] = (float)(u32_of(A) == u32_of(B)); break;
    case BPK_CREATE_WIDGET: set_ent(&res, ui_create_widget(w, u32_of(A) ? bp_find_hash(u32_of(A)) : NULL)); break;
    case BPK_ADD_TO_VIEWPORT: ui_add_to_viewport(ent_of(A) ? ent_of(A) : in->e, (int)B->v[0]); return true;
    case BPK_REMOVE_FROM_PARENT: ui_remove_from_parent(ent_of(A) ? ent_of(A) : in->e); return true;
    case BPK_WIDGET_SET_TEXT: ui_set_text(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), str_of(in, B)); return true;
    case BPK_WIDGET_SET_PERCENT: ui_set_percent(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), B->v[0]); return true;
    case BPK_WIDGET_SET_VISIBLE: ui_set_visible(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), B->v[0] != 0.0f); return true;
    case BPK_WIDGET_SET_COLOR: ui_set_color(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), B->v); return true;
    case BPK_WIDGET_SET_VALUE: ui_set_value(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), B->v[0]); return true;
    case BPK_WIDGET_SET_ENABLED: ui_set_enabled(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c), B->v[0] != 0.0f); return true;
    case BPK_WIDGET_GET_VALUE: res.v[0] = ui_get_value(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c)); break;
    case BPK_WIDGET_GET_TEXT: set_str(&res, ui_get_text(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c))); break;
    case BPK_PLAY_WIDGET_ANIM: // (the loops in B, the speed in C, the animation's name in d)
        ui_play_anim(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->d), (int)B->v[0], C->v[0], false);
        return true;
    case BPK_STOP_WIDGET_ANIM: ui_stop_anim(ent_of(A) ? ent_of(A) : in->e, name_at(in, o->c)); return true;
    case BPK_PLAY_SEQUENCE: seq_play(w, name_at(in, o->c), A->v[0] != 0.0f, B->v[0]); return true;
    case BPK_ED_SELECTED_COUNT: res.v[0] = g_ed_hooks.selected_count ? (float)g_ed_hooks.selected_count() : 0.0f; break;
    case BPK_ED_SELECTED_AT: set_ent(&res, g_ed_hooks.selected_at && A->v[0] >= 0.0f ? g_ed_hooks.selected_at((uint32_t)A->v[0]) : ENTITY_NONE); break;
    case BPK_ED_SELECT:
        if (g_ed_hooks.select) g_ed_hooks.select(ent_of(A), B->v[0] != 0.0f);
        return true;
#define AU_NAMED(kind) audio_asset_by_name((kind), str_of(in, A)) // (a constant's text or the Sound pin's value)
    // --- audio (A: the asset's name (a string's index) where the node has one; B, C, V.np[]: the inputs after it) ---
    case BPK_PLAY_SOUND_2D: case BPK_SPAWN_SOUND_2D: {
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = AU_NAMED(-1), p.volume = B->v[0], p.pitch = C->v[0], p.start_time = V.np[0].v[0], p.bone = -1, p.owner = in->e;
        AudioHandle h = audio_play(&p);
        if (kind == BPK_PLAY_SOUND_2D) return true;
        res.v[0] = (float)h;
        break;
    }
    case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_AT: {
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = AU_NAMED(-1), p.spatial = true, p.volume = C->v[0], p.pitch = V.np[0].v[0], p.start_time = V.np[1].v[0], p.bone = -1;
        memcpy(p.pos, B->v, 12);
        p.owner = in->e;
        AudioHandle h = audio_play(&p);
        if (kind == BPK_PLAY_SOUND_AT) return true;
        res.v[0] = (float)h;
        break;
    }
    case BPK_SPAWN_SOUND_ATTACHED: { // B: the actor; the bone's name: the string index after the inputs
        Entity at = ent_of(B) ? ent_of(B) : in->e;
        const char* bone = in->cls->strings[(int)V.np[2].v[0] & 127];
        Entity follow = bone && bone[0] ? anim_bone_entity(w, at, bone) : ENTITY_NONE;
        if (!follow) follow = at;
        const CTransform* t = ecs_get(w, follow, COMP_TRANSFORM);
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = AU_NAMED(-1), p.spatial = true, p.follow = follow, p.bone = -1, p.owner = at;
        p.volume = C->v[0], p.pitch = V.np[0].v[0], p.start_time = V.np[1].v[0];
        if (t) memcpy(p.pos, t->location, 12);
        res.v[0] = (float)audio_play(&p);
        break;
    }
    case BPK_STOP_SOUND: audio_stop((AudioHandle)A->v[0], B->v[0]); return true;
    case BPK_SET_SOUND_PAUSED: audio_set_paused((AudioHandle)A->v[0], B->v[0] != 0.0f); return true;
    case BPK_IS_SOUND_PLAYING: res.v[0] = audio_is_playing((AudioHandle)A->v[0]) ? 1.0f : 0.0f; break;
    case BPK_SET_SOUND_VOLUME: audio_set_volume((AudioHandle)A->v[0], B->v[0]); return true;
    case BPK_SET_SOUND_PITCH: audio_set_pitch((AudioHandle)A->v[0], B->v[0]); return true;
    case BPK_AUDIO_PLAY: audio_component_play(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_AUDIO_STOP: audio_component_stop(w, ent_of(A) ? ent_of(A) : in->e, 0.0f); return true;
    case BPK_AUDIO_FADE_IN: audio_component_fade_in(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], C->v[0], V.np[0].v[0]); return true;
    case BPK_AUDIO_FADE_OUT: { // (to 0: stopped at the end, Unreal's)
        Entity e = ent_of(A) ? ent_of(A) : in->e;
        AudioHandle h = audio_component_handle(w, e);
        if (C->v[0] <= 0.0f) audio_component_stop(w, e, B->v[0]);
        else if (h) audio_fade(h, B->v[0], C->v[0]);
        return true;
    }
    case BPK_AUDIO_SET_PAUSED: audio_component_set_paused(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_AUDIO_SET_VOLUME: audio_component_set_volume(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_AUDIO_SET_PITCH: audio_component_set_pitch(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_AUDIO_IS_PLAYING: res.v[0] = audio_component_is_playing(w, ent_of(A) ? ent_of(A) : in->e) ? 1.0f : 0.0f; break;
    case BPK_AUDIO_SET_SOUND: audio_component_set_sound(w, ent_of(B) ? ent_of(B) : in->e, AU_NAMED(-1)); return true;
    case BPK_PUSH_SOUND_MIX: audio_push_mix(AU_NAMED(AUA_MIX)); return true;
    case BPK_POP_SOUND_MIX: audio_pop_mix(AU_NAMED(AUA_MIX)); return true;
    case BPK_CLEAR_SOUND_MIXES: audio_clear_mixes(); return true;
    case BPK_SET_BASE_SOUND_MIX: audio_set_base_mix(AU_NAMED(AUA_MIX)); return true;
    case BPK_SET_CLASS_VOLUME: {
        AuAsset* c = au_asset(AU_NAMED(AUA_CLASS));
        if (c && c->kind == AUA_CLASS) c->u.cls.volume = fmaxf(B->v[0], 0.0f), c->u.cls.pitch = C->v[0] > 0.01f ? C->v[0] : 1.0f;
        return true;
    }
    case BPK_STOP_SEQUENCE: seq_stop(w, name_at(in, o->c)); return true;
    case BPK_IS_SEQUENCE_PLAYING: res.v[0] = seq_is_playing(name_at(in, o->c)) ? 1.0f : 0.0f; break;
    case BPK_SHOW_MOUSE: ui_show_mouse(A->v[0] != 0.0f); return true;
    case BPK_GET_DISPLAY_NAME: set_str(&res, actor_name(ent_of(A))); break;
    case BPK_OBJ_EQUAL: res.v[0] = (float)(ent_of(A) == ent_of(B)); break;
    case BPK_GET_ALL_ACTORS:
    case BPK_GET_ACTOR_OF_CLASS: {
        const BpAsset* want = u32_of(A) ? bp_find_hash(u32_of(A)) : NULL;
        Arr* out = kind == BPK_GET_ALL_ACTORS ? arr_get(D) : NULL;
        if (out) out->count = 0;
        Entity first = ENTITY_NONE;
        for (uint32_t ai = 0; ai < w->archetype_count && (out || first == ENTITY_NONE); ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!ar->count || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            if (want && !(ar->mask & COMP_BIT(COMP_BLUEPRINT))) continue;
            if (ar->mask & COMP_BIT(COMP_ATTACH)) continue; // (components, not actors)
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t i = 0; i < ar->count; i++) {
                if (want) {
                    const BpAsset* c = class_of(ents[i]);
                    if (!c || !bp_is_child_of(c, want->name)) continue;
                }
                BpValue v;
                set_ent(&v, ents[i]);
                if (out) arr_push(in, out, &v);
                else if (first == ENTITY_NONE) first = ents[i];
            }
        }
        if (out) return true;
        set_ent(&res, first);
        break;
    }
    // timers (a Custom Event called later)
    case BPK_SET_TIMER:
    case BPK_CLEAR_TIMER: {
        int32_t pc = find_entry(in->cls, BPK_EVENT_CUSTOM, name_at(in, o->c));
        uint32_t ii = (uint32_t)(in - insts());
        for (uint32_t t = 0; t < V.ntimers;) // the event's timer replaced (Unreal's: one per function)
            if (V.timers[t].inst == ii && (int32_t)V.timers[t].pc == pc) V.timers[t] = V.timers[--V.ntimers];
            else t++;
        if (kind == BPK_SET_TIMER && pc >= 0 && A->v[0] > 0.0f && V.ntimers < 256)
            V.timers[V.ntimers++] = (BpTimer){ ii, (uint32_t)pc, A->v[0], A->v[0], B->v[0] != 0.0f };
        return true;
    }
    // Enhanced Input (b 1: an event's entry: the elapsed and triggered seconds after the value)
    case BPK_GET_COMPONENT: { // self's component of that name (its own entity: ATTACH_SCS, owned by self)
        const char* cn = name_at(in, o->c);
        Entity found = ENTITY_NONE;
        for (uint32_t ai = 0; ai < w->archetype_count && !found; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_NAME);
            if ((ar->mask & need) != need || !ar->count) continue;
            const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
            const CName* nm = ecs_arch_column(ar, COMP_NAME);
            for (uint32_t r = 0; r < ar->count; r++)
                if (at[r].owner == in->e && at[r].kind == ATTACH_SCS && !strcmp(nm[r].text, cn)) {
                    found = ecs_arch_entities(ar)[r];
                    break;
                }
        }
        set_ent(&D[0], found);
        return true;
    }
    case BPK_GET_ACTION_VALUE: {
        float v[3], el = 0, tr = 0;
        uint32_t ev = 0;
        input_action_state(name_at(in, o->c), v, &ev, &el, &tr);
        D[0] = (BpValue){ { v[0], v[1], v[2], 0 } };
        if (o->b) D[1] = (BpValue){ { el } }, D[2] = (BpValue){ { tr } };
        return true;
    }
    case BPK_ADD_MAPPING_CONTEXT: input_add_context(name_at(in, o->c), (int)A->v[0]); return true;
    case BPK_REMOVE_MAPPING_CONTEXT: input_remove_context(name_at(in, o->c)); return true;
    // levels
    case BPK_OPEN_LEVEL: if (V.hooks.open) V.hooks.open(str_of(in, A)); return true;
    case BPK_GET_LEVEL_NAME: set_str(&res, V.hooks.current ? V.hooks.current() : ""); break;
    // arrays, sets, maps (A: the container)
    case BPK_ARR_ADD: case BPK_ARR_ADD_UNIQUE: case BPK_SET_ADD: {
        Arr* a = arr_get(A);
        int32_t at = -1;
        if (a && (kind == BPK_ARR_ADD || arr_find(in, a, B) < 0)) at = arr_push(in, a, B);
        if (kind != BPK_SET_ADD && o->d) r[o->d] = (BpValue){ { (float)at } };
        return true;
    }
    case BPK_ARR_INSERT: {
        Arr* a = arr_get(A);
        int at = (int)C->v[0];
        if (!a || at < 0 || (uint32_t)at > a->count || !arr_reserve(a, a->count + 1)) return true;
        size_t st = (size_t)(a->w + a->w2);
        memmove(arr_at(a, (uint32_t)at + 1), arr_at(a, (uint32_t)at), (size_t)(a->count - (uint32_t)at) * st * sizeof(BpValue));
        a->count++;
        arr_put(in, a, (uint32_t)at, B, 0);
        return true;
    }
    case BPK_ARR_REMOVE_INDEX: {
        Arr* a = arr_get(A);
        int at = (int)B->v[0];
        if (!a || at < 0 || (uint32_t)at >= a->count) return true;
        size_t st = (size_t)(a->w + a->w2);
        memmove(arr_at(a, (uint32_t)at), arr_at(a, (uint32_t)at + 1), (size_t)(a->count - (uint32_t)at - 1) * st * sizeof(BpValue));
        a->count--;
        return true;
    }
    case BPK_ARR_REMOVE: case BPK_SET_REMOVE: case BPK_MAP_REMOVE: {
        Arr* a = arr_get(A);
        bool any = false;
        for (int32_t i; a && (i = arr_find(in, a, B)) >= 0;) {
            size_t st = (size_t)(a->w + a->w2);
            memmove(arr_at(a, (uint32_t)i), arr_at(a, (uint32_t)i + 1), (size_t)(a->count - (uint32_t)i - 1) * st * sizeof(BpValue));
            a->count--, any = true;
        }
        if (o->d) r[o->d] = (BpValue){ { (float)any } };
        return true;
    }
    case BPK_ARR_CLEAR: case BPK_SET_CLEAR: case BPK_MAP_CLEAR: {
        Arr* a = arr_get(A);
        if (a) a->count = 0;
        return true;
    }
    case BPK_ARR_SET: {
        Arr* a = arr_get(A);
        int at = (int)B->v[0];
        if (!a || at < 0) return true;
        if ((uint32_t)at >= a->count) {
            if (N0->v[0] == 0.0f || !arr_reserve(a, (uint32_t)at + 1)) return true;
            memset(arr_at(a, a->count), 0, (size_t)((uint32_t)at + 1 - a->count) * (a->w + a->w2) * sizeof(BpValue));
            a->count = (uint32_t)at + 1;
        }
        arr_put(in, a, (uint32_t)at, C, 0);
        return true;
    }
    case BPK_ARR_GET: {
        const Arr* a = arr_get(A);
        int at = (int)B->v[0];
        if (a && at >= 0 && (uint32_t)at < a->count) memcpy(D, arr_at(a, (uint32_t)at), a->w * sizeof(BpValue));
        else memset(D, 0, (a ? a->w : 1) * sizeof(BpValue));
        return true;
    }
    case BPK_RAND_ARRAY_ITEM: {
        const Arr* a = arr_get(A);
        uint32_t wd = a ? a->w : 1;
        if (a && a->count) {
            uint32_t i = (uint32_t)(rnd() * (float)a->count);
            if (i >= a->count) i = a->count - 1;
            memcpy(D, arr_at(a, i), wd * sizeof(BpValue));
            D[wd] = (BpValue){ { (float)i } };
        } else memset(D, 0, (wd + 1) * sizeof(BpValue)), D[wd].v[0] = -1.0f;
        return true;
    }
    case BPK_ARR_LEN: case BPK_SET_LEN: case BPK_MAP_LEN: { const Arr* a = arr_get(A); res.v[0] = a ? (float)a->count : 0.0f; break; }
    case BPK_ARR_LAST: { const Arr* a = arr_get(A); res.v[0] = a ? (float)a->count - 1.0f : -1.0f; break; }
    case BPK_ARR_IS_EMPTY: { const Arr* a = arr_get(A); res.v[0] = (float)(!a || !a->count); break; }
    case BPK_ARR_VALID_INDEX: { const Arr* a = arr_get(A); res.v[0] = (float)(a && B->v[0] >= 0.0f && B->v[0] < (float)a->count); break; }
    case BPK_ARR_CONTAINS: case BPK_SET_CONTAINS: case BPK_MAP_CONTAINS: { const Arr* a = arr_get(A); res.v[0] = (float)(a && arr_find(in, a, B) >= 0); break; }
    case BPK_ARR_FIND: { const Arr* a = arr_get(A); res.v[0] = a ? (float)arr_find(in, a, B) : -1.0f; break; }
    case BPK_ARR_SHUFFLE: {
        Arr* a = arr_get(A);
        size_t st = a ? (size_t)(a->w + a->w2) : 0;
        BpValue tmp[16];
        for (uint32_t i = a && st <= 16 ? a->count : 0; i > 1; i--) {
            uint32_t j = (uint32_t)(rnd() * (float)i);
            if (j >= i) j = i - 1;
            memcpy(tmp, arr_at(a, i - 1), st * sizeof(BpValue));
            memcpy(arr_at(a, i - 1), arr_at(a, j), st * sizeof(BpValue));
            memcpy(arr_at(a, j), tmp, st * sizeof(BpValue));
        }
        return true;
    }
    case BPK_ARR_APPEND: {
        Arr* a = arr_get(A);
        const Arr* s = arr_get(B);
        if (!a || !s || a->w != s->w || a == s || !arr_reserve(a, a->count + s->count)) return true;
        memcpy(arr_at(a, a->count), arr_at(s, 0), (size_t)s->count * (a->w + a->w2) * sizeof(BpValue));
        a->count += s->count;
        return true;
    }
    case BPK_ARR_RESIZE: {
        Arr* a = arr_get(A);
        int n = (int)B->v[0];
        if (!a || n < 0 || !arr_reserve(a, (uint32_t)n)) return true;
        if ((uint32_t)n > a->count) memset(arr_at(a, a->count), 0, (size_t)((uint32_t)n - a->count) * (a->w + a->w2) * sizeof(BpValue));
        a->count = (uint32_t)n;
        return true;
    }
    case BPK_SET_TO_ARRAY: case BPK_MAP_KEYS: case BPK_MAP_VALUES: {
        const Arr* s = arr_get(A);
        Arr* out = arr_get(D);
        if (!out) return true;
        out->count = 0;
        for (uint32_t i = 0; s && i < s->count; i++) arr_push(in, out, arr_at(s, i) + (kind == BPK_MAP_VALUES ? s->w : 0));
        return true;
    }
    case BPK_MAP_ADD: {
        Arr* a = arr_get(A);
        if (!a) return true;
        int32_t i = arr_find(in, a, B);
        if (i < 0) i = arr_push(in, a, B);
        if (i >= 0) arr_put(in, a, (uint32_t)i, C, 1);
        return true;
    }
    case BPK_MAP_FIND: {
        const Arr* a = arr_get(A);
        int32_t i = a ? arr_find(in, a, B) : -1;
        uint32_t w2 = a && a->w2 ? a->w2 : 1;
        if (i >= 0) memcpy(D, arr_at(a, (uint32_t)i) + a->w, w2 * sizeof(BpValue));
        else memset(D, 0, w2 * sizeof(BpValue));
        D[w2] = (BpValue){ { (float)(i >= 0) } };
        return true;
    }
    // --- Online (online.h) ---
    case BPK_NET_CREATE_SESSION: online_host(w, str_of(in, C), (int)A->v[0], B->v[0] != 0.0f); return true;
    case BPK_NET_FIND_SESSIONS: online_find(A->v[0] != 0.0f); return true;
    case BPK_NET_SESSION_COUNT: res.v[0] = (float)online_session_count(); break;
    case BPK_NET_SESSION_NAME: { const OnlineSession* s = online_session((int)A->v[0]); set_str(&res, s ? s->name : ""); break; }
    case BPK_NET_SESSION_PLAYERS: { const OnlineSession* s = online_session((int)A->v[0]); res.v[0] = s ? (float)s->players : 0.0f; break; }
    case BPK_NET_JOIN_SESSION: online_join(w, (int)A->v[0]); return true;
    case BPK_NET_JOIN_ADDRESS: online_join_address(w, str_of(in, A)); return true;
    case BPK_NET_DESTROY_SESSION: online_leave(w); return true;
    case BPK_NET_IS_SERVER: res.v[0] = (float)(online_role() != ONLINE_CLIENT); break;
    case BPK_NET_IS_CLIENT: res.v[0] = (float)(online_role() == ONLINE_CLIENT); break;
    case BPK_NET_IN_SESSION: res.v[0] = (float)online_active(); break;
    case BPK_NET_HAS_AUTHORITY: res.v[0] = (float)online_has_authority(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_NET_LOCALLY_CONTROLLED: res.v[0] = (float)online_is_locally_controlled(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_NET_PLAYER_COUNT: res.v[0] = (float)online_player_count(); break;
    case BPK_NET_PLAYER_NAME: set_str(&res, online_player_name((int)A->v[0])); break;
    case BPK_NET_PLAYER_PAWN: set_ent(&res, online_player_pawn(w, (int)A->v[0])); break;
    case BPK_NET_PING: res.v[0] = online_player_ping((int)A->v[0]); break;
    case BPK_NET_PACKET_LOSS: res.v[0] = online_player_loss((int)A->v[0]) * 100.0f; break;
    case BPK_NET_LOCAL_PLAYER: res.v[0] = (float)online_local_player(); break;
    case BPK_NET_KICK: online_kick((int)A->v[0], str_of(in, B)); return true;
    case BPK_NET_CHAT: online_chat(str_of(in, A)); return true;
    case BPK_NET_SET_REPLICATES: online_set_replicates(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_NET_RPC_SERVER:
    case BPK_NET_RPC_MULTICAST:
    case BPK_NET_RPC_OWNER: { // Target, Float, Vector; Text and Reliable in the extra inputs; the event in d
        OnlineRpc mode = kind == BPK_NET_RPC_SERVER ? ONLINE_RPC_SERVER : kind == BPK_NET_RPC_MULTICAST ? ONLINE_RPC_MULTICAST : ONLINE_RPC_OWNER;
        online_rpc(w, ent_of(A) ? ent_of(A) : in->e, mode, in->cls->strings[o->d & 127], N1->v[0] != 0.0f, B->v[0], C->v, str_of(in, N0));
        return true;
    }
    case BPK_NET_RPC_FLOAT: online_rpc_args(&res.v[0], NULL, NULL, NULL); break;
    case BPK_NET_RPC_VECTOR: online_rpc_args(NULL, res.v, NULL, NULL); break;
    case BPK_NET_RPC_TEXT: { const char* t = ""; online_rpc_args(NULL, NULL, &t, NULL); set_str(&res, t); break; }
    case BPK_NET_RPC_SENDER: { int s = 0; online_rpc_args(NULL, NULL, NULL, &s); res.v[0] = (float)s; break; }
    case BPK_NET_REPLICATE_VAR: {
        Entity re = ent_of(A) ? ent_of(A) : in->e;
        bool sent = online_replicate_var(w, re, in->cls->strings[o->d & 127]);
        // (a RepNotify variable set: OnRep_<Name> here too, as Unreal's Blueprints do; a listen host already ran it)
        if (o->b == 1 && !(sent && online_role() == ONLINE_LISTEN)) {
            char rn[80];
            snprintf(rn, sizeof rn, "OnRep_%s", in->cls->strings[o->d & 127]);
            bp_call_event(w, re, rn);
        }
        return true;
    }
    case BPK_NET_SPAWN: set_ent(&res, online_spawn(w, u32_of(A), B->v, C->v, (int)N0->v[0])); break; // (class, location, rotation, owner)
    case BPK_NET_DESTROY: if (ent_of(A)) online_destroy(w, ent_of(A)); return true;
    case BPK_NET_EMULATION: {
        NetSim s = { fmaxf(A->v[0], 0.0f), fmaxf(B->v[0], 0.0f), fminf(fmaxf(C->v[0] * 0.01f, 0.0f), 1.0f), 0.0f };
        online_set_sim(&s);
        return true;
    }
    // --- Steam (steam.h) ---
    case BPK_STEAM_AVAILABLE: res.v[0] = (float)steam_available(); break;
    case BPK_STEAM_NAME: set_str(&res, steam_user_name()); break;
    case BPK_STEAM_ID: snprintf(buf, sizeof buf, "%llu", (unsigned long long)steam_user_id()); set_str(&res, buf); break;
    case BPK_STEAM_UNLOCK: steam_achievement_unlock(str_of(in, A)); return true;
    case BPK_STEAM_CLEAR: steam_achievement_clear(str_of(in, A)); return true;
    case BPK_STEAM_IS_UNLOCKED: { bool got = false; steam_achievement_get(str_of(in, A), &got); res.v[0] = (float)got; break; }
    case BPK_STEAM_PROGRESS: steam_achievement_progress(str_of(in, A), (uint32_t)fmaxf(B->v[0], 0.0f), (uint32_t)fmaxf(C->v[0], 1.0f)); return true;
    case BPK_STEAM_SET_STAT:
        if (C->v[0] != 0.0f) steam_stat_set_int(str_of(in, A), (int32_t)lrintf(B->v[0]));
        else steam_stat_set_float(str_of(in, A), B->v[0]);
        return true;
    case BPK_STEAM_GET_STAT:
        if (B->v[0] != 0.0f) { int32_t v = 0; steam_stat_get_int(str_of(in, A), &v); res.v[0] = (float)v; }
        else steam_stat_get_float(str_of(in, A), &res.v[0]);
        break;
    case BPK_STEAM_STORE_STATS: steam_stats_store(); return true;
    case BPK_STEAM_UPLOAD_SCORE: steam_leaderboard_upload(str_of(in, A), (int32_t)lrintf(B->v[0]), C->v[0] != 0.0f); return true;
    case BPK_STEAM_DOWNLOAD_SCORES: { // Kind 1: around the player (Count split above and below)
        int k = (int)B->v[0], n = C->v[0] > 0.0f ? (int)C->v[0] : 10;
        steam_leaderboard_download(str_of(in, A), k, k == 1 ? -n / 2 : 1, k == 1 ? n / 2 : n);
        return true;
    }
    case BPK_STEAM_SCORE_COUNT: { int n = 0; while (n < 100 && steam_leaderboard_entry(n, NULL, NULL, NULL)) n++; res.v[0] = (float)n; break; }
    case BPK_STEAM_SCORE_NAME: { uint64_t u = 0; steam_leaderboard_entry((int)A->v[0], &u, NULL, NULL); set_str(&res, u ? steam_friend_name(u) : ""); break; }
    case BPK_STEAM_SCORE_VALUE: { int32_t s = 0; steam_leaderboard_entry((int)A->v[0], NULL, NULL, &s); res.v[0] = (float)s; break; }
    case BPK_STEAM_SCORE_RANK: { int32_t r = 0; steam_leaderboard_entry((int)A->v[0], NULL, &r, NULL); res.v[0] = (float)r; break; }
    case BPK_STEAM_OVERLAY: steam_overlay(str_of(in, A)); return true;
    case BPK_STEAM_INVITE: steam_overlay_invite(); return true;
    case BPK_STEAM_PRESENCE: steam_rich_presence(str_of(in, A), str_of(in, B)); return true;
    case BPK_STEAM_DLC: res.v[0] = (float)steam_dlc_installed((uint32_t)fmaxf(A->v[0], 0.0f)); break;
    case BPK_STEAM_LANGUAGE: set_str(&res, steam_language()); break;
    case BPK_STEAM_FRIEND_COUNT: res.v[0] = (float)steam_friend_count(); break;
    case BPK_STEAM_FRIEND_NAME: set_str(&res, steam_friend_name(steam_friend((int)A->v[0]))); break;
    case BPK_STEAM_CLOUD_ENABLED: res.v[0] = (float)steam_cloud_enabled(); break;
    // --- advanced sessions (online.h; A, B, C and V.np[] the inputs in order) ---
    case BPK_NET_CREATE_ADV: { // Name, Max, Private | LAN, Visibility, Password, JIP, Invites, Advertise, Map, Mode
        OnlineSessionSettings st;
        online_settings_default(&st);
        snprintf(st.name, sizeof st.name, "%s", str_of(in, A));
        st.max_players = (int)B->v[0], st.private_slots = (int)C->v[0], st.lan = V.np[0].v[0] != 0.0f;
        int vis = (int)V.np[1].v[0];
        st.visibility = vis == 1 ? ONLINE_VIS_FRIENDS : vis == 2 ? ONLINE_VIS_INVITE_ONLY : ONLINE_VIS_PUBLIC;
        snprintf(st.password, sizeof st.password, "%s", str_of(in, &V.np[2]));
        st.allow_join_in_progress = V.np[3].v[0] != 0.0f, st.allow_invites = V.np[4].v[0] != 0.0f, st.advertise = V.np[5].v[0] != 0.0f;
        snprintf(st.map, sizeof st.map, "%s", str_of(in, &V.np[6]));
        snprintf(st.game_mode, sizeof st.game_mode, "%s", str_of(in, &V.np[7]));
        online_host_advanced(w, &st);
        return true;
    }
    case BPK_NET_FIND_ADV: { // LAN, Max Results, Server Type | Min Slots, Exclude Full, Empty, Non Empty, Hide Pw, Hide Started, Max Ping, Same Build
        OnlineSearch q;
        memset(&q, 0, sizeof q);
        q.lan = A->v[0] != 0.0f, q.max_results = (int)B->v[0];
        int t = (int)C->v[0];
        q.server_type = t == 1 ? ONLINE_SERVERS_LISTEN : t == 2 ? ONLINE_SERVERS_DEDICATED : ONLINE_SERVERS_ALL;
        q.min_open_slots = (int)V.np[0].v[0], q.exclude_full = V.np[1].v[0] != 0.0f, q.empty_only = V.np[2].v[0] != 0.0f;
        q.non_empty_only = V.np[3].v[0] != 0.0f, q.hide_passworded = V.np[4].v[0] != 0.0f, q.hide_in_progress = V.np[5].v[0] != 0.0f;
        q.max_ping_ms = V.np[6].v[0], q.same_build_only = V.np[7].v[0] != 0.0f;
        online_search_filters(q.filters, &q.nfilters);
        online_find_advanced(&q);
        return true;
    }
    case BPK_NET_ADD_FILTER: {
        int c = (int)C->v[0];
        res.v[0] = (float)online_search_filter_add(str_of(in, A), str_of(in, B), c >= 0 && c <= 6 ? (OnlineCompare)c : ONLINE_CMP_EQ);
        break;
    }
    case BPK_NET_CLEAR_FILTERS: online_search_filters_clear(); return true;
    case BPK_NET_SET_PROP: res.v[0] = (float)online_set_property(str_of(in, A), str_of(in, B)); break;
    case BPK_NET_REMOVE_PROP: res.v[0] = (float)online_remove_property(str_of(in, A)); break;
    case BPK_NET_GET_PROP: {
        bool found;
        const char* v = online_get_property(str_of(in, A), &found);
        set_str(&D[0], v), D[1] = (BpValue){ { (float)found } };
        return true;
    }
    case BPK_NET_RESULT_PROP: {
        bool found;
        const char* v = online_session_property((int)A->v[0], str_of(in, B), &found);
        set_str(&D[0], v), D[1] = (BpValue){ { (float)found } };
        return true;
    }
    case BPK_NET_LAG_TRACE: {
        Entity hit = ENTITY_NONE;
        float at[3];
        bool ok = online_lag_trace(w, (int)C->v[0], A->v, B->v, V.np[0].v[0], &hit, at);
        D[0] = (BpValue){ { (float)ok } };
        set_ent(&D[1], hit);
        D[2] = (BpValue){ { at[0], at[1], at[2] } };
        return true;
    }
    case BPK_NET_RESULT_INFO: {
        const OnlineSession* ses = online_session((int)A->v[0]);
        static const OnlineSession none;
        if (!ses) ses = &none;
        set_str(&D[0], ses->name), set_str(&D[1], ses->host);
        D[2] = (BpValue){ { (float)ses->players } }, D[3] = (BpValue){ { (float)ses->max_players } };
        D[4] = (BpValue){ { (float)(ses->max_players > ses->players ? ses->max_players - ses->players : 0) } };
        D[5] = (BpValue){ { ses == &none ? 0.0f : ses->ping_ms } };
        set_str(&D[6], ses->level), set_str(&D[7], ses->game_mode);
        D[8] = (BpValue){ { (float)ses->dedicated } }, D[9] = (BpValue){ { (float)ses->has_password } };
        D[10] = (BpValue){ { (float)ses->in_progress } }, D[11] = (BpValue){ { (float)(ses != &none && !ses->steam) } };
        return true;
    }
    case BPK_NET_JOIN_ADV: online_join_ex(w, (int)A->v[0], str_of(in, B)); return true;
    case BPK_NET_JOIN_ADDR_ADV: online_join_address_ex(w, str_of(in, A), str_of(in, B)); return true;
    case BPK_NET_UPDATE_SESSION: { // Name, Max, Private | Password, Advertise, JIP, Map, Mode
        OnlineSessionSettings st = *online_current_settings();
        snprintf(st.name, sizeof st.name, "%s", str_of(in, A));
        st.max_players = (int)B->v[0], st.private_slots = (int)C->v[0];
        snprintf(st.password, sizeof st.password, "%s", str_of(in, &V.np[0]));
        st.advertise = V.np[1].v[0] != 0.0f, st.allow_join_in_progress = V.np[2].v[0] != 0.0f;
        snprintf(st.map, sizeof st.map, "%s", str_of(in, &V.np[3]));
        snprintf(st.game_mode, sizeof st.game_mode, "%s", str_of(in, &V.np[4]));
        res.v[0] = (float)online_update_session(&st);
        break;
    }
    case BPK_NET_START_SESSION: res.v[0] = (float)online_start_session(); break;
    case BPK_NET_END_SESSION: res.v[0] = (float)online_end_session(); break;
    case BPK_NET_SET_JOINABLE: res.v[0] = (float)online_set_joinable(A->v[0] != 0.0f); break;
    case BPK_NET_SESSION_STATE: res.v[0] = (float)online_session_state(); break;
    case BPK_NET_SESSION_SETTINGS: {
        const OnlineSessionSettings* st = online_current_settings();
        bool on = online_active();
        set_str(&D[0], on ? st->name : "");
        D[1] = (BpValue){ { on ? (float)st->max_players : 0.0f } }, D[2] = (BpValue){ { on ? (float)st->private_slots : 0.0f } };
        D[3] = (BpValue){ { (float)online_player_count() } }, D[4] = (BpValue){ { (float)(on && st->lan) } };
        D[5] = (BpValue){ { (float)(on && st->password[0]) } }, D[6] = (BpValue){ { (float)(on && st->joinable) } };
        D[7] = (BpValue){ { (float)(on && st->advertise) } }, D[8] = (BpValue){ { (float)(on && st->allow_join_in_progress) } };
        D[9] = (BpValue){ { (float)(on && st->allow_invites) } };
        set_str(&D[10], on ? st->map : ""), set_str(&D[11], on ? st->game_mode : "");
        return true;
    }
    case BPK_NET_BAN: res.v[0] = (float)online_ban((int)A->v[0], str_of(in, B)); break;
    case BPK_NET_UNBAN: online_unban((int)A->v[0]); return true;
    case BPK_NET_UNBAN_ALL: online_unban_all(); return true;
    case BPK_NET_BAN_COUNT: res.v[0] = (float)online_ban_count(); break;
    case BPK_NET_BAN_NAME: set_str(&res, online_ban_name((int)A->v[0])); break;
    case BPK_NET_PLAYER_ID: set_str(&res, online_player_unique_id((int)A->v[0])); break;
    case BPK_NET_LAST_ERROR: set_str(&res, online_last_error()); break;
    case BPK_NET_FRIEND_IN_SESSION: res.v[0] = (float)online_friend_in_session((int)A->v[0]); break;
    case BPK_NET_JOIN_FRIEND: res.v[0] = (float)online_join_friend(w, (int)A->v[0], str_of(in, B)); break;
    case BPK_NET_INVITE_FRIEND: res.v[0] = (float)online_invite_friend((int)A->v[0]); break;
    case BPK_NET_AUTO_INVITES: online_set_auto_accept_invites(A->v[0] != 0.0f); return true;
    case BPK_NET_ACCEPT_INVITE: res.v[0] = (float)online_accept_invite(w); break;
    case BPK_NET_IS_FINDING: res.v[0] = (float)online_finding(); break;
    // --- IK Rig and Physics Asset (ikrig.c): on the actor's Skeletal Mesh ---
    case BPK_PHYS_SET_SIMULATE: phys_set_simulate(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v[0] != 0.0f ? 1 : 0, NULL); return true;
    case BPK_PHYS_SET_BELOW: phys_set_simulate(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), C->v[0] != 0.0f ? 2 : 0, str_of(in, B)); return true;
    case BPK_PHYS_SET_BLEND: phys_set_blend(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v[0]); return true;
    case BPK_PHYS_IMPULSE: phys_add_impulse(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, C), B->v); return true;
    case BPK_PHYS_IS_SIMULATING: res.v[0] = (float)(phys_simulating(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)) != 0); break;
    // --- the active ragdoll (2026-11-16, ikrig.h PHYS_ACTIVE) ---
    case BPK_ACR_SET: phys_set_simulate(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v[0] != 0.0f ? PHYS_ACTIVE : PHYS_OFF, NULL); return true;
    case BPK_ACR_STRENGTH: phys_set_active_strength(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B), C->v[0], V.np[0].v[0]); return true;
    case BPK_ACR_PROFILE: res.v[0] = phys_set_muscle_profile(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), ENUM_NAME("EMuscleProfile", B)) ? 1.0f : 0.0f; break;
    case BPK_ACR_HIT: phys_hit_reaction(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B), C->v, V.np[0].v[0], V.np[1].v[0]); return true;
    case BPK_ACR_BALANCE: res.v[0] = (float)phys_balance_state(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)); break;
    case BPK_ACR_IS_BALANCED: res.v[0] = phys_balance_state(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)) == BAL_BALANCED ? 1.0f : 0.0f; break;
    case BPK_ACR_GET_UP: phys_get_up(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)); return true;
    case BPK_ACR_AUTO_GET_UP: phys_set_auto_get_up(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v[0] != 0.0f, C->v[0]); return true;
    // --- (Q10) the ragdoll's blend by bone, the facing, the dynamic bones, sockets ---
    case BPK_PHYS_BLEND_BELOW: phys_set_blend_below(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B), C->v[0], V.np[0].v[0]); return true;
    case BPK_ACR_WOUND: phys_wound(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B), C->v, V.np[0].v[0]); return true; // (Q27.5)
    case BPK_ACR_GET_LIMP: res.v[0] = phys_limp(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)); break;
    case BPK_ACR_IS_WRITHING: res.v[0] = phys_writhing(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)) ? 1.0f : 0.0f; break;
    case BPK_ACR_FACE_DOWN: res.v[0] = phys_face_down(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)) == 1 ? 1.0f : 0.0f; break;
    case BPK_DYN_SET: dyn_set_enabled(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v[0] != 0.0f); return true;
    case BPK_DYN_WIND: dyn_set_wind(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), B->v); return true;
    case BPK_DYN_RESET: dyn_reset(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e)); return true;
    case BPK_ATTACH_SOCKET: res.v[0] = anim_attach_actor_to_socket(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B), str_of(in, C), V.np[0].v, V.np[1].v) ? 1.0f : 0.0f; break;
    case BPK_DETACH_SOCKET: anim_detach_socket(w, ent_of(A) ? ent_of(A) : in->e); return true;
    case BPK_SOCKET_LOCATION: case BPK_SOCKET_ROTATION: {
        float rows[3][4];
        CTransform st;
        memset(&st, 0, sizeof st);
        if (anim_socket_world(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), rows)) anim_rows_transform(rows, &st);
        memcpy(res.v, kind == BPK_SOCKET_LOCATION ? st.location : st.rotation, 12);
        break;
    }
    // --- (Q12) the First Person Camera (fpcam.h): Target a pawn, none the player; the Camera Shakes on the player's view ---
    case BPK_FP_SET_EXERTION: fpcam_set_exertion(w, ent_of(A), B->v[0]); return true;
    case BPK_FP_SET_FEAR: fpcam_set_fear(w, ent_of(A), B->v[0]); return true;
    case BPK_FP_GET_EXERTION: res.v[0] = fpcam_get_exertion(w, ent_of(A)); break;
    case BPK_FP_GET_FEAR: res.v[0] = fpcam_get_fear(w, ent_of(A)); break;
    case BPK_FP_RECOIL: fpcam_add_recoil(w, ent_of(A), B->v[0], C->v[0], V.np[0].v[0]); return true;
    case BPK_FP_FOCUS_TARGET: fpcam_set_focus_target(w, ent_of(A), ent_of(B)); return true;
    case BPK_FP_FOCUS_DISTANCE: res.v[0] = fpcam_get_focus_distance(w, ent_of(A)); break;
    case BPK_FP_PRESET: res.v[0] = fpcam_set_preset(w, ent_of(A), ENUM_NAME("EFirstPersonPreset", B)) ? 1.0f : 0.0f; break;
    case BPK_SHAKE_START: res.v[0] = (float)fpcam_start_shake(str_of(in, A), B->v[0], 0); break;
    case BPK_SHAKE_STOP: fpcam_stop_shake((uint32_t)A->v[0], B->v[0] != 0.0f); return true;
    case BPK_SHAKE_STOP_ALL: fpcam_stop_all_shakes(A->v[0] != 0.0f); return true;
    case BPK_SHAKE_WORLD: res.v[0] = (float)fpcam_play_world_shake(w, str_of(in, A), B->v, C->v[0], V.np[0].v[0], V.np[1].v[0]); break;
    case BPK_HIDE_BONE: anim_hide_bone(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0] != 0.0f); return true;
    case BPK_IK_SET_GOAL: ikrig_set_goal(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B), C->v, NULL, fminf(fmaxf(V.np[0].v[0], 0.0f), 1.0f)); return true;
    case BPK_IK_CLEAR_GOAL: ikrig_clear_goal(anim_mesh_entity(w, ent_of(A) ? ent_of(A) : in->e), str_of(in, B)); return true;
    // --- vehicles (vehicle.c): the Target or self ---
    case BPK_CLOTH_TEAR: cloth_tear(A->v, B->v[0]); return true;
    // --- (Q9) the soft bodies (cloth.h): the Target or self ---
    case BPK_SOFT_IMPULSE: cloth_soft_impulse(ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0], V.np[0].v); return true;
    case BPK_SOFT_STIFFNESS: cloth_soft_set_prop(w, ent_of(A) ? ent_of(A) : in->e, "Stiffness (Young)", B->v[0]); return true;
    case BPK_SOFT_PINNED: cloth_soft_set_prop(w, ent_of(A) ? ent_of(A) : in->e, "Held", B->v[0] != 0.0f ? 1.0f : 0.0f); return true;
    case BPK_SOFT_PROP_SET: res.v[0] = cloth_soft_set_prop(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]) ? 1.0f : 0.0f; break;
    // --- (Q11) the toon and the outline (toon_actor.h): the Target or self ---
    case BPK_TOON_SET_PARAM: res.v[0] = toon_actor_set_scalar(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]) ? 1.0f : 0.0f; break;
    case BPK_TOON_SET_COLOR: res.v[0] = toon_actor_set_color(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v) ? 1.0f : 0.0f; break;
    case BPK_TOON_GET_PARAM: {
        float tv = 0.0f;
        toon_actor_get_scalar(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), &tv);
        res.v[0] = tv;
        break;
    }
    case BPK_SET_ACTOR_OUTLINE: toon_actor_set_outline(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f ? OUTLINE_ACTOR_ON : OUTLINE_ACTOR_OFF, C->v, N0->v[0]); return true;
    case BPK_SET_LIGHT_TOON: toon_light_set_mode(w, ent_of(A) ? ent_of(A) : in->e, (int)lroundf(B->v[0])); return true;
    // --- (2026-11-25, item 16) the actor's Groom (groom.c groom_of): the Target or self ---
    case BPK_SET_GROOM_VISIBLE: {
        CGroom* gm = groom_of(w, ent_of(A) ? ent_of(A) : in->e);
        if (gm) gm->visible = B->v[0] != 0.0f ? 1u : 0u;
        return true;
    }
    case BPK_SET_GROOM_SIMULATE: {
        CGroom* gm = groom_of(w, ent_of(A) ? ent_of(A) : in->e);
        if (gm) gm->dynamic = B->v[0] != 0.0f ? 1u : 0u;
        return true;
    }
    case BPK_GET_GROOM_VISIBLE: {
        const CGroom* gm = groom_of(w, ent_of(A) ? ent_of(A) : in->e);
        res.v[0] = gm && gm->visible ? 1.0f : 0.0f;
        break;
    }
    case BPK_GET_GROOM_SIMULATE: {
        const CGroom* gm = groom_of(w, ent_of(A) ? ent_of(A) : in->e);
        res.v[0] = gm && gm->dynamic ? 1.0f : 0.0f;
        break;
    }
    case BPK_VR_ACTIVE: res.v[0] = xr_active() ? 1.0f : 0.0f; break;
    case BPK_VR_HEAD: {
        float p[3], f[3], u[3];
        xr_head(p, f, u);
        memcpy(res.v, p, 12), memcpy(D[1].v, f, 12), memcpy(D[2].v, u, 12); // (the outputs after the first: the next registers)
        break;
    }
    case BPK_VR_HAND: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        memcpy(res.v, hnd->pos, 12), memcpy(D[1].v, hnd->fwd, 12), D[2].v[0] = hnd->active ? 1.0f : 0.0f;
        break;
    }
    case BPK_VR_HAND_INPUT: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        res.v[0] = hnd->trigger, D[1].v[0] = hnd->grip;
        D[2].v[0] = hnd->stick[0], D[2].v[1] = hnd->stick[1], D[2].v[2] = 0.0f;
        D[3].v[0] = hnd->primary ? 1.0f : 0.0f, D[4].v[0] = hnd->secondary ? 1.0f : 0.0f;
        break;
    }
    case BPK_VR_HAPTIC: xr_haptic((int)A->v[0], B->v[0], C->v[0]); return true;
    // (Q25) the VR pawn (vr_pawn.h)
    case BPK_VR_TELEPORT: vr_pawn_teleport(w, A->v, B->v[0], C->v[0] != 0.0f); return true;
    case BPK_VR_SET_LOCOMOTION: vr_pawn_set_locomotion((int)lroundf(A->v[0]), (int)lroundf(B->v[0])); return true;
    case BPK_VR_SET_VIGNETTE: vr_pawn_set_vignette(A->v[0]); return true;
    case BPK_VR_SET_HAND_COLLISION: vr_pawn_set_hand_collision(A->v[0] != 0.0f, B->v[0] != 0.0f, C->v[0] != 0.0f); return true;
    case BPK_VR_GRAB: res.v[0] = vr_pawn_grab(w, (int)A->v[0], ent_of(B)) ? 1.0f : 0.0f; break;
    case BPK_VR_RELEASE: vr_pawn_release(w, (int)A->v[0]); return true;
    case BPK_VR_GET_HELD: set_ent(&res, vr_pawn_held((int)A->v[0])); break;
    case BPK_VR_LASER: {
        float hit[3];
        Entity act = ENTITY_NONE;
        res.v[0] = vr_pawn_laser((int)A->v[0], hit, &act) ? 1.0f : 0.0f;
        memcpy(D[1].v, hit, 12), set_ent(&D[2], act);
        break;
    }
    case BPK_VR_HAND_TRACKING: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        res.v[0] = hnd->tracked ? 1.0f : 0.0f, D[1].v[0] = hnd->pinch, D[2].v[0] = hnd->tracked ? hnd->grip : 0.0f;
        break;
    }
    case BPK_VR_SET_REFRESH: res.v[0] = xr_set_refresh(A->v[0]) ? 1.0f : 0.0f; break;
    case BPK_VR_FINGER_CURLS: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        res.v[0] = hnd->curl[0];
        for (int f = 1; f < 5; f++) D[f].v[0] = hnd->curl[f];
        break;
    }
    case BPK_VR_HAND_GESTURE: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        int g = hnd->tracked && hnd->gesture >= 0 && hnd->gesture < XR_GESTURE_COUNT ? hnd->gesture : 0;
        res.v[0] = (float)g;
        set_str(&D[1], g_xr_gesture_names[g]);
        break;
    }
    case BPK_VR_FINGER_TOUCH: {
        const XrHand* hnd = xr_hand((int)A->v[0]);
        int f = (int)B->v[0];
        res.v[0] = f >= 0 && f < 4 ? hnd->touch[f] : 0.0f;
        break;
    }
    case BPK_AR_TRACKING: res.v[0] = arcore_view()->state == ARCORE_TRACKING ? 1.0f : 0.0f; break;
    case BPK_AR_HIT: {
        float p[3] = { 0, 0, 0 }, n[3] = { 0, 0, 1 };
        bool h = arcore_hit(A->v[0], B->v[0], p, n);
        res.v[0] = h ? 1.0f : 0.0f;
        memcpy(D[1].v, p, 12), memcpy(D[2].v, n, 12); // (the outputs after the first: the next registers)
        break;
    }
    case BPK_AR_PLANES: res.v[0] = (float)arcore_view()->planes; break;
    case BPK_EVENT_TOUCH: // (the event's entry: its finger's place, its index)
        res.v[0] = V.touch_xy[0], res.v[1] = V.touch_xy[1], res.v[2] = 0.0f;
        D[1].v[0] = (float)V.touch_finger;
        break;
    case BPK_GET_MOUSE_POS:
        res.v[0] = V.cursor[0];
        D[1].v[0] = V.cursor[1], D[2].v[0] = V.cursor_on ? 1.0f : 0.0f;
        break;
    case BPK_RAIN_SET: water_set_rain(w, A->v[0]); return true;
    case BPK_RAIN_GET: res.v[0] = water_rain_intensity(); break;
    case BPK_RAIN_PROP_SET: res.v[0] = water_set_rain_prop(w, str_of(in, A), B->v[0]) ? 1.0f : 0.0f; break;
    case BPK_RAIN_PROP_GET: res.v[0] = water_get_rain_prop(w, str_of(in, A)); break;
    case BPK_RAIN_PRESET: res.v[0] = water_set_rain_preset(w, ENUM_NAME("ERainPreset", A)) ? 1.0f : 0.0f; break;
    case BPK_RAIN_DROP_COLOR:
        water_set_rain_prop(w, "Drop Color R", A->v[0]), water_set_rain_prop(w, "Drop Color G", A->v[1]), water_set_rain_prop(w, "Drop Color B", A->v[2]);
        return true;
    case BPK_RAIN_SHADING: res.v[0] = water_set_rain_shading_style(w, ENUM_NAME("ERainShading", A)) ? 1.0f : 0.0f; break;
    case BPK_RAIN_PUDDLE_COLOR:
        water_set_rain_prop(w, "Puddle Color R", A->v[0]), water_set_rain_prop(w, "Puddle Color G", A->v[1]), water_set_rain_prop(w, "Puddle Color B", A->v[2]);
        return true;
    case BPK_ACTOR_WETNESS: res.v[0] = water_actor_wetness(ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_SET_ACTOR_WETNESS: water_set_actor_wetness(ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_SET_LIGHTING_LEVEL: res.v[0] = sublevels_set_lighting_visible(NULL, w, str_of(in, A), B->v[0] != 0.0f, true) ? 1.0f : 0.0f; break;
    case BPK_GET_LIGHTING_LEVEL: res.v[0] = sublevels_lighting_visible(str_of(in, A)) ? 1.0f : 0.0f; break;
    case BPK_SET_LIGHTING_BLEND: res.v[0] = sublevels_set_lighting_blend(NULL, w, str_of(in, A), B->v[0], true) ? 1.0f : 0.0f; break;
    case BPK_GET_LIGHTING_BLEND: res.v[0] = sublevels_lighting_blend(str_of(in, A)); break;
    case BPK_VEH_THROTTLE: vehicle_set_control(w, ent_of(A) ? ent_of(A) : in->e, VI_THROTTLE, B->v[0]); return true;
    case BPK_VEH_STEER: vehicle_set_control(w, ent_of(A) ? ent_of(A) : in->e, VI_STEER, B->v[0]); return true;
    case BPK_VEH_BRAKE: vehicle_set_control(w, ent_of(A) ? ent_of(A) : in->e, VI_BRAKE, B->v[0]); return true;
    case BPK_VEH_HANDBRAKE: vehicle_set_control(w, ent_of(A) ? ent_of(A) : in->e, VI_HANDBRAKE, B->v[0] != 0.0f ? 1.0f : 0.0f); return true;
    case BPK_VEH_FLIGHT: {
        Entity ve = ent_of(A) ? ent_of(A) : in->e;
        vehicle_set_control(w, ve, VI_PITCH, B->v[0]), vehicle_set_control(w, ve, VI_ROLL, C->v[0]);
        vehicle_set_control(w, ve, VI_YAW, V.np[0].v[0]), vehicle_set_control(w, ve, VI_COLLECTIVE, V.np[1].v[0]);
        return true;
    }
    case BPK_VEH_GEAR: vehicle_shift(w, ent_of(A) ? ent_of(A) : in->e, (int)B->v[0]); return true;
    case BPK_VEH_ENGINE: vehicle_set_engine(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_VEH_PART_SET: vehicle_part_target(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]); return true;
    case BPK_VEH_PART_TOGGLE: vehicle_part_toggle(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); return true;
    case BPK_VEH_PART_GET: res.v[0] = vehicle_part_value(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); break;
    case BPK_VEH_ENTER: res.v[0] = (float)vehicle_enter(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_VEH_EXIT: res.v[0] = (float)vehicle_exit(w); break;
    case BPK_VEH_SPEED: case BPK_VEH_RPM: case BPK_VEH_GET_GEAR: {
        VehInfo vi;
        bool ok = vehicle_info(w, ent_of(A) ? ent_of(A) : in->e, &vi);
        res.v[0] = !ok ? 0.0f : kind == BPK_VEH_SPEED ? vi.speed_kmh : kind == BPK_VEH_RPM ? vi.rpm : (float)vi.gear;
        break;
    }
    // --- particles (particles.c) ---
    case BPK_PS_SPAWN: { // A: the system's name (a string index), B: location, C: rotation, np[0]: auto destroy
        const char* name = str_of(in, A);
        uint32_t h = 0;
        for (uint32_t i = 0; name && i < ps_asset_count() && !h; i++)
            if (ps_asset_at(i) && !strcmp(ps_asset_at(i)->name, name)) h = ps_asset_at(i)->hash;
        set_ent(&res, h ? particles_spawn(w, h, B->v, C->v, V.np[0].v[0] != 0.0f) : ENTITY_NONE);
        break;
    }
    case BPK_PS_ACTIVATE: particles_activate(w, ent_of(A) ? ent_of(A) : in->e, true, B->v[0] != 0.0f); return true;
    case BPK_PS_DEACTIVATE: particles_activate(w, ent_of(A) ? ent_of(A) : in->e, false, false); return true;
    case BPK_PS_SET_COLOR: {
        CParticleSystem* c = ecs_get(w, ent_of(A) ? ent_of(A) : in->e, COMP_PARTICLE_SYSTEM);
        if (c) memcpy(c->color, B->v, 12), c->color[3] = 1.0f;
        return true;
    }
    case BPK_PS_SET_SPAWN: {
        CParticleSystem* c = ecs_get(w, ent_of(A) ? ent_of(A) : in->e, COMP_PARTICLE_SYSTEM);
        if (c) c->spawn_scale = fmaxf(B->v[0], 0.0f);
        return true;
    }
    case BPK_PS_IS_ACTIVE: {
        const CParticleSystem* c = ecs_get(w, ent_of(A) ? ent_of(A) : in->e, COMP_PARTICLE_SYSTEM);
        res.v[0] = (float)(c && c->active);
        break;
    }
    // --- saves (online.c) ---
    case BPK_SAVE_GAME: res.v[0] = (float)online_save(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); break;
    case BPK_LOAD_GAME: res.v[0] = (float)online_load(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); break;
    case BPK_SAVE_EXISTS: res.v[0] = (float)online_save_exists(str_of(in, A)); break;
    case BPK_DELETE_SAVE: online_save_delete(str_of(in, A)); return true;
    // --- Physics Constraints: Target unconnected is self ---
    case BPK_CONSTRAINT_BREAK: physics_constraint_break(w, ent_of(A) ? ent_of(A) : in->e); return true;
    case BPK_CONSTRAINT_MOTOR: physics_constraint_motor(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f, C->v[0]); return true;
    // --- (Q8) rigid bodies and constraints: Target unconnected is self; inputs past the third staged (N0, N1, np) ---
    case BPK_PHYS_ADD_FORCE: physics_add_force(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] != 0.0f); return true;
    case BPK_PHYS_ADD_TORQUE: physics_add_torque(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] != 0.0f); return true;
    case BPK_PHYS_ANG_IMPULSE: physics_add_angular_impulse(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] != 0.0f); return true;
    case BPK_PHYS_IMPULSE_AT: physics_add_impulse_at(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v); return true;
    case BPK_PHYS_SET_LIN_VEL: physics_set_linear_velocity(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] != 0.0f); return true;
    case BPK_PHYS_SET_ANG_VEL: physics_set_angular_velocity(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] != 0.0f); return true;
    case BPK_PHYS_GET_ANG_VEL: physics_get_angular_velocity(w, ent_of(A) ? ent_of(A) : in->e, res.v); break;
    case BPK_PHYS_SET_MASS: physics_set_mass(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_PHYS_GET_MASS: res.v[0] = physics_get_mass(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_PHYS_WAKE: physics_wake(w, ent_of(A) ? ent_of(A) : in->e, true); return true;
    case BPK_PHYS_SLEEP: physics_wake(w, ent_of(A) ? ent_of(A) : in->e, false); return true;
    case BPK_PHYS_IS_AWAKE: res.v[0] = physics_is_awake(w, ent_of(A) ? ent_of(A) : in->e) ? 1.0f : 0.0f; break;
    case BPK_PHYS_SET_GRAVITY: physics_set_gravity(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_PHYS_SET_MATERIAL: {
        const char* m = str_of(in, B);
        physics_set_material(w, ent_of(A) ? ent_of(A) : in->e, m && m[0] ? physmat_vpath_hash(m) : 0u);
        if (m && m[0]) physmat_remember(m);
        return true;
    }
    case BPK_PHYS_SET_CCD: physics_set_ccd(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_CONSTRAINT_LIN_DRIVE: case BPK_CONSTRAINT_ANG_DRIVE: { // (Target, the two vectors; the bits, Frequency, Damping, Max staged)
        uint32_t bits = (N0->v[0] != 0.0f ? CDRIVE_POSITION : 0u) | (N1->v[0] != 0.0f ? CDRIVE_VELOCITY : 0u);
        physics_constraint_drive(w, ent_of(A) ? ent_of(A) : in->e, kind == BPK_CONSTRAINT_ANG_DRIVE, B->v, C->v, bits, V.np[2].v[0], V.np[3].v[0], V.np[4].v[0]);
        return true;
    }
    case BPK_CONSTRAINT_LIMITS: physics_constraint_limits(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], C->v[0], N0->v[0], N1->v[0]); return true;
    case BPK_CONSTRAINT_MOTION: physics_constraint_motion(w, ent_of(A) ? ent_of(A) : in->e, (uint32_t)B->v[0], (uint32_t)C->v[0]); return true;
    case BPK_CONSTRAINT_FORCE: {
        float f = 0.0f, t = 0.0f;
        bool broke = physics_constraint_force(w, ent_of(A) ? ent_of(A) : in->e, &f, &t);
        res.v[0] = f, D[1].v[0] = t, D[2].v[0] = broke ? 1.0f : 0.0f;
        break;
    }
    // --- Morph targets (anim.h): Target unconnected is self ---
    case BPK_MORPH_SET: anim_morph_set(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]); return true;
    case BPK_MORPH_GET: res.v[0] = anim_morph_get(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); break;
    // --- Health and damage (health.h): Target unconnected is self; the actions not in the editor's Construction Script ---
    case BPK_APPLY_DAMAGE:
        if (V.editor_cs) return true;
        res.v[0] = health_apply_damage(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], str_of(in, C), ent_of(N0), ent_of(N1));
        break;
    case BPK_APPLY_RADIAL_DAMAGE: { // Base Damage, Origin, Radius; Type, Instigator, Causer, Falloff, Minimum Damage staged
        if (V.editor_cs) return true;
        uint32_t hits = health_apply_radial(w, B->v, C->v[0], A->v[0], V.np[4].v[0], V.np[3].v[0], str_of(in, N0), ent_of(N1), ent_of(&V.np[2]));
        res.v[0] = hits ? 1.0f : 0.0f, D[1].v[0] = (float)hits;
        break;
    }
    case BPK_HEAL:
        if (V.editor_cs) return true;
        res.v[0] = health_heal(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], ent_of(C));
        break;
    case BPK_GET_HEALTH: res.v[0] = health_get(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_GET_HEALTH_PERCENT: res.v[0] = health_percent(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_GET_MAX_HEALTH: res.v[0] = health_get_max(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_GET_SHIELD: {
        Entity e = ent_of(A) ? ent_of(A) : in->e;
        float s = health_shield(w, e), m = health_shield_max(w, e);
        res.v[0] = s, D[1].v[0] = m > 0.0f ? fminf(s / m, 1.0f) : 0.0f, D[2].v[0] = m;
        break;
    }
    case BPK_IS_DEAD: res.v[0] = (float)health_is_dead(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_SET_MAX_HEALTH: if (!V.editor_cs) health_set_max(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], C->v[0] != 0.0f); return true;
    case BPK_SET_SHIELD: if (!V.editor_cs) health_set_shield(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], C->v[0]); return true;
    case BPK_REVIVE: if (!V.editor_cs) health_revive(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]); return true;
    case BPK_SET_INVINCIBLE: if (!V.editor_cs) health_set_invincible(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_SET_DAMAGE_MULT: if (!V.editor_cs) health_set_multiplier(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]); return true;
    // --- Gameplay Tags (gameplay_tags.h): Target unconnected is self ---
    case BPK_TAG_ADD: gt_add(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); return true;
    case BPK_TAG_REMOVE: gt_remove(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); return true;
    case BPK_TAG_HAS:
    case BPK_TAG_HAS_EXACT: res.v[0] = (float)gt_has(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), kind == BPK_TAG_HAS_EXACT); break;
    case BPK_TAG_MATCHES: res.v[0] = (float)gt_matches(str_of(in, A), str_of(in, B), C->v[0] != 0.0f); break;
    case BPK_GET_ALL_WITH_TAG:
    case BPK_GET_ALL_WITH_GTAG: { // only the archetypes with Gameplay Tags are read (as gt_find_actors)
        Arr* out = arr_get(D);
        if (!out) return true;
        out->count = 0;
        const char* q = str_of(in, A);
        bool exact = kind == BPK_GET_ALL_WITH_TAG || B->v[0] != 0.0f;
        for (uint32_t ai = 0; q[0] && ai < w->archetype_count; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)) || !ar->count || (ar->mask & COMP_BIT(COMP_ATTACH))) continue;
            const CGameplayTags* g = ecs_arch_column(ar, COMP_GAMEPLAY_TAGS);
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t i = 0; i < ar->count; i++)
                for (uint32_t t = 0; t < g[i].count && t < GT_MAX_TAGS; t++)
                    if (gt_matches(g[i].tags[t], q, exact)) {
                        BpValue v;
                        set_ent(&v, ents[i]);
                        arr_push(in, out, &v);
                        break;
                    }
        }
        return true;
    }
    // --- the NavMesh (navmesh.h; the grid while it is built): the outputs after the first in the next registers ---
    case BPK_NAV_PROJECT: {
        float ext[3] = { B->v[0] > 0.0f ? B->v[0] : 150.0f, B->v[1] > 0.0f ? B->v[1] : 150.0f, B->v[2] > 0.0f ? B->v[2] : 300.0f }, p[3];
        bool ok = ai_nav_project(A->v, ext, p);
        memcpy(res.v, ok ? p : A->v, 12);
        D[1].v[0] = ok ? 1.0f : 0.0f;
        break;
    }
    case BPK_NAV_RANDOM_REACHABLE: {
        float p[3];
        bool ok = ai_nav_random_point(A->v, B->v[0] > 1.0f ? B->v[0] : 500.0f, p);
        memcpy(res.v, ok ? p : A->v, 12);
        D[1].v[0] = ok ? 1.0f : 0.0f;
        break;
    }
    // --- Pause and options (game_settings.h) ---
    case BPK_SET_GAME_PAUSED: {
        bool p = A->v[0] != 0.0f;
        res.v[0] = p != game_paused() ? 1.0f : 0.0f;
        game_set_paused(p);
        break;
    }
    case BPK_IS_GAME_PAUSED: res.v[0] = game_paused() ? 1.0f : 0.0f; break;
    case BPK_SET_TICK_PAUSED: {
        Entity t = ent_of(A) ? ent_of(A) : in->e;
        Inst* ti = inst_of(w, t);
        if (ti) ti->paused_tick = B->v[0] != 0.0f;
        CBlueprint* cb = ecs_get(w, t, COMP_BLUEPRINT);
        if (cb) cb->flags = B->v[0] != 0.0f ? cb->flags | BP_FLAG_TICK_WHEN_PAUSED : cb->flags & ~BP_FLAG_TICK_WHEN_PAUSED;
        return true;
    }
    case BPK_GET_GUS: {
        char t[96] = "";
        float v = 0.0f;
        gus_get(str_of(in, A), &v, t, sizeof t);
        res.v[0] = v;
        set_str(&D[1], t);
        break;
    }
    case BPK_SET_GUS: {
        const char* name = str_of(in, A);
        res.v[0] = gus_set(name, B->v[0], str_of(in, C)) ? 1.0f : 0.0f;
        if (res.v[0] != 0.0f) gus_apply_live();
        break;
    }
    case BPK_APPLY_GUS: gus_apply(true); return true;
    case BPK_LOAD_GUS: gus_load(), gus_apply(false); return true;
    case BPK_DEFAULTS_GUS: gus_reset(), gus_apply_live(); return true;
    case BPK_REMAP_KEY: res.v[0] = gus_remap(str_of(in, A), str_of(in, B), str_of(in, C)) ? 1.0f : 0.0f; break;
    case BPK_GET_ACTION_KEY: set_str(&res, gus_action_key(str_of(in, A), B->v[0] != 0.0f)); break;
    case BPK_OPEN_PAUSE_MENU: pause_menu_open(w); return true;
    case BPK_CLOSE_PAUSE_MENU: pause_menu_close(w); return true;
    case BPK_MAIN_MENU: {
        pause_menu_close(w);
        const ProjectSettings* ps = project();
        const char* map = ps->main_menu_map[0] ? ps->main_menu_map : ps->game_default_map;
        char lname[128] = "";
        const char* b = strrchr(map, '/');
        snprintf(lname, sizeof lname, "%s", b ? b + 1 : map);
        char* dot = strrchr(lname, '.');
        if (dot) *dot = 0;
        if (!lname[0] && V.hooks.current) snprintf(lname, sizeof lname, "%s", V.hooks.current());
        if (lname[0] && V.hooks.open) V.hooks.open(lname);
        return true;
    }
    case BPK_QUIT_GAME: game_request_quit(); return true;
    // --- Dialogue (dialogue.h): Target unconnected is self ---
    case BPK_DLG_START: { // (Player unconnected reads self: the speaker talking to itself means the possessed pawn)
        Entity sp = ent_of(A) ? ent_of(A) : in->e, pl = ent_of(C);
        res.v[0] = (float)dlg_start_speaker(w, sp, str_of(in, B), pl == sp ? ENTITY_NONE : pl);
        break;
    }
    case BPK_DLG_CHOOSE: res.v[0] = (float)dlg_choose(w, (int)A->v[0]); break;
    case BPK_DLG_SKIP: dlg_skip(w); return true;
    case BPK_DLG_END: dlg_end(w); return true;
    case BPK_DLG_IS_IN: {
        Entity sp = ENTITY_NONE;
        res.v[0] = (float)dlg_active(&sp);
        set_ent(&D[1], sp);
        break;
    }
    case BPK_DLG_SET_LANGUAGE: dlg_set_language(dlg_language_from_code(str_of(in, A))); return true;
    // --- Quests (quest.h): the quest by its asset's name, the objective by its id or index; Quest Log unconnected: the player's ---
    case BPK_QST_START: res.v[0] = (float)quest_start(w, ent_of(B), str_of(in, A)); break;
    case BPK_QST_COMPLETE: res.v[0] = (float)quest_complete(w, ent_of(B), str_of(in, A)); break;
    case BPK_QST_FAIL: res.v[0] = (float)quest_fail(w, ent_of(B), str_of(in, A)); break;
    case BPK_QST_ADVANCE: case BPK_QST_SET_PROGRESS:
        res.v[0] = (float)quest_advance(w, ent_of(N0), str_of(in, A), str_of(in, B), (int)C->v[0], kind == BPK_QST_SET_PROGRESS);
        break;
    case BPK_QST_IS_ACTIVE: res.v[0] = (float)(quest_state(w, ent_of(B), str_of(in, A), NULL) == QST_ACTIVE); break;
    case BPK_QST_IS_COMPLETED: res.v[0] = (float)(quest_state(w, ent_of(B), str_of(in, A), NULL) == QST_COMPLETED); break;
    case BPK_QST_GET_STATE: {
        int stage = 0, s = quest_state(w, ent_of(B), str_of(in, A), &stage);
        res.v[0] = (float)s;
        set_str(&D[1], g_qst_state_names[s % QST_STATE_COUNT]);
        D[2].v[0] = (float)stage;
        break;
    }
    case BPK_QST_GET_PROGRESS: {
        int p = 0, n = 0;
        bool done = false;
        const char* t = "";
        quest_objective(w, ent_of(C), str_of(in, A), str_of(in, B), &p, &n, &done, &t);
        res.v[0] = (float)p, D[1].v[0] = (float)n, D[2].v[0] = done ? 1.0f : 0.0f;
        set_str(&D[3], t);
        break;
    }
    case BPK_QST_TRACK: res.v[0] = (float)quest_track(w, ent_of(B), str_of(in, A)); break;
    case BPK_QST_GET_TRACKED: {
        const Quest* q = quest_tracked(w, ent_of(A));
        set_str(&res, q ? q->name : "");
        set_str(&D[1], q ? quest_title(q, quest_language()) : "");
        break;
    }
    // --- Game Rules (game_rules.h): Target unconnected (it reads self) is the local player, unless self is a player ---
#define GR_PLAYER gr_player(w, in, A)
    case BPK_GR_GET_PHASE: {
        float left = 0.0f;
        int rounds = 1, ph = rules_phase(&left);
        res.v[0] = (float)ph;
        set_str(&D[1], g_match_phase_names[ph % MATCH_PHASE_COUNT]);
        D[2].v[0] = left, D[3].v[0] = (float)rules_round(&rounds), D[4].v[0] = (float)rounds;
        break;
    }
    case BPK_GR_SET_PHASE: res.v[0] = (float)rules_set_phase(w, (int)A->v[0]); break;
    case BPK_GR_START: res.v[0] = (float)rules_start_match(w); break;
    case BPK_GR_END: res.v[0] = (float)rules_end_match(w, A->v[0] != 0.0f, str_of(in, B), gr_player(w, in, C)); break;
    case BPK_GR_ADD_SCORE: res.v[0] = (float)rules_add_score(w, GR_PLAYER, (int32_t)B->v[0]); break;
    case BPK_GR_GET_SCORE: {
        const RulesSlot* s = rules_slot(w, GR_PLAYER);
        res.v[0] = (float)rules_score(w, GR_PLAYER);
        D[1].v[0] = s ? (float)s->kills : 0.0f, D[2].v[0] = s ? (float)s->deaths : 0.0f, D[3].v[0] = (float)rules_team_of(w, GR_PLAYER);
        break;
    }
    case BPK_GR_TEAM_SCORE: {
        int t = (int)A->v[0];
        res.v[0] = (float)rules_team_score(t);
        D[1].v[0] = (float)rules_team_rounds(t);
        break;
    }
    case BPK_GR_TIME: {
        float el = 0.0f, left = rules_time_remaining(&el);
        char tm[24];
        rules_format_time(rules_counts_down() ? left : el, tm, sizeof tm);
        res.v[0] = left, D[1].v[0] = el;
        set_str(&D[2], tm);
        break;
    }
    case BPK_GR_SET_TIMER: rules_set_timer(w, A->v[0], B->v[0] != 0.0f); return true;
    case BPK_GR_GET_LIVES: {
        bool inf = false;
        const RulesSlot* s = rules_slot(w, GR_PLAYER);
        res.v[0] = (float)rules_lives(w, GR_PLAYER, &inf);
        D[1].v[0] = inf ? 1.0f : 0.0f, D[2].v[0] = s && s->out ? 1.0f : 0.0f;
        break;
    }
    case BPK_GR_ADD_LIVES: res.v[0] = (float)rules_add_lives(w, GR_PLAYER, (int32_t)B->v[0]); break;
    case BPK_GR_RESPAWN: res.v[0] = (float)rules_respawn(w, GR_PLAYER); break;
    case BPK_GR_RESTART: rules_restart(w, A->v[0] != 0.0f); return true;
    case BPK_GR_RESULT: {
        bool won = false;
        char why[64];
        Entity win = ENTITY_NONE;
        bool over = rules_result(&won, why, sizeof why, &win);
        res.v[0] = won ? 1.0f : 0.0f;
        set_str(&D[1], why), set_ent(&D[2], win);
        D[3].v[0] = over ? 1.0f : 0.0f;
        break;
    }
#undef GR_PLAYER
    // --- The HUD (hud.h): Target unconnected is self ---
    case BPK_HUD_MESSAGE: res.v[0] = (float)hud_message(str_of(in, A), B->v[0], C->v); break;
    case BPK_HUD_ADD_COMPASS:
        res.v[0] = (float)hud_add_marker(w, ent_of(A) ? ent_of(A) : in->e, NULL, str_of(in, B), C->v, 0.0f, HUD_MK_COMPASS | HUD_MK_MINIMAP | (N0->v[0] != 0.0f ? HUD_MK_DISTANCE : 0u));
        break;
    case BPK_HUD_REMOVE_COMPASS: res.v[0] = (float)(hud_remove_marker(ent_of(A) ? ent_of(A) : in->e, HUD_MK_COMPASS) > 0); break;
    case BPK_HUD_ADD_SCREEN: // (Target, Label, Color; Height Offset, Show Off Screen, Show Distance staged)
        res.v[0] = (float)hud_add_marker(w, ent_of(A) ? ent_of(A) : in->e, NULL, str_of(in, B), C->v, N0->v[0],
                                         HUD_MK_SCREEN | (N1->v[0] != 0.0f ? HUD_MK_OFFSCREEN : 0u) | (V.np[2].v[0] != 0.0f ? HUD_MK_DISTANCE : 0u));
        break;
    case BPK_HUD_REMOVE_SCREEN: res.v[0] = (float)(hud_remove_marker(ent_of(A) ? ent_of(A) : in->e, HUD_MK_SCREEN) > 0); break;
    case BPK_HUD_DAMAGE_NUMBER: hud_damage_number(A->v, B->v[0], C->v[0] != 0.0f, N0->v[0] != 0.0f); return true;
    case BPK_HUD_SET_VISIBLE: hud_set_visible(A->v[0] != 0.0f); return true;
    case BPK_HUD_BIND_BAR: res.v[0] = (float)hud_bind_bar(w, ent_of(A), str_of(in, B), ent_of(C), str_of(in, N0)); break;
    case BPK_HUD_SET_SPREAD: hud_set_spread(A->v[0]); return true;
    case BPK_HUD_PHOTO_MODE: hud_set_photo_mode(A->v[0] != 0.0f); return true;
    case BPK_HUD_IS_VISIBLE: res.v[0] = (float)hud_shown(); break;
    // --- The render style (render.h, A9.0) and the decals (decal.h, M6) ---
    case BPK_SET_RENDER_STYLE: render_set_style_override((int)lroundf(A->v[0])); return true;
    case BPK_WP_SET_LAYER: res.v[0] = (float)wp_set_layer_state(str_of(in, A), (int)lroundf(B->v[0])); break;
    case BPK_AUDIO_SET_PARAM: res.v[0] = (float)audio_component_set_param(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0]); break;
    case BPK_WP_GET_LAYER: res.v[0] = (float)wp_get_layer_state(str_of(in, A)); break;
    case BPK_WP_LOCATION_LOADED: res.v[0] = (float)wp_location_loaded(A->v); break;
    case BPK_GET_RENDER_STYLE: res.v[0] = (float)render_style(), set_str(&D[1], render_style_name(render_style())); break;
    case BPK_SPAWN_DECAL: { // A: the material's name (a string index), B: location, C: rotation, np[0]: size, np[1]: life span
        const char* name = str_of(in, A);
        uint32_t h = 0;
        for (uint32_t i = 0; name && i < mat_count() && !h; i++)
            if (!strcmp(mat_at(i)->name, name)) h = mat_at(i)->hash;
        set_ent(&res, h ? decal_spawn(w, h, B->v, C->v, V.np[0].v, V.np[1].v[0]) : ENTITY_NONE);
        break;
    }
    case BPK_NAV_PATH_LENGTH: {
        float len = ai_nav_path_length(A->v, B->v);
        res.v[0] = len, D[1].v[0] = len >= 0.0f ? 1.0f : 0.0f;
        break;
    }
    // --- Inventory and Items (inventory.h): the item by its Item Definition's name; Target unconnected is self ---
    case BPK_INV_ADD: {
        const ItemDef* it = inv_item_by_name(str_of(in, B));
        int want = C->v[0] > 0.0f ? (int)C->v[0] : 1, n = it ? inv_add(w, ent_of(A) ? ent_of(A) : in->e, it->hash, want) : 0;
        res.v[0] = n == want ? 1.0f : 0.0f;
        D[1].v[0] = (float)(want - n);
        break;
    }
    case BPK_INV_REMOVE: {
        const ItemDef* it = inv_item_by_name(str_of(in, B));
        res.v[0] = (float)(it ? inv_remove(w, ent_of(A) ? ent_of(A) : in->e, it->hash, (int)C->v[0]) : 0);
        break;
    }
    case BPK_INV_HAS: case BPK_INV_COUNT: {
        const ItemDef* it = inv_item_by_name(str_of(in, B));
        int n = it ? inv_count(w, ent_of(A) ? ent_of(A) : in->e, it->hash) : 0;
        res.v[0] = kind == BPK_INV_COUNT ? (float)n : n >= (C->v[0] > 0.0f ? (int)C->v[0] : 1) ? 1.0f : 0.0f;
        break;
    }
    case BPK_INV_USE: case BPK_INV_EQUIP: {
        const ItemDef* it = inv_item_by_name(str_of(in, B));
        Entity t = ent_of(A) ? ent_of(A) : in->e;
        res.v[0] = (float)(it && (kind == BPK_INV_USE ? inv_use(w, t, it->hash) : inv_equip(w, t, it->hash)));
        break;
    }
    case BPK_INV_UNEQUIP: res.v[0] = (float)inv_unequip(w, ent_of(A) ? ent_of(A) : in->e, inv_equip_slot_of(str_of(in, B))); break;
    case BPK_INV_DROP: {
        const ItemDef* it = inv_item_by_name(str_of(in, B));
        set_ent(&res, it ? inv_drop(w, ent_of(A) ? ent_of(A) : in->e, it->hash, (int)C->v[0]) : ENTITY_NONE);
        break;
    }
    case BPK_INV_GET_SLOT: {
        uint32_t item = 0;
        int n = 0;
        bool has = inv_get_slot(w, ent_of(A) ? ent_of(A) : in->e, (int)B->v[0], &item, &n);
        const ItemDef* it = inv_item_by_hash(item);
        set_str(&res, it ? it->name : "None");
        D[1].v[0] = (float)n, D[2].v[0] = has ? 0.0f : 1.0f;
        break;
    }
    case BPK_INV_MOVE: res.v[0] = (float)inv_move(w, ent_of(A) ? ent_of(A) : in->e, (int)B->v[0], (int)C->v[0]); break;
    case BPK_INV_FIND_TAG: {
        uint32_t item = 0;
        int slot = -1;
        bool f = inv_find_tag(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), &item, &slot);
        const ItemDef* it = inv_item_by_hash(item);
        set_str(&res, it ? it->name : "None");
        D[1].v[0] = (float)slot, D[2].v[0] = f ? 1.0f : 0.0f;
        break;
    }
    case BPK_INV_SPLIT: res.v[0] = (float)inv_split(w, ent_of(A) ? ent_of(A) : in->e, (int)B->v[0], (int)C->v[0]); break;
    case BPK_INV_SORT: inv_sort(w, ent_of(A) ? ent_of(A) : in->e); return true;
    case BPK_INV_GET_EQUIPPED: {
        const ItemDef* it = inv_item_by_hash(inv_equipped(w, ent_of(A) ? ent_of(A) : in->e, inv_equip_slot_of(str_of(in, B))));
        set_str(&res, it ? it->name : "None");
        D[1].v[0] = it ? 1.0f : 0.0f;
        break;
    }
    case BPK_INV_WEIGHT: {
        float mx = 0.0f;
        res.v[0] = inv_weight(w, ent_of(A) ? ent_of(A) : in->e, &mx);
        D[1].v[0] = mx;
        break;
    }
    case BPK_INV_ITEM_INFO: { // Display Name, Description, Weight, Max Stack, Value, Equip Slot
        const ItemDef* it = inv_item_by_name(str_of(in, A));
        set_str(&res, it ? loc_item(it, false) : ""); // (2026-11-24: in the culture, loc.h)
        set_str(&D[1], it ? loc_item(it, true) : "");
        D[2].v[0] = it ? it->weight : 0.0f, D[3].v[0] = it ? (float)it->max_stack : 0.0f, D[4].v[0] = it ? (float)it->value : 0.0f;
        set_str(&D[5], it && it->equip >= 0 ? g_inv_equip_names[it->equip] : "None");
        break;
    }
    case BPK_INV_PICK_UP: res.v[0] = (float)inv_pick_up(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B)); break;
    case BPK_INV_TRANSFER: res.v[0] = (float)inv_transfer(w, ent_of(A) ? ent_of(A) : in->e, (int)B->v[0], ent_of(C), (int)V.np[0].v[0]); break;
    case BPK_INV_SLOT_COUNT: res.v[0] = (float)inv_slot_count(w, ent_of(A) ? ent_of(A) : in->e); break;
    // --- Save and load the progress (save_progress.h): a slot unconnected is "" (refused) ---
    case BPK_CREATE_SAVE_OBJECT: set_ent(&res, progress_create_save_object(w, u32_of(A))); break;
    case BPK_GET_PROGRESS_SAVE: set_ent(&res, progress_save_game(w)); break;
    case BPK_SET_PROGRESS_SAVE: progress_set_save_game(w, ent_of(A)); return true;
    case BPK_SAVE_PROGRESS: res.v[0] = (float)progress_save(w, str_of(in, A), str_of(in, B), 0); break;
    case BPK_LOAD_PROGRESS: res.v[0] = (float)progress_load(w, str_of(in, A)); break;
    case BPK_NEW_GAME: progress_new_game(w, str_of(in, A)); return true;
    case BPK_AUTO_SAVE: res.v[0] = (float)progress_auto_save(w); break;
    case BPK_LIST_SAVE_SLOTS: case BPK_GET_SLOT_NAME: {
        static SaveSlotInfo list[SAVE_MAX_SLOTS]; // (a node's call, not every frame: the folder's headers read)
        uint32_t n = progress_list(list, SAVE_MAX_SLOTS, kind == BPK_LIST_SAVE_SLOTS && A->v[0] != 0.0f);
        if (kind == BPK_GET_SLOT_NAME) {
            int k = (int)A->v[0];
            set_str(&res, k >= 0 && (uint32_t)k < n ? list[k].slot : "");
            break;
        }
        Arr* out = arr_get(D);
        if (out) {
            out->count = 0;
            for (uint32_t i = 0; i < n; i++) {
                BpValue v;
                set_str(&v, list[i].slot);
                arr_push(in, out, &v);
            }
        }
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = (float)n;
        return true;
    }
    case BPK_GET_SLOT_INFO: { // Return Value, Display Name, Level Name, Date Time, Play Time, Play Time Text, Is Auto Save
        SaveSlotInfo si;
        bool ok = progress_slot_info(str_of(in, A), &si);
        if (!ok) memset(&si, 0, sizeof si);
        char when[40] = "", pt[32] = "";
        if (ok) progress_format_date(si.time, when, sizeof when), progress_format_duration(si.play_time, pt, sizeof pt);
        res.v[0] = ok ? 1.0f : 0.0f;
        set_str(&D[1], si.display), set_str(&D[2], si.level), set_str(&D[3], when);
        memset(&D[4], 0, sizeof D[4]), D[4].v[0] = (float)si.play_time;
        set_str(&D[5], pt);
        memset(&D[6], 0, sizeof D[6]), D[6].v[0] = (si.flags & (SAVE_FLAG_AUTO | SAVE_FLAG_CHECKPOINT)) ? 1.0f : 0.0f;
        break;
    }
    case BPK_GET_LATEST_SLOT: {
        char s[SAVE_SLOT_NAME] = "";
        bool f = progress_latest(s, sizeof s);
        set_str(&res, s);
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = f ? 1.0f : 0.0f;
        break;
    }
    case BPK_GET_PLAY_TIME: {
        char t[32];
        progress_format_duration(progress_play_time(), t, sizeof t);
        res.v[0] = (float)progress_play_time();
        set_str(&D[1], t);
        break;
    }
    case BPK_GET_CURRENT_SLOT: set_str(&res, progress_current_slot()); break;
    case BPK_SET_AUTO_SAVE_INTERVAL: progress_set_autosave_interval(A->v[0]); return true;
    case BPK_RESPAWN_CHECKPOINT: res.v[0] = (float)progress_respawn(w, ent_of(A) ? ent_of(A) : bp_player(w), B->v[0] != 0.0f); break;
    case BPK_GET_CHECKPOINT: {
        Entity c = progress_checkpoint(w);
        set_ent(&res, c);
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = c != ENTITY_NONE ? 1.0f : 0.0f;
        break;
    }
    case BPK_SET_CHECKPOINT: progress_set_checkpoint(w, ent_of(A)); return true;
    case BPK_OPEN_SAVE_MENU: res.v[0] = (float)save_menu_open(w, A->v[0] != 0.0f); break;
    // --- Interaction (interaction.h): Target unconnected is self (the getters: self when it has an Interactor, else the player) ---
    case BPK_IA_INTERACT: res.v[0] = (float)interaction_interact(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B)); break;
    case BPK_IA_GET_FOCUSED: case BPK_IA_GET_PROMPT: {
        Entity t = ent_of(A) ? ent_of(A) : ecs_get(w, in->e, COMP_INTERACTOR) ? in->e : V.player;
        if (kind == BPK_IA_GET_FOCUSED) {
            Entity f = interaction_focused(w, t);
            set_ent(&res, f);
            D[1].v[0] = f ? 1.0f : 0.0f;
            break;
        }
        char text[96], key[32];
        float pr = 0.0f;
        bool can = false, has = interaction_prompt(w, t, text, sizeof text, key, sizeof key, &pr, &can);
        set_str(&res, text);
        set_str(&D[1], key);
        D[2].v[0] = pr, D[3].v[0] = has ? 1.0f : 0.0f, D[4].v[0] = can ? 1.0f : 0.0f;
        break;
    }
    case BPK_IA_SET_ENABLED: interaction_set_enabled(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_IA_SET_PROMPT: interaction_set_prompt(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)); return true;
    case BPK_IA_DEVICE_SET: interaction_device_set(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f, in->e); return true;
    case BPK_IA_DEVICE_IS: res.v[0] = (float)interaction_device_active(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_ABIL_GIVE: case BPK_ABIL_REMOVE: case BPK_ABIL_TRY: case BPK_ABIL_TRY_TAG: case BPK_ABIL_CANCEL: case BPK_ABIL_END:
    case BPK_ABIL_OWNER: case BPK_ABIL_ON_COOLDOWN: case BPK_ABIL_COOLDOWN: case BPK_ABIL_IS_ACTIVE: case BPK_GE_APPLY: case BPK_GE_REMOVE:
    case BPK_GE_HAS: case BPK_ATTR_GET: case BPK_ATTR_SET: return abil_native(in, kind, A, B, C, D); // (abilities and effects: ability.h)
    case BPK_SET_TENSION: case BPK_ADD_TENSION: case BPK_GET_TENSION: case BPK_TRIGGER_FILTER: case BPK_SET_FILTER: case BPK_CLEAR_FILTERS:
    case BPK_GET_FILTER: case BPK_SET_LETTERBOX: case BPK_DIP_BLACK: case BPK_DIP_WHITE: case BPK_SCARE_RIG: case BPK_SET_LIGHT_FLICKER:
    case BPK_SET_LIGHT_CHANNELS: case BPK_SET_RIM_TARGET: return terror_native(in, kind, A, B, C, D); // (Q13: the horror kit, terror.h)
    case BPK_SPLINE_LENGTH: case BPK_SPLINE_LOC_AT_DIST: case BPK_SPLINE_DIR_AT_DIST: case BPK_SPLINE_ROT_AT_DIST: case BPK_SPLINE_XFORM_AT_DIST:
    case BPK_SPLINE_LOC_AT_KEY: case BPK_SPLINE_DIST_AT_KEY: case BPK_SPLINE_CLOSEST_KEY: case BPK_SPLINE_CLOSEST_LOC: case BPK_SPLINE_NUM_POINTS:
    case BPK_SPLINE_POINT_LOC: case BPK_SPLINE_ADD_POINT: case BPK_SPLINE_SET_POINT: case BPK_SPLINE_REMOVE_POINT: case BPK_SPLINE_CLEAR:
    case BPK_SPLINE_SET_CLOSED: case BPK_SPLINE_SET_TYPE: case BPK_CABLE_ATTACH_END: case BPK_CABLE_ATTACH_START: case BPK_CABLE_DETACH:
    case BPK_CABLE_SET_LENGTH: case BPK_CABLE_GET_LENGTH: case BPK_CABLE_IMPULSE: return spline_native(in, kind, A, B, C, D); // (Q14: splines and cables)
    case BPK_PCG_GENERATE: case BPK_PCG_CLEANUP: case BPK_PCG_SET_SEED: case BPK_PCG_SET_PARAM: case BPK_PCG_IS_GENERATED:
        return pcg_native(in, kind, A, B, C, D); // (Q15: PCG, pcg.h)
    case BPK_BUILD_LIGHTING: case BPK_LIGHTING_PROGRESS: case BPK_CANCEL_LIGHTING: case BPK_SAVE_LIGHTING: case BPK_LOAD_LIGHTING:
    case BPK_SET_PROPERTY: case BPK_GET_PROPERTY: case BPK_SET_CVAR: case BPK_GET_CVAR: case BPK_EXEC_CONSOLE: case BPK_LIST_PROPERTIES:
        return ingame_native(in, kind, A, B, C, D); // (Q16: everything editable in the game)
    case BPK_MR_SET_PASSTHROUGH: case BPK_MR_IS_PASSTHROUGH: case BPK_MR_SET_OPACITY: case BPK_MR_SET_STYLE: case BPK_MR_SET_EDGE:
    case BPK_MR_SET_BACKGROUND: case BPK_MR_SET_BLEND: case BPK_MR_SET_WINDOW: case BPK_MR_SET_OCCLUSION: case BPK_MR_LOAD_ROOM:
    case BPK_MR_ROOM_SETUP: case BPK_MR_ROOM_PLANES: case BPK_MR_PLANE_INFO: case BPK_MR_TRACE:
        return mr_native(in, kind, A, B, C, D); // (Q25b: mixed reality, xr_mr.h)
    case BPK_LINE_TRACE: case BPK_SPHERE_TRACE: case BPK_BOX_TRACE: case BPK_CAPSULE_TRACE: case BPK_MULTI_LINE_TRACE:
    case BPK_MULTI_SPHERE_TRACE: case BPK_MULTI_BOX_TRACE: case BPK_MULTI_CAPSULE_TRACE: case BPK_LINE_TRACE_BATCH:
    case BPK_SPHERE_TRACE_BATCH: case BPK_DRAW_DEBUG_LINE: case BPK_DRAW_DEBUG_POINT: case BPK_DRAW_DEBUG_BOX:
    case BPK_DRAW_DEBUG_SPHERE: case BPK_DRAW_DEBUG_CAPSULE: case BPK_FLUSH_DEBUG_LINES: case BPK_SET_TRACE_COLORS:
    case BPK_ASYNC_LINE_TRACE_BATCH: case BPK_ASYNC_SPHERE_TRACE_BATCH: case BPK_GET_ASYNC_TRACE:
        return trace_native(in, kind, A, B, C, D); // (2026-11-19: the traces and the debug drawing)
    case BPK_GET_TIME_OF_DAY: res.v[0] = city_hour(); break; // (Q27.10)
    case BPK_SET_TIME_OF_DAY:
        if (!V.editor_cs) city_set_time(A->v[0], B->v[0]);
        return true;
    case BPK_REPORT_CRIME:
        if (!V.editor_cs) city_report_crime(A->v, B->v[0]);
        return true;
    case BPK_GET_POLICE_CALLS: res.v[0] = (float)city_police_calls(); break;
    case BPK_IS_OPEN_NOW: res.v[0] = city_is_open(ent_of(A) ? ent_of(A) : in->e) ? 1.0f : 0.0f; break;
    case BPK_GET_VISITORS: res.v[0] = (float)city_visitors(ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_SET_LOCO_STYLE: { // (Q27.11) Target, Style
        static const char* const names[ANIM_STYLE_COUNT] = { "Normal", "Injured", "Tired", "Drunk", "Carrying" };
        const char* sn = ENUM_NAME("ELocomotionStyle", B);
        int st = -1;
        for (int k = 0; k < ANIM_STYLE_COUNT; k++)
            if (sn && (!_stricmp(sn, names[k]) || (k == ANIM_STYLE_CARRY && !_stricmp(sn, "Carry")))) st = k;
        res.v[0] = st >= 0 && anim_set_locomotion_style(w, ent_of(A) ? ent_of(A) : in->e, st) ? 1.0f : 0.0f;
        break;
    }
    case BPK_GET_LOCO_STYLE: res.v[0] = (float)anim_locomotion_style(w, ent_of(A) ? ent_of(A) : in->e); break;
    case BPK_ADD_WARP_TARGET: // (2026-11-23) Target, Name, Location, Rotation
        if (!V.editor_cs) anim_warp_target_set(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v, N0->v[1]);
        return true;
    case BPK_REMOVE_WARP_TARGET:
        if (!V.editor_cs) anim_warp_target_remove(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B));
        return true;
    case BPK_PLAY_PAIRED_MONTAGE: // Primary, Partner, Montage, Play Rate
        res.v[0] = !V.editor_cs && anim_play_paired(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B), str_of(in, C), N0->v[0] > 0.0f ? N0->v[0] : 1.0f) ? 1.0f : 0.0f;
        break;
    case BPK_STOP_PAIRED_MONTAGE:
        if (!V.editor_cs) anim_stop_paired(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] > 0.0f ? B->v[0] : 0.25f);
        return true;
    case BPK_FIND_PAIRED_MONTAGE: { // (2026-11-23) Primary, Partner, Prefix, Max Distance, Max Angle
        const char* m = anim_find_paired(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B), str_of(in, C), N0->v[0], N1->v[0]);
        res.v[0] = m ? 1.0f : 0.0f;
        set_str(&D[1], m ? m : "");
        break;
    }
    case BPK_PRELOAD_LEVEL: // (2026-11-23, level_preload.h) speed 0: the global one
        if (!V.editor_cs) level_preload(str_of(in, A), B->v[0] > 0.0f ? B->v[0] : -1.0f);
        return true;
    case BPK_SET_LEVEL_LOAD_SPEED:
        if (!V.editor_cs) level_preload_set_speed(str_of(in, A), B->v[0] > 0.0f ? B->v[0] : -1.0f);
        return true;
    case BPK_SET_ASYNC_LOAD_SPEED:
        if (!V.editor_cs) level_async_set_speed(A->v[0] > 0.0f ? A->v[0] : 1.0f);
        return true;
    case BPK_GET_PRELOAD_PROGRESS: {
        float pr = 0.0f;
        PreloadState st = level_preload_state(str_of(in, A), &pr);
        res.v[0] = pr, D[1].v[0] = st == PRE_READY ? 1.0f : 0.0f, D[2].v[0] = st != PRE_NONE && st != PRE_FAILED ? 1.0f : 0.0f;
        D[3].v[0] = st == PRE_FAILED ? 1.0f : 0.0f; // (2026-11-24) Failed
        break;
    }
    case BPK_SET_CULTURE: { // (2026-11-24, loc.h) the game's texts in another culture, live; kept in the player's settings
        if (V.editor_cs) return true;
        const char* c = str_of(in, A);
        bool ok = loc_switch(c);
        if (B->v[0] != 0.0f) gus_set("Culture", 0.0f, c), gus_apply_live(), gus_save();
        res.v[0] = ok ? 1.0f : 0.0f;
        break;
    }
    case BPK_GET_CULTURE: set_str(&res, loc_culture()); break;
    case BPK_GET_CULTURES: {
        Arr* out = arr_get(D);
        if (out) {
            out->count = 0;
            const char* cs[LOC_MAX_CULTURES + 1];
            int nc = loc_cultures(cs, LOC_MAX_CULTURES + 1);
            for (int i = 0; i < nc; i++) {
                BpValue v;
                set_str(&v, cs[i]);
                arr_push(in, out, &v);
            }
        }
        return true;
    }
    case BPK_GET_STREAM_STATE: { // (2026-11-24) a Load Stream Level's Progress, Loaded, Failed by name
        int sr = 0;
        float pr = level_stream_state(str_of(in, A), &sr);
        res.v[0] = pr, D[1].v[0] = sr == 1 ? 1.0f : 0.0f, D[2].v[0] = sr == 2 ? 1.0f : 0.0f;
        break;
    }
    case BPK_CANCEL_PRELOAD:
        if (!V.editor_cs) level_preload_cancel(str_of(in, A));
        return true;
    case BPK_ENTER_COVER: res.v[0] = !V.editor_cs && anim_cover_enter(w, ent_of(A) ? ent_of(A) : in->e, B->v[0]) ? 1.0f : 0.0f; break; // (2026-11-23)
    case BPK_EXIT_COVER:
        if (!V.editor_cs) anim_cover_exit(w, ent_of(A) ? ent_of(A) : in->e);
        return true;
    case BPK_COVER_PEEK:
        if (!V.editor_cs) anim_cover_peek(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f);
        return true;
    case BPK_IS_IN_COVER: {
        bool lo, le, ri, on = anim_cover_state(w, ent_of(A) ? ent_of(A) : in->e, &lo, &le, &ri);
        res.v[0] = on ? 1.0f : 0.0f, D[1].v[0] = lo ? 1.0f : 0.0f, D[2].v[0] = le ? 1.0f : 0.0f, D[3].v[0] = ri ? 1.0f : 0.0f;
        break;
    }
    case BPK_PLAY_HIT_REACTION: // (2026-11-23) Target, Hit Location, Strength, Montage Prefix
        set_str(&res, V.editor_cs ? "None" : anim_hit_reaction(w, ent_of(A) ? ent_of(A) : in->e, B->v, C->v[0] > 0.0f ? C->v[0] : 1.0f, str_of(in, N0)));
        break;
    case BPK_MONTAGE_COMBO_INPUT: res.v[0] = !V.editor_cs && anim_montage_combo_input(w, ent_of(A) ? ent_of(A) : in->e) ? 1.0f : 0.0f; break;
    case BPK_IN_MONTAGE_WINDOW: res.v[0] = anim_montage_window_open(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B)) ? 1.0f : 0.0f; break;
    case BPK_CAN_PLAY_PAIRED: res.v[0] = anim_can_play_paired(w, ent_of(A) ? ent_of(A) : in->e, ent_of(B), str_of(in, C), N0->v[0], N1->v[0]) ? 1.0f : 0.0f; break;
    case BPK_TRY_TRAVERSAL: { // (2026-11-23) Target, Max Distance, Max Height, Draw Debug
        float th = 0.0f;
        int act = V.editor_cs ? 0 : anim_try_traversal(w, ent_of(A) ? ent_of(A) : in->e, B->v[0], C->v[0], N0->v[0] != 0.0f, &th);
        res.v[0] = act ? 1.0f : 0.0f;
        set_str(&D[1], anim_traversal_name(act));
        D[2].v[0] = th;
        break;
    }
    case BPK_SET_SMART_OBJECT: { // Target, Slots, Opens At, Closes At
        CInteractable* ia = ecs_get(w, ent_of(A) ? ent_of(A) : in->e, COMP_INTERACTABLE);
        if (ia && !V.editor_cs) {
            ia->so_slots = (uint8_t)fminf(fmaxf(B->v[0], 0.0f), 64.0f);
            ia->open_q = (uint8_t)(fminf(fmaxf(C->v[0], 0.0f), 24.0f) * 4.0f + 0.5f), ia->close_q = (uint8_t)(fminf(fmaxf(V.np[0].v[0], 0.0f), 24.0f) * 4.0f + 0.5f);
        }
        return true;
    }
    case BPK_ADD_STORM: // (Q27.6) Location, Radius, Intensity, Duration
        if (V.editor_cs) return true;
        res.v[0] = (float)wfield_add_storm(A->v[0], A->v[1], B->v[0], C->v[0], V.np[0].v[0]);
        break;
    case BPK_CLEAR_STORMS:
        if (!V.editor_cs) wfield_clear();
        return true;
    case BPK_GET_RAIN_AT: {
        WeatherSample s;
        wfield_sample(A->v[0], A->v[1], &s);
        res.v[0] = wfield_on() ? s.rain : water_rain_intensity();
        break;
    }
    case BPK_GET_WIND_AT: {
        WeatherSample s;
        wfield_sample(A->v[0], A->v[1], &s);
        res.v[0] = s.wind[0], res.v[1] = s.wind[1], res.v[2] = 0.0f;
        break;
    }
    case BPK_GET_CLOUDS_AT: {
        WeatherSample s;
        wfield_sample(A->v[0], A->v[1], &s);
        res.v[0] = s.clouds;
        break;
    }
    case BPK_CROWD_ALARM: // (Q27.1) Location, Radius
        if (V.editor_cs) return true;
        crowd_alarm(A->v, B->v[0]);
        break;
    default: return false;
    }
    *D = res;
    return true;
}

static bool overlapping(Entity a, Entity b); // (the overlaps, below)
// The editor's Construction Script (sugestÃƒÂ£o #64, Unreal's: it runs as the actor is placed, moved or changed) shapes its
// own actor only: its transform, its components' transforms, colors, lights, visibility. Nothing that reaches the rest of
// the level or the game (spawning, destroying, physics, AI, widgets, input, timers, levels, the sky, post process,
// dynamic materials) and no write to another actor: the editor keeps what is authored apart from what it shows.
static bool editor_cs_allows(World* w, uint32_t kind, const BpValue* target) {
    switch (kind) {
    case BPK_DESTROY: case BPK_ADD_IMPULSE: case BPK_RADIAL_IMPULSE: case BPK_SET_SIMULATE: case BPK_AI_MOVE_TO: case BPK_RUN_BT:
    case BPK_PHYS_ADD_FORCE: case BPK_PHYS_ADD_TORQUE: case BPK_PHYS_ANG_IMPULSE: case BPK_PHYS_IMPULSE_AT: case BPK_PHYS_SET_LIN_VEL:
    case BPK_PHYS_SET_ANG_VEL: case BPK_PHYS_SET_MASS: case BPK_PHYS_WAKE: case BPK_PHYS_SLEEP: case BPK_PHYS_SET_GRAVITY: case BPK_PHYS_SET_MATERIAL:
    case BPK_PHYS_SET_CCD: case BPK_CONSTRAINT_LIN_DRIVE: case BPK_CONSTRAINT_ANG_DRIVE: case BPK_CONSTRAINT_LIMITS: case BPK_CONSTRAINT_MOTION:
    case BPK_JUMP: case BPK_LAUNCH_CHARACTER: case BPK_PLAY_ANIMATION: case BPK_PLAY_MONTAGE: case BPK_STOP_MONTAGE: case BPK_REPORT_NOISE:
    case BPK_ADD_MAPPING_CONTEXT: case BPK_REMOVE_MAPPING_CONTEXT: case BPK_CREATE_WIDGET: case BPK_ADD_TO_VIEWPORT:
    case BPK_REMOVE_FROM_PARENT: case BPK_WIDGET_SET_COLOR: case BPK_WIDGET_SET_PERCENT: case BPK_WIDGET_SET_TEXT:
    case BPK_PRELOAD_LEVEL: case BPK_SET_LEVEL_LOAD_SPEED: case BPK_SET_ASYNC_LOAD_SPEED: case BPK_CANCEL_PRELOAD: // (2026-11-23)
    case BPK_WIDGET_SET_VISIBLE: case BPK_OPEN_LEVEL: case BPK_SET_TIMER: case BPK_CLEAR_TIMER: case BPK_SHOW_MOUSE:
    case BPK_FOG_COLOR: case BPK_FOG_DENSITY: case BPK_CLOUD_COVERAGE: case BPK_SUN_ANGLE: case BPK_PP_SET_FLOAT: case BPK_PP_SET_TINT:
    case BPK_PP_SET_WEIGHT: case BPK_MAT_SCALAR: case BPK_MAT_VECTOR: case BPK_BB_SET_ACTOR: case BPK_BB_SET_BOOL: case BPK_BB_SET_FLOAT:
    case BPK_BB_SET_VECTOR: case BPK_WIDGET_SET_VALUE: case BPK_WIDGET_SET_ENABLED: case BPK_PLAY_WIDGET_ANIM: case BPK_STOP_WIDGET_ANIM:
    case BPK_PLAY_SEQUENCE: case BPK_STOP_SEQUENCE:
    case BPK_PLAY_SOUND_2D: case BPK_PLAY_SOUND_AT: case BPK_SPAWN_SOUND_2D: case BPK_SPAWN_SOUND_AT: case BPK_SPAWN_SOUND_ATTACHED:
    case BPK_STOP_SOUND: case BPK_SET_SOUND_PAUSED: case BPK_SET_SOUND_VOLUME: case BPK_SET_SOUND_PITCH: case BPK_AUDIO_PLAY:
    case BPK_AUDIO_STOP: case BPK_AUDIO_FADE_IN: case BPK_AUDIO_FADE_OUT: case BPK_AUDIO_SET_PAUSED: case BPK_AUDIO_SET_VOLUME:
    case BPK_AUDIO_SET_PITCH: case BPK_AUDIO_SET_SOUND: case BPK_PUSH_SOUND_MIX: case BPK_POP_SOUND_MIX: case BPK_CLEAR_SOUND_MIXES:
    case BPK_SET_BASE_SOUND_MIX: case BPK_SET_CLASS_VOLUME:
    case BPK_NET_CREATE_SESSION: case BPK_NET_FIND_SESSIONS: case BPK_NET_JOIN_SESSION: case BPK_NET_JOIN_ADDRESS:
    case BPK_NET_DESTROY_SESSION: case BPK_NET_KICK: case BPK_NET_CHAT: case BPK_NET_SET_REPLICATES: case BPK_NET_RPC_SERVER:
    case BPK_NET_RPC_MULTICAST: case BPK_NET_RPC_OWNER: case BPK_NET_REPLICATE_VAR: case BPK_NET_SPAWN: case BPK_NET_DESTROY:
    case BPK_NET_CREATE_ADV: case BPK_NET_FIND_ADV: case BPK_NET_ADD_FILTER: case BPK_NET_CLEAR_FILTERS: case BPK_NET_SET_PROP: case BPK_NET_REMOVE_PROP: case BPK_NET_JOIN_ADV: case BPK_NET_JOIN_ADDR_ADV: case BPK_NET_UPDATE_SESSION: case BPK_NET_START_SESSION: case BPK_NET_END_SESSION: case BPK_NET_SET_JOINABLE: case BPK_NET_BAN: case BPK_NET_UNBAN: case BPK_NET_UNBAN_ALL: case BPK_NET_JOIN_FRIEND: case BPK_NET_INVITE_FRIEND: case BPK_NET_AUTO_INVITES: case BPK_NET_ACCEPT_INVITE:
    case BPK_NET_EMULATION: case BPK_STEAM_UNLOCK: case BPK_STEAM_CLEAR: case BPK_STEAM_PROGRESS: case BPK_STEAM_SET_STAT:
    case BPK_STEAM_STORE_STATS: case BPK_STEAM_UPLOAD_SCORE: case BPK_STEAM_DOWNLOAD_SCORES: case BPK_STEAM_OVERLAY:
    case BPK_STEAM_INVITE: case BPK_STEAM_PRESENCE: case BPK_SAVE_GAME: case BPK_LOAD_GAME: case BPK_DELETE_SAVE:
    case BPK_PS_SPAWN: case BPK_PS_ACTIVATE: case BPK_PS_DEACTIVATE: case BPK_PS_SET_COLOR: case BPK_PS_SET_SPAWN:
    case BPK_SPAWN_DECAL: case BPK_SET_RENDER_STYLE: case BPK_WP_SET_LAYER: case BPK_AUDIO_SET_PARAM:
    case BPK_PHYS_SET_SIMULATE: case BPK_PHYS_SET_BELOW: case BPK_PHYS_SET_BLEND: case BPK_PHYS_IMPULSE: case BPK_IK_SET_GOAL:
    case BPK_ACR_SET: case BPK_ACR_STRENGTH: case BPK_ACR_PROFILE: case BPK_ACR_HIT: case BPK_ACR_GET_UP: case BPK_ACR_AUTO_GET_UP: case BPK_ACR_WOUND:
    case BPK_PHYS_BLEND_BELOW: case BPK_DYN_SET: case BPK_DYN_WIND: case BPK_DYN_RESET: case BPK_ATTACH_SOCKET: case BPK_DETACH_SOCKET: // (Q10)
    case BPK_FP_SET_EXERTION: case BPK_FP_SET_FEAR: case BPK_FP_RECOIL: case BPK_FP_FOCUS_TARGET: case BPK_FP_PRESET: case BPK_SHAKE_START:
    case BPK_SHAKE_STOP: case BPK_SHAKE_STOP_ALL: case BPK_SHAKE_WORLD: case BPK_HIDE_BONE: // (Q12)
    case BPK_SET_TENSION: case BPK_ADD_TENSION: case BPK_TRIGGER_FILTER: case BPK_SET_FILTER: case BPK_CLEAR_FILTERS: case BPK_SET_LETTERBOX: // (Q13)
    case BPK_DIP_BLACK: case BPK_DIP_WHITE: case BPK_SCARE_RIG: case BPK_SET_LIGHT_FLICKER: case BPK_SET_LIGHT_CHANNELS: case BPK_SET_RIM_TARGET:
    case BPK_SPLINE_ADD_POINT: case BPK_SPLINE_SET_POINT: case BPK_SPLINE_REMOVE_POINT: case BPK_SPLINE_CLEAR: case BPK_SPLINE_SET_CLOSED: // (Q14)
    case BPK_SPLINE_SET_TYPE: case BPK_CABLE_ATTACH_END: case BPK_CABLE_ATTACH_START: case BPK_CABLE_DETACH: case BPK_CABLE_SET_LENGTH: case BPK_CABLE_IMPULSE:
    case BPK_PCG_GENERATE: case BPK_PCG_CLEANUP: case BPK_PCG_SET_SEED: case BPK_PCG_SET_PARAM: // (Q15)
    case BPK_BUILD_LIGHTING: case BPK_CANCEL_LIGHTING: case BPK_SAVE_LIGHTING: case BPK_LOAD_LIGHTING: case BPK_SET_CVAR: case BPK_EXEC_CONSOLE: // (Q16)
    case BPK_IK_CLEAR_GOAL:
    case BPK_VEH_THROTTLE: case BPK_VEH_STEER: case BPK_VEH_BRAKE: case BPK_VEH_HANDBRAKE: case BPK_VEH_FLIGHT: case BPK_VEH_GEAR:
    case BPK_VEH_ENGINE: case BPK_VEH_PART_SET: case BPK_VEH_PART_TOGGLE: case BPK_VEH_ENTER: case BPK_VEH_EXIT:
    case BPK_VR_TELEPORT: case BPK_VR_SET_LOCOMOTION: case BPK_VR_SET_VIGNETTE: case BPK_VR_GRAB: case BPK_VR_RELEASE: case BPK_VR_SET_REFRESH: case BPK_VR_SET_HAND_COLLISION: // (Q25)
    case BPK_MR_SET_PASSTHROUGH: case BPK_MR_SET_OPACITY: case BPK_MR_SET_STYLE: case BPK_MR_SET_EDGE: case BPK_MR_SET_BACKGROUND: // (Q25b)
    case BPK_MR_SET_BLEND: case BPK_MR_SET_WINDOW: case BPK_MR_SET_OCCLUSION: case BPK_MR_LOAD_ROOM: case BPK_MR_ROOM_SETUP:
    case BPK_CLOTH_TEAR: case BPK_RAIN_SET: case BPK_VR_HAPTIC: case BPK_RAIN_PROP_SET: case BPK_RAIN_PRESET: case BPK_RAIN_DROP_COLOR: case BPK_RAIN_SHADING: case BPK_RAIN_PUDDLE_COLOR: case BPK_SET_LIGHTING_LEVEL: case BPK_SET_LIGHTING_BLEND:
    case BPK_SET_ACTOR_WETNESS: case BPK_SOFT_IMPULSE: case BPK_SOFT_STIFFNESS: case BPK_SOFT_PINNED: case BPK_SOFT_PROP_SET:
    case BPK_TOON_SET_PARAM: case BPK_TOON_SET_COLOR: // (Q11: an actor's dynamic material instance)
    case BPK_SET_GAME_PAUSED: case BPK_SET_TICK_PAUSED: case BPK_SET_GUS: case BPK_APPLY_GUS: case BPK_LOAD_GUS: case BPK_DEFAULTS_GUS:
    case BPK_REMAP_KEY: case BPK_OPEN_PAUSE_MENU: case BPK_CLOSE_PAUSE_MENU: case BPK_MAIN_MENU: case BPK_QUIT_GAME:
    case BPK_CREATE_SAVE_OBJECT: case BPK_SET_PROGRESS_SAVE: case BPK_SAVE_PROGRESS: case BPK_LOAD_PROGRESS: case BPK_NEW_GAME: case BPK_AUTO_SAVE:
    case BPK_SET_AUTO_SAVE_INTERVAL: case BPK_RESPAWN_CHECKPOINT: case BPK_SET_CHECKPOINT: case BPK_OPEN_SAVE_MENU:
    case BPK_IA_INTERACT: case BPK_IA_SET_ENABLED: case BPK_IA_SET_PROMPT: case BPK_IA_DEVICE_SET:
    case BPK_DLG_START: case BPK_DLG_CHOOSE: case BPK_DLG_SKIP: case BPK_DLG_END: case BPK_DLG_SET_LANGUAGE:
    case BPK_QST_START: case BPK_QST_COMPLETE: case BPK_QST_FAIL: case BPK_QST_ADVANCE: case BPK_QST_SET_PROGRESS: case BPK_QST_TRACK:
    case BPK_GR_SET_PHASE: case BPK_GR_START: case BPK_GR_END: case BPK_GR_ADD_SCORE: case BPK_GR_SET_TIMER: case BPK_GR_ADD_LIVES:
    case BPK_GR_RESPAWN: case BPK_GR_RESTART:
    case BPK_ABIL_GIVE: case BPK_ABIL_REMOVE: case BPK_ABIL_TRY: case BPK_ABIL_TRY_TAG: case BPK_ABIL_CANCEL: case BPK_ABIL_END:
    case BPK_GE_APPLY: case BPK_GE_REMOVE: case BPK_ATTR_SET:
    case BPK_HUD_MESSAGE: case BPK_HUD_ADD_COMPASS: case BPK_HUD_REMOVE_COMPASS: case BPK_HUD_ADD_SCREEN: case BPK_HUD_REMOVE_SCREEN:
    case BPK_HUD_DAMAGE_NUMBER: case BPK_HUD_SET_VISIBLE: case BPK_HUD_BIND_BAR: case BPK_HUD_SET_SPREAD: case BPK_HUD_PHOTO_MODE:
        return false;
    case BPK_SET_LOCATION: case BPK_ADD_OFFSET: case BPK_SET_ROTATION: case BPK_ADD_ROTATION: case BPK_SET_SCALE: case BPK_SET_TRANSFORM:
    case BPK_SET_VISIBILITY: case BPK_SET_COLOR: case BPK_LIGHT_INTENSITY: case BPK_LIGHT_COLOR: {
        Entity e = ent_of(target);
        return ecs_alive(w, e) && (e == V.cs_actor || bp_component_owner(w, e) == V.cs_actor);
    }
    default:
        return true; // (math, getters, containers, strings: the script's own values)
    }
}

static void char_jump(CCharacter* c); // (below: move_character's)
static void native(Inst* in, const BpOp* o, BpValue* r, const BpValue* k) {
#define RD(x) ((x) & BP_K ? &k[(x) & 0x7FFFu] : &r[(x)])
    const BpValue* A = RD(o->a);
    const BpValue* B = RD(o->b);
    const BpValue* C = RD(o->c);
    BpValue* D = &r[o->d];
    World* w = V.w;
    uint32_t kind = o->op - OP_NATIVE;
    BpValue res = { { 0 } };
    bool out = true;
    if (V.editor_cs && !editor_cs_allows(w, kind, A)) return; // (the editor's Construction Script: its own actor only)
    if (kind == BPK_PLUGIN_PURE || kind == BPK_PLUGIN_CALL) { // (B14c) a plugin's function: the staged inputs, its outputs in a row
        static struct { const char* s; int fi; } cache[64]; // (by the name's address: the class's text table)
        const char* name = in->cls->strings[o->a];
        uint32_t h = (uint32_t)(((uintptr_t)name >> 4) & 63);
        const PluginFn* f = cache[h].s == name && cache[h].fi >= 0 ? plugin_fn_at((uint32_t)cache[h].fi) : NULL;
        // (2026-11-25, bug-fixer) the name checked too: another class's text at a freed one's address, or the plugins
        // changed (a project switch unloads its plugins), gave another function's index
        if (cache[h].s != name || (cache[h].fi >= 0 && (!f || strcmp(f->name, name)))) cache[h].s = name, cache[h].fi = plugin_fn_find(name);
        f = plugin_fn_at(cache[h].fi < 0 ? 0xFFFFFFFFu : (uint32_t)cache[h].fi);
        float res4[4][4];
        uint32_t nout = f ? f->nout : 0;
        plugin_fn_call(cache[h].fi, (const float(*)[4])V.np, o->b, res4, nout);
        for (uint32_t i = 0; i < nout; i++) memcpy(r[o->d + i].v, res4[i], sizeof res4[i]);
        return;
    }
    if (native_more(in, o, r, A, B, C, D)) return;
    switch (kind) {
    case BPK_ADD: res.v[0] = A->v[0] + B->v[0]; break;
    case BPK_SUB: res.v[0] = A->v[0] - B->v[0]; break;
    case BPK_MUL: res.v[0] = A->v[0] * B->v[0]; break;
    case BPK_DIV: res.v[0] = B->v[0] != 0.0f ? A->v[0] / B->v[0] : 0.0f; break;
    case BPK_VADD: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] + B->v[i]; break;
    case BPK_VSUB: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] - B->v[i]; break;
    case BPK_VMULS: for (int i = 0; i < 3; i++) res.v[i] = A->v[i] * B->v[0]; break;
    case BPK_DOT: res.v[0] = A->v[0] * B->v[0] + A->v[1] * B->v[1] + A->v[2] * B->v[2]; break;
    case BPK_VLEN: res.v[0] = sqrtf(A->v[0] * A->v[0] + A->v[1] * A->v[1] + A->v[2] * A->v[2]); break;
    case BPK_NORMALIZE: {
        float l = sqrtf(A->v[0] * A->v[0] + A->v[1] * A->v[1] + A->v[2] * A->v[2]);
        if (l > 1e-8f) for (int i = 0; i < 3; i++) res.v[i] = A->v[i] / l;
        break;
    }
    case BPK_MAKE_VECTOR:
    case BPK_MAKE_ROTATOR: res.v[0] = A->v[0], res.v[1] = B->v[0], res.v[2] = C->v[0]; break;
    case BPK_BREAK_VECTOR:
    case BPK_BREAK_ROTATOR: {
        float x = A->v[0], y = A->v[1], z = A->v[2];
        r[o->d] = (BpValue){ { x } }, r[o->d + 1] = (BpValue){ { y } }, r[o->d + 2] = (BpValue){ { z } };
        out = false;
        break;
    }
    case BPK_GREATER: res.v[0] = (float)(A->v[0] > B->v[0]); break;
    case BPK_LESS: res.v[0] = (float)(A->v[0] < B->v[0]); break;
    case BPK_EQUAL: res.v[0] = (float)(fabsf(A->v[0] - B->v[0]) < 1e-4f); break;
    case BPK_AND: res.v[0] = (float)(A->v[0] != 0.0f && B->v[0] != 0.0f); break;
    case BPK_OR: res.v[0] = (float)(A->v[0] != 0.0f || B->v[0] != 0.0f); break;
    case BPK_NOT: res.v[0] = (float)(A->v[0] == 0.0f); break;
    case BPK_RANDOM: res.v[0] = A->v[0] + rnd() * (B->v[0] - A->v[0]); break;
    case BPK_LERP: res.v[0] = A->v[0] + (B->v[0] - A->v[0]) * C->v[0]; break;
    case BPK_CLAMP: res.v[0] = fminf(fmaxf(A->v[0], B->v[0]), C->v[0]); break;
    case BPK_SIN: res.v[0] = sinf(A->v[0] * DEG2RAD); break;
    case BPK_COS: res.v[0] = cosf(A->v[0] * DEG2RAD); break;
    case BPK_SELECT: res.v[0] = A->v[0] != 0.0f ? B->v[0] : C->v[0]; break;
    case BPK_GET_PLAYER: set_ent(&res, V.player); break;
    case BPK_GET_OWNER:
    case BPK_TRY_GET_PAWN_OWNER: set_ent(&res, bp_component_owner(w, in->e)); break;
    case BPK_RADIAL_IMPULSE: physics_radial_impulse(w, A->v, B->v[0], C->v[0]); break;
    case BPK_REPORT_NOISE: ai_report_noise(w, A->v, B->v[0], in->e); break;
    case BPK_DELTA: res.v[0] = V.dt; break;
    case BPK_GAME_TIME: res.v[0] = (float)V.time; break;
    case BPK_INT_TO_FLOAT: res.v[0] = A->v[0]; break;
    case BPK_FLOAT_TO_INT: res.v[0] = truncf(A->v[0]); break;
    case BPK_AXIS: res.v[0] = V.axes[o->a & 3]; break;
    case BPK_KEY_DOWN: res.v[0] = (float)(V.keys[o->a & 511] != 0); break;
    case BPK_IS_VALID: res.v[0] = (float)(ent_of(A) != ENTITY_NONE && ecs_alive(w, ent_of(A))); break;
    case BPK_DOES_IMPLEMENT: res.v[0] = (float)(implements(class_of(ent_of(A)), in->cls->strings[o->b])); break;
    case BPK_GET_CONTROL_ROTATION: {
        Entity p = ecs_get(w, in->e, COMP_CHARACTER) ? in->e : V.player;
        const CCharacter* c = ecs_alive(w, p) ? ecs_get(w, p, COMP_CHARACTER) : NULL;
        if (c) res.v[0] = c->control_pitch, res.v[1] = c->control_yaw;
        break;
    }
    case BPK_CONTROL_FORWARD:
    case BPK_CONTROL_RIGHT: {
        Entity p = ecs_get(w, in->e, COMP_CHARACTER) ? in->e : V.player;
        const CCharacter* c = ecs_alive(w, p) ? ecs_get(w, p, COMP_CHARACTER) : NULL;
        float yaw = c ? c->control_yaw * DEG2RAD : 0.0f;
        if (kind == BPK_CONTROL_FORWARD) res.v[0] = cosf(yaw), res.v[1] = sinf(yaw);
        else res.v[0] = -sinf(yaw), res.v[1] = cosf(yaw);
        break;
    }
    default: { // the actions and getters on an actor (the first input), the level's fog, clouds and sun
        out = false;
        Entity e = ent_of(A);
        bool alive = e != ENTITY_NONE && ecs_alive(w, e);
        CTransform* t = alive ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        switch (kind) {
        case BPK_GET_LOCATION: if (t) memcpy(res.v, t->location, 12); out = true; break;
        case BPK_GET_ROTATION: if (t) memcpy(res.v, t->rotation, 12); out = true; break;
        case BPK_GET_SCALE: if (t) memcpy(res.v, t->scale, 12); out = true; break;
        case BPK_GET_FORWARD:
        case BPK_GET_RIGHT: {
            float f[3] = { 1, 0, 0 }, rt[3] = { 0, 1, 0 };
            if (t) axes_of(t, f, rt);
            memcpy(res.v, kind == BPK_GET_FORWARD ? f : rt, 12);
            out = true;
            break;
        }
        case BPK_DISTANCE: {
            Entity e2 = ent_of(B);
            const CTransform* t2 = ecs_alive(w, e2) ? ecs_get(w, e2, COMP_TRANSFORM) : NULL;
            if (t && t2) {
                float d[3] = { t->location[0] - t2->location[0], t->location[1] - t2->location[1], t->location[2] - t2->location[2] };
                res.v[0] = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            }
            out = true;
            break;
        }
        case BPK_SET_LOCATION: if (t) memcpy(t->location, B->v, 12), ecs_touch(w, e); break;
        case BPK_ADD_OFFSET: if (t) for (int i = 0; i < 3; i++) t->location[i] += B->v[i]; if (t) ecs_touch(w, e); break;
        case BPK_SET_ROTATION: if (t) memcpy(t->rotation, B->v, 12), ecs_touch(w, e); break;
        case BPK_ADD_ROTATION:
            if (t) {
                for (int i = 0; i < 3; i++) t->rotation[i] = fmodf(t->rotation[i] + B->v[i], 360.0f);
                ecs_touch(w, e);
            }
            break;
        case BPK_SET_SCALE: if (t) memcpy(t->scale, B->v, 12), ecs_touch(w, e); break;
        case BPK_ADD_IMPULSE: if (alive) physics_add_impulse(w, e, B->v, C->v[0] != 0.0f); break;
        case BPK_GET_ACTOR_VELOCITY: {
            const CCharacter* ch = alive ? ecs_get(w, e, COMP_CHARACTER) : NULL;
            const CRigidBody* rb = alive && !ch ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
            const CFloating* fl = alive && !ch && !rb ? ecs_get(w, e, COMP_FLOATING) : NULL;
            const float* v = ch ? ch->velocity : rb ? rb->velocity : fl ? fl->velocity : NULL;
            if (v) memcpy(res.v, v, 12);
            out = true;
            break;
        }
        case BPK_IS_OVERLAPPING: res.v[0] = overlapping(alive ? e : ENTITY_NONE, ent_of(B)) ? 1.0f : 0.0f; out = true; break;
        case BPK_IS_FALLING: {
            const CCharacter* ch = alive ? ecs_get(w, e, COMP_CHARACTER) : NULL;
            res.v[0] = ch && !ch->on_ground && !ch->anim_vertical ? 1.0f : 0.0f; // (a jump its animation carries: not falling)
            out = true;
            break;
        }
        case BPK_PLAY_MONTAGE: if (alive) anim_play_montage(w, e, name_at(in, o->c), B->v[0]); break;
        case BPK_BB_GET_FLOAT: case BPK_BB_GET_BOOL: case BPK_BB_GET_VECTOR: case BPK_BB_GET_ACTOR:
            if (alive) ai_bb_get(w, e, name_at(in, o->c), res.v);
            out = true;
            break;
        case BPK_BB_SET_FLOAT: case BPK_BB_SET_BOOL: case BPK_BB_SET_VECTOR: case BPK_BB_SET_ACTOR:
            if (alive) ai_bb_set(w, e, name_at(in, o->c), B->v);
            break;
        case BPK_AI_MOVE_TO: if (alive) ai_move_to(w, e, B->v, C->v[0]); break;
        case BPK_RUN_BT: if (alive) ai_run_tree(w, e, name_at(in, o->c)); break;
        case BPK_STOP_MONTAGE: if (alive) anim_stop_montage(w, e, B->v[0]); break;
        case BPK_PLAY_ANIMATION: if (alive) anim_play(w, e, name_at(in, o->c), B->v[0] != 0.0f); break;
        case BPK_SET_SIMULATE: if (alive) physics_set_simulate(w, e, B->v[0] != 0.0f); break;
        case BPK_GET_VELOCITY: {
            const CRigidBody* rb = alive ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
            if (rb) memcpy(res.v, rb->velocity, 12);
            out = true;
            break;
        }
        case BPK_SET_VISIBILITY:
            if (t) {
                t->flags = B->v[0] != 0.0f ? (t->flags | TRANSFORM_HIDDEN_GAME) : (t->flags & ~TRANSFORM_HIDDEN_GAME);
                ecs_touch(w, e);
            }
            break;
        case BPK_MAT_SCALAR:
        case BPK_MAT_VECTOR: { // the actor's dynamic material instance (made on first use): a value that folds (2026-11-24: the
            // affine fold, the Emission Intensity) writes its slot only; one the bake reads bakes it again before the frame
            CStaticMesh* m = alive ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
            Material* mat = m && m->material ? mat_find_hash(m->material) : NULL;
            if (!mat) break;
            uint32_t slot = mat_dynamic_for(e, mat);
            float v[4] = { B->v[0], kind == BPK_MAT_SCALAR ? B->v[0] : B->v[1], kind == BPK_MAT_SCALAR ? B->v[0] : B->v[2], 1.0f };
            mat_set_param(slot, name_at(in, o->c), v);
            break;
        }
        case BPK_SET_COLOR: {
            CStaticMesh* m = alive ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
            if (m) memcpy(m->base_color, B->v, 12), ecs_touch(w, e);
            break;
        }
        case BPK_PP_SET_FLOAT:
        case BPK_PP_SET_WEIGHT:
        case BPK_PP_SET_TINT: { // the volume (none: the level's first), read by the renderer at the camera every frame
            if (!alive || !ecs_get(w, e, COMP_POST_PROCESS)) e = first_with(COMP_POST_PROCESS);
            CPostProcess* p = e ? ecs_get(w, e, COMP_POST_PROCESS) : NULL;
            if (!p) break;
            if (kind == BPK_PP_SET_WEIGHT) p->blend_weight = fminf(fmaxf(B->v[0], 0.0f), 1.0f);
            else if (kind == BPK_PP_SET_TINT) memcpy(p->s.color_tint, B->v, 12), p->overrides |= 1u << PP_GAIN;
            else {
                int f = post_field_index(name_at(in, o->c));
                if (f >= 0) p->s.v[f] = B->v[0], p->overrides |= 1u << f;
            }
            break;
        }
        case BPK_LIGHT_INTENSITY:
        case BPK_LIGHT_COLOR: {
            CLight* l = alive ? ecs_get(w, e, COMP_LIGHT) : NULL;
            if (!l) break;
            if (kind == BPK_LIGHT_INTENSITY) l->intensity = fmaxf(B->v[0], 0.0f);
            else memcpy(l->color, B->v, 12);
            ecs_touch(w, e);
            break;
        }
        case BPK_DESTROY:
            if (alive) {
                if (e == V.player) V.player = ENTITY_NONE;
                uint32_t ti = inst_index_of(e); // its End Play first
                int32_t epc = ti != UINT32_MAX ? find_entry(insts()[ti].cls, BPK_EVENT_END_PLAY, NULL) : -1;
                if (epc >= 0) run(ti, (uint32_t)epc);
                ecs_destroy(w, e);
                V.boxes_sweep = true;
            }
            break;
        case BPK_FOG_DENSITY:
        case BPK_FOG_COLOR: { // the fog has no target: the level's (read by the renderer every frame)
            Entity f = first_with(COMP_FOG);
            CFog* g = f ? ecs_get(w, f, COMP_FOG) : NULL;
            if (!g) break;
            if (kind == BPK_FOG_DENSITY) g->density = fmaxf(A->v[0], 0.0f);
            else memcpy(g->albedo, A->v, 12);
            break;
        }
        case BPK_CLOUD_COVERAGE: {
            Entity f = first_with(COMP_CLOUD);
            CCloud* c = f ? ecs_get(w, f, COMP_CLOUD) : NULL;
            if (c) c->coverage = fminf(fmaxf(A->v[0], 0.0f), 1.0f);
            break;
        }
        case BPK_SUN_ANGLE: {
            Entity s = sun();
            CTransform* st = s ? ecs_get(w, s, COMP_TRANSFORM) : NULL;
            if (st) st->rotation[0] = A->v[0], st->rotation[1] = B->v[0], ecs_touch(w, s);
            break;
        }
        case BPK_LAUNCH_CHARACTER: { // on self (a Character): its velocity, added or replaced per axis; in the air at once
            CCharacter* c = ecs_get(w, in->e, COMP_CHARACTER);
            if (!c) break;
            for (int i = 0; i < 2; i++) c->velocity[i] = B->v[0] != 0.0f ? A->v[i] : c->velocity[i] + A->v[i];
            c->velocity[2] = C->v[0] != 0.0f ? A->v[2] : c->velocity[2] + A->v[2];
            if (A->v[2] > 0.0f) c->on_ground = 0;
            break;
        }
        case BPK_MOVE_INPUT:
        case BPK_JUMP:
        case BPK_YAW_INPUT:
        case BPK_PITCH_INPUT: { // on self (a Character)
            CCharacter* c = ecs_get(w, in->e, COMP_CHARACTER);
            CFloating* fl = c ? NULL : ecs_get(w, in->e, COMP_FLOATING);
            if (fl && kind == BPK_MOVE_INPUT) // a Pawn with Floating Pawn Movement
                for (int i = 0; i < 3; i++) fl->input[i] += A->v[i] * B->v[0];
            if (!c) break;
            if (kind == BPK_MOVE_INPUT)
                for (int i = 0; i < 3; i++) c->input[i] += A->v[i] * B->v[0];
            else if (kind == BPK_JUMP) {
                char_jump(c);
            } else if (kind == BPK_YAW_INPUT) c->control_yaw = fmodf(c->control_yaw + A->v[0], 360.0f);
            else c->control_pitch = fminf(fmaxf(c->control_pitch + A->v[0], -80.0f), 80.0f);
            break;
        }
        default: break;
        }
        break;
    }
    }
    if (out) *D = res;
#undef RD
}

// --- Event Dispatchers: the bindings (source actor, dispatcher, listening instance); a call runs each bound instance's
// Event of that dispatcher with the staged arguments (a copy: an event may stage its own calls) ---
#define MAX_BINDS 2048u
typedef struct BpBind { Entity src; uint32_t inst; char name[BP_NAME], event[BP_NAME]; } BpBind; // (event: a Custom Event bound by its delegate; "": the Event of the dispatcher)
static BpBind g_binds[MAX_BINDS];
static uint32_t g_nbinds;

// An actor made: its class's Construction Script (Unreal's), before its BeginPlay
static void construct(uint32_t ii) {
    if (ii >= V.ninst) return;
    int32_t pc = find_entry(insts()[ii].cls, BPK_FUNC_ENTRY, "ConstructionScript");
    if (pc >= 0) run(ii, (uint32_t)pc);
}

static void dispatch_bind(Entity src, const char* name, uint32_t inst, uint32_t mode, const char* event) {
    if (!event) event = "";
    for (uint32_t i = 0; i < g_nbinds;) {
        BpBind* b = &g_binds[i];
        bool same = b->src == src && !strcmp(b->name, name);
        bool same_ev = !event[0] || !strcmp(b->event, event); // (Unbind with a delegate: that event's binding only)
        if (same && mode == 0 && b->inst == inst && !strcmp(b->event, event)) return; // (bound already)
        if (same && (mode == 2 || (mode == 1 && b->inst == inst && same_ev))) *b = g_binds[--g_nbinds];
        else i++;
    }
    if (mode == 0 && src != ENTITY_NONE && g_nbinds < MAX_BINDS) {
        BpBind* b = &g_binds[g_nbinds++];
        b->src = src, b->inst = inst;
        snprintf(b->name, sizeof b->name, "%s", name);
        snprintf(b->event, sizeof b->event, "%s", event);
    }
}

uint32_t g_bp_remote_events; // (the tests': Custom Events run on another actor)
uint32_t g_bp_dispatch_calls, g_bp_dispatch_runs; // (the tests' diagnostics: Event Dispatcher calls, the bound events run,
float g_bp_dispatch_arg0;                         //  the last call's first argument)
static void dispatch_call(Entity src, const char* name) {
    BpValue args[64];
    memcpy(args, V.stage, sizeof args);
    g_bp_dispatch_calls++, g_bp_dispatch_arg0 = args[0].v[0];
    for (uint32_t i = 0; i < g_nbinds; i++) {
        BpBind b = g_binds[i];
        if (b.src != src || strcmp(b.name, name) || b.inst >= V.ninst || !insts()[b.inst].alive) continue;
        int32_t epc = b.event[0] ? find_entry(insts()[b.inst].cls, BPK_EVENT_CUSTOM, b.event) : find_entry(insts()[b.inst].cls, BPK_EVENT_DISPATCH, name);
        if (epc < 0) continue;
        memcpy(V.stage, args, sizeof args);
        run(b.inst, (uint32_t)epc);
        g_bp_dispatch_runs++;
    }
}

static void run(uint32_t ii, uint32_t pc) {
    if (V.depth >= 4) return; // an interface call inside an interface call ... (no deeper)
    if (g_dbg_block && !bpf_in_vm()) return; // (paused at a breakpoint: the world waits; bp_debug.h)
    V.depth++;
    V.fr[V.depth - 1].inst = ii, V.fr[V.depth - 1].op = NULL;
    Inst* in = &insts()[ii];
    const BpAsset* a = in->cls;
    const BpOp* code = a->code;
    BpValue* r = in->r;
    const BpValue* k = a->consts;
    uint32_t stack[BP_MAX_CALL], sp = 0, budget = BP_OP_BUDGET;
#define RD(x) ((x) & BP_K ? &k[(x) & 0x7FFFu] : &r[(x)])
    // the native code (bp_jit.c): it runs every op it compiled; the interpreter the one it stopped at, then back in
    if (a->jit_stale && V.depth == 1) { // (a breakpoint changed its code since: made again, no other run holding it)
        bp_jit_free((BpAsset*)a);
        ((BpAsset*)a)->jit_failed = ((BpAsset*)a)->jit_stale = false;
    }
    if (g_bp_jit && !a->jit && !a->jit_failed) bp_jit_build((BpAsset*)a);
    uint32_t (*jit)(BpValue*, const BpValue*, void*, uint32_t) = g_bp_jit && a->jit_ok ? a->jit_entry : NULL;
    while (pc < a->ncode) {
        if (jit && a->jit_ok) { // (not ok: a breakpoint changed the code while this run went on; interpreted)
            BpJitCtx jc = { (int32_t)budget, V.dt };
            uint32_t np = jit(r, k, &jc, pc);
            budget = jc.budget > 1 ? (uint32_t)jc.budget : 1u;
            if (np == BP_JIT_BUDGET) {
                message("%s: an infinite loop (over %u ops) was stopped", a->name, BP_OP_BUDGET);
                budget = 0;
                break;
            }
            pc = np;
            if (pc >= a->ncode) break;
        }
        if (!--budget) {
            message("%s: an infinite loop (over %u ops) was stopped", a->name, BP_OP_BUDGET);
            break;
        }
        const BpOp* o = &code[pc++];
        BpOp dbg_op;
    redo:
        switch (o->op) {
        case OP_END: goto done;
        case OP_MOVN: memmove(&r[o->a], RD(o->b), (size_t)o->c * sizeof(BpValue)); break;
        case OP_JMP: pc = o->a; break;
        case OP_JZ: if (RD(o->a)->v[0] == 0.0f) pc = o->b; break;
        case OP_JNZ: if (RD(o->a)->v[0] != 0.0f) pc = o->b; break;
        case OP_JGT: if (RD(o->a)->v[0] > RD(o->b)->v[0]) pc = o->c; break;
        case OP_JEQ: if (RD(o->a)->v[0] == RD(o->b)->v[0]) pc = o->c; break;
        case OP_PUSHRET:
            if (sp == BP_MAX_CALL) {
                message("%s: too many nested calls", a->name);
                goto done;
            }
            stack[sp++] = o->a;
            break;
        case OP_LATENT:
            if (!in->lat_pc[o->a]) { // already waiting: ignored (Unreal's Delay)
                in->lat_t[o->a] = RD(o->b)->v[0];
                in->lat_pc[o->a] = (uint32_t)o->c + 1u;
            }
            // fall through: the chain stops here
        case OP_RET:
            if (!sp) goto done;
            pc = stack[--sp];
            break;
        case OP_TIMELINE: { // (2026-11-16) Play 0, Play from Start 1, Stop 2, Reverse 3, Reverse from End 4, Set New Time 5
            struct TlState* st = &in->tls[o->a];
            const BpTimeline* tl = a->tlc[o->a].tl;
            float len = tl ? bp_timeline_length(tl) : 0.0f;
            switch (o->b) {
            case 0: st->playing = 1, st->dir = 1.0f; if (st->t >= len && !(tl && tl->loop)) st->t = len; break;
            case 1: st->playing = 1, st->dir = 1.0f, st->t = 0.0f; break;
            case 2: st->playing = 0; break;
            case 3: st->playing = 1, st->dir = -1.0f; break;
            case 4: st->playing = 1, st->dir = -1.0f, st->t = len; break;
            default: st->t = fminf(fmaxf(RD(o->c)->v[0], 0.0f), len); break;
            }
            if (!sp) goto done;
            pc = stack[--sp];
            break;
        }
        case OP_DELTA: r[o->a] = (BpValue){ { V.dt } }; break;
        case OP_INC: r[o->a].v[0] += 1.0f; break;
        case OP_PRINT: if (!V.editor_cs) message("%s", str_of(in, RD(o->a))); break; // (the input was made a string by the compiler)
        case OP_COMP: r[o->a] = (BpValue){ { RD(o->b)->v[o->c & 3] } }; break;
        case OP_SETCOMP: r[o->a].v[o->c & 3] = RD(o->b)->v[0]; break;
        case OP_TOSTR: {
            char buf[512];
            to_text(in, RD(o->b), (uint8_t)o->c, buf, sizeof buf);
            set_str(&r[o->a], buf);
            break;
        }
        case OP_CONV: conv(RD(o->b), &r[o->a], (uint8_t)(o->c & 255), (uint8_t)(o->c >> 8)); break;
        case OP_ACOPY: {
            Arr* d = arr_get(&r[o->a]);
            const Arr* s = arr_get(RD(o->b));
            if (d && d != s) arr_copy(d, s);
            else if (!d) r[o->a] = *RD(o->b);
            break;
        }
        case OP_NPARAM: V.np[o->a % BP_MAX_NPARAM] = *RD(o->b); break;
        case OP_ARG:
            if ((uint32_t)o->a + o->c <= 64) {
                memcpy(&V.stage[o->a], RD(o->b), (size_t)o->c * sizeof(BpValue));
                if (o->d && o->c == 1) glob_str(in, &V.stage[o->a]); // (a text leaves this class: made a play string)
            }
            break;
        case OP_GETARGS: if (o->b <= 64) memcpy(&r[o->a], V.stage, (size_t)o->b * sizeof(BpValue)); break;
        case OP_PUTRES: if (o->b <= 64) memcpy(V.stage, &r[o->a], (size_t)o->b * sizeof(BpValue)); break;
        case OP_TAKERES: if ((uint32_t)o->b + o->c <= 64) memcpy(&r[o->a], &V.stage[o->c], (size_t)o->b * sizeof(BpValue)); break;
        case OP_ALEN: { const Arr* q = arr_get(RD(o->b)); r[o->a] = (BpValue){ { q ? (float)q->count : 0.0f } }; break; }
        case OP_AGET: {
            const Arr* q = arr_get(RD(o->b));
            int i = (int)RD(o->c)->v[0];
            if (q && i >= 0 && (uint32_t)i < q->count) memcpy(&r[o->a], arr_at(q, (uint32_t)i), q->w * sizeof(BpValue));
            break;
        }
        case OP_JGE: if (RD(o->a)->v[0] >= RD(o->b)->v[0]) pc = o->c; break;
        case OP_LATENT_INFO: V.li.prog_reg = o->a, V.li.fail_pc = o->b, V.li.set = true; break; // (2026-11-24)
        case OP_LATENT2: {
            uint32_t slot = o->a;
            if (o->d == LAT_RETRIGGER) in->lat_t[slot] = RD(o->b)->v[0], in->lat_pc[slot] = (uint32_t)o->c + 1u;
            else if (!in->lat_pc[slot]) {
                in->lat_pc[slot] = (uint32_t)o->c + 1u;
                in->lat_t[slot] = 0.0f; // LAT_SPAWN: the next frame (the class is loaded with the project's Blueprints)
                if (o->d == LAT_LIGHTING && V.nstreams < 64) { // (Q16) Completed once the game's bake ended (bp_tick polls)
                    V.streams[V.nstreams++] = (BpStream){ (uint32_t)(in - insts()), BP_REQ_LIGHTING, (uint16_t)slot };
                    in->lat_t[slot] = 1e30f;
                }
                if (o->d == LAT_STREAM_LOAD || o->d == LAT_STREAM_UNLOAD) {
                    uint32_t req = V.hooks.stream ? V.hooks.stream(str_of(in, RD(o->b)), o->d == LAT_STREAM_LOAD, V.np[0].v[0] != 0.0f) : 0;
                    if (req && V.nstreams < 64) {
                        V.streams[V.nstreams++] = (BpStream){ (uint32_t)(in - insts()), req, (uint16_t)slot, V.li.fail_pc, V.li.prog_reg, V.li.set, "" };
                        in->lat_t[slot] = 1e30f; // until the level is in (bp_tick polls)
                    } else if (V.li.set && V.li.fail_pc) // (2026-11-24) no request: Failed next frame
                        in->lat_pc[slot] = (uint32_t)V.li.fail_pc + 1u;
                }
                if (o->d == LAT_PRELOAD && V.nstreams < 64) { // (2026-11-24) until its level is ready or failed (bp_tick polls)
                    BpStream* st = &V.streams[V.nstreams++];
                    *st = (BpStream){ (uint32_t)(in - insts()), BP_REQ_PRELOAD, (uint16_t)slot, V.li.fail_pc, V.li.prog_reg, true, "" };
                    snprintf(st->name, sizeof st->name, "%s", str_of(in, RD(o->b)));
                    in->lat_t[slot] = 1e30f;
                }
            }
            V.li.set = false;
            if (!sp) goto done;
            pc = stack[--sp];
            break;
        }
        case OP_CAST: {
            Entity e = ent_of(RD(o->a));
            const char* cls = a->strings[o->b];
            bool ok = e != ENTITY_NONE && ecs_alive(V.w, e);
            if (ok && strcmp(cls, "Actor")) {
                const BpAsset* c = class_of(e);
                bool is_char = ecs_get(V.w, e, COMP_CHARACTER) != NULL;
                ok = (c && bp_is_child_of(c, cls)) || (is_char && (!strcmp(cls, "Character") || !strcmp(cls, "Pawn")));
            }
            set_ent(&r[o->c], ok ? e : ENTITY_NONE);
            if (!ok) pc = o->d;
            break;
        }
        case OP_DISPATCH: V.fr[V.depth - 1].op = o; if (!V.editor_cs) dispatch_call(ent_of(RD(o->a)), a->strings[o->b & 127]); break;
        case OP_CALLEV: { // another actor's Custom Event (its class or a parent's), with the staged inputs
            if (V.editor_cs) break;
            V.fr[V.depth - 1].op = o;
            Inst* ti = inst_of(V.w, ent_of(RD(o->a)));
            int32_t epc = ti ? find_entry(ti->cls, BPK_EVENT_CUSTOM, a->strings[o->b & 127]) : -1;
            if (epc >= 0) run((uint32_t)(ti - insts()), (uint32_t)epc), g_bp_remote_events++;
            break;
        }
        case OP_DBIND: if (!V.editor_cs) dispatch_bind(ent_of(RD(o->a)), a->strings[o->b & 127], ii, o->c, o->d ? a->strings[(o->d - 1) & 127] : NULL); break;
        case OP_CALLIFACE: {
            V.fr[V.depth - 1].op = o;
            Entity e = ent_of(RD(o->a));
            const CBlueprint* b = ecs_alive(V.w, e) && (!V.editor_cs || e == V.cs_actor) ? ecs_get(V.w, e, COMP_BLUEPRINT) : NULL; // (the editor's Construction Script: its own only)
            if (b && b->inst && b->inst <= V.ninst) {
                uint32_t ti = b->inst - 1;
                int32_t epc = find_entry(insts()[ti].cls, BPK_EVENT_INTERFACE, a->strings[o->b & 127]);
                if (epc >= 0) run(ti, (uint32_t)epc);
                else memset(V.stage, 0, sizeof V.stage); // (no implementation: the results are zero)
            } else memset(V.stage, 0, sizeof V.stage);
            break;
        }
        case OP_SPAWNARG: // (Expose on Spawn) staged for the next OP_SPAWN
            if (V.nsp < 16) {
                V.sp[V.nsp].hash = u32_of(RD(o->b)), V.sp[V.nsp].str = o->d != 0;
                if (o->d) snprintf(V.sp[V.nsp].text, sizeof V.sp[V.nsp].text, "%s", str_of(in, RD(o->a)));
                else V.sp[V.nsp].v = *RD(o->a);
                V.nsp++;
            }
            break;
        case OP_SPAWN: { // a: the transform (location, rotation, scale), b: the class
            V.fr[V.depth - 1].op = o; // (its BeginPlay runs inside)
            const BpValue* t = RD(o->a);
            BpAsset* cls = u32_of(RD(o->b)) ? bp_find_hash(u32_of(RD(o->b))) : NULL;
            if (cls && cls->kind != BPA_CLASS) cls = NULL;
            float loc[3], rot[3];
            memcpy(loc, t[0].v, 12);
            memcpy(rot, t[1].v, 12);
            if (V.editor_cs) cls = NULL; // (the editor's Construction Script spawns nothing)
            V.sp_cls = cls; // (the Expose on Spawn values staged: inst_new gives them to this class's actor)
            Entity e = cls ? bp_spawn(V.w, cls, loc, rot) : ENTITY_NONE;
            V.nsp = 0, V.sp_cls = NULL;
            if (e != ENTITY_NONE && (t[2].v[0] != 1.0f || t[2].v[1] != 1.0f || t[2].v[2] != 1.0f) && (t[2].v[0] || t[2].v[1] || t[2].v[2])) {
                CTransform* tr = ecs_get(V.w, e, COMP_TRANSFORM);
                if (tr) for (int q = 0; q < 3; q++) tr->scale[q] *= t[2].v[q];
            }
            if (!cls && !V.editor_cs) message("%s: Spawn Actor without a class", a->name);
            set_ent(&r[o->c], e);
            break;
        }
        default:
            if (o->op >= OP_NATIVE) native(in, o, r, k);
            else if (o->op == OP_BREAK) { // the debugger: a breakpoint, a step or a trace here (it may pause); then the op it replaced
                dbg_op = dbg_hit(ii, pc - 1, stack, sp);
                o = &dbg_op;
                goto redo;
            }
            break;
        }
    }
done:
#undef RD
    V.ops += BP_OP_BUDGET - budget;
    V.depth--;
}

// The instance of an actor (its class compiled first); the index, or UINT32_MAX
static uint32_t make_instance(Entity e, BpAsset* cls) {
    if (!cls || V.ninst >= MAX_INST) return UINT32_MAX;
    if (!cls->compiled && !bp_compile(cls)) {
        message("%s didn't compile: %s", cls->name, cls->error);
        return UINT32_MAX;
    }
    if (!re_vm_ensure(&V.insts, (size_t)(V.ninst + 1) * sizeof(Inst))) return UINT32_MAX;
    BpValue* r = re_arena_alloc(&V.mem, (size_t)cls->nslots * sizeof(BpValue) + (size_t)cls->nlatent * 8u + (size_t)cls->ntlc * 12u + 8u);
    if (!r) return UINT32_MAX;
    uint32_t ii = V.ninst++;
    Inst* in = &insts()[ii];
    memset(in, 0, sizeof *in);
    in->e = e;
    in->cls = cls;
    in->r = r;
    if (cls->nslots) memcpy(r, cls->init, (size_t)cls->nslots * sizeof(BpValue));
    set_ent(&r[0], e);
    for (uint32_t s = 0; s < cls->narr; s++) { // its own containers (with the variables' defaults)
        const BpArrSlot* as = &cls->arr[s];
        uint8_t vt = as->def ? as->def->vtype : 0;
        uint32_t h = arr_new(as->cont, as->w, as->w2, as->type, vt);
        r[as->slot] = (BpValue){ { (float)h } };
        Arr* q = arr_get(&r[as->slot]);
        for (uint32_t d = 0; q && as->def && as->w == 1 && as->cont != BPC_MAP && d < as->def->nadef; d++) arr_push(NULL, q, &as->def->adef[d]);
    }
    in->lat_t = (float*)(r + cls->nslots);
    in->lat_pc = (uint32_t*)(in->lat_t + cls->nlatent);
    memset(in->lat_t, 0, (size_t)cls->nlatent * 8u);
    in->tls = (struct TlState*)(in->lat_pc + cls->nlatent);
    for (uint32_t k = 0; k < cls->ntlc; k++) { // AutoPlay: from its start as the actor begins
        in->tls[k] = (struct TlState){ 0.0f, 1.0f, 0, { 0 } };
        if (cls->tlc[k].tl && cls->tlc[k].tl->autoplay) in->tls[k].playing = 1;
    }
    in->tick = find_entry(cls, BPK_EVENT_TICK, NULL);
    in->alive = 1;
    CBlueprint* b = ecs_get(V.w, e, COMP_BLUEPRINT);
    if (b) b->inst = ii + 1;
    in->paused_tick = bp_native_of(cls) == BPN_USER_WIDGET || (b && (b->flags & BP_FLAG_TICK_WHEN_PAUSED)); // (Unreal: the widgets tick paused)
    // the actor's Instance Editable values over the class's defaults (before its Construction Script and BeginPlay)
    for (uint32_t k = 0; b && k < b->noverrides && k < BP_MAX_OVERRIDES; k++) {
        for (uint32_t vi = 0; vi < cls->nvars; vi++) {
            const BpVar* bv = &cls->vars[vi];
            if (!bv->editable || !bp_var_overridable(bv) || bp_name_hash(bv->name) != b->ov[k].var) continue;
            BpValue* reg = &r[cls->var_slot[vi]];
            if (bp_type_stringish(bv->type)) set_str(reg, b->ov[k].text);
            else memcpy(reg->v, b->ov[k].v, sizeof reg->v);
            V.stats.overrides++;
            break;
        }
    }
    // (2026-11-10) Expose on Spawn: the values the Spawn Actor node gave (over the defaults and the overrides; before
    // its Construction Script and BeginPlay, as Unreal's)
    if (V.nsp && cls == V.sp_cls) {
        for (uint32_t k = 0; k < V.nsp; k++)
            for (uint32_t vi = 0; vi < cls->nvars; vi++) {
                const BpVar* bv = &cls->vars[vi];
                if (!bv->expose_spawn || !bp_var_overridable(bv) || bp_name_hash(bv->name) != V.sp[k].hash) continue;
                BpValue* reg = &r[cls->var_slot[vi]];
                if (V.sp[k].str) set_str(reg, V.sp[k].text);
                else *reg = V.sp[k].v;
                break;
            }
        V.nsp = 0;
    }
    CCharacter* c = ecs_get(V.w, e, COMP_CHARACTER);
    if (c) {
        V.characters = true;
        if (c->auto_possess && V.player == ENTITY_NONE) V.player = e;
        const CTransform* t = ecs_get(V.w, e, COMP_TRANSFORM);
        if (t) c->control_yaw = t->rotation[1];
    }
    return ii;
}

Entity bp_spawn(World* w, BpAsset* cls, const float location[3], const float rotation[3]) {
    if (!cls || cls->kind != BPA_CLASS) return ENTITY_NONE;
    uint64_t mask = cls->comp_mask & COMP_ALL;
    mask |= COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_BLUEPRINT);
    Entity e = ecs_create(w, mask);
    if (e == ENTITY_NONE) return e;
    for (int c = 0; c < COMP_COUNT; c++) {
        if (!(mask & COMP_BIT(c)) || c == COMP_NAME || c == COMP_BLUEPRINT || g_components[c].size > sizeof cls->comp_data[0]) continue;
        void* dst = ecs_get(w, e, (ComponentId)c);
        if (dst && (cls->comp_mask & COMP_BIT(c))) memcpy(dst, cls->comp_data[c], g_components[c].size);
    }
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t->scale[0] == 0.0f && t->scale[1] == 0.0f && t->scale[2] == 0.0f) t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    memcpy(t->location, location, 12);
    memcpy(t->rotation, rotation, 12);
    CName* n = ecs_get(w, e, COMP_NAME);
    snprintf(n->text, sizeof n->text, "%s", cls->name);
    CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    b->asset_hash = cls->hash;
    b->inst = 0;
    ecs_touch(w, e);
    bp_components_build(w, e, cls);
    if (V.playing && V.w == w) { // spawned while playing: its boxes at the next tick (Characters bump into it), its BeginPlay now
        bp_box_spawned(e);
        uint32_t ii = make_instance(e, cls);
        construct(ii);
        int32_t pc = ii != UINT32_MAX ? find_entry(cls, BPK_EVENT_BEGIN_PLAY, NULL) : -1;
        if (pc >= 0) run(ii, (uint32_t)pc);
    }
    return e;
}

// --- Collision of the Characters: their capsules as boxes against the boxes of the meshes ---

static uint32_t cell_hash(int x, int y) { return ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & (GRID_BUCKETS - 1); }

// The world box of a local shape (center c, half extents h) under a transform (at least 1 cm each way: a plane)
static void box_of(const CTransform* t, const float c[3], const float h[3], Box* b) {
    float rows[3][4];
    render_transform_rows(t, rows);
    for (int k = 0; k < 3; k++) {
        float m = rows[k][3], e = 0.0f;
        for (int j = 0; j < 3; j++) m += rows[k][j] * c[j], e += fabsf(rows[k][j]) * h[j];
        e = fmaxf(e, 1.0f);
        b->mn[k] = m - e, b->mx[k] = m + e;
    }
}

// A box that may move later: remembered with its shape (an actor with physics, a Movable one, a component's collision)
static void track_box(uint32_t box, Entity e, const float c[3], const float h[3], const CTransform* t) {
    if (!re_vm_ensure(&V.track, (size_t)(V.ntrack + 1) * sizeof(BoxTrack))) return;
    BoxTrack* k = &((BoxTrack*)V.track.base)[V.ntrack++];
    k->box = box, k->e = e;
    memcpy(k->c, c, 12), memcpy(k->h, h, 12);
    memcpy(k->loc, t->location, 12), k->yaw = t->rotation[1];
}

// The box of a Box / Sphere / Capsule Collision that blocks (a component of ce's actor, at: its attachment) into the next
// slot (its room made by the caller); false: none
static bool collision_box(World* w, Entity ce, const CTransform* tr, const CCollision* co, const CAttach* at) {
    if (!co->block || (tr->flags & TRANSFORM_HIDDEN_GAME)) return false;
    if (at && ecs_get(w, at->owner, COMP_CHARACTER)) return false; // a Character's own shapes
    float e[3], zero[3] = { 0, 0, 0 };
    if (co->shape == COLLISION_BOX) memcpy(e, co->extent, 12);
    else if (co->shape == COLLISION_SPHERE) e[0] = e[1] = e[2] = co->extent[0];
    else e[0] = e[1] = co->extent[0], e[2] = fmaxf(co->extent[1], co->extent[0]);
    Box* b = &((Box*)V.boxes.base)[V.nboxes];
    memset(b, 0, sizeof *b);
    box_of(tr, zero, e, b);
    b->e = ce, b->owner = at ? at->owner : ENTITY_NONE;
    // (a component's shape follows its actor; a Movable actor's own: either may move)
    if (at || (tr->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE) track_box(V.nboxes, ce, zero, e, tr);
    V.nboxes++;
    return true;
}

// The box of a mesh (its bounds under its transform) into the next slot; bodies: it simulates (it may move); at: a
// prototyping shape's attachment (the actor it belongs to)
static bool mesh_box(Entity me, const CTransform* tr, const CStaticMesh* sm, bool bodies, const CAttach* at) {
    if (tr->flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_COLLISION)) return false;
    float mn[3], mx[3], c[3], h[3];
    render_mesh_bounds(sm->mesh, mn, mx);
    // a flat mesh (the Plane: its bounds 1 cm thick) stays 1 cm thick at any scale: a floor scaled 208 times made a box
    // 2 m thick around the player standing on it, pushed out sideways to the floor's edge on the first tick
    for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f, h[k] = (mx[k] - mn[k]) * 0.5f, h[k] = h[k] <= 0.5f ? 0.0f : h[k];
    Box* b = &((Box*)V.boxes.base)[V.nboxes];
    memset(b, 0, sizeof *b);
    box_of(tr, c, h, b); // (a plane: 1 cm thick)
    b->e = me, b->owner = at ? at->owner : ENTITY_NONE;
    if (bodies || (tr->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE) track_box(V.nboxes, me, c, h, tr);
    V.nboxes++;
    return true;
}

// The archetypes whose meshes block (not a Character's, a projectile's, a vehicle's own mesh: they'd hold themselves)
static bool mesh_archetype(const EcsArchetype* a) {
    const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
    return (a->mask & need) == need && !(a->mask & (COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_PROJECTILE) | COMP_BIT(COMP_VEHICLE)));
}

static void build_boxes(void) {
    double t0 = re_time_seconds();
    V.nboxes = V.nbig = V.ntrack = V.nxref = 0;
    V.boxes_built = true, V.nspawned = 0, V.boxes_sweep = false; // (everything alive is in: nothing waits)
    World* w = V.w;
    uint32_t total = 0;
    // the meshes of the level's actors (a Blueprint's mesh components don't block: a Character's would hold itself) and
    // the Box / Sphere / Capsule Collision components that block
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) == need && !(a->mask & COMP_BIT(COMP_CHARACTER))) total += a->count;
        if (a->mask & COMP_BIT(COMP_COLLISION)) total += a->count;
    }
    if (!total || !re_vm_ensure(&V.boxes, (size_t)total * sizeof(Box))) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & COMP_BIT(COMP_COLLISION)) && (a->mask & COMP_BIT(COMP_TRANSFORM))) {
            const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            const CCollision* co = ecs_arch_column(a, COMP_COLLISION);
            const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
            const Entity* ce = ecs_arch_entities(a);
            for (uint32_t i = 0; i < a->count; i++) collision_box(w, ce[i], &tr[i], &co[i], at ? &at[i] : NULL);
        }
        if (!mesh_archetype(a)) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const CAttach* pat = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL; // (a component: only a prototype's boxes)
        bool bodies = (a->mask & COMP_BIT(COMP_RIGID_BODY)) != 0;
        const Entity* me = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!pat || pat[i].kind == ATTACH_PROTO || (pat[i].kind == ATTACH_SPLINE && (pat[i].node & SPLINE_NODE_INST))) // (Q14: a Spline's instances)
                mesh_box(me[i], &tr[i], &sm[i], bodies, pat ? &pat[i] : NULL);
    }
    // the grid: counts, offsets, then the references (a box covering over 64 cells goes to the big list)
    if (!re_vm_ensure(&V.cells, (GRID_BUCKETS + 1) * sizeof(uint32_t)) || !re_vm_ensure(&V.big, (size_t)V.nboxes * 4u)) return;
    uint32_t* cells = (uint32_t*)V.cells.base;
    memset(cells, 0, (GRID_BUCKETS + 1) * sizeof(uint32_t));
    uint32_t* big = (uint32_t*)V.big.base;
    Box* bx = (Box*)V.boxes.base;
    uint32_t nref = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t* refs = (uint32_t*)V.refs.base;
        for (uint32_t i = 0; i < V.nboxes; i++) {
            int x0 = (int)floorf(bx[i].mn[0] / GRID_CELL), x1 = (int)floorf(bx[i].mx[0] / GRID_CELL);
            int y0 = (int)floorf(bx[i].mn[1] / GRID_CELL), y1 = (int)floorf(bx[i].mx[1] / GRID_CELL);
            if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
                if (pass == 0) big[V.nbig++] = i;
                continue;
            }
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++) {
                    uint32_t h = cell_hash(x, y);
                    if (pass == 0) cells[h + 1]++, nref++;
                    else refs[cells[h]++] = i;
                }
        }
        if (pass == 0) {
            for (uint32_t h = 0; h < GRID_BUCKETS; h++) cells[h + 1] += cells[h];
            if (!re_vm_ensure(&V.refs, (size_t)(nref ? nref : 1) * 4u)) return;
        }
    }
    // the second pass moved each start to its end: back by one bucket
    memmove(cells + 1, cells, GRID_BUCKETS * sizeof(uint32_t));
    cells[0] = 0;
    V.box_ms = (re_time_seconds() - t0) * 1000.0;
}

// A box added while playing (box i, already in V.boxes) into the spawned ones' grid: a ref per cell it covers, chained
// in front of the cell's (over 64 cells: the big list)
static void grid_add(uint32_t i) {
    const Box* b = &((const Box*)V.boxes.base)[i];
    int x0 = (int)floorf(b->mn[0] / GRID_CELL), x1 = (int)floorf(b->mx[0] / GRID_CELL);
    int y0 = (int)floorf(b->mn[1] / GRID_CELL), y1 = (int)floorf(b->mx[1] / GRID_CELL);
    int64_t n = (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1);
    if (n > 64) {
        if (re_vm_ensure(&V.big, (size_t)(V.nbig + 1) * 4u)) ((uint32_t*)V.big.base)[V.nbig++] = i;
        return;
    }
    if (!V.nxref) { // the first: the chains' heads cleared (once a play)
        if (!re_vm_ensure(&V.xhead, GRID_BUCKETS * sizeof(uint32_t))) return;
        memset(V.xhead.base, 0, GRID_BUCKETS * sizeof(uint32_t));
    }
    if (!re_vm_ensure(&V.xref, (size_t)(V.nxref + (uint32_t)n) * 8u)) return;
    uint32_t* head = (uint32_t*)V.xhead.base;
    uint32_t* xr = (uint32_t*)V.xref.base;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++) {
            uint32_t h = cell_hash(x, y), r = V.nxref++;
            xr[r * 2] = i, xr[r * 2 + 1] = head[h];
            head[h] = r + 1;
        }
}

static int ent_cmp(const void* x, const void* y) {
    Entity a = *(const Entity*)x, b = *(const Entity*)y;
    return a < b ? -1 : a > b;
}
static bool spawned_has(Entity e) {
    return e != ENTITY_NONE && bsearch(&e, V.spawned, V.nspawned, sizeof(Entity), ent_cmp) != NULL;
}

// The actors spawned since the last tick: their boxes (their meshes, their prototypes' and Collision components) added
// to the level's, into the spawned ones' grid; the level's grid stays (one pass over the archetypes that may block,
// a lookup of the owner per component)
static void add_spawned_boxes(void) {
    double t0 = re_time_seconds();
    World* w = V.w;
    qsort(V.spawned, V.nspawned, sizeof(Entity), ent_cmp);
    uint32_t first = V.nboxes;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        bool col = (a->mask & COMP_BIT(COMP_COLLISION)) && (a->mask & COMP_BIT(COMP_TRANSFORM)), mesh = mesh_archetype(a);
        if (!a->count || (!col && !mesh)) continue;
        const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            if (!spawned_has(at ? at[i].owner : ents[i])) continue;
            if (!re_vm_ensure(&V.boxes, (size_t)(V.nboxes + 2) * sizeof(Box))) return;
            if (col) collision_box(w, ents[i], &tr[i], &((const CCollision*)ecs_arch_column(a, COMP_COLLISION))[i], at ? &at[i] : NULL);
            if (mesh && (!at || at[i].kind == ATTACH_PROTO || (at[i].kind == ATTACH_SPLINE && (at[i].node & SPLINE_NODE_INST))))
                mesh_box(ents[i], &tr[i], &((const CStaticMesh*)ecs_arch_column(a, COMP_STATIC_MESH))[i], (a->mask & COMP_BIT(COMP_RIGID_BODY)) != 0,
                         at ? &at[i] : NULL);
        }
    }
    for (uint32_t i = first; i < V.nboxes; i++) grid_add(i);
    V.spawn_boxes += V.nboxes - first;
    V.nspawned = 0;
    V.box_ms = (re_time_seconds() - t0) * 1000.0;
}

// The destroyed actors' boxes emptied where they are (their grid refs stay, pointing at nothing): a pass of handle checks
static void sweep_boxes(void) {
    double t0 = re_time_seconds();
    Box* bx = (Box*)V.boxes.base;
    for (uint32_t i = 0; i < V.nboxes; i++) {
        Box* b = &bx[i];
        if (b->mn[0] > b->mx[0] || (ecs_alive(V.w, b->e) && (b->owner == ENTITY_NONE || ecs_alive(V.w, b->owner)))) continue;
        b->mn[0] = b->mn[1] = b->mn[2] = 1e30f, b->mx[0] = b->mx[1] = b->mx[2] = -1e30f;
        b->d[0] = b->d[1] = b->d[2] = 0.0f, b->dyaw = 0.0f;
    }
    V.boxes_sweep = false;
    V.box_ms = (re_time_seconds() - t0) * 1000.0;
}

// Every frame before the Characters move: the boxes that may move read again from their entities. One that moved leaves
// the grid (its cells were where it began) for the big list, scanned whole; d is how far its entity went, dyaw how much
// it turned (a platform carries and turns what stands on it). A gone entity's box becomes empty. Nothing to do: a
// compare per tracked box (most never move)
static void track_boxes(void) {
    Box* bx = (Box*)V.boxes.base;
    BoxTrack* tk = (BoxTrack*)V.track.base;
    for (uint32_t i = 0; i < V.ntrack; i++) {
        Box* b = &bx[tk[i].box];
        const CTransform* t = ecs_alive(V.w, tk[i].e) ? ecs_get(V.w, tk[i].e, COMP_TRANSFORM) : NULL;
        Box nb = *b;
        bool gone = !t || (t->flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_COLLISION)); // (Q25: no collision now, as a VR hand's held thing: out of the way)
        if (!gone) box_of(t, tk[i].c, tk[i].h, &nb);
        else nb.mn[0] = nb.mn[1] = nb.mn[2] = 1e30f, nb.mx[0] = nb.mx[1] = nb.mx[2] = -1e30f;
        float d = 0.0f, dl[3] = { 0, 0, 0 }, dy = 0.0f;
        for (int k = 0; k < 3; k++) d += fabsf(nb.mn[k] - b->mn[k]) + fabsf(nb.mx[k] - b->mx[k]);
        if (!gone) {
            for (int k = 0; k < 3; k++) dl[k] = t->location[k] - tk[i].loc[k];
            dy = remainderf(t->rotation[1] - tk[i].yaw, 360.0f);
            memcpy(tk[i].loc, t->location, 12), tk[i].yaw = t->rotation[1];
        }
        if (d < 1e-3f && fabsf(dy) < 1e-4f && fabsf(dl[0]) + fabsf(dl[1]) + fabsf(dl[2]) < 1e-3f) {
            b->d[0] = b->d[1] = b->d[2] = 0.0f, b->dyaw = 0.0f;
            continue;
        }
        bool was_empty = b->mn[0] > b->mx[0];
        for (int k = 0; k < 3; k++) b->d[k] = gone || was_empty ? 0.0f : dl[k];
        b->dyaw = gone || was_empty ? 0.0f : dy;
        if (!gone) memcpy(b->piv, t->location, 12);
        memcpy(b->mn, nb.mn, 12), memcpy(b->mx, nb.mx, 12);
        if (!b->moving && re_vm_ensure(&V.big, (size_t)(V.nbig + 1) * 4u)) b->moving = 1, ((uint32_t*)V.big.base)[V.nbig++] = tk[i].box;
        V.box_moves++;
    }
}

// A Character walking into a body with physics pushes it: the body's speed along the push goes toward the Character's,
// all of it for a body up to 80 kg, less for heavier ones (Unreal's CharacterMovement pushing physics objects)
static void push_body(const Box* b, int axis, float vchar) {
    CRigidBody* rb = ecs_alive(V.w, b->e) ? ecs_get(V.w, b->e, COMP_RIGID_BODY) : NULL;
    if (!rb || !rb->simulate) return;
    float m = rb->mass;
    if (m <= 0.0f) m = (b->mx[0] - b->mn[0]) * (b->mx[1] - b->mn[1]) * (b->mx[2] - b->mn[2]) * 1e-4f; // (100 kg a cubic meter)
    float share = fminf(1.0f, 80.0f / fmaxf(m, 1.0f));
    float want = vchar * share, have = rb->velocity[axis];
    if (want > 0.0f ? have >= want : have <= want) return;
    float imp[3] = { 0, 0, 0 };
    imp[axis] = want - have;
    physics_add_impulse(V.w, b->e, imp, true);
    V.pushes++;
}

static void collide_box(const Box* b, float p[3], const float ext[3], float v[3], int axis, float step, CCharacter* c) {
    for (int k = 0; k < 3; k++)
        if (p[k] + ext[k] <= b->mn[k] + 0.01f || p[k] - ext[k] >= b->mx[k] - 0.01f) return;
    if (axis < 2 && b->mx[2] - (p[2] - ext[2]) <= step) { // a step: up onto it
        p[2] = b->mx[2] + ext[2];
        return;
    }
    // its feet sunk into a box's top without moving this way (spawned a little into the floor, a box that grew): up onto
    // it, not out sideways (deeper than half its height it is a wall: as before; a teleport across a city goes on)
    float up = b->mx[2] - (p[2] - ext[2]);
    if (axis < 2 && v[axis] == 0.0f && up <= ext[2]) {
        p[2] += up;
        c->on_ground = 1, c->ground_box = (uint32_t)(b - (const Box*)V.boxes.base) + 1u;
        if (v[2] < 0.0f) v[2] = 0.0f;
        return;
    }
    if (v[axis] > 0.0f) p[axis] = b->mn[axis] - ext[axis];
    else p[axis] = b->mx[axis] + ext[axis];
    if (axis == 2 && v[2] <= 0.0f) c->on_ground = 1, c->ground_box = (uint32_t)(b - (const Box*)V.boxes.base) + 1u;
    if (axis < 2 && b->moving && v[axis] != 0.0f) push_body(b, axis, v[axis]);
    v[axis] = 0.0f;
}

static void collide(float p[3], const float ext[3], float v[3], int axis, float step, CCharacter* c) {
    float gz; // the landscapes' ground: under it, up onto it (its slopes walk up)
    if (landscape_height(p[0], p[1], &gz) && p[2] - ext[2] < gz) {
        p[2] = gz + ext[2];
        if (axis == 2 && v[2] <= 0.0f) c->on_ground = 1, c->ground_box = 0, v[2] = 0.0f;
    }
    const Box* bx = (const Box*)V.boxes.base;
    if (!V.nboxes) return;
    const uint32_t* cells = (const uint32_t*)V.cells.base;
    const uint32_t* refs = (const uint32_t*)V.refs.base;
    const uint32_t* big = (const uint32_t*)V.big.base;
    for (uint32_t i = 0; i < V.nbig; i++) collide_box(&bx[big[i]], p, ext, v, axis, step, c);
    int x0 = (int)floorf((p[0] - ext[0]) / GRID_CELL), x1 = (int)floorf((p[0] + ext[0]) / GRID_CELL);
    int y0 = (int)floorf((p[1] - ext[1]) / GRID_CELL), y1 = (int)floorf((p[1] + ext[1]) / GRID_CELL);
    const uint32_t* xh = V.nxref ? (const uint32_t*)V.xhead.base : NULL;
    const uint32_t* xr = (const uint32_t*)V.xref.base;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++) {
            uint32_t h = cell_hash(x, y);
            for (uint32_t r = cells[h]; r < cells[h + 1]; r++)
                if (!bx[refs[r]].moving) collide_box(&bx[refs[r]], p, ext, v, axis, step, c); // (a moved one: the big list's)
            for (uint32_t r = xh ? xh[h] : 0; r; r = xr[(r - 1) * 2 + 1]) // (the ones spawned while playing)
                if (!bx[xr[(r - 1) * 2]].moving) collide_box(&bx[xr[(r - 1) * 2]], p, ext, v, axis, step, c);
        }
}

// The slab test of a segment (a + d t) against a box grown by r: where it enters, below best (a start inside: none)
static float trace_box(const Box* q, const float a[3], const float d[3], float r, float best) {
    if (q->mn[0] > q->mx[0]) return best; // (emptied: a destroyed actor's; its inverted slabs met at 0)
    float t0 = 0.0f, t1 = best;
    bool inside = true;
    for (int k = 0; k < 3; k++) {
        float lo = q->mn[k] - r, hi = q->mx[k] + r;
        if (a[k] < lo || a[k] > hi) inside = false;
        if (fabsf(d[k]) < 1e-6f) {
            if (a[k] < lo || a[k] > hi) return best;
            continue;
        }
        float ta = (lo - a[k]) / d[k], tb = (hi - a[k]) / d[k];
        if (ta > tb) {
            float s = ta;
            ta = tb, tb = s;
        }
        t0 = fmaxf(t0, ta), t1 = fminf(t1, tb);
        if (t0 > t1) return best;
    }
    return inside ? best : fminf(t0, best);
}

// The level's collision boxes touching [mn, mx] (the ragdolls' contacts, ikrig.c): up to cap of them as mn xyz, mx xyz
uint32_t bp_world_boxes(const float mn[3], const float mx[3], float (*out)[6], uint32_t cap) {
    if (!V.playing || !V.nboxes) return 0;
    const Box* bx = (const Box*)V.boxes.base;
    const uint32_t* cells = (const uint32_t*)V.cells.base;
    const uint32_t* refs = (const uint32_t*)V.refs.base;
    const uint32_t* big = (const uint32_t*)V.big.base;
    uint32_t n = 0;
#define TAKE(i_)                                                                                                             do {                                                                                                                         const Box* b_ = &bx[i_];                                                                                                 if (b_->mx[0] < mn[0] || b_->mn[0] > mx[0] || b_->mx[1] < mn[1] || b_->mn[1] > mx[1] || b_->mx[2] < mn[2] ||                b_->mn[2] > mx[2])                                                                                                       break;                                                                                                               bool dup_ = false;                                                                                                       for (uint32_t k_ = 0; k_ < n && !dup_; k_++) dup_ = !memcmp(out[k_], b_, sizeof(float) * 6);                           if (!dup_ && n < cap) memcpy(out[n++], b_, sizeof(float) * 6);                                                       } while (0)
    for (uint32_t i = 0; i < V.nbig; i++) TAKE(big[i]);
    int x0 = (int)floorf(mn[0] / GRID_CELL), x1 = (int)floorf(mx[0] / GRID_CELL);
    int y0 = (int)floorf(mn[1] / GRID_CELL), y1 = (int)floorf(mx[1] / GRID_CELL);
    const uint32_t* xh = V.nxref ? (const uint32_t*)V.xhead.base : NULL;
    const uint32_t* xr = (const uint32_t*)V.xref.base;
    if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) <= 64)
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++) {
                uint32_t h = cell_hash(x, y);
                for (uint32_t r = cells[h]; r < cells[h + 1]; r++)
                    if (!bx[refs[r]].moving) TAKE(refs[r]);
                for (uint32_t r = xh ? xh[h] : 0; r; r = xr[(r - 1) * 2 + 1])
                    if (!bx[xr[(r - 1) * 2]].moving) TAKE(xr[(r - 1) * 2]);
            }
#undef TAKE
    return n;
}

// (2026-11-19) The scene queries' copy of the play world's boxes (scene_query.c): each one's bounds (mn xyz, mx xyz),
// its actor (the component's owner) and whether it moves. Built now when a query comes before any Character asked for
// them (a level of boxes and a turret tracing); the moving ones followed here when no Character does it. Returns the
// count (more than cap: call again with room; 0: not playing)
uint32_t bp_query_boxes(float (*b)[6], Entity* actor, uint8_t* moving, Entity* mesh_actor, uint32_t cap) {
    if (!V.playing || !V.w) return 0;
    if (!V.boxes_built) build_boxes();
    else if (!V.characters && V.ntrack) track_boxes();
    const Box* bx = (const Box*)V.boxes.base;
    for (uint32_t i = 0; i < V.nboxes && i < cap; i++) {
        memcpy(b[i], bx[i].mn, sizeof(float) * 6);
        actor[i] = bx[i].owner != ENTITY_NONE ? bx[i].owner : bx[i].e;
        moving[i] = bx[i].moving != 0;
        // (an actor's own mesh box, its transform the world's: Trace Complex reads its triangles; a component's: not)
        if (mesh_actor) mesh_actor[i] = bx[i].owner == ENTITY_NONE && ecs_get(V.w, bx[i].e, COMP_STATIC_MESH) ? bx[i].e : ENTITY_NONE;
    }
    return V.nboxes;
}
uint32_t bp_query_play_frame(void) { return V.playing ? V.play_frames : 0; } // (a new frame: the queries' world refreshed)

float bp_trace_boxes(const float a[3], const float b[3], float radius) {
    if (!V.playing) return 1.0f;
    float land = landscape_trace(a, b); // (the ground of the landscapes too)
    if (!V.nboxes) return land;
    const Box* bx = (const Box*)V.boxes.base;
    const uint32_t* cells = (const uint32_t*)V.cells.base;
    const uint32_t* refs = (const uint32_t*)V.refs.base;
    const uint32_t* big = (const uint32_t*)V.big.base;
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, best = 1.0f;
    for (uint32_t i = 0; i < V.nbig; i++) best = trace_box(&bx[big[i]], a, d, radius, best);
    int x0 = (int)floorf((fminf(a[0], b[0]) - radius) / GRID_CELL), x1 = (int)floorf((fmaxf(a[0], b[0]) + radius) / GRID_CELL);
    int y0 = (int)floorf((fminf(a[1], b[1]) - radius) / GRID_CELL), y1 = (int)floorf((fmaxf(a[1], b[1]) + radius) / GRID_CELL);
    const uint32_t* xh = V.nxref ? (const uint32_t*)V.xhead.base : NULL;
    const uint32_t* xr = (const uint32_t*)V.xref.base;
    if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) <= 64)
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++) {
                uint32_t h = cell_hash(x, y);
                for (uint32_t r = cells[h]; r < cells[h + 1]; r++)
                    if (!bx[refs[r]].moving) best = trace_box(&bx[refs[r]], a, d, radius, best);
                for (uint32_t r = xh ? xh[h] : 0; r; r = xr[(r - 1) * 2 + 1])
                    if (!bx[xr[(r - 1) * 2]].moving) best = trace_box(&bx[xr[(r - 1) * 2]], a, d, radius, best);
            }
    return fminf(best, land);
}

// Unreal's Projectile Movement: launched along the actor's forward, falls, bounces off the boxes (or stops on them)
static void move_projectile(Entity e, CTransform* t, CProjectile* p, float dt) {
    if (!p->active) return;
    if (!p->started) { // the local velocity (its direction, times Initial Speed when set) turned to the world
        float m[3][3], v[3] = { 0, 0, 0 };
        rotator_axes(t->rotation, m);
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) v[k] += m[a][k] * p->velocity[a];
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (p->initial_speed > 0.0f && l > 1e-6f)
            for (int k = 0; k < 3; k++) v[k] *= p->initial_speed / l;
        memcpy(p->velocity, v, 12);
        p->started = 1;
    }
    p->velocity[2] -= project()->gravity_z * p->gravity_scale * dt;
    float sp = sqrtf(p->velocity[0] * p->velocity[0] + p->velocity[1] * p->velocity[1] + p->velocity[2] * p->velocity[2]);
    if (p->max_speed > 0.0f && sp > p->max_speed)
        for (int k = 0; k < 3; k++) p->velocity[k] *= p->max_speed / sp;
    float ext[3] = { 2.0f, 2.0f, 2.0f };
    CCharacter dummy = { 0 };
    for (int k = 0; k < 3; k++) {
        float before = p->velocity[k];
        t->location[k] += p->velocity[k] * dt;
        collide(t->location, ext, p->velocity, k, 0.0f, &dummy);
        if (before == 0.0f || p->velocity[k] != 0.0f) continue;
        if (!p->should_bounce) { // Unreal's: it stops where it hit
            p->velocity[0] = p->velocity[1] = p->velocity[2] = 0.0f;
            p->active = 0;
            break;
        }
        p->velocity[k] = -before * p->bounciness;
        for (int j = 0; j < 3; j++)
            if (j != k) p->velocity[j] *= 1.0f - fminf(fmaxf(p->friction, 0.0f), 1.0f);
        if (k == 2 && fabsf(p->velocity[2]) < 20.0f) p->velocity[2] = 0.0f; // resting on the floor
        sp = sqrtf(p->velocity[0] * p->velocity[0] + p->velocity[1] * p->velocity[1] + p->velocity[2] * p->velocity[2]);
        if (sp < 5.0f) p->active = 0;
    }
    if (p->rotation_follows_velocity && sp > 1.0f) {
        t->rotation[1] = atan2f(p->velocity[1], p->velocity[0]) / DEG2RAD;
        t->rotation[0] = atan2f(p->velocity[2], sqrtf(p->velocity[0] * p->velocity[0] + p->velocity[1] * p->velocity[1])) / DEG2RAD;
    }
    if (t->location[2] < -100000.0f) p->active = 0;
    ecs_touch(V.w, e);
}

// Unreal's Floating Pawn Movement: flies toward the input, eases in and out, no gravity
static void move_floating(Entity e, CTransform* t, CFloating* f, float dt) {
    float in[3] = { f->input[0], f->input[1], f->input[2] };
    float l = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    if (l > 1.0f) in[0] /= l, in[1] /= l, in[2] /= l, l = 1.0f;
    for (int k = 0; k < 3; k++) {
        float dv = in[k] * f->max_speed - f->velocity[k];
        float rate = (l > 0.01f ? f->acceleration : f->deceleration) * dt, m = fabsf(dv);
        f->velocity[k] += m > rate ? dv / m * rate : dv;
    }
    float ext[3] = { 20.0f, 20.0f, 20.0f };
    CCharacter dummy = { 0 };
    bool moved = false;
    for (int k = 0; k < 3; k++)
        if (f->velocity[k] != 0.0f) {
            t->location[k] += f->velocity[k] * dt;
            collide(t->location, ext, f->velocity, k, 0.0f, &dummy);
            moved = true;
        }
    f->input[0] = f->input[1] = f->input[2] = 0.0f;
    if (moved) ecs_touch(V.w, e);
}

// Jump: on the ground it leaves it (a Motion Matching database with jumps: asks the animation for one, jump_wait s to
// come, else the physical one: move_character), swimming it rises (B12f)
static void char_jump(CCharacter* c) {
    if (c->on_ground && c->anim_jump_ok) {
        if (c->jump_wait <= 0.0f && !c->anim_vertical) c->jump_wait = 0.25f, c->jumps++;
    } else if (c->on_ground) c->velocity[2] = c->jump_z_velocity, c->on_ground = 0, c->jumps++;
    else if (c->swimming) c->velocity[2] = fmaxf(c->velocity[2], 0.5f * c->jump_z_velocity), c->jumps++;
}
void bp_character_jump(World* w, Entity e) {
    CCharacter* c = ecs_alive(w, e) ? ecs_get(w, e, COMP_CHARACTER) : NULL;
    if (c) char_jump(c);
}

static BpMoveHook g_move_hook; // (B13: below)
static uint32_t g_nskip;
static bool char_skipped(Entity e);
static void move_character(Entity e, CTransform* t, CCharacter* c, float dt) {
    { // (2026-11-23) a montage's root motion (anim_warp.inc: its clip's travel, a Motion Warping window's correction): it
      // moves the body this frame, with collision; the input waits (Unreal's root motion from montages)
        float rmd[3], rmy;
        bool rmv, rmend, rmnc;
        if (dt > 1e-5f && anim_root_motion_take(e, rmd, &rmy, &rmv, &rmend, &rmnc)) {
            CTransform before = *t;
            if (rmnc) { // (traversal: through the obstacle, its warp ends it on the floor; settled at its end)
                for (int k = 0; k < 3; k++) t->location[k] += rmd[k], c->velocity[k] = rmd[k] / dt;
                c->on_ground = 0;
                if (rmend) {
                    float ext[3] = { c->radius, c->radius, c->half_height }, probe[3] = { t->location[0], t->location[1], t->location[2] - 0.1f }, pv[3] = { 0, 0, -1.0f };
                    collide(probe, ext, pv, 2, 0.0f, c);
                    c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
                }
                t->rotation[1] = remainderf(t->rotation[1] + rmy, 360.0f);
                c->input[0] = c->input[1] = c->input[2] = 0.0f;
                ecs_touch(V.w, e);
                return;
            }
            c->velocity[0] = rmd[0] / dt, c->velocity[1] = rmd[1] / dt;
            if (rmv) c->velocity[2] = rmd[2] / dt; // (a vault, a climb: its height too, no gravity)
            else if (!c->on_ground || c->velocity[2] > 0.0f) c->velocity[2] -= project()->gravity_z * c->gravity_scale * dt;
            else c->velocity[2] = 0.0f;
            float ext[3] = { c->radius, c->radius, c->half_height };
            bool was = c->on_ground != 0;
            c->on_ground = 0;
            for (int k = 0; k < 3; k++) {
                t->location[k] += c->velocity[k] * dt;
                collide(t->location, ext, c->velocity, k, 45.0f, c);
            }
            if (!c->on_ground && (was || c->velocity[2] <= 0.0f) && !rmv) { // (resting: a probe 1 mm down)
                float probe[3] = { t->location[0], t->location[1], t->location[2] - 0.1f }, pv[3] = { 0, 0, -1.0f };
                collide(probe, ext, pv, 2, 0.0f, c);
                if (c->on_ground) c->velocity[2] = 0.0f;
            }
            t->rotation[1] = remainderf(t->rotation[1] + rmy, 360.0f);
            if (rmend) c->velocity[0] = c->velocity[1] = 0.0f; // (the clip's end: it stands where the clip stops)
            c->input[0] = c->input[1] = c->input[2] = 0.0f;
            if (memcmp(&before, t, sizeof before)) ecs_touch(V.w, e);
            return;
        }
    }
    bool in_cover = anim_cover_input(V.w, e, t, c, dt); // (2026-11-23) in cover: along the wall only, not past its ends
    float in[2] = { c->input[0], c->input[1] };
    float l = sqrtf(in[0] * in[0] + in[1] * in[1]);
    if (l > 1.0f) in[0] /= l, in[1] /= l;
    float accel = (c->on_ground ? 4096.0f : 1400.0f) * dt; // Unreal's: quick on the ground, some air control
    c->desired[0] = in[0] * c->max_walk_speed, c->desired[1] = in[1] * c->max_walk_speed;
    if (c->on_ground && c->anim_accel > 0.0f) { // Motion Matching's body (anim_mm_gen.inc): as quick as its starts and stops
        bool braking = l < 0.01f || in[0] * c->velocity[0] + in[1] * c->velocity[1] < 0.0f;
        accel = (braking ? fmaxf(c->anim_decel, c->anim_accel) : c->anim_accel) * dt;
    }
    for (int k = 0; k < 2; k++) {
        float dv = in[k] * c->max_walk_speed - c->velocity[k];
        float m = fabsf(dv);
        c->velocity[k] += m > accel ? dv / m * accel : dv;
    }
    CTransform before = *t;
    // standing on a box that moved this frame (a platform, a body with physics): carried by its move first, turned by its
    // yaw around its entity's place (Unreal's based movement: the body and the control rotation, so the camera, turn too)
    if (c->on_ground && c->ground_box && c->ground_box <= V.nboxes) {
        const Box* gb = &((const Box*)V.boxes.base)[c->ground_box - 1];
        if (gb->moving && gb->dyaw != 0.0f) {
            float s = sinf(gb->dyaw * DEG2RAD), co = cosf(gb->dyaw * DEG2RAD);
            float rx = t->location[0] - (gb->piv[0] - gb->d[0]), ry = t->location[1] - (gb->piv[1] - gb->d[1]);
            t->location[0] = gb->piv[0] + rx * co - ry * s;
            t->location[1] = gb->piv[1] + rx * s + ry * co;
            t->location[2] += gb->d[2];
            t->rotation[1] += gb->dyaw;
            c->control_yaw += gb->dyaw;
            V.platform_turns++;
        } else if (gb->moving)
            for (int k = 0; k < 3; k++) t->location[k] += gb->d[k];
    }
    c->ground_box = 0;
    // (B12f) Unreal's Swimming: water over its chest: no gravity, its head held above the surface (a spring to the level
    // minus half its height), slower, the current carrying it; wading (water over its knees) slower too
    float wl, wcur[2], wrho, wdep;
    float feet[3] = { t->location[0], t->location[1], t->location[2] - c->half_height };
    bool in_water = water_query(V.w, feet, &wl, wcur, &wrho, &wdep);
    float wd = in_water ? wl - feet[2] : 0.0f;
    c->swimming = wd > 1.3f * c->half_height ? 1u : 0u;
    if (c->swimming) {
        float sp = c->swim_speed > 0.0f ? c->swim_speed : 0.55f * c->max_walk_speed;
        float a2 = 900.0f * dt;
        for (int k = 0; k < 2; k++) {
            float dv = in[k] * sp - c->velocity[k], m = fabsf(dv);
            c->velocity[k] += m > a2 ? dv / m * a2 : dv;
        }
        float target = wl - 0.45f * c->half_height; // (its center: the head out of the water)
        c->velocity[2] += ((target - t->location[2]) * 8.0f - c->velocity[2] * 4.0f) * dt;
        for (int k = 0; k < 2; k++) t->location[k] += wcur[k] * dt; // (the current carries it)
        c->on_ground = 0;
    } else {
        if (in_water && wd > 0.5f * c->half_height) // wading: the water holds it back
            for (int k = 0; k < 2; k++) c->velocity[k] *= 1.0f - fminf(2.5f * dt, 0.5f);
        if (c->jump_wait > 0.0f && (c->jump_wait -= dt) <= 0.0f) { // a jump asked of the animation that none took: the physical one
            c->jump_wait = 0.0f;
            if (c->on_ground && !c->anim_vertical) c->velocity[2] = c->jump_z_velocity, c->on_ground = 0;
        }
        if (c->anim_vertical) { // a jump in a Motion Matching clip: its vertical root motion carries the capsule (it collides)
            c->velocity[2] = c->anim_vz;
            if (c->anim_vz > 0.0f) c->on_ground = 0;
        } else if (!c->on_ground || c->velocity[2] > 0.0f) // standing on the ground: no gravity (it would sink a hair a frame
            c->velocity[2] -= project()->gravity_z * c->gravity_scale * dt; // and snap back up: a body that never rests)
    }
    float ext[3] = { c->radius, c->radius, c->half_height };
    c->on_ground = 0;
    for (int k = 0; k < 3; k++) {
        t->location[k] += c->velocity[k] * dt;
        collide(t->location, ext, c->velocity, k, 45.0f, c);
    }
    if (!c->on_ground && c->velocity[2] <= 0.0f) { // resting: the snap leaves it touching, not inside; a probe 1 mm down
        float probe[3] = { t->location[0], t->location[1], t->location[2] - 0.1f }, pv[3] = { 0, 0, -1.0f };
        collide(probe, ext, pv, 2, 0.0f, c);
        if (c->on_ground) c->velocity[2] = 0.0f;
    }
    float sp = sqrtf(c->velocity[0] * c->velocity[0] + c->velocity[1] * c->velocity[1]);
    if (c->use_controller_yaw) { // (Q12) Use Controller Rotation Yaw: the body faces where the player looks
        float d = remainderf(c->control_yaw - t->rotation[1], 360.0f), turn = 720.0f * dt;
        t->rotation[1] += fabsf(d) < turn ? d : (d > 0 ? turn : -turn);
    } else if (c->anim_turning) // (a Motion Matching clip turns it: a turn in place, a pivot)
        t->rotation[1] += c->anim_yaw_rate * dt;
    else if (sp > 10.0f) { // Orient Rotation to Movement
        float target = atan2f(c->velocity[1], c->velocity[0]) / DEG2RAD;
        // the shortest way (remainder: -180..180 whatever the yaw grew to; fmodf of a negative stayed negative and
        // the body spun the long way round)
        float d = remainderf(target - t->rotation[1], 360.0f);
        float turn = 720.0f * dt;
        t->rotation[1] += fabsf(d) < turn ? d : (d > 0 ? turn : -turn);
    }
    t->rotation[1] = remainderf(t->rotation[1], 360.0f); // (kept in -180..180)
    if (t->location[2] < -100000.0f) { // fell out of the world: back up
        t->location[2] = 1000.0f;
        c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
    }
    c->input[0] = c->input[1] = c->input[2] = 0.0f;
    if (in_cover) anim_cover_after(V.w, e, t, dt); // (2026-11-23) glided in, held to the wall, its back to it
    // touched only when it moved: the renderer re-reads it, the dynamic TLAS is rebuilt, the GI sees a change
    if (memcmp(&before, t, sizeof before)) ecs_touch(V.w, e);
}

// --- (B13) the network's prediction ---
static Entity g_skip[32];
static bool char_skipped(Entity e) {
    for (uint32_t i = 0; i < g_nskip; i++)
        if (g_skip[i] == e) return true;
    return false;
}
void bp_set_move_hook(BpMoveHook hook) { g_move_hook = hook; }
void bp_character_skip(Entity e, bool skip) {
    for (uint32_t i = 0; i < g_nskip; i++)
        if (g_skip[i] == e) {
            if (!skip) g_skip[i] = g_skip[--g_nskip];
            return;
        }
    if (skip && g_nskip < 32) g_skip[g_nskip++] = e;
}
void bp_character_step(World* w, Entity e, const float input[2], bool jump, float dt) {
    CTransform* t = ecs_alive(w, e) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    CCharacter* c = t ? ecs_get(w, e, COMP_CHARACTER) : NULL;
    if (!c || dt <= 0.0f || dt > 0.25f) return;
    if (jump) { // (as the Jump node: on the ground it leaves it, swimming it rises)
        if (c->on_ground) c->velocity[2] = c->jump_z_velocity, c->on_ground = 0;
        else if (c->swimming) c->velocity[2] = fmaxf(c->velocity[2], 0.5f * c->jump_z_velocity);
    }
    c->input[0] = input[0], c->input[1] = input[1], c->input[2] = 0.0f;
    move_character(e, t, c, dt);
}

// --- Play ---

// The VM's memory, reserved once (virtual: pages committed as used); false when it couldn't be
static bool vm_memory(void) {
    if (!V.mem_ready) {
        if (!re_arena_init(&V.mem, (size_t)1 << 30) || !re_vm_reserve(&V.insts, (size_t)MAX_INST * sizeof(Inst)) ||
            !re_vm_reserve(&V.boxes, (size_t)RE_ECS_MAX_ENTITIES * sizeof(Box)) || !re_vm_reserve(&V.cells, (GRID_BUCKETS + 1) * 4u) ||
            !re_vm_reserve(&V.refs, (size_t)RE_ECS_MAX_ENTITIES * 64u * 4u) || !re_vm_reserve(&V.big, (size_t)RE_ECS_MAX_ENTITIES * 4u) ||
            !re_vm_reserve(&V.track, (size_t)RE_ECS_MAX_ENTITIES * sizeof(BoxTrack)) || !re_vm_reserve(&V.xhead, GRID_BUCKETS * 4u) ||
            !re_vm_reserve(&V.xref, (size_t)RE_ECS_MAX_ENTITIES * 16u * 8u))
            return false;
        V.mem_ready = true;
    }
    if (!V.sht.base && (!re_vm_reserve(&V.sdata, (size_t)64 << 20) || !re_vm_reserve(&V.soff, (size_t)SHT * 4u) ||
                        !re_vm_reserve(&V.sht, (size_t)SHT * 4u) || !re_vm_reserve(&V.arrs, (size_t)(1u << 20) * sizeof(Arr)) ||
                        !re_vm_reserve(&V.adata, (size_t)256 << 20) || !re_vm_ensure(&V.sht, (size_t)SHT * 4u))) {
        V.sht.base = NULL;
        return false;
    }
    return true;
}

// The editor's Construction Script (sugestÃƒÂ£o #64): the actor's class's ConstructionScript run on the editor's world
// with an instance of its own, then forgotten (no game runs: no BeginPlay, no tick, nothing kept). Only the actor and
// its components change (editor_cs_allows); the editor (editor_construct.c) keeps what was authored apart.
void bp_set_editor_hooks(const BpEditorHooks* h) { g_ed_hooks = h ? *h : (BpEditorHooks){ 0 }; }

static const char* g_read_var; // (bp_run_event_read: the variable read before the run's actor goes)
static BpValue g_read_out;
static bool g_read_ok;

bool bp_run_event_read(World* w, BpAsset* cls, const char* event, const char* var, BpValue* out) {
    g_read_var = var, g_read_ok = false;
    bool ok = bp_run_editor_event(w, cls, event);
    g_read_var = NULL;
    if (out) *out = g_read_out;
    return ok && g_read_ok;
}

bool bp_run_editor_event(World* w, BpAsset* cls, const char* event) {
    if (V.playing || !w || !cls || cls->kind != BPA_CLASS || !vm_memory()) return false;
    if (!cls->compiled && !bp_compile(cls)) return false;
    int32_t pc = find_entry(cls, BPK_EVENT_CUSTOM, event);
    if (pc < 0) return false;
    if (V.nstr || V.sused > 1) memset(V.sht.base, 0, (size_t)SHT * 4u);
    V.nstr = V.narr = 0, V.sused = 1, V.aused = 0;
    re_arena_reset(&V.mem);
    V.w = w;
    V.ninst = 0;
    V.player = ENTITY_NONE;
    V.time = 0.0, V.dt = 0.0f;
    V.characters = false;
    // its self: an actor made for the run (hidden; gone after it), so the nodes that read "self" have one
    Entity self = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    if (self) {
        CTransform* t = ecs_get(w, self, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE | TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME;
    }
    uint32_t ii = self ? make_instance(self, cls) : UINT32_MAX;
    if (ii != UINT32_MAX) run(ii, (uint32_t)pc);
    if (ii != UINT32_MAX && g_read_var) {
        int v = bp_var_index(cls, g_read_var);
        if (v >= 0) g_read_out = insts()[ii].r[cls->var_slot[v]], g_read_ok = true;
    }
    if (self) ecs_destroy(w, self);
    V.ninst = 0, V.w = NULL;
    re_arena_reset(&V.mem);
    return ii != UINT32_MAX;
}

bool bp_construct_in_editor(World* w, Entity actor) {
    if (V.playing || !w || !ecs_alive(w, actor)) return false;
    CBlueprint* b = ecs_get(w, actor, COMP_BLUEPRINT);
    BpAsset* cls = b ? bp_find_hash(b->asset_hash) : NULL;
    if (!cls || cls->kind != BPA_CLASS || !vm_memory()) return false;
    uint32_t keep_inst = b->inst;
    if (V.nstr || V.sused > 1) memset(V.sht.base, 0, (size_t)SHT * 4u); // (the strings' table: cleared only when used, 4 MB)
    V.nstr = V.narr = 0, V.sused = 1, V.aused = 0;
    re_arena_reset(&V.mem);
    V.w = w;
    V.ninst = 0;
    V.player = ENTITY_NONE;
    V.time = 0.0, V.dt = 0.0f;
    V.characters = false;
    V.editor_cs = true, V.cs_actor = actor;
    uint32_t ii = make_instance(actor, cls);
    if (ii != UINT32_MAX) construct(ii);
    if ((b = ecs_get(w, actor, COMP_BLUEPRINT))) b->inst = keep_inst; // (the editor's actor has no game instance)
    V.editor_cs = false, V.cs_actor = ENTITY_NONE;
    V.ninst = 0, V.player = ENTITY_NONE, V.w = NULL;
    re_arena_reset(&V.mem);
    return ii != UINT32_MAX;
}

// Whether a class's Construction Script does anything (more than its entry node)
bool bp_has_construction(const BpAsset* cls) {
    for (uint32_t i = 1; cls && i < cls->ngraphs; i++)
        if (cls->graphs[i].gkind == BPG_FUNCTION && !strcmp(cls->graphs[i].name, "ConstructionScript")) return cls->graphs[i].nnodes > 1;
    return false;
}

static void dlg_event_hook(World* w, int ev, Entity speaker, const char* s0, const char* s1, int i); // (below: dialogue.h)
static bool dlg_get_var(World* w, Entity e, const char* var, float* num, char* text, size_t cap);
static bool dlg_set_var(World* w, Entity e, const char* var, const char* value);
static bool dlg_camera_hook(World* w, float pos[3], float* yaw, float* pitch);
static void quest_event_hook(World* w, int ev, Entity owner, const char* quest, const char* obj, int index, int progress, int count); // (below: quest.h)
static void health_event(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b); // (below)
static void ia_event(World* w, Entity e, int ev, Entity a, float progress);
static bool ia_call(World* w, Entity e, const char* event, bool on);
static void health_destroy(World* w, Entity e);
static bool health_ragdoll(World* w, Entity e, bool on);
static void begin_play_body(World* w) { // (bp_begin_play, at the end: on the debugger's fiber while debugging)
    ui_begin_play(w); // (no widgets yet)
    pause_menu_end_play(), game_quit_clear(); // (a play starts unpaused, no pause menu, no Quit asked)
    terror_reset(); // (Q13: no tension, filter, letterbox or rig left over)
    if (!vm_memory()) return;
    memset(V.sht.base, 0, (size_t)SHT * 4u);
    V.nstr = V.narr = 0, V.sused = 1, V.aused = 0; // (string 0: "")
    V.ntimers = V.nstreams = 0;
    input_begin_play(); // Enhanced Input: no context until a Blueprint (or the project) adds one
    if (project()->default_imc[0]) input_add_context(project()->default_imc, 0);
    re_arena_reset(&V.mem);
    V.w = w;
    V.ninst = 0;
    V.player = ENTITY_NONE;
    V.time = 0.0;
    V.dt = 0.0f;
    V.characters = false;
    V.rng = 0x9E3779B97F4A7C15ull;
    memset(V.keys, 0, sizeof V.keys);
    memset(V.axes, 0, sizeof V.axes);
    V.playing = true;
    health_set_hooks(&(HealthHooks){ health_event, health_destroy, health_ragdoll }); // (the Health events reach the actors)
    health_begin_play(w);
    dlg_set_hooks(&(DlgHooks){ dlg_event_hook, bp_call_event_text, dlg_get_var, dlg_set_var, bp_player, dlg_camera_hook }); // (dialogue.h)
    dlg_begin_play(w);
    quest_set_hooks(&(QuestHooks){ quest_event_hook, bp_call_event_text, bp_player }); // (quest.h: before the Save Progress load)
    quest_begin_play(w);
    double t0 = re_time_seconds();
    inv_begin_play(w); // the inventories with their starting items, the Pickups walked through (before the boxes, any BeginPlay)
    interaction_set_hooks(&(InteractHooks){ ia_event, ia_call, bp_key_held }); // (the Interactables' events reach the actors)
    interaction_begin_play(w); // the devices' poses read, their moving parts Movable (before the boxes: they follow them)
    abil_begin_play(w); // the Ability Systems: attributes, starting abilities and effects (ability.h; before any BeginPlay, before a load)
    hud_begin_play(w); // (the HUD's markers, messages and bars empty: hud.h)
    // the instances first (BeginPlay may spawn and move archetypes around), then BeginPlay in the level's order
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_BLUEPRINT))) continue;
        const Entity* ents = ecs_arch_entities(a);
        CBlueprint* b = ecs_arch_column(a, COMP_BLUEPRINT);
        for (uint32_t i = 0; i < a->count; i++) {
            b[i].inst = 0;
            BpAsset* cls = bp_find_hash(b[i].asset_hash);
            if (!cls) {
                message("%s: its Blueprint Class is missing", actor_name(ents[i]));
                continue;
            }
            make_instance(ents[i], cls);
        }
    }
    // a placed Character (no Blueprint) with Auto Possess: the player takes it when no Blueprint pawn was taken
    for (uint32_t ai = 0; ai < w->archetype_count && V.player == ENTITY_NONE; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_CHARACTER)) || (a->mask & COMP_BIT(COMP_AI_AGENT))) continue;
        const CCharacter* cc = ecs_arch_column(a, COMP_CHARACTER);
        const Entity* ce = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && V.player == ENTITY_NONE; i++)
            if (cc[i].auto_possess) V.player = ce[i];
    }
    // the level's Characters (a Blueprint's or not) walk, the AIs see through the same boxes
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].count && (w->archetypes[ai].mask & (COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_AI_AGENT)))) V.characters = true;
    bool skeletal = false; // (a Skeletal Mesh: its Foot IK traces the same boxes)
    for (uint32_t ai = 0; ai < w->archetype_count && !skeletal; ai++)
        skeletal = w->archetypes[ai].count && (w->archetypes[ai].mask & (COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_VEHICLE))); // (and a vehicle: its wheels and chassis)
    V.boxes_built = false, V.nspawned = 0, V.nxref = V.spawn_boxes = V.platform_turns = 0, V.boxes_sweep = false;
    if (V.characters || skeletal) build_boxes();
    uint32_t n = V.ninst;
    for (uint32_t i = 0; i < n; i++) // every Construction Script before any BeginPlay (Unreal's order)
        if (ecs_alive(w, insts()[i].e)) construct(i);
    rules_begin_play(w); // (game_rules.h: no match yet; a load's match state, read next, waits for the first frame's)
    progress_begin_play(w); // (save_progress.h: the Saveables' keys, the Save Game object back, a load's level actors put back: before BeginPlay)
    for (uint32_t i = 0; i < n; i++) {
        int32_t pc = find_entry(insts()[i].cls, BPK_EVENT_BEGIN_PLAY, NULL);
        if (pc >= 0 && ecs_alive(w, insts()[i].e)) run(i, (uint32_t)pc);
    }
    // the player's Health with Show Health Bar: the example widget on the screen (health.h health_bar_asset)
    const CHealth* ph = V.player && ecs_alive(w, V.player) ? ecs_get(w, V.player, COMP_HEALTH) : NULL;
    if (ph && (ph->flags & HEALTH_SHOW_BAR)) {
        BpAsset* hb = bp_find_hash(health_bar_asset());
        Entity we = hb && (hb->compiled || bp_compile(hb)) ? ui_create_widget(w, hb) : ENTITY_NONE;
        if (we) ui_add_to_viewport(we, 0);
    }
    if (plugins_gameplay_enabled()) { // (the Gameplay Framework plugin: off, Play makes none of its pieces by itself)
        interaction_after_begin_play(w, V.player); // the player's Interactor (made when it has none) and WBP_InteractPrompt
        quest_after_begin_play(w); // the Quest Logs' starting quests, WBP_QuestTracker (quest.h)
        abil_after_begin_play(w, V.player); // WBP_AbilityBar for the player's Ability System with Show Ability Bar (ability.h)
        hud_after_begin_play(w, V.player); // the HUD Layout's widget (WBP_GameHUD) made and shown (hud.h)
    }
    V.stats.instances = V.ninst;
    V.stats.ms_last_frame = (re_time_seconds() - t0) * 1000.0;
}

uint32_t g_bp_input_runs; // (the tests' diagnostics: Enhanced Input events run since the start)
// --- Overlaps: Unreal's ActorBeginOverlap / ActorEndOverlap, and ActorOverlapping every frame while they touch ---
// The sensors: Box / Sphere / Capsule Collision with Generate Overlap Events (an actor's components, a Trigger's own).
// The bodies: what moves or acts (a Character's capsule, a rigid body, a projectile, a Blueprint's shapes and meshes, a
// Skeletal Mesh, an AI); the level's plain static meshes don't overlap (Unreal's default for them). The touching pairs
// of actors this frame against the last: begin / every frame / end, both actors told (Other Actor: the other one), and
// the Level Blueprint's events named after either. Nothing runs while the level has no sensor.
#define MAX_OV_PAIRS 4096u
#define MAX_OV_BODIES 65536u
typedef struct OvBody { Entity actor; float mn[3], mx[3]; } OvBody;
static uint64_t g_ov_prev[MAX_OV_PAIRS], g_ov_cur[MAX_OV_PAIRS];
static uint32_t g_ov_nprev, g_ov_ncur;
static OvBody g_ov_sensor[1024], g_ov_body[MAX_OV_BODIES];

static Entity actor_of(World* w, Entity e, const EcsArchetype* a, uint32_t i) {
    if (a->mask & COMP_BIT(COMP_ATTACH)) {
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        return at[i].owner && ecs_alive(w, at[i].owner) ? at[i].owner : e;
    }
    return e;
}

// An oriented box (half extents e) at the transform: its world bounds
static void ov_bounds(const CTransform* tr, const float c[3], const float e[3], float mn[3], float mx[3]) {
    float rows[3][4];
    render_transform_rows(tr, rows);
    for (int k = 0; k < 3; k++) {
        float cc = rows[k][3], ext = 0.0f;
        for (int j = 0; j < 3; j++) cc += rows[k][j] * c[j], ext += fabsf(rows[k][j]) * e[j];
        mn[k] = cc - ext, mx[k] = cc + ext;
    }
}

static int u64_cmp(const void* x, const void* y) {
    uint64_t a = *(const uint64_t*)x, b = *(const uint64_t*)y;
    return a < b ? -1 : a > b;
}

// An actor's own overlap events (no name) and the ones named after it (the Level Blueprint's), with Other Actor
static void fire_overlap(World* w, Entity self, Entity other, uint16_t kind) {
    const CName* nm = ecs_get(w, self, COMP_NAME);
    for (uint32_t i = 0; i < V.ninst; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++) {
            const BpEntry* en = &in->cls->entries_code[k];
            if (en->kind != kind) continue;
            bool mine = !en->name[0] && in->e == self, named = en->name[0] && nm && !strcmp(en->name, nm->text);
            if (!mine && !named) continue;
            set_ent(&V.stage[0], other);
            run(i, en->pc);
        }
    }
}

static bool overlapping(Entity a, Entity b) {
    if (a == ENTITY_NONE || b == ENTITY_NONE) return false;
    uint64_t key = a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a;
    return bsearch(&key, g_ov_cur, g_ov_ncur, sizeof key, u64_cmp) != NULL;
}

static void overlaps_update(World* w) {
    uint32_t ns = 0, nb = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // the sensors first: none, nothing to do
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_COLLISION)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CCollision* co = ecs_arch_column(a, COMP_COLLISION);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && ns < 1024; i++) {
            if (!co[i].generate_overlap_events) continue;
            float e[3], c[3] = { 0, 0, 0 };
            if (co[i].shape == COLLISION_BOX) memcpy(e, co[i].extent, 12);
            else if (co[i].shape == COLLISION_SPHERE) e[0] = e[1] = e[2] = co[i].extent[0];
            else e[0] = e[1] = co[i].extent[0], e[2] = fmaxf(co[i].extent[1], co[i].extent[0]);
            OvBody* s = &g_ov_sensor[ns++];
            s->actor = actor_of(w, ents[i], a, i);
            ov_bounds(&tr[i], c, e, s->mn, s->mx);
        }
    }
    g_ov_ncur = 0;
    if (ns) {
        const uint64_t bodies = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_PROJECTILE) | COMP_BIT(COMP_COLLISION) |
                                COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_AI_AGENT) | COMP_BIT(COMP_BLUEPRINT) | COMP_BIT(COMP_ATTACH);
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & bodies) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            const Entity* ents = ecs_arch_entities(a);
            const CCharacter* ch = (a->mask & COMP_BIT(COMP_CHARACTER)) ? ecs_arch_column(a, COMP_CHARACTER) : NULL;
            const CCollision* co = (a->mask & COMP_BIT(COMP_COLLISION)) ? ecs_arch_column(a, COMP_COLLISION) : NULL;
            const CStaticMesh* sm = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
            for (uint32_t i = 0; i < a->count && nb < MAX_OV_BODIES; i++) {
                if (tr[i].flags & TRANSFORM_HIDDEN_GAME && !co) continue;
                OvBody* b = &g_ov_body[nb];
                float c[3] = { 0, 0, 0 }, e[3];
                if (ch) { // the capsule (its center at the actor)
                    for (int k = 0; k < 3; k++) b->mn[k] = tr[i].location[k] - ch[i].radius, b->mx[k] = tr[i].location[k] + ch[i].radius;
                    b->mn[2] = tr[i].location[2] - ch[i].half_height, b->mx[2] = tr[i].location[2] + ch[i].half_height;
                } else if (co) {
                    if (co[i].shape == COLLISION_BOX) memcpy(e, co[i].extent, 12);
                    else if (co[i].shape == COLLISION_SPHERE) e[0] = e[1] = e[2] = co[i].extent[0];
                    else e[0] = e[1] = co[i].extent[0], e[2] = fmaxf(co[i].extent[1], co[i].extent[0]);
                    ov_bounds(&tr[i], c, e, b->mn, b->mx);
                } else if (sm) {
                    float mn[3], mx[3];
                    render_mesh_bounds(sm[i].mesh, mn, mx);
                    for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f, e[k] = fmaxf((mx[k] - mn[k]) * 0.5f, 0.5f);
                    ov_bounds(&tr[i], c, e, b->mn, b->mx);
                } else if (a->mask & (COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_AI_AGENT) | COMP_BIT(COMP_PROJECTILE) | COMP_BIT(COMP_RIGID_BODY))) {
                    float half[3] = { 30.0f, 30.0f, 90.0f }; // (no shape: a person's size)
                    ov_bounds(&tr[i], c, half, b->mn, b->mx);
                } else continue; // (an actor root without a shape: its components stand for it)
                b->actor = actor_of(w, ents[i], a, i);
                nb++;
            }
        }
        for (uint32_t s = 0; s < ns; s++)
            for (uint32_t i = 0; i < nb && g_ov_ncur < MAX_OV_PAIRS; i++) {
                const OvBody *A = &g_ov_sensor[s], *B = &g_ov_body[i];
                if (A->actor == B->actor || A->mn[0] > B->mx[0] || A->mx[0] < B->mn[0] || A->mn[1] > B->mx[1] || A->mx[1] < B->mn[1] ||
                    A->mn[2] > B->mx[2] || A->mx[2] < B->mn[2])
                    continue;
                Entity x = A->actor, y = B->actor;
                g_ov_cur[g_ov_ncur++] = x < y ? (uint64_t)x << 32 | y : (uint64_t)y << 32 | x;
            }
        qsort(g_ov_cur, g_ov_ncur, sizeof g_ov_cur[0], u64_cmp);
        uint32_t u = 0; // (each pair once)
        for (uint32_t i = 0; i < g_ov_ncur; i++)
            if (!u || g_ov_cur[i] != g_ov_cur[u - 1]) g_ov_cur[u++] = g_ov_cur[i];
        g_ov_ncur = u;
    }
    // begin / every frame / end: a merge of the two sorted lists
    uint32_t i = 0, j = 0;
    while (i < g_ov_ncur || j < g_ov_nprev) {
        uint64_t c = i < g_ov_ncur ? g_ov_cur[i] : UINT64_MAX, p = j < g_ov_nprev ? g_ov_prev[j] : UINT64_MAX;
        uint16_t kind = c == p ? BPK_EVENT_OVERLAPPING : c < p ? BPK_EVENT_BEGIN_OVERLAP : BPK_EVENT_END_OVERLAP;
        uint64_t key = c <= p ? c : p;
        if (c <= p) i++;
        if (p <= c) j++;
        Entity x = (Entity)(key >> 32), y = (Entity)(key & 0xFFFFFFFFu);
        bool xa = ecs_alive(w, x), ya = ecs_alive(w, y);
        if (xa) fire_overlap(w, x, ya ? y : ENTITY_NONE, kind);
        if (ya) fire_overlap(w, y, xa ? x : ENTITY_NONE, kind);
    }
    memcpy(g_ov_prev, g_ov_cur, sizeof(uint64_t) * g_ov_ncur);
    g_ov_nprev = g_ov_ncur;
}

double bp_game_time(void) { return V.playing ? V.time : 0.0; }

// (2026-11-25, bug-fixer: the blueprints test's Timeline read Value 2 with Pings and Done 0) a class compiled again while
// its instances play: they keep the old code's registers. Told (the cause to find)
static void warn_recompile_playing(const BpAsset* a) {
    if (!V.playing) return;
    uint32_t live = 0;
    for (uint32_t i = 0; i < V.ninst; i++) live += insts()[i].alive && insts()[i].cls == a;
    if (live) printf("[RealEngine] aviso: Blueprint %s compiled again while %u of its instances play (game time %.2f s)\n", a->name, live, V.time);
}

static void tick_body(World* w, float dt) { // (bp_tick, at the end: on the debugger's fiber while debugging)
    if (!V.playing || V.w != w) return;
    double t0 = re_time_seconds();
    V.ops = 0;
    V.dt = dt;
    V.time += dt;
    V.play_frames++;
    uint32_t n = V.ninst, live = 0;
    // Enhanced Input: the actions this frame, their events into the instances that listen (before the Ticks)
    if (V.boxes_dirty || (V.characters && !V.boxes_built)) build_boxes(), V.boxes_dirty = false; // (a Character spawned into a level without boxes)
    else {
        if (V.boxes_sweep) sweep_boxes(); // the destroyed actors' boxes out, the spawned ones' in: the level's grid stays
        if (V.nspawned) add_spawned_boxes();
    }
    input_set_devices(V.keys, 512, &V.axes[2], 0.0f);
    input_update(dt);
    // the game paused (Set Game Paused, the pause menu): only the widgets and the actors with Tick When Paused tick, count
    // their Delays and timers and hear the input; no movement, no Level Sequence, no overlaps (game_settings.h)
    const bool paused = game_paused();
    for (uint32_t i = 0; i < n; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e) || (paused && !in->paused_tick)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++) {
            const BpEntry* en = &in->cls->entries_code[k];
            if ((en->kind & 0xFFF) != BPK_EVENT_INPUT_ACTION) continue;
            uint32_t ev;
            if (input_action_state(en->name, NULL, &ev, NULL, NULL) && (ev >> ((en->kind >> 12) - 1) & 1u)) run(i, en->pc), g_bp_input_runs++;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        Inst* in = &insts()[i];
        if (!in->alive) continue;
        if (!ecs_alive(w, in->e)) { // destroyed
            in->alive = 0;
            continue;
        }
        live++;
        if (paused && !in->paused_tick) continue;
        double p0 = V.profile ? re_time_seconds() : 0.0;
        uint32_t ops0 = V.ops;
        for (uint32_t s = 0; s < V.nstreams;) { // a streamed level that is in (or out): its node goes on
            BpStream* st = &V.streams[s];
            if (st->inst != i) {
                s++;
                continue;
            }
            // (2026-11-24) res: 0 going on, 1 done / loaded, 2 failed; the Progress pin follows it each frame
            int res = 0;
            float pr = 0.0f;
            if (st->req == BP_REQ_LIGHTING) res = rtl_busy() ? 0 : 1;
            else if (st->req == BP_REQ_PRELOAD) {
                PreloadState ps = level_preload_state(st->name, &pr);
                res = ps == PRE_READY ? 1 : (ps == PRE_FAILED || ps == PRE_NONE) ? 2 : 0;
            } else if (st->info) pr = level_stream_progress(st->req, &res);
            else res = !V.hooks.stream_done || V.hooks.stream_done(st->req) ? 1 : 0;
            if (st->info && st->prog_reg != 0xFFFF) in->r[st->prog_reg].v[0] = res == 1 ? 1.0f : pr;
            if (!res) {
                s++;
                continue;
            }
            if (res == 2 && st->info && st->fail_pc) in->lat_pc[st->slot] = (uint32_t)st->fail_pc + 1u;
            in->lat_t[st->slot] = 0.0f;
            *st = V.streams[--V.nstreams];
        }
        for (uint32_t l = 0; l < in->cls->nlatent; l++) // the Delays that are due
            if (in->lat_pc[l] && (in->lat_t[l] -= dt) <= 0.0f) {
                uint32_t pc = in->lat_pc[l] - 1u;
                in->lat_pc[l] = 0;
                run(i, pc);
            }
        for (uint32_t k = 0; k < in->cls->ntlc; k++) tl_tick(i, k, dt); // (2026-11-16) its Timelines playing
        if (in->tick >= 0) run(i, (uint32_t)in->tick);
        if (V.profile) { // the Debug Viewer: its Tick and Delays this frame (the events of input and overlaps add up elsewhere)
            in->prof_ms = (float)((re_time_seconds() - p0) * 1000.0);
            in->prof_ops = V.ops - ops0;
        }
    }
    for (uint32_t t = 0; t < V.ntimers;) { // Set Timer by Event
        BpTimer* tm = &V.timers[t];
        if (tm->inst >= V.ninst || !insts()[tm->inst].alive) {
            V.timers[t] = V.timers[--V.ntimers];
            continue;
        }
        if (paused && !insts()[tm->inst].paused_tick) { // (its actor waits with the world)
            t++;
            continue;
        }
        if ((tm->t -= dt) > 0.0f) {
            t++;
            continue;
        }
        uint32_t ii = tm->inst, pc = tm->pc;
        if (tm->loop) tm->t += fmaxf(tm->period, 0.001f), t++;
        else V.timers[t] = V.timers[--V.ntimers];
        run(ii, pc);
    }
    health_update(w, dt); // the Health components: regeneration, the deaths' destroys (health.h)
    progress_update(w, dt); // a load's player and spawned actors, the play time, auto save, the Checkpoints, a save's picture (save_progress.h)
    dlg_tick(w, dt); // the conversation: its letters, its timed lines, WBP_Dialogue (dialogue.h; also while paused)
    rules_tick(w, dt); // the match: its phases, clock (waits while paused), respawns, win and lose (game_rules.h)
    hud_tick(w, dt, V.player); // the HUD: its markers where their actors are, the feed's queue, the crosshair, the hiding (hud.h; also while paused)
    double c0 = re_time_seconds();
    if (paused) { // (the world waits: the Characters, projectiles, sequences, components and overlaps where they are)
        V.prof_char_ms = 0.0;
        V.stats.instances = live;
        V.stats.ops_last_frame = V.ops;
        V.stats.ms_last_frame = (re_time_seconds() - t0) * 1000.0;
        return;
    }
    if (V.characters) track_boxes(), water_gather(w); // (the platforms and the bodies where they are now; the water: B12f)
    if (V.characters)
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_TRANSFORM);
            if ((a->mask & need) != need) continue;
            CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
            CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = 0; i < a->count; i++) {
                if (g_nskip && char_skipped(ents[i])) continue; // (B13: a remote player's pawn on the server: its moves move it)
                bool hook = g_move_hook && ents[i] == V.player;
                float in2[2] = { ch[i].input[0], ch[i].input[1] };
                bool jumped = ch[i].jumps != ch[i].jumps_seen;
                ch[i].jumps_seen = ch[i].jumps;
                move_character(ents[i], &tr[i], &ch[i], dt);
                if (hook) g_move_hook(w, ents[i], in2, jumped, dt); // (the prediction's record: the move and where it led)
            }
        }
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // the movement components on actors
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & (COMP_BIT(COMP_PROJECTILE) | COMP_BIT(COMP_FLOATING))) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        if (a->mask & COMP_BIT(COMP_PROJECTILE)) {
            CProjectile* pj = ecs_arch_column(a, COMP_PROJECTILE);
            for (uint32_t i = 0; i < a->count; i++) move_projectile(ents[i], &tr[i], &pj[i], dt);
        }
        if ((a->mask & COMP_BIT(COMP_FLOATING)) && !(a->mask & COMP_BIT(COMP_CHARACTER))) {
            CFloating* fl = ecs_arch_column(a, COMP_FLOATING);
            for (uint32_t i = 0; i < a->count; i++) move_floating(ents[i], &tr[i], &fl[i], dt);
        }
    }
    seq_tick(w, dt); // the Level Sequences playing (after the movement: a cinematic's keys win; the components follow them)
    terror_tick(w, dt, true); // (Q13) the tension, the filters' fades, the flickering lights' sounds, the Rim Lights (terror.c)
    demo_play_tick(w, dt); // a demo's game mode (demo_play.c: the Fighting, Horror, RPG templates)
    inv_tick(w, dt); // the Pickups touched, the inventories' events (inventory.c; the equipment hangs as components, below)
    interaction_tick(w, dt, V.player); // the doors and levers moving, the Interactors' focus, keys and holds (interaction.c)
    quest_tick(w, dt); // the quests: Go To, Collect, time limits, the tracker, the markers (quest.c)
    abil_tick(w, dt, V.player); // the effects' time and periods, casts, cooldowns, the keys 1-6, the events (ability.c)
    fpcam_tick(w, dt); // (Q12) the Camera Shakes' time (the player's First Person Camera moves with the components, below)
    bp_components_update(w, dt, true); // the components follow (the camera on its boom after the Character moved)
    overlaps_update(w);
    V.prof_char_ms = (re_time_seconds() - c0) * 1000.0;
    V.stats.instances = live;
    V.stats.ops_last_frame = V.ops;
    V.stats.ms_last_frame = (re_time_seconds() - t0) * 1000.0;
}

void bp_end_play(World* w) {
    g_nskip = 0; // (B13: no pawn left to the network)
    bp_debug_end_play(); // (a pause at a breakpoint abandoned first: the End Play events run)
    seq_end_play(); // (the Level Sequences stop with the play)
    terror_reset(); // (Q13: the editor's view without the play's horror)
    demo_play_end();
    dlg_end_play(w); // (the conversation over without its events: the instances are going)
    quest_end_play(w); // (the logs stay for the next level; progress_session_end forgets them)
    rules_end_play(w); // (the match over without its events)
    g_ov_nprev = g_ov_ncur = 0; // (the next play starts with no overlaps)
    g_nbinds = 0; // (nor bindings)
    ui_end_play();
    inv_end_play();
    interaction_end_play(w);
    abil_end_play(); // (the Ability Systems' state goes with the play)
    fpcam_begin_play(); // (Q12: the First Person Cameras' states and the shakes go with the play)
    hud_end_play();
    pause_menu_end_play(); // (the pause menu gone, the game unpaused)
    for (uint32_t i = 0; V.playing && i < V.ninst; i++) { // End Play of every live instance
        Inst* in = &insts()[i];
        int32_t pc = in->alive && w && ecs_alive(w, in->e) ? find_entry(in->cls, BPK_EVENT_END_PLAY, NULL) : -1;
        if (pc >= 0) run(i, (uint32_t)pc);
    }
    mat_end_play(); // the actors' dynamic material instances
    progress_end_play(w); // (the Progress Save Game's variables kept for the next level)
    health_end_play();
    V.playing = false;
    sq_reset(), dd_clear_all(); // (2026-11-19) the traces' world and their lines go with the play
    V.ninst = 0;
    V.player = ENTITY_NONE;
    V.nboxes = V.nbig = V.ntrack = V.nxref = V.nspawned = 0;
    V.boxes_built = V.boxes_sweep = false;
    V.nmsg = 0;
    if (V.mem_ready) re_arena_reset(&V.mem);
}

static void key_event_body(World* w, int key, bool pressed) {
    if (key < 0 || key >= 512) return;
    V.keys[key] = pressed;
    if (!V.playing || V.w != w) return;
    uint16_t kind = (uint16_t)(BPK_EVENT_KEY | (pressed ? 0 : 0x100));
    const bool paused = game_paused(); // (paused: only the actors that tick when paused hear the keys)
    for (uint32_t i = 0; i < V.ninst; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e) || (paused && !in->paused_tick)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++) {
            const BpEntry* en = &in->cls->entries_code[k];
            if (en->kind == kind && bp_key_code(en->name) == key) run(i, en->pc);
        }
    }
}

static void touch_event(World* w, int finger, float x, float y, bool pressed) {
    V.touch_xy[0] = x, V.touch_xy[1] = y, V.touch_finger = finger;
    uint16_t kind = (uint16_t)(BPK_EVENT_TOUCH | (pressed ? 0 : 0x100));
    for (uint32_t i = 0; i < V.ninst; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++)
            if (in->cls->entries_code[k].kind == kind) run(i, in->cls->entries_code[k].pc);
    }
}

static void touch_update_body(World* w, const float mouse[2], bool mouse_down) {
    int ids[10];
    float xy[10][2];
    int n = platform_touch_points(ids, xy, 10);
    if (n <= 0 && mouse && mouse_down) n = 1, ids[0] = 0, xy[0][0] = mouse[0], xy[0][1] = mouse[1]; // (the PC: the mouse is a finger)
    if (n < 0) n = 0;
    V.cursor_on = n > 0 || mouse != NULL;
    if (n > 0) V.cursor[0] = xy[0][0], V.cursor[1] = xy[0][1];
    else if (mouse) V.cursor[0] = mouse[0], V.cursor[1] = mouse[1];
    bool live = V.playing && V.w == w;
    // the fingers gone: Released (those that fired Pressed)
    for (int f = 0; f < 10; f++) {
        if (!V.fingers[f].state) continue;
        bool still = false;
        for (int p = 0; p < n; p++) still |= ids[p] == V.fingers[f].id;
        if (still) continue;
        if (V.fingers[f].state == 2 && live) touch_event(w, f, V.fingers[f].x, V.fingers[f].y, false);
        V.fingers[f].state = 0;
    }
    // the fingers here: a new one waits a frame (a touch widget may take it: then no event), then Pressed
    for (int p = 0; p < n; p++) {
        int f = -1, free_f = -1;
        for (int k = 0; k < 10; k++) {
            if (V.fingers[k].state && V.fingers[k].id == ids[p]) f = k;
            if (!V.fingers[k].state && free_f < 0) free_f = k;
        }
        if (f < 0) {
            if (free_f < 0) continue;
            f = free_f, V.fingers[f].id = ids[p], V.fingers[f].state = 1;
        } else if (V.fingers[f].state == 1) {
            if (ui_touch_held(ids[p])) V.fingers[f].state = 3; // (a widget's: never an event)
            else {
                V.fingers[f].state = 2;
                if (live) touch_event(w, f, xy[p][0], xy[p][1], true);
            }
        }
        V.fingers[f].x = xy[p][0], V.fingers[f].y = xy[p][1];
    }
}

void bp_set_input(const float move[2], const float look[2]) {
    float tm[2] = { 0, 0 }, tl[2] = { 0, 0 };
    ui_touch_axes(tm, tl);
    V.axes[0] = fminf(fmaxf(move[0] + tm[0], -1.0f), 1.0f), V.axes[1] = fminf(fmaxf(move[1] + tm[1], -1.0f), 1.0f);
    V.axes[2] = look[0] + tl[0], V.axes[3] = look[1] + tl[1];
}
void bp_boxes_changed(void) { V.boxes_dirty = true; } // (procgen.c: a facility built, the origin moved)
// An actor made while playing (Spawn Actor, a dropped Pickup, a network spawn): its boxes join at the next tick; too
// many in one frame: the whole set made again
void bp_box_spawned(Entity e) {
    if (!V.playing || !V.boxes_built) return; // (no boxes yet: build_boxes takes it)
    if (V.nspawned < sizeof V.spawned / sizeof V.spawned[0]) V.spawned[V.nspawned++] = e;
    else V.boxes_dirty = true;
}
void bp_box_stats(uint32_t* boxes, uint32_t* spawned, uint32_t* turns, double* ms) {
    if (boxes) *boxes = V.nboxes;
    if (spawned) *spawned = V.spawn_boxes;
    if (turns) *turns = V.platform_turns;
    if (ms) *ms = V.box_ms;
}
void bp_get_input(float move[2], float look[2]) { // (vehicle.c: the player's axes when it drives)
    move[0] = V.axes[0], move[1] = V.axes[1], look[0] = V.axes[2], look[1] = V.axes[3];
}

Entity bp_player(World* w) {
    if (!V.playing || V.w != w || !ecs_alive(w, V.player)) return ENTITY_NONE;
    return V.player;
}

bool bp_player_camera(World* w, float pos[3], float* yaw, float* pitch) { return bp_player_camera_ex(w, pos, yaw, pitch, NULL, NULL); }

// (Q12) the view with its roll and FOV: a pawn's camera component carries its First Person Camera and the shakes in its
// transform (bp_components.c); the other pawn views get the shakes here. A sequence's, a conversation's, a demo's: as they are
bool bp_player_camera_ex(World* w, float pos[3], float* yaw, float* pitch, float* roll, float* fov) {
    if (roll) *roll = 0.0f;
    if (fov) *fov = 90.0f;
    if (seq_view(w, pos, yaw, pitch, NULL)) return true; // a playing Level Sequence's Camera Cuts (sugestÃƒÂ£o #45)
    if (dlg_view_camera(w, pos, yaw, pitch)) return true; // a conversation's camera (dialogue.h: its speaker's)
    if (demo_play_view(w, pos, yaw, pitch)) return true; // a demo's own camera (the fight's)
    Entity p = bp_player(w);
    const CTransform* t = p ? ecs_get(w, p, COMP_TRANSFORM) : NULL;
    const CCharacter* c = p ? ecs_get(w, p, COMP_CHARACTER) : NULL;
    if (!t) return false;
    if (ecs_get(w, p, COMP_VEHICLE) && vehicle_camera(w, p, pos, yaw, pitch)) { // a vehicle's chase camera
        float fa = 0.0f;
        fpcam_shake_view(pos, yaw, pitch, roll, &fa);
        if (fov) *fov += fa;
        return true;
    }
    const CCamera* cc = NULL;
    if (bp_component_camera_ex(w, p, pos, yaw, pitch, roll, &cc)) { // its Camera component (on its Spring Arm; the shakes in it)
        if (fov && cc && (cc->fp & FP_ON)) *fov = cc->fov_deg > 1.0f ? cc->fov_deg : 90.0f; // (a First Person Camera: its own FOV)
        if (fov) *fov += fpcam_shake_fov();
        return true;
    }
    float y = c ? c->control_yaw : t->rotation[1], pt = c ? c->control_pitch : 0.0f;
    float dist = c ? c->camera_distance : 0.0f, hh = c ? c->half_height : 50.0f;
    float sp = sinf(pt * DEG2RAD), cp = cosf(pt * DEG2RAD), sy = sinf(y * DEG2RAD), cy = cosf(y * DEG2RAD);
    float fwd[3] = { cp * cy, cp * sy, sp };
    float pivot[3] = { t->location[0], t->location[1], t->location[2] + hh * 0.75f };
    for (int k = 0; k < 3; k++) pos[k] = pivot[k] - fwd[k] * dist;
    *yaw = y;
    *pitch = pt;
    float fa = 0.0f;
    fpcam_shake_view(pos, yaw, pitch, roll, &fa); // (Q12: the shakes on the pawn's plain view)
    if (fov) *fov += fa;
    return true;
}

void bp_apply_defaults(World* w, const BpAsset* cls) {
    if (!cls || cls->kind != BPA_CLASS) return;
    uint64_t mask = cls->comp_mask & COMP_ALL;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CBlueprint* b = e ? ecs_get(w, e, COMP_BLUEPRINT) : NULL;
        if (!b || bp_find_hash(b->asset_hash) != cls) continue;
        for (int c = 0; c < COMP_COUNT; c++) {
            if (c == COMP_NAME || c == COMP_TRANSFORM || c == COMP_BLUEPRINT || g_components[c].size > sizeof cls->comp_data[0]) continue;
            bool has = (ecs_mask(w, e) & COMP_BIT(c)) != 0;
            if (!(mask & COMP_BIT(c))) continue; // what the actor got in the level stays
            if (!has && !ecs_add(w, e, (ComponentId)c)) continue;
            void* dst = ecs_get(w, e, (ComponentId)c);
            if (dst) memcpy(dst, cls->comp_data[c], g_components[c].size);
        }
        CBlueprint* nb = ecs_get(w, e, COMP_BLUEPRINT);
        if (nb) nb->asset_hash = cls->hash; // a renamed class: its new hash from now on
        ecs_touch(w, e);
    }
    bp_components_rebuild(w, cls); // its component tree
}

// --- The Debug Viewer's measure (debug_viewer.c): only while it is open ---

void bp_profile(bool on) { V.profile = on; }

uint32_t bp_profile_items(BpProfileItem* out, uint32_t cap, double* movement_ms) {
    *movement_ms = V.playing ? V.prof_char_ms : 0.0;
    if (!V.playing || !V.profile) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < V.ninst && n < cap; i++) {
        const Inst* in = &insts()[i];
        if (!in->alive) continue;
        out[n].e = in->e;
        out[n].cls = in->cls ? in->cls->name : "";
        out[n].ms = in->prof_ms;
        out[n].ops = in->prof_ops;
        n++;
    }
    return n;
}

// --- Animation Blueprints (anim.c): their variables and events ---

static uint32_t inst_index_of(Entity e) {
    const CBlueprint* b = ecs_alive(V.w, e) ? ecs_get(V.w, e, COMP_BLUEPRINT) : NULL;
    return b && b->inst && b->inst <= V.ninst && insts()[b->inst - 1].e == e ? b->inst - 1 : UINT32_MAX;
}

static Inst* inst_of(World* w, Entity e) {
    if (!V.playing || V.w != w || !ecs_alive(w, e)) return NULL;
    const CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    if (!b || !b->inst || b->inst > V.ninst) return NULL;
    Inst* in = &insts()[b->inst - 1];
    return in->alive && in->e == e ? in : NULL;
}

bool bp_instance_var(World* w, Entity e, const char* name, BpValue* out) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int v = bp_var_index(in->cls, name);
    if (v < 0) return false;
    *out = in->r[in->cls->var_slot[v]];
    return true;
}

bool bp_instance_var_text(World* w, Entity e, const char* name, char* out, size_t cap) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int v = bp_var_index(in->cls, name);
    if (v < 0) return false;
    to_text(in, &in->r[in->cls->var_slot[v]], in->cls->vars[v].type, out, cap);
    return true;
}

bool bp_call_event(World* w, Entity e, const char* name) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int32_t pc = find_entry(in->cls, BPK_EVENT_CUSTOM, name);
    if (pc < 0) return false;
    run((uint32_t)(in - insts()), (uint32_t)pc);
    return true;
}

void bp_start_instance(World* w, Entity e) {
    if (!V.playing || V.w != w || !ecs_alive(w, e)) return;
    CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    BpAsset* cls = b && !b->inst ? bp_find_hash(b->asset_hash) : NULL;
    if (!cls || (!cls->compiled && !bp_compile(cls))) return;
    uint32_t ii = make_instance(e, cls);
    construct(ii);
    int32_t pc = ii != UINT32_MAX ? find_entry(cls, BPK_EVENT_BEGIN_PLAY, NULL) : -1;
    if (pc >= 0) run(ii, (uint32_t)pc);
}

bool bp_key_held(int key) { return key >= 0 && key < 512 && V.keys[key] != 0; }

int bp_debug_instance(Entity e, int* alive, int* inputs) { // (the tests' diagnostics)
    uint32_t i = inst_index_of(e);
    *alive = *inputs = 0;
    if (i == UINT32_MAX) return -1;
    *alive = insts()[i].alive;
    for (uint32_t k = 0; k < insts()[i].cls->nentry; k++) *inputs += (insts()[i].cls->entries_code[k].kind & 0xFFF) == BPK_EVENT_INPUT_ACTION;
    return (int)i;
}

// (the tests' diagnostics: an instance's class code, entries and registers, on a failure of --bp-test)
void bp_debug_dump(Entity e, const char* label) {
    if (!V.playing || !V.w) { // (no Play: its setup failed; nothing to read, and no world to look in)
        printf("[RealEngine] dump %s: no Play\n", label);
        return;
    }
    uint32_t ii = inst_index_of(e);
    if (ii == UINT32_MAX) {
        printf("[RealEngine] dump %s: no instance\n", label);
        return;
    }
    const Inst* in = &insts()[ii];
    const BpAsset* a = in->cls;
    printf("[RealEngine] dump %s: class %s, %u ops, %u slots, %u entries, alive %u\n", label, a->name, a->ncode, a->nslots, a->nentry, in->alive);
    for (uint32_t i = 0; i < a->nentry; i++)
        printf("[RealEngine]   entry %s kind %u pc %u\n", a->entries_code[i].name, a->entries_code[i].kind, a->entries_code[i].pc);
    for (uint32_t i = 0; i < a->ncode; i++)
        printf("[RealEngine]   %3u: op %u a %u b %u c %u d %u\n", i, a->code[i].op, a->code[i].a, a->code[i].b, a->code[i].c, a->code[i].d);
    for (uint32_t s = 0; s < a->nslots; s++)
        printf("[RealEngine]   r%u = %g %g %g %g\n", s, in->r[s].v[0], in->r[s].v[1], in->r[s].v[2], in->r[s].v[3]);
    for (uint32_t k = 0; k < a->ntlc; k++) { // (2026-11-25) its Timelines: where they are, their outputs' code
        const struct BpTlCode* tc = &a->tlc[k];
        printf("[RealEngine]   timeline %u: t %g dir %g playing %u; update pc %d, finished pc %d, events %d %d %d, tracks %u, jit %s, depth %u\n", k,
               in->tls[k].t, in->tls[k].dir, in->tls[k].playing, tc->pc_update, tc->pc_finished, tc->pc_event[0], tc->pc_event[1], tc->pc_event[2],
               tc->tl ? tc->tl->ntracks : 0u, a->jit ? "yes" : "no", V.depth);
    }
    for (uint32_t s = 0; s < a->nvars; s++) printf("[RealEngine]   var %s slot %u\n", a->vars[s].name, a->var_slot[s]);
}

// --- online.c and the saves: the possessed pawn, an instance's variables, events of a kind fired from outside ---------
void bp_set_player(World* w, Entity e) {
    if (!V.playing || V.w != w || !ecs_alive(w, e)) return;
    V.player = e;
}

// a plain variable (not a container, not an actor or object: what makes sense on another machine or in a file)
static bool portable_var(const BpVar* v) {
    return !v->cont && v->type != BPT_ACTOR && v->type != BPT_OBJECT && v->type != BPT_CLASS && v->type != BPT_EXEC;
}

bool bp_get_instance_var(World* w, Entity e, const char* name, BpValue* out, uint32_t cap, uint32_t* width, char* text,
                         size_t tcap, bool* is_text) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int v = bp_var_index(in->cls, name);
    if (v < 0 || !portable_var(&in->cls->vars[v])) return false;
    const BpVar* bv = &in->cls->vars[v];
    *is_text = bp_type_stringish(bv->type);
    if (*is_text) {
        snprintf(text, tcap, "%s", str_of(in, &in->r[in->cls->var_slot[v]]));
        *width = 1;
        return true;
    }
    uint32_t wd = bp_var_width(bv);
    if (wd > cap) wd = cap;
    memcpy(out, &in->r[in->cls->var_slot[v]], wd * sizeof(BpValue));
    *width = wd;
    return true;
}

bool bp_set_instance_var(World* w, Entity e, const char* name, const BpValue* v, uint32_t width) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int k = bp_var_index(in->cls, name);
    if (k < 0 || !portable_var(&in->cls->vars[k]) || bp_type_stringish(in->cls->vars[k].type)) return false;
    uint32_t wd = bp_var_width(&in->cls->vars[k]);
    memcpy(&in->r[in->cls->var_slot[k]], v, (width < wd ? width : wd) * sizeof(BpValue));
    return true;
}

bool bp_set_instance_text(World* w, Entity e, const char* name, const char* text) {
    Inst* in = inst_of(w, e);
    if (!in) return false;
    int k = bp_var_index(in->cls, name);
    if (k < 0 || !bp_type_stringish(in->cls->vars[k].type)) return false;
    set_str(&in->r[in->cls->var_slot[k]], text ? text : "");
    return true;
}

// Every plain variable as records: name, kind (0 values, 1 text), width, then the values or the text (save_only: the ones
// marked SaveGame, save_progress.h)
uint32_t bp_vars_dump(World* w, Entity e, uint8_t* out, uint32_t cap) { return bp_vars_dump_ex(w, e, out, cap, false); }
uint32_t bp_vars_dump_ex(World* w, Entity e, uint8_t* out, uint32_t cap, bool save_only) {
    Inst* in = inst_of(w, e);
    if (!in) return 0;
    uint32_t at = 0;
    if (cap < 8) return 0;
    memcpy(out, "RESAV001", 8), at = 8;
    for (uint32_t i = 0; i < in->cls->nvars; i++) {
        const BpVar* v = &in->cls->vars[i];
        if (!portable_var(v) || (save_only && !v->save_game)) continue;
        const BpValue* r = &in->r[in->cls->var_slot[i]];
        size_t nl = strlen(v->name);
        bool txt = bp_type_stringish(v->type);
        const char* s = txt ? str_of(in, r) : "";
        uint32_t wd = txt ? 0 : bp_var_width(v), sl = (uint32_t)strlen(s);
        uint32_t need = 1 + (uint32_t)nl + 2 + (txt ? 2 + sl : wd * (uint32_t)sizeof(BpValue));
        if (at + need > cap) return 0;
        out[at++] = (uint8_t)nl;
        memcpy(out + at, v->name, nl), at += (uint32_t)nl;
        out[at++] = txt ? 1 : 0;
        out[at++] = (uint8_t)wd;
        if (txt) {
            out[at++] = (uint8_t)sl, out[at++] = (uint8_t)(sl >> 8);
            memcpy(out + at, s, sl), at += sl;
        } else {
            memcpy(out + at, r, wd * sizeof(BpValue)), at += wd * (uint32_t)sizeof(BpValue);
        }
    }
    return at;
}

bool bp_vars_load(World* w, Entity e, const uint8_t* d, uint32_t len) {
    if (!inst_of(w, e) || len < 8 || memcmp(d, "RESAV001", 8)) return false;
    uint32_t at = 8;
    while (at + 3 < len) {
        uint32_t nl = d[at++];
        if (at + nl + 2 > len) return false;
        char name[BP_NAME];
        uint32_t k = nl < BP_NAME - 1 ? nl : BP_NAME - 1;
        memcpy(name, d + at, k), name[k] = 0, at += nl;
        uint8_t txt = d[at++], wd = d[at++];
        if (txt) {
            if (at + 2 > len) return false;
            uint32_t sl = d[at] | (uint32_t)d[at + 1] << 8;
            at += 2;
            if (at + sl > len) return false;
            char s[1024];
            uint32_t c = sl < sizeof s - 1 ? sl : (uint32_t)sizeof s - 1;
            memcpy(s, d + at, c), s[c] = 0, at += sl;
            bp_set_instance_text(w, e, name, s); // (a variable gone since the save: skipped)
        } else {
            if (at + wd * sizeof(BpValue) > len) return false;
            BpValue v[16];
            uint32_t c = wd < 16 ? wd : 16;
            memcpy(v, d + at, c * sizeof(BpValue)), at += wd * (uint32_t)sizeof(BpValue);
            bp_set_instance_var(w, e, name, v, c);
        }
    }
    return true;
}

// --- Health (health.h): the actor's events, its destroy (End Play first, as Destroy Actor), its Skeletal Mesh's ragdoll ---
static void health_event(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b) {
    static const uint16_t kinds[HEALTH_EV_COUNT] = { BPK_EVENT_ANY_DAMAGE, BPK_EVENT_HEALED, BPK_EVENT_DEATH, BPK_EVENT_SHIELD_BROKEN };
    if (!V.playing || V.w != w || ev < 0 || ev >= HEALTH_EV_COUNT) return;
    if (ev == HEALTH_EV_DEATH) quest_notify_death(w, e, a); // (the quests' Kill objectives: quest.h)
    if (ev == HEALTH_EV_DEATH) rules_on_death(w, e, a, type); // (game_rules.h: the killer's points, a player's life; before the actor's own On Death)
    Inst* in = inst_of(w, e);
    int32_t pc = in ? find_entry(in->cls, kinds[ev], NULL) : -1;
    if (pc < 0) return;
    BpValue* s = V.stage; // (the event's outputs, as the network's events get theirs)
    memset(s, 0, 4 * sizeof *s);
    switch (ev) {
    case HEALTH_EV_DAMAGED: s[0].v[0] = amount, set_str(&s[1], type), set_ent(&s[2], a), set_ent(&s[3], b); break;
    case HEALTH_EV_HEALED: s[0].v[0] = amount, set_ent(&s[1], a); break;
    case HEALTH_EV_DEATH: set_ent(&s[0], a), set_str(&s[1], type), set_ent(&s[2], b); break;
    default: set_ent(&s[0], a); break;
    }
    run((uint32_t)(in - insts()), (uint32_t)pc);
}
static void health_destroy(World* w, Entity e) {
    if (!ecs_alive(w, e)) return;
    if (V.playing && V.w == w) {
        if (e == V.player) V.player = ENTITY_NONE;
        uint32_t ti = inst_index_of(e);
        int32_t epc = ti != UINT32_MAX ? find_entry(insts()[ti].cls, BPK_EVENT_END_PLAY, NULL) : -1;
        if (epc >= 0) run(ti, (uint32_t)epc);
        V.boxes_sweep = true;
    }
    if (ecs_alive(w, e)) ecs_destroy(w, e);
}
void bp_destroy_actor(World* w, Entity e, bool end_play) {
    if (end_play) {
        health_destroy(w, e);
        return;
    }
    if (!ecs_alive(w, e)) return;
    if (e == V.player) V.player = ENTITY_NONE;
    ecs_destroy(w, e);
    V.boxes_sweep = true;
}
bool bp_has_save_vars(World* w, Entity e) {
    Inst* in = inst_of(w, e);
    for (uint32_t i = 0; in && i < in->cls->nvars; i++)
        if (in->cls->vars[i].save_game && portable_var(&in->cls->vars[i])) return true;
    return false;
}
static bool health_ragdoll(World* w, Entity e, bool on) {
    Entity sk = anim_mesh_entity(w, e);
    const CSkeletalMesh* s = sk ? ecs_get(w, sk, COMP_SKELETAL_MESH) : NULL;
    if (!s || !s->physics_asset) return false;
    phys_set_simulate(sk, on ? 1 : 0, NULL);
    return true;
}

// The events of a kind (the network's, Steam's): every instance that has one, with its outputs' values
void bp_fire_event(World* w, uint16_t kind, const BpValue* args, uint32_t nargs) {
    if (!V.playing || V.w != w) return;
    for (uint32_t i = 0; i < V.ninst; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++) {
            if ((in->cls->entries_code[k].kind & 0xFFF) != kind) continue;
            if (nargs) memcpy(V.stage, args, (nargs < 64 ? nargs : 64) * sizeof(BpValue));
            run(i, in->cls->entries_code[k].pc);
        }
    }
}

// The events of a kind on one actor (inventory.c): its own instance and its Actor Components', with their outputs' values
void bp_fire_actor_event(World* w, Entity actor, uint16_t kind, const BpValue* args, uint32_t nargs) {
    if (!V.playing || V.w != w || actor == ENTITY_NONE) return;
    for (uint32_t i = 0; i < V.ninst; i++) {
        Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(w, in->e) || (in->e != actor && bp_component_owner(w, in->e) != actor)) continue;
        for (uint32_t k = 0; k < in->cls->nentry; k++) {
            if (in->cls->entries_code[k].kind != kind) continue;
            if (nargs) memcpy(V.stage, args, (nargs < 64 ? nargs : 64) * sizeof(BpValue));
            run(i, in->cls->entries_code[k].pc);
        }
    }
}

// A Custom Event on an actor with its inputs' values (a Lever's target: its bool input On); false: none by that name
bool bp_call_event_args(World* w, Entity e, const char* name, const BpValue* args, uint32_t nargs) {
    Inst* in = inst_of(w, e);
    int32_t pc = in ? find_entry(in->cls, BPK_EVENT_CUSTOM, name) : -1;
    if (pc < 0) return false;
    memset(V.stage, 0, sizeof V.stage);
    if (nargs) memcpy(V.stage, args, (nargs < 64 ? nargs : 64) * sizeof(BpValue));
    run((uint32_t)(in - insts()), (uint32_t)pc);
    return true;
}

// (2026-11-23, anim_combo.inc) a montage's windows told to its actor (and its Actor Components)
void bp_montage_window_event(World* w, Entity actor, const char* name, bool begin) {
    BpValue s[2];
    memset(s, 0, sizeof s);
    set_str(&s[0], name), s[1].v[0] = begin ? 1.0f : 0.0f;
    bp_fire_actor_event(w, actor, BPK_EVENT_MONTAGE_WINDOW, s, 2);
}
void bp_montage_hit_event(World* w, Entity actor, Entity other, const float loc[3], const char* window) {
    BpValue s[3];
    memset(s, 0, sizeof s);
    set_ent(&s[0], other), memcpy(s[1].v, loc, 12), set_str(&s[2], window);
    bp_fire_actor_event(w, actor, BPK_EVENT_MONTAGE_HIT, s, 3);
}

// --- Interaction (interaction.h): the Interactable's events on its actor (and its Actor Components), a target's Custom Event ---
static void ia_event(World* w, Entity e, int ev, Entity a, float progress) {
    static const uint16_t kinds[IA_EV_COUNT] = { BPK_EVENT_IA_INTERACT, BPK_EVENT_IA_FOCUS_BEGIN, BPK_EVENT_IA_FOCUS_END, BPK_EVENT_IA_HOLD };
    if (!V.playing || V.w != w || ev < 0 || ev >= IA_EV_COUNT) return;
    if (ev == IA_EV_INTERACT) quest_notify_interact(w, e, a); // (the quests' Interact objectives: quest.h)
    BpValue s[2];
    memset(s, 0, sizeof s);
    if (ev == IA_EV_HOLD) s[0].v[0] = progress, set_ent(&s[1], a);
    else set_ent(&s[0], a);
    bp_fire_actor_event(w, e, kinds[ev], s, 2);
}
static bool ia_call(World* w, Entity e, const char* event, bool on) {
    BpValue v;
    memset(&v, 0, sizeof v);
    v.v[0] = on ? 1.0f : 0.0f;
    return bp_call_event_args(w, e, event, &v, 1);
}

// a play string as a value (online.c builds its events' text outputs with it)
void bp_make_string(BpValue* v, const char* s) { set_str(v, s ? s : ""); }

// The network's and Steam's events into the Blueprints that have them (once a frame, after online_pre_tick)
void bp_online_events(World* w) {
    OnlineEvent ev;
    while (online_poll(&ev)) {
        BpValue a[2];
        memset(a, 0, sizeof a);
        uint16_t kind = 0;
        switch (ev.type) {
        case ONLINE_EV_PLAYER_JOINED: kind = BPK_EVENT_NET_PLAYER_JOINED; a[0].v[0] = (float)ev.i; set_str(&a[1], ev.text); break;
        case ONLINE_EV_PLAYER_LEFT: kind = BPK_EVENT_NET_PLAYER_LEFT; a[0].v[0] = (float)ev.i; set_str(&a[1], ev.text); break;
        case ONLINE_EV_SESSION_CREATED: kind = BPK_EVENT_NET_SESSION_CREATED; a[0].v[0] = (float)ev.ok; break;
        case ONLINE_EV_SESSIONS_FOUND: kind = BPK_EVENT_NET_SESSIONS_FOUND; a[0].v[0] = (float)ev.i; break;
        case ONLINE_EV_JOINED: kind = BPK_EVENT_NET_JOINED; a[0].v[0] = (float)ev.ok; set_str(&a[1], ev.text); break;
        case ONLINE_EV_ERROR: kind = BPK_EVENT_NET_ERROR; set_str(&a[0], ev.text); break;
        case ONLINE_EV_CHAT: kind = BPK_EVENT_NET_CHAT; a[0].v[0] = (float)ev.i; set_str(&a[1], ev.text); break;
        case ONLINE_EV_LEADERBOARD: kind = BPK_EVENT_STEAM_LEADERBOARD; a[0].v[0] = (float)ev.i; break;
        case ONLINE_EV_OVERLAY: kind = BPK_EVENT_STEAM_OVERLAY; a[0].v[0] = (float)ev.ok; break;
        case ONLINE_EV_SESSION_UPDATED: kind = BPK_EVENT_NET_SESSION_UPDATED; set_str(&a[0], ev.text); break;
        case ONLINE_EV_SESSION_STATE: kind = BPK_EVENT_NET_SESSION_STATE; a[0].v[0] = (float)ev.i; break;
        case ONLINE_EV_INVITE: kind = BPK_EVENT_NET_INVITE; set_str(&a[0], ev.text); a[1].v[0] = (float)ev.ok; break;
        case ONLINE_EV_JOIN_REFUSED: { // "name: reason" into its two outputs
            kind = BPK_EVENT_NET_JOIN_REFUSED;
            char nm[128];
            snprintf(nm, sizeof nm, "%s", ev.text);
            char* colon = strstr(nm, ": ");
            if (colon) *colon = 0;
            set_str(&a[0], nm), set_str(&a[1], colon ? colon + 2 : "");
            break;
        }
        default: break;
        }
        if (kind) bp_fire_event(w, kind, a, 2);
        if (ev.type == ONLINE_EV_ERROR || (ev.type == ONLINE_EV_JOINED && !ev.ok)) message("%s", ev.text[0] ? ev.text : "network error"); // (on screen, as Print String)
    }
}

// A Custom Event run on an actor with its inputs given as text (a Dialogue's action): each converted to the event's input
// of its place (a number, a bool, a vector "x y z", a string / name / text)
bool bp_call_event_text(World* w, Entity e, const char* name, const char* const* args, uint32_t nargs) {
    Inst* in = V.playing && V.w == w ? inst_of(w, e) : NULL;
    if (!in || !name || !name[0]) return false;
    int32_t pc = find_entry(in->cls, BPK_EVENT_CUSTOM, name);
    if (pc < 0) return false;
    const BpNode* ev = NULL; // (its node: the inputs' types)
    for (uint32_t g = 0; g < in->cls->ngraphs && !ev; g++)
        for (uint32_t k = 0; k < in->cls->graphs[g].nnodes && !ev; k++) {
            const BpNode* n = &in->cls->graphs[g].nodes[k];
            if (n->kind == BPK_EVENT_CUSTOM && !strcmp(n->text, name)) ev = n;
        }
    memset(V.stage, 0, sizeof V.stage);
    uint32_t at = 0, ai = 0;
    for (int p = 0; ev && p < ev->npins && at < 64; p++) {
        const BpPin* pn = &ev->pins[p];
        if (!pn->out || pn->type == BPT_EXEC || pn->sub > 1) continue;
        uint32_t wd = whole_width(pn);
        const char* s = ai < nargs && args && args[ai] ? args[ai] : "";
        ai++;
        BpValue* d = &V.stage[at];
        if (bp_type_stringish(pn->type)) set_str(d, s);
        else if (pn->type == BPT_BOOL) d->v[0] = (!_stricmp(s, "true") || !_stricmp(s, "yes") || atof(s) != 0.0) ? 1.0f : 0.0f;
        else if (pn->type == BPT_VECTOR || pn->type == BPT_ROTATOR) sscanf(s, "%f %f %f", &d->v[0], &d->v[1], &d->v[2]);
        else if (pn->type != BPT_ACTOR) d->v[0] = (float)atof(s);
        at += wd ? wd : 1;
    }
    run((uint32_t)(in - insts()), (uint32_t)pc);
    return true;
}

// --- Dialogue (dialogue.h): its events to every Blueprint that has them, the variables its conditions read ---
static void dlg_event_hook(World* w, int ev, Entity speaker, const char* s0, const char* s1, int i) {
    BpValue a[4];
    memset(a, 0, sizeof a);
    set_ent(&a[0], speaker);
    switch (ev) {
    case DLG_EV_LINE: set_str(&a[1], s0), set_str(&a[2], s1), a[3].v[0] = (float)i, bp_fire_event(w, BPK_EVENT_DLG_LINE, a, 4); break;
    case DLG_EV_CHOICE: a[1].v[0] = (float)i, set_str(&a[2], s1), bp_fire_event(w, BPK_EVENT_DLG_CHOICE, a, 3); break;
    case DLG_EV_END:
        set_str(&a[1], s0), bp_fire_event(w, BPK_EVENT_DLG_END, a, 2);
        quest_notify_talk(w, speaker, s0); // (the quests' Talk objectives: quest.h)
        break;
    default: break;
    }
}

// --- Quests (quest.h): their events to every Blueprint that has them ---
static void quest_event_hook(World* w, int ev, Entity owner, const char* quest, const char* obj, int index, int progress, int count) {
    BpValue a[5];
    memset(a, 0, sizeof a);
    set_str(&a[0], quest);
    switch (ev) {
    case QST_EV_STARTED: set_ent(&a[1], owner), bp_fire_event(w, BPK_EVENT_QST_STARTED, a, 2); break;
    case QST_EV_OBJ_UPDATED:
        set_str(&a[1], obj), a[2].v[0] = (float)index, a[3].v[0] = (float)progress, a[4].v[0] = (float)count;
        bp_fire_event(w, BPK_EVENT_QST_OBJ_UPDATED, a, 5);
        break;
    case QST_EV_OBJ_COMPLETED: set_str(&a[1], obj), a[2].v[0] = (float)index, bp_fire_event(w, BPK_EVENT_QST_OBJ_COMPLETED, a, 3); break;
    case QST_EV_COMPLETED: set_ent(&a[1], owner), bp_fire_event(w, BPK_EVENT_QST_COMPLETED, a, 2); break;
    case QST_EV_FAILED: set_ent(&a[1], owner), bp_fire_event(w, BPK_EVENT_QST_FAILED, a, 2); break;
    default: break;
    }
}
static bool dlg_get_var(World* w, Entity e, const char* var, float* num, char* text, size_t cap) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    text[0] = 0, *num = 0.0f;
    if (!bp_get_instance_var(w, e, var, v, 4, &wd, text, cap, &is_text)) return false;
    *num = is_text ? (float)atof(text) : v[0].v[0];
    return true;
}
static bool dlg_set_var(World* w, Entity e, const char* var, const char* value) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    char t[8];
    if (!bp_get_instance_var(w, e, var, v, 4, &wd, t, sizeof t, &is_text)) return false;
    if (is_text) return bp_set_instance_text(w, e, var, value);
    memset(v, 0, sizeof v);
    if (!_stricmp(value, "true")) v[0].v[0] = 1.0f;
    else if (sscanf(value, "%f %f %f", &v[0].v[0], &v[0].v[1], &v[0].v[2]) < 1) v[0].v[0] = 0.0f;
    return bp_set_instance_var(w, e, var, v, wd ? wd : 1);
}
static bool dlg_camera_hook(World* w, float pos[3], float* yaw, float* pitch) { return bp_player_camera(w, pos, yaw, pitch); }

// --- Abilities and effects (ability.h): Target unconnected is self (in a Gameplay Ability: its owner) ---
static bool abil_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    Entity self = in->e, owner = abil_owner_of(w, self), t = ent_of(A) ? ent_of(A) : owner ? owner : self;
    BpValue res = { { 0 } };
    switch (kind) {
    case BPK_ABIL_GIVE: {
        const AbilityDef* d = abil_ability_by_name(str_of(in, B));
        int slot = d ? abil_give(w, t, d->hash) : -1;
        if (!d) printf("[RealEngine] Give Ability: no Gameplay Ability \"%s\"\n", str_of(in, B));
        res.v[0] = slot >= 0 ? 1.0f : 0.0f;
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = (float)slot;
        break;
    }
    case BPK_ABIL_REMOVE: {
        const AbilityDef* d = abil_ability_by_name(str_of(in, B));
        res.v[0] = (float)(d && abil_remove(w, t, d->hash));
        break;
    }
    case BPK_ABIL_TRY: {
        const AbilityDef* d = abil_ability_by_name(str_of(in, B));
        int r = d ? abil_try_activate(w, t, d->hash) : ABIL_FAIL_NOT_GRANTED;
        res.v[0] = r == ABIL_OK ? 1.0f : 0.0f;
        set_str(&D[1], g_abil_fail_names[r < ABIL_FAIL_COUNT ? r : ABIL_FAIL_NOT_GRANTED]);
        break;
    }
    case BPK_ABIL_TRY_TAG: {
        uint32_t which = 0;
        int r = abil_try_activate_tag(w, t, str_of(in, B), &which);
        const AbilityDef* d = abil_ability_by_hash(which);
        res.v[0] = r == ABIL_OK ? 1.0f : 0.0f;
        set_str(&D[1], d ? d->name : "None");
        break;
    }
    case BPK_ABIL_CANCEL: res.v[0] = (float)abil_cancel(w, t, str_of(in, B)); break;
    case BPK_ABIL_END: abil_end(w, ent_of(A) ? ent_of(A) : self); return true;
    case BPK_ABIL_OWNER: set_ent(&res, abil_owner_of(w, ent_of(A) ? ent_of(A) : self)); break;
    case BPK_ABIL_ON_COOLDOWN: case BPK_ABIL_IS_ACTIVE: {
        const AbilityDef* d = abil_ability_by_name(str_of(in, B));
        res.v[0] = (float)(d && (kind == BPK_ABIL_ON_COOLDOWN ? abil_on_cooldown(w, t, d->hash) : abil_is_active(w, t, d->hash)));
        break;
    }
    case BPK_ABIL_COOLDOWN: {
        const AbilityDef* d = abil_ability_by_name(str_of(in, B));
        float dur = 0.0f;
        int ch = 0, mx = 0;
        res.v[0] = d ? abil_cooldown_remaining(w, t, d->hash, &dur, &ch, &mx) : 0.0f;
        for (int k = 1; k < 4; k++) memset(&D[k], 0, sizeof D[k]);
        D[1].v[0] = dur, D[2].v[0] = (float)ch, D[3].v[0] = (float)mx;
        break;
    }
    case BPK_GE_APPLY: {
        const EffectDef* d = abil_effect_by_name(str_of(in, B));
        if (!d) printf("[RealEngine] Apply Gameplay Effect: no Gameplay Effect \"%s\"\n", str_of(in, B));
        int st = d ? abil_apply_effect(w, t, d->hash, ent_of(C) ? ent_of(C) : owner ? owner : self) : 0;
        res.v[0] = st > 0 ? 1.0f : 0.0f;
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = (float)st;
        break;
    }
    case BPK_GE_REMOVE: res.v[0] = (float)abil_remove_effect(w, t, str_of(in, B), (int)C->v[0]); break;
    case BPK_GE_HAS: {
        float rem = 0.0f;
        int st = abil_has_effect(w, t, str_of(in, B), &rem);
        res.v[0] = st > 0 ? 1.0f : 0.0f;
        memset(&D[1], 0, sizeof D[1]), memset(&D[2], 0, sizeof D[2]);
        D[1].v[0] = (float)st, D[2].v[0] = rem;
        break;
    }
    case BPK_ATTR_GET: {
        float cur = 0.0f, base = 0.0f;
        bool f = abil_get_attr(w, t, str_of(in, B), &cur, &base);
        res.v[0] = cur;
        memset(&D[1], 0, sizeof D[1]), memset(&D[2], 0, sizeof D[2]);
        D[1].v[0] = base, D[2].v[0] = f ? 1.0f : 0.0f;
        break;
    }
    case BPK_ATTR_SET: abil_set_attr(w, t, str_of(in, B), C->v[0]); return true;
    default: return false;
    }
    *D = res;
    return true;
}
// =====================================================================================================================
// The Blueprint debugger (bp_debug.h): breakpoints as OP_BREAK in the code, the pause on the VM's fiber, the steps, the
// values, the call stack, the traces of the wires that ran
// =====================================================================================================================

#define DBG_USER 1u
#define DBG_STEP 2u
#define DBG_TRACE 4u
typedef struct DbgPatch { BpAsset* a; uint32_t pc; BpOp orig; uint8_t why; } DbgPatch;
typedef struct DbgBreak { const BpAsset* owner; uint32_t node; uint16_t graph; bool enabled; uint32_t hits; } DbgBreak;
typedef struct DbgTrace { const BpAsset* owner; uint32_t node_id; Entity actor; double t; uint16_t graph; uint8_t ord; } DbgTrace;
#define DBG_MAX_BREAKS 256u
#define DBG_TRACES 2048u

static struct {
    DbgBreak brk[DBG_MAX_BREAKS];
    uint32_t nbrk;
    DbgPatch* patch;            // (an editor's table: grown when breakpoints are set, never in a frame)
    uint32_t npatch, cap_patch; // (npatch: the slots used so far; a free one has a NULL asset)
    uint32_t nactive;           // patches in place: the ticks run on the VM's fiber while any is
    int cmd;                    // the command waiting for the next tick (BPD_*)
    bool paused, stepping;
    int step_mode, step_depth, step_line;
    const BpAsset* step_cls;
    // where it stopped: the instance, the op, its run's return stack, the run's depth
    uint32_t inst, pc, sp, depth;
    uint32_t stack[BP_MAX_CALL];
    uint16_t node; // (an index of the class's dbg_nodes; UINT16_MAX: none)
    struct { const BpAsset* cls; Entity e; } obj[64];
    uint32_t nobj;
    const BpAsset* watched[16];
    uint32_t nwatched;
    DbgTrace trace[DBG_TRACES];
    uint32_t ntrace;
    BpDbgStats stats;
} D;

static bool dbg_on(const BpDbgNode* n, const DbgBreak* b) { return n->owner == b->owner && n->graph == b->graph && n->node_id == b->node; }
static bool dbg_watched(const BpAsset* owner) {
    for (uint32_t i = 0; i < D.nwatched; i++)
        if (D.watched[i] == owner) return true;
    return false;
}

// The JIT's code was made from the code before a patch changed it: dropped by the next run at the top (bp_vm.c run)
static void jit_outdated(BpAsset* a) {
    if (a->jit) a->jit_ok = false, a->jit_stale = true;
}

static void patch_set(BpAsset* a, uint32_t pc, uint8_t why) {
    if (!a->compiled || pc >= a->ncode) return;
    if (a->code[pc].op == OP_BREAK) {
        D.patch[a->code[pc].a].why |= why;
        return;
    }
    uint32_t i = 0;
    while (i < D.npatch && D.patch[i].a) i++;
    if (i == D.npatch) {
        if (D.npatch >= 0xFFFFu) return;
        if (D.npatch == D.cap_patch) {
            uint32_t cap = D.cap_patch ? D.cap_patch * 2 : 256;
            DbgPatch* np = realloc(D.patch, sizeof(DbgPatch) * cap); // (setting breakpoints: not a frame)
            if (!np) return;
            D.patch = np, D.cap_patch = cap;
        }
        D.npatch++;
    }
    D.patch[i] = (DbgPatch){ a, pc, a->code[pc], why };
    a->code[pc] = (BpOp){ OP_BREAK, (uint16_t)i, 0, 0, 0 };
    a->dbg_patched++, D.nactive++;
    D.stats.patches = D.nactive;
    jit_outdated(a);
}

static void patch_clear(BpAsset* a, uint32_t pc, uint8_t why) {
    if (pc >= a->ncode || a->code[pc].op != OP_BREAK) return;
    DbgPatch* p = &D.patch[a->code[pc].a];
    p->why &= (uint8_t)~why;
    if (p->why) return;
    a->code[pc] = p->orig;
    p->a = NULL;
    a->dbg_patched--, D.nactive--;
    D.stats.patches = D.nactive;
    jit_outdated(a);
}

// The asset's patches of one kind made again: off at every site, then on where they belong
static void reapply(BpAsset* a, uint8_t why) {
    if (!a->compiled || !a->dbg_sites) return;
    for (uint32_t s = 0; s < a->ndbg_sites; s++) patch_clear(a, a->dbg_sites[s].pc, why);
    for (uint32_t s = 0; s < a->ndbg_sites; s++) {
        const BpDbgNode* n = &a->dbg_nodes[a->dbg_sites[s].node];
        bool on = false;
        if (why == DBG_USER)
            for (uint32_t b = 0; b < D.nbrk && !on; b++) on = D.brk[b].enabled && dbg_on(n, &D.brk[b]);
        else if (why == DBG_TRACE) on = V.playing && dbg_watched(n->owner);
        else on = D.stepping;
        if (on) patch_set(a, a->dbg_sites[s].pc, why);
    }
}
static void reapply_all(uint8_t why) {
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        BpAsset* a = bp_asset_at(i);
        if (a && a->kind == BPA_CLASS && a->compiled) reapply(a, why);
    }
}

void bp_debug_forget(BpAsset* a) { // (its code as compiled again: the ops put back)
    for (uint32_t i = 0; i < D.npatch; i++)
        if (D.patch[i].a == a) {
            if (D.patch[i].pc < a->ncode && a->code[D.patch[i].pc].op == OP_BREAK) a->code[D.patch[i].pc] = D.patch[i].orig;
            D.patch[i].a = NULL, D.nactive--;
        }
    a->dbg_patched = 0;
    D.stats.patches = D.nactive;
}
void bp_debug_compiled(BpAsset* a) {
    if (D.paused) return; // (bp_compile refuses then)
    reapply(a, DBG_USER);
    if (D.nwatched) reapply(a, DBG_TRACE);
    if (D.stepping) reapply(a, DBG_STEP);
}
// An asset's contents moved into another (bp_adopt: a script compiled again): its tables point at the one kept
void bp_debug_moved(BpAsset* from, BpAsset* to) {
    for (uint32_t i = 0; i < to->ndbg_nodes; i++)
        if (to->dbg_nodes[i].owner == from) to->dbg_nodes[i].owner = to;
    for (uint32_t i = 0; i < to->ndbg_vars; i++)
        if (to->dbg_vars[i].owner == from) to->dbg_vars[i].owner = to;
    for (uint32_t i = 0; i < D.npatch; i++)
        if (D.patch[i].a == from) D.patch[i].a = to;
}

// --- Breakpoints ---

static int32_t break_index(const BpAsset* owner, uint32_t graph, uint32_t node) {
    for (uint32_t i = 0; i < D.nbrk; i++)
        if (D.brk[i].owner == owner && D.brk[i].graph == graph && D.brk[i].node == node) return (int32_t)i;
    return -1;
}
bool bp_debug_toggle(BpAsset* owner, uint32_t graph, uint32_t node_id) {
    if (!owner) return false;
    int32_t i = break_index(owner, graph, node_id);
    bool now = i < 0;
    if (i >= 0) D.brk[i] = D.brk[--D.nbrk];
    else if (D.nbrk < DBG_MAX_BREAKS) D.brk[D.nbrk++] = (DbgBreak){ owner, node_id, (uint16_t)graph, true, 0 };
    else return false;
    reapply_all(DBG_USER);
    return now;
}
bool bp_debug_has(const BpAsset* owner, uint32_t graph, uint32_t node_id, bool* enabled) {
    int32_t i = break_index(owner, graph, node_id);
    if (enabled) *enabled = i >= 0 && D.brk[i].enabled;
    return i >= 0;
}
void bp_debug_enable(BpAsset* owner, uint32_t graph, uint32_t node_id, bool on) {
    int32_t i = break_index(owner, graph, node_id);
    if (i < 0 || D.brk[i].enabled == on) return;
    D.brk[i].enabled = on;
    reapply_all(DBG_USER);
}
void bp_debug_clear(const BpAsset* owner) {
    for (uint32_t i = 0; i < D.nbrk;)
        if (!owner || D.brk[i].owner == owner) D.brk[i] = D.brk[--D.nbrk];
        else i++;
    reapply_all(DBG_USER);
}
uint32_t bp_debug_breakpoints(BpDbgBreak* out, uint32_t cap) {
    for (uint32_t i = 0; i < D.nbrk && i < cap; i++)
        out[i] = (BpDbgBreak){ D.brk[i].owner, D.brk[i].node, D.brk[i].graph, D.brk[i].enabled, D.brk[i].hits };
    return D.nbrk;
}

// --- The debug objects and the traces ---

void bp_debug_set_object(const BpAsset* cls, Entity e) {
    for (uint32_t i = 0; i < D.nobj; i++)
        if (D.obj[i].cls == cls) {
            if (e == ENTITY_NONE) D.obj[i] = D.obj[--D.nobj];
            else D.obj[i].e = e;
            return;
        }
    if (e != ENTITY_NONE && D.nobj < 64) D.obj[D.nobj].cls = cls, D.obj[D.nobj++].e = e;
}
Entity bp_debug_object(const BpAsset* cls) {
    for (uint32_t i = 0; i < D.nobj; i++)
        if (D.obj[i].cls == cls) return D.obj[i].e;
    return ENTITY_NONE;
}
static bool is_of(const BpAsset* c, const BpAsset* cls) { return c == cls || (c && cls && bp_is_child_of(c, cls->name)); }
uint32_t bp_debug_instances(const BpAsset* cls, Entity* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; V.playing && i < V.ninst; i++) {
        const Inst* in = &insts()[i];
        if (!in->alive || !ecs_alive(V.w, in->e) || !is_of(in->cls, cls)) continue;
        if (n < cap) out[n] = in->e;
        n++;
    }
    return n;
}

void bp_debug_watch(const BpAsset* owner, bool on) {
    bool has = dbg_watched(owner);
    if (on == has || (on && (!V.playing || D.nwatched >= 16))) return;
    if (on) D.watched[D.nwatched++] = owner;
    else
        for (uint32_t i = 0; i < D.nwatched; i++)
            if (D.watched[i] == owner) D.watched[i] = D.watched[--D.nwatched];
    reapply_all(DBG_TRACE);
}

float bp_debug_node_age(const BpAsset* owner, uint32_t graph, uint32_t node_id, int ord) {
    Entity obj = bp_debug_object(owner);
    uint32_t n = D.ntrace < DBG_TRACES ? D.ntrace : DBG_TRACES;
    for (uint32_t k = 0; k < n; k++) { // (newest first)
        const DbgTrace* t = &D.trace[(D.ntrace - 1 - k) % DBG_TRACES];
        if (t->owner == owner && t->node_id == node_id && t->graph == graph && (ord < 0 || t->ord == ord) && (!obj || t->actor == obj))
            return (float)(V.time - t->t);
    }
    return -1.0f;
}

// --- The pause ---

// The depth of a run's place: the nested runs (another actor's event, an interface call) count 64, the functions and
// local Custom Events still to return to (the return stack's entries pushed by a call node) one each
static int dbg_depth(const BpAsset* a, const uint32_t* stack, uint32_t sp) {
    int d = (V.depth > 0 ? V.depth - 1 : 0) * 64;
    for (uint32_t i = 0; i < sp && a->op_src; i++) {
        uint32_t r = stack[i];
        uint16_t s = r >= 1 && r - 1 < a->ncode ? a->op_src[r - 1] : 0;
        if (s && (a->dbg_nodes[s - 1].kind == BPK_CALL_FUNCTION || a->dbg_nodes[s - 1].kind == BPK_CALL_CUSTOM)) d++;
    }
    return d;
}
static int node_line(const BpAsset* a, uint16_t node) { return node != UINT16_MAX && node < a->ndbg_nodes ? a->dbg_nodes[node].line : 0; }

static void step_on(void) {
    D.stepping = true;
    reapply_all(DBG_STEP);
}
static void step_off(void) {
    D.stepping = false;
    for (uint32_t i = 0; i < D.npatch; i++)
        if (D.patch[i].a && (D.patch[i].why & DBG_STEP)) patch_clear(D.patch[i].a, D.patch[i].pc, DBG_STEP);
}

// OP_BREAK ran (bp_vm.c run): a trace, a breakpoint or a step at this node's start. Pauses when it should (on the VM's
// fiber: back to the editor until a command), then hands back the op it replaced
static BpOp dbg_hit(uint32_t ii, uint32_t pc, const uint32_t* stack, uint32_t sp) {
    Inst* in = &insts()[ii];
    BpAsset* a = in->cls;
    DbgPatch p = D.patch[a->code[pc].a]; // (a copy: the table may change while paused)
    D.stats.hits++;
    uint16_t node = UINT16_MAX;
    int32_t brk = -1;
    for (uint32_t s = 0; s < a->ndbg_sites; s++) {
        if (a->dbg_sites[s].pc != pc) continue;
        const BpDbgNode* n = &a->dbg_nodes[a->dbg_sites[s].node];
        if (node == UINT16_MAX) node = a->dbg_sites[s].node; // (the first made here: the node about to run)
        if ((p.why & DBG_TRACE) && dbg_watched(n->owner)) {
            D.trace[D.ntrace % DBG_TRACES] = (DbgTrace){ n->owner, n->node_id, in->e, V.time, n->graph, a->dbg_sites[s].ord };
            D.ntrace++;
        }
        for (uint32_t b = 0; (p.why & DBG_USER) && brk < 0 && b < D.nbrk; b++) {
            if (!D.brk[b].enabled || !dbg_on(n, &D.brk[b])) continue;
            Entity obj = bp_debug_object(D.brk[b].owner);
            if (obj && obj != in->e) continue; // (another instance than the one debugged)
            brk = (int32_t)b, node = a->dbg_sites[s].node;
        }
    }
    bool stop = brk >= 0;
    if (!stop && D.stepping && (p.why & DBG_STEP)) {
        int depth = dbg_depth(a, stack, sp);
        stop = D.step_mode == BPD_STEP_INTO || (D.step_mode == BPD_STEP_OVER && depth <= D.step_depth) ||
               (D.step_mode == BPD_STEP_OUT && depth < D.step_depth);
        // (a script's step goes by lines: the same line's other nodes run on)
        if (stop && D.step_line && D.step_mode != BPD_STEP_OUT && a == D.step_cls && node_line(a, node) == D.step_line) stop = false;
    }
    if (!stop) return p.orig;
    if (brk >= 0) D.brk[brk].hits++;
    if (!bpf_in_vm()) { // (a run outside the Play's tick: a widget's click, an online event; it can't wait there)
        D.stats.outside++;
        const BpDbgNode* n = node != UINT16_MAX ? &a->dbg_nodes[node] : NULL;
        const BpNodeDesc* d = n ? bp_node_desc(n->kind) : NULL;
        message("%s: breakpoint at %s ran outside the game's tick: not paused", a->name, d ? d->name : "?");
        return p.orig;
    }
    D.inst = ii, D.pc = pc, D.sp = sp < BP_MAX_CALL ? sp : BP_MAX_CALL, D.depth = (uint32_t)V.depth, D.node = node;
    memcpy(D.stack, stack, D.sp * sizeof(uint32_t));
    if (D.stepping) step_off();
    D.paused = true, g_dbg_block = true;
    D.stats.stops++;
    bpf_suspend(); // the editor goes on; bp_tick switches back here with a command
    D.paused = false, g_dbg_block = false;
    int cmd = D.cmd;
    D.cmd = 0;
    if (cmd == BPD_STEP_OVER || cmd == BPD_STEP_INTO || cmd == BPD_STEP_OUT) {
        D.step_mode = cmd, D.step_depth = dbg_depth(a, stack, sp);
        D.step_line = node_line(a, node), D.step_cls = a;
        D.stats.steps++;
        step_on();
    }
    return p.orig;
}

bool bp_debug_paused(void) { return D.paused; }
void bp_debug_command(int cmd) {
    if (D.paused && cmd >= BPD_CONTINUE && cmd <= BPD_STEP_OUT) D.cmd = cmd;
}
const BpDbgStats* bp_debug_stats(void) { return &D.stats; }
Entity bp_debug_paused_actor(void) { return D.paused && D.inst < V.ninst ? insts()[D.inst].e : ENTITY_NONE; }

// The tick's turn while paused: a command waiting switches back to the paused run (the rest of its tick then goes on)
static void dbg_resume(void) {
    if (!D.cmd) return;
    double t0 = re_time_seconds();
    bpf_resume();
    D.stats.resume_ms = (re_time_seconds() - t0) * 1000.0;
}

void bp_debug_end_play(void) {
    if (bpf_waiting() && !bpf_in_vm()) bpf_abandon(); // (its frames gone with the fiber's stack)
    if (D.paused) V.depth = 0;
    D.paused = false, g_dbg_block = false, D.cmd = 0;
    if (D.stepping) step_off();
    D.nwatched = 0, D.ntrace = 0, D.nobj = 0;
    reapply_all(DBG_TRACE);
}

// --- Values ---

static void dbg_format(const Inst* in, const BpValue* v, uint8_t type, const char* ref, uint8_t cont, char* out, size_t cap, int depth) {
    if (cap) out[0] = 0;
    if (cont) {
        const Arr* q = arr_get(v);
        if (!q) {
            snprintf(out, cap, "[]");
            return;
        }
        size_t n = (size_t)snprintf(out, cap, "[%u]", q->count);
        for (uint32_t i = 0; i < q->count && i < 4 && n + 4 < cap; i++) {
            char e[96];
            dbg_format(in, arr_at(q, i), q->type, ref, 0, e, sizeof e, depth + 1);
            n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : " ", e);
        }
        if (q->count > 4 && n + 4 < cap) snprintf(out + n, cap - n, ", ...");
        return;
    }
    if (type == BPT_STRUCT && depth < 2) {
        const BpAsset* s = bp_find_name(BPA_STRUCT, ref);
        size_t n = (size_t)snprintf(out, cap, "{");
        uint32_t off = 0;
        for (uint32_t f = 0; s && f < s->nfields && n + 8 < cap; f++) {
            const BpVar* fv = &s->fields[f];
            char e[96];
            dbg_format(in, v + off, fv->type, fv->ref, fv->cont, e, sizeof e, depth + 1);
            n += (size_t)snprintf(out + n, cap - n, "%s%s: %s", f ? ", " : "", fv->name, e);
            off += bp_var_width(fv);
        }
        if (n + 2 < cap) snprintf(out + n, cap - n, "}");
        return;
    }
    if (type == BPT_ENUM) {
        const BpAsset* e = bp_find_name(BPA_ENUM, ref);
        uint32_t k = (uint32_t)v->v[0];
        if (e && k < e->nentries) {
            snprintf(out, cap, "%s", e->entries[k]);
            return;
        }
    }
    to_text(in, v, type, out, cap);
}

static const Inst* dbg_inst(Entity actor, const BpAsset* owner) {
    if (!V.playing) return NULL;
    if (actor != ENTITY_NONE) return inst_of(V.w, actor);
    if (D.paused && D.inst < V.ninst && is_of(insts()[D.inst].cls, owner)) return &insts()[D.inst];
    Entity obj = bp_debug_object(owner);
    if (obj) return inst_of(V.w, obj);
    for (uint32_t i = 0; i < V.ninst; i++) // (no debug object: the first instance of the class)
        if (insts()[i].alive && is_of(insts()[i].cls, owner)) return &insts()[i];
    return NULL;
}

bool bp_debug_pin_text(const BpAsset* owner, uint32_t graph, uint32_t node_id, int pin, Entity actor, char* out, size_t cap) {
    const Inst* in = dbg_inst(actor, owner);
    if (!in || !owner || graph >= owner->ngraphs) return false;
    BpGraph* g = (BpGraph*)&owner->graphs[graph];
    const BpNode* n = bp_node(g, node_id);
    for (int hop = 0; n && hop < 8; hop++) { // (an input follows its wire to the output; a Reroute on to its input)
        if (pin < 0 || pin >= n->npins) return false;
        const BpPin* p = &n->pins[pin];
        if (p->type == BPT_EXEC) return false;
        if (!p->out) {
            const BpLink* l = link_to(g, n->id, pin);
            if (!l) { // its typed value
                if (bp_type_stringish(p->type)) snprintf(out, cap, "%s", p->slit);
                else if (p->type == BPT_ACTOR) snprintf(out, cap, "self");
                else to_text(in, &p->lit, p->type, out, cap);
                return true;
            }
            n = bp_node(g, l->from_node), pin = l->from_pin;
            continue;
        }
        if (n->kind == BPK_REROUTE) { // (its value is its input's)
            const BpLink* l = link_to(g, n->id, 0);
            if (!l) return false;
            n = bp_node(g, l->from_node), pin = l->from_pin;
            continue;
        }
        if (p->sub > 1) return false; // (a split pin's other parts)
        const BpAsset* c = in->cls;
        if (n->kind == BPK_VAR_GET || n->kind == BPK_VAR_SET) // (a variable's node: the variable itself, its register)
            for (uint32_t i = 0; i < c->ndbg_vars; i++) {
                const BpVar* v = &c->dbg_vars[i].owner->vars[c->dbg_vars[i].var];
                if (strcmp(v->name, n->text) || c->dbg_vars[i].slot >= c->nslots) continue;
                dbg_format(in, &in->r[c->dbg_vars[i].slot], v->type, v->ref, v->cont, out, cap, 0);
                return true;
            }
        const BpDbgNode* dn = NULL;
        for (uint32_t i = 0; i < c->ndbg_nodes && !dn; i++)
            if (c->dbg_nodes[i].owner == owner && c->dbg_nodes[i].graph == graph && c->dbg_nodes[i].node_id == n->id) dn = &c->dbg_nodes[i];
        if (!dn) return false; // (not compiled: it never runs)
        uint32_t reg = dn->out_reg;
        for (int i = 0; i < pin; i++)
            if (n->pins[i].out && n->pins[i].type != BPT_EXEC && n->pins[i].sub <= 1) reg += whole_width(&n->pins[i]);
        if (reg >= c->nslots) return false;
        uint8_t t = p->sub ? p->ptype : p->type;
        dbg_format(in, &in->r[reg], t, p->sub ? p->pref : p->ref, p->cont, out, cap, 0);
        return true;
    }
    return false;
}

bool bp_debug_var_text(Entity actor, uint32_t i, char* name, size_t ncap, char* out, size_t cap) {
    const Inst* in = actor ? (V.playing ? inst_of(V.w, actor) : NULL) : (D.paused && D.inst < V.ninst ? &insts()[D.inst] : NULL);
    if (!in || i >= in->cls->ndbg_vars) return false;
    const BpDbgVar* dv = &in->cls->dbg_vars[i];
    const BpVar* v = &dv->owner->vars[dv->var];
    snprintf(name, ncap, "%s", v->name);
    if (dv->slot >= in->cls->nslots) return false;
    dbg_format(in, &in->r[dv->slot], v->type, v->ref, v->cont, out, cap, 0);
    return true;
}

// --- The call stack ---

static void frame_of(const Inst* in, uint16_t node, BpDbgFrame* f, const char* prefix) {
    memset(f, 0, sizeof *f);
    f->cls = in->cls, f->actor = in->e;
    const BpDbgNode* n = node < in->cls->ndbg_nodes ? &in->cls->dbg_nodes[node] : NULL;
    if (n) f->owner = n->owner, f->graph = n->graph, f->node_id = n->node_id, f->line = n->line;
    const BpNodeDesc* d = n ? bp_node_desc(n->kind) : NULL;
    const BpNode* bn = n && n->graph < n->owner->ngraphs ? bp_node((BpGraph*)&n->owner->graphs[n->graph], n->node_id) : NULL;
    snprintf(f->what, sizeof f->what, "%s%s: %s%s%s (%s)", prefix, actor_name(in->e), d ? d->name : "?", bn && bn->text[0] ? " " : "",
             bn ? bn->text : "", n && n->graph < n->owner->ngraphs ? n->owner->graphs[n->graph].name : "?");
}

uint32_t bp_debug_stack(BpDbgFrame* out, uint32_t cap) {
    if (!D.paused || D.inst >= V.ninst) return 0;
    uint32_t n = 0;
    const Inst* in = &insts()[D.inst];
    const BpAsset* a = in->cls;
    if (n < cap) frame_of(in, D.node, &out[n++], "");
    for (uint32_t i = D.sp; i-- > 0 && n < cap;) { // the calls still to return to in this run (functions, local Custom Events)
        uint32_t r = D.stack[i];
        uint16_t s = a->op_src && r >= 1 && r - 1 < a->ncode ? a->op_src[r - 1] : 0;
        if (s && (a->dbg_nodes[s - 1].kind == BPK_CALL_FUNCTION || a->dbg_nodes[s - 1].kind == BPK_CALL_CUSTOM)) frame_of(in, (uint16_t)(s - 1), &out[n++], "");
    }
    for (int d = (int)D.depth - 2; d >= 0 && n < cap; d--) { // the runs that called this one (another actor's event, an interface)
        if (V.fr[d].inst >= V.ninst) continue;
        const Inst* oi = &insts()[V.fr[d].inst];
        const BpOp* op = V.fr[d].op;
        uint16_t s = op && op >= oi->cls->code && op < oi->cls->code + oi->cls->ncode && oi->cls->op_src ? oi->cls->op_src[op - oi->cls->code] : 0;
        frame_of(oi, s ? (uint16_t)(s - 1) : UINT16_MAX, &out[n++], "");
    }
    return n;
}

// --- Scripts: lines ---

uint32_t bp_debug_toggle_line(BpAsset* cls, int line) {
    if (!cls || line <= 0) return 0;
    if (!cls->compiled) bp_compile(cls);
    uint32_t had = 0;
    for (uint32_t i = 0; i < D.nbrk;) // the line's breakpoints off when it has any
        if (D.brk[i].owner == cls && D.brk[i].graph < cls->ngraphs) {
            const BpNode* bn = bp_node(&cls->graphs[D.brk[i].graph], D.brk[i].node);
            if (bn && bn->comment[0] == 'L' && atoi(bn->comment + 1) == line) {
                D.brk[i] = D.brk[--D.nbrk], had++;
                continue;
            }
            i++;
        } else
            i++;
    if (had) {
        reapply_all(DBG_USER);
        return 0;
    }
    // on: the line's first node to run (its lowest site), as if it were the line's start
    uint32_t best = UINT32_MAX;
    const BpDbgNode* bn = NULL;
    for (uint32_t s = 0; s < cls->ndbg_sites; s++) {
        const BpDbgNode* n = &cls->dbg_nodes[cls->dbg_sites[s].node];
        if (n->owner == cls && n->line == line && cls->dbg_sites[s].pc < best) best = cls->dbg_sites[s].pc, bn = n;
    }
    if (!bn || D.nbrk >= DBG_MAX_BREAKS) return 0;
    D.brk[D.nbrk++] = (DbgBreak){ cls, bn->node_id, bn->graph, true, 0 };
    reapply_all(DBG_USER);
    return 1;
}
bool bp_debug_line_has(const BpAsset* cls, int line) {
    for (uint32_t i = 0; cls && i < D.nbrk; i++)
        if (D.brk[i].owner == cls && D.brk[i].graph < cls->ngraphs) {
            const BpNode* bn = bp_node((BpGraph*)&cls->graphs[D.brk[i].graph], D.brk[i].node);
            if (bn && bn->comment[0] == 'L' && atoi(bn->comment + 1) == line) return true;
        }
    return false;
}
int bp_debug_paused_line(const BpAsset* cls) {
    if (!D.paused || D.inst >= V.ninst) return 0;
    const BpAsset* a = insts()[D.inst].cls;
    return D.node < a->ndbg_nodes && a->dbg_nodes[D.node].owner == cls ? a->dbg_nodes[D.node].line : 0;
}

// --- The Play's entries on the VM's fiber while debugging (a breakpoint can leave them half done) ---

typedef struct DbgCall { int kind; World* w; float dt; int key; bool pressed, has_mouse, down; float mouse[2]; } DbgCall;
static void dbg_call(void* p) {
    DbgCall c = *(const DbgCall*)p; // (copied first: the caller's frame goes on when this one waits)
    switch (c.kind) {
    case 0: tick_body(c.w, c.dt); break;
    case 1: begin_play_body(c.w); break;
    case 2: key_event_body(c.w, c.key, c.pressed); break;
    default: touch_update_body(c.w, c.has_mouse ? c.mouse : NULL, c.down); break;
    }
}
// (2026-11-16) A Timeline playing: its time on, its tracks into the node's outputs, Update; the Event tracks' keys it
// passed; Finished at an end (Loop: around). No allocation, a binary search per track.
static void tl_events(uint32_t ii, const struct BpTlCode* tc, float from, float to) {
    float lo = fminf(from, to), hi = fmaxf(from, to);
    for (uint8_t j = 0; j < tc->tl->ntracks && j < 6; j++) {
        const BpTlTrack* tr = &tc->tl->tracks[j];
        if (tr->type != TLT_EVENT || tc->pc_event[j] < 0) continue;
        for (uint8_t k = 0; k < tr->nkeys; k++)
            if (tr->keys[k].t > lo && tr->keys[k].t <= hi) run(ii, (uint32_t)tc->pc_event[j]);
    }
}
static void tl_tick(uint32_t ii, uint32_t k, float dt) {
    Inst* in = &insts()[ii];
    struct TlState* st = &in->tls[k];
    const struct BpTlCode* tc = &in->cls->tlc[k];
    if (!st->playing || !tc->tl) return;
    const BpTimeline* tl = tc->tl;
    float len = bp_timeline_length(tl), prev = st->t;
    st->t += dt * (tl->rate > 0.0f ? tl->rate : 1.0f) * st->dir;
    bool fin = false;
    if (st->dir > 0.0f && st->t >= len) {
        if (tl->loop && len > 0.0f) tl_events(ii, tc, prev, len), prev = 0.0f, st->t = fmodf(st->t - len, len);
        else st->t = len, st->playing = 0, fin = true;
    } else if (st->dir < 0.0f && st->t <= 0.0f) {
        if (tl->loop && len > 0.0f) tl_events(ii, tc, prev, 0.0f), prev = len, st->t = len + fmodf(st->t, len);
        else st->t = 0.0f, st->playing = 0, fin = true;
    }
    in = &insts()[ii]; // (a run may have grown the instances)
    if (tc->reg_dir) in->r[tc->reg_dir] = (BpValue){ { st->dir > 0.0f ? 0.0f : 1.0f } }; // (Forward 0, Backward 1)
    for (uint8_t j = 0; j < tl->ntracks && j < 6; j++)
        if (tl->tracks[j].type != TLT_EVENT && tc->reg_track[j]) bp_timeline_eval(&tl->tracks[j], st->t, in->r[tc->reg_track[j]].v);
    if (tc->pc_update >= 0) run(ii, (uint32_t)tc->pc_update);
    tl_events(ii, tc, prev, st->t);
    if (fin && tc->pc_finished >= 0) run(ii, (uint32_t)tc->pc_finished);
}

void bp_tick(World* w, float dt) {
    if (D.paused) { // the world waits; a command goes on with the paused run (and the rest of its tick)
        dbg_resume();
        return;
    }
    if (D.nactive && !bpf_in_vm()) {
        bpf_run(dbg_call, &(DbgCall){ .kind = 0, .w = w, .dt = dt });
        return;
    }
    tick_body(w, dt);
}
void bp_begin_play(World* w) {
    fpcam_begin_play(); // (Q12: no state or shake from a play before)
    if (D.nactive && !bpf_in_vm() && !D.paused) {
        bpf_run(dbg_call, &(DbgCall){ .kind = 1, .w = w });
        return;
    }
    begin_play_body(w);
}
void bp_key_event(World* w, int key, bool pressed) {
    if (D.nactive && !bpf_in_vm() && !D.paused) {
        bpf_run(dbg_call, &(DbgCall){ .kind = 2, .w = w, .key = key, .pressed = pressed });
        return;
    }
    key_event_body(w, key, pressed); // (paused: the keys' state kept, the events don't run)
}
void bp_touch_update(World* w, const float mouse[2], bool mouse_down) {
    if (D.nactive && !bpf_in_vm() && !D.paused) {
        DbgCall c = { .kind = 3, .w = w, .down = mouse_down };
        if (mouse) c.mouse[0] = mouse[0], c.mouse[1] = mouse[1], c.has_mouse = true;
        bpf_run(dbg_call, &c);
        return;
    }
    touch_update_body(w, mouse, mouse_down);
}

// --- (Q13) the horror kit (terror.h): the Tension Director, the filters, the letterbox, the dips, the rigs, the lights ---
// an actor and its components (a Blueprint's mesh parts, a character's skeletal parts): their lights' or meshes' Lighting Channels
static void set_channels(World* w, Entity e, uint32_t mask) {
    if (!e) return;
    CLight* l = ecs_get(w, e, COMP_LIGHT);
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    if (l) l->light_channels = mask;
    if (m) m->light_channels = mask;
    if (l || m) ecs_touch(w, e);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & (COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_STATIC_MESH)))) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* es = ecs_arch_entities(ar);
        for (uint32_t i = 0; i < ar->count; i++)
            if (at[i].owner == e) {
                CLight* cl = (ar->mask & COMP_BIT(COMP_LIGHT)) ? ecs_get(w, es[i], COMP_LIGHT) : NULL;
                CStaticMesh* cm = (ar->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_get(w, es[i], COMP_STATIC_MESH) : NULL;
                if (cl) cl->light_channels = mask;
                if (cm) cm->light_channels = mask;
                ecs_touch(w, es[i]);
            }
    }
}
// the actor's light (itself, or its first light component)
static CLight* light_of(World* w, Entity e, Entity* found) {
    CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
    if (l) {
        if (found) *found = e;
        return l;
    }
    for (uint32_t ai = 0; e && ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_LIGHT))) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        for (uint32_t i = 0; i < ar->count; i++)
            if (at[i].owner == e) {
                if (found) *found = ecs_arch_entities(ar)[i];
                return &((CLight*)ecs_arch_column(ar, COMP_LIGHT))[i];
            }
    }
    return NULL;
}
static bool terror_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    BpValue res = { { 0 } };
    switch (kind) {
    case BPK_SET_TENSION: terror_set_tension(A->v[0], B->v[0]); return true;
    case BPK_ADD_TENSION: terror_add_tension(A->v[0], B->v[0]); return true;
    case BPK_GET_TENSION: res.v[0] = terror_tension(); break;
    case BPK_TRIGGER_FILTER: {
        int f = terror_filter_by_name(ENUM_NAME("EHorrorFilter", A));
        if (f >= 0) terror_filter_trigger(f, B->v[0], C->v[0], V.np[0].v[0], V.np[1].v[0]);
        else printf("[RealEngine] Trigger Horror Filter: no filter %d\n", (int)A->v[0]);
        res.v[0] = f >= 0 ? 1.0f : 0.0f;
        break;
    }
    case BPK_SET_FILTER: {
        int f = terror_filter_by_name(ENUM_NAME("EHorrorFilter", A));
        if (f >= 0) terror_filter_set(f, B->v[0], C->v[0]);
        res.v[0] = f >= 0 ? 1.0f : 0.0f;
        break;
    }
    case BPK_CLEAR_FILTERS: terror_filter_clear(A->v[0]); return true;
    case BPK_GET_FILTER: res.v[0] = terror_filter_value(terror_filter_by_name(ENUM_NAME("EHorrorFilter", A))); break;
    case BPK_SET_LETTERBOX: terror_set_letterbox(A->v[0], B->v[0]); return true;
    case BPK_DIP_BLACK: case BPK_DIP_WHITE: {
        const float c[3] = { kind == BPK_DIP_WHITE ? 1.0f : 0.0f, kind == BPK_DIP_WHITE ? 1.0f : 0.0f, kind == BPK_DIP_WHITE ? 1.0f : 0.0f };
        terror_dip(c, A->v[0], B->v[0], C->v[0]);
        return true;
    }
    case BPK_SCARE_RIG: {
        int r = terror_rig_by_name(ENUM_NAME("EScareRig", A));
        if (r >= 0) terror_scare_rig(r, B->v[0], C->v[0]);
        res.v[0] = r >= 0 ? 1.0f : 0.0f;
        break;
    }
    case BPK_SET_LIGHT_FLICKER: {
        Entity le = ENTITY_NONE;
        CLight* l = light_of(w, ent_of(A) ? ent_of(A) : in->e, &le);
        const char* pn = ENUM_NAME("ELightFlicker", B);
        int p = -1;
        for (int k = 0; k < FLICKER_COUNT; k++)
            if (!_stricmp(pn, light_flicker_name(k))) p = k;
        if (p < 0 && (!_stricmp(pn, "Faulty") || !_stricmp(pn, "Bulb"))) p = FLICKER_FAULTY;
        if (l && p >= 0) {
            l->flicker = (uint32_t)p, l->flicker_amount = fminf(fmaxf(C->v[0], 0.0f), 1.0f), l->flicker_speed = fmaxf(V.np[0].v[0], 0.0f);
            if (l->flicker_amount <= 0.0f && p) l->flicker_amount = 1e-3f; // (0 reads as 1: a tiny amount stays a tiny amount)
            ecs_touch(w, le);
        }
        res.v[0] = l && p >= 0 ? 1.0f : 0.0f;
        break;
    }
    case BPK_SET_LIGHT_CHANNELS: {
        uint32_t m = (B->v[0] != 0.0f ? 1u : 0u) | (C->v[0] != 0.0f ? 2u : 0u) | (V.np[0].v[0] != 0.0f ? 4u : 0u);
        set_channels(w, ent_of(A) ? ent_of(A) : in->e, m ? m : 1u);
        return true;
    }
    case BPK_SET_RIM_TARGET: {
        Entity le = ENTITY_NONE;
        CLight* l = light_of(w, ent_of(A) ? ent_of(A) : in->e, &le);
        if (!l) return true;
        Entity t = ent_of(B);
        const CName* tn = t ? ecs_get(w, t, COMP_NAME) : NULL;
        if (C->v[0] == 0.0f) l->rim = 0;
        else if (tn) l->rim = RIM_ACTOR, snprintf(l->rim_target, sizeof l->rim_target, "%s", tn->text);
        else l->rim = RIM_PLAYER;
        l->rim_distance = V.np[0].v[0], l->rim_height = V.np[1].v[0];
        ecs_touch(w, le);
        return true;
    }
    default: return false;
    }
    *D = res;
    return true;
}

// (Q14) splines and cables (spline.h): Target a Spline / Rail / Cable actor, none: self
static bool spline_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    BpValue res = { { 0 } };
    Entity e = ent_of(A) ? ent_of(A) : in->e;
    CRail* r = e ? ecs_get(w, e, COMP_SPLINE) : NULL;
    switch (kind) {
    case BPK_SPLINE_LENGTH: case BPK_SPLINE_LOC_AT_DIST: case BPK_SPLINE_DIR_AT_DIST: case BPK_SPLINE_ROT_AT_DIST: case BPK_SPLINE_XFORM_AT_DIST:
    case BPK_SPLINE_LOC_AT_KEY: case BPK_SPLINE_DIST_AT_KEY: case BPK_SPLINE_CLOSEST_KEY: case BPK_SPLINE_CLOSEST_LOC: case BPK_SPLINE_POINT_LOC: {
        if (!r) break;
        bool world = kind == BPK_SPLINE_LENGTH || kind == BPK_SPLINE_DIST_AT_KEY || kind == BPK_SPLINE_CLOSEST_KEY || kind == BPK_SPLINE_CLOSEST_LOC ||
                     C->v[0] != 0.0f; // (the others' World Space pin is their third)
        static SplineCurve sc; // (the VM's thread; a loop of these allocates nothing)
        SplineCurve* c = &sc;
        if (world || spline_kind(r) == SPLINE_KIND_CABLE) spline_world_curve(w, e, c);
        else spline_curve_from(c, r, NULL);
        if (kind == BPK_SPLINE_LENGTH) res.v[0] = c->length;
        else if (kind == BPK_SPLINE_DIST_AT_KEY) res.v[0] = spline_distance_at_key(c, B->v[0]);
        else if (kind == BPK_SPLINE_CLOSEST_KEY) res.v[0] = spline_closest_key(c, B->v);
        else if (kind == BPK_SPLINE_CLOSEST_LOC) spline_eval_key(c, spline_closest_key(c, B->v), res.v, NULL);
        else if (kind == BPK_SPLINE_LOC_AT_KEY) spline_eval_key(c, B->v[0], res.v, NULL);
        else if (kind == BPK_SPLINE_POINT_LOC) {
            int i = (int)B->v[0];
            if (i >= 0 && (uint32_t)i < c->n) memcpy(res.v, c->p[i], 12);
        } else {
            float key = spline_key_at_distance(c, B->v[0]), ax[3][3], sc[2];
            spline_frame_at_key(c, key, res.v, ax[0], ax[1], ax[2], sc);
            if (kind == BPK_SPLINE_DIR_AT_DIST) memcpy(res.v, ax[0], 12);
            else if (kind == BPK_SPLINE_ROT_AT_DIST) axes_rotator(ax, res.v);
            else if (kind == BPK_SPLINE_XFORM_AT_DIST) {
                axes_rotator(ax, D[1].v);
                D[2].v[0] = 1.0f, D[2].v[1] = sc[0], D[2].v[2] = sc[1];
            }
        }
        break;
    }
    case BPK_SPLINE_NUM_POINTS: {
        if (!r) break;
        if (spline_kind(r) == SPLINE_KIND_CABLE) {
            float pts[CABLE_MAX_SEGMENTS + 1][3];
            res.v[0] = (float)cable_points(w, e, pts, CABLE_MAX_SEGMENTS + 1);
        } else res.v[0] = (float)r->npoints;
        break;
    }
    case BPK_SPLINE_ADD_POINT: res.v[0] = spline_bp_add_point(w, e, B->v, C->v[0] != 0.0f) ? 1.0f : 0.0f; break;
    case BPK_SPLINE_SET_POINT: res.v[0] = spline_bp_set_point(w, e, (int)B->v[0], C->v, V.np[0].v[0] != 0.0f) ? 1.0f : 0.0f; break;
    case BPK_SPLINE_REMOVE_POINT: res.v[0] = spline_bp_remove_point(w, e, (int)B->v[0]) ? 1.0f : 0.0f; break;
    case BPK_SPLINE_CLEAR: spline_bp_clear(w, e); return true;
    case BPK_SPLINE_SET_CLOSED: spline_bp_set_closed(w, e, B->v[0] != 0.0f); return true;
    case BPK_SPLINE_SET_TYPE: {
        const char* tn = ENUM_NAME("ESplinePointType", C);
        for (uint32_t t = 0; r && t < SPLINE_POINT_COUNT; t++)
            if (!_stricmp(tn, g_spline_point_names[t]) || (t == SPLINE_POINT_CUSTOM && !_stricmp(tn, "Custom"))) {
                spline_set_point_type(r, (int)B->v[0], t);
                ecs_touch(w, e);
            }
        return true;
    }
    case BPK_CABLE_ATTACH_END: case BPK_CABLE_ATTACH_START:
        res.v[0] = cable_attach(w, e, kind == BPK_CABLE_ATTACH_END, ent_of(B), str_of(in, C)) ? 1.0f : 0.0f;
        break;
    case BPK_CABLE_DETACH: cable_detach(w, e, B->v[0] != 0.0f); return true;
    case BPK_CABLE_SET_LENGTH: cable_set_length(w, e, B->v[0]); return true;
    case BPK_CABLE_GET_LENGTH: res.v[0] = r ? r->cable_length : 0.0f; break;
    case BPK_CABLE_IMPULSE: cable_add_impulse(w, e, B->v, C->v, V.np[0].v[0]); return true;
    default: return false;
    }
    D[0] = res;
    return true;
}

// (Q15) PCG (pcg.h): the PCG component of an actor (Target empty: this one)
static bool pcg_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    BpValue res = { { 0 } };
    Entity e = ent_of(A) ? ent_of(A) : in->e;
    switch (kind) {
    case BPK_PCG_GENERATE: res.v[0] = pcg_generate(w, e) ? 1.0f : 0.0f; break;
    case BPK_PCG_CLEANUP: res.v[0] = pcg_cleanup(w, e) ? 1.0f : 0.0f; break;
    case BPK_PCG_SET_SEED: res.v[0] = pcg_set_seed(w, e, (uint32_t)(int32_t)B->v[0]) ? 1.0f : 0.0f; break;
    case BPK_PCG_SET_PARAM: res.v[0] = pcg_set_parameter(w, e, str_of(in, B), C->v[0]) ? 1.0f : 0.0f; break;
    case BPK_PCG_IS_GENERATED:
        res.v[0] = pcg_is_generated(w, e) ? 1.0f : 0.0f;
        D[1].v[0] = (float)pcg_generated_count(w, e);
        break;
    default: return false;
    }
    *D = res;
    return true;
}

// --- (Q16) everything editable in the game: Build Lighting while playing (runtime_lighting.h), the Details' properties by
// name (props.h), the console variables (cvars.h) ---
static bool name_is(const char* s, const char* a, const char* b) { // a name pin's choice: its words or its number
    return s && (!_stricmp(s, a) || !_stricmp(s, b));
}
static bool ingame_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    BpValue res = { { 0 } };
    switch (kind) {
    case BPK_BUILD_LIGHTING: { // Mode, Quality, Budget ms -> Started
        const char* m = ENUM_NAME("ELightingBuildMode", A);
        RtlMode mode = name_is(m, "Loading Screen", "0") || name_is(m, "LoadingScreen", "Blocking") ? RTL_LOADING_SCREEN : RTL_PROGRESSIVE;
        int q = name_is(ENUM_NAME("ELightingQuality", B), "Production", "1") ? 1 : 0;
        res.v[0] = rtl_request(q, mode, C->v[0]) ? 1.0f : 0.0f;
        break;
    }
    case BPK_LIGHTING_PROGRESS:
        res.v[0] = rtl_progress();
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = rtl_busy() ? 1.0f : 0.0f;
        memset(&D[2], 0, sizeof D[2]), D[2].v[0] = rtl_built() ? 1.0f : 0.0f;
        break;
    case BPK_CANCEL_LIGHTING: rtl_cancel(); return true;
    case BPK_SAVE_LIGHTING: res.v[0] = rtl_save(str_of(in, A)) ? 1.0f : 0.0f; break;
    case BPK_LOAD_LIGHTING: res.v[0] = rtl_load(str_of(in, A)) ? 1.0f : 0.0f; break;
    case BPK_SET_PROPERTY: // Target, Property, Value, Vector (np[0]): a number or a checkbox from Value, a vector or a color from Vector
        res.v[0] = prop_set(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), C->v[0], V.np[0].v) ? 1.0f : 0.0f;
        break;
    case BPK_GET_PROPERTY: { // Value, Vector, Found
        float v[4];
        bool ok = prop_get(w, ent_of(A) ? ent_of(A) : in->e, str_of(in, B), v);
        res.v[0] = v[0];
        memset(&D[1], 0, sizeof D[1]), memcpy(D[1].v, v, sizeof(float) * 3);
        memset(&D[2], 0, sizeof D[2]), D[2].v[0] = ok ? 1.0f : 0.0f;
        break;
    }
    case BPK_SET_CVAR: res.v[0] = cvar_set(str_of(in, A), B->v[0], str_of(in, C)) ? 1.0f : 0.0f; break;
    case BPK_GET_CVAR: {
        char t[96] = "";
        float v = 0.0f;
        bool ok = cvar_get(str_of(in, A), &v, t, sizeof t);
        res.v[0] = v;
        set_str(&D[1], t);
        memset(&D[2], 0, sizeof D[2]), D[2].v[0] = ok ? 1.0f : 0.0f;
        break;
    }
    case BPK_EXEC_CONSOLE: {
        char t[160] = "";
        res.v[0] = cvar_exec(str_of(in, A), t, sizeof t) ? 1.0f : 0.0f;
        set_str(&D[1], t);
        break;
    }
    case BPK_LIST_PROPERTIES: { // every property the actor (and its components) has, "Section.Label"
        Entity e = ent_of(A) ? ent_of(A) : in->e;
        Arr* out = arr_get(D);
        uint32_t n = 0;
        if (out) out->count = 0;
        uint64_t mask = ecs_alive(w, e) ? ecs_mask(w, e) : 0;
        for (int i = 0; i < prop_count(); i++) {
            const PropInfo* p = prop_at(i);
            if (!(mask & COMP_BIT(p->comp))) continue;
            char t[160];
            snprintf(t, sizeof t, "%s.%s", p->section[0] ? p->section : g_components[p->comp].name, p->label);
            BpValue v;
            set_str(&v, t);
            if (out) arr_push(in, out, &v);
            n++;
        }
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = (float)n;
        return true;
    }
    default: return false;
    }
    *D = res;
    return true;
}

// (Q25b) mixed reality's nodes (xr_mr.h: they write Project Settings > VR > Mixed Reality, read every frame)
// --- (2026-11-19) The traces and the debug drawing (scene_query.h, debug_draw.h; notas-dev/traces.md) ---
static struct {
    uint32_t color, hit_color; // (Set Debug Trace Colors; 0: Unreal's red and green)
    SqQuery* q;
    SqHit* h;
    uint32_t cap;
    struct { uint32_t seq, handle; } ring[256]; // (the async batches: a small number for the graph, the queries' handle)
    uint32_t seq;
} TQ;
static uint8_t trace_objects(int channel) {
    switch (channel) {
    case 2: return SQ_OBJ_STATIC;
    case 3: return SQ_OBJ_DYNAMIC;
    case 4: return SQ_OBJ_PAWN;
    case 5: return SQ_OBJ_LANDSCAPE;
    default: return SQ_OBJ_ALL; // (Visibility, Camera: everything that collides)
    }
}
// Draw Debug Type -> debug_draw's seconds (0 one frame, > 0 that long, < 0 kept); false: none
static bool trace_draw_seconds(int type, float draw_time, float* s) {
    if (type == DD_FOR_ONE_FRAME) return *s = 0.0f, true;
    if (type == DD_FOR_DURATION) return *s = draw_time > 0.0f ? draw_time : 5.0f, true;
    if (type == DD_PERSISTENT) return *s = -1.0f, true;
    return false;
}
static uint32_t trace_rgba(bool hit) {
    if (!TQ.color) TQ.color = dd_rgba(1, 0, 0, 1), TQ.hit_color = dd_rgba(0, 1, 0, 1);
    return hit ? TQ.hit_color : TQ.color;
}
// self and Actors to Ignore (64 at most)
static uint16_t trace_ignore(Inst* in, const BpValue* arr, Entity* out) {
    uint16_t n = 0;
    if (in->e) out[n++] = in->e;
    const Arr* a = arr_get(arr);
    for (uint32_t i = 0; a && i < a->count && n < 64; i++) out[n++] = ent_of(arr_at(a, i));
    return n;
}
static Arr* trace_out_arr(BpValue* v) {
    Arr* a = arr_get(v);
    if (a) a->count = 0;
    return a;
}
static bool trace_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    const BpValue* np = V.np;
    switch (kind) {
    case BPK_DRAW_DEBUG_LINE: dd_line(A->v, B->v, dd_rgba(C->v[0], C->v[1], C->v[2], C->v[3]), np[1].v[0], np[0].v[0]); return true;
    case BPK_DRAW_DEBUG_POINT: dd_point(A->v, dd_rgba(C->v[0], C->v[1], C->v[2], C->v[3]), B->v[0], np[0].v[0]); return true;
    case BPK_DRAW_DEBUG_BOX: {
        float mn[3] = { A->v[0] - B->v[0], A->v[1] - B->v[1], A->v[2] - B->v[2] }, mx[3] = { A->v[0] + B->v[0], A->v[1] + B->v[1], A->v[2] + B->v[2] };
        dd_box(mn, mx, dd_rgba(C->v[0], C->v[1], C->v[2], C->v[3]), np[1].v[0], np[0].v[0]);
        return true;
    }
    case BPK_DRAW_DEBUG_SPHERE: dd_sphere(A->v, B->v[0], dd_rgba(C->v[0], C->v[1], C->v[2], C->v[3]), np[1].v[0], np[0].v[0]); return true;
    case BPK_DRAW_DEBUG_CAPSULE: dd_capsule(A->v, B->v[0], C->v[0], dd_rgba(np[0].v[0], np[0].v[1], np[0].v[2], np[0].v[3]), np[2].v[0], np[1].v[0]); return true;
    case BPK_FLUSH_DEBUG_LINES: dd_flush_persistent(); return true;
    case BPK_SET_TRACE_COLORS:
        TQ.color = dd_rgba(A->v[0], A->v[1], A->v[2], A->v[3]), TQ.hit_color = dd_rgba(B->v[0], B->v[1], B->v[2], B->v[3]);
        return true;
    case BPK_GET_ASYNC_TRACE: { // Ready, Hit Count, Hits, Locations, Normals, Hit Actors
        uint32_t seq = (uint32_t)A->v[0], handle = 0, n = 0, nh = 0;
        if (seq && TQ.ring[seq & 255u].seq == seq) handle = TQ.ring[seq & 255u].handle;
        const SqHit* hs = NULL;
        bool ready = handle && sq_async_result(handle, &hs, &n);
        Arr *hits = trace_out_arr(&D[2]), *locs = trace_out_arr(&D[3]), *nrms = trace_out_arr(&D[4]), *acts = trace_out_arr(&D[5]);
        if (ready && (!hits || arr_reserve(hits, n)) && (!locs || arr_reserve(locs, n)) && (!nrms || arr_reserve(nrms, n)) && (!acts || arr_reserve(acts, n))) {
            for (uint32_t i = 0; i < n; i++) {
                const SqHit* h = &hs[i];
                nh += h->blocking;
                if (hits) memset(arr_at(hits, i), 0, sizeof(BpValue)), arr_at(hits, i)->v[0] = h->blocking ? 1.0f : 0.0f;
                if (locs) memset(arr_at(locs, i), 0, sizeof(BpValue)), memcpy(arr_at(locs, i)->v, h->location, 12);
                if (nrms) memset(arr_at(nrms, i), 0, sizeof(BpValue)), memcpy(arr_at(nrms, i)->v, h->normal, 12);
                if (acts) set_ent(arr_at(acts, i), h->actor);
            }
            if (hits) hits->count = n;
            if (locs) locs->count = n;
            if (nrms) nrms->count = n;
            if (acts) acts->count = n;
        }
        memset(&D[0], 0, sizeof D[0] * 2), D[0].v[0] = ready ? 1.0f : 0.0f, D[1].v[0] = (float)nh;
        return true;
    }
    default: break;
    }
    // the traces: Start, End, the shape's sizes, then Trace Channel, Actors to Ignore, Draw Debug Type, Draw Time
    bool multi = kind >= BPK_MULTI_LINE_TRACE && kind <= BPK_MULTI_CAPSULE_TRACE;
    bool async = kind == BPK_ASYNC_LINE_TRACE_BATCH || kind == BPK_ASYNC_SPHERE_TRACE_BATCH;
    bool batch = kind == BPK_LINE_TRACE_BATCH || kind == BPK_SPHERE_TRACE_BATCH || async;
    int sk = batch ? (kind == BPK_LINE_TRACE_BATCH || kind == BPK_ASYNC_LINE_TRACE_BATCH ? 0 : 1) : (int)((kind - (multi ? BPK_MULTI_LINE_TRACE : BPK_LINE_TRACE)));
    static const uint8_t shapes[4] = { SQ_LINE, SQ_SPHERE, SQ_BOX, SQ_CAPSULE };
    uint8_t shape = shapes[sk];
    // the inputs after Start and End: C, np[0..]
    const BpValue* rest[7] = { C, &np[0], &np[1], &np[2], &np[3], &np[4], &np[5] };
    int at = 0;
    SqQuery q;
    memset(&q, 0, sizeof q);
    q.shape = shape;
    if (shape == SQ_SPHERE) q.radius = rest[at++]->v[0];
    else if (shape == SQ_BOX) memcpy(q.half, rest[at++]->v, 12);
    else if (shape == SQ_CAPSULE) q.radius = rest[at]->v[0], q.half[2] = rest[at + 1]->v[0], at += 2;
    int channel = (int)rest[at]->v[0], dtype = (int)rest[at + 3]->v[0];
    float dtime = rest[at + 4]->v[0], secs = 0.0f;
    q.cplx = rest[at + 1]->v[0] != 0.0f ? 1u : 0u;
    static Entity ign[64];
    q.nignore = trace_ignore(in, rest[at + 2], ign), q.ignore = ign, q.objects = trace_objects(channel);
    bool draw = trace_draw_seconds(dtype, dtime, &secs) && dd_enabled();
    if (async) { // the queries straight into the batch's own room; its handle (a small number) out
        const Arr* sa = arr_get(A);
        const Arr* ea = arr_get(B);
        uint32_t n = sa && ea ? (sa->count < ea->count ? sa->count : ea->count) : 0, handle = 0;
        static Entity aign[64][64]; // (each batch's ignore list lives until it ran: 64 a frame, as its slots)
        static uint32_t aslot;
        Entity* ig = aign[aslot++ & 63u];
        memcpy(ig, ign, sizeof(Entity) * q.nignore), q.ignore = ig;
        SqQuery* dst = n ? sq_async_begin(w, n, &handle) : NULL;
        memset(&D[0], 0, sizeof D[0]);
        if (!dst) return true;
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = q;
            memcpy(dst[i].start, arr_at(sa, i)->v, 12), memcpy(dst[i].end, arr_at(ea, i)->v, 12);
        }
        sq_async_kick(handle, draw ? dd_rgb565(trace_rgba(false)) << 16 | dd_rgb565(trace_rgba(true)) : 0u);
        uint32_t seq = (++TQ.seq & 0x7FFFFFu) ? (TQ.seq & 0x7FFFFFu) : (++TQ.seq & 0x7FFFFFu); // (exact in a float)
        TQ.ring[seq & 255u].seq = seq, TQ.ring[seq & 255u].handle = handle;
        D[0].v[0] = (float)seq;
        return true;
    }
    if (batch) { // Starts, Ends: arrays in A and B
        const Arr* sa = arr_get(A);
        const Arr* ea = arr_get(B);
        uint32_t n = sa && ea ? (sa->count < ea->count ? sa->count : ea->count) : 0;
        if (n > TQ.cap) { // (the room: grown once when a bigger batch comes, never each frame)
            uint32_t c = TQ.cap ? TQ.cap : 1024;
            while (c < n) c *= 2;
            SqQuery* nq = realloc(TQ.q, sizeof(SqQuery) * c);
            if (nq) TQ.q = nq;
            SqHit* nh = nq ? realloc(TQ.h, sizeof(SqHit) * c) : NULL;
            if (nh) TQ.h = nh, TQ.cap = c;
            else n = TQ.cap;
        }
        for (uint32_t i = 0; i < n; i++) {
            TQ.q[i] = q;
            memcpy(TQ.q[i].start, arr_at(sa, i)->v, 12), memcpy(TQ.q[i].end, arr_at(ea, i)->v, 12);
        }
        if (draw && secs == 0.0f) // (one frame: the workers write the records the GPU draws)
            sq_trace_batch_drawn(w, TQ.q, n, TQ.h, dd_rgb565(trace_rgba(false)) << 16 | dd_rgb565(trace_rgba(true)));
        else {
            sq_trace_batch(w, TQ.q, n, TQ.h);
            if (draw) dd_traces(TQ.q, TQ.h, n, trace_rgba(false), trace_rgba(true), secs);
        }
        Arr *hits = trace_out_arr(&D[1]), *locs = trace_out_arr(&D[2]), *nrms = trace_out_arr(&D[3]), *acts = trace_out_arr(&D[4]);
        uint32_t nh = 0;
        if ((!hits || arr_reserve(hits, n)) && (!locs || arr_reserve(locs, n)) && (!nrms || arr_reserve(nrms, n)) && (!acts || arr_reserve(acts, n)))
            for (uint32_t i = 0; i < n; i++) { // (written in place: the arrays' room reserved above)
                const SqHit* h = &TQ.h[i];
                nh += h->blocking;
                if (hits) memset(arr_at(hits, i), 0, sizeof(BpValue)), arr_at(hits, i)->v[0] = h->blocking ? 1.0f : 0.0f;
                if (locs) memset(arr_at(locs, i), 0, sizeof(BpValue)), memcpy(arr_at(locs, i)->v, h->location, 12);
                if (nrms) memset(arr_at(nrms, i), 0, sizeof(BpValue)), memcpy(arr_at(nrms, i)->v, h->normal, 12);
                if (acts) set_ent(arr_at(acts, i), h->actor);
            }
        if (hits) hits->count = n;
        if (locs) locs->count = n;
        if (nrms) nrms->count = n;
        if (acts) acts->count = n;
        memset(&D[0], 0, sizeof D[0]), D[0].v[0] = (float)nh;
        return true;
    }
    memcpy(q.start, A->v, 12), memcpy(q.end, B->v, 12);
    if (multi) {
        static SqHit hs[256];
        uint32_t n = sq_trace_multi(w, &q, hs, 256);
        Arr *acts = trace_out_arr(&D[1]), *locs = trace_out_arr(&D[2]), *nrms = trace_out_arr(&D[3]), *dist = trace_out_arr(&D[4]);
        for (uint32_t i = 0; i < n; i++) {
            BpValue v;
            memset(&v, 0, sizeof v), set_ent(&v, hs[i].actor);
            if (acts) arr_push(in, acts, &v);
            memset(&v, 0, sizeof v), memcpy(v.v, hs[i].location, 12);
            if (locs) arr_push(in, locs, &v);
            memset(&v, 0, sizeof v), memcpy(v.v, hs[i].normal, 12);
            if (nrms) arr_push(in, nrms, &v);
            memset(&v, 0, sizeof v), v.v[0] = hs[i].distance;
            if (dist) arr_push(in, dist, &v);
        }
        if (draw) { // (the line red to the first hit, green after; a point on each)
            SqHit first = n ? hs[0] : (SqHit){ 0 };
            if (!n) memcpy(first.location, q.end, 12);
            dd_traces(&q, &first, 1, trace_rgba(false), trace_rgba(true), secs);
            for (uint32_t i = 1; i < n; i++) dd_point(hs[i].impact_point, trace_rgba(true), 8.0f, secs);
        }
        memset(&D[0], 0, sizeof D[0]), D[0].v[0] = n ? 1.0f : 0.0f;
        return true;
    }
    SqHit h;
    bool hit = sq_trace(w, &q, &h);
    if (draw) {
        dd_traces(&q, &h, 1, trace_rgba(false), trace_rgba(true), secs);
        uint32_t c = trace_rgba(hit);
        const float* at3 = hit ? h.location : q.end;
        if (shape == SQ_SPHERE) dd_sphere(at3, q.radius, c, 1.0f, secs), dd_sphere(q.start, q.radius, trace_rgba(false), 1.0f, secs);
        else if (shape == SQ_BOX) {
            float mn[3], mx[3];
            for (int k = 0; k < 3; k++) mn[k] = at3[k] - fabsf(q.half[k]), mx[k] = at3[k] + fabsf(q.half[k]);
            dd_box(mn, mx, c, 1.0f, secs);
        } else if (shape == SQ_CAPSULE) dd_capsule(at3, q.half[2], q.radius, c, 1.0f, secs);
    }
    for (int k = 0; k < 6; k++) memset(&D[k], 0, sizeof D[k]);
    D[0].v[0] = hit ? 1.0f : 0.0f;
    memcpy(D[1].v, h.location, 12), memcpy(D[2].v, h.impact_point, 12), memcpy(D[3].v, h.normal, 12);
    set_ent(&D[4], h.actor);
    D[5].v[0] = h.distance;
    return true;
}

static bool mr_native(Inst* in, uint32_t kind, const BpValue* A, const BpValue* B, const BpValue* C, BpValue* D) {
    World* w = V.w;
    BpValue res = { { 0 } };
    ProjectVr* pv = &project()->vr;
    switch (kind) {
    case BPK_MR_SET_PASSTHROUGH: mr_set_passthrough(A->v[0] != 0.0f); return true;
    case BPK_MR_IS_PASSTHROUGH: res.v[0] = mr_passthrough_active() ? 1.0f : 0.0f; break;
    case BPK_MR_SET_OPACITY: pv->mr_opacity = fminf(fmaxf(A->v[0], 0.0f), 1.0f); return true;
    case BPK_MR_SET_STYLE: { // Style, Brightness, Contrast, Saturation (np[0]), Tint (np[1])
        int st = (int)lroundf(A->v[0]);
        pv->mr_style = st < 0 ? 0 : st >= MR_STYLE_COUNT ? MR_STYLE_COUNT - 1 : st;
        pv->mr_brightness = fminf(fmaxf(B->v[0], -100.0f), 100.0f), pv->mr_contrast = fmaxf(C->v[0], 0.0f), pv->mr_saturation = fmaxf(V.np[0].v[0], 0.0f);
        if (V.np[1].v[0] + V.np[1].v[1] + V.np[1].v[2] > 0.0f) memcpy(pv->mr_tint, V.np[1].v, 12);
        return true;
    }
    case BPK_MR_SET_EDGE: memcpy(pv->mr_edge, A->v, 12), pv->mr_edge[3] = fminf(fmaxf(B->v[0], 0.0f), 1.0f); return true;
    case BPK_MR_SET_BACKGROUND: pv->mr_background = A->v[0] != 0.0f ? MR_BG_ROOM : MR_BG_LEVEL; return true;
    case BPK_MR_SET_BLEND: pv->mr_virtual_opacity = fminf(fmaxf(A->v[0], 0.0f), 1.0f); return true;
    case BPK_MR_SET_WINDOW: mr_set_window(w, ent_of(A) ? ent_of(A) : in->e, B->v[0] != 0.0f); return true;
    case BPK_MR_SET_OCCLUSION: {
        int m = (int)lroundf(A->v[0]);
        pv->mr_occlusion = m < MR_OCC_NONE ? MR_OCC_NONE : m > MR_OCC_DEPTH ? MR_OCC_DEPTH : m;
        return true;
    }
    case BPK_MR_LOAD_ROOM: res.v[0] = mr_load_room() ? 1.0f : 0.0f; break;
    case BPK_MR_ROOM_SETUP: res.v[0] = mr_request_room_setup() ? 1.0f : 0.0f; break;
    case BPK_MR_ROOM_PLANES: { // Actors (an array), Count
        Arr* out = arr_get(D);
        if (out) out->count = 0;
        Entity ents[MR_MAX_ANCHORS];
        uint32_t n = mr_room_actors(ents, MR_MAX_ANCHORS);
        for (uint32_t i = 0; i < n && out; i++) {
            BpValue v;
            memset(&v, 0, sizeof v);
            set_ent(&v, ents[i]);
            arr_push(in, out, &v);
        }
        memset(&D[1], 0, sizeof D[1]), D[1].v[0] = (float)n;
        return true;
    }
    case BPK_MR_PLANE_INFO: { // Valid, Label, Center, Normal, Size
        int label = 0;
        float c[3] = { 0, 0, 0 }, n[3] = { 0, 0, 1 }, sz[3] = { 0, 0, 0 };
        bool ok = mr_actor_info(w, ent_of(A), &label, c, n, sz);
        res.v[0] = ok ? 1.0f : 0.0f;
        set_str(&D[1], ok ? mr_label_name(label) : "");
        memset(&D[2], 0, sizeof D[2] * 3), memcpy(D[2].v, c, 12), memcpy(D[3].v, n, 12), memcpy(D[4].v, sz, 12);
        break;
    }
    case BPK_MR_TRACE: { // Hit, Location, Normal, Label, Actor
        float hit[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 1 };
        int label = -1;
        Entity act = ENTITY_NONE;
        bool h = mr_line_trace(A->v, B->v, hit, nrm, &label, &act);
        res.v[0] = h ? 1.0f : 0.0f;
        memset(&D[1], 0, sizeof D[1] * 2), memcpy(D[1].v, hit, 12), memcpy(D[2].v, nrm, 12);
        set_str(&D[3], h ? mr_label_name(label) : "");
        set_ent(&D[4], act);
        break;
    }
    default: return false;
    }
    *D = res;
    return true;
}
