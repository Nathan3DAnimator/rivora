// The heavy model's test (--scene heavy; notas-dev/render.md "Malhas pesadas"): one model of 2.1 M triangles written
// into Content/HeavyTest the first time (the version in the names: a change makes them again):
// - a displaced rock of 1.54 M triangles, opaque, a 4096 noise material with bump;
// - 300 k Masked cards around it (600 k triangles, two sided, a 4096 voronoi alpha): a hairdo's or a bush's layers.
// Both through mesh_build.c (welded, ordered, LODs), as an import of that size would be
#pragma once
#include <stdbool.h>

typedef struct HeavyAssets {
    char rock_mesh[128], cards_mesh[128]; // "/Content/HeavyTest/..." (render_mesh_id_of)
    char rock_mat[800], cards_mat[800];   // on disk (mat_load)
    char rock_tess_mat[800];              // (Q17) the rock's noise as a Displacement too, Enable Tessellation (--scene rocks_tess)
    double ms;                            // the making's time (0: already there)
    unsigned tris;
} HeavyAssets;
bool heavy_test_assets(const char* content_root, HeavyAssets* out);
