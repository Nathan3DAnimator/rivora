// (Q16) Console variables (cvars.h): one table, the place of each value by its offset; nothing allocated, a name found
// by a short case-blind scan (~90 names: a fraction of a microsecond, notas-dev/em-jogo.md)
#include "cvars.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_settings.h"
#include "project.h"

#define GI_F(n, f, lo, hi, h) { n, CVW_GI, CVT_FLOAT, (uint16_t)offsetof(GiSettings, f), lo, hi, #f, h }
#define GI_I(n, f, lo, hi, h) { n, CVW_GI, CVT_INT, (uint16_t)offsetof(GiSettings, f), lo, hi, #f, h }
#define GI_B(n, f, h) { n, CVW_GI, CVT_BOOL, (uint16_t)offsetof(GiSettings, f), 0, 1, #f, h }
#define PR_F(n, f, lo, hi, h) { n, CVW_PROJECT, CVT_FLOAT, (uint16_t)offsetof(ProjectSettings, f), lo, hi, #f, h }
#define PR_I(n, f, lo, hi, h) { n, CVW_PROJECT, CVT_INT, (uint16_t)offsetof(ProjectSettings, f), lo, hi, #f, h }
#define PR_B(n, f, h) { n, CVW_PROJECT, CVT_BOOL, (uint16_t)offsetof(ProjectSettings, f), 0, 1, #f, h }
#define PR_V(n, f, h) { n, CVW_PROJECT, CVT_VEC3, (uint16_t)offsetof(ProjectSettings, f), 0, 0, #f, h }
#define GUS(n, opt, t, lo, hi, h) { n, CVW_GUS, t, 0, lo, hi, opt, h }

