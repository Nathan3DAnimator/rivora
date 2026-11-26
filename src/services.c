#include "services.h"

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "crashlog.h"
#include "endpoints.h"
#include "i18n.h"
#include "platform.h"
#include "report_upload.h"
#include "ui_icons.h"
#include "updater.h"
#include "vkctx.h"

// --- Sending to the developers (report_upload.h): an outbox flushed on a thread ---

static struct {
    UploadTarget target;
    char outbox[1100], rejected[1100];
    ReThread* thread;
    volatile int32_t running, done; // done: a flush finished and its message waits for the status bar
    char last[400];
} Q;

static int flush_thread(void* arg) {
    (void)arg;
    UploadResult r;
    upload_flush(Q.outbox, Q.rejected, &Q.target, &r);
    if (r.rejected) snprintf(Q.last, sizeof Q.last, TR("The server refused the report (%s): it will not be sent again"), r.last_error);
    else if (r.sent) snprintf(Q.last, sizeof Q.last, TR("Sent to the developers (%s)"), r.last_id);
    else if (r.retry) snprintf(Q.last, sizeof Q.last, TR("Could not send now (%s): it stays queued for the next time"), r.last_error);
    else Q.last[0] = 0;
    printf("[RealEngine] Reports: %d sent, %d to retry, %d refused\n", r.sent, r.retry, r.rejected);
    re_atomic_store(&Q.done, 1);
    re_atomic_store(&Q.running, 0);
    return 0;
}

static void kick_flush(void) {
    if (!Q.outbox[0] || !upload_target_enabled(&Q.target) || re_atomic_load(&Q.running)) return;
    if (!upload_outbox_count(Q.outbox)) return;
    if (Q.thread) re_thread_join(Q.thread);
    re_atomic_store(&Q.running, 1);
    Q.thread = re_thread_start(flush_thread, NULL);
}

// The report's text with the product on top (the relay shared with the Tlender: the reports viewer tells them apart by
// it even where the relay does not label the product yet)
static char* product_body(const char* body) {
    static const char head[] = "- Produto: Real Engine\n- Versão: " RE_VERSION_STRING "\n\n";
    size_t n = strlen(body);
    char* out = malloc(sizeof head + n);
    if (!out) return NULL;
    memcpy(out, head, sizeof head - 1);
    memcpy(out + sizeof head - 1, body, n + 1);
    return out;
}

// Queues one item and starts sending; msg says what happened
static bool send_item(UploadKind kind, const char* title, const char* body, const char* name, const char* const* files,
                      int nfiles, char* msg, size_t cap) {
    if (!upload_target_enabled(&Q.target)) {
        snprintf(msg, cap, "%s", TR("Sending is off in this build: the report stays in the local folder."));
        return false;
    }
    char* text = product_body(body ? body : "");
    UploadItem it = { kind, title, text ? text : body, name, files, nfiles };
    char dir[1200], err[300] = "";
    bool ok = upload_enqueue(Q.outbox, &it, dir, sizeof dir, err, sizeof err);
    free(text);
    if (!ok) {
        snprintf(msg, cap, TR("Could not prepare the report: %s"), err);
        return false;
    }
    kick_flush();
    snprintf(msg, cap, "%s", TR("Sending in the background. Without internet it stays queued and goes next time."));
    return true;
}

static char* read_text_file(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s) {
        size_t got = fread(s, 1, (size_t)n, f);
        s[got] = 0;
    }
    fclose(f);
    return s;
}

// --- Crash reports ---

static Editor* g_ed;
static bool g_bug_open, g_suggest_open;
static char g_crash_report[1024]; // last run's crash report, asked whether to send (crash_send_popup)
static bool g_crash_ask;

