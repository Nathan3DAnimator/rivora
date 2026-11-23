// The sun's virtual shadow maps (notas-dev/luz.md, "Sombras virtuais"): Unreal's idea (a huge shadow map rendered only
// where it changed, kept between frames) adapted to what is cheap here: a clipmap of concentric square levels around the
// camera, each a layer of one depth array (level 0: 16 m, each next twice as wide; 2048 texels a side: 0.8 cm a texel
// at the feet, the last level 2 km), cached. A level is drawn again whole only when the camera walked an eighth of it
// away, the sun turned or the scene's depth range grew (a budget of levels a frame, the finest first); a caster that
// moved redraws only its rectangle in each level (cleared and drawn again: the vertex shader drops every instance
// outside it). A still scene costs nothing; a walking character, its rectangles. The main pass filters it with PCSS
// (blocker search, penumbra from the sun's angle: contact-hardening, sharp at the feet, soft far from the caster) on
// fixed Vogel disks (no noise), blending into the next level at each level's edge.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "render_gpu.h"

struct VkCtx;

// (VSM_MAX_LEVELS: render_gpu.h, the frame holds one matrix per level)
#define VSM_LEVEL0_CM 1600.0f // level 0's side
#define VSM_FORMAT VK_FORMAT_D32_SFLOAT

typedef struct VsmSettings {
    int levels;       // 0: off (the sun unshadowed by the map); else 1..VSM_MAX_LEVELS
    int res;          // texels a side (1024 or 2048)
    int taps;         // PCSS filter taps (4: plain 3x3 PCF, no blocker search)
    int full_budget;  // levels drawn whole a frame (after the camera moved / the sun turned); new ones always
    float sun_angle;  // the sun's angular diameter (degrees): the penumbra (Unreal's Source Angle, 0.5357)
} VsmSettings;

// One level's work this frame (vsm_plan): drawn into layer `level`
typedef struct VsmJob {
    int level;
    bool full;                 // cleared and drawn whole; else the rectangles cleared and drawn again
    uint32_t x, y, w, h;       // the texel rectangle drawn (viewport and scissor)
    uint32_t nclear;           // (partial) the rectangles cleared inside it
    uint32_t clear[4][4];      // x, y, w, h
} VsmJob;

typedef struct VsmStats {
    int levels, res;
    uint32_t jobs, full, partial; // this frame
    double texels;                // texels redrawn this frame (millions)
    uint64_t full_total, partial_total, frames;
} VsmStats;

bool vsm_init(struct VkCtx* vk);
void vsm_shutdown(struct VkCtx* vk);
// (Re)creates the array for these settings when they changed (waits for the GPU then); the views change: rebind
bool vsm_configure(struct VkCtx* vk, const VsmSettings* s, bool* views_changed);
VkImageView vsm_view(void);           // every level (sampler2DArray / sampler2DArrayShadow)
VkImageView vsm_layer_view(int level); // one level (the depth attachment)
VkImage vsm_image(void);
bool vsm_enabled(void);

// A shadow caster changed inside this world box (before and after a move: both boxes); everything (a level opened)
void vsm_dirty_box(const float mn[3], const float mx[3]);
void vsm_invalidate(void);

// This frame's work: the levels around the camera for the light traveling along `light_dir` (unit, from the sun down),
// the scene's casters' bounds; fills fr's vsm_* fields and returns the jobs (at most VSM_MAX_LEVELS) and the cull's
// shadow box (planes, inside: dot >= 0; none when no job)
int vsm_plan(GpuFrame* fr, const float camera[3], const float light_dir[3], const float casters_mn[3],
             const float casters_mx[3], VsmJob* jobs, float planes[6][4]);
const VsmStats* vsm_stats(void);
void vsm_stats_reset(void); // (the measure starts: the totals from zero)
// The static casters' own layers (Unreal's static / dynamic split): a level drawn whole draws them there first; a dirty
// rectangle copies them back from there and draws only the dynamic casters over them
VkImageView vsm_static_view(int level);
int vsm_levels(void);
// A static caster appeared, went or moved inside this box (its old and new boxes): the levels over it drawn whole
void vsm_dirty_static(const float mn[3], const float mx[3]);
