#version 460
// The water's surface (water_surface.vert): the normal from the ocean's slopes (or the lake's waves), the ripples and
// the rain's rings; then water's light: the sky and the sun reflected (Fresnel, n = 1.33), the sun's glints (GGX, rougher
// far away where a pixel covers many waves), the light under it (its deep color lit by the sky and the sun), the sun
// through the crests (scattering), foam where the waves fold. The air and the fog over it, as the meshes'.
// RE_RT (GPUs with ray queries): it is transparent where shallow: a ray along the refracted view finds the bottom (its
// material's own texture there), lit by the sun (its virtual shadow, the waves' caustics) and the sky, seen through the
// water's absorption over that length (its clarity); white water where the bottom comes within a few tens of cm of the
// surface (shores, rocks and posts standing in it).
#extension GL_EXT_ray_query : enable
#include "common.glsl"
#include "atmosphere.glsl"
#include "lights.glsl"
#include "weather.glsl"
#include "vsm.glsl"
#include "water.glsl"
#ifdef RE_RT
#define MAT_SET 1
#include "mat_common.glsl" // (the bottom's material: its baked base color, bound for the main pass)
#include "gi.glsl"
#include "rt.glsl"
layout(set = 0, binding = 22) uniform sampler2D u_cache_direct;   // the surface cache (as mesh.frag's)
layout(set = 0, binding = 23) uniform sampler2D u_cache_indirect;

// The caustics: the sun focused by the waves into a moving net of bright lines on the bottom. Two layers of cells drifting
// against each other; near a border between two cells (F2 - F1 small) is a line. Its mean is ~1 (the light only moved
// around); sharp a few tens of cm deep, spread out and fainter deeper.
float w_cell_edge(vec2 p) {
    vec2 i = floor(p), fp = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 o = vec2(float(x), float(y));
            vec2 c = o + 0.25 + 0.5 * w_hash22(i + o) - fp; // (jittered within the cell's middle: 3 x 3 always holds the 2 nearest, no seams)
            float d = dot(c, c);
            if (d < d1) d2 = d1, d1 = d;
            else if (d < d2) d2 = d;
        }
    return sqrt(d2) - sqrt(d1);
}
float w_caustics(vec2 p, float t, float depth) {
    float k = smoothstep(2.0, 25.0, depth) * exp(-depth / 600.0);
    if (k < 0.01) return 1.0; // (deep: spread out to nothing)
    vec2 q = p / 90.0; // (cm: cells ~90 cm across, as a light breeze's)
    // two octaves of drifting warp: the cells' borders curve as the real ones do (straight ones read as tiles)
    q += vec2(sin(q.y * 1.7 + t * 0.6), cos(q.x * 1.5 - t * 0.5)) * 0.45;
    q += vec2(sin(q.y * 3.9 - t * 0.9), cos(q.x * 3.3 + t * 0.8)) * 0.18;
    float e1 = w_cell_edge(q + vec2(t * 0.08, t * 0.05)), e2 = w_cell_edge(q * 1.9 - vec2(t * 0.07, -t * 0.1));
    float sharp = mix(22.0, 6.0, clamp(depth / 300.0, 0.0, 1.0)); // (deeper: the lines blur)
    float lines = exp(-e1 * sharp) * 0.75 + exp(-e2 * sharp * 1.2) * 0.4;
    return mix(1.0, 0.72 + lines * 2.8, k); // (mean ~1)
}
#endif

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec2 i_base;
layout(location = 2) in float i_spacing;
layout(location = 3) in vec3 i_flow; // (B12c) a river's flow (cm/s; z 1 on a river)

// (B12c) a river's small waves: value noise's slopes at two scales, dragged along the flow in two phases half a period
// apart, each faded out as it resets (a flow map: the waves run downstream without stretching for ever)
float w_vn(vec2 p) {
    vec2 i = floor(p), u = fract(p);
    u = u * u * (3.0 - 2.0 * u);
    return mix(mix(w_hash22(i).x, w_hash22(i + vec2(1, 0)).x, u.x), mix(w_hash22(i + vec2(0, 1)).x, w_hash22(i + vec2(1, 1)).x, u.x), u.y);
}
vec2 w_vn_slope(vec2 p) {
    const float e = 0.08;
    return vec2(w_vn(p + vec2(e, 0.0)) - w_vn(p - vec2(e, 0.0)), w_vn(p + vec2(0.0, e)) - w_vn(p - vec2(0.0, e))) / (2.0 * e);
}
vec2 river_slope(vec2 p, vec2 flow, float t) {
    float sp = length(flow);
    vec2 sl = vec2(0.0);
    for (int k = 0; k < 2; k++) {
        float ph = fract(t * 0.35 + 0.5 * float(k)), wgt = 1.0 - abs(2.0 * ph - 1.0);
        vec2 q = p - flow * (ph * 2.857); // (a period's travel)
        sl += (w_vn_slope(q / 45.0) / 45.0 * 6.0 + w_vn_slope(q / 13.0) / 13.0 * 2.0) * wgt;
    }
    return sl * (0.6 + sp / 150.0);
}
layout(location = 0) out vec4 o_color;

