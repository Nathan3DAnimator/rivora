// PCG execution (Q15, pcg.h): a graph run over a box. The nodes in an order where every input is ready (Kahn), each
// one's points in structure-of-arrays in the run's linear arena (nothing freed until the caller resets it), the
// samplers split over the job system's workers by rows (each row writes its own stretch, then the rows are packed in
// order: the same points whatever the threads). Random numbers: a hash of the component's seed, the node's id and the
// point's seed (its grid cell's), so they don't depend on the order, the threads or the cell being made.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "landscape.h"
#include "pcg.h"
#include "platform.h"
#include "render.h"
#include "render_gpu.h"

#define DEG 0.017453292519943295f
#define MAX_POINTS (4u << 20) // a node's points at most (4 M)

static inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return x;
}
static inline uint32_t hash3u(uint32_t a, uint32_t b, uint32_t c) { return mix32(a * 0x9E3779B1u ^ mix32(b + 0x85EBCA77u) ^ mix32(c * 0xC2B2AE3Du + 0x27D4EB2Fu)); }
static inline float rnd(uint32_t seed, uint32_t node, uint32_t pseed, uint32_t k) {
    return (float)(mix32(hash3u(seed, node, pseed) + k * 0x9E3779B9u) >> 8) * (1.0f / 16777216.0f);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

typedef struct Exec {
    PcgRun* r;
    const PcgGraph* g;
    float bmin[3], bmax[3];
    uint32_t seed;
    float params[PCG_MAX_PARAMS];
    const PcgPoints* input;
    PcgPoints* output;
    int depth;
} Exec;

static void fail(PcgRun* r, const char* msg) {
    if (!r->error[0]) snprintf(r->error, sizeof r->error, "%s", msg);
}

// --- Points ---
static PcgPoints g_empty; // (n 0: an unconnected input)

static float* farr(PcgRun* r, uint32_t n) { return re_arena_alloc(r->arena, sizeof(float) * (n ? n : 1)); }
static PcgPoints* pts_new(PcgRun* r, uint32_t cap, const PcgPoints* like) {
    PcgPoints* p = re_arena_alloc(r->arena, sizeof *p);
    if (!p) return fail(r, "out of memory (the arena)"), NULL;
    memset(p, 0, sizeof *p);
    p->cap = cap;
    float** f[] = { &p->x, &p->y, &p->z, &p->nx, &p->ny, &p->nz, &p->pitch, &p->yaw, &p->roll, &p->sx, &p->sy, &p->sz, &p->density, &p->ext };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
        if (!(*f[i] = farr(r, cap))) return fail(r, "out of memory (the arena)"), NULL;
    if (!(p->seed = re_arena_alloc(r->arena, sizeof(uint32_t) * (cap ? cap : 1)))) return fail(r, "out of memory (the arena)"), NULL;
    if (like) {
        p->nattr = like->nattr;
        memcpy(p->attr_name, like->attr_name, sizeof p->attr_name);
        for (uint32_t a = 0; a < p->nattr; a++)
            if (!(p->attr[a] = farr(r, cap))) return fail(r, "out of memory (the arena)"), NULL;
    }
    return p;
}
static int attr_index(const PcgPoints* p, const char* name) {
    for (uint32_t a = 0; a < p->nattr; a++)
        if (!_stricmp(p->attr_name[a], name)) return (int)a;
    return -1;
}
// point i of a into j of b (b has a's attributes, maybe more: those get 0)
static void pts_copy(PcgPoints* b, uint32_t j, const PcgPoints* a, uint32_t i) {
    b->x[j] = a->x[i], b->y[j] = a->y[i], b->z[j] = a->z[i];
    b->nx[j] = a->nx[i], b->ny[j] = a->ny[i], b->nz[j] = a->nz[i];
    b->pitch[j] = a->pitch[i], b->yaw[j] = a->yaw[i], b->roll[j] = a->roll[i];
    b->sx[j] = a->sx[i], b->sy[j] = a->sy[i], b->sz[j] = a->sz[i];
    b->density[j] = a->density[i], b->ext[j] = a->ext[i], b->seed[j] = a->seed[i];
    for (uint32_t k = 0; k < b->nattr; k++) {
        int s = k < a->nattr && !strcmp(a->attr_name[k], b->attr_name[k]) ? (int)k : attr_index(a, b->attr_name[k]);
        b->attr[k][j] = s >= 0 ? a->attr[s][i] : 0.0f;
    }
}
static void pt_init(PcgPoints* p, uint32_t j, float x, float y, float z, const float n[3], float ext, uint32_t seed) {
    p->x[j] = x, p->y[j] = y, p->z[j] = z;
    p->nx[j] = n[0], p->ny[j] = n[1], p->nz[j] = n[2];
    p->pitch[j] = p->yaw[j] = p->roll[j] = 0.0f;
    p->sx[j] = p->sy[j] = p->sz[j] = 1.0f;
    p->density[j] = 1.0f, p->ext[j] = ext, p->seed[j] = seed;
}
// Several sets in one (the attributes of all of them)
static PcgPoints* pts_merge(PcgRun* r, const PcgPoints** in, int n) {
    uint32_t total = 0;
    PcgPoints like = { 0 };
    for (int k = 0; k < n; k++) {
        total += in[k]->n;
        for (uint32_t a = 0; a < in[k]->nattr; a++)
            if (attr_index(&like, in[k]->attr_name[a]) < 0 && like.nattr < PCG_MAX_ATTRS) memcpy(like.attr_name[like.nattr++], in[k]->attr_name[a], PCG_ATTR_NAME);
    }
    PcgPoints* o = pts_new(r, total, &like);
    if (!o) return NULL;
    for (int k = 0; k < n; k++)
        for (uint32_t i = 0; i < in[k]->n; i++) pts_copy(o, o->n++, in[k], i);
    return o;
}

// --- The ground ---
typedef struct MeshGrid {
    float x0, y0, cell;
    int nx, ny;
    uint32_t* start;  // nx * ny + 1 (CSR)
    uint32_t* tris;
    float (*v)[9];    // the triangles (world)
    uint32_t ntri;
} MeshGrid;

static MeshGrid* grid_build(PcgRun* r, const float bmin[3], const float bmax[3]) {
    const PcgSources* s = r->src;
    MeshGrid* m = re_arena_alloc(r->arena, sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    const GpuVertex* vv;
    const uint32_t* ii;
    uint32_t total = 0;
    for (uint32_t k = 0; s && k < s->nmesh; k++) {
        uint32_t fi, ic;
        int32_t vo;
        render_mesh_cpu(s->mesh[k].mesh, &vv, &ii, &fi, &ic, &vo);
        if (vv && ii) total += ic / 3;
    }
    m->v = re_arena_alloc(r->arena, sizeof(float) * 9 * (total ? total : 1));
    if (!m->v) return NULL;
    for (uint32_t k = 0; s && k < s->nmesh; k++) {
        uint32_t fi, ic;
        int32_t vo;
        render_mesh_cpu(s->mesh[k].mesh, &vv, &ii, &fi, &ic, &vo);
        if (!vv || !ii) continue;
        const float(*R)[4] = s->mesh[k].rows;
        for (uint32_t t = 0; t + 2 < ic; t += 3) {
            float* o = m->v[m->ntri];
            float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
            for (int c = 0; c < 3; c++) {
                const float* p = vv[(int64_t)ii[fi + t + c] + vo].pos;
                for (int a = 0; a < 3; a++) o[c * 3 + a] = R[a][0] * p[0] + R[a][1] * p[1] + R[a][2] * p[2] + R[a][3];
                lo[0] = fminf(lo[0], o[c * 3]), lo[1] = fminf(lo[1], o[c * 3 + 1]), hi[0] = fmaxf(hi[0], o[c * 3]), hi[1] = fmaxf(hi[1], o[c * 3 + 1]);
            }
            if (hi[0] < bmin[0] || lo[0] > bmax[0] || hi[1] < bmin[1] || lo[1] > bmax[1]) continue;
            m->ntri++;
        }
    }
    float w = bmax[0] - bmin[0], h = bmax[1] - bmin[1];
    m->cell = fmaxf(fmaxf(w, h) / 128.0f, 50.0f);
    m->nx = (int)(w / m->cell) + 1, m->ny = (int)(h / m->cell) + 1;
    m->x0 = bmin[0], m->y0 = bmin[1];
    size_t nc = (size_t)m->nx * m->ny;
    m->start = re_arena_alloc(r->arena, sizeof(uint32_t) * (nc + 1));
    uint32_t* head = re_arena_alloc(r->arena, sizeof(uint32_t) * (nc + 1));
    if (!m->start || !head) return NULL;
    memset(m->start, 0, sizeof(uint32_t) * (nc + 1));
    for (int pass = 0; pass < 2; pass++) { // (counts, then the triangles: compressed rows)
        if (pass) {
            for (size_t c = 1; c <= nc; c++) m->start[c] += m->start[c - 1];
            memcpy(head, m->start, sizeof(uint32_t) * (nc + 1));
            m->tris = re_arena_alloc(r->arena, sizeof(uint32_t) * (m->start[nc] ? m->start[nc] : 1));
            if (!m->tris) return NULL;
        }
        for (uint32_t t = 0; t < m->ntri; t++) {
            const float* o = m->v[t];
            float lx = fminf(o[0], fminf(o[3], o[6])), hx = fmaxf(o[0], fmaxf(o[3], o[6]));
            float ly = fminf(o[1], fminf(o[4], o[7])), hy = fmaxf(o[1], fmaxf(o[4], o[7]));
            int x0 = (int)floorf((lx - m->x0) / m->cell), x1 = (int)floorf((hx - m->x0) / m->cell);
            int y0 = (int)floorf((ly - m->y0) / m->cell), y1 = (int)floorf((hy - m->y0) / m->cell);
            x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 >= m->nx ? m->nx - 1 : x1, y1 = y1 >= m->ny ? m->ny - 1 : y1;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    size_t c = (size_t)y * m->nx + x;
                    if (!pass) m->start[c + 1]++;
                    else m->tris[head[c]++] = t;
                }
        }
    }
    return m;
}

// The highest hit of a vertical ray at (x, y) among the triangles below zmax; false: none
static bool grid_hit(const MeshGrid* m, float x, float y, float zmax, float* z, float n[3]) {
    int cx = (int)floorf((x - m->x0) / m->cell), cy = (int)floorf((y - m->y0) / m->cell);
    if (cx < 0 || cy < 0 || cx >= m->nx || cy >= m->ny) return false;
    size_t c = (size_t)cy * m->nx + cx;
    bool any = false;
    for (uint32_t k = m->start[c]; k < m->start[c + 1]; k++) {
        const float* o = m->v[m->tris[k]];
        float e1x = o[3] - o[0], e1y = o[4] - o[1], e2x = o[6] - o[0], e2y = o[7] - o[1];
        float det = e1x * e2y - e1y * e2x;
        if (fabsf(det) < 1e-6f) continue; // (vertical: no top)
        float px = x - o[0], py = y - o[1];
        float u = (px * e2y - py * e2x) / det, v = (e1x * py - e1y * px) / det;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
        float hz = o[2] + u * (o[5] - o[2]) + v * (o[8] - o[2]);
        if (hz > zmax || (any && hz <= *z)) continue;
        float ax = o[3] - o[0], ay = o[4] - o[1], az = o[5] - o[2], bx = o[6] - o[0], by = o[7] - o[1], bz = o[8] - o[2];
        float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx, l = sqrtf(nx * nx + ny * ny + nz * nz);
        if (l < 1e-12f) continue;
        if (nz < 0.0f) nx = -nx, ny = -ny, nz = -nz;
        *z = hz, n[0] = nx / l, n[1] = ny / l, n[2] = nz / l, any = true;
    }
    return any;
}

static bool land_at(float x, float y, float* z, float n[3]) {
    if (!landscape_height(x, y, z)) return false;
    const float d = 50.0f;
    float hx0 = *z, hx1 = *z, hy0 = *z, hy1 = *z;
    landscape_height(x - d, y, &hx0), landscape_height(x + d, y, &hx1);
    landscape_height(x, y - d, &hy0), landscape_height(x, y + d, &hy1);
    float gx = (hx1 - hx0) / (2.0f * d), gy = (hy1 - hy0) / (2.0f * d), l = sqrtf(gx * gx + gy * gy + 1.0f);
    n[0] = -gx / l, n[1] = -gy / l, n[2] = 1.0f / l;
    return true;
}

// The ground for a target (0 Landscape, 1 Meshes, 2 both: the highest)
static bool surface_at(const MeshGrid* mg, int target, float x, float y, float zmax, float* z, float n[3]) {
    bool any = false;
    if (target != 1) any = land_at(x, y, z, n);
    if (target != 0 && mg) {
        float mz, mn[3];
        if (grid_hit(mg, x, y, zmax, &mz, mn) && (!any || mz > *z)) *z = mz, memcpy(n, mn, 12), any = true;
    }
    return any;
}

// --- Parameters ---
static float P(const Exec* x, const PcgNode* n, int slot) {
    int b = n->bind[slot];
    return b && b <= (int)x->g->nparams ? x->params[b - 1] : n->f[slot];
}

// --- Samplers (rows on the workers) ---
typedef struct RowJob {
    Exec* x;
    const PcgNode* n;
    PcgPoints* out;
    const MeshGrid* mg;
    int64_t ix0, iy0, iz0;
    int nx, ny, nz;
    float step[3];
    uint32_t* cnt;  // per row
    int rows_per;   // rows a job item does
    int nrows;
} RowJob;

static void surface_rows(void* arg, int item) {
    RowJob* j = arg;
    const Exec* x = j->x;
    const PcgNode* n = j->n;
    int target = (int)P(x, n, 0);
    float ext = P(x, n, 2), loose = clampf(P(x, n, 3), 0.0f, 1.0f), s = j->step[0];
    for (int row = item * j->rows_per; row < (item + 1) * j->rows_per && row < j->nrows; row++) {
        int64_t iy = j->iy0 + row;
        uint32_t base = (uint32_t)row * (uint32_t)j->nx, c = 0;
        for (int col = 0; col < j->nx; col++) {
            int64_t ix = j->ix0 + col;
            uint32_t cs = hash3u((uint32_t)ix, (uint32_t)iy, n->id);
            float px = ((float)ix + 0.5f) * s + (rnd(x->seed, n->id, cs, 0) - 0.5f) * loose * s;
            float py = ((float)iy + 0.5f) * s + (rnd(x->seed, n->id, cs, 1) - 0.5f) * loose * s;
            if (px < x->bmin[0] || px >= x->bmax[0] || py < x->bmin[1] || py >= x->bmax[1]) continue;
            float z, nn[3];
            if (!surface_at(j->mg, target, px, py, x->bmax[2], &z, nn) || z < x->bmin[2] || z > x->bmax[2]) continue;
            pt_init(j->out, base + c++, px, py, z, nn, ext, cs);
        }
        j->cnt[row] = c;
    }
}

static void volume_rows(void* arg, int item) {
    RowJob* j = arg;
    const Exec* x = j->x;
    const PcgNode* n = j->n;
    float loose = clampf(P(x, n, 3), 0.0f, 1.0f), ext = P(x, n, 4);
    static const float up[3] = { 0, 0, 1 };
    for (int row = item * j->rows_per; row < (item + 1) * j->rows_per && row < j->nrows; row++) { // (a row: one y and one z)
        int64_t iy = j->iy0 + row % j->ny, iz = j->iz0 + row / j->ny;
        uint32_t base = (uint32_t)row * (uint32_t)j->nx, c = 0;
        for (int col = 0; col < j->nx; col++) {
            int64_t ix = j->ix0 + col;
            uint32_t cs = hash3u((uint32_t)ix, (uint32_t)iy * 7919u + (uint32_t)iz, n->id);
            float p[3];
            int64_t ic[3] = { ix, iy, iz };
            for (int a = 0; a < 3; a++) p[a] = ((float)ic[a] + 0.5f) * j->step[a] + (rnd(x->seed, n->id, cs, (uint32_t)a) - 0.5f) * loose * j->step[a];
            if (p[0] < x->bmin[0] || p[0] >= x->bmax[0] || p[1] < x->bmin[1] || p[1] >= x->bmax[1] || p[2] < x->bmin[2] || p[2] >= x->bmax[2]) continue;
            pt_init(j->out, base + c++, p[0], p[1], p[2], up, ext, cs);
        }
        j->cnt[row] = c;
    }
}

// rows into one packed set (in order)
static void pack_rows(PcgPoints* o, const uint32_t* cnt, int nrows, uint32_t row_len) {
    uint32_t w = 0;
    for (int r = 0; r < nrows; r++) {
        uint32_t src = (uint32_t)r * row_len;
        if (src != w)
            for (uint32_t k = 0; k < cnt[r]; k++) pts_copy(o, w + k, o, src + k);
        w += cnt[r];
    }
    o->n = w;
}

static void run_rows(Exec* x, RowJob* j, void (*fn)(void*, int)) {
    int items = (j->nrows + j->rows_per - 1) / j->rows_per;
    if (x->r->serial || (uint64_t)j->nrows * (uint64_t)j->nx < 8192u) {
        for (int i = 0; i < items; i++) fn(j, i);
    } else
        re_parallel_for(items, fn, j);
}

static const MeshGrid* grid_for(Exec* x, int target) {
    if (target == 0 || !x->r->src || !x->r->src->nmesh) return NULL;
    // (one per run: the top level's box, a little bigger; a nested graph's box is inside it or near)
    if (!x->r->grid) {
        float lo[3], hi[3];
        for (int a = 0; a < 3; a++) lo[a] = x->r->bmin[a] - 2000.0f, hi[a] = x->r->bmax[a] + 2000.0f;
        x->r->grid = grid_build(x->r, lo, hi);
    }
    return x->r->grid;
}

static PcgPoints* surface_sampler(Exec* x, const PcgNode* n) {
    float dens = fmaxf(P(x, n, 1), 1e-4f), s = sqrtf(10000.0f / dens);
    int64_t ix0 = (int64_t)floorf(x->bmin[0] / s), ix1 = (int64_t)floorf(x->bmax[0] / s);
    int64_t iy0 = (int64_t)floorf(x->bmin[1] / s), iy1 = (int64_t)floorf(x->bmax[1] / s);
    int64_t nx = ix1 - ix0 + 1, ny = iy1 - iy0 + 1;
    if (nx <= 0 || ny <= 0) return pts_new(x->r, 0, NULL);
    if ((uint64_t)nx * (uint64_t)ny > MAX_POINTS) return fail(x->r, "Surface Sampler: too many points (over 4 M): lower Points per m2"), NULL;
    const MeshGrid* mg = grid_for(x, (int)P(x, n, 0));
    PcgPoints* o = pts_new(x->r, (uint32_t)(nx * ny), NULL);
    uint32_t* cnt = re_arena_alloc(x->r->arena, sizeof(uint32_t) * (size_t)ny);
    if (!o || !cnt) return fail(x->r, "out of memory (the arena)"), NULL;
    RowJob j = { x, n, o, mg, ix0, iy0, 0, (int)nx, (int)ny, 1, { s, s, s }, cnt, 0, (int)ny };
    j.rows_per = j.nrows / 64 + 1;
    run_rows(x, &j, surface_rows);
    pack_rows(o, cnt, (int)ny, (uint32_t)nx);
    return o;
}

static PcgPoints* volume_sampler(Exec* x, const PcgNode* n) {
    float st[3] = { fmaxf(P(x, n, 0), 10.0f), fmaxf(P(x, n, 1), 10.0f), fmaxf(P(x, n, 2), 10.0f) };
    int64_t i0[3], cnt3[3];
    for (int a = 0; a < 3; a++) i0[a] = (int64_t)floorf(x->bmin[a] / st[a]), cnt3[a] = (int64_t)floorf(x->bmax[a] / st[a]) - i0[a] + 1;
    if (cnt3[0] <= 0 || cnt3[1] <= 0 || cnt3[2] <= 0) return pts_new(x->r, 0, NULL);
    if ((uint64_t)cnt3[0] * (uint64_t)cnt3[1] * (uint64_t)cnt3[2] > MAX_POINTS) return fail(x->r, "Volume Sampler: too many points (over 4 M): a bigger Voxel Size"), NULL;
    int nrows = (int)(cnt3[1] * cnt3[2]);
    PcgPoints* o = pts_new(x->r, (uint32_t)(cnt3[0] * nrows), NULL);
    uint32_t* cnt = re_arena_alloc(x->r->arena, sizeof(uint32_t) * (size_t)nrows);
    if (!o || !cnt) return fail(x->r, "out of memory (the arena)"), NULL;
    RowJob j = { x, n, o, NULL, i0[0], i0[1], i0[2], (int)cnt3[0], (int)cnt3[1], (int)cnt3[2], { st[0], st[1], st[2] }, cnt, 0, nrows };
    j.rows_per = nrows / 64 + 1;
    run_rows(x, &j, volume_rows);
    pack_rows(o, cnt, nrows, (uint32_t)cnt3[0]);
    return o;
}

// A spline at a distance along it (its samples are dense: linear between them)
static void spline_eval(const PcgSplinePoint* s, uint32_t n, float d, float p[3], float t[3], float* width) {
    uint32_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (s[mid].dist <= d) lo = mid;
        else hi = mid;
    }
    float seg = s[hi].dist - s[lo].dist, u = seg > 1e-4f ? clampf((d - s[lo].dist) / seg, 0.0f, 1.0f) : 0.0f;
    for (int a = 0; a < 3; a++) p[a] = s[lo].p[a] + (s[hi].p[a] - s[lo].p[a]) * u, t[a] = s[lo].t[a] + (s[hi].t[a] - s[lo].t[a]) * u;
    float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (l > 1e-6f) t[0] /= l, t[1] /= l, t[2] /= l;
    else t[0] = 1, t[1] = t[2] = 0;
    *width = s[lo].width + (s[hi].width - s[lo].width) * u;
}

