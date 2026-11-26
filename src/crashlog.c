// Crash reports and the app's log (crashlog.h). Windows: SEH, dbghelp, minidumps. Elsewhere only the log works.
#include "crashlog.h"

#include "i18n.h"
#include "platform.h"
#include "re_commit.h" // RE_COMMIT, written by tools/gen_commit.cmake on every build

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RE_VERSION_STRING
#define RE_VERSION_STRING "(testes)"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <intrin.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>
#include <signal.h>
#include <tlhelp32.h>
#include <time.h>
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Small text helpers (no allocation: they run inside a crashed process too)
// ---------------------------------------------------------------------------------------------------------------------

typedef struct Buf {
    char* p;
    size_t n, cap;
} Buf;

static void bput(Buf* b, const char* s) {
    if (!s) return;
    size_t len = strlen(s);
    if (b->n + 1 >= b->cap) return;
    if (len > b->cap - 1 - b->n) len = b->cap - 1 - b->n;
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = 0;
}

static void bprintf(Buf* b, const char* fmt, ...) {
    if (b->n + 1 >= b->cap) return;
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, a);
    va_end(a);
    if (w < 0) return;
    b->n += (size_t)w < b->cap - b->n ? (size_t)w : b->cap - 1 - b->n;
}

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The log ring
// ---------------------------------------------------------------------------------------------------------------------

typedef struct LogLine {
    uint64_t when; // FILETIME (UTC, 100 ns) on Windows, seconds elsewhere
    uint32_t thread;
    uint8_t level;
    char text[CRASH_LOG_LINE_LEN];
} LogLine;

typedef struct ActionLine {
    uint64_t when;
    char text[CRASH_ACTION_LEN];
} ActionLine;

static LogLine g_lines[CRASH_LOG_LINES];
static ActionLine g_actions[CRASH_ACTIONS];
static volatile int32_t g_line_count, g_action_count;
static volatile int32_t g_log_lock;
static volatile int32_t g_capturing;

static uint64_t now_stamp(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    return (uint64_t)re_time_seconds();
#endif
}

// A spin lock; a reader inside a crashed process gives up after a while (the owner may be frozen) and reads anyway.
static bool log_lock(int tries) {
    for (int i = 0; tries < 0 || i < tries; i++) {
        if (re_atomic_cas(&g_log_lock, 0, 1)) return true;
        if (i > 64) re_yield();
    }
    return false;
}
static void log_unlock(void) { re_atomic_store(&g_log_lock, 0); }

#ifdef _WIN32
static HANDLE g_console_err = NULL; // the real stderr while captured (re_log echoes there)
#endif

#define LOG_FROM_STDOUT 0xFFFFFFFFu // the line came through the captured stdout / stderr (no thread of its own)
#define LOG_FROM_STDERR 0xFFFFFFFEu
static void log_text(ReLogLevel level, const char* text, size_t len, uint32_t thread);
static void log_file_line(uint64_t when, ReLogLevel level, uint32_t thread, const char* text, size_t len);

void re_log_text(ReLogLevel level, const char* text, size_t len) { log_text(level, text, len, re_thread_id()); }

static void log_text(ReLogLevel level, const char* text, size_t len, uint32_t thread) {
    if (!text) return;
    if (len == (size_t)-1) len = strlen(text);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    log_lock(-1);
    int32_t idx = g_line_count;
    LogLine* l = &g_lines[idx % CRASH_LOG_LINES];
    uint64_t when = now_stamp();
    l->when = when;
    l->thread = thread;
    l->level = (uint8_t)level;
    size_t n = len < CRASH_LOG_LINE_LEN - 1 ? len : CRASH_LOG_LINE_LEN - 1;
    if (n < len)
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--; // cut between UTF-8 characters, not inside one
    memcpy(l->text, text, n);
    l->text[n] = 0;
    for (size_t i = 0; i < n; i++) // one line per entry
        if (l->text[i] == '\n' || l->text[i] == '\r') l->text[i] = ' ';
    re_atomic_store(&g_line_count, idx + 1);
    log_unlock();
    log_file_line(when, level, thread, l->text, n);
}

void re_log(ReLogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    const char* s = buf;
    while (*s) { // one ring line per text line
        const char* e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        re_log_text(level, s, n);
        s += n + (e ? 1 : 0);
    }
    if (level == RE_LOG_INFO) return; // only warnings and errors reach the console
    // Shown on the console too (once: while stderr is captured its lines come back into the ring by themselves)
    static const char* const tags[] = { "", "aviso: ", "erro: " };
#ifdef _WIN32
    if (re_atomic_load(&g_capturing)) {
        if (g_console_err) {
            char line[1100];
            int w = snprintf(line, sizeof line, "[RealEngine] %s%s\n", tags[level], buf);
            DWORD wr;
            if (w > 0) WriteFile(g_console_err, line, (DWORD)(w < (int)sizeof line ? w : (int)sizeof line - 1), &wr, NULL);
        }
        return;
    }
#endif
    fprintf(stderr, "[RealEngine] %s%s\n", tags[level], buf);
}

