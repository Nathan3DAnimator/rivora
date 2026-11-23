#include "river.h"

#include <math.h>
#include <string.h>

#include "components.h"
#include "render.h"
#include "spline.h"

uint32_t river_samples(const CRiver* r, const CTransform* t, RiverSample* out, uint32_t max) {
    uint32_t np = r->npoints > RIVER_MAX_POINTS ? RIVER_MAX_POINTS : r->npoints;
    if (np < 2 || max < 2) return 0;
    float rows[3][4], wp[RIVER_MAX_POINTS][3];
    render_transform_rows(t, rows);
    // (Q14) the engine's one curve (spline.h): Curve points' auto tangents are the Catmull-Rom this sampled before
    SplineCurve c;
    spline_curve_points(&c, (const float(*)[3])r->points, np, false, rows);
    for (uint32_t i = 0; i < np; i++) memcpy(wp[i], c.p[i], 12);
    uint32_t per = 8, n = 0;
    for (uint32_t s = 0; s + 1 < np && n < max; s++) {
        for (uint32_t k = 0; k < per && n < max; k++) {
            float u = (float)k / (float)per;
            RiverSample* o = &out[n++];
            spline_eval_key(&c, (float)s + u, o->p, NULL);
            o->width = r->width[s] + (r->width[s + 1] - r->width[s]) * u;
            o->speed = r->speed[s] + (r->speed[s + 1] - r->speed[s]) * u;
            o->depth = r->depth[s] + (r->depth[s + 1] - r->depth[s]) * u;
        }
    }
    if (n < max) { // the last point
        RiverSample* o = &out[n++];
        memcpy(o->p, wp[np - 1], 12);
        o->width = r->width[np - 1], o->speed = r->speed[np - 1], o->depth = r->depth[np - 1];
    }
    for (uint32_t i = 0; i < n; i++) { // the flow along the curve
        const float* a = out[i ? i - 1 : 0].p;
        const float* b = out[i + 1 < n ? i + 1 : n - 1].p;
        float dx = b[0] - a[0], dy = b[1] - a[1], l = sqrtf(dx * dx + dy * dy);
        out[i].dir[0] = l > 1e-4f ? dx / l : 1.0f, out[i].dir[1] = l > 1e-4f ? dy / l : 0.0f;
        if (out[i].width < 10.0f) out[i].width = 10.0f;
    }
    return n;
}

bool river_at(const RiverSample* s, uint32_t n, float x, float y, float* z, float cur[2], float* depth) {
    float best = 1e30f;
    int bi = -1;
    float bt = 0.0f;
    for (uint32_t i = 0; i + 1 < n; i++) {
        float ax = s[i].p[0], ay = s[i].p[1], dx = s[i + 1].p[0] - ax, dy = s[i + 1].p[1] - ay;
        float l2 = dx * dx + dy * dy, t = l2 > 1e-6f ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        float px = ax + dx * t - x, py = ay + dy * t - y, d2 = px * px + py * py;
        if (d2 < best) best = d2, bi = (int)i, bt = t;
    }
    if (bi < 0) return false;
    const RiverSample *a = &s[bi], *b = &s[bi + 1];
    float hw = 0.5f * (a->width + (b->width - a->width) * bt);
    if (best > hw * hw) return false;
    float edge = 1.0f - sqrtf(best) / hw; // (the banks flow slower)
    float sp = (a->speed + (b->speed - a->speed) * bt) * (0.35f + 0.65f * sqrtf(edge));
    *z = a->p[2] + (b->p[2] - a->p[2]) * bt;
    cur[0] = (a->dir[0] + (b->dir[0] - a->dir[0]) * bt) * sp, cur[1] = (a->dir[1] + (b->dir[1] - a->dir[1]) * bt) * sp;
    *depth = (a->depth + (b->depth - a->depth) * bt) * (0.3f + 0.7f * edge);
    return true;
}
