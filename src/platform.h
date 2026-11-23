// OS services (Windows): UTF-8 files, native file dialogs, threads, atomics and the clock.
// Copied from the Tlender (src/platform.h) and adapted: re_ prefix, no pen/touch hook, no scene-kind dialogs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct GLFWwindow;

// fopen() that accepts UTF-8 paths.
FILE* re_fopen(const char* path_utf8, const char* mode);
// Atomically moves src over dst (replacing it). UTF-8 paths.
bool re_replace_file(const char* src_utf8, const char* dst_utf8);
void re_remove_file(const char* path_utf8);
// Calls fn with the name (UTF-8, no folder) of every regular file in a folder; false when the folder cannot be read.
bool re_list_dir(const char* dir_utf8, void (*fn)(void* user, const char* name), void* user);
// The command line as UTF-8 (Windows hands main() its arguments in the ANSI code page). The strings live until the
// process ends.
char** re_utf8_argv(int argc, char** argv);
// Per-user settings folder (%APPDATA%\RealEngine), created when missing. UTF-8.
bool re_config_dir(char* out, size_t cap);
bool re_home_dir(char* out, size_t cap); // the user's folder (%USERPROFILE%), UTF-8
// Per-user cache folder (%LOCALAPPDATA%/RealEngine: machine data, never roamed), created when missing. UTF-8.
bool re_cache_dir(char* out, size_t cap);
// The Windows interface language is Portuguese (the editor's default language when there are no preferences yet)
bool re_system_language_portuguese(void);
// The color on the screen at (x, y), screen pixels: r | g << 8 | b << 16 (the eyedropper). False when unavailable.
bool re_screen_pixel(int x, int y, unsigned* rgb);
// REALENGINE_MONITOR=N (the visible tests: Windows' display N): its work area. False: unset or no such display
bool re_test_monitor(int* x, int* y, int* w, int* h);

// Synthetic input (automated runs): the native dialogs answer "cancelled" without showing.
extern bool g_platform_synthetic_input;

// Shows the native Open/Save dialog for a list of extensions without dots ("rasset;rlevel"; the first is added when
// saving without one), shown under `label` (already translated). path holds the initial and the chosen path.
bool platform_file_dialog_ext(struct GLFWwindow* parent, bool save, const char* label, const char* extensions,
                              char* path, size_t cap);
// Open dialog that picks several files: their full paths in out (UTF-8). Returns the count, 0 when cancelled.
int platform_file_dialog_multi(struct GLFWwindow* parent, const char* label, const char* extensions, char (*out)[1024],
                               int cap);
// The native folder picker: path holds the start (a file or folder; the nearest folder that exists is opened) and the
// chosen folder, with a separator at its end.
bool platform_folder_dialog(struct GLFWwindow* parent, const char* title, char* path, size_t cap);
// The longest leading part of path that is an existing folder (UTF-8), without the separator at its end; "" if none.
void platform_existing_folder(const char* path, char* out, size_t cap);

// Threads
typedef struct ReThread ReThread;
ReThread* re_thread_start(int (*fn)(void* arg), void* arg);
void re_thread_join(ReThread* t); // waits and frees
int re_cpu_count(void);
double re_free_memory_gb(void); // physical memory free now (0 when unknown)
double re_time_seconds(void);
uint32_t re_thread_id(void);
// The calling thread below normal priority (work nobody waits for) or back to normal.
void re_thread_set_low_priority(bool low);

// Atomics (sequentially consistent)
int32_t re_atomic_add(volatile int32_t* p, int32_t v); // returns the new value
int32_t re_atomic_load(volatile int32_t* p);
void re_atomic_store(volatile int32_t* p, int32_t v);
bool re_atomic_cas(volatile int32_t* p, int32_t expected, int32_t desired);
// Sets the bits of v in *p; returns the old value.
uint64_t re_atomic_or64(volatile uint64_t* p, uint64_t v);
// (2026-11-19) A gate: threads sleep on a count until it changes (re_gate_bump adds one and wakes them all)
typedef struct ReGate ReGate;
ReGate* re_gate_create(void);
int32_t re_gate_wait(ReGate* g, int32_t seen); // returns once the count is not seen: the count
void re_gate_bump(ReGate* g);
void re_yield(void);
void re_cpu_relax(void); // a spin-wait hint (pause)
// Parallel loop on worker threads kept between calls (one per core but the caller's): fn(arg, i) for every i in
// 0..count-1, in any order, the caller taking items too; returns when all are done. Calls from inside fn, or while
// another thread runs one, run serially on the caller. (The job system with dependencies builds on this later.)
void re_parallel_for(int count, void (*fn)(void* arg, int index), void* arg);
// (B14b) set while a trace records (trace.c): each thread's share of a parallel_for, its span and items
extern void (*volatile g_re_pool_span)(double t0, double t1, int items);
int re_pool_threads(void); // workers + the caller (1: serial)
// The touch screen's fingers now (Android; the PC: none): their ids and positions (window pixels); the count
int platform_touch_points(int* ids, float (*xy)[2], int cap);
// Sleeps about s seconds (sub-millisecond precision: frame rate limit).
void re_sleep_seconds(double s);
// Ends the process at once: no atexit handlers, no library shutdown.
void re_terminate_process(int code);
// (B13) another process (a command line, as typed: the program's path first, quoted), and its end (the editor's Play
// with several players: their windows). NULL: not started
typedef struct ReProcess ReProcess;
ReProcess* re_process_start(const char* cmdline);
void re_process_kill(ReProcess* p); // ended and freed (NULL: nothing)
// (B14) a command line run to its end without a window, its output (stdout and stderr) into out (NUL-terminated, cut
// at cap) and its exit code. False: it couldn't start
bool re_run_capture(const char* cmdline, char* out, size_t cap, int* exit_code);
