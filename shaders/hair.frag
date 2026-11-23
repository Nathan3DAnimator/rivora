#version 460
// Hair in strands (A9.3b, groom.c): a fiber's light by the Marschner model as Karis made it real time (SIGGRAPH 2016:
// the R, TT and TRT lobes, each a longitudinal Gaussian shifted by the cuticle's tilt times an azimuthal term; d'Eon's
// widths per lobe) plus the light scattered many times through the hair (a soft wrap tinted by the fiber's color).
// Each light's share through the hair before it by the groom's density grid (hair.glsl: the deep shadow); the sun's
// shadow from its maps; the ambient from the GI probes (or the sky light) occluded by the hair above. Coverage: a fiber
// thinner than the pixel keeps its share of the pixels (a hash per strand, pixel and frame: TSR averages them). The air
// and the fog over it, as the meshes'. HAIR_OUT: the main pass's targets this pipeline has (render.c), the ones past
// the color written zero (nothing of the meshes behind left for the split's or the denoiser's filters)
#include "common.glsl"
#include "gi.glsl"
#include "atmosphere.glsl"
#include "lights.glsl"
#include "weather.glsl"
#include "vsm.glsl"
#include "hair.glsl"

#ifndef HAIR_OUT
#define HAIR_OUT 1
#endif
#ifndef HAIR_DEPTH
layout(early_fragment_tests) in; // (shaded on the prepass's depth, EQUAL: no discard here, each pixel once)
#endif
layout(set = 0, binding = 3) uniform sampler2D u_probe_irr;
layout(set = 0, binding = 4) uniform sampler2D u_probe_dist;

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_tan;
layout(location = 2) in float i_t;
layout(location = 3) flat in uint i_strand;
layout(location = 4) in float i_cover;
layout(location = 0) out vec4 o_color;
#if HAIR_OUT >= 2
layout(location = 1) out vec4 o_split;
#endif
#if HAIR_OUT >= 4
layout(location = 2) out uvec4 o_den;
layout(location = 3) out uvec4 o_guide;
#endif

float hair_g(float b, float x) { return exp(-0.5 * x * x / (b * b)) / (2.5066283 * b); }
float hair_f(float c) { const float f0 = 0.0465; return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); } // (IOR 1.55)

// One light's scattering off the fiber (T its direction): Marschner's three paths, then the many-times scattered light
vec3 hair_light(vec3 T, vec3 V, vec3 L, vec3 color, float rough, float through) {
    float sl = clamp(dot(T, L), -1.0, 1.0), sv = clamp(dot(T, V), -1.0, 1.0);
    float cd = cos(0.5 * abs(asin(sv) - asin(sl)));                  // cos of the half difference angle (theta_d)
    vec3 lp = L - sl * T, vp = V - sv * T;
    float cphi = dot(lp, vp) * inversesqrt(max(dot(lp, lp) * dot(vp, vp), 1e-8)); // the azimuth between them
    float chp = sqrt(clamp(0.5 + 0.5 * cphi, 0.0, 1.0));
    float b = max(rough * rough, 0.004), sum = sl + sv;
    const float shift = 0.035; // (the cuticle's scales tilt each lobe: R up, TT and TRT down)
    // R: reflected off the surface
    vec3 s = vec3(hair_g(b, sum + 2.0 * shift) * 0.25 * chp * hair_f(sqrt(clamp(0.5 + 0.5 * dot(V, L), 0.0, 1.0))));
    // TT: through the fiber (forward: a backlit halo), tinted by its path inside
    float np = 1.19 / cd + 0.36 * cd, a = 1.0 / np;
    float h = chp * (1.0 + a * (0.6 - 0.8 * cphi));
    float f = hair_f(cd * sqrt(clamp(1.0 - h * h, 0.0, 1.0)));
    vec3 tt = pow(color, vec3(0.5 * sqrt(clamp(1.0 - h * h * a * a, 0.0, 1.0)) / cd));
    s += hair_g(b * 0.5, sum - shift) * exp(-3.65 * cphi - 3.98) * (1.0 - f) * (1.0 - f) * tt;
    // TRT: in, reflected inside, out (the colored second highlight)
    f = hair_f(cd * 0.5);
    s += hair_g(b * 2.0, sum - 4.0 * shift) * exp(17.0 * cphi - 16.78) * (1.0 - f) * (1.0 - f) * f * pow(color, vec3(0.8 / cd));
    // the many times scattered: a wrapped light around a normal facing the view, tinted more where the hair is deep
    vec3 nf = normalize(V - T * sv + vec3(1e-6));
    float wrap = clamp((dot(nf, L) + 1.0) * 0.25, 0.0, 1.0);
    float kk = 1.0 - abs(sl);
    float lum = max(dot(color, vec3(0.2126, 0.7152, 0.0722)), 1e-3);
    vec3 tint = pow(color / lum, vec3(1.0 - through));
    s += sqrt(color) * (mix(wrap, kk, 0.33) / PI) * tint;
    return s;
}

