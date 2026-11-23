// (Q11) The Toon shading model (notas-dev/toon.md; toon.c toon_pack, material_gpu.inc toon_slot_put): a material's
// light in bands by its ramp, the dark side toward its shadow color, the cast shadows in a color of their own, a hard
// stylized highlight, a rim of light, hatching on the screen where the light is low. The main pass (mesh.frag, the
// RE_EXT variants) and the asset previews (preview.frag) light a toon material with it. Needs mat_common.glsl.
#ifndef TOON_GLSL
#define TOON_GLSL

// Per material slot, a table after the GpuMatExt one (13 vec4):
//   t0 shadow color rgb, bands (0-1: none)            t1 the received shadows' color rgb, softness (a share of a band)
//   t2 the highlight's color x strength, its size     t3 the rim's color x strength, its width
//   t4 the hatching's color, its strength (0: none)   t5 hatch spacing (px at 1440p), angle (rad), threshold, the highlight's hardness
//   t6 rim softness, ambient share, the ramp image (bindless index as a float; 0: the stops), the stops' count
//   t7 the stops' positions                           t8-t11 the stops' colors rgb (t8.w: the shadow threshold)
struct GpuToon { vec4 t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer GpuToonBuf { GpuMaterial m[MAT_EXT_BASE]; GpuMatExt x[MAT_EXT_BASE]; GpuToon t[]; };

// GpuLight.type's toon bits (lights.c, CLight.toon_mode): this light shades every material in bands (a stylized key
// light over a realistic scene) / never (a fill light that keeps a toon material soft)
#define LIGHT_TOON_FORCE 0x04u // (lote 4: bits 2-3, the kind is bits 0-1; Q13 has 4-7)
#define LIGHT_TOON_NEVER 0x08u

// The level t (0..1) cut into n bands, each edge soft over a share of a band (n < 2: t itself, the ramp shapes it)
float toon_band(float t, float n, float soft) {
    if (n < 1.5) return t;
    float x = t * n, i = floor(x);
    return clamp((i - 1.0 + smoothstep(0.0, max(soft, 1e-3), x - i)) / (n - 1.0), 0.0, 1.0);
}

// The ramp's tone at q: its image (left dark, right lit; sRGB) or its stops
vec3 toon_ramp(GpuToonBuf tb, uint slot, float q) {
    vec4 t6 = tb.t[slot].t6;
    uint img = uint(t6.z + 0.5);
    if (img != 0u) return srgb_to_linear(textureLod(u_mat_tex[nonuniformEXT(img)], vec2(clamp(q, 0.004, 0.996), 0.5), 0.0).rgb);
    vec4 p = tb.t[slot].t7;
    int n = int(t6.w + 0.5);
    vec3 c0 = tb.t[slot].t8.rgb;
    if (n <= 1 || q <= p.x) return c0;
    vec3 c1 = tb.t[slot].t9.rgb;
    if (q <= p.y || n == 2) return mix(c0, c1, clamp((q - p.x) / max(p.y - p.x, 1e-4), 0.0, 1.0));
    vec3 c2 = tb.t[slot].t10.rgb;
    if (q <= p.z || n == 3) return mix(c1, c2, clamp((q - p.y) / max(p.z - p.y, 1e-4), 0.0, 1.0));
    return mix(c2, tb.t[slot].t11.rgb, clamp((q - p.z) / max(p.w - p.z, 1e-4), 0.0, 1.0));
}

// One light on a toon surface: what it reflects of the light (times the light's radiance and reach outside), the
// shadow vis (0..1, the map's or the ray's) taken in as a band too. level: the light's tone here (the hatching's)
vec3 toon_shade(GpuToonBuf tb, uint slot, vec3 n, vec3 v, vec3 l, vec3 diff, float vis, out float level) {
    vec4 t0 = tb.t[slot].t0, t1 = tb.t[slot].t1;
    float nl = dot(n, l);
    float t = clamp(nl * 0.5 + 1.0 - tb.t[slot].t8.w, 0.0, 1.0); // (half Lambert, its terminator moved by the threshold)
    float q = toon_band(t, t0.w, t1.w);
    float hs = 0.5 * t1.w + 0.02;
    // (the cast shadow in one step too; faded out toward the terminator, where a shadow map's own acne would be cut into
    // hard stair steps: the dark side is the bands' there anyway)
    float vq = smoothstep(0.5 - hs, 0.5 + hs, mix(1.0, vis, smoothstep(0.0, 0.2, nl)));
    vec3 F = t0.rgb + (1.0 - t0.rgb) * toon_ramp(tb, slot, q);
    F = mix(F, t1.rgb, (1.0 - vq) * q); // (the lit side under a cast shadow: its own color)
    level = dot(F, vec3(0.2126, 0.7152, 0.0722));
    vec3 r = diff * F;
    vec4 t2 = tb.t[slot].t2;
    if (t2.r + t2.g + t2.b > 0.0) { // the stylized highlight: a spot cut where n.h passes 1 - size^2 / 2
        float nh = max(dot(n, normalize(v + l)), 0.0), c = 1.0 - 0.5 * t2.w * t2.w;
        float w = mix(0.3, 0.01, tb.t[slot].t5.w) * (1.0 - c) + 1e-4;
        r += t2.rgb * (smoothstep(c - w, c + w, nh) * vq * step(0.0, nl));
    }
    vec4 t3 = tb.t[slot].t3;
    if (t3.r + t3.g + t3.b > 0.0) { // the rim of light: the grazing edge, on the side this light reaches
        float rs = tb.t[slot].t6.x * 0.5 + 1e-3, e = 1.0 - max(dot(n, v), 0.0);
        r += t3.rgb * (smoothstep(1.0 - t3.w - rs, 1.0 - t3.w + rs, e) * vq * smoothstep(0.0, 0.25, t));
    }
    return r * (1.0 / PI);
}

// A material lit by a LIGHT_TOON_FORCE light without being a Toon one: a hard cel step at the terminator (lit or not;
// the shadow and the reach multiply it outside, the ambient gives the dark side its tone)
vec3 toon_forced(vec3 n, vec3 l, vec3 diff) { return diff * (smoothstep(-0.03, 0.03, dot(n, l)) * (1.0 / PI)); }

// The hatching (on the screen, steady: p the pixel without the TSR's jitter; px the spacing's scale, render px per
// 1440p px): lines across where the light's tone is under the threshold, crossed where it is darker. The color's factor
vec3 toon_hatch(GpuToonBuf tb, uint slot, vec2 p, float px, float level) {
    vec4 t4 = tb.t[slot].t4, t5 = tb.t[slot].t5;
    if (t4.w <= 0.0) return vec3(1.0);
    float d = clamp(1.0 - level / max(t5.z, 1e-3), 0.0, 1.0);
    if (d <= 0.0) return vec3(1.0);
    float sp = max(t5.x * px, 2.0);
    vec2 a = vec2(cos(t5.y), sin(t5.y));
    float u1 = dot(p, vec2(-a.y, a.x)) / sp, u2 = dot(p, a) / sp;
    float hw1 = mix(0.06, 0.4, d) * sp * 0.5, hw2 = mix(0.0, 0.35, clamp(d * 2.0 - 1.0, 0.0, 1.0)) * sp * 0.5;
    float l1 = 1.0 - smoothstep(hw1 - 0.5, hw1 + 0.5, abs(fract(u1) - 0.5) * sp);
    float l2 = hw2 > 0.0 ? 1.0 - smoothstep(hw2 - 0.5, hw2 + 0.5, abs(fract(u2) - 0.5) * sp) : 0.0;
    return mix(vec3(1.0), t4.rgb, max(l1, l2) * t4.w);
}
#endif
