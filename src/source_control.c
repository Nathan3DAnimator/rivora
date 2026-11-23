// Source Control (source_control.h; notas-dev/editor.md "Controle de versão"): Git through its command line, each call
// on a thread (re_run_capture), the results swapped in under a lock-free flag the editor polls.
#include "source_control.h"

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#define SC_MAX 4096
static struct {
    char project[700], root[700], rel[700]; // the project's folder, the repository's top, the project inside it ("" at the top)
    bool active;
    ScFile files[SC_MAX], next[SC_MAX];      // the last status; the thread's
    uint32_t nfiles, nnext;
    volatile int busy, ready;
    ReThread* thread;
    char msg[300];
    char who[160];                           // "-c user.name=... -c user.email=..." (tests)
    // a submit's job
    char* add_list;                          // the paths, quoted, space separated
    char commit_msg[512];
} S;

static void unix_slashes(char* s) {
    for (; *s; s++)
        if (*s == '\\') *s = '/';
}

void sc_set_identity(const char* name, const char* email) {
    snprintf(S.who, sizeof S.who, "-c user.name=\"%s\" -c user.email=\"%s\" ", name ? name : "", email ? email : "");
}

void sc_init(const char* project_dir) {
    if (S.thread) re_thread_join(S.thread), S.thread = NULL;
    free(S.add_list);
    memset(&S, 0, sizeof S);
    if (!project_dir || !project_dir[0]) return;
    snprintf(S.project, sizeof S.project, "%s", project_dir);
    unix_slashes(S.project);
    char cmd[900], out[1024];
    int code = -1;
    snprintf(cmd, sizeof cmd, "git -C \"%s\" rev-parse --show-toplevel", S.project);
    if (!re_run_capture(cmd, out, sizeof out, &code) || code != 0) return; // (no Git, or not in a repository)
    char* nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    snprintf(S.root, sizeof S.root, "%s", out);
    unix_slashes(S.root);
    size_t rl = strlen(S.root);
    if (!_strnicmp(S.project, S.root, rl)) snprintf(S.rel, sizeof S.rel, "%s", S.project[rl] == '/' ? S.project + rl + 1 : S.project + rl);
    S.active = true;
    sc_refresh();
}

bool sc_active(void) { return S.active; }
const char* sc_root(void) { return S.root; }
bool sc_busy(void) { return S.busy != 0; }
const char* sc_last_message(void) { return S.msg; }

// `git status --porcelain` into S.next: "XY path" (renames "XY old -> new": the new)
static void parse_status(const char* out) {
    S.nnext = 0;
    for (const char* l = out; *l && S.nnext < SC_MAX;) {
        const char* e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        if (n > 3) {
            char x = l[0], y = l[1];
            ScFile* f = &S.next[S.nnext];
            const char* p = l + 3;
            size_t pn = n - 3;
            const char* arrow = NULL;
            for (size_t i = 0; i + 3 < pn; i++)
                if (!memcmp(p + i, " -> ", 4)) arrow = p + i + 4;
            if (arrow) pn -= (size_t)(arrow - p), p = arrow;
            if (pn && p[0] == '"') p++, pn -= 2; // (a quoted path)
            if (pn >= sizeof f->path) pn = sizeof f->path - 1;
            memcpy(f->path, p, pn), f->path[pn] = 0;
            while (pn && (f->path[pn - 1] == '\r' || f->path[pn - 1] == ' ')) f->path[--pn] = 0;
            f->state = x == '?' ? SC_NEW : (x == 'D' || y == 'D') ? SC_DELETED : x == 'A' ? SC_ADDED : SC_MODIFIED;
            S.nnext++;
        }
        l = e ? e + 1 : l + n;
    }
}

static int status_thread(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    size_t cap = 1u << 20;
    char* out = malloc(cap); // (a status's text: a thread's, not a frame's)
    char cmd[1200];
    int code = -1;
    snprintf(cmd, sizeof cmd, "git -C \"%s\" status --porcelain=v1 --untracked-files=all -- .", S.project);
    if (out && re_run_capture(cmd, out, cap, &code) && code == 0) parse_status(out), S.ready = 1;
    free(out);
    S.busy = 0;
    return 0;
}

