#include "cloth_mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "render_gpu.h"

void cloth_mesh_free(ClothMeshTopo* t) {
    free(t->rest), free(t->uv), free(t->adj_off), free(t->adj), free(t->tris), free(t->color);
    memset(t, 0, sizeof *t);
}

static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

void cloth_mesh_key(uint32_t key[8], uint32_t mesh, const float scale[3], float band, const float axis[3], bool pin) {
    key[0] = mesh, key[1] = fbits(scale[0]), key[2] = fbits(scale[1]), key[3] = fbits(scale[2]);
    key[4] = pin ? fbits(band) : 0xFFFFFFFFu, key[5] = fbits(axis[0]), key[6] = fbits(axis[1]), key[7] = fbits(axis[2]);
}

static uint32_t hash3(int32_t x, int32_t y, int32_t z) { return (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u; }
static uint32_t hash2(uint32_t a, uint32_t b) { return a * 2654435761u ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2)); }

static float dist3(const float* a, const float* b) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool cloth_mesh_build_arrays(ClothMeshTopo* t, const float* pos, const float* uv, uint32_t nin, const uint32_t* idx, uint32_t ni,
                             const float scale[3], float band, const float axis[3], bool pin) {
    cloth_mesh_free(t);
    if (!pos || !idx || ni < 3 || nin < 3) return false;
    uint32_t ntri_in = ni / 3u;
    // --- weld: the input vertices that are one place (0.01 cm) become one ---
    uint32_t cap = 1;
    while (cap < nin * 2u) cap <<= 1;
    uint32_t* table = malloc(sizeof(uint32_t) * cap);
    uint32_t* remap = malloc(sizeof(uint32_t) * nin);
    float* sp = malloc(sizeof(float) * 3 * nin);
    float* su = malloc(sizeof(float) * 2 * nin);
    uint32_t* tris = malloc(sizeof(uint32_t) * 3 * ntri_in);
    if (!table || !remap || !sp || !su || !tris) return free(table), free(remap), free(sp), free(su), free(tris), false;
    memset(table, 0xFF, sizeof(uint32_t) * cap);
    uint32_t nv = 0;
    for (uint32_t i = 0; i < nin; i++) {
        float p[3] = { pos[3 * i] * scale[0], pos[3 * i + 1] * scale[1], pos[3 * i + 2] * scale[2] };
        int32_t q[3] = { (int32_t)lroundf(p[0] * 100.0f), (int32_t)lroundf(p[1] * 100.0f), (int32_t)lroundf(p[2] * 100.0f) };
        uint32_t h = hash3(q[0], q[1], q[2]) & (cap - 1u);
        uint32_t found = 0xFFFFFFFFu;
        while (table[h] != 0xFFFFFFFFu) {
            const float* o = sp + 3 * table[h];
            if (lroundf(o[0] * 100.0f) == q[0] && lroundf(o[1] * 100.0f) == q[1] && lroundf(o[2] * 100.0f) == q[2]) { found = table[h]; break; }
            h = (h + 1u) & (cap - 1u);
        }
        if (found == 0xFFFFFFFFu) {
            if (nv >= CLOTH_MESH_MAX_VERTS) { free(table), free(remap), free(sp), free(su), free(tris); return false; }
            found = nv++;
            memcpy(sp + 3 * found, p, 12);
            su[2 * found] = uv ? uv[2 * i] : 0.0f, su[2 * found + 1] = uv ? uv[2 * i + 1] : 0.0f;
            table[h] = found;
        }
        remap[i] = found;
    }
    uint32_t nt = 0;
    for (uint32_t k = 0; k < ntri_in && nt < 2u * nv + 2u; k++) {
        uint32_t a = idx[3 * k], b = idx[3 * k + 1], c = idx[3 * k + 2];
        if (a >= nin || b >= nin || c >= nin) continue;
        a = remap[a], b = remap[b], c = remap[c];
        if (a == b || b == c || a == c) continue; // (welded away)
        tris[3 * nt] = a, tris[3 * nt + 1] = b, tris[3 * nt + 2] = c, nt++;
    }
    if (nt > 2u * nv) nt = 2u * nv; // (the BLAS's room: 6 indices a vertex; a mesh has ~2 triangles a vertex)
    free(table), free(remap);
    if (nt == 0) return free(sp), free(su), free(tris), false;
    // --- the edges, with the vertex across each side (two: an inner edge, bending across it) ---
    uint32_t ecap = 1;
    while (ecap < nt * 4u) ecap <<= 1;
    uint32_t* ek = malloc(sizeof(uint32_t) * 2 * ecap); // a, b (a < b); 0xFFFFFFFF empty
    uint32_t* eo = malloc(sizeof(uint32_t) * 2 * ecap); // the vertices across (or 0xFFFFFFFF)
    uint32_t* deg = calloc(nv, sizeof(uint32_t));
    if (!ek || !eo || !deg) return free(ek), free(eo), free(deg), free(sp), free(su), free(tris), false;
    memset(ek, 0xFF, sizeof(uint32_t) * 2 * ecap), memset(eo, 0xFF, sizeof(uint32_t) * 2 * ecap);
    for (uint32_t k = 0; k < nt; k++)
        for (int s = 0; s < 3; s++) {
            uint32_t a = tris[3 * k + s], b = tris[3 * k + (s + 1) % 3], o = tris[3 * k + (s + 2) % 3];
            if (a > b) { uint32_t x = a; a = b, b = x; }
            uint32_t h = hash2(a, b) & (ecap - 1u);
            while (ek[2 * h] != 0xFFFFFFFFu && (ek[2 * h] != a || ek[2 * h + 1] != b)) h = (h + 1u) & (ecap - 1u);
            if (ek[2 * h] == 0xFFFFFFFFu) ek[2 * h] = a, ek[2 * h + 1] = b, eo[2 * h] = o;
            else if (eo[2 * h + 1] == 0xFFFFFFFFu && eo[2 * h] != o) eo[2 * h + 1] = o;
        }
    // --- each vertex's entries: count, then fill (capped) ---
    for (uint32_t h = 0; h < ecap; h++) {
        if (ek[2 * h] == 0xFFFFFFFFu) continue;
        deg[ek[2 * h]]++, deg[ek[2 * h + 1]]++;
        if (eo[2 * h + 1] != 0xFFFFFFFFu) deg[eo[2 * h]]++, deg[eo[2 * h + 1]]++;
    }
    for (uint32_t k = 0; k < nt; k++)
        for (int s = 0; s < 3; s++) deg[tris[3 * k + s]]++;
    t->adj_off = malloc(sizeof(uint32_t) * 2 * nv);
    uint32_t total = 0;
    for (uint32_t v = 0; v < nv; v++) {
        uint32_t d = deg[v] < CLOTH_MESH_MAX_ADJ ? deg[v] : CLOTH_MESH_MAX_ADJ;
        if (t->adj_off) t->adj_off[2 * v] = total, t->adj_off[2 * v + 1] = 0;
        total += d;
    }
    t->adj = malloc(sizeof(uint32_t) * 2 * (total ? total : 1));
    if (!t->adj_off || !t->adj) { free(ek), free(eo), free(deg), free(sp), free(su), free(tris), cloth_mesh_free(t); return false; }
#define PUSH(v, x, y)                                                                                                   \
    do {                                                                                                                \
        uint32_t* oc = &t->adj_off[2 * (v)];                                                                            \
        uint32_t lim = deg[v] < CLOTH_MESH_MAX_ADJ ? deg[v] : CLOTH_MESH_MAX_ADJ;                                       \
        if (oc[1] < lim) t->adj[2 * (oc[0] + oc[1])] = (x), t->adj[2 * (oc[0] + oc[1]) + 1] = (y), oc[1]++;             \
    } while (0)
    double esum = 0.0;
    uint32_t en = 0;
    for (uint32_t h = 0; h < ecap; h++) { // (springs first: a capped vertex drops triangles, not springs)
        if (ek[2 * h] == 0xFFFFFFFFu) continue;
        uint32_t a = ek[2 * h], b = ek[2 * h + 1];
        float L = dist3(sp + 3 * a, sp + 3 * b) * 0.01f;
        PUSH(a, b | CLOTH_ADJ_STRETCH << 30, fbits(L));
        PUSH(b, a | CLOTH_ADJ_STRETCH << 30, fbits(L));
        esum += L, en++;
        uint32_t o1 = eo[2 * h], o2 = eo[2 * h + 1];
        if (o2 != 0xFFFFFFFFu) {
            float Lb = dist3(sp + 3 * o1, sp + 3 * o2) * 0.01f;
            PUSH(o1, o2 | CLOTH_ADJ_BEND << 30, fbits(Lb));
            PUSH(o2, o1 | CLOTH_ADJ_BEND << 30, fbits(Lb));
        }
    }
    for (uint32_t k = 0; k < nt; k++)
        for (int s = 0; s < 3; s++) PUSH(tris[3 * k + s], k | CLOTH_ADJ_TRI << 30, 0u);
#undef PUSH
    free(ek), free(eo);
    // --- colors: the most connected first, the lowest color none of its spring neighbors has ---
    uint32_t* ord = malloc(sizeof(uint32_t) * nv);
    t->color = malloc(nv);
    if (!ord || !t->color) { free(ord), free(deg), free(sp), free(su), free(tris), cloth_mesh_free(t); return false; }
    for (uint32_t v = 0; v < nv; v++) ord[v] = v;
    for (uint32_t i = 1; i < nv; i++) { // (insertion sort by degree, falling: stable and fine for 16k)
        uint32_t x = ord[i], j = i;
        while (j > 0 && deg[ord[j - 1]] < deg[x]) ord[j] = ord[j - 1], j--;
        ord[j] = x;
    }
    memset(t->color, 0xFF, nv);
    uint32_t ncol = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t v = ord[i], used = 0;
        for (uint32_t e = 0; e < t->adj_off[2 * v + 1]; e++) {
            uint32_t x = t->adj[2 * (t->adj_off[2 * v] + e)];
            if ((x >> 30) == CLOTH_ADJ_TRI) continue;
            uint8_t cc = t->color[x & 0x3FFFFFFFu];
            if (cc != 0xFF) used |= 1u << cc;
        }
        uint32_t c = 0;
        while (c < CLOTH_MESH_COLORS - 1u && (used & (1u << c))) c++;
        t->color[v] = (uint8_t)c;
        if (c + 1u > ncol) ncol = c + 1u;
    }
    free(ord), free(deg);
    // --- rest places, pins, sizes ---
    t->rest = malloc(sizeof(float) * 4 * nv);
    t->uv = su, su = NULL;
    t->tris = tris;
    if (!t->rest) { free(sp), cloth_mesh_free(t); return false; }
    float ax[3] = { axis[0], axis[1], axis[2] }, al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (al < 1e-6f) ax[0] = ax[1] = 0.0f, ax[2] = 1.0f, al = 1.0f;
    ax[0] /= al, ax[1] /= al, ax[2] /= al;
    float top = -1e30f, rad = 0.0f;
    for (uint32_t v = 0; v < nv; v++) {
        const float* p = sp + 3 * v;
        top = fmaxf(top, p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2]);
        rad = fmaxf(rad, sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
    }
    t->pinned = 0;
    for (uint32_t v = 0; v < nv; v++) {
        const float* p = sp + 3 * v;
        bool pinned = pin && p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2] >= top - fmaxf(band, 0.01f);
        t->rest[4 * v] = p[0], t->rest[4 * v + 1] = p[1], t->rest[4 * v + 2] = p[2], t->rest[4 * v + 3] = pinned ? 1.0f : 0.0f;
        t->pinned += pinned;
    }
    double area = 0.0;
    for (uint32_t k = 0; k < nt; k++) {
        const float *a = sp + 3 * tris[3 * k], *b = sp + 3 * tris[3 * k + 1], *c = sp + 3 * tris[3 * k + 2];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        area += 0.5 * sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]) * 1e-4;
    }
    free(sp);
    t->nv = nv, t->nt = nt, t->nadj = total, t->ncolors = ncol;
    t->area = (float)area, t->edge = en ? (float)(esum / en) : 0.01f, t->radius = rad;
    return true;
}

