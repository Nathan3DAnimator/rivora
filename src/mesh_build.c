#include "mesh_build.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meshoptimizer.h"
#include "platform.h"
#include "platform_files.h"

static void oct_decode(uint32_t o, float n[3]) {
    float x = (float)(int16_t)(o & 0xFFFFu) / 32767.0f, y = (float)(int16_t)(o >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}

void mesh_morphs_free(MeshMorphs* m) {
    free(m->name), free(m->weight), free(m->first), free(m->d);
    memset(m, 0, sizeof *m);
}

void mesh_built_free(MeshBuilt* m) {
    free(m->v), free(m->idx), free(m->col);
    mesh_morphs_free(&m->morph);
    memset(m, 0, sizeof *m);
}

static void bounds_area(MeshBuilt* m, const GpuVertex* v, const uint32_t* idx, uint32_t ni) {
    for (int k = 0; k < 3; k++) m->mn[k] = 1e30f, m->mx[k] = -1e30f;
    double area = 0.0;
    for (uint32_t i = 0; i + 2 < ni; i += 3) {
        const float *a = v[idx[i]].pos, *b = v[idx[i + 1]].pos, *c = v[idx[i + 2]].pos;
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        area += 0.5 * sqrt((double)cr[0] * cr[0] + (double)cr[1] * cr[1] + (double)cr[2] * cr[2]);
        for (int k = 0; k < 3; k++) {
            m->mn[k] = fminf(m->mn[k], fminf(a[k], fminf(b[k], c[k])));
            m->mx[k] = fmaxf(m->mx[k], fmaxf(a[k], fmaxf(b[k], c[k])));
        }
    }
    m->area = (float)area;
}

// LOD 0's order: triangles for the post-transform cache, then for overdraw (front-most first, 5% more cache misses
// allowed), the vertices in the order the indices first use them (tag, a word per vertex, moved with them: the morphs'
// source vertex)
static uint32_t order(GpuVertex* v, uint32_t nv, uint32_t* idx, uint32_t ni, uint32_t* tag) {
    meshopt_optimizeVertexCache(idx, idx, ni, nv);
    meshopt_optimizeOverdraw(idx, idx, ni, v[0].pos, nv, sizeof(GpuVertex), 1.05f);
    uint32_t* remap = tag ? malloc(sizeof(uint32_t) * (size_t)nv) : NULL;
    if (!remap) {
        if (tag) memset(tag, 0xFF, sizeof(uint32_t) * (size_t)nv); // (no memory: the vertices lose their morphs, not their place)
        return (uint32_t)meshopt_optimizeVertexFetch(v, idx, ni, v, nv, sizeof(GpuVertex));
    }
    uint32_t n = (uint32_t)meshopt_optimizeVertexFetchRemap(remap, idx, ni, nv);
    meshopt_remapIndexBuffer(idx, idx, ni, remap);
    meshopt_remapVertexBuffer(v, v, nv, sizeof(GpuVertex), remap);
    meshopt_remapVertexBuffer(tag, tag, nv, sizeof(uint32_t), remap);
    free(remap);
    return n;
}

// The LODs of a welded mesh (wv, wnv; LOD 0's indices widx): each a quarter of the one before (the cull's rule: a level
// for every x0.4 of radius on screen, a sixth of the pixels), simplified with the normals and UVs weighed, the mesh's
// open borders locked (a part cut by material or by bone keeps meeting its neighbor). Stops when it can't halve.
// Writes the LOD indices (into wv) after out[0..], returns how many, their counts and errors (cm)
// (2026-11-24, the viewers' LOD Settings) the LODs asked for: how many and each one's share of LOD 0's triangles
// (mesh_build_indexed_lods; thread-local: the importers build on workers at the same time). NULL: the automatic rule
static __declspec(thread) const MeshLodOptions* t_lod_opts;
static __declspec(thread) bool t_free_borders; // (2026-11-24) make_lods for a skin: its borders free when locked ones keep it whole

static uint32_t make_lods(const GpuVertex* wv, uint32_t wnv, const uint32_t* widx, uint32_t wni, uint32_t* out, uint32_t cap,
                          uint32_t first[], uint32_t count[], float err[]) {
    const MeshLodOptions* o = t_lod_opts;
    float* attr = malloc(sizeof(float) * 5 * (size_t)wnv);
    uint32_t* tmp = malloc(sizeof(uint32_t) * (size_t)wni);
    if (!attr || !tmp) {
        free(attr), free(tmp);
        return 0;
    }
    for (uint32_t i = 0; i < wnv; i++) {
        oct_decode(wv[i].normal_oct, &attr[i * 5]);
        attr[i * 5 + 3] = wv[i].uv[0], attr[i * 5 + 4] = wv[i].uv[1];
    }
    static const float weights[5] = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };
    float scale = meshopt_simplifyScale(wv[0].pos, wnv, sizeof(GpuVertex));
    const uint32_t* src = widx;
    uint32_t src_n = wni, n = 0, used = 0;
    for (uint32_t l = 1; l < RMESH_MAX_LODS; l++) {
        if (o && l >= o->lods) break;
        size_t target = (size_t)(src_n / 4u / 3u) * 3u;
        if (o) { // (its share of LOD 0, never more than the level before)
            float pc = o->percent[l] > 0.01f ? o->percent[l] : 100.0f / (float)(1u << (2u * l));
            target = (size_t)((double)(wni / 3u) * pc / 100.0) * 3u;
            if (target >= src_n) target = (size_t)(src_n / 3u) * 3u - 3u;
        }
        if (target < 3) break;
        float e = 0.0f;
        size_t got = meshopt_simplifyWithAttributes(tmp, src, src_n, wv[0].pos, wnv, sizeof(GpuVertex), attr, sizeof(float) * 5, weights,
                                                    5, NULL, target, o ? 1.0f : 0.25f, meshopt_SimplifyLockBorder, &e);
        if (o && got >= (size_t)src_n) break; // (asked for: nothing more to take away)
        if (!o && got > (size_t)src_n / 2u) {
            // many small pieces (cards: every edge a border, nothing collapses with the borders locked): the borders free
            // and the pieces under the level's error pruned (1%, 4%, 16% of the mesh): far foliage thins, as Unreal's
            got = meshopt_simplifyWithAttributes(tmp, src, src_n, wv[0].pos, wnv, sizeof(GpuVertex), attr, sizeof(float) * 5, weights, 5,
                                                 NULL, target, 0.01f * (float)(1u << (2u * (l - 1u))), meshopt_SimplifyPrune, &e);
            // (2026-11-24, item 4) a skin (t_free_borders): its LODs share LOD 0's vertices, no other mesh meets its
            // borders: free, the error a share of its size (the Unamed scene's capsule, its LED and UDIM parts all borders,
            // kept its 9284 triangles at every distance: 3929 of them were 36.5 M triangles)
            if (got > (size_t)src_n * 7u / 10u && t_free_borders)
                got = meshopt_simplifyWithAttributes(tmp, src, src_n, wv[0].pos, wnv, sizeof(GpuVertex), attr, sizeof(float) * 5, weights, 5,
                                                     NULL, target, 1.0f, 0, &e); // (the target leads; the error measured picks the LOD on screen)
            // (a skin whose topology blocks the collapses, many small shells and non-manifold edges: the Unamed capsule stopped at
            // 75% with its borders free) the sloppy simplifier, by the positions' grid: its far LODs only need the outline
            if (got > (size_t)src_n * 7u / 10u && t_free_borders)
                got = meshopt_simplifySloppy(tmp, src, src_n, wv[0].pos, wnv, sizeof(GpuVertex), NULL, target, 1.0f, &e);
            if (getenv("REALENGINE_LOD_TRACE")) printf("[RealEngine] lod %u: tries ended at %zu of %u (free borders %d)\n", l, got, src_n, (int)t_free_borders);
            if (got > (size_t)src_n * 7u / 10u) break;
        }
        if (getenv("REALENGINE_LOD_TRACE")) printf("[RealEngine] lod %u: %u -> %zu (target %zu)\n", l, src_n, got, target); // (2026-11-24)
        if (got < 3 || used + got > cap) break; // (can't reduce: the next level would be the same)
        meshopt_optimizeVertexCache(tmp, tmp, got, wnv);
        memcpy(out + used, tmp, sizeof(uint32_t) * got);
        first[n] = used, count[n] = (uint32_t)got, err[n] = e * scale + (n ? err[n - 1] : 0.0f); // (from the level before: summed)
        src = out + used, src_n = (uint32_t)got;
        used += (uint32_t)got, n++;
    }
    free(attr), free(tmp);
    return n;
}

