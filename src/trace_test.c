// (2026-11-19) --trace-test (notas-dev/traces.md): a Blueprint with the trace nodes over a floor and a cube - Line
// Trace By Channel down onto the cube (450 cm), Sphere Trace (its center 50 cm above the cube: 400 cm), Multi Line Trace
// through the cube to the floor, Line Trace Batch of two lines (both hit) - every one drawn (Draw Debug Type). And
// --trace-bench N: N traces a frame (lines, spheres, boxes and capsules in turn) around the camera while playing,
// drawn For One Frame, their cost measured.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "render.h"
#include "debug_draw.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "scene_query.h"

static bool g_fail;
static Entity g_probe;

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] trace-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = pin(g, a, 1, ap), pb = pin(g, b, 0, bpn);
    if (!bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb)) printf("[RealEngine] trace-test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn), g_fail = true;
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x, const char* text, const char* text2) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 320.0f, floorf(x / 6.0f) * 220.0f, text, text2);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}
static void lit(BpGraph* g, uint32_t id, const char* name, float x, float y, float z) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) n->pins[p].lit = (BpValue){ { x, y, z, 0 } };
}
static void var(BpAsset* a, const char* name, uint8_t type) {
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "%s", name), v->type = type;
}

bool trace_test_setup(Editor* ed) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/TraceTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* a = bp_create(BPA_CLASS, "/Content/TraceTest", "BP_TraceProbe", "Actor");
    if (!a) return printf("[RealEngine] trace-test: could not create the class\n"), false;
    var(a, "LineHit", BPT_BOOL), var(a, "LineDist", BPT_FLOAT), var(a, "SphereDist", BPT_FLOAT), var(a, "MultiHit", BPT_BOOL);
    var(a, "BatchHits", BPT_INT), var(a, "BoxDist", BPT_FLOAT), var(a, "CapsuleDist", BPT_FLOAT);
    var(a, "SimpleDist", BPT_FLOAT), var(a, "ComplexDist", BPT_FLOAT), var(a, "CxSphereDist", BPT_FLOAT), var(a, "AsyncHandle", BPT_INT), var(a, "AsyncHits", BPT_INT);
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    // a line down onto the cube (its top at 150): drawn Persistent
    uint32_t lt = node(a, g, BPK_LINE_TRACE, 1, NULL, NULL);
    lit(g, lt, "Start", 0, 0, 600), lit(g, lt, "End", 0, 0, -200), lit(g, lt, "Draw Debug Type", 3, 0, 0);
    uint32_t s1 = node(a, g, BPK_VAR_SET, 2, "LineHit", NULL), s2 = node(a, g, BPK_VAR_SET, 3, "LineDist", NULL);
    link(g, begin, "", lt, ""), link(g, lt, "", s1, ""), link(g, lt, "Return Value", s1, "LineHit");
    link(g, s1, "", s2, ""), link(g, lt, "Distance", s2, "LineDist");
    // a sphere of 50: its center stops at 200
    uint32_t st = node(a, g, BPK_SPHERE_TRACE, 4, NULL, NULL);
    lit(g, st, "Start", 0, 0, 600), lit(g, st, "End", 0, 0, -200), lit(g, st, "Radius", 50, 0, 0), lit(g, st, "Draw Debug Type", 2, 0, 0);
    uint32_t s3 = node(a, g, BPK_VAR_SET, 5, "SphereDist", NULL);
    link(g, s2, "", st, ""), link(g, st, "", s3, ""), link(g, st, "Distance", s3, "SphereDist");
    // a box of half size 30: stops at 180; a capsule (radius 30, half height 80): stops at 230
    uint32_t bt = node(a, g, BPK_BOX_TRACE, 6, NULL, NULL);
    lit(g, bt, "Start", 0, 0, 600), lit(g, bt, "End", 0, 0, -200), lit(g, bt, "Half Size", 30, 30, 30), lit(g, bt, "Draw Debug Type", 3, 0, 0);
    uint32_t s4 = node(a, g, BPK_VAR_SET, 7, "BoxDist", NULL);
    link(g, s3, "", bt, ""), link(g, bt, "", s4, ""), link(g, bt, "Distance", s4, "BoxDist");
    uint32_t ct = node(a, g, BPK_CAPSULE_TRACE, 8, NULL, NULL);
    lit(g, ct, "Start", 0, 0, 600), lit(g, ct, "End", 0, 0, -200), lit(g, ct, "Radius", 30, 0, 0), lit(g, ct, "Half Height", 80, 0, 0);
    lit(g, ct, "Draw Debug Type", 3, 0, 0);
    uint32_t s5 = node(a, g, BPK_VAR_SET, 9, "CapsuleDist", NULL);
    link(g, s4, "", ct, ""), link(g, ct, "", s5, ""), link(g, ct, "Distance", s5, "CapsuleDist");
    // through the cube to the floor beside it: two actors
    uint32_t mt = node(a, g, BPK_MULTI_LINE_TRACE, 10, NULL, NULL);
    lit(g, mt, "Start", 0, 0, 600), lit(g, mt, "End", 0, 0, -200), lit(g, mt, "Draw Debug Type", 3, 0, 0);
    uint32_t s6 = node(a, g, BPK_VAR_SET, 11, "MultiHit", NULL);
    link(g, s5, "", mt, ""), link(g, mt, "", s6, ""), link(g, mt, "Return Value", s6, "MultiHit");
    // a batch of two lines: onto the cube and onto the floor 30 m away
    uint32_t v0 = node(a, g, BPK_MAKE_VECTOR, 12, NULL, NULL), v1 = node(a, g, BPK_MAKE_VECTOR, 13, NULL, NULL);
    lit(g, v0, "Z", 600, 0, 0), lit(g, v1, "Z", -200, 0, 0);
    uint32_t ms = node(a, g, BPK_ARR_MAKE, 14, NULL, NULL), me = node(a, g, BPK_ARR_MAKE, 15, NULL, NULL);
    link(g, v0, "", ms, "[0]"), link(g, v1, "", me, "[0]");
    lit(g, ms, "[1]", 3000, 3000, 600), lit(g, me, "[1]", 3000, 3000, -200);
    uint32_t bb = node(a, g, BPK_LINE_TRACE_BATCH, 16, NULL, NULL);
    lit(g, bb, "Draw Debug Type", 3, 0, 0);
    link(g, ms, "Array", bb, "Starts"), link(g, me, "Array", bb, "Ends");
    uint32_t s7 = node(a, g, BPK_VAR_SET, 17, "BatchHits", NULL);
    link(g, s6, "", bb, ""), link(g, bb, "", s7, ""), link(g, bb, "Hit Count", s7, "BatchHits");
    // Trace Complex on a sphere of 50 at (0, -600, 100): 30 cm off its axis the box's top is at 150 (450 cm), the
    // sphere's surface at 140 (460); a sphere of 20 down its axis stops at 170 (430)
    uint32_t sl = node(a, g, BPK_LINE_TRACE, 18, NULL, NULL), cl = node(a, g, BPK_LINE_TRACE, 20, NULL, NULL), cs = node(a, g, BPK_SPHERE_TRACE, 22, NULL, NULL);
    lit(g, sl, "Start", 30, -600, 600), lit(g, sl, "End", 30, -600, -200);
    lit(g, cl, "Start", 30, -600, 600), lit(g, cl, "End", 30, -600, -200), lit(g, cl, "Trace Complex", 1, 0, 0), lit(g, cl, "Draw Debug Type", 3, 0, 0);
    lit(g, cs, "Start", 0, -600, 600), lit(g, cs, "End", 0, -600, -200), lit(g, cs, "Radius", 20, 0, 0), lit(g, cs, "Trace Complex", 1, 0, 0);
    lit(g, cs, "Draw Debug Type", 3, 0, 0);
    uint32_t s8 = node(a, g, BPK_VAR_SET, 19, "SimpleDist", NULL), s9 = node(a, g, BPK_VAR_SET, 21, "ComplexDist", NULL), s10 = node(a, g, BPK_VAR_SET, 23, "CxSphereDist", NULL);
    link(g, s7, "", sl, ""), link(g, sl, "", s8, ""), link(g, sl, "Distance", s8, "SimpleDist");
    link(g, s8, "", cl, ""), link(g, cl, "", s9, ""), link(g, cl, "Distance", s9, "ComplexDist");
    link(g, s9, "", cs, ""), link(g, cs, "", s10, ""), link(g, cs, "Distance", s10, "CxSphereDist");
    // an async batch of the same two lines: its handle kept, its results read on Tick once Ready
    uint32_t ab = node(a, g, BPK_ASYNC_LINE_TRACE_BATCH, 24, NULL, NULL);
    lit(g, ab, "Draw Debug Type", 1, 0, 0);
    link(g, ms, "Array", ab, "Starts"), link(g, me, "Array", ab, "Ends");
    uint32_t s11 = node(a, g, BPK_VAR_SET, 25, "AsyncHandle", NULL);
    link(g, s10, "", ab, ""), link(g, ab, "", s11, ""), link(g, ab, "Handle", s11, "AsyncHandle");
    uint32_t tick = node(a, g, BPK_EVENT_TICK, 26, NULL, NULL), gh = node(a, g, BPK_VAR_GET, 27, "AsyncHandle", NULL);
    uint32_t gr = node(a, g, BPK_GET_ASYNC_TRACE, 28, NULL, NULL), br = node(a, g, BPK_BRANCH, 29, NULL, NULL);
    uint32_t s12 = node(a, g, BPK_VAR_SET, 30, "AsyncHits", NULL);
    link(g, tick, "", gr, ""), link(g, gh, "AsyncHandle", gr, "Handle"), link(g, gr, "", br, ""), link(g, gr, "Ready", br, "Condition");
    link(g, br, "True", s12, ""), link(g, gr, "Hit Count", s12, "AsyncHits");
    if (g_fail || !bp_save(a) || !bp_compile(a)) return printf("[RealEngine] trace-test: the class did not compile\n"), false;
    // the level: a floor (100 m) and a cube of 1 m at z 100 (its top at 150)
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, 0.0);
    CTransform* ft = ecs_get(&ed->world, floor, COMP_TRANSFORM);
    if (ft) ft->scale[0] = ft->scale[1] = 100.0f, ft->location[2] = -1.0f;
    editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ 0, 0, 100 }, 0.0);
    editor_spawn_shape(ed, SHAPE_SPHERE, (float[3]){ 0, -600, 100 }, 0.0);
    g_probe = bp_spawn(&ed->world, a, (float[3]){ 600, 600, 50 }, (float[3]){ 0, 0, 0 });
    ed->cam.pos[0] = -900.0f, ed->cam.pos[1] = -900.0f, ed->cam.pos[2] = 700.0f;
    return g_probe != ENTITY_NONE;
}

