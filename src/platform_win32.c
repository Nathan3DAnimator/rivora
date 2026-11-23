#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

bool g_platform_synthetic_input;
#ifndef __ANDROID__
int platform_touch_points(int* ids, float (*xy)[2], int cap) {
    (void)ids, (void)xy, (void)cap;
    return 0; // (the PC: the touch widgets take the mouse)
}
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib") // CommandLineToArgvW
#include <commdlg.h>
#define COBJMACROS
#include <shobjidl.h>

#include "rpak.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

FILE* re_fopen(const char* path_utf8, const char* mode) {
    if (rpak_mounted() && mode[0] == 'r' && !strchr(mode, '+')) { // (B15a: from the packaged game's Content.rpak, read only)
        uint64_t n = 0;
        uint8_t* b = rpak_read(path_utf8, &n);
        if (b) { // (decompressed into a temporary file the system deletes when it closes)
            wchar_t dir[MAX_PATH], tmp[MAX_PATH];
            FILE* f = NULL;
            if (GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"rpk", 0, tmp)) f = _wfopen(tmp, L"w+bTD");
            if (f && fwrite(b, 1, (size_t)n, f) == n) rewind(f);
            else if (f) fclose(f), f = NULL;
            free(b);
            if (f) return f;
        }
    }
    wchar_t wpath[1024], wmode[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath, 1024)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16)) return NULL;
    return _wfopen(wpath, wmode);
}

char** re_utf8_argv(int argc, char** argv) {
    int n = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w || n != argc) {
        if (w) LocalFree(w);
        return argv;
    }
    char** out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; out && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = len > 0 ? malloc((size_t)len) : NULL;
        if (!out[i] || !WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL)) out[i] = argv[i];
    }
    LocalFree(w);
    return out ? out : argv;
}

bool re_replace_file(const char* src_utf8, const char* dst_utf8) {
    wchar_t src[1024], dst[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, src_utf8, -1, src, 1024)) return false;
    if (!MultiByteToWideChar(CP_UTF8, 0, dst_utf8, -1, dst, 1024)) return false;
    // a file just written can be held for a moment (an antivirus scanning it, the indexer): try again for a while
    for (int attempt = 0; attempt < 40; attempt++) {
        if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) return false;
        Sleep(5);
    }
    return false;
}

void re_remove_file(const char* path_utf8) {
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, w, 1024)) DeleteFileW(w);
}

bool re_list_dir(const char* dir_utf8, void (*fn)(void* user, const char* name), void* user) {
    wchar_t w[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir_utf8 && dir_utf8[0] ? dir_utf8 : ".", -1, w, 1020);
    if (!n) return false;
    n--; // without the terminator
    if (n > 0 && w[n - 1] != L'\\' && w[n - 1] != L'/') w[n++] = L'\\';
    w[n++] = L'*';
    w[n] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) fn(user, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool re_system_language_portuguese(void) { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE; }

static bool user_dir(const wchar_t* var, char* out, size_t cap) {
    const wchar_t* appdata = _wgetenv(var);
    if (!appdata || !appdata[0]) return false;
    wchar_t dir[1024];
    _snwprintf(dir, 1024, L"%ls\\RealEngine", appdata);
    dir[1023] = 0;
    CreateDirectoryW(dir, NULL); // fails harmlessly when it already exists
    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, (int)cap, NULL, NULL) > 0;
}

bool re_config_dir(char* out, size_t cap) { return user_dir(L"APPDATA", out, cap); }
bool re_cache_dir(char* out, size_t cap) { return user_dir(L"LOCALAPPDATA", out, cap); }
bool re_home_dir(char* out, size_t cap) {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    return home && home[0] && WideCharToMultiByte(CP_UTF8, 0, home, -1, out, (int)cap, NULL, NULL) > 0;
}

// The color on the screen at (x, y) in screen pixels (the theme editor's eyedropper): r | g << 8 | b << 16.
bool re_screen_pixel(int x, int y, unsigned* rgb) {
    HDC dc = GetDC(NULL);
    if (!dc) return false;
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(NULL, dc);
    if (c == CLR_INVALID) return false;
    *rgb = (unsigned)GetRValue(c) | ((unsigned)GetGValue(c) << 8) | ((unsigned)GetBValue(c) << 16);
    return true;
}

