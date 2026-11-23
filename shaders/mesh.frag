#version 460
// Forward PBR: GGX specular (height-correlated Smith, Schlick Fresnel) + Lambert diffuse; directional, point, spot
// and rect lights (closest point on the rectangle); the directional light's shadow map (PCF); the sky light (SH
// irradiance + sky reflection); the GI (gi.c): baked lightmaps on Static meshes, the probes elsewhere, and (compiled
// with RE_RT, on GPUs with ray queries) traced shadows and reflections on smooth surfaces. No temporal history here.
#extension GL_EXT_control_flow_attributes : enable
#define RE_DONT_UNROLL [[dont_unroll]] // (weather.glsl fog_segment's loop)
#include "common.glsl"
#include "gi.glsl"
#include "atmosphere.glsl"
#include "lights.glsl"
#include "weather.glsl"
#define MAT_SET 1
#include "mat_common.glsl"
#include "impostor.glsl" // (Q21) the impostors' cards (their atlases in the materials' set)
#include "toon.glsl" // (Q11) the Toon shading model and the Force Toon lights: only the RE_TOON variants use it
// (Q11) RE_TOON: the RE_EXT variants' twins with the toon's code (render.c takes them once a Toon material or a Force Toon
// light is sent); every other variant is the code it was (the toon's checks in the lights' loop cost the rain scene's
// meshes 0.1 ms, and made mesh_rt_x spill 16 B more)
#if defined(RE_TOON) && defined(RE_EXT)
#define RE_TOON_MAT
#endif
#ifdef RE_RT
#include "rt.glsl" // (after the materials: its shadow rays test a Masked material's alpha)
#endif

#include "vsm.glsl" // the sun's virtual shadow maps (bindings 0, 51)
// the local lights' shadow maps (binding 66: Low, the GPUs without ray tracing): every variant but the plain opaque one,
// which render.c swaps for RE_LSM while a local light has a map (their code took the plain one from 95 registers to 80
// and 48 bytes of local memory: -3% of the pass with no map to read)
#if defined(RE_LSM) || defined(RE_RT) || defined(RE_MASKED) || defined(RE_TRANSLUCENT)
#define RE_LOCAL_SM
#include "lsm.glsl"
#endif
#include "water.glsl" // the rain on the surfaces (water.c)
layout(set = 0, binding = 3) uniform sampler2D u_probe_irr;
layout(set = 0, binding = 4) uniform sampler2D u_probe_dist;
layout(set = 0, binding = 9) uniform sampler2D u_lightmap0; // E / pi for the geometric normal
layout(set = 0, binding = 10) uniform sampler2D u_lightmap1; // the luminance-weighted mean direction (normal maps)
layout(set = 0, binding = 23) uniform sampler2D u_cache_indirect; // the surface cache (gi_cache.c)
layout(location = 3) in vec4 i_uvs; // mesh.vert: UV2, the materials' UV
#define i_uv2 i_uvs.xy
#define i_uv i_uvs.zw
layout(location = 4) flat in float i_ray_bias_s; // mesh.vert: a coarse LOD sits inside the BLAS the rays see; (Q17) negative: displaced
#define i_ray_bias abs(i_ray_bias_s)
#define i_disp_bias max(-i_ray_bias_s, 0.0) // (Q17: how far a displaced surface sinks under the shadow maps' casters, cm)
#ifdef RE_EXT
layout(location = 5) in vec4 i_vcol; // mesh_x.vert: the vertex color (M5), white without
#endif

layout(set = 0, binding = 22) uniform sampler2D u_cache_direct; // the surface cache's direct light (its view)

// The texel grid of an atlas over the surface (the Surface Cache and Lightmap Density views): 0 on a line, 1 inside
float texel_grid(vec2 g) {
    vec2 fw = max(fwidth(g), vec2(1e-5)), d = min(fract(g), 1.0 - fract(g));
    vec2 l = smoothstep(vec2(0.0), fw * 1.2, d);
    return min(l.x, l.y);
}

#ifdef RE_TERMINATOR
// The triangle's three vertices (the ray tracing GPUs all have fragment barycentrics): the position and normal
// interpolated here as the rasterizer would, and the shadow rays' origin lifted onto the curved surface they describe
#extension GL_EXT_fragment_shader_barycentric : require
layout(location = 0) pervertexEXT in vec3 pv_world[3];
layout(location = 1) pervertexEXT in vec3 pv_normal[3];
#define i_world (pv_world[0] * gl_BaryCoordEXT.x + pv_world[1] * gl_BaryCoordEXT.y + pv_world[2] * gl_BaryCoordEXT.z)
#define i_normal (pv_normal[0] * gl_BaryCoordEXT.x + pv_normal[1] * gl_BaryCoordEXT.y + pv_normal[2] * gl_BaryCoordEXT.z)

// Hanika, "Hacking the Shadow Terminator" (Ray Tracing Gems II): the point moved off each vertex's tangent plane by
// what lies below it, weighted by the barycentrics. A flat triangle of a smooth low-poly surface no longer shadows
// itself (the blocks a game character's skin showed under ray traced shadows)
vec3 shadow_terminator_origin(vec3 p, float side) {
    vec3 o = p;
    for (int k = 0; k < 3; k++) {
        vec3 nk = normalize(pv_normal[k]) * side;
        o -= gl_BaryCoordEXT[k] * min(dot(p - pv_world[k], nk), 0.0) * nk;
    }
    return o;
}
#else
layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
#endif
layout(location = 2) flat in uint i_instance_raw; // (Q21: the top bit, an impostor's card)
#define i_instance (i_instance_raw & 0x7FFFFFFFu)
// (only the Masked runs draw the card, cull.comp: the opaque variants keep their code and registers of before)
#ifdef RE_MASKED
#define i_card ((i_instance_raw & 0x80000000u) != 0u)
#else
#define i_card false
#endif
// (Q21) the point lit: i_world, or for an impostor's card the surface it shows, g_pt cm farther along the view ray (one
// float live through the lighting, not three: a vec3 here made the main pass's shaders spill 272 bytes)
float g_pt = 0.0;
#define g_pw (i_world - v * g_pt)
layout(location = 0) out vec4 o_color;
// (2026-11-20) a mirror's reflection ray through the fog (weather.glsl fog_segment), marched at the end where little else
// is live (marched in place, its loop spilled the main pass's registers): x its direction (octahedral),
// y its length and specular weight (halves; 0: none). Two registers: with four the main pass spilled
uvec2 g_fr = uvec2(0u);
// The shadow split (GI_SHADOW_SPLIT, shadow_split.comp): this pixel's shadow-casting local lights left out of o_color,
// their light unshadowed (rgb, pre-exposed and fogged as o_color) and its luminance times the picked lights' raw ray
// traced visibility (a); the compute filters the visibility over the neighbors and adds it. Zero: nothing to add
#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
layout(location = 1) out vec4 o_split; // (only here: the others' writes would go to the target without an image)
// The unified denoiser's (A9.1d, GI_DENOISE: the Photoreal styles; denoise.comp). o_den, what each of its taps reads
// in one load: x, y the noisy signals left out of o_color (the indirect diffuse light, the rough reflection's;
// pre-exposed, common.glsl pack_r11g11b10), z the key (common.glsl den_key: normal, roughness, the actor's hash), w the
// albedo times the air's and fog's transmittance. o_guide, the pixel's own: x the reflection's weight times the
// transmittance, y the actor + 1, z the depth this fragment wrote. Zero: nothing of this pixel's to filter.
// Only in the RE_DEN variants (render.c den_rt): the rest are the same code as before it. Written always, even as zeros,
// the two outputs cost the Realistic main pass 0.2 ms on a 3060; and a specialization constant, its code dead, still
// changed the driver's code enough to move the flicker and light-leak tests (gi_house, spec_leak) under validation
#ifdef RE_DEN
#include "denoise.glsl"
layout(location = 2) out uvec4 o_den;
layout(location = 3) out uvec4 o_guide;
uvec4 g_den = uvec4(0u); // (packed as soon as known: they stay live to the end; z the albedo, w the reflection's weight)
// ReSTIR DI (A9.1a, GI_RESTIR): this pixel's reservoir for the many lights, kept in the guide's w for next frame (light
// slot 16 bits | M 3 bits | W as a half float's top 13 bits); last frame's read from the denoiser's copy of the guide
uint g_resv = 0u;
layout(set = 0, binding = 71, rgba32ui) uniform readonly uimage2D u_den_prev;
#endif
#endif
#ifdef RE_RT
// (packed: the main pass's ray traced shader sits at 128 registers; 4 floats live to the end spilled it)
uvec2 g_split = uvec2(0u); // taken: the light pre-exposed (half rg, half b) and its raw visibility (the second half)
#endif
#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
// Many lights (GI_SHADOW_SPLIT; notas-dev/luz.md "Muitas luzes com sombra"), like Unreal's MegaLights: every shadow-casting local
// light of the pixel shaded without its shadow and summed (g_ml_e), the sum of their luminances (g_ml_w), and two of them
// picked in proportion to their light (weighted reservoir sampling, g_ml_sel: slots). The picked ones' rays give the share
// of the sum that arrives (its expectation is exactly that share: each pick's chance is its share of the luminance); the
// split's filter (shadow_split.comp) smooths it over the neighbors and the TSR over the frames. The rays a pixel traces
// no longer grow with its lights (before: the first 8 traced, the rest lit through the walls)
vec3 g_ml_e = vec3(0.0);
float g_ml_w = 0.0;
uvec2 g_ml_sel = uvec2(0u);
uint g_ml_n = 0u; // the lights summed (one hard one: its exact ray straight into the color, no split; two exact ones
// spilled the shader: 64 bytes, for nothing measurable)
uint g_ml_seed = 0u;
uint ml_hash(uint x) { // (PCG's output permutation: well mixed in 3 multiplies)
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}
#endif
#ifdef RE_TRANSLUCENT
// The OIT pass (render.c, weighted blended OIT, McGuire and Bavoil 2013): the color times its opacity and a weight
// falling with the distance, summed in any order (accum); and the product of the transmittances (revealage, r).
// shadow_split.comp pass 3 puts them over the scene
layout(location = 1) out vec4 o_reveal;
// (2026-11-16) refractive glass: the screen offset (uv) of what is seen through it, its roughness and dispersion, times
// the same weight as the color (shadow_split.comp pass 3 divides by the color's weight)
layout(location = 2) out vec4 o_refr;
float g_opacity = 1.0; // the baked alpha (a Translucent material's opacity)
uint g_mflags = 0u;    // (its material's flags: MATF_REFRACT at the end)
vec3 g_tint = vec3(1.0); // (2026-11-17, Q4) what passes through it keeps this color (its base color by the transmission, the absorption)
#endif
#ifdef RE_MASKED_EQ
// after the prepass (masked.frag cut it): the depth test before the shader, equal to what it kept, nothing to cut here
layout(early_fragment_tests) in;
#endif

float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float v_smith(float nv, float nl, float a) {
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2), gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 f_schlick(vec3 f0, float vh) { return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0); }

// (Q13) the instance's INST_ flags for the Lighting Channels (common.glsl light_channel_ok)
uint g_inst_y = 0u;
#ifdef RE_EXT
// The Principled BSDF's extra lobes (M1b, material_gpu.inc principled_ext): the material's slot + 1 when it has them
// (MATF_EXT; brdf reads its constants from the buffer then), its anisotropy's tangent
// (one register through the lights' loop: the slot + 1 in 12 bits, the tangent octahedral in 2 x 9; the mesh_rt
// variant sits at 128 registers with no spill, a vec3 more made it spill). The top 2 bits: the skin's checkerboard
// (A9.3a, GI_SSS: sss.comp): SSS_D a pixel of only its diffuse light, SSS_S one of the rest (its specular, emission)
uint g_ext = 0u;
#define SSS_D 0x40000000u
#define SSS_S 0x80000000u
#define SSS_BITS 0xC0000000u
vec3 g_f0d = vec3(0.04);
#ifdef RE_TOON_MAT
// (Q11) a Toon material in one register: bits 0-11 its slot + 1 (MATF_TOON: shade_light lights it in bands, toon.glsl);
// for the hatching, the tone of its strongest light so far (bits 12-19) and that light's weight (bits 20-31: its
// luminance to the 1/4, x 200)
uint g_toon = 0u;
#define TOON_SLOT (g_toon & 4095u)
#endif
// (Q13) the Eye's retro-reflection (Eye Glow: an animal's or an animatronic's eyes shining back at a flashlight): its
// strength in the top bits, its color as RGB565 below (material_gpu.inc, e5.w), on the iris only; 0 everywhere else
uint g_retro = 0u;
// (Q27.8) the pixel's world footprint (cm, its derivatives: taken where the flow is uniform), for the flakes
float g_wfw = 1.0;
vec4 g_e12 = vec4(0.0); // (Q27.8) the flakes' (e12), read once a pixel in the lobes' block, not once a light
uint ext_pack(uint slot, vec3 t) {
    vec2 o = t.xy / (abs(t.x) + abs(t.y) + abs(t.z));
    if (t.z < 0.0) o = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    uvec2 q = uvec2(clamp(o * 0.5 + 0.5, 0.0, 1.0) * 511.0 + 0.5);
    return (slot + 1u) | (q.x << 12u) | (q.y << 21u);
}
vec3 ext_tangent(uint e) {
    vec2 o = vec2(float((e >> 12u) & 511u), float((e >> 21u) & 511u)) / 511.0 * 2.0 - 1.0;
    vec3 t = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (t.z < 0.0) t.xy = (1.0 - abs(t.yx)) * vec2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
    return normalize(t);
}
// A thin film's interference (M4, a cheap Belcour-Barla: the optical path's phase per wavelength 650 / 510 / 475 nm):
// the reflection's color by the angle, 1 on average
vec3 thin_film(float vh, float nm, float ior) {
    float st2 = (1.0 - vh * vh) / (ior * ior), ct = sqrt(max(1.0 - st2, 0.0));
    vec3 ph = 12.5663706 * ior * nm * ct / vec3(650.0, 510.0, 475.0);
    return 1.0 + 0.9 * cos(ph);
}
// (Q27.8) Metallic flakes (car paint): a flake in most cells of Flake Size, tilted from the surface by Flake Spread, glints
// where its own normal takes the half vector (a sharp GGX). Past the pixel's footprint the cells merge (by powers of two,
// blended: no pop as it moves away): the glints dim as 1/sqrt of the flakes a cell holds, into the spread's wider lobe
uvec3 flake_hash(uvec3 v) { // (pcg3d)
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z, v.y += v.z * v.x, v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z, v.y += v.z * v.x, v.z += v.x * v.y;
    return v;
}
float flake_level(vec3 n, vec3 h, vec3 p, float cell, float spread) {
    uvec3 hs = flake_hash(uvec3(ivec3(floor(p / cell)) + 0x40000));
    vec3 r = vec3(hs & 0xFFFFu) / 65535.0;
    if (r.z > 0.7) return 0.0; // (a cell without a flake)
    vec3 fn = normalize(n + (vec3(r.xy, fract(r.x + r.y)) * 2.0 - 1.0) * spread);
    float nh = max(dot(fn, h), 0.0);
    return d_ggx(nh, 0.06) * 0.08;
}
float flakes(vec3 n, vec3 h, vec4 e12) {
    float lv = max(log2(max(g_wfw / e12.y, 1.0)), 0.0), l0 = floor(lv), t = lv - l0;
    float c0 = e12.y * exp2(l0), c1 = c0 * 2.0;
    float g = mix(flake_level(n, h, i_world, c0, e12.z) * exp2(-l0), flake_level(n, h, i_world, c1, e12.z) * exp2(-l0 - 1.0), t);
    float wide = d_ggx(max(dot(n, h), 0.0), clamp(0.15 + e12.z * 0.6, 0.15, 0.9)) * 0.056; // (the flakes' mean)
    return g + wide * (1.0 - exp2(-lv));
}
// GGX stretched along the tangent t (Burley 2012: the aspect from the anisotropy)
float d_ggx_aniso(vec3 n, vec3 h, vec3 t, float a, float aniso) {
    float aspect = sqrt(1.0 - 0.9 * aniso), ax = max(a / aspect, 1e-3), ay = max(a * aspect, 1e-3);
    vec3 b = cross(n, t);
    float th = dot(t, h) / ax, bh = dot(b, h) / ay, nh = dot(n, h), d = th * th + bh * bh + nh * nh;
    return 1.0 / (PI * ax * ay * d * d);
}
#endif
vec3 brdf_base(vec3 n, vec3 v, vec3 l, vec3 diff, vec3 f0, float a, float spec_scale) {
    vec3 h = normalize(v + l);
    float nl0 = dot(n, l);
    float nl = max(nl0, 0.0), nv = max(dot(n, v), 1e-4), nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
#ifdef RE_EXT
    [[dont_flatten]] if (g_ext == 0u) {
#endif
        vec3 spec = d_ggx(nh, a) * v_smith(nv, nl, a) * f_schlick(f0, vh) * spec_scale; // the light's Specular Scale
        return (diff / PI + spec) * nl;
#ifdef RE_EXT
    }
    if ((g_ext & SSS_BITS) == SSS_D) return diff / PI * nl; // (the skin's diffuse half: sss.comp blurs it, no wrap)
    uvec2 mb = FrameBuf(pc.frame).materials;
    uint slot = (g_ext & 4095u) - 1u;
    vec4 e0 = GpuMatExtBuf(mb).x[slot].e0;
    float dd = e0.w > 0.0 ? d_ggx_aniso(n, h, ext_tangent(g_ext), a, e0.w) : d_ggx(nh, a);
    vec4 e4 = GpuMatExtBuf(mb).x[slot].e4;
    vec3 fr = f_schlick(f0, vh);
    if (e4.z > 1.0) fr = clamp(fr * thin_film(vh, e4.z, e4.w), 0.0, 1.0); // (a thin film's colors: M4)
    vec3 r = dd * v_smith(nv, nl, a) * fr * spec_scale * nl;
    if (e4.x > 0.0) { // sheen (M3, Charlie: Estevez and Kulla 2017; Ashikhmin's visibility): cloth's rim of light
        float ir = 1.0 / e4.y, sh = sqrt(max(1.0 - nh * nh, 0.0));
        float ds = (2.0 + ir) * pow(sh, ir) / (2.0 * PI), vs = 1.0 / (4.0 * (nl + nv - nl * nv) + 1e-4);
        r += GpuMatExtBuf(mb).x[slot].e5.rgb * (e4.x * ds * vs * nl * spec_scale);
    }
    // subsurface: the light wraps past the terminator, farther in the channels that scatter more (skin: red)
    vec4 e3 = GpuMatExtBuf(mb).x[slot].e3;
    vec3 w = e3.rgb * e3.w;
#ifndef RE_TRANSLUCENT
    // (2026-11-17, Q4) Thin transmission (a leaf, a curtain, paper, a lampshade, an ear): that share of the diffuse light
    // goes through the sheet, so a light behind it lights its front, in its color; the front keeps the rest
    vec4 e11 = GpuMatExtBuf(mb).x[slot].e11;
    [[dont_flatten]] if (e11.w > 0.5) {
        float tw = e11.y;
        if (e11.z > 0.5) tw = clamp(WetParamBuf(FrameBuf(pc.frame).mat_params).p[GpuMaterialBuf(mb).m[slot].param_off + uint(e11.z + 0.5) - 1u].x, 0.0, 1.0);
        r += diff / PI * (max((vec3(nl0) + w) / (1.0 + w), 0.0) / (1.0 + w) * (1.0 - tw) + tw * max(-nl0, 0.0));
    } else
#endif
    r += diff / PI * max((vec3(nl0) + w) / (1.0 + w), 0.0) / (1.0 + w);
    if (g_e12.x > 0.0) r += f0 * (g_e12.x * flakes(n, h, g_e12) * nl * spec_scale); // (Q27.8: in the paint, under the coat)
    if (e0.x > 0.0) { // the coat: a smooth layer over it on the surface's own normal (not the normal map's)
        vec3 cn = normalize(i_normal);
        if (dot(cn, v) < 0.0) cn = -cn;
        float cnl = max(dot(cn, l), 0.0), cnv = max(dot(cn, v), 1e-4), cnh = max(dot(cn, h), 0.0);
        float fc = e0.z + (1.0 - e0.z) * pow(1.0 - vh, 5.0);
        float sc = d_ggx(cnh, e0.y) * v_smith(cnv, cnl, e0.y) * fc * spec_scale * cnl;
        r = r * mix(vec3(1.0), GpuMatExtBuf(mb).x[slot].e1.rgb, e0.x) * (1.0 - e0.x * fc) + vec3(sc * e0.x);
    }
    return r;
#endif
}

