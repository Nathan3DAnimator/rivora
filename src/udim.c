// (Q18) UDIM textures: names, the .rudim asset (import, reading), the BC7 / BC5 codecs on the CPU. udim.h has the
// format; notas-dev/udim.md the design and the measures.
#include "udim.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "platform_files.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _stricmp strcasecmp
#include <strings.h>
#endif

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

// ============================================================================================
// Names
// ============================================================================================

static const char* base_name(const char* p) {
    const char* b = p + strlen(p);
    while (b > p && b[-1] != '/' && b[-1] != '\\') b--;
    return b;
}

bool udim_is_path(const char* path) {
    if (!path || !path[0]) return false;
    size_t n = strlen(path);
    if (n > 6 && !_stricmp(path + n - 6, ".rudim")) return true;
    return strstr(path, "<UDIM>") || strstr(path, "<UVTILE>");
}

bool udim_pattern_of(const char* path, char* pattern, size_t cap, int* udim) {
    if (!path || !path[0]) return false;
    const char* name = base_name(path);
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    size_t pre = (size_t)(name - path);
    // <base>.1001.ext, <base>_1001.ext, <base>-1001.ext
    const char* d = dot - 4;
    if (d > name && strchr("._-", d[-1]) && isdigit((unsigned char)d[0]) && isdigit((unsigned char)d[1]) &&
        isdigit((unsigned char)d[2]) && isdigit((unsigned char)d[3])) {
        int v = atoi(d);
        if (v >= UDIM_FIRST && v < UDIM_FIRST + UDIM_MAX_TILES) {
            if (pattern) snprintf(pattern, cap, "%.*s<UDIM>%s", (int)(pre + (size_t)(d - name)), path, dot);
            if (udim) *udim = v;
            return true;
        }
    }
    // Mudbox / ZBrush: <base>_u1_v1.ext (u and v from 1)
    const char* u = NULL;
    for (const char* q = name; q + 4 < dot; q++)
        if ((q[0] == '_' || q[0] == '.') && (q[1] == 'u' || q[1] == 'U') && isdigit((unsigned char)q[2])) u = q;
    if (u) {
        int uu = 0, vv = 0, used = 0;
        if (sscanf(u + 2, "%d_%*[vV]%d%n", &uu, &vv, &used) == 2 && u + 2 + used == dot && uu >= 1 && uu <= UDIM_COLS && vv >= 1 && vv <= UDIM_ROWS) {
            if (pattern) snprintf(pattern, cap, "%.*s<UVTILE>%s", (int)(pre + (size_t)(u + 1 - name)), path, dot);
            if (udim) *udim = UDIM_FIRST + (uu - 1) + (vv - 1) * UDIM_COLS;
            return true;
        }
    }
    return false;
}

void udim_tile_path(const char* pattern, int udim, char* out, size_t cap) {
    const char* t = strstr(pattern, "<UDIM>");
    if (t) {
        snprintf(out, cap, "%.*s%04d%s", (int)(t - pattern), pattern, udim, t + 6);
        return;
    }
    t = strstr(pattern, "<UVTILE>");
    if (t) {
        int x = 0, y = 0;
        udim_tile_xy(udim, &x, &y);
        snprintf(out, cap, "%.*su%d_v%d%s", (int)(t - pattern), pattern, x + 1, y + 1, t + 8);
        return;
    }
    snprintf(out, cap, "%s", pattern);
}

int udim_find_tiles(const char* pattern, int* udims, int cap) {
    int n = 0;
    char p[1024];
    for (int u = UDIM_FIRST; u < UDIM_FIRST + UDIM_MAX_TILES && n < cap; u++) {
        udim_tile_path(pattern, u, p, sizeof p);
        bool dir = false;
        if (re_file_stat(p, NULL, NULL, &dir) && !dir) udims[n++] = u;
    }
    return n;
}

bool udim_detect(const char* path, char* pattern, size_t cap) {
    char pat[1024];
    if (!path || !path[0]) return false;
    if (strstr(path, "<UDIM>") || strstr(path, "<UVTILE>")) snprintf(pat, sizeof pat, "%s", path);
    else if (!udim_pattern_of(path, pat, sizeof pat, NULL)) return false;
    int t[UDIM_MAX_TILES];
    int n = udim_find_tiles(pat, t, UDIM_MAX_TILES);
    // one tile alone is a plain image (a "rock.1024.png" is its size, not a UDIM), unless the name has the token
    bool token = strstr(path, "<UDIM>") || strstr(path, "<UVTILE>");
    if (n < (token ? 1 : 2)) return false;
    if (pattern) snprintf(pattern, cap, "%s", pat);
    return true;
}

bool udim_name_is_normal(const char* path) {
    char low[256];
    const char* b = base_name(path);
    size_t n = 0;
    for (; b[n] && n + 1 < sizeof low; n++) low[n] = (char)tolower((unsigned char)b[n]);
    low[n] = 0;
    return strstr(low, "normal") || strstr(low, "nrm") || strstr(low, "_nor") || strstr(low, "_n.") || strstr(low, "_n_") ||
           strstr(low, "-n.") || strstr(low, "_nml");
}

void udim_asset_name(const char* src_pattern, const char* folder, char* out, size_t cap) {
    const char* b = base_name(src_pattern);
    const char* t = strstr(b, "<UDIM>");
    if (!t) t = strstr(b, "<UVTILE>");
    if (!t) t = strrchr(b, '.');
    if (!t) t = b + strlen(b);
    int n = (int)(t - b);
    while (n > 0 && strchr("._-", b[n - 1])) n--; // ("skin.<UDIM>.png" -> "skin")
    snprintf(out, cap, "%s/%.*s.rudim", folder, n > 0 ? n : 4, n > 0 ? b : "udim");
}

// ============================================================================================
// BC7 mode 6 (bc7.comp's encoder on the CPU: the endpoints along the principal axis, a least squares refit, p-bits by
// their error) and its decoder
// ============================================================================================

static const float W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static void bc7_quantize(const float e[4], uint32_t q[4], uint32_t* p) {
    float best = 1e30f;
    for (uint32_t pb = 0; pb < 2; pb++) {
        uint32_t qq[4];
        float err = 0.0f;
        for (int c = 0; c < 4; c++) {
            qq[c] = (uint32_t)clampf(roundf((e[c] - (float)pb) * 0.5f), 0.0f, 127.0f);
            float r = (float)(qq[c] * 2 + pb) - e[c];
            err += r * r;
        }
        if (err < best) best = err, memcpy(q, qq, sizeof qq), *p = pb;
    }
}

