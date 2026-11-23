// The curve (spline.h): cubic Hermite segments between the points, Unreal's Spline Component types, arc length by
// Gauss-Legendre per step, queries by distance and by input key; the component's defaults and point edits. Pure: no
// world, no allocation (the rivers, the rails, the editor, the Blueprint nodes and spline_world.c share it)
#include "spline.h"

#include <math.h>
#include <string.h>

#include "components.h"

const char* const g_spline_kind_names[SPLINE_KIND_COUNT] = { "Rail", "Spline", "Cable" };
const char* const g_spline_point_names[SPLINE_POINT_COUNT] = { "Curve", "Curve Custom Tangent", "Linear", "Constant" };

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}

uint32_t spline_segment_count(const SplineCurve* c) {
    if (c->n < 2) return 0;
    return c->closed ? c->n : c->n - 1;
}
float spline_key_max(const SplineCurve* c) { return (float)spline_segment_count(c); }

// Unreal's auto tangent (tension 0, uniform keys): half the neighbors' difference, the ends clamped (a Catmull-Rom whose
// missing neighbor is the end itself). A Linear / Constant point arrives along its chord
static void auto_tangents(SplineCurve* c) {
    uint32_t n = c->n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ip = c->closed ? (i + n - 1) % n : (i ? i - 1 : 0), in = c->closed ? (i + 1) % n : (i + 1 < n ? i + 1 : n - 1);
        if (c->type[i] == SPLINE_POINT_CUSTOM) continue;
        for (int k = 0; k < 3; k++) c->leave[i][k] = c->arrive[i][k] = 0.5f * (c->p[in][k] - c->p[ip][k]);
        if (c->type[i] == SPLINE_POINT_LINEAR || c->type[i] == SPLINE_POINT_CONSTANT)
            for (int k = 0; k < 3; k++) c->arrive[i][k] = c->p[i][k] - c->p[ip][k];
    }
}

void spline_segment(const SplineCurve* c, uint32_t s, float p0[3], float t0[3], float p1[3], float t1[3]) {
    uint32_t n = c->n, i0 = s % n, i1 = (s + 1) % n;
    memcpy(p0, c->p[i0], 12), memcpy(p1, c->p[i1], 12);
    switch (c->type[i0]) {
    case SPLINE_POINT_LINEAR:
        for (int k = 0; k < 3; k++) t0[k] = t1[k] = p1[k] - p0[k];
        break;
    case SPLINE_POINT_CONSTANT:
        memcpy(p1, p0, 12);
        memset(t0, 0, 12), memset(t1, 0, 12);
        break;
    default:
        memcpy(t0, c->leave[i0], 12), memcpy(t1, c->arrive[i1], 12);
        break;
    }
}

static void hermite(const float p0[3], const float t0[3], const float p1[3], const float t1[3], float t, float pos[3], float d[3]) {
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    float d00 = 6 * t2 - 6 * t, d10 = 3 * t2 - 4 * t + 1, d01 = -6 * t2 + 6 * t, d11 = 3 * t2 - 2 * t;
    for (int k = 0; k < 3; k++) {
        if (pos) pos[k] = h00 * p0[k] + h10 * t0[k] + h01 * p1[k] + h11 * t1[k];
        if (d) d[k] = d00 * p0[k] + d10 * t0[k] + d01 * p1[k] + d11 * t1[k];
    }
}

static float speed_at(const float p0[3], const float t0[3], const float p1[3], const float t1[3], float t) {
    float d[3];
    hermite(p0, t0, p1, t1, t, NULL, d);
    return sqrtf(dot3(d, d));
}

void spline_curve_finish(SplineCurve* c) {
    if (c->n > SPLINE_MAX_POINTS) c->n = SPLINE_MAX_POINTS;
    auto_tangents(c);
    uint32_t ns = spline_segment_count(c);
    c->dist[0] = 0.0f;
    float acc = 0.0f;
    static const float gx[3] = { -0.7745966692f, 0.0f, 0.7745966692f }, gw[3] = { 0.5555555556f, 0.8888888889f, 0.5555555556f };
    for (uint32_t s = 0; s < ns; s++) {
        float p0[3], t0[3], p1[3], t1[3];
        spline_segment(c, s, p0, t0, p1, t1);
        for (uint32_t k = 0; k < SPLINE_STEPS; k++) { // each step's length: 3-point Gauss-Legendre of the speed
            float a = (float)k / SPLINE_STEPS, h = 0.5f / SPLINE_STEPS, l = 0.0f;
            for (int g = 0; g < 3; g++) l += gw[g] * speed_at(p0, t0, p1, t1, a + h + h * gx[g]);
            acc += l * h;
            c->dist[s * SPLINE_STEPS + k + 1] = acc;
        }
    }
    c->length = acc;
}

