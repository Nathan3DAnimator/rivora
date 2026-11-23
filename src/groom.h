// Hair in strands (A9.3b; notas-dev/render.md "Cabelo em fios"; shaders/hair.glsl, hair.vert, hair.frag): every Groom
// (CGroom) grown on the CPU once (its strands in its actor's space, its density grid for the deep shadow), copied into
// one device buffer; each frame its transform, its look and its LOD (how many strands, how wide) into a small table;
// drawn in the main pass as ribbons facing the camera, lit as fibers.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;
struct CGroom;

bool groom_init(struct VkCtx* vk, VkPipelineLayout layout); // (the layout: the simulation's compute pipeline)
// (2026-11-15) a groom moved since last frame (its actor or the bone it follows): the velocity pass draws the hair's motion
bool groom_moving(void);
uint32_t groom_draw_velocity(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe);
void groom_shutdown(struct VkCtx* vk);
// The frame: the grooms of the world (a new one or one whose numbers changed grown again), their table into this
// frame's buffer; the camera for the LOD (cm, the horizontal field of view in degrees, the view's width in pixels)
void groom_fill(World* w, const float cam[3], float fov_deg, uint32_t width_px, uint32_t slot);
// Before the main pass: what was grown this frame into the device buffer
// ... and the dynamic grooms simulated (hair_sim.comp, 2026-11-16)
void groom_record(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout);
// Its depth first (the coverage's cut), then shaded where it stayed (depth EQUAL: each pixel once)
uint32_t groom_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline depth, VkPipeline pipe);
// A preset's numbers into a groom (length, curl, gravity, clumps)
void groom_apply_preset(struct CGroom* g, int preset);
extern const char* const g_groom_preset_names[];

// (2026-11-16) Guides (.rgroom): hair curves made elsewhere (the Tlender's hair, OBJ polylines), in the actor's space (cm);
// a Groom whose Guides names one grows its strands from them (each strand interpolated from its 3 nearest guides, as
// Blender's Interpolate Curves) instead of on the head's ellipsoid. Written by the importers.
typedef struct GroomGuidesInfo { uint32_t strands, segments; float root_width, tip_width, color[3]; } GroomGuidesInfo; // (suggested)
bool groom_guides_write(const char* disk_path, const float* pts, const uint32_t* counts, uint32_t nguides, const GroomGuidesInfo* info);
uint32_t groom_guides_hash(const char* vpath); // what CGroom.guides holds
// the project's .rgroom files (the Details' Guides list; a walk of the Content folder: only while the list is open)
typedef struct GroomGuidesEntry { char name[96], vpath[300]; } GroomGuidesEntry;
uint32_t groom_guides_list(GroomGuidesEntry* out, uint32_t max);

typedef struct GroomStats {
    uint32_t grooms, strands, drawn, points;
    double grow_ms;
    uint32_t segments, simulated; // (Q27.3) the strand segments drawn this frame (under the budget), the grooms simulated
    float budget_k;               // (Q27.3) the factor the wishes were scaled by (1: under the budget)
} GroomStats;
const GroomStats* groom_stats(void);
