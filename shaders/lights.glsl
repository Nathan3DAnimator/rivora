// The local lights (point, spot, rect): every one in a buffer on the GPU (lights.c, a slot per entity, type ~0u when
// empty) and a world grid of them around the camera, rebuilt each frame by light_grid.comp (notas-dev/luz.md). The
// directional lights stay in FrameBuf.lights. Included after common.glsl.
#ifndef LIGHTS_GLSL
#define LIGHTS_GLSL

// A clipmap of LG_LEVELS grids centered on the camera, each LG_DIM_XY x LG_DIM_XY x LG_DIM_Z cells; level L's cells
// are lg_params.x * 4^L cm (4 m, 16 m, 64 m: 256 m, 1 km and 4 km across)
#define LG_LEVELS 3u
#define LG_DIM_XY 64
#define LG_DIM_Z 16
#define LG_LEVEL_CELLS 65536u
#define LG_SLOT_MASK 0x3FFFFFFFu // an item: the light's slot | its home level << 30

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LightBuf { GpuLight l[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer CellBuf { uvec2 c[]; }; // per cell: offset, count

float lg_cell_size(FrameBuf f, uint L) { return f.lg_params.x * float(1u << (2u * L)); }

// p's cell in level L (its index in the level), -1 outside it
int lg_cell(FrameBuf f, vec3 p, uint L) {
    ivec3 c = ivec3(floor(p / lg_cell_size(f, L))) - f.lg_origin[L].xyz;
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(LG_DIM_XY, LG_DIM_XY, LG_DIM_Z)))) return -1;
    return (c.z * LG_DIM_XY + c.y) * LG_DIM_XY + c.x;
}

// A light's home level: the finest whose cells it spans at most ~17 of per axis (light_grid.comp inserts it there and
// in the coarser levels, where its range is still worth a cell)
uint lg_home(FrameBuf f, float range) {
    for (uint L = 0u; L < LG_LEVELS - 1u; L++)
        if (range <= lg_cell_size(f, L) * 8.0) return L;
    return LG_LEVELS - 1u;
}

// The lists that light p: at the finest level holding it (lp) every light homed there or finer; at the coarser ones
// only the lights homed there (too big for the finer cells). lp == LG_LEVELS: outside the grid, no local light.
// (No vector or array indexed by a variable here or in the callers: NVIDIA puts those in local memory, and with them the
// driver's background shader compiler crashed far more often: notas-dev/escala.md. The levels are unrolled by hand.)
struct LightLists {
    uint lp;
    uvec2 r0, r1, r2; // per level: the list's offset and count
};
uvec2 lg_list(FrameBuf f, vec3 p, uint L, uint homes, inout uint lp) {
    if (lp != LG_LEVELS && (homes & (1u << L)) == 0u) return uvec2(0u); // coarser: only its own lights, if any
    int c = lg_cell(f, p, L);
    if (c < 0) return uvec2(0u);
    if (lp == LG_LEVELS) lp = L;
    return CellBuf(f.lg_cells).c[L * LG_LEVEL_CELLS + uint(c)];
}
LightLists light_lists(FrameBuf f, vec3 p) {
    LightLists r;
    r.lp = LG_LEVELS;
    r.r0 = r.r1 = r.r2 = uvec2(0u);
    if (f.local_lights == 0u) return r;
    uint homes = UintBuf(f.lg_cursor).v[LG_LEVELS * LG_LEVEL_CELLS + 1u]; // the levels some light calls home
    r.r0 = lg_list(f, p, 0u, homes, r.lp);
    r.r1 = lg_list(f, p, 1u, homes, r.lp);
    r.r2 = lg_list(f, p, 2u, homes, r.lp);
    return r;
}
// Level L's list (offset, count)
uvec2 light_range(LightLists ll, uint L) { return L == 0u ? ll.r0 : L == 1u ? ll.r1 : ll.r2; }
// Whether an item found in level L's list is one of p's (each light counted once)
bool light_take(uint item, uint L, uint lp) {
    uint home = item >> 30;
    return L == lp ? home <= lp : home == L;
}
uint light_item(FrameBuf f, uvec2 range, uint k) { return UintBuf(f.lg_items).v[range.x + k]; }
GpuLight local_light(FrameBuf f, uint item) { return LightBuf(f.light_buf).l[item & LG_SLOT_MASK]; }

// How far a light reaches: its range, from a rectangle's surface (as far again as its corners)
float light_reach(GpuLight L) { return L.range + (light_kind(L) == LIGHT_RECT ? length(vec2(L.half_w, L.half_h)) : 0.0); }

#endif
