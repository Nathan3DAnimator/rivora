// Trace (B14b, notas-dev/debug-viewer.md "Trace"): the frames recorded into a file, as Unreal Insights. While recording,
// every frame leaves its CPU scopes (the game thread's phases: Blueprints, physics, animation, render recording...; the
// workers' parallel jobs) and the GPU's passes (their timestamps) in a buffer made when the recording starts (no malloc in
// the frame); stopping writes it, on a thread, as a Chrome trace (JSON: chrome://tracing, ui.perfetto.dev) that the
// editor's Trace window opens too (Tools > Trace: the frames, a timeline with zoom, the timers' totals).
// Off, a scope costs one bool read.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

extern volatile bool g_trace_on;

// The recording: into path (a .json; its folder made). False: already recording, or no memory
bool trace_start(const char* path);
// Stops and writes the file on a thread (trace_writing while it runs). The path written ("": none)
const char* trace_stop(void);
bool trace_active(void);
bool trace_writing(void);
uint32_t trace_events(void);       // recorded so far (the buffer's room: TRACE_MAX_EVENTS; full: the rest is dropped)
double trace_seconds(void);        // since the start
#define TRACE_MAX_EVENTS (1u << 21)

// A scope: t0 = trace_t0() before, trace_scope(name, t0) after (name: a string that lives as long as the program)
static inline double trace_t0(void) { return g_trace_on ? re_time_seconds() : 0.0; }
void trace_scope(const char* name, double t0);
#define TRACE(name, stmt) do { double trace_t0_ = trace_t0(); stmt; trace_scope(name, trace_t0_); } while (0)
// A span already measured (t0..t1, seconds of re_time_seconds) on this thread
void trace_span(const char* name, double t0, double t1);
// A value over time (a counter track: FPS, memory...)
void trace_counter(const char* name, double value);
// The frame's end (the game thread): the frame t0..t1 and the GPU's passes of the frame before last laid on the GPU track
// from t0 (gpu_names[i] lasted gpu_ms[i]), the frame rate as a counter
void trace_frame(double t0, double t1, const char* const* gpu_names, const double* gpu_ms, uint32_t ngpu);
// This thread's name on its track (the game thread names itself on the first event; the workers: "Worker N")
void trace_name_thread(const char* name);

// A file read back (the Trace window; the tests): every event of a track, in order
typedef struct TraceEv { double ts, dur; uint32_t name; uint16_t track; uint8_t depth, counter; float value; } TraceEv;
typedef struct TraceFile {
    TraceEv* ev; uint32_t n;
    char (*names)[48]; uint32_t nnames;
    char tracks[32][40]; uint32_t ntracks; uint8_t track_depth[32];
    double* frames; uint32_t nframes;   // each frame's start (seconds), and its length in frame_ms
    float* frame_ms;
    double t_end;
} TraceFile;
bool trace_load(const char* path, TraceFile* f);
void trace_free(TraceFile* f);

// The editor's Trace window (trace_view.c): record, open, the frames, the timeline, the timers
void trace_window(bool* open, const char* save_dir);
void trace_window_load(const char* path);                    // a file opened in it (the next frame)
bool trace_window_loaded(uint32_t* events, uint32_t* frames); // (the tests) what it shows
// The CPU and GPU parts a frame recorded (render_stats' passes) as names and ms: the editor and the game's loops
uint32_t trace_gpu_passes(const char** names, double* ms, uint32_t cap);
// (--trace FILE) at exit: stops, waits for the file, reads it back and prints what it has. 1: unreadable or empty
int trace_finish_check(void);
