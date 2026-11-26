// The Editors window (2026-11-25, item 9): Unreal's asset editor window. An asset editor opened (a Graph, a Material,
// the Static Mesh / Texture viewer, the Skeletal Mesh editor, a Particle System) goes in as a tab beside the others
// already open, whatever their kinds; any tab drags out to a window of its own, docks back, docks beside the level
// editor's panels. The window shows while it holds a document and goes when the last one leaves; dragged out of the
// editor it is a real window owned by the editor's (ui_viewports.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct Editor;

// Each frame, before the asset editors draw (editor.c)
void edhost_frame(struct Editor* ed);
// Before an asset editor's igBegin: the documents' window class; *to_host (set when it opened): it docks into the
// Editors window (cleared once done: a later drag out stays where the user put it)
void edhost_doc(bool* to_host);
// The documents in the Editors window (the last frame), its dock space, whether it is up, the class's id (the test)
int edhost_docs(void);
unsigned edhost_dockspace(void);
bool edhost_shown(void);
unsigned edhost_class_id(void);
// A project switch, the editor closing: the window forgets where it was (the next document places it again)
void edhost_reset(void);
// The layout's documents: the editors open written as "[RealEngine][Documents]" lines (editor_dock.c's layout text),
// and opened again from a project's layout (those whose files still exist)
size_t edhost_layout_write(char* out, size_t cap);
int edhost_layout_read(const char* text);
