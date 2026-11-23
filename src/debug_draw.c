// (2026-11-19) Debug drawing: debug_draw.h, notas-dev/traces.md
#include "debug_draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define DD_MAX_PRIMS (4u << 20)  // a list's ceiling (128 MB): past it, counted as dropped
#define DD_MAX_GROUPS (1u << 16) // kept runs (a reserve each)

typedef struct Group {
    uint32_t start, count;
    double until; // < 0: until flushed
} Group;

static struct {
    DdPrim* frame;
    uint32_t cap_frame;
    volatile int32_t nframe;
    DdPrim* kept;
    uint32_t cap_kept;
    volatile int32_t nkept;
    Group* groups;
    volatile int32_t ngroups;
    DdTrace* tr; // (this frame's trace records)
    uint32_t cap_tr;
    volatile int32_t ntr, want_tr;
    volatile int32_t want_frame, want_kept; // (the room asked for this frame: grown at the next start)
    volatile int32_t dropped;
    uint32_t dropped_last;
    double now, next_expiry;
    bool off;
} D;

uint32_t dd_rgba(float r, float g, float b, float a) {
    float c[4] = { r, g, b, a };
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        float v = c[k] < 0.0f ? 0.0f : c[k] > 1.0f ? 1.0f : c[k];
        if (k < 3) v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f; // (linear -> sRGB: the swapchain's)
        out |= (uint32_t)(v * 255.0f + 0.5f) << (k * 8);
    }
    return out;
}

void dd_set_enabled(bool on) { D.off = !on; }
bool dd_enabled(void) { return !D.off; }
uint32_t dd_dropped_last(void) { return D.dropped_last; }

static bool grow_list(DdPrim** p, uint32_t* cap, uint32_t need, uint32_t keep);
DdPrim* dd_reserve(uint32_t n, float seconds) {
    if (D.off || !n) return NULL;
    if (!D.groups) { // (the first use before any frame began: a BeginPlay at the start; the main thread)
        D.groups = malloc(sizeof(Group) * DD_MAX_GROUPS);
        grow_list(&D.frame, &D.cap_frame, n > 4096u ? n : 4096u, 0), grow_list(&D.kept, &D.cap_kept, n > 4096u ? n : 4096u, 0);
        if (!D.groups) return NULL;
    }
    if (seconds == 0.0f) {
        int32_t end = re_atomic_add(&D.nframe, (int32_t)n);
        if ((uint32_t)end > D.cap_frame) {
            re_atomic_add(&D.nframe, -(int32_t)n); // (nothing written: undone)
            re_atomic_add(&D.want_frame, (int32_t)n), re_atomic_add(&D.dropped, (int32_t)n);
            return NULL;
        }
        return &D.frame[end - (int32_t)n];
    }
    int32_t g = re_atomic_add(&D.ngroups, 1) - 1;
    int32_t end = re_atomic_add(&D.nkept, (int32_t)n);
    if ((uint32_t)end > D.cap_kept || g >= (int32_t)DD_MAX_GROUPS) {
        re_atomic_add(&D.nkept, -(int32_t)n), re_atomic_add(&D.ngroups, -1);
        re_atomic_add(&D.want_kept, (int32_t)n), re_atomic_add(&D.dropped, (int32_t)n);
        return NULL;
    }
    D.groups[g] = (Group){ (uint32_t)(end - (int32_t)n), n, seconds < 0.0f ? -1.0 : D.now + seconds };
    if (seconds > 0.0f && (D.next_expiry <= 0.0 || D.now + seconds < D.next_expiry)) D.next_expiry = D.now + seconds; // (a race only delays a compaction)
    return &D.kept[end - (int32_t)n];
}