float falloff_h(float d, GpuLight L) {
    float w = clamp(1.0 - pow(d * d * L.inv_range2, 2.0), 0.0, 1.0);
    float dm = max(d * 0.01, max(L.source_radius * 0.01, 0.01));
    return w * w / (dm * dm);
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
#ifdef HAIR_DEPTH
    // the depth prepass: the coverage, a strand thinner than the pixel drops the rest of its pixels (each strand its own
    // pattern, turned every frame: TSR averages them)
    uint hs = (uint(gl_FragCoord.x) * 73856093u) ^ (uint(gl_FragCoord.y) * 19349663u) ^ (i_strand * 83492791u) ^ (f.frame_index * 2654435761u);
    hs ^= hs >> 15, hs *= 0x2C1B3C6Du, hs ^= hs >> 12;
    if (float(hs & 0xFFFFu) * (1.0 / 65536.0) >= i_cover) discard;
    return;
#endif
    GroomInfo g = GroomInfoBuf(pc.instances).g[pc.pass];
    vec3 T = normalize(i_tan);
    vec3 V = view_ortho(f) ? -f.camera_fwd.xyz : normalize(f.camera_pos.xyz - i_world);
    vec3 color = clamp(g.color.rgb, vec3(1e-4), vec3(1.0));
    float rough = clamp(g.color.w, 0.1, 1.0);
    uvec2 data = pc.vertices;
    vec3 pl = groom_local(g, i_world);
    vec3 lit = vec3(0.0);
    // the directional lights (the sun on its virtual shadow maps), then the local ones of this point's cell
    int sun = int(f.sun_radiance.w);
    for (uint i = 0u; i < f.light_count; i++) {
        GpuLight L = f.lights[i];
        vec3 l = -L.direction;
        float vis = groom_transmittance(g, data, pl, normalize(groom_local_dir(g, l)));
        if (int(i) == sun) {
            if ((f.gi_flags & GI_VSM) != 0u) vis *= vsm_shadow(f, i_world, l);
            vis *= cloud_shadow(f, i_world);
        }
        if (vis > 1e-4) lit += hair_light(T, V, l, color, rough, vis) * L.radiance * vis;
    }
    LightLists ll = light_lists(f, i_world);
    for (uint lv = ll.lp; lv < LG_LEVELS; lv++) {
        uvec2 rg = light_range(ll, lv);
        for (uint k = 0u; k < rg.y; k++) {
            uint it = light_item(f, rg, k);
            if (!light_take(it, lv, ll.lp)) continue;
            GpuLight L = local_light(f, it);
            vec3 lv3 = L.position - i_world;
            float d = length(lv3);
            vec3 l = lv3 / max(d, 1e-3);
            float atten = falloff_h(d, L);
            if (light_kind(L) == LIGHT_SPOT) {
                float c = clamp((dot(-l, L.direction) - L.cos_outer) / max(L.cos_inner - L.cos_outer, 1e-4), 0.0, 1.0);
                atten *= c * c;
            } else if (light_kind(L) == LIGHT_RECT) atten *= max(dot(-l, L.direction), 0.0);
            if (atten <= 0.0) continue;
            float vis = groom_transmittance(g, data, pl, normalize(groom_local_dir(g, l)));
            lit += hair_light(T, V, l, color, rough, vis) * L.radiance * (atten * vis);
        }
    }
    // the ambient: the GI around (probes; else the sky light) on a normal facing the view, darkened by the hair over it
    vec3 nf = normalize(V - T * dot(T, V) + vec3(1e-6));
    vec3 amb = (f.gi_flags & GI_PROBES) != 0u ? probe_irradiance(f, u_probe_irr, u_probe_dist, i_world, nf, V) * f.gi_params.x
                                              : (f.sky_params.x > 0.0 ? sh_irradiance(f, nf) / PI : vec3(0.0));
    float occ = groom_transmittance(g, data, pl, normalize(groom_local_dir(g, vec3(0.0, 0.0, 1.0)) + groom_local_dir(g, nf)));
    lit += amb * sqrt(color) * mix(1.0, occ, g.ao);
    float dist = length(f.camera_pos.xyz - i_world);
    if (f.sun_illuminance.w > 0.0) {
        vec4 ap = aerial_perspective(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, dist);
        lit = lit * ap.a + ap.rgb;
    }
    vec4 fg = fog_apply(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, -V, dist);
    lit = lit * fg.a + fg.rgb;
    o_color = vec4(min(lit * pre_exposure(f), vec3(60000.0)), 1.0);
    if ((f.gi_flags & GI_AA_WEIGHTED) != 0u) o_color = aa_encode(o_color.rgb);
#if HAIR_OUT >= 2
    o_split = vec4(0.0);
#endif
#if HAIR_OUT >= 4
    o_den = uvec4(0u);
    o_guide = uvec4(0u, 0u, floatBitsToUint(gl_FragCoord.z), 0u);
#endif
}
