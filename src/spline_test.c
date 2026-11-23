// The splines' and cables' tests without a window (spline.h, notas-dev/splines.md):
// - spline_eval: lengths against a dense polyline, distance <-> input key, the closest key, the point types (Linear,
//   Constant, Custom tangents), the frame and its roll, a curve through rows (rotated, scaled), 64 points;
// - spline_rail_river: the rivers' samples and the rails' curve the same as the Catmull-Rom they had before (to 1e-3 cm),
//   and a level saved before the Spline (a Rail row at its old size) loads as a Rail with its points;
// - cable_hang: the XPBD cable between two pins settles to the catenary's sag (5%), inextensible (1%), still; a free end
//   hangs down its length; at dt 1/30 with 64 segments it stays finite; laid over a box it rests on top; the cost per cable
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "platform.h"
#include "river.h"
#include "spline.h"

#define CHECK(c, ...)                                                                                                       \
    do {                                                                                                                   \
        if (!(c)) {                                                                                                        \
            printf("[RealEngine] FAIL: " __VA_ARGS__);                                                                     \
            printf("\n");                                                                                                  \
            ok = false;                                                                                                    \
        }                                                                                                                  \
    } while (0)

static float dist3(const float a[3], const float b[3]) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
static float poly_length(const SplineCurve* c, uint32_t steps) {
    float L = 0.0f, prev[3], p[3];
    float km = spline_key_max(c);
    spline_eval_key(c, 0.0f, prev, NULL);
    for (uint32_t i = 1; i <= steps; i++) {
        spline_eval_key(c, km * (float)i / (float)steps, p, NULL);
        L += dist3(p, prev);
        memcpy(prev, p, 12);
    }
    return L;
}

