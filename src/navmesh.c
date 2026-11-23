// The NavMesh at run time (navmesh.h): its tiles and their links, the queries (Detour's ideas: A* over the polygons, the
// funnel), the Nav Links, and the level followed through the ECS's change ring: what blocks the Character, where, which
// tiles that dirties, the batches built off the main thread (async.h; the game: a thread of its own).
#include "navmesh.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "async.h"
#include "components.h"
#include "debug_viewer.h"
#include "i18n.h"
#include "landscape.h"
#include "navmesh_internal.h"
#include "platform.h"
#include "render.h"

const char* const g_nav_area_names[NAV_AREA_COUNT] = { "Null", "Default", "Obstacle", "Shallow Water" };
const float g_nav_area_cost[NAV_AREA_COUNT] = { 1e6f, 1.0f, 8.0f, 3.0f };

#define TILE_CELLS 96
#define BASE_CELL 12.0f           // cm (the agent's radius in 3 cells)
#define MAX_CELLS 24.0e6f         // the wanted area in cells at most (coarser cells past it)
#define MAX_TILES 16384
#define LEVEL_WHOLE 30000.0f      // cm: a level up to this wide is covered whole; past it, around the agents
#define AROUND_AGENT 12000.0f     // cm each way around an agent (a huge level)
#define BATCH_TILES 64            // tiles per background batch (the nearest to the agents first)
#define SETTLE_S 0.25             // the editor: quiet this long after a change before building
#define MAX_NODES 8192
#define NODE_HASH 16384
#define MAX_CORRIDOR 1024
#define H_SCALE 0.999f
#define OFF_SLOT 0xFFFFu

static inline uint32_t mkref(uint32_t slot, uint32_t poly) { return (slot << 16) | poly; }
static inline uint32_t ref_slot(uint32_t r) { return r >> 16; }
static inline uint32_t ref_poly(uint32_t r) { return r & 0xFFFFu; }
static inline bool ref_off(uint32_t r) { return ref_slot(r) == OFF_SLOT; }

static const int TDX[4] = { -1, 0, 1, 0 }, TDY[4] = { 0, 1, 0, -1 }; // a portal's side: the neighbor tile

typedef struct Link { uint32_t ref, next; uint8_t edge, side, bmin, bmax; } Link;
typedef struct Tile { NavTileData* d; Link* links; uint32_t nlinks, cap; int tx, ty; } Tile;
typedef struct OffMesh { float a[3], b[3]; uint32_t pa, pb; uint8_t both; Entity e; } OffMesh;
typedef struct Src { uint32_t index; Entity e; float mn[3], mx[3]; uint8_t mob, used; } Src;
typedef struct TKey { int32_t x, y; uint32_t v; } TKey; // v: 0 empty, 1 tombstone, else value + 2

// A small open-addressing set / map of tiles (tx, ty) -> uint32
typedef struct TMap { TKey* k; uint32_t cap, n, dead; } TMap;
static uint32_t th(int x, int y) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u); }
static bool tm_grow(TMap* m) {
    uint32_t nc = m->cap ? m->cap * 2 : 256;
    while (nc < (m->n + 1) * 2) nc *= 2;
    TKey* nk = calloc(nc, sizeof(TKey));
    if (!nk) return false;
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->k[i].v >= 2) {
            uint32_t j = th(m->k[i].x, m->k[i].y) & (nc - 1);
            while (nk[j].v) j = (j + 1) & (nc - 1);
            nk[j] = m->k[i];
        }
    free(m->k);
    m->k = nk, m->cap = nc, m->dead = 0;
    return true;
}
static TKey* tm_find(const TMap* m, int x, int y) {
    if (!m->cap) return NULL;
    for (uint32_t j = th(x, y) & (m->cap - 1);; j = (j + 1) & (m->cap - 1)) {
        TKey* k = &m->k[j];
        if (!k->v) return NULL;
        if (k->v >= 2 && k->x == x && k->y == y) return k;
    }
}
static bool tm_put(TMap* m, int x, int y, uint32_t v) {
    TKey* f = tm_find(m, x, y);
    if (f) return f->v = v + 2, true;
    if ((m->n + m->dead + 1) * 2 > m->cap && !tm_grow(m)) return false;
    uint32_t j = th(x, y) & (m->cap - 1);
    while (m->k[j].v >= 2) j = (j + 1) & (m->cap - 1);
    if (m->k[j].v == 1) m->dead--;
    m->k[j] = (TKey){ x, y, v + 2 };
    m->n++;
    return true;
}
static void tm_del(TMap* m, int x, int y) {
    TKey* f = tm_find(m, x, y);
    if (f) f->v = 1, m->n--, m->dead++;
}
static void tm_clear(TMap* m) {
    if (m->k) memset(m->k, 0, sizeof(TKey) * m->cap);
    m->n = m->dead = 0;
}

typedef struct Batch {
    NavTileInput* in;
    NavTileData** out;
    uint32_t n, gen;
    void* mem;                 // the inputs' boxes and heights
    volatile int32_t finished;
    double ms;
    ReThread* thread;          // the game (no async workers)
} Batch;

static struct {
    NavCfg cfg;
    bool have_cfg;
    Tile* tiles;               // [slot] (0 unused)
    uint32_t ntiles, cap_tiles;
    uint32_t* free_slots;
    uint32_t nfree;
    TMap at;                   // (tx, ty) -> slot
    TMap want;                 // the tiles the NavMesh covers
    TMap dirty;                // to build
    OffMesh* off;
    uint32_t noff, cap_off;
    Src* src;                  // the colliders as last built (open addressing by entity index)
    uint32_t cap_src, nsrc, dsrc;
    bool sources_changed, nav_touched, agents_touched;
    uint32_t want_hash;
    uint32_t changes, revision;
    bool scanned, active, wanted_by_user, stale;
    uint32_t gen;              // bumps on a reset: a batch made before is dropped
    Batch* batch;
    double last_change, next_agents;
    float focus[3];
    float content_mn[3], content_mx[3];
    bool content_ok;
    // the nav actors as last seen (a change: wanted tiles, modifiers' tiles, links)
    uint32_t nav_hash;
    NavInArea* areas;
    uint32_t nareas, cap_areas;
    float vol[64][4];          // the bounds volumes (xy rects)
    uint32_t nvol;
    uint32_t mode;
    bool force;                // navmesh_rebuild / navmesh_wait: no settling
    NavMeshStats st;
    double query_sum;
} M;

static void reset_all(void);

// --- tiles ---
static Tile* tile_of(uint32_t slot) { return slot && slot < M.ntiles && M.tiles[slot].d ? &M.tiles[slot] : NULL; }
static uint32_t slot_at(int tx, int ty) {
    TKey* k = tm_find(&M.at, tx, ty);
    return k ? k->v - 2 : 0;
}
static bool poly_of(uint32_t ref, const Tile** t, const NavPoly** p) {
    if (ref_off(ref)) return false;
    const Tile* tl = tile_of(ref_slot(ref));
    if (!tl || ref_poly(ref) >= tl->d->npolys) return false;
    *t = tl, *p = &tl->d->polys[ref_poly(ref)];
    return true;
}
static const float* pv(const Tile* t, const NavPoly* p, int j) { return &t->d->verts[p->v[j % p->nv] * 3]; }

static void add_link(Tile* t, uint32_t poly, uint32_t ref, uint8_t edge, uint8_t side, uint8_t bmin, uint8_t bmax) {
    if (t->nlinks == t->cap) {
        uint32_t c = t->cap ? t->cap * 2 : 256;
        Link* nl = realloc(t->links, sizeof(Link) * c); // (a tile's links: when it changes, not a frame)
        if (!nl) return;
        t->links = nl, t->cap = c;
    }
    NavPoly* p = &t->d->polys[poly];
    t->links[t->nlinks] = (Link){ ref, p->first_link, edge, side, bmin, bmax };
    p->first_link = t->nlinks++;
}

// Detour's overlapSlabs: two edges on the same tile side (1D along it, with their heights) that meet
static bool overlap_slabs(const float amin[2], const float amax[2], const float bmin[2], const float bmax[2], float px, float py) {
    float minx = fmaxf(amin[0] + px, bmin[0] + px), maxx = fminf(amax[0] - px, bmax[0] - px);
    if (minx > maxx) return false;
    float ad = (amax[1] - amin[1]) / fmaxf(amax[0] - amin[0], 1e-6f), ak = amin[1] - ad * amin[0];
    float bd = (bmax[1] - bmin[1]) / fmaxf(bmax[0] - bmin[0], 1e-6f), bk = bmin[1] - bd * bmin[0];
    float dmin = (bd * minx + bk) - (ad * minx + ak), dmax = (bd * maxx + bk) - (ad * maxx + ak);
    if (dmin * dmax < 0) return true;
    float thr = (py * 2) * (py * 2);
    return dmin * dmin <= thr || dmax * dmax <= thr;
}
static void slab(const float* va, const float* vb, int axis, float mn[2], float mx[2]) {
    if (va[axis] < vb[axis]) mn[0] = va[axis], mn[1] = va[2], mx[0] = vb[axis], mx[1] = vb[2];
    else mn[0] = vb[axis], mn[1] = vb[2], mx[0] = va[axis], mx[1] = va[2];
}

