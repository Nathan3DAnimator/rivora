// The GPU-side layouts shared by render.c and gi.c (shaders/common.glsl, scalar layout), and the small buffer and
// image helpers both use.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct VkCtx;

#define MAX_LIGHTS 16 // the directional ones (FrameBuf.lights); the local lights live in lights.c's buffer
#define RE_GI_RANGE 6000.0f // cm: what is nearer the camera can light the view or be reflected in it (relevance, light grid)
#define VSM_MAX_LEVELS 8 // the sun's virtual shadow map levels (vsm.c): GpuFrame.vsm_*

typedef struct GpuLight {
    float position[3], range;
    float direction[3]; uint32_t type;
    float radiance[3], inv_range2;
    float cos_outer, cos_inner, half_w, half_h;
    float right[3], source_radius;
} GpuLight;

// The contact AO's occluders (notas-dev/gi.md, layer 4): the Movable actors nearest the camera as boxes or spheres,
// their cosine-weighted occlusion computed exactly per pixel (no noise, no history)
#define MAX_OCCLUDERS 32
#define MAX_GLASS_BOXES 16 // (Q4) GpuFrame.glass
#define OCCLUDER_BOX 0.0f
#define OCCLUDER_SPHERE 1.0f
// GpuOccluder.center[3] on the GPU: its reach (cm, where its contact AO fades out), negative for a sphere (gi.glsl reads
// the 16 bytes of the center first and the rest only for the ones in reach: every pixel looped over 64 bytes x 32)
typedef struct GpuOccluder {
    float center[4];               // xyz, w: the type
    float ax[4], ay[4];            // half-axis vectors (cm); a sphere: ax.x is the radius
    float az[3]; uint32_t entity;  // itself casts nothing on itself
} GpuOccluder;

