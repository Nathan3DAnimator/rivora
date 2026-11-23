// (2026-11-19) Scene queries: scene_query.h, notas-dev/traces.md
#include "scene_query.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "debug_draw.h"
#include "debug_viewer.h"
#include "landscape.h"
#include "phys_simd.h"
#include "physics.h"
#include "physics_shapes.h"
#include "platform.h"

uint32_t bp_query_boxes(float (*b)[6], Entity* actor, uint8_t* moving, Entity* mesh_actor, uint32_t cap); // (bp_vm.c)
uint32_t bp_query_play_frame(void);

#define LEAF_MAX 4
#define BIG 1e30f

// A primitive: a core box and a radius (a box: 0; a capsule: its vertical segment as a flat box, its radius)
typedef struct Prim {
    float mn[3], mx[3], r;
    uint32_t obj; // SQ_OBJ_*
    Entity e;
    int32_t cx;   // its mesh's triangles (Trace Complex): S.cx's index, -1 none
} Prim;

// (Trace Complex) an actor's mesh: its triangles' BVH (the mesh's space) and where it is: world = loc + sum s[j] l[j] R[j]
typedef struct CxMesh {
    const PhysMesh* m;
    float loc[3], R[3][3], s[3];
} CxMesh;

// 8 children: their bounds side by side (one fw each), a child node's index (>= 0) or a leaf (< 0: -1 - (first * 4 +
// count - 1) into idx); lanes: the used children's bits
typedef struct Node8 {
    float bmn[3][8], bmx[3][8];
    int32_t child[8];
    uint32_t lanes;
    uint32_t pad[7];
} Node8;

typedef struct BNode {
    float mn[3], mx[3];
    int32_t l, r;
    uint32_t start, count;
} BNode;

static struct {
    Prim* prim;
    uint32_t nprim, cap_prim;
    uint32_t* idx; // the leaves' primitives, in order
    Node8* node;
    uint32_t nnode, cap_node;
    BNode* bn; // (the build's binary tree, then collapsed)
    uint32_t nbn, cap_bn;
    float (*cent)[3];
    float (*bx)[6]; // bp_query_boxes' copy
    Entity* bxe;
    uint8_t* bxm;
    Entity* bxa; // (the boxes' own meshes' actors: Trace Complex)
    CxMesh* cx;
    uint32_t ncx, cap_cx;
    bool complex_wanted; // (a Trace Complex came: the meshes' triangles read from then on)
    uint32_t cap_box;
    uint32_t built_n, since_build, synced_frame;
    bool any_moving, synced, has_land;
    World* w;
    SqStats st;
    volatile int32_t q_frame, b_frame; // (this frame's counts: the batches' workers add theirs)
    double ms_frame;
} S;

static int ctz8(int v) {
    int n = 0;
    while (!(v & 1)) v >>= 1, n++;
    return n;
}
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static bool grow(void** p, uint32_t* cap, uint32_t need, size_t elem) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void* q = realloc(*p, elem * c); // (the world grew: once, not each frame)
    if (!q) return false;
    *p = q, *cap = c;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The exact tests. The segment is o + d t, t in [0, tmax] (d the whole way: t 1 is the end)

