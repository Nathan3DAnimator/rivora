// The Blueprint preview (bp_preview.h): the class's components drawn on the CPU into ImGui's draw list. Every mesh is a
// convex basic shape: culling each one's faces turned away and drawing the meshes back to front is all the depth it
// needs; the ImGui rasterizer interpolates the colors (Gouraud) and the silhouettes are drawn again with anti-aliased
// lines. The component tree's helpers (collisions, arrows, cameras, booms) are wires; the selected component gets the
// W/E/R gizmo. Cost: lost in the frame's noise (editor CPU 0.55 ms with the Character's Viewport open or not,
// notas-dev/blueprints.md); a thumbnail uses LOD 2.
// The editors' viewports ask the GPU for the same view instead (bp_preview_*_gpu: render_preview, the materials' textures,
// the view modes, the shadow); the CPU drawing stays for the thumbnails and when there is no GPU preview.
#include "bp_preview.h"

#define THEME_SCENE // (overlays on the 3D scene: theme.h)
#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

#include "anim.h"
#include "blueprint.h"
#include "components.h"
#include "i18n.h"
#include "render.h"
#include "render_gpu.h"
#include "ui_icons.h"

#define DEG2RAD 0.017453293f
#define FOV_DEG 40.0f // vertical
#define MAX_LOD 4

// A shape's LOD, decoded once: normals, which triangle lies across each edge (by position: the UV seams don't split it)
typedef struct ShapeCache {
    bool built, failed;
    const GpuVertex* v;
    uint32_t nv, ntri;
    const uint32_t* idx;
    float* n;     // 3 per vertex
    int32_t* adj; // 3 per triangle: the neighbor across edge k (k, k+1), -1: an open edge
} ShapeCache;

static ShapeCache g_shapes[RE_MAX_MESHES][MAX_LOD]; // per mesh slot (the shapes, then the imported meshes)
// the frame's scratch (the editor's UI: grown, never freed)
static ImVec2* g_scr;
static uint32_t* g_col;
static uint8_t* g_front;
static uint32_t g_cap_v, g_cap_t;

static void oct_decode(uint32_t p, float n[3]) {
    float x = (float)(int16_t)(p & 0xFFFF) / 32767.0f, y = (float)(int16_t)(p >> 16) / 32767.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l, n[1] = y / l, n[2] = z / l;
}

static uint32_t hash3(int32_t a, int32_t b, int32_t c) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u ^ (uint32_t)c * 83492791u;
    return h ^ (h >> 15);
}

static const ShapeCache* shape(uint32_t s, uint32_t lod) {
    static uint32_t gen;
    if (gen != render_meshes_generation()) { // the meshes were reloaded (an import): the cached pointers are stale
        gen = render_meshes_generation();
        for (uint32_t i = 0; i < RE_MAX_MESHES; i++)
            for (uint32_t l = 0; l < MAX_LOD; l++) free(g_shapes[i][l].n), free(g_shapes[i][l].adj);
        memset(g_shapes, 0, sizeof g_shapes);
    }
    s = render_mesh_slot(s); // (an imported mesh by its id)
    if (s >= RE_MAX_MESHES) return NULL;
    if (lod >= MAX_LOD) lod = MAX_LOD - 1;
    ShapeCache* c = &g_shapes[s][lod];
    if (c->built) return c;
    if (c->failed) return NULL;
    const void* vv;
    uint32_t ni;
    if (!render_shape_cpu(s, lod, &vv, &c->nv, &c->idx, &ni) || !c->nv) {
        c->failed = true;
        return NULL;
    }
    c->v = vv;
    c->ntri = ni / 3;
    // once per shape and LOD, the first time a preview shows it
    c->n = malloc(sizeof(float) * 3 * c->nv);
    c->adj = malloc(sizeof(int32_t) * 3 * c->ntri);
    uint32_t* pid = malloc(sizeof(uint32_t) * c->nv);
    uint32_t hcap = 64, ecap = 64;
    while (hcap < c->nv * 2) hcap <<= 1;
    while (ecap < c->ntri * 6) ecap <<= 1;
    int32_t* ht = malloc(sizeof(int32_t) * hcap);
    int32_t* et = malloc(sizeof(int32_t) * ecap); // an edge's first (triangle * 3 + k)
    if (!c->n || !c->adj || !pid || !ht || !et) {
        free(c->n), free(c->adj), free(pid), free(ht), free(et);
        c->n = NULL, c->adj = NULL;
        c->failed = true;
        return NULL;
    }
    memset(ht, 0xFF, sizeof(int32_t) * hcap);
    memset(et, 0xFF, sizeof(int32_t) * ecap);
    for (uint32_t i = 0; i < c->nv; i++) {
        oct_decode(c->v[i].normal_oct, &c->n[i * 3]);
        int32_t q[3];
        for (int k = 0; k < 3; k++) q[k] = (int32_t)lroundf(c->v[i].pos[k] * 64.0f);
        for (uint32_t h = hash3(q[0], q[1], q[2]) & (hcap - 1);; h = (h + 1) & (hcap - 1)) {
            if (ht[h] < 0) {
                ht[h] = (int32_t)i, pid[i] = i;
                break;
            }
            const float* o = c->v[ht[h]].pos;
            if ((int32_t)lroundf(o[0] * 64.0f) == q[0] && (int32_t)lroundf(o[1] * 64.0f) == q[1] && (int32_t)lroundf(o[2] * 64.0f) == q[2]) {
                pid[i] = pid[ht[h]];
                break;
            }
        }
    }
    for (uint32_t t = 0; t < c->ntri; t++)
        for (int k = 0; k < 3; k++) {
            c->adj[t * 3 + k] = -1;
            uint32_t a = pid[c->idx[t * 3 + k]], b = pid[c->idx[t * 3 + (k + 1) % 3]];
            uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
            for (uint32_t h = hash3((int32_t)lo, (int32_t)hi, 7) & (ecap - 1);; h = (h + 1) & (ecap - 1)) {
                if (et[h] < 0) {
                    et[h] = (int32_t)(t * 3 + k);
                    break;
                }
                uint32_t ot = (uint32_t)et[h] / 3, ok = (uint32_t)et[h] % 3;
                uint32_t oa = pid[c->idx[ot * 3 + ok]], ob = pid[c->idx[ot * 3 + (ok + 1) % 3]];
                if ((oa < ob ? oa : ob) == lo && (oa < ob ? ob : oa) == hi && c->adj[et[h]] < 0 && ot != t) {
                    c->adj[et[h]] = (int32_t)t;
                    c->adj[t * 3 + k] = (int32_t)ot;
                    break;
                }
            }
        }
    free(pid), free(ht), free(et);
    c->built = true;
    return c;
}

static bool scratch(uint32_t nv, uint32_t nt) {
    if (nv > g_cap_v) {
        ImVec2* s = realloc(g_scr, sizeof(ImVec2) * nv);
        uint32_t* cl = s ? realloc(g_col, sizeof(uint32_t) * nv) : NULL;
        if (s) g_scr = s;
        if (cl) g_col = cl;
        if (!s || !cl) return false;
        g_cap_v = nv;
    }
    if (nt > g_cap_t) {
        uint8_t* f = realloc(g_front, nt);
        if (!f) return false;
        g_front = f, g_cap_t = nt;
    }
    return true;
}

// --- Light and color ---

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-8f) v[0] /= l, v[1] /= l, v[2] /= l;
}

