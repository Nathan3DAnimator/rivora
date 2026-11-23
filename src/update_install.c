// The file copy of an update (updater.h: update_install_files), only Windows' API: realengine_updater.exe links it with
// re_crypto.c and nothing else of the program.
#include "updater.h"
#include "re_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define BACKUP_DIR L".realengine_update_old"

typedef struct Op {
    wchar_t rel[512];
    bool had_old; // the old file went to the backup folder
} Op;

typedef struct Ops {
    Op* v;
    int n, cap;
} Ops;

static void make_parents(const wchar_t* path) {
    wchar_t tmp[1600];
    wcsncpy(tmp, path, 1599);
    tmp[1599] = 0;
    for (wchar_t* c = tmp + 3; *c; c++)
        if (*c == L'\\' || *c == L'/') {
            wchar_t s = *c;
            *c = 0;
            CreateDirectoryW(tmp, NULL);
            *c = s;
        }
}

// Every file under stage\rel, depth first.
static bool collect(const wchar_t* stage, const wchar_t* rel, Ops* ops) {
    wchar_t pattern[1600];
    _snwprintf(pattern, 1600, L"%ls\\%ls%ls*", stage, rel, rel[0] ? L"\\" : L"");
    pattern[1599] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t sub[512];
        _snwprintf(sub, 512, L"%ls%ls%ls", rel, rel[0] ? L"\\" : L"", fd.cFileName);
        sub[511] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = collect(stage, sub, ops);
        } else {
            if (ops->n == ops->cap) {
                int nc = ops->cap ? ops->cap * 2 : 64;
                Op* nv = realloc(ops->v, sizeof *nv * (size_t)nc);
                if (!nv) {
                    ok = false;
                    break;
                }
                ops->v = nv;
                ops->cap = nc;
            }
            Op* o = &ops->v[ops->n++];
            memset(o, 0, sizeof *o);
            wcscpy(o->rel, sub);
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static void remove_tree_w(const wchar_t* dir) {
    wchar_t pattern[1600];
    _snwprintf(pattern, 1600, L"%ls\\*", dir);
    pattern[1599] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[1600];
            _snwprintf(p, 1600, L"%ls\\%ls", dir, fd.cFileName);
            p[1599] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree_w(p);
            else {
                SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

bool update_install_files(const char* stage_u8, const char* install_u8, int* copied, char* err, size_t err_cap) {
    if (copied) *copied = 0;
    wchar_t stage[1100], install[1100], backup[1200];
    MultiByteToWideChar(CP_UTF8, 0, stage_u8, -1, stage, 1100);
    MultiByteToWideChar(CP_UTF8, 0, install_u8, -1, install, 1100);
    for (wchar_t* c = stage; *c; c++)
        if (*c == L'/') *c = L'\\';
    for (wchar_t* c = install; *c; c++)
        if (*c == L'/') *c = L'\\';
    _snwprintf(backup, 1200, L"%ls\\%ls", install, BACKUP_DIR);
    backup[1199] = 0;
    remove_tree_w(backup); // a failed run before
    Ops ops = { 0 };
    if (!collect(stage, L"", &ops) || ops.n == 0) {
        snprintf(err, err_cap, "a pasta do pacote está vazia");
        free(ops.v);
        return false;
    }
    CreateDirectoryW(backup, NULL);
    SetFileAttributesW(backup, FILE_ATTRIBUTE_HIDDEN);
    bool ok = true;
    int done = 0;
    for (; done < ops.n; done++) {
        Op* o = &ops.v[done];
        wchar_t src[1700], dst[1700], old[1800];
        _snwprintf(src, 1700, L"%ls\\%ls", stage, o->rel);
        _snwprintf(dst, 1700, L"%ls\\%ls", install, o->rel);
        _snwprintf(old, 1800, L"%ls\\%ls", backup, o->rel);
        src[1699] = dst[1699] = old[1799] = 0;
        make_parents(dst);
        if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
            make_parents(old);
            // a file in use (another Real Engine open) can still be renamed... but not a DLL another process loaded:
            // tried for a few seconds
            bool moved = false;
            int tries = getenv("REALENGINE_INSTALL_TRIES") ? atoi(getenv("REALENGINE_INSTALL_TRIES")) : 20;
            for (int t = 0; t < tries && !moved; t++) {
                moved = MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING) != 0;
                if (!moved) Sleep(250);
            }
            if (!moved) {
                char name[600];
                WideCharToMultiByte(CP_UTF8, 0, o->rel, -1, name, sizeof name, NULL, NULL);
                snprintf(err, err_cap, "%s está em uso (close the other Real Engine windows)", name);
                ok = false;
                break;
            }
            o->had_old = true;
        }
        if (!CopyFileW(src, dst, FALSE)) {
            char name[600];
            WideCharToMultiByte(CP_UTF8, 0, o->rel, -1, name, sizeof name, NULL, NULL);
            snprintf(err, err_cap, "não foi possível copiar %s (%lu)", name, (unsigned long)GetLastError());
            if (o->had_old) MoveFileExW(old, dst, MOVEFILE_REPLACE_EXISTING);
            ok = false;
            break;
        }
    }
    if (!ok) { // back to the installed version
        for (int i = done - 1; i >= 0; i--) {
            Op* o = &ops.v[i];
            wchar_t dst[1700], old[1800];
            _snwprintf(dst, 1700, L"%ls\\%ls", install, o->rel);
            _snwprintf(old, 1800, L"%ls\\%ls", backup, o->rel);
            dst[1699] = old[1799] = 0;
            if (o->had_old) MoveFileExW(old, dst, MOVEFILE_REPLACE_EXISTING);
            else DeleteFileW(dst);
        }
    } else if (copied) {
        *copied = ops.n;
    }
    remove_tree_w(backup);
    free(ops.v);
    return ok;
}
#else
bool update_install_files(const char* stage, const char* install, int* copied, char* err, size_t err_cap) {
    (void)stage; (void)install; (void)copied;
    snprintf(err, err_cap, "só no Windows");
    return false;
}
#endif

// A version that can name a folder: digits, letters, '.', '_' and '-' only, no "..", not empty, short. The version comes
// from the server's release tag: anything else (a path) is refused before it reaches a file name.
bool update_version_safe(const char* v) {
    size_t n = v ? strlen(v) : 0;
    if (n == 0 || n > 40 || strstr(v, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// The signature check (re_crypto.c), here so realengine_updater.exe --verify has it without the rest of updater.c.
bool update_verify_file(const char* path, const char* sig_hex, const char* pubkey_hex, char* err, size_t err_cap) {
    uint8_t pub[RE_P256_PUB_LEN], sig[RE_P256_SIG_LEN], hash[RE_SHA256_LEN];
    if (re_hex_decode(pubkey_hex, pub, sizeof pub) < 0) {
        snprintf(err, err_cap, "chave pública inválida na configuração");
        return false;
    }
    if (re_hex_decode(sig_hex, sig, sizeof sig) < 0) {
        snprintf(err, err_cap, "assinatura do pacote inválida");
        return false;
    }
    if (!re_sha256_file(path, hash)) {
        snprintf(err, err_cap, "não foi possível ler o pacote");
        return false;
    }
    if (!re_p256_verify(pub, hash, sig)) {
        snprintf(err, err_cap, "a assinatura não confere: o pacote não é o publicado pelo projeto");
        return false;
    }
    return true;
}

#ifdef _WIN32
// A folder and everything in it (the unpacked package after the install): only Windows' API, no shell.
void update_remove_dir(const char* dir_u8) {
    wchar_t w[1200];
    MultiByteToWideChar(CP_UTF8, 0, dir_u8, -1, w, 1200);
    for (wchar_t* c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    remove_tree_w(w);
}
#else
void update_remove_dir(const char* dir_u8) { (void)dir_u8; }
#endif
