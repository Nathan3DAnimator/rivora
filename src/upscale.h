// Upscaling (notas-dev/upscaling.md; the plan's 4.4a, B15c, A10.2): the render at a share of the output, rebuilt by a
// temporal upscaler. The engine's own TSR always; FSR, DLSS and XeSS when their runtime is installed (a missing DLL
// hides the option, never a crash). The modes as the vendors name them (Quality 67%, Balanced 58%, Performance 50%,
// Ultra Performance 33%, Native 100%: antialiasing only), and the dynamic resolution: the share follows the GPU's time
// against a frame rate target (steps of 5%, each one reallocating the targets: only when sustained).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum UpscalerKind { UPS_TSR = 0, UPS_FSR, UPS_DLSS, UPS_XESS, UPS_COUNT } UpscalerKind;
typedef enum UpscaleMode {
    UPM_PRESET = 0,      // the scalability preset's share (Low 50%, Medium 60%, High 67%, Epic 75%, Cinematic 100%)
    UPM_NATIVE,          // 100%: the upscaler as antialiasing (DLAA, FSR Native AA)
    UPM_QUALITY,         // 67%
    UPM_BALANCED,        // 58%
    UPM_PERFORMANCE,     // 50%
    UPM_ULTRA,           // 33%
    UPM_COUNT
} UpscaleMode;

const char* upscale_kind_name(int kind);  // "TSR", "AMD FSR", "NVIDIA DLSS", "Intel XeSS"
const char* upscale_mode_name(int mode);  // "Preset", "Native", "Quality"... (English: TR() them)
// The render's share of the output for a mode (UPM_PRESET: the preset's own)
float upscale_mode_scale(int mode, float preset_scale);

// Whether an upscaler can run here, and why not (its runtime / SDK missing, the GPU's vendor): TSR always
bool upscale_available(int kind, const char** why);
// The one to use: the asked if available, else TSR
int upscale_resolve(int kind);

// The dynamic resolution: once a frame with the GPU's time; returns the share to render at (between min and max), the
// same most frames (a change reallocates the render's targets). target_fps 0: off (returns max)
typedef struct UpsDynamic {
    float scale;          // the share now
    double window_ms;     // the GPU time summed this window
    uint32_t window_frames;
    int calm;             // windows well under the target in a row (mobile_res_step)
    uint32_t steps;       // changes made (the measure)
} UpsDynamic;
float upscale_dynamic(UpsDynamic* d, double gpu_ms, float target_fps, float min_scale, float max_scale);
// (2026-11-14, "a resolução dinâmica fica com um blur bem feio") the lowest share the dynamic resolution goes to under a
// mode's share: 70% of it, never under 50% of the output (a Quality 67% went down to 33%: a blur no upscaler hides)
float upscale_dynamic_floor(float top, int min_pct); // min_pct: the project's Dynamic Resolution Min (0: Auto, 70% of top, >= 50%)

// The self-test (--upscale-test): the modes' shares, availability, the dynamic resolution converging on a simulated
// GPU whose time goes with the pixels
bool upscale_selftest(void);
