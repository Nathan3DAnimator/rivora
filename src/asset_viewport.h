// The asset viewport (Q7, notas-dev/editor.md "Viewport de asset"): the one 3D view every asset editor shows its asset
// in (Static Mesh, Skeletal Mesh / Skeleton / Animation / Montage / Blend Space / Anim Blueprint, Physics Asset, IK Rig,
// Material, Particle System, Vehicle, Cloth), like Unreal's asset editors' viewport:
//   - the model always drawn by the GPU (render_preview: its materials and textures, the shadow, 4x MSAA) in any view
//     mode (Lit, Unlit, Wireframe, Lit Wireframe, Lighting Only, Albedo, World Normal, Roughness, Metallic, AO, UV, LOD,
//     Overdraw), its LOD by the distance or forced;
//   - over it (ImGui): the bones (none / the selected / all, their names, their axes), the sockets (picked, moved with
//     the gizmo, a preview mesh drawn on each), a Physics Asset's bodies and constraints, the UV layout;
//   - the floor and its grid, the studio or an outdoor sky, the key light turned by L + drag (or its menu);
//   - the camera: orbit (left drag), pan (middle), zoom (wheel), fly (right drag + WASD/QE, wheel: speed), F frames,
//     Alt + right: dolly, its FOV;
//   - the tabs at the top right (Unreal's Skeleton / Mesh / Animation / Physics): the window switches what it edits
//     without closing (the caller does the switch: AvpResult.tab).
// Each editor keeps an AssetViewport (its settings and its camera) and gives a scene each frame (its draws, no copy
// kept); the cost: avp_stats.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bp_preview.h"
#include "render.h"

struct SkelMesh;
struct PhysicsAsset;

enum { AVP_BONES_NONE = 0, AVP_BONES_SELECTED, AVP_BONES_ALL };
enum { AVP_TAB_MODEL = 0, AVP_TAB_SKELETON, AVP_TAB_ANIMATION, AVP_TAB_PHYSICS, AVP_TAB_IKRIG, AVP_TAB_COUNT };
#define AVP_TAB(t) (1u << (t))
enum { AVP_ENV_STUDIO = 0, AVP_ENV_SKY };
#define AVP_DRAWS_MAX 600 // the meshes of one view (a Skeletal Mesh's parts, its skins, the sockets' previews)

typedef struct AssetViewport {
    bool init;
    BpPreviewCam cam;
    int mode;                                 // PREVIEW_ (render.h)
    int bones;                                // AVP_BONES_
    bool bone_names, bone_axes, sockets, bodies, constraints, floor, grid, stats, uv_layout;
    int env;                                  // AVP_ENV_
    bool light_custom;                        // turned by the user (else it follows the camera, the studio's way)
    float light_yaw, light_pitch, light_scale; // degrees; the key's intensity
    float fov;                                // vertical, degrees
    float fly_speed;                          // cm/s (0: by the subject's size)
    int lod;                                  // -1: by the distance, else forced
    int socket;                               // the selected socket (-1: none)
    // the last frame's view (avp_to_screen: the editors' own overlays)
    float x0, y0, x1, y1, focal, eye[3], fwd[3], right[3], up[3], radius;
    // the socket gizmo's drag
    int gizmo_axis, gizmo_hot;
    bool dragging, flying;
    float drag_from[3];
    // what the last frame drew
    uint32_t tris, draws, lods, lod_shown;
    double cpu_ms;
} AssetViewport;

typedef struct AvpScene {
    const PreviewDraw* draws;           // the meshes (copied: only for this frame)
    uint32_t ndraws;
    const struct SkelMesh* skel;        // its bones and sockets drawn over (NULL: none)
    const float (*bones)[3][4];         // their pose (rows in the view's space)
    int sel_bone;
    struct SkelMesh* edit_sockets;      // its sockets can be picked and moved (NULL: shown only)
    const struct PhysicsAsset* phys;    // its bodies and constraints drawn over (on skel's bones)
    int sel_body, sel_constraint;
    unsigned tabs;                      // AVP_TAB() of the tabs to show (0: none)
    int tab;                            // the current one
    const float* mn;                    // the bounds to frame (NULL: the draws' and the bones')
    const float* mx;
    const char* hint;                   // the controls' line at the bottom (NULL: the camera's)
    bool lock_left;                     // the left button is the caller's (a handle under it): no orbit, no pick
} AvpScene;

