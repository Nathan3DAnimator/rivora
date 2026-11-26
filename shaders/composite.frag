#version 460
// The scene into the swapchain's viewport rectangle: the HDR target with the Post Process Volumes' depth of field and
// motion blur gathered here (only where they change something: pp[7].x, render.c fill_post), the bloom, the lens
// (chromatic aberration, sharpen), color grading (white balance, saturation, contrast, gamma, gain, offset), exposure,
// ACES (fitted) tone mapping, vignette, grain, sRGB; and Unreal's orange selection outline from the mask.
#include "common.glsl"
#include "water.glsl" // (rain_on)
#include "vsm.glsl"   // (Q25b: the virtual things' shadows on the real room)

// A white noise from integers (PCG): the sin hash (fract(sin(dot(xy, k)) * 43758)) lost its precision on 1440p's pixel
// coordinates plus the time and drew lines and grids that crawled over the picture (the grain, the motion blur's jitter)
uint pcg_hash(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float noise01(uvec2 p, uint frame) { return float(pcg_hash(p.x + pcg_hash(p.y + pcg_hash(frame)))) * (1.0 / 4294967296.0); }

layout(set = 0, binding = 1) uniform sampler2D u_hdr;
layout(set = 0, binding = 2) uniform sampler2D u_mask;
layout(set = 0, binding = 21) uniform sampler2D u_depth;    // the main pass's, one sample (the post's blur; Q25b: mixed reality's alpha)
#ifdef RE_POST // (the variant render.c binds when pp[7].x: a frame with something to blur)
layout(set = 0, binding = 40) uniform sampler2D u_velocity; // the moved actors' screen motion (with pp[7].y)
#endif
layout(set = 0, binding = 41) uniform sampler2D u_bloom;
layout(set = 0, binding = 80) uniform sampler2D u_lut;
layout(set = 0, binding = 81) uniform sampler2D u_ui;  // (A9.4) HDR10: the game's UI drawn apart (sRGB, premultiplied by ImGui's blend)
#include "mr.glsl" // (Q25b) mixed reality

// (Q25b) Mixed reality: the eye's picture over the headset's passthrough, premultiplied (the compositor: rgb + real x (1 -
// alpha)). Alpha 0 where nothing virtual is drawn (Background), under a Passthrough Window, where a real thing is nearer
// (the real depth, the room's boxes; a soft ramp); the real surfaces darkened by the virtual things' sun shadow. What is
// drawn without depth over the empty background (sparks, glows, glass) stays, added over the real world. Simulated: the
// stand-in room drawn in the real world's place
vec4 mr_out(FrameBuf f, vec3 c, vec2 uv) {
    float d = texelFetch(u_depth, ivec2(uv * vec2(textureSize(u_depth, 0))), 0).r;
    vec3 ro = f.camera_pos.xyz, rd = view_ray(f, uv * 2.0 - 1.0);
    float dist = d > 0.0 ? f.mr[9].z / d / max(dot(rd, f.camera_fwd.xyz), 0.05) : 1e9; // (along the ray, cm)
    bool empty = d <= 0.0 && (f.mr_flags & MR_BACKGROUND) != 0u;
    float a = empty ? 0.0 : 1.0;
    vec3 rn = vec3(0.0, 0.0, 1.0);
    float tw = -1.0, tr = -1.0, rl = -1.0;
    if (f.mr_box_count > 0u) mr_hits(f, ro, rd, tw, tr, rn, rl); // (the windows' and the room's surface along the ray)
    if (tw > 0.0 && tw < dist) a = 0.0, empty = false; // (a Passthrough Window: the real world over the virtual, its glows too)
    if (a > 0.0 && d > 0.0) { // a real thing nearer than the virtual one hides it
        float occ = 0.0;
        if ((f.mr_flags & MR_DEPTH) != 0u) occ = mr_depth_occlusion(f, ro + rd * dist);
        if ((f.mr_flags & MR_ROOM_OCCLUDES) != 0u && tr > 0.0) occ = max(occ, clamp((dist - tr - 1.0) / max(f.mr[0].z, 0.5), 0.0, 1.0));
        a *= 1.0 - occ;
    }
    a *= f.mr[0].y; // (the virtual's opacity: Set Passthrough Blend)
    // the real surface seen here: the room's box (exact), else the real depth's (any real thing, coarser)
    vec3 p = ro + rd * max(tr, 0.0), n = rn;
    bool real = tr > 0.0;
    if (!real && (f.mr_flags & MR_DEPTH) != 0u) {
        vec3 dp, dn;
        if (mr_depth_surface(f, ro, rd, dp, dn)) p = dp, n = dn, real = true;
    }
    float dark = 0.0;
    if (a < 1.0 && real && (f.mr_flags & MR_SHADOWS) != 0u && (f.gi_flags & GI_VSM) != 0u && f.sun_dir.w > 0.0 && dot(n, f.sun_dir.xyz) > 0.0)
        dark = f.mr[0].w * (1.0 - clamp(vsm_shadow(f, p + n * 1.5, n), 0.0, 1.0));
    float alpha = a + (1.0 - a) * dark;
    vec3 rgb = empty ? max(c - 0.02, vec3(0.0)) : c * a; // (the empty background's glows added; the grain's haze left out)
    if ((f.mr_flags & MR_SIM) != 0u) {
        vec3 room = mr_style(f, real ? mr_sim_room(f, p, n, rl, true, gl_FragCoord.xy) : vec3(0.75)); // (no room: a light wall, the edges seen)
        return vec4(rgb + room * (1.0 - alpha), 1.0);
    }
    if ((f.mr_flags & 64u) != 0u) return vec4(alpha > 0.004 ? min(rgb / alpha, vec3(1.0)) : rgb, alpha); // (vr.MRUnpremultiplied)
    return vec4(rgb, alpha);
}

// (A9.4) HDR10's encoding: linear BT.709 nits -> BT.2020 -> PQ (SMPTE ST.2084, 10000 nits at 1)
vec3 to_pq(vec3 nits) {
    const mat3 to2020 = mat3(0.6274040, 0.0690970, 0.0163916, 0.3292820, 0.9195400, 0.0880132, 0.0433136, 0.0113612, 0.8955950);
    vec3 y = pow(clamp(to2020 * nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
} // (A9.4) the Color Grading LUT: an N*N x N strip (x: red + blue slice x N, y: green)

vec3 from_srgb(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
// The LUT in display space (sRGB encoded, as Unreal's and the .cube files' grading): the two blue slices around the
// color, each sampled bilinearly in red and green, blended
vec3 grade_lut(vec3 lin, float n) {
    vec3 s = clamp(vec3(1.055) * pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4)) - 0.055, 0.0, 1.0); // (to sRGB: the LUT's space)
    float b = s.b * (n - 1.0), b0 = floor(b), b1 = min(b0 + 1.0, n - 1.0);
    float u = (s.r * (n - 1.0) + 0.5) / (n * n), v = (s.g * (n - 1.0) + 0.5) / n;
    vec3 c0 = texture(u_lut, vec2(u + b0 / n, v)).rgb, c1 = texture(u_lut, vec2(u + b1 / n, v)).rgb;
    return from_srgb(mix(c0, c1, b - b0));
}


// (Q13) The horror kit's filter stack (terror.c terror_view, GpuFrame.terror; notas-dev/terror.md): every step skipped at
// zero, the noise stepped by the kit's own clock (24 steps a second: the same picture for the same time, the tests'
// determinism), nothing read but this pixel's color except the VHS's chroma and the aberration (in the lens block)
// The distortion (a warping wave and a breathing barrel) and the VHS's tracking (lines pushed sideways, a band rolling
// down) move where the picture is read
// (one hash per line, per cell, per pixel: their bytes are the noises; 0.39 ms -> under 0.3 at 1440p with 16 noise01s)
float byte01(uint h, uint k) { return float((h >> (k * 8u)) & 255u) * (1.0 / 255.0); }
vec2 terror_uv(FrameBuf f, vec2 uv) {
    float d = f.terror[1].z, v = f.terror[1].x, t = f.terror[3].x;
    if (d > 0.0) {
        vec2 q = uv - 0.5;
        uv += d * (vec2(sin(uv.y * 17.0 + t * 3.1), cos(uv.x * 13.0 + t * 2.3)) * 0.011 + q * 0.07 * sin(t * 1.7) * dot(q, q) * 4.0);
    }
    if (v > 0.0) {
        uint hl = pcg_hash(uint(uv.y * 240.0) * 7919u ^ pcg_hash(uint(f.terror[3].y) + 101u)); // (this line's: its jitter, the band's push)
        float in_band = 1.0 - smoothstep(0.0, 0.035, abs(uv.y - fract(t * 0.11)));
        uv.x += v * ((byte01(hl, 0u) - 0.5) * 0.003 + in_band * (byte01(hl, 1u) - 0.3) * 0.03);
    }
    return uv;
}
// After the tone mapping and the grain, in display light: the toe, the tension's vignette, pulse, grain and color, then
// each filter over what is under it (Hallucination, Black & White Blown Out, VHS, TV Static, White Flash), the dip
vec3 terror_display(FrameBuf f, vec3 c, vec2 uv, vec2 osize) {
    uint step_ = uint(f.terror[3].y);
    uvec2 px = uvec2(gl_FragCoord.xy);
    uint hp = pcg_hash((px.x * 1973u + px.y * 9277u) ^ pcg_hash(step_ * 3u + 1u)); // (this pixel's noises: grain, tape, snow)
    float toe = f.terror[3].z;
    if (toe > 0.0) { // the shadows pushed down, never lifted (1 stays 1, the curve under the line)
        float k = toe * 0.3;
        c = c * c * (1.0 + k) / (c + k);
    }
    vec2 q = uv - 0.5;
    float vig = f.terror[2].y + f.terror[2].w * 0.35;
    if (vig > 0.0) c *= mix(1.0, smoothstep(0.85, 0.15, dot(q, q) * 2.2), min(vig, 1.0)) * (1.0 - f.terror[2].w * 0.08); // (the heart: the edges close in)
    if (f.terror[2].z != 1.0) c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, f.terror[2].z), vec3(0.0)); // the tension drains the color
    if (f.terror[2].x > 0.0) c = max(c + (byte01(hp, 0u) - 0.5) * f.terror[2].x * 0.12 * (1.0 - dot(c, vec3(0.33))), vec3(0.0));
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float h = f.terror[0].x;
    if (h > 0.0) { // Hallucination: a sensor blown out, red, coarse colored noise, the blacks crushed and the whites burnt
        uint cs = uint(max(floor(osize.y / 240.0), 2.0)); // (cells of ~6 px at 1440p)
        uint hc = pcg_hash((px.x / cs) * 1973u + (px.y / cs) * 9277u ^ pcg_hash(step_ * 5u + 7u));
        vec3 n3 = vec3(byte01(hc, 0u), byte01(hc, 1u), byte01(hc, 2u));
        float m = smoothstep(0.06, 0.55, l + (n3.x - 0.5) * 0.25);
        float lv = max(f.terror[6].w, 2.0);
        m = floor(m * lv + 0.5) / lv; // posterized
        vec3 r = clamp(m * f.terror[6].rgb * 1.4 + (n3 - 0.5) * 0.45 * (0.3 + m), 0.0, 1.0);
        c = mix(c, r, h);
    }
    float bw = f.terror[0].y;
    if (bw > 0.0) { // Black & White Blown Out: the picture overexposed in grey, the black lifted into it
        float b = 1.0 - exp(-l * 7.0 - 0.45);
        c = mix(c, vec3(b), bw);
    }
    float v = f.terror[1].x;
    if (v > 0.0) { // VHS: scanlines, the color washed, a little warm, the tape's noise and the tracking band's white streaks
        float sl = 0.5 + 0.5 * sin(gl_FragCoord.y * 3.14159);
        float band = 1.0 - smoothstep(0.0, 0.035, abs(uv.y - fract(f.terror[3].x * 0.11)));
        vec3 vc = mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, 0.7) * vec3(1.04, 1.0, 0.92) * (1.0 - 0.14 * sl);
        float streak = band > 0.0 ? step(0.93, byte01(pcg_hash((px.x / 9u) * 1973u + px.y * 9277u ^ step_), 0u)) * band : 0.0;
        vc += (byte01(hp, 1u) - 0.5) * 0.06 + streak * 0.5;
        c = mix(c, clamp(vc, 0.0, 1.0), v);
    }
    float st = f.terror[0].w;
    if (st > 0.0) { // TV Static: snow, the scanlines, a slow rolling bar
        float sn = float((hp >> 16u) & 0xFFFFu) * (1.0 / 65535.0);
        float roll = 0.85 + 0.15 * sin(uv.y * 6.0 - f.terror[3].x * 7.0);
        c = mix(c, vec3(sn * roll * (0.8 + 0.2 * sin(gl_FragCoord.y * 3.14159))), st);
    }
    if (f.terror[0].z > 0.0) c = mix(c, vec3(1.0), f.terror[0].z); // White Flash
    if (f.terror[4].w > 0.0) c = mix(c, f.terror[4].rgb, f.terror[4].w); // Dip to Black / White
    return c;
}

