// The GI probes' sampling (gi.c): no ray queries here, so the raster shaders use it on any GPU. Included after
// common.glsl.

// --- The GI probes (gi.c): a grid that follows the camera, stored toroidally (a probe's slot is its world cell
// modulo the grid), irradiance and distance moments in octahedral tiles of 8x8 texels plus a 1 texel border ---
#define PROBE_TEXELS 8
#define PROBE_TILE 10
// The probes and the lightmaps hold E / pi times this (half floats: the sun's 100 000 lux would overflow them)
#define GI_STORE (1.0 / 256.0)
#define GI_LOAD 256.0

// The cell is moved to positive numbers before the modulo: on the NVIDIA driver `%` of a negative cell by a size that
// isn't a power of two (the 12 layers in z) gave a wrong slot, two cells shared one (a probe under the floor and the
// one at a character's height), each reset the other at every update and the light on the actors flickered. 65536
// grids of offset: cells down to -786 432 (z), far beyond any level.
ivec3 probe_slot(ivec3 world_cell, ivec3 dims) { return ivec3(uvec3(world_cell + dims * 65536) % uvec3(dims)); }

// The texel coordinate (in the atlas, for linear sampling) of direction d in the probe stored at slot (tile_y: its grid's
// first row of tiles: 0 the near grid, dims.y the far one)
vec2 probe_uv_at(ivec3 slot, ivec3 dims, int tile_y, vec3 d, vec2 atlas_size) {
    vec2 tile = vec2(float(slot.x + slot.z * dims.x), float(slot.y + tile_y)) * float(PROBE_TILE);
    vec2 o = (oct_encode_f(d) * 0.5 + 0.5) * float(PROBE_TEXELS) + 1.0;
    return (tile + o) / atlas_size;
}
vec2 probe_uv(ivec3 slot, ivec3 dims, vec3 d, vec2 atlas_size) { return probe_uv_at(slot, dims, 0, d, atlas_size); }

// The probe grids (A2, notas-dev/gi.md "Sondas distantes"): 0 the near one (gi.c: probe_min, probe_dims, probe_params),
// 1 the far one: as many probes PROBE_FAR_SCALE times wider apart around the camera (32 x 32 x 12 at 2 m: 64 m; at 16 m:
// 512 m), its tiles under the near grid's in the atlas, its state after the near grid's (and the slots' counter)
#define PROBE_FAR_SCALE 8.0
struct ProbeGrid { ivec3 lo, dims; float spacing; int tile_y; uint state_base; };
ProbeGrid probe_grid(FrameBuf f, int c) {
    ProbeGrid g;
    g.dims = f.probe_dims.xyz;
    if (c == 0) {
        g.lo = f.probe_min.xyz, g.spacing = f.probe_params.x, g.tile_y = 0, g.state_base = 0u;
    } else { // (as gi.c centers the near one: more room above the camera than below)
        g.spacing = f.probe_params.x * PROBE_FAR_SCALE;
        vec3 cc = floor(f.camera_pos.xyz / g.spacing);
        g.lo = ivec3(cc) - ivec3(g.dims.x / 2, g.dims.y / 2, g.dims.z / 3);
        g.tile_y = g.dims.y;
        g.state_base = uint(g.dims.x * g.dims.y * g.dims.z) + 1u;
    }
    return g;
}

