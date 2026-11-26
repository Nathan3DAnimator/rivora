// (2026-11-24) The affine fold (notas-dev/materiais.md "Parâmetros vivos"). Included by material_gpu.inc after the compiler.
// A parameter (a Value / RGB node with a Parameter Name, the Emission Intensity) that reaches the base color, metallic,
// roughness or emission only through a Multiply, an Add, a Subtract, a Divide by it, a Mix with a fixed factor, an Invert,
// a Separate / Combine, a conversion or a shader's Strength makes that channel `root * a + b`: the bake writes the root (what
// doesn't depend on any parameter: a texture, a noise, a constant) and the coefficients a, b are computed on the CPU from
// the parameters (a small program of the uniform part: the parameters' own math) into the slot's GpuMatAff block, applied
// by the shaders (mat_common.glsl mat_affine). A Material Instance, a Blueprint's Set Scalar / Vector Parameter Value, the
// Material Editor's values and the Emission Intensity then change a buffer, not the textures: no bake.
// A parameter that enters any other way (a noise's scale, a UV, a Color Ramp's factor, a texture mask's mix, a normal, the
// alpha) is in bake_mask: changing it bakes the variant, as before.

static AffC g_ac[IR_MAX_VALS][4];
static int8_t g_uni[IR_MAX_VALS];       // 1: a value of parameters and constants only, with an op the CPU computes
static int16_t g_cpu[IR_MAX_VALS];      // its value in the CPU program (-1: not emitted)
static int16_t g_tv[IR_MAX_VALS][4];    // its term (+1; 0: none yet)
static uint32_t g_tpm[AFF_MAX_TERMS];   // the parameters a term reads
static AffProg g_ap;
static const Ir* g_air;
static bool g_aover;                    // (the CPU program full: no fold)
static uint32_t g_bad[IR_MAX_VALS];     // the parameters that made a value not affine here or before it (a texture's, a noise's inputs...)
static uint32_t g_live;                 // the parameters still taken as live (a baked one counts as a constant: the bake reads it)

#define AT0 0
#define AT1 1

static int t_new(uint8_t op, int a, int b, float k, uint8_t comp, uint32_t pm) {
    if (g_ap.nterms >= AFF_MAX_TERMS) {
        g_aover = true;
        return AT0;
    }
    int i = g_ap.nterms++;
    g_ap.t[i] = (AffTerm){ op, comp, (uint16_t)a, (uint16_t)b, k };
    g_tpm[i] = pm;
    return i;
}
static bool t_const(int t, float* k) {
    if (g_ap.t[t].op != AT_CONST) return false;
    *k = g_ap.t[t].k;
    return true;
}
static bool t_is(int t, float k) {
    float v;
    return t_const(t, &v) && v == k;
}
static int t_k(float k) { return k == 0.0f ? AT0 : k == 1.0f ? AT1 : t_new(AT_CONST, 0, 0, k, 0, 0); }
static float aff_safe_div(float a, float b) { return b != 0.0f ? a / b : 0.0f; }
static int t_mul(int a, int b) {
    float x, y;
    if (t_is(a, 0.0f) || t_is(b, 0.0f)) return AT0;
    if (t_is(a, 1.0f)) return b;
    if (t_is(b, 1.0f)) return a;
    if (t_const(a, &x) && t_const(b, &y)) return t_k(x * y);
    return t_new(AT_MUL, a, b, 0.0f, 0, g_tpm[a] | g_tpm[b]);
}
static int t_add(int a, int b) {
    float x, y;
    if (t_is(a, 0.0f)) return b;
    if (t_is(b, 0.0f)) return a;
    if (t_const(a, &x) && t_const(b, &y)) return t_k(x + y);
    return t_new(AT_ADD, a, b, 0.0f, 0, g_tpm[a] | g_tpm[b]);
}
static int t_sub(int a, int b) {
    float x, y;
    if (t_is(b, 0.0f)) return a;
    if (t_const(a, &x) && t_const(b, &y)) return t_k(x - y);
    return t_new(AT_SUB, a, b, 0.0f, 0, g_tpm[a] | g_tpm[b]);
}
static int t_div(int a, int b) { // (Blender's safe divide: 0 by 0)
    float x, y;
    if (t_is(b, 1.0f)) return a;
    if (t_const(a, &x) && t_const(b, &y)) return t_k(aff_safe_div(x, y));
    return t_new(AT_DIV, a, b, 0.0f, 0, g_tpm[a] | g_tpm[b]);
}
static int t_sat(int a) {
    float x;
    if (t_const(a, &x)) return t_k(x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x);
    return t_new(AT_SAT, a, 0, 0.0f, 0, g_tpm[a]);
}

