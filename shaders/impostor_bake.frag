#version 460
// (Q21) An impostor's bake (farview.c, impostor_bake.vert): the material's baked base color (sRGB, as the materials keep
// it) and its alpha cut; its normal map in mesh space (the tangent frame from the derivatives, as mesh.frag), packed
// octahedral, the depth along the frame's view and the roughness
#include "common.glsl"
#extension GL_EXT_nonuniform_qualifier : require
#define MAT_SET 1
#include "mat_common.glsl"

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ImpBakeBuf { vec4 sphere; uvec4 info; uvec2 materials; };

layout(location = 0) in vec3 i_pos;
layout(location = 1) in vec3 i_normal;
layout(location = 2) in vec2 i_uv;
layout(location = 3) flat in vec3 i_dir;
layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_nd;

void main() {
    ImpBakeBuf B = ImpBakeBuf(pc.frame);
    uint slot = B.info.y; // (0xFFFFFFFF: no material, a white base the instance's color multiplies)
    vec3 n = normalize(i_normal);
    if (dot(n, i_dir) < 0.0) n = -n; // (two-sided leaves: the face toward the camera)
    vec4 tb = vec4(1.0), tn = vec4(0.5, 0.5, 0.6, 0.0);
    if (slot != 0xFFFFFFFFu) {
        GpuMaterial gm = GpuMaterialBuf(B.materials).m[slot];
        vec2 uv = i_uv * uintBitsToFloat(B.info.z);
        tb = texture(u_mat_tex[nonuniformEXT(gm.tex_base)], uv);
        tn = texture(u_mat_tex[nonuniformEXT(gm.tex_nrm)], uv);
        if ((gm.flags & MATF_MASKED) != 0u && tb.a < float(gm.flags >> 24u) / 255.0) discard;
        vec3 dp1 = dFdx(i_pos), dp2 = dFdy(i_pos);
        vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
        vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x, Bt = dp2perp * duv1.y + dp1perp * duv2.y;
        float im = inversesqrt(max(max(dot(T, T), dot(Bt, Bt)), 1e-20));
        vec3 nts = vec3(tn.xy * 2.0 - 1.0, 0.0);
        nts.z = sqrt(max(1.0 - dot(nts.xy, nts.xy), 0.0));
        vec3 nm = normalize(mat3(T * im, Bt * im, n) * nts);
        if (dot(nm, nm) > 0.5) n = nm;
    }
    o_albedo = vec4(tb.rgb, 1.0);
    o_nd = vec4(oct_encode_f(n) * 0.5 + 0.5, gl_FragCoord.z, clamp(tn.z, 0.045, 1.0));
}