static const Cvar g_cvars[] = {
    // --- the player's options (game_settings.h): the same as Set Game User Setting ---
    GUS("sg.Scalability", "Scalability", CVT_INT, 0, 4, "The scalability preset: 0 Low, 1 Medium, 2 High, 3 Epic, 4 Cinematic."),
    GUS("r.ScreenPercentage", "ResolutionScale", CVT_INT, 25, 100, "The render's share of the output (%), under the upscaling mode's."),
    GUS("r.VSync", "VSync", CVT_BOOL, 0, 1, "Waits for the display's refresh."),
    GUS("t.MaxFPS", "FrameRateLimit", CVT_INT, 0, 1000, "The frame limit (0: none)."),
    GUS("r.Upscaling.Mode", "UpscalingMode", CVT_INT, 0, 5, "The upscaling mode (Preset, Native, Quality, Balanced, Performance, Ultra Performance)."),
    GUS("r.DynamicResolution", "DynamicResolution", CVT_BOOL, 0, 1, "The render's share follows the GPU's time."),
    GUS("au.MasterVolume", "MasterVolume", CVT_FLOAT, 0, 1, "The master volume (0..1)."),
    GUS("au.MusicVolume", "MusicVolume", CVT_FLOAT, 0, 1, "The Music class's volume."),
    GUS("au.SFXVolume", "SFXVolume", CVT_FLOAT, 0, 1, "The effects' volume."),
    GUS("au.VoiceVolume", "VoiceVolume", CVT_FLOAT, 0, 1, "The voices' volume."),
    GUS("input.MouseSensitivity", "MouseSensitivity", CVT_FLOAT, 0.05f, 10, "The mouse's sensitivity (x the project's)."),
    GUS("input.InvertY", "InvertY", CVT_BOOL, 0, 1, "The look's Y inverted."),
    // (Q25) VR (Project Settings > VR; vr_pawn.c reads them every frame)
    PR_I("vr.Locomotion", vr.locomotion, 0, 2, "VR locomotion: 0 Teleport, 1 Smooth, 2 both."),
    PR_I("vr.Turn", vr.turn, 0, 2, "VR turning: 0 Snap, 1 Smooth, 2 none."),
    PR_F("vr.SnapAngle", vr.snap_angle, 15, 90, "Degrees of a VR snap turn."),
    PR_F("vr.TurnSpeed", vr.turn_speed, 30, 360, "Degrees a second of the VR smooth turn."),
    PR_F("vr.MoveSpeed", vr.move_speed, 50, 600, "cm/s of the VR smooth walk."),
    PR_F("vr.TeleportRange", vr.teleport_range, 200, 3000, "cm the VR teleport's arc reaches."),
    PR_F("vr.Vignette", vr.vignette, 0, 1, "The VR comfort vignette's strength (0: off)."),
    PR_F("vr.TeleportFade", vr.teleport_fade, 0, 0.5f, "Seconds of the blink at a VR teleport (0: none)."),
    PR_B("vr.TurnFade", vr.turn_fade, "A blink at each VR snap turn."),
    PR_I("vr.DominantHand", vr.dominant_hand, 0, 1, "The VR dominant hand: 0 right, 1 left."),
    PR_I("vr.MoveDirection", vr.move_direction, 0, 1, "The VR smooth walk toward 0 the head, 1 the hand."),
    PR_B("vr.Hands", vr.hands, "The VR hands shown."),
    PR_B("vr.HandTracking", vr.hand_tracking, "The real hands (hand tracking) used when the headset has them."),
    PR_B("vr.Laser", vr.laser, "The VR laser pointer."),
    PR_F("vr.LaserLength", vr.laser_length, 100, 5000, "cm of the VR laser."),
    PR_B("vr.DragToMove", vr.drag_locomotion, "A VR hand closed in the air pulls the player along."),
    PR_B("vr.PhysicalHands", vr.physical_hands, "The VR hands stop at walls and tables."),
    PR_B("vr.HandObjects", vr.hand_objects, "The physical VR hands stop at bodies and Interactables too (off: the level only)."),
    PR_B("vr.HandPush", vr.hand_push, "The VR hands push bodies with physics."),
    PR_B("vr.HeldCollide", vr.held_collide, "What a VR hand holds stops at walls."),
    PR_I("vr.HeadCollision", vr.head_collision, 0, 2, "The VR head into a wall: 0 nothing, 1 fade to black, 2 push back."),
    PR_B("vr.HideBody", vr.hide_body, "The VR pawn's own body hidden."),
    PR_I("vr.GrabGesture", vr.grab_gesture, 0, 3, "What a VR tracked hand does to grab: 0 a fist, 1 thumb to index, 2 any finger to the index, 3 a fist or any finger to the index."),
    PR_I("vr.DragGesture", vr.drag_gesture, 0, 3, "What a VR tracked hand does to drag the world (the same choices as vr.GrabGesture)."),
    PR_I("vr.MirrorMode", vr.mirror_mode, 0, 2, "The PC window's VR mirror: 0 both eyes, 1 one eye, 2 the Spectator Camera (recording, streaming)."),
    PR_F("vr.SpectatorFov", vr.spectator_fov, 50, 120, "Degrees across the VR Spectator Camera."),
    PR_F("vr.SpectatorSmooth", vr.spectator_smooth, 0, 0.95f, "How slowly the VR Spectator Camera follows the head."),
    PR_B("vr.SafetyNet", vr.safety_net, "The VR pawn fallen out of the level is put back where it last stood."),
    PR_F("vr.GrabReach", vr.grab_reach, 2, 40, "cm from a VR hand a Grabbable is taken."),
    PR_B("vr.DistanceGrab", vr.distance_grab, "The VR grip on what the laser points at pulls it into the hand."),
    PR_F("vr.ThrowScale", vr.throw_scale, 0.2f, 3, "The VR throw's speed times this."),
    PR_F("vr.Haptics", vr.haptics, 0, 1, "The VR controllers' vibration strength (0: off)."),
    PR_F("vr.MenuDistance", vr.menu_distance, 50, 400, "cm from the head the VR menu panel opens."),
    PR_F("vr.MenuWidth", vr.menu_width, 40, 400, "cm wide the VR menu panel is."),
    PR_I("vr.HeadroomFps", vr.headroom_fps, 0, 240, "The frame rate the VR GPU's two eyes are kept able to reach (dynamic foveation's target; 0: the display's)."),
    PR_I("vr.Foveation", vr.foveation, 0, 3, "VR fixed foveated rendering: 0 off .. 3 high (the game reads it at its start)."),
    // (Q25b) mixed reality (xr_mr.h)
    PR_B("vr.MixedReality", vr.mr_enabled, "Mixed reality's extensions asked of the headset (passthrough, the room, the real depth; read when the headset starts)."),
    PR_B("vr.Passthrough", vr.mr_passthrough, "The real world seen through the headset's cameras."),
    PR_I("vr.PassthroughBackground", vr.mr_background, 0, 1, "Where the room shows: 0 in the level's sky's place, 1 only through windows (the sky kept)."),
    PR_F("vr.PassthroughOpacity", vr.mr_opacity, 0, 1, "The passthrough's opacity."),
    PR_I("vr.PassthroughStyle", vr.mr_style, 0, 3, "The passthrough's look: 0 color, 1 black & white, 2 one color, 3 adjusted (brightness, contrast, saturation)."),
    PR_F("vr.PassthroughBrightness", vr.mr_brightness, -100, 100, "The adjusted passthrough's brightness."),
    PR_F("vr.PassthroughContrast", vr.mr_contrast, 0, 4, "The adjusted passthrough's contrast (1: as seen)."),
    PR_F("vr.PassthroughSaturation", vr.mr_saturation, 0, 4, "The adjusted passthrough's saturation (1: as seen)."),
    PR_F("vr.PassthroughBlend", vr.mr_virtual_opacity, 0, 1, "The virtual things' opacity over the room (1 solid; 0: only the room)."),
    PR_I("vr.MROcclusion", vr.mr_occlusion, 0, 2, "Real things hiding the virtual ones: 0 none, 1 the room's planes and furniture, 2 the real depth."),
    PR_F("vr.MROcclusionSoftness", vr.mr_occlusion_soft, 0.5f, 30, "cm of the occlusion's soft edge."),
    PR_B("vr.MRHandRemoval", vr.mr_hand_removal, "The real depth without the hands (the virtual hands drawn over them)."),
    PR_F("vr.MRShadows", vr.mr_shadows, 0, 1, "The virtual things' shadows on the real surfaces (0: none)."),
    PR_I("vr.MRLight", vr.mr_light, 0, 1, "The light in mixed reality: 0 a room's (the sun from above, warm), 1 the level's."),
    PR_F("vr.MRExposure", vr.mr_ev100, 0, 16, "The exposure (EV100) while the room is seen; 0: automatic (only the virtual things metered)."),
    PR_B("vr.MRRoomActors", vr.mr_scene_actors, "The room's planes and furniture as actors that collide."),
    PR_B("vr.MRRoomMesh", vr.mr_room_mesh, "The room's mesh loaded too (line traces on any real thing)."),
    PR_B("vr.MRAlignRoom", vr.mr_align, "The level turned to the room's walls when the room loads."),
    PR_B("vr.MRRealWindows", vr.mr_windows_real, "The room's real windows and doors show through the virtual walls."),
    PR_B("vr.MRHideFloor", vr.mr_hide_floor, "The level's floor planes not drawn while the room is seen (they still collide)."),
    PR_B("vr.MRDepthFlipY", vr.mr_depth_flip, "The real depth's rows read from the bottom (a runtime's convention)."),
    PR_B("vr.MRUnpremultiplied", vr.mr_unpremultiplied, "The eyes' alpha given to the compositor unpremultiplied (if the virtual things' edges show a dark ring over the room)."),
    // --- the view's render and GI (gi.h GiSettings: the scalability preset sets them, these change one) ---
    { "r.MSAA", CVW_MSAA, CVT_INT, 0, 1, 8, "msaa", "MSAA samples (1, 2, 4, 8)." },
    GI_B("r.GI.Probes", probes, "The dynamic GI's probes around the camera."),
    GI_I("r.GI.RaysPerProbe", rays_per_probe, 8, 64, "The probes' rays (32 or 64)."),
    GI_F("r.GI.ProbeSpacing", probe_spacing, 50, 1000, "The probes' spacing (cm)."),
    GI_F("r.GI.Intensity", intensity, 0, 8, "The indirect light's scale (1: physical)."),
    GI_B("r.GI.SurfaceCache", surface_cache, "The surface cache (real-time radiosity on the surfaces near the camera)."),
    GI_I("r.GI.CacheRays", cache_rays, 1, 64, "The surface cache's rays per texel and update."),
    GI_F("r.GI.CacheTexel", cache_texel, 5, 200, "The surface cache's density (cm per texel)."),
    GI_I("r.GI.CachePerFrame", cache_per_frame, 1024, 4194304, "The surface cache's texels updated a frame."),
    GI_B("r.GI.ContactAO", contact_ao, "The Movable actors darken the indirect light around them."),
    GI_B("r.GI.ReSTIR", restir_gi, "The denoiser's rays reused by their neighbours (ReSTIR GI)."),
    GI_B("r.GI.DenoiseChecker", den_checker, "The unified denoiser's rays in a checkerboard."),
    GI_B("r.Reflections", reflections, "Ray traced reflections."),
    GI_F("r.Reflections.MaxRoughness", reflection_roughness, 0, 1, "Reflections traced below this roughness."),
    GI_B("r.SSR", ssr, "Screen space reflections where no ray traces them."),
    GI_B("r.AmbientOcclusion", gtao, "Screen space ambient occlusion (GTAO)."),
    GI_B("r.ContactShadows", contact_shadows, "The sun's screen space contact shadows."),
    GI_B("r.RayTracedShadows", rt_shadows, "Ray traced shadows for every light."),
    GI_I("r.RayTracedShadows.Rays", shadow_rays, 1, 16, "An area light's shadow rays a pixel."),
    GI_I("r.SSS.Samples", sss_samples, 0, 64, "The skin's screen space scattering's samples (0: 16)."),
    GI_B("r.VRS", vrs, "Variable rate shading by last frame's detail."),
    GI_B("r.VolumetricFog", volumetric_fog, "The height fog lit per froxel (shadows, local lights)."),
    GI_B("r.Clouds.Screen", screen_clouds, "The clouds marched in the view (else the panorama alone)."),
    GI_B("r.Clouds.Half", clouds_half, "The clouds at half the view each way (sharper)."),
    GI_I("r.Shadow.Virtual.Levels", vsm_levels, 0, 16, "The sun's virtual shadow map levels (0: none)."),
    GI_I("r.Shadow.Virtual.Resolution", vsm_res, 512, 8192, "Their texels a side."),
    GI_I("r.Shadow.Virtual.Taps", vsm_taps, 1, 64, "Their PCSS taps."),
    GI_I("r.Shadow.Virtual.FullBudget", vsm_full_budget, 0, 16, "Their levels drawn whole a frame."),
    GI_F("r.Shadow.Virtual.SunAngle", vsm_sun_angle, 0, 5, "The sun's angular diameter (degrees)."),
    GI_B("r.Shadow.Virtual.Sun", vsm_sun, "The sun on the virtual shadow maps even when the other lights are traced."),
    GI_I("r.Shadow.Local.Lights", lsm_lights, 0, 256, "The local lights with a shadow map at once."),
    GI_I("r.Shadow.Local.Atlas", lsm_atlas, 512, 16384, "The local shadow maps' atlas side."),
    GI_I("r.Shadow.Local.Resolution", lsm_res, 64, 4096, "A face's largest side."),
    GI_I("r.Shadow.Local.Taps", lsm_taps, 4, 9, "The filter's taps (4 or 9)."),
    GI_I("r.Shadow.Local.Redraw", lsm_redraw, 0, 64, "Faces redrawn a frame."),
    GI_F("r.LOD.ErrorPixels", lod_error_px, 0.25f, 32, "The error on screen a mesh's LOD may have (px)."),
    GI_F("r.TSR.Scale", tsr_scale, 0, 1, "Temporal super resolution's share of the output (1 native, 0 off: MSAA alone)."),
    GI_B("r.Tessellation", tessellation, "(Q17) The displaced materials (Enable Tessellation) moved for real."),
    GI_F("r.Tessellation.DicingRate", tess_px, 0.5f, 64, "(Q17) Their micro triangles' edge on screen (px)."),
    GI_I("r.Tessellation.MaxFactor", tess_max, 1, 32, "(Q17) The most micro edges along a base edge (1: displaced, not diced)."),
    GI_F("r.Tessellation.Budget", tess_budget, 0.1f, 64, "(Q17) Micro triangles a frame at most (millions): past it, coarser."),
    GI_B("r.FarShadow", far_shadow, "(Q21) The terrain's far shadow: kilometers past the sun's virtual maps."),
    GI_I("r.FarShadow.Resolution", far_res, 256, 4096, "(Q21) Its heightfield's texels a side."),
    GI_B("r.Impostors", impostors, "(Q21) Far instances drawn by their octahedral impostor."),
    GI_F("r.Impostors.ScreenSize", impostor_px, 1, 512, "(Q21) The screen radius (px) under which the impostor is drawn."),
    GI_I("r.Impostors.Resolution", impostor_res, 256, 2048, "(Q21) An impostor's atlas side (8 x 8 frames)."),
    GI_F("r.Fog.SkyAtmosphere", fog_sky, 0, 1, "(Q21) The far fog's ambient light from the sky's horizon (0..1)."),
    // --- the project's render settings (Project Settings > Rendering: read each frame) ---
    PR_I("r.RenderStyle", render_style, 0, 3, "The render style: 0 Stylized, 1 Realistic, 2 Photorealistic, 3 Video-realistic."),
    PR_B("r.AutoExposure", auto_exposure, "The automatic exposure."),
    PR_F("r.ExposureCompensation", exposure_compensation, -10, 10, "The exposure's compensation (EV)."),
    PR_F("r.FilmGrain", film_grain, 0, 2, "The film grain (0 none, 1 the style's)."),
    PR_I("r.ShadowDenoise", shadow_denoise, 0, 3, "The shadows' denoise (0 Low .. 3 Epic)."),
    PR_I("r.ImageDenoise", image_denoise, 0, 2, "The traced light's denoiser (0 Automatic, 1 On, 2 Off)."),
    PR_B("r.AsyncCompute", async_compute, "The particles' simulation on the compute queue."),
    PR_B("r.VariableRateShading", vrs, "Variable rate shading (the project's)."),
    PR_B("r.TSR.Disocclusion", tsr_disocclusion, "(Q20) The TSR drops the history a moving actor uncovers (no trail behind it)."),
    PR_B("r.Denoiser.HistoryFix", den_history_fix, "(Q20) The denoiser's short histories take their converged neighbors (no noisy trail)."),
    PR_B("r.DynamicResolution.Project", dyn_res, "The project's dynamic resolution."),
    PR_I("r.DynamicResolution.TargetFPS", dyn_res_fps, 20, 500, "Its frame rate target."),
    PR_I("r.DynamicResolution.MinPercent", dyn_res_min, 0, 100, "Its smallest share (%, 0: automatic)."),
    PR_I("r.Upscaler", upscaler, 0, 4, "The upscaler (TSR, FSR, DLSS, XeSS where their runtime is)."),
    PR_I("r.Upscaler.ProjectMode", upscale_mode, 0, 5, "The project's upscaling mode (the player's option r.Upscaling.Mode goes over it)."),
    PR_B("r.PathTracing", game_path_tracing, "The game drawn by the path tracer (Quality mode)."),
    PR_I("r.Stylized.Bands", stylized_bands, 1, 8, "The Stylized style's light bands."),
    PR_F("r.Stylized.Softness", stylized_softness, 0, 1, "Their softness."),
    PR_F("r.Stylized.Saturation", stylized_saturation, 0, 2, "Its saturation."),
    PR_V("r.Stylized.ShadowTint", stylized_shadow_tint, "The shadows' tint (\"r g b\")."),
    PR_V("r.Stylized.OutlineColor", stylized_outline_color, "The outline's color (\"r g b\")."),
    PR_F("r.Stylized.Outline", stylized_outline, 0, 8, "Its outline's width (px, 0 off)."),
    PR_F("r.Stylized.OutlineDepth", stylized_outline_depth, 0, 1000, "How far apart in depth an edge is."),
    PR_I("r.Stylized.AnimFPS", stylized_anim_fps, 0, 60, "The animations' own frame rate (0 smooth)."),
    PR_F("r.Video.Distortion", video_distortion, 0, 2, "The Video-realistic lens's distortion (x the style's)."),
    PR_F("r.Video.Fringe", video_fringe, 0, 2, "Its color fringes."),
    PR_F("r.Video.Halation", video_halation, 0, 2, "Its halation."),
    PR_F("r.Video.Vignette", video_vignette, 0, 2, "Its vignette."),
    // --- physics (Project Settings > Physics: read each step) ---
    PR_F("p.GravityZ", gravity_z, -100000, 100000, "Gravity (cm/s², positive: down; 980 the Earth's)."),
    PR_I("p.MaxSubsteps", max_substeps, 1, 32, "The physics' 120 Hz steps a frame at most."),
    PR_I("p.SolverSubsteps", solver_substeps, 1, 32, "The solver's substeps in each step (stiffer stacks and chains)."),
    PR_I("p.Rate", physics_rate, 30, 240, "The physics' fixed steps a second (Hz; Physics Quality: Epic 120, High 90, Medium and Low 60)."),
    // --- audio (Project Settings > Audio) ---
    PR_I("au.MaxChannels", audio_max_channels, 1, 1024, "The voices mixed at once (the rest virtual)."),
    PR_B("au.Binaural", audio_binaural, "Binaural spatialization (headphones)."),
    PR_F("au.ProjectMasterVolume", audio_master_volume, 0, 1, "The project's master volume."),
    // --- saving (Project Settings > Save Game) ---
    PR_F("save.AutoSaveInterval", autosave_interval, 0, 3600, "The timed auto save (s, 0: off)."),
    PR_I("save.AutoSaveSlots", autosave_slots, 1, 16, "Its slots in turn."),
    PR_B("save.AutoSaveOnLevel", autosave_on_level, "An auto save when a level opens."),
    PR_B("save.Thumbnail", save_thumbnail, "The picture of the screen in each save."),
    PR_F("input.ProjectMouseSensitivity", mouse_sensitivity, 0.001f, 10, "The project's mouse sensitivity (degrees a pixel)."),
};
#define NCVAR ((int)(sizeof g_cvars / sizeof g_cvars[0]))