static uint32_t uf_find(uint32_t* p, uint32_t x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}
static uint32_t comp_hash(uint32_t c) {
    c ^= c >> 16, c *= 0x7feb352du, c ^= c >> 15, c *= 0x846ca68bu, c ^= c >> 16;
    return c;
}

// The LODs of a mesh of many small pieces (cards: a hairdo's, a bush's; over a sixteenth as many pieces as triangles),
// which simplification can't reduce: thinned like Unreal's foliage. Level l keeps a quarter of the pieces of level l-1
// (nested: one in 4^l by a hash), each scaled 2^l about its middle, so the screen they cover stays about the same.
// Its error: a piece's mean radius at that scale / 4, so the cull thins when a piece is under ~4 px of radius.
// New vertices (out_v, the kept pieces' scaled copies) and indices into them; returns the levels made (0: not pieces)
static uint32_t thin_lods(const GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, GpuVertex** out_v, uint32_t* out_nv,
                          uint32_t** out_i, uint32_t first[], uint32_t count[], float err[]) {
    uint32_t ntri = ni / 3u, nl = 0;
    uint32_t* parent = malloc(sizeof(uint32_t) * (size_t)nv);
    uint32_t* comp = malloc(sizeof(uint32_t) * (size_t)nv); // a vertex's piece (dense)
    if (!parent || !comp) goto fail;
    for (uint32_t i = 0; i < nv; i++) parent[i] = i;
    for (uint32_t t = 0; t < ntri; t++) {
        uint32_t a = uf_find(parent, idx[t * 3]), b = uf_find(parent, idx[t * 3 + 1]), c = uf_find(parent, idx[t * 3 + 2]);
        if (b != a) parent[b] = a;
        if (c != a && c != b) parent[uf_find(parent, c)] = uf_find(parent, a);
    }
    uint32_t np = 0;
    for (uint32_t i = 0; i < nv; i++) comp[i] = UINT32_MAX;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t r = uf_find(parent, i);
        if (comp[r] == UINT32_MAX) comp[r] = np++;
        comp[i] = comp[r];
    }
    if ((uint64_t)np * 16u < ntri || np < 64u) goto fail; // (big connected surfaces: simplification's job)
    {
        float(*mid)[4] = calloc(np, sizeof(float) * 4); // xyz: the piece's middle (mean of its vertices), w: count then radius
        float* rad = calloc(np, sizeof(float));
        GpuVertex* lv = malloc(sizeof(GpuVertex) * (size_t)nv); // (the levels' copies: a third of nv at most)
        uint32_t* li = malloc(sizeof(uint32_t) * (size_t)ni);
        uint32_t* vmap = malloc(sizeof(uint32_t) * (size_t)nv);
        if (!mid || !rad || !lv || !li || !vmap) {
            free(mid), free(rad), free(lv), free(li), free(vmap);
            goto fail;
        }
        for (uint32_t i = 0; i < nv; i++) {
            float* m = mid[comp[i]];
            m[0] += v[i].pos[0], m[1] += v[i].pos[1], m[2] += v[i].pos[2], m[3] += 1.0f;
        }
        for (uint32_t p = 0; p < np; p++) mid[p][0] /= mid[p][3], mid[p][1] /= mid[p][3], mid[p][2] /= mid[p][3];
        double rsum = 0.0;
        for (uint32_t i = 0; i < nv; i++) {
            const float* m = mid[comp[i]];
            float d[3] = { v[i].pos[0] - m[0], v[i].pos[1] - m[1], v[i].pos[2] - m[2] };
            rad[comp[i]] = fmaxf(rad[comp[i]], sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
        for (uint32_t p = 0; p < np; p++) rsum += rad[p];
        float r_mean = (float)(rsum / np);
        uint32_t lnv = 0, lni = 0;
        for (uint32_t l = 1; l < RMESH_MAX_LODS; l++) {
            uint32_t mask = (1u << (2u * l)) - 1u; // (nested: kept at level l => kept below it)
            float s = (float)(1u << l);
            for (uint32_t i = 0; i < nv; i++) vmap[i] = UINT32_MAX;
            uint32_t f0 = lni;
            for (uint32_t t = 0; t < ntri; t++) {
                uint32_t pc = comp[idx[t * 3]];
                if (comp_hash(pc) & mask) continue;
                for (int k = 0; k < 3; k++) {
                    uint32_t o = idx[t * 3 + k];
                    if (vmap[o] == UINT32_MAX) {
                        if (lnv >= nv) goto done; // (can't happen: a third at most)
                        GpuVertex g = v[o];
                        const float* m = mid[pc];
                        for (int q = 0; q < 3; q++) g.pos[q] = m[q] + (g.pos[q] - m[q]) * s;
                        lv[lnv] = g, vmap[o] = lnv++;
                    }
                    li[lni++] = vmap[o];
                }
            }
            if (lni - f0 < 3u) break;
            first[nl] = f0, count[nl] = lni - f0, err[nl] = r_mean * s * 0.5f / 4.0f;
            nl++;
        }
    done:
        free(mid), free(rad), free(vmap);
        if (!nl) {
            free(lv), free(li);
            goto fail;
        }
        *out_v = lv, *out_nv = lnv, *out_i = li;
    }
    free(parent), free(comp);
    return nl;
fail:
    free(parent), free(comp);
    return 0;
}

static bool build(GpuVertex* v, uint32_t nv, uint32_t* idx, uint32_t ni, bool atlas, bool lods, MeshBuilt* out, uint32_t* tag) {
    double t0 = re_time_seconds();
    memset(out, 0, sizeof *out);
    bounds_area(out, v, idx, ni);
    nv = order(v, nv, idx, ni, tag);
    out->flags = atlas ? 0u : RMESH_NO_ATLAS;
    out->lods.count = 1, out->lods.first_index[0] = 0, out->lods.index_count[0] = ni, out->lods.error[0] = 0.0f;
    uint32_t lod_first[RMESH_MAX_LODS], lod_count[RMESH_MAX_LODS], nlods = 0;
    float lod_err[RMESH_MAX_LODS];
    uint32_t* lidx = NULL;
    GpuVertex* lv = NULL; // (a mesh with an atlas: its LODs on welded copies of the corners, uv2 -1)
    uint32_t lnv = 0, lni = 0;
    bool own = atlas; // (the LODs on vertices of their own, after LOD 0's)
    const MeshLodOptions* o = t_lod_opts;
    uint32_t min_tris = o ? (o->min_tris ? o->min_tris : 16u) : 1024u;
    if (o && o->lods <= 1) lods = false;
    if (lods && ni / 3u >= min_tris && !atlas && !o) { // many small pieces: thinned (simplification can't reduce cards)
        nlods = thin_lods(v, nv, idx, ni, &lv, &lnv, &lidx, lod_first, lod_count, lod_err);
        for (uint32_t l = 0; l < nlods; l++) lni = lod_first[l] + lod_count[l];
        own = nlods > 0;
    }
    if (lods && ni / 3u >= min_tris && !nlods) {
        const GpuVertex* wv = v;
        uint32_t wnv = nv, *widx = idx;
        uint32_t* remap = NULL;
        if (atlas) { // welded without the per-triangle UV2 (the LODs read no atlas)
            lv = malloc(sizeof(GpuVertex) * (size_t)nv);
            remap = malloc(sizeof(uint32_t) * (size_t)nv);
            widx = malloc(sizeof(uint32_t) * (size_t)ni);
            if (!lv || !remap || !widx) goto lods_done;
            memcpy(lv, v, sizeof(GpuVertex) * (size_t)nv);
            for (uint32_t i = 0; i < nv; i++) lv[i].uv2[0] = lv[i].uv2[1] = -1.0f;
            wnv = (uint32_t)meshopt_generateVertexRemap(remap, idx, ni, lv, nv, sizeof(GpuVertex));
            meshopt_remapVertexBuffer(lv, lv, nv, sizeof(GpuVertex), remap);
            meshopt_remapIndexBuffer(widx, idx, ni, remap);
            wv = lv;
        }
        lidx = malloc(sizeof(uint32_t) * (size_t)ni);
        if (lidx) nlods = make_lods(wv, wnv, widx, ni, lidx, ni, lod_first, lod_count, lod_err);
        for (uint32_t l = 0; l < nlods; l++) lni = lod_first[l] + lod_count[l];
        if (atlas && nlods) lnv = (uint32_t)meshopt_optimizeVertexFetch(lv, lidx, lni, lv, wnv, sizeof(GpuVertex)); // (only what they use)
    lods_done:
        free(remap);
        if (widx != idx) free(widx);
    }
    // the output: LOD 0's vertices (then the LODs' own, with an atlas), LOD 0's indices then the LODs'
    out->nv = nv + lnv;
    out->ni = ni + lni;
    out->v = malloc(sizeof(GpuVertex) * (size_t)out->nv);
    out->idx = malloc(sizeof(uint32_t) * (size_t)out->ni);
    bool ok = out->v && out->idx;
    if (ok) {
        memcpy(out->v, v, sizeof(GpuVertex) * (size_t)nv);
        if (lnv) memcpy(out->v + nv, lv, sizeof(GpuVertex) * (size_t)lnv);
        memcpy(out->idx, idx, sizeof(uint32_t) * (size_t)ni);
        uint32_t base = own ? nv : 0u;
        for (uint32_t i = 0; i < lni; i++) out->idx[ni + i] = lidx[i] + base;
        for (uint32_t l = 0; l < nlods; l++) {
            out->lods.first_index[l + 1] = ni + lod_first[l];
            out->lods.index_count[l + 1] = lod_count[l];
            out->lods.error[l + 1] = lod_err[l];
        }
        out->lods.count = 1 + nlods;
    }
    free(lv), free(lidx);
    if (!ok) mesh_built_free(out);
    out->ms = (re_time_seconds() - t0) * 1000.0;
    return ok;
}

// The colors of the built vertices: LOD 0's from their corner (tag), the LODs' own copies by their position (a hash of
// LOD 0's positions; one not found: white)
static bool colors_gather(MeshBuilt* out, const uint32_t* tag, uint32_t nv0, const uint32_t* colors, uint32_t ncorners) {
    out->col = malloc(sizeof(uint32_t) * (size_t)out->nv);
    if (!out->col) return false;
    for (uint32_t i = 0; i < nv0 && i < out->nv; i++) out->col[i] = tag[i] < ncorners ? colors[tag[i]] : 0xFFFFFFFFu;
    if (out->nv > nv0) {
        uint32_t cap = 1;
        while (cap < nv0 * 2u) cap <<= 1;
        uint32_t* tab = malloc(sizeof(uint32_t) * cap);
        if (tab) memset(tab, 0xFF, sizeof(uint32_t) * cap);
#define POS_HASH(p) (((uint32_t)((p)[0] * 997.0f) * 73856093u ^ (uint32_t)((p)[1] * 997.0f) * 19349663u ^ (uint32_t)((p)[2] * 997.0f) * 83492791u) & (cap - 1u))
        for (uint32_t i = 0; tab && i < nv0; i++) {
            uint32_t h = POS_HASH(out->v[i].pos);
            while (tab[h] != UINT32_MAX) h = (h + 1u) & (cap - 1u);
            tab[h] = i;
        }
        for (uint32_t i = nv0; i < out->nv; i++) {
            out->col[i] = 0xFFFFFFFFu;
            for (uint32_t h = tab ? POS_HASH(out->v[i].pos) : 0u; tab && tab[h] != UINT32_MAX; h = (h + 1u) & (cap - 1u))
                if (!memcmp(out->v[tab[h]].pos, out->v[i].pos, 12)) {
                    out->col[i] = out->col[tab[h]];
                    break;
                }
        }
#undef POS_HASH
        free(tab);
    }
    out->flags |= RMESH_COLORS;
    return true;
}

bool mesh_build_corners_col(const GpuVertex* corners, const uint32_t* colors, uint32_t ntri, bool lods, MeshBuilt* out) {
    if (!colors) return mesh_build_corners(corners, ntri, lods, out);
    memset(out, 0, sizeof *out);
    if (!ntri) return false;
    uint32_t n = ntri * 3u;
    bool atlas = ntri <= MESH_ATLAS_MAX_TRIS;
    GpuVertex* v = malloc(sizeof(GpuVertex) * (size_t)n);
    uint32_t* idx = malloc(sizeof(uint32_t) * (size_t)n);
    uint32_t* tag = malloc(sizeof(uint32_t) * (size_t)n);
    uint32_t* remap = atlas ? NULL : malloc(sizeof(uint32_t) * (size_t)n);
    if (!v || !idx || !tag || (!atlas && !remap)) {
        free(v), free(idx), free(tag), free(remap);
        return false;
    }
    memcpy(v, corners, sizeof(GpuVertex) * (size_t)n);
    uint32_t nv = n;
    if (atlas) {
        for (uint32_t i = 0; i < n; i++) idx[i] = i, tag[i] = i;
    } else { // welded: the corners that are the same vertex (position, normal, UV) and the same color
        for (uint32_t i = 0; i < n; i++) v[i].uv2[0] = v[i].uv2[1] = -1.0f;
        struct meshopt_Stream st[2] = { { v, sizeof(GpuVertex), sizeof(GpuVertex) }, { colors, 4, 4 } };
        nv = (uint32_t)meshopt_generateVertexRemapMulti(remap, NULL, n, n, st, 2);
        for (uint32_t i = 0; i < n; i++) tag[remap[i]] = i;
        meshopt_remapVertexBuffer(v, v, n, sizeof(GpuVertex), remap);
        meshopt_remapIndexBuffer(idx, NULL, n, remap);
    }
    bool ok = build(v, nv, idx, n, atlas, lods, out, tag) && colors_gather(out, tag, nv, colors, n);
    if (!ok) mesh_built_free(out);
    free(v), free(idx), free(tag), free(remap);
    return ok;
}

bool mesh_build_corners(const GpuVertex* corners, uint32_t ntri, bool lods, MeshBuilt* out) {
    memset(out, 0, sizeof *out);
    if (!ntri) return false;
    uint32_t n = ntri * 3u;
    bool atlas = ntri <= MESH_ATLAS_MAX_TRIS;
    GpuVertex* v = malloc(sizeof(GpuVertex) * (size_t)n);
    uint32_t* idx = malloc(sizeof(uint32_t) * (size_t)n);
    if (!v || !idx) {
        free(v), free(idx);
        return false;
    }
    memcpy(v, corners, sizeof(GpuVertex) * (size_t)n);
    uint32_t nv = n;
    if (atlas) {
        for (uint32_t i = 0; i < n; i++) idx[i] = i;
    } else { // welded: the corners that are the same vertex (position, normal, UV) one vertex
        for (uint32_t i = 0; i < n; i++) v[i].uv2[0] = v[i].uv2[1] = -1.0f;
        uint32_t* remap = malloc(sizeof(uint32_t) * (size_t)n);
        if (!remap) {
            free(v), free(idx);
            return false;
        }
        nv = (uint32_t)meshopt_generateVertexRemap(remap, NULL, n, v, n, sizeof(GpuVertex));
        meshopt_remapVertexBuffer(v, v, n, sizeof(GpuVertex), remap);
        meshopt_remapIndexBuffer(idx, NULL, n, remap);
        free(remap);
    }
    bool ok = build(v, nv, idx, n, atlas, lods, out, NULL);
    free(v), free(idx);
    return ok;
}

// The morphs of the built vertices: vertex i's deltas are its source vertex tag[i]'s (none: UINT32_MAX)
static void morphs_gather(MeshBuilt* out, const uint32_t* tag, const MeshMorphs* src) {
    MeshMorphs* m = &out->morph;
    uint32_t nd = 0;
    for (uint32_t i = 0; i < out->nv; i++)
        if (tag[i] < src->nv) nd += src->first[tag[i] + 1] - src->first[tag[i]];
    if (!nd) return; // (none of its vertices moves: no morphs)
    m->count = src->count, m->nv = out->nv, m->nd = nd;
    m->name = malloc(sizeof *m->name * src->count);
    m->weight = malloc(sizeof(float) * src->count);
    m->first = malloc(sizeof(uint32_t) * ((size_t)out->nv + 1u));
    m->d = malloc(sizeof(RMeshMorphDelta) * nd);
    if (!m->name || !m->weight || !m->first || !m->d) {
        mesh_morphs_free(m);
        return;
    }
    memcpy(m->name, src->name, sizeof *m->name * src->count);
    memcpy(m->weight, src->weight, sizeof(float) * src->count);
    uint32_t at = 0;
    for (uint32_t i = 0; i < out->nv; i++) {
        m->first[i] = at;
        if (tag[i] >= src->nv) continue;
        uint32_t a = src->first[tag[i]], b = src->first[tag[i] + 1];
        memcpy(m->d + at, src->d + a, sizeof(RMeshMorphDelta) * (b - a));
        at += b - a;
    }
    m->first[out->nv] = at;
    out->flags |= RMESH_MORPHS;
}

// (2026-11-24, item 4: the tests' dense skin, REALENGINE_IMPORT_SUBDIV=k) each triangle into 4, k times: an edge's middle
// the two ends' mean position and UV, the normal and the weights of the smaller end (bytes: the same on both triangles of
// an edge, welded after), so the 236 triangles of the tests' rig become a dense skin (k 4: 60 416) for its DAG
static GpuVertex* skin_subdiv(const GpuVertex* c, uint32_t* ntri, int k) {
    GpuVertex* cur = malloc(sizeof(GpuVertex) * 3u * *ntri);
    if (!cur) return NULL;
    memcpy(cur, c, sizeof(GpuVertex) * 3u * *ntri);
    for (int it = 0; it < k && *ntri <= 4000000u; it++) {
        GpuVertex* nx = malloc(sizeof(GpuVertex) * 12u * *ntri);
        if (!nx) break;
        for (uint32_t t = 0; t < *ntri; t++) {
            const GpuVertex* p = cur + 3u * t;
            GpuVertex m[3];
            for (int e = 0; e < 3; e++) {
                const GpuVertex *a = &p[e], *b = &p[(e + 1) % 3];
                const GpuVertex* s = memcmp(a, b, sizeof *a) <= 0 ? a : b;
                m[e] = *s;
                for (int q = 0; q < 3; q++) m[e].pos[q] = (a->pos[q] + b->pos[q]) * 0.5f;
                for (int q = 0; q < 2; q++) m[e].uv[q] = (a->uv[q] + b->uv[q]) * 0.5f;
            }
            GpuVertex* o = nx + 12u * t;
            o[0] = p[0], o[1] = m[0], o[2] = m[2];
            o[3] = m[0], o[4] = p[1], o[5] = m[1];
            o[6] = m[2], o[7] = m[1], o[8] = p[2];
            o[9] = m[0], o[10] = m[1], o[11] = m[2];
        }
        free(cur);
        cur = nx, *ntri *= 4u;
    }
    return cur;
}

// (2026-11-25, item 4) LOD 0's islands joined by position (a UV or normal seam doesn't split one; a loose bolt does): how
// many, and how many under small_tris triangles
uint32_t mesh_islands(const MeshBuilt* m, uint32_t small_tris, uint32_t* n_small) {
    if (n_small) *n_small = 0;
    if (!m->nv || !m->lods.count) return 0;
    uint32_t *rm = malloc(sizeof(uint32_t) * m->nv), *par = malloc(sizeof(uint32_t) * m->nv), *cnt = calloc(m->nv, sizeof(uint32_t));
    uint32_t isl = 0;
    if (rm && par && cnt) {
        struct meshopt_Stream ps = { m->v[0].pos, 12, sizeof(GpuVertex) };
        meshopt_generateVertexRemapMulti(rm, NULL, m->nv, m->nv, &ps, 1); // (rm[k]: its place's id, 0..places-1)
        for (uint32_t k = 0; k < m->nv; k++) par[k] = k;
        const uint32_t* ix = m->idx + m->lods.first_index[0];
        for (uint32_t t = 0; t + 2 < m->lods.index_count[0]; t += 3)
            for (int e = 1; e < 3; e++) {
                uint32_t a = rm[ix[t]], c = rm[ix[t + e]];
                while (par[a] != a) a = par[a] = par[par[a]];
                while (par[c] != c) c = par[c] = par[par[c]];
                if (a != c) par[a] = c;
            }
        for (uint32_t t = 0; t + 2 < m->lods.index_count[0]; t += 3) {
            uint32_t a = rm[ix[t]];
            while (par[a] != a) a = par[a];
            cnt[a]++;
        }
        for (uint32_t k = 0; k < m->nv; k++)
            if (cnt[k]) {
                isl++;
                if (n_small && cnt[k] < small_tris) (*n_small)++;
            }
    }
    free(rm), free(par), free(cnt);
    return isl;
}

// (2026-11-25, item 4) the distance from p to the triangle a b c (Ericson's closest point)
static float pt_tri_dist2(const float p[3], const float a[3], const float b[3], const float c[3]) {
    float ab[3], ac[3], ap[3], q[3];
    for (int k = 0; k < 3; k++) ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k];
#define DOT3(x, y) ((x)[0] * (y)[0] + (x)[1] * (y)[1] + (x)[2] * (y)[2])
    float d1 = DOT3(ab, ap), d2 = DOT3(ac, ap);
    if (d1 <= 0 && d2 <= 0) memcpy(q, a, 12);
    else {
        float bp[3], cp[3];
        for (int k = 0; k < 3; k++) bp[k] = p[k] - b[k], cp[k] = p[k] - c[k];
        float d3 = DOT3(ab, bp), d4 = DOT3(ac, bp), d5 = DOT3(ab, cp), d6 = DOT3(ac, cp);
        float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0 && d4 <= d3) memcpy(q, b, 12);
        else if (d6 >= 0 && d5 <= d6) memcpy(q, c, 12);
        else if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k]; }
        else if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); for (int k = 0; k < 3; k++) q[k] = a[k] + w * ac[k]; }
        else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
        } else {
            float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
            for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;
        }
    }
