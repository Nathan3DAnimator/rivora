// The hooks the imgui copied from the Tlender calls (external/cimgui/cimconfig.h): typed text parsed by ImGui; a
// field's range is where a drag goes (soft), a typed value goes past it (Blender's), never below 0 when the range
// starts at 0 or above (sizes, intensities), and not at all when the field always clamps (a byte, a 0-1 factor so marked).
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "ui_tips.h"

int tl_imgui_temp_input_apply(const char* buf, int data_type, void* p_data, const char* format, float x0, float y0,
                              float x1, float y1) {
    (void)buf; (void)data_type; (void)p_data; (void)format; (void)x0; (void)y0; (void)x1; (void)y1;
    return -1;
}

int num_limits_field(double lo, double hi, int flags, double* hlo, double* hhi) {
    if (lo < hi && (flags & ImGuiSliderFlags_ClampOnInput)) {
        *hlo = lo, *hhi = hi;
        return 3;
    }
    *hlo = lo < hi && lo >= 0.0 ? 0.0 : -HUGE_VAL;
    *hhi = HUGE_VAL;
    return lo < hi ? 3 : 0;
}

void num_limits_group(bool on) { (void)on; }

// The descriptions (ui_tips.h): the hovered labeled widget, reported by the patched ItemAdd; copied only while hovered
UiHover g_ui_hover;

void re_imgui_hover_label(const char* label, unsigned int id, float x0, float y0, float x1, float y1, int window_flags,
                          float win_x0, float win_x1, float vp_x0, float vp_y0, float vp_x1, float vp_y1) {
    UiHover* h = &g_ui_hover;
    if (h->id != id || h->frame != igGetFrameCount() - 1) snprintf(h->label, sizeof h->label, "%s", label);
    h->id = id;
    h->frame = igGetFrameCount();
    h->window_flags = window_flags;
    h->x0 = x0, h->y0 = y0, h->x1 = x1, h->y1 = y1;
    h->win_x0 = win_x0, h->win_x1 = win_x1;
    h->vp_x0 = vp_x0, h->vp_y0 = vp_y0, h->vp_x1 = vp_x1, h->vp_y1 = vp_y1;
}

void re_imgui_tooltip_begun(void) { g_ui_hover.tooltip_frame = igGetFrameCount(); }

// The combos' search field (imgui_widgets.cpp, sugestão #60): its hint, in the editor's language (set by the editor each
// frame: g_re_search_hint; English where no one sets it, the tests)
const char* g_re_search_hint = "Search...";
const char* re_imgui_search_hint(void) { return g_re_search_hint; }

// The Assistant's "Show me where" (src/assistant_ui.c): the labels it points at (as shown: translated, without their ##
// part) and where they were drawn this frame (GREHighlight on: ItemAdd offers every labeled item)
char g_re_hl_label[4][128];
int g_re_hl_nlabel;
float g_re_hl_rect[8][4];
int g_re_hl_nrect, g_re_hl_frame;

// (item 21) the UI sweep's (ui_sweep.c sets it while it runs: every labeled item drawn)
void (*g_ui_sweep_item_hook)(const char* label, float x0, float y0, float x1, float y1);
void re_imgui_highlight_item(const char* label, float x0, float y0, float x1, float y1) {
    if (g_ui_sweep_item_hook) g_ui_sweep_item_hook(label, x0, y0, x1, y1);
    const char* cut = strstr(label, "##");
    size_t n = cut ? (size_t)(cut - label) : strlen(label);
    while (n && label[n - 1] == ' ') n--;
    for (int i = 0; i < g_re_hl_nlabel; i++) {
        size_t k = strlen(g_re_hl_label[i]);
        if (k != n || strncmp(label, g_re_hl_label[i], n)) continue;
        int f = igGetFrameCount();
        if (f != g_re_hl_frame) g_re_hl_frame = f, g_re_hl_nrect = 0;
        if (g_re_hl_nrect < 8) {
            float* r = g_re_hl_rect[g_re_hl_nrect++];
            r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
        }
        return;
    }
}