// REALENGINE_MONITOR=N (the visible test suites: tools/suite.ps1 -Visivel -Monitor N): Windows' display N (DISPLAYN, the
// number in Settings > Display), its work area's corner. False: unset, 0, or no such display (then the primary, as before)
typedef struct { char want[32]; RECT work; bool found; } ReMonFind;
static BOOL CALLBACK re_mon_find(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc, (void)r;
    ReMonFind* f = (ReMonFind*)lp;
    MONITORINFOEXA mi = { .cbSize = sizeof mi };
    if (GetMonitorInfoA(m, (MONITORINFO*)&mi) && !_stricmp(mi.szDevice, f->want)) f->work = mi.rcWork, f->found = true;
    return !f->found;
}
bool re_test_monitor(int* x, int* y, int* w, int* h) {
    const char* e = getenv("REALENGINE_MONITOR");
    int n = e ? atoi(e) : 0;
    if (n <= 0) return false;
    ReMonFind f = { 0 };
    snprintf(f.want, sizeof f.want, "\\\\.\\DISPLAY%d", n);
    EnumDisplayMonitors(NULL, NULL, re_mon_find, (LPARAM)&f);
    if (!f.found) return false;
    if (x) *x = f.work.left;
    if (y) *y = f.work.top;
    if (w) *w = f.work.right - f.work.left;
    if (h) *h = f.work.bottom - f.work.top;
    return true;
}

bool platform_file_dialog_ext(struct GLFWwindow* parent, bool save, const char* label, const char* extensions,
                              char* path, size_t cap) {
    if (g_platform_synthetic_input) return false;
    wchar_t buf[1024] = L"", initial_dir[1024] = L"";
    if (path && path[0]) {
        // A start in a folder that doesn't exist (the output path's "//render/" before the first render) makes the
        // dialog fail at once without showing: it starts in the nearest folder that exists, with the file's name.
        char dir[1024], start[1024];
        platform_existing_folder(path, dir, sizeof dir);
        const char* name = strrchr(path, '\\');
        const char* name2 = strrchr(path, '/');
        if (name2 > name) name = name2;
        name = name ? name + 1 : path;
        snprintf(start, sizeof start, "%s", name);
        MultiByteToWideChar(CP_UTF8, 0, start, -1, buf, 1024);
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, initial_dir, 1024);
    }
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    if (initial_dir[0]) ofn.lpstrInitialDir = initial_dir;
    ofn.hwndOwner = parent ? glfwGetWin32Window(parent) : NULL;
    // "label (*.a;*.b)|*.a;*.b|All files (*.*)|*.*|", '|' becoming the separators after the conversion
    char patterns[1024] = "", def[32] = "";
    size_t n = 0;
    for (const char* e = extensions ? extensions : ""; *e;) {
        const char* end = strchr(e, ';');
        size_t len = end ? (size_t)(end - e) : strlen(e);
        if (!def[0]) snprintf(def, sizeof def, "%.*s", (int)len, e);
        if (n < sizeof patterns) n += (size_t)snprintf(patterns + n, sizeof patterns - n, "%s*.%.*s", n ? ";" : "", (int)len, e);
        e = end ? end + 1 : e + len;
    }
    char filter8[2600];
    if (strlen(patterns) <= 60) snprintf(filter8, sizeof filter8, "%s (%s)|%s|%s (*.*)|*.*|", label, patterns, patterns, TR("All files"));
    else snprintf(filter8, sizeof filter8, "%s|%s|%s (*.*)|*.*|", label, patterns, TR("All files")); // long lists: only the label
    wchar_t filter[2600];
    if (!MultiByteToWideChar(CP_UTF8, 0, filter8, -1, filter, 2600)) filter[0] = filter[1] = L'\0';
    for (wchar_t* c = filter; *c; c++)
        if (*c == L'|') *c = L'\0';
    wchar_t defw[32] = L"";
    MultiByteToWideChar(CP_UTF8, 0, def, -1, defw, 32);
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = defw[0] ? defw : NULL;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    return WideCharToMultiByte(CP_UTF8, 0, buf, -1, path, (int)cap, NULL, NULL) > 0;
}


