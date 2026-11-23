// Procedural textures and color math of the shader nodes, following Cycles' definitions (svm_noise,
// svm_voronoi, svm_checker, svm_gradient, svm_wave, svm_math, svm_color_util) so that the same
// node setups look the same as in Blender.
#ifndef KERNEL_SVM_TEX_H
#define KERNEL_SVM_TEX_H

// (Real Engine: the texture functions need no kernel globals: the Tlender's kernel_globals.h is not copied)
#include "kernel_svm_types.h"

// --- Hashes: Bob Jenkins' lookup3, as Cycles' hash_uint* ------------------------------------

KSTRUCT(Abc)
struct Abc {
    kuint a, b, c;
};

KFN kuint krot(kuint x, kuint k) { return (x << k) | (x >> (32u - k)); }

KFN Abc jenkins_mix(Abc s) {
    s.a -= s.c; s.a ^= krot(s.c, 4u);  s.c += s.b;
    s.b -= s.a; s.b ^= krot(s.a, 6u);  s.a += s.c;
    s.c -= s.b; s.c ^= krot(s.b, 8u);  s.b += s.a;
    s.a -= s.c; s.a ^= krot(s.c, 16u); s.c += s.b;
    s.b -= s.a; s.b ^= krot(s.a, 19u); s.a += s.c;
    s.c -= s.b; s.c ^= krot(s.b, 4u);  s.b += s.a;
    return s;
}

KFN Abc jenkins_final(Abc s) {
    s.c ^= s.b; s.c -= krot(s.b, 14u);
    s.a ^= s.c; s.a -= krot(s.c, 11u);
    s.b ^= s.a; s.b -= krot(s.a, 25u);
    s.c ^= s.b; s.c -= krot(s.b, 16u);
    s.a ^= s.c; s.a -= krot(s.c, 4u);
    s.b ^= s.a; s.b -= krot(s.a, 14u);
    s.c ^= s.b; s.c -= krot(s.b, 24u);
    return s;
}

KFN Abc jenkins_init(kuint n) {
    Abc s;
    s.a = 0xdeadbeefu + (n << 2u) + 13u;
    s.b = s.a;
    s.c = s.a;
    return s;
}

KFN kuint hash_uint1(kuint kx) {
    Abc s = jenkins_init(1u);
    s.a += kx;
    return jenkins_final(s).c;
}
KFN kuint hash_uint2(kuint kx, kuint ky) {
    Abc s = jenkins_init(2u);
    s.b += ky;
    s.a += kx;
    return jenkins_final(s).c;
}
KFN kuint hash_uint3(kuint kx, kuint ky, kuint kz) {
    Abc s = jenkins_init(3u);
    s.c += kz;
    s.b += ky;
    s.a += kx;
    return jenkins_final(s).c;
}
KFN kuint hash_uint4(kuint kx, kuint ky, kuint kz, kuint kw) {
    Abc s = jenkins_init(4u);
    s.a += kx;
    s.b += ky;
    s.c += kz;
    s = jenkins_mix(s);
    s.a += kw;
    return jenkins_final(s).c;
}

KFN float uint_to_float_incl(kuint n) { return KFLOAT(n) * (1.0f / 4294967295.0f); }
KFN float hash_float_to_float(float k) { return uint_to_float_incl(hash_uint1(kfloat_as_uint(k))); }
KFN float hash_float2_to_float(float x, float y) {
    return uint_to_float_incl(hash_uint2(kfloat_as_uint(x), kfloat_as_uint(y)));
}
KFN float hash_float3_to_float(vec3 k) {
    return uint_to_float_incl(hash_uint3(kfloat_as_uint(k.x), kfloat_as_uint(k.y), kfloat_as_uint(k.z)));
}
KFN float hash_float4_to_float(vec3 k, float w) {
    return uint_to_float_incl(hash_uint4(kfloat_as_uint(k.x), kfloat_as_uint(k.y), kfloat_as_uint(k.z), kfloat_as_uint(w)));
}
KFN vec3 hash_float3_to_float3(vec3 k) {
    return v3(hash_float3_to_float(k), hash_float4_to_float(k, 1.0f), hash_float4_to_float(k, 2.0f));
}
KFN vec3 random_float3_offset(float seed) {
    return v3(100.0f + hash_float2_to_float(seed, 0.0f) * 100.0f, 100.0f + hash_float2_to_float(seed, 1.0f) * 100.0f,
              100.0f + hash_float2_to_float(seed, 2.0f) * 100.0f);
}

// --- Perlin noise and fBm ------------------------------------------------------------------

KFN float noise_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
KFN float negate_if(float v, kuint cond) { return cond != 0u ? -v : v; }

KFN float noise_grad3(kuint hash, float x, float y, float z) {
    kuint h = hash & 15u;
    float u = h < 8u ? x : y;
    float vt = (h == 12u || h == 14u) ? x : z;
    float v = h < 4u ? y : vt;
    return negate_if(u, h & 1u) + negate_if(v, h & 2u);
}

