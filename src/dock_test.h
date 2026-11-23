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