static float bc7_indices(const float c[16][4], const float a[4], const float b[4], uint32_t idx[16]) {
    float d[4], dd = 0.0f, err = 0.0f;
    for (int k = 0; k < 4; k++) d[k] = b[k] - a[k], dd += d[k] * d[k];
    if (dd < 1e-6f) dd = 1e-6f;
    for (int i = 0; i < 16; i++) {
        float t = 0.0f;
        for (int k = 0; k < 4; k++) t += (c[i][k] - a[k]) * d[k];
        t = clampf(t / dd, 0.0f, 1.0f) * 64.0f;
        int k = (int)clampf(roundf(t * 15.0f / 64.0f), 0.0f, 15.0f);
        if (k > 0 && fabsf(W4[k - 1] - t) < fabsf(W4[k] - t)) k--;
        if (k < 15 && fabsf(W4[k + 1] - t) < fabsf(W4[k] - t)) k++;
        idx[i] = (uint32_t)k;
        for (int ch = 0; ch < 4; ch++) {
            float r = floorf(((64.0f - W4[k]) * a[ch] + W4[k] * b[ch] + 32.0f) / 64.0f) - c[i][ch];
            err += r * r;
        }
    }
    return err;
}

static void put_bits(uint8_t o[16], uint32_t* pos, uint32_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, (*pos)++)
        if ((v >> i) & 1u) o[*pos >> 3] |= (uint8_t)(1u << (*pos & 7u));
}
static uint32_t get_bits(const uint8_t in[16], uint32_t* pos, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++, (*pos)++) v |= (uint32_t)((in[*pos >> 3] >> (*pos & 7u)) & 1u) << i;
    return v;
}

void udim_bc7_encode_block(const uint8_t rgba[64], uint8_t out[16]) {
    float c[16][4], mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 }, mean[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) {
            c[i][k] = (float)rgba[i * 4 + k];
            if (c[i][k] < mn[k]) mn[k] = c[i][k];
            if (c[i][k] > mx[k]) mx[k] = c[i][k];
            mean[k] += c[i][k] * (1.0f / 16.0f);
        }
    float cov[4][4] = { { 0 } };
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) cov[a][b] += (c[i][a] - mean[a]) * (c[i][b] - mean[b]);
    float axis[4];
    for (int k = 0; k < 4; k++) axis[k] = mx[k] - mn[k];
    for (int it = 0; it < 8; it++) {
        float a2[4] = { 0, 0, 0, 0 }, l = 0.0f;
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) a2[a] += cov[a][b] * axis[b];
            l += a2[a] * a2[a];
        }
        l = sqrtf(l);
        if (l < 1e-6f) break;
        for (int a = 0; a < 4; a++) axis[a] = a2[a] / l;
    }
    float al = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] + axis[3] * axis[3]);
    for (int k = 0; k < 4; k++) axis[k] = al > 1e-6f ? axis[k] / al : 0.0f;
    float t0 = 1e30f, t1 = -1e30f;
    for (int i = 0; i < 16; i++) {
        float t = 0.0f;
        for (int k = 0; k < 4; k++) t += (c[i][k] - mean[k]) * axis[k];
        if (t < t0) t0 = t;
        if (t > t1) t1 = t;
    }
    float e0[4], e1[4];
    for (int k = 0; k < 4; k++) {
        e0[k] = al <= 1e-6f ? mean[k] : clampf(mean[k] + axis[k] * t0, 0.0f, 255.0f);
        e1[k] = al <= 1e-6f ? mean[k] : clampf(mean[k] + axis[k] * t1, 0.0f, 255.0f);
    }
    uint32_t q0[4], q1[4], p0 = 0, p1 = 0, idx[16];
    bc7_quantize(e0, q0, &p0), bc7_quantize(e1, q1, &p1);
    float a[4], b[4];
    for (int k = 0; k < 4; k++) a[k] = (float)(q0[k] * 2 + p0), b[k] = (float)(q1[k] * 2 + p1);
    float err = bc7_indices(c, a, b, idx);
    { // the refit
        float A = 0, B = 0, C = 0, X[4] = { 0 }, Y[4] = { 0 };
        for (int i = 0; i < 16; i++) {
            float w = W4[idx[i]] / 64.0f, v = 1.0f - w;
            A += v * v, B += v * w, C += w * w;
            for (int k = 0; k < 4; k++) X[k] += v * c[i][k], Y[k] += w * c[i][k];
        }
        float det = A * C - B * B;
        if (fabsf(det) > 1e-4f) {
            float f0[4], f1[4];
            for (int k = 0; k < 4; k++) f0[k] = clampf((C * X[k] - B * Y[k]) / det, 0, 255), f1[k] = clampf((A * Y[k] - B * X[k]) / det, 0, 255);
            uint32_t r0[4], r1[4], s0 = 0, s1 = 0, idx2[16];
            bc7_quantize(f0, r0, &s0), bc7_quantize(f1, r1, &s1);
            float a2[4], b2[4];
            for (int k = 0; k < 4; k++) a2[k] = (float)(r0[k] * 2 + s0), b2[k] = (float)(r1[k] * 2 + s1);
            if (bc7_indices(c, a2, b2, idx2) < err) {
                memcpy(q0, r0, sizeof q0), memcpy(q1, r1, sizeof q1), p0 = s0, p1 = s1;
                memcpy(idx, idx2, sizeof idx);
            }
        }
    }
    if (idx[0] >= 8) { // the anchor's top bit 0
        uint32_t t[4];
        memcpy(t, q0, sizeof t), memcpy(q0, q1, sizeof t), memcpy(q1, t, sizeof t);
        uint32_t tp = p0;
        p0 = p1, p1 = tp;
        for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
    }
    memset(out, 0, 16);
    uint32_t pos = 0;
    put_bits(out, &pos, 1u << 6, 7);
    for (int ch = 0; ch < 4; ch++) put_bits(out, &pos, q0[ch], 7), put_bits(out, &pos, q1[ch], 7);
    put_bits(out, &pos, p0, 1), put_bits(out, &pos, p1, 1);
    put_bits(out, &pos, idx[0], 3);
    for (int i = 1; i < 16; i++) put_bits(out, &pos, idx[i], 4);
}