layout(location = 0) in vec2 i_ndc;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
// AgX (Troy Sobotka; Blender's view transform), the sigmoid as Wrensch's polynomial fit, linear sRGB in and out: a wider
// range than ACES and bright saturated light that goes to white instead of to the primaries (A5a). Punchy: Blender's
// look (more contrast and color)
vec3 agx(vec3 c, bool punchy) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
                            0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
                             -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float lo = -12.47393, hi = 4.026069;
    c = clamp(log2(max(inset * c, vec3(1e-10))), lo, hi);
    c = (c - lo) / (hi - lo);
    vec3 x2 = c * c, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    if (punchy) {
        c = pow(max(c, vec3(0.0)), vec3(1.35));
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = l + 1.4 * (c - l);
    }
    return clamp(pow(max(outset * c, vec3(0.0)), vec3(2.2)), 0.0, 1.0);
}

#ifdef RE_POST
// The camera ray of a pixel (uv) at a view depth (cm): its world point
vec3 world_at(FrameBuf f, vec2 uv, float vz) {
    vec2 ndc = uv * 2.0 - 1.0;
    float th = f.camera_fwd.w;
    vec3 d = f.camera_fwd.xyz + ndc.x * th * f.camera_right.xyz - ndc.y * th / f.camera_right.w * f.camera_up.xyz;
    return f.camera_pos.xyz + d * vz;
}
float view_z(FrameBuf f, float d) { return d > 0.0 ? f.pp[5].w / d : 1e9; } // reversed infinite Z: the sky at 0
// the blur's radius (px): |z - focus| / z times k (render.c: the simple model's 8 / f-stop of the size, or a Cine Camera's
// circle of confusion on its sensor), at most Max Bokeh Size
float coc_of(FrameBuf f, float vz) { return min(abs(vz - f.pp[5].x) / max(vz, 1.0) * f.pp[5].y, f.pp[5].z); }
// (A9.4) the iris's polygon: a tap's radius scaled to the polygon's edge at its angle (n blades; 0: the round disc)
vec2 iris(vec2 d, float n, float rot) {
    if (n < 3.0) return d;
    float a = atan(d.y, d.x) - rot, seg = 6.2831853 / n;
    float m = mod(a, seg) - seg * 0.5;
    return d * (cos(seg * 0.5) / cos(m));
}