// The editor's state in a report (the report thread: no locks, nothing allocated)
static void crash_state(char* out, size_t cap, void* user) {
    Editor* ed = user;
    snprintf(out, cap,
             "- **Level**: %s%s\n- **Actors**: %u\n- **Playing**: %s\n- **View mode**: %u\n- **Viewport**: %.0fx%.0f\n"
             "- **Undo steps**: %d\n",
             ed->level_path[0] ? ed->level_path : "(untitled)", ed->dirty ? " (unsaved changes)" : "", ed->world.alive,
             ed->playing ? "yes" : "no", ed->view_mode, ed->vp_rect[2], ed->vp_rect[3], ed->undo_count);
}

// The level as it was, next to the report (the editor's world: while playing, the snapshot taken at Play)
static bool crash_emergency_save(const char* path, void* user) {
    Editor* ed = user;
    if (ed->playing) return false; // the play world is not the user's work; the snapshot is the editor's
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    return editor_save_level(ed, p, 0.0);
}

void services_early_init(void) {
    CrashConfig cfg = { NULL, true, true };
    crash_install(&cfg);
    re_log_capture_std();
    re_log_open_file();
}

// --- Updates (a thread checks and downloads; the frame reads its state) ---

typedef enum UpdState { UPD_IDLE, UPD_CHECKING, UPD_NONE, UPD_AVAILABLE, UPD_ERROR, UPD_DOWNLOADING, UPD_READY, UPD_FAILED } UpdState;

static struct {
    volatile int32_t state;
    bool manual;          // Help > Check for Updates: "up to date" and errors are shown too
    bool dialog;          // the update window is open
    bool dismissed;       // "Later" for this run
    UpdateInfo info;
    UpdateStage stage;
    char msg[512];
    char api[512], page[512];
    volatile int32_t progress_permille; // -1 unknown
    ReThread* thread;
} U;

static bool update_progress(void* user, uint64_t done, uint64_t total) {
    (void)user;
    re_atomic_store(&U.progress_permille, total ? (int32_t)(done * 1000 / total) : -1);
    return true;
}

static int check_thread(void* arg) {
    (void)arg;
    UpdateInfo info;
    memset(&info, 0, sizeof info);
    char err[256] = "";
    if (!update_fetch_latest(U.api, endpoints_get("update", "asset"), &info, err, sizeof err)) {
        snprintf(U.msg, sizeof U.msg, "%s", err);
        re_atomic_store(&U.state, UPD_ERROR);
        return 0;
    }
    update_info_free(&U.info);
    U.info = info;
    re_atomic_store(&U.state, update_is_offer(&U.info, RE_VERSION_STRING) ? UPD_AVAILABLE : UPD_NONE);
    return 0;
}

static int download_thread(void* arg) {
    (void)arg;
    char work[1024], err[256] = "";
    if (!update_work_dir(work, sizeof work) ||
        !update_download_and_stage(&U.info, endpoints_get("update", "pubkey"), work, update_progress, NULL, &U.stage, err, sizeof err)) {
        snprintf(U.msg, sizeof U.msg, "%s", err[0] ? err : "no work folder");
        re_atomic_store(&U.state, UPD_FAILED);
        return 0;
    }
    re_atomic_store(&U.state, UPD_READY);
    return 0;
}

static void start_check(bool manual) {
    int32_t st = re_atomic_load(&U.state);
    if (st == UPD_CHECKING || st == UPD_DOWNLOADING || st == UPD_READY) return;
    if (!U.api[0]) return;
    if (U.thread) re_thread_join(U.thread);
    U.manual = manual;
    U.dismissed = false;
    re_atomic_store(&U.state, UPD_CHECKING);
    U.thread = re_thread_start(check_thread, NULL);
}

