#version 460
// The particles' points (particle_point.vert): the point's square as the quad (gl_PointCoord), the camera's axes
#include "common.glsl"
#include "particles.glsl"
#include "particles_frag.glsl"

layout(location = 0) in vec4 i_color;
layout(location = 1) flat in float i_frame;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    vec2 uv = vec2(gl_PointCoord.x * 2.0 - 1.0, 1.0 - gl_PointCoord.y * 2.0);
    vec4 c = shade_sprite(uv, i_color, f.camera_right.xyz, f.camera_up.xyz, i_frame);
    if (c.a < 0.0) discard;
    o_color = c;
}
