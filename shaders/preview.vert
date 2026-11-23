#version 460
// The asset previews (render_preview.c): the scene's vertices (the shared buffer), the preview's own instances; pass 1
// is the key light's shadow map
#include "common.glsl"
#include "preview.glsl"

layout(location = 0) out vec3 o_world;
layout(location = 1) out vec3 o_normal;
layout(location = 2) flat out uint o_instance;
layout(location = 3) out vec2 o_uv;

void main() {
    PvFrame f = PvFrame(pc.frame);
    Vertex v = VertexBuf(pc.vertices).v[gl_VertexIndex];
    Instance inst = InstanceBuf(pc.instances).i[gl_InstanceIndex];
    vec4 p = vec4(v.pos, 1.0);
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    vec3 a = vec3(inst.row0.x, inst.row1.x, inst.row2.x);
    vec3 b = vec3(inst.row0.y, inst.row1.y, inst.row2.y);
    vec3 c = vec3(inst.row0.z, inst.row1.z, inst.row2.z);
    vec3 n = oct_decode(v.normal_oct);
    o_normal = cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z;
    o_world = world;
    o_instance = gl_InstanceIndex;
    o_uv = v.uv * ((inst.ids.y & INST_MATERIAL) != 0u ? inst.emissive_rough.y : 1.0);
    gl_Position = (pc.pass == 1u ? f.light_matrix : f.view_proj) * vec4(world, 1.0);
}