static void relink(uint32_t slot) {
    Tile* t = tile_of(slot);
    if (!t) return;
    t->nlinks = 0;
    NavTileData* d = t->d;
    for (uint32_t i = 0; i < d->npolys; i++) d->polys[i].first_link = NAV_NO_LINK;
    for (uint32_t i = 0; i < d->npolys; i++) {
        NavPoly* p = &d->polys[i];
        for (int j = 0; j < p->nv; j++) {
            uint16_t nb = p->nei[j];
            if (!nb) continue;
            if (!(nb & NAV_PORTAL)) {
                add_link(t, i, mkref(slot, nb - 1u), (uint8_t)j, 0xFF, 0, 255);
                continue;
            }
            int dir = nb & 3;
            uint32_t ns = slot_at(t->tx + TDX[dir], t->ty + TDY[dir]);
            const Tile* n = tile_of(ns);
            if (!n) continue;
            int axis = (dir == 0 || dir == 2) ? 1 : 0, opp = (dir + 2) & 3;
            const float *va = pv(t, p, j), *vb = pv(t, p, j + 1);
            float amn[2], amx[2];
            slab(va, vb, axis, amn, amx);
            float side = va[axis ^ 1]; // (the border's line)
            for (uint32_t k = 0; k < n->d->npolys; k++) {
                const NavPoly* q = &n->d->polys[k];
                const float* qb = &n->d->pbox[k * 6];
                if (qb[axis ^ 1] > side + 1.0f || qb[3 + (axis ^ 1)] < side - 1.0f) continue; // (not on that side)
                for (int e = 0; e < q->nv; e++) {
                    if (q->nei[e] != (NAV_PORTAL | (uint16_t)opp)) continue;
                    const float *qa = pv(n, q, e), *qc = pv(n, q, e + 1);
                    float bmn[2], bmx[2];
                    slab(qa, qc, axis, bmn, bmx);
                    if (!overlap_slabs(amn, amx, bmn, bmx, 0.01f, M.cfg.climb)) continue;
                    float len = vb[axis] - va[axis];
                    float tmin = (fmaxf(amn[0], bmn[0]) - va[axis]) / len, tmax = (fminf(amx[0], bmx[0]) - va[axis]) / len;
                    if (tmin > tmax) { float s = tmin; tmin = tmax, tmax = s; }
                    tmin = fminf(fmaxf(tmin, 0.0f), 1.0f), tmax = fminf(fmaxf(tmax, 0.0f), 1.0f);
                    add_link(t, i, mkref(ns, k), (uint8_t)j, (uint8_t)dir, (uint8_t)lroundf(tmin * 255.0f), (uint8_t)lroundf(tmax * 255.0f));
                }
            }
        }
        // the Nav Links that start here (or end here, both ways)
        uint32_t me = mkref(slot, i);
        for (uint32_t k = 0; k < M.noff; k++) {
            if (M.off[k].pa == me && M.off[k].pb) add_link(t, i, mkref(OFF_SLOT, k), 0xFF, 0xFF, 0, 255);
            else if (M.off[k].both && M.off[k].pb == me && M.off[k].pa) add_link(t, i, mkref(OFF_SLOT, k), 0xFF, 0xFF, 0, 255);
        }
    }
}

static void tile_remove(int tx, int ty) {
    uint32_t s = slot_at(tx, ty);
    if (!s) return;
    Tile* t = &M.tiles[s];
    free(t->d), free(t->links);
    memset(t, 0, sizeof *t);
    tm_del(&M.at, tx, ty);
    uint32_t* nf = realloc(M.free_slots, sizeof(uint32_t) * (M.nfree + 1));
    if (nf) M.free_slots = nf, M.free_slots[M.nfree++] = s;
}
static uint32_t tile_put(NavTileData* d) {
    tile_remove(d->tx, d->ty);
    uint32_t s = 0;
    if (M.nfree) s = M.free_slots[--M.nfree];
    else {
        if (M.ntiles == 0) M.ntiles = 1; // (slot 0: none)
        if (M.ntiles >= OFF_SLOT) return free(d), 0;
        if (M.ntiles >= M.cap_tiles) {
            uint32_t c = M.cap_tiles ? M.cap_tiles * 2 : 256;
            Tile* nt = realloc(M.tiles, sizeof(Tile) * c);
            if (!nt) return free(d), 0;
            memset(nt + M.cap_tiles, 0, sizeof(Tile) * (c - M.cap_tiles));
            M.tiles = nt, M.cap_tiles = c;
        }
        s = M.ntiles++;
    }
    Tile* t = &M.tiles[s];
    memset(t, 0, sizeof *t);
    t->d = d, t->tx = d->tx, t->ty = d->ty;
    tm_put(&M.at, d->tx, d->ty, s);
    return s;
}
static void tiles_clear(void) {
    for (uint32_t s = 1; s < M.ntiles; s++) free(M.tiles[s].d), free(M.tiles[s].links);
    if (M.tiles) memset(M.tiles, 0, sizeof(Tile) * M.cap_tiles);
    M.ntiles = 0, M.nfree = 0;
    tm_clear(&M.at);
    for (uint32_t k = 0; k < M.noff; k++) M.off[k].pa = M.off[k].pb = 0;
}

// --- geometry ---
static float dist2d_sq(const float* a, const float* b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }
static float dist3(const float* a, const float* b) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
static float tri2(const float* a, const float* b, const float* c) { // Detour's dtTriArea2D (x, y)
    float abx = b[0] - a[0], aby = b[1] - a[1], acx = c[0] - a[0], acy = c[1] - a[1];
    return acx * aby - abx * acy;
}
static bool point_in_poly(const Tile* t, const NavPoly* p, const float* pt) {
    bool c = false;
    for (int i = 0, j = p->nv - 1; i < p->nv; j = i++) {
        const float *vi = pv(t, p, i), *vj = pv(t, p, j);
        if (((vi[1] > pt[1]) != (vj[1] > pt[1])) && (pt[0] < (vj[0] - vi[0]) * (pt[1] - vi[1]) / (vj[1] - vi[1]) + vi[0])) c = !c;
    }
    return c;
}
static float seg_closest(const float* p, const float* a, const float* b, float out[2]) { // squared, 2D
    float dx = b[0] - a[0], dy = b[1] - a[1], d = dx * dx + dy * dy, t = 0.0f;
    if (d > 0) t = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / d;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    out[0] = a[0] + dx * t, out[1] = a[1] + dy * t;
    return (out[0] - p[0]) * (out[0] - p[0]) + (out[1] - p[1]) * (out[1] - p[1]);
}
// The polygon's nearest point to p; its distance (inside: how far above or below, less the climb)
static float closest_on_poly(const Tile* t, const NavPoly* p, const float* pos, float out[3]) {
    if (point_in_poly(t, p, pos)) {
        out[0] = pos[0], out[1] = pos[1], out[2] = nav_poly_height(t->d, p, pos[0], pos[1]);
        float dz = fabsf(out[2] - pos[2]) - M.cfg.climb;
        return dz > 0 ? dz * dz : 0.0f;
    }
    float best = 1e30f, q[2];
    for (int j = 0; j < p->nv; j++) {
        float c[2], d = seg_closest(pos, pv(t, p, j), pv(t, p, j + 1), c);
        if (d < best) best = d, q[0] = c[0], q[1] = c[1];
    }
    out[0] = q[0], out[1] = q[1], out[2] = nav_poly_height(t->d, p, q[0], q[1]);
    return best + (out[2] - pos[2]) * (out[2] - pos[2]);
}
static uint32_t nearest_poly(const float* pos, const float* ext, float out[3]) {
    if (!M.have_cfg) return 0;
    float ts = (float)TILE_CELLS * M.cfg.cs;
    int x0 = (int)floorf((pos[0] - ext[0]) / ts), x1 = (int)floorf((pos[0] + ext[0]) / ts);
    int y0 = (int)floorf((pos[1] - ext[1]) / ts), y1 = (int)floorf((pos[1] + ext[1]) / ts);
    uint32_t best = 0;
    float bd = 1e30f;
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            uint32_t s = slot_at(tx, ty);
            const Tile* t = tile_of(s);
            if (!t) continue;
            for (uint32_t i = 0; i < t->d->npolys; i++) {
                const float* b = &t->d->pbox[i * 6];
                if (b[0] > pos[0] + ext[0] || b[3] < pos[0] - ext[0] || b[1] > pos[1] + ext[1] || b[4] < pos[1] - ext[1] ||
                    b[2] > pos[2] + ext[2] || b[5] < pos[2] - ext[2])
                    continue;
                float c[3], d = closest_on_poly(t, &t->d->polys[i], pos, c);
                if (d < bd) {
                    bd = d, best = mkref(s, i);
                    if (out) memcpy(out, c, 12);
                }
            }
        }
    return best;
}

// The portal between two neighbors of a corridor (left, right as Detour's): a shared edge, or a Nav Link's end
static bool portal(uint32_t from, uint32_t to, float left[3], float right[3]) {
    if ((ref_off(to) && ref_poly(to) >= M.noff) || (ref_off(from) && ref_poly(from) >= M.noff)) return false;
    if (ref_off(to)) {
        const OffMesh* o = &M.off[ref_poly(to)];
        const float* p = o->pa == from ? o->a : o->b;
        memcpy(left, p, 12), memcpy(right, p, 12);
        return true;
    }
    if (ref_off(from)) {
        const OffMesh* o = &M.off[ref_poly(from)];
        const float* p = o->pb == to ? o->b : o->a;
        memcpy(left, p, 12), memcpy(right, p, 12);
        return true;
    }
    const Tile* t;
    const NavPoly* p;
    if (!poly_of(from, &t, &p)) return false;
    for (uint32_t l = p->first_link; l != NAV_NO_LINK; l = t->links[l].next) {
        const Link* k = &t->links[l];
        if (k->ref != to || k->edge == 0xFF) continue;
        const float *v0 = pv(t, p, k->edge), *v1 = pv(t, p, k->edge + 1);
        if (k->side != 0xFF && (k->bmin != 0 || k->bmax != 255)) {
            float a = k->bmin / 255.0f, b = k->bmax / 255.0f;
            for (int i = 0; i < 3; i++) left[i] = v0[i] + (v1[i] - v0[i]) * a, right[i] = v0[i] + (v1[i] - v0[i]) * b;
        } else
            memcpy(left, v0, 12), memcpy(right, v1, 12);
        return true;
    }
    return false;
}

// --- A* over the polygons (Detour's findPath) ---
typedef struct Node { uint32_t ref, parent, heap; float pos[3], cost, total; uint8_t state; } Node; // state 1 open, 2 closed
static Node g_nodes[MAX_NODES];
static uint32_t g_hash[NODE_HASH]; // node + 1
static uint32_t g_heap[MAX_NODES];
static uint32_t g_nnodes, g_nheap;

