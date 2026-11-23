// The scripts' IDE (script_ide.c; notas-dev/scripts.md): a code editor inside the engine for Real, C#, Python and C —
// colors by language, completion that knows the class, its locals and the engine's API, the signature of the call
// being typed, errors under the code as it's typed (compiled half a second after a pause), go to definition (F12),
// find / replace, undo, comments, auto indent; and the API reference (a floating window, searchable, examples in the
// four languages). Editor utilities (scripts and Blueprints that add tools to the editor) run from its menu.
#pragma once
#include <stdbool.h>

struct Editor;

void script_ide_open(const char* vpath);
void script_ide_open_at(const char* vpath, int line);
bool script_ide_add_menu(const char* folder_vpath);     // the Content Browser's Add > Script
void script_ide_draw(struct Editor* ed, double now, float dt);
bool script_ide_focused(void);                          // (the editor's shortcuts wait)
void script_api_reference_open(const char* name);       // the API reference at a function (NULL: its list)
void script_api_reference_draw(void);
// Editor utilities: the classes whose parent is "Editor Utility" (scripts and Blueprints): their Custom Events as the
// tools' buttons (Tools > Editor Utilities), run on the editor's level (undoable)
void editor_utilities_draw(struct Editor* ed);
void editor_utilities_bind(struct Editor* ed);
void script_ide_demo(const char* vpath, int line, int col, const char* word); // (--ide-demo: the IDE as a picture shows it) // the selection's nodes (Get Selected Actor...) read this editor's
void editor_utilities_open(void);
