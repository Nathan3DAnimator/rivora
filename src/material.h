// Materials (notas-dev/materiais.md): node graphs with Blender's / the Tlender's shader nodes (the same idnames, sockets,
// defaults and enums: a Tlender material opens the same), compiled to a register bytecode and split in two:
//   - the static part (everything that depends only on the UV and constants: textures, noise, color math...) is baked
//     on the GPU into textures at the material's resolution, once per edit;
//   - the dynamic part (what depends on a parameter a Material Instance or a Blueprint changes, or on the view: Fresnel,
//     Layer Weight) runs per pixel in mesh.frag over the baked textures (shaders/mat_vm.glsl).
// A material without parameters costs a pixel 2-3 texture reads, however many nodes it has.
// Material Instances (Unreal's): a material asset with parameter values (.rmi), and per actor a dynamic instance a
// Blueprint changes (Set Scalar / Vector Parameter Value): each is a GpuMaterial slot sharing its parent's textures.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../shaders/tl/kernel_svm_types.h" // the Tlender's node types and enums (NODE_*, MATH_*, BLEND_*...)
#include "ecs.h"
#include "toon.h" // (Q11) the Toon shading model's and the outline's values
#include <vulkan/vulkan.h>

struct VkCtx;

#define MAT_MAX_NODES 256
#define MAT_MAX_LINKS 512
#define MAT_NODE_REROUTE NODE_TYPE_COUNT // the editor's knot (Blender's NodeReroute): compiled through, never itself
#define MAT_NODE_MAX_IN 32
#define MAT_NODE_MAX_OUT 8
#define MAT_NAME 64
#define MAT_PATH 260
#define MAT_MAX_PARAMS 32
#define MAT_EMISSION_INTENSITY "Emission Intensity" // (2026-11-24) every glowing material's parameter (Material.emission_intensity)

typedef enum MatSockType { MSOCK_FLOAT, MSOCK_VECTOR, MSOCK_COLOR, MSOCK_SHADER } MatSockType;
enum {
    MSF_HIDE_VALUE = 1, MSF_FACTOR = 2, MSF_IMPLICIT_NORMAL = 4, MSF_IMPLICIT_UV = 64, MSF_IMPLICIT_GENERATED = 16,
    MSF_ANGLE = 32,
};

typedef struct MatSockDef {
    const char* name;  // Blender's identifier (the file format, the Tlender's)
    const char* label; // Portuguese (the Tlender's label)
    uint8_t type;
    uint16_t flags;
    float def[4], min, max;
} MatSockDef;

typedef struct MatPropDef {
    const char* label; // Portuguese
    const char* const* items; // NULL: a checkbox
    int count, def;
} MatPropDef;

typedef enum MatCategory { MCAT_INPUT, MCAT_SHADER, MCAT_TEXTURE, MCAT_COLOR, MCAT_VECTOR, MCAT_CONVERTER, MCAT_OUTPUT, MCAT_COUNT } MatCategory;

typedef struct MatNodeDef {
    int type;            // NODE_*
    const char* idname;  // Blender's ("ShaderNodeBsdfPrincipled")
    const char* name;    // English (Blender's UI name)
    const char* label;   // Portuguese (the Tlender's)
    int category;
    float width;
    int nin, nout, nprops;
    MatSockDef in[MAT_NODE_MAX_IN];
    MatSockDef out[MAT_NODE_MAX_OUT];
    MatPropDef props[SVM_PROPS];
} MatNodeDef;

const MatNodeDef* mat_node_def(int type); // NULL: not supported here
// (2026-11-23) A texture's map by its name's ending (Substance, Quixel, Poly Haven, Blender's bakes: Chair_BaseColor,
// rock_nor_gl_2k, WoodRoughness, T_Wall_ORM...): MAT_MAP_*; key (optional): the name before the ending, letters and digits
enum { MAT_MAP_NONE = 0, MAT_MAP_BASE, MAT_MAP_NORMAL, MAT_MAP_NORMAL_DX, MAT_MAP_ROUGH, MAT_MAP_GLOSS, MAT_MAP_METAL,
       MAT_MAP_ORM, MAT_MAP_EMIT, MAT_MAP_ALPHA, MAT_MAP_AO, MAT_MAP_HEIGHT, MAT_MAP_COUNT };
