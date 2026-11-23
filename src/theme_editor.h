// Editor Preferences > Appearance: the themes and the Theme Editor (theme_editor.c, notas-dev/temas-e-janelas.md)
#pragma once
#include <stdbool.h>

struct GLFWwindow;
// The page's contents (inside the preferences' page child); true when the theme changed (the preferences are saved)
bool theme_editor_page(struct GLFWwindow* win);
