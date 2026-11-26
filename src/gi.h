// Global illumination (notas-dev/gi.md): the ray tracing structures (a BLAS per mesh, a TLAS of every instance
// rebuilt each frame), the dynamic probes (a grid around the camera updated with ray queries, history in world space),
// the Path Tracing view mode (the reference), and the baked lightmaps (gi_bake.c). Without hardware ray tracing every
// call here is a no-op and the renderer falls back to the sky light.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "render_gpu.h"

struct VkCtx;

typedef struct GiMesh {
    uint32_t first_index, index_count;
    int32_t vertex_offset;
} GiMesh;

typedef struct GiSettings {
    bool probes, reflections;
    int rays_per_probe;       // 32 or 64
    float probe_spacing;      // cm
    float intensity;          // the indirect light's scale (1: physically right)
    float reflection_roughness; // traced reflections below this roughness
    bool contact_ao;            // the Movable actors darken the indirect light around them (any GPU)
    bool gtao;                  // screen space ambient occlusion on the indirect light (gtao.comp, A4a: High and up)
    bool ssr;                   // screen space reflections where no ray traces them (A4c: last frame's color and depth)
    bool contact_shadows;       // the sun's screen space contact shadows (gtao.comp, A4b: where no ray shades the sun: Low, GPUs without ray tracing)
    bool den_checker;           // the unified denoiser's rays in a checkerboard (GI_DEN_CHECKER): half of them, Low to Epic
    bool restir_gi;             // (2026-11-14) the denoiser's rays reused by their neighbors (ReSTIR GI, den_trace.comp): High up
    int sss_samples;            // the skin's screen space scattering's samples a pixel (sss.comp, A9.3a; 0: 16)
    bool vrs;                   // variable rate shading by last frame's detail (vrs.comp, A9.7b: opt-in, where the GPU has it)
    bool rt_shadows;            // ray traced shadows for every light (else: the sun's shadow map only)
    int shadow_rays;            // per area light (Source Radius, rect) and pixel: the penumbra's samples
    bool surface_cache;         // real-time radiosity on the surfaces near the camera (gi_cache.c)
    int cache_rays;             // its rays per texel and update
    float cache_texel;          // its density (cm per texel)
    int cache_per_frame;        // texels updated per frame
    bool volumetric_fog;        // the height fog lit per froxel (shadows, local lights); else analytic
    bool screen_clouds;         // the clouds marched at a quarter of the view (their detail); else the panorama alone
    bool clouds_half;           // Epic: at half the view each way (a quarter of its pixels marched a frame): sharper edges
    // The sun's virtual shadow maps (vsm.c): levels (0: none), texels a side, PCSS taps, levels drawn whole a frame, the
    // sun's angular diameter (degrees); vsm_sun: the sun on them even when the other lights are traced
    int vsm_levels, vsm_res, vsm_taps, vsm_full_budget;
    float vsm_sun_angle;
    bool vsm_sun;
    // The local lights' shadow maps (lsm.c), used when their shadows aren't traced (Low, a GPU without ray tracing): the
    // lights with a map at once, the atlas's side, a face's largest side, the filter's taps (4 or 9), faces redrawn a frame
    int lsm_lights, lsm_atlas, lsm_res, lsm_taps, lsm_redraw;
    // The imported meshes' LODs (mesh_build.c, cull.comp): the error on screen a LOD may have (px). Epic 1, High 2,
    // Medium 4, Low 8, Cinematic 0.5 (a card thinned under ~4 px of radius at 1)
    float lod_error_px;
    // Temporal super resolution (render.c, tsr.comp): the share of the output's width and height rendered (1: native,
    // the temporal AA alone; 0: off, MSAA alone)
    float tsr_scale;
    // (Q17) Nanite Tessellation (tess.c): the displaced materials on; their DAG clusters diced to tess_px px per micro
    // edge (Epic 2, High 4, Medium 8), at most tess_max per base edge (1: displaced, not diced: Low), under tess_budget
    // million micro triangles a frame (the px scaled up past it)
    bool tessellation;
    float tess_px;
    int tess_max;
    float tess_budget;
    // (Q21) the distant view (farview.c): the terrain's far shadow (a heightfield of far_res texels a side over the
    // landscapes, read past the sun's virtual maps); the impostors (a card per far instance under impostor_px of screen
    // radius, atlases of impostor_res a side); fog_sky: the analytic fog's ambient light from the sky's horizon (0..1)
    bool far_shadow;
    int far_res;
    bool impostors;
    float impostor_px;
    int impostor_res;
    float fog_sky;
} GiSettings;

