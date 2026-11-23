// The NavMesh's tile builder (navmesh_internal.h): Recast's pipeline, written here from its published algorithms.
// Inside, the coordinates are Recast's: x and z across (z is the world's y), y up, in cells (cs) and heights (ch) from
// the tile's corner with its border. Nothing here touches the world: any thread.
#include "navmesh_internal.h"
#include "navmesh.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define NULL_AREA 0
#define NOT_CON 0xFFu
#define BORDER_REG 0x8000u
#define MAX_H 0xFFFF
#define BORDER_VERTEX 0x10000
#define AREA_BORDER 0x20000
#define REG_MASK 0xFFFF
#define NULL_IDX 0xFFFFu

static const int DX[4] = { -1, 0, 1, 0 }, DZ[4] = { 0, 1, 0, -1 }; // Recast's directions: -x, +z, +x, -z

// --- small growable arrays (a build, not a frame) ---
typedef struct IVec { int* a; int n, cap; bool bad; } IVec;
static void iv_reserve(IVec* v, int n) {
    if (n <= v->cap || v->bad) return;
    int c = v->cap ? v->cap : 64;
    while (c < n) c *= 2;
    int* na = realloc(v->a, sizeof(int) * (size_t)c);
    if (!na) { v->bad = true; return; }
    v->a = na, v->cap = c;
}
static void iv_push(IVec* v, int x) {
    iv_reserve(v, v->n + 1);
    if (!v->bad) v->a[v->n++] = x;
}
static void iv_push4(IVec* v, int a, int b, int c, int d) { iv_push(v, a), iv_push(v, b), iv_push(v, c), iv_push(v, d); }

// --- The heightfield: solid spans per column ---
typedef struct HSpan { uint16_t smin, smax; uint8_t area; uint32_t next; } HSpan; // next: index + 1 (0: none)
typedef struct Hf {
    int w, h;
    uint32_t* cols; // the column's lowest span + 1
    HSpan* sp;
    uint32_t n, cap, free_list;
    bool bad;
} Hf;

static uint32_t span_alloc(Hf* hf) {
    if (hf->free_list) {
        uint32_t i = hf->free_list - 1;
        hf->free_list = hf->sp[i].next;
        return i + 1;
    }
    if (hf->n == hf->cap) {
        uint32_t c = hf->cap ? hf->cap * 2 : 4096;
        HSpan* ns = realloc(hf->sp, sizeof(HSpan) * c);
        if (!ns) { hf->bad = true; return 0; }
        hf->sp = ns, hf->cap = c;
    }
    return ++hf->n;
}
static void span_free(Hf* hf, uint32_t s) { hf->sp[s - 1].next = hf->free_list, hf->free_list = s; }

// Recast's addSpan: merged with the spans it touches; the top's area wins (the higher id within the climb)
static void add_span(Hf* hf, int x, int z, uint16_t smin, uint16_t smax, uint8_t area, int merge_thr) {
    uint32_t si = span_alloc(hf);
    if (!si) return;
    HSpan* s = &hf->sp[si - 1];
    s->smin = smin, s->smax = smax, s->area = area, s->next = 0;
    uint32_t* col = &hf->cols[x + z * hf->w];
    uint32_t prev = 0, cur = *col;
    while (cur) {
        HSpan* c = &hf->sp[cur - 1];
        if (c->smin > s->smax) break;
        if (c->smax < s->smin) {
            prev = cur, cur = c->next;
            continue;
        }
        if (c->smin < s->smin) s->smin = c->smin;
        if (c->smax > s->smax) s->smax = c->smax;
        if (abs((int)s->smax - (int)c->smax) <= merge_thr && c->area > s->area) s->area = c->area;
        uint32_t next = c->next;
        span_free(hf, cur);
        if (prev) hf->sp[prev - 1].next = next;
        else *col = next;
        cur = next;
    }
    if (prev) s->next = hf->sp[prev - 1].next, hf->sp[prev - 1].next = si;
    else s->next = *col, *col = si;
}

// A box (what blocks the Character: an axis-aligned box) into every column it covers; its top is flat: walkable
static void rast_box(Hf* hf, const NavInBox* b, const float bmin[3], float cs, float ch, float yrange, int climb) {
    float lo = b->mn[2] - bmin[2], hi = b->mx[2] - bmin[2];
    if (hi < 0.0f || lo > yrange) return;
    int x0 = (int)floorf((b->mn[0] - bmin[0]) / cs), x1 = (int)ceilf((b->mx[0] - bmin[0]) / cs) - 1;
    int z0 = (int)floorf((b->mn[1] - bmin[1]) / cs), z1 = (int)ceilf((b->mx[1] - bmin[1]) / cs) - 1;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= hf->w) x1 = hf->w - 1;
    if (z1 >= hf->h) z1 = hf->h - 1;
    if (x0 > x1 || z0 > z1) return;
    if (lo < 0.0f) lo = 0.0f;
    if (hi > yrange) hi = yrange;
    int smin = (int)floorf(lo / ch), smax = (int)ceilf(hi / ch);
    if (smin > MAX_H - 1) smin = MAX_H - 1;
    if (smax <= smin) smax = smin + 1;
    if (smax > MAX_H) smax = MAX_H;
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) add_span(hf, x, z, (uint16_t)smin, (uint16_t)smax, 1, climb);
}

// Recast's dividePoly: the part at or below x on the axis, and the rest
static void divide_poly(const float* in, int nin, float* out1, int* nout1, float* out2, int* nout2, float x, int axis) {
    float d[12];
    for (int i = 0; i < nin; i++) d[i] = x - in[i * 3 + axis];
    int m = 0, n = 0;
    for (int i = 0, j = nin - 1; i < nin; j = i, i++) {
        bool ina = d[j] >= 0, inb = d[i] >= 0;
        if (ina != inb) {
            float s = d[j] / (d[j] - d[i]);
            for (int k = 0; k < 3; k++) out1[m * 3 + k] = out2[n * 3 + k] = in[j * 3 + k] + (in[i * 3 + k] - in[j * 3 + k]) * s;
            m++, n++;
            if (d[i] > 0) memcpy(&out1[m * 3], &in[i * 3], 12), m++;
            else if (d[i] < 0) memcpy(&out2[n * 3], &in[i * 3], 12), n++;
        } else {
            if (d[i] >= 0) {
                memcpy(&out1[m * 3], &in[i * 3], 12), m++;
                if (d[i] != 0) continue;
            }
            memcpy(&out2[n * 3], &in[i * 3], 12), n++;
        }
    }
    *nout1 = m, *nout2 = n;
}