static bool is_dir_w(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void platform_existing_folder(const char* path, char* out, size_t cap) {
    if (cap) out[0] = '\0';
    wchar_t w[1024];
    if (!path || !path[0] || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return;
    for (wchar_t* c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    for (;;) {
        size_t n = wcslen(w);
        while (n > 3 && w[n - 1] == L'\\') w[--n] = L'\0'; // "C:\" stays
        if (n && is_dir_w(w)) break;
        wchar_t* sep = wcsrchr(w, L'\\');
        if (!sep) return;
        if (sep == w + 2 && w[1] == L':') sep[1] = L'\0'; // down to the drive's root
        else *sep = L'\0';
        if (!w[0]) return;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

// The common item dialog in folder mode. It needs an STA apartment: on the calling thread when its COM is (or can be)
// STA; else (the Media Foundation made it MTA) on a thread of its own and without an owner window: a dialog owned by
// a window whose thread waits for it never shows (that thread must answer the owner's messages: it hung the app).
typedef struct FolderJob {
    HWND owner;
    wchar_t title[256], start[1024], result[1024];
    bool ok;
} FolderJob;

static void folder_run(FolderJob* j) {
    IFileOpenDialog* d = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d))) {
        DWORD opts = 0;
        d->lpVtbl->GetOptions(d, &opts);
        d->lpVtbl->SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (j->title[0]) d->lpVtbl->SetTitle(d, j->title);
        IShellItem* start = NULL;
        if (j->start[0] && SUCCEEDED(SHCreateItemFromParsingName(j->start, NULL, &IID_IShellItem, (void**)&start))) {
            d->lpVtbl->SetFolder(d, start);
            start->lpVtbl->Release(start);
        }
        if (SUCCEEDED(d->lpVtbl->Show(d, j->owner))) {
            IShellItem* item = NULL;
            if (SUCCEEDED(d->lpVtbl->GetResult(d, &item))) {
                PWSTR p = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
                    wcsncpy(j->result, p, 1023);
                    j->result[1023] = L'\0';
                    j->ok = true;
                    CoTaskMemFree(p);
                }
                item->lpVtbl->Release(item);
            }
        }
        d->lpVtbl->Release(d);
    }
}

static DWORD WINAPI folder_thread(LPVOID arg) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    folder_run((FolderJob*)arg);
    if (SUCCEEDED(ci)) CoUninitialize();
    return 0;
}

bool platform_folder_dialog(struct GLFWwindow* parent, const char* title, char* path, size_t cap) {
    FolderJob j;
    ZeroMemory(&j, sizeof j);
    j.owner = parent ? glfwGetWin32Window(parent) : NULL;
    if (title) MultiByteToWideChar(CP_UTF8, 0, title, -1, j.title, 256);
    char start[1024];
    platform_existing_folder(path, start, sizeof start);
    if (start[0]) MultiByteToWideChar(CP_UTF8, 0, start, -1, j.start, 1024);
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(ci)) { // this thread is STA: the dialog runs here, modal to the window
        folder_run(&j);
        CoUninitialize();
    } else {
        j.owner = NULL;
        HANDLE t = CreateThread(NULL, 0, folder_thread, &j, 0, NULL);
        if (!t) return false;
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    if (!j.ok) return false;
    size_t n = wcslen(j.result);
    if (n && j.result[n - 1] != L'\\' && n + 1 < 1024) {
        j.result[n] = L'\\';
        j.result[n + 1] = L'\0';
    }
    return WideCharToMultiByte(CP_UTF8, 0, j.result, -1, path, (int)cap, NULL, NULL) > 0;
}
int platform_file_dialog_multi(struct GLFWwindow* parent, const char* label, const char* extensions, char (*out)[1024],
                               int cap) {
    if (g_platform_synthetic_input) return 0;
    static wchar_t buf[32768];
    buf[0] = L'\0';
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = parent ? glfwGetWin32Window(parent) : NULL;
    char patterns[1024] = "";
    size_t n = 0;
    for (const char* e = extensions ? extensions : ""; *e;) {
        const char* end = strchr(e, ';');
        size_t len = end ? (size_t)(end - e) : strlen(e);
        if (n < sizeof patterns) n += (size_t)snprintf(patterns + n, sizeof patterns - n, "%s*.%.*s", n ? ";" : "", (int)len, e);
        e = end ? end + 1 : e + len;
    }
    char filter8[2600];
    if (strlen(patterns) <= 60) snprintf(filter8, sizeof filter8, "%s (%s)|%s|%s (*.*)|*.*|", label, patterns, patterns, TR("All files"));
    else snprintf(filter8, sizeof filter8, "%s|%s|%s (*.*)|*.*|", label, patterns, TR("All files")); // long lists: only the label
    wchar_t filter[2600];
    if (!MultiByteToWideChar(CP_UTF8, 0, filter8, -1, filter, 2600)) filter[0] = filter[1] = L'\0';
    for (wchar_t* c = filter; *c; c++)
        if (*c == L'|') *c = L'\0';
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof buf / sizeof buf[0]);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return 0;
    // One file: its full path. Several: the folder, then each name, separated by '\0' and ended by an empty one.
    const wchar_t* dir = buf;
    const wchar_t* name = buf + wcslen(buf) + 1;
    int count = 0;
    if (!*name) {
        if (cap > 0 && WideCharToMultiByte(CP_UTF8, 0, dir, -1, out[0], 1024, NULL, NULL) > 0) count = 1;
        return count;
    }
    for (; *name && count < cap; name += wcslen(name) + 1) {
        wchar_t full[1024];
        _snwprintf(full, 1024, L"%ls\\%ls", dir, name);
        full[1023] = 0;
        if (WideCharToMultiByte(CP_UTF8, 0, full, -1, out[count], 1024, NULL, NULL) > 0) count++;
    }
    return count;
}

