#include "platform_files.h"
#include "rpak.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <share.h>
#include <windows.h>

#include <shellapi.h>

static bool wide(const char* utf8, wchar_t* out, int cap) {
    return utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}

static int64_t filetime_unix(FILETIME ft) {
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime; // 100 ns since 1601
    return (int64_t)(t / 10000000ull) - 11644473600ll;
}

bool re_dir_walk(const char* dir, void (*fn)(void* user, const ReDirEntry* e), void* user) {
    if (rpak_mounted() && rpak_walk(dir, fn, user)) return true; // (B15a: the packaged game's Content.rpak)
    wchar_t w[1100];
    if (!wide(dir && dir[0] ? dir : ".", w, 1090)) return false;
    size_t n = wcslen(w);
    if (n > 0 && w[n - 1] != L'\\' && w[n - 1] != L'/') w[n++] = L'\\';
    w[n++] = L'*';
    w[n] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        ReDirEntry e = { name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                         (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, filetime_unix(fd.ftLastWriteTime) };
        fn(user, &e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool re_file_stat(const char* path, uint64_t* size, int64_t* mtime, bool* is_dir) {
    if (rpak_mounted() && rpak_has(path, size, is_dir)) {
        if (mtime) *mtime = 0;
        return true;
    }
    wchar_t w[1100];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!wide(path, w, 1100) || !GetFileAttributesExW(w, GetFileExInfoStandard, &a)) return false;
    if (size) *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    if (mtime) *mtime = filetime_unix(a.ftLastWriteTime);
    if (is_dir) *is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

bool re_make_dirs(const char* path) {
    wchar_t w[1100];
    if (!wide(path, w, 1100)) return false;
    for (wchar_t* p = w; *p; p++) {
        if ((*p != L'\\' && *p != L'/') || p == w || p[-1] == L':') continue;
        wchar_t c = *p;
        *p = 0;
        CreateDirectoryW(w, NULL);
        *p = c;
    }
    CreateDirectoryW(w, NULL);
    DWORD attr = GetFileAttributesW(w);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool re_move_path(const char* src, const char* dst) {
    wchar_t a[1100], b[1100];
    return wide(src, a, 1100) && wide(dst, b, 1100) && MoveFileExW(a, b, MOVEFILE_COPY_ALLOWED) != 0;
}

bool re_trash_path(const char* path) {
    wchar_t w[1102];
    if (!wide(path, w, 1100)) return false;
    for (wchar_t* p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    w[wcslen(w) + 1] = 0; // double terminated, as SHFileOperation wants
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = w;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

FILE* re_fopen_share(const char* path, const char* mode, bool deny_write) {
    wchar_t w[1100], m[16];
    if (!wide(path, w, 1100) || !wide(mode, m, 16)) return NULL;
    return _wfsopen(w, m, deny_write ? _SH_DENYWR : _SH_DENYNO);
}

struct ReFileMap {
    HANDLE file, map;
    const void* view;
    uint8_t* mem; // (B15a: a file from the pack, decompressed: freed with the map)
};

ReFileMap* re_map_file(const char* path, const uint8_t** data, uint64_t* size) {
    if (rpak_mounted()) {
        uint64_t n = 0;
        uint8_t* b = rpak_read(path, &n);
        if (b) {
            ReFileMap* m = calloc(1, sizeof *m);
            if (!m || !n) { free(b), free(m); return NULL; }
            m->mem = b, *data = b, *size = n;
            return m;
        }
    }
    wchar_t w[1100];
    if (!wide(path, w, 1100)) return NULL;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        CloseHandle(f);
        return NULL;
    }
    HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    const void* v = mp ? MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0) : NULL;
    ReFileMap* m = v ? malloc(sizeof *m) : NULL;
    if (!m) {
        if (v) UnmapViewOfFile(v);
        if (mp) CloseHandle(mp);
        CloseHandle(f);
        return NULL;
    }
    m->file = f;
    m->map = mp;
    m->view = v;
    m->mem = NULL;
    *data = v;
    *size = (uint64_t)sz.QuadPart;
    return m;
}

void re_unmap_file(ReFileMap* m) {
    if (!m) return;
    if (m->mem) { free(m->mem), free(m); return; }
    UnmapViewOfFile(m->view);
    CloseHandle(m->map);
    CloseHandle(m->file);
    free(m);
}

bool re_copy_file(const char* src, const char* dst) {
    wchar_t a[1100], b[1100];
    return wide(src, a, 1100) && wide(dst, b, 1100) && CopyFileW(a, b, TRUE) != 0;
}

bool re_exe_dir(char* out, size_t cap) {
    wchar_t w[1100];
    DWORD n = GetModuleFileNameW(NULL, w, 1100);
    if (n == 0 || n >= 1100) return false;
    while (n > 0 && w[n - 1] != L'\\' && w[n - 1] != L'/') n--;
    w[n > 0 ? n - 1 : 0] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

int re_run_command(const char* cmdline, const char* log_path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
    if (n <= 0) return -1;
    wchar_t* cmd = malloc(sizeof(wchar_t) * (size_t)n); // (a tool's run, not a frame)
    if (!cmd) return -1;
    MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, cmd, n);
    HANDLE out = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    wchar_t wl[1100];
    if (log_path && log_path[0] && wide(log_path, wl, 1100))
        out = CreateFileW(wl, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    if (out != INVALID_HANDLE_VALUE) si.dwFlags = STARTF_USESTDHANDLES, si.hStdOutput = si.hStdError = out, si.hStdInput = NULL;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, out != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    int code = -1;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0;
        if (GetExitCodeProcess(pi.hProcess, &ec)) code = (int)ec;
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return code;
}

void re_show_in_explorer(const char* path) {
    wchar_t w[1100], args[1200];
    if (!wide(path, w, 1100)) return;
    for (wchar_t* p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    DWORD attr = GetFileAttributesW(w);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    } else { // a file: its folder with it selected
        _snwprintf(args, 1200, L"/select,\"%ls\"", w);
        args[1199] = 0;
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
}

#else
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

bool re_dir_walk(const char* dir, void (*fn)(void* user, const ReDirEntry* e), void* user) {
    DIR* d = opendir(dir && dir[0] ? dir : ".");
    if (!d) return false;
    for (struct dirent* de; (de = readdir(d)) != NULL;) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[2048];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir && dir[0] ? dir : ".", de->d_name);
        if (stat(path, &st) != 0) continue;
        ReDirEntry e = { de->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, (int64_t)st.st_mtime };
        fn(user, &e);
    }
    closedir(d);
    return true;
}

bool re_file_stat(const char* path, uint64_t* size, int64_t* mtime, bool* is_dir) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}

bool re_make_dirs(const char* path) {
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char* c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0755);
            *c = '/';
        }
    mkdir(p, 0755);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool re_move_path(const char* src, const char* dst) {
    struct stat st;
    return stat(dst, &st) != 0 && rename(src, dst) == 0;
}
bool re_trash_path(const char* path) { return remove(path) == 0; }
FILE* re_fopen_share(const char* path, const char* mode, bool deny_write) {
    (void)deny_write;
    return fopen(path, mode);
}

struct ReFileMap {
    void* data;
};
ReFileMap* re_map_file(const char* path, const uint8_t** data, uint64_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ReFileMap* m = n > 0 ? malloc(sizeof *m) : NULL;
    if (m) m->data = malloc((size_t)n);
    if (!m || !m->data || fread(m->data, 1, (size_t)n, f) != (size_t)n) {
        if (m) free(m->data);
        free(m);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *data = m->data;
    *size = (uint64_t)n;
    return m;
}
void re_unmap_file(ReFileMap* m) {
    if (!m) return;
    free(m->data);
    free(m);
}
bool re_copy_file(const char* src, const char* dst) {
    FILE* a = fopen(src, "rb");
    if (!a) return false;
    FILE* b = fopen(dst, "wb");
    if (!b) return fclose(a), false;
    static char buf[256 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) ok &= fwrite(buf, 1, n, b) == n;
    fclose(a);
    return (fclose(b) == 0) && ok;
}
#ifdef __ANDROID__
const char* re_android_files_dir(void);
#endif
// The executable's folder: the packaged game's files (Android: the APK's assets extracted into <files>/game)
bool re_exe_dir(char* out, size_t cap) {
#ifdef __ANDROID__
    snprintf(out, cap, "%s/game", re_android_files_dir());
    return true;
#else
    char p[1100];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return false;
    p[n] = 0;
    char* s = strrchr(p, '/');
    if (s) *s = 0;
    snprintf(out, cap, "%s", p);
    return true;
#endif
}
void re_show_in_explorer(const char* path) { (void)path; }
int re_run_command(const char* cmdline, const char* log_path) {
    (void)cmdline, (void)log_path;
    return -1; // (the Android game runs no tools)
}

#endif

int64_t re_unix_time(void) { return (int64_t)time(NULL); }

typedef struct TreeCtx {
    char dir[1100];
    bool ok;
} TreeCtx;

static void tree_one(void* user, const ReDirEntry* e) {
    TreeCtx* c = user;
    char p[1300];
    snprintf(p, sizeof p, "%s/%s", c->dir, e->name);
    if (e->is_dir) {
        if (!re_remove_tree(p)) c->ok = false;
    } else {
#ifdef _WIN32
        wchar_t w[1300];
        if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1300) || !DeleteFileW(w)) c->ok = false;
#else
        if (remove(p) != 0) c->ok = false;
#endif
    }
}

bool re_remove_tree(const char* path) {
    bool is_dir = false;
    if (!re_file_stat(path, NULL, NULL, &is_dir)) return true;
    if (!is_dir) return false;
    TreeCtx c;
    snprintf(c.dir, sizeof c.dir, "%s", path);
    c.ok = true;
    re_dir_walk(path, tree_one, &c);
#ifdef _WIN32
    wchar_t w[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100) || !RemoveDirectoryW(w)) c.ok = false;
#else
    if (rmdir(path) != 0) c.ok = false;
#endif
    return c.ok;
}