KFN float perlin_3d(vec3 p) {
    float fx0 = floorf(p.x), fy0 = floorf(p.y), fz0 = floorf(p.z);
    kuint X = KUINT(KINT(fx0)), Y = KUINT(KINT(fy0)), Z = KUINT(KINT(fz0));
    float fx = p.x - fx0, fy = p.y - fy0, fz = p.z - fz0;
    float u = noise_fade(fx), v = noise_fade(fy), w = noise_fade(fz);
    float g000 = noise_grad3(hash_uint3(X, Y, Z), fx, fy, fz);
    float g100 = noise_grad3(hash_uint3(X + 1u, Y, Z), fx - 1.0f, fy, fz);
    float g010 = noise_grad3(hash_uint3(X, Y + 1u, Z), fx, fy - 1.0f, fz);
    float g110 = noise_grad3(hash_uint3(X + 1u, Y + 1u, Z), fx - 1.0f, fy - 1.0f, fz);
    float g001 = noise_grad3(hash_uint3(X, Y, Z + 1u), fx, fy, fz - 1.0f);
    float g101 = noise_grad3(hash_uint3(X + 1u, Y, Z + 1u), fx - 1.0f, fy, fz - 1.0f);
    float g011 = noise_grad3(hash_uint3(X, Y + 1u, Z + 1u), fx, fy - 1.0f, fz - 1.0f);
    float g111 = noise_grad3(hash_uint3(X + 1u, Y + 1u, Z + 1u), fx - 1.0f, fy - 1.0f, fz - 1.0f);
    float x00 = kmix(g000, g100, u), x10 = kmix(g010, g110, u), x01 = kmix(g001, g101, u), x11 = kmix(g011, g111, u);
    return kmix(kmix(x00, x10, v), kmix(x01, x11, v), w);
}

// Cycles repeats the noise every 100000 on each axis (float precision), half a cell off past 1e6 (snoise_*d).
KFN float noise_wrap(float x) {
    if (!(fabsf(x) >= 100000.0f)) return x;
    return x - 100000.0f * truncf(x * 1e-5f) + (fabsf(x) >= 1000000.0f ? 0.5f : 0.0f);
}

// Signed noise in about [-1, 1] (Cycles' noise_scale3).
KFN float snoise_3d(vec3 p) { return 0.9820f * perlin_3d(v3(noise_wrap(p.x), noise_wrap(p.y), noise_wrap(p.z))); }

// 1D, 2D and 4D Perlin noise (Cycles' svm/noise.h, the standard implementation).
KFN float perlin_1d(float x) {
    float fx0 = floorf(x);
    kuint X = KUINT(KINT(fx0));
    float fx = x - fx0;
    kuint h0 = hash_uint1(X) & 15u, h1 = hash_uint1(X + 1u) & 15u;
    float g0 = negate_if(1.0f + KFLOAT(h0 & 7u), h0 & 8u) * fx, g1 = negate_if(1.0f + KFLOAT(h1 & 7u), h1 & 8u) * (fx - 1.0f);
    return kmix(g0, g1, noise_fade(fx));
}

KFN float noise_grad2(kuint hash, float x, float y) {
    kuint h = hash & 7u;
    float u = h < 4u ? x : y;
    float v = 2.0f * (h < 4u ? y : x);
    return negate_if(u, h & 1u) + negate_if(v, h & 2u);
}

KFN float perlin_2d(float x, float y) {
    float fx0 = floorf(x), fy0 = floorf(y);
    kuint X = KUINT(KINT(fx0)), Y = KUINT(KINT(fy0));
    float fx = x - fx0, fy = y - fy0;
    float u = noise_fade(fx), v = noise_fade(fy);
    float g00 = noise_grad2(hash_uint2(X, Y), fx, fy), g10 = noise_grad2(hash_uint2(X + 1u, Y), fx - 1.0f, fy);
    float g01 = noise_grad2(hash_uint2(X, Y + 1u), fx, fy - 1.0f), g11 = noise_grad2(hash_uint2(X + 1u, Y + 1u), fx - 1.0f, fy - 1.0f);
    return (1.0f - v) * (g00 * (1.0f - u) + g10 * u) + v * (g01 * (1.0f - u) + g11 * u);
}

KFN float noise_grad4(kuint hash, float x, float y, float z, float w) {
    kuint h = hash & 31u;
    float u = h < 24u ? x : y;
    float v = h < 16u ? y : z;
    float s = h < 8u ? z : w;
    return negate_if(u, h & 1u) + negate_if(v, h & 2u) + negate_if(s, h & 4u);
}

// One W layer of the 4D noise: the trilinear mix of its 8 corners (Cycles' tri_mix order)
KFN float perlin_4d_layer(kuint X, kuint Y, kuint Z, kuint W, float fx, float fy, float fz, float fw, float u, float v, float t) {
    float g0 = noise_grad4(hash_uint4(X, Y, Z, W), fx, fy, fz, fw);
    float g1 = noise_grad4(hash_uint4(X + 1u, Y, Z, W), fx - 1.0f, fy, fz, fw);
    float g2 = noise_grad4(hash_uint4(X, Y + 1u, Z, W), fx, fy - 1.0f, fz, fw);
    float g3 = noise_grad4(hash_uint4(X + 1u, Y + 1u, Z, W), fx - 1.0f, fy - 1.0f, fz, fw);
    float g4 = noise_grad4(hash_uint4(X, Y, Z + 1u, W), fx, fy, fz - 1.0f, fw);
    float g5 = noise_grad4(hash_uint4(X + 1u, Y, Z + 1u, W), fx - 1.0f, fy, fz - 1.0f, fw);
    float g6 = noise_grad4(hash_uint4(X, Y + 1u, Z + 1u, W), fx, fy - 1.0f, fz - 1.0f, fw);
    float g7 = noise_grad4(hash_uint4(X + 1u, Y + 1u, Z + 1u, W), fx - 1.0f, fy - 1.0f, fz - 1.0f, fw);
    float x1 = 1.0f - u, y1 = 1.0f - v, z1 = 1.0f - t;
    return z1 * (y1 * (g0 * x1 + g1 * u) + v * (g2 * x1 + g3 * u)) + t * (y1 * (g4 * x1 + g5 * u) + v * (g6 * x1 + g7 * u));
}

