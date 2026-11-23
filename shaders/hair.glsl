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
    uint dyn, prev_first, pad0, pad1; // (2026-11-16) simulated: its points already in the world (rows: identity), last step's at prev_first
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer GroomInfoBuf { GroomInfo g[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer HairVec4 { vec4 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer HairFloat { float v[]; };

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