// a sphere (center c, radius r): its first t >= 0 below best, else best
static float ray_sphere(const float o[3], const float d[3], const float c[3], float r, float best) {
    float m[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
    float a = dot3(d, d), b = dot3(m, d), cc = dot3(m, m) - r * r;
    if (a <= 0.0f) return best;
    float disc = b * b - a * cc;
    if (disc < 0.0f) return best;
    float t = (-b - sqrtf(disc)) / a;
    return t >= 0.0f && t < best ? t : best;
}
// a capsule (segment p0..p1, radius r)
static float ray_capsule(const float o[3], const float d[3], const float p0[3], const float p1[3], float r, float best) {
    float n[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, m[3] = { o[0] - p0[0], o[1] - p0[1], o[2] - p0[2] };
    float dd = dot3(n, n);
    if (dd > 1e-8f) { // the side (an infinite cylinder, then the part between the ends)
        float md = dot3(m, n), nd = dot3(d, n), mn = dot3(m, d), d2 = dot3(d, d);
        float a = dd * d2 - nd * nd, b = dd * mn - nd * md, c = dd * (dot3(m, m) - r * r) - md * md;
        if (fabsf(a) > 1e-12f) {
            float disc = b * b - a * c;
            if (disc >= 0.0f) {
                float t = (-b - sqrtf(disc)) / a;
                if (t >= 0.0f && t < best) {
                    float s = md + t * nd;
                    if (s >= 0.0f && s <= dd) best = t;
                }
            }
        }
    }
    best = ray_sphere(o, d, p0, r, best);
    return ray_sphere(o, d, p1, r, best);
}

// The rounded box: every point within r of [lo, hi]. Its first t below tmax; *in: the start is already inside (t 0).
// The faces by slabs of the box grown by r; an entry beyond an edge or a corner (two or three axes outside the core)
// by that edge's capsule or the corner's three (Ericson, Real-Time Collision Detection 5.5.7)
static bool ray_rbox(const float o[3], const float d[3], const float lo[3], const float hi[3], float r, float tmax, float* tout, float nrm[3], bool* in) {
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return false; // (an emptied box: a destroyed actor's)
    float q[3], dd = 0.0f;
    for (int k = 0; k < 3; k++) q[k] = o[k] < lo[k] ? lo[k] : o[k] > hi[k] ? hi[k] : o[k], dd += (o[k] - q[k]) * (o[k] - q[k]);
    if (dd <= r * r) { // started inside
        float c[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f }, v[3] = { o[0] - c[0], o[1] - c[1], o[2] - c[2] };
        float l = sqrtf(dot3(v, v)), dl = sqrtf(dot3(d, d));
        if (l > 1e-6f) nrm[0] = v[0] / l, nrm[1] = v[1] / l, nrm[2] = v[2] / l;
        else if (dl > 0.0f) nrm[0] = -d[0] / dl, nrm[1] = -d[1] / dl, nrm[2] = -d[2] / dl;
        else nrm[0] = nrm[1] = 0.0f, nrm[2] = 1.0f;
        *tout = 0.0f, *in = true;
        return true;
    }
    float t0 = 0.0f, t1 = tmax;
    int ax = -1;
    for (int k = 0; k < 3; k++) {
        float l = lo[k] - r, h = hi[k] + r;
        if (fabsf(d[k]) < 1e-12f) {
            if (o[k] < l || o[k] > h) return false;
            continue;
        }
        float inv = 1.0f / d[k], ta = (l - o[k]) * inv, tb = (h - o[k]) * inv;
        if (ta > tb) {
            float s = ta;
            ta = tb, tb = s;
        }
        if (ta > t0) t0 = ta, ax = k;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    float p[3] = { o[0] + d[0] * t0, o[1] + d[1] * t0, o[2] + d[2] * t0 };
    const float eps = 1e-4f * (1.0f + r);
    int out = 0, oa[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++)
        if (p[k] < lo[k] - eps || p[k] > hi[k] + eps) oa[out++] = k;
    if (r <= 0.0f || out <= 1) { // a face
        if (ax < 0) return false; // (inside the grown box beside a face: within r, taken above)
        nrm[0] = nrm[1] = nrm[2] = 0.0f;
        nrm[ax] = d[ax] > 0.0f ? -1.0f : 1.0f;
        *tout = t0, *in = false;
        return true;
    }
    float c[3]; // the corner nearest the entry
    for (int k = 0; k < 3; k++) c[k] = p[k] < (lo[k] + hi[k]) * 0.5f ? lo[k] : hi[k];
    float best = tmax;
    if (out == 2) { // the edge along the axis inside
        int k = 3 - oa[0] - oa[1];
        float p0[3] = { c[0], c[1], c[2] }, p1[3] = { c[0], c[1], c[2] };
        p0[k] = lo[k], p1[k] = hi[k];
        best = ray_capsule(o, d, p0, p1, r, best);
    } else
        for (int k = 0; k < 3; k++) { // the corner: its three edges
            float p0[3] = { c[0], c[1], c[2] }, p1[3] = { c[0], c[1], c[2] };
            p1[k] = c[k] == lo[k] ? hi[k] : lo[k];
            best = ray_capsule(o, d, p0, p1, r, best);
        }
    if (best >= tmax) return false;
    float h[3] = { o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best }, v[3];
    for (int k = 0; k < 3; k++) v[k] = h[k] - (h[k] < lo[k] ? lo[k] : h[k] > hi[k] ? hi[k] : h[k]);
    float l = sqrtf(dot3(v, v));
    if (l > 1e-6f) nrm[0] = v[0] / l, nrm[1] = v[1] / l, nrm[2] = v[2] / l;
    else nrm[0] = nrm[1] = 0.0f, nrm[2] = 1.0f;
    *tout = best, *in = false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The BVH

static void prim_bounds(const Prim* p, float mn[3], float mx[3]) {
    for (int k = 0; k < 3; k++) mn[k] = p->mn[k] - p->r, mx[k] = p->mx[k] + p->r;
}
static float area(const float mn[3], const float mx[3]) {
    float e[3] = { maxf(mx[0] - mn[0], 0.0f), maxf(mx[1] - mn[1], 0.0f), maxf(mx[2] - mn[2], 0.0f) };
    return e[0] * e[1] + e[1] * e[2] + e[2] * e[0];
}

#define BINS 12
static int32_t build_bin(uint32_t start, uint32_t count) {
    int32_t id = (int32_t)S.nbn++;
    BNode* n = &S.bn[id];
    float cmn[3] = { BIG, BIG, BIG }, cmx[3] = { -BIG, -BIG, -BIG };
    for (int k = 0; k < 3; k++) n->mn[k] = BIG, n->mx[k] = -BIG;
    for (uint32_t i = start; i < start + count; i++) {
        float a[3], b[3];
        prim_bounds(&S.prim[S.idx[i]], a, b);
        for (int k = 0; k < 3; k++) {
            if (a[k] <= b[k]) n->mn[k] = minf(n->mn[k], a[k]), n->mx[k] = maxf(n->mx[k], b[k]);
            cmn[k] = minf(cmn[k], S.cent[S.idx[i]][k]), cmx[k] = maxf(cmx[k], S.cent[S.idx[i]][k]);
        }
    }
    n->start = start, n->count = count, n->l = n->r = -1;
    if (count <= LEAF_MAX) return id;
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (cmx[k] - cmn[k] > cmx[ax] - cmn[ax]) ax = k;
    float ext = cmx[ax] - cmn[ax];
    uint32_t mid = count / 2;
    if (ext > 1e-6f) { // binned SAH over the centroids
        struct { float mn[3], mx[3]; uint32_t n; } bin[BINS];
        for (int b = 0; b < BINS; b++) bin[b].n = 0, bin[b].mn[0] = bin[b].mn[1] = bin[b].mn[2] = BIG, bin[b].mx[0] = bin[b].mx[1] = bin[b].mx[2] = -BIG;
        float sc = (float)BINS / ext;
        for (uint32_t i = start; i < start + count; i++) {
            int b = (int)((S.cent[S.idx[i]][ax] - cmn[ax]) * sc);
            b = b < 0 ? 0 : b >= BINS ? BINS - 1 : b;
            float a[3], c[3];
            prim_bounds(&S.prim[S.idx[i]], a, c);
            bin[b].n++;
            for (int k = 0; k < 3; k++) bin[b].mn[k] = minf(bin[b].mn[k], a[k]), bin[b].mx[k] = maxf(bin[b].mx[k], c[k]);
        }
        float lc[BINS], la[BINS], bestc = BIG;
        int bests = -1;
        {
            float mn[3] = { BIG, BIG, BIG }, mx[3] = { -BIG, -BIG, -BIG };
            uint32_t cnt = 0;
            for (int b = 0; b < BINS - 1; b++) {
                cnt += bin[b].n;
                for (int k = 0; k < 3; k++) mn[k] = minf(mn[k], bin[b].mn[k]), mx[k] = maxf(mx[k], bin[b].mx[k]);
                lc[b] = (float)cnt, la[b] = cnt ? area(mn, mx) : 0.0f;
            }
        }
        {
            float mn[3] = { BIG, BIG, BIG }, mx[3] = { -BIG, -BIG, -BIG };
            uint32_t cnt = 0;
            for (int b = BINS - 1; b > 0; b--) {
                cnt += bin[b].n;
                for (int k = 0; k < 3; k++) mn[k] = minf(mn[k], bin[b].mn[k]), mx[k] = maxf(mx[k], bin[b].mx[k]);
                float cost = lc[b - 1] * la[b - 1] + (float)cnt * (cnt ? area(mn, mx) : 0.0f);
                if (lc[b - 1] > 0.0f && cnt > 0 && cost < bestc) bestc = cost, bests = b;
            }
        }
        if (bests > 0) { // partition at that bin's edge
            uint32_t i = start, j = start + count;
            while (i < j) {
                int b = (int)((S.cent[S.idx[i]][ax] - cmn[ax]) * sc);
                b = b < 0 ? 0 : b >= BINS ? BINS - 1 : b;
                if (b < bests) i++;
                else {
                    uint32_t t = S.idx[i];
                    S.idx[i] = S.idx[--j], S.idx[j] = t;
                }
            }
            mid = i - start;
        }
    }
    if (mid == 0 || mid >= count) mid = count / 2; // (all on one side: halves)
    int32_t l = build_bin(start, mid), r = build_bin(start + mid, count - mid);
    S.bn[id].l = l, S.bn[id].r = r;
    return id;
}

// The binary tree's node b (an inner one) as an 8-wide node: its descendants opened, the widest first, until 8
static int32_t collapse(int32_t b) {
    int32_t kids[8] = { S.bn[b].l, S.bn[b].r };
    int nk = 2;
    while (nk < 8) {
        int pick = -1;
        float pa = -1.0f;
        for (int i = 0; i < nk; i++)
            if (S.bn[kids[i]].l >= 0) {
                float a = area(S.bn[kids[i]].mn, S.bn[kids[i]].mx);
                if (a > pa) pa = a, pick = i;
            }
        if (pick < 0) break;
        int32_t k = kids[pick];
        kids[pick] = S.bn[k].l, kids[nk++] = S.bn[k].r;
    }
    int32_t id = (int32_t)S.nnode++;
    Node8* n = &S.node[id];
    memset(n, 0, sizeof *n);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) n->bmn[k][i] = BIG, n->bmx[k][i] = -BIG;
    n->lanes = (1u << nk) - 1u;
    for (int i = 0; i < nk; i++) {
        const BNode* c = &S.bn[kids[i]];
        for (int k = 0; k < 3; k++) S.node[id].bmn[k][i] = c->mn[k], S.node[id].bmx[k][i] = c->mx[k];
        if (c->l < 0) S.node[id].child[i] = -1 - (int32_t)(c->start * 4u + (c->count - 1u));
        else {
            int32_t ci = collapse(kids[i]); // (a preorder: children after their parent, the refit walks back)
            S.node[id].child[i] = ci;
        }
    }
    return id;
}

static void build(void) {
    double t0 = re_time_seconds();
    S.nnode = 0, S.nbn = 0;
    if (!S.nprim) {
        S.built_n = 0;
        return;
    }
    static uint32_t cap_idx, cap_cent;
    if (!grow((void**)&S.idx, &cap_idx, S.nprim, sizeof(uint32_t)) || !grow((void**)&S.cent, &cap_cent, S.nprim, sizeof(float) * 3) ||
        !grow((void**)&S.bn, &S.cap_bn, S.nprim * 2 + 2, sizeof(BNode)) || !grow((void**)&S.node, &S.cap_node, S.nprim + 2, sizeof(Node8))) {
        S.built_n = 0;
        return;
    }
    for (uint32_t i = 0; i < S.nprim; i++) {
        S.idx[i] = i;
        for (int k = 0; k < 3; k++) S.cent[i][k] = (S.prim[i].mn[k] + S.prim[i].mx[k]) * 0.5f;
    }
    int32_t root = build_bin(0, S.nprim);
    if (S.bn[root].l < 0) { // (a single leaf: a node holding it)
        Node8* n = &S.node[S.nnode++];
        memset(n, 0, sizeof *n);
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 3; k++) n->bmn[k][i] = BIG, n->bmx[k][i] = -BIG;
        for (int k = 0; k < 3; k++) n->bmn[k][0] = S.bn[root].mn[k], n->bmx[k][0] = S.bn[root].mx[k];
        n->child[0] = -1 - (int32_t)(S.bn[root].start * 4u + (S.bn[root].count - 1u));
        n->lanes = 1u;
    } else
        collapse(root);
    S.built_n = S.nprim, S.since_build = 0;
    S.st.rebuilds++;
    S.st.build_ms = (re_time_seconds() - t0) * 1000.0;
}

// The bounds again from the primitives where they are now (the tree kept): the nodes back to front
static void refit(void) {
    double t0 = re_time_seconds();
    for (int32_t i = (int32_t)S.nnode - 1; i >= 0; i--) {
        Node8* n = &S.node[i];
        for (int l = 0; l < 8; l++) {
            if (!(n->lanes & (1u << l))) continue;
            float mn[3] = { BIG, BIG, BIG }, mx[3] = { -BIG, -BIG, -BIG };
            int32_t c = n->child[l];
            if (c < 0) {
                uint32_t code = (uint32_t)(-1 - c), first = code >> 2, cnt = (code & 3u) + 1u;
                for (uint32_t j = first; j < first + cnt; j++) {
                    float a[3], b[3];
                    prim_bounds(&S.prim[S.idx[j]], a, b);
                    for (int k = 0; k < 3; k++)
                        if (a[k] <= b[k]) mn[k] = minf(mn[k], a[k]), mx[k] = maxf(mx[k], b[k]);
                }
            } else {
                const Node8* cn = &S.node[c];
                for (int m = 0; m < 8; m++)
                    if (cn->lanes & (1u << m))
                        for (int k = 0; k < 3; k++) mn[k] = minf(mn[k], cn->bmn[k][m]), mx[k] = maxf(mx[k], cn->bmx[k][m]);
            }
            for (int k = 0; k < 3; k++) n->bmn[k][l] = mn[k], n->bmx[k][l] = mx[k];
        }
    }
    S.st.refits++;
    S.st.refit_ms = (re_time_seconds() - t0) * 1000.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The world's copy

void rotator_axes(const float rot_deg[3], float axes[3][3]); // (render.c)
static void gather(World* w) {
    uint32_t nb = bp_query_boxes(S.bx, S.bxe, S.bxm, S.bxa, S.cap_box);
    if (nb > S.cap_box) {
        uint32_t c0 = S.cap_box, c1 = S.cap_box, c2 = S.cap_box, c3 = S.cap_box;
        if (!grow((void**)&S.bx, &c0, nb, sizeof(float) * 6) || !grow((void**)&S.bxe, &c1, nb, sizeof(Entity)) || !grow((void**)&S.bxm, &c2, nb, 1) ||
            !grow((void**)&S.bxa, &c3, nb, sizeof(Entity)))
            nb = 0;
        else S.cap_box = c0, nb = bp_query_boxes(S.bx, S.bxe, S.bxm, S.bxa, S.cap_box);
        if (nb > S.cap_box) nb = S.cap_box;
    }
    S.ncx = 0;
    if (S.complex_wanted && nb && !grow((void**)&S.cx, &S.cap_cx, nb, sizeof(CxMesh))) S.complex_wanted = false;
    uint32_t npawn = 0;
    for (uint32_t ai = 0; w && ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) == need) npawn += a->count;
    }
    if (!grow((void**)&S.prim, &S.cap_prim, nb + npawn + 1, sizeof(Prim))) nb = npawn = 0;
    uint32_t n = 0;
    S.any_moving = false;
    for (uint32_t i = 0; i < nb; i++) {
        Prim* p = &S.prim[n++];
        memcpy(p->mn, S.bx[i], 12), memcpy(p->mx, &S.bx[i][3], 12);
        p->r = 0.0f, p->e = S.bxe[i], p->obj = S.bxm[i] ? SQ_OBJ_DYNAMIC : SQ_OBJ_STATIC, p->cx = -1;
        S.any_moving |= S.bxm[i] != 0;
        if (S.complex_wanted && S.bxa[i] != ENTITY_NONE && w) { // its own mesh's triangles, where it is now
            const CStaticMesh* sm = ecs_get(w, S.bxa[i], COMP_STATIC_MESH);
            const CTransform* tr = ecs_get(w, S.bxa[i], COMP_TRANSFORM);
            const PhysMesh* m = sm && tr ? physics_mesh_tris(sm->mesh) : NULL;
            if (m) {
                CxMesh* c = &S.cx[S.ncx];
                c->m = m;
                memcpy(c->loc, tr->location, 12), memcpy(c->s, tr->scale, 12);
                rotator_axes(tr->rotation, c->R);
                p->cx = (int32_t)S.ncx++;
            }
        }
    }
    S.st.boxes = nb, S.st.pawns = 0;
    for (uint32_t ai = 0; w && ai < w->archetype_count && npawn; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CCharacter* ch = ecs_arch_column(a, COMP_CHARACTER);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) { // (a capsule: Unreal's half height has the radius in it)
            float cr = ch[i].radius > 0.0f ? ch[i].radius : 34.0f, hh = ch[i].half_height > cr ? ch[i].half_height - cr : 0.0f;
            Prim* p = &S.prim[n++];
            p->mn[0] = p->mx[0] = tr[i].location[0], p->mn[1] = p->mx[1] = tr[i].location[1];
            p->mn[2] = tr[i].location[2] - hh, p->mx[2] = tr[i].location[2] + hh;
            p->r = cr, p->e = ents[i], p->obj = SQ_OBJ_PAWN, p->cx = -1;
            S.st.pawns++;
        }
    }
    S.any_moving |= S.st.pawns > 0;
    S.nprim = n;
}

