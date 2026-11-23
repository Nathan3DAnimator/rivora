#include "softbody.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mesh_build.h"
#include "platform.h"
#include "render.h"

const char* const g_soft_preset_names[SOFT_PRESET_COUNT] = { "Custom", "Foam", "Rubber", "Jelly", "Cushion", "Flesh" };
const char* const g_soft_attach_names[SOFT_ATTACH_COUNT] = { "Free", "Bottom", "Band", "Bone", "Painted" };

// Kuhn's 6 tetrahedra of a cube: 0 -> e_a -> e_a + e_b -> 7 for each order (a, b, c) of the axes. They share the main
// diagonal and tile every cube alike, so the faces of neighbor cubes match (no parity to keep)
const uint8_t g_soft_tets[6][4] = { { 0, 1, 3, 7 }, { 0, 1, 5, 7 }, { 0, 2, 3, 7 }, { 0, 2, 6, 7 }, { 0, 4, 5, 7 }, { 0, 4, 6, 7 } };

// density kg/m3, Young's kPa, Poisson, damping, plasticity, recovery s, yield, friction, cells
static const float k_soft[SOFT_PRESET_COUNT][9] = {
    { 0 },
    { 35, 30, 0.25f, 0.08f, 0.35f, 4.0f, 0.2f, 0.6f, 10 },  // foam: memory foam, slow to come back
    { 1100, 400, 0.47f, 0.01f, 0.0f, 0.0f, 0.0f, 0.8f, 8 }, // rubber: bounces, keeps its volume
    { 1050, 3, 0.48f, 0.004f, 0.0f, 0.0f, 0.0f, 0.4f, 8 },  // jelly: wobbles long
    { 40, 12, 0.3f, 0.05f, 0.15f, 3.0f, 0.25f, 0.7f, 10 },  // cushion: a sofa's seat
    { 1000, 20, 0.45f, 0.04f, 0.0f, 0.0f, 0.0f, 0.6f, 10 }, // flesh: belly, fat, held to the bones
};

void soft_apply_preset(CCloth* c, int preset) {
    if (preset <= SOFT_CUSTOM || preset >= SOFT_PRESET_COUNT) return;
    const float* p = k_soft[preset];
    c->soft_preset = (uint32_t)preset;
    c->soft_density = p[0], c->youngs = p[1], c->poisson = p[2], c->soft_damping = p[3], c->plasticity = p[4], c->recovery = p[5];
    c->yield = p[6], c->friction = p[7];
    if (preset == SOFT_FLESH) c->attach = SOFT_ATTACH_BONE, c->attach_falloff = 8.0f;
}

void soft_topo_key(uint32_t key[12], uint32_t mesh, const float scale[3], const CCloth* c) {
    memset(key, 0, sizeof(uint32_t) * 12);
    key[0] = mesh;
    memcpy(&key[1], scale, 12);
    key[4] = c->soft_res, key[5] = c->attach;
    memcpy(&key[6], &c->pin_band, 4), memcpy(&key[7], c->pin_axis, 12), memcpy(&key[10], &c->attach_falloff, 4);
    key[11] = 0x50F7u; // (0: no key)
}

void soft_topo_free(SoftTopo* t) {
    free(t->cells), free(t->rest), free(t->aux), free(t->color), free(t->embed);
    memset(t, 0, sizeof *t);
}

static float pt_tri_dist2(const float p[3], const float a[3], const float b[3], const float c[3]) {
    // (Ericson's closest point on a triangle)
    float ab[3], ac[3], ap[3], q[3];
    for (int k = 0; k < 3; k++) ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k];
    float d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2], d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
    if (d1 <= 0.0f && d2 <= 0.0f) { memcpy(q, a, 12); goto done; }
    float bp[3];
    for (int k = 0; k < 3; k++) bp[k] = p[k] - b[k];
    float d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2], d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
    if (d3 >= 0.0f && d4 <= d3) { memcpy(q, b, 12); goto done; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k];
        goto done;
    }
    float cp[3];
    for (int k = 0; k < 3; k++) cp[k] = p[k] - c[k];
    float d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2], d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
    if (d6 >= 0.0f && d5 <= d6) { memcpy(q, c, 12); goto done; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) q[k] = a[k] + w * ac[k];
        goto done;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
        goto done;
    }
    {
        float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
        for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;
    }