static struct {
    GiSettings* gi;
    int* msaa;
    // a play in the editor: what was there before it
    bool playing, project_changed;
    GiSettings gi0;
    int msaa0;
    ProjectSettings proj0;
} C;

// the view's values set before a view is bound (a game's BeginPlay runs before its view exists): kept here, given to the
// view when it is bound
static GiSettings S_gi;
static int S_msaa;
static uint8_t S_pending[sizeof g_cvars / sizeof g_cvars[0]];
static size_t type_bytes(const Cvar* c) { return c->type == CVT_BOOL ? sizeof(bool) : c->type == CVT_VEC3 ? 12 : 4; }

void cvar_bind_view(GiSettings* gi, int* msaa) {
    C.gi = gi, C.msaa = msaa;
    for (int i = 0; i < NCVAR; i++) {
        const Cvar* c = &g_cvars[i];
        if (!S_pending[i]) continue;
        if (c->where == CVW_GI && gi) memcpy((uint8_t*)gi + c->off, (uint8_t*)&S_gi + c->off, type_bytes(c)), S_pending[i] = 0;
        if (c->where == CVW_MSAA && msaa) *msaa = S_msaa, S_pending[i] = 0;
    }
}
void cvar_begin_play(void) {
    C.playing = true, C.project_changed = false;
    if (C.gi) C.gi0 = *C.gi;
    if (C.msaa) C.msaa0 = *C.msaa;
    C.proj0 = *project();
}
void cvar_end_play(void) {
    if (!C.playing) return;
    C.playing = false;
    if (C.gi) *C.gi = C.gi0;
    if (C.msaa) *C.msaa = C.msaa0;
    if (C.project_changed) *project() = C.proj0, project_apply_audio();
    C.project_changed = false;
}

