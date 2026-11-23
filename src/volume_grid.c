// (A9.5a: copied from the Tlender, ../tlender/src/volume_grid.c, adapted: the engine's zlib and files; the format unchanged)
// Sparse voxel grids (volume_grid.h).
#include "volume_grid.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int floor_div8(int v) { return v >= 0 ? v / VG_LEAF_DIM : -((-v + VG_LEAF_DIM - 1) / VG_LEAF_DIM); }

static uint32_t leaf_hash(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

void volume_grid_init(VolumeGrid* g, const char* name, int type, float voxel_size) {
    memset(g, 0, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", name ? name : "");
    g->type = type;
    g->grid_class = VG_CLASS_FOG;
    float vs = voxel_size > 0.0f ? voxel_size : 0.1f;
    g->xform = mat4_scale(v3(vs, vs, vs));
}

void volume_grid_free(VolumeGrid* g) {
    for (int i = 0; i < g->leaf_count; i++) free(g->leaves[i].values);
    free(g->leaves);
    free(g->hash);
    g->leaves = NULL;
    g->hash = NULL;
    g->leaf_count = g->leaf_cap = g->hash_cap = 0;
}

int volume_grid_components(const VolumeGrid* g) { return g->type == VG_VEC3 ? 3 : 1; }

static int find_leaf(const VolumeGrid* g, int ox, int oy, int oz) {
    if (!g->hash_cap) return -1;
    uint32_t mask = (uint32_t)g->hash_cap - 1;
    for (uint32_t h = leaf_hash(ox, oy, oz) & mask;; h = (h + 1) & mask) {
        int e = g->hash[h];
        if (!e) return -1;
        const VolumeLeaf* l = &g->leaves[e - 1];
        if (l->origin[0] == ox && l->origin[1] == oy && l->origin[2] == oz) return e - 1;
    }
}

static bool rehash(VolumeGrid* g, int cap) {
    int* h = calloc((size_t)cap, sizeof *h);
    if (!h) return false;
    uint32_t mask = (uint32_t)cap - 1;
    for (int i = 0; i < g->leaf_count; i++) {
        const VolumeLeaf* l = &g->leaves[i];
        uint32_t k = leaf_hash(l->origin[0], l->origin[1], l->origin[2]) & mask;
        while (h[k]) k = (k + 1) & mask;
        h[k] = i + 1;
    }
    free(g->hash);
    g->hash = h;
    g->hash_cap = cap;
    return true;
}

VolumeLeaf* volume_grid_leaf(const VolumeGrid* g, int i, int j, int k) {
    int li = find_leaf(g, floor_div8(i) * VG_LEAF_DIM, floor_div8(j) * VG_LEAF_DIM, floor_div8(k) * VG_LEAF_DIM);
    return li >= 0 ? &g->leaves[li] : NULL;
}

VolumeLeaf* volume_grid_touch_leaf(VolumeGrid* g, int i, int j, int k) {
    int ox = floor_div8(i) * VG_LEAF_DIM, oy = floor_div8(j) * VG_LEAF_DIM, oz = floor_div8(k) * VG_LEAF_DIM;
    int li = find_leaf(g, ox, oy, oz);
    if (li >= 0) return &g->leaves[li];
    if ((g->leaf_count + 1) * 2 > g->hash_cap && !rehash(g, g->hash_cap ? g->hash_cap * 2 : 64)) return NULL;
    if (g->leaf_count == g->leaf_cap) {
        int cap = g->leaf_cap ? g->leaf_cap * 2 : 32;
        VolumeLeaf* nl = realloc(g->leaves, (size_t)cap * sizeof *nl);
        if (!nl) return NULL;
        g->leaves = nl;
        g->leaf_cap = cap;
    }
    int nc = volume_grid_components(g);
    float* vals = malloc((size_t)VG_LEAF_VOXELS * (size_t)nc * sizeof *vals);
    if (!vals) return NULL;
    for (int v = 0; v < VG_LEAF_VOXELS; v++)
        for (int c = 0; c < nc; c++) vals[v * nc + c] = g->background[c];
    VolumeLeaf* l = &g->leaves[g->leaf_count];
    memset(l, 0, sizeof *l);
    l->origin[0] = ox;
    l->origin[1] = oy;
    l->origin[2] = oz;
    l->values = vals;
    uint32_t mask = (uint32_t)g->hash_cap - 1;
    uint32_t h = leaf_hash(ox, oy, oz) & mask;
    while (g->hash[h]) h = (h + 1) & mask;
    g->hash[h] = ++g->leaf_count;
    return l;
}

static int voxel_offset(const VolumeLeaf* l, int i, int j, int k) {
    return (i - l->origin[0]) + VG_LEAF_DIM * ((j - l->origin[1]) + VG_LEAF_DIM * (k - l->origin[2]));
}

bool volume_grid_set(VolumeGrid* g, int i, int j, int k, const float* value) {
    VolumeLeaf* l = volume_grid_touch_leaf(g, i, j, k);
    if (!l) return false;
    int nc = volume_grid_components(g), o = voxel_offset(l, i, j, k);
    for (int c = 0; c < nc; c++) l->values[o * nc + c] = value[c];
    l->active[o >> 6] |= 1ull << (o & 63);
    return true;
}

void volume_grid_get(const VolumeGrid* g, int i, int j, int k, float* out) {
    int nc = volume_grid_components(g);
    const VolumeLeaf* l = volume_grid_leaf(g, i, j, k);
    if (!l) {
        for (int c = 0; c < nc; c++) out[c] = g->background[c];
        return;
    }
    int o = voxel_offset(l, i, j, k);
    for (int c = 0; c < nc; c++) out[c] = l->values[o * nc + c];
}

void volume_grid_sample(const VolumeGrid* g, vec3 world, float* out) {
    int nc = volume_grid_components(g);
    mat4 inv;
    if (!mat4_inverse(g->xform, &inv)) {
        for (int c = 0; c < nc; c++) out[c] = g->background[c];
        return;
    }
    vec3 p = mat4_mul_point(inv, world);
    float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    int x = (int)fx, y = (int)fy, z = (int)fz;
    for (int c = 0; c < nc; c++) out[c] = 0.0f;
    for (int n = 0; n < 8; n++) {
        int dx = n & 1, dy = (n >> 1) & 1, dz = (n >> 2) & 1;
        float w = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty) * (dz ? tz : 1.0f - tz);
        if (w == 0.0f) continue;
        float v[3];
        volume_grid_get(g, x + dx, y + dy, z + dz, v);
        for (int c = 0; c < nc; c++) out[c] += w * v[c];
    }
}

bool volume_grid_index_bounds(const VolumeGrid* g, int lo[3], int hi[3]) {
    bool any = false;
    for (int i = 0; i < g->leaf_count; i++) {
        const VolumeLeaf* l = &g->leaves[i];
        bool act = false;
        for (int w = 0; w < VG_LEAF_VOXELS / 64; w++) act |= l->active[w] != 0;
        if (!act) continue;
        for (int a = 0; a < 3; a++) {
            if (!any || l->origin[a] < lo[a]) lo[a] = l->origin[a];
            if (!any || l->origin[a] + VG_LEAF_DIM - 1 > hi[a]) hi[a] = l->origin[a] + VG_LEAF_DIM - 1;
        }
        any = true;
    }
    return any;
}

bool volume_grid_world_bounds(const VolumeGrid* g, vec3* lo, vec3* hi) {
    int a[3], b[3];
    if (!volume_grid_index_bounds(g, a, b)) return false;
    for (int n = 0; n < 8; n++) {
        vec3 c = v3((float)((n & 1) ? b[0] : a[0]), (float)((n & 2) ? b[1] : a[1]), (float)((n & 4) ? b[2] : a[2]));
        vec3 w = mat4_mul_point(g->xform, c);
        if (!n) *lo = *hi = w;
        lo->x = fminf(lo->x, w.x), lo->y = fminf(lo->y, w.y), lo->z = fminf(lo->z, w.z);
        hi->x = fmaxf(hi->x, w.x), hi->y = fmaxf(hi->y, w.y), hi->z = fmaxf(hi->z, w.z);
    }
    return true;
}

size_t volume_grid_memory(const VolumeGrid* g) {
    return (size_t)g->leaf_cap * sizeof(VolumeLeaf) + (size_t)g->hash_cap * sizeof(int) +
           (size_t)g->leaf_count * VG_LEAF_VOXELS * (size_t)volume_grid_components(g) * sizeof(float);
}

float volume_grid_voxel_size(const VolumeGrid* g) {
    const float* m = g->xform.m;
    return sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
}

bool volume_grid_copy(VolumeGrid* dst, const VolumeGrid* src) {
    VolumeGrid g = *src;
    g.leaves = NULL;
    g.hash = NULL;
    g.leaf_count = g.leaf_cap = g.hash_cap = 0;
    int nc = volume_grid_components(src);
    if (src->leaf_count) {
        g.leaves = malloc((size_t)src->leaf_count * sizeof *g.leaves);
        g.hash = malloc((size_t)src->hash_cap * sizeof *g.hash);
        if (!g.leaves || !g.hash) {
            free(g.leaves);
            free(g.hash);
            return false;
        }
        g.leaf_cap = src->leaf_count;
        g.hash_cap = src->hash_cap;
        memcpy(g.hash, src->hash, (size_t)src->hash_cap * sizeof *g.hash);
        for (int i = 0; i < src->leaf_count; i++) {
            g.leaves[i] = src->leaves[i];
            size_t bytes = (size_t)VG_LEAF_VOXELS * (size_t)nc * sizeof(float);
            g.leaves[i].values = malloc(bytes);
            if (!g.leaves[i].values) {
                g.leaf_count = i;
                volume_grid_free(&g);
                return false;
            }
            memcpy(g.leaves[i].values, src->leaves[i].values, bytes);
        }
        g.leaf_count = src->leaf_count;
    }
    *dst = g;
    return true;
}

void volume_set_free(VolumeGridSet* set) {
    for (int i = 0; i < set->count; i++) volume_grid_free(&set->grids[i]);
    free(set->grids);
    set->grids = NULL;
    set->count = set->cap = 0;
}

VolumeGrid* volume_set_add(VolumeGridSet* set, const char* name, int type, float voxel_size) {
    if (set->count == set->cap) {
        int cap = set->cap ? set->cap * 2 : 4;
        VolumeGrid* n = realloc(set->grids, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        set->grids = n;
        set->cap = cap;
    }
    VolumeGrid* g = &set->grids[set->count++];
    volume_grid_init(g, name, type, voxel_size);
    return g;
}

VolumeGrid* volume_set_find(const VolumeGridSet* set, const char* name) {
    for (int i = 0; i < set->count; i++)
        if (strcmp(set->grids[i].name, name) == 0) return &set->grids[i];
    return NULL;
}
