// The Vehicle editor (vehicle_editor.c): a Vehicle asset's chassis, gears and parts, seen from above and from the side
#pragma once
#include <stdbool.h>

struct Editor;
void vehicle_editor_open(const char* vpath);
bool vehicle_editor_handles(const char* vpath);          // a .rvehicle
bool vehicle_editor_add_menu(const char* folder_vpath);  // Content Browser > Add > Vehicles (the templates)
void vehicle_editor_draw(struct Editor* ed);             // every frame
void vehicle_editor_close(void); // (Q7: the asset viewport test)
