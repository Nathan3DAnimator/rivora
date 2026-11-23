#version 460
// The particles' ribbons (B10, Niagara's Ribbon Renderer; particles.c): 6 vertices a segment, segment s between rate slot
// s and the next one (the births come one after the other through the slots: slot s at s / rate, then the next lap).
// A pair not born one after the other (a dead end, the lap's seam on a still-empty slot) collapses out of view. The strip
// faces the camera, its width each end's size; uv.x across (-1..1), uv.y along (births, the texture's repeats).
#include "common.glsl"
#include "particles.glsl"

layout(location = 0) out vec2 o_uv;
layout(location = 1) out vec4 o_color;
layout(location = 2) flat out vec3 o_right;
layout(location = 3) flat out vec3 o_up;
layout(location = 4) flat out float o_frame;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    uint vi = uint(gl_VertexIndex), seg = vi / 6u, c = vi % 6u, n = e.rate_slots;
    uint s0 = seg, s1 = seg + 1u < n ? seg + 1u : 0u;
    Particle a = ParticleBuf(e.pool).p[e.base + s0], b = ParticleBuf(e.pool).p[e.base + s1];
    float gap = b.birth - a.birth, step_t = 1.0 / max(e.rate, 1e-4);
    bool ok = a.k != 0xFFFFFFFFu && b.k != 0xFFFFFFFFu && gap > 0.0 && gap < step_t * 1.5;
    if (!ok) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0); // (outside the view: the pair isn't one after the other)
        o_uv = vec2(0.0), o_color = vec4(0.0), o_right = vec3(0.0), o_up = vec3(0.0), o_frame = 0.0;
        return;
    }
    // the corner: 0 a-, 1 b-, 2 b+, 3 a-, 4 b+, 5 a+
    const uint end_of[6] = uint[6](0u, 1u, 1u, 0u, 1u, 0u);
    const float side_of[6] = float[6](-1.0, -1.0, 1.0, -1.0, 1.0, 1.0);
    uint which = end_of[c];
    Particle q = which == 0u ? a : b;
    uint slot = which == 0u ? s0 : s1;
    vec4 r = p_rand4(e.seed, slot, q.k);
    float age = e.time - q.birth, life = max(p_life(e, r), 1e-4), t = age / life;
    vec3 pa = p_to_world(e, a.pos), pb = p_to_world(e, b.pos), p = which == 0u ? pa : pb;
    vec3 along = pb - pa;
    float len = length(along);
    // a pair farther apart than their motion explains (a particle flung off, a slot not yet moved): the strip breaks there
    float width_a = p_size(e, p_rand4(e.seed, s0, a.k), (e.time - a.birth) / max(p_life(e, p_rand4(e.seed, s0, a.k)), 1e-4)) * e.ribbon.x;
    float reach = 4.0 * (length(a.vel) + length(b.vel)) * step_t + 8.0 * width_a + 1.0;
    if (!(len <= reach)) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        o_uv = vec2(0.0), o_color = vec4(0.0), o_right = vec3(0.0), o_up = vec3(0.0), o_frame = 0.0;
        return;
    }
    along = len > 1e-4 ? along / len : f.camera_up.xyz;
    vec3 to_cam = normalize(f.camera_pos.xyz - p);
    vec3 side = cross(along, to_cam);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : f.camera_right.xyz;
    float width = p_size(e, r, t) * e.ribbon.x;
    vec4 col = p_color(e, r, t);
    col.a *= mix(1.0, clamp(1.0 - t, 0.0, 1.0), e.ribbon.z); // (Tail Fade: the old end thins out)
    vec3 pos = p + side * (side_of[c] * width * 0.5);
    gl_Position = f.view_proj * vec4(pos, 1.0);
    // along: the births since the slot's lap began (a texture repeats every ribbon.y of them)
    float births = (q.birth * e.rate) / max(e.ribbon.y, 1.0);
    o_uv = vec2(side_of[c], births);
    o_color = col;
    o_right = side, o_up = along;
    o_frame = 0.0;
}
