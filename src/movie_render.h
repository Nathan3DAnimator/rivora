// Cinematics (B9, notas-dev/sequencer.md "Movie Render Queue e Take Recorder"):
//   - Movie Render Queue: a Level Sequence rendered to PNG frames offline in Play in Editor - its clock fixed (1 / (fps x
//     temporal samples) a frame, whatever the GPU takes), each output frame the mean of its temporal samples (motion
//     blur and anti-aliasing as Unreal's temporal sub-sampling), warm-up frames first (the GI and the caches settle);
//   - Take Recorder: while playing, the chosen actors' transforms (the player's pawn and the selection) recorded into a
//     new Level Sequence (Content/Cinematics/Takes/Take_N), one key a frame at the take's rate.
// Window > Cinematics opens both. --cine-test: a falling cube recorded, then its take rendered.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct Editor;

typedef struct MovieJob {
    char sequence[300];  // its name or "/Content/....rseq"
    char out_dir[700];   // the frames' folder ("": <project>/Saved/MovieRenders/<sequence>)
    int fps;             // output frames a second (24)
    int samples;         // temporal samples a frame (1 = none; 8: smooth motion blur)
    int warmup;          // frames rendered before the first (the GI, the streaming)
    int max_frames;      // 0: the whole sequence
} MovieJob;
typedef struct MovieStats {
    bool active, done, ok;
    uint32_t frames, written, samples_grabbed;
    double ms;           // the render's wall time
    char out_dir[700], status[200];
} MovieStats;

bool movie_render_start(struct Editor* ed, const MovieJob* job);
void movie_render_cancel(struct Editor* ed);
// Every frame before editor_frame: the last frame's grab taken in, the next asked for; the fixed step to simulate (0: the
// real clock: not rendering)
double movie_render_tick(struct Editor* ed, double now);
const MovieStats* movie_render_stats(void);

// Take Recorder (while playing)
bool take_record_start(struct Editor* ed, int fps); // the player's pawn and the selected actors
bool take_record_stop(struct Editor* ed);          // the take saved (its name: take_last())
bool take_recording(void);
void take_record_tick(struct Editor* ed, double dt); // every frame while playing
const char* take_last(void);                         // the last take's "/Content/....rseq" ("": none)

void cinematics_window(struct Editor* ed, bool* open); // Window > Cinematics: the Movie Render Queue and the Take Recorder

// --cine-test
bool cine_test_setup(struct Editor* ed, const char* out_dir);
void cine_test_tick(struct Editor* ed, double dt);
bool cine_test_check(struct Editor* ed);