struct ReThread {
    HANDLE handle;
    int (*fn)(void*);
    void* arg;
};

static DWORD WINAPI thread_entry(LPVOID p) {
    ReThread* t = p;
    return (DWORD)t->fn(t->arg);
}

ReThread* re_thread_start(int (*fn)(void* arg), void* arg) {
    ReThread* t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 0, thread_entry, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

void re_thread_join(ReThread* t) {
    if (!t) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

int re_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
}

double re_free_memory_gb(void) {
    MEMORYSTATUSEX ms = { sizeof ms };
    return GlobalMemoryStatusEx(&ms) ? (double)ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0) : 0.0;
}

double re_time_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

uint32_t re_thread_id(void) { return (uint32_t)GetCurrentThreadId(); }
void re_thread_set_low_priority(bool low) {
    SetThreadPriority(GetCurrentThread(), low ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
}

int32_t re_atomic_add(volatile int32_t* p, int32_t v) { return InterlockedAdd((volatile LONG*)p, v); }
int32_t re_atomic_load(volatile int32_t* p) { return InterlockedCompareExchange((volatile LONG*)p, 0, 0); }
void re_atomic_store(volatile int32_t* p, int32_t v) { InterlockedExchange((volatile LONG*)p, v); }
bool re_atomic_cas(volatile int32_t* p, int32_t expected, int32_t desired) {
    return InterlockedCompareExchange((volatile LONG*)p, desired, expected) == expected;
}
uint64_t re_atomic_or64(volatile uint64_t* p, uint64_t v) { return (uint64_t)InterlockedOr64((volatile LONG64*)p, (LONG64)v); }
void re_yield(void) { SwitchToThread(); }
void re_cpu_relax(void) { YieldProcessor(); }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// A high resolution waitable timer (Windows 10 1803+) sleeps with ~0.5 ms precision; the rest is a short spin.
void re_sleep_seconds(double s) {
    if (s <= 0.0) return;
    double end = re_time_seconds() + s;
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) bool tried;
    if (!tried) {
        tried = true;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    double coarse = s - 0.0015; // leave the last 1.5 ms to the spin
    if (coarse > 0.0) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(coarse * 1e7); // relative, 100 ns units
            if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep((DWORD)(coarse * 1000.0));
        }
    }
    while (re_time_seconds() < end) SwitchToThread();
}

void re_terminate_process(int code) { TerminateProcess(GetCurrentProcess(), (UINT)code); }

// --- Thread pool ---
// Workers wait for a new generation of the job, take items by an atomic counter, each item counted done. The caller
// takes items too and returns once every item is done: never waiting for a worker that slept and wakes late (a
// sleeping worker takes ~0.1-0.3 ms to wake; waiting for all of them cost that on every call: 2026-11-19, ~15 calls a
// physics step). The generation is odd while the caller writes a job: it first waits for the workers still inside the
// last one (registered in `active`, running, so it's short) to leave; a worker registers, then checks the generation
// again, so it never reads a job half written. After a job workers spin for a while before sleeping: the calls of one
// frame (rig, skinning, normals) follow each other within microseconds.
#define POOL_MAX 31
#define POOL_SPIN_SECONDS 0.0002
static struct {
    volatile LONG state;  // 0 not started, 1 starting, 2 ready, 3 unavailable
    int workers;
    SRWLOCK lock, call;
    CONDITION_VARIABLE wake, done_cv;
    volatile LONG gen, next, count, done, active, waiting;
    void (*fn)(void*, int);
    void* arg;
} g_pool = { 0, 0, SRWLOCK_INIT, SRWLOCK_INIT, CONDITION_VARIABLE_INIT, CONDITION_VARIABLE_INIT };
static __declspec(thread) int t_pool_worker;

void (*volatile g_re_pool_span)(double t0, double t1, int items);
static void pool_take_items(void) {
    void (*span)(double, double, int) = g_re_pool_span;
    double t0 = span ? re_time_seconds() : 0.0;
    int items = 0;
    for (;;) {
        LONG i = InterlockedIncrement(&g_pool.next) - 1;
        if (i >= g_pool.count) break;
        g_pool.fn(g_pool.arg, (int)i);
        items++;
        if (InterlockedIncrement(&g_pool.done) == g_pool.count && g_pool.waiting) { // (the last item: the caller sleeps)
            AcquireSRWLockExclusive(&g_pool.lock);
            WakeConditionVariable(&g_pool.done_cv);
            ReleaseSRWLockExclusive(&g_pool.lock);
        }
    }
    if (span && items) span(t0, re_time_seconds(), items); // (B14b: the trace's worker tracks)
}

