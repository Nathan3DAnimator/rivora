#version 460
// The velocity pass for the crowds (render.c, after velocity.vert's moved actors): the cull's visible lists drawn once
// more (pc.pass 0: the early one, 3: the late one), only the crowds' parts emitted (the others' triangles collapse to a
// point), each part's motion from its rows last frame (crowd.comp keeps them: pc.instances, from slot pc.pad) to now.
// Without it the TSR took the agents for still and reprojected the history from where they were: smears behind them
#include "common.glsl"

invariant gl_Position;
layout(location = 0) out vec4 o_cur;
layout(location = 1) out vec4 o_prev;

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer PrevRowsBuf { vec4 r[]; };

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    uint slot = pc.pass == 3u ? UintBuf(f.visible_late).v[gl_InstanceIndex] : UintBuf(f.visible_main).v[gl_InstanceIndex];
    Instance inst = InstanceBuf(f.instances).i[slot];
    // (not a crowd's: nothing drawn. The far agents too, LOD 1-2: skipping them left their smears, measured)
    if ((inst.ids.y & INST_CROWD) == 0u || slot < pc.pad) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        o_cur = o_prev = gl_Position;
        return;
    }
    Vertex v = VertexBuf(pc.vertices).v[gl_VertexIndex];
    vec4 p = vec4(v.pos, 1.0);
    uint k = (slot - pc.pad) * 3u;
    PrevRowsBuf pr = PrevRowsBuf(pc.instances);
    vec3 world = vec3(dot(inst.row0, p), dot(inst.row1, p), dot(inst.row2, p));
    vec3 prev = vec3(dot(pr.r[k], p), dot(pr.r[k + 1u], p), dot(pr.r[k + 2u], p));
    gl_Position = f.view_proj * vec4(world, 1.0);
    o_cur = gl_Position;
    o_prev = f.view_proj * vec4(prev, 1.0); // (this frame's camera: the part's own motion only, as velocity.vert)
}