static uint32_t node_hash(uint32_t r) { return (r * 2654435761u) & (NODE_HASH - 1); }
static void pool_reset(void) { memset(g_hash, 0, sizeof g_hash), g_nnodes = g_nheap = 0; }
static Node* node_get(uint32_t ref, bool make) {
    uint32_t h = node_hash(ref);
    for (;; h = (h + 1) & (NODE_HASH - 1)) {
        if (!g_hash[h]) break;
        if (g_nodes[g_hash[h] - 1].ref == ref) return &g_nodes[g_hash[h] - 1];
    }
    if (!make || g_nnodes >= MAX_NODES) return NULL;
    Node* n = &g_nodes[g_nnodes++];
    memset(n, 0, sizeof *n);
    n->ref = ref, n->parent = 0xFFFFFFFFu;
    g_hash[h] = g_nnodes;
    return n;
}
static void heap_up(uint32_t i) {
    uint32_t n = g_heap[i];
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (g_nodes[g_heap[p]].total <= g_nodes[n].total) break;
        g_heap[i] = g_heap[p], g_nodes[g_heap[i]].heap = i, i = p;
    }
    g_heap[i] = n, g_nodes[n].heap = i;
}
static void heap_push(Node* nd) { g_heap[g_nheap] = (uint32_t)(nd - g_nodes), heap_up(g_nheap++); }
static Node* heap_pop(void) {
    uint32_t top = g_heap[0];
    g_heap[0] = g_heap[--g_nheap];
    for (uint32_t i = 0;;) {
        uint32_t l = i * 2 + 1, r = l + 1, m = i;
        if (l < g_nheap && g_nodes[g_heap[l]].total < g_nodes[g_heap[m]].total) m = l;
        if (r < g_nheap && g_nodes[g_heap[r]].total < g_nodes[g_heap[m]].total) m = r;
        if (m == i) break;
        uint32_t t = g_heap[m];
        g_heap[m] = g_heap[i], g_heap[i] = t;
        g_nodes[g_heap[i]].heap = i, g_nodes[g_heap[m]].heap = m;
        i = m;
    }
    if (g_nheap) g_nodes[g_heap[0]].heap = 0;
    return &g_nodes[top];
}
static float area_cost(uint32_t ref) {
    const Tile* t;
    const NavPoly* p;
    return poly_of(ref, &t, &p) && p->area < NAV_AREA_COUNT ? g_nav_area_cost[p->area] : 1.0f;
}

// The neighbors of a node: a polygon's links; a Nav Link's other end
typedef void (*VisitFn)(Node* cur, uint32_t nb, void* u);
static void neighbors(Node* cur, VisitFn fn, void* u) {
    uint32_t parent_ref = cur->parent != 0xFFFFFFFFu ? g_nodes[cur->parent].ref : 0;
    if (ref_off(cur->ref)) {
        if (ref_poly(cur->ref) >= M.noff) return;
        const OffMesh* o = &M.off[ref_poly(cur->ref)];
        fn(cur, parent_ref == o->pa ? o->pb : o->pa, u);
        return;
    }
    const Tile* t;
    const NavPoly* p;
    if (!poly_of(cur->ref, &t, &p)) return;
    for (uint32_t l = p->first_link; l != NAV_NO_LINK; l = t->links[l].next)
        if (t->links[l].ref != parent_ref) fn(cur, t->links[l].ref, u);
}

typedef struct Search { uint32_t eref; float epos[3]; Node* best; float best_h; } Search;
static void visit_path(Node* cur, uint32_t nb, void* u) {
    Search* s = u;
    if (!nb) return;
    Node* n = node_get(nb, true);
    if (!n) return;
    if (!n->state && n->parent == 0xFFFFFFFFu) { // first seen: its place is the portal's middle
        float l[3], r[3];
        if (!portal(cur->ref, nb, l, r)) return;
        for (int k = 0; k < 3; k++) n->pos[k] = (l[k] + r[k]) * 0.5f;
    }
    float cost = cur->cost + dist3(cur->pos, n->pos) * (ref_off(cur->ref) ? 1.0f : area_cost(cur->ref)), h;
    if (nb == s->eref) cost += dist3(n->pos, s->epos) * area_cost(nb), h = 0.0f;
    else h = dist3(n->pos, s->epos) * H_SCALE;
    float total = cost + h;
    if (n->state && total >= n->total) return;
    n->parent = (uint32_t)(cur - g_nodes), n->cost = cost, n->total = total;
    if (n->state == 1) heap_up(n->heap);
    else n->state = 1, heap_push(n);
    if (h < s->best_h) s->best_h = h, s->best = n;
}

static int find_corridor(uint32_t sref, const float spos[3], uint32_t eref, const float epos[3], uint32_t* path, int cap, bool* partial) {
    *partial = false;
    if (sref == eref) return path[0] = sref, 1;
    pool_reset();
    Node* s = node_get(sref, true);
    memcpy(s->pos, spos, 12);
    s->cost = 0, s->total = dist3(spos, epos) * H_SCALE, s->state = 1;
    heap_push(s);
    Search ctx = { eref, { epos[0], epos[1], epos[2] }, s, s->total };
    Node* last = NULL;
    while (g_nheap) {
        Node* cur = heap_pop();
        cur->state = 2;
        if (cur->ref == eref) {
            last = cur;
            break;
        }
        neighbors(cur, visit_path, &ctx);
    }
    if (!last) last = ctx.best, *partial = true;
    int n = 0;
    for (Node* k = last; k; k = k->parent != 0xFFFFFFFFu ? &g_nodes[k->parent] : NULL) n++;
    if (n > cap) *partial = true;
    int i = n < cap ? n : cap;
    // (too long: the first cap polygons from the start)
    Node* k = last;
    for (int skip = n - i; skip > 0; skip--) k = &g_nodes[k->parent];
    int len = i;
    while (k && i > 0) path[--i] = k->ref, k = k->parent != 0xFFFFFFFFu ? &g_nodes[k->parent] : NULL;
    return len;
}

// --- the funnel (Detour's findStraightPath), one stretch of polygons between two points ---
typedef struct Out { float (*p)[3]; uint8_t* f; uint32_t cap, n; } Out;
static void out_add(Out* o, const float* p, uint8_t flag) {
    if (o->n && dist2d_sq(o->p[o->n - 1], p) < 0.01f && fabsf(o->p[o->n - 1][2] - p[2]) < 1.0f) {
        if (o->f) o->f[o->n - 1] |= flag;
        return;
    }
    if (o->n >= o->cap) return;
    memcpy(o->p[o->n], p, 12);
    if (o->f) o->f[o->n] = flag;
    o->n++;
}
static bool veq2(const float* a, const float* b) { return dist2d_sq(a, b) < 0.0001f; }
static void funnel(const uint32_t* polys, int n, const float start[3], const float end[3], Out* o) {
    float apex[3], left[3], right[3];
    memcpy(apex, start, 12), memcpy(left, start, 12), memcpy(right, start, 12);
    int apex_i = 0, left_i = 0, right_i = 0;
    for (int i = 0; i < n; i++) {
        float pl[3], pr[3];
        if (i + 1 < n) {
            if (!portal(polys[i], polys[i + 1], pl, pr)) memcpy(pl, end, 12), memcpy(pr, end, 12);
            if (i == 0) {
                float c[2];
                if (seg_closest(apex, pl, pr, c) < 1e-6f) continue;
            }
        } else
            memcpy(pl, end, 12), memcpy(pr, end, 12);
        if (tri2(apex, right, pr) <= 0.0f) {
            if (veq2(apex, right) || tri2(apex, left, pr) > 0.0f) memcpy(right, pr, 12), right_i = i;
            else {
                memcpy(apex, left, 12), apex_i = left_i;
                out_add(o, apex, 0);
                memcpy(left, apex, 12), memcpy(right, apex, 12), left_i = right_i = apex_i;
                i = apex_i;
                continue;
            }
        }
        if (tri2(apex, left, pl) >= 0.0f) {
            if (veq2(apex, left) || tri2(apex, right, pl) < 0.0f) memcpy(left, pl, 12), left_i = i;
            else {
                memcpy(apex, right, 12), apex_i = right_i;
                out_add(o, apex, 0);
                memcpy(left, apex, 12), memcpy(right, apex, 12), left_i = right_i = apex_i;
                i = apex_i;
                continue;
            }
        }
    }
    out_add(o, end, 0);
}

static double q_begin(void) { return re_time_seconds(); }
static void q_end(double t0) {
    double ms = (re_time_seconds() - t0) * 1000.0;
    M.st.queries++, M.st.query_ms_last = ms;
    if (ms > M.st.query_ms_max) M.st.query_ms_max = ms;
    M.query_sum += ms;
    M.st.query_us_avg = M.query_sum * 1000.0 / M.st.queries;
}

static const float k_ext[3] = { 150.0f, 150.0f, 300.0f }; // (where a path's ends look for the mesh)

bool navmesh_path(const float from[3], const float to[3], float (*out)[3], uint8_t* flags, uint32_t cap, uint32_t* n) {
    *n = 0;
    if (!navmesh_ready() || !cap) return false;
    double t0 = q_begin();
    float sp[3], ep[3];
    uint32_t sref = nearest_poly(from, k_ext, sp);
    float far_ext[3] = { 600.0f, 600.0f, 600.0f };
    uint32_t eref = nearest_poly(to, k_ext, ep);
    if (!eref) eref = nearest_poly(to, far_ext, ep); // (a goal off the mesh: its nearest place)
    if (!sref || !eref) return q_end(t0), false;
    static uint32_t corridor[MAX_CORRIDOR];
    bool partial;
    int nc = find_corridor(sref, sp, eref, ep, corridor, MAX_CORRIDOR, &partial);
    if (nc <= 0) return q_end(t0), false;
    float goal[3];
    if (partial) { // the nearest point of the last polygon to the goal
        const Tile* t;
        const NavPoly* p;
        if (poly_of(corridor[nc - 1], &t, &p)) closest_on_poly(t, p, ep, goal);
        else memcpy(goal, ep, 12);
    } else
        memcpy(goal, ep, 12);
    // the funnel stretch by stretch, split at the Nav Links (their start and landing points kept, flagged)
    Out o = { out, flags, cap, 0 };
    float at[3];
    memcpy(at, sp, 12);
    int s0 = 0;
    for (int i = 0; i <= nc; i++) {
        bool link = i < nc && ref_off(corridor[i]);
        if (!link && i < nc) continue;
        if (link && ref_poly(corridor[i]) >= M.noff) break;
        if (link) {
            const OffMesh* om = &M.off[ref_poly(corridor[i])];
            bool fwd = i > 0 && om->pa == corridor[i - 1];
            const float *a = fwd ? om->a : om->b, *b = fwd ? om->b : om->a;
            if (i > s0) funnel(&corridor[s0], i - s0, at, a, &o);
            // walked off when it only drops a little way out; else a jump
            float hz = sqrtf(dist2d_sq(a, b));
            uint8_t f = NAV_PT_LINK | ((b[2] < a[2] - M.cfg.climb && hz < 200.0f) ? 0 : NAV_PT_LINK_JUMP);
            out_add(&o, a, f);
            if (o.f && o.n) o.f[o.n - 1] = f; // (the start point carries the link even when merged)
            out_add(&o, b, 0);
            memcpy(at, b, 12);
            s0 = i + 1;
        } else if (i > s0)
            funnel(&corridor[s0], i - s0, at, goal, &o);
        else
            out_add(&o, goal, 0);
    }
    *n = o.n;
    q_end(t0);
    return o.n > 0;
}