static int pool_worker(void* p) {
    (void)p;
    t_pool_worker = 1;
    LONG seen = 0;
    for (;;) {
        if (g_pool.gen == seen || (g_pool.gen & 1)) {
            double until = re_time_seconds() + POOL_SPIN_SECONDS;
            for (int k = 0; g_pool.gen == seen || (g_pool.gen & 1); k++) {
                YieldProcessor();
                if ((k & 63) == 63 && re_time_seconds() > until) break;
            }
            AcquireSRWLockExclusive(&g_pool.lock);
            while (g_pool.gen == seen || (g_pool.gen & 1)) SleepConditionVariableSRW(&g_pool.wake, &g_pool.lock, INFINITE, 0);
            ReleaseSRWLockExclusive(&g_pool.lock);
        }
        LONG g = g_pool.gen;
        InterlockedIncrement(&g_pool.active);
        if (g_pool.gen != g || (g & 1)) { // (a new job is being written: wait for it)
            InterlockedDecrement(&g_pool.active);
            continue;
        }
        seen = g;
        pool_take_items();
        InterlockedDecrement(&g_pool.active);
    }
}

static bool pool_ready(void) {
    LONG st = InterlockedCompareExchange(&g_pool.state, 1, 0);
    if (st == 0) {
        int n = re_cpu_count() - 1;
        n = n > POOL_MAX ? POOL_MAX : n;
        int started = 0;
        for (int i = 0; i < n; i++) started += re_thread_start(pool_worker, NULL) != NULL; // kept until the process ends
        g_pool.workers = started;
        InterlockedExchange(&g_pool.state, started > 0 ? 2 : 3);
        return started > 0;
    }
    while (st == 1) {
        SwitchToThread();
        st = g_pool.state;
    }
    return st == 2;
}

int re_pool_threads(void) { return pool_ready() ? g_pool.workers + 1 : 1; }

void re_parallel_for(int count, void (*fn)(void* arg, int index), void* arg) {
    if (count <= 0) return;
    // Serially: one item, inside a worker (nested), or while another thread runs a job.
    if (count == 1 || t_pool_worker || !pool_ready() || !TryAcquireSRWLockExclusive(&g_pool.call)) {
        for (int i = 0; i < count; i++) fn(arg, i);
        return;
    }
    InterlockedIncrement(&g_pool.gen); // (odd: being written)
    for (int k = 0; g_pool.active; k++) // the workers still in the last job leave it (they run: a moment)
        if (k < 4096) YieldProcessor();
        else SwitchToThread();
    g_pool.fn = fn;
    g_pool.arg = arg;
    g_pool.count = count;
    g_pool.done = 0;
    InterlockedExchange(&g_pool.next, 0);
    AcquireSRWLockExclusive(&g_pool.lock);
    InterlockedIncrement(&g_pool.gen); // (even: ready)
    WakeAllConditionVariable(&g_pool.wake);
    ReleaseSRWLockExclusive(&g_pool.lock);
    t_pool_worker = 1; // nested calls from fn run serially
    pool_take_items();
    t_pool_worker = 0;
    double until = re_time_seconds() + POOL_SPIN_SECONDS;
    for (int k = 0; g_pool.done < count; k++) { // (the items other workers still run)
        YieldProcessor();
        if ((k & 63) == 63 && re_time_seconds() > until) break;
    }
    if (g_pool.done < count) {
        InterlockedExchange(&g_pool.waiting, 1);
        AcquireSRWLockExclusive(&g_pool.lock);
        while (g_pool.done < count) SleepConditionVariableSRW(&g_pool.done_cv, &g_pool.lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&g_pool.lock);
        InterlockedExchange(&g_pool.waiting, 0);
    }
    ReleaseSRWLockExclusive(&g_pool.call);
}
#else // POSIX (the Android game: android/platform_android.c has the folders; notas-dev/android.md)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include "re_android.h"
#endif

FILE* re_fopen(const char* path_utf8, const char* mode) { return fopen(path_utf8, mode); }
bool re_replace_file(const char* src_utf8, const char* dst_utf8) { return rename(src_utf8, dst_utf8) == 0; }
char** re_utf8_argv(int argc, char** argv) {
    (void)argc;
    return argv;
}
void re_remove_file(const char* path_utf8) { remove(path_utf8); }

bool re_list_dir(const char* dir_utf8, void (*fn)(void* user, const char* name), void* user) {
    DIR* d = opendir(dir_utf8 && dir_utf8[0] ? dir_utf8 : ".");
    if (!d) return false;
    for (struct dirent* e; (e = readdir(d)) != NULL;) {
        char path[2048];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir_utf8 && dir_utf8[0] ? dir_utf8 : ".", e->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) fn(user, e->d_name);
    }
    closedir(d);
    return true;
}