static PcgPoints* spline_sampler(Exec* x, const PcgNode* n) {
    const PcgSources* src = x->r->src;
    int mode = (int)P(x, n, 0);
    float spacing = fmaxf(P(x, n, 1), 5.0f), off = P(x, n, 2), wprop = P(x, n, 6);
    bool both = P(x, n, 3) != 0.0f, project = P(x, n, 4) != 0.0f, align = P(x, n, 5) != 0.0f;
    uint32_t cap = 0;
    for (uint32_t k = 0; src && k < src->nspline; k++) {
        if (n->s[0][0] && _stricmp(n->s[0], src->spline_name[k])) continue;
        uint32_t m = src->spline_n[k];
        if (m < 2) continue;
        float len = src->spline[k][m - 1].dist, wmax = wprop;
        for (uint32_t i = 0; i < m && wprop <= 0.0f; i++) wmax = fmaxf(wmax, src->spline[k][i].width);
        uint64_t c = (uint64_t)(len / spacing + 2.0f) * (mode == 1 ? (uint64_t)(wmax / spacing + 2.0f) : both ? 2u : 1u);
        if (cap + c > MAX_POINTS) return fail(x->r, "Spline Sampler: too many points: a bigger Spacing"), NULL;
        cap += (uint32_t)c;
    }
    PcgPoints* o = pts_new(x->r, cap, NULL);
    if (!o) return NULL;
    for (uint32_t k = 0; src && k < src->nspline; k++) {
        if (n->s[0][0] && _stricmp(n->s[0], src->spline_name[k])) continue;
        const PcgSplinePoint* sp = src->spline[k];
        uint32_t m = src->spline_n[k];
        if (m < 2) continue;
        // (the spline's identity: its first point, whatever the order the volume or the cell lists the splines in)
        uint32_t sid = hash3u((uint32_t)(int32_t)lroundf(sp[0].p[0]), (uint32_t)(int32_t)lroundf(sp[0].p[1]), (uint32_t)(int32_t)lroundf(sp[0].p[2]));
        float len = sp[m - 1].dist;
        for (uint32_t di = 0; (float)di * spacing <= len + 0.01f; di++) {
            float p[3], t[3], w;
            spline_eval(sp, m, (float)di * spacing, p, t, &w);
            if (wprop > 0.0f) w = wprop;
            float rl = sqrtf(t[0] * t[0] + t[1] * t[1]);
            float right[2] = { rl > 1e-6f ? -t[1] / rl : 0.0f, rl > 1e-6f ? t[0] / rl : 1.0f };
            int nside = mode == 1 ? (int)(w / spacing) + 1 : both ? 2 : 1;
            for (int si = 0; si < nside; si++) {
                float lat = mode == 1 ? (nside > 1 ? -0.5f * w + w * (float)si / (float)(nside - 1) : 0.0f) : (si ? -off : off);
                uint32_t ps = hash3u(sid, di, (uint32_t)si * 131u + n->id);
                float q[3] = { p[0] + right[0] * lat, p[1] + right[1] * lat, p[2] };
                if (mode == 1) { // (inside: a little looseness so the rows don't show)
                    q[0] += (rnd(x->seed, n->id, ps, 0) - 0.5f) * spacing * 0.8f, q[1] += (rnd(x->seed, n->id, ps, 1) - 0.5f) * spacing * 0.8f;
                }
                float nn[3] = { 0, 0, 1 };
                if (project) {
                    float z;
                    if (land_at(q[0], q[1], &z, nn)) q[2] = z;
                }
                if (q[0] < x->bmin[0] || q[0] >= x->bmax[0] || q[1] < x->bmin[1] || q[1] >= x->bmax[1] || q[2] < x->bmin[2] || q[2] > x->bmax[2]) continue;
                uint32_t j = o->n++;
                pt_init(o, j, q[0], q[1], q[2], nn, spacing * 0.5f, ps);
                if (align) o->yaw[j] = atan2f(t[1], t[0]) / DEG, o->pitch[j] = asinf(clampf(t[2], -1.0f, 1.0f)) / DEG * (project ? 0.0f : 1.0f);
            }
        }
    }
    return o;
}