// (2026-11-15) the rain's water film over the surface (water.glsl rain_shade: g_rc, its normal and roughness): a
// dielectric layer of water (F0 0.02) on top, the energy it reflects taken from what is under it
vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 diff, vec3 f0, float a, float spec_scale) {
    vec3 r = brdf_base(n, v, l, diff, f0, a, spec_scale);
#ifdef RE_EXT
    [[dont_flatten]] if (g_retro != 0u) { // (Q13) the light sent back toward where it came from: bright when it comes from beside the viewer
        vec3 rc = vec3(float((g_retro >> 11u) & 31u) / 31.0, float((g_retro >> 5u) & 63u) / 63.0, float(g_retro & 31u) / 31.0);
        r += rc * (float(g_retro >> 16u) / 16.0) * pow(max(dot(l, v), 0.0), 40.0);
    }
#endif
    [[dont_flatten]] if (g_rcp.y != 0u) {
        vec2 wa = unpackHalf2x16(g_rcp.y);
        vec3 cn = rc_n(), h = normalize(v + l);
        float cnl = max(dot(cn, l), 0.0), cnv = max(dot(cn, v), 1e-4), cnh = max(dot(cn, h), 0.0);
        float fc = 0.02 + 0.98 * pow(1.0 - max(dot(v, h), 0.0), 5.0);
        r = r * (1.0 - wa.x * fc) + vec3(d_ggx(cnh, wa.y) * v_smith(cnv, cnl, wa.y) * fc * spec_scale * cnl * wa.x);
    }
    return r;
}

// (the film's reflection carried as halfs, pre-exposed: 2 registers instead of 3 floats through the reflection's ray)
uvec2 ce_put(FrameBuf f, vec3 c) {
    c = min(c * pre_exposure(f), vec3(60000.0));
    return uvec2(packHalf2x16(c.rg), packHalf2x16(vec2(c.b, 0.0)));
}
vec3 ce_get(FrameBuf f, uvec2 p) { return vec3(unpackHalf2x16(p.x), unpackHalf2x16(p.y).x) / pre_exposure(f); }

float shadow_factor(FrameBuf f, vec3 world, vec3 n) { return vsm_shadow(f, world, n); }

// Unreal's inverse-square falloff with a smooth window at the attenuation radius (distances in cm, energy in m)
float falloff(float d, GpuLight L) {
    float w = clamp(1.0 - pow(d * d * L.inv_range2, 2.0), 0.0, 1.0);
    float dm = max(d * 0.01, max(L.source_radius * 0.01, 0.01));
    return w * w / (dm * dm);
}

#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
// A picked local light's rays from p: the direction to it (a rectangle's closest point) and the distance short of it
void ml_geom(GpuLight L, vec3 p, out vec3 l, out float ldist) {
    vec3 q = L.position;
    if (light_kind(L) == LIGHT_RECT) {
        vec3 up = cross(L.direction, L.right), rel = p - L.position;
        q += L.right * clamp(dot(rel, L.right), -L.half_w, L.half_w) + up * clamp(dot(rel, up), -L.half_h, L.half_h);
    }
    vec3 lv = q - p;
    float d = length(lv);
    l = lv / max(d, 1e-3);
    ldist = d - 1.0;
}
#endif

// The surface a light shades: its point, normal, view, BRDF inputs, the shadow rays' origin and rotation
struct Surf {
    vec3 p, n, v, diff, f0, shadow_o;
    float a, rot;
};