// Irradiance at p (normal n, toward the viewer v) from the 8 probes around it: trilinear, a backface term and the
// Chebyshev visibility test on the distance moments (no light through walls).
// One grid's: inside, how far p is in it (0 at its last probes or out, 1 from 2 probes in)
vec3 probe_irradiance_grid(FrameBuf f, sampler2D irr_tex, sampler2D dist_tex, ProbeGrid G, vec3 p, vec3 n, vec3 v, out float inside) {
    float spacing = G.spacing;
    ivec3 dims = G.dims;
    float bias = spacing / f.probe_params.x; // (the far grid's biases as far as its spacing)
    vec3 bp = p + n * f.probe_params.z * bias + v * f.probe_params.w * bias;
    ivec3 lo = G.lo, hi = G.lo + dims - 1;
    // a point beyond the grid reads its edge where it is nearest, without the visibility test: its distances to the
    // probes are meaningless there, and the 8 weights all crushed to nothing made tiny changes flicker (the horizon)
    vec3 gmin = (vec3(lo) + 0.5) * spacing, gmax = (vec3(hi) + 0.5) * spacing;
    bool outside = any(lessThan(bp, gmin)) || any(greaterThan(bp, gmax));
    vec3 ein = min(bp - gmin, gmax - bp) / spacing; // (in probes from the grid's edge)
    inside = clamp((min(min(ein.x, ein.y), ein.z) - 0.5) / 1.5, 0.0, 1.0);
    bp = clamp(bp, gmin, gmax);
    vec3 g = bp / spacing - 0.5; // probes sit at cell centers (never on a floor at z = 0)
    ivec3 base = ivec3(floor(g));
    vec3 alpha = g - vec3(base);
    vec2 atlas = vec2(textureSize(irr_tex, 0));
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 8; i++) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 cell = clamp(base + off, lo, hi);
        vec3 probe = (vec3(cell) + 0.5) * spacing;
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        float w = tri.x * tri.y * tri.z;
        vec3 to_probe = normalize(probe - p);
        float wrap = (dot(to_probe, n) + 1.0) * 0.5;
        w *= wrap * wrap + 0.2;
        ivec3 slot = probe_slot(cell, dims);
        vec3 pd = bp - probe;
        float r = length(pd);
        vec2 m = textureLod(dist_tex, probe_uv_at(slot, dims, G.tile_y, pd / max(r, 1e-3), atlas), 0.0).rg;
        if (r > m.x && !outside) {
            float var = abs(m.y - m.x * m.x);
            float c = var / (var + (r - m.x) * (r - m.x));
            w *= max(c * c * c, 0.0);
        }
        w = max(w, 1e-5);
        if (w < 0.2) w *= w * w / 0.04; // crush the tiny weights (DDGI)
        vec4 e = textureLod(irr_tex, probe_uv_at(slot, dims, G.tile_y, n, atlas), 0.0);
        w *= e.a + 1e-3; // alpha: 0 for a probe inside an object or under a floor (probe_update.comp)
        sum += e.rgb * w;
        wsum += w;
    }
    return wsum > 0.0 ? sum / wsum * GI_LOAD : vec3(0.0);
}

// Irradiance at p (normal n, toward the viewer v): the near grid; past it, and across its last 2 probes blended, the far
// one (GI_PROBES_FAR): the light of buildings and hills hundreds of meters off bounces and is occluded, not the sky alone
vec3 probe_irradiance(FrameBuf f, sampler2D irr_tex, sampler2D dist_tex, vec3 p, vec3 n, vec3 v) {
    float in0, in1;
    vec3 e0 = probe_irradiance_grid(f, irr_tex, dist_tex, probe_grid(f, 0), p, n, v, in0);
    if ((f.gi_flags & GI_PROBES_FAR) == 0u || in0 >= 1.0) return e0;
    vec3 e1 = probe_irradiance_grid(f, irr_tex, dist_tex, probe_grid(f, 1), p, n, v, in1);
    return mix(e1, e0, in0);
}

// --- Contact AO (render.c fills the occluders): the cosine-weighted share of the hemisphere above (p, n) that the
// Movable actors nearest the camera cover, computed exactly for boxes and spheres. No rays, no noise, no history:
// what the lightmaps and the probes are too coarse to see (an actor on a baked floor, two actors touching). ---

// One edge's term of a polygon's projected solid angle (Lambert): positive for edges that turn counter-clockwise
// around n's side
float ao_edge(vec3 a, vec3 b, vec3 n) {
    vec3 x = cross(a, b);
    float s = length(x);
    return s > 1e-6 ? acos(clamp(dot(a, b), -1.0, 1.0)) * dot(n, x) / s : 0.0;
}

// The box face at c (half-axes u, w) seen from p: its projected solid angle / pi, what is below the horizon dropped
float ao_face(vec3 p, vec3 n, vec3 c, vec3 u, vec3 w) {
    vec3 v0 = normalize(c + u + w - p), v1 = normalize(c - u + w - p), v2 = normalize(c - u - w - p), v3 = normalize(c + u - w - p);
    float s = ao_edge(v0, v1, n) + ao_edge(v1, v2, n) + ao_edge(v2, v3, n) + ao_edge(v3, v0, n);
    // the winding as seen from p: the face's normal (u x w) pointing away from p counts as positive
    if (dot(cross(u, w), c - p) < 0.0) s = -s;
    return max(s, 0.0) / (2.0 * PI);
}

