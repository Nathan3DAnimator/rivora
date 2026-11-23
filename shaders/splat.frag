#version 460
// A Gaussian's pixel (A9.5d, splat.vert): its weight at the offset by the conic, times its opacity; premultiplied, over
// what is behind (the quads come back to front), the scene's depth in front of it (the pipeline's test)
#include "common.glsl"

layout(location = 0) in vec2 i_d;
layout(location = 1) flat in vec3 i_conic;
layout(location = 2) flat in vec4 i_color;
layout(location = 0) out vec4 o_color;

void main() {
    float p = -0.5 * (i_conic.x * i_d.x * i_d.x + i_conic.z * i_d.y * i_d.y) - i_conic.y * i_d.x * i_d.y;
    if (p > 0.0) discard;
    float a = min(0.99, i_color.a * exp(p));
    if (a < 1.0 / 255.0) discard;
    o_color = vec4(i_color.rgb * a, a);
}