typedef struct AvpResult {
    int tab;                 // the tab clicked (-1: none)
    int bone, body, socket;  // clicked in the view (-1: none)
    bool clicked_empty;      // a click on nothing
    bool hovered, busy;      // the mouse over the view; dragging the camera, the light or the gizmo
    bool socket_moved;       // the gizmo moved the selected socket (the mesh changed)
    bool drawn;              // the GPU's picture is shown
} AvpResult;

void avp_init(AssetViewport* v); // (avp_draw calls it on first use)
// The view into the rectangle (screen px) with its toolbar, overlays and tabs; key: the window's own number (not 0,
// BP_PV_KEY_*: the GPU's picture is kept per key). r may be NULL
void avp_draw(AssetViewport* v, unsigned key, float x, float y, float w, float h, const AvpScene* s, AvpResult* r);
// The last frame's projection: a point (the view's space) to the screen (false: behind the camera); a mouse move (px)
// at a point as a move in the view's plane there
bool avp_to_screen(const AssetViewport* v, const float p[3], float out[2]);
void avp_screen_delta(const AssetViewport* v, const float at[3], float dx, float dy, float out[3]);
void avp_frame(AssetViewport* v, const float mn[3], const float mx[3]); // the camera onto the bounds (F)
// The tabs (AVP_TAB() mask) right-aligned on the current line of the window's toolbar (Unreal's asset editors' top
// right): the one clicked (-1: none)
int avp_tabs_bar(unsigned tabs, int tab);
void avp_view_at(AssetViewport* v, float x, float y, float w, float h);   // the projection for a rectangle (no drawing)

// A Skeletal Mesh at a pose (bones: its rows) as draws: its rigid parts and its skins (pal: room for its palette,
// ANIM_MAX_BONES). Selected: the part, else the parts of the bone. The bounds widened. Their count
uint32_t avp_skel_draws(const struct SkelMesh* m, const float (*bones)[3][4], int sel_bone, int sel_part, PreviewDraw* out, uint32_t cap,
                        float (*pal)[3][4], float mn[3], float mx[3]);
// The Skeleton tab's sockets (Unreal's: Add Socket on a bone, rename, its bone, its place and turn, a preview mesh):
// true when the mesh changed
bool avp_socket_panel(AssetViewport* v, struct SkelMesh* m, int sel_bone);
// A socket moved by a world move (the view's space; its bone's space found from the pose): the gizmo's drag
void avp_socket_move_world(struct SkelMesh* m, int socket, const float (*bones)[3][4], const float delta[3]);
// A Static Mesh drawn plainly (its material) at the origin (the Material Editor's sphere, the Static Mesh Editor)
void avp_mesh_draw(PreviewDraw* d, uint32_t mesh, uint32_t material, const float color[3], float roughness, float metallic);

// The cost (the asset viewports of the last frame): their count, their CPU (asking + overlays) and the previews' GPU
typedef struct AvpStats {
    uint32_t views;
    double cpu_ms, gpu_ms;
} AvpStats;
AvpStats avp_stats(void);

// The test (--asset-viewport-test, asset_viewport_test.c): started once, then a step each editor frame after the UI
// (true: finished, *ok: passed)
struct Editor;
void avp_test_start(void);
bool avp_test_running(void);
bool avp_test_frame(struct Editor* ed, bool* ok);
// (the test) every viewport in this view mode, without the floor, the grid or the sky (-1: off)
void avp_test_force(int mode);
uint32_t avp_test_frames(unsigned key); // frames the GPU's picture of key was asked for
