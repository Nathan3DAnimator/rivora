#ifndef WEATHER_GLSL
#define WEATHER_GLSL
// Clouds and fog (weather.c, weather.comp; notas-dev/luz.md "Céu, nuvens e neblina"): what the shaders that see the sky
// or light a surface read. Included after common.glsl and atmosphere.glsl.
//  - Clouds: a panorama around the camera (azimuth x elevation) of their light (for 1 lux of sun) and transmittance,
//    updated a quarter of its rows per frame; their shadow on the ground, a top-down map around the camera.
//  - Fog (Unreal's Exponential Height Fog): its light per froxel (128 x 72 x 64 over the view, slices exponential in
//    distance up to the volumetric distance) integrated front to back; beyond, the analytic height fog.

layout(set = 0, binding = 27) uniform sampler2D u_cloud_pano;
layout(set = 0, binding = 29) uniform sampler2D u_cloud_shadow;
layout(set = 0, binding = 31) uniform sampler2D u_fog;
layout(set = 0, binding = 49) uniform sampler2D u_cloud_hist; // the screen's clouds (weather.comp pass 6; cloud_scr's part of it)
#define CLOUD_SCR_W 1920.0 // its image (weather.c): half of 4K
#define CLOUD_SCR_H 1080.0

#define WEATHER_FOG 1u
#define WEATHER_FOG_VOLUME 2u
#define WEATHER_CLOUDS 4u
#define PANO_EL_MIN -0.12 // radians: the panorama's lowest elevation (a little below the horizon)
#define FOG_NEAR 50.0     // cm: the first froxel slice
#define FOG_X 128
#define FOG_Y 72
#define FOG_Z 64

vec2 pano_uv(vec3 d) {
    float az = atan(d.y, d.x) / (2.0 * PI) + 0.5;
    float el = asin(clamp(d.z, -1.0, 1.0));
    return vec2(az, clamp((el - PANO_EL_MIN) / (0.5 * PI - PANO_EL_MIN), 0.0, 1.0));
}
vec3 pano_dir(vec2 uv) {
    float az = (uv.x - 0.5) * 2.0 * PI, el = PANO_EL_MIN + uv.y * (0.5 * PI - PANO_EL_MIN);
    return vec3(cos(el) * cos(az), cos(el) * sin(az), sin(el));
}

// The clouds toward d: rgb their light (the frame's sun applied), a the sky's light that gets through them
vec4 clouds_toward(FrameBuf f, vec3 d) {
    if ((f.weather_flags & WEATHER_CLOUDS) == 0u || d.z < PANO_EL_MIN) return vec4(0.0, 0.0, 0.0, 1.0);
    vec4 c = textureLod(u_cloud_pano, pano_uv(d), 0.0);
    return vec4(c.rgb * f.sun_illuminance.rgb, c.a);
}

// The screen's clouds at a view point (ndc), where pass 6 made them; else the panorama's
vec4 clouds_screen(FrameBuf f, vec2 ndc, vec3 d) {
    if ((f.weather_flags & WEATHER_CLOUDS) == 0u || d.z < PANO_EL_MIN) return vec4(0.0, 0.0, 0.0, 1.0);
    if (f.cloud_scr.w < 0.5) return clouds_toward(f, d);
    // magnified 2-4x: Catmull-Rom (five bilinear reads), sharp edges where bilinear blurred them into clay
    vec2 tsz = vec2(CLOUD_SCR_W, CLOUD_SCR_H), p = (ndc * 0.5 + 0.5) * f.cloud_scr.xy - 0.5;
    vec2 i = floor(p), t = p - i;
    vec2 w0 = t * (-0.5 + t * (1.0 - 0.5 * t)), w1 = 1.0 + t * t * (-2.5 + 1.5 * t), w2 = t * (0.5 + t * (2.0 - 1.5 * t)), w3 = t * t * (-0.5 + 0.5 * t);
    vec2 w12 = w1 + w2, o12 = w2 / w12;
    vec2 lim = f.cloud_scr.xy - 0.5; // (inside the part pass 6 wrote)
    vec2 c0 = max(i - 0.5, vec2(0.5)) / tsz, c3 = min(i + 2.5, lim) / tsz, c12 = clamp(i + 0.5 + o12, vec2(0.5), lim) / tsz;
    vec4 c = textureLod(u_cloud_hist, vec2(c12.x, c0.y), 0.0) * w12.x * w0.y +
             textureLod(u_cloud_hist, vec2(c0.x, c12.y), 0.0) * w0.x * w12.y + textureLod(u_cloud_hist, c12, 0.0) * w12.x * w12.y +
             textureLod(u_cloud_hist, vec2(c3.x, c12.y), 0.0) * w3.x * w12.y + textureLod(u_cloud_hist, vec2(c12.x, c3.y), 0.0) * w12.x * w3.y;
    c /= (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y);
    c = max(c, vec4(0.0));
    c.a = min(c.a, 1.0);
    return vec4(c.rgb * f.sun_illuminance.rgb, c.a);
}