bool cloth_mesh_build(ClothMeshTopo* t, uint32_t mesh, const float scale[3], float band, const float axis[3], bool pin) {
    const GpuVertex* V = NULL;
    const uint32_t* I = NULL;
    uint32_t fi = 0, ic = 0;
    int32_t vo = 0;
    if (!(mesh & 0x80000000u) || render_mesh_slot(mesh) < SHAPE_COUNT) return false; // (an imported mesh only)
    render_mesh_cpu(mesh, &V, &I, &fi, &ic, &vo);
    if (!V || !I || ic < 3) return false;
    // (one corner a vertex: the weld makes them one again; the renderer's LOD 0)
    float* pos = malloc(sizeof(float) * 3 * ic);
    float* uv = malloc(sizeof(float) * 2 * ic);
    uint32_t* idx = malloc(sizeof(uint32_t) * ic);
    bool ok = false;
    if (pos && uv && idx) {
        for (uint32_t k = 0; k < ic; k++) {
            const GpuVertex* g = &V[(int64_t)I[fi + k] + vo];
            memcpy(pos + 3 * k, g->pos, 12), memcpy(uv + 2 * k, g->uv, 8);
            idx[k] = k;
        }
        ok = cloth_mesh_build_arrays(t, pos, uv, ic, idx, ic, scale, band, axis, pin);
    }
    free(pos), free(uv), free(idx);
    if (ok) cloth_mesh_key(t->key, mesh, scale, band, axis, pin);
    return ok;
}

