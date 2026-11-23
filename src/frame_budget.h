// The frame budget (notas-dev/orcamento.md; the plan's R11 and 4.4b): the 10 ms of the Epic preset at 1440p split into
// slices, each with its goal. The GPU slices are the render's timestamps grouped as the plan splits the frame; the CPU
// slices are each system's own measure. The Debug Viewer shows them (the "Frame Budget" list), and --budget <ms> makes a
// measured run (--bench) fail when the whole frame or a slice goes past its goal.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum FbSlice {
    // GPU (the goals of R11, for 10 ms)
    FB_GEOMETRY,    // geometry, visibility and materials: the scene's upload, the cull, the main pass's meshes (opaque and translucent)
    FB_SHADOWS,     // direct light and shadows: the sun's virtual shadow maps, the local lights' maps, the shadow rays' setup
    FB_GI,          // GI and reflections: the surface cache, the probes, the traces
    FB_VOLUMES,     // volumes, water, particles and sky: the atmosphere, the weather, the sky / grid and the resolve
    FB_POST,        // camera, color and post: the selection mask, the exposure, depth of field, motion blur, bloom
    FB_RECON,       // reconstruction (TSR or an upscaler) and the composite with the UI
    FB_GPU_COUNT,
    // CPU (each system's measure of the frame)
    FB_CPU_RENDER = FB_GPU_COUNT, // gathering the instances and lights, recording the command buffers
    FB_CPU_BLUEPRINTS,            // the Blueprint VM (Tick, Delays, events) and the Characters' movement
    FB_CPU_PHYSICS,               // rigid bodies, vehicles, cloth
    FB_CPU_ANIMATION,             // skeletal meshes, Anim Blueprints, motion matching
    FB_CPU_AI,                    // Behavior Trees, navigation, crowds
    FB_CPU_PARTICLES,             // the particle systems' CPU side
    FB_CPU_GAMEPLAY,              // the game modules: health, inventory, interaction, dialogue, quests, rules, abilities, HUD
    FB_SLICES
} FbSlice;

typedef struct FbReport {
    double mean[FB_SLICES];   // each slice's mean over the frames added (ms)
    double peak[FB_SLICES];   // its worst frame
    double goal[FB_SLICES];   // its goal for the budget asked
    double gpu_mean, cpu_mean; // the whole frame's GPU and CPU (ms)
    double budget_ms;
    uint32_t frames;
} FbReport;

const char* fb_slice_name(int slice);    // English (the Debug Viewer translates)
bool fb_slice_gpu(int slice);
double fb_goal(int slice, double budget_ms); // the slice's goal: R11's split of 10 ms, scaled to the budget asked

// The measure: once per frame while measuring (the render's detail timestamps on: fb_begin turns them on)
void fb_begin(void);
void fb_add_frame(double cpu_ms, double gpu_ms);
const FbReport* fb_result(double budget_ms);
// The result printed ("[RealEngine] Budget: ..."); false (and "FAIL" lines) when the frame is past the budget, or, strict,
// a slice past its goal (else a slice over is only marked OVER: the scenes that don't fit yet still guard the total)
bool fb_check(double budget_ms, const char* scene, bool strict);

// This frame's slices now (the stats as last read: the GPU ones are the frame before last's)
void fb_sample(double out[FB_SLICES]);
// The Debug Viewer's "Frame Budget" list (debug_viewer.c calls it once)
void fb_register_debug(void);