KFN float perlin_4d(vec3 p, float w) {
    float fx0 = floorf(p.x), fy0 = floorf(p.y), fz0 = floorf(p.z), fw0 = floorf(w);
    kuint X = KUINT(KINT(fx0)), Y = KUINT(KINT(fy0)), Z = KUINT(KINT(fz0)), W = KUINT(KINT(fw0));
    float fx = p.x - fx0, fy = p.y - fy0, fz = p.z - fz0, fw = w - fw0;
    float u = noise_fade(fx), v = noise_fade(fy), t = noise_fade(fz), s = noise_fade(fw);
    float a = 0.0f, b = 0.0f;
    for (int k = 0; k < 2; k++) { // the two W layers
        float l = perlin_4d_layer(X, Y, Z, W + KUINT(k), fx, fy, fz, fw - KFLOAT(k), u, v, t);
        if (k == 0) a = l;
        else b = l;
    }
    return kmix(a, b, s);
}

// Signed noise of 1 to 4 dimensions: 1D reads w, 2D x and y, 3D p, 4D p and w (Cycles' snoise_*d with its scales).
KFN float snoise_nd(int dims, vec3 p, float w) {
    if (dims == 1) return 0.2500f * perlin_1d(noise_wrap(w));
    if (dims == 2) return 0.6616f * perlin_2d(noise_wrap(p.x), noise_wrap(p.y));
    if (dims == 3) return snoise_3d(p);
    return 0.8344f * perlin_4d(v3(noise_wrap(p.x), noise_wrap(p.y), noise_wrap(p.z)), noise_wrap(w));
}

KFN float noise_fbm(vec3 p, float detail, float roughness, float lacunarity, bool normalize) {
    float fscale = 1.0f, amp = 1.0f, maxamp = 0.0f, sum = 0.0f;
    detail = clampf(detail, 0.0f, 15.0f);
    roughness = fmaxf(roughness, 0.0f);
    int n = KINT(detail);
    for (int i = 0; i <= 15; i++) {
        if (i > n) break;
        float t = snoise_3d(v3_scale(p, fscale));
        sum += t * amp;
        maxamp += amp;
        amp *= roughness;
        fscale *= lacunarity;
    }
    float rmd = detail - floorf(detail);
    if (rmd != 0.0f) {
        float t = snoise_3d(v3_scale(p, fscale));
        float sum2 = sum + t * amp;
        return normalize ? kmix(0.5f * sum / maxamp + 0.5f, 0.5f * sum2 / (maxamp + amp) + 0.5f, rmd) : kmix(sum, sum2, rmd);
    }
    return normalize ? 0.5f * sum / maxamp + 0.5f : sum;
}

KSTRUCT(TexResult)
struct TexResult {
    float fac;
    vec3 color;
    vec3 position;
};

// Component k of the seed offsets of a dims-dimensional noise (Cycles' random_float*_offset): in 1D the offset is of w,
// a scalar of its own hash; else component 3 is w.
KFN float noise_offset_comp(int dims, float seed, int k) {
    if (dims == 1) return 100.0f + hash_float_to_float(seed) * 100.0f;
    return 100.0f + hash_float2_to_float(seed, KFLOAT(k)) * 100.0f;
}

// The point (p, w) moved by the seed offsets of a dims-dimensional noise
KFN vec3 noise_offset_p(int dims, vec3 p, float seed) {
    if (dims == 1) return p;
    return v3(p.x + noise_offset_comp(dims, seed, 0), p.y + noise_offset_comp(dims, seed, 1),
              dims >= 3 ? p.z + noise_offset_comp(dims, seed, 2) : p.z);
}
KFN float noise_offset_w(int dims, float w, float seed) {
    if (dims == 1) return w + noise_offset_comp(1, seed, 0);
    return dims == 4 ? w + noise_offset_comp(dims, seed, 3) : w;
}

// The five fractal noises of Blender's Noise Texture (Cycles' svm/fractal_noise.h) in one octave loop: octave i samples at
// lacunarity^i, the fraction of Detail one more octave. Runtime bounds (no unrolled copies of the noise).
KFN float noise_fractal(int type, int dims, vec3 p, float w, float detail, float roughness, float lacunarity, float offset,
                        float gain, bool normalize) {
    int n = KINT(detail);
    float rmd = detail - floorf(detail);
    float fscale = 1.0f, amp = 1.0f, maxamp = 0.0f, sum = 0.0f, sum2 = 0.0f;
    float value = type == NOISE_MULTIFRACTAL ? 1.0f : 0.0f;
    float pwr = type == NOISE_HETERO_TERRAIN || type == NOISE_RIDGED_MULTIFRACTAL ? roughness : 1.0f;
    float weight = 1.0f, signal = 0.0f;
    bool remainder = false;
    for (int i = 0; i <= n + 1; i++) {
        bool last = i == n + 1;
        if (last && (rmd == 0.0f || type == NOISE_RIDGED_MULTIFRACTAL)) break;
        if (type == NOISE_HYBRID_MULTIFRACTAL && !(weight > 0.001f)) break;
        float t = snoise_nd(dims, v3_scale(p, fscale), w * fscale);
        fscale *= lacunarity;
        if (type == NOISE_FBM) {
            if (last) {
                sum2 = sum + t * amp;
                remainder = true;
            } else {
                sum += t * amp;
                maxamp += amp;
                amp *= roughness;
            }
        } else if (type == NOISE_MULTIFRACTAL) {
            value *= (last ? rmd : 1.0f) * pwr * t + 1.0f;
            pwr *= roughness;
        } else if (type == NOISE_HETERO_TERRAIN) {
            if (i == 0) {
                value = offset + t;
            } else {
                value += (last ? rmd : 1.0f) * (t + offset) * pwr * value;
                pwr *= roughness;
            }
        } else if (type == NOISE_HYBRID_MULTIFRACTAL) {
            weight = fminf(weight, 1.0f);
            signal = (t + offset) * pwr;
            pwr *= roughness;
            value += (last ? rmd : 1.0f) * weight * signal;
            weight *= gain * signal;
        } else { // ridged
            if (i > 0) weight = ksaturate(signal * gain);
            signal = offset - fabsf(t);
            signal *= signal;
            if (i == 0) {
                value = signal;
            } else {
                signal *= weight;
                value += signal * pwr;
                pwr *= roughness;
            }
        }
    }
    if (type != NOISE_FBM) return value;
    if (remainder)
        return normalize ? kmix(0.5f * sum / maxamp + 0.5f, 0.5f * sum2 / (maxamp + amp) + 0.5f, rmd) : kmix(sum, sum2, rmd);
    return normalize ? 0.5f * sum / maxamp + 0.5f : sum;
}

