// VR and AR (notas-dev/xr.md). VR through OpenXR with no SDK in the build: the active runtime (the registry's
// Khronos\OpenXR\1\ActiveRuntime JSON: Meta Horizon/Link, SteamVR, WMR...) is loaded and negotiated with as the
// OpenXR loader does; on the Quest (Android) the Khronos loader packaged in the APK (libopenxr_loader.so, dlopen).
// Vulkan through XR_KHR_vulkan_enable (the instance's and the device's extensions the runtime asks for, the GPU it
// drives). (Q25) Both eyes are rendered in one frame of the engine: the first eye pays the frame's shared work (the
// simulations, the GI's updates, the shadows), the second only its view (RenderView.stereo_second), and the window shows
// both once (none on the Quest). The head and the controllers (OpenXR actions: trigger, grip, thumbstick, A/B/X/Y, menu,
// poses; tracked hands by XR_EXT_hand_tracking) reach Blueprints and the VR pawn (vr_pawn.h). Without a headset, the
// simulated mode (--vr-sim) draws both eyes side by side in the window, and the tests drive its head and hands.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

// (Q25) What game.ini (Project Settings > VR) asks of the headset: read before xr_init
typedef struct XrConfig {
    bool start;              // vr=1: Start in VR
    int refresh_hz;          // vr_refresh: 72 / 90 / 120 (0: the headset's own)
    int tracking;            // vr_tracking: 0 floor (STAGE, else LOCAL), 1 seated (LOCAL)
    int quality;             // vr_quality: VR_QUALITY_* (project.h; 4 = the Quest's preset)
    int foveation;           // vr_foveation: 0 off .. 3 high (fixed foveated rendering by the shading rate)
    bool foveation_dynamic;  // vr_foveation_dynamic: deeper only while the GPU is short of its time
    bool spacewarp;          // vr_spacewarp: Application SpaceWarp asked (XR_FB_space_warp)
    float render_scale;      // vr_render_scale: the eyes' resolution over the recommended (0.5..1.5)
} XrConfig;
void xr_config_defaults(XrConfig* c);
bool xr_config_key(XrConfig* c, const char* key, const char* value); // a game.ini line ("vr_refresh", "90"); false: not one
void xr_set_config(const XrConfig* c); // before xr_init
const XrConfig* xr_config(void);

typedef struct XrInfo {
    bool runtime;              // a runtime is installed
    bool headset;              // and it has a headset now
    char runtime_name[128], system_name[128];
    uint32_t eye_w, eye_h;     // per eye (px): the recommended times the render scale
    uint32_t rec_w, rec_h;     // the runtime's recommended
    float refresh_hz;          // the display's (0: unknown)
    float refresh_rates[8];    // the display's choices (XR_FB_display_refresh_rate), refresh_count of them
    int refresh_count;
    bool sim;                  // the simulated mode (no headset)
    bool android;              // the Quest standalone (the loader in the APK)
    bool ext_refresh, ext_hands, ext_spacewarp; // the runtime's extensions in use
    bool hand_tracking;        // the headset tracks hands (and the trackers were made)
    bool stage;                // the play space is the floor (STAGE); false: LOCAL (seated, or no stage)
} XrInfo;

// Before Vulkan: finds the runtime, makes the instance, finds the headset; the Vulkan extensions it needs go to vkctx
// (vkctx_extra_*). False: no VR (the game runs on the monitor). sim: no headset is fine (the side by side mode)
bool xr_init(bool sim);
const XrInfo* xr_info(void);
// After Vulkan: the GPU the runtime wants (vkctx picks it: VK_NULL_HANDLE: any) and the session with its swapchains
VkPhysicalDevice xr_vulkan_gpu(VkInstance instance);
bool xr_start_session(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, uint32_t queue_family, VkFormat color_format);
void xr_shutdown(void);
bool xr_active(void);      // a session is running (or the simulated one)
bool xr_focused(void);     // the session has the input (not the headset's own menu over it)
bool xr_set_refresh(float hz); // (XR_FB_display_refresh_rate) asks the display for hz (one of refresh_rates); false: not there

