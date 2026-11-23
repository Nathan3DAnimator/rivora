// AR (notas-dev/xr.md): the virtual scene matched to the camera's picture so it sits in the real world.
//   - Light and color matching (any camera picture, every platform): the picture's exposure (its mean luminance as an
//     EV100), its white balance (gray world: the light's color temperature and tint, McCamy), the main light (the
//     brightest region's direction in the camera's view and how much it stands out), and the ambient light as 9
//     spherical harmonics (the picture spread over the front half of the sphere, the back mirrored). Applied to the
//     level's sun and sky light and the view's exposure: virtual things get the real light's color, direction and
//     strength. Smoothed over time (a camera's auto exposure doesn't flicker the scene).
//   - The device's rotation (Android: the rotation vector sensor, fused gyroscope + accelerometer + compass): the 3-DoF
//     AR camera (looking around a fixed point) when there is no tracking.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef struct ArLight {
    float ev100;          // the picture's exposure
    float kelvin, tint;   // its white balance (tint: + magenta, - green)
    float color[3];       // the light's color (linear, max 1)
    float dir_view[3];    // the main light's direction in the camera's view (x right, y up, z forward); 0: none found
    float strength;       // how much it stands out (0 overcast .. 1 a hard sun)
    float sh[9][3];       // the ambient light (linear, the picture's scale)
    float mean_lum;       // the picture's mean luminance (0..1 encoded)
} ArLight;

// A picture (RGBA8, sRGB) into its light; hfov: the camera's horizontal field of view (degrees)
void ar_estimate(const uint8_t* rgba, uint32_t w, uint32_t h, float hfov_deg, ArLight* out);
// Blends toward a new estimate (t: 0..1 this frame's share)
void ar_blend(ArLight* cur, const ArLight* next, float t);
// The level's first sun and sky light made to match (the camera's basis, engine space); the exposure for the view
float ar_apply(World* w, const ArLight* l, const float cam_fwd[3], const float cam_right[3], const float cam_up[3]);
// The device's rotation as a camera basis (Android's rotation vector sensor); false: no sensor (PC)
bool ar_device_rotation(float fwd[3], float right[3], float up[3]);
int ar_selftest(void); // synthetic pictures: warm / cool / a bright corner; 0 = ok