// The ops the CPU program computes (the uniform part: what a parameter's value goes through before it meets a texture)
static bool aff_cpu_op(int op, const uint32_t p[4]) {
    switch (op) {
    case MOP_CONST: case MOP_PARAM: case MOP_CVT_LUM: case MOP_CVT_AVG: case MOP_CVT_SPLAT: case NODE_INVERT:
    case NODE_SEPARATE_XYZ: case NODE_COMBINE_XYZ: case NODE_RGB_TO_BW: case NODE_CLAMP: return true;
    case NODE_MATH:
        return p[0] == MATH_ADD || p[0] == MATH_SUBTRACT || p[0] == MATH_MULTIPLY || p[0] == MATH_DIVIDE || p[0] == MATH_POWER ||
               p[0] == MATH_MINIMUM || p[0] == MATH_MAXIMUM || p[0] == MATH_MULTIPLY_ADD;
    case NODE_VECTOR_MATH:
        return p[0] == VMATH_ADD || p[0] == VMATH_SUBTRACT || p[0] == VMATH_MULTIPLY || p[0] == VMATH_DIVIDE || p[0] == VMATH_SCALE ||
               p[0] == VMATH_MULTIPLY_ADD || p[0] == VMATH_MINIMUM || p[0] == VMATH_MAXIMUM;
    case NODE_MIX: return p[1] == BLEND_MIX || p[1] == BLEND_ADD || p[1] == BLEND_MUL || p[1] == BLEND_SUB;
    case NODE_SEPARATE_COLOR: case NODE_COMBINE_COLOR: return p[0] == COLOR_MODE_RGB;
    case NODE_MAP_RANGE: return p[0] == MAP_RANGE_LINEAR;
    default: return false;
    }
}

// A uniform IR value's place in the CPU program (its instruction and its inputs' emitted first)
static int cpu_of(int v) {
    if (g_cpu[v] >= 0) return g_cpu[v];
    const IrIns* x = &g_air->ins[g_air->vals[v].def];
    int in[8] = { 0 };
    for (int k = 0; k < x->nin && k < 8; k++) in[k] = cpu_of(x->in[k]);
    if (g_aover || g_ap.nins >= AFF_MAX_INS || g_ap.nvals + x->nout > AFF_MAX_VALS || x->nin > 8 || x->nout > 4) {
        g_aover = true;
        return 0;
    }
    AffIns* a = &g_ap.ins[g_ap.nins++];
    memset(a, 0, sizeof *a);
    a->op = (uint16_t)x->op, a->nin = (uint8_t)x->nin, a->nout = (uint8_t)x->nout;
    memcpy(a->p, x->p, sizeof a->p);
    for (int k = 0; k < x->nin; k++) a->in[k] = (uint8_t)in[k];
    for (int o = 0; o < x->nout; o++) {
        a->out[o] = (uint8_t)g_ap.nvals++;
        g_cpu[x->out[o]] = (int16_t)a->out[o];
    }
    return g_cpu[v];
}
static int t_val(int v, int c) {
    if (g_tv[v][c]) return g_tv[v][c] - 1;
    int cv = cpu_of(v);
    int t = t_new(AT_VAL, cv, 0, 0.0f, (uint8_t)c, g_ac[v][c].pm);
    g_tv[v][c] = (int16_t)(t + 1);
    return t;
}

