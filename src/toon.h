// (Q11) The Toon shading model and the outline (notas-dev/toon.md): a material's toon look (its light in bands by a
// ramp, the shadow's color, a stylized highlight, a rim of light, hatching on the screen, the cast shadows in a color of
// their own) and its outline (Auto Adaptive: the width in pixels follows the distance along the silhouette).
// The values live in the material (.rmat "toon" / "outline"), a Material Instance may override them (.rmi), an actor's
// dynamic instance changes them while the game runs (Set Toon Parameter, Set Toon Color: no bake, the slot's block is
// written). The GPU reads them per material slot (GpuToon, after the GpuMatExt table: shaders/toon.glsl).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TOON_STOPS 4
#define TOON_PATH 260

typedef struct MatToon {
    // the light in bands: the half-Lambert level (n.l / 2 + 1/2, shifted by shadow_threshold) cut into bands (0-1: a
    // smooth ramp), each band's edge softened by softness (a share of a band); the ramp (its stops, or an image read
    // left to right) gives each level's tone; the dark side tends to shadow_color
    int bands;               // 0..8 (0 or 1: no bands, the ramp alone)
    float softness;          // 0..1
    float shadow_threshold;  // 0..1 (0.5: the terminator where n.l = 0)
    float shadow_color[3];   // linear
    float recv_color[3];     // the cast shadows' color (where something else hides the light)
    int nstops;              // 1..4
    float stop_pos[TOON_STOPS];
    float stop_col[TOON_STOPS][3];
    char ramp_image[TOON_PATH]; // an image (left: dark, right: lit); "": the stops
    // the stylized highlight: a hard spot where n.h passes a size; hardness sharpens its edge
    float spec_color[3], spec_strength, spec_size, spec_hardness;
    // the rim of light (luz de borda): the grazing edge facing away, on the side a light reaches
    float rim_color[3], rim_strength, rim_width, rim_softness;
    // hatching on the screen where the light is low: lines every hatch_scale px (at 1440p), crossed when darker
    float hatch_color[3], hatch_strength, hatch_scale, hatch_angle, hatch_threshold;
    float ambient;           // the indirect light's share (1: as the GI says)
    // the outline (any shading model): its color, width in px at 1440p, the Auto Adaptive curve (the width times
    // near_scale at near_dist m, times far_scale at far_dist m and beyond, curve_exp shaping it), the edges found by
    // depth (depth_sens: a share of the distance) and by the normal (crease_deg: 0 none)
    bool outline;
    float outline_color[3], outline_width;
    bool adaptive;
    float near_dist, far_dist, near_scale, far_scale, curve_exp;
    float depth_sens, crease_deg;
} MatToon;

void toon_defaults(MatToon* t);
struct JsonValue;
struct JsonWriter;
void toon_read(MatToon* t, const struct JsonValue* root);          // its keys under "toon" and "outline" (missing: defaults)
void toon_write(struct JsonWriter* w, const MatToon* t, bool shading_toon); // the "toon" object (when Toon) and "outline" (when on)
// The GPU block (12 vec4: shaders/toon.glsl GpuToon); ramp_tex: the ramp image's bindless index (0: the stops)
void toon_pack(const MatToon* t, uint32_t ramp_tex, float out[12][4]);

// A field by name (the Blueprint nodes; spaces and case don't matter): a float (bands, softness, sizes...) or a color.
// False: no such field
bool toon_set_scalar(MatToon* t, const char* name, float v);
bool toon_set_color(MatToon* t, const char* name, const float rgb[3]);
bool toon_get_scalar(const MatToon* t, const char* name, float* v);

// The outline's width in render pixels (radius: half the width) at a distance (cm), for a render h pixels tall
float toon_outline_radius(const MatToon* t, float dist_cm, float render_h);

// CStaticMesh.outline (an actor's own outline, over its material's): mode in the top byte, sRGB color below
enum { OUTLINE_ACTOR_MATERIAL = 0, OUTLINE_ACTOR_ON, OUTLINE_ACTOR_OFF, OUTLINE_ACTOR_COUNT };
static inline uint32_t outline_actor_mode(uint32_t packed) { return packed >> 24; }