int cloth_mesh_selftest(void) {
    // a tube 16 around x 6 down, its seam column doubled (u 0 and 1 at the same place), as an importer gives it
    enum { SEG = 16, RING = 6 };
    float pos[(SEG + 1) * (RING + 1) * 3], uv[(SEG + 1) * (RING + 1) * 2];
    uint32_t idx[SEG * RING * 6], ni = 0;
    for (int r = 0; r <= RING; r++)
        for (int s = 0; s <= SEG; s++) {
            int i = r * (SEG + 1) + s;
            float a = 6.2831853f * (float)s / SEG;
            pos[3 * i] = 20.0f * cosf(a), pos[3 * i + 1] = 20.0f * sinf(a), pos[3 * i + 2] = -10.0f * (float)r;
            uv[2 * i] = (float)s / SEG, uv[2 * i + 1] = (float)r / RING;
        }
    for (int r = 0; r < RING; r++)
        for (int s = 0; s < SEG; s++) {
            uint32_t a = (uint32_t)(r * (SEG + 1) + s), b = a + 1, c = a + SEG + 1, d = c + 1;
            idx[ni++] = a, idx[ni++] = c, idx[ni++] = b, idx[ni++] = b, idx[ni++] = c, idx[ni++] = d;
        }
    ClothMeshTopo t;
    memset(&t, 0, sizeof t);
    const float one[3] = { 1, 1, 1 }, up[3] = { 0, 0, 1 };
    int fails = 0;
    if (!cloth_mesh_build_arrays(&t, pos, uv, (SEG + 1) * (RING + 1), idx, ni, one, 5.0f, up, true)) return 1;
    // welded: 16 x 7 vertices; 2 x 16 x 6 triangles; every spring's ends differ in color
    uint32_t bad = 0, stretch = 0, bend = 0;
    for (uint32_t v = 0; v < t.nv; v++)
        for (uint32_t e = 0; e < t.adj_off[2 * v + 1]; e++) {
            uint32_t x = t.adj[2 * (t.adj_off[2 * v] + e)], k = x >> 30, o = x & 0x3FFFFFFFu;
            if (k == CLOTH_ADJ_TRI) continue;
            stretch += k == CLOTH_ADJ_STRETCH, bend += k == CLOTH_ADJ_BEND;
            if (t.color[o] == t.color[v]) bad++;
        }
    printf("[RealEngine] Cloth mesh: %u vertices (welded from %u), %u triangles, %u stretch, %u bending, %u colors, %u pinned, %.3f m2\n", t.nv,
           (SEG + 1) * (RING + 1), t.nt, stretch / 2, bend / 2, t.ncolors, t.pinned, t.area);
    if (t.nv != SEG * (RING + 1) || t.nt != SEG * RING * 2) printf("[RealEngine] FAIL: cloth mesh: the weld\n"), fails++;
    if (bad) printf("[RealEngine] FAIL: cloth mesh: %u springs inside one color\n", bad), fails++;
    if (t.pinned != SEG) printf("[RealEngine] FAIL: cloth mesh: the top ring pinned (%u)\n", t.pinned), fails++;
    if (stretch / 2 != (uint32_t)(SEG * RING * 3 + SEG)) printf("[RealEngine] FAIL: cloth mesh: the edges\n"), fails++;
    cloth_mesh_free(&t);
    return fails;
}
