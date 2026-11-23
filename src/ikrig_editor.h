// The IK Rig and Physics Asset editors (ikrig_editor.c): Unreal's IK Rig Editor and Physics Asset Editor, simpler
#pragma once
#include <stdbool.h>

struct Editor;
struct CSkeletalMesh;
void ikrig_editor_open(const char* vpath);
void ikrig_editor_close(void); // (Q7: the animation editors take the window)
bool ikrig_editor_handles(const char* vpath);          // a .rikrig or a .rphys
bool ikrig_editor_add_menu(const char* folder_vpath);  // Content Browser > Add > Animation: IK Rig, Physics Asset (from a Skeletal Mesh)
void ikrig_editor_draw(struct Editor* ed);             // every frame
bool ikrig_component_details(struct CSkeletalMesh* c); // the Skeletal Mesh's Details: its IK Rig, Physics Asset, simulation
int ikrig_editor_selftest(void);                       // (the editors' preview math: projection, a drag solved)
