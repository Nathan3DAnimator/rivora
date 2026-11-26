// The asset picker (Q6, 2026-11-17; notas-dev/content-browser.md): Unreal's Asset Picker / Pick Parent Class window. A
// modal window with a search (every word, the name or the folder), the types and the folder to filter by, the assets'
// real thumbnails (the Content Browser's), and in its corner the view: Preview only / Name only / Preview and name and
// the preview's size Small / Medium / Large (kept in editor.ini). Every Details field of an asset (asset_field) and
// every creation that needs a base asset (IK Rig, Physics Asset, Ragdoll, Anim Blueprint, a Blueprint's parent class,
// Montage...) opens it instead of listing the assets by name in a menu.
//
// The candidates: the /Content files of the kinds asked for (read once when it opens, never every frame), plus the ones
// the caller adds (a registry's: the native classes, the basic shapes). Filtering builds an index list only when the
// search, the types or the folder change; only the visible rows are drawn.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define AK(kind) (1ull << (kind)) // a kind's bit in AssetPickerSpec.kinds (AssetKind, content.h)

// What happens when an asset is chosen: ref is its "/Content/..." path or the caller's own reference (asset_picker_add),
// "" for None; user: the caller's number of an added entry (0 for a file); ctx: AssetPickerSpec.ctx as it was given
typedef void (*AssetPickFn)(const char* ref, uint32_t user, const char* ctx);

typedef struct AssetPickerSpec {
    const char* title;   // the window's title ("Pick Skeletal Mesh")
    const char* hint;    // a line under it (what it is for); NULL: none
    uint64_t kinds;      // the /Content files listed (AK(ASSET_...) bits); 0: only the entries added
    const char* current; // the ref selected when it opens (NULL: none)
    bool allow_none;     // a None button (a field that can be empty)
    uint32_t owner;      // a field's ImGui id (asset_picker_result); 0 for a callback
    AssetPickFn on_pick; // called once when an asset is chosen (NULL: a field's result)
    const char* ctx;     // copied (512 bytes): what on_pick needs (the folder to create in)
} AssetPickerSpec;

void asset_picker_open(const AssetPickerSpec* spec);
// An entry of the caller's own after opening (before the next draw): label shown, ref given back, its kind (the
// thumbnail and filter), an icon (UiIcon; -1: the kind's), a group (0 first: Unreal's Common Classes) and a number
void asset_picker_add(const char* label, const char* ref, int kind, int icon, int group, uint32_t user);
void asset_picker_add_tip(const char* tip); // the last added entry's description (kept as a pointer: a static text)
bool asset_picker_is_open(void);
void asset_picker_close(void);
void asset_picker_draw(void); // every editor frame (at the top level, not inside a window)

// A field's answer: true once, the frame after its asset was chosen (ref: "" when None)
bool asset_picker_result(uint32_t owner, char* ref, size_t cap, uint32_t* user);

// Unreal's asset field in the Details: the asset's thumbnail and name; a click opens the picker on these kinds, an asset
// dragged from the Content Browser drops on it. Returns ASSET_FIELD_OPENED the frame the picker opens (the caller may
// asset_picker_add its own entries then), ASSET_FIELD_PICKED when an asset was chosen (out: its ref, "" for None)
enum { ASSET_FIELD_IDLE = 0, ASSET_FIELD_OPENED, ASSET_FIELD_PICKED };
int asset_field(const char* id, const char* shown, const char* cur_ref, int kind, uint64_t kinds, bool allow_none,
                char* out, size_t cap);
void asset_field_open_next(void); // (2026-11-25, --show bp-var) the next asset_field drawn opens its picker as if clicked

// The view (the corner button): 0 Preview only, 1 Name only, 2 Preview and name; size 0 Small, 1 Medium, 2 Large
enum { PICKER_VIEW_PREVIEW = 0, PICKER_VIEW_NAME, PICKER_VIEW_BOTH, PICKER_VIEW_COUNT };
void asset_picker_set_view(int view, int size);
void asset_picker_get_view(int* view, int* size);
void asset_picker_write_prefs(FILE* f);                 // editor.ini (editor.c)
bool asset_picker_read_pref(const char* key, const char* v);

// The tests' hooks (picker_test.c) and the measures
void asset_picker_set_filter(const char* text);
int asset_picker_count(void);       // the candidates
int asset_picker_visible(void);     // after the search and filters
const char* asset_picker_visible_ref(int i);
void asset_picker_choose(int visible_index); // as a double click
void asset_picker_layout(int* per_row, float* row_h); // the last draw's grid
double asset_picker_scan_ms(void);  // reading the candidates when it opened
double asset_picker_draw_ms(void);  // the last draw (CPU)
