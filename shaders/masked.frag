#version 460
// The depth-only passes (the prepass, the sun's shadow map) of the meshes whose materials are Masked or Two Sided
// (render.c's masked runs): the baked base color's alpha under the clip value is cut, a one-sided back face too. The
// opaque meshes keep their pipelines without a fragment shader (early depth, nothing to run).
#include "common.glsl"

layout(location = 0) in vec3 i_world;
layout(location = 2) flat in uint i_instance_raw; // (Q21: the top bit, an impostor's card)
#define i_instance (i_instance_raw & 0x7FFFFFFFu)
layout(location = 3) in vec4 i_uvs; // mesh.vert: zw the materials' UV (a card: its corner)

#define MAT_SET 1
#include "mat_common.glsl"
#include "impostor.glsl" // (Q21)

void main() {
    gl_SampleMask[0] = -1; // (every sample unless the alpha says otherwise: written on every path)
    FrameBuf f = FrameBuf(pc.frame);
    Instance inst = InstanceBuf(pc.instances).i[i_instance];
    if ((i_instance_raw & 0x80000000u) != 0u) { // (Q21) an impostor's card: the atlas's coverage there
        vec3 wp;
        ImpSample is = imp_card(f, inst, i_world, i_uvs.zw, false, wp);
        uint m = alpha_coverage(is.alpha, fwidth(is.alpha), 0.5, uint(max(f.sky_params.z, 1.0)), uint(gl_FragCoord.x) * 3u + uint(gl_FragCoord.y) * 5u + i_instance);
        if (m == 0u) discard;
        gl_SampleMask[0] = int(m);
        return;
    }
    if ((inst.ids.y & INST_MATERIAL) == 0u) {
        if (!gl_FrontFacing && pc.pass != 1u) discard; // (the shadow map keeps both sides: no light leaks)
        return;
    }
    GpuMaterial gm = GpuMaterialBuf(f.materials).m[floatBitsToUint(inst.emissive_rough.x)];
    if (!gl_FrontFacing && pc.pass != 1u && (gm.flags & MATF_TWO_SIDED) == 0u) discard;
    if ((gm.flags & MATF_MASKED) == 0u) return;
    uint tex = gm.tex_base;
    if (gm.anim.x > 1u) { // animated: the frame shown now (mesh.frag blends two; the cut follows the nearer)
        float ft = fract(f.camera_pos.w / max(uintBitsToFloat(gm.anim.y), 0.05)) * float(gm.anim.x);
        tex = MatFrameBuf(f.mat_params).t[gm.anim.z + uint(ft + 0.5) % gm.anim.x].x;
    }
    // (the mips keep the cut's coverage: material.c alpha_coverage_mips)
    float a, clip = float(gm.flags >> 24u) / 255.0;
    if ((gm.flags & MATF_UDIM) != 0u) { // (Q18) the tile under the uv, read with the whole uv's gradients (no seam)
        vec2 uv = i_uvs.zw, gx = dFdx(uv), gy = dFdy(uv);
        float g = pc.pass == 1u ? 1.0 : exp2(tsr_mip_bias(f.tsr_size));
        tex = udim_tile(f.mat_params, gm, uv).x;
        a = textureGrad(u_mat_tex[nonuniformEXT(tex)], uv, gx * g, gy * g).a;
    } else a = texture(u_mat_tex[nonuniformEXT(tex)], i_uvs.zw, pc.pass == 1u ? 0.0 : tsr_mip_bias(f.tsr_size)).a;
    if (pc.pass == 1u) { // the sun's shadow map: one sample, the plain cut
        if (a < clip) discard;
        return;
    }
    // the prepass (MSAA: sky_params.z samples): the cut as coverage, the main pass then shades the samples it kept
    uint m = alpha_coverage(a, fwidth(a), clip, uint(max(f.sky_params.z, 1.0)), uint(gl_FragCoord.x) * 3u + uint(gl_FragCoord.y) * 5u + i_instance);
    if (m == 0u) discard;
    gl_SampleMask[0] = int(m);
}