static AffC ac_fail(uint32_t pm) { return (AffC){ -2, 0, AT1, AT0, pm }; }
static AffC ac_static(int v, int c) { return (AffC){ (int16_t)v, (int8_t)c, AT1, AT0, 0u }; }
// an operand: a uniform value as a term (root -1), else its form
static AffC opnd(int v, int c) {
    AffC a = g_ac[v][c];
    if (g_uni[v] == 1) a.root = -1, a.rc = 0, a.A = AT0, a.B = t_val(v, c);
    return a;
}
static AffC ac_scale(AffC x, int t) {
    if (x.root == -2) return x.pm |= g_tpm[t], x;
    x.A = t_mul(x.A, t), x.B = t_mul(x.B, t), x.pm |= g_tpm[t];
    return x;
}
static AffC ac_addt(AffC x, int t) {
    if (x.root == -2) return x.pm |= g_tpm[t], x;
    x.B = t_add(x.B, t), x.pm |= g_tpm[t];
    return x;
}
// x * tx + y * ty (one root at most)
static AffC ac_lin(AffC x, int tx, AffC y, int ty) {
    uint32_t pm = x.pm | y.pm | g_tpm[tx] | g_tpm[ty];
    if (x.root == -2 || y.root == -2) return ac_fail(pm);
    AffC r;
    if (x.root == -1) r = y, r.A = t_mul(y.A, ty), r.B = t_add(t_mul(y.B, ty), t_mul(x.B, tx));
    else if (y.root == -1) r = x, r.A = t_mul(x.A, tx), r.B = t_add(t_mul(x.B, tx), t_mul(y.B, ty));
    else if (x.root == y.root && x.rc == y.rc) r = x, r.A = t_add(t_mul(x.A, tx), t_mul(y.A, ty)), r.B = t_add(t_mul(x.B, tx), t_mul(y.B, ty));
    else return ac_fail(pm);
    r.pm = pm;
    return r;
}
static AffC ac_mul(AffC x, AffC y) {
    if (x.root == -1) {
        AffC r = ac_scale(y, x.B);
        r.pm |= x.pm;
        return r;
    }
    if (y.root == -1) {
        AffC r = ac_scale(x, y.B);
        r.pm |= y.pm;
        return r;
    }
    return ac_fail(x.pm | y.pm);
}

