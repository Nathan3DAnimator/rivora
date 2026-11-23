// The Assistant's window (assistant_ui.c)
#pragma once
#include <stdbool.h>

// An action of an article the editor runs ("open:package", "add:PointLight", "run:play", "suggest:<question>"):
// true when it knew it
typedef bool (*AssistantActionFn)(const char* action);
void assistant_ui_set_action(AssistantActionFn fn);
void assistant_ui_open(const char* query); // (NULL: as it was)
void assistant_ui_show_article(const char* id);
void assistant_ui_draw(void); // each editor frame (also the "Show me where" outlines)
bool assistant_ui_is_open(void);
// Its preferences in editor.ini (how a recipe shows: Blueprint, Real, C#, Python or C)
#include <stdio.h>
void assistant_ui_write_prefs(FILE* f);
bool assistant_ui_read_pref(const char* key, const char* value);