static void async_drain(void);
void sq_sync(World* w) {
    uint32_t f = bp_query_play_frame();
    if (S.synced && f == S.synced_frame && w == S.w) return;
    async_drain(); // (a batch still on the background threads reads the world: it ends first)
    S.synced = true, S.synced_frame = f, S.w = w;
    landscape_query_prepare();
    float mn[2], mx[2];
    S.has_land = landscape_bounds(mn, mx);
    uint32_t before = S.nprim;
    gather(w);
    if (S.nprim != S.built_n || S.nprim != before || (S.any_moving && S.since_build > 240)) build();
    else if (S.any_moving) refit(), S.since_build++;
    S.st.prims = S.nprim, S.st.nodes = S.nnode;
}

void sq_reset(void) {
    async_drain();
    S.nprim = S.built_n = S.nnode = 0, S.ncx = 0, S.complex_wanted = false;
    S.synced = false, S.w = NULL;
    S.st.rebuilds = S.st.refits = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The queries

typedef struct Ray {
    float o[3], d[3], inv[3], h[3], rq; // h, rq: the query's core half size and radius
    uint8_t objects, cplx, shape;
    const Entity* ignore;
    uint16_t nignore;
} Ray;

static void ray_from(const SqQuery* q, Ray* r) {
    for (int k = 0; k < 3; k++) {
        r->o[k] = q->start[k], r->d[k] = q->end[k] - q->start[k];
        float dk = fabsf(r->d[k]) < 1e-20f ? (r->d[k] < 0.0f ? -1e-20f : 1e-20f) : r->d[k];
        r->inv[k] = 1.0f / dk;
        r->h[k] = 0.0f;
    }
    r->rq = 0.0f;
    switch (q->shape) {
    case SQ_SPHERE: r->rq = fmaxf(q->radius, 0.0f); break;
    case SQ_BOX:
        for (int k = 0; k < 3; k++) r->h[k] = fabsf(q->half[k]);
        break;
    case SQ_CAPSULE:
        r->rq = fmaxf(q->radius, 0.0f);
        r->h[2] = fmaxf(fabsf(q->half[2]) - r->rq, 0.0f);
        break;
    default: break;
    }
    r->objects = q->objects ? q->objects : (uint8_t)SQ_OBJ_ALL;
    r->ignore = q->ignore, r->nignore = q->nignore;
    r->cplx = q->cplx && S.complex_wanted, r->shape = q->shape;
}

static bool ignored(const Ray* r, Entity e) {
    for (uint16_t i = 0; i < r->nignore; i++)
        if (r->ignore[i] == e && e != ENTITY_NONE) return true;
    return false;
}

// --- (Trace Complex) a mesh's triangles: the line exact (Moller-Trumbore in the mesh's space), the shapes by conservative
// advancement (GJK's distance between the shape and each triangle the swept bounds reach) ---

static float tri_ray(const float o[3], const float d[3], const float* v0, const float* v1, const float* v2, float best, float n[3]) {
    float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] }, e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
    float pv[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    float det = dot3(e1, pv);
    if (fabsf(det) < 1e-14f) return best;
    float inv = 1.0f / det, tv[3] = { o[0] - v0[0], o[1] - v0[1], o[2] - v0[2] }, u = dot3(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return best;
    float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] }, v = dot3(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return best;
    float t = dot3(e2, qv) * inv;
    if (t < 0.0f || t >= best) return best;
    n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    return t;
}