static bool dir_in(const char* base, const char* sub, char* out, size_t cap) {
    if (!base || !base[0]) return false;
    snprintf(out, cap, "%s/%s", base, sub);
    mkdir(out, 0755);
    return true;
}
bool re_config_dir(char* out, size_t cap) {
#ifdef __ANDROID__
    return dir_in(re_android_files_dir(), "config", out, cap);
#else
    char b[1024];
    const char* home = getenv("HOME");
    if (!home) return false;
    snprintf(b, sizeof b, "%s/.config", home);
    mkdir(b, 0755);
    return dir_in(b, "realengine", out, cap);
#endif
}
bool re_home_dir(char* out, size_t cap) {
#ifdef __ANDROID__
    snprintf(out, cap, "%s", re_android_files_dir());
    return true;
#else
    const char* home = getenv("HOME");
    if (!home) return false;
    snprintf(out, cap, "%s", home);
    return true;
#endif
}
bool re_cache_dir(char* out, size_t cap) {
#ifdef __ANDROID__
    snprintf(out, cap, "%s", re_android_cache_dir());
    return true;
#else
    char b[1024];
    const char* home = getenv("HOME");
    if (!home) return false;
    snprintf(b, sizeof b, "%s/.cache", home);
    mkdir(b, 0755);
    return dir_in(b, "realengine", out, cap);
#endif
}
bool re_system_language_portuguese(void) {
    const char* l = getenv("LANG");
#ifdef __ANDROID__
    static char lang[16];
    if (!lang[0]) re_android_call_string("language", "()Ljava/lang/String;", lang, sizeof lang);
    l = lang;
#endif
    return l && !strncmp(l, "pt", 2);
}
bool re_screen_pixel(int x, int y, unsigned* rgb) {
    (void)x, (void)y, (void)rgb;
    return false;
}

bool re_test_monitor(int* x, int* y, int* w, int* h) {
    (void)x, (void)y, (void)w, (void)h;
    return false;
}
// (the game has no file dialogs: answered "cancelled")
bool platform_file_dialog_ext(struct GLFWwindow* parent, bool save, const char* label, const char* extensions, char* path, size_t cap) {
    (void)parent, (void)save, (void)label, (void)extensions, (void)path, (void)cap;
    return false;
}
int platform_file_dialog_multi(struct GLFWwindow* parent, const char* label, const char* extensions, char (*out)[1024], int cap) {
    (void)parent, (void)label, (void)extensions, (void)out, (void)cap;
    return 0;
}
bool platform_folder_dialog(struct GLFWwindow* parent, const char* title, char* path, size_t cap) {
    (void)parent, (void)title, (void)path, (void)cap;
    return false;
}
void platform_existing_folder(const char* path, char* out, size_t cap) {
    char b[1024];
    snprintf(b, sizeof b, "%s", path ? path : "");
    for (;;) {
        struct stat st;
        size_t n = strlen(b);
        while (n && b[n - 1] == '/') b[--n] = 0;
        if (!n) break;
        if (stat(b, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, cap, "%s", b);
            return;
        }
        char* s = strrchr(b, '/');
        if (!s) break;
        *s = 0;
    }
    if (cap) out[0] = 0;
}