float navmesh_path_length(const float from[3], const float to[3]) {
    float pts[128][3];
    uint32_t n = 0;
    if (!navmesh_path(from, to, pts, NULL, 128, &n) || !n) return -1.0f;
    float len = dist3(from, pts[0]);
    for (uint32_t i = 1; i < n; i++) len += dist3(pts[i - 1], pts[i]);
    return len;
}

bool navmesh_project(const float p[3], const float extent[3], float out[3]) {
    if (!navmesh_ready()) return false;
    double t0 = q_begin();
    bool ok = nearest_poly(p, extent, out) != 0;
    q_end(t0);
    return ok;
}

// --- a random reachable point in a radius (Detour's findRandomPointAroundCircle) ---
static uint32_t g_rng = 0x9E3779B9u;
static float rnd(void) {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / 16777216.0f;
}
static float poly_area(const Tile* t, const NavPoly* p) {
    float a = 0.0f;
    for (int j = 2; j < p->nv; j++) a += fabsf(tri2(pv(t, p, 0), pv(t, p, j - 1), pv(t, p, j))) * 0.5f;
    return a;
}
static void point_in(const Tile* t, const NavPoly* p, float out[3]) {
    float total = poly_area(t, p), r = rnd() * total, acc = 0.0f;
    int tri = 2;
    for (int j = 2; j < p->nv; j++) {
        acc += fabsf(tri2(pv(t, p, 0), pv(t, p, j - 1), pv(t, p, j))) * 0.5f;
        if (r <= acc) { tri = j; break; }
        tri = j;
    }
    float s = rnd(), u = rnd();
    if (s + u > 1.0f) s = 1.0f - s, u = 1.0f - u;
    const float *a = pv(t, p, 0), *b = pv(t, p, tri - 1), *c = pv(t, p, tri);
    out[0] = a[0] + (b[0] - a[0]) * s + (c[0] - a[0]) * u;
    out[1] = a[1] + (b[1] - a[1]) * s + (c[1] - a[1]) * u;
    out[2] = nav_poly_height(t->d, p, out[0], out[1]);
}
typedef struct RandCtx { float c[3], r2, sum; uint32_t pick; } RandCtx;
static void visit_rand(Node* cur, uint32_t nb, void* u) {
    RandCtx* r = u;
    if (!nb) return;
    float l[3], rt[3];
    if (!portal(cur->ref, nb, l, rt)) return;
    float c2[2];
    if (seg_closest(r->c, l, rt, c2) > r->r2) return; // (the portal outside the circle)
    Node* n = node_get(nb, true);
    if (!n || n->state) return;
    for (int k = 0; k < 3; k++) n->pos[k] = (l[k] + rt[k]) * 0.5f;
    n->parent = (uint32_t)(cur - g_nodes), n->cost = cur->cost + dist3(cur->pos, n->pos), n->total = n->cost, n->state = 1;
    heap_push(n);
}
bool navmesh_random_point(const float around[3], float radius, float out[3]) {
    if (!navmesh_ready() || radius <= 0.0f) return false;
    double t0 = q_begin();
    float sp[3];
    uint32_t sref = nearest_poly(around, k_ext, sp);
    if (!sref) return q_end(t0), false;
    pool_reset();
    Node* s = node_get(sref, true);
    memcpy(s->pos, sp, 12), s->state = 1;
    heap_push(s);
    RandCtx ctx = { { sp[0], sp[1], sp[2] }, radius * radius, 0.0f, 0 };
    while (g_nheap) {
        Node* cur = heap_pop();
        cur->state = 2;
        const Tile* t;
        const NavPoly* p;
        if (poly_of(cur->ref, &t, &p)) { // (each polygon by its area: reservoir sampling)
            float a = poly_area(t, p);
            ctx.sum += a;
            if (rnd() * ctx.sum <= a) ctx.pick = cur->ref;
        }
        neighbors(cur, visit_rand, &ctx);
    }
    const Tile* t;
    const NavPoly* p;
    bool ok = ctx.pick && poly_of(ctx.pick, &t, &p);
    if (ok) {
        float best[3], bd = 1e30f;
        for (int k = 0; k < 8; k++) { // (a point of the polygon inside the circle; else the nearest tried)
            float q[3];
            point_in(t, p, q);
            float d = dist2d_sq(q, sp);
            if (d < bd) bd = d, memcpy(best, q, 12);
            if (d <= ctx.r2) break;
        }
        memcpy(out, best, 12);
    }
    q_end(t0);
    return ok;
}

// --- the level: what blocks, the nav actors ---
static void aabb_of(const CTransform* t, const float c[3], const float h[3], float mn[3], float mx[3]) {
    float rows[3][4];
    render_transform_rows(t, rows);
    for (int k = 0; k < 3; k++) {
        float m = rows[k][3], e = 0.0f;
        for (int j = 0; j < 3; j++) m += rows[k][j] * c[j], e += fabsf(rows[k][j]) * h[j];
        e = fmaxf(e, 1.0f);
        mn[k] = m - e, mx[k] = m + e;
    }
}

// What blocks the Character, as bp_vm.c's boxes (build_boxes): a mesh actor's box, a blocking Collision component.
// Not: Characters, AI pawns, projectiles, vehicles, bodies with physics, hidden in game, Collision off
static bool source_of(World* w, Entity e, float mn[3], float mx[3], uint8_t* mob) {
    uint64_t mask = ecs_mask(w, e);
    if (!(mask & COMP_BIT(COMP_TRANSFORM))) return false;
    const CTransform* tr = ecs_get(w, e, COMP_TRANSFORM);
    if (tr->flags & TRANSFORM_HIDDEN_GAME) return false;
    const CRigidBody* rb = (mask & COMP_BIT(COMP_RIGID_BODY)) ? ecs_get(w, e, COMP_RIGID_BODY) : NULL;
    if (rb && rb->simulate) return false;
    *mob = (uint8_t)(tr->flags & TRANSFORM_MOBILITY_MASK);
    const CAttach* at = (mask & COMP_BIT(COMP_ATTACH)) ? ecs_get(w, e, COMP_ATTACH) : NULL;
    if (mask & COMP_BIT(COMP_COLLISION)) {
        const CCollision* co = ecs_get(w, e, COMP_COLLISION);
        if (!co->block || (at && ecs_get(w, at->owner, COMP_CHARACTER))) return false;
        float ex[3], zero[3] = { 0, 0, 0 };
        if (co->shape == COLLISION_BOX) memcpy(ex, co->extent, 12);
        else if (co->shape == COLLISION_SPHERE) ex[0] = ex[1] = ex[2] = co->extent[0];
        else ex[0] = ex[1] = co->extent[0], ex[2] = fmaxf(co->extent[1], co->extent[0]);
        aabb_of(tr, zero, ex, mn, mx);
        if (at && at->owner && ecs_alive(w, at->owner)) { // (a component of a Movable actor moves with it)
            const CTransform* ot = ecs_get(w, at->owner, COMP_TRANSFORM);
            if (ot) *mob = (uint8_t)(ot->flags & TRANSFORM_MOBILITY_MASK);
        }
        return true;
    }
    const uint64_t not_blocking = COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_PROJECTILE) | COMP_BIT(COMP_VEHICLE) | COMP_BIT(COMP_AI_AGENT);
    if (!(mask & COMP_BIT(COMP_STATIC_MESH)) || (mask & not_blocking)) return false;
    if (tr->flags & TRANSFORM_NO_COLLISION) return false;
    if (at && at->kind != ATTACH_PROTO) return false;
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    float lmn[3], lmx[3], c[3], h[3];
    render_mesh_bounds(sm->mesh, lmn, lmx);
    for (int k = 0; k < 3; k++) c[k] = (lmn[k] + lmx[k]) * 0.5f, h[k] = (lmx[k] - lmn[k]) * 0.5f;
    aabb_of(tr, c, h, mn, mx);
    if (at && at->owner && ecs_alive(w, at->owner)) {
        const CTransform* ot = ecs_get(w, at->owner, COMP_TRANSFORM);
        if (ot) *mob = (uint8_t)(ot->flags & TRANSFORM_MOBILITY_MASK);
    }
    return true;
}