vec2 ripple_slope(FrameBuf f, vec2 p) {
    float e = max(f.water[11].x, 1.0);
    return vec2(ripple_at(f, p + vec2(e, 0.0)) - ripple_at(f, p - vec2(e, 0.0)), ripple_at(f, p + vec2(0.0, e)) - ripple_at(f, p - vec2(0.0, e))) / (2.0 * e);
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    vec3 v = normalize(f.camera_pos.xyz - i_world);
    float dist = length(f.camera_pos.xyz - i_world);
    vec2 sl = vec2(0.0);
    float foam = 0.0, crest = 0.0;
    // the pixel's footprint (cm): the waves under it average out (their slopes fade, the roughness grows)
    float px = fwidth(i_base.x) + fwidth(i_base.y);
    if (pc.pass >= 100u && pc.pass < 200u) { // (the ocean; 200 and up: the rivers, B12c)
        // (2026-11-10) a Water Body Exclusion: no ocean over its rectangle (land below the sea, a dock, a boat's inside)
        for (int k = 0; k < int(f.water[63].x); k++) {
            vec4 q = f.water[64 + 2 * k];
            vec2 cs = f.water[65 + 2 * k].xy, dd = i_world.xy - q.xy;
            vec2 uv = vec2(dot(dd, cs), dot(dd, vec2(-cs.y, cs.x)));
            if (all(lessThanEqual(abs(uv), q.zw))) discard;
        }
        for (uint c = 0u; c < 3u; c++) {
            float L = W_CASCADE_SIDE(f, c) * 100.0, texel = L / float(W_OCEAN_N);
            float fade = 1.0 - smoothstep(texel * 1.5, texel * 6.0, px);
            vec4 d = ocean_tap(f, c, i_base, 1u);
            sl += d.xy * fade;
            vec4 dz = ocean_tap(f, c, i_base, 0u);
            foam += dz.w * fade;
            crest += dz.z * 100.0 * fade;
        }
    } else if (pc.pass >= 200u) { // (B12c) a river
        sl = river_slope(i_base, i_flow.xy, f.water[0].y);
        crest = 0.0;
    } else {
        uint b = min(pc.pass, 3u);
        vec4 a1 = f.water[30u + 2u * b];
        float t = f.water[0].y, kk = a1.z, w = a1.w;
        float x = i_base.x, y = i_base.y;
        float c0 = cos(kk * x - w * t), c1 = cos(kk * 0.7 * (x * 0.6 + y * 0.8) - w * 0.8 * t);
        sl = a1.y * vec2(0.6 * kk * c0 + 0.4 * kk * 0.7 * 0.6 * c1, 0.4 * kk * 0.7 * 0.8 * c1);
        crest = a1.y * 0.6 * sin(kk * x - w * t);
    }
    sl += ripple_slope(f, i_base);
    if (f.water[0].x > 0.0 && (f.water_addr[7].x | f.water_addr[7].y) != 0u) sl += rain_ripple_map(f, i_base) * f.water[2].w * (1.0 - smoothstep(1500.0, 4000.0, dist));
    vec3 n = normalize(vec3(-sl, 1.0));
    if (dot(n, v) < 0.02) n = normalize(n + v * (0.02 - dot(n, v))); // (a wave's back seen at grazing angles)
    foam = clamp(foam, 0.0, 1.0);
    // the light
    float nv = clamp(dot(n, v), 0.0, 1.0);
    float F = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    vec3 r = reflect(-v, n);
    r.z = abs(r.z);
    vec3 sky = sky_radiance(f, normalize(r), false);
    vec3 amb = f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) / PI : vec3(0.0);
    bool lake = pc.pass < 100u;
    uint li = min(pc.pass, 3u); // (both sides of a select may be read: never past the lakes' 4)
    vec3 col = lake ? f.water[40u + li].rgb : f.water[8].rgb, scat = lake ? f.water[44u + li].rgb : f.water[9].rgb;
    float clarity = lake ? f.water[40u + li].w : f.water[8].w;
    float shore = lake ? f.water[44u + li].w : max(f.water[7].w - 1.0, 0.0);
    if (pc.pass >= 200u) { // (B12c) a river's look: its Water Body's
        RiverBuf R = RiverBuf(pc.instances);
        uint base = (pc.pass - 200u) * RIVER_STRIDE;
        vec4 k1 = R.v[base + 1u], k2 = R.v[base + 2u];
        col = k1.rgb, clarity = k1.w, scat = k2.rgb, shore = k2.w;
    }
    vec3 thru = vec3(0.0); // how much of the bottom shows (0: deep, opaque), by channel
    vec3 bottom = vec3(0.0);
