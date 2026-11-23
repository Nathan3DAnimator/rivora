#version 460
// AR (ar_arcore.c, render.c): the real floor's planes that receive the virtual things' shadows, as triangles (world,
// cm) in a buffer (pc.instances): drawn over the camera's picture (ar_plane.frag darkens it where a shadow falls)
#include "common.glsl"

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ArVerts { vec4 p[]; };
layout(location = 0) out vec3 o_world;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    vec3 p = ArVerts(pc.instances).p[gl_VertexIndex].xyz;
    o_world = p;
    gl_Position = f.view_proj * vec4(p, 1.0);
}