static float aces(float x) { return fminf(fmaxf((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f), 1.0f); }
static uint32_t to_srgb8(const float c[3], float alpha) {
    uint32_t o = 0;
    for (int k = 0; k < 3; k++) {
        float x = aces(c[k] * 0.8f);
        x = x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
        o |= (uint32_t)lroundf(fminf(fmaxf(x, 0.0f), 1.0f) * 255.0f) << (8 * k);
    }
    return o | ((uint32_t)lroundf(alpha * 255.0f) << 24);
}

typedef struct Look {
    float base[3], metal, rough, emissive[3];
    float key_dir[3], key[3];
} Look;

// A studio: a warm key from the camera's upper left, a blue sky above, a dark floor bounce below, a cool rim behind
static void shade(const Look* L, const float n[3], const float v[3], float out[3]) {
    static const float sky[3] = { 0.34f, 0.40f, 0.52f }, ground[3] = { 0.10f, 0.09f, 0.08f }, rim[3] = { 0.45f, 0.55f, 0.75f };
    float ndl = fmaxf(dot3(n, L->key_dir), 0.0f), ndv = fmaxf(dot3(n, v), 1e-3f);
    float h[3] = { L->key_dir[0] + v[0], L->key_dir[1] + v[1], L->key_dir[2] + v[2] };
    norm3(h);
    float ndh = fmaxf(dot3(n, h), 0.0f), vdh = fmaxf(dot3(v, h), 0.0f);
    float r = fmaxf(L->rough, 0.08f), a2 = r * r * r * r;
    float d = ndh * ndh * (a2 - 1.0f) + 1.0f, D = a2 / (3.14159265f * d * d);
    float k = (r + 1.0f) * (r + 1.0f) / 8.0f, G = (ndl / (ndl * (1 - k) + k)) * (ndv / (ndv * (1 - k) + k));
    float spec = ndl > 0.0f ? D * G / (4.0f * ndl * ndv) * ndl : 0.0f;
    float fv = powf(1.0f - vdh, 5.0f), fe = powf(1.0f - ndv, 5.0f);
    float rr[3] = { 2.0f * dot3(n, v) * n[0] - v[0], 2.0f * dot3(n, v) * n[1] - v[1], 2.0f * dot3(n, v) * n[2] - v[2] };
    float hemi_n = n[2] * 0.5f + 0.5f, hemi_r = rr[2] * 0.5f + 0.5f, back = powf(1.0f - ndv, 3.0f) * 0.5f;
    for (int c = 0; c < 3; c++) {
        float f0 = 0.04f + (L->base[c] - 0.04f) * L->metal;
        float amb = ground[c] + (sky[c] - ground[c]) * hemi_n;
        float env = (ground[c] + (sky[c] * 1.6f - ground[c]) * hemi_r) * (f0 + (1.0f - f0) * fe * (1.0f - r)) * (1.0f - r * 0.6f);
        out[c] = L->base[c] * (1.0f - L->metal) * (L->key[c] * ndl + amb) + L->key[c] * spec * (f0 + (1.0f - f0) * fv) + env +
                 rim[c] * back * (0.4f + 0.6f * hemi_n) + L->emissive[c];
    }
}

// --- The camera ---

typedef struct View {
    float eye[3], fwd[3], right[3], up[3];
    float cx, cy, f; // screen center, focal length (px)
} View;

static void view_of(const BpPreviewCam* cam, float x0, float y0, float x1, float y1, View* v) {
    float sp = sinf(cam->pitch * DEG2RAD), cp = cosf(cam->pitch * DEG2RAD), sy = sinf(cam->yaw * DEG2RAD), cy = cosf(cam->yaw * DEG2RAD);
    v->fwd[0] = cp * cy, v->fwd[1] = cp * sy, v->fwd[2] = sp; // level_camera_basis's
    v->right[0] = -sy, v->right[1] = cy, v->right[2] = 0.0f;
    v->up[0] = -sp * cy, v->up[1] = -sp * sy, v->up[2] = cp;
    for (int k = 0; k < 3; k++) v->eye[k] = cam->target[k] - v->fwd[k] * cam->dist;
    v->cx = (x0 + x1) * 0.5f, v->cy = (y0 + y1) * 0.5f;
    v->f = (y1 - y0) * 0.5f / tanf(FOV_DEG * 0.5f * DEG2RAD);
}

// view space: x right, y up, z depth
static void to_view(const View* v, const float p[3], float o[3]) {
    float d[3] = { p[0] - v->eye[0], p[1] - v->eye[1], p[2] - v->eye[2] };
    o[0] = dot3(d, v->right), o[1] = dot3(d, v->up), o[2] = dot3(d, v->fwd);
}
static ImVec2 project_view(const View* v, const float q[3]) {
    float z = fmaxf(q[2], 0.1f);
    return (ImVec2){ v->cx + q[0] * v->f / z, v->cy - q[1] * v->f / z };
}
static ImVec2 project(const View* v, const float p[3]) {
    float q[3];
    to_view(v, p, q);
    return project_view(v, q);
}

// A segment in the world, cut at the near plane (the grid passes behind the camera)
static void line3(ImDrawList* dl, const View* v, const float a[3], const float b[3], uint32_t col, float th) {
    float qa[3], qb[3];
    to_view(v, a, qa), to_view(v, b, qb);
    const float nz = 1.0f;
    if (qa[2] < nz && qb[2] < nz) return;
    if (qa[2] < nz || qb[2] < nz) {
        float t = (nz - qa[2]) / (qb[2] - qa[2]);
        float* m = qa[2] < nz ? qa : qb;
        for (int k = 0; k < 3; k++) m[k] = qa[k] + (qb[k] - qa[k]) * t;
    }
    ImDrawList_AddLine(dl, project_view(v, qa), project_view(v, qb), col, th);
}

// --- The class's look: its root (the actor's mesh, light, Character) and its component tree ---

typedef struct Parts {
    const BpAsset* a;
    const CTransform* t;
    const CStaticMesh* mesh;
    const CCharacter* chr;
    const CLight* light;
    float mn[3], mx[3]; // bounds (cm, actor space)
} Parts;

static void grow(Parts* p, const float c[3], const float e[3]) {
    for (int k = 0; k < 3; k++) p->mn[k] = fminf(p->mn[k], c[k] - e[k]), p->mx[k] = fmaxf(p->mx[k], c[k] + e[k]);
}

// helpers: the component helpers count in the bounds (the editor's frame); a thumbnail frames the meshes and lights
static bool parts_of(const BpAsset* a, Parts* p, bool helpers) {
    memset(p, 0, sizeof *p);
    if (!a || a->kind != BPA_CLASS) return false;
    p->a = a;
    p->t = (const CTransform*)a->comp_data[COMP_TRANSFORM];
    if (a->comp_mask & COMP_BIT(COMP_STATIC_MESH)) {
        p->mesh = (const CStaticMesh*)a->comp_data[COMP_STATIC_MESH];
    }
    if (a->comp_mask & COMP_BIT(COMP_CHARACTER)) p->chr = (const CCharacter*)a->comp_data[COMP_CHARACTER];
    if (a->comp_mask & COMP_BIT(COMP_LIGHT)) p->light = (const CLight*)a->comp_data[COMP_LIGHT];
    if (!p->mesh && !p->chr && !p->light && !a->nscs) return false;
    for (int k = 0; k < 3; k++) p->mn[k] = 1e30f, p->mx[k] = -1e30f;
    const float zero[3] = { 0, 0, 0 };
    if (p->mesh) {
        float hz = p->mesh->mesh == SHAPE_PLANE ? 0.5f : 50.0f, e[3];
        for (int k = 0; k < 3; k++) e[k] = (k == 2 ? hz : 50.0f) * fabsf(p->t->scale[k] != 0.0f ? p->t->scale[k] : 1.0f);
        grow(p, zero, e);
    }
    if (p->chr) {
        float r = p->chr->radius, h = p->chr->half_height;
        p->mn[0] = fminf(p->mn[0], -r), p->mn[1] = fminf(p->mn[1], -r), p->mn[2] = fminf(p->mn[2], -h);
        p->mx[0] = fmaxf(p->mx[0], r * 2.2f), p->mx[1] = fmaxf(p->mx[1], r), p->mx[2] = fmaxf(p->mx[2], h);
    }
    if (p->light && !p->mesh && !p->chr) grow(p, zero, (float[3]){ 25, 25, 25 });
    for (uint32_t i = 0; i < a->nscs; i++) {
        const BpScsNode* n = &a->scs[i];
        bool mesh = n->kind == SCS_STATIC_MESH;
        bool light = n->kind >= SCS_POINT_LIGHT && n->kind <= SCS_RECT_LIGHT;
        float ax[3][3], o[3], e[3] = { 15, 15, 15 };
        if (n->kind == SCS_SKELETAL_MESH && anim_mesh(((const CSkeletalMesh*)n->data)->mesh)) { // (a mannequin: ~1.8 m up from its feet)
            bp_scs_actor_space(a, (int)i, false, ax, o);
            float c[3] = { o[0], o[1], o[2] + 90.0f * fabsf(ax[2][2] != 0.0f ? ax[2][2] : 1.0f) };
            grow(p, c, (float[3]){ 45, 45, 95 });
            continue;
        }
        if (!mesh && !light && !helpers) continue;
        bp_scs_actor_space(a, (int)i, false, ax, o);
        if (mesh)
            for (int k = 0; k < 3; k++) e[k] = 50.0f * (fabsf(ax[0][k]) + fabsf(ax[1][k]) + fabsf(ax[2][k]));
        grow(p, o, e);
    }
    if (p->mn[0] > p->mx[0]) // only helpers, and a thumbnail: a small box around the origin
        for (int k = 0; k < 3; k++) p->mn[k] = -25.0f, p->mx[k] = 25.0f;
    return true;
}

bool bp_preview_has_look(const BpAsset* a) {
    Parts p;
    return parts_of(a, &p, true);
}

static void frame(const BpAsset* a, BpPreviewCam* cam, bool helpers) {
    Parts p;
    float r = 60.0f;
    cam->target[0] = cam->target[1] = cam->target[2] = 0.0f;
    if (parts_of(a, &p, helpers)) {
        float e[3];
        for (int k = 0; k < 3; k++) cam->target[k] = (p.mn[k] + p.mx[k]) * 0.5f, e[k] = (p.mx[k] - p.mn[k]) * 0.5f;
        r = fmaxf(sqrtf(dot3(e, e)), 10.0f);
    }
    cam->yaw = -35.0f; // from the front left, a little above (Unreal's thumbnails and its Blueprint viewport)
    cam->pitch = -18.0f;
    cam->dist = r / sinf(FOV_DEG * 0.5f * DEG2RAD) * 1.3f;
    cam->framed = true;
}

void bp_preview_frame(const BpAsset* a, BpPreviewCam* cam) { frame(a, cam, true); }

static void draw_background(ImDrawList* dl, float x0, float y0, float x1, float y1, bool thumb) {
    uint32_t top = TH(TK_PREVIEW_TOP), bottom = TH(TK_PREVIEW_BOTTOM); // (the theme's preview gradient; a thumbnail the same)
    (void)thumb;
    ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, top, top, bottom, bottom);
}