uint32_t dd_rgb565(uint32_t rgba) {
    uint32_t r = rgba & 255u, g = (rgba >> 8) & 255u, b = (rgba >> 16) & 255u;
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
}
DdTrace* dd_reserve_traces(uint32_t n) {
    if (D.off || !n) return NULL;
    int32_t end = re_atomic_add(&D.ntr, (int32_t)n);
    if ((uint32_t)end > D.cap_tr) {
        re_atomic_add(&D.ntr, -(int32_t)n);
        re_atomic_add(&D.want_tr, (int32_t)n), re_atomic_add(&D.dropped, (int32_t)n);
        return NULL;
    }
    return &D.tr[end - (int32_t)n];
}
static const DdTrace* g_blk[64];
static uint32_t g_blkn[64], g_nblk;
void dd_add_trace_block(const DdTrace* p, uint32_t n) {
    if (!D.off && p && n && g_nblk < 64) g_blk[g_nblk] = p, g_blkn[g_nblk++] = n;
}
uint32_t dd_trace_blocks(const DdTrace* const** p, const uint32_t** n) {
    *p = (const DdTrace* const*)g_blk, *n = g_blkn;
    return D.off ? 0u : g_nblk;
}
const DdTrace* dd_frame_traces(uint32_t* n) {
    *n = D.off ? 0u : (uint32_t)re_atomic_load(&D.ntr);
    return D.tr;
}

void dd_line(const float a[3], const float b[3], uint32_t rgba, float thickness, float seconds) {
    DdPrim* p = dd_reserve(1, seconds);
    if (!p) return;
    memcpy(p->a, a, 12), memcpy(p->b, b, 12);
    p->rgba = rgba, p->size = thickness > 0.5f ? thickness : 1.0f;
}
void dd_point(const float pt[3], uint32_t rgba, float size, float seconds) {
    DdPrim* p = dd_reserve(1, seconds);
    if (!p) return;
    memcpy(p->a, pt, 12), memcpy(p->b, pt, 12);
    p->rgba = rgba, p->size = -(size > 1.0f ? size : 1.0f);
}
void dd_box(const float mn[3], const float mx[3], uint32_t rgba, float thickness, float seconds) {
    DdPrim* p = dd_reserve(12, seconds);
    if (!p) return;
    static const uint8_t e[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (int i = 0; i < 12; i++) {
        for (int s = 0; s < 2; s++) {
            int c = e[i][s];
            float* o = s ? p[i].b : p[i].a;
            o[0] = c & 1 ? mx[0] : mn[0], o[1] = c & 2 ? mx[1] : mn[1], o[2] = c & 4 ? mx[2] : mn[2];
        }
        p[i].rgba = rgba, p[i].size = thickness > 0.5f ? thickness : 1.0f;
    }
}
// a circle of 16 segments in the plane of the axes u, v around c
static void circle(DdPrim* p, const float c[3], const float u[3], const float v[3], float r, uint32_t rgba, float th) {
    for (int i = 0; i < 16; i++) {
        float a0 = (float)i / 16.0f * 6.2831853f, a1 = (float)(i + 1) / 16.0f * 6.2831853f;
        for (int k = 0; k < 3; k++) {
            p[i].a[k] = c[k] + (u[k] * cosf(a0) + v[k] * sinf(a0)) * r;
            p[i].b[k] = c[k] + (u[k] * cosf(a1) + v[k] * sinf(a1)) * r;
        }
        p[i].rgba = rgba, p[i].size = th;
    }
}
void dd_sphere(const float c[3], float r, uint32_t rgba, float thickness, float seconds) {
    DdPrim* p = dd_reserve(48, seconds);
    if (!p) return;
    float th = thickness > 0.5f ? thickness : 1.0f, X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    circle(p, c, X, Y, r, rgba, th), circle(p + 16, c, X, Z, r, rgba, th), circle(p + 32, c, Y, Z, r, rgba, th);
}
void dd_capsule(const float c[3], float half_height, float r, uint32_t rgba, float thickness, float seconds) {
    DdPrim* p = dd_reserve(16 * 6 + 4, seconds);
    if (!p) return;
    float th = thickness > 0.5f ? thickness : 1.0f, hh = half_height > r ? half_height - r : 0.0f;
    float top[3] = { c[0], c[1], c[2] + hh }, bot[3] = { c[0], c[1], c[2] - hh }, X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    circle(p, top, X, Y, r, rgba, th), circle(p + 16, bot, X, Y, r, rgba, th);
    circle(p + 32, top, X, Z, r, rgba, th), circle(p + 48, bot, X, Z, r, rgba, th);
    circle(p + 64, top, Y, Z, r, rgba, th), circle(p + 80, bot, Y, Z, r, rgba, th);
    const float off[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int i = 0; i < 4; i++) {
        DdPrim* q = &p[96 + i];
        q->a[0] = q->b[0] = c[0] + off[i][0] * r, q->a[1] = q->b[1] = c[1] + off[i][1] * r;
        q->a[2] = top[2], q->b[2] = bot[2], q->rgba = rgba, q->size = th;
    }
}

void dd_flush_persistent(void) {
    // (the main thread, between frames' reservations: the groups that never end dropped; the timed ones kept)
    int32_t ng = re_atomic_load(&D.ngroups);
    for (int32_t g = 0; g < ng; g++)
        if (D.groups[g].until < 0.0) D.groups[g].until = 0.0; // (expired at the next start)
    D.next_expiry = D.now > 0.0 ? D.now : 1e-9;
}

void dd_clear_all(void) {
    re_atomic_store(&D.ntr, 0), g_nblk = 0;
    re_atomic_store(&D.nframe, 0), re_atomic_store(&D.nkept, 0), re_atomic_store(&D.ngroups, 0);
    D.next_expiry = 0.0;
}

static int by_start(const void* a, const void* b) {
    uint32_t x = ((const Group*)a)->start, y = ((const Group*)b)->start;
    return x < y ? -1 : x > y;
}

static bool grow_list(DdPrim** p, uint32_t* cap, uint32_t need, uint32_t keep) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap : 65536u;
    while (c < need && c < DD_MAX_PRIMS) c *= 2;
    if (c > DD_MAX_PRIMS) c = DD_MAX_PRIMS;
    if (c <= *cap) return false;
    DdPrim* q = malloc(sizeof(DdPrim) * c); // (the room ran out last frame: grown once, between frames)
    if (!q) return false;
    if (keep && *p) memcpy(q, *p, sizeof(DdPrim) * keep);
    free(*p);
    *p = q, *cap = c;
    return true;
}

