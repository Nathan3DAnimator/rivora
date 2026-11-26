// The scene renderer (Vulkan 1.3, forward PBR): the viewport's scene into its own HDR targets, then composited into
// the swapchain under the UI. Passes: directional shadow map, main (MSAA: meshes, sky, grid), selection mask,
// composite (exposure, ACES, sRGB, outline). Buffers reached by device address (vertex pulling, no vertex input),
// one instanced draw per mesh, CPU frustum culling (GPU-driven culling comes with imported meshes).
// Every pass has GPU timestamps (RenderStats).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "gi.h"

struct VkCtx;
typedef struct VkCommandBuffer_T* VkCommandBuffer;

// Unreal's view modes and buffer views, and Blender's Solid (studio light, gray or random colors) and Texture (only the
// base color / textures, no light)
typedef enum ViewMode {
    VIEW_LIT = 0, VIEW_UNLIT, VIEW_WIREFRAME, VIEW_LIGHTING_ONLY, VIEW_NORMALS,
    VIEW_SOLID, VIEW_SOLID_RANDOM, VIEW_TEXTURE, VIEW_ROUGHNESS, VIEW_METALLIC,
    VIEW_PATH_TRACING, VIEW_GI_ONLY, // the reference, and only the indirect light (E / pi)
    // the technologies at work (Unreal's Nanite / Lumen visualizations): the LOD the GPU cull drew, the triangles, the
    // mobility; the surface cache, which GI source lights a pixel, the texel density; the lights per pixel, the shading cost
    VIEW_LOD, VIEW_TRIANGLES, VIEW_MOBILITY, VIEW_SURFACE_CACHE, VIEW_GI_SOURCE, VIEW_TEXEL_DENSITY, VIEW_LIGHT_COMPLEXITY,
    VIEW_SHADER_COST,
    VIEW_OBJECT_COST, // what each object costs (sugestão #65): its meshes, animation, Blueprint, physics, AI, lights near
    VIEW_MODE_COUNT
} ViewMode;
extern const char* const g_view_mode_names[VIEW_MODE_COUNT];
// VIEW_OBJECT_COST's values (sugestão #65): a float per scene slot (0 green .. 1 red), made on first use; NULL: none
struct VkCtx;
float* render_object_cost_map(struct VkCtx* vk, uint32_t* count);

// The second 3D view (render_view2, view2.comp): its camera, how it draws; ray traced into a picture of its own after
// the main view (the same scene, the same frame), so none of the main view's state is shared
enum { VIEW2_LIT = 0, VIEW2_UNLIT, VIEW2_NORMALS, VIEW2_PATH_TRACING, VIEW2_MODE_COUNT };
typedef struct View2 {
    float pos[3], fwd[3], right[3], up[3]; // cm; Z up
    float fov_deg;                          // horizontal (perspective)
    float ortho_half;                       // cm: the half height of an orthographic view (0: perspective)
    uint32_t w, h, mode;
} View2;
// Records it (after render_scene, in the same command buffer: its TLAS and frame data); false: no ray tracing or no room.
// Its picture (RGBA8, sRGB-encoded, ready to sample by the UI): render_view2_image, its generation bumps when it is
// made again (the UI registers it again); the path tracer's samples so far
bool render_view2(struct VkCtx* vk, VkCommandBuffer cmd, const View2* v);
// AR (ar_arcore.c): this frame's camera picture behind the scene (in the sky's place: y w x h and uv w/2 x h/2
// interleaved, YUV 4:2:0, copied when serial changes; corners: where the screen's corners fall in it, ar_arcore.h's order) and the real
// floor's triangles that receive the shadows (xyz, cm; shadow: the main light's share of its light). on false: the sky
void render_ar_frame(bool on, const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, uint64_t serial, const float corners[4][2],
                     const float* tris, uint32_t nverts, float shadow); // (serial: the picture copied only when it changes)
