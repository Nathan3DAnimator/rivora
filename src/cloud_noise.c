// The clouds' noise, made once on the CPU (weather.c uploads it): the Tlender's model (its sky_clouds.c, copied and
// adapted; Schneider 2015 / Hillaire 2016): a tileable 64^3 shape texture (Perlin-Worley and three Worley fbm octaves),
// a 32^3 detail texture (three Worley fbm octaves), and from the shape's values the coverage thresholds (quantiles: a
// local coverage c covers that share of the layer) and the prefiltered means (what a far sample, whose step spans many
// noise cells, reads instead of the noise: no streaks toward the horizon).
#include "cloud_noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
static float hashf(uint32_t h) { return (float)(h & 0xffffff) / 16777216.0f; }
static int wrapi(int i, int n) { return ((i % n) + n) % n; }
static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float sat(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float remap(float v, float lo, float hi, float nlo, float nhi) {
    if (hi - lo == 0.0f) return nlo;
    return nlo + (v - lo) * (nhi - nlo) / (hi - lo);
}

// Tileable gradient noise (period p cells), in -1..1
static float perlin3(float x, float y, float z, int p, uint32_t seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y), z0 = (int)floorf(z);
    float fx = x - x0, fy = y - y0, fz = z - z0;
    float u = fade(fx), v = fade(fy), w = fade(fz);
    float acc[8];
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        uint32_t h = hash3(wrapi(x0 + dx, p), wrapi(y0 + dy, p), wrapi(z0 + dz, p), seed);
        float gx = hashf(h) * 2 - 1, gy = hashf(h * 747796405u + 1u) * 2 - 1, gz = hashf(h * 2891336453u + 7u) * 2 - 1;
        float gl = sqrtf(gx * gx + gy * gy + gz * gz) + 1e-6f;
        acc[k] = (gx * (fx - dx) + gy * (fy - dy) + gz * (fz - dz)) / gl;
    }
    float x00 = acc[0] + u * (acc[1] - acc[0]), x10 = acc[2] + u * (acc[3] - acc[2]);
    float x01 = acc[4] + u * (acc[5] - acc[4]), x11 = acc[6] + u * (acc[7] - acc[6]);
    float y0v = x00 + v * (x10 - x00), y1v = x01 + v * (x11 - x01);
    return (y0v + w * (y1v - y0v)) * 1.5f;
}

// Tileable Worley noise: 1 - the distance to the nearest feature point (in cells), 0..1
static float worley3(float x, float y, float z, int p, uint32_t seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y), z0 = (int)floorf(z);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int cx = x0 + dx, cy = y0 + dy, cz = z0 + dz;
                uint32_t h = hash3(wrapi(cx, p), wrapi(cy, p), wrapi(cz, p), seed);
                float px = cx + hashf(h), py = cy + hashf(h * 747796405u + 1u), pz = cz + hashf(h * 2891336453u + 7u);
                float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                if (d < best) best = d;
            }
    return 1.0f - fminf(sqrtf(best), 1.0f);
}

static float worley_fbm(float x, float y, float z, int f, uint32_t seed) {
    return worley3(x * f, y * f, z * f, f, seed) * 0.625f + worley3(x * 2 * f, y * 2 * f, z * 2 * f, 2 * f, seed + 1) * 0.25f +
           worley3(x * 4 * f, y * 4 * f, z * 4 * f, 4 * f, seed + 2) * 0.125f;
}

typedef struct Job { uint16_t* shape; uint8_t* detail; } Job;

