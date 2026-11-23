// Common dialect of C11 and GLSL 4.60 for the shading kernel.
//
// Everything under src/kernel/ is compiled twice: by the C compiler for the CPU renderer
// (render.c) and by glslc for the GPU renderer (shaders/pt.comp, which defines KERNEL_GLSL).
// The code therefore sticks to what both languages share:
//   - scalar math with the C names (sqrtf, fmaxf, ...; mapped to GLSL built-ins below);
//   - vec3 math through the math3d.h functions (v3_add, v3_dot, ...; operators in GLSL);
//   - structs passed and returned by value, no pointers, no out parameters, no recursion;
//   - KFN in front of every function, KSTRUCT(Name) before every struct definition;
//   - explicit conversions with KFLOAT / KINT / KUINT;
//   - no `const` locals (GLSL rejects non-constant initializers in some versions).
// Data that lives in buffers is read through a handful of functions each side provides
// (see kernel_globals.h), which take the KernelGlobals handle `kg` (KG_ARG / kg).
#ifndef KERNEL_COMPAT_H
#define KERNEL_COMPAT_H

#ifdef KERNEL_GLSL

#define KFN
#define KPRECISE precise // no contraction: exact float rounding where the result is compared (the checker)
#define KSTRUCT(name)
#define kuint uint
#define float2 vec2
#define make_float2(a, b) vec2(a, b)
#define KFLOAT(x) float(x)
#define KINT(x) int(x)
#define KUINT(x) uint(x)
#define KG_ARG int kg
#define kfloat_as_int(x) floatBitsToInt(x)
#define kint_as_float(x) intBitsToFloat(x)
#define kfloat_as_uint(x) floatBitsToUint(x)
#define kreverse_bits(x) bitfieldReverse(x)

#define sqrtf sqrt
#define sinf sin
#define cosf cos
#define tanf tan
#define asinf asin
#define acosf acos
#define atanf atan
#define atan2f atan
#define expf exp
#define logf log
#define log2f log2
#define exp2f exp2
#define powf pow
#define fabsf abs
#define fmaxf max
#define fminf min
#define floorf floor
#define ceilf ceil
#define truncf trunc
#define sinhf sinh
#define coshf cosh
#define tanhf tanh
#define clampf clamp

#define v3(x, y, z) vec3(x, y, z)
#define v3_add(a, b) ((a) + (b))
#define v3_sub(a, b) ((a) - (b))
#define v3_scale(a, s) ((a) * (s))
#define v3_mul(a, b) ((a) * (b))
#define v3_dot(a, b) dot(a, b)
#define v3_cross(a, b) cross(a, b)
#define v3_len(a) length(a)
#define v3_min(a, b) min(a, b)
#define v3_max(a, b) max(a, b)
#define v3_div(a, b) ((a) / (b))
#define v3_mix(a, b, t) mix(a, b, t)
#define v3_exp(a) exp(a)
#define v3_sqrt(a) sqrt(a)
#define v3_pow(a, e) pow(a, vec3(e))
#define v3_get(a, i) ((a)[i])

vec3 v3_norm(vec3 a) {
    float l = length(a);
    return l > 1e-12f ? a / l : vec3(0.0f);
}
float copysignf(float a, float b) { return b < 0.0f ? -abs(a) : abs(a); }

#else // C

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../math3d.h"

#define KFN static inline
#define KPRECISE
#define KSTRUCT(name) typedef struct name name;
typedef uint32_t kuint;
typedef struct float2 {
    float x, y;
} float2;
static inline float2 make_float2(float a, float b) {
    float2 r = { a, b };
    return r;
}
#define KFLOAT(x) ((float)(x))
#define KINT(x) ((int)(x))
#define KUINT(x) ((kuint)(x))

typedef struct KernelGlobals KernelGlobals; // defined by render.c
#define KG_ARG const KernelGlobals* kg

static inline int kfloat_as_int(float f) {
    int i;
    memcpy(&i, &f, sizeof i);
    return i;
}
static inline float kint_as_float(int i) {
    float f;
    memcpy(&f, &i, sizeof f);
    return f;
}
static inline kuint kfloat_as_uint(float f) {
    kuint u;
    memcpy(&u, &f, sizeof u);
    return u;
}
static inline kuint kreverse_bits(kuint x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x >> 8) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8);
    return (x >> 16) | (x << 16);
}

static inline vec3 v3_div(vec3 a, vec3 b) { return v3(a.x / b.x, a.y / b.y, a.z / b.z); }
static inline vec3 v3_mix(vec3 a, vec3 b, float t) {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}
static inline vec3 v3_exp(vec3 a) { return v3(expf(a.x), expf(a.y), expf(a.z)); }
static inline vec3 v3_sqrt(vec3 a) { return v3(sqrtf(a.x), sqrtf(a.y), sqrtf(a.z)); }
static inline vec3 v3_pow(vec3 a, float e) { return v3(powf(a.x, e), powf(a.y, e), powf(a.z, e)); }