// One light on the surface: its reach (falloff, cone, the rectangle's closest point), the shadow (the sun's map when
// map_shadow, or with RE_RT a ray while the pixel's budget of shadow-casting lights lasts: the rest are unshadowed)
#ifdef RE_DEN
// ReSTIR DI's target (A9.1a): a shadow-casting local light's unshadowed luminance at this point (as shade_light adds it
// to g_ml_w); 0 for anything else (a slot gone, a directional light)
float ml_target(FrameBuf f, uint slot, Surf s) {
    GpuLight L = LightBuf(f.light_buf).l[slot];
    if (light_kind(L) == LIGHT_DIRECTIONAL || !light_casts(L) || !light_channel_ok(L, g_inst_y)) return 0.0;
    vec3 l;
    float d;
    ml_geom(L, s.p, l, d);
    d += 1.0;
    float atten = falloff(d, L);
    if (light_kind(L) == LIGHT_RECT) atten *= max(dot(-l, L.direction), 0.0);
    else if (light_kind(L) == LIGHT_SPOT) {
        float c = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
        atten *= c * c;
    }
    if (atten <= 0.0) return 0.0;
    return dot(brdf(s.n, s.v, l, s.diff, s.f0, s.a, light_specular_scale(L)) * L.radiance * atten, vec3(0.2126, 0.7152, 0.0722));
}
// last frame's reservoir of this point: its world position by last frame's view, the guide there on the same actor
uint resv_prev(FrameBuf f, vec3 p, uint actor) {
    vec4 b = f.tsr_prev_vp * vec4(p, 1.0);
    if (b.w <= 1e-4) return 0u;
    vec2 size = floor(1.0 / f.viewport.zw + 0.5);
    ivec2 q = ivec2((b.xy / b.w * 0.5 + 0.5) * size);
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(size)))) return 0u;
    uvec4 g = imageLoad(u_den_prev, q);
    return g.y == actor ? g.w : 0u;
}
#endif
#if defined(RE_RT) && defined(RE_EXT) && !defined(RE_TRANSLUCENT)
// The skin's transmission (A9.3a): a light behind a thin part (an ear, a finger, a nostril's wing) through it. Its
// thickness along the light by one ray from inside (where it leaves its own mesh: the eyes, the teeth sunk into a head
// are other actors, not its far side; they made rings of red light around the eyes), the light through it per channel
// as through a slab (Beer-Lambert over the material's scattering distances: an ear's 3 mm let the red and some green
// through, a hand's 2 cm a faint red; Burley's own profile kept a third of the red past 2 cm), the light taken as
// reaching the far side unshadowed. Past 7 times the widest distance nothing comes through (a head: no ray). A diffuse
// pixel of the checkerboard only
#ifndef RE_SWRT
// the nearest hit of [o + 0.5 l, o + tmax l] on the instance me alone (every triangle a candidate: short rays only)
float own_hit(FrameBuf f, bool dyn, vec3 o, vec3 l, float tmax, uint me) {
    rayQueryEXT rq;
    if (dyn) rayQueryInitializeEXT(rq, dyn_tlas(f), gl_RayFlagsNoOpaqueEXT, RT_MASK_SHADOW, o, 0.5, l, tmax);
    else rayQueryInitializeEXT(rq, scene_tlas(f), gl_RayFlagsNoOpaqueEXT, RT_MASK_SHADOW, o, 0.5, l, tmax);
    while (rayQueryProceedEXT(rq))
        if (uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) == me) rayQueryConfirmIntersectionEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT ? rayQueryGetIntersectionTEXT(rq, true) : 1e9;
}
#endif
vec3 sss_transmit(FrameBuf f, vec3 p, vec3 n, vec3 l, float ldist, vec3 diff, inout uint budget) {
    if (budget == 0u) return vec3(0.0);
    vec4 e7 = GpuMatExtBuf(f.materials).x[(g_ext & 4095u) - 1u].e7;
    uint q = uint(e7.w + 0.5);
    vec3 d = max(e7.z * vec3(float((q >> 16u) & 255u), float((q >> 8u) & 255u), float(q & 255u)) / 255.0, vec3(1e-3));
    float tmax = min(7.0 * e7.z, ldist) + 0.45;
    budget--;
    // (from 0.45 cm in front, so the queries' 0.5 cm start is just inside the surface)
    vec3 o = p - l * 0.45;
    float t = 1e9;
    if (!ray_ok(o, l, tmax)) return vec3(0.0);
#ifdef RE_SWRT
    float tt;
    uint ii, prim;
    vec2 bc;
    bool fr;
    if (closest_in(f, false, o, l, tmax, RT_MASK_SHADOW, tt, ii, prim, bc, fr)) t = tt;
    if (hits_dyn(f, o, l, tmax) && closest_in(f, true, o, l, tmax, RT_MASK_SHADOW, tt, ii, prim, bc, fr)) t = min(t, tt);
#else
    t = own_hit(f, false, o, l, tmax, i_instance);
    if (hits_dyn(f, o, l, tmax)) t = min(t, own_hit(f, true, o, l, tmax, i_instance));
#endif
    if (t > tmax) return vec3(0.0);
    float th = max(t - 0.45, 0.0);
    vec3 tr = exp(-th / d);
    return diff / PI * tr * max(-dot(n, l), 0.0);
}
#endif
#if defined(RE_RT) && defined(RE_EXT) && !defined(RE_TRANSLUCENT) && !defined(RE_SWRT)
// (2026-11-17, Q4) The colored shadow of glass: the panes a ray toward the sun crosses (the Translucent instances are in
// the TLAS under their own mask, RT_MASK_GLASS, which no other ray uses), each entered face times its material's tint
// (material_gpu.inc: its base color by the absorption through its thickness, GpuMatExt e9; none: clear)
vec3 glass_tint(FrameBuf f, vec3 o, vec3 d) {
    vec3 t = vec3(1.0);
    for (int k = 0; k < 2; k++) {
        if (k == 1 && !hits_dyn(f, o, d, 1e7)) break;
        rayQueryEXT rq;
        if (k == 0) rayQueryInitializeEXT(rq, scene_tlas(f), gl_RayFlagsNoOpaqueEXT, RT_MASK_GLASS, o, 0.5, d, 1e7);
        else rayQueryInitializeEXT(rq, dyn_tlas(f), gl_RayFlagsNoOpaqueEXT, RT_MASK_GLASS, o, 0.5, d, 1e7);
        uint n = 0u;
        while (rayQueryProceedEXT(rq)) {
            if (!rayQueryGetIntersectionFrontFaceEXT(rq, false)) continue;
            Instance gi = InstanceBuf(f.instances).i[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false))];
            if ((gi.ids.y & INST_MATERIAL) == 0u) continue;
            uint ms = floatBitsToUint(gi.emissive_rough.x);
            if ((GpuMaterialBuf(f.materials).m[ms].flags & MATF_EXT) == 0u) continue;
            vec4 e9 = GpuMatExtBuf(f.materials).x[ms].e9;
            if (e9.w > 0.5) t *= e9.rgb;
            n++;
            if (n >= 6u || max(t.r, max(t.g, t.b)) < 0.01) rayQueryTerminateEXT(rq);
        }
    }
    return t;
}
#endif
#if defined(RE_EXT) && !defined(RE_TRANSLUCENT)
// (2026-11-17, Q4) Interior Mapping (the Interior Mapping node, MATX_INTERIOR): each window of the surface's UV grid (e8.xy
// rooms along U and V) is a room behind it, a box Room Depth widths deep (e8.z) the view ray enters (Van Dongen 2008):
// its back wall, side walls, floor and ceiling where the ray hits them, from the atlas (e7: its texture, columns, rows;
// a room a tile, perspective as seen from the window) or made up per room (a wall color, a wooden floor, a light
// ceiling); a layer of furniture at 60% of the depth (a sofa, a desk, a shelf: silhouettes per room), curtains just
// behind the glass (drawn shut by Curtains x a per room amount, with folds), lit by the day coming in or, at Night
// (e9.z, a parameter in e11.x), by a lamp in a random share of the rooms (e9.w; some switch every half minute) in e10's
// color and strength. The frame (e8.w of each window's width) stays the facade's shader; in the window the surface turns
// glass (dark, smooth: the sky's reflection) and the room is its emission through it. Returns the window's share
float ih(vec2 c, float k) { return fract(sin(dot(c, vec2(127.1, 311.7)) + k * 74.7) * 43758.5453); }
float interior(FrameBuf f, GpuMaterial gm, uint slot, vec2 uv, mat3 tbn, vec3 v, out vec3 room) {
    GpuMatExt X = GpuMatExtBuf(f.materials).x[slot];
    vec4 e7 = X.e7, e8 = X.e8, e9 = X.e9, e10 = X.e10;
    float night = e9.z;
    if (X.e11.x > 0.5) night = clamp(WetParamBuf(f.mat_params).p[gm.param_off + uint(X.e11.x + 0.5) - 1u].x, 0.0, 1.0);
    vec2 p = uv * e8.xy, cell = floor(p), q = fract(p) * 2.0 - 1.0; // (the window: -1..1 on each axis)
    room = vec3(0.0);
    float fr = e8.w * 2.0;
    if (abs(q.x) > 1.0 - fr || abs(q.y) > 1.0 - fr) return 0.0; // the frame
    q /= 1.0 - fr; // (the opening's edges are the room's)
    vec3 T = normalize(tbn[0]), B = normalize(tbn[1]), N = tbn[2];
    vec3 d = vec3(dot(-v, T), dot(-v, B), min(dot(-v, N), -1e-3)); // (into the room: z < 0)
    d.xy = vec2(abs(d.x) < 1e-4 ? 1e-4 : d.x, abs(d.y) < 1e-4 ? 1e-4 : d.y); // (straight on: no 0 / 0 at the side walls)
    float D = 2.0 * e8.z, seed = e7.w;
    vec3 o = vec3(q, 0.0);
    vec3 tt = (sign(d) - o) / d; // (the side walls at +-1)
    float tz = (-D - o.z) / d.z;
    float t = min(min(tt.x, tt.y), tz);
    vec3 h = o + d * t;
    bool bup = dot(B, vec3(0.0, 0.0, 1.0)) >= 0.0; // (the V axis up the facade: y = -1 is the floor)
    float r0 = ih(cell, seed), r1 = ih(cell, seed + 1.0), r2 = ih(cell, seed + 2.0);
    vec3 alb;
    float depth01 = clamp(-h.z / D, 0.0, 1.0);
    if (e7.x > 0.5) { // the atlas: this room's tile, the box seen from the window (the back wall the middle half)
        float s = 1.0 / (1.0 + depth01);
        vec2 tuv = h.xy * s * 0.5 + 0.5;
        uint nt = uint(e7.y) * uint(e7.z), ti = uint(r0 * float(nt)) % max(nt, 1u);
        vec2 tile = vec2(float(ti % uint(e7.y)), float(ti / uint(e7.y)));
        alb = srgb_to_linear(textureLod(u_mat_tex[nonuniformEXT(uint(e7.x))], (tile + clamp(tuv, 0.002, 0.998)) / e7.yz, 0.0).rgb);
    } else { // made up: walls in a color of the room, a wooden floor, a light ceiling, the corners darker
        vec3 wall = mix(vec3(0.78, 0.74, 0.66), 0.35 + 0.6 * vec3(r0, r1, r2), 0.35);
        bool fl = t == tt.y && (h.y < 0.0) == bup, ce = t == tt.y && !fl;
        alb = fl ? vec3(0.36, 0.22, 0.12) * (0.75 + 0.5 * r1) * (0.85 + 0.15 * sin(h.x * 18.0)) : ce ? vec3(0.86) : t == tz ? wall * 0.92 : wall;
        vec2 e = 1.0 - abs(t == tz ? h.xy : t == tt.x ? vec2(h.y, h.z / D * 2.0 + 1.0) : vec2(h.x, h.z / D * 2.0 + 1.0));
        alb *= 0.75 + 0.25 * smoothstep(0.0, 0.25, min(e.x, e.y));
    }
    // furniture: a silhouette on a plane at 60% of the depth (its kind and place by the room)
    float zf = -D * 0.6, tf = (zf - o.z) / d.z;
    if (tf < t && e9.x > r2 * 0.999) {
        vec2 fp = (o + d * tf).xy;
        float fy = bup ? fp.y : -fp.y, fx = fp.x - (r1 - 0.5) * 0.6;
        float k = floor(r2 * 3.0);
        bool on = k < 1.0 ? (fy < -0.45 && abs(fx) < 0.65) || (fy < -0.1 && abs(fx) < 0.65 && fy > -0.45 && abs(fx) > 0.5) // a sofa
                : k < 2.0 ? (fy < -0.35 && fy > -0.45 && abs(fx) < 0.55) || (fy < -0.45 && abs(abs(fx) - 0.5) < 0.05) // a desk
                          : abs(fx + 0.55) < 0.25 && fy < 0.6; // a shelf
        if (on) t = tf, alb = vec3(0.12, 0.1, 0.09) + 0.2 * vec3(r0, r1 * 0.8, r2 * 0.6), depth01 = 0.6;
    }
    // the light in it: the day coming in (dimmer deep inside) or a lamp at night in some rooms
    // (the sky through the window and the sun bounced in off the floor and the street: a few percent of it)
    // the daylight inside (a daylight factor: a room near its window gets ~5% of the light outside on a horizontal
    // plane, the sky's and the sun's; less deep inside): what the window lets in, bounced around the room
    vec3 eh = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) : vec3(0.6);
    for (uint li = 0u; li < f.light_count; li++)
        if (light_kind(f.lights[li]) == LIGHT_DIRECTIONAL) eh += f.lights[li].radiance * max(-f.lights[li].direction.z, 0.0);
    vec3 day = eh * (0.05 / PI) * (1.0 - 0.6 * depth01);
    float lit = step(ih(cell + floor(f.camera_pos.w / 30.0 + r1) * 0.013, seed + 5.0) * 0.85 + ih(cell, seed + 7.0) * 0.15, e9.w);
    vec3 lamp = e10.rgb * e10.w * lit * (0.55 + 0.45 * exp(-dot(h.xy - vec2(0.0, bup ? 0.9 : -0.9), h.xy - vec2(0.0, bup ? 0.9 : -0.9)) * 0.8));
    room = alb * mix(day, lamp + day * 0.05, night);
    // curtains just behind the glass: from both sides, Curtains x a share by room, folds; lit through from inside
    float cs = e9.y * (0.2 + 0.8 * r0);
    if (cs > 0.01 && (q.x < -1.0 + cs || q.x > 1.0 - cs)) {
        vec3 fab = mix(vec3(0.85, 0.82, 0.74), vec3(r2, r0, r1) * 0.8, 0.4) * (0.78 + 0.22 * sin(q.x * 42.0 + r1 * 6.0));
        room = fab * mix(day * 1.4, lamp * 0.45 + day * 0.05, night);
    }
    if (any(isnan(room)) || any(isinf(room))) room = vec3(0.0); // (never a NaN out of here: the TSR and the exposure spread one)
    room = min(room, vec3(60000.0));
    return 1.0;
}
#endif
#if (defined(RE_EXT) || defined(RE_LSM)) && !defined(RE_TRANSLUCENT)
// (2026-11-17, Q4) The colored shadow of glass without rays (Low, a GPU without ray queries, the software rays): the glass
// with Colored Shadow nearest the camera as oriented boxes (render.c glass_fill, GpuFrame.glass); the ray from p toward
// the sun through each box takes its material's tint (GpuMatExt e9, as glass_tint's)
vec3 glass_box_tint(FrameBuf f, vec3 p, vec3 d) {
    vec3 t = vec3(1.0);
    uint nb = min(f.rgi_info.y, 16u);
    for (uint i = 0u; i < nb; i++) {
        vec4 c = f.glass[i * 4u];
        vec3 ax = f.glass[i * 4u + 1u].xyz, ay = f.glass[i * 4u + 2u].xyz, az = f.glass[i * 4u + 3u].xyz;
        vec3 w = p - c.xyz;
        vec3 ia = vec3(1.0 / max(dot(ax, ax), 1e-6), 1.0 / max(dot(ay, ay), 1e-6), 1.0 / max(dot(az, az), 1e-6));
        vec3 o = vec3(dot(w, ax), dot(w, ay), dot(w, az)) * ia, dl = vec3(dot(d, ax), dot(d, ay), dot(d, az)) * ia;
        vec3 inv = 1.0 / vec3(abs(dl.x) > 1e-7 ? dl.x : 1e-7, abs(dl.y) > 1e-7 ? dl.y : 1e-7, abs(dl.z) > 1e-7 ? dl.z : 1e-7);
        vec3 t0 = (-1.0 - o) * inv, t1 = (1.0 - o) * inv;
        vec3 lo = min(t0, t1), hi = max(t0, t1);
        float tn = max(max(lo.x, lo.y), lo.z), tf = min(min(hi.x, hi.y), hi.z);
        if (tf < max(tn, 1e-3) || all(lessThan(abs(o), vec3(1.0)))) continue; // (missed, or p inside it: the pane's own face)
        uint ms = floatBitsToUint(c.w);
        vec4 e9 = GpuMatExtBuf(f.materials).x[ms].e9;
        if (e9.w > 0.5) t *= e9.rgb;
    }
    return t;
}
#endif
#if defined(RE_EXT) || defined(RE_LSM)
#include "lightfn.glsl" // (Q4: also in the volumetric fog and the GI)
#endif
#ifdef RE_TOON
// (Q11) the toon path: a Toon material (unless the light says LIGHT_TOON_NEVER) or any material under a LIGHT_TOON_FORCE
// light. shade_light computes the light as always (its reach and its shadow in atten: the common path keeps its
// registers) and turns to toon_end at its end; the shadow taken back out of atten by the reach computed again
bool toon_path(GpuLight L) {
#ifdef RE_TOON_MAT
    return g_toon != 0u ? (L.type & LIGHT_TOON_NEVER) == 0u : (L.type & LIGHT_TOON_FORCE) != 0u;
#else
    return (L.type & LIGHT_TOON_FORCE) != 0u;
#endif
}
vec3 toon_end(FrameBuf f, GpuLight L, vec3 l, Surf s, float atten, float ldist) {
    float reach = 1.0; // (a directional light: no falloff, its atten is its shadow and the clouds')
    if (light_kind(L) != LIGHT_DIRECTIONAL) {
        float d = ldist + 1.0; // (the distance shade_light used: the rectangle's closest point)
        reach = light_kind(L) == LIGHT_RECT ? falloff(d, L) * max(dot(-l, L.direction), 0.0) : falloff(d, L);
        if (light_kind(L) == LIGHT_SPOT) {
            float c = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
            reach *= c * c;
        }
        reach = max(reach, 1e-12);
    }
#ifdef RE_TOON_MAT
    float vis = clamp(atten / reach, 0.0, 1.0);
    [[dont_flatten]] if (g_toon != 0u) { // a Toon material: its bands; the tone of its strongest light for the hatching
        float level;
        vec3 r = toon_shade(GpuToonBuf(f.materials), TOON_SLOT - 1u, s.n, s.v, l, s.diff, vis, level) * L.radiance;
        uint w = uint(min(sqrt(sqrt(max(dot(L.radiance, vec3(0.2126, 0.7152, 0.0722)), 0.0))) * 200.0, 4095.0));
        if (w >= (g_toon >> 20u)) g_toon = TOON_SLOT | (uint(clamp(level, 0.0, 1.0) * 255.0 + 0.5) << 12u) | (w << 20u);
        return r * reach;
    }
#endif
    return toon_forced(s.n, l, s.diff) * L.radiance * atten; // (a hard cel step)
}
#endif
vec3 shade_light(FrameBuf f, GpuLight L, uint id, float map_vis, Surf s, inout uint budget, bool local_map) {
    if (!light_channel_ok(L, g_inst_y)) return vec3(0.0); // (Q13) not one of its Lighting Channels
    bool map_shadow = map_vis >= 0.0; // (the sun's shadow, read once before the lights' loop: its filter's registers die there)
    vec3 l;
    float atten, ldist = 1e7;
    if (light_kind(L) == LIGHT_DIRECTIONAL) {
        l = -L.direction;
        atten = map_shadow ? map_vis : 1.0;
        atten *= cloud_shadow(f, s.p); // the clouds passing over (weather.glsl)
#ifdef RE_TOON
        if (atten <= 0.0 && toon_path(L)) atten = 1e-30; // (Q11: in the shadow a Toon material still takes its band)
#endif
    } else if (light_kind(L) == LIGHT_RECT) {
        // the closest point of the rectangle (a one-sided emitter facing L.direction)
        vec3 up = cross(L.direction, L.right);
        vec3 rel = s.p - L.position;
        vec3 q = L.position + L.right * clamp(dot(rel, L.right), -L.half_w, L.half_w) + up * clamp(dot(rel, up), -L.half_h, L.half_h);
        vec3 lv = q - s.p;
        float d = length(lv);
        l = lv / max(d, 1e-3);
        ldist = d - 1.0;
        atten = falloff(d, L) * max(dot(-l, L.direction), 0.0);
    } else {
        vec3 lv = L.position - s.p;
        float d = length(lv);
        l = lv / max(d, 1e-3);
        ldist = d - 1.0;
        atten = falloff(d, L);
        if (light_kind(L) == LIGHT_SPOT) {
            float c = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
            atten *= c * c;
        }
    }
    if (atten <= 0.0) return vec3(0.0);
#if defined(RE_EXT) || defined(RE_LSM)
    [[dont_flatten]] if (light_kind(L) != LIGHT_RECT && L.half_w >= 1.0) { // (Q4) its Light Function (the RE_EXT and RE_LSM variants)
        L.radiance *= light_fn(f, L, s.p);
        if (dot(L.radiance, vec3(1.0)) <= 0.0) return vec3(0.0);
    }
#endif
#if defined(RE_RT) && defined(RE_EXT) && !defined(RE_TRANSLUCENT)
    if ((g_ext & SSS_BITS) == SSS_D && dot(s.n, l) < 0.0) // (a light behind the skin: through it, A9.3a)
        return sss_transmit(f, s.p, s.n, l, ldist, s.diff, budget) * L.radiance * atten;
#endif
#ifdef RE_LOCAL_SM
    // its shadow map (lsm.glsl; set only when the local lights aren't traced): none for a light without one this frame
    // (local_map: false in the directional lights' call, its copy of this compiled out)
    if (local_map && (f.gi_flags & GI_LOCAL_SM) != 0u && light_casts(L) && dot(s.n, l) > 0.0) {
        atten *= lsm_shadow(f, id, s.p, s.n);
#ifdef RE_TOON
        if (atten <= 0.0 && !toon_path(L)) return vec3(0.0); // (Q11: a Toon material's band in the shadow)
#else
        if (atten <= 0.0) return vec3(0.0);
#endif
    }
#endif
#ifdef RE_RT
#ifndef RE_TRANSLUCENT
    // a shadow-casting local light under the many lights' sampling (the split: render.c, Lit views): summed unshadowed,
    // maybe picked for a ray (a pixel without rays, budget 0, keeps every light unshadowed as before)
    if ((f.gi_flags & GI_SHADOW_SPLIT) != 0u && light_kind(L) != LIGHT_DIRECTIONAL && light_casts(L) && budget > 0u
#ifdef RE_TOON
        && !toon_path(L) // (Q11: the toon's own ray)
#endif
    ) {
        vec3 c = brdf(s.n, s.v, l, s.diff, s.f0, s.a, light_specular_scale(L)) * L.radiance * atten;
        float w = dot(c, vec3(0.2126, 0.7152, 0.0722));
        if (w > 0.0) {
            g_ml_e += c;
            g_ml_w += w;
            g_ml_n++;
            uint h = ml_hash(g_ml_seed ^ (id * 0x9E3779B9u)); // (16 bits a slot)
            if (float(h & 0xFFFFu) * (1.0 / 65536.0) * g_ml_w < w) g_ml_sel.x = id;
            if (float(h >> 16u) * (1.0 / 65536.0) * g_ml_w < w) g_ml_sel.y = id;
        }
        return vec3(0.0);
    }
#endif
    float vis = 1.0;
    if (!map_shadow && (f.gi_flags & GI_RT_SHADOWS) != 0u && light_casts(L) && budget > 0u && dot(s.n, l) > 0.0) { // (the sun on its map: no ray)
        budget--;
        vis = light_visibility(f, s.shadow_o, L, l, ldist, uint(f.sky_params.y), s.rot, RT_MASK_SHADOW);
    }
#ifdef RE_TOON
    [[dont_flatten]] if (toon_path(L)) return toon_end(f, L, l, s, atten * vis, ldist); // (Q11)
#endif
    return brdf(s.n, s.v, l, s.diff, s.f0, s.a, light_specular_scale(L)) * L.radiance * atten * vis;
#else
#ifdef RE_TOON
    [[dont_flatten]] if (toon_path(L)) return toon_end(f, L, l, s, atten, ldist); // (Q11)
#endif
    return brdf(s.n, s.v, l, s.diff, s.f0, s.a, light_specular_scale(L)) * L.radiance * atten;
#endif
}
// (A4c) Screen space reflections: last frame's render (its color as the main pass left it, its depth) along the reflected
// ray, in growing steps from 8 cm (to ~34 m), each point projected by last frame's view; in front of the depth there by
// less than a thickness: the color there. rgb: the radiance, a: the confidence (the screen's edges, the far steps fade)
#ifdef RE_LOCAL_SM // (not in the plain variant, kept lean: render.c takes the RE_LSM one while SSR runs; it spilled 64 B there)
layout(set = 0, binding = 82) uniform sampler2D u_ssr_col;
layout(set = 0, binding = 83) uniform sampler2D u_ssr_dep;
vec4 ssr_trace(FrameBuf f, vec3 o, vec3 d) {
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy + float(f.frame_index & 7u) * 7.0, vec2(0.06711056, 0.00583715))));
    float t = 8.0 * (1.0 + 0.3 * jit);
    vec4 A = f.cloud_prev_vp * vec4(o, 1.0), B = f.cloud_prev_vp * vec4(d, 0.0); // (the ray projected once: a point is A + B t)
    [[dont_unroll]] for (int k = 0; k < 16; k++, t *= 1.45) { // (not unrolled: the main pass's registers; 16 steps to ~34 m: 24 cost 0.7 ms in the crowd)
        vec4 c = A + B * t;
        if (c.w <= 1.0) break;
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float dd = textureLod(u_ssr_dep, uv, 0.0).r;
        if (dd <= 0.0) continue; // (the sky)
        float diff = c.w - f.pp[5].w / dd; // (the point behind the surface there by this much: reversed infinite Z, near / d)
        if (diff > 0.0 && diff < max(t * 0.2, 12.0)) {
            vec2 e = min(uv, 1.0 - uv);
            return vec4(textureLod(u_ssr_col, uv, 0.0).rgb / max(pre_exposure(f), 1e-12),
                        smoothstep(0.0, 0.08, min(e.x, e.y)) * (1.0 - float(k) / 16.0));
        }
    }
    return vec4(0.0);
}
#endif