// A frame: wait for the display's time, the events, the head's and the eyes' poses (engine space, cm, relative to the
// play space's origin, X forward, Y right, Z up) and fovs; false: don't render this frame (the headset is off / idle)
typedef struct XrEye {
    float pos[3], fwd[3], right[3], up[3];
    float tan_left, tan_right, tan_up, tan_down; // the fov's tangents (left and down negative)
    uint32_t w, h;
} XrEye;
bool xr_begin_frame(XrEye eyes[2]);
// The eye's image to render into (its view, its image, the format), then released; the frame ended with both
bool xr_eye_target(uint32_t eye, VkImage* image, VkImageView* view, uint32_t* w, uint32_t* h);
void xr_eye_done(uint32_t eye);
void xr_end_frame(void);
double xr_frame_period_ms(void); // the display's period (11.1 ms at 90 Hz; the simulated mode's 90 Hz)

// The controllers (Blueprint Get Motion Controller): pose (engine space as the eyes), trigger, grip, thumbstick, buttons
typedef struct XrHand {
    bool active;
    float pos[3], fwd[3], up[3];         // the grip's pose (the hand holding)
    float aim_pos[3], aim_fwd[3];        // (Q25) the aim's pose (the laser's ray)
    float trigger, grip, stick[2];
    bool primary, secondary, menu, stick_click; // A / X, B / Y, menu, the stick pressed
    // (Q25) tracked hands (XR_EXT_hand_tracking): the 26 joints (OpenXR's order: palm, wrist, thumb 4, index..little 5
    // each), engine space; the trigger and the grip then come from the pinch and the fist
    bool tracked;
    float joint[26][3];
    float pinch;                         // thumb tip to index tip: 1 touching, 0 apart (>= 3.5 cm for an adult's hand)
    float grip_s, pinch_s;               // (their smoothing over the frames: xr_hand_from_joints)
    // (Q25) the fingers one by one (tracked hands): their curls 0 straight .. 1 curled (thumb, index, middle, ring, little),
    // how much each other finger's tip touches the index (thumb, middle, ring, little: 1 touching), the hand's shape
    float curl[5], touch[4];
    int gesture;                         // XR_GESTURE_*
} XrHand;
enum { XR_GESTURE_NONE = 0, XR_GESTURE_OPEN, XR_GESTURE_FIST, XR_GESTURE_POINT, XR_GESTURE_PINCH, XR_GESTURE_THUMBS_UP, XR_GESTURE_PEACE, XR_GESTURE_COUNT };
extern const char* const g_xr_gesture_names[XR_GESTURE_COUNT]; // "None", "Open", "Fist", "Point", "Pinch", "Thumbs Up", "Peace"
const XrHand* xr_hand(int hand); // 0 left, 1 right
void xr_head(float pos[3], float fwd[3], float up[3]);
void xr_haptic(int hand, float amplitude, float seconds);

// (Q25) The VR menu panel: the game's UI drawn into its own image (game_main: ImGui's draw data), shown as an OpenXR
// quad layer at pose (engine space, cm; facing -fwd's way toward the player) and size (cm). The simulated mode keeps
// the pose for the laser's tests. NULL image: no panel this frame
bool xr_panel_target(uint32_t w, uint32_t h, VkImage* image, VkImageView* view); // the quad's swapchain image (acquired)
void xr_panel_done(void);
void xr_panel_place(const float pos[3], const float fwd[3], float width_cm, float height_cm, bool shown);
bool xr_panel_hit(const float origin[3], const float dir[3], float* t, float uv[2]); // a ray against the shown panel

// (Q25) The simulated mode's inputs (the tests and --vr-sim with the keyboard): the head's pose and each hand's
void xr_sim_head(const float pos[3], float yaw_deg, float pitch_deg);
void xr_sim_hand(int hand, const XrHand* h);
// A tracked hand's joints into its gestures: the pinch (thumb tip to index tip), and without a controller the trigger
// (the pinch), the grip (a fist: the middle, ring and little tips near the palm) and the aim (along the index)
void xr_hand_from_joints(XrHand* h);
void xr_hand_shape(XrHand* h); // (the fingers' curls, the touches on the index, the shape: from its joints)
// (Q25b) mixed reality (passthrough, the room, the real depth): xr_mr.h