static void draw_grid(ImDrawList* dl, const View* v, const BpPreviewCam* cam, float floor_z, float size) {
    float step = size < 60.0f ? 10.0f : size < 400.0f ? 50.0f : 100.0f;
    int n = 14;
    float c0 = roundf(cam->target[0] / step) * step, c1 = roundf(cam->target[1] / step) * step, ext = step * (float)n;
    for (int axis = 0; axis < 2; axis++)
        for (int i = -n; i <= n; i++) {
            float off = (float)i * step;
            bool major = ((int)lroundf(((axis ? c0 : c1) + off) / step) % 5) == 0;
            // in pieces, each fading with its distance to the middle
            for (int s = -4; s < 4; s++) {
                float t0 = (float)s / 4.0f * ext, t1 = (float)(s + 1) / 4.0f * ext;
                float fade = 1.0f - fminf(sqrtf(off * off + ((t0 + t1) * 0.5f) * ((t0 + t1) * 0.5f)) / ext, 1.0f);
                if (fade <= 0.02f) continue;
                float a[3], b[3];
                if (axis) a[0] = c0 + off, b[0] = c0 + off, a[1] = c1 + t0, b[1] = c1 + t1;
                else a[1] = c1 + off, b[1] = c1 + off, a[0] = c0 + t0, b[0] = c0 + t1;
                a[2] = b[2] = floor_z;
                uint32_t alpha = (uint32_t)((major ? 120.0f : 55.0f) * fade);
                line3(dl, v, a, b, (alpha << 24) | 0x00A0A0A0u, major ? 1.3f : 1.0f);
            }
        }
}

// A soft shadow under it: a fan from a dark middle to a clear rim, on the floor
static void contact_shadow(ImDrawList* dl, const View* v, const float c[3], float rx, float ry, float z, ImVec2 uv) {
    enum { N = 40 };
    ImDrawList_PrimReserve(dl, N * 3, N + 1);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    float mid[3] = { c[0], c[1], z };
    ImDrawList_PrimWriteVtx(dl, project(v, mid), uv, THA(TK_N0, 128));
    for (int i = 0; i < N; i++) {
        float ang = (float)i / N * 6.2831853f, p[3] = { c[0] + cosf(ang) * rx, c[1] + sinf(ang) * ry, z };
        ImDrawList_PrimWriteVtx(dl, project(v, p), uv, THEME_NONE);
    }
    for (int i = 0; i < N; i++) {
        ImDrawList_PrimWriteIdx(dl, base);
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + 1 + i));
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + 1 + (i + 1) % N));
    }
}

// A mesh at a transform (axes: rows, the actor-space direction of local X, Y, Z times the scale; o: its origin).
// outline: the silhouette's color (0: the shading's)
static void draw_mesh(ImDrawList* dl, const View* v, const CStaticMesh* m, const float ax[3][3], const float o[3], uint32_t lod,
                      ImVec2 uv, uint32_t outline) {
    const ShapeCache* s = shape(m->mesh, lod);
    if (!s || !scratch(s->nv, s->ntri)) return;
    Look L;
    memcpy(L.base, m->base_color, sizeof L.base);
    L.metal = m->metallic, L.rough = m->roughness;
    memcpy(L.emissive, m->emissive, sizeof L.emissive);
    // the key from the camera's upper left (every preview lit the same way, whatever the orbit)
    for (int k = 0; k < 3; k++) L.key_dir[k] = -v->fwd[k] * 0.55f - v->right[k] * 0.55f + v->up[k] * 0.65f;
    L.key_dir[2] += 0.35f;
    norm3(L.key_dir);
    L.key[0] = 1.75f, L.key[1] = 1.65f, L.key[2] = 1.5f;
    // normals: the inverse transpose (each axis over its squared length)
    float nax[3][3];
    for (int a = 0; a < 3; a++) {
        float l2 = dot3(ax[a], ax[a]);
        for (int k = 0; k < 3; k++) nax[a][k] = l2 > 1e-12f ? ax[a][k] / l2 : 0.0f;
    }
    for (uint32_t i = 0; i < s->nv; i++) {
        const float* q = s->v[i].pos;
        float w[3], n[3];
        for (int k = 0; k < 3; k++) {
            w[k] = o[k] + q[0] * ax[0][k] + q[1] * ax[1][k] + q[2] * ax[2][k];
            n[k] = s->n[i * 3] * nax[0][k] + s->n[i * 3 + 1] * nax[1][k] + s->n[i * 3 + 2] * nax[2][k];
        }
        norm3(n);
        float vd[3] = { v->eye[0] - w[0], v->eye[1] - w[1], v->eye[2] - w[2] }, out[3];
        norm3(vd);
        shade(&L, n, vd, out);
        g_scr[i] = project(v, w);
        g_col[i] = to_srgb8(out, 1.0f);
    }
    // facing the camera: the face's normal (mb_tri wound it outward; a mirroring transform turns it in) toward the eye
    float c01[3] = { ax[0][1] * ax[1][2] - ax[0][2] * ax[1][1], ax[0][2] * ax[1][0] - ax[0][0] * ax[1][2], ax[0][0] * ax[1][1] - ax[0][1] * ax[1][0] };
    float mirror = dot3(c01, ax[2]) < 0.0f ? -1.0f : 1.0f;
    for (uint32_t t = 0; t < s->ntri; t++) {
        const uint32_t* id = &s->idx[t * 3];
        float w[3][3];
        for (int j = 0; j < 3; j++) {
            const float* q = s->v[id[j]].pos;
            for (int k = 0; k < 3; k++) w[j][k] = o[k] + q[0] * ax[0][k] + q[1] * ax[1][k] + q[2] * ax[2][k];
        }
        float e1[3] = { w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2] }, e2[3] = { w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2] };
        float fn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float to_eye[3] = { v->eye[0] - w[0][0], v->eye[1] - w[0][1], v->eye[2] - w[0][2] };
        // (the local winding is the shape's: a positive determinant keeps it; Unreal's left-handed axes flip it once)
        g_front[t] = dot3(fn, to_eye) * mirror > 0.0f;
    }
    ImDrawList_PrimReserve(dl, (int)(s->ntri * 3), (int)s->nv);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (uint32_t i = 0; i < s->nv; i++) ImDrawList_PrimWriteVtx(dl, g_scr[i], uv, g_col[i]);
    uint32_t written = 0;
    for (uint32_t t = 0; t < s->ntri; t++) {
        if (!g_front[t]) continue;
        for (int k = 0; k < 3; k++) ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + s->idx[t * 3 + k]));
        written += 3;
    }
    ImDrawList_PrimUnreserve(dl, (int)(s->ntri * 3 - written), 0);
    // the outline: anti-aliased where a face turned to us meets one turned away (or the mesh ends)
    for (uint32_t t = 0; t < s->ntri; t++) {
        if (!g_front[t]) continue;
        for (int k = 0; k < 3; k++) {
            int32_t nb = s->adj[t * 3 + k];
            if (nb >= 0 && g_front[nb]) continue;
            uint32_t ia = s->idx[t * 3 + k], ib = s->idx[t * 3 + (k + 1) % 3];
            uint32_t avg = outline;
            if (!avg) {
                uint32_t ca = g_col[ia], cb = g_col[ib];
                avg = 0xFF000000u; // theme: literal (the average of two vertex colors, opaque)
                for (int ch = 0; ch < 3; ch++) avg |= ((((ca >> (8 * ch)) & 0xFF) + ((cb >> (8 * ch)) & 0xFF)) / 2) << (8 * ch);
            }
            ImDrawList_AddLine(dl, g_scr[ia], g_scr[ib], avg, outline ? 2.2f : 1.2f);
        }
    }
}

// col with its alpha scaled (the far side of a wire, behind the body)
static uint32_t faded(uint32_t col, float k) { return (col & 0x00FFFFFFu) | ((uint32_t)((float)(col >> 24) * k) << 24); }

static void ring(ImDrawList* dl, const View* v, const float c[3], float r, uint32_t col, float th) {
    enum { N = 32 };
    for (int i = 0; i < N; i++) {
        float a0 = (float)i / N * 6.2831853f, a1 = (float)(i + 1) / N * 6.2831853f, am = (a0 + a1) * 0.5f;
        float p0[3] = { c[0] + cosf(a0) * r, c[1] + sinf(a0) * r, c[2] }, p1[3] = { c[0] + cosf(a1) * r, c[1] + sinf(a1) * r, c[2] };
        float out[3] = { cosf(am), sinf(am), 0.0f }, to_eye[3] = { v->eye[0] - c[0], v->eye[1] - c[1], 0.0f };
        line3(dl, v, p0, p1, dot3(out, to_eye) > 0.0f ? col : faded(col, 0.3f), th); // the far half behind the body
    }
}