bool trace_test_check(Editor* ed) {
    World* w = &ed->world;
    BpValue lh = { { 0 } }, ld = { { 0 } }, sd = { { 0 } }, mh = { { 0 } }, bh = { { 0 } }, bd = { { 0 } }, cd = { { 0 } };
    bool got = bp_instance_var(w, g_probe, "LineHit", &lh);
    if (!got) printf("[RealEngine] trace-test: the probe's variables are not readable (actor %u alive %d)\n", g_probe, (int)ecs_alive(w, g_probe));
    bp_instance_var(w, g_probe, "LineDist", &ld), bp_instance_var(w, g_probe, "SphereDist", &sd), bp_instance_var(w, g_probe, "MultiHit", &mh);
    bp_instance_var(w, g_probe, "BatchHits", &bh), bp_instance_var(w, g_probe, "BoxDist", &bd), bp_instance_var(w, g_probe, "CapsuleDist", &cd);
    BpValue sd2 = { { 0 } }, cxd = { { 0 } }, cxs = { { 0 } }, ah = { { 0 } };
    bp_instance_var(w, g_probe, "SimpleDist", &sd2), bp_instance_var(w, g_probe, "ComplexDist", &cxd), bp_instance_var(w, g_probe, "CxSphereDist", &cxs);
    bp_instance_var(w, g_probe, "AsyncHits", &ah);
    bool cx_ok = fabsf(sd2.v[0] - 450.0f) < 1.0f && fabsf(cxd.v[0] - 460.0f) < 2.0f && fabsf(cxs.v[0] - 430.0f) < 2.0f && ah.v[0] == 2.0f;
    printf("[RealEngine] Trace test complex/async: simple %.1f, complex %.1f (460), complex sphere %.1f (430), async %.0f hits: %s\n", sd2.v[0], cxd.v[0], cxs.v[0],
           ah.v[0], cx_ok ? "ok" : "WRONG");
    bool ok = cx_ok && got && !g_fail && lh.v[0] != 0.0f && fabsf(ld.v[0] - 450.0f) < 1.0f && fabsf(sd.v[0] - 400.0f) < 1.0f && fabsf(bd.v[0] - 420.0f) < 1.0f &&
              fabsf(cd.v[0] - 370.0f) < 1.0f && mh.v[0] != 0.0f && bh.v[0] == 2.0f;
    printf("[RealEngine] Trace test: line hit %.0f at %.1f cm, sphere %.1f, box %.1f, capsule %.1f, multi %.0f, batch %.0f hits; %u debug primitives drawn: %s\n",
           lh.v[0], ld.v[0], sd.v[0], bd.v[0], cd.v[0], mh.v[0], bh.v[0], render_debug_drawn(), ok ? "ok" : "WRONG");
    if (!ok) printf("[RealEngine] FAIL: trace-test\n");
    return ok;
}