// After the meshes exist: the BLAS of each mesh, the probe atlases, the pipelines (layout: render.c's)
bool gi_init(struct VkCtx* vk, VkPipelineLayout layout, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices,
             const GiMesh* meshes, int nmeshes);
void gi_shutdown(struct VkCtx* vk);
bool gi_available(void);
bool gi_sw(void);     // (A1) the GI's rays by software: no ray queries on this GPU (sw_rt.c)
bool gi_traced(void); // the GI traces rays, by hardware or by software (the probes, the surface cache)
// (A2) whether the far probes are worth their cost: the scene reaches past the near grid around the camera
void gi_set_far_needed(bool need); // hardware ray queries
// The views render.c puts in its descriptor set (valid even without ray tracing: 1x1 stand-ins)
VkImageView gi_probe_irradiance_view(void);
VkImageView gi_probe_distance_view(void);
VkImageView gi_accum_view(void);
VkImageView gi_lightmap_view(int i); // 0: L0, 1: L1, 2: the walls around each texel (gi.glsl atlas_read)
GpuImage* gi_lightmap_image(int i);
// The lightmap images at side x side, cleared (gi_bake.c; render_rebind_descriptors after it)
bool gi_lightmap_resize(struct VkCtx* vk, uint32_t side);
bool gi_lightmap_make(struct VkCtx* vk, GpuImage out[3], uint32_t side); // (Q16: a thread's; then gi_lightmap_swap on the frame)
void gi_lightmap_swap(struct VkCtx* vk, GpuImage in[3]);
// The path tracer's accumulation image at the viewport's size (render.c calls it with its own targets)
bool gi_resize(struct VkCtx* vk, uint32_t w, uint32_t h);

// The frame's GI fields (probe grid around the camera, flags)
void gi_fill_frame(GpuFrame* fr, const float camera[3], uint32_t frame_index, const GiSettings* s, bool lightmaps);
// The scene's TLAS (which 0: the static scene; 1: the dynamic set, 0 when empty) from n instance descriptors on the GPU (render.c's tlas_fill.comp writes them): kept, refit (only
// transforms moved) or rebuilt; returns its device address, 0 without ray tracing. Recorded in cmd, ready for the
// compute and fragment shaders after it.
typedef enum GiTlasWork { GI_TLAS_KEEP = 0, GI_TLAS_REFIT, GI_TLAS_BUILD } GiTlasWork;
VkDeviceAddress gi_tlas(struct VkCtx* vk, VkCommandBuffer cmd, int which, VkDeviceAddress instances, uint32_t n, GiTlasWork work);
VkDeviceAddress gi_blas_address(int mesh); // the mesh's BLAS (0 without ray tracing)
// A BLAS of its own over vertices that change (a skin's, render.c): built the first time and every 64 updates (its
// quality), refit in between (fast build, update allowed). The vertices: stride GpuVertex, uint32 indices
typedef struct GiDynBlas {
    GpuBuf buf, scratch;
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
    uint32_t prims, updates;
    uint32_t stride;    // the vertices' stride (0: GpuVertex)
    bool always_build;  // built every time (its indices change: a torn cloth), never refit
} GiDynBlas;
bool gi_dyn_blas_create(struct VkCtx* vk, GiDynBlas* b, uint32_t prims, uint32_t maxv);
void gi_dyn_blas_record(VkCommandBuffer cmd, GiDynBlas* b, VkDeviceAddress vertices, uint32_t maxv, VkDeviceAddress indices);
// (2026-11-24, item 4) many at once (one build call, n infos)
typedef struct GiDynBlasJob { GiDynBlas* blas; VkDeviceAddress vertices, indices; uint32_t maxv; } GiDynBlasJob;
void gi_dyn_blas_record_batch(VkCommandBuffer cmd, const GiDynBlasJob* jobs, uint32_t n);
void gi_dyn_blas_free(struct VkCtx* vk, GiDynBlas* b);
// A Masked mesh's BLAS rebuilt with an opacity micromap (data: 16 bytes a triangle, render.c's omm.comp); false: unchanged
bool gi_blas_omm(struct VkCtx* vk, int mesh, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices, const GiMesh* m,
                 VkDeviceAddress data, uint32_t ntri, uint32_t level);