// A triangle (build space: x, y up, z) clipped into every cell it touches (Recast's rasterizeTri)
static void rast_tri(Hf* hf, const float* v0, const float* v1, const float* v2, uint8_t area, float cs, float ch, float yrange, int climb) {
    float tmin[3], tmax[3];
    for (int k = 0; k < 3; k++) tmin[k] = fminf(v0[k], fminf(v1[k], v2[k])), tmax[k] = fmaxf(v0[k], fmaxf(v1[k], v2[k]));
    if (tmax[0] < 0 || tmin[0] > hf->w * cs || tmax[2] < 0 || tmin[2] > hf->h * cs || tmax[1] < 0 || tmin[1] > yrange) return;
    int z0 = (int)(tmin[2] / cs), z1 = (int)(tmax[2] / cs);
    if (z0 < -1) z0 = -1;
    if (z0 > hf->h - 1) z0 = hf->h - 1;
    if (z1 < 0) z1 = 0;
    if (z1 > hf->h - 1) z1 = hf->h - 1;
    float buf[7 * 3 * 4];
    float *in = buf, *inrow = buf + 7 * 3, *p1 = inrow + 7 * 3, *p2 = p1 + 7 * 3;
    memcpy(in, v0, 12), memcpy(in + 3, v1, 12), memcpy(in + 6, v2, 12);
    int nvrow, nvin = 3;
    for (int z = z0; z <= z1; z++) {
        float cz = (float)z * cs;
        divide_poly(in, nvin, inrow, &nvrow, p1, &nvin, cz + cs, 2);
        float* t = in;
        in = p1, p1 = t;
        if (nvrow < 3 || z < 0) continue;
        float minx = inrow[0], maxx = inrow[0];
        for (int i = 1; i < nvrow; i++) minx = fminf(minx, inrow[i * 3]), maxx = fmaxf(maxx, inrow[i * 3]);
        int x0 = (int)(minx / cs), x1 = (int)(maxx / cs);
        if (x1 < 0 || x0 >= hf->w) continue;
        if (x0 < -1) x0 = -1;
        if (x1 > hf->w - 1) x1 = hf->w - 1;
        int nv, nv2 = nvrow;
        for (int x = x0; x <= x1; x++) {
            float cx = (float)x * cs;
            divide_poly(inrow, nv2, p1, &nv, p2, &nv2, cx + cs, 0);
            t = inrow, inrow = p2, p2 = t;
            if (nv < 3 || x < 0) continue;
            float smin = p1[1], smax = p1[1];
            for (int i = 1; i < nv; i++) smin = fminf(smin, p1[i * 3 + 1]), smax = fmaxf(smax, p1[i * 3 + 1]);
            if (smax < 0.0f || smin > yrange) continue;
            if (smin < 0.0f) smin = 0.0f;
            if (smax > yrange) smax = yrange;
            int is0 = (int)floorf(smin / ch), is1 = (int)ceilf(smax / ch);
            if (is0 > MAX_H - 1) is0 = MAX_H - 1;
            if (is1 <= is0) is1 = is0 + 1;
            if (is1 > MAX_H) is1 = MAX_H;
            add_span(hf, x, z, (uint16_t)is0, (uint16_t)is1, area, climb);
        }
    }
}

// --- The filters (Recast's) ---
static void filter_low_hanging(Hf* hf, int climb) { // a curb on a walkable floor is walkable
    for (int c = 0; c < hf->w * hf->h; c++) {
        bool prev_walk = false;
        uint8_t prev_area = NULL_AREA;
        uint32_t ps = 0;
        for (uint32_t s = hf->cols[c]; s; ps = s, s = hf->sp[s - 1].next) {
            HSpan* sp = &hf->sp[s - 1];
            bool walk = sp->area != NULL_AREA;
            if (!walk && prev_walk && abs((int)sp->smax - (int)hf->sp[ps - 1].smax) <= climb) sp->area = prev_area;
            prev_walk = walk, prev_area = sp->area;
        }
    }
}

static void filter_ledges(Hf* hf, int height, int climb) { // next to a drop (more than the climb): not walkable
    for (int z = 0; z < hf->h; z++)
        for (int x = 0; x < hf->w; x++)
            for (uint32_t s = hf->cols[x + z * hf->w]; s; s = hf->sp[s - 1].next) {
                HSpan* sp = &hf->sp[s - 1];
                if (sp->area == NULL_AREA) continue;
                int bot = sp->smax, top = sp->next ? hf->sp[sp->next - 1].smin : MAX_H;
                int minh = MAX_H, asmin = sp->smax, asmax = sp->smax;
                for (int d = 0; d < 4; d++) {
                    int nx = x + DX[d], nz = z + DZ[d];
                    if (nx < 0 || nz < 0 || nx >= hf->w || nz >= hf->h) {
                        minh = minh < -climb - bot ? minh : -climb - bot;
                        continue;
                    }
                    uint32_t ns = hf->cols[nx + nz * hf->w];
                    int nbot = -climb, ntop = ns ? hf->sp[ns - 1].smin : MAX_H;
                    if ((top < ntop ? top : ntop) - (bot > nbot ? bot : nbot) > height) minh = minh < nbot - bot ? minh : nbot - bot;
                    for (; ns; ns = hf->sp[ns - 1].next) {
                        const HSpan* n = &hf->sp[ns - 1];
                        nbot = n->smax, ntop = n->next ? hf->sp[n->next - 1].smin : MAX_H;
                        if ((top < ntop ? top : ntop) - (bot > nbot ? bot : nbot) > height) {
                            minh = minh < nbot - bot ? minh : nbot - bot;
                            if (abs(nbot - bot) <= climb) {
                                if (nbot < asmin) asmin = nbot;
                                if (nbot > asmax) asmax = nbot;
                            }
                        }
                    }
                }
                if (minh < -climb || asmax - asmin > climb) sp->area = NULL_AREA;
            }
}

static void filter_low_height(Hf* hf, int height) { // too little head room
    for (int c = 0; c < hf->w * hf->h; c++)
        for (uint32_t s = hf->cols[c]; s; s = hf->sp[s - 1].next) {
            HSpan* sp = &hf->sp[s - 1];
            int top = sp->next ? hf->sp[sp->next - 1].smin : MAX_H;
            if (top - (int)sp->smax <= height) sp->area = NULL_AREA;
        }
}

// --- The compact heightfield: the open space over each walkable span, linked to its four neighbors ---
typedef struct CCell { uint32_t index, count; } CCell;
typedef struct CSpan { uint16_t y, h, reg; uint8_t con[4]; } CSpan;
typedef struct Chf {
    int w, h, border;
    uint32_t nspans;
    CCell* cells;
    CSpan* spans;
    uint8_t* areas;
} Chf;

static inline uint32_t con_index(const Chf* c, int x, int z, const CSpan* s, int d) {
    return c->cells[(x + DX[d]) + (z + DZ[d]) * c->w].index + s->con[d];
}

static bool build_compact(const Hf* hf, int height, int climb, Chf* c) {
    c->w = hf->w, c->h = hf->h;
    uint32_t n = 0;
    for (int i = 0; i < hf->w * hf->h; i++)
        for (uint32_t s = hf->cols[i]; s; s = hf->sp[s - 1].next) n += hf->sp[s - 1].area != NULL_AREA;
    c->nspans = n;
    c->cells = calloc((size_t)hf->w * hf->h, sizeof(CCell));
    c->spans = calloc(n ? n : 1, sizeof(CSpan));
    c->areas = calloc(n ? n : 1, 1);
    if (!c->cells || !c->spans || !c->areas) return false;
    uint32_t idx = 0;
    for (int i = 0; i < hf->w * hf->h; i++) {
        c->cells[i].index = idx;
        for (uint32_t s = hf->cols[i]; s; s = hf->sp[s - 1].next) {
            const HSpan* sp = &hf->sp[s - 1];
            if (sp->area == NULL_AREA) continue;
            int bot = sp->smax, top = sp->next ? hf->sp[sp->next - 1].smin : MAX_H;
            c->spans[idx].y = (uint16_t)bot;
            c->spans[idx].h = (uint16_t)(top - bot > 0xFFFF ? 0xFFFF : top - bot);
            c->areas[idx] = sp->area;
            idx++, c->cells[i].count++;
        }
    }
    for (int z = 0; z < c->h; z++)
        for (int x = 0; x < c->w; x++) {
            const CCell* cl = &c->cells[x + z * c->w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                CSpan* s = &c->spans[i];
                for (int d = 0; d < 4; d++) {
                    s->con[d] = NOT_CON;
                    int nx = x + DX[d], nz = z + DZ[d];
                    if (nx < 0 || nz < 0 || nx >= c->w || nz >= c->h) continue;
                    const CCell* nc = &c->cells[nx + nz * c->w];
                    for (uint32_t k = nc->index; k < nc->index + nc->count; k++) {
                        const CSpan* ns = &c->spans[k];
                        int bot = s->y > ns->y ? s->y : ns->y;
                        int top = (int)s->y + s->h < (int)ns->y + ns->h ? (int)s->y + s->h : (int)ns->y + ns->h;
                        if (top - bot >= height && abs((int)ns->y - (int)s->y) <= climb) {
                            if (k - nc->index < NOT_CON) s->con[d] = (uint8_t)(k - nc->index);
                            break;
                        }
                    }
                }
            }
        }
    return true;
}