// One instruction's outputs: static (no parameter), uniform (the CPU computes them), affine in a root, or neither
static void aff_ins(int i) {
    const IrIns* x = &g_air->ins[i];
    if (!x->nout) return;
    uint32_t pin = 0, inbad = 0;
    bool uni = aff_cpu_op(x->op, x->p), inherited = false;
    for (int k = 0; k < x->nin; k++) {
        int v = x->in[k];
        if (v < 0) continue;
        for (int c = 0; c < 4; c++) pin |= g_ac[v][c].pm, inherited |= g_ac[v][c].root == -2;
        inbad |= g_bad[v];
        if (g_uni[v] != 1) uni = false;
    }
    if (x->op == MOP_PARAM && x->p[0] < 32u && (g_live >> x->p[0] & 1u)) pin |= 1u << x->p[0];
    if (pin == 0 || uni) {
        for (int o = 0; o < x->nout; o++) {
            int v = x->out[o];
            g_uni[v] = uni ? 1 : 0;
            g_bad[v] = inbad;
            for (int c = 0; c < 4; c++) // (a uniform one's term made when something reads it: opnd)
                g_ac[v][c] = pin == 0 ? ac_static(v, c) : (AffC){ -1, 0, AT0, AT0, pin };
        }
        return;
    }
    AffC r[MAT_NODE_MAX_OUT][4];
    for (int o = 0; o < MAT_NODE_MAX_OUT; o++)
        for (int c = 0; c < 4; c++) r[o][c] = ac_fail(pin);
#define I(k, c) opnd(x->in[k], c)
#define OUTV(o) (x->out[o])
    switch (x->op) {
    case NODE_MATH: {
        if (x->p[1]) break; // (clamped: not affine)
        AffC a = I(0, 0), b = I(1, 0), s = ac_fail(pin);
        switch (x->p[0]) {
        case MATH_ADD: s = ac_lin(a, AT1, b, AT1); break;
        case MATH_SUBTRACT: s = ac_lin(a, AT1, b, t_k(-1.0f)); break;
        case MATH_MULTIPLY: s = ac_mul(a, b); break;
        case MATH_DIVIDE:
            if (b.root == -1) s = ac_scale(a, t_div(AT1, b.B)), s.pm |= b.pm;
            break;
        case MATH_MULTIPLY_ADD: s = ac_lin(ac_mul(a, b), AT1, I(2, 0), AT1); break;
        default: break;
        }
        for (int c = 0; c < 4; c++) r[0][c] = s;
        break;
    }
    case NODE_VECTOR_MATH: {
        uint32_t op = x->p[0];
        if (op != VMATH_ADD && op != VMATH_SUBTRACT && op != VMATH_MULTIPLY && op != VMATH_DIVIDE && op != VMATH_SCALE && op != VMATH_MULTIPLY_ADD) break;
        for (int k = 0; k < 3; k++) {
            AffC a = I(0, k), b = I(1, k), s = ac_fail(pin);
            if (op == VMATH_ADD) s = ac_lin(a, AT1, b, AT1);
            else if (op == VMATH_SUBTRACT) s = ac_lin(a, AT1, b, t_k(-1.0f));
            else if (op == VMATH_MULTIPLY) s = ac_mul(a, b);
            else if (op == VMATH_DIVIDE) {
                if (b.root == -1) s = ac_scale(a, t_div(AT1, b.B)), s.pm |= b.pm;
            } else if (op == VMATH_SCALE) s = ac_mul(a, I(3, 0));
            else s = ac_lin(ac_mul(a, b), AT1, I(2, k), AT1);
            r[0][k] = s;
        }
        r[0][3] = ac_static(OUTV(0), 3);
        for (int c = 0; c < 4; c++) r[1][c] = ac_static(OUTV(1), c); // (the float output: 0 for these)
        break;
    }
    case NODE_MIX: {
        AffC f = I(0, 0);
        if (f.root != -1) break; // (a factor from a texture: not affine)
        int tf = x->p[2] ? t_sat(f.B) : f.B, tg = t_sub(AT1, tf);
        for (int c = 0; c < 4; c++) r[0][c] = ac_lin(I(1, 0), tg, I(2, 0), tf), r[0][c].pm |= f.pm;
        for (int k = 0; k < 3; k++) r[1][k] = ac_lin(I(3, k), tg, I(4, k), tf), r[1][k].pm |= f.pm;
        r[1][3] = ac_static(OUTV(1), 3);
        if (!x->p[3])
            for (int k = 0; k < 3; k++) {
                AffC c1 = I(5, k), c2 = I(6, k), s = ac_fail(pin);
                if (x->p[1] == BLEND_MIX) s = ac_lin(c1, tg, c2, tf);
                else if (x->p[1] == BLEND_ADD) s = ac_lin(c1, AT1, c2, tf);
                else if (x->p[1] == BLEND_SUB) s = ac_lin(c1, AT1, c2, t_sub(AT0, tf));
                else if (x->p[1] == BLEND_MUL) {
                    if (c2.root == -1) s = ac_scale(c1, t_add(tg, t_mul(tf, c2.B))), s.pm |= c2.pm;
                    else if (c1.root == -1) s = ac_lin(c1, tg, c2, t_mul(tf, c1.B));
                }
                s.pm |= f.pm;
                r[2][k] = s;
            }
        r[2][3] = ac_static(OUTV(2), 3);
        break;
    }
    case NODE_INVERT: { // mix(c, 1 - c, f) = c (1 - 2f) + f
        AffC f = I(0, 0);
        if (f.root != -1) break;
        int tf = f.B;
        for (int k = 0; k < 3; k++) r[0][k] = ac_addt(ac_scale(I(1, k), t_sub(AT1, t_mul(t_k(2.0f), tf))), tf), r[0][k].pm |= f.pm;
        r[0][3] = ac_static(OUTV(0), 3);
        break;
    }
    case MOP_CVT_SPLAT:
        for (int k = 0; k < 3; k++) r[0][k] = I(0, 0);
        r[0][3] = ac_static(OUTV(0), 3);
        break;
    case MOP_CVT_AVG:
    case MOP_CVT_LUM:
    case NODE_RGB_TO_BW: {
        bool avg = x->op == MOP_CVT_AVG;
        int w0 = t_k(avg ? 1.0f / 3.0f : 0.2126f), w1 = t_k(avg ? 1.0f / 3.0f : 0.7152f), w2 = t_k(avg ? 1.0f / 3.0f : 0.0722f);
        AffC s = ac_lin(ac_lin(I(0, 0), w0, I(0, 1), w1), AT1, I(0, 2), w2);
        for (int c = 0; c < 4; c++) r[0][c] = s;
        break;
    }
    case NODE_SEPARATE_COLOR:
        if (x->p[0] != COLOR_MODE_RGB) break;
        /* fallthrough */
    case NODE_SEPARATE_XYZ:
        for (int o = 0; o < 3 && o < x->nout; o++)
            for (int c = 0; c < 4; c++) r[o][c] = I(0, o);
        break;
    case NODE_COMBINE_COLOR:
        if (x->p[0] != COLOR_MODE_RGB) break;
        /* fallthrough */
    case NODE_COMBINE_XYZ:
        for (int k = 0; k < 3; k++) r[0][k] = I(k, 0);
        r[0][3] = ac_static(OUTV(0), 3);
        break;
    case MOP_CLOSURE: // base, metal, rough, alpha, normal, emission color, strength -> (base, metal), (emission, rough), (normal, alpha)
        for (int k = 0; k < 3; k++) r[0][k] = I(0, k), r[1][k] = ac_mul(I(5, k), I(6, 0)), r[2][k] = I(4, k);
        r[0][3] = I(1, 0), r[1][3] = I(2, 0), r[2][3] = I(3, 0);
        break;
    case MOP_SET_W:
        for (int k = 0; k < 3; k++) r[0][k] = I(0, k);
        r[0][3] = I(1, 0);
        break;
    case MOP_SHADER_MIX: { // fac, A (3), B (3): mixed per channel; the normal normalized after
        AffC f = I(0, 0);
        for (int o = 0; o < 3; o++)
            for (int c = 0; c < 4; c++) {
                AffC a = I(1 + o, c), b = I(4 + o, c);
                if (!(a.pm | b.pm | f.pm)) r[o][c] = ac_static(OUTV(o), c);
                else if (f.root == -1 && !(o == 2 && c < 3)) {
                    int tf = t_sat(f.B);
                    r[o][c] = ac_lin(a, t_sub(AT1, tf), b, tf), r[o][c].pm |= f.pm;
                }
            }
        break;
    }
    case MOP_SHADER_ADD: // A (3) + B (3): the colors and the emission add; the w's max / min and the normal not affine
        for (int o = 0; o < 3; o++)
            for (int c = 0; c < 4; c++) {
                AffC a = I(o, c), b = I(3 + o, c);
                if (!(a.pm | b.pm)) r[o][c] = ac_static(OUTV(o), c);
                else if (c < 3 && o < 2) r[o][c] = ac_lin(a, AT1, b, AT1);
            }
        break;
    default: break;
    }
#undef I
#undef OUTV
    // a component that couldn't fold here with no input already failed: its inputs' parameters are to blame (baked)
    bool fails = false;
    for (int o = 0; o < x->nout; o++)
        for (int c = 0; c < 4; c++) fails |= r[o][c].root == -2;
    uint32_t bad = inbad | (fails && !inherited ? pin : 0u);
    for (int o = 0; o < x->nout; o++) {
        int v = x->out[o];
        g_uni[v] = 0;
        g_bad[v] = bad;
        for (int c = 0; c < 4; c++) g_ac[v][c] = r[o][c];
    }
}

