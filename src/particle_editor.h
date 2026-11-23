// The Particle System editor (particle_editor.c): Unreal's Niagara Editor, its emitters and stacks of modules
#pragma once
#include <stdbool.h>

struct Editor;
void particle_editor_open(const char* vpath);
bool particle_editor_handles(const char* vpath);          // a .rparticle
bool particle_editor_add_menu(const char* folder_vpath);  // Content Browser > Add > Visual Effects (the templates)
void particle_editor_draw(struct Editor* ed);             // every frame
int particle_editor_preview_count(void); // (Q7, the asset viewport test) the particles its preview drew last frame
void particle_editor_close(void); // (Q7: the asset viewport test)
