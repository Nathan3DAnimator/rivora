// The game on phones and tablets running smoothly (notas-dev/android.md "Rodar liso"): a profile per device (its tier
// from the GPU and the memory, or the project's choice), the window's buffers scaled (Unreal's
// r.MobileContentScaleFactor: the compositor's free upscale), a dynamic resolution that holds the frame budget, the frame
// rate paced to the display (and told to it), the heat (Android's thermal headroom: the budget lowered before the system
// throttles), the battery saver (30 fps), ADPF's performance hint (the CPU's clocks matched to the frame's work), the
// texture budget of a shared memory and the low-memory warnings. On the PC: REALENGINE_MOBILE=1 (tier from
// REALENGINE_MOBILE_TIER) runs the same profile, measured.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct GiSettings;
struct VkCtx;

enum { MOBILE_AUTO = 0, MOBILE_LOW, MOBILE_MEDIUM, MOBILE_HIGH };

typedef struct MobileConfig { // (Project Settings > Android; 0: automatic)
    int quality;          // MOBILE_AUTO .. MOBILE_HIGH
    int target_fps;       // 30, 60, 90, 120 (0: 60)
    bool dynamic_res;
    float content_scale;  // the window's buffers against the screen (0: by the tier)
} MobileConfig;

typedef struct MobileState {
    bool on;
    int tier;                 // MOBILE_LOW .. MOBILE_HIGH (as chosen)
    int target_fps, fps_cap;  // the project's; now (the heat, the battery saver lower it)
    float content_scale;      // applied to the window
    float res, res_min, res_max; // the render's share of the output (TSR): now, its range
    int res_changes;
    int thermal;              // Android's thermal status (0 none .. 6 shutdown; -1 unknown)
    float headroom;           // 0..1 before throttling (-1 unknown)
    bool power_save, perf_hint;
    uint64_t texture_budget;  // bytes
    double gpu_avg_ms, cpu_avg_ms, fps;
} MobileState;

// On: Android, or REALENGINE_MOBILE=1. Picks the tier, applies the window's scale and frame rate, the texture budget
bool mobile_init(struct VkCtx* vk, const MobileConfig* cfg);
// The tier's render settings over the scalability's (no traced effects, lighter shadows, clouds, fog; TSR's share)
void mobile_apply(struct GiSettings* gi, int* msaa);
// Once a frame after it was presented: the measures in, the resolution and the cap adjusted, the frame paced (it may
// sleep to the cap), the hint reported. gi: its tsr_scale changed when the resolution steps
void mobile_frame_end(struct GiSettings* gi, double frame_start, double cpu_ms, double gpu_ms);
const MobileState* mobile_state(void);
// The dynamic resolution's controller alone (the test): a window's GPU average against the budget -> the next share
float mobile_res_step(float res, float res_min, float res_max, double gpu_ms, double budget_ms, int* calm_windows);
// The tier from a GPU's name and the memory (the test)
int mobile_tier_for(const char* gpu_name, uint64_t ram_bytes);
int mobile_selftest(void); // failures (--mobile-test)
