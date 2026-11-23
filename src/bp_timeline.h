// Timelines (2026-11-16, "vamos adicionar também uma timeline aos blueprints (o node de timeline que a Unreal tem)";
// notas-dev/blueprints.md). As in Unreal, a Blueprint class owns its Timelines (a name, a length, Loop, AutoPlay, Use
// Last Keyframe, tracks of keys); the Timeline node names one: Play, Play from Start, Stop, Reverse, Reverse from End,
// Set New Time (New Time) in; Update (each frame it plays), Finished, Direction and one output per track out (a Float, a
// Vector, a Linear Color or an Event track's exec). The VM keeps each instance's time (bp_vm.c: OP_TIMELINE, the tick).
// The curves are evaluated without allocation: keys in order, cubic (auto tangents), linear or constant between them.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "blueprint.h"

#define BP_TL_MAX 8      // timelines per class
#define BP_TL_TRACKS 6   // tracks per timeline (the node's pins)
#define BP_TL_KEYS 64    // keys per track
enum { TLT_FLOAT, TLT_VECTOR, TLT_COLOR, TLT_EVENT, TLT_COUNT };
enum { TLI_AUTO, TLI_LINEAR, TLI_CONSTANT, TLI_COUNT }; // a key's interpolation to the next (Unreal: Auto, Linear, Constant)

typedef struct BpTlKey { float t, v[4]; uint8_t interp; } BpTlKey;
typedef struct BpTlTrack {
    char name[BP_NAME];
    uint8_t type, nkeys;
    BpTlKey keys[BP_TL_KEYS]; // by time
} BpTlTrack;
typedef struct BpTimeline {
    char name[BP_NAME];
    float length;        // seconds
    bool loop, autoplay; // AutoPlay: plays from its start when the actor begins play
    bool use_last_key;   // the length is the last key's time
    float rate;          // Play Rate (1: real time)
    uint8_t ntracks;
    BpTlTrack tracks[BP_TL_TRACKS];
} BpTimeline;
typedef struct BpTimelines { uint32_t n; BpTimeline t[BP_TL_MAX]; } BpTimelines;

extern const char* const g_tl_track_type_names[TLT_COUNT];
extern const char* const g_tl_interp_names[TLI_COUNT];

BpTimeline* bp_timeline_find(BpAsset* a, const char* name); // its own or a parent class's
BpTimeline* bp_timeline_add(BpAsset* a);                    // "Timeline_N" with a Float track "Alpha" 0 -> 1 over 1 s
bool bp_timeline_remove(BpAsset* a, const char* name);
BpTlTrack* bp_timeline_add_track(BpTimeline* tl, uint8_t type);
float bp_timeline_length(const BpTimeline* tl);
// A track's value at t (Float: v[0]; Vector: v[0..2]; Color: v[0..3]); an Event track: nothing
void bp_timeline_eval(const BpTlTrack* tr, float t, float out[4]);
int bp_timeline_add_key(BpTlTrack* tr, float t, const float v[4]); // kept in order; its index (-1: full)
void bp_timeline_sort(BpTlTrack* tr);

// The .rbp's "timelines" (blueprint.c bp_save / the load)
struct JsonWriter;
struct JsonValue;
void bp_timelines_write(struct JsonWriter* w, const BpAsset* a);
void bp_timelines_read(const struct JsonValue* root, BpAsset* a);
void bp_timelines_free(BpAsset* a);

// The Timeline editor (bp_editor.c: double-click a Timeline node): length, Loop, AutoPlay, the tracks, the keys on a
// curve (Shift+click adds one, drag moves it, Delete removes it, right-click its interpolation)
bool bp_timeline_editor(BpAsset* a, char* name, bool* open); // true: changed (the node's pins refreshed by the caller)

// tests
int bp_timeline_selftest(void); // 0: ok
