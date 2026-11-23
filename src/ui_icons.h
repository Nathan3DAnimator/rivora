// Icons drawn with ImGui's draw list (no icon font, no textures): crisp at every DPI and cost a few vertices.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum UiIcon {
    ICON_FOLDER = 0,
    ICON_CUBE, ICON_SPHERE, ICON_CYLINDER, ICON_CONE, ICON_PLANE, // the basic shapes, in BasicShape order
    ICON_ACTOR,       // an empty actor (a small axis triad)
    ICON_SUN,         // directional light
    ICON_BULB,        // point light
    ICON_SPOT,        // spot light
    ICON_RECT,        // rect light
    ICON_SKY,         // sky light
    ICON_CAMERA,
    ICON_PLAYER,      // player start
    ICON_FILE,        // a file that isn't an asset
    ICON_IMAGE,       // a texture source
    ICON_LEVEL,
    ICON_EYE, ICON_EYE_CLOSED,
    ICON_SEARCH, ICON_SAVE, ICON_PLUS, ICON_IMPORT, ICON_BACK, ICON_FORWARD, ICON_SETTINGS,
    ICON_SELECT, ICON_MOVE, ICON_ROTATE, ICON_SCALE, ICON_MENU, ICON_GRID, ICON_ANGLE, ICON_LOG, ICON_OPEN,
    // the view modes, in ViewMode order (ICON_VM_LIT + view_mode)
    ICON_VM_LIT, ICON_VM_UNLIT, ICON_VM_WIREFRAME, ICON_VM_LIGHTING, ICON_VM_NORMALS, ICON_VM_SOLID, ICON_VM_RANDOM,
    ICON_VM_TEXTURE, ICON_VM_ROUGHNESS, ICON_VM_METALLIC, ICON_VM_PATH_TRACING, ICON_VM_GI, ICON_VM_LOD, ICON_VM_TRIANGLES,
    ICON_VM_MOBILITY, ICON_VM_SURFACE_CACHE, ICON_VM_GI_SOURCE, ICON_VM_DENSITY, ICON_VM_LIGHT_COMPLEXITY, ICON_VM_SHADER_COST,
    ICON_VM_OBJECT_COST,
    ICON_PERSPECTIVE, ICON_NEW, ICON_UNDO, ICON_REDO, ICON_DUPLICATE, ICON_DELETE, ICON_RENAME, ICON_FOCUS, ICON_FLOOR,
    ICON_BAKE, ICON_STATS, ICON_FULLSCREEN, ICON_TIMER, ICON_EXPOSURE, ICON_GAMEPAD, ICON_GLOBE, ICON_EXIT, ICON_INFO,
    ICON_HELP, ICON_BUG, ICON_UPDATE, ICON_PANEL, ICON_CHEVRON,
    // the Blueprints: the asset kinds and the native classes (the Content Browser, the Add menu), the editor's panels
    ICON_BLUEPRINT, ICON_CHARACTER, ICON_PAWN, ICON_GAMEMODE, ICON_STRUCT, ICON_ENUM, ICON_INTERFACE, ICON_DATA,
    ICON_FUNCTION, ICON_EVENT, ICON_VARIABLE, ICON_COMPONENT, ICON_ARROW, ICON_CAPSULE, ICON_MOVEMENT, ICON_SLIDERS,
    ICON_CHECK, ICON_CROSS, ICON_GRAPH,
    ICON_SOUND, // a speaker with its waves (audio assets, Audio Components)
    // (Q5) every asset kind its own (the Content Browser's tiles and badges, the asset fields, the editors' tabs)
    ICON_MATERIAL, ICON_MATERIAL_INSTANCE, ICON_TEXTURE, ICON_STATIC_MESH, ICON_SKELETAL_MESH, ICON_SKELETON, ICON_ANIMATION,
    ICON_MONTAGE, ICON_BLEND_SPACE, ICON_ANIM_BP, ICON_BEHAVIOR_TREE, ICON_BRAIN, ICON_INPUT_ACTION, ICON_INPUT_CONTEXT,
    ICON_MACRO_LIB, ICON_SEQUENCE, ICON_SOUND_WAVE, ICON_SOUND_CUE, ICON_ATTENUATION, ICON_SOUND_CLASS, ICON_SOUND_MIX,
    ICON_CONCURRENCY, ICON_SUBMIX, ICON_METASOUND, ICON_SCRIPT, ICON_PARTICLES, ICON_IK_RIG, ICON_PHYSICS_ASSET, ICON_VEHICLE,
    ICON_DIALOGUE, ICON_QUEST, ICON_WIDGET, ICON_DATA_TABLE, ICON_CURVE, ICON_THEME, ICON_PLUGIN, ICON_LANDSCAPE, ICON_FOLIAGE,
    ICON_GROOM, ICON_CLOTH, ICON_WATER, ICON_RASSET, ICON_ITEM, ICON_ABILITY,
    // the engine's own (the orbit as their motif: Play, Simulate, Compile, Project) and the tools
    ICON_PLAY, ICON_PAUSE, ICON_STOP, ICON_SIMULATE, ICON_COMPILE, ICON_BUILD, ICON_PACKAGE, ICON_PROJECT, ICON_CONTENT,
    ICON_OUTLINER, ICON_DETAILS, ICON_ASSISTANT, ICON_GIT, ICON_LOCK, ICON_UNLOCK, ICON_COPY, ICON_PASTE, ICON_CUT, ICON_LINK,
    ICON_TAG, ICON_FILTER, ICON_SORT, ICON_REFRESH, ICON_DOWNLOAD, ICON_UPLOAD, ICON_RECORD, ICON_KEY, ICON_SNAP, ICON_BRUSH,
    ICON_LAYERS, ICON_WINDOW, ICON_KEYBOARD, ICON_COMMENT, ICON_BREAKPOINT, ICON_STEP, ICON_VR, ICON_PHONE, ICON_NETWORK,
    ICON_HEALTH, ICON_FOG, ICON_DECAL, ICON_VOLUME, ICON_SPLINE, ICON_GROUP, ICON_LANGUAGE, ICON_MEASURE, ICON_CROWD, ICON_NAVMESH,
    // states (compiling, error, warning, unsaved, OK)
    ICON_WARNING, ICON_ERROR, ICON_UNSAVED, ICON_OK,
    // the nodes' families (their headers): Pure, Macro, Flow Control, Cast (Event, Function, Variable, the clock above)
    ICON_PURE, ICON_MACRO, ICON_FLOW, ICON_CAST,
    // (Q26) the target platforms (PC, Android; VR is ICON_VR) and the frame rate's readout (bars)
    ICON_MONITOR, ICON_ANDROID, ICON_SIGNAL,
    ICON_COUNT
} UiIcon;