// Unreal's Character: the capsule (wire), the arrow (forward, +X) and the camera on its boom behind
static void draw_character(ImDrawList* dl, const View* v, const CCharacter* c, bool with_boom) {
    const uint32_t cap = THA(TK_AMBER, 216), arrow = TH(TK_BLUE_L), boom = THA(TK_RED, 200), camc = TH(TK_N14);
    float r = c->radius, h = c->half_height, zc = fmaxf(h - r, 0.0f);
    float top[3] = { 0, 0, zc }, bot[3] = { 0, 0, -zc };
    ring(dl, v, top, r, cap, 1.4f);
    ring(dl, v, bot, r, cap, 1.4f);
    // the side lines and the two caps' arcs, in the plane facing the camera
    float side[3] = { v->right[0], v->right[1], 0.0f };
    norm3(side);
    float upv[3] = { 0, 0, 1 };
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        float a[3] = { side[0] * r * (float)sgn, side[1] * r * (float)sgn, -zc }, b[3] = { a[0], a[1], zc };
        line3(dl, v, a, b, cap, 1.4f);
    }
    for (int end = -1; end <= 1; end += 2) {
        enum { N = 16 };
        for (int i = 0; i < N; i++) {
            float a0 = (float)i / N * 3.14159265f, a1 = (float)(i + 1) / N * 3.14159265f, cz = zc * (float)end;
            float p0[3], p1[3];
            for (int k = 0; k < 3; k++) {
                p0[k] = side[k] * cosf(a0) * r + upv[k] * sinf(a0) * r * (float)end;
                p1[k] = side[k] * cosf(a1) * r + upv[k] * sinf(a1) * r * (float)end;
            }
            p0[2] += cz, p1[2] += cz;
            line3(dl, v, p0, p1, cap, 1.4f);
        }
    }
    // the arrow
    float a0[3] = { r * 0.3f, 0, 0 }, a1[3] = { r * 2.0f, 0, 0 }, w0[3] = { r * 1.55f, r * 0.35f, 0 }, w1[3] = { r * 1.55f, -r * 0.35f, 0 };
    line3(dl, v, a0, a1, arrow, 2.4f);
    line3(dl, v, a1, w0, arrow, 2.4f);
    line3(dl, v, a1, w1, arrow, 2.4f);
    // the camera boom and the camera
    if (with_boom && c->camera_distance > 1.0f) { // (a class with a component tree draws its own boom)
        float pivot[3] = { 0, 0, h * 0.6f }, cam[3] = { -c->camera_distance * 0.35f, 0, h * 0.6f + 20.0f };
        line3(dl, v, pivot, cam, boom, 1.6f);
        ImVec2 s = project(v, cam);
        float sz = 18.0f;
        ui_icon(dl, ICON_CAMERA, s.x - sz * 0.5f, s.y - sz * 0.5f, sz, camc);
    }
}

// --- The component tree's helpers (Unreal's: wire shapes for collisions, the arrow, the camera, the boom) ---

#define SEL_COLOR TH(TK_ORANGE) // Unreal's selection orange
#define COLLISION_COLOR TH(TK_ORANGE)

static void unit_axes(const float ax[3][3], float u[3][3]) {
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(dot3(ax[a], ax[a]));
        for (int k = 0; k < 3; k++) u[a][k] = l > 1e-8f ? ax[a][k] / l : (a == k ? 1.0f : 0.0f);
    }
}

// p = o + x X + y Y + z Z (the node's axes, scaled)
static void at3(const float o[3], const float ax[3][3], float x, float y, float z, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = o[k] + x * ax[0][k] + y * ax[1][k] + z * ax[2][k];
}

// A circle of radius r around o in the plane of a and b (unit vectors)
static void circle3(ImDrawList* dl, const View* v, const float o[3], const float a[3], const float b[3], float r, uint32_t col, float th) {
    enum { N = 32 };
    float prev[3];
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N * 6.2831853f, p[3];
        for (int k = 0; k < 3; k++) p[k] = o[k] + (a[k] * cosf(t) + b[k] * sinf(t)) * r;
        if (i) line3(dl, v, prev, p, col, th);
        memcpy(prev, p, sizeof p);
    }
}

