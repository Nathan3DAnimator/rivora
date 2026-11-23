// Sublevels and Lighting Levels (2026-11-16, "como na Unreal tem um sistema de sublevel no painel, vamos criar isso também,
// mas também tem a possibilidade de light-level ... na Unreal só pode ter 1 light level por vez, aqui vamos poder ter vários
// um por cima do outro"; notas-dev/niveis.md). A level (the persistent one) lists other levels as its sublevels in a text
// file beside it (<level>.rlevels): each loaded into the same world with its actors marked (CStreamed: its id), shown or
// hidden, saved back to its own .rlevel. A sublevel marked Lighting is a Lighting Level: its lights bake into its own
// .rlight (Build Lighting: one bake per lighting level, with only its lights), and the lightmaps the editor or the game
// shows are the persistent level's plus every visible lighting level's, added: the light is linear, so any mix of them on
// at once is exact (Unreal's Lighting Scenarios: one at a time).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

struct VkCtx;

#define SUBLEVEL_MAX 32
#define SUBLEVEL_ID0 1u // the sublevels' CStreamed ids: SUBLEVEL_ID0 + their slot (the Play's streaming uses 1001 and up)

typedef struct Sublevel {
    char name[96];   // the level's name (Content/**/<name>.rlevel: found by name, so a package finds it too)
    char path[700];  // where it was found (empty: missing)
    uint32_t id;     // its actors' CStreamed id (0: not loaded)
    bool visible;    // shown (the editor; the game: loaded visible)
    bool initially_loaded; // the game and the Play start with it in
    bool lighting;   // a Lighting Level: its lights bake to its own lightmaps, added to the others'
    bool locked;     // the editor doesn't select or move its actors
    bool dirty;      // changed since saved
    float weight;    // (2026-11-16) a Lighting Level's blend: its lights and baked light times this (Set Lighting Level Blend)
    int layer;       // its bake's layer on the GPU (gi_lightmaps_load_layers; -1: none)
    float color[3];  // the Levels panel's swatch (and the actors' outline)
} Sublevel;

typedef struct SublevelSet {
    Sublevel s[SUBLEVEL_MAX];
    uint32_t n;
    int current; // where new actors go: -1 the persistent level, else a sublevel's slot
} SublevelSet;

// <level>.rlevels: the list beside a level
void sublevels_list_path(const char* level_path, char* out, size_t cap);
bool sublevels_read(const char* level_path, SublevelSet* set);       // false: no list (an empty set)
bool sublevels_write(const char* level_path, const SublevelSet* set); // an empty set removes the file

// Its actors into w (its CStreamed id: SUBLEVEL_ID0 + slot), hidden in the editor when it isn't visible; false: no such
// level or a file this version can't read. Unload: its actors out.
bool sublevel_load(World* w, SublevelSet* set, uint32_t slot);
void sublevel_unload(World* w, SublevelSet* set, uint32_t slot);
// Shown or hidden in the editor (TRANSFORM_HIDDEN on its actors; the game hides with TRANSFORM_HIDDEN_GAME too)
void sublevel_set_visible(World* w, Sublevel* s, bool visible, bool game);
// The sublevel slot an actor belongs to (-1: the persistent level)
int sublevel_of(World* w, const SublevelSet* set, Entity e);
// Moves an actor (and what it owns: a Blueprint's components) to a sublevel slot (-1: the persistent level)
void sublevel_move_actor(World* w, const SublevelSet* set, Entity e, int slot);
// Its actors (they lose their mark) into its own file; the persistent level's save leaves every sublevel's actors out
bool sublevel_save(World* w, const Sublevel* s);
size_t sublevels_count_actors(World* w, const Sublevel* s);
struct LevelCamera;
bool level_save_without_sublevels(World* w, const struct LevelCamera* cam, const char* path, size_t* bytes);

// The baked lighting shown: the persistent level's .rlight plus every visible lighting level's, added (gi_bake.c)
bool sublevels_load_lighting(struct VkCtx* vk, const char* level_path, SublevelSet* set); // (each Lighting Level a GPU layer: s->layer)
// The lights a bake sees (2026-11-17: and the fog, clouds, fog volumes and Post Process Volumes): only the persistent level's and the non-lighting sublevels' (slot -1), or only one lighting
// level's (its slot): the others hidden for the bake (TRANSFORM_HIDDEN), their flags kept to put back
typedef struct LightMask { uint32_t n; Entity e[1024]; uint32_t flags[1024]; } LightMask;
void sublevels_lights_only(World* w, const SublevelSet* set, int lighting_slot, LightMask* saved);
void sublevels_lights_restore(World* w, const LightMask* saved);

// The game (game_main.c) and the Play: the initially loaded sublevels in, their lighting added; the names registered with
// the streaming so Load / Unload Stream Level and Set Lighting Level find them
void sublevels_begin_game(struct VkCtx* vk, World* w, const char* level_path, SublevelSet* set);
// The Blueprints' Set Lighting Level Visible: a lighting level's lights and its share of the baked light on or off (the
// lightmaps added again); false: no such lighting level
bool sublevels_set_lighting_visible(struct VkCtx* vk, World* w, const char* name, bool visible, bool game);
bool sublevels_lighting_visible(const char* name); // Is Lighting Level Visible
// (2026-11-16) Set Lighting Level Blend: a Lighting Level at a weight (0 off .. 1 as baked; more: brighter). Its lights and
// its baked light follow at once on the GPU (no file, no CPU sum): a timeline can fade it or make it flicker. A level not
// loaded comes in on a worker (its file read there; its actors in on the main thread a frame or two later). False: no
// such Lighting Level
bool sublevels_set_lighting_blend(struct VkCtx* vk, World* w, const char* name, float weight, bool game);
float sublevels_lighting_blend(const char* name); // its weight now (0: off or no such level)
void sublevels_apply_weight(World* w, Sublevel* s);  // its lights and its layer at its weight (the editor's slider)
// (2026-11-17) every level's weight again (an open, a removed level: its slot's weight back to 1), the lights touched
void sublevels_apply_all(World* w, SublevelSet* set);
void sublevels_bake_weights(World* w, const SublevelSet* set); // every level at 1 while Build Lighting runs
SublevelSet* sublevels_active(void); // the set the world was begun with (NULL: none)
void sublevels_set_active(SublevelSet* set, const char* level_path);