typedef struct GpuFrame {
    float view_proj[16];
    float shadow_matrix[16];
    float camera_pos[4], camera_fwd[4], camera_right[4], camera_up[4];
    float sun_dir[4], sun_radiance[4];
    float sun_illuminance[4]; // rgb: the sky's sun above the atmosphere (lux, colored); w: aerial perspective scale
    float exposure_params[4]; // manual exposure, compensation (linear), auto exposure's min and max
    VkDeviceAddress sky;      // the sky light's SH (atmosphere.comp)
    VkDeviceAddress occ_light; // the contact GI's occluder radiances (gi.c, occluder_light.comp)
    float sky_params[4];      // sky light intensity, area lights' shadow rays, main pass MSAA samples, grid on
    float viewport[4];
    uint32_t light_count, view_mode, pad0, pad1; // pad0: the outline is on; pad1: the path tracer's sample count
    // Global illumination
    VkDeviceAddress tlas;
    uint32_t gi_flags, frame_index;
    int32_t probe_min[4];   // xyz: world cell of the first probe, w: rays per probe
    int32_t probe_dims[4];  // xyz: probes per axis, w: the update phase (low byte) and stride (>> 8)
    float probe_params[4];  // spacing, hysteresis, normal bias, view bias
    float gi_params[4];     // GI intensity, sky light for misses, reflections' max roughness, lightmap intensity
    VkDeviceAddress indices, instances, probe_rays, probe_state;
    uint32_t occluder_count, post_flags; // post_flags: POST_AUTO_EXPOSURE
    VkDeviceAddress exposure;          // the auto exposure (shaders/exposure.comp)
    GpuOccluder occluders[MAX_OCCLUDERS];
    // The GPU cull (shaders/cull.comp, notas-dev/escala.md): the view's and the shadow box's planes (inside: dot >= 0)
    float cull_planes[6][4], shadow_planes[6][4];
    float cull_params[4]; // pixels per unit of radius over distance, min radius (px), LOD 0 radius (px), slots
    VkDeviceAddress mesh_info, vis, visible_main, visible_shadow, draws;
    uint32_t ncmd, pad_cull;  // ncmd: the cull's draws per list
    VkDeviceAddress tlas_dyn; // the dynamic set's TLAS (0: none), traced with tlas
    // Two-phase occlusion (cull.comp passes 3-5): the late list, last frame's visibility, the Hi-Z (min-depth pyramid
    // of the prepass, hiz.comp) and its level 0 size
    VkDeviceAddress visible_late, vis_prev, hiz;
    uint32_t hiz_w, hiz_h, hiz_levels, hiz_pad;
    float dyn_box[2][4];   // the dynamic set's bounds (min, max): a ray that misses them skips its TLAS (rt.glsl)
    // The local lights (shaders/lights.glsl, light_grid.comp, lights.c)
    VkDeviceAddress light_buf, lg_cells, lg_items, lg_cursor;
    int32_t lg_origin[3][4];
    float lg_params[4];      // level 0's cell (cm), the LOD, -, the slots
    uint32_t local_lights, lg_cap, shadow_budget, lg_pad;
    // The surface cache (gi_cache.c, shaders/gi_cache.comp): the slot -> chart table, the texels, this frame's share
    VkDeviceAddress cache_st, cache_texels;
    uint32_t cache_count, cache_first, cache_n, cache_rays; // texels; this frame's first and count; rays per texel
    float cache_params[4];  // atlas side, hysteresis, -, -
    // Relevance (cull.comp pass 0): a bit per scene slot, set when the actor can show up in this view (seen, casting a
    // shadow into it, or near enough to light it or be reflected: gi_range cm); the dynamic TLAS and the systems read it
    VkDeviceAddress relevance;
    float gi_range;
    float ups_exposure; // > 0: the pre-exposure chosen on the CPU (a vendor's upscaler; common.glsl pre_exposure)
    VkDeviceAddress mesh_words; // the meshes' words (R.morphs: morph targets, vertex colors at GpuMeshInfo.vcol_first; 0: none)
    // the decals (M6, decal.c): their GpuDecals, the screen's 16 px tiles' lists (decal_bin.comp: a count then up to
    // DECAL_TILE_MAX indices each), how many and the tiles across / down
    VkDeviceAddress decals, decal_tiles;
    uint32_t decal_count, decal_tiles_x, decal_tiles_y, decal_pad;
    // Weather (weather.c, shaders/weather.glsl): the height fog and the clouds
    VkDeviceAddress weather_sky; // the sky light's SH with the clouds over it (weather.comp pass 5)
    uint32_t weather_flags, weather_pad;
    VkDeviceAddress cloud_tables; // the clouds' coverage thresholds and prefiltered means (cloud_noise.c)
    float fog0[4];  // density (1/cm) at the base, height falloff (1/cm), base height (cm), max opacity
    float fog1[4];  // albedo rgb, anisotropy
    float fog2[4];  // emissive rgb (nits), start distance (cm)
    float fog3[4];  // the froxels' reach (cm), -, -, -
    float cloud0[4]; // coverage, density (1/cm), bottom, top (cm)
    float cloud1[4]; // the wind's offset x, y (cm), detail, shadow strength
    float cloud2[4]; // albedo rgb, -
    float cloud_shadow_origin[4]; // the shadow map's center xy and side (cm)
    // The surface cache's events (render.c): a Movable actor started or stopped moving here (center, radius cm): the texels
    // inside keep a short history for a full cycle (gi_cache.comp), its shadow doesn't linger as a mark
    float cache_events[8][4];
    uint32_t cache_nevents, ce_pad[3];
    // The materials (material.c, shaders/mat_vm.glsl): their GpuMaterial slots, programs and parameter values
    VkDeviceAddress materials, mat_programs, mat_params, object_cost; // (object_cost: VIEW_OBJECT_COST's float per slot, 0: none)
    // The post processing (post.c, render.c fill_post; the layout is shaders/composite.frag's, bloom.comp's)
    float pp[8][4];
    float prev_view_proj[16]; // last frame's view_proj (the camera's motion blur)
    float cloud_prev_vp[16];  // last frame's view_proj, always (the screen's clouds: weather.comp pass 6)
    float cloud_scr[4];       // their size (px), a march's weight over the history (0: none), on
    // The sun's virtual shadow maps (vsm.c, shaders/vsm.glsl): per level, world -> its light space (x, y: cm from its
    // center; z: depth 0..1), its half side, texel and depth range (cm) and whether it holds a drawing; this frame's draw
    // into it (mesh.vert pass 1: the rectangle's corner, cm, and 2 / its size); levels, PCSS taps, tan(sun's radius)
    float vsm_mat[VSM_MAX_LEVELS][16];
    float vsm_level[VSM_MAX_LEVELS][4];
    float vsm_job[VSM_MAX_LEVELS][4];
    float vsm_params[4];
    // The local lights' shadow maps (lsm.c, shaders/lsm.glsl): the frame's table (light slot -> its map, the faces drawn,
    // the cull's spheres), the spheres for the cull this frame, the filter's taps; 1 / the atlas's side, the border
    VkDeviceAddress lsm_tab;
    uint32_t lsm_cull, lsm_flags;
    float lsm_params[4];
    // The clusters (cull.comp pass 6, render.c cluster_record): GpuCluster per cluster, the work (dispatch, per instance
    // a draw and its slot / clusters), the visible clusters' indices; cl_entries 0: off
    VkDeviceAddress clusters, cl_work, cl_idx, cl_perm; // (cl_perm: per mesh LOD's clusters, 6 view orders)
    uint32_t cl_entries, cl_idx_cap;
    // Temporal super resolution (tsr.comp): this frame's view_proj without its jitter, last frame's, the inverse of the
    // jittered one (the depth back to the world); the jitter (render px), the velocity pass ran, no history; the render's
    // and the output's sizes (px)
    float tsr_vp[16], tsr_prev_vp[16], tsr_inv[16];
    float tsr[4], tsr_size[4];
    // Water (water.c, shaders/water.glsl): the rain's surface and its water, their flow, the ripples, the ocean's
    // spectrum, FFT and maps; the numbers (water.glsl lists them)
    VkDeviceAddress water_addr[8];
    float water[96][4];
    // AR (ar_arcore.c): where the screen's corners fall in the camera's picture (0: (-1,-1), (1,-1); 1: (-1,1), (1,1)),
    // 2: on, the main light's share of the real floor's light (the shadows over the picture), the picture's w, h; 3: the
    // real depth on, its w, h, the projection's near (cm); 4, 5: the screen's corners in the depth (as 0, 1)
    float ar[6][4];
    // (A9.5b) the Local Fog Volumes (weather.c, weather.comp pass 3): FOGVOL_STRIDE vec4s each (world -> its unit space,
    // shape / density 1/cm / edge / height falloff, albedo, emissive)
    VkDeviceAddress fog_volumes;
    uint32_t fog_volume_count, fog_volume_pad;
    float fade[4]; // (B9) the picture faded to rgb by a (the Sequencer's Fade track; composite.frag, last)
    float grade_lut[4]; // (A9.4) the Color Grading LUT: x its intensity, y its size N (binding 80: an N*N x N strip); z the bokeh's blades, w their rotation
    float hdr[4];       // (A9.4) HDR10 output: x on, y paper white (nits), z peak (nits), w the UI target (binding 81) to lay over
    // (2026-11-10) the Stylized style (project.h stylized_*): 0 bands, softness, saturation, outline width (px, 0 off);
    // 1 the shadows' tint rgb, the outline's depth step (a share of the distance); 2 the outline's color rgb, the film
    // grain's amount (project.h film_grain: 0 none .. 2); (2026-11-10, every style's own options) 3 the shadows' denoise
    // (0 Low .. 3 Epic: shadow_split.comp), the Video-realistic lens's distortion, fringes and halation (x the style's);
    // 4 its vignette (x the style's), the rest free
    float style[5][4];
    // (2026-11-14) ReSTIR GI (den_trace.comp): its samples' address (lo, hi), 2 free; on, 3 free
    uint32_t rgi[4], rgi_info[4]; // (rgi_info.y: Q4, the glass boxes below in use)
    // (2026-11-17, Q4) the colored shadow without rays: the glass with Colored Shadow nearest the camera as oriented boxes
    // (center xyz + its material slot as float bits; the three half axes), mesh.frag glass_box_tint
    float glass[MAX_GLASS_BOXES][4][4];
    // (Q13) the horror kit (terror.c, composite.frag, exposure.comp): 0 hallucination, black & white blown out, white flash,
    // TV static; 1 VHS, chromatic aberration, distortion, the letterbox's bar (a share of the height); 2 the tension's grain,
    // vignette (added), saturation (x), the pulse now; 3 the clock (s), its noise step, the toe, anything on; 4 the dip's
    // color and amount; 5 the horror exposure (on, the metered share), -, -; 6 the hallucination's tint, its levels; 7 free
    float terror[8][4];
    // (Q17) Nanite Tessellation (tess.c, shaders/tess.glsl): R.tess and the patterns' vertices; px per micro edge (the
    // budget's scale in), the most factor; TESS_ flags, the triangles' room
    VkDeviceAddress tess_buf, tess_pat;
    float tess[4];
    uint32_t tess_u[4];
    // (Q21) the distant view (farview.c, shaders/farview.glsl): the heightfield and its horizon, the impostors' table (a
    // uvec4 per mesh slot); FAR_ flags, the card's draw (slot x 4), its first vertex, the heightfield's side; its origin
    // (cm), texel (cm), the impostors' switch (screen radius px); the far shadow's strength, the sun's elevation (rad),
    // the fog's sky share, -
    VkDeviceAddress far_buf, imp_tab;
    uint32_t far_u[4];
    float far_f[4];
    float far_g[4];
    // (Q25b) mixed reality (xr_mr.c, shaders/mr.glsl): the room's boxes and the Passthrough Windows (MR_BOX_VEC4S vec4s
    // each: world -> its unit cube as 3 rows, then kind and label); MR_ flags; the numbers (mr.glsl's list)
    VkDeviceAddress mr_boxes;
    uint32_t mr_box_count, mr_flags;
    float mr[12][4];
    GpuLight lights[MAX_LIGHTS];
} GpuFrame;

