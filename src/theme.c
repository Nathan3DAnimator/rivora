// The editor's themes (theme.h, notas-dev/temas.md). The only file of src/ with color literals: the built-in themes
// below, written in the .rtheme format itself (one parser for them and the user's files), and the derivation of the
// tokens a theme does not give (the neutral ramp, the hues, pins, node and asset families) from the ones it gives.
#include "theme.h"
#include "platform.h"
#include "platform_files.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp_ _stricmp
#else
#include <strings.h>
#define strcasecmp_ strcasecmp
#endif

uint32_t g_theme_u32[TK_COUNT];
uint32_t g_theme_scene_u32[TK_COUNT];
ImVec4 g_theme_v4[TK_COUNT];
float g_theme_metric[TM_COUNT];
ImFont* g_font_ui;
ImFont* g_font_title;
ImFont* g_font_mono;
ImFont* g_font_bold;

// The patched ImGui's durations ([Real Engine] in imgui.cpp / imgui_widgets.cpp): the fades and the hover's ease
extern float RE_FadeInSeconds, RE_FadeQuickSeconds, RE_FadeOutSeconds, RE_HoverEaseSeconds;

// --- Names --------------------------------------------------------------------------------------------------------
typedef struct TokInfo { const char* name; const char* label; uint8_t group; } TokInfo;
enum { G_SURF, G_TEXT, G_ACCENT, G_STATE, G_RAMP, G_HUE, G_PIN, G_NODE, G_ASSET, G_GRAPH, G_COUNT };
static const char* const g_group_names[G_COUNT] = { "Surfaces", "Text", "Accents", "States", "Neutral ramp", "Hues",
                                                    "Blueprint pins", "Node families", "Asset families", "Graphs, previews and docking" };
static const TokInfo g_tok[TK_COUNT] = {
    { "bg_deep", "Deep background", G_SURF }, { "bg_app", "Application", G_SURF }, { "bg_panel", "Panel", G_SURF },
    { "bg_raised", "Raised (menus, popups)", G_SURF }, { "bg_field", "Input field", G_SURF }, { "bg_control", "Button", G_SURF },
    { "bg_hover", "Hover", G_SURF }, { "bg_selected_dim", "Selected (no focus)", G_SURF }, { "border", "Border", G_SURF },
    { "border_strong", "Strong border", G_SURF },
    { "text", "Text", G_TEXT }, { "text_strong", "Strong text", G_TEXT }, { "text_dim", "Secondary text", G_TEXT },
    { "text_disabled", "Disabled text", G_TEXT }, { "text_on_accent", "Text on the accent", G_TEXT },
    { "accent", "Accent", G_ACCENT }, { "accent_strong", "Accent (pressed)", G_ACCENT }, { "accent_dim", "Accent (selected row)", G_ACCENT },
    { "accent2", "Secondary accent (Play)", G_ACCENT },
    { "ok", "Success", G_STATE }, { "warn", "Warning", G_STATE }, { "error", "Error", G_STATE }, { "info", "Information", G_STATE },
    { "n0", "Ramp 0", G_RAMP }, { "n1", "Ramp 1", G_RAMP }, { "n2", "Ramp 2", G_RAMP }, { "n3", "Ramp 3", G_RAMP },
    { "n4", "Ramp 4", G_RAMP }, { "n5", "Ramp 5", G_RAMP }, { "n6", "Ramp 6", G_RAMP }, { "n7", "Ramp 7", G_RAMP },
    { "n8", "Ramp 8", G_RAMP }, { "n9", "Ramp 9", G_RAMP }, { "n10", "Ramp 10", G_RAMP }, { "n11", "Ramp 11", G_RAMP },
    { "n12", "Ramp 12", G_RAMP }, { "n13", "Ramp 13", G_RAMP }, { "n14", "Ramp 14", G_RAMP }, { "n15", "Ramp 15", G_RAMP },
    { "red_d", "Red (dark)", G_HUE }, { "red", "Red", G_HUE }, { "red_l", "Red (light)", G_HUE },
    { "orange_d", "Orange (dark)", G_HUE }, { "orange", "Orange", G_HUE }, { "orange_l", "Orange (light)", G_HUE },
    { "amber_d", "Amber (dark)", G_HUE }, { "amber", "Amber", G_HUE }, { "amber_l", "Amber (light)", G_HUE },
    { "lime_d", "Lime (dark)", G_HUE }, { "lime", "Lime", G_HUE }, { "lime_l", "Lime (light)", G_HUE },
    { "green_d", "Green (dark)", G_HUE }, { "green", "Green", G_HUE }, { "green_l", "Green (light)", G_HUE },
    { "teal_d", "Teal (dark)", G_HUE }, { "teal", "Teal", G_HUE }, { "teal_l", "Teal (light)", G_HUE },
    { "cyan_d", "Cyan (dark)", G_HUE }, { "cyan", "Cyan", G_HUE }, { "cyan_l", "Cyan (light)", G_HUE },
    { "blue_d", "Blue (dark)", G_HUE }, { "blue", "Blue", G_HUE }, { "blue_l", "Blue (light)", G_HUE },
    { "violet_d", "Violet (dark)", G_HUE }, { "violet", "Violet", G_HUE }, { "violet_l", "Violet (light)", G_HUE },
    { "magenta_d", "Magenta (dark)", G_HUE }, { "magenta", "Magenta", G_HUE }, { "magenta_l", "Magenta (light)", G_HUE },
    { "pink_d", "Pink (dark)", G_HUE }, { "pink", "Pink", G_HUE }, { "pink_l", "Pink (light)", G_HUE },
    { "pin_exec", "Execution", G_PIN }, { "pin_bool", "Boolean", G_PIN }, { "pin_byte", "Byte", G_PIN }, { "pin_int", "Integer", G_PIN },
    { "pin_int64", "Integer64", G_PIN }, { "pin_float", "Float", G_PIN }, { "pin_string", "String", G_PIN }, { "pin_name", "Name", G_PIN },
    { "pin_text", "Text", G_PIN }, { "pin_vector", "Vector", G_PIN }, { "pin_rotator", "Rotator", G_PIN }, { "pin_transform", "Transform", G_PIN },
    { "pin_color", "Linear Color", G_PIN }, { "pin_object", "Actor / Object", G_PIN }, { "pin_class", "Class", G_PIN },
    { "pin_struct", "Structure", G_PIN }, { "pin_enum", "Enum", G_PIN }, { "pin_wildcard", "Wildcard", G_PIN }, { "pin_delegate", "Delegate", G_PIN },
    { "node_event", "Event", G_NODE }, { "node_function", "Function", G_NODE }, { "node_pure", "Pure", G_NODE }, { "node_latent", "Latent", G_NODE },
    { "node_macro", "Macro", G_NODE }, { "node_flow", "Flow Control", G_NODE }, { "node_cast", "Cast", G_NODE }, { "node_entry", "Function entry", G_NODE },
    { "node_title", "Node title", G_NODE },
    { "asset_blueprint", "Blueprint", G_ASSET }, { "asset_material", "Material", G_ASSET }, { "asset_mesh", "Mesh", G_ASSET },
    { "asset_texture", "Texture", G_ASSET }, { "asset_animation", "Animation", G_ASSET }, { "asset_audio", "Audio", G_ASSET },
    { "asset_fx", "Effects", G_ASSET }, { "asset_ui", "UI", G_ASSET }, { "asset_data", "Data", G_ASSET }, { "asset_level", "Level", G_ASSET },
    { "asset_folder", "Folder", G_ASSET },
    { "graph_bg", "Graph background", G_GRAPH }, { "graph_grid", "Graph grid", G_GRAPH }, { "graph_grid_major", "Graph grid (major)", G_GRAPH },
    { "preview_top", "Preview (top)", G_GRAPH }, { "preview_bottom", "Preview (bottom)", G_GRAPH },
    { "dock_target", "Docking target", G_GRAPH }, { "dock_preview", "Docking preview", G_GRAPH },
};
typedef struct MetricInfo { const char* name; const char* label; float lo, hi; } MetricInfo;
static const MetricInfo g_met[TM_COUNT] = {
    { "radius_window", "Window corners", 0, 16 }, { "radius_panel", "Panel corners", 0, 16 }, { "radius_frame", "Control corners", 0, 12 },
    { "radius_popup", "Popup corners", 0, 16 }, { "radius_tab", "Tab corners", 0, 12 }, { "radius_node", "Node corners", 0, 16 },
    { "space_card", "Gap between panels", 1, 12 }, { "space_pad", "Panel padding", 2, 16 }, { "space_item", "Spacing", 2, 12 },
    { "border", "Border width", 0, 3 }, { "dur_fast", "Fast animation (ms)", 0, 400 }, { "dur_normal", "Animation (ms)", 0, 600 },
    { "dur_enter", "Window fade-in (ms)", 0, 800 }, { "font_size", "Text size", 11, 22 }, { "font_title", "Title size", 12, 30 },
};
const char* theme_tok_name(int k) { return k >= 0 && k < TK_COUNT ? g_tok[k].name : ""; }
const char* theme_tok_label(int k) { return k >= 0 && k < TK_COUNT ? g_tok[k].label : ""; }
int theme_tok_group(int k) { return k >= 0 && k < TK_COUNT ? g_tok[k].group : 0; }
const char* theme_group_name(int g) { return g >= 0 && g < G_COUNT ? g_group_names[g] : ""; }
int theme_group_count(void) { return G_COUNT; }
const char* theme_metric_name(int m) { return m >= 0 && m < TM_COUNT ? g_met[m].name : ""; }
const char* theme_metric_label(int m) { return m >= 0 && m < TM_COUNT ? g_met[m].label : ""; }

