// The sun's virtual shadow maps read (vsm.c): the finest level around the point, PCSS (a blocker search, then a filter as
// wide as the penumbra the sun's disk casts from that blocker: sharp where the caster touches, soft far from it), on
// fixed Vogel disks of bilinear compares (smooth, no noise), blended into the next level over each level's last 10%.
#ifndef VSM_GLSL
#define VSM_GLSL
#include "farview.glsl" // (Q21) past the last level: the terrain's far shadow

layout(set = 0, binding = 0) uniform sampler2DArrayShadow u_vsm;   // compare (less or equal: lit)
layout(set = 0, binding = 51) uniform sampler2DArray u_vsm_depth;  // the depths (the blocker search)

vec2 vsm_vogel(uint i, uint n) {
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * 2.39996323;
    return r * vec2(cos(a), sin(a));
}

// The hybrid (mesh.frag, ray traced shadows on): with g_vsm_rt set, a point whose blockers are near (a penumbra under
// VSM_RAY_PEN texels: the sun's disk past a near caster draws a thinner edge than the map can) is left to an exact ray
// (g_vsm_ray). With a low sun a texel stretches over 2-3 cm of floor: hair cards became blurred blocks of texels that
// toggled as the hair swayed. The map keeps what it is sure of: no blocker around (lit), a far caster's wide penumbra
// Each level's ray counts by that level's weight
// (g_vsm_ray_w: the caller adds ray * g_vsm_ray_w to what vsm_shadow returns), so a shadow never pops in as the camera
// walks up to a character (2026-11-08: the Delight's chest went from lit to the ray's shadow in one frame).
#define VSM_RAY_PEN 2.5
#ifndef VSM_BAND
#define VSM_BAND 0.10 // the share of each level that fades into the next (wider ones measured: no better, more flicker)
#endif
bool g_vsm_rt = false, g_vsm_ray = false;
float g_vsm_ray_w = 0.0; // (the ray's share of the result)

// One level: 1 lit, 0 shadowed. p: the point lifted along its normal by about a texel of this level
float vsm_level(FrameBuf f, uint lv, vec3 p, uint taps) {
    vec4 L = f.vsm_level[lv]; // half side, texel, depth range (cm), drawn
    vec3 c = (f.vsm_mat[lv] * vec4(p, 1.0)).xyz;
    vec2 uv = c.xy / (2.0 * L.x) + 0.5;
    if (c.z >= 1.0) return 1.0;
    float texel_uv = 1.0 / float(textureSize(u_vsm_depth, 0).x);
    float z = c.z - 0.5 * L.y / L.z; // (half a texel of depth: a slope's own texels don't shadow it)
    if (taps <= 4u) { // Low: a 3x3 PCF of bilinear compares
        float s = 0.0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) s += texture(u_vsm, vec4(uv + vec2(x, y) * texel_uv, float(lv), z));
        return s / 9.0;
    }
    // blocker search: as wide as the penumbra the farthest caster above could cast (capped), in texels: four gathers
    // (16 depths) on a rotated square; none blocked: lit, every one: inside the shadow (the filter only runs in a penumbra)
    float tan_r = f.vsm_params.z;
    float search = clamp(c.z * L.z * tan_r / L.y, 1.5, 20.0);
    vec2 so = vec2(0.62, 0.26) * search * texel_uv;
    vec4 zz = vec4(z), d0 = textureGather(u_vsm_depth, vec3(uv + so, float(lv))),
         d1 = textureGather(u_vsm_depth, vec3(uv + vec2(-so.y, so.x), float(lv))),
         d2 = textureGather(u_vsm_depth, vec3(uv - so, float(lv))),
         d3 = textureGather(u_vsm_depth, vec3(uv + vec2(so.y, -so.x), float(lv)));
    vec4 b0 = vec4(lessThan(d0, zz)), b1 = vec4(lessThan(d1, zz)), b2 = vec4(lessThan(d2, zz)), b3 = vec4(lessThan(d3, zz));
    float bn = dot(b0 + b1 + b2 + b3, vec4(1.0));
    if (bn == 0.0) return 1.0;
    float bavg = (dot(d0, b0) + dot(d1, b1) + dot(d2, b2) + dot(d3, b3)) / bn;
    float pen_raw = (z - bavg) * L.z * tan_r / L.y;
    if (g_vsm_rt && pen_raw <= VSM_RAY_PEN) return -1.0; // (the ray decides: the filter's taps saved. A ramp ran the filter
                                                       // and the ray both: +0.18 ms)
    // the penumbra's radius (texels): the sun's disk seen past the blocker, at least a texel and a half (a smooth edge)
    float pen = clamp(pen_raw, 1.5, search);
    if (bn == 16.0 && pen < search * 0.5) return 0.0; // (deep inside: its whole disk is shadow)
    // the filter: taps by the penumbra's area (a sharp contact edge needs few), up to the preset's
    uint n = clamp(uint(pen * pen * 1.5), 6u, taps);
    float s = 0.0;
    for (uint i = 0u; i < n; i++) s += texture(u_vsm, vec4(uv + vsm_vogel(i, n) * pen * texel_uv, float(lv), z));
    return s / float(n);
}

// The sun's shadow at a point (n: its normal facing the light side, for the lift)
float vsm_shadow(FrameBuf f, vec3 world, vec3 n) {
    uint levels = uint(f.vsm_params.x), taps = uint(f.vsm_params.y);
    // the finest level holding the point with room for the filter (its widest search)
    uint lv = levels;
    float t = 0.0, fade = 0.0; // (fade: the last level's last 10% fades out to lit, no edge where the shadows end)
    for (uint l = 0u; l < levels; l++) {
        vec4 L = f.vsm_level[l];
        if (L.w == 0.0) continue;
        vec2 xy = (f.vsm_mat[l] * vec4(world, 1.0)).xy;
        float room = L.x - 24.0 * L.y, e = max(abs(xy.x), abs(xy.y));
        if (e >= room) continue;
        lv = l;
        // its last VSM_BAND fades into the next level
        t = clamp((e - (1.0 - VSM_BAND) * room) / (VSM_BAND * room), 0.0, 1.0);
        if (lv + 1u >= levels || f.vsm_level[lv + 1u].w == 0.0) fade = t, t = 0.0;
        break;
    }
    // (one call site in a loop of one or two: the filter's code once in the shader, not twice). -1: that level leaves
    // the point to the ray, for its weight. Past the last level: lit, the far shadow below (Q21)
    float s = 0.0, rw = 0.0;
    uint n_eval = lv >= levels ? 0u : t > 0.0 ? 2u : 1u;
    if (lv >= levels) s = 1.0, fade = 1.0;
    for (uint k = 0u; k < n_eval; k++) {
        uint l = lv + k;
        float w = k == 0u ? 1.0 - t : t;
        float v = vsm_level(f, l, world + n * (f.vsm_level[l].y * 1.5), taps);
        if (v < 0.0) rw += w;
        else s += v * w;
    }
    g_vsm_ray_w = rw * (1.0 - fade);
    g_vsm_ray = g_vsm_ray_w > 0.0;
    // (Q21) the terrain past the maps' reach (the far shadow's words past the split) darkens inside them too; at the last
    // level's end, into the whole far shadow (not to lit)
    // (one call site: the far shadow's code once in the shader, its registers after the filter's are gone)
    float fs = 1.0;
    if ((f.far_u.x & FAR_SHADOW) != 0u) fs = far_terrain_shadow(f, world, fade <= 0.0);
    return fade > 0.0 ? mix(s, fs, fade) : min(s, fs);
}

#endif
