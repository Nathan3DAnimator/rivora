// The physics' collision shapes (physics_shapes.h, notas-dev/fisica.md "Formas de colisão").
//   hulls: the mesh's extreme points along K directions (they are vertices of its hull) -> an incremental hull
//          (quickhull's steps: the visible faces out, the horizon closed to the new point) -> at most 64 vertices (fewer
//          directions until it fits) -> coplanar triangles merged into faces -> volume, center and inertia
//   meshes: triangles in a BVH (median splits, 4 per leaf)
//   pairs:  GJK on the cores (distance and closest points; far apart: out at once), EPA when the cores overlap, then
//           the supporting features (a face, an edge or a point) clipped one against the other: the reference face is
//           the one best aligned with the normal, the incident one clipped to its sides; up to 4 points kept (the
//           deepest, the farthest from it, the widest triangle, the other side)
#include "physics_shapes.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static volatile int32_t g_gjk, g_epa, g_fallback; // (atomic: the narrowphase runs on every worker)

void phys_shape_counters(uint32_t* gjk, uint32_t* epa, uint32_t* epa_fallback) {
    *gjk = (uint32_t)re_atomic_load(&g_gjk), *epa = (uint32_t)re_atomic_load(&g_epa), *epa_fallback = (uint32_t)re_atomic_load(&g_fallback);
}

// --- Small vector math ---

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static float norm3(float a[3]) {
    float l = len3(a);
    if (l > 1e-20f) a[0] /= l, a[1] /= l, a[2] /= l;
    return l;
}
static void perp3(const float n[3], float u[3], float v[3]) {
    if (fabsf(n[2]) < 0.9f) u[0] = -n[1], u[1] = n[0], u[2] = 0.0f;
    else u[0] = 0.0f, u[1] = -n[2], u[2] = n[1];
    norm3(u);
    cross3(n, u, v);
}
static void local_to_world(const Cvx* c, const float l[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = c->c[k] + l[0] * c->R[0][k] + l[1] * c->R[1][k] + l[2] * c->R[2][k];
}

// --- Supports and features ---

static void support(const Cvx* c, const float d[3], float o[3]) {
    switch (c->kind) {
    case CVX_POINT: memcpy(o, c->c, 12); return;
    case CVX_SEGMENT: {
        const float* a = c->R[c->axis];
        float s = dot3(a, d) >= 0.0f ? c->h[c->axis] : -c->h[c->axis];
        for (int k = 0; k < 3; k++) o[k] = c->c[k] + a[k] * s;
        return;
    }
    case CVX_BOX:
        memcpy(o, c->c, 12);
        for (int a = 0; a < 3; a++) {
            float s = dot3(c->R[a], d) >= 0.0f ? c->h[a] : -c->h[a];
            for (int k = 0; k < 3; k++) o[k] += c->R[a][k] * s;
        }
        return;
    case CVX_CYLINDER: {
        const float* a = c->R[2];
        float ad = dot3(a, d), r[3] = { d[0] - a[0] * ad, d[1] - a[1] * ad, d[2] - a[2] * ad }, l2 = dot3(r, r);
        // (d along the axis: what is left of it is rounding noise, pointing anywhere, even along the axis: no radius)
        float s = ad >= 0.0f ? c->h[2] : -c->h[2], q = l2 > 1e-8f * dot3(d, d) ? c->h[0] / sqrtf(l2) : 0.0f;
        for (int k = 0; k < 3; k++) o[k] = c->c[k] + a[k] * s + r[k] * q;
        return;
    }
    case CVX_HULL: {
        const PhysHull* h = c->hull;
        float dl[3];
        for (int k = 0; k < 3; k++) dl[k] = dot3(c->R[k], d) * c->s[k];
        uint32_t best = 0;
        float bd = -1e30f;
        for (uint32_t i = 0; i < h->nv; i++) {
            float x = dot3(h->v[i], dl);
            if (x > bd) bd = x, best = i;
        }
        float l[3];
        for (int k = 0; k < 3; k++) l[k] = h->v[best][k] * c->s[k] + c->o[k];
        local_to_world(c, l, o);
        return;
    }
    case CVX_TRIANGLE: {
        float d0 = dot3(c->t[0], d), d1 = dot3(c->t[1], d), d2 = dot3(c->t[2], d);
        memcpy(o, c->t[d0 >= d1 && d0 >= d2 ? 0 : d1 >= d2 ? 1 : 2], 12);
        return;
    }
    }
    memcpy(o, c->c, 12);
}

#define FEAT_MAX 64
#define CYL_RING 16

// The feature of c that faces d: a polygon (>= 3 points; n its outward normal), a segment (2) or a point (1)
static int feature(const Cvx* c, const float d[3], float out[FEAT_MAX][3], float n[3]) {
    switch (c->kind) {
    case CVX_POINT: memcpy(out[0], c->c, 12); return 1;
    case CVX_SEGMENT: {
        const float* a = c->R[c->axis];
        float hl = c->h[c->axis];
        for (int k = 0; k < 3; k++) out[0][k] = c->c[k] - a[k] * hl, out[1][k] = c->c[k] + a[k] * hl;
        return 2;
    }
    case CVX_BOX: {
        int ax = 0;
        float best = -1.0f;
        for (int k = 0; k < 3; k++)
            if (fabsf(dot3(c->R[k], d)) > best) best = fabsf(dot3(c->R[k], d)), ax = k;
        float s = dot3(c->R[ax], d) >= 0.0f ? 1.0f : -1.0f;
        int u = (ax + 1) % 3, v = (ax + 2) % 3;
        static const float su[4] = { 1, -1, -1, 1 }, sv[4] = { 1, 1, -1, -1 };
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 3; k++)
                out[i][k] = c->c[k] + c->R[ax][k] * c->h[ax] * s + c->R[u][k] * c->h[u] * su[i] + c->R[v][k] * c->h[v] * sv[i];
        for (int k = 0; k < 3; k++) n[k] = c->R[ax][k] * s;
        return 4;
    }
    case CVX_CYLINDER: {
        const float* a = c->R[2];
        float ad = dot3(a, d);
        if (fabsf(ad) > 0.7071f) { // a cap (a 16-gon inside the circle)
            static const float ring[CYL_RING][2] = { { 1.0f, 0.0f }, { 0.92388f, 0.382683f }, { 0.707107f, 0.707107f }, { 0.382683f, 0.92388f },
                                                     { 0.0f, 1.0f }, { -0.382683f, 0.92388f }, { -0.707107f, 0.707107f }, { -0.92388f, 0.382683f },
                                                     { -1.0f, 0.0f }, { -0.92388f, -0.382683f }, { -0.707107f, -0.707107f }, { -0.382683f, -0.92388f },
                                                     { 0.0f, -1.0f }, { 0.382683f, -0.92388f }, { 0.707107f, -0.707107f }, { 0.92388f, -0.382683f } };
            float s = ad >= 0.0f ? 1.0f : -1.0f;
            for (int i = 0; i < CYL_RING; i++) {
                float cs = ring[i][0] * c->h[0], sn = ring[i][1] * c->h[0];
                for (int k = 0; k < 3; k++) out[i][k] = c->c[k] + a[k] * c->h[2] * s + c->R[0][k] * cs + c->R[1][k] * sn;
            }
            for (int k = 0; k < 3; k++) n[k] = a[k] * s;
            return CYL_RING;
        }
        float r[3] = { d[0] - a[0] * ad, d[1] - a[1] * ad, d[2] - a[2] * ad };
        norm3(r);
        for (int k = 0; k < 3; k++) {
            out[0][k] = c->c[k] - a[k] * c->h[2] + r[k] * c->h[0];
            out[1][k] = c->c[k] + a[k] * c->h[2] + r[k] * c->h[0];
        }
        return 2; // the side: a line along the axis
    }
    case CVX_HULL: {
        const PhysHull* h = c->hull;
        float dl[3];
        for (int k = 0; k < 3; k++) dl[k] = dot3(c->R[k], d);
        uint32_t best = 0;
        float bs = -1e30f, bn[3] = { 0, 0, 1 };
        if (c->s[0] == c->s[1] && c->s[1] == c->s[2]) { // a uniform scale: the normals as they are (a mirror flips them)
            float sg = c->s[0] < 0.0f ? -1.0f : 1.0f;
            for (uint32_t f = 0; f < h->nf; f++) {
                float x = dot3(h->n[f], dl) * sg;
                if (x > bs) bs = x, best = f;
            }
            for (int k = 0; k < 3; k++) bn[k] = h->n[best][k] * sg;
        } else
            for (uint32_t f = 0; f < h->nf; f++) { // (a scaled normal: n / s, the inverse transpose)
                float ns[3] = { h->n[f][0] / c->s[0], h->n[f][1] / c->s[1], h->n[f][2] / c->s[2] };
                float l = len3(ns), x = dot3(ns, dl) / (l > 1e-20f ? l : 1.0f);
                if (x > bs) bs = x, best = f, bn[0] = ns[0] / l, bn[1] = ns[1] / l, bn[2] = ns[2] / l;
            }
        int m = h->count[best] > FEAT_MAX ? FEAT_MAX : h->count[best];
        for (int i = 0; i < m; i++) {
            const float* v = h->v[h->idx[h->first[best] + i]];
            float l[3] = { v[0] * c->s[0] + c->o[0], v[1] * c->s[1] + c->o[1], v[2] * c->s[2] + c->o[2] };
            local_to_world(c, l, out[i]);
        }
        for (int k = 0; k < 3; k++) n[k] = bn[0] * c->R[0][k] + bn[1] * c->R[1][k] + bn[2] * c->R[2][k];
        return m;
    }
    case CVX_TRIANGLE:
        memcpy(out, c->t, 36);
        memcpy(n, c->tn, 12);
        return 3;
    }
    memcpy(out[0], c->c, 12);
    return 1;
}