// Recast's rcErodeWalkableArea: the spans closer than the radius to a wall or an edge leave
static bool erode(Chf* c, int radius) {
    uint8_t* dist = malloc(c->nspans ? c->nspans : 1);
    if (!dist) return false;
    memset(dist, 0xFF, c->nspans);
    for (int z = 0; z < c->h; z++)
        for (int x = 0; x < c->w; x++) {
            const CCell* cl = &c->cells[x + z * c->w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                if (c->areas[i] == NULL_AREA) {
                    dist[i] = 0;
                    continue;
                }
                const CSpan* s = &c->spans[i];
                int nc = 0;
                for (int d = 0; d < 4; d++)
                    if (s->con[d] != NOT_CON && c->areas[con_index(c, x, z, s, d)] != NULL_AREA) nc++;
                if (nc != 4) dist[i] = 0;
            }
        }
#define RELAX(idx, add)                                          \
    do {                                                         \
        int nd_ = (int)dist[idx] + (add);                        \
        if (nd_ > 255) nd_ = 255;                                \
        if (nd_ < dist[i]) dist[i] = (uint8_t)nd_;               \
    } while (0)
    for (int z = 0; z < c->h; z++)
        for (int x = 0; x < c->w; x++) {
            const CCell* cl = &c->cells[x + z * c->w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                const CSpan* s = &c->spans[i];
                if (s->con[0] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 0);
                    RELAX(ai, 2);
                    const CSpan* as = &c->spans[ai];
                    if (as->con[3] != NOT_CON) RELAX(con_index(c, x + DX[0], z + DZ[0], as, 3), 3);
                }
                if (s->con[3] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 3);
                    RELAX(ai, 2);
                    const CSpan* as = &c->spans[ai];
                    if (as->con[2] != NOT_CON) RELAX(con_index(c, x + DX[3], z + DZ[3], as, 2), 3);
                }
            }
        }
    for (int z = c->h - 1; z >= 0; z--)
        for (int x = c->w - 1; x >= 0; x--) {
            const CCell* cl = &c->cells[x + z * c->w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                const CSpan* s = &c->spans[i];
                if (s->con[2] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 2);
                    RELAX(ai, 2);
                    const CSpan* as = &c->spans[ai];
                    if (as->con[1] != NOT_CON) RELAX(con_index(c, x + DX[2], z + DZ[2], as, 1), 3);
                }
                if (s->con[1] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 1);
                    RELAX(ai, 2);
                    const CSpan* as = &c->spans[ai];
                    if (as->con[0] != NOT_CON) RELAX(con_index(c, x + DX[1], z + DZ[1], as, 0), 3);
                }
            }
        }
#undef RELAX
    int thr = radius * 2;
    for (uint32_t i = 0; i < c->nspans; i++)
        if (dist[i] < thr) c->areas[i] = NULL_AREA;
    free(dist);
    return true;
}

// A Nav Modifier Volume: the spans whose floor is inside it get its area (NULL: gone)
static void mark_areas(Chf* c, const NavTileInput* in, bool null_only) {
    const NavCfg* g = &in->cfg;
    for (uint32_t a = 0; a < in->nareas; a++) {
        const NavInArea* m = &in->areas[a];
        if ((m->area == NAV_AREA_NULL) != null_only) continue;
        int x0 = (int)floorf((m->mn[0] - in->bmin[0]) / g->cs), x1 = (int)floorf((m->mx[0] - in->bmin[0]) / g->cs);
        int z0 = (int)floorf((m->mn[1] - in->bmin[1]) / g->cs), z1 = (int)floorf((m->mx[1] - in->bmin[1]) / g->cs);
        int y0 = (int)floorf((m->mn[2] - in->bmin[2]) / g->ch), y1 = (int)ceilf((m->mx[2] - in->bmin[2]) / g->ch);
        if (x0 < 0) x0 = 0;
        if (z0 < 0) z0 = 0;
        if (x1 >= c->w) x1 = c->w - 1;
        if (z1 >= c->h) z1 = c->h - 1;
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) {
                const CCell* cl = &c->cells[x + z * c->w];
                for (uint32_t i = cl->index; i < cl->index + cl->count; i++)
                    if (c->areas[i] != NULL_AREA && c->spans[i].y >= y0 && c->spans[i].y <= y1) c->areas[i] = m->area;
            }
    }
}

// --- Regions: Recast's monotone partitioning (no holes, no overlaps), then the small islands dropped ---
typedef struct Sweep { uint16_t rid, id, nei; uint32_t ns; } Sweep;
#define NULL_NEI 0xFFFFu

static void paint_rect(Chf* c, int x0, int x1, int z0, int z1, uint16_t reg, uint16_t* src) {
    for (int z = z0; z < z1; z++)
        for (int x = x0; x < x1; x++) {
            const CCell* cl = &c->cells[x + z * c->w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++)
                if (c->areas[i] != NULL_AREA) src[i] = reg;
        }
}

static uint32_t uf_find(uint32_t* p, uint32_t x) {
    while (p[x] != x) p[x] = p[p[x]], x = p[x];
    return x;
}

static bool build_regions(Chf* c, int border, int min_region, uint32_t* nregions) {
    int w = c->w, h = c->h;
    uint16_t* src = calloc(c->nspans ? c->nspans : 1, sizeof(uint16_t));
    Sweep* sweeps = calloc((size_t)c->nspans + 2, sizeof(Sweep));
    uint32_t* prev = calloc((size_t)c->nspans + 8, sizeof(uint32_t));
    if (!src || !sweeps || !prev) {
        free(src), free(sweeps), free(prev);
        return false;
    }
    uint32_t id = 1;
    int bw = border < w ? border : w, bh = border < h ? border : h;
    paint_rect(c, 0, bw, 0, h, (uint16_t)(id++ | BORDER_REG), src);
    paint_rect(c, w - bw, w, 0, h, (uint16_t)(id++ | BORDER_REG), src);
    paint_rect(c, 0, w, 0, bh, (uint16_t)(id++ | BORDER_REG), src);
    paint_rect(c, 0, w, h - bh, h, (uint16_t)(id++ | BORDER_REG), src);
    bool ok = true;
    for (int z = border; z < h - border && ok; z++) {
        memset(prev, 0, sizeof(uint32_t) * (id + 1));
        uint32_t rid = 1;
        for (int x = border; x < w - border; x++) {
            const CCell* cl = &c->cells[x + z * w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                const CSpan* s = &c->spans[i];
                if (c->areas[i] == NULL_AREA) continue;
                uint32_t previd = 0;
                if (s->con[0] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 0);
                    if (!(src[ai] & BORDER_REG) && c->areas[i] == c->areas[ai]) previd = src[ai];
                }
                if (!previd) {
                    previd = rid++;
                    sweeps[previd].rid = (uint16_t)previd, sweeps[previd].ns = 0, sweeps[previd].nei = 0;
                }
                if (s->con[3] != NOT_CON) {
                    uint32_t ai = con_index(c, x, z, s, 3);
                    if (src[ai] && !(src[ai] & BORDER_REG) && c->areas[i] == c->areas[ai]) {
                        uint16_t nr = src[ai];
                        if (!sweeps[previd].nei || sweeps[previd].nei == nr) {
                            sweeps[previd].nei = nr, sweeps[previd].ns++;
                            prev[nr]++;
                        } else
                            sweeps[previd].nei = NULL_NEI;
                    }
                }
                src[i] = (uint16_t)previd;
            }
        }
        for (uint32_t i = 1; i < rid; i++) {
            if (sweeps[i].nei != NULL_NEI && sweeps[i].nei != 0 && prev[sweeps[i].nei] == sweeps[i].ns) sweeps[i].id = sweeps[i].nei;
            else {
                if (id >= BORDER_REG - 1) { ok = false; break; } // (more regions than the ids hold)
                sweeps[i].id = (uint16_t)id++;
            }
        }
        for (int x = border; x < w - border && ok; x++) {
            const CCell* cl = &c->cells[x + z * w];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++)
                if (src[i] > 0 && src[i] < rid && !(src[i] & BORDER_REG)) src[i] = sweeps[src[i]].id;
        }
    }
    // the islands smaller than min_region cells, not reaching the tile's side, dropped (Recast's filter, without its merge)
    uint32_t* par = ok ? malloc(sizeof(uint32_t) * id * 3) : NULL;
    if (par) {
        uint32_t *size = par + id, *touch = par + 2 * id;
        for (uint32_t r = 0; r < id; r++) par[r] = r, size[r] = 0, touch[r] = 0;
        for (int z = 0; z < h; z++)
            for (int x = 0; x < w; x++) {
                const CCell* cl = &c->cells[x + z * w];
                for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                    uint16_t r = src[i];
                    if (!r || (r & BORDER_REG)) continue;
                    size[r]++;
                    const CSpan* s = &c->spans[i];
                    for (int d = 0; d < 4; d++) {
                        if (s->con[d] == NOT_CON) continue;
                        uint16_t nr = src[con_index(c, x, z, s, d)];
                        if (nr & BORDER_REG) touch[r] = 1;
                        else if (nr && nr != r) {
                            uint32_t a = uf_find(par, r), b = uf_find(par, nr);
                            if (a != b) par[a] = b;
                        }
                    }
                }
            }
        uint32_t* total = calloc(id, sizeof(uint32_t));
        uint8_t* reach = calloc(id, 1);
        if (total && reach) {
            for (uint32_t r = 1; r < id; r++) {
                uint32_t root = uf_find(par, r);
                total[root] += size[r];
                if (touch[r]) reach[root] = 1;
            }
            for (uint32_t i = 0; i < c->nspans; i++) {
                uint16_t r = src[i];
                if (!r || (r & BORDER_REG)) continue;
                uint32_t root = uf_find(par, r);
                if (total[root] < (uint32_t)min_region && !reach[root]) src[i] = 0;
            }
        }
        free(total), free(reach), free(par);
    }
    for (uint32_t i = 0; i < c->nspans; i++) c->spans[i].reg = src[i];
    *nregions = id;
    free(src), free(sweeps), free(prev);
    return ok;
}

