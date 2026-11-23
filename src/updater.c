#include "updater.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "re_crypto.h"
#include "zip_read.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

// --- Versions ---------------------------------------------------------------------------------------------------------

void re_version_clean(const char* tag, char* out, size_t cap) {
    const char* s = tag ? tag : "";
    while (*s == ' ') s++;
    if (!_strnicmp(s, "realengine", 10)) s += 10;
    while (*s == ' ' || *s == '-' || *s == '_') s++;
    if (*s == 'v' || *s == 'V') s++;
    snprintf(out, cap, "%s", s);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\r' || out[n - 1] == '\n')) out[--n] = 0;
}

bool re_version_is_beta(const char* v) {
    char c[64];
    re_version_clean(v, c, sizeof c);
    size_t n = strlen(c);
    return n >= 2 && c[n - 2] == '_' && (c[n - 1] == 'b' || c[n - 1] == 'B');
}

// The numbers of a version (up to 6) and whether it is a beta.
static int parse_version(const char* v, long nums[6], bool* beta) {
    char c[64];
    re_version_clean(v, c, sizeof c);
    *beta = re_version_is_beta(c);
    int n = 0;
    const char* s = c;
    while (*s && n < 6) {
        if (!isdigit((unsigned char)*s)) break;
        nums[n++] = strtol(s, (char**)&s, 10);
        if (*s == '.') s++;
        else break;
    }
    for (int i = n; i < 6; i++) nums[i] = 0;
    return n;
}

int re_version_compare(const char* a, const char* b) {
    long na[6], nb[6];
    bool ba, bb;
    parse_version(a, na, &ba);
    parse_version(b, nb, &bb);
    for (int i = 0; i < 6; i++)
        if (na[i] != nb[i]) return na[i] < nb[i] ? -1 : 1;
    if (ba != bb) return ba ? -1 : 1; // 0.09_b comes before 0.09
    return 0;
}

bool update_wildcard(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (const char* t = s;; t++) {
            if (update_wildcard(p + 1, t)) return true;
            if (!*t) return false;
        }
    }
    return *s && tolower((unsigned char)*p) == tolower((unsigned char)*s) && update_wildcard(p + 1, s + 1);
}

// --- The release ------------------------------------------------------------------------------------------------------

void update_info_free(UpdateInfo* u) {
    free(u->notes);
    u->notes = NULL;
}

bool update_parse_release(const char* json, size_t len, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    JsonDoc doc;
    if (!json_parse(json, len, &doc, err, err_cap)) return false;
    const JsonValue* r = &doc.root;
    if (r->type != JSON_OBJECT) {
        snprintf(err, err_cap, "resposta inesperada do servidor");
        json_free(&doc);
        return false;
    }
    snprintf(u->tag, sizeof u->tag, "%s", json_get_str(r, "tag_name", ""));
    re_version_clean(u->tag, u->version, sizeof u->version);
    snprintf(u->name, sizeof u->name, "%s", json_get_str(r, "name", u->tag));
    snprintf(u->page_url, sizeof u->page_url, "%s", json_get_str(r, "html_url", ""));
    u->draft = json_get_bool(r, "draft", false);
    u->prerelease = json_get_bool(r, "prerelease", false);
    const char* body = json_get_str(r, "body", "");
    if (body[0]) {
        u->notes = malloc(strlen(body) + 1);
        if (u->notes) strcpy(u->notes, body);
    }
    const JsonValue* assets = json_get(r, "assets");
    for (size_t i = 0; i < json_len(assets); i++) {
        const JsonValue* a = json_at(assets, i);
        const char* name = json_get_str(a, "name", "");
        if (!u->asset_name[0] && pattern && update_wildcard(pattern, name)) {
            snprintf(u->asset_name, sizeof u->asset_name, "%s", name);
            snprintf(u->asset_url, sizeof u->asset_url, "%s", json_get_str(a, "browser_download_url", ""));
            snprintf(u->asset_api_url, sizeof u->asset_api_url, "%s", json_get_str(a, "url", ""));
            u->asset_size = (uint64_t)json_get_num(a, "size", 0);
        }
    }
    if (u->asset_name[0]) { // its signature and hash: "<package>.sig", "<package>.sha256"
        for (size_t i = 0; i < json_len(assets); i++) {
            const JsonValue* a = json_at(assets, i);
            const char* name = json_get_str(a, "name", "");
            size_t pn = strlen(u->asset_name);
            if (strncmp(name, u->asset_name, pn) != 0) continue;
            if (!_stricmp(name + pn, ".sig")) {
                snprintf(u->sig_url, sizeof u->sig_url, "%s", json_get_str(a, "browser_download_url", ""));
                snprintf(u->sig_api_url, sizeof u->sig_api_url, "%s", json_get_str(a, "url", ""));
            }
            else if (!_stricmp(name + pn, ".sha256"))
                snprintf(u->sha_url, sizeof u->sha_url, "%s", json_get_str(a, "browser_download_url", ""));
        }
    }
    json_free(&doc);
    if (!update_version_safe(u->version)) { // the version names folders: a tag like "../x" never gets there
        snprintf(err, err_cap, "versão inválida na release: %s", u->tag);
        update_info_free(u);
        return false;
    }
    return true;
}