static bool test_eval(void) {
    bool ok = true;
    static SplineCurve c;
    // a straight line: 10 m, the distance read back exactly (the auto tangents make it non-uniform in the key)
    const float line[2][3] = { { 0, 0, 0 }, { 1000, 0, 0 } };
    spline_curve_points(&c, line, 2, false, NULL);
    CHECK(fabsf(c.length - 1000.0f) < 0.05f, "spline_eval: straight length %.3f", c.length);
    float p[3];
    spline_location_at_distance(&c, 250.0f, p);
    CHECK(fabsf(p[0] - 250.0f) < 0.5f && fabsf(p[1]) < 1e-3f, "spline_eval: 250 cm along a line at %.2f", p[0]);
    // a closed loop of 6 points and a hilly open curve: the arc length against a dense polyline (0.05%)
    float ring[6][3];
    for (int i = 0; i < 6; i++) ring[i][0] = 1000.0f * cosf(1.0471976f * (float)i), ring[i][1] = 1000.0f * sinf(1.0471976f * (float)i), ring[i][2] = 0.0f;
    spline_curve_points(&c, (const float(*)[3])ring, 6, true, NULL);
    float pl = poly_length(&c, 20000);
    CHECK(fabsf(c.length - pl) / pl < 5e-4f, "spline_eval: loop length %.2f vs %.2f", c.length, pl);
    CHECK(fabsf(c.length - 6283.2f) / 6283.2f < 0.03f, "spline_eval: a ring of radius 10 m is %.0f cm around", c.length);
    // round trips: distance -> key -> distance
    float worst = 0.0f;
    for (int i = 0; i <= 100; i++) {
        float d = c.length * (float)i / 100.0f, k = spline_key_at_distance(&c, d), d2 = spline_distance_at_key(&c, k);
        float e = fabsf(fmodf(d2 - d + c.length * 1.5f, c.length) - c.length * 0.5f);
        if (e > worst) worst = e;
    }
    CHECK(worst < 0.5f, "spline_eval: distance -> key -> distance off by %.3f cm", worst);
    // the closest key: a point 50 cm off the curve at key 1.37
    float q[3], d[3];
    spline_eval_key(&c, 1.37f, q, d);
    float n[3] = { -d[1], d[0], 0.0f }, nl = sqrtf(n[0] * n[0] + n[1] * n[1]);
    for (int k = 0; k < 3; k++) q[k] += n[k] / nl * 50.0f;
    float ck = spline_closest_key(&c, q);
    CHECK(fabsf(ck - 1.37f) < 0.01f, "spline_eval: the closest key %.4f (1.37)", ck);
    // the point types
    const float zig[4][3] = { { 0, 0, 0 }, { 500, 300, 0 }, { 1000, 0, 0 }, { 1500, 300, 100 } };
    CRail r;
    spline_component_defaults(&r, SPLINE_KIND_SPLINE);
    r.npoints = 4, memcpy(r.points, zig, sizeof zig);
    r.ptype[1] = SPLINE_POINT_LINEAR, r.ptype[2] = SPLINE_POINT_CONSTANT;
    spline_curve_from(&c, &r, NULL);
    spline_eval_key(&c, 1.5f, p, NULL);
    CHECK(dist3(p, (float[3]){ 750, 150, 0 }) < 1e-3f, "spline_eval: Linear segment's middle (%.2f %.2f %.2f)", p[0], p[1], p[2]);
    spline_eval_key(&c, 2.7f, p, NULL);
    CHECK(dist3(p, zig[2]) < 1e-3f, "spline_eval: a Constant segment stays at its point");
    spline_set_point_type(&r, 0, SPLINE_POINT_CUSTOM);
    r.leave[0][0] = 0.0f, r.leave[0][1] = 3000.0f, r.leave[0][2] = 0.0f;
    spline_curve_from(&c, &r, NULL);
    float f[3], rt[3], up[3], sc[2];
    spline_frame_at_key(&c, 0.0f, NULL, f, rt, up, sc);
    CHECK(f[1] > 0.999f, "spline_eval: a custom leave tangent along +Y gives the direction (%.3f %.3f %.3f)", f[0], f[1], f[2]);
    // the frame: right-handed, orthonormal; a roll of 90 degrees turns up into -right
    const float flat[2][3] = { { 0, 0, 0 }, { 1000, 0, 0 } };
    spline_component_defaults(&r, SPLINE_KIND_SPLINE);
    memcpy(r.points, flat, sizeof flat);
    r.roll[0] = r.roll[1] = 90.0f, r.pscale[1][0] = 3.0f;
    spline_curve_from(&c, &r, NULL);
    spline_frame_at_key(&c, 0.5f, NULL, f, rt, up, sc);
    CHECK(fabsf(f[0] - 1.0f) < 1e-4f && fabsf(up[1] + 1.0f) < 1e-3f && fabsf(rt[2] - 1.0f) < 1e-3f, "spline_eval: the roll's frame (up %.2f %.2f %.2f, right %.2f %.2f %.2f)",
          up[0], up[1], up[2], rt[0], rt[1], rt[2]);
    CHECK(fabsf(sc[0] - 2.0f) < 1e-4f && fabsf(sc[1] - 1.0f) < 1e-4f, "spline_eval: the scale halfway %.3f %.3f", sc[0], sc[1]);
    // through rows: turned 90 degrees, scaled 2, moved: twice as long, the start where the rows put it
    float rows[3][4] = { { 0, -2, 0, 100 }, { 2, 0, 0, 200 }, { 0, 0, 2, 300 } };
    spline_component_defaults(&r, SPLINE_KIND_SPLINE);
    spline_curve_from(&c, &r, rows);
    spline_location_at_distance(&c, 0.0f, p);
    CHECK(fabsf(c.length - 1000.0f) < 0.1f && dist3(p, (float[3]){ 100, 200, 300 }) < 1e-3f, "spline_eval: through rows: length %.2f, start %.1f %.1f %.1f", c.length, p[0], p[1], p[2]);
    spline_direction_at_distance(&c, 500.0f, d);
    CHECK(fabsf(d[1] - 1.0f) < 1e-4f, "spline_eval: turned 90 degrees, the direction is +Y (%.3f %.3f %.3f)", d[0], d[1], d[2]);
    // 64 points: the most a spline holds; inserting past it refused; removing to two
    spline_component_defaults(&r, SPLINE_KIND_SPLINE);
    while (spline_insert_point(&r, (int)r.npoints - 1)) {}
    CHECK(r.npoints == SPLINE_MAX_POINTS, "spline_eval: %u points after inserting to the limit", r.npoints);
    spline_curve_from(&c, &r, NULL);
    CHECK(c.length > 1000.0f && isfinite(c.length), "spline_eval: 64 points' length %.1f", c.length);
    while (spline_remove_point(&r, 0)) {}
    CHECK(r.npoints == 2, "spline_eval: removing stops at two (%u)", r.npoints);
    // the cost: a 16-point curve built (tangents, arc length) and 10 000 queries by distance
    const float(*pts)[3] = (const float(*)[3])ring;
    double t0 = re_time_seconds();
    for (int i = 0; i < 1000; i++) spline_curve_points(&c, pts, 6, true, NULL);
    double build_us = (re_time_seconds() - t0) * 1000.0;
    t0 = re_time_seconds();
    float acc = 0.0f;
    for (int i = 0; i < 10000; i++) {
        spline_location_at_distance(&c, (float)i * 0.61f, p);
        acc += p[0];
    }
    double q_ns = (re_time_seconds() - t0) * 1e9 / 10000.0;
    printf("[RealEngine] spline_eval: build %.2f us (6 points, closed), a query by distance %.0f ns (%g)\n", build_us, q_ns, acc > 0.0f ? 1.0 : 0.0);
    if (ok) printf("[RealEngine] spline_eval: ok\n");
    return ok;
}