// --- Point operations ---
static float attr_get(const PcgPoints* p, int which, uint32_t i) {
    switch (which) {
    case -1: return p->density[i];
    case -2: return p->x[i];
    case -3: return p->y[i];
    case -4: return p->z[i];
    case -5: return acosf(clampf(p->nz[i], -1.0f, 1.0f)) / DEG;
    case -6: return p->sz[i];
    case -7: return p->yaw[i];
    case -8: return (float)(p->seed[i] & 0xFFFFu) / 65535.0f;
    default: return which >= 0 ? p->attr[which][i] : 0.0f;
    }
}
static int attr_which(const PcgPoints* p, const char* name) {
    static const char* const builtin[] = { "Density", "X", "Y", "Z", "Steepness", "Scale", "Yaw", "Seed" };
    int a = attr_index(p, name);
    if (a >= 0) return a;
    for (int k = 0; k < 8; k++)
        if (!_stricmp(name, builtin[k])) return -1 - k;
    if (!_stricmp(name, "Height")) return -4;
    return -100;
}

// A filter: kept[i] decides; out[0] the kept, out[1] the others
static bool split(Exec* x, const PcgPoints* in, const uint8_t* keep, PcgPoints* out[2]) {
    uint32_t nk = 0;
    for (uint32_t i = 0; i < in->n; i++) nk += keep[i];
    out[0] = pts_new(x->r, nk, in), out[1] = pts_new(x->r, in->n - nk, in);
    if (!out[0] || !out[1]) return false;
    for (uint32_t i = 0; i < in->n; i++) pts_copy(out[keep[i] ? 0 : 1], out[keep[i] ? 0 : 1]->n++, in, i);
    return true;
}