// A decal as the main pass reads it (M6, decal.c; common.glsl Decal): world -> its box ([-1, 1] each way: rows 0-2),
// its rectangle on the screen (render pixels: min, max), the projection's axis (its X; w: the opacity, every fade in),
// the box's Y and Z as the texture's U and V (unit; w: its material slot, and the cosine past which it fades out)
typedef struct GpuDecal {
    float row[3][4];
    float rect[4];
    float axis[4];
    float u_axis[4]; // w: material slot (a float value)
    float v_axis[4]; // w: cos(angle fade)
} GpuDecal;
#define DECAL_TILE 16
#define DECAL_TILE_MAX 31

typedef struct GpuVertex {
    float pos[3];
    uint32_t normal_oct;
    float uv[2];
    float uv2[2]; // the lightmap's unwrap
} GpuVertex;

typedef struct GpuInstance {
    float rows[3][4];
    float base_metal[4];
    float emissive_rough[4];
    uint32_t ids[4];       // entity, INST_ flags, first index, vertex offset
    float lightmap_st[4];  // UV2 -> atlas: uv * xy + zw
} GpuInstance;
// With INST_MATERIAL the material gives the emission (and the roughness, the normal): emissive_rough.x holds its
// GpuMaterial slot (uint bits), .y the UV tiling; .w stays the mean roughness (the GI's hits) and .z 0
#define INST_MATERIAL 64u
#define INST_NO_OCCLUDER 128u // not one of the contact AO's occluders (a skeletal mesh's small parts: anim.c)

