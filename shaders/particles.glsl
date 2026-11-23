// The particles (particles.c, notas-dev/particulas.md): an emitter's block (render_gpu.h GpuEmitter), its pool and the
// deterministic births: slot i of an emitter is born at a time that comes from i alone (the Tlender's way), so no
// free list, no atomics to spawn, and the same result every run. A particle keeps its position, velocity, birth time
// and birth index (32 bytes); its lifetime, size, color and spin come from a hash of (seed, slot, birth index).
#ifndef PARTICLES_GLSL
#define PARTICLES_GLSL

#define PF_LOCAL 1u       // the pool in the emitter's space (the actor carries it)
#define PF_TL 2u          // the Tlender's births: count over [tl_start, tl_end]
#define PF_LOOP 4u        // the Tlender's: born again after the end
#define PF_GRAVITY 8u
#define PF_DRAG 16u
#define PF_CURL 32u
#define PF_VORTEX 64u
#define PF_ATTRACT 128u
#define PF_WIND 256u
#define PF_COLLIDE 512u
#define PF_LIMIT 1024u
#define PF_KILLBOX 2048u
#define PF_COLOR_LIFE 4096u
#define PF_SIZE_LIFE 8192u
#define PF_BROWNIAN 16384u
#define PF_MESH 32768u    // the mesh renderer (else sprites)
#define PF_LIT 65536u     // sprites lit by the sun and the sky (a sphere's normal)
#define PF_ADDITIVE 131072u // sprites added (else blended over)
#define PF_VELOCITY_ALIGN 262144u // sprites stretched along their velocity (sparks, rain)
#define PF_SHADOWS 524288u // meshes receive the sun's shadow
#define PF_SUBUV 1048576u  // a flipbook: the texture's cells over the life, at a frame rate or one at random
#define PF_SPEED_SCALE 2097152u // size and alpha by the speed (Niagara's Scale Sprite Size / Color by Speed)
#define PF_SUBUV_BLEND 4194304u // the flipbook blends a frame into the next
#define PF_RIBBON 8388608u // (B10) the Ribbon Renderer (ribbon.vert)

#define PSHAPE_POINT 0u
#define PSHAPE_SPHERE 1u
#define PSHAPE_BOX 2u
#define PSHAPE_CONE 3u
#define PSHAPE_DISC 4u
#define PSHAPE_MESH_FACES 5u
#define PSHAPE_MESH_VERTS 6u
#define PSHAPE_MESH_VOLUME 7u
#define PSHAPE_CYLINDER 8u // shape_p: x radius, y height, z surface only
#define PSHAPE_TORUS 9u    // shape_p: x major radius, y minor radius, z surface only

struct Particle { vec3 pos; float birth; vec3 vel; uint k; }; // k: the birth's index (0xFFFFFFFF: none yet)
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer ParticleBuf { Particle p[]; };
struct ShapeTri { vec4 a, b, c; }; // a.w: the cumulative area up to it (0..1)
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ShapeBuf { ShapeTri t[]; };

