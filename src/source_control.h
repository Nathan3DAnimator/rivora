// Source Control (B14, notas-dev/editor.md "Controle de versão"): Git in the editor, as Unreal's Source Control (Git
// plugin). The project's folder in a Git repository: each asset's state (new, modified, deleted) read by `git status` on
// a thread (the editor never waits) and shown on its Content Browser tile; Submit Content (File > Source Control) adds
// the chosen files and commits them with a message; Git LFS tracks the binary assets when asked. Nothing runs without Git
// installed or outside a repository.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SC_NONE = 0, SC_NEW, SC_MODIFIED, SC_DELETED, SC_ADDED }; // (untracked; changed; gone; staged as new)
typedef struct ScFile { char path[300]; uint8_t state; } ScFile; // path: the repository's, '/' separated

void sc_init(const char* project_dir);      // the repository the project is in (none: everything answers "off")
bool sc_active(void);                       // a repository and Git found
const char* sc_root(void);                  // the repository's top folder ("": none)
void sc_refresh(void);                      // `git status` again on its thread
bool sc_busy(void);                         // a status or a submit running
int sc_state(const char* vpath);            // a Content asset's state ("/Content/..."; SC_NONE: clean or unknown)
uint32_t sc_changed(const ScFile** out);    // every changed file under the project (the last status)
// The chosen files added and committed with the message (on its thread; then a status). False: not started
bool sc_submit(const char* const* paths, uint32_t n, const char* message);
const char* sc_last_message(void);          // the last submit's / LFS's result for the editor's status bar
bool sc_lfs_track(void);                    // `git lfs track` for the engine's binary assets (.rmesh, .rtex...)
// (B14) on the thread, then a status (false: not started): the remote's commits brought in (fast-forward only), this
// repository's sent; a Content asset locked or unlocked for the others (Git LFS's locks: a binary edited by one at a time)
bool sc_pull(void);
bool sc_push(void);
bool sc_lock(const char* vpath, bool lock);
// A file's changes since the last commit (`git diff`, the repository's path; a binary: Git's one line); its length
uint32_t sc_diff(const char* repo_path, char* out, size_t cap);
void sc_set_identity(const char* name, const char* email); // (the tests: a commit's author without the user's git config)
int sc_selftest(void);                  // (--sc-test) a temporary repository: states, a submit, a change
void sc_window(bool* open);                 // File > Source Control > Submit Content: the changed files, a message, Submit
