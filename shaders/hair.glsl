// Hair in strands (A9.3b; groom.c, notas-dev/render.md "Cabelo em fios"): every Groom's strands in one device buffer
// (pc.vertices: vec4 points in its actor's space, cm; w the place along the strand, 0 root .. 1 tip; then each groom's
// density grid, floats), the grooms' table in a per-frame buffer (pc.instances), pc.pass the groom drawn.
#ifndef HAIR_GLSL
#define HAIR_GLSL

#define HAIR_GRID 32 // the density grid's side (groom.c GROOM_GRID)

struct GroomInfo {
    vec4 rows[3];          // actor -> world (cm)
    vec4 inv[3];           // world -> actor
    uint first, strands, pts, drawn; // its first point, its strands, points a strand, strands drawn (the LOD)
    float root_w, tip_w, cover, ao;  // widths (cm), the coverage factor (1 / the LOD's share), the sky's occlusion scale
    vec4 color;            // the fiber's color for one cm of path (rgb: exp(-absorption)), roughness
    vec4 bmin, bmax;       // the density grid's box (actor space, cm); bmin.w: its first float in the buffer
    vec4 prev[3];          // (2026-11-15) last frame's rows (the velocity pass: HAIR_VELOCITY)
    uint dyn, prev_first, bind, pal; // (2026-11-16) simulated: its points already in the world (rows: identity), last step's at prev_first
                           // (2026-11-25, item 16) on a skin: its strands' bones (2 words each, from word bind; 0: none),
                           // its bones' palette (3 vec4 a bone, from vec4 pal of this buffer; bmax.w: how many bones)
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer GroomInfoBuf { GroomInfo g[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer HairVec4 { vec4 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer HairFloat { float v[]; };

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer HairUint { uint v[]; };
// (2026-11-25, item 16) a strand on a skin: its root's 4 bones blended (linear blend skinning, the mesh's space), the
// whole strand bent with its root; none: the identity
mat3x4 groom_skin(GroomInfo g, uvec2 data, uvec2 info, uint strand) {
    mat3x4 m = mat3x4(vec4(1, 0, 0, 0), vec4(0, 1, 0, 0), vec4(0, 0, 1, 0));
    if (g.bind == 0u || g.dyn != 0u) return m; // (a simulated one: bind, pal its rest and its leaders, hair.vert)
    HairUint B = HairUint(data);
    uint bi = B.v[g.bind + strand * 2u], bw = B.v[g.bind + strand * 2u + 1u];
    HairVec4 P = HairVec4(info);
    uint nb = max(uint(g.bmax.w), 1u);
    mat3x4 acc = mat3x4(vec4(0.0), vec4(0.0), vec4(0.0));
    float ws = 0.0;
    for (int k = 0; k < 4; k++) {
        float w = float((bw >> (8 * k)) & 255u) / 255.0;
        if (w <= 0.0) continue;
        uint b = min((bi >> (8 * k)) & 255u, nb - 1u) * 3u;
        acc[0] += P.v[g.pal + b] * w, acc[1] += P.v[g.pal + b + 1u] * w, acc[2] += P.v[g.pal + b + 2u] * w;
        ws += w;
    }
    return ws > 0.0 ? acc * (1.0 / ws) : m;
}
vec3 groom_skin_point(mat3x4 m, vec3 p) { return vec3(dot(m[0], vec4(p, 1.0)), dot(m[1], vec4(p, 1.0)), dot(m[2], vec4(p, 1.0))); }

vec3 groom_world(GroomInfo g, vec3 p) { return vec3(dot(g.rows[0], vec4(p, 1.0)), dot(g.rows[1], vec4(p, 1.0)), dot(g.rows[2], vec4(p, 1.0))); }
vec3 groom_local(GroomInfo g, vec3 p) { return vec3(dot(g.inv[0], vec4(p, 1.0)), dot(g.inv[1], vec4(p, 1.0)), dot(g.inv[2], vec4(p, 1.0))); }
vec3 groom_local_dir(GroomInfo g, vec3 d) { return vec3(dot(g.inv[0].xyz, d), dot(g.inv[1].xyz, d), dot(g.inv[2].xyz, d)); }

// The light through the hair from a point toward a direction (actor space): the density grid marched (its extinction:
// the fibers' length times their width per cm^3, 1/cm), the transmittance (the deep shadow of Unreal's voxelized hair)
float groom_transmittance(GroomInfo g, uvec2 data, vec3 p, vec3 d) {
    vec3 ext = g.bmax.xyz - g.bmin.xyz;
    vec3 cell = ext / float(HAIR_GRID);
    float step_cm = min(cell.x, min(cell.y, cell.z));
    uint base = uint(g.bmin.w);
    HairFloat D = HairFloat(data);
    float tau = 0.0;
    vec3 q = p + d * (0.5 * step_cm); // (half a step: the fiber itself not counted)
    for (int i = 0; i < 16; i++) {
        vec3 u = (q - g.bmin.xyz) / ext;
        if (any(lessThan(u, vec3(0.0))) || any(greaterThanEqual(u, vec3(1.0)))) break;
        ivec3 c = ivec3(u * float(HAIR_GRID));
        tau += D.v[base + uint((c.z * HAIR_GRID + c.y) * HAIR_GRID + c.x)] * step_cm;
        if (tau > 12.0) break;
        q += d * step_cm;
    }
    return exp(-tau);
}
#endif
