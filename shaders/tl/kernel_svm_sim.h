// Procedural textures of the simulation materials (C11 / GLSL dialect, kernel_compat.h): Blender's Brick and Magic
// textures (Cycles' svm_brick / svm_magic) and Tlender's weave, cells (dried-mud cracks, cobblestones, sand grains),
// ocean waves and fibers. The surface shaders built on them (water, fabric, soil, mud, sand, snow, ice, rock, bark,
// leaf, lava, wetness) are compiled into the other instructions (svm_sim_nodes.inc): they add no kernel code.
#ifndef KERNEL_SVM_SIM_H
#define KERNEL_SVM_SIM_H

#include "kernel_svm_tex.h"

// a % b != 0 for b > 0 with C's sign rules (GLSL's % is undefined for negative operands)
KFN bool kimod_nonzero(int a, int b) { return (KUINT(a < 0 ? -a : a) % KUINT(b)) != 0u; }

// --- Brick (Cycles' svm_brick) ------------------------------------------------------------------------------

KFN float brick_noise(kuint n) {
    n = (n + 1013u) & 0x7fffffffu;
    n = (n >> 13u) ^ n;
    kuint nn = (n * (n * n * 60493u + 19990303u) + 1376312589u) & 0x7fffffffu;
    return 0.5f * (KFLOAT(nn) / 1073741824.0f);
}

// x: the brick's tint, y: the mortar factor
KFN float2 tex_brick(vec3 p, float mortar_size, float mortar_smooth, float bias, float brick_width, float row_height,
                     float offset_amount, int offset_frequency, float squash_amount, int squash_frequency) {
    row_height = fmaxf(row_height, 1e-6f);
    int rownum = KINT(floorf(p.y / row_height));
    float offset = 0.0f;
    if (offset_frequency > 0 && squash_frequency > 0) {
        brick_width *= kimod_nonzero(rownum, squash_frequency) ? 1.0f : squash_amount;
        offset = kimod_nonzero(rownum, offset_frequency) ? 0.0f : brick_width * offset_amount;
    }
    brick_width = fmaxf(brick_width, 1e-6f);
    int bricknum = KINT(floorf((p.x + offset) / brick_width));
    float x = (p.x + offset) - brick_width * KFLOAT(bricknum);
    float y = p.y - row_height * KFLOAT(rownum);
    float tint = ksaturate(brick_noise((KUINT(rownum) << 16u) + (KUINT(bricknum) & 0xFFFFu)) + bias);
    float min_dist = fminf(fminf(x, y), fminf(brick_width - x, row_height - y));
    float mortar;
    if (min_dist >= mortar_size) {
        mortar = 0.0f;
    } else if (mortar_smooth == 0.0f) {
        mortar = 1.0f;
    } else {
        min_dist = 1.0f - min_dist / mortar_size;
        mortar = min_dist < mortar_smooth ? kstep_smooth(0.0f, mortar_smooth, min_dist) : 1.0f;
    }
    return make_float2(tint, mortar);
}

// --- Magic (Cycles' svm_magic) -------------------------------------------------------------------------------