static void cx_to_world(const CxMesh* c, const float l[3], float w[3]) {
    for (int k = 0; k < 3; k++) w[k] = c->loc[k] + l[0] * c->s[0] * c->R[0][k] + l[1] * c->s[1] * c->R[1][k] + l[2] * c->s[2] * c->R[2][k];
}

// The query's shape at the segment's t as a convex shape (physics_shapes.h)
static void query_cvx(const Ray* r, float t, Cvx* a) {
    memset(a, 0, sizeof *a);
    for (int k = 0; k < 3; k++) a->c[k] = r->o[k] + r->d[k] * t, a->R[k][k] = 1.0f;
    a->axis = 2, a->radius = r->rq;
    if (r->shape == SQ_BOX) a->kind = CVX_BOX, memcpy(a->h, r->h, 12);
    else if (r->shape == SQ_CAPSULE) a->kind = CVX_SEGMENT, a->h[2] = r->h[2];
    else a->kind = CVX_POINT;
}

static bool cx_test(const Ray* r, const CxMesh* c, float tmax, float* tout, float nout[3], bool* in) {
    const PhysMesh* m = c->m;
    float lo[3], ld[3], dd[3] = { r->o[0] - c->loc[0], r->o[1] - c->loc[1], r->o[2] - c->loc[2] };
    for (int j = 0; j < 3; j++) {
        float s = fabsf(c->s[j]) < 1e-6f ? (c->s[j] < 0.0f ? -1e-6f : 1e-6f) : c->s[j];
        lo[j] = dot3(c->R[j], dd) / s, ld[j] = dot3(c->R[j], r->d) / s;
    }
    bool line = r->shape == SQ_LINE || (r->rq <= 0.0f && r->h[0] <= 0.0f && r->h[1] <= 0.0f && r->h[2] <= 0.0f);
    // the query's reach in the mesh's space (the swept shape's bounds around the local segment)
    float ext[3] = { r->h[0] + r->rq, r->h[1] + r->rq, r->h[2] + r->rq }, el[3];
    for (int j = 0; j < 3; j++) {
        float s = fmaxf(fabsf(c->s[j]), 1e-6f);
        el[j] = line ? 0.0f : (fabsf(c->R[j][0]) * ext[0] + fabsf(c->R[j][1]) * ext[1] + fabsf(c->R[j][2]) * ext[2]) / s;
    }
    float inv[3];
    for (int j = 0; j < 3; j++) inv[j] = 1.0f / (fabsf(ld[j]) < 1e-20f ? (ld[j] < 0.0f ? -1e-20f : 1e-20f) : ld[j]);
    float best = tmax, bn[3] = { 0, 0, 1 };
    bool hit = false, start_in = false;
    uint32_t stack[96];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const PhysMeshNode* nd = &m->node[stack[--sp]];
        float t0 = 0.0f, t1 = best;
        for (int k = 0; k < 3 && t0 <= t1; k++) {
            float a = (nd->mn[k] - el[k] - lo[k]) * inv[k], b = (nd->mx[k] + el[k] - lo[k]) * inv[k];
            t0 = fmaxf(t0, fminf(a, b)), t1 = fminf(t1, fmaxf(a, b));
        }
        if (t0 > t1) continue;
        uint32_t idx = (uint32_t)(nd - m->node);
        if (nd->count == 0) { // inner: the left child follows, first is the right
            if (sp + 2 <= 96) stack[sp++] = nd->first, stack[sp++] = idx + 1;
            continue;
        }
        for (uint32_t i = nd->first; i < nd->first + nd->count; i++) {
            const float(*tri)[3] = m->tri[i];
            if (line) {
                float n[3];
                float t = tri_ray(lo, ld, tri[0], tri[1], tri[2], best, n);
                if (t < best) best = t, hit = true, memcpy(bn, n, 12);
                continue;
            }
            // the triangle's reach: its bounds grown by the shape's, where the segment enters them
            float tmn[3], tmx[3], te = 0.0f, tx = best;
            for (int k = 0; k < 3; k++) {
                tmn[k] = fminf(tri[0][k], fminf(tri[1][k], tri[2][k])) - el[k], tmx[k] = fmaxf(tri[0][k], fmaxf(tri[1][k], tri[2][k])) + el[k];
                float a = (tmn[k] - lo[k]) * inv[k], b = (tmx[k] - lo[k]) * inv[k];
                te = fmaxf(te, fminf(a, b)), tx = fminf(tx, fmaxf(a, b));
            }
            if (te > tx) continue;
            Cvx T, A;
            memset(&T, 0, sizeof T);
            T.kind = CVX_TRIANGLE, T.axis = 2, T.R[0][0] = T.R[1][1] = T.R[2][2] = 1.0f;
            for (int v = 0; v < 3; v++) cx_to_world(c, tri[v], T.t[v]);
            float e1[3] = { T.t[1][0] - T.t[0][0], T.t[1][1] - T.t[0][1], T.t[1][2] - T.t[0][2] }, e2[3] = { T.t[2][0] - T.t[0][0], T.t[2][1] - T.t[0][1], T.t[2][2] - T.t[0][2] };
            T.tn[0] = e1[1] * e2[2] - e1[2] * e2[1], T.tn[1] = e1[2] * e2[0] - e1[0] * e2[2], T.tn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float tl = sqrtf(dot3(T.tn, T.tn));
            if (tl < 1e-10f) continue;
            for (int k = 0; k < 3; k++) T.tn[k] /= tl, T.c[k] = (T.t[0][k] + T.t[1][k] + T.t[2][k]) / 3.0f;
            // conservative advancement: the separating plane's distance over the closing speed along its normal
            float len = sqrtf(dot3(r->d, r->d)), t = te;
            for (int it = 0; it < 48 && t < best; it++) {
                query_cvx(r, t, &A);
                float n[3], maxd = (best - t) * len + 1.0f;
                float dist = phys_distance(&A, &T, maxd, n);
                if (dist > maxd) break;
                if (dist <= 0.02f) {
                    best = t, hit = true, memcpy(bn, n, 12);
                    if (t <= 0.0f && dist <= 0.0f) start_in = true;
                    break;
                }
                float closing = -dot3(r->d, n);
                if (closing <= 1e-6f * len) break; // (moving apart or along it)
                t += dist / closing;
            }
        }
    }
    if (!hit) return false;
    if (line) { // the local normal back to the world (the inverse transpose), facing the start
        float w[3];
        for (int k = 0; k < 3; k++) w[k] = c->R[0][k] * bn[0] / c->s[0] + c->R[1][k] * bn[1] / c->s[1] + c->R[2][k] * bn[2] / c->s[2];
        float l = sqrtf(dot3(w, w));
        if (l > 0.0f) bn[0] = w[0] / l, bn[1] = w[1] / l, bn[2] = w[2] / l;
        if (dot3(bn, r->d) > 0.0f) bn[0] = -bn[0], bn[1] = -bn[1], bn[2] = -bn[2];
    }
    *tout = best, *in = start_in;
    memcpy(nout, bn, 12);
    return true;
}

// One primitive: its rounded box summed with the query's; Trace Complex: its mesh's triangles
static bool prim_test(const Ray* r, const Prim* p, float tmax, float* t, float n[3], bool* in) {
    if (r->cplx && p->cx >= 0) return cx_test(r, &S.cx[p->cx], tmax, t, n, in);
    float lo[3] = { p->mn[0] - r->h[0], p->mn[1] - r->h[1], p->mn[2] - r->h[2] }, hi[3] = { p->mx[0] + r->h[0], p->mx[1] + r->h[1], p->mx[2] + r->h[2] };
    return ray_rbox(r->o, r->d, lo, hi, p->r + r->rq, tmax, t, n, in);
}

typedef struct Best {
    float t, n[3];
    bool in;
    int32_t prim; // -1: none
} Best;

