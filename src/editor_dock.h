// The level editor's modular windows (Q2, notas-dev/temas-e-janelas.md): every panel (Place Actors, Outliner, Levels,
// Details, the docked Content Browser) is a window docked around the viewport, dragged anywhere (another monitor too);
// while one is dragged every place it can go shows (each panel's center and 4 sides, the outer edges, the gaps between
// panels); layouts saved and loaded by name, per project and per user (Window > Layout), the last one back on open.
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct Editor;

// (Q26: Modes beside Place Actors, the Output Log beside the Content Browser, Platform Support under Details)
typedef enum DockPanel { DP_VIEWPORT, DP_PLACE, DP_OUTLINER, DP_DETAILS, DP_CONTENT, DP_LEVELS, DP_MODES, DP_LOG, DP_PLATFORMS, DP_COUNT } DockPanel;

const char* dock_panel_id(DockPanel p);  // "###re_outliner" (the window's name after its title)
// Inside the editor's host window, between the toolbar and the status bar: the dock space (the default layout built
// the first time and by Window > Layout > Default)
void dock_host(struct Editor* ed, float x, float y, float w, float h);
// A panel's window (docked by default where the layout puts it); true: draw its contents. dock_panel_end always.
bool dock_panel_begin(DockPanel p, const char* title, bool* open, int extra_flags);
void dock_panel_end(void);
// After every window of the frame: the docking targets while a window is dragged, a drop on a gap between panels
void dock_frame_end(struct Editor* ed);
// Between frames (before igNewFrame): a layout to load is applied there
void dock_between_frames(struct Editor* ed);
// Window > Layout (save, load, default)
void dock_layout_menu(struct Editor* ed);
void dock_layout_popups(struct Editor* ed);
// The layouts: the current one into a file (project: <project>/Config/Layouts, else %APPDATA%\RealEngine\Layouts)
bool dock_layout_save(struct Editor* ed, const char* name, bool project);
bool dock_layout_load_file(struct Editor* ed, const char* path); // applied between frames
void dock_layout_default(struct Editor* ed);
// The last layout: saved when the editor closes or another project opens, loaded back when it opens
void dock_layout_autosave(struct Editor* ed);
// A close that reached the editor's main window while another of its windows (a window dragged out of the editor,
// maximized) was in front is that one's: it closes, never the engine. True: rerouted (main.c, before the quit check)
struct GLFWwindow;
bool dock_route_main_close(struct GLFWwindow* main_window);
void dock_route_test_foreground(void* hwnd); // (the test: the window taken as the one in front)
// (the tests) the dock space's node of a panel's window and its rectangle; the dock space's
bool dock_panel_rect(DockPanel p, float r[4]);
bool dock_space_rect(float r[4]);
unsigned dock_space_id(void);
// (the dock test) the window maximized by a double-click on its title bar (its old place kept: another one restores it)
bool dock_window_zoomed(const char* name);