done:;
    float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

bool soft_topo_build_arrays(SoftTopo* t, const float* pos, uint32_t npos, const uint32_t* idx, uint32_t nidx, const uint32_t* colors,
                            const CCloth* c) {
    double t0 = re_time_seconds();
    uint32_t key[12];
    memcpy(key, t->key, sizeof key);
    soft_topo_free(t);
    memcpy(t->key, key, sizeof key);
    if (npos < 3 || nidx < 3) return false;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < npos; i++)
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], pos[3 * i + k]), mx[k] = fmaxf(mx[k], pos[3 * i + k]);
    float ext = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2]));
    uint32_t res = c->soft_res < 2 ? 2 : c->soft_res > SOFT_MAX_RES ? SOFT_MAX_RES : c->soft_res;
    float cell = fmaxf(ext / (float)res, 0.5f);
    uint32_t nc[3];
    for (int k = 0; k < 3; k++) {
        uint32_t n = (uint32_t)ceilf((mx[k] - mn[k]) / cell - 1e-4f);
        nc[k] = (n < 1 ? 1 : n) + 2; // (a cell of padding each side: the outside's flood goes round)
        float span = (float)(nc[k] - 2) * cell;
        t->origin[k] = mn[k] - 0.5f * (span - (mx[k] - mn[k])) - cell; // (the mesh centered in its cells)
    }
    t->cell = cell;
    t->nx = nc[0] + 1, t->ny = nc[1] + 1, t->nz = nc[2] + 1;
    t->nv = t->nx * t->ny * t->nz;
    uint32_t ncell = nc[0] * nc[1] * nc[2];
    uint8_t* mark = calloc(ncell, 1); // 1 the surface crosses it, 2 outside, 0 inside
    uint32_t* stack = malloc(sizeof(uint32_t) * ncell);
    t->cells = calloc(t->nv, 1), t->rest = calloc(t->nv, 16), t->aux = calloc(t->nv, 8), t->color = calloc(t->nv, 1);
    t->nrender = npos, t->embed = calloc(npos, 16);
    if (!mark || !stack || !t->cells || !t->rest || !t->aux || !t->color || !t->embed) {
        free(mark), free(stack), soft_topo_free(t);
        return false;
    }
