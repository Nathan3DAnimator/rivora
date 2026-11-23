// The async tasks' cards (async_ui.c): bottom right, movable, stacked, a thin mode with only the progress
#pragma once
#include <stdbool.h>
#include <stdio.h>

void async_ui_draw(double now); // after the editor's windows (drawn over them)
void async_ui_write_prefs(FILE* f);
bool async_ui_read_pref(const char* key, const char* value); // editor.ini's async_x / async_y / async_compact
int async_ui_cards_shown(void);                              // (the test)
void async_ui_set_compact(bool compact);
