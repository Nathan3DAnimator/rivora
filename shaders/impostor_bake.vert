#version 460
// (Q21) An impostor's bake (farview.c): the mesh's LOD 0 drawn orthographically from one of the atlas's N x N
// hemi-octahedral directions into that frame's square (the viewport). pc.frame: the bake's parameters (the mesh's
// bounding sphere, N, the material slot, the materials' buffer), pc.vertices: the vertices, pc.pass: the frame (i + j N)
#include "common.glsl"

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ImpBakeBuf { vec4 sphere; uvec4 info; uvec2 materials; };

layout(location = 0) out vec3 o_pos;   // mesh space
layout(location = 1) out vec3 o_normal;
layout(location = 2) out vec2 o_uv;
layout(location = 3) flat out vec3 o_dir; // toward the frame's camera

vec3 hemi_dec(vec2 e) {
    vec2 t = vec2(e.x + e.y, e.x - e.y) * 0.5;
    return normalize(vec3(t, max(1.0 - abs(t.x) - abs(t.y), 0.0)));
}

void main() {
    ImpBakeBuf B = ImpBakeBuf(pc.frame);
    uint n = B.info.x;
    uvec2 k = uvec2(pc.pass % n, pc.pass / n);
    vec3 d = hemi_dec(vec2(k) / float(n - 1u) * 2.0 - 1.0);
    vec3 c = cross(vec3(0.0, 0.0, 1.0), d);
    vec3 r = dot(c, c) < 1e-8 ? vec3(1.0, 0.0, 0.0) : normalize(c);
    vec3 u = cross(d, r);
    Vertex v = VertexBuf(pc.vertices).v[gl_VertexIndex];
    vec3 q = (v.pos - B.sphere.xyz) / B.sphere.w;
    o_pos = v.pos;
    o_normal = oct_decode(v.normal_oct);
    o_uv = v.uv;
    o_dir = d;
    // reversed depth: nearer the frame's camera (along +d) is larger
    gl_Position = vec4(dot(q, r), -dot(q, u), clamp(0.5 + 0.5 * dot(q, d), 0.0, 1.0), 1.0);
}