int mat_map_slot(const char* file, char* key, size_t cap);
const MatNodeDef* mat_node_def_by_idname(const char* idname);
// An input the renderer reads (false: kept and saved but no visible effect yet: Principled BSDF's Coat, Transmission,
// Subsurface, Anisotropic, Specular Tint, Sheen, Thin Film, IOR... notas-dev/materiais.md "Pinos sem efeito"). The editor
// greys them and warns when one is set; the glTF importer says which extensions came in without effect.
bool mat_input_has_effect(int type, int sock);
// An input whose constant has an effect but a link not yet (the Principled's coat, anisotropy, subsurface, specular, IOR,
// transmission: per material constants, material_gpu.inc principled_ext). Linked, the editor greys it as above
bool mat_input_const_only(int type, int sock);
// The node's inputs without effect whose value differs from the default or that are linked (-1: none); names: their
// names joined by ", " (cap bytes). Returns how many.
struct MatGraph;
int mat_node_unused_inputs(const struct MatGraph* g, int node, char* names, size_t cap);
int mat_pins_selftest(void); // the compiler reads exactly the inputs said to have effect (--mat-pins-test); failures
int mat_node_defs(const MatNodeDef** out, int cap); // the Add menu's order
const char* mat_category_name(int cat);
uint32_t mat_category_color(int cat); // IM_COL32 like Blender's header colors
uint32_t mat_socket_color(int type);

typedef struct MatNode {
    int type;
    float x, y;
    int prop[SVM_PROPS];
    float in[MAT_NODE_MAX_IN][4];
    float value[4];        // RGB / Value nodes
    int ramp_count;        // Color Ramp: (pos, r, g, b, a)
    float ramp[RAMP_MAX_STOPS][5];
    char image[MAT_PATH];  // Image Texture: the file (UTF-8, absolute or under Content)
    char label[MAT_NAME];
    char param[MAT_NAME];  // Value / RGB / Image Texture: a parameter's name ("": a constant), Material Instances set it
    bool selected;
} MatNode;

typedef struct MatLink { int16_t from, from_sock, to, to_sock; } MatLink;

typedef struct MatGraph {
    int node_count, link_count;
    MatNode nodes[MAT_MAX_NODES];
    MatLink links[MAT_MAX_LINKS];
    float view[3]; // the editor's pan and zoom
} MatGraph;

void mat_graph_default(MatGraph* g); // Principled BSDF -> Material Output (Blender's new material)
int mat_graph_add(MatGraph* g, int type, float x, float y);
void mat_graph_remove(MatGraph* g, int node);
bool mat_graph_link(MatGraph* g, int from, int from_sock, int to, int to_sock); // replaces the input's link
int mat_graph_input_link(const MatGraph* g, int to, int to_sock);
void mat_graph_unlink_input(MatGraph* g, int to, int to_sock);
void mat_node_init(MatNode* n, int type);
void mat_ramp_eval(const MatNode* n, float f, float rgba[4]);

// --- Assets: a material (.rmat) or a Material Instance (.rmi: its parent and parameter values) ---

typedef struct MatParam {
    char name[MAT_NAME];
    uint8_t kind;   // 0 scalar, 1 vector (color)
    float value[4];
} MatParam;

