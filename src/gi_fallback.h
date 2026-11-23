// (Q27.12) The GI's fallback for Low, Android and Quest (notas-dev/gi.md "GI de fallback"): where nothing else lights the
// diffuse (no lightmap, no probes, no surface cache: the GPUs without rays), the sky light is darkened by an AO volume
// (128 x 128 x 32 m around the camera, 2 m cells: the meshes' boxes as occupancy, each cell's sky seen along 5 rays),
// and the ground sends the sun back up (its bounce: an albedo of 0.2 under the sun's light on it, as GTA 6's patent's
// terrain bounce map). Built on a thread when the camera moves 16 m or every 2 s; no GPU pass, a few reads per pixel.
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

// Each frame (render.c, after the water): a rebuild started when due, a finished one into its GPU buffer; the volume's
// address and the bounce into the frame (water[24..26], GI_FALLBACK)
void gi_fallback_frame(struct VkCtx* vk, GpuFrame* fr, World* w, const float cam[3]);
void gi_fallback_shutdown(struct VkCtx* vk);

typedef struct GiFallbackStats {
    bool ready;
    uint32_t boxes, builds;
    double build_ms;       // the last build on its thread
    float mean_vis;        // the volume's mean sky visibility (0..1)
} GiFallbackStats;
const GiFallbackStats* gi_fallback_stats(void);