static void draw_node_helper(ImDrawList* dl, const View* v, const BpAsset* a, int i, bool sel, bool thumb) {
    const BpScsNode* n = &a->scs[i];
    float ax[3][3], o[3], u[3][3];
    bp_scs_actor_space(a, i, false, ax, o);
    unit_axes(ax, u);
    float th = sel ? 2.4f : 1.4f;
    switch (n->kind) {
    case SCS_POINT_LIGHT: case SCS_SPOT_LIGHT: case SCS_RECT_LIGHT: {
        const CLight* l = (const CLight*)n->data;
        uint32_t lc = to_srgb8((float[3]){ l->color[0] * 2.0f, l->color[1] * 2.0f, l->color[2] * 2.0f }, 1.0f);
        ImVec2 s = project(v, o);
        float sz = thumb ? 22.0f : 28.0f;
        ui_icon(dl, n->kind == SCS_SPOT_LIGHT ? ICON_SPOT : ICON_BULB, s.x - sz * 0.5f, s.y - sz * 0.5f, sz, sel ? SEL_COLOR : lc);
        if (n->kind == SCS_SPOT_LIGHT && !thumb) { // the cone's rim, 80 cm out along X
            float r = tanf(fminf(l->outer_cone, 80.0f) * DEG2RAD) * 80.0f, c[3];
            at3(o, u, 80.0f, 0, 0, c);
            circle3(dl, v, c, u[1], u[2], r, sel ? SEL_COLOR : THA(TK_AMBER_L, 144), th);
            for (int q = 0; q < 4; q++) {
                float t = (float)q * 1.5707963f, e[3];
                at3(c, u, 0, cosf(t) * r, sinf(t) * r, e);
                line3(dl, v, o, e, sel ? SEL_COLOR : THA(TK_AMBER_L, 144), 1.0f);
            }
        }
        break;
    }
    case SCS_CAMERA: { // the camera icon and a small frustum along X
        uint32_t c = sel ? SEL_COLOR : TH(TK_N14);
        float d = 40.0f, h = d * tanf(fminf(((const CCamera*)n->data)->fov_deg, 170.0f) * 0.5f * DEG2RAD), w2 = h, h2 = h * 0.56f;
        float cs[4][3];
        for (int q = 0; q < 4; q++) at3(o, u, d, (q & 1 ? w2 : -w2), (q & 2 ? h2 : -h2), cs[q]);
        for (int q = 0; q < 4; q++) line3(dl, v, o, cs[q], c, th);
        line3(dl, v, cs[0], cs[1], c, th), line3(dl, v, cs[1], cs[3], c, th);
        line3(dl, v, cs[3], cs[2], c, th), line3(dl, v, cs[2], cs[0], c, th);
        ImVec2 s = project(v, o);
        ui_icon(dl, ICON_CAMERA, s.x - 9.0f, s.y - 9.0f, 18.0f, c);
        break;
    }
    case SCS_SPRING_ARM: { // the boom: a red line from its start to its end
        float e[3], eax[3][3];
        bp_scs_actor_space(a, i, true, eax, e);
        uint32_t c = sel ? SEL_COLOR : THA(TK_RED, 200);
        line3(dl, v, o, e, c, sel ? 2.6f : 1.8f);
        ImDrawList_AddCircleFilled(dl, project(v, e), 3.5f, c, 12);
        break;
    }
    case SCS_BOX: case SCS_SPHERE: case SCS_CAPSULE: {
        const CCollision* q = (const CCollision*)n->data;
        uint32_t c = sel ? SEL_COLOR : COLLISION_COLOR;
        if (n->kind == SCS_BOX) {
            float p[8][3];
            for (int k = 0; k < 8; k++)
                at3(o, ax, (k & 1 ? 1 : -1) * q->extent[0], (k & 2 ? 1 : -1) * q->extent[1], (k & 4 ? 1 : -1) * q->extent[2], p[k]);
            static const int e[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (int k = 0; k < 12; k++) line3(dl, v, p[e[k][0]], p[e[k][1]], c, th);
        } else {
            float s = sqrtf(dot3(ax[0], ax[0])), r = q->extent[0] * s;
            float hh = n->kind == SCS_CAPSULE ? fmaxf(q->extent[1] * sqrtf(dot3(ax[2], ax[2])) - r, 0.0f) : 0.0f;
            for (int end = -1; end <= 1; end += 2) {
                float c2[3];
                at3(o, u, 0, 0, hh * (float)end, c2);
                circle3(dl, v, c2, u[0], u[1], r, c, th);
                if (n->kind == SCS_SPHERE) break;
            }
            if (n->kind == SCS_SPHERE) {
                circle3(dl, v, o, u[0], u[2], r, c, th);
                circle3(dl, v, o, u[1], u[2], r, c, th);
            } else {
                for (int sd = 0; sd < 4; sd++) {
                    float t = (float)sd * 1.5707963f, p0[3], p1[3];
                    at3(o, u, cosf(t) * r, sinf(t) * r, -hh, p0);
                    at3(o, u, cosf(t) * r, sinf(t) * r, hh, p1);
                    line3(dl, v, p0, p1, c, th);
                }
                for (int pl = 0; pl < 2; pl++) // the caps' arcs
                    for (int end = -1; end <= 1; end += 2) {
                        float prev[3], c2[3];
                        at3(o, u, 0, 0, hh * (float)end, c2);
                        for (int k = 0; k <= 16; k++) {
                            float t = (float)k / 16.0f * 3.14159265f, p[3];
                            for (int j = 0; j < 3; j++) p[j] = c2[j] + (u[pl][j] * cosf(t) + u[2][j] * sinf(t) * (float)end) * r;
                            if (k) line3(dl, v, prev, p, c, th);
                            memcpy(prev, p, sizeof p);
                        }
                    }
            }
        }
        break;
    }
    case SCS_ARROW: {
        const CArrow* ar = (const CArrow*)n->data;
        uint32_t c = sel ? SEL_COLOR : to_srgb8(ar->color, 1.0f);
        float L = ar->length > 1.0f ? ar->length : 80.0f, p0[3], p1[3], w0[3], w1[3];
        at3(o, u, 0, 0, 0, p0), at3(o, u, L, 0, 0, p1), at3(o, u, L * 0.8f, L * 0.12f, 0, w0), at3(o, u, L * 0.8f, -L * 0.12f, 0, w1);
        line3(dl, v, p0, p1, c, sel ? 3.0f : 2.4f);
        line3(dl, v, p1, w0, c, sel ? 3.0f : 2.4f);
        line3(dl, v, p1, w1, c, sel ? 3.0f : 2.4f);
        break;
    }
    case SCS_SCENE: {
        ImVec2 s = project(v, o);
        ImDrawList_AddCircle(dl, s, 6.0f, sel ? SEL_COLOR : TH(TK_N14), 12, th);
        break;
    }
    default: break;
    }
}

typedef struct MeshDraw {
    const CStaticMesh* m;
    float ax[3][3], o[3], depth;
    int node; // BP_SCS_ROOT: the actor's own
} MeshDraw;

// --- The GPU's picture (render_preview): each preview's ImGui texture, registered again when its picture was made
// again; the old one freed a few frames later (a frame in flight may still draw it) ---

#define GPU_TEX_MAX 8
static struct { unsigned key; uint32_t gen; int used; VkDescriptorSet set; } g_tex[GPU_TEX_MAX];
static struct { VkDescriptorSet set; int after; } g_tex_dead[16];

static ImTextureID gpu_texture(unsigned key, VkImageView iv, uint32_t gen) {
    int now = igGetFrameCount();
    for (int i = 0; i < 16; i++)
        if (g_tex_dead[i].set && now >= g_tex_dead[i].after) ImGui_ImplVulkan_RemoveTexture(g_tex_dead[i].set), g_tex_dead[i].set = VK_NULL_HANDLE;
    int k = -1, old = 0;
    for (int i = 0; i < GPU_TEX_MAX && k < 0; i++)
        if (g_tex[i].set && g_tex[i].key == key) k = i;
    for (int i = 0; i < GPU_TEX_MAX && k < 0; i++)
        if (!g_tex[i].set) k = i;
    if (k < 0) { // (the one shown longest ago)
        for (int i = 1; i < GPU_TEX_MAX; i++)
            if (g_tex[i].used < g_tex[old].used) old = i;
        k = old;
    }
    if (!g_tex[k].set || g_tex[k].key != key || g_tex[k].gen != gen) {
        if (g_tex[k].set) {
            int d = 0;
            while (d < 15 && g_tex_dead[d].set) d++;
            if (g_tex_dead[d].set) ImGui_ImplVulkan_RemoveTexture(g_tex_dead[d].set); // (all waiting: rare)
            g_tex_dead[d].set = g_tex[k].set, g_tex_dead[d].after = now + 4; // (the frames in flight and one more)
        }
        g_tex[k].set = ImGui_ImplVulkan_AddTexture(iv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_tex[k].key = key, g_tex[k].gen = gen;
    }
    g_tex[k].used = now;
    return (ImTextureID)(uintptr_t)g_tex[k].set;
}

// The view of the orbit as the GPU draws it; true when its picture was placed into the rectangle
static bool gpu_picture(ImDrawList* dl, float x0, float y0, float x1, float y1, const View* v, const BpPreviewCam* cam, float radius,
                        float floor_z, bool grid, const PreviewDraw* d, uint32_t n, const BpPreviewGpu* g) {
    static int cpu_only = -1; // REALENGINE_PREVIEW_CPU=1: the CPU drawing (the measures' before)
    if (cpu_only < 0) cpu_only = getenv("REALENGINE_PREVIEW_CPU") != NULL;
    if (!g || !g->key || cpu_only || !render_preview_available()) return false;
    ImGuiIO* io = igGetIO_Nil();
    float fs = io->DisplayFramebufferScale.x > 0.0f ? io->DisplayFramebufferScale.x : 1.0f;
    PreviewView pv;
    memset(&pv, 0, sizeof pv);
    memcpy(pv.eye, v->eye, 12), memcpy(pv.fwd, v->fwd, 12), memcpy(pv.right, v->right, 12), memcpy(pv.up, v->up, 12);
    pv.fov_deg = FOV_DEG;
    memcpy(pv.target, cam->target, 12);
    pv.radius = radius, pv.floor_z = floor_z;
    pv.w = (uint32_t)((x1 - x0) * fs), pv.h = (uint32_t)((y1 - y0) * fs);
    pv.mode = (uint32_t)g->mode;
    pv.flags = PREVIEW_FLOOR | (grid ? PREVIEW_GRID : 0u);
    pv.time = g->time;
    uint32_t gen = 0;
    float uvm[2];
    VkImageView iv = render_preview(g->key, &pv, d, n, &gen, uvm);
    if (!iv) return false;
    ImTextureID t = gpu_texture(g->key, iv, gen);
    ImDrawList_AddImage(dl, (ImTextureRef_c){ NULL, t }, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, (ImVec2){ 0, 0 }, (ImVec2){ uvm[0], uvm[1] },
                        THEME_IMAGE_TINT);
    return true;
}

void* bp_preview_texture(unsigned key, void* image_view, unsigned gen) {
    return (void*)(uintptr_t)gpu_texture(key, (VkImageView)image_view, gen);
}

static void draw_of(PreviewDraw* d, const CStaticMesh* m, const float ax[3][3], const float o[3], bool selected) {
    memset(d, 0, sizeof *d);
    d->mesh = m->mesh, d->material = m->material;
    for (int k = 0; k < 3; k++) {
        for (int a = 0; a < 3; a++) d->rows[k][a] = ax[a][k];
        d->rows[k][3] = o[k];
    }
    memcpy(d->color, m->base_color, 12), memcpy(d->emissive, m->emissive, 12);
    d->roughness = m->roughness, d->metallic = m->metallic, d->uv_scale = m->uv_scale;
    d->selected = selected;
}

// (2026-11-10, "a mesh dos personagens não tá sendo exibida nas blueprints") the Skeletal Mesh components (a Character's
// Mesh, the Mannequin) at their mesh's reference pose (each bone's bind), where the node is: the rigid parts as meshes
// (the GPU's picture and the CPU's), the skins bent by the bones on the GPU's. Before, only Static Meshes were drawn
#define SKEL_DRAW_MAX 192
#define SKEL_NODES_MAX 4
static uint32_t skel_nodes(const BpAsset* a, int selected, PreviewDraw* pd, uint32_t* npd, MeshDraw* md, uint32_t md_room) {
    static AnimXf pose[ANIM_MAX_BONES];
    static float bones[ANIM_MAX_BONES][3][4];
    static float pal[SKEL_NODES_MAX][ANIM_MAX_BONES][3][4];
    static CStaticMesh looks[SKEL_DRAW_MAX];
    uint32_t nmd = 0, nlook = 0, nnode = 0;
    for (uint32_t i = 0; i < a->nscs && nnode < SKEL_NODES_MAX; i++) {
        if (a->scs[i].kind != SCS_SKELETAL_MESH) continue;
        const SkelMesh* m = anim_mesh(((const CSkeletalMesh*)a->scs[i].data)->mesh);
        if (!m || !m->nbones) continue;
        float ax[3][3], o[3], rows[3][4];
        bp_scs_actor_space(a, (int)i, false, ax, o);
        for (int k = 0; k < 3; k++) {
            for (int c = 0; c < 3; c++) rows[k][c] = ax[c][k];
            rows[k][3] = o[k];
        }
        for (uint32_t b = 0; b < m->nbones; b++) pose[b] = m->bones[b].bind;
        anim_world(m, pose, rows, bones);
        bool sel = (int)i == selected;
        for (uint32_t k = 0; k < m->nparts && nlook < SKEL_DRAW_MAX && *npd < BP_MAX_SCS + 1 + SKEL_DRAW_MAX; k++) {
            const SkPart* sp = &m->parts[k];
            float r[3][4];
            anim_part_rows(sp, bones[sp->bone], r);
            CStaticMesh* l = &looks[nlook++];
            memset(l, 0, sizeof *l);
            l->mesh = sp->mesh, l->material = sp->material, l->roughness = sp->roughness, l->metallic = sp->metallic;
            memcpy(l->base_color, sp->color, 12);
            float pax[3][3], po[3];
            for (int c = 0; c < 3; c++) {
                for (int q = 0; q < 3; q++) pax[c][q] = r[q][c];
                po[c] = r[c][3];
            }
            draw_of(&pd[(*npd)++], l, pax, po, sel);
            if (nmd < md_room) {
                memset(&md[nmd], 0, sizeof md[nmd]);
                md[nmd].m = l, md[nmd].node = (int)i;
                memcpy(md[nmd].ax, pax, sizeof pax), memcpy(md[nmd].o, po, 12);
                nmd++;
            }
        }
        if (m->nskins) { // the skins: the palette maps the bind straight to the view's world (the node's place is in the bones)
            for (uint32_t b = 0; b < m->nbones; b++)
                for (int q = 0; q < 3; q++)
                    for (int c = 0; c < 4; c++)
                        pal[nnode][b][q][c] = bones[b][q][0] * m->bind_inv[b][0][c] + bones[b][q][1] * m->bind_inv[b][1][c] +
                                              bones[b][q][2] * m->bind_inv[b][2][c] + (c == 3 ? bones[b][q][3] : 0.0f);
            float id3[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, zero[3] = { 0, 0, 0 };
            for (uint32_t k = 0; k < m->nskins && nlook < SKEL_DRAW_MAX && *npd < BP_MAX_SCS + 1 + SKEL_DRAW_MAX; k++) {
                const SkSkin* sk = &m->skins[k];
                CStaticMesh* l = &looks[nlook++];
                memset(l, 0, sizeof *l);
                l->mesh = sk->mesh, l->material = sk->material, l->roughness = sk->roughness, l->metallic = sk->metallic;
                memcpy(l->base_color, sk->color, 12);
                PreviewDraw* d = &pd[(*npd)++];
                draw_of(d, l, id3, zero, sel);
                d->palette = (const float(*)[3][4])pal[nnode], d->nbones = m->nbones;
            }
        }
        nnode++;
    }
    return nmd;
}

static const char* const g_mode_names[PREVIEW_MODE_COUNT] = { "Lit", "Unlit", "Wireframe", "Lit Wireframe", "Lighting Only", "World Normal",
                                                              "Roughness", "Metallic", "Albedo", "Ambient Occlusion", "UV", "LOD Coloration",
                                                              "Overdraw" };
static const UiIcon g_mode_icons[PREVIEW_MODE_COUNT] = { ICON_VM_LIT, ICON_VM_UNLIT, ICON_VM_WIREFRAME, ICON_VM_WIREFRAME, ICON_VM_LIGHTING,
                                                         ICON_VM_NORMALS, ICON_VM_ROUGHNESS, ICON_VM_METALLIC, ICON_VM_TEXTURE, ICON_VM_GI,
                                                         ICON_GRID, ICON_VM_LOD, ICON_VM_SHADER_COST };

const char* bp_preview_mode_name(int mode) { return TR(g_mode_names[mode >= 0 && mode < PREVIEW_MODE_COUNT ? mode : 0]); }

bool bp_preview_mode_button(const char* id, int* mode) {
    int m = *mode >= 0 && *mode < PREVIEW_MODE_COUNT ? *mode : 0;
    bool ch = false;
    igPushID_Str(id);
    if (ui_icon_button("pv_mode", g_mode_icons[m], bp_preview_mode_name(m), TH(TK_N15), false,
                       TR("View mode: how the preview draws (Lit, Unlit, Wireframe, Lighting Only, the material's channels, AO, UV, LOD, Overdraw)")))
        igOpenPopup_Str("pv_mode_menu", 0);
    if (igBeginPopup("pv_mode_menu", 0)) {
        bool wire = render_preview_stats()->wireframe;
        for (int k = 0; k < PREVIEW_MODE_COUNT; k++) {
            if (k == PREVIEW_LIGHTING_ONLY || k == PREVIEW_AO) igSeparator();
            if (ui_menu_item(g_mode_icons[k], bp_preview_mode_name(k), NULL, k == m, wire || (k != PREVIEW_WIREFRAME && k != PREVIEW_LIT_WIREFRAME)))
                *mode = k, ch = true;
        }
        igEndPopup();
    }
    igPopID();
    return ch;
}

static int mesh_far_first(const void* x, const void* y) {
    float a = ((const MeshDraw*)x)->depth, b = ((const MeshDraw*)y)->depth;
    return a < b ? 1 : a > b ? -1 : 0;
}

static bool draw_impl(ImDrawList* dl, float x0, float y0, float x1, float y1, const BpAsset* a, BpPreviewCam* cam, unsigned flags,
                      int selected, const BpPreviewGpu* g) {
    bool thumb = (flags & BP_PREVIEW_THUMB) != 0;
    Parts p;
    if (!parts_of(a, &p, !thumb) || x1 - x0 < 4.0f || y1 - y0 < 4.0f) return false;
    if (!cam->framed) frame(a, cam, !thumb);
    ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
    View v;
    view_of(cam, x0, y0, x1, y1, &v);
    ImVec2 uv = igGetFontTexUvWhitePixel();
    float e[3] = { (p.mx[0] - p.mn[0]) * 0.5f, (p.mx[1] - p.mn[1]) * 0.5f, (p.mx[2] - p.mn[2]) * 0.5f };
    float size = sqrtf(dot3(e, e)), floor_z = p.mn[2];
    // the meshes back to front (each is convex: its own faces turned away are culled, the others are in order)
    static MeshDraw md[BP_MAX_SCS + 1 + SKEL_DRAW_MAX];
    uint32_t nm = 0;
    if (p.mesh) {
        memset(&md[nm], 0, sizeof md[nm]);
        md[nm].m = p.mesh, md[nm].node = BP_SCS_ROOT;
        for (int k = 0; k < 3; k++) md[nm].ax[k][k] = p.t->scale[k];
        nm++;
    }
    for (uint32_t i = 0; i < a->nscs; i++)
        if (a->scs[i].kind == SCS_STATIC_MESH) {
            md[nm].m = (const CStaticMesh*)a->scs[i].data, md[nm].node = (int)i;
            bp_scs_actor_space(a, (int)i, false, md[nm].ax, md[nm].o);
            nm++;
        }
    static PreviewDraw pd[BP_MAX_SCS + 1 + SKEL_DRAW_MAX];
    for (uint32_t i = 0; i < nm; i++) draw_of(&pd[i], md[i].m, md[i].ax, md[i].o, md[i].node == selected);
    uint32_t npd = nm;
    nm += skel_nodes(a, selected, pd, &npd, md + nm, (uint32_t)(sizeof md / sizeof md[0]) - nm);
    if (!thumb && gpu_picture(dl, x0, y0, x1, y1, &v, cam, size, floor_z, (flags & BP_PREVIEW_GRID) != 0, pd, npd, g)) {
        // (the GPU drew the meshes, the floor and their shadow: the helpers below go over them)
    } else {
        draw_background(dl, x0, y0, x1, y1, thumb);
        if (flags & BP_PREVIEW_GRID) draw_grid(dl, &v, cam, floor_z, size);
        if (p.mesh && p.mesh->mesh != SHAPE_PLANE) {
            float c[3] = { 0, 0, 0 };
            contact_shadow(dl, &v, c, 50.0f * fabsf(p.t->scale[0]) * 1.25f, 50.0f * fabsf(p.t->scale[1]) * 1.25f, floor_z, uv);
        }
        for (uint32_t i = 0; i < nm; i++) {
            float q[3];
            to_view(&v, md[i].o, q);
            md[i].depth = q[2];
        }
        qsort(md, nm, sizeof md[0], mesh_far_first);
        // a thumbnail's mesh is a few dozen pixels: LOD 2 (the cylinder's 12 sides) reads the same
        for (uint32_t i = 0; i < nm; i++) draw_mesh(dl, &v, md[i].m, md[i].ax, md[i].o, thumb ? 2u : 0u, uv, md[i].node == selected ? SEL_COLOR : 0u);
    }
    if (p.light) {
        uint32_t lc = to_srgb8((float[3]){ p.light->color[0] * 2.0f, p.light->color[1] * 2.0f, p.light->color[2] * 2.0f }, 1.0f);
        ImVec2 s = project(&v, (float[3]){ 0, 0, 0 });
        float sz = thumb ? (y1 - y0) * 0.35f : 34.0f;
        if (!p.mesh) ImDrawList_AddCircleFilled(dl, s, sz * 0.75f, (lc & 0x00FFFFFFu) | 0x30000000u, 32);
        if ((flags & BP_PREVIEW_HELPERS) || !p.mesh) ui_icon(dl, ICON_BULB, s.x - sz * 0.5f, s.y - sz * 0.5f, sz, lc);
    }
    if (p.chr && ((flags & BP_PREVIEW_HELPERS) || (!p.mesh && thumb))) draw_character(dl, &v, p.chr, a->nscs == 0);
    for (uint32_t i = 0; i < a->nscs; i++) {
        uint32_t k = a->scs[i].kind;
        bool light = k >= SCS_POINT_LIGHT && k <= SCS_RECT_LIGHT;
        if (k == SCS_STATIC_MESH || (thumb && !light) || (!thumb && !(flags & BP_PREVIEW_HELPERS) && !light && (int)i != selected)) continue;
        draw_node_helper(dl, &v, a, (int)i, (int)i == selected, thumb);
    }
    ImDrawList_PopClipRect(dl);
    return true;
}

bool bp_preview_draw_ex(ImDrawList* dl, float x0, float y0, float x1, float y1, const BpAsset* a, BpPreviewCam* cam, unsigned flags,
                        int selected) {
    return draw_impl(dl, x0, y0, x1, y1, a, cam, flags, selected, NULL);
}
bool bp_preview_draw_gpu(ImDrawList* dl, float x0, float y0, float x1, float y1, const BpAsset* a, BpPreviewCam* cam, unsigned flags,
                         int selected, const BpPreviewGpu* g) {
    return draw_impl(dl, x0, y0, x1, y1, a, cam, flags, selected, g);
}

bool bp_preview_draw(ImDrawList* dl, float x0, float y0, float x1, float y1, const BpAsset* a, BpPreviewCam* cam, unsigned flags) {
    return bp_preview_draw_ex(dl, x0, y0, x1, y1, a, cam, flags, BP_PREVIEW_NONE);
}

// --- Picking and the gizmo ---

static void mouse_dir(const View* v, float mx, float my, float d[3]) {
    float x = (mx - v->cx) / v->f, y = -(my - v->cy) / v->f;
    for (int k = 0; k < 3; k++) d[k] = v->fwd[k] + v->right[k] * x + v->up[k] * y;
    norm3(d);
}

// The ray against a node's box (local [-50, 50], a plane 1 cm thick): its distance, or -1
static float ray_mesh(const float eye[3], const float d[3], const float ax[3][3], const float o[3], bool plane) {
    // local coordinates: solve M x = p - o (M's columns: the axes) with the inverse by cofactors
    const float* X = ax[0];
    const float* Y = ax[1];
    const float* Z = ax[2];
    float yz[3] = { Y[1] * Z[2] - Y[2] * Z[1], Y[2] * Z[0] - Y[0] * Z[2], Y[0] * Z[1] - Y[1] * Z[0] };
    float zx[3] = { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
    float xy[3] = { X[1] * Y[2] - X[2] * Y[1], X[2] * Y[0] - X[0] * Y[2], X[0] * Y[1] - X[1] * Y[0] };
    float det = dot3(X, yz);
    if (fabsf(det) < 1e-9f) return -1.0f;
    float rel[3] = { eye[0] - o[0], eye[1] - o[1], eye[2] - o[2] };
    float lo[3] = { dot3(rel, yz) / det, dot3(rel, zx) / det, dot3(rel, xy) / det };
    float ld[3] = { dot3(d, yz) / det, dot3(d, zx) / det, dot3(d, xy) / det };
    float t0 = 0.0f, t1 = 1e30f;
    for (int k = 0; k < 3; k++) {
        float h = k == 2 && plane ? 0.5f : 50.0f;
        if (fabsf(ld[k]) < 1e-9f) {
            if (lo[k] < -h || lo[k] > h) return -1.0f;
            continue;
        }
        float ta = (-h - lo[k]) / ld[k], tb = (h - lo[k]) / ld[k];
        if (ta > tb) {
            float s = ta;
            ta = tb, tb = s;
        }
        t0 = fmaxf(t0, ta), t1 = fminf(t1, tb);
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

static float seg_dist2(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 1e-6f ? fminf(fmaxf(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f), 1.0f) : 0.0f;
    float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return ex * ex + ey * ey;
}

int bp_preview_pick(float x0, float y0, float x1, float y1, const BpAsset* a, const BpPreviewCam* cam, float mx, float my) {
    Parts p;
    if (!parts_of(a, &p, true)) return BP_PREVIEW_NONE;
    View v;
    view_of(cam, x0, y0, x1, y1, &v);
    float d[3];
    mouse_dir(&v, mx, my, d);
    int best = BP_PREVIEW_NONE;
    float best_t = 1e30f;
    ImVec2 m = { mx, my };
    // the helpers first (small: a few pixels around them win over the meshes behind)
    for (uint32_t i = 0; i < a->nscs; i++) {
        uint32_t k = a->scs[i].kind;
        if (k == SCS_STATIC_MESH) continue;
        float ax[3][3], o[3], q[3];
        bp_scs_actor_space(a, (int)i, false, ax, o);
        to_view(&v, o, q);
        if (q[2] < 1.0f) continue;
        ImVec2 s = project_view(&v, q);
        float d2 = (s.x - mx) * (s.x - mx) + (s.y - my) * (s.y - my);
        if (k == SCS_SPRING_ARM) {
            float e[3], eax[3][3];
            bp_scs_actor_space(a, (int)i, true, eax, e);
            d2 = seg_dist2(m, s, project(&v, e));
        }
        if (d2 < 12.0f * 12.0f && q[2] - 5.0f < best_t) best_t = q[2] - 5.0f, best = (int)i;
    }
    if (best != BP_PREVIEW_NONE) return best;
    if (p.mesh) {
        float ax[3][3] = { { p.t->scale[0], 0, 0 }, { 0, p.t->scale[1], 0 }, { 0, 0, p.t->scale[2] } }, o[3] = { 0, 0, 0 };
        float t = ray_mesh(v.eye, d, ax, o, p.mesh->mesh == SHAPE_PLANE);
        if (t >= 0.0f && t < best_t) best_t = t, best = BP_SCS_ROOT;
    }
    for (uint32_t i = 0; i < a->nscs; i++) {
        if (a->scs[i].kind != SCS_STATIC_MESH) continue;
        const CStaticMesh* sm = (const CStaticMesh*)a->scs[i].data;
        float ax[3][3], o[3];
        bp_scs_actor_space(a, (int)i, false, ax, o);
        float t = ray_mesh(v.eye, d, ax, o, sm->mesh == SHAPE_PLANE);
        if (t >= 0.0f && t < best_t) best_t = t, best = (int)i;
    }
    if (best == BP_PREVIEW_NONE && p.chr) best = BP_SCS_ROOT; // the capsule: the root
    return best;
}

static const uint32_t g_axis_col[3] = { TK_RED, TK_GREEN, TK_BLUE };
#define HOT_COLOR TH(TK_AMBER)

// The handles of a node: the parent's axes (moving and turning work in the parent's space, where the values live), the
// node's own for scale; L: the gizmo's length in cm (70 px on screen)
typedef struct Handles {
    float o[3], pu[3][3], nu[3][3], L;
    ImVec2 so;
} Handles;

static bool handles_of(const View* v, const BpAsset* a, int node, Handles* h) {
    float pax[3][3], po[3], nax[3][3];
    bp_scs_actor_space(a, a->scs[node].parent, true, pax, po);
    bp_scs_actor_space(a, node, false, nax, h->o);
    unit_axes(pax, h->pu);
    unit_axes(nax, h->nu);
    float q[3];
    to_view(v, h->o, q);
    if (q[2] < 1.0f) return false;
    h->L = 70.0f * q[2] / v->f;
    h->so = project_view(v, q);
    return true;
}

// The handle under the mouse: 0..2 an axis, 3 the middle (uniform scale), -1 none
static int gizmo_hot(const View* v, const Handles* h, int mode, ImVec2 m) {
    int best = -1;
    float best_d = 8.0f * 8.0f;
    if (mode == 2) {
        float dx = m.x - h->so.x, dy = m.y - h->so.y;
        if (dx * dx + dy * dy < 9.0f * 9.0f) return 3;
    }
    for (int k = 0; k < 3; k++) {
        if (mode == 1) { // the rings
            const float* b1 = h->pu[(k + 1) % 3];
            const float* b2 = h->pu[(k + 2) % 3];
            ImVec2 prev = { 0, 0 };
            for (int i = 0; i <= 32; i++) {
                float t = (float)i / 32.0f * 6.2831853f, p[3];
                for (int j = 0; j < 3; j++) p[j] = h->o[j] + (b1[j] * cosf(t) + b2[j] * sinf(t)) * h->L;
                ImVec2 s = project(v, p);
                if (i) {
                    float d2 = seg_dist2(m, prev, s);
                    if (d2 < best_d) best_d = d2, best = k;
                }
                prev = s;
            }
        } else {
            const float* dir = mode == 2 ? h->nu[k] : h->pu[k];
            float e[3] = { h->o[0] + dir[0] * h->L, h->o[1] + dir[1] * h->L, h->o[2] + dir[2] * h->L };
            float d2 = seg_dist2(m, h->so, project(v, e));
            if (d2 < best_d) best_d = d2, best = k;
        }
    }
    return best;
}

static void gizmo_draw(ImDrawList* dl, const View* v, const Handles* h, int mode, int hot) {
    for (int k = 0; k < 3; k++) {
        uint32_t c = k == hot ? HOT_COLOR : TH(g_axis_col[k]);
        if (mode == 1) {
            circle3(dl, v, h->o, h->pu[(k + 1) % 3], h->pu[(k + 2) % 3], h->L, c, k == hot ? 3.0f : 2.0f);
            continue;
        }
        const float* dir = mode == 2 ? h->nu[k] : h->pu[k];
        float e[3] = { h->o[0] + dir[0] * h->L, h->o[1] + dir[1] * h->L, h->o[2] + dir[2] * h->L };
        ImVec2 se = project(v, e);
        ImDrawList_AddLine(dl, h->so, se, c, k == hot ? 3.5f : 2.5f);
        if (mode == 2) ImDrawList_AddRectFilled(dl, (ImVec2){ se.x - 5, se.y - 5 }, (ImVec2){ se.x + 5, se.y + 5 }, c, 0, 0);
        else { // the arrowhead
            float dx = se.x - h->so.x, dy = se.y - h->so.y, l = sqrtf(dx * dx + dy * dy);
            if (l > 1.0f) {
                dx /= l, dy /= l;
                ImDrawList_AddTriangleFilled(dl, (ImVec2){ se.x + dx * 10, se.y + dy * 10 }, (ImVec2){ se.x - dy * 5, se.y + dx * 5 },
                                             (ImVec2){ se.x + dy * 5, se.y - dx * 5 }, c);
            }
        }
    }
    if (mode == 2) ImDrawList_AddRectFilled(dl, (ImVec2){ h->so.x - 5, h->so.y - 5 }, (ImVec2){ h->so.x + 5, h->so.y + 5 }, hot == 3 ? HOT_COLOR : TH(TK_N14), 0, 0);
}

static float snapf(float x, float step) { return step > 0.0f ? roundf(x / step) * step : x; }

bool bp_preview_gizmo(ImDrawList* dl, float x0, float y0, float x1, float y1, BpAsset* a, const BpPreviewCam* cam, int node, int mode,
                      BpGizmo* g, bool* changed) {
    *changed = false;
    if (node < 0 || node >= (int)a->nscs || mode < 0 || mode > 2) {
        g->dragging = false;
        return false;
    }
    View v;
    view_of(cam, x0, y0, x1, y1, &v);
    Handles h;
    if (!handles_of(&v, a, node, &h)) return false;
    ImGuiIO* io = igGetIO_Nil();
    ImVec2 m = io->MousePos;
    bool inside = m.x >= x0 && m.x < x1 && m.y >= y0 && m.y < y1;
    BpScsNode* n = &a->scs[node];
    int hot = g->dragging ? g->axis : (inside ? gizmo_hot(&v, &h, mode, m) : -1);
    if (!g->dragging && hot >= 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) && igIsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        g->dragging = true;
        g->axis = hot;
        g->mode = mode;
        g->start_mouse[0] = m.x, g->start_mouse[1] = m.y;
        memcpy(g->start_loc, n->location, 12);
        memcpy(g->start_rot, n->rotation, 12);
        memcpy(g->start_scale, n->scale, 12);
        g->start_angle = atan2f(m.y - h.so.y, m.x - h.so.x);
    }
    if (g->dragging && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) g->dragging = false;
    if (g->dragging && g->mode == mode) {
        bool snap = io->KeyCtrl; // Ctrl: Unreal's grid snapping (10 cm, 15 degrees, 0.1)
        float dmx = m.x - g->start_mouse[0], dmy = m.y - g->start_mouse[1];
        int k = g->axis;
        if (mode == 0 || (mode == 2 && k < 3)) {
            const float* dir = mode == 2 ? h.nu[k] : h.pu[k];
            float e[3] = { h.o[0] + dir[0] * h.L, h.o[1] + dir[1] * h.L, h.o[2] + dir[2] * h.L };
            ImVec2 se = project(&v, e);
            float sx = se.x - h.so.x, sy = se.y - h.so.y, sl = sqrtf(sx * sx + sy * sy);
            float px = sl > 1e-3f ? (dmx * sx + dmy * sy) / sl : 0.0f; // pixels along the axis
            if (mode == 0) {
                // the parent's axis a (scaled by the parent): the value moves amount / the parent's scale on it
                float pax[3][3], po[3];
                bp_scs_actor_space(a, n->parent, true, pax, po);
                float ps = sqrtf(dot3(pax[k], pax[k]));
                float amount = sl > 1e-3f ? px * h.L / sl : 0.0f;
                memcpy(n->location, g->start_loc, 12);
                n->location[k] = g->start_loc[k] + amount / (ps > 1e-6f ? ps : 1.0f);
                if (snap) n->location[k] = snapf(n->location[k], 10.0f);
            } else {
                memcpy(n->scale, g->start_scale, 12);
                n->scale[k] = g->start_scale[k] * fmaxf(1.0f + px / 100.0f, 0.01f);
                if (snap) n->scale[k] = fmaxf(snapf(n->scale[k], 0.1f), 0.1f);
            }
        } else if (mode == 2) { // the middle: all three
            float f = fmaxf(1.0f + (dmx - dmy) / 150.0f, 0.01f);
            for (int j = 0; j < 3; j++) n->scale[j] = g->start_scale[j] * f;
            if (snap)
                for (int j = 0; j < 3; j++) n->scale[j] = fmaxf(snapf(n->scale[j], 0.1f), 0.1f);
        } else { // a ring: the angle the mouse went around the middle, its sign as the ring turns on screen
            float ang = atan2f(m.y - h.so.y, m.x - h.so.x) - g->start_angle;
            const float* b1 = h.pu[(k + 1) % 3];
            const float* b2 = h.pu[(k + 2) % 3];
            float t0 = 0.3f, p0[3], p1[3];
            for (int j = 0; j < 3; j++) {
                p0[j] = h.o[j] + (b1[j] * cosf(t0) + b2[j] * sinf(t0)) * h.L;
                p1[j] = h.o[j] + (b1[j] * cosf(t0 + 0.05f) + b2[j] * sinf(t0 + 0.05f)) * h.L;
            }
            ImVec2 s0 = project(&v, p0), s1 = project(&v, p1);
            float c0 = (s0.x - h.so.x) * (s1.y - h.so.y) - (s0.y - h.so.y) * (s1.x - h.so.x); // > 0: + turns clockwise on screen
            float theta = (c0 >= 0.0f ? ang : -ang);
            if (snap) theta = snapf(theta / DEG2RAD, 15.0f) * DEG2RAD;
            // the start's axes (in the parent's space) turned about the parent's axis k
            float R[3][3];
            rotator_axes(g->start_rot, R);
            int i1 = (k + 1) % 3, i2 = (k + 2) % 3;
            float c = cosf(theta), s = sinf(theta);
            for (int r = 0; r < 3; r++) {
                float x = R[r][i1], y = R[r][i2];
                R[r][i1] = x * c - y * s;
                R[r][i2] = x * s + y * c;
            }
            axes_rotator(R, n->rotation);
        }
        *changed = true;
        if (!handles_of(&v, a, node, &h)) return true;
    }
    gizmo_draw(dl, &v, &h, mode, hot);
    return g->dragging || hot >= 0;
}

// --- Any set of shapes (the animation editors: a skeletal mesh's parts at a pose, its bones as lines) ---

void bp_preview_frame_bounds(BpPreviewCam* cam, const float mn[3], const float mx[3]) {
    float e[3];
    for (int k = 0; k < 3; k++) cam->target[k] = (mn[k] + mx[k]) * 0.5f, e[k] = (mx[k] - mn[k]) * 0.5f;
    float r = fmaxf(sqrtf(dot3(e, e)), 10.0f);
    cam->yaw = -145.0f; // from the front (the meshes face +X), a little to the left and above
    cam->pitch = -12.0f;
    cam->dist = r / sinf(FOV_DEG * 0.5f * DEG2RAD) * 1.15f;
    cam->framed = true;
}

static void shapes_impl(ImDrawList* dl, float x0, float y0, float x1, float y1, BpPreviewCam* cam, const BpPreviewShape* s, uint32_t n,
                        const float (*lines)[6], uint32_t nlines, int selected_line, bool grid, const BpPreviewGpu* g) {
    if (x1 - x0 < 4.0f || y1 - y0 < 4.0f) return;
    ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
    View v;
    view_of(cam, x0, y0, x1, y1, &v);
    ImVec2 uv = igGetFontTexUvWhitePixel();
    enum { MAXS = 256 };
    MeshDraw md[MAXS];
    uint32_t nm = 0;
    static CStaticMesh looks[MAXS];
    static PreviewDraw pd[MAXS];
    // the floor under the lowest part (the GPU's: its bounds; never above 0, where the feet stand)
    float floor_z = 1e30f, gmn[3] = { 1e30f, 1e30f, 1e30f }, gmx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n && nm < MAXS; i++) {
        CStaticMesh* l = &looks[nm];
        memset(l, 0, sizeof *l);
        l->mesh = s[i].shape, l->material = s[i].material;
        memcpy(l->base_color, s[i].color, 12);
        l->roughness = s[i].roughness, l->metallic = s[i].metallic;
        md[nm].m = l, md[nm].node = s[i].selected ? 1 : 0;
        memcpy(md[nm].ax, s[i].ax, sizeof md[nm].ax);
        memcpy(md[nm].o, s[i].o, 12);
        float q[3];
        to_view(&v, s[i].o, q);
        md[nm].depth = q[2];
        floor_z = fminf(floor_z, s[i].o[2] - 2.0f);
        draw_of(&pd[nm], l, s[i].ax, s[i].o, s[i].selected);
        pd[nm].palette = s[i].palette, pd[nm].nbones = s[i].nbones; // (a skin: bent at the pose)
        float lmn[3], lmx[3];
        render_mesh_bounds(l->mesh, lmn, lmx);
        for (int k = 0; k < 3; k++) {
            float c = s[i].o[k], e = 0.0f;
            for (int a = 0; a < 3; a++) c += s[i].ax[a][k] * (lmn[a] + lmx[a]) * 0.5f, e += fabsf(s[i].ax[a][k]) * (lmx[a] - lmn[a]) * 0.5f;
            gmn[k] = fminf(gmn[k], c - e), gmx[k] = fmaxf(gmx[k], c + e);
        }
        nm++;
    }
    if (!n) floor_z = 0.0f, gmn[2] = 0.0f;
    float ge[3] = { (gmx[0] - gmn[0]) * 0.5f, (gmx[1] - gmn[1]) * 0.5f, (gmx[2] - gmn[2]) * 0.5f };
    float radius = n ? sqrtf(dot3(ge, ge)) : 50.0f;
    if (!gpu_picture(dl, x0, y0, x1, y1, &v, cam, radius, fminf(gmn[2], 0.0f), grid, pd, nm, g)) {
        draw_background(dl, x0, y0, x1, y1, false);
        if (grid) draw_grid(dl, &v, cam, fminf(floor_z, 0.0f), 300.0f);
        contact_shadow(dl, &v, cam->target, 45.0f, 45.0f, fminf(floor_z, 0.0f), uv);
        qsort(md, nm, sizeof md[0], mesh_far_first);
        for (uint32_t i = 0; i < nm; i++) draw_mesh(dl, &v, md[i].m, md[i].ax, md[i].o, 0u, uv, md[i].node ? SEL_COLOR : 0u);
    }
    for (uint32_t i = 0; i < nlines; i++) { // the bones: over everything (Unreal's skeleton view)
        uint32_t c = (int)i == selected_line ? SEL_COLOR : THA(TK_N15, 224);
        line3(dl, &v, lines[i], lines[i] + 3, c, (int)i == selected_line ? 2.5f : 1.3f);
        ImDrawList_AddCircleFilled(dl, project(&v, lines[i] + 3), (int)i == selected_line ? 4.0f : 2.5f, c, 8);
    }
    ImDrawList_PopClipRect(dl);
}

void bp_preview_shapes(ImDrawList* dl, float x0, float y0, float x1, float y1, BpPreviewCam* cam, const BpPreviewShape* s, uint32_t n,
                       const float (*lines)[6], uint32_t nlines, int selected_line, bool grid) {
    shapes_impl(dl, x0, y0, x1, y1, cam, s, n, lines, nlines, selected_line, grid, NULL);
}
void bp_preview_shapes_gpu(ImDrawList* dl, float x0, float y0, float x1, float y1, BpPreviewCam* cam, const BpPreviewShape* s, uint32_t n,
                           const float (*lines)[6], uint32_t nlines, int selected_line, bool grid, const BpPreviewGpu* g) {
    shapes_impl(dl, x0, y0, x1, y1, cam, s, n, lines, nlines, selected_line, grid, g);
}
