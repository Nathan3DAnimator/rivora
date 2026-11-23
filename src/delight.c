#include "delight.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mesh_build.h"
#include "platform.h"
#include "vkctx.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)

static float s2l(uint8_t c) { float v = c / 255.0f; return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
static uint8_t l2s(float v) {
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}
static void sh9(const float n[3], float y[9]) { // (the real SH basis, bands 0-2, unnormalized constants: the fit absorbs them)
    float x = n[0], yy = n[1], z = n[2];
    y[0] = 1.0f, y[1] = yy, y[2] = z, y[3] = x, y[4] = x * yy, y[5] = yy * z, y[6] = 3.0f * z * z - 1.0f, y[7] = x * z, y[8] = x * x - yy * yy;
}
// solves A x = b (9 x 9, symmetric positive: Gaussian elimination with partial pivoting)
static bool solve9(double A[9][9], double b[9], double x[9]) {
    for (int c = 0; c < 9; c++) {
        int p = c;
        for (int r = c + 1; r < 9; r++)
            if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        if (fabs(A[p][c]) < 1e-12) return false;
        if (p != c) {
            for (int k = 0; k < 9; k++) { double t = A[c][k]; A[c][k] = A[p][k], A[p][k] = t; }
            double t = b[c]; b[c] = b[p], b[p] = t;
        }
        for (int r = c + 1; r < 9; r++) {
            double f = A[r][c] / A[c][c];
            for (int k = c; k < 9; k++) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 8; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < 9; k++) s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return true;
}
// a box blur of v (masked: the covered texels only), radius r, run 3 times (~ a Gaussian)
static void blur(float* v, const uint8_t* mask, uint32_t w, uint32_t h, int r) {
    float* tmp = malloc(sizeof(float) * w * h);
    float* wt = malloc(sizeof(float) * w * h);
    float* tw = malloc(sizeof(float) * w * h);
    if (!tmp || !wt || !tw) { free(tmp), free(wt), free(tw); return; }
    for (uint32_t i = 0; i < w * h; i++) wt[i] = mask ? (mask[i] ? 1.0f : 0.0f) : 1.0f, v[i] *= wt[i];
    for (int pass = 0; pass < 3; pass++)
        for (int axis = 0; axis < 2; axis++) {
            uint32_t n = axis ? h : w, lines = axis ? w : h;
            for (uint32_t L = 0; L < lines; L++) {
                double s = 0.0, sw = 0.0;
                for (int k = -r; k <= r; k++) {
                    int q = k < 0 ? 0 : k >= (int)n ? (int)n - 1 : k;
                    uint32_t at = axis ? (uint32_t)q * w + L : L * w + (uint32_t)q;
                    s += v[at], sw += wt[at];
                }
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t at = axis ? i * w + L : L * w + i;
                    tmp[at] = (float)s, tw[at] = (float)sw;
                    int add = (int)i + r + 1, sub = (int)i - r;
                    add = add >= (int)n ? (int)n - 1 : add, sub = sub < 0 ? 0 : sub;
                    uint32_t aa = axis ? (uint32_t)add * w + L : L * w + (uint32_t)add, ss = axis ? (uint32_t)sub * w + L : L * w + (uint32_t)sub;
                    s += v[aa] - v[ss], sw += wt[aa] - wt[ss];
                }
            }
            memcpy(v, tmp, sizeof(float) * w * h), memcpy(wt, tw, sizeof(float) * w * h);
        }
    for (uint32_t i = 0; i < w * h; i++) v[i] = wt[i] > 1e-6f ? v[i] / wt[i] : 0.0f;
    free(tmp), free(wt), free(tw);
}
static float spread(const float* logy, const uint8_t* mask, uint32_t n) {
    double s = 0.0, s2 = 0.0, c = 0.0;
    for (uint32_t i = 0; i < n; i++)
        if (!mask || mask[i]) s += logy[i], s2 += (double)logy[i] * logy[i], c += 1.0;
    if (c < 1.0) return 0.0f;
    double m = s / c;
    return (float)sqrt(fmax(s2 / c - m * m, 0.0));
}

void delight_rgba(uint8_t* rgba, uint32_t w, uint32_t h, const float* nrm, const uint8_t* mask, float strength, DelightStats* st) {
    double t0 = re_time_seconds();
    uint32_t n = w * h;
    strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
    float* lin = malloc(sizeof(float) * 3 * n);
    float* y = malloc(sizeof(float) * n);
    float* ly = malloc(sizeof(float) * n);
    if (!lin || !y || !ly) { free(lin), free(y), free(ly); return; }
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) lin[3 * i + k] = s2l(rgba[4 * i + k]);
        y[i] = 0.2126f * lin[3 * i] + 0.7152f * lin[3 * i + 1] + 0.0722f * lin[3 * i + 2];
        ly[i] = logf(y[i] + 1e-3f);
    }
    if (st) st->before = spread(ly, mask, n), st->fit_r2 = 0.0f;
    // 1. the lighting: Y ~ sum c_k SH_k(n) over the covered texels (least squares), divided out (normalized to its mean)
    if (nrm) {
        double A[9][9] = { { 0 } }, b[9] = { 0 }, c[9] = { 0 }, sy = 0.0, syy = 0.0, cnt = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            if (mask && !mask[i]) continue;
            float s[9];
            sh9(&nrm[3 * i], s);
            for (int r = 0; r < 9; r++) {
                for (int k = 0; k < 9; k++) A[r][k] += (double)s[r] * s[k];
                b[r] += (double)s[r] * y[i];
            }
            sy += y[i], syy += (double)y[i] * y[i], cnt += 1.0;
        }
        for (int r = 0; r < 9; r++) A[r][r] += 1e-6 * (cnt + 1.0); // (a little ridge: a mesh seen from one side)
        if (cnt > 16.0 && solve9(A, b, c)) {
            double mean = 0.0, res = 0.0, tot = syy - sy * sy / cnt;
            float* S = malloc(sizeof(float) * n);
            if (S) {
                for (uint32_t i = 0; i < n; i++) {
                    float s[9];
                    sh9(&nrm[3 * i], s);
                    double v = 0.0;
                    for (int k = 0; k < 9; k++) v += c[k] * s[k];
                    S[i] = (float)v;
                    if (!mask || mask[i]) mean += v, res += (y[i] - v) * (y[i] - v);
                }
                mean /= cnt;
                if (st) st->fit_r2 = tot > 1e-12 ? (float)fmax(0.0, 1.0 - res / tot) : 0.0f;
                for (uint32_t i = 0; i < n; i++) {
                    if (mask && !mask[i]) continue;
                    float f = powf(fmaxf(S[i], (float)mean * 0.08f) / (float)mean, strength);
                    for (int k = 0; k < 3; k++) lin[3 * i + k] /= f;
                }
                free(S);
            }
        }
    }
    // 2. what is left of the broad shading (soft shadows, occlusion): the log brightness blurred wide, half of it out
    for (uint32_t i = 0; i < n; i++) ly[i] = logf(0.2126f * lin[3 * i] + 0.7152f * lin[3 * i + 1] + 0.0722f * lin[3 * i + 2] + 1e-3f);
    double m = 0.0, c = 0.0;
    for (uint32_t i = 0; i < n; i++)
        if (!mask || mask[i]) m += ly[i], c += 1.0;
    m = c > 0.0 ? m / c : 0.0;
    memcpy(y, ly, sizeof(float) * n);
    int r = (int)((w > h ? w : h) / 8u); // (wide: the albedo's own patches stay, the day's broad shading goes)
    blur(y, mask, w, h, r < 2 ? 2 : r);
    for (uint32_t i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        float f = expf((y[i] - (float)m) * 0.5f * strength);
        for (int k = 0; k < 3; k++) {
            lin[3 * i + k] /= f;
            rgba[4 * i + k] = l2s(lin[3 * i + k]);
        }
        ly[i] = logf(0.2126f * lin[3 * i] + 0.7152f * lin[3 * i + 1] + 0.0722f * lin[3 * i + 2] + 1e-3f);
    }
    if (st) st->after = spread(ly, mask, n), st->ms = (re_time_seconds() - t0) * 1000.0;
    free(lin), free(y), free(ly);
}