void udim_bc7_decode_block(const uint8_t in[16], uint8_t rgba[64]) {
    uint32_t pos = 0;
    if (get_bits(in, &pos, 7) != (1u << 6)) { // (only mode 6 is written here)
        for (int i = 0; i < 16; i++) rgba[i * 4] = 255, rgba[i * 4 + 1] = 0, rgba[i * 4 + 2] = 255, rgba[i * 4 + 3] = 255;
        return;
    }
    uint32_t e[2][4];
    for (int ch = 0; ch < 4; ch++) e[0][ch] = get_bits(in, &pos, 7), e[1][ch] = get_bits(in, &pos, 7);
    uint32_t p0 = get_bits(in, &pos, 1), p1 = get_bits(in, &pos, 1);
    for (int ch = 0; ch < 4; ch++) e[0][ch] = e[0][ch] << 1 | p0, e[1][ch] = e[1][ch] << 1 | p1;
    for (int i = 0; i < 16; i++) {
        uint32_t k = get_bits(in, &pos, i ? 4 : 3), w = (uint32_t)W4[k];
        for (int ch = 0; ch < 4; ch++) rgba[i * 4 + ch] = (uint8_t)(((64 - w) * e[0][ch] + w * e[1][ch] + 32) >> 6);
    }
}

// BC4 (one channel of BC5): the block's least and most as the endpoints, the 8-value mode
static void bc4_encode(const uint8_t* v, int stride, uint8_t out[8]) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        int x = v[i * stride];
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    out[0] = (uint8_t)hi, out[1] = (uint8_t)lo;
    float pal[8];
    pal[0] = (float)hi, pal[1] = (float)lo;
    for (int i = 2; i < 8; i++) pal[i] = ((float)(8 - i) * hi + (float)(i - 1) * lo) / 7.0f;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        float bd = 1e30f;
        if (hi > lo)
            for (int k = 0; k < 8; k++) {
                float d = fabsf(pal[k] - (float)v[i * stride]);
                if (d < bd) bd = d, best = k;
            }
        bits |= (uint64_t)best << (3 * i);
    }
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}
static void bc4_decode(const uint8_t in[8], uint8_t* v, int stride) {
    int e0 = in[0], e1 = in[1];
    float pal[8];
    pal[0] = (float)e0, pal[1] = (float)e1;
    if (e0 > e1)
        for (int i = 2; i < 8; i++) pal[i] = ((float)(8 - i) * e0 + (float)(i - 1) * e1) / 7.0f;
    else {
        for (int i = 2; i < 6; i++) pal[i] = ((float)(6 - i) * e0 + (float)(i - 1) * e1) / 5.0f;
        pal[6] = 0.0f, pal[7] = 255.0f;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= (uint64_t)in[2 + k] << (8 * k);
    for (int i = 0; i < 16; i++) v[i * stride] = (uint8_t)(pal[(bits >> (3 * i)) & 7u] + 0.5f);
}
void udim_bc5_encode_block(const uint8_t rgba[64], uint8_t out[16]) {
    bc4_encode(rgba, 4, out);
    bc4_encode(rgba + 1, 4, out + 8);
}
void udim_bc5_decode_block(const uint8_t in[16], uint8_t rgba[64]) {
    bc4_decode(in, rgba, 4);
    bc4_decode(in + 8, rgba + 1, 4);
    for (int i = 0; i < 16; i++) { // (the shaders do the same: mat_common.glsl udim_rebuild_b)
        float x = rgba[i * 4] / 127.5f - 1.0f, y = rgba[i * 4 + 1] / 127.5f - 1.0f;
        float z = sqrtf(fmaxf(1.0f - x * x - y * y, 0.0f));
        rgba[i * 4 + 2] = (uint8_t)(z * 127.5f + 127.5f + 0.5f > 255.0f ? 255 : (int)(z * 127.5f + 127.5f + 0.5f));
        rgba[i * 4 + 3] = 255;
    }
}

size_t udim_mip_bytes(uint32_t format, uint32_t w, uint32_t h) {
    if (format == UDIM_FMT_RGBA8) return (size_t)w * h * 4;
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
}

// ============================================================================================
// Import
// ============================================================================================

static float g_s2l[256];
static void s2l_init(void) {
    if (g_s2l[255] != 0.0f) return;
    for (int i = 0; i < 256; i++) {
        float s = i / 255.0f;
        g_s2l[i] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
    }
}
static uint8_t l2s(float l) {
    l = clampf(l, 0.0f, 1.0f);
    float s = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

static uint32_t pow2_for(uint32_t s, uint32_t cap) {
    uint32_t p = 4;
    while (p * 2 <= s) p *= 2;
    if ((float)s > (float)p * 1.4f) p *= 2; // (the nearest in log terms)
    while (p > cap) p /= 2;
    return p < 4 ? 4 : p;
}

// The image at a size: box filtered down, bilinear up (per axis)
static uint8_t* resize_rgba(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, bool srgb) {
    uint8_t* out = malloc((size_t)dw * dh * 4);
    if (!out) return NULL;
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            float acc[4] = { 0, 0, 0, 0 }, wsum = 0.0f;
            float fx0 = (float)x * sw / dw, fx1 = (float)(x + 1) * sw / dw, fy0 = (float)y * sh / dh, fy1 = (float)(y + 1) * sh / dh;
            if (fx1 - fx0 < 1.0f) fx0 = (fx0 + fx1) * 0.5f - 0.5f, fx1 = fx0 + 1.0f; // (up: a texel's footprint, bilinear)
            if (fy1 - fy0 < 1.0f) fy0 = (fy0 + fy1) * 0.5f - 0.5f, fy1 = fy0 + 1.0f;
            for (int sy = (int)floorf(fy0); sy < (int)ceilf(fy1); sy++)
                for (int sx = (int)floorf(fx0); sx < (int)ceilf(fx1); sx++) {
                    float wx = fminf(fx1, (float)sx + 1.0f) - fmaxf(fx0, (float)sx), wy = fminf(fy1, (float)sy + 1.0f) - fmaxf(fy0, (float)sy);
                    if (wx <= 0.0f || wy <= 0.0f) continue;
                    int cx = sx < 0 ? 0 : sx >= (int)sw ? (int)sw - 1 : sx, cy = sy < 0 ? 0 : sy >= (int)sh ? (int)sh - 1 : sy;
                    const uint8_t* p = &src[((size_t)cy * sw + (size_t)cx) * 4];
                    float w = wx * wy;
                    for (int k = 0; k < 4; k++) acc[k] += w * (srgb && k < 3 ? g_s2l[p[k]] : (float)p[k] / 255.0f);
                    wsum += w;
                }
            uint8_t* o = &out[((size_t)y * dw + x) * 4];
            for (int k = 0; k < 4; k++) {
                float v = wsum > 0.0f ? acc[k] / wsum : 0.0f;
                o[k] = srgb && k < 3 ? l2s(v) : (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    return out;
}

// The next mip: colors averaged in linear light, normals as vectors (renormalized), the rest as they are
static void mip_down(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh, int kind) {
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t xs[2] = { x * 2 < sw ? x * 2 : sw - 1, x * 2 + 1 < sw ? x * 2 + 1 : sw - 1 };
            uint32_t ys[2] = { y * 2 < sh ? y * 2 : sh - 1, y * 2 + 1 < sh ? y * 2 + 1 : sh - 1 };
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const uint8_t* p = &src[((size_t)ys[j] * sw + xs[i]) * 4];
                    for (int k = 0; k < 4; k++) acc[k] += kind == 1 && k < 3 ? g_s2l[p[k]] : (float)p[k] / 255.0f;
                }
            uint8_t* o = &dst[((size_t)y * dw + x) * 4];
            for (int k = 0; k < 4; k++) acc[k] *= 0.25f;
            if (kind == 2) { // a normal: the mean direction
                float n[3] = { acc[0] * 2 - 1, acc[1] * 2 - 1, acc[2] * 2 - 1 }, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-5f)
                    for (int k = 0; k < 3; k++) acc[k] = n[k] / l * 0.5f + 0.5f;
            }
            for (int k = 0; k < 4; k++) o[k] = kind == 1 && k < 3 ? l2s(acc[k]) : (uint8_t)(clampf(acc[k], 0, 1) * 255.0f + 0.5f);
        }
}

typedef struct EncJob {
    const uint8_t* px;
    uint32_t w, h, bw;
    uint32_t format;
    uint8_t* out;
} EncJob;
static void enc_row(void* arg, int by) {
    EncJob* j = arg;
    uint8_t blk[64];
    for (uint32_t bx = 0; bx < j->bw; bx++) {
        for (int i = 0; i < 16; i++) {
            uint32_t x = bx * 4 + (uint32_t)(i & 3), y = (uint32_t)by * 4 + (uint32_t)(i >> 2);
            if (x >= j->w) x = j->w - 1;
            if (y >= j->h) y = j->h - 1;
            memcpy(&blk[i * 4], &j->px[((size_t)y * j->w + x) * 4], 4);
        }
        uint8_t* o = &j->out[((size_t)by * j->bw + bx) * 16];
        if (j->format == UDIM_FMT_BC5) udim_bc5_encode_block(blk, o);
        else udim_bc7_encode_block(blk, o);
    }
}

// mip 0 against what its blocks decode to (dB over the channels kept)
static double block_psnr(const uint8_t* px, uint32_t w, uint32_t h, const uint8_t* blocks, uint32_t format) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    double se = 0.0;
    uint64_t n = 0;
    uint8_t d[64];
    int chans = format == UDIM_FMT_BC5 ? 2 : 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = &blocks[((size_t)by * bw + bx) * 16];
            if (format == UDIM_FMT_BC5) udim_bc5_decode_block(b, d);
            else udim_bc7_decode_block(b, d);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                if (x >= w || y >= h) continue;
                for (int k = 0; k < chans; k++) {
                    double e = (double)d[i * 4 + k] - px[((size_t)y * w + x) * 4 + k];
                    se += e * e, n++;
                }
            }
        }
    if (!n || se <= 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / (se / (double)n));
}