struct ReThread {
    pthread_t handle;
    int (*fn)(void*);
    void* arg;
};
static void* thread_entry(void* p) {
    ReThread* t = p;
    t->fn(t->arg);
    return NULL;
}
ReThread* re_thread_start(int (*fn)(void* arg), void* arg) {
    ReThread* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn, t->arg = arg;
    if (pthread_create(&t->handle, NULL, thread_entry, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}
void re_thread_join(ReThread* t) {
    if (!t) return;
    pthread_join(t->handle, NULL);
    free(t);
}
int re_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
double re_free_memory_gb(void) {
    long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? (double)pages * (double)size / (1024.0 * 1024.0 * 1024.0) : 0.0;
}
double re_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
uint32_t re_thread_id(void) { return (uint32_t)(uintptr_t)pthread_self(); }
void re_thread_set_low_priority(bool low) {
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, (id_t)gettid(), low ? 10 : 0); // (per thread on Android: Linux threads are tasks)
#else
    (void)low;
#endif
}
int32_t re_atomic_add(volatile int32_t* p, int32_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
int32_t re_atomic_load(volatile int32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void re_atomic_store(volatile int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
bool re_atomic_cas(volatile int32_t* p, int32_t expected, int32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
uint64_t re_atomic_or64(volatile uint64_t* p, uint64_t v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
void re_yield(void) { sched_yield(); }
void re_cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
void re_sleep_seconds(double s) {
    if (s <= 0.0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}
void re_terminate_process(int code) { _exit(code); }

// --- The thread pool (Windows' scheme: generations, an atomic item counter, a short spin before sleeping; phones spin
// less: the battery) ---
#define POOL_MAX 31
#ifdef __ANDROID__
#define POOL_SPIN_SECONDS 0.00005
#else
#define POOL_SPIN_SECONDS 0.0002
#endif
static struct {
    volatile int32_t state; // 0 not started, 1 starting, 2 ready, 3 unavailable
    int workers;
    pthread_mutex_t lock, call;
    pthread_cond_t wake, done_cv;
    volatile int32_t gen, next, count, done, active, waiting; // (gen odd: a job being written; see the Windows pool)
    void (*fn)(void*, int);
    void* arg;
} g_pool = { 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, NULL, NULL };
static __thread int t_pool_worker;

void (*volatile g_re_pool_span)(double t0, double t1, int items);
static void pool_take_items(void) {
    void (*span)(double, double, int) = g_re_pool_span;
    double t0 = span ? re_time_seconds() : 0.0;
    int items = 0;
    for (;;) {
        int32_t i = re_atomic_add(&g_pool.next, 1) - 1;
        if (i >= g_pool.count) break;
        g_pool.fn(g_pool.arg, (int)i);
        items++;
        if (re_atomic_add(&g_pool.done, 1) == g_pool.count && re_atomic_load(&g_pool.waiting)) {
            pthread_mutex_lock(&g_pool.lock);
            pthread_cond_signal(&g_pool.done_cv);
            pthread_mutex_unlock(&g_pool.lock);
        }
    }
    if (span && items) span(t0, re_time_seconds(), items);
}
static bool pool_idle(int32_t seen) {
    int32_t g = re_atomic_load(&g_pool.gen);
    return g == seen || (g & 1);
}
static int pool_worker(void* p) {
    (void)p;
    t_pool_worker = 1;
    int32_t seen = 0;
    for (;;) {
        if (pool_idle(seen)) {
            double until = re_time_seconds() + POOL_SPIN_SECONDS;
            for (int k = 0; pool_idle(seen); k++) {
                re_cpu_relax();
                if ((k & 63) == 63 && re_time_seconds() > until) break;
            }
            pthread_mutex_lock(&g_pool.lock);
            while (pool_idle(seen)) pthread_cond_wait(&g_pool.wake, &g_pool.lock);
            pthread_mutex_unlock(&g_pool.lock);
        }
        int32_t g = re_atomic_load(&g_pool.gen);
        re_atomic_add(&g_pool.active, 1);
        if (re_atomic_load(&g_pool.gen) != g || (g & 1)) {
            re_atomic_add(&g_pool.active, -1);
            continue;
        }
        seen = g;
        pool_take_items();
        re_atomic_add(&g_pool.active, -1);
    }
    return 0;
}
static bool pool_ready(void) {
    if (re_atomic_cas(&g_pool.state, 0, 1)) {
        int n = re_cpu_count() - 1;
        n = n > POOL_MAX ? POOL_MAX : n;
        int started = 0;
        for (int i = 0; i < n; i++) started += re_thread_start(pool_worker, NULL) != NULL; // (kept until the process ends)
        g_pool.workers = started;
        re_atomic_store(&g_pool.state, started > 0 ? 2 : 3);
        return started > 0;
    }
    int32_t st;
    while ((st = re_atomic_load(&g_pool.state)) == 1) sched_yield();
    return st == 2;
}
int re_pool_threads(void) { return pool_ready() ? g_pool.workers + 1 : 1; }
void re_parallel_for(int count, void (*fn)(void* arg, int index), void* arg) {
    if (count <= 0) return;
    if (count == 1 || t_pool_worker || !pool_ready() || pthread_mutex_trylock(&g_pool.call) != 0) {
        for (int i = 0; i < count; i++) fn(arg, i);
        return;
    }
    re_atomic_add(&g_pool.gen, 1); // (odd: being written)
    for (int k = 0; re_atomic_load(&g_pool.active); k++)
        if (k < 4096) re_cpu_relax();
        else sched_yield();
    g_pool.fn = fn, g_pool.arg = arg, g_pool.count = count;
    re_atomic_store(&g_pool.done, 0);
    re_atomic_store(&g_pool.next, 0);
    pthread_mutex_lock(&g_pool.lock);
    re_atomic_add(&g_pool.gen, 1); // (even: ready)
    pthread_cond_broadcast(&g_pool.wake);
    pthread_mutex_unlock(&g_pool.lock);
    t_pool_worker = 1; // (nested calls from fn run serially)
    pool_take_items();
    t_pool_worker = 0;
    double until = re_time_seconds() + POOL_SPIN_SECONDS;
    for (int k = 0; re_atomic_load(&g_pool.done) < count; k++) {
        re_cpu_relax();
        if ((k & 63) == 63 && re_time_seconds() > until) break;
    }
    if (re_atomic_load(&g_pool.done) < count) {
        re_atomic_store(&g_pool.waiting, 1);
        pthread_mutex_lock(&g_pool.lock);
        while (re_atomic_load(&g_pool.done) < count) pthread_cond_wait(&g_pool.done_cv, &g_pool.lock);
        pthread_mutex_unlock(&g_pool.lock);
        re_atomic_store(&g_pool.waiting, 0);
    }
    pthread_mutex_unlock(&g_pool.call);
}
#endif // _WIN32

#ifdef _WIN32
// (B13) the editor's other players' processes
struct ReProcess { PROCESS_INFORMATION pi; };
ReProcess* re_process_start(const char* cmdline) {
    ReProcess* p = calloc(1, sizeof *p);
    wchar_t* w = p ? malloc(sizeof(wchar_t) * 4096) : NULL;
    if (!w || !MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, w, 4096)) return free(w), free(p), NULL;
    STARTUPINFOW si = { sizeof si };
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, FALSE, 0, NULL, NULL, &si, &p->pi);
    free(w);
    if (!ok) return free(p), NULL;
    return p;
}
void re_process_kill(ReProcess* p) {
    if (!p) return;
    TerminateProcess(p->pi.hProcess, 0);
    CloseHandle(p->pi.hProcess), CloseHandle(p->pi.hThread);
    free(p);
}

bool re_run_capture(const char* cmdline, char* out, size_t cap, int* exit_code) {
    if (out && cap) out[0] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t* w = malloc(sizeof(wchar_t) * 8192);
    if (!w || !MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, w, 8192)) return free(w), CloseHandle(rd), CloseHandle(wr), false;
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES, si.hStdOutput = wr, si.hStdError = wr, si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(w);
    CloseHandle(wr);
    if (!ok) return CloseHandle(rd), false;
    size_t n = 0;
    char buf[4096];
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) { // (read to the end: a full pipe would stop the child)
        if (out && n + 1 < cap) {
            size_t k = got < cap - 1 - n ? got : cap - 1 - n;
            memcpy(out + n, buf, k), n += k, out[n] = 0;
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    if (exit_code) *exit_code = (int)code;
    return true;
}
#else // (the Android game starts no process: the editor's tools only)
ReProcess* re_process_start(const char* cmdline) { (void)cmdline; return NULL; }
void re_process_kill(ReProcess* p) { (void)p; }
bool re_run_capture(const char* cmdline, char* out, size_t cap, int* exit_code) {
    (void)cmdline;
    if (out && cap) out[0] = 0;
    if (exit_code) *exit_code = -1;
    return false;
}
#endif

// (2026-11-19) A gate: a count threads sleep on until it changes (the async traces' workers, scene_query.c)
#ifdef _WIN32
struct ReGate {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    volatile LONG value;
};
ReGate* re_gate_create(void) {
    ReGate* g = calloc(1, sizeof *g); // (once, at the first async trace)
    if (g) InitializeSRWLock(&g->lock), InitializeConditionVariable(&g->cv);
    return g;
}
int32_t re_gate_wait(ReGate* g, int32_t seen) {
    AcquireSRWLockExclusive(&g->lock);
    while (g->value == seen) SleepConditionVariableSRW(&g->cv, &g->lock, INFINITE, 0);
    int32_t v = g->value;
    ReleaseSRWLockExclusive(&g->lock);
    return v;
}
void re_gate_bump(ReGate* g) {
    AcquireSRWLockExclusive(&g->lock);
    g->value++;
    ReleaseSRWLockExclusive(&g->lock);
    WakeAllConditionVariable(&g->cv);
}
#else
struct ReGate {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    volatile int32_t value;
};
ReGate* re_gate_create(void) {
    ReGate* g = calloc(1, sizeof *g);
    if (g) pthread_mutex_init(&g->lock, NULL), pthread_cond_init(&g->cv, NULL);
    return g;
}
int32_t re_gate_wait(ReGate* g, int32_t seen) {
    pthread_mutex_lock(&g->lock);
    while (g->value == seen) pthread_cond_wait(&g->cv, &g->lock);
    int32_t v = g->value;
    pthread_mutex_unlock(&g->lock);
    return v;
}
void re_gate_bump(ReGate* g) {
    pthread_mutex_lock(&g->lock);
    g->value++;
    pthread_mutex_unlock(&g->lock);
    pthread_cond_broadcast(&g->cv);
}
#endif