void dd_frame_begin(double now) {
    D.now = now;
    g_nblk = 0; // (the async batches' blocks: handed again by sq_async_join after this)
    D.dropped_last = (uint32_t)re_atomic_load(&D.dropped);
    re_atomic_store(&D.dropped, 0);
    int32_t wf = re_atomic_load(&D.want_frame), wk = re_atomic_load(&D.want_kept);
    re_atomic_store(&D.want_frame, 0), re_atomic_store(&D.want_kept, 0);
    // this frame's list: emptied, grown to what was asked (with a margin)
    uint32_t used = (uint32_t)re_atomic_load(&D.nframe);
    if (wf > 0) grow_list(&D.frame, &D.cap_frame, (used + (uint32_t)wf) + (used + (uint32_t)wf) / 4u, 0);
    else if (!D.frame) grow_list(&D.frame, &D.cap_frame, 4096u, 0);
    re_atomic_store(&D.nframe, 0);
    { // the trace records the same way (grown in place: never kept past the frame)
        int32_t wt = re_atomic_load(&D.want_tr);
        uint32_t ut = (uint32_t)re_atomic_load(&D.ntr), need = ut + (uint32_t)(wt > 0 ? wt : 0);
        re_atomic_store(&D.want_tr, 0), re_atomic_store(&D.ntr, 0);
        if (wt > 0 && need + need / 4u > D.cap_tr && need <= DD_MAX_PRIMS) {
            DdTrace* t = malloc(sizeof(DdTrace) * (need + need / 4u)); // (the room ran out last frame: once, between frames)
            if (t) free(D.tr), D.tr = t, D.cap_tr = need + need / 4u;
        }
    }
    if (!D.groups) D.groups = malloc(sizeof(Group) * DD_MAX_GROUPS); // (once)
    // the kept ones: the expired out (compacted in place, by their start), the room grown
    int32_t ng = re_atomic_load(&D.ngroups);
    if (ng > 0 && D.next_expiry > 0.0 && now >= D.next_expiry) {
        qsort(D.groups, (size_t)ng, sizeof(Group), by_start);
        uint32_t w = 0;
        int32_t keepg = 0;
        double next = 0.0;
        for (int32_t g = 0; g < ng; g++) {
            Group gr = D.groups[g];
            if (gr.until >= 0.0 && gr.until <= now) continue;
            if (gr.start != w) memmove(&D.kept[w], &D.kept[gr.start], sizeof(DdPrim) * gr.count);
            gr.start = w, w += gr.count;
            if (gr.until > 0.0 && (next <= 0.0 || gr.until < next)) next = gr.until;
            D.groups[keepg++] = gr;
        }
        re_atomic_store(&D.ngroups, keepg), re_atomic_store(&D.nkept, (int32_t)w);
        D.next_expiry = next;
    }
    if (wk > 0) {
        uint32_t nk = (uint32_t)re_atomic_load(&D.nkept);
        grow_list(&D.kept, &D.cap_kept, (nk + (uint32_t)wk) + (nk + (uint32_t)wk) / 4u, nk);
    } else if (!D.kept)
        grow_list(&D.kept, &D.cap_kept, 4096u, 0);
}

