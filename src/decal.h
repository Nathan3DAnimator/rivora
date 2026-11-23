// Decals (M6; notas-dev/materiais.md "Decals"): Unreal's Decal actor and Spawn Decal at Location. A material projected
// along the actor's X onto the opaque surfaces inside its box (100 cm * scale each way): its baked base color and
// alpha, normal, roughness and metallic laid over theirs before the light (mesh.frag RE_EXT, the 16 px tiles of
// decal_bin.comp), so a decal is lit and shadowed as the surface is. Fades by angle, by distance and at the end of its
// life span (bullet holes, blood).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "render_gpu.h"

#define DECAL_MAX 1024 // drawn at once (the nearest first past it)

// The world's decals in view this frame into out (sorted: sort order, then the bigger under the smaller), with their
// box, screen rectangle (vp: the frame's view-projection, column-major, its jitter in; vw x vh render pixels) and every
// fade; the decals with no material or out of view left out. Returns how many
uint32_t decals_gather(World* w, const float vp[16], uint32_t vw, uint32_t vh, const float cam[3], GpuDecal* out, uint32_t cap);
// Their life spans (playing): the age goes on, the actor goes at its end
void decals_update(World* w, float dt, bool playing);
// Unreal's Spawn Decal at Location: a decal actor (size: its half extents in cm; life_span 0: forever)
Entity decal_spawn(World* w, uint32_t material, const float loc[3], const float rot[3], const float size[3], float life_span);

typedef struct DecalStats {
    uint32_t total, drawn; // last frame's: the world's decals, those in view with a material
} DecalStats;
const DecalStats* decal_stats(void);
