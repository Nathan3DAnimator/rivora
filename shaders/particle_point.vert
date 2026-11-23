#version 460
// The particles' small sprites (particles.c): one vertex a visible particle of the cull's point list, a point as wide as
// the particle on screen (up to its point_px); its spin doesn't show at that size. Six times fewer vertices than quads.
#include "common.glsl"
#include "particles.glsl"

layout(location = 0) out vec4 o_color;
layout(location = 1) flat out float o_frame;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    uint slot = UintBuf(e.list_point).v[gl_VertexIndex];
    Particle q = ParticleBuf(e.pool).p[e.base + slot];
    vec4 r = p_rand4(e.seed, slot, q.k);
    float age = e.time - q.birth, a = age / max(p_life(e, r), 1e-4);
    vec3 vel = (e.flags & PF_LOCAL) != 0u ? p_dir_to_world(e, q.vel) : q.vel;
    float size = p_size(e, r, a) * p_speed_size(e, vel);
    vec4 col = p_color(e, r, a);
    col.a *= p_speed_alpha(e, vel);
    if ((e.flags & PF_MESH) != 0u) col.rgb *= e.mesh_color.rgb, size *= max(e.mesh_scale.w, 1.0) * 0.7;
    vec3 w = p_to_world(e, q.pos) - f.camera_fwd.xyz * e.extra.x;
    gl_Position = f.view_proj * vec4(w, 1.0);
    float dist = max(gl_Position.w, 1.0);
    gl_PointSize = view_ortho(f) ? max(size * f.cull_params.x, 1.0) : max(size / dist * f.cull_params.x, 1.0);
    o_color = col;
    o_frame = p_subuv_frame(e, r, age, a);
}
