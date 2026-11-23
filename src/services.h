// The editor's own services (copied from the Tlender's and adapted): crash reports (crashlog.h) with the editor's
// state and an emergency save of the level, last run's crash (asked whether to send it), Help > Report a Bug (written
// next to the executable and sent) and Help > Send a Suggestion (sent through the relay shared with the Tlender, an
// outbox flushed on a thread: report_upload.h), and the updates (updater.h): checked on a thread at startup,
// a badge in the status bar, the signed package downloaded on a thread and installed by realengine_updater.exe when
// the editor closes.
#pragma once
#include "editor.h"

// Before the window: crash handler, log file, stdout/stderr into the log
void services_early_init(void);
// After the editor exists: crash hooks and GPU, last run's crash, the update check
void services_init(Editor* ed);
// Every frame (inside the ImGui frame): thread results, dialogs
void services_frame(Editor* ed);
// Help menu items; the status bar's update badge (returns true when it drew one)
void services_help_menu(Editor* ed);
// Send a Suggestion opened with a title and a text written in (the user reads and sends it)
void services_suggest_prefill(const char* title, const char* body);
bool services_status_badge(Editor* ed);
// After the main loop: the installer started when an update is ready
void services_on_exit(Editor* ed);