// The colliders as built: open addressing by entity index. used: 0 empty, 1 in, 2 a tombstone, 3 in but not yet seen
// by the running full scan
static bool src_live(const Src* s) { return s->used == 1 || s->used == 3; }
static Src* src_find_any(uint32_t index) {
    if (!M.cap_src) return NULL;
    for (uint32_t j = (index * 2654435761u) & (M.cap_src - 1);; j = (j + 1) & (M.cap_src - 1)) {
        if (!M.src[j].used) return NULL;
        if (src_live(&M.src[j]) && M.src[j].index == index) return &M.src[j];
    }
}
static Src* src_find(uint32_t index) {
    Src* s = src_find_any(index);
    return s && s->used == 1 ? s : NULL;
}
static void src_rehash(uint32_t want) {
    uint32_t nc = 1024;
    while (nc < want * 2) nc *= 2;
    Src* ns = calloc(nc, sizeof(Src)); // (a level's colliders: grows with the level, not per frame)
    if (!ns) return;
    for (uint32_t i = 0; i < M.cap_src; i++)
        if (src_live(&M.src[i])) {
            uint32_t j = (M.src[i].index * 2654435761u) & (nc - 1);
            while (ns[j].used) j = (j + 1) & (nc - 1);
            ns[j] = M.src[i];
        }
    free(M.src);
    M.src = ns, M.cap_src = nc, M.dsrc = 0;
}
static Src* src_put(uint32_t index) {
    Src* f = src_find_any(index);
    if (f) return f->used = 1, f;
    if ((M.nsrc + M.dsrc + 1) * 2 > M.cap_src) src_rehash(M.nsrc + 1 > M.cap_src / 4 ? M.nsrc * 2 + 2 : M.nsrc + 1);
    if (!M.cap_src) return NULL;
    uint32_t j = (index * 2654435761u) & (M.cap_src - 1);
    while (src_live(&M.src[j])) j = (j + 1) & (M.cap_src - 1);
    if (M.src[j].used == 2) M.dsrc--;
    M.src[j] = (Src){ index, 0, { 0 }, { 0 }, 0, 1 };
    M.nsrc++;
    return &M.src[j];
}
static void src_del(Src* s) { s->used = 2, M.nsrc--, M.dsrc++; } // (a tombstone: the probe chains stay whole)
static void src_clear(void) {
    if (M.src) memset(M.src, 0, sizeof(Src) * M.cap_src);
    M.nsrc = M.dsrc = 0;
}

static float tile_size(void) { return (float)TILE_CELLS * M.cfg.cs; }
static float border_cm(void) { return (float)M.cfg.border * M.cfg.cs; }

static void dirty_box(const float mn[3], const float mx[3]) {
    if (!M.have_cfg) return;
    float ts = tile_size(), b = border_cm() + 1.0f;
    int x0 = (int)floorf((mn[0] - b) / ts), x1 = (int)floorf((mx[0] + b) / ts);
    int y0 = (int)floorf((mn[1] - b) / ts), y1 = (int)floorf((mx[1] + b) / ts);
    if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > (int64_t)M.want.n * 2 + 64) { // (a huge box: the wanted tiles it covers)
        for (uint32_t i = 0; i < M.want.cap; i++) {
            const TKey* k = &M.want.k[i];
            if (k->v >= 2 && k->x >= x0 && k->x <= x1 && k->y >= y0 && k->y <= y1) tm_put(&M.dirty, k->x, k->y, 0);
        }
        return;
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (tm_find(&M.want, x, y)) tm_put(&M.dirty, x, y, 0);
}

static bool same_box(const Src* s, const float mn[3], const float mx[3]) {
    for (int k = 0; k < 3; k++)
        if (fabsf(s->mn[k] - mn[k]) > 0.5f || fabsf(s->mx[k] - mx[k]) > 0.5f) return false;
    return true;
}

// One entity again: its box now against the one built; both places dirty when it moved, came or went. Playing, a
// Movable one counts where play found it (a door, a platform, a crate: not rebuilt as they move); gone, it leaves
static void process_entity(World* w, uint32_t index, bool playing) {
    Entity e = ecs_entity_at(w, index);
    uint64_t mask = e ? ecs_mask(w, e) : 0;
    if (mask & COMP_BIT(COMP_NAV)) M.nav_touched = true;
    if (mask & COMP_BIT(COMP_AI_AGENT)) M.agents_touched = true;
    Src* r = src_find(index);
    float mn[3], mx[3];
    uint8_t mob = 0;
    bool now = e && source_of(w, e, mn, mx, &mob);
    if (r && r->e != e) { // (the slot reused by another entity: the old one went)
        dirty_box(r->mn, r->mx);
        src_del(r);
        r = NULL, M.sources_changed = true;
    }
    if (playing && M.scanned && e && ((now && mob == MOBILITY_MOVABLE) || (r && r->mob == MOBILITY_MOVABLE))) return;
    if (!now && !r) return;
    if (now && r && same_box(r, mn, mx)) return;
    if (r) dirty_box(r->mn, r->mx);
    if (now) {
        dirty_box(mn, mx);
        if (!r) r = src_put(index);
        if (r) r->e = e, memcpy(r->mn, mn, 12), memcpy(r->mx, mx, 12), r->mob = mob;
    } else
        src_del(r);
    M.sources_changed = true;
    M.last_change = re_time_seconds();
}

// Everything again (a level loaded, play started or stopped, the ring overflowed): only the differences dirty tiles
static void full_scan(World* w, bool playing, bool again) {
    uint32_t upper = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].mask & (COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_COLLISION))) upper += w->archetypes[ai].count;
    if ((M.nsrc + upper + 1) * 2 > M.cap_src) src_rehash(M.nsrc + upper + 1); // (no growth inside the scan)
    for (uint32_t i = 0; i < M.cap_src; i++)
        if (M.src[i].used == 1) M.src[i].used = 3; // (seen below: 1 again)
    bool keep_movable = playing && M.scanned;
    uint32_t moved = 0, total = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !(a->mask & (COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_COLLISION)))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            float mn[3], mx[3];
            uint8_t mob;
            if (!source_of(w, ents[i], mn, mx, &mob)) continue;
            uint32_t index = ents[i] & ((1u << RE_ECS_INDEX_BITS) - 1);
            Src* r = src_find_any(index);
            total++;
            if (r && r->e == ents[i]) {
                bool keep = keep_movable && mob == MOBILITY_MOVABLE; // (a Movable one: where play found it)
                if (!keep && !same_box(r, mn, mx)) dirty_box(r->mn, r->mx), dirty_box(mn, mx), memcpy(r->mn, mn, 12), memcpy(r->mx, mx, 12), moved++;
                r->used = 1, r->mob = mob;
                continue;
            }
            if (r) dirty_box(r->mn, r->mx), src_del(r);
            if (keep_movable && mob == MOBILITY_MOVABLE) continue; // (spawned while playing: not in the NavMesh)
            Src* n = src_put(index);
            if (n) n->e = ents[i], memcpy(n->mn, mn, 12), memcpy(n->mx, mx, 12), n->mob = mob;
            dirty_box(mn, mx);
            moved++;
        }
    }
    for (uint32_t i = 0; i < M.cap_src; i++)
        if (M.src[i].used == 3) {
            Entity e = ecs_entity_at(w, M.src[i].index);
            if (keep_movable && e == M.src[i].e && M.src[i].mob == MOBILITY_MOVABLE) { M.src[i].used = 1; continue; } // (it hid itself: still where it was)
            dirty_box(M.src[i].mn, M.src[i].mx), src_del(&M.src[i]), moved++;
        }
    if (again && total && moved * 2 > total + 8) { // (most of it changed: another level; its old tiles go now, all wanted
        reset_all();                               // again, and the scan from nothing: its Movable actors count too)
        tm_clear(&M.want);
        src_clear();
        M.scanned = false;
        full_scan(w, playing, false);
        M.stale = true;
        return;
    }
    M.scanned = true;
    if (moved) M.last_change = re_time_seconds(), M.sources_changed = true;
}

static void content_bounds(void) {
    M.content_ok = false;
    for (int k = 0; k < 3; k++) M.content_mn[k] = 1e30f, M.content_mx[k] = -1e30f;
    for (uint32_t i = 0; i < M.cap_src; i++) {
        if (M.src[i].used != 1) continue;
        for (int k = 0; k < 3; k++) M.content_mn[k] = fminf(M.content_mn[k], M.src[i].mn[k]), M.content_mx[k] = fmaxf(M.content_mx[k], M.src[i].mx[k]);
        M.content_ok = true;
    }
    float lmn[2], lmx[2];
    if (landscape_bounds(lmn, lmx)) {
        for (int k = 0; k < 2; k++) M.content_mn[k] = fminf(M.content_mn[k], lmn[k]), M.content_mx[k] = fmaxf(M.content_mx[k], lmx[k]);
        M.content_ok = true;
    }
}

// The agent: the largest AI Character's capsule (Unreal's default agent when there is none)
static void agent_size(World* w, float* radius, float* height, float focus[3], uint32_t* nagents, float (*at)[3], uint32_t cap) {
    *radius = 34.0f, *height = 176.0f;
    float r = 0, h = 0;
    uint32_t n = 0;
    float sum[3] = { 0, 0, 0 };
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_AI_AGENT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CCharacter* ch = (a->mask & COMP_BIT(COMP_CHARACTER)) ? ecs_arch_column(a, COMP_CHARACTER) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            if (ch) r = fmaxf(r, ch[i].radius), h = fmaxf(h, ch[i].half_height * 2.0f);
            for (int k = 0; k < 3; k++) sum[k] += tr[i].location[k];
            if (at && n < cap) memcpy(at[n], tr[i].location, 12);
            n++;
        }
    }
    if (r > 1.0f) *radius = r;
    if (h > 1.0f) *height = h;
    if (n) for (int k = 0; k < 3; k++) focus[k] = sum[k] / (float)n;
    *nagents = n;
}

static uint32_t hash_f(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// The nav actors: the bounds volumes (where), the modifiers (areas), the links. A hash says whether they changed
static uint32_t nav_actors_hash(World* w, uint32_t* nmod, uint32_t* nlink) {
    uint32_t h = 2166136261u;
    *nmod = *nlink = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_NAV)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CNav* nv = ecs_arch_column(a, COMP_NAV);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            h = hash_f(h, &nv[i], sizeof(CNav)), h = hash_f(h, tr[i].location, 36), h = hash_f(h, &ents[i], 4);
            *nmod += nv[i].kind == NAV_MODIFIER_VOLUME, *nlink += nv[i].kind == NAV_LINK_PROXY;
        }
    }
    return h;
}