struct Emitter {
    vec4 rows[3];          // emitter -> world
    uvec2 pool, list_sprite, list_mesh, args, shape, list_point;
    uint base, capacity, rate_slots, flags;
    float time, dt, rate, stop;   // stop: no birth after it (the emission's end or a deactivation; huge: never)
    float burst_time, burst_interval, burst_count, tl_start;
    float tl_end, life_min, life_max, scale;
    uint seed, shape_kind, shape_tris;
    float point_px;        // sprites up to this many pixels across drawn as points (one vertex: particle_point.vert)
    vec4 shape_p;          // sphere: radius; box: half extents; cone: angle (deg), length; disc: radius; mesh volume: center
    vec4 vel_dir;          // xyz direction (emitter space), w speed (cm/s)
    vec4 vel_p;            // x cone (deg), y speed random (0..1), z outward speed, w normal speed (mesh)
    vec4 size_p;           // x size min, y size max (cm), z rotation random (deg), w rotation rate (deg/s)
    vec4 color0, color1;   // initial color range (linear rgb, alpha)
    vec4 gravity;          // xyz (cm/s^2), w drag (1/s)
    vec4 curl;             // x strength (cm/s^2), y scale (cm), z speed, w brownian (cm/s^2)
    vec4 vortex;           // x strength (deg/s), y pull (cm/s^2), zw unused
    vec4 vortex_axis;      // xyz (emitter space), w unused
    vec4 attract;          // xyz position (emitter space), w strength (cm/s^2)
    vec4 attract2;         // x radius (0: everywhere), y kill radius
    vec4 wind;             // xyz velocity (cm/s), w response (1/s)
    vec4 collide;          // x ground height (world cm), y bounce, z friction, w 1 = die on contact
    vec4 limits;           // x max speed, y min speed
    vec4 killbox_min, killbox_max; // world; killbox_min.w: kill inside (1) or outside (0)
    vec4 col_life[4];      // color over life: 4 stops (rgba)
    vec4 col_life_t;       // their ages (0..1)
    vec4 size_life;        // size factor at 0, 1/3, 2/3, 1 of the life
    vec4 render_p;         // x shape (0 soft, 1 hard circle, 2 square, 3 texture), y softness, z stretch, w emissive
    vec4 render2;          // x mesh pixels under which a mesh particle is a sprite, y alpha, z texture slot + 1, w roughness
    uint mesh_first_index, mesh_index_count, mesh_vertex_offset, mesh_vertices;
    vec4 mesh_color;       // the mesh's color (rgb), w metallic
    vec4 mesh_scale;       // xyz the mesh's scale per cm of size, w unused
    vec4 sub_uv;           // x columns, y rows, z mode (0 over life, 1 frame rate, 2 random), w frame rate (fps)
    vec4 speed_scale;      // x speed min, y speed max (cm/s), z size factor at min, w at max
    vec4 extra;            // x camera offset (cm), y alpha at the min speed, z at the max, w (the CPU's start delay)
    vec4 ribbon;           // (B10) x width scale, y particles a texture repeat, z tail fade
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer EmitterBuf { Emitter e; };

// --- Random ---
uint p_hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
vec4 p_rand4(uint seed, uint slot, uint k) {
    uint h = p_hash(seed ^ p_hash(slot * 0x9E3779B9u ^ p_hash(k + 0x68E31DA4u)));
    uvec4 q = uvec4(h, p_hash(h + 1u), p_hash(h + 2u), p_hash(h + 3u));
    return vec4(q >> 8u) * (1.0 / 16777216.0);
}

// The birth of slot i at the emitter's time: its time b and index k; false when it isn't born yet or won't be
bool p_birth(Emitter e, uint i, out float b, out uint k) {
    float t = e.time;
    b = 0.0, k = 0u;
    if ((e.flags & PF_TL) != 0u) { // the Tlender's: count particles over [start, end], again each cycle when looping
        float span = max(e.tl_end - e.tl_start, 0.0);
        float b0 = e.tl_start + span * float(i) / float(max(e.capacity, 1u));
        if (t < b0) return false;
        if ((e.flags & PF_LOOP) != 0u) {
            float period = span + e.life_max + 1e-3;
            float kk = floor((t - b0) / period);
            b = b0 + kk * period, k = uint(kk);
            return b <= e.stop;
        }
        b = b0;
        return b <= e.stop;
    }
    if (i < e.rate_slots) { // a continuous rate: slot i born at i / rate, then each rate_slots / rate
        if (e.rate <= 0.0) return false;
        float b0 = float(i) / e.rate, period = float(e.rate_slots) / e.rate;
        float last = min(t, e.stop);
        if (last < b0) return false;
        float kk = floor((last - b0) / period);
        b = b0 + kk * period, k = uint(kk);
        return true;
    }
    // bursts: the rest of the slots, all born at each burst
    if (t < e.burst_time || float(i - e.rate_slots) >= e.burst_count) return false;
    float kk = e.burst_interval > 0.0 ? floor((min(t, e.stop) - e.burst_time) / e.burst_interval) : 0.0;
    if (kk < 0.0) return false;
    b = e.burst_time + kk * e.burst_interval, k = uint(kk);
    return b <= e.stop;
}

float p_life(Emitter e, vec4 r) { return mix(e.life_min, e.life_max, r.x); }

// The piecewise curves over the life (age 0..1)
float p_size_factor(Emitter e, float a) {
    if ((e.flags & PF_SIZE_LIFE) == 0u) return 1.0;
    float x = clamp(a, 0.0, 1.0) * 3.0;
    int s = min(int(x), 2);
    // (the stops chosen, not e.size_life[s]: a vec4 indexed by a variable, which Adreno's compiler refuses)
    vec4 L = e.size_life;
    float a0 = s == 0 ? L.x : s == 1 ? L.y : L.z, a1 = s == 0 ? L.y : s == 1 ? L.z : L.w;
    return mix(a0, a1, x - float(s));
}
vec4 p_color(Emitter e, vec4 r, float a) {
    vec4 c = mix(e.color0, e.color1, r.y);
    if ((e.flags & PF_COLOR_LIFE) == 0u) return c;
    a = clamp(a, 0.0, 1.0);
    vec4 t = e.col_life_t;
    vec4 l = a <= t.y ? mix(e.col_life[0], e.col_life[1], clamp((a - t.x) / max(t.y - t.x, 1e-4), 0.0, 1.0))
           : a <= t.z ? mix(e.col_life[1], e.col_life[2], clamp((a - t.y) / max(t.z - t.y, 1e-4), 0.0, 1.0))
                      : mix(e.col_life[2], e.col_life[3], clamp((a - t.z) / max(t.w - t.z, 1e-4), 0.0, 1.0));
    return c * l;
}
float p_size(Emitter e, vec4 r, float a) { return mix(e.size_p.x, e.size_p.y, r.z) * p_size_factor(e, a) * e.scale; }

// Scale by Speed: 0..1 along [min, max] of the speed (the factors: size z..w, alpha y..z of extra)
float p_speed_t(Emitter e, vec3 v) { return clamp((length(v) - e.speed_scale.x) / max(e.speed_scale.y - e.speed_scale.x, 1e-3), 0.0, 1.0); }
float p_speed_size(Emitter e, vec3 v) { return (e.flags & PF_SPEED_SCALE) != 0u ? mix(e.speed_scale.z, e.speed_scale.w, p_speed_t(e, v)) : 1.0; }
float p_speed_alpha(Emitter e, vec3 v) { return (e.flags & PF_SPEED_SCALE) != 0u ? mix(e.extra.y, e.extra.z, p_speed_t(e, v)) : 1.0; }

// Sub UV Animation: the flipbook's frame (fractional: the blend into the next) at an age
float p_subuv_frame(Emitter e, vec4 r, float age, float a) {
    if ((e.flags & PF_SUBUV) == 0u) return 0.0;
    float n = max(e.sub_uv.x * e.sub_uv.y, 1.0);
    uint mode = uint(e.sub_uv.z);
    if (mode == 2u) return floor(r.w * n);                                 // a random cell, kept
    if (mode == 1u) return mod(age * e.sub_uv.w + floor(r.w * n), n);       // at a frame rate (from a random cell)
    return min(clamp(a, 0.0, 1.0) * n, n - 1e-3);                          // over the life, once
}
// A cell's uv (0..1 inside the cell -> the texture's)
vec2 p_subuv_uv(Emitter e, vec2 uv01, float cell) {
    float cols = max(e.sub_uv.x, 1.0), rows = max(e.sub_uv.y, 1.0);
    float c = floor(cell);
    vec2 at = vec2(mod(c, cols), mod(floor(c / cols), rows));
    return (at + uv01) / vec2(cols, rows);
}

vec3 p_to_world(Emitter e, vec3 p) {
    if ((e.flags & PF_LOCAL) == 0u) return p;
    vec4 q = vec4(p, 1.0);
    return vec3(dot(e.rows[0], q), dot(e.rows[1], q), dot(e.rows[2], q));
}
vec3 p_dir_to_world(Emitter e, vec3 d) {
    return vec3(dot(e.rows[0].xyz, d), dot(e.rows[1].xyz, d), dot(e.rows[2].xyz, d));
}
#endif