// Blender's Noise Texture (Cycles' svm_node_tex_noise): dims 1 to 4 (1D: W, 4D: Vector and W), the fractal type.
// color: the Color output is used (two more fractals at seeded offsets).
KFN TexResult tex_noise(int dims, int type, bool normalize, vec3 p, float w, float scale, float detail, float roughness,
                        float lacunarity, float offset, float gain, float distortion, bool color) {
    detail = clampf(detail, 0.0f, 15.0f);
    roughness = fmaxf(roughness, 0.0f);
    dims = kmini(kmaxi(dims, 1), 4);
    p = v3_scale(p, scale);
    w *= scale;
    if (distortion != 0.0f) { // each component by the noise at its own offset of the undistorted point
        vec3 dp = v3_zero();
        float dw = 0.0f;
        for (int k = 0; k < dims; k++) {
            float d = snoise_nd(dims, noise_offset_p(dims, p, KFLOAT(k)), noise_offset_w(dims, w, KFLOAT(k))) * distortion;
            int c = dims == 1 ? 3 : k; // 1D distorts w
            if (c == 0) dp.x = d;
            else if (c == 1) dp.y = d;
            else if (c == 2) dp.z = d;
            else dw = d;
        }
        p = v3_add(p, dp);
        w += dw;
    }
    TexResult r;
    vec3 ch = v3_zero();
    for (int c = 0; c < (color ? 3 : 1); c++) { // the value, then the color's G and B at seeds dims and dims + 1
        float seed = KFLOAT(dims + c - 1);
        vec3 pc = c == 0 ? p : noise_offset_p(dims, p, seed);
        float wc = c == 0 ? w : noise_offset_w(dims, w, seed);
        float f = noise_fractal(type, dims, pc, wc, detail, roughness, lacunarity, offset, gain, normalize);
        if (c == 0) ch.x = f;
        else if (c == 1) ch.y = f;
        else ch.z = f;
    }
    r.fac = ch.x;
    r.color = ch;
    r.position = v3_zero();
    return r;
}

// --- Voronoi (F1 / F2 in 3D) ---------------------------------------------------------------

KFN float voronoi_distance(vec3 a, vec3 b, int metric) {
    vec3 d = v3_abs(v3_sub(a, b));
    if (metric == VORONOI_MANHATTAN) return d.x + d.y + d.z;
    if (metric == VORONOI_CHEBYCHEV) return fmaxf(d.x, fmaxf(d.y, d.z));
    return v3_len(d);
}

KFN TexResult tex_voronoi(vec3 coord, float scale, float randomness, int feature, int metric) {
    coord = v3_scale(coord, scale);
    randomness = ksaturate(randomness);
    vec3 cell = v3(floorf(coord.x), floorf(coord.y), floorf(coord.z));
    vec3 local = v3_sub(coord, cell);
    float d1 = 8.0f, d2 = 8.0f;
    vec3 off1 = v3_zero(), off2 = v3_zero(), pos1 = v3_zero(), pos2 = v3_zero();
    for (int k = -1; k <= 1; k++)
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                vec3 off = v3(KFLOAT(i), KFLOAT(j), KFLOAT(k));
                vec3 pt = v3_add(off, v3_scale(hash_float3_to_float3(v3_add(cell, off)), randomness));
                float d = voronoi_distance(pt, local, metric);
                if (d < d1) {
                    d2 = d1;
                    off2 = off1;
                    pos2 = pos1;
                    d1 = d;
                    off1 = off;
                    pos1 = pt;
                } else if (d < d2) {
                    d2 = d;
                    off2 = off;
                    pos2 = pt;
                }
            }
    TexResult r;
    bool f2 = feature == VORONOI_F2;
    r.fac = f2 ? d2 : d1;
    r.color = hash_float3_to_float3(v3_add(cell, f2 ? off2 : off1));
    r.position = v3_scale(v3_add(f2 ? pos2 : pos1, cell), scale != 0.0f ? 1.0f / scale : 0.0f);
    return r;
}

// --- Checker, gradient, wave, white noise ----------------------------------------------------

KFN float tex_checker(vec3 p) {
    // Cycles' offset, without contraction: an fma(p, 0.999999, 1e-6 * 0.999999) moves 1.0 (the flat axis of a plane's
    // Generated coordinate times an even scale) to the other cell on the GPU.
    KPRECISE float x = (p.x + 0.000001f) * 0.999999f;
    KPRECISE float y = (p.y + 0.000001f) * 0.999999f;
    KPRECISE float z = (p.z + 0.000001f) * 0.999999f;
    int xi = KINT(fabsf(floorf(x))), yi = KINT(fabsf(floorf(y))), zi = KINT(fabsf(floorf(z)));
    return ((xi % 2 == yi % 2) == (zi % 2 != 0)) ? 1.0f : 0.0f;
}