static void make_shape(void* arg, int z) {
    Job* j = arg;
    for (int y = 0; y < CLOUD_SHAPE_N; y++)
        for (int x = 0; x < CLOUD_SHAPE_N; x++) {
            float px = (x + 0.5f) / CLOUD_SHAPE_N, py = (y + 0.5f) / CLOUD_SHAPE_N, pz = (z + 0.5f) / CLOUD_SHAPE_N;
            float pf = 0.0f, amp = 1.0f, tot = 0.0f; // Perlin fbm, period 4..16 cells
            for (int o = 0; o < 3; o++) {
                int per = 4 << o;
                pf += perlin3(px * per, py * per, pz * per, per, 11u + o) * amp;
                tot += amp;
                amp *= 0.5f;
            }
            pf = sat(pf / tot * 0.5f + 0.5f);
            float wf = worley_fbm(px, py, pz, 4, 21u);
            float pw = remap(pf, 0.0f, 1.0f, wf, 1.0f); // Perlin-Worley: billowy Perlin
            float v[4] = { sat(pw), worley_fbm(px, py, pz, 4, 31u), worley_fbm(px, py, pz, 8, 41u), worley_fbm(px, py, pz, 16, 51u) };
            uint16_t* o = j->shape + (((size_t)z * CLOUD_SHAPE_N + y) * CLOUD_SHAPE_N + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = (uint16_t)lroundf(sat(v[k]) * 65535.0f);
        }
}

static void make_detail(void* arg, int z) {
    Job* j = arg;
    for (int y = 0; y < CLOUD_DETAIL_N; y++)
        for (int x = 0; x < CLOUD_DETAIL_N; x++) {
            float px = (x + 0.5f) / CLOUD_DETAIL_N, py = (y + 0.5f) / CLOUD_DETAIL_N, pz = (z + 0.5f) / CLOUD_DETAIL_N;
            uint8_t* o = j->detail + (((size_t)z * CLOUD_DETAIL_N + y) * CLOUD_DETAIL_N + x) * 4;
            o[0] = (uint8_t)lroundf(sat(worley_fbm(px, py, pz, 2, 61u)) * 255.0f);
            o[1] = (uint8_t)lroundf(sat(worley_fbm(px, py, pz, 4, 71u)) * 255.0f);
            o[2] = (uint8_t)lroundf(sat(worley_fbm(px, py, pz, 8, 81u)) * 255.0f);
            o[3] = 255;
        }
}

static int cmp_float(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

static float threshold_of(const float* quant, float cov) {
    float x = sat(cov) * (CLOUD_QUANT_N - 1);
    int i = (int)x;
    if (i >= CLOUD_QUANT_N - 1) return quant[CLOUD_QUANT_N - 1];
    return quant[i] + (quant[i + 1] - quant[i]) * (x - i);
}

bool cloud_noise_make(uint16_t* shape, uint8_t* detail, float* tables) {
    Job j = { shape, detail };
    re_parallel_for(CLOUD_SHAPE_N, make_shape, &j);
    re_parallel_for(CLOUD_DETAIL_N, make_detail, &j);
    // the shape's values (every 3rd texel, as density() remaps them): their quantiles and the mean density by coverage
    // and height gradient
    enum { STRIDE = 3 };
    int n = 0, side = CLOUD_SHAPE_N / STRIDE + 1;
    float* b0 = malloc(sizeof(float) * (size_t)side * side * side);
    if (!b0) return false;
    for (int z = 0; z < CLOUD_SHAPE_N; z += STRIDE)
        for (int y = 0; y < CLOUD_SHAPE_N; y += STRIDE)
            for (int x = 0; x < CLOUD_SHAPE_N; x += STRIDE) {
                const uint16_t* s = shape + (((size_t)z * CLOUD_SHAPE_N + y) * CLOUD_SHAPE_N + x) * 4;
                float s0 = s[0] / 65535.0f, low = s[1] / 65535.0f * 0.625f + s[2] / 65535.0f * 0.25f + s[3] / 65535.0f * 0.125f;
                b0[n++] = remap(s0, -(1.0f - low), 1.0f, 0.0f, 1.0f);
            }
    qsort(b0, (size_t)n, sizeof(float), cmp_float);
    float* quant = tables;
    float* mean = tables + CLOUD_QUANT_N;
    for (int i = 0; i < CLOUD_QUANT_N; i++) { // a share c = i / (N - 1) of the values above quant[i]
        float c = (float)i / (CLOUD_QUANT_N - 1);
        int k = (int)((1.0f - c) * (n - 1) + 0.5f);
        quant[i] = i == 0 ? 1.0f : i == CLOUD_QUANT_N - 1 ? 0.0f : b0[k];
    }
    for (int ci = 0; ci < CLOUD_MEAN_N; ci++)
        for (int gi = 0; gi < CLOUD_MEAN_N; gi++) {
            float cov = (float)ci / (CLOUD_MEAN_N - 1), grad = (float)gi / (CLOUD_MEAN_N - 1), thr = threshold_of(quant, cov);
            double sum = 0.0;
            for (int i = 0; i < n; i++) {
                float b = remap(b0[i] * grad, thr, 1.0f, 0.0f, 1.0f) * (0.4f + 0.6f * cov); // as density()
                if (b > 0.0f) sum += b;
            }
            mean[ci * CLOUD_MEAN_N + gi] = (float)(sum / n);
        }
    free(b0);
    return true;
}

// The coverage map (the Tlender's weather(): value-noise fbm, three octaves at x1, x2.3, x5.1), made tileable: over 10
// cells of the first octave the others span 23 and 51 whole cells, each hashed modulo its period (no seam)
static float vnoise2_tile(float x, float y, int period, uint32_t seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    float u = fade(fx), v = fade(fy);
    int xa = wrapi(x0, period), xb = wrapi(x0 + 1, period), ya = wrapi(y0, period), yb = wrapi(y0 + 1, period);
    float a = hashf(hash3(xa, ya, 0, seed)), b = hashf(hash3(xb, ya, 0, seed));
    float c = hashf(hash3(xa, yb, 0, seed)), d = hashf(hash3(xb, yb, 0, seed));
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

typedef struct WJob { uint16_t* out; } WJob;
static void make_weather_row(void* arg, int y) {
    WJob* j = arg;
    for (int x = 0; x < CLOUD_WEATHER_N; x++) {
        float px = (x + 0.5f) / CLOUD_WEATHER_N * CLOUD_WEATHER_CELLS, py = (y + 0.5f) / CLOUD_WEATHER_N * CLOUD_WEATHER_CELLS;
        float v = vnoise2_tile(px, py, CLOUD_WEATHER_CELLS, 3u) * 0.55f +
                  vnoise2_tile(px * 2.3f, py * 2.3f, (int)(CLOUD_WEATHER_CELLS * 2.3f + 0.5f), 5u) * 0.28f +
                  vnoise2_tile(px * 5.1f, py * 5.1f, (int)(CLOUD_WEATHER_CELLS * 5.1f + 0.5f), 7u) * 0.17f;
        j->out[(size_t)y * CLOUD_WEATHER_N + x] = (uint16_t)lroundf(sat(v) * 65535.0f);
    }
}

void cloud_weather_make(uint16_t* out) {
    WJob j = { out };
    re_parallel_for(CLOUD_WEATHER_N, make_weather_row, &j);
}
