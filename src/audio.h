// Audio (notas-dev/audio.md): the engine's own mixer, Unreal's audio as its parts are named there.
//   * the device: WASAPI shared mode, event driven (~10 ms), on its own thread at the "Pro Audio" priority; float32
//     stereo; a silent device (the same mixing, nothing heard) under REALENGINE_BACKGROUND / REALENGINE_NO_AUDIO or
//     without a device; a lost device (headphones pulled) is opened again
//   * Sound Wave (.rsound): PCM 16 bits, mapped from its file (no load: the OS pages it in; a long one is prefetched
//     ahead of where it plays: streaming); its settings (volume, pitch, looping, class, attenuation, concurrency)
//   * voices (Unreal's Active Sounds): started, stopped, faded, paused from the game thread through a queue; their
//     parameters (volume, pitch, direction, filters, sends) published every frame, smoothed per sample on the mixer's
//     side (no clicks); cubic (Hermite) resampling; more than the real voices: the quietest are virtual (time runs,
//     nothing mixed)
//   * spatialization: equal-power panning or binaural (the head's delay and shadow between the ears, Epic); the
//     attenuation's distance curve, air absorption (a low pass with the distance), occlusion, doppler; a stereo
//     source's spread
//   * mixing: Sound Classes (a hierarchy: volume, pitch, the submix), Sound Mixes (pushed / popped: the classes'
//     volumes faded, ducking), submixes with effects (reverb, EQ, compressor) and the master's limiter; Audio Volumes'
//     reverb by area (audio_world.c)
//   * every voice costs nothing when silent; the mixer's ms per block in audio_stats (the Debug Viewer, the tests)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// --- Sound Wave (.rsound): a 256-byte header then the samples (interleaved int16) ---
#define RSND_MAGIC 0x444E5352u // "RSND"
#define RSND_VERSION 1u
enum { SND_LOOPING = 1u, SND_STREAM = 2u, SND_UI = 4u /* plays while the game is paused (a menu's) */ };
#define SND_WAVE_POINTS 128
typedef struct SoundWaveHeader {
    uint32_t magic, version;
    uint16_t channels, format;     // 1 or 2; 0: PCM 16
    uint32_t sample_rate, frames;  // frames per channel
    uint32_t loop_start, loop_end; // the looped part (frames; loop_end 0: the end)
    uint32_t flags;                // SND_*
    float volume, pitch, priority; // Unreal's Volume, Pitch, Priority
    uint32_t sound_class, attenuation, concurrency; // their assets' hashes (0: the defaults)
    float peak, rms;               // the samples' (0-1)
    uint32_t data_offset, data_bytes;
    uint8_t wave[SND_WAVE_POINTS]; // the outline (thumbnails, the editor): the peak of each 1/128 of it, 0-255
    uint32_t reserved[14];
} SoundWaveHeader;
_Static_assert(sizeof(SoundWaveHeader) == 256, "SoundWaveHeader");

typedef struct SoundWave {
    char name[64], path[512];      // path: on disk
    uint32_t hash;                 // of its vpath (the components and the nodes find it by this)
    SoundWaveHeader h;             // (its settings: edited, then audio_wave_save)
    const int16_t* pcm;            // the samples, mapped
    void *file, *mapping;          // (the OS handles)
    const void* view;
    double length;                 // seconds
    bool dirty;
    const struct MsAsset* ms;      // a MetaSound's (metasound.h): its graph renders the voice, pcm isn't read (NULL: a wave)
} SoundWave;

// The hash the assets are found by (the Blueprints', the animations')
uint32_t audio_vpath_hash(const char* vpath);
// Writes a Sound Wave from samples (the importer, the tests): interleaved int16, its settings' defaults; the outline,
// peak and loudness measured. stream: long sounds (music): prefetched while they play
bool audio_wave_write(const char* path, const int16_t* pcm, uint32_t frames, uint32_t channels, uint32_t rate, bool stream);
SoundWave* audio_wave(uint32_t hash);           // mapped on its first use (NULL: none)
SoundWave* audio_wave_load(const char* vpath);
bool audio_wave_save(SoundWave* w);             // its settings (the header) written back
uint32_t audio_wave_count(void);
SoundWave* audio_wave_at(uint32_t i);
void audio_rescan(void);                        // new files in Content (an import): found on the next lookup
uint32_t audio_sound_by_name(const char* name);  // a Sound Wave's or Sound Cue's hash by its name (the Sequencer's keys)
const char* audio_content_root(void);

