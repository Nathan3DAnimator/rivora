// The Enhanced Input assets' editors (input_editor.c): Input Action, Input Mapping Context
#pragma once
#include <stdbool.h>

void input_editor_open(const char* vpath);           // a .rinputaction / .rimc
bool input_editor_add_menu(const char* folder_vpath); // the Content Browser's Add > Input
void input_editor_draw(void);