// The Real Engine's own node (not the Tlender's / Blender's): Time, for animated materials (a free id of the range)
#define NODE_RE_TIME 259
// (2026-11-16) the Real Engine's Wet Surface: a shader in, the same shader wet out (darker where porous, a water film over
// it, drops that stay and run, sweat); its values as the material's constants, or Value parameters a Blueprint changes at
// run time without a bake (mesh.frag reads them). Inputs: Shader, Wetness, Porosity, Water Film, Film Roughness, Droplets,
// Droplet Size (mm), Running, Sweat; property: the Mask
#define NODE_RE_WET 258
// (2026-11-17, Q4) the Real Engine's Interior Mapping: a shader in (the facade, its window frames), rooms seen through the
// windows out: each room a box behind the surface (no geometry), with depth, a layer of furniture, curtains, and at night
// a light on in a random share of them. Its image: an atlas of rooms (Columns x Rows; none: procedural rooms). Inputs:
// Shader, Rooms U, Rooms V, Room Depth, Frame, Furniture, Curtains, Night, Lit Rooms, Light Color, Light Strength, Seed
// (the Night: a Value with a Parameter Name changes at run time). mesh.frag interior(), material_gpu.inc interior_ext
#define NODE_RE_INTERIOR 257
enum { MAT_WET_MASK_ALL = 0, MAT_WET_MASK_VCOL, MAT_WET_MASK_UP, MAT_WET_MASK_PATCHY, MAT_WET_MASK_COUNT };
// Blender's Color Attribute (ShaderNodeVertexColor): the mesh's vertex colors (RMESH_COLORS; M5). Baked as white; the
// renderer multiplies the Base Color by them (mat_vcol_usage: the uses that come out exact). A free id of the range
#define NODE_VERTEX_COLOR 133
// How a graph uses its Color Attribute: none, only multiplying the Base Color (exact at run time), or another way (baked
// as white: the node gets the orange "!")
enum { MAT_VCOL_NONE = 0, MAT_VCOL_TINT, MAT_VCOL_OTHER };
int mat_vcol_usage(const MatGraph* g);
#define MAT_ANIM_MAX 64 // an animated material's baked frames (its flipbook)
enum { MAT_BLEND_OPAQUE = 0, MAT_BLEND_MASKED, MAT_BLEND_TRANSLUCENT, MAT_BLEND_COUNT }; // Material.blend_mode (Unreal's names)
enum { MAT_TRANS_REFRACTIVE = 0, MAT_TRANS_THIN, MAT_TRANS_COUNT }; // (Q4) Material.transmission_mode
// (Q4) Unreal's Material Domain: a surface, or a Light Function (its Emission, else its Base Color, multiplies a light)
enum { MAT_DOMAIN_SURFACE = 0, MAT_DOMAIN_LIGHT_FUNCTION, MAT_DOMAIN_COUNT };
enum { MAT_SHADING_DEFAULT = 0, MAT_SHADING_EYE, MAT_SHADING_TOON, MAT_SHADING_COUNT }; // Material.shading_model (Unreal's Shading Model; Toon: Q11)
#define MAT_OPACITY_CLIP_DEFAULT 0.3333f // Unreal's default Opacity Mask Clip Value