#ifndef RE_RT
// (Q27.12) the GI fallback's AO volume (gi_fallback.c): a byte a cell, 64 x 64 x 16
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer GiFbBuf { uint v[]; };
float gifb(GiFbBuf b, ivec3 c) {
    uint i = (uint(c.z) * 64u + uint(c.y)) * 64u + uint(c.x);
    return float((b.v[i >> 2u] >> ((i & 3u) * 8u)) & 255u) * (1.0 / 255.0);
}
#endif
void main() {
#if defined(RE_MASKED) && !defined(RE_MASKED_EQ)
    gl_SampleMask[0] = -1; // (the cut below as MSAA coverage, masked.frag's: written on every path)
#endif
    FrameBuf f = FrameBuf(pc.frame);
    Instance inst = InstanceBuf(pc.instances).i[i_instance];
    g_inst_y = inst.ids.y; // (Q13: the Lighting Channels)
#if defined(RE_MASKED) && !defined(RE_MASKED_EQ)
    if (!gl_FrontFacing && (inst.ids.y & INST_MATERIAL) == 0u && !i_card) discard; // (no material: one-sided)
#endif
    vec3 n = normalize(i_normal);
    vec3 v = view_ortho(f) ? -f.camera_fwd.xyz : normalize(f.camera_pos.xyz - i_world); // (orthographic: the same everywhere)
#ifdef RE_EXT
    g_wfw = max(length(fwidth(i_world)), 1e-4); // (Q27.8: the flakes' footprint, here where every pixel of the quad runs)
#endif
    if (dot(n, v) < 0.0) n = -n; // two-sided look for open meshes seen from behind
    vec3 base = inst.base_metal.rgb;
    float metal = inst.base_metal.w;
    float rough = clamp(inst.emissive_rough.w, 0.045, 1.0);
    vec3 emissive = (inst.ids.y & INST_MATERIAL) != 0u ? vec3(0.0) : inst.emissive_rough.rgb;
    // (Q21) an impostor's card: the atlas's three frames around the view (impostor.glsl), its base color, normal (mesh
    // space -> world by the cofactors, as mesh.vert), roughness; the point it shows lit (g_pw: the shadows, the air)
#ifdef RE_MASKED
    [[dont_flatten]] if (i_card) {
        vec3 wp;
        ImpSample is = imp_card(f, inst, i_world, i_uvs.zw, true, wp);
#if defined(RE_MASKED) && !defined(RE_MASKED_EQ)
        { // (without the prepass: masked.frag's coverage)
            uint m = alpha_coverage(is.alpha, fwidth(is.alpha), 0.5, uint(max(f.sky_params.z, 1.0)), uint(gl_FragCoord.x) * 3u + uint(gl_FragCoord.y) * 5u + i_instance);
            if (m == 0u) discard;
            gl_SampleMask[0] = int(m);
        }
#endif
        bool own = (inst.ids.y & INST_MATERIAL) != 0u;
        base = srgb_to_linear(is.base) * (own ? vec3(1.0) : inst.base_metal.rgb);
        metal = own ? 0.0 : inst.base_metal.w;
        rough = clamp(is.rough, 0.045, 1.0);
        emissive = vec3(0.0);
        vec3 ca = vec3(inst.row0.x, inst.row1.x, inst.row2.x), cb = vec3(inst.row0.y, inst.row1.y, inst.row2.y), cc = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
        n = normalize(cross(cb, cc) * is.n.x + cross(cc, ca) * is.n.y + cross(ca, cb) * is.n.z + vec3(0.0, 0.0, 1e-6));
        if (dot(n, v) < 0.0) n = normalize(n - v * dot(n, v) * 1.01);
        g_pt = max(dot(wp - i_world, -v), 0.0);
    } else
#endif
    // The material (material.c): its baked textures (2-3 reads, however many nodes), or its per-pixel program over them
    // (parameters, the view). The tangent frame from the derivatives (no tangents in the vertices): inside the branch,
    // which is uniform per quad (a quad is one triangle's, inst.mat is flat), so the derivatives stay defined; a mesh
    // without a material pays nothing
    if ((inst.ids.y & INST_MATERIAL) != 0u) {
        GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
        // World Aligned: the UV from the world position projected on the face's dominant axis, uv_scale cm a tile (the
        // level's walls and floors tile at one size whatever their scale; the tangent frame below follows this UV)
        vec2 muv = i_uv;
        float mbias = tsr_mip_bias(f.tsr_size); // (TSR / an upscaler: the output's sharpness)
        if ((gm.flags & MATF_WORLD_UV) != 0u) {
            vec3 gn = abs(normalize(i_normal));
            vec2 wp = gn.z >= gn.x && gn.z >= gn.y ? i_world.xy : gn.x >= gn.y ? vec2(i_world.y, -i_world.z) : vec2(i_world.x, -i_world.z);
            muv = wp / max(gm.avg.w, 1.0);
        }
        vec3 dp1 = dFdx(i_world), dp2 = dFdy(i_world);
        vec2 duv1 = dFdx(muv), duv2 = dFdy(muv);
        vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x, B = dp2perp * duv1.y + dp1perp * duv2.y;
        float im = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        mat3 tbn = mat3(T * im, B * im, n);
#ifdef RE_EXT
        // Parallax occlusion (M7): the surface seen sunk into its height map (the baked base's alpha, MATF_POM): a march
        // along the view in tangent space (8 steps facing it, up to e7.y grazing), then the crossing between the last two;
        // the textures below are read at the uv it lands on. Their gradients from the uv's own (the loop is not uniform)
        [[dont_flatten]] if ((gm.flags & MATF_POM) != 0u && gm.anim.x <= 1u) {
            vec4 e7 = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)].e7;
            vec3 tu = normalize(T), tv = normalize(B);
            vec3 vt = vec3(dot(v, tu), dot(v, tv), max(dot(v, n), 0.05));
            float steps = mix(e7.y, 8.0, vt.z), dz = 1.0 / steps;
            vec2 shift = -vt.xy / vt.z * e7.x * dz;
            vec2 gx = dFdx(muv), gy = dFdy(muv);
            vec2 uvp = muv;
            float depth = 0.0, h = 1.0 - textureGrad(u_mat_tex[nonuniformEXT(gm.tex_base)], uvp, gx, gy).a, hprev = h;
            for (int k = 0; k < 64 && depth < h; k++) {
                uvp += shift, depth += dz, hprev = h;
                h = 1.0 - textureGrad(u_mat_tex[nonuniformEXT(gm.tex_base)], uvp, gx, gy).a;
            }
            float after = h - depth, before = hprev - (depth - dz), t = after / min(after - before, -1e-5);
            muv = uvp - shift * clamp(t, 0.0, 1.0);
        }
        // The eye (A9.3c, MATF_EYE: Unreal's Eye shading model): the iris lies under the cornea's dome (its height over
        // the iris, e7.y cm at the center, 0 at the limbus); the view refracted by the cornea (IOR 1.376) reaches it
        // displaced: its textures read where the refracted ray lands (the uv by the derivatives' frame). The eye looks
        // along its actor's forward (+X); the iris is centered at uv (0.5, 0.5), e7.x its radius
        [[dont_flatten]] if ((gm.flags & MATF_EYE) != 0u) {
            vec4 e7 = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)].e7;
            float r = length(muv - 0.5) / max(e7.x, 1e-3);
            vec3 F = normalize(vec3(inst.row0.x, inst.row1.x, inst.row2.x) + vec3(1e-6));
            vec3 rd = refract(-v, n, 1.0 / 1.376);
            vec3 o = rd * (e7.y * max(1.0 - r * r, 0.0) / max(dot(rd, -F), 0.25)); // (0 past the iris: no seam)
            float det = dot(n, cross(dp1, dp2));
            if (abs(det) > 1e-20) muv += vec2(dot(o, T), dot(o, B)) / det;
        }
#endif
        // every material is baked (its instances and the actors' dynamic instances too, again when a parameter changes):
        // a pixel reads 2-3 textures whatever the graph (no interpreter here: it cost every pixel registers)
        vec4 tb, tn;
        uint emis_tex = gm.tex_emis;
        vec2 udim_uv = muv, udim_gx = vec2(0.0), udim_gy = vec2(0.0); // (Q18)
        bool udim_on = false;
        if (gm.anim.x > 1u) { // animated: the loop's two frames around now, blended (6 reads; a still material 3)
            float ft = fract(f.camera_pos.w / max(uintBitsToFloat(gm.anim.y), 0.05)) * float(gm.anim.x);
            uint f0 = uint(ft) % gm.anim.x, f1 = (f0 + 1u) % gm.anim.x;
            float a = fract(ft);
            MatFrameBuf fb = MatFrameBuf(f.mat_params);
            uvec4 t0 = fb.t[gm.anim.z + f0], t1 = fb.t[gm.anim.z + f1];
            tb = mix(texture(u_mat_tex[nonuniformEXT(t0.x)], muv, mbias), texture(u_mat_tex[nonuniformEXT(t1.x)], muv, mbias), a);
            tn = mix(texture(u_mat_tex[nonuniformEXT(t0.y)], muv, mbias), texture(u_mat_tex[nonuniformEXT(t1.y)], muv, mbias), a);
            emis_tex = a < 0.5 ? t0.z : t1.z;
        } else if ((gm.flags & MATF_UDIM) != 0u) {
            // (Q18) UDIM: the tile under the uv (its own textures), read at the uv inside it with the whole uv's gradients
            // (they don't jump at the tile's edge: no seam of the smallest mip there; the tiles clamp, no bleed either)
            vec2 tuv = muv;
            uvec4 tt = udim_tile(f.mat_params, gm, tuv);
            float g = exp2(mbias);
            udim_gx = duv1 * g, udim_gy = duv2 * g, udim_uv = tuv, udim_on = true;
            tb = textureGrad(u_mat_tex[nonuniformEXT(tt.x)], tuv, udim_gx, udim_gy);
            tn = textureGrad(u_mat_tex[nonuniformEXT(tt.y)], tuv, udim_gx, udim_gy);
            emis_tex = tt.z;
        } else {
            tb = texture(u_mat_tex[nonuniformEXT(gm.tex_base)], muv, mbias);
            tn = texture(u_mat_tex[nonuniformEXT(gm.tex_nrm)], muv, mbias);
        }
#ifdef RE_TRANSLUCENT
        if (!gl_FrontFacing && (gm.flags & MATF_TWO_SIDED) == 0u) discard; // (both sides rasterized: one-sided unless Two Sided)
        if ((gm.flags & MATF_TRANSLUCENT) != 0u) g_opacity = tb.a;
#endif
#if defined(RE_MASKED) && !defined(RE_MASKED_EQ)
        // (the masked pipelines: Masked cuts by the baked alpha; their back faces drawn only when Two Sided)
        if (!gl_FrontFacing && (gm.flags & MATF_TWO_SIDED) == 0u) discard;
        if ((gm.flags & MATF_MASKED) != 0u) { // (without the prepass: the same coverage as masked.frag's)
            uint m = alpha_coverage(tb.a, fwidth(tb.a), float(gm.flags >> 24u) / 255.0, uint(max(f.sky_params.z, 1.0)),
                                    uint(gl_FragCoord.x) * 3u + uint(gl_FragCoord.y) * 5u + i_instance);
            if (m == 0u) discard;
            gl_SampleMask[0] = int(m);
        }