static float combine(int mode, float cur, float v) {
    switch (mode) {
    case 0: return v;
    case 1: return cur * v;
    case 2: return clampf(cur + v, 0.0f, 1.0f);
    case 3: return fminf(cur, v);
    default: return fmaxf(cur, v);
    }
}

static float vnoise(float x, float y, uint32_t seed) { // value noise, smooth (C1)
    float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    int32_t ix = (int32_t)fx, iy = (int32_t)fy;
    float a = (float)(hash3u((uint32_t)ix, (uint32_t)iy, seed) >> 8) / 16777216.0f, b = (float)(hash3u((uint32_t)ix + 1, (uint32_t)iy, seed) >> 8) / 16777216.0f;
    float c = (float)(hash3u((uint32_t)ix, (uint32_t)iy + 1, seed) >> 8) / 16777216.0f, d = (float)(hash3u((uint32_t)ix + 1, (uint32_t)iy + 1, seed) >> 8) / 16777216.0f;
    tx = tx * tx * (3.0f - 2.0f * tx), ty = ty * ty * (3.0f - 2.0f * ty);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// The axes of a point's rotation, its up tilted toward the normal by k (0..1)
static void align_rot(float* pitch, float* yaw, float* roll, const float n[3], float k) {
    if (k <= 0.0f) return;
    float ax[3][3];
    float r[3] = { *pitch, *yaw, *roll };
    rotator_axes(r, ax);
    float up[3] = { ax[2][0] + (n[0] - ax[2][0]) * k, ax[2][1] + (n[1] - ax[2][1]) * k, ax[2][2] + (n[2] - ax[2][2]) * k };
    float l = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (l < 1e-5f) return;
    up[0] /= l, up[1] /= l, up[2] /= l;
    float* f = ax[0];
    float d = f[0] * up[0] + f[1] * up[1] + f[2] * up[2];
    float fw[3] = { f[0] - up[0] * d, f[1] - up[1] * d, f[2] - up[2] * d };
    l = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    if (l < 1e-5f) return;
    fw[0] /= l, fw[1] /= l, fw[2] /= l;
    float rt[3] = { up[1] * fw[2] - up[2] * fw[1], up[2] * fw[0] - up[0] * fw[2], up[0] * fw[1] - up[1] * fw[0] };
    float m[3][3] = { { fw[0], fw[1], fw[2] }, { rt[0], rt[1], rt[2] }, { up[0], up[1], up[2] } };
    // (the right axis as the engine has it: the same handedness as rotator_axes' row 1)
    float d1 = m[1][0] * ax[1][0] + m[1][1] * ax[1][1] + m[1][2] * ax[1][2];
    if (d1 < 0.0f) m[1][0] = -m[1][0], m[1][1] = -m[1][1], m[1][2] = -m[1][2];
    float out[3];
    axes_rotator(m, out);
    *pitch = out[0], *yaw = out[1], *roll = out[2];
}

static PcgPoints* transform_points(Exec* x, const PcgNode* n, const PcgPoints* in) {
    PcgPoints* o = pts_new(x->r, in->n, in);
    if (!o) return NULL;
    float om[3], oM[3], rm[3], rM[3], sm[3], sM[3];
    for (int a = 0; a < 3; a++)
        om[a] = P(x, n, a), oM[a] = P(x, n, 3 + a), rm[a] = P(x, n, 6 + a), rM[a] = P(x, n, 9 + a), sm[a] = P(x, n, 12 + a), sM[a] = P(x, n, 15 + a);
    bool uniform = P(x, n, 18) != 0.0f, absolute = P(x, n, 19) != 0.0f;
    float align = clampf(P(x, n, 20), 0.0f, 1.0f);
    for (uint32_t i = 0; i < in->n; i++) {
        pts_copy(o, i, in, i);
        uint32_t s = in->seed[i];
        o->x[i] += om[0] + (oM[0] - om[0]) * rnd(x->seed, n->id, s, 0);
        o->y[i] += om[1] + (oM[1] - om[1]) * rnd(x->seed, n->id, s, 1);
        o->z[i] += om[2] + (oM[2] - om[2]) * rnd(x->seed, n->id, s, 2);
        float rr[3];
        for (int a = 0; a < 3; a++) rr[a] = rm[a] + (rM[a] - rm[a]) * rnd(x->seed, n->id, s, 3 + (uint32_t)a);
        if (absolute) o->pitch[i] = rr[0], o->yaw[i] = rr[1], o->roll[i] = rr[2];
        else o->pitch[i] += rr[0], o->yaw[i] += rr[1], o->roll[i] += rr[2];
        float u = rnd(x->seed, n->id, s, 6);
        float sc[3];
        for (int a = 0; a < 3; a++) sc[a] = uniform ? sm[0] + (sM[0] - sm[0]) * u : sm[a] + (sM[a] - sm[a]) * rnd(x->seed, n->id, s, 7 + (uint32_t)a);
        o->sx[i] *= sc[0], o->sy[i] *= sc[1], o->sz[i] *= sc[2];
        float nn[3] = { in->nx[i], in->ny[i], in->nz[i] };
        align_rot(&o->pitch[i], &o->yaw[i], &o->roll[i], nn, align);
    }
    o->n = in->n;
    return o;
}

static PcgPoints* project_points(Exec* x, const PcgNode* n, const PcgPoints* in) {
    int target = (int)P(x, n, 0);
    float off = P(x, n, 1), align = clampf(P(x, n, 2), 0.0f, 1.0f);
    const MeshGrid* mg = grid_for(x, target);
    PcgPoints* o = pts_new(x->r, in->n, in);
    if (!o) return NULL;
    for (uint32_t i = 0; i < in->n; i++) {
        float z, nn[3];
        if (!surface_at(mg, target, in->x[i], in->y[i], in->z[i] + 1e6f, &z, nn)) continue;
        uint32_t j = o->n++;
        pts_copy(o, j, in, i);
        o->z[j] = z + off, o->nx[j] = nn[0], o->ny[j] = nn[1], o->nz[j] = nn[2];
        align_rot(&o->pitch[j], &o->yaw[j], &o->roll[j], nn, align);
    }
    return o;
}

// --- Pruning: a grid of the kept points (3D cells of 2x the largest radius, 3D distance). Dense (a head per cell,
// the x neighbors side by side in memory) when the points' box fits in 16 cells a point; else a hash of the cells ---
typedef struct HGrid {
    float cell, o[3];
    int32_t n[3];
    bool dense;
    int32_t* head;  // dense: per cell; hash: per slot
    int64_t* key;   // (hash)
    uint32_t mask;  // (hash)
    int32_t* next;  // per point put
    float (*pk)[4]; // the points put, packed in the order they came (x, y, z, radius): the neighbors' data side by side
    uint32_t npk;
} HGrid;
static bool hgrid_init(PcgRun* r, HGrid* h, const PcgPoints* p, uint32_t n, float cell) {
    memset(h, 0, sizeof *h);
    h->cell = fmaxf(cell, 1.0f);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; i++) {
        lo[0] = fminf(lo[0], p->x[i]), lo[1] = fminf(lo[1], p->y[i]), lo[2] = fminf(lo[2], p->z[i]);
        hi[0] = fmaxf(hi[0], p->x[i]), hi[1] = fmaxf(hi[1], p->y[i]), hi[2] = fmaxf(hi[2], p->z[i]);
    }
    double cells = 1.0;
    for (int a = 0; a < 3; a++) {
        h->o[a] = (n ? lo[a] : 0.0f) - h->cell * 1.5f;
        double c = n ? ((double)hi[a] - lo[a]) / h->cell + 4.0 : 4.0;
        h->n[a] = (int32_t)fmin(c, 1e8), cells *= c;
    }
    h->next = re_arena_alloc(r->arena, sizeof(int32_t) * (n ? n : 1));
    h->pk = re_arena_alloc(r->arena, sizeof(float) * 4 * (n ? n : 1));
    if (!h->next || !h->pk) return false;
    h->dense = cells <= 16.0 * n + 64.0 && cells < 64e6;
    if (h->dense) {
        size_t nc = (size_t)h->n[0] * h->n[1] * h->n[2];
        h->head = re_arena_alloc(r->arena, sizeof(int32_t) * nc);
        if (!h->head) return false;
        memset(h->head, 0xFF, sizeof(int32_t) * nc);
        return true;
    }
    uint32_t cap = 64;
    while (cap < n * 2u) cap <<= 1;
    h->mask = cap - 1;
    h->head = re_arena_alloc(r->arena, sizeof(int32_t) * cap), h->key = re_arena_alloc(r->arena, sizeof(int64_t) * cap);
    if (!h->head || !h->key) return false;
    memset(h->head, 0xFF, sizeof(int32_t) * cap);
    return true;
}
static int64_t hkey(int32_t cx, int32_t cy, int32_t cz) { return ((int64_t)cx << 42) ^ ((int64_t)(cy & 0x1FFFFF) << 21) ^ (int64_t)(cz & 0x1FFFFF); }
static int32_t* hcell(const HGrid* h, int32_t cx, int32_t cy, int32_t cz, bool make) {
    if (h->dense) {
        if (cx < 0 || cy < 0 || cz < 0 || cx >= h->n[0] || cy >= h->n[1] || cz >= h->n[2]) return NULL;
        return &h->head[((size_t)cz * h->n[1] + cy) * h->n[0] + cx];
    }
    int64_t k = hkey(cx, cy, cz);
    uint32_t s = mix32((uint32_t)k ^ mix32((uint32_t)(k >> 32))) & h->mask;
    while (h->head[s] >= 0 && h->key[s] != k) s = (s + 1) & h->mask;
    if (h->head[s] < 0 && !make) return NULL;
    h->key[s] = k;
    return &h->head[s];
}
static void hgrid_put(HGrid* h, float x, float y, float z, float r) {
    int32_t* c = hcell(h, (int32_t)floorf((x - h->o[0]) / h->cell), (int32_t)floorf((y - h->o[1]) / h->cell), (int32_t)floorf((z - h->o[2]) / h->cell), true);
    if (!c) return;
    uint32_t k = h->npk++;
    h->pk[k][0] = x, h->pk[k][1] = y, h->pk[k][2] = z, h->pk[k][3] = r;
    h->next[k] = *c, *c = (int32_t)k;
}
// a point put near (x, y, z) within r + its own radius (at most rmax); true: touches. Only the cells that reach covers
// (a small point: 8 cells, not 27)
static bool hgrid_touch(const HGrid* h, float x, float y, float z, float r, float rmax) {
    float reach = r + rmax, inv = 1.0f / h->cell;
    int32_t x0 = (int32_t)floorf((x - reach - h->o[0]) * inv), x1 = (int32_t)floorf((x + reach - h->o[0]) * inv);
    int32_t y0 = (int32_t)floorf((y - reach - h->o[1]) * inv), y1 = (int32_t)floorf((y + reach - h->o[1]) * inv);
    int32_t z0 = (int32_t)floorf((z - reach - h->o[2]) * inv), z1 = (int32_t)floorf((z + reach - h->o[2]) * inv);
    for (int32_t cz = z0; cz <= z1; cz++)
        for (int32_t cy = y0; cy <= y1; cy++)
            for (int32_t cx = x0; cx <= x1; cx++) {
                const int32_t* c = hcell(h, cx, cy, cz, false);
                for (int32_t j = c ? *c : -1; j >= 0; j = h->next[j]) {
                    const float* q = h->pk[j];
                    float ex = q[0] - x, ey = q[1] - y, ez = q[2] - z, rr = r + q[3];
                    if (ex * ex + ey * ey + ez * ez < rr * rr) return true;
                }
            }
    return false;
}

