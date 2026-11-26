#version 460
// Hair in strands (A9.3b, groom.c): each segment of a strand a ribbon facing the camera, from gl_VertexIndex (6 a
// segment). Its width the fiber's, at least a pixel: a thinner fiber keeps its share as its coverage (hair.frag drops
// that share of its pixels, TSR averages them). Past the drawn strands (the LOD: the far ones fewer, wider) nothing.
#include "common.glsl"
#include "hair.glsl"

#ifdef HAIR_VELOCITY // (2026-11-15) the velocity pass (velocity.frag): this frame's place and last frame's by its rows
layout(location = 0) out vec4 o_cur;
layout(location = 1) out vec4 o_prev;
#else
layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_tan;
layout(location = 2) out float o_t;
layout(location = 3) flat out uint o_strand;
layout(location = 4) out float o_cover;
#endif
invariant gl_Position; // (the depth prepass and the shading pass: the same depth, the EQUAL test)

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    GroomInfo g = GroomInfoBuf(pc.instances).g[pc.pass];
    uint segs = max(g.pts, 2u) - 1u;
    uint q = uint(gl_VertexIndex) / 6u, k = uint(gl_VertexIndex) % 6u;
    uint s = q / segs, j = q % segs;
    if (s >= g.drawn) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    HairVec4 P = HairVec4(pc.vertices);
    uint at = g.first + s * g.pts + j;
    vec4 a = P.v[at], b = P.v[at + 1u];
    mat3x4 sk = groom_skin(g, pc.vertices, pc.instances, s); // (2026-11-25, item 16: on a skin, bent by its root's bones)
    a.xyz = groom_skin_point(sk, a.xyz), b.xyz = groom_skin_point(sk, b.xyz);
    // (2026-11-25, item 16) simulated with leaders (groom.c lead_strands): past the first bmax.w strands a strand follows its
    // leader: the leader's simulated points plus its own offset from the leader at rest (bind: the rest points, actor
    // space), turned into the world by the actor's rows (inv's inverse)
    bool follower = g.dyn != 0u && g.pal != 0u && s >= uint(g.bmax.w);
    uint L = s;
    vec3 off0 = vec3(0.0), off1 = vec3(0.0);
    if (follower) {
        L = HairUint(pc.vertices).v[g.pal + s];
        mat3 R = inverse(transpose(mat3(g.inv[0].xyz, g.inv[1].xyz, g.inv[2].xyz)));
        uint rc = g.bind + s * g.pts + j, rl = g.bind + L * g.pts + j, sl = g.first + L * g.pts + j;
        off0 = R * (P.v[rc].xyz - P.v[rl].xyz), off1 = R * (P.v[rc + 1u].xyz - P.v[rl + 1u].xyz);
        a = vec4(P.v[sl].xyz + off0, a.w), b = vec4(P.v[sl + 1u].xyz + off1, b.w);
    }
    vec3 wa = groom_world(g, a.xyz), wb = groom_world(g, b.xyz);
    bool tip_end = k == 1u || k == 2u || k == 4u;   // (two triangles: a a' b' / a b' b)
    float side = k == 0u || k == 1u || k == 3u ? -1.0 : 1.0;
    vec3 wp = tip_end ? wb : wa;
    float t = tip_end ? b.w : a.w;
    vec3 T = wb - wa;
    T = dot(T, T) > 1e-12 ? normalize(T) : vec3(0.0, 0.0, 1.0);
    vec3 V = view_ortho(f) ? -f.camera_fwd.xyz : normalize(f.camera_pos.xyz - wp);
    vec3 sd = cross(T, V);
    sd = dot(sd, sd) > 1e-8 ? normalize(sd) : f.camera_right.xyz;
    // a pixel's size here (cm): its distance along the view times the field of view over the width in pixels
    float dist = max(dot(wp - f.camera_pos.xyz, f.camera_fwd.xyz), 1.0);
    float px = view_ortho(f) ? 2.0 * f.camera_up.w * f.camera_right.w * f.viewport.z : 2.0 * dist * f.camera_fwd.w * f.viewport.z;
    float w = mix(g.root_w, g.tip_w, t) * g.cover;
    float ew = max(w, px);
    wp += sd * (0.5 * ew * side);
#ifdef HAIR_VELOCITY
    vec3 pa = tip_end ? b.xyz : a.xyz; // (the point in actor space by last frame's rows; the ribbon's width as now)
    if (g.dyn != 0u) pa = P.v[g.prev_first + L * g.pts + j + (tip_end ? 1u : 0u)].xyz + (tip_end ? off1 : off0); // (simulated: last step's world point; a follower: its leader's and its offset)
    vec3 pw = vec3(dot(g.prev[0], vec4(pa, 1.0)), dot(g.prev[1], vec4(pa, 1.0)), dot(g.prev[2], vec4(pa, 1.0))) + sd * (0.5 * ew * side);
    gl_Position = f.view_proj * vec4(wp, 1.0);
    o_cur = gl_Position;
    o_prev = f.view_proj * vec4(pw, 1.0); // (this frame's camera: the hair's own motion only, as velocity.vert)
#else
    o_world = wp;
    o_tan = T;
    o_t = t;
    o_strand = s;
    o_cover = w / ew;
    gl_Position = f.view_proj * vec4(wp, 1.0);
#endif
}