typedef struct DirScan {
    char dir[1024];
    char best[1024];
    int best_n;
} DirScan;
static void dir_scan(void* user, const char* name) {
    DirScan* s = user;
    char full[1100], pat[1100];
    snprintf(full, sizeof full, "%s/%s", s->dir, name);
    int u;
    if (!udim_pattern_of(full, pat, sizeof pat, &u)) return;
    int t[UDIM_MAX_TILES];
    int n = udim_find_tiles(pat, t, UDIM_MAX_TILES);
    if (n > s->best_n || (n == s->best_n && strcmp(pat, s->best) < 0)) s->best_n = n, snprintf(s->best, sizeof s->best, "%s", pat);
}

static void put64(uint8_t** p, size_t* n, size_t* cap, const void* d, size_t len) {
    if (*n + len > *cap) {
        size_t c = *cap ? *cap : (1u << 20);
        while (c < *n + len) c *= 2;
        uint8_t* q = realloc(*p, c);
        if (!q) return;
        *p = q, *cap = c;
    }
    if (d) memcpy(*p + *n, d, len);
    else memset(*p + *n, 0, len);
    *n += len;
}

bool udim_import(const char* src, const char* out_path, const UdimImportOpts* o, UdimImportReport* r) {
    UdimImportReport dummy;
    if (!r) r = &dummy;
    memset(r, 0, sizeof *r);
    double t0 = re_time_seconds();
    s2l_init();
    // the pattern: the folder's biggest set, the token's, or the tile's
    char pat[1024];
    bool dir = false;
    if (re_file_stat(src, NULL, NULL, &dir) && dir) {
        DirScan* s = calloc(1, sizeof *s);
        if (!s) return false;
        snprintf(s->dir, sizeof s->dir, "%s", src);
        size_t dl = strlen(s->dir);
        while (dl && (s->dir[dl - 1] == '/' || s->dir[dl - 1] == '\\')) s->dir[--dl] = 0;
        re_list_dir(s->dir, dir_scan, s);
        snprintf(pat, sizeof pat, "%s", s->best);
        bool found = s->best_n > 0;
        free(s);
        if (!found) {
            snprintf(r->err, sizeof r->err, "no UDIM tiles (<name>.1001.png ...) in the folder");
            return false;
        }
    } else if (strstr(src, "<UDIM>") || strstr(src, "<UVTILE>")) snprintf(pat, sizeof pat, "%s", src);
    else if (!udim_pattern_of(src, pat, sizeof pat, NULL)) {
        snprintf(r->err, sizeof r->err, "not a UDIM tile's name (<name>.1001.png)");
        return false;
    }
    snprintf(r->pattern, sizeof r->pattern, "%s", pat);
    int udims[UDIM_MAX_TILES];
    int n = udim_find_tiles(pat, udims, UDIM_MAX_TILES);
    if (!n) {
        snprintf(r->err, sizeof r->err, "no tile of %s found", base_name(pat));
        return false;
    }
    UdimImportOpts def = { -1, 0, false };
    if (!o) o = &def;
    bool normal = o->normal > 0 || (o->normal < 0 && udim_name_is_normal(pat));
    uint32_t format = o->rgba8 ? UDIM_FMT_RGBA8 : normal ? UDIM_FMT_BC5 : UDIM_FMT_BC7;
    uint32_t cap = o->max_size >= 4 && o->max_size <= UDIM_MAX_SIZE ? o->max_size : UDIM_MAX_SIZE;
    // masks (roughness, metal, AO, height...) averaged as they are; colors in linear light; normals as vectors
    char low[256];
    {
        const char* b = base_name(pat);
        size_t k = 0;
        for (; b[k] && k + 1 < sizeof low; k++) low[k] = (char)tolower((unsigned char)b[k]);
        low[k] = 0;
    }
    bool mask = strstr(low, "rough") || strstr(low, "metal") || strstr(low, "_ao") || strstr(low, "occlusion") || strstr(low, "height") ||
                strstr(low, "disp") || strstr(low, "mask") || strstr(low, "spec") || strstr(low, "gloss") || strstr(low, "orm");
    int kind = normal ? 2 : mask ? 0 : 1;
    RUdimTile* tiles = calloc((size_t)n, sizeof *tiles);
    uint8_t* data = NULL;
    size_t dn = 0, dcap = 0;
    double enc_ms = 0.0, psnr_min = 99.0;
    bool ok = tiles != NULL;
    for (int t = 0; ok && t < n; t++) {
        char p[1100];
        udim_tile_path(pat, udims[t], p, sizeof p);
        uint32_t w = 0, h = 0;
        uint8_t* px = mat_load_image_rgba(p, &w, &h);
        if (!px) {
            snprintf(r->err, sizeof r->err, "can't read %s", base_name(p));
            ok = false;
            break;
        }
        uint32_t tw = pow2_for(w, cap), th = pow2_for(h, cap);
        if (tw != w || th != h) {
            uint8_t* rs = resize_rgba(px, w, h, tw, th, kind == 1);
            free(px);
            px = rs, w = tw, h = th;
            if (!px) {
                ok = false;
                break;
            }
        }
        if (normal)
            for (size_t i = 0; i < (size_t)w * h; i++) px[i * 4 + 3] = 255;
        uint32_t mips = 1;
        while ((w >> mips) > 0 || (h >> mips) > 0) mips++;
        while (dn & 63) put64(&data, &dn, &dcap, NULL, 1);
        RUdimTile* tl = &tiles[t];
        tl->udim = (uint32_t)udims[t], tl->width = w, tl->height = h, tl->mips = mips, tl->data_offset = dn; // (relative: fixed below)
        { // the mean (the last mip's texel would be the same; computed here so it exists before the mips)
            double acc[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < (size_t)w * h; i++)
                for (int k = 0; k < 4; k++) acc[k] += kind == 1 && k < 3 ? g_s2l[px[i * 4 + k]] : px[i * 4 + k] / 255.0;
            uint32_t a = 0;
            for (int k = 0; k < 4; k++) {
                double v = acc[k] / ((double)w * h);
                uint8_t b = kind == 1 && k < 3 ? l2s((float)v) : (uint8_t)(clampf((float)v, 0, 1) * 255.0f + 0.5f);
                a |= (uint32_t)b << (8 * k);
            }
            tl->avg_rgba = a;
        }
        uint8_t* cur = px;
        uint32_t cw = w, ch = h;
        for (uint32_t m = 0; m < mips && ok; m++) {
            size_t mb = udim_mip_bytes(format, cw, ch);
            size_t at = dn;
            put64(&data, &dn, &dcap, NULL, mb);
            if (dn != at + mb) {
                ok = false;
                break;
            }
            if (format == UDIM_FMT_RGBA8) memcpy(data + at, cur, mb);
            else {
                double e0 = re_time_seconds();
                EncJob j = { cur, cw, ch, (cw + 3) / 4, format, data + at };
                re_parallel_for((int)((ch + 3) / 4), enc_row, &j);
                enc_ms += (re_time_seconds() - e0) * 1000.0;
                if (m == 0) {
                    double ps = block_psnr(cur, cw, ch, data + at, format);
                    if (ps < psnr_min) psnr_min = ps;
                }
            }
            if (m + 1 < mips) {
                uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
                uint8_t* nx = malloc((size_t)nw * nh * 4);
                if (!nx) {
                    ok = false;
                    break;
                }
                mip_down(cur, cw, ch, nx, nw, nh, kind);
                if (cur != px) free(cur);
                cur = nx, cw = nw, ch = nh;
            }
        }
        if (cur != px) free(cur);
        tl->data_size = dn - tl->data_offset;
        free(px);
        if (w > r->w) r->w = w;
        if (h > r->h) r->h = h;
    }
    if (ok && data) {
        RUdimHeader hd;
        memset(&hd, 0, sizeof hd);
        hd.magic = UDIM_MAGIC, hd.version = UDIM_VERSION, hd.header_size = sizeof hd;
        hd.flags = (normal ? UDIM_F_NORMAL : 0u) | (kind == 1 ? UDIM_F_SRGB : 0u);
        hd.format = format, hd.tile_count = (uint32_t)n, hd.tile_entry_size = sizeof(RUdimTile);
        hd.table_offset = sizeof hd;
        uint64_t data0 = (sizeof hd + sizeof(RUdimTile) * (uint64_t)n + 63) & ~63ull;
        for (int t = 0; t < n; t++) tiles[t].data_offset += data0;
        hd.file_size = data0 + dn;
        snprintf(hd.source, sizeof hd.source, "%s", base_name(pat));
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
        FILE* f = re_fopen(tmp, "wb");
        ok = f != NULL;
        if (f) {
            static const uint8_t zero[64];
            ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(tiles, sizeof(RUdimTile), (size_t)n, f) == (size_t)n;
            uint64_t at = sizeof hd + sizeof(RUdimTile) * (uint64_t)n;
            if (ok && data0 > at) ok = fwrite(zero, 1, (size_t)(data0 - at), f) == data0 - at;
            ok = ok && fwrite(data, 1, dn, f) == dn;
            ok = fclose(f) == 0 && ok;
        }
        ok = ok && re_replace_file(tmp, out_path);
        if (!ok) {
            re_remove_file(tmp);
            if (!r->err[0]) snprintf(r->err, sizeof r->err, "can't write %s", base_name(out_path));
        }
        r->bytes = hd.file_size;
    }
    free(data);
    free(tiles);
    r->tiles = ok ? n : 0;
    r->encode_ms = enc_ms;
    r->psnr_min = format == UDIM_FMT_RGBA8 ? 0.0 : psnr_min;
    r->ms = (re_time_seconds() - t0) * 1000.0;
    if (ok)
        printf("[RealEngine] UDIM import: %s -> %s, %d tiles up to %ux%u, %s, %.1f MB, %.0f ms (encode %.0f ms), PSNR >= %.1f dB\n",
               base_name(pat), base_name(out_path), n, r->w, r->h, format == UDIM_FMT_BC5 ? "BC5" : format == UDIM_FMT_BC7 ? "BC7" : "RGBA8",
               (double)r->bytes / 1048576.0, r->ms, r->encode_ms, r->psnr_min);
    return ok;
}