#ifdef RE_RT
    // (High/Epic, with the traced reflections: ~0.23 ms with water over the whole view at 1440p; past 150 m the
    // reflection is nearly all one sees at that angle)
    if ((f.gi_flags & (GI_RT | GI_REFLECTIONS)) == (GI_RT | GI_REFLECTIONS) && dist < 15000.0 && clarity > 1.0) {
        vec3 rd = refract(-v, n, 1.0 / 1.33);
        if (dot(rd, rd) < 1e-6) rd = vec3(0.0, 0.0, -1.0);
        float reach = min(clarity * 3.0, 6000.0); // (past it the bottom is under 5% anyway)
        Hit h = trace_closest(f, i_world + rd * 0.6, rd, reach);
        if (h.hit) {
            // its light: the surface cache's (as the traced reflections: the sun, its shadow, the bounce), else the sky
            // and the sun with its virtual shadow; dimmed by the water above it. The direct light (the sun's) moved
            // around by the caustics
            float wd = max(i_world.z - h.pos.z, 0.0); // (the water over the bottom there)
            float ca = f.sun_radiance.w >= 0.0 ? w_caustics(h.pos.xy, f.water[0].y, wd) : 1.0; // (no sun: none)
            vec4 hst = CacheStBuf(f.cache_st).st[h.inst];
            vec3 bl;
            if ((f.gi_flags & GI_SURFACE_CACHE) != 0u && hst.x > 0.0) {
                vec2 uv = h.uv2 * hst.xy + hst.zw;
                bl = (cache_read(u_cache_direct, uv, f.cache_params.x) * ca + cache_read(u_cache_indirect, uv, f.cache_params.x) * f.gi_params.x) * GI_LOAD;
            } else {
                bl = f.sky_params.x > 0.0 ? sh_irradiance(f, h.normal) / PI : vec3(0.0);
                int si = int(f.sun_radiance.w);
                if (si >= 0) {
                    GpuLight L = f.lights[si];
                    float bs = (f.gi_flags & GI_VSM) != 0u ? vsm_shadow(f, h.pos, h.normal) : 1.0;
                    bl += L.radiance * bs * max(dot(h.normal, -L.direction), 0.0) / PI * ca;
                }
            }
            // its color: the material's baked base color at the hit (the mip by the ray's cone), its mean for an
            // animated one
            vec3 alb = h.base;
            Instance hi = InstanceBuf(f.instances).i[h.inst];
            if ((hi.ids.y & INST_MATERIAL) != 0u) {
                GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(hi.emissive_rough.x)];
                alb = gm.avg.xyz;
                if (gm.anim.x <= 1u) {
                    vec2 muv = h.uv * hi.emissive_rough.y; // (the tiling, as mesh.vert)
                    float dens = h.uv_density * hi.emissive_rough.y;
                    if ((gm.flags & MATF_WORLD_UV) != 0u) {
                        vec3 gn = abs(h.normal);
                        vec2 wp = gn.z >= gn.x && gn.z >= gn.y ? h.pos.xy : gn.x >= gn.y ? vec2(h.pos.y, -h.pos.z) : vec2(h.pos.x, -h.pos.z);
                        muv = wp / max(gm.avg.w, 1.0), dens = 1.0 / max(gm.avg.w, 1.0);
                    }
                    // a ray cone: the pixel's angle over the path (to the surface, then down), widened where the ray
                    // meets the bottom slanted; its texels across it
                    float foot = (dist + h.t) * 2.0 * f.camera_fwd.w * f.viewport.z / max(abs(dot(rd, h.normal)), 0.2);
                    float lod = log2(max(foot * dens * float(textureSize(u_mat_tex[nonuniformEXT(gm.tex_base)], 0).x), 1.0));
                    alb = srgb_to_linear(textureLod(u_mat_tex[nonuniformEXT(gm.tex_base)], muv, lod).rgb);
                    if ((gm.flags & MATF_AFFINE) != 0u) alb = mat_affine_base(f.materials, floatBitsToUint(hi.emissive_rough.x), alb); // (2026-11-24)
                    if ((gm.flags & MATF_WORLD_GRID) != 0u) { // the grid material's lines (as mesh.frag's, the cone for fwidth)
                        vec3 an = abs(h.normal);
                        vec2 wp = an.z >= an.x && an.z >= an.y ? h.pos.xy : an.x >= an.y ? h.pos.yz : h.pos.xz;
                        float fw = max(foot, 1e-3);
                        vec2 d1 = abs(fract(wp / 100.0 - 0.5) - 0.5) * 100.0 / fw, d2 = abs(fract(wp / 10.0 - 0.5) - 0.5) * 10.0 / fw;
                        float l1 = 1.0 - clamp(min(d1.x, d1.y) - 0.5, 0.0, 1.0), l2 = (1.0 - clamp(min(d2.x, d2.y) - 0.5, 0.0, 1.0)) * clamp(4.0 - fw, 0.0, 1.0);
                        float sq = mod(floor(wp.x / 100.0) + floor(wp.y / 100.0), 2.0);
                        alb *= (sq > 0.5 ? 0.9 : 1.0) * mix(1.0, 0.45, l1) * mix(1.0, 0.8, l2);
                    }
                }
            }
            bottom = alb * (1.0 - h.metal) * bl * exp(-wd / clarity * vec3(2.2, 1.0, 0.75)) + h.emissive;
            thru = exp(-h.t / clarity * vec3(2.2, 1.0, 0.75)); // (Beer-Lambert: the water takes the red first, then the green)
            // shore foam: the bottom within ~35 cm under the surface, broken up and drifting
            float depth = max(i_world.z - h.pos.z, 0.0) * abs(rd.z);
            float edge = 1.0 - smoothstep(4.0, 35.0, depth);
            if (shore > 0.0 && edge > 0.0) {
                float t = f.water[0].y;
                float nz = w_hash12(floor(i_base / 9.0 + vec2(t * 0.35, t * 0.2))) * 0.5 + w_hash12(floor(i_base / 23.0 - vec2(t * 0.15, 0.0))) * 0.5;
                foam = max(foam, shore * edge * smoothstep(0.35, 0.7, nz + edge * 0.45));
            }
        }
    }