// The Catmull-Rom the rivers and rails used before Q14 (river.c, vehicle_sim.inc), kept here as the reference
static void old_cr(const float* p0, const float* p1, const float* p2, const float* p3, float t, float o[3]) {
    float t2 = t * t, t3 = t2 * t;
    for (int k = 0; k < 3; k++)
        o[k] = 0.5f * (2.0f * p1[k] + (-p0[k] + p2[k]) * t + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * t2 + (-p0[k] + 3.0f * p1[k] - 3.0f * p2[k] + p3[k]) * t3);
}

static bool test_rail_river(void) {
    bool ok = true;
    // a river: 7 points through a transform; the old samples (8 a segment) against river_samples
    CRiver rv;
    memset(&rv, 0, sizeof rv);
    rv.npoints = 7;
    srand(7);
    for (uint32_t i = 0; i < rv.npoints; i++) {
        rv.points[i][0] = (float)i * 900.0f + (float)(rand() % 300), rv.points[i][1] = (float)(rand() % 1200) - 600.0f, rv.points[i][2] = (float)(rand() % 200);
        rv.width[i] = 800.0f, rv.depth[i] = 200.0f, rv.speed[i] = 150.0f;
    }
    CTransform t = { { 1000, -500, 30 }, { 0, 37, 0 }, { 1.5f, 1.5f, 1 }, 0 };
    RiverSample s[RIVER_SAMPLES];
    uint32_t n = river_samples(&rv, &t, s, RIVER_SAMPLES);
    float rows[3][4], wp[16][3];
    {
        float ax[3][3];
        float sp = sinf(t.rotation[0] * 0.017453293f), cp = cosf(t.rotation[0] * 0.017453293f), sy = sinf(t.rotation[1] * 0.017453293f), cy = cosf(t.rotation[1] * 0.017453293f);
        ax[0][0] = cp * cy, ax[0][1] = cp * sy, ax[0][2] = sp; // (roll 0: rotator_axes)
        ax[1][0] = -sy, ax[1][1] = cy, ax[1][2] = 0.0f;
        ax[2][0] = -sp * cy, ax[2][1] = -sp * sy, ax[2][2] = cp;
        for (int r = 0; r < 3; r++) {
            for (int a = 0; a < 3; a++) rows[r][a] = ax[a][r] * t.scale[a];
            rows[r][3] = t.location[r];
        }
    }
    for (uint32_t i = 0; i < rv.npoints; i++)
        for (int k = 0; k < 3; k++) wp[i][k] = rows[k][0] * rv.points[i][0] + rows[k][1] * rv.points[i][1] + rows[k][2] * rv.points[i][2] + rows[k][3];
    float worst = 0.0f;
    uint32_t m = 0;
    for (uint32_t sg = 0; sg + 1 < rv.npoints; sg++)
        for (int k = 0; k < 8; k++, m++) {
            float o[3];
            old_cr(wp[sg ? sg - 1 : 0], wp[sg], wp[sg + 1], wp[sg + 2 < rv.npoints ? sg + 2 : rv.npoints - 1], (float)k / 8.0f, o);
            if (m < n) worst = fmaxf(worst, dist3(o, s[m].p));
        }
    CHECK(n == (rv.npoints - 1) * 8 + 1, "spline_rail_river: %u river samples", n);
    CHECK(worst < 1e-3f, "spline_rail_river: the river moved %.5f cm from its old curve", worst);
    // a rail: closed, 6 points; the old wrapped Catmull-Rom against the Spline's evaluation
    CRail r;
    spline_component_defaults(&r, SPLINE_KIND_RAIL);
    const float rp[6][3] = { { -4000, -1500, 0 }, { 0, -2500, 0 }, { 4000, -1500, 0 }, { 4000, 1500, 0 }, { 0, 2500, 300 }, { -4000, 1500, 0 } };
    r.npoints = 6, r.closed = 1, memcpy(r.points, rp, sizeof rp);
    static SplineCurve c;
    spline_curve_from(&c, &r, NULL);
    float wr = 0.0f;
    for (uint32_t sg = 0; sg < 6; sg++)
        for (int k = 0; k < 16; k++) {
            float o[3], q[3];
            old_cr(rp[(sg + 5) % 6], rp[sg], rp[(sg + 1) % 6], rp[(sg + 2) % 6], (float)k / 16.0f, o);
            spline_eval_key(&c, (float)sg + (float)k / 16.0f, q, NULL);
            wr = fmaxf(wr, dist3(o, q));
        }
    CHECK(wr < 1e-3f, "spline_rail_river: the rail moved %.5f cm from its old curve", wr);
    // a level from before: the Rail's row at its old size (up to `built`) loads as a Rail with its points
    World w;
    ecs_init(&w);
    Entity e = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_RAIL));
    CRail* cr = ecs_get(&w, e, COMP_RAIL);
    spline_component_defaults(cr, SPLINE_KIND_RAIL);
    cr->npoints = 6, cr->closed = 1, memcpy(cr->points, rp, sizeof rp), cr->gauge = 100.0f;
    size_t sz = ecs_serialized_size(&w);
    uint8_t* blob = malloc(sz), *old = malloc(sz);
    bool loaded = false;
    if (blob && old && ecs_serialize(&w, blob) == sz) {
        // rewritten as an older engine wrote it: COMP_RAIL's size the old one, its column's rows cut to it
        const uint32_t osz = (uint32_t)offsetof(CRail, kind), nsz = (uint32_t)sizeof(CRail);
        size_t at = 0, wo = 0;
        uint32_t ncomp;
#define COPY(n_) (memcpy(old + wo, blob + at, (n_)), wo += (n_), at += (n_))
        COPY(8);
        memcpy(&ncomp, blob + at, 4);
        COPY(4);
        uint32_t sizes[COMP_COUNT];
        memcpy(sizes, blob + at, 4u * ncomp);
        uint32_t s2 = osz;
        for (uint32_t k = 0; k < ncomp; k++) {
            if (k == COMP_RAIL) memcpy(old + wo, &s2, 4), wo += 4, at += 4;
            else COPY(4);
        }
        uint32_t hdr[4];
        memcpy(hdr, blob + at, 16);
        COPY(16);
        COPY(sizeof(EcsSlot) * hdr[0] + 4u * hdr[1]);
        for (uint32_t a = 0; a < hdr[3]; a++) {
            uint64_t mask;
            uint32_t count;
            COPY(4);
            memcpy(&mask, blob + at, 8);
            COPY(8);
            memcpy(&count, blob + at, 4);
            COPY(4);
            COPY(sizeof(Entity) * count);
            for (uint32_t k = 0; k < ncomp; k++) {
                if (!(mask & COMP_BIT(k))) continue;
                if (k == COMP_RAIL) {
                    for (uint32_t q = 0; q < count; q++) memcpy(old + wo, blob + at + (size_t)nsz * q, osz), wo += osz;
                    at += (size_t)nsz * count;
                } else COPY((size_t)sizes[k] * count);
            }
        }
#undef COPY
        World w2;
        ecs_init(&w2);
        loaded = ecs_deserialize(&w2, old, wo);
        const CRail* lr = loaded ? ecs_get(&w2, e, COMP_RAIL) : NULL;
        CHECK(lr && lr->kind == SPLINE_KIND_RAIL && lr->npoints == 6 && lr->closed && lr->gauge == 100.0f && dist3(lr->points[4], rp[4]) < 1e-6f &&
                  lr->pscale[3][0] == 1.0f && lr->ptype[2] == SPLINE_POINT_CURVE,
              "spline_rail_river: a Rail saved before the Spline loads as a Rail (kind %u, %u points)", lr ? lr->kind : 99u, lr ? lr->npoints : 0u);
        ecs_shutdown(&w2);
    }
    CHECK(loaded, "spline_rail_river: the old level's blob was refused");
    free(blob), free(old);
    ecs_shutdown(&w);
    if (ok) printf("[RealEngine] spline_rail_river: ok (river %.5f cm, rail %.5f cm from the old curves)\n", worst, wr);
    return ok;
}