static void oct_dec(uint32_t o, float n[3]) {
    float x = (float)(int16_t)(o & 0xFFFFu) / 32767.0f, y = (float)(int16_t)(o >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}
// the mesh's normals drawn into the texture's UV space (its LOD 0's triangles; UVs wrapped into 0..1)
static void raster_normals(const MeshBuilt* mb, uint32_t w, uint32_t h, float* nrm, uint8_t* mask) {
    uint32_t ni = mb->lods.count ? mb->lods.index_count[0] : mb->ni, first = mb->lods.count ? mb->lods.first_index[0] : 0;
    for (uint32_t t = 0; t + 2 < ni; t += 3) {
        const GpuVertex* v[3] = { &mb->v[mb->idx[first + t]], &mb->v[mb->idx[first + t + 1]], &mb->v[mb->idx[first + t + 2]] };
        float px[3], py[3], nn[3][3];
        for (int k = 0; k < 3; k++) {
            float u = v[k]->uv[0] - floorf(v[0]->uv[0]), vv = v[k]->uv[1] - floorf(v[0]->uv[1]); // (the triangle's own tile)
            px[k] = u * (float)w, py[k] = vv * (float)h;
            oct_dec(v[k]->normal_oct, nn[k]);
        }
        float x0 = fminf(px[0], fminf(px[1], px[2])), x1 = fmaxf(px[0], fmaxf(px[1], px[2]));
        float y0 = fminf(py[0], fminf(py[1], py[2])), y1 = fmaxf(py[0], fmaxf(py[1], py[2]));
        float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (fabsf(area) < 1e-9f) continue;
        for (int yy = (int)floorf(y0); yy <= (int)ceilf(y1); yy++)
            for (int xx = (int)floorf(x0); xx <= (int)ceilf(x1); xx++) {
                float cx = xx + 0.5f, cy = yy + 0.5f;
                float b0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                float b1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                float b2 = 1.0f - b0 - b1;
                if (b0 < -0.01f || b1 < -0.01f || b2 < -0.01f) continue;
                uint32_t tx = (uint32_t)(((xx % (int)w) + (int)w) % (int)w), ty = (uint32_t)(((yy % (int)h) + (int)h) % (int)h);
                uint32_t at = ty * w + tx;
                float q[3];
                for (int k = 0; k < 3; k++) q[k] = nn[0][k] * b0 + nn[1][k] * b1 + nn[2][k] * b2;
                float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (l < 1e-6f) continue;
                for (int k = 0; k < 3; k++) nrm[3 * at + k] = q[k] / l;
                mask[at] = 1;
            }
    }
}

bool delight_file(const char* image, const char* rmesh, const char* out_png, float strength, DelightStats* st) {
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(image, &w, &h);
    if (!px) return false;
    float* nrm = NULL;
    uint8_t* mask = NULL;
    if (rmesh && rmesh[0]) {
        MeshBuilt mb;
        memset(&mb, 0, sizeof mb);
        if (rmesh_read(rmesh, &mb, 16u << 20, 64u << 20)) {
            nrm = calloc((size_t)w * h * 3u, sizeof(float)), mask = calloc((size_t)w * h, 1);
            if (nrm && mask) raster_normals(&mb, w, h, nrm, mask);
            mesh_built_free(&mb);
            uint32_t covered = 0;
            for (uint32_t i = 0; mask && i < w * h; i++) covered += mask[i];
            if (covered < 64) free(nrm), free(mask), nrm = NULL, mask = NULL; // (its UVs don't cover the texture)
        }
    }
    delight_rgba(px, w, h, nrm, mask, strength, st);
    uint8_t* rgb = malloc((size_t)w * h * 3u);
    bool ok = rgb != NULL;
    for (uint32_t i = 0; ok && i < w * h; i++) rgb[3 * i] = px[4 * i], rgb[3 * i + 1] = px[4 * i + 1], rgb[3 * i + 2] = px[4 * i + 2];
    ok = ok && vkctx_png_write_rgb(out_png, rgb, w, h);
    free(rgb), free(px), free(nrm), free(mask);
    return ok;
}

int delight_selftest(void) {
    // a sphere's lat-long texture: a checker of two colors, lit by a sun from one side and the sky (0.25 + 0.75 n.l),
    // with a soft dark band (an occlusion); de-lit with its normals
    const uint32_t W = 256, H = 128;
    uint8_t* px = malloc(W * H * 4);
    float* nrm = malloc(sizeof(float) * W * H * 3);
    uint8_t* mask = malloc(W * H);
    float* alb = malloc(sizeof(float) * W * H);
    if (!px || !nrm || !mask || !alb) return 1;
    const float L[3] = { 0.57f, 0.33f, 0.75f };
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t i = y * W + x;
            float lon = ((float)x + 0.5f) / W * 6.2831853f, lat = ((float)y + 0.5f) / H * 3.1415927f;
            float n[3] = { sinf(lat) * cosf(lon), sinf(lat) * sinf(lon), cosf(lat) };
            memcpy(&nrm[3 * i], n, 12), mask[i] = 1;
            bool chk = ((x / 32) + (y / 32)) & 1;
            float a = chk ? 0.6f : 0.25f;
            alb[i] = a;
            float sh = 0.25f + 0.75f * fmaxf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0.0f);
            for (int k = 0; k < 3; k++) px[4 * i + k] = l2s(a * sh * (k == 0 ? 1.0f : k == 1 ? 0.9f : 0.8f)), px[4 * i + 3] = 255;
        }
    // the light's share before and after: the spread of log(brightness / albedo) over the sphere
    float before = 0.0f, after = 0.0f;
    for (int pass = 0; pass < 2; pass++) {
        double s = 0.0, s2 = 0.0;
        for (uint32_t i = 0; i < W * H; i++) {
            float yv = 0.2126f * s2l(px[4 * i]) + 0.7152f * s2l(px[4 * i + 1]) + 0.0722f * s2l(px[4 * i + 2]);
            double v = log(yv / alb[i] + 1e-4);
            s += v, s2 += v * v;
        }
        double m = s / (W * H);
        float sd = (float)sqrt(fmax(s2 / (W * H) - m * m, 0.0));
        if (pass == 0) before = sd, delight_rgba(px, W, H, nrm, mask, 1.0f, NULL);
        else after = sd;
    }
    DelightStats st = { 0 };
    printf("[RealEngine] De-light: the light's spread %.3f -> %.3f (log units)\n", before, after);
    (void)st;
    free(px), free(nrm), free(mask), free(alb);
    if (after > before * 0.35f) {
        printf("[RealEngine] FAIL: de-light: the baked light mostly stays\n");
        return 1;
    }
    return 0;
}