// --- Contours (Recast's rcBuildContours: the walk, the simplification) ---
static int corner_height(const Chf* c, int x, int z, uint32_t i, int dir) {
    const CSpan* s = &c->spans[i];
    int ch = s->y, dirp = (dir + 1) & 3;
    if (s->con[dir] != NOT_CON) {
        int ax = x + DX[dir], az = z + DZ[dir];
        uint32_t ai = con_index(c, x, z, s, dir);
        const CSpan* as = &c->spans[ai];
        if (as->y > ch) ch = as->y;
        if (as->con[dirp] != NOT_CON) {
            const CSpan* as2 = &c->spans[con_index(c, ax, az, as, dirp)];
            if (as2->y > ch) ch = as2->y;
        }
    }
    if (s->con[dirp] != NOT_CON) {
        int ax = x + DX[dirp], az = z + DZ[dirp];
        uint32_t ai = con_index(c, x, z, s, dirp);
        const CSpan* as = &c->spans[ai];
        if (as->y > ch) ch = as->y;
        if (as->con[dir] != NOT_CON) {
            const CSpan* as2 = &c->spans[con_index(c, ax, az, as, dir)];
            if (as2->y > ch) ch = as2->y;
        }
    }
    return ch;
}

static void walk_contour(const Chf* c, int x, int z, uint32_t i, uint8_t* flags, IVec* pts) {
    int dir = 0;
    while (!(flags[i] & (1 << dir))) dir++;
    int start_dir = dir;
    uint32_t start_i = i;
    uint8_t area = c->areas[i];
    for (int iter = 0; iter < 40000; iter++) {
        if (flags[i] & (1 << dir)) {
            int px = x, py = corner_height(c, x, z, i, dir), pz = z;
            switch (dir) {
            case 0: pz++; break;
            case 1: px++, pz++; break;
            case 2: px++; break;
            }
            int r = 0;
            const CSpan* s = &c->spans[i];
            if (s->con[dir] != NOT_CON) {
                uint32_t ai = con_index(c, x, z, s, dir);
                r = c->spans[ai].reg;
                if (area != c->areas[ai]) r |= AREA_BORDER;
            }
            iv_push4(pts, px, py, pz, r);
            flags[i] &= (uint8_t)~(1 << dir);
            dir = (dir + 1) & 3;
        } else {
            const CSpan* s = &c->spans[i];
            if (s->con[dir] == NOT_CON) return; // (should not happen)
            uint32_t ni = con_index(c, x, z, s, dir);
            x += DX[dir], z += DZ[dir], i = ni;
            dir = (dir + 3) & 3;
        }
        if (i == start_i && dir == start_dir) break;
    }
}

static float dist_pt_seg(int x, int z, int px, int pz, int qx, int qz) {
    float pqx = (float)(qx - px), pqz = (float)(qz - pz), dx = (float)(x - px), dz = (float)(z - pz);
    float d = pqx * pqx + pqz * pqz, t = pqx * dx + pqz * dz;
    if (d > 0) t /= d;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    dx = px + t * pqx - x, dz = pz + t * pqz - z;
    return dx * dx + dz * dz;
}

static void insert4(IVec* s, int at, const int* v) { // a point into the simplified contour before index at
    iv_reserve(s, s->n + 4);
    if (s->bad) return;
    memmove(&s->a[(at + 1) * 4], &s->a[at * 4], sizeof(int) * (size_t)(s->n - at * 4));
    memcpy(&s->a[at * 4], v, sizeof(int) * 4);
    s->n += 4;
}

static void simplify_contour(const IVec* pts, IVec* simp, float max_error, int max_edge) {
    const int* p = pts->a;
    int pn = pts->n / 4;
    bool connections = false;
    for (int i = 0; i < pn && !connections; i++) connections = (p[i * 4 + 3] & REG_MASK) != 0;
    if (connections) {
        for (int i = 0; i < pn; i++) {
            int ii = (i + 1) % pn;
            bool diff = (p[i * 4 + 3] & REG_MASK) != (p[ii * 4 + 3] & REG_MASK);
            bool areas = (p[i * 4 + 3] & AREA_BORDER) != (p[ii * 4 + 3] & AREA_BORDER);
            if (diff || areas) iv_push4(simp, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], i);
        }
    }
    if (simp->n == 0) { // no portals: the lower-left and upper-right points to start from
        int ll = 0, ur = 0;
        for (int i = 0; i < pn; i++) {
            int x = p[i * 4], z = p[i * 4 + 2];
            if (x < p[ll * 4] || (x == p[ll * 4] && z < p[ll * 4 + 2])) ll = i;
            if (x > p[ur * 4] || (x == p[ur * 4] && z > p[ur * 4 + 2])) ur = i;
        }
        iv_push4(simp, p[ll * 4], p[ll * 4 + 1], p[ll * 4 + 2], ll);
        iv_push4(simp, p[ur * 4], p[ur * 4 + 1], p[ur * 4 + 2], ur);
    }
    // points added until the raw ones are within the error (the walls and the area borders; portals stay straight)
    for (int i = 0; i < simp->n / 4 && !simp->bad;) {
        int ii = (i + 1) % (simp->n / 4);
        int ax = simp->a[i * 4], az = simp->a[i * 4 + 2], ai = simp->a[i * 4 + 3];
        int bx = simp->a[ii * 4], bz = simp->a[ii * 4 + 2], bi = simp->a[ii * 4 + 3];
        float maxd = 0;
        int maxi = -1, ci, cinc, endi;
        if (bx > ax || (bx == ax && bz > az)) cinc = 1, ci = (ai + cinc) % pn, endi = bi;
        else {
            cinc = pn - 1, ci = (bi + cinc) % pn, endi = ai;
            int t = ax;
            ax = bx, bx = t, t = az, az = bz, bz = t;
        }
        if ((p[ci * 4 + 3] & REG_MASK) == 0 || (p[ci * 4 + 3] & AREA_BORDER)) {
            while (ci != endi) {
                float d = dist_pt_seg(p[ci * 4], p[ci * 4 + 2], ax, az, bx, bz);
                if (d > maxd) maxd = d, maxi = ci;
                ci = (ci + cinc) % pn;
            }
        }
        if (maxi != -1 && maxd > max_error * max_error) {
            int v[4] = { p[maxi * 4], p[maxi * 4 + 1], p[maxi * 4 + 2], maxi };
            insert4(simp, i + 1, v);
        } else
            i++;
    }
    // the long wall edges split (a big floor's polygons stay compact)
    if (max_edge > 0) {
        for (int i = 0; i < simp->n / 4 && !simp->bad;) {
            int ii = (i + 1) % (simp->n / 4);
            int ax = simp->a[i * 4], az = simp->a[i * 4 + 2], ai = simp->a[i * 4 + 3];
            int bx = simp->a[ii * 4], bz = simp->a[ii * 4 + 2], bi = simp->a[ii * 4 + 3];
            int maxi = -1, ci = (ai + 1) % pn;
            bool tess = (p[ci * 4 + 3] & REG_MASK) == 0 || (p[ci * 4 + 3] & AREA_BORDER);
            if (tess) {
                int dx = bx - ax, dz = bz - az;
                if (dx * dx + dz * dz > max_edge * max_edge) {
                    int n = bi < ai ? (bi + pn - ai) : (bi - ai);
                    if (n > 1) maxi = (bx > ax || (bx == ax && bz > az)) ? (ai + n / 2) % pn : (ai + (n + 1) / 2) % pn;
                }
            }
            if (maxi != -1) {
                int v[4] = { p[maxi * 4], p[maxi * 4 + 1], p[maxi * 4 + 2], maxi };
                insert4(simp, i + 1, v);
            } else
                i++;
        }
    }
    for (int i = 0; i < simp->n / 4; i++) { // the edge's neighbor from the next raw point
        int ai = (simp->a[i * 4 + 3] + 1) % pn, bi = simp->a[i * 4 + 3];
        simp->a[i * 4 + 3] = (p[ai * 4 + 3] & (REG_MASK | AREA_BORDER)) | (p[bi * 4 + 3] & BORDER_VERTEX);
    }
}

