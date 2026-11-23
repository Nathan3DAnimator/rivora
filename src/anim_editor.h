// The animation editors (anim_editor.c), Unreal's: the Skeletal Mesh Editor (skeleton tree, parts, preview), the
// Animation Editor (timeline, notifies, the skeleton's other animations), the Montage Editor (slot, blends, sections)
// and the Anim Graph tab of an Animation Blueprint (states, transitions, blend spaces).
#pragma once
#include <stdbool.h>

struct Editor;
struct BpAsset;

void anim_editor_open(const char* vpath); // a .rskm, .ranim or .rmontage ("/Content/...")
// (Q7) the window's tabs (asset_viewport.h AVP_TAB_*): the window on a mesh's Model / Skeleton / Animation tab; its tabs
// (the Physics Asset and IK Rig made for it found in the project); a switch (Physics: one made when it has none; the
// IK Rig / Physics editor takes the same window). Closed: the IK Rig / Physics editor opened in its place
struct SkelMesh;
void anim_editor_open_mesh_tab(struct SkelMesh* m, int tab);
unsigned anim_persona_tabs(const struct SkelMesh* m);
bool anim_persona_switch(struct SkelMesh* m, int tab);
void anim_editor_close(void);
void anim_persona_refresh(void); // (the Physics Asset / IK Rig of a mesh searched again: one was made)
void anim_editor_draw(struct Editor* ed, double now);
// The Content Browser's Add menu (inside an open popup): Animation Blueprint, Anim Montage; true when one was made
bool anim_editor_add_menu(const char* folder_vpath);
// The Blueprint Editor's AnimGraph tab (an Animation Blueprint's): true when the graph changed
bool anim_graph_tab(struct BpAsset* a, float width, float height);
// A Skeletal Mesh component's properties (the level's Details, a Blueprint's component): true when changed
struct CSkeletalMesh;
bool anim_component_details(struct CSkeletalMesh* c);
// The Motion Matching editor on an Animation Blueprint's first Motion Matching state (or node); query: its Query
// Debugger (the command line's --mm-editor: screenshots, tests)
void anim_mm_editor_open_asset(const char* vpath, bool query);
// An Anim Blueprint's first blend space in the Blend Space editor (screenshots, tests)
void anim_blend_space_open_asset(const char* vpath);
// An Anim Blueprint's first transition in its rule window (screenshots, tests)
void anim_transition_open_asset(const char* vpath);
// An Animation Blueprint's preview into the rectangle (screen px): its Skeletal Mesh animated by its graph, the view
// mode, pause and restart; the Viewport (corner false) also gets the Anim Preview Editor (the variables' values).
// False when its Skeleton isn't set (a hint is drawn instead)
bool anim_bp_preview(struct BpAsset* a, float x, float y, float w, float h, bool corner);