typedef struct Material {
    char path[MAT_PATH];  // the asset's file
    char name[MAT_NAME];
    uint32_t hash;        // of its virtual path ("/Content/..."): what a Static Mesh stores
    bool instance;        // a Material Instance (.rmi)
    char parent_path[MAT_PATH];
    struct Material* parent;
    MatGraph* graph;      // a material's (NULL for an instance)
    int resolution;       // the bake's texture side (256 .. 4096)
    bool world_grid;      // Unreal's grid material: world-aligned grid lines over the base color (mesh.frag)
    uint8_t blend_mode;   // Unreal's Blend Mode: Opaque, Masked (the baked alpha cuts it), Translucent (the alpha is the opacity)
    bool two_sided;       // Unreal's Two Sided: the back faces drawn too, lit from their side (hair cards, leaves)
    // Unreal's Shading Model: Default Lit, or Eye (A9.3c: a clear cornea over the iris, the iris refracted under it and
    // lit as a dish; the iris centered at uv 0.5, 0.5, eye_iris_radius its radius in UV, eye_iris_depth the cornea's
    // height over it in cm; the eye looks along its actor's forward, +X)
    uint8_t shading_model;
    uint8_t domain;       // (Q4) MAT_DOMAIN_: a Light Function's lights read its baked textures (mesh.frag light_fn)
    float eye_iris_radius, eye_iris_depth;
    // (Q13) Eye Glow: the iris sends a light back toward where it came from (a flashlight beside the camera: the eyes
    // shine in the dark, an animal's, an animatronic's); its strength (0 none .. 16) and color
    float eye_glow, eye_glow_color[3];
    // (Q27.8, the city at night) a retroreflector (road signs, cat's eyes, a hi-vis vest's stripes, a bike's reflector):
    // the light sent back toward where it came from, any shading model but the Eye (its own Eye Glow); its strength
    // (0 none .. 16) and color. Metallic flakes under the coat (car paint): how much they glint (0 none .. 4), their size
    // (cm) and how far they tilt from the surface (0..1)
    float retro, retro_color[3];
    float flakes, flake_size, flake_spread;
    bool world_aligned;   // Unreal's WorldAlignedTexture: the UV from the world position (mesh.frag), world_tile cm a tile
    float world_tile;
    float opacity_clip;   // Masked: alpha below it is cut (Unreal's Opacity Mask Clip Value)
    // (2026-11-16) refraction: a Translucent or transmissive glass bends what is seen through it (shadow_split.comp)
    float refraction;     // its strength (0 off, 1 as its IOR says)
    float refr_thickness; // cm the view travels through it (a pane ~1, a bottle ~5, a lens or a block more)
    float dispersion;     // 0..1: the colors split at its edges (a prism, cut crystal)
    // (2026-11-17, Q4) Transmission (the Principled BSDF's Transmission Weight, or a parameter on its pin): Refractive (glass,
    // water: the OIT pass, bent, rough, absorbing) or Thin (a leaf, a curtain, paper, a lampshade, an ear: opaque, lit
    // from behind through it). Absorption: the color the light keeps after absorption_dist cm inside (Refractive);
    // colored_shadow: the sun's light through it tinted on what is behind (ray traced shadows)
    uint8_t transmission_mode;
    float absorption[3], absorption_dist;
    bool colored_shadow;
    int anim_frames;      // a graph with a Time node: its loop baked in so many frames (blended while it plays)
    int anim_res;         // ... each this size (the still material keeps resolution)
    MatParam params[MAT_MAX_PARAMS]; // a material: its parameters and defaults; an instance: its overrides
    int nparams;
    // compiled / baked (material.c)
    uint32_t slot;        // its GpuMaterial (0: none yet)
    bool dirty;           // edited: compile and bake before the next frame
    // (2026-11-24, report #180) being edited (the Material Editor's values dragged): until live_until its bakes keep
    // RGBA8 (the same textures reused each edit, no BC7 swap); bc7_pending: compressed once the editing stops
    double live_until;
    bool bc7_pending;
    char error[160];
    float avg_base[3];    // the baked base color's mean (the GI sees the mesh with it)
    // (2026-11-23) the baked emission's mean (x100: nits): the GI's rays see the mesh glow with it, and an actor with
    // enough of it lights the scene as a point light (lights.c); Emissive Light scales that light (0: its glow lights nothing)
    float avg_emis[3];
    float emissive_light;
    bool emissive_shadows; // (its light's shadows by ray: off by default, the many lights' sampling costs ~1 ms)
    // (2026-11-24) Emission Intensity: the emission (the Principled's, an Emission node's) times it, at run time (not baked):
    // the parameter MAT_EMISSION_INTENSITY of every glowing material, a Material Instance's and a Blueprint's Set Scalar
    // Parameter Value change it without a bake (1: as baked; Unreal's emissive multiplier)
    float emission_intensity;
    double bake_ms, compile_ms;
    uint32_t ninstr, nstatic, ndynamic, nslots;
    bool animated;        // compiled with a Time node: a flipbook
    bool subsurface;      // its Principled BSDF's Subsurface Weight and Scale above 0 (mat_sss)
    bool transmissive;    // its Principled BSDF's Transmission Weight above 0: drawn translucent (mat_translucent)
    bool ext_lobes;       // coat, anisotropy, subsurface, specular tint / IOR or transmission in use (MATF_EXT)
    float anim_period;    // its loop (s)
    // M5: a detail normal (another material's baked normal, tiled finer over this one's: Unreal's detail texturing);
    // the Color Attribute's use (MAT_VCOL_)
    char detail_path[MAT_PATH]; // its virtual path ("/Content/..."; "": none)
    // (B11) a landscape's painted layers (Unreal's Landscape Layer Blend): three other materials blended over this one by
    // the mesh's vertex colors (r, g, b), each tiled every layer_tile cm over the world's xy ("": none)
    char layer_path[3][MAT_PATH];
    float layer_tile[3];
    bool interior;        // (2026-11-17, Q4) an Interior Mapping node on its Surface (MATX_INTERIOR: the RE_EXT pipelines)
    bool wet;             // (2026-11-16) a Wet Surface node on its Surface (MATX_WET: the RE_EXT pipelines)
    bool parallax;        // M7: a Displacement into its Material Output (opaque): parallax occlusion (MATF_POM)
    // (Q17) Nanite Tessellation (Unreal's Enable Tessellation and Displacement Scaling): its Displacement moves the
    // surface for real, its DAG clusters diced on the GPU by their size on screen (tess.c). Magnitude: cm (of the mesh,
    // the actor's scale too) the height 0..1 spans; Center: the height that stays in place (Blender's Midlevel)
    bool tessellation;
    float disp_magnitude, disp_center;
    bool tess_active;     // compiled: tessellation on with a Displacement (opaque, not animated): MATF_TESS
    float detail_tiling, detail_strength;
    int vcol;
    // (Q11) the Toon shading model's values and the outline's (toon.h): a material's; an instance's own when toon_own
    // (else its parent's)
    MatToon toon;
    bool toon_own;
} Material;