// AR's real depth (ar_arcore.h: mm along the view axis, 0 unknown; NULL: none): the real things nearer than the virtual
// ones hide them (ar_occlude.frag in the sky's place)
void render_ar_depth(const uint16_t* mm, uint32_t w, uint32_t h, uint64_t serial, const float corners[4][2]);
// (Q25b) Mixed reality (xr_mr.h, shaders/mr.glsl): each VR frame, before the eyes. The eye's image gets an alpha for the
// headset's passthrough (0 where the real room shows: the empty background, a Passthrough Window, a real thing nearer than
// the virtual one); the virtual things' sun shadow darkens the real surfaces; the simulated headset draws a stand-in room
// in its place, styled. Boxes: world -> the box's unit cube as three rows (xyz, offset), then kind (0 the room, 1 a
// window) and label. The real depth: the headset's (its image: D16, 2 layers, w x h) or a CPU one (2 x w x h D16 values), and each
// eye's depth camera (world, cm). NULL or on false: off
#define MR_MAX_BOXES 64
typedef struct RenderMr {
    bool on, background, sim, room_occludes, shadows, unpremultiplied;
    float virtual_opacity, soft_cm, shadow_strength;
    float pt_opacity, brightness, contrast, saturation, tint[3]; // (the simulated look: the style)
    int pt_style;
    float lamp[3];
    const float (*boxes)[4][4];
    uint32_t nboxes;
    bool depth, depth_flip_y;
    VkImage depth_image;             // (the headset's: copied into the renderer's own)
    const uint16_t* depth_cpu;       // (the simulated one: both layers)
    uint32_t depth_w, depth_h;
    uint64_t depth_serial;           // (a new one each time it changes)
    float depth_near_cm, depth_far_cm; // (0 far: infinite)
    float dpos[2][3], dfwd[2][3], dright[2][3], dup[2][3], dtan[2][4]; // each eye's depth camera; tangents left, right, up, down
} RenderMr;
void render_mr_frame(const RenderMr* m);
VkImageView render_view2_image(uint32_t* generation);
uint32_t render_view2_samples(void);
uint32_t render_view2_frames(void); // drawn so far (the tests)

// The asset previews (render_preview.c): Unreal's asset editors' viewports (the Skeletal Mesh / Animation / Static Mesh
// editors, the Blueprint Viewport, the Blend Space and Anim Graph previews) drawn by the GPU, each into a picture of its
// own: the meshes with their materials, a studio light with its shadow, 4x MSAA, the floor and its grid. Asked for while
// the UI is built (the draws are copied), recorded by render_preview_record after the scene.
// (Q7, 2026-11-17) Albedo, AO (the subject's own occlusion: a depth pass and a screen-space hemisphere), UV (a checker by
// the texture coordinates), LOD (each mesh tinted by the LOD drawn: white, red, green, blue as Unreal's LOD Coloration),
// Overdraw (every surface added up without a depth test: the layers a pixel pays for)
enum { PREVIEW_LIT = 0, PREVIEW_UNLIT, PREVIEW_WIREFRAME, PREVIEW_LIT_WIREFRAME, PREVIEW_LIGHTING_ONLY, PREVIEW_NORMALS, PREVIEW_ROUGHNESS,
       PREVIEW_METALLIC, PREVIEW_ALBEDO, PREVIEW_AO, PREVIEW_UV, PREVIEW_LOD, PREVIEW_OVERDRAW, PREVIEW_MODE_COUNT };
