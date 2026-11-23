// --- (Q19, notas-dev/importar-tlender.md) The Tlender's shader nodes into the Engine's material graph (included by
// tlend_import.c, used by import_material): a node the Engine has comes in the same (or baked: the ones it evaluates only
// in the GPU bake of the material's textures, mat_vm.glsl MAT_BAKE); a node it lacks is rewritten as Engine nodes (a
// Metallic BSDF into a Principled, RGB Curves into Color Ramps that follow Blender's curve, Light Path into the
// constants of a camera ray...); a node whose output the Tlender baked into an image ("bake" lines, defined here for
// its export) becomes that Image Texture; the rest is lost with its reason. Each source socket maps to Engine sockets
// (McMap), and the file's links are made through them. ---

typedef struct McSock { int16_t node, sock; } McSock;
typedef struct McMap {
    McSock out[16];       // a source output: the Engine node and socket it is now (node -1: none)
    McSock in[32][3];     // a source input: the Engine inputs it feeds (up to 3)
    uint8_t nt[32];
} McMap;

static void mc_out(McMap* m, int s, int node, int sock) {
    if (s >= 0 && s < 16 && node >= 0) m->out[s].node = (int16_t)node, m->out[s].sock = (int16_t)sock;
}
static void mc_in(McMap* m, int s, int node, int sock) {
    if (s < 0 || s >= 32 || node < 0 || m->nt[s] >= 3) return;
    m->in[s][m->nt[s]].node = (int16_t)node, m->in[s][m->nt[s]].sock = (int16_t)sock;
    m->nt[s]++;
}
static int mc_add(MatGraph* g, int type, const TNode* s, float dx, float dy) {
    return mat_graph_add(g, type, s->x + dx, -(s->y + dy));
}
static void mc_set(MatGraph* g, int node, int sock, float a, float b, float c, float d) {
    if (node < 0 || sock < 0 || sock >= MAT_NODE_MAX_IN) return;
    float* v = g->nodes[node].in[sock];
    v[0] = a, v[1] = b, v[2] = c, v[3] = d;
}
// source input ss feeds Engine input (node, sock): its constant copied, its link made later
static void mc_feed(MatGraph* g, const TNode* s, McMap* m, int ss, int node, int sock) {
    if (node < 0) return;
    if (ss < s->nin && ss < 32 && sock < MAT_NODE_MAX_IN) memcpy(g->nodes[node].in[sock], s->in[ss], 16);
    mc_in(m, ss, node, sock);
}
static int mc_value(MatGraph* g, const TNode* s, float v, float dy) {
    int k = mc_add(g, NODE_VALUE, s, -200.0f, dy);
    if (k >= 0) g->nodes[k].value[0] = v;
    return k;
}
static int mc_rgb(MatGraph* g, const TNode* s, float r, float gg, float b, float dy) {
    int k = mc_add(g, NODE_RGB, s, -200.0f, dy);
    if (k >= 0) g->nodes[k].value[0] = r, g->nodes[k].value[1] = gg, g->nodes[k].value[2] = b, g->nodes[k].value[3] = 1.0f;
    return k;
}
// a source output as a constant (only made when something reads it: the links decide; cheap anyway)
static void mc_const_out(MatGraph* g, const TNode* s, McMap* m, int so, float v) { mc_out(m, so, mc_value(g, s, v, 60.0f * (float)so), 0); }
static void mc_const_out3(MatGraph* g, const TNode* s, McMap* m, int so, float a, float b, float c) {
    mc_out(m, so, mc_rgb(g, s, a, b, c, 60.0f * (float)so), 0);
}
static bool mc_linked(const TMat* t, int node, int sock) {
    for (int i = 0; i < t->nlinks; i++)
        if (t->links[i][2] == node && t->links[i][3] == sock) return true;
    return false;
}
static bool mc_out_used(const TMat* t, int node, int sock) {
    for (int i = 0; i < t->nlinks; i++)
        if (t->links[i][0] == node && t->links[i][1] == sock) return true;
    return false;
}

// A Principled BSDF standing for another BSDF: its output the source's, the given inputs mapped (pairs: source input,
// Principled input), the rest at the Principled's defaults
static int mc_principled(MatGraph* g, const TNode* s, McMap* m, const int (*pairs)[2], int np) {
    int k = mc_add(g, NODE_BSDF_PRINCIPLED, s, 0, 0);
    if (k < 0) return -1;
    for (int i = 0; i < np; i++) mc_feed(g, s, m, pairs[i][0], k, pairs[i][1]);
    mc_out(m, 0, k, 0);
    return k;
}

// --- Curves (Blender's CurveMapping, the Tlender's nodes.c: control points in the ramp rows (x, y, curve, handle)) ---
#define MC_TABLE 256
typedef struct McCurve { float y[MC_TABLE + 1]; float min, range; bool ok; } McCurve;
typedef struct McBez { float v[3][2]; int h; } McBez;

static void mc_calchandle(McBez* b, const McBez* prev, const McBez* next) {
    float* p2 = b->v[1];
    float pt[2];
    const float *p1, *p3;
    if (!prev) {
        p3 = next->v[1], pt[0] = 2.0f * p2[0] - p3[0], pt[1] = 2.0f * p2[1] - p3[1], p1 = pt;
    } else p1 = prev->v[1];
    if (!next) {
        p1 = prev->v[1], pt[0] = 2.0f * p2[0] - p1[0], pt[1] = 2.0f * p2[1] - p1[1], p3 = pt;
    } else p3 = next->v[1];
    float da[2] = { p2[0] - p1[0], p2[1] - p1[1] }, db[2] = { p3[0] - p2[0], p3[1] - p2[1] };
    float la = sqrtf(da[0] * da[0] + da[1] * da[1]), lb = sqrtf(db[0] * db[0] + db[1] * db[1]);
    if (la == 0.0f) la = 1.0f;
    if (lb == 0.0f) lb = 1.0f;
    if (b->h == 0 || b->h == 2) { // Auto, Auto Clamped
        float t[2] = { db[0] / lb + da[0] / la, db[1] / lb + da[1] / la };
        float len = sqrtf(t[0] * t[0] + t[1] * t[1]) * 2.5614f;
        if (len != 0.0f) {
            float a = la / len, c = lb / len;
            b->v[0][0] = p2[0] - t[0] * a, b->v[0][1] = p2[1] - t[1] * a, b->v[2][0] = p2[0] + t[0] * c, b->v[2][1] = p2[1] + t[1] * c;
            if (b->h == 2 && next && prev) {
                float y1 = prev->v[1][1] - p2[1], y2 = next->v[1][1] - p2[1];
                if ((y1 <= 0.0f && y2 <= 0.0f) || (y1 >= 0.0f && y2 >= 0.0f)) b->v[0][1] = b->v[2][1] = p2[1];
            }
        }
    } else { // Vector
        b->v[0][0] = p2[0] - da[0] / 3.0f, b->v[0][1] = p2[1] - da[1] / 3.0f, b->v[2][0] = p2[0] + db[0] / 3.0f, b->v[2][1] = p2[1] + db[1] / 3.0f;
    }
}

