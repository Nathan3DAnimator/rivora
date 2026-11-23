// Rivers (B12c; notas-dev/agua.md "Rios"): Unreal's Water Body River. A Water Body with a River component: its spline
// (CRiver's points in the actor's space, each with its width, depth and speed) sampled by Catmull-Rom into world-space
// samples; the water.c surface follows them (a ribbon, its waves dragged along the flow), the physics floats things on
// them and their current carries them (vehicle_sim.inc water_query).
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct CRiver;
struct CTransform;

#define RIVER_SAMPLES 256
typedef struct RiverSample {
    float p[3], width;   // the center (world cm), the width (cm)
    float dir[2], speed, depth; // the flow's direction (xy, unit), its speed (cm/s), the depth under the surface (cm)
} RiverSample;

// The river's samples (8 a segment, through every point); 0: fewer than 2 points
uint32_t river_samples(const struct CRiver* r, const struct CTransform* t, RiverSample* out, uint32_t max);
// The water at x, y (world cm): on the river (within half its width of the nearest segment), its surface height, current
// (cm/s) and depth there
bool river_at(const RiverSample* s, uint32_t n, float x, float y, float* z, float cur[2], float* depth);