#define PREVIEW_FLOOR 1u       // the floor under it (its shadow)
#define PREVIEW_GRID 2u        // the grid on the floor
#define PREVIEW_SKY 4u         // (Q7) an outdoor sky behind it and in its reflections (else the studio's backdrop)
#define PREVIEW_LIGHT_WORLD 8u // (Q7) the key light from light_dir (world: turned by the user), not turned with the camera
typedef struct PreviewView {
    float eye[3], fwd[3], right[3], up[3]; // cm, Z up (right: the screen's)
    float fov_deg;                          // vertical
    float target[3], radius;                // what it turns around and the subject's size (cm): the floor's middle
    float floor_z;
    uint32_t w, h, mode, flags;             // px; PREVIEW_ mode and flags
    float time;                             // s: the animated materials
    float light_dir[3];                     // (PREVIEW_LIGHT_WORLD) toward the key light
    float light_scale;                      // the key light's intensity (0: 1)
} PreviewView;
typedef struct PreviewDraw {
    uint32_t mesh, material;                // a mesh's id; a material's hash (0: the color)
    float rows[3][4];                       // object -> world (affine rows)
    float color[3], roughness, metallic, emissive[3], uv_scale;
    bool selected;                          // tinted (Unreal's selection)
    // (2026-11-10) a Skeletal Mesh's skin: its bones' palette (pose x bind inverse, the mesh's space; NULL: rigid). The
    // preview bends its vertices on the CPU (an editor's preview: a few at once), the morph targets left out
    const float (*palette)[3][4];
    uint32_t nbones;
    uint32_t lod;                           // (Q7) the LOD drawn (clamped to the mesh's; a skin has one)
} PreviewDraw;
// The preview with this key (the window's own number) drawn this frame: its picture (NULL: not yet, or no room), its
// generation (bumped when it was made again: the UI registers it again) and the part of it the view fills (uv_max)
VkImageView render_preview(uint32_t key, const PreviewView* v, const PreviewDraw* d, uint32_t n, uint32_t* gen, float uv_max[2]);
bool render_preview_available(void);
void render_preview_record(struct VkCtx* vk, VkCommandBuffer cmd); // main.c, after render_scene
typedef struct PreviewStats {
    double gpu_ms, cpu_ms, last_cpu_ms;     // last measured GPU time (timestamps) of all of them; their CPU (asking)
    uint32_t views, frame_views, msaa;
    uint64_t triangles, frame_triangles, drawn_frames;
    uint32_t made;                          // pictures made (again: a new size)
    bool wireframe;                         // the GPU draws lines (fillModeNonSolid)
} PreviewStats;
const PreviewStats* render_preview_stats(void);
// (Q7, the asset viewport's test) the picture of key copied to the CPU after the next frame that draws it: ask, then
// read until true (RGBA8, w x h, display values); false meanwhile
void render_preview_readback(uint32_t key);
bool render_preview_read(uint32_t key, const uint8_t** rgba, uint32_t* w, uint32_t* h);
// A mesh's LODs (count; their triangles and error in cm against LOD 0, NULL: not wanted) and the one the scene's cull
// would draw at that distance (cm), scale and focal length (px per cm at 1 cm: the view's height / 2 / tan(fov / 2))
uint32_t render_mesh_lods(uint32_t mesh, uint32_t tris[4], float error[4]);
uint32_t render_mesh_nanite(uint32_t mesh, uint32_t* lod_clusters, bool* rigid); // (2026-11-24) its DAG's clusters as loaded (0: none)
bool render_mesh_is_skinned(uint32_t mesh); // (2026-11-25) a skin's mesh (uv2's bits: 4 bones and weights)
// (2026-11-25, item 17) the selection mask the last frame left: pixels set, a hash of where, the mask's draws (GPU idled)
bool render_mask_read(struct VkCtx* vk, uint32_t* set, uint64_t* hash, uint32_t* draws, uint8_t* copy, size_t cap, uint32_t* w, uint32_t* h); // (copy: the pixels, w x h bytes; NULL: none)
uint32_t render_mesh_lod_for(uint32_t mesh, float scale, float dist, float focal_px);
// (render.c) the previews' start and end, and the shared buffers they draw from: a mesh's LOD 0 range in them
void render_preview_init(struct VkCtx* vk);
void render_preview_shutdown(struct VkCtx* vk);
bool render_mesh_range(uint32_t mesh, uint32_t* first_index, uint32_t* index_count, int32_t* vertex_offset);
// (Q7) the same for one of its LODs (the last one when it has fewer)
bool render_mesh_lod_range(uint32_t mesh, uint32_t lod, uint32_t* first_index, uint32_t* index_count, int32_t* vertex_offset);
VkBuffer render_index_buffer(void);
uint64_t render_vertex_address(void);

#define RENDER_ORTHO_BACK 500000.0f // cm: an orthographic view's reach behind (and in front of) the point it looks at