// --- GJK (in double: a floor 40 m wide against a body of 30 cm lost the closest point in float, and GJK said "apart"
// with the body 10 cm into it; the scalar math costs the same) ---

typedef struct SV { double w[3]; float a[3], b[3]; } SV; // a point of A - B and the two it came from
typedef struct Simplex { SV v[4]; double l[4]; int n; } Simplex;

static double dotd(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void subd(const double a[3], const double b[3], double o[3]) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static void crossd(const double a[3], const double b[3], double o[3]) {
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static double normd(double a[3]) {
    double l = sqrt(dotd(a, a));
    if (l > 1e-300) a[0] /= l, a[1] /= l, a[2] /= l;
    return l;
}

static void sv_make(const Cvx* A, const Cvx* B, const double d[3], SV* s) { // (a support needs no unit direction)
    float df[3] = { (float)d[0], (float)d[1], (float)d[2] }, nd[3] = { -df[0], -df[1], -df[2] };
    support(A, df, s->a), support(B, nd, s->b);
    for (int c = 0; c < 3; c++) s->w[c] = (double)s->a[c] - (double)s->b[c];
}

static void keep1(Simplex* s, int i) {
    s->v[0] = s->v[i];
    s->l[0] = 1.0, s->n = 1;
}
static void keep2(Simplex* s, int i, int j, double t) { // (1 - t) i + t j
    SV a = s->v[i], b = s->v[j];
    s->v[0] = a, s->v[1] = b;
    s->l[0] = 1.0 - t, s->l[1] = t, s->n = 2;
}

static void closest_seg(Simplex* s) {
    const double* a = s->v[0].w;
    double ab[3];
    subd(s->v[1].w, a, ab);
    double den = dotd(ab, ab), t = den > 1e-30 ? -dotd(a, ab) / den : 0.0;
    if (t <= 0.0) keep1(s, 0);
    else if (t >= 1.0) keep1(s, 1);
    else s->l[0] = 1.0 - t, s->l[1] = t;
}

// Ericson's closest point on a triangle (to the origin), Real-Time Collision Detection 5.1.5
static void closest_tri(Simplex* s) {
    const double *a = s->v[0].w, *b = s->v[1].w, *c = s->v[2].w;
    double ab[3], ac[3];
    subd(b, a, ab), subd(c, a, ac);
    double d1 = -dotd(ab, a), d2 = -dotd(ac, a);
    if (d1 <= 0.0 && d2 <= 0.0) { keep1(s, 0); return; }
    double d3 = -dotd(ab, b), d4 = -dotd(ac, b);
    if (d3 >= 0.0 && d4 <= d3) { keep1(s, 1); return; }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) { keep2(s, 0, 1, d1 / (d1 - d3)); return; }
    double d5 = -dotd(ab, c), d6 = -dotd(ac, c);
    if (d6 >= 0.0 && d5 <= d6) { keep1(s, 2); return; }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) { keep2(s, 0, 2, d2 / (d2 - d6)); return; }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) { keep2(s, 1, 2, (d4 - d3) / ((d4 - d3) + (d5 - d6))); return; }
    double den = va + vb + vc;
    if (fabs(den) < 1e-30) { // (a flat triangle: an edge)
        keep2(s, 0, 1, 0.5);
        closest_seg(s);
        return;
    }
    double v = vb / den, w = vc / den;
    s->l[0] = 1.0 - v - w, s->l[1] = v, s->l[2] = w;
}

static void simplex_point(const Simplex* s, double v[3]) {
    v[0] = v[1] = v[2] = 0.0;
    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < 3; k++) v[k] += s->v[i].w[k] * s->l[i];
}

// True when the origin is inside the tetrahedron; otherwise reduced to the closest face's feature
static bool closest_tet(Simplex* s) {
    static const int F[4][4] = { { 0, 1, 2, 3 }, { 0, 3, 1, 2 }, { 0, 2, 3, 1 }, { 1, 3, 2, 0 } };
    double e1[3], e2[3], e3[3], cr[3];
    subd(s->v[1].w, s->v[0].w, e1), subd(s->v[2].w, s->v[0].w, e2), subd(s->v[3].w, s->v[0].w, e3);
    crossd(e1, e2, cr);
    double vol = dotd(cr, e3), scale = dotd(e1, e1) + dotd(e2, e2) + dotd(e3, e3);
    if (fabs(vol) < 1e-12 * scale * sqrt(scale)) { // flat: the newest point added nothing
        s->n = 3;
        closest_tri(s);
        return false;
    }
    bool outside = false;
    Simplex best = *s;
    double bd = 1e300;
    for (int f = 0; f < 4; f++) {
        const double *a = s->v[F[f][0]].w, *b = s->v[F[f][1]].w, *c = s->v[F[f][2]].w, *o = s->v[F[f][3]].w;
        double ab[3], ac[3], n[3], ao[3];
        subd(b, a, ab), subd(c, a, ac), crossd(ab, ac, n);
        subd(o, a, ao);
        if (dotd(n, ao) > 0.0) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
        if (-dotd(n, a) <= 0.0) continue; // the origin on the inner side of this face
        outside = true;
        Simplex t;
        t.v[0] = s->v[F[f][0]], t.v[1] = s->v[F[f][1]], t.v[2] = s->v[F[f][2]], t.n = 3;
        closest_tri(&t);
        double p[3];
        simplex_point(&t, p);
        if (dotd(p, p) < bd) bd = dotd(p, p), best = t;
    }
    if (!outside) return true;
    *s = best;
    return false;
}

