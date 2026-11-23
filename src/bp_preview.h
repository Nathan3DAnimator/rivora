// The look of a Blueprint class drawn with ImGui's draw list (no render target, no GPU pass of its own): its Static Mesh
// lit by a key light, the sky and a rim (GGX, ACES), smooth shading and anti-aliased silhouettes, on a floor grid with a
// soft contact shadow; a Character's capsule, arrow and camera, a light's bulb. The Blueprint Editor's Viewport and the
// Content Browser's thumbnails.
#pragma once
#include <stdbool.h>

struct ImDrawList;
struct BpAsset;

typedef struct BpPreviewCam {
    float yaw, pitch, dist; // degrees, cm: an orbit around target
    float target[3];
    bool framed;            // set once bp_preview_frame placed it
} BpPreviewCam;

enum {
    BP_PREVIEW_GRID = 1,    // the floor grid
    BP_PREVIEW_HELPERS = 2, // the capsule, the arrow, the camera, the light's bulb
    BP_PREVIEW_THUMB = 4,   // a thumbnail: coarser mesh, its own background
};

// The camera that shows all of the class (its bounds), from the front-left and a little above, like Unreal's
void bp_preview_frame(const struct BpAsset* a, BpPreviewCam* cam);
// Draws the class into the rectangle (clipped to it). False when it has nothing to show (no mesh, no light, not a
// Character): the caller draws its type's icon instead.
bool bp_preview_draw(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const struct BpAsset* a, BpPreviewCam* cam,
                     unsigned flags);
// Whether the class has something to draw
bool bp_preview_has_look(const struct BpAsset* a);

// Any shapes at transforms (axes: rows, scaled; o: the origin), lines (from xyz to xyz) over them: the animation editors
typedef struct BpPreviewShape {
    unsigned shape;
    float ax[3][3], o[3];
    float color[3], roughness, metallic;
    bool selected;
    unsigned material; // a material's hash (0: the color; the GPU's picture only)
    const float (*palette)[3][4]; // a skin's bones (pose x bind inverse; NULL: rigid): bent by the GPU's preview
    unsigned nbones;
} BpPreviewShape;
void bp_preview_frame_bounds(BpPreviewCam* cam, const float mn[3], const float mx[3]);
void bp_preview_shapes(struct ImDrawList* dl, float x0, float y0, float x1, float y1, BpPreviewCam* cam, const BpPreviewShape* s,
                       unsigned n, const float (*lines)[6], unsigned nlines, int selected_line, bool grid);

// The asset editors' viewports drawn by the GPU (render_preview, render.h): the materials' textures, a view mode
// (PREVIEW_*: Lit, Unlit, Wireframe...), the key light's shadow, 4x MSAA. key: the window's own number (not 0); without
// the GPU's preview the CPU drawing above. The overlays (bones, helpers, gizmo) are still drawn over it by ImGui.
enum { BP_PV_KEY_ANIM = 1, BP_PV_KEY_MOTION_MATCH, BP_PV_KEY_STATIC_MESH, BP_PV_KEY_BLEND_SPACE, BP_PV_KEY_ANIM_GRAPH, BP_PV_KEY_ABP,
       BP_PV_KEY_IKRIG, BP_PV_KEY_MATERIAL, BP_PV_KEY_PARTICLES, BP_PV_KEY_VEHICLE, BP_PV_KEY_CLOTH, // (Q7: the asset viewport, asset_viewport.h)
       BP_PV_KEY_BP_VIEWPORT = 0x100 /* + the Blueprint window's index */ };
typedef struct BpPreviewGpu {
    unsigned key;
    int mode;   // PREVIEW_ (render.h)
    float time; // s: the animated materials
} BpPreviewGpu;
void bp_preview_shapes_gpu(struct ImDrawList* dl, float x0, float y0, float x1, float y1, BpPreviewCam* cam, const BpPreviewShape* s,
                           unsigned n, const float (*lines)[6], unsigned nlines, int selected_line, bool grid, const BpPreviewGpu* g);
// Unreal's view mode menu (a button with the mode, its list in a popup) where the cursor is: true when it changed
bool bp_preview_mode_button(const char* id, int* mode);
// (Q7) the ImGui texture of a GPU preview's picture (render_preview's view; registered again when its generation changes)
void* bp_preview_texture(unsigned key, void* image_view, unsigned gen);
const char* bp_preview_mode_name(int mode); // (translated)

// The Blueprint Editor's Viewport: a component drawn selected (an index in the class's component tree, -1 the root,
// BP_PREVIEW_NONE nothing), picking and the W/E/R gizmo
#define BP_PREVIEW_NONE (-2)
bool bp_preview_draw_ex(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const struct BpAsset* a, BpPreviewCam* cam,
                        unsigned flags, int selected);
bool bp_preview_draw_gpu(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const struct BpAsset* a, BpPreviewCam* cam,
                         unsigned flags, int selected, const BpPreviewGpu* g);
// The component under the mouse (screen pixels): a node, -1 the root, BP_PREVIEW_NONE nothing
int bp_preview_pick(float x0, float y0, float x1, float y1, const struct BpAsset* a, const BpPreviewCam* cam, float mx, float my);
typedef struct BpGizmo {
    bool dragging;
    int axis, mode;
    float start_mouse[2], start_loc[3], start_rot[3], start_scale[3], start_angle;
} BpGizmo;
// Draws the gizmo of a node (mode 0 move, 1 rotate, 2 scale) and drags it with the left button. True while the mouse is
// on it or dragging (the viewport doesn't orbit); *changed when the node moved
bool bp_preview_gizmo(struct ImDrawList* dl, float x0, float y0, float x1, float y1, struct BpAsset* a, const BpPreviewCam* cam,
                      int node, int mode, BpGizmo* g, bool* changed);
