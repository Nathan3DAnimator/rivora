// material.c's second half: the compiler (graph -> bytecode, static part / dynamic part), the textures (sources read
// through WIC, baked ones), the bake and the GpuMaterial slots. Included by material.c (its state M).

// ============================================================================================
// The compiler
// ============================================================================================

#define IR_MAX_VALS 4096
#define IR_MAX_INS 2048
#define IR_MAX_IN 40

typedef struct IrIns {
    int op, nin, nout;
    uint32_t p[4];
    int in[IR_MAX_IN];
    int out[MAT_NODE_MAX_OUT]; // (Q19: up to 5, the Tlender's cells)
    bool dyn;
} IrIns;

typedef struct IrVal {
    int def;          // the instruction that writes it
    bool dyn;         // depends on a parameter or the view
    bool emissive;    // a closure's emission part that can be non-zero
    int slot;         // a static value the dynamic part reads: its baked texture (-1: none)
} IrVal;

typedef struct Ir {
    const Material* m;
    const MatGraph* g;
    IrIns ins[IR_MAX_INS];
    int nins;
    IrVal vals[IR_MAX_VALS];
    int nvals;
    int memo[MAT_MAX_NODES][3][MAT_NODE_MAX_OUT]; // per node and context (0, +u, +v): its outputs' values (+1; 0: not yet)
    int memo_cl[MAT_MAX_NODES][3][3];             // a shader node's closure (3 values)
    char pnames[MAT_MAX_PARAMS][MAT_NAME];
    float pdef[MAT_MAX_PARAMS][4];
    int nparams;
    float texel;
    bool fail;
    char err[160];
    float anim_period; // a Time node's loop (0: none)
} Ir;

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static int ir_emit(Ir* ir, int op, const uint32_t p[4], const int* in, int nin, int nout, int* outs) {
    if (ir->nins >= IR_MAX_INS || ir->nvals + nout > IR_MAX_VALS || nin > IR_MAX_IN) {
        ir->fail = true;
        snprintf(ir->err, sizeof ir->err, "material too large");
        return -1;
    }
    IrIns* x = &ir->ins[ir->nins];
    memset(x, 0, sizeof *x);
    x->op = op;
    x->nin = nin;
    x->nout = nout;
    if (p) memcpy(x->p, p, sizeof x->p);
    // everything is baked (parameters into each variant's own textures; the view as seen head-on): nothing per pixel
    bool dyn = false;
    for (int i = 0; i < nin; i++) {
        x->in[i] = in[i];
        dyn = dyn || (in[i] >= 0 && ir->vals[in[i]].dyn);
    }
    x->dyn = dyn;
    for (int o = 0; o < nout; o++) {
        int v = ir->nvals++;
        ir->vals[v] = (IrVal){ ir->nins, dyn, false, -1 };
        x->out[o] = v;
        if (outs) outs[o] = v;
    }
    return ir->nins++;
}

static int ir_const(Ir* ir, const float v[4]) {
    uint32_t p[4] = { fbits(v[0]), fbits(v[1]), fbits(v[2]), fbits(v[3]) };
    int out;
    ir_emit(ir, MOP_CONST, p, NULL, 0, 1, &out);
    return out;
}
static int ir_const1(Ir* ir, float x) {
    float v[4] = { x, x, x, x };
    return ir_const(ir, v);
}
static int ir_simple(Ir* ir, int op, uint32_t p0, uint32_t p1) {
    uint32_t p[4] = { p0, p1, 0, 0 };
    int out;
    ir_emit(ir, op, p, NULL, 0, 1, &out);
    return out;
}
static int ir_uv(Ir* ir, int ctx) { return ir_simple(ir, MOP_UV, (uint32_t)ctx, fbits(ir->texel)); }

static bool sock_is_vec(int t) { return t == MSOCK_VECTOR || t == MSOCK_COLOR; }

static void compile_node(Ir* ir, int node, int ctx);

// A value converted between socket types (Blender's implicit conversions)
static int ir_convert(Ir* ir, int v, int from, int to) {
    if (from == to || (sock_is_vec(from) && sock_is_vec(to))) return v;
    int out = v;
    if (to == MSOCK_FLOAT) ir_emit(ir, from == MSOCK_COLOR ? MOP_CVT_LUM : MOP_CVT_AVG, NULL, &v, 1, 1, &out);
    else ir_emit(ir, MOP_CVT_SPLAT, NULL, &v, 1, 1, &out);
    return out;
}

// A link out of a Reroute followed back to what feeds the knot (knots into knots too); -1: nothing
static int follow_link(const MatGraph* g, int li) {
    for (int k = 0; k < 64 && li >= 0 && g->nodes[g->links[li].from].type == MAT_NODE_REROUTE; k++) li = mat_graph_input_link(g, g->links[li].from, 0);
    return li;
}

// The value of a node's input: its link, its implicit data (UV, normal) or its constant
static int compile_input(Ir* ir, int node, int sock, int ctx) {
    const MatNode* n = &ir->g->nodes[node];
    const MatNodeDef* d = mat_node_def(n->type);
    const MatSockDef* s = &d->in[sock];
    int li = follow_link(ir->g, mat_graph_input_link(ir->g, node, sock));
    if (li >= 0) {
        const MatLink* l = &ir->g->links[li];
        compile_node(ir, l->from, ctx);
        if (ir->fail) return -1;
        const MatNodeDef* fd = mat_node_def(ir->g->nodes[l->from].type);
        int v = ir->memo[l->from][ctx][l->from_sock] - 1;
        return ir_convert(ir, v, fd->out[l->from_sock].type, s->type);
    }
    if (s->flags & (MSF_IMPLICIT_UV | MSF_IMPLICIT_GENERATED)) return ir_uv(ir, ctx);
    if (s->flags & MSF_IMPLICIT_NORMAL) return ir_simple(ir, MOP_NORMAL, 0, 0);
    return ir_const(ir, n->in[sock]);
}

// A closure input (a shader socket): its link, or Blender's default (black: nothing)
static void compile_closure_input(Ir* ir, int node, int sock, int ctx, int cl[3]) {
    int li = follow_link(ir->g, mat_graph_input_link(ir->g, node, sock));
    if (li >= 0) {
        int from = ir->g->links[li].from;
        compile_node(ir, from, ctx);
        if (!ir->fail)
            for (int k = 0; k < 3; k++) cl[k] = ir->memo_cl[from][ctx][k] - 1;
        return;
    }
    float zero[4] = { 0, 0, 0, 0 }, rough[4] = { 0, 0, 0, 0.5f }, nrm[4] = { 0, 0, 1, 0 };
    cl[0] = ir_const(ir, zero);
    cl[1] = ir_const(ir, rough);
    cl[2] = ir_const(ir, nrm);
}

static int param_index(Ir* ir, const MatNode* n, int kind) {
    for (int i = 0; i < ir->nparams; i++)
        if (!strcmp(ir->pnames[i], n->param)) return i;
    if (ir->nparams >= MAT_MAX_PARAMS) return -1;
    int i = ir->nparams++;
    snprintf(ir->pnames[i], MAT_NAME, "%s", n->param);
    memcpy(ir->pdef[i], kind ? n->value : (float[4]){ n->value[0], n->value[0], n->value[0], n->value[0] }, 16);
    return i;
}

// A BSDF as a closure: base color, metallic, roughness, alpha, normal, emission color, emission strength
static void closure(Ir* ir, int node, const int ins[7]) {
    int outs[3];
    ir_emit(ir, MOP_CLOSURE, NULL, ins, 7, 3, outs);
    if (ir->fail) return;
    // emissive unless its strength is a constant 0 or its color a constant black
    const IrIns* sx = &ir->ins[ir->vals[ins[6]].def];
    const IrIns* cx = &ir->ins[ir->vals[ins[5]].def];
    float s, c0, c1, c2;
    memcpy(&s, &sx->p[0], 4);
    memcpy(&c0, &cx->p[0], 4), memcpy(&c1, &cx->p[1], 4), memcpy(&c2, &cx->p[2], 4);
    bool zero = (sx->op == MOP_CONST && s == 0.0f) || (cx->op == MOP_CONST && c0 == 0.0f && c1 == 0.0f && c2 == 0.0f);
    ir->vals[outs[1]].emissive = !zero;
    for (int k = 0; k < 3; k++) ir->memo_cl[node][0][k] = outs[k] + 1; // (the caller copies it to its context)
}