// The occluders' light (occluder_light.comp, ray tracing only): six radiances each (E / pi units, like the probes),
// for a box its faces +X -X +Y -Y +Z -Z along its own axes; for a sphere the six world directions (an ambient cube)
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer OccLightBuf { vec4 l[]; };

// The contact GI of the Movable actors nearest the camera at (p, n): rgb, the light their lit faces send to p (one
// bounce, form factor times their radiance: a red ball in the sun tints the floor next to it, baked or not); a, the
// share of the hemisphere they leave open (AO). Exact for boxes, a sphere for the round shapes; no rays, no noise, no
// history.
// cached: p's surface has surface cache texels, which already hold the actors standing still (their occlusion and
// bounce): only the moving ones (ay.w 1: the cache's rays leave them out) are added
vec4 contact_gi(FrameBuf f, vec3 p, vec3 n, uint self, bool cached) {
    if ((f.gi_flags & GI_CONTACT_AO) == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    bool bounce = (f.gi_flags & GI_CONTACT_BOUNCE) != 0u;
    OccLightBuf ol = OccLightBuf(f.occ_light);
    float occ = 0.0;
    vec3 light = vec3(0.0);
    for (uint i = 0u; i < f.occluder_count; i++) {
        // its center and reach first (16 bytes): most are out of reach of most pixels (the whole occluder loaded for each
        // of the 32 cost 2.1 ms with two characters' parts filling the list)
        vec4 oc = f.occluders[i].center;
        vec3 d = oc.xyz - p;
        float dist = length(d), reach = abs(oc.w);
        if (dist > reach) continue;
        Occluder o = f.occluders[i];
        if (o.entity == self) continue;
        // a cached surface already has the still actors (their occlusion and bounce: the cache's rays see them); only the
        // moving ones count there. A house of Movable walls standing still: their boxes doubled the corners' shadow and
        // cost every pixel 1.8 ms
        if (cached && o.ay.w < 0.5) continue;
        bool ob = bounce;
        float o_occ;
        vec3 o_light = vec3(0.0);
        if (oc.w < 0.0) { // a sphere: (r / d)^2 cos
            float r = o.ax.x;
            o_occ = clamp(dot(n, d / max(dist, 1e-3)), 0.0, 1.0) * min(r * r / max(dist * dist, 1e-3), 1.0);
            if (ob && o_occ > 0.0) { // the side facing p: the ambient cube toward p
                vec3 t = -d / max(dist, 1e-3), w = t * t;
                uint b = i * 6u;
                o_light = (w.x * ol.l[b + (t.x > 0.0 ? 0u : 1u)].rgb + w.y * ol.l[b + (t.y > 0.0 ? 2u : 3u)].rgb +
                           w.z * ol.l[b + (t.z > 0.0 ? 4u : 5u)].rgb) * o_occ;
            }
        } else { // a box: its faces that face p
            vec3 X = o.ax.xyz, Y = o.ay.xyz, Z = o.az;
            vec3 q = -d;
            o_occ = 0.0;
            float lx = dot(q, X) / dot(X, X), ly = dot(q, Y) / dot(Y, Y), lz = dot(q, Z) / dot(Z, Z);
            uint b = i * 6u;
            if (abs(lx) > 1.0) {
                float ff = ao_face(p, n, o.center.xyz + X * sign(lx), Y * sign(lx), Z);
                o_occ += ff;
                if (ob) o_light += ff * ol.l[b + (lx > 0.0 ? 0u : 1u)].rgb;
            }
            if (abs(ly) > 1.0) {
                float ff = ao_face(p, n, o.center.xyz + Y * sign(ly), Z * sign(ly), X);
                o_occ += ff;
                if (ob) o_light += ff * ol.l[b + (ly > 0.0 ? 2u : 3u)].rgb;
            }
            if (abs(lz) > 1.0) {
                float ff = ao_face(p, n, o.center.xyz + Z * sign(lz), X * sign(lz), Y);
                o_occ += ff;
                if (ob) o_light += ff * ol.l[b + (lz > 0.0 ? 4u : 5u)].rgb;
            }
        }
        float fade = 1.0 - smoothstep(reach * 0.75, reach, dist); // no edge where the search stops
        occ += o_occ * fade;
        light += o_light * fade;
    }
    if (occ > 1.0) light /= occ; // overlapping occluders: what they cover together is the hemisphere at most
    return vec4(light, 1.0 - clamp(occ, 0.0, 1.0));
}

// --- The surface cache (gi_cache.c, gi_cache.comp) ---
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer CacheStBuf { vec4 st[]; };

// The indirect light the surface cache holds for a mesh (its scene slot) at uv2: rgb E / pi (before the GI intensity),
// a how much to trust it (0: no chart, or texels not updated yet / inside another mesh: use the probes)
// The walls around each texel (gi_cache.comp): 1 - the distance to one / 1.5 texels along the atlas +x, -x, +y, -y (0: none
// that near; a texel never measured reads 0 too)
layout(set = 0, binding = 36) uniform sampler2D u_cache_walls;
// The same for the baked lightmaps (gi_bake.c, lightmap_bake.comp pass 2; zero in a .rlight before v4: plain bilinear)
layout(set = 0, binding = 38) uniform sampler2D u_lightmap_walls;

// Whether texel t may light a point o texels from its center: not when a wall stands between them (along either axis)
bool atlas_lends(sampler2D walls, ivec2 t, vec2 o) {
    vec4 w = texelFetch(walls, t, 0);
    float dx = 1.5 * (1.0 - (o.x >= 0.0 ? w.x : w.y)), dy = 1.5 * (1.0 - (o.y >= 0.0 ? w.z : w.w));
    return abs(o.x) < dx && abs(o.y) < dy;
}

// The cache's light at an atlas uv: bilinear over the 4 texels around it, weighed by validity (texels buried inside other
// meshes and the gutters between islands left out) and only the texels with no wall between their center and the point:
// one floor for two rooms, one long wall past an inner wall, a wall thinner than a texel standing on it, a texel
// straddling it: the light of the far side never crosses. When none of the 4 may light it, the darkest valid one of the
// 8 around its texel that may, else the darkest valid one (never the brighter: the far side of a wall is the lit one).
// The lightmaps are read the same way (their texels are the cache's: gi_bake.c), with their own walls.
vec3 atlas_read(sampler2D tex, sampler2D walls, vec2 uv, float atlas) {
    ivec2 hi = ivec2(int(atlas) - 1);
    vec2 g = uv * atlas;
    ivec2 b = ivec2(floor(g - 0.5));
    vec2 fr = g - 0.5 - vec2(b);
    vec4 sum = vec4(0.0);
    for (int k = 0; k < 4; k++) {
        ivec2 d = ivec2(k & 1, k >> 1);
        ivec2 t = clamp(b + d, ivec2(0), hi);
        float wgt = (d.x == 1 ? fr.x : 1.0 - fr.x) * (d.y == 1 ? fr.y : 1.0 - fr.y);
        if (wgt <= 0.0 || !atlas_lends(walls, t, g - (vec2(b + d) + 0.5))) continue;
        sum += texelFetch(tex, t, 0) * wgt; // premultiplied by validity
    }
    if (sum.a > 0.05) return sum.rgb / sum.a;
    ivec2 t = clamp(ivec2(g), ivec2(0), hi);
    vec3 best = vec3(0.0), any_best = vec3(0.0);
    float bl = 1e30, al = 1e30;
    for (int i = 0; i < 9; i++) {
        ivec2 tt = clamp(t + ivec2(i % 3 - 1, i / 3 - 1), ivec2(0), hi);
        vec4 n = texelFetch(tex, tt, 0);
        if (n.a <= 0.5) continue;
        vec3 v = n.rgb / n.a;
        float l = dot(v, vec3(0.2126, 0.7152, 0.0722));
        if (l < al) al = l, any_best = v;
        if (l < bl && atlas_lends(walls, tt, g - (vec2(tt) + 0.5))) bl = l, best = v;
    }
    return bl < 1e30 ? best : any_best;
}
vec3 cache_read(sampler2D tex, vec2 uv, float atlas) { return atlas_read(tex, u_cache_walls, uv, atlas); }

// A baked lightmap (E / pi times GI_STORE, premultiplied by validity) at a mesh's uv2 and its rectangle in the atlas
vec3 lightmap_read(sampler2D lm, vec4 st, vec2 uv2) {
    return atlas_read(lm, u_lightmap_walls, uv2 * st.xy + st.zw, float(textureSize(lm, 0).x));
}

// The cache's light at a mesh's uv2. A chart closed on itself (wrap: MeshInfo.uv2_wrap, the side of a round mesh) has its
// two edges on one seam, lit by different texels: within 1.5 texels of it the read is mixed with the same point read on
// the other edge (half and half on the seam itself), or the seam shows as a line down the mesh.
vec3 cache_read_uv2(sampler2D tex, vec4 st, float atlas, vec2 uv2, vec2 wrap) {
    vec3 c = cache_read(tex, uv2 * st.xy + st.zw, atlas);
    if (wrap.x <= 0.0 || uv2.y >= wrap.y) return c;
    float d = min(uv2.x - wrap.x, 1.0 - wrap.x - uv2.x) * st.x * atlas; // texels to the seam
    if (d >= 1.5) return c;
    float span = 1.0 - 2.0 * wrap.x;
    vec2 o = vec2(uv2.x < 0.5 ? uv2.x + span : uv2.x - span, uv2.y);
    return mix(c, cache_read(tex, o * st.xy + st.zw, atlas), 0.5 - 0.5 * max(d, 0.0) / 1.5);
}

// A point of a wrapped chart moved by o (uv2) stays on it: past one edge, back in from the other
vec2 uv2_wrapped(vec2 uv2, vec2 o, vec2 wrap) {
    vec2 p = uv2 + o;
    if (wrap.x > 0.0 && uv2.y < wrap.y) {
        float span = 1.0 - 2.0 * wrap.x;
        p.x = p.x < wrap.x ? p.x + span : (p.x > 1.0 - wrap.x ? p.x - span : p.x);
    }
    return p;
}

// A Movable actor's own texels, read wider (the point and one texel around it, 5 reads): they see the light change as it
// moves and each one is redone every few frames from a few rays: read one by one they made blotches that followed it
vec4 cache_indirect_soft(FrameBuf f, sampler2D tex, uint slot, vec2 uv2, vec2 wrap) {
    if ((f.gi_flags & GI_SURFACE_CACHE) == 0u) return vec4(0.0);
    vec4 st = CacheStBuf(f.cache_st).st[slot];
    if (st.x <= 0.0) return vec4(0.0);
    float atlas = f.cache_params.x;
    vec2 e = 1.0 / (atlas * st.xy); // one texel in uv2
    vec3 s = cache_read_uv2(tex, st, atlas, uv2, wrap) * 2.0 +
             cache_read_uv2(tex, st, atlas, uv2_wrapped(uv2, vec2(e.x, 0.0), wrap), wrap) +
             cache_read_uv2(tex, st, atlas, uv2_wrapped(uv2, vec2(-e.x, 0.0), wrap), wrap) +
             cache_read_uv2(tex, st, atlas, uv2_wrapped(uv2, vec2(0.0, e.y), wrap), wrap) +
             cache_read_uv2(tex, st, atlas, uv2_wrapped(uv2, vec2(0.0, -e.y), wrap), wrap);
    return vec4(s / 6.0 * GI_LOAD, 1.0);
}

vec4 cache_indirect(FrameBuf f, sampler2D tex, uint slot, vec2 uv2, vec2 wrap) {
    if ((f.gi_flags & GI_SURFACE_CACHE) == 0u) return vec4(0.0);
    vec4 st = CacheStBuf(f.cache_st).st[slot];
    if (st.x <= 0.0) return vec4(0.0);
    // a surface with a chart is never lit by the probes (they leak: a bright line where a wall meets the floor)
    return vec4(cache_read_uv2(tex, st, f.cache_params.x, uv2, wrap) * GI_LOAD, 1.0);
}