static void curve_reset(SplineCurve* c, uint32_t n, bool closed) {
    c->n = n > SPLINE_MAX_POINTS ? SPLINE_MAX_POINTS : n;
    c->closed = closed && c->n > 2;
    c->up[0] = c->up[1] = 0.0f, c->up[2] = 1.0f;
    for (uint32_t i = 0; i < c->n; i++) c->roll[i] = 0.0f, c->scale[i][0] = c->scale[i][1] = 1.0f, c->type[i] = SPLINE_POINT_CURVE;
}
static void xform_point(const float rows[3][4], const float p[3], float o[3]) {
    for (int r = 0; r < 3; r++) o[r] = rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2] + rows[r][3];
}
static void xform_vec(const float rows[3][4], const float v[3], float o[3]) {
    for (int r = 0; r < 3; r++) o[r] = rows[r][0] * v[0] + rows[r][1] * v[1] + rows[r][2] * v[2];
}

void spline_curve_points(SplineCurve* c, const float (*pts)[3], uint32_t n, bool closed, const float rows[3][4]) {
    curve_reset(c, n, closed);
    for (uint32_t i = 0; i < c->n; i++) {
        if (rows) xform_point(rows, pts[i], c->p[i]);
        else memcpy(c->p[i], pts[i], 12);
    }
    if (rows) {
        float z[3] = { rows[0][2], rows[1][2], rows[2][2] };
        if (norm3(z) > 1e-9f) memcpy(c->up, z, 12);
    }
    spline_curve_finish(c);
}

void spline_curve_from(SplineCurve* c, const CRail* r, const float rows[3][4]) {
    curve_reset(c, r->npoints, r->closed != 0);
    for (uint32_t i = 0; i < c->n; i++) {
        uint8_t ty = r->ptype[i] < SPLINE_POINT_COUNT ? r->ptype[i] : SPLINE_POINT_CURVE;
        c->type[i] = ty;
        c->roll[i] = r->roll[i] * 0.017453293f;
        c->scale[i][0] = r->pscale[i][0], c->scale[i][1] = r->pscale[i][1];
        if (rows) {
            xform_point(rows, r->points[i], c->p[i]);
            xform_vec(rows, r->arrive[i], c->arrive[i]);
            xform_vec(rows, r->leave[i], c->leave[i]);
        } else {
            memcpy(c->p[i], r->points[i], 12);
            memcpy(c->arrive[i], r->arrive[i], 12);
            memcpy(c->leave[i], r->leave[i], 12);
        }
    }
    if (rows) {
        float z[3] = { rows[0][2], rows[1][2], rows[2][2] };
        if (norm3(z) > 1e-9f) memcpy(c->up, z, 12);
    }
    spline_curve_finish(c);
}

static float clamp_key(const SplineCurve* c, float key, uint32_t* seg, float* t) {
    uint32_t ns = spline_segment_count(c);
    if (!ns) return *seg = 0, *t = 0.0f, 0.0f;
    if (c->closed) {
        key = fmodf(key, (float)ns);
        if (key < 0.0f) key += (float)ns;
    } else key = key < 0.0f ? 0.0f : key > (float)ns ? (float)ns : key;
    uint32_t s = (uint32_t)floorf(key);
    if (s >= ns) s = ns - 1;
    *seg = s, *t = key - (float)s;
    return key;
}

void spline_eval_key(const SplineCurve* c, float key, float pos[3], float deriv[3]) {
    if (c->n == 0) {
        if (pos) memset(pos, 0, 12);
        if (deriv) deriv[0] = 1.0f, deriv[1] = deriv[2] = 0.0f;
        return;
    }
    if (c->n == 1) {
        if (pos) memcpy(pos, c->p[0], 12);
        if (deriv) deriv[0] = 1.0f, deriv[1] = deriv[2] = 0.0f;
        return;
    }
    uint32_t s;
    float t, p0[3], t0[3], p1[3], t1[3];
    clamp_key(c, key, &s, &t);
    spline_segment(c, s, p0, t0, p1, t1);
    hermite(p0, t0, p1, t1, t, pos, deriv);
    if (deriv && dot3(deriv, deriv) < 1e-12f) { // (a Constant segment, a cusp: the chord to the next point)
        uint32_t i1 = (s + 1) % c->n;
        for (int k = 0; k < 3; k++) deriv[k] = c->p[i1][k] - c->p[s % c->n][k];
        if (dot3(deriv, deriv) < 1e-12f) deriv[0] = 1.0f, deriv[1] = deriv[2] = 0.0f;
    }
}