typedef struct RenderView {
    float pos[3], fwd[3], right[3], up[3]; // the camera (cm; Z up)
    float fov_deg;                          // horizontal
    float ortho_half;                       // cm: an orthographic view's half height (0: perspective). pos is then the
                                            // point looked at; the view reaches RENDER_ORTHO_BACK behind it
                                            // and as far in front
    float x, y, w, h;                       // the viewport in framebuffer pixels
    uint32_t view_mode;                     // ViewMode
    bool show_grid;
    bool editor;                            // hidden-in-editor actors are skipped
    Entity selected;                        // outlined (ENTITY_NONE: none)
    const Entity* selection;                // every selected actor (outlined; selected is the primary one)
    uint32_t selection_count;
    // The physical camera: manual EV100, or auto exposure between two EV100; a compensation in stops for both
    float exposure_ev100, exposure_comp, auto_min_ev100, auto_max_ev100;
    bool auto_exposure;
    float aerial_scale; // the aerial perspective's distances (1: physical, 0: off)
    int msaa;                               // 1, 2, 4 or 8 (clamped to what the GPU has)
    GiSettings gi;
    // The Post Process Volumes blended at the camera (post.c post_blend_at; NULL: the defaults without motion blur, a
    // preview) and which settings a volume overrides; camera_blur: the camera's own motion blurs the view too
    const struct PostSettings* post;
    uint32_t post_overrides;
    bool camera_blur;
    // The game's highlight (interaction.h: the Interactable in focus and its moving part; ENTITY_NONE: none), outlined in
    // its own warm color over the selection's mask; mode: IAR_OUTLINE 1 (the outline), IAR_GLOW 2 (its silhouette lit)
    Entity highlight[2];
    uint32_t highlight_mode;
    // (A9.4) the Post Process Volumes' Color Grading LUT ("/Content/...": .cube or a PNG strip; NULL: none) and its weight
    const char* grade_lut;
    float grade_lut_intensity;
    // (A9.4) the player's Cine Camera (post.c post_fill_view): a manual exposure, a physical depth of field (its lens:
    // focal length and sensor, mm; 0: the post's simple model), the iris's blades (0: round)
    bool cam_manual_exposure;
    float cam_ev100;
    float dof_focal_mm, dof_sensor_mm;
    uint32_t bokeh_blades;
    float bokeh_rot_deg;
    // (Q13) the horror kit's frame (terror.c terror_view; composite.frag, exposure.comp read GpuFrame.terror): the filters,
    // the letterbox, the tension's grain / vignette / saturation / pulse, the clock, the dip, the horror exposure; and its
    // EV floor (auto exposure: never brighter than a scene this dark; -100: none)
    float terror[8][4];
    float terror_ev_floor;
    // (Q25, xr.h) VR: the second eye of a pair (the frame's shared work, the simulations, the GI's updates and the bake's
    // step, done by the first: this one only draws its view); fixed foveated rendering (0 off, 1..3: the periphery shaded
    // at 2x1 / 2x2 by the rate image, around the eye's center; MSAA kept); the comfort vignette (0..1: darkens the view's
    // edge while the VR pawn moves or turns)
    bool stereo_second;
    int foveation;
    float vr_vignette;
    float vr_fade;          // (Q25) the head inside a wall: the view to black (0..1)
    int vr_eye;             // (Q25b) the eye of a VR pair (0 left, 1 right): mixed reality's real depth layer
} RenderView;