bool mat_init(struct VkCtx* vk, const char* content_root);
void mat_set_root(const char* content_root); // the project's Content folder (the editor, the game)
const char* mat_root(void); // (that folder: the runtime's modules read their files under it, splat.c)
// The engine's grid material and the node examples under Content/Materials (made when missing: mat_examples.c)
void mat_make_engine_materials(const char* content_root);
void mat_shutdown(struct VkCtx* vk);
Material* mat_load(const char* path);           // cached; NULL: unreadable
// Masked or Two Sided (an instance: its parent's): the renderer draws its meshes in the masked runs
bool mat_cuts(const Material* m);
// Translucent (its own Blend Mode, or its parent's): drawn in the OIT pass, casts no shadow, out of the ray tracing
bool mat_translucent(const Material* m);
bool mat_refract(const Material* m); // (2026-11-16) translucent with Refraction above 0 (its parent's for an instance)
bool mat_glass_shadow(const Material* m); // (Q4) refractive transmission with Colored Shadow (its parent's for an instance)
// Coat, anisotropy, subsurface, specular tint / IOR or transmission in its Principled BSDF (its parent's for an instance):
// the renderer's RE_EXT pipelines (MATF_EXT)
bool mat_ext(const Material* m);
// (Q11) the Toon shading model (its parent's for an instance): the RE_EXT pipelines (MATF_TOON); its values: the instance's
// own when it overrides them, else its parent's
bool mat_toon(const Material* m);
const MatToon* mat_toon_values(const Material* m);
// (Q11) a GpuMaterial slot's toon and outline values (a material's, an instance's, an actor's dynamic instance's); NULL:
// none. mat_toon_slot_set writes them (a dynamic instance's: a Blueprint's Set Toon Parameter) without a bake
const MatToon* mat_toon_slot(uint32_t slot);
bool mat_toon_slot_set(uint32_t slot, const MatToon* t);
uint32_t mat_outline_generation(void); // bumped when a slot's outline turns on or off (the renderer finds its actors again)
bool mat_sss(const Material* m); // a subsurface material (A9.3a): the skin's screen space scattering (render.c GI_SSS)
bool mat_tess(const Material* m); // (Q17) Nanite Tessellation active (its parent's for an instance): MATF_TESS
Material* mat_find_hash(uint32_t hash);         // a loaded one, or the Content folder scanned for it
uint32_t mat_base_texture(uint32_t hash);       // its base color texture in the bindless set (the particles' sprites); 0: none
uint32_t mat_count(void);
Material* mat_at(uint32_t i);
bool mat_vpath(const Material* m, char* out, size_t cap); // "/Content/..." of its file
bool mat_save(Material* m);
Material* mat_create(const char* path, bool instance, const char* parent_path); // a new asset written to path
uint32_t mat_vpath_hash(const char* vpath); // a material's id from its "/Content/..." path (before it is loaded)
bool mat_reload_file(const char* path);     // written by someone else (a background import): read again if loaded
void mat_mark_dirty(Material* m);               // the graph or a parameter changed
bool mat_live_selftest(void); // (2026-11-24, #180) --mat-live-test: a 4096 material edited 60 times reuses its textures
bool mat_param_selftest(void); // (2026-11-24) --mat-param-test: parameters set without a bake, the fold against a full bake
const char* mat_default_path(void);             // the basic textured material (the engine's grid material)

