// The UI sweep (--ui-sweep <dir>; 2026-11-25, item 21): a robot that goes through the whole editor by injected input
// (ImGui's mouse and keys, as a user's): every menu and item (File..Help and their submenus), the toolbar, every
// window those open (panels, Project Settings, Editor Preferences, plugins, the asset editors the Content Browser's
// Create menu makes and opens) and their buttons and tabs, the right click menus of the viewport, the Outliner, the
// Content Browser and a graph; then Play: the HUD, the pause menu and the options, each button clicked.
// Each control is checked: no crash (the report is written as it goes: the control running is the last line), no
// validation error, no ImGui assert, no text without translation (pt/es), no absurd frame, its windows closed after.
// The destructive ones (delete, quit, publish, send a report, Git, package, URLs, language, a new/open project) are
// never clicked; the run is in a project of its own (--new-project). Report: <dir>/ui_sweep.jsonl (one line per
// control) and tools/ui_sweep_report.py makes the Markdown and the bug list (notas-dev/testes.md).
#pragma once
#include <stdbool.h>

struct Editor;

void ui_sweep_start(const char* out_dir);
bool ui_sweep_active(void);
void ui_sweep_input(struct Editor* ed);   // each frame, before igNewFrame (ImGui's input queue)
bool ui_sweep_finished(bool* ok);         // true once done (*ok: no control failed)
// From the ImGui hooks (imgui_hooks.c, crashlog.c): every labeled item drawn, an IM_ASSERT that failed
void ui_sweep_item(const char* label, float x0, float y0, float x1, float y1);
bool ui_sweep_assert(const char* expr, const char* file, int line); // true: recorded, the run goes on