// curve c of node n as MC_TABLE + 1 values over [min, min + range] (the Tlender's curve_table, without its extrapolation)
static void mc_curve(const TNode* n, int c, bool vector, McCurve* t) {
    McBez bz[16];
    int np = 0;
    float lo = vector ? -1.0f : 0.0f, hi = 1.0f;
    for (int i = 0; i < n->ramp_count && np < 16; i++) {
        if ((int)n->ramp[i][2] != c) continue;
        memset(&bz[np], 0, sizeof bz[np]);
        bz[np].v[1][0] = n->ramp[i][0], bz[np].v[1][1] = n->ramp[i][1], bz[np].h = (int)n->ramp[i][3];
        lo = fminf(lo, bz[np].v[1][0]), hi = fmaxf(hi, bz[np].v[1][0]);
        np++;
    }
    for (int i = 1; i < np; i++) // (sorted by x)
        for (int j = i; j > 0 && bz[j].v[1][0] < bz[j - 1].v[1][0]; j--) {
            McBez tmp = bz[j];
            bz[j] = bz[j - 1], bz[j - 1] = tmp;
        }
    t->min = lo, t->range = hi - lo, t->ok = np > 0;
    if (np == 0) {
        for (int a = 0; a <= MC_TABLE; a++) t->y[a] = lo + t->range * (float)a / MC_TABLE; // (no points: identity)
        return;
    }
    if (np == 1) {
        for (int a = 0; a <= MC_TABLE; a++) t->y[a] = bz[0].v[1][1];
        return;
    }
    for (int a = 0; a < np; a++) mc_calchandle(&bz[a], a ? &bz[a - 1] : NULL, a < np - 1 ? &bz[a + 1] : NULL);
    // the segments sampled densely (x, y), then read at equal x steps
    enum { RES = 64 };
    float pts[15 * RES + 1][2];
    int npt = 0;
    for (int a = 0; a < np - 1; a++)
        for (int k = 0; k < RES; k++) {
            float u = (float)k / RES, w = 1.0f - u;
            float b0 = w * w * w, b1 = 3 * w * w * u, b2 = 3 * w * u * u, b3 = u * u * u;
            float x1 = fminf(fmaxf(bz[a].v[2][0], bz[a].v[1][0]), bz[a + 1].v[1][0]), x2 = fminf(fmaxf(bz[a + 1].v[0][0], bz[a].v[1][0]), bz[a + 1].v[1][0]);
            pts[npt][0] = b0 * bz[a].v[1][0] + b1 * x1 + b2 * x2 + b3 * bz[a + 1].v[1][0];
            pts[npt][1] = b0 * bz[a].v[1][1] + b1 * bz[a].v[2][1] + b2 * bz[a + 1].v[0][1] + b3 * bz[a + 1].v[1][1];
            npt++;
        }
    pts[npt][0] = bz[np - 1].v[1][0], pts[npt][1] = bz[np - 1].v[1][1], npt++;
    int j = 0;
    for (int a = 0; a <= MC_TABLE; a++) {
        float x = lo + t->range * (float)a / MC_TABLE;
        while (j + 1 < npt && pts[j + 1][0] < x) j++;
        if (x <= pts[0][0]) t->y[a] = pts[0][1];
        else if (j + 1 >= npt) t->y[a] = pts[npt - 1][1];
        else {
            float dx = pts[j + 1][0] - pts[j][0], f = dx > 1e-9f ? (x - pts[j][0]) / dx : 0.0f;
            t->y[a] = pts[j][1] + (pts[j + 1][1] - pts[j][1]) * f;
        }
    }
}
static float mc_curve_at(const McCurve* t, float x) {
    float f = t->range > 0.0f ? (x - t->min) / t->range * MC_TABLE : 0.0f;
    if (f <= 0.0f) return t->y[0];
    if (f >= MC_TABLE) return t->y[MC_TABLE];
    int i = (int)f;
    return t->y[i] + (t->y[i + 1] - t->y[i]) * (f - (float)i);
}
// a Color Ramp of up to 16 stops (gray: the values) that follows y over [0, 1]: the stops where the straight pieces miss
// the most (greedy), so a curve with a few bends needs few
static float mc_ramp_fit(const float* y, int n, float (*stops)[5], int* count) {
    int sel[16] = { 0, n - 1 }, ns = 2;
    float worst = 0.0f;
    while (ns < 16) {
        worst = 0.0f;
        int at = -1;
        for (int s = 0; s + 1 < ns; s++)
            for (int i = sel[s] + 1; i < sel[s + 1]; i++) {
                float f = (float)(i - sel[s]) / (float)(sel[s + 1] - sel[s]);
                float e = fabsf(y[i] - (y[sel[s]] + (y[sel[s + 1]] - y[sel[s]]) * f));
                if (e > worst) worst = e, at = i;
            }
        if (at < 0 || worst < 1e-3f) break;
        int p = ns;
        while (p > 0 && sel[p - 1] > at) sel[p] = sel[p - 1], p--;
        sel[p] = at, ns++;
    }
    for (int s = 0; s < ns; s++) {
        float v = y[sel[s]];
        stops[s][0] = (float)sel[s] / (float)(n - 1), stops[s][1] = stops[s][2] = stops[s][3] = v, stops[s][4] = 1.0f;
    }
    *count = ns;
    return worst;
}
// a ramp node fed by the value in [lo, lo + range] (a Map Range in front when that isn't [0, 1]); returns the ramp
static int mc_ramp_of(MatGraph* g, const TNode* s, const float* y, float lo, float range, float dy, int* in_node, int* in_sock, float* err) {
    int r = mc_add(g, NODE_COLOR_RAMP, s, 120.0f, dy);
    if (r < 0) return -1;
    MatNode* rn = &g->nodes[r];
    int cnt = 0;
    *err = fmaxf(*err, mc_ramp_fit(y, MC_TABLE + 1, rn->ramp, &cnt));
    rn->ramp_count = cnt, rn->prop[0] = 0; // (linear)
    *in_node = r, *in_sock = 0;
    if (fabsf(lo) > 1e-6f || fabsf(range - 1.0f) > 1e-6f) {
        int mr = mc_add(g, NODE_MAP_RANGE, s, 0.0f, dy);
        if (mr >= 0) {
            mc_set(g, mr, 1, lo, 0, 0, 0), mc_set(g, mr, 2, lo + range, 0, 0, 0), mc_set(g, mr, 3, 0, 0, 0, 0), mc_set(g, mr, 4, 1, 0, 0, 0);
            mat_graph_link(g, mr, 0, r, 0);
            *in_node = mr, *in_sock = 0;
        }
    }
    return r;
}

// RGB / Vector / Float Curves: per channel a ramp of Blender's curve (RGB: the combined curve first), mixed by the factor
static bool mc_curves(MatGraph* g, const TNode* s, const TMat* t, int si, McMap* m, int kind, float* err) {
    // kind 0 RGB, 1 Vector, 2 Float
    int nch = kind == 2 ? 1 : 3, data = kind == 0 ? 2 : kind == 1 ? 1 : 0;
    McCurve comb, ch;
    if (kind == 0) mc_curve(s, 3, false, &comb);
    int sep = -1, cmb = -1;
    if (kind != 2) {
        sep = mc_add(g, kind == 0 ? NODE_SEPARATE_COLOR : NODE_SEPARATE_XYZ, s, -120.0f, 0);
        cmb = mc_add(g, kind == 0 ? NODE_COMBINE_COLOR : NODE_COMBINE_XYZ, s, 360.0f, 0);
        if (sep < 0 || cmb < 0) return false;
        mc_feed(g, s, m, 1, sep, 0);
    }
    float y[MC_TABLE + 1];
    for (int c = 0; c < nch; c++) {
        mc_curve(s, kind == 2 ? 0 : c, kind == 1, &ch);
        float lo = kind == 1 ? ch.min : 0.0f, range = kind == 1 ? ch.range : 1.0f;
        if (kind == 0) lo = comb.min, range = comb.range;
        if (kind == 2) lo = ch.min, range = ch.range;
        for (int a = 0; a <= MC_TABLE; a++) {
            float x = lo + range * (float)a / MC_TABLE;
            if (kind == 0 && comb.ok) x = mc_curve_at(&comb, x);
            y[a] = mc_curve_at(&ch, x);
        }
        int in_node, in_sock;
        int r = mc_ramp_of(g, s, y, lo, range, 80.0f * (float)c, &in_node, &in_sock, err);
        if (r < 0) return false;
        if (kind == 2) mc_feed(g, s, m, 1, in_node, in_sock);
        else mat_graph_link(g, sep, c, in_node, in_sock), mat_graph_link(g, r, 0, cmb, c);
        if (kind == 2) cmb = r; // (the float's result: the ramp's color, read as a value)
    }
    if (s->prop[0] != 0 && kind == 1) *err = fmaxf(*err, 0.0f); // (extrapolated: held at the ends here)
    // the factor: a constant 1 unlinked needs no mix
    bool full = !mc_linked(t, si, 0) && s->in[0][0] >= 0.9999f;
    if (full) mc_out(m, 0, cmb, 0);
    else {
        int mx = mc_add(g, NODE_MIX, s, 480.0f, 0);
        if (mx < 0) return false;
        g->nodes[mx].prop[0] = data, g->nodes[mx].prop[1] = 0;
        int a = data == 0 ? 1 : data == 1 ? 3 : 5;
        mc_feed(g, s, m, 0, mx, 0);
        mc_feed(g, s, m, 1, mx, a);
        mat_graph_link(g, cmb, 0, mx, a + 1);
        mc_out(m, 0, mx, data);
    }
    return true;
}