static void remove_degenerate(IVec* s) {
    int n = s->n / 4;
    for (int i = 0; i < n; i++) {
        int ni = i + 1 < n ? i + 1 : 0;
        if (s->a[i * 4] == s->a[ni * 4] && s->a[i * 4 + 2] == s->a[ni * 4 + 2]) {
            memmove(&s->a[i * 4], &s->a[(i + 1) * 4], sizeof(int) * (size_t)((n - i - 1) * 4));
            s->n -= 4, n--, i--;
        }
    }
}

// --- Triangulation (Recast's ear clipping over the contour's integer points) ---
static inline int nxt(int i, int n) { return i + 1 < n ? i + 1 : 0; }
static inline int prv(int i, int n) { return i - 1 >= 0 ? i - 1 : n - 1; }
static inline int area2(const int* a, const int* b, const int* c) { return (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]); }
static inline bool lft(const int* a, const int* b, const int* c) { return area2(a, b, c) < 0; }
static inline bool lft_on(const int* a, const int* b, const int* c) { return area2(a, b, c) <= 0; }
static inline bool colin(const int* a, const int* b, const int* c) { return area2(a, b, c) == 0; }
static bool intersect_prop(const int* a, const int* b, const int* c, const int* d) {
    if (colin(a, b, c) || colin(a, b, d) || colin(c, d, a) || colin(c, d, b)) return false;
    return (lft(a, b, c) != lft(a, b, d)) && (lft(c, d, a) != lft(c, d, b));
}
static bool betw(const int* a, const int* b, const int* c) {
    if (!colin(a, b, c)) return false;
    if (a[0] != b[0]) return (a[0] <= c[0] && c[0] <= b[0]) || (a[0] >= c[0] && c[0] >= b[0]);
    return (a[2] <= c[2] && c[2] <= b[2]) || (a[2] >= c[2] && c[2] >= b[2]);
}
static bool isect(const int* a, const int* b, const int* c, const int* d) {
    return intersect_prop(a, b, c, d) || betw(a, b, c) || betw(a, b, d) || betw(c, d, a) || betw(c, d, b);
}
static inline bool veq(const int* a, const int* b) { return a[0] == b[0] && a[2] == b[2]; }
#define VI(k) (&verts[(idx[k] & 0x0fffffff) * 4])
static bool diagonalie(int i, int j, int n, const int* verts, const int* idx, bool loose) {
    const int *d0 = VI(i), *d1 = VI(j);
    for (int k = 0; k < n; k++) {
        int k1 = nxt(k, n);
        if (k == i || k1 == i || k == j || k1 == j) continue;
        const int *p0 = VI(k), *p1 = VI(k1);
        if (veq(d0, p0) || veq(d1, p0) || veq(d0, p1) || veq(d1, p1)) continue;
        if (loose ? intersect_prop(d0, d1, p0, p1) : isect(d0, d1, p0, p1)) return false;
    }
    return true;
}
static bool in_cone(int i, int j, int n, const int* verts, const int* idx, bool loose) {
    const int *pi = VI(i), *pj = VI(j), *pi1 = VI(nxt(i, n)), *pin1 = VI(prv(i, n));
    if (lft_on(pin1, pi, pi1)) return loose ? (lft_on(pi, pj, pin1) && lft_on(pj, pi, pi1)) : (lft(pi, pj, pin1) && lft(pj, pi, pi1));
    return !(lft_on(pi, pj, pi1) && lft_on(pj, pi, pin1));
}
static bool diagonal(int i, int j, int n, const int* verts, const int* idx, bool loose) {
    return in_cone(i, j, n, verts, idx, loose) && diagonalie(i, j, n, verts, idx, loose);
}
static int triangulate(int n, const int* verts, int* idx, int* tris) {
    int ntris = 0;
    int* dst = tris;
    for (int i = 0; i < n; i++) {
        int i1 = nxt(i, n), i2 = nxt(i1, n);
        if (diagonal(i, i2, n, verts, idx, false)) idx[i1] |= (int)0x80000000;
    }
    while (n > 3) {
        int min_len = -1, mini = -1;
        for (int i = 0; i < n; i++) {
            int i1 = nxt(i, n);
            if (idx[i1] & (int)0x80000000) {
                const int *p0 = VI(i), *p2 = VI(nxt(i1, n));
                int dx = p2[0] - p0[0], dz = p2[2] - p0[2], len = dx * dx + dz * dz;
                if (min_len < 0 || len < min_len) min_len = len, mini = i;
            }
        }
        if (mini == -1) { // overlapping segments: a looser test
            for (int i = 0; i < n; i++) {
                int i1 = nxt(i, n), i2 = nxt(i1, n);
                if (diagonal(i, i2, n, verts, idx, true)) {
                    const int *p0 = VI(i), *p2 = VI(nxt(i2, n));
                    int dx = p2[0] - p0[0], dz = p2[2] - p0[2], len = dx * dx + dz * dz;
                    if (min_len < 0 || len < min_len) min_len = len, mini = i;
                }
            }
            if (mini == -1) return -ntris; // (a messed-up contour: what was made so far)
        }
        int i = mini, i1 = nxt(i, n), i2 = nxt(i1, n);
        *dst++ = idx[i] & 0x0fffffff, *dst++ = idx[i1] & 0x0fffffff, *dst++ = idx[i2] & 0x0fffffff;
        ntris++;
        n--;
        for (int k = i1; k < n; k++) idx[k] = idx[k + 1];
        if (i1 >= n) i1 = 0;
        i = prv(i1, n);
        if (diagonal(prv(i, n), i1, n, verts, idx, false)) idx[i] |= (int)0x80000000;
        else idx[i] &= 0x0fffffff;
        if (diagonal(i, nxt(i1, n), n, verts, idx, false)) idx[i1] |= (int)0x80000000;
        else idx[i1] &= 0x0fffffff;
    }
    *dst++ = idx[0] & 0x0fffffff, *dst++ = idx[1] & 0x0fffffff, *dst++ = idx[2] & 0x0fffffff;
    return ntris + 1;
}
#undef VI