// The nearest (multi: every one, into out[] by t, at most cap; *nout)
typedef struct Multi {
    SqHit* out;
    float* tt;
    int32_t* pp;
    uint32_t n, cap;
} Multi;

static uint64_t g_nodes_seen, g_prims_tested; // (--sq-test's counts; not atomic: one thread counts)
static bool g_count;
static void walk(const Ray* r, Best* b, Multi* m) {
    if (!S.nnode) return;
#if defined(__AVX2__)
    const fw ox = fw_set(r->o[0]), oy = fw_set(r->o[1]), oz = fw_set(r->o[2]);
    const fw ix = fw_set(r->inv[0]), iy = fw_set(r->inv[1]), iz = fw_set(r->inv[2]);
    const fw ex = fw_set(r->h[0] + r->rq), ey = fw_set(r->h[1] + r->rq), ez = fw_set(r->h[2] + r->rq);
#endif
    int32_t stack[512];
    float stt[512];
    int sp = 0;
    stack[sp] = 0, stt[sp++] = 0.0f;
    while (sp) {
        --sp;
        int32_t ni = stack[sp];
        if (!m && stt[sp] > b->t) continue;
        const Node8* n = &S.node[ni];
        if (g_count) g_nodes_seen++;
        float tn[8];
        int bits;
#if defined(__AVX2__)
        {
            fw t1 = fw_mul(fw_sub(fw_sub(_mm256_loadu_ps(n->bmn[0]), ex), ox), ix), t2 = fw_mul(fw_sub(fw_add(_mm256_loadu_ps(n->bmx[0]), ex), ox), ix);
            fw tmin = fw_min(t1, t2), tmax = fw_max(t1, t2);
            t1 = fw_mul(fw_sub(fw_sub(_mm256_loadu_ps(n->bmn[1]), ey), oy), iy), t2 = fw_mul(fw_sub(fw_add(_mm256_loadu_ps(n->bmx[1]), ey), oy), iy);
            tmin = fw_max(tmin, fw_min(t1, t2)), tmax = fw_min(tmax, fw_max(t1, t2));
            t1 = fw_mul(fw_sub(fw_sub(_mm256_loadu_ps(n->bmn[2]), ez), oz), iz), t2 = fw_mul(fw_sub(fw_add(_mm256_loadu_ps(n->bmx[2]), ez), oz), iz);
            tmin = fw_max(fw_max(tmin, fw_min(t1, t2)), fw_zero()), tmax = fw_min(fw_min(tmax, fw_max(t1, t2)), fw_set(m ? 1.0f : b->t));
            bits = fw_bits(fw_le(tmin, tmax)) & (int)n->lanes;
            _mm256_storeu_ps(tn, tmin);
        }
#else
        bits = 0;
        for (int l = 0; l < 8; l++) {
            if (!(n->lanes & (1u << l))) continue;
            float tmin = 0.0f, tmax = m ? 1.0f : b->t, e[3] = { r->h[0] + r->rq, r->h[1] + r->rq, r->h[2] + r->rq };
            for (int k = 0; k < 3; k++) {
                float t1 = (n->bmn[k][l] - e[k] - r->o[k]) * r->inv[k], t2 = (n->bmx[k][l] + e[k] - r->o[k]) * r->inv[k];
                tmin = maxf(tmin, minf(t1, t2)), tmax = minf(tmax, maxf(t1, t2));
            }
            tn[l] = tmin;
            if (tmin <= tmax) bits |= 1 << l;
        }
#endif
        // the children hit: the leaves now, the nodes pushed farthest first
        int32_t kid[8];
        float kt[8];
        int nk = 0;
        while (bits) {
            int l = ctz8(bits);
            bits &= bits - 1;
            int32_t c = n->child[l];
            if (c < 0) {
                uint32_t code = (uint32_t)(-1 - c), first = code >> 2, cnt = (code & 3u) + 1u;
                for (uint32_t j = first; j < first + cnt; j++) {
                    uint32_t pi = S.idx[j];
                    const Prim* p = &S.prim[pi];
                    if (!(p->obj & r->objects) || ignored(r, p->e)) continue;
                    if (g_count) g_prims_tested++;
                    float t, nn[3];
                    bool in;
                    if (!m) {
                        if (prim_test(r, p, b->t, &t, nn, &in) && (t < b->t || (t == b->t && b->prim < 0))) {
                            b->t = t, b->in = in, b->prim = (int32_t)pi;
                            memcpy(b->n, nn, 12);
                        }
                    } else if (prim_test(r, p, 1.0f, &t, nn, &in)) { // every one: kept by t, one per actor
                        uint32_t k = 0;
                        for (; k < m->n; k++)
                            if (S.prim[m->pp[k]].e == p->e && p->e != ENTITY_NONE) break;
                        if (k < m->n) {
                            if (t >= m->tt[k]) continue;
                            for (uint32_t q = k; q + 1 < m->n; q++) m->tt[q] = m->tt[q + 1], m->pp[q] = m->pp[q + 1], m->out[q] = m->out[q + 1];
                            m->n--;
                        }
                        uint32_t at = m->n;
                        while (at > 0 && m->tt[at - 1] > t) at--;
                        if (at >= m->cap) continue;
                        uint32_t last = m->n < m->cap ? m->n : m->cap - 1;
                        for (uint32_t q = last; q > at; q--) m->tt[q] = m->tt[q - 1], m->pp[q] = m->pp[q - 1], m->out[q] = m->out[q - 1];
                        m->tt[at] = t, m->pp[at] = (int32_t)pi;
                        memcpy(m->out[at].normal, nn, 12);
                        m->out[at].start_penetrating = in;
                        if (m->n < m->cap) m->n++;
                    }
                }
            } else
                kid[nk] = c, kt[nk++] = tn[l];
        }
        for (int i = 1; i < nk; i++) // (by t, descending: the nearest on top)
            for (int j = i; j > 0 && kt[j] > kt[j - 1]; j--) {
                float ft = kt[j];
                int32_t fk = kid[j];
                kt[j] = kt[j - 1], kid[j] = kid[j - 1], kt[j - 1] = ft, kid[j - 1] = fk;
            }
        for (int i = 0; i < nk && sp < 512; i++) stack[sp] = kid[i], stt[sp++] = kt[i];
    }
}

static void fill(const Ray* r, const SqQuery* q, float t, const float n[3], bool in, int32_t prim, Entity land_actor, SqHit* h) {
    float len = sqrtf(dot3(r->d, r->d));
    for (int k = 0; k < 3; k++) h->location[k] = r->o[k] + r->d[k] * t;
    memcpy(h->normal, n, 12), memcpy(h->impact_normal, n, 12);
    h->time = t, h->distance = t * len, h->blocking = 1, h->start_penetrating = in;
    if (prim >= 0 && r->cplx && S.prim[prim].cx >= 0) { // a triangle: the shape's point along the normal
        const Prim* p = &S.prim[prim];
        float sup = r->rq + fabsf(n[0]) * r->h[0] + fabsf(n[1]) * r->h[1] + fabsf(n[2]) * r->h[2];
        for (int k = 0; k < 3; k++) h->impact_point[k] = h->location[k] - n[k] * sup;
        h->actor = p->e, h->object = (uint8_t)p->obj;
    } else if (prim >= 0) {
        const Prim* p = &S.prim[prim];
        float c[3], v[3];
        for (int k = 0; k < 3; k++) c[k] = h->location[k] < p->mn[k] ? p->mn[k] : h->location[k] > p->mx[k] ? p->mx[k] : h->location[k], v[k] = h->location[k] - c[k];
        float l = sqrtf(dot3(v, v));
        if (p->r > 0.0f && l > 1e-6f)
            for (int k = 0; k < 3; k++) c[k] += v[k] / l * p->r;
        memcpy(h->impact_point, c, 12);
        h->actor = p->e, h->object = (uint8_t)p->obj;
    } else { // a landscape: the shape's bottom on the ground
        for (int k = 0; k < 3; k++) h->impact_point[k] = h->location[k];
        h->impact_point[2] -= q->shape == SQ_LINE ? 0.0f : q->shape == SQ_SPHERE ? r->rq : q->shape == SQ_BOX ? r->h[2] : r->h[2] + r->rq;
        h->actor = land_actor, h->object = SQ_OBJ_LANDSCAPE;
    }
}