// --- Colors -------------------------------------------------------------------------------------------------------
#define CR(c) ((float)((c) & 255u) / 255.0f)
#define CG(c) ((float)(((c) >> 8) & 255u) / 255.0f)
#define CB(c) ((float)(((c) >> 16) & 255u) / 255.0f)
#define CA(c) ((float)((c) >> 24) / 255.0f)
static uint32_t pack(float r, float g, float b, float a) {
    float v[4] = { r, g, b, a };
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        float x = v[i] < 0.0f ? 0.0f : v[i] > 1.0f ? 1.0f : v[i];
        o |= (uint32_t)(x * 255.0f + 0.5f) << (8 * i);
    }
    return o;
}
uint32_t theme_mix(uint32_t a, uint32_t b, float t) {
    return pack(CR(a) + (CR(b) - CR(a)) * t, CG(a) + (CG(b) - CG(a)) * t, CB(a) + (CB(b) - CB(a)) * t, CA(a) + (CA(b) - CA(a)) * t);
}
uint32_t theme_scale(uint32_t c, float k) { return pack(CR(c) * k, CG(c) * k, CB(c) * k, CA(c)); }
uint32_t theme_alpha(uint32_t c, unsigned a) { return (c & 0x00FFFFFFu) | ((uint32_t)(a > 255u ? 255u : a) << 24); }

static void rgb_hsl(uint32_t c, float* h, float* s, float* l) {
    float r = CR(c), g = CG(c), b = CB(c);
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *l = (mx + mn) * 0.5f;
    *s = d < 1e-5f ? 0.0f : d / (1.0f - fabsf(2.0f * *l - 1.0f) + 1e-6f);
    if (d < 1e-5f) *h = 0.0f;
    else if (mx == r) *h = 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) *h = 60.0f * ((b - r) / d + 2.0f);
    else *h = 60.0f * ((r - g) / d + 4.0f);
}
static float hue_ch(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}
static uint32_t hsl_rgb(float h, float s, float l) {
    s = fminf(fmaxf(s, 0.0f), 1.0f), l = fminf(fmaxf(l, 0.0f), 1.0f);
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q, hh = fmodf(h + 360.0f, 360.0f) / 360.0f;
    return pack(hue_ch(p, q, hh + 1.0f / 3), hue_ch(p, q, hh), hue_ch(p, q, hh - 1.0f / 3), 1.0f);
}

uint32_t theme_from_hex(const char* s, bool* ok) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    unsigned v[4] = { 0, 0, 0, 255 };
    int n = 0;
    while (isxdigit((unsigned char)s[n])) n++;
    if (n != 6 && n != 8) {
        if (ok) *ok = false;
        return 0xFF000000u;
    }
    for (int i = 0; i < n / 2; i++) {
        char b[3] = { s[2 * i], s[2 * i + 1], 0 };
        v[i] = (unsigned)strtoul(b, NULL, 16);
    }
    if (ok) *ok = true;
    return v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
}
void theme_to_hex(uint32_t c, char out[10]) {
    if ((c >> 24) == 255u) snprintf(out, 10, "#%02X%02X%02X", c & 255u, (c >> 8) & 255u, (c >> 16) & 255u);
    else snprintf(out, 10, "#%02X%02X%02X%02X", c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24);
}

static float lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float luminance(uint32_t c) { return 0.2126f * lin(CR(c)) + 0.7152f * lin(CG(c)) + 0.0722f * lin(CB(c)); }
float theme_contrast(uint32_t a, uint32_t b) {
    float la = luminance(a), lb = luminance(b);
    return (fmaxf(la, lb) + 0.05f) / (fminf(la, lb) + 0.05f);
}

// --- Derivation ---------------------------------------------------------------------------------------------------
// The neutral ramp: N0-N15 stand for the old fixed grays at these values (0-255), drawn through the theme's surfaces
// and texts at the values they had (the stops): a light theme's surfaces are light, so the ramp turns around by itself.
static const float g_ramp_at[16] = { 0, 10, 16, 21, 26, 32, 38, 46, 56, 66, 90, 115, 145, 180, 215, 250 };
// The hues' centers (degrees) and the old colors' tone limits (HSL lightness): the conversion (tools/theme_convert.py)
// classified each fixed color the same way
static const float g_hue_at[11] = { 0, 25, 45, 85, 135, 168, 192, 215, 262, 295, 330 };

static uint32_t ramp(const Theme* t, float v) {
    struct { float at; uint32_t c; } st[] = {
        { 0, t->light ? 0xFFFFFFFFu : theme_mix(t->col[TK_BG_DEEP], 0xFF000000u, 0.55f) },
        { 12, t->col[TK_BG_DEEP] }, { 16, t->col[TK_BG_FIELD] }, { 21, t->col[TK_BG_APP] }, { 36, t->col[TK_BG_PANEL] },
        { 44, t->col[TK_BG_RAISED] }, { 52, t->col[TK_BG_CONTROL] }, { 70, t->col[TK_BG_HOVER] }, { 92, t->col[TK_BORDER_STRONG] },
        { 112, t->col[TK_TEXT_DISABLED] }, { 150, t->col[TK_TEXT_DIM] }, { 195, t->col[TK_TEXT] }, { 235, t->col[TK_TEXT_STRONG] },
        { 255, t->light ? 0xFF000000u : 0xFFFFFFFFu },
    };
    int n = (int)(sizeof st / sizeof st[0]);
    for (int i = 0; i + 1 < n; i++)
        if (v <= st[i + 1].at) return theme_mix(st[i].c, st[i + 1].c, (v - st[i].at) / (st[i + 1].at - st[i].at));
    return st[n - 1].c;
}

// Okabe-Ito's palette (safe for the three color blindnesses), by the hue it stands in for
static const uint32_t g_okabe[11] = { 0xFF005ED5u, 0xFF009FE6u, 0xFF42E4F0u, 0xFF739E00u, 0xFF739E00u, 0xFF9AB800u,
                                      0xFFE9B456u, 0xFFB27200u, 0xFFA779CCu, 0xFFA779CCu, 0xFFB08AE0u };

// A color pushed lighter or darker (HSL lightness) until its contrast with bg reaches min
static uint32_t ensure_contrast(uint32_t c, uint32_t bg, float min, bool lighter) {
    float h, s, l;
    rgb_hsl(c, &h, &s, &l);
    for (int i = 0; i < 40 && theme_contrast(c, bg) < min; i++) {
        l += lighter ? 0.02f : -0.02f;
        if (l <= 0.0f || l >= 1.0f) break;
        c = (c & 0xFF000000u) | (hsl_rgb(h, s, l) & 0x00FFFFFFu);
    }
    return c;
}

