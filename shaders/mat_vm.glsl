// The material bytecode's interpreter (material.c compiles it; mat_ops.h): the bake (material_bake.comp: the static
// part, per texel of the material's textures) and the per-pixel part (mesh.frag: what a parameter or the view changes)
// run the same code. The nodes' math is the Tlender's (tl/kernel_svm_tex.h, copied: Blender's noise, Voronoi, waves,
// blend modes, math), so a material looks as it does there.
// The includer defines MAT_SET (the descriptor set of the material textures) and MAT_REGS (the registers).
#ifndef MAT_VM_GLSL
#define MAT_VM_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#define KERNEL_GLSL
#include "tl/kernel_compat.h"
#include "tl/kernel_svm_types.h"
#include "tl/kernel_svm_tex.h"
#include "mat_common.glsl"

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer MatProgBuf { uint w[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MatParamBuf { vec4 p[]; };

struct MatEnv {
    vec2 uv;        // the surface's
    vec2 duvdx, duvdy; // its derivatives (per pixel: the texture reads' filtering; zero in the bake)
    vec3 view;      // toward the eye, tangent space
    float texel;    // the bake's texel (the Bump's height offsets)
    uvec2 params;   // MatParamBuf
    uint param_off;
    uvec4 slot_tex;
    bool bake;
    uvec2 udim;     // (Q18) the parameters buffer (the UDIM images' tables: mat_sample_udim)
};

vec4 mr[MAT_REGS];
// the result (MOP_OUTPUT)
vec4 mo_base;  // rgb, metallic
vec4 mo_emis;  // rgb, roughness
vec4 mo_nrm;   // tangent-space normal, alpha

vec4 mat_sample(uint tex, vec2 uv, MatEnv e) {
    if (e.bake) return textureLod(u_mat_tex[nonuniformEXT(tex)], uv, 0.0);
    return textureGrad(u_mat_tex[nonuniformEXT(tex)], uv, e.duvdx, e.duvdy);
}


// (Q18) A UDIM image (material_udim.inc ud_source): its table's row in the texture word (bit 31). The tile from the uv
// (1001 at u 0..1, v 0..1; the engine's V down: 1011 at v -1..0), read at the uv inside it (the tiles clamp: no bleed from
// their far edge); a tile it doesn't have reads black. A BC5 normal map's B rebuilt from R and G
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MatUdimBuf { uint t[]; };
vec4 mat_sample_udim(uint word, vec2 uv, MatEnv e) {
    uint row = word & 0x7FFFFFFFu;
    MatUdimBuf b = MatUdimBuf(e.udim);
    vec2 tl = floor(uv);
    uint tx = uint(int(tl.x)), ty = uint(-int(tl.y));
    if (tx >= 10u || ty >= 10u) return vec4(0.0);
    uint tex = b.t[(row + 1u) * 4u + tx + ty * 10u];
    if (tex == 0u) return vec4(0.0);
    vec4 t = mat_sample(tex, uv - tl, e);
    if ((b.t[row * 4u] & 1u) != 0u) {
        vec2 n = t.xy * 2.0 - 1.0;
        t = vec4(t.xy, sqrt(max(1.0 - dot(n, n), 0.0)) * 0.5 + 0.5, 1.0);
    }
    return t;
}

float ramp_alpha_out; // NODE_COLOR_RAMP's second output

#ifdef MAT_BAKE
#include "tl/kernel_svm_sim.h"
// (Q19) Blackbody (Cycles' fit, the Tlender's render_tables.c): the color of a temperature in kelvin, luminance 1
const float BB_R[21] = float[21](1.61919106e+03, -2.05010916e-03, 5.02995757e+00, 2.48845471e+03, -1.11330907e-03, 3.22621544e+00,
                                 3.34143193e+03, -4.86551192e-04, 1.76486769e+00, 4.09461742e+03, -1.27446582e-04, 7.25731635e-01,
                                 4.67028036e+03, 2.91258199e-05, 1.26703442e-01, 4.59509185e+03, 2.87495649e-05, 1.50345020e-01,
                                 3.78717450e+03, 9.35907826e-06, 3.99075871e-01);
const float BB_G[21] = float[21](-4.88999748e+02, 6.04330754e-04, -7.55807526e-02, -7.55994277e+02, 3.16730098e-04, 4.78306139e-01,
                                 -1.02363977e+03, 1.20223470e-04, 9.36662319e-01, -1.26571316e+03, 4.87340896e-06, 1.27054498e+00,
                                 -1.42529332e+03, -4.01150431e-05, 1.43972784e+00, -1.17554822e+03, -2.16378048e-05, 1.30408023e+00,
                                 -5.00799571e+02, -4.59832026e-06, 1.09098763e+00);
const float BB_B[28] = float[28](5.96945309e-11, -4.85742887e-08, -9.70622247e-05, -4.07936148e-03, 2.40430366e-11, 5.55021075e-08,
                                 -1.98503712e-04, 2.89312858e-02, -1.40949732e-11, 1.89878968e-07, -3.56632824e-04, 9.10767778e-02,
                                 -3.61460868e-11, 2.84822009e-07, -4.93211319e-04, 1.56723440e-01, -1.97075738e-11, 1.75359352e-07,
                                 -2.50542825e-04, -2.22783266e-02, -1.61997957e-13, -1.64216008e-08, 3.86216271e-04, -7.38077418e-01,
                                 6.72650283e-13, -2.73078809e-08, 4.24098264e-04, -7.52335691e-01);
vec3 mat_blackbody(float t) {
    if (t >= 12000.0) return vec3(0.8262954810464208, 0.9945080501520986, 1.566307710274283);
    if (t < 800.0) return vec3(5.413294490189271, -0.20319390035873933, -0.0822535242887164);
    int i = t >= 6365.0 ? 6 : (t >= 3315.0 ? 5 : (t >= 1902.0 ? 4 : (t >= 1449.0 ? 3 : (t >= 1167.0 ? 2 : (t >= 965.0 ? 1 : 0)))));
    float ti = 1.0 / t;
    vec3 c = vec3(BB_R[i * 3] * ti + BB_R[i * 3 + 1] * t + BB_R[i * 3 + 2], BB_G[i * 3] * ti + BB_G[i * 3 + 1] * t + BB_G[i * 3 + 2],
                  ((BB_B[i * 4] * t + BB_B[i * 4 + 1]) * t + BB_B[i * 4 + 2]) * t + BB_B[i * 4 + 3]);
    return max(c, vec3(0.0));
}
#endif

// p: properties, i: the first input word, o: the first output word
void mat_op(MatProgBuf pb, uint op, uint nin, uint nout, uvec4 p, uint i, uint o, MatEnv e) {
#define IN(k) mr[pb.w[i + uint(k)]]
#define INF(k) mr[pb.w[i + uint(k)]].x
#define INV(k) mr[pb.w[i + uint(k)]].xyz
#define OUT(k) mr[pb.w[o + uint(k)]]
    switch (op) {
    case MOP_CONST: OUT(0) = uintBitsToFloat(p); break;
    case MOP_UV: {
        float off = uintBitsToFloat(p.y);
        OUT(0) = vec4(e.uv + (p.x == 1u ? vec2(off, 0.0) : p.x == 2u ? vec2(0.0, off) : vec2(0.0)), 0.0, 0.0);
        break;
    }
    case MOP_NORMAL: OUT(0) = vec4(0.0, 0.0, 1.0, 0.0); break;
    case MOP_VIEW: OUT(0) = vec4(e.view, 0.0); break;
    case MOP_PARAM: OUT(0) = MatParamBuf(e.params).p[e.param_off + p.x]; break;
    case MOP_LOAD_SLOT: OUT(0) = mat_sample(e.slot_tex[p.x], e.uv, e); break;
    case MOP_CVT_LUM: OUT(0) = vec4(dot(INV(0), vec3(0.2126, 0.7152, 0.0722))); break;
    case MOP_CVT_AVG: OUT(0) = vec4((IN(0).x + IN(0).y + IN(0).z) / 3.0); break;
    case MOP_CVT_SPLAT: OUT(0) = vec4(vec3(INF(0)), 0.0); break;
    case MOP_CLOSURE: {
        // base, metal, rough, alpha, normal, emission color, emission strength
        OUT(0) = vec4(INV(0), clamp(INF(1), 0.0, 1.0));
        OUT(1) = vec4(INV(5) * INF(6), clamp(INF(2), 0.0, 1.0));
        OUT(2) = vec4(normalize(INV(4)), clamp(INF(3), 0.0, 1.0));
        break;
    }
    case MOP_SHADER_MIX: {
        float t = clamp(INF(0), 0.0, 1.0);
        OUT(0) = mix(IN(1), IN(4), t);
        OUT(1) = mix(IN(2), IN(5), t);
        vec4 n = mix(IN(3), IN(6), t);
        OUT(2) = vec4(normalize(n.xyz), n.w);
        break;
    }
    case MOP_SHADER_ADD: { // the closures summed: the light of both (base colors add, an emission adds)
        OUT(0) = vec4(IN(0).rgb + IN(3).rgb, max(IN(0).w, IN(3).w));
        OUT(1) = vec4(IN(1).rgb + IN(4).rgb, min(IN(1).w, IN(4).w));
        OUT(2) = vec4(normalize(IN(2).xyz + IN(5).xyz), max(IN(2).w, IN(5).w));
        break;
    }
    case MOP_OUTPUT: mo_base = IN(0); mo_emis = IN(1); mo_nrm = IN(2); break;
    case MOP_SET_W: OUT(0) = vec4(INV(0), clamp(INF(1), 0.0, 1.0)); break;
#ifdef MAT_BAKE
    case MOP_STORE_SLOT: mat_store_slot(p.x, IN(0)); break; // material_bake.comp
#endif
    case MOP_BUMP_HEIGHTS: {
        // in: strength, distance, h, normal, h(+u), h(+v); the slope over one texel of uv (a uv unit ~ 1 m)
        float off = uintBitsToFloat(p.y), strength = clamp(INF(0), 0.0, 1.0), dist = INF(1) * (p.x != 0u ? -1.0 : 1.0);
        float dhdu = (INF(4) - INF(2)) / off, dhdv = (INF(5) - INF(2)) / off;
        vec3 nin = normalize(INV(3));
        vec3 n = normalize(nin - vec3(dhdu, dhdv, 0.0) * dist * 0.01);
        OUT(0) = vec4(normalize(mix(nin, n, strength)), 0.0);
        break;
    }
    // --- the nodes (as the Tlender's kernel_svm.h runs them) ---
    case NODE_FRESNEL: { // IOR, normal, view
        float eta = max(INF(0), 1e-5), c = abs(dot(INV(2), normalize(INV(1))));
        float g = eta * eta - 1.0 + c * c, f;
        if (g > 0.0) {
            g = sqrt(g);
            float A = (g - c) / (g + c), B = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
            f = 0.5 * A * A * (1.0 + B * B);
        } else f = 1.0;
        OUT(0) = vec4(f);
        break;
    }
    case NODE_LAYER_WEIGHT: { // blend, normal, view: Fresnel, Facing
        float blend = clamp(INF(0), 0.0, 0.99999), c = abs(dot(INV(2), normalize(INV(1))));
        float eta = 1.0 / (1.0 - blend), g = eta * eta - 1.0 + c * c, f = 1.0;
        if (g > 0.0) {
            g = sqrt(g);
            float A = (g - c) / (g + c), B = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
            f = 0.5 * A * A * (1.0 + B * B);
        }
        float facing = 1.0 - c;
        if (blend != 0.5) facing = pow(facing, blend < 0.5 ? 2.0 * blend : 0.5 / (1.0 - blend));
        OUT(0) = vec4(f);
        OUT(1) = vec4(facing);
        break;
    }
    case NODE_TEX_IMAGE: { // p0 interpolation, p1 extension, p2 color space, p3 the texture
        vec2 uv = INV(0).xy;
        vec4 t;
        if ((p.w & 0x80000000u) != 0u) t = mat_sample_udim(p.w, uv, e); // (Q18: the extension is the tiles')
        else {
            if (p.y == uint(IMAGE_EXT_EXTEND)) uv = clamp(uv, 0.0, 1.0);
            else if (p.y == uint(IMAGE_EXT_MIRROR)) uv = 1.0 - abs(mod(uv, 2.0) - 1.0);
            t = mat_sample(p.w, uv, e);
            if (p.y == uint(IMAGE_EXT_CLIP) && (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))) t = vec4(0.0);
        }
        if (p.z == uint(IMAGE_COLORSPACE_SRGB)) t.rgb = srgb_to_linear(t.rgb);
        OUT(0) = vec4(t.rgb, 0.0);
        OUT(1) = vec4(t.a);
        break;
    }
    case NODE_TEX_NOISE: {
        TexResult t = tex_noise(int(p.x) + 1, int(p.y), p.z != 0u, INV(0), INF(1), INF(2), INF(3), INF(4), INF(5), INF(6), INF(7), INF(8), true);
        OUT(0) = vec4(t.fac);
        OUT(1) = vec4(t.color, 0.0);
        break;
    }
    case NODE_TEX_VORONOI: {
        TexResult t = tex_voronoi(INV(0), INF(1), INF(2), int(p.x), int(p.y));
        OUT(0) = vec4(t.fac);
        OUT(1) = vec4(t.color, 0.0);
        OUT(2) = vec4(t.position, 0.0);
        break;
    }
    case NODE_TEX_CHECKER: {
        float f = tex_checker(INV(0) * INF(3));
        OUT(0) = vec4(f == 1.0 ? INV(1) : INV(2), 0.0);
        OUT(1) = vec4(f);
        break;
    }
    case NODE_TEX_GRADIENT: {
        float f = clamp(tex_gradient(INV(0), int(p.x)), 0.0, 1.0);
        OUT(0) = vec4(vec3(f), 0.0);
        OUT(1) = vec4(f);
        break;
    }
    case NODE_TEX_WAVE: {
        float f = tex_wave(INV(0) * INF(1), int(p.x), int(p.y), int(p.z), INF(2), INF(3), INF(4), INF(5), INF(6));
        OUT(0) = vec4(vec3(f), 0.0);
        OUT(1) = vec4(f);
        break;
    }
    case NODE_TEX_WHITE_NOISE: {
        OUT(0) = vec4(hash_float3_to_float(INV(0)));
        OUT(1) = vec4(hash_float3_to_float3(INV(0)), 0.0);
        break;
    }
    case NODE_MIX: { // p0 data type, p1 blend mode, p2 clamp factor, p3 clamp result
        float fac = INF(0);
        if (p.z != 0u) fac = clamp(fac, 0.0, 1.0);
        OUT(0) = vec4(mix(INF(1), INF(2), fac));
        OUT(1) = vec4(mix(INV(3), INV(4), fac), 0.0);
        vec3 c = svm_mix_color(int(p.y), fac, INV(5), INV(6));
        OUT(2) = vec4(p.w != 0u ? clamp(c, 0.0, 1.0) : c, 0.0);
        break;
    }
    case NODE_INVERT: OUT(0) = vec4(mix(INV(1), 1.0 - INV(1), INF(0)), 0.0); break;
    case NODE_HUE_SAT: {
        vec3 hsv = rgb_to_hsv(INV(4));
        hsv.x = fract(hsv.x + INF(0) + 0.5);
        hsv.y = clamp(hsv.y * INF(1), 0.0, 1.0);
        hsv.z *= INF(2);
        OUT(0) = vec4(max(mix(INV(4), hsv_to_rgb(hsv), INF(3)), 0.0), 0.0);
        break;
    }
    case NODE_BRIGHT_CONTRAST: {
        float a = 1.0 + INF(2), b = INF(1) - INF(2) * 0.5;
        OUT(0) = vec4(max(INV(0) * a + b, 0.0), 0.0);
        break;
    }
    case NODE_GAMMA: {
        vec3 c = INV(0);
        float g = INF(1);
        c = g == 0.0 ? vec3(1.0) : vec3(c.x > 0.0 ? pow(c.x, g) : c.x, c.y > 0.0 ? pow(c.y, g) : c.y, c.z > 0.0 ? pow(c.z, g) : c.z);
        OUT(0) = vec4(c, 0.0);
        break;
    }
    case NODE_MAPPING: OUT(0) = vec4(svm_mapping(int(p.x), INV(0), INV(1), INV(2), INV(3)), 0.0); break;
    case NODE_NORMAL_MAP: { // p0 space (tangent only here), p1 DirectX; in: strength, color
        vec3 c = INV(1) * 2.0 - 1.0;
        if (p.y != 0u) c.y = -c.y;
        vec3 n = normalize(c);
        OUT(0) = vec4(normalize(mix(vec3(0.0, 0.0, 1.0), n, max(INF(0), 0.0))), 0.0);
        break;
    }
    case NODE_COLOR_RAMP: { // p0 interpolation, p1 stops; in: fac, then per stop its color (rgba) and its position
        float f = INF(0);
        uint n = max(p.y, 1u);
        uint hi = n;
        for (uint k = 0u; k < n; k++)
            if (hi == n && f < IN(2u + k * 2u).x) hi = k;
        uint lo = hi > 0u ? hi - 1u : 0u;
        hi = min(hi, n - 1u);
        float plo = IN(2u + lo * 2u).x, phi = IN(2u + hi * 2u).x, t = 0.0;
        if (hi != lo && phi > plo) t = clamp((f - plo) / (phi - plo), 0.0, 1.0);
        if (p.x == uint(RAMP_CONSTANT)) t = 0.0;
        else if (p.x == uint(RAMP_EASE)) t = t * t * (3.0 - 2.0 * t);
        vec4 c = mix(IN(1u + lo * 2u), IN(1u + hi * 2u), t);
        OUT(0) = vec4(c.rgb, 0.0);
        OUT(1) = vec4(c.a);
        break;
    }
    case NODE_MATH: {
        float x = svm_math(int(p.x), INF(0), INF(1), INF(2));
        OUT(0) = vec4(p.y != 0u ? clamp(x, 0.0, 1.0) : x);
        break;
    }
    case NODE_VECTOR_MATH: {
        VMathResult m = svm_vector_math(int(p.x), INV(0), INV(1), INV(2), INF(3));
        OUT(0) = vec4(m.v, 0.0);
        OUT(1) = vec4(m.f);
        break;
    }
    case NODE_MAP_RANGE: OUT(0) = vec4(svm_map_range(int(p.x), p.y != 0u, INF(0), INF(1), INF(2), INF(3), INF(4), INF(5))); break;
    case NODE_CLAMP: {
        float lo = INF(1), hi = INF(2);
        if (p.x == uint(CLAMP_RANGE) && lo > hi) { float t = lo; lo = hi; hi = t; }
        OUT(0) = vec4(clamp(INF(0), lo, hi));
        break;
    }
    case NODE_SEPARATE_XYZ: OUT(0) = vec4(IN(0).x); OUT(1) = vec4(IN(0).y); OUT(2) = vec4(IN(0).z); break;
    case NODE_COMBINE_XYZ: OUT(0) = vec4(INF(0), INF(1), INF(2), 0.0); break;
    case NODE_SEPARATE_COLOR: {
        vec3 c = INV(0);
        if (p.x == uint(COLOR_MODE_HSV)) c = rgb_to_hsv(c);
        else if (p.x == uint(COLOR_MODE_HSL)) c = rgb_to_hsl(c);
        OUT(0) = vec4(c.x); OUT(1) = vec4(c.y); OUT(2) = vec4(c.z);
        break;
    }
    case NODE_COMBINE_COLOR: {
        vec3 c = vec3(INF(0), INF(1), INF(2));
        if (p.x == uint(COLOR_MODE_HSV)) c = hsv_to_rgb(c);
        else if (p.x == uint(COLOR_MODE_HSL)) c = hsl_to_rgb(c);
        OUT(0) = vec4(c, 0.0);
        break;
    }
    case NODE_RGB_TO_BW: OUT(0) = vec4(dot(INV(0), vec3(0.2126, 0.7152, 0.0722))); break;
#ifdef MAT_BAKE
    // (Q19, notas-dev/importar-tlender.md) the Tlender's nodes the Engine bakes only (static: never per pixel, so the
    // mesh shader's code is the same): Blender's Brick and Magic, the Tlender's weave, cells (cracks, stones, grains),
    // ocean and fibers (tl/kernel_svm_sim.h, copied), Vector Rotate and Blackbody
    case NODE_TEX_BRICK: { // p0 offset frequency, p1 squash frequency (index + 1); in: the Tlender's 12
        float2 b = tex_brick(INV(0) * INF(4), INF(5), INF(6), INF(7), INF(8), INF(9), INF(10), int(p.x), INF(11), int(p.y));
        vec3 c1 = INV(1);
        if (b.y != 1.0) c1 = mix(c1, INV(2), b.x);
        OUT(0) = vec4(mix(c1, INV(3), b.y), 0.0);
        OUT(1) = vec4(b.y);
        break;
    }
    case NODE_TEX_MAGIC: {
        vec3 c = tex_magic(INV(0) * INF(1), int(p.x), INF(2));
        OUT(0) = vec4(c, 0.0);
        OUT(1) = vec4((c.x + c.y + c.z) / 3.0);
        break;
    }
    case NODE_TEX_WEAVE: {
        WeaveResult w = tex_weave(INV(0), int(p.x), INF(1), INF(2), INV(3), INV(4), INF(5), INF(6));
        OUT(0) = vec4(w.color, 0.0);
        OUT(1) = vec4(w.height);
        OUT(2) = vec4(w.warp);
        break;
    }
    case NODE_TEX_CRACKS: { // p0 CELLS_* (Stones, Grains: their inputs in the cells' order, material_gpu.inc)
        CellsResult c = tex_cells(INV(0), int(p.x), INF(1), INF(2), INF(3), INF(4), INF(5), INF(6), 27 + int(p.y));
        OUT(0) = vec4(c.gap);
        OUT(1) = vec4(c.height);
        OUT(2) = vec4(c.random);
        OUT(3) = vec4(c.dist);
        OUT(4) = vec4(c.rvec, 0.0);
        break;
    }
    case NODE_TEX_OCEAN: {
        float2 oc = tex_ocean(INV(0), INF(1), INF(2), INF(3), INF(4), INF(5), INF(6), p.x != 0u); // (not "o": the outputs' word)
        OUT(0) = vec4(oc.x);
        OUT(1) = vec4(oc.y);
        break;
    }
    case NODE_TEX_FIBERS: {
        float2 f = tex_fibers(INV(0), int(p.x), INF(1), INF(2), INF(3), INF(4), INF(5), 3 + int(p.y));
        OUT(0) = vec4(f.x);
        OUT(1) = vec4(f.y);
        break;
    }
    case NODE_VECTOR_ROTATE: // p0 type, p1 invert; in: vector, center, axis, angle, rotation
        OUT(0) = vec4(svm_vector_rotate(int(p.x), p.y != 0u, INV(0), INV(1), INV(2), INF(3), INV(4)), 0.0);
        break;
    case NODE_BLACKBODY: OUT(0) = vec4(mat_blackbody(INF(0)), 0.0); break;
#endif
    default: break;
    }
#undef IN
#undef INF
#undef INV
#undef OUT
}

// Runs words [off, off + len) of the program
void mat_run(uvec2 prog, uint off, uint len, MatEnv e) {
    MatProgBuf pb = MatProgBuf(prog);
    mo_base = vec4(0.8, 0.8, 0.8, 0.0);
    mo_emis = vec4(0.0, 0.0, 0.0, 0.5);
    mo_nrm = vec4(0.0, 0.0, 1.0, 1.0);
    uint pc = off, end = off + len;
    while (pc < end) {
        uint w0 = pb.w[pc];
        uint op = w0 & 0xFFFFu, nin = (w0 >> 16) & 0xFFu, nout = w0 >> 24;
        uvec4 p = uvec4(pb.w[pc + 1u], pb.w[pc + 2u], pb.w[pc + 3u], pb.w[pc + 4u]);
        uint i = pc + 5u;
        mat_op(pb, op, nin, nout, p, i, i + nin, e);
        pc = i + nin + nout;
    }
}

#endif
