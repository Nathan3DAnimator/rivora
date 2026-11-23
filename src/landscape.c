// Landscape (landscape.h, notas-dev/landscape.md): the heights (noise or a heightmap image), their chunk meshes, the ground queries,
// the sculpting brushes
#include "landscape.h"
#include "procgen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "render_gpu.h"

#define LAND_MAX 4
#define RLAND_MAGIC 0x444E4C52u // "RLND"
#define PLANT_NODE LANDSCAPE_PLANT_NODE // (an older level's plant: CAttach.node this + its index; a chunk's: its index)

typedef struct Land {
    Entity e;
    char name[32];
    uint32_t res, chunks;
    float size, origin[3];
    float* h;              // (res + 1)^2, cm above the actor
    uint8_t* wt;           // (B11) (res + 1)^2 x 3: the painted layers' weights (0-255), NULL: none painted
    struct { float* h; uint8_t* wt; } undo[8]; // (B11) the strokes' befores (a ring)
    uint32_t nundo;
    uint64_t chunk_dirty[4]; // (up to 16 x 16 chunks) to write again
    uint32_t mesh_hash;    // the chunk meshes' parameters when they were made
    uint32_t edits;        // bumped by a committed stroke or an import
    float flat_z;          // Flatten: the stroke's height
    bool stroking, alive, need_children, save;
    double last_commit;
    float* tmm;            // (2026-11-19) landscape_raycast's tiles: lowest, highest height of each LAND_TILE^2 cells
    uint32_t tmm_key, tmm_n; // (the heights' key it was made for; tiles a side)
} Land;
#define LAND_TILE 16

static struct {
    char root[600];
    Land land[LAND_MAX];
    uint32_t nland;
    LandStats stats;
    // a heightmap asked for (landscape_import_heightmap): applied once its landscape's grid is ready (a new size first)
    struct { Entity e; bool add; char path[600]; } pending;
} L;

static int g_paint_layer = 1;
void landscape_set_paint_layer(int layer) { g_paint_layer = layer < 1 ? 1 : layer > 3 ? 3 : layer; }
static void undo_free(Land* l) {
    for (int k = 0; k < 8; k++) free(l->undo[k].h), free(l->undo[k].wt), l->undo[k].h = NULL, l->undo[k].wt = NULL;
    l->nundo = 0;
}

void landscape_init(const char* content_root) {
    for (uint32_t i = 0; i < L.nland; i++) free(L.land[i].h), free(L.land[i].wt), free(L.land[i].tmm), undo_free(&L.land[i]);
    memset(&L, 0, sizeof L);
    snprintf(L.root, sizeof L.root, "%s", content_root ? content_root : "");
}

const LandStats* landscape_stats(void) { return &L.stats; }

// --- Noise: value noise in fBm, ridged in its higher octaves (mountains), a flatter middle (where the player starts) ---

static float hash2(int x, int y, uint32_t s) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
}
static float vnoise(float x, float y, uint32_t s) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3.0f - 2.0f * fx), fy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash2(ix, iy, s), b = hash2(ix + 1, iy, s), c = hash2(ix, iy + 1, s), d = hash2(ix + 1, iy + 1, s);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static void generate(Land* l, const CLandscape* c) {
    uint32_t n = l->res + 1;
    float pers = 0.35f + 0.3f * fminf(fmaxf(c->roughness, 0.0f), 1.0f);
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < n; x++) {
            float u = (float)x / (float)l->res, v = (float)y / (float)l->res, sum = 0.0f, amp = 1.0f, norm = 0.0f, f = 3.0f;
            for (int o = 0; o < 6; o++) {
                float nz = vnoise(u * f, v * f, c->seed + (uint32_t)o * 101u);
                if (o >= 2) nz = 1.0f - fabsf(nz * 2.0f - 1.0f); // ridges
                sum += nz * amp, norm += amp;
                amp *= pers, f *= 2.0f;
            }
            float du = u - 0.5f, dv = v - 0.5f, r = sqrtf(du * du + dv * dv) / 0.18f; // (a gentle middle: 18% of the size)
            float mid = r < 1.0f ? r * r * (3.0f - 2.0f * r) : 1.0f;
            float hv = (sum / norm - 0.35f) * c->height;
            l->h[y * n + x] = hv * (0.15f + 0.85f * mid);
        }
    for (int k = 0; k < 4; k++) l->chunk_dirty[k] = ~0ull;
    l->save = true, l->edits++;
}

static void path_of(const Land* l, const char* suffix, char* out, size_t cap) { snprintf(out, cap, "%s/Landscapes/%s%s", L.root, l->name, suffix); }

static bool load(Land* l) {
    char p[800];
    path_of(l, ".rland", p, sizeof p);
    FILE* f = re_fopen(p, "rb");
    if (!f) return false;
    uint32_t hd[3] = { 0 };
    float size = 0.0f;
    bool ok = fread(hd, 4, 3, f) == 3 && fread(&size, 4, 1, f) == 1 && hd[0] == RLAND_MAGIC && hd[1] == 1u && hd[2] == l->res;
    size_t n = (size_t)(l->res + 1) * (l->res + 1);
    ok = ok && fread(l->h, sizeof(float), n, f) == n;
    fclose(f);
    if (ok) { // (B11) the painted layers (<name>.rlw: their weights, 3 bytes a point; none: not painted)
        path_of(l, ".rlw", p, sizeof p);
        FILE* g = re_fopen(p, "rb");
        if (g) {
            uint8_t* wt = malloc(n * 3);
            if (wt && fread(wt, 1, n * 3, g) == n * 3) free(l->wt), l->wt = wt;
            else free(wt);
            fclose(g);
        }
    }
    return ok;
}