// Depth of field then motion blur at this pixel; false when neither changes it (the plain HDR value is used)
bool post_gather(FrameBuf f, vec2 uv, vec2 texel, ivec2 px, inout vec3 c) {
    if (view_ortho(f)) return false; // (an orthographic view, the editor's: no depth of field nor motion blur)
    float d = texelFetch(u_depth, px, 0).r;
    float vz = view_z(f, d);
    bool changed = false;
    // --- Depth of field: a gather on a disc, each tap weighed by its own blur so the sharp doesn't smear ---
    if (f.pp[5].x > 0.0) {
        float coc = coc_of(f, vz);
        if (coc > 0.75) {
            const vec2 disc[16] = vec2[16](vec2(0.0, 0.0), vec2(0.54, 0.1), vec2(-0.2, 0.52), vec2(-0.5, -0.24), vec2(0.18, -0.56),
                                           vec2(0.95, 0.3), vec2(0.35, 0.93), vec2(-0.55, 0.8), vec2(-0.98, 0.18), vec2(-0.75, -0.62),
                                           vec2(-0.1, -0.99), vec2(0.6, -0.78), vec2(0.88, -0.32), vec2(0.72, 0.62), vec2(-0.3, 0.2), vec2(0.25, -0.2));
            int n = coc > 6.0 ? 16 : 8; // a small blur: half the taps
            vec3 acc = vec3(0.0);
            float wsum = 0.0;
            for (int i = 0; i < n; i++) {
                vec2 su = uv + iris(disc[i], f.grade_lut.z, f.grade_lut.w) * coc * texel;
                float svz = view_z(f, texelFetch(u_depth, ivec2(su * vec2(textureSize(u_depth, 0))), 0).r); // (the render's size: TSR)
                float w = svz < vz ? clamp(coc_of(f, svz) / max(coc, 1e-3), 0.0, 1.0) : 1.0; // a nearer tap: by its own blur
                acc += textureLod(u_hdr, su, 0.0).rgb * w;
                wsum += w;
            }
            c = acc / max(wsum, 1e-4);
            changed = true;
        }
    }
    // --- Motion blur (Unreal's amount as the shutter, capped at a share of the screen) ---
    float amount = f.pp[4].x;
    if (amount > 0.0) {
        bool actors = f.pp[7].y != 0.0;
        vec2 vel = actors ? texelFetch(u_velocity, px, 0).xy : vec2(0.0); // the actors' own motion
        if (f.pp[4].z != 0.0 && d > 0.0) { // the camera's: this pixel's world point where last frame's view had it
            vec4 prev = f.prev_view_proj * vec4(world_at(f, uv, vz), 1.0);
            vel += uv - (prev.xy / max(prev.w, 1e-4) * 0.5 + 0.5);
        }
        vel *= amount;
        float len = length(vel), cap = f.pp[4].y;
        if (len > cap) vel *= cap / len, len = cap;
        float len_px = len / texel.x;
        if (len_px > 1.5) { // (a shorter streak doesn't show)
            int n = int(clamp(len_px * 0.34, 3.0, 8.0)); // a tap per three pixels of the streak (the jitter fills between)
            vec2 size = vec2(textureSize(u_depth, 0)); // (the depth's and the velocity's size: the render's with TSR)
            vec3 acc = c;
            float wsum = 1.0;
            float jitter = noise01(uvec2(px), f.frame_index & 7u); // breaks the steps' bands
            for (int i = 0; i < n; i++) {
                vec2 su = uv + vel * ((float(i) + jitter) / float(n) - 0.5);
                // a tap of the background over a moving thing in front: kept out (it would ghost through it)
                // (only the moving actors ghost: with the camera's motion alone every tap counts)
                float w = 1.0;
                if (actors) {
                    ivec2 sp = clamp(ivec2(su * size), ivec2(0), ivec2(size) - 1);
                    float sd = texelFetch(u_depth, sp, 0).r;
                    if (sd < d * 0.995 && length(texelFetch(u_velocity, sp, 0).xy) * amount < len * 0.25) w = 0.25;
                }
                acc += textureLod(u_hdr, su, 0.0).rgb * w;
                wsum += w;
            }
            c = acc / wsum;
            changed = true;
        }
    }
    return changed;
}
#endif