static void compile_node(Ir* ir, int node, int ctx) {
    if (ir->fail) return;
    const MatNode* n = &ir->g->nodes[node];
    const MatNodeDef* d = mat_node_def(n->type);
    if (!d) {
        ir->fail = true;
        snprintf(ir->err, sizeof ir->err, "unsupported node");
        return;
    }
    bool shader = d->nout > 0 && d->out[0].type == MSOCK_SHADER;
    if (shader ? ir->memo_cl[node][ctx][0] : (d->nout == 0 || ir->memo[node][ctx][0])) return;
    int outs[MAT_NODE_MAX_OUT] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    float zero[4] = { 0, 0, 0, 0 }, one[4] = { 1, 1, 1, 1 };
    switch (n->type) {
    case NODE_RGB:
    case NODE_VALUE: {
        int kind = n->type == NODE_RGB;
        int pi = n->param[0] ? param_index(ir, n, kind) : -1;
        if (pi >= 0) outs[0] = ir_simple(ir, MOP_PARAM, (uint32_t)pi, 0);
        else outs[0] = ir_const(ir, kind ? n->value : (float[4]){ n->value[0], n->value[0], n->value[0], n->value[0] });
        break;
    }
    case NODE_TEX_COORD: {
        int uv = ir_uv(ir, ctx), nrm = ir_simple(ir, MOP_NORMAL, 0, 0), view = ir_simple(ir, MOP_VIEW, 0, 0);
        int o[7] = { uv, nrm, uv, uv, view, uv, view };
        memcpy(outs, o, sizeof o);
        break;
    }
    case NODE_UV_MAP: outs[0] = ir_uv(ir, ctx); break;
    case NODE_DISPLACEMENT: { // (its height splatted: what the parallax reads, M7)
        int h = compile_input(ir, node, 0, ctx);
        if (h >= 0) ir_emit(ir, MOP_CVT_SPLAT, NULL, &h, 1, 1, &outs[0]);
        break;
    }
    case NODE_VERTEX_COLOR: // (baked white: the renderer multiplies by the vertex colors, mat_vcol_usage)
        outs[0] = ir_const(ir, (const float[4]){ 1.0f, 1.0f, 1.0f, 1.0f }), outs[1] = ir_const1(ir, 1.0f);
        break;
    case NODE_RE_TIME: { // the loop's time (a hidden parameter each baked frame sets), its phase, a wave of it
        MatNode tn = *n;
        snprintf(tn.param, sizeof tn.param, "__time");
        tn.value[0] = 0.0f;
        int pi = param_index(ir, &tn, 0);
        int t = pi >= 0 ? ir_simple(ir, MOP_PARAM, (uint32_t)pi, 0) : ir_const1(ir, 0.0f);
        int per = ir_const1(ir, n->in[0][0] > 0.05f ? n->in[0][0] : 0.05f), zero1 = ir_const1(ir, 0.0f);
        int in3[3] = { t, per, zero1 }, ph = -1, ang = -1, s = -1, c = -1;
        ir_emit(ir, NODE_MATH, (uint32_t[4]){ MATH_DIVIDE, 0, 0, 0 }, in3, 3, 1, &ph);
        int in4[3] = { ph, ir_const1(ir, 6.28318531f), zero1 };
        ir_emit(ir, NODE_MATH, (uint32_t[4]){ MATH_MULTIPLY, 0, 0, 0 }, in4, 3, 1, &ang);
        int in5[3] = { ang, zero1, zero1 };
        ir_emit(ir, NODE_MATH, (uint32_t[4]){ MATH_SINE, 0, 0, 0 }, in5, 3, 1, &s);
        ir_emit(ir, NODE_MATH, (uint32_t[4]){ MATH_COSINE, 0, 0, 0 }, in5, 3, 1, &c);
        outs[0] = t, outs[1] = ph, outs[2] = s, outs[3] = c;
        ir->anim_period = n->in[0][0] > 0.05f ? n->in[0][0] : 0.05f;
        break;
    }
    case NODE_FRESNEL:
    case NODE_LAYER_WEIGHT: {
        int in[3] = { compile_input(ir, node, 0, ctx), compile_input(ir, node, 1, ctx), ir_simple(ir, MOP_VIEW, 0, 0) };
        ir_emit(ir, n->type, NULL, in, 3, d->nout, outs);
        break;
    }
    case NODE_BSDF_PRINCIPLED: {
        int in[7] = { compile_input(ir, node, 0, ctx), compile_input(ir, node, 1, ctx), compile_input(ir, node, 2, ctx),
                      compile_input(ir, node, 4, ctx), compile_input(ir, node, 5, ctx), compile_input(ir, node, 26, ctx),
                      compile_input(ir, node, 27, ctx) };
        if (!ir->fail) closure(ir, node, in);
        break;
    }
    case NODE_BSDF_DIFFUSE:
    case NODE_BSDF_GLOSSY: {
        bool glossy = n->type == NODE_BSDF_GLOSSY;
        // (Glossy: Color 0, Roughness 1, Normal 4; its Color, Roughness and Normal were read from 1, 2 and 5 before:
        // the color from the Roughness, the roughness from the Anisotropy, the normal from the Tangent)
        int in[7] = { compile_input(ir, node, 0, ctx), ir_const1(ir, glossy ? 1.0f : 0.0f),
                      glossy ? compile_input(ir, node, 1, ctx) : ir_const1(ir, 1.0f), ir_const1(ir, 1.0f),
                      compile_input(ir, node, glossy ? 4 : 2, ctx), ir_const(ir, zero), ir_const1(ir, 0.0f) };
        if (!ir->fail) closure(ir, node, in);
        break;
    }
    case NODE_BSDF_TRANSPARENT: {
        int in[7] = { ir_const(ir, zero), ir_const1(ir, 0.0f), ir_const1(ir, 0.5f), ir_const1(ir, 0.0f),
                      ir_simple(ir, MOP_NORMAL, 0, 0), ir_const(ir, zero), ir_const1(ir, 0.0f) };
        closure(ir, node, in);
        break;
    }
    case NODE_EMISSION: {
        int in[7] = { ir_const(ir, zero), ir_const1(ir, 0.0f), ir_const1(ir, 1.0f), ir_const(ir, one),
                      ir_simple(ir, MOP_NORMAL, 0, 0), compile_input(ir, node, 0, ctx), compile_input(ir, node, 1, ctx) };
        if (!ir->fail) closure(ir, node, in);
        break;
    }
    case NODE_RE_INTERIOR: // (2026-11-17, Q4) its shader as it is: the rooms are the renderer's (interior_ext, mesh.frag interior)
    case NODE_RE_WET: { // (2026-11-16) its shader as it is: the wetness is the renderer's (wet_ext, mesh.frag mat_wet)
        int cl[3];
        compile_closure_input(ir, node, 0, ctx, cl);
        if (ir->fail) return;
        for (int j = 0; j < 3; j++) ir->memo_cl[node][0][j] = cl[j] + 1;
        break;
    }
    case NODE_MIX_SHADER:
    case NODE_ADD_SHADER: {
        bool mix = n->type == NODE_MIX_SHADER;
        int a[3], b[3], in[7], co[3];
        int k = 0;
        if (mix) in[k++] = compile_input(ir, node, 0, ctx);
        compile_closure_input(ir, node, mix ? 1 : 0, ctx, a);
        compile_closure_input(ir, node, mix ? 2 : 1, ctx, b);
        if (ir->fail) return;
        for (int j = 0; j < 3; j++) in[k + j] = a[j], in[k + 3 + j] = b[j];
        ir_emit(ir, mix ? MOP_SHADER_MIX : MOP_SHADER_ADD, NULL, in, mix ? 7 : 6, 3, co);
        if (ir->fail) return;
        ir->vals[co[1]].emissive = ir->vals[a[1]].emissive || ir->vals[b[1]].emissive;
        for (int j = 0; j < 3; j++) ir->memo_cl[node][0][j] = co[j] + 1;
        break;
    }
    case NODE_BUMP: {
        // its height at the point and one texel along u and along v (the Height input compiled in those contexts)
        int in[6] = { compile_input(ir, node, 0, ctx), compile_input(ir, node, 1, ctx), compile_input(ir, node, 2, ctx),
                      compile_input(ir, node, 3, ctx), compile_input(ir, node, 2, ctx ? ctx : 1),
                      compile_input(ir, node, 2, ctx ? ctx : 2) };
        uint32_t p[4] = { (uint32_t)n->prop[0], fbits(ir->texel), 0, 0 };
        ir_emit(ir, MOP_BUMP_HEIGHTS, p, in, 6, 1, outs);
        break;
    }
    case NODE_COLOR_RAMP: {
        int in[1 + 2 * RAMP_MAX_STOPS], c = n->ramp_count > 0 ? n->ramp_count : 1;
        in[0] = compile_input(ir, node, 0, ctx);
        for (int s = 0; s < c; s++) {
            in[1 + s * 2] = ir_const(ir, &n->ramp[s][1]);
            in[2 + s * 2] = ir_const1(ir, n->ramp[s][0]);
        }
        uint32_t p[4] = { (uint32_t)n->prop[0], (uint32_t)c, 0, 0 };
        ir_emit(ir, NODE_COLOR_RAMP, p, in, 1 + 2 * c, 2, outs);
        break;
    }
    case NODE_TEX_IMAGE: {
        extern uint32_t mat_source_texture(const char* path);
        int in[1] = { compile_input(ir, node, 0, ctx) };
        uint32_t p[4] = { (uint32_t)n->prop[0], (uint32_t)n->prop[1], (uint32_t)n->prop[2], mat_source_texture(n->image) };
        ir_emit(ir, NODE_TEX_IMAGE, p, in, 1, 2, outs);
        break;
    }
    // (Q19) the Tlender's nodes the Engine bakes only (mat_vm.glsl under MAT_BAKE): Brick's frequencies are the props'
    // index + 1; Cracks, Stones and Grains are one instruction (the cells) with their inputs in its order
    case NODE_TEX_BRICK: {
        int in[NIN_TEX_BRICK];
        for (int i = 0; i < NIN_TEX_BRICK; i++) in[i] = compile_input(ir, node, i, ctx);
        if (ir->fail) return;
        uint32_t p[4] = { (uint32_t)n->prop[0] + 1u, (uint32_t)n->prop[1] + 1u, 0, 0 };
        ir_emit(ir, NODE_TEX_BRICK, p, in, NIN_TEX_BRICK, 2, outs);
        break;
    }
    case NODE_TEX_CRACKS:
    case NODE_TEX_STONES:
    case NODE_TEX_GRAINS: {
        int s[7];
        for (int i = 0; i < d->nin && i < 7; i++) s[i] = compile_input(ir, node, i, ctx);
        if (ir->fail) return;
        int zero1 = ir_const1(ir, 0.0f), c[5];
        int in[NIN_TEX_CELLS], mode = n->type == NODE_TEX_STONES ? CELLS_STONES : n->type == NODE_TEX_GRAINS ? CELLS_GRAINS : CELLS_CRACKS;
        if (mode == CELLS_CRACKS) { // vector, scale, width, distortion, detail, randomness (6), roundness (the Curl, 5)
            int o[NIN_TEX_CELLS] = { s[0], s[1], s[2], s[3], s[4], s[6], s[5] };
            memcpy(in, o, sizeof o);
        } else if (mode == CELLS_STONES) { // vector, scale, mortar, distortion (4), detail 0, randomness (5), roundness (3)
            int o[NIN_TEX_CELLS] = { s[0], s[1], s[2], s[4], zero1, s[5], s[3] };
            memcpy(in, o, sizeof o);
        } else { // Grains: vector, scale, grain size, 0, 0, randomness (3), 0
            int o[NIN_TEX_CELLS] = { s[0], s[1], s[2], zero1, zero1, s[3], zero1 };
            memcpy(in, o, sizeof o);
        }
        uint32_t p[4] = { (uint32_t)mode, 0, 0, 0 };
        ir_emit(ir, NODE_TEX_CRACKS, p, in, NIN_TEX_CELLS, NOUT_TEX_CELLS, c); // gap, height, random, distance, random vector
        if (ir->fail) return;
        if (mode == CELLS_CRACKS) outs[0] = c[0], outs[1] = c[1], outs[2] = c[2], outs[3] = c[3];
        else if (mode == CELLS_GRAINS) outs[0] = c[1], outs[1] = c[2], outs[2] = c[4], outs[3] = c[0];
        else { // Stones: the mortar, height, random, and the random vector into [0, 1] as its color
            float half[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
            int h = ir_const(ir, half), vin[4] = { c[4], h, h, ir_const1(ir, 1.0f) }, vo[2];
            ir_emit(ir, NODE_VECTOR_MATH, (uint32_t[4]){ VMATH_MULTIPLY_ADD, 0, 0, 0 }, vin, 4, 2, vo);
            outs[0] = c[0], outs[1] = c[1], outs[2] = c[2], outs[3] = vo[0];
        }
        break;
    }
    default: { // the generic nodes: every input, every output, the properties as p0..p3
        int in[MAT_NODE_MAX_IN];
        for (int i = 0; i < d->nin; i++) in[i] = compile_input(ir, node, i, ctx);
        if (ir->fail) return;
        uint32_t p[4];
        for (int k = 0; k < 4; k++) p[k] = (uint32_t)n->prop[k];
        ir_emit(ir, n->type, p, in, d->nin, d->nout, outs);
        break;
    }
    }
    if (ir->fail) return;
    if (shader) {
        if (ctx != 0)
            for (int k = 0; k < 3; k++) ir->memo_cl[node][ctx][k] = ir->memo_cl[node][0][k];
        return;
    }
    for (int o = 0; o < d->nout; o++) ir->memo[node][ctx][o] = outs[o] + 1;
}

// Registers by liveness: a value takes a register when written and frees it after its last read in that program
typedef struct Prog {
    uint32_t* w;
    uint32_t n, cap;
} Prog;

static void prog_put(Prog* p, uint32_t x) {
    if (p->n >= p->cap) {
        uint32_t c = p->cap ? p->cap * 2 : 1024;
        uint32_t* nw = realloc(p->w, c * 4); // a compile, not a frame
        if (!nw) return;
        p->w = nw;
        p->cap = c;
    }
    p->w[p->n++] = x;
}

// Writes the instructions marked in take[] (in order) with registers; extra: the LOAD_SLOT / STORE_SLOT ops. False when
// it needs more than regs registers.
static bool emit_program(Ir* ir, const bool* take, bool runtime, int regs, Prog* out) {
    static int last[IR_MAX_VALS], reg[IR_MAX_VALS];
    for (int v = 0; v < ir->nvals; v++) last[v] = -1, reg[v] = -1;
    // a value read in this program: from its own instruction, or (runtime) a baked slot
    for (int i = 0; i < ir->nins; i++) {
        if (!take[i]) continue;
        for (int k = 0; k < ir->ins[i].nin; k++) last[ir->ins[i].in[k]] = i;
    }
    if (!runtime) // the bake keeps a slot's value until its store at the end
        for (int v = 0; v < ir->nvals; v++)
            if (ir->vals[v].slot >= 0) last[v] = IR_MAX_INS;
    bool used[MAT_BAKE_REGS] = { 0 };
    int pending_free[IR_MAX_IN + 3];
    // runtime: the slots first (each loaded once)
    if (runtime)
        for (int v = 0; v < ir->nvals; v++) {
            if (ir->vals[v].slot < 0 || last[v] < 0) continue;
            int r = 0;
            while (r < regs && used[r]) r++;
            if (r >= regs) return false;
            used[r] = true;
            reg[v] = r;
            prog_put(out, MOP_LOAD_SLOT | 0u << 16 | 1u << 24);
            prog_put(out, (uint32_t)ir->vals[v].slot), prog_put(out, 0), prog_put(out, 0), prog_put(out, 0);
            prog_put(out, (uint32_t)r);
        }
    for (int i = 0; i < ir->nins; i++) {
        if (!take[i]) continue;
        const IrIns* x = &ir->ins[i];
        int ins_r[IR_MAX_IN], np = 0;
        for (int k = 0; k < x->nin; k++) {
            int v = x->in[k];
            ins_r[k] = reg[v] < 0 ? 0 : reg[v];
            if (last[v] == i) pending_free[np++] = v;
        }
        int outs_r[MAT_NODE_MAX_OUT];
        for (int o = 0; o < x->nout; o++) {
            int r = 0;
            while (r < regs && used[r]) r++;
            if (r >= regs) return false;
            used[r] = true;
            outs_r[o] = r;
            reg[x->out[o]] = r;
        }
        // the inputs read for the last time free their registers after the instruction (an output may not overwrite
        // an input it still reads: the outputs took fresh registers above)
        for (int k = 0; k < np; k++)
            if (reg[pending_free[k]] >= 0) used[reg[pending_free[k]]] = false;
        prog_put(out, (uint32_t)x->op | (uint32_t)x->nin << 16 | (uint32_t)x->nout << 24);
        for (int k = 0; k < 4; k++) prog_put(out, x->p[k]);
        for (int k = 0; k < x->nin; k++) prog_put(out, (uint32_t)ins_r[k]);
        for (int o = 0; o < x->nout; o++) prog_put(out, (uint32_t)outs_r[o]);
        for (int o = 0; o < x->nout; o++) // an output nothing reads: free at once
            if (last[x->out[o]] < 0) used[outs_r[o]] = false;
    }
    if (!runtime) // the slots' stores
        for (int v = 0; v < ir->nvals; v++) {
            if (ir->vals[v].slot < 0 || reg[v] < 0) continue;
            prog_put(out, MOP_STORE_SLOT | 1u << 16);
            prog_put(out, (uint32_t)ir->vals[v].slot), prog_put(out, 0), prog_put(out, 0), prog_put(out, 0);
            prog_put(out, (uint32_t)reg[v]);
        }
    return true;
}

typedef struct Built {
    Prog bake, run;
    int nslots;
    bool dynamic, emissive;
    float pom_depth;   // M7: the parallax's depth (uv units; 0: no Displacement, no parallax)
    int ninstr, nstatic, ndyn;
} Built;

static bool compile_material(Material* m, Ir* ir, Built* b) {
    memset(ir, 0, sizeof *ir);
    memset(b, 0, sizeof *b);
    ir->m = m;
    ir->g = m->graph;
    ir->texel = 1.0f / (float)(m->resolution > 0 ? m->resolution : 1024);
    int out_node = -1;
    for (int i = 0; i < ir->g->node_count; i++)
        if (ir->g->nodes[i].type == NODE_OUTPUT_MATERIAL) out_node = i;
    int cl[3];
    if (out_node < 0) { // no output: Blender renders black; here the default gray (visible)
        float b0[4] = { 0.8f, 0.8f, 0.8f, 0 }, b1[4] = { 0, 0, 0, 0.5f }, b2[4] = { 0, 0, 1, 1 };
        cl[0] = ir_const(ir, b0), cl[1] = ir_const(ir, b1), cl[2] = ir_const(ir, b2);
    } else {
        compile_closure_input(ir, out_node, 0, 0, cl);
    }
    if (ir->fail) return false;
    // M7: the Displacement's height into the closure's alpha (an opaque material's: its alpha is free), the parallax's map
    b->pom_depth = 0.0f;
    if (out_node >= 0 && m->blend_mode == MAT_BLEND_OPAQUE && follow_link(ir->g, mat_graph_input_link(ir->g, out_node, 2)) >= 0) {
        int li = follow_link(ir->g, mat_graph_input_link(ir->g, out_node, 2));
        const MatNode* dn = &ir->g->nodes[ir->g->links[li].from];
        float scale = dn->type == NODE_DISPLACEMENT && mat_graph_input_link(ir->g, ir->g->links[li].from, 2) < 0 ? dn->in[2][0] : 1.0f;
        int dv = compile_input(ir, out_node, 2, 0), hv = -1, nv = -1;
        if (dv >= 0) ir_emit(ir, MOP_CVT_AVG, NULL, &dv, 1, 1, &hv);
        int in2[2] = { cl[2], hv };
        if (hv >= 0 && ir_emit(ir, MOP_SET_W, NULL, in2, 2, 1, &nv) >= 0 && nv >= 0) cl[2] = nv, b->pom_depth = scale * 0.05f;
        if (ir->fail) return false;
    }
    int out_ins = ir_emit(ir, MOP_OUTPUT, NULL, cl, 3, 0, NULL);
    if (out_ins < 0) return false;
    b->emissive = ir->vals[cl[1]].emissive;
    b->dynamic = ir->ins[out_ins].dyn;
    // which instructions each program needs: back from the output (and, at runtime, the static values the dynamic
    // ones read become slots: baked)
    static bool need[IR_MAX_INS], take_bake[IR_MAX_INS], take_run[IR_MAX_INS];
    memset(need, 0, sizeof need);
    need[out_ins] = true;
    for (int i = ir->nins - 1; i >= 0; i--) {
        if (!need[i]) continue;
        for (int k = 0; k < ir->ins[i].nin; k++) need[ir->vals[ir->ins[i].in[k]].def] = true;
    }
    for (int i = 0; i < ir->nins; i++) {
        take_bake[i] = need[i] && !ir->ins[i].dyn && (!b->dynamic || i != out_ins);
        take_run[i] = need[i] && b->dynamic && ir->ins[i].dyn;
        b->ninstr += need[i];
        b->nstatic += take_bake[i];
        b->ndyn += take_run[i];
    }
    if (b->dynamic) {
        // the static values a dynamic instruction reads: baked into slots (constants are cheaper read again)
        for (int i = 0; i < ir->nins; i++) {
            if (!take_run[i]) continue;
            for (int k = 0; k < ir->ins[i].nin; k++) {
                IrVal* v = &ir->vals[ir->ins[i].in[k]];
                if (v->dyn || v->slot >= 0) continue;
                if (ir->ins[v->def].op == MOP_CONST || ir->ins[v->def].op == MOP_UV || ir->ins[v->def].op == MOP_NORMAL) {
                    take_run[v->def] = true; // recomputed per pixel (free)
                    continue;
                }
                if (b->nslots >= MAT_MAX_SLOTS) { // too many: everything per pixel
                    for (int j = 0; j < ir->nins; j++) take_run[j] = need[j], take_bake[j] = false;
                    for (int j = 0; j < ir->nvals; j++) ir->vals[j].slot = -1;
                    b->nslots = 0;
                    goto emit;
                }
                v->slot = b->nslots++;
            }
        }
        // a static instruction only the bake's slots need stays in the bake; one nothing needs leaves it
        static bool need_bake[IR_MAX_INS];
        memset(need_bake, 0, sizeof need_bake);
        for (int v = 0; v < ir->nvals; v++)
            if (ir->vals[v].slot >= 0) need_bake[ir->vals[v].def] = true;
        for (int i = ir->nins - 1; i >= 0; i--) {
            if (!need_bake[i] || !take_bake[i]) continue;
            for (int k = 0; k < ir->ins[i].nin; k++) need_bake[ir->vals[ir->ins[i].in[k]].def] = true;
        }
        for (int i = 0; i < ir->nins; i++) take_bake[i] = take_bake[i] && need_bake[i];
    }
emit:
    if (!emit_program(ir, take_bake, false, MAT_BAKE_REGS, &b->bake)) {
        snprintf(ir->err, sizeof ir->err, "too many values alive at once (bake)");
        return false;
    }
    if (b->dynamic && !emit_program(ir, take_run, true, MAT_VM_REGS, &b->run)) {
        snprintf(ir->err, sizeof ir->err, "too many values alive at once (per pixel)");
        return false;
    }
    return true;
}

// ============================================================================================
// Textures
// ============================================================================================

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(M.vk->gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static void write_descriptors(uint32_t idx) {
    MatTex* t = &M.tex[idx];
    VkDescriptorImageInfo si = { t->clamp ? M.sampler_clamp : M.sampler, t->view, VK_IMAGE_LAYOUT_GENERAL }; // (Q18: a UDIM tile's clamps)
    VkDescriptorImageInfo st = { VK_NULL_HANDLE, t->store_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    w[0].dstSet = M.set;
    w[0].dstBinding = 0;
    w[0].dstArrayElement = idx;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &si;
    w[1] = w[0];
    w[1].dstBinding = t->format == VK_FORMAT_R8G8B8A8_UNORM ? 1 : 2;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &st;
    vkUpdateDescriptorSets(M.vk->device, t->store_view ? 2 : 1, w, 0, NULL); // (BC7: sampled only)
}

// The textures' memory: sub-allocated from blocks of TB_SIZE (one vkAllocateMemory / vkFreeMemory costs the driver 0.3
// to 1.5 ms, whatever the size: the streaming makes and frees textures every few frames; the driver also allows only
// ~4096 allocations). First fit over each block's free ranges, merged when freed; an empty block is kept while it is
// the only one empty (the next climb reuses it). Bigger than half a block: an allocation of its own.
#define TB_SIZE (256ull << 20)
#define TB_MAX 64
#define TB_RANGES 512
typedef struct TexBlock {
    VkDeviceMemory mem;
    uint32_t type, nfree;
    VkDeviceSize used;
    struct { VkDeviceSize off, size; } fr[TB_RANGES]; // free ranges, by offset
} TexBlock;
static TexBlock* g_tb[TB_MAX];

static bool tb_take(TexBlock* b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize* out) {
    for (uint32_t i = 0; i < b->nfree; i++) {
        VkDeviceSize o = (b->fr[i].off + align - 1) / align * align, end = b->fr[i].off + b->fr[i].size;
        if (o + size > end) continue;
        VkDeviceSize lead = o - b->fr[i].off, tail = end - (o + size);
        if (lead && tail) { // split in two: a range more
            if (b->nfree >= TB_RANGES) continue;
            memmove(&b->fr[i + 2], &b->fr[i + 1], (b->nfree - i - 1) * sizeof b->fr[0]);
            b->nfree++;
            b->fr[i].size = lead;
            b->fr[i + 1].off = o + size, b->fr[i + 1].size = tail;
        } else if (lead) b->fr[i].size = lead;
        else if (tail) b->fr[i].off = o + size, b->fr[i].size = tail;
        else {
            memmove(&b->fr[i], &b->fr[i + 1], (b->nfree - i - 1) * sizeof b->fr[0]);
            b->nfree--;
        }
        b->used += size;
        *out = o;
        return true;
    }
    return false;
}

static void tb_give(TexBlock* b, VkDeviceSize off, VkDeviceSize size) {
    uint32_t i = 0;
    while (i < b->nfree && b->fr[i].off < off) i++;
    bool prev = i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == off, next = i < b->nfree && off + size == b->fr[i].off;
    if (prev && next) {
        b->fr[i - 1].size += size + b->fr[i].size;
        memmove(&b->fr[i], &b->fr[i + 1], (b->nfree - i - 1) * sizeof b->fr[0]);
        b->nfree--;
    } else if (prev) b->fr[i - 1].size += size;
    else if (next) b->fr[i].off = off, b->fr[i].size += size;
    else if (b->nfree < TB_RANGES) {
        memmove(&b->fr[i + 1], &b->fr[i], (b->nfree - i) * sizeof b->fr[0]);
        b->fr[i].off = off, b->fr[i].size = size;
        b->nfree++;
    } // (else the range is lost until the block empties: 512 holes never happen with these sizes)
    b->used -= size;
}

// Memory for an image: a block's range (block >= 0) or its own allocation (block -1)
static bool tex_mem_alloc(const VkMemoryRequirements* req, uint32_t type, VkDeviceMemory* mem, int* block, VkDeviceSize* off) {
    *block = -1, *off = 0;
    if (req->size <= TB_SIZE / 2) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < TB_MAX; i++) {
                TexBlock* b = g_tb[i];
                if (!b && pass == 1) { // a new block
                    b = calloc(1, sizeof *b);
                    if (!b) break;
                    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                    ai.allocationSize = TB_SIZE;
                    ai.memoryTypeIndex = type;
                    if (vkAllocateMemory(M.vk->device, &ai, NULL, &b->mem) != VK_SUCCESS) {
                        free(b);
                        break; // (out of memory for a block: an allocation of its own below)
                    }
                    b->type = type, b->nfree = 1, b->fr[0].off = 0, b->fr[0].size = TB_SIZE;
                    g_tb[i] = b;
                }
                if (!b || b->type != type || !tb_take(b, req->size, req->alignment, off)) continue;
                *mem = b->mem, *block = i;
                return true;
            }
        }
    }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req->size;
    ai.memoryTypeIndex = type;
    return vkAllocateMemory(M.vk->device, &ai, NULL, mem) == VK_SUCCESS;
}