// How much sun reaches p through the clouds (1: clear)
float cloud_shadow(FrameBuf f, vec3 p) {
    if ((f.weather_flags & WEATHER_CLOUDS) == 0u) return 1.0;
    vec2 uv = (p.xy - f.cloud_shadow_origin.xy) / f.cloud_shadow_origin.z + 0.5;
    float t = textureLod(u_cloud_shadow, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).r;
    return mix(1.0, t, f.cloud1.w);
}

// (A9.5b) the Local Fog Volumes (weather.c): world -> unit space rows, shape / density / edge / height falloff, albedo, emissive
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FogVolBuf { vec4 v[]; };
float fog_density_at(FrameBuf f, float z) { return f.fog0.x * exp(-f.fog0.y * (z - f.fog0.z)); }

float hg_phase(float g, float c) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// The analytic height fog between distances t0 and t1 along d (from the camera): its optical depth
float fog_optical_depth(FrameBuf f, vec3 d, float t0, float t1) {
    if (t1 <= t0) return 0.0;
    float k = f.fog0.y * d.z;
    float base = fog_density_at(f, f.camera_pos.z);
    if (abs(k) < 1e-7) return base * (t1 - t0);
    return base * (exp(-k * t0) - exp(-k * t1)) / k;
}

// The light the analytic fog scatters toward the camera per unit of its opacity (no shadows: the far part)
vec3 fog_far_light(FrameBuf f, vec3 d) {
    vec3 amb = (sh_irradiance(f, vec3(0.0, 0.0, 1.0)) + sh_irradiance(f, vec3(0.0, 0.0, -1.0))) * (0.5 / PI);
    // (Q21) the sky atmosphere lighting the fog (Unreal's Sky Atmosphere affecting the Height Fog): its ambient part is the
    // sky's light from the side the view looks toward (the sky light's SH at the horizon ahead: brighter and warmer toward
    // the sun, bluer away from it), not the gray of the whole sky: far away the fog takes the color of the sky behind
    // it, the aerial perspective's. (The sky-view LUT's horizon row cost the ray traced main pass 16 bytes of spill; the
    // SH is already in its registers.) far_u.x FAR_FOG_SKY, far_g.z its share
    if ((f.far_u.x & 4u) != 0u && f.far_g.z > 0.0) {
        vec2 hz = dot(d.xy, d.xy) > 1e-6 ? normalize(d.xy) : vec2(1.0, 0.0);
        amb = mix(amb, sh_irradiance(f, vec3(hz, 0.0)) * (1.0 / PI), f.far_g.z);
    }
    vec3 sun = vec3(0.0);
    for (uint i = 0u; i < f.light_count; i++)
        if (light_kind(f.lights[i]) == LIGHT_DIRECTIONAL) sun += f.lights[i].radiance * hg_phase(f.fog1.w, dot(d, -f.lights[i].direction));
    return f.fog1.rgb * (amb + sun) + f.fog2.rgb;
}