// The hues' tones: a dark theme's dark tone fills (headers, bars under white text), its light tone draws (text, icons on
// the dark panels); a light theme turns them around like the ramp: the "dark" tone is a pastel fill under dark text,
// the "light" one a dark ink on the light panels. The middle tone keeps 3:1 on the panel (icons).
static uint32_t hue_tone(const Theme* t, int h, int tone) {
    float L[3];
    if (t->light) L[0] = 0.86f, L[1] = 0.40f, L[2] = 0.28f;
    else L[0] = 0.32f, L[1] = 0.56f, L[2] = 0.74f;
    float l = L[tone] + (t->light ? -t->hue_light : t->hue_light);
    uint32_t c;
    if (t->palette == 1) { // Okabe-Ito: its color, darker or lighter for the tone
        float hh, ss, ll;
        rgb_hsl(g_okabe[h], &hh, &ss, &ll);
        c = hsl_rgb(hh, ss, tone == 1 ? (t->light ? fminf(ll, 0.42f) : fmaxf(ll, 0.52f)) : l);
    } else {
        c = hsl_rgb(g_hue_at[h], t->hue_sat, l);
    }
    if (tone == 1) c = ensure_contrast(c, t->col[TK_BG_PANEL], 3.15f, !t->light);
    if (tone == 2) c = ensure_contrast(c, t->col[TK_BG_PANEL], 4.6f, !t->light);
    return c;
}

// A color of the Órbita reference (the pins', the families') as this theme's hue of the same family
static uint32_t reclass(const Theme* t, uint32_t c) {
    float h, s, l;
    rgb_hsl(c, &h, &s, &l);
    if (s < 0.15f) return ramp(t, l * 255.0f);
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 11; i++) {
        float d = fabsf(h - g_hue_at[i]);
        if (d > 180.0f) d = 360.0f - d;
        if (d < bd) bd = d, best = i;
    }
    int tone = l < 0.38f ? 0 : l > 0.66f ? 2 : 1;
    return hue_tone(t, best, tone);
}

// The Órbita theme resolved (read once): its families (pins, node headers, assets) are the reference another theme's
// hues follow, and its grays and hues are the scene's table under a light theme
static const char* const g_builtin_text[THEME_BUILTIN_COUNT];
static void theme_defaults(Theme* t);
static const Theme* orbita(void) {
    static Theme orb;
    static bool have;
    if (!have) {
        have = true; // (before reading: Órbita gives all its families, so its resolve never comes back here)
        theme_defaults(&orb);
        theme_read_text(&orb, g_builtin_text[0]);
    }
    return &orb;
}
static uint32_t ref_family(int k) { return orbita()->col[k]; }

void theme_resolve(Theme* t) {
    uint32_t* c = t->col;
#define DEF(k, v) do { if (!t->set[k]) c[k] = (v); } while (0)
    // surfaces and texts: from the application's background and the text (a theme gives at least these two)
    if (!t->set[TK_BG_APP]) c[TK_BG_APP] = t->light ? 0xFFF5F1EEu : 0xFF130E0Bu;
    if (!t->set[TK_TEXT]) c[TK_TEXT] = t->light ? 0xFF30221Au : 0xFFEEE4DDu;
    uint32_t up = t->light ? 0xFFFFFFFFu : c[TK_TEXT], down = t->light ? c[TK_TEXT] : 0xFF000000u;
    DEF(TK_BG_DEEP, theme_mix(c[TK_BG_APP], t->light ? c[TK_TEXT] : 0xFF000000u, t->light ? 0.08f : 0.35f));
    DEF(TK_BG_PANEL, theme_mix(c[TK_BG_APP], up, t->light ? 0.6f : 0.035f));
    DEF(TK_BG_RAISED, theme_mix(c[TK_BG_PANEL], up, t->light ? 0.6f : 0.04f));
    DEF(TK_BG_FIELD, theme_mix(c[TK_BG_APP], t->light ? 0xFFFFFFFFu : down, t->light ? 1.0f : 0.25f));
    DEF(TK_BG_CONTROL, theme_mix(c[TK_BG_RAISED], t->light ? c[TK_TEXT] : up, t->light ? 0.07f : 0.05f));
    DEF(TK_BG_HOVER, theme_mix(c[TK_BG_CONTROL], t->light ? c[TK_TEXT] : up, t->light ? 0.08f : 0.06f));
    DEF(TK_BORDER, theme_mix(c[TK_BG_PANEL], c[TK_TEXT], 0.08f));
    DEF(TK_BORDER_STRONG, theme_mix(c[TK_BG_PANEL], c[TK_TEXT], 0.16f));
    DEF(TK_TEXT_STRONG, theme_mix(c[TK_TEXT], t->light ? 0xFF000000u : 0xFFFFFFFFu, 0.5f));
    DEF(TK_TEXT_DIM, theme_mix(c[TK_TEXT], c[TK_BG_PANEL], 0.33f));
    DEF(TK_TEXT_DISABLED, theme_mix(c[TK_TEXT], c[TK_BG_PANEL], 0.6f));
    DEF(TK_ACCENT, t->light ? 0xFFC08F0Bu : 0xFFF0C63Cu);
    DEF(TK_ACCENT_STRONG, theme_mix(c[TK_ACCENT], t->light ? 0xFF000000u : c[TK_BG_PANEL], 0.2f));
    DEF(TK_ACCENT_DIM, theme_mix(c[TK_ACCENT], c[TK_BG_PANEL], t->light ? 0.78f : 0.7f));
    DEF(TK_BG_SELECTED_DIM, theme_mix(c[TK_ACCENT], c[TK_BG_PANEL], 0.82f));
    DEF(TK_TEXT_ON_ACCENT, theme_contrast(c[TK_ACCENT], 0xFF000000u) > theme_contrast(c[TK_ACCENT], 0xFFFFFFFFu) ? 0xFF181006u : 0xFFFFFFFFu);
    DEF(TK_ACCENT2, t->light ? 0xFF0F70D9u : 0xFF3D9AFFu);
    DEF(TK_OK, t->light ? 0xFF5F9113u : 0xFF97DC3Du);
    DEF(TK_WARN, t->light ? 0xFF006AA6u : 0xFF4BC2FFu);
    DEF(TK_ERROR, t->light ? 0xFF4A3AD6u : 0xFF6C5DFFu);
    DEF(TK_INFO, t->light ? 0xFFD96F3Bu : 0xFFFFA77Au);
    for (int i = 0; i < 16; i++) DEF(TK_N0 + i, ramp(t, g_ramp_at[i]));
    for (int h = 0; h < 11; h++)
        for (int k = 0; k < 3; k++) DEF(TK_RED_D + h * 3 + k, hue_tone(t, h, k));
    DEF(TK_NODE_TITLE, t->light ? c[TK_TEXT_STRONG] : theme_mix(c[TK_TEXT_STRONG], 0xFFFFFFFFu, 0.6f));
    for (int k = TK_PIN_EXEC; k <= TK_ASSET_FOLDER; k++) {
        if (t->set[k] || k == TK_NODE_TITLE) continue;
        uint32_t v = reclass(t, ref_family(k));
        if (k >= TK_NODE_EVENT && k <= TK_NODE_ENTRY) { // a header: its title readable on it (4.5:1)
            float h, sat, l;
            rgb_hsl(v, &h, &sat, &l);
            if (t->light) v = hsl_rgb(h, fminf(sat, 0.6f), 0.84f);
            v = ensure_contrast(v, c[TK_NODE_TITLE], 4.6f, t->light);
        } else if (k <= TK_PIN_DELEGATE || k >= TK_ASSET_BLUEPRINT) { // a pin, an asset's icon: 3:1 on the panel
            v = ensure_contrast(v, c[TK_BG_PANEL], 3.15f, !t->light);
        }
        c[k] = v;
    }
    DEF(TK_GRAPH_BG, theme_mix(c[TK_BG_FIELD], c[TK_BG_APP], 0.5f));
    DEF(TK_GRAPH_GRID, theme_mix(c[TK_GRAPH_BG], c[TK_TEXT], 0.05f));
    DEF(TK_GRAPH_GRID_MAJOR, theme_mix(c[TK_GRAPH_BG], c[TK_TEXT], 0.11f));
    DEF(TK_PREVIEW_TOP, theme_mix(c[TK_BG_RAISED], c[TK_TEXT], t->light ? 0.0f : 0.1f));
    DEF(TK_PREVIEW_BOTTOM, c[TK_BG_APP]);
    DEF(TK_DOCK_TARGET, c[TK_ACCENT]);
    DEF(TK_DOCK_PREVIEW, c[TK_ACCENT]);
#undef DEF
}

