// Content Browser / Content Drawer (Unreal's): the project's Content folder and the engine's BasicShapes. Folder tree,
// back/forward and breadcrumb, recursive search, tiles with type icons and colors, new folder, rename, delete (to the
// Recycle Bin), import (dialog or dropping files from Explorer), drag and drop (into folders to move, into the
// viewport to place). The folder is read on demand (open, navigation, changes, the window regaining focus), never
// every frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum AssetKind {
    ASSET_FOLDER = 0,
    ASSET_BASIC_SHAPE, // /Engine/BasicShapes: a Static Mesh made by the engine
    ASSET_RASSET,      // .rasset (meshes, materials, textures: the format shared with the Tlender)
    ASSET_LEVEL,       // .rlevel
    ASSET_TEXTURE_SOURCE, // png, jpg, tga, exr, hdr (imported into .rasset in step 2)
    ASSET_MESH_SOURCE, // fbx, obj, gltf, glb
    ASSET_SOUND_SOURCE, // wav, ogg
    ASSET_BLUEPRINT,   // .rbp: a Blueprint Class (blueprint.h)
    ASSET_BP_STRUCT,   // .rstruct
    ASSET_BP_ENUM,     // .renum
    ASSET_BP_INTERFACE, // .rinterface
    ASSET_DATA_ASSET,  // .rdata
    ASSET_MATERIAL,    // .rmat (material.h)
    ASSET_MATERIAL_INSTANCE, // .rmi
    ASSET_SKELETAL_MESH, // .rskm (anim.h)
    ASSET_ANIMATION,     // .ranim
    ASSET_ANIM_MONTAGE,  // .rmontage
    ASSET_STATIC_MESH,   // .rmesh: an imported mesh (render.h)
    ASSET_BEHAVIOR_TREE, // .rbt (ai.h)
    ASSET_BRAIN,         // .rbrain
    ASSET_INPUT_ACTION,  // .rinputaction (input.h)
    ASSET_INPUT_CONTEXT, // .rimc
    ASSET_MACRO_LIB,     // .rmacros (blueprint.h)
    ASSET_LEVEL_SEQUENCE, // .rseq (sequencer.h)
    // Audio (audio.h): Sound Wave .rsound, Sound Cue .rscue, Sound Attenuation .rsatt, Sound Class .rsclass, Sound Mix
    // .rsmix, Sound Concurrency .rsconc, Submix .rsubmix
    ASSET_SOUND_WAVE, ASSET_SOUND_CUE, ASSET_SOUND_ATTENUATION, ASSET_SOUND_CLASS, ASSET_SOUND_MIX, ASSET_SOUND_CONCURRENCY,
    ASSET_SUBMIX,
    ASSET_SCRIPT, // a script (script.h): .real, .cs, .py, .c — a class like a Blueprint's
    ASSET_PARTICLE_SYSTEM, // .rparticle (particles.h): Unreal's Niagara System
    ASSET_IK_RIG,          // .rikrig (ikrig.h): Unreal's IK Rig
    ASSET_PHYSICS_ASSET,   // .rphys: Unreal's Physics Asset (the bones' bodies, ragdoll)
    ASSET_VEHICLE,         // .rvehicle (vehicle.h): a vehicle's chassis and parts
    ASSET_DIALOGUE,        // .rdlg (dialogue.h): a conversation's lines, choices, conditions and actions
    ASSET_QUEST,           // .rquest (quest.h): a quest's stages, objectives and rewards
    ASSET_METASOUND,       // .rmsnd (metasound.h): a MetaSound Source, a sound made by a graph of DSP nodes (B8)
    ASSET_PHYS_MATERIAL,   // .rphysmat (physmat.h, Q8): Unreal's Physical Material (friction, bounce, density)
    ASSET_CAMERA_SHAKE,    // .rshake (camshake.h, Q12): Unreal's Camera Shake (oscillations, Perlin noise, curves)
    ASSET_PCG_GRAPH,       // .rpcg (pcg.h, Q15): Unreal's PCG Graph (points sampled, filtered, transformed and spawned)
    ASSET_OTHER,
    ASSET_KIND_COUNT
} AssetKind;

// The payload of an asset being dragged (ImGui drag and drop type RE_PAYLOAD_ASSET)
#define RE_PAYLOAD_ASSET "RE_ASSET"
typedef struct AssetPayload {
    AssetKind kind;
    int shape;          // ASSET_BASIC_SHAPE: the BasicShape
    char vpath[512];    // "/Content/Props/Rock.rasset"
} AssetPayload;
// (2026-11-23) The assets a drag carries: the dragged one and the others selected with it (the dragged one first); the count
int content_drag_paths(const char* vpath, char (*out)[512], int cap);