void services_init(Editor* ed) {
    g_ed = ed;
    CrashHooks hooks = { crash_state, crash_emergency_save, ed };
    crash_set_hooks(&hooks);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(ed->vk->gpu, &mem);
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mem.memoryHeaps[i].size;
    const VkPhysicalDeviceProperties* p = &ed->vk->gpu_props;
    crash_set_gpu(p->deviceName, p->vendorID, p->driverVersion, p->apiVersion, vram);

    upload_target_load(&Q.target);
    upload_dir("outbox", Q.outbox, sizeof Q.outbox);
    upload_dir("outbox_rejected", Q.rejected, sizeof Q.rejected);
    char report[1024];
    if (crash_take_pending(report, sizeof report)) {
        editor_status(ed, 0.0, TR("The editor closed with a crash last time. The report: %s"), report);
        ed->show_log = true;
        if (upload_target_enabled(&Q.target)) { // asked once whether it goes to the developers (crash_send_popup)
            snprintf(g_crash_report, sizeof g_crash_report, "%s", report);
            g_crash_ask = true;
        }
    }
    kick_flush(); // what an earlier run could not send
    char work[1024], text[512];
    bool ok;
    if (update_work_dir(work, sizeof work) && update_take_result(work, &ok, text, sizeof text))
        editor_status(ed, 0.0, ok ? TR("Updated: %s") : TR("The update failed: %s"), text);

    update_endpoint_urls(endpoints_get("update", "releases_repo"), endpoints_get("update", "latest_url"), U.api, sizeof U.api,
                         U.page, sizeof U.page);
    if (!endpoints_get("update", "latest_url")[0]) { // (2026-11-25) the relay first (no API limit), then the API, then the feed
        char relay[600];
        update_relay_url(endpoints_get("reports", "report_url"), endpoints_get("update", "releases_repo"), relay, sizeof relay);
        if (relay[0]) snprintf(U.api, sizeof U.api, "%s", relay);
    }
    if (!getenv("REALENGINE_NO_UPDATE_CHECK")) start_check(false); // the tests and --bench skip it
}

bool services_status_badge(Editor* ed) {
    (void)ed;
    int32_t st = re_atomic_load(&U.state);
    if (st != UPD_AVAILABLE && st != UPD_DOWNLOADING && st != UPD_READY) return false;
    char label[96];
    if (st == UPD_READY) snprintf(label, sizeof label, TR("Restart to update to %s"), U.info.version);
    else snprintf(label, sizeof label, TR("Update %s available"), U.info.version);
    igPushStyleColor_Vec4(ImGuiCol_Button, THV(TK_GREEN_D));
    if (igButton(label, (ImVec2){ 0, 0 })) U.dialog = true;
    igPopStyleColor(1);
    return true;
}

void services_help_menu(Editor* ed) {
    (void)ed;
    if (ui_menu_item(ICON_UPDATE, TR("Check for Updates..."), NULL, false, U.api[0] != 0)) {
        start_check(true);
        U.dialog = true;
    }
    if (ui_menu_item(ICON_BUG, TR("Report a Bug..."), NULL, false, true)) g_bug_open = true; // opened at the root (services_frame)
    if (ui_menu_item(ICON_BUG, TR("Send a Suggestion..."), NULL, false, true)) g_suggest_open = true;
    if (ui_menu_item(ICON_FOLDER, TR("Open the Reports Folder"), NULL, false, true)) {
        char dir[1024];
        if (crash_reports_dir("crashes", dir, sizeof dir)) crash_open_folder(dir, NULL);
    }
}