static void tex_mem_free(VkDeviceMemory mem, int block, VkDeviceSize off, VkDeviceSize size) {
    if (block < 0) {
        vkFreeMemory(M.vk->device, mem, NULL);
        return;
    }
    TexBlock* b = g_tb[block];
    tb_give(b, off, size);
    if (b->used) return;
    for (int i = 0; i < TB_MAX; i++) // an empty block: kept if it is the only empty one
        if (i != block && g_tb[i] && !g_tb[i]->used) {
            vkFreeMemory(M.vk->device, b->mem, NULL);
            free(b);
            g_tb[block] = NULL;
            return;
        }
}

static void tex_mem_shutdown(void) {
    for (int i = 0; i < TB_MAX; i++)
        if (g_tb[i]) vkFreeMemory(M.vk->device, g_tb[i]->mem, NULL), free(g_tb[i]), g_tb[i] = NULL;
}

static void tex_free(uint32_t idx) {
    MatTex* t = &M.tex[idx];
    if (!t->image) return;
    VkDevice d = M.vk->device;
    if (t->store_view) vkDestroyImageView(d, t->store_view, NULL);
    vkDestroyImageView(d, t->view, NULL);
    vkDestroyImage(d, t->image, NULL);
    tex_mem_free(t->memory, t->block, t->offset, t->bytes);
    M.stats.tex_bytes -= t->bytes;
    memset(t, 0, sizeof *t);
    M.stats.textures--;
}

// A texture with its mips (GENERAL layout, sampled and storage), into a free index of the bindless set (0 stays empty).
// cmd: its layout set in that command buffer (the streaming's, inside a frame: no wait; whatever writes it next writes
// all of it); none: set and cleared now (a one-time submit)
static uint32_t tex_create_in(VkCommandBuffer cmd, uint32_t w, uint32_t h, VkFormat fmt) {
    uint32_t idx = 0;
    for (uint32_t i = 1; i < MAT_TEX_MAX; i++)
        if (!M.tex[i].image) {
            idx = i;
            break;
        }
    if (!idx) return 0;
    MatTex* t = &M.tex[idx];
    memset(t, 0, sizeof *t);
    uint32_t mips = 1;
    while ((w >> mips) > 0 || (h >> mips) > 0) mips++;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    bool bc = fmt == VK_FORMAT_BC7_UNORM_BLOCK || fmt == VK_FORMAT_BC5_UNORM_BLOCK; // (compressed: written by copies, never by a shader; BC5: Q18's normal maps)
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (bc ? 0 : VK_IMAGE_USAGE_STORAGE_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkDevice d = M.vk->device;
    if (vkCreateImage(d, &ii, NULL, &t->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, t->image, &req);
    if (!tex_mem_alloc(&req, mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), &t->memory, &t->block, &t->offset)) {
        vkDestroyImage(d, t->image, NULL);
        t->image = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(d, t->image, t->memory, t->offset);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    vkCreateImageView(d, &vi, NULL, &t->view);
    vi.subresourceRange.levelCount = 1;
    if (!bc) vkCreateImageView(d, &vi, NULL, &t->store_view);
    t->w = w, t->h = h, t->mips = mips, t->format = fmt;
    t->bytes = req.size;
    bool once = !cmd;
    if (once) cmd = gpu_begin_once(M.vk);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = t->image;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (once) {
        VkClearColorValue zero = { { 0, 0, 0, 0 } };
        if (!bc) vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
        gpu_end_once(M.vk, cmd);
    }
    write_descriptors(idx);
    M.stats.textures++;
    M.stats.tex_bytes += t->bytes;
    return idx;
}
static uint32_t tex_create(uint32_t w, uint32_t h, VkFormat fmt) { return tex_create_in(VK_NULL_HANDLE, w, h, fmt); }

static bool bc7_on(void); // (BC7: below, tex_to_bc7)
static uint32_t tex_to_bc7(VkCommandBuffer cmd, uint32_t src, GpuBuf* blocks);

// The mips by halving blits (after a bake or an upload)
static void tex_mips(VkCommandBuffer cmd, uint32_t idx) {
    MatTex* t = &M.tex[idx];
    for (uint32_t l = 1; l < t->mips; l++) {
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        int32_t sw = (int32_t)(t->w >> (l - 1)), sh = (int32_t)(t->h >> (l - 1));
        int32_t dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        VkImageBlit b = { { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1 }, { { 0, 0, 0 }, { sw ? sw : 1, sh ? sh : 1, 1 } },
                          { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, { { 0, 0, 0 }, { dw, dh, 1 } } };
        vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT);
}

// (2026-11-23) The mips of a baked texture by mip_kaiser.comp: each level from the one above through a Kaiser-windowed
// sinc (8 taps a side) in linear space for the color (srgb), tiling (wrap) or clamped (a UDIM tile). The 2x2 blits
// averaged a checker's thin lines grey a mip early (and dark: in sRGB), the floor blurred a few meters away. Another
// format, or no pipeline: the blits. Cost: a 2048 texture's chain ~0.3 ms of GPU, once per bake
// The buffer for a res x res bake's mips (before a command buffer is recorded: making it submits one of its own). Grown
// with the GPU idle (a frame in flight may read the old one)
static void mip_buf_reserve(uint32_t res, bool half) {
    VkDeviceSize need = (VkDeviceSize)(res > 1 ? res / 2 : 1) * (res > 1 ? res / 2 : 1) * (half ? 8u : 4u);
    if (!M.mip || M.mip_buf.size >= need) return;
    if (M.mip_buf.buffer) vkDeviceWaitIdle(M.vk->device), gpu_buf_destroy(M.vk, &M.mip_buf);
    if (!gpu_buf_create(M.vk, &M.mip_buf, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
        memset(&M.mip_buf, 0, sizeof M.mip_buf);
}
static void tex_mips_hq(VkCommandBuffer cmd, uint32_t idx, bool srgb, bool wrap) {
    MatTex* t = &M.tex[idx];
    bool half = t->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!M.mip || t->mips < 2 || (t->format != VK_FORMAT_R8G8B8A8_UNORM && !half)) {
        tex_mips(cmd, idx);
        return;
    }
    uint32_t bpp = half ? 8u : 4u, w1 = t->w > 1 ? t->w / 2 : 1, h1 = t->h > 1 ? t->h / 2 : 1;
    if (M.mip_buf.size < (VkDeviceSize)w1 * h1 * bpp) { // (reserved before recording: mip_buf_reserve)
        tex_mips(cmd, idx);
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.mip);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &M.set, 0, NULL);
    for (uint32_t l = 1; l < t->mips; l++) {
        // the level above written (the bake, or the last copy) and the buffer free (the last copy read it, maybe another
        // texture's: a barrier's first scope covers what the queue ran before)
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        uint32_t sw = t->w >> (l - 1) ? t->w >> (l - 1) : 1, sh = t->h >> (l - 1) ? t->h >> (l - 1) : 1;
        uint32_t dw = t->w >> l ? t->w >> l : 1, dh = t->h >> l ? t->h >> l : 1;
        struct { VkDeviceAddress out; uint32_t tex, src_level, w, h, sw, sh, flags; } p = {
            M.mip_buf.addr, idx, l - 1, dw, dh, sw, sh, (srgb && !half ? 1u : 0u) | (wrap ? 2u : 0u) | (half ? 4u : 0u) };
        vkCmdPushConstants(cmd, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, { 0, 0, 0 }, { dw, dh, 1 } };
        vkCmdCopyBufferToImage(cmd, M.mip_buf.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT);
}

// (Q21) a texture of the bindless set for another module (farview.c's impostor atlases): RGBA8 with its mips, written by
// copies into mip 0 then mat_tex_mips_in; released by its owner (after the GPU is done with it)
uint32_t mat_tex_new(uint32_t w, uint32_t h) { return M.ready ? tex_create(w, h, VK_FORMAT_R8G8B8A8_UNORM) : 0u; }
VkImage mat_tex_image(uint32_t idx) { return idx && idx < MAT_TEX_MAX ? M.tex[idx].image : VK_NULL_HANDLE; }
void mat_tex_mips_in(VkCommandBuffer cmd, uint32_t idx) {
    if (idx && idx < MAT_TEX_MAX && M.tex[idx].image) tex_mips(cmd, idx);
}
void mat_tex_release(uint32_t idx) {
    if (idx && idx < MAT_TEX_MAX) tex_free(idx);
}

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c (WIC)

// An Image Texture node's file in the bindless set (loaded once; 0: none / unreadable: reads black)
static uint32_t ud_source(const char* full); // (Q18) material_udim.inc
uint32_t mat_source_texture(const char* path) {
    if (!path || !path[0] || !M.ready) return 0;
    char full[MAT_PATH * 2];
    if (!strncmp(path, "/Content", 8)) snprintf(full, sizeof full, "%s%s", M.root, path + 8);
    else snprintf(full, sizeof full, "%s", path);
    if (udim_is_path(full)) return ud_source(full); // (Q18) a UDIM image: its table's row (the bake picks the tile)
    for (uint32_t i = 1; i < MAT_TEX_MAX; i++)
        if (M.tex[i].image && !_stricmp(M.tex[i].source, full)) return i;
    uint32_t w, h;
    uint8_t* img_take(const char* full, uint32_t* w, uint32_t* h);
    uint8_t* px = img_take(full, &w, &h); // (decoded by a worker: the editor's bakes never wait for a 4K JPG)
    if (!px) px = mat_load_image_rgba(full, &w, &h);
    if (!px) return 0;
    TexSettings ts; // (2026-11-23) the texture's own settings: the largest side the materials read it at, its compression
    tex_settings_get(full, &ts);
    while (ts.max_size && (w > ts.max_size || h > ts.max_size) && w > 1 && h > 1) { // (halved: a 2x2 box each time)
        uint32_t nw = w / 2, nh = h / 2;
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    const uint8_t* a = &px[((size_t)(2 * y) * w + 2 * x) * 4 + c];
                    px[((size_t)y * nw + x) * 4 + c] = (uint8_t)((a[0] + a[4] + a[(size_t)w * 4] + a[(size_t)w * 4 + 4] + 2) / 4);
                }
        w = nw, h = nh;
    }
    uint32_t idx = tex_create(w, h, VK_FORMAT_R8G8B8A8_UNORM);
    GpuBuf staging;
    if (idx && gpu_buf_create(M.vk, &staging, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        memcpy(staging.mapped, px, (size_t)w * h * 4);
        VkCommandBuffer cmd = gpu_begin_once(M.vk);
        VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
        vkCmdCopyBufferToImage(cmd, staging.buffer, M.tex[idx].image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        tex_mips(cmd, idx);
        gpu_end_once(M.vk, cmd);
        gpu_buf_destroy(M.vk, &staging);
        if (bc7_on() && ts.compression != TEXZ_NONE) { // (the bakes read it: compressed like their results, a quarter of the memory)
            GpuBuf blk = { 0 };
            cmd = gpu_begin_once(M.vk);
            uint32_t n = tex_to_bc7(cmd, idx, &blk);
            gpu_end_once(M.vk, cmd);
            gpu_buf_destroy(M.vk, &blk);
            if (n) tex_free(idx), idx = n; // (nothing reads it yet)
        }
        snprintf(M.tex[idx].source, sizeof M.tex[idx].source, "%s", full);
    }
    free(px);
    return idx;
}

// ============================================================================================
// ============================================================================================
// Slots, the bake, the variants (the material's own, its Material Instances', the actors' dynamic instances')
// ============================================================================================

static int mat_index(const Material* m) {
    for (uint32_t i = 0; i < M.n; i++)
        if (M.mats[i] == m) return (int)i;
    return -1;
}

static uint32_t slot_alloc(void) { return M.nslots + 1 < MAT_GPU_SLOTS ? ++M.nslots : 0; }

static uint32_t params_alloc(uint32_t n) {
    if (M.param_used + n > MAT_PARAM_VEC4) return 0;
    uint32_t o = M.param_used;
    M.param_used += n ? n : 1;
    return o;
}

static float (*param_vals(void))[4] { return (float(*)[4])M.params.mapped; }

// The parameter values of a variant: the compiled defaults, then the material's own values, then an instance's
static void fill_params(uint32_t off, const Compiled* c, const Material* m, const Material* inst) {
    float(*pv)[4] = param_vals();
    for (int i = 0; i < c->nparams; i++) {
        memcpy(pv[off + i], c->pdefault[i], 16);
        const Material* srcs[2] = { m, inst };
        for (int s = 0; s < 2; s++) {
            if (!srcs[s]) continue;
            for (int k = 0; k < srcs[s]->nparams; k++)
                if (!strcmp(srcs[s]->params[k].name, c->pnames[i])) memcpy(pv[off + i], srcs[s]->params[k].value, 16);
        }
    }
}

#include "material_stream.inc"
static void source_full(const char* path, char* full, size_t cap);
static void variant_free(Variant* v);
#include "material_udim.inc" // (Q18) UDIM

static void variant_free(Variant* v) {
    stream_cancel(v);
    if (v->ud_n) ud_owner_free(v); // (Q18) its UDIM tiles
    if (v->own)
        for (int k = 0; k < 3; k++) st_bury1(v->tex[k]); // (a frame in flight may still read them)
    memset(v->tex, 0, sizeof v->tex);
    v->own = false;
}

// The Masked cut's coverage kept in the mips (Castano's alpha test coverage, NVTT): the linear blits average the alpha,
// a card's thin strands fall under the clip value mip after mip (or a blob's edge spreads over it) and the hair, the
// leaves thin out or swell with the distance, and in the shadows (read at coarser mips). Per mip: the scale that puts the
// same share of its texels at the clip value or over as mip 0 has, multiplied into its alpha (alpha_hist.comp, one view
// and descriptor set per mip): the shaders read the alpha as it is, nothing more per pixel. (A runtime scale per mip read
// cost 1.5 ms of the heavy scene's prepass.)
#define AH_MAX_MIPS 16
static struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set[AH_MAX_MIPS];
    VkPipeline pipe;
} AH;

static bool alpha_cov_init(VkCtx* vk) {
    VkDescriptorSetLayoutBinding bd = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 1;
    dl.pBindings = &bd;
    if (vkCreateDescriptorSetLayout(vk->device, &dl, NULL, &AH.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &AH.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(vk->device, &pl, NULL, &AH.layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, AH_MAX_MIPS };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = AH_MAX_MIPS;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vk->device, &pi, NULL, &AH.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout ls[AH_MAX_MIPS];
    for (int i = 0; i < AH_MAX_MIPS; i++) ls[i] = AH.dsl;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = AH.pool;
    ai.descriptorSetCount = AH_MAX_MIPS;
    ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(vk->device, &ai, AH.set) != VK_SUCCESS) return false;
    AH.pipe = gpu_compute_pipeline(vk, AH.layout, s_ahist, sizeof s_ahist);
    return AH.pipe != VK_NULL_HANDLE;
}

static void alpha_cov_shutdown(VkCtx* vk) {
    if (AH.pipe) vkDestroyPipeline(vk->device, AH.pipe, NULL);
    if (AH.pool) vkDestroyDescriptorPool(vk->device, AH.pool, NULL);
    if (AH.layout) vkDestroyPipelineLayout(vk->device, AH.layout, NULL);
    if (AH.dsl) vkDestroyDescriptorSetLayout(vk->device, AH.dsl, NULL);
    memset(&AH, 0, sizeof AH);
}

typedef struct AhPush { VkDeviceAddress out; uint32_t mode, level, w, h; float scale; uint32_t pad; } AhPush;

// (after the bake's mips, the GPU idle: gpu_end_once waited)
static void alpha_coverage_mips(uint32_t tex, uint32_t clip8) {
    MatTex* t = &M.tex[tex];
    if (!AH.pipe || !t->image || t->mips < 2 || clip8 == 0 || t->format != VK_FORMAT_R8G8B8A8_UNORM || getenv("REALENGINE_NO_ALPHA_COVERAGE")) return;
    uint32_t nl = t->mips < AH_MAX_MIPS ? t->mips : AH_MAX_MIPS;
    GpuBuf hb = { 0 };
    if (!gpu_buf_create(M.vk, &hb, (VkDeviceSize)nl * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return;
    memset(hb.mapped, 0, (size_t)nl * 256 * 4);
    VkImageView views[AH_MAX_MIPS] = { 0 };
    bool ok = true;
    for (uint32_t l = 0; l < nl && ok; l++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = t->format;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1 };
        ok = vkCreateImageView(M.vk->device, &vi, NULL, &views[l]) == VK_SUCCESS;
        if (!ok) break;
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, views[l], VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = AH.set[l];
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(M.vk->device, 1, &wr, 0, NULL);
    }
    float scale[AH_MAX_MIPS];
    bool any = false;
    if (ok) { // 1. each mip's histogram
        VkCommandBuffer cmd = gpu_begin_once(M.vk);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, AH.pipe);
        for (uint32_t l = 0; l < nl; l++) {
            AhPush p = { hb.addr, 0, l, t->w >> l ? t->w >> l : 1, t->h >> l ? t->h >> l : 1, 1.0f, 0 };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, AH.layout, 0, 1, &AH.set[l], 0, NULL);
            vkCmdPushConstants(cmd, AH.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (p.w + 7) / 8, (p.h + 7) / 8, 1);
        }
        gpu_end_once(M.vk, cmd);
        // 2. the scales: mip 0's coverage at the clip value is the target
        const uint32_t* h = hb.mapped;
        double n0 = 0.0, c0 = 0.0;
        for (uint32_t b = 0; b < 256; b++) n0 += h[b], c0 += b >= clip8 ? h[b] : 0;
        double target = n0 > 0.0 ? c0 / n0 : 0.0;
        for (uint32_t l = 0; l < nl; l++) {
            scale[l] = 1.0f;
            if (l == 0 || target <= 0.0 || target >= 1.0) continue;
            const uint32_t* hl = h + l * 256;
            double n = 0.0;
            for (uint32_t b = 0; b < 256; b++) n += hl[b];
            // the lowest alpha value whose share of texels at it or over reaches the target (tb), and the next value below
            // that any texel has (lo): every threshold in (lo, tb] keeps that coverage. The clip value inside: no scale;
            // else the middle of the gap lands on it (a texel exactly at the clip value would be half covered: see-through)
            double acc = 0.0;
            uint32_t tb = 255;
            for (int b = 255; b >= 1; b--) {
                acc += hl[b];
                tb = (uint32_t)b;
                if (acc >= target * n) break;
            }
            uint32_t lo = 0;
            for (int b = (int)tb - 1; b >= 0; b--)
                if (hl[b]) { lo = (uint32_t)b; break; }
            float s = clip8 > lo && clip8 <= tb ? 1.0f : (float)clip8 / (0.5f * (float)(lo + tb));
            scale[l] = s < 0.25f ? 0.25f : s > 4.0f ? 4.0f : s;
            any |= scale[l] != 1.0f;
        }
    }
    if (ok && any) { // 3. the alpha of each mip scaled in place
        VkCommandBuffer cmd = gpu_begin_once(M.vk);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, AH.pipe);
        for (uint32_t l = 1; l < nl; l++) {
            if (scale[l] == 1.0f) continue;
            AhPush p = { hb.addr, 1, l, t->w >> l ? t->w >> l : 1, t->h >> l ? t->h >> l : 1, scale[l], 0 };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, AH.layout, 0, 1, &AH.set[l], 0, NULL);
            vkCmdPushConstants(cmd, AH.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (p.w + 7) / 8, (p.h + 7) / 8, 1);
        }
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT);
        gpu_end_once(M.vk, cmd);
    }
    for (uint32_t l = 0; l < nl; l++)
        if (views[l]) vkDestroyImageView(M.vk->device, views[l], NULL);
    gpu_buf_destroy(M.vk, &hb);
}

// BC7 (bc7.comp, notas-dev/materiais.md "BC7"): a baked RGBA8 texture and its mips compressed into a new texture, 1 byte a
// texel against 4 (memory and the reads' bandwidth a quarter); its blocks go through a buffer (copied into the image).
// cmd: recorded there (the bake's once-submit or the streaming's frame); 0 when it can't (the caller keeps the RGBA8).
// REALENGINE_NO_BC7=1 keeps RGBA8 (the comparisons)
static bool bc7_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("REALENGINE_NO_BC7") ? 0 : 1;
    return on && M.bc7;
}