float spline_key_at_distance(const SplineCurve* c, float d) {
    uint32_t ns = spline_segment_count(c);
    if (!ns || c->length <= 0.0f) return 0.0f;
    if (c->closed) {
        d = fmodf(d, c->length);
        if (d < 0.0f) d += c->length;
    } else d = d < 0.0f ? 0.0f : d > c->length ? c->length : d;
    uint32_t lo = 0, hi = ns * SPLINE_STEPS;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (c->dist[mid] <= d) lo = mid;
        else hi = mid;
    }
    float a = c->dist[lo], b = c->dist[hi], f = b - a > 1e-9f ? (d - a) / (b - a) : 0.0f;
    // two Newton steps on the step's exact length (Gauss-Legendre from its start): the speed changes along a step, the
    // linear guess was half a centimeter off on a 10 m line
    uint32_t s = lo / SPLINE_STEPS;
    float k0 = (float)(lo % SPLINE_STEPS) / SPLINE_STEPS, t = k0 + f / SPLINE_STEPS;
    float p0[3], t0[3], p1[3], t1[3];
    spline_segment(c, s, p0, t0, p1, t1);
    static const float gx[3] = { -0.7745966692f, 0.0f, 0.7745966692f }, gw[3] = { 0.5555555556f, 0.8888888889f, 0.5555555556f };
    for (int it = 0; it < 2; it++) {
        float h = 0.5f * (t - k0), l = 0.0f;
        for (int g = 0; g < 3; g++) l += gw[g] * speed_at(p0, t0, p1, t1, k0 + h + h * gx[g]);
        float sp = speed_at(p0, t0, p1, t1, t);
        if (sp < 1e-6f) break;
        t += (d - a - l * h) / sp;
        float lo_t = k0, hi_t = k0 + 1.0f / SPLINE_STEPS;
        t = t < lo_t ? lo_t : t > hi_t ? hi_t : t;
    }
    return (float)s + t;
}

float spline_distance_at_key(const SplineCurve* c, float key) {
    uint32_t ns = spline_segment_count(c);
    if (!ns) return 0.0f;
    uint32_t s;
    float t;
    float k = clamp_key(c, key, &s, &t);
    float x = k * SPLINE_STEPS;
    uint32_t i = (uint32_t)floorf(x);
    if (i >= ns * SPLINE_STEPS) return c->length;
    return c->dist[i] + (c->dist[i + 1] - c->dist[i]) * (x - (float)i);
}

void spline_location_at_distance(const SplineCurve* c, float d, float out[3]) { spline_eval_key(c, spline_key_at_distance(c, d), out, NULL); }
void spline_direction_at_distance(const SplineCurve* c, float d, float out[3]) {
    spline_eval_key(c, spline_key_at_distance(c, d), NULL, out);
    norm3(out);
}

void spline_frame(const float fwd_in[3], const float up_in[3], float roll, float right[3], float upo[3]) {
    float f[3] = { fwd_in[0], fwd_in[1], fwd_in[2] };
    if (norm3(f) < 1e-12f) f[0] = 1.0f, f[1] = f[2] = 0.0f;
    float up[3] = { up_in[0], up_in[1], up_in[2] };
    if (fabsf(dot3(f, up)) > 0.999f) up[0] = fabsf(f[0]) < 0.9f ? 1.0f : 0.0f, up[1] = fabsf(f[0]) < 0.9f ? 0.0f : 1.0f, up[2] = 0.0f;
    float r[3];
    cross3(up, f, r); // (Unreal's: X forward, Y right, Z up)
    norm3(r);
    float u[3];
    cross3(f, r, u);
    float cr = cosf(roll), sr = sinf(roll);
    for (int k = 0; k < 3; k++) right[k] = r[k] * cr + u[k] * sr, upo[k] = u[k] * cr - r[k] * sr;
}

float spline_roll_at_key(const SplineCurve* c, float key) {
    uint32_t s;
    float t;
    if (c->n < 2) return c->n ? c->roll[0] : 0.0f;
    clamp_key(c, key, &s, &t);
    return c->roll[s % c->n] + (c->roll[(s + 1) % c->n] - c->roll[s % c->n]) * t;
}

