// The Sequencer window (sequencer_ui.c): a Level Sequence's tracks and keys on a timeline, previewed in the level
#pragma once
#include <stdbool.h>

struct Editor;

void seq_editor_open(const char* vpath);
bool seq_editor_add_menu(const char* folder_vpath);        // the Content Browser's Add menu (Cinematics)
void seq_editor_draw(struct Editor* ed, double now, float dt);
void seq_editor_restore(struct Editor* ed);                 // the actors as before the preview (play starts, a level opens)
bool seq_editor_is_open(void);
bool seq_editor_focused(void);  // (the window has the keys: the editor's own shortcuts wait, as for the Blueprint windows)
bool seq_editor_selftest(void); // (seq_test.c: the window's undo and redo)
