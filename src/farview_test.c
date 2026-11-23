// (Q21) The distant view's scenes and checks (notas-dev/visao-distante.md). The assets are made under Content/Vista the
// first time (a tree of leaf cards with its bark and leaf textures, a rock):
//   --scene vista: a lookout over 8 x 8 km of mountains at a low sun (a Landscape of 1024 quads, ~25 000 trees of a
//     Foliage actor, 3000 rocks in a World Partition with its HLOD, a height fog): the camera on the highest point of a
//     corner looking across. Checked: the far shadow (valleys shaded by ridges kilometers away, on the CPU's twin of
//     the GPU's march), impostors baked and drawn, the cost (--min-fps)
//   --scene vista_pop: the same view, still: two shots, the impostors switched at their screen size and at 2.5 times it
//     (the trees in between drawn by their mesh, then by their card); the pictures compared: the switch's "pop"
//   --scene vista_walk: the camera flying low over the forest (--cam-move): the flicker probe while the trees cross the
//     switch (--max-flicker)
#include "farview_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "farview.h"
#include "foliage.h"
#include "landscape.h"
#include "material.h"
#include "mesh_build.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"
#include "world_partition.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

static struct {
    int scene; // 1 vista, 2 pop, 3 walk
    bool placed, ready;
    uint64_t frames_ready;
    int pop_phase;
    char shot[3][700];
    uint32_t trees, rocks;
    double t_ready, t0;
} VT;

static void vpath(char* out, size_t cap, const char* sub) { snprintf(out, cap, "%s/Vista/%s", mat_root(), sub); }

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return x;
}
static float rnd(uint32_t* s) { *s = hash32(*s + 0x9e3779b9u); return (float)(*s >> 8) / 16777216.0f; }

// The tree's textures: 512 x 512, the left eighth bark (brown streaks, opaque), the rest leaf clusters (round blobs of
// greens, cut by the mask's white)
static bool make_textures(void) {
    char pc[900], pm[900];
    vpath(pc, sizeof pc, "T_Vista_Tree.png"), vpath(pm, sizeof pm, "T_Vista_TreeMask.png");
    if (re_file_stat(pc, NULL, NULL, NULL) && re_file_stat(pm, NULL, NULL, NULL)) return true;
    const uint32_t N = 512;
    uint8_t* c = calloc((size_t)N * N * 3, 1);
    uint8_t* m = calloc((size_t)N * N * 3, 1);
    if (!c || !m) return free(c), free(m), false;
    for (uint32_t y = 0; y < N; y++)
        for (uint32_t x = 0; x < N / 8; x++) {
            uint32_t h = hash32(x * 7u + (y / 6u) * 131u);
            float v = 0.75f + 0.25f * (float)(h & 255u) / 255.0f;
            uint8_t* p = &c[(y * N + x) * 3];
            p[0] = (uint8_t)(92.0f * v), p[1] = (uint8_t)(64.0f * v), p[2] = (uint8_t)(42.0f * v);
            m[(y * N + x) * 3] = m[(y * N + x) * 3 + 1] = m[(y * N + x) * 3 + 2] = 255;
        }
    uint32_t s = 77u;
    for (int k = 0; k < 900; k++) { // (needle clusters)
        float cx = (float)N / 8.0f + 6.0f + rnd(&s) * ((float)N * 7.0f / 8.0f - 12.0f), cy = 6.0f + rnd(&s) * ((float)N - 12.0f);
        float r = 5.0f + rnd(&s) * 9.0f, g = 0.6f + 0.4f * rnd(&s);
        for (int yy = (int)(cy - r); yy <= (int)(cy + r); yy++)
            for (int xx = (int)(cx - r); xx <= (int)(cx + r); xx++) {
                if (xx < (int)N / 8 + 1 || xx >= (int)N || yy < 0 || yy >= (int)N) continue;
                float dx = (float)xx - cx, dy = (float)yy - cy;
                if (dx * dx + dy * dy > r * r) continue;
                uint8_t* p = &c[((uint32_t)yy * N + (uint32_t)xx) * 3];
                float sh = 0.7f + 0.3f * (1.0f - sqrtf(dx * dx + dy * dy) / r);
                p[0] = (uint8_t)(30.0f * g * sh), p[1] = (uint8_t)(78.0f * g * sh), p[2] = (uint8_t)(34.0f * g * sh);
                uint8_t* q = &m[((uint32_t)yy * N + (uint32_t)xx) * 3];
                q[0] = q[1] = q[2] = 255;
            }
    }
    char dir[900];
    vpath(dir, sizeof dir, "");
    re_make_dirs(dir);
    bool ok = vkctx_png_write_rgb(pc, c, N, N) && vkctx_png_write_rgb(pm, m, N, N);
    free(c), free(m);
    return ok;
}

