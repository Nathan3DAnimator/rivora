// Async: the editor never waits for long work (notas-dev/async.md). A task is
//   - work on a background thread (async_run: a fixed pool of low priority workers; the result applied on the main
//     thread by its done callback, at the start of a frame), or
//   - steps on the main thread (async_steps: the GPU's or the world's work, a slice per frame within a budget), or
//   - another system's own progress shown the same way (async_track: the lighting bake).
// Each one reports what it is doing (async_status) and how far it got (async_progress); the editor shows them as cards
// at the bottom right (async_ui.c). Nothing here touches the world, the GPU or a registry from a worker: the work
// writes files or private memory, the done callback (main thread) puts the result in place.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct AsyncTask AsyncTask;
typedef void (*AsyncWorkFn)(AsyncTask* t, void* arg);           // a worker thread
typedef bool (*AsyncStepFn)(AsyncTask* t, void* arg);           // the main thread, each frame: true when finished
typedef void (*AsyncDoneFn)(AsyncTask* t, void* arg, bool ok);  // the main thread, once (ok: not failed nor cancelled)
typedef float (*AsyncPollFn)(void* arg, char* status, int cap);  // async_track: the progress (< 0: finished)

enum {
    ASYNC_CANCELLABLE = 1u << 0, // the card has a Cancel (async_cancelled turns true; the work stops when it checks)
    ASYNC_QUIET = 1u << 1,       // no card unless it takes long (> 0.4 s); the rest show after 0.15 s (no flicker)
    ASYNC_HIDDEN = 1u << 2,      // never a card (the parts of a bigger task)
};

enum { ASYNC_QUEUED, ASYNC_RUNNING, ASYNC_DONE, ASYNC_FAILED, ASYNC_CANCELLED };

void async_init(void);     // the workers (2, or 1 on a 2-core machine), low priority
bool async_ready(void);    // initialized with workers (the game without them: the callers do their work at once)
void async_shutdown(void); // waits for the running work; the queued is dropped (its done called with ok = false)

// A task's id (0: no room: the work ran right away, on the caller). label: what it is ("Importing Bonnie.tlend",
// translated by the caller); the arg is the task's (freed by its done when it owns memory).
uint32_t async_run(const char* label, AsyncWorkFn work, AsyncDoneFn done, void* arg, uint32_t flags);
uint32_t async_steps(const char* label, AsyncStepFn step, AsyncDoneFn done, void* arg, uint32_t flags);
uint32_t async_track(const char* label, AsyncPollFn poll, void (*cancel)(void* arg), void* arg, uint32_t flags);

// Inside a task (any thread): what it does now, how far (0..1; < 0: unknown, the bar slides), stop when cancelled
void async_status(AsyncTask* t, const char* fmt, ...);
void async_progress(AsyncTask* t, float p);
void async_progress_n(AsyncTask* t, int done, int total); // progress done / total, "done / total" in the card
bool async_cancelled(const AsyncTask* t);
void async_fail(AsyncTask* t, const char* fmt, ...); // the card turns red with the reason
AsyncTask* async_current(void);                      // the task this thread works on (NULL: none): deep code reports

// The main thread, at the start of each frame: the finished work's done callbacks, then the step tasks within
// budget_ms (each runs at least one step a frame)
void async_update(double budget_ms);
int async_busy(void);             // tasks not finished (queued, running or steps)
bool async_idle(void);            // nothing queued, running nor waiting for its done
void async_wait_all(double max_s); // the tests and automated runs: until idle (the done callbacks run here)
void async_cancel(uint32_t id);
bool async_running(uint32_t id); // not finished yet (0: false)

// The cards' view of the tasks (async_ui.c): the visible ones, oldest first
typedef struct AsyncView {
    uint32_t id, flags;
    uint8_t state;
    float progress;   // < 0: unknown
    int done, total;  // async_progress_n (total 0: none)
    double t_start, t_end; // re_time_seconds (t_end 0: running)
    char label[96], status[160];
} AsyncView;
int async_snapshot(AsyncView* out, int cap, double now);
void async_dismiss(uint32_t id); // a finished card closed by the user

// The measures (the test, the Debug Viewer): the longest main-thread slice of the step tasks and done callbacks
typedef struct AsyncStats { uint32_t started, finished, failed, cancelled; double main_ms_max, main_ms_last; } AsyncStats;
AsyncStats async_stats(void);