static void miss(const SqQuery* q, SqHit* h) {
    memset(h, 0, sizeof *h);
    memcpy(h->location, q->end, 12), memcpy(h->impact_point, q->end, 12);
    float d[3] = { q->end[0] - q->start[0], q->end[1] - q->start[1], q->end[2] - q->start[2] };
    h->time = 1.0f, h->distance = sqrtf(dot3(d, d));
}

static float land_lift(const Ray* r, const SqQuery* q) {
    return q->shape == SQ_LINE ? 0.0f : q->shape == SQ_SPHERE ? r->rq : q->shape == SQ_BOX ? r->h[2] : r->h[2] + r->rq;
}

static bool trace_one(const SqQuery* q, SqHit* hit) {
    Ray r;
    ray_from(q, &r);
    Best b = { 1.0f, { 0, 0, 1 }, false, -1 };
    Entity la = ENTITY_NONE;
    bool land = false;
    float ln[3];
    if (S.has_land && (r.objects & SQ_OBJ_LANDSCAPE)) {
        float lt;
        if (landscape_raycast(q->start, q->end, land_lift(&r, q), &lt, ln, &la) && !ignored(&r, la)) b.t = lt, land = true;
    }
    bool before = land;
    float tl = b.t;
    walk(&r, &b, NULL);
    if (b.prim >= 0 && (!before || b.t <= tl)) {
        fill(&r, q, b.t, b.n, b.in, b.prim, ENTITY_NONE, hit);
        return true;
    }
    if (land) {
        fill(&r, q, tl, ln, false, -1, la, hit);
        return true;
    }
    miss(q, hit);
    return false;
}

// A Trace Complex query on the main thread: the meshes' triangles read from now on (the world copied again with them)
static void want_complex(World* w, const SqQuery* q, uint32_t n) {
    if (S.complex_wanted) return;
    for (uint32_t i = 0; i < n; i++)
        if (q[i].cplx) {
            S.complex_wanted = true, S.synced = false;
            sq_sync(w);
            return;
        }
}

bool sq_trace(World* w, const SqQuery* q, SqHit* hit) {
    double t0 = re_time_seconds();
    sq_sync(w);
    want_complex(w, q, 1);
    bool h = trace_one(q, hit);
    re_atomic_add(&S.q_frame, 1);
    S.ms_frame += (re_time_seconds() - t0) * 1000.0;
    return h;
}

uint32_t sq_trace_multi(World* w, const SqQuery* q, SqHit* hits, uint32_t cap) {
    double t0 = re_time_seconds();
    sq_sync(w);
    want_complex(w, q, 1);
    if (cap > 256) cap = 256;
    Ray r;
    ray_from(q, &r);
    float tt[257];
    int32_t pp[257];
    Multi m = { hits, tt, pp, 0, cap };
    walk(&r, &(Best){ 1.0f, { 0, 0, 1 }, false, -1 }, &m);
    for (uint32_t i = 0; i < m.n; i++) {
        float n[3];
        memcpy(n, hits[i].normal, 12);
        bool in = hits[i].start_penetrating;
        fill(&r, q, tt[i], n, in, pp[i], ENTITY_NONE, &hits[i]);
    }
    if (S.has_land && (r.objects & SQ_OBJ_LANDSCAPE) && cap) { // the ground: by its t among them
        float lt, ln[3];
        Entity la;
        if (landscape_raycast(q->start, q->end, land_lift(&r, q), &lt, ln, &la) && !ignored(&r, la)) {
            uint32_t at = m.n;
            while (at > 0 && hits[at - 1].time > lt) at--;
            if (at < cap) {
                uint32_t last = m.n < cap ? m.n : cap - 1;
                for (uint32_t k = last; k > at; k--) hits[k] = hits[k - 1];
                fill(&r, q, lt, ln, false, -1, la, &hits[at]);
                if (m.n < cap) m.n++;
            }
        }
    }
    re_atomic_add(&S.q_frame, 1);
    S.ms_frame += (re_time_seconds() - t0) * 1000.0;
    return m.n;
}

typedef struct BatchArg {
    const SqQuery* qs;
    SqHit* hits;
    uint32_t n, chunk;
    DdTrace* draw; // (the drawn batch's records, or NULL)
    uint32_t colors;
} BatchArg;
static void batch_fn(void* arg, int index) {
    const BatchArg* a = arg;
    uint32_t s = (uint32_t)index * a->chunk, e = s + a->chunk < a->n ? s + a->chunk : a->n;
    for (uint32_t i = s; i < e; i++) {
        trace_one(&a->qs[i], &a->hits[i]);
        if (a->draw) {
            DdTrace* d = &a->draw[i];
            memcpy(d->start, a->qs[i].start, 12), memcpy(d->end, a->qs[i].end, 12);
            d->t = a->hits[i].blocking ? fminf(a->hits[i].time, 0.99999f) : 1.0f, d->colors = a->colors;
        }
    }
}
void sq_trace_batch_drawn(World* w, const SqQuery* qs, uint32_t n, SqHit* hits, uint32_t colors) {
    if (!n) return;
    double t0 = re_time_seconds();
    sq_sync(w);
    want_complex(w, qs, n);
    BatchArg a = { qs, hits, n, 256, dd_reserve_traces(n), colors };
    re_parallel_for((int)((n + a.chunk - 1) / a.chunk), batch_fn, &a);
    re_atomic_add(&S.q_frame, (int32_t)n), re_atomic_add(&S.b_frame, 1);
    S.ms_frame += (re_time_seconds() - t0) * 1000.0;
}

void sq_trace_batch(World* w, const SqQuery* qs, uint32_t n, SqHit* hits) {
    if (!n) return;
    double t0 = re_time_seconds();
    sq_sync(w);
    want_complex(w, qs, n);
    BatchArg a = { qs, hits, n, 256, NULL, 0 };
    re_parallel_for((int)((n + a.chunk - 1) / a.chunk), batch_fn, &a);
    re_atomic_add(&S.q_frame, (int32_t)n), re_atomic_add(&S.b_frame, 1);
    S.ms_frame += (re_time_seconds() - t0) * 1000.0;
}

// --- (2026-11-19) Async batches: background threads (cores - 2: the main thread and the render's room) take 256-query
// chunks of the queued batches while the frame goes on; the next frame's start (sq_async_join) waits for any left (the
// main thread helps) and hands their lines to the frame's drawing. Each batch's room (queries, hits, records) is kept
// and reused two frames later (the even and the odd frames' 64 slots) ---
#define AS_SLOTS 64
#define AS_CHUNK 256
typedef struct AsJob {
    SqQuery* q;
    SqHit* h;
    DdTrace* d;
    uint32_t cap, n, colors, nchunks, frame;
    volatile int32_t next, done, live; // live: 1 queued (kicked), 0 not
    uint8_t state; // 0 free, 1 filling (begun), 2 kicked, 3 finished (results this frame)
} AsJob;
static struct {
    AsJob job[2][AS_SLOTS];
    ReGate* gate;
    ReThread* th[32];
    int nth;
    uint32_t frame; // (counted by sq_async_join)
    volatile int32_t stop;
} AS;

static bool as_take(AsJob* j) { // a chunk of j, run here; false: none left
    int32_t c = re_atomic_add(&j->next, 1) - 1;
    if (c >= (int32_t)j->nchunks) return false;
    uint32_t s = (uint32_t)c * AS_CHUNK, e = s + AS_CHUNK < j->n ? s + AS_CHUNK : j->n;
    for (uint32_t i = s; i < e; i++) {
        trace_one(&j->q[i], &j->h[i]);
        if (j->colors) {
            DdTrace* d = &j->d[i];
            memcpy(d->start, j->q[i].start, 12), memcpy(d->end, j->q[i].end, 12);
            d->t = j->h[i].blocking ? fminf(j->h[i].time, 0.99999f) : 1.0f, d->colors = j->colors;
        }
    }
    re_atomic_add(&j->done, 1);
    return true;
}
static int as_worker(void* arg) {
    (void)arg;
    int32_t seen = -1;
    while (!re_atomic_load(&AS.stop)) {
        seen = re_gate_wait(AS.gate, seen);
        for (bool any = true; any && !re_atomic_load(&AS.stop);) {
            any = false;
            for (int p = 0; p < 2; p++)
                for (int k = 0; k < AS_SLOTS; k++) {
                    AsJob* j = &AS.job[p][k];
                    if (re_atomic_load(&j->live))
                        while (as_take(j)) any = true;
                }
        }
    }
    return 0;
}
static void as_finish(AsJob* j) { // (the main thread: what is left run here, then waited for)
    while (as_take(j)) {}
    while (re_atomic_load(&j->done) < (int32_t)j->nchunks) re_yield();
    re_atomic_store(&j->live, 0);
    j->state = 3;
}
static void async_drain(void) {
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < AS_SLOTS; k++)
            if (AS.job[p][k].state == 2) as_finish(&AS.job[p][k]);
}