static int sock(const MatNodeDef* d, const char* name, bool out) {
    int n = out ? d->nout : d->nin;
    for (int i = 0; i < n; i++)
        if (!strcmp(out ? d->out[i].name : d->in[i].name, name)) return i;
    return -1;
}
static int add(MatGraph* g, const char* idname, float x, float y) {
    const MatNodeDef* d = mat_node_def_by_idname(idname);
    return d ? mat_graph_add(g, d->type, x, y) : -1;
}
static void link(MatGraph* g, int from, const char* out, int to, const char* in) {
    if (from < 0 || to < 0) return;
    int a = sock(mat_node_def(g->nodes[from].type), out, true), b = sock(mat_node_def(g->nodes[to].type), in, false);
    if (a >= 0 && b >= 0) mat_graph_link(g, from, a, to, b);
}

// The tree's material: Masked and Two Sided, the color texture into the Base Color, the mask into the Alpha
static uint32_t make_material(void) {
    char p[900];
    vpath(p, sizeof p, "M_Vista_Tree.rmat");
    Material* m = re_file_stat(p, NULL, NULL, NULL) ? mat_load(p) : NULL;
    if (m) return m->hash;
    m = mat_create(p, false, NULL);
    if (!m) return 0;
    MatGraph* g = m->graph;
    int bsdf = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
    int ic = add(g, "ShaderNodeTexImage", -520, 100), im = add(g, "ShaderNodeTexImage", -520, -220);
    if (ic >= 0) snprintf(g->nodes[ic].image, sizeof g->nodes[ic].image, "/Content/Vista/T_Vista_Tree.png");
    if (im >= 0) snprintf(g->nodes[im].image, sizeof g->nodes[im].image, "/Content/Vista/T_Vista_TreeMask.png"), g->nodes[im].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
    link(g, ic, "Color", bsdf, "Base Color");
    link(g, im, "Color", bsdf, "Alpha");
    if (bsdf >= 0) g->nodes[bsdf].in[sock(mat_node_def(NODE_BSDF_PRINCIPLED), "Roughness", false)][0] = 0.7f;
    m->blend_mode = MAT_BLEND_MASKED, m->two_sided = true, m->opacity_clip = 0.5f;
    m->resolution = 1024;
    mat_save(m);
    mat_mark_dirty(m);
    return m->hash;
}

