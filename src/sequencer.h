// Sequencer (sugestão #45, notas-dev/sequencer.md): Unreal's Level Sequence — a cinematic as tracks of keys on a
// timeline, played in the editor (preview, scrubbing), in Play in Editor and in the packaged game.
//   - a Level Sequence asset (.rseq, JSON) has its length and tracks; a track is bound to an actor of the level by its
//     name (Unreal's binding: the Outliner's name) or is a master track (Camera Cuts, Events);
//   - tracks: Transform (location, rotation, scale; cubic between keys), Visibility (Hidden In Game), Light Intensity,
//     Field Of View (a camera's), Animation (an Animation Sequence played on a Skeletal Mesh from the key), Camera
//     Cuts (which camera actor the view is from) and Events (a custom event called on the bound actor's Blueprint, or
//     on every Blueprint actor that has it);
//   - players: Play Sequence / Stop Sequence / Is Sequence Playing (Blueprint nodes), up to SEQ_PLAYERS at once;
//     evaluated after the Blueprints each frame; their Camera Cuts give the view (bp_player_camera) while they play.
// No allocation while playing: the keys are read where they are (sorted by time), the bindings resolved at Play.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define SEQ_MAX_TRACKS 48
#define SEQ_NAME 64
#define SEQ_PLAYERS 4

typedef enum SeqTrackType {
    SEQT_TRANSFORM = 0, SEQT_VISIBILITY, SEQT_LIGHT_INTENSITY, SEQT_FOV, SEQT_ANIMATION, SEQT_CAMERA_CUTS, SEQT_EVENT,
    SEQT_AUDIO, // a sound (the key's name: a Sound Wave or Cue; v[0] volume, v[1] pitch) at the bound actor, else 2D
    SEQT_FADE,  // (B9) a master track: the picture faded to a color (v[0] 0..1, linear between keys; v[1..3] the color)
    SEQT_HORROR_FILTER, // (Q13) a master track: a horror filter (the key's name: "Hallucination", "VHS"..., or "Letterbox") at v[0] 0..1
    SEQT_COUNT
} SeqTrackType;

// (2026-11-10, "faz o sequencer igual o da Unreal") a key's interpolation toward the next one, Unreal's: Auto (cubic,
// the tangents from the neighbors), User (cubic, its own tangents: the Curve Editor's handles), Linear, Constant (holds
// until the next key). Visibility, Animation, Camera Cuts, Events and Audio are stepped whatever it says
typedef enum SeqInterp { SEQI_AUTO = 0, SEQI_USER, SEQI_LINEAR, SEQI_CONSTANT, SEQI_COUNT } SeqInterp;

typedef struct SeqKey {
    float t;             // seconds from the start
    float v[9];          // Transform: location, rotation (pitch, yaw, roll), scale; the others: v[0]
    char name[SEQ_NAME]; // Animation: the sequence; Camera Cuts: the camera actor; Event: the event
    uint8_t interp;      // SeqInterp (toward the next key)
    float tan[9];        // SEQI_USER: each channel's slope (units per second), arriving and leaving alike
} SeqKey;

typedef struct SeqTrack {
    uint8_t type;            // SeqTrackType
    bool loop_anim;          // Animation: the clip loops
    char actor[SEQ_NAME];    // the bound actor's name ("" for Camera Cuts and the Events of every actor)
    SeqKey* keys;            // by time
    uint32_t nkeys, cap;
} SeqTrack;

typedef struct Sequence {
    char path[520];          // the file
    char vpath[300];         // "/Content/..."
    char name[SEQ_NAME];
    float length;            // s (Unreal's playback range end)
    float start;             // s (the playback range's start: the players and the preview begin there; 0 in v1 files)
    float fps;               // the timeline's frame rate (keys snap to its frames in the editor)
    SeqTrack tracks[SEQ_MAX_TRACKS];
    uint32_t ntracks;
} Sequence;

// The project's Content folder (the editor's, the game's)
void seq_init(const char* content_root);
// A sequence by its "/Content/....rseq" path or its name (cached; NULL: none)
Sequence* seq_load(const char* vpath_or_name);
Sequence* seq_create(const char* folder_vpath, const char* name); // a new one under the folder (unique name), saved
bool seq_save(Sequence* s);
void seq_reload_all(void);                       // the files changed (a level opened, a project switched)
uint32_t seq_generation(void);                   // counts the reloads (a Sequence* kept across one is gone)
uint32_t seq_list(char (*out)[300], uint32_t cap); // every .rseq under Content ("/Content/..." paths)

// Editing (the Sequencer window): tracks and keys
int seq_add_track(Sequence* s, SeqTrackType type, const char* actor); // its index (-1: full)
void seq_remove_track(Sequence* s, int track);
int seq_add_key(SeqTrack* tr, const SeqKey* k);  // inserted in time order (a key at the same time replaced); its index
void seq_remove_key(SeqTrack* tr, int key);
void seq_sort_keys(SeqTrack* tr);                 // after a key's time was dragged
const char* seq_track_type_name(int type);        // English (TR it)
const char* seq_interp_name(int interp);          // "Auto", "User", "Linear", "Constant" (TR it)
int seq_track_channels(int type);                 // the curves a track's keys hold (Transform 9, Light / FOV / Fade / Horror Filter 1, the rest 0)
// A track's channel c at time t (the Curve Editor's curves and the evaluation: the keys' interpolations)
float seq_channel_at(const SeqTrack* tr, int c, float t);

// Evaluation at a time into the world (the editor's preview, the players). events: fire the Event keys crossed since
// from (exclusive) to t (inclusive); anim: start the Animation keys crossed the same way.
void seq_evaluate(World* w, const Sequence* s, float from, float t, bool events);
// The camera the Camera Cuts give at t (ENTITY_NONE: none)
Entity seq_camera_at(World* w, const Sequence* s, float t);
Entity seq_find_actor(World* w, const char* name);

// Players (the Blueprints' Play Sequence; the editor's PIE and the game)
bool seq_play(World* w, const char* vpath_or_name, bool loop, float rate);
void seq_stop(World* w, const char* vpath_or_name); // NULL: all
bool seq_is_playing(const char* vpath_or_name);
void seq_tick(World* w, float dt);  // after the Blueprints (bp_tick calls it)
void seq_end_play(void);            // every player stopped (play ends)
// The view while a playing sequence's Camera Cuts give one (bp_player_camera): position, yaw, pitch (degrees), FOV
bool seq_view(World* w, float pos[3], float* yaw, float* pitch, float* fov_deg);

typedef struct SeqStats { uint32_t playing, sequences, events_fired, anim_restored; double tick_ms; } SeqStats; // (Q23 anim_restored: actors given back to their Anim BP)
const SeqStats* seq_stats(void);