bool update_is_offer(const UpdateInfo* u, const char* current) {
    if (u->draft || u->prerelease || re_version_is_beta(u->version) || re_version_is_beta(u->tag)) return false;
    if (!u->asset_url[0] && !u->page_url[0]) return false;
    return re_version_compare(u->version, current) > 0;
}

void update_endpoint_urls(const char* repo, const char* latest_url, char* api, size_t api_cap, char* page, size_t page_cap) {
    api[0] = page[0] = 0;
    bool repo_ok = repo && repo[0] && strchr(repo, '/') && !strpbrk(repo, " ?#\\");
    if (latest_url && latest_url[0]) snprintf(api, api_cap, "%s", latest_url);
    else if (repo_ok) snprintf(api, api_cap, "https://api.github.com/repos/%s/releases/latest", repo);
    if (repo_ok) snprintf(page, page_cap, "https://github.com/%s/releases", repo);
}

static const char* k_json_headers = "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";

bool update_fetch_latest(const char* url, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    if (!url || !url[0]) {
        snprintf(err, err_cap, "atualização automática não configurada");
        return false;
    }
    HttpRequest rq = { "GET", url, k_json_headers, NULL, 0, NULL, NULL, NULL, 15000 };
    HttpResponse r;
    if (!http_request(&rq, &r)) {
        snprintf(err, err_cap, "%s", r.error);
        return false;
    }
    bool ok = false;
    if (r.status == 404) snprintf(err, err_cap, "nenhuma release publicada");
    else if (r.status == 403 || r.status == 429) snprintf(err, err_cap, "limite de consultas do GitHub (tente mais tarde)");
    else if (r.status != 200) snprintf(err, err_cap, "o servidor respondeu %d", r.status);
    else ok = update_parse_release((const char*)r.body, r.len, pattern, u, err, err_cap);
    http_response_free(&r);
    return ok;
}

// --- Download, check, unpack -------------------------------------------------------------------------------------------

// GitHub serves a release file by two paths: the API (api.github.com/.../releases/assets/<id> with
// "Accept: application/octet-stream", redirects to release-assets.githubusercontent.com) and the web one
// (github.com/<repo>/releases/download/<tag>/<name>). The web one answered 504 Gateway Time-out for hours (2026-11-08)
// while the API one worked, so: the API first, then the web path; each tried 3 times on a network error or a 5xx.
static const char* k_octet_headers = "Accept: application/octet-stream\r\n";

static bool is_api_asset(const char* url) { return url && strstr(url, "api.github.com/") && strstr(url, "/releases/assets/"); }

static bool get_retry(const char* url, const char* out_path, HttpProgress progress, void* user, int timeout_ms, HttpResponse* r,
                      char* err, size_t err_cap) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) re_sleep_seconds(1.5 * attempt);
        HttpRequest rq = { "GET", url, is_api_asset(url) ? k_octet_headers : NULL, NULL, 0, out_path, progress, user, timeout_ms };
        if (!http_request(&rq, r)) {
            snprintf(err, err_cap, "%s", r->error);
            http_response_free(r);
            continue;
        }
        if (r->status == 200) return true;
        snprintf(err, err_cap, "o servidor respondeu %d (%s)", r->status, url);
        bool again = r->status >= 500 || r->status == 429;
        if (!again) return false; // r is kept for the caller to free
        http_response_free(r);
    }
    memset(r, 0, sizeof *r);
    return false;
}