static uint32_t tex_to_bc7(VkCommandBuffer cmd, uint32_t src, GpuBuf* blocks) {
    MatTex* s = &M.tex[src];
    if (!M.bc7 || !s->image || s->format != VK_FORMAT_R8G8B8A8_UNORM || (s->w & 3) || (s->h & 3) || s->mips > 16) return 0;
    uint32_t w = s->w, h = s->h, mips = s->mips;
    VkDeviceSize off[16], total = 0;
    for (uint32_t l = 0; l < mips; l++) {
        uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
        off[l] = total;
        total += (VkDeviceSize)((mw + 3) / 4) * ((mh + 3) / 4) * 16;
    }
    if (!gpu_buf_create(M.vk, blocks, total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)) return 0;
    uint32_t dst = tex_create_in(cmd, w, h, VK_FORMAT_BC7_UNORM_BLOCK);
    if (!dst) {
        gpu_buf_destroy(M.vk, blocks);
        return 0;
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.bc7);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &M.set, 0, NULL);
    VkBufferImageCopy r[16];
    for (uint32_t l = 0; l < mips; l++) {
        uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
        struct { VkDeviceAddress out; uint32_t tex, level, bw, bh, w, h; } p = { blocks->addr + off[l], src, l, (mw + 3) / 4, (mh + 3) / 4, mw, mh };
        vkCmdPushConstants(cmd, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (p.bw + 7) / 8, (p.bh + 7) / 8, 1);
        r[l] = (VkBufferImageCopy){ off[l], 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, { 0, 0, 0 }, { mw, mh, 1 } };
    }
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyBufferToImage(cmd, blocks->buffer, M.tex[dst].image, VK_IMAGE_LAYOUT_GENERAL, mips, r);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT);
    return dst;
}

// A baked variant's color and normal compressed (after its bake: the GPU idle); the RGBA8 ones buried (a frame in flight
// may still read them through a slot); the slots that share them follow (write_slot runs after for the variant's own)
static void variant_bc7(const Compiled* c, Variant* v) {
    if (!bc7_on()) return;
    bool any = false;
    for (int k = 0; k < 2; k++) {
        if (!v->tex[k] || M.tex[v->tex[k]].format != VK_FORMAT_R8G8B8A8_UNORM) continue;
        GpuBuf blk = { 0 };
        VkCommandBuffer cmd = gpu_begin_once(M.vk);
        uint32_t n = tex_to_bc7(cmd, v->tex[k], &blk);
        gpu_end_once(M.vk, cmd);
        gpu_buf_destroy(M.vk, &blk);
        if (!n) continue;
        st_bury1(v->tex[k]);
        v->tex[k] = n;
        any = true;
    }
    if (any) st_share(c, v); // (no slot keeps a buried one)
}

// (Q11) the toon and outline values per slot: a mirror of the GpuToon blocks (the renderer reads the outline's here);
// state 1 written, 2 a dynamic instance's own (a Blueprint set it: a bake of its material keeps it)
static MatToon g_slot_toon[MAT_GPU_SLOTS];
static uint8_t g_slot_toon_state[MAT_GPU_SLOTS];
static uint32_t g_outline_gen;
static void toon_slot_put(uint32_t slot, const MatToon* t) {
    if (!slot || slot >= MAT_GPU_SLOTS || !M.materials.mapped) return;
    bool was = g_slot_toon_state[slot] && g_slot_toon[slot].outline;
    if (was != t->outline) g_outline_gen++; // (the renderer finds the outlined actors again)
    g_slot_toon[slot] = *t;
    if (!g_slot_toon_state[slot]) g_slot_toon_state[slot] = 1;
    uint32_t rt = t->ramp_image[0] ? mat_source_texture(t->ramp_image) : 0u;
    GpuToon* g = &((GpuToon*)((uint8_t*)M.materials.mapped + (sizeof(GpuMaterial) + sizeof(GpuMatExt)) * MAT_GPU_SLOTS))[slot];
    toon_pack(t, rt, g->t);
}
// the values a variant's slot takes: an instance's own when it overrides them, a dynamic instance's material's, else the
// compiled material's
static const MatToon* toon_source(const Variant* v, const Compiled* c) {
    if (v >= g_inst_var && v < g_inst_var + MAT_MAX_ASSETS) {
        const Material* im = M.mats[v - g_inst_var];
        if (im && im->instance && im->toon_own) return &im->toon;
    }
    for (uint32_t i = 0; i <= M.ndyn && i < MAT_MAX_DYNAMIC; i++) // (<=: the one mat_dynamic_for is making)
        if (&M.dyn[i].v == v && M.dyn[i].m) return mat_toon_values(M.dyn[i].m);
    return &c->toon;
}
const MatToon* mat_toon_slot(uint32_t slot) { return slot && slot < MAT_GPU_SLOTS && g_slot_toon_state[slot] ? &g_slot_toon[slot] : NULL; }
uint32_t mat_outline_generation(void) { return g_outline_gen; }
bool mat_toon_slot_set(uint32_t slot, const MatToon* t) {
    if (!slot || slot >= MAT_GPU_SLOTS || !t) return false;
    toon_slot_put(slot, t);
    for (uint32_t i = 0; i < M.ndyn; i++)
        if (M.dyn[i].v.slot == slot) g_slot_toon_state[slot] = 2; // (a dynamic instance's: kept through its material's bakes)
    return true;
}

static void write_slot(const Variant* v, const Compiled* c) {
    if (!v->slot) return;
    if (g_slot_toon_state[v->slot] != 2) toon_slot_put(v->slot, toon_source(v, c)); // (Q11)
    GpuMaterial* g = &((GpuMaterial*)M.materials.mapped)[v->slot];
    memset(g, 0, sizeof *g);
    const Variant* t = v->own ? v : &c->v; // shared: the material's textures
    g->tex_base = t->tex[0];
    g->tex_nrm = t->tex[1];
    g->tex_emis = t->tex[2];
    g->flags = c->flags;
    g->param_off = v->param_off;
    g->nparams = (uint32_t)c->nparams;
    memcpy(g->avg, t->avg, sizeof g->avg);
    g->uv_scale = (c->flags & MATF_WORLD_UV) ? c->world_tile : 1.0f; // (World Aligned: cm a tile)
    GpuMatExt* x = &((GpuMatExt*)((uint8_t*)M.materials.mapped + sizeof(GpuMaterial) * MAT_GPU_SLOTS))[v->slot];
    if (c->flags & (MATF_EXT | MATF_REFRACT)) { // (a refractive glass reads its IOR, thickness and dispersion here too)
        memcpy(x->e, c->ext, sizeof x->e);
        // (M5: the detail normal's material slot, its tiling and strength, the MATX_ flags in e6; the slot and the flags as
        // float values, not bits: small integers' bits are denormals, which the GPU flushed to 0 on the load)
        x->e[6][0] = (float)c->detail_slot, x->e[6][3] = (float)c->xflags;
        if (c->xflags & MATX_LAYERS) // (B11: the layers' slots in e3, their tiles in e5 - unread without the lobes)
            for (int k = 0; k < 3; k++) x->e[3][k] = (float)c->layer_slot[k], x->e[5][k] = c->ext[5][k];
    } else memset(x, 0, sizeof *x);
    if ((c->flags & MATF_UDIM) && t->ud_table) g->anim[3] = t->ud_table; // (Q18) its tiles' table (mesh.frag udim_tile)
    else g->flags &= ~MATF_UDIM;
    if ((c->flags & MATF_TESS) && c->nframes <= 1) g->anim[2] = c->tess_mc; // (Q17: MATF_TESS's magnitude and center; a still one)
    else g->flags &= ~MATF_TESS;
    if (c->nframes > 1) { // the flipbook (instances share their material's frames)
        g->anim[0] = c->nframes;
        memcpy(&g->anim[1], &c->period, 4);
        g->anim[2] = c->table_off;
    }
}

// a half float (the emission texture's texel) as a float
static float mg_half(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 0.0f : ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

// Bakes a variant: its textures (made when missing) from the material's program with the variant's parameter values;
// the mips; the mean base color (the smallest mip read back: what the GI sees)
static bool bake_variant(const Compiled* c, Variant* v) {
    stream_cancel(v);
    if (!v->ud_tile) ud_master_origin(c, v); // (Q18: a UDIM material's own variant is its first tile)
    uint32_t res = stream_res(c, v); // (its full resolution, unless the streaming budget is short)
    int udim = ud_on(c) ? udim_of_uv(v->uv_off[0] + 0.5f, v->uv_off[1] + 0.5f) : 0, src_udim = udim ? udim : ud_lone(c);
    if (src_udim) ud_src_need(VK_NULL_HANDLE, c, src_udim, res); // (Q18: the sources' tile at the size baked; one tile too)
    if (src_udim && !udim) M.stats.udim_lone_res = res, M.stats.udim_lone_src = ud_src_res(c, src_udim); // (the udim_seams test's)
    VkFormat fm[3] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT };
    for (int k = 0; k < 3; k++) {
        bool want = k < 2 || (c->flags & MATF_EMISSIVE);
        if (v->tex[k] && (!want || M.tex[v->tex[k]].w != res || M.tex[v->tex[k]].format != fm[k]))
            st_bury1(v->tex[k]), v->tex[k] = 0; // (a frame in flight may read them; a BC7 one is baked again in RGBA8)
        if (want && !v->tex[k]) v->tex[k] = tex_create(res, res, fm[k]);
        if (want && !v->tex[k]) return false;
    }
    v->own = true;
    mip_buf_reserve(res, (c->flags & MATF_EMISSIVE) != 0); // (2026-11-23: the Kaiser mips' buffer, before recording)
    GpuBuf rb = { 0 };
    bool readback = gpu_buf_create(M.vk, &rb, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkCommandBuffer cmd = gpu_begin_once(M.vk);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.bake);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &M.set, 0, NULL);
    BakePush p = { M.programs.addr, M.params.addr + (VkDeviceAddress)v->param_off * 16u, c->bake_off, c->bake_len, res, res,
                   v->tex[0], v->tex[1], v->tex[2], c->flags, { 0, 0, 0, 0 } };
    p.udim = M.params.addr, p.uv_off[0] = v->uv_off[0], p.uv_off[1] = v->uv_off[1]; // (Q18)
    vkCmdPushConstants(cmd, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (res + 7) / 8, (res + 7) / 8, 1);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int k = 0; k < 3; k++) // (the color in sRGB; a UDIM tile clamped, a tiling material wrapped)
        if (v->tex[k]) tex_mips_hq(cmd, v->tex[k], k == 0, !ud_on(c));
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    // the color and the normal compressed in this same submit (a Masked one after its coverage, below: the CPU reads its
    // histogram between); 400 materials of 4096 took 6.8 s with a submit each, 2.8 s without BC7
    uint32_t bcn[2] = { 0, 0 };
    GpuBuf bcb[2] = { { 0 }, { 0 } };
    for (int k = 0; k < 2 && bc7_on() && !(c->flags & MATF_MASKED); k++)
        if (v->tex[k]) bcn[k] = tex_to_bc7(cmd, v->tex[k], &bcb[k]);
    if (readback) {
        MatTex* t = &M.tex[v->tex[0]];
        VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, t->mips - 1, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
        vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &r);
        if (v->tex[2]) { // (2026-11-23) the emission's mean too (RGBA16F, its smallest mip at byte 8): the GI and its light
            MatTex* te = &M.tex[v->tex[2]];
            VkBufferImageCopy re = { 8, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, te->mips - 1, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
            vkCmdCopyImageToBuffer(cmd, te->image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &re);
        }
    }
    gpu_end_once(M.vk, cmd);
    if (readback) {
        const uint8_t* px = rb.mapped;
        for (int k = 0; k < 3; k++) { // sRGB-encoded in the texture
            float s = px[k] / 255.0f;
            v->avg[k] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
        }
        const uint16_t* he = (const uint16_t*)(px + 8);
        for (int k = 0; k < 3; k++) v->avg_emis[k] = v->tex[2] ? fmaxf(mg_half(he[k]), 0.0f) : 0.0f;
        gpu_buf_destroy(M.vk, &rb);
    }
    bool swapped = false;
    for (int k = 0; k < 2; k++) {
        gpu_buf_destroy(M.vk, &bcb[k]);
        if (bcn[k]) st_bury1(v->tex[k]), v->tex[k] = bcn[k], swapped = true;
    }
    if (swapped) st_share(c, v); // (no slot keeps a buried one)
    if (c->flags & MATF_MASKED) alpha_coverage_mips(v->tex[0], c->flags >> 24); // (the cut's coverage kept in the mips)
    variant_bc7(c, v); // (a Masked one's, after its coverage: a quarter of the memory)
    v->dirty = false;
    if (udim) ud_variant_clamp(v); // (Q18) a tile's textures clamped; the source's tile back to its least; an owner's other tiles, its table
    if (src_udim) ud_src_trim(VK_NULL_HANDLE, c, src_udim);
    if (!v->ud_tile) ud_owner_sync(c, v);
    return true;
}