KFN float tex_gradient(vec3 p, int type) {
    float x = p.x, y = p.y, z = p.z;
    if (type == GRADIENT_QUADRATIC) {
        float r = fmaxf(x, 0.0f);
        return r * r;
    }
    if (type == GRADIENT_EASING) {
        float r = fminf(fmaxf(x, 0.0f), 1.0f);
        float t = r * r;
        return 3.0f * t - 2.0f * t * r;
    }
    if (type == GRADIENT_DIAGONAL) return (x + y) * 0.5f;
    if (type == GRADIENT_RADIAL) return atan2f(y, x) / K_2PI + 0.5f;
    if (type == GRADIENT_QUADRATIC_SPHERE) {
        float r = fmaxf(0.999999f - sqrtf(x * x + y * y + z * z), 0.0f);
        return r * r;
    }
    if (type == GRADIENT_SPHERICAL) return fmaxf(0.999999f - sqrtf(x * x + y * y + z * z), 0.0f);
    return x; // linear
}

// direction: bands 0 X, 1 Y, 2 Z, 3 diagonal; rings 0 X, 1 Y, 2 Z, 3 spherical.
KFN float tex_wave(vec3 p, int type, int direction, int profile, float distortion, float detail, float dscale,
                   float droughness, float phase) {
    p = v3_scale(v3_add(p, v3_splat(0.000001f)), 0.999999f);
    float n;
    if (type == WAVE_BANDS) {
        if (direction == 0) n = p.x * 20.0f;
        else if (direction == 1) n = p.y * 20.0f;
        else if (direction == 2) n = p.z * 20.0f;
        else n = (p.x + p.y + p.z) * 10.0f;
    } else {
        vec3 rp = p;
        if (direction == 0) rp = v3_mul(rp, v3(0.0f, 1.0f, 1.0f));
        else if (direction == 1) rp = v3_mul(rp, v3(1.0f, 0.0f, 1.0f));
        else if (direction == 2) rp = v3_mul(rp, v3(1.0f, 1.0f, 0.0f));
        n = v3_len(rp) * 20.0f;
    }
    n += phase;
    if (distortion != 0.0f) n += distortion * (noise_fbm(v3_scale(p, dscale), detail, droughness, 2.0f, true) * 2.0f - 1.0f);
    if (profile == WAVE_PROFILE_SAW) {
        n /= K_2PI;
        return n - floorf(n);
    }
    if (profile == WAVE_PROFILE_TRI) {
        n /= K_2PI;
        return fabsf(n - floorf(n + 0.5f)) * 2.0f;
    }
    return 0.5f + 0.5f * sinf(n - K_PI * 0.5f);
}

// --- Color -------------------------------------------------------------------------------

KFN vec3 rgb_to_hsv(vec3 rgb) {
    float cmax = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z)), cmin = fminf(rgb.x, fminf(rgb.y, rgb.z));
    float delta = cmax - cmin, h = 0.0f, s = 0.0f;
    if (cmax != 0.0f) s = delta / cmax;
    if (s != 0.0f) {
        vec3 c = v3_scale(v3_sub(v3_splat(cmax), rgb), 1.0f / delta);
        if (rgb.x == cmax) h = c.z - c.y;
        else if (rgb.y == cmax) h = 2.0f + c.x - c.z;
        else h = 4.0f + c.y - c.x;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    return v3(h, s, cmax);
}

KFN vec3 hsv_to_rgb(vec3 hsv) {
    float h = hsv.x, s = hsv.y, v = hsv.z;
    if (s == 0.0f) return v3(v, v, v);
    if (h == 1.0f) h = 0.0f;
    h *= 6.0f;
    float i = floorf(h), f = h - i;
    float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    int ii = KINT(i);
    if (ii == 0) return v3(v, t, p);
    if (ii == 1) return v3(q, v, p);
    if (ii == 2) return v3(p, v, t);
    if (ii == 3) return v3(p, q, v);
    if (ii == 4) return v3(t, p, v);
    return v3(v, p, q);
}

KFN vec3 rgb_to_hsl(vec3 rgb) {
    float cmax = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z)), cmin = fminf(rgb.x, fminf(rgb.y, rgb.z));
    float h = 0.0f, s = 0.0f, l = fminf(1.0f, (cmax + cmin) * 0.5f);
    if (cmax != cmin) {
        float d = cmax - cmin;
        s = l > 0.5f ? d / (2.0f - cmax - cmin) : d / (cmax + cmin);
        if (cmax == rgb.x) h = (rgb.y - rgb.z) / d + (rgb.y < rgb.z ? 6.0f : 0.0f);
        else if (cmax == rgb.y) h = (rgb.z - rgb.x) / d + 2.0f;
        else h = (rgb.x - rgb.y) / d + 4.0f;
        h /= 6.0f;
    }
    return v3(h, s, l);
}

KFN float hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

KFN vec3 hsl_to_rgb(vec3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s == 0.0f) return v3(l, l, l);
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    return v3(hue_to_rgb(p, q, h + 1.0f / 3.0f), hue_to_rgb(p, q, h), hue_to_rgb(p, q, h - 1.0f / 3.0f));
}

KFN float dodge_channel(float c1, float c2, float t) {
    if (c1 == 0.0f) return c1;
    float tmp = 1.0f - t * c2;
    if (tmp <= 0.0f) return 1.0f;
    return fminf(c1 / tmp, 1.0f);
}
KFN float burn_channel(float c1, float c2, float t) {
    float tmp = (1.0f - t) + t * c2;
    if (tmp <= 0.0f) return 0.0f;
    return clampf(1.0f - (1.0f - c1) / tmp, 0.0f, 1.0f);
}
KFN float overlay_channel(float c1, float c2, float t) {
    float tm = 1.0f - t;
    return c1 < 0.5f ? c1 * (tm + 2.0f * t * c2) : 1.0f - (tm + 2.0f * t * (1.0f - c2)) * (1.0f - c1);
}
KFN float div_channel(float c1, float c2, float t) { return c2 != 0.0f ? (1.0f - t) * c1 + t * c1 / c2 : c1; }