// First url that answers 200 (urls[i] may be empty).
static bool get_any(const char* const* urls, int n, const char* out_path, HttpProgress progress, void* user, int timeout_ms,
                    HttpResponse* r, char* err, size_t err_cap) {
    bool tried = false;
    for (int i = 0; i < n; i++) {
        if (!urls[i] || !urls[i][0]) continue;
        tried = true;
        if (get_retry(urls[i], out_path, progress, user, timeout_ms, r, err, err_cap)) return true;
        http_response_free(r);
    }
    if (!tried) snprintf(err, err_cap, "a release não tem o endereço do arquivo");
    return false;
}

static bool fetch_text2(const char* api_url, const char* web_url, char* out, size_t cap, char* err, size_t err_cap) {
    const char* urls[2] = { api_url, web_url };
    HttpResponse r;
    if (!get_any(urls, 2, NULL, NULL, NULL, 15000, &r, err, err_cap)) return false;
    snprintf(out, cap, "%s", r.body ? (const char*)r.body : "");
    http_response_free(&r);
    return true;
}

bool update_download_and_stage(const UpdateInfo* u, const char* pubkey_hex, const char* work_dir, HttpProgress progress,
                               void* user, UpdateStage* out, char* err, size_t err_cap) {
    memset(out, 0, sizeof *out);
    if (!pubkey_hex || !pubkey_hex[0]) {
        snprintf(err, err_cap, "esta build não tem a chave para conferir os pacotes: baixe pela página da release");
        return false;
    }
    if (!u->asset_url[0]) {
        snprintf(err, err_cap, "a release não tem o pacote do Windows");
        return false;
    }
    if (!u->sig_url[0]) {
        snprintf(err, err_cap, "a release não tem a assinatura do pacote (%s.sig)", u->asset_name);
        return false;
    }
    if (!update_version_safe(u->version)) {
        snprintf(err, err_cap, "versão inválida na release");
        return false;
    }
    char sig[512];
    if (!fetch_text2(u->sig_api_url, u->sig_url, sig, sizeof sig, err, err_cap)) return false;
    char dir[1100], zip[1300];
    snprintf(dir, sizeof dir, "%s/%s", work_dir, u->version);
    re_remove_tree(dir); // an earlier attempt
    if (!re_make_dirs(dir)) {
        snprintf(err, err_cap, "não foi possível criar %s", dir);
        return false;
    }
    bool plain_name = u->asset_name[0] && !strpbrk(u->asset_name, "/\\:") && !strstr(u->asset_name, "..");
    snprintf(zip, sizeof zip, "%s/%s", dir, plain_name ? u->asset_name : "pacote.zip");
    const char* urls[2] = { u->asset_api_url, u->asset_url };
    HttpResponse r;
    bool got = get_any(urls, 2, zip, progress, user, 60000, &r, err, err_cap);
    http_response_free(&r);
    if (!got) return false;
    if (!update_verify_file(zip, sig, pubkey_hex, err, err_cap)) {
        re_remove_file(zip);
        return false;
    }
    snprintf(out->stage_dir, sizeof out->stage_dir, "%s/files", dir);
    char zerr[300];
    if (!zip_extract(zip, out->stage_dir, true, &out->files, zerr, sizeof zerr)) {
        snprintf(err, err_cap, "não foi possível abrir o pacote: %s", zerr);
        return false;
    }
    re_remove_file(zip);
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/realengine.exe", out->stage_dir);
    if (!re_file_stat(probe, NULL, NULL, NULL)) {
        snprintf(err, err_cap, "o pacote não tem o realengine.exe");
        return false;
    }
    snprintf(out->version, sizeof out->version, "%s", u->version);
    return true;
}

bool update_take_result(const char* work_dir, bool* ok, char* text, size_t cap) {
    char path[1100];
    snprintf(path, sizeof path, "%s/result.txt", work_dir);
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    char line[600] = "";
    size_t n = fread(line, 1, sizeof line - 1, f);
    line[n] = 0;
    fclose(f);
    re_remove_file(path);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    *ok = !strncmp(line, "ok ", 3);
    const char* t = strchr(line, ' ');
    snprintf(text, cap, "%s", t ? t + 1 : line);
    return true;
}

#ifdef _WIN32
bool update_exe_dir(char* out, size_t cap) {
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (!n || n >= 1024) return false;
    wchar_t* s = wcsrchr(w, L'\\');
    if (s) *s = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

bool update_work_dir(char* out, size_t cap) {
    wchar_t w[1024];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", w, 1024)) return false;
    char base[1024];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, base, sizeof base, NULL, NULL) <= 0) return false;
    snprintf(out, cap, "%s\\Real Engine\\update", base);
    return re_make_dirs(out);
}

