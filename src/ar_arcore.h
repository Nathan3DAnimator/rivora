// ARCore (notas-dev/xr.md): the AR of a phone with 6 degrees of freedom. The camera's place and direction tracked in the
// real world, the planes it finds (floors, tables, walls), the camera's picture behind the scene and its HDR light
// (the main light's direction and color, the ambient as spherical harmonics).
//   - Android: libarcore_sdk_c.so loaded at run time (dlopen; the APK carries it when Project Settings > Android >
//     Augmented Reality is on, android/package_apk.ps1 -AR): a phone without Google Play Services for AR, or a game
//     without AR, runs as before (the 3-DoF gyroscope of ar.h stays).
//   - The PC: a simulated session (--ar-sim with --ar-image): the picture as the camera's, a floor plane at the level's
//     origin, the camera where the level puts it. The same path through the renderer (the picture behind the scene, the
//     plane receiving the shadows) and the collision, testable without a phone.
// The real world's units become the engine's: ARCore's meters with Y up and -Z forward (right-handed) to centimeters
// with X forward, Y right, Z up; the session's origin (where the phone was when it started) is the level's.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef enum ArcoreState {
    ARCORE_OFF = 0,        // not started
    ARCORE_UNSUPPORTED,    // no ARCore here (a PC, an unsupported phone, the library missing from the APK)
    ARCORE_NEEDS_INSTALL,  // Google Play Services for AR missing or too old (arcore_open_store opens its page)
    ARCORE_NO_PERMISSION,  // the camera not allowed (asked; tried again while the game runs)
    ARCORE_PAUSED,         // the app in the background
    ARCORE_SEARCHING,      // running, not tracking yet (move the phone slowly over a textured surface)
    ARCORE_TRACKING,       // the camera's pose is good
} ArcoreState;

typedef struct ArcoreView {
    ArcoreState state;
    bool simulated;
    float pos[3], fwd[3], right[3], up[3]; // the camera (engine space, cm)
    float hfov_deg, vfov_deg;              // its field of view (the projection ARCore gives, symmetric)
    // the light ARCore estimates (Environmental HDR): valid when it has one
    bool light_valid;
    float light_dir[3];   // toward the main light (engine space)
    float light_rgb[3];   // its intensity (linear, relative: ~1 an overcast day's sun)
    float sh[9][3];       // the ambient (linear, the same scale)
    // the camera's picture: YUV 4:2:0, y w x h (row stride w), uv w/2 x h/2 interleaved (U, V); NULL: none this frame
    const uint8_t* y;
    const uint8_t* uv;
    uint32_t img_w, img_h;
    uint64_t img_serial;    // a new picture each time it changes (the GPU copy made only then: the camera runs at 30 fps)
    float uv_corners[4][2]; // where the screen's corners fall in the picture (0..1): (-1,-1) (1,-1) (-1,1) (1,1), Vulkan's
                            // NDC (y down)
    // the real world's depth (ARCore's Depth API; the simulated session's floor): mm along the camera's view axis, 0
    // unknown; depth_w x depth_h, a new one each time depth_serial changes; where the screen's corners fall in it (as
    // uv_corners). NULL: none (a phone without depth, the occlusion off): the virtual things drawn over everything
    const uint16_t* depth;
    uint32_t depth_w, depth_h;
    uint64_t depth_serial;
    float depth_corners[4][2];
    uint32_t planes;        // the planes tracked
    uint64_t frame;
} ArcoreView;

// The real things hide the virtual ones behind them (the Depth API where the phone has it; the simulated floor on the
// PC): on by default; set before arcore_start / arcore_simulate
void arcore_set_occlusion(bool on);
bool arcore_start(void); // false: no session (arcore_view()->state says why)
void arcore_stop(void);
// The simulated session (PC): rgba (w x h, sRGB) as the camera's picture, copied; hfov: its field of view
bool arcore_simulate(const uint8_t* rgba, uint32_t w, uint32_t h, float hfov_deg);
// The simulated session's camera: the level's (the PC has no tracking)
void arcore_sim_camera(const float pos[3], const float fwd[3], const float right[3], const float up[3], float hfov_deg);
// Each frame: the session updated for a screen of w x h pixels
const ArcoreView* arcore_update(uint32_t screen_w, uint32_t screen_h);
const ArcoreView* arcore_view(void);
const char* arcore_state_name(ArcoreState s);
// The planes that face up (floors, tables) as triangles (world, cm; xyz per vertex): the surfaces that receive the
// virtual things' shadows over the camera's picture. Returns the vertices written (a multiple of 3)
uint32_t arcore_floor_triangles(float* out_xyz, uint32_t max_verts);
// The planes as "AR Plane" actors (a blocking Box Collision each, never drawn): the Characters walk on the real floor
void arcore_sync_planes(World* w);
// A screen point (pixels) onto the real world: the nearest plane hit, its place (cm) and normal
bool arcore_hit(float sx, float sy, float out_pos[3], float out_normal[3]);
// Opens Google Play Services for AR's page in the store (ARCORE_NEEDS_INSTALL)
void arcore_open_store(void);
int arcore_selftest(void); // the simulated session's planes, triangles and hit; 0 = ok