static void save(Land* l) {
    char dir[700], p[800];
    snprintf(dir, sizeof dir, "%s/Landscapes", L.root);
    re_make_dirs(dir);
    path_of(l, ".rland", p, sizeof p);
    FILE* f = re_fopen(p, "wb");
    if (!f) return;
    uint32_t hd[3] = { RLAND_MAGIC, 1u, l->res };
    fwrite(hd, 4, 3, f), fwrite(&l->size, 4, 1, f);
    fwrite(l->h, sizeof(float), (size_t)(l->res + 1) * (l->res + 1), f);
    fclose(f);
    if (l->wt) { // (B11)
        path_of(l, ".rlw", p, sizeof p);
        FILE* g = re_fopen(p, "wb");
        if (g) fwrite(l->wt, 1, (size_t)(l->res + 1) * (l->res + 1) * 3, g), fclose(g);
    }
    l->save = false;
}

// --- The ground ---

static float grid_h(const Land* l, int x, int y) {
    int n = (int)l->res + 1;
    x = x < 0 ? 0 : x >= n ? n - 1 : x, y = y < 0 ? 0 : y >= n ? n - 1 : y;
    return l->h[y * n + x];
}

static bool land_at(const Land* l, float wx, float wy, float* z) {
    float gx = (wx - l->origin[0] + l->size * 0.5f) / l->size * (float)l->res, gy = (wy - l->origin[1] + l->size * 0.5f) / l->size * (float)l->res;
    if (gx < 0.0f || gy < 0.0f || gx > (float)l->res || gy > (float)l->res) return false;
    int ix = (int)gx, iy = (int)gy;
    float fx = gx - (float)ix, fy = gy - (float)iy;
    // (the triangle the mesh draws there: the quad split along its diagonal)
    float h00 = grid_h(l, ix, iy), h10 = grid_h(l, ix + 1, iy), h01 = grid_h(l, ix, iy + 1), h11 = grid_h(l, ix + 1, iy + 1);
    float h = fx + fy <= 1.0f ? h00 + (h10 - h00) * fx + (h01 - h00) * fy : h11 + (h01 - h11) * (1.0f - fx) + (h10 - h11) * (1.0f - fy);
    *z = l->origin[2] + h;
    return true;
}

bool landscape_height(float x, float y, float* z) {
    bool any = procgen_height(x, y, z); // (a Procedural World's ground first: the landscapes over it win where higher)
    for (uint32_t i = 0; i < L.nland; i++) {
        float hz;
        if (L.land[i].alive && L.land[i].h && land_at(&L.land[i], x, y, &hz) && (!any || hz > *z)) *z = hz, any = true;
    }
    return any;
}