#define INST_LIGHTMAP 1u
#define INST_CAST_SHADOW 2u
#define INST_STATIC 4u // Static mobility: in the lightmap bake's rays
#define INST_ALIVE 8u  // the slot holds a mesh to draw (the scene's slots are entity indices: most are empty)
#define INST_DYNAMIC 16u // in the dynamic set: the dynamic TLAS has it, the static one leaves it out
#define INST_MOVING 32u  // a dynamic one whose transform changed in the last 60 frames (render.c scene_prepare)
#define INST_WIND 512u  // a plant (CAST_SHADOW_WIND): mesh.vert sways its vertices
#define INST_TRANSLUCENT 1024u // a Translucent material: the OIT pass draws it (render.c), no shadow, not in the TLAS
#define INST_CROWD 256u // a crowd's part (crowd.comp): after the world's slots, no surface cache, not in the TLAS
// a skin (RMESH_SKINNED) deformed on the GPU: ids[3] its own vertices (this frame's), lightmap_st[0] last frame's
// (uint bits: the velocity pass), lightmap_st[2..3] its own BLAS's address (uint bits)
#define INST_SKINNED 2048u
#define INST_ROUND_OCCLUDER 4096u // (CAST_SHADOW_ROUND_OCCLUDER) the contact AO sees it as spheres along its upright axis
#define INST_CHAN_SHIFT 13 // (Q13) bits 13-15: the Lighting Channels, channel 0 flipped (0: channel 0 only; common.glsl light_channel_ok)
#define INST_SHAPE_SHIFT 16 // ids[1] >> 16: the mesh (shape)