#endif
        base = srgb_to_linear(tb.rgb);
        vec3 nts = vec3(tn.xy * 2.0 - 1.0, 0.0);
        nts.z = sqrt(max(1.0 - dot(nts.xy, nts.xy), 0.0));
        rough = clamp(tn.z, 0.045, 1.0);
        metal = tn.w;
        // Blender's emission strength 1: 100 nits
        emissive = (gm.flags & MATF_EMISSIVE) == 0u ? vec3(0.0) :
                   (udim_on ? textureGrad(u_mat_tex[nonuniformEXT(emis_tex)], udim_uv, udim_gx, udim_gy).rgb : texture(u_mat_tex[nonuniformEXT(emis_tex)], muv, mbias).rgb) * 100.0;
        if ((gm.flags & MATF_WORLD_GRID) != 0u) { // Unreal's grid material: 1 m squares and lines, faint 10 cm lines
            vec3 an = abs(n);
            vec2 wp = an.z >= an.x && an.z >= an.y ? i_world.xy : an.x >= an.y ? i_world.yz : i_world.xz;
            // (2026-11-23, "the checker blurs a few meters away") the pixel's footprint at the OUTPUT's size (a smaller render
            // under TSR: its jitter resolves the thinner lines over the frames, as the textures' mip bias), and each set of
            // 10 cm lines faded by its own axis (the old max(fw.x, fw.y) took the depth axis of a grazing floor for both and
            // wiped them a few meters away), between 4 and 2 pixels apart (before 3.3 to 2.5)
            float tsr_px = f.tsr_size.z > 0.0 ? f.tsr_size.x / f.tsr_size.z : 1.0;
            vec2 fw = max(fwidth(wp) * tsr_px, vec2(1e-3));
            vec2 d1 = abs(fract(wp / 100.0 - 0.5) - 0.5) * 100.0 / fw, d2 = abs(fract(wp / 10.0 - 0.5) - 0.5) * 10.0 / fw;
            float l1 = 1.0 - clamp(min(d1.x, d1.y) - 0.5, 0.0, 1.0);
            vec2 l2a = 1.0 - clamp(d2 - 0.5, 0.0, 1.0), f2 = clamp((10.0 / fw - 2.0) * 0.5, 0.0, 1.0); // (the 10 cm lines fade before they alias)
            float l2 = max(l2a.x * f2.x, l2a.y * f2.y);
            float sq = mod(floor(wp.x / 100.0) + floor(wp.y / 100.0), 2.0);
            base *= (sq > 0.5 ? 0.9 : 1.0) * mix(1.0, 0.45, l1) * mix(1.0, 0.8, l2);
        }
#ifdef RE_EXT
        uint xflags = 0u;
        [[dont_flatten]] if ((gm.flags & MATF_EXT) != 0u) { // the vertex color and the detail normal (M5)
            vec4 e6 = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)].e6;
            xflags = uint(e6.w + 0.5); // (e6.x and .w as float values: material_gpu.inc write_slot)
            if ((xflags & MATX_VCOL_TINT) != 0u) base *= i_vcol.rgb; // (the Color Attribute multiplying the Base Color)
            // (B11) a landscape's painted layers (Unreal's Landscape Layer Blend): the vertex color's r, g, b the weights of
            // three other materials over this one (what is left: this one), each read at the world's xy over its tile
            [[dont_flatten]] if ((xflags & MATX_LAYERS) != 0u) {
                GpuMatExt X = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)];
                vec3 lw = i_vcol.a < 0.5 ? clamp(i_vcol.rgb, 0.0, 1.0) : vec3(0.0); // (alpha 1: no painted layers on this mesh)
                float ws = lw.x + lw.y + lw.z;
                if (ws > 1.0) lw /= ws, ws = 1.0;
                float w0 = 1.0 - ws;
                vec3 lb = base * w0, ln = nts * w0;
                float lr = rough * w0, lmt = metal * w0;
                for (int k = 0; k < 3; k++) {
                    if (lw[k] < 0.002) continue;
                    GpuMaterial lm = GpuMaterialBuf(f.materials).m[uint(X.e3[k] + 0.5)];
                    vec2 luv = i_world.xy / max(X.e5[k], 1.0);
                    vec4 tb2 = texture(u_mat_tex[nonuniformEXT(lm.tex_base)], luv, mbias), tn2 = texture(u_mat_tex[nonuniformEXT(lm.tex_nrm)], luv, mbias);
                    vec3 n2 = vec3(tn2.xy * 2.0 - 1.0, 0.0);
                    n2.z = sqrt(max(1.0 - dot(n2.xy, n2.xy), 0.0));
                    lb += srgb_to_linear(tb2.rgb) * lw[k], ln += n2 * lw[k], lr += tn2.z * lw[k], lmt += tn2.w * lw[k];
                }
                base = lb, nts = normalize(ln + vec3(0.0, 0.0, 1e-4)), rough = clamp(lr, 0.045, 1.0), metal = lmt;
            }
            uint ds = uint(e6.x + 0.5);

            if (ds != 0u) { // another material's baked normal, tiled finer, blended over this one's (whiteout)
                vec3 dn = vec3(texture(u_mat_tex[nonuniformEXT(GpuMaterialBuf(f.materials).m[ds].tex_nrm)], muv * e6.y, mbias).xy * 2.0 - 1.0, 0.0);
                dn.xy *= e6.z;
                dn.z = sqrt(max(1.0 - dot(dn.xy, dn.xy), 0.0));
                nts = normalize(vec3(nts.xy + dn.xy, nts.z * dn.z));
            }
        }
#endif
        n = normalize(tbn * nts);
#ifdef RE_EXT
        // (the eye: the iris a shallow dish facing the look, its normal tilted toward the center: a light from one side
        // pools on the far side of the iris, as the cornea's caustic does; the sclera keeps the sphere's)
        [[dont_flatten]] if ((gm.flags & MATF_EYE) != 0u) {
            vec4 e7 = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)].e7;
            vec2 d = muv - 0.5;
            float r = length(d) / max(e7.x, 1e-3);
            vec3 F = normalize(vec3(inst.row0.x, inst.row1.x, inst.row2.x) + vec3(1e-6));
            vec3 rad = T * d.x + B * d.y;
            vec3 ni = normalize(F - rad / max(length(rad), 1e-12) * (0.6 * min(r, 1.0)));
            n = normalize(mix(n, ni, smoothstep(1.0, 0.9, r)));
            if (r < 0.9) g_retro = uint(round(GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)].e5.w)); // (Q13) Eye Glow on the iris (round, not + 0.5: past 2^23 a float steps by 1 and the half rounded the color bits away)
        }
#endif
        if (dot(n, v) < 0.0) n = normalize(n - v * dot(n, v) * 1.01); // a normal map tilting past the view
#ifdef RE_EXT
        [[dont_flatten]] if ((xflags & MATX_LOBES) != 0u) { // the Principled's extra lobes (M1b)
            uint es = floatBitsToUint(inst.emissive_rough.x);
            vec4 e1 = GpuMatExtBuf(f.materials).x[es].e1, e2 = GpuMatExtBuf(f.materials).x[es].e2;
            g_f0d = e2.rgb;
            vec3 ta = cos(e1.w) * tbn[0] + sin(e1.w) * tbn[1]; // (the anisotropy's tangent, turned by its rotation)
            g_ext = ext_pack(es, normalize(ta - n * dot(ta, n) + vec3(1e-6)));
            g_e12 = GpuMatExtBuf(f.materials).x[es].e12;
            if ((gm.flags & MATF_EYE) == 0u) { uint gr = uint(round(GpuMatExtBuf(f.materials).x[es].e5.w)); g_retro = gr > 65535u ? gr : 0u; } // (e5.w is a glass's IOR without it: below 2^16) // (Q27.8) a road sign, a reflector
#ifndef RE_TRANSLUCENT
            // the skin's checkerboard (A9.3a, GI_SSS: one sample, the Lit view): a subsurface material with its scattering
            // distance, its slot in the alpha's 11 bits (half floats: exact to 2048). Even pixels its diffuse light, odd
            // ones the rest; sss.comp puts them back together, the diffuse spread by Burley's profile
            if ((f.gi_flags & GI_SSS) != 0u && es < 2047u) {
                vec4 e3 = GpuMatExtBuf(f.materials).x[es].e3, e7 = GpuMatExtBuf(f.materials).x[es].e7;
                if (e3.w > 0.001 && e7.z > 0.0) g_ext |= ((uint(gl_FragCoord.x) + uint(gl_FragCoord.y)) & 1u) == 0u ? SSS_D : SSS_S;
            }
#endif
        }
        // (Q11) the Toon shading model: lit in bands (shade_light's toon path), no reflection of its own (its stylized
        // highlight instead): rough and dielectric for the indirect part
#ifdef RE_TOON_MAT
        if ((gm.flags & MATF_TOON) != 0u) g_toon = floatBitsToUint(inst.emissive_rough.x) + 1u, rough = 1.0, metal = 0.0;
#endif
#endif
#if defined(RE_EXT) && !defined(RE_TRANSLUCENT)
        // (2026-11-17, Q4) the Interior Mapping: the window turns glass, the room behind it its light through the glass
        [[dont_flatten]] if ((xflags & MATX_INTERIOR) != 0u) {
            vec3 room;
            float win = interior(f, gm, floatBitsToUint(inst.emissive_rough.x), muv, tbn, v, room);
            if (win > 0.0) {
                float F = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
                base = vec3(0.015), metal = 0.0, rough = 0.03;
                emissive += room * (1.0 - F);
            }
        }
        // (2026-11-16) the Wet Surface node: the material's own wetness, rain or not (water.glsl mat_wet)
        [[dont_flatten]] if ((xflags & MATX_WET) != 0u) {
            vec3 ng = normalize(i_normal);
            if (dot(ng, v) < 0.0) ng = -ng;
            mat3 Mc = transpose(mat3(inst.row0.xyz, inst.row1.xyz, inst.row2.xyz));
            mat3 R = mat3(normalize(Mc[0]), normalize(Mc[1]), normalize(Mc[2]));
            mat_wet(f, floatBitsToUint(inst.emissive_rough.x), gm.param_off, ng, i_world - vec3(inst.row0.w, inst.row1.w, inst.row2.w),
                    R, i_vcol, n, base, rough, metal);
        }
#endif
#ifdef RE_TRANSLUCENT
        g_mflags = gm.flags;
        if ((gm.flags & MATF_EXT) != 0u) { // (Transmission: glass lets that share through)
            GpuMatExt gx = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)];
            float tw = gx.e2.w;
            // (2026-11-17, Q4) its weight a parameter (a Material Instance's, a Blueprint's Set Scalar Parameter Value)
            if (gx.e8.w > 0.5) tw = clamp(WetParamBuf(f.mat_params).p[gm.param_off + uint(gx.e8.w + 0.5) - 1u].x, 0.0, 1.0);
            tw *= 1.0 - metal;
            g_opacity *= 1.0 - tw;
            // what is behind seen through it: its base color by the transmission (as Blender), absorbed along the way in
            // (Beer-Lambert: its thickness, longer at a grazing angle)
            float path = ((gm.flags & MATF_REFRACT) != 0u ? max(gx.e7.x, 0.1) : 1.0) / max(abs(dot(n, v)), 0.25);
            g_tint = mix(vec3(1.0), clamp(base, 0.0, 1.0), tw) * exp(-gx.e8.rgb * path);
        }
#endif
    }
#ifdef RE_EXT
    // The decals (M6, decal.c): this pixel's 16 px tile's list (decal_bin.comp), each box tested; inside, the decal's
    // material laid over the surface before the light (its alpha: the share; faded by the angle to its axis and its
    // depth in the box). The textures' gradients from the position's (the loop is not uniform: no implicit ones)
    [[dont_flatten]] if (f.decal_count != 0u) {
        uvec2 tp = uvec2(max(gl_FragCoord.xy - f.viewport.xy, vec2(0.0))) / DECAL_TILE;
        uint t0 = (min(tp.y, f.decal_tiles_y - 1u) * f.decal_tiles_x + min(tp.x, f.decal_tiles_x - 1u)) * (DECAL_TILE_MAX + 1u);
        uint nd = min(UintBuf(f.decal_tiles).v[t0], DECAL_TILE_MAX);
        vec3 dpx = dFdx(i_world), dpy = dFdy(i_world);
        for (uint k = 0u; k < nd; k++) {
            Decal d = DecalBuf(f.decals).d[UintBuf(f.decal_tiles).v[t0 + 1u + k]];
            vec3 lp = vec3(dot(d.r0.xyz, i_world) + d.r0.w, dot(d.r1.xyz, i_world) + d.r1.w, dot(d.r2.xyz, i_world) + d.r2.w);
            if (any(greaterThan(abs(lp), vec3(1.0)))) continue;
            float cosa = d.v_axis.w, facing = dot(n, -d.axis.xyz);
            float a = d.axis.w * smoothstep(cosa, min(cosa + 0.15, 1.0), facing) * (1.0 - smoothstep(0.8, 1.0, abs(lp.x)));
            if (a <= 0.001) continue;
            vec2 duv = vec2(lp.y, -lp.z) * 0.5 + 0.5;
            vec2 gx = vec2(dot(d.r1.xyz, dpx), -dot(d.r2.xyz, dpx)) * 0.5, gy = vec2(dot(d.r1.xyz, dpy), -dot(d.r2.xyz, dpy)) * 0.5;
            GpuMaterial dm = GpuMaterialBuf(f.materials).m[uint(d.u_axis.w + 0.5)];
            vec4 db = textureGrad(u_mat_tex[nonuniformEXT(dm.tex_base)], duv, gx, gy);
            vec4 dn = textureGrad(u_mat_tex[nonuniformEXT(dm.tex_nrm)], duv, gx, gy);
            a *= db.a;
            base = mix(base, srgb_to_linear(db.rgb), a);
            rough = mix(rough, clamp(dn.z, 0.045, 1.0), a);
            metal = mix(metal, dn.w, a);
            vec2 dxy = dn.xy * 2.0 - 1.0; // (its tangent space: U along its Y, V down its Z)
            n = normalize(mix(n, normalize(n + d.u_axis.xyz * dxy.x - d.v_axis.xyz * dxy.y), a));
        }
    }
#endif
#if !defined(RE_TRANSLUCENT) && (defined(RE_EXT) || defined(RE_LSM))
    // (2026-11-16) the rain on the surfaces only in the RE_EXT variants (render.c picks them while it rains) and RE_LSM:
    // in the plain ones its code made the driver spill 48 bytes in mesh_rt and mesh_rt_t (the main pass 2.8% slower
    // with no rain at all, notas-dev/render.md); without it g_rcp stays 0 and the film's code goes away
    [[dont_flatten]] if (rain_on(f)) { // the rain: wet, puddles and streams, streaks down the walls (water.glsl; under a roof: dry)
        vec3 ng = normalize(i_normal);
        if (dot(ng, v) < 0.0) ng = -ng;
        float own = rain_own_wet(f, inst.ids.x);
        rain_shade(f, i_world, ng, own, n, base, rough, metal);
        // (2026-11-15) drops on what moves and is wet: in the part's own space (they ride on it), running down by the world
        [[dont_flatten]] if (own > 0.1 && f.water[39].x < 0.5) {
            // (the part's rotation alone, R of M = R S: a skeletal part is scaled unevenly, and its own space squashed the
            // drops into lines; R's frame is metric and turns with the part)
            mat3 Mc = transpose(mat3(inst.row0.xyz, inst.row1.xyz, inst.row2.xyz)); // (its columns: M's)
            mat3 R = mat3(normalize(Mc[0]), normalize(Mc[1]), normalize(Mc[2])), Rt = transpose(R);
            vec3 pr = Rt * (i_world - vec3(inst.row0.w, inst.row1.w, inst.row2.w));
            rain_actor_drops(f, own, pr * 0.01, Rt * ng, Rt * vec3(0.0, 0.0, -1.0), R, distance(i_world, f.camera_pos.xyz), base);
        }
    }
#elif defined(RE_TRANSLUCENT)
    [[dont_flatten]] if (rain_on(f)) { // (2026-11-15) a window in the rain: drops staying and running down it (water.glsl)
        vec3 ng = normalize(i_normal);
        if (dot(ng, v) < 0.0) ng = -ng;
        g_opacity = rain_glass(f, i_world, ng, g_opacity);
    }