// --- Polygons (Recast's rcBuildPolyMesh: the triangles of each contour merged into convex polygons) ---
#define VBUCKETS 4096
static int vertex_hash(int x, int z) { return (int)(((unsigned)x * 0x8da6b343u + (unsigned)z * 0xcb1ab31fu) & (VBUCKETS - 1)); }
static int add_vertex(int x, int y, int z, uint16_t* verts, int* first, int* next, int* nv) {
    int b = vertex_hash(x, z);
    for (int i = first[b]; i != -1; i = next[i]) {
        const uint16_t* v = &verts[i * 3];
        if (v[0] == x && abs((int)v[1] - y) <= 2 && v[2] == z) return i;
    }
    int i = (*nv)++;
    verts[i * 3] = (uint16_t)x, verts[i * 3 + 1] = (uint16_t)y, verts[i * 3 + 2] = (uint16_t)z;
    next[i] = first[b], first[b] = i;
    return i;
}
static int count_pv(const uint16_t* p) {
    for (int i = 0; i < NAV_NVP; i++)
        if (p[i] == NULL_IDX) return i;
    return NAV_NVP;
}
static bool uleft(const uint16_t* a, const uint16_t* b, const uint16_t* c) {
    return ((int)b[0] - (int)a[0]) * ((int)c[2] - (int)a[2]) - ((int)c[0] - (int)a[0]) * ((int)b[2] - (int)a[2]) < 0;
}
static int merge_value(const uint16_t* pa, const uint16_t* pb, const uint16_t* verts, int* ea, int* eb) {
    int na = count_pv(pa), nb = count_pv(pb);
    if (na + nb - 2 > NAV_NVP) return -1;
    *ea = *eb = -1;
    for (int i = 0; i < na && *ea < 0; i++) {
        uint16_t va0 = pa[i], va1 = pa[(i + 1) % na];
        if (va0 > va1) { uint16_t t = va0; va0 = va1, va1 = t; }
        for (int j = 0; j < nb; j++) {
            uint16_t vb0 = pb[j], vb1 = pb[(j + 1) % nb];
            if (vb0 > vb1) { uint16_t t = vb0; vb0 = vb1, vb1 = t; }
            if (va0 == vb0 && va1 == vb1) {
                *ea = i, *eb = j;
                break;
            }
        }
    }
    if (*ea < 0) return -1;
    if (!uleft(&verts[pa[(*ea + na - 1) % na] * 3], &verts[pa[*ea] * 3], &verts[pb[(*eb + 2) % nb] * 3])) return -1;
    if (!uleft(&verts[pb[(*eb + nb - 1) % nb] * 3], &verts[pb[*eb] * 3], &verts[pa[(*ea + 2) % na] * 3])) return -1;
    int dx = (int)verts[pa[*ea] * 3] - (int)verts[pa[(*ea + 1) % na] * 3];
    int dz = (int)verts[pa[*ea] * 3 + 2] - (int)verts[pa[(*ea + 1) % na] * 3 + 2];
    return dx * dx + dz * dz;
}
static void merge_polys(uint16_t* pa, const uint16_t* pb, int ea, int eb) {
    int na = count_pv(pa), nb = count_pv(pb), n = 0;
    uint16_t tmp[NAV_NVP];
    memset(tmp, 0xFF, sizeof tmp);
    for (int i = 0; i < na - 1; i++) tmp[n++] = pa[(ea + 1 + i) % na];
    for (int i = 0; i < nb - 1; i++) tmp[n++] = pb[(eb + 1 + i) % nb];
    memcpy(pa, tmp, sizeof tmp);
}

typedef struct Contour { int* v; int nv; uint16_t reg; uint8_t area; } Contour;

