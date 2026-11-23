#version 460
// The particles' quads (particle.vert): particles_frag.glsl shades them
#include "common.glsl"
#include "particles.glsl"
#include "particles_frag.glsl"

layout(location = 0) in vec2 i_uv;
layout(location = 1) in vec4 i_color;
layout(location = 2) flat in vec3 i_right;
layout(location = 3) flat in vec3 i_up;
layout(location = 4) flat in float i_frame;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 c = shade_sprite(i_uv, i_color, i_right, i_up, i_frame);
    if (c.a < 0.0) discard;
    o_color = c;
}