SqQuery* sq_async_begin(World* w, uint32_t n, uint32_t* handle) {
    *handle = 0;
    if (!n) return NULL;
    if (!AS.gate) { // (the first async batch: the threads, kept)
        AS.gate = re_gate_create();
        int nt = re_cpu_count() - 2;
        if (nt < 1) nt = 1;
        if (nt > 32) nt = 32;
        for (int i = 0; AS.gate && i < nt; i++)
            if ((AS.th[AS.nth] = re_thread_start(as_worker, NULL)) != NULL) AS.nth++;
    }
    sq_sync(w);
    uint32_t par = AS.frame & 1u;
    for (int k = 0; k < AS_SLOTS; k++) {
        AsJob* j = &AS.job[par][k];
        if (j->state == 2) as_finish(j); // (two frames old and never joined: a frame without the main loop's join)
        if (j->state == 0 || (j->state == 3 && j->frame != AS.frame)) {
            if (n > j->cap) { // (the room: grown once when a bigger batch comes)
                uint32_t c = j->cap ? j->cap : 1024;
                while (c < n) c *= 2;
                SqQuery* q = realloc(j->q, sizeof(SqQuery) * c);
                if (q) j->q = q;
                SqHit* h = q ? realloc(j->h, sizeof(SqHit) * c) : NULL;
                if (h) j->h = h;
                DdTrace* d = h ? realloc(j->d, sizeof(DdTrace) * c) : NULL;
                if (!d) return NULL;
                j->d = d, j->cap = c;
            }
            j->n = n, j->state = 1, j->frame = AS.frame;
            *handle = (AS.frame << 7 | par << 6 | (uint32_t)k) + 1u;
            return j->q;
        }
    }
    return NULL; // (64 this frame already)
}
static AsJob* as_job(uint32_t handle) {
    if (!handle) return NULL;
    uint32_t h = handle - 1u, k = h & 63u, par = (h >> 6) & 1u, f = h >> 7;
    AsJob* j = &AS.job[par][k];
    return j->frame == (f & 0x1FFFFFFu) ? j : NULL;
}
void sq_async_kick(uint32_t handle, uint32_t draw_colors) {
    AsJob* j = as_job(handle);
    if (!j || j->state != 1) return;
    want_complex(S.w, j->q, j->n);
    j->colors = draw_colors, j->nchunks = (j->n + AS_CHUNK - 1) / AS_CHUNK, j->next = j->done = 0;
    if (!draw_colors) j->colors = 0;
    j->state = 2;
    re_atomic_add(&S.q_frame, (int32_t)j->n), re_atomic_add(&S.b_frame, 1);
    re_atomic_store(&j->live, 1);
    if (AS.gate && AS.nth) re_gate_bump(AS.gate);
    else as_finish(j); // (no threads: here and now)
}
bool sq_async_result(uint32_t handle, const SqHit** hits, uint32_t* n) {
    AsJob* j = as_job(handle);
    if (!j || j->state != 3 || j->frame == AS.frame) return *hits = NULL, *n = 0, false;
    return *hits = j->h, *n = j->n, true;
}
void sq_async_join(void) {
    uint32_t par = AS.frame & 1u;
    for (int k = 0; k < AS_SLOTS; k++) {
        AsJob* j = &AS.job[par][k];
        if (j->state == 1) j->state = 0; // (begun, never kicked)
        if (j->state != 2) continue;
        as_finish(j);
        if (j->colors) dd_add_trace_block(j->d, j->n);
    }
    AS.frame = (AS.frame + 1u) & 0x1FFFFFFu;
}

void sq_frame_end(void) {
    sq_async_join();
    S.st.queries_frame = (uint32_t)re_atomic_load(&S.q_frame), S.st.batches_frame = (uint32_t)re_atomic_load(&S.b_frame);
    S.st.query_ms_frame = S.ms_frame;
    re_atomic_store(&S.q_frame, 0), re_atomic_store(&S.b_frame, 0);
    S.ms_frame = 0.0;
}

SqStats sq_stats(void) { return S.st; }