// Blender's blackbody (Cycles' fit: the Tlender's render_tables.c), for a constant temperature
static void mc_blackbody(float t, float out[3]) {
    static const float R[7][3] = { { 1.61919106e+03f, -2.05010916e-03f, 5.02995757e+00f }, { 2.48845471e+03f, -1.11330907e-03f, 3.22621544e+00f },
                                   { 3.34143193e+03f, -4.86551192e-04f, 1.76486769e+00f }, { 4.09461742e+03f, -1.27446582e-04f, 7.25731635e-01f },
                                   { 4.67028036e+03f, 2.91258199e-05f, 1.26703442e-01f },  { 4.59509185e+03f, 2.87495649e-05f, 1.50345020e-01f },
                                   { 3.78717450e+03f, 9.35907826e-06f, 3.99075871e-01f } };
    static const float G[7][3] = { { -4.88999748e+02f, 6.04330754e-04f, -7.55807526e-02f }, { -7.55994277e+02f, 3.16730098e-04f, 4.78306139e-01f },
                                   { -1.02363977e+03f, 1.20223470e-04f, 9.36662319e-01f },  { -1.26571316e+03f, 4.87340896e-06f, 1.27054498e+00f },
                                   { -1.42529332e+03f, -4.01150431e-05f, 1.43972784e+00f }, { -1.17554822e+03f, -2.16378048e-05f, 1.30408023e+00f },
                                   { -5.00799571e+02f, -4.59832026e-06f, 1.09098763e+00f } };
    static const float B[7][4] = { { 5.96945309e-11f, -4.85742887e-08f, -9.70622247e-05f, -4.07936148e-03f },
                                   { 2.40430366e-11f, 5.55021075e-08f, -1.98503712e-04f, 2.89312858e-02f },
                                   { -1.40949732e-11f, 1.89878968e-07f, -3.56632824e-04f, 9.10767778e-02f },
                                   { -3.61460868e-11f, 2.84822009e-07f, -4.93211319e-04f, 1.56723440e-01f },
                                   { -1.97075738e-11f, 1.75359352e-07f, -2.50542825e-04f, -2.22783266e-02f },
                                   { -1.61997957e-13f, -1.64216008e-08f, 3.86216271e-04f, -7.38077418e-01f },
                                   { 6.72650283e-13f, -2.73078809e-08f, 4.24098264e-04f, -7.52335691e-01f } };
    if (t >= 12000.0f) out[0] = 0.8262955f, out[1] = 0.9945081f, out[2] = 1.5663077f;
    else if (t < 800.0f) out[0] = 5.4132945f, out[1] = -0.2031939f, out[2] = -0.0822535f;
    else {
        int i = t >= 6365.0f ? 6 : t >= 3315.0f ? 5 : t >= 1902.0f ? 4 : t >= 1449.0f ? 3 : t >= 1167.0f ? 2 : t >= 965.0f ? 1 : 0;
        float ti = 1.0f / t;
        out[0] = R[i][0] * ti + R[i][1] * t + R[i][2], out[1] = G[i][0] * ti + G[i][1] * t + G[i][2];
        out[2] = ((B[i][0] * t + B[i][1]) * t + B[i][2]) * t + B[i][3];
    }
    for (int k = 0; k < 3; k++) out[k] = fmaxf(out[k], 0.0f);
}

// A hash in [0, 1) (Object Info's Random: per material here)
static float mc_hash01(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return (float)(h >> 8) / 16777216.0f;
}

// The idnames the Engine has but bakes only (evaluated in the GPU bake into the material's textures)
static bool mc_baked_only(const char* id) {
    static const char* const ids[] = { "ShaderNodeTexBrick", "ShaderNodeTexMagic", "TlShaderNodeTexWeave", "TlShaderNodeTexCracks",
                                       "TlShaderNodeTexStones", "TlShaderNodeTexGrains", "TlShaderNodeTexOcean", "TlShaderNodeTexFibers",
                                       "ShaderNodeVectorRotate", "ShaderNodeBlackbody" };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (!strcmp(ids[i], id)) return true;
    return false;
}

