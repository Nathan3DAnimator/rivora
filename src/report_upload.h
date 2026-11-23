// Sending bug reports, crash reports and suggestions to the developers: the project's relay (a Cloudflare Worker,
// tools/report_relay/worker.js) keeps the GitHub token; the program only knows its address and an app key
// ([reports] report_url and app_key of endpoints.ini). Nothing personal of the user leaves beyond what they wrote:
// the text loses the Windows user name, the home folder (C:\Users\<name> -> %USERPROFILE%), the computer name and
// e-mails. An item is first a draft (the window shows exactly what goes and asks), then goes to the local outbox
// (%APPDATA%\RealEngine\outbox\<stamp>_<kind>\item.json + its files) and is sent in the background; without network, on
// 429 or 5xx it stays for the next try; on 400/403/413 it is not sent again (outbox_rejected, the error shown).
// No UI here; also in the tests.
//
// The Worker's contract: POST <report_url>/report, header X-Tlender-Key, JSON
//   { "kind": "bug"|"crash"|"suggestion", "title", "body" (markdown), "version", "name", "files": [ {name, data} ] }
// answering 200 {ok:true,id} or 400/403/413/429/502 {ok:false,error}. Its limits are enforced here before sending.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum UploadKind { UPLOAD_BUG = 0, UPLOAD_CRASH, UPLOAD_SUGGESTION, UPLOAD_KIND_COUNT } UploadKind;

// The Worker's limits
#define UPLOAD_MAX_BODY (200 * 1024)
#define UPLOAD_MAX_FILES 8
#define UPLOAD_MAX_FILE (3 * 1024 * 1024)
#define UPLOAD_MAX_TOTAL (8 * 1024 * 1024)

typedef struct UploadTarget {
    char url[512]; // <report_url>/report ("" = not configured: nothing leaves the computer)
    char key[160]; // X-Tlender-Key
} UploadTarget;

const char* upload_kind_id(UploadKind k); // "bug", "crash", "suggestion"
// From endpoints.ini (call on the main thread: the threads get the copy).
void upload_target_load(UploadTarget* t);
bool upload_target_enabled(const UploadTarget* t);

typedef struct UploadItem {
    UploadKind kind;
    const char* title;
    const char* body;   // Markdown: the report.md of a bug report, the .txt of a crash, a suggestion's text
    const char* name;   // a suggestion's sender (the name the person typed); NULL otherwise
    const char* const* files; // attachments (images, the minidump, the scene...): checked against the limits
    int file_count;
} UploadItem;

typedef struct UploadPrivacy {
    const char* user;     // USERNAME
    const char* home;     // USERPROFILE
    const char* computer; // COMPUTERNAME
} UploadPrivacy;
// The text with the home folder, the user and computer names and e-mails replaced (malloc'd). NULL: this computer's.
char* upload_scrub(const char* text, const UploadPrivacy* who);
// A file name the Worker takes ([A-Za-z0-9._-]{1,64}, png/jpg/jpeg/txt/md/log/dmp/rlevel/json, not report.md): out
// gets it (accents dropped, other characters '_'); false when the extension is not accepted.
bool upload_file_name(const char* path, char* out, size_t cap);

// %APPDATA%\RealEngine\<sub> (created): "outbox", "outbox_draft", "outbox_rejected".
bool upload_dir(const char* sub, char* out, size_t cap);
// Writes an item into root (a draft or the outbox) with the text scrubbed and the files within the limits (the ones
// left out are listed in item.json "skipped" with the reason). out_dir gets its folder.
bool upload_enqueue(const char* root, const UploadItem* item, char* out_dir, size_t dir_cap, char* err, size_t err_cap);
int upload_outbox_count(const char* root);

// What a draft sends, for the confirmation window.
typedef struct UploadPreview {
    int kind;
    char title[256];
    char name[96];
    char* body;
    int file_count;
    char files[UPLOAD_MAX_FILES][72];
    long long sizes[UPLOAD_MAX_FILES];
    char skipped[1024]; // "file: reason" lines
} UploadPreview;
bool upload_preview_load(const char* item_dir, UploadPreview* p);
void upload_preview_free(UploadPreview* p);
// The confirmed draft moves into the outbox (out_dir: its new folder).
bool upload_commit_draft(const char* draft_dir, const char* outbox, char* out_dir, size_t cap);

// The request body (malloc'd JSON) of an outbox item.
char* upload_build_request(const char* item_dir, size_t* len);

typedef enum UploadSendResult { UPLOAD_SENT = 0, UPLOAD_RETRY, UPLOAD_REJECTED } UploadSendResult;
// Sends one item: sent (its folder removed, id = the Worker's folder), retry (kept, attempts counted) or rejected
// (moved to rejected_root, not sent again). err says why.
UploadSendResult upload_send_one(const char* item_dir, const UploadTarget* t, const char* rejected_root, char* id, size_t id_cap,
                                 char* err, size_t err_cap);

typedef struct UploadResult {
    int sent, retry, rejected;
    char last_error[256];
    char last_id[160];
} UploadResult;
// Tries every item of the outbox (oldest first). One flush at a time.
void upload_flush(const char* outbox, const char* rejected_root, const UploadTarget* t, UploadResult* r);