#endif
    // Specular anti-aliasing (Kaplanyan and Hoffman 2016): the normal's change across the pixel widens the lobe. Far
    // away a bumpy material's normals vary within a pixel; without it the far grass sparkled and read as wet
    {
        vec3 nx = dFdx(n), ny = dFdy(n);
        float a2 = rough * rough * rough * rough + min(0.5 * (dot(nx, nx) + dot(ny, ny)), 0.18);
        rough = sqrt(sqrt(min(a2, 1.0)));
        // (2026-11-15, "a tela fica flickando em cores meio roxas"; "o céu fica piscando") the rain's film too: its alpha
        // went down to ~1.6e-4 with no filtering, and the sun on a puddle's ripples peaked past what a half float holds
        // (Inf/NaN the TSR spread as purple and black blotches). The base's floor (roughness 0.045) and the same widening
        // by its normal's change across the pixel (the derivatives outside the branch: the quad's pixels may differ)
        vec3 rcn = rc_n(), rcx = dFdx(rcn), rcy = dFdy(rcn);
        [[dont_flatten]] if (g_rcp.y != 0u) {
            vec2 wa = unpackHalf2x16(g_rcp.y);
            float ca = sqrt(max(wa.y * wa.y + min(0.5 * (dot(rcx, rcx) + dot(rcy, rcy)), 0.18), 0.045 * 0.045 * 0.045 * 0.045));
            g_rcp.y = packHalf2x16(vec2(wa.x, ca));
        }
    }
    uint mode = f.view_mode;
    if (mode == VIEW_UNLIT) { o_color = vec4(base + emissive, 1.0); return; }
    if (mode == VIEW_TEXTURE) { o_color = vec4(base, 1.0); return; } // the textures come with .rasset materials
    if (mode == VIEW_NORMALS) { o_color = vec4(n * 0.5 + 0.5, 1.0); return; }
    if (mode == VIEW_ROUGHNESS) { o_color = vec4(vec3(inst.emissive_rough.w), 1.0); return; }
    if (mode == VIEW_METALLIC) { o_color = vec4(vec3(metal), 1.0); return; }
    if (mode == VIEW_SOLID || mode == VIEW_SOLID_RANDOM) {
        // Blender's studio light: a key from above-left of the camera, a soft fill, a rim; no shadows
        vec3 albedo = vec3(0.8);
        if (mode == VIEW_SOLID_RANDOM) {
            uint h = inst.ids.x * 2654435761u;
            float hue = float(h >> 8) / 16777216.0;
            albedo = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0) * 0.55 + 0.3;
        }
        vec3 key = normalize(-f.camera_fwd.xyz * 0.5 + f.camera_up.xyz * 0.7 - f.camera_right.xyz * 0.5);
        vec3 fill = normalize(-f.camera_fwd.xyz * 0.6 - f.camera_up.xyz * 0.2 + f.camera_right.xyz * 0.8);
        float nv = max(dot(n, v), 0.0);
        vec3 c = albedo * (0.18 + 0.72 * max(dot(n, key), 0.0) + 0.25 * max(dot(n, fill), 0.0));
        c += vec3(0.06) * pow(1.0 - nv, 3.0);                                    // rim
        c += vec3(0.12) * pow(max(dot(reflect(-v, n), key), 0.0), 24.0);          // a soft highlight
        o_color = vec4(c, 1.0);
        return;
    }
    // --- The technologies at work (render.h): what the GPU cull drew, the triangles, the mobility, the texel density ---
    // a plain light for the visualizations (the shapes read); a define: only the modes that show it compute it
#define shade (0.35 + 0.65 * max(dot(n, normalize(v + f.camera_up.xyz * 0.6)), 0.0))
    if (mode == VIEW_LOD) { // the LOD the GPU cull picked per instance (cull.comp): 0 green, 1 lime, 2 orange, 3 red
        vec3 lc[4] = vec3[4](vec3(0.15, 0.8, 0.25), vec3(0.7, 0.85, 0.1), vec3(1.0, 0.55, 0.05), vec3(0.95, 0.12, 0.1));
        uint lod = (UintBuf(f.vis).v[i_instance] & 0xFFu) % 4u; // the cull's choice (only this view reads it)
        o_color = vec4(lc[lod] * shade, 1.0);
        return;
    }
    if (mode == VIEW_OBJECT_COST) { // what each object costs (sugestÃ£o #65, editor_cost.c): green light, yellow, red heavy
        float c = (f.object_cost.x | f.object_cost.y) != 0u ? clamp(uintBitsToFloat(UintBuf(f.object_cost).v[i_instance]), 0.0, 1.0) : 0.0;
        vec3 col = c < 0.5 ? mix(vec3(0.12, 0.72, 0.22), vec3(0.98, 0.84, 0.12), c * 2.0) : mix(vec3(0.98, 0.84, 0.12), vec3(0.95, 0.12, 0.08), c * 2.0 - 1.0);
        o_color = vec4(col * shade, 1.0);
        return;
    }
    if (mode == VIEW_TRIANGLES) { // a color per triangle of each instance
        uint h = (inst.ids.x * 2654435761u) ^ (uint(gl_PrimitiveID) * 2246822519u);
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
        vec3 c = vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
        o_color = vec4((c * 0.7 + 0.3) * shade, 1.0);
        return;
    }
    if (mode == VIEW_MOBILITY) { // like Unreal: Static white, Movable blue (orange while it moves: out of the GI's rays)
        vec3 c = (inst.ids.y & INST_STATIC) != 0u ? vec3(0.92) : (inst.ids.y & INST_MOVING) != 0u ? vec3(1.0, 0.45, 0.05) : vec3(0.2, 0.45, 1.0);
        o_color = vec4(c * shade, 1.0);
        return;
    }
    if (mode == VIEW_TEXEL_DENSITY || mode == VIEW_SURFACE_CACHE) {
        // the atlas this surface reads its indirect light from: the lightmap (Static after Build Lighting), else the surface
        // cache's chart; none (magenta): the probes light it
        // (the Surface Cache view always shows the cache, also on a Static mesh that reads its lightmap)
        bool lm = mode == VIEW_TEXEL_DENSITY && (f.gi_flags & GI_LIGHTMAPS) != 0u && (inst.ids.y & INST_LIGHTMAP) != 0u;
        vec4 st = lm ? inst.lightmap_st : ((f.gi_flags & GI_SURFACE_CACHE) != 0u && (inst.ids.y & INST_CROWD) == 0u ? CacheStBuf(f.cache_st).st[i_instance] : vec4(0.0));
        float atlas = lm ? float(textureSize(u_lightmap0, 0).x) : f.cache_params.x;
        if (st.x <= 0.0) { o_color = vec4(vec3(0.6, 0.05, 0.6) * shade, 1.0); return; }
        vec2 g = (i_uv2 * st.xy + st.zw) * atlas;
        float grid = texel_grid(g);
        if (mode == VIEW_TEXEL_DENSITY) {
            // cm per texel on screen: green at the target (10 cm lightmap, 15 cm cache), red denser, blue coarser
            float cm = 0.5 * (length(dFdx(i_world)) / max(length(dFdx(g)), 1e-6) + length(dFdy(i_world)) / max(length(dFdy(g)), 1e-6));
            float t = log2(cm / (lm ? 10.0 : 15.0));
            vec3 c = t < 0.0 ? mix(vec3(0.2, 0.85, 0.3), vec3(1.0, 0.15, 0.1), min(-t, 1.0)) : mix(vec3(0.2, 0.85, 0.3), vec3(0.15, 0.3, 1.0), min(t * 0.5, 1.0));
            o_color = vec4(c * shade * mix(0.45, 1.0, grid), 1.0);
            return;
        }
        // Surface Cache: the light the cache holds (direct + indirect) on the base color, its texels outlined; a buried
        // texel (inside another mesh, left out of the reads) shows dark red
        vec2 uv = g / atlas;
        vec4 dt = texture(u_cache_direct, uv), it = texture(u_cache_indirect, uv);
        vec3 c = it.a < 0.25 ? vec3(0.5, 0.0, 0.0) / pre_exposure(f)
                             : base * ((dt.rgb + it.rgb * f.gi_params.x) / max(it.a, 1e-3)) * GI_LOAD + emissive;
        o_color = vec4(min(c * mix(0.6, 1.0, grid) * pre_exposure(f), vec3(60000.0)), 1.0);
        return;
    }
    if ((inst.ids.y & INST_CROWD) != 0u && inst.lightmap_st.w > 1.5) base *= inst.lightmap_st.rgb; // (Q27.9: a crowd agent's look, crowd.comp)
    if (mode == VIEW_LIGHTING_ONLY) { base = vec3(0.5); metal = 0.0; emissive = vec3(0.0); }

    vec3 diff = base * (1.0 - metal);
#ifdef RE_EXT
    if ((g_ext & SSS_BITS) == SSS_S) diff = vec3(0.0); // (the skin's specular half: its diffuse light is the even pixels')
    if ((g_ext & SSS_BITS) == SSS_D) emissive = vec3(0.0);
#endif
#ifdef RE_EXT
    vec3 f0 = mix(g_f0d, base, metal); // (the dielectric's: 0.04, or the Principled's IOR, Specular IOR Level and Tint)
#else
    vec3 f0 = mix(vec3(0.04), base, metal);
#endif
    float a = rough * rough;
    vec3 color = vec3(0.0);
    int shadow_light = int(f.sun_radiance.w);
    // Ray traced shadows (High/Epic, RE_RT): the lights with Cast Shadows, exact, up to f.shadow_budget of them per
    // pixel; the sun's map is not read then. None in a helper invocation: its result only feeds derivatives (a quad's
    // pixels off the triangle), and their rays are waste
    bool rt_shadows = (f.gi_flags & GI_RT_SHADOWS) != 0u;
    bool vsm_sun = (f.gi_flags & GI_VSM) != 0u; // the sun on its virtual shadow maps even when the others are traced
    // An instance under ~3 pixels across (a field seen from far): no shadow rays, no reflection ray. Its few pixels pay
    // a whole 2x2 quad each (4x the shading) and neither shows at that size
    float px_cm = length(fwidth(i_world)); // cm per pixel here
    float obj_cm = 87.0 * sqrt(max(dot(inst.row0.xyz, inst.row0.xyz), max(dot(inst.row1.xyz, inst.row1.xyz), dot(inst.row2.xyz, inst.row2.xyz))));
    bool tiny = obj_cm < 3.0 * px_cm && (inst.ids.y >> INST_SHAPE_SHIFT) != 4u; // (4: the plane, a floor)
    uint budget = gl_HelperInvocation || tiny ? 0u : f.shadow_budget;
    Surf s;
    s.p = g_pw;
    s.n = n;
    s.v = v;
    s.diff = diff;
    s.f0 = f0;
    s.a = a;
#ifdef RE_TERMINATOR
    s.shadow_o = i_card ? g_pw + n * (0.5 + i_ray_bias) : shadow_terminator_origin(i_world, dot(i_normal, v) < 0.0 ? -1.0 : 1.0) + n * (0.25 + i_ray_bias);
#else
    s.shadow_o = g_pw + n * (0.5 + i_ray_bias);
#endif
#ifdef RE_RT
    s.rot = ign(gl_FragCoord.xy) * 6.2831853;
#else
    s.rot = 0.0;
#endif
    // the directional lights, then the local ones of this point's cell in the light grid (lights.glsl)
    // the sun's virtual shadow maps, once (before the loop: the PCSS's temporaries don't stay live through the lights)
    float sun_vis = -1.0;
    if (shadow_light >= 0 && (vsm_sun || !rt_shadows)) {
        vec3 sl = -f.lights[shadow_light].direction;
#ifdef RE_RT
#ifndef RE_MASKED
        // (the hybrid: vsm.glsl leaves the near casters' shadows to a ray. Not on a Masked surface: cards shadowing cards
        // are a speckle the map draws as well, and their rays crossing every layer cost 6.7 ms in the heavy scene)
        g_vsm_rt = (f.gi_flags & GI_SUN_HYBRID) != 0u && budget > 0u;
#endif
#endif
#if defined(RE_EXT) && !defined(RE_TRANSLUCENT)
        // (Q4) a thin sheet lit from behind: the sun's shadow taken on its far side (its own face does not shade it)
        sun_vis = dot(n, sl) > 0.0 ? shadow_factor(f, g_pw + n * i_disp_bias, n)
                : g_ext != 0u && (g_ext & SSS_BITS) == 0u && GpuMatExtBuf(f.materials).x[(g_ext & 4095u) - 1u].e11.w > 0.5 ? shadow_factor(f, i_world, -n) : 1.0;
#else
        sun_vis = dot(n, sl) > 0.0 ? shadow_factor(f, g_pw + n * i_disp_bias, n) : 1.0;
#endif
#ifdef RE_RT
        if (g_vsm_ray) // (Masked by their alpha; by the share the map left to the ray: vsm.glsl)
            sun_vis = min(sun_vis + (trace_visible_mask(f, s.shadow_o, sl, 1e7, RT_MASK_SHADOW) ? g_vsm_ray_w : 0.0), 1.0);
#endif
    }
    uint nlights = f.light_count, budget0 = budget; // what this pixel evaluates (the Light Complexity, Shader Cost views)
#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
    g_ml_seed = ml_hash(uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 13u)) ^ ml_hash(f.frame_index); // (other picks every frame)
#endif
#ifndef RE_RT
    float sun_l = 0.0; // (A4b: the sun's part of the light, for the contact shadows; not in the ray traced variants: rays
                       // shade the sun's near casters there, and the float spilled 32 B of their 128 registers)
    for (uint i = 0; i < f.light_count; i++) {
        vec3 lc = shade_light(f, f.lights[i], 0xFFFFFFFFu, int(i) == shadow_light ? sun_vis : -1.0, s, budget, false);
#if defined(RE_EXT) || defined(RE_LSM)
        // (2026-11-17, Q4) the sun through glass without rays: the glass boxes' tints
        [[dont_flatten]] if (int(i) == shadow_light && (f.gi_flags & GI_GLASS_SHADOW) != 0u && f.rgi_info.y > 0u && dot(lc, lc) > 0.0)
            lc *= glass_box_tint(f, i_world, -f.lights[i].direction);
#endif
        if (int(i) == shadow_light) sun_l = dot(lc, vec3(0.2126, 0.7152, 0.0722));
        color += lc;
    }
#else
    for (uint i = 0; i < f.light_count; i++) {
#if defined(RE_EXT) && !defined(RE_TRANSLUCENT)
        vec3 lc = shade_light(f, f.lights[i], 0xFFFFFFFFu, int(i) == shadow_light ? sun_vis : -1.0, s, budget, false);
        // (2026-11-17, Q4) the sun through glass: tinted by the panes it crosses (their color, their absorption; the
        // software rays: the glass boxes)
        [[dont_flatten]] if (int(i) == shadow_light && (f.gi_flags & GI_GLASS_SHADOW) != 0u && dot(lc, lc) > 0.0)
#ifdef RE_SWRT
            lc *= glass_box_tint(f, i_world, -f.lights[i].direction);
#else
            lc *= glass_tint(f, s.shadow_o, -f.lights[i].direction);
#endif
        color += lc;
#else
        color += shade_light(f, f.lights[i], 0xFFFFFFFFu, int(i) == shadow_light ? sun_vis : -1.0, s, budget, false);
#endif
    }
#endif
    LightLists ll = light_lists(f, i_world);
    for (uint L = ll.lp; L < LG_LEVELS; L++) {
        uvec2 rg = light_range(ll, L);
        for (uint k = 0u; k < rg.y; k++) {
            uint it = light_item(f, rg, k);
            if (light_take(it, L, ll.lp)) {
                color += shade_light(f, local_light(f, it), it & LG_SLOT_MASK, -1.0, s, budget, true);
                nlights++;
            }
        }
    }
