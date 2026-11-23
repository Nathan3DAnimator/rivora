// Automatic updates from GitHub Releases ([update] of endpoints.ini). Only releases: never a draft, a pre-release or a
// beta (a version ending in _b). The check reads the latest release (no token); installing downloads the package,
// checks its SHA-256 against the signature made with the project's private key (the public one is in endpoints.ini;
// without it nothing is installed: the release page opens in the browser), unpacks it into a staging folder and,
// after the program closes, realengine_updater.exe copies it over the installation (keeping the replaced files until it
// worked) and opens the new version. No UI here; also in the tests (a local server).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net_http.h"

// "0.08" < "0.08.1" < "0.09_b" < "0.09" < "0.10" < "1.0". Leading 'v' and "Real Engine " are ignored. Returns <0, 0, >0.
int re_version_compare(const char* a, const char* b);
bool re_version_is_beta(const char* v); // ends in _b (any case)
// "v0.09" / "Real Engine 0.09" -> "0.09".
void re_version_clean(const char* tag, char* out, size_t cap);

typedef struct UpdateInfo {
    char version[48]; // from the tag
    char tag[64];
    char name[160];
    char* notes;      // the release's text (Markdown; malloc'd; NULL when empty)
    char page_url[512];
    bool draft, prerelease;
    char asset_name[160];
    char asset_url[1024];     // browser_download_url (github.com/.../releases/download/...)
    char asset_api_url[1024]; // the same file through the API (api.github.com/.../releases/assets/<id>): tried first,
                              // github.com's download path answers 504 at times while this one works
    uint64_t asset_size;
    char sig_url[1024];    // <asset>.sig: the ECDSA signature of the package's SHA-256 (hex)
    char sig_api_url[1024];
    char sha_url[1024];    // <asset>.sha256: the SHA-256 (hex), when there is no signature
} UpdateInfo;

void update_info_free(UpdateInfo* u);
// GitHub's release JSON into u; the package is the first asset whose name matches pattern ('*' wildcards).
bool update_parse_release(const char* json, size_t len, const char* pattern, UpdateInfo* u, char* err, size_t err_cap);
// A release to offer the user: newer than current, not a draft, a pre-release or a beta, with a package.
bool update_is_offer(const UpdateInfo* u, const char* current);
bool update_wildcard(const char* pattern, const char* s);

// The API and page addresses from [update]: releases_repo ("owner/repo": https://api.github.com/repos/<it>/releases/latest
// and https://github.com/<it>/releases), latest_url overriding the API one (the tests). Empty when neither is set.
void update_endpoint_urls(const char* releases_repo, const char* latest_url, char* api, size_t api_cap, char* page,
                          size_t page_cap);
// GET the latest release. False when it could not be read (no network...): err says why.
bool update_fetch_latest(const char* url, const char* pattern, UpdateInfo* u, char* err, size_t err_cap);

typedef struct UpdateStage {
    char stage_dir[1024]; // where the package was unpacked
    char version[48];
    int files;
} UpdateStage;

// Downloads the package into work_dir, checks it (signature with pubkey_hex, which must be set), and unpacks it into
// work_dir/<version>/files. progress(user, done, total) of the download; false cancels.
bool update_download_and_stage(const UpdateInfo* u, const char* pubkey_hex, const char* work_dir, HttpProgress progress,
                               void* user, UpdateStage* out, char* err, size_t err_cap);
// The check alone: the SHA-256 of the file and its signature (hex) with the public key (hex).
bool update_verify_file(const char* path, const char* sig_hex, const char* pubkey_hex, char* err, size_t err_cap);

// Starts realengine_updater.exe (a copy in work_dir, so it can replace its own file) to wait for this process to end, copy
// stage_dir over install_dir and open exe_name again. Asks for administrator rights when install_dir is not writable.
bool update_launch_installer(const char* work_dir, const UpdateStage* st, const char* install_dir, const char* exe_name,
                             char* err, size_t err_cap);
// The folder of the running executable (UTF-8, no trailing slash).
bool update_exe_dir(char* out, size_t cap);
// A web page in the browser (UTF-8 URL).
void update_open_url(const char* url);
// %LOCALAPPDATA%\RealEngine\update (created).
bool update_work_dir(char* out, size_t cap);

// What the last install left (work_dir/result.txt: "ok <version>" or "erro <text>"), read once and removed.
bool update_take_result(const char* work_dir, bool* ok, char* text, size_t cap);

// The installer itself (realengine_updater.exe and the tests): copies every file of stage into install (the replaced
// ones go to install/.realengine_update_old first and come back when something fails). False with the reason.
bool update_install_files(const char* stage, const char* install, int* copied, char* err, size_t err_cap);
// A version usable as a folder name (digits, letters, '.', '_', '-'; no ".."): the release tag comes from the server.
bool update_version_safe(const char* v);
// Removes a folder with everything in it (Windows API only: realengine_updater.exe).
void update_remove_dir(const char* dir);
// --update-test: reads the configured releases/latest, downloads the package by the same paths the editor uses
// (API first, web path as fallback, retries on 5xx) into a temp folder and checks its signature. 0 = ok.
int update_selftest(void);