// The cores' distance: true and the closest points when apart (far: farther than maxd, nothing else computed);
// false when they overlap (s: the simplex EPA starts from). vout: the closest point of A - B to the origin
static bool gjk(const Cvx* A, const Cvx* B, float maxd, Simplex* s, float* dist, float pa[3], float pb[3], float vout[3], bool* far) {
    re_atomic_add(&g_gjk, 1);
    double v[3] = { (double)A->c[0] - B->c[0], (double)A->c[1] - B->c[1], (double)A->c[2] - B->c[2] };
    if (dotd(v, v) < 1e-12) v[0] = 1.0, v[1] = v[2] = 0.0;
    s->n = 0;
    *far = false;
    double md2 = (double)maxd * maxd;
    for (int it = 0; it < 64; it++) {
        double d[3] = { -v[0], -v[1], -v[2] };
        SV sv;
        sv_make(A, B, d, &sv);
        double vv = dotd(v, v), vw = dotd(v, sv.w);
        if (vw > 0.0 && vw * vw > vv * md2) {
            *far = true;
            return true;
        }
        if (s->n && vv - vw <= 1e-10 * vv + 1e-12) break; // no progress: v is the closest point
        bool dup = false;
        for (int i = 0; i < s->n && !dup; i++) {
            double e[3];
            subd(sv.w, s->v[i].w, e);
            dup = dotd(e, e) < 1e-14 * (1.0 + dotd(sv.w, sv.w));
        }
        if (dup) break;
        s->v[s->n++] = sv;
        if (s->n == 1) s->l[0] = 1.0;
        else if (s->n == 2) closest_seg(s);
        else if (s->n == 3) closest_tri(s);
        else if (closest_tet(s)) return false;
        simplex_point(s, v);
        if (dotd(v, v) < 1e-12) return false; // touching: as overlapping
    }
    double a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < 3; k++) a[k] += s->v[i].a[k] * s->l[i], b[k] += s->v[i].b[k] * s->l[i];
    for (int k = 0; k < 3; k++) pa[k] = (float)a[k], pb[k] = (float)b[k], vout[k] = (float)v[k];
    *dist = (float)sqrt(dotd(v, v));
    return true;
}

// --- EPA (double, as GJK) ---

#define EPA_V 96
#define EPA_F 192
typedef struct EFace { int a, b, c; double n[3], d; } EFace;

static bool epa_face(const SV* V, int a, int b, int c, EFace* f) {
    double ab[3], ac[3];
    subd(V[b].w, V[a].w, ab), subd(V[c].w, V[a].w, ac);
    crossd(ab, ac, f->n);
    if (normd(f->n) < 1e-18) return false;
    f->a = a, f->b = b, f->c = c;
    f->d = dotd(f->n, V[a].w);
    return true;
}

static void barycentric(const double a[3], const double b[3], const double c[3], const double p[3], double l[3]) {
    double v0[3], v1[3], v2[3];
    subd(b, a, v0), subd(c, a, v1), subd(p, a, v2);
    double d00 = dotd(v0, v0), d01 = dotd(v0, v1), d11 = dotd(v1, v1), d20 = dotd(v2, v0), d21 = dotd(v2, v1);
    double den = d00 * d11 - d01 * d01;
    if (fabs(den) < 1e-30) {
        l[0] = 1.0, l[1] = l[2] = 0.0;
        return;
    }
    l[1] = (d11 * d20 - d01 * d21) / den, l[2] = (d00 * d21 - d01 * d20) / den, l[0] = 1.0 - l[1] - l[2];
}

// The penetration: the polytope grown toward the face nearest the origin. nf: that face's outward normal (A moves by
// -nf * depth to separate)
static bool epa(const Cvx* A, const Cvx* B, const Simplex* s, float nf[3], float* depth, float pa[3], float pb[3]) {
    re_atomic_add(&g_epa, 1);
    SV V[EPA_V];
    int nv = s->n;
    for (int i = 0; i < nv; i++) V[i] = s->v[i];
    // a tetrahedron (the simplex grown along the axes when GJK stopped short of one)
    static const double dirs[14][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0.577, 0.577, 0.577 },
                                        { -0.577, -0.577, 0.577 }, { 0.577, -0.577, -0.577 }, { -0.577, 0.577, -0.577 },
                                        { 0.577, -0.577, 0.577 }, { -0.577, 0.577, 0.577 }, { 0.577, 0.577, -0.577 }, { -0.577, -0.577, -0.577 } };
    for (int k = 0; k < 14 && nv < 4; k++) {
        SV p;
        sv_make(A, B, dirs[k], &p);
        double e[3];
        bool adds = false;
        if (nv == 0) adds = true;
        else if (nv == 1) subd(p.w, V[0].w, e), adds = dotd(e, e) > 1e-6;
        else if (nv == 2) {
            double ab[3], ap[3], c[3];
            subd(V[1].w, V[0].w, ab), subd(p.w, V[0].w, ap), crossd(ab, ap, c);
            adds = dotd(c, c) > 1e-6 * dotd(ab, ab);
        } else {
            double ab[3], ac[3], n[3], ap[3];
            subd(V[1].w, V[0].w, ab), subd(V[2].w, V[0].w, ac), crossd(ab, ac, n), subd(p.w, V[0].w, ap);
            double l = sqrt(dotd(n, n));
            adds = l > 1e-18 && fabs(dotd(n, ap)) / l > 1e-3;
        }
        if (adds) V[nv++] = p;
    }
    if (nv < 4) return false;
    EFace F[EPA_F];
    int nf_ = 0;
    {
        double cen[3];
        for (int k = 0; k < 3; k++) cen[k] = (V[0].w[k] + V[1].w[k] + V[2].w[k] + V[3].w[k]) * 0.25;
        static const int T[4][3] = { { 0, 1, 2 }, { 0, 3, 1 }, { 0, 2, 3 }, { 1, 3, 2 } };
        for (int t = 0; t < 4; t++) {
            int a = T[t][0], b = T[t][1], c = T[t][2];
            EFace f;
            if (!epa_face(V, a, b, c, &f)) return false;
            double ac[3];
            subd(V[a].w, cen, ac);
            if (dotd(f.n, ac) < 0.0 && !epa_face(V, a, c, b, &f)) return false;
            F[nf_++] = f;
        }
    }
    double scale = 0.0; // (the visibility tolerance grows with the polytope)
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) scale = fmax(scale, fabs(V[i].w[k]));
    int best = 0;
    bool done = false;
    for (int it = 0; it < 64; it++) {
        best = 0;
        for (int i = 1; i < nf_; i++)
            if (F[i].d < F[best].d) best = i;
        SV p;
        sv_make(A, B, F[best].n, &p);
        double dp = dotd(p.w, F[best].n);
        if (dp - F[best].d < 0.005 + 1e-5 * fabs(dp)) {
            done = true;
            break;
        }
        if (nv >= EPA_V) break;
        for (int k = 0; k < 3; k++) scale = fmax(scale, fabs(p.w[k]));
        double vis = 1e-7 + 1e-10 * scale;
        int ni = nv;
        V[nv++] = p;
        int E[EPA_F * 3][2], ne = 0;
        for (int i = nf_ - 1; i >= 0; i--) {
            double ap[3];
            subd(p.w, V[F[i].a].w, ap);
            if (dotd(F[i].n, ap) <= vis) continue;
            int ed[3][2] = { { F[i].a, F[i].b }, { F[i].b, F[i].c }, { F[i].c, F[i].a } };
            for (int e = 0; e < 3; e++) {
                int found = -1;
                for (int j = 0; j < ne && found < 0; j++)
                    if (E[j][0] == ed[e][1] && E[j][1] == ed[e][0]) found = j;
                if (found >= 0) E[found][0] = E[ne - 1][0], E[found][1] = E[ne - 1][1], ne--;
                else if (ne < EPA_F * 3) E[ne][0] = ed[e][0], E[ne][1] = ed[e][1], ne++;
            }
            F[i] = F[--nf_];
        }
        bool full = false;
        for (int e = 0; e < ne; e++) {
            if (nf_ >= EPA_F) { full = true; break; }
            EFace f;
            if (epa_face(V, E[e][0], E[e][1], ni, &f)) F[nf_++] = f;
        }
        if (full || nf_ < 4) break;
    }
    if (nf_ <= 0) return false;
    best = 0;
    for (int i = 1; i < nf_; i++)
        if (F[i].d < F[best].d) best = i;
    const EFace* f = &F[best];
    for (int k = 0; k < 3; k++) nf[k] = (float)f->n[k];
    *depth = (float)f->d;
    double q[3] = { f->n[0] * f->d, f->n[1] * f->d, f->n[2] * f->d }, l[3];
    barycentric(V[f->a].w, V[f->b].w, V[f->c].w, q, l);
    for (int k = 0; k < 3; k++) {
        pa[k] = (float)(V[f->a].a[k] * l[0] + V[f->b].a[k] * l[1] + V[f->c].a[k] * l[2]);
        pb[k] = (float)(V[f->a].b[k] * l[0] + V[f->b].b[k] * l[1] + V[f->c].b[k] * l[2]);
    }
    return done && f->d > -1.0;
}

