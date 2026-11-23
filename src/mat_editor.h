// The Material Editor (mat_editor.c): Blender's / the Tlender's shader nodes; Material Instances' parameters
#pragma once
#include "material.h"
// (2026-11-23) Textures dropped onto the graph (onto a Principled BSDF: each into its input by name, organized); the count
int mat_editor_drop_textures(MatGraph* g, Material* m, char (*paths)[512], int n, int pbr, float x, float y);
#include <stdbool.h>

struct Editor;

void mat_editor_open(const char* vpath);           // a .rmat or .rmi ("/Content/...")
void mat_editor_draw(struct Editor* ed, double now); // every frame
// The Content Browser's Add menu items (inside an open popup): Material, Material Instance (of a material); true when
// one was made (the folder is read again)
// part: 0 Material (Create Basic Asset), 1 Material Instance (Create Advanced Asset > Material)
bool mat_editor_add_menu(const char* folder_vpath, int part);
bool mat_editor_focused(void);

// A lit sphere with a material's baked base color (its glow, a highlight from its roughness) in the square at o, turned
// by yaw / pitch; nu x nv quads (40 x 20 at most). (cimgui's types: include cimgui.h with its structs first.)
struct Material;
#ifdef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
void mat_sphere_draw(ImDrawList* dl, ImVec2 o, float size, const struct Material* m, ImTextureID tex, ImTextureID emis, float yaw,
                     float pitch, int nu, int nv);
#endif
void mat_editor_close(void); // (Q7: the asset viewport test)