// LSD radix sort of 64-bit keys carrying an index
static void radix64(uint64_t* k, uint32_t* v, uint64_t* tk, uint32_t* tv, uint32_t n) {
    for (int pass = 0; pass < 8; pass++) {
        uint32_t cnt[257] = { 0 };
        int sh = pass * 8;
        for (uint32_t i = 0; i < n; i++) cnt[((k[i] >> sh) & 255u) + 1]++;
        bool trivial = false;
        for (int b = 0; b < 256; b++) trivial |= cnt[b + 1] == n;
        if (trivial) continue;
        for (int b = 0; b < 256; b++) cnt[b + 1] += cnt[b];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t d = cnt[(k[i] >> sh) & 255u]++;
            tk[d] = k[i], tv[d] = v[i];
        }
        memcpy(k, tk, sizeof(uint64_t) * n), memcpy(v, tv, sizeof(uint32_t) * n);
    }
}
static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u; // (radii are >= 0: the bits order as the values)
}

static PcgPoints* self_prune(Exec* x, const PcgNode* n, const PcgPoints* in) {
    int mode = (int)P(x, n, 0);
    float mult = fmaxf(P(x, n, 1), 0.0f);
    uint32_t N = in->n;
    float* rad = farr(x->r, N);
    uint64_t *k = re_arena_alloc(x->r->arena, sizeof(uint64_t) * (N + 1)), *tk = re_arena_alloc(x->r->arena, sizeof(uint64_t) * (N + 1));
    uint32_t *v = re_arena_alloc(x->r->arena, sizeof(uint32_t) * (N + 1)), *tv = re_arena_alloc(x->r->arena, sizeof(uint32_t) * (N + 1));
    uint8_t* keep = re_arena_alloc(x->r->arena, N + 1);
    HGrid h;
    float rmax = 0.0f;
    if (!rad || !k || !tk || !v || !tv || !keep) return fail(x->r, "out of memory (the arena)"), NULL;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < N; i++) {
        rad[i] = in->ext[i] * fmaxf(fabsf(in->sx[i]), fabsf(in->sy[i])) * mult, rmax = fmaxf(rmax, rad[i]);
        lo[0] = fminf(lo[0], in->x[i]), lo[1] = fminf(lo[1], in->y[i]), lo[2] = fminf(lo[2], in->z[i]);
        hi[0] = fmaxf(hi[0], in->x[i]), hi[1] = fmaxf(hi[1], in->y[i]), hi[2] = fmaxf(hi[2], in->z[i]);
    }
    // The order the points are tried in (Unreal's Radius Similarity Factor): radii within 25 % of each other are one
    // class, the largest class first (or the smallest); inside a class, along a Morton curve of the points' place (the
    // neighbors' cells stay in the cache: 4x faster than a random order), ties by their seeds. Random: by a hash
    float span = fmaxf(fmaxf(hi[0] - lo[0], hi[1] - lo[1]), fmaxf(hi[2] - lo[2], 1.0f));
    for (uint32_t i = 0; i < N; i++) {
        uint32_t primary;
        if (mode == 2) primary = mix32(hash3u(x->seed, n->id, in->seed[i]));
        else {
            int b = rad[i] > 1e-3f ? (int)floorf(logf(rad[i]) * 4.48142f) + 64 : 0; // (log base 1.25)
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            uint32_t m = 0, q[3] = { (uint32_t)((in->x[i] - lo[0]) / span * 255.0f), (uint32_t)((in->y[i] - lo[1]) / span * 255.0f),
                                     (uint32_t)((in->z[i] - lo[2]) / span * 255.0f) };
            for (int bit = 7; bit >= 0; bit--) m = (m << 3) | ((q[0] >> bit & 1u) << 2) | ((q[1] >> bit & 1u) << 1) | (q[2] >> bit & 1u);
            primary = ((uint32_t)(mode == 0 ? 255 - b : b) << 24) | m;
        }
        k[i] = ((uint64_t)primary << 32) | in->seed[i], v[i] = i;
    }
    radix64(k, v, tk, tv, N);
    if (!hgrid_init(x->r, &h, in, N, 2.0f * rmax)) return fail(x->r, "out of memory (the arena)"), NULL;
    memset(keep, 0, N);
    uint32_t kept = 0;
    for (uint32_t s = 0; s < N; s++) {
        uint32_t i = v[s];
        if (hgrid_touch(&h, in->x[i], in->y[i], in->z[i], rad[i], rmax)) continue;
        keep[i] = 1, kept++;
        hgrid_put(&h, in->x[i], in->y[i], in->z[i], rad[i]);
    }
    PcgPoints* o = pts_new(x->r, kept, in); // (the kept ones in the input's order)
    if (!o) return NULL;
    for (uint32_t i = 0; i < N; i++)
        if (keep[i]) pts_copy(o, o->n++, in, i);
    return o;
}