// When EPA can't (a flat Minkowski difference: a point against a triangle in its plane): the least overlap among a few
// axes (the centers, the triangle's normal, the boxes' axes)
static float sep_along(const Cvx* A, const Cvx* B, const float u[3]) {
    float nu[3] = { -u[0], -u[1], -u[2] }, a[3], b[3];
    support(A, nu, a), support(B, u, b);
    return dot3(a, u) - dot3(b, u);
}
static void fallback_axis(const Cvx* A, const Cvx* B, float n[3], float* sep, float pa[3], float pb[3]) {
    float cand[16][3];
    int nc = 0;
    sub3(A->c, B->c, cand[nc]);
    if (norm3(cand[nc]) > 1e-6f) nc++;
    if (B->kind == CVX_TRIANGLE) memcpy(cand[nc++], B->tn, 12);
    for (int k = 0; k < 3; k++) {
        memcpy(cand[nc++], A->R[k], 12);
        if (B->kind != CVX_TRIANGLE) memcpy(cand[nc++], B->R[k], 12);
    }
    float best = -1e30f;
    for (int i = 0; i < nc; i++)
        for (int sg = 0; sg < 2; sg++) {
            float u[3] = { cand[i][0] * (sg ? -1.0f : 1.0f), cand[i][1] * (sg ? -1.0f : 1.0f), cand[i][2] * (sg ? -1.0f : 1.0f) };
            float s = sep_along(A, B, u);
            if (s > best) best = s, memcpy(n, u, 12);
        }
    *sep = best;
    float nn[3] = { -n[0], -n[1], -n[2] };
    support(A, nn, pa), support(B, n, pb);
}

// --- The manifold ---

static int clip_poly(const float in[][3], int n, const float pn[3], float d, float out[][3], int cap) {
    int m = 0;
    for (int i = 0; i < n && m < cap; i++) {
        const float* a = in[i];
        const float* b = in[(i + 1) % n];
        float da = dot3(pn, a) - d, db = dot3(pn, b) - d;
        if (da <= 0.0f) memcpy(out[m++], a, 12);
        if ((da <= 0.0f) != (db <= 0.0f) && m < cap) {
            float t = da / (da - db);
            for (int k = 0; k < 3; k++) out[m][k] = a[k] + (b[k] - a[k]) * t;
            m++;
        }
    }
    return m;
}

static int clip_seg(float p[2][3], const float pn[3], float d) {
    float da = dot3(pn, p[0]) - d, db = dot3(pn, p[1]) - d;
    if (da > 0.0f && db > 0.0f) return 0;
    if (da > 0.0f || db > 0.0f) {
        float t = da / (da - db), q[3];
        for (int k = 0; k < 3; k++) q[k] = p[0][k] + (p[1][k] - p[0][k]) * t;
        memcpy(p[da > 0.0f ? 0 : 1], q, 12);
    }
    return 2;
}

#define CLIP_CAP 160

// The incident feature clipped to the reference polygon's sides (in place); returns its points
static int clip_to_ref(float inc[CLIP_CAP][3], int ni, const float ref[][3], int nr, const float nref[3]) {
    float cen[3] = { 0, 0, 0 };
    for (int i = 0; i < nr; i++)
        for (int k = 0; k < 3; k++) cen[k] += ref[i][k] / (float)nr;
    float tmp[CLIP_CAP][3];
    for (int i = 0; i < nr && ni > 0; i++) {
        const float* a = ref[i];
        const float* b = ref[(i + 1) % nr];
        float e[3], sn[3], ca[3];
        sub3(b, a, e);
        cross3(e, nref, sn);
        if (norm3(sn) < 1e-9f) continue;
        sub3(cen, a, ca);
        if (dot3(sn, ca) > 0.0f) sn[0] = -sn[0], sn[1] = -sn[1], sn[2] = -sn[2];
        float d = dot3(sn, a) + 1e-4f;
        if (ni == 1) ni = dot3(sn, inc[0]) <= d ? 1 : 0;
        else if (ni == 2) ni = clip_seg(inc, sn, d);
        else {
            ni = clip_poly((const float(*)[3])inc, ni, sn, d, tmp, CLIP_CAP);
            memcpy(inc, tmp, sizeof(float) * 3 * (size_t)ni);
        }
    }
    return ni;
}

static void add_point(PhysManifold* m, float pts[][3], float* dep, int* np, int cap, const float p[3], float depth) {
    (void)m;
    if (*np >= cap) return;
    memcpy(pts[*np], p, 12);
    dep[*np] = depth;
    (*np)++;
}

// At most 4 of the points: the deepest, the farthest from it, the widest triangle, the far side
static void reduce(PhysManifold* m, float pts[][3], const float* dep, int np) {
    if (np <= 4) {
        for (int i = 0; i < np; i++) memcpy(m->p[i], pts[i], 12), m->depth[i] = dep[i];
        m->np = (uint32_t)np;
        return;
    }
    int k[4] = { 0, -1, -1, -1 };
    for (int i = 1; i < np; i++)
        if (dep[i] > dep[k[0]]) k[0] = i;
    float bd = -1.0f;
    for (int i = 0; i < np; i++) {
        float e[3];
        sub3(pts[i], pts[k[0]], e);
        if (dot3(e, e) > bd) bd = dot3(e, e), k[1] = i;
    }
    float ab[3], area[CLIP_CAP];
    sub3(pts[k[1]], pts[k[0]], ab);
    bd = -1.0f;
    for (int i = 0; i < np; i++) {
        float ap[3], c[3];
        sub3(pts[i], pts[k[0]], ap), cross3(ab, ap, c);
        area[i] = dot3(c, m->n);
        if (fabsf(area[i]) > bd) bd = fabsf(area[i]), k[2] = i;
    }
    float side = area[k[2]] >= 0.0f ? -1.0f : 1.0f;
    bd = 0.0f;
    for (int i = 0; i < np; i++)
        if (area[i] * side > bd) bd = area[i] * side, k[3] = i;
    m->np = 0;
    for (int j = 0; j < 4; j++) {
        if (k[j] < 0) continue;
        bool seen = false;
        for (int q = 0; q < j; q++) seen |= k[q] == k[j];
        if (seen) continue;
        memcpy(m->p[m->np], pts[k[j]], 12);
        m->depth[m->np++] = dep[k[j]];
    }
}

