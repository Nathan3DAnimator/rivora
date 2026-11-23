#version 460
// AR's occlusion (render.c, in the sky's place when the real depth is on): the camera's picture with the real world's
// depth, so that a real thing nearer than a virtual one hides it (the depth test: drawn where the real is nearer than
// what the scene drew, the sky's 0 included). The depth kept in the depth buffer: the real floor's shadow plane, the
// rain and the particles behind a real thing hidden too. Pushed back 3% + 3 cm: the real floor under a virtual thing,
// as near as its base, never cuts it, nor the floor's own shadow plane.
#include "common.glsl"
#include "ar.glsl"

layout(location = 0) in vec2 i_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    o_color = ar_camera_color(f, i_ndc);
    float d = ar_real_depth(f, i_ndc);
    gl_FragDepth = d > 0.0 ? f.ar[3].w / (d * 1.03 + 3.0) : 0.0; // (reversed infinite Z: near / depth; unknown: the sky's)
}