// (Q5) The icons' grammar (notas-dev/icones-e-tooltips.md): a 16 grid, a 1.5 px stroke that follows the scale, round
// ends; duotone: the stroke in the given color (the text's) and the bodies filled at 20% in the icon's family color
// (Blueprint cyan, Material green, Mesh orange, Animation pink, Audio violet, Effects amber, UI blue, Data blue-gray,
// Level white; the tools the accent, the engine's own the Ignition orange). Every color a theme token.
// Its family's token (TK_ASSET_..., TK_ACCENT, TK_ACCENT2...) and color
int ui_icon_family_token(UiIcon icon);
uint32_t ui_icon_family_color(UiIcon icon);
// An asset kind's icon and family token (AssetKind of content.h; a Blueprint by its parent's in content.c)
UiIcon ui_asset_icon(int asset_kind);
int ui_asset_family_token(int asset_kind);
// The icon's name ("ICON_MATERIAL" without ICON_: "MATERIAL"), for the icon sheet and the tests
const char* ui_icon_name(UiIcon icon);
// The icon sheet (--icon-sheet, the test icon_sheet): every icon at 16, 24 and 32 px in a window of its own, named,
// in the theme's colors (the screenshot goes to --shot)
void ui_icon_sheet_draw(void);

struct ImDrawList;
// Draws icon centered in the square (x, y, size) with color col (0xAABBGGRR).
void ui_icon(struct ImDrawList* dl, UiIcon icon, float x, float y, float size, uint32_t col);
// An icon at the cursor as an item of size x size (for menus and rows); returns nothing, advances the cursor.
void ui_icon_item(UiIcon icon, float size, uint32_t col);
// A button holding an icon (and an optional label after it); tip shows on hover.
bool ui_icon_button(const char* id, UiIcon icon, const char* label, uint32_t col, bool active, const char* tip);
// The same with a â–¾ after the label: a button that opens a menu (the viewport's Perspective, View Mode, Show)
bool ui_dropdown_button(const char* id, UiIcon icon, const char* label, uint32_t col, const char* tip);
// Menu items and submenus with an icon in front, like Unreal's (greyed with the item when disabled)
bool ui_menu_item(UiIcon icon, const char* label, const char* shortcut, bool selected, bool enabled);
bool ui_menu_item_ptr(UiIcon icon, const char* label, const char* shortcut, bool* selected, bool enabled);
bool ui_begin_menu(UiIcon icon, const char* label, bool enabled);

// (2026-11-10, "redimensionar como na Unreal") a bar between two panels at the cursor, dragged to resize them: *size
// grows by the mouse's move times sign (+1: the panel before the bar, -1: the one after), kept in [mn, mx]; vertical:
// a bar of width UI_SPLIT and height len between panels side by side (else one of height UI_SPLIT under the first).
// Double-click: back to def. True while dragged
#define UI_SPLIT 6.0f
bool ui_splitter(const char* id, bool vertical, float* size, float sign, float mn, float mx, float len, float def);

// Animated: t is the time (seconds; ImGui's igGetTime(), or the time since the event for UI_ANIM_POP)
typedef enum UiAnim { UI_ANIM_SPIN = 0, UI_ANIM_PULSE, UI_ANIM_POP, UI_ANIM_BOUNCE } UiAnim;
// (Q26) A folder drawn solid in col (the Content Browser's folder tiles), in the square (x, y, size)
void ui_folder_solid(struct ImDrawList* dl, float x, float y, float size, uint32_t col);
// A spinner (an arc going round, breathing): work in progress
void ui_spinner(struct ImDrawList* dl, float x, float y, float size, uint32_t col, double t);
// A dot with a ring growing out of it: a state that wants attention (unsaved, playing, recording)
void ui_pulse_dot(struct ImDrawList* dl, float x, float y, float size, uint32_t col, double t);
// An icon turning, breathing, popping in (overshoot, settle) or bouncing
void ui_icon_anim(struct ImDrawList* dl, UiIcon icon, float x, float y, float size, uint32_t col, UiAnim anim, double t);