#endif
    vec3 under = col * amb;
    vec3 deep = 1.0 - thru; // (the deep color's share: the rest is the bottom seen through)
    vec3 spec = vec3(0.0);
    int sli = int(f.sun_radiance.w);
    if (sli >= 0) {
        GpuLight L = f.lights[sli];
        vec3 l = -L.direction;
        float sh = 1.0;
        if ((f.gi_flags & GI_VSM) != 0u && dot(n, l) > 0.0) sh = vsm_shadow(f, i_world, n);
        float nl = max(dot(n, l), 0.0);
        // the sun's glints: GGX, rougher far away (the waves under a pixel)
        float a = clamp(0.03 + px * 0.0015, 0.03, 0.35);
        vec3 h = normalize(l + v);
        float nh = max(dot(n, h), 0.0), a2 = a * a, dd = nh * nh * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * dd * dd);
        float Fh = 0.02 + 0.98 * pow(1.0 - max(dot(h, v), 0.0), 5.0);
        float Vis = 0.25 / max(nl * nv + 1e-3, 1e-3);
        spec = L.radiance * sh * nl * D * Fh * min(Vis, 50.0);
        under += col * L.radiance * sh * max(l.z, 0.0) / PI * 0.6;
        // the sun through the crests (looking toward it, higher waves glow)
        float through = pow(clamp(dot(v, -l) * 0.5 + 0.5, 0.0, 1.0), 4.0) * clamp(0.3 + crest / 120.0, 0.0, 1.5);
        under += scat * L.radiance * sh * through * 0.15;
        under = under * deep + bottom * thru; // (the bottom seen through, before the foam over it)
        thru = vec3(0.0);
        // foam: lit like a rough white surface
        vec3 foam_col = vec3(0.85) * (amb + L.radiance * sh * nl / PI);
        float fn = foam * smoothstep(0.25, 0.75, w_hash12(floor(i_base / 18.0)) * 0.5 + foam * 0.7);
        under = mix(under, foam_col, fn);
        F *= 1.0 - fn;
        spec *= 1.0 - fn;
    }
    under = under * (1.0 - thru) + bottom * thru; // (no sun: blended here)
    vec3 lit = mix(under, sky, F) + spec;
    if (f.sun_illuminance.w > 0.0) {
        vec4 ap = aerial_perspective(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, dist);
        lit = lit * ap.a + ap.rgb;
    }
    vec4 fg = fog_apply(f, gl_FragCoord.xy * f.viewport.zw * 2.0 - 1.0, -v, dist);
    lit = lit * fg.a + fg.rgb;
    o_color = vec4(min(lit * pre_exposure(f), vec3(60000.0)), 1.0);
    if ((f.gi_flags & GI_AA_WEIGHTED) != 0u) o_color = aa_encode(o_color.rgb);
}