static void vtx(GpuVertex* g, const float p[3], const float n[3], float u, float v) {
    memset(g, 0, sizeof *g);
    memcpy(g->pos, p, sizeof g->pos);
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float nn[3] = { n[0] / l, n[1] / l, n[2] / l };
    float s = fabsf(nn[0]) + fabsf(nn[1]) + fabsf(nn[2]);
    float ex = nn[0] / s, ey = nn[1] / s;
    if (nn[2] < 0.0f) {
        float tx = (1.0f - fabsf(ey)) * (ex >= 0.0f ? 1.0f : -1.0f), ty = (1.0f - fabsf(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
        ex = tx, ey = ty;
    }
    int32_t qx = (int32_t)lroundf(ex * 32767.0f), qy = (int32_t)lroundf(ey * 32767.0f);
    g->normal_oct = ((uint32_t)qx & 0xFFFFu) | ((uint32_t)qy << 16);
    g->uv[0] = u, g->uv[1] = v;
    g->uv2[0] = g->uv2[1] = -1.0f;
}

// A conifer of leaf cards (14 m): a tapered trunk (16 sides, the texture's bark column) and 420 cards around it in a cone,
// their normals bent outward (a crown lit as a volume, not as flat cards)
static bool make_tree(const char* path) {
    const uint32_t NC = 420, SEG = 16;
    uint32_t ntri = SEG * 2 + NC * 2;
    GpuVertex* c = calloc((size_t)ntri * 3, sizeof *c);
    if (!c) return false;
    uint32_t t = 0;
    for (uint32_t k = 0; k < SEG; k++) {
        float a0 = 6.2831853f * (float)k / SEG, a1 = 6.2831853f * (float)(k + 1) / SEG;
        float r0 = 32.0f, r1 = 9.0f, h = 1250.0f;
        float p00[3] = { cosf(a0) * r0, sinf(a0) * r0, 0 }, p10[3] = { cosf(a1) * r0, sinf(a1) * r0, 0 };
        float p01[3] = { cosf(a0) * r1, sinf(a0) * r1, h }, p11[3] = { cosf(a1) * r1, sinf(a1) * r1, h };
        float n0[3] = { cosf(a0), sinf(a0), 0.1f }, n1[3] = { cosf(a1), sinf(a1), 0.1f };
        float u0 = 0.01f + 0.1f * (float)k / SEG, u1 = 0.01f + 0.1f * (float)(k + 1) / SEG;
        vtx(&c[t * 3 + 0], p00, n0, u0, 1.0f), vtx(&c[t * 3 + 1], p11, n1, u1, 0.0f), vtx(&c[t * 3 + 2], p10, n1, u1, 1.0f), t++;
        vtx(&c[t * 3 + 0], p00, n0, u0, 1.0f), vtx(&c[t * 3 + 1], p01, n0, u0, 0.0f), vtx(&c[t * 3 + 2], p11, n1, u1, 0.0f), t++;
    }
    uint32_t s = 4242u;
    for (uint32_t k = 0; k < NC; k++) {
        float hz = 250.0f + 1150.0f * powf(rnd(&s), 0.8f); // (more near the bottom)
        float reach = (1400.0f - hz) / 1150.0f * 330.0f + 40.0f;
        float a = rnd(&s) * 6.2831853f, d = reach * (0.35f + 0.65f * sqrtf(rnd(&s)));
        float cx = cosf(a) * d, cy = sinf(a) * d;
        float yaw = rnd(&s) * 6.2831853f, tilt = (rnd(&s) - 0.5f) * 1.2f, w = 70.0f + 70.0f * rnd(&s), hh = 60.0f + 50.0f * rnd(&s);
        float ax[3] = { cosf(yaw), sinf(yaw), 0.0f }, ay[3] = { -sinf(yaw) * sinf(tilt), cosf(yaw) * sinf(tilt), cosf(tilt) };
        float cn[3] = { ay[1] * ax[2] - ay[2] * ax[1], ay[2] * ax[0] - ay[0] * ax[2], ay[0] * ax[1] - ay[1] * ax[0] };
        float out[3] = { cx, cy, (hz - 820.0f) * 0.35f }, ol = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]) + 1e-3f;
        float side = cn[0] * out[0] + cn[1] * out[1] + cn[2] * out[2] < 0.0f ? -1.0f : 1.0f;
        float n[3];
        for (int i = 0; i < 3; i++) n[i] = cn[i] * side * 0.35f + out[i] / ol * 0.65f;
        float q[4][3];
        for (int i = 0; i < 3; i++) {
            float base = i == 0 ? cx : i == 1 ? cy : hz;
            q[0][i] = base - ax[i] * w - ay[i] * hh, q[1][i] = base + ax[i] * w - ay[i] * hh;
            q[2][i] = base + ax[i] * w + ay[i] * hh, q[3][i] = base - ax[i] * w + ay[i] * hh;
        }
        float u0 = 0.13f + 0.87f * rnd(&s) * 0.75f, v0 = rnd(&s) * 0.75f, us = 0.2f, vs = 0.25f;
        vtx(&c[t * 3 + 0], q[0], n, u0, v0 + vs), vtx(&c[t * 3 + 1], q[2], n, u0 + us, v0), vtx(&c[t * 3 + 2], q[1], n, u0 + us, v0 + vs), t++;
        vtx(&c[t * 3 + 0], q[0], n, u0, v0 + vs), vtx(&c[t * 3 + 1], q[3], n, u0, v0), vtx(&c[t * 3 + 2], q[2], n, u0 + us, v0), t++;
    }
    MeshBuilt mb;
    bool ok = mesh_build_corners(c, ntri, true, &mb);
    if (ok) ok = rmesh_write(path, &mb), mesh_built_free(&mb);
    free(c);
    return ok;
}

