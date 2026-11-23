// (Q16, notas-dev/em-jogo.md) Console variables: every render, quality, physics and audio setting by a name, as
// Unreal's (r.ScreenPercentage, r.GI.Probes, p.GravityZ, au.MasterVolume...), read and written in the game by the
// Blueprints (Set / Get Console Variable, Execute Console Command), the command line (--cvar name=value) and the tests.
// Where each one lives: the view's GiSettings and MSAA (the host binds its own: the game's view, the editor's), the
// project's settings (ProjectSettings: what the renderer, the physics and the mixer read each frame), the player's
// options (game_settings.h: the preset, the resolution scale, the volumes...).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "gi.h"

typedef enum CvarType { CVT_FLOAT = 0, CVT_INT, CVT_BOOL, CVT_VEC3 } CvarType;
typedef enum CvarWhere { CVW_GI = 0, CVW_MSAA, CVW_PROJECT, CVW_GUS } CvarWhere;
typedef struct Cvar {
    const char* name;  // "r.GI.Probes"
    uint8_t where, type;
    uint16_t off;      // the field's offset in GiSettings / ProjectSettings
    float lo, hi;      // its range (lo == hi: none)
    const char* field; // the C field's name (GiSettings / ProjectSettings) or the option's (gus_get): tools/ingame_sweep.py
    const char* help;  // English (the Blueprint node's list, the Assistant)
} Cvar;

// The host's view: its GiSettings and MSAA (the game: its RenderView's; the editor: its own). NULL: the r.* of the view fail
void cvar_bind_view(GiSettings* gi, int* msaa);
// A play in the editor: the view's settings and the project's as they are now, put back by cvar_end_play (what a
// Blueprint changed while playing never stays in the editor nor reaches the .rproject)
void cvar_begin_play(void);
void cvar_end_play(void);

int cvar_count(void);
const Cvar* cvar_at(int i);
const Cvar* cvar_find(const char* name); // case doesn't matter
// The value as a number (bools 0/1) and as text; false: no such variable (or its place isn't bound)
bool cvar_get(const char* name, float* value, char* text, size_t cap);
// Text when given ("75", "true", "Epic" for an option with choices), else value; clamped to its range. False: none
bool cvar_set(const char* name, float value, const char* text);
// A console line: "r.ScreenPercentage 75" sets, "r.ScreenPercentage" reads into out ("r.ScreenPercentage = 75"); false:
// unknown or empty
bool cvar_exec(const char* line, char* out, size_t cap);