#endif

// --- Shared helpers (both languages) -----------------------------------------------

#define K_PI 3.14159265358979f
#define K_2PI 6.28318530717959f
#define K_1_PI 0.318309886183791f
#define K_INF 1e30f

KFN float ksqr(float x) { return x * x; }
KFN float ksaturate(float x) { return clampf(x, 0.0f, 1.0f); }
KFN float kmix(float a, float b, float t) { return a + (b - a) * t; }
KFN float ksmoothstep(float e0, float e1, float x) {
    float t = ksaturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
KFN float ksafe_sqrt(float x) { return sqrtf(fmaxf(x, 0.0f)); }
KFN float ksafe_acos(float x) { return acosf(clampf(x, -1.0f, 1.0f)); }
KFN float kinverse_lerp(float a, float b, float x) { return (x - a) / (b - a); }
KFN int kmini(int a, int b) { return a < b ? a : b; }
KFN int kmaxi(int a, int b) { return a > b ? a : b; }

KFN vec3 v3_splat(float s) { return v3(s, s, s); }
KFN vec3 v3_zero() { return v3(0.0f, 0.0f, 0.0f); }
KFN vec3 v3_one() { return v3(1.0f, 1.0f, 1.0f); }
KFN vec3 v3_neg(vec3 a) { return v3_scale(a, -1.0f); }
KFN vec3 v3_madd(vec3 a, vec3 b, float s) { return v3_add(a, v3_scale(b, s)); } // a + b * s
KFN float v3_max_comp(vec3 a) { return fmaxf(a.x, fmaxf(a.y, a.z)); }
KFN float v3_avg(vec3 a) { return (a.x + a.y + a.z) * (1.0f / 3.0f); }
KFN float v3_sum(vec3 a) { return a.x + a.y + a.z; }
KFN float v3_lum(vec3 a) { return 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z; } // Rec.709
KFN bool v3_is_zero(vec3 a) { return a.x == 0.0f && a.y == 0.0f && a.z == 0.0f; }
KFN vec3 v3_abs(vec3 a) { return v3(fabsf(a.x), fabsf(a.y), fabsf(a.z)); }
KFN vec3 v3_saturate(vec3 a) { return v3(ksaturate(a.x), ksaturate(a.y), ksaturate(a.z)); }
KFN vec3 v3_clamp_min(vec3 a, float m) { return v3(fmaxf(a.x, m), fmaxf(a.y, m), fmaxf(a.z, m)); }
KFN vec3 v3_safe_div(vec3 a, vec3 b) {
    return v3(b.x != 0.0f ? a.x / b.x : 0.0f, b.y != 0.0f ? a.y / b.y : 0.0f, b.z != 0.0f ? a.z / b.z : 0.0f);
}
KFN vec3 v3_reflect(vec3 i, vec3 n) { return v3_sub(v3_scale(n, 2.0f * v3_dot(i, n)), i); } // i points away
KFN bool v3_is_finite(vec3 a) {
    return a.x == a.x && a.y == a.y && a.z == a.z && fabsf(a.x) < 1e30f && fabsf(a.y) < 1e30f && fabsf(a.z) < 1e30f;
}

// Orthonormal basis around a unit vector (Duff et al. 2017).
KSTRUCT(Frame)
struct Frame {
    vec3 t, b, n;
};
KFN Frame frame_from_normal(vec3 n) {
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float bb = n.x * n.y * a;
    Frame f;
    f.t = v3(1.0f + s * n.x * n.x * a, s * bb, -s * n.x);
    f.b = v3(bb, s + n.y * n.y * a, -n.y);
    f.n = n;
    return f;
}
// Frame with its tangent as close as possible to `t` (anisotropic lobes).
KFN Frame frame_from_normal_tangent(vec3 n, vec3 t) {
    vec3 tt = v3_sub(t, v3_scale(n, v3_dot(t, n)));
    if (v3_dot(tt, tt) < 1e-12f) return frame_from_normal(n);
    Frame f;
    f.n = n;
    f.t = v3_norm(tt);
    f.b = v3_cross(n, f.t);
    return f;
}
KFN vec3 frame_to_local(Frame f, vec3 v) { return v3(v3_dot(v, f.t), v3_dot(v, f.b), v3_dot(v, f.n)); }
KFN vec3 frame_to_world(Frame f, vec3 v) {
    return v3_add(v3_add(v3_scale(f.t, v.x), v3_scale(f.b, v.y)), v3_scale(f.n, v.z));
}

#endif