void re_log_action(const char* fmt, ...) {
    char buf[CRASH_ACTION_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    char* nl = strchr(buf, '\n'); // a tooltip: its first line
    if (nl) *nl = 0;
    log_lock(-1);
    int32_t idx = g_action_count;
    ActionLine* l = &g_actions[idx % CRASH_ACTIONS];
    l->when = now_stamp();
    copy_str(l->text, sizeof l->text, buf);
    re_atomic_store(&g_action_count, idx + 1);
    log_unlock();
}

static void format_clock(uint64_t when, char* out, size_t cap) {
#ifdef _WIN32
    FILETIME ft = { (DWORD)when, (DWORD)(when >> 32) }, lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    snprintf(out, cap, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    snprintf(out, cap, "%llu", (unsigned long long)when);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// The log file and the debugging console (the app is a window app: no console of its own)
// ---------------------------------------------------------------------------------------------------------------------

#ifdef _WIN32
static HANDLE g_log_file = NULL;
#endif
static char g_log_path[1024];

static void log_file_line(uint64_t when, ReLogLevel level, uint32_t thread, const char* text, size_t len) {
#ifdef _WIN32
    HANDLE f = g_log_file;
    if (!f) return;
    static const char* const lv[] = { "info ", "aviso", "erro " };
    char clock[32], line[CRASH_LOG_LINE_LEN + 64], who[16];
    format_clock(when, clock, sizeof clock);
    if (thread >= LOG_FROM_STDERR) copy_str(who, sizeof who, thread == LOG_FROM_STDOUT ? "stdout" : "stderr");
    else snprintf(who, sizeof who, "t%u", thread);
    int w = snprintf(line, sizeof line, "%s %s %-7s %.*s\r\n", clock, lv[level <= 2 ? level : 0], who, (int)len, text);
    if (w <= 0) return;
    if (w >= (int)sizeof line) w = (int)sizeof line - 1;
    DWORD wr;
    WriteFile(f, line, (DWORD)w, &wr, NULL); // opened for appending: whole lines from any thread
#else
    (void)when; (void)level; (void)thread; (void)text; (void)len;
#endif
}

bool re_log_open_file(void) {
#ifdef _WIN32
    if (g_log_file) return true;
    char dir[1024];
    if (!crash_reports_dir("logs", dir, sizeof dir)) return false;
    wchar_t wdir[1024], wpath[1100], wold[1100];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 1024);
    swprintf(wpath, 1100, L"%ls\\realengine.log", wdir);
    swprintf(wold, 1100, L"%ls\\realengine_anterior.log", wdir);
    // The previous run's log kept once; a log another Real Engine has open stays (this one writes its own)
    HANDLE h = CreateFileW(wpath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    bool busy = false;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    else busy = GetLastError() == ERROR_SHARING_VIOLATION;
    if (!busy) MoveFileExW(wpath, wold, MOVEFILE_REPLACE_EXISTING);
    else swprintf(wpath, 1100, L"%ls\\realengine_%lu.log", wdir, (unsigned long)GetCurrentProcessId());
    h = CreateFileW(wpath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr;
    WriteFile(h, "\xEF\xBB\xBF", 3, &wr, NULL); // UTF-8 (the Notepad shows the accents)
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, g_log_path, (int)sizeof g_log_path, NULL, NULL);
    // the lines the ring got before the file existed, then every new one
    log_lock(-1);
    int32_t total = g_line_count, n = total < CRASH_LOG_LINES ? total : CRASH_LOG_LINES;
    g_log_file = h;
    log_unlock();
    for (int32_t i = total - n; i < total; i++) {
        const LogLine* l = &g_lines[i % CRASH_LOG_LINES];
        log_file_line(l->when, (ReLogLevel)l->level, l->thread, l->text, strlen(l->text));
    }
    return true;
#else
    return false;
#endif
}

const char* re_log_file_path(void) { return g_log_path; }

bool re_console_open(void) {
#ifdef _WIN32
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return false;
    SetConsoleOutputCP(CP_UTF8); // the log is UTF-8
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return false;
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, out);
    return true;
#else
    return false;
#endif
}

size_t re_log_dump(char* out, size_t cap, int max_lines) {
    Buf b = { out, 0, cap };
    if (cap) out[0] = 0;
    bool locked = log_lock(200000);
    int32_t total = re_atomic_load(&g_line_count);
    int32_t n = total < CRASH_LOG_LINES ? total : CRASH_LOG_LINES;
    if (max_lines >= 0 && n > max_lines) n = max_lines;
    static const char* const lv[] = { "info ", "aviso", "erro " };
    for (int32_t i = total - n; i < total; i++) {
        const LogLine* l = &g_lines[i % CRASH_LOG_LINES];
        char clock[32];
        format_clock(l->when, clock, sizeof clock);
        if (l->thread >= LOG_FROM_STDERR)
            bprintf(&b, "%s %s %-7s %s\n", clock, lv[l->level <= 2 ? l->level : 0],
                    l->thread == LOG_FROM_STDOUT ? "stdout" : "stderr", l->text);
        else
            bprintf(&b, "%s %s t%-6u %s\n", clock, lv[l->level <= 2 ? l->level : 0], l->thread, l->text);
    }
    if (locked) log_unlock();
    return b.n;
}

size_t re_log_actions_dump(char* out, size_t cap, int max_actions) {
    Buf b = { out, 0, cap };
    if (cap) out[0] = 0;
    bool locked = log_lock(200000);
    int32_t total = re_atomic_load(&g_action_count);
    int32_t n = total < CRASH_ACTIONS ? total : CRASH_ACTIONS;
    if (max_actions >= 0 && n > max_actions) n = max_actions;
    for (int32_t i = total - n; i < total; i++) {
        const ActionLine* l = &g_actions[i % CRASH_ACTIONS];
        char clock[32];
        format_clock(l->when, clock, sizeof clock);
        bprintf(&b, "%s %s\n", clock, l->text);
    }
    if (locked) log_unlock();
    return b.n;
}

int re_log_count(void) { return re_atomic_load(&g_line_count); }

void re_log_clear(void) {
    log_lock(-1);
    re_atomic_store(&g_line_count, 0);
    re_atomic_store(&g_action_count, 0);
    log_unlock();
}

// ---------------------------------------------------------------------------------------------------------------------
// stdout / stderr into the log
// ---------------------------------------------------------------------------------------------------------------------

#ifdef _WIN32
typedef struct StdPipe {
    HANDLE read, console;
    ReLogLevel level;
} StdPipe;
static StdPipe g_pipes[2];
static volatile int32_t g_pipe_pending; // bytes read and not yet ended by a newline (the report waits a moment)

static ReLogLevel stderr_level(const char* s, size_t n) {
    for (size_t i = 0; i + 4 < n; i++)
        if (!_strnicmp(s + i, "erro", 4) || !_strnicmp(s + i, "fail", 4) || !_strnicmp(s + i, "falh", 4) ||
            !_strnicmp(s + i, "assert", 6))
            return RE_LOG_ERROR;
    return RE_LOG_WARN;
}

static DWORD WINAPI pipe_reader(LPVOID arg) {
    StdPipe* p = arg;
    char chunk[4096], line[CRASH_LOG_LINE_LEN];
    size_t ln = 0;
    DWORD got;
    while (ReadFile(p->read, chunk, sizeof chunk, &got, NULL) && got) {
        DWORD wr;
        if (p->console) WriteFile(p->console, chunk, got, &wr, NULL);
        for (DWORD i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n' || ln + 1 >= sizeof line) {
                if (ln)
                    log_text(p->level == RE_LOG_INFO ? RE_LOG_INFO : stderr_level(line, ln), line, ln,
                             p->level == RE_LOG_INFO ? LOG_FROM_STDOUT : LOG_FROM_STDERR);
                ln = 0;
                if (c == '\n') continue;
            }
            line[ln++] = c;
        }
        re_atomic_store(&g_pipe_pending, (int32_t)ln);
    }
    return 0;
}

static bool redirect_fd(int fd, DWORD std_id, StdPipe* p, ReLogLevel level) {
    HANDLE rd, wr, cur = GetStdHandle(std_id), dup = NULL;
    if (cur && cur != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!CreatePipe(&rd, &wr, NULL, 1 << 16)) return false;
    int nfd = _open_osfhandle((intptr_t)wr, _O_TEXT | _O_WRONLY);
    if (nfd < 0) {
        CloseHandle(rd);
        CloseHandle(wr);
        return false;
    }
    FILE* fp = fd == 1 ? stdout : stderr;
    fflush(fp);
    // A window app starts with no stdout/stderr (the stream has no descriptor): it gets one first
    if (_fileno(fp) < 0) freopen("NUL", "w", fp);
    _dup2(nfd, fd);
    if (_fileno(fp) >= 0 && _fileno(fp) != fd) _dup2(nfd, _fileno(fp));
    _close(nfd);
    SetStdHandle(std_id, (HANDLE)_get_osfhandle(fd));
    p->read = rd;
    p->console = dup;
    p->level = level;
    HANDLE t = CreateThread(NULL, 64 * 1024, pipe_reader, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
    return true;
}
#endif

void re_log_capture_std(void) {
#ifdef _WIN32
    if (re_atomic_load(&g_capturing)) return;
    redirect_fd(1, STD_OUTPUT_HANDLE, &g_pipes[0], RE_LOG_INFO);
    if (redirect_fd(2, STD_ERROR_HANDLE, &g_pipes[1], RE_LOG_WARN)) g_console_err = g_pipes[1].console;
    setvbuf(stdout, NULL, _IONBF, 0); // a pipe would make stdout fully buffered: lines arrive as they are printed
    re_atomic_store(&g_capturing, 1);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Folders and names
// ---------------------------------------------------------------------------------------------------------------------

void crash_native_slashes(char* path) {
#ifdef _WIN32
    for (char* c = path; *c; c++)
        if (*c == '/') *c = '\\';
#else
    (void)path;
#endif
}

static char g_base[1024];   // folder that gets crashes/ and bugReport/ (UTF-8)
static bool g_installed;
static CrashHooks g_hooks;
static char g_gpu[512] = "(desconhecida)";

void crash_timestamp(char* out, size_t cap) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    snprintf(out, cap, "%.0f", re_time_seconds());
#endif
}

#ifdef _WIN32
static int to_w(const char* s, wchar_t* out, int cap) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap); }
static int to_u8(const wchar_t* s, char* out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    if (n <= 0 && cap) out[0] = 0;
    return n;
}

static bool exe_dir(char* out, size_t cap) {
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (!n || n >= 1024) return false;
    while (n && w[n - 1] != L'\\' && w[n - 1] != L'/') n--;
    if (n) w[n - 1] = 0;
    return to_u8(w, out, (int)cap) > 0;
}