static void read_nav_actors(World* w, uint32_t nmod, uint32_t nlink) {
    if (nmod > M.cap_areas) {
        NavInArea* nar = realloc(M.areas, sizeof(NavInArea) * nmod); // (when the level's modifiers change)
        if (nar) M.areas = nar, M.cap_areas = nmod;
    }
    if (nlink > M.cap_off) {
        OffMesh* no = realloc(M.off, sizeof(OffMesh) * nlink);
        if (no) M.off = no, M.cap_off = nlink;
    }
    M.nvol = M.nareas = M.noff = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_NAV)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CNav* nv = ecs_arch_column(a, COMP_NAV);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            float c[3] = { 0, 0, 0 }, one[3] = { 100.0f, 100.0f, 100.0f }, mn[3], mx[3], rows[3][4];
            if (nv[i].kind == NAV_BOUNDS_VOLUME && M.nvol < 64) {
                aabb_of(&tr[i], c, one, mn, mx);
                M.vol[M.nvol][0] = mn[0], M.vol[M.nvol][1] = mn[1], M.vol[M.nvol][2] = mx[0], M.vol[M.nvol][3] = mx[1];
                M.nvol++;
            } else if (nv[i].kind == NAV_MODIFIER_VOLUME && M.nareas < M.cap_areas) {
                aabb_of(&tr[i], c, one, mn, mx);
                NavInArea* ar = &M.areas[M.nareas++];
                memcpy(ar->mn, mn, 12), memcpy(ar->mx, mx, 12);
                ar->area = (uint8_t)(nv[i].area < NAV_AREA_COUNT ? nv[i].area : NAV_AREA_DEFAULT);
            } else if (nv[i].kind == NAV_LINK_PROXY && M.noff < M.cap_off) {
                render_transform_rows(&tr[i], rows);
                OffMesh* o = &M.off[M.noff++];
                memset(o, 0, sizeof *o);
                for (int k = 0; k < 3; k++) {
                    o->a[k] = rows[k][3] + rows[k][0] * nv[i].left[0] + rows[k][1] * nv[i].left[1] + rows[k][2] * nv[i].left[2];
                    o->b[k] = rows[k][3] + rows[k][0] * nv[i].right[0] + rows[k][1] * nv[i].right[1] + rows[k][2] * nv[i].right[2];
                }
                if (nv[i].direction == NAV_LINK_RIGHT_TO_LEFT) {
                    float t[3];
                    memcpy(t, o->a, 12), memcpy(o->a, o->b, 12), memcpy(o->b, t, 12);
                }
                o->both = nv[i].direction == NAV_LINK_BOTH_WAYS;
                o->e = ents[i];
            }
        }
    }
}

// The Nav Links' ends onto the mesh (their snap radius); the tiles that hold them get their links again
static void anchor_links(World* w, TMap* relink_set) {
    for (uint32_t k = 0; k < M.noff; k++) {
        OffMesh* o = &M.off[k];
        float snap = 60.0f;
        const CNav* nv = ecs_alive(w, o->e) ? ecs_get(w, o->e, COMP_NAV) : NULL;
        if (nv && nv->snap_radius > 1.0f) snap = nv->snap_radius;
        float ext[3] = { snap, snap, fmaxf(snap, M.cfg.height * 0.5f) }, pa[3], pb[3];
        uint32_t old_a = o->pa, old_b = o->pb;
        o->pa = nearest_poly(o->a, ext, pa), o->pb = nearest_poly(o->b, ext, pb);
        if (o->pa) memcpy(o->a, pa, 12);
        if (o->pb) memcpy(o->b, pb, 12);
        uint32_t refs[4] = { old_a, old_b, o->pa, o->pb };
        for (int r = 0; r < 4; r++)
            if (refs[r] && tile_of(ref_slot(refs[r]))) tm_put(relink_set, M.tiles[ref_slot(refs[r])].tx, M.tiles[ref_slot(refs[r])].ty, 0);
    }
}

static void relink_all_in(TMap* set) {
    for (uint32_t i = 0; i < set->cap; i++)
        if (set->k[i].v >= 2) relink(slot_at(set->k[i].x, set->k[i].y));
}

static void count_stats(void) {
    M.st.tiles = M.st.polys = M.st.verts = M.st.links = 0;
    for (uint32_t s = 1; s < M.ntiles; s++)
        if (M.tiles[s].d) M.st.tiles++, M.st.polys += M.tiles[s].d->npolys, M.st.verts += M.tiles[s].d->nverts, M.st.links += M.tiles[s].nlinks;
    M.st.offmesh = M.noff, M.st.offmesh_linked = 0;
    for (uint32_t k = 0; k < M.noff; k++) M.st.offmesh_linked += M.off[k].pa && M.off[k].pb;
    M.st.pending = M.dirty.n;
    M.st.cell = M.have_cfg ? M.cfg.cs : 0.0f, M.st.tile_size = M.have_cfg ? tile_size() : 0.0f;
    M.st.agent_radius = M.cfg.radius, M.st.agent_height = M.cfg.height, M.st.agent_climb = M.cfg.climb;
    M.st.area_mode = M.mode;
}

// --- the batches ---
static void batch_free(Batch* b) {
    if (!b) return;
    if (b->out)
        for (uint32_t i = 0; i < b->n; i++) free(b->out[i]);
    free(b->out), free(b->in), free(b->mem), free(b);
}

static void batch_work(Batch* b, AsyncTask* t) {
    double t0 = re_time_seconds();
    for (uint32_t i = 0; i < b->n; i++) {
        if (t && async_cancelled(t)) break;
        b->out[i] = b->in[i].nboxes || b->in[i].land ? nav_build_tile(&b->in[i]) : NULL;
        if (t) async_progress_n(t, (int)i + 1, (int)b->n);
    }
    b->ms = (re_time_seconds() - t0) * 1000.0;
}
static void async_work(AsyncTask* t, void* arg) { batch_work(arg, t); }
static int thread_work(void* arg) {
    Batch* b = arg;
    re_thread_set_low_priority(true);
    batch_work(b, NULL);
    re_atomic_store(&b->finished, 1);
    return 0;
}

static World* g_apply_world;
static void apply_batch(Batch* b, bool ok) {
    if (M.batch == b) M.batch = NULL;
    if (!ok || b->gen != M.gen) { // (cancelled, or a reset since: its tiles again)
        if (b->gen == M.gen)
            for (uint32_t i = 0; i < b->n; i++) tm_put(&M.dirty, b->in[i].tx, b->in[i].ty, 0);
        batch_free(b);
        return;
    }
    double t0 = re_time_seconds();
    TMap set = { 0 };
    for (uint32_t i = 0; i < b->n; i++) {
        int tx = b->in[i].tx, ty = b->in[i].ty;
        if (!tm_find(&M.want, tx, ty)) { // (no longer wanted)
            tile_remove(tx, ty);
            for (int d = 0; d < 4; d++) tm_put(&set, tx + TDX[d], ty + TDY[d], 0);
            continue;
        }
        if (b->out[i]) {
            M.st.tiles_built++;
            if (b->out[i]->ms > M.st.max_tile_ms) M.st.max_tile_ms = b->out[i]->ms;
            M.st.last_tile_ms = b->out[i]->ms;
            tile_put(b->out[i]);
            b->out[i] = NULL;
        } else
            tile_remove(tx, ty);
        tm_put(&set, tx, ty, 0);
        for (int d = 0; d < 4; d++) tm_put(&set, tx + TDX[d], ty + TDY[d], 0);
    }
    if (g_apply_world) anchor_links(g_apply_world, &set);
    relink_all_in(&set);
    free(set.k);
    M.stale = false;
    M.st.builds++, M.st.last_batch_ms = b->ms, M.st.total_build_ms += b->ms;
    M.st.apply_ms = (re_time_seconds() - t0) * 1000.0;
    count_stats();
    batch_free(b);
}
static void async_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    apply_batch(arg, ok);
}

typedef struct Cnt { uint32_t boxes, areas; } Cnt;
static float g_focus_sort[3];
typedef struct TileXY { int x, y; float d; } TileXY;
static int by_dist(const void* a, const void* b) {
    float da = ((const TileXY*)a)->d, db = ((const TileXY*)b)->d;
    return da < db ? -1 : da > db;
}

