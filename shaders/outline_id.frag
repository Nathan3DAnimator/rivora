#version 460
// (Q11) The outlined actors' ids (outline.c, notas-dev/toon.md), at the render's size with the main pass's jitter: the
// entry of its outline in the frame's table (pc.pad: 1..255, bits 0-7), a hash of its slot (two actors of one material
// apart: bits 8-15, never 0), its normal octahedral (bits 16-31: the creases). A Masked material keeps its cut.
#include "common.glsl"
#define MAT_SET 1
#include "mat_common.glsl"

layout(location = 0) in vec3 i_world;
layout(location = 1) in vec3 i_normal;
layout(location = 2) flat in uint i_instance;
layout(location = 3) in vec4 i_uvs; // (zw: the material's UV)
layout(location = 0) out uint o_id;

void main() {
    FrameBuf f = FrameBuf(pc.frame);
    Instance inst = InstanceBuf(pc.instances).i[i_instance];
    if ((inst.ids.y & INST_MATERIAL) != 0u) {
        GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
        if ((gm.flags & MATF_MASKED) != 0u && texture(u_mat_tex[nonuniformEXT(gm.tex_base)], i_uvs.zw).a < float(gm.flags >> 24u) / 255.0) discard;
    }
    vec3 n = normalize(i_normal);
    if (!gl_FrontFacing) n = -n;
    vec2 o = clamp(oct_encode_f(n) * 0.5 + 0.5, 0.0, 1.0);
    uint h = (i_instance * 2654435761u) >> 24;
    o_id = (pc.pad & 0xFFu) | (max(h, 1u) << 8) | (uint(o.x * 255.0 + 0.5) << 16) | (uint(o.y * 255.0 + 0.5) << 24);
}