// A rock: an icosphere (3 subdivisions, 1280 triangles) pushed by a few waves, flattened, 2 m across
static bool make_rock(const char* path) {
    static float v[642][3];
    static uint32_t f[1280][3], f2[1280][3];
    const float X = 0.525731f, Z = 0.850651f;
    float base[12][3] = { { -X, 0, Z }, { X, 0, Z }, { -X, 0, -Z }, { X, 0, -Z }, { 0, Z, X }, { 0, Z, -X }, { 0, -Z, X }, { 0, -Z, -X }, { Z, X, 0 }, { -Z, X, 0 }, { Z, -X, 0 }, { -Z, -X, 0 } };
    uint32_t bf[20][3] = { { 0, 4, 1 }, { 0, 9, 4 }, { 9, 5, 4 }, { 4, 5, 8 }, { 4, 8, 1 }, { 8, 10, 1 }, { 8, 3, 10 }, { 5, 3, 8 }, { 5, 2, 3 }, { 2, 7, 3 },
                           { 7, 10, 3 }, { 7, 6, 10 }, { 7, 11, 6 }, { 11, 0, 6 }, { 0, 1, 6 }, { 6, 1, 10 }, { 9, 0, 11 }, { 9, 11, 2 }, { 9, 2, 5 }, { 7, 2, 11 } };
    uint32_t nv = 12, nf = 20;
    memcpy(v, base, sizeof base), memcpy(f, bf, sizeof bf);
    for (int lv = 0; lv < 3; lv++) {
        uint32_t n2 = 0;
        for (uint32_t i = 0; i < nf; i++) {
            uint32_t m[3];
            for (int e = 0; e < 3; e++) {
                uint32_t a = f[i][e], b = f[i][(e + 1) % 3];
                float p[3];
                for (int k = 0; k < 3; k++) p[k] = (v[a][k] + v[b][k]) * 0.5f;
                float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                uint32_t found = UINT32_MAX;
                for (uint32_t q = 12; q < nv && found == UINT32_MAX; q++)
                    if (fabsf(v[q][0] - p[0] / l) < 1e-5f && fabsf(v[q][1] - p[1] / l) < 1e-5f && fabsf(v[q][2] - p[2] / l) < 1e-5f) found = q;
                if (found == UINT32_MAX) found = nv, v[nv][0] = p[0] / l, v[nv][1] = p[1] / l, v[nv][2] = p[2] / l, nv++;
                m[e] = found;
            }
            uint32_t tri[4][3] = { { f[i][0], m[0], m[2] }, { m[0], f[i][1], m[1] }, { m[2], m[1], f[i][2] }, { m[0], m[1], m[2] } };
            memcpy(&f2[n2], tri, sizeof tri), n2 += 4;
        }
        memcpy(f, f2, sizeof(uint32_t) * 3 * n2), nf = n2;
    }
    for (uint32_t i = 0; i < nv; i++) {
        float* p = v[i];
        float r = 1.0f + 0.18f * sinf(p[0] * 5.1f + 1.3f) * cosf(p[1] * 4.3f) + 0.1f * sinf(p[2] * 9.7f + p[0] * 3.1f);
        p[0] *= 100.0f * r, p[1] *= 90.0f * r, p[2] = p[2] * 60.0f * r + 30.0f;
    }
    GpuVertex* c = calloc((size_t)nf * 3, sizeof *c);
    if (!c) return false;
    for (uint32_t i = 0; i < nf; i++) {
        float* a = v[f[i][0]];
        float* b = v[f[i][1]];
        float* d = v[f[i][2]];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        // (a facet's normal: the corners split, a rock's hard faces)
        vtx(&c[i * 3 + 0], a, n, 0, 0), vtx(&c[i * 3 + 1], d, n, 0, 1), vtx(&c[i * 3 + 2], b, n, 1, 0);
    }
    MeshBuilt mb;
    bool ok = mesh_build_corners(c, nf, true, &mb);
    if (ok) ok = rmesh_write(path, &mb), mesh_built_free(&mb);
    free(c);
    return ok;
}