void spline_frame_at_key(const SplineCurve* c, float key, float pos[3], float fwd[3], float right[3], float up[3], float scale[2]) {
    float d[3], p[3];
    spline_eval_key(c, key, p, d);
    norm3(d);
    if (pos) memcpy(pos, p, 12);
    if (fwd) memcpy(fwd, d, 12);
    float r[3], u[3];
    spline_frame(d, c->up, spline_roll_at_key(c, key), r, u);
    if (right) memcpy(right, r, 12);
    if (up) memcpy(up, u, 12);
    if (scale) {
        uint32_t s;
        float t;
        if (c->n < 2) scale[0] = scale[1] = 1.0f;
        else {
            clamp_key(c, key, &s, &t);
            const float *a = c->scale[s % c->n], *b = c->scale[(s + 1) % c->n];
            scale[0] = a[0] + (b[0] - a[0]) * t, scale[1] = a[1] + (b[1] - a[1]) * t;
        }
    }
}

float spline_closest_key(const SplineCurve* c, const float p[3]) {
    uint32_t ns = spline_segment_count(c);
    if (!ns) return 0.0f;
    float best = 1e30f, bk = 0.0f;
    for (uint32_t i = 0; i <= ns * SPLINE_STEPS; i++) { // the nearest sample, then Newton on the squared distance
        float q[3], k = (float)i / SPLINE_STEPS;
        spline_eval_key(c, k, q, NULL);
        float dd = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
        if (dd < best) best = dd, bk = k;
    }
    float h = 1.0f / SPLINE_STEPS;
    for (int it = 0; it < 6; it++) {
        float q[3], d[3], a[3], b[3];
        spline_eval_key(c, bk, q, d);
        spline_eval_key(c, bk + h * 0.01f, NULL, a);
        spline_eval_key(c, bk - h * 0.01f, NULL, b);
        float e[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
        float g = dot3(e, d), dd2[3] = { (a[0] - b[0]) / (h * 0.02f), (a[1] - b[1]) / (h * 0.02f), (a[2] - b[2]) / (h * 0.02f) };
        float H = dot3(d, d) + dot3(e, dd2);
        if (fabsf(H) < 1e-9f) break;
        float step = -g / H;
        if (step > h) step = h;
        if (step < -h) step = -h;
        bk += step;
        if (!c->closed) bk = bk < 0.0f ? 0.0f : bk > (float)ns ? (float)ns : bk;
        if (fabsf(step) < 1e-6f) break;
    }
    return bk;
}

// --- The component ---

uint32_t spline_kind(const CRail* r) { return r->kind < SPLINE_KIND_COUNT ? r->kind : SPLINE_KIND_SPLINE; }

void spline_component_defaults(CRail* r, uint32_t kind) {
    uint32_t built = r->built;
    memset(r, 0, sizeof *r);
    r->built = built;
    r->kind = kind;
    r->gauge = 143.5f; // (a Rail's standard gauge)
    for (int i = 0; i < SPLINE_MAX_POINTS; i++) r->pscale[i][0] = r->pscale[i][1] = 1.0f;
    if (kind == SPLINE_KIND_RAIL) {
        r->npoints = 4, r->closed = 1;
        const float pts[4][3] = { { 0, 0, 0 }, { 4000, 0, 0 }, { 4000, 3000, 0 }, { 0, 3000, 0 } };
        memcpy(r->points, pts, sizeof pts);
    } else if (kind == SPLINE_KIND_SPLINE) { // Unreal's two points along X (a little longer: a segment you can see)
        r->npoints = 2;
        r->points[1][0] = 500.0f;
    }
    r->mesh = SHAPE_CUBE, r->mesh_axis = 0, r->mesh_length = 0.0f;
    r->mesh_color[0] = r->mesh_color[1] = r->mesh_color[2] = 0.6f, r->mesh_roughness = 0.6f;
    r->inst_mesh = SHAPE_CYLINDER, r->inst_spacing = 200.0f, r->inst_seed = 1, r->inst_scale_min = r->inst_scale_max = 1.0f;
    r->inst_align = SPLINE_ALIGN_UPRIGHT, r->inst_color[0] = r->inst_color[1] = r->inst_color[2] = 0.5f;
    r->inst_scale3[0] = r->inst_scale3[1] = r->inst_scale3[2] = 1.0f;
    r->cable_end_offset[0] = 600.0f;
    r->cable_segments = 24, r->cable_stiffness = 1.0f, r->cable_bend = 0.05f, r->cable_iterations = 8, r->cable_substeps = 4;
    r->cable_damping = 0.1f, r->cable_gravity = 1.0f, r->cable_width = 3.0f, r->cable_render = CABLE_DRAW_TUBE;
    r->cable_collide = SPLINE_COLLIDE_WORLD | SPLINE_COLLIDE_BODIES, r->cable_friction = 0.3f, r->cable_wind_dir = 0.0f, r->cable_gusts = 0.4f;
    r->cable_color[0] = r->cable_color[1] = r->cable_color[2] = 0.03f, r->cable_roughness = 0.45f;
    if (kind == SPLINE_KIND_CABLE) r->cable_length = 700.0f;
}

bool spline_insert_point(CRail* r, int i) {
    uint32_t n = r->npoints;
    if (n >= SPLINE_MAX_POINTS || i < 0 || (uint32_t)i >= n) return false;
    SplineCurve c;
    spline_curve_from(&c, r, NULL);
    float np[3];
    bool last = (uint32_t)i == n - 1 && !r->closed;
    if (last) { // past the end: as far again along its direction
        float d[3];
        spline_eval_key(&c, (float)i, NULL, d);
        float l = sqrtf(dot3(d, d)), step = n > 1 ? sqrtf((r->points[i][0] - r->points[i - 1][0]) * (r->points[i][0] - r->points[i - 1][0]) +
                                                         (r->points[i][1] - r->points[i - 1][1]) * (r->points[i][1] - r->points[i - 1][1]) +
                                                         (r->points[i][2] - r->points[i - 1][2]) * (r->points[i][2] - r->points[i - 1][2]))
                                                 : 500.0f;
        for (int k = 0; k < 3; k++) np[k] = r->points[i][k] + (l > 1e-6f ? d[k] / l : (k == 0)) * (step > 1.0f ? step : 500.0f);
    } else spline_eval_key(&c, (float)i + 0.5f, np, NULL); // halfway along the curve to the next
    uint32_t at = (uint32_t)i + 1, m = n - at;
    memmove(&r->points[at + 1], &r->points[at], sizeof(float) * 3 * m);
    memmove(&r->arrive[at + 1], &r->arrive[at], sizeof(float) * 3 * m);
    memmove(&r->leave[at + 1], &r->leave[at], sizeof(float) * 3 * m);
    memmove(&r->roll[at + 1], &r->roll[at], sizeof(float) * m);
    memmove(&r->pscale[at + 1], &r->pscale[at], sizeof(float) * 2 * m);
    memmove(&r->ptype[at + 1], &r->ptype[at], m);
    memcpy(r->points[at], np, 12);
    uint32_t nx = last ? (uint32_t)i : (at + 1) % (n + 1);
    r->roll[at] = 0.5f * (r->roll[i] + r->roll[nx]);
    r->pscale[at][0] = 0.5f * (r->pscale[i][0] + r->pscale[nx][0]), r->pscale[at][1] = 0.5f * (r->pscale[i][1] + r->pscale[nx][1]);
    r->ptype[at] = r->ptype[i] == SPLINE_POINT_CUSTOM ? SPLINE_POINT_CURVE : r->ptype[i];
    memset(r->arrive[at], 0, 12), memset(r->leave[at], 0, 12);
    r->npoints = n + 1;
    return true;
}

bool spline_remove_point(CRail* r, int i) {
    uint32_t n = r->npoints;
    if (n <= 2 || i < 0 || (uint32_t)i >= n) return false;
    uint32_t m = n - (uint32_t)i - 1;
    memmove(&r->points[i], &r->points[i + 1], sizeof(float) * 3 * m);
    memmove(&r->arrive[i], &r->arrive[i + 1], sizeof(float) * 3 * m);
    memmove(&r->leave[i], &r->leave[i + 1], sizeof(float) * 3 * m);
    memmove(&r->roll[i], &r->roll[i + 1], sizeof(float) * m);
    memmove(&r->pscale[i], &r->pscale[i + 1], sizeof(float) * 2 * m);
    memmove(&r->ptype[i], &r->ptype[i + 1], m);
    r->npoints = n - 1;
    if (r->npoints < 3) r->closed = 0;
    return true;
}

void spline_set_point_type(CRail* r, int i, uint32_t type) {
    if (i < 0 || (uint32_t)i >= r->npoints || type >= SPLINE_POINT_COUNT) return;
    if (type == SPLINE_POINT_CUSTOM && r->ptype[i] != SPLINE_POINT_CUSTOM) { // (it starts from the tangents it had)
        SplineCurve c;
        spline_curve_from(&c, r, NULL);
        memcpy(r->arrive[i], c.arrive[i], 12), memcpy(r->leave[i], c.leave[i], 12);
    }
    r->ptype[i] = (uint8_t)type;
}
