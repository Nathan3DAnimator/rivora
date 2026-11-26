// The number fields (ui_number.h): the UI range for dragging, the hard limits for any value. Dear ImGui already lets a
// Ctrl+click typed value pass a drag's or a slider's range unless ImGuiSliderFlags_ClampOnInput (in AlwaysClamp) is
// set, and keeps a value already past the range while it is pushed further out: the fields here never pass it, then
// hold the value to the hard limits only
#include "ui_number.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

static int ui_flags(int flags) { return flags & ~(int)ImGuiSliderFlags_ClampOnInput; }

static bool hold_f(float* v, float lo, float hi) {
    if (!(lo < hi)) return false;
    float c = *v < lo ? lo : *v > hi ? hi : *v;
    if (!(c == c)) c = lo; // (a NaN typed)
    bool moved = c != *v;
    *v = c;
    return moved;
}
static void hold_i(int* v, int lo, int hi) {
    if (lo < hi) *v = *v < lo ? lo : *v > hi ? hi : *v;
}

bool ui_num_float(const char* label, float* v, float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags) {
    bool c = igDragFloat(label, v, speed, ui_min, ui_max, fmt, ui_flags(flags));
    if (c) hold_f(v, hard_min, hard_max);
    return c;
}
bool ui_num_int(const char* label, int* v, float speed, int ui_min, int ui_max, int hard_min, int hard_max, const char* fmt, int flags) {
    bool c = igDragInt(label, v, speed, ui_min, ui_max, fmt, ui_flags(flags));
    if (c) hold_i(v, hard_min, hard_max);
    return c;
}
bool ui_slider_float(const char* label, float* v, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags) {
    bool c = igSliderFloat(label, v, ui_min, ui_max, fmt, ui_flags(flags));
    if (c) hold_f(v, hard_min, hard_max);
    return c;
}
bool ui_slider_int(const char* label, int* v, int ui_min, int ui_max, int hard_min, int hard_max, const char* fmt, int flags) {
    bool c = igSliderInt(label, v, ui_min, ui_max, fmt, ui_flags(flags));
    if (c) hold_i(v, hard_min, hard_max);
    return c;
}
bool ui_num_float_range(const char* label, float* a, float* b, float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt_a,
                        const char* fmt_b, int flags) {
    bool c = igDragFloatRange2(label, a, b, speed, ui_min, ui_max, fmt_a, fmt_b, ui_flags(flags));
    if (c) {
        hold_f(a, hard_min, hard_max), hold_f(b, hard_min, hard_max);
        if (*b < *a) *b = *a; // (a range: its end never before its start)
    }
    return c;
}
bool ui_num_float3(const char* label, float v[3], float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags) {
    bool c = igDragFloat3(label, v, speed, ui_min, ui_max, fmt, ui_flags(flags));
    if (c)
        for (int k = 0; k < 3; k++) hold_f(&v[k], hard_min, hard_max);
    return c;
}