static void collect(DvList* l, const DvContext* c) {
    (void)c;
    char info[48];
    snprintf(info, sizeof info, "%u queries, %u batches", S.st.queries_frame, S.st.batches_frame);
    dv_add(l, "Traces (last frame)", info, (float)S.st.query_ms_frame, ENTITY_NONE);
    snprintf(info, sizeof info, "%u primitives, %u nodes", S.st.prims, S.st.nodes);
    dv_add(l, "BVH build", info, (float)S.st.build_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u boxes, %u pawns", S.st.boxes, S.st.pawns);
    dv_add(l, "BVH refit", info, (float)S.st.refit_ms, ENTITY_NONE);
    l->ms = S.st.query_ms_frame;
    snprintf(l->note, sizeof l->note, "Line / Sphere / Box / Capsule traces on the CPU (a batch's wall time on the workers)");
}
void sq_register_debug(void) { dv_register("Scene Queries", collect); }

// ---------------------------------------------------------------------------------------------------------------------
// --sq-test: the shapes against a brute force over random worlds, then the batches' speed

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static float frand(float a, float b) {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return a + (b - a) * (float)((g_rng >> 40) & 0xFFFFFF) / 16777216.0f;
}

// a brute force: every primitive, the rounded box sampled along the segment (no analytic test shared), its first entry
// refined by halving
static float brute(const SqQuery* q, int32_t* who) {
    Ray r;
    ray_from(q, &r);
    float best = 2.0f;
    *who = -1;
    for (uint32_t i = 0; i < S.nprim; i++) {
        const Prim* p = &S.prim[i];
        float lo[3], hi[3], rr = p->r + r.rq;
        for (int k = 0; k < 3; k++) lo[k] = p->mn[k] - r.h[k], hi[k] = p->mx[k] + r.h[k];
        (void)lo;
        const int steps = 4096;
        float prev = 0.0f;
        for (int s = 0; s <= steps; s++) {
            float t = (float)s / steps, dist;
            {
                float dd = 0.0f;
                for (int k = 0; k < 3; k++) {
                    float x = r.o[k] + r.d[k] * t, c = x < lo[k] ? lo[k] : x > hi[k] ? hi[k] : x;
                    dd += (x - c) * (x - c);
                }
                dist = sqrtf(dd) - rr;
            }
            if (dist <= 0.0f) {
                float a = s ? prev : 0.0f, b = t;
                for (int it = 0; it < 30 && s; it++) {
                    float mid = (a + b) * 0.5f, dd = 0.0f;
                    for (int k = 0; k < 3; k++) {
                        float x = r.o[k] + r.d[k] * mid, c = x < lo[k] ? lo[k] : x > hi[k] ? hi[k] : x;
                        dd += (x - c) * (x - c);
                    }
                    if (sqrtf(dd) - rr <= 0.0f) b = mid;
                    else a = mid;
                }
                if (b < best) best = b, *who = (int32_t)i;
                break;
            }
            prev = t;
        }
    }
    return best;
}

static void random_world(uint32_t nbox, uint32_t ncap, float extent) {
    S.nprim = 0;
    grow((void**)&S.prim, &S.cap_prim, nbox + ncap, sizeof(Prim));
    for (uint32_t i = 0; i < nbox; i++) {
        Prim* p = &S.prim[S.nprim++];
        float c[3] = { frand(-extent, extent), frand(-extent, extent), frand(0.0f, 600.0f) }, h[3] = { frand(10, 200), frand(10, 200), frand(10, 150) };
        for (int k = 0; k < 3; k++) p->mn[k] = c[k] - h[k], p->mx[k] = c[k] + h[k];
        p->r = 0.0f, p->obj = SQ_OBJ_STATIC, p->e = 1000u + i;
    }
    for (uint32_t i = 0; i < ncap; i++) {
        Prim* p = &S.prim[S.nprim++];
        float c[3] = { frand(-extent, extent), frand(-extent, extent), frand(90.0f, 400.0f) };
        p->mn[0] = p->mx[0] = c[0], p->mn[1] = p->mx[1] = c[1], p->mn[2] = c[2] - 50.0f, p->mx[2] = c[2] + 50.0f;
        p->r = 34.0f, p->obj = SQ_OBJ_PAWN, p->e = 500000u + i;
    }
    S.has_land = false;
    S.synced = true, S.synced_frame = bp_query_play_frame(), S.w = NULL;
    build();
}

static void random_query(SqQuery* q, int shape, float extent) {
    memset(q, 0, sizeof *q);
    for (int k = 0; k < 2; k++) q->start[k] = frand(-extent, extent), q->end[k] = q->start[k] + frand(-3000.0f, 3000.0f);
    q->start[2] = frand(-100.0f, 800.0f), q->end[2] = frand(-100.0f, 800.0f);
    q->shape = (uint8_t)shape;
    q->radius = frand(5.0f, 60.0f);
    q->half[0] = frand(5.0f, 60.0f), q->half[1] = frand(5.0f, 60.0f), q->half[2] = shape == SQ_CAPSULE ? q->radius + frand(0.0f, 80.0f) : frand(5.0f, 60.0f);
}

int sq_selftest(void) {
    int fails = 0;
    // 1. exactness: 600 queries of each shape against a brute force (small worlds: the brute force is slow)
    random_world(400, 40, 3000.0f);
    const char* names[SQ_SHAPE_COUNT] = { "line", "sphere", "box", "capsule" };
    for (int sh = 0; sh < SQ_SHAPE_COUNT; sh++) {
        uint32_t agree = 0, total = 0, hits = 0;
        float worst = 0.0f;
        for (int i = 0; i < 600; i++) {
            SqQuery q;
            random_query(&q, sh, 3000.0f);
            SqHit h;
            trace_one(&q, &h);
            int32_t who;
            float bt = brute(&q, &who);
            bool bh = bt <= 1.0f;
            float len = sqrtf((q.end[0] - q.start[0]) * (q.end[0] - q.start[0]) + (q.end[1] - q.start[1]) * (q.end[1] - q.start[1]) +
                              (q.end[2] - q.start[2]) * (q.end[2] - q.start[2]));
            float err = bh && h.blocking ? fabsf(bt - h.time) * len : 0.0f; // (cm)
            total++, hits += h.blocking;
            bool ok = (bh == (h.blocking != 0)) && err < 0.5f + len / 4096.0f; // (the brute force's step)
            if (!ok && bh != (h.blocking != 0)) { // a graze within the step: the same within a step's length
                float gap = bh ? bt : h.time;
                ok = fabsf(1.0f - gap) * len < len / 4096.0f * 2.0f;
            }
            if (!ok && h.blocking && (!bh || h.time < bt) && h.actor) { // an earlier hit the samples stepped over: a graze of
                // an edge or a corner. Right when the BVH's point is on that primitive's (summed) surface
                uint32_t bi = h.actor >= 500000u ? 400u + (h.actor - 500000u) : h.actor - 1000u;
                Ray r0;
                ray_from(&q, &r0);
                const Prim* p = &S.prim[bi];
                float dd = 0.0f;
                for (int k = 0; k < 3; k++) {
                    float lo = p->mn[k] - r0.h[k], hi = p->mx[k] + r0.h[k], x = h.location[k], c = x < lo ? lo : x > hi ? hi : x;
                    dd += (x - c) * (x - c);
                }
                ok = fabsf(sqrtf(dd) - (p->r + r0.rq)) < 0.02f;
            }
            if (!ok && getenv("REALENGINE_SQ_DEBUG")) {
                printf("  mismatch %s: bvh %d t=%.6f brute t=%.6f prim %d; start %.2f %.2f %.2f end %.2f %.2f %.2f\n", names[sh], h.blocking, h.time, bt, who,
                       q.start[0], q.start[1], q.start[2], q.end[0], q.end[1], q.end[2]);
                if (h.actor) {
                    uint32_t bi = h.actor >= 500000u ? 400u + (h.actor - 500000u) : h.actor - 1000u;
                    printf("    bvh's prim %u: mn %.2f %.2f %.2f mx %.2f %.2f %.2f r %.2f\n", bi, S.prim[bi].mn[0], S.prim[bi].mn[1], S.prim[bi].mn[2], S.prim[bi].mx[0],
                           S.prim[bi].mx[1], S.prim[bi].mx[2], S.prim[bi].r);
                }
                if (who >= 0)
                    printf("    prim mn %.2f %.2f %.2f mx %.2f %.2f %.2f r %.2f\n", S.prim[who].mn[0], S.prim[who].mn[1], S.prim[who].mn[2], S.prim[who].mx[0],
                           S.prim[who].mx[1], S.prim[who].mx[2], S.prim[who].r);
            }
            agree += ok;
            if (h.blocking && bh) worst = fmaxf(worst, err);
        }
        printf("[RealEngine] sq-test %s: %u/%u agree with the brute force (%u hits, worst %.3f cm)\n", names[sh], agree, total, hits, worst);
        if (agree + 3 < total) fails++, printf("[RealEngine] FAIL: sq-test %s exactness\n", names[sh]);
    }
    // 2. multi: sorted, one per actor, the first the single trace's
    {
        uint32_t bad = 0;
        for (int i = 0; i < 300; i++) {
            SqQuery q;
            random_query(&q, SQ_LINE, 3000.0f);
            SqHit one, many[64];
            trace_one(&q, &one);
            uint32_t n = sq_trace_multi(NULL, &q, many, 64);
            for (uint32_t k = 1; k < n; k++) bad += many[k].time < many[k - 1].time;
            if (one.blocking != (n > 0) || (n && fabsf(one.time - many[0].time) > 1e-5f)) bad++;
        }
        printf("[RealEngine] sq-test multi: %u disagreements\n", bad);
        if (bad) fails++, printf("[RealEngine] FAIL: sq-test multi\n");
    }
    // 3. speed: 200 000 traces in a batch over a world of 20 000 boxes and 500 capsules
    random_world(20000, 500, 40000.0f);
    printf("[RealEngine] sq-test BVH: %u primitives, %u nodes, built in %.2f ms\n", S.nprim, S.nnode, S.st.build_ms);
    const uint32_t N = 200000;
    SqQuery* qs = malloc(sizeof *qs * N); // (a test's)
    SqHit* hs = malloc(sizeof *hs * N);
    if (!qs || !hs) return free(qs), free(hs), 1;
    for (int sh = 0; sh < SQ_SHAPE_COUNT; sh++) {
        for (uint32_t i = 0; i < N; i++) random_query(&qs[i], sh, 40000.0f);
        double best = 1e9;
        for (int rep = 0; rep < 5; rep++) {
            double t0 = re_time_seconds();
            BatchArg a = { qs, hs, N, 256, NULL, 0 };
            re_parallel_for((int)((N + a.chunk - 1) / a.chunk), batch_fn, &a);
            best = fmin(best, (re_time_seconds() - t0) * 1000.0);
        }
        uint32_t nh = 0;
        for (uint32_t i = 0; i < N; i++) nh += hs[i].blocking;
        double one = 0.0;
        { // one thread, for the per-trace cost
            double t0 = re_time_seconds();
            for (uint32_t i = 0; i < 20000; i++) trace_one(&qs[i], &hs[i]);
            one = (re_time_seconds() - t0) * 1e9 / 20000.0;
            g_count = true, g_nodes_seen = g_prims_tested = 0;
            for (uint32_t i = 0; i < 20000; i++) trace_one(&qs[i], &hs[i]);
            g_count = false;
            printf("[RealEngine] sq-test %s: %.1f nodes and %.1f primitives a trace\n", names[sh], g_nodes_seen / 20000.0, g_prims_tested / 20000.0);
        }
        printf("[RealEngine] sq-test batch %s: %u traces in %.2f ms on %d threads (%u hit; %.0f ns a trace on one thread)\n", names[sh], N, best,
               re_pool_threads(), nh, one);
        const char* lim = getenv("REALENGINE_SQ_MAX_MS");
        if (lim && best > atof(lim)) fails++, printf("[RealEngine] FAIL: sq-test batch %s took %.2f ms > %s\n", names[sh], best, lim);
    }
    free(qs), free(hs);
    sq_reset();
    printf("[RealEngine] sq-test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