typedef struct RenderStats {
    double gpu_scene_ms, gpu_gi_ms, gpu_shadow_ms, gpu_main_ms, gpu_mask_ms, gpu_exposure_ms, gpu_composite_ms; // the frame before last (timestamps)
    double gpu_post_ms;       // the post pass (depth of field, motion blur) and the bloom
    float auto_exposure, avg_log_lum; // the exposure in use (linear), the mean log2 luminance (nits) auto exposure saw
    uint32_t pt_samples;      // the path tracer's accumulated samples
    uint32_t unbuilt;         // Static meshes without a valid lightmap while lightmaps exist ("lighting needs rebuild")
    double cpu_ms;            // gathering instances and lights, recording
    uint32_t instances, visible, draws, lights;
    uint32_t grid_items, grid_capacity; // the local lights' grid (lights.c): items asked for, the room for them
    uint64_t triangles;       // drawn by the main pass (the GPU cull's counts, a frame late)
    bool occlusion;           // the occlusion path this frame (chosen by measuring: render.c occlusion_auto)
    double occlusion_ms[2];   // its last measure: the GPU frame without, with
    uint32_t width, height, msaa;
    // with render_set_detail (the Debug Viewer): the GPU's parts of the GI and the main pass
    bool detail;
    double gpu_atmosphere_ms, gpu_cull_ms, gpu_weather_ms, gpu_probes_ms, gpu_cache_ms, gpu_gi_other_ms, gpu_meshes_ms, gpu_sky_ms;
    double gpu_lsm_ms;        // the local lights' shadow maps drawn (lsm.c; 0: none drawn that frame)
    double gpu_skin_ms;       // the skins' bends (skin.comp, the morph targets in them; 0: none bent that frame)
    double gpu_gtao_ms;       // the screen space AO (gtao.comp; inside the main pass's interval, after its meshes; 0: off)
    double gpu_sss_ms;        // the skin's screen space scattering (sss.comp, A9.3a; inside the main pass's interval; 0: off)
    double gpu_upscale_ms;    // the upscale: TSR or a vendor's (render.c 2c; inside the main pass's interval, after the sky; 0: off)
    double gpu_denoise_ms;    // the unified ray traced denoiser (denoise.comp, A9.1d; inside the main pass's interval; 0: off)
    double gpu_tess_ms;       // (Q17) Nanite Tessellation's dicing (tess.comp, the first DAG phase; inside the main pass's interval; 0: off)
    uint64_t tess_micro;      // ... its micro triangles drawn (in triangles too), base triangles diced, the budget's scale
    uint32_t tess_base;
    float tess_scale;
    int upscaler;             // the one that ran (UPS_*, upscale.h; -1: none)
} RenderStats;

