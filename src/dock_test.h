// The modular windows' UI test (--dock-test, dock_test.c; notas-dev/temas-e-janelas.md)
#pragma once
#include <stdbool.h>

struct Editor;
struct GLFWwindow;
void dock_test_start(struct GLFWwindow* main_window, bool viewports);
bool dock_test_running(void);
bool dock_test_finished(bool* ok);
void dock_test_input(struct Editor* ed); // before igNewFrame (ImGui's input queue)
void dock_test_frame(struct Editor* ed); // in the frame (its window)
// (2026-11-25, item 9) the second part: the asset editors in the Editors window, windows of their own owned by the
// editor's, a project switch, the editor closed with them up (dock_test_docs.c)
bool dock_docs_step(struct Editor* ed, struct GLFWwindow* main_window, bool* test_window, bool* ok);
bool dock_test_after_shutdown(void); // after ImGui's shutdown: false when a window of its own is left