// Blender's color blend modes (Mix node, MA_RAMP_*).
KFN vec3 svm_mix_color(int type, float t, vec3 c1, vec3 c2) {
    float tm = 1.0f - t;
    if (type == BLEND_ADD) return v3_mix(c1, v3_add(c1, c2), t);
    if (type == BLEND_MUL) return v3_mix(c1, v3_mul(c1, c2), t);
    if (type == BLEND_SUB) return v3_mix(c1, v3_sub(c1, c2), t);
    if (type == BLEND_SCREEN)
        return v3_sub(v3_one(), v3_mul(v3_add(v3_splat(tm), v3_scale(v3_sub(v3_one(), c2), t)), v3_sub(v3_one(), c1)));
    if (type == BLEND_DIV) return v3(div_channel(c1.x, c2.x, t), div_channel(c1.y, c2.y, t), div_channel(c1.z, c2.z, t));
    if (type == BLEND_DIFF) return v3_mix(c1, v3_abs(v3_sub(c1, c2)), t);
    if (type == BLEND_EXCLUSION)
        return v3_clamp_min(v3_mix(c1, v3_sub(v3_add(c1, c2), v3_scale(v3_mul(c1, c2), 2.0f)), t), 0.0f);
    if (type == BLEND_DARK) return v3_mix(c1, v3_min(c1, c2), t);
    if (type == BLEND_LIGHT) return v3_mix(c1, v3_max(c1, c2), t);
    if (type == BLEND_OVERLAY)
        return v3(overlay_channel(c1.x, c2.x, t), overlay_channel(c1.y, c2.y, t), overlay_channel(c1.z, c2.z, t));
    if (type == BLEND_DODGE) return v3(dodge_channel(c1.x, c2.x, t), dodge_channel(c1.y, c2.y, t), dodge_channel(c1.z, c2.z, t));
    if (type == BLEND_BURN) return v3(burn_channel(c1.x, c2.x, t), burn_channel(c1.y, c2.y, t), burn_channel(c1.z, c2.z, t));
    if (type == BLEND_HUE || type == BLEND_COLOR) {
        vec3 hsv2 = rgb_to_hsv(c2);
        if (hsv2.y == 0.0f) return c1;
        vec3 hsv = rgb_to_hsv(c1);
        hsv.x = hsv2.x;
        if (type == BLEND_COLOR) hsv.y = hsv2.y;
        return v3_mix(c1, hsv_to_rgb(hsv), t);
    }
    if (type == BLEND_SAT) {
        vec3 hsv = rgb_to_hsv(c1);
        if (hsv.y == 0.0f) return c1;
        hsv.y = tm * hsv.y + t * rgb_to_hsv(c2).y;
        return hsv_to_rgb(hsv);
    }
    if (type == BLEND_VAL) {
        vec3 hsv = rgb_to_hsv(c1);
        hsv.z = tm * hsv.z + t * rgb_to_hsv(c2).z;
        return hsv_to_rgb(hsv);
    }
    if (type == BLEND_SOFT) {
        vec3 scr = v3_sub(v3_one(), v3_mul(v3_sub(v3_one(), c2), v3_sub(v3_one(), c1)));
        return v3_add(v3_scale(c1, tm), v3_scale(v3_add(v3_mul(v3_mul(v3_sub(v3_one(), c1), c2), c1), v3_mul(c1, scr)), t));
    }
    if (type == BLEND_LINEAR) return v3_add(c1, v3_scale(v3_sub(v3_scale(c2, 2.0f), v3_one()), t));
    return v3_mix(c1, c2, t);
}

// --- Math -------------------------------------------------------------------------------

KFN float safe_divide(float a, float b) { return b != 0.0f ? a / b : 0.0f; }
KFN float kfmod(float a, float b) { return a - b * truncf(a / b); } // C's fmodf (GLSL mod floors)
KFN float safe_modulo(float a, float b) { return b != 0.0f ? kfmod(a, b) : 0.0f; }
KFN float safe_floored_modulo(float a, float b) { return b != 0.0f ? a - floorf(a / b) * b : 0.0f; }
KFN float kfract(float a) { return a - floorf(a); }
KFN float wrapf(float value, float vmax, float vmin) {
    float range = vmax - vmin;
    return range != 0.0f ? value - range * floorf((value - vmin) / range) : vmin;
}
KFN float pingpongf(float a, float b) { return b != 0.0f ? fabsf(kfract((a - b) / (b * 2.0f)) * b * 2.0f - b) : 0.0f; }
KFN float smoothminf(float a, float b, float k) {
    if (k != 0.0f) {
        float h = fmaxf(k - fabsf(a - b), 0.0f) / k;
        return fminf(a, b) - h * h * h * k * (1.0f / 6.0f);
    }
    return fminf(a, b);
}
KFN float safe_powf(float a, float b) {
    if (a < 0.0f && b != floorf(b)) return 0.0f;
    if (b == 0.0f) return 1.0f;
    if (a < 0.0f) {
        float r = powf(-a, b);
        return (KINT(b) % 2 == 0) ? r : -r;
    }
    return powf(a, b);
}
KFN float safe_logf(float a, float b) {
    if (a <= 0.0f || b <= 0.0f) return 0.0f;
    return safe_divide(logf(a), logf(b));
}
KFN float signf(float a) { return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f); }