// The fog between the camera and a point dist_cm away along d (seen at ndc): rgb the light it adds, a what is left of
// the point's light. occl: how much of the sky the point sees (1 outdoors): the analytic part has no shadows, so the
// fog in front of a wall indoors (a tunnel, a house) glowed with the sun and the sky through the walls
vec4 fog_apply_occl(FrameBuf f, vec2 ndc, vec3 d, float dist_cm, float occl) {
    if ((f.weather_flags & WEATHER_FOG) == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    float start = f.fog2.w, far = f.fog3.x;
    vec3 L = vec3(0.0);
    float T = 1.0, from = start;
    if ((f.weather_flags & WEATHER_FOG_VOLUME) != 0u && dist_cm > FOG_NEAR) {
        // the froxels: the slice of the distance (exponential), bilinear in the slice, linear between two slices
        float s = log(min(dist_cm, far) / FOG_NEAR) / log(far / FOG_NEAR) * float(FOG_Z) - 1.0;
        vec2 uv = clamp((ndc * 0.5 + 0.5) * vec2(FOG_X, FOG_Y), vec2(0.5), vec2(FOG_X, FOG_Y) - 0.5);
        vec2 size = vec2(float(FOG_X * FOG_Z), float(FOG_Y));
        float s0 = floor(s), fr = s - s0;
        vec4 a = s0 < 0.0 ? vec4(0.0, 0.0, 0.0, 1.0) : textureLod(u_fog, (vec2(s0 * float(FOG_X), 0.0) + uv) / size, 0.0);
        vec4 b = textureLod(u_fog, (vec2(min(s0 + 1.0, float(FOG_Z - 1)) * float(FOG_X), 0.0) + uv) / size, 0.0);
        vec4 v = mix(a, b, clamp(fr, 0.0, 1.0));
        L = v.rgb;
        T = v.a;
        from = max(far, start);
    }
    if (dist_cm > from) { // the analytic fog beyond the froxels
        float tau = fog_optical_depth(f, d, from, dist_cm);
        float t = exp(-tau);
        L += T * fog_far_light(f, d) * (1.0 - t) * occl;
        T *= t;
    }
    T = max(T, 1.0 - f.fog0.w); // Max Opacity
    return vec4(L, T);
}
vec4 fog_apply(FrameBuf f, vec2 ndc, vec3 d, float dist_cm) { return fog_apply_occl(f, ndc, d, dist_cm, 1.0); }

#ifndef RE_DONT_UNROLL
#define RE_DONT_UNROLL
#endif
// (2026-11-20) The volumetric fog along any segment inside the view (a mirror's reflection ray: the sun's shafts seen
// in a puddle): each of 8 points read from the froxels, the light that slice scatters per cm (the integrated light's
// step over the slice's thickness, divided by what reached it) and its extinction (the transmittance's step), the sun's
// phase turned from the camera's direction to the segment's (clamped: a ratio of Henyey-Greensteins is spiky). Points
// outside the view or past the froxels add nothing (a reflection of the sky behind the camera stays clean). rgb the light
// added, a what is left of the reflected light
vec4 fog_segment(FrameBuf f, vec3 p, vec3 dir, float len) {
    if ((f.weather_flags & (WEATHER_FOG | WEATHER_FOG_VOLUME)) != (WEATHER_FOG | WEATHER_FOG_VOLUME) || len <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    float far = f.fog3.x, lr = log(far / FOG_NEAR), stp = len / 8.0, g = f.fog1.w;
    vec2 size = vec2(float(FOG_X * FOG_Z), float(FOG_Y));
    float ph_seg = hg_phase(g, dot(dir, f.sun_dir.xyz));
    vec3 L = vec3(0.0);
    float T = 1.0;
    // (the projection is linear: the start's clip position and a step's, not the matrix, live through the loop: with
    // the matrix in it mesh_rt spilled 16 bytes more to local memory)
    vec4 c0 = f.tsr_vp * vec4(p + dir * (0.5 * stp), 1.0), cs = f.tsr_vp * vec4(dir * stp, 0.0);
    RE_DONT_UNROLL // (not unrolled where the shader can say so: mesh.frag)
    for (int i = 0; i < 8; i++) {
        vec3 x = p + dir * ((float(i) + 0.5) * stp);
        vec4 c = c0 + cs * float(i);
        if (c.w <= 1.0) continue;
        vec2 ndc = c.xy / c.w;
        if (any(greaterThan(abs(ndc), vec2(1.0)))) continue;
        vec3 vc = x - f.camera_pos.xyz;
        float dist = length(vc);
        if (dist <= FOG_NEAR || dist >= far) continue;
        float s0 = floor(log(dist / FOG_NEAR) / lr * float(FOG_Z) - 1.0);
        if (s0 < 0.0 || s0 >= float(FOG_Z - 1)) continue;
        vec2 uv = clamp((ndc * 0.5 + 0.5) * vec2(FOG_X, FOG_Y), vec2(0.5), vec2(FOG_X, FOG_Y) - 0.5);
        vec4 a = textureLod(u_fog, (vec2(s0 * float(FOG_X), 0.0) + uv) / size, 0.0);
        vec4 b = textureLod(u_fog, (vec2((s0 + 1.0) * float(FOG_X), 0.0) + uv) / size, 0.0);
        float d0 = FOG_NEAR * exp((s0 + 1.0) / float(FOG_Z) * lr), d1 = FOG_NEAR * exp((s0 + 2.0) / float(FOG_Z) * lr);
        float th = max(d1 - d0, 1.0), ta = max(a.a, 1e-3);
        vec3 scat = max(b.rgb - a.rgb, vec3(0.0)) / ta / th;               // the light this slice scatters to the camera, per cm
        float sigma = -log(clamp(b.a / ta, 1e-4, 1.0)) / th;              // its extinction, per cm
        float k = clamp(ph_seg / max(hg_phase(g, dot(vc / dist, f.sun_dir.xyz)), 1e-4), 0.0, 8.0);
        float tr = exp(-sigma * stp);
        L += T * scat * k * (sigma > 1e-7 ? (1.0 - tr) / sigma : stp); // (the step integrated, its own extinction in it)
        T *= tr;
    }
    return vec4(L, T);
}
#endif