// The Principled BSDF's extra lobes as the material's constants (M1b; mat_input_const_only): the node feeding the Material
// Output's Surface (through reroutes; a Mix / Add Shader: none), its unlinked inputs (a linked one keeps Blender's default).
// Out: GpuMaterial.ext (mat_common.glsl ext0-3); false when nothing differs from the plain PBR (no MATF_EXT)
static float pin1(const MatGraph* g, int node, int sock) {
    const MatSockDef* s = &mat_node_def(g->nodes[node].type)->in[sock];
    return mat_graph_input_link(g, node, sock) >= 0 ? s->def[0] : g->nodes[node].in[sock][0];
}
static void pin3(const MatGraph* g, int node, int sock, float o[3]) {
    const MatSockDef* s = &mat_node_def(g->nodes[node].type)->in[sock];
    bool linked = mat_graph_input_link(g, node, sock) >= 0;
    for (int k = 0; k < 3; k++) o[k] = linked ? s->def[k] : g->nodes[node].in[sock][k];
}
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
// (Q17) a half (GLSL unpackHalf2x16): rounded, the tiny ones 0, the huge ones the largest
static uint16_t f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, m = x & 0x7FFFFFu;
    int32_t e = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7BFFu);
    uint32_t h = sign | ((uint32_t)e << 10) | (m >> 13);
    if ((m & 0x1000u) && (h & 0x7FFFu) < 0x7BFFu) h++;
    return (uint16_t)h;
}
static bool principled_ext(const MatGraph* g, float ext[8][4], bool* transmissive) {
    memset(ext, 0, sizeof(float) * 32);
    *transmissive = false;
    if (!g) return false;
    int out = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    int li = out >= 0 ? follow_link(g, mat_graph_input_link(g, out, 0)) : -1;
    if (li >= 0 && g->nodes[g->links[li].from].type == NODE_RE_INTERIOR) li = follow_link(g, mat_graph_input_link(g, g->links[li].from, 0)); // (Q4: through the Interior Mapping)
    if (li >= 0 && g->nodes[g->links[li].from].type == NODE_RE_WET) li = follow_link(g, mat_graph_input_link(g, g->links[li].from, 0)); // (through the Wet Surface)
    if (li < 0) return false;
    int p = g->links[li].from;
    if (g->nodes[p].type != NODE_BSDF_PRINCIPLED) return false;
    // the dielectric's F0: Blender 4's ((IOR - 1) / (IOR + 1))^2 times 2 x Specular IOR Level, tinted
    float ior = pin1(g, p, 3), level = clamp01(pin1(g, p, 12)), tint[3];
    pin3(g, p, 13, tint);
    if (ior < 1.0f) ior = 1.0f;
    float r = (ior - 1.0f) / (ior + 1.0f), f0 = r * r * 2.0f * level;
    for (int k = 0; k < 3; k++) ext[2][k] = f0 * (tint[k] < 0.0f ? 0.0f : tint[k]);
    // (close to the plain 0.04, as Blender's old 1.45 default gives 0.034: not a reason for the RE_EXT pipelines on its own)
    bool f0_plain = fabsf(ext[2][0] - 0.04f) < 0.01f && fabsf(ext[2][1] - 0.04f) < 0.01f && fabsf(ext[2][2] - 0.04f) < 0.01f;
    float tr = clamp01(pin1(g, p, 17));
    ext[2][3] = tr;
    *transmissive = tr > 0.001f;
    // the coat: its weight, roughness (as alpha), F0 by its IOR, tint
    float cw = clamp01(pin1(g, p, 18)), cr = clamp01(pin1(g, p, 19)), cior = pin1(g, p, 20), ct[3];
    pin3(g, p, 21, ct);
    if (cior < 1.0f) cior = 1.0f;
    float cr0 = (cior - 1.0f) / (cior + 1.0f);
    if (cr < 0.045f) cr = 0.045f;
    ext[0][0] = cw, ext[0][1] = cr * cr, ext[0][2] = cr0 * cr0;
    for (int k = 0; k < 3; k++) ext[1][k] = clamp01(ct[k]);
    // anisotropy (Blender's 0..1 stretches along the tangent) and its rotation (0..1: a turn)
    ext[0][3] = clamp01(pin1(g, p, 14));
    ext[1][3] = pin1(g, p, 15) * 6.2831853f;
    // subsurface: the light wraps past the terminator per channel by the radius (normalized) and the scale (Blender's
    // 5 cm default: half a wrap on the reddest channel of skin)
    float sw = clamp01(pin1(g, p, 7)), rad[3], sc = pin1(g, p, 9);
    pin3(g, p, 8, rad);
    float rmax = fmaxf(rad[0], fmaxf(rad[1], rad[2]));
    float amount = clamp01(sc / 0.05f * 0.5f);
    for (int k = 0; k < 3; k++) ext[3][k] = rmax > 0.0f ? clamp01(rad[k] / rmax) * amount : 0.0f;
    ext[3][3] = sw;
    // (A9.3a) the screen space scattering's Burley distances (sss.comp): the widest channel's in cm (Radius x Scale, m),
    // the channels' shares of it in 8 bits each, a float value (exact: under 2^24)
    float dmax = rmax * (sc > 0.0f ? sc : 0.0f) * 100.0f;
    if (sw > 0.001f && dmax > 0.0f) {
        uint32_t q = 0;
        for (int k = 0; k < 3; k++) q = q << 8 | (uint32_t)(clamp01(rad[k] / rmax) * 255.0f + 0.5f);
        ext[7][2] = dmax > 50.0f ? 50.0f : dmax;
        ext[7][3] = (float)q;
    }
    // sheen (M3: cloth, velvet, dust; Blender's Charlie lobe) and a thin film's colors (M4: soap, oil, a coated lens)
    float shw = clamp01(pin1(g, p, 23)), shr = clamp01(pin1(g, p, 24)), sht[3];
    pin3(g, p, 25, sht);
    float tf = pin1(g, p, 28), tfior = pin1(g, p, 29);
    ext[4][0] = shw, ext[4][1] = shr < 0.07f ? 0.07f : shr, ext[4][2] = tf < 0.0f ? 0.0f : tf > 3000.0f ? 3000.0f : tf;
    ext[4][3] = tfior < 1.0f ? 1.0f : tfior;
    ext[5][3] = ior; // (2026-11-16: the glass's refraction, mesh.frag RE_TRANSLUCENT)
    for (int k = 0; k < 3; k++) ext[5][k] = clamp01(sht[k]);
    return cw > 0.001f || ext[0][3] > 0.001f || sw > 0.001f || !f0_plain || *transmissive || shw > 0.001f || ext[4][2] > 1.0f;
}

// (2026-11-16) The Wet Surface on the Material Output's Surface (through reroutes): its values in e8-e9, the mask in e11;
// a pin linked to a Value node with a Parameter Name reads that parameter at run time (e10: its index + 1, added to the
// material's parameters when the bake doesn't read it). False: no Wet Surface
static int wet_node(const MatGraph* g) {
    if (!g) return -1;
    int out = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    int li = out >= 0 ? follow_link(g, mat_graph_input_link(g, out, 0)) : -1;
    return li >= 0 && g->nodes[g->links[li].from].type == NODE_RE_WET ? g->links[li].from : -1;
}
static bool wet_ext(const MatGraph* g, Ir* ir, float ext[13][4]) {
    memset(ext[8], 0, sizeof(float) * 16);
    int w = wet_node(g);
    if (w < 0) return false;
    ext[8][0] = clamp01(pin1(g, w, 1));
    float por = pin1(g, w, 2);
    ext[8][1] = por < 0.0f ? -1.0f : clamp01(por);
    ext[8][2] = clamp01(pin1(g, w, 3));
    float fr = pin1(g, w, 4);
    fr = fr < 0.02f ? 0.02f : fr > 1.0f ? 1.0f : fr;
    ext[8][3] = fr * fr;
    ext[9][0] = clamp01(pin1(g, w, 5));
    float ds = pin1(g, w, 6);
    ext[9][1] = ds < 0.3f ? 0.3f : ds > 20.0f ? 20.0f : ds;
    ext[9][2] = clamp01(pin1(g, w, 7)), ext[9][3] = clamp01(pin1(g, w, 8));
    int mask = g->nodes[w].prop[0];
    ext[11][0] = (float)(mask >= 0 && mask < MAT_WET_MASK_COUNT ? mask : 0);
    static const int ps[4] = { 1, 3, 5, 8 }; // (the ones a Blueprint drives: Wetness, Water Film, Droplets, Sweat)
    for (int k = 0; k < 4; k++) {
        int li = follow_link(g, mat_graph_input_link(g, w, ps[k]));
        const MatNode* v = li >= 0 ? &g->nodes[g->links[li].from] : NULL;
        if (!v || v->type != NODE_VALUE) continue;
        if (!v->param[0]) { // (a plain Value: its number)
            float c = clamp01(v->value[0]);
            if (k == 0) ext[8][0] = c;
            else if (k == 1) ext[8][2] = c;
            else if (k == 2) ext[9][0] = c;
            else ext[9][3] = c;
            continue;
        }
        int pi = param_index(ir, v, 0);
        if (pi >= 0) ext[10][k] = (float)(pi + 1);
    }
    return true;
}

// (2026-11-17, Q4) Transmission: the Principled BSDF on the Material Output (as principled_ext finds it); its Transmission
// Weight a constant, a plain Value node's number, or a Value with a Parameter Name (an instance's or a Blueprint's value
// at run time: its index + 1 in *param, read by mesh.frag; the bake doesn't read it). The weight; -1: no Principled
static float transmission_of(const MatGraph* g, Ir* ir, int* param, float base[3]) {
    *param = 0;
    base[0] = base[1] = base[2] = 1.0f;
    if (!g) return -1.0f;
    int out = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    int li = out >= 0 ? follow_link(g, mat_graph_input_link(g, out, 0)) : -1;
    if (li >= 0 && g->nodes[g->links[li].from].type == NODE_RE_INTERIOR) li = follow_link(g, mat_graph_input_link(g, g->links[li].from, 0));
    if (li >= 0 && g->nodes[g->links[li].from].type == NODE_RE_WET) li = follow_link(g, mat_graph_input_link(g, g->links[li].from, 0));
    if (li < 0 || g->nodes[g->links[li].from].type != NODE_BSDF_PRINCIPLED) return -1.0f;
    int p = g->links[li].from;
    if (mat_graph_input_link(g, p, 0) < 0) pin3(g, p, 0, base); // (its Base Color: the light through it takes that color)
    int vl = follow_link(g, mat_graph_input_link(g, p, 17));
    if (vl < 0) return clamp01(g->nodes[p].in[17][0]);
    const MatNode* v = &g->nodes[g->links[vl].from];
    if (v->type != NODE_VALUE) return 0.0f; // (a texture or a math: not a per material value, as before)
    if (v->param[0]) {
        int pi = param_index(ir, v, 0);
        if (pi >= 0) *param = pi + 1;
    }
    return clamp01(v->value[0]);
}

// (2026-11-17, Q4) The Interior Mapping on the Material Output's Surface: its values in e8-e10 and e7 (mat_common.glsl), the
// Night's parameter index + 1 in e11.x (a Value with a Parameter Name). False: none
static bool interior_ext(const MatGraph* g, Ir* ir, float ext[13][4]) {
    if (!g) return false;
    int out = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    int li = out >= 0 ? follow_link(g, mat_graph_input_link(g, out, 0)) : -1;
    if (li < 0 || g->nodes[g->links[li].from].type != NODE_RE_INTERIOR) return false;
    int n = g->links[li].from;
    float lc[3];
    pin3(g, n, 9, lc);
    float ru = pin1(g, n, 1), rv = pin1(g, n, 2), dp = pin1(g, n, 3), fr = pin1(g, n, 4);
    ext[8][0] = ru < 1.0f ? 1.0f : ru, ext[8][1] = rv < 1.0f ? 1.0f : rv, ext[8][2] = dp < 0.1f ? 0.1f : dp > 4.0f ? 4.0f : dp;
    ext[8][3] = fr < 0.0f ? 0.0f : fr > 0.45f ? 0.45f : fr;
    ext[9][0] = clamp01(pin1(g, n, 5)), ext[9][1] = clamp01(pin1(g, n, 6)), ext[9][2] = clamp01(pin1(g, n, 7)), ext[9][3] = clamp01(pin1(g, n, 8));
    for (int k = 0; k < 3; k++) ext[10][k] = lc[k] < 0.0f ? 0.0f : lc[k];
    float ls = pin1(g, n, 10);
    ext[10][3] = ls < 0.0f ? 0.0f : ls;
    ext[11][0] = 0.0f;
    int vl = follow_link(g, mat_graph_input_link(g, n, 7)); // the Night: a parameter
    if (vl >= 0 && g->nodes[g->links[vl].from].type == NODE_VALUE) {
        const MatNode* v = &g->nodes[g->links[vl].from];
        ext[9][2] = clamp01(v->value[0]);
        if (v->param[0]) {
            int pi = param_index(ir, v, 0);
            if (pi >= 0) ext[11][0] = (float)(pi + 1);
        }
    }
    extern uint32_t mat_source_texture(const char* path);
    const MatNode* nd = &g->nodes[n];
    ext[7][0] = nd->image[0] ? (float)mat_source_texture(nd->image) : 0.0f; // (the atlas; 0: procedural rooms)
    ext[7][1] = (float)(nd->prop[0] >= 0 ? nd->prop[0] + 1 : 1), ext[7][2] = (float)(nd->prop[1] >= 0 ? nd->prop[1] + 1 : 1); // (the props: 0 = 1)
    ext[7][3] = pin1(g, n, 11);
    return true;
}