static Entity actor(World* w, uint64_t extra, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | extra);
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_STATIC;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    return e;
}

bool farview_test_scene(Editor* ed, const char* name) {
    int sc = !strcmp(name, "vista") ? 1 : !strcmp(name, "vista_pop") ? 2 : !strcmp(name, "vista_walk") ? 3 : 0;
    if (!sc) return false;
    memset(&VT, 0, sizeof VT);
    VT.scene = sc;
    double t0 = re_time_seconds();
    VT.t0 = t0;
    char dir[900], tree[900], rock[900];
    vpath(dir, sizeof dir, "");
    re_make_dirs(dir);
    vpath(tree, sizeof tree, "SM_Vista_Tree_v1.rmesh"), vpath(rock, sizeof rock, "SM_Vista_Rock_v1.rmesh");
    bool ok = make_textures(), meshes = false;
    if (!re_file_stat(tree, NULL, NULL, NULL)) ok = make_tree(tree) && ok, meshes = true;
    if (!re_file_stat(rock, NULL, NULL, NULL)) ok = make_rock(rock) && ok, meshes = true;
    if (meshes) render_meshes_reload(ed->vk);
    uint32_t mat = make_material();
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE);
    uint32_t tree_id = render_mesh_id_of("/Content/Vista/SM_Vista_Tree_v1.rmesh"), rock_id = render_mesh_id_of("/Content/Vista/SM_Vista_Rock_v1.rmesh");
    if (render_mesh_slot(tree_id) < SHAPE_COUNT || render_mesh_slot(rock_id) < SHAPE_COUNT || !mat)
        printf("[RealEngine] FAIL: Vista: the tree, the rock or the leaves' material didn't load\n"), ok = false;
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    // the terrain: 8 km of mountains (1024 quads: 7.8 m), 16 x 16 blocks
    Entity land = actor(w, COMP_BIT(COMP_LANDSCAPE), "Landscape", 0, 0, 0);
    CLandscape* lc = ecs_get(w, land, COMP_LANDSCAPE);
    snprintf(lc->name, sizeof lc->name, "VistaLandscape");
    lc->res = 1024, lc->chunks = 16, lc->size = 800000.0f, lc->height = 90000.0f, lc->roughness = 0.62f, lc->seed = 2026;
    lc->color[0] = 0.16f, lc->color[1] = 0.2f, lc->color[2] = 0.09f;
    // the forest: one Foliage actor (filled once over the landscape)
    Entity fol = actor(w, COMP_BIT(COMP_FOLIAGE), "Foliage", 0, 0, 0);
    CFoliage* fc = ecs_get(w, fol, COMP_FOLIAGE);
    snprintf(fc->name, sizeof fc->name, "@fill");
    fc->ntypes = 1, fc->seed = 21;
    FoliageType* ft = &fc->types[0];
    ft->mesh = tree_id, ft->material = mat, ft->density = 0.09f, ft->scale[0] = 0.7f, ft->scale[1] = 1.35f, ft->stretch = 1.0f;
    ft->slope = 32.0f, ft->wind = 0.0f, ft->paint = 1;
    ft->color[0] = ft->color[1] = ft->color[2] = 1.0f;
    // the rocks: in the World Partition's grid (their HLOD: proxies near, the regions' merged meshes far, in Play)
    Entity wpe = actor(w, COMP_BIT(COMP_WORLD_PARTITION), "WorldPartition", 0, 0, 0);
    (void)wpe;
    VT.rocks = 3000;
    uint32_t s = 99u;
    for (uint32_t i = 0; i < VT.rocks; i++) {
        float x = (rnd(&s) - 0.5f) * 760000.0f, y = (rnd(&s) - 0.5f) * 760000.0f, sc2 = 0.8f + 3.2f * rnd(&s) * rnd(&s);
        Entity r = actor(w, COMP_BIT(COMP_STATIC_MESH), "Rock", x, y, 0.0f);
        CStaticMesh* m = ecs_get(w, r, COMP_STATIC_MESH);
        m->mesh = rock_id, m->uv_scale = 1.0f, m->roughness = 0.85f, m->cast_shadow = 1;
        m->base_color[0] = 0.33f, m->base_color[1] = 0.31f, m->base_color[2] = 0.28f;
        CTransform* t = ecs_get(w, r, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = sc2, t->rotation[2] = 360.0f * rnd(&s);
    }
    // a low sun (long shadows across the valleys), the sky, a light haze
    Entity sun = actor(w, COMP_BIT(COMP_LIGHT), "Sun", 0, 0, 100000);
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->type = LIGHT_DIRECTIONAL;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->intensity = 120000.0f;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->cast_shadows = 1;
    CTransform* st = ecs_get(w, sun, COMP_TRANSFORM);
    st->rotation[0] = -9.0f, st->rotation[1] = 285.0f; // (from the left, low: the ridges' shadows across the view)
    Entity sky = actor(w, COMP_BIT(COMP_LIGHT), "Sky", 0, 0, 99000);
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->type = LIGHT_SKY;
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->intensity = 1.0f; // (a scale: the ECS's default is a point light's candelas)
    Entity fog = actor(w, COMP_BIT(COMP_FOG), "Fog", 0, 0, 0);
    CFog* fg = ecs_get(w, fog, COMP_FOG);
    fg->density = 0.00025f, fg->height_falloff = 0.0012f, fg->max_opacity = 1.0f, fg->volumetric = 0;
    fg->albedo[0] = fg->albedo[1] = fg->albedo[2] = 0.9f;
    // where the camera starts (moved to the highest point of the corner once the heights exist: farview_test_frame)
    ed->cam.pos[0] = -300000.0f, ed->cam.pos[1] = -300000.0f, ed->cam.pos[2] = 60000.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -6.0f;
    ed->game_view = true; // (no icons over the view)
    char msg[256];
    if (!wp_build_hlods(w, mat_root(), "Untitled", msg, sizeof msg)) printf("[RealEngine] Vista: no HLOD built (%s)\n", msg);
    else render_meshes_reload(ed->vk);
    printf("[RealEngine] Vista scene %s: assets ready in %.0f ms%s\n", name, (re_time_seconds() - t0) * 1000.0, ok ? "" : " - FAIL");
    return true;
}