#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
    if (g_ml_n == 1u && !light_is_area(LightBuf(f.light_buf).l[g_ml_sel.x])) { // one hard light: its exact ray (as before)
        GpuLight L0 = LightBuf(f.light_buf).l[g_ml_sel.x];
        vec3 l0;
        float d0;
        ml_geom(L0, s.p, l0, d0);
        if (trace_visible_mask(f, s.shadow_o, l0, d0, RT_MASK_SHADOW)) color += g_ml_e;
    } else if (g_ml_w > 0.0) { // the two picked lights' rays (a quarter of the preset's: Epic 2 each, Cinematic 4, High 1)
        uint rays = max(uint(f.sky_params.y) / 4u, 1u);
        float r0 = ign(gl_FragCoord.xy + 5.588238 * float(f.frame_index & 63u)); // (turned every frame: the TSR averages them)
#ifdef RE_DEN
      if ((f.gi_flags & GI_RESTIR) != 0u) {
        // ReSTIR DI (A9.1a; Bitterli et al. 2020), temporal: this frame's pick (one light among all of them in proportion
        // to its unshadowed light: W = their sum / its) and last frame's reservoir of this point, its light weighed by
        // its light here, merged (one kept in proportion to p * W * M); then one light's rays, its visibility. A
        // reservoir whose light was hidden is not kept (visibility reuse): the visible lights spread from frame to
        // frame, the rays stop going to lights behind walls. The share that arrives: p(y) W V(y) / the sum, filtered by
        // the denoiser as the two picks' visibility was
        uint y = g_ml_sel.x;
        float py = ml_target(f, y, s), wsum = g_ml_w, M = 1.0;
        uint pr = resv_prev(f, s.p, inst.ids.x + 1u);
        if (pr != 0u) {
            uint yp = pr & 0xFFFFu;
            float mp = float((pr >> 16u) & 7u), wp_ = unpackHalf2x16((pr >> 19u) << 2u).x;
            float pp = ml_target(f, yp, s), wp = pp * wp_ * mp;
            M += mp;
            if (wp > 0.0) {
                wsum += wp;
                if (float(ml_hash(g_ml_seed ^ 0x51ED27u) & 0xFFFFu) * (1.0 / 65536.0) * wsum < wp) y = yp, py = pp;
            }
        }
        float W = py > 0.0 ? wsum / (M * py) : 0.0;
        GpuLight Ly = LightBuf(f.light_buf).l[y];
        vec3 ly;
        float dy;
        ml_geom(Ly, s.p, ly, dy);
        float vy = W > 0.0 ? light_visibility(f, s.shadow_o, Ly, ly, dy, rays, r0 * 6.2831853, RT_MASK_SHADOW) : 0.0;
        g_resv = vy > 0.0 && y < 65536u ? (y | (uint(min(M, 7.0)) << 16u) | ((packHalf2x16(vec2(min(W, 60000.0), 0.0)) >> 2u) << 19u)) : 0u;
        vec3 e = min(g_ml_e * pre_exposure(f), vec3(60000.0));
        g_split = uvec2(packHalf2x16(e.rg), packHalf2x16(vec2(e.b, clamp(py * W * vy / g_ml_w, 0.0, 4.0)))) | uvec2(1u, 0u);
      } else {
#endif
        GpuLight L0 = LightBuf(f.light_buf).l[g_ml_sel.x];
        vec3 l0;
        float d0;
        ml_geom(L0, s.p, l0, d0);
        float v0 = light_visibility(f, s.shadow_o, L0, l0, d0, rays, r0 * 6.2831853, RT_MASK_SHADOW), v1 = v0;
        if (g_ml_sel.y != g_ml_sel.x) { // (the same light twice: the same rays; a single area light keeps its 2 rays as before)
            GpuLight L1 = LightBuf(f.light_buf).l[g_ml_sel.y];
            vec3 l1;
            float d1;
            ml_geom(L1, s.p, l1, d1);
            v1 = light_visibility(f, s.shadow_o, L1, l1, d1, rays, fract(r0 + 0.618034) * 6.2831853, RT_MASK_SHADOW);
        }
        vec3 e = min(g_ml_e * pre_exposure(f), vec3(60000.0));
        g_split = uvec2(packHalf2x16(e.rg), packHalf2x16(vec2(e.b, 0.5 * (v0 + v1)))) | uvec2(1u, 0u); // (never zero)
#ifdef RE_DEN
      }
#endif
    }
#endif
    // Indirect light. Diffuse: the baked lightmap (Static meshes after Build Lighting), else the GI probes, else the
    // sky light's SH. Specular: the sky reflection, darkened where the GI says the sky is hidden, or (RE_RT) a traced
    // reflection on smooth surfaces. Everything here is computed fresh each frame: nothing to ghost.
    float sky = f.sky_params.x;
    vec3 sky_irr = sky > 0.0 ? sh_irradiance(f, n) / PI : vec3(0.0); // E / pi
    vec3 irr = sky_irr;
    bool gi = false, cached = false;
    int gi_src = 0; // the GI Source view: 0 the sky light, 1 the lightmap, 2 the surface cache, 3 the probes
    // (uv2 < 0: no atlas here: a welded heavy mesh, or an imported mesh's simplified LOD; mesh_build.c)
    bool atlas = i_uv2.x >= 0.0;
    if ((f.gi_flags & GI_LIGHTMAPS) != 0u && (inst.ids.y & INST_LIGHTMAP) != 0u && atlas) {
        // never across a wall standing on the surface or from a texel inside another mesh (gi.glsl atlas_read)
        irr = lightmap_read(u_lightmap0, inst.lightmap_st, i_uv2) * (GI_LOAD * f.gi_params.w);
        gi = true;
        gi_src = 1;
    } else {
        // the surface cache where this mesh has a chart (real-time radiosity on its own surface), the probes elsewhere or
        // while its texels warm up
        // a Movable actor while it moves: its texels read wider (they followed it in blotches)
        vec2 wrap = MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].uv2_wrap; // a round mesh's seam
        // (a crowd's part has no chart: its slot is past the cache's table, the probes light it)
        vec4 cc = (inst.ids.y & INST_CROWD) != 0u || !atlas ? vec4(0.0)
                  : (inst.ids.y & INST_MOVING) != 0u ? cache_indirect_soft(f, u_cache_indirect, i_instance, i_uv2, wrap)
                                                      : cache_indirect(f, u_cache_indirect, i_instance, i_uv2, wrap);
        cached = cc.a > 0.0;
        if (cc.a < 1.0 && (f.gi_flags & GI_PROBES) != 0u) {
            irr = probe_irradiance(f, u_probe_irr, u_probe_dist, i_world, n, v) * f.gi_params.x;
            gi = true;
            gi_src = 3;
        }
        if (cached) {
            irr = mix(irr, cc.rgb * f.gi_params.x, cc.a);
            gi = true;
            if (cc.a >= 0.5) gi_src = 2;
        }
    }
#ifndef RE_RT
    // (Q27.12) the GI's fallback (no lightmap, no probes, no cache: Low, Android, Quest): the sky light through the AO
    // volume's visibility here (out of the surface by half a cell), the ground's bounce on what faces down
    if (!gi && (f.gi_flags & GI_FALLBACK) != 0u) {
        GiFbBuf fb = GiFbBuf(uvec2(floatBitsToUint(f.water[24].x), floatBitsToUint(f.water[24].y)));
        float cs = f.water[25].y;
        vec3 g = (i_world + n * cs * 0.5 - vec3(f.water[24].zw, f.water[25].x)) / cs - 0.5;
        float vis = 1.0;
        if (all(greaterThanEqual(g, vec3(0.0))) && all(lessThan(g, vec3(63.0, 63.0, 15.0)))) {
            ivec3 c0 = ivec3(floor(g));
            vec3 t = g - vec3(c0);
            float v000 = gifb(fb, c0), v100 = gifb(fb, c0 + ivec3(1, 0, 0)), v010 = gifb(fb, c0 + ivec3(0, 1, 0)), v110 = gifb(fb, c0 + ivec3(1, 1, 0));
            float v001 = gifb(fb, c0 + ivec3(0, 0, 1)), v101 = gifb(fb, c0 + ivec3(1, 0, 1)), v011 = gifb(fb, c0 + ivec3(0, 1, 1)), v111 = gifb(fb, c0 + ivec3(1, 1, 1));
            vis = mix(mix(mix(v000, v100, t.x), mix(v010, v110, t.x), t.y), mix(mix(v001, v101, t.x), mix(v011, v111, t.x), t.y), t.z);
        }
        // (the sky's hidden part sees the surfaces around, lit by the sun's bounce off the ground: the patent's terrain
        // bounce map - where it sees out at all: none in a closed room (under 1/4 of the sky seen it fades out);
        // what faces down or sideways sees the ground when it sees out)
        irr = sky_irr * vis + f.water[26].rgb * ((1.0 - vis) * 0.8 * min(vis * 4.0, 1.0) + clamp(0.5 - 0.5 * n.z, 0.0, 1.0) * min(vis * 4.0, 1.0)); // (0.8: the closest to the RealGI, tools/gi_fallback_compare.py) // (0.8: the closest to the RealGI, tools/gi_fallback_compare.py)
    }
#endif
    // the Movable actors nearby (the bake and the probes miss them): the light they hide (AO) and the light they bounce
    vec4 cgi = contact_gi(f, i_world, n, inst.ids.x, cached);
    float ao = mix(1.0, cgi.a, f.pp[6].y); // (the Post Process Volume's Ambient Occlusion Intensity)
    vec3 bounce = cgi.rgb * f.gi_params.x; // the moving actors' bounce on a cached surface, every actor's elsewhere
    if (mode == VIEW_GI_ONLY) { o_color = vec4(min((irr * ao + bounce) * pre_exposure(f), vec3(60000.0)), 1.0); return; }
    if (mode == VIEW_GI_SOURCE) { // the lightmap green, the surface cache blue, the probes yellow, the sky light red;
        // darker where the contact GI of a Movable actor nearby takes light away
        vec3 sc[4] = vec3[4](vec3(0.95, 0.2, 0.15), vec3(0.2, 0.85, 0.3), vec3(0.2, 0.5, 1.0), vec3(1.0, 0.85, 0.15));
        o_color = vec4(sc[gi_src] * shade * mix(0.35, 1.0, ao), 1.0);
        return;
    }
    if (mode == VIEW_LIGHT_COMPLEXITY) { // the lights this pixel evaluates: 0 dark blue ... 16 or more white
        o_color = vec4(heat(float(nlights) / 16.0) * mix(0.75, 1.0, shade), 1.0);
        return;
    }
    float nv = max(dot(n, v), 1e-4);
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    vec3 refl = reflect(-v, n);
    // (2026-11-15) the rain's film: its reflection is the sharp one (the base's own is rough under it): the sky, the
    // screen's and the traced ray go along the film's normal
    bool rcoat = rc_w() > 0.02, rsteady = g_rcs > 0.02; // (the path by the steady film: water.glsl g_rcs)
    if (rsteady) refl = reflect(-v, rc_n());
    vec4 rc = clouds_toward(f, refl); // the clouds in the sky's reflection
    float rough_mix = clamp(rough * 1.3, 0.0, 1.0); // a rough surface reflects its diffuse light more than the sky
    // the sky sharp (with its clouds) on a smooth surface, blurred (its irradiance toward refl) on a rough one: a matte
    // wall showed the clouds' shapes, as if seen through it. High / Epic only: in the Low / Medium shader the second sky
    // made the driver spill registers (80 and 48 bytes out instead of 95 in: -3 to -6% of the main pass with crowds,
    // --shader-stats, notas-dev/render.md); there the sky stays sharp, as in 0.14
    vec3 sky_sharp = sky_radiance(f, refl, false) * rc.a + rc.rgb;
#ifdef RE_RT
    vec3 sky_ref = mix(sky_sharp, sky > 0.0 ? sh_irradiance(f, refl) / PI : sky_sharp, smoothstep(0.12, 0.55, rough));
#else
    vec3 sky_ref = sky_sharp;
#endif
    vec3 env = mix(sky_ref * sky, irr, rough_mix);
    float fog_occl = 1.0, sky_vis = 1.0; // (the sky this point sees: the analytic fog in front of it, weather.glsl fog_apply_occl)
    if (gi) { // the sky seen in a reflection only as much as the diffuse GI sees it (no sky light leaking indoors)
        float ls = dot(sky_irr, vec3(0.2126, 0.7152, 0.0722)), lg = dot(irr, vec3(0.2126, 0.7152, 0.0722));
        if (ls > 1e-4) sky_vis = clamp(lg / ls, 0.0, 1.0), env *= sky_vis, fog_occl = mix(0.1, 1.0, sqrt(sky_vis));
    }
    env *= ao; // a traced reflection (below) sees the occluder itself
    uvec2 cenv = rcoat ? ce_put(f, sky_sharp * sky * sky_vis * ao) : uvec2(0u); // (the film's: the sky sharp until a ray or the screen says better)
    // (A4c) where no ray traces reflections (Low, Medium, GPUs without ray tracing): the screen's, on smooth surfaces
#ifdef RE_LOCAL_SM
    if ((f.gi_flags & GI_SSR) != 0u && (rough < 0.35 || g_rcs > 0.25) && !tiny && mode == VIEW_LIT && !gl_HelperInvocation) {
        vec4 sr = ssr_trace(f, i_world + n * (1.0 + i_ray_bias), refl);
        env = mix(env, sr.rgb, sr.a * (1.0 - smoothstep(0.2, 0.35, rough)));
        if (rcoat) cenv = ce_put(f, mix(ce_get(f, cenv), sr.rgb, sr.a));
    }
#endif
    bool traced_refl = false;
#ifdef RE_DEN
    // A9.1d, the unified denoiser (GI_DENOISE): the indirect diffuse light by one ray (where no lightmap holds it) and a
    // rough reflection by one ray along a GGX sample, both noisy, filtered with the shadows by denoise.comp. Not with a
    // coat or a film (their lobes read the reflection: they keep the one ray below)
    bool den = (f.gi_flags & GI_DENOISE) != 0u && mode == VIEW_LIT && !tiny && !gl_HelperInvocation;
#ifdef RE_EXT
#ifdef RE_TOON_MAT
    if (g_toon != 0u) den = false; // (Q11: a toon surface keeps its indirect light flat, times its ambient share)
#endif
    if (den && g_ext != 0u) {
        uint xs = (g_ext & 4095u) - 1u;
        den = GpuMatExtBuf(f.materials).x[xs].e0.x <= 0.0 && GpuMatExtBuf(f.materials).x[xs].e4.z <= 1.0;
    }
#endif
    bool den_d = den && gi_src != 1, den_s = den && rough >= f.gi_params.z && rough < 0.9 && !(g_rcs > 0.25); // (a rain film: its sharp ray below)
#ifdef RE_EXT
    if ((g_ext & SSS_BITS) == SSS_D) den_s = false; // (the skin's checkerboard: each half traces only its own ray)
    if ((g_ext & SSS_BITS) == SSS_S) den_d = false;
#endif
#define DEN_NO_REFL !den_s && // (its reflection the denoiser's)
#else
#define DEN_NO_REFL
#endif
#ifdef RE_RT
    // One reflection ray (High / Epic). A smooth surface (rough < gi_params.z) shows what it hits, sharp. A rougher one
    // (until its reflection is all diffuse light) traces it too, but only to know what stands in front of the sky: a hit
    // within 30 m replaces the sky part of its reflection (an object standing on a floor no longer reflects as if it
    // were transparent: the sky through it). The hit is lit from its surface cache (direct + indirect: no light loop,
    // no shadow rays); a hit without a chart, by its lights and its probes. A miss keeps the sky.
    // (the rough ones within 40 m: past that a reflection's detail is a few pixels, and a field seen from above would
    // trace a ray for every pixel of the ground)
    // (and only where the sky's share of the reflection is worth it: specular weight x the sky's share of the reflection
    // x how much sky the point sees, over 5%. An 8% without the sky's visibility left the RPG demo's rough floor at a
    // grazing view (5-7%) reflecting the sky through the houses and characters standing on it; seen from above (~4%) or
    // indoors it traces nothing. A share of the pixel's whole color (sky part over 4% of it) chose the same rays, but
    // kept the pixel's color, diffuse and light alive up to here: -10% fps in the main pass on High (registers))
    float sky_w = (dot(f0, vec3(0.2126, 0.7152, 0.0722)) * ab.x + ab.y) * (1.0 - rough_mix) * sky_vis;
    bool want_ray = rough < f.gi_params.z ||
                    (sky_w > 0.05 && rough < 0.7 && dot(i_world - f.camera_pos.xyz, i_world - f.camera_pos.xyz) < 1.6e7) ||
                    (g_rcs > 0.25 && dot(i_world - f.camera_pos.xyz, i_world - f.camera_pos.xyz) < 1.6e7); // (a rain film's mirror)