bool phys_collide(const Cvx* A, const Cvx* B, float margin, PhysManifold* m) {
    float rs = A->radius + B->radius, maxd = rs + margin;
    Simplex s;
    float dist = 0.0f, pa[3], pb[3], n[3], v[3], sep;
    bool far;
    bool apart = gjk(A, B, maxd, &s, &dist, pa, pb, v, &far);
    if (apart && far) return false;
    // (the normal from the simplex's point: the difference of the closest points cancels when the shapes are large, a
    // floor 40 m wide, and a gap under 0.01 cm is touching: EPA's)
    if (apart && dist > 0.01f) {
        if (dist > maxd) return false;
        for (int k = 0; k < 3; k++) n[k] = v[k] / dist;
        sep = dist - rs;
    } else {
        float nf[3], depth, fn[3], fsep, fa[3], fb[3], se = -1e30f;
        bool ok = epa(A, B, &s, nf, &depth, pa, pb);
        // EPA's normal measured (the shapes' real overlap along it, not the polytope's) and checked against the shapes'
        // own axes: the least overlap of them wins. A sound EPA is that least; a lost one (a curved shape's polytope that
        // missed the origin) said too little or too much
        if (ok) {
            n[0] = -nf[0], n[1] = -nf[1], n[2] = -nf[2];
            se = sep_along(A, B, n);
        }
        fallback_axis(A, B, fn, &fsep, fa, fb);
        if (!ok || fsep > se + 0.02f + 1e-3f * fabsf(se)) {
            re_atomic_add(&g_fallback, 1);
            memcpy(n, fn, 12), memcpy(pa, fa, 12), memcpy(pb, fb, 12);
            sep = fsep - rs;
        } else
            sep = se - rs;
        if (sep > margin) return false;
    }
    if (norm3(n) < 1e-6f) n[0] = n[1] = 0.0f, n[2] = 1.0f;
    memcpy(m->n, n, 12);
    m->np = 0;
    float FA[FEAT_MAX][3], FB[FEAT_MAX][3], nA[3] = { 0, 0, 0 }, nB[3] = { 0, 0, 0 }, dn[3] = { -n[0], -n[1], -n[2] };
    int ka = feature(A, dn, FA, nA), kb = feature(B, n, FB, nB);
    float alA = ka >= 3 ? dot3(nA, dn) : -1.0f, alB = kb >= 3 ? dot3(nB, n) : -1.0f;
    const float ALIGN = 0.9f; // (25 degrees: a face that much off the normal is not the one resting)
    float pts[CLIP_CAP][3], dep[CLIP_CAP];
    int np = 0;
    if (alA >= ALIGN || alB >= ALIGN) {
        bool refB = alB >= ALIGN && (alA < ALIGN || alB >= alA - 0.02f); // (a tie: B's, the static's or the triangle's)
        const float(*ref)[3] = refB ? (const float(*)[3])FB : (const float(*)[3])FA;
        const float* nref = refB ? nB : nA; // outward, toward the other
        int nr = refB ? kb : ka, ni = refB ? ka : kb;
        float r_inc = refB ? A->radius : B->radius, r_ref = refB ? B->radius : A->radius;
        float inc[CLIP_CAP][3];
        memcpy(inc, refB ? FA : FB, sizeof(float) * 3 * (size_t)ni);
        ni = clip_to_ref(inc, ni, ref, nr, nref);
        float mn[3];
        for (int k = 0; k < 3; k++) mn[k] = refB ? nref[k] : -nref[k];
        memcpy(m->n, mn, 12);
        for (int i = 0; i < ni; i++) {
            float e[3];
            sub3(inc[i], ref[0], e);
            float dq = dot3(nref, e), sp = dq - rs;
            if (sp > margin) continue;
            float p[3];
            for (int k = 0; k < 3; k++) {
                float qi = inc[i][k] - nref[k] * r_inc, qr = inc[i][k] - nref[k] * dq + nref[k] * r_ref;
                p[k] = (qi + qr) * 0.5f;
            }
            add_point(m, pts, dep, &np, CLIP_CAP, p, -sp);
        }
        // a mesh's triangle facing it with nothing over it: the contact is the neighbor triangle's (seen across their
        // shared edge it leans: a ball bounced off a flat step's diagonal)
        if (!np && refB && B->kind == CVX_TRIANGLE) return false;
    } else if (ka == 2 && kb == 2) { // two segments side by side (capsules, a cylinder's side): the overlap of their extents
        float da[3], db[3];
        sub3(FA[1], FA[0], da), sub3(FB[1], FB[0], db);
        float la = norm3(da), lb = norm3(db);
        if (la > 1e-4f && lb > 1e-4f && fabsf(dot3(da, db)) > 0.98f && fabsf(dot3(da, n)) < 0.2f) {
            float seg[2][3];
            memcpy(seg, FA, 24);
            float nb0[3] = { -db[0], -db[1], -db[2] };
            int k2 = clip_seg(seg, nb0, dot3(nb0, FB[0]) + 1e-4f);
            if (k2) k2 = clip_seg(seg, db, dot3(db, FB[1]) + 1e-4f);
            for (int i = 0; i < k2; i++) {
                float e[3];
                sub3(seg[i], FB[0], e);
                float dq = dot3(n, e), sp = dq - rs, p[3];
                if (sp > margin) continue;
                for (int k = 0; k < 3; k++) p[k] = (seg[i][k] - n[k] * A->radius + seg[i][k] - n[k] * dq + n[k] * B->radius) * 0.5f;
                add_point(m, pts, dep, &np, CLIP_CAP, p, -sp);
            }
        }
    }
    if (!np) { // a single point: the closest (or deepest) pair
        // (a mesh's triangle seen across its edge, leaning less than 45 degrees, with the shape not over it: the
        // neighbor's contact, the internal edge's ghost: a ball dropped on a flat step bounced sideways)
        if (B->kind == CVX_TRIANGLE && dot3(n, B->tn) > 0.7f) {
            float q[3], l[3], d = dot3(B->tn, pa) - dot3(B->tn, B->t[0]);
            for (int k = 0; k < 3; k++) q[k] = pa[k] - B->tn[k] * d;
            double qd[3] = { q[0], q[1], q[2] }, t0[3] = { B->t[0][0], B->t[0][1], B->t[0][2] }, t1[3] = { B->t[1][0], B->t[1][1], B->t[1][2] },
                   t2[3] = { B->t[2][0], B->t[2][1], B->t[2][2] }, ld[3];
            barycentric(t0, t1, t2, qd, ld);
            for (int k = 0; k < 3; k++) l[k] = (float)ld[k];
            if (l[0] < -1e-3f || l[1] < -1e-3f || l[2] < -1e-3f) return false;
        }
        memcpy(m->n, n, 12);
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = (pa[k] - n[k] * A->radius + pb[k] + n[k] * B->radius) * 0.5f;
        add_point(m, pts, dep, &np, CLIP_CAP, p, -sep);
    }
    reduce(m, pts, dep, np);
    return m->np > 0;
}

// --- Convex hulls ---

typedef struct HTri { int a, b, c; float n[3], d; } HTri;

static bool htri(const float (*p)[3], int a, int b, int c, HTri* t) {
    float ab[3], ac[3];
    sub3(p[b], p[a], ab), sub3(p[c], p[a], ac);
    cross3(ab, ac, t->n);
    if (norm3(t->n) < 1e-12f) return false;
    t->a = a, t->b = b, t->c = c, t->d = dot3(t->n, p[a]);
    return true;
}