static bool dir_writable(const char* dir) {
    wchar_t w[1100];
    to_w(dir, w, 1024);
    CreateDirectoryW(w, NULL);
    wcscat(w, L"\\.realengine_write_test");
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
#endif

static const char* report_base(void) {
    if (!g_base[0]) {
#ifdef _WIN32
        if (!exe_dir(g_base, sizeof g_base) || !dir_writable(g_base)) {
            if (!re_config_dir(g_base, sizeof g_base)) copy_str(g_base, sizeof g_base, ".");
        }
#else
        copy_str(g_base, sizeof g_base, ".");
#endif
    }
    return g_base;
}

bool crash_reports_dir(const char* sub, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s", report_base(), sub);
    crash_native_slashes(out);
#ifdef _WIN32
    wchar_t w[1024];
    to_w(out, w, 1024);
    CreateDirectoryW(w, NULL);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return true;
#endif
}

#ifdef _WIN32
// Creates path exclusively; INVALID_HANDLE_VALUE when it exists.
static HANDLE create_new(const char* path) {
    wchar_t w[1100];
    to_w(path, w, 1100);
    return CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
}
#endif

static void unique_name(char* out, size_t cap, const char* dir, const char* prefix, const char* stamp, int n,
                        const char* ext) {
    if (n <= 1) snprintf(out, cap, "%s/%s_%s%s", dir, prefix, stamp, ext);
    else snprintf(out, cap, "%s/%s_%s_%d%s", dir, prefix, stamp, n, ext);
    crash_native_slashes(out);
}

bool crash_create_unique(const char* dir, const char* prefix, const char* stamp, const char* ext, char* path,
                         size_t cap) {
#ifdef _WIN32
    for (int n = 1; n < 10000; n++) {
        unique_name(path, cap, dir, prefix, stamp, n, ext);
        HANDLE h = create_new(path);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
#else
    (void)dir; (void)prefix; (void)stamp; (void)ext;
#endif
    if (cap) path[0] = 0;
    return false;
}

bool crash_take_pending(char* report_path, size_t cap) {
    if (cap) report_path[0] = 0;
    char dir[1100], marker[1200];
    snprintf(dir, sizeof dir, "%s/crashes", report_base());
    snprintf(marker, sizeof marker, "%s/ultimo_crash.txt", dir);
    FILE* f = re_fopen(marker, "rb");
    if (!f) return false;
    size_t n = fread(report_path, 1, cap ? cap - 1 : 0, f);
    fclose(f);
    if (cap) report_path[n] = 0;
    while (n && (report_path[n - 1] == '\n' || report_path[n - 1] == '\r')) report_path[--n] = 0;
    re_remove_file(marker);
    return n > 0;
}

void crash_open_folder(const char* folder, const char* select) {
#ifdef _WIN32
    wchar_t args[2200];
    wchar_t w[1100];
    if (select && select[0]) {
        to_w(select, w, 1100);
        for (wchar_t* c = w; *c; c++)
            if (*c == L'/') *c = L'\\';
        _snwprintf(args, 2200, L"/select,\"%ls\"", w);
    } else {
        to_w(folder, w, 1100);
        for (wchar_t* c = w; *c; c++)
            if (*c == L'/') *c = L'\\';
        _snwprintf(args, 2200, L"\"%ls\"", w);
    }
    args[2199] = 0;
    if (!g_platform_synthetic_input) ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
#else
    (void)folder; (void)select;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// System information (gathered once at install: nothing slow nor allocating at crash time)
// ---------------------------------------------------------------------------------------------------------------------

static char g_sysinfo[4096];
static uint64_t g_start_tick;
static uint32_t g_main_thread;

void crash_set_gpu(const char* name, uint32_t vendor_id, uint32_t driver_version, uint32_t api_version, uint64_t vram) {
    char drv[64];
    if (vendor_id == 0x10DE) // NVIDIA: 10.8.8.6 bits
        snprintf(drv, sizeof drv, "%u.%u", (driver_version >> 22) & 0x3ff, (driver_version >> 14) & 0xff);
    else if (vendor_id == 0x8086) // Intel on Windows: 18.14 bits
        snprintf(drv, sizeof drv, "%u.%u", driver_version >> 14, driver_version & 0x3fff);
    else
        snprintf(drv, sizeof drv, "%u.%u.%u", driver_version >> 22, (driver_version >> 12) & 0x3ff, driver_version & 0xfff);
    const char* vendor = vendor_id == 0x10DE ? "NVIDIA" : vendor_id == 0x1002 ? "AMD" : vendor_id == 0x8086 ? "Intel" : "?";
    snprintf(g_gpu, sizeof g_gpu, "%s (%s, id 0x%04X), driver %s, Vulkan %u.%u.%u, VRAM %.1f GB", name ? name : "?", vendor,
             vendor_id, drv, api_version >> 22, (api_version >> 12) & 0x3ff, api_version & 0xfff,
             (double)vram / (1024.0 * 1024.0 * 1024.0));
}

#ifdef _WIN32
static void reg_string(HKEY root, const wchar_t* key, const wchar_t* name, char* out, size_t cap) {
    wchar_t w[256];
    DWORD size = sizeof w;
    if (cap) out[0] = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, NULL, w, &size) == ERROR_SUCCESS) to_u8(w, out, (int)cap);
}

static DWORD reg_dword(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, size = sizeof v;
    RegGetValueW(root, key, name, RRF_RT_REG_DWORD, NULL, &v, &size);
    return v;
}

static void gather_sysinfo(void) {
    Buf b = { g_sysinfo, 0, sizeof g_sysinfo };
    // Windows: RtlGetVersion (GetVersionEx lies to unmanifested apps) and the registry's names
    RTL_OSVERSIONINFOW vi = { sizeof vi };
    typedef LONG(WINAPI * RtlGetVersionFn)(RTL_OSVERSIONINFOW*);
    RtlGetVersionFn rgv = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&vi);
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    char product[128], display[64];
    reg_string(HKEY_LOCAL_MACHINE, cv, L"ProductName", product, sizeof product);
    reg_string(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", display, sizeof display);
    DWORD ubr = reg_dword(HKEY_LOCAL_MACHINE, cv, L"UBR");
    if (vi.dwBuildNumber >= 22000 && !strncmp(product, "Windows 10", 10)) product[9] = '1'; // the registry keeps "10"
    bprintf(&b, "- **Sistema**: %s %s (%lu.%lu.%lu.%lu)\n", product[0] ? product : "Windows", display, vi.dwMajorVersion,
            vi.dwMinorVersion, vi.dwBuildNumber, ubr);
    // CPU: the brand string
    int regs[4];
    char brand[64] = "";
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            __cpuid(regs, 0x80000002 + i);
            memcpy(brand + i * 16, regs, 16);
        }
        brand[48] = 0;
    }
    char* bp = brand;
    while (*bp == ' ') bp++;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    bprintf(&b, "- **CPU**: %s, %lu threads\n", bp[0] ? bp : "?", si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    bprintf(&b, "- **RAM**: %.1f GB\n", (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    // Displays: the primary's size and DPI scale
    bprintf(&b, "- **Tela principal**: %d x %d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}
#endif

static void program_info(Buf* b) {
    bprintf(b, "- **Versão**: Rivora Engine %s, commit `%s`", RE_VERSION_STRING, RE_COMMIT);
#ifdef NDEBUG
    bput(b, ", build Release");
#else
    bput(b, ", build Debug");
#endif
#ifdef _WIN32
    // The exe's link time (PE header): the build the report comes from
    HMODULE exe = GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)exe + dos->e_lfanew);
    time_t linked = (time_t)nt->FileHeader.TimeDateStamp;
    struct tm tmv;
    if (localtime_s(&tmv, &linked) == 0)
        bprintf(b, " de %04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    bprintf(b, ", MSVC %d\n", _MSC_FULL_VER);
    wchar_t w[1024];
    char u[2048];
    GetModuleFileNameW(NULL, w, 1024);
    to_u8(w, u, sizeof u);
    bprintf(b, "- **Executável**: `%s` (processo %lu)\n", u, GetCurrentProcessId());
    to_u8(GetCommandLineW(), u, sizeof u);
    bprintf(b, "- **Linha de comando**: `%s`\n", u);
    uint64_t up = (GetTickCount64() - g_start_tick) / 1000;
    if (g_start_tick) bprintf(b, "- **Tempo aberto**: %llu min %llu s\n", up / 60, up % 60);
#else
    bput(b, "\n");
#endif
}

static void memory_info(Buf* b) {
#ifdef _WIN32
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    PROCESS_MEMORY_COUNTERS_EX pm = { sizeof pm };
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    bprintf(b, "- **Memória livre**: %.2f GB de RAM (%lu%% em uso), %.2f GB de commit\n", (double)ms.ullAvailPhys / gb,
            ms.dwMemoryLoad, (double)ms.ullAvailPageFile / gb);
    bprintf(b, "- **Memória do processo**: %.2f GB (pico %.2f GB), privada %.2f GB\n", (double)pm.WorkingSetSize / gb,
            (double)pm.PeakWorkingSetSize / gb, (double)pm.PrivateUsage / gb);
    if (ms.ullAvailPhys < ms.ullTotalPhys / 20 || ms.ullAvailPageFile < 512ull * 1024 * 1024)
        bput(b, "- **Atenção**: pouca memória livre (pode ser falta de memória)\n");
#else
    (void)b;
#endif
}

// The app's state through its hook, guarded: the app's memory may be what broke.
static void state_info(Buf* b) {
    if (!g_hooks.state) {
        bput(b, "(sem estado do app)\n");
        return;
    }
    static char state[8192];
    state[0] = 0;
#ifdef _WIN32
    __try {
        g_hooks.state(state, sizeof state, g_hooks.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(state + strlen(state), sizeof state - strlen(state), "\n(falha ao ler o estado: exceção 0x%08lX)\n",
                 GetExceptionCode());
    }
#else
    g_hooks.state(state, sizeof state, g_hooks.user);
#endif
    bput(b, state);
    if (b->n && b->p[b->n - 1] != '\n') bput(b, "\n");
}

size_t crash_context_markdown(char* out, size_t cap, unsigned sections) {
    Buf b = { out, 0, cap };
    if (cap) out[0] = 0;
    if (sections & CRASH_CTX_SYSTEM) {
        bput(&b, "## Programa e sistema\n\n");
        program_info(&b);
        bput(&b, g_sysinfo);
        bprintf(&b, "- **GPU**: %s\n", g_gpu);
        memory_info(&b);
        bput(&b, "\n");
    }
    if (sections & CRASH_CTX_STATE) {
        bput(&b, "## Estado do app\n\n");
        state_info(&b);
        bput(&b, "\n");
    }
    if (sections & CRASH_CTX_ACTIONS) {
        static char actions[CRASH_ACTIONS * (CRASH_ACTION_LEN + 24)];
        re_log_actions_dump(actions, sizeof actions, 50);
        bput(&b, "## Últimas ações (da mais antiga à mais nova)\n\n```text\n");
        bput(&b, actions[0] ? actions : "(nenhuma)\n");
        bput(&b, "```\n\n");
    }
    if (sections & CRASH_CTX_LOG) {
        static char log[CRASH_LOG_LINES * (CRASH_LOG_LINE_LEN + 32)];
        re_log_dump(log, sizeof log, 200);
        bput(&b, "## Log (últimas 200 linhas)\n\n```text\n");
        bput(&b, log[0] ? log : "(vazio)\n");
        bput(&b, "```\n\n");
        if (g_log_path[0]) bprintf(&b, "O log inteiro desta sessão: `%s`\n\n", g_log_path);
    }
    return b.n;
}

void crash_set_hooks(const CrashHooks* hooks) {
    if (hooks) g_hooks = *hooks;
    else memset(&g_hooks, 0, sizeof g_hooks);
}

bool crash_installed(void) { return g_installed; }

// ---------------------------------------------------------------------------------------------------------------------
// The report (Windows)
// ---------------------------------------------------------------------------------------------------------------------

#ifdef _WIN32

#define MAX_THREADS 256
#define MAX_FRAMES 64
#define MAX_CRASH_FRAMES 256

typedef struct ThreadStack {
    DWORD id;
    HANDLE handle;
    int count;
    bool ok;
    DWORD64 frames[MAX_FRAMES];
} ThreadStack;

typedef struct CrashRequest {
    const char* kind;
    char message[2048];
    DWORD thread;
    DWORD code;
    bool has_exception;
    EXCEPTION_RECORD record;
    EXCEPTION_POINTERS pointers;
} CrashRequest;

static CrashConfig g_cfg;
static CrashRequest g_req;
static __declspec(align(16)) CONTEXT g_ctx;
static __declspec(align(16)) CONTEXT g_walk;
static DWORD64 g_crash_frames[MAX_CRASH_FRAMES];
static int g_crash_frame_count;
static ThreadStack g_threads[MAX_THREADS];
static int g_thread_count;
static HANDLE g_ev_request, g_ev_done, g_writer;
static DWORD g_writer_id;
static volatile LONG g_crashing;
static volatile LONG g_phase; // writer progress (the crashed thread stops waiting on a hung emergency save)
static volatile ULONGLONG g_phase_start;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static char g_crash_dir[1024];
enum { PHASE_IDLE, PHASE_WRITING, PHASE_SAVING, PHASE_DIALOG, PHASE_DONE };

// File output through WriteFile and a static buffer (no CRT FILE, no heap)
typedef struct Out {
    HANDLE h;
    char buf[16384];
    size_t n;
} Out;
static Out g_out;

static void out_flush(Out* o) {
    DWORD w;
    if (o->n && o->h != INVALID_HANDLE_VALUE) WriteFile(o->h, o->buf, (DWORD)o->n, &w, NULL);
    o->n = 0;
}
static void out_write(Out* o, const char* s, size_t len) {
    while (len) {
        size_t room = sizeof o->buf - o->n;
        if (!room) {
            out_flush(o);
            continue;
        }
        size_t k = len < room ? len : room;
        memcpy(o->buf + o->n, s, k);
        o->n += k;
        s += k;
        len -= k;
    }
}
static void out_puts(Out* o, const char* s) { out_write(o, s, strlen(s)); }
static void out_printf(Out* o, const char* fmt, ...) {
    char line[2048];
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    if (w > 0) out_write(o, line, (size_t)w < sizeof line ? (size_t)w : sizeof line - 1);
}

// --- Unwinding (RtlVirtualUnwind: no heap, safe while other threads are suspended) ---
static int unwind_stack(CONTEXT* c, DWORD64* out, int max) {
    int n = 0;
    __try {
        while (n < max) {
            DWORD64 pc = c->Rip, sp = c->Rsp;
            if (!pc) break;
            out[n++] = pc;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, NULL);
            if (fn) {
                PVOID handler_data = NULL;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fn, c, &handler_data, &frame, NULL);
            } else { // leaf function: the return address is on top of the stack
                c->Rip = *(DWORD64*)c->Rsp;
                c->Rsp += 8;
            }
            if (c->Rsp < sp || (c->Rip == pc && c->Rsp == sp)) break; // no progress: a broken stack
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The other threads: suspended one at a time while their registers and frames are read (no allocation meanwhile).
static void collect_threads(void) {
    g_thread_count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof te };
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_writer_id || te.th32ThreadID == g_req.thread) continue;
            if (g_thread_count >= MAX_THREADS) break;
            ThreadStack* t = &g_threads[g_thread_count++];
            memset(t, 0, sizeof *t);
            t->id = te.th32ThreadID;
            t->handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   t->id);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    for (int i = 0; i < g_thread_count; i++) {
        ThreadStack* t = &g_threads[i];
        if (!t->handle) continue;
        if (SuspendThread(t->handle) == (DWORD)-1) continue;
        memset(&g_walk, 0, sizeof g_walk);
        g_walk.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->handle, &g_walk)) {
            t->count = unwind_stack(&g_walk, t->frames, MAX_FRAMES);
            t->ok = true;
        }
        ResumeThread(t->handle);
    }
}