static bool bake(Material* m) {
    int mi = mat_index(m);
    if (mi < 0 || !m->graph) return false;
    double t0 = re_time_seconds();
    static Ir ir;
    Built b;
    m->error[0] = 0;
    bool ok = compile_material(m, &ir, &b);
    m->compile_ms = (re_time_seconds() - t0) * 1000.0;
    if (!ok) {
        snprintf(m->error, sizeof m->error, "%s", ir.err[0] ? ir.err : "compile failed");
        free(b.bake.w);
        free(b.run.w);
        return false;
    }
    vkDeviceWaitIdle(M.vk->device); // the old textures may still be read by a frame
    stream_cancel(NULL); // (the program buffer may move: a transition in flight is dropped)
    st_drain(true);
    S.hstale = true;
    Compiled* c = &M.comp[mi];
    c->res = (uint32_t)(m->resolution >= 64 && m->resolution <= 4096 ? m->resolution : 1024);
    c->flags = (b.emissive ? MATF_EMISSIVE : 0u) | (m->world_grid ? MATF_WORLD_GRID : 0u) | (m->two_sided ? MATF_TWO_SIDED : 0u);
    if (m->world_aligned) c->flags |= MATF_WORLD_UV; // (its tile: write_slot puts it in the uv scale)
    ud_compile(m, c); // (Q18) its UDIM images' tiles: MATF_UDIM
    c->world_tile = m->world_tile > 1.0f ? m->world_tile : 200.0f;
    if (m->blend_mode == MAT_BLEND_MASKED) {
        float clip = m->opacity_clip < 0.0f ? 0.0f : m->opacity_clip > 1.0f ? 1.0f : m->opacity_clip;
        c->flags |= MATF_MASKED | (uint32_t)(clip * 255.0f + 0.5f) << 24;
    }
    if (m->blend_mode == MAT_BLEND_TRANSLUCENT) c->flags |= MATF_TRANSLUCENT; // (the baked alpha: the opacity, mesh.frag RE_TRANSLUCENT)
    // the Principled BSDF's extra lobes (coat, anisotropy, subsurface, specular tint / IOR, transmission: M1b)
    m->ext_lobes = principled_ext(m->graph, c->ext, &m->transmissive);
    // (2026-11-10) a Masked material keeps its cut by the Alpha: its Transmission doesn't make it glass (hair, leaves:
    // the two fought, the OIT pass drawing the cut parts), it stays a lobe the light wraps through
    if (m->blend_mode == MAT_BLEND_MASKED) m->transmissive = false;
    m->subsurface = m->ext_lobes && c->ext[3][3] > 0.001f && c->ext[7][2] > 0.0f; // (A9.3a: render.c's GI_SSS)
    if (m->shading_model == MAT_SHADING_EYE && m->blend_mode == MAT_BLEND_OPAQUE) {
        // the Eye (A9.3c): the cornea a clear coat over the whole eye (the wet surface: its own normal, the sphere's;
        // F0 by IOR 1.376), the base without a specular of its own (the iris and the sclera under it); the iris's radius
        // and depth in e7 (mesh.frag MATF_EYE: the refraction, the dish)
        float r0 = (1.376f - 1.0f) / (1.376f + 1.0f);
        c->ext[0][0] = 1.0f, c->ext[0][1] = 0.0025f, c->ext[0][2] = r0 * r0, c->ext[0][3] = 0.0f;
        c->ext[1][0] = c->ext[1][1] = c->ext[1][2] = 1.0f, c->ext[1][3] = 0.0f;
        c->ext[2][0] = c->ext[2][1] = c->ext[2][2] = 0.0f;
        c->ext[7][0] = m->eye_iris_radius > 0.01f ? m->eye_iris_radius : 0.25f;
        c->ext[7][1] = m->eye_iris_depth >= 0.0f ? (m->eye_iris_depth > 2.0f ? 2.0f : m->eye_iris_depth) : 0.25f;
        c->flags |= MATF_EYE;
        m->ext_lobes = true;
        if (m->eye_glow > 0.0f) { // (Q13) Eye Glow in e5.w (mesh.frag g_retro): its strength x16 in the top bits, RGB565 below
            uint32_t k = (uint32_t)fminf(m->eye_glow * 16.0f + 0.5f, 255.0f), cr[3];
            const uint32_t mx[3] = { 31u, 63u, 31u };
            for (int i = 0; i < 3; i++) cr[i] = (uint32_t)(fminf(fmaxf(m->eye_glow_color[i], 0.0f), 1.0f) * (float)mx[i] + 0.5f);
            c->ext[5][3] = (float)(k << 16 | cr[0] << 11 | cr[1] << 5 | cr[2]);
        }
    }
    // (Q27.8) a retroreflector on any surface but the Eye (its iris has Eye Glow in the same place: e5.w, mesh.frag g_retro);
    // metallic flakes in e12 (mesh.frag flake_glint): the lobes' pipelines
    c->ext[12][0] = c->ext[12][1] = c->ext[12][2] = c->ext[12][3] = 0.0f;
    if (m->shading_model != MAT_SHADING_EYE && m->shading_model != MAT_SHADING_TOON && m->retro > 0.0f) {
        uint32_t k = (uint32_t)fminf(m->retro * 16.0f + 0.5f, 255.0f), cr[3];
        const uint32_t mx[3] = { 31u, 63u, 31u };
        for (int i = 0; i < 3; i++) cr[i] = (uint32_t)(fminf(fmaxf(m->retro_color[i], 0.0f), 1.0f) * (float)mx[i] + 0.5f);
        c->ext[5][3] = (float)(k << 16 | cr[0] << 11 | cr[1] << 5 | cr[2]);
        m->ext_lobes = true;
    }
    if (m->shading_model != MAT_SHADING_TOON && m->flakes > 0.0f) {
        c->ext[12][0] = fminf(m->flakes, 4.0f), c->ext[12][1] = fmaxf(m->flake_size, 0.002f), c->ext[12][2] = fminf(fmaxf(m->flake_spread, 0.0f), 1.0f);
        m->ext_lobes = true;
    }
    c->toon = m->toon; // (Q11) its toon and outline values (its slots' blocks: write_slot)
    if (m->shading_model == MAT_SHADING_TOON && m->blend_mode != MAT_BLEND_TRANSLUCENT) {
        // the Toon shading model: lit in bands by its ramp (mesh.frag toon_shade), none of the Principled's lobes
        // (g_ext stays 0: the toon path replaces the BRDF), the RE_EXT pipelines
        m->ext_lobes = false, m->subsurface = false;
        c->flags |= MATF_TOON;
    }
    if (!m->ext_lobes && c->ext[2][0] == 0.0f && c->ext[2][1] == 0.0f && c->ext[2][2] == 0.0f) // (no Principled: the plain F0)
        c->ext[2][0] = c->ext[2][1] = c->ext[2][2] = 0.04f;
    // M5: the Color Attribute multiplying the Base Color, a detail normal (the RE_EXT pipelines; without the lobes the plain BRDF)
    m->vcol = mat_vcol_usage(m->graph);
    c->xflags = (m->ext_lobes ? MATX_LOBES : 0u) | (m->vcol == MAT_VCOL_TINT ? MATX_VCOL_TINT : 0u);
    c->nbaked = ir.nparams; // (the bake's parameters; the Wet Surface's own ones after them)
    m->wet = m->blend_mode != MAT_BLEND_TRANSLUCENT && wet_ext(m->graph, &ir, c->ext) && !getenv("REALENGINE_NO_WET"); // (2026-11-16; the env: to measure without)
    if (m->wet) c->xflags |= MATX_WET;
    { // (2026-11-17, Q4) Transmission's mode, parameter, absorption and colored shadow (mat_common.glsl e8, e9, e11)
        int tparam = 0;
        float tbase[3];
        float tw = transmission_of(m->graph, &ir, &tparam, tbase);
        if (m->blend_mode != MAT_BLEND_MASKED && (tw > 0.001f || tparam)) {
            if (!m->wet) memset(c->ext[8], 0, sizeof(float) * 16);
            if (m->transmission_mode == MAT_TRANS_THIN) { // opaque, lit through from behind (mesh.frag brdf_base)
                m->transmissive = false;
                c->ext[2][3] = 0.0f;
                c->ext[11][1] = tw, c->ext[11][2] = (float)tparam, c->ext[11][3] = 1.0f;
                m->ext_lobes = true;
                c->xflags |= MATX_LOBES;
            } else { // glass: the OIT pass (the absorption per cm, the parameter's index; the shadow's tint)
                m->transmissive = true;
                c->ext[2][3] = tw;
                float d = m->absorption_dist > 0.1f ? m->absorption_dist : 0.1f;
                for (int k = 0; k < 3; k++) {
                    float a = m->absorption[k] < 0.001f ? 0.001f : m->absorption[k] > 1.0f ? 1.0f : m->absorption[k];
                    c->ext[8][k] = -logf(a) / d;
                    // (the shadow: what a pane lets through, its base color's mean times the absorption through ~its thickness)
                    float th = m->refr_thickness > 0.1f ? m->refr_thickness : 1.0f;
                    float bc = m->avg_base[0] + m->avg_base[1] + m->avg_base[2] > 0.0f ? m->avg_base[k] : tbase[k];
                    c->ext[9][k] = clamp01(bc) * expf(-c->ext[8][k] * th);
                }
                c->ext[8][3] = (float)tparam;
                c->ext[9][3] = m->colored_shadow ? 1.0f : 0.0f;
                c->ext[11][3] = 0.0f;
            }
        } else if (!m->wet) memset(c->ext[8], 0, sizeof(float) * 16);
    }
    // (2026-11-17, Q4) the Interior Mapping: rooms behind the windows (not on glass: it is drawn opaque, the glass its own)
    m->interior = m->blend_mode != MAT_BLEND_TRANSLUCENT && !m->transmissive && !getenv("REALENGINE_NO_INTERIOR") && interior_ext(m->graph, &ir, c->ext); // (the env: the measure's "before")
    if (m->interior) c->xflags |= MATX_INTERIOR;
    c->detail_hash = m->detail_path[0] ? mat_vpath_hash(m->detail_path) : 0u;
    c->detail_slot = 0, c->detail_tried = false; // (mat_update finds its slot)
    c->layer_tried = false; // (B11: the painted layers, found as the detail)
    for (int k = 0; k < 3; k++) {
        c->layer_hash[k] = m->layer_path[k][0] ? mat_vpath_hash(m->layer_path[k]) : 0u, c->layer_slot[k] = 0;
        if (c->layer_hash[k]) c->xflags |= MATX_LAYERS;
        c->ext[5][k] = m->layer_tile[k] >= 1.0f ? m->layer_tile[k] : 400.0f;
    }
    if (c->xflags & MATX_LAYERS) c->xflags &= ~MATX_LOBES; // (a landscape's layers take e3 / e5: its own extra lobes off)
    c->ext[6][1] = m->detail_tiling > 0.01f ? m->detail_tiling : 8.0f;
    c->ext[6][2] = m->detail_strength < 0.0f ? 0.0f : m->detail_strength > 4.0f ? 4.0f : m->detail_strength;
    // M7: parallax occlusion (the Displacement baked into the base's alpha), its depth and most steps in e7
    // (Q17) Nanite Tessellation in its place: the same height moves its DAG clusters' surface for real (tess.glsl), the
    // magnitude and center in GpuMaterial.anim[2] (two halfs; anim[3] is the UDIM's table); an opaque still one (a flipbook's height changes per frame)
    m->tess_active = m->tessellation && b.pom_depth > 0.0f && m->blend_mode == MAT_BLEND_OPAQUE && !(c->flags & MATF_EYE) &&
                     !(c->flags & MATF_WORLD_UV) && fabsf(m->disp_magnitude) > 1e-4f;
    c->tess_mc = 0u;
    if (m->tess_active) {
        float mg = m->disp_magnitude < -1000.0f ? -1000.0f : m->disp_magnitude > 1000.0f ? 1000.0f : m->disp_magnitude;
        c->flags |= MATF_TESS, c->tess_mc = (uint32_t)f32_to_f16(mg) | ((uint32_t)f32_to_f16(clamp01(m->disp_center)) << 16);
    }
    m->parallax = b.pom_depth > 0.0f && !m->tess_active;
    if (m->parallax && !(c->flags & MATF_EYE)) c->flags |= MATF_POM, c->ext[7][0] = b.pom_depth > 0.5f ? 0.5f : b.pom_depth, c->ext[7][1] = 32.0f;
    if (m->ext_lobes || c->xflags || c->detail_hash || (c->flags & (MATF_POM | MATF_TOON))) c->flags |= MATF_EXT;
    if (c->detail_hash || (c->xflags & MATX_LAYERS)) M.any_detail = true; // (B11: the layers followed the same way)
    if (m->transmissive) c->flags |= MATF_TRANSLUCENT; // (glass: the OIT pass, its opacity 1 - transmission on a dielectric)
    // (2026-11-16) refraction: its thickness, dispersion and strength in e7 (a glass has no parallax, eye or skin there)
    if ((c->flags & MATF_TRANSLUCENT) && m->refraction > 0.0f && !(c->flags & (MATF_POM | MATF_EYE)) && !m->subsurface) {
        c->flags |= MATF_REFRACT;
        c->ext[7][0] = m->refr_thickness > 0.0f ? (m->refr_thickness > 200.0f ? 200.0f : m->refr_thickness) : 0.0f;
        c->ext[7][1] = m->dispersion < 0.0f ? 0.0f : m->dispersion > 1.0f ? 1.0f : m->dispersion;
        c->ext[7][2] = m->refraction > 4.0f ? 4.0f : m->refraction;
    }
    // the bake program into the program buffer (append; a full buffer starts over)
    if (M.prog_used + b.bake.n > MAT_PROG_WORDS) M.prog_used = 0;
    c->bake_off = M.prog_used;
    c->bake_len = b.bake.n;
    if (b.bake.n) memcpy((uint32_t*)M.programs.mapped + c->bake_off, b.bake.w, b.bake.n * 4);
    M.prog_used += b.bake.n;
    c->nparams = ir.nparams;
    for (int i = 0; i < ir.nparams; i++) {
        snprintf(c->pnames[i], MAT_NAME, "%s", ir.pnames[i]);
        memcpy(c->pdefault[i], ir.pdef[i], 16);
    }
    // the material lists its parameters (the instances' editor shows them; the values set keep theirs)
    MatParam keep[MAT_MAX_PARAMS];
    int nkeep = m->nparams;
    memcpy(keep, m->params, sizeof keep);
    m->nparams = 0;
    for (int i = 0; i < ir.nparams; i++) {
        MatParam* q = &m->params[m->nparams++];
        snprintf(q->name, sizeof q->name, "%s", ir.pnames[i]);
        memcpy(q->value, ir.pdef[i], 16);
        q->kind = 1;
        for (int k = 0; k < nkeep; k++)
            if (!strcmp(keep[k].name, q->name)) q->kind = keep[k].kind;
    }
    if (!c->v.slot) c->v.slot = slot_alloc();
    c->v.param_off = params_alloc((uint32_t)c->nparams);
    fill_params(c->v.param_off, c, m, NULL);
    // animated: frame k bakes with __time = k / frames of the period (frame 0 is the material's own variant)
    int time_param = -1;
    for (int i = 0; i < c->nparams; i++)
        if (!strcmp(c->pnames[i], "__time")) time_param = i;
    uint32_t had = c->nframes > c->anim_next ? c->nframes : c->anim_next; // (a flipbook still being baked: its frames so far)
    for (uint32_t k = 1; k < had; k++) variant_free(&c->frames[k]);
    c->nframes = 0, c->anim_next = c->anim_total = 0;
    m->animated = time_param >= 0, m->anim_period = ir.anim_period;
    c->animated = m->animated;
    if (m->animated && m->tess_active) m->tess_active = false, c->flags &= ~MATF_TESS, c->tess_mc = 0u; // (Q17: a flipbook's height changes per frame)
    uint32_t still_res = c->res;
    if (m->animated) {
        uint32_t nf = (uint32_t)(m->anim_frames >= 2 && m->anim_frames <= MAT_ANIM_MAX ? m->anim_frames : 16);
        c->res = (uint32_t)(m->anim_res >= 64 && m->anim_res <= 2048 ? m->anim_res : 512);
        c->period = ir.anim_period > 0.05f ? ir.anim_period : 2.0f;
        c->table_off = params_alloc(nf);
        c->time_param = time_param;
        // frame 0 now (shown still); the rest a few per editor frame (anim_continue: 16 frames took 65-130 ms at once)
        param_vals()[c->v.param_off + (uint32_t)time_param][0] = 0.0f;
        bool fok = bake_variant(c, &c->v);
        {
            float* row = param_vals()[c->table_off];
            uint32_t ids[4] = { c->v.tex[0], c->v.tex[1], c->v.tex[2], 0 };
            memcpy(row, ids, 16);
        }
        c->nframes = 0;
        c->anim_next = fok ? 1 : 0, c->anim_total = fok ? nf : 0;
        if (fok) M.any_dirty = true; // (mat_update comes back for the rest)
        ok = fok;
        // the material's parameter list leaves the hidden time out
        for (int i = 0; i < m->nparams;)
            if (!strncmp(m->params[i].name, "__", 2)) m->params[i] = m->params[--m->nparams];
            else i++;
    } else ok = bake_variant(c, &c->v);
    (void)still_res;
    c->valid = ok;
    m->slot = c->v.slot;
    memcpy(m->avg_base, c->v.avg, sizeof m->avg_base);
    memcpy(m->avg_emis, c->v.avg_emis, sizeof m->avg_emis);
    write_slot(&c->v, c);
    m->bake_ms = (re_time_seconds() - t0) * 1000.0;
    m->ninstr = (uint32_t)b.ninstr, m->nstatic = (uint32_t)b.nstatic, m->ndynamic = 0, m->nslots = 0;
    M.stats.bake_ms_total += m->bake_ms;
    printf("[RealEngine] Material %s: %d instructions, %d parameters, %ux%u, %.1f ms\n", m->name, b.ninstr, ir.nparams, c->res,
           c->res, m->bake_ms);
    free(b.bake.w);
    free(b.run.w);
    return ok;
}