// ============================================================================================
// Reading
// ============================================================================================

bool udim_open(const char* path, UdimFile* f, char* err, size_t err_cap) {
    memset(f, 0, sizeof *f);
    char dummy[8];
    if (!err) err = dummy, err_cap = sizeof dummy;
    err[0] = 0;
    const uint8_t* d = NULL;
    uint64_t size = 0;
    f->map = re_map_file(path, &d, &size);
    if (!f->map) {
        snprintf(err, err_cap, "can't open");
        return false;
    }
    f->data = d, f->size = size;
    RUdimHeader h;
    memset(&h, 0, sizeof h);
    bool ok = size >= 24;
    if (ok) memcpy(&h, d, size < sizeof h ? (size_t)size : sizeof h);
    if (!ok || h.magic != UDIM_MAGIC) snprintf(err, err_cap, "not a .rudim"), ok = false;
    else if (h.version == 0 || h.version > UDIM_VERSION) snprintf(err, err_cap, "version %u (this engine reads up to %u)", h.version, UDIM_VERSION), ok = false;
    else if (h.header_size < 48 || h.header_size > size || h.tile_entry_size < 40 || h.tile_count == 0 || h.tile_count > UDIM_MAX_TILES ||
             h.format > UDIM_FMT_BC5 || h.table_offset < h.header_size || h.table_offset + (uint64_t)h.tile_entry_size * h.tile_count > size)
        snprintf(err, err_cap, "damaged header"), ok = false;
    if (ok) {
        f->version = h.version, f->flags = h.flags, f->format = h.format, f->count = h.tile_count;
        for (uint32_t i = 0; i < h.tile_count && ok; i++) {
            RUdimTile* t = &f->tiles[i];
            size_t take = h.tile_entry_size < sizeof *t ? h.tile_entry_size : sizeof *t;
            memset(t, 0, sizeof *t);
            memcpy(t, d + h.table_offset + (uint64_t)h.tile_entry_size * i, take);
            uint64_t need = 0;
            uint32_t w = t->width, hh = t->height;
            for (uint32_t m = 0; m < t->mips && m < 16; m++) {
                need += udim_mip_bytes(f->format, w, hh);
                w = w > 1 ? w / 2 : 1, hh = hh > 1 ? hh / 2 : 1;
            }
            int x, y;
            if (!udim_tile_xy((int)t->udim, &x, &y) || !t->width || !t->height || t->width > UDIM_MAX_SIZE || t->height > UDIM_MAX_SIZE ||
                !t->mips || t->mips > 16 || t->data_offset + need > size || t->data_size < need)
                snprintf(err, err_cap, "damaged tile %u", t->udim), ok = false;
        }
    }
    if (!ok) udim_close(f);
    return ok;
}

