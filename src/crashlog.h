// Crash reports and the app's log (no UI, also in the tests).
//
// The log is a ring of the last lines (the status messages, warnings, Vulkan errors and, in the app, everything the
// process prints on stdout/stderr) and of the last user actions (shortcuts, undo steps, operators). A crash report
// writes them with the system, the app's state and the stacks of every thread (symbolized with the .pdb next to the
// exe) to <exe folder>/crashes/RealEngine_crash_<YYYY-MM-DD_HH-MM-SS>.txt (Markdown) plus a minidump with the same name;
// a name already there is never overwritten (_2, _3...). The bug report (bugreport.h) writes the same context.
//
// What ends in a report: unhandled exceptions (access violation, division by zero, C++ exceptions...), stack overflow
// and heap corruption (a vectored handler: they may never reach the unhandled filter), abort() / SIGABRT, the CRT's
// invalid parameter and pure call handlers, ImGui's IM_ASSERT (cimconfig.h), and fatal errors the code finds
// (crash_fatal: Vulkan device lost, out of memory). The report is written by a thread started at install (its own
// stack: the crashed thread may have none left), the other threads are suspended only while their stack is read.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRASH_LOG_LINES 256   // lines kept in the ring (the report shows the last 200)
#define CRASH_LOG_LINE_LEN 320
#define CRASH_ACTIONS 64      // actions kept (the report shows the last 50)
#define CRASH_ACTION_LEN 160

typedef enum ReLogLevel { RE_LOG_INFO = 0, RE_LOG_WARN, RE_LOG_ERROR } ReLogLevel;

// --- The log (thread safe; never allocates) ---
void re_log(ReLogLevel level, const char* fmt, ...);
void re_log_text(ReLogLevel level, const char* text, size_t len); // one line (len may be (size_t)-1)
// A user action: "atalho: Mover (G)", "desfazer: Bevel", "menu: Arquivo > Salvar".
void re_log_action(const char* fmt, ...);
// The last max lines / actions, oldest first, one per line ("HH:MM:SS.mmm [nível] texto"). Returns the length.
size_t re_log_dump(char* out, size_t cap, int max_lines);
size_t re_log_actions_dump(char* out, size_t cap, int max_actions);
int re_log_count(void); // lines written since the start (tests)
void re_log_clear(void); // tests

// Copies the process's stdout and stderr into the log (a pipe read by a thread); they still reach the console. The
// app calls it once at start (printf, fprintf(stderr), the CRT's assert message, GLFW and the validation layer).
void re_log_capture_std(void);
// The log in a file too (the app, a window app without a console): <reports folder>/logs/realengine.log, the previous
// run kept as realengine_anterior.log (another Real Engine open: realengine_<pid>.log). Every ring line is appended as written.
// Call after crash_install (the reports folder). False when it can't be created.
bool re_log_open_file(void);
const char* re_log_file_path(void); // "" without a file
// A console to watch the log (--console / REALENGINE_CONSOLE=1): the terminal that started the app, else a new one.
// Call before re_log_capture_std (the capture echoes to it).
bool re_console_open(void);

// --- Crash reports ---
typedef struct CrashConfig {
    const char* dir;  // folder that gets crashes/ (NULL: the exe's folder; %APPDATA%\RealEngine when it can't be written)
    bool dialog;      // a message box after writing (the app; off in tests)
    bool minidump;    // write the .dmp too
} CrashConfig;
void crash_install(const CrashConfig* cfg);
bool crash_installed(void);

// What the app adds to a report. Both run on the report thread with the rest of the process frozen or broken: no
// locks, little memory, guarded (a fault inside is caught and noted).
typedef struct CrashHooks {
    void (*state)(char* out, size_t cap, void* user);        // Markdown list of the app's state
    bool (*emergency_save)(const char* path, void* user);    // saves the scene (.rlevel) for recovery
    void* user;
} CrashHooks;
void crash_set_hooks(const CrashHooks* hooks);
// The message box after a crash on or off (off in --bench: a test never waits for a click)
void crash_set_dialog(bool on);
// The GPU in the report (from VkPhysicalDeviceProperties and the device-local heap).
void crash_set_gpu(const char* name, uint32_t vendor_id, uint32_t driver_version, uint32_t api_version, uint64_t vram);
// Called now and then from the main loop: puts the unhandled exception filter back if a library replaced it.
void crash_tick(void);
// The hang watchdog: the main loop calls crash_heartbeat once a pass (and the loading screen once a frame); stopped
// for `seconds`, where the main thread is goes to the log and to crashes/RealEngine_travamento_<data>.txt (it runs on).
void crash_watchdog_start(double seconds);
void crash_heartbeat(void);
const char* crash_last_hang_report(void); // the last report it wrote ("" none: the test)

// A fatal error found by the code: writes the report (kind = short title, the message after it) and ends the
// process. Without crash_install it prints and aborts.
#ifdef _MSC_VER
__declspec(noreturn)
#endif
void crash_fatal(const char* kind, const char* fmt, ...);
// IM_ASSERT (external/cimgui/cimconfig.h): a failed ImGui assert ends in a report with the expression and the line.
void re_imgui_assert_fail(const char* expr, const char* file, int line);

// The folder for reports ("crashes" or "bugReport") under the configured base, created when missing. UTF-8.
bool crash_reports_dir(const char* sub, char* out, size_t cap);
// "2026-10-24_15-30-05" (local time).
void crash_timestamp(char* out, size_t cap);
// Creates <dir>/<prefix>_<stamp><ext> exclusively (never replaces a file): when taken, <prefix>_<stamp>_2<ext>,
// _3... path gets the one created (empty file, closed). False when the folder can't be written.
bool crash_create_unique(const char* dir, const char* prefix, const char* stamp, const char* ext, char* path,
                         size_t cap);

// The report of the last crash, when the app crashed last time (a marker in crashes/); the marker is removed.
bool crash_take_pending(char* report_path, size_t cap);

// The context both reports share, as Markdown (program, system, memory, GPU, the app's state, the last actions and
// the log). Returns the length.
enum { CRASH_CTX_SYSTEM = 1, CRASH_CTX_STATE = 2, CRASH_CTX_ACTIONS = 4, CRASH_CTX_LOG = 8, CRASH_CTX_ALL = 15 };
size_t crash_context_markdown(char* out, size_t cap, unsigned sections);

// Opens a folder in Explorer (select = a file in it to select).
void crash_open_folder(const char* folder, const char* select);

// Crashes on purpose, to test the reports (tlender_tests crash-test, REALENGINE_CRASH_TEST=<kind> in the app): "acesso"
// (access violation), "divisao" (integer division by zero), "abort", "pilha" (stack overflow), "parametro" (CRT
// invalid parameter), "purecall", "heap" (double free), "thread" (a thread of its own), "pool" (a re_parallel_for
// worker), "imgui" (an IM_ASSERT) and "gpu" (Vulkan device lost). Returns only for an unknown kind.
void crash_test(const char* kind);
// Opens a file with its program (the report in a text editor).
void crash_open_file(const char* path);
// '/' to '\' on Windows (the paths shown and written in the reports).
void crash_native_slashes(char* path);