#define CIDX(x, y, z) (((z) * nc[1] + (y)) * nc[0] + (x))
#define VIDX(x, y, z) (((z) * t->ny + (y)) * t->nx + (x))
    // the surface: each triangle sampled at half a cell (its cells marked)
    for (uint32_t f = 0; f + 2 < nidx; f += 3) {
        const float *a = &pos[3 * idx[f]], *b = &pos[3 * idx[f + 1]], *cc = &pos[3 * idx[f + 2]];
        if (idx[f] >= npos || idx[f + 1] >= npos || idx[f + 2] >= npos) continue;
        float l = 0.0f;
        for (int k = 0; k < 3; k++) l = fmaxf(l, fmaxf(fabsf(b[k] - a[k]), fmaxf(fabsf(cc[k] - a[k]), fabsf(cc[k] - b[k]))));
        int n = (int)ceilf(l / (0.4f * cell));
        if (n < 1) n = 1;
        if (n > 512) n = 512;
        for (int i = 0; i <= n; i++)
            for (int j = 0; i + j <= n; j++) {
                float u = (float)i / n, v = (float)j / n, w = 1.0f - u - v;
                int q[3];
                for (int k = 0; k < 3; k++) {
                    float p = a[k] * w + b[k] * u + cc[k] * v;
                    q[k] = (int)floorf((p - t->origin[k]) / cell);
                    q[k] = q[k] < 1 ? 1 : q[k] > (int)nc[k] - 2 ? (int)nc[k] - 2 : q[k];
                }
                mark[CIDX(q[0], q[1], q[2])] = 1;
            }
    }
    // the outside: flooded from the padding's corner through the cells no surface crosses
    uint32_t sp = 0;
    mark[0] = 2, stack[sp++] = 0;
    while (sp) {
        uint32_t ci = stack[--sp];
        int x = (int)(ci % nc[0]), y = (int)((ci / nc[0]) % nc[1]), z = (int)(ci / (nc[0] * nc[1]));
        static const int d6[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int d = 0; d < 6; d++) {
            int X = x + d6[d][0], Y = y + d6[d][1], Z = z + d6[d][2];
            if (X < 0 || Y < 0 || Z < 0 || X >= (int)nc[0] || Y >= (int)nc[1] || Z >= (int)nc[2]) continue;
            uint32_t ni = CIDX(X, Y, Z);
            if (mark[ni]) continue;
            mark[ni] = 2, stack[sp++] = ni;
        }
    }
    free(stack);
    // the filled cells (crossed or inside) give their 8 corners a bit each
    for (uint32_t z = 0; z < nc[2]; z++)
        for (uint32_t y = 0; y < nc[1]; y++)
            for (uint32_t x = 0; x < nc[0]; x++) {
                if (mark[CIDX(x, y, z)] == 2) continue;
                t->filled++;
                for (uint32_t b = 0; b < 8; b++) { // corner (x + bx, y + by, z + bz): this cell is its cell (1 - bx, 1 - by, 1 - bz)
                    uint32_t bx = b & 1u, by = (b >> 1) & 1u, bz = b >> 2;
                    t->cells[VIDX(x + bx, y + by, z + bz)] |= (uint8_t)(1u << (7u - b));
                }
            }
    // the active vertices: their rest, color, the hold's weight
    float ax[3] = { c->pin_axis[0], c->pin_axis[1], c->pin_axis[2] };
    float al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (al < 1e-6f) ax[0] = ax[1] = 0.0f, ax[2] = 1.0f, al = 1.0f;
    for (int k = 0; k < 3; k++) ax[k] /= al;
    float smin = 1e30f, smax = -1e30f;
    double cs[3] = { 0, 0, 0 };
    for (uint32_t z = 0; z < t->nz; z++)
        for (uint32_t y = 0; y < t->ny; y++)
            for (uint32_t x = 0; x < t->nx; x++) {
                uint32_t v = VIDX(x, y, z);
                float* r = &t->rest[4 * v];
                r[0] = t->origin[0] + (float)x * cell, r[1] = t->origin[1] + (float)y * cell, r[2] = t->origin[2] + (float)z * cell;
                t->color[v] = (uint8_t)((x & 1u) + 2u * (y & 1u) + 4u * (z & 1u));
                t->aux[2 * v + 1] = (float)t->cells[v];
                if (!t->cells[v]) continue;
                t->nactive++;
                float s = r[0] * ax[0] + r[1] * ax[1] + r[2] * ax[2];
                smin = fminf(smin, s), smax = fmaxf(smax, s);
                for (int k = 0; k < 3; k++) cs[k] += r[k];
            }
    if (!t->nactive) {
        free(mark), soft_topo_free(t);
        return false;
    }
    for (int k = 0; k < 3; k++) t->center[k] = (float)(cs[k] / t->nactive);
    float band = fmaxf(c->pin_band, 0.3f * cell), fall = fmaxf(c->attach_falloff, 0.0f);
    for (uint32_t v = 0; v < t->nv; v++) {
        if (!t->cells[v]) continue;
        float* r = &t->rest[4 * v];
        float dx = r[0] - t->center[0], dy = r[1] - t->center[1], dz = r[2] - t->center[2];
        t->radius = fmaxf(t->radius, sqrtf(dx * dx + dy * dy + dz * dz));
        float s = r[0] * ax[0] + r[1] * ax[1] + r[2] * ax[2], d = -1.0f;
        if (c->attach == SOFT_ATTACH_BOTTOM) d = s - smin;
        else if (c->attach == SOFT_ATTACH_BAND) d = smax - s;
        else if (c->attach == SOFT_ATTACH_BONE) d = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); // (from the actor's origin: the bone it follows)
        float wgt = 0.0f;
        if (d >= 0.0f) wgt = d <= band ? 1.0f : fall > 0.0f ? fmaxf(0.0f, 1.0f - (d - band) / fall) : 0.0f;
        r[3] = wgt;
    }
    // the embedding: each render vertex's place in a filled cell (a vertex on a cell's face: the filled side)
    for (uint32_t i = 0; i < npos; i++) {
        float q[3];
        int cc[3];
        for (int k = 0; k < 3; k++) q[k] = (pos[3 * i + k] - t->origin[k]) / cell;
        for (int k = 0; k < 3; k++) cc[k] = (int)floorf(q[k]), cc[k] = cc[k] < 0 ? 0 : cc[k] > (int)nc[k] - 1 ? (int)nc[k] - 1 : cc[k];
        if (mark[CIDX(cc[0], cc[1], cc[2])] == 2) { // (on a face or a corner: the nearest filled cell around)
            float best = 1e30f;
            int bc[3] = { cc[0], cc[1], cc[2] };
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int X = cc[0] + dx, Y = cc[1] + dy, Z = cc[2] + dz;
                        if (X < 0 || Y < 0 || Z < 0 || X >= (int)nc[0] || Y >= (int)nc[1] || Z >= (int)nc[2] || mark[CIDX(X, Y, Z)] == 2) continue;
                        float e = 0.0f;
                        int o[3] = { X, Y, Z };
                        for (int k = 0; k < 3; k++) {
                            float cl = fminf(fmaxf(q[k], (float)o[k]), (float)o[k] + 1.0f);
                            e += (cl - q[k]) * (cl - q[k]);
                        }
                        if (e < best) best = e, bc[0] = X, bc[1] = Y, bc[2] = Z;
                    }
            memcpy(cc, bc, sizeof cc);
        }
        for (int k = 0; k < 3; k++) t->embed[4 * i + k] = fminf(fmaxf(q[k], (float)cc[k]), (float)cc[k] + 1.0f);
        t->embed[4 * i + 3] = 0.0f;
    }
    // painted: the mesh's red near each lattice vertex (its render vertices weighted by their trilinear share)
    if (c->attach == SOFT_ATTACH_PAINTED && colors) {
        float* acc = calloc(t->nv, 8);
        if (acc) {
            for (uint32_t i = 0; i < npos; i++) {
                const float* q = &t->embed[4 * i];
                int c0[3] = { (int)floorf(q[0]), (int)floorf(q[1]), (int)floorf(q[2]) };
                float red = (float)(colors[i] & 255u) / 255.0f;
                for (uint32_t b = 0; b < 8; b++) {
                    int X = c0[0] + (int)(b & 1u), Y = c0[1] + (int)((b >> 1) & 1u), Z = c0[2] + (int)(b >> 2);
                    if (X < 0 || Y < 0 || Z < 0 || X >= (int)t->nx || Y >= (int)t->ny || Z >= (int)t->nz) continue;
                    float wx = (b & 1u) ? q[0] - c0[0] : 1.0f - (q[0] - c0[0]), wy = ((b >> 1) & 1u) ? q[1] - c0[1] : 1.0f - (q[1] - c0[1]);
                    float wz = (b >> 2) ? q[2] - c0[2] : 1.0f - (q[2] - c0[2]), wt = wx * wy * wz;
                    uint32_t v = VIDX(X, Y, Z);
                    acc[2 * v] += wt * red, acc[2 * v + 1] += wt;
                }
            }
            for (uint32_t v = 0; v < t->nv; v++)
                if (t->cells[v]) t->rest[4 * v + 3] = acc[2 * v + 1] > 1e-4f ? fminf(acc[2 * v] / acc[2 * v + 1], 1.0f) : 0.0f;
            free(acc);
        }
    }
    for (uint32_t v = 0; v < t->nv; v++) {
        if (!t->cells[v]) continue;
        float wgt = t->rest[4 * v + 3];
        t->pinned += wgt >= 0.999f, t->held += wgt > 0.001f && wgt < 0.999f;
    }
    // the surface's offset: a boundary vertex (an empty cell beside it) stands outside the mesh by its distance to it
    // (its contacts let it in by that much: the mesh, not the lattice, touches what it meets)
    for (uint32_t f = 0; f + 2 < nidx; f += 3) {
        if (idx[f] >= npos || idx[f + 1] >= npos || idx[f + 2] >= npos) continue;
        const float *a = &pos[3 * idx[f]], *b = &pos[3 * idx[f + 1]], *cc = &pos[3 * idx[f + 2]];
        int lo[3], hi[3];
        for (int k = 0; k < 3; k++) {
            float tmn = fminf(a[k], fminf(b[k], cc[k])), tmx = fmaxf(a[k], fmaxf(b[k], cc[k]));
            lo[k] = (int)floorf((tmn - t->origin[k]) / cell) - 1, hi[k] = (int)ceilf((tmx - t->origin[k]) / cell) + 1;
            int top = k == 0 ? (int)t->nx - 1 : k == 1 ? (int)t->ny - 1 : (int)t->nz - 1;
            lo[k] = lo[k] < 0 ? 0 : lo[k], hi[k] = hi[k] > top ? top : hi[k];
        }
        for (int z = lo[2]; z <= hi[2]; z++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int x = lo[0]; x <= hi[0]; x++) {
                    uint32_t v = VIDX(x, y, z);
                    if (!t->cells[v] || t->cells[v] == 0xFF) continue;
                    float d = sqrtf(pt_tri_dist2(&t->rest[4 * v], a, b, cc)) * 0.01f; // (m)
                    float* o = &t->aux[2 * v];
                    if (o[0] == 0.0f || d < o[0] - 1.0f) o[0] = d + 1.0f; // (stored + 1: 0 none yet)
                    else if (d + 1.0f < o[0]) o[0] = d + 1.0f;
                }
    }
    for (uint32_t v = 0; v < t->nv; v++) {
        float* o = &t->aux[2 * v];
        o[0] = o[0] > 0.0f ? fminf(o[0] - 1.0f, cell * 0.01f) : (t->cells[v] && t->cells[v] != 0xFF ? cell * 0.01f : 0.0f);
    }
    t->rest_volume = (float)t->filled * (cell * 0.01f) * (cell * 0.01f) * (cell * 0.01f);
    free(mark);
    { // the padding's vertex layers (only empty cells around them) dropped: a cushion's 13 x 13 x 6 is 11 x 11 x 4, all of it
        // active; its colors, places and the mesh's coordinates moved by one
        uint32_t ox = t->nx - 2, oy = t->ny - 2, oz = t->nz - 2, on = ox * oy * oz;
        for (uint32_t z = 0; z < oz; z++)
            for (uint32_t y = 0; y < oy; y++)
                for (uint32_t x = 0; x < ox; x++) {
                    uint32_t d = (z * oy + y) * ox + x, s = VIDX(x + 1, y + 1, z + 1); // (d <= s: in place, in order)
                    t->cells[d] = t->cells[s], t->aux[2 * d] = t->aux[2 * s], t->aux[2 * d + 1] = t->aux[2 * s + 1];
                    memmove(&t->rest[4 * d], &t->rest[4 * s], 16);
                    t->color[d] = (uint8_t)((x & 1u) + 2u * (y & 1u) + 4u * (z & 1u));
                }
        for (uint32_t i = 0; i < npos; i++) t->embed[4 * i] -= 1.0f, t->embed[4 * i + 1] -= 1.0f, t->embed[4 * i + 2] -= 1.0f;
        for (int k = 0; k < 3; k++) t->origin[k] += cell;
        t->nx = ox, t->ny = oy, t->nz = oz, t->nv = on;
    }