#undef DOT3
    float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}
static int by_float(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
// (2026-11-25, item 4) LOD l's real one-sided error: LOD 0's vertices (up to 4096 of them, evenly) to its surface, by brute
// force (offline: --skin-lods-test). Its max and mean (cm); returns the 99th percentile
double mesh_lod_true_error(const MeshBuilt* m, uint32_t l, float* mx, float* mean) {
    *mx = 0.0f, *mean = 0.0f;
    if (l >= m->lods.count || !m->nv) return 0.0;
    uint32_t step = m->nv > 4096u ? m->nv / 4096u : 1u, ns = 0;
    float* d = malloc(sizeof(float) * (m->nv / step + 1u));
    if (!d) return 0.0;
    const uint32_t* ix = m->idx + m->lods.first_index[l];
    uint32_t nt = m->lods.index_count[l] / 3u;
    double sum = 0.0;
    for (uint32_t k = 0; k < m->nv; k += step) {
        float best = FLT_MAX;
        for (uint32_t t = 0; t < nt; t++) {
            float q = pt_tri_dist2(m->v[k].pos, m->v[ix[3 * t]].pos, m->v[ix[3 * t + 1]].pos, m->v[ix[3 * t + 2]].pos);
            if (q < best) best = q;
        }
        d[ns] = sqrtf(best), sum += d[ns];
        if (d[ns] > *mx) *mx = d[ns];
        ns++;
    }
    *mean = (float)(sum / ns);
    qsort(d, ns, sizeof(float), by_float);
    double p99 = d[(uint32_t)((ns - 1u) * 0.99)];
    free(d);
    return p99;
}

static bool mesh_build_skinned_in(const GpuVertex* corners, uint32_t ntri, const uint32_t* csrc, const MeshMorphs* morphs, MeshBuilt* out);
bool mesh_build_skinned(const GpuVertex* corners, uint32_t ntri, const uint32_t* csrc, const MeshMorphs* morphs, MeshBuilt* out) {
    const char* sd = getenv("REALENGINE_IMPORT_SUBDIV");
    if (ntri && sd && atoi(sd) > 0 && !(csrc && morphs && morphs->count)) { // (tests only; a skin with morphs as it is)
        uint32_t nt = ntri;
        GpuVertex* d = skin_subdiv(corners, &nt, atoi(sd));
        if (d) {
            printf("[RealEngine] Skin subdivided for the tests: %u -> %u triangles\n", ntri, nt);
            bool ok = mesh_build_skinned_in(d, nt, NULL, NULL, out);
            free(d);
            return ok;
        }
    }
    return mesh_build_skinned_in(corners, ntri, csrc, morphs, out);
}
static bool mesh_build_skinned_in(const GpuVertex* corners, uint32_t ntri, const uint32_t* csrc, const MeshMorphs* morphs, MeshBuilt* out) {
    memset(out, 0, sizeof *out);
    if (!ntri) return false;
    uint32_t n = ntri * 3u;
    bool morph = csrc && morphs && morphs->count && morphs->nd && morphs->first && morphs->d;
    GpuVertex* v = malloc(sizeof(GpuVertex) * (size_t)n);
    uint32_t* idx = malloc(sizeof(uint32_t) * (size_t)n);
    uint32_t* remap = malloc(sizeof(uint32_t) * (size_t)n);
    uint32_t* tag = morph ? malloc(sizeof(uint32_t) * (size_t)n) : NULL;
    bool ok = v && idx && remap && (tag || !morph);
    if (ok) {
        memcpy(v, corners, sizeof(GpuVertex) * (size_t)n);
        uint32_t nv;
        if (tag) { // (the same vertex with the same weights and the same source: its morphs are the same)
            memcpy(tag, csrc, sizeof(uint32_t) * (size_t)n);
            struct meshopt_Stream st[2] = { { v, sizeof(GpuVertex), sizeof(GpuVertex) }, { tag, sizeof(uint32_t), sizeof(uint32_t) } };
            nv = (uint32_t)meshopt_generateVertexRemapMulti(remap, NULL, n, n, st, 2);
            meshopt_remapVertexBuffer(tag, tag, n, sizeof(uint32_t), remap);
        } else
            nv = (uint32_t)meshopt_generateVertexRemap(remap, NULL, n, v, n, sizeof(GpuVertex)); // (the weights' bits too)
        meshopt_remapVertexBuffer(v, v, n, sizeof(GpuVertex), remap);
        meshopt_remapIndexBuffer(idx, NULL, n, remap);
        ok = build(v, nv, idx, n, true, false, out, tag); // (atlas: uv2 kept as it is, the weights)
        if (ok) out->flags = RMESH_NO_ATLAS | RMESH_SKINNED;
        if (ok && tag) morphs_gather(out, tag, morphs);
        if (ok) mesh_skinned_lods(out); // (after the morphs: they follow LOD 0's vertices, the LODs only add indices)
    }
    free(v), free(idx), free(remap), free(tag);
    return ok;
}

// A skin's LODs on its own vertices (RMESH_SKINNED: skin.comp bends LOD 0's vertices, every LOD's indices read them):
// simplified over the positions welded (meshopt_generateShadowIndexBuffer: the UV and normal seams split a vertex, as
// borders they locked the simplification), each index the first vertex at its place. No vertex moves (a collapse keeps
// one of the two), so the bones' weights stay right. Appended after LOD 0's indices; false: none made (small, or the
// simplification couldn't halve it)
#define SL_CACHE_MAGIC 0x314C5352u // "RSL1"
static bool mesh_skinned_lods_in(MeshBuilt* mb);
bool mesh_skinned_lods(MeshBuilt* mb) {
    if (!mb || !(mb->flags & RMESH_SKINNED) || mb->lods.count != 1u) return false;
    t_free_borders = true; // (2026-11-24: below, make_lods: a skin's borders free when locked ones keep it whole)
    bool r = mesh_skinned_lods_in(mb);
    t_free_borders = false;
    return r;
}
static bool mesh_skinned_lods_in(MeshBuilt* mb) {
    uint32_t ni0 = mb->lods.index_count[0], f0 = mb->lods.first_index[0];
    const MeshLodOptions* o = t_lod_opts; // (2026-11-24: the viewer's LOD Settings: its own count and shares, no cache)
    if (ni0 / 3u < (o ? (o->min_tris ? o->min_tris : 16u) : 4096u) || f0 + ni0 > mb->ni) return false; // (a small skin: its LODs save nothing)
    uint32_t first[RMESH_MAX_LODS], count[RMESH_MAX_LODS], n = 0, lni = 0;
    float err[RMESH_MAX_LODS];
    uint32_t* lidx = NULL;
    // kept in the cache folder by content (300 k triangles: ~0.9 s made, ~5 ms read), as the clusters
    uint64_t key = 1469598103934665603ull ^ SL_CACHE_MAGIC;
    const uint8_t* bytes[2] = { (const uint8_t*)mb->v, (const uint8_t*)(mb->idx + f0) };
    size_t sizes[2] = { sizeof(GpuVertex) * (size_t)mb->nv, sizeof(uint32_t) * (size_t)ni0 };
    for (int k = 0; k < 2; k++) {
        size_t q = 0;
        for (; q + 8 <= sizes[k]; q += 8) {
            uint64_t w;
            memcpy(&w, bytes[k] + q, 8);
            key = (key ^ w) * 1099511628211ull;
            key ^= key >> 29;
        }
        for (; q < sizes[k]; q++) key = (key ^ bytes[k][q]) * 1099511628211ull;
    }
    char cache[600] = "";
    if (!o && re_cache_dir(cache, sizeof cache)) {
        size_t l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/meshes");
        re_make_dirs(cache);
        l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/%016llx_sl1.bin", (unsigned long long)key);
        FILE* f = re_fopen(cache, "rb");
        if (f) { // magic, LODs, indices; per LOD first, count, error; the indices
            uint32_t h[3];
            bool ok = fread(h, sizeof h, 1, f) == 1 && h[0] == SL_CACHE_MAGIC && h[1] >= 1u && h[1] < RMESH_MAX_LODS && h[2] && h[2] <= ni0;
            ok = ok && fread(first, 4, h[1], f) == h[1] && fread(count, 4, h[1], f) == h[1] && fread(err, 4, h[1], f) == h[1];
            lidx = ok ? malloc(sizeof(uint32_t) * (size_t)h[2]) : NULL;
            ok = ok && lidx && fread(lidx, 4, h[2], f) == h[2];
            for (uint32_t k = 0; ok && k < h[2]; k++) ok = lidx[k] < mb->nv;
            for (uint32_t k = 0; ok && k < h[1]; k++) ok = first[k] + count[k] <= h[2];
            fclose(f);
            if (ok) n = h[1], lni = h[2];
            else free(lidx), lidx = NULL;
        }
    }
    if (!n) {
        // simplified over the positions welded: each index the first vertex at its place
        uint32_t* shadow = malloc(sizeof(uint32_t) * (size_t)ni0);
        lidx = malloc(sizeof(uint32_t) * (size_t)ni0);
        if (shadow && lidx) {
            meshopt_generateShadowIndexBuffer(shadow, mb->idx + f0, ni0, mb->v, mb->nv, sizeof(float) * 3, sizeof(GpuVertex));
            n = make_lods(mb->v, mb->nv, shadow, ni0, lidx, ni0, first, count, err);
        }
        free(shadow);
        if (n >= RMESH_MAX_LODS) n = RMESH_MAX_LODS - 1u;
        lni = n ? first[n - 1] + count[n - 1] : 0u;
        if (n && cache[0]) {
            FILE* f = re_fopen(cache, "wb");
            if (f) {
                uint32_t h[3] = { SL_CACHE_MAGIC, n, lni };
                bool ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(first, 4, n, f) == n && fwrite(count, 4, n, f) == n &&
                          fwrite(err, 4, n, f) == n && fwrite(lidx, 4, lni, f) == lni;
                fclose(f);
                if (!ok) remove(cache); // (a half file: made again next time)
            }
        }
    }
    uint32_t* ni = n ? realloc(mb->idx, sizeof(uint32_t) * ((size_t)mb->ni + lni)) : NULL;
    if (ni) {
        mb->idx = ni;
        memcpy(mb->idx + mb->ni, lidx, sizeof(uint32_t) * lni);
        for (uint32_t l = 0; l < n; l++) {
            mb->lods.first_index[l + 1] = mb->ni + first[l];
            mb->lods.index_count[l + 1] = count[l];
            mb->lods.error[l + 1] = err[l];
        }
        mb->lods.count = n + 1u;
        mb->ni += lni;
    }
    free(lidx);
    return ni != NULL;
}

bool mesh_build_indexed(const GpuVertex* v0, uint32_t nv, const uint32_t* idx0, uint32_t ni, bool atlas, bool lods, MeshBuilt* out) {
    memset(out, 0, sizeof *out);
    GpuVertex* v = malloc(sizeof(GpuVertex) * (size_t)nv);
    uint32_t* idx = malloc(sizeof(uint32_t) * (size_t)ni);
    bool ok = v && idx && nv && ni;
    if (ok) {
        memcpy(v, v0, sizeof(GpuVertex) * (size_t)nv);
        memcpy(idx, idx0, sizeof(uint32_t) * (size_t)ni);
        if (!atlas)
            for (uint32_t i = 0; i < nv; i++) v[i].uv2[0] = v[i].uv2[1] = -1.0f;
        ok = build(v, nv, idx, ni, atlas, lods, out, NULL);
    }
    free(v), free(idx);
    return ok;
}

bool mesh_build_indexed_lods(const GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, bool atlas, const MeshLodOptions* o, MeshBuilt* out) {
    t_lod_opts = o;
    bool ok = mesh_build_indexed(v, nv, idx, ni, atlas, o == NULL || o->lods > 1, out);
    t_lod_opts = NULL;
    return ok;
}

bool mesh_skinned_lods_opts(MeshBuilt* mb, const MeshLodOptions* o) {
    if (mb && mb->lods.count > 1u) mb->ni = mb->lods.first_index[0] + mb->lods.index_count[0], mb->lods.count = 1u; // (its LODs made again)
    t_lod_opts = o;
    bool ok = mesh_skinned_lods(mb);
    t_lod_opts = NULL;
    return ok;
}

bool rmesh_write(const char* path, const MeshBuilt* m) {
    RMeshHeader h = { RMESH_MAGIC, RMESH_VERSION, m->nv, m->ni, { m->mn[0], m->mn[1], m->mn[2] }, { m->mx[0], m->mx[1], m->mx[2] }, m->area, m->flags };
    const MeshMorphs* mm = &m->morph;
    bool morph = mm->count && mm->nd && mm->nv == m->nv && mm->first && mm->d;
    if (morph) h.flags |= RMESH_MORPHS;
    else h.flags &= ~RMESH_MORPHS;
    if (m->col) h.flags |= RMESH_COLORS;
    else h.flags &= ~RMESH_COLORS;
    FILE* f = re_fopen(path, "wb");
    bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&m->lods, sizeof m->lods, 1, f) == 1 &&
              fwrite(m->v, sizeof(GpuVertex), m->nv, f) == m->nv && fwrite(m->idx, 4, m->ni, f) == m->ni;
    if (ok && morph) { // (v3's block)
        uint32_t c[2] = { mm->count, mm->nd };
        ok = fwrite(c, 4, 2, f) == 2 && fwrite(mm->name, sizeof *mm->name, mm->count, f) == mm->count &&
             fwrite(mm->weight, 4, mm->count, f) == mm->count && fwrite(mm->first, 4, (size_t)m->nv + 1u, f) == (size_t)m->nv + 1u &&
             fwrite(mm->d, sizeof(RMeshMorphDelta), mm->nd, f) == mm->nd;
    }
    if (ok && m->col) ok = fwrite(m->col, 4, m->nv, f) == m->nv; // (v4's block: the vertex colors)
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

bool rmesh_read(const char* path, MeshBuilt* out, uint32_t max_verts, uint32_t max_indices) {
    memset(out, 0, sizeof *out);
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    RMeshHeader h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == RMESH_MAGIC && h.version >= 1u && h.version <= RMESH_VERSION && h.nverts &&
              h.nindices && h.nverts <= max_verts && h.nindices <= max_indices;
    RMeshLods lods = { 1, { 0 }, { h.nindices } };
    if (ok && h.version >= 2u) ok = fread(&lods, sizeof lods, 1, f) == 1 && lods.count >= 1 && lods.count <= RMESH_MAX_LODS;
    GpuVertex* v = ok ? malloc(sizeof(GpuVertex) * (size_t)h.nverts) : NULL;
    uint32_t* idx = ok ? malloc(sizeof(uint32_t) * (size_t)h.nindices) : NULL;
    ok = ok && v && idx && fread(v, sizeof(GpuVertex), h.nverts, f) == h.nverts && fread(idx, 4, h.nindices, f) == h.nindices;
    MeshMorphs mm = { 0 };
    if (ok && h.version >= 3u && (h.flags & RMESH_MORPHS)) { // the morph targets (a damaged block: the mesh without them)
        uint32_t c[2] = { 0, 0 };
        bool mo = fread(c, 4, 2, f) == 2 && c[0] && c[0] <= RMESH_MAX_MORPHS && c[1] && (uint64_t)c[1] <= 64ull * h.nverts;
        if (mo) {
            mm.count = c[0], mm.nd = c[1], mm.nv = h.nverts;
            mm.name = malloc(sizeof *mm.name * mm.count), mm.weight = malloc(4u * mm.count);
            mm.first = malloc(4u * ((size_t)h.nverts + 1u)), mm.d = malloc(sizeof(RMeshMorphDelta) * mm.nd);
            mo = mm.name && mm.weight && mm.first && mm.d && fread(mm.name, sizeof *mm.name, mm.count, f) == mm.count &&
                 fread(mm.weight, 4, mm.count, f) == mm.count && fread(mm.first, 4, (size_t)h.nverts + 1u, f) == (size_t)h.nverts + 1u &&
                 fread(mm.d, sizeof(RMeshMorphDelta), mm.nd, f) == mm.nd && mm.first[0] == 0u && mm.first[h.nverts] == mm.nd;
            for (uint32_t i = 0; mo && i < h.nverts; i++) mo = mm.first[i] <= mm.first[i + 1];
            for (uint32_t k = 0; mo && k < mm.nd; k++) mo = mm.d[k].target < mm.count;
            for (uint32_t k = 0; mo && k < mm.count; k++) mm.name[k][RMESH_MORPH_NAME - 1] = 0;
        }
        if (!mo) mesh_morphs_free(&mm);
    }
    uint32_t* col = NULL;
    if (ok && h.version >= 4u && (h.flags & RMESH_COLORS)) { // the vertex colors (a damaged block: the mesh without them)
        col = malloc(4u * (size_t)h.nverts);
        if (col && fread(col, 4, h.nverts, f) != h.nverts) free(col), col = NULL;
    }
    fclose(f);
    for (uint32_t k = 0; ok && k < h.nindices; k++)
        if (idx[k] >= h.nverts) idx[k] = 0; // (a damaged file draws a fan, not a crash)
    for (uint32_t l = 0; ok && l < lods.count; l++)
        ok = lods.first_index[l] + lods.index_count[l] <= h.nindices && lods.index_count[l] % 3u == 0u && lods.index_count[l];
    if (!ok) {
        free(v), free(idx), free(col);
        mesh_morphs_free(&mm);
        return false;
    }
    if (h.version >= 2u) {
        out->v = v, out->nv = h.nverts, out->idx = idx, out->ni = h.nindices, out->lods = lods, out->flags = h.flags & ~(RMESH_MORPHS | RMESH_COLORS);
        if (mm.count) out->morph = mm, out->flags |= RMESH_MORPHS;
        if (col) out->col = col, out->flags |= RMESH_COLORS;
        memcpy(out->mn, h.mn, 12), memcpy(out->mx, h.mx, 12);
        out->area = h.area;
        return true;
    }
    // v1: processed once per content (the result kept in the cache folder, %LOCALAPPDATA%: the project's files are left as
    // they are, so an older version still opens them; 124 meshes took 1.4 s at every start)
    uint64_t key = 1469598103934665603ull;
    const uint8_t* bytes[3] = { (const uint8_t*)&h, (const uint8_t*)v, (const uint8_t*)idx };
    size_t sizes[3] = { sizeof h, sizeof(GpuVertex) * (size_t)h.nverts, sizeof(uint32_t) * (size_t)h.nindices };
    for (int s = 0; s < 3; s++)
        for (size_t q = 0; q < sizes[s]; q++) key = (key ^ bytes[s][q]) * 1099511628211ull;
    char cache[600] = "";
    if (re_cache_dir(cache, sizeof cache)) {
        size_t l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/meshes");
        re_make_dirs(cache);
        l = strlen(cache);
        // (v2's name: v3 without morphs is v2's layout, the caches made before stay good)
        snprintf(cache + l, sizeof cache - l, "/%016llx_v2.rmesh", (unsigned long long)key);
        FILE* cf = re_fopen(cache, "rb");
        if (cf) {
            fclose(cf);
            if (rmesh_read(cache, out, max_verts, max_indices) && out->lods.count >= 1) {
                free(v), free(idx);
                return true;
            }
        }
    }
    // the importers' corners (indexed 0..n in order: the landscape's blocks are indexed for real, kept as they are)
    bool corners = h.nverts == h.nindices;
    for (uint32_t k = 0; corners && k < h.nindices; k++) corners = idx[k] == k;
    bool built = corners ? mesh_build_corners(v, h.nindices / 3u, true, out) : mesh_build_indexed(v, h.nverts, idx, h.nindices, true, false, out);
    if (built && h.area > 0.0f) out->area = h.area;
    if (built && cache[0]) rmesh_write(cache, out); // (a failed write: processed again next time)
    free(v), free(idx);
    return built;
}

typedef struct ClusterKey { float d; uint32_t m; } ClusterKey;
static int by_cluster_key(const void* a, const void* b) {
    float x = ((const ClusterKey*)a)->d, y = ((const ClusterKey*)b)->d;
    return x < y ? -1 : x > y ? 1 : (int)((const ClusterKey*)a)->m - (int)((const ClusterKey*)b)->m;
}

uint32_t mesh_clusters(const GpuVertex* v, uint32_t nv, uint32_t* idx, uint32_t ni, MeshCluster* out, uint32_t cap, uint32_t* perm) {
    size_t maxm = meshopt_buildMeshletsBound(ni, 128, 128);
    struct meshopt_Meshlet* ml = malloc(sizeof *ml * maxm);
    unsigned int* mv = malloc(sizeof(unsigned int) * maxm * 128); // (meshopt's sizes: the bound times the limits)
    unsigned char* mt = malloc(maxm * 128 * 3);
    uint32_t* re = malloc(sizeof(uint32_t) * (size_t)ni);
    uint32_t n = 0;
    if (ml && mv && mt && re) {
        size_t cnt = meshopt_buildMeshlets(ml, mv, mt, idx, ni, v[0].pos, nv, sizeof(GpuVertex), 128, 128, 0.25f);
        ClusterKey* key = cnt <= cap ? malloc(sizeof *key * cnt) : NULL;
        if (key) {
            // the outer clusters first (their distance from the mesh's middle, the farthest first): the order the
            // overdraw optimizer gave the triangles, kept by clusters. A model seen from outside draws its outer layers
            // first and the alpha-tested prepass rejects the inner ones by depth (cards dozens deep: 27 ms -> 17 ms
            // main in the heavy scene against the meshlets' own order)
            double mid[3] = { 0, 0, 0 }, wsum = 0;
            for (size_t m = 0; m < cnt; m++) {
                struct meshopt_Bounds bd = meshopt_computeMeshletBounds(mv + ml[m].vertex_offset, mt + ml[m].triangle_offset, ml[m].triangle_count,
                                                                         v[0].pos, nv, sizeof(GpuVertex));
                MeshCluster* c = &out[m];
                memcpy(c->sphere, bd.center, 12), c->sphere[3] = bd.radius;
                memcpy(c->cone, bd.cone_axis, 12), c->cone[3] = bd.cone_cutoff;
                for (int k = 0; k < 3; k++) mid[k] += (double)bd.center[k] * ml[m].triangle_count;
                wsum += ml[m].triangle_count;
            }
            for (size_t m = 0; m < cnt; m++) {
                float d[3];
                for (int k = 0; k < 3; k++) d[k] = out[m].sphere[k] - (float)(mid[k] / (wsum > 0 ? wsum : 1));
                key[m].d = -(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), key[m].m = (uint32_t)m;
            }
            qsort(key, cnt, sizeof *key, by_cluster_key);
            MeshCluster* tmp = malloc(sizeof *tmp * cnt);
            if (tmp) {
                memcpy(tmp, out, sizeof *tmp * cnt);
                uint32_t at = 0;
                for (size_t q = 0; q < cnt; q++) {
                    uint32_t m = key[q].m;
                    MeshCluster* c = &out[n++];
                    *c = tmp[m];
                    c->first = at, c->count = ml[m].triangle_count * 3u;
                    for (uint32_t t = 0; t < ml[m].triangle_count * 3u; t++) re[at++] = mv[ml[m].vertex_offset + mt[ml[m].triangle_offset + t]];
                }
                free(tmp);
                if (at != ni) n = 0;
            }
            free(key);
        }
        // the view orders: by the center along each axis (seen along +X, the smallest x first): the cull picks the one
        // nearest the camera's direction per instance, the alpha-tested prepass then draws front to back
        ClusterKey* k6 = n && perm ? malloc(sizeof *k6 * n) : NULL;
        for (int o = 0; k6 && o < 6; o++) {
            for (uint32_t m = 0; m < n; m++) k6[m].d = out[m].sphere[o >> 1] * ((o & 1) ? -1.0f : 1.0f), k6[m].m = m;
            qsort(k6, n, sizeof *k6, by_cluster_key);
            for (uint32_t m = 0; m < n; m++) perm[(uint32_t)o * n + m] = k6[m].m;
        }
        if (perm && n && !k6) n = 0;
        free(k6);
        uint32_t at = n ? ni : 0;
        if (n && at == ni) memcpy(idx, re, sizeof(uint32_t) * (size_t)ni);
        else n = 0; // (out of room: the mesh keeps its plain draws)
    }
    free(ml), free(mv), free(mt), free(re);
    return n;
}

#define CL_CACHE_MAGIC 0x31434C52u // "RCL1"
uint32_t mesh_built_clusters(MeshBuilt* mb, MeshCluster** out, uint32_t** perm, uint32_t first[RMESH_MAX_LODS], uint32_t count[RMESH_MAX_LODS]) {
    *out = NULL, *perm = NULL;
    memset(first, 0, sizeof(uint32_t) * RMESH_MAX_LODS), memset(count, 0, sizeof(uint32_t) * RMESH_MAX_LODS);
    // the content's key (8 bytes at a time: 45 MB in ~10 ms)
    uint64_t key = 1469598103934665603ull ^ CL_CACHE_MAGIC;
    const uint8_t* bytes[3] = { (const uint8_t*)mb->v, (const uint8_t*)mb->idx, (const uint8_t*)&mb->lods };
    size_t sizes[3] = { sizeof(GpuVertex) * (size_t)mb->nv, sizeof(uint32_t) * (size_t)mb->ni, sizeof mb->lods };
    for (int s = 0; s < 3; s++) {
        size_t q = 0;
        for (; q + 8 <= sizes[s]; q += 8) {
            uint64_t w;
            memcpy(&w, bytes[s] + q, 8);
            key = (key ^ w) * 1099511628211ull;
            key ^= key >> 29;
        }
        for (; q < sizes[s]; q++) key = (key ^ bytes[s][q]) * 1099511628211ull;
    }
    char cache[600] = "";
    if (re_cache_dir(cache, sizeof cache)) {
        size_t l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/meshes");
        re_make_dirs(cache);
        l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/%016llx_cl1.bin", (unsigned long long)key);
        FILE* f = re_fopen(cache, "rb");
        if (f) { // magic, clusters, indices; per LOD first and count; the clusters, the orders, the indices
            uint32_t h[3 + 2 * RMESH_MAX_LODS];
            bool ok = fread(h, sizeof h, 1, f) == 1 && h[0] == CL_CACHE_MAGIC && h[2] == mb->ni && h[1] && h[1] <= mb->ni;
            MeshCluster* c = ok ? malloc(sizeof *c * h[1]) : NULL;
            uint32_t* p = ok ? malloc(sizeof *p * 6 * h[1]) : NULL;
            uint32_t* ix = ok ? malloc(sizeof *ix * mb->ni) : NULL;
            ok = ok && c && p && ix && fread(c, sizeof *c, h[1], f) == h[1] && fread(p, 4, 6 * (size_t)h[1], f) == 6 * (size_t)h[1] &&
                 fread(ix, 4, mb->ni, f) == mb->ni;
            for (uint32_t k = 0; ok && k < mb->ni; k++) ok = ix[k] < mb->nv;
            for (uint32_t k = 0; ok && k < h[1]; k++) ok = c[k].first + c[k].count <= mb->ni;
            fclose(f);
            if (ok) {
                memcpy(mb->idx, ix, sizeof *ix * mb->ni);
                free(ix);
                memcpy(first, h + 3, sizeof(uint32_t) * RMESH_MAX_LODS), memcpy(count, h + 3 + RMESH_MAX_LODS, sizeof(uint32_t) * RMESH_MAX_LODS);
                *out = c, *perm = p;
                return h[1];
            }
            free(c), free(p), free(ix);
        }
    }
    uint32_t n = 0, cap = 0;
    MeshCluster* all = NULL;
    uint32_t* allp = NULL;
    for (uint32_t l = 0; l < mb->lods.count && l < RMESH_MAX_LODS; l++) {
        uint32_t ni = mb->lods.index_count[l], want = ni / 3u / 16u + 64u; // (128 triangles at most; cards: 64 vertices, 32)
        if (n + want > cap) {
            cap = (n + want) * 2u;
            MeshCluster* a = realloc(all, sizeof *a * cap);
            if (a) all = a;
            uint32_t* pp = a ? realloc(allp, sizeof *pp * 6 * cap) : NULL;
            if (!pp) break;
            allp = pp;
        }
        uint32_t k = mesh_clusters(mb->v, mb->nv, mb->idx + mb->lods.first_index[l], ni, all + n, want, allp + (size_t)n * 6);
        first[l] = n, count[l] = k;
        for (uint32_t c = 0; c < k; c++) all[n + c].first += mb->lods.first_index[l];
        n += k;
    }
    if (!n) {
        free(all), free(allp);
        return 0;
    }
    if (cache[0]) {
        FILE* f = re_fopen(cache, "wb");
        if (f) {
            uint32_t h[3 + 2 * RMESH_MAX_LODS] = { CL_CACHE_MAGIC, n, mb->ni };
            memcpy(h + 3, first, sizeof(uint32_t) * RMESH_MAX_LODS), memcpy(h + 3 + RMESH_MAX_LODS, count, sizeof(uint32_t) * RMESH_MAX_LODS);
            bool ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(all, sizeof *all, n, f) == n && fwrite(allp, 4, 6 * (size_t)n, f) == 6 * (size_t)n &&
                      fwrite(mb->idx, 4, mb->ni, f) == mb->ni;
            fclose(f);
            if (!ok) remove(cache); // (a half file: made again next time)
        }
    }
    *out = all, *perm = allp;
    return n;
}

// the 6 view orders of n spheres (mesh_clusters' rule: by the center along +X, -X, +Y, -Y, +Z, -Z), perm[o * n + k]
static bool view_orders(const float* spheres, uint32_t stride_floats, uint32_t n, uint32_t* perm) {
    ClusterKey* k6 = malloc(sizeof *k6 * (n ? n : 1));
    if (!k6) return false;
    for (int o = 0; o < 6; o++) {
        for (uint32_t m = 0; m < n; m++) k6[m].d = spheres[(size_t)m * stride_floats + (o >> 1)] * ((o & 1) ? -1.0f : 1.0f), k6[m].m = m;
        qsort(k6, n, sizeof *k6, by_cluster_key);
        for (uint32_t m = 0; m < n; m++) perm[(uint32_t)o * n + m] = k6[m].m;
    }
    free(k6);
    return true;
}

#define DAG_CACHE_MAGIC 0x32474452u // "RDG2" (2: each cluster's own sphere)
uint32_t mesh_built_dag(const MeshBuilt* mb, MeshDagCluster** out, uint32_t** perm, uint32_t** idx, uint32_t* nidx) {
    *out = NULL, *perm = NULL, *idx = NULL, *nidx = 0;
    if (!mb->lods.count) return 0;
    const uint32_t* src = mb->idx + mb->lods.first_index[0];
    uint32_t ni = mb->lods.index_count[0];
    // (2026-11-25, item 4) a skin or a mesh of many shells (the Unamed capsule: 115, 52 under 64 triangles): a group stuck
    // under regular simplification tries the permissive one, then the sloppy one (else its DAG kept every triangle)
    uint32_t cflags = (mb->flags & RMESH_SKINNED) || mesh_islands(mb, 0u, NULL) > 32u ? MESH_CLOD_STUCK_FALLBACK : 0u;
    // the content's key: the vertices and LOD 0's indices (as mesh_built_clusters), and how it is simplified
    uint64_t key = (1469598103934665603ull ^ DAG_CACHE_MAGIC) + (uint64_t)cflags * 0x9E3779B97F4A7C15ull;
    const uint8_t* bytes[2] = { (const uint8_t*)mb->v, (const uint8_t*)src };
    size_t sizes[2] = { sizeof(GpuVertex) * (size_t)mb->nv, sizeof(uint32_t) * (size_t)ni };
    for (int s = 0; s < 2; s++) {
        size_t q = 0;
        for (; q + 8 <= sizes[s]; q += 8) {
            uint64_t w;
            memcpy(&w, bytes[s] + q, 8);
            key = (key ^ w) * 1099511628211ull;
            key ^= key >> 29;
        }
        for (; q < sizes[s]; q++) key = (key ^ bytes[s][q]) * 1099511628211ull;
    }
    char cache[600] = "";
    if (re_cache_dir(cache, sizeof cache)) {
        size_t l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/meshes");
        re_make_dirs(cache);
        l = strlen(cache);
        snprintf(cache + l, sizeof cache - l, "/%016llx_dag1.bin", (unsigned long long)key);
        FILE* f = re_fopen(cache, "rb");
        if (f) { // magic, clusters, indices; the clusters, the orders, the indices
            uint32_t h[3];
            bool ok = fread(h, sizeof h, 1, f) == 1 && h[0] == DAG_CACHE_MAGIC && h[1] && h[2] && h[2] <= ni * 4u;
            MeshDagCluster* c = ok ? malloc(sizeof *c * h[1]) : NULL;
            uint32_t* p = ok ? malloc(sizeof *p * 6 * h[1]) : NULL;
            uint32_t* ix = ok ? malloc(sizeof *ix * h[2]) : NULL;
            ok = ok && c && p && ix && fread(c, sizeof *c, h[1], f) == h[1] && fread(p, 4, 6 * (size_t)h[1], f) == 6 * (size_t)h[1] &&
                 fread(ix, 4, h[2], f) == h[2];
            for (uint32_t k = 0; ok && k < h[2]; k++) ok = ix[k] < mb->nv;
            for (uint32_t k = 0; ok && k < h[1]; k++) ok = c[k].first + c[k].count <= h[2];
            fclose(f);
            if (ok) {
                *out = c, *perm = p, *idx = ix, *nidx = h[2];
                return h[1];
            }
            free(c), free(p), free(ix);
        }
    }
    // the attributes the simplification weighs (as the LODs': the normal, the uv; the uv's seams kept)
    float* attr = malloc(sizeof(float) * 5 * (size_t)mb->nv);
    if (!attr) return 0;
    for (uint32_t i = 0; i < mb->nv; i++) {
        oct_decode(mb->v[i].normal_oct, &attr[i * 5]);
        attr[i * 5 + 3] = mb->v[i].uv[0], attr[i * 5 + 4] = mb->v[i].uv[1];
    }
    static const float weights[5] = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };
    MeshDagCluster* c;
    uint32_t *ix, nix, levels;
    double t0 = re_time_seconds();
    uint32_t n = mesh_clod_build(mb->v[0].pos, sizeof(GpuVertex), mb->nv, attr, 5, weights, (1u << 3) | (1u << 4), src, ni, &c, &ix, &nix, &levels, cflags);
    free(attr);
    uint32_t* p = n ? malloc(sizeof *p * 6 * n) : NULL;
    if (!p || !view_orders(c[0].sphere, sizeof *c / sizeof(float), n, p)) {
        free(c), free(ix), free(p);
        return 0;
    }
    printf("[RealEngine] Cluster LOD: %u triangles -> %u clusters in %u levels (%u triangles in all), %.0f ms\n", ni / 3u, n, levels, nix / 3u,
           (re_time_seconds() - t0) * 1000.0);
    if (cache[0]) {
        FILE* f = re_fopen(cache, "wb");
        if (f) {
            uint32_t h[3] = { DAG_CACHE_MAGIC, n, nix };
            bool ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(c, sizeof *c, n, f) == n && fwrite(p, 4, 6 * (size_t)n, f) == 6 * (size_t)n &&
                      fwrite(ix, 4, nix, f) == nix;
            fclose(f);
            if (!ok) remove(cache);
        }
    }
    *out = c, *perm = p, *idx = ix, *nidx = nix;
    return n;
}
