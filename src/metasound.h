// MetaSounds (B8, notas-dev/audio.md "MetaSounds"): a sound made by a graph of DSP nodes instead of recorded samples,
// as Unreal's MetaSound Source. The asset (.rmsnd, JSON) is a list of nodes: oscillators, noise, impulses (drops,
// crackle), filters, envelopes, math, inputs (named parameters the game sets while it plays: an engine's RPM, the
// rain's intensity) and the Output. Loaded, the graph is sorted once into a flat program; the mixer thread runs it a
// block at a time into the voice's samples (no allocation, a state per voice slot), then the voice goes through the
// same attenuation, spatialization, filters, classes and submixes as a Sound Wave.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "audio.h"

#define MS_MAX_NODES 32
#define MS_MAX_IN 6
#define MS_MAX_PARAMS 8
#define MS_NAME 32

enum {
    MSN_OUTPUT = 0, // in: Signal, Volume
    MSN_INPUT,      // a graph input (a parameter): name, default; its value, smoothed over a block
    MSN_OSC,        // in: Frequency (Hz), Amplitude, Offset, Pulse Width; mode: 0 sine, 1 saw, 2 square, 3 triangle
    MSN_NOISE,      // in: Amplitude; mode: 0 white, 1 pink, 2 brown
    MSN_IMPULSES,   // in: Rate (a second), Amplitude, Randomness: clicks at random times with random heights (drops, crackle)
    MSN_FILTER,     // in: Signal, Cutoff (Hz), Resonance (Q); mode: 0 low pass, 1 high pass, 2 band pass (a biquad)
    MSN_ADSR,       // in: Gate (>= 0.5 on), Attack, Decay, Sustain, Release (s); 0..1
    MSN_MUL,        // in: A, B
    MSN_ADD,        // in: A, B
    MSN_MAP,        // in: Value, In Min, In Max, Out Min, Out Max (clamped)
    MSN_SMOOTH,     // in: Value, Time (s): a one-pole glide
    MSN_DISTORT,    // in: Signal, Drive: tanh
    MSN_KIND_COUNT
};
extern const char* const g_ms_kind_names[MSN_KIND_COUNT];
extern const char* const* const g_ms_in_names[MSN_KIND_COUNT]; // each kind's inputs (NULL-terminated)
extern const char* const* const g_ms_mode_names[MSN_KIND_COUNT]; // its modes (NULL: none)

typedef struct MsIn { int8_t node; float value; } MsIn; // node >= 0: that node's output; -1: the value
typedef struct MsNode {
    uint8_t kind, mode, nin, pad;
    MsIn in[MS_MAX_IN];
    char name[MS_NAME]; // an input's
    float x, y;         // (the editor)
} MsNode;

typedef struct MsAsset {
    char name[64], path[512];
    uint32_t hash;
    uint32_t nnodes;         // [0]: the Output
    MsNode n[MS_MAX_NODES];
    uint8_t order[MS_MAX_NODES], norder; // the program: the nodes in the order they run (each after its inputs)
    uint8_t param_node[MS_MAX_PARAMS], nparams; // the inputs (parameters) in order
    float volume;
    uint32_t sound_class, attenuation;
    char error[96];          // a cycle, a bad input ("": compiled)
    SoundWave wave;          // what the mixer plays (ms set: rendered, not read)
} MsAsset;

// The per-voice state the mixer keeps (one per voice slot): the oscillators' phases, the filters' memories, the envelopes
typedef struct MsNodeState { double phase; float z1, z2, b0, b1, b2, a1, a2, env, y; uint32_t rng; uint8_t stage, gate; } MsNodeState;
typedef struct MsState { const MsAsset* a; MsNodeState s[MS_MAX_NODES]; float pv[MS_MAX_PARAMS]; } MsState;

void ms_init(const char* content_root);                 // the project's MetaSounds (Content/**/*.rmsnd) and the engine's made when missing
const MsAsset* ms_find(uint32_t hash);                  // NULL: not a MetaSound
MsAsset* ms_find_edit(uint32_t hash);
uint32_t ms_count(void);
MsAsset* ms_at(uint32_t i);
uint32_t ms_by_name(const char* name);                  // its hash (0: none)
bool ms_compile(MsAsset* a);                            // the program from the nodes (false: a.error says why)
bool ms_save(MsAsset* a);
MsAsset* ms_create(const char* vpath);                  // a new one (an oscillator into the output), written
int ms_param_index(const MsAsset* a, const char* name); // -1: none
void ms_node_defaults(MsNode* n, int kind);

// The mixer thread (audio.c): a voice's state set up when it starts; a block of n samples at rate (mono, -1..1) into
// out with its parameters' values; pitch scales every oscillator's frequency
void ms_reset(MsState* st, const MsAsset* a);
void ms_render(MsState* st, const float* params, float* out, uint32_t n, float rate, float pitch);
// The engine's own (Content/Audio/MetaSounds): an engine (RPM, Load), the rain (Intensity), the wind (Speed)
uint32_t ms_engine_sound(void);
uint32_t ms_rain_sound(void);
uint32_t ms_rain_styled_sound(void); // (2026-11-14) the rain with its style's inputs (Tone, Drop Pitch, Drop Rate, Drop Level, Muffle, Volume)
uint32_t ms_thunder_sound(void);      // thunder: Rumble (0..1, driven), Volume
uint32_t ms_wind_sound(void);