bool render_init(struct VkCtx* vk);
void render_shutdown(struct VkCtx* vk);
// Records the offscreen passes (before the swapchain's rendering begins). Returns false when the view is empty.
bool render_scene(struct VkCtx* vk, VkCommandBuffer cmd, World* w, const RenderView* view);
// Draws the scene into the swapchain's viewport rectangle (inside the swapchain's rendering, before the UI)
void render_composite(struct VkCtx* vk, VkCommandBuffer cmd, const RenderView* view);
// (A9.4) HDR10 output (vk->hdr): the UI drawn into its own target between begin and end (before the swapchain's
// rendering; ImGui's pipeline in render_hdr_ui_format), the composite lays it over the picture; paper white and peak nits
void render_hdr_set(float paper_nits, float peak_nits);
VkFormat render_hdr_ui_format(void);
void render_hdr_ui_begin(struct VkCtx* vk, VkCommandBuffer cmd);
void render_hdr_ui_end(struct VkCtx* vk, VkCommandBuffer cmd);
const RenderStats* render_stats(void);
// (2026-11-24) the main pass's runs of draws the last frame: plain, RE_EXT (only the meshes an RE_EXT material draws), and
// how many meshes are marked so (render.c draw_runs, MESH_INFO_EXT)
void render_ext_runs(uint32_t* plain, uint32_t* ext, uint32_t* meshes);
// The skins (render.c "skin"): how many, bent last frame (their vertices) and the CPU time of their palettes (ms)
void render_skin_stats(uint32_t* skins, uint32_t* bent, uint32_t* verts, double* cpu_ms);
// (2026-11-24) last frame's skins drawn at their bind (a pose that is the bind's, or no room) and of those the ones with no room
void render_skin_bind_stats(uint32_t* at_bind, uint32_t* no_room);
// (2026-11-24, item 4) the rigid skins sent the last frame (every vertex on one bone: the mesh moved by that bone)
uint32_t render_skin_rigid_count(void);
// (2026-11-24, item 4) the dynamic instances dormant the last frame (still 120 frames: static TLAS, cached shadow), slept and woke since the start
void render_dormant_stats(uint32_t* now, uint32_t* slept, uint32_t* woke);
int render_mesh_collision(uint32_t slot); // (2026-11-24) the mesh's Collision Complexity (.rlod): 0 the actor's, 1 simple, 2 complex
bool render_last_camera(float pos[3]); // (2026-11-24) the last frame's camera (cm); false: none drawn yet
// The morph targets (RMESH_MORPHS): the meshes that have them, last frame's skins bent with a weight set and their vertices
void render_morph_stats(uint32_t* meshes, uint32_t* jobs, uint32_t* verts);
// A test's look (morph_test.c): the positions of the skin of the Skeletal Mesh actor owner as the GPU bent them last
// (the GPU idle, a copy back); how many (0: no skin bent yet)
uint32_t render_skin_readback(struct VkCtx* vk, World* w, Entity owner, float (*pos)[3], uint32_t cap);
void render_morph_off(bool off); // (the measure's: the skins bent without their morphs, the bones' cost alone)
// --shader-stats: the bytes the main pass's shaders (mesh, mesh_rt*) spill to local memory (-1: not known, the
// driver has no such statistic). A spill there cost 3-10% of the pass (notas-dev/render.md): the test asks for 0
int64_t render_shader_spill(void);
// The Debug Viewer (debug_viewer.c): the finer timestamps (RenderStats.detail) only while on; each scene slot's
// visibility as the GPU cull left it (cull.comp vis: the draw shape * 4 + LOD in bits 0-11, 0xFFF none; the shadow's in
// bits 16-27), copied on request a frame or two later (the slot is the entity's index)
void render_set_detail(bool on);
void render_want_foveation(bool on); // (Q25) before render_init: VR's foveated rendering (the rate image's pipelines)
void render_request_vis(void);
const uint32_t* render_debug_vis(uint32_t* n);
uint32_t render_lod_triangles(uint32_t shape, uint32_t lod);
// A basic shape's LOD on the CPU (the Blueprint preview draws it): its vertices (GpuVertex, render_gpu.h) and triangles,
// indices relative to *v. False when there is no such mesh.
bool render_shape_cpu(uint32_t shape, uint32_t lod, const void** v, uint32_t* nverts, const uint32_t** idx, uint32_t* nidx);
// The flicker probe (a still camera, --bench): the mean relative change of the scene's luminance between frames
double render_flicker(void);
double render_flicker_spots(void); // the share of the view's points changing more than 2% a frame
double render_flicker_pulses(void); // ... and going back and forth by more than 2% in two frames (with motion in the view)
const uint32_t* render_flicker_map(uint32_t* frames); // 64 x 32: how many frames each point flickered
void render_flicker_reset(void);
double render_ghosting(double* background, double* samples_per_frame); // (Q20) the trail behind moving actors (exposure.comp pass 3)
// The flicker probe's scene luminance (exposure divided out) since the last reset: its brightest point over the frames
// and the mean (--max-scene-lum: the spec_leak test's dark side, notas-dev/luz.md "Especular vazando")
void render_probe_lum(double* max_nits, double* mean_nits);
// The meshes: the basic shapes (ids 0-4), then the imported ones (.rmesh files in Content, the importers' output: their
// id is the hash of their "/Content/..." path with the high bit set). A slot is the mesh's place in the renderer's list.
#define RE_MAX_MESHES 1023 // (cull.comp keeps a mesh x LOD in 12 bits, NONE = 0xFFF, and counts 2 x 1023 x 4 draws in shared memory; 255 until 0.20)
#define RMESH_MAGIC 0x48534D52u // "RMSH"
// 2 (2026-11-06): indexed (welded vertices, cache/overdraw/fetch order), a LOD table, flags (mesh_build.c). 1 still
// opens: one corner per triangle, processed the same way at load
// 3 (2026-11-08): a morph target block after the indices when RMESH_MORPHS is set (else v2's layout): uint32 count,
// uint32 deltas; count names (RMESH_MORPH_NAME bytes) and default weights (float); per vertex its first delta (nverts + 1
// uint32, CSR); the deltas (RMeshMorphDelta). v1 and v2 still open
// 4 (2026-11-09): the vertex colors (RMESH_COLORS: a uint32 RGBA8 per vertex after everything else; M5). v1-v3 still open
#define RMESH_VERSION 4u
#define RMESH_MAX_LODS 4
#define RMESH_NO_ATLAS 1u // no UV2 (uv2 = -1): no lightmap, no surface cache chart (the probes light it)
// skinned (a Skeletal Mesh's skin, weights per vertex): uv2's bits hold its 4 bones (uint8 each) and their weights
// (unorm8, summing to 255); one LOD, no clusters; the renderer deforms it on the GPU each frame (render.c "skin")
#define RMESH_SKINNED 2u
// morph targets (blend shapes, v3): a skin's vertices moved by weight x delta (position and normal) before its bones
// (skin.comp; the weights: CSkeletalMesh.morph_weight by name, notas-dev/animacao.md "Morph targets")
#define RMESH_MORPHS 4u
#define RMESH_MORPH_NAME 48
// vertex colors (v4: glTF's COLOR_0; the material's Color Attribute node reads them: notas-dev/materiais.md "M5")
#define RMESH_COLORS 8u
#define RMESH_MAX_MORPHS 64
typedef struct RMeshMorphDelta { uint32_t target; float dp[3], dn[3]; } RMeshMorphDelta; // (28 bytes, skin.comp's)
typedef struct RMeshHeader {
    uint32_t magic, version, nverts, nindices; // v2: then RMeshLods, nverts GpuVertex, nindices uint32_t (every LOD's)
    float mn[3], mx[3];
    float area;                                // cm² (the surface cache sizes its charts with it)
    uint32_t flags;                            // RMESH_ (v1: 0)
} RMeshHeader;
typedef struct RMeshLods {
    uint32_t count;
    uint32_t first_index[RMESH_MAX_LODS], index_count[RMESH_MAX_LODS]; // (into the file's index list; vertices shared)
    float error[RMESH_MAX_LODS];                                       // cm: how far a LOD's surface strays from LOD 0's
} RMeshLods;
bool render_mesh_has_atlas(uint32_t slot); // a UV2 to bake into (RMESH_NO_ATLAS clear)
uint32_t render_mesh_id_of(const char* vpath); // an imported mesh's id
uint32_t render_mesh_slot(uint32_t mesh_id);   // its slot (a missing one: the cube's)
uint32_t render_mesh_count(void);
uint32_t render_mesh_id(uint32_t slot);
const char* render_mesh_name(uint32_t slot);
const char* render_mesh_vpath(uint32_t slot); // its file's virtual path ("/Content/Meshes/..."; "": a shape)
float render_mesh_area(uint32_t slot);
// The imported meshes read again (after an import): the GPU waits, the buffers, BLAS and mesh table are made again
bool render_meshes_reload(struct VkCtx* vk);
// The same in the background (async.h): a worker reads the files, the main thread sends them (the old meshes drawn
// meanwhile); asked again while running: once more after it
void render_meshes_reload_async(struct VkCtx* vk);
bool render_meshes_reloading(void);
double render_meshes_apply_ms(void); // the last one's main-thread part (ms)
// Counts the reloads: what render_shape_cpu returned before one is gone (the previews' caches compare it)
uint32_t render_meshes_generation(void);
bool render_any_dag(void); // a mesh has a cluster LOD DAG (mesh_clod.h; the rocks test)
bool render_pipelines_pending(void); // the Masked shadow rays' pipelines still being made on their thread
// (2026-11-19) called while pipelines compile on threads (a cold cache): the window's messages pumped, so it keeps
// answering (glfwPollEvents)
void render_set_wait_pump(void (*fn)(void));
// (2026-11-19) the main pass's pipelines for that MSAA compiled on background threads from now (the intro, the Project
// Browser and the level's load hide a cold cache's compile); the first frame takes them
void render_prewarm(struct VkCtx* vk, int msaa);
// (2026-11-19) the debug lines and points (debug_draw.h) over the composite, in its pass (after render_composite)
void render_debug_draw(struct VkCtx* vk, VkCommandBuffer cmd, const RenderView* view);
uint32_t render_debug_drawn(void); // last frame's primitives
// While it compiles: true, with the pipelines done and their count (the loading screen shows them)
bool render_prewarm_progress(uint32_t* done, uint32_t* total);
int render_pipelines_pending_count(void); // (Q26) how many (the status bar's Shader Compile)
void render_set_content_root(const char* content_root); // the project's Content (the next reload reads it)
const char* render_content_root(void);                 // (Q9: a soft body's painted hold reads its mesh's file)
// A mesh's local bounds (cm), for picking and focusing
void render_mesh_bounds(uint32_t mesh, float mn[3], float mx[3]);
// A Movable actor's scene slot moving right now (its transform changed in the last 60 frames: INST_MOVING)
bool render_slot_moving(uint32_t slot);
// A rotator (pitch, yaw, roll degrees) as axes (axes[a]: the world direction of local axis a) and back
void rotator_axes(const float rot_deg[3], float axes[3][3]);
void axes_rotator(const float axes[3][3], float rot_deg[3]);
// The object -> world rows (affine) of a transform, Unreal's rotation order
void render_transform_rows(const CTransform* t, float rows[3][4]);
uint32_t render_emis_pack(const float nits[3], bool lit); // (2026-11-23) a material's mean emission for the rays (GpuInstance.emissive_rough.z)
// The basic shapes on the CPU (the lightmap bake's raster)
// Every mesh's vertices and indices as the GPU has them (the CPU copy; the BLAS of clusters read it: rt_clusters.c)
void render_cpu_mesh_data(const GpuVertex** v, const uint32_t** idx, uint32_t* nv);
struct GiMesh;
int render_gi_meshes(struct GiMesh* gm); // every mesh's range as the GI traces it (LOD 0): its count
int render_sw_meshes(struct GiMesh* gm, int32_t* prim_base, uint32_t max_tris); // (A1: the LOD the software rays trace)
void render_mesh_cpu(uint32_t mesh, const GpuVertex** v, const uint32_t** idx, uint32_t* first_index, uint32_t* index_count,
                     int32_t* vertex_offset);