// --- --trace-bench N ---
static struct {
    SqQuery* q;
    SqHit* h;
    uint32_t n, frames;
    double ms, worst, draw_ms;
    float cam[3];
    uint32_t seed;
} TB;

static float rndf(uint64_t* r, float a, float b) {
    *r ^= *r << 13, *r ^= *r >> 7, *r ^= *r << 17;
    return a + (b - a) * (float)((*r >> 40) & 0xFFFFFF) / 16777216.0f;
}
static void bench_make(void* arg, int chunk) {
    uint32_t n = *(const uint32_t*)arg, s = (uint32_t)chunk * 4096u, e = s + 4096u < n ? s + 4096u : n;
    uint64_t r = 0x9E3779B97F4A7C15ull ^ ((uint64_t)(chunk + 1) * 0xD1B54A32D192ED03ull) ^ TB.seed;
    for (uint32_t i = s; i < e; i++) {
        SqQuery* q = &TB.q[i];
        memset(q, 0, sizeof *q);
        q->shape = (uint8_t)(i & 3u);
        float p[3] = { TB.cam[0] + rndf(&r, -1500, 1500), TB.cam[1] + rndf(&r, -1500, 1500), TB.cam[2] + rndf(&r, -300, 300) };
        float d[3] = { rndf(&r, -1, 1), rndf(&r, -1, 1), rndf(&r, -1, 0.3f) }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f, len = rndf(&r, 500, 2500);
        for (int k = 0; k < 3; k++) q->start[k] = p[k], q->end[k] = p[k] + d[k] / l * len;
        q->radius = 15.0f, q->half[0] = q->half[1] = q->half[2] = 15.0f;
        if (q->shape == SQ_CAPSULE) q->half[2] = 60.0f;
    }
}