// The hull's triangles over m points (outward); -1 for a flat set
static int hull_tris(const float (*p)[3], int m, float eps, HTri* T, int cap, int (*E)[2], int ecap) {
    if (m < 4) return -1;
    int i0 = 0;
    for (int i = 1; i < m; i++)
        if (p[i][0] < p[i0][0]) i0 = i;
    int i1 = -1;
    float bd = -1.0f;
    for (int i = 0; i < m; i++) {
        float e[3];
        sub3(p[i], p[i0], e);
        if (dot3(e, e) > bd) bd = dot3(e, e), i1 = i;
    }
    if (bd < eps * eps) return -1;
    float ab[3];
    sub3(p[i1], p[i0], ab);
    int i2 = -1;
    bd = -1.0f;
    for (int i = 0; i < m; i++) {
        float e[3], c[3];
        sub3(p[i], p[i0], e), cross3(ab, e, c);
        if (dot3(c, c) > bd) bd = dot3(c, c), i2 = i;
    }
    if (bd < eps * eps * dot3(ab, ab)) return -1;
    HTri base;
    if (!htri(p, i0, i1, i2, &base)) return -1;
    int i3 = -1;
    bd = -1.0f;
    for (int i = 0; i < m; i++) {
        float x = fabsf(dot3(base.n, p[i]) - base.d);
        if (x > bd) bd = x, i3 = i;
    }
    if (bd < eps) return -1;
    float cen[3];
    for (int k = 0; k < 3; k++) cen[k] = (p[i0][k] + p[i1][k] + p[i2][k] + p[i3][k]) * 0.25f;
    int q[4] = { i0, i1, i2, i3 }, nt = 0;
    static const int F[4][3] = { { 0, 1, 2 }, { 0, 3, 1 }, { 0, 2, 3 }, { 1, 3, 2 } };
    for (int f = 0; f < 4; f++) {
        HTri t;
        if (!htri(p, q[F[f][0]], q[F[f][1]], q[F[f][2]], &t)) return -1;
        if (dot3(t.n, cen) - t.d > 0.0f && !htri(p, q[F[f][0]], q[F[f][2]], q[F[f][1]], &t)) return -1;
        T[nt++] = t;
    }
    for (int i = 0; i < m; i++) {
        if (i == i0 || i == i1 || i == i2 || i == i3) continue;
        int ne = 0;
        bool any = false;
        for (int f = nt - 1; f >= 0; f--) {
            if (dot3(T[f].n, p[i]) - T[f].d <= eps) continue;
            any = true;
            int ed[3][2] = { { T[f].a, T[f].b }, { T[f].b, T[f].c }, { T[f].c, T[f].a } };
            for (int e = 0; e < 3; e++) {
                int found = -1;
                for (int j = 0; j < ne && found < 0; j++)
                    if (E[j][0] == ed[e][1] && E[j][1] == ed[e][0]) found = j;
                if (found >= 0) E[found][0] = E[ne - 1][0], E[found][1] = E[ne - 1][1], ne--;
                else if (ne < ecap) E[ne][0] = ed[e][0], E[ne][1] = ed[e][1], ne++;
            }
            T[f] = T[--nt];
        }
        if (!any) continue;
        for (int e = 0; e < ne; e++) {
            HTri t;
            if (nt >= cap) return -1;
            if (htri(p, E[e][0], E[e][1], i, &t)) T[nt++] = t;
        }
    }
    return nt;
}

static void fib_dir(int i, int n, float d[3]) {
    float z = 1.0f - (2.0f * (float)i + 1.0f) / (float)n, r = sqrtf(fmaxf(1.0f - z * z, 0.0f)), ph = 2.39996323f * (float)i;
    d[0] = r * cosf(ph), d[1] = r * sinf(ph), d[2] = z;
}

#define HULL_DIRS 128
#define HULL_FIXED 26
#define HULL_CAND (HULL_DIRS + HULL_FIXED)
#define HULL_GROUPS ((HULL_CAND + 7) / 8)

static int gather_extremes(const float (*src)[3], const void* pos, uint32_t stride, uint32_t n, int ndirs, float eps, float (*out)[3]) {
    float dirs[HULL_CAND][3];
    int nd = 0;
    for (int x = -1; x <= 1; x++) // the box's 26 directions (its corners, edges and faces: a box mesh's 8 corners)
        for (int y = -1; y <= 1; y++)
            for (int z = -1; z <= 1; z++) {
                if (!x && !y && !z) continue;
                float l = sqrtf((float)(x * x + y * y + z * z));
                dirs[nd][0] = (float)x / l, dirs[nd][1] = (float)y / l, dirs[nd][2] = (float)z / l, nd++;
            }
    for (int i = 0; i < ndirs; i++) fib_dir(i, ndirs, dirs[nd++]);
    // AVX2: 8 directions at a time
    int ng = (nd + 7) / 8;
    _Alignas(32) float dx[HULL_GROUPS * 8], dy[HULL_GROUPS * 8], dz[HULL_GROUPS * 8];
    _Alignas(32) uint32_t arg[HULL_GROUPS * 8];
    for (int k = 0; k < ng * 8; k++) {
        const float* d = dirs[k < nd ? k : 0];
        dx[k] = d[0], dy[k] = d[1], dz[k] = d[2];
    }
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    __m256 best[HULL_GROUPS];
    __m256i bi[HULL_GROUPS];
    for (int g = 0; g < ng; g++) best[g] = _mm256_set1_ps(-1e30f), bi[g] = _mm256_setzero_si256();
    for (uint32_t i = 0; i < n; i++) {
        const float* v = src ? src[i] : (const float*)((const uint8_t*)pos + (size_t)i * stride);
        __m256 vx = _mm256_set1_ps(v[0]), vy = _mm256_set1_ps(v[1]), vz = _mm256_set1_ps(v[2]);
        __m256 vi = _mm256_castsi256_ps(_mm256_set1_epi32((int)i));
        for (int g = 0; g < ng; g++) {
            __m256 d = _mm256_fmadd_ps(vx, _mm256_load_ps(dx + 8 * g), _mm256_fmadd_ps(vy, _mm256_load_ps(dy + 8 * g), _mm256_mul_ps(vz, _mm256_load_ps(dz + 8 * g))));
            __m256 m = _mm256_cmp_ps(d, best[g], _CMP_GT_OQ);
            best[g] = _mm256_blendv_ps(best[g], d, m);
            bi[g] = _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(bi[g]), vi, m));
        }
    }
    for (int g = 0; g < ng; g++) _mm256_store_si256((__m256i*)(arg + 8 * g), bi[g]);
#else // (ARM: the Android game) the same search, one direction at a time
    float bestv[HULL_GROUPS * 8];
    for (int k = 0; k < ng * 8; k++) bestv[k] = -1e30f, arg[k] = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float* v = src ? src[i] : (const float*)((const uint8_t*)pos + (size_t)i * stride);
        for (int k = 0; k < nd; k++) {
            float d = v[0] * dx[k] + v[1] * dy[k] + v[2] * dz[k];
            if (d > bestv[k]) bestv[k] = d, arg[k] = i;
        }
    }
#endif
    int m = 0;
    for (int k = 0; k < nd; k++) {
        const float* v = src ? src[arg[k]] : (const float*)((const uint8_t*)pos + (size_t)arg[k] * stride);
        bool dup = false;
        for (int j = 0; j < m && !dup; j++) {
            float e[3];
            sub3(v, out[j], e);
            dup = dot3(e, e) < eps * eps;
        }
        if (!dup) memcpy(out[m++], v, 12);
    }
    return m;
}