// Per mesh, for the cull: the local bounding sphere and its LODs (cull.comp picks one; tlas_fill.comp its BLAS)
#define MESH_INFO_MASKED 0x100u // GpuMeshInfo.lod_count: drawn by a Masked / Two Sided material (cull.comp: always early)
#define MESH_INFO_ERROR_LOD 0x200u // ... its LOD picked by lod_error on screen (the imported meshes' simplified LODs)
#define MESH_INFO_TRANSLUCENT 0x400u // ... drawn by a Translucent material: its own run of draws (the OIT pass), no clusters
#define MESH_INFO_SKINNED 0x1000u // ... a skin (RMESH_SKINNED): its instances read their own vertices (INST_SKINNED)
#define MESH_INFO_RT_LOD_SHIFT 16u // ... a skin's: the LOD its BLAS holds (2 bits; render.c skin_rt_lod): its rays start that LOD's error off
#define MESH_INFO_OMM 0x800u // ... its BLAS has an opacity micromap (gi_blas_omm): its instances not forced non-opaque
// (2026-11-24) ... drawn by a material with the RE_EXT features (mat_ext: the Principled's extra lobes, Toon, Wet, detail,
// parallax...): its own run of draws, the RE_EXT pipelines only there (cull.comp's runs; render.c draw_runs)
#define MESH_INFO_EXT 0x2000u
typedef struct GpuMeshInfo {
    float sphere[4];
    uint32_t lod_count;
    float lod_error[3]; // LODs 1-3: how far inside LOD 0 (the BLAS) their surface can be (cm, mesh space): the rays' bias
    uint32_t first_index[4], index_count[4];
    int32_t vertex_offset[4];
    VkDeviceAddress blas; // 0 without ray tracing
    // a UV2 chart that closes on itself around a round mesh: x from [0] to 1 - [0] (one seam) where y < [1] (0: none);
    // gi.glsl reads the surface cache across that seam
    float uv2_wrap[2];
    // its clusters per LOD (GpuCluster, render.c's R.clusters; count 0: none, drawn whole): cull.comp pass 6
    uint32_t cluster_first[4], cluster_count[4];
    // its cluster LOD DAG (mesh_clod.h; count 0: none): an opaque one near the camera is drawn by the clusters whose
    // error on screen fits (cull.comp pass 6), the rest of it coarser
    uint32_t dag_first, dag_count;
    uint32_t vcol_first; // its vertex colors (RMESH_COLORS): a word each in R.morphs from here (UINT32_MAX: none)
    uint32_t tess_first; // (Q17) its canonical table in R.morphs' words (tess.c; UINT32_MAX: none). 144 bytes: the C struct's 8-byte alignment, the GLSL stride the same
} GpuMeshInfo;
// A cluster of a big mesh (mesh_build.c: up to 128 vertices and 128 triangles): its bounding sphere (mesh space), index
// range (in R.indices, relative to the LOD's vertex_offset); a DAG's: its own error and its parent's (cm, mesh space) and
// the spheres they are measured from (drawn when its own fits on screen and its parent's doesn't). The per-LOD clusters:
// 0 and FLT_MAX (always)
typedef struct GpuCluster {
    float sphere[4];
    uint32_t first_index, index_count;
    float self_err, parent_err;
    float self_sphere[4], parent_sphere[4];
} GpuCluster;
#define CL_MAX_ENTRIES 4096u // the instances drawn by clusters per frame (more: drawn whole)
#define CL_MAX_CHUNKS 65536u // their blocks of 64 clusters (4 M clusters)
// R.cl_work: 32 B (dispatch x y z, the indices' and the blocks' cursors), a draw per instance (20 B), per instance its
// slot, first cluster, clusters and first block (16 B), per block its seen clusters' mask and index count (16 B)
#define CL_DRAWS_AT 32u
// then a second draw per instance (20 B): the opaque ones' (the DAG), drawn by the opaque pipelines (each instance's draw
// in one of the two, the other's instance count 0)
#define CL_DRAWS2_AT (32u + CL_MAX_ENTRIES * 36u + CL_MAX_CHUNKS * 16u)
// then a third (20 B): a DAG's late clusters (the second phase: last frame's Hi-Z rejected them, this frame's didn't)
#define CL_DRAWS3_AT (CL_DRAWS2_AT + CL_MAX_ENTRIES * 20u)
// then the DAG entries' list (4 B each): the DAGs' first phase dispatches over it (its x, y, z: header words 5-7)
#define CL_DAGLIST_AT (CL_DRAWS3_AT + CL_MAX_ENTRIES * 20u)
#define CL_WORK_BYTES (CL_DAGLIST_AT + CL_MAX_ENTRIES * 4u)
#define GI_RT 1u
#define GI_PROBES 2u
#define GI_LIGHTMAPS 4u
#define GI_REFLECTIONS 8u
#define GI_BAKING 16u
#define GI_CONTACT_AO 32u
#define GI_RT_SHADOWS 64u   // every Cast Shadows light traced in the main pass (sky_params[1]: rays per area light)
#define GI_CONTACT_BOUNCE 128u // the occluders bounce their light (occ_light)
#define GI_SURFACE_CACHE 256u // the surface cache lights the meshes that have a chart (gi_cache.c)
#define GI_RT_MASKED 512u   // a Masked mesh is drawn: their TLAS instances aren't opaque (rt.glsl tests the alpha)
#define GI_FALLBACK 134217728u // (Q27.12) the GI's fallback volume is in water[24..26] (gi_fallback.c): mesh.frag's sky light where nothing else lights
#define GI_GLASS_SHADOW 67108864u // (2026-11-17, Q4) refractive glass with Colored Shadow drawn: mesh.frag tints the sun through it (RT_MASK_GLASS)
#define GI_REFRACT 33554432u // (2026-11-16) a refractive glass drawn this frame: shadow_split.comp pass 3 reads the scene behind it bent (binding 84 the sums, 82 the scene)
#define GI_NO_EMISSIVE 16777216u // (Lighting Levels, sublevels.c) a lighting level's bake: the rays see no emissive (the base bake has it once)
#define GI_VSM 1024u        // the sun's virtual shadow maps (vsm.c): the cull keeps every caster in the shadow box (cached)
#define GI_VSM_DYN 2048u    // (with GI_VSM) no level drawn whole this frame: the shadow list holds only the dynamic casters
#define GI_SHADOW_SPLIT 4096u // the area lights' shadows filtered (mesh.frag o_split, shadow_split.comp)
#define GI_AA_WEIGHTED 8192u  // the MSAA resolve weighted by brightness (common.glsl aa_encode, shadow_split.comp pass 2)
#define GI_SUN_HYBRID 16384u  // the sun's map with rays for its near casters (vsm.glsl: hard shadows exact, hair by its alpha)
#define GI_LOCAL_SM 32768u    // the local lights' shadows by their maps (lsm.c: Low, the GPUs without ray tracing)
#define GI_GTAO 65536u        // the screen space AO (gtao.comp, A4a): mesh.frag's alpha holds the indirect diffuse share
#define GI_DENOISE 131072u    // the unified RT denoiser (A9.1d, the Photoreal styles: mesh.frag o_den / o_guide, denoise.comp)
#define GI_PROBES_FAR 262144u // the far probes (A2, gi.glsl): a second grid 8x wider apart, read past the near one
#define GI_RESTIR 524288u     // ReSTIR DI on the many lights (A9.1a: the Photoreal styles with the unified denoiser)
#define GI_SSR 4194304u // (A4c) screen space reflections from last frame's copies (mesh.frag; where GI_REFLECTIONS isn't on)
#define GI_CONTACT_SHADOWS 2097152u // the sun's contact shadows (A4b, gtao.comp): mesh.frag leaves the sun's share in the alpha too
#define GI_DEN_CHECKER 8388608u // (with GI_DENOISE) the diffuse and rough specular rays in a checkerboard: one a pixel a frame (ReBLUR's)
#define GI_SSS 1048576u       // the skin's screen space subsurface scattering (A9.3a, sss.comp: one sample, a subsurface material seen)
#define GPU_LIGHT_CASTS 0x100u // in GpuLight.type
#define POST_AUTO_EXPOSURE 1u

