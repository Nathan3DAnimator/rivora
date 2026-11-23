#version 460
// The particles' ribbons (ribbon.vert): soft across (the sprites' soft disc on the across axis alone), a material's
// texture along it (u across, v the births), lit or glowing as the sprites (particles_frag.glsl)
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
    Emitter e = EmitterBuf(pc.instances).e;
    vec4 col = i_color;
    if (e.render2.z > 0.5) { // (the texture along it; the sprite's shading then takes the ribbon as a square across)
        uint ti = uint(e.render2.z) - 1u;
        vec4 t = texture(u_mat_tex[nonuniformEXT(ti)], vec2(i_uv.x * 0.5 + 0.5, fract(i_uv.y)));
        col.rgb *= t.rgb, col.a *= t.a;
    }
    // the across falloff alone (the sprite's soft disc with the along coordinate at its center)
    vec4 c = shade_sprite(vec2(i_uv.x, 0.0), col, i_right, i_up, i_frame);
    if (c.a < 0.0) discard;
    o_color = max(c, vec4(0.0));
}