// Help > Report a Bug: a title and what happened, saved with the editor's context (system, state, log, actions) next to
// the executable and, ticked, sent to the developers (the relay shared with the Tlender)
static void bug_report_popup(Editor* ed) {
    static char title[128], body[2048];
    static char saved[1100], saved_dir[1024], sent_msg[400];
    static bool send = true;
    if (g_bug_open) {
        igOpenPopup_Str(TR_ID("Report a Bug"), 0);
        g_bug_open = false;
    }
    if (!igBeginPopupModal(TR_ID("Report a Bug"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    float w = igGetFontSize() * 36.0f;
    bool upload = upload_target_enabled(&Q.target);
    if (saved[0]) {
        igTextWrapped(TR("Saved at %s"), saved);
        if (sent_msg[0]) igTextWrapped("%s", sent_msg);
        if (igButton(TR("Open the folder"), (ImVec2){ 0, 0 })) crash_open_folder(saved_dir, saved);
        igSameLine(0, 8);
        if (igButton(TR("Close"), (ImVec2){ 0, 0 })) {
            saved[0] = title[0] = body[0] = sent_msg[0] = 0;
            igCloseCurrentPopup();
        }
        igEndPopup();
        return;
    }
    igTextUnformatted(TR("Title"), NULL);
    igSetNextItemWidth(w);
    igInputText("##bug_title", title, sizeof title, 0, NULL, NULL);
    igTextUnformatted(TR("What happened, and the steps to get there"), NULL);
    igInputTextMultiline("##bug_body", body, sizeof body, (ImVec2){ w, igGetFontSize() * 10.0f }, 0, NULL, NULL);
    igTextDisabled("%s", TR("The report includes the system, the GPU, the editor's state, the last actions and the log."));
    if (upload) {
        igCheckbox(TR("Send to the developers"), &send);
        igTextDisabled("%s", TR("Your Windows user name, computer name and e-mails are removed before sending."));
        igTextDisabled("%s", TR("The state and the country you are in go with it (approximate, without the IP or the city).")); // (the relay stamps the region: ../notas-dev/visualizador-de-reports.md)
    } else {
        igTextDisabled("%s", TR("Sending is off in this build: the report stays in the local folder."));
    }
    igBeginDisabled(!title[0]);
    if (igButton(upload && send ? TR("Send Report") : TR("Save Report"), (ImVec2){ 0, 0 })) {
        char dir[1024], stamp[64], path[1100];
        crash_timestamp(stamp, sizeof stamp);
        if (crash_reports_dir("bugReport", dir, sizeof dir) && crash_create_unique(dir, "RealEngine_bug", stamp, ".md", path, sizeof path)) {
            static char ctx[64 * 1024], text[72 * 1024];
            crash_context_markdown(ctx, sizeof ctx, CRASH_CTX_ALL);
            snprintf(text, sizeof text, "# %s\n\n%s\n\n---\n\n%s", title, body, ctx);
            FILE* f = re_fopen(path, "wb");
            if (f) {
                fputs(text, f);
                fclose(f);
                snprintf(saved, sizeof saved, "%s", path);
                snprintf(saved_dir, sizeof saved_dir, "%s", dir);
                crash_native_slashes(saved);
                editor_status(ed, igGetTime(), TR("Bug report saved: %s"), saved);
                if (upload && send) send_item(UPLOAD_BUG, title, text, NULL, NULL, 0, sent_msg, sizeof sent_msg);
            }
        }
    }
    igEndDisabled();
    igSameLine(0, 8);
    if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) igCloseCurrentPopup();
    igEndPopup();
}

// Help > Send a Suggestion: a name (optional), a title and the idea, sent to the developers
static char g_sug_title[128], g_sug_body[4096];
static bool g_sug_prefill;
void services_suggest_prefill(const char* title, const char* body) { // (the Assistant: a question it couldn't answer)
    snprintf(g_sug_title, sizeof g_sug_title, "%s", title ? title : "");
    snprintf(g_sug_body, sizeof g_sug_body, "%s", body ? body : "");
    g_sug_prefill = true, g_suggest_open = true;
}
static void suggestion_popup(Editor* ed) {
    static char name[64], title[128], body[4096], msg[400];
    if (g_sug_prefill) {
        memcpy(title, g_sug_title, sizeof title), memcpy(body, g_sug_body, sizeof body);
        g_sug_prefill = false;
    }
    if (g_suggest_open) {
        igOpenPopup_Str(TR_ID("Send a Suggestion"), 0);
        g_suggest_open = false;
        msg[0] = 0;
    }
    if (!igBeginPopupModal(TR_ID("Send a Suggestion"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    float w = igGetFontSize() * 36.0f;
    if (msg[0]) {
        igTextWrapped("%s", msg);
        if (igButton(TR("Close"), (ImVec2){ 0, 0 })) {
            msg[0] = title[0] = body[0] = 0;
            igCloseCurrentPopup();
        }
        igEndPopup();
        return;
    }
    bool upload = upload_target_enabled(&Q.target);
    igTextUnformatted(TR("Your name (optional)"), NULL);
    igSetNextItemWidth(w);
    igInputText("##sug_name", name, sizeof name, 0, NULL, NULL);
    igTextUnformatted(TR("Title"), NULL);
    igSetNextItemWidth(w);
    igInputText("##sug_title", title, sizeof title, 0, NULL, NULL);
    igTextUnformatted(TR("Your idea: what is missing, what could be better"), NULL);
    igInputTextMultiline("##sug_body", body, sizeof body, (ImVec2){ w, igGetFontSize() * 10.0f }, 0, NULL, NULL);
    if (upload) igTextDisabled("%s", TR("The state and the country you are in go with it (approximate, without the IP or the city)."));
    if (!upload) igTextDisabled("%s", TR("Sending is off in this build: the report stays in the local folder."));
    igBeginDisabled(!title[0] || !body[0] || !upload);
    if (igButton(TR("Send Suggestion"), (ImVec2){ 0, 0 })) {
        if (send_item(UPLOAD_SUGGESTION, title, body, name[0] ? name : NULL, NULL, 0, msg, sizeof msg))
            editor_status(ed, igGetTime(), "%s", msg);
    }
    igEndDisabled();
    igSameLine(0, 8);
    if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) igCloseCurrentPopup();
    igEndPopup();
}

// Last run's crash: asked once whether its report (and the minidump) go to the developers
static void crash_send_popup(Editor* ed) {
    if (g_crash_ask) {
        igOpenPopup_Str(TR_ID("Send the Crash Report?"), 0);
        g_crash_ask = false;
    }
    if (!igBeginPopupModal(TR_ID("Send the Crash Report?"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    igTextUnformatted(TR("The editor closed with a crash last time."), NULL);
    igTextWrapped(TR("Send the report to the developers? It has the call stacks, the system and the log: %s"), g_crash_report);
    igTextDisabled("%s", TR("Your Windows user name, computer name and e-mails are removed before sending."));
    igTextDisabled("%s", TR("The state and the country you are in go with it (approximate, without the IP or the city)."));
    if (igButton(TR("Send"), (ImVec2){ 0, 0 })) {
        char* text = read_text_file(g_crash_report);
        char dmp[1100];
        snprintf(dmp, sizeof dmp, "%s", g_crash_report);
        char* dot = strrchr(dmp, '.');
        if (dot) snprintf(dot, sizeof dmp - (size_t)(dot - dmp), ".dmp");
        FILE* probe = re_fopen(dmp, "rb");
        const char* files[1] = { dmp };
        if (probe) fclose(probe);
        char msg[400];
        if (text) send_item(UPLOAD_CRASH, TR("Crash"), text, NULL, probe ? files : NULL, probe ? 1 : 0, msg, sizeof msg);
        else snprintf(msg, sizeof msg, TR("Could not prepare the report: %s"), g_crash_report);
        free(text);
        editor_status(ed, igGetTime(), "%s", msg);
        igCloseCurrentPopup();
    }
    igSameLine(0, 8);
    if (igButton(TR("Don't Send"), (ImVec2){ 0, 0 })) igCloseCurrentPopup();
    igEndPopup();
}

static void update_window(Editor* ed) {
    if (!U.dialog) return;
    int32_t st = re_atomic_load(&U.state);
    if (!U.manual && st != UPD_AVAILABLE && st != UPD_DOWNLOADING && st != UPD_READY && st != UPD_FAILED) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 34.0f, 0 }, ImGuiCond_Appearing);
    char title[96];
    snprintf(title, sizeof title, "%s###updates", TR("Updates"));
    bool open = true;
    if (!igBegin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        igEnd();
        U.dialog = open;
        return;
    }
    igText(TR("This version: %s"), RE_VERSION_STRING);
    bool signed_ok = endpoints_get("update", "pubkey")[0] != 0;
    switch (st) {
    case UPD_CHECKING: igTextDisabled("%s", TR("Checking...")); break;
    case UPD_NONE: igTextUnformatted(TR("Rivora Engine is up to date."), NULL); break;
    case UPD_ERROR: igTextWrapped(TR("Could not check: %s"), U.msg); break;
    case UPD_AVAILABLE:
    case UPD_DOWNLOADING:
    case UPD_READY:
    case UPD_FAILED:
        igText(TR("New version: %s"), U.info.version);
        if (U.info.notes) {
            igSeparator();
            igBeginChild_Str("notes", (ImVec2){ 0, igGetFontSize() * 12.0f }, ImGuiChildFlags_Borders, 0);
            igTextWrapped("%s", U.info.notes);
            igEndChild();
        }
        if (st == UPD_DOWNLOADING) {
            int32_t pm = re_atomic_load(&U.progress_permille);
            igProgressBar(pm < 0 ? -1.0f * (float)igGetTime() : pm / 1000.0f, (ImVec2){ -1, 0 }, NULL);
        } else if (st == UPD_READY) {
            igTextWrapped("%s", TR("Downloaded and checked. It installs when the editor closes."));
            if (igButton(TR("Restart Now"), (ImVec2){ 0, 0 })) ed->quit = true;
        } else {
            if (st == UPD_FAILED) igTextWrapped(TR("The download failed: %s"), U.msg);
            if (signed_ok && igButton(TR("Download and Install"), (ImVec2){ 0, 0 })) {
                if (U.thread) re_thread_join(U.thread);
                re_atomic_store(&U.progress_permille, 0);
                re_atomic_store(&U.state, UPD_DOWNLOADING);
                U.thread = re_thread_start(download_thread, NULL);
            }
            igSameLine(0, 8);
            if (igButton(TR("Release Page"), (ImVec2){ 0, 0 })) update_open_url(U.info.page_url[0] ? U.info.page_url : U.page);
            igSameLine(0, 8);
            if (igButton(TR("Later"), (ImVec2){ 0, 0 })) open = false;
        }
        break;
    default: break;
    }
    igEnd();
    U.dialog = open;
}

void services_frame(Editor* ed) {
    crash_tick();
    // A release found by the automatic check opens the window once
    if (!U.manual && !U.dismissed && re_atomic_load(&U.state) == UPD_AVAILABLE) {
        U.dialog = true;
        U.dismissed = true;
    }
    bug_report_popup(ed);
    suggestion_popup(ed);
    crash_send_popup(ed);
    update_window(ed);
    if (re_atomic_load(&Q.done)) { // a flush finished: its result in the status bar
        re_atomic_store(&Q.done, 0);
        if (Q.last[0]) editor_status(ed, igGetTime(), "%s", Q.last);
    }
}

void services_on_exit(Editor* ed) {
    (void)ed;
    if (Q.thread) { // a report being sent finishes (or stays in the outbox for the next run)
        re_thread_join(Q.thread);
        Q.thread = NULL;
    }
    if (U.thread) {
        re_thread_join(U.thread);
        U.thread = NULL;
    }
    if (re_atomic_load(&U.state) != UPD_READY) return;
    char work[1024], exe[1024], err[256];
    if (update_work_dir(work, sizeof work) && update_exe_dir(exe, sizeof exe) &&
        !update_launch_installer(work, &U.stage, exe, "rivora.exe", err, sizeof err))
        printf("[RealEngine] The installer did not start: %s\n", err);
}
