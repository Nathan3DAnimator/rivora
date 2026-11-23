#version 460
// A cloth's light (cloth.vert): both faces lit (the side facing the camera), a cloth's sheen at grazing angles (Charlie,
// Estevez and Kulla 2017), some light through thin cloth (backlit), the sun's shadow from its maps, the sky; the air and
// the fog over it, as the meshes'.
#extension GL_EXT_ray_query : enable
#include "common.glsl"
#include "atmosphere.glsl"
#include "lights.glsl"
#include "weather.glsl"
#include "vsm.glsl"
#include "cloth.glsl"

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
layout(location = 2) in vec2 i_uv;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    ClothHeader H = ClothHeaderBuf(pc.instances).h;
    ClothInfo c = ClothInfoBuf(H.info).c[pc.pass];
    vec3 v = normalize(f.camera_pos.xyz - i_world);
    vec3 n = normalize(i_normal);
    if (dot(n, v) < 0.0) n = -n;
    // a weave: the threads' faint grid (the cloth's scale: a thread every ~0.4 cm of its rest size)
    vec2 w = cl_mesh(c) ? i_uv * 250.0 : i_uv * vec2(float(c.nx), float(c.ny)) * vec2(c.dx, c.dy) * 100.0 / 0.4; // (a mesh: its UV's 0..1 ~1 m)
    float weave = 0.92 + 0.08 * (0.5 + 0.5 * sin(w.x * 6.2831853) * sin(w.y * 6.2831853));
    vec3 base = c.tint.rgb * weave;
    float rough = clamp(c.tint.w, 0.3, 1.0), sheen = c.look.x;
    vec3 amb = f.sky_params.x > 0.0 ? sh_irradiance(f, n) / PI : vec3(0.0);
    vec3 lit = base * amb;
    int sli = int(f.sun_radiance.w);
    if (sli >= 0) {
        GpuLight L = f.lights[sli];
        vec3 l = -L.direction;
        float nl = dot(n, l);
        float sh = 1.0;
        if ((f.gi_flags & GI_VSM) != 0u) sh = vsm_shadow(f, i_world, nl > 0.0 ? n : -n);
        float front = max(nl, 0.0), back = max(-nl, 0.0) * c.look.y; // (thin cloth: some light comes through)
        vec3 h = normalize(l + v);
        float nh = max(dot(n, h), 0.0);
        float a = rough * rough, inv = 1.0 / a, sin2 = max(1.0 - nh * nh, 1e-4);
        float D = (2.0 + inv) * pow(sin2, 0.5 * inv) / (2.0 * PI); // (Charlie)
        float Vs = 1.0 / (4.0 * (front + max(dot(n, v), 0.0) - front * max(dot(n, v), 0.0)) + 1e-3);
        lit += L.radiance * sh * (base / PI * (front + back) + vec3(sheen) * D * Vs * front);
    }
    float dist = length(f.camera_pos.xyz - i_world);
    if (f.sun_illuminance.w > 0.0) {
        vec4 ap = aerial_perspective(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, dist);
        lit = lit * ap.a + ap.rgb;
    }
    vec4 fg = fog_apply(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, -v, dist);
    lit = lit * fg.a + fg.rgb;
    o_color = vec4(min(lit * pre_exposure(f), vec3(60000.0)), 1.0);
    if ((f.gi_flags & GI_AA_WEIGHTED) != 0u) o_color = aa_encode(o_color.rgb);
}
