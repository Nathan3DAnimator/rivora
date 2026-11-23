// Camera Shakes (Q12, notas-dev/camera-primeira-pessoa.md): Unreal's Camera Shake as an asset (.rshake, a small JSON):
// how long it lasts, its blend in and out, its scale and, for each of the view's seven channels (location forward,
// right, up; pitch, yaw, roll; field of view), an oscillation: a sine, Perlin noise or a curve of keys. Played on the
// player's view by Start Camera Shake (fpcam.h; a handle back), stopped by Stop Camera Shake (blending out) and Play
// World Camera Shake (an epicenter: full inside the inner radius, nothing past the outer one).
// Deterministic: a shake's samples come only from its time and its seed (the noise's and the random phases'), so the
// same shake played twice with the same seed moves the view the same way (the test camshake_determinism).
// Read once, again when the file changes (its time); the built-in presets work without any file ("Explosion").
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { CSW_SINE = 0, CSW_PERLIN, CSW_CURVE, CSW_COUNT }; // a channel's waveform
extern const char* const g_camshake_wave_names[CSW_COUNT];
// The channels: location along the view (cm: forward, right, up), its rotation (degrees: pitch, yaw, roll), the FOV (degrees)
enum { CS_LOC_X = 0, CS_LOC_Y, CS_LOC_Z, CS_PITCH, CS_YAW, CS_ROLL, CS_FOV, CS_CHANNELS };
extern const char* const g_camshake_channel_names[CS_CHANNELS];

#define CS_MAX_KEYS 8
typedef struct CamShakeChannel {
    float amplitude;        // cm or degrees (0: the channel is still)
    float frequency;        // Hz (sine, Perlin)
    uint32_t wave;          // CSW_*
    uint32_t random_offset; // a sine starts at a random phase (Unreal's Initial Offset: Random), else at 0
    uint32_t nkeys;         // a curve's keys: (seconds, value -1..1) times the amplitude; past the last, the last
    float keys[CS_MAX_KEYS][2];
} CamShakeChannel;

typedef struct CamShake {
    float duration;         // seconds (0: until Stop Camera Shake)
    float blend_in, blend_out; // seconds
    float scale;            // the asset's own scale (Start Camera Shake's Scale multiplies it)
    uint32_t single_instance; // played again while it plays: the one playing starts over instead of a second one
    CamShakeChannel ch[CS_CHANNELS];
} CamShake;

void camshake_defaults(CamShake* s);
// The raw oscillation at time t (seconds since it started; no envelope): out[CS_CHANNELS]
void camshake_sample(const CamShake* s, float t, uint32_t seed, float out[CS_CHANNELS]);
// Its envelope at t: the blend in, the blend out at the end (or from stop_t once stopped: < 0 not stopped), 0..1
float camshake_envelope(const CamShake* s, float t, float stop_t);
// Done: past its duration, or blended out after a stop
bool camshake_finished(const CamShake* s, float t, float stop_t);

// The assets: by the path's hash (the same hash as the other assets'), by a name ("CS_Explosion": the Content's .rshake
// of that name, else a built-in preset of that name: "Explosion", "Earthquake", "Handheld", "Hit", "Gunshot",
// "Footstep", "Heartbeat", "Dread"), NULL: none
uint32_t camshake_vpath_hash(const char* vpath);
bool camshake_is_path(const char* vpath);
const CamShake* camshake_get(uint32_t hash);
const CamShake* camshake_find(const char* name_or_vpath);
const char* camshake_vpath(uint32_t hash);
bool camshake_load(const char* vpath, CamShake* s);
bool camshake_save(const char* vpath, const CamShake* s);
bool camshake_create(const char* folder_vpath, const char* name, const CamShake* from, char* out_vpath, uint32_t cap);
void camshake_reload(void);
void camshake_remember(const char* vpath);

typedef struct CamShakePreset { const char* name; CamShake s; } CamShakePreset;
#define CAMSHAKE_PRESETS 8
extern CamShakePreset g_camshake_presets[CAMSHAKE_PRESETS]; // (filled once: camshake_presets())
const CamShakePreset* camshake_presets(void);

// The Camera Shake editor (camshake_editor.c, the editor only): a window with its channels and a preview plot
void camshake_editor_open(const char* vpath);
void camshake_editor_draw(void);
bool camshake_editor_add_menu(const char* folder_vpath); // Content Browser > Add > Camera > Camera Shake (a preset)