#ifdef RE_EXT
    if ((g_ext & SSS_BITS) == SSS_D) want_ray = false; // (the skin's diffuse half reflects nothing)
#endif
    if ((f.gi_flags & GI_REFLECTIONS) != 0u && want_ray && DEN_NO_REFL !tiny && (mode == VIEW_LIT || mode == VIEW_SHADER_COST) && !gl_HelperInvocation) {
        traced_refl = true;
        bool sharp = rough < f.gi_params.z, sharp_ray = sharp || rsteady;
        Hit h = trace_closest(f, i_world + n * (0.5 + i_ray_bias), refl, sharp_ray ? 1e6 : 3000.0);
        if (h.hit) {
            vec3 hd = h.base * (1.0 - h.metal);
            vec4 hst = CacheStBuf(f.cache_st).st[h.inst];
            vec3 hl;
            if ((f.gi_flags & GI_SURFACE_CACHE) != 0u && hst.x > 0.0) {
                vec2 uv = h.uv2 * hst.xy + hst.zw;
                if (sharp_ray)
                    hl = (cache_read(u_cache_direct, uv, f.cache_params.x) + cache_read(u_cache_indirect, uv, f.cache_params.x) * f.gi_params.x) * GI_LOAD;
                else { // a rough reflection blurs it anyway: one bilinear read of each (premultiplied by validity)
                    vec4 cd = textureLod(u_cache_direct, uv, 0.0), ci = textureLod(u_cache_indirect, uv, 0.0);
                    hl = (cd.rgb / max(cd.a, 1e-3) + ci.rgb / max(ci.a, 1e-3) * f.gi_params.x) * GI_LOAD;
                }
            } else {
                vec3 hirr = (f.gi_flags & GI_PROBES) != 0u ? probe_irradiance(f, u_probe_irr, u_probe_dist, h.pos, h.normal, -refl) * f.gi_params.x
                                                           : sh_irradiance(f, h.normal) / PI;
                hl = (sharp_ray ? direct_irradiance(f, h.pos, h.normal) / PI : vec3(0.0)) + hirr;
            }
            vec3 traced = hd * hl + h.emissive;
            if (rcoat) cenv = ce_put(f, traced * ao); // (the film's mirror: what the ray hit, sharp)
            // sharp: the hit itself; rough: the hit where the sky was (the diffuse part stays the surface's own light)
            // (rough: a single ray along the mirror direction is one sample of a wide lobe; taken whole it pasted a sharp
            // image of the trunks and posts into the far grass, as if wet: it counts less as the surface gets rougher)
            env = sharp ? mix(traced, mix(traced, irr, rough_mix), smoothstep(f.gi_params.z * 0.5, f.gi_params.z, rough))
                        : mix(mix(traced, irr, rough_mix), irr, smoothstep(0.35, 0.7, rough));
            env *= ao;
        }
        // (2026-11-20) a mirror's ray through the volumetric fog: the sun's shafts it crosses, in the puddle (to its hit
        // or 30 m: the sky through a gap in the roof too)
        if (sharp && rough < 0.25) { // (the mirrors only: puddles, wet films, glass; marched at the end, g_fr)
            vec3 fw = (f0 * ab.x + ab.y) * ao;
            g_fr = uvec2(packSnorm2x16(oct_encode_f(refl)), packHalf2x16(vec2(h.hit ? min(h.t, 3000.0) : 3000.0, dot(fw, vec3(0.3333)))));
        }
    }
#endif
#ifdef RE_DEN
    vec3 spw = f0 * ab.x + ab.y;
#define DEN_SPW spw
    if (den_d || den_s) {
        // (den_trace.comp traces the two rays after this pass, with its registers its own: here what it needs. x: the
        // diffuse light here, pre-exposed (a diffuse miss's, and the sky's share a specular miss leaves); y: the sky this
        // point sees (half: its intensity times the visibility; 8 bits: the visibility), the rays' offset (6 bits: the
        // square root of the cm, in quarters: 0-248 cm), which rays; w's alpha the diffuse hit's distance, there)
        float bq = clamp(sqrt(max(i_ray_bias, 0.0)) * 4.0 + 0.5, 0.0, 63.0);
        g_den = uvec4(pack_r11g11b10(min(irr * pre_exposure(f), vec3(60000.0))),
                      (packHalf2x16(vec2(sky > 0.0 ? sky * sky_vis : 0.0, 0.0)) & 0xFFFFu) | (uint(clamp(sky_vis, 0.0, 1.0) * 255.0 + 0.5) << 16u) |
                          (uint(bq) << 24u) | (den_d ? DEN_RAY_D : 0u) | (den_s ? DEN_RAY_S : 0u),
                      packUnorm4x8(vec4(den_d ? diff : vec3(0.0), 1.0)), packUnorm4x8(vec4(den_s ? spw : vec3(0.0), 0.0)));
    }
    if (den_d) bounce = vec3(0.0), irr = vec3(0.0); // (out of the color: the denoiser adds them back; the ray saw the occluders)
    if (den_s) env = vec3(0.0);
#else
#define DEN_SPW (f0 * ab.x + ab.y)
#endif
#ifdef RE_EXT
    // the coat (M1b): its reflection, of the same light the base's sees (env), over the base's indirect light
    vec3 ind = diff * (irr * ao + bounce) + env * DEN_SPW;
    if ((g_ext & SSS_BITS) == SSS_D) ind = diff * (irr * ao + bounce); // (the skin's diffuse half: no reflection, no coat)
    else [[dont_flatten]] if (g_ext != 0u) {
        uint slot = (g_ext & 4095u) - 1u;
        vec4 e4 = GpuMatExtBuf(f.materials).x[slot].e4;
        if (e4.z > 1.0) ind += env * (f0 * ab.x + ab.y) * (thin_film(nv, e4.z, e4.w) - 1.0); // (the film's colors in its reflection: M4)
        vec4 e0 = GpuMatExtBuf(f.materials).x[slot].e0;
        if (e0.x > 0.0) {
            vec3 cn = normalize(i_normal);
            if (dot(cn, v) < 0.0) cn = -cn;
            float cnv = max(dot(cn, v), 1e-4), fc = e0.z + (1.0 - e0.z) * pow(1.0 - cnv, 5.0);
            // (a smooth base's traced reflection is the coat's too; else the sky sharp along the coat's own normal)
            vec3 menv = traced_refl && rough < 0.3 ? env : sky_radiance(f, reflect(-v, cn), false) * sky * sky_vis * ao;
            menv = mix(menv, irr * ao, clamp(sqrt(e0.y) * 1.3, 0.0, 1.0));
            ind = ind * mix(vec3(1.0), GpuMatExtBuf(f.materials).x[slot].e1.rgb, e0.x) * (1.0 - e0.x * fc) + menv * (e0.x * fc);
        }
    }
    [[dont_flatten]] if (rcoat) { // (2026-11-15) the rain's film: its reflection over what is under it
        vec2 rwa = unpackHalf2x16(g_rcp.y);
        float fcv = 0.02 + 0.98 * pow(1.0 - max(dot(rc_n(), v), 1e-4), 5.0);
        fcv *= 1.0 - 0.6 * smoothstep(0.01, 0.15, rwa.y); // (a rough film loses the grazing peak)
        ind = ind * (1.0 - rwa.x * fcv) + mix(ce_get(f, cenv), irr * ao, smoothstep(0.01, 0.15, rwa.y)) * (rwa.x * fcv);
    }
#ifdef RE_TOON_MAT
    [[dont_flatten]] if (g_toon != 0u) { // (Q11) the toon's ambient share, then its hatching where the lights' tone is low
        GpuToonBuf tb = GpuToonBuf(f.materials);
        color += ind * tb.t[TOON_SLOT - 1u].t6.y;
        float rh = f.tsr_size.y > 0.0 ? f.tsr_size.y : 1.0 / max(f.viewport.w, 1e-6);
        color *= toon_hatch(tb, TOON_SLOT - 1u, gl_FragCoord.xy - f.tsr.xy, rh / 1440.0, g_toon >= (1u << 20u) ? float((g_toon >> 12u) & 255u) / 255.0 : 0.0);
    } else
#endif
    color += ind;
#else
    vec3 ind = diff * (irr * ao + bounce) + env * DEN_SPW;
    [[dont_flatten]] if (rcoat) { // (2026-11-15) the rain's film: its reflection over what is under it
        vec2 rwa = unpackHalf2x16(g_rcp.y);
        float fcv = 0.02 + 0.98 * pow(1.0 - max(dot(rc_n(), v), 1e-4), 5.0);
        fcv *= 1.0 - 0.6 * smoothstep(0.01, 0.15, rwa.y); // (a rough film loses the grazing peak)
        ind = ind * (1.0 - rwa.x * fcv) + mix(ce_get(f, cenv), irr * ao, smoothstep(0.01, 0.15, rwa.y)) * (rwa.x * fcv);
    }
    color += ind;
#endif
    // (the indirect diffuse light's luminance: gtao.comp darkens that share of the pixel; 0 without it)
    float gtao_l = (f.gi_flags & GI_GTAO) != 0u ? dot(diff * (irr * ao + bounce), vec3(0.2126, 0.7152, 0.0722)) : 0.0;
    if (mode == VIEW_SHADER_COST) {
        // a weighted count of this pixel's work: a light 1, a shadow ray 4 (area lights trace several), the GI read 1-4
        // (the probes gather 8), the traced reflection 12 (a ray and the hit's lighting), the contact GI's occluders 1
        float rays = float(budget0 - budget) * max(f.sky_params.y, 1.0);
        float cost = float(nlights) + 4.0 * rays + (gi_src == 3 ? 4.0 : 1.0) + (traced_refl ? 12.0 : 0.0) +
                     float(f.occluder_count) * 0.25;
        o_color = vec4(heat(cost / 48.0) * mix(0.75, 1.0, shade), 1.0);
        return;
    }
    // pre-exposed (the composite divides it back out); half floats end at 65504
    vec3 lit = color + emissive;
    // (2026-11-16) never a NaN or an Inf out of here: one pixel of it spread by the TSR, the bloom and the exposure made the
    // whole screen flicker (a two-sided part's normals in the rain: water.glsl w_nmix)
    if (any(isnan(lit)) || any(isinf(lit))) lit = vec3(0.0);
    float through = 1.0; // (the air's and the fog's transmittance: the split light gets it too)
#ifdef RE_EXT
    float inscatter = (g_ext & SSS_BITS) == SSS_D ? 0.0 : 1.0; // (the skin's diffuse half: the fog's light is the other's)
#else
    const float inscatter = 1.0;
#endif
#ifdef RE_RT
    if (g_fr.y != 0u) { // (2026-11-20) the sun's shafts a mirror sees (the reflection's own light not dimmed by them: small)
        uint od = g_fr.x;
        vec2 lw = unpackHalf2x16(g_fr.y);
        vec4 fs = fog_segment(f, i_world, oct_decode(od), lw.x);
        lit += fs.rgb * lw.y;
    }
#endif
    if (f.sun_illuminance.w > 0.0) { // the air between the camera and the surface (aerial perspective)
        vec4 ap = aerial_perspective(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, length(g_pw - f.camera_pos.xyz));
        lit = lit * ap.a + ap.rgb * inscatter;
        through = ap.a;
    }
    // the height fog between the camera and the surface (weather.glsl: its froxels, then the analytic fog)
    vec4 fg = fog_apply_occl(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, -v, length(g_pw - f.camera_pos.xyz), fog_occl);
    lit = lit * fg.a + fg.rgb * inscatter;
#ifdef RE_TRANSLUCENT
    { // the sums (the weight: McGuire and Bavoil's eq. 10 in meters, capped so the half floats hold a few dozen layers)
        float a = clamp(g_opacity, 0.0, 1.0), dm = length(i_world - f.camera_pos.xyz) * 0.01;
        float w = a * clamp(0.03 / (1e-5 + pow(dm / 200.0, 4.0)), 1e-2, 30.0);
        o_color = vec4(min(lit * pre_exposure(f), vec3(2000.0)) * w, w);
        o_reveal = vec4(a, 1.0 - g_tint); // (Q4: the colored transmittance's product in gba, shadow_split.comp pass 3)
        o_refr = vec4(0.0);
        [[dont_flatten]] if ((g_mflags & MATF_REFRACT) != 0u) {
            // the view ray bent by the surface (Snell, its IOR), carried through its thickness: where that lands on the
            // screen is what is seen through this pixel (a flat pane barely moves it, a curved bottle or a lens bends it)
            GpuMatExt gx = GpuMatExtBuf(f.materials).x[floatBitsToUint(inst.emissive_rough.x)];
            float ior = gx.e5.w > 1.0 ? gx.e5.w : 1.5;
            vec3 rd = refract(-v, n, 1.0 / ior);
            if (dot(rd, rd) < 1e-6) rd = -v; // (total internal reflection: straight on)
            vec3 dw = (rd + v) * (gx.e7.x * gx.e7.z);
            vec4 c0 = f.view_proj * vec4(i_world, 1.0), c1 = f.view_proj * vec4(i_world + dw, 1.0);
            vec2 off = (c1.xy / max(c1.w, 1e-3) - c0.xy / max(c0.w, 1e-3)) * 0.5;
            o_refr = vec4(clamp(off, vec2(-0.25), vec2(0.25)), rough, gx.e7.y) * w;
        }
        return;
    }
#endif
    // (with GTAO: the indirect diffuse share, through the air and the fog like the color, in the alpha as 1 + share)
    float gshare = gtao_l > 0.0 ? clamp(gtao_l * through * fg.a / max(dot(lit, vec3(0.2126, 0.7152, 0.0722)), 1e-6), 0.0, 1.0) : 0.0;
#ifndef RE_RT
    // (with the contact shadows: the sun's share too, in 15ths, as 2 x its level: 1 + gshare + 2 x level, gtao.comp)
    float sshare = (f.gi_flags & GI_CONTACT_SHADOWS) != 0u && sun_l > 0.0
                       ? floor(clamp(sun_l * through * fg.a / max(dot(lit, vec3(0.2126, 0.7152, 0.0722)), 1e-6), 0.0, 1.0) * 15.0 + 0.5) : 0.0;
    o_color = vec4(min(lit * pre_exposure(f), vec3(60000.0)), 1.0 + gshare + 2.0 * sshare);
#else
    o_color = vec4(min(lit * pre_exposure(f), vec3(60000.0)), 1.0 + gshare);
#endif
#ifdef RE_EXT
    if ((g_ext & SSS_BITS) != 0u) o_color.a = -float(g_ext & 4095u); // (the skin: -(its slot + 1) for sss.comp; no GTAO share)
#endif
    if ((f.gi_flags & GI_AA_WEIGHTED) != 0u) o_color = aa_encode(o_color.rgb);
    // (written here only: an output set at the start stays live through the shader, and 4 registers spilled the ray
    // traced one; the returns above are other views', where render.c leaves the split off and drops these writes)
#if defined(RE_RT) && !defined(RE_TRANSLUCENT)
    o_split = vec4(0.0);
    if (g_split != uvec2(0u)) {
        vec2 rg = unpackHalf2x16(g_split.x), bv = unpackHalf2x16(g_split.y);
        vec3 c = vec3(rg, bv.x) * (through * fg.a);
        o_split = vec4(c, dot(c, vec3(0.2126, 0.7152, 0.0722)) * clamp(bv.y, 0.0, 1.0));
    }
#ifdef RE_DEN
    { // (every pixel of ours: the denoiser's guide even where nothing is noisy)
        float tr = through * fg.a;
        vec4 al = unpackUnorm4x8(g_den.z);
        o_den = uvec4(g_den.xy, den_key(n, rough, inst.ids.x), packUnorm4x8(vec4(al.rgb * tr, al.a)));
        o_guide = uvec4(packUnorm4x8(vec4(unpackUnorm4x8(g_den.w).rgb * tr, 0.0)), inst.ids.x + 1u, floatBitsToUint(gl_FragCoord.z), g_resv);
    }
#endif
#endif
}
