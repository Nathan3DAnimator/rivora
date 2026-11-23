// Shared by every shader: the per-frame data, the buffers reached by device address (no descriptor sets for
// buffers), the procedural sky and small helpers. Layouts match src/render_types.h (scalar layout).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require

#define PI 3.14159265

#define VIEW_LIT 0
#define VIEW_UNLIT 1
#define VIEW_WIREFRAME 2
#define VIEW_LIGHTING_ONLY 3
#define VIEW_NORMALS 4
#define VIEW_SOLID 5
#define VIEW_SOLID_RANDOM 6
#define VIEW_TEXTURE 7
#define VIEW_ROUGHNESS 8
#define VIEW_METALLIC 9
#define VIEW_PATH_TRACING 10
#define VIEW_GI_ONLY 11
#define VIEW_LOD 12
#define VIEW_TRIANGLES 13
#define VIEW_MOBILITY 14
#define VIEW_SURFACE_CACHE 15
#define VIEW_GI_SOURCE 16
#define VIEW_TEXEL_DENSITY 17
#define VIEW_LIGHT_COMPLEXITY 18
#define VIEW_SHADER_COST 19
#define VIEW_OBJECT_COST 20

// The modes that skip the lighting and the tone mapping (shown as they are)
bool unlit_mode(uint m) {
    return m == VIEW_UNLIT || (m >= VIEW_NORMALS && m <= VIEW_METALLIC) || (m >= VIEW_LOD && m != VIEW_SURFACE_CACHE);
}
// A heat map for the counts and costs (the visualizations): 0 dark blue, then blue, green, yellow, red, white
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c[6] = vec3[6](vec3(0.02, 0.02, 0.15), vec3(0.0, 0.25, 1.0), vec3(0.0, 0.85, 0.3), vec3(1.0, 0.9, 0.0), vec3(1.0, 0.1, 0.0), vec3(1.0));
    float x = t * 5.0;
    int i = min(int(x), 4);
    return mix(c[i], c[i + 1], x - float(i));
}

// gi_flags (render.c)
#define GI_RT 1u          // the TLAS is valid: ray queries
#define GI_PROBES 2u      // the probes (dynamic, or the baked volume) light the diffuse
#define GI_LIGHTMAPS 4u   // baked lightmaps exist (instances with ids.y & INST_LIGHTMAP use them)
#define GI_REFLECTIONS 8u // traced reflections on smooth surfaces
#define GI_BAKING 16u     // Build Lighting: the probes form the baked volume (rays see the Static meshes only, a running mean)
#define GI_CONTACT_AO 32u // the Movable actors darken the indirect light around them (gi.glsl)
#define GI_RT_SHADOWS 64u // every shadow-casting light traced (sky_params.y: rays per area light); else the sun's map
#define GI_CONTACT_BOUNCE 128u // occ_light holds the occluders' light: they bounce it around them (gi.glsl)
#define GI_SURFACE_CACHE 256u // the surface cache lights the meshes that have a chart (gi_cache.comp)
#define GI_RT_MASKED 512u // a Masked mesh is drawn: its TLAS instances aren't opaque (the shadow rays test its alpha)
#define GI_FALLBACK 134217728u // (Q27.12) the GI's fallback: the AO volume and the ground's bounce (gi_fallback.c, water[24..26])
#define GI_GLASS_SHADOW 67108864u // (2026-11-17, Q4) glass with Colored Shadow in the scene: the sun through it tinted (mesh.frag glass_tint)
#define GI_REFRACT 33554432u // (2026-11-16) refractive glass this frame (shadow_split.comp pass 3)
#define GI_NO_EMISSIVE 16777216u // (Lighting Levels, sublevels.c) a lighting level's bake: the rays see no emissive (the base bake has it once)
#define GI_VSM 1024u      // the sun's virtual shadow maps (vsm.c): the cull keeps every caster in the shadow box (a cache)
#define GI_VSM_DYN 2048u  // (with GI_VSM) no level drawn whole this frame: the shadow list holds only the dynamic casters
#define GI_SHADOW_SPLIT 4096u // mesh.frag's first area light per pixel out to o_split, filtered by shadow_split.comp
#define GI_AA_WEIGHTED 8192u  // the main pass writes aa_encode(color): the MSAA resolve weighted by brightness
#define GI_SUN_HYBRID 16384u  // the sun's map leaves its near casters' shadows to a ray (vsm.glsl, mesh.frag)
#define GI_LOCAL_SM 32768u    // the local lights' shadows by their maps (lsm.c, lsm.glsl: Low, the GPUs without ray tracing)
#define GI_GTAO 65536u        // mesh.frag leaves its indirect diffuse share in the alpha (1 + share) for gtao.comp
#define GI_DENOISE 131072u    // the unified denoiser (A9.1d, Photoreal): mesh.frag's noisy signals to o_den, denoise.comp
#define GI_PROBES_FAR 262144u // the far probes (A2, gi.glsl probe_grid 1): read where the near grid ends
#define GI_RESTIR 524288u     // ReSTIR DI on the many lights (A9.1a, mesh.frag RE_DEN: the reservoirs in the denoiser's guide)
#define GI_SSR 4194304u // (A4c) screen space reflections from last frame's color and depth (mesh.frag, bindings 82-83)
#define GI_CONTACT_SHADOWS 2097152u // mesh.frag also leaves the sun's share in the alpha (+ 2 x its 15ths) for gtao.comp's contact shadows (A4b)
#define GI_DEN_CHECKER 8388608u // (with GI_DENOISE) a pixel needing both traces one of them a frame, in a checkerboard (denoise.comp fills the other)
#define GI_SSS 1048576u       // the skin's screen space subsurface scattering (A9.3a): mesh.frag's checkerboard for sss.comp