// (B12h) Under water (water.c f.water[50]: the surface's height where the camera is, 1, the clarity; [51] its color):
// this pixel's point on the near plane under the surface (the waterline cuts the view when the lens sits at it), the
// view wavering a little (RE_POST: render.c picks the variant while the camera is in water), the light absorbed by the water between (red first, then green: it turns blue-green) toward
// the water's own color lit by the sky and the sun from above, darker deeper
#ifdef RE_POST
bool under_water(FrameBuf f, vec2 uv) {
    if (f.water[50].y < 0.5) return false;
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 d = view_ray(f, ndc);
    float zn = view_origin(f, ndc).z + d.z * f.pp[5].w / max(dot(d, f.camera_fwd.xyz), 0.05);
    return zn < f.water[50].x;
}
vec3 water_fog(FrameBuf f, vec2 uv, vec3 c) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 d = view_ray(f, ndc);
    float dep = texelFetch(u_depth, ivec2(uv * vec2(textureSize(u_depth, 0))), 0).r;
    float dist = min(view_z(f, dep) / max(dot(d, f.camera_fwd.xyz), 0.05), 1e6);
    float clarity = max(f.water[50].z, 1.0);
    vec3 absorb = exp(-dist / (clarity * vec3(0.18, 0.6, 1.0)));
    float below = max(f.water[50].x - f.camera_pos.z, 0.0);
    vec3 light = (f.sky_params.x > 0.0 ? sh_irradiance(f, vec3(0.0, 0.0, 1.0)) / PI : vec3(0.0));
    int si = int(f.sun_radiance.w);
    if (si >= 0) light += f.lights[si].radiance * max(-f.lights[si].direction.z, 0.0) / PI;
    vec3 fogc = f.water[51].rgb * 4.0 * light * exp(-below / (clarity * vec3(0.3, 0.8, 1.2))) * pre_exposure(f);
    return c * absorb + fogc * (1.0 - absorb);
}
// (2026-11-10) the Stylized outline: 0..1 where the depth steps by more than style[1].w of the distance within
// style[0].w pixels (the silhouettes, the edges of objects in front of others); the sky's edge counts too
float toon_edge(FrameBuf f, vec2 uv) {
    float w = f.style[0].w;
    if (w <= 0.0) return 0.0;
    vec2 ds = vec2(textureSize(u_depth, 0));
    ivec2 c = ivec2(uv * ds), hi = ivec2(ds) - 1;
    float zc = view_z(f, texelFetch(u_depth, c, 0).r);
    int r = int(ceil(w));
    float zn = zc;
    const ivec2 dirs[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int i = 0; i < 4; i++) zn = min(zn, view_z(f, texelFetch(u_depth, clamp(c + dirs[i] * r, ivec2(0), hi), 0).r));
    float step_ = (zc - zn) / max(zn, 1.0); // (the nearer neighbor draws the line: one line, on the far side of the edge)
    return smoothstep(f.style[1].w, f.style[1].w * 2.0, step_) * clamp(w, 0.0, 1.0);
}
#else
bool under_water(FrameBuf f, vec2 uv) { return false; }
vec3 water_fog(FrameBuf f, vec2 uv, vec3 c) { return c; }
float toon_edge(FrameBuf f, vec2 uv) { return 0.0; }
#endif

