// (Q27.12) The GI's fallback for Low, Android and Quest (notas-dev/gi.md "GI de fallback"): where nothing else lights the
// diffuse (no lightmap, no probes, no surface cache: the GPUs without rays), the sky light is darkened by an AO volume
// (128 x 128 x 32 m around the camera, 2 m cells: the meshes' boxes as occupancy, each cell's sky seen along 5 rays),
// and the ground sends the sun back up (its bounce: an albedo of 0.2 under the sun's light on it, as GTA 6's patent's
// terrain bounce map); the local lights bounce through a finer volume (GIFL_*). Built on a thread when the camera moves
// 16 m (4 m for the fine one), every 2 s or when a light changes; no GPU pass, a few reads per pixel.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

struct VkCtx;

#define GIFB_NX 64
#define GIFB_NY 64
#define GIFB_NZ 16
#define GIFB_CELL 200.0f
// (2026-11-25) the local lights' bounce (a closed room lit by a lamp was black on Low: the sky and the sun were the only
// sources): a finer volume, 32 x 32 x 8 m of 50 cm cells around the camera, after the AO's bytes in the same buffer. The
// meshes' boxes stop faces between cells (a wall a plane of them); a cell beside a stopped face gets the light that face
// reflects (the lamps' direct light at it, shadowed through the stopped faces), and the light spreads through the open
// faces (each cell the mean of its neighbours, a wall giving back its albedo times what it sees: the bounces), never
// through a stopped one. A cell: rgb in halves + its stopped faces toward +x, +y, +z (bits 16-18 of its second word)
#define GIFL_NX 64
#define GIFL_NY 64
#define GIFL_NZ 16
#define GIFL_CELL 50.0f
#define GIFL_MAX_LIGHTS 32

// Each frame (render.c, after the water): a rebuild started when due, a finished one into its GPU buffer; the volume's
// address and the bounce into the frame (water[24..26], GI_FALLBACK)
void gi_fallback_frame(struct VkCtx* vk, GpuFrame* fr, World* w, const float cam[3]);
void gi_fallback_shutdown(struct VkCtx* vk);

typedef struct GiFallbackStats {
    bool ready;
    uint32_t boxes, builds;
    double build_ms;       // the last build on its thread
    float mean_vis;        // the volume's mean sky visibility (0..1)
    // the local lights' bounce: lights used, wall faces lit, the solver's sweeps, its time (in build_ms too), the mean
    // and the brightest cell's light (luminance, E/pi)
    uint32_t fine_lights, fine_faces, fine_iters;
    double fine_ms;
    float fine_mean, fine_max;
} GiFallbackStats;
const GiFallbackStats* gi_fallback_stats(void);