// The MSAA resolve weighted by brightness (Karis): the main pass writes (c * w, w) with w = 1 / (1 + max(c)), the
// resolve averages both, and shadow_split.comp (pass 2) divides: each pixel's samples averaged with the weights w. A
// plain average let a bright background swallow an edge (a pixel half hair, half lit wall read almost as bright as the
// wall: the edges looked stair-stepped even at 8x). The weight in the alpha, not a curve to invert: c / (1 + max(c))
// turned back from half floats made each frame's tiny GI change a step of up to 0.3% (the house's flicker 0.066 ->
// 0.127%). Alpha 1 (the grid's blend, the other views): the color as it is. (4096: the weight a normal half float)
vec4 aa_encode(vec3 c) {
    float w = 1.0 / (1.0 + min(max(max(c.r, c.g), c.b), 4096.0));
    return vec4(c * w, w);
}
vec3 aa_decode(vec4 t) { return t.a > 0.0 ? t.rgb / t.a : t.rgb; }

#define POST_AUTO_EXPOSURE 1u
#define INST_LIGHTMAP 1u   // Instance.ids.y
#define INST_CAST_SHADOW 2u
#define INST_STATIC 4u
#define INST_ALIVE 8u
#define INST_DYNAMIC 16u
#define INST_MATERIAL 64u // ids.y: emissive_rough.x is the material's slot (uint bits), .y its tiling (render_gpu.h)
#define INST_MOVING 32u // render.c: a Movable actor moving right now (its transform changed in the last 60 frames)
#define INST_NO_OCCLUDER 128u // not one of the contact AO's occluders (render_gpu.h)
#define INST_WIND 512u        // a plant: its vertices sway in the wind (mesh.vert)
#define INST_TRANSLUCENT 1024u // a Translucent material (render.c's OIT pass): no shadow, not in the TLAS
#define INST_CROWD 256u       // a crowd's part (crowd.comp): after the world's slots, no surface cache, not in the TLAS
#define INST_SKINNED 2048u    // a skin deformed on the GPU (render_gpu.h): ids.w its vertices, lightmap_st.x last frame's
#define INST_SHAPE_SHIFT 16u
// (2026-11-23) A material's emission for the rays: its baked mean (nits) packed by render.c emis_pack in emissive_rough.z,
// 10 bits a channel (log2 in 1/32 octave from 2^-10 nits; 0: none) and bit 30 when its actor lights the scene as a point
// light (lights.c emissive_light): the diffuse GI leaves it out then (that light brings it, shadowed by rays: both would
// count it twice), a reflection still shows the glow. An instance without a material: its Emissive (nits) as it is
vec3 emis_unpack(uint v) {
    uvec3 c = uvec3(v, v >> 10u, v >> 20u) & 1023u;
    return mix(exp2(vec3(c) / 32.0 - 10.0), vec3(0.0), equal(c, uvec3(0u)));
}
vec3 inst_emission(uint flags, vec4 er) { return (flags & INST_MATERIAL) != 0u ? emis_unpack(floatBitsToUint(er.z)) : er.rgb; }
vec3 inst_glow(uint flags, vec4 er) { // (the diffuse GI's: without the glow a light already brings)
    if ((flags & INST_MATERIAL) == 0u) return er.rgb;
    uint v = floatBitsToUint(er.z);
    return (v & 0x40000000u) != 0u ? vec3(0.0) : emis_unpack(v);
}

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define LIGHT_RECT 3
#define LIGHT_CASTS 0x100u // in GpuLight.type: the light casts shadows (Cast Shadows)