void trace_bench_frame(Editor* ed, uint32_t n) {
    if (!ed->playing || !n) return;
    if (!TB.q) {
        TB.q = malloc(sizeof(SqQuery) * n), TB.h = malloc(sizeof(SqHit) * n), TB.n = n; // (once)
        if (!TB.q || !TB.h) return;
    }
    // around the camera, out to 25 m; a quarter each shape (made on the workers, as a game's jobs would)
    TB.cam[0] = ed->cam.pos[0], TB.cam[1] = ed->cam.pos[1], TB.cam[2] = ed->cam.pos[2], TB.seed++;
    const char* as = getenv("REALENGINE_TRACE_BENCH_ASYNC"); // (=1: async batches, their results next frame)
    if (as && as[0] == '1') { // (made straight into the batch's room; the main thread only kicks it)
        uint32_t handle = 0;
        SqQuery* own = TB.q;
        SqQuery* dst = sq_async_begin(&ed->world, n, &handle);
        if (!dst) return;
        TB.q = dst;
        re_parallel_for((int)((n + 4095u) / 4096u), bench_make, &n);
        TB.q = own;
        double t0 = re_time_seconds();
        sq_async_kick(handle, dd_rgb565(dd_rgba(1, 0, 0, 1)) << 16 | dd_rgb565(dd_rgba(0, 1, 0, 1)));
        double msa = (re_time_seconds() - t0) * 1000.0;
        TB.ms += msa, TB.frames++;
        if (msa > TB.worst && TB.frames > 10) TB.worst = msa;
        return;
    }
    re_parallel_for((int)((n + 4095u) / 4096u), bench_make, &n);
    double t0 = re_time_seconds();
    const char* nd = getenv("REALENGINE_TRACE_BENCH_DRAW"); // (=0: the traces alone, not drawn)
    if (nd && nd[0] == '0') sq_trace_batch(&ed->world, TB.q, n, TB.h);
    else sq_trace_batch_drawn(&ed->world, TB.q, n, TB.h, dd_rgb565(dd_rgba(1, 0, 0, 1)) << 16 | dd_rgb565(dd_rgba(0, 1, 0, 1)));
    double t1 = re_time_seconds(), t2 = t1; // (drawn by the workers as they trace: nothing laid out after)
    double ms = (t1 - t0) * 1000.0;
    TB.ms += ms, TB.draw_ms += (t2 - t1) * 1000.0, TB.frames++;
    if (ms > TB.worst && TB.frames > 10) TB.worst = ms;
}

void trace_bench_report(void) {
    if (!TB.frames) return;
    SqStats st = sq_stats();
    printf("[RealEngine] Trace bench: %u traces a frame (lines, spheres, boxes, capsules) over %u primitives: %.2f ms a frame on the workers (worst %.2f), "
           "%.2f ms to lay out their debug lines, %u drawn last frame (%u dropped)\n",
           TB.n, st.prims, TB.ms / TB.frames, TB.worst, TB.draw_ms / TB.frames, render_debug_drawn(), dd_dropped_last());
}
