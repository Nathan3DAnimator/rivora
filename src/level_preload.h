// (2026-11-23, the user: "in the lobby the next map loads little by little behind it, slowly so nothing hitches; when
// everyone presses Play it goes straight in if it is loaded, else a loading screen where the speed goes up; the
// background world doesn't play (no BeginPlay) until the switch, and the switch is clean, like a camera cut") Levels
// loaded in the background before they are opened, at a speed the game sets and changes while it loads:
//  - Preload Level: a thread reads the .rlevel in slices (paced by the speed) and builds its world apart (ECS rows, no
//    BeginPlay, nothing drawn), reads its lightmaps into the OS cache; then the main thread builds its Blueprints'
//    component trees a few per frame, inside a budget the speed sets (0.02: ~0.1 ms a frame, 1: ~8 ms).
//  - Open Level of a preloaded level: the world prepared swaps in at once (a pointer swap, then the BeginPlays): no
//    file read, no parse. Not finished yet: the host shows the loading screen and finishes it at full speed.
//  - The speed (0..1): per level (Set Level Load Speed) or every background load (Set Async Loading Speed: the
//    preloads, Load Stream Level's reads). 1 is the old behavior (as fast as possible).
// notas-dev/niveis.md "Pré-carregar níveis".
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "level.h"

#define PRELOAD_MAX 4

typedef enum PreloadState { PRE_NONE = 0, PRE_READING, PRE_BUILDING, PRE_READY, PRE_FAILED } PreloadState;

// Starts preloading a level (a name of the project's or a path); speed 0..1 (< 0: the global one). Already
// preloading: only its speed changes. False: no such level, or no room (PRELOAD_MAX).
bool level_preload(const char* name, float speed);
void level_preload_set_speed(const char* name, float speed); // NULL / "": every preload
void level_async_set_speed(float speed);                     // the global speed (every background load; 1 the default)
float level_async_speed(void);
// Its state and progress 0..1 (PRE_NONE / 0 when it isn't preloading)
PreloadState level_preload_state(const char* name, float* progress);
void level_preload_cancel(const char* name); // NULL: all
// Each frame on the main thread (the hosts' loops): the component trees within the budget
void level_preload_update(void);
// Open Level: a preloaded level's world into w (w's old contents freed). Not ready yet: finished here at full speed,
// pump (may be NULL) called between steps (the loading screen's frame, progress 0..1). False: it wasn't preloaded
// (the host reads the file as before). ms: the swap's time.
bool level_preload_take(const char* path, World* w, LevelCamera* cam, void (*pump)(float progress), double* ms);
// Lightmaps the preload read (the .rlight's bytes kept warm in memory, freed by the take): for gi
bool level_preload_path(const char* name, char* out, unsigned cap);

typedef struct PreloadStats {
    uint32_t started, taken, cancelled;
    double last_read_ms, last_build_ms, last_take_ms; // the last one's: reading + parsing (thread), the trees (main), the swap
    double frame_ms_max;                             // the most a frame spent on the preloads (main thread)
    uint32_t last_actors;
} PreloadStats;
const PreloadStats* level_preload_stats(void);
bool level_preload_selftest(const char* dir); // (--preload-test <folder>)