// The next batch's inputs, copied now (the worker never reads the world): the boxes over each tile with its border,
// the modifiers, the Landscape's heights
static void start_batch(World* w) {
    double t0 = re_time_seconds();
    uint32_t n = 0;
    TileXY* list = malloc(sizeof(TileXY) * (M.dirty.n ? M.dirty.n : 1));
    if (!list) return;
    float ts = tile_size();
    for (uint32_t i = 0; i < M.dirty.cap; i++)
        if (M.dirty.k[i].v >= 2) {
            float cx = ((float)M.dirty.k[i].x + 0.5f) * ts - g_focus_sort[0], cy = ((float)M.dirty.k[i].y + 0.5f) * ts - g_focus_sort[1];
            list[n++] = (TileXY){ M.dirty.k[i].x, M.dirty.k[i].y, cx * cx + cy * cy };
        }
    qsort(list, n, sizeof(TileXY), by_dist);
    if (n > BATCH_TILES) n = BATCH_TILES;
    Batch* b = calloc(1, sizeof(Batch));
    if (!b) {
        free(list);
        return;
    }
    b->n = n, b->gen = M.gen;
    b->in = calloc(n ? n : 1, sizeof(NavTileInput));
    b->out = calloc(n ? n : 1, sizeof(NavTileData*));
    Cnt* cnt = calloc(n ? n : 1, sizeof(Cnt));
    TMap idx = { 0 };
    if (!b->in || !b->out || !cnt) {
        free(cnt), free(list), batch_free(b);
        return;
    }
    float bw = border_cm();
    for (uint32_t i = 0; i < n; i++) {
        NavTileInput* in = &b->in[i];
        in->tx = list[i].x, in->ty = list[i].y, in->cfg = M.cfg;
        in->bmin[0] = (float)in->tx * ts - bw, in->bmin[1] = (float)in->ty * ts - bw;
        in->bmax[0] = (float)(in->tx + 1) * ts + bw, in->bmax[1] = (float)(in->ty + 1) * ts + bw;
        in->bmin[2] = 1e30f, in->bmax[2] = -1e30f;
        tm_put(&idx, in->tx, in->ty, i);
        tm_del(&M.dirty, in->tx, in->ty);
    }
    // two passes over the colliders: count, then copy (one block for the batch)
    size_t land_n = 0;
    float lmn[2], lmx[2];
    bool land = landscape_bounds(lmn, lmx);
    float lstep = fmaxf(M.cfg.cs * 2.0f, 25.0f);
    for (uint32_t i = 0; i < n && land; i++) {
        NavTileInput* in = &b->in[i];
        if (in->bmax[0] < lmn[0] || in->bmin[0] > lmx[0] || in->bmax[1] < lmn[1] || in->bmin[1] > lmx[1]) continue;
        in->lw = (int)ceilf((in->bmax[0] - in->bmin[0]) / lstep) + 1, in->lh = (int)ceilf((in->bmax[1] - in->bmin[1]) / lstep) + 1;
        in->lstep = lstep;
        land_n += (size_t)in->lw * in->lh;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < M.cap_src; s++) {
            const Src* r = &M.src[s];
            if (r->used != 1) continue;
            int x0 = (int)floorf((r->mn[0] - bw) / ts), x1 = (int)floorf((r->mx[0] + bw) / ts);
            int y0 = (int)floorf((r->mn[1] - bw) / ts), y1 = (int)floorf((r->mx[1] + bw) / ts);
            if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) > (int64_t)n * 4 + 16) { // (a huge box: the batch's tiles under it)
                for (uint32_t i = 0; i < n; i++) {
                    NavTileInput* in = &b->in[i];
                    if (in->tx < x0 || in->tx > x1 || in->ty < y0 || in->ty > y1) continue;
                    if (pass == 0) cnt[i].boxes++;
                    else in->boxes[in->nboxes++] = (NavInBox){ { r->mn[0], r->mn[1], r->mn[2] }, { r->mx[0], r->mx[1], r->mx[2] } };
                }
                continue;
            }
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    TKey* k = tm_find(&idx, x, y);
                    if (!k) continue;
                    uint32_t i = k->v - 2;
                    if (pass == 0) cnt[i].boxes++;
                    else b->in[i].boxes[b->in[i].nboxes++] = (NavInBox){ { r->mn[0], r->mn[1], r->mn[2] }, { r->mx[0], r->mx[1], r->mx[2] } };
                }
        }
        for (uint32_t a = 0; a < M.nareas; a++)
            for (uint32_t i = 0; i < n; i++) {
                NavTileInput* in = &b->in[i];
                const NavInArea* m = &M.areas[a];
                if (m->mx[0] < in->bmin[0] || m->mn[0] > in->bmax[0] || m->mx[1] < in->bmin[1] || m->mn[1] > in->bmax[1]) continue;
                if (pass == 0) cnt[i].areas++;
                else in->areas[in->nareas++] = *m;
            }
        if (pass == 0) {
            size_t bytes = 0;
            for (uint32_t i = 0; i < n; i++) bytes += sizeof(NavInBox) * cnt[i].boxes + sizeof(NavInArea) * cnt[i].areas;
            bytes += sizeof(float) * land_n;
            b->mem = malloc(bytes ? bytes : 1);
            if (!b->mem) {
                free(cnt), free(list), free(idx.k), batch_free(b);
                return;
            }
            uint8_t* p = b->mem;
            for (uint32_t i = 0; i < n; i++) {
                b->in[i].boxes = (NavInBox*)p, p += sizeof(NavInBox) * cnt[i].boxes;
                b->in[i].areas = (NavInArea*)p, p += sizeof(NavInArea) * cnt[i].areas;
            }
            for (uint32_t i = 0; i < n; i++)
                if (b->in[i].lw) b->in[i].land = (float*)p, p += sizeof(float) * (size_t)b->in[i].lw * b->in[i].lh;
        }
    }
    // the Landscape sampled (its heights now: the worker never calls into it), and each tile's height range
    for (uint32_t i = 0; i < n; i++) {
        NavTileInput* in = &b->in[i];
        for (uint32_t k = 0; k < in->nboxes; k++) in->bmin[2] = fminf(in->bmin[2], in->boxes[k].mn[2]), in->bmax[2] = fmaxf(in->bmax[2], in->boxes[k].mx[2]);
        if (in->land) {
            bool any = false;
            for (int y = 0; y < in->lh; y++)
                for (int x = 0; x < in->lw; x++) {
                    float z, *dst = &in->land[y * in->lw + x];
                    if (landscape_height(in->bmin[0] + (float)x * in->lstep, in->bmin[1] + (float)y * in->lstep, &z)) {
                        *dst = z, any = true;
                        in->bmin[2] = fminf(in->bmin[2], z), in->bmax[2] = fmaxf(in->bmax[2], z);
                    } else
                        *dst = NAN;
                }
            if (!any) in->land = NULL;
        }
        if (in->bmin[2] > in->bmax[2]) in->bmin[2] = in->bmax[2] = 0.0f;
        in->bmin[2] -= 100.0f, in->bmax[2] += M.cfg.height + 100.0f;
        if (in->bmax[2] - in->bmin[2] > 0xFFF0 * M.cfg.ch) in->bmin[2] = in->bmax[2] - 0xFFF0 * M.cfg.ch; // (the heights' range)
    }
    free(cnt), free(list), free(idx.k);
    M.st.gather_ms = (re_time_seconds() - t0) * 1000.0;
    M.batch = b;
    g_apply_world = w;
    if (async_ready() && !M.force) { // (navmesh_wait: a thread of its own, waited for alone)
        if (!async_run(TR("Building the navigation (NavMesh)"), async_work, async_done, b, ASYNC_QUIET)) { /* (ran on this thread: done already called) */ }
    } else {
        b->thread = re_thread_start(thread_work, b);
        if (!b->thread) thread_work(b), apply_batch(b, true); // (no thread: right here)
    }
}

static void make_cfg(float radius, float height, float area_cm2, NavCfg* c) {
    memset(c, 0, sizeof *c);
    c->cs = BASE_CELL;
    while (area_cm2 / (c->cs * c->cs) > MAX_CELLS && c->cs < 96.0f) c->cs *= 2.0f;
    c->ch = 5.0f;
    c->radius = radius, c->height = height, c->climb = 45.0f, c->slope_deg = 45.0f;
    c->tile = TILE_CELLS;
    c->border = (int)ceilf(radius / c->cs) + 3;
    c->max_error = 1.3f;
    c->max_edge = (int)(1200.0f / c->cs);
    c->min_region = 8 * 8;
    c->detail_step = 2;
}

static void reset_all(void) {
    tiles_clear();
    tm_clear(&M.dirty);
    M.gen++; // (a batch running is dropped when it ends)
    M.want_hash = 0;
}

// The wanted tiles: the bounds volumes; else the level whole; a huge one around the agents. A change: the new ones
// dirty, the gone ones removed
static void update_wanted(World* w, float radius, float height, uint32_t nagents, float (*agents)[3]) {
    float rects[64][4];
    uint32_t nr = 0;
    if (M.nvol) {
        for (uint32_t i = 0; i < M.nvol; i++) memcpy(rects[nr++], M.vol[i], 16);
        M.mode = 1;
    } else if (M.content_ok && (M.content_mx[0] - M.content_mn[0] <= LEVEL_WHOLE && M.content_mx[1] - M.content_mn[1] <= LEVEL_WHOLE)) {
        rects[nr][0] = M.content_mn[0], rects[nr][1] = M.content_mn[1], rects[nr][2] = M.content_mx[0], rects[nr][3] = M.content_mx[1];
        nr++, M.mode = 2;
    } else if (M.content_ok) {
        M.mode = 3;
        for (uint32_t i = 0; i < nagents && nr < 64; i++) {
            float r[4] = { fmaxf(agents[i][0] - AROUND_AGENT, M.content_mn[0]), fmaxf(agents[i][1] - AROUND_AGENT, M.content_mn[1]),
                           fminf(agents[i][0] + AROUND_AGENT, M.content_mx[0]), fminf(agents[i][1] + AROUND_AGENT, M.content_mx[1]) };
            if (r[0] < r[2] && r[1] < r[3]) memcpy(rects[nr++], r, 16);
        }
        if (!nr) { // (no agent: around the level's middle)
            float c[2] = { (M.content_mn[0] + M.content_mx[0]) * 0.5f, (M.content_mn[1] + M.content_mx[1]) * 0.5f };
            rects[0][0] = c[0] - AROUND_AGENT, rects[0][1] = c[1] - AROUND_AGENT, rects[0][2] = c[0] + AROUND_AGENT, rects[0][3] = c[1] + AROUND_AGENT;
            nr = 1;
        }
    } else
        M.mode = 0;
    float area = 0.0f;
    for (uint32_t i = 0; i < nr; i++) area += (rects[i][2] - rects[i][0]) * (rects[i][3] - rects[i][1]);
    NavCfg c;
    make_cfg(radius, height, area, &c);
    if (!M.have_cfg || memcmp(&c, &M.cfg, sizeof c) != 0) {
        reset_all();
        M.cfg = c, M.have_cfg = true;
        tm_clear(&M.want);
    }
    float ts = tile_size();
    // (the same rectangles: the same tiles; a drag in the editor costs nothing here)
    int32_t ir[64][4];
    for (uint32_t i = 0; i < nr; i++)
        ir[i][0] = (int32_t)floorf(rects[i][0] / ts), ir[i][1] = (int32_t)floorf(rects[i][1] / ts), ir[i][2] = (int32_t)floorf(rects[i][2] / ts),
        ir[i][3] = (int32_t)floorf(rects[i][3] / ts);
    uint32_t wh = hash_f(hash_f(2166136261u, ir, sizeof(int32_t) * 4 * nr), &M.cfg, sizeof M.cfg);
    if (wh == M.want_hash && M.want.k) return;
    M.want_hash = wh;
    TMap nw = { 0 };
    for (uint32_t i = 0; i < nr; i++) {
        int x0 = (int)floorf(rects[i][0] / ts), x1 = (int)floorf(rects[i][2] / ts);
        int y0 = (int)floorf(rects[i][1] / ts), y1 = (int)floorf(rects[i][3] / ts);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1 && nw.n < MAX_TILES; x++) tm_put(&nw, x, y, 0);
    }
    // a huge level around the agents keeps what it made (no flicker as they walk); the rest follows the wanted set
    for (uint32_t i = 0; i < M.want.cap; i++) {
        const TKey* k = &M.want.k[i];
        if (k->v < 2 || tm_find(&nw, k->x, k->y)) continue;
        if (M.mode == 3 && nw.n < MAX_TILES && slot_at(k->x, k->y)) {
            tm_put(&nw, k->x, k->y, 0);
            continue;
        }
        tile_remove(k->x, k->y), tm_del(&M.dirty, k->x, k->y);
        for (int d = 0; d < 4; d++) relink(slot_at(k->x + TDX[d], k->y + TDY[d]));
    }
    for (uint32_t i = 0; i < nw.cap; i++) {
        const TKey* k = &nw.k[i];
        if (k->v >= 2 && !tm_find(&M.want, k->x, k->y)) tm_put(&M.dirty, k->x, k->y, 0);
    }
    free(M.want.k);
    M.want = nw;
    (void)w;
}

