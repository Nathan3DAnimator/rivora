// Eight floats at once for the rigid bodies' solver (physics.c, notas-dev/fisica.md "20 mil corpos"): AVX2 (+FMA) on
// x64 (/arch:AVX2), a plain struct of 8 elsewhere (the Android game: the compiler vectorizes its loops with NEON).
// fw: 8 lanes; a mask is an fw with every bit of a lane set (the comparisons' result).
#pragma once
#include <stdint.h>
#include <string.h>

#define FW_LANES 8

#if defined(__AVX2__)
#include <immintrin.h>
typedef __m256 fw;
static inline fw fw_load(const float* p) { return _mm256_load_ps(p); }
static inline void fw_store(float* p, fw v) { _mm256_store_ps(p, v); }
static inline fw fw_set(float f) { return _mm256_set1_ps(f); }
static inline fw fw_zero(void) { return _mm256_setzero_ps(); }
static inline fw fw_add(fw a, fw b) { return _mm256_add_ps(a, b); }
static inline fw fw_sub(fw a, fw b) { return _mm256_sub_ps(a, b); }
static inline fw fw_mul(fw a, fw b) { return _mm256_mul_ps(a, b); }
static inline fw fw_div(fw a, fw b) { return _mm256_div_ps(a, b); }
static inline fw fw_madd(fw a, fw b, fw c) { return _mm256_fmadd_ps(a, b, c); }  // a b + c
static inline fw fw_nmadd(fw a, fw b, fw c) { return _mm256_fnmadd_ps(a, b, c); } // c - a b
static inline fw fw_max(fw a, fw b) { return _mm256_max_ps(a, b); }
static inline fw fw_min(fw a, fw b) { return _mm256_min_ps(a, b); }
static inline fw fw_sqrt(fw a) { return _mm256_sqrt_ps(a); }
static inline fw fw_gt(fw a, fw b) { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
static inline fw fw_le(fw a, fw b) { return _mm256_cmp_ps(a, b, _CMP_LE_OQ); }
static inline fw fw_and(fw a, fw b) { return _mm256_and_ps(a, b); }
static inline fw fw_or(fw a, fw b) { return _mm256_or_ps(a, b); }
static inline fw fw_andnot(fw m, fw a) { return _mm256_andnot_ps(m, a); } // a where m is not set
static inline fw fw_sel(fw m, fw a, fw b) { return _mm256_blendv_ps(b, a, m); } // m ? a : b
static inline int fw_bits(fw m) { return _mm256_movemask_ps(m); }
static inline fw fw_all(int on) { return _mm256_castsi256_ps(_mm256_set1_epi32(on ? -1 : 0)); }
static inline int fw_nonzero_bits(fw a) { // the lanes whose bits aren't all zero (an integer in a float lane)
    return ~_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_castps_si256(a), _mm256_setzero_si256()))) & 0xFF;
}
// 8 rows of 8 floats (each 32-byte aligned) <-> 8 columns
static inline void fw_transpose(fw r[8]) {
    __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]), t1 = _mm256_unpackhi_ps(r[0], r[1]), t2 = _mm256_unpacklo_ps(r[2], r[3]),
           t3 = _mm256_unpackhi_ps(r[2], r[3]), t4 = _mm256_unpacklo_ps(r[4], r[5]), t5 = _mm256_unpackhi_ps(r[4], r[5]),
           t6 = _mm256_unpacklo_ps(r[6], r[7]), t7 = _mm256_unpackhi_ps(r[6], r[7]);
    __m256 s0 = _mm256_shuffle_ps(t0, t2, 0x44), s1 = _mm256_shuffle_ps(t0, t2, 0xEE), s2 = _mm256_shuffle_ps(t1, t3, 0x44),
           s3 = _mm256_shuffle_ps(t1, t3, 0xEE), s4 = _mm256_shuffle_ps(t4, t6, 0x44), s5 = _mm256_shuffle_ps(t4, t6, 0xEE),
           s6 = _mm256_shuffle_ps(t5, t7, 0x44), s7 = _mm256_shuffle_ps(t5, t7, 0xEE);
    r[0] = _mm256_permute2f128_ps(s0, s4, 0x20), r[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
    r[2] = _mm256_permute2f128_ps(s2, s6, 0x20), r[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
    r[4] = _mm256_permute2f128_ps(s0, s4, 0x31), r[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
    r[6] = _mm256_permute2f128_ps(s2, s6, 0x31), r[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
}
// the rows at base + idx[k] * stride bytes (32 bytes each, aligned) as 8 columns; and back (only the lanes in bits)
static inline void fw_gather8(const void* base, const int32_t idx[8], size_t stride, fw out[8]) {
    const char* b = (const char*)base;
    for (int k = 0; k < 8; k++) out[k] = _mm256_load_ps((const float*)(b + (size_t)idx[k] * stride));
    fw_transpose(out);
}
static inline void fw_scatter8(void* base, const int32_t idx[8], size_t stride, fw cols[8], int bits) {
    char* b = (char*)base;
    fw_transpose(cols);
    for (int k = 0; k < 8; k++)
        if (bits & (1 << k)) _mm256_store_ps((float*)(b + (size_t)idx[k] * stride), cols[k]);
}
#else
typedef struct fw { float f[FW_LANES]; } fw;
#define FW_OP2(name, expr)                                                                                                     \
    static inline fw name(fw a, fw b) {                                                                                        \
        fw o;                                                                                                                  \
        for (int k = 0; k < FW_LANES; k++) o.f[k] = (expr);                                                                    \
        return o;                                                                                                              \
    }
static inline fw fw_load(const float* p) { fw o; memcpy(o.f, p, sizeof o.f); return o; }
static inline void fw_store(float* p, fw v) { memcpy(p, v.f, sizeof v.f); }
static inline fw fw_set(float f) { fw o; for (int k = 0; k < FW_LANES; k++) o.f[k] = f; return o; }
static inline fw fw_zero(void) { return fw_set(0.0f); }
static inline uint32_t fw_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float fw_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
FW_OP2(fw_add, a.f[k] + b.f[k])
FW_OP2(fw_sub, a.f[k] - b.f[k])
FW_OP2(fw_mul, a.f[k] * b.f[k])
FW_OP2(fw_div, a.f[k] / b.f[k])
FW_OP2(fw_max, a.f[k] > b.f[k] ? a.f[k] : b.f[k])
FW_OP2(fw_min, a.f[k] < b.f[k] ? a.f[k] : b.f[k])
FW_OP2(fw_gt, fw_f(a.f[k] > b.f[k] ? ~0u : 0u))
FW_OP2(fw_le, fw_f(a.f[k] <= b.f[k] ? ~0u : 0u))
FW_OP2(fw_and, fw_f(fw_u(a.f[k]) & fw_u(b.f[k])))
FW_OP2(fw_or, fw_f(fw_u(a.f[k]) | fw_u(b.f[k])))
FW_OP2(fw_andnot, fw_f(~fw_u(a.f[k]) & fw_u(b.f[k])))
static inline fw fw_madd(fw a, fw b, fw c) { fw o; for (int k = 0; k < FW_LANES; k++) o.f[k] = a.f[k] * b.f[k] + c.f[k]; return o; }
static inline fw fw_nmadd(fw a, fw b, fw c) { fw o; for (int k = 0; k < FW_LANES; k++) o.f[k] = c.f[k] - a.f[k] * b.f[k]; return o; }
static inline fw fw_sqrt(fw a) { fw o; for (int k = 0; k < FW_LANES; k++) o.f[k] = a.f[k] > 0.0f ? __builtin_sqrtf(a.f[k]) : 0.0f; return o; }
static inline fw fw_sel(fw m, fw a, fw b) { fw o; for (int k = 0; k < FW_LANES; k++) o.f[k] = fw_u(m.f[k]) ? a.f[k] : b.f[k]; return o; }
static inline int fw_bits(fw m) { int b = 0; for (int k = 0; k < FW_LANES; k++) b |= (fw_u(m.f[k]) >> 31) << k; return b; }
static inline fw fw_all(int on) { return fw_set(fw_f(on ? ~0u : 0u)); }
static inline int fw_nonzero_bits(fw a) { int b = 0; for (int k = 0; k < FW_LANES; k++) b |= (fw_u(a.f[k]) != 0u) << k; return b; }
static inline void fw_gather8(const void* base, const int32_t idx[8], size_t stride, fw out[8]) {
    const char* b = (const char*)base;
    for (int k = 0; k < 8; k++) {
        const float* row = (const float*)(b + (size_t)idx[k] * stride);
        for (int c = 0; c < 8; c++) out[c].f[k] = row[c];
    }
}
static inline void fw_scatter8(void* base, const int32_t idx[8], size_t stride, fw cols[8], int bits) {
    char* b = (char*)base;
    for (int k = 0; k < 8; k++) {
        if (!(bits & (1 << k))) continue;
        float* row = (float*)(b + (size_t)idx[k] * stride);
        for (int c = 0; c < 8; c++) row[c] = cols[c].f[k];
    }
}
#undef FW_OP2
#endif