struct GpuLight {
    vec3 position;  float range;      // cm
    vec3 direction; uint type;        // the way the light travels
    vec3 radiance;  float inv_range2;
    float cos_outer; float cos_inner; float half_w; float half_h; // rect: half extents in cm
    vec3 right;     float source_radius;
};
// (Q11, Q13) the type's bits 0-1 are the kind (directional, point, spot, rect); 0x04 / 0x08 the toon's (toon.glsl
// LIGHT_TOON_FORCE / NEVER), 4-6 the Lighting Channels, 7 the beam's dust, 8 casts, 9-15 / 16-23 / 24-31 the scales
uint light_kind(GpuLight L) { return L.type & 0x3u; }
// (Q13) Unreal's Lighting Channels: the light's in the type's bits 4-6, the instance's in INST_ flags' bits 13-15, both
// stored flipped on channel 0 (0: channel 0 only, the default); a light reaches a surface sharing a channel with it
bool light_channel_ok(GpuLight L, uint inst_flags) { return ((((L.type >> 4u) ^ 1u) & ((inst_flags >> 13u) ^ 1u)) & 7u) != 0u; }
bool light_dust(GpuLight L) { return (L.type & 0x80u) != 0u; } // (Q13) its cone shows dust in the volumetric fog
bool light_casts(GpuLight L) { return (L.type & LIGHT_CASTS) != 0u; }
// Unreal's per-light scales (lights.c packs them in the type's free bits): the fog's (/32), the highlights' and the GI's (/64)
float light_volumetric_scale(GpuLight L) { return float((L.type >> 9) & 127u) / 32.0; }
float light_specular_scale(GpuLight L) { return float((L.type >> 16) & 255u) / 64.0; }
float light_indirect_scale(GpuLight L) { return float(L.type >> 24) / 64.0; }

#define MAX_OCCLUDERS 32
struct Occluder {
    vec4 center;          // xyz cm, w: its reach (cm; negative: a sphere)
    vec4 ax; vec4 ay;     // half-axis vectors (cm); a sphere: ax.x is the radius; ay.w 1: moving (INST_MOVING)
    vec3 az; uint entity; // not itself
};