KFN float svm_math(int op, float a, float b, float c) {
    if (op == MATH_ADD) return a + b;
    if (op == MATH_SUBTRACT) return a - b;
    if (op == MATH_MULTIPLY) return a * b;
    if (op == MATH_DIVIDE) return safe_divide(a, b);
    if (op == MATH_POWER) return safe_powf(a, b);
    if (op == MATH_LOGARITHM) return safe_logf(a, b);
    if (op == MATH_SQRT) return ksafe_sqrt(a);
    if (op == MATH_INV_SQRT) return a > 0.0f ? 1.0f / sqrtf(a) : 0.0f;
    if (op == MATH_ABSOLUTE) return fabsf(a);
    if (op == MATH_RADIANS) return a * (K_PI / 180.0f);
    if (op == MATH_DEGREES) return a * (180.0f / K_PI);
    if (op == MATH_MINIMUM) return fminf(a, b);
    if (op == MATH_MAXIMUM) return fmaxf(a, b);
    if (op == MATH_LESS_THAN) return a < b ? 1.0f : 0.0f;
    if (op == MATH_GREATER_THAN) return a > b ? 1.0f : 0.0f;
    if (op == MATH_ROUND) return floorf(a + 0.5f);
    if (op == MATH_FLOOR) return floorf(a);
    if (op == MATH_CEIL) return ceilf(a);
    if (op == MATH_FRACTION) return kfract(a);
    if (op == MATH_MODULO) return safe_modulo(a, b);
    if (op == MATH_FLOORED_MODULO) return safe_floored_modulo(a, b);
    if (op == MATH_TRUNC) return truncf(a);
    if (op == MATH_SNAP) return floorf(safe_divide(a, b)) * b;
    if (op == MATH_WRAP) return wrapf(a, b, c);
    if (op == MATH_PINGPONG) return pingpongf(a, b);
    if (op == MATH_SINE) return sinf(a);
    if (op == MATH_COSINE) return cosf(a);
    if (op == MATH_TANGENT) return tanf(a);
    if (op == MATH_SINH) return sinhf(a);
    if (op == MATH_COSH) return coshf(a);
    if (op == MATH_TANH) return tanhf(a);
    if (op == MATH_ARCSINE) return asinf(clampf(a, -1.0f, 1.0f));
    if (op == MATH_ARCCOSINE) return acosf(clampf(a, -1.0f, 1.0f));
    if (op == MATH_ARCTANGENT) return atanf(a);
    if (op == MATH_ARCTAN2) return atan2f(a, b);
    if (op == MATH_SIGN) return signf(a);
    if (op == MATH_EXPONENT) return expf(a);
    if (op == MATH_COMPARE) return (a == b || fabsf(a - b) <= fmaxf(c, 1.192092896e-07f)) ? 1.0f : 0.0f;
    if (op == MATH_MULTIPLY_ADD) return a * b + c;
    if (op == MATH_SMOOTH_MIN) return smoothminf(a, b, c);
    if (op == MATH_SMOOTH_MAX) return -smoothminf(-a, -b, c);
    return 0.0f;
}

KSTRUCT(VMathResult)
struct VMathResult {
    vec3 v;
    float f;
};

KFN vec3 v3_safe_normalize(vec3 a) {
    float l = v3_len(a);
    return l > 0.0f ? v3_scale(a, 1.0f / l) : v3_zero();
}

KFN VMathResult svm_vector_math(int op, vec3 a, vec3 b, vec3 c, float s) {
    VMathResult r;
    r.v = v3_zero();
    r.f = 0.0f;
    if (op == VMATH_ADD) r.v = v3_add(a, b);
    else if (op == VMATH_SUBTRACT) r.v = v3_sub(a, b);
    else if (op == VMATH_MULTIPLY) r.v = v3_mul(a, b);
    else if (op == VMATH_DIVIDE) r.v = v3(safe_divide(a.x, b.x), safe_divide(a.y, b.y), safe_divide(a.z, b.z));
    else if (op == VMATH_CROSS) r.v = v3_cross(a, b);
    else if (op == VMATH_PROJECT) {
        float l2 = v3_dot(b, b);
        r.v = l2 != 0.0f ? v3_scale(b, v3_dot(a, b) / l2) : v3_zero();
    } else if (op == VMATH_REFLECT) {
        vec3 n = v3_safe_normalize(b);
        r.v = v3_sub(a, v3_scale(n, 2.0f * v3_dot(n, a)));
    } else if (op == VMATH_REFRACT) {
        vec3 n = v3_safe_normalize(b);
        float d = v3_dot(n, a), k = 1.0f - s * s * (1.0f - d * d);
        r.v = k < 0.0f ? v3_zero() : v3_sub(v3_scale(a, s), v3_scale(n, s * d + sqrtf(k)));
    } else if (op == VMATH_FACEFORWARD) r.v = v3_dot(c, b) < 0.0f ? a : v3_neg(a);
    else if (op == VMATH_MULTIPLY_ADD) r.v = v3_add(v3_mul(a, b), c);
    else if (op == VMATH_DOT) r.f = v3_dot(a, b);
    else if (op == VMATH_DISTANCE) r.f = v3_len(v3_sub(a, b));
    else if (op == VMATH_LENGTH) r.f = v3_len(a);
    else if (op == VMATH_SCALE) r.v = v3_scale(a, s);
    else if (op == VMATH_NORMALIZE) r.v = v3_safe_normalize(a);
    else if (op == VMATH_SNAP)
        r.v = v3(floorf(safe_divide(a.x, b.x)) * b.x, floorf(safe_divide(a.y, b.y)) * b.y, floorf(safe_divide(a.z, b.z)) * b.z);
    else if (op == VMATH_FLOOR) r.v = v3(floorf(a.x), floorf(a.y), floorf(a.z));
    else if (op == VMATH_CEIL) r.v = v3(ceilf(a.x), ceilf(a.y), ceilf(a.z));
    else if (op == VMATH_MODULO) r.v = v3(safe_modulo(a.x, b.x), safe_modulo(a.y, b.y), safe_modulo(a.z, b.z));
    else if (op == VMATH_WRAP) r.v = v3(wrapf(a.x, b.x, c.x), wrapf(a.y, b.y, c.y), wrapf(a.z, b.z, c.z));
    else if (op == VMATH_FRACTION) r.v = v3(kfract(a.x), kfract(a.y), kfract(a.z));
    else if (op == VMATH_ABSOLUTE) r.v = v3_abs(a);
    else if (op == VMATH_MINIMUM) r.v = v3_min(a, b);
    else if (op == VMATH_MAXIMUM) r.v = v3_max(a, b);
    else if (op == VMATH_SINE) r.v = v3(sinf(a.x), sinf(a.y), sinf(a.z));
    else if (op == VMATH_COSINE) r.v = v3(cosf(a.x), cosf(a.y), cosf(a.z));
    else if (op == VMATH_TANGENT) r.v = v3(tanf(a.x), tanf(a.y), tanf(a.z));
    return r;
}