#undef CIDX
#undef VIDX
    t->build_ms = (re_time_seconds() - t0) * 1000.0;
    return true;
}

bool soft_topo_build(SoftTopo* t, uint32_t mesh, const float scale[3], const CCloth* c) {
    const GpuVertex* V = NULL;
    const uint32_t* I = NULL;
    uint32_t fi = 0, ic = 0;
    int32_t vo = 0;
    soft_topo_key(t->key, mesh, scale, c);
    render_mesh_cpu(mesh, &V, &I, &fi, &ic, &vo);
    if (!V || !I || ic < 3) return false;
    uint32_t hi = 0;
    for (uint32_t k = 0; k < ic; k++) hi = I[fi + k] > hi ? I[fi + k] : hi;
    uint32_t nv = hi + 1u;
    float* pos = malloc(sizeof(float) * 3 * nv);
    uint32_t* idx = malloc(sizeof(uint32_t) * ic);
    uint32_t* col = NULL;
    bool ok = false;
    if (pos && idx) {
        for (uint32_t i = 0; i < nv; i++)
            for (int k = 0; k < 3; k++) pos[3 * i + k] = V[(int64_t)vo + i].pos[k] * scale[k];
        for (uint32_t k = 0; k < ic; k++) idx[k] = I[fi + k];
        if (c->attach == SOFT_ATTACH_PAINTED && (mesh & 0x80000000u)) { // (the vertex colors: from the mesh's file, its LOD 0 first)
            const char* vp = render_mesh_vpath(render_mesh_slot(mesh));
            char path[1100];
            MeshBuilt mb;
            memset(&mb, 0, sizeof mb);
            if (vp && !strncmp(vp, "/Content", 8)) {
                snprintf(path, sizeof path, "%s%s", render_content_root(), vp + 8);
                if (rmesh_read(path, &mb, 1u << 24, 1u << 26) && mb.col && mb.nv >= nv && (col = malloc(4u * nv)) != NULL) memcpy(col, mb.col, 4u * nv);
                mesh_built_free(&mb);
            }
            if (!col) printf("[RealEngine] Soft body: %s has no vertex colors: nothing held (Painted)\n", vp ? vp : "?");
        }
        ok = soft_topo_build_arrays(t, pos, nv, idx, ic, col, c);
    }
    free(pos), free(idx), free(col);
    soft_topo_key(t->key, mesh, scale, c);
    return ok;
}

