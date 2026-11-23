// The Assistant's pictures (assistant_draw.c): diagrams drawn by the editor from an article's ```diagram <kind>``` and
// ```blueprint``` blocks (blueprint, steps, keys, axes, layout, compare, bars <unit>)
#pragma once
#include <stdbool.h>

typedef void (*AsShowFn)(const char* label); // "Show me where" (a layout's panel clicked)
bool assistant_draw_diagram(const char* kind, const char* body, int id, AsShowFn show); // false: an unknown kind
