// The materials' data both the bake and the main pass read (material.c): the GpuMaterial slots and the bindless set
// of textures (set MAT_SET, defined by the includer).
#ifndef MAT_COMMON_GLSL
#define MAT_COMMON_GLSL
#extension GL_EXT_nonuniform_qualifier : require
#include "mat_ops.h"

layout(set = MAT_SET, binding = 0) uniform sampler2D u_mat_tex[]; // sources (Image Texture) and baked textures

struct GpuMaterial {
    uint tex_base, tex_nrm, tex_emis, flags;
    uint prog_off, prog_len, param_off, nparams;
    uvec4 anim; // an animated material (a Time node): x its frames (0: still), y its loop (float bits), z its frames' table
    vec4 avg; // xyz: the mean base color, w: the uv scale
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer GpuMaterialBuf { GpuMaterial m[]; };
// The Principled BSDF's extra lobes (M1b, material_gpu.inc principled_ext; MATF_EXT): a table after the GpuMaterials (the
// struct stays 64 bytes: a bigger one loaded whole made the 128-register mesh_rt spill). The node's constants:
//   e0 coat weight, coat roughness (alpha), coat F0, anisotropy (0..1)   e1 coat tint rgb, anisotropic rotation (rad)
//   e2 the dielectric F0 rgb (IOR, Specular IOR Level and Tint), transmission   e3 the subsurface wrap rgb, its weight
#define MAT_EXT_BASE 4096 // (material.c MAT_GPU_SLOTS)
//   e4 sheen weight, sheen roughness, thin film thickness (nm), thin film IOR   e5 sheen tint rgb, unused (M3, M4)
//   e6 (M5) the detail normal's material slot (a float value; 0: none), its tiling, its strength; w: MATX_ flags (a float value)
//   e7 (M7) the parallax's depth (uv units), its most steps (the Eye, A9.3c: the iris's radius in UV, its depth in cm); (A9.3a) z the subsurface's widest scattering distance (cm),
//      w the channels' shares of it (8 bits each, r << 16 | g << 8 | b, a float value): sss.comp's Burley profile
//   e8-e11 (2026-11-16, MATX_WET: the Wet Surface node) e8 wetness, porosity (< 0: by the roughness), water film, its
//      roughness (alpha); e9 droplets, droplet size (mm), running, sweat; e10 the parameters' indices + 1 (wetness, film,
//      droplets, sweat; 0: the constant) as float values; e11.x the mask (MAT_WET_MASK_)
struct GpuMatExt { vec4 e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12; }; // (e6.w: MATX_, mat_ops.h; e12: Q27.8's flakes)
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer GpuMatExtBuf { GpuMaterial m[MAT_EXT_BASE]; GpuMatExt x[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MatFrameBuf { uvec4 t[]; }; // base, normal, emission

// (Q18) A UDIM material's tile under the uv (MATF_UDIM, material_udim.inc): its textures (base, normal / roughness /
// metallic, emission) from its table in the parameters (row anim.w + 1 + column + 10 x row; 0: a tile it doesn't have,
// its first tile's), and the uv inside it. The engine's V runs down: tile 1011 is v -1..0
uvec4 udim_tile(uvec2 params, GpuMaterial gm, inout vec2 uv) {
    vec2 tl = floor(uv);
    uvec4 t = uvec4(gm.tex_base, gm.tex_nrm, gm.tex_emis, 0u);
    uint tx = uint(int(tl.x)), ty = uint(-int(tl.y));
    if (tx < 10u && ty < 10u) {
        uvec4 r = MatFrameBuf(params).t[gm.anim.w + 1u + tx + ty * 10u];
        if (r.x != 0u) t = r;
    }
    uv -= tl;
    return t;
}

vec3 srgb_to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }

// The Masked cut as MSAA coverage (alpha to coverage, sharpened): the alpha's edge a pixel wide (fwidth), its share of
// the n samples kept, so the resolve smooths the cut like a mesh's edge (a whole pixel cut or kept was the hair's and
// the torn cloth's stair steps at 8x MSAA). rot turns which samples (per pixel and instance: two cards over one
// another keep different ones and their layers mix). 0: none (discard); n = 1: the plain cut at the clip value.
// (fw: the caller's fwidth(a), where the quad is uniform like the texture read; this file is in compute shaders too)
uint alpha_coverage(float a, float fw, float clip, uint n, uint rot) {
    float c = clamp((a - clip) / max(fw, 1e-4) + 0.5, 0.0, 1.0);
    uint k = uint(c * float(n) + 0.5);
    uint full = n >= 32u ? 0xFFFFFFFFu : (1u << n) - 1u;
    if (k >= n) return full;
    uint m = (1u << k) - 1u, r = rot % n;
    return r == 0u ? m : ((m << r) | (m >> (n - r))) & full;
}
#endif