bool phys_hull_build(const void* pos, uint32_t stride, uint32_t n, PhysHull* h) {
    double t0 = re_time_seconds();
    memset(h, 0, sizeof *h);
    if (n < 4) return false;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; i++) {
        const float* v = (const float*)((const uint8_t*)pos + (size_t)i * stride);
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], v[k]), mx[k] = fmaxf(mx[k], v[k]);
    }
    float ext = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2]));
    if (!(ext > 1e-3f)) return false;
    float eps = fmaxf(ext * 1e-5f, 1e-4f);
    static float cand[HULL_CAND][3], prev[HULL_CAND][3]; // (built on one thread: the physics')
    static HTri T[4 * HULL_CAND];
    static int E[12 * HULL_CAND][2];
    int ndirs = HULL_DIRS, m = gather_extremes(NULL, pos, stride, n, ndirs, eps, cand);
    int nt = -1, used_v = 0;
    int map[HULL_CAND];
    for (int attempt = 0; attempt < 10; attempt++) {
        nt = hull_tris((const float(*)[3])cand, m, eps, T, 4 * HULL_CAND, E, 12 * HULL_CAND);
        if (nt < 0) return false;
        for (int i = 0; i < m; i++) map[i] = -1;
        used_v = 0;
        for (int t = 0; t < nt; t++) {
            int* ix[3] = { &T[t].a, &T[t].b, &T[t].c };
            for (int k = 0; k < 3; k++)
                if (map[*ix[k]] < 0) map[*ix[k]] = used_v++;
        }
        if (used_v <= PHYS_HULL_MAX_V) break;
        // too many: the extremes along fewer directions, among this hull's points
        int pm = 0;
        for (int i = 0; i < m; i++)
            if (map[i] >= 0) memcpy(prev[pm++], cand[i], 12);
        ndirs = ndirs * 2 / 3;
        if (ndirs < PHYS_HULL_MAX_V - HULL_FIXED) ndirs = PHYS_HULL_MAX_V - HULL_FIXED;
        m = gather_extremes((const float(*)[3])prev, NULL, 0, (uint32_t)pm, ndirs, eps, cand);
        if (attempt >= 8 && m > PHYS_HULL_MAX_V) m = PHYS_HULL_MAX_V; // (the last resort: the first 64 extremes)
    }
    if (used_v > PHYS_HULL_MAX_V || nt < 4) return false;
    h->nv = (uint32_t)used_v;
    for (int i = 0; i < m; i++)
        if (map[i] >= 0) memcpy(h->v[map[i]], cand[i], 12);
    for (int t = 0; t < nt; t++) T[t].a = map[T[t].a], T[t].b = map[T[t].b], T[t].c = map[T[t].c];
    // mass: tetrahedra from an inner point
    float ref[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < h->nv; i++)
        for (int k = 0; k < 3; k++) ref[k] += h->v[i][k] / (float)h->nv;
    double vol = 0.0, com[3] = { 0, 0, 0 }, C[3][3] = { { 0 } };
    for (int t = 0; t < nt; t++) {
        float a[3], b[3], c[3], bc[3];
        sub3(h->v[T[t].a], ref, a), sub3(h->v[T[t].b], ref, b), sub3(h->v[T[t].c], ref, c);
        cross3(b, c, bc);
        double det = dot3(a, bc);
        vol += det / 6.0;
        for (int k = 0; k < 3; k++) com[k] += det / 24.0 * (a[k] + b[k] + c[k]);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double s = (double)a[i] + b[i] + c[i], q = (double)a[j] + b[j] + c[j];
                C[i][j] += det / 120.0 * ((double)a[i] * a[j] + (double)b[i] * b[j] + (double)c[i] * c[j] + s * q);
            }
    }
    if (!(vol > 1e-9)) return false;
    for (int k = 0; k < 3; k++) com[k] /= vol;
    h->volume = (float)vol;
    for (int k = 0; k < 3; k++) h->com[k] = (float)(com[k] + ref[k]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) h->cov[i][j] = (float)(C[i][j] - vol * com[i] * com[j]);
    // faces: the coplanar triangles together, their vertices ordered around the face
    uint8_t group[4 * HULL_CAND];
    memset(group, 0, (size_t)nt);
    uint32_t nidx = 0;
    for (int t = 0; t < nt; t++) {
        if (group[t]) continue;
        if (h->nf >= PHYS_HULL_MAX_F) return false;
        uint32_t f = h->nf++;
        memcpy(h->n[f], T[t].n, 12);
        int vs[PHYS_HULL_MAX_V], nvs = 0;
        for (int u = t; u < nt; u++) {
            if (group[u] || dot3(T[u].n, T[t].n) < 0.9995f || fabsf(T[u].d - T[t].d) > eps * 8.0f) continue;
            group[u] = 1;
            int tv[3] = { T[u].a, T[u].b, T[u].c };
            for (int k = 0; k < 3; k++) {
                bool seen = false;
                for (int j = 0; j < nvs && !seen; j++) seen = vs[j] == tv[k];
                if (!seen && nvs < PHYS_HULL_MAX_V) vs[nvs++] = tv[k];
            }
        }
        float c[3] = { 0, 0, 0 }, u[3], w[3], ang[PHYS_HULL_MAX_V];
        for (int j = 0; j < nvs; j++)
            for (int k = 0; k < 3; k++) c[k] += h->v[vs[j]][k] / (float)nvs;
        perp3(h->n[f], u, w);
        for (int j = 0; j < nvs; j++) {
            float e[3];
            sub3(h->v[vs[j]], c, e);
            ang[j] = atan2f(dot3(e, w), dot3(e, u));
        }
        for (int j = 1; j < nvs; j++) // (insertion sort: a face has few vertices)
            for (int q = j; q > 0 && ang[q] < ang[q - 1]; q--) {
                float ta = ang[q];
                ang[q] = ang[q - 1], ang[q - 1] = ta;
                int tv = vs[q];
                vs[q] = vs[q - 1], vs[q - 1] = tv;
            }
        if (nidx + (uint32_t)nvs > PHYS_HULL_MAX_FI) return false;
        h->first[f] = (uint16_t)nidx, h->count[f] = (uint8_t)nvs;
        for (int j = 0; j < nvs; j++) h->idx[nidx++] = (uint8_t)vs[j];
    }
    for (int k = 0; k < 3; k++) h->mn[k] = 1e30f, h->mx[k] = -1e30f;
    for (uint32_t i = 0; i < h->nv; i++)
        for (int k = 0; k < 3; k++) h->mn[k] = fminf(h->mn[k], h->v[i][k]), h->mx[k] = fmaxf(h->mx[k], h->v[i][k]);
    float bv = (h->mx[0] - h->mn[0]) * (h->mx[1] - h->mn[1]) * (h->mx[2] - h->mn[2]);
    h->boxlike = h->nv == 8 && h->nf == 6 && h->volume >= 0.995f * bv;
    h->build_ms = (re_time_seconds() - t0) * 1000.0;
    return true;
}

float phys_hull_inner(const PhysHull* h, const float s[3]) {
    float best = 1e30f;
    for (uint32_t f = 0; f < h->nf; f++) {
        float ns[3] = { h->n[f][0] / s[0], h->n[f][1] / s[1], h->n[f][2] / s[2] }, l = len3(ns);
        const float* v = h->v[h->idx[h->first[f]]];
        float d[3] = { (v[0] - h->com[0]) * s[0], (v[1] - h->com[1]) * s[1], (v[2] - h->com[2]) * s[2] };
        if (l > 1e-20f) best = fminf(best, fabsf(dot3(ns, d)) / l);
    }
    return best < 1e30f ? best : 0.0f;
}