// A particle emitter's block (particles.c; shaders/particles.glsl Emitter: the same fields in the same order, scalar)
typedef struct GpuEmitter {
    _Alignas(16) float rows[3][4]; // (16: a block's address in an array must keep EmitterBuf's alignment)
    VkDeviceAddress pool, list_sprite, list_mesh, args, shape, list_point;
    uint32_t base, capacity, rate_slots, flags;
    float time, dt, rate, stop;
    float burst_time, burst_interval, burst_count, tl_start;
    float tl_end, life_min, life_max, scale;
    uint32_t seed, shape_kind, shape_tris;
    float point_px; // sprites up to this many pixels across drawn as points (0: none)
    float shape_p[4], vel_dir[4], vel_p[4], size_p[4], color0[4], color1[4], gravity[4], curl[4], vortex[4], vortex_axis[4];
    float attract[4], attract2[4], wind[4], collide[4], limits[4], killbox_min[4], killbox_max[4];
    float col_life[4][4], col_life_t[4], size_life[4], render_p[4], render2[4];
    uint32_t mesh_first_index, mesh_index_count, mesh_vertex_offset, mesh_vertices;
    float mesh_color[4], mesh_scale[4];
    float sub_uv[4];      // Sub UV Animation: x columns, y rows, z mode (0 over life, 1 frame rate, 2 random), w frame rate
    float speed_scale[4]; // Scale by Speed: x speed min, y speed max (cm/s), z size at min, w size at max
    float extra[4];       // x camera offset (cm), y alpha at the min speed, z at the max, w Emitter State's start delay (s, CPU)
    float ribbon[4];      // (B10) x width scale, y particles a texture repeat, z tail fade (0..1)
} GpuEmitter;
_Static_assert(sizeof(GpuEmitter) % 16 == 0, "GpuEmitter: blocks 16-aligned");
// GpuEmitter.flags (particles.glsl PF_*)
#define PF_LOCAL 1u
#define PF_TL 2u
#define PF_LOOP 4u
#define PF_GRAVITY 8u
#define PF_DRAG 16u
#define PF_CURL 32u
#define PF_VORTEX 64u
#define PF_ATTRACT 128u
#define PF_WIND 256u
#define PF_COLLIDE 512u
#define PF_LIMIT 1024u
#define PF_KILLBOX 2048u
#define PF_COLOR_LIFE 4096u
#define PF_SIZE_LIFE 8192u
#define PF_BROWNIAN 16384u
#define PF_MESH 32768u
#define PF_LIT 65536u
#define PF_ADDITIVE 131072u
#define PF_VELOCITY_ALIGN 262144u
#define PF_SHADOWS 524288u
#define PF_RIBBON 8388608u // (B10) the Ribbon Renderer: a strip through the births (ribbon.vert)
#define PF_SUBUV 1048576u
#define PF_SPEED_SCALE 2097152u
#define PF_SUBUV_BLEND 4194304u