void udim_close(UdimFile* f) {
    if (f->map) re_unmap_file(f->map);
    memset(f, 0, sizeof *f);
}

int udim_find(const UdimFile* f, int udim) {
    for (uint32_t i = 0; i < f->count; i++)
        if ((int)f->tiles[i].udim == udim) return (int)i;
    return -1;
}

const uint8_t* udim_mip(const UdimFile* f, int tile, uint32_t mip, uint32_t* w, uint32_t* h, size_t* bytes) {
    if (!f->data || tile < 0 || (uint32_t)tile >= f->count) return NULL;
    const RUdimTile* t = &f->tiles[tile];
    if (mip >= t->mips) return NULL;
    uint64_t off = t->data_offset;
    uint32_t cw = t->width, ch = t->height;
    for (uint32_t m = 0; m < mip; m++) {
        off += udim_mip_bytes(f->format, cw, ch);
        cw = cw > 1 ? cw / 2 : 1, ch = ch > 1 ? ch / 2 : 1;
    }
    if (w) *w = cw;
    if (h) *h = ch;
    if (bytes) *bytes = udim_mip_bytes(f->format, cw, ch);
    return f->data + off;
}

void udim_prefetch(const UdimFile* f, int tile, uint32_t from_mip) {
    uint32_t w, h;
    size_t mb;
    const uint8_t* p = udim_mip(f, tile, from_mip, &w, &h, &mb);
    if (!p) return;
    const RUdimTile* t = &f->tiles[tile];
    size_t len = (size_t)(t->data_offset + t->data_size - (uint64_t)(p - f->data));
#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY e = { (void*)p, len };
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &e, 0);
#else
    (void)len;
#endif
}

// A .rudim's first tile as RGBA8 (the Content Browser's thumbnail and the texture viewer): its mip of 1024 or less
// decoded on the CPU (a BC5 normal map with its B rebuilt)
uint8_t* udim_preview_rgba(const char* path, uint32_t* w, uint32_t* h) {
    UdimFile* f = calloc(1, sizeof *f);
    if (!f || !udim_open(path, f, NULL, 0)) return free(f), (uint8_t*)NULL;
    uint32_t mip = 0;
    while (mip + 1 < f->tiles[0].mips && ((f->tiles[0].width >> mip) > 1024 || (f->tiles[0].height >> mip) > 1024)) mip++;
    uint32_t mw = 0, mh = 0;
    size_t mb = 0;
    const uint8_t* src = udim_mip(f, 0, mip, &mw, &mh, &mb);
    uint8_t* px = src ? malloc((size_t)mw * mh * 4) : NULL;
    if (px && f->format == UDIM_FMT_RGBA8) memcpy(px, src, (size_t)mw * mh * 4);
    else if (px) {
        uint32_t bw = (mw + 3) / 4;
        uint8_t blk[64];
        for (uint32_t by = 0; by < (mh + 3) / 4; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* b = src + ((size_t)by * bw + bx) * 16;
                if (f->format == UDIM_FMT_BC5) udim_bc5_decode_block(b, blk);
                else udim_bc7_decode_block(b, blk);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                    if (x < mw && y < mh) memcpy(px + ((size_t)y * mw + x) * 4, blk + i * 4, 4);
                }
            }
    }
    udim_close(f);
    free(f);
    *w = mw, *h = mh;
    return px;
}

// ============================================================================================
// The self test (--udim-test): no GPU
// ============================================================================================

static int g_fail;
#define CHECK(c, ...)                                                                                                                      \
    do {                                                                                                                                   \
        if (!(c)) printf("[RealEngine] FAIL: UDIM test: " __VA_ARGS__), printf("\n"), g_fail++;                                            \
    } while (0)