double soft_volume(const SoftTopo* t, const float* p) {
    static const double sgn[6] = { 1, -1, -1, 1, 1, -1 }; // (half of Kuhn's tetrahedra are wound the other way: their rest's sign)
    double vol = 0.0;
    for (uint32_t z = 0; z + 1 < t->nz; z++)
        for (uint32_t y = 0; y + 1 < t->ny; y++)
            for (uint32_t x = 0; x + 1 < t->nx; x++) {
                uint32_t v0 = (z * t->ny + y) * t->nx + x;
                if (!(t->cells[v0] & 0x80u)) continue; // (its cell (x, y, z) is bit 7 of its corner 0: (i - 1 + 1, ...))
                uint32_t cv[8];
                for (uint32_t b = 0; b < 8; b++) cv[b] = ((z + (b >> 2)) * t->ny + (y + ((b >> 1) & 1u))) * t->nx + x + (b & 1u);
                for (int k = 0; k < 6; k++) {
                    const float *a = &p[4 * cv[g_soft_tets[k][0]]], *b = &p[4 * cv[g_soft_tets[k][1]]], *c = &p[4 * cv[g_soft_tets[k][2]]], *d = &p[4 * cv[g_soft_tets[k][3]]];
                    double e1[3], e2[3], e3[3];
                    for (int j = 0; j < 3; j++) e1[j] = b[j] - a[j], e2[j] = c[j] - a[j], e3[j] = d[j] - a[j];
                    vol += sgn[k] * (e1[0] * (e2[1] * e3[2] - e2[2] * e3[1]) - e1[1] * (e2[0] * e3[2] - e2[2] * e3[0]) + e1[2] * (e2[0] * e3[1] - e2[1] * e3[0])) / 6.0;
                }
            }
    return vol;
}