// --- The built-in themes (.rtheme text) ---------------------------------------------------------------------------
static const char* const g_builtin_text[THEME_BUILTIN_COUNT] = { // (declared above)
    // Órbita Escuro: bluish graphite surfaces, the Órbita cyan and the Ignição orange (the plan's 9.3 b)
    "rtheme 1\nname = Órbita Escuro\n"
    "bg_deep = #07090D\nbg_app = #0B0E13\nbg_panel = #11151C\nbg_raised = #171C25\nbg_field = #0D1117\nbg_control = #1B212B\n"
    "bg_hover = #232B37\nbg_selected_dim = #243042\nborder = #222A35\nborder_strong = #2E3846\n"
    "text = #DDE4EE\ntext_strong = #F4F7FB\ntext_dim = #95A1B3\ntext_disabled = #5B6676\ntext_on_accent = #061018\n"
    "accent = #3CC6F0\naccent_strong = #12A4D6\naccent_dim = #164659\naccent2 = #FF9A3D\n"
    "ok = #3DDC97\nwarn = #FFC24B\nerror = #FF5D6C\ninfo = #7AA7FF\n"
    "pin_exec = #E8EEF5\npin_bool = #FF5D6C\npin_byte = #9CCBFF\npin_int = #4D8DFF\npin_int64 = #2F5FCC\npin_float = #6FB8FF\n"
    "pin_string = #F26BD0\npin_name = #B98CFF\npin_text = #FF9CC9\npin_vector = #FFC94D\npin_rotator = #FFA24A\n"
    "pin_transform = #FF7A3D\npin_color = #FFB0E0\npin_object = #2ED3A6\npin_class = #8BE6CC\npin_struct = #9C7BFF\n"
    "pin_enum = #B7E25B\npin_wildcard = #9AA6B6\npin_delegate = #F05D6A\n"
    "node_event = #A84E1E\nnode_function = #1F6F8F\nnode_pure = #1D7A6A\nnode_latent = #6A4CB0\nnode_macro = #455568\n"
    "node_flow = #5C6B7A\nnode_cast = #1A7A80\nnode_entry = #704C9A\n"
    "asset_blueprint = #3CC6F0\nasset_material = #3DDC97\nasset_mesh = #FF9A3D\nasset_texture = #FF7A6B\nasset_animation = #FF7AC8\n"
    "asset_audio = #A98BFF\nasset_fx = #FFC24B\nasset_ui = #7AA7FF\nasset_data = #8FA3BF\nasset_level = #E8EEF5\nasset_folder = #D9B26A\n"
    "graph_bg = #0A0D12\ngraph_grid = #141922\ngraph_grid_major = #1D2430\npreview_top = #1C2533\npreview_bottom = #0B0E13\n"
    "hue_sat = 0.78\n",
    // Grafite: neutral grays (#121212 to #2A2A2A), the same accents
    "rtheme 1\nname = Grafite\n"
    "bg_deep = #0C0C0C\nbg_app = #121212\nbg_panel = #181818\nbg_raised = #202020\nbg_field = #0F0F0F\nbg_control = #262626\n"
    "bg_hover = #303030\nbg_selected_dim = #2B3238\nborder = #262626\nborder_strong = #333333\n"
    "text = #E0E0E0\ntext_strong = #F5F5F5\ntext_dim = #A0A0A0\ntext_disabled = #686868\n"
    "accent = #3CC6F0\naccent_strong = #12A4D6\naccent_dim = #17414F\naccent2 = #FF9A3D\n"
    "ok = #3DDC97\nwarn = #FFC24B\nerror = #FF5D6C\ninfo = #7AA7FF\nhue_sat = 0.7\n",
    // Meia-noite: black for OLED screens and dark rooms
    "rtheme 1\nname = Meia-noite\n"
    "bg_deep = #000000\nbg_app = #000000\nbg_panel = #0A0C10\nbg_raised = #11141A\nbg_field = #040507\nbg_control = #151922\n"
    "bg_hover = #1C222D\nbg_selected_dim = #1C2A3A\nborder = #161A22\nborder_strong = #232A36\n"
    "text = #D5DCE6\ntext_strong = #F0F3F8\ntext_dim = #8A96A8\ntext_disabled = #4E5868\n"
    "accent = #3CC6F0\naccent_strong = #12A4D6\naccent_dim = #0F3A4A\naccent2 = #FF9A3D\n"
    "ok = #3DDC97\nwarn = #FFC24B\nerror = #FF5D6C\ninfo = #7AA7FF\nhue_sat = 0.72\nhue_light = -0.03\n",
    // Claro
    "rtheme 1\nname = Claro\nlight = 1\n"
    "bg_deep = #DCE1E8\nbg_app = #EEF1F5\nbg_panel = #F8FAFC\nbg_raised = #FFFFFF\nbg_field = #FFFFFF\nbg_control = #E3E8EF\n"
    "bg_hover = #D6DEE8\nbg_selected_dim = #D5E9F3\nborder = #D3D9E1\nborder_strong = #B4BECB\n"
    "text = #1A2230\ntext_strong = #070B12\ntext_dim = #485365\ntext_disabled = #848E9C\ntext_on_accent = #FFFFFF\n"
    "accent = #0A7FAB\naccent_strong = #076A90\naccent_dim = #C4E3F0\naccent2 = #C85F00\n"
    "ok = #11804F\nwarn = #946000\nerror = #C92E3E\ninfo = #2F62C8\n"
    "graph_bg = #F1F4F8\ngraph_grid = #E3E8EE\ngraph_grid_major = #D2D9E2\npreview_top = #E9EEF4\npreview_bottom = #C9D2DD\n"
    "hue_sat = 0.75\n",
    // Alto Contraste: black, white text, 2 px borders, a yellow accent
    "rtheme 1\nname = Alto Contraste\n"
    "bg_deep = #000000\nbg_app = #000000\nbg_panel = #000000\nbg_raised = #0A0A0A\nbg_field = #000000\nbg_control = #1A1A1A\n"
    "bg_hover = #383838\nbg_selected_dim = #3A3000\nborder = #FFFFFF\nborder_strong = #FFFFFF\n"
    "text = #FFFFFF\ntext_strong = #FFFFFF\ntext_dim = #E6E6E6\ntext_disabled = #A6A6A6\ntext_on_accent = #000000\n"
    "accent = #FFD400\naccent_strong = #FFE34D\naccent_dim = #4D4000\naccent2 = #00E5FF\n"
    "ok = #00FF7F\nwarn = #FFD400\nerror = #FF5C5C\ninfo = #66B3FF\n"
    "border = 2\nhue_sat = 1.0\nhue_light = 0.06\n",
    // Brasa: orange on charcoal
    "rtheme 1\nname = Brasa\n"
    "bg_deep = #0B0907\nbg_app = #14110E\nbg_panel = #1B1714\nbg_raised = #231E1A\nbg_field = #100D0B\nbg_control = #2A231E\n"
    "bg_hover = #362C24\nbg_selected_dim = #43301F\nborder = #2B241F\nborder_strong = #3A3029\n"
    "text = #EFE6DC\ntext_strong = #FFF8F0\ntext_dim = #B3A595\ntext_disabled = #6E6255\n"
    "accent = #FF8A3D\naccent_strong = #E06A1A\naccent_dim = #5A2E12\naccent2 = #FFC24B\n"
    "ok = #7BD88F\nwarn = #FFC24B\nerror = #FF6B5B\ninfo = #8FB8FF\nhue_sat = 0.66\n",
    // Floresta: deep greens
    "rtheme 1\nname = Floresta\n"
    "bg_deep = #070B08\nbg_app = #0E1410\nbg_panel = #141B16\nbg_raised = #1A231D\nbg_field = #0B100D\nbg_control = #1F2A22\n"
    "bg_hover = #28362C\nbg_selected_dim = #23402D\nborder = #213028\nborder_strong = #2D3F33\n"
    "text = #E0EBE2\ntext_strong = #F4FAF5\ntext_dim = #9AB0A0\ntext_disabled = #5C6E61\n"
    "accent = #5FD38D\naccent_strong = #34B56A\naccent_dim = #1C4A2E\naccent2 = #E8B04B\n"
    "ok = #5FD38D\nwarn = #E8C24B\nerror = #FF6B6B\ninfo = #7FB3FF\nhue_sat = 0.6\n",
    // Neon: synthwave
    "rtheme 1\nname = Neon\n"
    "bg_deep = #06030C\nbg_app = #0D0818\nbg_panel = #140D22\nbg_raised = #1B122E\nbg_field = #0A0614\nbg_control = #22173A\n"
    "bg_hover = #2E1F4C\nbg_selected_dim = #3A1D5A\nborder = #2A1D44\nborder_strong = #3D2A63\n"
    "text = #F0E8FF\ntext_strong = #FFFFFF\ntext_dim = #B4A3D6\ntext_disabled = #6D5E8C\n"
    "accent = #FF2BD6\naccent_strong = #D400B0\naccent_dim = #4E1045\naccent2 = #00F0FF\n"
    "ok = #39FF9F\nwarn = #FFE14D\nerror = #FF3B6B\ninfo = #4DA3FF\nhue_sat = 1.0\nhue_light = 0.03\n",
    // Sépia / Papel: a warm light theme
    "rtheme 1\nname = Sépia\nlight = 1\n"
    "bg_deep = #E0D3BC\nbg_app = #EDE3D0\nbg_panel = #F6EFE2\nbg_raised = #FBF7EE\nbg_field = #FFFBF3\nbg_control = #E6DAC4\n"
    "bg_hover = #DACBAF\nbg_selected_dim = #EBD7AE\nborder = #D6C6A8\nborder_strong = #BCA683\n"
    "text = #3A2E20\ntext_strong = #1F150A\ntext_dim = #66553F\ntext_disabled = #9C8A70\ntext_on_accent = #FFF8EC\n"
    "accent = #8C4F16\naccent_strong = #723E0D\naccent_dim = #EDD2AE\naccent2 = #A8352A\n"
    "ok = #3A7535\nwarn = #8A5E00\nerror = #A82F27\ninfo = #2B5683\n"
    "graph_bg = #F3EBDC\ngraph_grid = #E8DCC6\ngraph_grid_major = #DCCCB0\npreview_top = #EFE5D3\npreview_bottom = #D6C6A8\n"
    "hue_sat = 0.42\n",
    // Daltônico seguro: Órbita's surfaces, Okabe-Ito's accents, states and hues
    "rtheme 1\nname = Daltônico seguro\n"
    "bg_deep = #07090D\nbg_app = #0B0E13\nbg_panel = #11151C\nbg_raised = #171C25\nbg_field = #0D1117\nbg_control = #1B212B\n"
    "bg_hover = #232B37\nbg_selected_dim = #243042\nborder = #222A35\nborder_strong = #2E3846\n"
    "text = #DDE4EE\ntext_strong = #F4F7FB\ntext_dim = #95A1B3\ntext_disabled = #5B6676\ntext_on_accent = #061018\n"
    "accent = #56B4E9\naccent_strong = #2E95D3\naccent_dim = #183F57\naccent2 = #E69F00\n"
    "ok = #1FB88A\nwarn = #F0E442\nerror = #E66A1A\ninfo = #56B4E9\npalette = okabe\n",
    // Azul Real (0.44, the default: the user's two mockups): deep navy surfaces, one saturated blue for what is active
    // (tabs, selection, the primary buttons), green Play, the Blueprint headers red (events) / blue (functions) /
    // green (pure), orange folders; thin borders, 4-6 px corners
    "rtheme 1\nname = Azul Real\n"
    "bg_deep = #06090E\nbg_app = #0B0F15\nbg_panel = #10151D\nbg_raised = #161D27\nbg_field = #0A0E14\nbg_control = #19212C\n"
    "bg_hover = #222C3A\nbg_selected_dim = #1A2B45\nborder = #1D2531\nborder_strong = #2A3442\n"
    "text = #D6DDE7\ntext_strong = #F3F6FA\ntext_dim = #8D99AC\ntext_disabled = #586374\ntext_on_accent = #FFFFFF\n"
    "accent = #2B6FE3\naccent_strong = #2259C2\naccent_dim = #172F55\naccent2 = #34C46B\n"
    "ok = #3DD68C\nwarn = #F5B942\nerror = #F25767\ninfo = #5B9BFF\n"
    "node_event = #A3262E\nnode_function = #1F4F9E\nnode_pure = #1E6B3E\nnode_latent = #5B3FA6\nnode_macro = #3F4B5C\n"
    "node_flow = #4A5566\nnode_cast = #1C6A72\nnode_entry = #6A3F9A\n"
    "asset_blueprint = #4D8DFF\nasset_material = #3DD68C\nasset_mesh = #2EC4D6\nasset_texture = #FF6B6B\nasset_animation = #FF7AC8\n"
    "asset_audio = #A98BFF\nasset_fx = #F5B942\nasset_ui = #7AA7FF\nasset_data = #8FA3BF\nasset_level = #E8EEF5\nasset_folder = #E9A23B\n"
    "graph_bg = #0A0E14\ngraph_grid = #121822\ngraph_grid_major = #1A2230\npreview_top = #18212E\npreview_bottom = #0A0E14\n"
    "radius_window = 6\nradius_panel = 6\nradius_frame = 4\nradius_popup = 6\nradius_tab = 4\nradius_node = 6\n"
    "space_card = 4\nspace_pad = 8\nspace_item = 6\nhue_sat = 0.78\n",
};