// --- Symbols ---
static bool g_sym_ok;

static void sym_init(void) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS);
    wchar_t exe[1024];
    DWORD n = GetModuleFileNameW(NULL, exe, 1024);
    while (n && exe[n - 1] != L'\\') n--;
    if (n) exe[n - 1] = 0;
    g_sym_ok = SymInitializeW(GetCurrentProcess(), exe, TRUE) != FALSE;
}

// "src/mesh.c" from the full path the .pdb keeps
static const char* short_source(const char* path) {
    static const char* const roots[] = { "\\src\\", "\\tests\\", "\\external\\", "/src/", "/tests/", "/external/" };
    const char* best = NULL;
    for (int i = 0; i < 6; i++) {
        const char* p = path, *hit;
        while ((hit = strstr(p, roots[i])) != NULL) {
            best = hit + 1 > best ? hit + 1 : best;
            p = hit + 1;
        }
    }
    return best ? best : path;
}

static void module_of(DWORD64 addr, char* name, size_t cap, DWORD64* base) {
    HMODULE m = NULL;
    *base = 0;
    if (cap) name[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)addr, &m) && m) {
        wchar_t w[1024];
        DWORD n = GetModuleFileNameW(m, w, 1024);
        const wchar_t* b = w + n;
        while (b > w && b[-1] != L'\\') b--;
        to_u8(b, name, (int)cap);
        *base = (DWORD64)(uintptr_t)m;
    } else {
        copy_str(name, cap, "?");
    }
}

