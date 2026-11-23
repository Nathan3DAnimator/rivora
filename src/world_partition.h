// World Partition (B1, notas-dev/mundos-grandes.md): Unreal's grid streaming, Data Layers and HLOD for big levels.
// A level with a World Partition actor (CWorldPartition, one per level, like Unreal's World Settings) is split while it
// plays: every spatially loaded actor (Details > World Partition > Is Spatially Loaded; Auto: the props, local lights,
// Blueprints that aren't the player's...) belongs to the grid cell under it; the cells within the Loading Range of the
// streaming source (the camera) are in the world, the others' actors are kept as bytes and made again when the player
// nears (as the level had them: Unreal's reset). The loads and unloads are spread over the frames (Streaming Budget, ms
// a frame, nearest first) so walking never hitches.
//   - Data Layers: named sets of actors (an actor in any of them); each Unloaded (out), Loaded (in, hidden) or Activated
//     (in, shown), set by the level (Initial State) and by Blueprints (Set Data Layer Runtime State).
//   - HLOD: what is past the Loading Range is still seen. Near: the unloaded cells' big meshes as proxies (Unreal's
//     Instancing HLOD: the same mesh and material, no Blueprint, no collision, no tick); far: each region of N x N cells
//     as one mesh merged and simplified with meshoptimizer, its materials baked into vertex colors (Unreal's Merged /
//     Simplified Mesh), made by Build > Build HLODs into Content/HLOD/<level>/.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

enum { WP_LAYER_UNLOADED = 0, WP_LAYER_LOADED, WP_LAYER_ACTIVATED, WP_LAYER_STATE_COUNT };
extern const char* const g_wp_layer_state_names[WP_LAYER_STATE_COUNT];
enum { WP_SPATIAL_AUTO = 0, WP_SPATIAL_YES, WP_SPATIAL_NO, WP_SPATIAL_COUNT };
extern const char* const g_wp_spatial_names[WP_SPATIAL_COUNT];

Entity wp_settings(World* w);               // the level's World Partition actor (ENTITY_NONE: none)
bool wp_actor_spatial(World* w, Entity e);  // its Is Spatially Loaded (Auto resolved)
uint32_t wp_actor_layers(World* w, Entity e); // its Data Layers (a bit each)
void wp_cell_of(float cell_size, const float p[3], int32_t* cx, int32_t* cy);

// A play (PIE or the game): begin before the BeginPlays (the actors out of range leave before theirs), update every
// frame (focus: the streaming source, cm), end when it stops (the editor's snapshot brings everything back)
void wp_begin(World* w, const char* level_path, const char* content_root, const float focus[3]);
void wp_update(World* w, const float focus[3], bool playing);
void wp_end(World* w);
bool wp_active(void);

// The Blueprints' (Set / Get Data Layer Runtime State, Is Location Loaded)
bool wp_set_layer_state(const char* name, int state);
int wp_get_layer_state(const char* name); // -1: no such layer
bool wp_location_loaded(const float p[3]); // its cell in the world (true without a World Partition)

typedef struct WpStats {
    uint32_t cells, cells_in, records, records_in, layered;
    uint32_t proxies, proxies_shown, regions, regions_shown;
    uint32_t loads, unloads, queued;
    double ms, ms_peak, begin_ms; // the update's main-thread time (this frame, the worst), wp_begin's
    // (Q27.7) the focus's speed (m/s), the cells wanted ahead of it now, records waiting to load well inside the range
    // (this frame, the most, the frames with any, of the frames played)
    float speed;
    uint32_t ahead, missing, missing_peak, missing_frames, frames;
} WpStats;
#define WP_PREDICT_S 2.0f // (Q27.7) the seconds of the focus's path streamed ahead
const WpStats* wp_stats(void);

// Build > Build HLODs: each region's merged mesh (Content/HLOD/<level>/r_<x>_<y>.rmesh); the meshes reload after it.
// msg: what was made. False: no World Partition, nothing to merge or unwritten
bool wp_build_hlods(World* w, const char* content_root, const char* level_name, char* msg, size_t cap);
void wp_hlod_dir(const char* content_root, const char* level_name, char* out, size_t cap);
void wp_level_name(const char* level_path, char* out, size_t cap); // its file's name without the extension ("Untitled": none)

// The editor: a Data Layer shown or hidden in the viewport (its actors' Outliner eye)
void wp_editor_layer_visible(World* w, int layer, bool visible);
void wp_editor_layer_removed(World* w, int layer); // (the actors' bits after it move down)

// Window > World Partition's map: the cells while playing (their place, in or out), NULL when not playing
typedef struct WpCellView { int32_t cx, cy; uint32_t actors; bool in, region_shown; } WpCellView;
uint32_t wp_cells_view(const WpCellView** out);