// The auto exposure (exposure.comp): its histogram, the exposure the frame renders with (pre-exposure) and the one it
// was rendered with before the last adaptation (the composite divides the latter out)
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer ExposureBuf {
    uint bins[64];
    float exposure;
    float pre_exposure;
    float time;
    float avg_log; // the mean log2 luminance (nits) it saw
};
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer SkyBuf { vec4 sh[9]; };

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FrameBuf {
    mat4 view_proj;
    mat4 shadow_matrix;      // world -> shadow map uv (xy) and depth (z)
    vec4 camera_pos;         // xyz cm, w: time
    vec4 camera_fwd;         // xyz, w: tan(fov/2) horizontal
    vec4 camera_right;       // xyz, w: aspect (w/h)
    vec4 camera_up;
    vec4 sun_dir;            // xyz: toward the sun (for the sky), w: 1 when a directional light drives it
    vec4 sun_radiance;       // rgb, w: the shadow-casting light's index or -1
    vec4 sun_illuminance;    // rgb: the sky's sun at the top of the atmosphere (lux, colored); w: aerial perspective scale
    vec4 exposure_params;    // x: manual exposure, y: compensation (linear), z/w: auto exposure's min/max
    uvec2 sky;               // SkyBuf: the sky light's SH (atmosphere.comp)
    uvec2 occ_light;         // the contact GI's occluder radiances (occluder_light.comp, gi.glsl)
    vec4 sky_params;         // x: sky light intensity, y: area lights' shadow rays, z: the main pass's MSAA samples, w: grid on
    vec4 viewport;           // the viewport's origin in the framebuffer (px) and 1 / its size
    uint light_count;
    uint view_mode;
    uint pad0, pad1;
    // Global illumination (render.c / gi.c)
    uvec2 tlas;              // the scene's top-level acceleration structure (device address)
    uint gi_flags;
    uint frame_index;
    ivec4 probe_min;         // xyz: the world cell of the grid's first probe, w: rays per probe
    ivec4 probe_dims;        // xyz: probes per axis, w: the update phase (low byte) and stride (a quarter of the probes per frame; all during a bake)
    vec4 probe_params;       // x: spacing (cm), y: hysteresis, z: normal bias (cm), w: view bias (cm)
    vec4 gi_params;          // x: GI intensity, y: sky light (for probe misses), z: reflection max roughness, w: lightmap intensity
    uvec2 indices;           // the index buffer (hit normals)
    uvec2 instances;         // the instance buffer (every instance: the TLAS custom index points here)
    uvec2 probe_rays;        // the probes' ray results
    uvec2 probe_state;       // per probe slot: the world cell it holds
    uint occluder_count;
    uint post_flags;         // POST_AUTO_EXPOSURE: the composite reads the exposure buffer
    uvec2 exposure;          // shaders/exposure.comp
    Occluder occluders[MAX_OCCLUDERS]; // the contact AO's (gi.glsl)
    vec4 cull_planes[6];     // the GPU cull (cull.comp): the view's frustum, inside: dot >= 0
    vec4 shadow_planes[6];   // the shadow map's box
    vec4 cull_params;        // x: px per radius/distance, y: min radius (px), z: LOD 0 radius (px), w: slots
    uvec2 mesh_info;         // MeshInfo per mesh
    uvec2 vis;               // per slot: its main draw | shadow draw << 16 (12 bits each, 0xFFF: none; cull.comp)
    uvec2 visible_main;      // the slots each indirect draw reads (gl_InstanceIndex)
    uvec2 visible_shadow;
    uvec2 draws;             // the indirect draws, their counts and cursors
    uint pad_cull0;          // the draws per list (cull.comp)
    uint pad_cull1;          // float bits: the imported meshes' LOD error on screen (px; scalability)
    uvec2 tlas_dyn;          // the dynamic set's TLAS (0: none), traced with tlas (rt.glsl)
    uvec2 visible_late;      // two-phase occlusion: the late list (cull.comp passes 3-5)
    uvec2 vis_prev;          // per slot: drawn last frame (the early list)
    uvec2 hiz;               // the prepass's min-depth pyramid (hiz.comp)
    uint hiz_w; uint hiz_h; uint hiz_levels;
    uint hiz_pad;            // occlusion culling on (1) this frame: the prepass, the Hi-Z and the late list run
    vec4 dyn_box[2];         // the dynamic set's bounds (min, max): a ray that misses them skips its TLAS (rt.glsl)
    // The local lights (lights.glsl, light_grid.comp)
    uvec2 light_buf;         // GpuLight per slot (type ~0u: none)
    uvec2 lg_cells;          // per cell of the grid: its items' offset and count
    uvec2 lg_items;          // slot | home level << 30
    uvec2 lg_cursor;         // per cell: the fill's cursor; then the items asked for
    ivec4 lg_origin[3];      // per level: its first cell (in cells of that level)
    vec4 lg_params;          // x: level 0's cell (cm), y: the LOD (range / cell), w: the slots
    uint local_lights;       // how many (0: no grid)
    uint lg_cap;             // the items' capacity
    uint shadow_budget;      // the shadow-casting lights traced per pixel at most (the rest unshadowed)
    uint lg_pad;
    // The surface cache (gi_cache.c, gi_cache.comp)
    uvec2 cache_st;          // per scene slot: its chart (uv2 * xy + zw), x 0: none
    uvec2 cache_texels;      // the texels (mesh-space point and normal, slot, atlas texel)
    uint cache_count;        // texels
    uint cache_first;        // this frame's first texel to update
    uint cache_n;            // and how many
    uint cache_rays;         // rays per texel
    vec4 cache_params;       // x: atlas side, y: hysteresis
    uvec2 relevance;         // a bit per scene slot: the actor can show up in this view (cull.comp pass 0)
    float gi_range;          // cm: what is nearer than this can light the view or be reflected in it
    float ups_exposure;      // > 0: the pre-exposure this frame renders with, chosen on the CPU (a vendor's upscaler: render.c)
    uvec2 mesh_words;        // UintBuf: the meshes' morph targets and vertex colors (MeshInfo.vcol_first)
    uvec2 decals;            // DecalBuf (M6: decal.c)
    uvec2 decal_tiles;       // UintBuf: per 16 px tile a count then up to DECAL_TILE_MAX decal indices (decal_bin.comp)
    uint decal_count, decal_tiles_x, decal_tiles_y, decal_pad;
    // Weather (weather.c, weather.glsl)
    uvec2 weather_sky;       // SkyBuf: the sky light's SH with the clouds over it
    uint weather_flags;      // WEATHER_FOG, WEATHER_FOG_VOLUME, WEATHER_CLOUDS
    uint weather_pad;
    uvec2 cloud_tables;      // the clouds' coverage thresholds (65) and prefiltered means (17 x 17) (cloud_noise.c)
    vec4 fog0;               // x: density (1/cm) at the base height, y: falloff (1/cm), z: base height (cm), w: max opacity
    vec4 fog1;               // albedo rgb, anisotropy
    vec4 fog2;               // emissive rgb (nits), start distance (cm)
    vec4 fog3;               // x: the froxels' reach (cm)
    vec4 cloud0;             // coverage, extinction at density 1 (1/m), bottom, top (cm)
    vec4 cloud1;             // the wind's offset xy (cm), detail, shadow strength
    vec4 cloud2;             // albedo rgb, the cirrus's amount (0-1)
    vec4 cloud_shadow_origin; // the shadow map's center xy and side (cm)
    vec4 cache_events[8];    // the surface cache: a Movable actor started or stopped moving here (xyz, radius cm)
    uint cache_nevents;
    uint ce_pad0, ce_pad1, ce_pad2;
    uvec2 materials, mat_programs, mat_params, object_cost; // material.c: GpuMaterial slots, programs, parameters; the object cost view's values (a float per slot)
    vec4 pp[8];          // the Post Process Volumes blended at the camera (post.c, render.c fill_post)
    mat4 prev_view_proj; // last frame's view_proj (the camera's motion blur)
    mat4 cloud_prev_vp;  // last frame's view_proj, always (the screen's clouds' history: weather.comp pass 6)
    vec4 cloud_scr;      // their size (px), a march's weight over the history (0: none), on
    // The sun's virtual shadow maps (vsm.c, vsm.glsl): per level world -> light space (x, y cm from its center, z depth
    // 0..1); its half side, texel, depth range (cm), drawn (1); this frame's draw into it (corner cm, 2 / size)
    mat4 vsm_mat[8];
    vec4 vsm_level[8];
    vec4 vsm_job[8];
    vec4 vsm_params;         // levels, PCSS taps, tan(the sun's angular radius)
    uvec2 lsm_tab;           // the local lights' shadow maps (lsm.c, lsm.glsl): the frame's table
    uint lsm_cull;           // the lights drawn this frame (the cull's spheres in the table)
    uint lsm_flags;          // the filter's taps (4, 9)
    vec4 lsm_params;         // 1 / the atlas's side, the faces' border (texels)
    uvec2 clusters;          // ClusterBuf (cull.comp pass 6)
    uvec2 cl_work;           // render_gpu.h CL_WORK_BYTES: dispatch, cursors; per instance a draw, its clusters; per block its seen ones
    uvec2 cl_idx;            // the visible clusters' indices (the instances' draws read them)
    uvec2 cl_perm;           // per mesh LOD's clusters (from its first: 6 x count), front to back along +X -X +Y -Y +Z -Z
    uint cl_entries;         // the instances' room (0: clusters off)
    uint cl_idx_cap;         // the indices' room
    mat4 tsr_vp;             // tsr.comp: this frame's view_proj without its jitter
    mat4 tsr_prev_vp;        // last frame's (without its jitter)
    mat4 tsr_inv;            // the inverse of the jittered view_proj (the depth back to the world)
    vec4 tsr;                // jitter xy (render px), the velocity pass ran, no history
    vec4 tsr_size;           // the render's size, the output's (px)
    uvec2 water_addr[8];     // water.c (water.glsl): the rain's surface, its water, their flow, the ripples, the ocean's spectrum, FFT, maps
    vec4 water[96];          // its numbers (water.glsl)
    vec4 ar[6];              // AR (ar_arcore.c, ar.glsl): the picture's corners, on, the shadows' strength; the real depth's
    uvec2 fog_volumes;       // (A9.5b) the Local Fog Volumes (weather.c): 6 vec4s each
    uint fog_volume_count, fog_volume_pad;
    vec4 fade;               // (B9) the picture faded to rgb by a (the Sequencer's Fade track)
    vec4 grade_lut;          // (A9.4) the Color Grading LUT: x intensity, y its size N (composite.frag, binding 80); z the bokeh's blades (0 round), w their rotation
    vec4 hdr;                // (A9.4) HDR10 output: x on, y paper white (nits), z peak (nits), w the UI target to lay over (binding 81)
    vec4 style[5];           // (2026-11-10) Stylized: bands, softness, saturation, outline px; shadow tint, edge depth step; outline color, grain; shadow denoise, lens distortion, fringes, halation; vignette
    uvec4 rgi;               // (2026-11-14) ReSTIR GI (den_trace.comp): its samples' address (xy); zw free
    uvec4 rgi_info;          // x on; y (Q4) the glass boxes in use; the rest free
    vec4 glass[16 * 4];      // (2026-11-17, Q4) the colored shadow without rays: 16 boxes (center + material slot bits, 3 half axes)
    vec4 terror[8];          // (Q13) the horror kit (render_gpu.h GpuFrame.terror: the filters, letterbox, tension, clock, dip, exposure)
    uvec2 tess_buf;          // (Q17) Nanite Tessellation (tess.glsl): R.tess (header, draws, triangles, records), the patterns' vertices
    uvec2 tess_pat;
    vec4 tess;               // x px per micro edge (the budget's scale in), y the most factor
    uvec4 tess_u;            // x TESS_ flags, y the triangles' room (0: none)
    uvec2 far_buf;           // (Q21) the distant view (farview.glsl): the heightfield and its horizon
    uvec2 imp_tab;           // ... the impostors' table (a uvec4 per mesh slot)
    uvec4 far_u;             // FAR_ flags, the card's draw (slot x 4), its first vertex, the heightfield's side
    vec4 far_f;              // the heightfield's origin xy, texel (cm), the impostors' switch (screen radius, px)
    vec4 far_g;              // the far shadow's strength, the sun's elevation (rad), the fog's sky share, -
    uvec2 mr_boxes;          // (Q25b) mixed reality (mr.glsl): the room's boxes and the Passthrough Windows
    uint mr_box_count, mr_flags;
    vec4 mr[12];             // its numbers (mr.glsl's list)
    GpuLight lights[];
};