// The fold of a compiled graph: cl its closure into the Material Output (the IR up to its instruction); emis_param the
// Emission Intensity's parameter (-1: not emissive). Into P: the CPU program and the channels' coefficients; into gather
// (when a channel folds): the bake's result's parts (value, component) per channel, 12 of them. False: nothing to apply
// (no parameter folds and nothing glows)
// live: the parameters taken as live (the caller repeats it without the ones a channel couldn't fold: compile_material)
static __declspec(noinline) bool aff_analyze(const Ir* ir, int upto, const int cl[3], int emis_param, uint32_t live, AffProg* P, int gv[12], int gc[12],
                                             bool* folded, uint32_t* bad_out) {
    g_live = live;
    memset(g_bad, 0, sizeof(uint32_t) * (size_t)ir->nvals);
    memset(&g_ap, 0, sizeof g_ap);
    g_air = ir;
    g_aover = false;
    memset(g_uni, 0, sizeof(int8_t) * (size_t)ir->nvals);
    memset(g_cpu, 0xFF, sizeof(int16_t) * (size_t)ir->nvals);
    memset(g_tv, 0, sizeof(int16_t) * 4 * (size_t)ir->nvals);
    t_new(AT_CONST, 0, 0, 0.0f, 0, 0); // AT0
    t_new(AT_CONST, 0, 0, 1.0f, 0, 0); // AT1
    for (int v = 0; v < ir->nvals; v++)
        for (int c = 0; c < 4; c++) g_ac[v][c] = ac_static(v, c);
    for (int i = 0; i < upto && !g_aover; i++) aff_ins(i);
    *folded = false;
    uint32_t mask = 0;
    *bad_out = g_bad[cl[0]] | g_bad[cl[1]] | g_bad[cl[2]];
    int ta[12], tb[12];
    for (int ch = 0; ch < 12; ch++) {
        int v = cl[ch / 4], c = ch & 3;
        AffC a = g_ac[v][c].pm && g_uni[v] == 1 ? opnd(v, c) : g_ac[v][c];
        gv[ch] = v, gc[ch] = c, ta[ch] = AT1, tb[ch] = AT0;
        if (g_aover) { // (the CPU program full: as before, every parameter baked)
            mask |= a.pm;
            continue;
        }
        if (ch >= 8 || a.pm == 0 || a.root == -2) { // the normal and the alpha, a channel without parameters, or not affine
            mask |= a.pm;
            continue;
        }
        if (a.root == -1) gv[ch] = -1, gc[ch] = 0, ta[ch] = AT0, tb[ch] = a.B; // (uniform: the bake writes 0)
        else gv[ch] = a.root, gc[ch] = a.rc, ta[ch] = a.A, tb[ch] = a.B;
        *folded = true;
    }
    if (g_aover) *folded = false;
    if (emis_param >= 0 && !g_aover) { // the Emission Intensity scales the emission whatever it is
        int te = t_new(AT_PARAM, emis_param, 0, 0.0f, 0, 1u << emis_param);
        for (int ch = 4; ch < 7; ch++) ta[ch] = t_mul(ta[ch], te), tb[ch] = t_mul(tb[ch], te);
    }
    if (g_aover) { // (the terms ran out above: nothing folds, the emission plain)
        *folded = false;
        for (int ch = 0; ch < 12; ch++) gv[ch] = cl[ch / 4], gc[ch] = ch & 3;
        memset(P, 0, sizeof *P);
        P->bake_mask = 0xFFFFFFFFu;
        return false;
    }
    for (int ch = 0; ch < AFF_NCH; ch++) g_ap.a[ch] = (uint16_t)ta[ch], g_ap.b[ch] = (uint16_t)tb[ch];
    g_ap.bake_mask = mask | ~live;
    g_ap.on = *folded || emis_param >= 0;
    *P = g_ap;
    return P->on;
}

