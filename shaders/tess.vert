#version 460
// (Q22) Nanite Tessellation's micro vertices (notas-dev/tessellation.md): Q17's pass 9, out of mesh.vert into its own
// shader (render.c's tess twins: each opaque mesh pipeline's copy with this vertex shader). mesh.vert went back to its
// size (every other pass paid the registers of this one), and a micro vertex reads its base triangle's corners that
// tess.comp gathered (tess_tri_load: 128 B, the same for every point of the triangle) instead of walking the record,
// the instance, the mesh, the material, the indices, the vertices and the canonical table (and a texture read per
// corner point). Same outputs as mesh.vert (mesh.frag reads them); RE_EXT: the vertex color, white.
#include "common.glsl"
#extension GL_EXT_nonuniform_qualifier : require
#define MAT_SET 1
#include "mat_common.glsl"
#include "tess.glsl"

// the prepass and the main pass compute the same depth (the main pass tests EQUAL-or-nearer against it)
invariant gl_Position;

layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_normal;
layout(location = 2) flat out uint o_instance;
layout(location = 3) out vec4 o_uvs;
layout(location = 4) flat out float o_ray_bias; // (negative: a displaced surface, mesh.vert)
#ifdef RE_EXT
layout(location = 5) out vec4 o_vcol;
#endif

void main() {
    FrameBuf f = FrameBuf(pc.frame);
#ifdef RE_EXT
    o_vcol = vec4(1.0);
#endif
    // tess.comp's record (gl_InstanceIndex: the base triangle, its instance, its edge factors), the pattern's point (i, j
    // of N). On an edge: snapped to that edge's own factor (k of e) and computed from the edge alone (tess_tri_edge: no
    // crack against the triangle beside it, whatever its N); inside: the barycentric point (the point is its index:
    // i | j << 6 | N - 1 << 12 | its copy in the batch << 17, tess.c)
    uint pv = uint(gl_VertexIndex);
    uint i = pv & 63u, j = (pv >> 6) & 63u, N = ((pv >> 12) & 31u) + 1u;
    uvec4 rec = TessRecBuf(addr_add(f.tess_buf, TESS_REC_AT)).r[uint(gl_InstanceIndex) * tess_batch(N) + (pv >> 17)];
    if (rec.x == TESS_NONE) { // (a list's padding: dropped)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        o_world = vec3(0.0), o_normal = vec3(0.0, 0.0, 1.0), o_instance = 0u, o_uvs = vec4(0.0), o_ray_bias = 0.0;
        return;
    }
    TessTri t = tess_tri_load(f, rec.x);
    Instance inst = InstanceBuf(pc.instances).i[rec.y];
    TessCtx x; // (its rows alone: tess_world)
    x.r0 = inst.row0, x.r1 = inst.row1, x.r2 = inst.row2;
    uint e01 = rec.z & 63u, e12 = (rec.z >> 6) & 63u, e02 = (rec.z >> 12) & 63u;
    TessCorner a = t.k0, b = t.k1;
    uint st = i, e = e01, fl = t.flags & 7u;
    bool edge = true;
    if (j == 0u) {
    } else if (i == 0u) {
        b = t.k2, st = j, e = e02, fl = (t.flags >> 6) & 7u;
    } else if (i + j == N) {
        a = t.k1, b = t.k2, st = j, e = e12, fl = (t.flags >> 3) & 7u;
    } else
        edge = false;
    vec3 pm, nm;
    vec2 uv, uv2;
    if (edge) {
        uint k = (2u * st * e + N) / (2u * N);
        pm = tess_tri_edge(f, t, x, a, b, fl, k, e);
        float wb = float(k) / float(e), wa = 1.0 - wb;
        nm = oct_decode(a.n) * wa + oct_decode(b.n) * wb;
        uv = a.uv * wa + b.uv * wb, uv2 = unpackUnorm2x16(a.uv2) * wa + unpackUnorm2x16(b.uv2) * wb;
    } else {
        float w1 = float(i) / float(N), w2 = float(j) / float(N), w0 = 1.0 - w1 - w2;
        vec3 p = t.k0.p * w0 + t.k1.p * w1 + t.k2.p * w2;
        nm = oct_decode(t.k0.n) * w0 + oct_decode(t.k1.n) * w1 + oct_decode(t.k2.n) * w2;
        uv = t.k0.uv * w0 + t.k1.uv * w1 + t.k2.uv * w2;
        uv2 = unpackUnorm2x16(t.k0.uv2) * w0 + unpackUnorm2x16(t.k1.uv2) * w1 + unpackUnorm2x16(t.k2.uv2) * w2;
        pm = p + normalize(nm) * tess_tri_disp(f, t, x, uv, p);
    }
    vec3 world = tess_world(x, pm);
    vec3 ra = vec3(inst.row0.x, inst.row1.x, inst.row2.x), rb = vec3(inst.row0.y, inst.row1.y, inst.row2.y), rc = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    o_normal = cross(rb, rc) * nm.x + cross(rc, ra) * nm.y + cross(ra, rb) * nm.z;
    o_world = world;
    o_instance = rec.y;
    o_uvs = vec4(uv2, uv * t.tiling);
    // its rays start past the DAG cut's error (as pass 6) and past how deep the displacement sinks under the BLAS
    float px_cm = view_ortho(f) ? 1.0 / f.cull_params.x : length(world - f.camera_pos.xyz) / f.cull_params.x;
    o_ray_bias = -(max(uintBitsToFloat(f.pad_cull1), 0.25) * px_cm * 1.25 + t.bias);
    gl_Position = f.view_proj * vec4(world, 1.0);
}