static const float g_metric_default[TM_COUNT] = { 8, 6, 4, 8, 4, 8, 4, 8, 6, 1, 90, 160, 220, 15, 17 };

static Theme g_themes[THEME_MAX];
static int g_count, g_cur, g_before = -1;
static Theme g_edit;
static bool g_editing, g_dirty_style = true;
static uint32_t g_gen = 1;
static char g_user_dir[600];

static void apply_table(const Theme* t) {
    memcpy(g_theme_u32, t->col, sizeof g_theme_u32);
    memcpy(g_theme_scene_u32, t->col, sizeof g_theme_scene_u32);
    if (t->light) { // over the 3D scene: a dark theme's grays and hues (the overlays' text keeps its contrast on a render)
        const Theme* o = orbita();
        for (int k = TK_N0; k <= TK_PINK_L; k++) g_theme_scene_u32[k] = o->col[k];
        g_theme_scene_u32[TK_NODE_TITLE] = o->col[TK_NODE_TITLE];
    }
    for (int k = 0; k < TK_COUNT; k++) g_theme_v4[k] = (ImVec4){ CR(t->col[k]), CG(t->col[k]), CB(t->col[k]), CA(t->col[k]) };
    memcpy(g_theme_metric, t->metric, sizeof g_theme_metric);
    g_dirty_style = true;
    g_gen++;
}

static void theme_defaults(Theme* t) { // (declared above)
    memset(t, 0, sizeof *t);
    memcpy(t->metric, g_metric_default, sizeof t->metric);
    t->hue_sat = 0.75f;
}

bool theme_read_text(Theme* t, const char* text) {
    char base_name[THEME_NAME] = { 0 };
    Theme tmp;
    theme_defaults(&tmp);
    bool header = false;
    const char* p = text;
    char line[256];
    while (*p) {
        size_t n = strcspn(p, "\r\n");
        size_t m = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, m);
        line[m] = 0;
        p += n;
        while (*p == '\r' || *p == '\n') p++;
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ';' || *s == '#') continue;
        if (!header) {
            if (strncmp(s, "rtheme", 6) != 0) return false;
            header = true;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) continue;
        char* ke = eq;
        while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
        if (!strcmp(s, "name")) snprintf(tmp.name, sizeof tmp.name, "%s", v);
        else if (!strcmp(s, "base")) snprintf(base_name, sizeof base_name, "%s", v);
        else if (!strcmp(s, "light")) tmp.light = atoi(v) != 0;
        else if (!strcmp(s, "hue_sat")) tmp.hue_sat = fminf(fmaxf((float)atof(v), 0.0f), 1.0f);
        else if (!strcmp(s, "hue_light")) tmp.hue_light = fminf(fmaxf((float)atof(v), -0.3f), 0.3f);
        else if (!strcmp(s, "palette")) tmp.palette = !strcmp(v, "okabe") || atoi(v) == 1;
        else {
            bool done = false;
            for (int k = 0; k < TK_COUNT && !done; k++)
                if (!strcmp(s, g_tok[k].name)) {
                    bool ok;
                    uint32_t c = theme_from_hex(v, &ok);
                    if (ok) tmp.col[k] = c, tmp.set[k] = 1;
                    done = true;
                }
            for (int m = 0; m < TM_COUNT && !done; m++)
                if (!strcmp(s, g_met[m].name)) {
                    float x = (float)atof(v);
                    tmp.metric[m] = fminf(fmaxf(x, g_met[m].lo), g_met[m].hi);
                    done = true;
                }
        }
    }
    if (!header || !tmp.name[0]) return false;
    if (base_name[0]) { // a user's theme: what it does not give comes from its base (its given values, then derived)
        int b = theme_find(base_name);
        if (b >= 0 && b < THEME_BUILTIN_COUNT) {
            Theme bt;
            theme_defaults(&bt);
            theme_read_text(&bt, g_builtin_text[b]);
            for (int k = 0; k < TK_COUNT; k++)
                if (!tmp.set[k] && bt.set[k]) tmp.col[k] = bt.col[k], tmp.set[k] = 1;
            if (!strstr(text, "\nlight")) tmp.light = bt.light;
            if (!strstr(text, "\nhue_sat")) tmp.hue_sat = bt.hue_sat;
            if (!strstr(text, "\nhue_light")) tmp.hue_light = bt.hue_light;
            if (!strstr(text, "\npalette")) tmp.palette = bt.palette;
            for (int m = 0; m < TM_COUNT; m++) {
                char key[48];
                snprintf(key, sizeof key, "\n%s", g_met[m].name);
                if (!strstr(text, key)) tmp.metric[m] = bt.metric[m];
            }
        }
        snprintf(tmp.base, sizeof tmp.base, "%s", base_name);
    }
    theme_resolve(&tmp);
    *t = tmp;
    return true;
}

