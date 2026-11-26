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
    else if (!_strnicmp(s, "rivora", 6)) s += 6; // (2026-11-23: the new name's tags)
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

// one release object of GitHub's JSON into u
static bool parse_release_value(const JsonValue* r, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    if (!r || r->type != JSON_OBJECT) {
        snprintf(err, err_cap, "resposta inesperada do servidor");
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
    if (!update_version_safe(u->version)) { // the version names folders: a tag like "../x" never gets there
        snprintf(err, err_cap, "versão inválida na release: %s", u->tag);
        update_info_free(u);
        return false;
    }
    return true;
}

// (2026-11-25, bug-fixer) one release (releases/latest, latest_url) or the list (the relay's /releases?repo=, the API's
// /releases): from a list the newest release that is no draft, pre-release nor beta and has a package
bool update_parse_release(const char* json, size_t len, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    JsonDoc doc;
    if (!json_parse(json, len, &doc, err, err_cap)) return false;
    bool ok = false;
    if (doc.root.type == JSON_ARRAY) {
        for (size_t i = 0; i < json_len(&doc.root); i++) {
            UpdateInfo c;
            char e2[160];
            if (!parse_release_value(json_at(&doc.root, i), pattern, &c, e2, sizeof e2)) continue;
            bool usable = !c.draft && !c.prerelease && !re_version_is_beta(c.version) && !re_version_is_beta(c.tag) && c.version[0] &&
                          (c.asset_url[0] || c.page_url[0]);
            if (usable && (!ok || re_version_compare(c.version, u->version) > 0)) {
                if (ok) update_info_free(u);
                *u = c, ok = true;
            } else update_info_free(&c);
        }
        if (!ok) snprintf(err, err_cap, "nenhuma release publicada");
    } else ok = parse_release_value(&doc.root, pattern, u, err, err_cap);
    json_free(&doc);
    return ok;
}

// (2026-11-25, bug-fixer, from the Tlender's updater.c) The public release feed (github.com/<repo>/releases.atom, no API
// limit): each entry's tag, its package named by the pattern's '*' as the version, the web download addresses
static bool atom_between(const char* s, const char* e, const char* a, const char* b, char* out, size_t cap) {
    size_t la = strlen(a), lb = strlen(b);
    for (const char* p = s; p + la <= e; p++) {
        if (memcmp(p, a, la) != 0) continue;
        const char* v = p + la;
        for (const char* q = v; q + lb <= e; q++)
            if (memcmp(q, b, lb) == 0) {
                size_t n = (size_t)(q - v) < cap - 1 ? (size_t)(q - v) : cap - 1;
                memcpy(out, v, n);
                out[n] = 0;
                return true;
            }
        return false;
    }
    return false;
}
bool update_parse_atom(const char* xml, size_t len, const char* repo, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    if (!xml || !repo || !repo[0] || !pattern || !pattern[0]) {
        snprintf(err, err_cap, "feed de releases sem dados");
        return false;
    }
    const char* end = xml + len;
    bool ok = false;
    for (const char* p = xml; p < end;) {
        const char* s0 = strstr(p, "<entry>");
        if (!s0 || s0 >= end) break;
        const char* e = strstr(s0, "</entry>");
        if (!e || e > end) e = end;
        p = e + 1;
        char tag[64] = "", title[160] = "";
        if (!atom_between(s0, e, "/releases/tag/", "\"", tag, sizeof tag) || !tag[0]) continue;
        atom_between(s0, e, "<title>", "</title>", title, sizeof title);
        UpdateInfo c;
        memset(&c, 0, sizeof c);
        snprintf(c.tag, sizeof c.tag, "%s", tag);
        re_version_clean(c.tag, c.version, sizeof c.version);
        if (!c.version[0] || !update_version_safe(c.version) || strpbrk(tag, "/\\?#\"<> ")) continue;
        if (re_version_is_beta(c.version) || re_version_is_beta(c.tag)) continue; // (the release channel only)
        snprintf(c.name, sizeof c.name, "%s", title[0] ? title : tag);
        snprintf(c.page_url, sizeof c.page_url, "https://github.com/%s/releases/tag/%s", repo, tag);
        const char* star = strchr(pattern, '*'); // ("Rivora-*-win64.zip" -> "Rivora-0.52-win64.zip")
        if (star) snprintf(c.asset_name, sizeof c.asset_name, "%.*s%s%s", (int)(star - pattern), pattern, c.version, star + 1);
        else snprintf(c.asset_name, sizeof c.asset_name, "%s", pattern);
        if (strchr(c.asset_name, '*') || strchr(c.asset_name, '?')) continue;
        char base[1200];
        snprintf(base, sizeof base, "https://github.com/%s/releases/download/%s/", repo, tag);
        snprintf(c.asset_url, sizeof c.asset_url, "%s%s", base, c.asset_name);
        snprintf(c.sig_url, sizeof c.sig_url, "%s%s.sig", base, c.asset_name);
        snprintf(c.sha_url, sizeof c.sha_url, "%s%s.sha256", base, c.asset_name);
        if (!ok || re_version_compare(c.version, u->version) > 0) *u = c, ok = true;
    }
    if (!ok) snprintf(err, err_cap, "nenhuma release publicada");
    return ok;
}

void update_relay_url(const char* relay, const char* repo, char* out, size_t cap) {
    if (cap) out[0] = 0;
    bool repo_ok = repo && repo[0] && strchr(repo, '/') && !strpbrk(repo, " ?#&\\");
    if (!relay || !relay[0] || !repo_ok) return;
    size_t n = strlen(relay);
    while (n > 0 && relay[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/releases?repo=%s", (int)n, relay, repo);
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

// owner/name from an API address (https://api.github.com/repos/<owner>/<name>/...)
static bool repo_from_api(const char* url, char* repo, size_t cap) {
    const char* p = url ? strstr(url, "api.github.com/repos/") : NULL;
    if (!p) return false;
    p += strlen("api.github.com/repos/");
    const char* s1 = strchr(p, '/');
    if (!s1) return false;
    const char* s2 = strchr(s1 + 1, '/');
    size_t n = s2 ? (size_t)(s2 - p) : strlen(p);
    if (n == 0 || n >= cap) return false;
    memcpy(repo, p, n);
    repo[n] = 0;
    return !strpbrk(repo, " ?#\\");
}

static bool fetch_api(const char* url, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    HttpRequest rq = { "GET", url, k_json_headers, NULL, 0, NULL, NULL, NULL, 15000 };
    HttpResponse r;
    if (!http_request(&rq, &r)) {
        snprintf(err, err_cap, "%s", r.error);
        return false;
    }
    bool ok = false, limited = false;
    if (r.status == 404) snprintf(err, err_cap, "nenhuma release publicada");
    else if (r.status == 403 || r.status == 429) limited = true;
    else if (r.status != 200) snprintf(err, err_cap, "o servidor respondeu %d", r.status);
    else ok = update_parse_release((const char*)r.body, r.len, pattern, u, err, err_cap);
    http_response_free(&r);
    char repo[256];
    if (limited && repo_from_api(url, repo, sizeof repo)) { // the API's limit: the public release feed instead
        char feed[512];
        snprintf(feed, sizeof feed, "https://github.com/%s/releases.atom", repo);
        HttpRequest fq = { "GET", feed, NULL, NULL, 0, NULL, NULL, NULL, 15000 };
        HttpResponse f;
        memset(&f, 0, sizeof f);
        if (http_request(&fq, &f) && f.status == 200) ok = update_parse_atom((const char*)f.body, f.len, repo, pattern, u, err, err_cap);
        http_response_free(&f);
    }
    if (limited && !ok) snprintf(err, err_cap, "limite de consultas do GitHub (tente mais tarde)");
    return ok;
}

// (2026-11-25, bug-fixer, as the Tlender does) the relay (update_relay_url: the Worker asks GitHub with its token and
// keeps the answer for everyone, so the 60 requests an hour per address of the API do not count); when it fails, the
// API; when the API is at its limit, the public feed. The update_download test went red on the limit alone
bool update_fetch_latest(const char* url, const char* pattern, UpdateInfo* u, char* err, size_t err_cap) {
    memset(u, 0, sizeof *u);
    if (!url || !url[0]) {
        snprintf(err, err_cap, "atualização automática não configurada");
        return false;
    }
    const char* rq_at = strstr(url, "/releases?repo=");
    if (rq_at) {
        HttpRequest rq = { "GET", url, k_json_headers, NULL, 0, NULL, NULL, NULL, 15000 };
        HttpResponse r;
        bool ok = http_request(&rq, &r) && r.status == 200 && update_parse_release((const char*)r.body, r.len, pattern, u, err, err_cap);
        int status = r.status;
        http_response_free(&r);
        if (ok) return true;
        char repo[256], api[512];
        snprintf(repo, sizeof repo, "%s", rq_at + strlen("/releases?repo="));
        repo[strcspn(repo, "&#")] = 0;
        fprintf(stderr, "[RealEngine] atualização: o relay não respondeu (%d), consulta direta ao GitHub\n", status);
        update_info_free(u), memset(u, 0, sizeof *u);
        snprintf(api, sizeof api, "https://api.github.com/repos/%s/releases?per_page=30", repo);
        return fetch_api(api, pattern, u, err, err_cap);
    }
    return fetch_api(url, pattern, u, err, err_cap);
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
    snprintf(probe, sizeof probe, "%s/rivora.exe", out->stage_dir);
    if (!re_file_stat(probe, NULL, NULL, NULL)) {
        snprintf(err, err_cap, "o pacote não tem o rivora.exe");
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
    snprintf(out, cap, "%s\\Rivora Engine\\update", base);
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
    snprintf(src, sizeof src, "%s/rivora_updater.exe", st->stage_dir);
    if (!re_file_stat(src, NULL, NULL, NULL)) snprintf(src, sizeof src, "%s/rivora_updater.exe", install_dir);
    snprintf(dst, sizeof dst, "%s/rivora_updater_run.exe", work_dir);
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

// (2026-11-25, bug-fixer) offline: the relay's list (a beta and a draft newer than the release: skipped) and the feed
int update_parse_selftest(void) {
    static const char list[] =
        "[{\"tag_name\":\"v0.53_b\",\"draft\":false,\"prerelease\":true,\"assets\":[{\"name\":\"Rivora-0.53_b-win64.zip\",\"browser_download_url\":\"https://x/b.zip\"}]},"
        "{\"tag_name\":\"v0.54\",\"draft\":true,\"prerelease\":false,\"assets\":[{\"name\":\"Rivora-0.54-win64.zip\",\"browser_download_url\":\"https://x/d.zip\"}]},"
        "{\"tag_name\":\"v0.52\",\"draft\":false,\"prerelease\":false,\"html_url\":\"https://x/p\",\"assets\":["
        "{\"name\":\"Rivora-0.52-win64.zip\",\"browser_download_url\":\"https://x/r.zip\",\"url\":\"https://api.github.com/x/1\",\"size\":10},"
        "{\"name\":\"Rivora-0.52-win64.zip.sig\",\"browser_download_url\":\"https://x/r.zip.sig\",\"url\":\"https://api.github.com/x/2\"}]},"
        "{\"tag_name\":\"v0.51\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"Rivora-0.51-win64.zip\",\"browser_download_url\":\"https://x/o.zip\"}]}]";
    static const char feed[] =
        "<?xml version=\"1.0\"?><feed><entry><link href=\"https://github.com/o/r/releases/tag/v0.53_b\"/><title>0.53 beta</title></entry>"
        "<entry><link href=\"https://github.com/o/r/releases/tag/v0.52\"/><title>Rivora 0.52</title></entry>"
        "<entry><link href=\"https://github.com/o/r/releases/tag/v0.51\"/><title>Rivora 0.51</title></entry></feed>";
    int fails = 0;
    UpdateInfo u;
    char err[200] = "";
    if (!update_parse_release(list, sizeof list - 1, "Rivora-*-win64.zip", &u, err, sizeof err) || strcmp(u.version, "0.52") ||
        strcmp(u.asset_url, "https://x/r.zip") || strcmp(u.sig_url, "https://x/r.zip.sig")) {
        printf("FAIL: update parse: the list gave \"%s\" (%s), want 0.52 with its package and signature\n", u.version, err);
        fails++;
    } else printf("update parse: the list: %s, %s (the beta and the draft skipped)\n", u.version, u.asset_name);
    update_info_free(&u);
    if (!update_parse_atom(feed, sizeof feed - 1, "o/r", "Rivora-*-win64.zip", &u, err, sizeof err) || strcmp(u.version, "0.52") ||
        strcmp(u.asset_url, "https://github.com/o/r/releases/download/v0.52/Rivora-0.52-win64.zip") ||
        strcmp(u.sig_url, "https://github.com/o/r/releases/download/v0.52/Rivora-0.52-win64.zip.sig")) {
        printf("FAIL: update parse: the feed gave \"%s\" %s (%s)\n", u.version, u.asset_url, err);
        fails++;
    } else printf("update parse: the feed: %s, %s\n", u.version, u.asset_url);
    update_info_free(&u);
    char relay[300];
    update_relay_url("https://relay.example/", "o/r", relay, sizeof relay);
    if (strcmp(relay, "https://relay.example/releases?repo=o/r")) printf("FAIL: update parse: the relay url %s\n", relay), fails++;
    if (!fails) printf("update parse: OK\n");
    return fails ? 1 : 0;
}

int update_selftest(void) {
    char api[512], page[512], err[300] = "";
    update_endpoint_urls(endpoints_get("update", "releases_repo"), endpoints_get("update", "latest_url"), api, sizeof api, page,
                         sizeof page);
    if (!endpoints_get("update", "latest_url")[0]) { // (the app's way: the relay first; update_fetch_latest falls back)
        char relay[600];
        update_relay_url(endpoints_get("reports", "report_url"), endpoints_get("update", "releases_repo"), relay, sizeof relay);
        if (relay[0]) snprintf(api, sizeof api, "%s", relay);
    }
    printf("update-test: asking %s\n", api);
    UpdateInfo u;
    double t0 = re_time_seconds();
    if (!update_fetch_latest(api, endpoints_get("update", "asset"), &u, err, sizeof err)) {
        // (only GitHub's limit, every path tried: not the engine's fault; the test says SKIP, update_sign covers the check)
        if (strstr(err, "limite de consultas")) {
            printf("update-test: SKIP (%s; relay, API and feed tried)\n", err);
            return 0;
        }
        fprintf(stderr, "update-test: releases/latest: %s\n", err);
        return 1;
    }
    printf("update-test: %s (%s), %llu bytes\n  api: %s\n  web: %s\n", u.tag, u.asset_name, (unsigned long long)u.asset_size,
           u.asset_api_url, u.asset_url);
    if (!u.asset_url[0] || !u.sig_url[0]) { // (the feed's have only the web path: the download tries the API one first when there)
        fprintf(stderr, "update-test: the release has no address for the package or its signature\n");
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