// --- each frame ---
void navmesh_update(World* w, bool playing, double now) {
    if (M.batch && M.batch->thread && re_atomic_load(&M.batch->finished)) { // (the game's thread finished)
        Batch* b = M.batch;
        re_thread_join(b->thread);
        b->thread = NULL;
        apply_batch(b, true);
    }
    // active: AI agents, a bounds volume, the P view or Build Paths; never in a Procedural World (it rebases)
    bool agents = false, volumes = false, procgen = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count) continue;
        agents |= (a->mask & COMP_BIT(COMP_AI_AGENT)) != 0;
        volumes |= (a->mask & COMP_BIT(COMP_NAV)) != 0;
        if (a->mask & COMP_BIT(COMP_PROC_WORLD)) { // ((Q15) a PCG Volume isn't a procedural world)
            const CProcWorld* pw = ecs_arch_column(a, COMP_PROC_WORLD);
            for (uint32_t r = 0; r < a->count; r++) procgen |= pw[r].mode != 16u /* PROCGEN_PCG */;
        }
    }
    bool active = !procgen && (agents || volumes || M.wanted_by_user);
    if (!active) {
        if (M.active) { // (gone: everything forgotten, nothing costs)
            reset_all();
            tm_clear(&M.want);
            M.have_cfg = false, M.scanned = false;
            src_clear();
            M.active = false;
            count_stats();
        }
        M.changes = w->changes, M.revision = w->revision;
        return;
    }
    bool first = !M.active;
    M.active = true;
    now = re_time_seconds();
    // 1. what changed since the last frame
    bool full = first || !M.scanned || w->revision != M.revision || w->changes - M.changes > RE_ECS_CHANGE_RING;
    M.sources_changed = M.nav_touched = M.agents_touched = false;
    if (full) full_scan(w, playing, !first);
    else
        for (uint32_t i = M.changes; i != w->changes; i++) process_entity(w, w->changed[i % RE_ECS_CHANGE_RING], playing);
    M.changes = w->changes, M.revision = w->revision;
    // 2. the nav actors, the agents, the wanted tiles: on what changes them (playing, the agents walk every frame:
    // their size is the one play started with; a huge level follows them once a second)
    bool agents_tick = playing && M.mode == 3 && now >= M.next_agents;
    bool again = full || first || agents_tick || M.nav_touched || (!playing && (M.agents_touched || M.sources_changed));
    if (again) {
        static float agents_at[64][3];
        float radius, height;
        uint32_t nag = 0, nmod, nlink;
        agent_size(w, &radius, &height, M.focus, &nag, agents_at, 64);
        if (playing && M.have_cfg) radius = M.cfg.radius, height = M.cfg.height;
        bool size_changed = !M.have_cfg || radius != M.cfg.radius || height != M.cfg.height;
        uint32_t nh = nav_actors_hash(w, &nmod, &nlink);
        bool nav_changed = nh != M.nav_hash || first;
        if (nav_changed) { // the modifiers' tiles (old and new places) again, the links again
            for (uint32_t i = 0; i < M.nareas; i++) dirty_box(M.areas[i].mn, M.areas[i].mx);
            read_nav_actors(w, nmod, nlink);
            for (uint32_t i = 0; i < M.nareas; i++) dirty_box(M.areas[i].mn, M.areas[i].mx);
            M.nav_hash = nh;
            M.last_change = now;
        }
        if (full || first || agents_tick || nav_changed || size_changed || M.sources_changed) { // (an AI moved in the editor: nothing here)
            content_bounds();
            update_wanted(w, radius, height, nag, agents_at);
        }
        if (nav_changed) { // (their indices changed: every tile's links again)
            TMap set = { 0 };
            anchor_links(w, &set);
            for (uint32_t s = 1; s < M.ntiles; s++) relink(s);
            free(set.k);
        }
        if (agents_tick) M.next_agents = now + 1.0;
        count_stats();
    }
    memcpy(g_focus_sort, M.focus, 12);
    // 3. a batch when the level has settled (the editor: a drag rebuilds once, at its end)
    bool settled = playing || M.force || now - M.last_change >= SETTLE_S || M.stale || !M.st.tiles;
    if (!M.batch && M.dirty.n && settled) start_batch(w);
    M.st.pending = M.dirty.n;
}

void navmesh_rebuild(World* w) {
    (void)w;
    for (uint32_t i = 0; i < M.want.cap; i++)
        if (M.want.k[i].v >= 2) tm_put(&M.dirty, M.want.k[i].x, M.want.k[i].y, 0);
    M.wanted_by_user = true;
    M.last_change = 0.0;
}
void navmesh_set_wanted(bool on) { M.wanted_by_user = on; }
void navmesh_reset(void) {
    reset_all();
    tm_clear(&M.want);
    M.have_cfg = false, M.scanned = false, M.active = false;
    src_clear();
    count_stats();
}
bool navmesh_ready(void) { return M.active && M.st.tiles > 0 && !M.stale; }
bool navmesh_building(void) { return M.batch != NULL || M.dirty.n > 0; }

void navmesh_wait(World* w, double max_s) {
    double t0 = re_time_seconds();
    M.force = true;
    for (int guard = 0; guard < 100000; guard++) {
        navmesh_update(w, false, re_time_seconds());
        if (!M.batch && !M.dirty.n) break;
        if (M.batch && M.batch->thread) {
            Batch* b = M.batch;
            re_thread_join(b->thread);
            b->thread = NULL;
            apply_batch(b, true);
        } else if (M.batch) // (one started by a frame before: the async workers')
            async_wait_all(max_s);
        if (re_time_seconds() - t0 > max_s) break;
    }
    M.force = false;
}

const NavMeshStats* navmesh_stats(void) { return &M.st; }

void navmesh_debug(const float center[3], float radius, NavPolyFn poly, NavLinkFn link, void* user) {
    if (!M.have_cfg) return;
    float ts = tile_size();
    int x0 = (int)floorf((center[0] - radius) / ts), x1 = (int)floorf((center[0] + radius) / ts);
    int y0 = (int)floorf((center[1] - radius) / ts), y1 = (int)floorf((center[1] + radius) / ts);
    for (int ty = y0; ty <= y1 && poly; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            const Tile* t = tile_of(slot_at(tx, ty));
            if (!t) continue;
            for (uint32_t i = 0; i < t->d->npolys; i++) {
                const NavPoly* p = &t->d->polys[i];
                float pts[NAV_NVP][3];
                for (int j = 0; j < p->nv; j++) {
                    memcpy(pts[j], pv(t, p, j), 12);
                    pts[j][2] = nav_poly_height(t->d, p, pts[j][0], pts[j][1]);
                }
                poly(user, (const float(*)[3])pts, p->nv, p->area, (tx + ty) & 1);
            }
        }
    for (uint32_t k = 0; k < M.noff && link; k++) link(user, M.off[k].a, M.off[k].b, M.off[k].both, M.off[k].pa && M.off[k].pb);
}

void navmesh_debug_edges(const float center[3], float radius, NavEdgeFn fn, void* user) {
    if (!M.have_cfg) return;
    float ts = tile_size();
    int x0 = (int)floorf((center[0] - radius) / ts), x1 = (int)floorf((center[0] + radius) / ts);
    int y0 = (int)floorf((center[1] - radius) / ts), y1 = (int)floorf((center[1] + radius) / ts);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            const Tile* t = tile_of(slot_at(tx, ty));
            if (!t) continue;
            for (uint32_t i = 0; i < t->d->npolys; i++) {
                const NavPoly* p = &t->d->polys[i];
                for (int j = 0; j < p->nv; j++) {
                    uint16_t nb = p->nei[j];
                    if (nb && !(nb & NAV_PORTAL) && nb - 1u < i) continue; // (an inner edge once)
                    float a[3], b[3];
                    memcpy(a, pv(t, p, j), 12), memcpy(b, pv(t, p, j + 1), 12);
                    a[2] = nav_poly_height(t->d, p, a[0], a[1]), b[2] = nav_poly_height(t->d, p, b[0], b[1]);
                    fn(user, a, b, nb != 0);
                }
            }
        }
}

// --- the Debug Viewer ---
static void collect_nav(DvList* l, const DvContext* c) {
    (void)c;
    const NavMeshStats* s = &M.st;
    l->gpu = false, l->estimated = false;
    l->ms = s->query_ms_last + s->apply_ms;
    snprintf(l->note, sizeof l->note, "%u tiles, %u polygons, %u links (%u Nav Links); cell %.0f cm; %u waiting", s->tiles, s->polys, s->links, s->offmesh,
             s->cell, s->pending);
    char info[48];
    snprintf(info, sizeof info, "%u batches, last %.1f ms", s->builds, s->last_batch_ms);
    dv_add(l, "Build (worker)", info, (float)s->last_tile_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u queries, avg %.1f us", s->queries, s->query_us_avg);
    dv_add(l, "Queries", info, (float)s->query_ms_last, ENTITY_NONE);
    snprintf(info, sizeof info, "gather %.2f ms", s->gather_ms);
    dv_add(l, "Put in place (main thread)", info, (float)s->apply_ms, ENTITY_NONE);
}
void navmesh_register_debug(void) { dv_register("Navigation (NavMesh)", collect_nav); }