bool theme_write_text(const Theme* t, char* out, size_t cap) {
    size_t n = 0;
#define PUT(...) do { int w_ = snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__); if (w_ < 0) return false; n += (size_t)w_; } while (0)
    PUT("rtheme 1\n; Real Engine theme (Editor Preferences > Appearance). #RRGGBB or #RRGGBBAA; the tokens not here are derived.\n");
    PUT("name = %s\n", t->name);
    if (t->base[0]) PUT("base = %s\n", t->base);
    PUT("light = %d\nhue_sat = %.3f\nhue_light = %.3f\n", t->light ? 1 : 0, t->hue_sat, t->hue_light);
    if (t->palette == 1) PUT("palette = okabe\n");
    for (int k = 0; k < TK_COUNT; k++) {
        if (!t->set[k]) continue;
        char hex[10];
        theme_to_hex(t->col[k], hex);
        PUT("%s = %s\n", g_tok[k].name, hex);
    }
    for (int m = 0; m < TM_COUNT; m++) PUT("%s = %g\n", g_met[m].name, t->metric[m]);
#undef PUT
    return n < cap;
}

bool theme_write_file(const Theme* t, const char* path) {
    static char buf[16384];
    if (!theme_write_text(t, buf, sizeof buf)) return false;
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, strlen(buf), f) == strlen(buf);
    return fclose(f) == 0 && ok;
}

bool theme_read_file(Theme* t, const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    static char buf[32768];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* s = buf;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3; // (a BOM)
    if (!theme_read_text(t, s)) return false;
    snprintf(t->file, sizeof t->file, "%s", path);
    return true;
}

void theme_builtin(Theme* t, int i) {
    theme_defaults(t);
    theme_read_text(t, g_builtin_text[i >= 0 && i < THEME_BUILTIN_COUNT ? i : 0]);
    t->builtin = true;
}

void theme_set_user_dir(const char* dir) { snprintf(g_user_dir, sizeof g_user_dir, "%s", dir ? dir : ""); }
void theme_user_dir(char* out, size_t cap) {
    if (g_user_dir[0]) {
        snprintf(out, cap, "%s", g_user_dir);
        return;
    }
    char d[512];
    if (!re_config_dir(d, sizeof d)) snprintf(d, sizeof d, ".");
    snprintf(out, cap, "%s\\Themes", d);
}

static void add_user_file(void* user, const char* name) {
    (void)user;
    size_t n = strlen(name);
    if (n < 8 || strcasecmp_(name + n - 7, ".rtheme") != 0 || g_count >= THEME_MAX) return;
    char dir[600], path[900];
    theme_user_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    Theme t;
    if (!theme_read_file(&t, path)) return;
    if (theme_find(t.name) >= 0) return; // (a name of a built-in or another file: the first one wins)
    g_themes[g_count++] = t;
}

static void load_user_themes(void) {
    g_count = THEME_BUILTIN_COUNT;
    char dir[600];
    theme_user_dir(dir, sizeof dir);
    re_list_dir(dir, add_user_file, NULL);
}

void theme_init(void) {
    for (int i = 0; i < THEME_BUILTIN_COUNT; i++) theme_builtin(&g_themes[i], i);
    g_count = THEME_BUILTIN_COUNT; // (theme_find during the user's files reads these)
    load_user_themes();
    g_cur = THEME_DEFAULT; // (Q26: Azul Real since 0.44)
    apply_table(&g_themes[THEME_DEFAULT]);
}

int theme_count(void) { return g_count; }
const Theme* theme_at(int i) { return i >= 0 && i < g_count ? &g_themes[i] : NULL; }
int theme_current(void) { return g_cur; }
const char* theme_current_name(void) { return g_editing ? g_edit.name : g_themes[g_cur].name; }
uint32_t theme_generation(void) { return g_gen; }
int theme_find(const char* name) {
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_themes[i].name, name)) return i;
    return -1;
}
bool theme_select(int i) {
    if (i < 0 || i >= g_count) return false;
    g_editing = false;
    g_cur = i;
    apply_table(&g_themes[i]);
    return true;
}
bool theme_select_name(const char* name) { return theme_select(theme_find(name)); }
bool theme_style_dirty(void) { return g_dirty_style; }

// --- Editing --------------------------------------------------------------------------------------------------------
Theme* theme_edit_begin(int from) {
    const Theme* src = theme_at(from);
    if (!src) return NULL;
    if (!g_editing) g_before = g_cur;
    g_edit = *src;
    if (src->builtin) { // a copy of a built-in theme: all its values given (its base kept for the derived ones)
        snprintf(g_edit.base, sizeof g_edit.base, "%s", src->name);
        g_edit.builtin = false;
        g_edit.file[0] = 0;
    }
    g_editing = true;
    apply_table(&g_edit);
    return &g_edit;
}
Theme* theme_edit_theme(void) { return g_editing ? &g_edit : NULL; }
bool theme_editing(void) { return g_editing; }
void theme_edit_changed(void) {
    if (!g_editing) return;
    theme_resolve(&g_edit);
    apply_table(&g_edit);
}
void theme_edit_cancel(void) {
    if (!g_editing) return;
    g_editing = false;
    theme_select(g_before >= 0 ? g_before : THEME_DEFAULT);
}

static void safe_file_name(const char* name, char* out, size_t cap) {
    size_t o = 0;
    for (const char* s = name; *s && o + 1 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        out[o++] = (ch < 32 || strchr("\\/:*?\"<>|", ch)) ? '_' : (char)ch;
    }
    out[o] = 0;
    if (!o) snprintf(out, cap, "theme");
}

static int store_user(Theme* t) {
    char dir[600], fn[200], path[900];
    theme_user_dir(dir, sizeof dir);
    re_make_dirs(dir);
    safe_file_name(t->name, fn, sizeof fn);
    snprintf(path, sizeof path, "%s\\%s.rtheme", dir, fn);
    t->builtin = false;
    snprintf(t->file, sizeof t->file, "%s", path);
    if (!theme_write_file(t, path)) return -1;
    int i = theme_find(t->name);
    if (i >= 0 && i < THEME_BUILTIN_COUNT) return -1; // (a built-in's name: never replaced)
    if (i < 0) {
        if (g_count >= THEME_MAX) return -1;
        i = g_count++;
    }
    g_themes[i] = *t;
    return i;
}

int theme_edit_save(const char* name) {
    if (!g_editing || !name || !name[0]) return -1;
    int at = theme_find(name);
    if (at >= 0 && at < THEME_BUILTIN_COUNT) return -1;
    Theme t = g_edit;
    snprintf(t.name, sizeof t.name, "%s", name);
    int i = store_user(&t);
    if (i < 0) return -1;
    g_editing = false;
    theme_select(i);
    return i;
}

int theme_duplicate(int i, const char* name) {
    const Theme* src = theme_at(i);
    if (!src || !name || !name[0] || theme_find(name) >= 0) return -1;
    Theme t = *src;
    snprintf(t.name, sizeof t.name, "%s", name);
    if (src->builtin) snprintf(t.base, sizeof t.base, "%s", src->name);
    return store_user(&t);
}

