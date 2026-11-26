// (2026-11-25, item 20: "um número digitado fora do limite continua valendo") How a material's lobe values go to the GPU
// (material_gpu.inc, the GpuMatExt floats): the Material Editor's fields (mat_editor.c, ui_number) and the test
// (tests/test_ui_number.c) use the same rules. Only the format's limits hold a value: Eye Glow and Retroreflection are 8 bits
// (x16, 255 at most: their field's top is MAT_GLOW8_MAX, so the screen shows what the GPU gets); the rest is a float, kept
// as typed (no negatives; the flakes' size off 0: a division). Before, the pack held Iris Depth at 2, the Metallic Flakes
// and the Detail Strength at 4, the Dispersion at 1, the Refraction at 4: the fields took more and the GPU never saw it
#pragma once
#include <stdint.h>

#define MAT_GLOW8_MAX (255.0f / 16.0f) // Eye Glow, Retroreflection: their strength x16 in 8 bits (e5.w: the RGB565 color below)
#define MAT_FLAKE_SIZE_MIN 0.002f      // cm: the flakes' cells (mesh.frag flake_glint divides by it)

static inline float mat_pack_nonneg(float v) { return v > 0.0f ? v : 0.0f; } // (a NaN: 0)
static inline uint32_t mat_pack_glow8(float v) { // the 8 bits of a strength x16
    float k = v * 16.0f + 0.5f;
    return k > 255.0f ? 255u : k > 0.0f ? (uint32_t)k : 0u;
}
static inline float mat_pack_iris_depth(float v) { return v >= 0.0f ? v : 0.25f; } // (negative: the default)
static inline float mat_pack_flakes(float v) { return mat_pack_nonneg(v); }
static inline float mat_pack_flake_size(float v) { return v > MAT_FLAKE_SIZE_MIN ? v : MAT_FLAKE_SIZE_MIN; }
static inline float mat_pack_detail(float v) { return mat_pack_nonneg(v); }
static inline float mat_pack_dispersion(float v) { return mat_pack_nonneg(v); }
static inline float mat_pack_refraction(float v) { return mat_pack_nonneg(v); }
