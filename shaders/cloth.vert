#version 460
// A cloth (cloth.c): its grid's quads from gl_VertexIndex (pc.pass: the cloth), its normals from the neighbors, a quad
// whose springs tore collapsed (the hole)
#include "common.glsl"
#include "cloth.glsl"

#ifdef CLOTH_VELOCITY // (2026-11-15) the velocity pass (velocity.frag): this frame's place and the step's start (last frame's)
layout(location = 0) out vec4 o_cur;
layout(location = 1) out vec4 o_prev;
#else
layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_normal;
layout(location = 2) out vec2 o_uv;
#endif

vec3 at(ClothHeader H, ClothInfo c, ivec2 g) {
    g = clamp(g, ivec2(0), ivec2(int(c.nx) - 1, int(c.ny) - 1));
    return ClothVec4(H.pos).v[c.base + uint(g.y) * c.nx + uint(g.x)].xyz * 100.0;
}

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    ClothHeader H = ClothHeaderBuf(pc.instances).h;
    if (pc.pass >= H.ncloth) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; } // (guards, as cloth_sim.comp)
    ClothInfo c = ClothInfoBuf(H.info).c[pc.pass];
    if (cl_mesh(c)) { // a mesh: its triangles (a cut vertex's collapsed), its normals from pass 9, its UVs
        uint t = uint(gl_VertexIndex) / 3u, kk = uint(gl_VertexIndex) % 3u;
        if (t >= c.iter || c.nx < 3u) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
        uvec3 tr = ClothUvec3(H.mtris).v[c.color + t];
        uint lv = kk == 0u ? tr.x : kk == 1u ? tr.y : tr.z;
        ClothUint mk = ClothUint(H.mask);
        if (lv >= c.nx || mk.v[c.base + tr.x] != 0u || mk.v[c.base + tr.y] != 0u || mk.v[c.base + tr.z] != 0u) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
        vec3 p = ClothVec4(H.pos).v[c.base + lv].xyz * 100.0;
#ifdef CLOTH_VELOCITY
        gl_Position = f.view_proj * vec4(p, 1.0);
        o_cur = gl_Position, o_prev = f.view_proj * vec4(ClothVec4(H.prev).v[c.base + lv].xyz * 100.0, 1.0);
        return;
#else
        o_world = p;
        o_normal = ClothVec4(H.mnormal).v[c.base + lv].xyz;
        o_uv = ClothVec2(H.muv).v[c.base + lv];
        if ((pc.pad & 0x10000u) != 0u) {
            uint lvl = pc.pad & 0xFFu;
            vec3 l = (f.vsm_mat[lvl] * vec4(p, 1.0)).xyz;
            vec4 jr = f.vsm_job[lvl];
            gl_Position = vec4((l.xy - jr.xy) * jr.zw - 1.0, l.z, 1.0);
            return;
        }
        gl_Position = f.view_proj * vec4(p, 1.0);
        return;
#endif
    }
    if (c.nx < 2u || c.ny < 2u) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    uint vi = uint(gl_VertexIndex), q = vi / 6u, k = vi % 6u;
    const ivec2 corner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));
    ivec2 g0 = ivec2(int(q % (c.nx - 1u)), int(q / (c.nx - 1u)));
    ivec2 g = g0 + corner[k];
    // a torn quad: its corner's springs to the right, down or across, or the far corner's back
    ClothUint mk = ClothUint(H.mask);
    uint m0 = mk.v[c.base + uint(g0.y) * c.nx + uint(g0.x)], m1 = mk.v[c.base + uint(g0.y + 1) * c.nx + uint(g0.x + 1)];
    if ((m0 & (1u | 4u | 16u)) != 0u || (m1 & (2u | 8u)) != 0u) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }
    vec3 p = at(H, c, g);
#ifdef CLOTH_VELOCITY
    ivec2 gc = clamp(g, ivec2(0), ivec2(int(c.nx) - 1, int(c.ny) - 1));
    gl_Position = f.view_proj * vec4(p, 1.0);
    o_cur = gl_Position, o_prev = f.view_proj * vec4(ClothVec4(H.prev).v[c.base + uint(gc.y) * c.nx + uint(gc.x)].xyz * 100.0, 1.0);
    return;
#else
    vec3 n = cross(at(H, c, g + ivec2(1, 0)) - at(H, c, g - ivec2(1, 0)), at(H, c, g + ivec2(0, 1)) - at(H, c, g - ivec2(0, 1)));
    o_world = p;
    o_normal = dot(n, n) > 1e-8 ? normalize(n) : vec3(0.0, 0.0, 1.0);
    o_uv = vec2(g) / vec2(float(c.nx - 1u), float(c.ny - 1u));
    if ((pc.pad & 0x10000u) != 0u) { // its depth into a virtual shadow map level (render.c VSM_PASS, as mesh.vert's pass 1)
        uint lv = pc.pad & 0xFFu;
        vec3 l = (f.vsm_mat[lv] * vec4(p, 1.0)).xyz;
        vec4 jr = f.vsm_job[lv];
        gl_Position = vec4((l.xy - jr.xy) * jr.zw - 1.0, l.z, 1.0);
        return;
    }
    gl_Position = f.view_proj * vec4(p, 1.0);
#endif
}
