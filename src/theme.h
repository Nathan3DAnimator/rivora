// The editor's look by tokens (Q1, notas-dev/temas.md): every color, corner, spacing and duration of the UI is a token of
// the current theme. No color literal outside theme.c (the theme_literals test fails on a new one): the UI asks
// TH(TK_...). Eleven built-in themes (Azul Real is the default) and the user's own in %APPDATA%\RealEngine\Themes
// (*.rtheme: text, versioned), changed while the editor runs: the next frame reads the new table. Cost: a table read
// per color (the table is resolved once, when the theme changes).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(__has_include)
#if __has_include(<cimgui.h>)
#define THEME_HAS_IMGUI 1
#endif
#endif
#ifdef THEME_HAS_IMGUI // (the UI: THV/THVA as ImVec4, the fonts; a renderer file reads only the ImU32 table)
#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#include <cimgui.h>
#endif

typedef enum ThemeTok {
    // Surfaces (the darkest to the lightest of a dark theme)
    TK_BG_DEEP,         // under everything: the gaps between the cards
    TK_BG_APP,          // the application's background, the menu bar, the toolbar
    TK_BG_PANEL,        // a panel (card)
    TK_BG_RAISED,       // headers, popups, menus, tooltips
    TK_BG_FIELD,        // an input field, a list's well
    TK_BG_CONTROL,      // a button at rest
    TK_BG_HOVER,        // what the mouse is over
    TK_BG_SELECTED_DIM, // selected, without the focus
    TK_BORDER,          // the subtle border
    TK_BORDER_STRONG,   // the strong border, the splitters
    // Text
    TK_TEXT, TK_TEXT_STRONG, TK_TEXT_DIM, TK_TEXT_DISABLED, TK_TEXT_ON_ACCENT,
    // Accents
    TK_ACCENT,        // the primary: selection, focus, the active tab, links, sliders
    TK_ACCENT_STRONG, // pressed, the selection's fill
    TK_ACCENT_DIM,    // a selected row's background
    TK_ACCENT2,       // the secondary: Play, Simulate, record, each screen's main action
    // States
    TK_OK, TK_WARN, TK_ERROR, TK_INFO,
    // The neutral ramp: the grays by their lightness, N0 the darkest (a light theme turns it around)
    TK_N0, TK_N1, TK_N2, TK_N3, TK_N4, TK_N5, TK_N6, TK_N7, TK_N8, TK_N9, TK_N10, TK_N11, TK_N12, TK_N13, TK_N14, TK_N15,
    // The hues (icons, tracks, charts): dark, middle and light of each
    TK_RED_D, TK_RED, TK_RED_L, TK_ORANGE_D, TK_ORANGE, TK_ORANGE_L, TK_AMBER_D, TK_AMBER, TK_AMBER_L,
    TK_LIME_D, TK_LIME, TK_LIME_L, TK_GREEN_D, TK_GREEN, TK_GREEN_L, TK_TEAL_D, TK_TEAL, TK_TEAL_L,
    TK_CYAN_D, TK_CYAN, TK_CYAN_L, TK_BLUE_D, TK_BLUE, TK_BLUE_L, TK_VIOLET_D, TK_VIOLET, TK_VIOLET_L,
    TK_MAGENTA_D, TK_MAGENTA, TK_MAGENTA_L, TK_PINK_D, TK_PINK, TK_PINK_L,
    // Blueprint pins (by family: numbers in blues, text in magentas, geometry in ambers, references in teal)
    TK_PIN_EXEC, TK_PIN_BOOL, TK_PIN_BYTE, TK_PIN_INT, TK_PIN_INT64, TK_PIN_FLOAT, TK_PIN_STRING, TK_PIN_NAME, TK_PIN_TEXT,
    TK_PIN_VECTOR, TK_PIN_ROTATOR, TK_PIN_TRANSFORM, TK_PIN_COLOR, TK_PIN_OBJECT, TK_PIN_CLASS, TK_PIN_STRUCT, TK_PIN_ENUM,
    TK_PIN_WILDCARD, TK_PIN_DELEGATE,
    // Node families (the header's color)
    TK_NODE_EVENT, TK_NODE_FUNCTION, TK_NODE_PURE, TK_NODE_LATENT, TK_NODE_MACRO, TK_NODE_FLOW, TK_NODE_CAST, TK_NODE_ENTRY,
    TK_NODE_TITLE, // the text on a node's header (white on the dark themes' headers, dark on the light themes' pastel ones)
    // Asset families (Content Browser, the Outliner's icons)
    TK_ASSET_BLUEPRINT, TK_ASSET_MATERIAL, TK_ASSET_MESH, TK_ASSET_TEXTURE, TK_ASSET_ANIMATION, TK_ASSET_AUDIO,
    TK_ASSET_FX, TK_ASSET_UI, TK_ASSET_DATA, TK_ASSET_LEVEL, TK_ASSET_FOLDER,
    // Graphs and previews
    TK_GRAPH_BG, TK_GRAPH_GRID, TK_GRAPH_GRID_MAJOR, TK_PREVIEW_TOP, TK_PREVIEW_BOTTOM,
    // Docking (Q2): the targets shown while a window is dragged
    TK_DOCK_TARGET, TK_DOCK_PREVIEW,
    TK_COUNT
} ThemeTok;

