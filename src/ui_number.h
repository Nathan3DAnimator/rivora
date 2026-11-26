// (2026-11-25, item 20 of the user's list: "digitando um número muito alto, o sistema responde a esse número; os
// limites só no slider") The number fields as Unreal's UI Min/Max against Clamp Min/Max: dragging (or a slider's grab)
// stays in the UI range ui_min..ui_max, a value typed (Ctrl+click, a double-click on a drag) is kept as typed, held only
// by the hard limits hard_min..hard_max: what the engine can't do past (an array, an index, a division by zero, the
// memory). hard_min >= hard_max: no hard limit. True when the value changed.
#pragma once
#include <stdbool.h>

bool ui_num_float(const char* label, float* v, float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags);
bool ui_num_int(const char* label, int* v, float speed, int ui_min, int ui_max, int hard_min, int hard_max, const char* fmt, int flags);
bool ui_slider_float(const char* label, float* v, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags);
bool ui_slider_int(const char* label, int* v, int ui_min, int ui_max, int hard_min, int hard_max, const char* fmt, int flags);
// two values (a range: Volume Range, Loop Region) the same way
bool ui_num_float_range(const char* label, float* a, float* b, float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt_a,
                        const char* fmt_b, int flags);
// three (a module's vector)
bool ui_num_float3(const char* label, float v[3], float speed, float ui_min, float ui_max, float hard_min, float hard_max, const char* fmt, int flags);

#define UI_NO_HARD 0.0f, 0.0f // (no hard limit: hard_min == hard_max)
