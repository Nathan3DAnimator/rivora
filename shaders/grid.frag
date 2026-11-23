#version 460
// The editor's ground grid: the z = 0 plane hit per pixel, lines anti-aliased with screen derivatives (1 m and 10 m),
// the X and Y axes in red and green, faded with distance. Writes the plane's depth so meshes hide it.
// An orthographic view (Top / Front / Left...): the plane facing the view through the origin (Unreal's), its two axes
// colored (X red, Y green, Z blue), no fade, the 10 m lines also when zoomed far out.
#include "common.glsl"

layout(location = 0) in vec2 i_ndc;
layout(location = 0) out vec4 o_color;

float lines(vec2 p, float spacing) {
    vec2 g = p / spacing;
    vec2 w = fwidth(g);
    vec2 a = abs(fract(g - 0.5) - 0.5) / max(w, 1e-5);
    return 1.0 - min(min(a.x, a.y), 1.0);
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    vec3 o = view_origin(f, i_ndc);
    vec3 d = view_ray(f, i_ndc);
    bool ortho = view_ortho(f);
    // the plane: z = 0, or (orthographic) the world plane most facing the view
    vec3 ad = abs(f.camera_fwd.xyz);
    int ax = !ortho ? 2 : (ad.x >= ad.y && ad.x >= ad.z) ? 0 : (ad.y >= ad.z ? 1 : 2);
    // (the axis chosen by swizzles: a vector indexed by a variable is refused by Adreno's compiler)
    float dax = ax == 0 ? d.x : ax == 1 ? d.y : d.z, oax = ax == 0 ? o.x : ax == 1 ? o.y : o.z;
    if (abs(dax) < 1e-6) discard;
    float t = -oax / dax;
    if (t <= 0.0) discard;
    vec3 p = o + d * t;
    vec4 clip = f.view_proj * vec4(p, 1.0);
    // a hair toward the camera: visible on a floor at the plane (orthographic: the depth is linear, 2 cm of it)
    gl_FragDepth = ortho ? clamp(clip.z + 2.0e-6, 0.0, 1.0) : clip.z / clip.w * 1.0002;
    int ua = ax == 0 ? 1 : 0, ub = ax == 2 ? 1 : 2; // the plane's two axes
    vec2 q = ax == 0 ? p.yz : ax == 1 ? p.xz : p.xy; // (ua, ub: the plane's axes, as swizzles)
    float fade = 1.0;
    if (!ortho) {
        float dist = length(p.xy - o.xy);
        fade = 1.0 - smoothstep(abs(o.z) * 8.0 + 1500.0, abs(o.z) * 20.0 + 9000.0, dist);
    }
    // (orthographic and zoomed out: 1 m lines too dense fade out; the 10 m and 100 m ones take over)
    float cm_px = ortho ? f.camera_up.w * 2.0 * f.viewport.w : 0.0; // (cm per pixel: viewport.w is 1 / its height)
    float minor = lines(q, 100.0) * 0.18 * (ortho ? 1.0 - smoothstep(8.0, 25.0, cm_px) : 1.0);
    float major = lines(q, 1000.0) * 0.35 * (ortho ? 1.0 - smoothstep(80.0, 250.0, cm_px) * 0.6 : 1.0);
    if (ortho) major = max(major, lines(q, 10000.0) * 0.4);
    vec2 axw = abs(q) / max(fwidth(q), vec2(1e-5));
    float a_line = 1.0 - min(axw.y, 1.0), b_line = 1.0 - min(axw.x, 1.0); // the first axis runs where the second is 0
    const vec3 axis_col[3] = vec3[3](vec3(0.9, 0.15, 0.15), vec3(0.2, 0.75, 0.2), vec3(0.25, 0.4, 0.95));
    vec3 c = vec3(1.0);
    float alpha = max(minor, major);
    if (a_line > 0.0) { c = mix(c, axis_col[ua], a_line); alpha = max(alpha, a_line * 0.8); }
    if (b_line > 0.0) { c = mix(c, axis_col[ub], b_line); alpha = max(alpha, b_line * 0.8); }
    alpha *= fade;
    if (alpha < 0.003) discard;
    // premultiplied, as is: over the weighted resolve's (c w, w) the blend leaves a mix the decode reads as the lines
    // weighted 1 (common.glsl aa_encode)
    o_color = vec4(c * alpha * 0.6, alpha);
}
