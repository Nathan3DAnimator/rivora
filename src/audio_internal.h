// Audio's parts shared between the mixer (audio.c), the assets (audio_assets.c) and the world (audio_world.c).
#pragma once
#include "audio.h"

// --- Sound Attenuation (.rsatt): how a 3D sound fades and moves between the ears with the distance ---
enum { AU_FALLOFF_LINEAR = 0, AU_FALLOFF_LOG, AU_FALLOFF_INVERSE, AU_FALLOFF_LOG_REVERSE, AU_FALLOFF_NATURAL, AU_FALLOFF_COUNT };
enum { AU_SPAT_PROJECT = 0, AU_SPAT_PANNING, AU_SPAT_BINAURAL, AU_SPAT_COUNT }; // the project's (Epic: binaural), or forced
typedef struct AuAttenuation {
    uint32_t hash;
    uint8_t spatialize, falloff, method, air_absorption, occlusion, reverb_send, focus_pad[2];
    float inner_radius;       // cm: full volume within
    float falloff_distance;   // cm beyond the inner radius to silence
    float db_at_max;          // Natural Sound: dB at the falloff's end (Unreal's -60)
    float non_spatial_radius; // cm: closer, it blends to 2D (a sound on the listener isn't in one ear)
    float spread;             // cm: a stereo source's width (its channels apart)
    float lpf_near_hz, lpf_far_hz, absorb_min, absorb_max; // air absorption: the low pass from near to far (cm)
    float occlusion_lpf_hz, occlusion_volume, occlusion_interp; // behind a wall: the low pass, the volume, seconds to change
    float reverb_wet_min, reverb_wet_max, reverb_dist_min, reverb_dist_max; // the reverb send: near (min) to far (max)
    float doppler;            // 0 off .. 1 physical (the pitch with the speed toward the listener)
    float priority_falloff;   // 0..1: a far sound's priority lowered (virtualized first)
} AuAttenuation;
void au_attenuation_defaults(AuAttenuation* a);

// --- Sound Class (.rsclass): a hierarchy of volumes and pitches, the submix its sounds go to ---
typedef struct AuClass {
    uint32_t hash, parent;    // parent 0: none (the root)
    float volume, pitch, lpf_hz;
    uint32_t submix;          // its sounds' submix (0: the parent's; the root's 0: the master)
    uint8_t ui, always_play, pad[2]; // ui: plays while paused (a menu's); always_play: never virtualized
    uint32_t passive_mix;     // a Sound Mix pushed while a sound of it plays above passive_threshold (ducking)
    float passive_threshold;
} AuClass;

// --- Sound Mix (.rsmix): the classes' volumes and pitches changed, faded in and out (push / pop, the base mix) ---
#define AU_MIX_ADJUST 8
typedef struct AuMix {
    uint32_t hash;
    uint32_t nadj;
    struct { uint32_t sound_class; float volume, pitch, lpf_hz; uint8_t children; } adj[AU_MIX_ADJUST];
    float fade_in, fade_out, duration; // seconds (duration 0: until popped)
} AuMix;

// --- Sound Concurrency (.rsconc): how many of a group play at once and what gives way ---
enum { AU_RES_PREVENT_NEW = 0, AU_RES_STOP_OLDEST, AU_RES_STOP_FARTHEST_THEN_PREVENT, AU_RES_STOP_FARTHEST_THEN_OLDEST,
       AU_RES_STOP_LOWEST_PRIORITY, AU_RES_STOP_QUIETEST, AU_RES_STOP_LOWEST_PRIORITY_THEN_PREVENT, AU_RES_COUNT };
typedef struct AuConcurrency {
    uint32_t hash;
    uint32_t max_count;       // 0: no limit
    uint8_t rule, per_owner, pad[2];
    float retrigger_time;     // seconds: a new one sooner is dropped
    float volume_scale;       // each newer one ducks the older ones by this (1: none)
} AuConcurrency;

// --- Submix (.rsubmix): a bus with effects; its output goes to its parent (the master at the end) ---
enum { AUFX_NONE = 0, AUFX_REVERB, AUFX_EQ, AUFX_COMPRESSOR, AUFX_DELAY, AUFX_FILTER, AUFX_COUNT };
#define AU_FX_PARAMS 8
#define AU_BUS_FX 4
typedef struct AuEffect { uint8_t kind, pad[3]; float p[AU_FX_PARAMS]; } AuEffect;
// the effects' parameters (p[]):
//   REVERB: decay s, high-frequency decay ratio, density (room size 0-1), pre-delay ms, wet, dry, early reflections, diffusion
//   EQ: low shelf Hz, gain dB, mid Hz, gain dB, Q, high shelf Hz, gain dB, output dB
//   COMPRESSOR: threshold dB, ratio, attack ms, release ms, makeup dB, knee dB, lookahead ms, -
//   DELAY: time ms, feedback, wet, dry, low pass Hz, ping-pong (0/1), -, -
//   FILTER: type (0 low pass, 1 high pass, 2 band pass), cutoff Hz, Q, -, ...
#define AU_MAX_BUSES 16
enum { AU_BUS_MASTER = 0, AU_BUS_REVERB = 1 }; // the master; the reverb send's (Audio Volumes' reverb); the submixes after
typedef struct AuSubmix {
    uint32_t hash, parent;    // parent 0: the master
    float volume;
    uint32_t nfx;
    AuEffect fx[AU_BUS_FX];
} AuSubmix;
void au_effect_defaults(AuEffect* e, int kind);

