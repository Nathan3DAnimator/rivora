#version 460
// The selected meshes' silhouettes, for the outline in composite.frag: an id per object (1..255, from its slot), the
// nearest one where they overlap (the pass's own depth), so each selected object gets its own outline
// The game's highlight (interaction.h, the push's pad 1): the top id (255), which composite.frag outlines in its own color
// (2026-11-25, item 17) pad 2 and up: the actor's own id (render.c: an actor's parts, its Skeletal Mesh's skin, its
// Blueprint's components share one, one outline around the whole actor); a Masked material cut by its alpha (masked.frag's
// plain cut): a leaf's outline, not its card's
#include "common.glsl"

layout(location = 2) flat in uint i_instance_raw;
layout(location = 3) in vec4 i_uvs; // mesh.vert: zw the materials' UV
layout(location = 0) out float o_mask;

#define MAT_SET 1
#include "mat_common.glsl"

void main() {
    uint ii = i_instance_raw & 0x7FFFFFFFu;
    FrameBuf f = FrameBuf(pc.frame);
    Instance inst = InstanceBuf(pc.instances).i[ii];
    if ((inst.ids.y & INST_MATERIAL) != 0u) {
        GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
        if ((gm.flags & MATF_MASKED) != 0u) {
            vec2 uv = i_uvs.zw;
            uint tex = (gm.flags & MATF_UDIM) != 0u ? udim_tile(f.mat_params, gm, uv).x : gm.tex_base;
            if (texture(u_mat_tex[nonuniformEXT(tex)], uv).a < float(gm.flags >> 24u) / 255.0) discard;
        }
    }
    uint h = (pc.pad >= 2u ? pc.pad : ii) * 2654435761u;
    o_mask = (pc.pass & 0x80000000u) != 0u ? float((pc.pass >> 16) & 255u) / 255.0 // (a crowd's part: its spawner's byte, hashed by render.c)
           : pc.pad == 1u ? 1.0 : float(1u + (h >> 24) % 254u) / 255.0;
}