const DdPrim* dd_frame_prims(uint32_t* n) {
    *n = D.off ? 0u : (uint32_t)re_atomic_load(&D.nframe);
    return D.frame;
}
const DdPrim* dd_kept_prims(uint32_t* n) {
    *n = D.off ? 0u : (uint32_t)re_atomic_load(&D.nkept);
    return D.kept;
}

// --- (2026-11-19) the traces' lines, many at once ---
#include "scene_query.h"

typedef struct TrArg {
    const SqQuery* qs;
    const SqHit* hits;
    uint32_t n, chunk;
    uint32_t* off; // each chunk's first primitive (counted, then summed)
    DdPrim* dst;
    uint32_t trace_rgba, hit_rgba;
} TrArg;
static void tr_count(void* arg, int c) {
    TrArg* a = arg;
    uint32_t s = (uint32_t)c * a->chunk, e = s + a->chunk < a->n ? s + a->chunk : a->n, k = 0;
    for (uint32_t i = s; i < e; i++) k += a->hits[i].blocking ? 3u : 1u;
    a->off[c] = k;
}
static void tr_fill(void* arg, int c) {
    TrArg* a = arg;
    uint32_t s = (uint32_t)c * a->chunk, e = s + a->chunk < a->n ? s + a->chunk : a->n;
    DdPrim* p = a->dst + a->off[c];
    for (uint32_t i = s; i < e; i++) {
        const SqQuery* q = &a->qs[i];
        const SqHit* h = &a->hits[i];
        if (!h->blocking) {
            memcpy(p->a, q->start, 12), memcpy(p->b, q->end, 12), p->rgba = a->trace_rgba, p->size = 1.0f, p++;
            continue;
        }
        memcpy(p->a, q->start, 12), memcpy(p->b, h->location, 12), p->rgba = a->trace_rgba, p->size = 1.0f, p++;
        memcpy(p->a, h->location, 12), memcpy(p->b, q->end, 12), p->rgba = a->hit_rgba, p->size = 1.0f, p++;
        memcpy(p->a, h->impact_point, 12), memcpy(p->b, h->impact_point, 12), p->rgba = a->trace_rgba, p->size = -8.0f, p++;
    }
}
void dd_traces(const SqQuery* qs, const SqHit* hits, uint32_t n, uint32_t trace_rgba, uint32_t hit_rgba, float seconds) {
    if (D.off || !n) return;
    enum { MAXC = 4096 };
    static uint32_t off[MAXC];
    TrArg a = { qs, hits, n, 1024, off, NULL, trace_rgba, hit_rgba };
    while ((n + a.chunk - 1) / a.chunk > MAXC) a.chunk *= 2;
    int nc = (int)((n + a.chunk - 1) / a.chunk);
    if (n > 4096) re_parallel_for(nc, tr_count, &a);
    else
        for (int c = 0; c < nc; c++) tr_count(&a, c);
    uint32_t total = 0;
    for (int c = 0; c < nc; c++) {
        uint32_t k = off[c];
        off[c] = total, total += k;
    }
    a.dst = dd_reserve(total, seconds);
    if (!a.dst) return;
    if (n > 4096) re_parallel_for(nc, tr_fill, &a);
    else
        for (int c = 0; c < nc; c++) tr_fill(&a, c);
}