// The highest point of the landscape's corner (2 x 2 km around (-2.6, -2.6) km): the lookout
static bool place_camera(Editor* ed) {
    float best = -1e30f, bx = 0.0f, by = 0.0f, z;
    for (int j = 0; j < 48; j++)
        for (int i = 0; i < 48; i++) {
            float x = -360000.0f + 200000.0f * (float)i / 47.0f, y = -360000.0f + 200000.0f * (float)j / 47.0f;
            if (landscape_height(x, y, &z) && z > best) best = z, bx = x, by = y;
        }
    if (best < -1e29f) return false;
    float zc;
    landscape_height(0.0f, 0.0f, &zc);
    ed->cam.pos[0] = bx, ed->cam.pos[1] = by, ed->cam.pos[2] = best + (VT.scene == 3 ? 6000.0f : 2500.0f);
    ed->cam.yaw = 45.0f;
    float dz = (zc - ed->cam.pos[2]), dh = sqrtf(bx * bx + by * by);
    ed->cam.pitch = fmaxf(fminf(atan2f(dz, dh) * 57.2958f + 1.5f, 0.0f), -14.0f);
    printf("[RealEngine] Vista: the lookout at (%.2f, %.2f) km, %.0f m high, pitch %.1f\n", bx * 1e-5f, by * 1e-5f, ed->cam.pos[2] * 0.01f, ed->cam.pitch);
    return true;
}

