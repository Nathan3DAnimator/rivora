#version 460
// The asset previews' background (fullscreen.vert): the editor's vertical gradient
#include "common.glsl"
#include "preview.glsl"

layout(location = 0) in vec2 i_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    PvFrame f = PvFrame(pc.frame);
    if (f.mode == PV_OVERDRAW) { // (Q7) black: the layers add up from nothing
        o_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if ((f.flags & PVF_SKY) != 0u) { // (Q7) the outdoor sky along the pixel's ray (i_ndc.y: -1 at the top)
        vec3 d = normalize(f.cam_fwd.xyz + f.cam_right.xyz * (i_ndc.x * f.cam_right.w) + f.cam_up.xyz * (-i_ndc.y * f.cam_up.w));
        o_color = vec4(pv_display(pv_sky(f, d)), 1.0);
        return;
    }
    o_color = vec4(pv_background(f, i_ndc.y * 0.5 + 0.5), 1.0);
}