// The material textures' mip bias under TSR or an upscaler: log2 of the render's share of the output (the textures at
// the output's sharpness, as the Unreal's screen percentage bias) and one mip more (FSR's and DLSS's -1: the jitter
// accumulated over the frames resolves it; without it a checker floor lost its fine pattern a few tiles away, #blur
// 2026-11-23); 0 without one
float tsr_mip_bias(vec4 tsr_size) { return tsr_size.z > 0.0 ? clamp(log2(tsr_size.x / tsr_size.z) - 1.0, -3.0, 0.0) : 0.0; }
struct Vertex {
    vec3 pos;
    uint normal_oct; // snorm16x2 octahedral
    vec2 uv;
    vec2 uv2;         // the lightmap's unwrap: charts that don't overlap, inside [0, 1]
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer VertexBuf { Vertex v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer IndexBuf { uint i[]; };

struct Instance {
    vec4 row0, row1, row2;  // affine object -> world (rows)
    vec4 base_metal;        // base color, metallic
    vec4 emissive_rough;    // emissive, roughness
    uvec4 ids;              // x: the entity (stable colors in Solid Random), y: INST_ flags, z: first index, w: vertex offset
    vec4 lightmap_st;       // the instance's UV2 rectangle in the lightmap atlas: uv * xy + zw
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer InstanceBuf { Instance i[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer UintBuf { uint v[]; };
// A device address (two uints) plus a byte offset
uvec2 addr_add(uvec2 a, uint off) { uint lo = a.x + off; return uvec2(lo, a.y + (lo < a.x ? 1u : 0u)); }
struct MeshInfo {
    vec4 sphere;
    uint lod_count; float lod_error[3]; // LODs 1-3: how far inside LOD 0 (the BLAS) they can be (cm, mesh space)
    uvec4 first_index;
    uvec4 index_count;
    ivec4 vertex_offset;
    uvec2 blas;
    vec2 uv2_wrap;   // a UV2 chart closed on itself (round meshes): x in [w.x, 1 - w.x] where y < w.y (0: none)
    uvec4 cluster_first; // per LOD: its clusters (ClusterBuf; count 0: drawn whole), cull.comp pass 6
    uvec4 cluster_count;
    uint dag_first, dag_count; // its cluster LOD DAG (count 0: none)
    uint vcol_first;           // its vertex colors in f.mesh_words (RGBA8, sRGB; 0xFFFFFFFF: none)
    uint tess_first;           // (Q17) its canonical table in f.mesh_words (tess.glsl; 0xFFFFFFFF: none)
};
// A decal (render_gpu.h GpuDecal): world -> its box (rows), its screen rectangle (render px), its axis (w: opacity), the
// box's Y and Z (w: its material slot; the cosine past which it fades)
struct Decal { vec4 r0, r1, r2, rect, axis, u_axis, v_axis; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer DecalBuf { Decal d[]; };
#define DECAL_TILE 16u
#define DECAL_TILE_MAX 31u
struct Cluster { vec4 sphere; uint first_index; uint index_count; float self_err; float parent_err; vec4 self_sphere; vec4 parent_sphere; }; // (render_gpu.h GpuCluster)
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer ClusterBuf { Cluster c[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MeshInfoBuf { MeshInfo m[]; };

layout(push_constant, scalar) uniform Push {
    uvec2 frame;
    uvec2 vertices;
    uvec2 instances;
    uint pass;       // 0 main, 1 shadow, 2 selection mask (7: Q11, the outline's ids: the instance itself, the jittered view)
    uint pad;
} pc;

// An orthographic view (the editor's Top / Front / Left...): camera_up.w is its half height (cm); 0: perspective
bool view_ortho(FrameBuf f) { return f.camera_up.w > 0.0; }

// Where the camera ray of a viewport position (NDC) starts: the camera, or (orthographic) the position's point on the
// view plane, RENDER_ORTHO_BACK (render.h) behind the point looked at
vec3 view_origin(FrameBuf f, vec2 ndc) {
    if (!view_ortho(f)) return f.camera_pos.xyz;
    float h = f.camera_up.w;
    return f.camera_pos.xyz + f.camera_right.xyz * (ndc.x * h * f.camera_right.w) - f.camera_up.xyz * (ndc.y * h) - f.camera_fwd.xyz * 500000.0;
}

// The camera ray through a viewport position in NDC (horizontal FOV, like Unreal; orthographic: the view's direction)
vec3 view_ray(FrameBuf f, vec2 ndc) {
    if (view_ortho(f)) return f.camera_fwd.xyz;
    float t = f.camera_fwd.w;
    return normalize(f.camera_fwd.xyz + f.camera_right.xyz * (ndc.x * t) - f.camera_up.xyz * (ndc.y * t / f.camera_right.w));
}

// A unit vector on the octahedron unfolded in [-1, 1]^2 (the probes' texels) and back
vec2 oct_wrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 oct_encode_f(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : oct_wrap(n.xy);
}
vec3 oct_decode_f(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = oct_wrap(n.xy);
    return normalize(n);
}

vec3 oct_decode(uint p) {
    vec2 e = vec2(float(int(p << 16) >> 16), float(int(p) >> 16)) / 32767.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// The sky light's irradiance E(n) (lux): the sky on SH order 2, convolved with the cosine lobe on the GPU
// (atmosphere.comp, for a sun of 1 lux), times the sun's illuminance and the Sky Light's intensity
vec3 sh_irradiance(FrameBuf f, vec3 n) {
    SkyBuf s = SkyBuf(f.sky);
    vec3 r = s.sh[0].rgb;
    r += s.sh[1].rgb * n.y + s.sh[2].rgb * n.z + s.sh[3].rgb * n.x;
    r += s.sh[4].rgb * (n.x * n.y) + s.sh[5].rgb * (n.y * n.z) + s.sh[6].rgb * (3.0 * n.z * n.z - 1.0);
    r += s.sh[7].rgb * (n.x * n.z) + s.sh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0)) * f.sun_illuminance.rgb * f.sky_params.x;
}

// What the shaders that light write into the HDR target multiply by: last frame's exposure (exposure.comp), so
// that 100 000 lux fits in half floats. The composite divides it back out.
// (a vendor's upscaler running: the CPU's value, the one DLSS / FSR are told, so their history follows every change)
float pre_exposure(FrameBuf f) { return f.ups_exposure > 0.0 ? f.ups_exposure : ExposureBuf(f.exposure).exposure; }