bool theme_delete_user(int i) {
    if (i < THEME_BUILTIN_COUNT || i >= g_count) return false;
    if (g_themes[i].file[0]) re_remove_file(g_themes[i].file);
    memmove(&g_themes[i], &g_themes[i + 1], sizeof(Theme) * (size_t)(g_count - i - 1));
    g_count--;
    if (g_cur == i) theme_select(THEME_DEFAULT);
    else if (g_cur > i) g_cur--;
    return true;
}

int theme_import(const char* path) {
    Theme t;
    if (!theme_read_file(&t, path)) return -1;
    int at = theme_find(t.name);
    if (at >= 0 && at < THEME_BUILTIN_COUNT) { // a built-in's name: imported under another
        char n2[THEME_NAME];
        snprintf(n2, sizeof n2, "%.50s (2)", t.name);
        snprintf(t.name, sizeof t.name, "%s", n2);
    }
    return store_user(&t);
}

bool theme_export(int i, const char* path) {
    const Theme* t = theme_at(i);
    if (g_editing && i == -1) t = &g_edit;
    if (!t) return false;
    Theme c = *t;
    if (c.builtin) memset(c.set, 1, sizeof c.set), c.base[0] = 0; // (a built-in exported whole: every token)
    return theme_write_file(&c, path);
}

// --- Contrast -----------------------------------------------------------------------------------------------------
// WCAG 2: text 4.5:1 (the main and secondary texts on every surface they sit on), large text, icons and controls 3:1
int theme_check_contrast(const Theme* t, char* out, size_t cap) {
    static const struct { uint8_t fg, bg; float min; } pairs[] = {
        { TK_TEXT, TK_BG_APP, 4.5f }, { TK_TEXT, TK_BG_PANEL, 4.5f }, { TK_TEXT, TK_BG_RAISED, 4.5f }, { TK_TEXT, TK_BG_FIELD, 4.5f },
        { TK_TEXT, TK_BG_CONTROL, 4.5f }, { TK_TEXT, TK_BG_HOVER, 4.5f }, { TK_TEXT, TK_ACCENT_DIM, 4.5f },
        { TK_TEXT, TK_BG_SELECTED_DIM, 4.5f }, { TK_TEXT_DIM, TK_BG_PANEL, 4.5f }, { TK_TEXT_DIM, TK_BG_APP, 4.5f },
        { TK_TEXT_DIM, TK_BG_RAISED, 4.5f }, { TK_TEXT_ON_ACCENT, TK_ACCENT, 4.5f }, { TK_TEXT_DISABLED, TK_BG_PANEL, 2.0f },
        { TK_ACCENT, TK_BG_PANEL, 3.0f }, { TK_ACCENT, TK_BG_APP, 3.0f }, { TK_ACCENT2, TK_BG_APP, 3.0f },
        { TK_OK, TK_BG_PANEL, 3.0f }, { TK_WARN, TK_BG_PANEL, 3.0f }, { TK_ERROR, TK_BG_PANEL, 3.0f }, { TK_INFO, TK_BG_PANEL, 3.0f },
        { TK_NODE_TITLE, TK_NODE_EVENT, 4.5f }, { TK_NODE_TITLE, TK_NODE_FUNCTION, 4.5f }, { TK_NODE_TITLE, TK_NODE_PURE, 4.5f },
        { TK_NODE_TITLE, TK_NODE_LATENT, 4.5f }, { TK_NODE_TITLE, TK_NODE_MACRO, 4.5f }, { TK_NODE_TITLE, TK_NODE_FLOW, 4.5f },
        { TK_NODE_TITLE, TK_NODE_CAST, 4.5f }, { TK_NODE_TITLE, TK_NODE_ENTRY, 4.5f },
        { TK_PIN_BOOL, TK_GRAPH_BG, 3.0f }, { TK_PIN_INT, TK_GRAPH_BG, 3.0f }, { TK_PIN_STRING, TK_GRAPH_BG, 3.0f },
        { TK_PIN_OBJECT, TK_GRAPH_BG, 3.0f }, { TK_PIN_VECTOR, TK_GRAPH_BG, 3.0f },
        { TK_N13, TK_N3, 4.5f }, { TK_N14, TK_N5, 4.5f }, // (the ramp's text grays on its panel grays)
    };
    int bad = 0;
    size_t n = 0;
    if (out && cap) out[0] = 0;
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        float r = theme_contrast(t->col[pairs[i].fg], t->col[pairs[i].bg]);
        if (r + 1e-3f >= pairs[i].min) continue;
        bad++;
        if (out && n < cap) {
            int w = snprintf(out + n, cap - n, "%s: %s on %s = %.2f (< %.1f)\n", t->name, g_tok[pairs[i].fg].name, g_tok[pairs[i].bg].name, r, pairs[i].min);
            if (w > 0) n += (size_t)w;
        }
    }
    // the hues (icons, tracks): the middle tone on the panel, 3:1
    for (int h = 0; h < 11; h++) {
        int k = TK_RED + h * 3;
        float r = theme_contrast(t->col[k], t->col[TK_BG_PANEL]);
        if (r + 1e-3f >= 3.0f) continue;
        bad++;
        if (out && n < cap) {
            int w = snprintf(out + n, cap - n, "%s: %s on bg_panel = %.2f (< 3.0)\n", t->name, g_tok[k].name, r);
            if (w > 0) n += (size_t)w;
        }
    }
    return bad;
}

