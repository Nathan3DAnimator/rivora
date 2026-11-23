// (Q11) The Toon and Outline sections of the Material Editor's Details and of a Material Instance (notas-dev/toon.md)
#pragma once
#include <stdbool.h>

#include "toon.h"

// The Toon section (when the material's Shading Model is Toon) and the Outline section; true: a value changed
bool toon_ui_draw(MatToon* t, bool toon_shading);