// The descriptor set again (the GI replaced an image: the lightmaps); waits for the GPU
void render_rebind_descriptors(struct VkCtx* vk);
// Engine Scalability (Unreal's Low/Medium/High/Epic/Cinematic: 0-4): the MSAA and the GI's settings each level uses (the
// editor's Settings menu and the packaged game's game.ini). Cinematic is the Quality rendering mode's (sugestão #45:
// ~90-120 fps instead of 250+): every ray count doubled, the surface cache finer, rougher traced reflections.
#define RENDER_SCALABILITY_LEVELS 5
void render_scalability(int level, int* msaa, GiSettings* gi);
const char* render_scalability_name(int level); // English (TR() it)
// The render style (A9.0, notas-dev/render.md "Estilos de render"): how the frame looks, apart from what it costs (the
// scalability). Realistic is the look so far (ACES) and the default; Photorealistic the AgX tone mapping; Video-realistic
// AgX with the camera's lens and film (halation, grain by the luminance, distortion, fringes, vignette); Stylized flat
// bands of light and stronger colors. The project's (Project Settings > Rendering), or a running override (Set Render
// Style, --render-style)
enum { RENDER_STYLE_STYLIZED = 0, RENDER_STYLE_REALISTIC, RENDER_STYLE_PHOTOREAL, RENDER_STYLE_VIDEO, RENDER_STYLE_COUNT };
const char* render_style_name(int style); // English (TR() it)
void render_set_style_override(int style); // -1: back to the project's
// (B9) the picture faded to a color (the Sequencer's Fade track): amount 0 none .. 1 all of it
void render_set_fade(float amount, const float color[3]);
// The upscaler (upscale.h UPS_*): the project's (Project Settings > Rendering) unless overridden (--upscaler; -1: the
// project's); the one running this frame (a vendor's falls back to TSR when it can't: -1 without TSR)
void render_set_upscaler_override(int kind);
int render_upscaler_active(void);
int render_style(void);                   // the one in use