// The sizes (px at 100%: the UI scale multiplies them) and durations (ms)
typedef enum ThemeMetric {
    TM_RADIUS_WINDOW, TM_RADIUS_PANEL, TM_RADIUS_FRAME, TM_RADIUS_POPUP, TM_RADIUS_TAB, TM_RADIUS_NODE,
    TM_SPACE_CARD, // the gap between the docked panels (cards)
    TM_SPACE_PAD,  // a panel's inner padding
    TM_SPACE_ITEM, // between two controls
    TM_BORDER,     // the borders' width
    TM_DUR_FAST, TM_DUR_NORMAL, TM_DUR_ENTER,
    TM_FONT_SIZE, TM_FONT_TITLE,
    TM_COUNT
} ThemeMetric;

// THEME_DEFAULT: the built-in a new user starts with (Azul Real since 0.44; Órbita Escuro, index 0, stays the
// reference the other themes' families follow)
enum { THEME_BUILTIN_COUNT = 11, THEME_DEFAULT = 10, THEME_NAME = 64, THEME_MAX = 48 };
#define THEME_DEFAULT_NAME "Azul Real"
#define THEME_PREVIOUS_DEFAULT_NAME "Órbita Escuro" // (the default before 0.44: an editor.ini still on it is offered the new one)

typedef struct Theme {
    char name[THEME_NAME];
    char base[THEME_NAME];  // the built-in theme it started from (a user's theme)
    char file[300];         // a user's theme: its file
    uint32_t col[TK_COUNT]; // ImU32 (0xAABBGGRR)
    uint8_t set[TK_COUNT];  // the tokens given; the rest derived by theme_resolve
    float metric[TM_COUNT];
    float hue_sat, hue_light; // the hues' saturation (0-1) and lightness shift (-0.3..0.3)
    int palette;            // 0 the hue circle, 1 Okabe-Ito (safe for color blindness)
    bool light;             // a light theme (the ramp turned around)
    bool builtin;
} Theme;

// Untinted (an image's tint, a white mask): not a theme color
#define THEME_IMAGE_TINT 0xFFFFFFFFu
#define THEME_NONE 0x00000000u