int soft_selftest(void) {
    // a sphere of 50 cm, 24 x 12 segments: its lattice at 10 cells across
    enum { SEG = 24, RING = 12 };
    float pos[(SEG + 1) * (RING + 1) * 3];
    uint32_t idx[SEG * RING * 6], ni = 0;
    for (int r = 0; r <= RING; r++)
        for (int s = 0; s <= SEG; s++) {
            float th = 3.14159265f * (float)r / RING, ph = 6.2831853f * (float)s / SEG;
            float* p = &pos[3 * (r * (SEG + 1) + s)];
            p[0] = 50.0f * sinf(th) * cosf(ph), p[1] = 50.0f * sinf(th) * sinf(ph), p[2] = 50.0f * cosf(th);
        }
    for (int r = 0; r < RING; r++)
        for (int s = 0; s < SEG; s++) {
            uint32_t a = (uint32_t)(r * (SEG + 1) + s), b = a + 1, c = a + SEG + 1, d = c + 1;
            idx[ni++] = a, idx[ni++] = c, idx[ni++] = b, idx[ni++] = b, idx[ni++] = c, idx[ni++] = d;
        }
    CCloth cc;
    memset(&cc, 0, sizeof cc);
    cc.soft_res = 10, cc.attach = SOFT_ATTACH_BOTTOM, cc.pin_band = 1.0f, cc.pin_axis[2] = 1.0f;
    SoftTopo t;
    memset(&t, 0, sizeof t);
    int fails = 0;
    if (!soft_topo_build_arrays(&t, pos, (SEG + 1) * (RING + 1), idx, ni, NULL, &cc)) return printf("[RealEngine] FAIL: soft body: no lattice\n"), 1;
    double sphere = 4.0 / 3.0 * 3.14159265 * 0.5 * 0.5 * 0.5, vol = 0.0;
    float* p4 = malloc(16u * t.nv);
    if (p4) {
        for (uint32_t v = 0; v < t.nv; v++) memcpy(&p4[4 * v], &t.rest[4 * v], 12), p4[4 * v] *= 0.01f, p4[4 * v + 1] *= 0.01f, p4[4 * v + 2] *= 0.01f;
        vol = soft_volume(&t, p4);
        free(p4);
    }
    // two vertices of one tetrahedron never share a color; every render vertex inside a filled cell
    uint32_t same = 0, outside = 0, hollow = 0;
    for (uint32_t z = 0; z + 1 < t.nz; z++)
        for (uint32_t y = 0; y + 1 < t.ny; y++)
            for (uint32_t x = 0; x + 1 < t.nx; x++) {
                uint32_t v0 = (z * t.ny + y) * t.nx + x;
                if (!(t.cells[v0] & 0x80u)) continue;
                for (uint32_t b = 1; b < 8; b++) {
                    uint32_t v = ((z + (b >> 2)) * t.ny + (y + ((b >> 1) & 1u))) * t.nx + x + (b & 1u);
                    same += t.color[v] == t.color[v0];
                }
            }
    for (uint32_t i = 0; i < t.nrender; i++) {
        uint32_t c0[3] = { (uint32_t)t.embed[4 * i], (uint32_t)t.embed[4 * i + 1], (uint32_t)t.embed[4 * i + 2] };
        if (t.embed[4 * i] == (float)c0[0] && c0[0]) c0[0]--; // (on the cell's far face: the cell below is its)
        if (t.embed[4 * i + 1] == (float)c0[1] && c0[1]) c0[1]--;
        if (t.embed[4 * i + 2] == (float)c0[2] && c0[2]) c0[2]--;
        bool in = false;
        for (uint32_t b = 0; b < 8 && !in; b++) { // (any corner of a filled cell holding the vertex)
            uint32_t X = c0[0] + (b & 1u), Y = c0[1] + ((b >> 1) & 1u), Z = c0[2] + (b >> 2);
            if (X < t.nx && Y < t.ny && Z < t.nz && t.cells[(Z * t.ny + Y) * t.nx + X]) in = true;
        }
        outside += !in;
    }
    // the middle is filled (the flood stopped at the surface)
    uint32_t mid = ((t.nz / 2) * t.ny + t.ny / 2) * t.nx + t.nx / 2;
    hollow = t.cells[mid] != 0xFF;
    printf("[RealEngine] Soft body lattice: %ux%ux%u, %u active of %u, %u cells filled, cell %.1f cm, %u pinned, volume %.4f m3 (sphere %.4f, cells %.4f), "
           "%.2f ms\n", t.nx, t.ny, t.nz, t.nactive, t.nv, t.filled, t.cell, t.pinned, vol, sphere, t.rest_volume, t.build_ms);
    if (same) printf("[RealEngine] FAIL: soft body: %u tetrahedron edges inside one color\n", same), fails++;
    if (outside) printf("[RealEngine] FAIL: soft body: %u mesh vertices outside the lattice\n", outside), fails++;
    if (hollow) printf("[RealEngine] FAIL: soft body: the middle is empty (the flood went in)\n"), fails++;
    if (fabs(vol - t.rest_volume) > 1e-3 * t.rest_volume) printf("[RealEngine] FAIL: soft body: the tetrahedra's volume %.5f != the cells' %.5f\n", vol, t.rest_volume), fails++;
    if (vol < sphere || vol > sphere * 1.9) printf("[RealEngine] FAIL: soft body: the filled volume %.4f against the sphere's %.4f\n", vol, sphere), fails++;
    if (!t.pinned || t.pinned > t.nactive / 4) printf("[RealEngine] FAIL: soft body: the bottom band held %u of %u\n", t.pinned, t.nactive), fails++;
    soft_topo_free(&t);
    return fails;
}
