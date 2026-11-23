#version 460
// The sky behind everything: drawn where the depth is still the cleared 0 (reversed Z: infinitely far). The clouds over
// the atmosphere, the height fog in front of both (weather.glsl)
#include "common.glsl"
#include "atmosphere.glsl"
#include "weather.glsl"
#include "ar.glsl"
#include "mr.glsl"

layout(location = 0) in vec2 i_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    if ((f.mr_flags & (MR_ON | MR_BACKGROUND)) == (MR_ON | MR_BACKGROUND)) { // (Q25b) mixed reality: the real room here (black: composite.frag's alpha 0)
        o_color = (f.gi_flags & GI_AA_WEIGHTED) != 0u ? aa_encode(vec3(0.0)) : vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (f.ar[2].x > 0.5) { // AR: the camera's picture behind the scene, as the camera shows it (ar.glsl)
        o_color = ar_camera_color(f, i_ndc);
        return;
    }
    bool blender_bg = unlit_mode(f.view_mode) && f.view_mode != VIEW_UNLIT;
    if (view_ortho(f) && !blender_bg) {
        // an orthographic view (Unreal's Top / Front...): its rays all point one way, no sky to show. A neutral gray as
        // bright as an 18% gray card under the sun and the sky (a light meter's reference): auto exposure then exposes
        // the meshes as in the perspective view, whatever the background's share of the screen; dark at night (one sky
        // lookup a pixel, no clouds nor fog)
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        vec3 hz = normalize(vec3(-f.sun_dir.y, f.sun_dir.x, 0.0) * f.sun_dir.w + vec3(0.0, 0.0, 0.35));
        float e = dot(f.sun_illuminance.rgb, lw) * max(f.sun_dir.z, 0.0) * f.sun_dir.w + 3.14159 * dot(sky_radiance(f, hz, false), lw);
        o_color = vec4(vec3(0.18 / 3.14159 * e * mix(0.8, 1.0, i_ndc.y * -0.5 + 0.5)) * pre_exposure(f), 1.0);
        if ((f.gi_flags & GI_AA_WEIGHTED) != 0u) o_color = aa_encode(o_color.rgb);
        return;
    }
    vec3 d = view_ray(f, i_ndc);
    vec3 L = sky_radiance(f, d, true); // the sun's disk too


    vec4 cl = clouds_screen(f, i_ndc, d); // (the screen's: their detail; reflections keep the panorama)
    L = L * cl.a + cl.rgb;
    vec4 fg = fog_apply(f, i_ndc, d, 1.0e7); // 100 km: the fog's whole depth toward the horizon
    L = L * fg.a + fg.rgb;
    vec3 c = min(L * pre_exposure(f), vec3(60000.0)); // pre-exposed
    if (blender_bg) // Blender's viewport background: a gray gradient
        c = mix(vec3(0.20), vec3(0.32), i_ndc.y * -0.5 + 0.5) * mix(1.0, 0.85, length(i_ndc) * 0.3);
    o_color = (f.gi_flags & GI_AA_WEIGHTED) != 0u ? aa_encode(c) : vec4(c, 1.0);
}