static int submit_thread(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    char out[2048];
    int code = -1;
    size_t cl = strlen(S.add_list ? S.add_list : "") + 900;
    char* cmd = malloc(cl);
    bool ok = false;
    if (cmd) {
        snprintf(cmd, cl, "git -C \"%s\" add -A -- %s", S.root, S.add_list ? S.add_list : "");
        ok = re_run_capture(cmd, out, sizeof out, &code) && code == 0;
        if (ok) {
            // the message through a file (its quotes and lines as typed)
            char mf[800];
            const char* tmp = getenv("TEMP");
            snprintf(mf, sizeof mf, "%s/re_commit_msg.txt", tmp && tmp[0] ? tmp : S.project);
            FILE* f = re_fopen(mf, "wb");
            if (f) fputs(S.commit_msg, f), fclose(f);
            snprintf(cmd, cl, "git %s-C \"%s\" commit -F \"%s\"", S.who, S.root, mf);
            ok = re_run_capture(cmd, out, sizeof out, &code) && code == 0;
            re_remove_file(mf);
        }
        free(cmd);
    }
    char* nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    snprintf(S.msg, sizeof S.msg, "%s%s", ok ? "" : "FAILED: ", out);
    free(S.add_list), S.add_list = NULL;
    // the status after it
    status_thread(NULL);
    return 0;
}

// (B14) a Git command on the thread: its first line (or what it did) to the status bar, then the status again
static char g_job[1400], g_job_ok[160];
static int job_thread(void* arg) {
    (void)arg;
    re_thread_set_low_priority(true);
    char out[2048] = "";
    int code = -1;
    bool ok = re_run_capture(g_job, out, sizeof out, &code) && code == 0;
    char* nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    if (ok) snprintf(S.msg, sizeof S.msg, "%s", g_job_ok);
    else snprintf(S.msg, sizeof S.msg, "FAILED: %s", out[0] ? out : g_job);
    status_thread(NULL);
    return 0;
}
static bool run_job(const char* done) {
    if (!S.active || S.busy) return false;
    snprintf(g_job_ok, sizeof g_job_ok, "%s", done);
    if (S.thread) re_thread_join(S.thread), S.thread = NULL;
    S.busy = 1;
    S.thread = re_thread_start(job_thread, NULL);
    if (!S.thread) job_thread(NULL);
    return true;
}
bool sc_pull(void) {
    if (!S.active) return false;
    snprintf(g_job, sizeof g_job, "git %s-C \"%s\" pull --ff-only", S.who, S.root);
    return run_job(TR("Pulled: the remote's commits are here."));
}
bool sc_push(void) {
    if (!S.active) return false;
    snprintf(g_job, sizeof g_job, "git %s-C \"%s\" push", S.who, S.root);
    return run_job(TR("Pushed: the commits are on the remote."));
}
bool sc_lock(const char* vpath, bool lock) {
    if (!S.active || !vpath || strncmp(vpath, "/Content/", 9)) return false;
    snprintf(g_job, sizeof g_job, "git -C \"%s\" lfs %s \"%s%sContent/%s\"", S.root, lock ? "lock" : "unlock", S.rel, S.rel[0] ? "/" : "", vpath + 9);
    return run_job(lock ? TR("Locked: the others can't push changes to it.") : TR("Unlocked."));
}
uint32_t sc_diff(const char* repo_path, char* out, size_t cap) {
    if (!S.active || !repo_path || cap < 2) return 0;
    char cmd[1400];
    int code = -1;
    snprintf(cmd, sizeof cmd, "git -C \"%s\" diff -- \"%s\"", S.root, repo_path);
    out[0] = 0;
    re_run_capture(cmd, out, cap, &code);
    if (!out[0]) snprintf(out, cap, "%s", TR("(no differences to show: a new file, or only a binary changed)"));
    return (uint32_t)strlen(out);
}

static void take_ready(void) {
    if (!S.ready) return;
    memcpy(S.files, S.next, sizeof(ScFile) * S.nnext);
    S.nfiles = S.nnext, S.ready = 0;
}

void sc_refresh(void) {
    if (!S.active || S.busy) return;
    if (S.thread) re_thread_join(S.thread), S.thread = NULL;
    S.busy = 1;
    S.thread = re_thread_start(status_thread, NULL);
    if (!S.thread) status_thread(NULL);
}

uint32_t sc_changed(const ScFile** out) {
    take_ready();
    *out = S.files;
    return S.nfiles;
}