// --- ImGui ------------------------------------------------------------------------------------------------------------
void theme_apply_imgui(float scale) {
    const uint32_t* c = g_theme_u32;
    ImGuiStyle* s = igGetStyle();
    igStyleColorsDark(s);
    const float* m = g_theme_metric;
    s->WindowRounding = m[TM_RADIUS_WINDOW];
    s->WindowBorderHoverPadding = 6.0f; // (2026-11-10: the windows' edges grabbed from 6 px around them)
    s->ChildRounding = m[TM_RADIUS_PANEL];
    s->FrameRounding = m[TM_RADIUS_FRAME];
    s->PopupRounding = m[TM_RADIUS_POPUP];
    s->GrabRounding = m[TM_RADIUS_FRAME];
    s->TabRounding = m[TM_RADIUS_TAB];
    s->ScrollbarRounding = m[TM_RADIUS_POPUP];
    s->ScrollbarSize = 10.0f;
    s->GrabMinSize = 9.0f;
    s->WindowBorderSize = m[TM_BORDER];
    s->ChildBorderSize = m[TM_BORDER];
    s->PopupBorderSize = m[TM_BORDER];
    s->FrameBorderSize = m[TM_BORDER] > 1.5f ? 1.0f : 0.0f; // (Alto Contraste: the fields outlined)
    s->TabBorderSize = 0.0f;
    s->TabBarBorderSize = m[TM_BORDER];
    s->TabBarOverlineSize = 2.0f;
    s->DockingSeparatorSize = m[TM_SPACE_CARD]; // the panels as cards, with a gap between them
    s->WindowPadding = (ImVec2){ m[TM_SPACE_PAD], m[TM_SPACE_PAD] - 2.0f };
    s->FramePadding = (ImVec2){ 6, 3 };
    s->ItemSpacing = (ImVec2){ m[TM_SPACE_ITEM], 4 };
    s->ItemInnerSpacing = (ImVec2){ 4, 4 };
    s->CellPadding = (ImVec2){ 6, 2 };
    s->IndentSpacing = 14.0f;
    s->WindowTitleAlign = (ImVec2){ 0.0f, 0.5f };
    s->SeparatorTextBorderSize = 1.0f;
    s->SeparatorTextPadding = (ImVec2){ 10, 2 };
    s->HoverDelayShort = 0.5f; // the widgets' own tooltips: after a moment still over them
    s->HoverDelayNormal = 0.6f;
    s->HoverStationaryDelay = 0.2f;
    ImVec4* col = s->Colors;
#define SET(i, v) col[i] = (ImVec4){ CR(v), CG(v), CB(v), CA(v) }
    SET(ImGuiCol_Text, c[TK_TEXT]);
    SET(ImGuiCol_TextDisabled, c[TK_TEXT_DISABLED]);
    SET(ImGuiCol_WindowBg, c[TK_BG_APP]);
    SET(ImGuiCol_ChildBg, c[TK_BG_PANEL]);
    SET(ImGuiCol_PopupBg, c[TK_BG_RAISED]);
    SET(ImGuiCol_Border, c[TK_BORDER]);
    SET(ImGuiCol_BorderShadow, THEME_NONE);
    SET(ImGuiCol_FrameBg, c[TK_BG_FIELD]);
    SET(ImGuiCol_FrameBgHovered, theme_mix(c[TK_BG_FIELD], c[TK_BG_HOVER], 0.6f));
    SET(ImGuiCol_FrameBgActive, c[TK_BG_HOVER]);
    SET(ImGuiCol_TitleBg, c[TK_BG_APP]);
    SET(ImGuiCol_TitleBgActive, c[TK_BG_RAISED]);
    SET(ImGuiCol_TitleBgCollapsed, c[TK_BG_APP]);
    SET(ImGuiCol_MenuBarBg, c[TK_BG_APP]);
    SET(ImGuiCol_ScrollbarBg, THEME_NONE);
    SET(ImGuiCol_ScrollbarGrab, c[TK_BORDER_STRONG]);
    SET(ImGuiCol_ScrollbarGrabHovered, theme_mix(c[TK_BORDER_STRONG], c[TK_TEXT_DISABLED], 0.5f));
    SET(ImGuiCol_ScrollbarGrabActive, c[TK_TEXT_DISABLED]);
    SET(ImGuiCol_CheckMark, c[TK_ACCENT]);
    SET(ImGuiCol_CheckboxSelectedBg, c[TK_BG_FIELD]);
    SET(ImGuiCol_SliderGrab, c[TK_ACCENT_STRONG]);
    SET(ImGuiCol_SliderGrabActive, c[TK_ACCENT]);
    SET(ImGuiCol_Button, c[TK_BG_CONTROL]);
    SET(ImGuiCol_ButtonHovered, c[TK_BG_HOVER]);
    SET(ImGuiCol_ButtonActive, c[TK_ACCENT_STRONG]);
    SET(ImGuiCol_Header, theme_mix(c[TK_BG_CONTROL], c[TK_BG_RAISED], 0.4f));
    SET(ImGuiCol_HeaderHovered, c[TK_BG_HOVER]);
    SET(ImGuiCol_HeaderActive, c[TK_ACCENT_DIM]);
    SET(ImGuiCol_Separator, c[TK_BORDER]);
    SET(ImGuiCol_SeparatorHovered, c[TK_ACCENT_STRONG]);
    SET(ImGuiCol_SeparatorActive, c[TK_ACCENT]);
    SET(ImGuiCol_ResizeGrip, THEME_NONE);
    SET(ImGuiCol_ResizeGripHovered, c[TK_ACCENT_STRONG]);
    SET(ImGuiCol_ResizeGripActive, c[TK_ACCENT]);
    SET(ImGuiCol_InputTextCursor, c[TK_TEXT_STRONG]);
    SET(ImGuiCol_Tab, c[TK_BG_APP]);
    SET(ImGuiCol_TabHovered, c[TK_BG_HOVER]);
    SET(ImGuiCol_TabSelected, theme_mix(c[TK_BG_PANEL], c[TK_ACCENT], 0.3f)); // (Q26: the active tab tinted by the accent, the mockups')
    SET(ImGuiCol_TabSelectedOverline, c[TK_ACCENT]);
    SET(ImGuiCol_TabDimmed, c[TK_BG_APP]);
    SET(ImGuiCol_TabDimmedSelected, theme_mix(c[TK_BG_PANEL], c[TK_ACCENT], 0.08f));
    SET(ImGuiCol_TabDimmedSelectedOverline, c[TK_BORDER_STRONG]);
    SET(ImGuiCol_DockingPreview, theme_alpha(c[TK_DOCK_PREVIEW], 90));
    SET(ImGuiCol_DockingEmptyBg, c[TK_BG_DEEP]);
    SET(ImGuiCol_PlotLines, c[TK_ACCENT]);
    SET(ImGuiCol_PlotLinesHovered, c[TK_ACCENT2]);
    SET(ImGuiCol_PlotHistogram, c[TK_ACCENT]);
    SET(ImGuiCol_PlotHistogramHovered, c[TK_ACCENT2]);
    SET(ImGuiCol_TableHeaderBg, c[TK_BG_RAISED]);
    SET(ImGuiCol_TableBorderStrong, c[TK_BORDER_STRONG]);
    SET(ImGuiCol_TableBorderLight, c[TK_BORDER]);
    SET(ImGuiCol_TableRowBg, c[TK_BG_PANEL]);
    SET(ImGuiCol_TableRowBgAlt, theme_mix(c[TK_BG_PANEL], c[TK_BG_RAISED], 0.45f));
    SET(ImGuiCol_TextLink, c[TK_ACCENT]);
    SET(ImGuiCol_TextSelectedBg, theme_alpha(c[TK_ACCENT], 100));
    SET(ImGuiCol_TreeLines, c[TK_BORDER_STRONG]);
    SET(ImGuiCol_DragDropTarget, c[TK_ACCENT]);
    SET(ImGuiCol_DragDropTargetBg, theme_alpha(c[TK_ACCENT], 40));
    SET(ImGuiCol_UnsavedMarker, c[TK_ACCENT2]);
    SET(ImGuiCol_NavCursor, c[TK_ACCENT]);
    SET(ImGuiCol_NavWindowingHighlight, c[TK_ACCENT]);
    SET(ImGuiCol_NavWindowingDimBg, theme_alpha(c[TK_BG_DEEP], 120));
    SET(ImGuiCol_ModalWindowDimBg, theme_alpha(c[TK_BG_DEEP], 150));
#undef SET
    ImGuiStyle_ScaleAllSizes(s, scale);
    s->FontScaleDpi = scale;
    RE_FadeInSeconds = m[TM_DUR_ENTER] * 0.001f * 0.64f; // (220 ms: the 0.14 s the windows had)
    RE_FadeQuickSeconds = m[TM_DUR_FAST] * 0.001f;
    RE_FadeOutSeconds = m[TM_DUR_NORMAL] * 0.001f * 0.75f;
    RE_HoverEaseSeconds = m[TM_DUR_NORMAL] * 0.001f * 0.75f;
    g_dirty_style = false;
}

// --- Fonts ------------------------------------------------------------------------------------------------------------
#include "font_inter_regular.inc"
#include "font_inter_semibold.inc"
#include "font_sora_semibold.inc"
#include "font_jetbrains_mono.inc"

static ImFont* add_font(const unsigned char* data, size_t n, float size, bool merge_fallback) {
    ImGuiIO* io = igGetIO_Nil();
    ImFontConfig* cfg = ImFontConfig_ImFontConfig();
    cfg->FontDataOwnedByAtlas = false; // (the executable's bytes)
    ImFont* f = ImFontAtlas_AddFontFromMemoryTTF(io->Fonts, (void*)data, (int)n, size, cfg, NULL);
    ImFontConfig_destroy(cfg);
#ifdef _WIN32
    if (f && merge_fallback) { // what Inter lacks (other scripts, symbols): Segoe UI's, merged into it
        static const char* const fb[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\seguisym.ttf" };
        for (size_t i = 0; i < sizeof fb / sizeof fb[0]; i++) {
            if (GetFileAttributesA(fb[i]) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig* mc = ImFontConfig_ImFontConfig();
            mc->MergeMode = true;
            ImFontAtlas_AddFontFromFileTTF(io->Fonts, fb[i], size, mc, NULL);
            ImFontConfig_destroy(mc);
        }
    }
#else
    (void)merge_fallback;
#endif
    return f;
}

// (2026-11-23) Sora SemiBold's file: the editor's intro (brand.c) writes the name with it, the menu bar's title font
const unsigned char* theme_title_font(size_t* n) {
    *n = sizeof g_font_sora_semibold;
    return g_font_sora_semibold;
}

void theme_load_fonts(float size) {
    g_font_ui = add_font(g_font_inter_regular, sizeof g_font_inter_regular, size, true);
    g_font_mono = add_font(g_font_jetbrains_mono, sizeof g_font_jetbrains_mono, size, false);
    g_font_title = add_font(g_font_sora_semibold, sizeof g_font_sora_semibold, size, false);
    g_font_bold = add_font(g_font_inter_semibold, sizeof g_font_inter_semibold, size, false); // (bold: the tooltips' names, the headers)
}
