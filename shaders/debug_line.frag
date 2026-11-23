#version 460
// (2026-11-19) The debug lines and points (debug_line.vert): the edge anti-aliased, a third as strong where the scene
// is nearer (reversed Z: the scene's depth greater), over the tone-mapped picture
#include "common.glsl"

layout(set = 0, binding = 21) uniform sampler2D u_depth; // the main pass's, one sample (the render's resolution)

layout(location = 0) in vec4 i_color;
layout(location = 1) noperspective in float i_across;
layout(location = 2) noperspective in float i_width;
layout(location = 3) in vec2 i_zw;
layout(location = 0) out vec4 o_color;

void main() {
    float edge = clamp((1.0 - abs(i_across)) * i_width * 0.5, 0.0, 1.0); // (the outer pixel fades)
    vec2 size = vec2(float(pc.pass), float(pc.pad));
    vec2 uv = (gl_FragCoord.xy - vec2(float(pc.instances.x), float(pc.instances.y & 0x3FFFFFFFu))) / size;
    float scene = texelFetch(u_depth, clamp(ivec2(uv * vec2(textureSize(u_depth, 0))), ivec2(0), textureSize(u_depth, 0) - 1), 0).r;
    float d = i_zw.x / max(i_zw.y, 1e-6);
    float a = i_color.a * edge * (d + 1e-6 < scene * 0.9995 ? 0.3 : 1.0);
    if (a <= 0.002) discard;
    o_color = vec4(i_color.rgb * a, a); // (premultiplied)
}