// The materials' GPU work before a frame (compiles and bakes the dirty ones; the renderer's command buffer)
// True when slots changed (the scene's instances are sent again).
bool mat_update(struct VkCtx* vk, VkCommandBuffer cmd);
bool mat_compiling(void); // (Q22) materials still being compiled / baked in the background (the measures wait)
int mat_compiling_count(void); // (Q26) how many (the status bar's Shader Compile)
// The dirty materials baked now (the project's loading screen: the editor opened frozen ~0.5 s per big material);
// progress: before each one
void mat_bake_pending(void (*progress)(int done, int total, const char* name));
// The slot an actor's mesh draws with: its dynamic instance of m if it has one, else m's
uint32_t mat_slot_of(Entity e, Material* m);
// The descriptor set of the textures (set 1) and the GpuMaterial buffer's address
VkDescriptorSetLayout mat_set_layout(void);
VkDescriptorSet mat_set(void);
// (Q21) another module's RGBA8 texture in the bindless set (farview.c's impostor atlases): made, its image (mip 0 written by
// copies, GENERAL layout), its mips by halving blits in cmd, released (the GPU done with it)
uint32_t mat_tex_new(uint32_t w, uint32_t h);
VkImage mat_tex_image(uint32_t idx);
void mat_tex_mips_in(VkCommandBuffer cmd, uint32_t idx);
void mat_tex_release(uint32_t idx);
uint64_t mat_buffer_address(void);
uint64_t mat_programs_address(void);
uint64_t mat_params_address(void);
// A dynamic instance for an actor (a Blueprint's Create Dynamic Material Instance / Set ... Parameter Value): its slot
uint32_t mat_dynamic_for(Entity e, Material* m);
bool mat_set_param(uint32_t slot, const char* name, const float v[4]);
// (2026-11-24) a slot's emission mean as it draws (x100: nits; its Emission Intensity and folded parameters applied): false
// for no slot
bool mat_slot_emission(uint32_t slot, float out[3]);
// (2026-11-24) the size of the base texture a material draws with now (0: its placeholder, nothing baked yet: the tests)
uint32_t mat_baked_size(const Material* m);
// (2026-11-24) no bake for the next frames (0: bakes again): the mat_first_frame test's first frame before any bake
void mat_hold_bakes(uint32_t frames);
void mat_end_play(void); // the dynamic instances dropped (PIE ends)