// --- At run time: a slot's coefficients from its parameters ---

static float aff_clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float aff_powf(float a, float b) { // (Blender's safe power)
    if (a < 0.0f && b != floorf(b)) return 0.0f;
    if (b == 0.0f) return 1.0f;
    if (a < 0.0f) {
        float r = powf(-a, b);
        return ((int)b % 2 == 0) ? r : -r;
    }
    return powf(a, b);
}
static float aff_math(uint32_t op, float a, float b, float c) {
    switch (op) {
    case MATH_ADD: return a + b;
    case MATH_SUBTRACT: return a - b;
    case MATH_MULTIPLY: return a * b;
    case MATH_DIVIDE: return aff_safe_div(a, b);
    case MATH_POWER: return aff_powf(a, b);
    case MATH_MINIMUM: return fminf(a, b);
    case MATH_MAXIMUM: return fmaxf(a, b);
    case MATH_MULTIPLY_ADD: return a * b + c;
    default: return 0.0f;
    }
}
static void aff_run(const AffProg* P, const float (*pv)[4], uint32_t np, float (*v)[4]) {
    for (uint32_t i = 0; i < P->nins; i++) {
        const AffIns* x = &P->ins[i];
#define IN(k) v[x->in[k]]
#define OUT(k) v[x->out[k]]
        switch (x->op) {
        case MOP_CONST: memcpy(OUT(0), x->p, 16); break;
        case MOP_PARAM:
            if (x->p[0] < np) memcpy(OUT(0), pv[x->p[0]], 16);
            else memset(OUT(0), 0, 16);
            break;
        case MOP_CVT_LUM:
        case NODE_RGB_TO_BW: {
            float l = IN(0)[0] * 0.2126f + IN(0)[1] * 0.7152f + IN(0)[2] * 0.0722f;
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = l;
            break;
        }
        case MOP_CVT_AVG: {
            float a = (IN(0)[0] + IN(0)[1] + IN(0)[2]) / 3.0f;
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = a;
            break;
        }
        case MOP_CVT_SPLAT: {
            float a = IN(0)[0];
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = a, OUT(0)[3] = 0.0f;
            break;
        }
        case NODE_INVERT:
            for (int k = 0; k < 3; k++) OUT(0)[k] = IN(1)[k] + (1.0f - 2.0f * IN(1)[k]) * IN(0)[0];
            OUT(0)[3] = 0.0f;
            break;
        case NODE_SEPARATE_COLOR:
        case NODE_SEPARATE_XYZ:
            for (int o = 0; o < 3 && o < x->nout; o++) OUT(o)[0] = OUT(o)[1] = OUT(o)[2] = OUT(o)[3] = IN(0)[o];
            break;
        case NODE_COMBINE_COLOR:
        case NODE_COMBINE_XYZ: OUT(0)[0] = IN(0)[0], OUT(0)[1] = IN(1)[0], OUT(0)[2] = IN(2)[0], OUT(0)[3] = 0.0f; break;
        case NODE_CLAMP: {
            float lo = IN(1)[0], hi = IN(2)[0];
            if (x->p[0] == CLAMP_RANGE && lo > hi) { float t = lo; lo = hi, hi = t; }
            float r = fminf(fmaxf(IN(0)[0], lo), hi);
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = r;
            break;
        }
        case NODE_MAP_RANGE: { // (linear)
            float f = aff_safe_div(IN(0)[0] - IN(1)[0], IN(2)[0] - IN(1)[0]), r = IN(3)[0] + f * (IN(4)[0] - IN(3)[0]);
            if (x->p[1]) {
                float lo = fminf(IN(3)[0], IN(4)[0]), hi = fmaxf(IN(3)[0], IN(4)[0]);
                r = fminf(fmaxf(r, lo), hi);
            }
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = r;
            break;
        }
        case NODE_MATH: {
            float r = aff_math(x->p[0], IN(0)[0], IN(1)[0], IN(2)[0]);
            if (x->p[1]) r = aff_clamp01(r);
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = r;
            break;
        }
        case NODE_VECTOR_MATH: {
            for (int k = 0; k < 3; k++) {
                float a = IN(0)[k], b = IN(1)[k], c = IN(2)[k], s = IN(3)[0], r = 0.0f;
                switch (x->p[0]) {
                case VMATH_ADD: r = a + b; break;
                case VMATH_SUBTRACT: r = a - b; break;
                case VMATH_MULTIPLY: r = a * b; break;
                case VMATH_DIVIDE: r = aff_safe_div(a, b); break;
                case VMATH_SCALE: r = a * s; break;
                case VMATH_MULTIPLY_ADD: r = a * b + c; break;
                case VMATH_MINIMUM: r = fminf(a, b); break;
                case VMATH_MAXIMUM: r = fmaxf(a, b); break;
                default: break;
                }
                OUT(0)[k] = r;
            }
            OUT(0)[3] = 0.0f;
            if (x->nout > 1) OUT(1)[0] = OUT(1)[1] = OUT(1)[2] = OUT(1)[3] = 0.0f;
            break;
        }
        case NODE_MIX: {
            float f = IN(0)[0];
            if (x->p[2]) f = aff_clamp01(f);
            float g = 1.0f - f, fl = IN(1)[0] * g + IN(2)[0] * f;
            OUT(0)[0] = OUT(0)[1] = OUT(0)[2] = OUT(0)[3] = fl;
            if (x->nout > 1) {
                for (int k = 0; k < 3; k++) OUT(1)[k] = IN(3)[k] * g + IN(4)[k] * f;
                OUT(1)[3] = 0.0f;
            }
            if (x->nout > 2) {
                for (int k = 0; k < 3; k++) {
                    float a = IN(5)[k], b = IN(6)[k], r;
                    if (x->p[1] == BLEND_ADD) r = a + b * f;
                    else if (x->p[1] == BLEND_SUB) r = a - b * f;
                    else if (x->p[1] == BLEND_MUL) r = a * g + a * b * f;
                    else r = a * g + b * f;
                    OUT(2)[k] = x->p[3] ? aff_clamp01(r) : r;
                }
                OUT(2)[3] = 0.0f;
            }
            break;
        }
        default: memset(OUT(0), 0, 16); break;
        }
#undef IN
#undef OUT
    }
}