float landscape_trace(const float a[3], const float b[3]) {
    float pz;
    if (!L.nland && !procgen_height(a[0], a[1], &pz)) return 1.0f;
    float z;
    // marched (64 steps), then the crossing found by halving
    float prev = 0.0f;
    bool below0 = landscape_height(a[0], a[1], &z) && a[2] < z;
    if (below0) return 1.0f; // (starting under the ground: as the boxes do with a start inside)
    for (int s = 1; s <= 64; s++) {
        float t = (float)s / 64.0f, p[3] = { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
        if (landscape_height(p[0], p[1], &z) && p[2] < z) {
            float lo = prev, hi = t;
            for (int k = 0; k < 10; k++) {
                float m = (lo + hi) * 0.5f, q[3] = { a[0] + (b[0] - a[0]) * m, a[1] + (b[1] - a[1]) * m, a[2] + (b[2] - a[2]) * m };
                if (landscape_height(q[0], q[1], &z) && q[2] < z) hi = m;
                else lo = m;
            }
            return lo;
        }
        prev = t;
    }
    return 1.0f;
}

// --- (2026-11-19) The scene queries' exact ray (scene_query.c): the tiles' bounds, then the cells' two triangles ---

void landscape_query_prepare(void) {
    for (uint32_t i = 0; i < L.nland; i++) {
        Land* l = &L.land[i];
        if (!l->alive || !l->h || !l->res) continue;
        uint32_t key = l->edits * 2654435761u ^ l->res * 40503u ^ (uint32_t)(uintptr_t)l->h;
        uint32_t nt = (l->res + LAND_TILE - 1) / LAND_TILE;
        if (l->tmm && l->tmm_key == key && l->tmm_n == nt) continue;
        float* t = realloc(l->tmm, sizeof(float) * 2 * nt * nt); // (a sculpt or an import: once, not a frame's)
        if (!t) continue;
        l->tmm = t, l->tmm_n = nt, l->tmm_key = key;
        for (uint32_t ty = 0; ty < nt; ty++)
            for (uint32_t tx = 0; tx < nt; tx++) {
                float lo = 1e30f, hi = -1e30f;
                for (uint32_t y = ty * LAND_TILE; y <= (ty + 1) * LAND_TILE && y <= l->res; y++)
                    for (uint32_t x = tx * LAND_TILE; x <= (tx + 1) * LAND_TILE && x <= l->res; x++) {
                        float h = l->h[y * (l->res + 1) + x];
                        lo = fminf(lo, h), hi = fmaxf(hi, h);
                    }
                t[(ty * nt + tx) * 2] = lo, t[(ty * nt + tx) * 2 + 1] = hi;
            }
    }
}

// a + d t against a triangle (Moller-Trumbore, both sides): its t in (0, best), else best
static float ray_tri(const float a[3], const float d[3], const float p0[3], const float p1[3], const float p2[3], float best, float n[3]) {
    float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
    float pv[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (fabsf(det) < 1e-12f) return best;
    float inv = 1.0f / det, tv[3] = { a[0] - p0[0], a[1] - p0[1], a[2] - p0[2] };
    float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < 0.0f || u > 1.0f) return best;
    float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
    float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return best;
    float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    if (t < 0.0f || t >= best) return best;
    float c[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (cl > 0.0f) {
        if (c[2] < 0.0f) cl = -cl; // (up: the ground's side)
        n[0] = c[0] / cl, n[1] = c[1] / cl, n[2] = c[2] / cl;
    }
    return t;
}

// One landscape: the segment in its grid (x, y in cells; z in cm above its actor), walked tile by tile (Amanatides-Woo),
// each tile whose height range the segment's crosses walked cell by cell
static float land_ray(const Land* l, const float a[3], const float b[3], float best, float n[3]) {
    float cs = l->size / (float)l->res, half = l->size * 0.5f;
    float o[3] = { (a[0] - l->origin[0] + half) / cs, (a[1] - l->origin[1] + half) / cs, a[2] - l->origin[2] };
    float d[3] = { (b[0] - a[0]) / cs, (b[1] - a[1]) / cs, b[2] - a[2] };
    // clipped to the grid's square and to the heights' range
    float t0 = 0.0f, t1 = best, lo[3] = { 0.0f, 0.0f, -1e30f }, hi[3] = { (float)l->res, (float)l->res, 1e30f };
    {
        float zl = 1e30f, zh = -1e30f;
        for (uint32_t k = 0; k < l->tmm_n * l->tmm_n; k++) zl = fminf(zl, l->tmm[k * 2]), zh = fmaxf(zh, l->tmm[k * 2 + 1]);
        lo[2] = zl, hi[2] = zh;
    }
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return best;
            continue;
        }
        float ta = (lo[k] - o[k]) / d[k], tb = (hi[k] - o[k]) / d[k];
        if (ta > tb) { float s = ta; ta = tb, tb = s; }
        t0 = fmaxf(t0, ta), t1 = fminf(t1, tb);
        if (t0 > t1) return best;
    }
    const int T = LAND_TILE, nt = (int)l->tmm_n, res = (int)l->res;
    float p[2] = { o[0] + d[0] * t0, o[1] + d[1] * t0 };
    int tx = (int)fminf(fmaxf(floorf(p[0] / T), 0.0f), (float)(nt - 1)), ty = (int)fminf(fmaxf(floorf(p[1] / T), 0.0f), (float)(nt - 1));
    int sx = d[0] > 0.0f ? 1 : -1, sy = d[1] > 0.0f ? 1 : -1;
    float dtx = fabsf(d[0]) > 1e-12f ? (float)T / fabsf(d[0]) : 1e30f, dty = fabsf(d[1]) > 1e-12f ? (float)T / fabsf(d[1]) : 1e30f;
    float ntx = fabsf(d[0]) > 1e-12f ? (((float)(tx + (sx > 0)) * T) - o[0]) / d[0] : 1e30f;
    float nty = fabsf(d[1]) > 1e-12f ? (((float)(ty + (sy > 0)) * T) - o[1]) / d[1] : 1e30f;
    float te = t0;
    for (int guard = 0; guard < 4 * nt + 8 && te <= t1 && te < best; guard++) {
        float tx_exit = fminf(fminf(ntx, nty), t1);
        // the segment's heights over this tile against the tile's
        float za = o[2] + d[2] * te, zb = o[2] + d[2] * tx_exit;
        const float* mm = &l->tmm[(ty * nt + tx) * 2];
        if (fminf(za, zb) <= mm[1] && fmaxf(za, zb) >= mm[0]) {
            // the cells inside the tile along [te, tx_exit]
            float q[2] = { o[0] + d[0] * te, o[1] + d[1] * te };
            int cx = (int)floorf(q[0]), cy = (int)floorf(q[1]);
            cx = cx < tx * T ? tx * T : cx >= (tx + 1) * T ? (tx + 1) * T - 1 : cx;
            cy = cy < ty * T ? ty * T : cy >= (ty + 1) * T ? (ty + 1) * T - 1 : cy;
            if (cx >= res) cx = res - 1;
            if (cy >= res) cy = res - 1;
            float dcx = fabsf(d[0]) > 1e-12f ? 1.0f / fabsf(d[0]) : 1e30f, dcy = fabsf(d[1]) > 1e-12f ? 1.0f / fabsf(d[1]) : 1e30f;
            float ncx = fabsf(d[0]) > 1e-12f ? ((float)(cx + (sx > 0)) - o[0]) / d[0] : 1e30f;
            float ncy = fabsf(d[1]) > 1e-12f ? ((float)(cy + (sy > 0)) - o[1]) / d[1] : 1e30f;
            for (int g2 = 0; g2 < 4 * T + 4; g2++) {
                if (cx >= 0 && cy >= 0 && cx < res && cy < res) {
                    int w1 = res + 1;
                    float h00 = l->h[cy * w1 + cx], h10 = l->h[cy * w1 + cx + 1], h01 = l->h[(cy + 1) * w1 + cx], h11 = l->h[(cy + 1) * w1 + cx + 1];
                    float v00[3] = { (float)cx, (float)cy, h00 }, v10[3] = { (float)cx + 1, (float)cy, h10 }, v01[3] = { (float)cx, (float)cy + 1, h01 },
                          v11[3] = { (float)cx + 1, (float)cy + 1, h11 };
                    // (the mesh's split: fx + fy <= 1 the first; normals in grid space, made world below)
                    float nn[3] = { 0, 0, 1 };
                    float bt = ray_tri(o, d, v00, v10, v01, best, nn);
                    bt = ray_tri(o, d, v11, v01, v10, bt, nn);
                    if (bt < best) {
                        best = bt;
                        float wn[3] = { nn[0] / cs, nn[1] / cs, nn[2] }, wl = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                        if (wl > 0.0f) n[0] = wn[0] / wl, n[1] = wn[1] / wl, n[2] = wn[2] / wl;
                    }
                }
                float cexit = fminf(ncx, ncy);
                if (cexit > tx_exit || cexit >= best) break;
                if (ncx < ncy) cx += sx, ncx += dcx;
                else cy += sy, ncy += dcy;
                if (cx < tx * T || cx >= (tx + 1) * T || cy < ty * T || cy >= (ty + 1) * T) break;
            }
        }
        if (tx_exit >= t1) break;
        te = tx_exit;
        if (ntx < nty) tx += sx, ntx += dtx;
        else ty += sy, nty += dty;
        if (tx < 0 || ty < 0 || tx >= nt || ty >= nt) break;
    }
    return best;
}

