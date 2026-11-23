#version 460
// The velocity pass (render.c, post processing): the actors that moved this frame drawn once more, their screen motion
// from last frame's transform to this one's (the camera's own motion is the post pass's, from the depth)
#include "common.glsl"

invariant gl_Position;
layout(location = 0) out vec4 o_cur;
layout(location = 1) out vec4 o_prev;

struct VelEntry { uint slot, pad0, pad1, pad2; vec4 row0, row1, row2; }; // last frame's rows
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer VelBuf { VelEntry e[]; };

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    VelEntry ve = VelBuf(pc.instances).e[gl_InstanceIndex];
    Instance inst = InstanceBuf(f.instances).i[ve.slot];
    Vertex v = VertexBuf(pc.vertices).v[gl_VertexIndex];
    vec4 p = vec4(v.pos, 1.0), pp = p;
    if ((inst.ids.y & INST_SKINNED) != 0u) { // a skin: this frame's bent vertex and last frame's (render.c "skin")
        uint local = uint(gl_VertexIndex - MeshInfoBuf(f.mesh_info).m[inst.ids.y >> INST_SHAPE_SHIFT].vertex_offset.x);
        p = vec4(VertexBuf(pc.vertices).v[inst.ids.w + local].pos, 1.0);
        pp = vec4(VertexBuf(pc.vertices).v[floatBitsToUint(inst.lightmap_st.x) + local].pos, 1.0);
    }
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    vec3 prev = vec3(dot(ve.row0, pp), dot(ve.row1, pp), dot(ve.row2, pp));
    gl_Position = f.view_proj * vec4(world, 1.0);
    o_cur = gl_Position;
    o_prev = f.view_proj * vec4(prev, 1.0); // (this frame's camera: the object's own motion only)
}