// The slot's coefficients (ab: a0, b0, a1, b1 as GpuMatAff) from its parameter values
static void aff_eval(const AffProg* P, const float (*pv)[4], uint32_t np, float ab[16]) {
    static float v[AFF_MAX_VALS][4], t[AFF_MAX_TERMS];
    aff_run(P, pv, np, v);
    for (uint32_t i = 0; i < P->nterms; i++) {
        const AffTerm* x = &P->t[i];
        switch (x->op) {
        case AT_CONST: t[i] = x->k; break;
        case AT_VAL: t[i] = v[x->a][x->comp & 3]; break;
        case AT_PARAM: t[i] = x->a < np ? pv[x->a][0] : 1.0f; break;
        case AT_MUL: t[i] = t[x->a] * t[x->b]; break;
        case AT_ADD: t[i] = t[x->a] + t[x->b]; break;
        case AT_SUB: t[i] = t[x->a] - t[x->b]; break;
        case AT_DIV: t[i] = aff_safe_div(t[x->a], t[x->b]); break;
        case AT_SAT: t[i] = aff_clamp01(t[x->a]); break;
        default: t[i] = 0.0f; break;
        }
    }
    for (int ch = 0; ch < AFF_NCH; ch++) {
        int blk = ch < 4 ? 0 : 8, k = ch & 3; // (a0 b0 | a1 b1)
        ab[blk + k] = P->nterms ? t[P->a[ch]] : 1.0f;
        ab[blk + 4 + k] = P->nterms ? t[P->b[ch]] : 0.0f;
    }
}
