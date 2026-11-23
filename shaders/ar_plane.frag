#version 460
// AR's shadow catcher (ar_plane.vert): the real floor already holds the real light; the virtual things take from it
//   - the sun's share (f.ar[2].y: how much of the floor's light is the main light's, from the light estimate) where they
//     hide the sun (its virtual shadow maps);
//   - the rest, the sky's, where they cover the floor's hemisphere (the contact AO of the actors near the camera, as the
//     meshes get it: an object on a table under an overcast sky still sits on it).
// Blended premultiplied: the camera's picture below times 1 - alpha.
#include "common.glsl"
#include "gi.glsl"
#include "vsm.glsl"

layout(location = 0) in vec3 i_world;
layout(location = 0) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    float s = clamp(f.ar[2].y, 0.0, 0.95);
    float vis = 1.0;
    if ((f.gi_flags & GI_VSM) != 0u && f.sun_dir.w > 0.0 && f.sun_dir.z > 0.0) vis = clamp(vsm_shadow(f, i_world, vec3(0.0, 0.0, 1.0)), 0.0, 1.0);
    float ao = clamp(contact_gi(f, i_world, vec3(0.0, 0.0, 1.0), 0xFFFFFFFFu, false).a, 0.0, 1.0);
    float keep = (1.0 - s * (1.0 - vis)) * (1.0 - (1.0 - s) * (1.0 - ao) * 0.85);
    o_color = vec4(0.0, 0.0, 0.0, 1.0 - keep);
}