// --- Sound Cue (.rscue): a graph evaluated each time it plays into waves to start (audio_assets.c) ---
#define AU_PLAN_WAVES 4
typedef struct AuPlan {
    uint32_t n;
    SoundWave* wave[AU_PLAN_WAVES];
    float volume[AU_PLAN_WAVES], pitch[AU_PLAN_WAVES], delay[AU_PLAN_WAVES]; // delay: seconds after the start
    uint8_t loop[AU_PLAN_WAVES];
    uint8_t sequential;       // a Concatenator: each after the one before (delay: added to its end)
    float volume_all, pitch_all;
    uint32_t attenuation, sound_class, concurrency; // the cue's (0: the first wave's)
    float priority;
} AuPlan;

// The assets' lookups (audio_assets.c): NULL for 0 or a missing one
const AuAttenuation* au_attenuation(uint32_t hash);
const AuClass* au_class(uint32_t hash);
const AuMix* au_mix(uint32_t hash);
const AuConcurrency* au_concurrency(uint32_t hash);
const AuSubmix* au_submix(uint32_t hash);
bool au_plan(uint32_t sound, AuPlan* out); // a Sound Wave or a Sound Cue into the waves to start
void au_assets_root(const char* root);
// The project's audio settings (Project Settings > Audio)
typedef struct AuProject {
    uint32_t max_channels;    // real voices (the rest virtual)
    uint8_t binaural;         // spatialization: 1 binaural (HRTF-like), 0 panning
    float master_volume;
    uint32_t default_class, default_attenuation, default_concurrency, base_mix;
} AuProject;
const AuProject* au_project(void);

// The mixer's buses as the game thread sets them (audio.c: copied by the mixer when changed)
uint32_t au_bus_of_submix(uint32_t submix_hash); // a submix's bus (made on first use; the master for 0)
void au_set_reverb(const AuEffect* reverb, float blend_seconds); // the reverb send's effect (Audio Volumes)

// --- Sound Cue's graph ---
#define AU_CUE_NODES 48
#define AU_CUE_IN 8
enum { CUE_OUTPUT = 0, CUE_WAVE, CUE_RANDOM, CUE_MODULATOR, CUE_MIXER, CUE_CONCAT, CUE_LOOPING, CUE_DELAY, CUE_KIND_COUNT };
typedef struct AuCueNode {
    uint8_t kind, nin, flag, pad;  // flag: Wave Player looping; Random no repeats
    int16_t in[AU_CUE_IN];         // its inputs (node indices; -1: none)
    float x, y;                    // in the editor
    uint32_t wave;                 // Wave Player: the Sound Wave
    float p[4];                    // Modulator: volume min, max, pitch min, max; Delay: min, max s; Mixer: -
    float weight[AU_CUE_IN];       // Random: each input's weight; Mixer: each input's volume
    int8_t last;                   // (Random: the last pick)
} AuCueNode;
typedef struct AuCue {
    uint32_t nnodes;               // [0]: the Output
    AuCueNode n[AU_CUE_NODES];
    float volume, pitch, priority;
    uint32_t attenuation, sound_class, concurrency;
} AuCue;
extern const char* const g_cue_kind_names[CUE_KIND_COUNT];

// --- The assets (one registry: found by their vpath's hash) ---
enum { AUA_ATTENUATION = 0, AUA_CLASS, AUA_MIX, AUA_CONCURRENCY, AUA_SUBMIX, AUA_CUE, AUA_KIND_COUNT };
extern const char* const g_aua_ext[AUA_KIND_COUNT];   // rsatt, rsclass, rsmix, rsconc, rsubmix, rscue
extern const char* const g_aua_names[AUA_KIND_COUNT]; // Unreal's names (Sound Attenuation...)
typedef struct AuAsset {
    int kind;
    char name[64], path[512];      // path: on disk ("" for the engine's own)
    uint32_t hash;
    bool builtin, dirty;
    union { AuAttenuation att; AuClass cls; AuMix mix; AuConcurrency conc; AuSubmix sub; AuCue cue; } u;
} AuAsset;
AuAsset* au_asset(uint32_t hash);
AuAsset* au_asset_load(const char* vpath);
AuAsset* au_asset_create(int kind, const char* vpath); // a new one with its defaults, written
bool au_asset_save(AuAsset* a);
uint32_t au_asset_count(void);
AuAsset* au_asset_at(uint32_t i);
void au_asset_defaults(AuAsset* a, int kind);
const char* au_sound_name(uint32_t hash);
uint32_t audio_asset_by_name(int kind, const char* name); // kind -1: a sound (wave or cue); else AUA_* (0: none)  // a Sound Wave's, a cue's, any audio asset's name ("" if none)
// The engine's own Sound Classes and their hashes (/Engine/Audio/...)
enum { AU_SC_MASTER = 0, AU_SC_MUSIC, AU_SC_SFX, AU_SC_VOICE, AU_SC_UI, AU_SC_AMBIENT, AU_SC_COUNT };
uint32_t au_builtin_class(int which);
uint32_t au_builtin_concurrency(void);
void audio_set_project(const AuProject* p);
// The mixes (Blueprints: Push / Pop Sound Mix Modifier, Set Base Sound Mix, Clear Sound Mix Modifiers)
void audio_push_mix(uint32_t mix_hash);
void audio_pop_mix(uint32_t mix_hash);
void audio_clear_mixes(void);
void audio_set_base_mix(uint32_t mix_hash);