bool landscape_raycast(const float a[3], const float b[3], float lift, float* t, float n[3], Entity* actor) {
    float aa[3] = { a[0], a[1], a[2] - lift }, bb[3] = { b[0], b[1], b[2] - lift }, best = 1.0f;
    n[0] = n[1] = 0.0f, n[2] = 1.0f;
    *actor = ENTITY_NONE;
    for (uint32_t i = 0; i < L.nland; i++) {
        const Land* l = &L.land[i];
        if (!l->alive || !l->h || !l->tmm || !l->res) continue;
        float nb = land_ray(l, aa, bb, best, n);
        if (nb < best) best = nb, *actor = l->e;
    }
    float pz;
    if (procgen_height(aa[0], aa[1], &pz) || procgen_height(bb[0], bb[1], &pz)) { // (a Procedural World: marched)
        float f = landscape_trace(aa, bb);
        if (f < best) {
            best = f;
            float p[3] = { aa[0] + (bb[0] - aa[0]) * f, aa[1] + (bb[1] - aa[1]) * f, 0.0f }, zx0, zx1, zy0, zy1;
            if (procgen_height(p[0] - 25.0f, p[1], &zx0) && procgen_height(p[0] + 25.0f, p[1], &zx1) && procgen_height(p[0], p[1] - 25.0f, &zy0) &&
                procgen_height(p[0], p[1] + 25.0f, &zy1)) {
                float g[3] = { zx0 - zx1, zy0 - zy1, 50.0f }, gl = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                n[0] = g[0] / gl, n[1] = g[1] / gl, n[2] = g[2] / gl;
            }
            *actor = ENTITY_NONE;
        }
    }
    *t = best;
    return best < 1.0f;
}

// (Q21) a number that changes whenever a landscape's heights may have (made, gone, moved, resized, sculpted, imported):
// farview.c builds its heightfield again
uint32_t landscape_generation(void) {
    uint32_t h = 2166136261u ^ L.nland;
    for (uint32_t i = 0; i < L.nland; i++) {
        const Land* l = &L.land[i];
        uint32_t v[8] = { l->alive, l->res, l->edits, l->h != NULL, 0, 0, 0, 0 };
        memcpy(&v[4], &l->size, 4), memcpy(&v[5], &l->origin[0], 4), memcpy(&v[6], &l->origin[1], 4), memcpy(&v[7], &l->origin[2], 4);
        for (int k = 0; k < 8; k++) h = (h ^ v[k]) * 16777619u;
    }
    return h;
}

bool landscape_bounds(float mn[2], float mx[2]) {
    bool any = false;
    for (uint32_t i = 0; i < L.nland; i++) {
        const Land* l = &L.land[i];
        if (!l->alive) continue;
        for (int k = 0; k < 2; k++) {
            float lo = l->origin[k] - l->size * 0.5f, hi = l->origin[k] + l->size * 0.5f;
            mn[k] = any ? fminf(mn[k], lo) : lo, mx[k] = any ? fmaxf(mx[k], hi) : hi;
        }
        any = true;
    }
    return any;
}

// --- Heightmaps (Unreal's Import from File): a gray image or a .r16 / .raw, its rows along +Y, its columns along +X ---

uint16_t* mat_load_image_gray16(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c: the Windows Imaging Component)

static bool ext_is(const char* path, const char* ext) {
    const char* d = strrchr(path, '.');
    return d && !_stricmp(d, ext);
}

// 16-bit samples (malloc'd); a .r16 / .raw is square, little-endian
static uint16_t* read_heightmap(const char* path, uint32_t* w, uint32_t* h) {
    if (ext_is(path, ".r16") || ext_is(path, ".raw")) {
        FILE* f = re_fopen(path, "rb");
        if (!f) return NULL;
        _fseeki64(f, 0, SEEK_END);
        long long n = _ftelli64(f) / 2;
        _fseeki64(f, 0, SEEK_SET);
        uint32_t side = (uint32_t)llround(sqrt((double)n));
        uint16_t* px = side >= 2 && (long long)side * side == n ? malloc((size_t)n * 2) : NULL; // (an import, not a frame)
        if (px && fread(px, 2, (size_t)n, f) != (size_t)n) free(px), px = NULL;
        fclose(f);
        *w = *h = side;
        return px;
    }
    return mat_load_image_gray16(path, w, h);
}

