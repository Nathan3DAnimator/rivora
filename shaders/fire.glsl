// Fire (A14; fire.c, notas-dev/particulas.md "Fogo"): each Fire actor a gas simulated on a grid of its own on the GPU
// (as Niagara Fluids' Grid 3D Gas): velocity, temperature, smoke and fuel advected semi-Lagrangian, combustion, buoyancy,
// vorticity confinement, the pressure projected by Jacobi; drawn by marching the grid (fire_render.comp). Everything in
// one buffer of vec4s (pc.vertices), the fires' table per frame (pc.instances). A fire's cells (cubic, cell cm):
//   [base + 0N, 1N) velocity A, [1N, 2N) velocity B (cells/s, xyz; ping-pong by FireInfo.cur)
//   [2N, 3N) scalars A, [3N, 4N) scalars B: x temperature (K over the air's), y smoke, z fuel, w the sun's light reaching it
//   [4N, 5N) the curl (xyz, w its length); [5N, 6N) x, y the pressure ping-pong, z the divergence
#ifndef FIRE_GLSL
#define FIRE_GLSL

struct FireInfo {
    vec4 rows[3];       // actor -> world (cm; the grid's box: x, y in [-w/2, w/2], z in [-h/5, 4h/5]: the actor in the flame)
    vec4 inv[3];        // world -> actor
    uvec4 dims;         // nx, ny, nz, its first vec4 (base)
    float cell, dt, time, cur;           // cell size (cm), the step (s), the clock, 0 or 1: the current A/B
    float buoyancy, vorticity, cooling, dissipation; // cells/s^2 per 1000 K, confinement, 1/s, 1/s (smoke)
    float burn, heat, smoke, fuel;       // fuel burnt per s, K per fuel burnt, smoke per fuel burnt, the emitter's fuel
    vec4 emitter;       // xyz its center (cells), w its radius (cells)
    vec4 wind;          // cells/s (xyz), w: turbulence of the emitter
    vec4 look;          // x brightness (nits at 1500 K), y smoke darkness (1/cm per unit), z smoke scattering albedo, w -
    vec4 sun;           // xyz toward the sun (actor space, normalized), w: 1 when there is one
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FireInfoBuf { FireInfo f[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer FireVec4 { vec4 v[]; };

uint fire_n(FireInfo F) { return F.dims.x * F.dims.y * F.dims.z; }
uint fire_idx(FireInfo F, ivec3 c) { return (uint(c.z) * F.dims.y + uint(c.y)) * F.dims.x + uint(c.x); }
bool fire_in(FireInfo F, ivec3 c) { return all(greaterThanEqual(c, ivec3(0))) && all(lessThan(c, ivec3(F.dims.xyz))); }

// a field's value at a place in cells (cell centers at +0.5), trilinear, clamped to the grid (field: its first vec4)
vec4 fire_sample(FireVec4 B, FireInfo F, uint field, vec3 p) {
    vec3 q = clamp(p - 0.5, vec3(0.0), vec3(F.dims.xyz) - 1.001);
    ivec3 c = ivec3(floor(q));
    vec3 t = q - vec3(c);
    ivec3 m = ivec3(F.dims.xyz) - 1;
    ivec3 d = min(c + 1, m);
    vec4 a000 = B.v[field + fire_idx(F, c)], a100 = B.v[field + fire_idx(F, ivec3(d.x, c.y, c.z))];
    vec4 a010 = B.v[field + fire_idx(F, ivec3(c.x, d.y, c.z))], a110 = B.v[field + fire_idx(F, ivec3(d.x, d.y, c.z))];
    vec4 a001 = B.v[field + fire_idx(F, ivec3(c.x, c.y, d.z))], a101 = B.v[field + fire_idx(F, ivec3(d.x, c.y, d.z))];
    vec4 a011 = B.v[field + fire_idx(F, ivec3(c.x, d.y, d.z))], a111 = B.v[field + fire_idx(F, d)];
    return mix(mix(mix(a000, a100, t.x), mix(a010, a110, t.x), t.y), mix(mix(a001, a101, t.x), mix(a011, a111, t.x), t.y), t.z);
}

// A black body's color at T kelvin (Planck at 610, 550, 465 nm), normalized to luminance 1
vec3 blackbody(float T) {
    T = max(T, 500.0);
    vec3 l = vec3(610e-9, 550e-9, 465e-9);
    vec3 b = 1.0 / (pow(l, vec3(5.0)) * (exp(1.4388e-2 / (l * T)) - 1.0));
    return b / max(dot(b, vec3(0.2126, 0.7152, 0.0722)), 1e-30);
}
// its brightness relative to 1500 K's (Planck's at 550 nm: the eye's)
float blackbody_level(float T) {
    T = max(T, 500.0);
    return (exp(1.4388e-2 / (550e-9 * 1500.0)) - 1.0) / (exp(1.4388e-2 / (550e-9 * T)) - 1.0);
}
#endif