// The baked textures of a material or instance (0 base color, 1 normal / roughness / metallic, 2 emission) for
// previews; VK_NULL_HANDLE: none yet
VkImageView mat_preview_view(const Material* m, int which);
// (2026-11-25) How the base color thumbnail shows (the affine fold's roots in the texture, its slot's coefficients make the
// color): MAT_PREVIEW_IMAGE the image times tint, MAT_PREVIEW_SOLID the color solid (linear)
enum { MAT_PREVIEW_IMAGE = 0, MAT_PREVIEW_SOLID = 1 };
int mat_preview_base(const Material* m, float tint[3], float solid[3]);
// An animated material's frame's base color (the editor's preview plays them; NULL: still or not baked)
VkImageView mat_preview_frame_view(const Material* m, int which, uint32_t frame); // which: 0 base, 2 emission
// Every compile's statistics are in the Material (ninstr, bake_ms...). Stats for the Debug Viewer
// The textures' memory budget (0: the GPU's own share); a phone sets its own (mobile.c), the low-memory warnings lower it
void material_set_texture_budget(uint64_t bytes);
// The texture streaming (material_stream.inc): the textures' bytes against the budget, the variants streamed, the
// transitions done (up: baked bigger in tiles; down: copied from a smaller mip) and the CPU / GPU work of the last frame
typedef struct MatStats {
    uint32_t materials, slots, textures;
    double bake_ms_total;
    uint64_t tex_bytes, budget;
    uint32_t streamed, ups, downs, pending;
    bool streaming;
    double stream_cpu_ms, stream_cpu_max; // (max: the worst frame since the start)
    double stream_gpu_ms, stream_gpu_max; // the last bake tile's GPU time (timestamps; 0: none that frame), the worst
    uint32_t stream_tile;   // texels a frame the tiles are sized to (ST_TILE_MS of GPU)
    uint32_t stream_texels; // baked this frame
    uint32_t climb_hq, climb_blit; // (2026-11-25) the forced climbs' ends (a first bake, a live edit): with the Kaiser mips, with blits
    // (Q18) the UDIM tiles (material_udim.inc): how many, how many above the least (seen), their bytes, the halvings
    // so they fit, the climbs and descents done, the sources' tiles uploaded
    uint32_t udim_tiles, udim_resident, udim_bias, udim_ups, udim_downs, udim_src_uploads;
    uint64_t udim_bytes;
    // a one-tile .rudim's last bake: the size baked and the least size its sources' tile had then (2026-11-23: was 64)
    uint32_t udim_lone_res, udim_lone_src;
    // (2026-11-24) the affine fold and the bakes: compiles that only wrote the slots (no bake), parameters set without a bake,
    // bakes recorded into a frame (no wait), the live 256 bakes, the parameters' and programs' room left (vec4, words)
    uint32_t live_writes, live_params, frame_bakes, live_bakes;
    uint32_t params_free, prog_free;
} MatStats;
const MatStats* mat_stats(void);

// Texture streaming (material_stream.inc, notas-dev/materiais.md): the renderer reports each material's size on screen
// (px: the mesh's diameter in pixels times its tiling) a slice of the world a frame, and the end of each pass
bool mat_stream_on(void);
bool mat_stream_note(uint32_t hash, float px); // true: a UDIM material (the caller notes its tiles: mat_stream_note_tile)
// (Q18) one tile of a mesh's UDIM material seen at px (its triangles in that tile: their bounds' size on screen)
void mat_stream_note_tile(uint32_t hash, int udim, float px);
// (Q18) the Debug Viewer's list of UDIM tiles: i-th tile's material, number, size now, wanted, full, need (false: none)
bool mat_udim_info(uint32_t i, const char** name, int* udim, uint32_t* cur, uint32_t* want, uint32_t* full, float* need_px);
void mat_stream_pass_end(void);
void mat_stream_pin(uint32_t hash); // always at full size (what the sweep doesn't see: a crowd's parts)
// An asset's streamed sizes (the Debug Viewer; false: not streamed)
bool mat_stream_info(uint32_t i, const char** name, uint32_t* cur, uint32_t* want, uint32_t* full, float* need_px, uint64_t* bytes);
