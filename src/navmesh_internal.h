// The NavMesh's tile builder (navmesh_build.c): pure, any thread. Its input is copied on the main thread (navmesh.c),
// its output one block of memory the main thread puts in place.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NAV_NVP 6           // a polygon's most corners (Recast's default)
#define NAV_PORTAL 0x8000u  // a polygon edge on the tile's side (| dir: 0 -x, 1 +y, 2 +x, 3 -y)
#define NAV_NO_LINK 0xFFFFFFFFu

typedef struct NavCfg {
    float cs, ch;           // the cell's size and height (cm)
    float radius, height, climb, slope_deg; // the agent's
    int tile;               // a tile's side in cells
    int border;             // cells around it built too (its edges match the neighbors')
    float max_error;        // contours: cells a simplified edge may leave the raw one
    int max_edge;           // contours: the longest wall edge (cells)
    int min_region;         // an island of fewer cells (not on the tile's side) is dropped
    int detail_step;        // cells between two height samples of a polygon
} NavCfg;

typedef struct NavInBox { float mn[3], mx[3]; } NavInBox;               // world (x, y, z up)
typedef struct NavInArea { float mn[3], mx[3]; uint8_t area; } NavInArea; // a Nav Modifier Volume

typedef struct NavTileInput {
    int tx, ty;
    NavCfg cfg;
    float bmin[3], bmax[3]; // the tile with its border; z from the content
    NavInBox* boxes;
    uint32_t nboxes;
    NavInArea* areas;
    uint32_t nareas;
    float* land;            // the Landscape's heights (lw x lh from bmin, lstep apart; NaN: none)
    int lw, lh;
    float lstep;
} NavTileInput;

typedef struct NavPoly {
    uint16_t v[NAV_NVP];    // its corners (the tile's vertices)
    uint16_t nei[NAV_NVP];  // across each edge: 0 a wall, a polygon of the tile + 1, or NAV_PORTAL | dir
    uint8_t nv, area;
    uint8_t dw, dh;         // its height grid (the detail): dw x dh samples
    uint32_t detail;        // the first sample (NavTileData.detail)
    float dx0, dy0;         // the grid's corner (world)
    uint32_t first_link;    // (the run's: navmesh.c)
} NavPoly;

typedef struct NavTileData {
    int tx, ty;
    uint32_t nverts, npolys, ndetail;
    float bmin[3], bmax[3]; // the tile (without its border), world
    float dstep;            // the detail's spacing (cm)
    float* verts;           // world x, y, z
    NavPoly* polys;
    float* pbox;            // per polygon: min x, y, z, max x, y, z
    float* detail;          // heights (world z)
    double ms;              // the build's
    uint32_t spans, regions, contours;
} NavTileData;

// Builds one tile (NULL: nothing walkable in it). free() the result.
NavTileData* nav_build_tile(const NavTileInput* in);
// The height of the polygon's surface at (x, y) (its detail grid)
float nav_poly_height(const NavTileData* t, const NavPoly* p, float x, float y);
