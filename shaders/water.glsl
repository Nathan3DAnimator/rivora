// Water (water.c, notas-dev/agua.md): the rain's maps around the camera (the first surface the rain meets, the water on
// it: puddles and streams by a shallow water simulation), the ocean's FFT waves and the ripples of what moves in water.
// Every map is a buffer (FrameBuf.water_addr), read with a manual bilinear: no descriptors.
//
// f.water_addr: 0 the rain's surface, 1 its water, 2 their flow, 3 the ripples, 4 the ocean's spectrum, 5 its FFT,
// 6 its maps, 7 the rain's rings baked (128^2 over 128 cm, tiling)
// f.water[]:
//   0  rain: intensity (mm/h; 0: none), time (s), wind (m/s) x, y
//   1  the rain's grid: cell (cm), N, its first cell (world) x, y (the window [first, first + N) around the camera;
//      a cell lives at index (x mod N, y mod N): the window slides without copying)
//   2  the look: wetness, puddles, streaks, ripples (0..1)
//   3  the drops: visible (0..1), drop size (mm), splashes (0..1), the drops drawn
//   4  the simulation: flow (0..1), water gain, loss (cm/s: infiltration + evaporation), dt (s)
//   5  wet rate (1/s), dry rate (1/s), micro relief (cm), flags (1: the surface by rays; 2: the water anew)
//   6  the surface's rows traced this frame: first, count; the rays' top (cm), their length (cm)
//   7  the ocean: its level (cm), choppiness, foam, on (1 + its shore foam 0..1)
//   8  its deep color (rgb), clarity (cm)
//   9  its scatter (rgb), interaction (0..1)
//   10 its cascades' sides (m) x, y, z; time (s)
//   11 the ripples' grid: cell (cm), N, first cell x, y (as the rain's)
//   12 the ripples: dt (s), damping, wave speed (cm/s), spheres
//   13..28 the spheres pushing the water: center (cm), radius (cm; negative: it moves up, the push turns)
//   29..36 the lakes (up to 4): center x, y, z, half x | half y, wave height, wave number (1/cm), angular speed (1/s)
//   37 lakes count, the ocean's body (-1: none), the swell (0..1), the wind's direction (radians)
//   38 the water a place starts with (cm), the snow's melt (1/s), the snow's cover in the open (0..1), its start (0..1)
//   39 snow: on (1), flake size (mm), the cover's growth (1/s), the steepest surface it covers (its normal's z)
//   40..43 the lakes' deep color (rgb), clarity (cm); 44..47 their scatter (rgb), shore foam (0..1)
//   48 the snow's trail (footprints): its buffer's address (lo, hi: uint bits), its first cell (world) x, y (W_TRAIL_N
//      cells of W_TRAIL_CELL cm around the camera; a cell at (x mod N, y mod N) with its place's key, as the rain's)
//   49 the trail: on (1), the stamps this frame, dt (s), the falling snow filling it back (1/s)
//   50..55 the stamps (a foot each): center x, y (cm), heading (radians), depth (0..1)
// The surface's w: the snow's cover there (0..1)
#ifndef WATER_GLSL_RIVER
#define WATER_GLSL_RIVER
// (B12c) the rivers' samples (water.c, at pc.instances): a river's header (n; color, clarity; scatter, shore foam; -) then its samples
// (center xyz, width; flow dir xy, speed cm/s, depth), RIVER_STRIDE vec4s a river
#define RIVER_STRIDE 516u
#define RIVER_ACROSS 16u
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer RiverBuf { vec4 v[]; };
#endif
#ifndef WATER_GLSL
#define WATER_GLSL
// (wrapping a cell into a window: never %, undefined in GLSL for negative numbers: floor, or & for powers of two)

#define W_OCEAN_N 256
// (a vec4's component by a variable index: Adreno's compiler (S23, driver 0.744) refused the shader; chosen instead)
#define W_CASCADE_SIDE(f, c) ((c) == 0u ? (f).water[10].x : (c) == 1u ? (f).water[10].y : (f).water[10].z)
#define W_RIPPLE_N 256

layout(buffer_reference, scalar, buffer_reference_align = 16) buffer WaterBuf { vec4 v[]; };

// --- The snow's trail (water.c, water_sim.comp pass 14): how deep the feet pressed the cover (1: a foot's full depth;
// below 0: the rim of snow they pushed up). x: that, y: its place's key (uint bits) ---
#define W_TRAIL_N 512
#define W_TRAIL_CELL 4.0
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer TrailBuf { vec2 v[]; };
TrailBuf trail_buf(FrameBuf f) { return TrailBuf(uvec2(floatBitsToUint(f.water[48].x), floatBitsToUint(f.water[48].y))); }
uint trail_key(ivec2 c) { return (uint(c.x) * 73856093u ^ uint(c.y) * 19349663u) | 1u; }
uint trail_index(ivec2 c) { return uint(c.x & (W_TRAIL_N - 1)) + uint(c.y & (W_TRAIL_N - 1)) * uint(W_TRAIL_N); }
float trail_at(FrameBuf f, ivec2 c) {
    ivec2 l = c - ivec2(f.water[48].zw);
    if (l.x < 0 || l.y < 0 || l.x >= W_TRAIL_N || l.y >= W_TRAIL_N) return 0.0;
    vec2 v = trail_buf(f).v[trail_index(c)];
    return floatBitsToUint(v.y) == trail_key(c) ? v.x : 0.0;
}

