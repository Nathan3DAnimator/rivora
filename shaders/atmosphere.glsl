// The physical sky (notas-dev/luz.md): Earth's atmosphere after Hillaire 2020 ("A Scalable and Production Ready Sky
// and Atmosphere Rendering Technique"). Rayleigh, Mie and ozone; LUTs made by atmosphere.comp (render.c/atmosphere.c):
// transmittance (256x64), multiple scattering (32x32) and the sky seen from the camera (192x108), all for a sun of
// illuminance 1: sky_radiance multiplies by the sun's (lux, colored). Distances in km here. Included after common.glsl.

layout(set = 0, binding = 13) uniform sampler2D u_transmittance;
layout(set = 0, binding = 14) uniform sampler2D u_skyview;
layout(set = 0, binding = 15) uniform sampler2D u_multiscatter;

#define ATMO_RG 6360.0  // ground radius
#define ATMO_RT 6460.0  // top of the atmosphere
#define SUN_COS_RADIUS 0.99998911 // the sun's angular radius: 0.2675 degrees
#define SUN_SOLID_ANGLE 6.8418e-5 // sr

const vec3 RAYLEIGH_SCATTERING = vec3(5.802, 13.558, 33.1) * 1e-3;
const float MIE_SCATTERING = 3.996e-3;
const float MIE_EXTINCTION = 4.40e-3;
const float MIE_G = 0.8;
const vec3 OZONE_ABSORPTION = vec3(0.650, 1.881, 0.085) * 1e-3;
const vec3 GROUND_ALBEDO = vec3(0.3);

struct Medium {
    vec3 scattering, extinction, rayleigh;
    float mie;
};

Medium medium_at(float r) {
    float h = max(r - ATMO_RG, 0.0);
    float dr = exp(-h / 8.0), dm = exp(-h / 1.2), dozone = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    Medium m;
    m.rayleigh = RAYLEIGH_SCATTERING * dr;
    m.mie = MIE_SCATTERING * dm;
    m.scattering = m.rayleigh + vec3(m.mie);
    m.extinction = m.rayleigh + vec3(MIE_EXTINCTION * dm) + OZONE_ABSORPTION * dozone;
    return m;
}

float phase_rayleigh(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }
float phase_mie(float c) { // Cornette-Shanks
    float g2 = MIE_G * MIE_G;
    return 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + c * c)) / ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * MIE_G * c, 1e-4), 1.5));
}

// The nearest positive distance along (r up, mu) to the sphere of radius R, -1 without one
float ray_sphere(float r, float mu, float R) {
    float b = r * mu, c = r * r - R * R, d = b * b - c;
    if (d < 0.0) return -1.0;
    float s = sqrt(d), t0 = -b - s, t1 = -b + s;
    return t0 > 0.0 ? t0 : t1 > 0.0 ? t1 : -1.0;
}
bool hits_ground(float r, float mu) { return mu < 0.0 && r * r * (mu * mu - 1.0) + ATMO_RG * ATMO_RG >= 0.0; }

// Bruneton's mapping of the transmittance LUT: (r, mu) <-> uv
vec2 transmittance_uv(float r, float mu) {
    float H = sqrt(ATMO_RT * ATMO_RT - ATMO_RG * ATMO_RG);
    float rho = sqrt(max(r * r - ATMO_RG * ATMO_RG, 0.0));
    float disc = r * r * (mu * mu - 1.0) + ATMO_RT * ATMO_RT;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dmin = ATMO_RT - r, dmax = rho + H;
    return vec2((d - dmin) / max(dmax - dmin, 1e-6), rho / H);
}
void transmittance_r_mu(vec2 uv, out float r, out float mu) {
    float H = sqrt(ATMO_RT * ATMO_RT - ATMO_RG * ATMO_RG);
    float rho = H * uv.y;
    r = sqrt(rho * rho + ATMO_RG * ATMO_RG);
    float dmin = ATMO_RT - r, dmax = rho + H, d = dmin + uv.x * (dmax - dmin);
    mu = d == 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}
vec3 transmittance(float r, float mu) {
    if (hits_ground(r, mu)) return vec3(0.0);
    return textureLod(u_transmittance, transmittance_uv(r, mu), 0.0).rgb;
}
vec3 multiscatter(float r, float mu_sun) {
    return textureLod(u_multiscatter, vec2(mu_sun * 0.5 + 0.5, (r - ATMO_RG) / (ATMO_RT - ATMO_RG)), 0.0).rgb;
}

// The viewer's radius (km) from the camera (cm above the ground at z = 0)
float view_radius(FrameBuf f) { return ATMO_RG + clamp(f.camera_pos.z * 1e-5, 0.001, 80.0); }