typedef struct Push {
    VkDeviceAddress frame, vertices, instances;
    uint32_t pass, pad;
} Push;

_Static_assert(sizeof(GpuLight) == 80, "GpuLight layout");
_Static_assert(sizeof(GpuOccluder) == 64, "GpuOccluder layout");
_Static_assert(offsetof(GpuFrame, sky) == 256, "GpuFrame layout (8-aligned addresses: GLSL scalar layout)");
_Static_assert(offsetof(GpuFrame, mesh_info) == 432 + 16 + 64 * MAX_OCCLUDERS + 208, "GpuFrame layout");
_Static_assert(offsetof(GpuFrame, lights) == 432 + 16 + 64 * MAX_OCCLUDERS + 448 + 64 + 144 + 144 + 32 + 192 + 80 + 784 + 8 + 40 + 224 + 64 + 1280 + 96 + 32 + 8 + 32 + 16 + 16 + 16 + 16 + 80 + 128 + 32 + 64 + 64 + 64 * MAX_GLASS_BOXES + 128 + 48 + 64 + 208, "GpuFrame layout"); // (+208: mixed reality, Q25b; +64: the distant view, Q21; +48: Nanite Tessellation, Q17; +128: the horror kit, Q13; +64: water 92 -> 96, the wet actors' drips; +64: water 88 -> 92, the rain's shading 2026-11-15; water 1024: 64 vec4, the rain movers 2026-11-10; +128: water 80 -> 88, the rain's look, far box and actors' wetness 2026-11-14; +80: the styles' options (Stylized's, the shadows' denoise, the lens); +16: HDR10, A9.4; +16: the Color Grading LUT, A9.4; +16: the fade, B9; +16: the Local Fog Volumes, A9.5b; +32: the local lights' shadow maps; +8: mesh_words, M5; +32: the decals, M6)
_Static_assert(offsetof(GpuFrame, tess_buf) % 8 == 0, "GpuFrame layout (Q17: 8-aligned addresses)");
_Static_assert(offsetof(GpuFrame, far_buf) % 8 == 0, "GpuFrame layout (Q21: 8-aligned addresses)");
_Static_assert(offsetof(GpuFrame, mr_boxes) % 8 == 0, "GpuFrame layout (Q25b: 8-aligned addresses)");
_Static_assert(offsetof(GpuFrame, clusters) % 8 == 0, "GpuFrame layout (8-aligned addresses: GLSL scalar layout)");
_Static_assert(sizeof(GpuMeshInfo) == 144 && sizeof(GpuCluster) == 64, "GpuMeshInfo / GpuCluster layout");
_Static_assert(sizeof(GpuVertex) == 32, "GpuVertex layout");
_Static_assert(sizeof(GpuDecal) == 112, "GpuDecal layout");
_Static_assert(sizeof(GpuInstance) == 112, "GpuInstance layout (a million of them: every byte is read each frame)");
_Static_assert(sizeof(Push) == 32, "Push layout");