NavTileData* nav_build_tile(const NavTileInput* in) {
    double t0 = re_time_seconds();
    const NavCfg* g = &in->cfg;
    const int W = g->tile + 2 * g->border, H = W;
    const int climb = (int)floorf(g->climb / g->ch), height = (int)ceilf(g->height / g->ch);
    const int radius = (int)ceilf(g->radius / g->cs);
    const float yrange = in->bmax[2] - in->bmin[2];
    NavTileData* out = NULL;
    Hf hf = { 0 };
    Chf c = { 0 };
    uint8_t* flags = NULL;
    Contour* conts = NULL;
    int ncont = 0, ccap = 0;
    IVec pts = { 0 }, simp = { 0 };
    uint16_t *mverts = NULL, *mpolys = NULL, *mneis = NULL;
    uint16_t* mregs = NULL;
    uint8_t* mareas = NULL;
    int *first = NULL, *next = NULL, *idx = NULL, *tris = NULL;
    uint16_t* cpolys = NULL;
    hf.w = W, hf.h = H;
    hf.cols = calloc((size_t)W * H, sizeof(uint32_t));
    if (!hf.cols) goto done;
    // 1. voxels: the boxes, then the landscape's triangles (walkable when their slope allows)
    for (uint32_t i = 0; i < in->nboxes; i++) rast_box(&hf, &in->boxes[i], in->bmin, g->cs, g->ch, yrange, climb);
    if (in->land && in->lw > 1 && in->lh > 1) {
        float walk_cos = cosf(g->slope_deg * 0.017453293f);
        for (int j = 0; j + 1 < in->lh; j++)
            for (int i = 0; i + 1 < in->lw; i++) {
                const float* L = in->land;
                float h00 = L[j * in->lw + i], h10 = L[j * in->lw + i + 1], h01 = L[(j + 1) * in->lw + i], h11 = L[(j + 1) * in->lw + i + 1];
                if (isnan(h00) || isnan(h10) || isnan(h01) || isnan(h11)) continue;
                float x0 = (float)i * in->lstep, x1 = x0 + in->lstep, z0 = (float)j * in->lstep, z1 = z0 + in->lstep;
                float b = in->bmin[2];
                float q[4][3] = { { x0, h00 - b, z0 }, { x1, h10 - b, z0 }, { x0, h01 - b, z1 }, { x1, h11 - b, z1 } };
                const int tri[2][3] = { { 0, 1, 3 }, { 0, 3, 2 } };
                for (int t = 0; t < 2; t++) {
                    const float *a = q[tri[t][0]], *bb = q[tri[t][1]], *cc = q[tri[t][2]];
                    float e0[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] }, e1[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
                    float n[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0] };
                    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    uint8_t area = l > 0.0f && fabsf(n[1]) / l > walk_cos ? 1 : NULL_AREA;
                    rast_tri(&hf, a, bb, cc, area, g->cs, g->ch, yrange, climb);
                }
            }
    }
    if (hf.bad) goto done;
    // 2. the agent's filters
    filter_low_hanging(&hf, climb);
    filter_ledges(&hf, height, climb);
    filter_low_height(&hf, height);
    // 3. the open space, the Null volumes, the radius, the areas
    if (!build_compact(&hf, height, climb, &c)) goto done;
    free(hf.cols), free(hf.sp), hf.cols = NULL, hf.sp = NULL;
    mark_areas(&c, in, true);
    if (!erode(&c, radius)) goto done;
    mark_areas(&c, in, false);
    // 4. regions
    uint32_t nreg = 0;
    if (!build_regions(&c, g->border, g->min_region, &nreg)) goto done;
    // 5. contours
    flags = calloc(c.nspans ? c.nspans : 1, 1);
    if (!flags) goto done;
    for (int z = 0; z < H; z++)
        for (int x = 0; x < W; x++) {
            const CCell* cl = &c.cells[x + z * W];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                const CSpan* s = &c.spans[i];
                if (!s->reg || (s->reg & BORDER_REG)) continue;
                uint8_t res = 0;
                for (int d = 0; d < 4; d++)
                    if (s->con[d] != NOT_CON && c.spans[con_index(&c, x, z, s, d)].reg == s->reg) res |= (uint8_t)(1 << d);
                flags[i] = res ^ 0xF;
            }
        }
    for (int z = 0; z < H; z++)
        for (int x = 0; x < W; x++) {
            const CCell* cl = &c.cells[x + z * W];
            for (uint32_t i = cl->index; i < cl->index + cl->count; i++) {
                if (flags[i] == 0 || flags[i] == 0xF) {
                    flags[i] = 0;
                    continue;
                }
                uint16_t reg = c.spans[i].reg;
                if (!reg || (reg & BORDER_REG)) continue;
                pts.n = simp.n = 0;
                walk_contour(&c, x, z, i, flags, &pts);
                if (pts.bad || pts.n < 12) continue;
                simplify_contour(&pts, &simp, g->max_error, g->max_edge);
                remove_degenerate(&simp);
                if (simp.bad) goto done;
                if (simp.n / 4 < 3) continue;
                if (ncont == ccap) {
                    int nc = ccap ? ccap * 2 : 64;
                    Contour* ncs = realloc(conts, sizeof(Contour) * (size_t)nc);
                    if (!ncs) goto done;
                    conts = ncs, ccap = nc;
                }
                Contour* ct = &conts[ncont];
                ct->v = malloc(sizeof(int) * (size_t)simp.n);
                if (!ct->v) goto done;
                memcpy(ct->v, simp.a, sizeof(int) * (size_t)simp.n);
                for (int k = 0; k < simp.n / 4; k++) ct->v[k * 4] -= g->border, ct->v[k * 4 + 2] -= g->border; // (the tile's own coordinates)
                ct->nv = simp.n / 4, ct->reg = reg, ct->area = c.areas[i];
                ncont++;
            }
        }
    // 6. polygons
    int maxv = 0, maxcv = 0;
    for (int i = 0; i < ncont; i++) {
        maxv += conts[i].nv;
        if (conts[i].nv > maxcv) maxcv = conts[i].nv;
    }
    if (!maxv) goto done;
    if (maxv > 65000) goto done; // (more than a tile's vertex index holds)
    int maxpolys = maxv - 2 * ncont;
    if (maxpolys < 1) maxpolys = 1;
    mverts = malloc(sizeof(uint16_t) * 3 * (size_t)maxv);
    mpolys = malloc(sizeof(uint16_t) * NAV_NVP * (size_t)maxpolys);
    mneis = malloc(sizeof(uint16_t) * NAV_NVP * (size_t)maxpolys);
    mregs = malloc(sizeof(uint16_t) * (size_t)maxpolys);
    mareas = malloc((size_t)maxpolys);
    first = malloc(sizeof(int) * VBUCKETS);
    next = malloc(sizeof(int) * (size_t)maxv);
    idx = malloc(sizeof(int) * (size_t)maxcv);
    tris = malloc(sizeof(int) * 3 * (size_t)maxcv);
    cpolys = malloc(sizeof(uint16_t) * NAV_NVP * (size_t)maxcv);
    if (!mverts || !mpolys || !mneis || !mregs || !mareas || !first || !next || !idx || !tris || !cpolys) goto done;
    for (int i = 0; i < VBUCKETS; i++) first[i] = -1;
    int nverts = 0, npolys = 0;
    for (int ci = 0; ci < ncont; ci++) {
        const Contour* ct = &conts[ci];
        for (int j = 0; j < ct->nv; j++) idx[j] = j;
        int ntris = triangulate(ct->nv, ct->v, idx, tris);
        if (ntris < 0) ntris = -ntris;
        for (int j = 0; j < ct->nv; j++) idx[j] = add_vertex(ct->v[j * 4], ct->v[j * 4 + 1], ct->v[j * 4 + 2], mverts, first, next, &nverts);
        int np = 0;
        memset(cpolys, 0xFF, sizeof(uint16_t) * NAV_NVP * (size_t)maxcv);
        for (int j = 0; j < ntris; j++) {
            const int* t = &tris[j * 3];
            if (idx[t[0]] == idx[t[1]] || idx[t[0]] == idx[t[2]] || idx[t[1]] == idx[t[2]]) continue;
            uint16_t* p = &cpolys[np * NAV_NVP];
            p[0] = (uint16_t)idx[t[0]], p[1] = (uint16_t)idx[t[1]], p[2] = (uint16_t)idx[t[2]];
            np++;
        }
        for (;;) { // the best pair merged (the longest shared edge), until none is convex
            int best = 0, ba = 0, bb = 0, bea = 0, beb = 0;
            for (int j = 0; j < np - 1; j++)
                for (int k = j + 1; k < np; k++) {
                    int ea, eb, v = merge_value(&cpolys[j * NAV_NVP], &cpolys[k * NAV_NVP], mverts, &ea, &eb);
                    if (v > best) best = v, ba = j, bb = k, bea = ea, beb = eb;
                }
            if (best <= 0) break;
            merge_polys(&cpolys[ba * NAV_NVP], &cpolys[bb * NAV_NVP], bea, beb);
            if (bb != np - 1) memcpy(&cpolys[bb * NAV_NVP], &cpolys[(np - 1) * NAV_NVP], sizeof(uint16_t) * NAV_NVP);
            np--;
        }
        for (int j = 0; j < np && npolys < maxpolys; j++) {
            memcpy(&mpolys[npolys * NAV_NVP], &cpolys[j * NAV_NVP], sizeof(uint16_t) * NAV_NVP);
            mregs[npolys] = ct->reg, mareas[npolys] = ct->area;
            npolys++;
        }
    }
    if (!npolys) goto done;
    // adjacency inside the tile (Recast's buildMeshAdjacency), then the tile's sides (portals)
    memset(mneis, 0xFF, sizeof(uint16_t) * NAV_NVP * (size_t)npolys);
    {
        int maxe = npolys * NAV_NVP;
        typedef struct Edge { uint16_t vert[2], poly[2], pedge[2]; } Edge;
        Edge* edges = malloc(sizeof(Edge) * (size_t)maxe);
        int* fe = malloc(sizeof(int) * ((size_t)nverts + (size_t)maxe));
        if (!edges || !fe) {
            free(edges), free(fe);
            goto done;
        }
        int* ne = fe + nverts, nedge = 0;
        for (int i = 0; i < nverts; i++) fe[i] = -1;
        for (int i = 0; i < npolys; i++) {
            const uint16_t* t = &mpolys[i * NAV_NVP];
            int nv = count_pv(t);
            for (int j = 0; j < nv; j++) {
                uint16_t v0 = t[j], v1 = t[(j + 1) % nv];
                if (v0 < v1) {
                    Edge* e = &edges[nedge];
                    e->vert[0] = v0, e->vert[1] = v1, e->poly[0] = e->poly[1] = (uint16_t)i, e->pedge[0] = (uint16_t)j, e->pedge[1] = 0;
                    ne[nedge] = fe[v0], fe[v0] = nedge++;
                }
            }
        }
        for (int i = 0; i < npolys; i++) {
            const uint16_t* t = &mpolys[i * NAV_NVP];
            int nv = count_pv(t);
            for (int j = 0; j < nv; j++) {
                uint16_t v0 = t[j], v1 = t[(j + 1) % nv];
                if (v0 > v1)
                    for (int e = fe[v1]; e != -1; e = ne[e])
                        if (edges[e].vert[1] == v0 && edges[e].poly[0] == edges[e].poly[1]) {
                            edges[e].poly[1] = (uint16_t)i, edges[e].pedge[1] = (uint16_t)j;
                            break;
                        }
            }
        }
        for (int i = 0; i < nedge; i++) {
            const Edge* e = &edges[i];
            if (e->poly[0] == e->poly[1]) continue;
            mneis[e->poly[0] * NAV_NVP + e->pedge[0]] = e->poly[1];
            mneis[e->poly[1] * NAV_NVP + e->pedge[1]] = e->poly[0];
        }
        free(edges), free(fe);
    }
    // 7. the tile's block: vertices in world space, the polygons, their boxes and height grids
    const int T = g->tile, step = g->detail_step > 0 ? g->detail_step : 2;
    const float ox = in->bmin[0] + (float)g->border * g->cs, oy = in->bmin[1] + (float)g->border * g->cs;
    uint32_t ndet = 0;
    for (int i = 0; i < npolys; i++) {
        const uint16_t* p = &mpolys[i * NAV_NVP];
        int nv = count_pv(p), x0 = 1 << 30, x1 = -1, z0 = 1 << 30, z1 = -1;
        for (int j = 0; j < nv; j++) {
            const uint16_t* v = &mverts[p[j] * 3];
            if (v[0] < x0) x0 = v[0];
            if (v[0] > x1) x1 = v[0];
            if (v[2] < z0) z0 = v[2];
            if (v[2] > z1) z1 = v[2];
        }
        int dw = (x1 - x0 + step - 1) / step + 1, dh = (z1 - z0 + step - 1) / step + 1;
        if (dw > 255) dw = 255;
        if (dh > 255) dh = 255;
        ndet += (uint32_t)(dw * dh);
    }
    size_t bytes = sizeof(NavTileData) + sizeof(float) * 3 * (size_t)nverts + sizeof(NavPoly) * (size_t)npolys + sizeof(float) * 6 * (size_t)npolys +
                   sizeof(float) * ndet;
    out = calloc(1, bytes);
    if (!out) goto done;
    uint8_t* mem = (uint8_t*)(out + 1);
    out->verts = (float*)mem, mem += sizeof(float) * 3 * (size_t)nverts;
    out->polys = (NavPoly*)mem, mem += sizeof(NavPoly) * (size_t)npolys;
    out->pbox = (float*)mem, mem += sizeof(float) * 6 * (size_t)npolys;
    out->detail = (float*)mem;
    out->tx = in->tx, out->ty = in->ty;
    out->nverts = (uint32_t)nverts, out->npolys = (uint32_t)npolys, out->ndetail = ndet;
    out->bmin[0] = ox, out->bmin[1] = oy, out->bmin[2] = in->bmin[2];
    out->bmax[0] = ox + (float)T * g->cs, out->bmax[1] = oy + (float)T * g->cs, out->bmax[2] = in->bmax[2];
    out->dstep = (float)step * g->cs;
    for (int i = 0; i < nverts; i++) {
        out->verts[i * 3] = ox + (float)mverts[i * 3] * g->cs;
        out->verts[i * 3 + 1] = oy + (float)mverts[i * 3 + 2] * g->cs;
        out->verts[i * 3 + 2] = in->bmin[2] + (float)mverts[i * 3 + 1] * g->ch;
    }
    uint32_t dat = 0;
    for (int i = 0; i < npolys; i++) {
        NavPoly* P = &out->polys[i];
        const uint16_t* p = &mpolys[i * NAV_NVP];
        int nv = count_pv(p);
        P->nv = (uint8_t)nv, P->area = mareas[i], P->first_link = NAV_NO_LINK;
        int x0 = 1 << 30, x1 = -1, z0 = 1 << 30, z1 = -1;
        float hmean = 0.0f;
        for (int j = 0; j < nv; j++) {
            P->v[j] = p[j];
            uint16_t nb = mneis[i * NAV_NVP + j];
            if (nb != NULL_IDX) P->nei[j] = (uint16_t)(nb + 1);
            else { // on the tile's side: a portal to the neighbor tile
                const uint16_t *va = &mverts[p[j] * 3], *vb = &mverts[p[(j + 1) % nv] * 3];
                if (va[0] == 0 && vb[0] == 0) P->nei[j] = NAV_PORTAL | 0;
                else if (va[2] == T && vb[2] == T) P->nei[j] = NAV_PORTAL | 1;
                else if (va[0] == T && vb[0] == T) P->nei[j] = NAV_PORTAL | 2;
                else if (va[2] == 0 && vb[2] == 0) P->nei[j] = NAV_PORTAL | 3;
                else P->nei[j] = 0;
            }
            const uint16_t* v = &mverts[p[j] * 3];
            if (v[0] < x0) x0 = v[0];
            if (v[0] > x1) x1 = v[0];
            if (v[2] < z0) z0 = v[2];
            if (v[2] > z1) z1 = v[2];
            hmean += out->verts[p[j] * 3 + 2];
        }
        hmean /= (float)nv;
        // the height grid: each sample the poly's region's floor in the cells around that corner (else the mean)
        int dw = (x1 - x0 + step - 1) / step + 1, dh = (z1 - z0 + step - 1) / step + 1;
        if (dw > 255) dw = 255;
        if (dh > 255) dh = 255;
        P->dw = (uint8_t)dw, P->dh = (uint8_t)dh, P->detail = dat;
        P->dx0 = ox + (float)x0 * g->cs, P->dy0 = oy + (float)z0 * g->cs;
        float zmin = 1e30f, zmax = -1e30f;
        for (int b = 0; b < dh; b++)
            for (int a = 0; a < dw; a++) {
                int sx = x0 + a * step, sz = z0 + b * step;
                if (sx > x1) sx = x1;
                if (sz > z1) sz = z1;
                int best = -1;
                for (int k = 0; k < 4; k++) {
                    int cx = sx - (k & 1) + g->border, cz = sz - (k >> 1) + g->border;
                    if (cx < 0 || cz < 0 || cx >= W || cz >= H) continue;
                    const CCell* cl = &c.cells[cx + cz * W];
                    for (uint32_t si = cl->index; si < cl->index + cl->count; si++)
                        if (c.spans[si].reg == mregs[i] && (int)c.spans[si].y > best) best = c.spans[si].y;
                }
                float hz = best >= 0 ? in->bmin[2] + (float)best * g->ch : hmean;
                out->detail[dat + (uint32_t)(b * dw + a)] = hz;
                zmin = fminf(zmin, hz), zmax = fmaxf(zmax, hz);
            }
        dat += (uint32_t)(dw * dh);
        float* bx = &out->pbox[i * 6];
        bx[0] = bx[1] = bx[2] = 1e30f, bx[3] = bx[4] = bx[5] = -1e30f;
        for (int j = 0; j < nv; j++)
            for (int k = 0; k < 3; k++) bx[k] = fminf(bx[k], out->verts[p[j] * 3 + k]), bx[3 + k] = fmaxf(bx[3 + k], out->verts[p[j] * 3 + k]);
        bx[2] = fminf(bx[2], zmin), bx[5] = fmaxf(bx[5], zmax);
    }
    out->spans = c.nspans, out->regions = nreg, out->contours = (uint32_t)ncont;