// --- The rain's grid ---
bool rain_on(FrameBuf f) { return (f.water_addr[0].x | f.water_addr[0].y) != 0u; } // (raining, snowing, or a cover melting)
// the cell under a world point (its world cell), and whether the window holds it
ivec2 rain_cell(FrameBuf f, vec2 p) { return ivec2(floor(p / f.water[1].x)); }
// (2026-11-10, "chuva com ângulo molha o que está coberto") the rain's slant: how far it moves sideways per cm it falls
// (the wind over the fall speed: 6 m/s a drop, 0.8 m/s a flake; at most 1.5). The grid lies along it: a point's place in
// it is xy + slant * z (water.c water_rain_slant: the same), so a line of rain keeps one cell and the first surface a
// cell holds is the one the slanted rain meets (wind blows it under the eaves; the lee side of a wall stays dry)
vec2 rain_slant(FrameBuf f) {
    vec2 w = f.water[0].zw * 100.0 / ((f.water[39].x > 0.5 ? 80.0 : 600.0) * max(f.water[56].z, 0.25)); // (Fall Speed: faster drops tilt less)
    float l = length(w);
    // (2026-11-15, "vários locais que deveriam ficar molhados por serem abertos não ficam") at most 0.7 (35 degrees): a
    // storm's wind slanted it to 1.5 (56) and every wall left a dry shadow longer than itself; the gusts, the splashes and
    // the water running in wet the lee of a wall too
    return l > 0.7 ? w * (0.7 / l) : w;
}
vec2 rain_u(FrameBuf f, vec3 p) { return p.xy + rain_slant(f) * p.z; }
bool rain_holds(FrameBuf f, ivec2 c) {
    ivec2 o = ivec2(f.water[1].zw), d = c - o;
    int n = int(f.water[1].y);
    return d.x >= 1 && d.y >= 1 && d.x < n - 1 && d.y < n - 1;
}
uint rain_index(FrameBuf f, ivec2 c) {
    int n = int(f.water[1].y);
    ivec2 m = c - n * ivec2(floor(vec2(c) / float(n))); // (GLSL's % is undefined for negative numbers)
    return uint(m.y * n + m.x);
}
// The surface (x: height cm, y: the cell's key, z: 1 set, w: -) and the water on it (x: depth cm, y: wetness 0..1,
// zw: the flow's speed cm/s) at a cell
// (Q27.6) the persistent wet map (water.c persist, water_sim.comp pass 15): 16 m cells on a toroidal window of
// water[53].y around the camera (water[52].zw its first cell, .xy its address), each its water (cm), wetness and key; the
// storms (water[13 + i]: x, y, radius, rate now; water[54].x of them, .y the field on)
WaterBuf persist_buf(FrameBuf f) { return WaterBuf(uvec2(floatBitsToUint(f.water[52].x), floatBitsToUint(f.water[52].y))); }
uint persist_key(ivec2 c) { return uint(c.x & 0x7FFF) | (uint(c.y & 0x7FFF) << 15u) | 0x80000000u; }
uint persist_index(FrameBuf f, ivec2 c) {
    int N = int(f.water[53].y);
    return uint(((c.y % N) + N) % N) * uint(N) + uint(((c.x % N) + N) % N);
}
bool persist_at(FrameBuf f, vec2 u, out vec4 v) {
    v = vec4(0.0);
    if (f.water[53].w < 0.5) return false;
    ivec2 c = ivec2(floor(u / f.water[53].x)), d = c - ivec2(f.water[52].zw);
    int N = int(f.water[53].y);
    if (d.x < 0 || d.y < 0 || d.x >= N || d.y >= N) return false;
    v = persist_buf(f).v[persist_index(f, c)];
    return floatBitsToUint(v.w) == persist_key(c);
}
float storm_rain(FrameBuf f, vec2 p) { // mm/h (weather_field.c wfield_sample: the same sum)
    float r = 0.0;
    int n = min(int(f.water[54].x), 8);
    for (int i = 0; i < n; i++) {
        vec4 s = f.water[13 + i];
        vec2 d = p - s.xy;
        r += s.w * exp(-dot(d, d) / (s.z * s.z));
    }
    return r < 0.05 ? 0.0 : r;
}
vec4 rain_surface(FrameBuf f, ivec2 c) { return WaterBuf(f.water_addr[0]).v[rain_index(f, c)]; }
// (2026-11-10, "as coisas que se movem deixam áreas secas") the surface above is the STATIC scene's (x: what wets, where
// water gathers): a walker no longer dries the floor it crosses. What moves over it is apart, in the buffer's second
// half: its speed (xyz, cm/s: the actor's and a skin's own motion) and its top (w; -1e9: nothing), where the drops stop
// and the splashes fly off it
vec4 rain_mover(FrameBuf f, ivec2 c) {
    uint n = uint(f.water[1].y);
    return WaterBuf(f.water_addr[0]).v[rain_index(f, c) + n * n];
}
float rain_top(FrameBuf f, ivec2 c) { return max(rain_surface(f, c).x, rain_mover(f, c).w); } // (the first thing the rain meets)
vec4 rain_water(FrameBuf f, ivec2 c) { return WaterBuf(f.water_addr[1]).v[rain_index(f, c)]; }
// Bilinear over the cells' centers
vec4 rain_water_at(FrameBuf f, vec2 p, out float h) {
    vec2 g = p / f.water[1].x - 0.5;
    ivec2 c = ivec2(floor(g));
    vec2 t = g - vec2(c);
    vec4 a = rain_water(f, c), b = rain_water(f, c + ivec2(1, 0)), cc = rain_water(f, c + ivec2(0, 1)), d = rain_water(f, c + ivec2(1, 1));
    float ha = rain_surface(f, c).x, hb = rain_surface(f, c + ivec2(1, 0)).x, hc = rain_surface(f, c + ivec2(0, 1)).x, hd = rain_surface(f, c + ivec2(1, 1)).x;
    h = mix(mix(ha, hb, t.x), mix(hc, hd, t.x), t.y);
    return mix(mix(a, b, t.x), mix(cc, d, t.x), t.y);
}
float rain_height_at(FrameBuf f, vec2 p) { return rain_surface(f, rain_cell(f, p)).x; }
// (2026-11-15) the water leaving cell c toward side s (one of the 4), over the rain one cell catches: how many cells of
// roof drain over that edge (a wall's thin top or a step: under 1; an eave: dozens)
float rain_outflow(FrameBuf f, ivec2 c, ivec2 s) {
    vec4 o = WaterBuf(f.water_addr[2]).v[rain_index(f, c)];
    float q = s.x < 0 ? o.x : s.x > 0 ? o.y : s.y < 0 ? o.z : o.w;
    float cell = f.water[1].x, rin = max(f.water[0].x, 0.5) * (0.1 / 3600.0) * max(f.water[4].y, 1.0) * cell * cell;
    return q / max(rin, 1e-6);
}

// --- Small hashes ---
float w_hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec2 w_hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// The rings of drops hitting water (a puddle, a lake): the slope (d height / d x, y) at p (cm), time t (s), the rain rate
// (mm/h). Tiles every W_RIP_TILE cm: water_sim.comp pass 4 bakes it a frame into a 256^2 map (water_addr[7], 1 cm a
// texel) the surfaces read (rain_ripple_map): the rings' math stays out of mesh.frag.
// (2026-11-16, "pesquise como realmente as chuvas funcionam") as many as hit: the drops of 1 mm and up of the Marshall-
// Palmer spectrum land ~1300 times a square meter a second at 10 mm/h, ~7700 at 100 (here 1500 (R/10)^0.75); each ring a
// short wave packet (~2 cm long: gravity-capillary waves near their slowest group speed, ~18 cm/s) gone in under half a
// second: in a downpour the puddle "boils" with small rings. Before: ~45 rings a square meter a second, 20 cm wide
#define W_RIP_TILE 128.0
#define W_RIP_N 128
vec2 rain_ripples(vec2 p, float t, float intensity) {
    vec2 s = vec2(0.0);
    const float CELL = 8.0, LIFE = 0.45; // cm (the tile's 16th), s
    float flux = 1500.0 * pow(max(intensity, 0.1) / 10.0, 0.75);           // rings a square meter a second
    float period = max(2.0 / max(flux * CELL * CELL * 1e-4, 1e-3), LIFE);    // (2 layers: each cell's ring every period)
    for (int layer = 0; layer < 2; layer++) {
        vec2 off = vec2(0.37, 0.61) * float(layer);
        vec2 g = p / CELL + off;
        ivec2 c0 = ivec2(floor(g));
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                ivec2 ci = c0 + ivec2(i, j);
                vec2 cw = vec2(ci) - 16.0 * floor(vec2(ci) / 16.0); // (its hash wraps with the tile)
                vec2 h = w_hash22(cw + float(layer) * 37.1);
                float age = mod(t + h.y * period * 7.0, period);
                if (age > LIFE) continue;
                vec2 at = (vec2(ci) + 0.15 + 0.7 * w_hash22(cw * 1.7 + 17.0 + float(layer) * 5.3) - off) * CELL;
                vec2 d = p - at;
                float r = length(d), k = age / LIFE;
                float x = (r - (0.6 + age * 18.0)) / 1.1;                       // (the packet travels ~18 cm/s)
                float env = (1.0 - k) * (1.0 - k) * exp(-x * x * 0.35) * (0.4 + 0.9 * h.x); // (bigger drops, taller rings)
                float dw = -2.0 * x * env * 0.06 * cos(x * 1.9); // (~3 cm a wave: 3 texels of the map)
                if (r > 1e-3) s += d / r * dw;
            }
    }
    return s;
}
// The baked rings at p (cm): bilinear over the 1 cm texels
vec2 rain_ripple_map(FrameBuf f, vec2 p) {
    vec2 g = p / W_RIP_TILE * float(W_RIP_N) - 0.5;
    ivec2 i = ivec2(floor(g));
    vec2 t = g - vec2(i);
    WaterBuf rb = WaterBuf(f.water_addr[7]);
    ivec2 a = i & (W_RIP_N - 1), b = (i + 1) & (W_RIP_N - 1);
    vec2 v00 = rb.v[a.y * W_RIP_N + a.x].xy, v10 = rb.v[a.y * W_RIP_N + b.x].xy, v01 = rb.v[b.y * W_RIP_N + a.x].xy, v11 = rb.v[b.y * W_RIP_N + b.x].xy;
    return mix(mix(v00, v10, t.x), mix(v01, v11, t.x), t.y);
}