typedef struct BoxCtx { float c[3], h[3]; } BoxCtx;
static bool box_collide(void* user, float p[3], const float prev[3], float radius, float n[3]) {
    const BoxCtx* b = user;
    float d[3], best = 1e30f, sg = 1.0f, far = 0.0f;
    int ax = -1;
    for (int k = 0; k < 3; k++) {
        d[k] = p[k] - b->c[k];
        float o = b->h[k] + radius - fabsf(d[k]);
        if (o <= 0.0f) return false;
        if (o < best) best = o, ax = k, sg = d[k] >= 0.0f ? 1.0f : -1.0f;
    }
    for (int k = 0; k < 3; k++) { // (through the face it came in by)
        float lp = prev[k] - b->c[k], out = fabsf(lp) - (b->h[k] + radius);
        if (out > far) far = out, ax = k, sg = lp >= 0.0f ? 1.0f : -1.0f, best = b->h[k] + radius - sg * d[k];
    }
    n[0] = n[1] = n[2] = 0.0f;
    n[ax] = sg;
    p[ax] += n[ax] * best;
    return true;
}

static float cable_len(const CableSim* s) {
    float L = 0.0f;
    for (uint32_t i = 1; i < s->n; i++) L += dist3(s->x[i], s->x[i - 1]);
    return L;
}