// --- Triangle meshes in a BVH ---

typedef struct MeshBuild {
    const float (*tri)[3][3];
    float (*cen)[3];
    uint32_t* perm;
    PhysMeshNode* node;
    uint32_t nnode;
} MeshBuild;

static void tri_bounds(const float t[3][3], float mn[3], float mx[3]) {
    for (int k = 0; k < 3; k++) {
        mn[k] = fminf(t[0][k], fminf(t[1][k], t[2][k]));
        mx[k] = fmaxf(t[0][k], fmaxf(t[1][k], t[2][k]));
    }
}

static void nth_element(MeshBuild* b, uint32_t lo, uint32_t hi, uint32_t nth, int axis) { // [lo, hi): quickselect by centroid
    while (hi - lo > 1) {
        float pivot = b->cen[b->perm[(lo + hi) / 2]][axis];
        uint32_t i = lo, j = hi - 1;
        while (i <= j) {
            while (b->cen[b->perm[i]][axis] < pivot) i++;
            while (b->cen[b->perm[j]][axis] > pivot) j--;
            if (i <= j) {
                uint32_t t = b->perm[i];
                b->perm[i] = b->perm[j], b->perm[j] = t;
                i++;
                if (j == 0) break;
                j--;
            }
        }
        if (nth <= j) hi = j + 1;
        else if (nth >= i) lo = i;
        else return;
    }
}

static uint32_t build_node(MeshBuild* b, uint32_t lo, uint32_t hi, int depth) {
    uint32_t ni = b->nnode++;
    PhysMeshNode* nd = &b->node[ni];
    float cmn[3] = { 1e30f, 1e30f, 1e30f }, cmx[3] = { -1e30f, -1e30f, -1e30f };
    for (int k = 0; k < 3; k++) nd->mn[k] = 1e30f, nd->mx[k] = -1e30f;
    for (uint32_t i = lo; i < hi; i++) {
        float tmn[3], tmx[3];
        tri_bounds(b->tri[b->perm[i]], tmn, tmx);
        for (int k = 0; k < 3; k++) {
            nd->mn[k] = fminf(nd->mn[k], tmn[k]), nd->mx[k] = fmaxf(nd->mx[k], tmx[k]);
            cmn[k] = fminf(cmn[k], b->cen[b->perm[i]][k]), cmx[k] = fmaxf(cmx[k], b->cen[b->perm[i]][k]);
        }
    }
    if (hi - lo <= 4 || depth > 60) {
        nd->first = lo, nd->count = hi - lo;
        return ni;
    }
    int axis = 0;
    for (int k = 1; k < 3; k++)
        if (cmx[k] - cmn[k] > cmx[axis] - cmn[axis]) axis = k;
    uint32_t mid = (lo + hi) / 2;
    nth_element(b, lo, hi, mid, axis);
    nd->count = 0;
    build_node(b, lo, mid, depth + 1);
    uint32_t right = build_node(b, mid, hi, depth + 1);
    b->node[ni].first = right;
    return ni;
}

PhysMesh* phys_mesh_build(const void* pos, uint32_t stride, uint32_t nverts, const uint32_t* idx, uint32_t nidx) {
    double t0 = re_time_seconds();
    uint32_t cap = nidx / 3;
    if (!cap) return NULL;
    float(*tri)[3][3] = malloc(sizeof(float) * 9 * cap); // (once a mesh, a cache: not a frame's)
    float(*cen)[3] = malloc(sizeof(float) * 3 * cap);
    uint32_t* perm = malloc(sizeof(uint32_t) * cap);
    PhysMesh* m = calloc(1, sizeof(PhysMesh));
    if (!tri || !cen || !perm || !m) {
        free(tri), free(cen), free(perm), free(m);
        return NULL;
    }
    uint32_t nt = 0;
    for (uint32_t t = 0; t < cap; t++) {
        uint32_t i[3] = { idx[3 * t], idx[3 * t + 1], idx[3 * t + 2] };
        if (i[0] >= nverts || i[1] >= nverts || i[2] >= nverts) continue;
        for (int c = 0; c < 3; c++) memcpy(tri[nt][c], (const uint8_t*)pos + (size_t)i[c] * stride, 12);
        float ab[3], ac[3], n[3];
        sub3(tri[nt][1], tri[nt][0], ab), sub3(tri[nt][2], tri[nt][0], ac), cross3(ab, ac, n);
        if (dot3(n, n) < 1e-10f) continue; // (degenerate)
        for (int k = 0; k < 3; k++) cen[nt][k] = (tri[nt][0][k] + tri[nt][1][k] + tri[nt][2][k]) / 3.0f;
        perm[nt] = nt;
        nt++;
    }
    if (!nt) {
        free(tri), free(cen), free(perm), free(m);
        return NULL;
    }
    m->node = malloc(sizeof(PhysMeshNode) * 2 * nt);
    m->tri = malloc(sizeof(float) * 9 * nt);
    if (!m->node || !m->tri) {
        free(tri), free(cen), free(perm), phys_mesh_free(m);
        return NULL;
    }
    MeshBuild b = { (const float(*)[3][3])tri, cen, perm, m->node, 0 };
    build_node(&b, 0, nt, 0);
    m->nnode = b.nnode, m->ntri = nt;
    for (uint32_t i = 0; i < nt; i++) memcpy(m->tri[i], tri[perm[i]], 36);
    memcpy(m->mn, m->node[0].mn, 12), memcpy(m->mx, m->node[0].mx, 12);
    free(tri), free(cen), free(perm);
    m->build_ms = (re_time_seconds() - t0) * 1000.0;
    return m;
}

void phys_mesh_free(PhysMesh* m) {
    if (!m) return;
    free(m->node), free(m->tri), free(m);
}

void phys_mesh_query(const PhysMesh* m, const float mn[3], const float mx[3], PhysTriFn fn, void* user) {
    uint32_t stack[96], sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const PhysMeshNode* nd = &m->node[stack[--sp]];
        if (nd->mn[0] > mx[0] || nd->mx[0] < mn[0] || nd->mn[1] > mx[1] || nd->mx[1] < mn[1] || nd->mn[2] > mx[2] || nd->mx[2] < mn[2]) continue;
        if (nd->count) {
            for (uint32_t i = nd->first; i < nd->first + nd->count; i++) {
                float tmn[3], tmx[3];
                tri_bounds((const float(*)[3])m->tri[i], tmn, tmx);
                if (tmn[0] > mx[0] || tmx[0] < mn[0] || tmn[1] > mx[1] || tmx[1] < mn[1] || tmn[2] > mx[2] || tmx[2] < mn[2]) continue;
                if (!fn(user, (const float(*)[3])m->tri[i])) return;
            }
            continue;
        }
        uint32_t self = (uint32_t)(nd - m->node);
        if (sp + 2 <= 96) stack[sp++] = nd->first, stack[sp++] = self + 1;
    }
}

// (Q8, CCD) the rounded shapes' distance (their cores' less the radii), n from B to A; <= 0 when they touch or overlap;
// maxd + 1 when farther than maxd (nothing else computed)
float phys_distance(const Cvx* A, const Cvx* B, float maxd, float n[3]) {
    float rs = A->radius + B->radius, dist = 0.0f, pa[3], pb[3], v[3];
    Simplex s;
    bool far;
    bool apart = gjk(A, B, maxd + rs, &s, &dist, pa, pb, v, &far);
    if (apart && far) return maxd + 1.0f;
    if (!apart || dist < 1e-4f) {
        n[0] = n[1] = 0.0f, n[2] = 1.0f;
        return 0.0f;
    }
    for (int k = 0; k < 3; k++) n[k] = v[k] / dist;
    return dist - rs;
}
