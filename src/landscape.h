// Landscape (notas-dev/landscape.md): Unreal's Landscape, simpler. A Landscape actor (CLandscape) is a height grid
// (its .rland in Content/Landscapes) drawn as chunk meshes (real .rmesh: culled, shadowed, ray traced and lit by the
// GI like any mesh) and as the ground the Characters walk on, the AI's navigation and the traces see.
//   * made by noise (fBm with ridges: hills to mountains), then sculpted in the viewport (Raise, Lower, Smooth,
//     Flatten; a stroke rewrites only the chunks it touched when the mouse lets go)
//   * or from a heightmap image (Unreal's Import from File: 16-bit PNG / TIFF, .r16 / .raw), as the heights or as a
//     displacement over them
//   * the plants on it are a Foliage actor's (foliage.h, the Foliage mode), apart from the ground's mesh
// The chunks are its child entities (CAttach kind ATTACH_LANDSCAPE), made again from it.
#define LANDSCAPE_PLANT_NODE 0x10000u // an older level's plant (CAttach.node: this + its index); a chunk's: its index
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

struct VkCtx;

enum { LAND_TOOL_NONE = 0, LAND_TOOL_RAISE, LAND_TOOL_LOWER, LAND_TOOL_SMOOTH, LAND_TOOL_FLATTEN, LAND_TOOL_PAINT, LAND_TOOL_ERASE, LAND_TOOL_COUNT };
// (B11) the layer Paint / Erase works on (1-3: its material's Landscape Layers, the vertex color's r, g, b)
void landscape_set_paint_layer(int layer);
// (B11) a stroke undone (the heights and the painted layers as before it; 8 kept); false: nothing to undo
bool landscape_undo(World* w, Entity e);
// (B11) the heights into a 16-bit .r16 (Unreal's raw heightmap: the lowest point 0, the highest 65535); its range (cm)
bool landscape_export_heightmap(Entity e, const char* path, float* lo, float* hi);
// (B11) a layer's share at a point (0..1), for the tests
float landscape_layer_weight(Entity e, int layer, float x, float y);
// (Q15) the same on whichever landscape is under (x, y) (the PCG's Landscape Layer Filter); 0: none there
float landscape_layer_at(float x, float y, int layer);

void landscape_init(const char* content_root);
// Every frame (editor and game): the heights loaded or made, the chunks and plants as the component says, the chunk
// meshes written again after an edit (the renderer's meshes then read again: vk)
void landscape_update(World* w, struct VkCtx* vk);
// The ground: the height under (x, y) of the landscapes (false: none there); a segment's first hit (0-1, 1: none)
bool landscape_height(float x, float y, float* z);
float landscape_trace(const float a[3], const float b[3]);
// (2026-11-19) The scene queries' (scene_query.c): the segment a..b lowered by lift (a sweep's bottom) against the
// landscapes' triangles exactly, tile by tile (each tile's lowest and highest heights skip it whole); its first t (0-1),
// the triangle's normal, the landscape's actor. A Procedural World's ground is marched as landscape_trace does. Thread
// safe once landscape_query_prepare ran this frame (the main thread)
bool landscape_raycast(const float a[3], const float b[3], float lift, float* t, float n[3], Entity* actor);
void landscape_query_prepare(void);
bool landscape_bounds(float mn[2], float mx[2]); // the landscapes' extent (the navigation grid's)
uint32_t landscape_generation(void); // (Q21) changes whenever a landscape's heights may have (farview.c)
// Editing (the editor): a brush at a point (cm, world), its radius (cm) and strength (cm/s); a stroke's end commits
void landscape_brush(World* w, Entity e, int tool, const float at[3], float radius, float strength, float dt);
void landscape_stroke_end(World* w, Entity e);
void landscape_regenerate(World* w, Entity e); // the noise again (the component's height, roughness, seed)
// The Details' line
typedef struct LandStats {
    uint32_t landscapes, chunks, triangles;
    double build_ms;
    char import_msg[160]; // the last heightmap's ("!path": it could not be read)
} LandStats;
// A heightmap onto the landscape, applied when its grid is ready (after a change of resolution): black at the actor,
// white at its Height; add: a displacement over its heights (mid gray: none). The image's size (false: unreadable)
void landscape_import_heightmap(Entity e, const char* path, bool add);
bool landscape_heightmap_size(const char* path, uint32_t* w, uint32_t* h);
const LandStats* landscape_stats(void);