// The meshes changed (render_meshes_reload): every BLAS made again
void gi_rebuild_blas(struct VkCtx* vk, const GpuBuf* vertices, uint32_t vertex_count, const GpuBuf* indices, const GiMesh* meshes, int nmeshes);
// The same in two parts: the sizes, the buffers and the structures made on any thread (NULL: no ray tracing), then the
// builds recorded on the main thread with the GPU idle (the old BLAS replaced); drop: one not committed
typedef struct GiBlasPrep GiBlasPrep;
GiBlasPrep* gi_blas_prepare(struct VkCtx* vk, VkDeviceAddress vertices, uint32_t vertex_count, VkDeviceAddress indices, const GiMesh* meshes, int nmeshes,
                            const GpuVertex* cpu_v, const uint32_t* cpu_i); // (the CPU copy the clusters are made from, A8; NULL: at the commit)
void gi_blas_commit(struct VkCtx* vk, GiBlasPrep* p);
void gi_blas_drop(struct VkCtx* vk, GiBlasPrep* p);
VkDeviceAddress gi_probe_rays_address(void);
VkDeviceAddress gi_probe_state_address(void);
// The probes' rays and update (a quarter of the probes per frame, all during a bake), or the baked volume put back in
// the atlases; the descriptor set and push constants are render.c's
void gi_update_probes(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, const GpuFrame* fr);
// The path tracer into the HDR target (storage binding 8), accumulating while sample > 0
void gi_path_trace(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t w, uint32_t h);
void gi_view2(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t w, uint32_t h); // (render_view2: push.pass / pad = its parameters' address)
// The contact GI's sources: each occluder's light (occluder_light.comp) for the main pass; ray tracing only (the flag
// GI_CONTACT_BOUNCE says the main pass may read it)
void gi_light_occluders(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, GpuFrame* fr);
// The probes cleared (a level opened, the GI turned on): they rebuild from zero
void gi_reset_probes(void);

// The baked probe volume (notas-dev/gi.md, layer 2): during Build Lighting the probe grid is fitted to the Static
// meshes' bounds and every probe is traced each frame against them only; at the end its atlases are kept (host
// memory) and saved in the .rlight. It lights what has no lightmap when the dynamic probes are off or the GPU has no
// ray tracing (then nothing is traced: the atlases are sampled as they are).
typedef struct GiVolume {
    int32_t min[3];
    float spacing;
} GiVolume;
void gi_volume_bake_begin(const float lo[3], const float hi[3]);
// (Q16) a bake in the game (runtime_lighting.c): the bake's probes traced a stride at a time, as the dynamic ones (1: all
// each frame, the editor's Build Lighting)
void gi_volume_bake_stride(uint32_t stride);
void gi_bake_set_no_emissive(bool on); // (Lighting Levels, sublevels.c) the next bakes: rays see no emissive (summed with a base bake that has it)
// The bake ended: keep (the atlases copied in cmd, readable once that frame is done) or drop the volume
void gi_volume_bake_end(VkCommandBuffer cmd, bool keep);
bool gi_volume_present(void);
const GiVolume* gi_volume(void);
// The atlases (irradiance RGBA16F then distance RG16F, PROBES_X*PROBES_Z*10 x PROBES_Y*10 each)
const void* gi_volume_data(size_t* bytes);
size_t gi_volume_bytes(void);
void gi_volume_dims(uint32_t* w, uint32_t* h); // the probe atlas's size (its irradiance: RGBA16F)
// A volume from a file (the probe images at full size even without ray tracing: render_rebind_descriptors after it)
bool gi_volume_load(struct VkCtx* vk, const GiVolume* v, const void* data, size_t bytes);
void gi_volume_clear(void);

typedef struct GiStats {
    uint32_t probes, probes_per_frame, rays_per_frame;
    uint32_t tlas_instances, tlas_builds, tlas_refits;
} GiStats;
const GiStats* gi_stats(void);
// The probe slots' check (the measures, --bench): on, the GPU's count of probes that found another cell of the grid
// in their slot is copied back each frame; gi_probe_collisions: how many since it was turned on (0 when right)
void gi_probe_check(bool on);
uint32_t gi_probe_collisions(void);