static PcgPoints* difference(Exec* x, const PcgNode* n, const PcgPoints* src, const PcgPoints* dif) {
    float mult = fmaxf(P(x, n, 0), 0.0f), rmax = 0.0f, smax = 0.0f;
    float* rd = farr(x->r, dif->n);
    PcgPoints* o = pts_new(x->r, src->n, src);
    if (!rd || !o) return NULL;
    for (uint32_t i = 0; i < dif->n; i++) rd[i] = dif->ext[i] * fmaxf(fabsf(dif->sx[i]), fabsf(dif->sy[i])) * mult, rmax = fmaxf(rmax, rd[i]);
    for (uint32_t i = 0; i < src->n; i++) smax = fmaxf(smax, src->ext[i] * fmaxf(fabsf(src->sx[i]), fabsf(src->sy[i])) * mult);
    HGrid h;
    if (!hgrid_init(x->r, &h, dif, dif->n, rmax + smax)) return fail(x->r, "out of memory (the arena)"), NULL;
    for (uint32_t i = 0; i < dif->n; i++) hgrid_put(&h, dif->x[i], dif->y[i], dif->z[i], rd[i]);
    for (uint32_t i = 0; i < src->n; i++) {
        float r = src->ext[i] * fmaxf(fabsf(src->sx[i]), fabsf(src->sy[i])) * mult;
        if (dif->n && hgrid_touch(&h, src->x[i], src->y[i], src->z[i], r, rmax)) continue;
        pts_copy(o, o->n++, src, i);
    }
    return o;
}

// --- Spawners ---
static PcgSpawn* spawn_slot(PcgRun* r) {
    if (!re_vm_ensure(r->spawns, (size_t)(r->nspawns + 1) * sizeof(PcgSpawn))) return fail(r, "too many spawns"), NULL;
    return &((PcgSpawn*)r->spawns->base)[r->nspawns++];
}

static void mesh_spawner(Exec* x, int ni, const PcgNode* n, const PcgPoints* in) {
    bool every = P(x, n, 0) != 0.0f;
    uint8_t flags = (P(x, n, 1) != 0.0f ? PCG_SPAWN_COLLISION : 0) | (P(x, n, 2) != 0.0f ? PCG_SPAWN_SHADOW : 0);
    float wind = clampf(P(x, n, 3), 0.0f, 1.0f);
    uint32_t mesh[PCG_MESH_ENTRIES], mat[PCG_MESH_ENTRIES];
    float wsum = 0.0f;
    bool used[PCG_MESH_ENTRIES];
    for (int e = 0; e < PCG_MESH_ENTRIES; e++) {
        used[e] = n->s[e][0] != 0 && (every || P(x, n, 4 + e * 10) > 0.0f);
        mesh[e] = used[e] ? pcg_mesh_id(n->s[e]) : 0u, mat[e] = pcg_material_hash(n->s[4 + e]);
        if (used[e] && !every) wsum += P(x, n, 4 + e * 10);
    }
    for (uint32_t i = 0; i < in->n; i++) {
        float ax[3][3];
        float rot[3] = { in->pitch[i], in->yaw[i], in->roll[i] }, ps[3] = { in->sx[i], in->sy[i], in->sz[i] };
        rotator_axes(rot, ax);
        float pick = rnd(x->seed, n->id, in->seed[i], 0) * wsum;
        for (int e = 0; e < PCG_MESH_ENTRIES; e++) {
            if (!used[e]) continue;
            if (!every) {
                float we = P(x, n, 4 + e * 10);
                if (pick >= we && e < PCG_MESH_ENTRIES - 1) {
                    pick -= we;
                    bool later = false;
                    for (int f = e + 1; f < PCG_MESH_ENTRIES; f++) later |= used[f];
                    if (later) continue;
                }
            }
            PcgSpawn* s = spawn_slot(x->r);
            if (!s) return;
            const float* base = &n->f[4 + e * 10];
            float off[3] = { P(x, n, 8 + e * 10) * ps[0], P(x, n, 9 + e * 10) * ps[1], P(x, n, 10 + e * 10) * ps[2] };
            s->kind = PCG_SPAWN_MESH, s->flags = flags, s->node = (uint16_t)ni, s->asset = mesh[e], s->material = mat[e], s->wind = wind;
            s->loc[0] = in->x[i], s->loc[1] = in->y[i], s->loc[2] = in->z[i];
            for (int a = 0; a < 3; a++) s->loc[0] += ax[a][0] * off[a], s->loc[1] += ax[a][1] * off[a], s->loc[2] += ax[a][2] * off[a];
            memcpy(s->rot, rot, 12);
            for (int a = 0; a < 3; a++) s->scale[a] = ps[a] * P(x, n, 11 + e * 10 + a), s->color[a] = base[1 + a];
            if (!every) break;
        }
    }
}