int sc_state(const char* vpath) {
    take_ready();
    if (!S.active || !vpath || strncmp(vpath, "/Content/", 9)) return SC_NONE;
    char rp[900]; // (the repository's path of it)
    snprintf(rp, sizeof rp, "%s%sContent/%s", S.rel, S.rel[0] ? "/" : "", vpath + 9);
    for (uint32_t i = 0; i < S.nfiles; i++)
        if (!_stricmp(S.files[i].path, rp)) return S.files[i].state;
    return SC_NONE;
}

bool sc_submit(const char* const* paths, uint32_t n, const char* message) {
    if (!S.active || S.busy || !n || !message || !message[0]) return false;
    size_t cap = 4;
    for (uint32_t i = 0; i < n; i++) cap += strlen(paths[i]) + 4;
    free(S.add_list);
    S.add_list = malloc(cap);
    if (!S.add_list) return false;
    S.add_list[0] = 0;
    for (uint32_t i = 0; i < n; i++) strcat(S.add_list, "\""), strcat(S.add_list, paths[i]), strcat(S.add_list, "\" ");
    snprintf(S.commit_msg, sizeof S.commit_msg, "%s", message);
    if (S.thread) re_thread_join(S.thread), S.thread = NULL;
    S.busy = 1;
    S.thread = re_thread_start(submit_thread, NULL);
    if (!S.thread) submit_thread(NULL);
    return true;
}

bool sc_lfs_track(void) {
    if (!S.active) return false;
    char cmd[1200], out[1024];
    int code = -1;
    snprintf(cmd, sizeof cmd, "git -C \"%s\" lfs track \"*.rmesh\" \"*.rtex\" \"*.rskm\" \"*.ranim\" \"*.rsound\" \"*.rlevel\" \"*.rlight\" \"*.rland\" \"*.rlw\" \"*.png\" \"*.jpg\" \"*.exr\" \"*.hdr\" \"*.wav\" \"*.ogg\" \"*.vdb\" \"*.ply\"",
             S.root);
    bool ok = re_run_capture(cmd, out, sizeof out, &code) && code == 0;
    snprintf(S.msg, sizeof S.msg, "%s", ok ? TR("Git LFS tracks the binary assets (.gitattributes): add it in the next Submit.")
                                           : TR("Git LFS isn't installed (git lfs install)."));
    sc_refresh();
    return ok;
}

