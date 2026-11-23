#version 460
// The particles' meshes (particles.c): an instance a visible particle of the cull's mesh list; the mesh's own vertices
// (its LOD 0 from the shared buffers), scaled by the particle's size, turned by its spin, at its place.
#include "common.glsl"
#include "particles.glsl"

layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_normal;
layout(location = 2) out vec4 o_color;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Emitter e = EmitterBuf(pc.instances).e;
    uint slot = UintBuf(e.list_mesh).v[gl_InstanceIndex];
    Particle q = ParticleBuf(e.pool).p[e.base + slot];
    vec4 r = p_rand4(e.seed, slot, q.k);
    float age = e.time - q.birth, a = age / max(p_life(e, r), 1e-4);
    float size = p_size(e, r, a) * p_speed_size(e, (e.flags & PF_LOCAL) != 0u ? p_dir_to_world(e, q.vel) : q.vel);
    Vertex v = VertexBuf(pc.vertices).v[gl_VertexIndex];
    // the spin: a random axis, its angle and rate (size_p.zw)
    vec4 r2 = p_rand4(e.seed ^ 0x2C1B3C6Du, slot, q.k);
    vec3 ax = normalize(r2.xyz * 2.0 - 1.0 + vec3(1e-4));
    float ang = radians((r.w * 2.0 - 1.0) * e.size_p.z + e.size_p.w * age);
    float cs = cos(ang), sn = sin(ang);
    vec3 lp = v.pos * e.mesh_scale.xyz * size;
    vec3 rp = lp * cs + cross(ax, lp) * sn + ax * dot(ax, lp) * (1.0 - cs); // (Rodrigues)
    // (the normal read as the vertex's 4th uint, not v.normal_oct: Adreno 740's compiler refused this pipeline when the
    // field came through the struct, the S23, 2026-11-08; the Vertex is 8 uints: pos 3, normal 1, uv 2, uv2 2)
    vec3 n0 = oct_decode(UintBuf(pc.vertices).v[uint(gl_VertexIndex) * 8u + 3u]) / max(e.mesh_scale.xyz, vec3(1e-4));
    vec3 rn = n0 * cs + cross(ax, n0) * sn + ax * dot(ax, n0) * (1.0 - cs);
    bool local = (e.flags & PF_LOCAL) != 0u;
    vec3 w = p_to_world(e, q.pos) - f.camera_fwd.xyz * e.extra.x + (local ? p_dir_to_world(e, rp) : rp);
    o_world = w;
    o_normal = normalize(local ? p_dir_to_world(e, rn) : rn);
    o_color = vec4(p_color(e, r, a).rgb * e.mesh_color.rgb, 1.0);
    gl_Position = f.view_proj * vec4(w, 1.0);
}