bool landscape_heightmap_size(const char* path, uint32_t* w, uint32_t* h) {
    uint16_t* px = read_heightmap(path, w, h);
    free(px);
    return px != NULL;
}

void landscape_import_heightmap(Entity e, const char* path, bool add) {
    L.pending.e = e, L.pending.add = add;
    snprintf(L.pending.path, sizeof L.pending.path, "%s", path);
}

// Black at the actor, white at the Height; add: a displacement (mid gray: none) over the heights it has
static void apply_heightmap(Land* l, const CLandscape* c) {
    L.pending.e = ENTITY_NONE;
    double t0 = re_time_seconds();
    uint32_t iw = 0, ih = 0;
    uint16_t* px = read_heightmap(L.pending.path, &iw, &ih);
    if (!px) {
        snprintf(L.stats.import_msg, sizeof L.stats.import_msg, "!%s", L.pending.path);
        return;
    }
    uint32_t n = l->res + 1;
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < n; x++) { // (bilinear: the image's size needn't be the grid's)
            float u = (float)x / (float)l->res * (float)(iw - 1), v = (float)y / (float)l->res * (float)(ih - 1);
            uint32_t x0 = (uint32_t)u, y0 = (uint32_t)v, x1 = x0 + 1 < iw ? x0 + 1 : x0, y1 = y0 + 1 < ih ? y0 + 1 : y0;
            float fx = u - (float)x0, fy = v - (float)y0;
            float a = px[y0 * iw + x0], b = px[y0 * iw + x1], cc = px[y1 * iw + x0], d = px[y1 * iw + x1];
            float s = (a + (b - a) * fx + (cc - a) * fy + (a - b - cc + d) * fx * fy) / 65535.0f;
            float* hp = &l->h[y * n + x];
            *hp = L.pending.add ? *hp + (s - 0.5f) * c->height : s * c->height;
        }
    free(px);
    for (int k = 0; k < 4; k++) l->chunk_dirty[k] = ~0ull;
    l->save = true, l->edits++;
    snprintf(L.stats.import_msg, sizeof L.stats.import_msg, "%ux%u -> %ux%u, %.1f ms", iw, ih, n, n, (re_time_seconds() - t0) * 1000.0);
}

// --- The chunk meshes ---

static uint32_t oct(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-12f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t ix = (int16_t)lroundf(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f), iy = (int16_t)lroundf(fminf(fmaxf(y, -1.0f), 1.0f) * 32767.0f);
    return (uint16_t)ix | ((uint32_t)(uint16_t)iy << 16);
}

static bool write_chunk(const Land* l, uint32_t cx, uint32_t cy) {
    uint32_t q = l->res / l->chunks, nv = (q + 1) * (q + 1), ni = q * q * 6;
    GpuVertex* v = malloc(sizeof(GpuVertex) * nv); // (an edit's write, not a frame)
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    if (!v || !idx) return free(v), free(idx), false;
    float step = l->size / (float)l->res;
    RMeshHeader h = { RMESH_MAGIC, RMESH_VERSION, nv, ni, { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, 0.0f, 0 };
    for (uint32_t y = 0; y <= q; y++)
        for (uint32_t x = 0; x <= q; x++) {
            int gx = (int)(cx * q + x), gy = (int)(cy * q + y);
            GpuVertex* g = &v[y * (q + 1) + x];
            g->pos[0] = -l->size * 0.5f + (float)gx * step, g->pos[1] = -l->size * 0.5f + (float)gy * step, g->pos[2] = grid_h(l, gx, gy);
            float n[3] = { grid_h(l, gx - 1, gy) - grid_h(l, gx + 1, gy), grid_h(l, gx, gy - 1) - grid_h(l, gx, gy + 1), 2.0f * step };
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; k++) n[k] /= nl;
            g->normal_oct = oct(n);
            g->uv[0] = g->pos[0] / 400.0f, g->uv[1] = g->pos[1] / 400.0f; // (the material tiles every 4 m)
            float pad = 0.5f / (float)(q * 4 + 4); // (the lightmap: the chunk's square, a half texel in)
            g->uv2[0] = pad + (float)x / (float)q * (1.0f - 2.0f * pad), g->uv2[1] = pad + (float)y / (float)q * (1.0f - 2.0f * pad);
            for (int k = 0; k < 3; k++) h.mn[k] = fminf(h.mn[k], g->pos[k]), h.mx[k] = fmaxf(h.mx[k], g->pos[k]);
        }
    uint32_t* o = idx;
    for (uint32_t y = 0; y < q; y++)
        for (uint32_t x = 0; x < q; x++) {
            uint32_t a = y * (q + 1) + x, b = a + 1, c = a + q + 1, d = c + 1;
            *o++ = a, *o++ = b, *o++ = c, *o++ = b, *o++ = d, *o++ = c; // (the diagonal land_at follows)
            const float *pa = v[a].pos, *pb = v[b].pos, *pc = v[c].pos, *pd = v[d].pos;
            for (int t = 0; t < 2; t++) {
                const float *p0 = t ? pb : pa, *p1 = t ? pd : pb, *p2 = pc;
                float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                h.area += 0.5f * sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
            }
        }
    char p[800], sfx[32];
    snprintf(sfx, sizeof sfx, "_%u.rmesh", cy * l->chunks + cx);
    path_of(l, sfx, p, sizeof p);
    FILE* f = re_fopen(p, "wb");
    RMeshLods lods = { 1, { 0 }, { ni } }; // (.rmesh v2: one LOD, the grid as it is)
    uint32_t* col = NULL;
    if (l->wt) { // (B11) the painted layers as the vertex colors (sRGB bytes: the shader's decode gives back the weight)
        col = malloc(sizeof(uint32_t) * nv);
        if (col) {
            h.flags |= RMESH_COLORS;
            for (uint32_t y = 0; y <= q; y++)
                for (uint32_t x = 0; x <= q; x++) {
                    const uint8_t* s = &l->wt[((size_t)(cy * q + y) * (l->res + 1) + cx * q + x) * 3];
                    uint32_t c = 0u; // (alpha 0: the layers' weights, not a color: mesh.frag tells them apart)
                    for (int k = 0; k < 3; k++) {
                        float lin = (float)s[k] / 255.0f, sr = lin <= 0.0031308f ? lin * 12.92f : 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f;
                        c |= (uint32_t)lroundf(fminf(fmaxf(sr, 0.0f), 1.0f) * 255.0f) << (8 * k);
                    }
                    col[y * (q + 1) + x] = c;
                }
        }
    }
    bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&lods, sizeof lods, 1, f) == 1 && fwrite(v, sizeof(GpuVertex), nv, f) == nv &&
              fwrite(idx, 4, ni, f) == ni;
    if (ok && col) ok = fwrite(col, 4, nv, f) == nv;
    free(col);
    if (f) ok = fclose(f) == 0 && ok;
    free(v), free(idx);
    return ok;
}