// A Material Instance (.rmi): its own textures when its parent has parameters to change, else the parent's
static void update_instance(Material* m) {
    Material* p = m->parent;
    int pi = p && !p->instance ? mat_index(p) : -1, mi = mat_index(m);
    if (pi < 0 || mi < 0) return;
    if (p->dirty || !M.comp[pi].valid) bake(p), p->dirty = false;
    Compiled* c = &M.comp[pi];
    Variant* v = &g_inst_var[mi];
    if (!v->slot) v->slot = slot_alloc();
    if (c->nparams > 0) {
        v->param_off = params_alloc((uint32_t)c->nparams);
        fill_params(v->param_off, c, p, m);
        bake_variant(c, v);
    } else {
        variant_free(v);
        memcpy(v->avg, c->v.avg, sizeof v->avg);
        memcpy(v->avg_emis, c->v.avg_emis, sizeof v->avg_emis);
    }
    m->slot = v->slot;
    memcpy(m->avg_base, v->own ? v->avg : c->v.avg, sizeof m->avg_base);
    memcpy(m->avg_emis, v->own ? v->avg_emis : c->v.avg_emis, sizeof m->avg_emis);
    write_slot(v, c);
    S.hstale = true; // (its own textures or its parent's: where its need goes)
}

static Compiled* compiled_of(const Material* m) {
    const Material* base = m->instance ? m->parent : m;
    int i = base ? mat_index(base) : -1;
    return i >= 0 && M.comp[i].valid ? &M.comp[i] : NULL;
}

uint32_t mat_slot_of(Entity e, Material* m) {
    for (uint32_t i = 0; i < M.ndyn; i++)
        if (M.dyn[i].e == e && M.dyn[i].m == m) return M.dyn[i].v.slot;
    return m->slot;
}

// --- The images decoded in the background (async.h) ---
// A dirty material waits (stays dirty, the old textures drawn) until a worker decoded its images; then it bakes on the
// main thread, a few per frame (MAT_FRAME_MS), with a card at the bottom right. The loading screen and the explicit
// calls (mat_update without a command buffer) still bake everything at once.
#define IMG_JOBS 128
#define MAT_FRAME_MS 6.0
typedef struct ImgJob { char full[MAT_PATH * 2]; uint8_t* px; uint32_t w, h; int state; bool orphan; } ImgJob; // 1 decoding, 2 ready, 3 failed
static ImgJob* g_img[IMG_JOBS];
static struct { uint32_t track; int done; char cur[64]; double t0; uint32_t decoded; } MB;

static void img_work(AsyncTask* t, void* arg) {
    (void)t;
    ImgJob* j = arg;
    j->px = mat_load_image_rgba(j->full, &j->w, &j->h);
}
static void img_done(AsyncTask* t, void* arg, bool ok) { // (main thread: the state read by the bakes)
    (void)t, (void)ok;
    ImgJob* j = arg;
    j->state = j->px ? 2 : 3;
    MB.decoded++;
    if (j->orphan) { // (its material baked meanwhile without it: decoded twice, this one dropped)
        for (int i = 0; i < IMG_JOBS; i++)
            if (g_img[i] == j) g_img[i] = NULL;
        free(j->px), free(j);
    }
}
static ImgJob* img_find(const char* full) {
    for (int i = 0; i < IMG_JOBS; i++)
        if (g_img[i] && !_stricmp(g_img[i]->full, full)) return g_img[i];
    return NULL;
}
// 0: decoding (asked now if it wasn't), 1: ready or nothing to wait for
static int img_ready(const char* full) {
    ImgJob* j = img_find(full);
    if (j) return j->state != 1;
    int slot = -1;
    for (int i = 0; i < IMG_JOBS && slot < 0; i++)
        if (!g_img[i]) slot = i;
    if (slot < 0) return 1; // (full: decoded on the main thread when baked)
    j = calloc(1, sizeof *j);
    if (!j) return 1;
    snprintf(j->full, sizeof j->full, "%s", full);
    j->state = 1;
    g_img[slot] = j;
    async_run(TR("Decoding a texture"), img_work, img_done, j, ASYNC_HIDDEN);
    return j->state != 1; // (no workers: it ran right here)
}
uint8_t* img_take(const char* full, uint32_t* w, uint32_t* h) {
    ImgJob* j = img_find(full);
    if (!j) return NULL;
    if (j->state == 1) { // still decoding: this bake decodes it itself (asked by a loading screen or a sync call)
        j->orphan = true;
        return NULL;
    }
    for (int i = 0; i < IMG_JOBS; i++)
        if (g_img[i] == j) g_img[i] = NULL;
    uint8_t* px = j->px;
    *w = j->w, *h = j->h;
    free(j);
    return px;
}
static void source_full(const char* path, char* full, size_t cap) {
    if (!strncmp(path, "/Content", 8)) snprintf(full, cap, "%s%s", M.root, path + 8);
    else snprintf(full, cap, "%s", path);
}
// its images on the GPU or decoded (the ones missing asked from the workers)
static bool images_ready(const Material* m) {
    const Material* base = m->instance ? m->parent : m;
    if (!base || !base->graph) return true;
    bool ok = true;
    for (int i = 0; i < base->graph->node_count; i++) {
        const MatNode* n = &base->graph->nodes[i];
        if (n->type != NODE_TEX_IMAGE || !n->image[0]) continue;
        char full[MAT_PATH * 2];
        source_full(n->image, full, sizeof full);
        if (udim_is_path(full)) { // (Q18) an asset is mapped (no wait); a pattern's tiles decoded by the workers
            bool known = false;
            for (uint32_t k = 0; k < g_nuds && !known; k++) known = g_uds[k] && !_stricmp(g_uds[k]->full, full);
            size_t fl = strlen(full);
            if (known || (fl > 6 && !_stricmp(full + fl - 6, ".rudim"))) continue;
            int tl[UDIM_MAX_TILES], nt = udim_find_tiles(full, tl, UDIM_MAX_TILES);
            for (int k = 0; k < nt; k++) {
                char tp[MAT_PATH * 2];
                udim_tile_path(full, tl[k], tp, sizeof tp);
                bool loaded = false;
                for (uint32_t q = 1; q < MAT_TEX_MAX && !loaded; q++) loaded = M.tex[q].image && !_stricmp(M.tex[q].source, tp);
                if (!loaded && !img_ready(tp)) ok = false;
            }
            continue;
        }
        bool loaded = false;
        for (uint32_t k = 1; k < MAT_TEX_MAX && !loaded; k++) loaded = M.tex[k].image && !_stricmp(M.tex[k].source, full);
        if (!loaded && !img_ready(full)) ok = false;
    }
    return ok;
}
static int dirty_count(void) {
    int n = 0;
    for (uint32_t i = 0; i < M.n; i++) n += M.mats[i]->dirty || (M.comp[i].anim_next && M.comp[i].anim_next < M.comp[i].anim_total);
    return n;
}
// (Q22) the project's materials still compiling in the background (the editor's start: every .rmat of the content, a
// slice a frame, one bake up to ~0.2 s): the measures wait for them as they wait for the pipelines (main.c --bench, the
// World Partition's and PCG's hitch counts)
bool mat_compiling(void) { return M.any_dirty && dirty_count() > 0; }
int mat_compiling_count(void) { return M.any_dirty ? dirty_count() : 0; } // (Q26: the status bar's Shader Compile)
// the card: how many are left (the poll ends it when none is)
static float mat_poll(void* arg, char* st, int cap) {
    (void)arg;
    int left = M.any_dirty ? dirty_count() : 0;
    if (!left) {
        MB.track = 0, MB.done = 0;
        return -1.0f;
    }
    int total = MB.done + left;
    if (MB.cur[0]) snprintf(st, (size_t)cap, TR("%s (%d of %d)"), MB.cur, MB.done + 1, total);
    else snprintf(st, (size_t)cap, "%s", TR("Decoding the textures..."));
    return (float)MB.done / (float)total;
}

static bool anim_continue(bool all, double t0);

void mat_bake_pending(void (*progress)(int done, int total, const char* name)) {
    if (!M.ready || !M.any_dirty) return;
    int total = 0, done = 0;
    for (uint32_t i = 0; i < M.n; i++) total += M.mats[i]->dirty && !M.mats[i]->instance;
    for (uint32_t i = 0; i < M.n; i++) {
        Material* m = M.mats[i];
        if (!m->dirty || m->instance) continue;
        if (progress) progress(done, total, m->name);
        bake(m);
        m->dirty = false, done++;
    }
    anim_continue(true, 0.0); // (the flipbooks' other frames: the loading screen waits for them)
    // (the instances and the slots: the first frame's mat_update, cheap now; still dirty so the scene is sent again)
}

static bool mat_update_dirty(bool all);

// M5's detail normals: each material's detail material found (loaded once) and its GpuMaterial slot written into the
// material's and its instances' e6 when it changes (a detail material baked after the one that uses it)
static void detail_follow(void) {
    for (uint32_t i = 0; i < M.n; i++) {
        if (!M.comp[i].detail_hash) continue;
        uint32_t h = M.comp[i].detail_hash;
        Material* dm = NULL;
        for (uint32_t j = 0; j < M.n && !dm; j++)
            if (M.mats[j]->hash == h) dm = M.mats[j];
        if (!dm && !M.comp[i].detail_tried) M.comp[i].detail_tried = true, dm = mat_find_hash(h); // (may load: M.comp moves)
        Compiled* c = &M.comp[i];
        uint32_t s = dm ? dm->slot : 0u;
        if (s == c->detail_slot) continue;
        c->detail_slot = s;
        write_slot(&c->v, c);
        for (uint32_t j = 0; j < M.n; j++)
            if (M.mats[j]->parent == M.mats[i] && g_inst_var[j].slot) write_slot(&g_inst_var[j], c);
    }
    // (B11) the painted layers' slots, the same way
    for (uint32_t i = 0; i < M.n; i++) {
        if (!(M.comp[i].xflags & MATX_LAYERS)) continue;
        bool changed = false;
        for (int k = 0; k < 3; k++) {
            uint32_t h = M.comp[i].layer_hash[k];
            if (!h) continue;
            Material* lm = NULL;
            for (uint32_t j = 0; j < M.n && !lm; j++)
                if (M.mats[j]->hash == h) lm = M.mats[j];
            if (!lm && !M.comp[i].layer_tried) lm = mat_find_hash(h); // (may load: M.comp moves)
            uint32_t s = lm ? lm->slot : 0u;
            if (s != M.comp[i].layer_slot[k]) M.comp[i].layer_slot[k] = s, changed = true;
        }
        M.comp[i].layer_tried = true;
        if (!changed) continue;
        Compiled* c = &M.comp[i];
        write_slot(&c->v, c);
        for (uint32_t j = 0; j < M.n; j++)
            if (M.mats[j]->parent == M.mats[i] && g_inst_var[j].slot) write_slot(&g_inst_var[j], c);
    }
}

bool mat_update(VkCtx* vk, VkCommandBuffer cmd) {
    (void)vk;
    if (!M.ready) return false;
    if (M.any_detail) detail_follow();
    // a frame's (the editor with its workers): a few at a time; an explicit call or the game's: all of them now
    bool changed = M.any_dirty && mat_update_dirty(!cmd || !async_ready());
    // the texture streaming's step for this frame (the frame's command buffer: a tile of a bake, copies or a swap)
    if (cmd) stream_step(cmd);
    else st_drain(false);
    return changed;
}

// The flipbooks' frames left (bake): within the slice (all: every one now); a finished one starts playing
static bool anim_continue(bool all, double t0) {
    bool left = false;
    for (uint32_t i = 0; i < M.n; i++) {
        Compiled* c = &M.comp[i];
        Material* m = M.mats[i];
        while (c->anim_next && c->anim_next < c->anim_total) {
            if (!all && (re_time_seconds() - t0) * 1000.0 > MAT_FRAME_MS) {
                left = true;
                break;
            }
            uint32_t k = c->anim_next++;
            Variant* fv = &c->frames[k];
            memset(fv, 0, sizeof *fv);
            fv->param_off = params_alloc((uint32_t)c->nparams);
            fill_params(fv->param_off, c, m, NULL);
            param_vals()[fv->param_off + (uint32_t)c->time_param][0] = c->period * (float)k / (float)c->anim_total;
            if (!bake_variant(c, fv)) {
                c->anim_next = c->anim_total = 0; // (stays still)
                break;
            }
            float* row = param_vals()[c->table_off + k]; // its textures, as the shader reads them
            uint32_t ids[4] = { fv->tex[0], fv->tex[1], fv->tex[2], 0 };
            memcpy(row, ids, 16);
            if (c->anim_next == c->anim_total) { // all of them: it plays (its instances' slots follow)
                c->nframes = c->anim_total;
                c->anim_next = c->anim_total = 0;
                write_slot(&c->v, c);
                for (uint32_t j = 0; j < M.n; j++)
                    if (M.mats[j]->parent == m) M.mats[j]->dirty = true, left = true;
            }
        }
    }
    return left;
}

static bool mat_update_dirty(bool all) {
    M.any_dirty = false;
    if (!MB.track) MB.done = 0; // (the card counts from its start)
    double t0 = re_time_seconds();
    bool left = false, baked = false;
    for (uint32_t i = 0; i < M.n; i++) {
        Material* m = M.mats[i];
        if (!m->dirty || m->instance) continue;
        if (!all && ((baked && (re_time_seconds() - t0) * 1000.0 > MAT_FRAME_MS) || !images_ready(m))) {
            left = true; // (next frame: its images still decoding, or this frame's slice spent)
            continue;
        }
        snprintf(MB.cur, sizeof MB.cur, "%s", m->name);
        bake(m);
        m->dirty = false, baked = true, MB.done++;
    }
    for (uint32_t i = 0; i < M.n; i++) {
        Material* m = M.mats[i];
        if (!m->dirty || !m->instance) continue;
        if (!all && ((m->parent && m->parent->dirty) || (baked && (re_time_seconds() - t0) * 1000.0 > MAT_FRAME_MS))) {
            left = true;
            continue;
        }
        update_instance(m);
        m->dirty = false, baked = true, MB.done++;
    }
    left |= anim_continue(all, t0);
    if (left) {
        M.any_dirty = true;
        if (!MB.track) MB.track = async_track(TR("Compiling materials"), mat_poll, NULL, NULL, ASYNC_QUIET);
    }
    if (all || !left) MB.cur[0] = 0;
    // the dynamic instances whose values a Blueprint changed (or whose material was rebaked): baked again
    for (uint32_t i = 0; i < M.ndyn; i++) {
        Dyn* d = &M.dyn[i];
        Compiled* c = compiled_of(d->m);
        if (!c || !d->v.dirty) continue;
        bake_variant(c, &d->v);
        write_slot(&d->v, c);
    }
    M.stats.materials = M.n;
    M.stats.slots = M.nslots;
    // (Q22) changed only when a material or an instance was baked, or an instance's slot changed: the renderer sends
    // every instance again on it (and waits for the GPU to send the meshes' info). "Always" made a level streaming in
    // (its images still decoding: nothing baked, the slice left for later) pay that every frame: world_partition's and
    // pcg_stream's hitches, ~12 ms a frame for ~1.5 s. The flipbooks' frames and the dynamic instances' values are
    // written into their slots in place (their instances point to the same slots)
    bool changed = baked || M.resend;
    M.resend = false;
    return changed;
}

uint32_t mat_dynamic_for(Entity e, Material* m) {
    if (!m) return 0;
    for (uint32_t i = 0; i < M.ndyn; i++)
        if (M.dyn[i].e == e && M.dyn[i].m == m) return M.dyn[i].v.slot;
    Compiled* c = compiled_of(m);
    if (M.ndyn >= MAT_MAX_DYNAMIC || !c || !m->slot) return m->slot;
    Dyn* d = &M.dyn[M.ndyn];
    memset(d, 0, sizeof *d);
    d->e = e;
    d->m = m;
    d->v.slot = slot_alloc();
    if (!d->v.slot) return m->slot;
    d->v.param_off = params_alloc((uint32_t)c->nparams);
    const GpuMaterial* src = &((GpuMaterial*)M.materials.mapped)[m->slot];
    memcpy(param_vals() + d->v.param_off, param_vals() + src->param_off, (size_t)c->nparams * 16);
    memcpy(d->v.avg, m->avg_base, sizeof d->v.avg);
    memcpy(d->v.avg_emis, m->avg_emis, sizeof d->v.avg_emis);
    write_slot(&d->v, c); // shares the material's textures until a parameter changes
    M.ndyn++;
    M.any_dirty = M.resend = true; // the actor's instance is sent again with its new slot
    return d->v.slot;
}

bool mat_set_param(uint32_t slot, const char* name, const float v[4]) {
    for (uint32_t i = 0; i < M.ndyn; i++) {
        Dyn* d = &M.dyn[i];
        if (d->v.slot != slot) continue;
        Compiled* c = compiled_of(d->m);
        if (!c) return false;
        for (int k = 0; k < c->nparams; k++)
            if (!_stricmp(c->pnames[k], name)) {
                float* dst = param_vals()[d->v.param_off + k];
                if (!memcmp(dst, v, 16)) return true;
                memcpy(dst, v, 16);
                if (k >= c->nbaked) return true; // (2026-11-16: only the Wet Surface reads it, at run time: no bake)
                d->v.dirty = true; // baked again before the next frame (once, however many values changed)
                M.any_dirty = true;
                return true;
            }
        return false;
    }
    return false;
}