// root: the folder /Content maps to (created when missing)
void content_init(const char* root);
// The panel in a rectangle; docked: shown as a fixed panel ("Dock in Layout") rather than the drawer.
// Returns false when the user closed it (Close / Esc) or asked to toggle docking (*toggle_dock set).
bool content_draw(float x, float y, float w, float h, bool docked, bool* toggle_dock);
// Read the current folder again on the next draw (the window got the focus back, files were dropped)
void content_mark_dirty(void);
// Copies files from Explorer into the current folder (/Content when an engine folder is shown)
void content_import(int count, const char** paths);
const char* content_root(void);
const char* asset_kind_name(AssetKind k);
// The panel had the keyboard focus when it was last drawn (the drawer hides when it loses it)
bool content_focused(void);
// The drawer's top edge dragged this frame (px, down: positive): the editor resizes it (2026-11-10)
float content_drawer_drag(void);
// Shows the asset's folder with the asset selected (the Blueprint Editor's Browse)
void content_browse_to(const char* vpath);
void content_test_hover(const char* vpath); // (--hover: that asset drawn as hovered, the tests' screenshots)
// An importer for files that become assets rather than being copied (the editor's: .tlend, .obj, .gltf, .glb; one per extension). Returns false on failure;
// msg: what it did; place: its objects also become actors in the level (the Import Options' choice)
typedef bool (*ContentImporter)(const char* src, const char* folder_vpath, bool place, char* msg, size_t cap);
void content_set_importer(const char* ext, ContentImporter fn);
// (2026-11-23) A folder from Explorer into <dst_dir>/<its name>: its subfolders mirrored, its models imported (now) or
// queued for the Import Options window, its images and sounds copied, its textures used in place by the models
bool content_import_folder(const char* src, const char* dst_dir, bool now, bool place);
// Unreal's Reimport: a Skeletal Mesh's source file (its "source"; asked by the Open dialog when unknown or moved) read
// again into its folder, the same names overwritten; the actors using it pick the new one up. False: nothing started
bool content_reimport(const char* vpath);
// Unreal's Import Options window for the files an importer converts (drawn every frame: a drop can come any time)
void content_import_options(void);
// The editor's levels: open one (a double click), write a new empty one (Add > Level) — disk paths
typedef struct ContentLevelHooks {
    bool (*open)(const char* path);
    bool (*create)(const char* path);
    void (*save_all)(void); // the toolbar's Save All (the editor's: the level and the changed assets)
    // (2026-11-24, the Filters) the level at that disk path has unsaved changes; the assets the level's actors use (their
    // vpaths' hashes, bp_vpath_hash: meshes, materials, classes, sounds...), their count
    bool (*unsaved)(const char* path);
    int (*used_assets)(uint32_t* out, int cap);
} ContentLevelHooks;
void content_set_level_hooks(const ContentLevelHooks* h);
// The editors call it first when asked to open an asset: true when the Content Browser just made it (Add): it is
// renamed first and opened when the name is typed (Unreal's way), so the editor doesn't open it now
bool content_defer_open(const char* vpath);
// Opens an asset's editor (or a level) by its extension
void content_open_asset(const char* vpath);
// File > Import Into Level: the import dialog (the Content Browser's Import)
void content_import_dialog(void);
// The assets whose names contain filter (any case) under /Content: up to cap of their "/Content/..." paths; their count
int content_search(const char* filter, char (*out)[512], int cap);
// (2026-11-10) a Static Mesh's own material, the one its import made (Unreal's material slot): a hidden ".<name>.mat"
// beside the .rmesh (its hash in hex). The Static Mesh viewer shows it (textured), a mesh dragged into the level gets it,
// the thumbnail takes its color. 0: none
uint32_t content_mesh_material(const char* mesh_vpath);
void content_mesh_material_set_disk(const char* mesh_disk_path, uint32_t mat_hash);