static bool test_cable(void) {
    bool ok = true;
    static CableSim s;
    CableParams p = { 1.0f, 0.0f, 1.5f, 1.0f, 0.3f, 1.0f, 12, 6, { 0, 0, 0 } };
    // between two pins 10 m apart, 12 m long: the catenary's sag (L = 2a sinh(D / 2a), sag = a (cosh(D / 2a) - 1))
    const float a[3] = { 0, 0, 1000 }, b[3] = { 1000, 0, 1000 };
    cable_sim_init(&s, 32, a, b, 1200.0f);
    for (int f = 0; f < 600; f++) cable_sim_step(&s, &p, 1.0f / 60.0f, a, b, NULL, NULL);
    float lo = 1e-3f, hi = 1e7f; // (bisection on a: L(a) falls as a grows)
    for (int it = 0; it < 200; it++) {
        float m = 0.5f * (lo + hi), L = 2.0f * m * sinhf(500.0f / m);
        if (L > 1200.0f) lo = m;
        else hi = m;
    }
    float ca = 0.5f * (lo + hi), sag_th = ca * (coshf(500.0f / ca) - 1.0f), sag = 0.0f, vmax = 0.0f;
    bool finite = true;
    for (uint32_t i = 0; i < s.n; i++) {
        sag = fmaxf(sag, 1000.0f - s.x[i][2]);
        vmax = fmaxf(vmax, dist3(s.x[i], s.prev[i]) * 60.0f * 6.0f);
        for (int k = 0; k < 3; k++) finite &= isfinite(s.x[i][k]) != 0;
    }
    float L = cable_len(&s), stretch = (L - 1200.0f) / 1200.0f * 100.0f;
    CHECK(finite, "cable_hang: a particle is not finite");
    CHECK(fabsf(sag - sag_th) / sag_th < 0.05f, "cable_hang: sag %.1f cm, the catenary's %.1f cm", sag, sag_th);
    CHECK(fabsf(stretch) < 1.0f, "cable_hang: stretched %.2f%%", stretch);
    CHECK(vmax < 5.0f, "cable_hang: still moving at %.2f cm/s after 10 s", vmax);
    float sag1 = sag, str1 = stretch;
    // a free end: hangs straight down its length
    cable_sim_init(&s, 24, a, b, 600.0f);
    for (int f = 0; f < 600; f++) cable_sim_step(&s, &p, 1.0f / 60.0f, a, NULL, NULL, NULL);
    float drop = a[2] - s.x[s.n - 1][2];
    CHECK(fabsf(drop - 600.0f) < 18.0f && fabsf(s.x[s.n - 1][0]) < 20.0f, "cable_hang: the free end hangs %.1f cm down (600), %.1f cm aside", drop, s.x[s.n - 1][0]);
    // a long stiff wire at 30 Hz, both ends whipped around: it stays finite and within its length's reach
    const float c0[3] = { 0, 0, 0 };
    float c1[3] = { 2000, 0, 0 };
    CableParams ps = { 1.0f, 0.6f, 0.2f, 1.0f, 0.3f, 1.0f, 8, 4, { 0, 0, 0 } };
    cable_sim_init(&s, CABLE_MAX_SEGMENTS, c0, c1, 2400.0f);
    bool stable = true;
    for (int f = 0; f < 300; f++) {
        c1[0] = 2000.0f * cosf((float)f * 0.3f), c1[1] = 2000.0f * sinf((float)f * 0.3f), c1[2] = 500.0f * sinf((float)f * 0.7f);
        cable_sim_step(&s, &ps, 1.0f / 30.0f, c0, c1, NULL, NULL);
        for (uint32_t i = 0; i < s.n; i++) stable &= isfinite(s.x[i][0]) && dist3(s.x[i], c0) < 4000.0f;
    }
    CHECK(stable, "cable_hang: a stiff wire whipped at 30 Hz went unstable");
    // laid over a box (a meter cube, its top at 100): the middle rests on the top, nothing inside
    const float l0[3] = { -300, 0, 300 }, l1[3] = { 300, 0, 300 };
    BoxCtx bx = { { 0, 0, 50 }, { 50, 50, 50 } };
    cable_sim_init(&s, 32, l0, l1, 900.0f);
    for (uint32_t i = 0; i < s.n; i++) // (laid straight above it: it falls onto the box)
        s.x[i][0] = -300.0f + 600.0f * (float)i / (float)(s.n - 1), s.x[i][1] = 0.0f, s.x[i][2] = 300.0f, memcpy(s.prev[i], s.x[i], 12);
    for (int f = 0; f < 600; f++) cable_sim_step(&s, &p, 1.0f / 60.0f, l0, l1, box_collide, &bx);
    uint32_t inside = 0;
    float top = 1e9f;
    for (uint32_t i = 0; i < s.n; i++) {
        inside += fabsf(s.x[i][0]) < 49.0f && fabsf(s.x[i][1]) < 49.0f && s.x[i][2] < 99.0f;
        if (fabsf(s.x[i][0]) < 30.0f) top = fminf(top, s.x[i][2]);
    }
    CHECK(!inside && top > 99.0f && top < 110.0f, "cable_hang: over the box: %u inside, the middle at %.1f cm (the top 100 + its radius)", inside, top);
    // the cost: 64 cables of 32 segments, 4 substeps x 8 iterations (the default), a frame
    static CableSim many[64];
    CableParams pd = { 1.0f, 0.05f, 0.1f, 1.0f, 0.3f, 1.5f, 8, 4, { 300, 0, 0 } };
    for (int i = 0; i < 64; i++) cable_sim_init(&many[i], 32, a, b, 1200.0f);
    double t0 = re_time_seconds();
    for (int f = 0; f < 60; f++)
        for (int i = 0; i < 64; i++) cable_sim_step(&many[i], &pd, 1.0f / 60.0f, a, b, NULL, NULL);
    double us = (re_time_seconds() - t0) * 1e6 / (60.0 * 64.0);
    CHECK(us < 50.0, "cable_hang: a cable's step costs %.1f us (> 50)", us);
    printf("[RealEngine] cable_hang: sag %.1f cm (catenary %.1f), stretch %.3f%%, free end %.1f cm down, a 32-segment cable's step %.2f us\n", sag1, sag_th, str1, drop, us);
    if (ok) printf("[RealEngine] cable_hang: ok\n");
    return ok;
}

bool spline_selftest_named(const char* name) {
    if (!strcmp(name, "spline_eval")) return test_eval();
    if (!strcmp(name, "spline_rail_river")) return test_rail_river();
    if (!strcmp(name, "cable_hang")) return test_cable();
    printf("[RealEngine] FAIL: no spline test \"%s\"\n", name);
    return false;
}