// Hillaire's sky-view parameterization: the horizon gets the texels; u: the azimuth from the sun (0..pi, mirrored)
vec2 skyview_uv(float r, vec3 d, vec3 sun) {
    float vh = sqrt(max(r * r - ATMO_RG * ATMO_RG, 0.0));
    float beta = acos(clamp(vh / r, -1.0, 1.0)), zh = PI - beta; // the horizon's zenith angle
    float vz = acos(clamp(d.z, -1.0, 1.0));
    float v;
    if (vz < zh) v = (1.0 - sqrt(max(1.0 - vz / zh, 0.0))) * 0.5;
    else v = sqrt(max((vz - zh) / beta, 0.0)) * 0.5 + 0.5;
    vec2 dh = d.xy, sh = sun.xy;
    float ld = length(dh), ls = length(sh);
    float cphi = ld > 1e-5 && ls > 1e-5 ? dot(dh / ld, sh / ls) : 1.0;
    float u = sqrt(clamp(-cphi * 0.5 + 0.5, 0.0, 1.0));
    vec2 size = vec2(textureSize(u_skyview, 0));
    return (vec2(u, v) * (size - 1.0) + 0.5) / size;
}

// The sky's radiance toward -d (looking along d), in nits; disk: with the sun itself (limb darkened)
vec3 sky_radiance(FrameBuf f, vec3 d, bool disk) {
    float r = view_radius(f);
    vec3 sun = f.sun_dir.xyz;
    vec3 L = textureLod(u_skyview, skyview_uv(r, d, sun), 0.0).rgb;
    if (disk) {
        float c = dot(d, sun);
        if (c > SUN_COS_RADIUS) {
            float t = clamp((1.0 - c) / (1.0 - SUN_COS_RADIUS), 0.0, 1.0); // 0 at the center, 1 at the limb
            float limb = (1.0 - 0.6 * (1.0 - sqrt(1.0 - t))) / 0.8;
            L += transmittance(r, d.z) * (limb / SUN_SOLID_ANGLE);
        }
    }
    return L * f.sun_illuminance.rgb;
}

// --- Aerial perspective: the atmosphere between the camera and what it sees (atmosphere.comp pass 4, every frame). A
// 32x32x32 froxel volume over the view (an atlas: 32 slices of 32x32 side by side), slices quadratic in distance up to
// AP_MAX_KM; per froxel the in-scattered light (1 lux of sun) and the mean transmittance. The frame's
// sun_illuminance.w scales the distances (1: physical; Unreal's Aerial Perspective View Distance Scale). ---
layout(set = 0, binding = 20) uniform sampler2D u_aerial;
#define AP_RES 32
#define AP_MAX_KM 64.0
float ap_slice_km(float s) { float x = (s + 1.0) / float(AP_RES); return AP_MAX_KM * x * x; }

// For a point seen at ndc, dist_cm away: rgb in-scattered light (lux of the frame's sun applied), a transmittance
// (2026-11-25, item 19: the sky's blue inside the procedural facility's closed rooms, 4 of their 5 nits) Unreal's Aerial
// Perspective Start Depth (0.1 km, its default): no air's light in the first 100 m. The froxels know no walls, and the
// first slice (62.5 m) read through its square root gave a point 10 m away 40% of it, in every room of every interior
#define AP_START_KM 0.1
vec4 aerial_perspective(FrameBuf f, vec2 ndc, float dist_cm) {
    float km = max(dist_cm * 1e-5 - AP_START_KM, 0.0) * f.sun_illuminance.w;
    if (km <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    float w = sqrt(clamp(km / AP_MAX_KM, 0.0, 1.0)) * float(AP_RES) - 1.0; // the slice index (fraction)
    vec2 uv = clamp((ndc * 0.5 + 0.5) * float(AP_RES), vec2(0.5), vec2(float(AP_RES) - 0.5)); // inside the slice
    vec2 size = vec2(float(AP_RES * AP_RES), float(AP_RES));
    float s0 = floor(w), fr = w - s0;
    vec4 a = s0 < 0.0 ? vec4(0.0, 0.0, 0.0, 1.0) : textureLod(u_aerial, (vec2(s0 * float(AP_RES), 0.0) + uv) / size, 0.0);
    vec4 b = textureLod(u_aerial, (vec2(min(s0 + 1.0, float(AP_RES - 1)) * float(AP_RES), 0.0) + uv) / size, 0.0);
    vec4 r = mix(a, b, fr);
    return vec4(r.rgb * f.sun_illuminance.rgb, r.a);
}

// The sky toward d with the clouds over it (weather.glsl: the shaders that include it define WEATHER_GLSL first)
#ifdef WEATHER_GLSL
vec3 sky_clouds(FrameBuf f, vec3 d, bool disk) {
    vec4 c = clouds_toward(f, d);
    return sky_radiance(f, d, disk) * c.a + c.rgb;
}
#endif
