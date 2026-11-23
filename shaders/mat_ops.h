// The material bytecode (material.c compiles it, shaders/mat_vm.glsl runs it): read by C and GLSL (defines only).
// An instruction: word 0 = op | nin << 16 | nout << 24, words 1-4 the node's properties (p0..p3; a constant's four
// floats), then nin input registers and nout output registers (each a vec4: a float in x, a vector / color in xyz).
// A node's own op is its NODE_* type (tl/kernel_svm_types.h); the rest are below.
#ifndef MAT_OPS_H
#define MAT_OPS_H

#define MOP_CONST 300       // out = (p0, p1, p2, p3) as floats
#define MOP_UV 301          // out = (uv + the bump offset of the context in p0, 0)
#define MOP_NORMAL 302      // out = the shading normal, tangent space ((0, 0, 1) before a normal map)
#define MOP_VIEW 303        // out = toward the eye, tangent space (dynamic: per pixel only)
#define MOP_PARAM 304       // out = the material's parameter p0 (dynamic: a Material Instance / a Blueprint sets it)
#define MOP_LOAD_SLOT 305   // out = the baked texture p0 (a slot) at the uv
#define MOP_STORE_SLOT 306  // the bake: in 0 into the slot texture p0
#define MOP_CVT_LUM 307     // out.x = the luminance of in 0 (a color into a float socket)
#define MOP_CVT_AVG 308     // out.x = the mean of in 0's xyz (a vector into a float socket)
#define MOP_CVT_SPLAT 309   // out = in 0's x in xyz (a float into a color / vector socket)
#define MOP_CLOSURE 310     // in: base color, metallic, roughness, alpha, normal, emission color, emission strength;
                            // out: (base.rgb, metal), (emission.rgb, rough), (normal.xyz, alpha)
#define MOP_SHADER_MIX 311  // in: fac, closure A (3), closure B (3); out: a closure
#define MOP_SHADER_ADD 312  // in: closure A (3), closure B (3); out: a closure
#define MOP_OUTPUT 313      // in: a closure: the material's result
#define MOP_SET_W 315        // out = (in 0's xyz, in 1's x): the height into the closure's alpha (M7, parallax)
#define MOP_BUMP_HEIGHTS 314 // in: strength, distance, h, normal, h(+u), h(+v); p0 invert, p1 the texel (float): out normal

#define MAT_MAX_SLOTS 4     // baked intermediate textures a dynamic material reads
#define MAT_VM_REGS 32      // registers of the per-pixel program (the bake has MAT_BAKE_REGS)
#define MAT_BAKE_REGS 64

// GpuMaterial.flags
#define MATF_PROGRAM 1u     // a per-pixel program runs (else the baked textures are the result)
#define MATF_EMISSIVE 2u    // the emission texture exists
#define MATF_WORLD_GRID 4u  // the engine's grid material: a world-aligned grid over the base color (mesh.frag)
#define MATF_MASKED 8u      // Masked: the baked alpha below the clip value (bits 24-31, / 255) is cut
#define MATF_TWO_SIDED 16u  // Two Sided: back faces drawn, their normal flipped (the masked pipelines, mesh.frag)
#define MATF_TRANSLUCENT 64u // Translucent: the baked alpha is the opacity, drawn after the opaque scene (render.c's OIT pass)
#define MATF_WORLD_UV 32u   // World Aligned (Unreal's WorldAlignedTexture): UV from the world position on the dominant axis, uv_scale cm a tile
// e6.w of the GpuMatExt (MATF_EXT): what of it is in use (M5)
#define MATX_LOBES 1u     // the Principled's extra lobes in e0-e5 (else only M5's: the plain BRDF)
#define MATX_VCOL_TINT 2u // the Color Attribute multiplies the Base Color
#define MATX_LAYERS 4u
#define MATX_INTERIOR 16u // (2026-11-17, Q4) the Interior Mapping node: e7 (the atlas), e8-e10, e11.x (mat_common.glsl, mesh.frag interior)
#define MATX_WET 8u       // (2026-11-16) the Wet Surface node: e8-e11 (mat_common.glsl)    // (B11) the landscape's painted layers: e3.xyz their material slots, e5.xyz their tiles (cm); the vertex color their weights
#define MATF_POM 256u        // parallax occlusion (M7): the baked base's alpha is the height (opaque only), e7 its depth
#define MATF_REFRACT 1024u   // (2026-11-16) refractive glass (Translucent / Transmission): what is behind seen bent by its normal; e5.w its IOR, e7.x its thickness (cm), e7.y the dispersion, e7.z the strength
#define MATF_EYE 512u        // the Eye shading model (A9.3c): the iris refracted under the cornea (a coat), e7.xy its radius and depth
#define MATF_TOON 2048u     // (Q11) the Toon shading model: its block after the GpuMatExt table (toon.glsl GpuToon), lit in bands (mesh.frag)
#define MATF_UDIM 4096u     // (Q18) UDIM: a tile per unit square of the UV, anim.w its table in the parameters (mesh.frag udim_tile)
#define MATF_TESS 8192u     // (Q17) Nanite Tessellation (Enable Tessellation): the baked base's alpha displaces its DAG clusters, diced on the GPU (tess.glsl); GpuMaterial anim.z its magnitude (a still one: anim.z is the flipbook's) (cm) and center (two halfs)
#define MATF_EXT 128u       // the Principled BSDF's extra lobes (GpuMaterial.ext: coat, anisotropy, subsurface, specular tint / IOR, transmission)

#endif