// A node the Engine lacks rewritten into m (its sockets); returns the report's kind and its reason in why
static int mc_convert(MatGraph* g, Material* mt, const TMat* t, int si, McMap* m, char* why, size_t cap) {
    const TNode* s = &t->nodes[si];
    const char* id = s->idname;
    why[0] = 0;
#define ISID(x) (!strcmp(id, x))
#define WHY(...) snprintf(why, cap, __VA_ARGS__)
    if (ISID("ShaderNodeBsdfMetallic")) {
        static const int p[][2] = { { 0, 0 }, { 4, 2 }, { 5, 14 }, { 6, 15 }, { 7, 5 }, { 8, 16 } };
        int k = mc_principled(g, s, m, p, 6);
        mc_set(g, k, 1, 1, 0, 0, 0);
        WHY("%s", TR("a Principled BSDF with Metallic 1 (the Edge Tint and the thin film are not kept)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeBsdfGlass") || ISID("ShaderNodeBsdfRefraction")) {
        static const int p[][2] = { { 0, 0 }, { 1, 2 }, { 2, 3 }, { 3, 5 } };
        int k = mc_principled(g, s, m, p, 4);
        mc_set(g, k, 17, 1, 0, 0, 0);
        WHY("%s", TR("a Principled BSDF with Transmission 1 (the material becomes translucent: refractive glass)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeBsdfSheen")) {
        static const int p[][2] = { { 0, 0 }, { 0, 25 }, { 1, 24 }, { 2, 5 } };
        int k = mc_principled(g, s, m, p, 4);
        mc_set(g, k, 23, 1, 0, 0, 0), mc_set(g, k, 2, 1, 0, 0, 0);
        WHY("%s", TR("a Principled BSDF with Sheen 1 over its color (velvet)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeBsdfTranslucent")) {
        static const int p[][2] = { { 0, 0 }, { 1, 5 } };
        int k = mc_principled(g, s, m, p, 2);
        mc_set(g, k, 7, 1, 0, 0, 0), mc_set(g, k, 2, 1, 0, 0, 0);
        WHY("%s", TR("a Principled BSDF with Subsurface 1 (light through thin leaves and paper, approximated)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeSubsurfaceScattering")) {
        static const int p[][2] = { { 0, 0 }, { 1, 9 }, { 2, 8 }, { 3, 10 }, { 4, 2 }, { 5, 11 }, { 6, 5 } };
        int k = mc_principled(g, s, m, p, 7);
        mc_set(g, k, 7, 1, 0, 0, 0);
        WHY("%s", TR("a Principled BSDF with Subsurface 1 (its radius, scale and IOR)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeBsdfHair") || ISID("ShaderNodeBsdfHairPrincipled") || ISID("TlShaderNodeBsdfHairToon")) {
        bool pr = ISID("ShaderNodeBsdfHairPrincipled"), toon = ISID("TlShaderNodeBsdfHairToon");
        int k = mc_add(g, NODE_BSDF_PRINCIPLED, s, 0, 0);
        if (k < 0) return IMPREP_LOST;
        mc_out(m, 0, k, 0);
        if (pr && s->prop[1] == 1 && !mc_linked(t, si, 1)) { // by melanin: the color it gives (Blender's absorption, read back)
            float mel = s->in[1][0], red = s->in[2][0], eu = mel * (1.0f - red), ph = mel * red;
            float sa[3] = { eu * 0.506f + ph * 0.343f, eu * 0.841f + ph * 0.733f, eu * 1.653f + ph * 1.924f };
            float c[3];
            for (int q = 0; q < 3; q++) c[q] = expf(-sqrtf(fmaxf(sa[q], 0.0f)) * 1.6f) * s->in[3][q];
            mc_set(g, k, 0, c[0], c[1], c[2], 1);
        } else mc_feed(g, s, m, 0, k, 0);
        if (pr) mc_feed(g, s, m, 6, k, 2);
        else if (!toon) mc_feed(g, s, m, 2, k, 2);
        if (toon) mt->shading_model = MAT_SHADING_TOON;
        WHY("%s", toon ? TR("a Principled BSDF with the root color, lit by the Toon shading model (the strands' highlights are the Groom's)")
                       : TR("a Principled BSDF with the hair's color and roughness (a Groom draws the strands with its own hair shading)"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeBsdfToon")) {
        int k = mc_add(g, NODE_BSDF_PRINCIPLED, s, 0, 0);
        if (k < 0) return IMPREP_LOST;
        mc_feed(g, s, m, 0, k, 0), mc_feed(g, s, m, 3, k, 5), mc_out(m, 0, k, 0);
        mc_set(g, k, 2, s->prop[0] == 1 ? fminf(fmaxf(s->in[1][0], 0.05f), 1.0f) : 1.0f, 0, 0, 0);
        mt->shading_model = MAT_SHADING_TOON;
        WHY("%s", TR("the material's Toon shading model (bands of light) over a Principled of its color"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeBackground")) {
        int k = mc_add(g, NODE_EMISSION, s, 0, 0);
        mc_feed(g, s, m, 0, k, 0), mc_feed(g, s, m, 1, k, 1), mc_out(m, 0, k, 0);
        WHY("%s", TR("an Emission of its color and strength (a world's background on a surface)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("TlShaderNodeLayer")) { // Coverage, Top, Bottom -> a Mix Shader (bottom, top) by the coverage
        int k = mc_add(g, NODE_MIX_SHADER, s, 0, 0);
        mc_feed(g, s, m, 0, k, 0), mc_feed(g, s, m, 2, k, 1), mc_feed(g, s, m, 1, k, 2), mc_out(m, 0, k, 0);
        WHY("%s", TR("a Mix Shader of the bottom and the top by the coverage (the layering's energy kept is approximated)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("TlShaderNodeWetLayer")) { // Shader, Wet Shader, Wetness... -> the Engine's Wet Surface over the shader
        int k = mc_add(g, NODE_RE_WET, s, 0, 0);
        mc_feed(g, s, m, 0, k, 0), mc_feed(g, s, m, 2, k, 1), mc_out(m, 0, k, 0);
        WHY("%s", TR("the Engine's Wet Surface over the shader, by its wetness (the wet shader of its own is not used)"));
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("TlShaderNodeWetness")) { // Color, Roughness, Wetness, Porosity -> darker, smoother; the coat by the wetness
        int fac = mc_add(g, NODE_MATH, s, -120.0f, 0), mx = mc_add(g, NODE_MIX, s, 60.0f, 0), rg = mc_add(g, NODE_MAP_RANGE, s, 60.0f, 120.0f);
        if (fac < 0 || mx < 0 || rg < 0) return IMPREP_LOST;
        g->nodes[fac].prop[0] = MATH_MULTIPLY;
        mc_feed(g, s, m, 2, fac, 0), mc_feed(g, s, m, 3, fac, 1);
        g->nodes[mx].prop[0] = 2, g->nodes[mx].prop[1] = BLEND_MUL;
        mat_graph_link(g, fac, 0, mx, 0);
        mc_feed(g, s, m, 0, mx, 5);
        mc_set(g, mx, 6, 0.45f, 0.45f, 0.45f, 1);
        mc_out(m, 0, mx, 2);
        // roughness toward 0.05 by the wetness: a Map Range of the wetness from [0, 1] to [roughness, 0.05]
        mc_feed(g, s, m, 2, rg, 0), mc_feed(g, s, m, 1, rg, 3), mc_set(g, rg, 4, 0.05f, 0, 0, 0);
        mc_out(m, 1, rg, 0);
        mc_out(m, 2, fac, 0);
        WHY("%s", TR("darker and smoother by the wetness and porosity (Mix, Math and Map Range nodes)"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeLightPath")) { // a camera ray: Is Camera Ray 1, the rest 0, length 1
        for (int o = 0; o < 13; o++)
            if (mc_out_used(t, si, o)) mc_const_out(g, s, m, o, o == 0 ? 1.0f : o == 7 ? 1.0f : 0.0f);
        WHY("%s", TR("the values of a camera ray (Is Camera Ray 1, the others 0): the Engine bakes the material as the camera sees it"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeObjectInfo")) {
        if (mc_out_used(t, si, 0)) mc_const_out3(g, s, m, 0, 0, 0, 0);
        if (mc_out_used(t, si, 1)) mc_const_out3(g, s, m, 1, 1, 1, 1);
        if (mc_out_used(t, si, 2)) mc_const_out(g, s, m, 2, 0);
        if (mc_out_used(t, si, 3)) mc_const_out(g, s, m, 3, mc_hash01(t->name));
        WHY("%s", TR("constants per material (Location 0, Color white, Random from its name): the Engine shares a material's textures among its actors"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeParticleInfo") || ISID("ShaderNodeHairInfo") || ISID("ShaderNodeVolumeInfo") || ISID("TlShaderNodeRainInfo") ||
        ISID("TlShaderNodeBurnInfo")) {
        bool hair = ISID("ShaderNodeHairInfo");
        for (int o = 0; o < 16; o++)
            if (mc_out_used(t, si, o)) {
                if (hair && (o == 4 || o == 6 || o == 7)) mc_const_out3(g, s, m, o, o == 4 ? 0.0f : 0.5f, o == 4 ? 0.0f : 0.5f, o == 4 ? 1.0f : 0.5f);
                else mc_const_out(g, s, m, o, hair && o == 1 ? 0.5f : hair && (o == 2 || o == 3) ? 1.0f : hair && o == 5 ? mc_hash01(t->name) : 0.0f);
            }
        WHY("%s", hair ? TR("constants (Intercept 0.5, Random per material): the Groom's strands have their own root-to-tip color")
                       : TR("constants (0): particles, volumes and the Tlender's simulations don't feed a surface material here"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeNewGeometry")) { // Position, Normal, Tangent, True Normal, Incoming, Backfacing (Pointiness, Random)
        int tc = mc_add(g, NODE_TEX_COORD, s, 0, 0);
        if (tc < 0) return IMPREP_LOST;
        mc_out(m, 0, tc, 3), mc_out(m, 1, tc, 1), mc_out(m, 3, tc, 1), mc_out(m, 4, tc, 4);
        if (mc_out_used(t, si, 2)) mc_const_out3(g, s, m, 2, 1, 0, 0);
        for (int o = 5; o < 8; o++)
            if (mc_out_used(t, si, o)) mc_const_out(g, s, m, o, o == 6 ? 0.5f : 0.0f);
        WHY("%s", TR("Texture Coordinate outputs (the Engine bakes in the UV space: Position is the Object coordinate); Backfacing 0, Pointiness 0.5"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeAttribute")) { // the mesh's color attribute, its UV map, else nothing
        const char* nm = s->attr;
        bool uv = !strncmp(nm, "UV", 2) || !strncmp(nm, "uv", 2);
        if (uv) {
            int k = mc_add(g, NODE_UV_MAP, s, 0, 0);
            mc_out(m, 0, k, 0), mc_out(m, 1, k, 0);
            WHY(TR("the UV map '%s' (the Engine's first UV)"), nm);
            return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
        }
        int k = mc_add(g, NODE_VERTEX_COLOR, s, 0, 0);
        mc_out(m, 0, k, 0), mc_out(m, 1, k, 0), mc_out(m, 3, k, 1);
        if (mc_out_used(t, si, 2)) mc_const_out(g, s, m, 2, 1.0f);
        WHY(TR("the mesh's vertex colors (Color Attribute) for the attribute '%s'"), nm[0] ? nm : "Col");
        return k >= 0 ? IMPREP_CONVERTED : IMPREP_LOST;
    }
    if (ISID("ShaderNodeNormal")) { // the node's direction, and its dot with the normalized input
        float d[3] = { s->value[0], s->value[1], s->value[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 1e-9f) d[2] = 1.0f, l = 1.0f;
        for (int q = 0; q < 3; q++) d[q] /= l;
        mc_const_out3(g, s, m, 0, d[0], d[1], d[2]);
        int nz = mc_add(g, NODE_VECTOR_MATH, s, 0, 60.0f), dt = mc_add(g, NODE_VECTOR_MATH, s, 120.0f, 60.0f);
        if (nz < 0 || dt < 0) return IMPREP_LOST;
        g->nodes[nz].prop[0] = VMATH_NORMALIZE, g->nodes[dt].prop[0] = VMATH_DOT;
        mc_feed(g, s, m, 0, nz, 0);
        mat_graph_link(g, nz, 0, dt, 0);
        mc_set(g, dt, 1, d[0], d[1], d[2], 0);
        mc_out(m, 1, dt, 1);
        WHY("%s", TR("its direction as a constant and the Dot by Vector Math nodes"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeAmbientOcclusion")) { // the color through; AO 1: the Engine's GI and contact shadows darken in real time
        int k = mc_add(g, NODE_MIX, s, 0, 0);
        if (k < 0) return IMPREP_LOST;
        g->nodes[k].prop[0] = 2;
        mc_set(g, k, 0, 0, 0, 0, 0);
        mc_feed(g, s, m, 0, k, 5);
        mc_out(m, 0, k, 2);
        if (mc_out_used(t, si, 1)) mc_const_out(g, s, m, 1, 1.0f);
        WHY("%s", TR("the color as it is, AO 1: the Engine's GI and contact shadows darken the crevices in real time (the Tlender can bake it: see the report's list)"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeBevel")) {
        int k = mc_add(g, NODE_TEX_COORD, s, 0, 0);
        bool lk = mc_linked(t, si, 1);
        if (lk) { // the normal it was given
            int nz = mc_add(g, NODE_VECTOR_MATH, s, 0, 60.0f);
            if (nz >= 0) g->nodes[nz].prop[0] = VMATH_NORMALIZE, mc_feed(g, s, m, 1, nz, 0), mc_out(m, 0, nz, 0);
        } else mc_out(m, 0, k, 1);
        WHY("%s", TR("the mesh's normal (the rounded edges of the shader don't exist in the Engine: bevel the mesh, or let the Tlender bake it)"));
        return IMPREP_LOST;
    }
    if (ISID("ShaderNodeWireframe")) {
        if (mc_out_used(t, si, 0)) mc_const_out(g, s, m, 0, 0.0f);
        WHY("%s", TR("0: the triangles' edges are not known in the bake (the Tlender can bake it)"));
        return IMPREP_LOST;
    }
    if (ISID("ShaderNodeVectorDisplacement")) { // its Z (up in tangent space) as a height
        int sx = mc_add(g, NODE_SEPARATE_XYZ, s, -120.0f, 0), d = mc_add(g, NODE_DISPLACEMENT, s, 0, 0);
        if (sx < 0 || d < 0) return IMPREP_LOST;
        mc_feed(g, s, m, 0, sx, 0);
        mat_graph_link(g, sx, 2, d, 0);
        mc_feed(g, s, m, 1, d, 1), mc_feed(g, s, m, 2, d, 2), mc_out(m, 0, d, 0);
        WHY("%s", TR("a Displacement by its height (Z): the parallax shows the height, not the sideways offsets"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeTexEnvironment")) {
        int k = mc_add(g, NODE_TEX_IMAGE, s, 0, 0);
        if (k < 0) return IMPREP_LOST;
        g->nodes[k].prop[0] = s->prop[0], g->nodes[k].prop[2] = s->prop[2];
        mc_feed(g, s, m, 0, k, 0), mc_out(m, 0, k, 0), mc_out(m, 1, k, 1);
        WHY("%s", TR("an Image Texture read by its vector (the sky of a level is the Engine's Sky Atmosphere or an HDRI Backdrop)"));
        return IMPREP_CONVERTED;
    }
    if (ISID("ShaderNodeRGBCurve") || ISID("ShaderNodeVectorCurve") || ISID("ShaderNodeFloatCurve")) {
        float err = 0.0f;
        int kind = ISID("ShaderNodeRGBCurve") ? 0 : ISID("ShaderNodeVectorCurve") ? 1 : 2;
        if (!mc_curves(g, s, t, si, m, kind, &err)) return IMPREP_LOST;
        WHY(TR("Color Ramps of up to 16 stops that follow Blender's curve (largest difference %.3f)%s"), err,
            s->prop[0] ? TR("; held past its ends (not extrapolated)") : "");
        return IMPREP_CONVERTED;
    }
    if (!strncmp(id, "ShaderNodeVolume", 16) || ISID("TlShaderNodeVolumeFire") || ISID("TlShaderNodeVolumeSmoke") || ISID("TlShaderNodeVolumeExplosion")) {
        WHY("%s", TR("a volume shader: the Engine's materials are surfaces (use the Exponential Height Fog's volumetric fog or the Fire)"));
        return IMPREP_LOST;
    }
    if (ISID("ShaderNodeOutputWorld") || ISID("ShaderNodeTexSky") || ISID("TlShaderNodeSky")) {
        WHY("%s", TR("the world's sky: the level's sky is the Engine's Sky Atmosphere and Sky Light"));
        return IMPREP_LOST;
    }
    // the Tlender's surface presets (water, fabric, soil, mud, sand, snow, ice, rock, bark, leaf, lava...) and the rest:
    // a Principled of its first color, so the surface keeps its tint
    if (!strncmp(id, "TlShaderNode", 12) && s->nin > 0) {
        int k = mc_add(g, NODE_BSDF_PRINCIPLED, s, 0, 0);
        if (k >= 0) {
            mc_set(g, k, 0, s->in[0][0], s->in[0][1], s->in[0][2], 1);
            mc_out(m, 0, k, 0);
        }
        WHY("%s", TR("a procedural material of the Tlender's: a Principled of its color stands in; the Tlender bakes it into images when it exports for the Engine"));
        return IMPREP_LOST;
    }
    WHY("%s", TR("no equivalent in the Engine: its output is left unconnected"));
    return IMPREP_LOST;
#undef ISID
#undef WHY
}

// --- Images: the node's file (absolute, next to the .tlend, by its name in the file's image list, or found by its name
// in the folders around), a generated image (Blender's New Image: Blank, UV Grid, Color Grid) written as a PNG, a UDIM's
// tile 1001, a sequence's first frame ---
typedef struct McImage { char name[128], path[512]; int colorspace, source; } McImage;
static McImage* g_mc_images;
static int g_mc_nimages;

static void parse_images(TFile* f) {
    free(g_mc_images), g_mc_images = NULL, g_mc_nimages = 0;
    size_t at = find(f, 0, "images");
    int n = 0;
    if (at >= f->nlines || sscanf(f->lines[at] + 7, "%d", &n) != 1 || n <= 0 || n > 4096) return;
    g_mc_images = calloc((size_t)n, sizeof(McImage));
    for (size_t i = at + 1; g_mc_images && i < f->nlines; i++) {
        const char* l = f->lines[i];
        if (starts(l, "img")) {
            if (g_mc_nimages >= n) break;
            McImage* im = &g_mc_images[g_mc_nimages++];
            im->colorspace = atoi(l + 4);
        } else if (g_mc_nimages && starts(l, "imgname")) snprintf(g_mc_images[g_mc_nimages - 1].name, 128, "%s", l[7] ? l + 8 : "");
        else if (g_mc_nimages && starts(l, "imgpath")) snprintf(g_mc_images[g_mc_nimages - 1].path, 512, "%s", l[7] ? l + 8 : "");
        else if (g_mc_nimages && starts(l, "imgsrc")) g_mc_images[g_mc_nimages - 1].source = atoi(l + 7);
        else if (!starts(l, "imgtiles") && !starts(l, "tile")) break;
    }
}

static bool mc_exists(const char* p) { return p[0] && re_file_stat(p, NULL, NULL, NULL); }

// a generated image's key into a PNG at `to` (false: not a key)
static bool mc_generated_png(const char* key, const char* to) {
    int id = 0, type = 0, w = 0, h = 0;
    float c[4] = { 0, 0, 0, 1 };
    if (strncmp(key, "generated:", 10) || sscanf(key + 10, "%d:%d:%d:%d:%f:%f:%f:%f", &id, &type, &w, &h, &c[0], &c[1], &c[2], &c[3]) < 4) return false;
    if (w < 1 || h < 1) w = h = 1024;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    uint8_t* px = malloc((size_t)w * h * 3);
    if (!px) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &px[((size_t)y * w + x) * 3];
            int cx = x * 8 / w, cy = y * 8 / h;
            if (type == 0) { // Blank: its color
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)(fminf(fmaxf(c[k], 0.0f), 1.0f) * 255.0f + 0.5f);
            } else if (type == 1) { // UV Grid: Blender's checker of grays with a cross per cell
                int lx = (x * 64 / w) % 8, ly = (y * 64 / h) % 8;
                uint8_t v = ((cx + cy) & 1) ? 64 : 192;
                if (lx == 4 || ly == 4) v = v > 128 ? 128 : 160;
                p[0] = p[1] = p[2] = v;
            } else { // Color Grid: a hue per column, lighter rows
                float hue = (float)cx / 8.0f * 6.0f, f2 = hue - floorf(hue), l = 0.45f + 0.07f * (float)cy;
                float rgb[6][3] = { { 1, f2, 0 }, { 1 - f2, 1, 0 }, { 0, 1, f2 }, { 0, 1 - f2, 1 }, { f2, 0, 1 }, { 1, 0, 1 - f2 } };
                const float* q = rgb[(int)hue % 6];
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)(fminf(q[k] * l + (1.0f - l) * 0.3f, 1.0f) * 255.0f);
                if ((x * 64 / w) % 8 == 0 || (y * 64 / h) % 8 == 0) p[0] = p[1] = p[2] = 30;
            }
        }
    bool ok = vkctx_png_write_rgb(to, px, (uint32_t)w, (uint32_t)h);
    free(px);
    return ok;
}

// (2026-11-23, the user: "FBX should look for its textures in the folders around") The images around an imported file,
// indexed once per source folder: its folder and its subfolders (4 deep), its parent's and the parent's other folders
// (3 deep); 20000 files at most. Each with its distance (0 its folder, 1 under it, 2 the parent's tree): the nearest wins
typedef struct McNear { char path[600], stem[128], ext[8]; uint8_t dist; } McNear;
static McNear* g_near;
static int g_nnear, g_cap_near;
static char g_near_dir[600];
static const char* const MC_IMG_EXT[] = { "png", "jpg", "jpeg", "tga", "tif", "tiff", "bmp", "dds", "exr", "hdr", "webp", "psd" };
static void mc_lower(char* s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}
typedef struct McWalk { char dir[600]; int depth, dist; const char* skip; } McWalk;
static void mc_near_add(void* user, const ReDirEntry* e);
static void mc_near_scan(const char* dir, int depth, int dist, const char* skip) {
    McWalk w;
    snprintf(w.dir, sizeof w.dir, "%s", dir);
    w.depth = depth, w.dist = dist, w.skip = skip;
    re_dir_walk(dir, mc_near_add, &w);
}
static void mc_near_add(void* user, const ReDirEntry* e) {
    const McWalk* w = user;
    if (g_nnear >= 20000) return;
    char full[600];
    snprintf(full, sizeof full, "%s/%s", w->dir, e->name);
    if (e->is_dir) {
        if (e->name[0] == '.' || w->depth <= 0 || (w->skip && !strcmp(full, w->skip))) return;
        mc_near_scan(full, w->depth - 1, w->dist ? w->dist : 1, w->skip);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot) return;
    char ext[8];
    snprintf(ext, sizeof ext, "%s", dot + 1);
    mc_lower(ext);
    bool img = false;
    for (size_t k = 0; k < sizeof MC_IMG_EXT / sizeof MC_IMG_EXT[0]; k++) img |= !strcmp(ext, MC_IMG_EXT[k]);
    if (!img) return;
    if (g_nnear >= g_cap_near) {
        int cap = g_cap_near ? g_cap_near * 2 : 256;
        McNear* n = realloc(g_near, sizeof(McNear) * (size_t)cap);
        if (!n) return;
        g_near = n, g_cap_near = cap;
    }
    McNear* m = &g_near[g_nnear++];
    snprintf(m->path, sizeof m->path, "%s", full);
    snprintf(m->stem, sizeof m->stem, "%.*s", (int)(dot - e->name), e->name);
    mc_lower(m->stem);
    snprintf(m->ext, sizeof m->ext, "%s", ext);
    m->dist = (uint8_t)w->dist;
}
static void mc_near_index(const char* src_dir) {
    if (g_near && !strcmp(g_near_dir, src_dir)) return;
    g_nnear = 0;
    snprintf(g_near_dir, sizeof g_near_dir, "%s", src_dir);
    mc_near_scan(src_dir, 4, 0, NULL);
    char parent[600];
    snprintf(parent, sizeof parent, "%s", src_dir);
    char* sl = parent + strlen(parent);
    while (sl > parent && sl[-1] != '/' && sl[-1] != '\\') sl--;
    if (sl > parent + 1) { // (its parent's tree, its own folder left out: already in)
        sl[-1] = 0;
        McWalk w;
        snprintf(w.dir, sizeof w.dir, "%s", parent);
        w.depth = 3, w.dist = 2, w.skip = src_dir;
        re_dir_walk(parent, mc_near_add, &w);
    }
}
// (2026-11-23) A dropped folder's mirror (content.c content_import_folder): a texture inside src is the Content's
// file at the same place under dst (copied there when missing)
// (per thread: the main thread queues a folder's files while a worker imports one of them with its own)
#ifdef _MSC_VER
#define MC_TLS __declspec(thread)
#else
#define MC_TLS __thread
#endif
static MC_TLS char g_mirror_src[1024], g_mirror_dst[600];
void tlend_import_get_mirror(char* src_root, size_t scap, char* dst_vroot, size_t dcap) {
    snprintf(src_root, scap, "%s", g_mirror_src), snprintf(dst_vroot, dcap, "%s", g_mirror_dst);
}
void tlend_import_set_mirror(const char* src_root, const char* dst_vroot) {
    snprintf(g_mirror_src, sizeof g_mirror_src, "%s", src_root ? src_root : "");
    snprintf(g_mirror_dst, sizeof g_mirror_dst, "%s", dst_vroot ? dst_vroot : "");
    for (char* c = g_mirror_src; *c; c++)
        if (*c == '\\') *c = '/';
}
// the file's place in the mirror (its vpath and its disk path; false: not inside the dropped folder)
static bool mc_mirror(const char* file, const char* folder_disk, const char* folder_vpath, char* vpath, size_t vcap, char* disk, size_t dcap) {
    if (!g_mirror_src[0] || !g_mirror_dst[0]) return false;
    char f[1100];
    snprintf(f, sizeof f, "%s", file);
    for (char* c = f; *c; c++)
        if (*c == '\\') *c = '/';
    size_t n = strlen(g_mirror_src);
    if (_strnicmp(f, g_mirror_src, n) || f[n] != '/') return false;
    // the Content's disk root: folder_disk less folder_vpath's part after /Content
    size_t vl = strlen(folder_vpath), dl = strlen(folder_disk), tail = vl > 8 ? vl - 8 : 0; // ("/Content" is 8)
    if (strncmp(folder_vpath, "/Content", 8) || dl < tail) return false;
    char root[900];
    snprintf(root, sizeof root, "%.*s", (int)(dl - tail), folder_disk);
    snprintf(vpath, vcap, "%s%s", g_mirror_dst, f + n);
    snprintf(disk, dcap, "%s%s%s", root, g_mirror_dst + 8, f + n);
    return true;
}

// a file by its name somewhere near the import (the nearest; the same name with another image extension when that one
// isn't there: a .tga written in the file, a .png beside it): out
static bool mc_find_near(const char* src_dir, const char* base, char* out, size_t cap) {
    mc_near_index(src_dir);
    char stem[128], ext[8] = "";
    snprintf(stem, sizeof stem, "%s", base);
    char* dot = strrchr(stem, '.');
    if (dot) snprintf(ext, sizeof ext, "%s", dot + 1), *dot = 0;
    mc_lower(stem), mc_lower(ext);
    int best = -1, bscore = 1 << 30;
    for (int i = 0; i < g_nnear; i++) {
        if (strcmp(g_near[i].stem, stem)) continue;
        int score = g_near[i].dist * 4 + (strcmp(g_near[i].ext, ext) ? 2 : 0);
        if (score < bscore) bscore = score, best = i;
    }
    if (best < 0) return false;
    snprintf(out, cap, "%s", g_near[best].path);
    return true;
}

// A name brought to its letters and digits, lower case ("M_Wood Floor.001" -> "woodfloor"; prefixes M_ MI_ MAT_ off)
static void mc_key(const char* in, char* out, size_t cap) {
    static const char* const pre[] = { "mi_", "m_", "mat_", "mtl_" };
    char low[200];
    snprintf(low, sizeof low, "%s", in);
    mc_lower(low);
    const char* s = low;
    for (size_t k = 0; k < sizeof pre / sizeof pre[0]; k++)
        if (!strncmp(s, pre[k], strlen(pre[k]))) s += strlen(pre[k]);
    char* dot = strrchr(low, '.'); // (Blender's .001)
    if (dot && dot[1] >= '0' && dot[1] <= '9') *dot = 0;
    size_t n = 0;
    for (; *s && n + 1 < cap; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) out[n++] = *s;
    out[n] = 0;
}
// The PBR maps by their names' endings (Substance, Quixel, Poly Haven, Blender's bakes): slot, endings
enum { MCM_BASE = 0, MCM_NORMAL, MCM_NORMAL_DX, MCM_ROUGH, MCM_GLOSS, MCM_METAL, MCM_ORM, MCM_EMIT, MCM_ALPHA, MCM_COUNT };
static const char* const MC_MAP_END[MCM_COUNT][8] = {
    { "basecolor", "basecolour", "albedo", "diffuse", "diff", "color", "col", "bc" },
    { "normalgl", "normal", "normalmap", "nrm", "nor", "norm", "nml", NULL },
    { "normaldx", NULL },
    { "roughness", "rough", "rgh", NULL },
    { "glossiness", "gloss", NULL },
    { "metallic", "metalness", "metal", "mtl", NULL },
    { "orm", "arm", "occlusionroughnessmetallic", "aorm", NULL },
    { "emissive", "emission", "emit", "glow", NULL },
    { "opacity", "alpha", "mask", NULL },
};
// a map of a slot for a key (a file whose stem, brought to its letters, is the key and then one of the slot's endings)
static int mc_map_find(const char* key, int slot) {
    size_t kl = strlen(key);
    if (kl < 2) return -1;
    int best = -1, bd = 99;
    for (int i = 0; i < g_nnear; i++) {
        char st[128];
        mc_key(g_near[i].stem, st, sizeof st);
        if (strncmp(st, key, kl)) continue;
        const char* rest = st + kl;
        for (int e = 0; e < 8 && MC_MAP_END[slot][e]; e++) {
            const char* en = MC_MAP_END[slot][e];
            size_t el = strlen(en);
            // the ending, then nothing or a resolution / a number (_2k, 4096)
            if (strncmp(rest, en, el)) continue;
            const char* t = rest + el;
            bool tail = !*t;
            if (!tail) {
                while (*t >= '0' && *t <= '9') t++;
                tail = !*t || !strcmp(t, "k") || !strcmp(t, "px");
            }
            if (tail && g_near[i].dist < bd) bd = g_near[i].dist, best = i;
        }
    }
    return best;
}

// The node's image into <folder>/Textures (n->image: its vpath, or where it was); reported. color: the node's color space
static void mc_image(const TNode* s, MatNode* n, const char* folder_disk, const char* folder_vpath, const char* src_dir, const char* owner, TlendReport* r) {
    char from[1024], key[512];
    snprintf(key, sizeof key, "%s", s->image);
    const McImage* lib = NULL;
    for (int i = 0; i < g_mc_nimages; i++) // (a name of the image list, or its path)
        if (!strcmp(g_mc_images[i].name, key) || !strcmp(g_mc_images[i].path, key)) lib = &g_mc_images[i];
    if (lib && lib->path[0] && strcmp(lib->name, key) == 0) snprintf(key, sizeof key, "%s", lib->path);
    char tdir[900];
    snprintf(tdir, sizeof tdir, "%s/Textures", folder_disk);
    re_make_dirs(tdir);
    if (!strncmp(key, "generated:", 10)) { // Blender's New Image: made here
        char nm[96], to[1100];
        snprintf(nm, sizeof nm, "%s", lib && lib->name[0] ? lib->name : "Generated");
        clean(nm);
        snprintf(to, sizeof to, "%s/%s.png", tdir, nm);
        if (mc_generated_png(key, to)) {
            snprintf(n->image, sizeof n->image, "%s/Textures/%s.png", folder_vpath, nm);
            r->textures++;
            imprep_add(IMPREP_BAKED, IMPREP_CAT_TEXTURE, owner, nm, "%s", TR("a generated image of the Tlender's (Blank / UV Grid / Color Grid) written as a PNG"));
        } else imprep_add(IMPREP_LOST, IMPREP_CAT_TEXTURE, owner, nm, "%s", TR("a generated image that could not be written"));
        return;
    }
    if (key[0] == '/' || key[0] == '\\' || (key[0] && key[1] == ':')) snprintf(from, sizeof from, "%s", key);
    else snprintf(from, sizeof from, "%s/%s", src_dir, key[0] == '/' && key[1] == '/' ? key + 2 : key);
    char upat[1024]; // (Q18) a UDIM set (<UDIM>, <UVTILE>, <name>.1001.png...): one .rudim of its tiles
    if (udim_detect(from, upat, sizeof upat)) {
        char uto[1100];
        udim_asset_name(upat, tdir, uto, sizeof uto);
        const char* nm = uto + strlen(tdir) + 1;
        if (mc_exists(uto) || udim_import(upat, uto, NULL, NULL)) {
            snprintf(n->image, sizeof n->image, "%s/Textures/%s", folder_vpath, nm);
            r->textures++;
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_TEXTURE, owner, nm, "%s", TR("a UDIM set: its tiles in one .rudim"));
        } else {
            snprintf(n->image, sizeof n->image, "%s", upat); // (read from the tiles where they are)
            imprep_add(IMPREP_LOST, IMPREP_CAT_TEXTURE, owner, nm, TR("a UDIM set whose tiles could not be read (%s)"), upat);
        }
        return;
    }
    const char* base = from + strlen(from);
    while (base > from && base[-1] != '/' && base[-1] != '\\') base--;
    char found[1100];
    bool moved = false;
    if (!mc_exists(from) && mc_find_near(src_dir, base, found, sizeof found)) {
        snprintf(from, sizeof from, "%s", found), moved = true;
        base = from + strlen(from); // (its own name: it may be another extension)
        while (base > from && base[-1] != '/' && base[-1] != '\\') base--;
    }
    char to[1100], mv[700], md[1100];
    snprintf(to, sizeof to, "%s/%s", tdir, base);
    if (mc_exists(from) && mc_mirror(from, folder_disk, folder_vpath, mv, sizeof mv, md, sizeof md)) { // (a dropped folder's texture: in place)
        char mdir[1100];
        snprintf(mdir, sizeof mdir, "%s", md);
        char* sl = strrchr(mdir, '/');
        if (sl) *sl = 0, re_make_dirs(mdir);
        if (mc_exists(md) || re_copy_file(from, md)) {
            snprintf(n->image, sizeof n->image, "%s", mv);
            r->textures++;
            imprep_add(IMPREP_SAME, IMPREP_CAT_TEXTURE, owner, base, "%s", TR("the dropped folder's texture, where it was mirrored"));
            return;
        }
    }
    if (mc_exists(to) || re_copy_file(from, to)) {
        snprintf(n->image, sizeof n->image, "%s/Textures/%s", folder_vpath, base);
        r->textures++;
        uint32_t w = 0, h = 0;
        uint8_t* px = mat_load_image_rgba(to, &w, &h);
        if (!px) imprep_add(IMPREP_LOST, IMPREP_CAT_TEXTURE, owner, base, "%s", TR("the Engine can't read this image format (PNG, JPEG, TGA, EXR, HDR, BMP, DDS): convert it"));
        else if (lib && (lib->source == 2 || lib->source == 3)) imprep_add(IMPREP_CONVERTED, IMPREP_CAT_TEXTURE, owner, base, "%s", TR("an image sequence / movie: its first frame"));
        else if (moved) imprep_add(IMPREP_CONVERTED, IMPREP_CAT_TEXTURE, owner, base, "%s", TR("not where the file said: found by its name in the folders around it"));
        else imprep_add(IMPREP_SAME, IMPREP_CAT_TEXTURE, owner, base, "%s", TR("copied to the Textures folder"));
        free(px);
    } else {
        snprintf(n->image, sizeof n->image, "%s", from); // (left where it was: found again when it shows up)
        imprep_add(IMPREP_LOST, IMPREP_CAT_TEXTURE, owner, base, TR("not found (%s): the node stays pointing at it"), from);
    }
}

// (2026-11-23) The PBR maps the file didn't link, found around it by name: the material's ("M_Chair" -> Chair_Normal.png)
// or its color map's (Chair_BaseColor.png -> Chair_Roughness.png, Chair_ORM.png): an Image Texture each, into the
// Principled BSDF's free inputs (a normal through a Normal Map, DirectX's flipped; gloss inverted into roughness; an ORM's
// green into roughness and blue into metallic; opacity cuts the material). True when it added any
static bool mc_glinked(const MatGraph* g, int node, int sock) {
    for (int k = 0; k < g->link_count; k++)
        if (g->links[k].to == node && g->links[k].to_sock == sock) return true;
    return false;
}
static int mc_auto_tex(MatGraph* g, const char* path, bool color, float x, float y, const char* folder_disk, const char* folder_vpath, const char* src_dir, const char* owner, TlendReport* r) {
    int k = mat_graph_add(g, NODE_TEX_IMAGE, x, y);
    if (k < 0) return -1;
    TNode tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.image, sizeof tmp.image, "%s", path);
    mc_image(&tmp, &g->nodes[k], folder_disk, folder_vpath, src_dir, owner, r);
    g->nodes[k].prop[2] = color ? IMAGE_COLORSPACE_SRGB : IMAGE_COLORSPACE_NONCOLOR;
    return k;
}
static bool mc_auto_maps(MatGraph* g, Material* m, const TMat* t, const char* folder_disk, const char* folder_vpath, const char* src_dir, TlendReport* r) {
    int pbr = -1;
    for (int k = 0; k < g->node_count && pbr < 0; k++)
        if (g->nodes[k].type == NODE_BSDF_PRINCIPLED) pbr = k;
    if (pbr < 0) return false;
    mc_near_index(src_dir);
    char keys[2][128];
    int nk = 0;
    mc_key(t->name, keys[nk], sizeof keys[0]);
    if (keys[nk][0]) nk++;
    for (int l = 0; l < g->link_count; l++) { // the color map's name less its ending
        const MatLink* L = &g->links[l];
        if (L->to != pbr || L->to_sock != 0 || g->nodes[L->from].type != NODE_TEX_IMAGE) continue;
        const char* img = g->nodes[L->from].image, *bn = img + strlen(img);
        while (bn > img && bn[-1] != '/' && bn[-1] != '\\') bn--;
        char st[128];
        snprintf(st, sizeof st, "%s", bn);
        char* dot = strrchr(st, '.');
        if (dot) *dot = 0;
        mc_key(st, keys[nk], sizeof keys[0]);
        size_t kl = strlen(keys[nk]);
        for (int e = 0; e < 8 && MC_MAP_END[MCM_BASE][e]; e++) {
            size_t el = strlen(MC_MAP_END[MCM_BASE][e]);
            if (kl > el + 1 && !strcmp(keys[nk] + kl - el, MC_MAP_END[MCM_BASE][e])) {
                keys[nk][kl - el] = 0;
                break;
            }
        }
        if (keys[nk][0] && (nk == 0 || strcmp(keys[nk], keys[0]))) nk++;
        break;
    }
    if (!nk) return false;
    int found[MCM_COUNT];
    for (int s = 0; s < MCM_COUNT; s++) {
        found[s] = -1;
        for (int k = 0; k < nk && found[s] < 0; k++) found[s] = mc_map_find(keys[k], s);
    }
    float x = g->nodes[pbr].x - 520.0f, y = g->nodes[pbr].y;
    bool added = false;
    const char* why = TR("found by the material's name near the file");
    if (found[MCM_BASE] >= 0 && !mc_glinked(g, pbr, 0)) {
        int k = mc_auto_tex(g, g_near[found[MCM_BASE]].path, true, x, y, folder_disk, folder_vpath, src_dir, t->name, r);
        if (k >= 0) mat_graph_link(g, k, 0, pbr, 0), added = true;
    }
    int nslot = found[MCM_NORMAL] >= 0 ? MCM_NORMAL : found[MCM_NORMAL_DX] >= 0 ? MCM_NORMAL_DX : -1;
    if (nslot >= 0 && !mc_glinked(g, pbr, 5)) {
        int k = mc_auto_tex(g, g_near[found[nslot]].path, false, x - 200.0f, y + 420.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        int nm = k >= 0 ? mat_graph_add(g, NODE_NORMAL_MAP, x + 180.0f, y + 420.0f) : -1;
        if (nm >= 0) {
            g->nodes[nm].prop[0] = 0 /* Tangent */, g->nodes[nm].prop[1] = nslot == MCM_NORMAL_DX ? 1 : 0; // (DirectX: green flipped)
            mat_graph_link(g, k, 0, nm, 1), mat_graph_link(g, nm, 0, pbr, 5), added = true;
        }
    }
    if (found[MCM_ORM] >= 0 && !mc_glinked(g, pbr, 2) && !mc_glinked(g, pbr, 1)) { // occlusion, roughness, metallic in R G B
        int k = mc_auto_tex(g, g_near[found[MCM_ORM]].path, false, x - 200.0f, y + 180.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        int sp = k >= 0 ? mat_graph_add(g, NODE_SEPARATE_COLOR, x + 180.0f, y + 180.0f) : -1;
        if (sp >= 0) mat_graph_link(g, k, 0, sp, 0), mat_graph_link(g, sp, 1, pbr, 2), mat_graph_link(g, sp, 2, pbr, 1), added = true;
    }
    if (!mc_glinked(g, pbr, 2) && (found[MCM_ROUGH] >= 0 || found[MCM_GLOSS] >= 0)) {
        bool gloss = found[MCM_ROUGH] < 0;
        int k = mc_auto_tex(g, g_near[found[gloss ? MCM_GLOSS : MCM_ROUGH]].path, false, x, y + 180.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        if (k >= 0 && gloss) {
            int inv = mat_graph_add(g, NODE_INVERT, x + 260.0f, y + 180.0f);
            if (inv >= 0) mat_graph_link(g, k, 0, inv, 1), mat_graph_link(g, inv, 0, pbr, 2), added = true;
        } else if (k >= 0)
            mat_graph_link(g, k, 0, pbr, 2), added = true;
    }
    if (found[MCM_METAL] >= 0 && !mc_glinked(g, pbr, 1)) {
        int k = mc_auto_tex(g, g_near[found[MCM_METAL]].path, false, x, y + 300.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        if (k >= 0) mat_graph_link(g, k, 0, pbr, 1), added = true;
    }
    if (found[MCM_EMIT] >= 0 && !mc_glinked(g, pbr, 26)) {
        int k = mc_auto_tex(g, g_near[found[MCM_EMIT]].path, true, x, y + 640.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        if (k >= 0) mat_graph_link(g, k, 0, pbr, 26), g->nodes[pbr].in[27][0] = fmaxf(g->nodes[pbr].in[27][0], 1.0f), added = true;
    }
    if (found[MCM_ALPHA] >= 0 && !mc_glinked(g, pbr, 4)) {
        int k = mc_auto_tex(g, g_near[found[MCM_ALPHA]].path, false, x, y + 760.0f, folder_disk, folder_vpath, src_dir, t->name, r);
        if (k >= 0) {
            mat_graph_link(g, k, 0, pbr, 4), added = true;
            m->blend_mode = MAT_BLEND_MASKED, m->two_sided = true, m->opacity_clip = MAT_OPACITY_CLIP_DEFAULT;
        }
    }
    if (added) imprep_add(IMPREP_CONVERTED, IMPREP_CAT_TEXTURE, t->name, keys[0], "%s", why);
    return added;
}