int cvar_count(void) { return NCVAR; }
const Cvar* cvar_at(int i) { return i >= 0 && i < NCVAR ? &g_cvars[i] : NULL; }
const Cvar* cvar_find(const char* name) {
    if (!name) return NULL;
    while (*name == ' ') name++;
    // (2026-11-23) the tessellation's old names (before the open source release) still answer: scripts and game.ini
    static const char* const old[4][2] = { { "r.Nanite.Tessellation", "r.Tessellation" }, { "r.Nanite.DicingRate", "r.Tessellation.DicingRate" },
                                           { "r.Nanite.MaxTessFactor", "r.Tessellation.MaxFactor" }, { "r.Nanite.TessBudget", "r.Tessellation.Budget" } };
    for (int k = 0; k < 4; k++) {
        size_t n = strlen(old[k][0]);
        if (!_strnicmp(name, old[k][0], n) && (!name[n] || name[n] == ' ')) return cvar_find(old[k][1]);
    }
    for (int i = 0; i < NCVAR; i++) {
        const char *a = g_cvars[i].name, *b = name;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (!*a && (!*b || *b == ' ')) return &g_cvars[i];
    }
    return NULL;
}

static void* place(const Cvar* c) {
    switch (c->where) {
    case CVW_GI: return C.gi ? (uint8_t*)C.gi + c->off : (uint8_t*)&S_gi + c->off; // (unbound: kept for the view)
    case CVW_MSAA: return C.msaa ? C.msaa : &S_msaa;
    case CVW_PROJECT: return (uint8_t*)project() + c->off;
    default: return NULL;
    }
}

