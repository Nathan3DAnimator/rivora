#version 460
// The asset previews' AO view mode (Q7, render_preview.c): the depth pass. No attachment: each fragment leaves its view
// depth in the pixel's slot of a buffer, the nearest kept (atomicMin on the float's bits: positive floats order as
// uints); preview.frag then compares a hemisphere of points around each surface with it
#include "common.glsl"
#include "preview.glsl"

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
layout(location = 2) flat in uint i_instance;
layout(location = 3) in vec2 i_uv;

void main() {
    PvFrame f = PvFrame(pc.frame);
    uvec2 px = uvec2(gl_FragCoord.xy);
    if (px.x >= f.ao_w || px.y >= f.ao_h) return;
    float d = max(dot(i_world - f.eye.xyz, f.cam_fwd.xyz), 0.0);
    atomicMin(PvDepth(f.ao_depth).d[px.y * f.ao_w + px.x], floatBitsToUint(d));
}