KFN float smootherstep(float e0, float e1, float x) {
    float t = ksaturate(safe_divide(x - e0, e1 - e0));
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
KFN float kstep_smooth(float e0, float e1, float x) { // Cycles' smoothstep (exact at the edges)
    if (x < e0) return 0.0f;
    if (x >= e1) return 1.0f;
    float t = (x - e0) / (e1 - e0);
    return (3.0f - 2.0f * t) * (t * t);
}

KFN float svm_map_range(int type, bool clamp_result, float value, float fmin, float fmax, float tmin, float tmax,
                        float steps) {
    float factor;
    if (type == MAP_RANGE_STEPPED) {
        factor = safe_divide(value - fmin, fmax - fmin);
        factor = steps > 0.0f ? floorf(factor * (steps + 1.0f)) / steps : 0.0f;
    } else if (type == MAP_RANGE_SMOOTHSTEP) {
        factor = fmin > fmax ? 1.0f - kstep_smooth(fmax, fmin, value) : kstep_smooth(fmin, fmax, value);
    } else if (type == MAP_RANGE_SMOOTHERSTEP) {
        factor = fmin > fmax ? 1.0f - smootherstep(fmax, fmin, value) : smootherstep(fmin, fmax, value);
    } else {
        factor = safe_divide(value - fmin, fmax - fmin);
    }
    float r = tmin + factor * (tmax - tmin);
    if (clamp_result && (type == MAP_RANGE_LINEAR || type == MAP_RANGE_STEPPED))
        r = clampf(r, fminf(tmin, tmax), fmaxf(tmin, tmax));
    return r;
}

// Euler XYZ rotation (R = Rz * Ry * Rx), applied to a vector or its transpose.
KFN vec3 euler_rotate(vec3 rot, vec3 v, bool transpose) {
    float cx = cosf(rot.x), sx = sinf(rot.x), cy = cosf(rot.y), sy = sinf(rot.y), cz = cosf(rot.z), sz = sinf(rot.z);
    vec3 r0 = v3(cy * cz, sy * sx * cz - cx * sz, sy * cx * cz + sx * sz);
    vec3 r1 = v3(cy * sz, sy * sx * sz + cx * cz, sy * cx * sz - sx * cz);
    vec3 r2 = v3(-sy, cy * sx, cy * cx);
    if (transpose) return v3_add(v3_add(v3_scale(r0, v.x), v3_scale(r1, v.y)), v3_scale(r2, v.z));
    return v3(v3_dot(r0, v), v3_dot(r1, v), v3_dot(r2, v));
}

KFN vec3 svm_mapping(int type, vec3 v, vec3 loc, vec3 rot, vec3 scale) {
    if (type == MAPPING_TEXTURE) {
        vec3 r = euler_rotate(rot, v3_sub(v, loc), true);
        return v3(safe_divide(r.x, scale.x), safe_divide(r.y, scale.y), safe_divide(r.z, scale.z));
    }
    if (type == MAPPING_VECTOR) return euler_rotate(rot, v3_mul(v, scale), false);
    if (type == MAPPING_NORMAL)
        return v3_safe_normalize(
            euler_rotate(rot, v3(safe_divide(v.x, scale.x), safe_divide(v.y, scale.y), safe_divide(v.z, scale.z)), false));
    return v3_add(euler_rotate(rot, v3_mul(v, scale), false), loc);
}

// Vector Rotate (Cycles' svm_node_vector_rotate): about an axis through the center, or by an Euler XYZ rotation.
KFN vec3 svm_vector_rotate(int type, bool invert, vec3 v, vec3 center, vec3 axis, float angle, vec3 rot) {
    vec3 p = v3_sub(v, center);
    if (type == VROT_EULER_XYZ) return v3_add(euler_rotate(rot, p, invert), center);
    if (type == VROT_X) axis = v3(1.0f, 0.0f, 0.0f);
    else if (type == VROT_Y) axis = v3(0.0f, 1.0f, 0.0f);
    else if (type == VROT_Z) axis = v3(0.0f, 0.0f, 1.0f);
    float len = v3_len(axis);
    if (len == 0.0f) return v;
    axis = v3_scale(axis, 1.0f / len);
    if (invert) angle = -angle;
    float c = cosf(angle), s = sinf(angle);
    vec3 r = v3_add(v3_add(v3_scale(p, c), v3_scale(v3_cross(axis, p), s)), v3_scale(axis, v3_dot(axis, p) * (1.0f - c)));
    return v3_add(r, center);
}

// (Real Engine: svm_blackbody left out: its table lives in the Tlender's kernel data)

#endif