static bool bound(const Cvar* c) { return c->where == CVW_GI ? C.gi != NULL : c->where == CVW_MSAA ? C.msaa != NULL : true; }

bool cvar_get(const char* name, float* value, char* text, size_t cap) {
    const Cvar* c = cvar_find(name);
    if (!c) return false;
    float v = 0.0f;
    if (c->where == CVW_GUS) {
        char t[96] = "";
        if (!gus_get(c->field, &v, t, sizeof t)) return false;
    } else {
        void* p = place(c);
        if (!p || (!bound(c) && !S_pending[c - g_cvars])) return false; // (no view yet, nothing set: unknown)
        v = c->type == CVT_FLOAT || c->type == CVT_VEC3 ? *(float*)p : c->type == CVT_INT ? (float)*(int*)p : *(bool*)p ? 1.0f : 0.0f;
        if (c->type == CVT_VEC3 && text && cap) { // ("r g b")
            const float* f = p;
            if (value) *value = v;
            snprintf(text, cap, "%g %g %g", f[0], f[1], f[2]);
            return true;
        }
    }
    if (value) *value = v;
    if (text && cap) {
        if (c->type == CVT_FLOAT) snprintf(text, cap, "%g", v);
        else snprintf(text, cap, "%d", (int)lroundf(v));
    }
    return true;
}