// The rain on a surface (mesh.frag): darker and glossier where it is wet, a film of water in puddles (mirror, ripples)
// and streams (the flow's waves), streaks down the walls. p: the point, n: its shading normal (changed in puddles),
// ng: the geometric normal. Returns how much is open water (0..1: its specular is water's)
float w_vnoise(vec2 p) { // smooth value noise (0..1)
    vec2 i = floor(p), t = p - i;
    t = t * t * (3.0 - 2.0 * t);
    return mix(mix(w_hash12(i), w_hash12(i + vec2(1, 0)), t.x), mix(w_hash12(i + vec2(0, 1)), w_hash12(i + vec2(1, 1)), t.x), t.y);
}

// The snow's cover on a surface (amount: the cell's cover times how exposed it is): white, matte, soft drifts (two
// octaves of smooth noise: thinner patches while it gathers, a little shade)
void snow_shade(FrameBuf f, vec3 p, vec3 ng, float amount, inout vec3 n, inout vec3 base, inout float rough, inout float metal) {
    float cos_max = f.water[39].w;
    float face = smoothstep(cos_max, mix(cos_max, 1.0, 0.35), ng.z); // (what faces up enough; thicker where flatter)
    if (face * amount <= 0.0) return;
    vec2 q = p.xy / 45.0;
    float drift = w_vnoise(q) * 0.65 + w_vnoise(q * 3.7 + 17.0) * 0.35;
    float cover = clamp(amount * 1.3 - drift * 0.3, 0.0, 1.0) * face;
    if (cover <= 0.0) return;
    vec3 snow = vec3(0.86, 0.88, 0.92) * (0.9 + 0.1 * drift);
    vec3 sn = vec3(0.0, 0.0, 1.0); // (the cover hides the relief under it; bumped normals drew dark streaks)
    if (f.water[49].x > 0.5 && face > 0.5) { // the feet's prints: the pressed depth (bilinear) and its slope as relief
        vec2 g = p.xy / W_TRAIL_CELL - 0.5;
        ivec2 c0 = ivec2(floor(g));
        vec2 t = g - vec2(c0);
        float d00 = trail_at(f, c0), d10 = trail_at(f, c0 + ivec2(1, 0)), d01 = trail_at(f, c0 + ivec2(0, 1)), d11 = trail_at(f, c0 + ivec2(1, 1));
        float d = mix(mix(d00, d10, t.x), mix(d01, d11, t.x), t.y);
        if (d00 != 0.0 || d10 != 0.0 || d01 != 0.0 || d11 != 0.0) {
            vec2 gr = vec2(mix(d10 - d00, d11 - d01, t.y), mix(d01 - d00, d11 - d10, t.x)) / W_TRAIL_CELL;
            float depth = 7.0 * cover; // (cm: how deep a foot goes into this cover)
            sn = normalize(vec3(gr * depth, 1.0));
            float pressed = clamp(d, 0.0, 1.0) * cover;
            snow *= 1.0 - 0.22 * pressed; // (packed snow: greyer, and the print's walls shade its floor)
            rough = mix(rough, 0.45, pressed);
        }
    }
    base = mix(base, snow, cover);
    rough = mix(rough, 0.6, cover);
    metal = mix(metal, 0.0, cover);
    n = normalize(mix(n, sn, cover));
}

