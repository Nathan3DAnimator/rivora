// realengine_updater.exe: installs an update after the program closed (updater.h), and the release tools.
//   realengine_updater --install <pid> <stage> <install> <exe> <work> <version>   (started by the program, hidden)
//   realengine_updater --keygen <private key file>      a key pair: the private key into the file (keep it out of the
//                                                    repository), the public one on stdout (endpoints.ini pubkey)
//   realengine_updater --sign <private key file> <package>   writes <package>.sig and <package>.sha256
//   realengine_updater --verify <public key hex> <package>   checks <package>.sig
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "re_crypto.h"
#include "updater.h"

static FILE* open_u8(const char* path, const wchar_t* mode) {
    wchar_t w[1200];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1200)) return NULL;
    return _wfopen(w, mode);
}

static char* read_text(const char* path) {
    FILE* f = open_u8(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = malloc((size_t)n + 1);
    if (t) t[fread(t, 1, (size_t)n, f)] = 0;
    fclose(f);
    return t;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = open_u8(path, L"wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void result(const char* work, const char* text) {
    char path[1200];
    snprintf(path, sizeof path, "%s\\result.txt", work);
    write_text(path, text);
}

static int install(char** a) {
    DWORD pid = (DWORD)strtoul(a[0], NULL, 10);
    const char *stage = a[1], *dir = a[2], *exe = a[3], *work = a[4], *version = a[5];
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (p) { // the program closes (saving its preferences) before the files can change
        WaitForSingleObject(p, 120000);
        CloseHandle(p);
    }
    char err[600] = "";
    int copied = 0;
    bool ok = update_install_files(stage, dir, &copied, err, sizeof err);
    char text[800];
    if (ok) snprintf(text, sizeof text, "ok %s\n", version);
    else snprintf(text, sizeof text, "erro %s\n", err);
    result(work, text);
    if (ok) { // the unpacked package is no longer needed: exactly <work>\<version>, and only when the stage is inside it
        char expect[1300], a[1300];
        snprintf(expect, sizeof expect, "%s/%s/files", work, version);
        snprintf(a, sizeof a, "%s", stage);
        for (char* c = expect; *c; c++)
            if (*c == '/') *c = '\\';
        for (char* c = a; *c; c++)
            if (*c == '/') *c = '\\';
        if (update_version_safe(version) && !_stricmp(expect, a)) {
            snprintf(expect, sizeof expect, "%s\\%s", work, version);
            update_remove_dir(expect);
        }
    }
    // the program again (the new one, or the old one with the error to show); "-": none (the tests)
    if (!strcmp(exe, "-")) return ok ? 0 : 1;
    char path[1400];
    snprintf(path, sizeof path, "%s\\%s", dir, exe);
    wchar_t wp[1400], wd[1200];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1400);
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wd, 1200);
    ShellExecuteW(NULL, L"open", wp, NULL, wd, SW_SHOWNORMAL);
    return ok ? 0 : 1;
}

static int keygen(const char* file) {
    uint8_t pub[RE_P256_PUB_LEN], priv[RE_P256_PRIV_LEN];
    if (!re_p256_keygen(pub, priv)) {
        fprintf(stderr, "falha ao gerar a chave\n");
        return 1;
    }
    char hex[2 * RE_P256_PRIV_LEN + 2];
    re_hex_encode(priv, sizeof priv, hex);
    strcat(hex, "\n");
    if (!write_text(file, hex)) {
        fprintf(stderr, "não foi possível gravar %s\n", file);
        return 1;
    }
    SecureZeroMemory(priv, sizeof priv);
    re_hex_encode(pub, sizeof pub, hex);
    printf("%s\n", hex);
    return 0;
}

static int sign(const char* keyfile, const char* file) {
    char* key = read_text(keyfile);
    uint8_t priv[RE_P256_PRIV_LEN], hash[RE_SHA256_LEN], sig[RE_P256_SIG_LEN];
    if (!key || re_hex_decode(key, priv, sizeof priv) < 0) {
        fprintf(stderr, "chave privada inválida: %s\n", keyfile);
        free(key);
        return 1;
    }
    free(key);
    if (!re_sha256_file(file, hash) || !re_p256_sign(priv, hash, sig)) {
        fprintf(stderr, "não foi possível assinar %s\n", file);
        return 1;
    }
    SecureZeroMemory(priv, sizeof priv);
    char hex[2 * RE_P256_SIG_LEN + 2], path[1200];
    re_hex_encode(sig, sizeof sig, hex);
    snprintf(path, sizeof path, "%s.sig", file);
    bool ok = write_text(path, hex);
    re_hex_encode(hash, sizeof hash, hex);
    const char* name = strrchr(file, '\\');
    if (!name) name = strrchr(file, '/');
    char line[400];
    snprintf(line, sizeof line, "%s  %s\n", hex, name ? name + 1 : file);
    snprintf(path, sizeof path, "%s.sha256", file);
    ok = ok && write_text(path, line);
    printf("%s\n", hex);
    return ok ? 0 : 1;
}

static int verify(const char* pub, const char* file) {
    char path[1200], err[300];
    snprintf(path, sizeof path, "%s.sig", file);
    char* sig = read_text(path);
    bool ok = sig && update_verify_file(file, sig, pub, err, sizeof err);
    free(sig);
    printf("%s\n", ok ? "assinatura OK" : (sig ? err : "sem .sig"));
    return ok ? 0 : 1;
}

int main(void) {
    // the arguments in UTF-8 (paths with accents: the ANSI argv loses them)
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char** argv = calloc((size_t)(argc + 1), sizeof *argv);
    for (int i = 0; wargv && argv && i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t)(n > 0 ? n : 1));
        if (argv[i]) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    if (!argv) return 2;
    if (argc >= 8 && !strcmp(argv[1], "--install")) return install(argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "--keygen")) return keygen(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--sign")) return sign(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--verify")) return verify(argv[2], argv[3]);
    fprintf(stderr, "uso: realengine_updater --keygen <chave> | --sign <chave> <pacote> | --verify <pub> <pacote>\n");
    return 2;
}