// (2026-11-15, "a tela reagir igual uma câmera real gravando na chuva com água escorrendo pelo visor"; "dependendo do
// ângulo ... ter inércia, poder mexer a câmera rápido para tirar a chuva") water on the lens, the Rain's Lens Water: the
// drops simulated on the CPU (lens_water.c: gravity by the camera's tilt, sticking, inertia, shaken off, merging, their
// trails), a tile's list of what touches it read here. A drop on the lens is out of focus: what is behind it comes
// through bent (a small lens: from the other side of its center) and blurred, darker at its rim, with no hard edge; a
// running drop stretched into a tail behind it; a trail a faint wet smear. (The tiles: lens_water.h's layout)
#define LENS_MAX 160u
#define LENS_TRAIL 8u
#define LENS_TX 32
#define LENS_TY 18
#define LENS_SEG_BASE (1u + 2u * LENS_MAX)
#define LENS_TILE_BASE (LENS_SEG_BASE + 2u * LENS_MAX * (LENS_TRAIL - 1u))
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LensBuf { vec4 v[]; };
vec3 lens_water(FrameBuf f, vec2 uv, vec2 osize, vec3 c) {
    LensBuf lb = LensBuf(uvec2(floatBitsToUint(f.water[91].x), floatBitsToUint(f.water[91].y)));
    float asp = f.water[90].w;
    vec2 p = vec2(uv.x * asp, uv.y);
    ivec2 t = clamp(ivec2(floor(uv * vec2(LENS_TX, LENS_TY))), ivec2(0), ivec2(LENS_TX - 1, LENS_TY - 1));
    uint base = LENS_TILE_BASE + uint(t.y * LENS_TX + t.x) * 7u;
    int n = int(lb.v[base].x);
    float best = 0.0, cov = 0.0, film = 0.0, flake = 0.0;
    vec2 sl = vec2(0.0);
    for (int k = 0; k < n; k++) {
        uint slot = uint(k + 1);
        uint it = uint(lb.v[base + slot / 4u][slot % 4u] + 0.5);
        if (it < LENS_MAX) { // a drop: a cap, stretched into a tail behind where it runs
            vec4 d0 = lb.v[1u + it * 2u], d1 = lb.v[2u + it * 2u];
            vec2 q = p - d0.xy, dir = d1.xy;
            float al = dot(q, dir), ac = dot(q, vec2(-dir.y, dir.x));
            vec2 e = vec2(ac, al < 0.0 ? al / d1.z : al) / d0.z;
            float q2 = dot(e, e);
            if (q2 >= 1.0) continue;
            float h = sqrt(1.0 - q2) * d0.w;
            if (h <= best) continue;
            best = h;
            cov = clamp((1.0 - q2) * 5.0, 0.0, 1.0) * d0.w;
            flake = d1.w;
            sl = (dir * e.y + vec2(-dir.y, dir.x) * e.x) * (0.6 / max(sqrt(1.0 - q2), 0.25));
        } else { // a piece of a trail
            uint j = it - LENS_MAX;
            vec4 s0 = lb.v[LENS_SEG_BASE + j * 2u], s1 = lb.v[LENS_SEG_BASE + j * 2u + 1u];
            vec2 ab = s0.zw - s0.xy;
            float h = clamp(dot(p - s0.xy, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
            float dd = length(p - s0.xy - ab * h);
            film = max(film, (1.0 - smoothstep(s1.x * 0.4, s1.x, dd)) * s1.y);
        }
    }
    if (cov + film <= 0.0) return c;
    float blur = f.water[90].y, refr = f.water[90].z;
    float rim = clamp(length(sl) * 0.9, 0.0, 1.0);
    vec2 q = uv - vec2(sl.x / asp, sl.y) * (0.035 * cov + 0.006 * film) * refr;
    float r = (0.003 + 0.008 * cov) * blur;
    vec3 s = vec3(0.0);
    for (int i = 0; i < 6; i++) {
        float ang = float(i) * 1.0471976 + 0.5;
        s += textureLod(u_hdr, q + vec2(cos(ang) / asp, sin(ang)) * r, 0.0).rgb;
    }
    s *= (1.0 / 6.0) * (1.0 - 0.35 * rim * cov);
    // (2026-11-15) a snowflake on the lens: white ice scattering the light around, soft, turning to water as it melts
    if (flake > 0.0) s = mix(s, vec3(dot(s, vec3(0.2126, 0.7152, 0.0722))) * 1.5 + c * 0.25, flake * 0.85);
    return mix(c, s, clamp(cov * (1.0 - 0.55 * smoothstep(0.5, 1.0, rim)) * 0.92 + film * 0.45, 0.0, 1.0));
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    // (the HDR target's size: the output's; f.viewport.zw is the render's, smaller with TSR)
    vec2 osize = vec2(textureSize(u_hdr, 0));
    vec2 uv = (gl_FragCoord.xy - f.viewport.xy) / osize;
    int style = int(f.pp[7].w + 0.5); // (render.h: 0 Stylized, 1 Realistic, 2 Photorealistic, 3 Video-realistic)
    float hdr_over = 0.0; // (A9.4: the scene's brightness past white, for HDR10's highlights)
    if (style == 3 && !unlit_mode(f.view_mode)) { // Video-realistic: a real lens's barrel distortion, slight
        vec2 q0 = uv - 0.5;
        float k = 0.035 * f.style[3].y; // (Project Settings > Rendering > Lens Distortion: x the style's)
        uv = 0.5 + q0 * (1.0 - k * dot(q0, q0)) / (1.0 - k * 0.5);
    }
    bool terror_on = f.terror[3].w > 0.0 && !unlit_mode(f.view_mode); // (Q13) the horror kit's frame (terror.c)
    if (terror_on && (f.terror[1].z > 0.0 || f.terror[1].x > 0.0)) uv = terror_uv(f, uv);
    vec2 texel = 1.0 / osize;
    bool unlit = unlit_mode(f.view_mode);
    bool wet = !unlit && under_water(f, uv); // (B12h) the view wavers under water
    if (wet) uv += vec2(sin(uv.y * 40.0 + f.water[0].y * 2.1), cos(uv.x * 37.0 + f.water[0].y * 1.7)) * 0.0025;
    vec3 c = texture(u_hdr, uv).rgb;
#ifdef RE_POST
    bool blurred = !unlit && post_gather(f, uv, texel, ivec2(uv * vec2(textureSize(u_depth, 0))), c);
#else
    const bool blurred = false;
#endif
    if (wet) c = water_fog(f, uv, c); // (B12h)
    if (f.water[85].x > 0.0) c *= 1.0 + 2.5 * f.water[85].x; // (2026-11-14) the lightning's flash (the Rain's Thunder and Lightning)
    if (!unlit && !blurred) {
        float ca = style == 3 ? max(f.pp[0].w, 0.15 * f.style[3].z) : f.pp[0].w; // (Video-realistic: the lens's own fringes, x Lens Fringes)
        vec2 bleed = vec2(0.0);
        if (terror_on) ca += f.terror[1].y * 10.0, bleed = vec2(f.terror[1].x * 0.004, 0.0); // (Q13) the Chromatic Aberration filter, the VHS's chroma smeared sideways
        if (ca > 0.0 || bleed.x > 0.0) { // chromatic aberration: red and blue pulled apart toward the edges
            vec2 dir = (uv - 0.5) * ca * 0.006;
            c = vec3(texture(u_hdr, uv + dir + bleed).r, c.g, texture(u_hdr, uv - dir + bleed * 0.5).b);
        }
        // the post volume's sharpen plus TSR's own (its upscale softens a little: more the smaller the render; f.tsr_size
        // is zero without TSR)
        // (2026-11-14, "a resolução dinâmica fica com um blur bem feio") stronger the further down the render goes: 0.2 at
        // native, 0.5 at 75%, 0.9 at 50% and under (held in the cross's range: no halos however strong)
        float sharpen = f.pp[4].w + (f.tsr_size.z > 0.0 ? mix(0.9, 0.2, smoothstep(0.5, 1.0, f.tsr_size.x / f.tsr_size.z)) : 0.0);
        if (sharpen > 0.0) { // an unsharp mask (the cross's mean taken away), held inside the cross's range (no halos)
            vec3 a = texture(u_hdr, uv + vec2(texel.x, 0)).rgb, b = texture(u_hdr, uv - vec2(texel.x, 0)).rgb;
            vec3 d = texture(u_hdr, uv + vec2(0, texel.y)).rgb, e = texture(u_hdr, uv - vec2(0, texel.y)).rgb;
            vec3 lo = min(min(min(a, b), min(d, e)), c), hi = max(max(max(a, b), max(d, e)), c);
            c = clamp(c + (c - (a + b + d + e) * 0.25) * sharpen, lo, hi);
        }
    }
    // (2026-11-15: the Rain's Lens Water, after the fringes and the sharpen: they read the screen again around the pixel, and
    // a drop's refracted green with the unrefracted red and blue came out purple and green)
    if (!wet && !unlit && rain_on(f) && f.water[90].x > 0.5) c = lens_water(f, uv, osize, c);
    if (!unlit && f.pp[7].z != 0.0) {
        vec3 bl = texture(u_bloom, uv).rgb;
        c += bl * f.pp[0].x * 0.25; // bloom (the chain's sum)
        if (style == 3) c += bl * vec3(0.5, 0.12, 0.05) * f.pp[0].x * 0.2 * f.style[3].w; // halation: film's red halo around bright light (x Halation)
    }
    // the target holds the scene times the exposure it was rendered with (pre-exposure): now the adapted one
    ExposureBuf e = ExposureBuf(f.exposure);
    float ex = e.exposure / max(e.pre_exposure, 1e-12);
    if (unlit) c = clamp(c, 0.0, 1.0);
    else {
        c *= ex;
        // color grading in linear light (Unreal's order: white balance, saturation, contrast, gamma, gain, offset)
        // (each step skipped at its neutral value: the defaults cost nothing)
        c *= f.pp[2].yzw * f.pp[3].xyz;
        if (f.pp[1].x != 1.0) c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, f.pp[1].x), vec3(0.0));
        if (f.pp[1].y != 1.0) c = 0.18 * pow(max(c / 0.18, vec3(0.0)), vec3(f.pp[1].y));
        if (f.pp[1].z != 1.0) c = pow(max(c, vec3(0.0)), vec3(1.0 / max(f.pp[1].z, 0.05)));
        c = c * f.pp[1].w + f.pp[2].x * 0.1;
        hdr_over = clamp((dot(c, vec3(0.2126, 0.7152, 0.0722)) * 0.6 - 1.0) / 4.0, 0.0, 1.0); // (A9.4: how far past white, for HDR10)
        if (style >= 2) c = agx(max(c, vec3(0.0)) * 0.6, false); // Photorealistic, Video-realistic: AgX (the same exposure)
        else c = aces(max(c, vec3(0.0)) * 0.6); // the fit expects 0.6x
        if (style == 0) { // Stylized (Project Settings > Rendering > Stylized): the light in bands (its hue kept), the
            // edges between them hard or soft, the darker bands tinted, the colors stronger
            float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
            float nb = max(f.style[0].x, 2.0), x = l * nb, fl = floor(x), soft = max(f.style[0].y, 1e-3);
            float lq = (fl + 0.6 + smoothstep(1.0 - soft, 1.0, x - fl)) / nb; // (soft: each band eases into the next)
            c *= min(lq / max(l, 1e-4), 4.0);
            c *= mix(f.style[1].rgb, vec3(1.0), smoothstep(0.0, 0.55, lq)); // (the shadows' tint)
            c = clamp(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, f.style[0].z), 0.0, 1.0);
            c = mix(c, f.style[2].rgb, toon_edge(f, uv)); // the outline
        }
        if (f.grade_lut.x > 0.0) c = mix(c, grade_lut(c, f.grade_lut.y), f.grade_lut.x); // (A9.4) the volumes' Color Grading LUT
        // vignette (the corners darker) and film grain (animated)
        vec2 q = uv - 0.5;
        c *= mix(1.0, smoothstep(0.85, 0.15, dot(q, q) * 2.2), style == 3 ? max(f.pp[0].z, 0.3 * f.style[4].x) : f.pp[0].z);
        // (Video-realistic: the sensor's grain, stronger in the dark; Project Settings > Film Grain scales it, 0 none)
        float g = (style == 3 ? max(f.pp[3].w, 0.04) : f.pp[3].w) * f.style[2].w;
        if (g > 0.0) {
            float n = noise01(uvec2(gl_FragCoord.xy), f.frame_index) - 0.5; // (a new pattern every frame, no structure)
            c = max(c + n * g * 0.12 * (1.0 - dot(c, vec3(0.33))), vec3(0.0));
        }
        if (terror_on) c = terror_display(f, c, (gl_FragCoord.xy - f.viewport.xy) / osize, osize); // (Q13) the filter stack
    }
    if (f.terror[1].w > 0.0) { // (Q13) the letterbox: black bars over the picture (the undistorted rows)
        float yv = (gl_FragCoord.y - f.viewport.y) / osize.y;
        if (yv < f.terror[1].w || yv > 1.0 - f.terror[1].w) c = vec3(0.0);
    }
    // Outline: where the selection's object id changes (mask.frag: an id per selected object, 0 outside), on the side of
    // the smaller id: around the selection and between two selected objects (only with a selection: pad0). Measured here,
    // drawn below in the SDR's sRGB and (2026-11-25, item 17) in HDR10's nits too (it returned before the outline)
    float ol_near = 0.0, ol_hl = 0.0, ol_glow = 0.0;
    if (f.pad0 != 0u) {
        ivec2 ms = textureSize(u_mask, 0);
        ivec2 pcx = ivec2(uv * vec2(ms));
        float id = texelFetch(u_mask, pcx, 0).r;
        const ivec2 taps[8] = ivec2[8](ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(1, 1), ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1));
        float tv[8];
        for (int i = 0; i < 8; i++) {
            float s = texelFetch(u_mask, clamp(pcx + taps[i], ivec2(0), ms - 1), 0).r;
            tv[i] = s;
            if (s > id + 0.002) ol_near = 1.0, ol_hl = max(ol_hl, step(0.998, s)); // (the game's highlight: the top id)
        }
        // (2026-11-25, item 17) a gap a few pixels wide inside one actor (between its parts: a rig's neck, a Mannequin's
        // joints): the same id on both opposite sides (the taps come in pairs) is the actor's inside, no line there. Only the
        // outer silhouette is outlined, as Unreal's
        if (id < 0.002)
            for (int i = 0; i < 8; i += 2)
                if (tv[i] > 0.002 && abs(tv[i] - tv[i + 1]) < 0.002) ol_near = 0.0;
        // the game's highlight (interaction.h): a warm white outline (IAR_OUTLINE, pad0 bit 1), its silhouette lit a little
        // (IAR_GLOW, bit 2); the editor's selection stays Unreal's orange
        uint hm = f.pad0 >> 1;
        if (ol_hl > 0.0 && (hm & 1u) == 0u) ol_near = 0.0;
        if ((hm & 2u) != 0u && id > 0.998) ol_glow = 1.0;
    }
    vec3 ol_color = ol_hl > 0.0 ? vec3(1.0, 0.93, 0.7) : vec3(1.0, 0.62, 0.12); // (sRGB)
    if (f.hdr.x > 0.0) { // (A9.4) HDR10: the picture at paper white, its highlights opened toward the peak; the UI over it; PQ
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        vec3 nits = c * f.hdr.y * (1.0 + (f.hdr.z / f.hdr.y - 1.0) * hdr_over * smoothstep(0.6, 1.0, l));
        nits = mix(nits, from_srgb(ol_color) * f.hdr.y, ol_near); // (the outline at paper white)
        if (ol_glow > 0.0) nits = nits * 1.18 + vec3(0.04) * f.hdr.y;
        nits = mix(nits, f.fade.rgb * f.hdr.y, clamp(f.fade.a, 0.0, 1.0));
        if (f.hdr.w > 0.0) {
            vec4 u = texelFetch(u_ui, ivec2(gl_FragCoord.xy), 0);
            if (u.a > 0.0) nits = mix(nits, from_srgb(clamp(u.rgb / u.a, 0.0, 1.0)) * f.hdr.y, clamp(u.a, 0.0, 1.0));
        }
        o_color = vec4(to_pq(nits), 1.0);
        return;
    }
    c = to_srgb(c);
    c = mix(c, ol_color, ol_near);
    if (ol_glow > 0.0) c = mix(c, vec3(1.0, 0.95, 0.8), 0.18) + vec3(0.04);    if ((f.mr_flags & MR_ON) != 0u) { // (Q25b) mixed reality: the alpha over the passthrough; the fade covers the real world too
        vec4 m = mr_out(f, c, uv);
        float fa = clamp(f.fade.a, 0.0, 1.0);
        o_color = vec4(mix(m.rgb, f.fade.rgb, fa), mix(m.a, 1.0, fa));
        return;
    }
    c = mix(c, f.fade.rgb, clamp(f.fade.a, 0.0, 1.0)); // (B9: the Sequencer's Fade track, over everything)
    o_color = vec4(c, 1.0);
}