// (2026-11-10, "quadrados onde devia estar molhado", "suavizar a borda") how open to the rain a point is: the covers of
// the 4 x 4 cells around it weighed by a cubic B-spline (C2: no squares where a roof's edge crosses the grid, no step):
// a soft edge about two cells wide, as the splashes and the drips wet a little under the edge of a roof
vec4 w_bspline(float t) {
    float t2 = t * t, t3 = t2 * t, u = 1.0 - t;
    return vec4(u * u * u, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0;
}
float rain_exposure(FrameBuf f, vec2 q, float z) {
    vec2 g = q / f.water[1].x - 0.5;
    ivec2 c0 = ivec2(floor(g)) - 1;
    vec2 t = g - floor(g);
    vec4 wx = w_bspline(t.x), wy = w_bspline(t.y);
    float e = 0.0;
    for (int j = 0; j < 4; j++) {
        float row = 0.0;
        for (int i = 0; i < 4; i++) row += wx[i] * smoothstep(-25.0, -6.0, z - rain_surface(f, c0 + ivec2(i, j)).x);
        e += wy[j] * row;
    }
    return clamp(e, 0.0, 1.0);
}

// (2026-11-14, "objetos molhados secam muito rápido ao sair da chuva, e molham também muito rápido") what moves keeps its
// own wetness (water.c wet_update: a byte per entity index, 0 not followed): -1 when the entity isn't followed
float rain_own_wet(FrameBuf f, uint entity) {
    if (f.water[80].z < 0.5) return -1.0;
    uint i = entity & 0xFFFFFu;
    uint v = (UintBuf(uvec2(floatBitsToUint(f.water[80].x), floatBitsToUint(f.water[80].y))).v[i >> 2u] >> ((i & 3u) * 8u)) & 255u;
    return v == 0u ? -1.0 : float(v - 1u) / 254.0;
}
// ===== The rain's look (2026-11-15, "o shading não tá realista, quero algo realmente realista, tanto em tempestades
// quanto nas gotas, poças, agua escorrendo, splashes e goteiras"; the Tlender's kernel_rain.h as the reference) =====
// What is wet is two layers, as the Tlender's "wet every material": the material itself darker and more saturated by
// how porous it is (Lagarde: the water fills the pores, the light scatters longer inside) and a film of water over it
// with its own normal and roughness: the coat mesh.frag lights (brdf, its reflection). The ground keeps its texture
// and bumps under a sharp mirror; a puddle is the film thick (ripples, the flow), the ground seen through it darker,
// tinted and smoothed. On top: the splashes of the drops hitting the surfaces (crowns and wet dots), the rivulets
// running down the walls, the drops on walls and windows (staying, and running down leaving a trail). Every knob is
// the Rain's (water[86..89]); the Stylized style changes the forms (hard puddle edges, flat tones).
// (packed: 5 floats live through every light took mesh.frag from 96 to 128 registers; two words keep it: x the normal
// octahedral in two snorm16, y the weight and the GGX alpha (roughness squared) as halfs)
uvec2 g_rcp = uvec2(0u);
// (2026-11-16, "a tela fica piscando quando algum personagem tá numa área secando") a mix of two normals, never zero: a
// two-sided part seen from behind (the Delight's skirt) has its shading normal opposite the geometric one, the half-way mix
// was zero and normalize gave NaN, which the TSR, the bloom and the exposure spread over the whole screen
vec3 w_nmix(vec3 a, vec3 b, float k) {
    vec3 m = mix(a, b, k);
    float l = dot(m, m);
    return l > 1e-8 ? m * inversesqrt(l) : b;
}
// (2026-11-15, "também tá flickando a tela") the film's steady part: the wet look and the puddles, without the splashes'
// crowns and the drops that come and go every frame. mesh.frag picks the reflection's path by it (the ray, the screen's,
// the denoiser's): picked by the whole weight, a pixel switched path each time a crown crossed the threshold
float g_rcs = 0.0;
vec2 w_oct(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}
vec3 w_unoct(vec2 o) {
    vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
float rc_w() { return unpackHalf2x16(g_rcp.y).x; }
float rc_a() { return unpackHalf2x16(g_rcp.y).y; }
vec3 rc_n() { return w_unoct(unpackSnorm2x16(g_rcp.x)); }
void rc_set(float w, vec3 n, float a) { g_rcp = uvec2(packSnorm2x16(w_oct(n)), packHalf2x16(vec2(w, a))); }
bool rain_stylized(FrameBuf f) { return f.water[86].w > 1.5; }

uint w_hash3u(uvec3 v) { // (pcg3d, Jarzynski and Olano 2020)
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z, v.y += v.z * v.x, v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z, v.y += v.z * v.x, v.z += v.x * v.y;
    return v.x ^ v.y ^ v.z;
}
uint w_next(uint h) { h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; return h ^ (h >> 16); }
float w_u01(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }
uint w_cellhash(float x, float y, uint seed) { return w_hash3u(uvec3(uint(int(x) + 32768), uint(int(y) + 32768), seed)); }

// How much a material drinks the water: rough, non-metal surfaces (concrete, asphalt, wood, cloth) are porous; polished
// ones (tiles, paint, plastic, metal) keep the water on top as a film
float rain_porosity(float rough, float metal) { return clamp((rough - 0.2) / 0.55, 0.0, 1.0) * (1.0 - metal); }
void rain_wet_base(FrameBuf f, float wet, inout vec3 base, inout float rough, float metal) {
    if (wet <= 0.0) return;
    float por = rain_porosity(rough, metal);
    // (2026-11-15, "o modelo ficou muito preto") as the Tlender's: up to ~0.6 at physical (pow 1.9, x 0.8); was 1.0
    float k = clamp(wet * mix(0.25, 0.6, por) * f.water[86].x, 0.0, 1.2);
    if (rain_stylized(f)) base *= 1.0 - 0.35 * min(k, 1.5); // (one flat darker tone)
    else base = pow(clamp(base, 0.0, 1.0), vec3(1.0 + 1.5 * k)) * (1.0 - 0.35 * min(k, 1.5)); // (darker and more saturated)
    // (its own glossy lobe sharpens as the water fills the pores, as the Tlender's wet lobes: the bumps glint; the film
    // over it is the mirror)
    rough = mix(rough, min(rough, mix(0.12, 0.3, por)), min(wet, 1.0) * 0.9);
}
void rain_set_coat(FrameBuf f, float w, vec3 cn, float crough) {
    g_rcs = clamp(w * f.water[86].y, 0.0, 1.0);
    rc_set(clamp(w * f.water[86].y, 0.0, 1.0), cn, max(crough * crough, 2e-5));
}

// The splashes of the drops hitting a surface (the Tlender's rain_impacts_on): the drops come down along dir, so where
// they hit is a lattice of the plane across the rain projected onto the surface (fewer on a wall it grazes); each a
// crown spreading for a tenth of a second, round where the drop falls straight onto it, stretched and thrown forward
// where it comes slanted (a roof, a wall in the wind), leaving a wet dot that fades in about a second. P in m; the
// crowns' height gradient along the surface (xyz, per m) and the dots (w)
vec4 rain_impacts_on(vec3 P, vec3 N, vec3 dir, float t, float rate, float size) {
    vec4 o = vec4(0.0);
    float cn = dot(dir, N);
    if (cn > -0.05) return o; // (the rain doesn't come onto this side)
    vec3 e1 = normalize(cross(dir, abs(dir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 e2 = cross(dir, e1);
    vec3 tdir = dir - N * cn;
    float slant = length(tdir);
    vec3 fw = slant > 1e-3 ? tdir / slant : e1;
    vec3 side = cross(N, fw);
    float scale = 11.0 / max(size, 0.05);
    float qa = dot(P, e1) * scale, qb = dot(P, e2) * scale;
    for (int layer = 0; layer < 2; layer++) {
        float ua = qa + 0.5 * float(layer), ub = qb + 0.31 * float(layer);
        float cx = floor(ua), cy = floor(ub);
        uint h = w_cellhash(cx, cy, uint(layer + 41));
        float r0 = w_u01(h);
        h = w_next(h);
        float r1 = w_u01(h);
        h = w_next(h);
        float r2 = w_u01(h);
        float period = (0.35 + 0.9 * r2) / max(rate * max(-cn, 0.1), 0.05);
        float tt = fract(t / period + r1) * period;
        vec3 o3 = e1 * ((cx + 0.3 + 0.4 * r0 - ua) / scale) + e2 * ((cy + 0.3 + 0.4 * r1 - ub) / scale);
        vec3 u = o3 - dir * (dot(o3, N) / cn); // (the impact - P, along the surface)
        float fa = -dot(u, fw) * scale, fc = -dot(u, side) * scale;
        float stretch = 1.0 + 1.8 * slant;
        float ax = (fa - tt * 1.6 * slant) / stretch; // (thrown forward as it spreads)
        float d = sqrt(ax * ax + fc * fc) + 1e-5;
        float back = fa < 0.0 ? 1.0 - 0.75 * slant : 1.0; // (little goes back up the slope)
        o.w = max(o.w, (1.0 - smoothstep(0.06, 0.1 + 0.05 * r2, d)) * exp(-tt * 1.5));
        float x = (d - tt * 2.2) * 18.0;
        float g = exp(-x * x) * exp(-tt * 14.0) * (1.0 - smoothstep(0.15, 0.25, d)) * back;
        o.xyz += (fw * (ax / (d * stretch)) + side * (fc / d)) * (-36.0 * x * g * scale);
    }
    return o;
}

// --- Drops on walls, windows and wet actors (after the Tlender's rain_drops) ---
// (2026-11-15) lean: the Tlender's port (two layers of staying drops, a noise path, the beads left along a trail, a struct
// per cap) took mesh.frag from 96 to 128 registers and spilled the ray traced variant; one layer, a path of sines and the
// best cap kept as one vec4 cost a fraction of it and look the same at a few pixels a drop.
// A drop that stays, in the cell grid of uv (cells): landing and gone after a while. Its surface's slope (xy, outward
// from its center; the contact angle of water on glass, the rim ~60 degrees), height (z) and coverage (w)
vec4 rain_bead(vec2 uv, float t, float density, uint seed) {
    vec2 cu = floor(uv);
    uint h = w_cellhash(cu.x, cu.y, seed);
    if (w_u01(h) >= density) return vec4(0.0);
    h = w_next(h);
    float r1 = w_u01(h);
    h = w_next(h);
    float r2 = w_u01(h);
    h = w_next(h);
    float r3 = w_u01(h);
    h = w_next(h);
    float ph = fract(t / (6.0 + 18.0 * w_u01(h)) + r3);
    float amp = smoothstep(0.0, 0.03, ph) * (1.0 - smoothstep(0.88, 1.0, ph));
    float rad = 0.12 + 0.26 * r2 * r2; // (mostly small, a few big; inside the cell)
    vec2 d = (uv - cu - (rad + (1.0 - 2.0 * rad) * vec2(r1, r3))) / rad;
    float q2 = dot(d, d);
    if (q2 >= 1.0) return vec4(0.0);
    float sq = sqrt(1.0 - q2);
    return vec4(-0.6 * d / max(sq, 0.2), sq, clamp((1.0 - q2) * 10.0, 0.0, 1.0)) * amp;
}
// The drops at P (m) on a surface of normal N with the world's down in that space: size the cells' (m); density of the
// ones that stay, running of the ones that run down (columns of 3.4 cells, a drop at a time, holding and slipping by a
// wandering path, leaving a trail that thins, wiping the drops it runs over). Nothing runs on a flat surface. Returns the
// drops' coverage (x) and the trails (y); Nd the normal through the water
vec2 rain_drops_d(vec3 P, vec3 N, vec3 down, float t, float size, float density, float running, float trail, out vec3 Nd) {
    vec3 g = down - N * dot(down, N);
    float gl = length(g);
    vec3 gd = gl > 1e-3 ? g / gl : normalize(cross(N, abs(N.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 across = normalize(cross(N, gd));
    float u = dot(P, across) / size, v = dot(P, gd) / size;
    vec4 best = rain_bead(vec2(u, v), t, density, 1013u);
    float tr = 0.0, run = running * clamp((gl - 0.25) / 0.35, 0.0, 1.0);
    if (run > 0.0) {
        const float CW = 3.4, CH = 16.0;
        float col = floor(u / CW), seg = floor(v / CH);
        uint hc = w_cellhash(col, seg, 3307u);
        if (w_u01(hc) < run) {
            hc = w_next(hc);
            float a1 = w_u01(hc);
            hc = w_next(hc);
            float period = 1.4 * CH / max((0.6 + 0.8 * a1) * 0.05 / size, 1e-3), ph = fract(t / period + w_u01(hc)), pp = ph * 1.4;
            float left = pp < 1.0 ? 1.0 : 1.0 - clamp((pp - 1.0) / 0.4, 0.0, 1.0); // (its trail dries once it is gone)
            float alive = pp < 1.0 ? smoothstep(0.0, 0.06, pp) * (1.0 - smoothstep(0.94, 1.0, pp)) : 0.0;
            pp = min(pp, 1.0);
            float vd = (seg + 0.05 + 0.9 * (pp - 0.9 * sin(pp * 18.849556) / 18.849556)) * CH; // (holding, then slipping)
            float r = 0.38 + 0.18 * a1, wc = (col + 0.5) * CW;
            if (v < vd && v > seg * CH) { // the trail above it, along the way it came
                float age = (vd - v) / CH, dist = abs(u - wc - sin(v * 0.37 + col * 2.1) * 0.6 - sin(v * 1.3 + col) * 0.15), tw = r * (0.3 - 0.14 * age);
                tr = (1.0 - smoothstep(tw * 0.5, tw, dist)) * trail * (1.0 - 0.7 * age) * left;
                best *= 1.0 - (1.0 - smoothstep(r * 0.8, r * 1.15, dist)) * left; // (it wiped the drops it ran over)
            }
            vec2 d = vec2(u - wc - sin(vd * 0.37 + col * 2.1) * 0.6 - sin(vd * 1.3 + col) * 0.15, v - vd);
            d.y /= d.y < 0.0 ? 1.35 : 0.85; // (a tail above it, a little flattened below)
            d /= r;
            float q2 = dot(d, d);
            if (q2 < 1.0 && alive > 0.0 && sqrt(1.0 - q2) * alive > best.z) {
                float sq = sqrt(1.0 - q2);
                best = vec4(-0.6 * d / max(sq, 0.2), sq, clamp((1.0 - q2) * 10.0, 0.0, 1.0)) * alive;
            }
        }
    }
    Nd = normalize(N - (across * best.x + gd * best.y));
    return vec2(clamp(best.w, 0.0, 1.0), clamp(tr, 0.0, 1.0));
}
vec2 rain_drops(vec3 P, vec3 N, float t, float size, float density, float running, float trail, out vec3 Nd) {
    return rain_drops_d(P, N, vec3(0.0, 0.0, -1.0), t, size, density, running, trail, Nd);
}

// (2026-11-15, "quando algo molhado entrar em um lugar seco molhar o chão") the wet marks on a floor: the prints of wet
// feet and the drips of wet actors where the rain doesn't reach (water_sim.comp pass_trail in the rain, drying), 0..1
float rain_marks(FrameBuf f, vec3 p, vec3 ng) {
    if (f.water[49].x < 1.5 || ng.z < 0.6 || any(greaterThan(abs(p.xy - f.camera_pos.xy), vec2(1000.0)))) return 0.0; // (the map: 20 m around the camera)
    vec2 g = p.xy / W_TRAIL_CELL - 0.5;
    ivec2 c0 = ivec2(floor(g));
    vec2 t = g - vec2(c0);
    // (the 4 cm cells read bilinear drew blocks: a quick look at the 4 nearest, then a cubic B-spline over 4 x 4 and a
    // soft threshold with a little noise: a print's outline, organic)
    if (trail_at(f, ivec2(floor(g + 0.5))) <= 0.0) return 0.0; // (one read where nothing is marked)
    vec4 wx = w_bspline(t.x), wy = w_bspline(t.y);
    float d = 0.0;
    for (int j = 0; j < 4; j++) {
        float row = 0.0;
        for (int i = 0; i < 4; i++) row += wx[i] * max(trail_at(f, c0 + ivec2(i - 1, j - 1)), 0.0);
        d += wy[j] * row;
    }
    d *= 0.8 + 0.4 * w_vnoise(p.xy / 2.5);
    return smoothstep(0.08, 0.45, d) * clamp(d * 2.0, 0.0, 1.0);
}

// (2026-11-15) drops on a wet actor: beads staying on it in the part's own (rotation-only) space, so they ride on it as it
// moves. Laid out on the two axes across the part's dominant normal (triplanar: a wall's "across the down" is constant
// around a cylinder and drew bands); back turns a normal of that space into the world's
void rain_actor_drops(FrameBuf f, float own, vec3 P, vec3 N, vec3 down, mat3 back, float dcam, inout vec3 base) {
    float wet = clamp(own * f.water[2].x, 0.0, 1.0);
    if (wet < 0.1 || f.water[88].z <= 0.0 || dcam > 1500.0) return;
    float k = f.water[88].z * (1.0 - smoothstep(900.0, 1500.0, dcam));
    vec3 an = abs(N), t1, t2;
    if (an.x > an.y && an.x > an.z) t1 = vec3(0.0, 1.0, 0.0), t2 = vec3(0.0, 0.0, 1.0);
    else if (an.y > an.z) t1 = vec3(1.0, 0.0, 0.0), t2 = vec3(0.0, 0.0, 1.0);
    else t1 = vec3(1.0, 0.0, 0.0), t2 = vec3(0.0, 1.0, 0.0);
    float size = f.water[88].w * 0.012, dens = clamp(0.45 * wet * k, 0.0, 0.9);
    float u = dot(P, t1) / size, v = dot(P, t2) / size, t = f.water[0].y;
    vec4 c = rain_bead(vec2(u, v), t, dens, 1013u);
    float on = min(c.w * k, 1.0);
    if (on <= 0.0) return;
    vec3 a1 = normalize(t1 - N * dot(t1, N) + vec3(1e-5)), a2 = normalize(t2 - N * dot(t2, N) + vec3(1e-5));
    vec3 nd = normalize(N - (a1 * c.x + a2 * c.y));
    rc_set(max(rc_w(), clamp(0.95 * on * f.water[86].y, 0.0, 1.0)), w_nmix(rc_n(), normalize(back * nd), on),
           mix(rc_a(), max(f.water[86].z * f.water[86].z * 0.25, 2e-5), on));
    base *= 1.0 - 0.55 * on * smoothstep(0.03, 0.35, 1.0 - dot(nd, N)); // (its rim bends the light away: a dark ring, a clear middle)
}

// (2026-11-16, "um sistema de shading molhado como se fosse suor, ou mesmo molhado") a material's own wetness: the Wet
// Surface node (MATX_WET, mat_common.glsl e8-e11), with or without rain. The same two layers as the rain's (the base
// darker where porous, its own lobe sharper; a water film over it), drops that stay and run down in the part's own space
// (they ride on it), sweat (a thin oily sheen: little darkening, a sharper lobe, small beads). The values the node marks
// as parameters read from the material's parameters (a Blueprint changes them at run time, no bake)
#ifdef MAT_COMMON_GLSL // (the materials table: mesh.frag)
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WetParamBuf { vec4 p[]; };
float w_vnoise(vec3 p) {
    vec3 i = floor(p), u = p - i;
    u = u * u * (3.0 - 2.0 * u);
    float r = 0.0;
    for (int k = 0; k < 8; k++) {
        vec3 o = vec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        uvec3 c = uvec3(ivec3(i + o) + 32768);
        r += w_u01(w_hash3u(c)) * mix(1.0 - u.x, u.x, o.x) * mix(1.0 - u.y, u.y, o.y) * mix(1.0 - u.z, u.z, o.z);
    }
    return r;
}
void mat_wet(FrameBuf f, uint slot, uint param_off, vec3 ng, vec3 P, mat3 R, vec4 vcol, inout vec3 n, inout vec3 base,
             inout float rough, float metal) {
    GpuMatExt X = GpuMatExtBuf(f.materials).x[slot];
    vec4 e8 = X.e8, e9 = X.e9;
    float wet = e8.x, film = e8.z, drops = e9.x, sweat = e9.w;
    uvec4 pi = uvec4(X.e10 + 0.5);
    if (pi.x != 0u) wet = WetParamBuf(f.mat_params).p[param_off + pi.x - 1u].x;
    if (pi.y != 0u) film = WetParamBuf(f.mat_params).p[param_off + pi.y - 1u].x;
    if (pi.z != 0u) drops = WetParamBuf(f.mat_params).p[param_off + pi.z - 1u].x;
    if (pi.w != 0u) sweat = WetParamBuf(f.mat_params).p[param_off + pi.w - 1u].x;
    wet = clamp(wet, 0.0, 1.0), film = clamp(film, 0.0, 1.0), drops = clamp(drops, 0.0, 1.0), sweat = clamp(sweat, 0.0, 1.0);
    mat3 Rt = transpose(R);
    vec3 pl = Rt * P; // (the part's own space, cm: the drops and the patches ride on it)
    uint mm = uint(X.e11.x + 0.5);
    float mask = 1.0;
    if (mm == 1u) mask = clamp(vcol.r, 0.0, 1.0);                 // (painted: the vertex color's red)
    else if (mm == 2u) mask = smoothstep(-0.2, 0.6, ng.z);        // (where the water lands)
    else if (mm == 3u) mask = smoothstep(0.35, 0.65, w_vnoise(pl * 0.06) * 0.7 + w_vnoise(pl * 0.23) * 0.3); // (patches)
    wet *= mask;
    if (wet <= 0.002) return;
    // the material wet: darker and more saturated as the pores fill (sweat soaks in little), its own lobe sharper
    float por = e8.y >= 0.0 ? clamp(e8.y, 0.0, 1.0) : rain_porosity(rough, metal);
    float k = clamp(wet * mix(0.25, 0.6, por) * (1.0 - 0.75 * sweat), 0.0, 1.2);
    base = pow(clamp(base, 0.0, 1.0), vec3(1.0 + 1.5 * k)) * (1.0 - 0.35 * min(k, 1.5));
    rough = mix(rough, min(rough, mix(0.12, 0.3, por)), wet * 0.9);
    rough = mix(rough, min(rough, 0.2), sweat * wet); // (skin's oily sheen)
    // the film over it: the water evens out the bumps (its normal between the surface's and the face's)
    float fw = film * smoothstep(0.15, 0.8, wet);
    vec3 cn = w_nmix(n, ng, 0.5 * fw);
    float fa = max(e8.w, 2e-5);
    // the drops (sweat's smaller): staying and running down the world's down, in the part's space
    float dd = drops * wet;
    if (dd > 0.01) {
        // (the drops laid on the plane of the normal's dominant axis in the part's space, as the rain's on actors: a
        // frame by the pixel's own normal turns with it and, on a curved part, the cells swam into noise)
        vec3 Nl = Rt * ng, an = abs(Nl), Na, Nd;
        Na = an.x > an.y && an.x > an.z ? vec3(sign(Nl.x), 0.0, 0.0) : an.y > an.z ? vec3(0.0, sign(Nl.y), 0.0) : vec3(0.0, 0.0, sign(Nl.z));
        float size = e9.y * 0.002; // (a cell twice a drop's size, m)
        vec2 dr = rain_drops_d(pl * 0.01, Na, Rt * vec3(0.0, 0.0, -1.0), f.camera_pos.w, size, clamp(dd, 0.0, 0.9), e9.z, 0.7, Nd);
        if (dr.x > 0.0) {
            vec3 nd = normalize(Nl + (Nd - Na)); // (the drop's dome over the surface's own normal)
            cn = w_nmix(cn, R * nd, dr.x);
            fw = max(fw, 0.95 * dr.x), fa = mix(fa, 2e-5, dr.x);
            base *= 1.0 - 0.55 * (1.0 - 0.6 * sweat) * dr.x * smoothstep(0.03, 0.35, 1.0 - dot(nd, Nl)); // (its rim bends the light away; sweat's beads flatter)
        }
        fw = max(fw, 0.6 * dr.y);
    }
    if (fw > 0.002) rc_set(max(rc_w(), clamp(fw, 0.0, 1.0)), cn, fa);
}
#endif

// (2026-11-15, "o escoamento nasce em locais sem sentido ... pode ser mais realista pois tá bem feio, e deve estar apenas
// em locais que têm sentido") water running down a wall: threads, not bands of noise. The face cut in columns of 7 cm;
// a column carries a thread when amount (how much water reaches this face: the rain it faces, the water pouring over its
// top) picks it, starting some way below the top and fed as it goes down (wider, steadier), wandering a little, with
// pulses running down and a damp trail beside it. P in m, below: m under the face's top. Returns the thread (x, its
// water) and the damp trail (y); the thread's roundness tilts cn
vec2 rain_rivulets(vec3 P, vec3 N, float t, float amount, float below, inout vec3 cn) {
    vec3 g = vec3(0.0, 0.0, -1.0) - N * (-N.z);
    float gl = length(g);
    if (gl < 0.3 || amount <= 0.0) return vec2(0.0);
    g /= gl;
    vec3 across = cross(N, g);
    const float CW = 0.07;
    float sa = dot(P, across), a = dot(P, g), col = floor(sa / CW);
    uint h = w_cellhash(col, 0.0, 5501u);
    float h0 = w_u01(h);
    h = w_next(h);
    float h1 = w_u01(h);
    h = w_next(h);
    float h2 = w_u01(h);
    if (h0 > amount) return vec2(0.0);                   // (not one of the columns the water picked)
    float start = h1 * h1 * 1.5, run = below - start;    // (it gathers some way down the face)
    if (run <= 0.0) return vec2(0.0);
    float grow = clamp(run / 1.2, 0.0, 1.0);
    float x = (col + 0.5) * CW + sin(a * 3.1 + h2 * 6.28) * 0.016 + sin(a * 9.7 + h1 * 6.28) * 0.005;
    float w = (0.002 + 0.005 * grow) * (0.6 + 0.8 * h2); // (m: half its width)
    float d = sa - x, ad = abs(d);
    float pulse = 0.65 + 0.35 * sin((a - t * (0.6 + 0.6 * h1)) * 22.0 + h2 * 9.0);
    float line = (1.0 - smoothstep(w * 0.55, w, ad)) * smoothstep(0.0, 0.08, run) * pulse;
    float damp = (1.0 - smoothstep(w, w * 5.0, ad)) * smoothstep(0.0, 0.15, run) * clamp(gl * 1.5, 0.0, 1.0);
    cn = normalize(cn + across * (sign(d) * clamp(ad / w, 0.0, 1.0) * line * 0.7)); // (a thread is round: its sides bend the light)
    return vec2(line, damp);
}

// The wet look alone (a followed actor: its own wetness, no puddles): the material wet and a thin film
void rain_wet_look(FrameBuf f, vec3 ng, float wet, inout vec3 n, inout vec3 base, inout float rough, float metal) {
    rain_wet_base(f, wet, base, rough, metal);
    rain_set_coat(f, smoothstep(0.3, 1.0, wet) * 0.6, w_nmix(n, ng, 0.5), f.water[86].z * 1.8);
}
float rain_shade(FrameBuf f, vec3 p, vec3 ng, float own, inout vec3 n, inout vec3 base, inout float rough, inout float metal) {
    if (own >= 0.0 && f.water[39].x < 0.5) { // a followed actor: its own wetness wherever it is (under a roof it dries slowly)
        rain_wet_look(f, ng, clamp(own * f.water[2].x, 0.0, 1.0), n, base, rough, metal);
        return 0.0;
    }
    float mark = rain_marks(f, p, ng); // (2026-11-15: wet feet and drips, also where the rain doesn't fall)
    vec2 q = rain_u(f, p); // (its place in the slanted grid)
    // a steep surface: the cell just outside it (the rain falls past a wall, not on its top)
    if (ng.z < 0.5) q += normalize(ng.xy + vec2(1e-4)) * f.water[1].x * 1.2;
    ivec2 c = rain_cell(f, q);
    if (!rain_holds(f, c)) { // beyond the simulated window: the open cover (no roofs known out there)
        if (f.water[38].z > 0.0) snow_shade(f, p, ng, f.water[38].z, n, base, rough, metal);
        // (2026-11-14, "a chuva no chão some quando a câmera sobe") the open ground's wetness (water.c open_wet), no
        // puddles out there; its film reflects the far street
        // (Q27.6) a place the window left, or one the storms wet (or kept dry) away from the camera: the persistent map's
        vec4 pv;
        float ow = persist_at(f, q, pv) ? pv.y : f.water[56].w;
        float wo = f.water[39].x > 0.5 ? 0.0 : max(clamp(ow * f.water[2].x, 0.0, 1.0), mark);
        if (wo > 0.0) rain_wet_look(f, ng, wo, n, base, rough, metal);
        return 0.0;
    }
    vec4 surf = rain_surface(f, c);
    float exposed = rain_exposure(f, q, p.z); // (under a roof: below the rain's surface; soft at the edges)
    if (exposed <= 0.0) {
        if (mark > 0.0) rain_wet_look(f, ng, mark, n, base, rough, metal); // (a wet print indoors)
        return 0.0;
    }
    if (f.water[39].x > 0.5 || surf.w > 0.0) { // snow: a white cover on what faces up enough
        snow_shade(f, p, ng, surf.w * exposed, n, base, rough, metal);
        if (f.water[39].x > 0.5) return 0.0; // (snowing: no wet look, no puddles)
    }
    // the water: bilinear over the cells' centers
    vec2 g = q / f.water[1].x - 0.5;
    ivec2 c0 = ivec2(floor(g));
    vec2 tq = g - vec2(c0);
    vec4 w = mix(mix(rain_water(f, c0), rain_water(f, c0 + ivec2(1, 0)), tq.x), mix(rain_water(f, c0 + ivec2(0, 1)), rain_water(f, c0 + ivec2(1, 1)), tq.x), tq.y);
    float wet = max(clamp(w.y * f.water[2].x, 0.0, 1.0) * exposed, mark);
    bool sty = rain_stylized(f);
    float t = f.water[0].y, rain = f.water[0].x, dcam = distance(p, f.camera_pos.xyz);
    vec3 P = p * 0.01; // (m: the Tlender's scales)
    vec3 dir = normalize(vec3(f.water[84].xy, -f.water[89].z)); // (the way the drops fall, with the gusts)
    float rainf = clamp(rain / 10.0, 0.0, 1.0);
    // (2026-11-15) a wall is as wet as what reaches it: the rain it faces, the splashes near the ground (40 cm), not the
    // ground's wetness up its whole height (a wall in the lee was soaked to the top)
    float facing = clamp(-dot(ng, dir) * 3.0 + 0.2, 0.0, 1.0) * exposed;
    if (ng.z < 0.5) wet *= max(mix(0.25, 1.0, facing), 1.0 - smoothstep(10.0, 45.0, p.z - surf.x));
    rain_wet_base(f, wet, base, rough, metal);
    // the film over the wet: thicker as it soaks; the water fills the small relief (its surface smoother than the
    // material's), sharper where it is soaked
    float coat = smoothstep(0.3, 1.0, wet) * 0.8, steady = coat;
    vec3 cn = w_nmix(n, ng, 0.55);
    float froughk = f.water[86].z, crough = froughk * mix(2.5, 1.0, smoothstep(0.6, 1.0, wet));
    // the walls and slopes facing the rain: rivulets and drops
    if (ng.z < 0.75 && wet > 0.0) {
        if (f.water[2].z > 0.0 && dcam < 6000.0) {
            // where water comes down this face from: the rain it faces (while it rains) and what pours over its top (the
            // shallow water leaving the cell above it toward this side: a roof draining over the wall)
            vec2 nxy = normalize(ng.xy + vec2(1e-4));
            ivec2 ci = rain_cell(f, rain_u(f, p) - nxy * f.water[1].x * 0.6), sv = abs(nxy.x) > abs(nxy.y) ? ivec2(sign(nxy.x), 0) : ivec2(0, sign(nxy.y));
            float topz = rain_holds(f, ci) ? rain_surface(f, ci).x : p.z;
            float pour = rain_holds(f, ci) ? clamp(rain_outflow(f, ci, sv) / 3.0, 0.0, 1.0) : 0.0;
            float amount = (facing * rainf * 0.35 + pour * 0.8) * f.water[2].z * (1.0 - smoothstep(4000.0, 6000.0, dcam));
            vec2 rv = rain_rivulets(P, ng, t, amount, max(topz - p.z, 0.0) * 0.01, cn);
            coat = max(coat, rv.x * 0.95);
            base *= 1.0 - 0.18 * rv.y; // (the damp trail beside it: darker)
            crough = mix(crough, froughk, rv.x);
        }
        if (f.water[88].z > 0.0 && dcam < 2500.0 && facing > 0.0) {
            vec3 nd;
            float k = f.water[88].z * facing * (1.0 - smoothstep(1500.0, 2500.0, dcam));
            vec2 dr = rain_drops(P, ng, t, f.water[88].w * 0.01, clamp((0.3 + 0.4 * rainf) * k, 0.0, 0.95) * wet,
                                 clamp(0.6 * rainf * k, 0.0, 1.0), 0.6, nd);
            float on = min(dr.x * k, 1.0);
            cn = w_nmix(cn, nd, on);
            coat = max(coat, max(0.95 * on, 0.7 * dr.y));
            base *= 1.0 - 0.5 * on * smoothstep(0.03, 0.35, 1.0 - dot(nd, ng)); // (the drop's dark rim)
            crough = mix(crough, froughk * 0.5, on);
        }
    }
    // the splashes of the drops on what the rain hits now (in the puddles the rings do it)
    float flat_ = smoothstep(0.75, 0.95, ng.z);
    // (within 15 m: a crown is a few cm, under 2 px farther)
    if (f.water[88].y > 0.0 && rain > 0.05 && dcam < 1500.0) {
        vec4 sp = rain_impacts_on(P, ng, dir, t, clamp(rain / 20.0, 0.15, 3.0), f.water[83].y);
        float k = exposed * f.water[88].y * (1.0 - smoothstep(900.0, 1500.0, dcam)) * (1.0 - smoothstep(0.1, 0.4, w.x * flat_));
        base *= 1.0 - 0.3 * sp.w * k * (1.0 - wet); // (the first drops: dark dots before the ground soaks)
        coat = max(coat, sp.w * k * 0.6);
        cn = normalize(cn - sp.xyz * (0.004 * k * (sty ? 1.6 : 1.0)));
    }
    // puddles and streams: open water over what is under (the rings baked a frame; a stream: the rings carried by the
    // flow, two phases blended so they don't stretch). Their edges: the water's depth plus a little noise (no shapes of
    // the cells), hard in the Stylized style. (They fade out over the window's last 8 cells: past it the open ground has
    // none, no edge.)
    ivec2 ed = min(c - ivec2(f.water[1].zw), ivec2(int(f.water[1].y) - 1) - (c - ivec2(f.water[1].zw)));
    float d = w.x * exposed;
    if (f.water[88].x > 0.0 && d > 0.0)
        d += ((w_vnoise(p.xy / 55.0) - 0.5) * 0.6 + (w_vnoise(p.xy / 13.0 + 31.0) - 0.5) * 0.25) * f.water[88].x * min(d * 4.0, 1.0);
    float pud = flat_ * f.water[2].y * smoothstep(1.0, 8.0, float(min(ed.x, ed.y))) * (sty ? smoothstep(0.26, 0.3, d) : smoothstep(0.05, 0.6, d));
    if (pud > 0.0) {
        vec2 v = w.zw, sl;
        float ph = fract(t * 0.5);
        if (dot(v, v) > 4.0) {
            sl = mix(rain_ripple_map(f, p.xy - v * (ph - 0.5) * 0.6), rain_ripple_map(f, p.xy - v * (fract(ph + 0.5) - 0.5) * 0.6 + 61.0), abs(ph - 0.5) * 2.0);
            sl *= 1.0 + min(length(v) / 60.0, 2.0);
        } else
            sl = rain_ripple_map(f, p.xy);
        // (2026-11-15) the drips and streams off an edge above ring the puddle where they land (the same edges rain.vert
        // drips from: 30 cm or more above, holding water), faster in a storm
        if (dcam < 2500.0) {
            const ivec2 s4[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
            float rate = 0.9 + 2.2 * clamp((rain - 12.0) / 48.0, 0.0, 1.0);
            for (int j = 0; j < 4; j++) {
                ivec2 nb = c + s4[j];
                if (!rain_holds(f, nb)) continue;
                float dh = rain_surface(f, nb).x - surf.x;
                if (dh <= 30.0 || dh >= 3000.0 || rain_water(f, nb).y < 0.4) continue;
                if (rain_outflow(f, nb, -s4[j]) < 2.0) continue; // (the same eaves rain.vert drips from: water leaving over it)
                float hs = w_hash12(vec2(nb) + vec2(s4[j]) * 0.37);
                vec2 ctr = (vec2(c) + 0.5 + vec2(s4[j]) * 0.5 + vec2(s4[j].y, s4[j].x) * (hs - 0.5) * 0.8) * f.water[1].x - rain_slant(f) * p.z;
                vec2 dd = p.xy - ctr;
                float r = length(dd);
                if (r > 45.0) continue;
                for (int k = 0; k < 2; k++) {
                    float ph = fract(t * rate + hs + 0.5 * float(k)), R = ph * 32.0, x = (r - R) / 1.8;
                    float env = (1.0 - ph) * (1.0 - ph) * exp(-x * x);
                    sl += dd / max(r, 1e-3) * (-2.0 * x * env * 0.3 * cos(x * 3.0));
                }
            }
        }
        vec3 wn = normalize(vec3(-sl * f.water[2].w, 1.0));
        // what is seen through the water: darker and tinted as it gets deeper (Clarity), the ground's bumps smoothed (the
        // still water's refraction), its own reflection given to the water's
        float deep = smoothstep(0.1, 1.5, d), clar = f.water[87].w;
        if (sty) base = mix(base, f.water[87].rgb * 0.45, pud * 0.65);
        else base *= mix(vec3(1.0), f.water[87].rgb * mix(0.45, 0.95, clar), pud * mix(0.6, 1.0, deep));
        n = normalize(mix(n, ng, pud * (0.6 + 0.4 * deep)));
        rough = mix(rough, max(rough, 0.55), pud);
        metal = mix(metal, 0.0, pud * deep);
        coat = max(coat, pud);
        steady = max(steady, pud);
        cn = normalize(mix(cn, wn, pud));
        crough = mix(crough, froughk * 0.25, pud);
    }
    // (2026-11-10, "o chão aparenta estar à frente da casa") a wet wall gets a sheen, never a mirror
    if (ng.z < 0.5) crough = max(crough, 0.08);
    rain_set_coat(f, coat, cn, crough);
    g_rcs = clamp(steady * f.water[86].y, 0.0, 1.0);
    return pud;
}
// The rain on glass (Translucent): the drops staying and running down a window the rain reaches, as a film with their
// normal (no puddles, no wet look on glass). Returns the opacity: a drop scatters a little of what is behind
float rain_glass(FrameBuf f, vec3 p, vec3 ng, float opacity) {
    if (f.water[39].x > 0.5 || f.water[88].z <= 0.0 || abs(ng.z) > 0.92) return opacity;
    float dcam = distance(p, f.camera_pos.xyz);
    if (dcam > 2500.0) return opacity;
    vec2 q = rain_u(f, p) + normalize(ng.xy + vec2(1e-4)) * f.water[1].x * 1.2;
    ivec2 c = rain_cell(f, q);
    bool held = rain_holds(f, c);
    float exposed = held ? rain_exposure(f, q, p.z) : 1.0;
    float wet = held ? clamp(rain_water(f, c).y * f.water[2].x, 0.0, 1.0) * exposed : clamp(f.water[56].w, 0.0, 1.0);
    vec3 dir = normalize(vec3(f.water[84].xy, -f.water[89].z));
    float k = f.water[88].z * clamp(-dot(ng, dir) * 3.0 + 0.2, 0.0, 1.0) * exposed * (1.0 - smoothstep(1500.0, 2500.0, dcam));
    if (k * wet <= 0.0) return opacity;
    float rainf = clamp(f.water[0].x / 10.0, 0.0, 1.0);
    vec3 nd;
    vec2 dr = rain_drops(p * 0.01, ng, f.water[0].y, f.water[88].w * 0.01, clamp((0.3 + 0.4 * rainf) * k, 0.0, 0.95) * wet,
                         clamp(0.6 * rainf * k, 0.0, 1.0), 0.6, nd);
    float on = min(dr.x * k, 1.0);
    rain_set_coat(f, max(0.95 * on, 0.6 * dr.y), w_nmix(ng, nd, on), f.water[86].z * 0.5);
    g_rcs = 0.0;
    return opacity + on * 0.3 * (1.0 - opacity);
}

// --- The ocean and the ripples (water_surface.vert/.frag) ---
// The ocean's maps at a place (cm), cascade c: displacement + foam (m), or slopes + Jacobian
vec4 ocean_tap(FrameBuf f, uint c, vec2 p, uint which) {
    float L = W_CASCADE_SIDE(f, c) * 100.0;
    vec2 g = p / L * float(W_OCEAN_N) - 0.5;
    ivec2 i = ivec2(floor(g));
    vec2 t = g - vec2(i);
    uint base = (c * 2u + which) * uint(W_OCEAN_N * W_OCEAN_N);
    WaterBuf ob = WaterBuf(f.water_addr[6]);
    ivec2 m0 = i & (W_OCEAN_N - 1), m1 = (i + 1) & (W_OCEAN_N - 1);
    vec4 a = ob.v[base + uint(m0.y * W_OCEAN_N + m0.x)], b = ob.v[base + uint(m0.y * W_OCEAN_N + m1.x)];
    vec4 cc = ob.v[base + uint(m1.y * W_OCEAN_N + m0.x)], d = ob.v[base + uint(m1.y * W_OCEAN_N + m1.x)];
    return mix(mix(a, b, t.x), mix(cc, d, t.x), t.y);
}

float ripple_at(FrameBuf f, vec2 p) {
    if ((f.water_addr[3].x | f.water_addr[3].y) == 0u || f.water[11].x <= 0.0) return 0.0;
    ivec2 c = ivec2(floor(p / f.water[11].x));
    ivec2 d = c - ivec2(f.water[11].zw);
    if (d.x < 2 || d.y < 2 || d.x >= W_RIPPLE_N - 2 || d.y >= W_RIPPLE_N - 2) return 0.0;
    ivec2 m = c & (W_RIPPLE_N - 1); // (a power of two: & wraps negative cells too)
    vec4 r = WaterBuf(f.water_addr[3]).v[uint(m.y * W_RIPPLE_N + m.x)];
    return (f.frame_index & 1u) != 0u ? r.x : r.y; // (the one just written)
}

#endif
