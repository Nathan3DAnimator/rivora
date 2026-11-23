#version 460
// The velocity pass: the object's motion on screen this frame (uv units, current minus previous)
layout(location = 0) in vec4 i_cur;
layout(location = 1) in vec4 i_prev;
layout(location = 0) out vec2 o_vel;

void main() {
    vec2 a = i_cur.xy / i_cur.w, b = i_prev.xy / max(i_prev.w, 1e-4);
    vec2 v = (a - b) * 0.5;
    o_vel = dot(v, v) > 0.0625 ? vec2(0.0) : v; // more than a quarter of the screen in a frame: a teleport, not motion
}