void mat_end_play(void) {
    vkDeviceWaitIdle(M.vk->device);
    for (uint32_t i = 0; i < M.ndyn; i++) variant_free(&M.dyn[i].v), g_slot_toon_state[M.dyn[i].v.slot % MAT_GPU_SLOTS] = 0; // (Q11)
    M.ndyn = 0;
    M.any_dirty = M.resend = true; // (the actors' instances back on their materials' slots)
}

void mat_set_root(const char* content_root) {
    snprintf(M.root, sizeof M.root, "%s", content_root);
    for (char* c = M.root; *c; c++)
        if (*c == '/') *c = 92; // backslashes, as mat_load stores the paths
    M.scanned = false;
    S.hstale = true;
    for (uint32_t i = 0; i < M.n; i++) { // the hashes are of the paths under the root
        char vp[MAT_PATH + 16];
        if (mat_vpath(M.mats[i], vp, sizeof vp)) M.mats[i]->hash = vpath_hash(vp);
    }
}

VkImageView mat_preview_frame_view(const Material* m, int which, uint32_t frame) {
    Compiled* c = m && !m->instance ? compiled_of(m) : NULL;
    if (!c || c->nframes < 2 || which < 0 || which > 2) return VK_NULL_HANDLE;
    const Variant* v = frame % c->nframes ? &c->frames[frame % c->nframes] : &c->v;
    return v->tex[which] ? M.tex[v->tex[which]].view : VK_NULL_HANDLE;
}

VkImageView mat_preview_view(const Material* m, int which) {
    Compiled* c = m ? compiled_of(m) : NULL;
    if (!c || which < 0 || which > 2) return VK_NULL_HANDLE;
    const Variant* v = &c->v;
    if (m->instance) {
        int mi = mat_index(m);
        if (mi >= 0 && g_inst_var[mi].own) v = &g_inst_var[mi];
    }
    uint32_t t = v->tex[which];
    return t ? M.tex[t].view : VK_NULL_HANDLE;
}

const MatStats* mat_stats(void) { return &M.stats; }
VkDescriptorSetLayout mat_set_layout(void) { return M.set_layout; }
VkDescriptorSet mat_set(void) { return M.set; }
uint64_t mat_buffer_address(void) { return M.materials.addr; }
uint64_t mat_programs_address(void) { return M.programs.addr; }
uint64_t mat_params_address(void) { return M.params.addr; }

bool mat_init(VkCtx* vk, const char* content_root) {
    M.vk = vk;
    snprintf(M.root, sizeof M.root, "%s", content_root ? content_root : "Content");
    for (char* c = M.root; *c; c++)
        if (*c == '/') *c = 92; // backslashes, as mat_load stores the paths
    // texture streaming writes new textures' descriptors while frames in flight read others of the set: update after
    // bind (unused while pending) where the GPU has it (vkctx enables it); without, the streaming stays off
    VkPhysicalDeviceVulkan12Features h12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 h2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h12 };
    vkGetPhysicalDeviceFeatures2(vk->gpu, &h2);
    bool uab = h12.descriptorBindingSampledImageUpdateAfterBind && h12.descriptorBindingStorageImageUpdateAfterBind &&
               h12.descriptorBindingUpdateUnusedWhilePending;
    VkDescriptorBindingFlags bfl = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        (uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0u);
    VkDescriptorBindingFlags bf[3] = { bfl, bfl, bfl };
    stream_init(vk, uab);
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    fi.bindingCount = 3;
    fi.pBindingFlags = bf;
    VkDescriptorSetLayoutBinding lb[3] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAT_TEX_MAX, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, NULL }, // (Q17: mesh.vert reads the displacement)
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAT_TEX_MAX, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAT_TEX_MAX, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.pNext = &fi;
    li.bindingCount = 3;
    li.pBindings = lb;
    if (uab) li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    if (vkCreateDescriptorSetLayout(vk->device, &li, NULL, &M.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAT_TEX_MAX }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAT_TEX_MAX } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    if (uab) pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(vk->device, &pi, NULL, &M.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = M.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &M.set_layout;
    if (vkAllocateDescriptorSets(vk->device, &ai, &M.set) != VK_SUCCESS) return false;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BakePush) };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &M.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(vk->device, &pl, NULL, &M.layout) != VK_SUCCESS) return false;
    M.bake = gpu_compute_pipeline(vk, M.layout, s_bake, sizeof s_bake);
    M.bc7 = vk->bc_textures ? gpu_compute_pipeline(vk, M.layout, s_bc7, sizeof s_bc7) : VK_NULL_HANDLE; // (no BC: RGBA8)
    M.mip = gpu_compute_pipeline(vk, M.layout, s_mip, sizeof s_mip); // (none: the blits, as before)
    if (!alpha_cov_init(vk)) printf("[RealEngine] aviso: a cobertura dos mips Masked ficou desligada.\n");
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;
    // 16x (the GPU's most, when lower): floors seen grazing keep their detail (8x lost it a few meters away)
    si.maxAnisotropy = vk->gpu_props.limits.maxSamplerAnisotropy >= 16.0f ? 16.0f : vk->gpu_props.limits.maxSamplerAnisotropy;
    si.maxLod = 16.0f;
    if (vkCreateSampler(vk->device, &si, NULL, &M.sampler) != VK_SUCCESS) return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // (Q18) the UDIM tiles'
    if (vkCreateSampler(vk->device, &si, NULL, &M.sampler_clamp) != VK_SUCCESS) return false;
    VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!M.bake || !gpu_buf_create(vk, &M.materials, (sizeof(GpuMaterial) + sizeof(GpuMatExt) + sizeof(GpuToon)) * MAT_GPU_SLOTS, st, true) ||
        !gpu_buf_create(vk, &M.programs, (VkDeviceSize)MAT_PROG_WORDS * 4, st, true) ||
        !gpu_buf_create(vk, &M.params, (VkDeviceSize)MAT_PARAM_VEC4 * 16, st, true))
        return false;
    memset(M.materials.mapped, 0, (sizeof(GpuMaterial) + sizeof(GpuMatExt) + sizeof(GpuToon)) * MAT_GPU_SLOTS); // (Q11: + the toon blocks)
    M.ready = true;
    return true;
}

void mat_shutdown(VkCtx* vk) {
    if (!M.ready) return;
    vkDeviceWaitIdle(vk->device);
    for (uint32_t i = 0; i < MAT_TEX_MAX; i++) tex_free(i);
    tex_mem_shutdown();
    for (uint32_t i = 0; i < M.n; i++) {
        free(M.mats[i]->graph);
        free(M.mats[i]);
    }
    gpu_buf_destroy(vk, &M.materials);
    gpu_buf_destroy(vk, &M.mip_buf);
    gpu_buf_destroy(vk, &M.programs);
    gpu_buf_destroy(vk, &M.params);
    vkDestroySampler(vk->device, M.sampler, NULL);
    vkDestroySampler(vk->device, M.sampler_clamp, NULL);
    for (uint32_t i = 0; i < g_nuds; i++) // (Q18) the UDIM sources (their textures went with the rest)
        if (g_uds[i]) {
            if (g_uds[i]->file) udim_close(g_uds[i]->file), free(g_uds[i]->file);
            free(g_uds[i]), g_uds[i] = NULL;
        }
    g_nuds = 0;
    memset(g_udt, 0, sizeof g_udt);
    vkDestroyPipeline(vk->device, M.bake, NULL);
    alpha_cov_shutdown(vk);
    if (M.bc7) vkDestroyPipeline(vk->device, M.bc7, NULL);
    if (M.mip) vkDestroyPipeline(vk->device, M.mip, NULL);
    vkDestroyPipelineLayout(vk->device, M.layout, NULL);
    vkDestroyDescriptorPool(vk->device, M.pool, NULL);
    vkDestroyDescriptorSetLayout(vk->device, M.set_layout, NULL);
    memset(&M, 0, sizeof M);
    if (S.qp) vkDestroyQueryPool(vk->device, S.qp, NULL);
    memset(&S, 0, sizeof S);
    S.job.asset = -1;
}

// A material's base color texture in the bindless set (the particles' sprites); 0 without one yet
uint32_t mat_base_texture(uint32_t hash) {
    Material* m = mat_find_hash(hash);
    if (!m || !m->slot || !M.materials.mapped) return 0u;
    return ((const GpuMaterial*)M.materials.mapped)[m->slot].tex_base;
}

// --- The self-test (--mat-pins-test, the test mat_pins): an input "has effect" (mat_input_has_effect, what the editor
// greys and warns about) exactly when the compiler reads it: a Value node linked into each input of the Principled BSDF
// and of the Glossy BSDF is compiled only for those ---
int mat_pins_selftest(void) {
    static MatGraph g;
    static Material m;
    static Ir ir;
    static Built b;
    static const int types[2] = { NODE_BSDF_PRINCIPLED, NODE_BSDF_GLOSSY };
    int fails = 0, checked = 0;
    for (int t = 0; t < 2; t++) {
        const MatNodeDef* d = mat_node_def(types[t]);
        for (int s = 0; d && s < d->nin; s++) {
            memset(&g, 0, sizeof g);
            int out = mat_graph_add(&g, NODE_OUTPUT_MATERIAL, 300, 0);
            int bs = mat_graph_add(&g, types[t], 0, 0);
            int v = mat_graph_add(&g, NODE_VALUE, -300, 0);
            if (out < 0 || bs < 0 || v < 0 || !mat_graph_link(&g, bs, 0, out, 0) || !mat_graph_link(&g, v, 0, bs, s)) {
                printf("[RealEngine] FAIL: mat_pins: graph %s input %d\n", d->name, s), fails++;
                continue;
            }
            g.nodes[v].value[0] = 0.37f;
            memset(&m, 0, sizeof m);
            m.graph = &g;
            bool ok = compile_material(&m, &ir, &b);
            bool read = ok && ir.memo[v][0][0] != 0;
            checked++;
            if (!ok || read != mat_input_has_effect(types[t], s)) {
                printf("[RealEngine] FAIL: mat_pins: %s \"%s\" (%d): compiled %s, read %s, said %s\n", d->name, d->in[s].name, s,
                       ok ? "yes" : "no", read ? "yes" : "no", mat_input_has_effect(types[t], s) ? "with effect" : "without effect");
                fails++;
            }
        }
    }
    // the warning: a Coat Weight set is named, the defaults are not
    memset(&g, 0, sizeof g);
    int bs = mat_graph_add(&g, NODE_BSDF_PRINCIPLED, 0, 0);
    char names[256];
    if (mat_node_unused_inputs(&g, bs, names, sizeof names) != 0) printf("[RealEngine] FAIL: mat_pins: defaults reported: %s\n", names), fails++;
    g.nodes[bs].in[6][0] = 1.0f; // Diffuse Roughness (no effect yet: reported)
    if (mat_node_unused_inputs(&g, bs, names, sizeof names) != 1 || strcmp(names, "Diffuse Roughness"))
        printf("[RealEngine] FAIL: mat_pins: Diffuse Roughness not reported (%s)\n", names), fails++;
    g.nodes[bs].in[6][0] = 0.0f;
    g.nodes[bs].in[18][0] = 1.0f; // Coat Weight: its constant works now (M1b), not reported
    if (mat_node_unused_inputs(&g, bs, names, sizeof names) != 0) printf("[RealEngine] FAIL: mat_pins: Coat Weight reported (%s)\n", names), fails++;
    { // the extra lobes read from the node feeding the output: coat, transmission, the plain material has none
        int out = mat_graph_add(&g, NODE_OUTPUT_MATERIAL, 300, 0);
        float ext[8][4];
        bool tr = false;
        if (out < 0 || !mat_graph_link(&g, bs, 0, out, 0) || !principled_ext(&g, ext, &tr) || fabsf(ext[0][0] - 1.0f) > 1e-4f || tr)
            printf("[RealEngine] FAIL: mat_pins: the coat lobe was not taken\n"), fails++;
        g.nodes[bs].in[18][0] = 0.0f;
        if (principled_ext(&g, ext, &tr)) printf("[RealEngine] FAIL: mat_pins: a plain Principled has extra lobes\n"), fails++;
        g.nodes[bs].in[17][0] = 1.0f; // Transmission
        if (!principled_ext(&g, ext, &tr) || !tr) printf("[RealEngine] FAIL: mat_pins: transmission not taken\n"), fails++;
        g.nodes[bs].in[17][0] = 0.0f, g.nodes[bs].in[3][0] = 1.33f; // IOR of water: a dielectric F0 of 0.02
        if (!principled_ext(&g, ext, &tr) || fabsf(ext[2][1] - 0.02006f) > 1e-3f) printf("[RealEngine] FAIL: mat_pins: the IOR's F0 is %.4f\n", ext[2][1]), fails++;
    }
    { // M5: the Color Attribute's uses (mat_vcol_usage): into Base Color or a Multiply's A exact, anything else baked white
        memset(&g, 0, sizeof g);
        int out = mat_graph_add(&g, NODE_OUTPUT_MATERIAL, 300, 0), p = mat_graph_add(&g, NODE_BSDF_PRINCIPLED, 0, 0);
        int vc = mat_graph_add(&g, NODE_VERTEX_COLOR, -400, 0), mx = mat_graph_add(&g, NODE_MIX, -200, 0);
        mat_graph_link(&g, p, 0, out, 0);
        if (mat_vcol_usage(&g) != MAT_VCOL_NONE) printf("[RealEngine] FAIL: mat_pins: an unused Color Attribute counts\n"), fails++;
        mat_graph_link(&g, vc, 0, p, 0);
        if (mat_vcol_usage(&g) != MAT_VCOL_TINT) printf("[RealEngine] FAIL: mat_pins: Color Attribute -> Base Color not a tint\n"), fails++;
        g.nodes[mx].prop[0] = MIX_DATA_COLOR, g.nodes[mx].prop[1] = BLEND_MUL, g.nodes[mx].in[0][0] = 0.5f;
        mat_graph_link(&g, vc, 0, mx, 5), mat_graph_link(&g, mx, 2, p, 0);
        if (mat_vcol_usage(&g) != MAT_VCOL_TINT) printf("[RealEngine] FAIL: mat_pins: a Multiply's A not a tint\n"), fails++;
        mat_graph_unlink_input(&g, mx, 5), mat_graph_link(&g, vc, 0, mx, 6);
        if (mat_vcol_usage(&g) != MAT_VCOL_OTHER) printf("[RealEngine] FAIL: mat_pins: a Multiply's B at factor 0.5 taken as a tint\n"), fails++;
        mat_graph_link(&g, vc, 1, p, 1); // (its Alpha into Metallic: never exact)
        char nm[64];
        if (mat_node_unused_inputs(&g, vc, nm, sizeof nm) != 1) printf("[RealEngine] FAIL: mat_pins: the Color Attribute's misuse not reported\n"), fails++;
    }
    { // (2026-11-16) the Wet Surface: through it to the Principled's lobes, its constants, a Value parameter on Sweat read at run time
        memset(&g, 0, sizeof g);
        int out = mat_graph_add(&g, NODE_OUTPUT_MATERIAL, 300, 0), p = mat_graph_add(&g, NODE_BSDF_PRINCIPLED, 0, 0);
        int w = mat_graph_add(&g, NODE_RE_WET, 150, 0), v = mat_graph_add(&g, NODE_VALUE, 0, 300);
        mat_graph_link(&g, p, 0, w, 0), mat_graph_link(&g, w, 0, out, 0);
        g.nodes[p].in[18][0] = 1.0f; // (a coat)
        g.nodes[w].in[1][0] = 0.4f, g.nodes[w].in[4][0] = 0.1f;
        g.nodes[v].value[0] = 0.5f;
        snprintf(g.nodes[v].param, sizeof g.nodes[v].param, "Sweat");
        mat_graph_link(&g, v, 0, w, 8);
        static Ir wir;
        memset(&wir, 0, sizeof wir);
        float ext[13][4];
        bool tr;
        if (!principled_ext(&g, ext, &tr) || ext[0][0] < 0.99f) printf("[RealEngine] FAIL: mat_pins: the coat lost behind the Wet Surface\n"), fails++;
        if (!wet_ext(&g, &wir, ext)) printf("[RealEngine] FAIL: mat_pins: the Wet Surface not found\n"), fails++;
        else if (fabsf(ext[8][0] - 0.4f) > 1e-4f || fabsf(ext[8][3] - 0.01f) > 1e-4f || ext[8][1] >= 0.0f || ext[10][3] != 1.0f || wir.nparams != 1)
            printf("[RealEngine] FAIL: mat_pins: the Wet Surface's values %.3f %.3f %.3f, Sweat's parameter %.0f (%d)\n", ext[8][0], ext[8][3],
                   ext[8][1], ext[10][3], wir.nparams), fails++;
        checked++;
    }
    printf("[RealEngine] mat_pins: %d inputs checked, %d failures\n", checked, fails);
    return fails;
}
