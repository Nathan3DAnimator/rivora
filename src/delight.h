// De-lighting (A9.5e; notas-dev/importar-tlender.md "De-lighting"): a scanned (photogrammetry) mesh's color texture
// has the light of the day it was shot baked in (the sun's side brighter, shadows, ambient occlusion). De-lighting
// takes it out so the scene's own light can be added: the mesh's normals drawn into the texture's UV space, a low-order
// lighting (spherical harmonics, 9 terms) fitted to the texture's brightness and divided out, then what is left of the
// broad shading (the soft shadows, the occlusion: a wide blur of the brightness) divided out by a share. Hue kept.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct DelightStats {
    float fit_r2;      // how much of the brightness the fitted lighting explained (0..1)
    float before, after; // the brightness's spread (log units) before and after
    double ms;
} DelightStats;

// In place on an sRGB RGBA8 texture (w x h). nrm: a normal per texel (w * h * 3; NULL: no mesh, only the broad shading),
// mask: the texels the mesh covers (NULL: all). strength 0..1
void delight_rgba(uint8_t* rgba, uint32_t w, uint32_t h, const float* nrm, const uint8_t* mask, float strength, DelightStats* st);
// A texture file and its mesh (.rmesh; NULL: none) into out_png (RGB). false: unread / unwritten
bool delight_file(const char* image, const char* rmesh, const char* out_png, float strength, DelightStats* st);
// --delight-test: a sphere's checker texture lit from one side, de-lit with its normals: the light's share mostly gone
int delight_selftest(void);