static uint32_t mesh_id(const Land* l, uint32_t i) {
    char vp[200];
    snprintf(vp, sizeof vp, "/Content/Landscapes/%s_%u.rmesh", l->name, i);
    return render_mesh_id_of(vp);
}

// --- The children: the chunks (an older level's plants, kept as children with node >= PLANT_NODE, go with them) ---

static void destroy_children(World* w, Entity owner, bool plants_only) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++)
            if (at[r].kind == ATTACH_LANDSCAPE && (at[r].owner == owner || !ecs_alive(w, at[r].owner)) && (!plants_only || at[r].node >= PLANT_NODE)) {
                ecs_destroy(w, ents[r]);
                r--; // (the last row moved here)
                if (!ar->count) break;
                at = ecs_arch_column(ar, COMP_ATTACH), ents = ecs_arch_entities(ar);
            }
    }
}

static Entity child(World* w, Entity owner, uint32_t node, const char* name) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = owner, at->parent = owner, at->node = node, at->kind = ATTACH_LANDSCAPE;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    return e;
}

static uint32_t fnv(const void* p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

static void make_chunks(World* w, Land* l, const CLandscape* c, const CTransform* t) {
    destroy_children(w, l->e, false);
    for (uint32_t i = 0; i < l->chunks * l->chunks; i++) {
        char nm[48];
        snprintf(nm, sizeof nm, "LandscapeChunk_%u", i);
        Entity e = child(w, l->e, i, nm);
        if (e == ENTITY_NONE) break;
        CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
        sm->mesh = mesh_id(l, i);
        memcpy(sm->base_color, c->color, 12);
        sm->roughness = 0.9f, sm->cast_shadow = 1, sm->material = c->material, sm->uv_scale = 1.0f;
        CTransform* ct = ecs_get(w, e, COMP_TRANSFORM);
        memcpy(ct->location, t->location, 12);
        ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
        ct->flags = t->flags;
    }
}

void landscape_update(World* w, struct VkCtx* vk) {
    if (!L.root[0]) return; // (no Content folder yet: nowhere to keep its files)
    double t0 = re_time_seconds();
    for (uint32_t i = 0; i < L.nland; i++) L.land[i].alive = false;
    bool reload = false;
    L.stats.landscapes = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_LANDSCAPE)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        for (uint32_t r = 0; r < ar->count; r++) {
            Entity e = ecs_arch_entities(ar)[r];
            CLandscape* c = ecs_get(w, e, COMP_LANDSCAPE);
            const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            if (!c->name[0]) snprintf(c->name, sizeof c->name, "Landscape_%u", entity_index(e));
            uint32_t chunks = c->chunks < 1 ? 1 : c->chunks > 16 ? 16 : c->chunks;
            uint32_t res = c->res < 32 ? 32 : c->res > 1024 ? 1024 : c->res;
            res = (res + chunks - 1) / chunks * chunks;
            Land* l = NULL;
            for (uint32_t i = 0; i < L.nland && !l; i++)
                if (L.land[i].e == e || !strcmp(L.land[i].name, c->name)) l = &L.land[i];
            if (!l) {
                if (L.nland >= LAND_MAX) continue;
                l = &L.land[L.nland++];
                memset(l, 0, sizeof *l);
            }
            l->e = e, l->alive = true;
            bool fresh = !l->h || l->res != res || strcmp(l->name, c->name);
            snprintf(l->name, sizeof l->name, "%s", c->name);
            l->size = fmaxf(c->size, 100.0f), l->chunks = chunks;
            memcpy(l->origin, t->location, 12);
            if (fresh) {
                free(l->h), free(l->wt), l->wt = NULL, undo_free(l);
                l->res = res;
                l->h = calloc((size_t)(res + 1) * (res + 1), sizeof(float)); // (a landscape's start, not a frame)
                if (!l->h) continue;
                if (!load(l)) generate(l, c);
                l->mesh_hash = 0;
            }
            if (L.pending.e == e) apply_heightmap(l, c);
            L.stats.landscapes++;
            // the chunk meshes: written when made or edited (all of them when the size / chunking changed)
            uint32_t mh = fnv(&l->res, 4, fnv(&l->chunks, 4, fnv(&l->size, 4, fnv(l->name, strlen(l->name), 2166136261u))));
            if (mh != l->mesh_hash) {
                for (int k = 0; k < 4; k++) l->chunk_dirty[k] = ~0ull;
                l->need_children = true;
            }
            bool stroke_wait = l->stroking && re_time_seconds() - l->last_commit < 0.35;
            bool any = (l->chunk_dirty[0] | l->chunk_dirty[1] | l->chunk_dirty[2] | l->chunk_dirty[3]) != 0;
            if (any && !stroke_wait) {
                char dir[700];
                snprintf(dir, sizeof dir, "%s/Landscapes", L.root);
                re_make_dirs(dir);
                for (uint32_t i = 0; i < chunks * chunks; i++)
                    if (l->chunk_dirty[i >> 6] >> (i & 63) & 1ull) write_chunk(l, i % chunks, i / chunks);
                memset(l->chunk_dirty, 0, sizeof l->chunk_dirty);
                l->mesh_hash = mh, l->last_commit = re_time_seconds();
                reload = true;
                if (l->save && !l->stroking) save(l);
            }
        }
    }
    if (reload && vk) render_meshes_reload_async(vk); // (a worker reads them: sculpting never stops the editor)
    // the children (after the meshes exist: their ids)
    L.stats.chunks = L.stats.triangles = 0;
    for (uint32_t i = 0; i < L.nland; i++) {
        Land* l = &L.land[i];
        if (!l->alive) {
            if (l->e) destroy_children(w, l->e, false), l->e = 0;
            continue;
        }
        CLandscape* c = ecs_get(w, l->e, COMP_LANDSCAPE);
        const CTransform* t = ecs_get(w, l->e, COMP_TRANSFORM);
        if (!c || !t) continue;
        if (l->need_children || reload) {
            make_chunks(w, l, c, t);
            l->need_children = false;
        }
        L.stats.chunks += l->chunks * l->chunks, L.stats.triangles += l->res * l->res * 2;
    }
    L.stats.build_ms = reload ? (re_time_seconds() - t0) * 1000.0 : L.stats.build_ms;
}

