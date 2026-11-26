// The asset editors' open documents, all of them (2026-11-25, bug-fixer: switching projects didn't close the editors).
// Switching projects and quitting close them, with the save prompt, before the systems let go of their assets. Before,
// editor_open_project's bp_shutdown() freed the Blueprints with their windows still pointing at them (bp_editor.c's
// g_win[].a): a use after free on the next frame, and the old project's editors went on open in the new one.
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct Editor;

// What each editor's hook does to its documents (it returns how many it counted or acted on)
enum {
    DOCS_OPEN = 0, // how many are open
    DOCS_UNSAVED,  // how many have unsaved changes (their names appended to names, one a line)
    DOCS_SAVE,     // the unsaved ones saved: returns how many could not be
    DOCS_CLOSE,    // all closed, unsaved or not (their previews let go): returns how many were open
};

// The hooks (each editor's file; ed may be NULL for DOCS_OPEN and DOCS_UNSAVED)
int bp_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int mat_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int asset_viewer_docs(int op, struct Editor* ed, char* names, size_t cap);
int anim_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int ikrig_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int ai_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int input_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int seq_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int audio_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int script_ide_docs(int op, struct Editor* ed, char* names, size_t cap);
int particle_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int vehicle_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int dlg_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int quest_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int physmat_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int camshake_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int pcg_editor_docs(int op, struct Editor* ed, char* names, size_t cap);
int ui_designer_docs(int op, struct Editor* ed, char* names, size_t cap); // (the UMG Designer's state: counts none)
const void* ui_designer_bound(void);                                  // (the test's: the Blueprint it shows)
const char* asset_viewer_test_thumb(const char* vpath, bool add);     // (the test's: a thumbnail's file, asset_viewer.c)
void editor_docs_add_name(char* names, size_t cap, const char* name); // (the hooks' DOCS_UNSAVED)

int editor_docs_open(void);                        // every editor's open documents
int editor_docs_unsaved(char* names, size_t cap);  // the unsaved ones (names: one a line; may be NULL)
int editor_docs_save(struct Editor* ed);           // the unsaved ones saved: how many could not be
// The end of the documents (a project switch, the editor closing): saved first unless the prompt's Don't Save said
// otherwise, then all closed. Before the systems free what they point at, and before ImGui's shutdown
void editor_docs_finish(struct Editor* ed);
void editor_docs_discard(void);                    // (the prompt's Don't Save: the next finish closes without saving)
// The editor closing: the Blueprints saved as they always were (unless Don't Save), the rest closed as they are
// (the quit prompt asked; a test's edits never written into the build's Content), then all closed
void editor_docs_finish_quit(struct Editor* ed);

// A project switch asked for (editor_open_project_later): true when the save prompt is up (the switch waits for it)
bool editor_docs_switch_ask(struct Editor* ed, const char* rproject, int tpl);
void editor_docs_popup(struct Editor* ed, double now); // the switch's prompt (each frame, with the other popups)
bool editor_docs_prompt_up(void);
// The test project_switch (project_switch_test.c, --project-switch-test)
void psw_test_start(void);
bool psw_test_frame(struct Editor* ed, bool* ok);
void editor_docs_test_answer(int answer); // (the test's: 1 Save and Open, 2 Don't Save, 3 Cancel on the next frame)