bool farview_test_ready(Editor* ed) {
    (void)ed;
    return VT.scene == 0 || VT.ready;
}

void farview_test_frame(Editor* ed, struct VkCtx* vk, bool measuring) {
    if (!VT.scene) return;
    if (!VT.placed) VT.placed = place_camera(ed);
    const FarviewStats* fs = farview_stats();
    if (!VT.ready && VT.placed) { // (ready: the trees filled, every candidate's impostor baked, the far shadow made)
        uint32_t tr = foliage_stats()->instances;
        static double last;
        double now = re_time_seconds();
        if (now - last > 3.0) { // (what the measure waits for)
            last = now;
            printf("[RealEngine] Vista: waiting: %u trees, impostors %u of %u, %u horizon passes\n", tr, fs->impostors, fs->candidates, fs->shadow_runs);
        }
        bool imp_ok = !fs->imp_on || (fs->candidates > 0 && fs->impostors >= fs->candidates); // (impostors off: none to wait for)
        const char* fe = getenv("REALENGINE_FAR_SHADOW");
        bool sh_ok = (fe && fe[0] == '0') || fs->shadow_runs > 0; // (the far shadow off: none to wait for)
        if (tr > 1000 && imp_ok && sh_ok) {
            if (!VT.frames_ready) VT.t_ready = re_time_seconds();
            if (++VT.frames_ready > 20) VT.ready = true, VT.trees = tr;
        }
    }
    if (VT.scene == 2 && measuring) { // the pop: the switch at its preset's screen size, then at 2.5 times it, a shot of each
        VT.pop_phase++;
        if (VT.pop_phase == 1) farview_set_switch_override(0.0f);
        if (VT.pop_phase == 60) snprintf(VT.shot[0], sizeof VT.shot[0], "vista_pop_mesh.png"), vkctx_request_shot(vk, VT.shot[0]);
        if (VT.pop_phase == 120) snprintf(VT.shot[1], sizeof VT.shot[1], "vista_pop_mesh2.png"), vkctx_request_shot(vk, VT.shot[1]);
        if (VT.pop_phase == 125) farview_set_switch_override(fs->switch_px * 2.5f);
        if (VT.pop_phase == 185) snprintf(VT.shot[2], sizeof VT.shot[2], "vista_pop_card.png"), vkctx_request_shot(vk, VT.shot[2]);
    }
}

// Two shots compared over the viewport (the panels and the status bar's FPS left out): the mean change (0-255) and the
// share of pixels changed by more than 16
static bool shot_diff(const char* p0, const char* p1, double* mean, double* share) {
    uint32_t w0, h0, w1, h1;
    uint8_t* a = mat_load_image_rgba(p0, &w0, &h0);
    uint8_t* b = mat_load_image_rgba(p1, &w1, &h1);
    bool ok = a && b && w0 == w1 && h0 == h1;
    if (ok) {
        double sum = 0.0, off[3] = { 0, 0, 0 };
        uint64_t big = 0, n = 0;
        uint32_t x0 = w0 / 100u, x1 = w0 * 82u / 100u, y0 = h0 * 6u / 100u, y1 = h0 * 96u / 100u;
        // (the whole view's own shift taken out first: the exposure still settling moves every pixel a little)
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = x0; x < x1; x++, n++)
                for (int k = 0; k < 3; k++) off[k] += (double)b[((size_t)y * w0 + x) * 4 + k] - (double)a[((size_t)y * w0 + x) * 4 + k];
        for (int k = 0; k < 3; k++) off[k] /= (double)(n ? n : 1);
        n = 0;
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = x0; x < x1; x++) {
                size_t i = (size_t)y * w0 + x;
                double d = 0.0;
                for (int k = 0; k < 3; k++) {
                    double e = fabs((double)b[i * 4 + k] - (double)a[i * 4 + k] - off[k]);
                    d = e > d ? e : d;
                }
                sum += d, n++;
                big += d > 16;
            }
        *mean = sum / (double)n, *share = 100.0 * (double)big / (double)n;
    }
    free(a), free(b);
    return ok;
}