static bool writable(const char* dir) {
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/.realengine_write_test", dir);
    FILE* f = re_fopen(probe, "wb");
    if (!f) return false;
    fclose(f);
    re_remove_file(probe);
    return true;
}

bool update_launch_installer(const char* work_dir, const UpdateStage* st, const char* install_dir, const char* exe_name,
                             char* err, size_t err_cap) {
    // The installer: the new one from the package when it has it, else the installed one; run from a copy
    char src[1200], dst[1200];
    snprintf(src, sizeof src, "%s/realengine_updater.exe", st->stage_dir);
    if (!re_file_stat(src, NULL, NULL, NULL)) snprintf(src, sizeof src, "%s/realengine_updater.exe", install_dir);
    snprintf(dst, sizeof dst, "%s/realengine_updater_run.exe", work_dir);
    wchar_t wsrc[1200], wdst[1200];
    MultiByteToWideChar(CP_UTF8, 0, src, -1, wsrc, 1200);
    MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, 1200);
    if (!CopyFileW(wsrc, wdst, FALSE)) {
        snprintf(err, err_cap, "não foi possível preparar o instalador (%s)", src);
        return false;
    }
    char args[4000];
    snprintf(args, sizeof args, "--install %lu \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", (unsigned long)GetCurrentProcessId(),
             st->stage_dir, install_dir, exe_name, work_dir, st->version);
    wchar_t wargs[4000];
    MultiByteToWideChar(CP_UTF8, 0, args, -1, wargs, 4000);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = writable(install_dir) ? L"open" : L"runas"; // Program Files: the administrator's permission
    sei.lpFile = wdst;
    sei.lpParameters = wargs;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        snprintf(err, err_cap, GetLastError() == ERROR_CANCELLED ? "permissão de administrador negada"
                                                                  : "não foi possível iniciar o instalador");
        return false;
    }
    return true;
}
#else
bool update_exe_dir(char* out, size_t cap) { (void)cap; out[0] = 0; return false; }
bool update_work_dir(char* out, size_t cap) { (void)cap; out[0] = 0; return false; }
bool update_launch_installer(const char* w, const UpdateStage* st, const char* i, const char* e, char* err, size_t cap) {
    (void)w; (void)st; (void)i; (void)e;
    snprintf(err, cap, "só no Windows");
    return false;
}
#endif

#ifdef _WIN32
void update_open_url(const char* url) {
    if (!url || !url[0]) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    wchar_t* w = n > 0 ? malloc(sizeof(wchar_t) * (size_t)n) : NULL;
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, n);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}
#else
void update_open_url(const char* url) { (void)url; }
#endif

#include "endpoints.h"

int update_selftest(void) {
    char api[512], page[512], err[300] = "";
    update_endpoint_urls(endpoints_get("update", "releases_repo"), endpoints_get("update", "latest_url"), api, sizeof api, page,
                         sizeof page);
    UpdateInfo u;
    double t0 = re_time_seconds();
    if (!update_fetch_latest(api, endpoints_get("update", "asset"), &u, err, sizeof err)) {
        fprintf(stderr, "update-test: releases/latest: %s\n", err);
        return 1;
    }
    printf("update-test: %s (%s), %llu bytes\n  api: %s\n  web: %s\n", u.tag, u.asset_name, (unsigned long long)u.asset_size,
           u.asset_api_url, u.asset_url);
    if (!u.asset_api_url[0] || !u.sig_api_url[0]) {
        fprintf(stderr, "update-test: the release JSON has no API url for the package or its signature\n");
        update_info_free(&u);
        return 1;
    }
    char tmp[1024];
    if (!update_work_dir(tmp, sizeof tmp)) {
        update_info_free(&u);
        return 1;
    }
    char work[1200];
    snprintf(work, sizeof work, "%s/selftest", tmp);
    UpdateStage st;
    bool ok = update_download_and_stage(&u, endpoints_get("update", "pubkey"), work, NULL, NULL, &st, err, sizeof err);
    printf("update-test: %s in %.1f s%s%s\n", ok ? "downloaded, signature ok, unpacked" : "FAILED", re_time_seconds() - t0,
           ok ? "" : ": ", ok ? "" : err);
    update_remove_dir(work);
    update_info_free(&u);
    return ok ? 0 : 1;
}