static void actor_spawner(Exec* x, int ni, const PcgNode* n, const PcgPoints* in) {
    if (!n->s[0][0]) return;
    uint32_t cls = pcg_vpath_hash(n->s[0]);
    for (uint32_t i = 0; i < in->n; i++) {
        PcgSpawn* s = spawn_slot(x->r);
        if (!s) return;
        memset(s, 0, sizeof *s);
        s->kind = PCG_SPAWN_ACTOR, s->node = (uint16_t)ni, s->asset = cls;
        s->loc[0] = in->x[i], s->loc[1] = in->y[i], s->loc[2] = in->z[i];
        s->rot[0] = in->pitch[i], s->rot[1] = in->yaw[i], s->rot[2] = in->roll[i];
        s->scale[0] = in->sx[i], s->scale[1] = in->sy[i], s->scale[2] = in->sz[i];
    }
}

static const PcgGraph* cached_graph(const char* vpath); // (pcg_world.c prepares them on the main thread)
static bool exec_graph(Exec* x);

static void child_params(const Exec* x, const PcgGraph* cg, float out[PCG_MAX_PARAMS]) {
    for (uint32_t i = 0; i < cg->nparams; i++) {
        out[i] = cg->params[i].value;
        int k = pcg_graph_param(x->g, cg->params[i].name);
        if (k >= 0) out[i] = x->params[k];
    }
}

static PcgPoints* nested(Exec* x, const PcgNode* n, const PcgPoints* in) {
    const PcgGraph* cg = cached_graph(n->s[0]);
    if (!cg || x->depth >= 4) return (PcgPoints*)in;
    float bs = P(x, n, 0);
    uint32_t maxp = (uint32_t)fmaxf(P(x, n, 1), 1.0f);
    for (uint32_t i = 0; i < in->n && i < maxp; i++) {
        Exec c = { 0 };
        c.r = x->r, c.g = cg, c.depth = x->depth + 1, c.input = &g_empty, c.seed = mix32(x->seed ^ in->seed[i]);
        float b = bs > 0.0f ? bs : in->ext[i] * fmaxf(fmaxf(in->sx[i], in->sy[i]), in->sz[i]);
        float p[3] = { in->x[i], in->y[i], in->z[i] };
        for (int a = 0; a < 3; a++) c.bmin[a] = p[a] - b, c.bmax[a] = p[a] + b;
        c.bmin[2] = p[2] - fmaxf(b, 5000.0f), c.bmax[2] = p[2] + fmaxf(b, 5000.0f); // (the ground under a slope: tall boxes)
        child_params(x, cg, c.params);
        if (!exec_graph(&c)) return NULL;
    }
    return (PcgPoints*)in;
}

static PcgPoints* subgraph(Exec* x, const PcgNode* n, const PcgPoints* in) {
    const PcgGraph* cg = cached_graph(n->s[0]);
    if (!cg || x->depth >= 4) return (PcgPoints*)in;
    Exec c = *x;
    c.g = cg, c.depth = x->depth + 1, c.input = in, c.output = NULL, c.seed = mix32(x->seed ^ n->id);
    child_params(x, cg, c.params);
    if (!exec_graph(&c)) return NULL;
    return c.output ? c.output : pts_new(x->r, 0, in);
}

// --- Debug points ---
static void debug_add(PcgRun* r, const PcgPoints* p, uint32_t tint) {
    if (!r->debug || !p) return;
    uint32_t n = p->n;
    if (r->ndebug + n > 400000u) n = r->ndebug < 400000u ? 400000u - r->ndebug : 0u;
    if (!n || !re_vm_ensure(r->debug, (size_t)(r->ndebug + n) * sizeof(PcgDebugPoint))) return;
    PcgDebugPoint* d = (PcgDebugPoint*)r->debug->base + r->ndebug;
    for (uint32_t i = 0; i < n; i++) {
        d[i].p[0] = p->x[i], d[i].p[1] = p->y[i], d[i].p[2] = p->z[i] + 20.0f;
        float v = clampf(p->density[i], 0.0f, 1.0f);
        uint32_t lo = tint & 0xFFu, g = (uint32_t)(80.0f + 175.0f * v);
        d[i].color = 0xFF000000u | (lo ? (tint & 0x00FFFFFFu) : ((uint32_t)(255.0f * (1.0f - v)) | (g << 8) | (40u << 16)));
    }
    r->ndebug += n;
}

