#version 460
// The particles' sprites (particles.c): 6 vertices a visible particle of the cull's sprite list (no instancing: tiny
// instances run at a fraction of the rate), a quad facing the camera, turned by its spin, or stretched along its
// velocity (sparks, rain). A mesh particle too small on screen comes here too: a sprite of its mesh's color and size.
#include "common.glsl"
#include "particles.glsl"

layout(location = 0) out vec2 o_uv;
layout(location = 1) out vec4 o_color;
layout(location = 2) flat out vec3 o_right;
layout(location = 3) flat out vec3 o_up;
layout(location = 4) flat out float o_frame; // the flipbook's cell (+ the blend into the next)

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    uint vi = uint(gl_VertexIndex), slot = UintBuf(e.list_sprite).v[vi / 6u], c = vi % 6u;
    Particle q = ParticleBuf(e.pool).p[e.base + slot];
    vec4 r = p_rand4(e.seed, slot, q.k);
    float age = e.time - q.birth, a = age / max(p_life(e, r), 1e-4);
    vec3 vel = (e.flags & PF_LOCAL) != 0u ? p_dir_to_world(e, q.vel) : q.vel;
    float size = p_size(e, r, a) * p_speed_size(e, vel);
    vec4 col = p_color(e, r, a);
    col.a *= p_speed_alpha(e, vel);
    bool mesh = (e.flags & PF_MESH) != 0u;
    if (mesh) col.rgb *= e.mesh_color.rgb, size *= max(e.mesh_scale.w, 1.0) * 0.7; // (a mesh far away: its color, its extent)
    vec3 w = p_to_world(e, q.pos) - f.camera_fwd.xyz * e.extra.x; // (Camera Offset: toward the camera)
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 k = corners[c];
    vec3 right = f.camera_right.xyz, up = f.camera_up.xyz, fwd = f.camera_fwd.xyz;
    float hx = size * 0.5, hy = size * 0.5;
    if ((e.flags & PF_VELOCITY_ALIGN) != 0u && !mesh) { // along the velocity seen from the camera, longer the faster
        vec3 v = (e.flags & PF_LOCAL) != 0u ? p_dir_to_world(e, q.vel) : q.vel;
        vec3 vs = v - fwd * dot(v, fwd);
        float sl = length(vs);
        if (sl > 1e-3) {
            up = vs / sl, right = normalize(cross(up, fwd));
            hy += sl * e.render_p.z * 0.01; // (stretch: cm of length per cm/s)
        }
    } else { // the spin
        float ang = radians((r.w * 2.0 - 1.0) * e.size_p.z + e.size_p.w * age);
        float cs = cos(ang), sn = sin(ang);
        vec3 r2 = right * cs + up * sn, u2 = up * cs - right * sn;
        right = r2, up = u2;
    }
    vec3 pos = w + right * (k.x * hx) + up * (k.y * hy);
    gl_Position = f.view_proj * vec4(pos, 1.0);
    o_uv = k;
    o_color = col;
    o_right = right, o_up = up;
    o_frame = p_subuv_frame(e, r, age, a);
}
