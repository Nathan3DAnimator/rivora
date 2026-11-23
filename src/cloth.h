// Cloth (notas-dev/tecido.md; shaders/cloth.glsl, cloth_sim.comp, cloth.vert/.frag): every Cloth actor (CCloth) simulated
// together on the GPU by Vertex Block Descent (Chen, Macklin, Kim, Yuksel 2024: a Newton step per vertex block, the
// vertices of a color in parallel, stable at any stiffness), its contacts bounded per vertex as Offset Geometric Contact
// does (Chen et al. 2025: a vertex never moves past a share of its distance to what it touches: no tunneling, no
// clipping, its thickness kept). What it touches: the Skeletal Meshes' bones (capsules, the Physics Asset's radii),
// the characters' capsules, the physics bodies and the level's meshes (boxes), a few dozen around each cloth. It tears
// where a thread stretches past its Tear Strain, or where a Blueprint cuts it (Tear Cloth). Nothing is read back.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

bool cloth_init(struct VkCtx* vk, VkPipelineLayout layout);
void cloth_shutdown(struct VkCtx* vk);
// The frame: the cloths of the world into the GPU's blocks (a new one or a changed size starts from its rest pose), their
// colliders; dt in seconds (0: paused, the editor not simulating)
void cloth_fill(World* w, float dt, uint32_t slot); // slot: the frame in flight (its own set of the CPU-written buffers)
void cloth_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push);
uint32_t cloth_count(void); // (2026-11-15) the cloths this frame (the velocity pass draws their motion)
uint32_t cloth_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe);
// A preset's numbers into a cloth (its density, springs, damping, look)
void cloth_apply_preset(struct CCloth* c, int preset);
extern const char* const g_cloth_preset_names[];
extern const char* const g_cloth_pin_names[];
// Blueprint Tear Cloth: every thread within radius (cm) of a point cut (the next frame)
void cloth_tear(const float center[3], float radius);

// Its shadow (render.c): the BLAS of each cloth that casts one, built from its vertices (before the dynamic TLAS), its
// instances in the dynamic TLAS (shadow rays only: a cloth has no material a ray could shade), its depth into a virtual
// shadow map level (VSM_PASS of the dynamic casters), its bounds (the dynamic set's box)
void cloth_blas_record(VkCommandBuffer cmd);
uint32_t cloth_tlas_instances(VkAccelerationStructureInstanceKHR* out, uint32_t max);
bool cloth_bounds(float mn[3], float mx[3]);
uint32_t cloth_draw_shadow(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe, uint32_t level);

typedef struct ClothStats { uint32_t cloths, vertices, colliders, dispatches; double cpu_ms; } ClothStats;
// (Q10) the tests: a cloth's vertices as the GPU left them (xyz in meters, w; mapped memory only with
// REALENGINE_CLOTH_READBACK=1 or _DEBUG at the start, read after the frames
// finished) and how many; 0: none (not a cloth this frame)
uint32_t cloth_debug_vertices(Entity cloth, const float** pos);
const ClothStats* cloth_stats(void);

// --- Soft bodies (Q9, softbody.h, notas-dev/soft-body.md): Cloths of kind CLOTH_KIND_SOFT ---
// The camera and whether the editor is not playing (render.c, before cloth_fill): the soft bodies simulate only in play
// (their rest pose in the editor, as the physics bodies), sleep far from the camera
void cloth_set_view(const float cam[3], bool editor);
// What the renderer's skin reads to bend a soft body's mesh (render.c skin_bind): from the last fill, so it matches the
// positions the GPU holds when this frame's skins run (before this frame's simulation)
typedef struct ClothSoftView {
    Entity e;
    VkDeviceAddress pos, embed;   // the lattice's positions (vec4, m, world), the mesh vertices' places in it (vec4)
    uint32_t base, nx, ny, nz;    // its first vertex, its counts
    uint32_t nrender;             // the mesh's LOD 0 vertices
    float scale_over_cell[3];     // the actor's scale / the cell (cm): a mesh cm in lattice units
    uint32_t version;             // changes whenever its positions did (a frame simulated, a reset)
} ClothSoftView;
bool cloth_soft_view(Entity e, ClothSoftView* out);
// Blueprint Add Soft Body Impulse: a velocity change (cm/s) to its vertices within radius (cm) of center, fading out;
// e: that soft body (ENTITY_NONE: every one)
void cloth_soft_impulse(Entity e, const float center[3], float radius, const float vel[3]);
// Blueprint Set Soft Body Property / Stiffness / Pinned: a soft body's field by its name in the Details (spaces and case
// ignored; "Held" 0 lets it go); held in its range; false: not a soft body or no such field
bool cloth_soft_set_prop(World* w, Entity e, const char* name, float value);
// The tests' look (the GPU idle, a copy back): a soft body's lattice positions (4 floats a vertex, m) into out (cap
// vertices), its lattice; the count (0: none)
struct SoftTopo;
uint32_t cloth_soft_readback(Entity e, float* out, uint32_t cap, const struct SoftTopo** topo);
typedef struct SoftStats {
    uint32_t bodies, awake, vertices, tets, rigid_pushes; // rigid_pushes: the physics bodies pushed last frame
    float rigid_force;                                    // N: the largest push a body took
    double build_ms;                                      // the last lattice built (CPU)
} SoftStats;
const SoftStats* soft_stats(void);
