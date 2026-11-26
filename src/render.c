#include "io_json.h" // (2026-11-24: a mesh's .rlod settings)
#include "render.h"
#include "trace.h"
#include "project.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "atmosphere.h"
#include "crowd.h"
#include "gi.h"
#include "sw_rt.h"
#include "anim.h"
#include "particles_gpu.h"
#include "gi_bake.h"
#include "gi_cache.h"
#include "vsm.h"
#include "lsm.h"
#include "weather.h"
#include "gi_fallback.h"
#include "water.h"
#include "cloth.h"
#include "groom.h"
#include "fire.h"
#include "splat.h"
#include "lights.h"
#include "mesh_build.h"
#include "material.h"
#include "udim.h" // (Q18) the UDIM tiles' streaming: stream_note_udim
#include "mem.h"
#include "debug_draw.h" // (2026-11-19) the debug lines and points drawn after the composite
#include "render_gpu.h"
#include "crashlog.h"
#include "platform.h"
#include "platform_files.h"
#include "post.h"
#include "vkctx.h"
#include "async.h"
#include "i18n.h"
#include "upscale.h"
#include "upscale_vendor.h"
#include "decal.h"
#include "tess.h"
#include "farview.h" // (Q21) the distant view
#include "outline.h" // (Q11) the outline pass
#include "spline.h"  // (Q14) the Spline Mesh pieces bent by skin.comp

// (Q22) a pipeline destroyed takes its tess twin with it (tess_twin_put: pipeline() made it; the same frames used both)
static void pipe_destroy_twin(VkDevice dev, VkPipeline p, const VkAllocationCallbacks* a) {
    VkPipeline t = tess_twin_take(p);
    if (t) vkDestroyPipeline(dev, t, a);
    vkDestroyPipeline(dev, p, a);
}
#define vkDestroyPipeline pipe_destroy_twin

// SPIR-V compiled at build time (CMake: glslc -mfmt=c)
static const uint32_t s_mesh_vert[] =
#include "mesh.vert.inc"
    ;
static const uint32_t s_tess_vert[] = // (Q22) Nanite Tessellation's micro vertices: the tess twins' vertex shader
#include "tess.vert.inc"
    ;
static const uint32_t s_tess_vert_x[] = // (... the RE_EXT pipelines' twins: the vertex color out)
#include "tess_x.vert.inc"
    ;
static const uint32_t s_mesh_frag[] =
#include "mesh.frag.inc"
    ;
static const uint32_t s_mesh_frag_lsm[] = // the same with the local lights' shadow maps (RE_LSM: lsm.c, while one has a map)
#include "mesh_lsm.frag.inc"
    ;
// ... with the Principled BSDF's extra lobes (RE_EXT, M1b: the frames a visible material has them; R.any_ext)
static const uint32_t s_mesh_frag_x[] =
#include "mesh_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_x_toon[] = // (Q11: RE_TOON)
#include "mesh_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_lsm_x[] = // (Q11) RE_LSM and RE_EXT together: the local lights' maps and SSR with a Toon material
#include "mesh_lsm_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_lsm_x_toon[] = // (Q11: RE_TOON)
#include "mesh_lsm_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_vert_x[] = // (its vertex shader: the vertex color out, M5)
#include "mesh_x.vert.inc"
    ;
static const uint32_t s_mesh_frag_rt_x[] =
#include "mesh_rt_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x[] =
#include "mesh_rt_t_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_t_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_a[] =
#include "mesh_rt_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_x_a_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_a[] =
#include "mesh_rt_t_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_t_x_a_toon.frag.inc"
    ;
// ... and the masked runs' with them (2026-11-10: Masked / Two Sided materials with coat, anisotropy, subsurface, IOR;
// while R.any_ext_masked): raster, ray traced (+ terminator), each with the shadow rays' alpha test and the denoiser's
static const uint32_t s_mesh_frag_masked_x[] =
#include "mesh_masked_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_masked_x_toon[] = // (Q11: RE_TOON)
#include "mesh_masked_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_masked_eq_x[] =
#include "mesh_masked_eq_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_masked_eq_x_toon[] = // (Q11: RE_TOON)
#include "mesh_masked_eq_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x[] =
#include "mesh_rt_masked_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_a[] =
#include "mesh_rt_masked_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_x_a_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_d[] =
#include "mesh_rt_masked_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_a_d[] =
#include "mesh_rt_masked_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x[] =
#include "mesh_rt_masked_eq_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_a[] =
#include "mesh_rt_masked_eq_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_x_a_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_d[] =
#include "mesh_rt_masked_eq_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_a_d[] =
#include "mesh_rt_masked_eq_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x[] =
#include "mesh_rt_masked_t_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_t_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_a[] =
#include "mesh_rt_masked_t_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_t_x_a_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_d[] =
#include "mesh_rt_masked_t_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_t_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_a_d[] =
#include "mesh_rt_masked_t_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_t_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x[] =
#include "mesh_rt_masked_eq_t_x.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_t_x_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_a[] =
#include "mesh_rt_masked_eq_t_x_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_a_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_t_x_a_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_d[] =
#include "mesh_rt_masked_eq_t_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_t_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_a_d[] =
#include "mesh_rt_masked_eq_t_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_masked_eq_t_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt[] = // the same with traced shadows and reflections (RE_RT): GPUs with ray queries
#include "mesh_rt.frag.inc"
    ;
static const uint32_t s_mesh_frag_masked[] = // the alpha cut and two-sided back faces (RE_MASKED: the masked runs)
#include "mesh_masked.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked[] =
#include "mesh_rt_masked.frag.inc"
    ;
static const uint32_t s_omm_comp[] = // (the opacity micromaps' data: omm.comp)
#include "omm.comp.inc"
    ;
static const uint32_t s_tsr_comp[] = // (temporal super resolution: tsr.comp)
#include "tsr.comp.inc"
    ;
static const uint32_t s_mesh_frag_tl[] = // (RE_TRANSLUCENT: the OIT pass)
#include "mesh_tl.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_tl[] =
#include "mesh_rt_tl.frag.inc"
    ;
static const uint32_t s_mesh_frag_masked_eq[] = // (RE_MASKED_EQ: after the prepass, early depth EQUAL, no cut)
#include "mesh_masked_eq.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq[] =
#include "mesh_rt_masked_eq.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t[] = // (RE_TERMINATOR: the imported meshes' runs, ray traced)
#include "mesh_rt_t.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t[] =
#include "mesh_rt_masked_t.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t[] =
#include "mesh_rt_masked_eq_t.frag.inc"
    ;
// the ray traced ones again with the shadow rays' alpha test (RT_ALPHA): picked once a Masked mesh is drawn
static const uint32_t s_mesh_frag_rt_a[] =
#include "mesh_rt_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_a[] =
#include "mesh_rt_masked_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_a[] =
#include "mesh_rt_masked_eq_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_a[] =
#include "mesh_rt_t_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_a[] =
#include "mesh_rt_masked_t_a.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_a[] =
#include "mesh_rt_masked_eq_t_a.frag.inc"
    ;
// ... and the ray traced ones writing the unified denoiser's targets (RE_DEN: den_rt, the Photoreal styles)
static const uint32_t s_mesh_frag_rt_d[] =
#include "mesh_rt_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_a_d[] =
#include "mesh_rt_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_d[] =
#include "mesh_rt_masked_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_a_d[] =
#include "mesh_rt_masked_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_d[] =
#include "mesh_rt_masked_eq_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_a_d[] =
#include "mesh_rt_masked_eq_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_d[] =
#include "mesh_rt_t_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_a_d[] =
#include "mesh_rt_t_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_d[] =
#include "mesh_rt_masked_t_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_t_a_d[] =
#include "mesh_rt_masked_t_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_d[] =
#include "mesh_rt_masked_eq_t_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_masked_eq_t_a_d[] =
#include "mesh_rt_masked_eq_t_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_d[] =
#include "mesh_rt_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_a_d[] =
#include "mesh_rt_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_d[] =
#include "mesh_rt_t_x_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_t_x_d_toon.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_a_d[] =
#include "mesh_rt_t_x_a_d.frag.inc"
    ;
static const uint32_t s_mesh_frag_rt_t_x_a_d_toon[] = // (Q11: RE_TOON)
#include "mesh_rt_t_x_a_d_toon.frag.inc"
    ;
static const uint32_t s_masked_frag[] = // the masked runs' depth-only passes (prepass, shadow)
#include "masked.frag.inc"
    ;
static const uint32_t s_fullscreen_vert[] =
#include "fullscreen.vert.inc"
    ;
static const uint32_t s_sky_frag[] =
#include "sky.frag.inc"
    ;
static const uint32_t s_grid_frag[] =
#include "grid.frag.inc"
    ;
static const uint32_t s_debug_line_vert[] = // (2026-11-19) the debug lines (debug_draw.h)
#include "debug_line.vert.inc"
    ;
static const uint32_t s_debug_line_frag[] =
#include "debug_line.frag.inc"
    ;
static const uint32_t s_mask_frag[] =
#include "mask.frag.inc"
    ;
static const uint32_t s_mask_hair_frag[] = // (2026-11-25, item 17) a selected actor's hair in the mask
#include "mask_hair.frag.inc"
    ;
static const uint32_t s_composite_frag[] =
#include "composite.frag.inc"
    ;
static const uint32_t s_composite_post_frag[] = // (RE_POST: the depth of field and motion blur gather)
#include "composite_post.frag.inc"
    ;
static const uint32_t s_exposure_comp[] =
#include "exposure.comp.inc"
    ;
static const uint32_t s_cull_comp[] =
#include "cull.comp.inc"
    ;
static const uint32_t s_tlas_fill_comp[] =
#include "tlas_fill.comp.inc"
    ;
static const uint32_t s_hiz_comp[] =
#include "hiz.comp.inc"
    ;
static const uint32_t s_light_grid_comp[] =
#include "light_grid.comp.inc"
    ;
static const uint32_t s_velocity_vert[] =
#include "velocity.vert.inc"
    ;
static const uint32_t s_velocity_crowd_vert[] = // (the crowds' parts in the velocity pass: velocity_crowd.vert)
#include "velocity_crowd.vert.inc"
    ;
static const uint32_t s_velocity_frag[] =
#include "velocity.frag.inc"
    ;
static const uint32_t s_bloom_comp[] =
#include "bloom.comp.inc"
    ;
static const uint32_t s_crowd_comp[] =
#include "crowd.comp.inc"
    ;
static const uint32_t s_skin_comp[] = // the skins bent by their bones (render.c "skin")
#include "skin.comp.inc"
    ;
static const uint32_t s_particle_sim_comp[] = // the particles (particles.c): simulation, cull, sprites, meshes
#include "particle_sim.comp.inc"
    ;
static const uint32_t s_particle_vert[] =
#include "particle.vert.inc"
    ;
static const uint32_t s_particle_frag[] =
#include "particle.frag.inc"
    ;
static const uint32_t s_ribbon_vert[] = // (B10) the particles' ribbons
#include "ribbon.vert.inc"
    ;
static const uint32_t s_ribbon_frag[] =
#include "ribbon.frag.inc"
    ;
static const uint32_t s_particle_point_vert[] =
#include "particle_point.vert.inc"
    ;
static const uint32_t s_particle_point_frag[] =
#include "particle_point.frag.inc"
    ;
static const uint32_t s_particle_mesh_vert[] =
#include "particle_mesh.vert.inc"
    ;
static const uint32_t s_particle_mesh_frag[] =
#include "particle_mesh.frag.inc"
    ;
static const uint32_t s_water_surface_vert[] = // the water bodies (water.c)
#include "water_surface.vert.inc"
    ;
static const uint32_t s_water_surface_frag[] =
#include "water_surface.frag.inc"
    ;
static const uint32_t s_water_surface_frag_rt[] = // the bottom through shallow water, shore foam (RE_RT)
#include "water_surface_rt.frag.inc"
    ;
static const uint32_t s_cloth_vert[] = // the cloths (cloth.c)
#include "cloth.vert.inc"
    ;
static const uint32_t s_cloth_frag[] =
#include "cloth.frag.inc"
    ;
static const uint32_t s_splat_vert[] = // the Gaussian splats (splat.c, A9.5d)
#include "splat.vert.inc"
    ;
static const uint32_t s_splat_frag[] =
#include "splat.frag.inc"
    ;
static const uint32_t s_hair_vert[] = // the hair in strands (groom.c, A9.3b): one per count of the main pass's targets
#include "hair.vert.inc"
    ;
static const uint32_t s_cloth_vel_vert[] = // (2026-11-15) the cloth's motion in the velocity pass
#include "cloth_vel.vert.inc"
    ;
static const uint32_t s_hair_vel_vert[] = // (2026-11-15) the hair's motion in the velocity pass
#include "hair_vel.vert.inc"
    ;
static const uint32_t s_hair_frag[] =
#include "hair.frag.inc"
    ;
static const uint32_t s_hair_depth_frag[] = // (its depth first: the coverage's cut; then each pixel shaded once)
#include "hair_depth.frag.inc"
    ;
static const uint32_t s_hair_frag_o2[] =
#include "hair_o2.frag.inc"
    ;
static const uint32_t s_hair_frag_o4[] =
#include "hair_o4.frag.inc"
    ;
static const uint32_t s_rain_vert[] = // the rain's drops and splashes (water.c)
#include "rain.vert.inc"
    ;
static const uint32_t s_rain_frag[] =
#include "rain.frag.inc"
    ;
static const uint32_t s_ar_plane_vert[] = // AR's real floor receiving the shadows (ar_arcore.c)
#include "ar_plane.vert.inc"
    ;
static const uint32_t s_ar_plane_frag[] =
#include "ar_plane.frag.inc"
    ;
static const uint32_t s_ar_occlude_frag[] = // AR's occlusion: the picture with the real depth (ar.glsl)
#include "ar_occlude.frag.inc"
    ;
static const uint32_t s_shadow_split_comp[] = // the area lights' shadows filtered (GI_SHADOW_SPLIT)
#include "shadow_split.comp.inc"
    ;
static const uint32_t s_denoise_comp[] = // the unified ray traced denoiser (GI_DENOISE, A9.1d)
#include "denoise.comp.inc"
    ;
static const uint32_t s_den_trace_comp[] = // the unified denoiser's rays, after the main pass (den_trace.comp)
#include "den_trace.comp.inc"
    ;
static const uint32_t s_gtao_comp[] = // the screen space AO on the indirect light (GI_GTAO, A4a)
#include "gtao.comp.inc"
    ;
static const uint32_t s_sss_comp[] = // the skin's screen space subsurface scattering (GI_SSS, A9.3a)
#include "sss.comp.inc"
    ;
// (Q25) the VR's fixed foveated rendering wants the rate image too (render_want_foveation, before render_init)
static bool g_foveation_wanted;
void render_want_foveation(bool on) { g_foveation_wanted = on; }
static bool vrs_wanted(void) { return project()->vrs || getenv("REALENGINE_VRS") != NULL || g_foveation_wanted; }
static const uint32_t s_vrs_comp[] = // the variable rate shading's rate image (A9.7b)
#include "vrs.comp.inc"
;
static const uint32_t s_vrs_fov_comp[] = // (Q25) its VR foveation alone (RE_VRS_FOVEATED)
#include "vrs_fov.comp.inc"
    ;
static const uint32_t s_ups_mv_comp[] = // the vendors' upscalers' motion vectors (upscale_vendor.c)
#include "ups_mv.comp.inc"
    ;
static const uint32_t s_decal_bin_comp[] = // the decals' 16 px tiles (M6, decal.c)
#include "decal_bin.comp.inc"
    ;

const char* const g_view_mode_names[VIEW_MODE_COUNT] = { "Lit", "Unlit", "Wireframe", "Lighting Only", "World Normal",
                                                         "Solid", "Solid (Random)", "Texture", "Roughness", "Metallic",
                                                         "Path Tracing", "GI Only", "LOD", "Triangles", "Mobility",
                                                         "Surface Cache", "GI Source", "Lightmap Density",
                                                         "Light Complexity", "Shader Cost", "Object Cost" };

#define TS_PER_FRAME 34 // 32-33 the distant view's horizon pass (Q21: when the sun turned); 30-31 the Nanite Tessellation's dicing (Q17); 28-29 a Build Lighting's share (Q16: its budget in the game); 26-27 the skin's scattering (A9.3a); 0-7 every frame; 8-13 with the Debug Viewer open (render_set_detail); 14 the post's split; 16-17 the skins (when bent); 18-19 the local lights' shadow maps (when drawn); 20-21 the screen space AO; 22-23 the upscale (TSR or a vendor's); 24-25 the unified denoiser

// A shadow caster changed: the sun's virtual shadow maps and the local lights' maps both keep a cache
static void shadows_dirty_box(const float mn[3], const float mx[3]) { vsm_dirty_box(mn, mx), lsm_dirty_box(mn, mx); }
static void shadows_dirty_static(const float mn[3], const float mx[3]) { vsm_dirty_static(mn, mx), lsm_dirty_static(mn, mx); }
static void shadows_invalidate(void) { vsm_invalidate(), lsm_invalidate(); }
#define MAX_LODS 4
#define NCMD (RE_MAX_MESHES * MAX_LODS) // the indirect draws' room: one per mesh and LOD (R.ncmd of them in use)
#define DEG2RAD 0.017453292519943295f

// The velocity pass's entries (shaders/velocity.vert): a moved actor's slot and last frame's rows (key: its mesh * MAX_LODS
// + the LOD the GPU cull draws it with, the draws' sort)
typedef struct VelEntry {
    uint32_t slot, key, pad[2];
    float rows[3][4];
} VelEntry;
#define VEL_MAX 65536 // moved actors drawn into the velocity at most (the rest blur with the camera only)
#define BLOOM_LEVELS 6

typedef struct Mesh {
    uint32_t first_index, index_count;
    int32_t vertex_offset;
    float mn[3], mx[3];
} Mesh;

static struct {
    VkDevice dev;
    GpuBuf vertices, indices;
    GpuVertex* cpu_v;
    uint32_t* cpu_i;
    uint32_t vertex_count;
    // The skins (RMESH_SKINNED, "skin" below): their own vertices after the meshes' (skin_room of them from vertex_count
    // on), the instances that hold them, this frame's palettes and jobs
    uint32_t skin_room, skin_room_need;
    uint32_t skin_short; // (Q14) this frame's skins that found no room (their vertices): one reload for all of them
    bool mesh_skinned[RE_MAX_MESHES];
    // (2026-11-24, item 4) a skin whose every vertex hangs on one bone (its weight on it whole, no morph targets): drawn as
    // the mesh moved by that bone's palette row (render.c skin_rigid), never bent: no room, no BLAS of its own, its LODs
    // and the static paths like any mesh, animated or not (-1: a real skin)
    int16_t mesh_rigid_bone[RE_MAX_MESHES];
    uint8_t mesh_collision[RE_MAX_MESHES]; // (2026-11-24, item 4) the asset's Collision Complexity (.rlod: 0 the actor's, 1 simple, 2 complex)
    struct SkinSlot* skins;
    uint32_t nskins, skins_cap;
    // (2026-11-24, the Unamed scene: 2080 still Skeletal Meshes) each scene slot's live skin (its place in R.skins,
    // UINT32_MAX none) and the skins drawn at their bind (no room, or a pose that is the bind's): no search, no retry.
    // skin_gen moves when room may have come back (a load, a skin freed); skin_nv: each mesh's LOD 0 vertices, once a load
    uint32_t* skin_map;
    struct SkinRaw* skin_raw;
    uint32_t skin_map_cap, skin_gen, skin_raw_n, skin_noroom_n;
    uint32_t skin_rigid_n; // (2026-11-24) the rigid skins sent this frame (drawn as their mesh moved by their bone)
    uint32_t dorm_n, dorm_slept, dorm_woke;
    float last_cam[3]; // (2026-11-24) the last frame's camera (anim.c's update rate by distance)
    bool have_cam; // (2026-11-24) the dormant dynamic instances this frame; slept and woke since the start
    uint32_t skin_nv[RE_MAX_MESHES];
    GpuBuf skin_pal[RE_FRAMES_IN_FLIGHT], skin_hdr[RE_FRAMES_IN_FLIGHT];
    uint32_t skin_pal_cap, skin_hdr_cap, nskin_jobs, skin_pal_used, skin_verts;
    uint32_t* skin_jobs;
    VkPipeline skin_pipe;
    double skin_ms;
    // The morph targets (RMESH_MORPHS, "skin" below): every mesh's CSR (a word per vertex + 1, its deltas' first) and
    // deltas (RMeshMorphDelta) in one device buffer; per mesh slot their word offsets, its target count and names
    GpuBuf morphs;
    uint32_t morph_first[RE_MAX_MESHES], morph_delta[RE_MAX_MESHES], morph_count[RE_MAX_MESHES], morph_name0[RE_MAX_MESHES];
    uint32_t vcol_first[RE_MAX_MESHES]; // each mesh's vertex colors in R.morphs' words (RMESH_COLORS; UINT32_MAX: none)
    uint32_t tess_first[RE_MAX_MESHES]; // (Q17) each DAG mesh's canonical table in the same words (tess.c; UINT32_MAX: none)
    bool any_tess;                      // (Q17) an instance sent with a displaced material (Enable Tessellation)
    bool tess_twins, tess_rebuild;      // (Q22) the tess twins made with the pipelines from now on (tess.vert); made again once
    uint32_t imp_card;                  // (Q21) the impostors' card's slot (UINT32_MAX: none)
    char (*morph_names)[RMESH_MORPH_NAME];
    uint32_t morph_meshes, morph_jobs, morph_deltas; // (the stats: meshes with morphs; last frame's jobs with a weight)
    bool morph_off;                                  // (the measure's: the morphs left out of the bends)
    bool skin_ts[RE_FRAMES_IN_FLIGHT];               // the slot's frame timed its skins (16-17)
    bool gtao_ts[RE_FRAMES_IN_FLIGHT];               // ... its screen space AO (20-21)
    bool ups_ts[RE_FRAMES_IN_FLIGHT];                // ... its upscale (22-23)
    bool den_ts[RE_FRAMES_IN_FLIGHT];                // ... its unified denoiser (24-25)
    bool sss_ts[RE_FRAMES_IN_FLIGHT];                // ... its skin's scattering (26-27)
    bool tess_ts[RE_FRAMES_IN_FLIGHT];               // ... (Q17) the Nanite Tessellation's dicing (30-31)
    bool far_ts[RE_FRAMES_IN_FLIGHT];                // ... (Q21) the distant view's horizon pass (32-33)
    uint32_t bake_rows[RE_FRAMES_IN_FLIGHT];         // (Q16) ... a bake's rows of the atlas, timed (28-29; 0: none)
    bool lsm_ts[RE_FRAMES_IN_FLIGHT];                // ... its local lights' shadow maps (18-19)
    VkPipeline part_sprite, part_mesh, part_point; // the particles' draws in the main pass (particles.c)
    VkPipeline part_ribbon;                       // (B10) their ribbons
    VkPipeline water_surf, rain; // the water bodies (opaque) and the rain (blended) in the main pass (water.c)
    VkPipeline cloth; // the cloths (cloth.c), opaque, two-sided
    VkPipeline hair, hair_depth; // the hair in strands (groom.c): its depth with the coverage's cut, then shaded on it (EQUAL)
    VkPipeline splat_pipe;       // the Gaussian splats (splat.c): one sample, blended back to front, the scene's depth tested
    double water_time; // (the water's clock: the last frame's time)
    uint32_t water_frame;
    Mesh meshes[RE_MAX_MESHES];
    Mesh lods[RE_MAX_MESHES][MAX_LODS];
    uint32_t lod_count[RE_MAX_MESHES];
    float lod_error[RE_MAX_MESHES][MAX_LODS]; // the chord's sagitta against LOD 0's (cm): GpuMeshInfo.lod_error
    // the meshes: the basic shapes, then the imported ones (.rmesh in Content: their id is their vpath's hash | 1 << 31)
    uint32_t nmeshes, ncmd;
    uint32_t mesh_id[RE_MAX_MESHES];
    float mesh_area[RE_MAX_MESHES];     // cm² at scale 1 (the surface cache's chart sizes)
    // a UV2 to bake into (a lightmap, a surface cache chart; RMESH_NO_ATLAS: the welded heavy meshes); the LOD picked by
    // its geometric error on screen (the imported meshes' simplified LODs, mesh_build.c; the shapes: by their radius)
    bool mesh_atlas[RE_MAX_MESHES], mesh_error_lod[RE_MAX_MESHES];
    char mesh_name[RE_MAX_MESHES][64];
    char mesh_vpath[RE_MAX_MESHES][160]; // (render_mesh_vpath: the de-lighting finds the mesh's file)
    GpuBuf frame[RE_FRAMES_IN_FLIGHT];
    GpuBuf frame_dev;  // the frame slot's copy in device memory (what the shaders read)
    GpuBuf mesh_info; // GpuMeshInfo per shape (the cull, the TLAS)
    // The big meshes' clusters (mesh_build.c mesh_clusters; cull.comp pass 6, cluster_record): GpuCluster each, per mesh
    // and LOD its range; the work (dispatch, a draw per instance) and the visible clusters' indices
    GpuBuf clusters, cl_work, cl_idx, cl_perm;
    uint32_t cluster_first[RE_MAX_MESHES][MAX_LODS], cluster_count[RE_MAX_MESHES][MAX_LODS], cl_idx_cap;
    uint32_t dag_first[RE_MAX_MESHES], dag_count[RE_MAX_MESHES]; // the cluster LOD DAGs (mesh_clod.h; in R.clusters)
    bool any_dag, hiz_have; // hiz_have: the Hi-Z holds last frame's depth (the DAGs' first phase tests against it)
    // Opacity micromaps (omm_update, gi_blas_omm): per Masked mesh its material's slot + 1 and tiling (the first seen),
    // whether another one draws it too (then none: one micromap per BLAS), the frame first seen, done, has one
    uint32_t omm_slot[RE_MAX_MESHES], omm_seen[RE_MAX_MESHES];
    float omm_uv[RE_MAX_MESHES];
    bool omm_bad[RE_MAX_MESHES], omm_done[RE_MAX_MESHES], mesh_omm[RE_MAX_MESHES];
    VkPipeline omm_pipe;
    float hiz_vp[16];       // the view (unjittered view_proj) that built it (another viewport's: not this one's last frame)
    GpuCluster* cpu_cl;
    uint32_t* cpu_perm; // (6 per cluster: the view orders, mesh_clusters)
    uint32_t ncl, cl_cap;

    // The persistent scene (notas-dev/escala.md): a GpuInstance per entity slot on the GPU, sent again only for what
    // changed (the ECS's change log, the selection) and for the dynamic set (Movable or rotating actors) each frame
    GpuBuf scene, vis, visible_main, visible_shadow, tlas_desc; // device: slots, cull results, TLAS instances
    GpuBuf visible_late, vis_prev;    // two-phase occlusion: the late list, last frame's visibility per slot
    uint32_t scene_cap, scene_count;  // slots allocated / in use (the world's high-water mark)
    uint32_t seen_revision, seen_changes;
    bool scene_valid, scene_editor;   // scene_editor: built with the editor's hidden actors left out
    uint32_t ss_arch, ss_row;         // the texture streaming's sweep: where it goes on next frame (stream_sweep)
    double ss_pass;                   // ... when its last pass ended (one each 0.1 s at most)
    uint8_t* slot_state;              // per slot: SLOT_ALIVE, SLOT_DYNAMIC, SLOT_UNBUILT
    uint32_t* moved_at;               // per slot: the frame its transform last changed (a Movable actor: INST_MOVING a while)
    uint32_t* vsm_hash;               // per slot: a static caster's rows hashed (0: none): did it move? (the virtual shadow maps)
    float (*vsm_box)[6];              // (Q15) per slot: that caster's box when hashed: a destroyed one dirties where it was, not every level
    uint32_t frame_no;
    uint32_t *sent_frame, *sent_at, sent_cap; // per slot: the frame it was last staged and where (scene_prepare's SEND)
    // The surface cache's events: where a Movable actor started or stopped moving (its rays begin or stop seeing it),
    // each until frame event_until (GpuFrame.cache_events)
    float events[8][4];
    uint32_t event_until[8], nevents;
    uint32_t* dyn;                    // the dynamic set's slots
    uint32_t ndyn, dyn_cap;
    GpuInstance* dyn_inst;            // ... and what was last sent for each (an unmoved one isn't sent again)
    bool dyn_changed;                 // the dynamic set changed this frame: its TLAS is rebuilt
    GpuBuf dyn_desc[RE_FRAMES_IN_FLIGHT]; // its TLAS's instances (host memory, per frame slot)
    uint32_t dyn_desc_cap, dyn_built, dyn_refits; // (refits since the dynamic TLAS was last built)
    uint64_t dyn_members;                 // the hash of its members (slot, BLAS) when written last
    VkDeviceAddress tlas_dyn;
    GpuInstance sel_copy;             // what was last sent for the selection
    uint32_t sel_slot;
    uint32_t unbuilt;
    float static_mn[3], static_mx[3]; // the shadow casters' bounds among the static slots
    float dyn_mn[3], dyn_mx[3];       // every live dynamic instance's bounds (rt.glsl: rays that miss them skip its TLAS)
    GpuBuf stage[RE_FRAMES_IN_FLIGHT]; // per frame: the instances sent this frame
    uint32_t stage_cap;
    VkBufferCopy* regions;            // this frame's copies (frame arena)
    uint32_t nregions, nhot;
    GiTlasWork tlas_work;
    uint64_t scene_version;           // bumps on every change sent (the path tracer starts over)
    Entity last_selected;
    // the GPU cull: the indirect draws (main, shadow), their counts and cursors; a copy of the counts per frame slot
    VkPipeline cull_pipe, tlas_fill_pipe;
    GpuBuf draws, cull_read[RE_FRAMES_IN_FLIGHT];
    GpuBuf cl_read[RE_FRAMES_IN_FLIGHT]; // the clusters' draws of that frame (3 arrays): their triangles in the stats
    uint64_t dag_tris[2];                 // the DAGs' triangles that frame: first phase, second (REALENGINE_DAG_STATS)
    bool dag_occ;                         // the DAGs' two-phase occlusion on (their triangles many: see hiz_pad)

    GpuImage color_ms, depth_ms, depth_resolve, hdr, mask, mask_depth; // mask_depth: the nearest selected object wins
    // the shadow split (mesh.frag o_split, shadow_split.comp): the main pass's second target (MSAA, resolved into split),
    // the filter's horizontal means; split_on: this frame's (an area light can be traced, the Lit views)
    GpuImage split_ms, split, split_sum;
    // the many lights' visibility over the frames (shadow_split.comp pass 1: reprojected, ping-pong; render size)
    GpuImage ml_hist[2];
    uint32_t ml_frame;
    bool ml_reset;
    VkPipeline split_pipe;
    // the unified ray traced denoiser (A9.1d, denoise.comp; the Photoreal styles): the main pass's third and fourth
    // targets (mesh.frag o_den, o_guide), last frame's guide, the histories (2 layers: ping-pong), the blur's two.
    // Made the first frame it runs (den_alloc; 32 + 48 bytes a render pixel), dropped with the other targets
    GpuImage den, den_guide, den_guide_prev, den_hd, den_hs, den_hv, den_ta, den_tb, den_key, den_hst;
    GpuBuf rgi[2]; // (2026-11-14) ReSTIR GI's samples (32 B a render pixel: diffuse and specular; [1] unused)
    VkPipeline den_pipe, den_pipes[3], den_trace_pipe; // (den_pipes: one a pass, its code alone: denoise.comp's constant_id 0)
    // the ray traced main pass's pipelines that write the denoiser's targets (mesh.frag DEN_OUT): made the first frame
    // it runs, again when the set they copy changes (its MSAA, the alpha test, the extra lobes); in place of the plain
    // ones while it runs (den_rt_key: what they were made for)
    VkPipeline den_rt[12]; // (RT_PIPES)
    uint32_t den_rt_key;
    bool den_ok, den_on, den_alloc, den_reset; // den_ok: the main pass has the two targets (as split_ok)
    uint32_t den_frame, den_w, den_h;
    uint32_t den_prev_w, den_prev_h; // the render's size last frame (denoise.comp pass 1)
    float den_prev_exp; // the vendor upscalers' pre-exposure last frame (FrameBuf.ups_exposure)
    VkPipeline gtao_pipe; // (gtao.comp)
    // the skin's screen space scattering (A9.3a, sss.comp): its pass's image (made when a subsurface material is first
    // drawn at this size; cleared once: the pass leaves it zeroed), on this frame, a subsurface material sent
    VkPipeline sss_pipe;
    GpuImage sss;
    bool sss_alloc, sss_clear, sss_on, any_sss;
    uint32_t sss_w, sss_h;
    VkPipeline vrs_pipe;  // (vrs.comp)
    VkPipeline vrs_fov_pipe; // (Q25: vrs.comp RE_VRS_FOVEATED, the VR's foveation)
    GpuImage vrs_img;     // the rate image: a texel per vk->vrs_texel px of the render (R8_UINT)
    bool vrs_on;          // this frame's main pass shades by it
    uint32_t fov_level;   // (Q25) its rates by VR's fixed foveation this frame (0: by the content)
    bool vrs_built;       // the pipelines were made for it (the attachment's flag): wanted when they were made
    bool gtao_on;         // this frame's screen space AO
    bool contact_on;      // this frame's sun contact shadows (A4b: gtao.comp too)
    bool async_on, async_split; // (A9.7c) the particles on the compute queue this frame; the frame to split before the main pass
    GpuImage lut;         // (A9.4) the Color Grading LUT's strip (binding 80)
    GpuImage ssr_col, ssr_dep; // (A4c) last frame's color and depth for the screen space reflections (bindings 82-83)
    // (Q20) last frame's depth for the TSR (binding 85): copied after the TSR on frames an actor moved, at the output's
    // size (the render's region of it: the dynamic resolution moves it); valid for the next frame at the same render size
    GpuImage tsr_dep;
    uint32_t tsr_dep_w, tsr_dep_h, tsr_dep_cap_w, tsr_dep_cap_h;
    bool tsr_dep_valid;
    GpuImage oit_refr;         // (2026-11-16) the OIT pass's third sum: the glass's screen offsets, roughness, dispersion (binding 84)
    bool any_refract;          // a refractive glass material was sent (the scene's copy made on the frames it is drawn)
    bool any_glass_shadow;     // (2026-11-17, Q4) a glass with Colored Shadow was sent: GI_GLASS_SHADOW (mesh.frag glass_tint)
    // (Q4) the glass with Colored Shadow as boxes (the shadow without rays): its entity, its box (GpuFrame.glass's form)
    Entity glass_e[128];
    float glass_box[128][4][4];
    uint32_t nglass;
    uint32_t ssr_w, ssr_h;
    bool ssr_valid, ssr_want; // (a copy of last frame there; this frame copies)
    GpuImage ui;          // (A9.4) HDR10: the UI drawn apart (the SDR swapchain's format; binding 81), the swapchain's size
    uint32_t ui_w, ui_h;
    bool ui_drawn;        // this frame's UI target was drawn (render_hdr_ui_begin / end)
    float hdr_paper, hdr_peak;
    uint32_t lut_n;
    char lut_path[120];   // the one loaded ("": the identity)
    bool lut_ok;
    // Temporal super resolution (tsr.comp): the render at its own size (scene) upscaled into hdr (the output's size, what
    // the post reads) with a history (two, ping-pong); out_w/out_h the output's size (R.width/height: the render's)
    GpuImage tsr_in, hist[2];
    VkPipeline tsr_pipe;
    // the vendors' upscalers (upscale_vendor.c: DLSS, FSR, XeSS) in TSR's place: the full motion vectors they read
    // (ups_mv.comp, render size), the one running (UPS_*), its jitter's phases, the vertical FOV
    GpuImage ups_mv;
    VkPipeline ups_mv_pipe;
    // the decals (M6, decal.c): each frame slot's GpuDecals (host) and tiles' lists (decal_bin.comp writes them)
    GpuBuf decal_buf[RE_FRAMES_IN_FLIGHT], decal_tiles[RE_FRAMES_IN_FLIGHT];
    VkPipeline decal_bin_pipe;
    int ups_kind;
    uint32_t ups_phases;
    float ups_fov_y;
    uint32_t out_w, out_h, tsr_frame;
    bool tsr_on, tsr_targets, tsr_reset, tsr_have_prev;
    float tsr_prev_vp[16], tsr_prev_pos[3], tsr_scale; // (tsr_prev_pos: the camera's, a cut is a jump of 3 m)
    bool split_on, aa_on; // aa_on: this frame's resolve weighted by brightness (GI_AA_WEIGHTED)
    // the GI's rays by software (A1, sw_rt.c): the static set as last gathered (scene_prepare, when the scene changed)
    // and its version; each frame slot's copy and the version it holds; the dynamic set's room
    SwInst *sw_static, *sw_dyn;
    float sw_gi_scale; // the share of the cache's texels a frame the software rays keep (the GI's time: 1 at most)
    uint32_t sw_static_n, sw_static_cap, sw_static_version, sw_dyn_cap;
    uint32_t sw_static_up[RE_FRAMES_IN_FLIGHT];
    VkDeviceAddress sw_static_addr[RE_FRAMES_IN_FLIGHT];
    GpuBuf hiz;          // the min-depth pyramid (hiz.comp)
    uint32_t hiz_levels;
    VkResolveModeFlagBits depth_resolve_mode; // MIN when the GPU has it (the farthest sample), else sample 0
    // Two-phase occlusion culling (prepass, Hi-Z, late list): off, it didn't pay off in the scenes measured (few
    // occluders). The driver crash it seemed to cause was the recording one (notas-dev/escala.md), fixed since.
    bool occlusion;
    // its automatic choice (occlusion_auto): forced (-1: auto), the phase (0 steady, 1/2 measuring), the sums
    int oa_forced;
    uint32_t oa_phase, oa_frames;
    double oa_sum[2], oa_next;
    bool oa_best;
    uint32_t width, height;
    VkSampleCountFlagBits samples;
    VkSampleCountFlagBits max_samples;
    VkSampler shadow_sampler, linear_sampler, repeat_sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline mesh, mesh_rt, mesh_wire, shadow_pipe, sky, grid, mask_pipe, composite, composite_post; // mesh_rt: ray traced (RE_RT)
    VkPipeline mask_hair_pipe; // (2026-11-25, item 17) a selected actor's hair in the selection's mask (hair.vert, mask_hair.frag)
    VkPipeline dd_pipe, dd_lines;            // (2026-11-19) the debug lines over the composite (debug_draw.h); the batches' as hardware lines
    GpuBuf dd_buf[RE_FRAMES_IN_FLIGHT];      // ... their primitives a frame slot (host visible)
    GpuBuf dd_tbuf[RE_FRAMES_IN_FLIGHT];     // ... the batches' trace records (DdTrace)
    uint32_t dd_drawn, dd_max;               // ... last frame's count, the most so far
    VkPipeline prepass, hiz_pipe;
    // the meshes drawn by a Masked or Two Sided material (mesh_masked: set when an instance gets one; reset by a reload)
    // go through these, in runs of meshes (draw_runs): the alpha cut, both sides; the opaque ones keep early depth
    VkPipeline mesh_m, mesh_rt_m, prepass_m, shadow_m, cloth_shadow;
    // AR (render_ar_frame): the camera's picture (Y R8, UV RG8), its staging per frame, the floor's triangles per frame
    GpuImage ar_img[2];
    uint32_t ar_w, ar_h;
    GpuBuf ar_stage[RE_FRAMES_IN_FLIGHT], ar_vbuf[RE_FRAMES_IN_FLIGHT];
    VkDeviceSize ar_stage_cap;
    bool ar_on, ar_fresh;                // on; a picture this frame
    uint8_t* ar_pic;                     // (its copy: y then uv)
    size_t ar_pic_cap;
    float ar_corners[4][2], ar_shadow;
    float ar_tris[4096][4];
    uint32_t ar_nverts;
    VkPipeline ar_plane;
    // the real depth (render_ar_depth): R16 (mm), its staging per frame, a copy of the last one
    GpuImage ar_depth;
    uint32_t ar_dw, ar_dh;
    GpuBuf ar_dstage[RE_FRAMES_IN_FLIGHT];
    VkDeviceSize ar_dstage_cap;
    uint16_t* ar_dpic;
    size_t ar_dpic_cap;
    bool ar_depth_on, ar_dfresh;
    float ar_dcorners[4][2];
    VkPipeline ar_occlude;
    // (Q25b) mixed reality (render_mr_frame): its numbers and boxes, the boxes per frame slot; the real depth (D16, 2
    // layers: the headset's copied in, or the simulated one sent through a staging buffer)
    RenderMr mr;
    bool mr_on;
    float mr_box[MR_MAX_BOXES][4][4];
    GpuBuf mr_buf[RE_FRAMES_IN_FLIGHT];
    GpuImage mr_depth;
    uint32_t mr_dw, mr_dh;
    GpuBuf mr_dstage[RE_FRAMES_IN_FLIGHT];
    VkDeviceSize mr_dstage_cap;
    uint16_t* mr_dcpu;
    size_t mr_dcpu_cap;
    uint64_t mr_dserial;
    VkImage mr_dsrc;
    bool mr_dfresh, mr_dvalid, mr_dresize;
    VkAccelerationStructureInstanceKHR cloth_inst[64]; // (cloth.c: this frame's, into the dynamic TLAS)
    bool mesh_masked[RE_MAX_MESHES], any_masked, masked_dirty; // (dirty: MeshInfo's flag sent again)
    bool mesh_ext[RE_MAX_MESHES], mesh_ext_warned[RE_MAX_MESHES]; // (an RE_EXT material on it; its masked runs said once)
    // a mesh drawn by a Translucent material (its own run: the OIT pass draws it after the opaque scene, oit_record)
    bool mesh_translucent[RE_MAX_MESHES], any_translucent, oit_on;
    VkPipeline mesh_tl, mesh_rt_tl; // (the OIT pass's: one sample, two targets, mesh.frag RE_TRANSLUCENT)
    VkPipeline mesh_lsm; // (mesh with the local lights' shadow maps: RE_LSM)
    VkPipeline mesh_lsm_x; // (Q11) ... and the RE_EXT features (a Toon material on Low: while any_ext and a map / SSR)
    // (Q11) the RE_EXT pipelines from their RE_TOON twins (a Toon material's code) since one was sent: toon_rebuild makes
    // them again at the next frame's start (the plain RE_EXT ones keep the registers they had)
    bool toon_spv, toon_rebuild;
    VkPipeline mesh_m_eq, mesh_rt_m_eq; // the masked runs after the prepass: depth EQUAL, early tests, no cut, no write
    VkPipeline mesh_rt_t, mesh_rt_m_t, mesh_rt_m_eq_t; // ray traced, the imported meshes: the shadow terminator's fix
    VkPipeline mesh_x, mesh_rt_x, mesh_rt_t_x; // the opaque ones with the Principled's extra lobes (RE_EXT: while any_ext)
    // (2026-11-10) the masked runs' with them: raster, raster after the prepass, ray traced (the same two), terminator's
    VkPipeline mesh_m_x, mesh_m_eq_x, mesh_rt_m_x, mesh_rt_m_eq_x, mesh_rt_m_t_x, mesh_rt_m_eq_t_x;
    bool any_ext;     // a material sent has coat / anisotropy / subsurface / IOR (mat_ext): the RE_EXT pipelines
    bool any_ext_masked; // ... a Masked or Two Sided one (mat_cuts): the masked runs' RE_EXT pipelines too
    // (2026-11-16) what wants the RE_EXT pipelines besides the rain: the materials, the decals (any_ext / any_ext_masked
    // also go on the first frame it rains, so they are made; the frames without rain then use the plain ones again)
    bool ext_mat, ext_mat_masked;
    bool ext_decal;              // (2026-11-24) decals drawn this frame: every mesh's RE_EXT (they are in those variants)
    uint32_t runs_plain, runs_ext; // the main pass's runs drawn plain / RE_EXT, the last frame (render_ext_runs)
    // auto exposure: the histogram and the adapted value (GPU), a copy of the value per frame slot for the stats
    VkPipeline exposure_pipe;
    GpuBuf exposure, exposure_read[RE_FRAMES_IN_FLIGHT];
    GpuBuf flicker[RE_FRAMES_IN_FLIGHT]; // the flicker probe: 64 x 32 luminances per frame (exposure.comp pass 2)
    GpuBuf ghost[RE_FRAMES_IN_FLIGHT];   // (Q20) the ghosting probe: 128 x 64, negative under a moving actor (pass 3)
    GpuBuf relevance, rel_read[RE_FRAMES_IN_FLIGHT]; // the relevance bits (cull.comp) and their copies the CPU reads
    GpuBuf object_cost; // VIEW_OBJECT_COST's value per scene slot (host-visible; made the first time the view is used)
    uint32_t rel_slots[RE_FRAMES_IN_FLIGHT];
    uint64_t dyn_rel_hash;                          // the dynamic TLAS's relevant members when it was built
    const uint32_t* rel_cur;
    uint32_t rel_cur_slots;
    uint32_t* rel_copy;       // rel_cur's copy in the CPU's memory
    uint32_t rel_copy_cap;
    float flicker_prev[64 * 32], flicker_prev2[64 * 32];
    double probe_max, probe_mean_sum; // the probe's brightest point over the measured frames, and its mean (render_probe_lum)
    uint32_t probe_frames;
    bool flicker_have, flicker_have2, flicker_on;
    double flicker_sum, flicker_spots, flicker_pulses;
    uint32_t flicker_map[64 * 32]; // per point: the frames it flickered (render_flicker_map)
    uint32_t flicker_frames;
    // (Q20) the ghosting probe's state: each point's last luminance and the frames since a moving actor left it; the
    // change of the points just left (2..8 frames) is the trail, of the points left long ago the background's
    float ghost_prev[128 * 64];
    uint8_t ghost_age[128 * 64];
    bool ghost_have;
    double ghost_num, ghost_den, bg_num, bg_den, ghost_samples;
    double ghost_cov; // the points under a moving actor, all frames
    uint32_t ghost_frames;
    VkSampleCountFlagBits pipe_samples; // the samples the MSAA pipelines were made for
    bool pipe_rt_alpha;                 // ... and whether with the shadow rays' alpha test (RT_ALPHA: a Masked mesh drawn)
    VkFormat swap_format;
    bool wire_ok;
    bool indep_blend; // (the main pass's two targets blended apart: the split's; without, the split stays off)
    bool split_ok;    // the main pass has the split's second target: RT, independent blend, unused attachments allowed

    uint32_t pt_samples;   // accumulated by the path tracer since the scene last changed
    Push v2_push;          // (this frame's: the second view's dispatch)
    bool v2_ok;
    uint64_t pt_cam_hash;  // (the camera's own: moved or not)
    uint64_t pt_hash;      // what the scene looked like then
    uint32_t frame_counter;
    VkQueryPool queries;
    bool queries_valid[RE_FRAMES_IN_FLIGHT];
    // The Debug Viewer (debug_viewer.c) open: the finer timestamps, each instance's visibility read back (vis_read)
    bool detail, detail_slot[RE_FRAMES_IN_FLIGHT];
    GpuBuf vis_read[RE_FRAMES_IN_FLIGHT];
    uint32_t vis_read_n[RE_FRAMES_IN_FLIGHT], vis_n, vis_cap;
    uint32_t* vis_cpu; // the last copy asked for (render_debug_vis): the viewer reads it while the GPU writes the next
    bool vis_want;

    // what render_scene recorded this frame, for the mask and composite: the selected slot and its mesh
#define MASK_MAX 32768 // outlined meshes at most (a bigger selection outlines its first ones; 2026-11-25: 4096, a crowd's agents x parts)
    uint32_t mask_draws, mask_slot[MASK_MAX], mask_mesh[MASK_MAX];
    // (2026-11-25, item 17) each draw's id (mask.frag's pad: 2 + the selected actor's index, its parts sharing it; 0: the
    // slot's own), the mask left cleared last frame with nothing selected (its pass skipped until a selection or a resize)
    uint32_t mask_id[MASK_MAX];
    // (2026-11-25, item 17) a selected crowd's parts: one draw a part, its agents as instances (their slots nparts apart:
    // mesh.vert's crowd mask), 840 draws of 30 agents x 28 parts took 0.200 ms, 0.017 the rest
#define MASK_CROWD (CROWD_MAX_RUNS * 256)
    uint32_t mask_cn, mask_c_slot[MASK_CROWD], mask_c_mesh[MASK_CROWD], mask_c_stride[MASK_CROWD], mask_c_count[MASK_CROWD], mask_c_id[MASK_CROWD];
    uint32_t mask_c_inst; // (their instances in all: the tests' count of the agents' parts drawn)
    bool mask_idle;
    const uint8_t* mask_sel; // (2026-11-25) this frame's selected entity slots (a bit each; frame memory), their count, the world
    uint32_t mask_sel_n;
    World* mask_world;
    uint32_t mask_hl, mask_hl_mode; // the last mask_hl draws are the game's highlight (interaction.h), in its mode
    // (Q11) the outline (outline.c): a byte per scene slot (its material's or its actor's outline is on: instance_of), any
    // so far (none: the pass costs nothing), the materials' outline generation the slots were marked by
    uint8_t* ol_flag;
    bool any_outline;
    uint32_t ol_gen;
    // Post processing (post.c; shaders/velocity.*, bloom.comp, composite.frag): the moved actors' screen motion, the
    // bloom's chain (a quarter size and five halvings: one image, a view per level, always GENERAL)
    GpuImage velocity;
    VkImage bloom_img;
    VkDeviceMemory bloom_mem;
    VkImageView bloom_view, bloom_mip[BLOOM_LEVELS];
    uint32_t bloom_w[BLOOM_LEVELS], bloom_h[BLOOM_LEVELS];
    bool bloom_general;
    VkSampler mip_sampler; // linear, every level (the bloom's chain)
    VkPipeline velocity_pipe, bloom_pipe, velocity_hair_pipe, velocity_cloth_pipe;
    // Crowds (crowd.c, crowd.comp): their parts' instances after the world's slots (from crowd_base: the world's capacity),
    // written on the GPU each frame; crowd_room: the room reserved for them (the scene's buffers hold cap + crowd_room)
    VkPipeline crowd_pipe;
    uint32_t crowd_base, crowd_count, crowd_room, crowd_alloc;
    struct { Entity e; uint32_t version, nparts; GpuBuf templates, baked, big; } crowd_res[CROWD_MAX_RUNS];
    uint32_t ncrowd;
    struct { uint64_t agents, templates, baked, big; uint32_t base, nagents, nparts, frames; uint64_t prev; uint32_t pbase; } crowd_head_cpu[CROWD_MAX_RUNS];
    // each crowd part's rows last frame (crowd.comp writes them; velocity_crowd.vert: the parts' motion for the TSR)
    GpuBuf crowd_prev;
    uint32_t crowd_prev_cap;
    VkPipeline velocity_crowd_pipe;
    GpuBuf crowd_agents[RE_FRAMES_IN_FLIGHT], crowd_head[RE_FRAMES_IN_FLIGHT];
    uint32_t crowd_agents_cap[RE_FRAMES_IN_FLIGHT];
    GpuBuf vel_buf[RE_FRAMES_IN_FLIGHT]; // the entries (host memory, per frame slot)
    VelEntry* vel;       // this frame's (scene_prepare, frame memory)
    uint32_t nvel;
    float prev_vp[16];   // the last post view's view_proj (the camera's motion blur)
    float cloud_vp[16];  // last frame's view_proj and size (the screen's clouds' history)
    uint32_t cloud_sw, cloud_sh, cloud_ow, cloud_oh; // (the screen's clouds' size, kept through the dynamic resolution's small steps)
    bool cloud_half; // (the screen's clouds at half the view: Epic)
    double cloud_t; // when that frame was prepared (a march's weight follows the time between frames)
    bool have_prev_vp;
    uint32_t prev_w, prev_h;
    double post_time;    // when the last post view rendered (Motion Blur Target FPS)
    // this frame's work (fill_post): the composite's gather (depth of field or something to blur) with the depth it
    // reads, the velocity pass (moved actors under motion blur), the bloom; each skipped when it would change nothing
    bool post_run, post_vel, post_bloom;
    RenderStats stats;
} R;

const RenderStats* render_stats(void) { return &R.stats; }
void render_ext_runs(uint32_t* plain, uint32_t* ext, uint32_t* meshes) { // (2026-11-24)
    *plain = R.runs_plain, *ext = R.runs_ext, *meshes = 0;
    for (uint32_t i = 0; i < RE_MAX_MESHES; i++) *meshes += R.mesh_ext[i];
}

uint32_t render_mesh_slot(uint32_t mesh) {
    if (mesh < R.nmeshes) return mesh; // a basic shape (or a slot)
    for (uint32_t s = SHAPE_COUNT; s < R.nmeshes; s++)
        if (R.mesh_id[s] == mesh) return s;
    return SHAPE_CUBE; // (a mesh missing from Content)
}
uint32_t render_mesh_count(void) { return R.imp_card < R.nmeshes ? R.imp_card : R.nmeshes; } // (Q21: the impostors' card, the last one, left out of the lists)
uint32_t render_mesh_id(uint32_t slot) { return slot < R.nmeshes ? R.mesh_id[slot] : 0; }
const char* render_mesh_name(uint32_t slot) { return slot < R.nmeshes ? R.mesh_name[slot] : ""; }
const char* render_mesh_vpath(uint32_t slot) { return slot < R.nmeshes ? R.mesh_vpath[slot] : ""; }
float render_mesh_area(uint32_t slot) { return slot < R.nmeshes ? R.mesh_area[slot] : 0.0f; }

void render_cpu_mesh_data(const GpuVertex** v, const uint32_t** idx, uint32_t* nv) { *v = R.cpu_v, *idx = R.cpu_i, *nv = R.vertex_count; }
// (A1) the meshes as the software rays see them: the first LOD with up to max_tris triangles (the GI is low frequency:
// the heavy scene's 2.1 M triangles cost the cache 172 ms a frame on Epic), sharing LOD 0's vertices; prim_base: its
// first triangle counted from LOD 0's (a hit's attributes are read through LOD 0's first index, rt.glsl)
int render_sw_meshes(GiMesh* gm, int32_t* prim_base, uint32_t max_tris) {
    for (uint32_t s = 0; s < R.nmeshes; s++) {
        const Mesh* l0 = &R.lods[s][0];
        uint32_t l = 0;
        while (l + 1 < R.lod_count[s] && R.lods[s][l].index_count / 3 > max_tris && R.lods[s][l + 1].index_count &&
               R.lods[s][l + 1].vertex_offset == l0->vertex_offset && R.lods[s][l + 1].first_index >= l0->first_index)
            l++;
        const Mesh* m = l ? &R.lods[s][l] : &R.meshes[s];
        gm[s] = (GiMesh){ m->first_index, m->index_count, m->vertex_offset };
        prim_base[s] = l ? (int32_t)((m->first_index - l0->first_index) / 3) : 0;
    }
    return (int)R.nmeshes;
}
int render_gi_meshes(GiMesh* gm) {
    for (uint32_t s = 0; s < R.nmeshes; s++) gm[s] = (GiMesh){ R.meshes[s].first_index, R.meshes[s].index_count, R.meshes[s].vertex_offset };
    return (int)R.nmeshes;
}

void render_mesh_cpu(uint32_t mesh, const GpuVertex** v, const uint32_t** idx, uint32_t* first_index, uint32_t* index_count,
                     int32_t* vertex_offset) {
    mesh = render_mesh_slot(mesh);
    *v = R.cpu_v;
    *idx = R.cpu_i;
    *first_index = R.meshes[mesh].first_index;
    *index_count = R.meshes[mesh].index_count;
    *vertex_offset = R.meshes[mesh].vertex_offset;
}

bool render_mesh_range(uint32_t mesh, uint32_t* first_index, uint32_t* index_count, int32_t* vertex_offset) {
    if (!R.vertices.buffer) return false;
    mesh = render_mesh_slot(mesh);
    if (mesh >= R.nmeshes) return false;
    *first_index = R.meshes[mesh].first_index, *index_count = R.meshes[mesh].index_count, *vertex_offset = R.meshes[mesh].vertex_offset;
    return true;
}
bool render_mesh_lod_range(uint32_t mesh, uint32_t lod, uint32_t* first_index, uint32_t* index_count, int32_t* vertex_offset) {
    if (!R.vertices.buffer) return false;
    mesh = render_mesh_slot(mesh);
    if (mesh >= R.nmeshes) return false;
    uint32_t n = R.lod_count[mesh];
    const Mesh* m = lod && n > 1u ? &R.lods[mesh][lod < n ? lod : n - 1u] : &R.meshes[mesh];
    if (!m->index_count) m = &R.meshes[mesh];
    *first_index = m->first_index, *index_count = m->index_count, *vertex_offset = m->vertex_offset;
    return true;
}
uint32_t render_mesh_lods(uint32_t mesh, uint32_t tris[4], float error[4]) {
    uint32_t s = render_mesh_slot(mesh);
    if (s >= R.nmeshes) return 0;
    uint32_t n = R.lod_count[s] ? R.lod_count[s] : 1u;
    for (uint32_t l = 0; l < 4; l++) {
        if (tris) tris[l] = l < n ? (l ? R.lods[s][l].index_count : R.meshes[s].index_count) / 3u : 0u;
        if (error) error[l] = l < n ? R.lod_error[s][l] : 0.0f;
    }
    return n;
}
// (2026-11-25, item 17) the selection's mask as the last frame left it (the tests: --scene selection), read back with the
// GPU idle: how many pixels are set, a hash of which ones (two poses of a skin differ), the last frame's mask draws
bool render_mask_read(VkCtx* vk, uint32_t* set, uint64_t* hash, uint32_t* draws, uint8_t* copy, size_t cap, uint32_t* w, uint32_t* h) {
    *set = 0, *hash = 0, *draws = R.mask_draws + R.mask_c_inst;
    if (w) *w = R.out_w;
    if (h) *h = R.out_h;
    if (!R.mask.image || !R.out_w || !R.out_h) return false;
    VkDeviceSize n = (VkDeviceSize)R.out_w * R.out_h;
    GpuBuf b;
    if (!gpu_buf_create(vk, &b, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    vkDeviceWaitIdle(vk->device);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkImageLayout was = R.mask.layout;
    gpu_transition(cmd, &R.mask, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    VkBufferImageCopy c = { 0 };
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, c.imageSubresource.layerCount = 1;
    c.imageExtent = (VkExtent3D){ R.out_w, R.out_h, 1 };
    vkCmdCopyImageToBuffer(cmd, R.mask.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &c);
    gpu_transition(cmd, &R.mask, was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : was, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, false);
    gpu_end_once(vk, cmd);
    const uint8_t* p = b.mapped;
    uint64_t hh = 1469598103934665603ull;
    for (VkDeviceSize i = 0; i < n; i++)
        if (p[i]) (*set)++, hh = (hh ^ (uint64_t)i) * 1099511628211ull;
    *hash = hh;
    if (copy && cap >= n) memcpy(copy, p, n); // (the pixels themselves: the test's own look at the outline)
    gpu_buf_destroy(vk, &b);
    return true;
}

// (2026-11-25, item 16) a skin's mesh (RMESH_SKINNED: uv2's bits its 4 bones and weights; groom.c binds hair roots to them)
bool render_mesh_is_skinned(uint32_t mesh) {
    uint32_t s = render_mesh_slot(mesh);
    return s < R.nmeshes && R.mesh_skinned[s];
}

// (2026-11-24, item 4) its Nanite as loaded (the Static / Skeletal Mesh viewer's Nanite Settings): its DAG's clusters (0:
// none) and its LODs' clusters (the Masked path), whether it is a rigid skin
uint32_t render_mesh_nanite(uint32_t mesh, uint32_t* lod_clusters, bool* rigid) {
    uint32_t s = render_mesh_slot(mesh);
    if (lod_clusters) *lod_clusters = 0;
    if (rigid) *rigid = false;
    if (s >= R.nmeshes) return 0;
    if (lod_clusters)
        for (uint32_t l = 0; l < MAX_LODS && l < R.lod_count[s]; l++) *lod_clusters += R.cluster_count[s][l];
    if (rigid) *rigid = R.mesh_skinned[s] && R.mesh_rigid_bone[s] >= 0; // (skin_is_rigid's)
    return R.dag_count[s];
}
// cull.comp's choice on the CPU: an imported mesh's coarsest LOD whose error stays under a pixel (Epic), a basic shape's
// LOD by its radius on screen (48 px, then x0.4 a level)
uint32_t render_mesh_lod_for(uint32_t mesh, float scale, float dist, float focal_px) {
    uint32_t s = render_mesh_slot(mesh);
    if (s >= R.nmeshes) return 0;
    uint32_t n = R.lod_count[s] ? R.lod_count[s] : 1u, lod = 0;
    const Mesh* m = &R.meshes[s];
    float e[3] = { (m->mx[0] - m->mn[0]) * 0.5f, (m->mx[1] - m->mn[1]) * 0.5f, (m->mx[2] - m->mn[2]) * 0.5f };
    float r = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * scale;
    float d = fmaxf(dist - r, 1.0f);
    if (R.mesh_error_lod[s]) {
        float k = scale / d * focal_px;
        for (uint32_t l = 1; l < n && R.lod_error[s][l] * k < 1.0f; l++) lod = l;
    } else {
        float px = r / d * focal_px;
        for (float t = 48.0f; lod + 1u < n && px < t; t *= 0.4f) lod++;
    }
    return lod;
}
VkBuffer render_index_buffer(void) { return R.indices.buffer; }
uint64_t render_vertex_address(void) { return R.vertices.addr; }

static void update_descriptors(VkCtx* vk);
static void hdr_ui_target(VkCtx* vk); // (A9.4: HDR10's UI target)

void render_rebind_descriptors(VkCtx* vk) {
    vkDeviceWaitIdle(vk->device);
    if (R.hdr.image) update_descriptors(vk);
}

void render_mesh_bounds(uint32_t mesh, float mn[3], float mx[3]) {
    mesh = render_mesh_slot(mesh);
    memcpy(mn, R.meshes[mesh].mn, sizeof(float) * 3);
    memcpy(mx, R.meshes[mesh].mx, sizeof(float) * 3);
}

void rotator_axes(const float r[3], float m[3][3]) {
    float sp = sinf(r[0] * DEG2RAD), cp = cosf(r[0] * DEG2RAD);
    float sy = sinf(r[1] * DEG2RAD), cy = cosf(r[1] * DEG2RAD);
    float sr = sinf(r[2] * DEG2RAD), cr = cosf(r[2] * DEG2RAD);
    m[0][0] = cp * cy; m[0][1] = cp * sy; m[0][2] = sp;
    m[1][0] = sr * sp * cy - cr * sy; m[1][1] = sr * sp * sy + cr * cy; m[1][2] = -sr * cp;
    m[2][0] = -(cr * sp * cy + sr * sy); m[2][1] = cy * sr - cr * sp * sy; m[2][2] = cr * cp;
}

void axes_rotator(const float x[3][3], float r[3]) {
    const float R2D = 57.29577951f;
    r[0] = atan2f(x[0][2], sqrtf(x[0][0] * x[0][0] + x[0][1] * x[0][1])) * R2D;
    r[1] = atan2f(x[0][1], x[0][0]) * R2D;
    r[2] = 0.0f;
    float m[3][3];
    rotator_axes(r, m); // the Y axis without roll; the roll is the angle from it to the real Y in the Y-Z plane
    r[2] = atan2f(x[2][0] * m[1][0] + x[2][1] * m[1][1] + x[2][2] * m[1][2],
                  x[1][0] * m[1][0] + x[1][1] * m[1][1] + x[1][2] * m[1][2]) * R2D;
}

// (2026-11-23) A material's mean emission (nits) for the rays, in GpuInstance.emissive_rough.z: 10 bits a channel, log2
// in 1/32 octave from 2^-10 nits (0: none), bit 30 when its actor lights as a point light (common.glsl inst_glow)
uint32_t render_emis_pack(const float nits[3], bool lit) {
    uint32_t v = lit ? 0x40000000u : 0u;
    for (int k = 0; k < 3; k++) {
        float x = nits[k];
        if (!(x > 0.0009765625f)) continue;
        float c = roundf((log2f(x) + 10.0f) * 32.0f);
        v |= (uint32_t)fminf(fmaxf(c, 1.0f), 1023.0f) << (10 * k);
    }
    return v;
}

void render_transform_rows(const CTransform* t, float rows[3][4]) {
    float m[3][3];
    rotator_axes(t->rotation, m); // m[a] = the world direction of local axis a
    for (int r = 0; r < 3; r++) {
        for (int a = 0; a < 3; a++) rows[r][a] = m[a][r] * t->scale[a];
        rows[r][3] = t->location[r];
    }
}

// --- Buffers and images ---

// (merge of the batch 7) the CPU's readbacks (the probes of the measures): host memory with the CPU's cache; plain
// HOST_COHERENT is uncached on NVIDIA, and reading Q20's 8192-point ghosting probe from it cost ~0.8 ms a frame
static bool s_buf_cached;
static uint32_t host_memory_type(VkCtx* vk, uint32_t bits) {
    if (s_buf_cached) {
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(vk->gpu, &mem);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return vkctx_find_memory(vk, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

bool gpu_buf_create(VkCtx* vk, GpuBuf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    uint32_t fams[2] = { vk->queue_family, vk->async_family };
    if (vk->async_ok) // (A9.7c) shared by the graphics and the compute queues: no ownership transfers (buffers: no cost)
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT, bi.queueFamilyIndexCount = 2, bi.pQueueFamilyIndices = fams;
    if (vkCreateBuffer(vk->device, &bi, NULL, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, b->buffer, &req);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = host ? host_memory_type(vk, req.memoryTypeBits) : vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &b->memory) != VK_SUCCESS) return false;
    vk_check(vkBindBufferMemory(vk->device, b->buffer, b->memory, 0), "vkBindBufferMemory");
    if (host) vk_check(vkMapMemory(vk->device, b->memory, 0, size, 0, &b->mapped), "vkMapMemory");
    VkBufferDeviceAddressInfo ad = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ad.buffer = b->buffer;
    b->addr = vkGetBufferDeviceAddress(vk->device, &ad);
    b->size = size;
    return true;
}

void gpu_buf_destroy(VkCtx* vk, GpuBuf* b) {
    if (b->buffer) vkDestroyBuffer(vk->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(vk->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

bool gpu_image_create(VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                         VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = samples;
    ii.usage = usage;
    if (vkCreateImage(vk->device, &ii, NULL, &im->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im->image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &im->memory) != VK_SUCCESS) return false;
    vk_check(vkBindImageMemory(vk->device, im->image, im->memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
    vk_check(vkCreateImageView(vk->device, &vi, NULL, &im->view), "vkCreateImageView");
    im->format = fmt;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void gpu_image_destroy(VkCtx* vk, GpuImage* im) {
    if (im->view) vkDestroyImageView(vk->device, im->view, NULL);
    if (im->image) vkDestroyImage(vk->device, im->image, NULL);
    if (im->memory) vkFreeMemory(vk->device, im->memory, NULL);
    memset(im, 0, sizeof *im);
}

// One barrier: a layout change with the stages/accesses of the two sides
void gpu_transition(VkCommandBuffer cmd, GpuImage* im, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                       VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                       bool discard) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    bool depth = im->format == VK_FORMAT_D32_SFLOAT;
    b.subresourceRange = (VkImageSubresourceRange){ depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers ? im->layers : 1u }; // (an array's own layers only)
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    im->layout = to;
}

// --- Procedural meshes (Unreal's BasicShapes: 100 cm) ---

typedef struct MeshBuilder {
    GpuVertex* v;
    uint32_t* idx;
    uint32_t nv, ni, cap_v, cap_i;
} MeshBuilder;

static uint32_t oct_encode(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        float oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t sx = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, x)) * 32767.0f);
    int16_t sy = (int16_t)lroundf(fmaxf(-1.0f, fminf(1.0f, y)) * 32767.0f);
    return (uint32_t)(uint16_t)sx | ((uint32_t)(uint16_t)sy << 16);
}

static uint32_t mb_vertex(MeshBuilder* b, float x, float y, float z, float nx, float ny, float nz, float u, float v) {
    if (b->nv >= b->cap_v) return 0;
    float n[3] = { nx, ny, nz }, l = sqrtf(nx * nx + ny * ny + nz * nz);
    for (int k = 0; k < 3; k++) n[k] /= l > 0 ? l : 1;
    GpuVertex* o = &b->v[b->nv];
    o->pos[0] = x; o->pos[1] = y; o->pos[2] = z;
    o->normal_oct = oct_encode(n);
    o->uv[0] = u; o->uv[1] = v;
    o->uv2[0] = 0.02f + u * 0.96f; o->uv2[1] = 0.02f + v * 0.96f; // the default unwrap (sphere, plane): one chart
    return b->nv++;
}

// A triangle whose face normal agrees with its vertices' normals (the winding is fixed here, once)
static void mb_tri(MeshBuilder* b, uint32_t i0, uint32_t i1, uint32_t i2) {
    if (b->ni + 3 > b->cap_i) return;
    const float *a = b->v[i0].pos, *c1 = b->v[i1].pos, *c2 = b->v[i2].pos;
    float e1[3] = { c1[0] - a[0], c1[1] - a[1], c1[2] - a[2] }, e2[3] = { c2[0] - a[0], c2[1] - a[1], c2[2] - a[2] };
    float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    // the vertex normals (decoded roughly: the sum of the three positions' outward guess is enough for a convex shape)
    float sum[3] = { 0, 0, 0 };
    uint32_t ids[3] = { i0, i1, i2 };
    for (int k = 0; k < 3; k++) {
        uint32_t p = b->v[ids[k]].normal_oct;
        float x = (float)(int16_t)(p & 0xFFFF) / 32767.0f, y = (float)(int16_t)(p >> 16) / 32767.0f;
        float z = 1.0f - fabsf(x) - fabsf(y);
        if (z < 0.0f) {
            float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
            x = ox, y = oy;
        }
        sum[0] += x; sum[1] += y; sum[2] += z;
    }
    bool flip = fn[0] * sum[0] + fn[1] * sum[1] + fn[2] * sum[2] < 0.0f;
    b->idx[b->ni++] = i0;
    b->idx[b->ni++] = flip ? i2 : i1;
    b->idx[b->ni++] = flip ? i1 : i2;
}

static void mb_quad(MeshBuilder* b, uint32_t a, uint32_t c1, uint32_t c2, uint32_t d) {
    mb_tri(b, a, c1, c2);
    mb_tri(b, a, c2, d);
}

static void build_cube(MeshBuilder* b) {
    static const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const float u[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
    for (int f = 0; f < 6; f++) {
        float vv[3] = { n[f][1] * u[f][2] - n[f][2] * u[f][1], n[f][2] * u[f][0] - n[f][0] * u[f][2], n[f][0] * u[f][1] - n[f][1] * u[f][0] };
        uint32_t id[4];
        for (int k = 0; k < 4; k++) {
            float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = k >= 2 ? 1.0f : -1.0f;
            id[k] = mb_vertex(b, 50 * (n[f][0] + su * u[f][0] + sv * vv[0]), 50 * (n[f][1] + su * u[f][1] + sv * vv[1]),
                              50 * (n[f][2] + su * u[f][2] + sv * vv[2]), n[f][0], n[f][1], n[f][2], su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
            GpuVertex* gv = &b->v[id[k]]; // the faces in a 3x2 grid of the lightmap, with a margin
            gv->uv2[0] = ((float)(f % 3) + 0.06f + gv->uv[0] * 0.88f) / 3.0f;
            gv->uv2[1] = ((float)(f / 3) + 0.06f + gv->uv[1] * 0.88f) / 2.0f;
        }
        mb_quad(b, id[0], id[1], id[2], id[3]);
    }
}

static void build_sphere(MeshBuilder* b, int seg, int rings) {
    uint32_t base = b->nv;
    for (int r = 0; r <= rings; r++) {
        float th = (float)r / rings * 3.14159265f, z = cosf(th), s = sinf(th);
        for (int i = 0; i <= seg; i++) {
            float ph = (float)i / seg * 6.2831853f, x = s * cosf(ph), y = s * sinf(ph);
            mb_vertex(b, x * 50, y * 50, z * 50, x, y, z, (float)i / seg, (float)r / rings);
        }
    }
    for (int r = 0; r < rings; r++)
        for (int i = 0; i < seg; i++) {
            uint32_t a = base + r * (seg + 1) + i, c = a + seg + 1;
            if (r > 0) mb_tri(b, a, a + 1, c);
            if (r < rings - 1) mb_tri(b, a + 1, c + 1, c);
        }
}

// Cylinder and cone: radius 50, z from -50 to 50 (the cone's tip at the top)
static void build_round(MeshBuilder* b, bool cone, int seg) {
    uint32_t side = b->nv;
    for (int i = 0; i <= seg; i++) {
        float ph = (float)i / seg * 6.2831853f, c = cosf(ph), s = sinf(ph);
        float nx = cone ? c * 100.0f : c, ny = cone ? s * 100.0f : s, nz = cone ? 50.0f : 0.0f;
        mb_vertex(b, c * 50, s * 50, -50, nx, ny, nz, (float)i / seg, 1);
        if (cone) {
            float pm = ((float)i + 0.5f) / seg * 6.2831853f; // the tip: one vertex per side, its normal mid-way
            mb_vertex(b, 0, 0, 50, cosf(pm) * 100.0f, sinf(pm) * 100.0f, 50.0f, (float)i / seg, 0);
        } else {
            mb_vertex(b, c * 50, s * 50, 50, nx, ny, nz, (float)i / seg, 0);
        }
    }
    for (uint32_t k = side; k < b->nv; k++) { // the side in the top 60% of the lightmap, the caps as circles below
        b->v[k].uv2[0] = 0.02f + b->v[k].uv[0] * 0.96f;
        b->v[k].uv2[1] = 0.02f + b->v[k].uv[1] * 0.56f;
    }
    for (int i = 0; i < seg; i++) {
        uint32_t a = side + i * 2;
        if (cone) mb_tri(b, a, a + 2, a + 1);
        else mb_quad(b, a, a + 2, a + 3, a + 1);
    }
    for (int cap = 0; cap < (cone ? 1 : 2); cap++) {
        float z = cap ? 50.0f : -50.0f, nz = cap ? 1.0f : -1.0f;
        uint32_t center = mb_vertex(b, 0, 0, z, 0, 0, nz, 0.5f, 0.5f), ring = b->nv;
        for (int i = 0; i <= seg; i++) {
            float ph = (float)i / seg * 6.2831853f;
            mb_vertex(b, cosf(ph) * 50, sinf(ph) * 50, z, 0, 0, nz, cosf(ph) * 0.5f + 0.5f, sinf(ph) * 0.5f + 0.5f);
        }
        for (uint32_t k = center; k < b->nv; k++) {
            b->v[k].uv2[0] = (cap ? 0.25f : 0.75f) + (b->v[k].uv[0] - 0.5f) * 0.36f;
            b->v[k].uv2[1] = 0.79f + (b->v[k].uv[1] - 0.5f) * 0.36f;
        }
        for (int i = 0; i < seg; i++) mb_tri(b, center, ring + i, ring + i + 1);
    }
}

static void build_plane(MeshBuilder* b) {
    uint32_t a = mb_vertex(b, -50, -50, 0, 0, 0, 1, 0, 1), c = mb_vertex(b, 50, -50, 0, 0, 0, 1, 1, 1);
    uint32_t d = mb_vertex(b, 50, 50, 0, 0, 0, 1, 1, 0), e = mb_vertex(b, -50, 50, 0, 0, 0, 1, 0, 0);
    mb_quad(b, a, c, d, e);
}

VkCommandBuffer gpu_begin_once(VkCtx* vk) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = vk->once_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(vk->device, &ai, &cmd), "one-time cmd");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void gpu_end_once(VkCtx* vk, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE), "one-time submit");
    vkQueueWaitIdle(vk->queue);
    vkFreeCommandBuffers(vk->device, vk->once_pool, 1, &cmd);
}

void gpu_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                 VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

static void upload(VkCtx* vk, GpuBuf* dst, const void* data, VkDeviceSize size) {
    GpuBuf staging;
    gpu_buf_create(vk, &staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, data, (size_t)size);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy region = { 0, 0, size };
    vkCmdCopyBuffer(cmd, staging.buffer, dst->buffer, 1, &region);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &staging);
}

// --- The imported meshes (.rmesh in Content: render.h) ---

uint32_t render_mesh_id_of(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h | 0x80000000u;
}

typedef struct RMeshFile { char path[600], vpath[600]; } RMeshFile;
typedef struct RMeshScan { RMeshFile* files; uint32_t n, cap; char root[600]; } RMeshScan;

static void scan_rmesh(RMeshScan* s, const char* dir, int depth);
static void scan_rmesh_entry(void* user, const ReDirEntry* e) {
    RMeshScan* s = ((void**)user)[0];
    const char* dir = ((void**)user)[1];
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        scan_rmesh(s, path, 1);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || strcmp(dot, ".rmesh") || s->n >= RE_MAX_MESHES) return;
    if (s->n == s->cap) {
        uint32_t c = s->cap ? s->cap * 2 : 16;
        RMeshFile* f = realloc(s->files, sizeof *f * c); // a load, not a frame
        if (!f) return;
        s->files = f, s->cap = c;
    }
    RMeshFile* f = &s->files[s->n++];
    snprintf(f->path, sizeof f->path, "%s", path);
    snprintf(f->vpath, sizeof f->vpath, "/Content%s", path + strlen(s->root));
}
static void scan_rmesh(RMeshScan* s, const char* dir, int depth) {
    if (depth > 12) return;
    void* user[2] = { s, (void*)dir };
    re_dir_walk(dir, scan_rmesh_entry, user);
}
static int by_vpath(const void* a, const void* b) { return strcmp(((const RMeshFile*)a)->vpath, ((const RMeshFile*)b)->vpath); }

static bool builder_room(MeshBuilder* b, uint32_t nv, uint32_t ni) {
    if (b->nv + nv > b->cap_v) {
        uint32_t c = b->cap_v;
        while (c < b->nv + nv) c *= 2;
        GpuVertex* v = realloc(b->v, sizeof(GpuVertex) * c);
        if (!v) return false;
        b->v = v, b->cap_v = c;
    }
    if (b->ni + ni > b->cap_i) {
        uint32_t c = b->cap_i;
        while (c < b->ni + ni) c *= 2;
        uint32_t* i = realloc(b->idx, sizeof(uint32_t) * c);
        if (!i) return false;
        b->idx = i, b->cap_i = c;
    }
    return true;
}

// Every .rmesh of the project's Content folder after the shapes (sorted by path: a new import keeps the others' slots
// when it sorts last; the slot is never stored, the id is)
static char g_mesh_root[600]; // the project's Content ("": the executable's)
void render_set_content_root(const char* content_root) { snprintf(g_mesh_root, sizeof g_mesh_root, "%s", content_root ? content_root : ""); }
const char* render_content_root(void) { return g_mesh_root; }

// The meshes' CPU side, made apart from the renderer's (a worker reads them: render_meshes_reload_async), then put in
// place and sent to the GPU on the main thread (stage_upload)
typedef struct MeshStage {
    MeshBuilder b;
    uint32_t nmeshes;
    Mesh meshes[RE_MAX_MESHES];
    Mesh lods[RE_MAX_MESHES][MAX_LODS];
    uint32_t lod_count[RE_MAX_MESHES];
    float lod_error[RE_MAX_MESHES][MAX_LODS];
    uint32_t mesh_id[RE_MAX_MESHES];
    float mesh_area[RE_MAX_MESHES];
    bool mesh_atlas[RE_MAX_MESHES], mesh_error_lod[RE_MAX_MESHES];
    char mesh_name[RE_MAX_MESHES][64];
    char mesh_vpath[RE_MAX_MESHES][160]; // (its file: "/Content/...", "" for the shapes)
    uint32_t cluster_first[RE_MAX_MESHES][MAX_LODS], cluster_count[RE_MAX_MESHES][MAX_LODS];
    GpuCluster* cpu_cl;
    uint32_t* cpu_perm;
    uint32_t ncl, cl_cap;
    uint32_t dag_first[RE_MAX_MESHES], dag_count[RE_MAX_MESHES]; // the cluster LOD DAGs (mesh_clod.h; in the clusters)
    bool any_dag;
    bool mesh_skinned[RE_MAX_MESHES]; // RMESH_SKINNED: their instances bend their own vertices ("skin")
    int16_t mesh_rigid_bone[RE_MAX_MESHES]; // (2026-11-24) R.mesh_rigid_bone's
    uint8_t mesh_collision[RE_MAX_MESHES];  // (2026-11-24) R.mesh_collision's
    float mesh_fallback[RE_MAX_MESHES];     // (2026-11-24) its rays' share of LOD 0's triangles (.rlod "fallback": the BLAS's LOD)
    uint32_t skinned_nv, skin_room;
    // their morph targets (R.morphs' words: each mesh's CSR then its deltas) and the names
    uint32_t* mw;
    size_t nmw, mw_cap;
    uint32_t morph_first[RE_MAX_MESHES], morph_delta[RE_MAX_MESHES], morph_count[RE_MAX_MESHES], morph_name0[RE_MAX_MESHES];
    char (*morph_names)[RMESH_MORPH_NAME];
    uint32_t nmorph_names, morph_meshes;
    uint32_t vcol_first[RE_MAX_MESHES]; // the vertex colors (RMESH_COLORS) in the same words (UINT32_MAX: none)
    uint32_t tess_first[RE_MAX_MESHES]; // (Q17) a DAG mesh's canonical table (tess_canon_table) in the same words
    GpuBuf g_m;
    // the GPU's buffers made and the data in host memory (stage_gpu_prepare: on the worker too, Vulkan's creation calls
    // are thread-safe); the main thread only records the copies (one submit)
    bool gpu_ready, cl_ok;
    GpuBuf g_v, g_i, g_cl, g_perm, g_work, g_idx, staging;
    GpuBuf g_read[RE_FRAMES_IN_FLIGHT]; // (R.cl_read: the clusters' draws to the stats)
    GiBlasPrep* blas; // (the ray tracing's structures of these meshes, made on the worker too)
} MeshStage;

static MeshStage* stage_new(void) {
    MeshStage* st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->b.cap_v = 16384;
    st->b.cap_i = 65536;
    st->b.v = malloc(sizeof(GpuVertex) * st->b.cap_v);
    st->b.idx = malloc(sizeof(uint32_t) * st->b.cap_i);
    if (!st->b.v || !st->b.idx) return free(st->b.v), free(st->b.idx), free(st), NULL;
    return st;
}

static void stage_gpu_drop(VkCtx* vk, MeshStage* st) {
    GpuBuf* all[8] = { &st->g_v, &st->g_i, &st->g_cl, &st->g_perm, &st->g_work, &st->g_idx, &st->staging, &st->g_m };
    for (int i = 0; i < 8; i++) gpu_buf_destroy(vk, all[i]);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &st->g_read[f]);
    st->gpu_ready = false;
}

static void stage_free(MeshStage* st) {
    if (!st) return;
    free(st->b.v), free(st->b.idx), free(st->cpu_cl), free(st->cpu_perm), free(st->mw), free(st->morph_names);
    free(st);
}

// The skins' room at most (vertices: 6 M, 192 MB; 2026-11-24, item 4: 10 M with 6 GB of VRAM, 16 M with 8 GB and up: 5000
// animated characters of ~1000 vertices, each bent twice, the last frame's for the velocity); REALENGINE_SKIN_ROOM_MAX lowers
// it (the skin_noroom test: the room full with a few skins, as 2080 capsules fill the 6 M)
static uint32_t g_skin_room_cap = 6u << 20; // (render_init: by the GPU's own memory)
static uint32_t skin_room_max(void) {
    static uint32_t m;
    if (!m) {
        const char* v = getenv("REALENGINE_SKIN_ROOM_MAX");
        m = v && atoi(v) > 0 && (uint32_t)atoi(v) < g_skin_room_cap ? (uint32_t)atoi(v) : g_skin_room_cap;
    }
    return m;
}
#define SKIN_ROOM_MAX skin_room_max()
#define SKINS_MAX 16384u
// (2026-11-24, item 4) a mesh's own settings saved beside it by the viewer (.<file>.rlod, JSON): "nanite" -1 auto, 0 off, 1 on;
// "fallback" its rays' share of LOD 0's triangles (%); "collision" 0 the actor's, 1 simple, 2 complex. Absent: the defaults
static void mesh_side_read(const char* path, int* nanite, float* fallback, int* collision) {
    const char* bn = path + strlen(path);
    while (bn > path && bn[-1] != '/' && bn[-1] != '\\') bn--;
    char side[1200];
    snprintf(side, sizeof side, "%.*s.%s.rlod", (int)(bn - path), path, bn);
    FILE* f = re_fopen(side, "rb");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    JsonDoc d;
    if (!json_parse(buf, n, &d, NULL, 0)) return;
    *nanite = json_get_int(&d.root, "nanite", *nanite);
    *fallback = (float)json_get_num(&d.root, "fallback", *fallback);
    *collision = json_get_int(&d.root, "collision", *collision);
    json_free(&d);
    if (*nanite < -1 || *nanite > 1) *nanite = -1;
    if (*collision < 0 || *collision > 2) *collision = 0;
    if (!(*fallback > 0.0f) || *fallback > 100.0f) *fallback = 100.0f;
} // (2026-11-24, item 4) live skins at most (the vertices' room usually ends them first)
// A mesh's LOD 0 vertices (its indices' highest + 1; 0: none), the bend of a skin on it: counted once a load (R.skin_nv)
static uint32_t skin_mesh_nv(uint32_t mesh) {
    if (mesh >= RE_MAX_MESHES) return 0u;
    if (R.skin_nv[mesh]) return R.skin_nv[mesh] == UINT32_MAX ? 0u : R.skin_nv[mesh];
    const Mesh* me = &R.meshes[mesh];
    uint32_t nv = 0;
    if (R.cpu_i && me->index_count) {
        uint32_t hi = 0;
        for (uint32_t q = 0; q < me->index_count; q++)
            if (R.cpu_i[me->first_index + q] > hi) hi = R.cpu_i[me->first_index + q];
        nv = hi + 1u;
    }
    R.skin_nv[mesh] = nv ? nv : UINT32_MAX;
    return nv;
}
// Any thread: the vertices, indices and clusters' buffers made, their data in one host staging buffer
static bool stage_gpu_prepare(VkCtx* vk, MeshStage* st) {
    MeshBuilder* b = &st->b;
    VkDeviceSize sv = sizeof(GpuVertex) * (VkDeviceSize)b->nv, si = sizeof(uint32_t) * (VkDeviceSize)b->ni;
    VkDeviceSize sc = sizeof(GpuCluster) * (VkDeviceSize)st->ncl, sp = sizeof(uint32_t) * 6 * (VkDeviceSize)st->ncl;
    // the BLAS read them (ray tracing), the hit shaders read the indices by address
    VkBufferUsageFlags as_input = vk->ray_query ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    // the skins' room after them: two regions (this frame, last frame) for 4 instances of each skin, more when a run asked
    // (skin_bind: out of room, the meshes reloaded with what it needed), 6 M vertices at most (192 MB)
    uint64_t room = (uint64_t)st->skinned_nv * 8u;
    if (room < R.skin_room_need) room = R.skin_room_need;
    if (room > SKIN_ROOM_MAX) room = SKIN_ROOM_MAX;
    st->skin_room = (uint32_t)room;
    // (TRANSFER_SRC: render_skin_readback, the morph test's look at a skin)
    bool ok = gpu_buf_create(vk, &st->g_v, sv + sizeof(GpuVertex) * room,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | as_input, false) &&
              gpu_buf_create(vk, &st->g_i, si, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | as_input, false);
    // the clusters, the work (dispatch x y z + a counter, per instance a draw and its 4 words), the indices' room
    // (64 MB: the LODs drawn by clusters this frame, their whole index count reserved)
    st->cl_ok = ok && st->ncl &&
                gpu_buf_create(vk, &st->g_cl, sc, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                gpu_buf_create(vk, &st->g_perm, sp, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
                gpu_buf_create(vk, &st->g_work, CL_WORK_BYTES,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) &&
                gpu_buf_create(vk, &st->g_idx, (VkDeviceSize)(16u << 20) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    for (int f = 0; st->cl_ok && f < RE_FRAMES_IN_FLIGHT; f++)
        st->cl_ok = gpu_buf_create(vk, &st->g_read[f], (VkDeviceSize)CL_MAX_ENTRIES * 60 + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!st->cl_ok) {
        gpu_buf_destroy(vk, &st->g_cl), gpu_buf_destroy(vk, &st->g_perm), gpu_buf_destroy(vk, &st->g_work), gpu_buf_destroy(vk, &st->g_idx);
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &st->g_read[f]);
        sc = sp = 0;
    }
    VkDeviceSize sm = sizeof(uint32_t) * (VkDeviceSize)st->nmw; // (the morph targets: skin.comp reads them)
    if (ok && sm && !gpu_buf_create(vk, &st->g_m, sm, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        sm = 0, st->nmw = 0, st->morph_meshes = 0, memset(st->morph_count, 0, sizeof st->morph_count), memset(st->vcol_first, 0xFF, sizeof st->vcol_first),
        memset(st->tess_first, 0xFF, sizeof st->tess_first);
    ok = ok && gpu_buf_create(vk, &st->staging, sv + si + sc + sp + sm, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!ok) {
        stage_gpu_drop(vk, st);
        return false;
    }
    uint8_t* m = st->staging.mapped;
    memcpy(m, b->v, (size_t)sv);
    memcpy(m + sv, b->idx, (size_t)si);
    if (sc) memcpy(m + sv + si, st->cpu_cl, (size_t)sc), memcpy(m + sv + si + sc, st->cpu_perm, (size_t)sp);
    if (sm) memcpy(m + sv + si + sc + sp, st->mw, (size_t)sm);
    st->gpu_ready = true;
    return true;
}

static void stage_imported(MeshStage* st, AsyncTask* task) {
    MeshBuilder* b = &st->b;
    RMeshScan s = { 0 };
    char exe[512];
    if (g_mesh_root[0]) snprintf(s.root, sizeof s.root, "%s", g_mesh_root);
    else if (re_exe_dir(exe, sizeof exe)) snprintf(s.root, sizeof s.root, "%s/Content", exe);
    else return;
    for (char* c = s.root; *c; c++)
        if (*c == '\\') *c = '/';
    scan_rmesh(&s, s.root, 0);
    if (s.n) qsort(s.files, s.n, sizeof *s.files, by_vpath);
    double t0 = re_time_seconds();
    uint32_t processed = 0, refused = 0;
    for (uint32_t i = 0; i < s.n && st->nmeshes < RE_MAX_MESHES; i++) {
        if (task) {
            async_progress_n(task, (int)i, (int)s.n);
            const char* nm0 = strrchr(s.files[i].vpath, '/');
            async_status(task, "%s", nm0 ? nm0 + 1 : s.files[i].vpath);
        }
        // .rmesh v2 as written; v1 (one corner per triangle) welded and given LODs here (mesh_build.c). The limits: 16 M
        // vertices, 64 M indices (a 2 M triangle model is ~1.1 M vertices welded; v1 kept them unwelded, 6 M)
        MeshBuilt mb;
        uint32_t ver = 0;
        { FILE* f = re_fopen(s.files[i].path, "rb"); uint32_t hv[2] = { 0, 0 }; if (f) { if (fread(hv, 4, 2, f) == 2) ver = hv[1]; fclose(f); } }
        if (!rmesh_read(s.files[i].path, &mb, 16u << 20, 64u << 20)) {
            refused++;
            printf("[RealEngine] aviso: malha não lida (corrompida ou acima de 16 M vértices): %s\n", s.files[i].vpath);
            continue;
        }
        if (ver == 1u) processed++;
        if ((mb.flags & RMESH_SKINNED) && mb.lods.count == 1u) { // (a skin imported before its LODs: made here, 0.9 s for 300 k triangles once, then read from the cache)
            double tl = re_time_seconds();
            if (mesh_skinned_lods(&mb))
                printf("[RealEngine] Skin LODs: %s, %u LODs (%u -> %u triangles) in %.0f ms\n", s.files[i].vpath, mb.lods.count,
                       mb.lods.index_count[0] / 3u, mb.lods.index_count[mb.lods.count - 1] / 3u, (re_time_seconds() - tl) * 1000.0);
            else if (mb.lods.index_count[0] / 3u >= 4096u) // (2026-11-24: a big skin drawn whole at every distance: said)
                printf("[RealEngine] aviso: Skin LODs: %s ficou sem LODs (%u triangulos em toda distancia)\n", s.files[i].vpath, mb.lods.index_count[0] / 3u);
        }
        if (!builder_room(b, mb.nv, mb.ni)) {
            mesh_built_free(&mb);
            refused++;
            continue;
        }
        uint32_t s2 = st->nmeshes++;
        memset(st->cluster_count[s2], 0, sizeof st->cluster_count[s2]);
        st->vcol_first[s2] = UINT32_MAX;
        st->tess_first[s2] = UINT32_MAX;
        if (mb.col) { // its vertex colors (M5: the materials' Color Attribute), after the morphs' words
            uint32_t* nw = st->nmw + mb.nv > st->mw_cap ? realloc(st->mw, sizeof(uint32_t) * (st->mw_cap = (st->nmw + mb.nv) * 2u)) : st->mw;
            if (nw) {
                st->mw = nw;
                st->vcol_first[s2] = (uint32_t)st->nmw;
                memcpy(st->mw + st->nmw, mb.col, sizeof(uint32_t) * mb.nv);
                st->nmw += mb.nv;
            } else
                st->mw_cap = st->nmw;
        }
        st->dag_count[s2] = 0;
        uint32_t dag_ni = 0; // (the DAG's own indices, after the mesh's)
        // 16 k triangles and up: its LODs in clusters (their triangles reordered cluster by cluster; the cache keeps
        // them). A Masked one is drawn by them (cull.comp pass 6): what the opaque meshes hide or the view leaves out
        // costs nothing
        bool skinned = (mb.flags & RMESH_SKINNED) != 0u; // (its instances bend their own vertices: drawn whole)
        st->mesh_skinned[s2] = skinned;
        st->mesh_rigid_bone[s2] = -1;
        if (skinned && !mb.morph.count) { // (2026-11-24) one bone holding every vertex whole: a rigid skin (its bones in uv2's bits, skin.comp)
            int rb = -1;
            for (uint32_t q = 0; q < mb.nv && rb != -2; q++) {
                uint32_t bx, by;
                memcpy(&bx, &mb.v[q].uv2[0], 4), memcpy(&by, &mb.v[q].uv2[1], 4);
                int b = -1;
                for (int k = 0; k < 4 && b < 0; k++)
                    if (((by >> (8 * k)) & 255u) >= 254u) b = (int)((bx >> (8 * k)) & 255u); // (its whole weight on that bone)
                rb = b < 0 || (rb >= 0 && rb != b) ? -2 : b;
            }
            if (rb >= 0) st->mesh_rigid_bone[s2] = (int16_t)rb;
        }
        if (skinned) st->skinned_nv += mb.nv;
        st->morph_count[s2] = 0;
        if (skinned && mb.morph.count && mb.morph.nv == mb.nv) { // its morph targets: the CSR, the deltas, the names
            size_t words = (size_t)mb.nv + 1u + (size_t)mb.morph.nd * (sizeof(RMeshMorphDelta) / 4u);
            uint32_t* nw = st->nmw + words > st->mw_cap ? realloc(st->mw, sizeof(uint32_t) * (st->mw_cap = (st->nmw + words) * 2u)) : st->mw;
            char(*nn)[RMESH_MORPH_NAME] = nw ? realloc(st->morph_names, RMESH_MORPH_NAME * ((size_t)st->nmorph_names + mb.morph.count)) : NULL;
            if (nw) st->mw = nw;
            if (nn) {
                st->morph_names = nn;
                memcpy(nn + st->nmorph_names, mb.morph.name, RMESH_MORPH_NAME * (size_t)mb.morph.count);
                st->morph_name0[s2] = st->nmorph_names, st->nmorph_names += mb.morph.count;
                st->morph_first[s2] = (uint32_t)st->nmw;
                memcpy(st->mw + st->nmw, mb.morph.first, sizeof(uint32_t) * ((size_t)mb.nv + 1u));
                st->nmw += (size_t)mb.nv + 1u;
                st->morph_delta[s2] = (uint32_t)st->nmw;
                memcpy(st->mw + st->nmw, mb.morph.d, sizeof(RMeshMorphDelta) * mb.morph.nd);
                st->nmw += (size_t)mb.morph.nd * (sizeof(RMeshMorphDelta) / 4u);
                st->morph_count[s2] = mb.morph.count, st->morph_meshes++;
            } else
                st->mw_cap = st->nmw; // (no memory: the mesh without its morphs)
        }
        // (2026-11-24, item 4) the mesh's own settings beside it (.<file>.rlod, the Static / Skeletal Mesh viewer's): Nanite
        // (clusters) automatic / on / off, its rays' fallback share, its Collision Complexity. A skin's DAG serves all its
        // instances (a live one's clusters widened by its pose's reach, cull.comp pass 6; its Masked clusters only off it)
        int side_nanite = -1, side_coll = 0;
        float side_fb = 100.0f;
        mesh_side_read(s.files[i].path, &side_nanite, &side_fb, &side_coll);
        if (side_nanite < 0 && getenv("REALENGINE_NANITE_ALL")) side_nanite = 1; // (the tests: every mesh's "On")
        st->mesh_collision[s2] = (uint8_t)side_coll, st->mesh_fallback[s2] = side_fb;
        // (Auto: from 16 384 triangles; a Skeletal Mesh from 4096: its instances come in crowds, the Unamed's 5000 capsules)
        uint32_t auto_min = (mb.flags & RMESH_SKINNED) ? 4096u : 16384u;
        bool want_cl = side_nanite == 1 ? mb.lods.index_count[0] >= 512u * 3u : side_nanite == 0 ? false : mb.lods.index_count[0] >= auto_min * 3u;
        if (mb.lods.count && want_cl) {
            MeshCluster* mc;
            uint32_t* mp;
            uint32_t cfirst[RMESH_MAX_LODS], ccount[RMESH_MAX_LODS];
            uint32_t n = mesh_built_clusters(&mb, &mc, &mp, cfirst, ccount);
            if (n && st->ncl + n > st->cl_cap) {
                uint32_t cap = (st->ncl + n) * 2u;
                GpuCluster* g = realloc(st->cpu_cl, sizeof(GpuCluster) * cap);
                if (g) st->cpu_cl = g;
                uint32_t* pp = g ? realloc(st->cpu_perm, sizeof(uint32_t) * 6 * cap) : NULL;
                if (pp) st->cpu_perm = pp, st->cl_cap = cap;
                else n = 0;
            }
            for (uint32_t l = 0; n && l < MAX_LODS && l < mb.lods.count; l++)
                st->cluster_first[s2][l] = st->ncl + cfirst[l], st->cluster_count[s2][l] = ccount[l];
            for (uint32_t c = 0; c < n; c++) {
                GpuCluster* g = &st->cpu_cl[st->ncl + c];
                memcpy(g->sphere, mc[c].sphere, sizeof g->sphere);
                g->first_index = b->ni + mc[c].first, g->index_count = mc[c].count;
                g->self_err = 0.0f, g->parent_err = FLT_MAX; // (a LOD's clusters: always drawn when seen)
                memset(g->self_sphere, 0, sizeof g->self_sphere), memset(g->parent_sphere, 0, sizeof g->parent_sphere);
            }
            if (n) memcpy(st->cpu_perm + (size_t)st->ncl * 6, mp, sizeof(uint32_t) * 6 * n);
            st->ncl += n;
            free(mc), free(mp);
            // its cluster LOD DAG (mesh_clod.h, the cache keeps it): an opaque instance near the camera drawn by it, the
            // near side fine and the far side coarse, the hidden and the outside clusters skipped (cull.comp pass 6)
            MeshDagCluster* dc;
            uint32_t *dp, *di, dni;
            // (not a mesh of loose pieces, cards: their DAG simplifies nothing, their LODs thin. 2026-11-25: counted by its
            // islands joined by position, over one in 8 triangles; the vertices' count took the UV and normal seams for
            // pieces: the Unamed capsule, 9949 vertices for 9284 triangles in 115 shells, had no DAG)
            bool pieces = mesh_islands(&mb, 0u, NULL) * 8u > mb.lods.index_count[0] / 3u;
            // (Q17) the copies of a vertex a hair apart (a UV seam's, a pole's: a generated sphere's seam at cos(2 pi))
            // moved onto the first one before the DAG: clusterlod welds by the exact position, so both sides of such a
            // seam simplify together (else T-junctions along it, which displacement opens into cracks). The canonical
            // table is Nanite Tessellation's too (tess.glsl)
            uint32_t tw = 0;
            uint32_t* tt = getenv("REALENGINE_NO_DAG") || pieces ? NULL : tess_canon_table(mb.v, mb.nv, mb.idx, mb.lods.index_count[0], &tw);
            for (uint32_t k = 0; tt && k < mb.nv; k++) {
                uint32_t c = tt[2u + k] & 0x7FFFFFFFu;
                if (c != k) memcpy(mb.v[k].pos, mb.v[c].pos, sizeof mb.v[k].pos);
            }
            uint32_t nd = getenv("REALENGINE_NO_DAG") || pieces ? 0u : mesh_built_dag(&mb, &dc, &dp, &di, &dni);
            if (nd && !builder_room(b, mb.nv, mb.ni + dni)) nd = 0, free(dc), free(dp), free(di);
            if (nd && st->ncl + nd > st->cl_cap) {
                uint32_t cap = (st->ncl + nd) * 2u;
                GpuCluster* g = realloc(st->cpu_cl, sizeof(GpuCluster) * cap);
                if (g) st->cpu_cl = g;
                uint32_t* pp = g ? realloc(st->cpu_perm, sizeof(uint32_t) * 6 * cap) : NULL;
                if (pp) st->cpu_perm = pp, st->cl_cap = cap;
                else nd = 0, free(dc), free(dp), free(di);
            }
            if (nd) {
                for (uint32_t c = 0; c < nd; c++) {
                    GpuCluster* g = &st->cpu_cl[st->ncl + c];
                    memcpy(g->sphere, dc[c].sphere, sizeof g->sphere);
                    g->first_index = b->ni + mb.ni + dc[c].first, g->index_count = dc[c].count;
                    g->self_err = dc[c].self_err, g->parent_err = dc[c].parent_err;
                    memcpy(g->self_sphere, dc[c].self_sphere, sizeof g->self_sphere), memcpy(g->parent_sphere, dc[c].parent_sphere, sizeof g->parent_sphere);
                }
                memcpy(st->cpu_perm + (size_t)st->ncl * 6, dp, sizeof(uint32_t) * 6 * nd);
                memcpy(b->idx + b->ni + mb.ni, di, sizeof(uint32_t) * dni);
                st->dag_first[s2] = st->ncl, st->dag_count[s2] = nd, st->any_dag = true;
                st->ncl += nd, dag_ni = dni;
                free(dc), free(dp), free(di);
                // (Q17) its canonical table: Nanite Tessellation displaces the copies of a vertex (a UV seam, a hard edge)
                // the same way, no crack (tess.glsl)
                uint32_t* nw = tt && st->nmw + tw > st->mw_cap ? realloc(st->mw, sizeof(uint32_t) * (st->mw_cap = (st->nmw + tw) * 2u)) : st->mw;
                if (tt && nw) {
                    st->mw = nw;
                    st->tess_first[s2] = (uint32_t)st->nmw;
                    memcpy(st->mw + st->nmw, tt, sizeof(uint32_t) * tw);
                    st->nmw += tw;
                } else if (tt)
                    st->mw_cap = st->nmw;
            }
            free(tt);
        }
        memcpy(b->v + b->nv, mb.v, sizeof(GpuVertex) * mb.nv);
        memcpy(b->idx + b->ni, mb.idx, sizeof(uint32_t) * mb.ni);
        st->lod_count[s2] = mb.lods.count < MAX_LODS ? mb.lods.count : MAX_LODS;
        memset(st->lod_error[s2], 0, sizeof st->lod_error[s2]);
        for (uint32_t l = 0; l < st->lod_count[s2]; l++) {
            Mesh* lm = &st->lods[s2][l];
            lm->first_index = b->ni + mb.lods.first_index[l], lm->index_count = mb.lods.index_count[l], lm->vertex_offset = (int32_t)b->nv;
            memcpy(lm->mn, mb.mn, 12), memcpy(lm->mx, mb.mx, 12);
            st->lod_error[s2][l] = mb.lods.error[l];
        }
        st->meshes[s2] = st->lods[s2][0];
        st->mesh_atlas[s2] = (mb.flags & RMESH_NO_ATLAS) == 0u;
        st->mesh_error_lod[s2] = true;
        st->mesh_id[s2] = render_mesh_id_of(s.files[i].vpath);
        st->mesh_area[s2] = mb.area > 1.0f ? mb.area : 10000.0f;
        const char* nm = strrchr(s.files[i].vpath, '/');
        snprintf(st->mesh_name[s2], sizeof st->mesh_name[s2], "%s", nm ? nm + 1 : s.files[i].vpath);
        snprintf(st->mesh_vpath[s2], sizeof st->mesh_vpath[s2], "%s", s.files[i].vpath);
        char* dot = strrchr(st->mesh_name[s2], '.');
        if (dot) *dot = 0;
        b->nv += mb.nv, b->ni += mb.ni + dag_ni;
        mesh_built_free(&mb);
        if (task && async_cancelled(task)) break;
    }
    free(s.files);
    if (st->nmeshes > SHAPE_COUNT)
        printf("[RealEngine] Imported meshes: %u (%u .rmesh v1 welded and given LODs here, %u refused), %.0f ms\n", st->nmeshes - SHAPE_COUNT,
               processed, refused, (re_time_seconds() - t0) * 1000.0);
}

// (Q21) the impostors' card (farview.c): a unit square every impostor draws (one draw for all of them; mesh.vert turns it
// toward the camera at the instance's bounding sphere), after the imported meshes (their slots stay)
#define CARD_MESH_ID 0xFA21CA2Du
static void stage_card(MeshStage* st) {
    MeshBuilder* b = &st->b;
    if (st->nmeshes >= RE_MAX_MESHES || !builder_room(b, 4, 6)) return;
    uint32_t s = st->nmeshes++, v0 = b->nv, i0 = b->ni;
    build_plane(b);
    for (uint32_t i = i0; i < b->ni; i++) b->idx[i] -= v0;
    st->lod_count[s] = 1;
    Mesh* m = &st->lods[s][0];
    m->first_index = i0, m->index_count = b->ni - i0, m->vertex_offset = (int32_t)v0;
    for (int k = 0; k < 3; k++) m->mn[k] = -50.0f, m->mx[k] = 50.0f;
    memset(st->lod_error[s], 0, sizeof st->lod_error[s]);
    st->meshes[s] = *m;
    st->mesh_id[s] = CARD_MESH_ID;
    st->mesh_atlas[s] = false, st->mesh_error_lod[s] = false;
    snprintf(st->mesh_name[s], sizeof st->mesh_name[s], "ImpostorCard");
    st->mesh_vpath[s][0] = 0;
    st->mesh_area[s] = 10000.0f;
}

static void stage_shapes(MeshStage* st) {
    MeshBuilder* b = &st->b;
    // Each shape and its LODs (the GPU cull picks one by the size on screen; LOD 0 is R.meshes, the BLAS's and the
    // bake's): the round ones halve their segments per level, the cube and the plane have just one
    // (the last one for a few pixels: a field of a million objects seen from above is mostly those)
    static const int segs[MAX_LODS] = { 48, 20, 10, 6 }, rings[MAX_LODS] = { 24, 10, 5, 3 };
    for (int s = 0; s < SHAPE_COUNT; s++) {
        st->lod_count[s] = (s == SHAPE_SPHERE || s == SHAPE_CYLINDER || s == SHAPE_CONE) ? MAX_LODS : 1;
        for (uint32_t l = 0; l < st->lod_count[s]; l++) {
            uint32_t v0 = b->nv, i0 = b->ni;
            switch (s) {
            case SHAPE_CUBE: build_cube(b); break;
            case SHAPE_SPHERE: build_sphere(b, segs[l], rings[l]); break;
            case SHAPE_CYLINDER: build_round(b, false, segs[l]); break;
            case SHAPE_CONE: build_round(b, true, segs[l]); break;
            case SHAPE_PLANE: build_plane(b); break;
            }
            for (uint32_t i = i0; i < b->ni; i++) b->idx[i] -= v0; // indices relative to the mesh's vertex_offset
            Mesh* m = &st->lods[s][l];
            m->first_index = i0;
            m->index_count = b->ni - i0;
            m->vertex_offset = (int32_t)v0;
            // the flat faces of a coarse LOD sink into LOD 0 (which the rays see) by up to their chord's sagitta: a
            // ray from the pixel starts that far out, or it hits its own object (dark dots on far round shapes)
            float sag = 50.0f * (1.0f - cosf(3.14159265f / (float)segs[l]));
            if (s == SHAPE_SPHERE) sag += 50.0f * (1.0f - cosf(3.14159265f / (float)(2 * rings[l])));
            st->lod_error[s][l] = l > 0 && st->lod_count[s] > 1 ? sag : 0.0f;
            for (int k = 0; k < 3; k++) { m->mn[k] = -50.0f; m->mx[k] = 50.0f; }
            if (s == SHAPE_PLANE) { m->mn[2] = -0.5f; m->mx[2] = 0.5f; }
        }
        st->meshes[s] = st->lods[s][0];
        st->mesh_id[s] = (uint32_t)s;
        st->mesh_atlas[s] = true, st->mesh_error_lod[s] = false;
        snprintf(st->mesh_name[s], sizeof st->mesh_name[s], "%s", g_shape_names[s]);
        static const float area1[SHAPE_COUNT] = { 60000.0f, 31416.0f, 47124.0f, 25416.0f, 10000.0f };
        st->mesh_area[s] = area1[s];
    }
    st->nmeshes = SHAPE_COUNT;
}

// The stage into the renderer's tables and the GPU's buffers (the main thread; the stage is freed, its vertices kept)
static bool stage_upload(VkCtx* vk, MeshStage* st) {
    R.nmeshes = st->nmeshes;
    R.imp_card = UINT32_MAX; // (Q21) the impostors' card (stage_card: the last one)
    for (uint32_t s2 = SHAPE_COUNT; s2 < st->nmeshes; s2++)
        if (st->mesh_id[s2] == CARD_MESH_ID) R.imp_card = s2;
    memcpy(R.meshes, st->meshes, sizeof R.meshes);
    memcpy(R.lods, st->lods, sizeof R.lods);
    memcpy(R.lod_count, st->lod_count, sizeof R.lod_count);
    memcpy(R.lod_error, st->lod_error, sizeof R.lod_error);
    memcpy(R.mesh_id, st->mesh_id, sizeof R.mesh_id);
    memcpy(R.mesh_area, st->mesh_area, sizeof R.mesh_area);
    memcpy(R.mesh_atlas, st->mesh_atlas, sizeof R.mesh_atlas);
    memcpy(R.mesh_error_lod, st->mesh_error_lod, sizeof R.mesh_error_lod);
    memcpy(R.mesh_name, st->mesh_name, sizeof R.mesh_name);
    memcpy(R.mesh_vpath, st->mesh_vpath, sizeof R.mesh_vpath);
    memcpy(R.cluster_first, st->cluster_first, sizeof R.cluster_first);
    memcpy(R.cluster_count, st->cluster_count, sizeof R.cluster_count);
    memcpy(R.dag_first, st->dag_first, sizeof R.dag_first);
    memcpy(R.dag_count, st->dag_count, sizeof R.dag_count);
    R.any_dag = st->any_dag;
    R.ncl = st->ncl;
    R.ncmd = R.nmeshes * MAX_LODS;
    MeshBuilder* b = &st->b;
    if (!st->gpu_ready && !stage_gpu_prepare(vk, st)) {
        printf("[RealEngine] FAIL: the meshes' buffers could not be made (%u vertices)\n", b->nv);
        stage_free(st);
        return false;
    }
    if (!st->cl_ok) memset(R.cluster_count, 0, sizeof R.cluster_count), memset(R.dag_count, 0, sizeof R.dag_count), R.any_dag = false, R.ncl = 0;
    R.cpu_cl = NULL, R.cpu_perm = NULL, R.cl_cap = 0;
    R.vertices = st->g_v, R.indices = st->g_i;
    if (st->cl_ok) {
        R.clusters = st->g_cl, R.cl_perm = st->g_perm, R.cl_work = st->g_work, R.cl_idx = st->g_idx, R.cl_idx_cap = 16u << 20;
        memcpy(R.cl_read, st->g_read, sizeof R.cl_read);
    }
    // one submit: the staging into the buffers
    VkDeviceSize sv = sizeof(GpuVertex) * (VkDeviceSize)b->nv, si = sizeof(uint32_t) * (VkDeviceSize)b->ni;
    VkDeviceSize sc = st->cl_ok ? sizeof(GpuCluster) * (VkDeviceSize)st->ncl : 0, sp = st->cl_ok ? sizeof(uint32_t) * 6 * (VkDeviceSize)st->ncl : 0;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy r0 = { 0, 0, sv }, r1 = { sv, 0, si };
    if (sv) vkCmdCopyBuffer(cmd, st->staging.buffer, R.vertices.buffer, 1, &r0);
    if (si) vkCmdCopyBuffer(cmd, st->staging.buffer, R.indices.buffer, 1, &r1);
    if (sc) {
        VkBufferCopy r2 = { sv + si, 0, sc }, r3 = { sv + si + sc, 0, sp };
        vkCmdCopyBuffer(cmd, st->staging.buffer, R.clusters.buffer, 1, &r2);
        vkCmdCopyBuffer(cmd, st->staging.buffer, R.cl_perm.buffer, 1, &r3);
    }
    if (st->nmw) {
        VkBufferCopy r4 = { sv + si + sc + sp, 0, sizeof(uint32_t) * (VkDeviceSize)st->nmw };
        vkCmdCopyBuffer(cmd, st->staging.buffer, st->g_m.buffer, 1, &r4);
    }
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &st->staging);
    gpu_buf_destroy(vk, &R.morphs);
    free(R.morph_names);
    R.morphs = st->g_m, st->g_m = (GpuBuf){ 0 };
    memcpy(R.morph_first, st->morph_first, sizeof R.morph_first), memcpy(R.morph_delta, st->morph_delta, sizeof R.morph_delta);
    memcpy(R.morph_count, st->morph_count, sizeof R.morph_count), memcpy(R.morph_name0, st->morph_name0, sizeof R.morph_name0);
    memcpy(R.vcol_first, st->vcol_first, sizeof R.vcol_first);
    memcpy(R.tess_first, st->tess_first, sizeof R.tess_first);
    R.morph_names = st->morph_names, st->morph_names = NULL, R.morph_meshes = st->morph_meshes;
    if (!R.morphs.buffer) memset(R.morph_count, 0, sizeof R.morph_count), R.morph_meshes = 0, memset(R.vcol_first, 0xFF, sizeof R.vcol_first), memset(R.tess_first, 0xFF, sizeof R.tess_first);
    R.vertex_count = b->nv;
    R.skin_room = st->skin_room;
    memcpy(R.mesh_skinned, st->mesh_skinned, sizeof R.mesh_skinned);
    memcpy(R.mesh_rigid_bone, st->mesh_rigid_bone, sizeof R.mesh_rigid_bone); // (2026-11-24)
    memcpy(R.mesh_collision, st->mesh_collision, sizeof R.mesh_collision);
    printf("[RealEngine] Basic shapes: %u vertices, %u triangles.\n", b->nv, b->ni / 3);
    R.cpu_v = b->v; // kept: the lightmap bake rasterizes the UV2 on the CPU
    R.cpu_i = b->idx;
    b->v = NULL, b->idx = NULL;
    // (2026-11-24) the skins' LOD 0 vertices once here (skin_bind scanned a mesh's indices for every skin not yet bound,
    // every frame: 2080 capsules without room were 49 M reads a frame); a Spline Mesh's or a soft body's mesh on its first
    memset(R.skin_nv, 0, sizeof R.skin_nv);
    for (uint32_t s = 0; s < R.nmeshes && s < RE_MAX_MESHES; s++)
        if (R.mesh_skinned[s]) skin_mesh_nv(s);
    stage_free(st);
    return true;
}

static bool build_meshes(VkCtx* vk) {
    MeshStage* st = stage_new();
    if (!st) return false;
    stage_shapes(st);
    stage_imported(st, NULL);
    stage_card(st);
    return stage_upload(vk, st);
}

// --- Pipelines ---

// The modules' names (SHADER: its array's), for the message when a driver refuses a pipeline (a phone's compiler can
// refuse what the PC's takes: the Adreno 740 did, 2026-11-08; the log then says which shader)
// (2026-11-19: and their SPIR-V, for the pipeline batches: a worker makes its own module of the same code)
static struct { VkShaderModule m; const char* name; const uint32_t* code; size_t bytes; } g_shader_names[256];
static uint32_t g_nshader_names;
static VkShaderModule shader_named(VkCtx* vk, const uint32_t* code, size_t bytes, const char* name) {
    VkShaderModule m = gpu_shader(vk, code, bytes);
    uint32_t i = g_nshader_names++ % 256u;
    g_shader_names[i].m = m, g_shader_names[i].name = name, g_shader_names[i].code = code, g_shader_names[i].bytes = bytes;
    return m;
}
static const uint32_t* shader_code(VkShaderModule m, size_t* bytes) {
    for (uint32_t k = 0; k < 256u && k < g_nshader_names && m; k++) {
        uint32_t i = (g_nshader_names - 1u - k) % 256u;
        if (g_shader_names[i].m == m) return *bytes = g_shader_names[i].bytes, g_shader_names[i].code;
    }
    return *bytes = 0, NULL;
}
static const char* shader_name(VkShaderModule m) {
    for (uint32_t k = 0; k < 256u && k < g_nshader_names && m; k++) { // (the newest first: a destroyed module's handle comes back)
        uint32_t i = (g_nshader_names - 1u - k) % 256u;
        if (g_shader_names[i].m == m) return g_shader_names[i].name;
    }
    return "?";
}

VkShaderModule gpu_shader(VkCtx* vk, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    vk_check(vkCreateShaderModule(vk->device, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}

static void wait_pump_now(void);
VkPipeline gpu_compute_pipeline(VkCtx* vk, VkPipelineLayout layout, const uint32_t* code, size_t bytes) {
    VkShaderModule m = gpu_shader(vk, code, bytes);
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = re_time_seconds();
    vk_check(vkCreateComputePipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &p), "vkCreateComputePipelines");
    vk->pipelines_ms += (re_time_seconds() - t0) * 1000.0, vk->pipelines++;
    vkDestroyShaderModule(vk->device, m, NULL);
    if (re_time_seconds() - t0 > 0.05) wait_pump_now(); // (a cold one: the window answers between them, render_init's)
    return p;
}

typedef struct PipeDesc {
    VkShaderModule vs, fs;
    VkFormat color;           // VK_FORMAT_UNDEFINED: depth only
    VkFormat depth;           // VK_FORMAT_UNDEFINED: none
    VkSampleCountFlagBits samples;
    VkCompareOp depth_op;
    bool depth_write, depth_test, blend, bias, wire;
    VkCullModeFlags cull;
    const char* name; // --shader-stats prints the named ones' (vkctx.h shader_stats)
    bool main, split; // the main pass's: its second target (the shadow split), written (mesh.frag) or kept (sky, grid)
    bool oit;         // the OIT pass's: accum (RGBA16F, added) and revealage (RG16F, r times 1 - opacity), one sample
    bool points;      // a point list (the particles' small sprites)
    bool lines;       // (2026-11-19) a line list (the debug traces' batches: hardware lines)
    bool mask_color;  // nothing written to the color targets (a depth prepass inside the main pass: the hair's)
    bool vrs;         // shaded at the rate image's rate (the opaque meshes; vk->vrs). Every color pass's pipeline takes the
                      // attachment's flag (one drawn in a pass that has the image must), at full rate unless this
} PipeDesc;

// --shader-stats: the driver's numbers for a pipeline's stages (on NVIDIA: the registers, a pixel's occupancy)
static int64_t g_shader_spill = -1; // render_shader_spill
static void shader_stats(VkCtx* vk, VkPipeline p, const char* name) {
    PFN_vkGetPipelineExecutablePropertiesKHR props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(vk->device, "vkGetPipelineExecutablePropertiesKHR");
    PFN_vkGetPipelineExecutableStatisticsKHR stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(vk->device, "vkGetPipelineExecutableStatisticsKHR");
    if (!props || !stats) return;
    if (!strcmp(name, "mesh")) g_shader_spill = -1; // (made again with the MSAA: mesh first, then mesh_rt, mesh_rt_t)
    VkPipelineInfoKHR pi = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, NULL, p };
    VkPipelineExecutablePropertiesKHR ex[4];
    uint32_t nex = 4;
    for (uint32_t i = 0; i < nex; i++) ex[i] = (VkPipelineExecutablePropertiesKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
    if (props(vk->device, &pi, &nex, ex) < 0) return;
    for (uint32_t e = 0; e < nex; e++) {
        bool vert = (ex[e].stages & VK_SHADER_STAGE_VERTEX_BIT) != 0; // (Q22: mesh.vert's and tess.vert's, the "mesh" pipeline's)
        if (!(ex[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT) && !(vert && (!strcmp(name, "mesh") || !strcmp(name, "mesh (tess)")))) continue;
        VkPipelineExecutableInfoKHR ei = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, NULL, p, e };
        VkPipelineExecutableStatisticKHR st[16];
        uint32_t nst = 16;
        for (uint32_t i = 0; i < nst; i++) st[i] = (VkPipelineExecutableStatisticKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR };
        if (stats(vk->device, &ei, &nst, st) < 0) continue;
        char line[1024];
        int n = snprintf(line, sizeof line, "[RealEngine] Shader stats %s (%s):", name, ex[e].name);
        for (uint32_t i = 0; i < nst && n > 0 && n < (int)sizeof line; i++) {
            uint64_t v = st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR ? st[i].value.b32
                         : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? (uint64_t)st[i].value.i64
                         : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? st[i].value.u64
                                                                                              : (uint64_t)st[i].value.f64;
            v &= 0xFFFFFFFFu; // (NVIDIA 595: the local memory's size comes with a bit set at 2^36; the counts all fit 32 bits)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %llu;", st[i].name, (unsigned long long)v);
            if (!vert && !strcmp(st[i].name, "Local Memory Size") && (!strcmp(name, "mesh") || !strcmp(name, "mesh_rt") || !strcmp(name, "mesh_rt_t"))) // (mesh_lsm and the _x ones: only while a local light has a map / a material has extra lobes)
                g_shader_spill = (g_shader_spill < 0 ? 0 : g_shader_spill) + (int64_t)v;
        }
        printf("%s\n", line);
    }
}
int64_t render_shader_spill(void) { return g_shader_spill; }

static VkPipeline pipeline_create(VkCtx* vk, const PipeDesc* d, double* out_ms) {
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = d->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = d->fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d->points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : d->lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = d->wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; // Z-up left-handed world, y-down framebuffer (see mb_tri)
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = d->bias;
    rs.depthBiasConstantFactor = 1.5f;
    rs.depthBiasSlopeFactor = 2.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = d->samples;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = d->depth_test;
    ds.depthWriteEnable = d->depth_write;
    ds.depthCompareOp = d->depth_op;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = d->mask_color ? 0 : d->color == VK_FORMAT_R8_UNORM ? VK_COLOR_COMPONENT_R_BIT : 0xF;
    if (d->blend) { // premultiplied alpha
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendAttachmentState bas[4] = { ba, { 0 }, { 0 }, { 0 } };
    bas[1].colorWriteMask = d->split ? 0xF : 0;
    bas[2].colorWriteMask = bas[3].colorWriteMask = d->split ? 0xF : 0; // (the denoiser's: integers, never blended)
    if (!R.indep_blend) bas[1] = ba; // (the same state required: the split is off then, its writes dropped)
    if (d->oit) { // weighted blended OIT (McGuire and Bavoil 2013): the sums, the product of the transmittances
        bas[0] = (VkPipelineColorBlendAttachmentState){ VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                                                        VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF };
        // (r: the revealage; 2026-11-17, Q4: g, b, a the product of the glass's colored transmittances, each 1 - its tint)
        bas[1] = (VkPipelineColorBlendAttachmentState){ VK_TRUE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_OP_ADD,
                                                        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF };
        bas[2] = bas[0]; // (2026-11-16) the refraction's sums: added like the color
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bool two = (d->main && R.split_ok) || d->oit;
    uint32_t ncol = d->color ? (two ? (d->main && R.den_ok ? 4u : d->oit ? 3u : 2u) : 1u) : 0u;
    cb.attachmentCount = ncol;
    cb.pAttachments = bas;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat formats[4] = { d->color, VK_FORMAT_R16G16B16A16_SFLOAT, // (Q4: the OIT's revealage target is rgba now)
                            d->oit ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT };
    ri.colorAttachmentCount = ncol;
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = d->depth;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    ci.stageCount = d->fs ? 2 : 1;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = R.layout;
    if (vk->shader_stats && d->name) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    // variable rate shading (A9.7b): the main pass's pipelines may be drawn with the rate image bound; the opaque meshes
    // take its rate (the combiner replaces the pipeline's 1x1), the rest keep 1x1
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr = { VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
    // (only while VRS is wanted: the flag alone, unused, changed the driver's code enough to raise the flicker tests)
    if (vk->vrs && R.vrs_built && d->color == VK_FORMAT_R16G16B16A16_SFLOAT && d->depth == VK_FORMAT_D32_SFLOAT) {
        ci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        fsr.fragmentSize = (VkExtent2D){ 1, 1 };
        fsr.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        fsr.combinerOps[1] = d->vrs ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        fsr.pNext = ri.pNext, ri.pNext = &fsr;
    }
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = re_time_seconds(); // (the driver compiles here: a first run's cost, vkctx.h)
    VkResult pr = vkCreateGraphicsPipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &p);
    if (pr != VK_SUCCESS) { // (which: a phone's driver can refuse a shader the PC takes)
        re_log(RE_LOG_ERROR, "pipeline refused (%d): vertex %s, fragment %s", (int)pr, shader_name(d->vs), d->fs ? shader_name(d->fs) : "-");
        vk_check(pr, "vkCreateGraphicsPipelines");
    }
    *out_ms = (re_time_seconds() - t0) * 1000.0;
    return p;
}

// (2026-11-19) Pipeline batches. Every travamento report since 0.18 (crashes/RealEngine_travamento_*: 6 to 30 s, the
// window "not responding" as a project or the Project Browser opened) had the main thread in the driver's compiler,
// one pipeline after another (build_msaa_pipelines, ext_pipelines_ensure) with a cold cache: the first start after an
// update, or one after a start closed while frozen (the cache was saved only at the start and the exit). A batch runs
// the builder twice: first recording each pipeline's description (its shaders' SPIR-V, nothing made), then compiling
// them all on threads, one per core (the driver compiles each one on one thread), while the main thread keeps the
// window answering (render_set_wait_pump) and the watchdog fed; the builder's second run takes the pipeline made for
// the same description instead of compiling it. The cache is saved after a batch that compiled.
typedef struct PbItem {
    PipeDesc d; // (vs, fs: the recording run's modules, gone; the code below)
    const uint32_t *vc, *fc;
    size_t vb, fb;
    VkPipeline p;
    double ms;
    bool taken;
    uint32_t st; // pipe_state() and the layout when recorded: what pipeline_create reads besides the description
    VkPipelineLayout lay;
} PbItem;
static struct {
    int mode;        // 0: none, 1: recording, 2: taking
    uint32_t thread; // (the batch's: the Masked rays' job builds on its own thread meanwhile)
    PbItem* it;
    uint32_t n, cap;
    volatile int32_t next, done;
    VkCtx* vk;
    double t0;
} PB;
// (2026-11-19) the prewarm: the main pass's set recorded at the start and compiled on background threads while the
// intro, the Project Browser and the level's load run (render_prewarm); the first batch takes them (pw_join)
static struct {
    PbItem* it;
    uint32_t n;
    volatile int32_t next, done;
    ReThread* th[32];
    int nth;
    VkCtx* vk;
} PW;
static void (*g_wait_pump)(void);
static uint32_t g_pump_thread; // (glfwPollEvents: the main thread's only)
void render_set_wait_pump(void (*fn)(void)) { g_wait_pump = fn, g_pump_thread = re_thread_id(); }
static void wait_pump_now(void) {
    if (g_wait_pump && re_thread_id() == g_pump_thread) crash_heartbeat(), g_wait_pump();
}
static bool pb_here(int mode) { return PB.mode == mode && PB.thread == re_thread_id(); }
static uint32_t pipe_state(void) { return (uint32_t)R.split_ok | (uint32_t)R.den_ok << 1 | (uint32_t)R.indep_blend << 2 | (uint32_t)R.vrs_built << 3; }
static bool pd_same(const PipeDesc* a, const PipeDesc* b) { // (the modules apart: their code compared by the caller)
    return a->color == b->color && a->depth == b->depth && a->samples == b->samples && a->depth_op == b->depth_op &&
           a->depth_write == b->depth_write && a->depth_test == b->depth_test && a->blend == b->blend && a->bias == b->bias &&
           a->wire == b->wire && a->cull == b->cull && a->main == b->main && a->split == b->split && a->oit == b->oit &&
           a->points == b->points && a->lines == b->lines && a->mask_color == b->mask_color && a->vrs == b->vrs;
}
// The tess twin's vertex shader a pipeline gets (R.tess_twins): 0 none, 1 tess.vert, 2 tess.vert RE_EXT
static int tess_twin_kind(const PipeDesc* d) {
    if (!R.tess_twins || d->depth != VK_FORMAT_D32_SFLOAT || d->points || d->oit || d->bias || !(d->cull == VK_CULL_MODE_BACK_BIT || d->wire) ||
        !(d->color == VK_FORMAT_R16G16B16A16_SFLOAT || (d->color == VK_FORMAT_UNDEFINED && !d->fs)))
        return 0;
    const char* vn = shader_name(d->vs);
    return !strcmp(vn, "s_mesh_vert_x") ? 2 : !strcmp(vn, "s_mesh_vert") ? 1 : 0;
}
static void pb_put(const PipeDesc* d, const uint32_t* vc, size_t vb, const uint32_t* fc, size_t fb) {
    if (PB.n == PB.cap) {
        uint32_t cap = PB.cap ? PB.cap * 2u : 64u;
        PbItem* it = realloc(PB.it, sizeof *it * cap); // (a batch's list: a pipeline build, never a frame's)
        if (!it) return;
        PB.it = it, PB.cap = cap;
    }
    PbItem* e = &PB.it[PB.n++];
    memset(e, 0, sizeof *e);
    e->d = *d, e->vc = vc, e->vb = vb, e->fc = fc, e->fb = fb, e->st = pipe_state(), e->lay = R.layout;
}

static VkPipeline pipeline(VkCtx* vk, const PipeDesc* d) {
    size_t vb = 0, fb = 0;
    const uint32_t* vc = PB.mode ? shader_code(d->vs, &vb) : NULL;
    const uint32_t* fc = PB.mode && d->fs ? shader_code(d->fs, &fb) : NULL;
    if (pb_here(1)) { // recording: its description (and its tess twin's), nothing made
        if (vc && (fc || !d->fs)) {
            pb_put(d, vc, vb, fc, fb);
            int tk = tess_twin_kind(d);
            if (tk) pb_put(d, tk == 2 ? s_tess_vert_x : s_tess_vert, tk == 2 ? sizeof s_tess_vert_x : sizeof s_tess_vert, fc, fb);
        }
        return VK_NULL_HANDLE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    if (pb_here(2) && vc)
        for (uint32_t i = 0; i < PB.n; i++) {
            PbItem* e = &PB.it[i];
            if (!e->taken && e->p && e->vc == vc && e->fc == fc && e->st == pipe_state() && e->lay == R.layout && pd_same(&e->d, d)) {
                p = e->p, e->taken = true;
                break;
            }
        }
    if (!p) {
        double ms = 0.0;
        p = pipeline_create(vk, d, &ms);
        vk->pipelines_ms += ms;
    }
    vk->pipelines++;
    if (vk->shader_stats && d->name) shader_stats(vk, p, d->name);
    // (Q22) its tess twin: an opaque mesh pipeline's copy with tess.vert (the micro vertices: tess_draws), while a
    // displaced material was sent (R.tess_twins; the pipelines made again then). mesh.vert keeps its registers
    int tk = p ? tess_twin_kind(d) : 0;
    if (tk) {
        PipeDesc t = *d;
        t.vs = tk == 2 ? shader_named(vk, s_tess_vert_x, sizeof s_tess_vert_x, "s_tess_vert_x") : shader_named(vk, s_tess_vert, sizeof s_tess_vert, "s_tess_vert"); // (named: a recycled handle isn't taken for mesh.vert)
        t.name = d->name && !strcmp(d->name, "mesh") ? "mesh (tess)" : NULL; // (--shader-stats: tess.vert's registers)
        VkPipeline tw = pipeline(vk, &t);
        vkDestroyShaderModule(vk->device, t.vs, NULL);
        if (tw && !tess_twin_put(p, tw)) vkDestroyPipeline(vk->device, tw, NULL);
    }
    return p;
}

// One batch item on a worker: its own modules of the same code
static void pb_compile(PbItem* e) {
    VkCtx* vk = PB.vk;
    PipeDesc d = e->d;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = e->vb, ci.pCode = e->vc;
    d.vs = d.fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk->device, &ci, NULL, &d.vs) != VK_SUCCESS) return;
    ci.codeSize = e->fb, ci.pCode = e->fc;
    if (!e->fc || vkCreateShaderModule(vk->device, &ci, NULL, &d.fs) == VK_SUCCESS) {
        e->p = pipeline_create(vk, &d, &e->ms);
        if (d.fs) vkDestroyShaderModule(vk->device, d.fs, NULL);
    }
    vkDestroyShaderModule(vk->device, d.vs, NULL);
}
static int pb_worker(void* arg) {
    (void)arg;
    for (int32_t i; (i = re_atomic_add(&PB.next, 1) - 1) < (int32_t)PB.n;) {
        if (!PB.it[i].p) pb_compile(&PB.it[i]); // (else the prewarm's)
        re_atomic_add(&PB.done, 1);
    }
    return 0;
}
static int pw_worker(void* arg) {
    (void)arg;
    for (int32_t i; (i = re_atomic_add(&PW.next, 1) - 1) < (int32_t)PW.n;) {
        PbItem* e = &PW.it[i];
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        PipeDesc d = e->d;
        d.vs = d.fs = VK_NULL_HANDLE;
        ci.codeSize = e->vb, ci.pCode = e->vc;
        if (vkCreateShaderModule(PW.vk->device, &ci, NULL, &d.vs) == VK_SUCCESS) {
            ci.codeSize = e->fb, ci.pCode = e->fc;
            if (!e->fc || vkCreateShaderModule(PW.vk->device, &ci, NULL, &d.fs) == VK_SUCCESS) {
                e->p = pipeline_create(PW.vk, &d, &e->ms);
                if (e->p && (pipe_state() != e->st || R.layout != e->lay)) // (the state changed meanwhile: not that one)
                    vkDestroyPipeline(PW.vk->device, e->p, NULL), e->p = VK_NULL_HANDLE;
                if (d.fs) vkDestroyShaderModule(PW.vk->device, d.fs, NULL);
            }
            vkDestroyShaderModule(PW.vk->device, d.vs, NULL);
        }
        re_atomic_add(&PW.done, 1);
    }
    return 0;
}
// The prewarm ended (waited for, pumping): its pipelines given to the batch's items of the same description (match:
// the batch being taken) or destroyed
static void pw_join(VkCtx* vk, bool match) {
    if (!PW.n) return;
    while (re_atomic_load(&PW.done) < (int32_t)PW.n) {
        wait_pump_now();
        re_sleep_seconds(0.008);
    }
    for (int i = 0; i < PW.nth; i++) re_thread_join(PW.th[i]);
    PW.nth = 0;
    uint32_t given = 0;
    double ms = 0.0;
    for (uint32_t k = 0; k < PW.n; k++) {
        PbItem* w = &PW.it[k];
        ms += w->ms;
        for (uint32_t i = 0; match && w->p && i < PB.n; i++) {
            PbItem* e = &PB.it[i];
            if (!e->p && e->vc == w->vc && e->fc == w->fc && e->st == w->st && e->lay == w->lay && pd_same(&e->d, &w->d)) {
                e->p = w->p, e->ms = w->ms, w->p = VK_NULL_HANDLE, given++;
                break;
            }
        }
        if (w->p) vkDestroyPipeline(vk->device, w->p, NULL), w->p = VK_NULL_HANDLE;
    }
    if (match) printf("[RealEngine] Prewarm: %u of %u pipelines ready before the level (%.0f ms of compiling on the background threads).\n", given, PW.n, ms);
    free(PW.it);
    PW.it = NULL, PW.n = 0;
}
static void pb_begin(void) { PB.n = 0, PB.thread = re_thread_id(), PB.mode = 1, PB.t0 = re_time_seconds(); }
// The recorded ones compiled on threads; the main thread pumps the window's messages meanwhile
static void pb_take(VkCtx* vk) {
    PB.mode = 0;
    pw_join(vk, true); // (the prewarm's, the first time)
    PB.vk = vk;
    PB.next = PB.done = 0;
    int nt = re_cpu_count() - 1;
    if (nt > (int)PB.n) nt = (int)PB.n;
    if (nt > 32) nt = 32;
    ReThread* th[32];
    int started = 0;
    for (int i = 0; i < nt; i++)
        if ((th[started] = re_thread_start(pb_worker, NULL)) != NULL) started++;
    if (!started) pb_worker(NULL);
    while (re_atomic_load(&PB.done) < (int32_t)PB.n) { // (a wait, not a hang: the watchdog told, the window answering)
        wait_pump_now();
        re_sleep_seconds(0.008);
    }
    for (int i = 0; i < started; i++) re_thread_join(th[i]);
    PB.mode = 2;
}
static void pb_end(VkCtx* vk) {
    double wall = (re_time_seconds() - PB.t0) * 1000.0, sum = 0.0;
    uint32_t made = 0, left = 0;
    for (uint32_t i = 0; i < PB.n; i++) {
        PbItem* e = &PB.it[i];
        sum += e->ms, made += e->p != VK_NULL_HANDLE;
        if (e->taken) vk->pipelines_ms += e->ms; // (the taking run counted it made: its time is the thread's)
        else if (e->p) vkDestroyPipeline(vk->device, e->p, NULL), left++;
    }
    PB.mode = 0, PB.thread = 0;
    if (sum > 250.0) { // (a cold cache: what was compiled kept now, not only at the exit)
        printf("[RealEngine] Pipelines: %u compiled on threads in %.0f ms (%.0f ms of compiling; %u unused).\n", made, wall, sum, left);
        vkctx_pipeline_cache_save(vk);
    }
    PB.n = 0;
}
// A builder run as a batch (it runs twice: it may only make pipelines, set them and make / free their modules)
#define PIPE_BATCH(vk, call) do { pb_begin(); call; pb_take(vk); call; pb_end(vk); } while (0)

#define SHADER(vk, arr) shader_named(vk, arr, sizeof(arr), #arr)
#define SHADER_X(vk, arr) (R.toon_spv ? SHADER(vk, arr##_toon) : SHADER(vk, arr)) // (Q11: an RE_EXT one or its RE_TOON twin)

static void destroy_msaa_pipelines(VkCtx* vk) {
    VkPipeline* ps[] = { &R.mesh, &R.mesh_lsm, &R.mesh_lsm_x, &R.mesh_rt, &R.mesh_wire, &R.sky, &R.grid, &R.prepass, &R.mesh_m, &R.mesh_rt_m, &R.prepass_m,
                         &R.mesh_m_eq, &R.mesh_rt_m_eq, &R.mesh_rt_t, &R.mesh_rt_m_t, &R.mesh_rt_m_eq_t, &R.mesh_x, &R.mesh_rt_x, &R.mesh_rt_t_x, &R.mesh_m_x, &R.mesh_m_eq_x, &R.mesh_rt_m_x, &R.mesh_rt_m_eq_x, &R.mesh_rt_m_t_x, &R.mesh_rt_m_eq_t_x, &R.part_sprite, &R.part_mesh, &R.part_point, &R.part_ribbon, &R.water_surf, &R.rain, &R.cloth, &R.hair, &R.hair_depth, &R.splat_pipe, &R.ar_plane, &R.ar_occlude };
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++)
        if (*ps[i]) vkDestroyPipeline(vk->device, *ps[i], NULL), *ps[i] = VK_NULL_HANDLE;
    for (int i = 0; i < 12; i++) // (the denoiser's copies of the ray traced set: made again when it next runs)
        if (R.den_rt[i]) vkDestroyPipeline(vk->device, R.den_rt[i], NULL), R.den_rt[i] = VK_NULL_HANDLE;
    R.den_rt_key = 0;
}

// The ray traced main pass's pipelines (RE_RT: shadows, reflections), with the shadow rays' alpha test (a, RT_ALPHA) or
// without: mesh_rt, mesh_rt_m, mesh_rt_m_eq, then the terminator's three (none without the ray query's barycentrics).
// Reads only the device, its cache and R.layout: also made on a thread (rt_alpha_job)
#define RT_PIPES 12 // (8..11: the masked runs' RE_EXT ones, 2026-11-10; R.den_rt the same count)
static VkPipeline* rt_pipe(int i) {
    VkPipeline* ps[RT_PIPES] = { &R.mesh_rt, &R.mesh_rt_m, &R.mesh_rt_m_eq, &R.mesh_rt_t, &R.mesh_rt_m_t, &R.mesh_rt_m_eq_t, &R.mesh_rt_x, &R.mesh_rt_t_x,
                                 &R.mesh_rt_m_x, &R.mesh_rt_m_eq_x, &R.mesh_rt_m_t_x, &R.mesh_rt_m_eq_t_x };
    return ps[i];
}
// (only_ext: the RE_EXT ones alone, 6..11: ext_pipelines_ensure's, the rest already drawn; 2026-11-10: it made the whole
// set and threw most away, 38 s with a cold cache)
static void build_rt_pipelines_ex(VkCtx* vk, VkSampleCountFlagBits samples, bool a, bool den, bool only_ext, VkPipeline out[RT_PIPES]) {
    memset(out, 0, sizeof(VkPipeline) * RT_PIPES);
    VkShaderModule mv = SHADER(vk, s_mesh_vert);
    // (den: the RE_DEN variants, the unified denoiser's targets written)
#define RT_FS(base) (den ? (a ? SHADER(vk, base##_a_d) : SHADER(vk, base##_d)) : (a ? SHADER(vk, base##_a) : SHADER(vk, base)))
    // (Q11) the RE_EXT ones: their RE_TOON twins once a Toon material was sent (R.toon_spv)
#define RT_FSX(base) (R.toon_spv ? (den ? (a ? SHADER(vk, base##_a_d_toon) : SHADER(vk, base##_d_toon)) : (a ? SHADER(vk, base##_a_toon) : SHADER(vk, base##_toon))) : RT_FS(base))
    VkShaderModule mfr = RT_FS(s_mesh_frag_rt);
    VkShaderModule mfrm = RT_FS(s_mesh_frag_rt_masked);
    VkShaderModule mfre = RT_FS(s_mesh_frag_rt_masked_eq);
    PipeDesc d = { mv, mfr, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true, true,
                   false, false, false, VK_CULL_MODE_BACK_BIT };
    d.main = d.split = true;
    d.name = den ? NULL : "mesh_rt";
    d.vrs = true;
    if (!only_ext) out[0] = pipeline(vk, &d);
    d.name = NULL;
    d.fs = mfrm, d.cull = VK_CULL_MODE_NONE, d.vrs = false;
    if (!only_ext) out[1] = pipeline(vk, &d);
    d.fs = mfre, d.depth_op = VK_COMPARE_OP_EQUAL, d.depth_write = false;
    if (!only_ext) out[2] = pipeline(vk, &d);
    if (vk->ray_query && !only_ext) { // (the barycentrics come with the ray query: vkctx.c)
        VkShaderModule t0 = RT_FS(s_mesh_frag_rt_t);
        VkShaderModule t1 = RT_FS(s_mesh_frag_rt_masked_t);
        VkShaderModule t2 = RT_FS(s_mesh_frag_rt_masked_eq_t);
        PipeDesc dt = { mv, t0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true,
                        true, false, false, false, VK_CULL_MODE_BACK_BIT };
        dt.main = dt.split = true;
        dt.name = den ? NULL : "mesh_rt_t";
        dt.vrs = true;
        out[3] = pipeline(vk, &dt);
        dt.name = NULL;
        dt.fs = t1, dt.cull = VK_CULL_MODE_NONE, dt.vrs = false;
        out[4] = pipeline(vk, &dt);
        dt.fs = t2, dt.depth_op = VK_COMPARE_OP_EQUAL, dt.depth_write = false;
        out[5] = pipeline(vk, &dt);
        vkDestroyShaderModule(vk->device, t0, NULL);
        vkDestroyShaderModule(vk->device, t1, NULL);
        vkDestroyShaderModule(vk->device, t2, NULL);
    }
    VkShaderModule mvx = R.any_ext ? SHADER(vk, s_mesh_vert_x) : VK_NULL_HANDLE; // (RE_EXT's vertices: the vertex color out, M5)
    if (vk->ray_query && R.any_ext) { // (the extra lobes': RE_EXT, only once a material has them: render_ext_pipelines)
        PipeDesc dt = { mvx, VK_NULL_HANDLE, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true,
                        true, false, false, false, VK_CULL_MODE_BACK_BIT };
        dt.main = dt.split = true;
        VkShaderModule tx = RT_FSX(s_mesh_frag_rt_t_x);
        PipeDesc dx = dt;
        dx.fs = tx, dx.cull = VK_CULL_MODE_BACK_BIT, dx.depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL, dx.depth_write = true, dx.name = den ? NULL : "mesh_rt_t_x";
        dx.vrs = true;
        out[7] = pipeline(vk, &dx);
        vkDestroyShaderModule(vk->device, tx, NULL);
    }
    if (R.any_ext) {
        VkShaderModule rx = RT_FSX(s_mesh_frag_rt_x);
        PipeDesc dx = { mvx, rx, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true, true,
                        false, false, false, VK_CULL_MODE_BACK_BIT };
        dx.main = dx.split = true;
        dx.name = den ? NULL : "mesh_rt_x";
        dx.vrs = true;
        out[6] = pipeline(vk, &dx);
        vkDestroyShaderModule(vk->device, rx, NULL);
    }
    if (R.any_ext && R.any_ext_masked) { // (2026-11-10) the masked runs' RE_EXT: both sides, the cut; after the prepass: EQUAL
        VkShaderModule m0 = RT_FSX(s_mesh_frag_rt_masked_x), m1 = RT_FSX(s_mesh_frag_rt_masked_eq_x);
        PipeDesc dm = { mvx, m0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true, true,
                        false, false, false, VK_CULL_MODE_NONE };
        dm.main = dm.split = true;
        out[8] = pipeline(vk, &dm);
        dm.fs = m1, dm.depth_op = VK_COMPARE_OP_EQUAL, dm.depth_write = false;
        out[9] = pipeline(vk, &dm);
        vkDestroyShaderModule(vk->device, m0, NULL);
        vkDestroyShaderModule(vk->device, m1, NULL);
        if (vk->ray_query) {
            VkShaderModule t0 = RT_FSX(s_mesh_frag_rt_masked_t_x), t1 = RT_FSX(s_mesh_frag_rt_masked_eq_t_x);
            dm.fs = t0, dm.depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL, dm.depth_write = true;
            out[10] = pipeline(vk, &dm);
            dm.fs = t1, dm.depth_op = VK_COMPARE_OP_EQUAL, dm.depth_write = false;
            out[11] = pipeline(vk, &dm);
            vkDestroyShaderModule(vk->device, t0, NULL);
            vkDestroyShaderModule(vk->device, t1, NULL);
        }
    }
#undef RT_FS
    vkDestroyShaderModule(vk->device, mfr, NULL);
    vkDestroyShaderModule(vk->device, mfrm, NULL);
    vkDestroyShaderModule(vk->device, mfre, NULL);
    vkDestroyShaderModule(vk->device, mv, NULL);
    if (mvx) vkDestroyShaderModule(vk->device, mvx, NULL);
}
static void build_rt_pipelines(VkCtx* vk, VkSampleCountFlagBits samples, bool a, bool den, VkPipeline out[RT_PIPES]) {
    build_rt_pipelines_ex(vk, samples, a, den, false, out);
}

// The alpha-tested variants made on a thread when the first Masked mesh is drawn. They were rebuilt on the main thread
// in that frame, and with a cold pipeline cache (the first start after an update: the shaders changed) the driver
// compiled for over 6 s: the window stopped responding (0.18, RealEngine_travamento_2026-11-04: opening the RPG demo,
// its foliage Masked). Until they are in, the shadow rays stay opaque (GI_RT_MASKED off), as before a Masked mesh
static struct {
    ReThread* thread;
    VkCtx vk; // (a copy: its counters the thread's own)
    VkSampleCountFlagBits samples;
    VkPipeline p[RT_PIPES];
    volatile int32_t done;
    double t0, pipelines_ms0;
    uint32_t pipelines0;
} RJ;
static bool den_rt_job_running(void);
static bool ext_job_running(void); // (2026-11-25: the RE_EXT set's job)
bool render_pipelines_pending(void) { return RJ.thread != NULL || den_rt_job_running() || ext_job_running(); } // (the measures wait: the Masked rays' pipelines change the frame's cost)
static int rt_alpha_job(void* arg) {
    (void)arg; // (normal priority: below it, the editor's uncapped frames starved it past 25 s)
    build_rt_pipelines(&RJ.vk, RJ.samples, true, false, RJ.p);
    re_atomic_store(&RJ.done, 1);
    return 0;
}
static void destroy_rt_set(VkCtx* vk, VkPipeline p[RT_PIPES]) {
    for (int i = 0; i < RT_PIPES; i++)
        if (p[i]) vkDestroyPipeline(vk->device, p[i], NULL), p[i] = VK_NULL_HANDLE;
}
// (waits: the job's pipelines thrown away, the caller makes its own)
static void rt_alpha_job_cancel(VkCtx* vk) {
    if (!RJ.thread) return;
    while (!re_atomic_load(&RJ.done)) crash_heartbeat(), re_sleep_seconds(0.01); // (a wait, not a hang: the watchdog told)
    re_thread_join(RJ.thread);
    RJ.thread = NULL;
    destroy_rt_set(vk, RJ.p);
}
// The alpha-tested set in place of the drawn one
static void install_rt_alpha(VkCtx* vk, VkPipeline p[RT_PIPES]) {
    vkDeviceWaitIdle(vk->device); // (the old ones may be in a frame in flight)
    for (int i = 0; i < RT_PIPES; i++) {
        if (*rt_pipe(i)) vkDestroyPipeline(vk->device, *rt_pipe(i), NULL);
        *rt_pipe(i) = p[i], p[i] = VK_NULL_HANDLE;
    }
    R.pipe_rt_alpha = true;
}
// Each frame: the job's pipelines in once made, for the samples still drawn
static void rt_alpha_job_poll(VkCtx* vk) {
    if (!RJ.thread || !re_atomic_load(&RJ.done)) return;
    re_thread_join(RJ.thread);
    RJ.thread = NULL;
    vk->pipelines += RJ.vk.pipelines - RJ.pipelines0, vk->pipelines_ms += RJ.vk.pipelines_ms - RJ.pipelines_ms0;
    if (RJ.samples != R.pipe_samples || R.pipe_rt_alpha) { // (made again meanwhile: for another MSAA)
        destroy_rt_set(vk, RJ.p);
        printf("[RealEngine] Masked shadow rays: the background pipelines dropped (MSAA changed).\n");
        return;
    }
    install_rt_alpha(vk, RJ.p);
    printf("[RealEngine] Masked shadow rays: their pipelines made in the background in %.0f ms.\n", (re_time_seconds() - RJ.t0) * 1000.0);
}
static void rt_alpha_job_start(VkCtx* vk) {
    if (RJ.thread || !gi_available()) return;
    RJ.vk = *vk;
    RJ.pipelines0 = vk->pipelines, RJ.pipelines_ms0 = vk->pipelines_ms;
    RJ.samples = R.pipe_samples;
    RJ.done = 0;
    RJ.t0 = re_time_seconds();
    RJ.thread = re_thread_start(rt_alpha_job, NULL);
    printf("[RealEngine] Masked shadow rays: their pipelines being made in the background (MSAA %dx).\n", (int)RJ.samples);
    if (!RJ.thread) { // (no thread: made here, as before)
        build_rt_pipelines(vk, R.pipe_samples, true, false, RJ.p);
        install_rt_alpha(vk, RJ.p);
    }
}

// The Principled's extra lobes' pipelines (RE_EXT), made the first frame a material with them is sent: the plain ones
// alone until then (startup and memory pay nothing for a project without coat, anisotropy or subsurface)
// The masked runs' raster RE_EXT pair (while a Masked / Two Sided material has the lobes)
static void ext_masked_raster(VkCtx* vk, VkSampleCountFlagBits samples, VkPipeline* m_x, VkPipeline* m_eq_x) {
    VkShaderModule vx = SHADER(vk, s_mesh_vert_x), f0 = SHADER_X(vk, s_mesh_frag_masked_x), f1 = SHADER_X(vk, s_mesh_frag_masked_eq_x);
    PipeDesc dm = { vx, f0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true, true,
                    false, false, false, VK_CULL_MODE_NONE };
    dm.main = dm.split = true;
    dm.name = "mesh_m_x";
    *m_x = pipeline(vk, &dm);
    dm.name = NULL;
    dm.fs = f1, dm.depth_op = VK_COMPARE_OP_EQUAL, dm.depth_write = false; // (after the prepass: its depth already cut)
    *m_eq_x = pipeline(vk, &dm);
    vkDestroyShaderModule(vk->device, vx, NULL);
    vkDestroyShaderModule(vk->device, f0, NULL);
    vkDestroyShaderModule(vk->device, f1, NULL);
}
// The RE_EXT set: the ones missing when asked (the flags), made into out (the main thread or the job below)
typedef struct ExtSet {
    VkPipeline mesh_x, lsm_x, m_x, m_eq_x, rt[RT_PIPES];
    bool want_m, want_x, want_lsm, want_rt, rt_alpha;
    VkSampleCountFlagBits samples;
} ExtSet;
static bool ext_wanted(ExtSet* s) { // (false: nothing missing)
    memset(s, 0, sizeof *s);
    bool need_m = R.any_ext_masked && (!R.mesh_m_x || (gi_available() && !R.mesh_rt_m_x));
    if (!R.any_ext || !R.pipe_samples || (!need_m && R.mesh_x && R.mesh_lsm_x && (!gi_available() || R.mesh_rt_x))) return false;
    s->want_m = R.any_ext_masked && !R.mesh_m_x;
    s->want_x = !R.mesh_x, s->want_lsm = !R.mesh_lsm_x;
    s->want_rt = gi_available() && (!R.mesh_rt_x || (R.any_ext_masked && !R.mesh_rt_m_x));
    s->rt_alpha = R.pipe_rt_alpha, s->samples = R.pipe_samples;
    return true;
}
static void ext_build(VkCtx* vk, ExtSet* s) {
    if (s->want_m) ext_masked_raster(vk, s->samples, &s->m_x, &s->m_eq_x);
    if (s->want_x) {
        PipeDesc d = { SHADER(vk, s_mesh_vert_x), SHADER_X(vk, s_mesh_frag_x), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, s->samples,
                       VK_COMPARE_OP_GREATER_OR_EQUAL, true, true, false, false, false, VK_CULL_MODE_BACK_BIT };
        d.main = d.split = true;
        d.name = "mesh_x";
        d.vrs = true;
        s->mesh_x = pipeline(vk, &d);
        vkDestroyShaderModule(vk->device, d.vs, NULL);
        vkDestroyShaderModule(vk->device, d.fs, NULL);
    }
    if (s->want_lsm) { // (Q11: the maps' and SSR's variant with them, Low / no rays)
        PipeDesc d = { SHADER(vk, s_mesh_vert_x), SHADER_X(vk, s_mesh_frag_lsm_x), VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, s->samples,
                       VK_COMPARE_OP_GREATER_OR_EQUAL, true, true, false, false, false, VK_CULL_MODE_BACK_BIT };
        d.main = d.split = true;
        d.name = "mesh_lsm_x";
        d.vrs = true;
        s->lsm_x = pipeline(vk, &d);
        vkDestroyShaderModule(vk->device, d.vs, NULL);
        vkDestroyShaderModule(vk->device, d.fs, NULL);
    }
    if (s->want_rt) build_rt_pipelines_ex(vk, s->samples, s->rt_alpha, false, true, s->rt); // (its RE_EXT ones alone)
}
// Into R where still missing (the rest destroyed: made meanwhile by another path)
static void ext_install(VkCtx* vk, ExtSet* s) {
#define EXT_PUT(dst, src) do { if ((src) && !(dst)) (dst) = (src); else if (src) vkDestroyPipeline(vk->device, (src), NULL); (src) = VK_NULL_HANDLE; } while (0)
    EXT_PUT(R.mesh_m_x, s->m_x);
    EXT_PUT(R.mesh_m_eq_x, s->m_eq_x);
    EXT_PUT(R.mesh_x, s->mesh_x);
    EXT_PUT(R.mesh_lsm_x, s->lsm_x);
    for (int i = 0; i < RT_PIPES; i++) {
        if (i >= 6) EXT_PUT(*rt_pipe(i), s->rt[i]);
        else if (s->rt[i]) vkDestroyPipeline(vk->device, s->rt[i], NULL), s->rt[i] = VK_NULL_HANDLE;
    }
#undef EXT_PUT
}
static void ext_drop(VkCtx* vk, ExtSet* s) {
    VkPipeline* all[4] = { &s->mesh_x, &s->lsm_x, &s->m_x, &s->m_eq_x };
    for (int i = 0; i < 4; i++)
        if (*all[i]) vkDestroyPipeline(vk->device, *all[i], NULL), *all[i] = VK_NULL_HANDLE;
    destroy_rt_set(vk, s->rt);
}
static void ext_pipelines_ensure_now(VkCtx* vk) {
    ExtSet s;
    if (!ext_wanted(&s)) return;
    double t0 = re_time_seconds();
    ext_build(vk, &s);
    if (!pb_here(1)) ext_install(vk, &s);
    if (!pb_here(1)) printf("[RealEngine] The extra lobes' pipelines (RE_EXT) made in %.0f ms.\n", (re_time_seconds() - t0) * 1000.0);
}
static void ext_pipelines_ensure(VkCtx* vk) { PIPE_BATCH(vk, ext_pipelines_ensure_now(vk)); }

// (2026-11-25, bug-fixer) The RE_EXT set made on a thread when a frame first needs it (Rain placed: 257 ms of its frame
// went making it, 300-450 ms frames in the UI sweep's Rain; a Light Function, a Force Toon light, a material with extra
// lobes the same). Until it is in, the plain pipelines draw (the main pass picks the RE_EXT ones only once they exist):
// the rain wets the surfaces a few frames later
static struct {
    ReThread* thread;
    VkCtx vk; // (a copy: its counters the thread's own)
    ExtSet s;
    bool toon;
    volatile int32_t done;
    double t0;
} EJ;
static int ext_job(void* arg) {
    (void)arg;
    ext_build(&EJ.vk, &EJ.s);
    re_atomic_store(&EJ.done, 1);
    return 0;
}
static bool ext_job_running(void) { return EJ.thread != NULL; }
static void ext_job_cancel(VkCtx* vk) { // (waits: its pipelines thrown away)
    if (!EJ.thread) return;
    while (!re_atomic_load(&EJ.done)) crash_heartbeat(), re_sleep_seconds(0.005);
    re_thread_join(EJ.thread);
    EJ.thread = NULL;
    ext_drop(vk, &EJ.s);
}
static void ext_job_poll(VkCtx* vk) {
    if (!EJ.thread || !re_atomic_load(&EJ.done)) return;
    re_thread_join(EJ.thread);
    EJ.thread = NULL;
    if (EJ.s.samples != R.pipe_samples || EJ.toon != R.toon_spv) { // (made again meanwhile: another MSAA or the toon twins)
        ext_drop(vk, &EJ.s);
        return;
    }
    ext_install(vk, &EJ.s);
    printf("[RealEngine] The extra lobes' pipelines (RE_EXT) made in the background in %.0f ms.\n", (re_time_seconds() - EJ.t0) * 1000.0);
}
// The frame's ask: started on the job (no thread: made here, as before)
static void ext_pipelines_request(VkCtx* vk) {
    ext_job_poll(vk);
    if (EJ.thread) return;
    ExtSet s;
    if (!ext_wanted(&s)) return;
    EJ.vk = *vk, EJ.s = s, EJ.toon = R.toon_spv, EJ.done = 0, EJ.t0 = re_time_seconds();
    EJ.thread = re_thread_start(ext_job, NULL);
    if (!EJ.thread) ext_pipelines_ensure(vk);
}

// The main pass's pipelines depend on the sample count: rebuilt when it changes
static void build_msaa_pipelines_now(VkCtx* vk, VkSampleCountFlagBits samples) {
    rt_alpha_job_cancel(vk);
    ext_job_cancel(vk); // (its set for the old MSAA: thrown away)
    R.vrs_built = vk->vrs && vrs_wanted(); // (VRS wanted: its flag in the pipelines)
    destroy_msaa_pipelines(vk);
    R.pipe_rt_alpha = R.any_masked;
    VkShaderModule mv = SHADER(vk, s_mesh_vert);
    VkShaderModule mf = SHADER(vk, s_mesh_frag);
    VkShaderModule fv = SHADER(vk, s_fullscreen_vert), sf = SHADER(vk, s_sky_frag), gf = SHADER(vk, s_grid_frag);
    // the prepass: depth only (the early list: the Hi-Z's occluders); the main pass then shades each pixel once, its
    // depth equal to the prepass's or nearer (reversed Z)
    PipeDesc pre = { mv, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER, true, true,
                     false, false, false, VK_CULL_MODE_BACK_BIT };
    R.prepass = pipeline(vk, &pre);
    // the masked runs: both sides rasterized, the fragment shaders cut (masked.frag, mesh.frag RE_MASKED)
    VkShaderModule kf = SHADER(vk, s_masked_frag), mfm = SHADER(vk, s_mesh_frag_masked), mfe = SHADER(vk, s_mesh_frag_masked_eq);
    PipeDesc prem = pre;
    prem.fs = kf, prem.cull = VK_CULL_MODE_NONE;
    R.prepass_m = pipeline(vk, &prem);
    PipeDesc d = { mv, mf, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL, true, true,
                   false, false, false, VK_CULL_MODE_BACK_BIT };
    d.main = d.split = true; // (mesh.frag writes o_split: zero without RE_RT)
    d.vrs = true; // (the rate image's rate: A9.7b)
    d.name = "mesh";
    R.mesh = pipeline(vk, &d);
    { // (its local shadow maps' variant: printed by --shader-stats, its spill not in the test's sum: notas-dev/luz.md)
        VkShaderModule mfl = SHADER(vk, s_mesh_frag_lsm);
        PipeDesc dl = d;
        dl.fs = mfl, dl.name = "mesh_lsm";
        R.mesh_lsm = pipeline(vk, &dl);
        vkDestroyShaderModule(vk->device, mfl, NULL);
        if (R.any_ext) { // (the extra lobes': RE_EXT; else made when one appears)
            VkShaderModule mfx = SHADER_X(vk, s_mesh_frag_x), mvx = SHADER(vk, s_mesh_vert_x);
            dl.fs = mfx, dl.vs = mvx, dl.name = "mesh_x"; // (Q11: its own vertex shader, the vertex color out: mesh.vert left location 5 unwritten)
            R.mesh_x = pipeline(vk, &dl);
            vkDestroyShaderModule(vk->device, mfx, NULL), vkDestroyShaderModule(vk->device, mvx, NULL);
        }
    }
    d.name = NULL;
    PipeDesc dm = d;
    dm.fs = mfm, dm.cull = VK_CULL_MODE_NONE, dm.vrs = false;
    R.mesh_m = pipeline(vk, &dm);
    dm.fs = mfe, dm.depth_op = VK_COMPARE_OP_EQUAL, dm.depth_write = false; // (after the prepass: its depth already cut)
    R.mesh_m_eq = pipeline(vk, &dm);
    if (R.any_ext && R.any_ext_masked) ext_masked_raster(vk, samples, &R.mesh_m_x, &R.mesh_m_eq_x); // (2026-11-10: else made when one appears)
    if (gi_available()) { // the ray traced shadows and reflections in a pipeline of their own: their ray query code
        // takes registers (occupancy) from every pixel, so it runs only when they are on
        VkPipeline p[RT_PIPES];
        build_rt_pipelines(vk, samples, R.pipe_rt_alpha, false, p);
        for (int i = 0; i < RT_PIPES; i++) *rt_pipe(i) = p[i];
    }
    vkDestroyShaderModule(vk->device, kf, NULL);
    vkDestroyShaderModule(vk->device, mfm, NULL);
    vkDestroyShaderModule(vk->device, mfe, NULL);
    if (R.wire_ok) {
        d.wire = true;
        d.cull = VK_CULL_MODE_NONE;
        R.mesh_wire = pipeline(vk, &d);
    }
    PipeDesc sky = { fv, sf, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL,
                     false, true, false, false, false, VK_CULL_MODE_NONE };
    sky.main = true; // (its second target left as cleared: nothing to add)
    R.sky = pipeline(vk, &sky);
    { // AR's occlusion: the sky's place, its depth written (ar_occlude.frag)
        PipeDesc oc = sky;
        oc.fs = SHADER(vk, s_ar_occlude_frag), oc.depth_write = true;
        R.ar_occlude = pipeline(vk, &oc);
        vkDestroyShaderModule(vk->device, oc.fs, NULL);
    }
    PipeDesc grid = sky;
    grid.fs = gf;
    grid.depth_op = VK_COMPARE_OP_GREATER;
    grid.blend = true;
    R.grid = pipeline(vk, &grid);
    { // the particles (particles.c): meshes with their depth, sprites blended over (premultiplied: over or added)
        VkShaderModule pv = SHADER(vk, s_particle_vert), pf = SHADER(vk, s_particle_frag);
        VkShaderModule mv2 = SHADER(vk, s_particle_mesh_vert), mf2 = SHADER(vk, s_particle_mesh_frag);
        PipeDesc ps = { pv, pf, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL,
                        false, true, true, false, false, VK_CULL_MODE_NONE };
        ps.main = true, ps.name = "particles";
        R.part_sprite = pipeline(vk, &ps);
        { // (B10) the ribbons: the sprites' state, their own strip
            VkShaderModule rv = SHADER(vk, s_ribbon_vert), rf = SHADER(vk, s_ribbon_frag);
            PipeDesc pr = ps;
            pr.vs = rv, pr.fs = rf, pr.name = NULL;
            R.part_ribbon = pipeline(vk, &pr);
            vkDestroyShaderModule(vk->device, rv, NULL), vkDestroyShaderModule(vk->device, rf, NULL);
        }
        if (vk->large_points) { // the small ones: a point each (one vertex, not six)
            VkShaderModule qv = SHADER(vk, s_particle_point_vert), qf = SHADER(vk, s_particle_point_frag);
            PipeDesc pq = ps;
            pq.vs = qv, pq.fs = qf, pq.points = true, pq.name = NULL;
            R.part_point = pipeline(vk, &pq);
            vkDestroyShaderModule(vk->device, qv, NULL), vkDestroyShaderModule(vk->device, qf, NULL);
        }
        PipeDesc pm = { mv2, mf2, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, samples, VK_COMPARE_OP_GREATER_OR_EQUAL,
                        true, true, false, false, false, VK_CULL_MODE_BACK_BIT };
        pm.main = true;
        R.part_mesh = pipeline(vk, &pm);
        { // the water (water.c): its surfaces opaque with their depth (as the meshes), the rain blended (as the sprites)
            VkShaderModule wv = SHADER(vk, s_water_surface_vert), wf = vk->ray_query && !getenv("REALENGINE_NO_WATER_RT") ? SHADER(vk, s_water_surface_frag_rt) : SHADER(vk, s_water_surface_frag);
            VkShaderModule rv = SHADER(vk, s_rain_vert), rf = SHADER(vk, s_rain_frag);
            PipeDesc ws = pm;
            ws.vs = wv, ws.fs = wf, ws.cull = VK_CULL_MODE_NONE;
            R.water_surf = pipeline(vk, &ws);
            VkShaderModule cv = SHADER(vk, s_cloth_vert), cf = SHADER(vk, s_cloth_frag);
            PipeDesc cd = ws;
            cd.vs = cv, cd.fs = cf;
            R.cloth = pipeline(vk, &cd);
            vkDestroyShaderModule(vk->device, cv, NULL), vkDestroyShaderModule(vk->device, cf, NULL);
            { // (A9.3b) the hair: its fragment shader writes zeros to the split's and the denoiser's targets when the pass
              // has them (nothing of the mesh behind left to filter onto the hair)
                uint32_t nt = R.split_ok ? (R.den_ok ? 4u : 2u) : 1u;
                VkShaderModule hv = SHADER(vk, s_hair_vert);
                VkShaderModule hf = nt == 4u ? SHADER(vk, s_hair_frag_o4) : nt == 2u ? SHADER(vk, s_hair_frag_o2) : SHADER(vk, s_hair_frag);
                VkShaderModule hz = SHADER(vk, s_hair_depth_frag);
                PipeDesc hd = ws;
                hd.vs = hv, hd.fs = hz, hd.split = false, hd.mask_color = true, hd.name = "hair_depth";
                R.hair_depth = pipeline(vk, &hd);
                hd.fs = hf, hd.split = nt > 1u, hd.mask_color = false, hd.name = "hair";
                hd.depth_op = VK_COMPARE_OP_EQUAL, hd.depth_write = false;
                R.hair = pipeline(vk, &hd);
                vkDestroyShaderModule(vk->device, hv, NULL), vkDestroyShaderModule(vk->device, hf, NULL), vkDestroyShaderModule(vk->device, hz, NULL);
            }
            { // (A9.5d) the Gaussian splats: their own pass after the opaque scene (one sample: the color and the depth resolved)
                VkShaderModule sv = SHADER(vk, s_splat_vert), sf = SHADER(vk, s_splat_frag);
                PipeDesc sd = { sv, sf, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                false, true, true, false, false, VK_CULL_MODE_NONE };
                sd.name = "splat";
                R.splat_pipe = pipeline(vk, &sd);
                vkDestroyShaderModule(vk->device, sv, NULL), vkDestroyShaderModule(vk->device, sf, NULL);
            }
            PipeDesc rd = ps;
            rd.vs = rv, rd.fs = rf, rd.name = NULL;
            R.rain = pipeline(vk, &rd);
            VkShaderModule av = SHADER(vk, s_ar_plane_vert), af = SHADER(vk, s_ar_plane_frag);
            PipeDesc ad = rd;
            ad.vs = av, ad.fs = af;
            R.ar_plane = pipeline(vk, &ad);
            vkDestroyShaderModule(vk->device, av, NULL), vkDestroyShaderModule(vk->device, af, NULL);
            vkDestroyShaderModule(vk->device, wv, NULL), vkDestroyShaderModule(vk->device, wf, NULL);
            vkDestroyShaderModule(vk->device, rv, NULL), vkDestroyShaderModule(vk->device, rf, NULL);
        }
        vkDestroyShaderModule(vk->device, pv, NULL), vkDestroyShaderModule(vk->device, pf, NULL);
        vkDestroyShaderModule(vk->device, mv2, NULL), vkDestroyShaderModule(vk->device, mf2, NULL);
    }
    vkDestroyShaderModule(vk->device, mv, NULL);
    vkDestroyShaderModule(vk->device, mf, NULL);
    vkDestroyShaderModule(vk->device, fv, NULL);
    vkDestroyShaderModule(vk->device, sf, NULL);
    vkDestroyShaderModule(vk->device, gf, NULL);
    R.pipe_samples = samples;
}
static void build_msaa_pipelines(VkCtx* vk, VkSampleCountFlagBits samples) { PIPE_BATCH(vk, build_msaa_pipelines_now(vk, samples)); }

bool render_prewarm_progress(uint32_t* done, uint32_t* total) {
    int32_t d = re_atomic_load(&PW.done);
    *done = (uint32_t)(d < 0 ? 0 : d), *total = PW.n;
    return PW.n && *done < PW.n;
}
void render_prewarm(VkCtx* vk, int msaa) {
    if (PW.n || R.pipe_samples || getenv("REALENGINE_NO_PREWARM")) return;
    VkSampleCountFlagBits samples = (VkSampleCountFlagBits)(msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1);
    while (samples > R.max_samples) samples = (VkSampleCountFlagBits)(samples >> 1);
    // recorded as the first frame will (its state put back after: nothing made)
    bool vrs_built = R.vrs_built, rt_alpha = R.pipe_rt_alpha;
    pb_begin();
    build_msaa_pipelines_now(vk, samples);
    PB.mode = 0, PB.thread = 0;
    R.pipe_samples = 0, R.vrs_built = vrs_built, R.pipe_rt_alpha = rt_alpha;
    PW.it = PB.it, PW.n = PB.n;
    PB.it = NULL, PB.n = PB.cap = 0;
    if (!PW.n) return;
    PW.vk = vk, PW.next = PW.done = 0;
    int nt = re_cpu_count() - 2; // (the main thread and the intro's: room left)
    if (nt > (int)PW.n) nt = (int)PW.n;
    if (nt > 32) nt = 32;
    if (nt < 1) nt = 1;
    for (int i = 0; i < nt; i++)
        if ((PW.th[PW.nth] = re_thread_start(pw_worker, NULL)) != NULL) PW.nth++;
    if (!PW.nth) pw_worker(NULL);
}

// The composite's two pipelines for the swapchain's format (made again when it changes: HDR10 turned on, A9.4)
static void composite_pipelines(VkCtx* vk) {
    if (R.composite) vkDestroyPipeline(vk->device, R.composite, NULL), R.composite = VK_NULL_HANDLE;
    if (R.composite_post) vkDestroyPipeline(vk->device, R.composite_post, NULL), R.composite_post = VK_NULL_HANDLE;
    if (R.dd_pipe) vkDestroyPipeline(vk->device, R.dd_pipe, NULL), R.dd_pipe = VK_NULL_HANDLE;
    if (R.dd_lines) vkDestroyPipeline(vk->device, R.dd_lines, NULL), R.dd_lines = VK_NULL_HANDLE;
    { // (2026-11-19) the debug lines: blended (premultiplied) over the picture, no depth target (the scene's read)
        VkShaderModule dv = SHADER(vk, s_debug_line_vert), df = SHADER(vk, s_debug_line_frag);
        PipeDesc dd = { dv, df, vk->color_format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_ALWAYS, false,
                        false, true, false, false, VK_CULL_MODE_NONE };
        R.dd_pipe = pipeline(vk, &dd);
        dd.lines = true; // (the batches' trace records: 8 vertices each, a pixel wide, no edge to fade)
        R.dd_lines = pipeline(vk, &dd);
        vkDestroyShaderModule(vk->device, dv, NULL), vkDestroyShaderModule(vk->device, df, NULL);
    }
    VkShaderModule fv = SHADER(vk, s_fullscreen_vert), cf = SHADER(vk, s_composite_frag), cpf = SHADER(vk, s_composite_post_frag);
    PipeDesc cp = { fv, cf, vk->color_format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_ALWAYS, false,
                    false, false, false, false, VK_CULL_MODE_NONE };
    R.composite = pipeline(vk, &cp);
    cp.fs = cpf;
    R.composite_post = pipeline(vk, &cp);
    vkDestroyShaderModule(vk->device, cpf, NULL);
    vkDestroyShaderModule(vk->device, cf, NULL);
    vkDestroyShaderModule(vk->device, fv, NULL);
    R.swap_format = vk->color_format;
}

static void build_fixed_pipelines_now(VkCtx* vk) {
    VkShaderModule mv = SHADER(vk, s_mesh_vert), kf = SHADER(vk, s_mask_frag);
    VkShaderModule fv = SHADER(vk, s_fullscreen_vert), cf = SHADER(vk, s_composite_frag);
    PipeDesc sh = { mv, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                    VK_COMPARE_OP_LESS_OR_EQUAL, true, true, false, true, false, VK_CULL_MODE_NONE };
    R.shadow_pipe = pipeline(vk, &sh);
    VkShaderModule kmf = SHADER(vk, s_masked_frag);
    PipeDesc shm = sh;
    shm.fs = kmf;
    R.shadow_m = pipeline(vk, &shm);
    vkDestroyShaderModule(vk->device, kmf, NULL);
    { // the cloths' depth into the virtual shadow maps (cloth.c)
        VkShaderModule clv = SHADER(vk, s_cloth_vert);
        PipeDesc cs = sh;
        cs.vs = clv;
        R.cloth_shadow = pipeline(vk, &cs);
        vkDestroyShaderModule(vk->device, clv, NULL);
    }
    PipeDesc mk = { mv, kf, VK_FORMAT_R8_UNORM, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_GREATER_OR_EQUAL, true,
                    true, false, false, false, VK_CULL_MODE_NONE }; // reversed Z: the nearest selected object's id
    R.mask_pipe = pipeline(vk, &mk);
    { // (2026-11-25, item 17) the hair's ribbons into the mask (the same targets and test)
        VkShaderModule hv = SHADER(vk, s_hair_vert), hm = SHADER(vk, s_mask_hair_frag);
        PipeDesc mh = mk;
        mh.vs = hv, mh.fs = hm;
        R.mask_hair_pipe = pipeline(vk, &mh);
        vkDestroyShaderModule(vk->device, hv, NULL), vkDestroyShaderModule(vk->device, hm, NULL);
    }
    vkDestroyShaderModule(vk->device, cf, NULL), cf = VK_NULL_HANDLE;
    composite_pipelines(vk);
    // the OIT pass (Translucent materials): one sample, tested against the scene's depth, nothing written to it
    if (!R.mesh_tl && R.indep_blend) {
        VkShaderModule tf = SHADER(vk, s_mesh_frag_tl);
        PipeDesc td = { mv, tf, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_GREATER_OR_EQUAL,
                        false, true, false, false, false, VK_CULL_MODE_NONE };
        td.oit = true;
        R.mesh_tl = pipeline(vk, &td);
        vkDestroyShaderModule(vk->device, tf, NULL);
        if (gi_available()) {
            td.fs = SHADER(vk, s_mesh_frag_rt_tl);
            R.mesh_rt_tl = pipeline(vk, &td);
            vkDestroyShaderModule(vk->device, td.fs, NULL);
        }
    }
    // the velocity: the moved actors once more (one sample, tested against the scene's depth, nothing written to it)
    if (!R.velocity_pipe) {
        VkShaderModule vv = SHADER(vk, s_velocity_vert), vf = SHADER(vk, s_velocity_frag);
        PipeDesc vd = { vv, vf, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_COMPARE_OP_GREATER_OR_EQUAL,
                        false, true, false, false, false, VK_CULL_MODE_NONE };
        R.velocity_pipe = pipeline(vk, &vd);
        vkDestroyShaderModule(vk->device, vv, NULL);
        vd.vs = SHADER(vk, s_velocity_crowd_vert); // (the crowds' parts: the cull's lists once more)
        R.velocity_crowd_pipe = pipeline(vk, &vd);
        vkDestroyShaderModule(vk->device, vd.vs, NULL);
        vd.vs = SHADER(vk, s_hair_vel_vert); // (2026-11-15) the hair in strands (groom.c)
        R.velocity_hair_pipe = pipeline(vk, &vd);
        vkDestroyShaderModule(vk->device, vd.vs, NULL);
        vd.vs = SHADER(vk, s_cloth_vel_vert); // (2026-11-15) the cloths (cloth.c)
        R.velocity_cloth_pipe = pipeline(vk, &vd);
        vkDestroyShaderModule(vk->device, vd.vs, NULL);
        vkDestroyShaderModule(vk->device, vf, NULL);
    }
    vkDestroyShaderModule(vk->device, mv, NULL);
    vkDestroyShaderModule(vk->device, kf, NULL);
    vkDestroyShaderModule(vk->device, fv, NULL);
    vkDestroyShaderModule(vk->device, cf, NULL);
}
static void build_fixed_pipelines(VkCtx* vk) { PIPE_BATCH(vk, build_fixed_pipelines_now(vk)); }

// --- Targets (the viewport's size) ---

// The set's bindings (shaders): 0 shadow map, 1 HDR, 2 selection mask, 3-4 GI probes (sampled), 5-6 GI probes
// (storage), 7 the path tracer's accumulation, 8 HDR (storage: the path tracer), 9-10 the lightmaps, 11-12 the
// lightmaps (storage: the bake)
#define SET_BINDINGS 87
#define PUSH_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)
static const VkDescriptorType g_binding_types[SET_BINDINGS] = {
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 13-15: the atmosphere's LUTs sampled (transmittance, sky view, multiple scattering); 16-18: written
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 19: the aerial perspective written, 20: sampled
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 21: the prepass's depth (one sample) the Hi-Z starts from
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 22-23: the surface cache sampled (direct, indirect filtered); 24-26: written (gi_cache.c: direct, indirect, meta)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 27-35 the weather (weather.c): cloud panorama sampled / written, their shadow sampled / written, the fog sampled /
    // written, its froxel light (written), the noise sampled / written
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // (34-35: the clouds' shape and detail noise, 3D)
    // 36-37: the surface cache's walls (the distance to one around each texel, gi.glsl cache_read) sampled / written
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 38-39: the lightmaps' walls (gi_bake.c, gi.glsl lightmap_read) sampled / written
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 40: the velocity sampled; 41: the bloom sampled (every level), 42-47: its levels written; 48-49: the screen's clouds
    // written / their history sampled (weather.c)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 50: the surface cache's indirect filtered written (gi_cache.comp pass 3; 23 samples it)
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 51: the sun's virtual shadow maps' depths (vsm.glsl: the blocker search; 0 is the same array with the compare)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 52: the clouds' coverage map (weather.c, tiling)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 53: the shadow split resolved (sampled); 54: the filter's horizontal means (shadow_split.comp)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 55: the render (TSR's input); 56, 57: the histories sampled; 58, 59: written; 60: the output written (tsr.comp)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 61, 62: the many lights' visibility histories (shadow_split.comp)
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    // 63, 64: AR's camera picture, Y and UV (sky.frag); 65: the real depth (ar_occlude.frag)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    // 66: the local lights' shadow maps (lsm.c, lsm.glsl: compare)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, // 67: the VRS rate image (vrs.comp writes it; the main pass reads it as its attachment)
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, // 68: the vendors' upscalers' motion vectors (ups_mv.comp writes them)
    // 69-78: the unified denoiser (denoise.comp): the signals, the guide, last frame's, the 3 histories, the pre-blur's 2,
    // the keys, the stabilized output
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, // 79: the skin's scattering (sss.comp, A9.3a)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 80: the Color Grading LUT (composite.frag, A9.4: an N*N x N strip)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 81: HDR10's UI target (composite.frag lays it over the picture, A9.4)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 82-83: last frame's color and depth (SSR, A4c)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 84: (2026-11-16) the OIT pass's refraction sums (shadow_split.comp pass 3)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 85: (Q20) last frame's depth for the TSR's disocclusions (tsr.comp)
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // 86: (Q25b) mixed reality's real depth (mr.glsl: D16, a layer per eye)
};

// --- the Color Grading LUT (A9.4): an N*N x N RGBA8 strip (x: red + blue slice x N, y: green), sRGB encoded values ---
uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)
static bool lut_upload(VkCtx* vk, const uint8_t* rgba, uint32_t n) {
    GpuImage im = { 0 };
    if (!gpu_image_create(vk, &im, n * n, n, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    GpuBuf st = { 0 };
    if (!gpu_buf_create(vk, &st, (VkDeviceSize)n * n * n * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        gpu_image_destroy(vk, &im);
        return false;
    }
    memcpy(st.mapped, rgba, (size_t)n * n * n * 4);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, &im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy rg = { 0 };
    rg.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rg.imageExtent = (VkExtent3D){ n * n, n, 1 };
    vkCmdCopyBufferToImage(cmd, st.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    gpu_transition(cmd, &im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &st);
    if (R.lut.image) gpu_image_destroy(vk, &R.lut);
    R.lut = im, R.lut_n = n;
    return true;
}
static void lut_identity(VkCtx* vk) { // (no LUT chosen: never sampled, but bound)
    enum { N = 16 };
    static uint8_t px[N * N * N * 4];
    for (uint32_t g = 0; g < N; g++)
        for (uint32_t b = 0; b < N; b++)
            for (uint32_t r = 0; r < N; r++) {
                uint8_t* p = &px[((size_t)g * N * N + b * N + r) * 4];
                p[0] = (uint8_t)(r * 255 / (N - 1)), p[1] = (uint8_t)(g * 255 / (N - 1)), p[2] = (uint8_t)(b * 255 / (N - 1)), p[3] = 255;
            }
    lut_upload(vk, px, N);
}
// A .cube (Resolve / Adobe: LUT_3D_SIZE N, then N^3 lines "r g b", red fastest) or a PNG strip (N*N x N) into the strip's
// pixels (malloc'd); NULL: unreadable or not a LUT
uint8_t* render_lut_read(const char* disk, uint32_t* n_out) {
    size_t l = strlen(disk);
    if (l > 5 && !_stricmp(disk + l - 5, ".cube")) {
        FILE* f = re_fopen(disk, "rb");
        if (!f) return NULL;
        char line[256];
        uint32_t n = 0, k = 0;
        float mn[3] = { 0, 0, 0 }, mx[3] = { 1, 1, 1 };
        uint8_t* px = NULL;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "LUT_3D_SIZE", 11)) {
                n = (uint32_t)atoi(line + 11);
                if (n < 2 || n > 64) break;
                px = malloc((size_t)n * n * n * 4);
                if (!px) break;
                continue;
            }
            if (!strncmp(line, "DOMAIN_MIN", 10)) sscanf(line + 10, "%f %f %f", &mn[0], &mn[1], &mn[2]);
            if (!strncmp(line, "DOMAIN_MAX", 10)) sscanf(line + 10, "%f %f %f", &mx[0], &mx[1], &mx[2]);
            float c[3];
            if (!px || k >= n * n * n || sscanf(line, "%f %f %f", &c[0], &c[1], &c[2]) != 3) continue;
            uint32_t r = k % n, g = (k / n) % n, b = k / (n * n);
            uint8_t* p = &px[((size_t)g * n * n + b * n + r) * 4];
            for (int i = 0; i < 3; i++) {
                float v = (c[i] - mn[i]) / fmaxf(mx[i] - mn[i], 1e-6f);
                p[i] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
            }
            p[3] = 255;
            k++;
        }
        fclose(f);
        if (!px || k != n * n * n) return free(px), NULL;
        *n_out = n;
        return px;
    }
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(disk, &w, &h);
    if (!px || h < 2 || h > 64 || w != h * h) return free(px), NULL;
    *n_out = h;
    return px;
}
// The view's LUT loaded when it changes (rare: the GPU waits once, then the set points at the new strip)
static void lut_follow(VkCtx* vk, const char* vpath) {
    if (!vpath || !vpath[0] || !strcmp(vpath, R.lut_path)) return;
    snprintf(R.lut_path, sizeof R.lut_path, "%s", vpath);
    char disk[1100], root[620];
    if (g_mesh_root[0]) snprintf(root, sizeof root, "%s", g_mesh_root); // (the project's Content, else the executable's)
    else if (!re_exe_dir(disk, 512) || snprintf(root, sizeof root, "%s/Content", disk) <= 0) return;
    snprintf(disk, sizeof disk, "%s%s", root, strncmp(vpath, "/Content", 8) ? vpath : vpath + 8);
    uint32_t n = 0;
    uint8_t* px = render_lut_read(disk, &n);
    R.lut_ok = false;
    if (!px) {
        printf("[RealEngine] Color grading LUT %s: unreadable (a .cube, or a PNG strip N*N x N)\n", vpath);
        return;
    }
    vkDeviceWaitIdle(vk->device);
    R.lut_ok = lut_upload(vk, px, n);
    free(px);
    if (R.lut_ok) update_descriptors(vk), printf("[RealEngine] Color grading LUT %s: %u^3\n", vpath, n);
}

static void update_descriptors(VkCtx* vk) {
    if (!R.lut.image) lut_identity(vk); // (A9.4: bound from the start)
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GEN = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo ii[SET_BINDINGS] = {
        { R.shadow_sampler, vsm_view(), GEN }, // (the virtual shadow maps: vsm.c)
        { R.linear_sampler, R.hdr.view, RO },
        { R.linear_sampler, R.mask.view, RO },
        { R.linear_sampler, gi_probe_irradiance_view(), GEN },
        { R.linear_sampler, gi_probe_distance_view(), GEN },
        { VK_NULL_HANDLE, gi_probe_irradiance_view(), GEN },
        { VK_NULL_HANDLE, gi_probe_distance_view(), GEN },
        { VK_NULL_HANDLE, gi_accum_view(), GEN },
        { VK_NULL_HANDLE, R.tsr_targets ? R.tsr_in.view : R.hdr.view, GEN }, // (8: the render's color, its storage: the split, the OIT, the path tracer)
        { R.linear_sampler, gi_lightmap_view(0), GEN },
        { R.linear_sampler, gi_lightmap_view(1), GEN },
        { VK_NULL_HANDLE, gi_lightmap_view(0), GEN },
        { VK_NULL_HANDLE, gi_lightmap_view(1), GEN },
        { R.linear_sampler, atmosphere_view(0), GEN },
        { R.linear_sampler, atmosphere_view(1), GEN },
        { R.linear_sampler, atmosphere_view(2), GEN },
        { VK_NULL_HANDLE, atmosphere_view(0), GEN },
        { VK_NULL_HANDLE, atmosphere_view(2), GEN },
        { VK_NULL_HANDLE, atmosphere_view(1), GEN },
        { VK_NULL_HANDLE, atmosphere_view(3), GEN },
        { R.linear_sampler, atmosphere_view(3), GEN },
        { R.linear_sampler, R.depth_resolve.view ? R.depth_resolve.view : R.depth_ms.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL },
        { R.linear_sampler, gi_cache_view(0), GEN },
        { R.linear_sampler, gi_cache_view(4), GEN },
        { VK_NULL_HANDLE, gi_cache_view(0), GEN },
        { VK_NULL_HANDLE, gi_cache_view(1), GEN },
        { VK_NULL_HANDLE, gi_cache_view(2), GEN },
        { R.linear_sampler, weather_view(0), GEN },
        { VK_NULL_HANDLE, weather_view(0), GEN },
        { R.linear_sampler, weather_view(1), GEN },
        { VK_NULL_HANDLE, weather_view(1), GEN },
        { R.linear_sampler, weather_view(2), GEN },
        { VK_NULL_HANDLE, weather_view(2), GEN },
        { VK_NULL_HANDLE, weather_view(3), GEN },
        { R.repeat_sampler, weather_view(4), GEN }, // (the clouds' shape noise, 3D, tiling)
        { R.repeat_sampler, weather_view(7), GEN }, // (their detail noise)
        { R.linear_sampler, gi_cache_view(3), GEN },
        { VK_NULL_HANDLE, gi_cache_view(3), GEN },
        { R.linear_sampler, gi_lightmap_view(2), GEN },
        { VK_NULL_HANDLE, gi_lightmap_view(2), GEN },
        { R.linear_sampler, R.velocity.view, RO },
        { R.mip_sampler, R.bloom_view, GEN },
        { VK_NULL_HANDLE, R.bloom_mip[0], GEN },
        { VK_NULL_HANDLE, R.bloom_mip[1], GEN },
        { VK_NULL_HANDLE, R.bloom_mip[2], GEN },
        { VK_NULL_HANDLE, R.bloom_mip[3], GEN },
        { VK_NULL_HANDLE, R.bloom_mip[4], GEN },
        { VK_NULL_HANDLE, R.bloom_mip[5], GEN },
        { VK_NULL_HANDLE, weather_view(5), GEN },
        { R.linear_sampler, weather_view(6), GEN },
        { VK_NULL_HANDLE, gi_cache_view(4), GEN },
        { R.linear_sampler, vsm_view(), GEN },
        { R.repeat_sampler, weather_view(8), GEN },
        { R.linear_sampler, R.split.view, RO },
        { VK_NULL_HANDLE, R.split_sum.view, GEN },
        // (without TSR: the HDR target in their place, never read)
        { R.linear_sampler, R.tsr_targets ? R.tsr_in.view : R.hdr.view, RO },
        { R.linear_sampler, R.tsr_targets ? R.hist[0].view : R.hdr.view, GEN },
        { R.linear_sampler, R.tsr_targets ? R.hist[1].view : R.hdr.view, GEN },
        { VK_NULL_HANDLE, R.tsr_targets ? R.hist[0].view : R.hdr.view, GEN },
        { VK_NULL_HANDLE, R.tsr_targets ? R.hist[1].view : R.hdr.view, GEN },
        { VK_NULL_HANDLE, R.hdr.view, GEN },
        { VK_NULL_HANDLE, R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.ml_hist[1].view, GEN },
        { R.linear_sampler, R.ar_img[0].view, RO },
        { R.linear_sampler, R.ar_img[1].view, RO },
        { R.linear_sampler, R.ar_depth.view, RO },
        { R.shadow_sampler, lsm_view(), GEN },
        { VK_NULL_HANDLE, R.vrs_img.view ? R.vrs_img.view : R.ml_hist[0].view, GEN }, // (67: the VRS rate image; no VRS: a stand-in never read)
        { VK_NULL_HANDLE, R.ups_mv.view ? R.ups_mv.view : R.ml_hist[0].view, GEN }, // (68: the upscalers' motion; without TSR: a stand-in never written)
        // (69-76: before the denoiser first runs, stand-ins never read)
        { VK_NULL_HANDLE, R.den_alloc ? R.den.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_guide.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_guide_prev.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_hd.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_hs.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_hv.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_ta.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_tb.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_key.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.den_alloc ? R.den_hst.view : R.ml_hist[0].view, GEN },
        { VK_NULL_HANDLE, R.sss_alloc ? R.sss.view : R.ml_hist[0].view, GEN }, // (79: before the skin's pass first runs, a stand-in never read)
        { R.linear_sampler, R.lut.view, RO }, // (80: the Color Grading LUT, A9.4)
        { R.linear_sampler, R.ui.view ? R.ui.view : R.lut.view, RO }, // (81: HDR10's UI target; else a stand-in never read)
        { R.linear_sampler, R.ssr_col.view ? R.ssr_col.view : R.lut.view, RO }, // (82-83: SSR's copies; else stand-ins never read)
        { R.linear_sampler, R.ssr_dep.view ? R.ssr_dep.view : R.lut.view, RO },
        { R.linear_sampler, R.oit_refr.view ? R.oit_refr.view : R.lut.view, RO }, // (84: the refraction's sums; else a stand-in never read)
        { R.linear_sampler, R.tsr_dep.view ? R.tsr_dep.view : R.lut.view, RO }, // (85, Q20: last frame's depth for the TSR; else a stand-in never read)
        { R.linear_sampler, R.mr_depth.view, RO }, // (86, Q25b: mixed reality's real depth; a 2 x 2 stand-in until one comes)
    };
    VkWriteDescriptorSet w[SET_BINDINGS];
    for (int i = 0; i < SET_BINDINGS; i++) {
        w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = R.set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = g_binding_types[i];
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(vk->device, SET_BINDINGS, w, 0, NULL);
}

static void bloom_destroy(VkCtx* vk) {
    for (int i = 0; i < BLOOM_LEVELS; i++)
        if (R.bloom_mip[i]) vkDestroyImageView(vk->device, R.bloom_mip[i], NULL);
    if (R.bloom_view) vkDestroyImageView(vk->device, R.bloom_view, NULL);
    if (R.bloom_img) vkDestroyImage(vk->device, R.bloom_img, NULL);
    if (R.bloom_mem) vkFreeMemory(vk->device, R.bloom_mem, NULL);
    memset(R.bloom_mip, 0, sizeof R.bloom_mip);
    R.bloom_view = VK_NULL_HANDLE;
    R.bloom_img = VK_NULL_HANDLE;
    R.bloom_mem = VK_NULL_HANDLE;
    R.bloom_general = false;
}

// The bloom's chain: a quarter of the view's size (32 at least: six levels always fit), each level half the one before
// (a wide blur: the composite's bilinear read of a quarter size doesn't show, and it costs a quarter of half size)
static bool bloom_create(VkCtx* vk, uint32_t w, uint32_t h) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (bw < 32) bw = 32;
    if (bh < 32) bh = 32;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = (VkExtent3D){ bw, bh, 1 };
    ii.mipLevels = BLOOM_LEVELS;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &R.bloom_img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, R.bloom_img, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &R.bloom_mem) != VK_SUCCESS) return false;
    vk_check(vkBindImageMemory(vk->device, R.bloom_img, R.bloom_mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = R.bloom_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, BLOOM_LEVELS, 0, 1 };
    vk_check(vkCreateImageView(vk->device, &vi, NULL, &R.bloom_view), "vkCreateImageView");
    for (uint32_t l = 0; l < BLOOM_LEVELS; l++) {
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1 };
        vk_check(vkCreateImageView(vk->device, &vi, NULL, &R.bloom_mip[l]), "vkCreateImageView");
        R.bloom_w[l] = bw >> l ? bw >> l : 1;
        R.bloom_h[l] = bh >> l ? bh >> l : 1;
    }
    R.bloom_general = false;
    return true;
}

// w, h: the render's size; ow, oh: the output's (the HDR target, the mask, the bloom: what the post reads); tsr: the
// render's own color target and the histories (temporal super resolution)
// A 4x4 matrix's inverse (column-major; the cofactors): TSR's depth back to the world
static void mat4_inverse(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    float id = fabsf(det) > 1e-30f ? 1.0f / det : 0.0f;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * id;
}

static void build_rt_pipelines(VkCtx* vk, VkSampleCountFlagBits samples, bool a, bool den, VkPipeline out[RT_PIPES]);
static void den_rt_destroy(VkCtx* vk) {
    for (int i = 0; i < RT_PIPES; i++)
        if (R.den_rt[i]) vkDestroyPipeline(vk->device, R.den_rt[i], NULL), R.den_rt[i] = VK_NULL_HANDLE;
    R.den_rt_key = 0;
}
// (the key: the samples, the alpha test, the extra lobes' pipelines present; 0: none made)
static uint32_t den_rt_want(void) { return (uint32_t)R.pipe_samples | (R.pipe_rt_alpha ? 256u : 0u) | (R.mesh_rt_x ? 512u : 0u) | 1024u | (R.mesh_rt_m_x ? 2048u : 0u) | (R.toon_spv ? 4096u : 0u) | (R.tess_twins ? 8192u : 0u); }
static bool den_rt_job_running(void);
// (made on a thread: with a cold pipeline cache the driver compiled the 8 for 31 s on a 3060; until they are in, the
// frame is drawn as before, with the separate filters)
static struct {
    ReThread* thread;
    VkCtx vk; // (a copy: its counters the thread's own)
    VkSampleCountFlagBits samples;
    bool alpha;
    uint32_t key;
    VkPipeline p[RT_PIPES];
    volatile int32_t done;
    double t0;
} DJ;
static int den_rt_job(void* arg) {
    (void)arg;
    build_rt_pipelines(&DJ.vk, DJ.samples, DJ.alpha, true, DJ.p);
    re_atomic_store(&DJ.done, 1);
    return 0;
}
static void den_rt_job_wait(VkCtx* vk, bool install) {
    if (!DJ.thread) return;
    while (!re_atomic_load(&DJ.done)) crash_heartbeat(), re_sleep_seconds(0.01); // (a wait, not a hang: the watchdog told)
    re_thread_join(DJ.thread);
    DJ.thread = NULL;
    if (install && DJ.key == den_rt_want()) {
        vkDeviceWaitIdle(vk->device); // (the old ones may be in a frame in flight)
        den_rt_destroy(vk);
        memcpy(R.den_rt, DJ.p, sizeof R.den_rt);
        R.den_rt_key = DJ.key;
        printf("[RealEngine] Denoiser unificado: pipelines do passe principal feitas em segundo plano em %.0f ms\n", (re_time_seconds() - DJ.t0) * 1000.0);
    } else
        for (int i = 0; i < RT_PIPES; i++)
            if (DJ.p[i]) vkDestroyPipeline(vk->device, DJ.p[i], NULL);
    memset(DJ.p, 0, sizeof DJ.p);
}
static bool den_rt_job_running(void) { return DJ.thread != NULL; }
// (Q26) the pipelines still being made on their threads (the status bar's "Shader Compile: N")
int render_pipelines_pending_count(void) { // (and the prewarm's still compiling: the cold start's)
    uint32_t d = 0, t = 0;
    int pw = render_prewarm_progress(&d, &t) ? (int)(t - d) : 0;
    return (RJ.thread ? RT_PIPES : 0) + (DJ.thread ? RT_PIPES : 0) + pw;
}
// Each frame it is wanted: true once its pipelines are in for the set drawn now
static bool den_rt_ensure(VkCtx* vk) {
    if (DJ.thread && re_atomic_load(&DJ.done)) den_rt_job_wait(vk, true);
    if (R.den_rt_key == den_rt_want()) return true;
    if (!DJ.thread) {
        DJ.vk = *vk, DJ.samples = R.pipe_samples, DJ.alpha = R.pipe_rt_alpha, DJ.key = den_rt_want();
        DJ.done = 0, DJ.t0 = re_time_seconds();
        DJ.thread = re_thread_start(den_rt_job, NULL);
        if (!DJ.thread) { // (no thread: made here)
            build_rt_pipelines(vk, DJ.samples, DJ.alpha, true, DJ.p);
            DJ.done = 1;
            DJ.thread = NULL;
            vkDeviceWaitIdle(vk->device);
            den_rt_destroy(vk);
            memcpy(R.den_rt, DJ.p, sizeof R.den_rt);
            memset(DJ.p, 0, sizeof DJ.p);
            R.den_rt_key = DJ.key;
            return true;
        }
        printf("[RealEngine] Denoiser unificado: pipelines do passe principal sendo feitas em segundo plano\n");
    }
    return false;
}

// The unified denoiser's images (A9.1d): made when it first runs at this size, dropped with the other targets
static void den_destroy(VkCtx* vk) {
    GpuImage* ims[] = { &R.den, &R.den_guide, &R.den_guide_prev, &R.den_hd, &R.den_hs, &R.den_hv, &R.den_ta, &R.den_tb, &R.den_key, &R.den_hst };
    for (size_t i = 0; i < sizeof ims / sizeof ims[0]; i++) gpu_image_destroy(vk, ims[i]);
    for (int k = 0; k < 2; k++)
        if (R.rgi[k].buffer) gpu_buf_destroy(vk, &R.rgi[k]);
    memset(R.rgi, 0, sizeof R.rgi);
    R.den_alloc = false;
}
static bool den_layers(VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h) { // (rgba16f, 2 layers: a history's ping-pong)
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &im->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im->image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &im->memory) != VK_SUCCESS) return false;
    vk_check(vkBindImageMemory(vk->device, im->image, im->memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = ii.format;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 };
    vk_check(vkCreateImageView(vk->device, &vi, NULL, &im->view), "vkCreateImageView");
    im->format = ii.format;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->layers = 2;
    return true;
}
static bool den_create(VkCtx* vk) {
    uint32_t w = R.width, h = R.height;
    VkImageUsageFlags att = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    vkDeviceWaitIdle(vk->device); // (the set's views change: the frames in flight still read it)
    bool ok = gpu_image_create(vk, &R.den, w, h, VK_FORMAT_R32G32B32A32_UINT, att, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.den_guide, w, h, VK_FORMAT_R32G32B32A32_UINT, att, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.den_guide_prev, w, h, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT) &&
              den_layers(vk, &R.den_hd, w, h) && den_layers(vk, &R.den_hs, w, h) && den_layers(vk, &R.den_hv, w, h) &&
              den_layers(vk, &R.den_hst, w, h) &&
              gpu_image_create(vk, &R.den_ta, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.den_tb, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.den_key, w, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    if (ok) ok = gpu_buf_create(vk, &R.rgi[0], 32u * (VkDeviceSize)w * h, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false); // (ReSTIR GI: pass 0 writes every pixel)
    if (!ok) {
        den_destroy(vk);
        printf("[RealEngine] aviso: o denoiser unificado nao conseguiu suas imagens (%ux%u)\n", w, h);
        return false;
    }
    R.den_alloc = true, R.den_reset = true;
    R.den_w = w, R.den_h = h;
    update_descriptors(vk);
    printf("[RealEngine] Denoiser unificado: imagens %ux%u (%.1f MB)\n", w, h, (double)w * h * 132.0 / 1048576.0);
    return true;
}

// The skin's scattering's image (A9.3a, sss.comp): made when a subsurface material is first drawn at this size; zeroed
// once (sss_clear: its pass leaves it zeroed behind it)
static void sss_destroy(VkCtx* vk) {
    gpu_image_destroy(vk, &R.sss);
    R.sss_alloc = false;
}
static bool sss_create(VkCtx* vk) {
    vkDeviceWaitIdle(vk->device); // (the set's view changes: the frames in flight still read it)
    if (!gpu_image_create(vk, &R.sss, R.width, R.height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
        sss_destroy(vk);
        printf("[RealEngine] aviso: a subsuperficie da pele nao conseguiu sua imagem (%ux%u)\n", R.width, R.height);
        return false;
    }
    R.sss_alloc = R.sss_clear = true;
    R.sss_w = R.width, R.sss_h = R.height;
    update_descriptors(vk);
    printf("[RealEngine] Subsuperficie em tela (pele, A9.3a): imagem %ux%u (%.1f MB)\n", R.width, R.height, (double)R.width * R.height * 8.0 / 1048576.0);
    return true;
}

static bool ensure_targets(VkCtx* vk, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, bool tsr, VkSampleCountFlagBits samples) {
    if (w == R.width && h == R.height && ow == R.out_w && oh == R.out_h && tsr == R.tsr_targets && samples == R.samples && R.hdr.image) {
        bool vrs_want = vk->vrs && vrs_wanted(); // (the VRS setting changed: the pipelines again)
        if (R.vrs_built != vrs_want) vkDeviceWaitIdle(vk->device), build_msaa_pipelines(vk, samples), update_descriptors(vk);
        return true;
    }
    vkDeviceWaitIdle(vk->device); // a resize: the frames in flight still read the old images
    // (2026-11-14, "Dynamic Resolution cria bugs na tela") only the render's share changed (the dynamic resolution, the
    // upscaling mode): what is at the output's size stays (the image, the outline's mask, the TSR's history, the bloom),
    // the TSR keeps its frames, the denoiser and the skin's scattering keep their images while the render fits in them.
    // Before, every step of the dynamic resolution made everything again and started every history over: a frame
    // aliased and noisy, the GI's noise back, the clouds blurred, a pause (the denoiser's 200 MB again)
    bool rescale = ow == R.out_w && oh == R.out_h && tsr == R.tsr_targets && samples == R.samples && R.hdr.image && tsr;
    if (!rescale || w > R.den_w || h > R.den_h) den_destroy(vk);
    if (!rescale || w > R.sss_w || h > R.sss_h) sss_destroy(vk);
    gpu_image_destroy(vk, &R.tsr_in);
    gpu_image_destroy(vk, &R.ups_mv);
    if (!rescale) gpu_image_destroy(vk, &R.hist[0]), gpu_image_destroy(vk, &R.hist[1]);
    gpu_image_destroy(vk, &R.color_ms);
    gpu_image_destroy(vk, &R.depth_ms);
    gpu_image_destroy(vk, &R.depth_resolve);
    if (!rescale) gpu_image_destroy(vk, &R.hdr);
    gpu_image_destroy(vk, &R.split_ms);
    gpu_image_destroy(vk, &R.split);
    gpu_image_destroy(vk, &R.split_sum), gpu_image_destroy(vk, &R.oit_refr);
    gpu_image_destroy(vk, &R.ml_hist[0]), gpu_image_destroy(vk, &R.ml_hist[1]);
    gpu_image_destroy(vk, &R.vrs_img);
    if (!rescale) gpu_image_destroy(vk, &R.mask_depth), gpu_image_destroy(vk, &R.mask);
    gpu_image_destroy(vk, &R.velocity);
    gpu_image_destroy(vk, &R.split_ms);
    gpu_image_destroy(vk, &R.split);
    gpu_image_destroy(vk, &R.split_sum), gpu_image_destroy(vk, &R.oit_refr);
    gpu_image_destroy(vk, &R.ml_hist[0]), gpu_image_destroy(vk, &R.ml_hist[1]);
    gpu_image_destroy(vk, &R.vrs_img);
    if (rescale) {
        static uint32_t said;
        if (said++ < 8) printf("[RealEngine] Resolucao dinamica: render %ux%u -> %ux%u (saida %ux%u, historicos mantidos)\n", R.width, R.height, w, h, ow, oh);
    }
    if (!rescale) { // (a rescale: the denoiser's history reprojects with last frame's size, den_prev_w: denoise.comp)
        bloom_destroy(vk);
        R.have_prev_vp = false;
        R.tsr_reset = true;
    }
    R.mask_idle = false; // (2026-11-25: the new mask cleared and its layout set once before its pass is skipped)
    bool ok = (rescale || gpu_image_create(vk, &R.hdr, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, // (transfer dst: DLSS clears its output on a reset)
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // (src: SSR's copy, A4c)
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.mask, ow, oh, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // (2026-11-25: render_mask_read)
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.mask_depth, ow, oh, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) &&
              gpu_image_create(vk, &R.depth_ms, w, h, VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, samples, VK_IMAGE_ASPECT_DEPTH_BIT) &&
              gpu_image_create(vk, &R.velocity, w, h, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              (rescale || bloom_create(vk, ow, oh)) &&
              gpu_image_create(vk, &R.split, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.split_sum, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, // (Q4: rgba, the OIT colored transmittance in gba)
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) && // (also the OIT pass's targets: accum, revealage)
              gpu_image_create(vk, &R.oit_refr, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) && // (2026-11-16: the OIT's refraction sums)
              gpu_image_create(vk, &R.ml_hist[0], w, h, VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT) &&
              gpu_image_create(vk, &R.ml_hist[1], w, h, VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT) &&
              (!vk->vrs || gpu_image_create(vk, &R.vrs_img, (w + vk->vrs_texel - 1) / vk->vrs_texel, (h + vk->vrs_texel - 1) / vk->vrs_texel,
                                           VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT));
    R.ml_reset = true;
    for (int k = 0; ok && tsr && k < (rescale ? 1 : 3); k++) // the render's color (the main pass resolves into it), the histories
        ok = gpu_image_create(vk, k ? &R.hist[k - 1] : &R.tsr_in, k ? ow : w, k ? oh : h, k ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    if (ok && tsr) // (the vendors' upscalers' motion vectors: 4 bytes a render pixel)
        ok = gpu_image_create(vk, &R.ups_mv, w, h, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (ok && samples != VK_SAMPLE_COUNT_1_BIT && gi_available()) // (the split's MSAA target: only where rays are traced)
        ok = gpu_image_create(vk, &R.split_ms, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples, VK_IMAGE_ASPECT_COLOR_BIT);
    if (ok && samples != VK_SAMPLE_COUNT_1_BIT)
        ok = gpu_image_create(vk, &R.color_ms, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples,
                          VK_IMAGE_ASPECT_COLOR_BIT) &&
             // the prepass's depth resolved to one sample (MIN: the farthest of each pixel's) for the Hi-Z
             gpu_image_create(vk, &R.depth_resolve, w, h, VK_FORMAT_D32_SFLOAT, // (TRANSFER_SRC: Q20's copy for the TSR's disocclusions)
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
    // the Hi-Z: every level) of the pyramid (level 0 at full size) in one buffer
    uint64_t hz = 0;
    R.hiz_levels = 0;
    for (uint32_t lw = (w + 1) / 2, lh = (h + 1) / 2;; lw = (lw + 1) / 2, lh = (lh + 1) / 2) { // level 0: half size
        hz += (uint64_t)lw * lh;
        R.hiz_levels++;
        if (lw == 1 && lh == 1) break;
    }
    gpu_buf_destroy(vk, &R.hiz);
    R.hiz_have = false; // (the DAGs' first phase reads last frame's: none now)
    if (ok) ok = gpu_buf_create(vk, &R.hiz, hz * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    if (ok && (w != R.width || h != R.height)) {
        ok = gi_resize(vk, w, h); // the path tracer's accumulation
        R.pt_samples = 0;
    }
    if (!ok) return false;
    R.width = w;
    R.height = h;
    R.out_w = ow, R.out_h = oh, R.tsr_targets = tsr;
    R.samples = samples;
    bool vrs_want = vk->vrs && vrs_wanted(); // (Project Settings changed: made again)
    if (R.pipe_samples != samples || R.vrs_built != vrs_want) build_msaa_pipelines(vk, samples);
    update_descriptors(vk);
    return true;
}

// --- Skins (RMESH_SKINNED; notas-dev/animacao.md "Skinning suave"): Unreal's GPU skin cache. A Skeletal Mesh's skin
// is an instance in the dynamic set whose vertices are its own: two regions in R.vertices after the meshes' (this
// frame's, last frame's for the velocity pass), bent from the mesh's bind by skin.comp whenever its palette (anim.c)
// changed, and its own BLAS refit after (the rays: shadows, GI, the hair's alpha). The instance points at them
// (INST_SKINNED, ids[3]); mesh.vert / velocity.vert / rt.glsl read them through it. Nothing runs for a still pose.
typedef struct SkinSlot {
    uint32_t slot;           // the scene slot (its entity's index)
    Entity e;
    uint32_t mesh, nv, src;  // the mesh's slot, its vertices, LOD 0's first (the bind)
    uint32_t base;           // its regions: base (this frame) and base + nv (last frame), vertex indices
    uint32_t pal_version, seen, dead_at;
    bool done;               // bent at least once
    uint32_t rt_lod;         // the LOD its BLAS holds (skin_rt_lod)
    GiDynBlas blas;
    // its morph targets (R.morph_count[mesh]): each one's index in the Skeletal Mesh's list (by name; 0xFF none), the
    // Skeletal Mesh it was matched to and the last weights' hash (a change bends it again)
    uint32_t morph_sk, morph_hash;
    uint8_t morph_map[RMESH_MAX_MORPHS];
} SkinSlot;
// (2026-11-24) A skin drawn at its bind (the mesh's own vertices, no room taken): no room for it (SKIN_RAW_ROOM, until
// skin_gen moves), or its pose is the bind's (SKIN_RAW_BIND, while its palette's version stays ver)
enum { SKIN_RAW_ROOM = 1, SKIN_RAW_BIND = 2 };
typedef struct SkinRaw {
    Entity e;
    uint32_t mesh, gen, ver, why;
} SkinRaw;
// (skin.comp's SkinJob, scalar: 64 bytes) morph: its targets' count (0: no weight set, the loop skipped), the CSR and the
// deltas (R.morphs) and the weights (after the palette)
typedef struct SkinJobGpu {
    uint32_t src, dst, prev, nv;
    VkDeviceAddress pal;
    uint32_t first, nmorph;
    VkDeviceAddress mfirst, mdelta, mweight;
    uint32_t pad[2];
} SkinJobGpu;

void render_skin_stats(uint32_t* skins, uint32_t* bent, uint32_t* verts, double* cpu_ms) {
    *skins = R.nskins, *bent = R.nskin_jobs, *verts = R.skin_verts, *cpu_ms = R.skin_ms;
}
void render_skin_bind_stats(uint32_t* at_bind, uint32_t* no_room) { *at_bind = R.skin_raw_n, *no_room = R.skin_noroom_n; }
uint32_t render_skin_rigid_count(void) { return R.skin_rigid_n; } // (2026-11-24)
int render_mesh_collision(uint32_t slot) { return slot < RE_MAX_MESHES && slot < R.nmeshes ? R.mesh_collision[slot] : 0; } // (2026-11-24)
bool render_last_camera(float pos[3]) { // (2026-11-24)
    memcpy(pos, R.last_cam, 12);
    return R.have_cam;
}
void render_dormant_stats(uint32_t* now, uint32_t* slept, uint32_t* woke) { *now = R.dorm_n, *slept = R.dorm_slept, *woke = R.dorm_woke; }
void render_morph_off(bool off) { R.morph_off = off; }
void render_morph_stats(uint32_t* meshes, uint32_t* jobs, uint32_t* verts) {
    *meshes = R.morph_meshes, *jobs = R.morph_jobs, *verts = R.morph_deltas;
}

uint32_t render_skin_readback(VkCtx* vk, World* w, Entity owner, float (*pos)[3], uint32_t cap) {
    const SkinSlot* k = NULL;
    for (uint32_t i = 0; i < R.nskins && !k; i++) {
        const CAttach* at = R.skins[i].done && !R.skins[i].dead_at ? ecs_get(w, R.skins[i].e, COMP_ATTACH) : NULL;
        if (at && at->owner == owner) k = &R.skins[i];
    }
    if (!k || !cap) return 0;
    uint32_t n = k->nv < cap ? k->nv : cap;
    GpuBuf hb = { 0 };
    if (!gpu_buf_create(vk, &hb, sizeof(GpuVertex) * (VkDeviceSize)n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return 0;
    vkDeviceWaitIdle(vk->device);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy r = { sizeof(GpuVertex) * (VkDeviceSize)k->base, 0, sizeof(GpuVertex) * (VkDeviceSize)n };
    vkCmdCopyBuffer(cmd, R.vertices.buffer, hb.buffer, 1, &r);
    gpu_end_once(vk, cmd);
    for (uint32_t i = 0; i < n; i++) memcpy(pos[i], ((const GpuVertex*)hb.mapped)[i].pos, 12);
    gpu_buf_destroy(vk, &hb);
    return n;
}

// A skin's weights now (its CSkeletalMesh's, by the Skeletal Mesh's names; the slot's map made once per Skeletal Mesh):
// into w (the skin's target order), their hash; any: one isn't zero
static uint32_t skin_morph_weights(World* w, SkinSlot* k, Entity owner, float* wt, bool* any) {
    uint32_t n = R.morph_count[k->mesh];
    *any = false;
    const CSkeletalMesh* c = owner ? ecs_get(w, owner, COMP_SKELETAL_MESH) : NULL;
    const SkelMesh* sk = c ? anim_mesh(c->mesh) : NULL;
    uint32_t skh = sk ? sk->hash : 0u;
    if (k->morph_sk != skh) { // the names matched once (a Skeletal Mesh changed: again)
        k->morph_sk = skh;
        for (uint32_t t = 0; t < n; t++) {
            int i = sk ? anim_morph_index(sk, R.morph_names[R.morph_name0[k->mesh] + t]) : -1;
            k->morph_map[t] = (uint8_t)(i >= 0 && i < 255 ? i : 0xFF);
        }
    }
    uint32_t h = 2166136261u ^ skh;
    for (uint32_t t = 0; t < n; t++) {
        float v = k->morph_map[t] != 0xFF ? anim_morph_weight(sk, c, k->morph_map[t]) : 0.0f;
        if (!(v == v)) v = 0.0f; // (NaN: none)
        wt[t] = v, *any |= v != 0.0f;
        uint32_t b;
        memcpy(&b, &v, 4);
        h = (h ^ b) * 16777619u;
    }
    return h;
}

static void skin_reset(VkCtx* vk) { // (the GPU idle: a reload)
    for (uint32_t i = 0; i < R.nskins; i++) gi_dyn_blas_free(vk, &R.skins[i].blas);
    R.nskins = 0;
    if (R.skin_map) memset(R.skin_map, 0xFF, sizeof(uint32_t) * R.skin_map_cap);
    R.skin_gen++; // (the room is new: the skins without room try again)
}

// (2026-11-24) The slot -> skin map and the skins at their bind, as long as the scene's slots; false: no memory
static bool skin_map_room(uint32_t s) {
    if (s < R.skin_map_cap) return true;
    uint32_t c = R.skin_map_cap ? R.skin_map_cap : 1024u;
    while (c <= s || c < R.scene_cap) c *= 2u;
    uint32_t* m = realloc(R.skin_map, sizeof(uint32_t) * c);
    if (!m) return false;
    R.skin_map = m;
    SkinRaw* r = realloc(R.skin_raw, sizeof(SkinRaw) * c);
    if (!r) return false;
    R.skin_raw = r;
    memset(m + R.skin_map_cap, 0xFF, sizeof(uint32_t) * (c - R.skin_map_cap));
    memset(r + R.skin_map_cap, 0, sizeof(SkinRaw) * (c - R.skin_map_cap)); // (gen 0: none, skin_gen starts at 1)
    R.skin_map_cap = c;
    if (!R.skin_gen) R.skin_gen = 1u;
    return true;
}
// The map from place `from` on (the list moved: a skin made or freed before them)
static void skin_map_fix(uint32_t from) {
    for (uint32_t i = from; i < R.nskins; i++)
        if (!R.skins[i].dead_at && R.skins[i].slot < R.skin_map_cap) R.skin_map[R.skins[i].slot] = i;
}
// Slot s's live skin of entity e on mesh (NULL: none): the map; a slot whose mesh changed has two for a few frames,
// the other one then searched
static SkinSlot* skin_find(uint32_t s, Entity e, uint32_t mesh) {
    uint32_t i = s < R.skin_map_cap ? R.skin_map[s] : UINT32_MAX;
    if (i >= R.nskins) return NULL;
    SkinSlot* k = &R.skins[i];
    if (k->slot == s && k->e == e && k->mesh == mesh && !k->dead_at) return k;
    for (uint32_t j = 0; j < R.nskins; j++)
        if (R.skins[j].slot == s && R.skins[j].e == e && R.skins[j].mesh == mesh && !R.skins[j].dead_at) return &R.skins[j];
    return NULL;
}
// A skin's palette now (its CSkeletalMesh's; NULL: none yet, its bind) and its version (0 without one: anim.c's start
// at 1, so the first palette made is a change)
static const float (*skin_palette(World* w, Entity e, uint32_t* nb, uint32_t* ver))[3][4] {
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    *nb = 0, *ver = 0;
    const float(*pal)[3][4] = at ? anim_skin_palette(at->owner, nb, ver) : NULL;
    if (!pal) *nb = 1, *ver = 0;
    return pal;
}
// (2026-11-24) Its pose is its bind's: every bone's palette the identity (a Skeletal Mesh standing still with no
// animation; none yet) and no morph targets. Bent, its vertices would be the mesh's own: drawn from them, no room taken
// (the Unamed scene's 2080 capsules needed 41 M vertices of a room of 6 M, 1764 of them out of room every frame)
static bool skin_pose_is_bind(uint32_t mesh, const float (*pal)[3][4], uint32_t nb) {
    if (mesh < RE_MAX_MESHES && R.morph_count[mesh] && R.morphs.buffer) return false; // (its weights may bend it)
    if (!pal) return true;
    for (uint32_t b = 0; b < nb; b++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) { // (the rotation and scale within 1e-4, the move within 0.01 cm: a float's error far out)
                float d = pal[b][r][c] - (c == r ? 1.0f : 0.0f);
                if (fabsf(d) > (c == 3 ? 0.01f : 1e-4f)) return false;
            }
    return true;
}
// (2026-11-24, item 4) A rigid skin (R.mesh_rigid_bone: every vertex on one bone): the instance's rows times that bone's
// palette row (the bone's pose times its bind's inverse, in the mesh's space), the mesh's own vertices drawn: no room, no
// BLAS of its own, its LODs, its shadow and its rays as any mesh's, a moving one's through the dynamic set's compare. Any
// thread (dyn_one's jobs, skin_bind). The 2080 capsules of the Unamed scene are such skins (two bones, one holding all)
static void skin_rigid(World* w, Entity e, uint32_t mesh, GpuInstance* o) {
    uint32_t nb, ver;
    const float(*pal)[3][4] = skin_palette(w, e, &nb, &ver);
    if (!pal || !nb) return; // (no pose yet: its bind)
    uint32_t b = (uint32_t)R.mesh_rigid_bone[mesh];
    if (b >= nb) b = nb - 1u;
    const float(*p)[4] = pal[b];
    float r[3][4];
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 4; c++)
            r[i][c] = o->rows[i][0] * p[0][c] + o->rows[i][1] * p[1][c] + o->rows[i][2] * p[2][c];
        r[i][3] += o->rows[i][3];
    }
    memcpy(o->rows, r, sizeof r);
}
static bool skin_is_rigid(uint32_t mesh) { return mesh < RE_MAX_MESHES && R.mesh_skinned[mesh] && R.mesh_rigid_bone[mesh] >= 0; }

// The fields skin_bind writes into a bone skin's instance (its own vertices, last frame's, its BLAS)
static void skin_instance(const SkinSlot* k, GpuInstance* o) {
    o->ids[1] |= INST_SKINNED;
    o->ids[2] = R.lods[k->mesh][k->rt_lod].first_index; // (the rays' hits: their triangle in the LOD the BLAS holds)
    o->ids[3] = k->base;
    uint32_t prev = k->base + k->nv;
    memcpy(&o->lightmap_st[0], &prev, 4);
    memcpy(&o->lightmap_st[2], &k->blas.addr, 8);
}
// (2026-11-24) Any thread (dyn_one, the dynamic set compared in parallel): slot s's bone skin would do nothing this frame
// (its pose and its morph weights as when it was last bent, or drawn at its bind and still so). o: its instance as
// skin_bind would make it. 0: it has work (or isn't known), 1: its live skin, 2: at its bind
static int skin_still(World* w, Entity e, uint32_t s, uint32_t mesh, GpuInstance* o) {
    if (skin_is_rigid(mesh)) return skin_rigid(w, e, mesh, o), 3; // (2026-11-24: its rows by its bone; compared as any mesh)
    if (s >= R.skin_map_cap) return 0;
    uint32_t nb, ver;
    skin_palette(w, e, &nb, &ver);
    uint32_t i = R.skin_map[s];
    if (i < R.nskins) {
        const SkinSlot* k = &R.skins[i];
        if (k->slot != s || k->e != e || k->mesh != mesh || k->dead_at || !k->done || k->pal_version != ver) return 0;
        if (mesh < RE_MAX_MESHES && R.morph_count[mesh] && R.morphs.buffer) return 0; // (its weights: read on this thread)
        skin_instance(k, o);
        return 1;
    }
    const SkinRaw* r = &R.skin_raw[s];
    if (r->gen == R.skin_gen && r->e == e && r->mesh == mesh && (r->why == SKIN_RAW_ROOM || r->ver == ver)) return 2;
    return 0;
}

// The LOD a skin's rays see (its BLAS, refit every frame it moves): LOD 1 when it has one, like Unreal's ray tracing LOD
// bias for skeletal meshes. A quarter of the triangles: the refit and the rays' traversal cost a quarter less on a
// close character, the rays start that LOD's error off the surface (mesh.vert). A Masked skin (hair cards: their LODs
// thin the cards, the shadows would thin) and REALENGINE_SKIN_RT_LOD=0 keep LOD 0
static uint32_t skin_rt_lod(uint32_t mesh) {
    static int want = -1;
    if (want < 0) want = getenv("REALENGINE_SKIN_RT_LOD") ? atoi(getenv("REALENGINE_SKIN_RT_LOD")) : 1;
    if (mesh >= RE_MAX_MESHES || R.mesh_masked[mesh] || want <= 0) return 0u;
    uint32_t l = (uint32_t)want;
    return l < R.lod_count[mesh] ? l : R.lod_count[mesh] ? R.lod_count[mesh] - 1u : 0u;
}

// A free run of 2 * nv vertices in the room (first fit between the slots, which are kept by base)
static bool skin_alloc(uint32_t nv, uint32_t* base) {
    uint32_t need = nv * 2u, at = R.vertex_count;
    for (uint32_t i = 0; i < R.nskins; i++) {
        if (R.skins[i].base >= at + need) break;
        uint32_t end = R.skins[i].base + R.skins[i].nv * 2u;
        if (end > at) at = end;
    }
    if (at + need > R.vertex_count + R.skin_room) return false;
    *base = at;
    return true;
}

// The skin of slot s (its instance o, made by instance_of): its slot found or made, its palette read, a job when the
// pose changed. True when it was bent this frame (a pose that moved: its shadow, the velocity pass)
// The skins' palette and jobs grown (between frames: the GPU idle once); false when a buffer could not be made
static bool skin_pal_grow(VkCtx* vk, uint32_t need, uint32_t fslot) {
    uint32_t pc = R.skin_pal_cap ? R.skin_pal_cap : 4096u, hc = R.skin_hdr_cap ? R.skin_hdr_cap : 64u;
    while (pc < need * 2u) pc *= 2u;
    while (hc <= R.nskin_jobs * 2u) hc *= 2u;
    vkDeviceWaitIdle(vk->device);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        // (this frame's slot keeps what was written this frame: copied over)
        GpuBuf np = { 0 }, nh = { 0 };
        if (!gpu_buf_create(vk, &np, (VkDeviceSize)pc * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &nh, (VkDeviceSize)hc * sizeof(SkinJobGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
            return gpu_buf_destroy(vk, &np), false;
        if ((uint32_t)f == fslot && R.skin_pal[f].mapped) {
            memcpy(np.mapped, R.skin_pal[f].mapped, (size_t)R.skin_pal_used * 16u);
            memcpy(nh.mapped, R.skin_hdr[f].mapped, (size_t)R.nskin_jobs * sizeof(SkinJobGpu));
            // (2026-11-16, crashes #135-137: VK_ERROR_DEVICE_LOST in "skin (ossos e BLAS)") this frame's jobs
            // already written point into the old palette (their bones and morph weights), freed just below: moved
            // to the new one. A sublevel added (every Blueprint part made again: their skins' first bends) grew
            // the palette mid-frame and the GPU read freed memory
            VkDeviceAddress d0 = R.skin_pal[f].addr, d1 = np.addr;
            SkinJobGpu* jh = nh.mapped;
            for (uint32_t q = 0; q < R.nskin_jobs; q++) {
                if (jh[q].pal >= d0 && jh[q].pal < d0 + (VkDeviceAddress)R.skin_pal_cap * 16u) jh[q].pal = jh[q].pal - d0 + d1;
                if (jh[q].mweight >= d0 && jh[q].mweight < d0 + (VkDeviceAddress)R.skin_pal_cap * 16u) jh[q].mweight = jh[q].mweight - d0 + d1;
            }
        }
        gpu_buf_destroy(vk, &R.skin_pal[f]), gpu_buf_destroy(vk, &R.skin_hdr[f]);
        R.skin_pal[f] = np, R.skin_hdr[f] = nh;
    }
    R.skin_pal_cap = pc, R.skin_hdr_cap = hc;
    return true;
}

// (Q14) The skins that found no room this frame, all at once: the meshes reloaded with room for them (twice what they
// missed, at least the room doubled); one reload, not one per skin
static void skin_room_grow(VkCtx* vk) {
    if (!R.skin_short) return;
    uint32_t want = R.skin_room + R.skin_short * 2u, twice = R.skin_room * 2u;
    if (want < twice) want = twice;
    R.skin_short = 0;
    if (want <= R.skin_room_need) return; // (asked already: the reload on its way)
    if (R.skin_room >= SKIN_ROOM_MAX) { // (2026-11-24) the room at its most: a reload of every mesh would give none more
        R.skin_room_need = want;
        printf("[RealEngine] Skins: out of room at the most (%u vertices): the rest drawn at their bind\n", R.skin_room);
        return;
    }
    R.skin_room_need = want;
    printf("[RealEngine] Skins: out of room (%u vertices), the meshes reloaded with %u\n", R.skin_room, R.skin_room_need);
    render_meshes_reload_async(vk);
}

// (Q9) sv: a soft body's (cloth.c): its mesh's vertices bent from its lattice by skin.comp's soft branch instead of bones;
// (Q14) spv: a Spline Mesh piece's (spline.h): bent along its Hermite by skin.comp's spline branch
static bool skin_bind(VkCtx* vk, World* w, uint32_t s, Entity e, uint32_t fslot, GpuInstance* o, const ClothSoftView* sv, const SplineMeshView* spv) {
    uint32_t mesh = o->ids[1] >> INST_SHAPE_SHIFT;
    const Mesh* me = &R.meshes[mesh];
    if (!sv && !spv && skin_is_rigid(mesh)) { // (2026-11-24, item 4) a rigid skin: its rows by its bone, nothing bent
        skin_rigid(w, e, mesh, o);
        R.skin_rigid_n++;
        return false;
    }
    if (!skin_map_room(s)) return false;
    // (2026-11-24, the Unamed scene) its skin by the slot's map (a search of the whole list for each skin: O(n^2) a frame)
    SkinSlot* k = skin_find(s, e, mesh);
    if (!k) {
        SkinRaw* r = &R.skin_raw[s];
        // (2026-11-24) no room for it since the last load or the last skin freed: drawn at its bind, not tried again (the
        // first fit over the whole list for each of 1764 capsules, every frame)
        if (r->gen == R.skin_gen && r->e == e && r->mesh == mesh && r->why == SKIN_RAW_ROOM) return R.skin_noroom_n++, false;
        if (!sv && !spv) { // a bone skin whose pose is its bind's: drawn from the mesh's vertices (no room, no bend, no BLAS)
            uint32_t nb, ver;
            if (r->gen == R.skin_gen && r->e == e && r->mesh == mesh && r->why == SKIN_RAW_BIND) {
                skin_palette(w, e, &nb, &ver);
                if (ver == r->ver) return false;
            }
            const float(*pal)[3][4] = skin_palette(w, e, &nb, &ver);
            if (skin_pose_is_bind(mesh, pal, nb)) {
                *r = (SkinRaw){ e, mesh, R.skin_gen, ver, SKIN_RAW_BIND };
                return false;
            }
        }
        uint32_t nv = skin_mesh_nv(mesh), base; // (LOD 0's vertices: counted at the load)
        if (!nv || R.nskins >= SKINS_MAX || !skin_alloc(nv, &base)) { // (2026-11-24: was 4096; the lists grow, their room is the limit)
            // no room: the meshes are made again with more (between frames), the skin drawn at its bind meanwhile
            // (Q14) summed over the frame, one reload after it (skin_room_grow): a level of many small skins (Spline Mesh
            // pieces, a cable's segments) reloaded the meshes once per piece, a dozen reloads in a row
            R.skin_short += nv * 2u;
            *r = (SkinRaw){ e, mesh, R.skin_gen, 0u, SKIN_RAW_ROOM };
            R.skin_noroom_n++;
            return false;
        }
        if (R.nskins >= R.skins_cap) {
            uint32_t c = R.skins_cap ? R.skins_cap * 2u : 16u;
            SkinSlot* ns = realloc(R.skins, sizeof(SkinSlot) * c); // (a slot list, not a frame's)
            if (!ns) return false;
            R.skins = ns, R.skins_cap = c;
        }
        uint32_t at = 0; // kept in base order
        while (at < R.nskins && R.skins[at].base < base) at++;
        memmove(&R.skins[at + 1], &R.skins[at], sizeof(SkinSlot) * (R.nskins - at));
        R.nskins++;
        // (2026-11-16, crashes #135-137: VK_ERROR_DEVICE_LOST in "skin (ossos e BLAS)") this frame's jobs already made
        // name their skins by place in the list: the ones after the new one moved one up. Left as they were, a job bent
        // and refit another skin's BLAS with this one's vertices and triangles (a refit of other geometry: the GPU hung)
        for (uint32_t q = 0; q < R.nskin_jobs; q++)
            if (R.skin_jobs[q] >= at) R.skin_jobs[q]++;
        k = &R.skins[at];
        memset(k, 0, sizeof *k);
        k->slot = s, k->e = e, k->mesh = mesh, k->nv = nv, k->src = (uint32_t)me->vertex_offset, k->base = base;
        skin_map_fix(at); // (it and the ones after it, one place up)
        R.skin_raw[s].gen = 0; // (not at its bind any more)
        k->rt_lod = sv || spv ? 0u : skin_rt_lod(mesh); // (a soft body's or a Spline Mesh's: its LOD 0, the vertices it bends)
        if (gi_available()) gi_dyn_blas_create(vk, &k->blas, R.lods[mesh][k->rt_lod].index_count / 3u, nv - 1u);
        if (mesh < RE_MAX_MESHES) R.omm_bad[mesh] = true; // (its BLAS is its own: no micromap)
    }
    k->seen = R.frame_no;
    if (spv) { // (Q14) a Spline Mesh piece: 12 vec4 (the rows both ways in cm, its Hermite, roll, scale, up and the mesh's axis)
        uint32_t ver = spv->version;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) {
                uint32_t b;
                memcpy(&b, &o->rows[r][c], 4);
                ver = (ver ^ b) * 16777619u;
            }
        bool bent = false;
        if (!k->done || ver != k->pal_version) {
            uint32_t need = R.skin_pal_used + 12u;
            if ((need > R.skin_pal_cap || R.nskin_jobs >= R.skin_hdr_cap) && !skin_pal_grow(vk, need, fslot)) return false;
            float* dst = (float*)R.skin_pal[fslot].mapped + (size_t)R.skin_pal_used * 4u;
            const float(*m)[4] = o->rows;
            float a[9] = { m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2] };
            float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
            if (fabsf(det) < 1e-12f) return false;
            float id = 1.0f / det, inv[9] = { (a[4] * a[8] - a[5] * a[7]) * id, (a[2] * a[7] - a[1] * a[8]) * id, (a[1] * a[5] - a[2] * a[4]) * id,
                                             (a[5] * a[6] - a[3] * a[8]) * id, (a[0] * a[8] - a[2] * a[6]) * id, (a[2] * a[3] - a[0] * a[5]) * id,
                                             (a[3] * a[7] - a[4] * a[6]) * id, (a[1] * a[6] - a[0] * a[7]) * id, (a[0] * a[4] - a[1] * a[3]) * id };
            for (int r = 0; r < 3; r++) { // world (cm) -> mesh (cm): inv (w - t)
                for (int c = 0; c < 3; c++) dst[4 * r + c] = inv[3 * r + c];
                dst[4 * r + 3] = -(inv[3 * r] * m[0][3] + inv[3 * r + 1] * m[1][3] + inv[3 * r + 2] * m[2][3]);
            }
            for (int r = 0; r < 3; r++) dst[12 + 4 * r] = m[r][0], dst[13 + 4 * r] = m[r][1], dst[14 + 4 * r] = m[r][2], dst[15 + 4 * r] = 0.0f;
            const float q[24] = { spv->p0[0], spv->p0[1], spv->p0[2], spv->roll0, spv->t0[0], spv->t0[1], spv->t0[2], spv->roll1,
                                  spv->p1[0], spv->p1[1], spv->p1[2], spv->xmin, spv->t1[0], spv->t1[1], spv->t1[2], spv->inv_len,
                                  spv->s0[0], spv->s0[1], spv->s1[0], spv->s1[1], spv->up[0], spv->up[1], spv->up[2], (float)(spv->axis % 3u) };
            memcpy(&dst[24], q, sizeof q);
            SkinJobGpu j = { k->src, k->base, k->base + k->nv, k->nv, R.skin_pal[fslot].addr + (VkDeviceAddress)R.skin_pal_used * 16u,
                             k->done ? 0u : 1u, 0u, 0, 0, 0, { 12u, 2u } }; // (pad[1] 2: skin.comp's spline branch)
            memcpy((SkinJobGpu*)R.skin_hdr[fslot].mapped + R.nskin_jobs, &j, sizeof j);
            R.skin_jobs[R.nskin_jobs++] = (uint32_t)(k - R.skins);
            R.skin_pal_used = need;
            R.skin_verts += k->nv;
            bent = k->done;
            k->done = true, k->pal_version = ver;
        }
        o->ids[1] |= INST_SKINNED;
        o->ids[2] = R.lods[mesh][0].first_index;
        o->ids[3] = k->base;
        uint32_t prev = k->base + k->nv;
        memcpy(&o->lightmap_st[0], &prev, 4);
        memcpy(&o->lightmap_st[2], &k->blas.addr, 8);
        return bent;
    }
    if (sv) { // (Q9) a soft body: 8 vec4 (the rows both ways, the lattice, the scale over the cell), its lattice's positions
        if (sv->nrender != k->nv) return false; // (its embedding not for this mesh yet: drawn whole)
        uint32_t ver = sv->version;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) {
                uint32_t b;
                memcpy(&b, &o->rows[r][c], 4);
                ver = (ver ^ b) * 16777619u;
            }
        bool bent = false;
        if (!k->done || ver != k->pal_version) {
            uint32_t need = R.skin_pal_used + 8u;
            if ((need > R.skin_pal_cap || R.nskin_jobs >= R.skin_hdr_cap) && !skin_pal_grow(vk, need, fslot)) return false;
            float* dst = (float*)R.skin_pal[fslot].mapped + (size_t)R.skin_pal_used * 4u;
            const float(*m)[4] = o->rows;
            float a[9] = { m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2] };
            float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
            if (fabsf(det) < 1e-12f) return false;
            float id = 1.0f / det, inv[9] = { (a[4] * a[8] - a[5] * a[7]) * id, (a[2] * a[7] - a[1] * a[8]) * id, (a[1] * a[5] - a[2] * a[4]) * id,
                                             (a[5] * a[6] - a[3] * a[8]) * id, (a[0] * a[8] - a[2] * a[6]) * id, (a[2] * a[3] - a[0] * a[5]) * id,
                                             (a[3] * a[7] - a[4] * a[6]) * id, (a[1] * a[6] - a[0] * a[7]) * id, (a[0] * a[4] - a[1] * a[3]) * id };
            for (int r = 0; r < 3; r++) { // world (m) -> mesh (cm): inv (100 w - t)
                for (int c = 0; c < 3; c++) dst[4 * r + c] = inv[3 * r + c] * 100.0f;
                dst[4 * r + 3] = -(inv[3 * r] * m[0][3] + inv[3 * r + 1] * m[1][3] + inv[3 * r + 2] * m[2][3]);
            }
            for (int r = 0; r < 3; r++) dst[12 + 4 * r] = m[r][0], dst[13 + 4 * r] = m[r][1], dst[14 + 4 * r] = m[r][2], dst[15 + 4 * r] = 0.0f;
            uint32_t lat[4] = { sv->base, sv->nx, sv->ny, sv->nz };
            memcpy(&dst[24], lat, 16);
            dst[28] = sv->scale_over_cell[0], dst[29] = sv->scale_over_cell[1], dst[30] = sv->scale_over_cell[2], dst[31] = 0.0f;
            SkinJobGpu j = { k->src, k->base, k->base + k->nv, k->nv, R.skin_pal[fslot].addr + (VkDeviceAddress)R.skin_pal_used * 16u,
                             k->done ? 0u : 1u, 0u, sv->embed, sv->pos, 0, { 8u, 1u } }; // (pad[1] 1: skin.comp's soft branch)
            memcpy((SkinJobGpu*)R.skin_hdr[fslot].mapped + R.nskin_jobs, &j, sizeof j);
            R.skin_jobs[R.nskin_jobs++] = (uint32_t)(k - R.skins);
            R.skin_pal_used = need;
            R.skin_verts += k->nv;
            bent = k->done;
            k->done = true, k->pal_version = ver;
        }
        o->ids[1] |= INST_SKINNED;
        o->ids[2] = R.lods[mesh][0].first_index;
        o->ids[3] = k->base;
        uint32_t prev = k->base + k->nv;
        memcpy(&o->lightmap_st[0], &prev, 4);
        memcpy(&o->lightmap_st[2], &k->blas.addr, 8);
        return bent;
    }
    // its palette: the CSkeletalMesh it belongs to (none yet: its bind)
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    uint32_t nb, ver;
    const float(*pal)[3][4] = skin_palette(w, e, &nb, &ver);
    bool bent = false;
    // its morph targets' weights (nothing to do without: the CPU cost is a table read)
    float mw[RMESH_MAX_MORPHS];
    bool many = false;
    uint32_t nmorph = R.morph_count[mesh] && R.morphs.buffer ? R.morph_count[mesh] : 0u, mhash = k->morph_hash;
    if (nmorph) mhash = skin_morph_weights(w, k, at ? at->owner : ENTITY_NONE, mw, &many);
    if (!k->done || ver != k->pal_version || mhash != k->morph_hash) {
        uint32_t bones = nb ? nb : 1u, mvec = many ? (nmorph + 3u) / 4u : 0u, need = R.skin_pal_used + bones * 3u + mvec;
        if ((need > R.skin_pal_cap || R.nskin_jobs >= R.skin_hdr_cap) && !skin_pal_grow(vk, need, fslot)) return false; // (grown between frames: the GPU idle once)
        float* dst = (float*)R.skin_pal[fslot].mapped + (size_t)R.skin_pal_used * 4u;
        if (pal) memcpy(dst, pal, sizeof(float) * 12u * nb);
        else {
            static const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
            memcpy(dst, id, sizeof id);
        }
        SkinJobGpu j = { k->src, k->base, k->base + k->nv, k->nv, R.skin_pal[fslot].addr + (VkDeviceAddress)R.skin_pal_used * 16u,
                         k->done ? 0u : 1u, 0u, 0, 0, 0, { bones, 0 } }; // (pad[0]: the palette's bones, skin.comp clamps to them)
        if (mvec) { // the weights after the bones; the mesh's CSR and deltas
            float* wd = dst + (size_t)bones * 12u;
            memset(wd, 0, sizeof(float) * 4u * mvec);
            memcpy(wd, mw, sizeof(float) * nmorph);
            j.nmorph = R.morph_off ? 0u : nmorph;
            j.mfirst = R.morphs.addr + (VkDeviceAddress)R.morph_first[mesh] * 4u;
            j.mdelta = R.morphs.addr + (VkDeviceAddress)R.morph_delta[mesh] * 4u;
            j.mweight = R.skin_pal[fslot].addr + (VkDeviceAddress)(R.skin_pal_used + bones * 3u) * 16u;
            R.morph_jobs++;
            R.morph_deltas += k->nv;
        }
        memcpy((SkinJobGpu*)R.skin_hdr[fslot].mapped + R.nskin_jobs, &j, sizeof j);
        R.skin_jobs[R.nskin_jobs++] = (uint32_t)(k - R.skins);
        R.skin_pal_used = need;
        R.skin_verts += k->nv;
        bent = k->done; // (the first bend isn't a motion)
        k->done = true, k->pal_version = ver, k->morph_hash = mhash;
    }
    skin_instance(k, o); // (the same fields skin_still gives a still one: dyn_one compares them)
    return bent;
}

// The skins whose entity went (not seen for a few frames: its BLAS still read by the frames in flight): freed
static void skin_sweep(VkCtx* vk) {
    for (uint32_t i = 0; i < R.nskins;) {
        SkinSlot* k = &R.skins[i];
        if (k->seen + 2u < R.frame_no && !k->dead_at) {
            k->dead_at = R.frame_no;
            if (k->slot < R.skin_map_cap && R.skin_map[k->slot] == i) R.skin_map[k->slot] = UINT32_MAX;
        }
        if (k->dead_at && R.frame_no - k->dead_at > RE_FRAMES_IN_FLIGHT + 1u) {
            gi_dyn_blas_free(vk, &k->blas);
            memmove(k, k + 1, sizeof(SkinSlot) * (R.nskins - i - 1));
            R.nskins--;
            for (uint32_t q = 0; q < R.nskin_jobs; q++) // (this frame's jobs after it: one place down, as in skin_bind)
                if (R.skin_jobs[q] > i) R.skin_jobs[q]--;
            skin_map_fix(i);
            R.skin_gen++; // (its room free: the skins without room try again)
            continue;
        }
        i++;
    }
}

// This frame's bends (before the cull and the TLAS): the jobs, then the BLAS refits
static void ts(VkCommandBuffer cmd, uint32_t slot, uint32_t i, VkPipelineStageFlags2 stage);
static void skin_record(VkCommandBuffer cmd, uint32_t slot, Push push) {
    R.skin_ts[slot] = R.nskin_jobs && R.queries;
    if (!R.nskin_jobs) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // (the frames before done reading these vertices: this frame overwrites them)
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    ts(cmd, slot, 16, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT); // (the bends' GPU time: RenderStats.gpu_skin_ms)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.skin_pipe);
    for (uint32_t i = 0; i < R.nskin_jobs; i++) {
        const SkinSlot* k = &R.skins[R.skin_jobs[i]];
        Push p = push;
        p.frame = R.skin_hdr[slot].addr + (VkDeviceAddress)i * sizeof(SkinJobGpu);
        p.vertices = R.vertices.addr;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (k->nv + 63u) / 64u, 1, 1);
    }
    ts(cmd, slot, 17, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    VkPipelineStageFlags2 AS = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    bool rt = gi_available();
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | (rt ? AS : 0),
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | (rt ? VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR : 0));
    if (!rt) return;
    // (2026-11-24, item 4) in one build call (5000 animated skins were 5000)
    GiDynBlasJob* bj = R.nskin_jobs ? RE_FRAME_ALLOC(GiDynBlasJob, R.nskin_jobs) : NULL;
    for (uint32_t i = 0; i < R.nskin_jobs; i++) {
        SkinSlot* k = &R.skins[R.skin_jobs[i]];
        const Mesh* me = &R.lods[k->mesh][k->rt_lod];
        if (!bj) { // (no frame memory: one by one)
            gi_dyn_blas_record(cmd, &k->blas, R.vertices.addr + (VkDeviceAddress)k->base * sizeof(GpuVertex), k->nv - 1u,
                               R.indices.addr + (VkDeviceAddress)me->first_index * sizeof(uint32_t));
            continue;
        }
        bj[i] = (GiDynBlasJob){ &k->blas, R.vertices.addr + (VkDeviceAddress)k->base * sizeof(GpuVertex),
                                R.indices.addr + (VkDeviceAddress)me->first_index * sizeof(uint32_t), k->nv - 1u };
    }
    if (bj) gi_dyn_blas_record_batch(cmd, bj, R.nskin_jobs);
    gpu_barrier(cmd, AS, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, AS | CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

// The cull's and the TLAS's table of the meshes: bounds, LODs, BLAS
static void upload_mesh_info(VkCtx* vk) {
    static GpuMeshInfo mi[RE_MAX_MESHES];
    memset(mi, 0, sizeof mi);
    for (uint32_t s = 0; s < R.nmeshes; s++) {
        const Mesh* m = &R.lods[s][0];
        float he[3];
        for (int k = 0; k < 3; k++) mi[s].sphere[k] = (m->mn[k] + m->mx[k]) * 0.5f, he[k] = (m->mx[k] - m->mn[k]) * 0.5f;
        mi[s].sphere[3] = sqrtf(he[0] * he[0] + he[1] * he[1] + he[2] * he[2]);
        if (R.mesh_skinned[s]) mi[s].sphere[3] *= 1.35f; // (a skin's pose reaches past its bind's bounds: arms, a kick)
        mi[s].lod_count = R.lod_count[s] | (R.mesh_masked[s] ? MESH_INFO_MASKED : 0u) | (R.mesh_error_lod[s] ? MESH_INFO_ERROR_LOD : 0u) |
                          (R.mesh_translucent[s] ? MESH_INFO_TRANSLUCENT : 0u) | (R.mesh_omm[s] ? MESH_INFO_OMM : 0u) |
                          (R.mesh_ext[s] ? MESH_INFO_EXT : 0u) | // (2026-11-24: its own run of draws, the RE_EXT pipelines there only)
                          (R.mesh_skinned[s] ? MESH_INFO_SKINNED | (skin_rt_lod(s) << MESH_INFO_RT_LOD_SHIFT) : 0u);
        for (int l = 1; l < MAX_LODS; l++) mi[s].lod_error[l - 1] = R.lod_error[s][l];
        for (uint32_t l = 0; l < MAX_LODS; l++) {
            const Mesh* lm = &R.lods[s][l < R.lod_count[s] ? l : 0];
            mi[s].first_index[l] = lm->first_index;
            mi[s].index_count[l] = lm->index_count;
            mi[s].vertex_offset[l] = lm->vertex_offset;
        }
        mi[s].blas = gi_blas_address((int)s);
        for (uint32_t l = 0; l < MAX_LODS; l++)
            mi[s].cluster_first[l] = R.cluster_first[s][l], mi[s].cluster_count[l] = l < R.lod_count[s] ? R.cluster_count[s][l] : 0u;
        mi[s].dag_first = R.dag_first[s], mi[s].dag_count = R.dag_count[s];
        mi[s].vcol_first = s >= SHAPE_COUNT ? R.vcol_first[s] : UINT32_MAX; // (the basic shapes: none)
        mi[s].tess_first = s >= SHAPE_COUNT ? R.tess_first[s] : UINT32_MAX; // (Q17)
        if (s == SHAPE_SPHERE) mi[s].uv2_wrap[0] = 0.02f, mi[s].uv2_wrap[1] = 1.0f; // mb_vertex's and build_round's unwrap
        if (s == SHAPE_CYLINDER || s == SHAPE_CONE) mi[s].uv2_wrap[0] = 0.02f, mi[s].uv2_wrap[1] = 0.6f;
    }
    upload(vk, &R.mesh_info, mi, sizeof mi);
}

static uint32_t g_mesh_generation;
uint32_t render_meshes_generation(void) { return g_mesh_generation; }
bool render_any_dag(void) { return R.any_dag; }

static double g_apply_split[3]; // the last apply's GPU waits (ms): the frames in flight, the copies, the BLAS builds
static bool reload_apply(VkCtx* vk, MeshStage* st) {
    double q0 = re_time_seconds();
    vkDeviceWaitIdle(vk->device);
    double q1 = re_time_seconds();
    g_mesh_generation++; // (the CPU copies render_shape_cpu handed out are freed)
    gpu_buf_destroy(vk, &R.vertices);
    gpu_buf_destroy(vk, &R.indices);
    gpu_buf_destroy(vk, &R.clusters), gpu_buf_destroy(vk, &R.cl_work), gpu_buf_destroy(vk, &R.cl_idx), gpu_buf_destroy(vk, &R.cl_perm); for (int f_ = 0; f_ < RE_FRAMES_IN_FLIGHT; f_++) gpu_buf_destroy(vk, &R.cl_read[f_]);
    free(R.cpu_v), free(R.cpu_i);
    R.cpu_v = NULL, R.cpu_i = NULL;
    skin_reset(vk); // (the vertices' buffer is new: every skin bent again into it)
    GiBlasPrep* blas = st->blas; // (stage_upload frees the stage)
    farview_meshes_changed(vk); // (Q21: the impostors' slots may hold other meshes now)
    if (!stage_upload(vk, st)) return gi_blas_drop(vk, blas), false;
    double q2 = re_time_seconds();
    if (blas) gi_blas_commit(vk, blas);
    else {
        GiMesh gm[RE_MAX_MESHES];
        for (uint32_t s = 0; s < R.nmeshes; s++) gm[s] = (GiMesh){ R.meshes[s].first_index, R.meshes[s].index_count, R.meshes[s].vertex_offset };
        gi_rebuild_blas(vk, &R.vertices, R.vertex_count, &R.indices, gm, (int)R.nmeshes);
    }
    double q3 = re_time_seconds();
    upload_mesh_info(vk);
    gi_cache_meshes_changed();
    render_rebind_descriptors(vk); // (the vertices' and indices' buffers are new)
    g_apply_split[0] = (q1 - q0) * 1e3, g_apply_split[1] = (q2 - q1) * 1e3, g_apply_split[2] = (q3 - q2) * 1e3;
    R.scene_valid = false;        // every instance sent again (the slots of the meshes may have moved)
    memset(R.mesh_masked, 0, sizeof R.mesh_masked), R.any_masked = false, R.any_tess = false; // (marked again as they are sent)
    memset(R.mesh_translucent, 0, sizeof R.mesh_translucent), R.any_translucent = false;
    R.any_ext = false, R.any_ext_masked = false, R.ext_mat = R.ext_mat_masked = false;
    memset(R.mesh_ext, 0, sizeof R.mesh_ext); // (2026-11-24: marked again as they are sent, their runs with them)
    R.any_sss = false;
    // (the BLAS made again without micromaps)
    memset(R.omm_slot, 0, sizeof R.omm_slot), memset(R.omm_bad, 0, sizeof R.omm_bad), memset(R.omm_done, 0, sizeof R.omm_done);
    memset(R.mesh_omm, 0, sizeof R.mesh_omm);
    return true;
}

bool render_meshes_reload(VkCtx* vk) {
    MeshStage* st = stage_new();
    if (!st) return false;
    stage_shapes(st);
    stage_imported(st, NULL);
    stage_card(st);
    return reload_apply(vk, st);
}

// The same, the files read by a worker (async.h): the editor keeps drawing the meshes it had until they are in place.
// A request while one runs: one more after it (the files may have changed again meanwhile)
static struct { VkCtx* vk; bool running, again; uint32_t id; double ms_apply; } RL;
static void reload_work(AsyncTask* t, void* arg) {
    MeshStage* st = arg;
    async_status(t, "%s", TR("Reading the files..."));
    stage_shapes(st);
    stage_imported(st, t);
    stage_card(st);
    async_status(t, "%s", TR("Sending them to the GPU..."));
    if (!async_cancelled(t) && stage_gpu_prepare(RL.vk, st)) { // (failed: the main thread tries again)
        GiMesh gm[RE_MAX_MESHES];
        for (uint32_t s = 0; s < st->nmeshes; s++) {
            gm[s] = (GiMesh){ st->meshes[s].first_index, st->meshes[s].index_count, st->meshes[s].vertex_offset };
            // (2026-11-24, item 4) its rays on the coarsest LOD still holding its Fallback share of LOD 0's triangles (.rlod)
            float fb = s >= SHAPE_COUNT && st->mesh_fallback[s] > 0.0f ? st->mesh_fallback[s] : 100.0f;
            for (uint32_t l = 1; fb < 99.99f && l < st->lod_count[s] && l < MAX_LODS; l++)
                if ((double)st->lods[s][l].index_count >= (double)st->lods[s][0].index_count * fb / 100.0)
                    gm[s].first_index = st->lods[s][l].first_index, gm[s].index_count = st->lods[s][l].index_count;
        }
        st->blas = gi_blas_prepare(RL.vk, st->g_v.addr, st->b.nv, st->g_i.addr, gm, (int)st->nmeshes, st->b.v, st->b.idx);
    }
}
static void reload_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    MeshStage* st = arg;
    double t0 = re_time_seconds();
    if (ok) reload_apply(RL.vk, st);
    else gi_blas_drop(RL.vk, st->blas), stage_gpu_drop(RL.vk, st), stage_free(st);
    RL.ms_apply = (re_time_seconds() - t0) * 1000.0;
    printf("[RealEngine] Meshes read again in the background: %.1f ms on the main thread (GPU idle %.1f, copies %.1f, BLAS %.1f)\n",
           RL.ms_apply, g_apply_split[0], g_apply_split[1], g_apply_split[2]);
    RL.running = false;
    if (RL.again) {
        RL.again = false;
        render_meshes_reload_async(RL.vk);
    }
}
void render_meshes_reload_async(VkCtx* vk) {
    RL.vk = vk;
    if (RL.running) {
        RL.again = true;
        return;
    }
    MeshStage* st = stage_new();
    if (!st) return;
    RL.running = true;
    RL.id = async_run(TR("Loading the meshes"), reload_work, reload_done, st, ASYNC_QUIET);
}
bool render_meshes_reloading(void) { return RL.running; }
double render_meshes_apply_ms(void) { return RL.ms_apply; }

// AR's picture: Y (w x h, R8) and UV (w/2 x h/2, RG8), read-only for the shaders (filled by ar_record)
static bool ar_images(VkCtx* vk, uint32_t w, uint32_t h) {
    for (int i = 0; i < 2; i++) gpu_image_destroy(vk, &R.ar_img[i]);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!gpu_image_create(vk, &R.ar_img[0], w, h, VK_FORMAT_R8_UNORM, u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !gpu_image_create(vk, &R.ar_img[1], w / 2 ? w / 2 : 1, h / 2 ? h / 2 : 1, VK_FORMAT_R8G8_UNORM, u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    for (int i = 0; i < 2; i++)
        gpu_transition(cmd, &R.ar_img[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
    gpu_end_once(vk, cmd);
    R.ar_w = w, R.ar_h = h;
    return true;
}

void render_ar_frame(bool on, const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, uint64_t serial, const float corners[4][2],
                     const float* tris, uint32_t nverts, float shadow) {
    static uint64_t last;
    R.ar_nverts = 0;
    if (!on) { R.ar_on = false, last = 0; return; }
    R.ar_on = true;
    if (corners) memcpy(R.ar_corners, corners, sizeof R.ar_corners);
    R.ar_shadow = shadow;
    if (y && uv && w >= 2 && h >= 2 && serial != last) { // (a new picture: copied; ar_record sends it to the GPU)
        last = serial;
        size_t need = (size_t)w * h + (size_t)(w / 2) * (h / 2) * 2;
        if (need > R.ar_pic_cap) {
            uint8_t* p = realloc(R.ar_pic, need);
            if (!p) return;
            R.ar_pic = p, R.ar_pic_cap = need;
        }
        memcpy(R.ar_pic, y, (size_t)w * h);
        memcpy(R.ar_pic + (size_t)w * h, uv, (size_t)(w / 2) * (h / 2) * 2);
        R.ar_fresh = true;
        if (w != R.ar_w || h != R.ar_h) R.ar_w = w | 0x80000000u, R.ar_h = h; // (resized at the next frame's start)
    }
    nverts = nverts > 4096 ? 4096 - 4096 % 3 : nverts;
    for (uint32_t i = 0; i < nverts; i++) R.ar_tris[i][0] = tris[3 * i], R.ar_tris[i][1] = tris[3 * i + 1], R.ar_tris[i][2] = tris[3 * i + 2], R.ar_tris[i][3] = 1.0f;
    R.ar_nverts = nverts;
}

static bool ar_depth_image(VkCtx* vk, uint32_t w, uint32_t h) {
    gpu_image_destroy(vk, &R.ar_depth);
    if (!gpu_image_create(vk, &R.ar_depth, w, h, VK_FORMAT_R16_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, &R.ar_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
    gpu_end_once(vk, cmd);
    R.ar_dw = w, R.ar_dh = h;
    return true;
}

void render_ar_depth(const uint16_t* mm, uint32_t w, uint32_t h, uint64_t serial, const float corners[4][2]) {
    static uint64_t last;
    if (!mm || w < 2 || h < 2) { R.ar_depth_on = false, last = 0; return; }
    R.ar_depth_on = true;
    memcpy(R.ar_dcorners, corners, sizeof R.ar_dcorners);
    if (serial == last) return;
    last = serial;
    size_t need = (size_t)w * h * 2;
    if (need > R.ar_dpic_cap) {
        uint16_t* p = realloc(R.ar_dpic, need);
        if (!p) { R.ar_depth_on = false; return; }
        R.ar_dpic = p, R.ar_dpic_cap = need;
    }
    memcpy(R.ar_dpic, mm, need);
    R.ar_dfresh = true;
    if (w != R.ar_dw || h != R.ar_dh) R.ar_dw = w | 0x80000000u, R.ar_dh = h; // (resized at the next frame's start)
}

// --- (Q25b) Mixed reality (render_mr_frame, shaders/mr.glsl) ---
static bool mr_depth_image(VkCtx* vk, uint32_t w, uint32_t h) { // (D16, 2 layers: an eye each; sampled, copied into)
    gpu_image_destroy(vk, &R.mr_depth);
    GpuImage* im = &R.mr_depth;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D, ii.format = VK_FORMAT_D16_UNORM, ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = 1, ii.arrayLayers = 2, ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(vk->device, &ii, NULL, &im->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, im->image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size, ai.memoryTypeIndex = vkctx_find_memory(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk->device, &ai, NULL, &im->memory) != VK_SUCCESS) return false;
    vk_check(vkBindImageMemory(vk->device, im->image, im->memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, vi.format = ii.format;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 };
    vk_check(vkCreateImageView(vk->device, &vi, NULL, &im->view), "vkCreateImageView");
    im->format = ii.format, im->layout = VK_IMAGE_LAYOUT_UNDEFINED, im->layers = 2;
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, b.image = im->image;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    gpu_end_once(vk, cmd);
    im->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    R.mr_dw = w, R.mr_dh = h;
    return true;
}

void render_mr_frame(const RenderMr* m) {
    if (!m || !m->on) { R.mr_on = false; return; }
    R.mr = *m;
    R.mr_on = true;
    uint32_t n = m->nboxes > MR_MAX_BOXES ? MR_MAX_BOXES : m->nboxes;
    if (n && m->boxes) memcpy(R.mr_box, m->boxes, (size_t)n * sizeof R.mr_box[0]);
    R.mr.nboxes = m->boxes ? n : 0, R.mr.boxes = NULL;
    R.mr.depth_cpu = NULL;
    R.mr_dvalid = m->depth && m->depth_w >= 2 && m->depth_h >= 2 && (m->depth_image || m->depth_cpu);
    if (!R.mr_dvalid) return;
    if (m->depth_w != R.mr_dw || m->depth_h != R.mr_dh) R.mr_dresize = true; // (made at the next frame's start)
    if (m->depth_serial != R.mr_dserial || R.mr_dresize) {
        R.mr_dserial = m->depth_serial, R.mr_dfresh = true;
        R.mr_dsrc = m->depth_image;
        if (m->depth_cpu) { // (the simulated one: kept until sent)
            size_t need = (size_t)m->depth_w * m->depth_h * 2u * 2u;
            if (need > R.mr_dcpu_cap) {
                uint16_t* p = realloc(R.mr_dcpu, need);
                if (!p) { R.mr_dvalid = false; return; }
                R.mr_dcpu = p, R.mr_dcpu_cap = need;
            }
            memcpy(R.mr_dcpu, m->depth_cpu, need);
            R.mr_dsrc = VK_NULL_HANDLE;
        }
    }
}

// The boxes into this frame slot's buffer; the first eye's frame sends the new real depth (the second reads it)
static void mr_record(VkCtx* vk, VkCommandBuffer cmd, uint32_t slot, bool first_eye) {
    if (!R.mr_on) return;
    if (R.mr.nboxes) {
        if (!R.mr_buf[slot].buffer) gpu_buf_create(vk, &R.mr_buf[slot], sizeof R.mr_box, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (R.mr_buf[slot].mapped) memcpy(R.mr_buf[slot].mapped, R.mr_box, (size_t)R.mr.nboxes * sizeof R.mr_box[0]);
        else R.mr.nboxes = 0;
    }
    if (!first_eye || !R.mr_dvalid || !R.mr_dfresh || R.mr_dresize) return;
    if (!R.mr_dsrc && !R.mr_dcpu) return;
    R.mr_dfresh = false;
    VkImageMemoryBarrier2 b[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 } };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 };
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, b[0].srcAccessMask = 0;
    b[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[0].image = R.mr_depth.image, b[0].subresourceRange = all;
    uint32_t nb = 1;
    if (R.mr_dsrc) { // the headset's image: handed for sampling (Meta's runtime), read here as a copy's source and given back
        b[1] = b[0];
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[1].image = R.mr_dsrc;
        nb = 2;
    }
    di.imageMemoryBarrierCount = nb, di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (R.mr_dsrc) {
        VkImageCopy c = { 0 };
        c.srcSubresource = c.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 2 };
        c.extent = (VkExtent3D){ R.mr_dw, R.mr_dh, 1 };
        vkCmdCopyImage(cmd, R.mr_dsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.mr_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        VkDeviceSize sz = (VkDeviceSize)R.mr_dw * R.mr_dh * 2u * 2u;
        bool ok = true;
        if (sz > R.mr_dstage_cap) {
            vkDeviceWaitIdle(vk->device);
            for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.mr_dstage[f]);
            R.mr_dstage_cap = 0;
            for (int f = 0; f < RE_FRAMES_IN_FLIGHT && ok; f++) ok = gpu_buf_create(vk, &R.mr_dstage[f], sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            if (ok) R.mr_dstage_cap = sz;
        }
        if (ok && R.mr_dstage[slot].mapped) {
            memcpy(R.mr_dstage[slot].mapped, R.mr_dcpu, (size_t)sz);
            VkBufferImageCopy c = { 0 };
            c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 2 };
            c.imageExtent = (VkExtent3D){ R.mr_dw, R.mr_dh, 1 };
            vkCmdCopyBufferToImage(cmd, R.mr_dstage[slot].buffer, R.mr_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        } else
            R.mr_dvalid = false;
    }
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, b[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (nb == 2) {
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, b[1].srcAccessMask = 0;
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b[1].dstAccessMask = 0;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier2(cmd, &di);
}

// The frame's mixed reality numbers (mr.glsl's list) for this eye
static void mr_fill(GpuFrame* fr, const RenderView* view, uint32_t slot, float near_cm) {
    fr->mr_boxes = 0, fr->mr_box_count = 0, fr->mr_flags = 0;
    memset(fr->mr, 0, sizeof fr->mr);
    if (!R.mr_on || view->editor) return;
    const RenderMr* m = &R.mr;
    fr->mr_flags = 1u | (m->background ? 2u : 0u) | (m->sim ? 4u : 0u) | (R.mr_dvalid && !R.mr_dresize ? 8u : 0u) |
                   (m->room_occludes ? 16u : 0u) | (m->shadows ? 32u : 0u) | (m->unpremultiplied ? 64u : 0u);
    if (m->nboxes && R.mr_buf[slot].buffer) fr->mr_boxes = R.mr_buf[slot].addr, fr->mr_box_count = m->nboxes;
    fr->mr[0][0] = 1.0f, fr->mr[0][1] = m->virtual_opacity, fr->mr[0][2] = m->soft_cm, fr->mr[0][3] = m->shadow_strength;
    fr->mr[1][0] = m->pt_opacity, fr->mr[1][1] = (float)m->pt_style, fr->mr[1][2] = m->brightness, fr->mr[1][3] = m->contrast;
    fr->mr[2][0] = m->saturation, fr->mr[2][1] = m->tint[0], fr->mr[2][2] = m->tint[1], fr->mr[2][3] = m->tint[2];
    int e = view->vr_eye == 1 ? 1 : 0;
    for (int k = 0; k < 3; k++) {
        fr->mr[4][k] = m->dpos[e][k], fr->mr[5][k] = m->dfwd[e][k], fr->mr[6][k] = m->dright[e][k], fr->mr[7][k] = m->dup[e][k];
        fr->mr[10][k] = m->lamp[k];
    }
    fr->mr[4][3] = m->depth_near_cm, fr->mr[5][3] = m->depth_far_cm, fr->mr[6][3] = m->dtan[e][0], fr->mr[7][3] = m->dtan[e][1];
    fr->mr[8][0] = m->dtan[e][2], fr->mr[8][1] = m->dtan[e][3], fr->mr[8][2] = (float)e, fr->mr[8][3] = m->depth_flip_y ? 1.0f : 0.0f;
    fr->mr[9][0] = (float)R.mr_dw, fr->mr[9][1] = (float)R.mr_dh, fr->mr[9][2] = near_cm;
}

// A picture of a new size: new images and the descriptors (render_scene's start: before the set is bound in this frame's
// commands; the old images may be read by a frame in flight)
static void ar_resize(VkCtx* vk) {
    bool pic = R.ar_on && (R.ar_w & 0x80000000u), dep = R.ar_depth_on && (R.ar_dw & 0x80000000u);
    if (R.mr_on && R.mr_dresize) { // (Q25b) mixed reality's real depth at a new size
        vkDeviceWaitIdle(vk->device);
        R.mr_dresize = false;
        if (!mr_depth_image(vk, R.mr.depth_w, R.mr.depth_h)) mr_depth_image(vk, 2, 2), R.mr_dvalid = false;
        R.mr_dfresh = true;
        update_descriptors(vk);
    }
    if (!pic && !dep) return;
    vkDeviceWaitIdle(vk->device);
    if (pic && !ar_images(vk, R.ar_w & 0x7FFFFFFFu, R.ar_h)) R.ar_on = false;
    if (dep && !ar_depth_image(vk, R.ar_dw & 0x7FFFFFFFu, R.ar_dh)) R.ar_depth_on = false;
    update_descriptors(vk);
}

// The picture into its images (before the main pass), the floor's triangles into this frame's buffer
static void ar_record(VkCtx* vk, VkCommandBuffer cmd, uint32_t slot) {
    if (!R.ar_on) return;
    if (R.ar_nverts) {
        if (!R.ar_vbuf[slot].buffer) gpu_buf_create(vk, &R.ar_vbuf[slot], sizeof R.ar_tris, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (R.ar_vbuf[slot].mapped) memcpy(R.ar_vbuf[slot].mapped, R.ar_tris, (size_t)R.ar_nverts * 16u);
        else R.ar_nverts = 0;
    }
    if (R.ar_depth_on && R.ar_dfresh && !(R.ar_dw & 0x80000000u)) { // the real depth (~160 x 120 x 2 bytes)
        R.ar_dfresh = false;
        VkDeviceSize dsz = (VkDeviceSize)R.ar_dw * R.ar_dh * 2;
        if (dsz > R.ar_dstage_cap) {
            vkDeviceWaitIdle(vk->device);
            for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.ar_dstage[f]);
            R.ar_dstage_cap = 0;
            for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++)
                if (!gpu_buf_create(vk, &R.ar_dstage[f], dsz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) { R.ar_depth_on = false; goto picture; }
            R.ar_dstage_cap = dsz;
        }
        memcpy(R.ar_dstage[slot].mapped, R.ar_dpic, (size_t)dsz);
        gpu_transition(cmd, &R.ar_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        VkBufferImageCopy dc = { 0 };
        dc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, dc.imageExtent = (VkExtent3D){ R.ar_dw, R.ar_dh, 1 };
        vkCmdCopyBufferToImage(cmd, R.ar_dstage[slot].buffer, R.ar_depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dc);
        gpu_transition(cmd, &R.ar_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
picture:
    if (!R.ar_fresh || (R.ar_w & 0x80000000u)) return;
    R.ar_fresh = false; // (sent once: the images keep it)
    uint32_t w = R.ar_w, h = R.ar_h;
    VkDeviceSize ysz = (VkDeviceSize)w * h, uvsz = (VkDeviceSize)(w / 2) * (h / 2) * 2;
    if (ysz + uvsz > R.ar_stage_cap) {
        vkDeviceWaitIdle(vk->device);
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.ar_stage[f]);
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++)
            if (!gpu_buf_create(vk, &R.ar_stage[f], ysz + uvsz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) { R.ar_on = false; return; }
        R.ar_stage_cap = ysz + uvsz;
    }
    memcpy(R.ar_stage[slot].mapped, R.ar_pic, (size_t)(ysz + uvsz));
    for (int i = 0; i < 2; i++)
        gpu_transition(cmd, &R.ar_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy c[2] = { { 0 } };
    c[0].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, c[0].imageExtent = (VkExtent3D){ w, h, 1 };
    c[1].bufferOffset = ysz;
    c[1].imageSubresource = c[0].imageSubresource, c[1].imageExtent = (VkExtent3D){ w / 2, h / 2, 1 };
    vkCmdCopyBufferToImage(cmd, R.ar_stage[slot].buffer, R.ar_img[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c[0]);
    vkCmdCopyBufferToImage(cmd, R.ar_stage[slot].buffer, R.ar_img[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c[1]);
    for (int i = 0; i < 2; i++)
        gpu_transition(cmd, &R.ar_img[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
}

bool render_init(VkCtx* vk) {
    memset(&R, 0, sizeof R);
    R.dev = vk->device;
    render_preview_init(vk); // (its pipelines made on the first preview)
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(vk->gpu, &feats);
    R.wire_ok = feats.fillModeNonSolid;
    { // (2026-11-24, item 4) the skins' room by the GPU's own memory (a phone and a small card keep the 6 M)
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(vk->gpu, &mp);
        VkDeviceSize vram = 0;
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > vram) vram = mp.memoryHeaps[h].size;
        g_skin_room_cap = vram >= (7ull << 30) + (512ull << 20) ? 16u << 20 : vram >= (5ull << 30) + (512ull << 20) ? 10u << 20 : 6u << 20;
    }
    R.indep_blend = feats.independentBlend;
    R.split_ok = R.indep_blend && vk->unused_attachments && vk->ray_query; // (gi_available: after gi_init)
    R.den_ok = R.split_ok && !getenv("REALENGINE_NO_DENOISER_TARGETS"); // (the main pass's 4 targets; the env: a measure of their cost)
    const char* oe = getenv("REALENGINE_OCCLUSION"); // 0 / 1: forced (the measures); else automatic
    R.oa_forced = oe ? atoi(oe) != 0 : -1;
    R.oa_next = re_time_seconds() + 0.5;
    VkSampleCountFlags sc = vk->gpu_props.limits.framebufferColorSampleCounts & vk->gpu_props.limits.framebufferDepthSampleCounts;
    R.max_samples = (sc & VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT : (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
                  : (sc & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;

    if (!build_meshes(vk)) return false;
    // the frame's data: written by the CPU per frame slot (host memory), copied each frame into device memory where
    // every shader reads it (the lights per pixel, the planes per instance: not over PCIe)
    if (!gpu_buf_create(vk, &R.frame_dev, sizeof(GpuFrame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        return false;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        if (!gpu_buf_create(vk, &R.frame[f], sizeof(GpuFrame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
        if (!gpu_buf_create(vk, &R.cull_read[f], sizeof(uint32_t) * 3 * NCMD, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    }

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(vk->device, &si, NULL, &R.linear_sampler), "sampler");
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vk_check(vkCreateSampler(vk->device, &si, NULL, &R.repeat_sampler), "repeat sampler"); // (the clouds' 3D noise)
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vk_check(vkCreateSampler(vk->device, &si, NULL, &R.mip_sampler), "mip sampler");
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 0.0f;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    vk_check(vkCreateSampler(vk->device, &si, NULL, &R.shadow_sampler), "shadow sampler");

    VkDescriptorSetLayoutBinding lb[SET_BINDINGS];
    for (int i = 0; i < SET_BINDINGS; i++)
        lb[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, g_binding_types[i], 1,
                                                VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = SET_BINDINGS;
    li.pBindings = lb;
    vk_check(vkCreateDescriptorSetLayout(vk->device, &li, NULL, &R.set_layout), "set layout");
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0 } };
    for (int i = 0; i < SET_BINDINGS; i++) ps[g_binding_types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE].descriptorCount++; // (the table's own counts)
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    vk_check(vkCreateDescriptorPool(vk->device, &pi, NULL, &R.pool), "descriptor pool");
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &R.set_layout;
    vk_check(vkAllocateDescriptorSets(vk->device, &ai, &R.set), "descriptor set");
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
    // set 1: the materials' textures (material.c: bindless, the baked ones and the sources)
    if (!mat_init(vk, "Content")) return false;
    VkDescriptorSetLayout sets[2] = { R.set_layout, mat_set_layout() };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 2;
    pl.pSetLayouts = sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vk_check(vkCreatePipelineLayout(vk->device, &pl, NULL, &R.layout), "pipeline layout");

    if (!vsm_init(vk)) return false;
    if (!lsm_init(vk)) return false;
    { // (off until a view asks: a 1x1 stand-in, the descriptor valid)
        LsmSettings ls0 = { 0, 2048, 512, 4, 12 };
        bool changed;
        if (!lsm_configure(vk, &ls0, &changed)) return false;
    }
    if (!ar_images(vk, 2, 2) || !ar_depth_image(vk, 2, 2)) return false; // (AR's picture and depth: placeholders until a camera sends them)
    if (!mr_depth_image(vk, 2, 2)) return false; // (Q25b: mixed reality's real depth, a stand-in until a headset sends one)
    {
        VsmSettings vs0 = { 1, 1024, 4, 1, 0.5357f }; // (a first array: render_frame configures it for the view)
        bool changed;
        if (!vsm_configure(vk, &vs0, &changed)) return false;
    }
    build_fixed_pipelines(vk);
    GiMesh gm[RE_MAX_MESHES];
    for (uint32_t s = 0; s < R.nmeshes; s++) gm[s] = (GiMesh){ R.meshes[s].first_index, R.meshes[s].index_count, R.meshes[s].vertex_offset };
    if (!gi_init(vk, R.layout, &R.vertices, R.vertex_count, &R.indices, gm, (int)R.nmeshes)) return false;
    if (!gi_bake_init(vk, R.layout)) return false;
    if (!gi_cache_init(vk, R.layout)) return false;
    if (!weather_init(vk, R.layout)) return false;
    if (!water_init(vk, R.layout)) return false;
    if (!cloth_init(vk, R.layout)) return false;
    if (!groom_init(vk, R.layout)) return false;
    if (!fire_init(vk, R.layout)) return false;
    if (!splat_init(vk, R.layout)) return false;
    if (!atmosphere_init(vk, R.layout)) return false;
    if (!lights_init(vk, R.layout, s_light_grid_comp, sizeof s_light_grid_comp)) return false;

    // The GPU cull: the shapes' bounds, LODs and BLAS; the indirect draws (main and shadow) with their counts and
    // cursors (cull.comp's layout)
    if (!gpu_buf_create(vk, &R.mesh_info, sizeof(GpuMeshInfo) * RE_MAX_MESHES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
        // the commands, counts and cursors (+ the occluded count), then each run's packed non-empty commands and their
        // counts (cull.comp pass 1/4, draw_runs)
        !gpu_buf_create(vk, &R.draws, (VkDeviceSize)NCMD * 3 * (sizeof(VkDrawIndexedIndirectCommand) + 2 * sizeof(uint32_t)) + 16 +
                                          (VkDeviceSize)NCMD * 3 * sizeof(VkDrawIndexedIndirectCommand) + (VkDeviceSize)RE_MAX_MESHES * 3 * 4 + 64,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
        return false;
    upload_mesh_info(vk);
    R.cull_pipe = gpu_compute_pipeline(vk, R.layout, s_cull_comp, sizeof s_cull_comp);
    tess_init(vk, R.layout); // (Q17: without it, the displaced materials are drawn flat)
    farview_init(vk, R.layout); // (Q21: without it, no far shadow and no impostors)
    R.tlas_fill_pipe = gpu_compute_pipeline(vk, R.layout, s_tlas_fill_comp, sizeof s_tlas_fill_comp);
    R.hiz_pipe = gpu_compute_pipeline(vk, R.layout, s_hiz_comp, sizeof s_hiz_comp);
    if (!R.cull_pipe || !R.tlas_fill_pipe || !R.hiz_pipe) return false;
    // the prepass's depth resolve for the Hi-Z: MIN keeps each pixel's farthest sample (conservative)
    VkPhysicalDeviceDepthStencilResolveProperties dr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dr };
    vkGetPhysicalDeviceProperties2(vk->gpu, &p2);
    R.depth_resolve_mode = (dr.supportedDepthResolveModes & VK_RESOLVE_MODE_MIN_BIT) ? VK_RESOLVE_MODE_MIN_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

    // Exposure: 64 bins, the exposure, the pre-exposure, the time, the mean
    R.exposure_pipe = gpu_compute_pipeline(vk, R.layout, s_exposure_comp, sizeof s_exposure_comp);
    R.bloom_pipe = gpu_compute_pipeline(vk, R.layout, s_bloom_comp, sizeof s_bloom_comp);
    R.crowd_pipe = gpu_compute_pipeline(vk, R.layout, s_crowd_comp, sizeof s_crowd_comp);
    R.skin_pipe = gpu_compute_pipeline(vk, R.layout, s_skin_comp, sizeof s_skin_comp);
    if (!particles_gpu_init(vk, R.layout, s_particle_sim_comp, sizeof s_particle_sim_comp, vk->large_points))
        printf("[RealEngine] aviso: as partículas não iniciaram na GPU\n");
    R.split_pipe = gpu_compute_pipeline(vk, R.layout, s_shadow_split_comp, sizeof s_shadow_split_comp);
    R.gtao_pipe = gpu_compute_pipeline(vk, R.layout, s_gtao_comp, sizeof s_gtao_comp); // (none: no GTAO, the rest as before)
    R.sss_pipe = gpu_compute_pipeline(vk, R.layout, s_sss_comp, sizeof s_sss_comp);    // (none: the skin's wrap, as before)
    if (R.den_ok) { // (one pipeline a pass: the specialization leaves each its own code, fewer registers, more pixels in flight)
        VkShaderModule m = gpu_shader(vk, s_denoise_comp, sizeof s_denoise_comp);
        for (uint32_t k = 0; k < 3; k++) {
            VkSpecializationMapEntry me = { 0, 0, 4 };
            VkSpecializationInfo si = { 1, &me, 4, &k };
            VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = m;
            ci.stage.pName = "main";
            ci.stage.pSpecializationInfo = &si;
            ci.layout = R.layout;
            if (vkCreateComputePipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL, &R.den_pipes[k]) != VK_SUCCESS) R.den_pipes[k] = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(vk->device, m, NULL);
        // its rays (den_trace.comp: ray queries, materials for the shadow rays' alpha)
        R.den_trace_pipe = vk->ray_query ? gpu_compute_pipeline(vk, R.layout, s_den_trace_comp, sizeof s_den_trace_comp) : VK_NULL_HANDLE;
        R.den_pipe = R.den_pipes[0] && R.den_pipes[1] && R.den_pipes[2] && R.den_trace_pipe ? R.den_pipes[0] : VK_NULL_HANDLE;
    }
    if (vk->vrs) R.vrs_pipe = gpu_compute_pipeline(vk, R.layout, s_vrs_comp, sizeof s_vrs_comp);
    if (vk->vrs && g_foveation_wanted) R.vrs_fov_pipe = gpu_compute_pipeline(vk, R.layout, s_vrs_fov_comp, sizeof s_vrs_fov_comp); // (Q25)
    R.ups_mv_pipe = gpu_compute_pipeline(vk, R.layout, s_ups_mv_comp, sizeof s_ups_mv_comp);
    R.decal_bin_pipe = gpu_compute_pipeline(vk, R.layout, s_decal_bin_comp, sizeof s_decal_bin_comp);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) // (the decals: DECAL_MAX a frame slot, 112 KB)
        gpu_buf_create(vk, &R.decal_buf[i], sizeof(GpuDecal) * DECAL_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    R.tsr_pipe = gpu_compute_pipeline(vk, R.layout, s_tsr_comp, sizeof s_tsr_comp);
    if (vk->omm) R.omm_pipe = gpu_compute_pipeline(vk, R.layout, s_omm_comp, sizeof s_omm_comp);
    if (!R.exposure_pipe || !R.bloom_pipe || !R.crowd_pipe || !R.split_pipe || !R.skin_pipe) return false;
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &R.crowd_head[i], 64 * CROWD_MAX_RUNS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    if (!gpu_buf_create(vk, &R.exposure, 64 * 4 + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
        return false;
    s_buf_cached = true; // (read by the CPU every frame: cached)
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &R.exposure_read[i], 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true) ||
            !gpu_buf_create(vk, &R.flicker[i], 64 * 32 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &R.ghost[i], 128 * 64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            s_buf_cached = false;
            return false;
        }
    s_buf_cached = false;
    // (a bit per slot: the world's and the crowds' (the cull writes theirs too; only the world's are read back))
    if (!gpu_buf_create(vk, &R.relevance, (RE_ECS_MAX_ENTITIES + CROWD_MAX_INSTANCES) / 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
        return false;
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &R.rel_read[i], RE_ECS_MAX_ENTITIES / 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkCommandBuffer once = gpu_begin_once(vk);
    vkCmdFillBuffer(once, R.exposure.buffer, 0, VK_WHOLE_SIZE, 0);
    float ex[2] = { 1.0f / (1.2f * 32768.0f), 1.0f / (1.2f * 32768.0f) }; // EV100 15 (a sunny day) until measured
    vkCmdUpdateBuffer(once, R.exposure.buffer, 64 * 4, sizeof ex, ex);
    gpu_end_once(vk, once);

    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = TS_PER_FRAME * RE_FRAMES_IN_FLIGHT;
    if (vk->timestamps) vk_check(vkCreateQueryPool(vk->device, &qi, NULL, &R.queries), "query pool");
    { // (Q11) the outline pass: mesh.vert for its ids (made once, its own pipelines; none: no outlines, the rest unchanged)
        VkShaderModule olv = gpu_shader(vk, s_mesh_vert, sizeof s_mesh_vert);
        if (olv) outline_init(vk, R.set_layout, R.layout, olv), vkDestroyShaderModule(vk->device, olv, NULL);
    }
    return true;
}

static void view2_free(VkCtx* vk);
void render_shutdown(VkCtx* vk) {
    if (!R.dev) return;
    vkDeviceWaitIdle(vk->device);
    view2_free(vk);
    render_preview_shutdown(vk);
    outline_shutdown(vk); // (Q11)
    mat_shutdown(vk);
    gi_bake_shutdown(vk);
    gi_cache_shutdown(vk);
    weather_shutdown(vk);
    water_shutdown(vk);
    gi_fallback_shutdown(vk); // (Q27.12)
    cloth_shutdown(vk);
    groom_shutdown(vk);
    fire_shutdown(vk);
    splat_shutdown(vk);
    atmosphere_shutdown(vk);
    lights_shutdown(vk);
    particles_gpu_shutdown(vk);
    skin_reset(vk); // (the skins' BLAS: before gi's device functions go)
    gpu_buf_destroy(vk, &R.morphs);
    free(R.morph_names), R.morph_names = NULL;
    free(R.skins), R.skins = NULL, R.skins_cap = 0;
    free(R.skin_map), free(R.skin_raw), R.skin_map = NULL, R.skin_raw = NULL, R.skin_map_cap = 0;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.skin_pal[f]), gpu_buf_destroy(vk, &R.skin_hdr[f]);
    R.skin_pal_cap = R.skin_hdr_cap = 0;
    if (R.skin_pipe) vkDestroyPipeline(vk->device, R.skin_pipe, NULL), R.skin_pipe = VK_NULL_HANDLE;
    gi_shutdown(vk);
    rt_alpha_job_cancel(vk); // (a job still compiling: waited for)
    ext_job_cancel(vk);
    den_rt_job_wait(vk, false);
    pw_join(vk, false); // (a prewarm never taken: quit from the Project Browser)
    free(PB.it), PB.it = NULL, PB.n = PB.cap = 0;
    destroy_msaa_pipelines(vk);
    tess_shutdown(vk);
    farview_shutdown(vk);
    if (R.dd_pipe) vkDestroyPipeline(vk->device, R.dd_pipe, NULL), R.dd_pipe = VK_NULL_HANDLE;
    if (R.dd_lines) vkDestroyPipeline(vk->device, R.dd_lines, NULL), R.dd_lines = VK_NULL_HANDLE;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.dd_buf[f]), gpu_buf_destroy(vk, &R.dd_tbuf[f]);
    VkPipeline ps[] = { R.shadow_m, R.cloth_shadow, R.shadow_pipe, R.mask_pipe, R.mask_hair_pipe, R.composite, R.exposure_pipe, R.cull_pipe, R.tlas_fill_pipe, R.hiz_pipe,
                        R.velocity_pipe, R.velocity_crowd_pipe, R.velocity_hair_pipe, R.velocity_cloth_pipe, R.bloom_pipe, R.composite_post, R.crowd_pipe, R.split_pipe, R.mesh_tl, R.mesh_rt_tl, R.tsr_pipe, R.omm_pipe, R.gtao_pipe, R.sss_pipe, R.vrs_pipe, R.vrs_fov_pipe, R.ups_mv_pipe, R.decal_bin_pipe, R.den_pipes[0], R.den_pipes[1], R.den_pipes[2], R.den_trace_pipe };
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++)
        if (ps[i]) vkDestroyPipeline(vk->device, ps[i], NULL);
    for (uint32_t i = 0; i < CROWD_MAX_RUNS; i++)
        gpu_buf_destroy(vk, &R.crowd_res[i].templates), gpu_buf_destroy(vk, &R.crowd_res[i].baked), gpu_buf_destroy(vk, &R.crowd_res[i].big);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.crowd_agents[i]), gpu_buf_destroy(vk, &R.crowd_head[i]);
    gpu_buf_destroy(vk, &R.crowd_prev), R.crowd_prev_cap = 0;
    GpuBuf* bufs[] = { &R.mesh_info, &R.draws, &R.scene, &R.vis, &R.visible_main, &R.visible_shadow, &R.tlas_desc, &R.frame_dev,
                       &R.visible_late, &R.vis_prev };
    for (int i = 0; i < 10; i++) gpu_buf_destroy(vk, bufs[i]);
    free(R.slot_state);
    free(R.moved_at);
    free(R.vsm_hash);
    free(R.vsm_box);
    R.vsm_box = NULL;
    free(R.sent_frame), free(R.sent_at);
    R.sent_frame = R.sent_at = NULL, R.sent_cap = 0;
    free(R.vis_cpu);
    free(R.dyn);
    free(R.dyn_inst);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.dyn_desc[f]);
    gpu_buf_destroy(vk, &R.exposure);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.exposure_read[i]);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.flicker[i]), gpu_buf_destroy(vk, &R.ghost[i]);
    gpu_buf_destroy(vk, &R.relevance);
    if (R.object_cost.buffer) gpu_buf_destroy(vk, &R.object_cost);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.rel_read[i]);
    gpu_image_destroy(vk, &R.color_ms);
    gpu_image_destroy(vk, &R.depth_ms);
    gpu_image_destroy(vk, &R.depth_resolve);
    gpu_buf_destroy(vk, &R.hiz);
    R.hiz_have = false; // (the DAGs' first phase reads last frame's: none now)
    gpu_image_destroy(vk, &R.tsr_in), gpu_image_destroy(vk, &R.hist[0]), gpu_image_destroy(vk, &R.hist[1]), gpu_image_destroy(vk, &R.ups_mv);
    den_destroy(vk);
    sss_destroy(vk);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.decal_buf[i]), gpu_buf_destroy(vk, &R.decal_tiles[i]);
    gpu_image_destroy(vk, &R.hdr);
    gpu_image_destroy(vk, &R.split_ms);
    gpu_image_destroy(vk, &R.split);
    gpu_image_destroy(vk, &R.split_sum), gpu_image_destroy(vk, &R.oit_refr);
    gpu_image_destroy(vk, &R.ml_hist[0]), gpu_image_destroy(vk, &R.ml_hist[1]);
    gpu_image_destroy(vk, &R.vrs_img);
    gpu_image_destroy(vk, &R.mask);
    gpu_image_destroy(vk, &R.mask_depth);
    vsm_shutdown(vk);
    lsm_shutdown(vk);
    for (int i = 0; i < 2; i++) gpu_image_destroy(vk, &R.ar_img[i]);
    gpu_image_destroy(vk, &R.ar_depth);
    gpu_image_destroy(vk, &R.mr_depth); // (Q25b)
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.mr_dstage[f]), gpu_buf_destroy(vk, &R.mr_buf[f]);
    free(R.mr_dcpu), R.mr_dcpu = NULL, R.mr_dcpu_cap = 0, R.mr_dstage_cap = 0;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.ar_dstage[f]);
    free(R.ar_dpic), R.ar_dpic = NULL, R.ar_dpic_cap = 0, R.ar_dstage_cap = 0;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &R.ar_stage[f]), gpu_buf_destroy(vk, &R.ar_vbuf[f]);
    free(R.ar_pic), R.ar_pic = NULL, R.ar_pic_cap = 0, R.ar_stage_cap = 0;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
        gpu_buf_destroy(vk, &R.frame[f]);
        gpu_buf_destroy(vk, &R.cull_read[f]);
        gpu_buf_destroy(vk, &R.vis_read[f]);
        gpu_buf_destroy(vk, &R.stage[f]);
    }
    gpu_buf_destroy(vk, &R.vertices);
    gpu_buf_destroy(vk, &R.indices);
    gpu_buf_destroy(vk, &R.clusters), gpu_buf_destroy(vk, &R.cl_work), gpu_buf_destroy(vk, &R.cl_idx), gpu_buf_destroy(vk, &R.cl_perm); for (int f_ = 0; f_ < RE_FRAMES_IN_FLIGHT; f_++) gpu_buf_destroy(vk, &R.cl_read[f_]);
    vkDestroySampler(vk->device, R.linear_sampler, NULL);
    vkDestroySampler(vk->device, R.repeat_sampler, NULL);
    vkDestroySampler(vk->device, R.mip_sampler, NULL);
    gpu_image_destroy(vk, &R.velocity);
    gpu_image_destroy(vk, &R.lut);
    gpu_image_destroy(vk, &R.ui);
    gpu_image_destroy(vk, &R.ssr_col), gpu_image_destroy(vk, &R.ssr_dep), gpu_image_destroy(vk, &R.tsr_dep);
    bloom_destroy(vk);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &R.vel_buf[i]);
    vkDestroySampler(vk->device, R.shadow_sampler, NULL);
    vkDestroyPipelineLayout(vk->device, R.layout, NULL);
    vkDestroyDescriptorPool(vk->device, R.pool, NULL);
    vkDestroyDescriptorSetLayout(vk->device, R.set_layout, NULL);
    if (R.queries) vkDestroyQueryPool(vk->device, R.queries, NULL);
    memset(&R, 0, sizeof R);
}

// --- The frame ---

static void normalize3(float* v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}
static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The contact AO's occluders: the Movable actors nearest the camera, as their oriented bounding box (a sphere mesh
// as its sphere; a plane has no volume)
static void occ_insert(GpuFrame* fr, float* dist, uint32_t* count, const GpuOccluder* o, const float cam[3]) {
    float d[3] = { o->center[0] - cam[0], o->center[1] - cam[1], o->center[2] - cam[2] };
    float dd = dot3(d, d);
    // insertion into the nearest MAX_OCCLUDERS
    uint32_t at = *count < MAX_OCCLUDERS ? *count : MAX_OCCLUDERS;
    while (at > 0 && dist[at - 1] > dd) at--;
    if (at >= MAX_OCCLUDERS) return;
    uint32_t last = *count < MAX_OCCLUDERS ? *count : MAX_OCCLUDERS - 1;
    for (uint32_t j = last; j > at; j--) fr->occluders[j] = fr->occluders[j - 1], dist[j] = dist[j - 1];
    fr->occluders[at] = *o;
    dist[at] = dd;
    if (*count < MAX_OCCLUDERS) (*count)++;
}
static void fill_occluders(GpuFrame* fr, const GpuInstance* inst, uint32_t n, const float cam[3]) {
    float dist[MAX_OCCLUDERS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const GpuInstance* t = &inst[i];
        uint32_t shape = t->ids[1] >> INST_SHAPE_SHIFT;
        if (!(t->ids[1] & INST_ALIVE) || (t->ids[1] & (INST_STATIC | INST_NO_OCCLUDER)) || shape == SHAPE_PLANE) continue;
        const Mesh* me = &R.meshes[shape];
        const float(*m)[4] = t->rows;
        GpuOccluder o;
        float lc[3], le[3];
        for (int k = 0; k < 3; k++) lc[k] = (me->mn[k] + me->mx[k]) * 0.5f, le[k] = (me->mx[k] - me->mn[k]) * 0.5f;
        float* axes[3] = { o.ax, o.ay, o.az };
        for (int r = 0; r < 3; r++) {
            o.center[r] = m[r][3] + m[r][0] * lc[0] + m[r][1] * lc[1] + m[r][2] * lc[2];
            for (int k = 0; k < 3; k++) axes[k][r] = m[r][k] * le[k];
        }
        o.ay[3] = (t->ids[1] & INST_MOVING) ? 1.0f : 0.0f; // moving: the surface cache doesn't see it (gi.glsl contact_gi)
        uint32_t scene_slot = t->ids[0] & (RE_ECS_MAX_ENTITIES - 1); // occluder_light.comp reads its material there
        memcpy(&o.ax[3], &scene_slot, sizeof scene_slot);
        o.center[3] = OCCLUDER_BOX;
        o.entity = t->ids[0];
        float rx = sqrtf(dot3(o.ax, o.ax)), ry = sqrtf(dot3(o.ay, o.ay)), rz = sqrtf(dot3(o.az, o.az));
        // (2026-11-14, "os personagens ainda estão com uma caixa de sombra embaixo deles") a skin's box is its bind pose's
        // (arms out, the skirt's whole span): its exact box AO drew a dark square on the floor under the character, and
        // a white one where the feet sank into the floor (inside the box no face looks at the floor). A skeletal mesh's
        // skin and parts are round: a capsule along their most upright axis, as two spheres (one for a short part), as
        // thick as their thinner side; a soft blob that follows the body, never a square
        if (t->ids[1] & (INST_ROUND_OCCLUDER | INST_SKINNED)) {
            float* ax3[3] = { o.ax, o.ay, o.az };
            float len[3] = { rx, ry, rz };
            int up = 0;
            for (int k = 1; k < 3; k++)
                if (fabsf(ax3[k][2]) / fmaxf(len[k], 1e-3f) > fabsf(ax3[up][2]) / fmaxf(len[up], 1e-3f)) up = k;
            float H = len[up], rr = fminf(len[(up + 1) % 3], len[(up + 2) % 3]) * 0.6f;
            rr = fmaxf(fminf(rr, H), 1.0f);
            float dir[3] = { ax3[up][0] / fmaxf(H, 1e-3f), ax3[up][1] / fmaxf(H, 1e-3f), ax3[up][2] / fmaxf(H, 1e-3f) };
            float off = fmaxf(H - rr, 0.0f) * 0.5f;
            int ns = off > rr * 0.5f ? 2 : 1;
            float c0[3] = { o.center[0], o.center[1], o.center[2] };
            for (int s = 0; s < ns; s++) {
                GpuOccluder q = o;
                float sg = ns == 1 ? 0.0f : (s ? 1.0f : -1.0f);
                for (int r = 0; r < 3; r++) q.center[r] = c0[r] + dir[r] * off * sg;
                q.ax[0] = rr;
                q.center[3] = -rr * 4.0f; // (a sphere: its reach, negative)
                occ_insert(fr, dist, &count, &q, cam);
            }
            continue;
        }
        // Round meshes as a sphere: their box would leave its corners' floor unshaded next to a round base. The mean of
        // the axes (a non-uniform scale: close enough for a soft shadow); a cone's sphere at its centroid, a quarter up.
        if (shape == SHAPE_SPHERE || shape == SHAPE_CYLINDER) {
            o.center[3] = OCCLUDER_SPHERE;
            o.ax[0] = (rx + ry + rz) / 3.0f;
        } else if (shape == SHAPE_CONE) {
            o.center[3] = OCCLUDER_SPHERE;
            for (int r = 0; r < 3; r++) o.center[r] -= o.az[r] * 0.5f;
            o.ax[0] = (rx + ry + rz) / 3.0f * 0.6f;
        }
        o.center[3] = o.center[3] == OCCLUDER_SPHERE ? -o.ax[0] * 4.0f : sqrtf(rx * rx + ry * ry + rz * rz) * 4.0f; // (the reach)
        occ_insert(fr, dist, &count, &o, cam);
    }
    fr->occluder_count = count;
}

// The relevance bit of a scene slot (the last completed frame's cull; unknown: relevant)
static bool rel_bit(uint32_t slot) { return !R.rel_cur || slot >= R.rel_cur_slots || (R.rel_cur[slot >> 5] >> (slot & 31u) & 1u); }

// --- The persistent scene (notas-dev/escala.md) ---

#define SLOT_ALIVE 1u
#define SLOT_DYNAMIC 2u
#define SLOT_UNBUILT 4u
// (2026-11-24, item 4) a dynamic instance still for 120 frames (DORM_FRAMES): kept in the dynamic set's compare, but sent
// without INST_DYNAMIC: in the static TLAS (built again once), out of the dynamic one, its shadow cached with the static
// casters' (the 5000 standing capsules redrew theirs every frame); the first change wakes it (dyn loop)
#define SLOT_DORMANT 8u
#define DORM_FRAMES 120u

// A mesh actor's instance (slot = its entity index); false (and zeroed) when nothing is drawn there. bmn/bmx: its world
// bounds (the shadow box's casters).
// An instance's rows hashed (never 0): a static caster's place, to see it moved (the virtual shadow maps)
static uint32_t rows_hash(const GpuInstance* o) {
    uint32_t h = 2166136261u, p[12];
    memcpy(p, o->rows, sizeof p);
    for (int i = 0; i < 12; i++) h = (h ^ p[i]) * 16777619u;
    return h | 1u;
}

// (2026-11-17, Q4) A glass with Colored Shadow's oriented box (its mesh's bounds through its rows), kept by entity; o NULL:
// none any more. The nearest MAX_GLASS_BOXES to the camera go to the GPU each frame (glass_fill)
static void glass_note(Entity e, const GpuInstance* o, uint32_t mesh, uint32_t slot) {
    uint32_t i = 0;
    while (i < R.nglass && R.glass_e[i] != e) i++;
    if (!o || mesh >= RE_MAX_MESHES) {
        if (i < R.nglass) R.nglass--, R.glass_e[i] = R.glass_e[R.nglass], memcpy(R.glass_box[i], R.glass_box[R.nglass], sizeof R.glass_box[i]);
        return;
    }
    if (i == R.nglass) {
        if (R.nglass >= 128) return;
        R.nglass++;
    }
    R.glass_e[i] = e;
    const Mesh* me = &R.meshes[mesh];
    float(*b)[4] = R.glass_box[i];
    float lc[3], le[3];
    for (int k = 0; k < 3; k++) lc[k] = (me->mn[k] + me->mx[k]) * 0.5f, le[k] = fmaxf((me->mx[k] - me->mn[k]) * 0.5f, 0.05f);
    for (int r = 0; r < 3; r++) {
        b[0][r] = o->rows[r][3] + o->rows[r][0] * lc[0] + o->rows[r][1] * lc[1] + o->rows[r][2] * lc[2];
        for (int k = 0; k < 3; k++) b[1 + k][r] = o->rows[r][k] * le[k];
    }
    memcpy(&b[0][3], &slot, 4);
    b[1][3] = b[2][3] = b[3][3] = 0.0f;
}
static void glass_fill(World* w, GpuFrame* fr, const float cam[3]) {
    float best[MAX_GLASS_BOXES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < R.nglass;) {
        if (!ecs_get(w, R.glass_e[i], COMP_STATIC_MESH)) { // (gone: dropped)
            R.nglass--, R.glass_e[i] = R.glass_e[R.nglass], memcpy(R.glass_box[i], R.glass_box[R.nglass], sizeof R.glass_box[i]);
            continue;
        }
        float dx = R.glass_box[i][0][0] - cam[0], dy = R.glass_box[i][0][1] - cam[1], dz = R.glass_box[i][0][2] - cam[2];
        float d = dx * dx + dy * dy + dz * dz;
        uint32_t at = n;
        while (at > 0 && best[at - 1] > d) at--;
        if (at < MAX_GLASS_BOXES) {
            uint32_t last = n < MAX_GLASS_BOXES ? n : MAX_GLASS_BOXES - 1;
            for (uint32_t j = last; j > at; j--) best[j] = best[j - 1], memcpy(fr->glass[j], fr->glass[j - 1], sizeof fr->glass[j]);
            best[at] = d;
            memcpy(fr->glass[at], R.glass_box[i], sizeof fr->glass[at]);
            if (n < MAX_GLASS_BOXES) n++;
        }
        i++;
    }
    fr->rgi_info[1] = n;
}

static __declspec(thread) bool t_dyn_worker; // (2026-11-25) instance_of called by dyn_one (a worker, every frame)
static bool instance_of(World* w, Entity e, bool editor, GpuInstance* o, float bmn[3], float bmx[3], bool* dynamic,
                        bool* unbuilt) {
    memset(o, 0, sizeof *o);
    *dynamic = *unbuilt = false;
    if (e == ENTITY_NONE) return false;
    uint32_t ol_slot = entity_index(e); // (Q11: marked again below when its outline is on)
    if (R.ol_flag && ol_slot < R.scene_cap) R.ol_flag[ol_slot] = 0;
    bool ol = false;
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (!t || !sm || (editor && (t->flags & TRANSFORM_HIDDEN)) || (t->flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_RENDER))) return false;
    if (ecs_get(w, e, COMP_WATER)) return false; // (a Water Body: drawn as water, water.c; its plane only for picking)
    uint32_t mesh = render_mesh_slot(sm->mesh);
    const Mesh* me = &R.meshes[mesh];
    render_transform_rows(t, o->rows);
    memcpy(o->base_metal, sm->base_color, sizeof(float) * 3);
    o->base_metal[3] = sm->metallic;
    memcpy(o->emissive_rough, sm->emissive, sizeof(float) * 3);
    o->emissive_rough[3] = sm->roughness;
    o->ids[0] = e;
    o->ids[1] = INST_ALIVE | (mesh << INST_SHAPE_SHIFT) | ((sm->cast_shadow & 1u) ? INST_CAST_SHADOW : 0u) |
                ((sm->cast_shadow & CAST_SHADOW_NO_OCCLUDER) ? INST_NO_OCCLUDER : 0u) | ((sm->cast_shadow & CAST_SHADOW_WIND) ? INST_WIND : 0u) | ((sm->cast_shadow & CAST_SHADOW_ROUND_OCCLUDER) ? INST_ROUND_OCCLUDER : 0u) |
                (((sm->light_channels ? sm->light_channels : 1u) ^ 1u) & 7u) << INST_CHAN_SHIFT; // (Q13: its Lighting Channels, channel 0 flipped)
    o->ids[2] = me->first_index;
    o->ids[3] = (uint32_t)me->vertex_offset;
    if (sm->material) { // its material's slot (a Blueprint's dynamic instance when it has one); the GI sees its mean color
        Material* m = mat_find_hash(sm->material);
        if (m && m->slot) {
            uint32_t slot = mat_slot_of(e, m);
            const MatToon* tt = mat_toon_slot(slot); // (Q11) its material's outline
            ol = tt && tt->outline;
            float uv = sm->uv_scale > 0.0f ? sm->uv_scale : 1.0f;
            memcpy(&o->emissive_rough[0], &slot, 4);
            o->emissive_rough[1] = uv;
            { // (2026-11-23) its glow for the GI's rays (before: 0, a material's emission lit nothing)
                float ae[3]; // (2026-11-24: its slot's own, the Emission Intensity applied)
                if (!mat_slot_emission(slot, ae)) memcpy(ae, m->avg_emis, sizeof ae);
                float nits[3] = { ae[0] * 100.0f, ae[1] * 100.0f, ae[2] * 100.0f };
                uint32_t pk = render_emis_pack(nits, lights_emissive_light(w, e, editor, NULL));
                memcpy(&o->emissive_rough[2], &pk, 4);
            }
            o->ids[1] |= INST_MATERIAL;
            memcpy(o->base_metal, m->avg_base, sizeof(float) * 3);
            if (mat_cuts(m) && mesh < RE_MAX_MESHES && !R.mesh_masked[mesh]) R.mesh_masked[mesh] = R.any_masked = R.masked_dirty = true;
            if (mat_tess(m)) { // (Q17: an actor with a displaced material: Nanite Tessellation on)
                R.any_tess = true;
                if (!R.tess_twins) R.tess_twins = R.tess_rebuild = true; // (Q22: the pipelines made again with their tess twins)
            }
            if (mat_cuts(m) && mesh < RE_MAX_MESHES) { // (its micromap's material: one per mesh, else none)
                if (!R.omm_slot[mesh]) R.omm_slot[mesh] = slot + 1u, R.omm_uv[mesh] = uv, R.omm_seen[mesh] = R.frame_counter;
                else if (R.omm_slot[mesh] != slot + 1u || R.omm_uv[mesh] != uv) R.omm_bad[mesh] = true;
            }
            if (mat_sss(m) && !R.any_sss) { // (the skin's screen space scattering from now on: A9.3a, sss.comp)
                R.any_sss = true;
                printf("[RealEngine] Material %s: subsurface (pele): a subsuperficie em tela (A9.3a)\n", m->name);
            }
            if (mat_refract(m) && !R.any_refract) R.any_refract = true; // (2026-11-16) refractive glass: the scene's copy from now on
            if (mat_glass_shadow(m) && !R.any_glass_shadow) R.any_glass_shadow = true; // (Q4) its colored shadow from now on
            glass_note(e, mat_glass_shadow(m) ? o : NULL, mesh, slot); // (Q4: its box for the shadow without rays)
            if (mat_toon(m) && !R.toon_spv) { // (Q11) the RE_EXT pipelines from their RE_TOON twins from now on
                R.toon_spv = R.toon_rebuild = true;
                printf("[RealEngine] Material %s: Toon: the RE_TOON pipelines\n", m->name);
            }
            if (mat_ext(m) && !R.ext_mat) { // (the RE_EXT pipelines from now on)
                R.any_ext = R.ext_mat = true;
                printf("[RealEngine] Material %s: extra lobes, vertex color or detail normal: the RE_EXT pipelines\n", m->name);
            }
            if (mat_ext(m) && mat_cuts(m) && !R.ext_mat_masked) { // (2026-11-10: the masked runs' RE_EXT ones too)
                R.any_ext_masked = R.ext_mat_masked = true;
                printf("[RealEngine] Material %s: Masked / Two Sided with extra lobes: the masked runs' RE_EXT pipelines\n", m->name);
            }
            if (mat_translucent(m)) {
                o->ids[1] = (o->ids[1] | INST_TRANSLUCENT) & ~INST_CAST_SHADOW;
                if (mesh < RE_MAX_MESHES && !R.mesh_translucent[mesh]) R.mesh_translucent[mesh] = R.any_translucent = R.masked_dirty = true;
            }
            // (a mesh's runs are opaque, masked or translucent as a whole: one that also carries a Translucent material is
            // drawn by the OIT pass, which has no RE_EXT: said once, its name. The masked runs have theirs: 2026-11-10)
            // (2026-11-24) its own run of draws from now on (the cull's: MESH_INFO_EXT), the only one with the RE_EXT pipelines
            if (mat_ext(m) && !mat_translucent(m) && mesh < RE_MAX_MESHES && !R.mesh_ext[mesh]) R.mesh_ext[mesh] = R.masked_dirty = true;
            if (mesh < RE_MAX_MESHES && R.mesh_ext[mesh] && !R.mesh_ext_warned[mesh] && R.mesh_translucent[mesh]) {
                R.mesh_ext_warned[mesh] = true;
                printf("[RealEngine] aviso: mesh %s has Translucent materials too: its extra lobes, vertex colors "
                       "and detail normals are not drawn (give those materials another mesh)\n", render_mesh_name(mesh));
            }
        }
    }
    { // (Q11) the actor's own outline over its material's
        uint32_t am = outline_actor_mode(sm->outline);
        if (am == OUTLINE_ACTOR_ON) ol = true;
        else if (am == OUTLINE_ACTOR_OFF) ol = false;
        if (ol && R.ol_flag && ol_slot < R.scene_cap) R.ol_flag[ol_slot] = 1, R.any_outline = true;
    }
    uint32_t mob = t->flags & TRANSFORM_MOBILITY_MASK;
    if (mob == MOBILITY_STATIC) {
        o->ids[1] |= INST_STATIC;
        if (gi_lightmaps_present()) {
            if (gi_lightmap_for(e, t, mesh, o->lightmap_st)) o->ids[1] |= INST_LIGHTMAP;
            else *unbuilt = true;
        }
    }
    *dynamic = mob == MOBILITY_MOVABLE || (ecs_mask(w, e) & (COMP_BIT(COMP_ROTATING) | COMP_BIT(COMP_CLOTH))); // (Q9: a soft body's mesh bends)
    if (*dynamic) o->ids[1] |= INST_DYNAMIC;
    // (Q21) a still instance of an imported mesh, opaque or Masked: an impostor may stand for it far away (farview.c).
    // (2026-11-25, item 4) a rigid skin's too, Movable or not: its rows its bone's (skin_rigid), the cull swaps it for the
    // card only while it sleeps (dormant: INST_DYNAMIC cleared), the Unamed's capsules far away
    bool rigid = mesh < RE_MAX_MESHES && R.mesh_skinned[mesh] && R.mesh_rigid_bone[mesh] >= 0;
    bool far_note = false, far_plant = false, far_allowed = true;
    uint32_t far_key = 0;
    if ((!*dynamic || rigid) && mesh >= SHAPE_COUNT && mesh < RE_MAX_MESHES && mesh != R.imp_card && (!R.mesh_skinned[mesh] || rigid) &&
        (o->ids[1] & (INST_TRANSLUCENT | INST_SKINNED)) == 0u && !ecs_get(w, e, COMP_SKELETAL_MESH)) {
        uint32_t key = 0xFFFFFFFFu;
        if (o->ids[1] & INST_MATERIAL) memcpy(&key, &o->emissive_rough[0], 4), key += 1u;
        // (a Foliage actor's plant: its Impostors flag; off, its mesh keeps its LODs everywhere)
        const CAttach* fa = ecs_get(w, e, COMP_ATTACH);
        const CFoliage* fo = fa && fa->kind == ATTACH_FOLIAGE ? ecs_get(w, (Entity)fa->owner, COMP_FOLIAGE) : NULL;
        far_note = true, far_key = key, far_plant = (o->ids[1] & INST_WIND) != 0u || fo != NULL, far_allowed = !fo || fo->impostors != 0u;
    }
    float lc[3], le[3];
    for (int k = 0; k < 3; k++) lc[k] = (me->mn[k] + me->mx[k]) * 0.5f, le[k] = (me->mx[k] - me->mn[k]) * 0.5f;
    for (int r = 0; r < 3; r++) {
        float c = o->rows[r][3] + o->rows[r][0] * lc[0] + o->rows[r][1] * lc[1] + o->rows[r][2] * lc[2];
        float x = fabsf(o->rows[r][0]) * le[0] + fabsf(o->rows[r][1]) * le[1] + fabsf(o->rows[r][2]) * le[2];
        bmn[r] = c - x;
        bmx[r] = c + x;
    }
    // (with its box: farview.c skips props too small to ever switch; 2026-11-25: not from dyn_one's workers, every frame in
    // parallel: a Movable rigid skin is noted once, by the scene's build)
    if (far_note && !t_dyn_worker) farview_note_instance(mesh, far_key, far_plant, far_allowed, bmn, bmx);
    return true;
}

// Room for cap slots: the scene, the cull's lists and the TLAS's instances (a power of two; the GPU idle first)
static bool scene_reserve(VkCtx* vk, uint32_t count) {
    if (R.scene.buffer && count <= R.scene_cap && R.crowd_alloc >= R.crowd_room) return true;
    uint32_t cap = R.scene_cap > 1024 ? R.scene_cap : 1024;
    while (cap < count) cap *= 2;
    uint32_t all = cap + R.crowd_room; // (the cull's per-slot lists: the world's and the crowds')
    vkDeviceWaitIdle(vk->device);
    GpuBuf* bufs[] = { &R.scene, &R.vis, &R.visible_main, &R.visible_shadow, &R.tlas_desc, &R.visible_late, &R.vis_prev };
    for (int i = 0; i < 7; i++) gpu_buf_destroy(vk, bufs[i]);
    VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferUsageFlags as = vk->ray_query ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    if (!gpu_buf_create(vk, &R.scene, (VkDeviceSize)all * sizeof(GpuInstance), st, false) ||
        !gpu_buf_create(vk, &R.vis, (VkDeviceSize)all * 4, st | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) || // (the Debug Viewer reads it back)
        !gpu_buf_create(vk, &R.visible_main, (VkDeviceSize)all * 4, st, false) ||
        !gpu_buf_create(vk, &R.visible_shadow, (VkDeviceSize)all * 4, st, false) ||
        !gpu_buf_create(vk, &R.visible_late, (VkDeviceSize)all * 4, st, false) ||
        !gpu_buf_create(vk, &R.vis_prev, (VkDeviceSize)all * 4, st, false) ||
        !gpu_buf_create(vk, &R.tlas_desc, (VkDeviceSize)cap * 64, st | as, false))
        return false;
    VkCommandBuffer cmd = gpu_begin_once(vk); // empty slots read as nothing until written
    vkCmdFillBuffer(cmd, R.scene.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, R.vis_prev.buffer, 0, VK_WHOLE_SIZE, 0); // nothing drawn yet: no early list
    gpu_end_once(vk, cmd);
    if (!lights_reserve(vk, cap)) return false; // the lights' slots: the same entities
    uint8_t* ss = realloc(R.slot_state, cap); // a resize, not a frame
    if (!ss) return false;
    R.slot_state = ss;
    uint32_t* ma = realloc(R.moved_at, (size_t)cap * sizeof *ma);
    if (!ma) return false;
    memset(ma + R.scene_cap, 0, (size_t)(cap - R.scene_cap) * sizeof *ma);
    R.moved_at = ma;
    uint32_t* vh = realloc(R.vsm_hash, (size_t)cap * sizeof *vh);
    if (!vh) return false;
    memset(vh + R.scene_cap, 0, (size_t)(cap - R.scene_cap) * sizeof *vh);
    R.vsm_hash = vh;
    float(*vb)[6] = realloc(R.vsm_box, (size_t)cap * sizeof *vb);
    if (!vb) return false;
    R.vsm_box = vb;
    uint8_t* of = realloc(R.ol_flag, cap); // (Q11)
    if (!of) return false;
    memset(of + R.scene_cap, 0, cap - R.scene_cap);
    R.ol_flag = of;
    R.scene_cap = cap;
    R.crowd_alloc = R.crowd_room;
    R.crowd_base = cap;
    return true;
}

// The crowds' data for this frame: each one's parts (templates: the instance without its rows), baked clips and big
// flags uploaded when they changed; every agent into this frame's buffer; the headers crowd.comp reads
static void crowd_prepare(VkCtx* vk, uint32_t slot) {
    const CrowdDraw* d;
    uint32_t n = crowd_draws(&d), total_agents = 0, base = 0;
    R.ncrowd = 0;
    for (uint32_t i = 0; i < n; i++) total_agents += d[i].nagents;
    if (!n || !total_agents) return;
    // the virtual shadow maps: a crowd that moved or animated (its agents changed) redraws its agents' shadows (a box
    // each, merged by vsm.c into a few over its groups); the GPU writes their parts, the CPU only knows the agents
    static uint32_t s_crowd_hash[CROWD_MAX_RUNS];
    for (uint32_t i = 0; i < n && i < CROWD_MAX_RUNS; i++) {
        uint32_t h = 2166136261u;
        const uint32_t* p = (const uint32_t*)d[i].agents;
        for (size_t k = 0, nw = (size_t)d[i].nagents * sizeof(CrowdAgentGpu) / 4; k < nw; k++) h = (h ^ p[k]) * 16777619u;
        if (h == s_crowd_hash[i]) continue;
        s_crowd_hash[i] = h;
        for (uint32_t a = 0; a < d[i].nagents; a++) {
            const CrowdAgentGpu* ag = &d[i].agents[a];
            if ((ag->clips >> 16) & 0xFFu) continue; // (LOD 1-2: no shadows)
            float mn[3] = { ag->pos[0] - 120.0f, ag->pos[1] - 120.0f, ag->pos[2] - 20.0f };
            float mx[3] = { ag->pos[0] + 120.0f, ag->pos[1] + 120.0f, ag->pos[2] + 260.0f };
            shadows_dirty_box(mn, mx);
        }
    }
    if (total_agents > R.crowd_agents_cap[slot]) {
        uint32_t c = 1024;
        while (c < total_agents) c *= 2;
        vkDeviceWaitIdle(vk->device); // (a bigger crowd: rare)
        gpu_buf_destroy(vk, &R.crowd_agents[slot]);
        if (!gpu_buf_create(vk, &R.crowd_agents[slot], (VkDeviceSize)c * sizeof(CrowdAgentGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            R.crowd_agents_cap[slot] = 0;
            return;
        }
        R.crowd_agents_cap[slot] = c;
    }
    if (R.crowd_room > R.crowd_prev_cap) { // the parts' rows last frame (crowd.comp; 48 bytes each)
        vkDeviceWaitIdle(vk->device); // (a bigger crowd: rare)
        gpu_buf_destroy(vk, &R.crowd_prev);
        R.crowd_prev_cap = gpu_buf_create(vk, &R.crowd_prev, (VkDeviceSize)R.crowd_room * 48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false) ? R.crowd_room : 0;
        if (!R.crowd_prev_cap) return;
    }
    CrowdAgentGpu* agents = R.crowd_agents[slot].mapped;
    uint32_t at = 0;
    for (uint32_t i = 0; i < n && R.ncrowd < CROWD_MAX_RUNS; i++) {
        const CrowdDraw* c = &d[i];
        uint64_t parts = (uint64_t)c->nagents * c->nparts;
        if (base + parts > R.crowd_room) break; // (over the room: the next frame's rebuild makes more)
        // its resources (by the spawner: kept while it lives)
        int ri = -1;
        for (uint32_t k = 0; k < CROWD_MAX_RUNS && ri < 0; k++)
            if (R.crowd_res[k].e == c->e && R.crowd_res[k].templates.buffer) ri = (int)k;
        for (uint32_t k = 0; k < CROWD_MAX_RUNS && ri < 0; k++)
            if (!R.crowd_res[k].templates.buffer) ri = (int)k, R.crowd_res[k].e = c->e;
        if (ri < 0) break;
        if (R.crowd_res[ri].version != c->version || R.crowd_res[ri].nparts != c->nparts) {
            vkDeviceWaitIdle(vk->device); // (new parts: the crowd changed in the editor)
            gpu_buf_destroy(vk, &R.crowd_res[ri].templates), gpu_buf_destroy(vk, &R.crowd_res[ri].baked), gpu_buf_destroy(vk, &R.crowd_res[ri].big);
            size_t bsz = sizeof(float) * 12 * c->nparts * CROWD_FRAMES * 3;
            if (!gpu_buf_create(vk, &R.crowd_res[ri].templates, sizeof(GpuInstance) * c->nparts, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !gpu_buf_create(vk, &R.crowd_res[ri].baked, bsz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !gpu_buf_create(vk, &R.crowd_res[ri].big, sizeof(uint32_t) * c->nparts, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
                break;
            GpuInstance* t = R.crowd_res[ri].templates.mapped;
            uint32_t* big = R.crowd_res[ri].big.mapped;
            for (uint32_t p = 0; p < c->nparts; p++) { // (as instance_of makes a mesh's instance, without its rows)
                const SkPart* sp = &c->mesh->parts[p];
                uint32_t mesh = render_mesh_slot(sp->mesh);
                const Mesh* me = &R.meshes[mesh];
                GpuInstance* o = &t[p];
                memset(o, 0, sizeof *o);
                memcpy(o->base_metal, sp->color, 12), o->base_metal[3] = sp->metallic;
                o->emissive_rough[3] = sp->roughness;
                o->ids[0] = c->e;
                o->ids[1] = (mesh << INST_SHAPE_SHIFT) | INST_CAST_SHADOW;
                o->ids[2] = me->first_index, o->ids[3] = (uint32_t)me->vertex_offset;
                Material* m = sp->material ? mat_find_hash(sp->material) : NULL;
                if (m && m->slot) {
                    uint32_t ms = mat_slot_of(c->e, m);
                    mat_stream_pin(sp->material); // (the agents aren't in the streaming's sweep: full size)
                    memcpy(&o->emissive_rough[0], &ms, 4);
                    o->emissive_rough[1] = 1.0f;
                    float nits[3] = { m->avg_emis[0] * 100.0f, m->avg_emis[1] * 100.0f, m->avg_emis[2] * 100.0f };
                    uint32_t pk = render_emis_pack(nits, false); // (2026-11-23) a crowd's glow: the GI's rays only
                    memcpy(&o->emissive_rough[2], &pk, 4);
                    o->ids[1] |= INST_MATERIAL;
                    memcpy(o->base_metal, m->avg_base, 12);
                    if (mat_cuts(m) && mesh < RE_MAX_MESHES && !R.mesh_masked[mesh]) R.mesh_masked[mesh] = R.any_masked = R.masked_dirty = true;
                    if (mat_cuts(m) && mesh < RE_MAX_MESHES) R.omm_bad[mesh] = true; // (a skeletal part's: no micromap)
                }
                big[p] = c->big[p];
            }
            float* bk = R.crowd_res[ri].baked.mapped;
            for (uint32_t q = 0; q < c->nparts * CROWD_FRAMES * 3; q++) memcpy(&bk[q * 12], &c->baked[q * 12], 48);
            R.crowd_res[ri].version = c->version, R.crowd_res[ri].nparts = c->nparts;
        }
        memcpy(&agents[at], c->agents, sizeof(CrowdAgentGpu) * c->nagents);
        R.crowd_head_cpu[R.ncrowd].agents = R.crowd_agents[slot].addr + (VkDeviceSize)at * sizeof(CrowdAgentGpu);
        R.crowd_head_cpu[R.ncrowd].templates = R.crowd_res[ri].templates.addr;
        R.crowd_head_cpu[R.ncrowd].baked = R.crowd_res[ri].baked.addr;
        R.crowd_head_cpu[R.ncrowd].big = R.crowd_res[ri].big.addr;
        R.crowd_head_cpu[R.ncrowd].base = R.crowd_base + base;
        R.crowd_head_cpu[R.ncrowd].nagents = c->nagents, R.crowd_head_cpu[R.ncrowd].nparts = c->nparts;
        R.crowd_head_cpu[R.ncrowd].frames = CROWD_FRAMES;
        R.crowd_head_cpu[R.ncrowd].prev = R.crowd_prev.addr, R.crowd_head_cpu[R.ncrowd].pbase = R.crowd_base;
        memcpy((uint8_t*)R.crowd_head[slot].mapped + 64 * R.ncrowd, &R.crowd_head_cpu[R.ncrowd], sizeof R.crowd_head_cpu[0]);
        R.ncrowd++;
        at += c->nagents, base += (uint32_t)parts;
    }
    R.crowd_count = base;
    // (the spawners gone: their resources freed)
    for (uint32_t k = 0; k < CROWD_MAX_RUNS; k++) {
        if (!R.crowd_res[k].templates.buffer) continue;
        bool live = false;
        for (uint32_t i = 0; i < n && !live; i++) live = d[i].e == R.crowd_res[k].e;
        if (live) continue;
        vkDeviceWaitIdle(vk->device);
        gpu_buf_destroy(vk, &R.crowd_res[k].templates), gpu_buf_destroy(vk, &R.crowd_res[k].baked), gpu_buf_destroy(vk, &R.crowd_res[k].big);
        memset(&R.crowd_res[k], 0, sizeof R.crowd_res[k]);
    }
}

// The slots the cull goes over: the world's, then (from the world's capacity) the crowds'
static uint32_t cull_count(void) { return R.crowd_count ? R.crowd_base + R.crowd_count : R.scene_count; }

static void dyn_add(uint32_t slot) {
    if (R.ndyn == R.dyn_cap) {
        uint32_t c = R.dyn_cap ? R.dyn_cap * 2 : 256;
        uint32_t* d = realloc(R.dyn, sizeof(uint32_t) * c); // grows with the dynamic set, rarely
        if (!d) return;
        R.dyn = d;
        GpuInstance* di = realloc(R.dyn_inst, sizeof(GpuInstance) * c);
        if (!di) return;
        R.dyn_inst = di;
        R.dyn_cap = c;
    }
    memset(&R.dyn_inst[R.ndyn], 0, sizeof(GpuInstance)); // never sent: differs from any instance
    R.dyn[R.ndyn++] = slot;
    R.slot_state[slot] |= SLOT_DYNAMIC;
}

// Everything sent again (a level loaded, a snapshot restored, more changes than the log keeps): through a staging
// buffer made for it, a pause the size of the scene (~100 MB for a million actors)
static bool scene_rebuild(VkCtx* vk, World* w, const RenderView* view) {
    uint32_t count = w->slot_count;
    if (!scene_reserve(vk, count)) return false;
    GpuBuf staging;
    VkDeviceSize bytes = (VkDeviceSize)count * sizeof(GpuInstance);
    if (!gpu_buf_create(vk, &staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    GpuInstance* st = staging.mapped;
    memset(st, 0, (size_t)bytes);
    memset(R.slot_state, 0, R.scene_cap);
    R.ndyn = 0;
    R.unbuilt = 0;
    memset(R.vsm_hash, 0, (size_t)R.scene_cap * sizeof *R.vsm_hash);
    shadows_invalidate(); // (every level drawn again)
    for (int k = 0; k < 3; k++) R.static_mn[k] = 1e30f, R.static_mx[k] = -1e30f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            uint32_t slot = entity_index(ents[i]);
            float bmn[3], bmx[3];
            bool dynamic, unbuilt;
            if (!instance_of(w, ents[i], view->editor, &st[slot], bmn, bmx, &dynamic, &unbuilt)) continue;
            R.slot_state[slot] |= SLOT_ALIVE | (unbuilt ? SLOT_UNBUILT : 0u);
            R.unbuilt += unbuilt;
            if (dynamic) dyn_add(slot);
            else if (st[slot].ids[1] & INST_CAST_SHADOW)
            {
                R.vsm_hash[slot] = rows_hash(&st[slot]);
                memcpy(R.vsm_box[slot], bmn, 12), memcpy(R.vsm_box[slot] + 3, bmx, 12);
                for (int k = 0; k < 3; k++) R.static_mn[k] = fminf(R.static_mn[k], bmn[k]), R.static_mx[k] = fmaxf(R.static_mx[k], bmx[k]);
            }
        }
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy region = { 0, 0, bytes };
    if (bytes) vkCmdCopyBuffer(cmd, staging.buffer, R.scene.buffer, 1, &region);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &staging);
    R.scene_count = count;
    if (!lights_rebuild(vk, w, view->editor)) return false;
    R.seen_revision = w->revision;
    R.seen_changes = w->changes;
    R.scene_editor = view->editor;
    R.scene_valid = true;
    R.tlas_work = GI_TLAS_BUILD;
    R.scene_version++;
    return true;
}

static void dyn_remove(uint32_t s) {
    for (uint32_t j = 0; j < R.ndyn; j++)
        if (R.dyn[j] == s) {
            R.dyn[j] = R.dyn[--R.ndyn];
            R.dyn_inst[j] = R.dyn_inst[R.ndyn];
            break;
        }
    R.slot_state[s] &= ~(SLOT_DYNAMIC | SLOT_DORMANT);
}

// same: as last sent (nothing to do); skin: a still skin's (skin_still: 1 its live skin, 2 at its bind)
typedef struct DynOut { GpuInstance o; float mn[3], mx[3]; bool alive, same; uint8_t skin; } DynOut;
typedef struct DynJob { World* w; bool editor; DynOut* out; } DynJob;

// An instance's world box from its rows and mesh (the virtual shadow maps' dirty boxes: where it was)
static void rows_bounds(const GpuInstance* o, float bmn[3], float bmx[3]) {
    uint32_t shape = o->ids[1] >> INST_SHAPE_SHIFT;
    if (shape >= R.nmeshes) {
        for (int k = 0; k < 3; k++) bmn[k] = 1e30f, bmx[k] = -1e30f;
        return;
    }
    const Mesh* me = &R.meshes[shape];
    float lc[3], le[3];
    for (int k = 0; k < 3; k++) lc[k] = (me->mn[k] + me->mx[k]) * 0.5f, le[k] = (me->mx[k] - me->mn[k]) * 0.5f;
    for (int r = 0; r < 3; r++) {
        float c = o->rows[r][3] + o->rows[r][0] * lc[0] + o->rows[r][1] * lc[1] + o->rows[r][2] * lc[2];
        float x = fabsf(o->rows[r][0]) * le[0] + fabsf(o->rows[r][1]) * le[1] + fabsf(o->rows[r][2]) * le[2];
        bmn[r] = c - x;
        bmx[r] = c + x;
    }
}

static void dyn_one(DynJob* j, uint32_t i) {
    bool dynamic, unbuilt;
    DynOut* d = &j->out[i];
    Entity e = ecs_entity_at(j->w, R.dyn[i]);
    t_dyn_worker = true; // (instance_of: no impostor notes from the workers)
    d->alive = instance_of(j->w, e, j->editor, &d->o, d->mn, d->mx, &dynamic, &unbuilt);
    t_dyn_worker = false;
    // (2026-11-19, the Q8 leftover) compared here, in parallel: 20 thousand bodies at rest were 20 thousand compares on
    // the render's thread every frame. Same as last sent (its moving bit as the frame gives it), not a skin, a Spline
    // Mesh or a soft body (bent each frame): the serial loop only counts its bounds
    d->same = false, d->skin = 0;
    if (!d->alive) return;
    uint32_t mesh = d->o.ids[1] >> INST_SHAPE_SHIFT, s = R.dyn[i];
    GpuInstance q = d->o;
    // (2026-11-24, the Unamed scene) a bone skin whose pose didn't change (or drawn at its bind) is compared as the rest:
    // 2080 still capsules went through skin_bind every frame on the render's thread
    if (R.skin_jobs && mesh < RE_MAX_MESHES && R.mesh_skinned[mesh]) {
        int k = skin_still(j->w, e, s, mesh, &q);
        if (!k) return;
        d->skin = (uint8_t)k;
    } else if (R.skin_jobs && mesh < RE_MAX_MESHES && (ecs_mask(j->w, e) & (COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_CLOTH))))
        return;
    if (memcmp(q.rows, R.dyn_inst[i].rows, sizeof q.rows) != 0) return;
    if (R.moved_at[s] && R.frame_no - R.moved_at[s] < 60u) q.ids[1] |= INST_MOVING;
    if (R.slot_state[s] & SLOT_DORMANT) q.ids[1] &= ~INST_DYNAMIC; // (2026-11-24: sent without it while dormant)
    d->same = memcmp(&q, &R.dyn_inst[i], sizeof q) == 0;
}

static void dyn_job(void* arg, int index) {
    DynJob* j = arg;
    uint32_t a = (uint32_t)index * 1024u, b = a + 1024u < R.ndyn ? a + 1024u : R.ndyn;
    for (uint32_t i = a; i < b; i++) dyn_one(j, i);
}

// This frame's changes into the frame slot's staging buffer: the change log since last frame, the selection (edited
// in place by the gizmo and Details: sent when it differs from what was sent), the dynamic set (each instance compared
// with its last copy: one that didn't move costs nothing). The copies and the TLAS work are recorded by scene_record.
// out_dyn: the dynamic set's instances (the contact AO's occluders); dmn/dmx: their casters' bounds.
// A color temperature's white (Kelvin, a fit of the black body: linear rgb, max 1)
static void kelvin_rgb(float t, float c[3]) {
    t = fminf(fmaxf(t, 1000.0f), 40000.0f) / 100.0f;
    float r = t <= 66.0f ? 1.0f : 1.292936f * powf(t - 60.0f, -0.1332047592f);
    float g = t <= 66.0f ? 0.390081579f * logf(t) - 0.631841444f : 1.129890861f * powf(t - 60.0f, -0.0755148492f);
    float b = t >= 66.0f ? 1.0f : t <= 19.0f ? 0.0f : 0.543206789f * logf(t - 10.0f) - 1.196254089f;
    c[0] = powf(fminf(fmaxf(r, 0.0f), 1.0f), 2.2f);
    c[1] = powf(fminf(fmaxf(g, 0.0f), 1.0f), 2.2f);
    c[2] = powf(fminf(fmaxf(b, 0.0f), 1.0f), 2.2f);
}

// The Post Process Volumes' blend at the camera (RenderView.post: post.c) as the frame's pp rows (their layout:
// shaders/composite.frag, bloom.comp); without one (a preview) the defaults with no motion blur
// The render style (render.h): the project's, or the running override
static int g_style_override = -1;
static const char* const g_style_names[RENDER_STYLE_COUNT] = { "Stylized", "Realistic", "Photorealistic", "Video-realistic" };
const char* render_style_name(int s) { return s >= 0 && s < RENDER_STYLE_COUNT ? g_style_names[s] : ""; }
static float g_fade[4]; // (B9) rgb, amount
void render_set_fade(float amount, const float color[3]) {
    g_fade[0] = color ? color[0] : 0.0f, g_fade[1] = color ? color[1] : 0.0f, g_fade[2] = color ? color[2] : 0.0f;
    g_fade[3] = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
}
void render_set_style_override(int s) { g_style_override = s >= 0 && s < RENDER_STYLE_COUNT ? s : -1; }
// The upscaler (upscale.h UPS_*): the project's (Project Settings > Rendering), or the running override (--upscaler)
static int g_ups_override = -1;
void render_set_upscaler_override(int k) { g_ups_override = k >= 0 && k < UPS_COUNT ? k : -1; }
int render_upscaler_active(void) { return R.tsr_on ? R.ups_kind : -1; }
int render_style(void) {
    if (g_style_override >= 0) return g_style_override;
    int s = project()->render_style;
    return s >= 0 && s < RENDER_STYLE_COUNT ? s : RENDER_STYLE_REALISTIC;
}

static void fill_post(GpuFrame* fr, const RenderView* view, const float vp[16], uint32_t vw, uint32_t vh, float near_cm, bool pt) {
    PostSettings def;
    const PostSettings* s = view->post;
    if (!s) {
        post_defaults(&def);
        def.v[PP_MB_AMOUNT] = 0.0f;
        s = &def;
    }
    const float* v = s->v;
    // white balance: the scene lit at Temperature looks white (a lower one: the image bluer), the tint green-magenta
    float w65[3], wt[3], wb[3];
    kelvin_rgb(6500.0f, w65);
    kelvin_rgb(v[PP_TEMPERATURE], wt);
    for (int k = 0; k < 3; k++) wb[k] = w65[k] / fmaxf(wt[k], 0.02f);
    wb[1] *= 1.0f - 0.3f * v[PP_TINT];
    wb[0] *= 1.0f + 0.15f * v[PP_TINT];
    wb[2] *= 1.0f + 0.15f * v[PP_TINT];
    float lum = 0.2126f * wb[0] + 0.7152f * wb[1] + 0.0722f * wb[2];
    for (int k = 0; k < 3; k++) wb[k] /= fmaxf(lum, 1e-4f);
    // motion blur: the amount as the shutter at Target FPS (the blur looks the same at any frame rate), capped at a
    // share of the screen; the camera's own motion only when the camera wants it (and with last frame's view)
    double now = re_time_seconds();
    float dt = R.post_time > 0.0 ? (float)(now - R.post_time) : 0.0f;
    if (view->post) R.post_time = now;
    float amount = pt ? 0.0f : v[PP_MB_AMOUNT];
    if (v[PP_MB_TARGET_FPS] > 0.0f && dt > 1e-5f && dt < 0.25f) amount *= fminf(1.0f / (dt * v[PP_MB_TARGET_FPS]), 30.0f);
    bool cam = view->post && view->camera_blur && R.have_prev_vp && R.prev_w == vw && R.prev_h == vh;
    float px_scale = (float)vh / 1080.0f; // (the bokeh's size given at 1080p)
    bool dof = !pt && v[PP_DOF_FOCAL_DISTANCE] > 0.0f && v[PP_DOF_MAX_BLUR] * px_scale > 0.5f;
    // the blur's growth (composite.frag coc_of: min(|z - focus| / z x k, Max Bokeh Size)): the post's simple model, 8 /
    // f-stop of the size; a Cine Camera's lens (A9.4): the circle of confusion f^2 / (N (S - f)) on its sensor, in pixels
    float coc_k = 8.0f * v[PP_DOF_MAX_BLUR] * px_scale / fmaxf(v[PP_DOF_FSTOP], 0.5f);
    if (view->dof_focal_mm > 0.0f && view->dof_sensor_mm > 0.0f) {
        float f = view->dof_focal_mm, S = fmaxf(v[PP_DOF_FOCAL_DISTANCE] * 10.0f, f * 1.01f), N = fmaxf(v[PP_DOF_FSTOP], 0.5f);
        coc_k = 0.5f * f * f / (N * (S - f)) / view->dof_sensor_mm * (float)vw; // (the circle's diameter on the sensor: its radius in pixels)
    }
    // (a camera whose streak is under 1.5 px everywhere: nothing to blur. Probed at the center and the
    // corners, near and far: last frame's view of this frame's points)
    if (cam && !pt && amount > 0.0f) {
        float th = tanf(view->fov_deg * 0.5f * DEG2RAD), aspect = (float)vw / (float)vh, streak = 0.0f;
        static const float probe[5][2] = { { 0.5f, 0.5f }, { 0.05f, 0.05f }, { 0.95f, 0.05f }, { 0.05f, 0.95f }, { 0.95f, 0.95f } };
        for (int i = 0; i < 5; i++)
            for (int z = 0; z < 2; z++) {
                float nx = probe[i][0] * 2.0f - 1.0f, ny = probe[i][1] * 2.0f - 1.0f, dist = z ? 3000.0f : 150.0f, p[4] = { 0, 0, 0, 1 };
                for (int k = 0; k < 3; k++)
                    p[k] = view->pos[k] + (view->fwd[k] + nx * th * view->right[k] - ny * th / aspect * view->up[k]) * dist;
                float c[4];
                for (int r = 0; r < 4; r++) c[r] = R.prev_vp[r] * p[0] + R.prev_vp[4 + r] * p[1] + R.prev_vp[8 + r] * p[2] + R.prev_vp[12 + r];
                if (c[3] <= 1e-3f) { streak = 1e9f; continue; } // behind last frame's camera: a big move
                float du = (probe[i][0] - (c[0] / c[3] * 0.5f + 0.5f)) * (float)vw, dv = (probe[i][1] - (c[1] / c[3] * 0.5f + 0.5f)) * (float)vh;
                streak = fmaxf(streak, sqrtf(du * du + dv * dv) * amount);
            }
        cam = streak > 1.5f; // (a streak under 1.5 px doesn't show)
    } else
        cam = false;
    // (TSR: the moved actors' motion for its history; the crowds' parts too, velocity_crowd.vert)
    R.post_vel = !pt && (amount > 0.0f || R.tsr_on) && (R.nvel > 0 || (R.crowd_count > 0 && R.crowd_prev_cap && R.velocity_crowd_pipe) || (groom_moving() && R.velocity_hair_pipe) || (cloth_count() && R.velocity_cloth_pipe));
    R.post_run = dof || cam || R.post_vel || (fr->water[50][1] > 0.5f && !pt) || // (B12h: under water, its fog reads the depth)
                 (render_style() == RENDER_STYLE_STYLIZED && project()->stylized_outline > 0.0f && !pt); // (the outline reads it too)
    R.post_bloom = v[PP_BLOOM_INTENSITY] > 0.0f;
    float rows[8][4] = {
        { v[PP_BLOOM_INTENSITY], v[PP_BLOOM_THRESHOLD], v[PP_VIGNETTE], v[PP_CHROMATIC] },
        { v[PP_SATURATION], v[PP_CONTRAST], v[PP_GAMMA], v[PP_GAIN] },
        { v[PP_OFFSET], wb[0], wb[1], wb[2] },
        { s->color_tint[0], s->color_tint[1], s->color_tint[2], v[PP_GRAIN] },
        { amount, v[PP_MB_MAX] * 0.01f, cam ? 1.0f : 0.0f, v[PP_SHARPEN] },
        { dof ? v[PP_DOF_FOCAL_DISTANCE] : 0.0f, coc_k, v[PP_DOF_MAX_BLUR] * px_scale, near_cm },
        { v[PP_GI_INTENSITY], v[PP_AO_INTENSITY], (float)(R.frame_counter & 1023u), pt ? 0.0f : 1.0f },
        { R.post_run ? 1.0f : 0.0f, R.post_vel ? 1.0f : 0.0f, R.post_bloom ? 1.0f : 0.0f, (float)render_style() }, // (composite.frag: the style)
    };
    memcpy(fr->pp, rows, sizeof rows);
    memcpy(fr->prev_view_proj, cam ? R.prev_vp : vp, sizeof fr->prev_view_proj);
    fr->gi_params[0] *= v[PP_GI_INTENSITY];
    if (view->post) { // the next frame's "last view"
        memcpy(R.prev_vp, vp, sizeof R.prev_vp);
        R.have_prev_vp = true;
        R.prev_w = vw;
        R.prev_h = vh;
    }
}

// The view as the velocity pass's cull sees it: the camera's basis, the half-angles' tangents, pixels per radius/distance
typedef struct VelFrustum {
    float pos[3], fwd[3], right[3], up[3], tx, ty, focal;
} VelFrustum;

static void vel_frustum(const RenderView* view, VelFrustum* vf) {
    memcpy(vf->pos, view->pos, sizeof vf->pos);
    memcpy(vf->fwd, view->fwd, sizeof vf->fwd);
    memcpy(vf->right, view->right, sizeof vf->right);
    memcpy(vf->up, view->up, sizeof vf->up);
    vf->tx = tanf(view->fov_deg * 0.5f * DEG2RAD);
    vf->ty = vf->tx * (view->h > 0.0f ? view->h / fmaxf(view->w, 1.0f) : 1.0f);
    vf->focal = fmaxf(view->w, 1.0f) * 0.5f / vf->tx;
    if (view->ortho_half > 0.0f) // (orthographic, the editor's: no motion blur there; every moved part kept, LOD 0)
        vf->tx = vf->ty = 1.0e6f, vf->focal = 1.0e12f;
}

// A moved part's bounds against the view: in front, inside the sides, over 2 px of radius; its LOD as cull.comp picks it
// (48 px of radius for LOD 0, then x0.4 a level)
static bool vel_visible(const VelFrustum* vf, const float mn[3], const float mx[3], uint32_t shape, uint32_t* lod) {
    float c[3], r = 0.0f, d[3];
    for (int k = 0; k < 3; k++) c[k] = 0.5f * (mn[k] + mx[k]), r += (mx[k] - mn[k]) * (mx[k] - mn[k]), d[k] = c[k] - vf->pos[k];
    r = 0.5f * sqrtf(r);
    float z = d[0] * vf->fwd[0] + d[1] * vf->fwd[1] + d[2] * vf->fwd[2];
    if (z < -r) return false;
    float x = d[0] * vf->right[0] + d[1] * vf->right[1] + d[2] * vf->right[2];
    float y = d[0] * vf->up[0] + d[1] * vf->up[1] + d[2] * vf->up[2];
    // (the sides' planes: |x| <= z tx + r sqrt(1 + tx^2))
    if (fabsf(x) > z * vf->tx + r * sqrtf(1.0f + vf->tx * vf->tx) || fabsf(y) > z * vf->ty + r * sqrtf(1.0f + vf->ty * vf->ty)) return false;
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float px = dist > r ? r / dist * vf->focal : 1e9f;
    if (px < 2.0f) return false;
    uint32_t lods = shape < RE_MAX_MESHES && R.lod_count[shape] ? R.lod_count[shape] : 1, l = 0;
    for (float t = 48.0f; l + 1 < lods && px < t; t *= 0.4f) l++;
    *lod = l;
    return true;
}

// The velocity pass's entries this frame: sorted by mesh and LOD (one instanced draw per run), into the frame slot's buffer
static uint32_t velocity_upload(VkCtx* vk, uint32_t slot, uint32_t* run_shape, uint32_t* run_first, uint32_t* run_count) {
    if (!R.nvel) return 0;
    VkDeviceSize need = (VkDeviceSize)R.nvel * sizeof(VelEntry);
    if (R.vel_buf[slot].size < need) {
        gpu_buf_destroy(vk, &R.vel_buf[slot]); // (the frame that used it is done: vkctx_begin_frame waited)
        if (!gpu_buf_create(vk, &R.vel_buf[slot], need + 1024 * sizeof(VelEntry), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return 0;
    }
    uint32_t count[NCMD] = { 0 }, at[NCMD];
    for (uint32_t i = 0; i < R.nvel; i++) count[R.vel[i].key < NCMD ? R.vel[i].key : 0]++;
    uint32_t nruns = 0, sum = 0;
    for (uint32_t k = 0; k < NCMD; k++) {
        at[k] = sum;
        if (count[k]) run_shape[nruns] = k, run_first[nruns] = sum, run_count[nruns] = count[k], nruns++;
        sum += count[k];
    }
    VelEntry* dst = R.vel_buf[slot].mapped;
    for (uint32_t i = 0; i < R.nvel; i++) dst[at[R.vel[i].key < NCMD ? R.vel[i].key : 0]++] = R.vel[i];
    return nruns;
}

// (A1) an instance's rays' mask for the software sets, as tlas_fill.comp and the dynamic TLAS give it
static uint32_t sw_mask(uint32_t flags, bool still) {
    if (flags & INST_TRANSLUCENT) return 0u;
    uint32_t m = 0x01u | ((flags & INST_CAST_SHADOW) ? 0x02u : 0u) | ((flags & INST_STATIC) ? 0x04u : 0u);
    if (still) m |= 0x08u | ((flags & INST_CAST_SHADOW) ? 0x10u : 0u);
    return m;
}
static bool sw_push(SwInst** a, uint32_t* n, uint32_t* cap, SwInst v) {
    if (*n + 1 > *cap) {
        uint32_t c = *cap ? *cap * 2 : 1024;
        SwInst* na = realloc(*a, sizeof(SwInst) * c);
        if (!na) return false;
        *a = na, *cap = c;
    }
    (*a)[(*n)++] = v;
    return true;
}
// (A1) the static set's instances again (the scene changed): every alive mesh not in the dynamic set
static void sw_static_gather(World* w, bool editor) {
    R.sw_static_n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            GpuInstance o;
            float bmn[3], bmx[3];
            bool dynamic, unbuilt;
            if (!instance_of(w, ents[i], editor, &o, bmn, bmx, &dynamic, &unbuilt) || dynamic || !(o.ids[1] & INST_ALIVE)) continue;
            uint32_t m = sw_mask(o.ids[1], true);
            if (!m) continue;
            SwInst v = { entity_index(ents[i]), m, { bmn[0], bmn[1], bmn[2] }, { bmx[0], bmx[1], bmx[2] } };
            if (!sw_push(&R.sw_static, &R.sw_static_n, &R.sw_static_cap, v)) return;
        }
    }
    R.sw_static_version++;
}

static bool scene_prepare(VkCtx* vk, World* w, const RenderView* view, uint32_t slot, const GpuInstance** out_dyn,
                          uint32_t* out_ndyn, float dmn[3], float dmx[3]) {
    R.nregions = 0;
    uint32_t crowd_need = crowd_instances();
    if (crowd_need > R.crowd_room) {
        uint32_t c = 4096;
        while (c < crowd_need) c *= 2;
        R.crowd_room = c;
        R.scene_valid = false; // (the buffers made again: everything sent again)
    }
    if (!crowd_need) R.crowd_count = 0;
    R.tlas_work = GI_TLAS_KEEP;
    bool full = !R.scene_valid || w->revision != R.seen_revision || w->changes - R.seen_changes > RE_ECS_CHANGE_RING ||
                w->slot_count > R.scene_cap || view->editor != R.scene_editor;
    if (full) {
        if (!scene_rebuild(vk, w, view)) return false;
        R.dyn_changed = true;
    }
    R.scene_count = w->slot_count;
    crowd_prepare(vk, slot); // (the crowds' agents and parts for crowd.comp)
    for (int k = 0; k < 3; k++) dmn[k] = 1e30f, dmx[k] = -1e30f;

    memcpy(R.last_cam, view->pos, sizeof R.last_cam), R.have_cam = true; // (2026-11-24: anim.c's rate by distance)
    // the hot slots: the log's, then the selection and the one before it
    uint32_t nlog = w->changes - R.seen_changes;
    uint32_t* hot = RE_FRAME_ALLOC(uint32_t, nlog + 3);
    if (!hot) return false;
    uint32_t nhot = 0;
    for (uint32_t c = R.seen_changes; c != w->changes; c++) hot[nhot++] = w->changed[c % RE_ECS_CHANGE_RING];
    uint32_t sel_at = nhot;
    if (view->selected != ENTITY_NONE) hot[nhot++] = entity_index(view->selected);
    if (R.last_selected != ENTITY_NONE && R.last_selected != view->selected) hot[nhot++] = entity_index(R.last_selected);
    R.last_selected = view->selected;
    R.seen_changes = w->changes;
    if (!lights_prepare(vk, w, view->editor, slot, hot, sel_at, view->selected)) return false;
    bool rebuild_static = false, static_changed = false;
    // the hot entries may join or leave the dynamic set (a Mobility changed): the static TLAS then changes too
    for (uint32_t i = 0; i < nhot; i++) {
        uint32_t s = hot[i];
        if (s >= R.scene_cap) continue;
        Entity e = ecs_entity_at(w, s);
        const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        bool dyn = t && ecs_get(w, e, COMP_STATIC_MESH) &&
                   ((t->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE || (ecs_mask(w, e) & COMP_BIT(COMP_ROTATING)));
        bool was = (R.slot_state[s] & SLOT_DYNAMIC) != 0;
        if (dyn != was) {
            if (dyn) dyn_add(s);
            else dyn_remove(s);
            rebuild_static = R.dyn_changed = true;
        }
    }
    uint32_t total = R.ndyn + nhot;
    if (total > R.stage_cap) {
        uint32_t c = R.stage_cap ? R.stage_cap : 1024;
        while (c < total) c *= 2;
        vkDeviceWaitIdle(vk->device);
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
            gpu_buf_destroy(vk, &R.stage[f]);
            if (!gpu_buf_create(vk, &R.stage[f], (VkDeviceSize)c * sizeof(GpuInstance), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
                return false;
        }
        R.stage_cap = c;
    }
    GpuInstance* st = R.stage[slot].mapped;
    R.regions = RE_FRAME_ALLOC(VkBufferCopy, total + 1);
    if (!R.regions) return false;
    // (one copy region per run of consecutive slots: an animated character's parts were made together; a slot sent twice
    // this frame, from the log and the dynamic set, has its first copy overwritten: one copy's regions must not overlap)
    if (R.sent_cap < R.scene_cap) {
        uint32_t c = R.scene_cap;
        uint32_t *f = realloc(R.sent_frame, sizeof(uint32_t) * c), *a = realloc(R.sent_at, sizeof(uint32_t) * c);
        if (f) R.sent_frame = f;
        if (a) R.sent_at = a;
        if (!f || !a) return false;
        memset(R.sent_frame + R.sent_cap, 0xFF, sizeof(uint32_t) * (c - R.sent_cap));
        R.sent_cap = c;
    }
    uint32_t nsent = 0;
#define SEND(slot_, inst_)                                                                                                 \
    do {                                                                                                                   \
        if ((slot_) < R.sent_cap && R.sent_frame[slot_] == R.frame_no) {                                                    \
            st[R.sent_at[slot_]] = *(inst_);                                                                               \
            break;                                                                                                         \
        }                                                                                                                  \
        if ((slot_) < R.sent_cap) R.sent_frame[slot_] = R.frame_no, R.sent_at[slot_] = nsent;                             \
        st[nsent] = *(inst_);                                                                                              \
        VkDeviceSize dst_ = (VkDeviceSize)(slot_) * sizeof(GpuInstance), src_ = (VkDeviceSize)nsent * sizeof(GpuInstance); \
        VkBufferCopy* last_ = R.nregions ? &R.regions[R.nregions - 1] : NULL;                                              \
        if (last_ && last_->srcOffset + last_->size == src_ && last_->dstOffset + last_->size == dst_)                     \
            last_->size += sizeof(GpuInstance);                                                                            \
        else                                                                                                               \
            R.regions[R.nregions++] = (VkBufferCopy){ src_, dst_, sizeof(GpuInstance) };                                  \
        nsent++;                                                                                                           \
    } while (0)

    // 1. the dynamic set: sent (and its TLAS rebuilt) only when an instance differs from its last copy. A Movable actor
    // whose transform changed in the last 60 frames is moving (INST_MOVING): the surface cache and the probes leave it out
    // then (the contact GI shades it), its own texels are redone every frame; standing still it is a wall like any other
    R.frame_no++;
    R.nskin_jobs = R.skin_pal_used = R.skin_verts = R.morph_jobs = R.morph_deltas = R.skin_raw_n = R.skin_noroom_n = R.skin_rigid_n = R.dorm_n = 0;
    static int no_dorm = -1; // (2026-11-24: REALENGINE_NO_DORMANT=1: never dormant, the measures' before)
    if (no_dorm < 0) no_dorm = getenv("REALENGINE_NO_DORMANT") != NULL;
    R.skin_jobs = R.nskins || R.ndyn ? RE_FRAME_ALLOC(uint32_t, R.ndyn + 1) : NULL;
    double skin_t0 = re_time_seconds();
    for (int k = 0; k < 3; k++) R.dyn_mn[k] = 1e30f, R.dyn_mx[k] = -1e30f;
    // with motion blur: the moved ones' last rows for the velocity pass
    R.nvel = 0;
    R.vel = view->post && view->post->v[PP_MB_AMOUNT] > 0.0f && R.ndyn ? RE_FRAME_ALLOC(VelEntry, (R.ndyn < VEL_MAX ? R.ndyn : VEL_MAX) + 1) : NULL;
    VelFrustum vf;
    vel_frustum(view, &vf);
    // the instances made in parallel (thousands of animated parts move every frame), compared and sent on this thread
    DynJob dj = { w, view->editor, RE_FRAME_ALLOC(DynOut, R.ndyn + 1) };
    if (!dj.out) return false;
    if (R.ndyn > 2048) re_parallel_for((int)((R.ndyn + 1023u) / 1024u), dyn_job, &dj);
    else
        for (uint32_t i = 0; i < R.ndyn; i++) dyn_one(&dj, i);
    for (uint32_t i = 0; i < R.ndyn; i++) {
        uint32_t s = R.dyn[i];
        float* bmn = dj.out[i].mn;
        float* bmx = dj.out[i].mx;
        if (dj.out[i].same) { // (at rest, as sent: its bounds counted, nothing else)
            uint8_t dorm = R.slot_state[s] & SLOT_DORMANT;
            R.slot_state[s] = (uint8_t)(SLOT_DYNAMIC | SLOT_ALIVE | dorm);
            // (2026-11-24, item 4) still long enough (not a live skin, a cloth, a crowd's): dormant from now on
            if (!dorm && !no_dorm && (R.dyn_inst[i].ids[1] & INST_ALIVE) && !(R.dyn_inst[i].ids[1] & (INST_SKINNED | INST_MOVING)) &&
                R.frame_no - R.moved_at[s] >= DORM_FRAMES && !(ecs_mask(w, ecs_entity_at(w, s)) & COMP_BIT(COMP_CLOTH))) {
                R.dyn_inst[i].ids[1] &= ~INST_DYNAMIC;
                SEND(s, &R.dyn_inst[i]);
                R.slot_state[s] |= SLOT_DORMANT;
                if (R.dyn_inst[i].ids[1] & INST_CAST_SHADOW) shadows_dirty_box(dj.out[i].mn, dj.out[i].mx); // (the static pages: with it now)
                rebuild_static = R.dyn_changed = true;
                R.dorm_slept++;
            }
            if (R.slot_state[s] & SLOT_DORMANT) R.dorm_n++;
            if (dj.out[i].skin == 1u && s < R.skin_map_cap && R.skin_map[s] < R.nskins) R.skins[R.skin_map[s]].seen = R.frame_no; // (kept: skin_sweep)
            else if (dj.out[i].skin == 2u) R.skin_raw_n++, R.skin_noroom_n += R.skin_raw[s].why == SKIN_RAW_ROOM;
            else if (dj.out[i].skin == 3u) R.skin_rigid_n++; // (2026-11-24: a rigid skin at rest)
            for (int k = 0; k < 3; k++) R.dyn_mn[k] = fminf(R.dyn_mn[k], bmn[k]), R.dyn_mx[k] = fmaxf(R.dyn_mx[k], bmx[k]);
            if (R.dyn_inst[i].ids[1] & INST_CAST_SHADOW)
                for (int k = 0; k < 3; k++) dmn[k] = fminf(dmn[k], bmn[k]), dmx[k] = fmaxf(dmx[k], bmx[k]);
            continue;
        }
        GpuInstance o = dj.out[i].o;
        bool alive = dj.out[i].alive;
        if (R.slot_state[s] & SLOT_DORMANT) { // (2026-11-24) it changed: awake (out of the static TLAS, its cached shadow redone)
            rebuild_static = R.dyn_changed = true;
            if (R.dyn_inst[i].ids[1] & INST_CAST_SHADOW) {
                float omn[3], omx[3];
                rows_bounds(&R.dyn_inst[i], omn, omx);
                shadows_dirty_box(omn, omx);
            }
            R.dorm_woke++;
        }
        R.slot_state[s] = (uint8_t)(SLOT_DYNAMIC | (alive ? SLOT_ALIVE : 0u));
        bool posed = false; // a skin bent this frame (its pose changed): it moved for its shadow and the velocity pass
        ClothSoftView sv;
        SplineMeshView spv;
        if (alive && R.skin_jobs && (o.ids[1] >> INST_SHAPE_SHIFT) < RE_MAX_MESHES && R.mesh_skinned[o.ids[1] >> INST_SHAPE_SHIFT]) {
            posed = skin_bind(vk, w, s, ecs_entity_at(w, s), slot, &o, NULL, NULL);
            if (!(o.ids[1] & INST_SKINNED) && !skin_is_rigid(o.ids[1] >> INST_SHAPE_SHIFT)) R.skin_raw_n++; // (drawn at its bind)
        } else if (alive && R.skin_jobs && (o.ids[1] >> INST_SHAPE_SHIFT) < RE_MAX_MESHES && (ecs_mask(w, ecs_entity_at(w, s)) & COMP_BIT(COMP_ATTACH)) &&
                 spline_mesh_view(w, ecs_entity_at(w, s), &spv))
            posed = skin_bind(vk, w, s, ecs_entity_at(w, s), slot, &o, NULL, &spv); // (Q14: a Spline Mesh piece, a Cable's segment)
        else if (alive && R.skin_jobs && (o.ids[1] >> INST_SHAPE_SHIFT) < RE_MAX_MESHES && (ecs_mask(w, ecs_entity_at(w, s)) & COMP_BIT(COMP_CLOTH)) &&
                 cloth_soft_view(ecs_entity_at(w, s), &sv))
            posed = skin_bind(vk, w, s, ecs_entity_at(w, s), slot, &o, &sv, NULL); // (Q9: a soft body's mesh, bent from its lattice)
        if (alive && (posed || memcmp(o.rows, R.dyn_inst[i].rows, sizeof o.rows) != 0)) {
            R.moved_at[s] = R.frame_no;
            if (o.ids[1] & INST_CAST_SHADOW) { // its shadow: where it is and where it was (the virtual shadow maps)
                shadows_dirty_box(bmn, bmx);
                if (R.dyn_inst[i].ids[1] & INST_ALIVE) {
                    float omn[3], omx[3];
                    rows_bounds(&R.dyn_inst[i], omn, omx);
                    shadows_dirty_box(omn, omx);
                }
            }
            // (in the view and over 2 px of radius: a crowd's far or hidden parts aren't drawn again)
            uint32_t lod;
            if (R.vel && (R.dyn_inst[i].ids[1] & INST_ALIVE) && R.nvel < VEL_MAX && vel_visible(&vf, bmn, bmx, o.ids[1] >> INST_SHAPE_SHIFT, &lod)) {
                VelEntry* e = &R.vel[R.nvel++];
                e->slot = s;
                e->key = (o.ids[1] >> INST_SHAPE_SHIFT) * MAX_LODS + lod;
                e->pad[0] = e->pad[1] = 0;
                memcpy(e->rows, R.dyn_inst[i].rows, sizeof e->rows);
            }
        }
        if (alive && R.moved_at[s] && R.frame_no - R.moved_at[s] < 60u) o.ids[1] |= INST_MOVING;
        // started or stopped moving (or appeared, or went away): the cache's rays begin or stop seeing it there, and the
        // texels around keep a long history (its old shadow stayed on the floor as a mark): an event shortens it
        if (((o.ids[1] ^ R.dyn_inst[i].ids[1]) & (INST_MOVING | INST_ALIVE)) && (alive || (R.dyn_inst[i].ids[1] & INST_ALIVE))) {
            float c[3], r = 0.0f;
            if (alive) {
                for (int k = 0; k < 3; k++) c[k] = 0.5f * (bmn[k] + bmx[k]), r += (bmx[k] - bmn[k]) * (bmx[k] - bmn[k]);
            } else { // gone: where it was (its last rows' translation, the size from its basis)
                for (int k = 0; k < 3; k++) {
                    const float* row = R.dyn_inst[i].rows[k];
                    c[k] = row[3];
                    r += 3.0f * (row[0] * row[0] + row[1] * row[1] + row[2] * row[2]) * 10000.0f;
                }
            }
            r = 0.5f * sqrtf(r) + 150.0f; // its contact shadow and bounce reach a little past it
            uint32_t at = R.nevents < 8 ? R.nevents++ : 0;
            for (uint32_t k = 0; k < R.nevents; k++) // reuse the oldest (or an expired) one
                if (R.event_until[k] < R.event_until[at]) at = k;
            memcpy(R.events[at], c, sizeof c);
            R.events[at][3] = r;
            R.event_until[at] = R.frame_no + 2u * gi_cache_cycle_frames() + 2u;
        }
        if (alive)
            for (int k = 0; k < 3; k++) R.dyn_mn[k] = fminf(R.dyn_mn[k], bmn[k]), R.dyn_mx[k] = fmaxf(R.dyn_mx[k], bmx[k]);
        if (alive && (o.ids[1] & INST_CAST_SHADOW))
            for (int k = 0; k < 3; k++) dmn[k] = fminf(dmn[k], bmn[k]), dmx[k] = fmaxf(dmx[k], bmx[k]);
        if ((o.ids[1] ^ R.dyn_inst[i].ids[1]) & (INST_ALIVE | INST_CAST_SHADOW)) { // appeared, went, or its shadow turned on / off
            if (alive && (o.ids[1] & INST_CAST_SHADOW)) shadows_dirty_box(bmn, bmx);
            if ((R.dyn_inst[i].ids[1] & (INST_ALIVE | INST_CAST_SHADOW)) == (INST_ALIVE | INST_CAST_SHADOW)) {
                float omn[3], omx[3];
                rows_bounds(&R.dyn_inst[i], omn, omx);
                shadows_dirty_box(omn, omx);
            }
        }
        if (memcmp(&o, &R.dyn_inst[i], sizeof o) != 0) {
            R.dyn_inst[i] = o;
            SEND(s, &o);
            R.dyn_changed = true;
        }
    }
    skin_room_grow(vk); // (Q14: the skins that found no room above, one reload)
    // 2. the other hot slots: the log's always; the selection when it differs from what was sent for it
    for (uint32_t i = 0; i < nhot; i++) {
        uint32_t s = hot[i];
        if (s >= R.scene_cap || (R.slot_state[s] & SLOT_DYNAMIC)) continue; // sent with the set
        float bmn[3], bmx[3];
        bool dynamic, unbuilt;
        GpuInstance o;
        bool alive = instance_of(w, ecs_entity_at(w, s), view->editor, &o, bmn, bmx, &dynamic, &unbuilt);
        { // a static caster appeared, went or moved: its shadow there and where it was (the virtual shadow maps)
            uint32_t h = alive && (o.ids[1] & INST_CAST_SHADOW) ? rows_hash(&o) : 0u;
            if (h != R.vsm_hash[s]) {
                if (h) shadows_dirty_static(bmn, bmx);
                if (R.vsm_hash[s]) {
                    float omn[3], omx[3];
                    if (s == R.sel_slot) rows_bounds(&R.sel_copy, omn, omx), shadows_dirty_static(omn, omx); // (dragged: its last place)
                    else shadows_dirty_static(R.vsm_box[s], R.vsm_box[s] + 3); // ((Q15) where it was, kept with its hash: a streamed cell
                    // dropped or a PCG's entities destroyed redraw the levels over it, not every level at once)
                }
                R.vsm_hash[s] = h;
                if (h) memcpy(R.vsm_box[s], bmn, 12), memcpy(R.vsm_box[s] + 3, bmx, 12);
            }
        }
        if (i >= sel_at) {
            if (s == R.sel_slot && memcmp(&o, &R.sel_copy, sizeof o) == 0) continue;
            R.sel_copy = o;
            R.sel_slot = s;
        }
        uint8_t was = R.slot_state[s];
        if (alive != ((was & SLOT_ALIVE) != 0)) rebuild_static = true; // an instance appeared or went: not a refit
        else if (alive) static_changed = true; // (an entity without a mesh changed: a camera, a boom, a light: no refit)
        R.unbuilt += (unbuilt ? 1u : 0u) - ((was & SLOT_UNBUILT) ? 1u : 0u);
        R.slot_state[s] = (uint8_t)((alive ? SLOT_ALIVE : 0u) | (unbuilt ? SLOT_UNBUILT : 0u));
        if (alive && (o.ids[1] & INST_CAST_SHADOW)) // a static caster moved: the shadow box grows to hold it
            for (int k = 0; k < 3; k++) R.static_mn[k] = fminf(R.static_mn[k], bmn[k]), R.static_mx[k] = fmaxf(R.static_mx[k], bmx[k]);
        SEND(s, &o);
    }
#undef SEND
    if (R.nskins) skin_sweep(vk);
    if (R.nskin_jobs) R.dyn_changed = true; // (their BLAS refit: the dynamic TLAS built again over them)
    particles_prepare(vk, w, slot); // (the emitters' blocks for this frame)
    R.skin_ms = (re_time_seconds() - skin_t0) * 1000.0;
    *out_dyn = R.dyn_inst;
    *out_ndyn = R.ndyn;
    R.nhot = nsent; // (the instances sent: the path tracer hashes them)
    if (!full) R.tlas_work = rebuild_static ? GI_TLAS_BUILD : static_changed ? GI_TLAS_REFIT : GI_TLAS_KEEP;
    if (gi_sw() && (R.tlas_work != GI_TLAS_KEEP || !R.sw_static_version)) sw_static_gather(w, view->editor); // (A1)
    return true;
}

// The frame's scene work on the GPU: the copies, the TLAS (its instances from the slots, then a refit or a build), the
// cull (counts, the draws' ranges, the visible lists), and a copy of the counts for the stats
static VkDeviceAddress scene_record(VkCtx* vk, VkCommandBuffer cmd, uint32_t slot, Push push) {
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (R.nregions) {
        vkCmdCopyBuffer(cmd, R.stage[slot].buffer, R.scene.buffer, R.nregions, R.regions);
        gpu_barrier(cmd, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
    gpu_mark(cmd, "skin (ossos e BLAS)");
    skin_record(cmd, slot, push); // (the skins bent and their BLAS refit: before the TLAS and the draws)
    gpu_mark(cmd, "tecido (BLAS)");
    cloth_blas_record(cmd);       // (the cloths' BLAS from last frame's vertices: their ray traced shadow)
    gpu_mark(cmd, "particulas");
    // (A9.7c) async compute: the particles' simulation on the compute queue (its frame data read from the host copy: the
    // device one is written further down this command buffer), the frame split before the main pass, which waits for it
    VkCommandBuffer ac = R.async_on && particles_active(slot) ? vkctx_async_begin(vk) : VK_NULL_HANDLE;
    if (ac) {
        Push pa = push;
        pa.frame = R.frame[slot].addr;
        particles_record(ac, slot, &pa, R.layout, true);
        vkctx_async_submit(vk, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        R.async_split = true;
    } else
        particles_record(cmd, slot, &push, R.layout, false); // (the particles moved and culled: their draws' counts)
    gpu_mark(cmd, "cena (TLAS, culling)");
    VkDeviceAddress tlas = 0;
    if (gi_available()) {
        if (R.tlas_work != GI_TLAS_KEEP) {
            Push p = push;
            p.vertices = R.tlas_desc.addr; // tlas_fill.comp: the descriptors
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.tlas_fill_pipe);
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (R.scene_count + 63) / 64, 1, 1);
        }
        tlas = gi_tlas(vk, cmd, 0, R.tlas_desc.addr, R.scene_count, R.tlas_work);
        // the dynamic set's TLAS: its instances written here (a few, from the copies), rebuilt when it changed
        // rebuilt when the set changed, or when its members' relevance did (one came near, one left)
        uint64_t rh = 1469598103934665603ull;
        for (uint32_t i = 0; i < R.ndyn; i++)
            if (rel_bit(R.dyn[i])) rh = (rh ^ R.dyn[i]) * 1099511628211ull;
        if (rh != R.dyn_rel_hash) R.dyn_changed = true;
        uint32_t ncl = cloth_tlas_instances(R.cloth_inst, 64);
        if (ncl) R.dyn_changed = true; // (their BLAS built again: the TLAS over them too)
        if (R.dyn_changed) {
            R.dyn_rel_hash = rh;
            if (R.ndyn + ncl > R.dyn_desc_cap) {
                uint32_t c = R.dyn_desc_cap ? R.dyn_desc_cap : 256;
                while (c < R.ndyn + ncl) c *= 2;
                vkDeviceWaitIdle(vk->device);
                for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
                    gpu_buf_destroy(vk, &R.dyn_desc[f]);
                    gpu_buf_create(vk, &R.dyn_desc[f], sizeof(VkAccelerationStructureInstanceKHR) * c,
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
                }
                R.dyn_desc_cap = c;
            }
            VkAccelerationStructureInstanceKHR* d = R.dyn_desc[slot].mapped;
            uint32_t k = 0;
            uint64_t mh = 1469598103934665603ull; // (its members in order: the same ones as built, only moved: a refit)
            for (uint32_t i = 0; d && i < R.ndyn; i++) {
                const GpuInstance* o = &R.dyn_inst[i];
                if (!(o->ids[1] & INST_ALIVE)) continue;
                if (R.slot_state[R.dyn[i]] & SLOT_DORMANT) continue; // (2026-11-24: dormant: in the static TLAS)
                if (!rel_bit(R.dyn[i])) continue; // can't show up in the view (nor its shadow, nor its light)
                // made on the stack, then copied: the mapped memory is write-combined (the bit fields' read-modify-
                // writes straight into it cost a read from the bus each: 14 ms for 28 000 animated parts)
                VkAccelerationStructureInstanceKHR inst_, *a = &inst_;
                memcpy(&a->transform, o->rows, sizeof a->transform);
                a->instanceCustomIndex = R.dyn[i];
                a->mask = 0x01u | ((o->ids[1] & INST_CAST_SHADOW) ? 0x02u : 0u) | ((o->ids[1] & INST_STATIC) ? 0x04u : 0u);
                if (!(o->ids[1] & INST_MOVING)) // still: the cache's and the probes' rays see it (rt.glsl RT_STILL_ONLY)
                    a->mask |= 0x08u | ((o->ids[1] & INST_CAST_SHADOW) ? 0x10u : 0u);
                if (o->ids[1] & INST_TRANSLUCENT) a->mask = 0x20u; // (no ray sees a Translucent one; Q4: the glass's colored shadow rays only)
                a->instanceShaderBindingTableRecordOffset = 0;
                a->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; // (a Masked mesh's: not opaque, rt.glsl)
                if ((o->ids[1] >> INST_SHAPE_SHIFT) < RE_MAX_MESHES && R.mesh_masked[o->ids[1] >> INST_SHAPE_SHIFT])
                    a->flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
                a->accelerationStructureReference = gi_blas_address((int)(o->ids[1] >> INST_SHAPE_SHIFT));
                if (o->ids[1] & INST_SKINNED) { // (a skin: its own BLAS, bent; none yet: left out)
                    memcpy(&a->accelerationStructureReference, &o->lightmap_st[2], 8);
                    if (!a->accelerationStructureReference) continue;
                }
                memcpy(&d[k++], a, sizeof *a);
                mh = (mh ^ a->instanceCustomIndex) * 1099511628211ull, mh = (mh ^ a->accelerationStructureReference) * 1099511628211ull;
            }
            if (d) memcpy(&d[k], R.cloth_inst, ncl * sizeof *R.cloth_inst), k += ncl; // the cloths (cloth.c: shadow rays only)
            // (2026-11-19, the Q8 leftover) the same members, only moved (20 thousand bodies falling): refitted, its tree
            // kept; built again when the members change or every 30th refit (the tree drifts from where things are)
            bool refit = !ncl && k && mh == R.dyn_members && k == R.dyn_built && R.dyn_refits < 30u;
            R.dyn_refits = refit ? R.dyn_refits + 1u : 0u, R.dyn_members = mh;
            R.tlas_dyn = k ? gi_tlas(vk, cmd, 1, R.dyn_desc[slot].addr, k, refit ? GI_TLAS_REFIT : GI_TLAS_BUILD) : 0;
            R.dyn_built = k;
            R.dyn_changed = false;
        }
    }
    else if (gi_sw()) { // (A1) the software sets: the static one as gathered, the dynamic one every frame
        if (R.sw_static_up[slot] != R.sw_static_version)
            R.sw_static_addr[slot] = swrt_set(vk, 0, slot, R.sw_static, R.sw_static_n), R.sw_static_up[slot] = R.sw_static_version;
        tlas = R.sw_static_addr[slot];
        uint32_t k = 0;
        for (uint32_t i = 0; i < R.ndyn; i++) {
            const GpuInstance* o = &R.dyn_inst[i];
            if (!(o->ids[1] & INST_ALIVE) || (o->ids[1] & INST_SKINNED) || !rel_bit(R.dyn[i])) continue; // (a skin: no BVH of its pose)
            uint32_t m = sw_mask(o->ids[1], !(o->ids[1] & INST_MOVING));
            if (!m) continue;
            SwInst v = { R.dyn[i], m, { 0 }, { 0 } };
            rows_bounds(o, v.mn, v.mx);
            if (sw_push(&R.sw_dyn, &k, &R.sw_dyn_cap, v)) continue;
            break;
        }
        R.tlas_dyn = k ? swrt_set(vk, 1, slot, R.sw_dyn, k) : 0;
    }
    // the crowds: their parts into the scene's instances (after this frame's world updates, before the cull reads them;
    // the frames before done drawing the old ones)
    if (R.ncrowd) {
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.crowd_pipe);
        for (uint32_t i = 0; i < R.ncrowd; i++) {
            Push p = push;
            p.frame = R.crowd_head[slot].addr + 64 * i;
            p.instances = R.scene.addr;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof p, &p);
            uint32_t cnt = R.crowd_head_cpu[i].nagents * R.crowd_head_cpu[i].nparts;
            vkCmdDispatch(cmd, (cnt + 63) / 64, 1, 1);
        }
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    // the cull: counts and cursors zeroed, classify, the early and shadow draws, their lists (the late ones after
    // the prepass: occlusion_record)
    VkDeviceSize counts_at = (VkDeviceSize)R.ncmd * 3 * sizeof(VkDrawIndexedIndirectCommand);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | CS | COPY, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | RW | VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, R.draws.buffer, counts_at, (VkDeviceSize)R.ncmd * 6 * sizeof(uint32_t), 0);
    if (R.cl_work.buffer) { // the clusters' dispatch (x: instances, y: their most clusters / 64, z 1) and indices' cursor
        static const uint32_t h[8] = { 0, 0, 1, 0, 0, 0, 0, 1 }; // (5-7: the DAGs' first phase's dispatch)
        vkCmdUpdateBuffer(cmd, R.cl_work.buffer, 0, sizeof h, h);
    }
    tess_reset(cmd); // (Q17: its cursors and sums)
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.cull_pipe);
    uint32_t groups = (cull_count() + 63) / 64;
    for (uint32_t p = 0; p < 3; p++) {
        push.pass = p;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, p == 1 ? 1 : (groups ? groups : 1), 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | COPY,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    { // the relevance bits, for the CPU (the dynamic TLAS, the systems) once this frame is done
        VkBufferCopy rb = { 0, 0, (VkDeviceSize)((R.scene_count + 63) / 64) * 8 };
        if (rb.size) vkCmdCopyBuffer(cmd, R.relevance.buffer, R.rel_read[slot].buffer, 1, &rb);
        R.rel_slots[slot] = (R.scene_count + 63) / 64 * 64;
    }
    if (!R.occlusion) { // the counts for the stats (with occlusion: after the late list, occlusion_record)
        VkBufferCopy rc = { counts_at, 0, sizeof(uint32_t) * 3 * R.ncmd };
        vkCmdCopyBuffer(cmd, R.draws.buffer, R.cull_read[slot].buffer, 1, &rc);
    }
    gpu_mark(cmd, "luzes (grade)");
    lights_record(cmd, slot, R.layout, R.set, push); // the local lights changed, then their grid around the camera
    return tlas;
}

// After the prepass (the early list's depth): the Hi-Z pyramid, every slot in the view tested against it (cull.comp
// passes 3-5: the late list, next frame's early flags), then a copy of the counts for the stats
static void occlusion_record(VkCommandBuffer cmd, uint32_t slot, Push push, const GpuImage* depth) {
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // the prepass's writes (its store, its resolve: COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE) done before the read
    gpu_transition(cmd, (GpuImage*)depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.hiz_pipe);
    for (uint32_t l = 0, w = (R.width + 1) / 2, h = (R.height + 1) / 2; l < R.hiz_levels; l++, w = (w + 1) / 2, h = (h + 1) / 2) {
        push.pass = l;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.cull_pipe);
    uint32_t groups = (cull_count() + 63) / 64;
    for (uint32_t p = 3; p < 6; p++) {
        push.pass = p;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, p == 4 ? 1 : (groups ? groups : 1), 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | COPY,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy rc = { (VkDeviceSize)R.ncmd * 3 * sizeof(VkDrawIndexedIndirectCommand), 0, sizeof(uint32_t) * 3 * R.ncmd };
    vkCmdCopyBuffer(cmd, R.draws.buffer, R.cull_read[slot].buffer, 1, &rc);
}

// A frustum's planes from a column-major clip matrix (inside: dot(n, p) + d >= 0, normalized); far: the sixth
static void planes_from(const float m[16], float out[6][4], bool far) {
    float r[4][4];
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++) r[i][c] = m[c * 4 + i];
    for (int c = 0; c < 4; c++) {
        out[0][c] = r[3][c] + r[0][c];
        out[1][c] = r[3][c] - r[0][c];
        out[2][c] = r[3][c] + r[1][c];
        out[3][c] = r[3][c] - r[1][c];
        out[4][c] = r[3][c] - r[2][c]; // reversed Z: z <= w is the near side; an ortho box: z <= 1
        out[5][c] = far ? r[2][c] : (c == 3 ? 1.0f : 0.0f); // z >= 0 (none for the infinite view)
    }
    for (int p = 0; p < 6; p++) {
        float l = sqrtf(out[p][0] * out[p][0] + out[p][1] * out[p][1] + out[p][2] * out[p][2]);
        if (l > 0.0f)
            for (int c = 0; c < 4; c++) out[p][c] /= l;
    }
}

// The big meshes' clusters (cull.comp pass 6): the seen ones' indices copied into R.cl_idx, a draw per instance (pass 0
// made them). group 0: the opaque ones' cluster LOD DAGs, before the prepass (they are occluders: the Hi-Z needs them);
// 1: the Masked ones, after the Hi-Z
static void cluster_record(VkCommandBuffer cmd, Push push, uint32_t group) {
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT); // (the cull's pass 0)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.cull_pipe);
    push.pad = group;
    static const uint32_t order[3] = { 6, 8, 7 }; // 6: which are seen, per block; 8: their indices' room; 7: copied in order
    for (uint32_t s = 0; s < 3; s++) {
        push.pass = order[s];
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatchIndirect(cmd, R.cl_work.buffer, group ? 0 : 20);
        if (s < 2) gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// The clusters' draws (cluster_record's; push.pass 6 already pushed): their own index buffer, then the meshes' again.
// pm: the Masked instances' (the first array), po: the DAGs' (the opaque ones', the second), pl: the DAGs' late clusters
// (the third: their second phase); NULL: not these
static uint32_t cluster_draws(VkCommandBuffer cmd, VkPipeline pm, VkPipeline po, VkPipeline pl) {
    if (!pm && !po && !pl) return 0;
    vkCmdBindIndexBuffer(cmd, R.cl_idx.buffer, 0, VK_INDEX_TYPE_UINT32);
    static const VkDeviceSize at[3] = { CL_DRAWS_AT, CL_DRAWS2_AT, CL_DRAWS3_AT };
    VkPipeline ps[3] = { pm, po, pl };
    uint32_t n = 0;
    for (int k = 0; k < 3; k++) {
        if (!ps[k]) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps[k]);
        vkCmdDrawIndexedIndirectCount(cmd, R.cl_work.buffer, at[k], R.cl_work.buffer, 0, CL_MAX_ENTRIES, sizeof(VkDrawIndexedIndirectCommand));
        n++;
    }
    vkCmdBindIndexBuffer(cmd, R.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    return n;
}

static void ts(VkCommandBuffer cmd, uint32_t slot, uint32_t i, VkPipelineStageFlags2 stage) {
    if (R.queries) vkCmdWriteTimestamp2(cmd, stage, R.queries, slot * TS_PER_FRAME + i);
}

// One of the cull's lists of draws (list_off: its first command) in runs of consecutive meshes of one class (one draw
// call per run): p[0] the opaque meshes, p[1] those of Masked / Two Sided materials, p[2] / p[3] the same for the
// imported meshes when given (the shadow terminator's fix; NULL: as the basic shapes). Without such meshes it is the
// single call it always was. Returns the draws issued.
// The draws of a list in runs of meshes of one class (Masked or not, a shape or imported): each run's non-empty commands,
// packed by cull.comp (pass 1/4) with their count: DrawIndexedIndirectCount draws only those (a mesh's LOD commands are
// mostly empty; they cost +10% of the main pass with 89 meshes registered, notas-dev/render.md)
// (2026-11-24) the RE_EXT runs' pipelines (opaque, masked, imported, imported masked; NULL: the plain ones), and the runs
// drawn plain / RE_EXT this frame (the mat_class_split test, the bench's line)
static VkPipeline g_run_x[4];
static uint32_t g_run_count[2];
static uint32_t draw_runs(VkCommandBuffer cmd, VkDeviceSize list_off, VkPipeline opaque, VkPipeline masked, VkPipeline opaque_t,
                          VkPipeline masked_t) {
    const VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    // (opaque NULL: the OIT pass, the Translucent runs alone with 'masked' as their pipeline)
    VkPipeline p4[4] = { opaque, masked ? masked : opaque, opaque_t ? opaque_t : opaque, masked_t ? masked_t : masked ? masked : opaque };
    uint32_t n = R.ncmd / MAX_LODS, list = (uint32_t)(list_off / ((VkDeviceSize)R.ncmd * stride));
    VkDeviceSize cbase = ((VkDeviceSize)R.ncmd * 3 * 20 + ((VkDeviceSize)R.ncmd * 6 + 1) * 4 + 15) & ~(VkDeviceSize)15;
    VkDeviceSize counts = cbase + (VkDeviceSize)R.ncmd * 3 * stride;
    // (2026-11-24) the meshes drawn by an RE_EXT material: their own runs (MESH_INFO_EXT), drawn with g_run_x's pipelines
    // when the pass gives them (the main pass: only those pay the RE_EXT variants' registers and spills)
#define RUN_CLASS(i) ((R.mesh_masked[i] ? 1 : 0) | ((i) >= SHAPE_COUNT ? 2 : 0) | (R.mesh_translucent[i] ? 4 : 0) | (R.mesh_ext[i] ? 8 : 0)) // (cull.comp's runs)
    VkPipeline px4[4] = { g_run_x[0], g_run_x[1] ? g_run_x[1] : g_run_x[0], g_run_x[2] ? g_run_x[2] : g_run_x[0],
                          g_run_x[3] ? g_run_x[3] : g_run_x[1] ? g_run_x[1] : g_run_x[0] };
    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t runs = 0;
    for (uint32_t a = 0; a < n;) {
        int m = RUN_CLASS(a);
        uint32_t b = a + 1;
        while (b < n && RUN_CLASS(b) == m) b++;
        // a Translucent run: only in the OIT pass (opaque NULL there: its pipeline is p4[1]); never in the others
        if (((m & 4) != 0) != (opaque == VK_NULL_HANDLE)) {
            a = b;
            continue;
        }
        int pk = ((m & 1) && masked ? 1 : 0) | ((m & 2) && opaque_t ? 2 : 0);
        bool xr = (m & 8) && !(m & 4) && px4[pk] && opaque;
        VkPipeline p = (m & 4) ? masked : xr ? px4[pk] : p4[pk];
        if (opaque) g_run_count[xr ? 1 : 0]++;
        if (p != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p), bound = p;
        vkCmdDrawIndexedIndirectCount(cmd, R.draws.buffer, cbase + ((VkDeviceSize)list * R.ncmd + (VkDeviceSize)a * MAX_LODS) * stride,
                                      R.draws.buffer, counts + ((VkDeviceSize)list * n + a) * 4, (b - a) * MAX_LODS, (uint32_t)stride);
        runs++;
        a = b;
    }
#undef RUN_CLASS
    return runs;
}

static void set_viewport(VkCommandBuffer cmd, float x, float y, float w, float h) {
    VkViewport vp = { x, y, w, h, 0.0f, 1.0f };
    VkRect2D sc = { { (int32_t)x, (int32_t)y }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// The occlusion path (prepass + Hi-Z + late list) decides itself, like Nanite paying only where it helps: it saves a
// lot where things hide one another (a house: each pixel shaded once, what's behind the walls not drawn) and costs a
// little in the open. Every 3 s the GPU's own frame time is measured for 24 frames without it and 24 with it (after 4
// frames of settling: the frames in flight) and the faster kept (on only when 3% faster). The image is the same either
// way (the two phases are conservative). REALENGINE_OCCLUSION=0 / 1 forces one.
#define OA_SETTLE 4u
#define OA_SAMPLE 24u
static void occlusion_auto(const VkCtx* vk) {
    if (R.oa_forced >= 0) {
        R.occlusion = R.oa_forced != 0;
        return;
    }
    double now = re_time_seconds();
    // a Masked mesh drawn: the prepass always (its layers cut once there, then shaded once); a frame without it, the
    // measure's, took 562 ms with the heavy test's cards (a freeze every 20 s; the Delights' hair: 83 ms)
    // (Q17) a displaced material with a DAG: the prepass always too (only there are its clusters diced: a frame of the
    // measure without it drew the rocks undiced, a pop every 3 s)
    if (R.any_masked || (R.any_dag && R.cl_work.buffer && R.any_tess)) {
        R.occlusion = true, R.oa_phase = 0;
        return;
    }
    if (R.oa_phase == 0) {
        R.occlusion = R.oa_best;
        if (now >= R.oa_next) R.oa_phase = 1, R.oa_frames = 0, R.oa_sum[0] = R.oa_sum[1] = 0.0;
        return;
    }
    uint32_t m = R.oa_phase == 1u ? 1u : 0u; // phase 1 measures with it, phase 2 without
    R.occlusion = m == 1u;
    // without it, a frame already far slower than the measure with it just taken: stopped there, on, measured again
    // later (hair cards dozens deep: 46 ms a frame without the prepass against 10 with it; the 28 frames of the test
    // every 3 s were a stutter that looked like a freeze)
    if (m == 0u && R.oa_frames >= OA_SETTLE && vk->gpu_ms > R.oa_sum[1] / OA_SAMPLE * 1.5 + 1.0) {
        R.oa_best = true, R.occlusion = true;
        R.stats.occlusion_ms[0] = vk->gpu_ms, R.stats.occlusion_ms[1] = R.oa_sum[1] / OA_SAMPLE;
        R.oa_phase = 0;
        R.oa_next = now + 20.0;
        return;
    }
    if (R.oa_frames >= OA_SETTLE) R.oa_sum[m] += vk->gpu_ms;
    if (++R.oa_frames < OA_SETTLE + OA_SAMPLE) return;
    R.oa_frames = 0;
    if (R.oa_phase == 1) {
        R.oa_phase = 2;
        return;
    }
    R.oa_best = R.oa_sum[1] < R.oa_sum[0] * 0.97;
    R.stats.occlusion_ms[0] = R.oa_sum[0] / OA_SAMPLE;
    R.stats.occlusion_ms[1] = R.oa_sum[1] / OA_SAMPLE;
    R.oa_phase = 0;
    R.oa_next = now + (R.oa_sum[1] < R.oa_sum[0] * 0.67 ? 20.0 : 3.0); // (a clear win: tested again much later)
}

// The texture streaming's need (material_stream.inc): each static mesh's material seen at its size on screen (its
// bounds' diameter in pixels at its nearest, times its tiling), STREAM_SWEEP meshes a frame; a pass over the world
// publishes the wants. Off-screen meshes count too (turning the camera never waits for a texture), hidden ones don't.
#define STREAM_SWEEP 16384u

// (Q18) A UDIM material's mesh: the bounds (local) of its LOD 0 triangles in each UDIM tile (a triangle's tile: its uv
// center's), made the first time the sweep meets the mesh with a UDIM material (again after the meshes reload)
#define UDIM_MESH_TILES 32
typedef struct UdimMeshTiles {
    uint32_t gen, first, count;
    int n;
    uint16_t udim[UDIM_MESH_TILES];
    float mn[UDIM_MESH_TILES][3], mx[UDIM_MESH_TILES][3];
} UdimMeshTiles;
static UdimMeshTiles* g_umt[RE_MAX_MESHES];
static const UdimMeshTiles* udim_mesh_tiles(uint32_t slot) {
    if (slot >= R.nmeshes || slot >= RE_MAX_MESHES || !R.cpu_v || !R.cpu_i) return NULL;
    const Mesh* me = &R.meshes[slot];
    UdimMeshTiles* t = g_umt[slot];
    if (t && t->gen == g_mesh_generation && t->first == me->first_index && t->count == me->index_count) return t;
    if (!t) t = g_umt[slot] = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->gen = g_mesh_generation, t->first = me->first_index, t->count = me->index_count, t->n = 0;
    for (uint32_t i = 0; i + 2 < me->index_count; i += 3) {
        const GpuVertex* v[3];
        for (int k = 0; k < 3; k++) v[k] = &R.cpu_v[(int64_t)me->vertex_offset + R.cpu_i[me->first_index + i + (uint32_t)k]];
        int ud = udim_of_uv((v[0]->uv[0] + v[1]->uv[0] + v[2]->uv[0]) / 3.0f, (v[0]->uv[1] + v[1]->uv[1] + v[2]->uv[1]) / 3.0f);
        if (!ud) continue;
        int k = 0;
        while (k < t->n && t->udim[k] != ud) k++;
        if (k == t->n) {
            if (t->n >= UDIM_MESH_TILES) continue;
            t->udim[t->n++] = (uint16_t)ud;
            for (int a = 0; a < 3; a++) t->mn[k][a] = FLT_MAX, t->mx[k][a] = -FLT_MAX;
        }
        for (int c = 0; c < 3; c++)
            for (int a = 0; a < 3; a++) t->mn[k][a] = fminf(t->mn[k][a], v[c]->pos[a]), t->mx[k][a] = fmaxf(t->mx[k][a], v[c]->pos[a]);
    }
    return t;
}
// Each of the mesh's tiles seen (in the view's cone, its bounds' size on screen) noted for its material: only the tiles
// seen climb, the rest go down to their least (material_udim.inc)
static void stream_note_udim(const CTransform* tr, uint32_t slot, uint32_t hash, const RenderView* view, float focal, bool ortho, float uvs) {
    const UdimMeshTiles* mt = udim_mesh_tiles(slot);
    if (!mt || !mt->n) return;
    float rows[3][4];
    render_transform_rows(tr, rows);
    float smax = fmaxf(fabsf(tr->scale[0]), fmaxf(fabsf(tr->scale[1]), fabsf(tr->scale[2])));
    float th = tanf(view->fov_deg * 0.5f * DEG2RAD), tv = th * fmaxf(view->h, 1.0f) / fmaxf(view->w, 1.0f);
    float cone = atanf(sqrtf(th * th + tv * tv));
    float aspect = fmaxf(view->w, 1.0f) / fmaxf(view->h, 1.0f);
    for (int k = 0; k < mt->n; k++) {
        float c[3], e2 = 0.0f, wp[3], d[3], dist = 0.0f;
        for (int a = 0; a < 3; a++) c[a] = (mt->mn[k][a] + mt->mx[k][a]) * 0.5f, e2 += (mt->mx[k][a] - mt->mn[k][a]) * (mt->mx[k][a] - mt->mn[k][a]);
        float half = sqrtf(e2) * 0.5f * smax;
        for (int r = 0; r < 3; r++) wp[r] = rows[r][0] * c[0] + rows[r][1] * c[1] + rows[r][2] * c[2] + rows[r][3], d[r] = wp[r] - view->pos[r], dist += d[r] * d[r];
        dist = sqrtf(dist);
        bool seen;
        if (ortho) {
            float x = d[0] * view->right[0] + d[1] * view->right[1] + d[2] * view->right[2], y = d[0] * view->up[0] + d[1] * view->up[1] + d[2] * view->up[2];
            seen = fabsf(x) <= view->ortho_half * aspect + half && fabsf(y) <= view->ortho_half + half;
        } else if (dist <= half) seen = true;
        else {
            float cs = (d[0] * view->fwd[0] + d[1] * view->fwd[1] + d[2] * view->fwd[2]) / dist;
            float ang = acosf(fmaxf(fminf(cs, 1.0f), -1.0f)), grow = asinf(fminf(half / dist, 1.0f));
            seen = ang <= cone + grow;
        }
        if (!seen) continue;
        float px = ortho ? 2.0f * half * focal : 2.0f * half * focal / fmaxf(dist - half, 1.0f);
        mat_stream_note_tile(hash, mt->udim[k], px * uvs);
    }
}

static void stream_sweep(World* w, const RenderView* view) {
    if (!mat_stream_on()) return;
    bool ortho = view->ortho_half > 0.0f;
    float focal = ortho ? fmaxf(view->h, 1.0f) * 0.5f / view->ortho_half : fmaxf(view->w, 1.0f) * 0.5f / tanf(view->fov_deg * 0.5f * DEG2RAD);
    const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
    uint32_t left = STREAM_SWEEP, last_id = UINT32_MAX, slot = 0;
    while (left) {
        if (R.ss_arch >= w->archetype_count) { // a pass done: its wants (10 a second at most: a small world's pass is a frame)
            double now = re_time_seconds();
            if (now - R.ss_pass < 0.1) break;
            R.ss_pass = now;
            mat_stream_pass_end();
            R.ss_arch = R.ss_row = 0;
            break;
        }
        const EcsArchetype* a = &w->archetypes[R.ss_arch];
        if ((a->mask & need) != need || R.ss_row >= a->count) {
            R.ss_arch++, R.ss_row = 0;
            continue;
        }
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        uint32_t end = R.ss_row + left < a->count ? R.ss_row + left : a->count;
        for (uint32_t i = R.ss_row; i < end; i++) {
            if (!sm[i].material || (view->editor && (tr[i].flags & TRANSFORM_HIDDEN)) || (tr[i].flags & TRANSFORM_HIDDEN_GAME)) continue;
            if (sm[i].mesh != last_id) last_id = sm[i].mesh, slot = render_mesh_slot(sm[i].mesh);
            const Mesh* me = &R.meshes[slot];
            float half = 0.0f, off = 0.0f, d2 = 0.0f;
            for (int k = 0; k < 3; k++) {
                float s = fabsf(tr[i].scale[k]), e = (me->mx[k] - me->mn[k]) * 0.5f * s, c = (me->mx[k] + me->mn[k]) * 0.5f * s;
                float d = tr[i].location[k] - view->pos[k];
                half += e * e, off += c * c, d2 += d * d;
            }
            half = sqrtf(half);
            float dist = sqrtf(d2) - half - sqrtf(off); // its nearest point (the pivot may be off its center)
            float uv = sm[i].uv_scale > 0.0f ? sm[i].uv_scale : 1.0f;
            float px = ortho ? 2.0f * half * focal : 2.0f * half * focal / fmaxf(dist, 1.0f);
            if (mat_stream_note(sm[i].material, px * uv)) stream_note_udim(&tr[i], slot, sm[i].material, view, focal, ortho, uv); // (Q18)
        }
        left -= end - R.ss_row;
        R.ss_row = end;
    }
}

// Opacity micromaps (notas-dev/render.md "Opacity micromaps"): a Masked mesh drawn by one material, a second after
// that material was first seen (baked): omm.comp marks its triangles' 64 micro triangles by the alpha, gi_blas_omm makes
// the micromap and the BLAS again with it. One mesh a frame, waiting for the GPU (once each)
static void omm_update(VkCtx* vk) {
    if (!R.omm_pipe || !gi_available() || !R.any_masked) return;
    for (uint32_t s = SHAPE_COUNT; s < R.nmeshes; s++) {
        if (!R.mesh_masked[s] || !R.omm_slot[s] || R.omm_bad[s] || R.omm_done[s] || R.frame_counter - R.omm_seen[s] < 120u) continue;
        R.omm_done[s] = true;
        uint32_t ntri = R.meshes[s].index_count / 3u;
        if (ntri < 128u) return;
        double t0 = re_time_seconds();
        GpuBuf job = { 0 }, data = { 0 };
        uint32_t level = getenv("REALENGINE_OMM_LEVEL") ? (uint32_t)atoi(getenv("REALENGINE_OMM_LEVEL")) : 3u;
        if (level < 1u || level > 6u) level = 3u;
        uint32_t stride = (1u << (2u * level)) / 4u;
        bool ok = gpu_buf_create(vk, &job, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) &&
                  gpu_buf_create(vk, &data, (VkDeviceSize)ntri * stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT, true);
        uint64_t st[4] = { 0, 0, 0, 0 }; // (the micro triangles by state: transparent, opaque, -, unknown)
        if (ok) {
            struct { uint64_t out, indices, materials; uint32_t first_index; int32_t vertex_offset; uint32_t ntri, slot; float tiling; uint32_t level; } j = {
                data.addr, R.indices.addr, mat_buffer_address(), R.meshes[s].first_index, R.meshes[s].vertex_offset, ntri, R.omm_slot[s] - 1u, R.omm_uv[s], level };
            memcpy(job.mapped, &j, sizeof j);
            VkCommandBuffer cmd = gpu_begin_once(vk);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.omm_pipe);
            VkDescriptorSet sets_[2] = { R.set, mat_set() };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 2, sets_, 0, NULL);
            Push p = { R.frame_dev.addr, R.vertices.addr, job.addr, 0, 0 };
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof p, &p);
            vkCmdDispatch(cmd, (ntri + 63u) / 64u, 1, 1);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT);
            gpu_end_once(vk, cmd);
            const uint32_t* dw = data.mapped;
            for (uint64_t k = 0; k < (uint64_t)ntri * stride / 4u; k++)
                for (uint32_t q = 0; q < 16u; q++) st[(dw[k] >> (q * 2u)) & 3u]++;
            GiMesh gm = { R.meshes[s].first_index, R.meshes[s].index_count, R.meshes[s].vertex_offset };
            ok = gi_blas_omm(vk, (int)s, &R.vertices, R.vertex_count, &R.indices, &gm, data.addr, ntri, level);
        }
        gpu_buf_destroy(vk, &job), gpu_buf_destroy(vk, &data);
        if (ok) {
            R.mesh_omm[s] = true;
            upload_mesh_info(vk);
            // both TLAS built again this frame (the BLAS moved, its instances no longer forced non-opaque: scene_prepare already
            // chose this frame's work, and a refit or a kept TLAS traced the freed BLAS: the GPU went down)
            R.tlas_work = GI_TLAS_BUILD, R.dyn_changed = true;
        }
        double tot = (double)ntri * (double)(1u << (2u * level));
        printf("[RealEngine] Opacity micromap: %s, %u triangles x %u (%.0f%% opaque, %.0f%% transparent, %.0f%% unknown), %s, %.0f ms\n",
               R.mesh_name[s], ntri, 1u << (2u * level), 100.0 * (double)st[1] / tot, 100.0 * (double)st[0] / tot, 100.0 * (double)st[3] / tot, ok ? "made" : "failed",
               (re_time_seconds() - t0) * 1000.0);
        return;
    }
}

// One step of the local lights' shadow maps (lsm.c): a rendering over a layer of the atlas, each face its viewport and
// scissor; mode 1: the whole faces' static casters into the static layer (their tiles cleared first), 2: every face's
// dynamic casters over the maps
static void lsm_pass(VkCommandBuffer cmd, Push* push, const LsmJob* jobs, int njobs, int mode, uint32_t* draws) {
    const uint32_t side = (uint32_t)lsm_atlas_side();
    VkRenderingAttachmentInfo sd = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    sd.imageView = lsm_layer_view(mode == 1 ? 1 : 0);
    sd.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    sd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { side, side } };
    ri.layerCount = 1;
    ri.pDepthAttachment = &sd;
    vkCmdBeginRendering(cmd, &ri);
    for (int j = 0; j < njobs; j++) {
        const LsmJob* jb = &jobs[j];
        if (mode == 1 && !jb->full) continue;
        if (mode == 1) { // (the tile cleared to far)
            VkClearAttachment ca = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, { .depthStencil = { 1.0f, 0 } } };
            VkClearRect cr = { { { (int32_t)jb->x, (int32_t)jb->y }, { jb->size, jb->size } }, 0, 1 };
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        }
        set_viewport(cmd, (float)jb->x, (float)jb->y, (float)jb->size, (float)jb->size);
        push->pass = 1;
        push->pad = 0x80000000u | jb->index | (uint32_t)mode << 16; // (mesh.vert: the face's job, the casters)
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof *push, push);
        *draws += draw_runs(cmd, (VkDeviceSize)R.ncmd * sizeof(VkDrawIndexedIndirectCommand), R.shadow_pipe, R.shadow_m, NULL, NULL);
    }
    vkCmdEndRendering(cmd);
}

// (Q11) the frame's outlines: each outlined actor in view (its slot marked by instance_of, alive) with its look (its
// actor's own, else its material slot's: a dynamic instance's when a Blueprint changed it) as an entry of the frame's
// table, drawn with the LOD the cull picks (vel_visible's rule); outline_record draws and composites them
static void outline_frame(VkCtx* vk, VkCommandBuffer cmd, World* w, const RenderView* view, uint32_t fslot, const Push* push,
                          uint32_t vw, uint32_t vh, GpuImage* sc) {
#define OL_MAX_DRAWS 8192
    static OutlineDraw draws[OL_MAX_DRAWS];
    static OutlineEntry entries[OUTLINE_MAX_ENTRIES + 1];
    static uint32_t keys[OUTLINE_MAX_ENTRIES + 1];
    if (!sc) {
        outline_record(vk, cmd, fslot, R.set, mat_set(), R.layout, push, vw, vh, NULL, NULL, 0, NULL, 0);
        return;
    }
    double t0 = re_time_seconds();
    uint32_t nd = 0, ne = 0;
    VelFrustum vf;
    vel_frustum(view, &vf);
    uint32_t n = R.scene_count < R.scene_cap ? R.scene_count : R.scene_cap;
    MatToon def;
    toon_defaults(&def);
    for (uint32_t s = 0; s < n && nd < OL_MAX_DRAWS; s++) {
        if (!R.ol_flag[s] || !(R.slot_state[s] & SLOT_ALIVE)) continue;
        Entity e = ecs_entity_at(w, s);
        const CStaticMesh* sm = e != ENTITY_NONE ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        const CTransform* t = e != ENTITY_NONE ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!sm || !t) continue;
        uint32_t mesh = render_mesh_slot(sm->mesh);
        const Mesh* me = &R.meshes[mesh];
        float rows[3][4], bmn[3], bmx[3], lc[3], le[3];
        render_transform_rows(t, rows);
        for (int k = 0; k < 3; k++) lc[k] = (me->mn[k] + me->mx[k]) * 0.5f, le[k] = (me->mx[k] - me->mn[k]) * 0.5f;
        for (int r = 0; r < 3; r++) {
            float c = rows[r][3] + rows[r][0] * lc[0] + rows[r][1] * lc[1] + rows[r][2] * lc[2];
            float x = fabsf(rows[r][0]) * le[0] + fabsf(rows[r][1]) * le[1] + fabsf(rows[r][2]) * le[2];
            bmn[r] = c - x, bmx[r] = c + x;
        }
        uint32_t lod = 0;
        if (!vel_visible(&vf, bmn, bmx, mesh, &lod)) continue;
        uint32_t fi = me->first_index, ic = me->index_count;
        int32_t vo = me->vertex_offset;
        if (lod) render_mesh_lod_range(mesh, lod, &fi, &ic, &vo);
        Material* m = sm->material ? mat_find_hash(sm->material) : NULL;
        uint32_t ms = m && m->slot ? mat_slot_of(e, m) : 0u;
        const MatToon* tt = mat_toon_slot(ms);
        uint32_t key;
        MatToon own;
        if (outline_actor_mode(sm->outline) == OUTLINE_ACTOR_ON) { // its own color and width, the rest its material's
            own = tt ? *tt : def;
            for (int k = 0; k < 3; k++) {
                float c = (float)((sm->outline >> (16 - 8 * k)) & 255u) / 255.0f;
                own.outline_color[k] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
            }
            if (sm->outline_width > 0.0f) own.outline_width = sm->outline_width;
            uint32_t wb;
            memcpy(&wb, &sm->outline_width, 4);
            key = 0x80000000u | ((sm->outline & 0xFFFFFFu) ^ (wb * 2654435761u) ^ (ms << 20));
            tt = &own;
        } else {
            if (!tt || !tt->outline) continue;
            key = ms;
        }
        uint32_t ei = 0;
        for (uint32_t k = 1; k <= ne && !ei; k++)
            if (keys[k] == key) ei = k;
        if (!ei) {
            if (ne >= OUTLINE_MAX_ENTRIES) continue; // (255 looks at most a frame)
            ei = ++ne;
            keys[ei] = key;
            outline_entry_of(tt, &entries[ei]);
        }
        draws[nd++] = (OutlineDraw){ s, fi, ic, ei, vo };
    }
    outline_set_cpu_ms((re_time_seconds() - t0) * 1000.0);
    outline_record(vk, cmd, fslot, R.set, mat_set(), R.layout, push, vw, vh, sc, draws, nd, entries, ne);
}

bool render_scene(VkCtx* vk, VkCommandBuffer cmd, World* w, const RenderView* view) {
    double t0 = re_time_seconds();
    uint32_t slot = vk->frame;
    uint32_t vw = (uint32_t)(view->w > 1 ? view->w : 1), vh = (uint32_t)(view->h > 1 ? view->h : 1);
    if (view->w < 2 || view->h < 2) return false;
    stream_sweep(w, view); // (the texture streaming's need: a slice of the world)
    // the materials edited since the last frame: compiled and baked (their instances sent again: new slots, colors)
    if (mat_outline_generation() != R.ol_gen) R.ol_gen = mat_outline_generation(), R.scene_valid = false; // (Q11: an outline on or off)
    if (mat_update(vk, cmd)) {
        R.scene_valid = false;
        // (2026-11-10) the meshes' classes marked again as their instances are sent: a material no longer Masked or
        // Translucent (its Transmission back to 0, say) takes its mesh back to the opaque pipelines
        memset(R.mesh_masked, 0, sizeof R.mesh_masked), R.any_masked = false, R.any_tess = false;
        memset(R.mesh_translucent, 0, sizeof R.mesh_translucent), R.any_translucent = false;
        memset(R.mesh_ext, 0, sizeof R.mesh_ext), memset(R.mesh_ext_warned, 0, sizeof R.mesh_ext_warned);
        R.masked_dirty = true;
    }
    occlusion_auto(vk);
    R.stats.occlusion = R.occlusion;

    // The timestamps of the frame that used this slot before (vkctx_begin_frame waited for it)
    if (R.queries && R.queries_valid[slot]) {
        uint64_t t[8]; // the ones every frame writes (8-13: the detail, read below)
        if (vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME, 8, sizeof t, t, sizeof t[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double k = vk->timestamp_ms;
            R.stats.gpu_scene_ms = (double)(t[1] - t[0]) * k;
            R.stats.gpu_gi_ms = (double)(t[2] - t[1]) * k;
            R.stats.gpu_shadow_ms = (double)(t[3] - t[2]) * k;
            R.stats.gpu_main_ms = (double)(t[4] - t[3]) * k;
            R.stats.gpu_mask_ms = (double)(t[5] - t[4]) * k;
            R.stats.gpu_exposure_ms = (double)(t[6] - t[5]) * k;
            R.stats.gpu_composite_ms = (double)(t[7] - t[6]) * k;
            uint64_t tp = 0; // 14: the post pass and the bloom done (between the exposure and the composite)
            if (vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 14, 1, sizeof tp, &tp, sizeof tp,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && tp >= t[6] && tp <= t[7]) {
                R.stats.gpu_post_ms = (double)(tp - t[6]) * k;
                R.stats.gpu_composite_ms = (double)(t[7] - tp) * k;
            }
            uint64_t lt[2] = { 0, 0 }; // 18-19: the local lights' shadow maps (none drawn that frame: 0)
            R.stats.gpu_lsm_ms = R.lsm_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 18, 2, sizeof lt, lt, sizeof lt[0],
                                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && lt[1] >= lt[0]
                                     ? (double)(lt[1] - lt[0]) * k
                                     : 0.0;
            if (R.lsm_ts[slot] && R.stats.gpu_lsm_ms > 0.0) lsm_note_gpu(R.stats.gpu_lsm_ms);
            uint64_t ga[2] = { 0, 0 }; // 20-21: the screen space AO (off that frame: 0)
            R.stats.gpu_gtao_ms = R.gtao_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 20, 2, sizeof ga, ga, sizeof ga[0],
                                                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ga[1] >= ga[0]
                                      ? (double)(ga[1] - ga[0]) * k
                                      : 0.0;
            uint64_t sq[2] = { 0, 0 }; // 26-27: the skin's scattering (off: 0)
            R.stats.gpu_sss_ms = R.sss_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 26, 2, sizeof sq, sq, sizeof sq[0],
                                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && sq[1] >= sq[0]
                                     ? (double)(sq[1] - sq[0]) * k
                                     : 0.0;
            uint64_t bk[2] = { 0, 0 }; // 28-29: a bake's share (Q16): its budget's measure (gi_bake.c)
            if (R.bake_rows[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 28, 2, sizeof bk, bk, sizeof bk[0],
                                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && bk[1] >= bk[0])
                gi_bake_note_gpu((double)(bk[1] - bk[0]) * k, R.bake_rows[slot]);
            uint64_t tq[2] = { 0, 0 }; // 30-31: (Q17) the Nanite Tessellation's dicing (off: 0)
            R.stats.gpu_tess_ms = R.tess_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 30, 2, sizeof tq, tq, sizeof tq[0],
                                                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && tq[1] >= tq[0]
                                      ? (double)(tq[1] - tq[0]) * k
                                      : 0.0;
            uint64_t fq[2] = { 0, 0 }; // 32-33: (Q21) the distant view's horizon pass (when it ran)
            if (R.far_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 32, 2, sizeof fq, fq, sizeof fq[0],
                                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && fq[1] >= fq[0])
                farview_collect((double)(fq[1] - fq[0]) * k);
            uint64_t dn[2] = { 0, 0 }; // 24-25: the unified denoiser (off: 0)
            R.stats.gpu_denoise_ms = R.den_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 24, 2, sizeof dn, dn, sizeof dn[0],
                                                                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && dn[1] >= dn[0]
                                         ? (double)(dn[1] - dn[0]) * k
                                         : 0.0;
            uint64_t ut[2] = { 0, 0 }; // 22-23: the upscale, TSR or a vendor's (none without it: 0)
            R.stats.gpu_upscale_ms = R.ups_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 22, 2, sizeof ut, ut, sizeof ut[0],
                                                                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ut[1] >= ut[0]
                                         ? (double)(ut[1] - ut[0]) * k
                                         : 0.0;
            R.stats.upscaler = R.ups_ts[slot] ? R.ups_kind : -1;
            uint64_t sk[2] = { 0, 0 }; // 16-17: the skins' bends (none that frame: 0)
            R.stats.gpu_skin_ms = R.skin_ts[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 16, 2, sizeof sk, sk, sizeof sk[0],
                                                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && sk[1] >= sk[0]
                                      ? (double)(sk[1] - sk[0]) * k
                                      : 0.0;
        }
        // the finer ones (the Debug Viewer): atmosphere, scene and cull, weather; probes, cache; the meshes of the main pass
        uint64_t d[6];
        if (R.detail_slot[slot] && vkGetQueryPoolResults(vk->device, R.queries, slot * TS_PER_FRAME + 8, 6, sizeof d, d, sizeof d[0],
                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double k = vk->timestamp_ms;
            R.stats.gpu_atmosphere_ms = (double)(d[0] - t[0]) * k;
            R.stats.gpu_cull_ms = (double)(d[1] - d[0]) * k;
            R.stats.gpu_weather_ms = (double)(d[2] - d[1]) * k;
            R.stats.gpu_probes_ms = (double)(d[3] - t[1]) * k;
            R.stats.gpu_cache_ms = (double)(d[4] - d[3]) * k;
            R.stats.gpu_gi_other_ms = (double)(t[2] - d[4]) * k;
            R.stats.gpu_meshes_ms = (double)(d[5] - t[3]) * k;
            R.stats.gpu_sky_ms = (double)(t[4] - d[5]) * k;
            R.stats.detail = true;
        }
    }
    if (R.detail_slot[slot] && R.vis_want && R.vis_read[slot].mapped) { // that frame's visibility per slot, when asked
        uint32_t n = R.vis_read_n[slot];
        if (n > R.vis_cap) {
            uint32_t* p = realloc(R.vis_cpu, (size_t)n * 4); // the Debug Viewer open and the scene grew: not the game's frame
            if (p) R.vis_cpu = p, R.vis_cap = n;
        }
        if (n <= R.vis_cap) {
            memcpy(R.vis_cpu, R.vis_read[slot].mapped, (size_t)n * 4);
            R.vis_n = n;
        }
        R.vis_want = false;
    }
    if (R.cull_read[slot].mapped && R.queries_valid[slot]) { // that frame's cull: instances and triangles drawn
        const uint32_t* c = R.cull_read[slot].mapped;
        uint64_t inst = 0, tris = 0;
        for (uint32_t d = 0; d < R.ncmd; d++) {
            uint32_t s = d / MAX_LODS, l = d % MAX_LODS;
            uint32_t k = c[d] + c[2 * R.ncmd + d]; // the early list and the late one
            inst += k;
            if (l < R.lod_count[s]) tris += (uint64_t)k * (R.lods[s][l].index_count / 3);
        }
        R.stats.visible = (uint32_t)inst;
        R.stats.triangles = tris;
    }
    if (R.cl_read[slot].mapped && R.queries_valid[slot]) { // that frame's clusters: their draws' indices (count x instances)
        const uint32_t* c = R.cl_read[slot].mapped;
        uint64_t t[3] = { 0, 0, 0 };
        uint32_t ne[3] = { 0, 0, 0 }; // (the instances drawn by each)
        // only the entries pass 0 made (the header's count, copied after the draws): the buffer is uncached host memory,
        // reading all 3 x 4096 draws cost ~1.1 ms of CPU a frame
        uint32_t used = c[3 * CL_MAX_ENTRIES * 5];
        if (used > CL_MAX_ENTRIES) used = CL_MAX_ENTRIES;
        for (uint32_t a = 0; a < 3; a++)
            for (uint32_t e = 0; e < used; e++) {
                t[a] += (uint64_t)c[(a * CL_MAX_ENTRIES + e) * 5] * c[(a * CL_MAX_ENTRIES + e) * 5 + 1] / 3u;
                ne[a] += c[(a * CL_MAX_ENTRIES + e) * 5 + 1];
            }
        R.stats.triangles += t[0] + t[1] + t[2];
        R.dag_tris[0] = t[1], R.dag_tris[1] = t[2];
        static uint32_t n;
        if (getenv("REALENGINE_DAG_STATS") && ++n % 240u == 0u)
            printf("[RealEngine] DAG: %u instances, %llu triangles in the first phase, %llu in the second; Masked clusters %llu (%u instances)\n",
                   ne[1], (unsigned long long)t[1], (unsigned long long)t[2], (unsigned long long)t[0], ne[0]);
    }
    { // (Q17) that frame's dicing: its micro triangles (the stats' triangles too) and the budget's next scale
        tess_collect(slot, R.stats.gpu_tess_ms);
        const TessStats* tst = tess_stats();
        R.stats.tess_micro = tst->micro, R.stats.tess_base = tst->base, R.stats.tess_scale = tst->scale;
        R.stats.triangles += tst->micro;
        if (!tst->subdiv) R.stats.gpu_tess_ms = 0.0;
    }
    // the relevance that frame computed: the systems (World.relevant) and the dynamic TLAS read it. Copied to the CPU's
    // memory in one go: the readback is uncached, and a read per animated part from it cost 5 ms for 28 000 parts
    R.rel_cur = NULL;
    R.rel_cur_slots = R.rel_slots[slot];
    if (R.rel_read[slot].mapped && R.rel_cur_slots) {
        uint32_t words = (R.rel_cur_slots + 31u) / 32u;
        if (words > R.rel_copy_cap) {
            uint32_t* nc = realloc(R.rel_copy, sizeof(uint32_t) * words * 2u); // the world grew (rare)
            if (nc) R.rel_copy = nc, R.rel_copy_cap = words * 2u;
        }
        if (words <= R.rel_copy_cap) {
            memcpy(R.rel_copy, R.rel_read[slot].mapped, sizeof(uint32_t) * words);
            R.rel_cur = R.rel_copy;
        }
    }
    w->relevant = R.rel_cur;
    w->relevant_slots = R.rel_cur_slots;
    if (R.flicker_on && R.flicker[slot].mapped && R.frame_counter > RE_FRAMES_IN_FLIGHT) { // (a measure only: host memory reads are slow)
        // the flicker probe: that frame's grid against the one before it (the mean relative change, a still camera)
        const float* g = R.flicker[slot].mapped;
        // and the share of points that changed more than 2% (a flicker in one corner, which the mean dilutes), and of
        // those that went back and forth by more than 2% in two frames (up then down: what moves smoothly, an animated
        // character, doesn't; a flicker does: the measure with things moving in the view)
        double num = 0.0, den = 0.0;
        uint32_t spots = 0, pulses = 0;
        double fmean = 0.0;
        for (int i = 0; i < 64 * 32; i++) {
            double a = g[i], b = R.flicker_prev[i], c = R.flicker_prev2[i], d = fabs(a - b), lim = 0.02 * fmax(fmax(a, b), 1e-6);
            if (a > R.probe_max && R.frame_counter > 32u) R.probe_max = a; // (the first frames' values aren't settled: up to ~2000 nits at 3-12, ray traced or not, in a closed dark room)
            fmean += a;
            num += d;
            den += a + b;
            if (d > lim) spots++, R.flicker_map[i]++;
            if (d > lim && fabs(b - c) > 0.02 * fmax(fmax(b, c), 1e-6) && (a - b) * (b - c) < 0.0) pulses++;
        }
        R.probe_mean_sum += fmean / (64.0 * 32.0), R.probe_frames++;
        if (R.flicker_have && den > 0.0) {
            R.flicker_sum += 2.0 * num / den;
            R.flicker_spots += (double)spots / (64.0 * 32.0);
            if (R.flicker_have2) R.flicker_pulses += (double)pulses / (64.0 * 32.0);
            R.flicker_frames++;
        }
        R.flicker_have2 = R.flicker_have;
        memcpy(R.flicker_prev2, R.flicker_prev, sizeof R.flicker_prev2);
        memcpy(R.flicker_prev, g, sizeof R.flicker_prev);
        R.flicker_have = true;
        // (Q20) the ghosting probe: a point a moving actor covered (negative) gets age 0; once left, what still changes
        // there over the next frames is the trail it left (the TSR's history, the denoiser's, the GI's lag), measured
        // against how much the points left long ago change (the background's own flicker)
        const float* h = R.ghost[slot].mapped;
        if (h) {
            double gn = 0.0, gd = 0.0, bn = 0.0, bd = 0.0, gs = 0.0;
            for (int i = 0; i < 128 * 64; i++) {
                float v = h[i], l = fabsf(v);
                if (v < 0.0f) R.ghost_age[i] = 0, R.ghost_cov++;
                else if (R.ghost_age[i] < 255) R.ghost_age[i]++;
                if (R.ghost_have && v >= 0.0f && R.ghost_prev[i] >= 0.0f) {
                    double d = fabs((double)l - R.ghost_prev[i]), m = (double)l + R.ghost_prev[i];
                    uint8_t a = R.ghost_age[i];
                    if (a >= 2 && a <= 8) gn += d, gd += m, gs += 1.0;
                    else if (a == 255) bn += d, bd += m;
                }
                R.ghost_prev[i] = R.ghost_age[i] == 0 ? -1.0f : l; // (covered: no step measured into the next frame)
            }
            if (R.ghost_have) {
                R.ghost_num += gn, R.ghost_den += gd, R.bg_num += bn, R.bg_den += bd, R.ghost_samples += gs;
                R.ghost_frames++;
            }
            R.ghost_have = true;
        }
    }
    if (R.exposure_read[slot].mapped) { // that frame's adapted exposure and mean log2 luminance
        const float* ev = R.exposure_read[slot].mapped;
        R.stats.auto_exposure = ev[0];
        R.stats.avg_log_lum = ev[3];
    }
    int want = view->msaa >= 8 ? 8 : view->msaa >= 4 ? 4 : view->msaa >= 2 ? 2 : 1;
    VkSampleCountFlagBits samples = (VkSampleCountFlagBits)want;
    while (samples > R.max_samples) samples = (VkSampleCountFlagBits)(samples >> 1);
    // Temporal super resolution: the render at a share of the output's size (REALENGINE_TSR_SCALE: a measure's override;
    // REALENGINE_NO_TSR: off); never under the path tracer
    {
        float sc = view->gi.tsr_scale;
        const char* es = getenv("REALENGINE_TSR_SCALE");
        if (es && atof(es) > 0.0) sc = (float)atof(es);
        if (getenv("REALENGINE_NO_TSR")) sc = 0.0f;
        R.tsr_on = sc > 0.0f && !(view->view_mode == VIEW_PATH_TRACING && gi_available());
        R.tsr_scale = R.tsr_on ? (sc < 0.25f ? 0.25f : sc > 1.0f ? 1.0f : sc) : 1.0f;
    }
    uint32_t out_w = vw, out_h = vh; // the output's size; vw, vh from here: the render's
    if (R.tsr_on) {
        vw = (uint32_t)((float)out_w * R.tsr_scale + 0.5f), vh = (uint32_t)((float)out_h * R.tsr_scale + 0.5f);
        if (vw < 8) vw = 8;
        if (vh < 8) vh = 8;
    }
    if (!ensure_targets(vk, vw, vh, out_w, out_h, R.tsr_on, samples)) return false;
    ar_resize(vk); // (AR's picture at a new size: before the set is bound)
    { // the upscaler: a vendor's in TSR's place when chosen and it runs here (a perspective view: their depth is the
      // reversed infinite Z's); its jitter's phases as theirs ask, 8 x the pixels' ratio (TSR too since 2026-11-23: 8 left
      // a 67% render's output pixels few distinct samples near their centers, the detail above the render's stayed lost)
        int uk = g_ups_override >= 0 ? g_ups_override : project()->upscaler;
        int prev = R.ups_kind;
        R.ups_kind = R.tsr_on && uk > UPS_TSR && uk < UPS_COUNT && view->ortho_half <= 0.0f && upscale_available(uk, NULL) ? uk : UPS_TSR;
        if (R.ups_kind != prev) R.tsr_reset = true;
        float ratio = (float)out_w / (float)vw;
        uint32_t ph = (uint32_t)ceilf(8.0f * ratio * ratio);
        R.ups_phases = ph < 8u ? 8u : ph > 64u ? 64u : ph;
        R.ups_fov_y = 2.0f * atanf(tanf(view->fov_deg * 0.5f * DEG2RAD) * (float)vh / (float)vw);
    }
    GpuImage* SC = R.tsr_on ? &R.tsr_in : &R.hdr; // the render's color (the output's HDR target without TSR)

    GpuFrame* fr = R.frame[slot].mapped;
    memcpy(fr->fade, g_fade, sizeof g_fade); // (B9: the Sequencer's Fade)
    memcpy(fr->terror, view->terror, sizeof fr->terror); // (Q13: the horror kit, terror.c terror_view)
    if (view->vr_vignette > 0.0f) { // (Q25) the VR pawn's comfort vignette by the horror kit's (composite.frag terror_display)
        if (fr->terror[3][3] <= 0.0f) fr->terror[2][2] = 1.0f, fr->terror[3][3] = 1.0f; // (off: its display parts are neutral, terror.c's 'any')
        fr->terror[2][1] = fmaxf(fr->terror[2][1], fminf(view->vr_vignette, 1.0f));
    }
    if (view->vr_fade > 0.0f) { // (Q25) the head in a wall: Dip to Black's channel (composite.frag terror_display)
        if (fr->terror[3][3] <= 0.0f) fr->terror[2][2] = 1.0f, fr->terror[3][3] = 1.0f;
        float a = fminf(view->vr_fade, 1.0f);
        if (a > fr->terror[4][3]) fr->terror[4][0] = fr->terror[4][1] = fr->terror[4][2] = 0.0f, fr->terror[4][3] = a;
    }
    lut_follow(vk, view->grade_lut); // (A9.4: the volumes' Color Grading LUT)
    fr->grade_lut[0] = R.lut_ok && view->grade_lut && !strcmp(view->grade_lut, R.lut_path) ? view->grade_lut_intensity : 0.0f;
    fr->grade_lut[1] = (float)R.lut_n;
    { // (Q20) the TSR's copy of last frame's depth (disocclusions: tsr.comp), at the output's size, made before this
        // frame binds anything; only for a one-sample depth and while an actor moves (the copy is made then)
        bool want = R.tsr_on && project()->tsr_disocclusion && !getenv("REALENGINE_NO_TSR_DISOCCLUSION");
        uint32_t cw = R.out_w > vw ? R.out_w : vw, ch = R.out_h > vh ? R.out_h : vh;
        if (want && R.post_vel && (!R.tsr_dep.image || R.tsr_dep_cap_w < vw || R.tsr_dep_cap_h < vh)) {
            vkDeviceWaitIdle(vk->device);
            gpu_image_destroy(vk, &R.tsr_dep);
            R.tsr_dep_valid = false;
            if (gpu_image_create(vk, &R.tsr_dep, cw, ch, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
                R.tsr_dep_cap_w = cw, R.tsr_dep_cap_h = ch;
                VkCommandBuffer c = gpu_begin_once(vk);
                gpu_transition(c, &R.tsr_dep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
                gpu_end_once(vk, c);
            } else R.tsr_dep_cap_w = R.tsr_dep_cap_h = 0;
            update_descriptors(vk);
        }
        if (!want) R.tsr_dep_valid = false;
    }
    { // (A4c) SSR's copies at the render's size, made before this frame binds anything (the set changes here)
        R.ssr_want = view->gi.ssr && R.samples == VK_SAMPLE_COUNT_1_BIT && !getenv("REALENGINE_NO_SSR") &&
                     (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY);
        uint32_t sw = vw, sh = vh; // (the render's size: under TSR smaller than the output)
        // (2026-11-16: refractive glass reads the same copy; 2026-11-17, Q4: the rain's drops too, what is behind them)
        bool rain_want = water_rain_intensity() > 0.0f && !getenv("REALENGINE_RAIN_FLAT");
        bool refr_want = (R.any_refract || rain_want) && R.samples == VK_SAMPLE_COUNT_1_BIT;
        if ((R.ssr_want || refr_want) && (!R.ssr_col.image || R.ssr_w != sw || R.ssr_h != sh)) {
            vkDeviceWaitIdle(vk->device);
            gpu_image_destroy(vk, &R.ssr_col), gpu_image_destroy(vk, &R.ssr_dep);
            R.ssr_w = sw, R.ssr_h = sh, R.ssr_valid = false;
            bool ok = gpu_image_create(vk, &R.ssr_col, sw, sh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
                      gpu_image_create(vk, &R.ssr_dep, sw, sh, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!ok) gpu_image_destroy(vk, &R.ssr_col), gpu_image_destroy(vk, &R.ssr_dep), R.ssr_want = false, R.any_refract = false;
            else { // (in the layout the set names from the start: read only after the first copy)
                VkCommandBuffer c = gpu_begin_once(vk);
                gpu_transition(c, &R.ssr_col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
                gpu_transition(c, &R.ssr_dep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
                gpu_end_once(vk, c);
            }
            update_descriptors(vk);
        }
    }
    if (vk->hdr) hdr_ui_target(vk); // (A9.4) HDR10: the UI's target, then the numbers the composite needs
    fr->hdr[0] = vk->hdr ? 1.0f : 0.0f, fr->hdr[1] = R.hdr_paper > 0.0f ? R.hdr_paper : 200.0f;
    fr->hdr[2] = R.hdr_peak > 0.0f ? R.hdr_peak : 1000.0f, fr->hdr[3] = vk->hdr && R.ui.image ? 1.0f : 0.0f;
    { // (2026-11-10) the Stylized style's own settings (composite.frag)
        const ProjectSettings* ps = project();
        float bands = (float)(ps->stylized_bands < 2 ? 2 : ps->stylized_bands > 32 ? 32 : ps->stylized_bands);
        float rs = (float)vk->extent.height / 1440.0f; // (the outline's width at 1440p: as thick on any screen)
        float st[3][4] = { { bands, fminf(fmaxf(ps->stylized_softness, 0.0f), 1.0f), ps->stylized_saturation, fmaxf(ps->stylized_outline, 0.0f) * fmaxf(rs, 0.5f) },
                           { ps->stylized_shadow_tint[0], ps->stylized_shadow_tint[1], ps->stylized_shadow_tint[2], fmaxf(ps->stylized_outline_depth, 0.001f) },
                           { ps->stylized_outline_color[0], ps->stylized_outline_color[1], ps->stylized_outline_color[2],
                             fminf(fmaxf(ps->film_grain, 0.0f), 2.0f) } }; // (w: the film grain's amount)
        memcpy(fr->style, st, sizeof st);
        // (2026-11-10) every style's options: the shadows' denoise, the Video-realistic lens (composite.frag, shadow_split.comp)
        fr->style[3][0] = (float)(ps->shadow_denoise < 0 ? 0 : ps->shadow_denoise > 3 ? 3 : ps->shadow_denoise);
        fr->style[3][1] = fminf(fmaxf(ps->video_distortion, 0.0f), 3.0f), fr->style[3][2] = fminf(fmaxf(ps->video_fringe, 0.0f), 3.0f);
        fr->style[3][3] = fminf(fmaxf(ps->video_halation, 0.0f), 3.0f);
        fr->style[4][0] = fminf(fmaxf(ps->video_vignette, 0.0f), 3.0f), fr->style[4][1] = fr->style[4][2] = fr->style[4][3] = 0.0f;
    }
    fr->grade_lut[2] = (float)view->bokeh_blades; // (A9.4: the iris's blades, 0 round; their rotation, radians)
    fr->grade_lut[3] = view->bokeh_rot_deg * DEG2RAD;
    fr->ups_exposure = 0.0f; // (set with the jitter below when a vendor's upscaler runs)
    float aspect = (float)vw / (float)vh, tanh_ = tanf(view->fov_deg * 0.5f * DEG2RAD);
    const float near_cm = 2.0f;

    // Instances: the persistent scene on the GPU (only what changed is sent; the cull runs there), the selection's
    // slot for the outline mask
    const GpuInstance* dyn_inst = NULL;
    uint32_t ndyn = 0;
    float dmn[3], dmx[3];
    if (!scene_prepare(vk, w, view, slot, &dyn_inst, &ndyn, dmn, dmx)) return false;
    rt_alpha_job_poll(vk); // (the Masked shadow rays' pipelines, once the thread made them)
    ext_job_poll(vk); // (the RE_EXT set, once its thread made it)
    omm_update(vk);
    if (R.toon_rebuild || R.tess_rebuild) { // (Q11) a Toon material sent: the pipelines made again with the RE_EXT ones' RE_TOON twins (once); (Q22) a displaced one: with their tess twins
        R.toon_rebuild = R.tess_rebuild = false;
        vkDeviceWaitIdle(vk->device);
        if (R.pipe_samples) build_msaa_pipelines(vk, R.pipe_samples), update_descriptors(vk);
    }
    if (R.masked_dirty) { // a mesh newly drawn by a Masked material: the cull puts it in the prepass from now on
        vkDeviceWaitIdle(vk->device);
        upload_mesh_info(vk);
        R.masked_dirty = false;
        R.tlas_work = GI_TLAS_BUILD, R.dyn_changed = true; // (its instances not opaque in both TLAS: rt.glsl tests the alpha)
        if (R.any_masked && !R.pipe_rt_alpha && R.pipe_samples) rt_alpha_job_start(vk); // (with the test: on a thread)
    }
    float smn[3], smx[3]; // the casters' bounds: the static ones and the dynamic set's
    for (int k = 0; k < 3; k++) smn[k] = fminf(R.static_mn[k], dmn[k]), smx[k] = fmaxf(R.static_mx[k], dmx[k]);
    R.mask_draws = 0;
    uint32_t nsel = view->selection ? view->selection_count : (view->selected != ENTITY_NONE ? 1u : 0u);
    // (2026-11-25, item 17) the selected actors' own meshes and those of everything hanging from them (CAttach: a Skeletal
    // Mesh's parts and skin, a Blueprint's components, a vehicle's, a Landscape's chunks), each with its actor's id: one
    // outline around the whole actor. A bitset of the selected (thousands selected: no O(n m))
    uint8_t* selbits = nsel ? RE_FRAME_ALLOC(uint8_t, (w->slot_count + 8u) / 8u) : NULL;
    if (selbits) memset(selbits, 0, (w->slot_count + 8u) / 8u);
    R.mask_sel = selbits, R.mask_sel_n = selbits ? w->slot_count : 0u, R.mask_world = w; // (the hair's outline: groom_draw_mask)
    for (uint32_t i = 0; i < nsel && R.mask_draws < MASK_MAX; i++) {
        Entity e = view->selection ? view->selection[i] : view->selected;
        uint32_t s = entity_index(e);
        if (selbits && s < w->slot_count) selbits[s >> 3] |= (uint8_t)(1u << (s & 7u));
        const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
        if (sm && s < R.scene_cap && (R.slot_state[s] & SLOT_ALIVE)) {
            R.mask_slot[R.mask_draws] = s;
            R.mask_mesh[R.mask_draws] = render_mesh_slot(sm->mesh);
            R.mask_id[R.mask_draws] = 2u + s;
            R.mask_draws++;
        }
    }
    for (uint32_t ai = 0; selbits && ai < w->archetype_count && R.mask_draws < MASK_MAX; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_STATIC_MESH);
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && R.mask_draws < MASK_MAX; i++) {
            uint32_t s = entity_index(ents[i]);
            if (s >= R.scene_cap || !(R.slot_state[s] & SLOT_ALIVE)) continue;
            // up its chain (the parent it hangs from, then its owner): the first selected one names the outline
            uint32_t sel = UINT32_MAX;
            Entity cur = ents[i];
            for (int depth = 0; depth < 64 && sel == UINT32_MAX; depth++) { // (a rig's parts hang one from another: pelvis, spine, neck, head... deeper than 8)
                const CAttach* at = ecs_get(w, cur, COMP_ATTACH);
                if (!at) break;
                uint32_t ps = entity_index((Entity)at->parent), os = entity_index((Entity)at->owner);
                if (at->parent && ps < w->slot_count && (selbits[ps >> 3] & (1u << (ps & 7u)))) sel = ps;
                else if (at->owner && os < w->slot_count && (selbits[os >> 3] & (1u << (os & 7u)))) sel = os;
                cur = at->parent ? (Entity)at->parent : (Entity)at->owner;
                if (cur == ents[i]) break;
            }
            if (sel == UINT32_MAX) continue;
            const CStaticMesh* sm = ecs_get(w, ents[i], COMP_STATIC_MESH);
            R.mask_slot[R.mask_draws] = s;
            R.mask_mesh[R.mask_draws] = render_mesh_slot(sm->mesh);
            R.mask_id[R.mask_draws] = 2u + sel;
            R.mask_draws++;
        }
    }
    R.mask_cn = R.mask_c_inst = 0;
    { // (2026-11-25, item 17) a selected Crowd Spawner's agents: each part a draw with every agent as an instance (their
        // slots after the world's, as crowd_prepare lays them: its base + agent x parts + part), the spawner's id
        const CrowdDraw* cd;
        uint32_t nc = selbits ? crowd_draws(&cd) : 0u, cbase = 0;
        for (uint32_t i = 0; i < nc && i < CROWD_MAX_RUNS; i++) {
            uint64_t parts = (uint64_t)cd[i].nagents * cd[i].nparts;
            if (cbase + parts > R.crowd_room) break;
            uint32_t s = entity_index(cd[i].e);
            if (s < w->slot_count && (selbits[s >> 3] & (1u << (s & 7u))) && cd[i].mesh && cd[i].nparts <= 511u) {
                uint32_t h = (2u + s) * 2654435761u; // (mask.frag's hash of the id, done here: its byte rides in the pass)
                // each part's coarsest LOD under half a pixel of error at the crowd's nearest (the spawner's distance less
                // its radius; the part's scale from its baked rows): 30 Mannequins at 15 m whole took 0.2 ms
                const CTransform* ct = ecs_get(w, cd[i].e, COMP_TRANSFORM);
                const CCrowd* cc = ecs_get(w, cd[i].e, COMP_CROWD);
                float dd = 0.0f;
                if (ct)
                    for (int k = 0; k < 3; k++) dd += (ct->location[k] - view->pos[k]) * (ct->location[k] - view->pos[k]);
                dd = fmaxf(sqrtf(dd) - (cc ? cc->radius : 0.0f), 100.0f);
                float px_cm = view->w / (2.0f * dd * tanf(fmaxf(view->fov_deg, 1.0f) * 0.5f * DEG2RAD));
                for (uint32_t p = 0; p < cd[i].nparts && R.mask_cn < MASK_CROWD; p++) {
                    uint32_t ms = render_mesh_slot(cd[i].mesh->parts[p].mesh), lod = 0;
                    const float* rw = &cd[i].baked[(size_t)p * 12];
                    float sc = fmaxf(sqrtf(rw[0] * rw[0] + rw[4] * rw[4] + rw[8] * rw[8]), fmaxf(sqrtf(rw[1] * rw[1] + rw[5] * rw[5] + rw[9] * rw[9]),
                                                                                             sqrtf(rw[2] * rw[2] + rw[6] * rw[6] + rw[10] * rw[10])));
                    for (uint32_t l = ms < R.nmeshes ? R.lod_count[ms] : 0u; l-- > 1u && !lod;)
                        if (R.lods[ms][l].index_count && R.lod_error[ms][l] * sc * px_cm < 0.5f) lod = l;
                    R.mask_c_slot[R.mask_cn] = R.crowd_base + cbase + p;
                    R.mask_c_mesh[R.mask_cn] = ms | lod << 24;
                    R.mask_c_stride[R.mask_cn] = cd[i].nparts;
                    R.mask_c_count[R.mask_cn] = cd[i].nagents;
                    R.mask_c_id[R.mask_cn] = 1u + (h >> 24) % 254u;
                    R.mask_cn++, R.mask_c_inst += cd[i].nagents;
                }
            }
            cbase += (uint32_t)parts;
        }
    }
    R.mask_hl = 0, R.mask_hl_mode = view->highlight_mode & 3u;
    for (int i = 0; i < 2 && R.mask_hl_mode && R.mask_draws < MASK_MAX; i++) { // (after the selection: drawn with their own id)
        Entity e = view->highlight[i];
        const CStaticMesh* sm = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
        uint32_t s = entity_index(e);
        if (sm && s < R.scene_cap && (R.slot_state[s] & SLOT_ALIVE)) {
            R.mask_slot[R.mask_draws] = s;
            R.mask_mesh[R.mask_draws] = render_mesh_slot(sm->mesh);
            R.mask_id[R.mask_draws] = 1u; // (the highlight's id, mask.frag's top one)
            R.mask_draws++, R.mask_hl++;
        }
    }

    // Lights: the directional and sky ones here (a short list lights.c keeps from the change log); the local ones are
    // in lights.c's buffer on the GPU and its grid
    uint32_t nl = 0;
    int shadow_light = -1;
    float far_strength = 1.0f; // (Q21) the shadow-casting sun's Far Shadow (0: off)
    float sun_dir[3] = { -0.55f, -0.35f, 0.76f }; // toward the sun when no directional light exists
    float sun_lux[3] = { 0.0f, 0.0f, 0.0f }; // without one: no sun, a black sky (night)
    float sky_intensity = 0.0f;
    bool have_sun = false;
    float sun_best = -1.0f; // (2026-11-17) the sky's sun: the brightest directional at its Lighting Level's blend (Day and Night stacked)
    uint32_t nglobals;
    const uint32_t* globals = lights_globals(&nglobals);
    for (uint32_t gi = 0; gi < nglobals; gi++) {
        Entity e = ecs_entity_at(w, globals[gi]);
        const CLight* l = ecs_get(w, e, COMP_LIGHT);
        if (!l) continue;
        if (l->type == LIGHT_SKY) {
            const CTransform* lt = view->editor ? ecs_get(w, e, COMP_TRANSFORM) : NULL; // (hidden in the editor: off, as lights.c's)
            if (lt && (lt->flags & TRANSFORM_HIDDEN)) continue;
            if (!l->ext || l->affects_world) sky_intensity += l->intensity * lights_level_weight(w, e); // (a Lighting Level's blend)
            continue;
        }
        if (nl >= MAX_LIGHTS) continue;
        GpuLight* g = &fr->lights[nl];
        if (!lights_gpu(w, e, view->editor, g)) continue;
        // a directional light's lux at the top of the atmosphere: what reaches the ground is times the atmosphere's
        // transmittance toward it (the sunset's red)
        float to_sun[3] = { -g->direction[0], -g->direction[1], -g->direction[2] }, tr_atm[3];
        atmosphere_transmittance(to_sun, view->pos[2], tr_atm);
        for (int k = 0; k < 3; k++) g->radiance[k] *= tr_atm[k];
        if (l->type == LIGHT_DIRECTIONAL) { // the sky's sun: the brightest one, at its level's weight
            float lw = lights_level_weight(w, e), rad[3];
            lights_radiance(l, rad); // the temperature's tint
            float lum = (rad[0] + rad[1] + rad[2]) * lw;
            if (lum > sun_best) {
                for (int k = 0; k < 3; k++) sun_lux[k] = rad[k] * lw, sun_dir[k] = to_sun[k];
                sun_best = lum, have_sun = true;
            }
        }
        if (shadow_light < 0 && l->cast_shadows) {
            shadow_light = (int)nl;
            if (l->type == LIGHT_DIRECTIONAL) far_strength = l->far_shadow ? fminf(fmaxf(l->far_shadow_strength, 0.0f), 1.0f) : 0.0f; // (Q21)
        }
        nl++;
    }
    normalize3(sun_dir);
    // Frame data
    float vp[16] = { 0 }; // column-major: P (reversed infinite Z, horizontal FOV, y down) * V
    for (int k = 0; k < 3; k++) {
        vp[k * 4 + 0] = view->right[k] / tanh_;
        vp[k * 4 + 1] = -view->up[k] * aspect / tanh_;
        vp[k * 4 + 2] = 0.0f;
        vp[k * 4 + 3] = view->fwd[k];
    }
    vp[12] = -dot3(view->right, view->pos) / tanh_;
    vp[13] = dot3(view->up, view->pos) * aspect / tanh_;
    vp[14] = near_cm;
    vp[15] = -dot3(view->fwd, view->pos);
    // An orthographic view (the editor's Top / Front / Left...): x, y the view plane's cm over the half sizes, reversed
    // Z linear from RENDER_ORTHO_BACK behind the point looked at (depth 1) to as far in front of it (0), w = 1
    const bool ortho = view->ortho_half > 0.0f;
    const float oh = ortho ? view->ortho_half : 1.0f, ospan = 2.0f * RENDER_ORTHO_BACK;
    if (ortho) {
        for (int k = 0; k < 3; k++) {
            vp[k * 4 + 0] = view->right[k] / (oh * aspect);
            vp[k * 4 + 1] = -view->up[k] / oh;
            vp[k * 4 + 2] = -view->fwd[k] / ospan;
            vp[k * 4 + 3] = 0.0f;
        }
        vp[12] = -dot3(view->right, view->pos) / (oh * aspect);
        vp[13] = dot3(view->up, view->pos) / oh;
        vp[14] = (RENDER_ORTHO_BACK + dot3(view->fwd, view->pos)) / ospan;
        vp[15] = 1.0f;
    }
    memcpy(fr->view_proj, vp, sizeof vp);
    { // TSR: the projection shifted by a sub-pixel jitter (Halton 2, 3: 8 phases; a point at q drawn at q + jitter, render
      // px); the history's matrices without it; the jittered one inverted (tsr.comp: the depth back to the world)
        float jx = 0.0f, jy = 0.0f;
        if (R.tsr_on) {
            uint32_t ph = R.ups_phases > 8u ? (R.tsr_frame++ % R.ups_phases) + 1u : (R.tsr_frame++ & 7u) + 1u;
            float h2 = 0.0f, h3 = 0.0f, f2 = 0.5f, f3 = 1.0f / 3.0f;
            for (uint32_t i = ph; i; i /= 2, f2 *= 0.5f) h2 += f2 * (float)(i % 2);
            for (uint32_t i = ph; i; i /= 3, f3 /= 3.0f) h3 += f3 * (float)(i % 3);
            jx = h2 - 0.5f, jy = h3 - 0.5f;
            for (int c = 0; c < 4; c++) {
                fr->view_proj[c * 4 + 0] += 2.0f * jx / (float)vw * vp[c * 4 + 3];
                fr->view_proj[c * 4 + 1] += 2.0f * jy / (float)vh * vp[c * 4 + 3];
            }
        }
        memcpy(fr->tsr_vp, vp, sizeof vp);
        memcpy(fr->tsr_prev_vp, R.tsr_have_prev ? R.tsr_prev_vp : vp, sizeof vp);
        mat4_inverse(fr->view_proj, fr->tsr_inv);
        // a cut (the view jumped far: a camera change, a teleport): no history (the clip would hold its colors a while).
        // The turn and the lens by the matrix's rotation and projection terms (unitless); the move by the camera's own
        // position: the translation terms are in cm, and comparing them too made any camera moving over ~0.5 cm a frame
        // a cut (a character's camera: no history while walking, the jittered frames raw, the floor's grid flickered)
        float turned = 0.0f, d[3];
        for (int k = 0; k < 12; k++) turned = fmaxf(turned, fabsf(vp[k] - R.tsr_prev_vp[k]));
        for (int k = 0; k < 3; k++) d[k] = view->pos[k] - R.tsr_prev_pos[k];
        bool cut = !R.tsr_have_prev || turned > 0.5f || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 300.0f * 300.0f;
        memcpy(R.tsr_prev_pos, view->pos, sizeof R.tsr_prev_pos);
        fr->tsr[0] = jx, fr->tsr[1] = jy, fr->tsr[2] = 0.0f, fr->tsr[3] = (R.tsr_reset || cut) ? 1.0f : 0.0f;
        // a vendor's upscaler: the frame lit with the exposure read back (a frame or two late; the composite adds the
        // difference to the adapted one), which DLSS / FSR are told exactly (their history in step with it)
        fr->ups_exposure = R.ups_kind != UPS_TSR && R.stats.auto_exposure > 0.0 ? (float)R.stats.auto_exposure : 0.0f;
        if (R.tsr_on) fr->tsr_size[0] = (float)vw, fr->tsr_size[1] = (float)vh, fr->tsr_size[2] = (float)out_w, fr->tsr_size[3] = (float)out_h;
        else memset(fr->tsr_size, 0, sizeof fr->tsr_size); // (the composite: no TSR, no sharpening of its own)
        memcpy(R.tsr_prev_vp, vp, sizeof vp);
        R.tsr_have_prev = true;
    }

    // The sun's virtual shadow maps (vsm.c): the levels around the camera, cached; this frame's jobs (the levels or the
    // rectangles of them drawn again) and the cull's shadow box around them. Used whenever the sun casts shadows, except
    // when the sun is traced (Ray Traced Shadows with the virtual maps turned off for it)
    VsmSettings vss = { view->gi.vsm_levels, view->gi.vsm_res, view->gi.vsm_taps, view->gi.vsm_full_budget, view->gi.vsm_sun_angle };
    bool sun_traced = view->gi.rt_shadows && gi_available() && !view->gi.vsm_sun;
    if (sun_traced || shadow_light < 0) vss.levels = 0;
    bool vsm_views = false, lsm_views = false;
    if (!vsm_configure(vk, &vss, &vsm_views)) return false;
    // The local lights' shadow maps (lsm.c): wherever their shadows aren't traced (Low, a GPU without ray tracing).
    // REALENGINE_LSM=0: off (the comparison); REALENGINE_LSM_LIGHTS=<n>: n lights with a map (a measure)
    const bool local_traced = view->gi.rt_shadows && gi_available();
    LsmSettings lss = { local_traced ? 0 : view->gi.lsm_lights, view->gi.lsm_atlas, view->gi.lsm_res, view->gi.lsm_taps, view->gi.lsm_redraw };
    {
        const char* e = getenv("REALENGINE_LSM");
        if (e && e[0] == '0') lss.lights = 0;
        const char* n = getenv("REALENGINE_LSM_LIGHTS");
        if (lss.lights && n && atoi(n) > 0) lss.lights = atoi(n), lss.atlas = 4096;
    }
    if (!lsm_configure(vk, &lss, &lsm_views)) return false;
    if (vsm_views || lsm_views) update_descriptors(vk);
    // the unified denoiser wanted (A9.1d): its images made now if missing (before this frame records: the set changes)
    const char* den_env = getenv("REALENGINE_DENOISER");
    bool den_want = R.den_ok && R.den_pipe && R.samples == VK_SAMPLE_COUNT_1_BIT && view->view_mode == VIEW_LIT && gi_available() &&
                    (den_env ? den_env[0] == '1'
                             : project()->image_denoise == 1 ? true // (Project Settings > Rendering > Denoise: On in every style)
                             : project()->image_denoise == 2 ? false
                                                             : render_style() >= RENDER_STYLE_PHOTOREAL);
    if (den_want && (!R.den_alloc || R.den_w < R.width || R.den_h < R.height)) { // (a smaller render fits: the dynamic resolution)
        if (R.den_alloc) den_destroy(vk);
        if (!den_create(vk)) den_want = false;
    }
    if (den_want) {
        if (R.any_ext) ext_pipelines_ensure(vk); // (the extra lobes' plain ones first: the set copies them)
        if (!den_rt_ensure(vk) || !R.den_rt[0]) den_want = false;
    }
    VsmJob vsm_jobs[VSM_MAX_LEVELS];
    float vsm_planes[6][4];
    float lf[3] = { -sun_dir[0], -sun_dir[1], -sun_dir[2] };
    if (shadow_light >= 0) memcpy(lf, fr->lights[shadow_light].direction, sizeof lf);
    {
        float reach = VSM_LEVEL0_CM * (float)(1u << (vss.levels > 0 ? vss.levels - 1 : 0)); // (the last level's side)
        for (int k = 0; k < 3; k++) smn[k] = fmaxf(smn[k], view->pos[k] - reach), smx[k] = fminf(smx[k], view->pos[k] + reach);
    }
    int vsm_njobs = vsm_plan(fr, view->pos, lf, smn, smx, vsm_jobs, vsm_planes);
    memset(fr->shadow_matrix, 0, sizeof fr->shadow_matrix);

    // The GPU cull's inputs: the view's planes (no far one: the projection is infinite), the shadow box's (none without
    // a job this frame: the maps are kept), the radius on screen that picks the LOD and drops an instance
    planes_from(vp, fr->cull_planes, ortho); // (orthographic: its far side too)
    memcpy(fr->shadow_planes, vsm_planes, sizeof vsm_planes);
    fr->cull_params[0] = ortho ? (float)vh * 0.5f / oh : (float)vw * 0.5f / tanh_; // radius / distance -> pixels (orthographic: cm -> pixels)
    fr->cull_params[1] = 0.5f;                       // under half a pixel of radius: not drawn
    fr->cull_params[2] = 48.0f;                      // LOD 0 above 48 px of radius, then x0.4 per level
    fr->cull_params[3] = (float)cull_count();
    { float px = view->gi.lod_error_px > 0.0f ? view->gi.lod_error_px : 1.0f; memcpy(&fr->pad_cull, &px, 4); } // (cull.comp: the LODs' error on screen)
    // the local lights' maps: the lights picked, the faces drawn this frame (their casters added to the cull's shadow list)
    LsmJob lsm_jobs[LSM_MAX_JOBS];
    if (getenv("REALENGINE_LSM_REDRAW")) lsm_invalidate(); // (every face drawn again each frame: the drawing's cost, a measure)
    int lsm_njobs = lsm_plan(fr, slot, view->pos, (const float(*)[4])fr->cull_planes, fr->cull_params[0], lsm_jobs);
    fr->mesh_info = R.mesh_info.addr;
    fr->vis = R.vis.addr;
    fr->materials = mat_buffer_address();
    fr->mat_programs = mat_programs_address();
    fr->mat_params = mat_params_address();
    fr->object_cost = R.object_cost.addr;
    fr->relevance = R.relevance.addr;
    fr->mesh_words = R.morphs.addr;
    fr->gi_range = RE_GI_RANGE;
    fr->visible_main = R.visible_main.addr;
    fr->visible_shadow = R.visible_shadow.addr;
    fr->draws = R.draws.addr;
    fr->ncmd = R.ncmd; // the meshes in use x their LODs (cull.comp's lists are this long)
    fr->visible_late = R.visible_late.addr;
    fr->vis_prev = R.vis_prev.addr;
    fr->hiz = R.hiz.addr;
    fr->hiz_w = (vw + 1) / 2; // level 0: half the view's size
    fr->hiz_h = (vh + 1) / 2;
    fr->hiz_levels = R.hiz_levels;
    // (2: last frame's Hi-Z is there and the view didn't jump: the DAGs' two phases, cull.comp occluded_prev)
    // The DAGs' occlusion only while they draw many triangles (on past 500 k a frame, off under 250 k: what it hides
    // then pays its second pass; the rocks scene draws 55-80 k, where it hid 30% of them for no time and cost ~0.1 ms)
    {
        uint64_t dt = R.dag_tris[0] + R.dag_tris[1];
        R.dag_occ = getenv("REALENGINE_DAG_OCCLUSION") ? true : getenv("REALENGINE_NO_DAG_OCCLUSION") ? false : R.dag_occ ? dt >= 250000u : dt >= 500000u;
    }
    bool hiz_prev = R.dag_occ && R.hiz_have && fr->tsr[3] < 0.5f && !memcmp(R.hiz_vp, fr->tsr_prev_vp, sizeof R.hiz_vp);
    fr->hiz_pad = R.occlusion ? 1u | (hiz_prev ? 2u : 0u) : 0u;
    fr->clusters = R.clusters.addr, fr->cl_work = R.cl_work.addr, fr->cl_idx = R.cl_idx.addr, fr->cl_perm = R.cl_perm.addr;
    fr->cl_entries = R.cl_work.buffer && (R.any_masked || R.any_dag) ? CL_MAX_ENTRIES : 0u, fr->cl_idx_cap = R.cl_idx_cap;
    // (Q17) Nanite Tessellation: the displaced materials' DAGs diced (tess.c; its buffer made on the first need)
    tess_prepare(vk, &view->gi, R.any_dag && R.cl_work.buffer && R.occlusion && R.tess_twins && !R.tess_rebuild, R.any_tess); // (without the DAG path: the base vertices displaced alone)
    tess_fill_frame(fr);
    { // (Q21) the distant view: its settings, the heightfield, an impostor baked (one a frame); its frame's fields
        float tow[3] = { -lf[0], -lf[1], -lf[2] };
        farview_prepare(vk, &view->gi, shadow_light >= 0 && !ortho && far_strength > 0.0f ? tow : NULL);
        bool card = R.imp_card < R.nmeshes;
        // (alpha cut: the prepass. Only once an impostor exists: any_masked turns on the Masked shadow rays' heavier main
        // pass pipelines in every scene, +0.6 ms on the skin scene when the card alone set it)
        if (card && farview_stats()->impostors && !R.mesh_masked[R.imp_card]) R.mesh_masked[R.imp_card] = R.any_masked = R.masked_dirty = true;
        farview_fill_frame(fr, card ? R.imp_card : UINT32_MAX, card ? R.meshes[R.imp_card].vertex_offset : 0);
        fr->far_g[0] = far_strength; // (the sun's Far Shadow Strength)
        const CFog* fog0 = NULL; // (the level's height fog: its Sky Atmosphere Ambient share)
        for (uint32_t ai = 0; ai < w->archetype_count && !fog0; ai++)
            if ((w->archetypes[ai].mask & COMP_BIT(COMP_FOG)) && w->archetypes[ai].count) fog0 = ecs_arch_column(&w->archetypes[ai], COMP_FOG);
        if (fog0) fr->far_g[2] *= fminf(fmaxf(fog0->sky_atmosphere_ambient, 0.0f), 1.0f);
    }

    memcpy(fr->camera_pos, view->pos, sizeof(float) * 3);
    fr->camera_pos[3] = (float)re_time_seconds();
    memcpy(fr->camera_fwd, view->fwd, sizeof(float) * 3);
    fr->camera_fwd[3] = tanh_;
    memcpy(fr->camera_right, view->right, sizeof(float) * 3);
    fr->camera_right[3] = aspect;
    memcpy(fr->camera_up, view->up, sizeof(float) * 3);
    fr->camera_up[3] = ortho ? oh : 0.0f; // (the shaders' orthographic flag: view_ortho in common.glsl)
    memcpy(fr->sun_dir, sun_dir, sizeof(float) * 3);
    fr->sun_dir[3] = have_sun ? 1.0f : 0.0f;
    fr->sun_radiance[3] = (float)shadow_light;
    fr->sky_params[0] = sky_intensity;
    fr->sky_params[1] = (float)(view->gi.shadow_rays > 0 ? view->gi.shadow_rays : 4); // area lights' shadow rays
    memcpy(fr->sun_illuminance, sun_lux, sizeof sun_lux);
    fr->sun_illuminance[3] = ortho ? 0.0f : fmaxf(view->aerial_scale, 0.0f); // the aerial perspective's distance scale (0: off)
    fr->sky = atmosphere_sky_address();
    // The physical camera: exposure = 1 / (1.2 * 2^EV100) (ISO 100, the saturation-based sensitivity), times the
    // compensation; auto exposure finds the EV100 from the scene's mean luminance between its limits
    // (a Post Process Volume adds its compensation and, when it overrides them, sets the auto exposure's limits)
    const PostSettings* ps = view->post;
    float comp = exp2f(view->exposure_comp + (ps ? ps->v[PP_EXPOSURE_COMP] : 0.0f));
    float ev_max = ps && (view->post_overrides >> PP_EXPOSURE_MAX_EV & 1u) ? ps->v[PP_EXPOSURE_MAX_EV] : view->auto_max_ev100;
    float ev_min = ps && (view->post_overrides >> PP_EXPOSURE_MIN_EV & 1u) ? ps->v[PP_EXPOSURE_MIN_EV] : view->auto_min_ev100;
    if (view->terror[5][0] > 0.0f) // (Q13) the horror exposure's EV floor: never adapted brighter than a scene this dark
        ev_min = fmaxf(ev_min, view->terror_ev_floor), ev_max = fmaxf(ev_max, ev_min);
    const float ev_fixed = view->cam_manual_exposure ? view->cam_ev100 : view->exposure_ev100; // (A9.4: a Cine Camera's ISO, shutter, aperture)
    fr->exposure_params[0] = comp / (1.2f * exp2f(ev_fixed));
    fr->exposure_params[1] = comp;
    fr->exposure_params[2] = 1.0f / (1.2f * exp2f(ev_max));
    fr->exposure_params[3] = 1.0f / (1.2f * exp2f(fminf(ev_min, ev_max)));
    fr->post_flags = view->auto_exposure && !view->cam_manual_exposure ? POST_AUTO_EXPOSURE : 0u;
    fr->exposure = R.exposure.addr;
    fr->sky_params[2] = (float)(R.pipe_samples ? R.pipe_samples : 1); // the main pass's MSAA (masked.frag: the cut as coverage)
    fr->sky_params[3] = view->show_grid ? 1.0f : 0.0f;
    fr->viewport[0] = view->x;
    fr->viewport[1] = view->y;
    fr->viewport[2] = 1.0f / (float)vw;
    fr->viewport[3] = 1.0f / (float)vh;
    fr->light_count = nl;
    // the local lights' grid around the camera; traced shadows per pixel: 4 lights at High, 8 at Epic
    { // (REALENGINE_SHADOW_BUDGET with REALENGINE_NO_SHADOW_FILTER: every light traced, the many lights' reference)
        const char* sb = getenv("REALENGINE_SHADOW_BUDGET");
        lights_fill_frame(fr, view->pos, R.scene_count, sb && atoi(sb) > 0 ? (uint32_t)atoi(sb) : view->gi.shadow_rays >= 8 ? 8u : 4u);
    }
    { // the decals in view (M6, decal.c): their boxes and screen rectangles, the tiles' lists made by decal_bin.comp
        uint32_t nd = 0, tx = (vw + DECAL_TILE - 1) / DECAL_TILE, ty = (vh + DECAL_TILE - 1) / DECAL_TILE;
        static int no_decals = -1; // (REALENGINE_NO_DECALS=1: the measure without them)
        if (no_decals < 0) no_decals = getenv("REALENGINE_NO_DECALS") != NULL;
        if (!no_decals && view->ortho_half <= 0.0f && view->view_mode != VIEW_WIREFRAME && R.decal_buf[slot].mapped && R.decal_bin_pipe)
            nd = decals_gather(w, fr->view_proj, vw, vh, view->pos, R.decal_buf[slot].mapped, DECAL_MAX);
        VkDeviceSize ts = (VkDeviceSize)tx * ty * (DECAL_TILE_MAX + 1u) * 4u;
        if (nd && R.decal_tiles[slot].size < ts) { // (this slot's last frame is done: vkctx waited)
            gpu_buf_destroy(vk, &R.decal_tiles[slot]);
            if (!gpu_buf_create(vk, &R.decal_tiles[slot], ts, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false)) nd = 0;
        }
        fr->decals = R.decal_buf[slot].addr, fr->decal_tiles = R.decal_tiles[slot].addr;
        fr->decal_count = nd, fr->decal_tiles_x = tx, fr->decal_tiles_y = ty, fr->decal_pad = 0;
        R.ext_decal = nd != 0; // (2026-11-24: the frames with decals, not from now on)
        if (nd && !R.any_ext) { // (the decals are drawn by the RE_EXT pipelines: made now)
            R.any_ext = true;
            printf("[RealEngine] Decals: the RE_EXT pipelines\n");
        }
    }
    for (int k = 0; k < 3; k++) fr->dyn_box[0][k] = R.dyn_mn[k], fr->dyn_box[1][k] = R.dyn_mx[k];
    cloth_bounds(fr->dyn_box[0], fr->dyn_box[1]); // (the cloths are in the dynamic TLAS too)
    memset(fr->ar, 0, sizeof fr->ar); // AR (render_ar_frame): the picture's corners, on, the shadows' strength
    if (R.ar_on && !view->editor) {
        fr->ar[0][0] = R.ar_corners[0][0], fr->ar[0][1] = R.ar_corners[0][1], fr->ar[0][2] = R.ar_corners[1][0], fr->ar[0][3] = R.ar_corners[1][1];
        fr->ar[1][0] = R.ar_corners[2][0], fr->ar[1][1] = R.ar_corners[2][1], fr->ar[1][2] = R.ar_corners[3][0], fr->ar[1][3] = R.ar_corners[3][1];
        fr->ar[2][0] = 1.0f, fr->ar[2][1] = R.ar_shadow, fr->ar[2][2] = (float)(R.ar_w & 0x7FFFFFFFu), fr->ar[2][3] = (float)R.ar_h;
        if (R.ar_depth_on) {
            fr->ar[3][0] = 1.0f, fr->ar[3][1] = (float)(R.ar_dw & 0x7FFFFFFFu), fr->ar[3][2] = (float)R.ar_dh, fr->ar[3][3] = near_cm;
            for (int k = 0; k < 4; k++) fr->ar[4 + k / 2][(k & 1) * 2] = R.ar_dcorners[k][0], fr->ar[4 + k / 2][(k & 1) * 2 + 1] = R.ar_dcorners[k][1];
        }
    }
    mr_fill(fr, view, slot, near_cm); // (Q25b) mixed reality (mr.glsl)
    fr->view_mode = view->view_mode;
    // composite.frag: bit 0 the outline is drawn, bits 1-2 the highlight's mode (IAR_OUTLINE, IAR_GLOW)
    fr->pad0 = (R.mask_draws || R.mask_cn ? 1u : 0u) | (R.mask_hl ? R.mask_hl_mode << 1 : 0u);

    // Global illumination: the probe grid around the camera, the addresses the hit shaders read
    bool pt = view->view_mode == VIEW_PATH_TRACING && gi_available();
    // (A1) the rays by software: the cache's texels a frame scaled to keep the GI near 2 ms (the time measured, as the
    // dynamic resolution): the heavy scene's foliage on Epic cost 172 ms a frame at the preset's 131K; there it settles
    // slower, the frame keeps its rate. The house (0.5 ms) and the RPG (2.6 ms) keep about all of theirs
    GiSettings gis = view->gi;
    if (gi_sw()) {
        double gms = R.stats.gpu_gi_ms;
        if (gms > 0.05) {
            double f = 2.0 / gms;
            R.sw_gi_scale *= (float)(f < 0.25 ? 0.25 : f > 1.1 ? 1.1 : f); // (down fast: a 170 ms frame is gone in 3 frames; up slowly)
        }
        if (!(R.sw_gi_scale > 0.0f) || R.sw_gi_scale > 1.0f) R.sw_gi_scale = 1.0f;
        if (R.sw_gi_scale < 1.0f / 64.0f) R.sw_gi_scale = 1.0f / 64.0f;
        int per = gis.cache_per_frame > 0 ? gis.cache_per_frame : 32768;
        per = (int)((float)per * R.sw_gi_scale);
        gis.cache_per_frame = per < 1024 ? 1024 : per;
        // (the probes' rays too, 8 at least: below 1/4 the cache's floor alone didn't keep the heavy scene's frame)
        int rays = gis.rays_per_probe > 0 ? gis.rays_per_probe : 32;
        rays = (int)((float)rays * (R.sw_gi_scale * 4.0f < 1.0f ? R.sw_gi_scale * 4.0f : 1.0f));
        gis.rays_per_probe = rays < 8 ? 8 : rays;
    }
    { // (A2) the far probes only where the static scene reaches past the near grid (32 x 32 x 12 probes around the
      // camera): a house fits in it, a village or a landscape doesn't. Their cost elsewhere: 0.3-0.45 ms on Epic
        float sp = gis.probe_spacing > 10.0f ? gis.probe_spacing : 200.0f, hx = 16.0f * sp, hz = 8.0f * sp;
        bool far = R.static_mn[0] < view->pos[0] - hx || R.static_mx[0] > view->pos[0] + hx || R.static_mn[1] < view->pos[1] - hx ||
                   R.static_mx[1] > view->pos[1] + hx || R.static_mn[2] < view->pos[2] - hz || R.static_mx[2] > view->pos[2] + hz;
        gi_set_far_needed(far && R.static_mn[0] <= R.static_mx[0]);
    }
    gi_fill_frame(fr, view->pos, R.frame_counter++, &gis, gi_lightmaps_shown()); // (Q16: a bake in the game shows its lightmaps once it ends)
    if (lsm_enabled() && lsm_stats()->mapped > 0) fr->gi_flags |= GI_LOCAL_SM; // (only when not traced: lsm_configure got 0 lights then)
    // the screen space AO (gtao.comp): one sample (MSAA's alpha is its resolve's weight), the lit views, not the path tracer
    R.gtao_on = view->gi.gtao && R.gtao_pipe && R.samples == VK_SAMPLE_COUNT_1_BIT && !pt && !getenv("REALENGINE_NO_GTAO") &&
                (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY);
    if (R.gtao_on) fr->gi_flags |= GI_GTAO;
    // the sun's contact shadows (A4b, the same pass): where its map alone shades it (no ray for the near casters: the hybrid,
    // or every light traced), one sample, the lit views
    R.contact_on = view->gi.contact_shadows && R.gtao_pipe && R.samples == VK_SAMPLE_COUNT_1_BIT && !pt && !getenv("REALENGINE_NO_CONTACT") &&
                   (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY) &&
                   !(fr->gi_flags & (GI_SUN_HYBRID | GI_RT_SHADOWS | GI_REFLECTIONS)) && fr->sun_radiance[3] >= 0.0f; // (the ray traced variants: no sun share)
    // (A4c) the screen space reflections where no ray traces them, from last frame's copies (none yet: next frame)
    if (R.ssr_want && R.ssr_valid && !pt && !(fr->gi_flags & GI_REFLECTIONS)) fr->gi_flags |= GI_SSR;
    R.async_on = vk->async_ok && (project()->async_compute || getenv("REALENGINE_ASYNC_COMPUTE")) && !getenv("REALENGINE_NO_ASYNC");
    if (R.contact_on) {
        fr->gi_flags |= GI_CONTACT_SHADOWS;
        static bool said;
        if (!said) said = true, printf("[RealEngine] Contact shadows: on (the sun's, in screen space: its map alone shades it here)\n");
    }
    // variable rate shading (vrs.comp): one sample, the lit views, last frame's color there to read
    // (opt-in: Project Settings > Rendering > Variable Rate Shading, or REALENGINE_VRS=1; on by default it raised the flicker
    // tests 0.09 -> 0.13% and leaked light through thin walls: the coarse pixels shade past a triangle's edge)
    R.vrs_on = (view->gi.vrs || project()->vrs || getenv("REALENGINE_VRS")) && R.vrs_built && R.vrs_pipe && R.vrs_img.image && R.samples == VK_SAMPLE_COUNT_1_BIT && !pt &&
               R.hdr.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY);
    // (Q25) VR's fixed foveated rendering: the rate by the distance to the eye's center (no color read: any sample count;
    // its own pipeline: vrs_fov.comp, nothing read; a coarse rate the sample count can't take is lowered by the GPU: VK_KHR_fragment_shading_rate)
    R.fov_level = view->foveation > 0 && R.vrs_built && R.vrs_fov_pipe && R.vrs_img.image && !pt && view->view_mode == VIEW_LIT ? (uint32_t)view->foveation : 0u;
    if (R.fov_level) R.vrs_on = true;
    // the skin's screen space scattering (A9.3a, sss.comp): one sample (MSAA's alpha is its resolve's weight: Cinematic
    // keeps the wrap), the Lit view, a subsurface material sent, not under VRS (a coarse pixel spans both halves of the
    // checkerboard) nor the path tracer. REALENGINE_NO_SSS=1: the wrap everywhere, as before
    R.sss_on = R.any_sss && R.any_ext && R.sss_pipe && R.samples == VK_SAMPLE_COUNT_1_BIT && !pt && !R.vrs_on &&
               view->view_mode == VIEW_LIT && !getenv("REALENGINE_NO_SSS");
    if (R.sss_on && (!R.sss_alloc || R.sss_w < R.width || R.sss_h < R.height)) {
        if (R.sss_alloc) sss_destroy(vk);
        if (!sss_create(vk)) R.sss_on = false;
    }
    if (R.sss_on) fr->gi_flags |= GI_SSS;
    if (vss.levels && vsm_enabled()) { // the sun on its virtual shadow maps (mesh.frag, cull.comp)
        fr->gi_flags |= GI_VSM;
        bool any_full = false;
        for (int j = 0; j < vsm_njobs; j++) any_full |= vsm_jobs[j].full;
        if (!any_full) fr->gi_flags |= GI_VSM_DYN; // (only rectangles: only the dynamic casters drawn, the static ones copied)
    }
    gi_cache_update(vk, w, view->pos, &gis); // the surface cache's charts around the camera
    gi_cache_fill_frame(fr, R.frame_counter);
    fr->cache_nevents = 0; // the surface cache's events still running (a full cycle of its texels each)
    for (uint32_t i = 0; i < R.nevents; i++)
        if (R.frame_no < R.event_until[i]) memcpy(fr->cache_events[fr->cache_nevents++], R.events[i], sizeof R.events[i]);
    weather_fill_frame(fr, w, view->gi.volumetric_fog); // the fog and the clouds (the sky light with them)
    { // the rain and the water bodies (water.c): their time runs with the frames
        double now = re_time_seconds();
        bool first = R.water_frame != R.frame_counter + 1u; // (once a frame: a second view this frame doesn't run it twice)
        float wdt = R.water_time > 0.0 && first ? (float)(now - R.water_time) : 0.0f;
        if (first) R.water_time = now, R.water_frame = R.frame_counter + 1u;
        water_fill_frame(fr, w, wdt, view->editor);
        if (first) cloth_set_view(view->pos, view->editor); // (Q9: the soft bodies simulate in play, sleep far away)
        if (first) cloth_fill(w, wdt, slot); // the cloths (cloth.c): once a frame, into this frame's buffers
        if (first) groom_fill(w, view->pos, view->fov_deg, vw, slot); // the hair in strands (groom.c, A9.3b)
        if (first) fire_fill(w, pt ? 0.0f : wdt, sun_dir, slot); // the fires (fire.c, A14)
        if (first) splat_fill(w, slot); // the Gaussian splats (splat.c, A9.5d)
    }
    gi_fallback_frame(vk, fr, w, view->pos); // (Q27.12) the GI's fallback where nothing else lights (after the water: its slots)
    if (ortho) fr->weather_flags = 0; // (an orthographic view: no fog, clouds or their shadows, as Unreal's)
    fill_post(fr, view, vp, vw, vh, near_cm, pt);
    { // the screen's clouds (weather.comp pass 6): a quarter of the view each way (Epic: half, a quarter of its pixels
      // marched a frame), last frame's view for their history
        const bool half = view->gi.clouds_half;
        // (by the render's size, kept while it stays within 30% of it: the dynamic resolution's 5% steps would start their
        // history over, the clouds blurred for a second; the sky reads them by direction, any size. By the output's size
        // instead cost 0.1 ms more on Epic, its render at 75%)
        uint32_t div = half ? 2u : 4u, sw = (vw + div - 1) / div, sh = (vh + div - 1) / div;
        if (R.cloud_sw && R.cloud_half == half && R.cloud_ow == out_w && R.cloud_oh == out_h && fabsf((float)sw / (float)R.cloud_sw - 1.0f) < 0.3f)
            sw = R.cloud_sw, sh = R.cloud_sh;
        if (sw > 1920) sw = 1920;
        if (sh > 1080) sh = 1080;
        bool same = R.cloud_sw == sw && R.cloud_sh == sh;
        // a march's weight over the history: a time constant of 0.35 s (each pixel marched every other frame). At 15% a
        // march the jitter's noise stayed: the cloud edges flickered (0.46% of the RPG demo's view at sunrise, 3% of it
        // changing > 2% a frame); the clouds are kilometers away and drift slowly, the history holds them
        double now = re_time_seconds(), dt = R.cloud_t > 0.0 ? now - R.cloud_t : 0.1;
        R.cloud_t = now;
        float hw = 1.0f - expf(-4.0f * (float)(dt < 0.1 ? dt : 0.1) / (half ? 0.5f : 0.4f)); // (each pixel every 4th frame, weather.comp pass 6; Epic a longer memory: the bigger steps would flicker)
        bool same_res = same && R.cloud_half == half;
        memcpy(fr->cloud_prev_vp, same_res ? R.cloud_vp : vp, sizeof fr->cloud_prev_vp);
        fr->cloud_scr[0] = (float)sw, fr->cloud_scr[1] = (float)sh, fr->cloud_scr[2] = same_res ? fmaxf(hw, 0.01f) : 0.0f;
        fr->cloud_scr[3] = (fr->weather_flags & 4u) && !pt && view->gi.screen_clouds ? (half ? 2.0f : 1.0f) : 0.0f; // (2: the 4-phase march)
        memcpy(R.cloud_vp, vp, sizeof R.cloud_vp);
        R.cloud_sw = sw, R.cloud_sh = sh, R.cloud_ow = out_w, R.cloud_oh = out_h, R.cloud_half = half;
    }
    fr->gi_params[1] = sky_intensity;
    fr->occluder_count = 0;
    if (view->gi.contact_ao) fill_occluders(fr, dyn_inst, ndyn, view->pos);
    glass_fill(w, fr, view->pos); // (Q4: the glass boxes, the colored shadow without rays)
    if (fr->occluder_count) fr->gi_flags |= GI_CONTACT_AO;
    if (R.pipe_rt_alpha && !getenv("REALENGINE_NO_RT_ALPHA")) fr->gi_flags |= GI_RT_MASKED; // (else the shadow rays stay opaque: the traversal skips the candidates; the env: a measure)
    // the shadow split (the many lights' sampling: mesh.frag): a local light casting shadows in the world, the Lit views,
    // the main pass ray traced
    R.split_on = (fr->gi_flags & GI_RT_SHADOWS) && R.mesh_rt && R.split_pipe && R.split_ok && lights_cast_count() > 0 && !pt &&
                 (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY) && (R.samples == VK_SAMPLE_COUNT_1_BIT || R.split_ms.image) &&
                 !getenv("REALENGINE_NO_SHADOW_FILTER");
    if (R.split_on) fr->gi_flags |= GI_SHADOW_SPLIT;
    else R.ml_reset = true; // (the many lights' history: stale when the split comes back)
    // the MSAA resolve weighted by brightness: the Lit views (the others' colors go out without the encoding)
    R.aa_on = R.samples > VK_SAMPLE_COUNT_1_BIT && R.split_pipe && !pt && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY) &&
              !getenv("REALENGINE_PLAIN_RESOLVE");
    if (R.aa_on) fr->gi_flags |= GI_AA_WEIGHTED;
    // the OIT pass: a Translucent material drawn, the Lit views (the others draw it as it is: oit_record leaves it out)
    R.oit_on = R.any_translucent && R.mesh_tl && !pt && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY) &&
               !getenv("REALENGINE_NO_OIT"); // (off: the Translucent ones not drawn, the measure)
    // the sun's hybrid (vsm.glsl): its map with ray traced shadows on, the near casters' shadows by a ray
    if ((fr->gi_flags & (GI_VSM | GI_RT_SHADOWS)) == (GI_VSM | GI_RT_SHADOWS) && !getenv("REALENGINE_SUN_HYBRID_OFF")) fr->gi_flags |= GI_SUN_HYBRID;
    fr->indices = R.indices.addr;
    fr->instances = R.scene.addr;
    if (pt) { // the path tracer accumulates while the camera, the lights and the actors stay as they were
        uint64_t h = 1469598103934665603ull;
        float cam_w = fr->camera_pos[3];
        fr->camera_pos[3] = 0.0f; // the time: not part of the scene
        // the instances sent this frame (the dynamic set, the selection: they may not have moved) and the scene's edits
        const uint8_t* parts[3] = { (const uint8_t*)fr->camera_pos, (const uint8_t*)fr->lights, (const uint8_t*)R.stage[slot].mapped };
        size_t sizes[3] = { sizeof(float) * 16, sizeof(GpuLight) * nl, sizeof(GpuInstance) * R.nhot };
        for (int p = 0; p < 3; p++)
            for (size_t b = 0; b < sizes[p]; b++) h = (h ^ parts[p][b]) * 1099511628211ull;
        h ^= (uint64_t)vw << 32 ^ vh ^ (uint64_t)(sky_intensity * 1000.0f) << 16;
        h ^= R.scene_version * 0x9E3779B97F4A7C15ull; // a rebuild (a level, an undo)
        if (h != R.pt_hash) R.pt_samples = 0;
        R.pt_hash = h;
        // (the camera itself moved: the shader keeps a shorter history than when only the scene changed)
        uint64_t ch = 1469598103934665603ull;
        for (size_t b = 0; b < sizeof(float) * 16; b++) ch = (ch ^ ((const uint8_t*)fr->camera_pos)[b]) * 1099511628211ull;
        bool cam_moved = ch != R.pt_cam_hash;
        R.pt_cam_hash = ch;
        fr->pad1 = R.pt_samples++ | (cam_moved ? 0x80000000u : 0u);
        fr->camera_pos[3] = cam_w;
    } else {
        R.pt_samples = 0;
    }
    R.stats.pt_samples = R.pt_samples;
    R.stats.unbuilt = gi_lightmaps_present() ? R.unbuilt : 0;

    // On NVIDIA drivers before 617 the recording waits for the last frame here, after the CPU work above (vkctx.c: their
    // background shader compiler crash); elsewhere this returns at once
    vkctx_wait_submitted(vk);
    Push push = { R.frame_dev.addr, R.vertices.addr, R.scene.addr, 0, 0 };
    R.v2_push = push, R.v2_ok = true; // (the second view, after this one: the same frame)
    VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (R.queries) vkCmdResetQueryPool(cmd, R.queries, slot * TS_PER_FRAME, TS_PER_FRAME);
    R.gtao_ts[slot] = false; // (timed once a frame: a second view's AO isn't)
    R.den_ts[slot] = false;
    R.sss_ts[slot] = false;
    R.tess_ts[slot] = false;
    R.far_ts[slot] = false;
    R.bake_rows[slot] = 0;
    R.ups_ts[slot] = false;
    ts(cmd, slot, 0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    vkCmdBindIndexBuffer(cmd, R.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    uint32_t draws = 0;

    // The last frame's exposure (every shader that lights reads it: pre-exposure), then the sky's LUTs when the sun
    // moved (sampled by everything below)
    gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    // ... and the frame's data into device memory (the shaders before are done with the last one: the barrier above)
    VkBufferCopy fc = { 0, 0, sizeof(GpuFrame) };
    vkCmdCopyBuffer(cmd, R.frame[slot].buffer, R.frame_dev.buffer, 1, &fc);
    lsm_upload(cmd, slot); // (the local lights' shadow maps' table)
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    gpu_mark(cmd, "ceu (LUTs da atmosfera)");
    atmosphere_update(cmd, R.set, &push, sun_dir, view->pos[2], fr->sun_illuminance[3] > 0.0f);
    { // (Q21) the terrain's horizon toward the sun, when it turned or the landscapes changed (farshadow.comp)
        gpu_mark(cmd, "visao distante (horizonte)");
        if (R.queries) ts(cmd, slot, 32, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        if (farview_record(cmd) && R.queries) ts(cmd, slot, 33, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT), R.far_ts[slot] = true;
    }
    R.detail_slot[slot] = R.detail && R.queries;
    if (R.detail_slot[slot]) ts(cmd, slot, 8, ALL);
    // 0. GI: the TLAS of every instance, then the probes (a quarter of them traced and updated, or the baked volume)
    fr->tlas = scene_record(vk, cmd, slot, push); // the changes sent, the TLAS, the GPU cull
    gpu_mark(cmd, "AR (imagem da camera)");
    ar_record(vk, cmd, slot); // AR's camera picture into its images (before the main pass)
    mr_record(vk, cmd, slot, !view->stereo_second && view->vr_eye == 0); // (Q25b) mixed reality's boxes and real depth
    if (R.detail_slot[slot]) ts(cmd, slot, 9, ALL);
    gpu_mark(cmd, "clima (nuvens, neblina)");
    weather_record(cmd, R.set, &push, fr); // the clouds (the fog's froxels after the sun's shadow maps: weather_record_fog)
    gpu_mark(cmd, "agua (chuva, ondas, oceano)");
    const bool shared = !view->stereo_second; // (Q25) a VR pair's second eye: the frame's simulations and GI updates done by the first
    if (!pt && shared) water_record(cmd, R.set, &push, fr); // the rain's surface and water, the ripples, the ocean's FFT (water.c)
    gpu_mark(cmd, "tecido (simulacao)");
    if (!pt && shared) cloth_record(cmd, R.set, &push); // the cloths' Vertex Block Descent (cloth.c)
    groom_record(cmd, &push, R.layout); // (the hair grown this frame into its device buffer; the dynamic hair simulated)
    gpu_mark(cmd, "fogo (simulacao)");
    if (!pt && shared) fire_record(cmd, R.set, &push); // the fires' gas, a step (fire.c, A14)
    gpu_mark(cmd, "gaussian splats (ordem)");
    if (!pt) splat_record(cmd, R.set, &push); // the splats culled and sorted for this view (splat.c, A9.5d)
    if (R.detail_slot[slot]) ts(cmd, slot, 10, ALL);
    fr->tlas_dyn = fr->tlas ? R.tlas_dyn : 0;
    ts(cmd, slot, 1, ALL);
    if (!fr->tlas) fr->gi_flags &= ~(GI_RT | GI_REFLECTIONS | GI_RT_SHADOWS); // a baked probe volume is still sampled
    { // (REALENGINE_NO_RT_REFLECTIONS=1: no reflection rays, the sky and the SSR instead: their cost's measure)
        static int nr = -1;
        if (nr < 0) nr = getenv("REALENGINE_NO_RT_REFLECTIONS") != NULL;
        if (nr) fr->gi_flags &= ~GI_REFLECTIONS;
    }
    { // the unified denoiser (A9.1d): the Photoreal styles' (REALENGINE_DENOISER=1: any style, =0: never), the ray traced
      // main pass, the Lit view, one sample (MSAA keeps the separate filters)
        bool was = R.den_on;
        R.den_on = den_want && R.den_alloc && R.den_w >= R.width && R.den_h >= R.height &&
                   (fr->gi_flags & (GI_REFLECTIONS | GI_RT_SHADOWS)) && R.mesh_rt;
        if (R.den_on) fr->gi_flags |= GI_DENOISE;
        { // its two rays a pixel in a checkerboard (ReBLUR's): Low to Epic; REALENGINE_DEN_CHECKER=0 / 1 forces it
            static int dc = -2;
            if (dc == -2) dc = getenv("REALENGINE_DEN_CHECKER") ? atoi(getenv("REALENGINE_DEN_CHECKER")) : -1;
            if (R.den_on && (dc > 0 || (dc < 0 && view->gi.den_checker))) fr->gi_flags |= GI_DEN_CHECKER;
        }
        // (A9.1a) ReSTIR DI on the many lights with it (its reservoirs in the guide); REALENGINE_NO_RESTIR=1: the two picks
        if (!was || fr->tsr[3] > 0.5f) R.den_reset = true;
        // (not on a reset: last frame's guide, the reservoirs read, isn't this view's)
        if (R.den_on && !R.den_reset && !getenv("REALENGINE_NO_RESTIR")) fr->gi_flags |= GI_RESTIR;
        // (2026-11-14) ReSTIR GI's spatial reuse (den_trace.comp); REALENGINE_NO_RESTIR_GI=1: the rays alone, as before
        static int rgi_off = -1;
        if (rgi_off < 0) rgi_off = getenv("REALENGINE_NO_RESTIR_GI") != NULL;
        memset(fr->rgi, 0, sizeof fr->rgi), fr->rgi_info[0] = fr->rgi_info[2] = fr->rgi_info[3] = 0u; // (rgi_info[1]: Q4's glass boxes)
        if (R.den_on && !rgi_off && view->gi.restir_gi && R.rgi[0].addr) {
            fr->rgi[0] = (uint32_t)R.rgi[0].addr, fr->rgi[1] = (uint32_t)(R.rgi[0].addr >> 32);
            fr->rgi_info[0] = 1u;
        }
    }
    gpu_mark(cmd, "GI (sondas)");
    if (!pt && shared) gi_update_probes(cmd, R.set, &push, fr);
    if (R.detail_slot[slot]) ts(cmd, slot, 11, ALL);
    if (!pt && fr->tlas && gi_cache_active()) { // the surface cache: this frame's share of its texels
        fr->gi_flags |= GI_SURFACE_CACHE;
        gpu_mark(cmd, "GI (cache de superficie)");
        if (shared) gi_cache_record(cmd, R.set, &push, fr);
    }
    if (R.detail_slot[slot]) ts(cmd, slot, 12, ALL);
    if (!pt && fr->tlas && fr->occluder_count) { // the contact GI's bounce: the occluders' light, after the probes they read
        fr->gi_flags |= GI_CONTACT_BOUNCE;
        gpu_mark(cmd, "GI (oclusores de contato)");
        if (shared) gi_light_occluders(cmd, R.set, &push, fr);
    }
    if (fr->tlas && gi_baking() && shared) { // Build Lighting, while it runs (Q16: timed, the game's budget measured by it)
        gpu_mark(cmd, "Build Lighting");
        if (R.queries) ts(cmd, slot, 28, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        uint32_t rows = gi_bake_step(cmd, R.set, push);
        if (R.queries) ts(cmd, slot, 29, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        R.bake_rows[slot] = R.queries ? rows : 0;
    }
    if (!pt && shared) gi_lightmaps_blend_step(cmd, R.set, push); // (2026-11-16) the Lighting Levels blended (only when a weight changed)
    ts(cmd, slot, 2, ALL);
    gpu_mark(cmd, "sombras virtuais do sol");

    // 1. The sun's virtual shadow maps: this frame's jobs only (a level drawn whole: its static casters into their own
    // layer, copied, the dynamic ones over them; a dirty rectangle: the static layer copied back, the dynamic ones drawn);
    // the rest of every level is kept from earlier frames. The array stays in GENERAL
    if (vsm_njobs) { // (with the path tracer too: the levels are planned as drawn)
        uint32_t pad_was = push.pad;
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, // (the froxels read them too)
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        { // (masked.frag reads the materials' textures; Q17: mesh.vert the displacement's height, statically: always bound)
            VkDescriptorSet sets_[2] = { R.set, mat_set() };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL);
        }
        const VkPipelineStageFlags2 TESTS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                    COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
        const VkAccessFlags2 DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        const uint32_t nlev = (uint32_t)vsm_levels();
        for (int j = 0; j < vsm_njobs; j++) {
            const VsmJob* job = &vsm_jobs[j];
            const uint32_t L = (uint32_t)job->level;
            // (one pass: into view, cleared or kept, the casters of the mode: 1 static, 2 dynamic)
#define VSM_PASS(view_, clear_, mode_)                                                                                    \
    do {                                                                                                                 \
        VkRenderingAttachmentInfo sd = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };                                   \
        sd.imageView = (view_);                                                                                          \
        sd.imageLayout = VK_IMAGE_LAYOUT_GENERAL;                                                                        \
        sd.loadOp = (clear_) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;                                 \
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;                                                                       \
        sd.clearValue.depthStencil.depth = 1.0f;                                                                         \
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };                                                       \
        ri.renderArea = (VkRect2D){ { (int32_t)job->x, (int32_t)job->y }, { job->w, job->h } };                         \
        ri.layerCount = 1;                                                                                               \
        ri.pDepthAttachment = &sd;                                                                                       \
        vkCmdBeginRendering(cmd, &ri);                                                                                   \
        set_viewport(cmd, (float)job->x, (float)job->y, (float)job->w, (float)job->h);                                  \
        push.pass = 1;                                                                                                   \
        push.pad = L | (uint32_t)(mode_) << 8; /* (mesh.vert: the level's matrix, this job's rectangle, the casters) */  \
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);                                           \
        draws += draw_runs(cmd, (VkDeviceSize)R.ncmd * sizeof(VkDrawIndexedIndirectCommand), R.shadow_pipe, R.shadow_m, NULL, NULL); \
        if ((mode_) == 2) draws += cloth_draw_shadow(cmd, &push, R.layout, R.cloth_shadow, L); /* (the cloths: dynamic) */ \
        vkCmdEndRendering(cmd);                                                                                          \
    } while (0)
            // the static layer's rectangles copied over the level's (what was there before the dynamic casters)
            VkImageCopy cp[4];
            uint32_t ncp = 0;
            if (job->full) {
                VSM_PASS(vsm_static_view(job->level), true, 1); // the static casters into their own layer, cleared
                gpu_barrier(cmd, TESTS, DEPTH_RW, COPY, VK_ACCESS_2_TRANSFER_READ_BIT);
                cp[ncp++] = (VkImageCopy){ { VK_IMAGE_ASPECT_DEPTH_BIT, 0, nlev + L, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, L, 1 }, { 0, 0, 0 }, { job->w, job->h, 1 } };
            } else {
                for (uint32_t i = 0; i < job->nclear; i++) {
                    VkOffset3D o = { (int32_t)job->clear[i][0], (int32_t)job->clear[i][1], 0 };
                    cp[ncp++] = (VkImageCopy){ { VK_IMAGE_ASPECT_DEPTH_BIT, 0, nlev + L, 1 }, o, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, L, 1 }, o, { job->clear[i][2], job->clear[i][3], 1 } };
                }
            }
            vkCmdCopyImage(cmd, vsm_image(), VK_IMAGE_LAYOUT_GENERAL, vsm_image(), VK_IMAGE_LAYOUT_GENERAL, ncp, cp);
            gpu_barrier(cmd, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, TESTS | COPY, DEPTH_RW | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VSM_PASS(vsm_layer_view(job->level), false, 2); // then the dynamic casters over them
#undef VSM_PASS
        }
        push.pad = pad_was;
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    // 1b. The local lights' shadow maps (lsm.c): this frame's faces. The whole ones' static casters into the static layer
    // (their tiles cleared), every face's tile copied from it into the maps, then the dynamic casters drawn over: three
    // steps for all the faces at once (their tiles never overlap), one rendering each. The atlas stays in GENERAL
    R.lsm_ts[slot] = lsm_njobs > 0 && R.queries;
    if (lsm_njobs) {
        ts(cmd, slot, 18, ALL);
        gpu_mark(cmd, "sombras das luzes locais");
        uint32_t pad_was = push.pad;
        const VkPipelineStageFlags2 TESTS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                    COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
        const VkAccessFlags2 DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, TESTS | COPY,
                    DEPTH_RW | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        { // (masked.frag reads the materials' textures; Q17: mesh.vert the displacement's height, statically: always bound)
            VkDescriptorSet sets_[2] = { R.set, mat_set() };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL);
        }
        bool any_full = false;
        for (int j = 0; j < lsm_njobs; j++) any_full |= lsm_jobs[j].full;
        if (any_full) {
            lsm_pass(cmd, &push, lsm_jobs, lsm_njobs, 1, &draws);
            gpu_barrier(cmd, TESTS, DEPTH_RW, COPY, VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        VkImageCopy cp[LSM_MAX_JOBS];
        for (int j = 0; j < lsm_njobs; j++) {
            VkOffset3D o = { (int32_t)lsm_jobs[j].x, (int32_t)lsm_jobs[j].y, 0 };
            cp[j] = (VkImageCopy){ { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1 }, o, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, o, { lsm_jobs[j].size, lsm_jobs[j].size, 1 } };
        }
        vkCmdCopyImage(cmd, lsm_image(), VK_IMAGE_LAYOUT_GENERAL, lsm_image(), VK_IMAGE_LAYOUT_GENERAL, (uint32_t)lsm_njobs, cp);
        gpu_barrier(cmd, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT, TESTS, DEPTH_RW);
        if (R.ndyn > 0 || R.crowd_count > 0) lsm_pass(cmd, &push, lsm_jobs, lsm_njobs, 2, &draws); // (no dynamic actor, no crowd: nothing over)
        push.pad = pad_was;
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | COPY, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        ts(cmd, slot, 19, ALL);
    }
    gpu_mark(cmd, "neblina (froxels)");
    weather_record_fog(cmd, R.set, &push, fr); // (2026-11-19) the fog's froxels: their shafts read this frame's sun maps
    ts(cmd, slot, 3, ALL);
    gpu_mark(cmd, "passe principal (pre-passe, malhas, ceu)"); // (the per-pass timing splits it: gpu_mark_detail below)

    // 2. Main pass: meshes, sky, grid (MSAA resolved into the HDR target); or the path tracer into it
    bool ms = samples != VK_SAMPLE_COUNT_1_BIT;
    VkPipelineStageFlags2 col_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (pt) {
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true);
        static uint32_t s_parity;
        Push ptp = push;
        ptp.pass = 0, ptp.pad = ++s_parity; // (the accumulation's halves swap every frame)
        gi_path_trace(cmd, R.set, &ptp, vw, vh);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    } else {
    // 2a. The prepass (two-phase occlusion): the early list's depth (what was drawn last frame), resolved to one sample
    // (MIN: each pixel's farthest) for the Hi-Z; then every slot in the view tested against it, the late list made.
    // The Wireframe mode draws no prepass (its lines don't hide one another): nothing is occluded there.
    VkPipelineStageFlags2 ds_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags2 ds_rw = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    gpu_transition(cmd, &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, ds_stage | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, ds_stage, ds_rw, true);
    bool occl = R.occlusion;
    bool clusters_on = occl && R.cl_work.buffer && R.any_masked; // (fr->cl_entries: cull.comp pass 0 made their draws)
    bool dag_on = occl && R.cl_work.buffer && R.any_dag; // the opaque meshes' cluster LOD DAGs: chosen now, drawn in the prepass
    if (dag_on) cluster_record(cmd, push, 0);
    bool tess_on = dag_on && tess_active(); // (Q17) the displaced DAGs' triangles diced (tess.comp), drawn by pass 9
    if (tess_on) {
        VkDescriptorSet tsets[2] = { R.set, mat_set() };
        if (R.queries) ts(cmd, slot, 30, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        gpu_mark_detail(cmd, "tessellation");
        tess_record(cmd, R.layout, tsets, push, 0);
        if (R.queries) ts(cmd, slot, 31, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT), R.tess_ts[slot] = true;
        gpu_mark_detail(cmd, "passe principal: pre-passe"); // (the dicing's mark ends here)
    }
    if (occl) {
    // the resolve writes in COLOR_ATTACHMENT_OUTPUT with COLOR_ATTACHMENT_WRITE (a depth resolve too): both covered,
    // or the layout change races it (a GPU fault: the driver went down with it)
    VkPipelineStageFlags2 rs_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | ds_stage;
    VkAccessFlags2 rs_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (ms)
        gpu_transition(cmd, &R.depth_resolve, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, rs_stage, rs_write | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true);
    VkRenderingAttachmentInfo pd = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    pd.imageView = R.depth_ms.view;
    pd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    pd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    pd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    pd.clearValue.depthStencil.depth = 0.0f; // reversed Z
    if (ms) {
        pd.resolveMode = R.depth_resolve_mode;
        pd.resolveImageView = R.depth_resolve.view;
        pd.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo pi = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    pi.renderArea.extent = (VkExtent2D){ vw, vh };
    pi.layerCount = 1;
    pi.pDepthAttachment = &pd;
    vkCmdBeginRendering(cmd, &pi);
    if (view->view_mode != VIEW_WIREFRAME) {
        set_viewport(cmd, 0, 0, (float)vw, (float)vh);
        { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
        push.pass = 0;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        draws += draw_runs(cmd, 0, R.prepass, R.prepass_m, NULL, NULL);
        if (dag_on) {
            push.pass = 6;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            draws += cluster_draws(cmd, VK_NULL_HANDLE, R.prepass, VK_NULL_HANDLE);
            if (tess_on) { // (Q17) the micro triangles: occluders too
                push.pass = 9;
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
                draws += tess_draws(cmd, tess_twin(R.prepass), 0, R.indices.buffer);
            }
            push.pass = 0;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        }
    }
    vkCmdEndRendering(cmd);
    occlusion_record(cmd, slot, push, ms ? &R.depth_resolve : &R.depth_ms);
    R.hiz_have = true, memcpy(R.hiz_vp, fr->tsr_vp, sizeof R.hiz_vp);
    bool dag_late = dag_on && (fr->hiz_pad & 2u); // (the DAGs' second phase: what last frame's depth hid, against this frame's)
    if (clusters_on || dag_late) {
        // the clusters the Hi-Z left: into the depth too (and its resolve), the main pass then shades each pixel once
        cluster_record(cmd, push, 1);
        if (tess_on && dag_late) { // (Q17) the late clusters' triangles diced: the second set
            VkDescriptorSet tsets[2] = { R.set, mat_set() };
            tess_record(cmd, R.layout, tsets, push, 1);
        }
        if (ms) {
            gpu_barrier(cmd, ds_stage, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, ds_stage, ds_rw);
            gpu_transition(cmd, &R.depth_resolve, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, rs_stage, rs_write | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true);
        } else
            gpu_transition(cmd, &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ds_stage, ds_rw, false);
        pd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd, &pi);
        if (view->view_mode != VIEW_WIREFRAME) {
            set_viewport(cmd, 0, 0, (float)vw, (float)vh);
            { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
            push.pass = 6;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            draws += cluster_draws(cmd, clusters_on ? R.prepass_m : VK_NULL_HANDLE, VK_NULL_HANDLE, dag_late ? R.prepass : VK_NULL_HANDLE);
            if (tess_on && dag_late) {
                push.pass = 9;
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
                draws += tess_draws(cmd, tess_twin(R.prepass), 1, R.indices.buffer);
            }
            push.pass = 0;
        }
        vkCmdEndRendering(cmd);
        if (ms) // (as occlusion_record left it: the post reads the prepass's resolve)
            gpu_transition(cmd, &R.depth_resolve, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, rs_stage, rs_write,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    }

    if ((clusters_on || dag_on) && R.cl_read[slot].buffer) { // the clusters' draws, final now: to the stats (a frame late)
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy cc[3] = { { CL_DRAWS_AT, 0, (VkDeviceSize)CL_MAX_ENTRIES * 20 },
                               { CL_DRAWS2_AT, (VkDeviceSize)CL_MAX_ENTRIES * 20, (VkDeviceSize)CL_MAX_ENTRIES * 40 },
                               { 0, (VkDeviceSize)CL_MAX_ENTRIES * 60, 4 } }; // (the header's entry count)
        vkCmdCopyBuffer(cmd, R.cl_work.buffer, R.cl_read[slot].buffer, 3, cc);
    }
    tess_readback(cmd, slot, tess_on); // (Q17: its micro triangles to the stats and the budget, a frame late)
    gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, col_stage,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    if (ms)
        gpu_transition(cmd, &R.color_ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, col_stage, 0, col_stage,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    // the depth after the main pass, one sample (resolved, MIN: each pixel's farthest), for the post pass
    bool depth_out = (R.post_run && !occl) || R.split_on || R.oit_on || R.tsr_on || R.mr_on; // (Q25b: mixed reality's alpha reads it) // (the split's filter, the OIT pass: this frame's depth, the late list's too)
    if (ms && depth_out) // (with the occlusion prepass: its resolve, what was seen last frame, is enough for the post)
        gpu_transition(cmd, &R.depth_resolve, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | ds_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | ds_rw, true);
    // the prepass's depth kept: the main pass shades each pixel once (equal or nearer)
    gpu_transition(cmd, &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, ds_stage | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ds_stage, ds_rw, false);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = ms ? R.color_ms.view : SC->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (ms) {
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = SC->view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = R.depth_ms.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = occl ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; // the prepass's, if any
    // (the resolve happens before the store)
    da.storeOp = !ms && (R.post_run || R.split_on || R.oit_on || R.tsr_on) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 0.0f; // reversed Z
    if (ms && depth_out) {
        da.resolveMode = R.depth_resolve_mode;
        da.resolveImageView = R.depth_resolve.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    // the shadow split's target (mesh.frag o_split): cleared to nothing to add; off, its writes are dropped (no view)
    VkRenderingAttachmentInfo cas[4] = { ca, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO },
                                         { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
    if (R.den_on) { // the denoiser's signals and guide (mesh.frag o_den, o_guide): cleared to nothing of ours
        // (last frame's guide, its ReSTIR reservoirs: written by denoise.comp, read by mesh.frag's RE_DEN variants)
        if (R.den_guide_prev.layout != VK_IMAGE_LAYOUT_GENERAL)
            gpu_transition(cmd, &R.den_guide_prev, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, true);
        else
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        GpuImage* dt[2] = { &R.den, &R.den_guide };
        for (int k = 0; k < 2; k++) {
            gpu_transition(cmd, dt[k], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, col_stage,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
            cas[2 + k].imageView = dt[k]->view;
            cas[2 + k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            cas[2 + k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            cas[2 + k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
    }
    if (R.split_on) {
        GpuImage* st = ms ? &R.split_ms : &R.split;
        if (ms)
            gpu_transition(cmd, &R.split_ms, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, col_stage, 0, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
        gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
        cas[1].imageView = st->view;
        cas[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cas[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        cas[1].storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (ms) {
            cas[1].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            cas[1].resolveImageView = R.split.view;
            cas[1].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    if (R.async_split) { // (A9.7c) the frame so far to the GPU: it runs beside the particles' simulation; the rest waits for it
        R.async_split = false;
        cmd = vkctx_split(vk);
        vkCmdBindIndexBuffer(cmd, R.indices.buffer, 0, VK_INDEX_TYPE_UINT32); // (a new command buffer: its state again)
    }
    if (fr->decal_count) { // the decals' tiles (decal_bin.comp) before the main pass reads them
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.decal_bin_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (fr->decal_tiles_x + 7) / 8, (fr->decal_tiles_y + 7) / 8, 1);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    VkRenderingFragmentShadingRateAttachmentInfoKHR vrsa = { VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
    if (R.vrs_on) { // the rate image from last frame's color (vrs.comp), then bound to the pass (A9.7b)
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        gpu_transition(cmd, &R.vrs_img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, CS,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false); // (kept: last frame's rates)
        // this frame's prepass depth (its edges stay at full rate, the color reprojected through it): read, then back
        // (always in the read layout: the descriptor is checked even where the shader skips it, pass & 1)
        gpu_transition(cmd, &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, ds_stage, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.fov_level ? R.vrs_fov_pipe : R.vrs_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        Push vp_ = push;
        vp_.pad = vk->vrs_texel, vp_.pass = (occl ? 1u : 0u) | (R.fov_level ? 2u | R.fov_level << 8 : 0u); // (Q25: the foveated pattern)
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof vp_, &vp_);
        vkCmdDispatch(cmd, ((vw + vk->vrs_texel - 1) / vk->vrs_texel + 7) / 8, ((vh + vk->vrs_texel - 1) / vk->vrs_texel + 7) / 8, 1);
        gpu_transition(cmd, &R.vrs_img, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR, false);
        gpu_transition(cmd, &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ds_stage, ds_rw, false);
        vrsa.imageView = R.vrs_img.view;
        vrsa.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        vrsa.shadingRateAttachmentTexelSize = (VkExtent2D){ vk->vrs_texel, vk->vrs_texel };
    }
    VkRenderingInfo mi = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    if (R.vrs_on) mi.pNext = &vrsa;
    mi.renderArea.extent = (VkExtent2D){ vw, vh };
    mi.layerCount = 1;
    mi.colorAttachmentCount = R.split_ok ? (R.den_ok ? 4 : 2) : 1;
    mi.pColorAttachments = cas;
    mi.pDepthAttachment = &da;
    if (R.oit_refr.image && R.oit_refr.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && R.oit_refr.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        gpu_transition(cmd, &R.oit_refr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true); // (2026-11-16: binding 84 in the layout the set names, before anything reads the set)
    gpu_mark_detail(cmd, "passe principal: malhas");
    vkCmdBeginRendering(cmd, &mi);
    set_viewport(cmd, 0, 0, (float)vw, (float)vh);
    { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
    push.pass = 0;
    vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    // (2026-11-16) the rain on the surfaces is only in the RE_EXT variants (mesh.frag): those while it rains
    bool rain_now = fr->water_addr[0] != 0;
    if (lights_function_seen() && (!R.any_ext || !R.any_ext_masked)) { // (2026-11-17, Q4) the Light Functions are read there too
        R.any_ext = R.any_ext_masked = true;
        printf("[RealEngine] Light Function: the RE_EXT pipelines (mesh.frag light_fn)\n");
    }
    bool tf_now = lights_toon_force_seen(); // (Q11) a Force Toon light: its step is only in the RE_TOON variants
    if (tf_now && (!R.any_ext || !R.any_ext_masked || !R.toon_spv)) {
        if (!R.toon_spv) R.toon_spv = R.toon_rebuild = true; // (the pipelines made again at the next frame's start)
        R.any_ext = R.any_ext_masked = true;
        printf("[RealEngine] Force Toon light: the RE_TOON pipelines\n");
    }
    if (rain_now && (!R.any_ext || !R.any_ext_masked)) {
        R.any_ext = R.any_ext_masked = true;
        printf("[RealEngine] Rain: the RE_EXT pipelines (the rain on the surfaces is in them)\n");
    }
    TRACE("Render: RE_EXT pipelines", ext_pipelines_request(vk)); // (the first frame a material with extra lobes is sent: on a thread)
    bool wire = view->view_mode == VIEW_WIREFRAME && R.mesh_wire, rt = (fr->gi_flags & (GI_REFLECTIONS | GI_RT_SHADOWS)) && R.mesh_rt;
    // (the Principled's extra lobes: their RE_EXT variants while a material sent has them; the plain ones keep their registers)
    bool lf_now = lights_function_seen(); // (Q4: the Light Functions are in the RE_EXT variants)
    // (2026-11-24) the materials' RE_EXT per mesh: only the runs of the meshes an RE_EXT material draws (MESH_INFO_EXT) take
    // those pipelines, the rest keep the plain ones (before: one such material switched every mesh, ~5-15% of the main
    // pass). The rain, the Light Functions, a Force Toon light and the decals still switch all of them (what they touch
    // isn't known per mesh). REALENGINE_NO_EXT_SPLIT=1: all of them, the measure's "before"
    static int no_split = -1;
    if (no_split < 0) no_split = getenv("REALENGINE_NO_EXT_SPLIT") != NULL;
    bool ext_any = (R.ext_mat || R.ext_decal || rain_now || lf_now || tf_now) && R.mesh_x && (!rt || R.mesh_rt_x);
    bool ext_all = ext_any && (R.ext_decal || rain_now || lf_now || tf_now || no_split);
    VkPipeline runx[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkPipeline main_o, main_m, main_ot, main_mt;
    for (int px = ext_any && !ext_all ? 1 : 0; px >= 0; px--) { // (the RE_EXT runs' set first, then the plain one)
    bool ext = px ? true : ext_all;
    // (Low / no RT: the local lights' maps first, in their own variant; the lobes' then wait for a frame without maps)
    main_o = wire ? R.mesh_wire : rt ? (ext ? R.mesh_rt_x : R.mesh_rt) : (fr->gi_flags & (GI_LOCAL_SM | GI_SSR)) && R.mesh_lsm ? (ext && R.mesh_lsm_x ? R.mesh_lsm_x : R.mesh_lsm) : ext ? R.mesh_x : R.mesh; // (A4c: SSR is in the RE_LSM variant, not the plain one; Q11: with the RE_EXT features, mesh_lsm_x)
    // the masked meshes are always in the prepass (cull.comp): with it, only the pixel it kept is shaded (hair cards
    // dozens deep cost 19 ms when each layer ran the cut and the shading); without it, the cut here
    // (2026-11-10) the masked runs' RE_EXT ones while a Masked / Two Sided material has the lobes (hair, leaves with a coat)
    static int no_extm = -1; // (REALENGINE_NO_MASKED_EXT: the plain masked ones, the measure's "before")
    if (no_extm < 0) no_extm = getenv("REALENGINE_NO_MASKED_EXT") != NULL;
    bool extm = ext && (R.ext_mat_masked || rain_now || lf_now || tf_now) && !no_extm && (rt ? R.mesh_rt_m_x && R.mesh_rt_m_eq_x : R.mesh_m_x && R.mesh_m_eq_x);
    main_m = wire ? VK_NULL_HANDLE : occl ? (rt ? (extm ? R.mesh_rt_m_eq_x : R.mesh_rt_m_eq) : (extm ? R.mesh_m_eq_x : R.mesh_m_eq))
                                                       : (rt ? (extm ? R.mesh_rt_m_x : R.mesh_rt_m) : (extm ? R.mesh_m_x : R.mesh_m));
    bool extmt = extm && R.mesh_rt_m_t_x && R.mesh_rt_m_eq_t_x;
    main_ot = rt && !wire ? (ext && R.mesh_rt_t_x ? R.mesh_rt_t_x : R.mesh_rt_t) : VK_NULL_HANDLE,
    main_mt = rt && !wire ? (occl ? (extmt ? R.mesh_rt_m_eq_t_x : R.mesh_rt_m_eq_t) : (extmt ? R.mesh_rt_m_t_x : R.mesh_rt_m_t)) : VK_NULL_HANDLE;
    if (R.den_on && rt && !wire) { // the denoiser's (den_rt: the same set, writing its targets)
        main_o = ext && R.den_rt[6] ? R.den_rt[6] : R.den_rt[0];
        main_m = occl ? (extm && R.den_rt[9] ? R.den_rt[9] : R.den_rt[2]) : (extm && R.den_rt[8] ? R.den_rt[8] : R.den_rt[1]);
        main_ot = ext && R.den_rt[7] ? R.den_rt[7] : R.den_rt[3];
        main_mt = occl ? (extmt && R.den_rt[11] ? R.den_rt[11] : R.den_rt[5]) : (extmt && R.den_rt[10] ? R.den_rt[10] : R.den_rt[4]);
    }
    if (px) runx[0] = main_o, runx[1] = main_m, runx[2] = main_ot, runx[3] = main_mt;
    }
    // (2026-11-24) the big meshes' clusters and their micro triangles aren't in runs: the RE_EXT set while any material has it
    VkPipeline cl_o = runx[0] ? runx[0] : main_o, cl_m = runx[0] ? runx[1] : main_m, cl_ot = runx[0] ? runx[2] : main_ot,
               cl_mt = runx[0] ? runx[3] : main_mt;

    // the cull's draws, one per mesh and LOD: the early list (already in the depth), then the late one (newly seen)
    memcpy(g_run_x, runx, sizeof g_run_x);
    g_run_count[0] = g_run_count[1] = 0;
    draws += draw_runs(cmd, 0, main_o, main_m, main_ot, main_mt);
    push.pass = 3;
    vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    if (occl) draws += draw_runs(cmd, (VkDeviceSize)R.ncmd * 2 * sizeof(VkDrawIndexedIndirectCommand), main_o, main_m, main_ot, main_mt);
    memset(g_run_x, 0, sizeof g_run_x);
    R.runs_plain = g_run_count[0], R.runs_ext = g_run_count[1];
    if (clusters_on || dag_on) { // the big meshes' seen clusters (the imported pipelines: the terminator's fix)
        push.pass = 6;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        draws += cluster_draws(cmd, clusters_on ? (cl_mt ? cl_mt : cl_m ? cl_m : cl_o) : VK_NULL_HANDLE,
                               dag_on ? (cl_ot ? cl_ot : cl_o) : VK_NULL_HANDLE, dag_on ? (cl_ot ? cl_ot : cl_o) : VK_NULL_HANDLE);
        if (tess_on) { // (Q17) the micro triangles (each set the prepass drew)
            push.pass = 9;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            VkPipeline tw = tess_twin(cl_ot ? cl_ot : cl_o); // (Q22: its tess.vert copy)
            draws += tess_draws(cmd, tw, 0, R.indices.buffer);
            if (occl && (fr->hiz_pad & 2u)) draws += tess_draws(cmd, tw, 1, R.indices.buffer);
        }
    }
    push.pass = 0;
    vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    bool parts = particles_any() && !pt && view->view_mode != VIEW_WIREFRAME;
    if (parts) draws += particles_draw_meshes(cmd, slot, &push, R.layout, R.part_mesh, R.indices.buffer, R.vertices.addr); // (their depth: before the sky)
    if (!pt && view->view_mode != VIEW_WIREFRAME) { // the water bodies (water.c): with their depth, before the sky
        draws += water_draw_surfaces(cmd, &push, R.layout, R.water_surf);
        draws += cloth_draw(cmd, &push, R.layout, R.cloth);
        draws += groom_draw(cmd, &push, R.layout, R.hair_depth, R.hair); // (A9.3b)
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    }
    if (R.detail_slot[slot]) ts(cmd, slot, 13, ALL); // the meshes drawn: the sky and the grid after
    gpu_mark_detail(cmd, "passe principal: ceu e grade");
    // (AR: the camera's picture in its place, sky.frag; with the real depth, ar_occlude.frag: the real things nearer
    // than the virtual ones hide them)
    bool ar_occ = R.ar_on && R.ar_depth_on && R.ar_occlude && !pt && !view->editor && !(R.ar_dw & 0x80000000u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ar_occ ? R.ar_occlude : R.sky);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (R.ar_on && R.ar_nverts && R.ar_plane && !pt && !view->editor && R.ar_vbuf[slot].buffer) { // AR: the real floor's shadows over the picture
        Push ap = push;
        ap.instances = R.ar_vbuf[slot].addr;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ar_plane);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof ap, &ap);
        vkCmdDraw(cmd, R.ar_nverts, 1, 0, 0);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        draws++;
    }
    if (view->show_grid) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.grid);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    draws += 1 + view->show_grid;
    if (!pt && view->view_mode != VIEW_WIREFRAME) { // the rain's drops and splashes, blended over (water.c)
        draws += water_draw_rain(cmd, &push, R.layout, R.rain);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    }
    if (parts) { // the sprites blended over everything (the sky too), tested against the depth
        Push pp = push;
        draws += particles_draw_sprites(cmd, slot, &pp, R.layout, R.part_sprite, R.part_point, R.part_ribbon);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    }
    vkCmdEndRendering(cmd);
    gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    { // the one-sample depth readable after the main pass (the composite binds it; it reads it with post_run)
        VkPipelineStageFlags2 dst = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        gpu_transition(cmd, ms ? &R.depth_resolve : &R.depth_ms, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                       col_stage | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, dst,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2b. The velocity: the actors that moved since last frame, their screen motion where they show (motion blur, TSR, the
    // vendors' upscalers; and (2026-11-15) the denoiser's history: drawn before it, after the main pass)
    if (R.post_vel) {
        gpu_transition(cmd, &R.velocity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
        static uint32_t run_shape[NCMD], run_first[NCMD], run_count[NCMD]; // (run_shape: mesh * MAX_LODS + LOD)
        uint32_t nruns = velocity_upload(vk, slot, run_shape, run_first, run_count);
        VkRenderingAttachmentInfo va = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        va.imageView = R.velocity.view;
        va.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        va.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        va.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo vd = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        vd.imageView = (ms ? R.depth_resolve : R.depth_ms).view;
        vd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vd.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo vri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        vri.renderArea.extent = (VkExtent2D){ vw, vh };
        vri.layerCount = 1;
        vri.colorAttachmentCount = 1;
        vri.pColorAttachments = &va;
        bool crowd_vel = R.crowd_count > 0 && R.crowd_prev_cap && R.velocity_crowd_pipe && !getenv("REALENGINE_NO_CROWD_VEL"); // (the env: a comparison)
        bool hair_vel = groom_moving() && R.velocity_hair_pipe; // (2026-11-15) the hair's own motion, the cloths'
        bool cloth_vel = cloth_count() && R.velocity_cloth_pipe;
        vri.pDepthAttachment = nruns || crowd_vel || hair_vel || cloth_vel ? &vd : NULL;
        vkCmdBeginRendering(cmd, &vri);
        if (nruns) {
            set_viewport(cmd, 0, 0, (float)vw, (float)vh);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.velocity_pipe);
            Push vp_ = push;
            vp_.instances = R.vel_buf[slot].addr;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof vp_, &vp_);
            for (uint32_t r = 0; r < nruns; r++) {
                uint32_t sh = run_shape[r] / MAX_LODS, l = run_shape[r] % MAX_LODS;
                const Mesh* m = sh < R.nmeshes ? &R.lods[sh][l < R.lod_count[sh] ? l : 0] : &R.meshes[0];
                vkCmdDrawIndexed(cmd, m->index_count, run_count[r], m->first_index, m->vertex_offset, run_first[r]);
            }
            draws += nruns;
        }
        if (crowd_vel) { // the crowds' parts: the cull's lists again (early, then late), only theirs emitted
            set_viewport(cmd, 0, 0, (float)vw, (float)vh);
            Push cp = push;
            cp.instances = R.crowd_prev.addr, cp.pad = R.crowd_base, cp.pass = 0;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof cp, &cp);
            draws += draw_runs(cmd, 0, R.velocity_crowd_pipe, R.velocity_crowd_pipe, R.velocity_crowd_pipe, R.velocity_crowd_pipe);
            if (R.occlusion) { // (the late list: only with the occlusion)
                cp.pass = 3;
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof cp, &cp);
                draws += draw_runs(cmd, (VkDeviceSize)R.ncmd * 2 * sizeof(VkDrawIndexedIndirectCommand), R.velocity_crowd_pipe, R.velocity_crowd_pipe,
                                   R.velocity_crowd_pipe, R.velocity_crowd_pipe);
            }
        }
        if (hair_vel) {
            set_viewport(cmd, 0, 0, (float)vw, (float)vh);
            draws += groom_draw_velocity(cmd, &push, R.layout, R.velocity_hair_pipe);
        }
        if (cloth_vel) {
            set_viewport(cmd, 0, 0, (float)vw, (float)vh);
            draws += cloth_draw(cmd, &push, R.layout, R.velocity_cloth_pipe);
        }
        vkCmdEndRendering(cmd);
        gpu_transition(cmd, &R.velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    } else if (R.velocity.layout == VK_IMAGE_LAYOUT_UNDEFINED) // (never drawn: its descriptor's layout, bound statically)
        gpu_transition(cmd, &R.velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
    // 2a. The screen space AO (gtao.comp, A4a): the indirect diffuse share mesh.frag left in the alpha darkened by the
    // horizons the depth shows; the alpha back to 1
    if (R.gtao_on || R.contact_on) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        gpu_mark(cmd, "gtao");
        bool time_it = R.queries && !R.gtao_ts[slot];
        if (time_it) ts(cmd, slot, 20, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT); // (RenderStats.gpu_gtao_ms)
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.gtao_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (vw + 15) / 16, (vh + 15) / 16, 1); // (a thread a 2x2 block)
        if (time_it) ts(cmd, slot, 21, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT), R.gtao_ts[slot] = true;
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2a''. (A4c) the render's color and depth copied for next frame's screen space reflections (where they may run).
    // Not while rays trace the reflections (High and up with RT: nothing reads them; 0.15-0.3 ms of copies at 1440p): the
    // copy starts again, a frame before the SSR, when the rays go
    // (2026-11-16) refractive glass drawn this frame: the copy made anyway (the scene behind it), GI_REFRACT for pass 3
    static int no_refr = -1; // (REALENGINE_NO_REFRACT: the glass drawn without bending, the measure's "before")
    if (no_refr < 0) no_refr = getenv("REALENGINE_NO_REFRACT") != NULL;
    bool refr_now = R.any_refract && R.oit_on && !no_refr && R.ssr_col.image && R.ssr_w == vw && R.ssr_h == vh;
    if (refr_now) fr->gi_flags |= GI_REFRACT;
    static int no_gsh = -1; // (REALENGINE_NO_GLASS_SHADOW: the measure's "before")
    if (no_gsh < 0) no_gsh = getenv("REALENGINE_NO_GLASS_SHADOW") != NULL;
    if (R.any_glass_shadow && R.oit_on && !no_gsh && !pt) fr->gi_flags |= GI_GLASS_SHADOW; // (Q4: rays in the ray traced RE_EXT variants, the boxes in the others)
    // (2026-11-17, Q4) raining: the copy made every frame, the drops of the next one read what is behind them there
    // (rain.frag; water[85].y says last frame's is there at this size)
    bool rain_now = water_rain_intensity() > 0.0f && !getenv("REALENGINE_RAIN_FLAT") && !pt && R.ssr_col.image && R.ssr_w == vw && R.ssr_h == vh;
    fr->water[85][1] = rain_now && R.ssr_valid ? 1.0f : 0.0f;
    static bool rain_said;
    if (fr->water[85][1] > 0.5f && !rain_said)
        rain_said = true, printf("[RealEngine] Rain: the drops read the scene behind them (the scene's copy, rain.frag drop_water)\n");
    if (R.ssr_want && (fr->gi_flags & GI_REFLECTIONS) && !refr_now && !rain_now) R.ssr_valid = false;
    else if ((R.ssr_want || refr_now || rain_now) && R.ssr_col.image && R.ssr_w == vw && R.ssr_h == vh) {
        const VkPipelineStageFlags2 ALLS = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        GpuImage* DS = &R.depth_ms;
        gpu_mark(cmd, "ssr copia");
        VkImageLayout cl = SC->layout, dl = DS->layout;
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALLS, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
        gpu_transition(cmd, DS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALLS, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
        gpu_transition(cmd, &R.ssr_col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        gpu_transition(cmd, &R.ssr_dep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
        VkImageCopy cc = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { vw, vh, 1 } };
        vkCmdCopyImage(cmd, SC->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.ssr_col.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
        cc.srcSubresource.aspectMask = cc.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImage(cmd, DS->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.ssr_dep.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
        gpu_transition(cmd, &R.ssr_col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        gpu_transition(cmd, &R.ssr_dep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        gpu_transition(cmd, SC, cl, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, ALLS, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, false);
        gpu_transition(cmd, DS, dl, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, ALLS, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, false);
        R.ssr_valid = true;
    }
    gpu_mark_detail(cmd, "denoiser / filtro das sombras");
    // 2a'. The weighted resolve turned back (pass 2), then the shadow split filtered (shadow_split.comp): the area
    // lights' penumbra smooth, added to the color
    if (R.den_on && !pt) { // the unified denoiser instead (A9.1d, denoise.comp): the shadows, the diffuse GI, the rough reflections
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bool time_it = R.queries && !R.den_ts[slot];
        if (time_it) ts(cmd, slot, 24, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT), R.den_ts[slot] = true;
        if (R.split_on)
            gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        else if (R.split.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, 0, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
        if (R.split_sum.layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &R.split_sum, VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        for (int k = 0; k < 2; k++)
            if (R.ml_hist[k].layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &R.ml_hist[k], VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        gpu_transition(cmd, &R.den, VK_IMAGE_LAYOUT_GENERAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        gpu_transition(cmd, &R.den_guide, VK_IMAGE_LAYOUT_GENERAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        GpuImage* keep[] = { &R.den_guide_prev, &R.den_hd, &R.den_hs, &R.den_hv, &R.den_ta, &R.den_tb, &R.den_key, &R.den_hst };
        for (size_t k = 0; k < sizeof keep / sizeof keep[0]; k++) // (the first frame: their contents undefined, the reset ignores them)
            if (keep[k]->layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, keep[k], VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        // (a vendor upscaler's pre-exposure: last frame's into this one's, for the histories)
        float ue = fr->ups_exposure, ratio = ue > 0.0f && R.den_prev_exp > 0.0f ? ue / R.den_prev_exp : 1.0f;
        R.den_prev_exp = ue;
        uint32_t ratio_bits;
        memcpy(&ratio_bits, &ratio, 4);
        // REALENGINE_DEN_DEBUG: 1 the diffuse alone, 2 the reflection alone, 3 the visibility, 4 the diffuse history's frames
        const char* dd = getenv("REALENGINE_DEN_DEBUG");
        uint32_t den_dbg = dd ? (uint32_t)atoi(dd) & 15u : 0u;
        { // its rays first (den_trace.comp: what mesh.frag left in the targets, traced with the registers its own)
            gpu_mark_detail(cmd, "denoiser: raios da GI");
            VkDescriptorSet sets_[2] = { R.set, mat_set() };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 2, sets_, 0, NULL);
            push.pass = 0, push.pad = 0;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.den_trace_pipe);
            vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
            if (fr->rgi_info[0]) { // (2026-11-14) ReSTIR GI's spatial reuse: the neighbors' reservoirs (den_trace.comp pass 1)
                gpu_mark_detail(cmd, "denoiser: ReSTIR GI espacial");
                push.pass = 1;
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
                vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
                gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
                push.pass = 0;
            }
        }
        // (last frame's render size, for the history's place: the dynamic resolution changes it. In the push's instances:
        // denoise.comp reads no instance)
        VkDeviceAddress keep_inst = push.instances;
        push.instances = (VkDeviceAddress)((R.den_prev_w ? R.den_prev_w : vw) | ((R.den_prev_h ? R.den_prev_h : vh) << 16));
        R.den_prev_w = vw, R.den_prev_h = vh;
        for (uint32_t pass = 0; pass < 3; pass++) {
            static int no_fix_env = -1; // (Q20: Project Settings > Denoiser History Fix; REALENGINE_NO_HISTORY_FIX, a comparison)
            if (no_fix_env < 0) no_fix_env = getenv("REALENGINE_NO_HISTORY_FIX") != NULL;
            bool no_fix = no_fix_env || !project()->den_history_fix;
            push.pass = pass | (R.den_reset ? 256u : 0u) | (R.split_on ? 512u : 0u) | ((R.den_frame & 1u) << 10) | (no_fix ? 2048u : 0u) | (den_dbg << 12);
            push.pad = ratio_bits;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            static const char* den_names[3] = { "denoiser: pre-blur", "denoiser: historico", "denoiser: pos-blur e cor" };
            gpu_mark_detail(cmd, den_names[pass]);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.den_pipes[pass]);
            vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
        }
        push.pass = 0, push.pad = 0, push.instances = keep_inst;
        R.den_frame++, R.den_reset = false;
        if (R.split_on) R.ml_reset = true; // (the split's own history: stale when it runs again)
        if (time_it) ts(cmd, slot, 25, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    } else if ((R.split_on || R.aa_on) && !pt) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (R.split_on) {
            gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
            gpu_transition(cmd, &R.split_sum, VK_IMAGE_LAYOUT_GENERAL, CS, RW, CS, RW, true);
        } else { // (the decode alone: the shader's other bindings still in the layouts the set names)
            if (R.split.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, 0, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
            if (R.split_sum.layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &R.split_sum, VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        }
        for (int k = 0; k < 2; k++)
            if (R.ml_hist[k].layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &R.ml_hist[k], VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.split_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        for (uint32_t step = R.aa_on ? 0u : 1u; step < (R.split_on ? 3u : 1u); step++) {
            uint32_t pass = step == 0u ? 2u : step - 1u; // (2: the decode; then the split's 0 and 1)
            push.pass = pass;
            push.pad = pass == 1u ? (R.ml_frame & 1u) | (R.ml_reset ? 2u : 0u) : 0u; // (pass 1: the history's parity, none)
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
        }
        push.pass = 0, push.pad = 0;
        if (R.split_on) R.ml_frame++, R.ml_reset = false;
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2a*. The skin's screen space subsurface scattering (sss.comp, A9.3a): after every light is in the color (the
    // denoiser's, the split's), before the translucent ones go over it: the checkerboard's halves put back together, the
    // diffuse one spread by Burley's profile; pass 1 writes the marked tiles back (alpha 1)
    if (R.sss_on && R.sss_alloc && !pt && !getenv("REALENGINE_SSS_RAW")) { // (REALENGINE_SSS_RAW=1: the checkerboard left as it is, a debug view)
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        gpu_mark(cmd, "sss");
        bool time_it = R.queries && !R.sss_ts[slot];
        if (time_it) ts(cmd, slot, 26, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        if (R.sss_clear) { // (its first frame: zeroed; afterwards pass 1 leaves it so)
            gpu_transition(cmd, &R.sss, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, CS, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
            VkClearColorValue zero = { { 0.0f, 0.0f, 0.0f, 0.0f } };
            VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, R.sss.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rr);
            gpu_transition(cmd, &R.sss, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW, false);
            R.sss_clear = false;
        } else if (R.sss.layout != VK_IMAGE_LAYOUT_GENERAL)
            gpu_transition(cmd, &R.sss, VK_IMAGE_LAYOUT_GENERAL, CS, RW, CS, RW, false);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.sss_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        for (uint32_t pass = 0; pass < 2; pass++) {
            push.pass = pass;
            push.pad = view->gi.sss_samples > 0 ? (uint32_t)view->gi.sss_samples : 16u;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
        }
        push.pass = 0, push.pad = 0;
        if (time_it) ts(cmd, slot, 27, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT), R.sss_ts[slot] = true;
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2a**. The fires (fire_render.comp, A14): their gas marched over the opaque scene, before the translucent ones
    if (fire_any() && !pt && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY)) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        gpu_mark(cmd, "fogo");
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW, false);
        fire_render(cmd, R.set, &push, vw, vh);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2a****. (Q11) the outlines (outline.c): the outlined actors' ids, their silhouettes widened by their distance (Auto
    // Adaptive), into the color before the translucent ones and the TSR
    if (R.any_outline && !pt && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY || view->view_mode == VIEW_UNLIT) &&
        view->ortho_half <= 0.0f && !getenv("REALENGINE_NO_OUTLINE"))
        outline_frame(vk, cmd, w, view, slot, &push, vw, vh, SC);
    else outline_frame(vk, cmd, w, view, slot, &push, vw, vh, NULL); // (none: its stats say 0)
    // 2a***. Gaussian Splatting (splat.c, A9.5d): back to front over the opaque scene, its depth tested (read only)
    if (splat_any() && R.splat_pipe && !pt && (view->view_mode == VIEW_LIT || view->view_mode == VIEW_LIGHTING_ONLY)) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        gpu_mark(cmd, "gaussian splats");
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, col_stage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, false);
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = SC->view, ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo sdp = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        sdp.imageView = (ms ? R.depth_resolve : R.depth_ms).view;
        sdp.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        sdp.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, sdp.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo sri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        sri.renderArea.extent = (VkExtent2D){ vw, vh };
        sri.layerCount = 1, sri.colorAttachmentCount = 1, sri.pColorAttachments = &ca, sri.pDepthAttachment = &sdp;
        vkCmdBeginRendering(cmd, &sri);
        set_viewport(cmd, 0, 0, (float)vw, (float)vh);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 1, &R.set, 0, NULL);
        draws += splat_draw(cmd, &push, R.layout, R.splat_pipe);
        vkCmdEndRendering(cmd);
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2a''. Translucent materials (weighted blended OIT): every one drawn in any order into the sums (the split's images,
    // free now), tested against the scene's depth; then over the color: hdr * revealage + mean color * (1 - revealage)
    if (R.oit_on) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        // (the many lights' histories are in the set its shaders bind: GENERAL even when the split never ran, a scene
        // without shadow-casting local lights; a glass material there read them UNDEFINED)
        for (int k = 0; k < 2; k++)
            if (R.ml_hist[k].layout != VK_IMAGE_LAYOUT_GENERAL) gpu_transition(cmd, &R.ml_hist[k], VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, RW, true);
        gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, true);
        gpu_transition(cmd, &R.split_sum, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CS, RW, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, true);
        gpu_transition(cmd, &R.oit_refr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, col_stage,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, true);
        VkRenderingAttachmentInfo oa[3] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO },
                                            { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
        oa[0].imageView = R.split.view, oa[1].imageView = R.split_sum.view, oa[2].imageView = R.oit_refr.view;
        for (int k = 0; k < 3; k++) {
            oa[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            oa[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            oa[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        for (int k = 0; k < 4; k++) oa[1].clearValue.color.float32[k] = 1.0f; // (the revealage and the tint: nothing in front yet)
        VkRenderingAttachmentInfo od = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        od.imageView = (ms ? R.depth_resolve : R.depth_ms).view;
        od.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        od.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        od.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo ori = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ori.renderArea.extent = (VkExtent2D){ vw, vh };
        ori.layerCount = 1;
        ori.colorAttachmentCount = 3;
        ori.pColorAttachments = oa;
        ori.pDepthAttachment = &od;
        vkCmdBeginRendering(cmd, &ori);
        set_viewport(cmd, 0, 0, (float)vw, (float)vh);
        { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
        VkPipeline tp = (fr->gi_flags & (GI_REFLECTIONS | GI_RT_SHADOWS)) && R.mesh_rt_tl ? R.mesh_rt_tl : R.mesh_tl;
        push.pass = 0;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        draws += draw_runs(cmd, 0, VK_NULL_HANDLE, tp, NULL, NULL);
        if (R.occlusion) { // (the late list: the prepass ran)
            push.pass = 3;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            draws += draw_runs(cmd, (VkDeviceSize)R.ncmd * 2 * sizeof(VkDrawIndexedIndirectCommand), VK_NULL_HANDLE, tp, NULL, NULL);
        }
        push.pass = 0;
        vkCmdEndRendering(cmd);
        gpu_transition(cmd, &R.split, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        gpu_transition(cmd, &R.split_sum, VK_IMAGE_LAYOUT_GENERAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        gpu_transition(cmd, &R.oit_refr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS | col_stage,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, RW, false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.split_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        push.pass = 3; // (shadow_split.comp pass 3: the composite)
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
        push.pass = 0;
        gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    // 2c. Temporal super resolution (tsr.comp): the render into the output's HDR target with its history; or a vendor's
    // upscaler (DLSS, FSR, XeSS: upscale_vendor.c) from the render, its depth and the full motion vectors (ups_mv.comp)
    bool ups_done = false;
    bool ups_time = R.tsr_on && R.queries && !R.ups_ts[slot]; // (RenderStats.gpu_upscale_ms: the budget's reconstruction)
    if (ups_time) ts(cmd, slot, 22, ALL);
    if (R.tsr_on) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        fr->tsr[2] = R.post_vel ? 1.0f : 0.0f; // (host-visible frame: read when the GPU runs it)
        if (R.velocity.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            gpu_transition(cmd, &R.velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, CS,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
        if (R.ups_kind != UPS_TSR && R.ups_mv.image) {
            gpu_mark(cmd, "upscaler");
            GpuImage* D = &R.depth_ms; // (one sample: MSAA is off under TSR and the upscalers)
            gpu_transition(cmd, &R.ups_mv, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true);
            gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, RW);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.ups_mv_pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            vkCmdDispatch(cmd, (vw + 7) / 8, (vh + 7) / 8, 1);
            gpu_transition(cmd, &R.ups_mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false);
            VkImageLayout dl = D->layout;
            gpu_transition(cmd, D, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, ALL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
            if (SC->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                gpu_transition(cmd, SC, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, ALL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
            gpu_transition(cmd, &R.hdr, VK_IMAGE_LAYOUT_GENERAL, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ALL,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, true);
            UpsEval ue;
            memset(&ue, 0, sizeof ue);
            ue.cmd = cmd;
            ue.color = (UpsImage){ SC->image, SC->view, SC->format, vw, vh };
            ue.depth = (UpsImage){ D->image, D->view, D->format, vw, vh };
            ue.motion = (UpsImage){ R.ups_mv.image, R.ups_mv.view, R.ups_mv.format, vw, vh };
            ue.output = (UpsImage){ R.hdr.image, R.hdr.view, R.hdr.format, out_w, out_h };
            ue.jitter_x = fr->tsr[0], ue.jitter_y = fr->tsr[1];
            ue.reset = fr->tsr[3] > 0.5f;
            ue.frame_ms = 0.0f; // (FSR: 16.7 ms then; DLSS: unused)
            ue.near_cm = 2.0f, ue.fov_y = R.ups_fov_y;
            ue.pre_exposure = fr->ups_exposure;
            ups_done = ups_vendor_eval(vk, R.ups_kind, &ue);
            // (the runtime's own pipeline, sets and push constants bound: the engine's back for what follows)
            gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            D->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            gpu_transition(cmd, D, dl, ALL, 0, ALL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, false);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 1, &R.set, 0, NULL);
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            if (ups_done) {
                gpu_transition(cmd, &R.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ALL, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS | CS,
                               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
                R.tsr_reset = false; // (a cut passed on; TSR's history is reset when it takes over: the kind changed)
            } else R.ups_kind = UPS_TSR; // (it failed: TSR this frame and on, upscale_vendor.c marked it)
        }
    }
    if (R.tsr_on && !ups_done) {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        for (int k = 0; k < 2; k++)
            if (R.hist[k].layout != VK_IMAGE_LAYOUT_GENERAL)
                gpu_transition(cmd, &R.hist[k], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, CS, RW, true);
        gpu_transition(cmd, &R.hdr, VK_IMAGE_LAYOUT_GENERAL, FS | CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true);
        gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.tsr_pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        Push tp = push;
        tp.pass = R.tsr_frame & 1u;
        // (the history read: [pass]; written: [1 - pass]. Q20: bit 1, last frame's depth copied at this render size)
        bool dep_ok = R.tsr_dep_valid && R.tsr_dep_w == vw && R.tsr_dep_h == vh && R.post_vel;
        if (dep_ok) tp.pass |= 2u;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof tp, &tp);
        vkCmdDispatch(cmd, (out_w + 7) / 8, (out_h + 7) / 8, 1);
        gpu_transition(cmd, &R.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS | CS,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | FS, RW);
        R.tsr_reset = false;
        // (Q20) this frame's depth for the next one's disocclusions: only while an actor moves (what leaves a point
        // uncovered with the camera still; the camera's own motion clips its history tight anyway)
        GpuImage* DS = R.depth_resolve.image ? &R.depth_resolve : &R.depth_ms;
        R.tsr_dep_valid = false;
        if (R.post_vel && R.tsr_dep.image && R.tsr_dep_cap_w >= vw && R.tsr_dep_cap_h >= vh && (R.depth_resolve.image || R.samples == VK_SAMPLE_COUNT_1_BIT)) {
            const VkPipelineStageFlags2 ALLS = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkImageLayout dl = DS->layout;
            gpu_transition(cmd, DS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ALLS, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
            gpu_transition(cmd, &R.tsr_dep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, false);
            VkImageCopy cc = { { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, { 0, 0, 0 }, { vw, vh, 1 } };
            vkCmdCopyImage(cmd, DS->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.tsr_dep.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
            gpu_transition(cmd, &R.tsr_dep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALLS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
            gpu_transition(cmd, DS, dl, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, ALLS, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, false);
            R.tsr_dep_valid = true, R.tsr_dep_w = vw, R.tsr_dep_h = vh;
        }
    }
    if (ups_time) ts(cmd, slot, 23, ALL), R.ups_ts[slot] = true;
    ts(cmd, slot, 4, ALL);
    gpu_mark(cmd, "translucidos e sobreposicoes");
    if (R.detail_slot[slot]) { // each slot's visibility and LOD (vis: cull.comp) for the Debug Viewer's lists
        uint32_t n = R.scene_count;
        if (R.vis_read[slot].size < (VkDeviceSize)n * 4) {
            gpu_buf_destroy(vk, &R.vis_read[slot]); // the frame that used it is done (vkctx_begin_frame waited)
            gpu_buf_create(vk, &R.vis_read[slot], (VkDeviceSize)(n + 1024) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        if (R.vis_read[slot].mapped && n) {
            gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferCopy vc = { 0, 0, (VkDeviceSize)n * 4 };
            vkCmdCopyBuffer(cmd, R.vis.buffer, R.vis_read[slot].buffer, 1, &vc);
        }
        R.vis_read_n[slot] = R.vis_read[slot].mapped ? n : 0;
    }

    // 3. Selection mask (2026-11-25, item 17: skipped while nothing is selected and the mask was left cleared, read-only)
    bool mask_hair = R.mask_sel && groom_any_selected(R.mask_world, R.mask_sel, R.mask_sel_n); // (a selected actor's Groom)
    bool mask_run = R.mask_draws || R.mask_cn || mask_hair || !R.mask_idle;
    if (mask_run) {
    gpu_transition(cmd, &R.mask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, col_stage,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    VkRenderingAttachmentInfo ka = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ka.imageView = R.mask.view;
    ka.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ka.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ka.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ki = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ki.renderArea.extent = (VkExtent2D){ out_w, out_h };
    ki.layerCount = 1;
    ki.colorAttachmentCount = 1;
    gpu_transition(cmd, &R.mask_depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
    VkRenderingAttachmentInfo kd = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    kd.imageView = R.mask_depth.view;
    kd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    kd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    kd.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    kd.clearValue.depthStencil.depth = 0.0f; // reversed Z: far
    ki.pDepthAttachment = &kd;
    ki.pColorAttachments = &ka;
    vkCmdBeginRendering(cmd, &ki);
    set_viewport(cmd, 0, 0, (float)out_w, (float)out_h);
    if (R.mask_draws || R.mask_cn) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.mask_pipe);
        { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); } // (mask.frag: a Masked material's alpha)
        push.pass = 2;
        uint32_t pad_now = UINT32_MAX;
        for (uint32_t i = 0; i < R.mask_draws; i++) { // the selected slots drawn directly (mesh.vert, pass 2)
            if (R.mask_id[i] != pad_now) { // its id (an actor's parts share it; the highlight's: 1)
                Push hp = push;
                hp.pad = pad_now = R.mask_id[i];
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof hp, &hp);
            }
            const Mesh* m = &R.meshes[R.mask_mesh[i]];
            vkCmdDrawIndexed(cmd, m->index_count, 1, m->first_index, m->vertex_offset, R.mask_slot[i]);
        }
        // the crowds' parts: a draw each, its agents as instances (mesh.vert: the slot pad + instance x stride; mask.frag:
        // the id's byte from the pass, bit 31 the crowd's mark)
        for (uint32_t i = 0; i < R.mask_cn; i++) {
            Push hp = push;
            hp.pass = 0x80000000u | R.mask_c_id[i] << 16 | R.mask_c_stride[i];
            hp.pad = R.mask_c_slot[i];
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof hp, &hp);
            uint32_t ms = R.mask_c_mesh[i] & 0xFFFFFFu, lod = R.mask_c_mesh[i] >> 24; // (its LOD for its distance)
            const Mesh* m = lod ? &R.lods[ms][lod] : &R.meshes[ms];
            vkCmdDrawIndexed(cmd, m->index_count, R.mask_c_count[i], m->first_index, m->vertex_offset, 0);
        }
        draws++;
    }
    if (mask_hair) { // (2026-11-25, item 17) the selected actors' hair, inside their outline
        { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
        draws += groom_draw_mask(cmd, &push, R.layout, R.mask_hair_pipe, R.mask_world, R.mask_sel, R.mask_sel_n);
    }
    vkCmdEndRendering(cmd);
    gpu_transition(cmd, &R.mask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, col_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    }
    R.mask_idle = !R.mask_draws && !R.mask_cn && !mask_hair;
    ts(cmd, slot, 5, ALL);
    gpu_mark(cmd, "exposicao");

    // 4. Exposure: with auto exposure the histogram of this frame's HDR target, then the adaptation (manual: the
    // EV100's); the next frame renders with it (pre-exposure), the composite reads both
    {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        gpu_barrier(cmd, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | RW);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.exposure_pipe);
        Push fp = push; // the flicker probe (with the exposure this frame rendered with)
        fp.pass = 2;
        fp.instances = R.flicker[slot].addr;
        // REALENGINE_GPU_FAULT_TEST=1: from frame 400 on the probe writes to an address with no memory: a real device lost,
        // to check the crash report names this pass and the address (notas-dev/servicos.md "Onde a GPU parou")
        static int fault_test = -1;
        if (fault_test < 0) fault_test = getenv("REALENGINE_GPU_FAULT_TEST") != NULL;
        if (fault_test && vk->frame_number > 400) fp.frame = fp.instances = 0xDEAD00000000ull;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof fp, &fp);
        vkCmdDispatch(cmd, 4, 2, 1);
        if (R.flicker_on) { // (Q20) the ghosting probe, a measure only
            fp.pass = 3, fp.instances = R.ghost[slot].addr;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof fp, &fp);
            vkCmdDispatch(cmd, 8, 4, 1);
        }
        if (fr->post_flags & POST_AUTO_EXPOSURE) {
            push.pass = 0;
            vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
            vkCmdDispatch(cmd, ((out_w + 3) / 4 + 15) / 16, ((out_h + 3) / 4 + 15) / 16, 1); // one pixel in 4x4
            gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
        }
        push.pass = 1;
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc = { 64 * 4, 0, 16 };
        vkCmdCopyBuffer(cmd, R.exposure.buffer, R.exposure_read[slot].buffer, 1, &bc);
    }
    ts(cmd, slot, 6, ALL);
    gpu_mark(cmd, "pos-processamento (bloom)");

    // 5. The bloom's chain (a prefilter into level 0, five halvings down, five tent-filtered steps back up); the
    // composite adds it, and gathers the depth of field and the motion blur itself
    {
        VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        if (!R.bloom_general) { // once: every level to GENERAL (it stays there)
            VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = CS;
            b.dstAccessMask = RW;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = R.bloom_img;
            b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, BLOOM_LEVELS, 0, 1 };
            VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &di);
            R.bloom_general = true;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 1, &R.set, 0, NULL);
        if (R.post_bloom) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.bloom_pipe);
            for (int step = 0; step < 2 * BLOOM_LEVELS - 1; step++) {
                // 0: prefilter into 0; 1-5: down into 1-5; 6-10: up into 4-0
                uint32_t kind = step == 0 ? 0u : step < BLOOM_LEVELS ? 1u : 2u;
                uint32_t level = kind == 2u ? (uint32_t)(2 * BLOOM_LEVELS - 2 - step) : (uint32_t)step;
                push.pass = kind;
                push.pad = level;
                vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
                vkCmdDispatch(cmd, (R.bloom_w[level] + 7) / 8, (R.bloom_h[level] + 7) / 8, 1);
                gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, RW);
            }
        }
        push.pass = 0;
        push.pad = 0;
    }
    ts(cmd, slot, 14, ALL);
    gpu_mark(cmd, "cena pronta");

    R.stats.instances = R.scene_count;
    R.stats.draws = draws;
    R.stats.lights = nl + lights_local_count();
    R.stats.grid_items = lights_grid_items(slot, &R.stats.grid_capacity);
    R.stats.width = out_w;
    R.stats.height = out_h;
    R.stats.msaa = (uint32_t)samples;
    R.stats.cpu_ms = (re_time_seconds() - t0) * 1000.0;
    return true;
}

// (A9.4) HDR10: the UI into its own target (the SDR swapchain's format: ImGui's pipeline), laid over by the composite
void render_hdr_set(float paper_nits, float peak_nits) {
    R.hdr_paper = paper_nits > 0.0f ? paper_nits : 200.0f, R.hdr_peak = peak_nits > R.hdr_paper ? peak_nits : 1000.0f;
}
VkFormat render_hdr_ui_format(void) { return VK_FORMAT_B8G8R8A8_UNORM; }
// (the target at the swapchain's size; render_scene makes it before its frame binds anything: the set changes here)
static void hdr_ui_target(VkCtx* vk) {
    if (R.ui.image && R.ui_w == vk->extent.width && R.ui_h == vk->extent.height) return;
    vkDeviceWaitIdle(vk->device);
    if (R.ui.image) gpu_image_destroy(vk, &R.ui);
    R.ui_w = vk->extent.width, R.ui_h = vk->extent.height;
    if (!gpu_image_create(vk, &R.ui, R.ui_w, R.ui_h, render_hdr_ui_format(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, // (dst: the clear below; seen only on a real HDR screen)
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
        memset(&R.ui, 0, sizeof R.ui);
        return;
    }
    VkCommandBuffer c = gpu_begin_once(vk); // (read before its first drawing: cleared to nothing)
    gpu_transition(c, &R.ui, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkClearColorValue z = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(c, R.ui.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &rg);
    gpu_transition(c, &R.ui, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_end_once(vk, c);
    update_descriptors(vk);
}
void render_hdr_ui_begin(VkCtx* vk, VkCommandBuffer cmd) {
    if (!R.ui.image || R.ui_w != vk->extent.width || R.ui_h != vk->extent.height) return; // (made by render_scene)
    gpu_transition(cmd, &R.ui, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = R.ui.view, ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = (VkExtent2D){ R.ui_w, R.ui_h }, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, 0, 0, (float)R.ui_w, (float)R.ui_h);
}
void render_hdr_ui_end(VkCtx* vk, VkCommandBuffer cmd) {
    (void)vk;
    if (!R.ui.image) return;
    vkCmdEndRendering(cmd);
    gpu_transition(cmd, &R.ui, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    R.ui_drawn = true;
}

void render_composite(VkCtx* vk, VkCommandBuffer cmd, const RenderView* view) {
    uint32_t slot = vk->frame;
    if (vk->color_format != R.swap_format) vkDeviceWaitIdle(vk->device), composite_pipelines(vk); // (A9.4: HDR10 on or off)

    set_viewport(cmd, view->x, view->y, (float)R.out_w, (float)R.out_h);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.post_run && R.composite_post ? R.composite_post : R.composite);
    { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
    Push push = { R.frame_dev.addr, R.vertices.addr, R.scene.addr, 3, 0 };
    vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    gpu_mark(cmd, "composicao (tone map)");
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ts(cmd, slot, 7, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    gpu_mark(cmd, "depois da composicao");
    R.queries_valid[slot] = R.queries != VK_NULL_HANDLE;
}

// (2026-11-19) The debug lines and points (debug_draw.h) over the composite, in its pass: this frame's and the kept
// ones copied into this frame slot's buffer (the copy on the workers when large), one instanced draw
typedef struct DdCopy {
    DdPrim* dst;
    const DdPrim* src;
    uint32_t n;
} DdCopy;
static void dd_copy_fn(void* arg, int i) {
    const DdCopy* c = arg;
    uint32_t s = (uint32_t)i * 32768u, e = s + 32768u < c->n ? s + 32768u : c->n;
    memcpy(c->dst + s, c->src + s, sizeof(DdPrim) * (e - s));
}
static bool dd_room(VkCtx* vk, GpuBuf* b, VkDeviceSize need) {
    if (b->size >= need) return true;
    gpu_buf_destroy(vk, b); // (this slot's frame is done: vkctx_begin_frame waited for it)
    VkDeviceSize sz = 1u << 20;
    while (sz < need + need / 4) sz *= 2;
    if (gpu_buf_create(vk, b, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return true;
    memset(b, 0, sizeof *b);
    return false;
}
void render_debug_draw(VkCtx* vk, VkCommandBuffer cmd, const RenderView* view) {
    uint32_t nf = 0, nk = 0, nt = 0;
    const DdPrim* fp = dd_frame_prims(&nf);
    const DdPrim* kp = dd_kept_prims(&nk);
    const DdTrace* tp = dd_frame_traces(&nt);
    const DdTrace* const* bp_;
    const uint32_t* bn_;
    uint32_t nb = dd_trace_blocks(&bp_, &bn_), nown = nt;
    for (uint32_t i = 0; i < nb; i++) nt += bn_[i];
    uint32_t n = nf + nk, slot = vk->frame;
    R.dd_drawn = 0;
    if ((!n && !nt) || !R.dd_pipe || !R.out_w || !R.out_h) return;
    VkDeviceAddress vxy = (VkDeviceAddress)(uint32_t)(view->x > 0.0f ? view->x : 0.0f) | ((VkDeviceAddress)(uint32_t)(view->y > 0.0f ? view->y : 0.0f) << 32);
    if (nt && dd_room(vk, &R.dd_tbuf[slot], (VkDeviceSize)nt * sizeof(DdTrace))) { // the batches' records: 18 vertices each
        DdPrim* dst = (DdPrim*)R.dd_tbuf[slot].mapped; // (32 bytes each, as a primitive)
        for (uint32_t i = 0; i <= nb; i++) { // the frame's own, then the async batches' blocks
            DdCopy c = { dst, (const DdPrim*)(i ? bp_[i - 1] : tp), i ? bn_[i - 1] : nown };
            if (c.n > 65536u) re_parallel_for((int)((c.n + 32767u) / 32768u), dd_copy_fn, &c);
            else if (c.n) memcpy(c.dst, c.src, sizeof(DdTrace) * c.n);
            dst += c.n;
        }
        set_viewport(cmd, view->x, view->y, (float)R.out_w, (float)R.out_h);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.dd_lines ? R.dd_lines : R.dd_pipe);
        { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
        Push push = { R.frame_dev.addr, R.dd_tbuf[slot].addr, vxy | (1ull << 63) | (R.dd_lines ? 1ull << 62 : 0), R.out_w, R.out_h }; // (the top bits: trace records, as lines)
        vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
        gpu_mark(cmd, "debug traces");
        vkCmdDraw(cmd, R.dd_lines ? 8 : 18, nt, 0, 0);
        R.dd_drawn += nt * 3u;
    }
    if (!n) {
        if (R.dd_drawn > R.dd_max) R.dd_max = R.dd_drawn;
        return;
    }
    GpuBuf* b = &R.dd_buf[slot];
    if (!dd_room(vk, b, (VkDeviceSize)n * sizeof(DdPrim))) return;
    DdPrim* dst = b->mapped;
    if (n > 65536u) {
        DdCopy c0 = { dst, fp, nf }, c1 = { dst + nf, kp, nk };
        if (nf) re_parallel_for((int)((nf + 32767u) / 32768u), dd_copy_fn, &c0);
        if (nk) re_parallel_for((int)((nk + 32767u) / 32768u), dd_copy_fn, &c1);
    } else {
        if (nf) memcpy(dst, fp, sizeof(DdPrim) * nf);
        if (nk) memcpy(dst + nf, kp, sizeof(DdPrim) * nk);
    }
    set_viewport(cmd, view->x, view->y, (float)R.out_w, (float)R.out_h);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.dd_pipe);
    { VkDescriptorSet sets_[2] = { R.set, mat_set() }; vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 2, sets_, 0, NULL); }
    Push push = { R.frame_dev.addr, b->addr, vxy, R.out_w, R.out_h };
    vkCmdPushConstants(cmd, R.layout, PUSH_STAGES, 0, sizeof push, &push);
    gpu_mark(cmd, "debug lines");
    vkCmdDraw(cmd, 6, n, 0, 0);
    R.dd_drawn += n;
    n = R.dd_drawn;
    if (n > R.dd_max) R.dd_max = n;
}
uint32_t render_debug_drawn(void) { return R.dd_max > R.dd_drawn ? R.dd_max : R.dd_drawn; } // (the most in a frame so far)

const char* render_scalability_name(int level) {
    static const char* const n[RENDER_SCALABILITY_LEVELS] = { "Low", "Medium", "High", "Epic", "Cinematic" };
    return n[level < 0 ? 0 : level >= RENDER_SCALABILITY_LEVELS ? RENDER_SCALABILITY_LEVELS - 1 : level];
}

void render_scalability(int level, int* msaa, GiSettings* gi) {
    // MSAA: Low to Epic none, the temporal super resolution (tsr_scale below) antialiases (TSR over 4x cost the heavy scene
    // 21.5 ms against 13.9 without it: Unreal's TSR alone too); Cinematic 8x under TSR at the output's size (the coverage's
    // edges kept smooth by the brightness-weighted resolve)
    static const int samples[RENDER_SCALABILITY_LEVELS] = { 1, 1, 1, 1, 8 };
    if (level < 0) level = 0;
    if (level >= RENDER_SCALABILITY_LEVELS) level = RENDER_SCALABILITY_LEVELS - 1;
    // the GI: Low none (the sky light), Medium the probes, High + traced reflections, Epic + rougher reflections
    gi->probes = level >= 1;
    gi->rays_per_probe = level >= 2 ? 64 : 32;
    gi->reflections = level >= 2;
    gi->reflection_roughness = level >= 3 ? 0.5f : 0.3f;
    // the local lights' shadows traced from Medium up (the sun stays on its map below High: vsm_sun). Medium had none: a
    // lamp behind a wall lit the other room (the spec_leak scene: its dark side 315 nits on average, now 0.001); +0.2-0.35
    // ms at 1440p (notas-dev/luz.md "Especular vazando"). Low (and a GPU without ray tracing): still none
    gi->rt_shadows = level >= 1;
    gi->gtao = level >= 2; // (the screen space AO: High and up; Cinematic's MSAA leaves it out: its alpha is the resolve's weight)
    gi->ssr = true; // (A4c: the screen's reflections where no ray traces them: Low, Medium, GPUs without ray tracing)
    gi->contact_shadows = true; // (A4b: the sun's contact shadows in screen space, any preset: off where rays shade the sun (RT shadows: the hybrid or every light traced), so on Low and on GPUs without ray tracing)
    gi->den_checker = level < 4; // (the denoiser's two rays a pixel in a checkerboard: one a frame; Cinematic traces both)
    gi->restir_gi = level >= 2; // (+0.5 ms on the house at Epic: the Low and Medium keep the rays alone)
    gi->sss_samples = level >= 3 ? 16 : level >= 2 ? 12 : level >= 1 ? 8 : 6; // (the skin's scattering, A9.3a: one sample only)
    gi->shadow_rays = level >= 3 ? 8 : 4;
    // where they aren't traced (Low, a GPU without ray tracing in any preset) the local lights' shadow maps (lsm.c): Low
    // 8 lights in a 2048 atlas (faces up to 512, a 2x2 filter, 12 faces redrawn a frame); Medium 16 lights in 4096 (1024,
    // 3x3, 24); High 24 (36); Epic and up 32 (48)
    gi->lsm_lights = level >= 3 ? 32 : level >= 2 ? 24 : level >= 1 ? 16 : 8;
    gi->lsm_atlas = level >= 1 ? 4096 : 2048;
    gi->lsm_res = level >= 1 ? 1024 : 512;
    gi->lsm_taps = level >= 1 ? 9 : 4;
    gi->lsm_redraw = level >= 3 ? 48 : level >= 2 ? 36 : level >= 1 ? 24 : 12;
    // the surface cache (Medium up): 30 cm texels and 4 rays; High 15 cm and 8; Epic 10 cm, twice the texels per frame
    gi->surface_cache = level >= 1;
    gi->cache_rays = level >= 2 ? 8 : 4;
    gi->cache_texel = level >= 3 ? 10.0f : level >= 2 ? 15.0f : 30.0f; // finer than a wall: no light under it
    gi->cache_per_frame = level >= 3 ? 131072 : level >= 2 ? 65536 : 16384;
    gi->volumetric_fog = level >= 1; // Low: the analytic height fog only
    gi->screen_clouds = level >= 2;  // High/Epic: the clouds at the view's resolution (~0.4 ms); Low/Medium: the panorama
    gi->clouds_half = level >= 3;    // Epic: at half the view each way (sugestão #44: a quarter blurred their edges into "clay")
    // the sun's virtual shadow maps: Low 5 levels of 1024 (to 128 m) and a 3x3 PCF; Medium 6 of 2048 (512 m), PCSS 12 taps;
    // High 7 (1 km), 16 taps; Epic 8 (2 km), 24; Cinematic 32. Two levels drawn whole a frame from High
    gi->vsm_levels = level >= 3 ? 8 : level >= 2 ? 7 : level >= 1 ? 6 : 5;
    gi->vsm_res = level >= 1 ? 2048 : 1024;
    gi->vsm_taps = level >= 4 ? 32 : level >= 3 ? 24 : level >= 2 ? 16 : level >= 1 ? 12 : 4;
    gi->vsm_full_budget = level >= 2 ? 2 : 1;
    if (gi->vsm_sun_angle <= 0.0f) gi->vsm_sun_angle = 0.5357f; // (Unreal's Source Angle: the real sun's disk)
    { const char* e = getenv("REALENGINE_VSM"); gi->vsm_sun = !(e && e[0] == '0'); } // (REALENGINE_VSM=0: the sun traced, the comparison)
    if (level >= 4) { // Cinematic (the Quality mode): the penumbras', the cache's rays doubled, its texels finer, reflections rougher
        gi->shadow_rays = 16;
        gi->cache_rays = 16;
        gi->cache_texel = 7.5f;
        gi->cache_per_frame = 262144;
        gi->reflection_roughness = 0.8f;
    }
    // temporal super resolution (tsr.comp): the share of the output rendered (Unreal's screen percentage with TSR); Cinematic
    // at the output's size (the temporal AA alone, over its MSAA)
    gi->tsr_scale = level >= 4 ? 1.0f : level >= 3 ? 0.75f : level >= 2 ? 0.67f : level >= 1 ? 0.6f : 0.5f;
    gi->lod_error_px = level >= 4 ? 0.5f : level >= 3 ? 1.0f : level >= 2 ? 2.0f : level >= 1 ? 4.0f : 8.0f;
    // (Q17) Nanite Tessellation: the displaced materials' DAGs diced to px per micro edge, at most so many per base edge,
    // under a budget of micro triangles (millions) a frame; Low displaces the base vertices alone (nothing diced)
    gi->tessellation = true;
    // (the raster's own triangles: 2 px edges, Nanite's software rate, cost +3.3 ms on the rocks scene, 4 px +1.5: notas-dev/tessellation.md)
    gi->tess_px = level >= 4 ? 2.0f : level >= 3 ? 4.0f : level >= 2 ? 6.0f : 10.0f;
    gi->tess_max = level >= 4 ? 32 : level >= 3 ? 16 : level >= 2 ? 8 : level >= 1 ? 4 : 1;
    gi->tess_budget = level >= 4 ? 12.0f : level >= 3 ? 4.0f : level >= 2 ? 2.0f : level >= 1 ? 1.0f : 0.5f;
    // (Q21) the distant view (farview.c): the terrain's far shadow in every preset (a heightfield of 512 texels on Low, 1024
    // up to Epic, 2048 Cinematic); the impostors: the far instances under 32 / 28 / 24 / 20 / 16 px of screen radius drawn
    // by their card, atlases of 512 (Low, Medium: 64 px frames) or 1024 (128 px); the fog's ambient from the sky's horizon
    gi->far_shadow = true;
    gi->far_res = level >= 4 ? 2048 : level >= 1 ? 1024 : 512;
    gi->impostors = true;
    gi->impostor_px = level >= 4 ? 16.0f : level >= 3 ? 20.0f : level >= 2 ? 24.0f : level >= 1 ? 28.0f : 32.0f;
    gi->impostor_res = level >= 2 ? 1024 : 512;
    gi->fog_sky = 1.0f;
    if (gi->probe_spacing <= 0.0f) gi->probe_spacing = 200.0f;
    if (gi->intensity <= 0.0f) gi->intensity = 1.0f;
    *msaa = samples[level];
    { const char* e = getenv("REALENGINE_MSAA"); if (e && atoi(e) > 0) *msaa = atoi(e); } // (a measure's override)
}

double render_flicker(void) { return R.flicker_frames ? R.flicker_sum / R.flicker_frames : 0.0; }
double render_flicker_spots(void) { return R.flicker_frames ? R.flicker_spots / R.flicker_frames : 0.0; }
double render_flicker_pulses(void) { return R.flicker_frames > 1 ? R.flicker_pulses / (R.flicker_frames - 1) : 0.0; }
void render_probe_lum(double* max_nits, double* mean_nits) {
    *max_nits = R.probe_max;
    *mean_nits = R.probe_frames ? R.probe_mean_sum / R.probe_frames : 0.0;
}
void render_flicker_reset(void) {
    R.flicker_on = true;
    R.probe_max = R.probe_mean_sum = 0.0, R.probe_frames = 0;
    R.flicker_sum = R.flicker_spots = R.flicker_pulses = 0.0;
    memset(R.flicker_map, 0, sizeof R.flicker_map);
    R.flicker_frames = 0;
    R.ghost_have = false;
    R.ghost_num = R.ghost_den = R.bg_num = R.bg_den = R.ghost_samples = R.ghost_cov = 0.0, R.ghost_frames = 0;
    memset(R.ghost_age, 255, sizeof R.ghost_age);
}
// (Q20) the trail behind moving actors: the mean relative change, frame to frame, of the points a mover left 2-8 frames
// ago (as the flicker's: 2 |a - b| / (a + b)); the background's, the points no mover touched for 255 frames; how many
// such trail points a frame. Zero without a mover in the view
double render_ghosting(double* background, double* samples_per_frame) {
    if (background) *background = R.bg_den > 0.0 ? 2.0 * R.bg_num / R.bg_den : 0.0;
    if (samples_per_frame) *samples_per_frame = R.ghost_frames ? R.ghost_samples / R.ghost_frames : 0.0;
    if (getenv("REALENGINE_GHOST_DEBUG")) printf("[RealEngine] ghost probe: %.0f covered points in %u frames, post_vel %d, nvel %u\n", R.ghost_cov, R.ghost_frames, (int)R.post_vel, R.nvel);
    return R.ghost_den > 0.0 ? 2.0 * R.ghost_num / R.ghost_den : 0.0;
}
const uint32_t* render_flicker_map(uint32_t* frames) {
    *frames = R.flicker_frames;
    return R.flicker_map;
}

bool render_slot_moving(uint32_t slot) {
    return R.moved_at && slot < R.scene_cap && R.moved_at[slot] && R.frame_no - R.moved_at[slot] < 60u;
}

bool render_shape_cpu(uint32_t shape, uint32_t lod, const void** v, uint32_t* nverts, const uint32_t** idx, uint32_t* nidx) {
    if (shape >= R.nmeshes || !R.cpu_v || !R.cpu_i) return false;
    if (lod >= R.lod_count[shape]) lod = R.lod_count[shape] - 1;
    const Mesh* m = &R.lods[shape][lod];
    // its vertices run to the next mesh's (build_meshes adds them in order) or to the end
    uint32_t end = R.vertex_count;
    for (uint32_t s = 0; s < R.nmeshes; s++)
        for (uint32_t l = 0; l < R.lod_count[s]; l++)
            if (R.lods[s][l].vertex_offset > m->vertex_offset && (uint32_t)R.lods[s][l].vertex_offset < end) end = (uint32_t)R.lods[s][l].vertex_offset;
    *v = R.cpu_v + m->vertex_offset;
    *nverts = end - (uint32_t)m->vertex_offset;
    *idx = R.cpu_i + m->first_index;
    *nidx = m->index_count;
    return true;
}

// --- The Debug Viewer (debug_viewer.c): finer measures only while it is open ---

void render_set_detail(bool on) { R.detail = on; }

void render_request_vis(void) { R.vis_want = true; }

const uint32_t* render_debug_vis(uint32_t* n) {
    *n = R.vis_cpu ? R.vis_n : 0;
    return R.vis_cpu;
}

uint32_t render_lod_triangles(uint32_t shape, uint32_t lod) {
    if (shape >= R.nmeshes || lod >= R.lod_count[shape]) return 0;
    return R.lods[shape][lod].index_count / 3;
}

// --- The second 3D view (view2.comp): its parameters and its picture per frame slot (one written while the last frame's
// is still drawn), a buffer the shader writes copied into an RGBA8 image the UI samples ---

static struct {
    GpuBuf params[RE_FRAMES_IN_FLIGHT], out[RE_FRAMES_IN_FLIGHT], accum;
    GpuImage img;
    uint32_t w, h, gen, samples, frame;
    uint64_t hash;
} V2;

VkImageView render_view2_image(uint32_t* generation) {
    if (generation) *generation = V2.gen;
    return V2.img.view;
}
uint32_t render_view2_samples(void) { return V2.samples; }
uint32_t render_view2_frames(void) { return V2.frame; }

static void view2_free(VkCtx* vk) {
    gpu_image_destroy(vk, &V2.img);
    gpu_buf_destroy(vk, &V2.accum);
    for (uint32_t i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &V2.out[i]), gpu_buf_destroy(vk, &V2.params[i]);
    memset(&V2, 0, sizeof V2);
}

bool render_view2(VkCtx* vk, VkCommandBuffer cmd, const View2* v) {
    if (!gi_available() || !R.v2_ok || v->w < 8 || v->h < 8) return false;
    uint32_t slot = vk->frame;
    if (v->w != V2.w || v->h != V2.h || !V2.img.image) { // (a new size: made again, the GPU idle)
        vkDeviceWaitIdle(vk->device);
        gpu_image_destroy(vk, &V2.img);
        gpu_buf_destroy(vk, &V2.accum);
        bool ok = gpu_image_create(vk, &V2.img, v->w, v->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
                  gpu_buf_create(vk, &V2.accum, (VkDeviceSize)v->w * v->h * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        for (uint32_t i = 0; ok && i < RE_FRAMES_IN_FLIGHT; i++) {
            gpu_buf_destroy(vk, &V2.out[i]);
            ok = gpu_buf_create(vk, &V2.out[i], (VkDeviceSize)v->w * v->h * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
            if (ok && !V2.params[i].buffer) ok = gpu_buf_create(vk, &V2.params[i], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        }
        if (!ok) return V2.w = V2.h = 0, false;
        V2.w = v->w, V2.h = v->h, V2.gen++, V2.samples = 0, V2.hash = 0;
        VkCommandBuffer once = gpu_begin_once(vk);
        gpu_transition(once, &V2.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
        gpu_end_once(vk, once);
    }
    // the path tracer starts over when the view or the main view's scene changed (its own count otherwise)
    uint64_t h = 1469598103934665603ull;
    for (size_t b = 0; b < sizeof *v; b++) h = (h ^ ((const uint8_t*)v)[b]) * 1099511628211ull;
    h ^= R.pt_hash * 0x9E3779B97F4A7C15ull ^ R.scene_version;
    if (h != V2.hash || v->mode != VIEW2_PATH_TRACING) V2.samples = 0;
    V2.hash = h;
    struct { float pos[4], fwd[4], right[4], up[4]; uint32_t size[4]; VkDeviceAddress out, accum; } P;
    float t = tanf(fminf(fmaxf(v->fov_deg, 5.0f), 170.0f) * 0.5f * 0.0174533f), aspect = (float)v->w / (float)v->h;
    memcpy(P.pos, v->pos, 12), P.pos[3] = t / aspect; // (the vertical half-angle's tangent: the fov is horizontal)
    memcpy(P.fwd, v->fwd, 12), P.fwd[3] = v->ortho_half;
    memcpy(P.right, v->right, 12), P.right[3] = aspect;
    memcpy(P.up, v->up, 12), P.up[3] = (float)v->mode;
    P.size[0] = v->w, P.size[1] = v->h, P.size[2] = V2.samples, P.size[3] = ++V2.frame;
    P.out = V2.out[slot].addr, P.accum = V2.accum.addr;
    memcpy(V2.params[slot].mapped, &P, sizeof P);
    if (v->mode == VIEW2_PATH_TRACING) V2.samples++;
    Push push = R.v2_push;
    push.pass = (uint32_t)V2.params[slot].addr, push.pad = (uint32_t)(V2.params[slot].addr >> 32);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    gi_view2(cmd, R.set, &push, v->w, v->h);
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    gpu_transition(cmd, &V2.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy rg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { v->w, v->h, 1 } };
    vkCmdCopyBufferToImage(cmd, V2.out[slot].buffer, V2.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    gpu_transition(cmd, &V2.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    return true;
}

// The object cost view's values (VIEW_OBJECT_COST, editor_cost.c): a float per scene slot (an entity's index; the
// crowds' after them), 0 green .. 1 red, written by the CPU a few times a second while the view is on. Made the first
// time it is asked for (host-visible, coherent: the next frames read what was written); NULL when it couldn't be.
float* render_object_cost_map(VkCtx* vk, uint32_t* count) {
    uint32_t n = RE_ECS_MAX_ENTITIES + CROWD_MAX_INSTANCES;
    if (!R.object_cost.buffer && !gpu_buf_create(vk, &R.object_cost, (VkDeviceSize)n * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
        memset(&R.object_cost, 0, sizeof R.object_cost);
        return NULL;
    }
    if (count) *count = n;
    return R.object_cost.mapped;
}

bool render_mesh_has_atlas(uint32_t slot) { return slot < R.nmeshes ? R.mesh_atlas[slot] : true; }