// --- File > Source Control > Submit Content ---
void sc_window(bool* open) {
    if (!*open) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 34.0f, igGetFontSize() * 28.0f }, ImGuiCond_FirstUseEver);
    if (!igBegin(TR("Submit Content"), open, 0)) {
        igEnd();
        return;
    }
    if (!S.active) {
        igTextWrapped("%s", TR("The project isn't in a Git repository (or Git isn't installed): git init in its folder, then open it again."));
        igEnd();
        return;
    }
    static bool pick[SC_MAX];
    static char message[512];
    const ScFile* f;
    uint32_t n = sc_changed(&f);
    igTextDisabled("%s: %s", TR("Repository"), S.root);
    if (igButton(TR("Refresh"), (ImVec2){ 0, 0 })) sc_refresh();
    igSameLine(0, 6);
    if (igButton(TR("Select All"), (ImVec2){ 0, 0 }))
        for (uint32_t i = 0; i < n; i++) pick[i] = true;
    igSameLine(0, 6);
    if (igButton(TR("Track Binaries with Git LFS"), (ImVec2){ 0, 0 })) sc_lfs_track();
    igSameLine(0, 6);
    igBeginDisabled(sc_busy());
    if (igButton(TR("Pull"), (ImVec2){ 0, 0 })) sc_pull();
    igSameLine(0, 6);
    if (igButton(TR("Push"), (ImVec2){ 0, 0 })) sc_push();
    igEndDisabled();
    static char diff[16384];
    static bool diff_open;
    if (igBeginChild_Str("sc_files", (ImVec2){ 0, -igGetFontSize() * 7.0f }, ImGuiChildFlags_Borders, 0)) {
        static const char* const st[] = { "", "new", "modified", "deleted", "added" };
        static const uint8_t col[] = { TK_TEXT, TK_GREEN, TK_AMBER, TK_RED, TK_GREEN };
        for (uint32_t i = 0; i < n && i < SC_MAX; i++) {
            igPushID_Int((int)i);
            igCheckbox("##p", &pick[i]);
            igSameLine(0, 6);
            igPushStyleColor_U32(ImGuiCol_Text, TH(col[f[i].state]));
            igTextUnformatted(TR(st[f[i].state]), NULL);
            igPopStyleColor(1);
            igSameLine(igGetFontSize() * 6.0f, -1);
            if (igSmallButton(TR("Diff"))) sc_diff(f[i].path, diff, sizeof diff), diff_open = true;
            igSameLine(0, 6);
            igTextUnformatted(f[i].path, NULL);
            igPopID();
        }
        if (!n) igTextDisabled("%s", sc_busy() ? TR("Reading the changes...") : TR("Nothing changed since the last commit."));
    }
    igEndChild();
    if (diff_open) { // (the file's changes: Git's diff, read only)
        igSetNextWindowSize((ImVec2){ igGetFontSize() * 44.0f, igGetFontSize() * 26.0f }, ImGuiCond_FirstUseEver);
        if (igBegin(TR("Diff"), &diff_open, 0))
            igInputTextMultiline("##diff", diff, sizeof diff, (ImVec2){ -1, -1 }, ImGuiInputTextFlags_ReadOnly, NULL, NULL);
        igEnd();
    }
    igInputTextMultiline("##msg", message, sizeof message, (ImVec2){ -1, igGetFontSize() * 4.0f }, 0, NULL, NULL);
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < n; i++) chosen += pick[i];
    igBeginDisabled(!chosen || !message[0] || sc_busy());
    if (igButton(TR("Submit"), (ImVec2){ 0, 0 })) {
        const char* paths[SC_MAX];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++)
            if (pick[i]) paths[k++] = f[i].path;
        if (sc_submit(paths, k, message)) {
            message[0] = 0;
            memset(pick, 0, sizeof pick);
        }
    }
    igEndDisabled();
    igSameLine(0, 8);
    igTextDisabled("%s", sc_busy() ? TR("Working...") : S.msg);
    igEnd();
}

// (the tests' repositories gone: Git's objects are read only on Windows, which re_remove_tree leaves; a run after found
// the last one's history and its pull brought nothing)
static void rm_repo(const char* path) {
    char cmd[1000], out[256], p[800];
    int code;
    snprintf(p, sizeof p, "%s", path);
    for (char* c = p; *c; c++)
        if (*c == '/') *c = '\\';
    snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", p);
    re_run_capture(cmd, out, sizeof out, &code);
    re_remove_tree(path);
}