// --- Sculpting ---

static Land* land_of(Entity e) {
    for (uint32_t i = 0; i < L.nland; i++)
        if (L.land[i].e == e && L.land[i].h) return &L.land[i];
    return NULL;
}

void landscape_brush(World* w, Entity e, int tool, const float at[3], float radius, float strength, float dt) {
    (void)w;
    Land* l = land_of(e);
    if (!l || tool <= LAND_TOOL_NONE || tool >= LAND_TOOL_COUNT) return;
    size_t npts = (size_t)(l->res + 1) * (l->res + 1);
    if (!l->stroking) {
        float z;
        l->flat_z = land_at(l, at[0], at[1], &z) ? z - l->origin[2] : 0.0f;
        l->stroking = true, l->last_commit = re_time_seconds();
        // (B11) the undo's before: the heights and the layers as the stroke finds them (the oldest of 8 dropped)
        int slot = (int)(l->nundo % 8u);
        free(l->undo[slot].h), free(l->undo[slot].wt);
        l->undo[slot].h = malloc(npts * sizeof(float));
        l->undo[slot].wt = l->wt ? malloc(npts * 3) : NULL;
        if (l->undo[slot].h) memcpy(l->undo[slot].h, l->h, npts * sizeof(float));
        if (l->undo[slot].wt) memcpy(l->undo[slot].wt, l->wt, npts * 3);
        l->nundo++;
    }
    if ((tool == LAND_TOOL_PAINT || tool == LAND_TOOL_ERASE) && !l->wt) { // (the first paint: the layers' grid, all 0; every chunk gets them)
        l->wt = calloc(npts, 3);
        if (!l->wt) return;
        for (int k = 0; k < 4; k++) l->chunk_dirty[k] = ~0ull;
    }
    float cell = l->size / (float)l->res, gx = (at[0] - l->origin[0] + l->size * 0.5f) / cell, gy = (at[1] - l->origin[1] + l->size * 0.5f) / cell;
    int rr = (int)ceilf(radius / cell), n = (int)l->res + 1;
    uint32_t q = l->res / l->chunks;
    for (int y = (int)gy - rr; y <= (int)gy + rr; y++)
        for (int x = (int)gx - rr; x <= (int)gx + rr; x++) {
            if (x < 0 || y < 0 || x >= n || y >= n) continue;
            float d = sqrtf(((float)x - gx) * ((float)x - gx) + ((float)y - gy) * ((float)y - gy)) * cell / radius;
            if (d >= 1.0f) continue;
            float wgt = 1.0f - d * d * (3.0f - 2.0f * d), *h = &l->h[y * n + x];
            switch (tool) {
            case LAND_TOOL_RAISE: *h += strength * dt * wgt; break;
            case LAND_TOOL_LOWER: *h -= strength * dt * wgt; break;
            case LAND_TOOL_SMOOTH: {
                float avg = (grid_h(l, x - 1, y) + grid_h(l, x + 1, y) + grid_h(l, x, y - 1) + grid_h(l, x, y + 1)) * 0.25f;
                *h += (avg - *h) * fminf(strength * dt * 0.02f, 1.0f) * wgt;
                break;
            }
            case LAND_TOOL_FLATTEN: *h += (l->flat_z - *h) * fminf(strength * dt * 0.02f, 1.0f) * wgt; break;
            case LAND_TOOL_PAINT: case LAND_TOOL_ERASE: { // (B11) the layer's weight up (the others down: the shares sum to 1 at most) or down
                uint8_t* s = &l->wt[((size_t)y * n + x) * 3];
                float add = fminf(strength * dt * 0.004f, 1.0f) * wgt * (tool == LAND_TOOL_ERASE ? -1.0f : 1.0f);
                float wv[3] = { s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f };
                int k = g_paint_layer - 1;
                wv[k] = fminf(fmaxf(wv[k] + add, 0.0f), 1.0f);
                float rest = wv[0] + wv[1] + wv[2] - wv[k];
                if (add > 0.0f && wv[k] + rest > 1.0f && rest > 0.0f) // (the others give way)
                    for (int o = 0; o < 3; o++)
                        if (o != k) wv[o] *= (1.0f - wv[k]) / rest;
                for (int o = 0; o < 3; o++) s[o] = (uint8_t)lroundf(wv[o] * 255.0f);
                break;
            }
            default: break;
            }
            // the chunks this point is in (a corner is in up to four)
            for (int oy = 0; oy <= 1; oy++)
                for (int ox = 0; ox <= 1; ox++) {
                    int cx = (x - ox) / (int)q, cy = (y - oy) / (int)q;
                    if (x - ox < 0 || y - oy < 0 || cx >= (int)l->chunks || cy >= (int)l->chunks) continue;
                    uint32_t ci = (uint32_t)(cy * (int)l->chunks + cx);
                    l->chunk_dirty[ci >> 6] |= 1ull << (ci & 63);
                }
        }
    l->save = true;
}