// One frame as text: "função + 0x1a  src/mesh.c:123  [rivora.exe+0x1234]". A return address is looked up one byte
// back (the call, not the next statement).
static void describe_frame(DWORD64 addr, bool is_return, char* out, size_t cap) {
    char mod[256];
    DWORD64 base;
    module_of(addr, mod, sizeof mod, &base);
    DWORD64 look = is_return && addr ? addr - 1 : addr;
    Buf b = { out, 0, cap };
    if (cap) out[0] = 0;
    bool named = false;
    if (g_sym_ok) {
        __try {
            char storage[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
            SYMBOL_INFOW* sym = (SYMBOL_INFOW*)storage;
            memset(sym, 0, sizeof(SYMBOL_INFOW));
            sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
            sym->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (SymFromAddrW(GetCurrentProcess(), look, &disp, sym)) {
                char name[1024];
                to_u8(sym->Name, name, sizeof name);
                bprintf(&b, "%s + 0x%llx", name, (unsigned long long)(addr - sym->Address));
                named = true;
            }
            IMAGEHLP_LINEW64 line = { sizeof line };
            DWORD ldisp = 0;
            if (SymGetLineFromAddrW64(GetCurrentProcess(), look, &ldisp, &line) && line.FileName) {
                char file[1024];
                to_u8(line.FileName, file, sizeof file);
                char* f = (char*)short_source(file);
                for (char* c = f; *c; c++)
                    if (*c == '\\') *c = '/';
                bprintf(&b, "  %s:%lu", f, line.LineNumber);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!named) bput(&b, "?");
    bprintf(&b, "  [%s+0x%llx]", mod, (unsigned long long)(addr - base));
}

static void write_frames(Out* o, const DWORD64* frames, int count, int max_shown) {
    out_puts(o, "```text\n");
    int shown = 0;
    for (int i = 0; i < count && shown < max_shown; i++) {
        int run = 1; // a recursion (stack overflow): equal consecutive frames shown once
        while (i + run < count && frames[i + run] == frames[i]) run++;
        char line[1600];
        describe_frame(frames[i], i > 0, line, sizeof line);
        if (run > 1) out_printf(o, "#%-3d %s   (repetido %d vezes)\n", i, line, run);
        else out_printf(o, "#%-3d %s\n", i, line);
        i += run - 1;
        shown++;
    }
    if (count == 0) out_puts(o, "(pilha indisponível)\n");
    else if (count >= MAX_CRASH_FRAMES) out_puts(o, "(... a pilha continua: os primeiros quadros acima)\n");
    out_puts(o, "```\n\n");
}

static void thread_name(HANDLE h, char* out, size_t cap) {
    if (cap) out[0] = 0;
    typedef HRESULT(WINAPI * GetDescFn)(HANDLE, PWSTR*);
    static GetDescFn fn;
    if (!fn) fn = (GetDescFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    PWSTR w = NULL;
    if (fn && h && SUCCEEDED(fn(h, &w)) && w) {
        to_u8(w, out, (int)cap);
        LocalFree(w);
    }
}

// --- What the exception was ---
static const char* exception_name(DWORD code, const char** ident) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: *ident = "EXCEPTION_ACCESS_VIOLATION"; return "Violação de acesso";
    case EXCEPTION_STACK_OVERFLOW: *ident = "EXCEPTION_STACK_OVERFLOW"; return "Estouro de pilha (recursão sem fim?)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: *ident = "EXCEPTION_INT_DIVIDE_BY_ZERO"; return "Divisão inteira por zero";
    case EXCEPTION_INT_OVERFLOW: *ident = "EXCEPTION_INT_OVERFLOW"; return "Estouro inteiro";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: *ident = "EXCEPTION_FLT_DIVIDE_BY_ZERO"; return "Divisão de ponto flutuante por zero";
    case EXCEPTION_FLT_INVALID_OPERATION: *ident = "EXCEPTION_FLT_INVALID_OPERATION"; return "Operação de ponto flutuante inválida";
    case EXCEPTION_ILLEGAL_INSTRUCTION: *ident = "EXCEPTION_ILLEGAL_INSTRUCTION"; return "Instrução ilegal";
    case EXCEPTION_PRIV_INSTRUCTION: *ident = "EXCEPTION_PRIV_INSTRUCTION"; return "Instrução privilegiada";
    case EXCEPTION_IN_PAGE_ERROR: *ident = "EXCEPTION_IN_PAGE_ERROR"; return "Erro de página (arquivo mapeado ou disco)";
    case EXCEPTION_DATATYPE_MISALIGNMENT: *ident = "EXCEPTION_DATATYPE_MISALIGNMENT"; return "Dado desalinhado";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: *ident = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"; return "Índice fora do array";
    case EXCEPTION_BREAKPOINT: *ident = "EXCEPTION_BREAKPOINT"; return "Ponto de parada (__debugbreak)";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: *ident = "EXCEPTION_NONCONTINUABLE_EXCEPTION"; return "Exceção não continuável";
    case EXCEPTION_INVALID_HANDLE: *ident = "EXCEPTION_INVALID_HANDLE"; return "Handle inválido";
    case 0xC0000374: *ident = "STATUS_HEAP_CORRUPTION"; return "Heap corrompido (escrita fora de um bloco, free duplo)";
    case 0xC0000409: *ident = "STATUS_STACK_BUFFER_OVERRUN"; return "Buffer da pilha estourado";
    case 0xC0000017: *ident = "STATUS_NO_MEMORY"; return "Falta de memória";
    case 0xE06D7363: *ident = "EXCEPTION_MSVC_CPP"; return "Exceção C++ não tratada";
    case 0x40000015: *ident = "STATUS_FATAL_APP_EXIT"; return "Erro fatal";
    default: *ident = "?"; return "Exceção";
    }
}

// "std::bad_alloc" from ".?AVbad_alloc@std@@"
static void demangle_type(const char* dec, char* out, size_t cap) {
    if (!strncmp(dec, ".?AV", 4) || !strncmp(dec, ".?AU", 4)) dec += 4;
    char parts[8][128];
    int np = 0;
    while (*dec && *dec != '@' && np < 8) {
        int k = 0;
        while (*dec && *dec != '@' && k < 127) parts[np][k++] = *dec++;
        parts[np++][k] = 0;
        if (*dec == '@') dec++;
    }
    Buf b = { out, 0, cap };
    if (cap) out[0] = 0;
    for (int i = np - 1; i >= 0; i--) {
        bput(&b, parts[i]);
        if (i) bput(&b, "::");
    }
}

// An MSVC C++ exception: its type (ThrowInfo) and, for std::exception, what().
static void cxx_exception_info(const EXCEPTION_RECORD* r, char* out, size_t cap) {
    if (cap) out[0] = 0;
    if (r->NumberParameters < 4 || r->ExceptionInformation[0] != 0x19930520) return;
    __try {
        const char* base = (const char*)r->ExceptionInformation[3];
        const int* throw_info = (const int*)r->ExceptionInformation[2];
        const int* catchables = (const int*)(base + throw_info[3]);
        int count = catchables[0];
        bool is_std = false;
        char type[256] = "";
        for (int i = 0; i < count && i < 16; i++) {
            const int* ct = (const int*)(base + catchables[1 + i]);
            const char* name = base + ct[1] + 16; // TypeDescriptor: vftable, spare, name
            if (i == 0) demangle_type(name, type, sizeof type);
            if (!strcmp(name, ".?AVexception@std@@")) is_std = true;
        }
        Buf b = { out, 0, cap };
        bprintf(&b, "tipo `%s`", type[0] ? type : "?");
        if (is_std) { // std::exception: vptr, then __std_exception_data { const char* what; bool free; }
            const char* what = *(const char* const*)((const char*)r->ExceptionInformation[1] + sizeof(void*));
            if (what) bprintf(&b, ", what(): \"%.400s\"", what);
        }
        if (strstr(type, "bad_alloc")) bput(&b, " (falta de memória)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void write_registers(Out* o, const CONTEXT* c) {
    out_printf(o, "```text\nRIP %016llX  RSP %016llX  RBP %016llX  EFLAGS %08lX\n", c->Rip, c->Rsp, c->Rbp, c->EFlags);
    out_printf(o, "RAX %016llX  RBX %016llX  RCX %016llX  RDX %016llX\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    out_printf(o, "RSI %016llX  RDI %016llX  R8  %016llX  R9  %016llX\n", c->Rsi, c->Rdi, c->R8, c->R9);
    out_printf(o, "R10 %016llX  R11 %016llX  R12 %016llX  R13 %016llX\n", c->R10, c->R11, c->R12, c->R13);
    out_printf(o, "R14 %016llX  R15 %016llX\n```\n\n", c->R14, c->R15);
}

static void write_modules(Out* o) {
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > 512) n = 512;
    out_puts(o, "## Módulos carregados\n\n```text\n");
    for (int i = 0; i < n; i++) {
        MODULEINFO mi;
        wchar_t w[1024];
        char u[2048];
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        GetModuleFileNameW(mods[i], w, 1024);
        to_u8(w, u, sizeof u);
        out_printf(o, "%016llX %9lu KB  %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage / 1024, u);
    }
    out_puts(o, "```\n\n");
}

static bool write_minidump(HANDLE h) {
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = g_req.thread;
    mei.ExceptionPointers = &g_req.pointers;
    mei.ClientPointers = FALSE;
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                         MiniDumpWithUnloadedModules | MiniDumpScanMemory);
    BOOL ok = FALSE;
    __try {
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type, &mei, NULL, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    return ok != FALSE;
}

static void set_phase(LONG p) {
    g_phase_start = GetTickCount64();
    InterlockedExchange(&g_phase, p);
}

static const char* file_part(const char* path) {
    const char* s = path;
    for (const char* c = path; *c; c++)
        if (*c == '/' || *c == '\\') s = c + 1;
    return s;
}

// Opens <crashes>/RealEngine_crash_<stamp>[_n].txt and the .dmp of the same name, both created new.
static bool open_report_files(char* txt, char* dmp, char* stem, size_t cap, HANDLE* ht, HANDLE* hd) {
    char stamp[64];
    crash_timestamp(stamp, sizeof stamp);
    for (int pass = 0; pass < 2; pass++) {
        const char* dir = g_crash_dir;
        char tmp[1024];
        if (pass == 1) { // the folder went away or became read-only: %TEMP%
            wchar_t w[MAX_PATH];
            GetTempPathW(MAX_PATH, w);
            to_u8(w, tmp, sizeof tmp);
            size_t l = strlen(tmp);
            if (l && (tmp[l - 1] == '\\' || tmp[l - 1] == '/')) tmp[l - 1] = 0;
            dir = tmp;
        }
        for (int n = 1; n < 10000; n++) {
            unique_name(txt, cap, dir, "RealEngine_crash", stamp, n, ".txt");
            unique_name(dmp, cap, dir, "RealEngine_crash", stamp, n, ".dmp");
            *ht = create_new(txt);
            if (*ht == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
                break; // the folder can't be written
            }
            *hd = g_cfg.minidump ? create_new(dmp) : INVALID_HANDLE_VALUE;
            if (g_cfg.minidump && *hd == INVALID_HANDLE_VALUE) { // a .dmp of that name exists: next number
                CloseHandle(*ht);
                wchar_t w[1100];
                to_w(txt, w, 1100);
                DeleteFileW(w);
                continue;
            }
            copy_str(stem, cap, txt);
            stem[strlen(stem) - 4] = 0;
            return true;
        }
    }
    return false;
}

static void write_report(void) {
    set_phase(PHASE_WRITING);
    static char txt[1100], dmp[1100], stem[1100];
    HANDLE ht = INVALID_HANDLE_VALUE, hd = INVALID_HANDLE_VALUE;
    // 1. The other threads' stacks first: suspending them while nothing here allocates
    __try {
        collect_threads();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // The crashed thread's own frames, from its context
    memcpy(&g_walk, &g_ctx, sizeof g_walk);
    g_crash_frame_count = unwind_stack(&g_walk, g_crash_frames, MAX_CRASH_FRAMES);
    if (!open_report_files(txt, dmp, stem, sizeof txt, &ht, &hd)) return;
    // The marker the app finds at its next start (its own file: the report is never overwritten)
    {
        char marker[1200];
        snprintf(marker, sizeof marker, "%s/ultimo_crash.txt", g_crash_dir);
        wchar_t w[1200];
        to_w(marker, w, 1200);
        HANDLE hm = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hm != INVALID_HANDLE_VALUE) {
            DWORD wr;
            WriteFile(hm, txt, (DWORD)strlen(txt), &wr, NULL);
            CloseHandle(hm);
        }
    }
    // Lines still in the stdout/stderr pipes (an assert message printed just before abort)
    for (int i = 0; i < 20 && re_atomic_load(&g_capturing); i++) Sleep(5);
    __try {
        sym_init();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_sym_ok = false;
    }

    Out* o = &g_out;
    o->h = ht;
    o->n = 0;
    char stamp[64];
    crash_timestamp(stamp, sizeof stamp);
    stamp[10] = ' ';
    stamp[13] = stamp[16] = ':';
    out_puts(o, "# Rivora Engine: relatório de crash\n\n");
    out_puts(o, "> Para corrigir: \"Onde\" e \"Pilha da thread que travou\" dizem a função e a linha; \"Últimas ações\" e "
                "\"Log\" dizem o que o usuário fazia. O `.dmp` de mesmo nome abre no Visual Studio ou no WinDbg com o "
                "`rivora.pdb` da mesma build (variáveis e memória).\n\n");
    // --- Summary ---
    __try {
        const char* ident = "";
        const char* name = g_req.kind ? g_req.kind : exception_name(g_req.code, &ident);
        out_printf(o, "- **Quando**: %s\n", stamp);
        if (g_req.has_exception) {
            exception_name(g_req.code, &ident);
            out_printf(o, "- **Tipo**: %s (%s, 0x%08lX)\n", name, ident, g_req.code);
            if (g_req.code == EXCEPTION_ACCESS_VIOLATION || g_req.code == EXCEPTION_IN_PAGE_ERROR) {
                ULONG_PTR rw = g_req.record.NumberParameters > 0 ? g_req.record.ExceptionInformation[0] : 0;
                ULONG_PTR at = g_req.record.NumberParameters > 1 ? g_req.record.ExceptionInformation[1] : 0;
                const char* what = rw == 0 ? "leitura" : rw == 1 ? "escrita" : rw == 8 ? "execução" : "acesso";
                out_printf(o, "- **Acesso**: %s no endereço 0x%016llX%s\n", what, (unsigned long long)at,
                           at < 0x10000 ? " (ponteiro nulo + deslocamento)" : "");
            }
            if (g_req.code == 0xE06D7363) {
                char cxx[600];
                cxx_exception_info(&g_req.record, cxx, sizeof cxx);
                if (cxx[0]) out_printf(o, "- **Exceção C++**: %s\n", cxx);
            }
        } else {
            out_printf(o, "- **Tipo**: %s\n", name);
        }
        if (g_req.message[0]) out_printf(o, "- **Mensagem**: %s\n", g_req.message);
        DWORD64 at = g_req.has_exception ? (DWORD64)(uintptr_t)g_req.record.ExceptionAddress : g_ctx.Rip;
        char where[1600], own[1600];
        describe_frame(at, false, where, sizeof where);
        // The first frame in Real Engine's own code: for a report from code (abort, assert) the one past the handlers;
        // for an exception in a system or driver module, the caller in Real Engine, on a line of its own.
        DWORD64 exe_base = (DWORD64)(uintptr_t)GetModuleHandleW(NULL);
        own[0] = 0;
        for (int i = 0; i < g_crash_frame_count; i++) {
            char mod[256];
            DWORD64 base;
            module_of(g_crash_frames[i], mod, sizeof mod, &base);
            if (base != exe_base) continue;
            describe_frame(g_crash_frames[i], i > 0, own, sizeof own);
            if (!g_req.has_exception &&
                (strstr(own, "crashlog.c") || strstr(own, "crash_now") || strstr(own, "crash_fatal") ||
                 strstr(own, "on_abort") || strstr(own, "re_imgui_assert_fail") || strstr(own, "vk_check"))) {
                own[0] = 0;
                continue;
            }
            break;
        }
        if (!g_req.has_exception && own[0]) {
            out_printf(o, "- **Onde**: `%s`\n", own);
        } else {
            out_printf(o, "- **Onde**: `%s`\n", where);
            if (own[0] && strcmp(own, where)) out_printf(o, "- **No código da Rivora Engine**: `%s`\n", own);
        }
        char tname[256];
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_req.thread);
        thread_name(th, tname, sizeof tname);
        if (th) CloseHandle(th);
        out_printf(o, "- **Thread**: %lu%s\n", g_req.thread,
                   tname[0] ? "" : (g_req.thread == g_main_thread ? " (principal)" : ""));
        if (tname[0]) out_printf(o, "- **Nome da thread**: %s\n", tname);
        out_printf(o, "- **Minidump**: %s\n", g_cfg.minidump ? file_part(dmp) : "(desligado)");
        out_printf(o, "- **Cena salva**: veja \"Salvamento de emergência\" no fim\n\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_puts(o, "\n(falha ao escrever o resumo)\n\n");
    }
    out_flush(o);
    // --- The crashed thread ---
    __try {
        out_puts(o, "## Pilha da thread que travou\n\n");
        write_frames(o, g_crash_frames, g_crash_frame_count, 96);
        out_puts(o, "## Registradores\n\n");
        write_registers(o, &g_ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_puts(o, "\n(falha ao escrever a pilha)\n\n");
    }
    out_flush(o);
    // --- Context: program, system, state, actions, log ---
    __try {
        static char ctx[160 * 1024];
        size_t n = crash_context_markdown(ctx, sizeof ctx, CRASH_CTX_ALL);
        out_write(o, ctx, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_puts(o, "\n(falha ao escrever o contexto)\n\n");
    }
    out_flush(o);
    // --- The other threads ---
    __try {
        out_printf(o, "## Outras threads (%d)\n\n", g_thread_count);
        for (int i = 0; i < g_thread_count; i++) {
            ThreadStack* t = &g_threads[i];
            char tname[256];
            thread_name(t->handle, tname, sizeof tname);
            if (tname[0]) out_printf(o, "### Thread %lu: %s\n\n", t->id, tname);
            else out_printf(o, "### Thread %lu%s\n\n", t->id, t->id == g_main_thread ? " (principal)" : "");
            if (!t->ok) out_puts(o, "(não deu para ler)\n\n");
            else write_frames(o, t->frames, t->count, 40);
            out_flush(o);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_puts(o, "\n(falha ao escrever as threads)\n\n");
    }
    for (int i = 0; i < g_thread_count; i++)
        if (g_threads[i].handle) CloseHandle(g_threads[i].handle);
    __try {
        write_modules(o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out_flush(o);
    // --- Minidump ---
    if (hd != INVALID_HANDLE_VALUE) {
        bool ok = write_minidump(hd);
        CloseHandle(hd);
        if (!ok) out_printf(o, "## Minidump\n\nFalhou (erro %lu).\n\n", GetLastError());
    }
    out_flush(o);
    // --- Emergency save (last: it may hang on a lock the frozen thread holds; the crashed thread stops waiting) ---
    out_puts(o, "## Salvamento de emergência\n\n");
    out_flush(o);
    if (g_hooks.emergency_save) {
        char scene[1200];
        snprintf(scene, sizeof scene, "%s_level.rlevel", stem);
        set_phase(PHASE_SAVING);
        bool ok = false;
        __try {
            ok = g_hooks.emergency_save(scene, g_hooks.user);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        set_phase(PHASE_WRITING);
        out_printf(o, ok ? "A cena foi salva em `%s` (pode estar incompleta: abra uma cópia).\n" : "Não foi possível salvar a cena.\n",
                   file_part(scene));
    } else {
        out_puts(o, "(sem salvamento)\n");
    }
    out_flush(o);
    CloseHandle(ht);
    if (g_cfg.dialog) {
        set_phase(PHASE_DIALOG);
        char msg[2400];
        snprintf(msg, sizeof msg, "%s\n\n%s\n%s\n\n%s", TR("Rivora Engine crashed and must close."),
                 TR("A report was saved at:"), txt, TR("Open the reports folder?"));
        wchar_t wmsg[2400], wtitle[128];
        to_w(msg, wmsg, 2400);
        to_w(TR("Rivora Engine: crash"), wtitle, 128);
        if (MessageBoxW(NULL, wmsg, wtitle, MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
            crash_open_folder(g_crash_dir, txt);
    }
    set_phase(PHASE_DONE);
}

static DWORD WINAPI writer_main(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(g_ev_request, INFINITE);
    write_report();
    SetEvent(g_ev_done);
    return 0;
}

// Hands the crash to the writer thread and ends the process. ep NULL: a report from code (the context is this
// thread's, here).
static void crash_now(const char* kind, const char* message, EXCEPTION_POINTERS* ep, DWORD exit_code) {
    DWORD me = GetCurrentThreadId();
    if (!g_installed || me == g_writer_id) TerminateProcess(GetCurrentProcess(), exit_code ? exit_code : 3);
    if (InterlockedCompareExchange(&g_crashing, 1, 0) != 0) {
        Sleep(INFINITE); // another thread is being reported: the writer ends the process
    }
    g_req.kind = kind;
    copy_str(g_req.message, sizeof g_req.message, message);
    g_req.thread = me;
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        g_req.has_exception = true;
        g_req.record = *ep->ExceptionRecord;
        g_req.code = ep->ExceptionRecord->ExceptionCode;
        memcpy(&g_ctx, ep->ContextRecord, sizeof g_ctx);
    } else {
        RtlCaptureContext(&g_ctx);
        memset(&g_req.record, 0, sizeof g_req.record);
        g_req.record.ExceptionCode = 0x40000015; // STATUS_FATAL_APP_EXIT, as abort() raises
        g_req.record.ExceptionAddress = (PVOID)(uintptr_t)g_ctx.Rip;
        g_req.code = 0x40000015;
    }
    g_req.pointers.ExceptionRecord = &g_req.record;
    g_req.pointers.ContextRecord = &g_ctx;
    SetEvent(g_ev_request);
    ULONGLONG start = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(g_ev_done, 250) == WAIT_OBJECT_0) break;
        ULONGLONG now = GetTickCount64();
        LONG phase = g_phase;
        if (phase == PHASE_DIALOG) continue; // the user reads the message
        if (phase == PHASE_SAVING && now - g_phase_start > 20000) break; // a hung save: the report is written
        if (now - start > 120000) break;
    }
    TerminateProcess(GetCurrentProcess(), exit_code ? exit_code : g_req.code);
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    crash_now(NULL, "", ep, ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

// First chance: what may never reach the unhandled filter (a stack overflow leaves no room to unwind; heap
// corruption is reported by the heap manager and ends the process). Everything else is left to its handlers (drivers
// probe memory with caught exceptions).
static LONG WINAPI on_vectored(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374 || code == 0xC0000409)
        crash_now(NULL, "", ep, code);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void on_abort(int sig) {
    (void)sig;
    crash_now("abort() foi chamado", "Um assert da CRT ou um erro fatal de uma biblioteca (veja o fim do log).", NULL, 3);
}

static void on_invalid_parameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line,
                                 uintptr_t reserved) {
    (void)reserved;
    char msg[1200] = "Uma função da CRT recebeu um parâmetro inválido";
    if (func) {
        char e[400] = "", f[200] = "", fl[300] = "";
        if (expr) to_u8(expr, e, sizeof e);
        to_u8(func, f, sizeof f);
        if (file) to_u8(file, fl, sizeof fl);
        snprintf(msg, sizeof msg, "%s: %s em %s (%s:%u)", "Parâmetro inválido", e, f, fl, line);
    }
    crash_now("Parâmetro inválido numa função da CRT", msg, NULL, 0xC000000D);
}

static void on_purecall(void) {
    crash_now("Chamada de função virtual pura", "Um objeto C++ foi usado durante a construção ou depois de destruído.",
              NULL, 0xC0000025);
}
#endif // _WIN32

#ifndef _WIN32
static CrashConfig g_cfg;
#endif
void crash_set_dialog(bool on) { g_cfg.dialog = on; }

void crash_install(const CrashConfig* cfg) {
#ifdef _WIN32
    if (g_installed) return;
    g_cfg = cfg ? *cfg : (CrashConfig){ NULL, true, true };
    if (g_cfg.dir && g_cfg.dir[0]) copy_str(g_base, sizeof g_base, g_cfg.dir);
    else g_base[0] = 0;
    report_base();
    crash_reports_dir("crashes", g_crash_dir, sizeof g_crash_dir);
    g_start_tick = GetTickCount64();
    g_main_thread = GetCurrentThreadId();
    gather_sysinfo();
    g_ev_request = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ev_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_writer = CreateThread(NULL, 1 << 20, writer_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_writer_id);
    typedef HRESULT(WINAPI * SetDescFn)(HANDLE, PCWSTR);
    SetDescFn sd = (SetDescFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (sd) {
        sd(g_writer, L"relatório de crash");
        sd(GetCurrentThread(), L"principal");
    }
    ULONG guarantee = 64 * 1024; // room for the handler when the main thread's stack overflows
    SetThreadStackGuarantee(&guarantee);
    g_installed = true;
    AddVectoredExceptionHandler(1, on_vectored);
    g_prev_filter = SetUnhandledExceptionFilter(on_unhandled);
    signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // no Windows "stopped working" box over ours
#else
    (void)cfg;
#endif
}

void crash_tick(void) {
#ifdef _WIN32
    if (!g_installed) return;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(on_unhandled);
    if (prev != on_unhandled) {
        char mod[256];
        DWORD64 base;
        module_of((DWORD64)(uintptr_t)prev, mod, sizeof mod, &base);
        re_log(RE_LOG_WARN, "o filtro de exceções foi trocado por %s: restaurado", prev ? mod : "NULL");
    }
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// The hang watchdog. The 0.17's "crash" as the loading screen ended was the editor frozen: Windows' "not responding",
// closed by the user, no report. The main loop beats once a pass (crash_heartbeat); stopped for the limit, a thread reads
// where the main thread is (suspended only for its registers and frames, nothing allocated meanwhile), writes it on the
// log and in crashes/RealEngine_travamento_*.txt, once more if it is still stopped at 4x, and lets it run.
// ---------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
static volatile LONG64 g_beat;
static HANDLE g_main_handle;
static DWORD g_watch_ms;
static char g_hang_path[1100];

void crash_heartbeat(void) { InterlockedExchange64(&g_beat, (LONG64)GetTickCount64()); }

static void hang_report(ULONGLONG stalled_ms, int shot) {
    static DWORD64 frames[MAX_FRAMES];
    static __declspec(align(16)) CONTEXT c;
    int n = 0;
    if (SuspendThread(g_main_handle) != (DWORD)-1) {
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        __try {
            if (GetThreadContext(g_main_handle, &c)) n = unwind_stack(&c, frames, MAX_FRAMES);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        ResumeThread(g_main_handle);
    }
    if (!g_sym_ok) sym_init();
    static char text[48 * 1024];
    Buf b = { text, 0, sizeof text };
    text[0] = 0;
    char stamp[64];
    crash_timestamp(stamp, sizeof stamp);
    bput(&b, "# Rivora Engine: travamento (a janela parou de responder)\n\n");
    program_info(&b);
    bprintf(&b, "- **Quando**: %s\n- **Thread principal parada há**: %.1f s%s\n", stamp, (double)stalled_ms / 1000.0,
            shot > 1 ? " (segunda leitura: ainda no mesmo quadro)" : "");
    memory_info(&b);
    bput(&b, "\n## Onde a thread principal está\n\n```text\n");
    re_log(RE_LOG_WARN, "Travamento: a thread principal está parada há %.1f s em:", (double)stalled_ms / 1000.0);
    for (int i = 0; i < n && i < 40; i++) {
        char line[1600];
        describe_frame(frames[i], i > 0, line, sizeof line);
        bprintf(&b, "#%-3d %s\n", i, line);
        if (i < 16) re_log(RE_LOG_WARN, "  #%d %s", i, line);
    }
    if (!n) bput(&b, "(pilha indisponível)\n");
    bput(&b, "```\n\n## Últimas linhas do log\n\n```text\n");
    if (b.n + 1 < b.cap) b.n += re_log_dump(text + b.n, b.cap - b.n, 80);
    bput(&b, "```\n");
    char path[1100];
    if (crash_create_unique(g_crash_dir, "RealEngine_travamento", stamp, ".txt", path, sizeof path)) {
        FILE* f = re_fopen(path, "wb");
        if (f) {
            fwrite(text, 1, b.n, f);
            fclose(f);
            copy_str(g_hang_path, sizeof g_hang_path, path);
        }
    }
}

static DWORD WINAPI watch_main(LPVOID arg) {
    (void)arg;
    LONG64 seen = 0;
    int shots = 0;
    for (;;) {
        Sleep(500);
        LONG64 beat = g_beat;
        if (!beat || g_crashing) continue;
        ULONGLONG stalled = GetTickCount64() - (ULONGLONG)beat;
        if (beat != seen) seen = beat, shots = 0; // a new stop (or none)
        if (stalled < g_watch_ms || shots >= 2 || (shots == 1 && stalled < 4ull * g_watch_ms)) continue;
        hang_report(stalled, ++shots);
    }
}
#endif

void crash_watchdog_start(double seconds) {
#ifdef _WIN32
    if (!g_installed || g_main_handle || seconds <= 0.0) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    g_watch_ms = (DWORD)(seconds * 1000.0);
    crash_heartbeat();
    HANDLE t = CreateThread(NULL, 256 * 1024, watch_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) return;
    typedef HRESULT(WINAPI * SetDescFn)(HANDLE, PCWSTR);
    SetDescFn sd = (SetDescFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (sd) sd(t, L"vigia de travamento");
    CloseHandle(t);
#else
    (void)seconds;
#endif
}
#ifndef _WIN32
void crash_heartbeat(void) {}
const char* crash_last_hang_report(void) { return ""; }
#else
const char* crash_last_hang_report(void) { return g_hang_path; }
#endif

void crash_fatal(const char* kind, const char* fmt, ...) {
    char msg[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt ? fmt : "", a);
    va_end(a);
    re_log(RE_LOG_ERROR, "%s: %s", kind ? kind : "erro fatal", msg);
#ifdef _WIN32
    if (g_installed) crash_now(kind ? kind : "Erro fatal", msg, NULL, 0xE0000001);
#endif
    fflush(stderr);
    abort();
}

// (item 21) the UI sweep's (ui_sweep.c sets it while it runs: an assert recorded on the control, the run goes on)
bool (*g_ui_sweep_assert_hook)(const char* expr, const char* file, int line);
void re_imgui_assert_fail(const char* expr, const char* file, int line) {
    if (g_ui_sweep_assert_hook && g_ui_sweep_assert_hook(expr, file, line)) return;
    const char* f = file ? file : "?";
    for (const char* c = f; *c; c++)
        if (*c == '/' || *c == '\\') f = c + 1;
    crash_fatal("Assert do ImGui", "IM_ASSERT(%s) falhou em %s:%d", expr ? expr : "?", f, line);
}

// ---------------------------------------------------------------------------------------------------------------------
// Crashes on purpose (tests)
// ---------------------------------------------------------------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4717) // recursive on all control paths: the point
#define CRASH_NOINLINE __declspec(noinline)
#else
#define CRASH_NOINLINE __attribute__((noinline))
#endif

static CRASH_NOINLINE void crash_test_access_violation(void) {
    volatile int* p = (volatile int*)(uintptr_t)0x10;
    *p = 42;
}

static CRASH_NOINLINE int crash_test_divide(int d) {
    volatile int den = d, num = 10;
    return num / den;
}

static CRASH_NOINLINE int crash_test_recurse(volatile char* prev, int depth) {
    volatile char buf[2048];
    buf[0] = (char)depth;
    buf[1] = prev ? prev[0] : 0;
    return crash_test_recurse(buf, depth + 1) + buf[1]; // not a tail call
}

static CRASH_NOINLINE void crash_test_invalid_parameter(void) {
    char small[4];
    const char* volatile text = "longo demais para caber";
    strcpy_s(small, sizeof small, text);
}

#ifdef _WIN32
int __cdecl _purecall(void);
#endif

static volatile uint32_t g_test_main_thread;
static CRASH_NOINLINE void crash_test_pool_item(void* arg, int index) {
    (void)arg;
    (void)index;
    if (re_thread_id() != g_test_main_thread) crash_test_access_violation();
    re_sleep_seconds(0.002);
}

static CRASH_NOINLINE int crash_test_thread_main(void* arg) {
    (void)arg;
    crash_test_access_violation();
    return 0;
}

void crash_test(const char* kind) {
    if (!kind) return;
    re_log(RE_LOG_WARN, "crash de teste: %s", kind);
    if (!strcmp(kind, "acesso")) crash_test_access_violation();
    else if (!strcmp(kind, "divisao")) printf("%d\n", crash_test_divide(0));
    else if (!strcmp(kind, "abort")) abort();
    else if (!strcmp(kind, "pilha")) printf("%d\n", crash_test_recurse(NULL, 0));
    else if (!strcmp(kind, "parametro")) crash_test_invalid_parameter();
#ifdef _WIN32
    else if (!strcmp(kind, "purecall")) _purecall();
#endif
    else if (!strcmp(kind, "heap")) {
        char* volatile p = malloc(64);
        free(p);
        free(p);
        crash_test_access_violation(); // a double free the heap did not notice
    } else if (!strcmp(kind, "thread")) {
        re_thread_join(re_thread_start(crash_test_thread_main, NULL));
    } else if (!strcmp(kind, "pool")) {
        g_test_main_thread = re_thread_id();
        re_parallel_for(256, crash_test_pool_item, NULL);
        re_thread_join(re_thread_start(crash_test_thread_main, NULL)); // the caller took every item
    } else if (!strcmp(kind, "imgui")) {
        re_imgui_assert_fail("teste && \"assert de teste\"", __FILE__, __LINE__);
    } else if (!strcmp(kind, "gpu")) {
        crash_fatal("A GPU parou de responder (VK_ERROR_DEVICE_LOST)", "VK_ERROR_DEVICE_LOST (-4) em vkQueueSubmit (teste)");
    }
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

void crash_open_file(const char* path) {
#ifdef _WIN32
    wchar_t w[1100];
    to_w(path, w, 1100);
    for (wchar_t* c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    if (!g_platform_synthetic_input) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}