// --- The graph ---
static bool exec_graph(Exec* x) {
    const PcgGraph* g = x->g;
    PcgRun* r = x->r;
    uint32_t N = g->nnodes;
    PcgPoints* outs[PCG_MAX_NODES][2] = { { 0 } };
    uint8_t indeg[PCG_MAX_NODES] = { 0 }, order[PCG_MAX_NODES], done[PCG_MAX_NODES] = { 0 };
    for (uint32_t i = 0; i < g->nlinks; i++) indeg[g->links[i].to]++;
    uint32_t no = 0, head = 0;
    for (uint32_t i = 0; i < N; i++)
        if (!indeg[i]) order[no++] = (uint8_t)i;
    while (head < no) {
        uint32_t k = order[head++];
        for (uint32_t i = 0; i < g->nlinks; i++)
            if (g->links[i].from == k && --indeg[g->links[i].to] == 0) order[no++] = g->links[i].to;
    }
    if (no < N) return fail(r, "the graph has a loop"), false;
    bool any_debug = false;
    for (uint32_t i = 0; i < N; i++) any_debug |= (g->nodes[i].flags & PCGNF_DEBUG) != 0;
    for (uint32_t oi = 0; oi < N; oi++) {
        int ni = order[oi];
        const PcgNode* n = &g->nodes[ni];
        const PcgNodeInfo* info = pcg_node_info(n->type);
        double t0 = x->depth == 0 ? re_time_seconds() : 0.0;
        // the inputs (several wires into one pin: merged in the links' order)
        const PcgPoints* in[PCG_MAX_PINS];
        for (int p = 0; p < info->nin; p++) {
            const PcgPoints* list[PCG_MAX_LINKS > 16 ? 16 : PCG_MAX_LINKS];
            int nl = 0;
            for (uint32_t i = 0; i < g->nlinks && nl < 16; i++) {
                const PcgLink* l = &g->links[i];
                if (l->to == ni && l->to_pin == p && outs[l->from][l->from_pin]) list[nl++] = outs[l->from][l->from_pin];
            }
            in[p] = nl == 0 ? &g_empty : nl == 1 ? list[0] : pts_merge(r, list, nl);
            if (!in[p]) return false;
        }
        PcgPoints* out[2] = { NULL, NULL };
        if (n->flags & PCGNF_DISABLED) {
            out[0] = info->nin ? (PcgPoints*)in[0] : pts_new(r, 0, NULL);
        } else {
            switch (n->type) {
            case PCGN_INPUT: out[0] = (PcgPoints*)(x->input ? x->input : &g_empty); break;
            case PCGN_OUTPUT: x->output = (PcgPoints*)in[0]; break;
            case PCGN_SURFACE: out[0] = surface_sampler(x, n); break;
            case PCGN_VOLUME: out[0] = volume_sampler(x, n); break;
            case PCGN_SPLINE: out[0] = spline_sampler(x, n); break;
            case PCGN_ATTR_FILTER: case PCGN_DENSITY_FILTER: case PCGN_STEEPNESS_FILTER: case PCGN_LAYER_FILTER: {
                const PcgPoints* a = in[0];
                uint8_t* keep = re_arena_alloc(r->arena, a->n + 1);
                if (!keep) return fail(r, "out of memory (the arena)"), false;
                if (n->type == PCGN_ATTR_FILTER) {
                    int w = attr_which(a, n->s[0][0] ? n->s[0] : "Density"), op = (int)P(x, n, 0);
                    float th = P(x, n, 1);
                    for (uint32_t i = 0; i < a->n; i++) {
                        float v = w == -100 ? 0.0f : attr_get(a, w, i);
                        bool k = op == 0 ? v < th : op == 1 ? v <= th : op == 2 ? v > th : op == 3 ? v >= th : op == 4 ? fabsf(v - th) < 1e-6f : fabsf(v - th) >= 1e-6f;
                        keep[i] = k;
                    }
                } else if (n->type == PCGN_DENSITY_FILTER) {
                    float lo = P(x, n, 0), hi = P(x, n, 1);
                    bool inv = P(x, n, 2) != 0.0f;
                    for (uint32_t i = 0; i < a->n; i++) keep[i] = (a->density[i] >= lo && a->density[i] <= hi) != inv;
                } else if (n->type == PCGN_STEEPNESS_FILTER) {
                    float lo = P(x, n, 0), hi = P(x, n, 1);
                    for (uint32_t i = 0; i < a->n; i++) {
                        float sl = acosf(clampf(a->nz[i], -1.0f, 1.0f)) / DEG;
                        keep[i] = sl >= lo && sl <= hi;
                    }
                } else {
                    int layer = (int)P(x, n, 0);
                    float lo = P(x, n, 1), hi = P(x, n, 2);
                    for (uint32_t i = 0; i < a->n; i++) {
                        float wv = landscape_layer_at(a->x[i], a->y[i], layer);
                        keep[i] = wv >= lo && wv <= hi;
                    }
                }
                if (!split(x, a, keep, out)) return false;
                break;
            }
            case PCGN_DENSITY_NOISE: case PCGN_SPATIAL_NOISE: {
                const PcgPoints* a = in[0];
                PcgPoints* o = pts_new(r, a->n, a);
                if (!o) return false;
                int mode = (int)P(x, n, 0);
                float scale = fmaxf(P(x, n, 1), 1.0f), contrast = P(x, n, 3), bias = P(x, n, 4);
                int oct = (int)clampf(P(x, n, 2), 1.0f, 6.0f);
                for (uint32_t i = 0; i < a->n; i++) {
                    pts_copy(o, i, a, i);
                    float v;
                    if (n->type == PCGN_DENSITY_NOISE) v = P(x, n, 1) + (P(x, n, 2) - P(x, n, 1)) * rnd(x->seed, n->id, a->seed[i], 0);
                    else {
                        float sum = 0.0f, amp = 1.0f, norm = 0.0f, fx = a->x[i] / scale, fy = a->y[i] / scale;
                        for (int k = 0; k < oct; k++) {
                            sum += vnoise(fx, fy, hash3u(x->seed, n->id, (uint32_t)k)) * amp, norm += amp;
                            amp *= 0.5f, fx *= 2.03f, fy *= 2.03f;
                        }
                        v = clampf((sum / norm - 0.5f) * contrast + 0.5f + bias, 0.0f, 1.0f);
                    }
                    o->density[i] = clampf(combine(mode, a->density[i], v), 0.0f, 1.0f);
                }
                o->n = a->n;
                out[0] = o;
                break;
            }
            case PCGN_CREATE_ATTR: {
                const PcgPoints* a = in[0];
                PcgPoints like = *a;
                const char* nm = n->s[0][0] ? n->s[0] : "Attribute";
                int k = attr_index(a, nm);
                if (k < 0 && like.nattr < PCG_MAX_ATTRS) snprintf(like.attr_name[like.nattr], PCG_ATTR_NAME, "%s", nm), k = (int)like.nattr++;
                PcgPoints* o = pts_new(r, a->n, &like);
                if (!o) return false;
                float lo = P(x, n, 0), hi = P(x, n, 1);
                for (uint32_t i = 0; i < a->n; i++) {
                    pts_copy(o, i, a, i);
                    if (k >= 0) o->attr[k][i] = lo + (hi - lo) * rnd(x->seed, n->id, a->seed[i], 0);
                }
                o->n = a->n;
                out[0] = o;
                break;
            }
            case PCGN_TRANSFORM: out[0] = transform_points(x, n, in[0]); break;
            case PCGN_PROJECT: out[0] = project_points(x, n, in[0]); break;
            case PCGN_SELF_PRUNE: out[0] = in[0]->n ? self_prune(x, n, in[0]) : (PcgPoints*)in[0]; break;
            case PCGN_DIFFERENCE: out[0] = difference(x, n, in[0], in[1]); break;
            case PCGN_MERGE: out[0] = pts_merge(r, in, 4); break;
            case PCGN_MESH_SPAWNER: mesh_spawner(x, ni, n, in[0]), out[0] = (PcgPoints*)in[0]; break;
            case PCGN_ACTOR_SPAWNER: actor_spawner(x, ni, n, in[0]), out[0] = (PcgPoints*)in[0]; break;
            case PCGN_NESTED: out[0] = nested(x, n, in[0]); break;
            case PCGN_SUBGRAPH: out[0] = subgraph(x, n, in[0]); break;
            default: out[0] = pts_new(r, 0, NULL); break;
            }
            if (r->error[0]) return false;
            if (info->nout && !out[0]) return fail(r, "a node failed"), false;
        }
        outs[ni][0] = out[0], outs[ni][1] = out[1];
        done[ni] = 1;
        uint32_t made = (out[0] ? out[0]->n : 0) + (out[1] ? out[1]->n : 0);
        r->points_made += made;
        bool spawner = n->type == PCGN_MESH_SPAWNER || n->type == PCGN_ACTOR_SPAWNER || n->type == PCGN_NESTED;
        if (spawner || n->type == PCGN_OUTPUT) r->points_out += in[0]->n;
        if (x->depth == 0) {
            r->node_points[ni] = made ? made : (info->nin ? in[0]->n : 0);
            r->node_ms[ni] = (float)((re_time_seconds() - t0) * 1000.0);
            if (n->flags & PCGNF_DEBUG) debug_add(r, out[0] ? out[0] : in[0], 0);
            else if (!any_debug && r->debug_all && spawner) debug_add(r, in[0], 0);
        }
    }
    (void)done;
    return true;
}

// The graphs a run may reach: read on the main thread before (no file is touched on a worker)
static const PcgGraph* cached_graph(const char* vpath) {
    return vpath && vpath[0] ? pcg_graph_cached(pcg_vpath_hash(vpath)) : NULL;
}

bool pcg_execute(const PcgGraph* g, PcgRun* run) {
    if (!g || !run || !run->arena) return false;
    run->error[0] = 0, run->points_made = run->points_out = 0, run->grid = NULL;
    memset(run->node_points, 0, sizeof run->node_points), memset(run->node_ms, 0, sizeof run->node_ms);
    Exec x = { 0 };
    x.r = run, x.g = g, x.seed = run->seed, x.input = &g_empty, x.depth = run->depth;
    memcpy(x.bmin, run->bmin, 12), memcpy(x.bmax, run->bmax, 12);
    memcpy(x.params, run->params, sizeof x.params);
    return exec_graph(&x);
}

uint64_t pcg_spawns_hash(const PcgSpawn* s, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = (const uint8_t*)s;
    for (size_t i = 0; i < (size_t)n * sizeof(PcgSpawn); i++) h = (h ^ b[i]) * 1099511628211ull;
    return h ^ n;
}