void landscape_stroke_end(World* w, Entity e) {
    (void)w;
    Land* l = land_of(e);
    if (!l || !l->stroking) return;
    l->stroking = false, l->edits++;
    l->last_commit = 0.0;
    if (!(l->chunk_dirty[0] | l->chunk_dirty[1] | l->chunk_dirty[2] | l->chunk_dirty[3])) save(l);
}

bool landscape_undo(World* w, Entity e) {
    (void)w;
    Land* l = land_of(e);
    if (!l || !l->nundo || l->stroking) return false;
    int slot = (int)((l->nundo - 1) % 8u);
    if (!l->undo[slot].h) return false;
    size_t npts = (size_t)(l->res + 1) * (l->res + 1);
    memcpy(l->h, l->undo[slot].h, npts * sizeof(float));
    if (l->undo[slot].wt) {
        if (!l->wt) l->wt = malloc(npts * 3);
        if (l->wt) memcpy(l->wt, l->undo[slot].wt, npts * 3);
    } else free(l->wt), l->wt = NULL;
    free(l->undo[slot].h), free(l->undo[slot].wt), l->undo[slot].h = NULL, l->undo[slot].wt = NULL;
    l->nundo--;
    for (int k = 0; k < 4; k++) l->chunk_dirty[k] = ~0ull;
    l->save = true, l->edits++;
    return true;
}

bool landscape_export_heightmap(Entity e, const char* path, float* lo, float* hi) {
    Land* l = land_of(e);
    if (!l) return false;
    size_t n = (size_t)(l->res + 1) * (l->res + 1);
    float a = 1e30f, b = -1e30f;
    for (size_t i = 0; i < n; i++) a = fminf(a, l->h[i]), b = fmaxf(b, l->h[i]);
    uint16_t* px = malloc(n * 2);
    if (!px) return false;
    float k = b > a ? 65535.0f / (b - a) : 0.0f;
    for (size_t i = 0; i < n; i++) px[i] = (uint16_t)lroundf((l->h[i] - a) * k); // (little-endian rows from -y to +y: as read back)
    FILE* f = re_fopen(path, "wb");
    bool ok = f && fwrite(px, 2, n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    free(px);
    if (lo) *lo = a;
    if (hi) *hi = b;
    return ok;
}

float landscape_layer_weight(Entity e, int layer, float x, float y) {
    Land* l = land_of(e);
    if (!l || !l->wt || layer < 1 || layer > 3) return 0.0f;
    float cell = l->size / (float)l->res;
    int gx = (int)lroundf((x - l->origin[0] + l->size * 0.5f) / cell), gy = (int)lroundf((y - l->origin[1] + l->size * 0.5f) / cell);
    int n = (int)l->res + 1;
    if (gx < 0 || gy < 0 || gx >= n || gy >= n) return 0.0f;
    return l->wt[((size_t)gy * n + gx) * 3 + (layer - 1)] / 255.0f;
}

// (Q15) a painted layer's share (0..1) at a world point, on whichever landscape is there (the PCG's Landscape Layer
// Filter); 0: no landscape or no paint there
float landscape_layer_at(float x, float y, int layer) {
    if (layer < 1 || layer > 3) return 0.0f;
    for (uint32_t i = 0; i < L.nland; i++) {
        const Land* l = &L.land[i];
        if (!l->alive || !l->h || !l->wt) continue;
        float cell = l->size / (float)l->res;
        int gx = (int)lroundf((x - l->origin[0] + l->size * 0.5f) / cell), gy = (int)lroundf((y - l->origin[1] + l->size * 0.5f) / cell);
        int n = (int)l->res + 1;
        if (gx < 0 || gy < 0 || gx >= n || gy >= n) continue;
        return l->wt[((size_t)gy * n + gx) * 3 + (layer - 1)] / 255.0f;
    }
    return 0.0f;
}

void landscape_regenerate(World* w, Entity e) {
    Land* l = land_of(e);
    CLandscape* c = ecs_get(w, e, COMP_LANDSCAPE);
    if (l && c) generate(l, c);
}