bool vkctx_png_write_rgb(const char* path, const uint8_t* rgb, uint32_t w, uint32_t h); // vkctx.c

// a tile whose color is a smooth function of the whole UDIM space (continuous across the tiles' edges: the seam test's)
void udim_write_test_tile(const char* path, int udim, uint32_t w, uint32_t h, bool normal) {
    uint8_t* rgb = malloc((size_t)w * h * 3);
    int tx = 0, ty = 0;
    udim_tile_xy(udim, &tx, &ty);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float u = tx + (x + 0.5f) / w, v = ty + 1.0f - (y + 0.5f) / h; // (Blender's V up: the image's top row is v = 1)
            uint8_t* p = &rgb[((size_t)y * w + x) * 3];
            if (normal) {
                float nx = 0.4f * sinf(u * 3.1f), ny = 0.4f * cosf(v * 2.3f), nz = sqrtf(1.0f - nx * nx - ny * ny);
                p[0] = (uint8_t)(nx * 127.5f + 127.5f), p[1] = (uint8_t)(ny * 127.5f + 127.5f), p[2] = (uint8_t)(nz * 127.5f + 127.5f);
            } else {
                p[0] = (uint8_t)(127.5f + 120.0f * sinf(u * 1.7f)), p[1] = (uint8_t)(127.5f + 120.0f * sinf(v * 2.1f + 0.5f));
                p[2] = (uint8_t)(127.5f + 120.0f * cosf((u + v) * 1.3f));
            }
        }
    vkctx_png_write_rgb(path, rgb, w, h);
    free(rgb);
}