// The pop: the meshes' two shots apart (the view drifting by itself: the exposure, the history) against the meshes' second
// and the cards': what the switch adds over the drift
static bool pop_check(void) {
    double dm, ds, pm, ps;
    if (!shot_diff(VT.shot[0], VT.shot[1], &dm, &ds) || !shot_diff(VT.shot[1], VT.shot[2], &pm, &ps)) {
        printf("[RealEngine] FAIL: Vista pop: the shots weren't written (%s, %s, %s)\n", VT.shot[0], VT.shot[1], VT.shot[2]);
        return false;
    }
    double mean = fmax(pm - dm, 0.0), share = fmax(ps - ds, 0.0);
    printf("[RealEngine] Vista pop: meshes -> cards at the switch: mean change %.3f of 255, %.3f%% of the pixels past 16 (over the "
           "view's own drift: %.3f, %.3f%%)\n", mean, share, dm, ds);
    if (mean > 2.5 || share > 0.4) printf("[RealEngine] FAIL: Vista pop: the switch to the impostors changes the view too much\n");
    return mean <= 2.5 && share <= 0.4;
}

bool farview_test_check(Editor* ed) {
    (void)ed;
    if (!VT.scene) return true;
    const FarviewStats* fs = farview_stats();
    bool ok = true;
    printf("[RealEngine] Vista: %u trees, %u rocks; impostors %u of %u candidates (%u failed); far shadow %s, %u x %u (%.1f m a texel), %u builds, "
           "%u horizon passes (the last %.3f ms GPU); ready after %.1f s\n",
           VT.trees, VT.rocks, fs->impostors, fs->candidates, fs->failed, fs->shadow_on ? "on" : "off", fs->res, fs->res, fs->texel_cm * 0.01f,
           fs->hf_builds, fs->shadow_runs, fs->shadow_gpu_ms, VT.t_ready > 0.0 ? VT.t_ready - VT.t0 : 0.0);
    if (!VT.ready) printf("[RealEngine] FAIL: Vista: never ready (trees, impostors or the far shadow missing)\n"), ok = false;
    if (fs->impostors < 2) printf("[RealEngine] FAIL: Vista: the tree's and the rock's impostors not baked\n"), ok = false;
    if (!fs->shadow_on) printf("[RealEngine] FAIL: Vista: no far shadow\n"), ok = false;
    // the far shadow on the CPU's twin: a ground point every 125 m; some of the valleys shaded (a low sun behind ridges),
    // most of the slopes facing it lit
    uint32_t shade = 0, lit = 0, all = 0;
    for (int j = 0; j < 64; j++)
        for (int i = 0; i < 64; i++) {
            float p[3] = { -390000.0f + 780000.0f * (float)i / 63.0f, -390000.0f + 780000.0f * (float)j / 63.0f, 0.0f }, v;
            if (!landscape_height(p[0], p[1], &p[2]) || !farview_shadow_at(p, &v)) continue;
            all++, shade += v < 0.1f, lit += v > 0.9f;
        }
    printf("[RealEngine] Vista far shadow: %u of %u ground points shaded by the terrain, %u lit\n", shade, all, lit);
    if (all < 3000 || shade < all / 50 || lit < all / 4) printf("[RealEngine] FAIL: Vista: the far shadow's coverage\n"), ok = false;
    if (VT.scene == 2) ok = pop_check() && ok;
    if (VT.scene == 3) { // (the flight: the cards turning and switching every frame, its cost by --min-fps)
        // (what goes back and forth, reported only: on this scene the probe gave 0.006% to 7% between runs shown on the
        // second monitor, the impostors on or off alike, and 0.07-0.13% hidden: no limit that means something here)
        double pulses = render_flicker_pulses() * 100.0;
        printf("[RealEngine] Vista walk: %.3f%% of the view back and forth\n", pulses);
    }
    return ok;
}