typedef struct GpuBuf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress addr;
    VkDeviceSize size;
    void* mapped;
} GpuBuf;

typedef struct GpuImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    VkImageLayout layout;
    uint32_t layers; // (0: one; gpu_transition moves these layers together)
} GpuImage;

bool gpu_buf_create(struct VkCtx* vk, GpuBuf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
void gpu_buf_destroy(struct VkCtx* vk, GpuBuf* b);
bool gpu_image_create(struct VkCtx* vk, GpuImage* im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
void gpu_image_destroy(struct VkCtx* vk, GpuImage* im);
// A layout change with both sides' stages and accesses (discard: the old contents don't matter)
void gpu_transition(VkCommandBuffer cmd, GpuImage* im, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, bool discard);
// A global memory barrier
void gpu_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                 VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
VkShaderModule gpu_shader(struct VkCtx* vk, const uint32_t* code, size_t bytes);
VkPipeline gpu_compute_pipeline(struct VkCtx* vk, VkPipelineLayout layout, const uint32_t* code, size_t bytes);
// Records and submits one command buffer and waits (startup work: uploads, the BLAS builds)
VkCommandBuffer gpu_begin_once(struct VkCtx* vk);
void gpu_end_once(struct VkCtx* vk, VkCommandBuffer cmd);