// --- The device and the mixer ---
typedef struct AudioStats {
    double mix_ms;        // the mixer's mean time per block (its thread)
    double mix_ms_max;    // its worst over the last second
    double period_ms;     // the device's period (the time a block has)
    uint32_t voices, real, virt, rate;
    float peak[8];        // the master's per channel (0-1, after the limiter), decaying
    uint32_t channels;    // the output's: 2 stereo, 6 (5.1), 8 (7.1)...
    char layout[24];      // "Stereo", "5.1", "7.1"...
    float limiter_db;     // the limiter's gain reduction now
    bool silent;          // the silent device (nothing is heard)
    char device[96];
} AudioStats;
bool audio_init(const char* content_root);      // false: no mixer (everything still answers, nothing plays)
void audio_set_content_root(const char* content_root); // another project
void audio_shutdown(void);
void audio_stats(AudioStats* out);
void audio_set_master_volume(float v);           // 0-1 (Project Settings, the game's options)
float audio_master_volume(void);
// The player's volume of an engine Sound Class (the options' Music, SFX, Voice: AUDIO_CLASS_*), 0-1 over the class's own
enum { AUDIO_CLASS_MASTER = 0, AUDIO_CLASS_MUSIC, AUDIO_CLASS_SFX, AUDIO_CLASS_VOICE, AUDIO_CLASS_UI, AUDIO_CLASS_AMBIENT };
void audio_set_user_class_volume(int which, float v);
float audio_user_class_volume(int which);
// The mixer's own frames for a test (the silent device only: n frames mixed now on this thread into out, interleaved
// in the output's channels: audio_stats' channels; REALENGINE_AUDIO_CHANNELS=6 / 8 makes the silent device 5.1 / 7.1)
void audio_render_test(float* out, uint32_t frames);

// --- Voices (Unreal's Active Sounds) ---
typedef uint32_t AudioHandle; // 0: none
typedef struct AudioPlay {
    uint32_t sound;           // a Sound Wave's or a Sound Cue's hash
    float volume, pitch;      // multipliers (0: 1)
    float start_time;         // seconds into it
    float fade_in;            // seconds (0: at once)
    bool spatial;             // 3D: at pos (or following its entity); else 2D (UI, music)
    float pos[3];             // cm
    uint32_t follow;          // an entity it moves with (0: none; audio_world.c)
    int bone;                 // its bone (a Skeletal Mesh's; -1: the entity itself)
    uint32_t attenuation;     // an override (0: the sound's)
    uint32_t sound_class;     // an override (0: the sound's)
    uint32_t concurrency;     // an override (0: the sound's)
    uint32_t owner;           // the actor it belongs to (Stop / concurrency per owner)
    bool ui;                  // plays while the game is paused
    bool preview;             // the editor's (not the game's: never paused, never stopped with the play)
} AudioPlay;
AudioHandle audio_play(const AudioPlay* p);
AudioHandle audio_play_2d(uint32_t sound, float volume, float pitch);
AudioHandle audio_play_at(uint32_t sound, const float pos[3], float volume, float pitch);
void audio_stop(AudioHandle h, float fade_out);  // seconds (0: at once)
void audio_fade(AudioHandle h, float seconds, float to); // Unreal's Adjust Volume: the volume faded to "to"
void audio_set_paused(AudioHandle h, bool paused);
bool audio_is_playing(AudioHandle h);            // (paused counts as playing, as in Unreal)
void audio_set_volume(AudioHandle h, float v);
void audio_set_pitch(AudioHandle h, float p);
void audio_set_pos(AudioHandle h, const float pos[3]);
// A MetaSound's input (an engine's "RPM", the rain's "Intensity") while it plays: Unreal's Set Float Parameter. False:
// not a MetaSound or no such input
bool audio_set_param(AudioHandle h, const char* name, float value);
float audio_playback_time(AudioHandle h);        // seconds into its sound
void audio_stop_owner(uint32_t owner, float fade_out);
void audio_stop_all(bool keep_previews);         // Play's end
void audio_pause_game(bool paused);
void audio_device_suspend(bool suspended);       // the app in the background: the device let go (Android's stream closed)              // every non-UI sound (the game's pause)

// The listener (the player's camera; the editor's view): cm, unit axes, cm/s
void audio_set_listener(const float pos[3], const float fwd[3], const float up[3], const float vel[3]);
// Called once a frame on the game thread: the voices' parameters (spatialization, classes, mixes, concurrency,
// virtualization), the finished ones, the streaming's prefetch. dt: seconds
void audio_update(float dt);
// The finished voices since the last call (On Audio Finished): handles written to out
uint32_t audio_finished(AudioHandle* out, uint32_t cap);

// --- Hooks the world sets (audio_world.c): an entity's position and velocity, a ray's occlusion ---
typedef struct AudioWorldHooks {
    bool (*entity_pos)(uint32_t entity, int bone, float pos[3]); // false: gone (the sound stops)
    float (*occlusion)(const float from[3], const float to[3]);  // 0 clear .. 1 blocked
    void (*zone)(const float src[3], float* gain, float* lpf);   // Audio Volumes' inside / outside levels for a sound there
} AudioWorldHooks;
void audio_set_world_hooks(const AudioWorldHooks* h);

// The sounds playing now (the Debug Viewer): their sound, how loud they are heard, their distance, real or virtual
typedef struct AudioInfo { uint32_t sound, owner; float audible, dist; bool virt, paused; } AudioInfo;
uint32_t audio_list(AudioInfo* out, uint32_t cap);