int udim_selftest(void) {
    g_fail = 0;
    double t0 = re_time_seconds();
    // names
    char pat[512];
    int u = 0;
    CHECK(udim_pattern_of("C:/a/skin.1001.png", pat, sizeof pat, &u) && !strcmp(pat, "C:/a/skin.<UDIM>.png") && u == 1001, "skin.1001.png -> %s %d", pat, u);
    CHECK(udim_pattern_of("body_1012.tif", pat, sizeof pat, &u) && !strcmp(pat, "body_<UDIM>.tif") && u == 1012, "body_1012.tif");
    CHECK(udim_pattern_of("/x/hand_u2_v1.png", pat, sizeof pat, &u) && !strcmp(pat, "/x/hand_<UVTILE>.png") && u == 1002, "hand_u2_v1.png -> %s %d", pat, u);
    CHECK(!udim_pattern_of("rock.png", pat, sizeof pat, &u), "rock.png is no tile");
    CHECK(!udim_pattern_of("rock_2048.png", pat, sizeof pat, &u), "rock_2048.png is no tile");
    CHECK(!udim_pattern_of("1001.png", pat, sizeof pat, &u), "a bare 1001.png is no tile");
    char tp[512];
    udim_tile_path("C:/a/skin.<UDIM>.png", 1023, tp, sizeof tp);
    CHECK(!strcmp(tp, "C:/a/skin.1023.png"), "tile path %s", tp);
    udim_tile_path("/x/hand_<UVTILE>.png", 1013, tp, sizeof tp);
    CHECK(!strcmp(tp, "/x/hand_u3_v2.png"), "uvtile path %s", tp);
    CHECK(udim_of_uv(0.5f, 0.5f) == 1001 && udim_of_uv(1.5f, 0.2f) == 1002 && udim_of_uv(0.5f, -0.5f) == 1011 && udim_of_uv(2.5f, -1.5f) == 1023 &&
              udim_of_uv(-0.5f, 0.5f) == 0,
          "udim_of_uv");
    CHECK(udim_name_is_normal("Body_Normal.<UDIM>.png") && udim_name_is_normal("x_nrm_1001.png") && !udim_name_is_normal("Body_BaseColor.1001.png"), "normal names");
    char an[512];
    udim_asset_name("C:/in/Skin_BaseColor.<UDIM>.png", "C:/out", an, sizeof an);
    CHECK(!strcmp(an, "C:/out/Skin_BaseColor.rudim"), "asset name %s", an);
    // codecs: a block of noise and a gradient within a few levels
    {
        uint8_t blk[64], dec[64], enc[16];
        uint32_t s = 7;
        double worst = 0.0;
        for (int it = 0; it < 200; it++) {
            for (int i = 0; i < 64; i++) s = s * 1664525u + 1013904223u, blk[i] = (uint8_t)(it & 1 ? (s >> 24) : (i / 4) * 16 + (int)((s >> 28) & 3));
            udim_bc7_encode_block(blk, enc);
            udim_bc7_decode_block(enc, dec);
            double se = 0.0;
            for (int i = 0; i < 64; i++) se += (double)(dec[i] - blk[i]) * (dec[i] - blk[i]);
            if (!(it & 1) && se / 64.0 > worst) worst = se / 64.0;
        }
        CHECK(worst < 16.0, "BC7 of a gradient: mean squared error %.1f", worst);
        for (int i = 0; i < 16; i++) blk[i * 4] = (uint8_t)(i * 15), blk[i * 4 + 1] = (uint8_t)(255 - i * 9), blk[i * 4 + 2] = blk[i * 4 + 3] = 0;
        udim_bc5_encode_block(blk, enc);
        udim_bc5_decode_block(enc, dec);
        int e = 0;
        for (int i = 0; i < 16; i++) e = abs(dec[i * 4] - blk[i * 4]) > e ? abs(dec[i * 4] - blk[i * 4]) : e;
        CHECK(e <= 17, "BC5 error %d (a ramp over 225 levels: 8 steps of 32, half a step at worst)", e);
    }
    // import: three color tiles (1001, 1002, 1011) of 128 and a normal set of two, from one tile's name and from the folder
    char dir[700];
    if (!re_cache_dir(dir, sizeof dir - 40)) snprintf(dir, sizeof dir, ".");
    strncat(dir, "/udim_test", sizeof dir - strlen(dir) - 1);
    re_remove_tree(dir);
    re_make_dirs(dir);
    char p[900], out[900];
    const int tiles[3] = { 1001, 1002, 1011 };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/T_Skin_BaseColor.%d.png", dir, tiles[i]);
        udim_write_test_tile(p, tiles[i], 128, 128, false);
    }
    snprintf(p, sizeof p, "%s/T_Odd.1001.png", dir), udim_write_test_tile(p, 1001, 100, 60, false);
    snprintf(p, sizeof p, "%s/T_Odd.1002.png", dir), udim_write_test_tile(p, 1002, 100, 60, false);
    for (int i = 0; i < 2; i++) snprintf(p, sizeof p, "%s/T_Skin_Normal_%d.png", dir, tiles[i]), udim_write_test_tile(p, tiles[i], 64, 64, true);
    char found[512];
    snprintf(p, sizeof p, "%s/T_Skin_BaseColor.1002.png", dir);
    CHECK(udim_detect(p, found, sizeof found) && strstr(found, "T_Skin_BaseColor.<UDIM>.png"), "detect from a tile: %s", found);
    snprintf(p, sizeof p, "%s/T_Lonely.1001.png", dir), udim_write_test_tile(p, 1001, 8, 8, false);
    CHECK(!udim_detect(p, NULL, 0), "a lone tile is a plain image");
    snprintf(p, sizeof p, "%s/T_Skin_BaseColor.1011.png", dir);
    udim_asset_name(found, dir, out, sizeof out);
    UdimImportReport r;
    bool ok = udim_import(p, out, NULL, &r);
    CHECK(ok && r.tiles == 3 && r.w == 128, "import from a tile: %s", r.err);
    UdimFile f;
    char err[120];
    if (ok && udim_open(out, &f, err, sizeof err)) {
        CHECK(f.count == 3 && f.format == UDIM_FMT_BC7 && (f.flags & UDIM_F_SRGB) && f.version == UDIM_VERSION, "header");
        CHECK(udim_find(&f, 1011) == 2 && udim_find(&f, 1003) < 0, "tiles");
        CHECK(f.tiles[0].mips == 8, "mips %u", f.tiles[0].mips);
        uint32_t mw, mh;
        size_t mb;
        CHECK(udim_mip(&f, 1, 7, &mw, &mh, &mb) && mw == 1 && mh == 1 && mb == 16, "the last mip");
        CHECK(r.psnr_min > 34.0, "BC7 PSNR %.1f dB", r.psnr_min);
        udim_close(&f);
    } else CHECK(false, "open: %s", err);
    // from the folder: its biggest set (the base color's 3 over the odd set's 2 and the normals' 2)
    snprintf(out, sizeof out, "%s/folder.rudim", dir);
    ok = udim_import(dir, out, NULL, &r);
    CHECK(ok && r.tiles == 3 && strstr(r.pattern, "T_Skin_BaseColor"), "import from the folder: %d tiles, %s %s", r.tiles, r.pattern, r.err);
    // a size that isn't a power of two: to the nearest (100 x 60 -> 128 x 64)
    snprintf(p, sizeof p, "%s/T_Odd.<UDIM>.png", dir);
    snprintf(out, sizeof out, "%s/odd.rudim", dir);
    ok = udim_import(p, out, NULL, &r) && udim_open(out, &f, err, sizeof err);
    if (ok) {
        CHECK(f.tiles[0].width == 128 && f.tiles[0].height == 64, "power of two %ux%u", f.tiles[0].width, f.tiles[0].height);
        udim_close(&f);
    } else CHECK(false, "odd sizes: %s %s", r.err, err);
    // normals: BC5, B rebuilt
    snprintf(p, sizeof p, "%s/T_Skin_Normal_1001.png", dir);
    snprintf(out, sizeof out, "%s/normal.rudim", dir);
    ok = udim_import(p, out, NULL, &r) && udim_open(out, &f, err, sizeof err);
    if (ok) {
        CHECK(f.format == UDIM_FMT_BC5 && (f.flags & UDIM_F_NORMAL) && f.count == 2, "normal map: format %u", f.format);
        CHECK(r.psnr_min > 36.0, "BC5 PSNR %.1f dB", r.psnr_min);
        udim_close(&f);
    } else CHECK(false, "normal map: %s %s", r.err, err);
    // versions: a file with a bigger header and tile entries (a later version's layout under version 1's number) opens; a
    // newer version is refused with the reason; a cut file is refused
    {
        snprintf(out, sizeof out, "%s/folder.rudim", dir);
        const uint8_t* d;
        uint64_t sz;
        ReFileMap* m = re_map_file(out, &d, &sz);
        RUdimHeader h;
        memcpy(&h, d, sizeof h);
        const RUdimTile* tt = (const RUdimTile*)(d + h.table_offset);
        uint32_t hs = 96, es = 64;
        uint64_t data0 = (hs + (uint64_t)es * h.tile_count + 63) & ~63ull, old0 = tt[0].data_offset;
        size_t nd = (size_t)(sz - old0);
        uint8_t* big = calloc(1, (size_t)(data0 + nd));
        RUdimHeader h2 = h;
        h2.header_size = (uint16_t)hs, h2.tile_entry_size = es, h2.table_offset = hs, h2.file_size = data0 + nd;
        memcpy(big, &h2, sizeof h2);
        for (uint32_t i = 0; i < h.tile_count; i++) {
            RUdimTile t2 = tt[i];
            t2.data_offset = t2.data_offset - old0 + data0;
            memcpy(big + hs + (uint64_t)es * i, &t2, sizeof t2);
            memset(big + hs + (uint64_t)es * i + sizeof t2, 0xAB, es - sizeof t2); // (fields this version doesn't know)
        }
        memcpy(big + data0, d + old0, nd);
        re_unmap_file(m);
        char fp[900];
        snprintf(fp, sizeof fp, "%s/grown.rudim", dir);
        FILE* w = re_fopen(fp, "wb");
        if (w) fwrite(big, 1, (size_t)(data0 + nd), w), fclose(w);
        ok = udim_open(fp, &f, err, sizeof err);
        CHECK(ok && f.count == 3 && f.tiles[2].udim == 1011, "a grown header opens: %s", err);
        if (ok) udim_close(&f);
        ((RUdimHeader*)big)->version = 2;
        w = re_fopen(fp, "wb");
        if (w) fwrite(big, 1, (size_t)(data0 + nd), w), fclose(w);
        CHECK(!udim_open(fp, &f, err, sizeof err) && strstr(err, "version 2"), "a newer version refused: %s", err);
        ((RUdimHeader*)big)->version = 1;
        w = re_fopen(fp, "wb");
        if (w) fwrite(big, 1, (size_t)(data0 + nd / 2), w), fclose(w);
        CHECK(!udim_open(fp, &f, err, sizeof err), "a cut file refused");
        free(big);
    }
    re_remove_tree(dir);
    printf("[RealEngine] UDIM test: %s (%d failures, %.0f ms)\n", g_fail ? "FAIL" : "passed", g_fail, (re_time_seconds() - t0) * 1000.0);
    return g_fail;
}
