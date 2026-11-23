// The Blueprint Editor (Unreal's): a window per open asset. Classes: My Blueprint (graphs, functions, variables,
// interfaces), the graph (nodes, wires, the action menu) and Details (the node, the variable, the class's defaults).
// Structures, enumerations, interfaces and data assets: a form.
#pragma once
#include <stdbool.h>

struct Editor;

void bp_editor_open(const char* vpath);          // opens the asset's window (or brings it to the front)
// A variable's value edited as the Blueprint editor's Default Value does (the actor's Details: Instance Editable)
bool bp_value_edit(const char* label, unsigned char type, const char* ref, struct BpValue* v, char* s, size_t scap);
void bp_editor_open_viewport(const char* vpath); // the same, on its Viewport tab (--open-viewport: the screenshots)
int bp_editor_menu_selftest(const char* vpath); // --bp-menu-test: the graph menu (graph_menu.h) over a Blueprint
void bp_editor_draw(struct Editor* ed, double now); // every frame: the open windows
bool bp_editor_focused(void);                    // a Blueprint window has the keyboard (the level's shortcuts wait)
void bp_editor_save_all(struct Editor* ed);      // before playing: what changed saved and compiled
// The Content Browser's Add menu items (inside an open popup): creates the asset in folder_vpath and opens it.
// True when something was created (the folder is read again).
// part: 0 Blueprint Class (Create Basic Asset), 1 the Blueprint submenu (Structure, Enumeration, Interface, Macro
// Library), 2 Widget Blueprint (User Interface), 3 Data Asset (Miscellaneous) - Unreal's Content Browser menu
bool bp_editor_add_menu(const char* folder_vpath, int part);
// Right click on a class in the Content Browser: a Child Blueprint Class next to it, opened. False when it couldn't.
bool bp_editor_create_child(const char* class_vpath, bool open);
// The Blueprints' test (bp_test.c): the assets and the level made and played; the world checked afterwards
bool bp_test_setup(struct Editor* ed, const char* package_dir); // package_dir: also saved and packaged there (NULL: not)
bool bp_test_check(struct Editor* ed);
bool bp_test_construct_editor(struct Editor* ed); // the Construction Script in the editor (sugestão #64)
bool umg_test_setup(struct Editor* ed); // --umg-test (umg_test.c): a Widget Blueprint made, shown, clicked
bool umg_test_check(struct Editor* ed);
bool seq_test_setup(struct Editor* ed); // --seq-test (seq_test.c): a Level Sequence played by a Blueprint in PIE
bool audio_test_setup(struct Editor* ed); // --audio-test (audio_test.c): sounds in PIE (Ambient Sound, components, nodes)
bool audio_test_check(struct Editor* ed);
bool script_test_setup(struct Editor* ed); // --script-test (script_test.c): the same class in Real, C#, Python and C in PIE
bool script_test_check(struct Editor* ed);
bool script_ide_demo_setup(struct Editor* ed); // --ide-demo: the Code Editor and the API reference shown (a picture)
// --async-test <file> (async_test.c): import, meshes, materials and steps in the background while the editor draws
void async_test_set_source(const char* path);
void async_test_frame(struct Editor* ed, double dt);
bool async_test_running(void);
bool async_test_check(struct Editor* ed);
// --assistant-test (assistant_test.c): the Assistant's base fetched, its answers, the offline copy
void assistant_test_frame(struct Editor* ed);
bool assistant_test_done(void);
bool assistant_test_check(struct Editor* ed);
void seq_test_frame(struct Editor* ed);
bool seq_test_check(struct Editor* ed);
