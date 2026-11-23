// Heterogeneous Volumes (A9.5a; notas-dev/particulas.md "Volumes VDB"): OpenVDB files (.vdb: EmberGen, Houdini, Blender's
// smoke caches, the Tlender's; read by vdb.c, the Tlender's own reader) and their sequences played by a Heterogeneous
// Volume actor (CVolume). Each frame resampled at load into a dense grid (temperature K, density), the box the files
// cover; fire.c draws them as it draws its fires (the flame's black body, the smoke lit by the sun through itself).
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct CVolume;

typedef struct VolumeClip {
    uint32_t nx, ny, nz, frames; // its grid (cells), its frames
    float cell;                  // cm a cell
    float center[3], bottom;     // the box in the file's space turned to the engine's (cm): its middle (x, y) and floor (z)
    uint16_t* data;              // frames x cells x 2 half floats: temperature (K), density
    uint32_t key;                // the file and the options it was made with
} VolumeClip;

// Loads the clip a Volume asks for (its file or sequence, its options); false: unread (clip left empty)
bool volume_load(const struct CVolume* v, VolumeClip* clip);
void volume_free(VolumeClip* clip);
uint32_t volume_key(const struct CVolume* v);
// The example (Content/Volumes/Example_Puff_0001.vdb .. 0024: a puff of smoke rising from a burning core), made when
// missing by vdb.c's writer; false: not made
bool volume_examples(void);
