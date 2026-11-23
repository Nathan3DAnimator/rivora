// The asset viewers (asset_viewer.c): Unreal's Texture Editor and Static Mesh Editor, simpler (a texture zoomed and
// panned over a checker with its channels, a mesh turned around with its counts and size), and the Content Browser's
// thumbnails of textures, Static Meshes, Skeletal Meshes and materials. The textures are decoded on a worker thread and
// shrunk to 128 px; the meshes rasterized once on the CPU (128 px, a depth buffer, a key light); the materials drawn as
// their preview sphere; each thumbnail is kept (a GPU image of 64 KB) for the session.
#pragma once
#include <stdint.h>
// (2026-11-23) a Static Mesh's material saved from the viewer: the editor updates the level's actors that had the old one
void asset_viewer_on_mesh_material(void (*fn)(const char* mesh_vpath, uint32_t old_mat, uint32_t new_mat));
#include <stdbool.h>

// The thumbnails kept at most: each one an ImGui texture, so main.c gives the UI a descriptor pool this big and more
// (it had 64: the 0.10_b crash, VK_ERROR_OUT_OF_POOL_MEMORY, with a 243-mesh import in the Content Browser)
#define ASSET_THUMB_MAX 1024
#define UI_TEXTURES_MAX (ASSET_THUMB_MAX + 256)

struct VkCtx;
struct ImDrawList;

void asset_viewer_init(struct VkCtx* vk);
void asset_viewer_shutdown(struct VkCtx* vk); // (the GPU idle)
// Whether a file opens here (a texture source, a .rmesh), and opening it ("/Content/...")
bool asset_viewer_handles(const char* vpath);
void asset_viewer_open(const char* vpath);
void asset_viewer_draw(void); // every frame (its window, and the thumbnails' uploads)
// A thumbnail into the rectangle; false when there is none (yet): the caller draws the kind's icon
bool asset_thumb(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, int kind);
// The animation under the pointer (call it each frame it is hovered): its thumbnail plays the clip
void asset_thumb_hover(const char* vpath);
// A list row (a combo's, sugestão #60) with the asset's thumbnail on the left and, under the mouse, a big preview beside
// it (an animation plays). path: "/Content/...", a disk path under the Content folder, or "#mesh/<id in hex>" (a mesh by
// its id: the basic shapes too); NULL: the kind's icon. Honors the combo's search. True when clicked
bool asset_selectable(const char* label, bool selected, const char* path, int kind);
bool asset_selectable_mesh(const char* label, bool selected, unsigned int mesh_id); // (a Static Mesh by its id)
bool asset_selectable_material(const char* label, bool selected, const char* path); // (a .rmat or a .rmi by its file)
// An animation's n frames side by side in a PNG, seen from yaw degrees (--anim-sheet: reviewing animations)
bool asset_anim_sheet(const char* vpath, const char* png, int n, float yaw);
void asset_viewer_close(void); // (Q7: the asset viewport test)

// (Q23, notas-dev/auditoria-artista.md) The vertices a mesh's triangles use, each once (the Static Mesh viewer's count)
#include <stdint.h>
uint32_t asset_mesh_used_verts(const uint32_t* idx, uint32_t first, uint32_t count);
// (Q23) What the artist test reads: the open texture (0 x 0 and its error when it couldn't be read), its mips, the
// thumbnails and the open texture read again after their files changed on disk, and the watch's cost
typedef struct AssetViewerState {
    bool open, is_hdr;
    unsigned w, h, src_w, src_h;
    int mips;
    const char* error;
    unsigned thumb_refreshes, reloads;
    double watch_us; // the thumbnails' watch on the worker: one file looked at (µs, the last)
} AssetViewerState;
AssetViewerState asset_viewer_state(void);
int asset_thumb_state(const char* vpath); // (the test: 4 shown, 5 couldn't be made, else on its way; -1 none)
// (Q23) artist_test.c: --artist-selftest (no window) and --artist-test (in the editor)
struct Editor;
int artist_selftest(void);
void artist_test_start(void);
bool artist_test_frame(struct Editor* ed, bool* ok);
