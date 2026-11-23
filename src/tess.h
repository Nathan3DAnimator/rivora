// (Q17) Nanite Tessellation (notas-dev/tessellation.md): a material's Displacement (Enable Tessellation) moves the
// surface of its cluster LOD DAGs for real, each base triangle diced on the GPU by its size on screen into a pattern of
// N x N micro triangles (shaders/tess.comp, tess.glsl, tess.vert), with no crack between neighbors, under a
// budget of micro triangles a frame. The renderer (render.c) calls these around its DAG passes.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "render_gpu.h"

struct VkCtx;
struct GiSettings;

// R.tess's layout (shaders/tess.glsl: the same numbers)
#define TESS_MAX 32u            // the most micro edges along a base edge: patterns 1..TESS_MAX
#define TESS_MAX_TRIS 262144u   // base triangles diced a frame (more: their instances drawn by pass 6, displaced only; Q22: was 524288)
#define TESS_BATCH(n) ((128u / ((n) * (n))) > 1u ? (128u / ((n) * (n))) : 1u) // base triangles an instance of pattern n draws
#define TESS_REC_PAD 8192u      // the records' room past TESS_MAX_TRIS: each pattern's list padded to its batch (2 sets)
#define TESS_CMDS_AT 320u
#define TESS_OWNER_AT 2048u
#define TESS_IDX_AT (TESS_OWNER_AT + TESS_MAX_TRIS * 4u)
#define TESS_TMP_AT (TESS_IDX_AT + TESS_MAX_TRIS * 12u)
#define TESS_REC_AT (TESS_TMP_AT + TESS_MAX_TRIS * 8u)
#define TESS_CRN_AT (TESS_REC_AT + (TESS_MAX_TRIS + TESS_REC_PAD) * 16u) // (Q22) per triangle its corners: 128 B (tess.vert)
#define TESS_BYTES (TESS_CRN_AT + TESS_MAX_TRIS * 128u)
#define TESS_ON 1u
#define TESS_SUBDIV 2u
#define TESS_SEAMS 4u

typedef struct TessStats {
    uint64_t micro;         // micro triangles drawn (the last frame read back)
    uint32_t base;          // base triangles diced
    uint32_t fallback;      // DAG instances that didn't fit (drawn by pass 6, displaced only)
    float scale;            // the budget's scale on the px per micro edge (1: none)
    float px, max_factor;   // what the frame asked
    double gpu_ms;          // the dicing's compute (the first phase's set; timestamps 30-31)
    uint32_t seam_xor[2], seam_edges, seam_frames; // the seam check (TESS_SEAMS)
    bool on, subdiv;
} TessStats;

bool tess_init(struct VkCtx* vk, VkPipelineLayout layout); // the pipeline and the patterns (small)
void tess_shutdown(struct VkCtx* vk);
// this frame's displacement (any: an instance with a displaced material; the settings) and dicing (can_dice: the DAG
// path drawn): makes the big buffer on the first need
bool tess_prepare(struct VkCtx* vk, const struct GiSettings* gi, bool can_dice, bool any);
void tess_fill_frame(GpuFrame* fr);
// the header zeroed (in the cull's transfer block: the caller's barrier after it)
void tess_reset(VkCommandBuffer cmd);
// one set's dicing (0: the DAGs' first phase, 1: their late one), after cluster_record of that phase; sets: R.set and the
// materials' (bound for compute here)
void tess_record(VkCommandBuffer cmd, VkPipelineLayout layout, const VkDescriptorSet sets[2], Push push, uint32_t set);
// one set's micro triangles drawn with pipeline p (a tess twin: tess.vert; push.pass 9 already pushed); index: the meshes' index buffer, bound
// back after. Returns the draws issued
uint32_t tess_draws(VkCommandBuffer cmd, VkPipeline p, uint32_t set, VkBuffer index);
// the header copied for the CPU (after the frame's draws), read back when that frame's slot comes around
void tess_readback(VkCommandBuffer cmd, uint32_t slot, bool on);
void tess_collect(uint32_t slot, double gpu_ms);
const TessStats* tess_stats(void);
bool tess_active(void); // (this frame dices)
void tess_set_seam_check(bool on); // (the tess_seams scene)

// (Q22) The tess twins: each opaque mesh pipeline's copy with tess.vert (render.c pipeline() makes it while a displaced
// material was sent, its destroy takes it). Thread safe (the ray traced sets are made on threads)
bool tess_twin_put(VkPipeline base, VkPipeline twin); // (no room: false back, the caller destroys twin)
VkPipeline tess_twin(VkPipeline base);                // (VK_NULL_HANDLE: none)
VkPipeline tess_twin_take(VkPipeline base);           // (removed from the table: the caller destroys it)

// A mesh's canonical table (words for R.morphs: its vertex count, the UV's units per cm as float bits, then per vertex
// the first vertex at its position, TESS_DUP when it has copies); NULL: no memory. *nwords: its length
uint32_t* tess_canon_table(const GpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni, uint32_t* nwords);