KFN vec3 tex_magic(vec3 p, int n, float distortion) {
    // sin / cos repeat every 2 pi, so the coordinates can be wrapped first (Cycles: no precision loss far away)
    p = v3(kfmod(p.x, K_2PI), kfmod(p.y, K_2PI), kfmod(p.z, K_2PI));
    float x = sinf((p.x + p.y + p.z) * 5.0f);
    float y = cosf((-p.x + p.y - p.z) * 5.0f);
    float z = -cosf((-p.x - p.y + p.z) * 5.0f);
    if (n > 0) {
        x *= distortion;
        y *= distortion;
        z *= distortion;
        y = -cosf(x - y + z);
        y *= distortion;
        if (n > 1) {
            x = cosf(x - y - z);
            x *= distortion;
            if (n > 2) {
                z = sinf(-x - y - z);
                z *= distortion;
                if (n > 3) {
                    x = -cosf(-x + y - z);
                    x *= distortion;
                    if (n > 4) {
                        y = -sinf(-x + y + z);
                        y *= distortion;
                        if (n > 5) {
                            y = -cosf(-x + y + z);
                            y *= distortion;
                            if (n > 6) {
                                x = cosf(x + y + z);
                                x *= distortion;
                                if (n > 7) {
                                    z = sinf(x + y - z);
                                    z *= distortion;
                                    if (n > 8) {
                                        x = -cosf(-x - y + z);
                                        x *= distortion;
                                        if (n > 9) {
                                            y = -sinf(x - y + z);
                                            y *= distortion;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (distortion != 0.0f) {
        distortion *= 2.0f;
        x /= distortion;
        y /= distortion;
        z /= distortion;
    }
    return v3(0.5f - x, 0.5f - y, 0.5f - z);
}

// --- Weave: woven cloth seen from above (warp threads along Y, weft along X) ----------------------------------

// 1 where the warp thread lies over the weft at crossing (i, j)
KFN float weave_warp_up(int pattern, int i, int j) {
    kuint ui = KUINT(i + 4096), uj = KUINT(j + 4096); // the textures repeat long before: positive indices
    if (pattern == WEAVE_TWILL) return ((ui + uj) % 4u) < 2u ? 1.0f : 0.0f; // 2/2 twill: diagonal ribs
    if (pattern == WEAVE_SATIN) return ((ui * 2u + uj) % 5u) != 0u ? 1.0f : 0.0f; // 5-harness: long warp floats
    if (pattern == WEAVE_BASKET) return ((ui / 2u + uj / 2u) % 2u) != 0u ? 1.0f : 0.0f;
    return ((ui + uj) % 2u) != 0u ? 1.0f : 0.0f;
}

// Elevation of the thread at a crossing, blended towards the next crossing near the cell's edge (the thread bends)
KFN float weave_elevation(float up, float up_next, float t) {
    float b = ksmoothstep(0.25f, 1.0f, fabsf(t - 0.5f) * 2.0f) * 0.5f;
    return kmix(up, up_next, b);
}

KSTRUCT(WeaveResult)
struct WeaveResult {
    vec3 color;
    float height;
    float warp;
};

KFN WeaveResult tex_weave(vec3 p, int pattern, float scale, float width, vec3 warp_color, vec3 weft_color, float fibers,
                          float randomness) {
    float px = p.x * scale, py = p.y * scale;
    float fi = floorf(px), fj = floorf(py);
    int i = KINT(fi), j = KINT(fj);
    float fx = px - fi, fy = py - fj;
    width = clampf(width, 0.05f, 1.0f);
    float up = weave_warp_up(pattern, i, j);
    // the warp thread (column i) bends towards its crossing above or below, the weft (row j) towards left or right
    int jn = fy < 0.5f ? j - 1 : j + 1, in_ = fx < 0.5f ? i - 1 : i + 1;
    float ew = weave_elevation(up, weave_warp_up(pattern, i, jn), fy);
    float ef = weave_elevation(1.0f - up, 1.0f - weave_warp_up(pattern, in_, j), fx);
    // round cross sections
    float aw = (fx - 0.5f) * 2.0f / width, af = (fy - 0.5f) * 2.0f / width;
    float sw = ksafe_sqrt(1.0f - aw * aw), sf = ksafe_sqrt(1.0f - af * af);
    // twisted fibers: slanted stripes along each thread
    float twist_w = 0.5f + 0.5f * sinf(K_2PI * (fy * 3.0f + fx * 1.5f));
    float twist_f = 0.5f + 0.5f * sinf(K_2PI * (fx * 3.0f - fy * 1.5f));
    float hw = sw * (0.45f + 0.55f * ew) * (1.0f - 0.25f * fibers * (1.0f - twist_w));
    float hf = sf * (0.45f + 0.55f * ef) * (1.0f - 0.25f * fibers * (1.0f - twist_f));
    WeaveResult r;
    r.warp = hw >= hf ? 1.0f : 0.0f;
    r.height = fmaxf(hw, hf);
    // each thread its own shade
    float rnd = r.warp > 0.5f ? hash_float2_to_float(KFLOAT(i), 17.0f) : hash_float2_to_float(KFLOAT(j), 31.0f);
    vec3 c = r.warp > 0.5f ? warp_color : weft_color;
    float shade = (1.0f + randomness * (rnd - 0.5f)) * (0.55f + 0.45f * r.height);
    r.color = v3_clamp_min(v3_scale(c, shade), 0.0f);
    return r;
}

// A vector of three signed noises in one loop (one copy of the noise code; `axes` is 3 at run time, see cells_offset)
KFN vec3 sim_noise3(vec3 p, float seed, int axes) {
    vec3 r = v3_zero();
    for (int a = 0; a < axes; a++) {
        float n = snoise_3d(v3_add(p, random_float3_offset(seed + KFLOAT(a))));
        if (a == 0) r.x = n;
        else if (a == 1) r.y = n;
        else r.z = n;
    }
    return r;
}

// fBm in [0, 1] with the octave count at run time (noise_fbm's constant loop is unrolled 16 times on the GPU)
KFN float sim_fbm(vec3 p, float detail, float roughness) {
    detail = clampf(detail, 0.0f, 15.0f);
    int n = KINT(detail);
    float rmd = detail - KFLOAT(n), fscale = 1.0f, amp = 1.0f, sum = 0.0f, maxamp = 0.0f;
    int count = n + (rmd > 0.0f ? 2 : 1);
    for (int i = 0; i < count; i++) {
        float w = i > n ? rmd : 1.0f;
        sum += snoise_3d(v3_scale(p, fscale)) * amp * w;
        maxamp += amp * w;
        amp *= fmaxf(roughness, 0.0f);
        fscale *= 2.0f;
    }
    return maxamp > 0.0f ? 0.5f * sum / maxamp + 0.5f : 0.5f;
}

// --- Cells: dried-mud cracks, cobblestones and sand grains --------------------------------------------------

KSTRUCT(CellsResult)
struct CellsResult {
    float gap;    // in the crack / the mortar / between grains
    float height; // the plate, stone or grain
    float random; // per cell
    float dist;   // to the cell's edge (cracks, stones) or to its point (grains), in cell units
    vec3 rvec;    // per cell, components in [-1, 1] (a grain's facet tilt, a stone's color)
};

// The closest feature point (relative to p's cell), its cell offset, and the distance to the nearest Voronoi edge (Inigo
// Quilez's two-pass edge distance) unless grains only need the distance to the point.
// The 27 cells around c + o0, offset q (0..26) as (i, j, k) in -1..1. `cells` is 27 at run time (the instruction's p1
// adds 0): with a constant count the GPU compiler unrolls the loops (54 hash copies per octave in every copy of the
// interpreter) and the extended pipeline took twice as long to compile.
KFN vec3 cells_offset(int q) { return v3(KFLOAT(q % 3 - 1), KFLOAT((q / 3) % 3 - 1), KFLOAT(q / 9 - 1)); }

// One octave: the closest feature point (relative to p), its cell, and the distance to the nearest Voronoi edge (Inigo
// Quilez's two-pass edge distance) unless grains only need the distance to the point.
KFN CellsResult cells_octave(vec3 p, int mode, float width, float randomness, float roundness, int cells) {
    vec3 c = v3(floorf(p.x), floorf(p.y), floorf(p.z));
    vec3 f = v3_sub(p, c);
    vec3 mr = v3_zero(), mo = v3_zero();
    float md2 = 1e10f;
    for (int q = 0; q < cells; q++) {
        vec3 o = cells_offset(q);
        vec3 r = v3_sub(v3_add(o, v3_scale(hash_float3_to_float3(v3_add(c, o)), randomness)), f);
        float d = v3_dot(r, r);
        if (d < md2) {
            md2 = d;
            mr = r;
            mo = o;
        }
    }
    CellsResult res;
    vec3 cell = v3_add(c, mo);
    res.random = hash_float3_to_float(cell);
    res.rvec = v3_sub(v3_scale(hash_float3_to_float3(v3_add(cell, v3(17.31f, 5.17f, 3.73f))), 2.0f), v3_one());
    if (mode == CELLS_GRAINS) {
        float radius = fmaxf(0.5f * width, 1e-3f) * (0.75f + 0.25f * hash_float3_to_float(v3_add(cell, v3_splat(9.1f))));
        float x = sqrtf(md2) / radius;
        res.dist = sqrtf(md2);
        res.height = ksafe_sqrt(1.0f - x * x);
        res.gap = 1.0f - ksmoothstep(0.0f, 0.15f, res.height);
        return res;
    }
    float md = 8.0f;
    for (int q = 0; q < cells; q++) {
        vec3 o = v3_add(mo, cells_offset(q));
        vec3 r = v3_sub(v3_add(o, v3_scale(hash_float3_to_float3(v3_add(c, o)), randomness)), f);
        vec3 dr = v3_sub(r, mr);
        if (v3_dot(dr, dr) > 1e-8f) md = fminf(md, v3_dot(v3_scale(v3_add(mr, r), 0.5f), v3_norm(dr)));
    }
    md = fmaxf(md, 0.0f);
    float w = fmaxf(0.5f * width, 0.0f);
    res.dist = md;
    if (mode == CELLS_STONES) {
        res.gap = 1.0f - ksmoothstep(w * 0.7f, w + 1e-4f, md);
        float x = ksaturate((md - w) / fmaxf(roundness * 0.45f, 1e-3f));
        res.height = ksafe_sqrt(1.0f - ksqr(1.0f - x)); // a quarter circle: rounded edges, a flat top
    } else {
        res.gap = 1.0f - ksmoothstep(w * 0.5f, w + 1e-4f, md);
        res.height = ksmoothstep(w, w + 0.3f, md) * (1.0f - 0.25f * roundness * ksmoothstep(0.15f, 0.5f, md));
    }
    return res;
}

// detail adds a finer level of cracks (2.3 times smaller) inside the plates: the same octave code run twice
KFN CellsResult tex_cells(vec3 p, int mode, float scale, float width, float distortion, float detail, float randomness,
                          float roundness, int cells) {
    p = v3_scale(p, scale);
    if (distortion != 0.0f) {
        p = v3_add(p, v3_scale(sim_noise3(v3_scale(p, 0.5f), 5.0f, cells - 24), distortion));
    }
    randomness = ksaturate(randomness);
    int octaves = mode == CELLS_CRACKS && detail > 0.0f ? 2 : 1;
    CellsResult r;
    r.gap = r.height = r.random = r.dist = 0.0f;
    r.rvec = v3_zero();
    for (int oct = 0; oct < octaves; oct++) {
        bool fine = oct == 1;
        CellsResult d = cells_octave(fine ? v3_add(v3_scale(p, 2.3f), v3_splat(11.7f)) : p, mode, fine ? width * 1.3f : width,
                                     randomness, roundness, cells);
        if (!fine) {
            r = d;
        } else {
            float g = d.gap * ksaturate(detail);
            r.gap = fmaxf(r.gap, g);
            r.height = r.height * (1.0f - g * 0.6f);
        }
    }
    return r;
}

// --- Ocean: a sum of sharp-crested directional waves (Gerstner-like), tileable on request ------------------------

KFN float2 tex_ocean(vec3 p, float scale, float direction, float spread, float choppiness, float waves, float time,
                     bool tile) {
    int n = kmini(kmaxi(KINT(waves + 0.5f), 1), 16);
    float base = 1.0f / fmaxf(fabsf(scale), 1e-4f); // the longest wavelength
    float sharp = 1.0f + 3.0f * ksaturate(choppiness);
    float h = 0.0f, foam = 0.0f, amp_sum = 0.0f, lambda = base;
    for (int k = 0; k < n; k++) { // n at run time: not unrolled
        float th = direction + K_PI * ksaturate(spread) * (hash_float2_to_float(KFLOAT(k), 3.0f) - 0.5f);
        float kl = K_2PI / lambda;
        float kx = cosf(th) * kl, ky = sinf(th) * kl;
        if (tile) { // integer cycles over the tile of size `base`
            float qx = floorf(kx * base / K_2PI + 0.5f), qy = floorf(ky * base / K_2PI + 0.5f);
            if (qx == 0.0f && qy == 0.0f) qx = 1.0f;
            kx = qx * K_2PI / base;
            ky = qy * K_2PI / base;
            kl = sqrtf(kx * kx + ky * ky);
        }
        float omega = sqrtf(9.81f * kl);
        float phase = kx * p.x + ky * p.y - omega * time + K_2PI * hash_float2_to_float(KFLOAT(k), 7.0f);
        float c = 0.5f + 0.5f * sinf(phase);
        float amp = lambda;
        h += amp * (2.0f * powf(c, sharp) - 1.0f);
        foam += amp * powf(c, 4.0f * sharp);
        amp_sum += amp;
        lambda *= 0.62f;
    }
    return make_float2(0.5f + 0.5f * h / amp_sum, foam / amp_sum);
}

// --- Fibers: noise stretched along an axis (wood, bark, hair-like fuzz) ---------------------------------------

KFN float2 tex_fibers(vec3 p, int axis, float scale, float stretch, float detail, float roughness, float distortion, int axes) {
    p = v3_scale(p, scale);
    float s = 1.0f / fmaxf(stretch, 1e-3f);
    if (axis == 0) p.x *= s;
    else if (axis == 1) p.y *= s;
    else p.z *= s;
    if (distortion != 0.0f)
        p = v3_add(p, v3_scale(sim_noise3(p, 8.0f, axes), distortion));
    float n = sim_fbm(p, detail, roughness);
    float ridge = 1.0f - fabsf(2.0f * n - 1.0f);
    return make_float2(n, ridge * ridge);
}

#endif