done:
    free(hf.cols), free(hf.sp);
    free(c.cells), free(c.spans), free(c.areas), free(flags);
    for (int i = 0; i < ncont; i++) free(conts[i].v);
    free(conts), free(pts.a), free(simp.a);
    free(mverts), free(mpolys), free(mneis), free(mregs), free(mareas), free(first), free(next), free(idx), free(tris), free(cpolys);
    if (out) out->ms = (re_time_seconds() - t0) * 1000.0;
    return out;
}

float nav_poly_height(const NavTileData* t, const NavPoly* p, float x, float y) {
    if (!p->dw || !p->dh) return t->verts[p->v[0] * 3 + 2];
    float gx = (x - p->dx0) / t->dstep, gy = (y - p->dy0) / t->dstep;
    gx = gx < 0 ? 0 : gx > (float)(p->dw - 1) ? (float)(p->dw - 1) : gx;
    gy = gy < 0 ? 0 : gy > (float)(p->dh - 1) ? (float)(p->dh - 1) : gy;
    int ix = (int)gx, iy = (int)gy;
    int ix1 = ix + 1 < p->dw ? ix + 1 : ix, iy1 = iy + 1 < p->dh ? iy + 1 : iy;
    float fx = gx - (float)ix, fy = gy - (float)iy;
    const float* d = &t->detail[p->detail];
    float a = d[iy * p->dw + ix] + (d[iy * p->dw + ix1] - d[iy * p->dw + ix]) * fx;
    float b = d[iy1 * p->dw + ix] + (d[iy1 * p->dw + ix1] - d[iy1 * p->dw + ix]) * fx;
    return a + (b - a) * fy;
}