// --sc-test: a repository made in the temporary folder, two assets new, submitted, one changed again; its states read
// back as the Content Browser would. Failures: the count (0: ok; Git missing: skipped)
int sc_selftest(void) {
    char dir[700], cmd[1200], out[2048], p[800];
    int code = -1, fails = 0;
    const char* tmp = getenv("TEMP");
    snprintf(dir, sizeof dir, "%s/re_sc_test", tmp && tmp[0] ? tmp : ".");
    unix_slashes(dir);
    rm_repo(dir);
    snprintf(p, sizeof p, "%s/Content/Props", dir);
    re_make_dirs(p);
    snprintf(cmd, sizeof cmd, "git -C \"%s\" init -q", dir);
    if (!re_run_capture(cmd, out, sizeof out, &code) || code != 0) {
        printf("[RealEngine] Source control test: skipped (no Git)\n");
        return 0;
    }
    const char* names[2] = { "Content/Props/M_Rock.rmat", "Content/Props/SM_Rock.rmesh" };
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE* f = re_fopen(p, "wb");
        if (f) fputs("first", f), fclose(f);
    }
    sc_init(dir);
    sc_set_identity("RealEngine Test", "test@example.invalid");
    for (int k = 0; k < 400 && sc_busy(); k++) re_sleep_seconds(0.01);
    int s0 = sc_state("/Content/Props/M_Rock.rmat"), s1 = sc_state("/Content/Props/SM_Rock.rmesh");
    const char* paths[2] = { names[0], names[1] };
    bool sub = sc_submit(paths, 2, "First rock\n\nMade by the source control test.");
    for (int k = 0; k < 800 && sc_busy(); k++) re_sleep_seconds(0.01);
    int s2 = sc_state("/Content/Props/M_Rock.rmat");
    snprintf(p, sizeof p, "%s/%s", dir, names[0]);
    FILE* f = re_fopen(p, "wb");
    if (f) fputs("second", f), fclose(f);
    sc_refresh();
    for (int k = 0; k < 400 && sc_busy(); k++) re_sleep_seconds(0.01);
    int s3 = sc_state("/Content/Props/M_Rock.rmat"), s4 = sc_state("/Content/Props/SM_Rock.rmesh");
    snprintf(cmd, sizeof cmd, "git -C \"%s\" log --oneline", dir);
    re_run_capture(cmd, out, sizeof out, &code);
    char* nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    printf("[RealEngine] Source control: new %d %d, submitted %d -> %d (%s), changed %d, the other %d; log \"%s\"\n", s0, s1, sub, s2, S.msg, s3, s4, out);
    if (!S.active || s0 != SC_NEW || s1 != SC_NEW || !sub || s2 != SC_NONE || s3 != SC_MODIFIED || s4 != SC_NONE || !strstr(out, "First rock"))
        printf("[RealEngine] FAIL: source control: the states, the submit or the log\n"), fails++;
    { // (B14) the diff of the change; push to a bare remote; a commit made in another clone pulled back
        static char d[4096];
        sc_diff(names[0], d, sizeof d);
        bool diff_ok = strstr(d, "-first") && strstr(d, "+second");
        char bare[800], other[800];
        snprintf(bare, sizeof bare, "%s_remote.git", dir);
        snprintf(other, sizeof other, "%s_other", dir);
        rm_repo(bare), rm_repo(other);
        snprintf(cmd, sizeof cmd, "git init -q --bare \"%s\"", bare);
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(cmd, sizeof cmd, "git -C \"%s\" remote add origin \"%s\"", dir, bare);
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(cmd, sizeof cmd, "git -C \"%s\" push -q -u origin HEAD", dir); // (the upstream once, as a user would)
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(cmd, sizeof cmd, "git -C \"%s\" checkout -q -- .", dir); // (the local change dropped: the pull fast-forwards)
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(cmd, sizeof cmd, "git clone -q \"%s\" \"%s\"", bare, other);
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(p, sizeof p, "%s/Content/Props/M_Rock.rmat", other);
        FILE* g = re_fopen(p, "wb");
        if (g) fputs("from the other", g), fclose(g);
        snprintf(cmd, sizeof cmd, "git %s-C \"%s\" commit -q -am \"Other change\"", S.who, other);
        re_run_capture(cmd, out, sizeof out, &code);
        snprintf(cmd, sizeof cmd, "git -C \"%s\" push -q", other);
        re_run_capture(cmd, out, sizeof out, &code);
        bool pulled = sc_pull();
        for (int k = 0; k < 800 && sc_busy(); k++) re_sleep_seconds(0.01);
        snprintf(p, sizeof p, "%s/Content/Props/M_Rock.rmat", dir);
        char got[64] = "";
        g = re_fopen(p, "rb");
        if (g) got[fread(got, 1, sizeof got - 1, g)] = 0, fclose(g);
        // a commit here pushed by sc_push: the remote has it
        g = re_fopen(p, "wb");
        if (g) fputs("third", g), fclose(g);
        const char* one[1] = { names[0] };
        sc_submit(one, 1, "Third");
        for (int k = 0; k < 800 && sc_busy(); k++) re_sleep_seconds(0.01);
        bool pushed = sc_push();
        for (int k = 0; k < 800 && sc_busy(); k++) re_sleep_seconds(0.01);
        snprintf(cmd, sizeof cmd, "git -C \"%s\" log --oneline -1", bare);
        re_run_capture(cmd, out, sizeof out, &code);
        bool remote_ok = strstr(out, "Third") != NULL;
        printf("[RealEngine] Source control: diff %s, pull %s (\"%s\"), push %s (remote: %s)\n", diff_ok ? "ok" : "WRONG", pulled ? "ran" : "no",
               got, pushed ? "ran" : "no", remote_ok ? "has it" : "MISSING");
        if (!diff_ok || !pulled || strcmp(got, "from the other") || !pushed || !remote_ok)
            printf("[RealEngine] FAIL: source control: diff, pull or push\n"), fails++;
        rm_repo(bare), rm_repo(other);
    }
    sc_init(NULL);
    rm_repo(dir);
    return fails;
}