// The current table: TH(k) a color with full alpha, THA(k, a) with alpha 0-255, THV/THVA as ImVec4
// Over the 3D scene (the viewport's overlays, the asset previews): a file that defines THEME_SCENE before including
// this reads the scene's table, the same theme with a dark theme's grays and hues when it is a light one (text over
// a render keeps its contrast whatever the editor's theme)
#ifdef THEME_SCENE
#define g_theme_u32 g_theme_scene_u32
#endif
extern uint32_t g_theme_u32[TK_COUNT];
extern float g_theme_metric[TM_COUNT];
static inline uint32_t TH(int k) { return g_theme_u32[k]; }
static inline uint32_t THA(int k, unsigned a) { return (g_theme_u32[k] & 0x00FFFFFFu) | ((uint32_t)(a > 255u ? 255u : a) << 24); }
#ifdef THEME_HAS_IMGUI
extern ImVec4 g_theme_v4[TK_COUNT];
static inline ImVec4 THV(int k) { return g_theme_v4[k]; }
static inline ImVec4 THVA(int k, float a) { ImVec4 v = g_theme_v4[k]; v.w = a; return v; }
#endif
static inline float TMV(ThemeMetric m) { return g_theme_metric[m]; }
uint32_t theme_mix(uint32_t a, uint32_t b, float t); // between two colors (alpha too)
uint32_t theme_scale(uint32_t c, float k);           // the color k times brighter (alpha kept)
uint32_t theme_alpha(uint32_t c, unsigned a);        // the color with another alpha

// The themes
int theme_count(void); // built-in + the user's
const Theme* theme_at(int i);
int theme_current(void);
const char* theme_current_name(void);
int theme_find(const char* name); // -1 if none
bool theme_select(int i);         // applied: the next frame draws with it
bool theme_select_name(const char* name);
uint32_t theme_generation(void);  // +1 at each change
void theme_init(void);            // the built-in themes and the user's from disk; THEME_DEFAULT current
bool theme_style_dirty(void);     // a change waits for theme_apply_imgui (between frames: main.c)
void theme_apply_imgui(float scale); // ImGui's style from the tokens
void theme_resolve(Theme* t);     // the tokens not given, derived (the ramp, hues, pins, nodes, assets)
void theme_builtin(Theme* t, int i); // a built-in theme, resolved

// Editing (Editor Preferences > Appearance > Theme Editor): a copy edited live
Theme* theme_edit_begin(int from); // a copy of that theme becomes the current (live preview)
Theme* theme_edit_theme(void);
void theme_edit_changed(void);     // a value of the copy changed: resolved and applied
int theme_edit_save(const char* name); // saved as a user theme (index, -1 failed), current
void theme_edit_cancel(void);      // back to the theme before
bool theme_editing(void);
int theme_duplicate(int i, const char* name); // a user theme copied from i (saved); its index
bool theme_delete_user(int i);

// The .rtheme files: "rtheme 1", "name = ...", "base = ...", "<token> = #RRGGBBAA", "<metric> = <number>"
bool theme_write_text(const Theme* t, char* out, size_t cap);
bool theme_read_text(Theme* t, const char* text);
bool theme_write_file(const Theme* t, const char* path);
bool theme_read_file(Theme* t, const char* path);
int theme_import(const char* path); // a .rtheme from anywhere: copied to the user's folder; its index (-1)
bool theme_export(int i, const char* path);
void theme_user_dir(char* out, size_t cap);
void theme_set_user_dir(const char* dir); // (the tests: a folder of their own)

// Names
const char* theme_tok_name(int k);    // "bg_panel"
const char* theme_tok_label(int k);   // "Panel" (TR by the caller)
int theme_tok_group(int k);           // index into theme_group_name
const char* theme_group_name(int g);  // "Surfaces", "Text"... (TR by the caller)
int theme_group_count(void);
const char* theme_metric_name(int m);
const char* theme_metric_label(int m);
uint32_t theme_from_hex(const char* s, bool* ok); // "#RRGGBB" / "#RRGGBBAA" -> ImU32
void theme_to_hex(uint32_t c, char out[10]);

// Contrast (WCAG 2): the ratio between two colors (1-21); the checks of a theme (the failing pairs into out)
float theme_contrast(uint32_t a, uint32_t b);
int theme_check_contrast(const Theme* t, char* out, size_t cap);

// Fonts: Inter (UI), Sora (titles), JetBrains Mono (code, numbers); OFL, embedded; Segoe UI merged for the rest
struct ImFont;
extern struct ImFont* g_font_ui;
extern struct ImFont* g_font_title;
extern struct ImFont* g_font_mono;
extern struct ImFont* g_font_bold;
void theme_load_fonts(float size);