bool cvar_set(const char* name, float value, const char* text) {
    const Cvar* c = cvar_find(name);
    if (!c) return false;
    float v = value;
    if (c->type == CVT_VEC3) { // "r g b" (one number: a gray)
        float* f = place(c);
        float r = value, g = value, b = value;
        if (text && text[0] && sscanf(text, "%f %f %f", &r, &g, &b) < 3) g = b = r;
        f[0] = r, f[1] = g, f[2] = b;
        C.project_changed = true;
        return true;
    }
    if (text && text[0]) {
        if (!strcmp(text, "true") || !strcmp(text, "True") || !strcmp(text, "on")) v = 1.0f;
        else if (!strcmp(text, "false") || !strcmp(text, "False") || !strcmp(text, "off")) v = 0.0f;
        else if (c->where == CVW_GUS && !(isdigit((unsigned char)text[0]) || text[0] == '-' || text[0] == '.')) { // ("Epic")
            bool ok = gus_set(c->field, 0.0f, text);
            if (ok) gus_apply_live();
            return ok;
        } else v = (float)atof(text);
    }
    if (c->lo < c->hi) v = v < c->lo ? c->lo : v > c->hi ? c->hi : v;
    if (c->type != CVT_FLOAT) v = roundf(v);
    if (c->where == CVW_GUS) {
        if (!gus_set(c->field, v, NULL)) return false;
        if (!strcmp(c->field, "VSync")) gus_apply(false); // (the swapchain: the display's part)
        else gus_apply_live();
        return true;
    }
    void* p = place(c);
    if (!p) return false;
    if (!bound(c)) S_pending[c - g_cvars] = 1;
    if (c->type == CVT_FLOAT) *(float*)p = v;
    else if (c->type == CVT_INT) *(int*)p = (int)v;
    else *(bool*)p = v != 0.0f;
    if (c->where == CVW_MSAA) { // (1, 2, 4 or 8)
        int m = (int)v, k = 1;
        while (k * 2 <= m && k < 8) k *= 2;
        *(int*)p = k;
    }
    if (c->where == CVW_PROJECT) {
        C.project_changed = true;
        if (!strncmp(c->name, "au.", 3)) project_apply_audio();
    }
    return true;
}

bool cvar_exec(const char* line, char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!line) return false;
    while (*line == ' ') line++;
    char name[96];
    size_t n = 0;
    while (line[n] && line[n] != ' ' && line[n] != '=' && n + 1 < sizeof name) name[n] = line[n], n++;
    name[n] = 0;
    if (!n) return false;
    const char* arg = line + n;
    while (*arg == ' ' || *arg == '=') arg++;
    const Cvar* c = cvar_find(name);
    if (!c) {
        if (out && cap) snprintf(out, cap, "Unknown command: %s", name);
        return false;
    }
    bool ok = *arg ? cvar_set(c->name, 0.0f, arg) : true;
    char t[96] = "";
    if (ok) ok = cvar_get(c->name, NULL, t, sizeof t);
    if (out && cap) snprintf(out, cap, ok ? "%s = %s" : "%s: not available here", c->name, t);
    return ok;
}
