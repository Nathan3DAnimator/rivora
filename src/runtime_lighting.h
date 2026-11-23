// (Q16, notas-dev/em-jogo.md) Build Lighting in the game: a level made while playing (procgen, PCG, a Blueprint's
// Spawn Actor) gets the editor's own bake (gi_bake.c: lightmaps, the baked probe volume) by a Blueprint node, in one of
// two modes: behind a loading screen (the game waits, the bake as fast as the GPU goes) or while it plays (a band of the
// atlas a frame in the frame's budget, the dynamic GI lighting the level until the lightmaps are ready). The hosts (the
// game, the editor's Play) call rtl_frame each frame before the render; the Blueprints ask through rtl_request.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ecs.h"

struct VkCtx;

typedef enum RtlMode { RTL_LOADING_SCREEN = 0, RTL_PROGRESSIVE = 1 } RtlMode;

// Asks for a bake of the world's Static meshes (quality: 0 Preview, 1 Production; budget_ms: the GPU's share a frame,
// 0: the mode's own, 30 ms behind the loading screen, 1 ms while playing). Started by the next rtl_frame. False: no
// ray tracing on this GPU, or a bake already under way (why says)
bool rtl_request(int quality, RtlMode mode, float budget_ms);
void rtl_cancel(void);
bool rtl_busy(void);       // asked or baking
bool rtl_blocking(void);   // a loading screen's bake under way: the game waits behind it (rtl_draw_loading)
float rtl_progress(void);  // 0..1 (1 once built)
bool rtl_built(void);      // the last bake asked by the game ended (its lightmaps shown)
double rtl_last_seconds(void); // how long it took
const char* rtl_status(void);  // the last start's failure, "" when none
// The host's frame, before the render: starts what was asked, notices the end (Event On Lighting Built, the latent
// Build Lighting (Async)'s Completed); the bake itself runs inside the render (gi_bake_step)
void rtl_frame(struct VkCtx* vk, World* w);
// The game's bake saved / read back by a name (Saved/SaveGames/<name>.rlight): a procedural map made again from the
// same seed takes its lighting back without baking (the meshes found by their transforms)
bool rtl_save(const char* name); // (the host's VkCtx: the last rtl_frame's)
bool rtl_load(const char* name);
// The end of a play (the editor's): a bake still running stops; the level's own lighting comes back when the game
// changed it (rlight: the level's .rlight, "" or NULL: none)
void rtl_end_play(struct VkCtx* vk, const char* rlight);
bool rtl_owns_bake(void);  // the bake under way (or just ended) is the game's: the editor doesn't save it as the level's