// --- (Q6, 2026-11-17) the asset picker's and the creations' share of the Content Browser ---
struct ImDrawList;
AssetKind content_kind_of(const char* name);  // by the file's extension
int content_kind_color(AssetKind k);          // its theme token (TH(...)): the tiles' bar
int content_kind_icon(int kind, const char* vpath); // its UiIcon (a Blueprint's: by its class)
// The tile's thumbnail of an asset into a rectangle: a Blueprint's class preview, a texture's, a mesh's, a material's
// sphere, a sound's wave (an animation plays while hovered). False: none (yet): the caller draws the kind's icon
bool content_thumb(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, int kind, bool hovered);
// A new asset made by a menu: named in the Content Browser first when it is in the folder shown, then opened
void content_created(const char* vpath);
void content_message(const char* fmt, ...); // the footer's note

// Create from the selection (content_create.c): what the right click on an asset makes from it, by its type
enum {
    CM_IK_RIG = 0, CM_PHYSICS_ASSET, CM_RAGDOLL, CM_ANIM_BLUEPRINT, CM_SKELETON, // a Skeletal Mesh
    CM_BLUEPRINT, CM_PHYSICS_ACTOR, CM_LODS, CM_CLOTH, CM_VEHICLE, CM_SOFT_BODY, // a Static Mesh
    CM_MATERIAL_INSTANCE,                                                       // a Material
    CM_MATERIAL, CM_LIGHT_FUNCTION,                                             // a Texture
    CM_MONTAGE, CM_BLEND_SPACE,                                                 // an Animation
    CM_COUNT
};
// Makes it from src ("/Content/..."), in dest_folder (NULL: src's folder): out the new asset's path, msg what went wrong
// (or what it did). The menus call it; the tests too
bool content_make(const char* src_vpath, int make, const char* dest_folder, char* out, size_t cap, char* msg, size_t mcap);
int content_make_options(int kind, int* out, int cap); // the makes a kind offers (the right click's order)
const char* content_make_label(int make);              // English (TR)
int content_make_from_kind(int make);
bool content_create_menu(const char* vpath, int kind, bool editable); // the right click's Create section (in its popup)
// The Add menu's single item for a make that needs a base asset: the picker of that kind, then made in folder_vpath
void content_pick_and_make(int make, const char* folder_vpath);
// The level editor's part: a Cloth or a Vehicle placed in the level from a Static Mesh, the meshes read again after LODs
typedef struct ContentEditorHooks {
    bool (*actor_from)(const char* mesh_vpath, int make, char* msg, size_t cap);
    void (*meshes_changed)(void);
} ContentEditorHooks;
void content_set_editor_hooks(const ContentEditorHooks* h);

// --- (2026-11-24) Filters, as Unreal's Content Browser Filters (content.c): the type filters that are on add up (OR),
// the Other filters and the search narrow them (AND); any filter on lists the current folder and below without folders ---
typedef enum CbFilterId {
    CBF_BLUEPRINT = 0, CBF_WIDGET, CBF_MATERIAL, CBF_MATERIAL_INSTANCE, CBF_STATIC_MESH, CBF_SKELETAL_MESH, CBF_TEXTURE,
    CBF_SOUND, CBF_LEVEL, CBF_ANIMATION, CBF_PARTICLES, CBF_SEQUENCE, CBF_DATA_ASSET, CBF_STRUCT, CBF_ENUM, CBF_AI,
    CBF_INPUT, CBF_SCRIPT, CBF_IK_RIG, CBF_PHYSICS, CBF_VEHICLE, CBF_DIALOGUE, CBF_QUEST, CBF_CAMERA_SHAKE, CBF_PCG,
    CBF_MODEL_FILE, CBF_OTHER_FILE,
    CBF_FIRST_OTHER, // the Other Filters
    CBF_FAVORITES = CBF_FIRST_OTHER, CBF_RECENT, CBF_MODIFIED, CBF_IN_LEVEL,
    CBF_COUNT
} CbFilterId;
int content_filter_count(void);
const char* content_filter_label(int id);                // English (TR)
void content_filter_set(int id, bool added, bool on);    // added: its chip shown; on: it filters
void content_filter_get(int id, bool* added, bool* on);
void content_filters_clear(void);
bool content_is_favorite(const char* vpath);
void content_set_favorite(const char* vpath, bool on);
double content_filter_scan_ms(void);                     // the last folder read's time (with the filters)
// The folder (and the search) listed now, as the panel would (the tests): the count, then each item's vpath
int content_test_list(const char* folder_vpath, const char* search);
const char* content_test_item(int i);
// editor.ini: the chips, which are on, the favorites and the recent assets; a change to write (taken once)
#include <stdio.h>
void content_write_prefs(FILE* f);
bool content_read_pref(const char* key, const char* v);
bool content_take_prefs_changed(void);
