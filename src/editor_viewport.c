// The viewport: the camera (Unreal's controls: right mouse + WASD/QE flies, the wheel changes the speed or dollies,
// middle mouse pans, F focuses), the scene rendered by render.c behind the UI, and on top of it the editor's
// overlays: icons of the actors without a mesh (lights, cameras, player starts), the selected light's shape, the
// gizmo (W move, E rotate, R scale; world space like Unreal's default, snapping from the viewport toolbar), picking,
// and assets dropped from the Content Browser.
#include "script.h"
#define THEME_SCENE // (overlays on the 3D scene: theme.h)
#include "theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <GLFW/glfw3.h>

#include "blueprint.h"
#include "ai.h"
#include "anim.h"
#include "particles.h"
#include "vehicle.h"
#include "content.h"
#include "material.h"
#include "movie_render.h"
#include "editor.h"
#include "ui_widget.h"
#include "save_progress.h"
#include "landscape.h"
#include "navmesh.h"
#include "foliage.h"
#include "post.h"
#include "gi_cache.h"
#include "i18n.h"
#include "ui_icons.h"
#include "interaction.h"
#include "quest.h"
#include "pcg.h"
#include "hud.h"
#include "spline.h"

#define DEG2RAD 0.017453292519943295f
#define NEAR_CM 1.0f

enum {
    GIZMO_NONE = 0,
    GIZMO_X, GIZMO_Y, GIZMO_Z,     // axes: move arrows, rotate rings, scale boxes
    GIZMO_YZ, GIZMO_ZX, GIZMO_XY,  // plane squares (named by the plane; normal X, Y, Z): move, scale
    GIZMO_VIEW,                    // move: the center ball (the view plane); rotate: the outer ring; scale: uniform
    GIZMO_TRACKBALL,               // (2026-11-23) rotate: inside the rings, a free turn (the Tlender's)
};

// The view: perspective (focal: pixels per unit of tangent), or orthographic (ppc > 0: pixels per cm; pos is the point
// looked at, the view reaches ORTHO_BACK cm behind it). ro: the last mouse ray's origin (mouse_ray sets it: an
// orthographic view's rays start at the pixel, not at a point)
#define ORTHO_BACK RENDER_ORTHO_BACK
typedef struct View {
    float pos[3], fwd[3], right[3], up[3];
    float cx, cy, focal;
    float ppc;
    float ro[3];
} View;


static void camera_basis(const EdCamera* c, float fwd[3], float right[3], float up[3]) { level_camera_basis(c, fwd, right, up); }

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static void normalize3(float* v) {
    float l = sqrtf(dot3(v, v));
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}

static void to_view(const View* v, const float p[3], float out[3]) {
    float d[3] = { p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2] };
    out[0] = dot3(d, v->right);
    out[1] = dot3(d, v->up);
    out[2] = dot3(d, v->fwd);
}
static ImVec2 project_view(const View* v, const float q[3]) {
    if (v->ppc > 0.0f) return (ImVec2){ v->cx + q[0] * v->ppc, v->cy - q[1] * v->ppc };
    return (ImVec2){ v->cx + q[0] / q[2] * v->focal, v->cy - q[1] / q[2] * v->focal };
}
static bool project_point(const View* v, const float p[3], ImVec2* out) {
    float q[3];
    to_view(v, p, q);
    if (q[2] < (v->ppc > 0.0f ? -ORTHO_BACK : NEAR_CM)) return false;
    *out = project_view(v, q);
    return true;
}

// A 3D segment clipped to the near plane, then projected
static void line3(ImDrawList* dl, const View* v, const float a[3], const float b[3], ImU32 col, float thick) {
    float qa[3], qb[3];
    to_view(v, a, qa);
    to_view(v, b, qb);
    if (v->ppc > 0.0f) { // (orthographic: nothing to clip)
        ImDrawList_AddLine(dl, project_view(v, qa), project_view(v, qb), col, thick);
        return;
    }
    if (qa[2] < NEAR_CM && qb[2] < NEAR_CM) return;
    if (qa[2] < NEAR_CM || qb[2] < NEAR_CM) {
        float t = (NEAR_CM - qa[2]) / (qb[2] - qa[2]);
        float* behind = qa[2] < NEAR_CM ? qa : qb;
        for (int k = 0; k < 3; k++) behind[k] = qa[k] + (qb[k] - qa[k]) * t;
        behind[2] = NEAR_CM;
    }
    ImDrawList_AddLine(dl, project_view(v, qa), project_view(v, qb), col, thick);
}

// The mouse's ray: its direction, its origin into v->ro (the camera; orthographic: the pixel's point, ORTHO_BACK
// behind the plane looked at). (The View is this frame's, made by viewport_draw: the origin is written through it.)
static void mouse_ray(const View* v, ImVec2 m, float dir[3]) {
    float* ro = ((View*)v)->ro;
    if (v->ppc > 0.0f) {
        float x = (m.x - v->cx) / v->ppc, y = (v->cy - m.y) / v->ppc;
        for (int k = 0; k < 3; k++) dir[k] = v->fwd[k], ro[k] = v->pos[k] + v->right[k] * x + v->up[k] * y - v->fwd[k] * ORTHO_BACK;
        return;
    }
    for (int k = 0; k < 3; k++) dir[k] = v->fwd[k] + v->right[k] * (m.x - v->cx) / v->focal + v->up[k] * (v->cy - m.y) / v->focal;
    normalize3(dir);
    memcpy(ro, v->pos, sizeof v->ro);
}

// A circle of radius r around c in the plane of a and b (unit, perpendicular)
static void circle3(ImDrawList* dl, const View* v, const float c[3], const float a[3], const float b[3], float r, ImU32 col,
                    float thick) {
    float prev[3];
    for (int i = 0; i <= 48; i++) {
        float t = (float)i / 48.0f * 6.2831853f, p[3];
        for (int k = 0; k < 3; k++) p[k] = c[k] + (a[k] * cosf(t) + b[k] * sinf(t)) * r;
        if (i) line3(dl, v, prev, p, col, thick);
        memcpy(prev, p, sizeof prev);
    }
}

// --- Picking: ray against each mesh's oriented box, icons by their distance on screen ---

static bool ray_obb(const float o[3], const float d[3], const CTransform* t, const float mn[3], const float mx[3], float* hit) {
    float m[3][3];
    rotator_axes(t->rotation, m);
    float rel[3] = { o[0] - t->location[0], o[1] - t->location[1], o[2] - t->location[2] };
    float t0 = 0.0f, t1 = 1e30f;
    for (int a = 0; a < 3; a++) {
        float s = fabsf(t->scale[a]) > 1e-6f ? t->scale[a] : 1e-6f;
        float lo = dot3(rel, m[a]) / s, ld = dot3(d, m[a]) / s;
        float lo_b = fminf(mn[a], mx[a]), hi_b = fmaxf(mn[a], mx[a]);
        if (fabsf(ld) < 1e-9f) {
            if (lo < lo_b || lo > hi_b) return false;
            continue;
        }
        float ta = (lo_b - lo) / ld, tb = (hi_b - lo) / ld;
        if (ta > tb) { float x = ta; ta = tb; tb = x; }
        t0 = fmaxf(t0, ta);
        t1 = fminf(t1, tb);
        if (t0 > t1) return false;
    }
    *hit = t0;
    return true;
}

// (2026-11-10, "seleciona o que está atrás") the click's truth: the nearest triangle of the mesh the ray meets, in the
// mesh's space (the same t as the world's: the ray is mapped linearly, as ray_obb does). The box only says it may hit:
// a big box (a Plane scaled 208 is 2 m thick, a wall, a volume the camera is in) took the clicks meant for what stands
// in front of it. 1: hit (*hit), 0: the ray passes the box's empty space, -1: no CPU copy (the box's answer stands)
static int ray_mesh_tris(const float o[3], const float d[3], const CTransform* t, uint32_t mesh, float* hit) {
    const GpuVertex* v = NULL;
    const uint32_t* idx = NULL;
    uint32_t fi = 0, ic = 0;
    int32_t vo = 0;
    render_mesh_cpu(mesh, &v, &idx, &fi, &ic, &vo);
    if (!v || !idx || !ic || ic > 6000000u) return -1;
    float m[3][3], lo[3], ld[3];
    rotator_axes(t->rotation, m);
    float rel[3] = { o[0] - t->location[0], o[1] - t->location[1], o[2] - t->location[2] };
    for (int a = 0; a < 3; a++) {
        float s = fabsf(t->scale[a]) > 1e-6f ? t->scale[a] : 1e-6f;
        lo[a] = dot3(rel, m[a]) / s, ld[a] = dot3(d, m[a]) / s;
    }
    float best = 1e30f;
    for (uint32_t k = fi; k + 2 < fi + ic; k += 3) { // Moller-Trumbore, both faces
        const float* p0 = v[(int64_t)idx[k] + vo].pos;
        const float* p1 = v[(int64_t)idx[k + 1] + vo].pos;
        const float* p2 = v[(int64_t)idx[k + 2] + vo].pos;
        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float pv[3] = { ld[1] * e2[2] - ld[2] * e2[1], ld[2] * e2[0] - ld[0] * e2[2], ld[0] * e2[1] - ld[1] * e2[0] };
        float det = dot3(e1, pv);
        if (fabsf(det) < 1e-12f) continue;
        float inv = 1.0f / det, tv[3] = { lo[0] - p0[0], lo[1] - p0[1], lo[2] - p0[2] };
        float u = dot3(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        float q[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
        float vv = dot3(ld, q) * inv;
        if (vv < 0.0f || u + vv > 1.0f) continue;
        float tt = dot3(e2, q) * inv;
        if (tt > 0.0f && tt < best) best = tt;
    }
    if (best >= 1e30f) return 0;
    *hit = best;
    return 1;
}

// Its icon hidden: the actor's own Show Icon (Details), or its owner's when it is a part of one
static bool icon_off(World* w, Entity e, const CTransform* t) {
    if (t->flags & TRANSFORM_NO_ICON) return true;
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    const CTransform* ot = at ? ecs_get(w, at->owner, COMP_TRANSFORM) : NULL;
    return ot && (ot->flags & TRANSFORM_NO_ICON);
}

static UiIcon actor_icon(World* w, Entity e) {
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    if (l) {
        static const UiIcon li[LIGHT_TYPE_COUNT] = { ICON_SUN, ICON_BULB, ICON_SPOT, ICON_RECT, ICON_SKY };
        return li[l->type < LIGHT_TYPE_COUNT ? l->type : 1];
    }
    if (ecs_get(w, e, COMP_CAMERA)) return ICON_CAMERA;
    if (ecs_get(w, e, COMP_PLAYER_START)) return ICON_PLAYER;
    if (ecs_get(w, e, COMP_COLLISION)) return ICON_CUBE; // (a Trigger)
    if (ecs_get(w, e, COMP_POST_PROCESS)) return ICON_EXPOSURE;
    if (ecs_get(w, e, COMP_DECAL)) return ICON_EXPOSURE;
    if (ecs_get(w, e, COMP_NAV)) return ICON_MOVEMENT;
    if (ecs_get(w, e, COMP_CHECKPOINT)) return ICON_PLAYER; // (a Checkpoint: where the player comes back)
    return ICON_ACTOR;
}

static bool icon_shown(const View* v, const float p[3]); // (the icons' grid, below)

Entity viewport_pick_ray(Editor* ed, const float o[3], const float d[3]) {
    World* w = &ed->world;
    Entity best = ENTITY_NONE;
    float best_t = 1e30f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || !(a->mask & COMP_BIT(COMP_STATIC_MESH))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (tr[i].flags & TRANSFORM_HIDDEN) continue;
            float mn[3], mx[3], t;
            render_mesh_bounds(sm[i].mesh, mn, mx);
            if (ray_obb(o, d, &tr[i], mn, mx, &t) && t < best_t) best_t = t, best = bp_component_owner(w, ents[i]);
        }
    }
    return best;
}

// --- Overlays ---

// The icons of the actors without a mesh (lights, cameras, player starts). Up to ICON_ALL of them, all are drawn; past
// that (a stress level, a city of street lamps) only those within ICON_DIST of the camera, found in a grid of 32 m
// columns rebuilt when the world changes (its change log): projecting a million each frame took the editor to 1 fps.
#define ICON_ALL 4096
#define ICON_DIST 5000.0f // cm
#define ICON_CELL 3200.0f
typedef struct IconEntry {
    uint64_t key; // the column: x << 32 | y
    uint32_t index;
} IconEntry;
static struct {
    IconEntry* e;
    float* ground;      // per icon: the z under it (the meshes, or the floor at 0): its line down and its shadow
    uint8_t* occluded;  // per icon: hidden behind a mesh from the camera (drawn faint)
    uint32_t n, cap, revision, changes, next_occ;
    bool valid;
} g_icons;

// The nearest mesh a ray meets within max_t (cm; 1e30: none). Many meshes: none tested (the icons keep the floor at 0)
static float ray_meshes(World* w, const float o[3], const float d[3], float max_t) {
    float best = 1e30f;
    uint32_t meshes = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].mask & COMP_BIT(COMP_STATIC_MESH)) meshes += w->archetypes[ai].count;
    if (meshes > 20000) return best;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        // (a landscape's chunks and plants: its ground below, not thousands of boxes)
        const CAttach* at = (a->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(a, COMP_ATTACH) : NULL;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        for (uint32_t i = 0; i < a->count; i++) {
            if ((tr[i].flags & TRANSFORM_HIDDEN) || (at && (at[i].kind == ATTACH_LANDSCAPE || at[i].kind == ATTACH_FOLIAGE))) continue;
            float mn[3], mx[3], t;
            render_mesh_bounds(sm[i].mesh, mn, mx);
            if (ray_obb(o, d, &tr[i], mn, mx, &t) && t > 0.5f && t < best && t < max_t) best = t;
        }
    }
    float far = fminf(max_t, 1.0e6f), end[3] = { o[0] + d[0] * far, o[1] + d[1] * far, o[2] + d[2] * far };
    float lt = landscape_trace(o, end);
    if (lt < 1.0f && lt * far < best) best = lt * far;
    return best;
}

static uint64_t icon_key(int32_t x, int32_t y) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y; }
static int icon_cmp(const void* a, const void* b) {
    uint64_t x = ((const IconEntry*)a)->key, y = ((const IconEntry*)b)->key;
    return x < y ? -1 : x > y;
}

static void icons_update(World* w) {
    if (g_icons.valid && g_icons.revision == w->revision && g_icons.changes == w->changes) return;
    g_icons.n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        // (a Blueprint's components: drawn by their own helpers, picked as their actor)
        // (an imported Static Mesh's actor too: its icon is its model, 2026-11-08; the basic shapes have none)
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        const CStaticMesh* sms = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            if (sms && (render_mesh_slot(sms[i].mesh) < SHAPE_COUNT || getenv("REALENGINE_NO_MESH_ICONS"))) continue;
            if (g_icons.n == g_icons.cap) { // the world grew: an edit, not a frame
                uint32_t c = g_icons.cap ? g_icons.cap * 2 : 1024;
                IconEntry* e = realloc(g_icons.e, sizeof(IconEntry) * c);
                if (!e) return;
                g_icons.e = e;
                g_icons.cap = c;
            }
            g_icons.e[g_icons.n++] = (IconEntry){ icon_key((int32_t)floorf(tr[i].location[0] / ICON_CELL),
                                                           (int32_t)floorf(tr[i].location[1] / ICON_CELL)),
                                                  entity_index(ents[i]) };
        }
    }
    qsort(g_icons.e, g_icons.n, sizeof(IconEntry), icon_cmp);
    // the ground under each (the first 512: the rest stand on the floor at 0)
    float* gz = realloc(g_icons.ground, sizeof(float) * (g_icons.cap ? g_icons.cap : 1));
    uint8_t* oc = realloc(g_icons.occluded, g_icons.cap ? g_icons.cap : 1);
    if (gz) g_icons.ground = gz;
    if (oc) g_icons.occluded = oc;
    for (uint32_t i = 0; gz && oc && i < g_icons.n; i++) {
        Entity e = ecs_entity_at(w, g_icons.e[i].index);
        const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        float down[3] = { 0, 0, -1 };
        float hit = t && i < 512 ? ray_meshes(w, t->location, down, 1e6f) : 1e30f;
        gz[i] = t ? (hit < 1e29f ? t->location[2] - hit : fminf(t->location[2], 0.0f)) : 0.0f;
        oc[i] = 0;
    }
    g_icons.revision = w->revision;
    g_icons.changes = w->changes;
    g_icons.valid = true;
}


static Entity pick(Editor* ed, const View* v, ImVec2 m) {
    World* w = &ed->world;
    float dir[3];
    mouse_ray(v, m, dir);
    Entity best = ENTITY_NONE;
    float best_t = 1e30f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        bool mesh = (a->mask & COMP_BIT(COMP_STATIC_MESH)) != 0;
        const CStaticMesh* sm = mesh ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            if (tr[i].flags & TRANSFORM_HIDDEN) continue;
            float t;
            if (mesh) {
                float mn[3], mx[3];
                render_mesh_bounds(sm[i].mesh, mn, mx);
                if (!ray_obb(v->ro, dir, &tr[i], mn, mx, &t) || t >= best_t) continue; // (its box first: cheap, and no nearer)
                float tt;
                int r = ray_mesh_tris(v->ro, dir, &tr[i], sm[i].mesh, &tt);
                if (r == 0) continue; // (through the box's empty space)
                if (r == 1) t = tt;
                if (t < best_t) best_t = t, best = bp_component_owner(w, ents[i]);
            }
        }
    }
    // the icons are drawn over the scene: a click inside one picks it (the nearest to the camera when they overlap), at
    // the size it is drawn (a circle, or a model's card for a mesh's actor), whatever mesh stands in front of its point
    if (ed->show_icons && !ed->game_view) {
        icons_update(w);
        float s = igGetFontSize() * 1.9f, icon_t = 1e30f;
        Entity icon = ENTITY_NONE;
        for (uint32_t k = 0; k < g_icons.n; k++) {
            Entity e = ecs_entity_at(w, g_icons.e[k].index);
            const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            if (!t || (t->flags & TRANSFORM_HIDDEN) || icon_off(w, e, t) || !icon_shown(v, t->location)) continue;
            ImVec2 p;
            if (!project_point(v, t->location, &p)) continue;
            bool card = ecs_get(w, e, COMP_SKELETAL_MESH) || ecs_get(w, e, COMP_STATIC_MESH);
            float r = card ? s * 0.95f : s * 0.62f + 2.0f, dx = p.x - m.x, dy = p.y - m.y;
            if (card ? fabsf(dx) > r || fabsf(dy) > r : dx * dx + dy * dy > r * r) continue;
            float d[3] = { t->location[0] - v->ro[0], t->location[1] - v->ro[1], t->location[2] - v->ro[2] };
            float dist = sqrtf(dot3(d, d));
            if (dist < icon_t) icon_t = dist, icon = e;
        }
        if (icon != ENTITY_NONE) return icon;
    }
    return best;
}

// Whether an icon at p is shown (and can be clicked)
static bool icon_shown(const View* v, const float p[3]) {
    if (g_icons.n <= ICON_ALL) return true;
    float d[3] = { p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2] };
    return dot3(d, d) < ICON_DIST * ICON_DIST;
}

// An icon with its depth: smaller far away, a line down to what is under it with a shadow there, faint when a mesh is
// in front of it (gi: its index in g_icons, UINT32_MAX: not listed)
static void draw_icon_at(Editor* ed, ImDrawList* dl, const View* v, Entity e, const CTransform* t, float s, uint32_t gi) {
    World* w = &ed->world;
    if (t->flags & TRANSFORM_HIDDEN) return;
    const CCollision* trig = ecs_get(w, e, COMP_COLLISION);
    if (trig) { // a Trigger: its shape in Unreal's orange wireframe (box edges; a sphere's or capsule's three rings)
        float m[3][3];
        rotator_axes(t->rotation, m);
        uint32_t tc = e == ed->selected ? TH(TK_AMBER) : THA(TK_ORANGE, 192);
        float ex[3] = { trig->extent[0] * t->scale[0], trig->extent[1] * t->scale[1], trig->extent[2] * t->scale[2] };
        if (trig->shape == COLLISION_BOX) {
            float p[8][3];
            for (int c = 0; c < 8; c++)
                for (int k = 0; k < 3; k++)
                    p[c][k] = t->location[k] + m[0][k] * ((c & 1) ? ex[0] : -ex[0]) + m[1][k] * ((c & 2) ? ex[1] : -ex[1]) + m[2][k] * ((c & 4) ? ex[2] : -ex[2]);
            for (int c = 0; c < 8; c++)
                for (int b = 1; b < 8; b <<= 1)
                    if (!(c & b)) line3(dl, v, p[c], p[c | b], tc, 1.5f);
        } else {
            float r = trig->extent[0] * fmaxf(t->scale[0], t->scale[1]), hh = trig->shape == COLLISION_CAPSULE ? fmaxf(trig->extent[1] * t->scale[2] - r, 0.0f) : 0.0f;
            // three rings (XY, XZ, YZ); a capsule's vertical ones split at the middle: the upper half raised, the lower lowered
            static const int axes[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
            for (int ring = 0; ring < 3; ring++)
                for (int k = 0; k < 32; k++) {
                    float a0 = (float)k * 0.19634954f, a1 = (float)(k + 1) * 0.19634954f, q0[3], q1[3];
                    int ua = axes[ring][0], ub = axes[ring][1];
                    float o0 = ring ? (sinf(a0) + sinf(a1) >= 0.0f ? hh : -hh) : 0.0f; // (one side per segment)
                    for (int c = 0; c < 3; c++) {
                        q0[c] = t->location[c] + (m[ua][c] * cosf(a0) + m[ub][c] * sinf(a0)) * r + m[2][c] * o0;
                        q1[c] = t->location[c] + (m[ua][c] * cosf(a1) + m[ub][c] * sinf(a1)) * r + m[2][c] * o0;
                    }
                    line3(dl, v, q0, q1, tc, 1.5f);
                }
            for (int s = 0; hh > 0.0f && s < 4; s++) { // the capsule's straight sides
                float q0[3], q1[3], sx = s == 0 ? 1.0f : s == 1 ? -1.0f : 0.0f, sy = s == 2 ? 1.0f : s == 3 ? -1.0f : 0.0f;
                for (int c = 0; c < 3; c++) {
                    float side = (m[0][c] * sx + m[1][c] * sy) * r;
                    q0[c] = t->location[c] + side + m[2][c] * hh, q1[c] = t->location[c] + side - m[2][c] * hh;
                }
                line3(dl, v, q0, q1, tc, 1.5f);
            }
            for (int s = 0; hh > 0.0f && s < 2; s++) // the capsule's top and bottom rings
                for (int k = 0; k < 32; k++) {
                    float a0 = (float)k * 0.19634954f, a1 = (float)(k + 1) * 0.19634954f, q0[3], q1[3], off = s ? hh : -hh;
                    for (int c = 0; c < 3; c++) {
                        q0[c] = t->location[c] + (m[0][c] * cosf(a0) + m[1][c] * sinf(a0)) * r + m[2][c] * off;
                        q1[c] = t->location[c] + (m[0][c] * cosf(a1) + m[1][c] * sinf(a1)) * r + m[2][c] * off;
                    }
                    line3(dl, v, q0, q1, tc, 1.5f);
                }
        }
    }
    ImVec2 p;
    if (icon_off(w, e, t) || !project_point(v, t->location, &p)) return;
    float d[3] = { t->location[0] - v->pos[0], t->location[1] - v->pos[1], t->location[2] - v->pos[2] };
    float dist = v->ppc > 0.0f ? 900.0f : sqrtf(dot3(d, d)); // (orthographic: no depth, the full size)
    s *= fminf(fmaxf(900.0f / fmaxf(dist, 1.0f), 0.5f), 1.25f);
    bool occ = gi != UINT32_MAX && g_icons.occluded && g_icons.occluded[gi];
    uint32_t fade = occ ? THA(TK_N0, 85) : TH(TK_N0);
    float gz = gi != UINT32_MAX && g_icons.ground ? g_icons.ground[gi] : 0.0f;
    const CLight* lt = ecs_get(w, e, COMP_LIGHT);
    bool everywhere = lt && (lt->type == LIGHT_DIRECTIONAL || lt->type == LIGHT_SKY); // (the sun and the sky: no place)
    if (!everywhere && t->location[2] - gz > 5.0f) { // the line to the ground and the shadow on it (a circle lying on the floor)
        float g[3] = { t->location[0], t->location[1], gz };
        ImVec2 gp;
        if (project_point(v, g, &gp)) {
            for (int k = 0; k < 12; k++) { // dashed
                float a0 = (float)k / 12.0f, a1 = ((float)k + 0.5f) / 12.0f;
                ImDrawList_AddLine(dl, (ImVec2){ p.x + (gp.x - p.x) * a0, p.y + (gp.y - p.y) * a0 }, (ImVec2){ p.x + (gp.x - p.x) * a1, p.y + (gp.y - p.y) * a1 },
                                   (occ ? THA(TK_N0, 48) : THA(TK_N0, 144)) | 0x00C8C8C8u, 1.2f);
            }
            ImVec2 ring[16];
            float rad = 18.0f;
            int nr = 0;
            for (int k = 0; k < 16; k++) {
                float q[3] = { g[0] + cosf((float)k * 0.3926991f) * rad, g[1] + sinf((float)k * 0.3926991f) * rad, gz + 0.5f };
                if (project_point(v, q, &ring[nr])) nr++;
            }
            if (nr == 16) {
                ImDrawList_AddConvexPolyFilled(dl, ring, 16, occ ? THA(TK_N0, 48) : THA(TK_N0, 112));
                ImDrawList_AddPolyline(dl, ring, 16, occ ? THA(TK_N14, 64) : THA(TK_N14, 160), 1.0f, ImDrawFlags_Closed);
            }
        }
    }
    bool sel = e == ed->selected;
    uint32_t col = TH(TK_N14);
    const CLight* light = ecs_get(w, e, COMP_LIGHT);
    if (light) { // tinted by the light's color
        const float* c = light->color;
        float mx = fmaxf(c[0], fmaxf(c[1], c[2]));
        if (mx > 0)
            col = 0xFF000000u | ((uint32_t)(c[2] / mx * 230 + 25) << 16) | ((uint32_t)(c[1] / mx * 230 + 25) << 8) |
                  (uint32_t)(c[0] / mx * 230 + 25);
    }
    col = (col & 0x00FFFFFFu) | fade;
    // a Skeletal Mesh's actor: its own picture (the model in T pose) on a rounded card, bigger than the icons
    float r = s * 0.95f;
    ImVec2 c0 = { p.x - r, p.y - r }, c1 = { p.x + r, p.y + r };
    if (ecs_get(w, e, COMP_SKELETAL_MESH) || ecs_get(w, e, COMP_STATIC_MESH)) { // (inside its own mesh: "behind" it, drawn anyway)
        ImDrawList_AddRectFilled(dl, c0, c1, sel ? THA(TK_ORANGE, 192) : THA(TK_N5, 144), r * 0.35f, 0);
        if (editor_actor_thumb(w, e, dl, c0.x + 2, c0.y + 2, c1.x - 2, c1.y - 2)) {
            ImDrawList_AddRect(dl, c0, c1, sel ? TH(TK_N15) : THA(TK_N15, 128), r * 0.35f, sel ? 2.0f : 1.0f, 0);
            return;
        }
    }
    ImDrawList_AddCircleFilled(dl, p, s * 0.62f, sel ? THA(TK_ORANGE, 192) : occ ? THA(TK_N5, 64) : THA(TK_N5, 144), 24);
    if (occ) ImDrawList_AddCircle(dl, p, s * 0.62f, THA(TK_N15, 96), 24, 1.0f); // behind something: its outline only
    ui_icon(dl, actor_icon(w, e), p.x - s * 0.4f, p.y - s * 0.4f, s * 0.8f, sel ? TH(TK_N15) : col);
}
static void draw_icon(Editor* ed, ImDrawList* dl, const View* v, Entity e, const CTransform* t, float s) {
    uint32_t gi = UINT32_MAX;
    for (uint32_t i = 0; i < g_icons.n && gi == UINT32_MAX; i++) // (the selection drawn on its own: its index)
        if (g_icons.e[i].index == entity_index(e)) gi = i;
    draw_icon_at(ed, dl, v, e, t, s, gi);
}

static void draw_icons(Editor* ed, ImDrawList* dl, const View* v) {
    World* w = &ed->world;
    float s = igGetFontSize() * 1.9f;
    icons_update(w);
    // which are behind a mesh: 24 icons a frame (round robin), a ray from the camera to each
    for (uint32_t k = 0; k < 24 && g_icons.n && g_icons.occluded && g_icons.n <= ICON_ALL; k++) {
        uint32_t i = g_icons.next_occ++ % g_icons.n;
        Entity e = ecs_entity_at(w, g_icons.e[i].index);
        const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t) continue;
        float d[3] = { t->location[0] - v->pos[0], t->location[1] - v->pos[1], t->location[2] - v->pos[2] };
        float len = sqrtf(dot3(d, d));
        if (len < 1.0f) continue;
        for (int c = 0; c < 3; c++) d[c] /= len;
        g_icons.occluded[i] = ray_meshes(w, v->pos, d, len - 20.0f) < 1e29f;
    }
    if (g_icons.n <= ICON_ALL) {
        for (uint32_t i = 0; i < g_icons.n; i++) {
            Entity e = ecs_entity_at(w, g_icons.e[i].index);
            const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            if (t) draw_icon_at(ed, dl, v, e, t, s, i);
        }
        return;
    }
    // the columns within ICON_DIST, each found by a binary search; the selection always (it may have left its column)
    int32_t x0 = (int32_t)floorf((v->pos[0] - ICON_DIST) / ICON_CELL), x1 = (int32_t)floorf((v->pos[0] + ICON_DIST) / ICON_CELL);
    int32_t y0 = (int32_t)floorf((v->pos[1] - ICON_DIST) / ICON_CELL), y1 = (int32_t)floorf((v->pos[1] + ICON_DIST) / ICON_CELL);
    for (int32_t x = x0; x <= x1; x++)
        for (int32_t y = y0; y <= y1; y++) {
            uint64_t k = icon_key(x, y);
            uint32_t lo = 0, hi = g_icons.n;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                if (g_icons.e[mid].key < k) lo = mid + 1;
                else hi = mid;
            }
            for (uint32_t i = lo; i < g_icons.n && g_icons.e[i].key == k; i++) {
                Entity e = ecs_entity_at(w, g_icons.e[i].index);
                const CTransform* t = e && e != ed->selected ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
                if (t && icon_shown(v, t->location)) draw_icon_at(ed, dl, v, e, t, s, i);
            }
        }
    const CTransform* st = ecs_get(w, ed->selected, COMP_TRANSFORM);
    const CStaticMesh* ssm = ecs_get(w, ed->selected, COMP_STATIC_MESH);
    if (st && (!ssm || render_mesh_slot(ssm->mesh) >= SHAPE_COUNT) && !ecs_get(w, ed->selected, COMP_ATTACH)) draw_icon(ed, dl, v, ed->selected, st, s);
}

// The selected light's reach and the camera's frustum, in its color
// Unreal's gameplay debugger (the ' key): each agent's path, its sight (green when it sees its target) and what it does
static void draw_ai(Editor* ed, ImDrawList* dl, const View* v) {
    static AiDebug d[256];
    uint32_t n = ai_debug(d, 256);
    World* w = &ed->world;
    for (uint32_t i = 0; i < n; i++) {
        const CTransform* t = ecs_get(w, d[i].pawn, COMP_TRANSFORM);
        if (!t) continue;
        ImVec2 prev, q;
        bool have = project_point(v, t->location, &prev);
        for (uint32_t k = 0; k < d[i].npath; k++) {
            float p[3] = { d[i].path[k][0], d[i].path[k][1], d[i].path[k][2] + 5.0f };
            bool ok = project_point(v, p, &q);
            if (have && ok) ImDrawList_AddLine(dl, prev, q, TH(TK_AMBER), 2.5f);
            if (ok) ImDrawList_AddCircleFilled(dl, q, 4.0f, TH(TK_AMBER), 8);
            prev = q, have = ok;
        }
        const CAIAgent* a = ecs_get(w, d[i].pawn, COMP_AI_AGENT);
        if (a) { // the sight cone on the ground
            float yaw = t->rotation[1] * 0.017453293f, half = a->sight_angle * 0.5f * 0.017453293f, r = fminf(a->sight_radius, 1500.0f);
            uint32_t col = d[i].sees ? THA(TK_GREEN, 160) : THA(TK_ORANGE_L, 96);
            ImVec2 c0;
            float base[3] = { t->location[0], t->location[1], t->location[2] - 80.0f };
            if (project_point(v, base, &c0)) {
                ImVec2 last = c0;
                for (int s = 0; s <= 12; s++) {
                    float ang = yaw - half + 2.0f * half * (float)s / 12.0f, p[3] = { base[0] + cosf(ang) * r, base[1] + sinf(ang) * r, base[2] };
                    ImVec2 pp;
                    if (!project_point(v, p, &pp)) continue;
                    ImDrawList_AddLine(dl, last, pp, col, 1.5f);
                    last = pp;
                }
                ImDrawList_AddLine(dl, last, c0, col, 1.5f);
            }
        }
        if (d[i].has_target) { // where it thinks the target will be (a cross), its slot in the squad's ring (a circle)
            ImVec2 pp, fp;
            if (project_point(v, d[i].predicted, &pp)) {
                ImDrawList_AddLine(dl, (ImVec2){ pp.x - 6, pp.y - 6 }, (ImVec2){ pp.x + 6, pp.y + 6 }, TH(TK_ORANGE), 2.0f);
                ImDrawList_AddLine(dl, (ImVec2){ pp.x - 6, pp.y + 6 }, (ImVec2){ pp.x + 6, pp.y - 6 }, TH(TK_ORANGE), 2.0f);
            }
            if (d[i].squad > 1 && project_point(v, d[i].flank, &fp)) ImDrawList_AddCircle(dl, fp, 7.0f, d[i].flanks ? TH(TK_BLUE) : THA(TK_BLUE, 128), 12, 2.0f);
        }
        float head[3] = { t->location[0], t->location[1], t->location[2] + 120.0f };
        ImVec2 hp;
        if (project_point(v, head, &hp)) {
            const CName* nm = ecs_get(w, d[i].pawn, COMP_NAME);
            char text[128];
            snprintf(text, sizeof text, "%s: %s", nm ? nm->text : "AI", d[i].what);
            ImVec2 ts = igCalcTextSize(text, NULL, false, -1);
            ImDrawList_AddRectFilled(dl, (ImVec2){ hp.x - ts.x * 0.5f - 4, hp.y - 2 }, (ImVec2){ hp.x + ts.x * 0.5f + 4, hp.y + ts.y + 2 }, THA(TK_N0, 176), 3, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ hp.x - ts.x * 0.5f, hp.y }, d[i].sees ? TH(TK_GREEN_L) : TH(TK_N15), text, NULL);
        }
    }
}

// Unreal's P: the NavMesh near the camera, green over the floor (each area its color, the tiles in two shades), its walls
// bright, the Nav Links as arcs (green: both ends on the mesh; red: an end off it). Seen from below, a floor is not drawn
typedef struct NavDraw { ImDrawList* dl; const View* v; } NavDraw;
static const uint8_t g_nav_area_col[NAV_AREA_COUNT] = { TK_RED, TK_GREEN, TK_ORANGE, TK_BLUE }; // (tokens; drawn at alpha 96 / 88)
static void nav_poly_cb(void* u, const float (*pts)[3], int n, int area, int parity) {
    NavDraw* d = u;
    float zmean = 0.0f;
    for (int i = 0; i < n; i++) zmean += pts[i][2];
    zmean /= (float)n;
    if (d->v->ppc <= 0.0f && d->v->pos[2] < zmean - 10.0f) return; // (from below)
    ImVec2 q[8];
    for (int i = 0; i < n; i++) {
        float p[3] = { pts[i][0], pts[i][1], pts[i][2] + 4.0f };
        if (!project_point(d->v, p, &q[i])) return;
    }
    uint32_t col = THA(g_nav_area_col[area < NAV_AREA_COUNT ? area : NAV_AREA_DEFAULT], area == 0 ? 96 : 88);
    if (parity) col = (col & 0xFF000000u) | ((col & 0x00FEFEFEu) >> 1) + ((col & 0x00FCFCFCu) >> 2); // (the other tiles a shade darker)
    for (int i = 2; i < n; i++) ImDrawList_AddTriangleFilled(d->dl, q[0], q[i - 1], q[i], col);
}
static void nav_edge_cb(void* u, const float a[3], const float b[3], bool portal) {
    NavDraw* d = u;
    if (d->v->ppc <= 0.0f && d->v->pos[2] < (a[2] + b[2]) * 0.5f - 10.0f) return;
    float pa[3] = { a[0], a[1], a[2] + 4.0f }, pb[3] = { b[0], b[1], b[2] + 4.0f };
    line3(d->dl, d->v, pa, pb, portal ? THA(TK_GREEN, 48) : THA(TK_GREEN, 224), portal ? 1.0f : 2.0f);
}
static void nav_link_cb(void* u, const float a[3], const float b[3], bool both, bool linked) {
    NavDraw* d = u;
    uint32_t col = linked ? TH(TK_GREEN) : TH(TK_RED);
    float prev[3] = { a[0], a[1], a[2] + 5.0f }, len = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
    float h = fmaxf(fmaxf(b[2] - a[2], 0.0f) + 60.0f, len * 0.25f);
    for (int s = 1; s <= 16; s++) { // (a parabola over the gap)
        float t = (float)s / 16.0f, p[3] = { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t + 4.0f * h * t * (1.0f - t) + 5.0f };
        line3(d->dl, d->v, prev, p, col, 2.5f);
        memcpy(prev, p, 12);
    }
    for (int e = 0; e < 2; e++) {
        const float* c = e ? b : a;
        ImVec2 q;
        float p[3] = { c[0], c[1], c[2] + 5.0f };
        if (project_point(d->v, p, &q)) ImDrawList_AddCircleFilled(d->dl, q, e == 0 || both ? 5.0f : 3.0f, col, 10);
    }
}
static void draw_navmesh(Editor* ed, ImDrawList* dl, const View* v, bool polys) {
    NavDraw d = { dl, v };
    float r = v->ppc > 0.0f ? 20000.0f : 5000.0f;
    if (polys) {
        navmesh_debug(v->pos, r, nav_poly_cb, NULL, &d);
        navmesh_debug_edges(v->pos, r, nav_edge_cb, &d);
    }
    navmesh_debug(v->pos, r, NULL, nav_link_cb, &d);
    (void)ed;
}

static void draw_selected_shape(Editor* ed, ImDrawList* dl, const View* v) {
    World* w = &ed->world;
    const CTransform* t = ecs_get(w, ed->selected, COMP_TRANSFORM);
    if (!t) return;
    float m[3][3];
    rotator_axes(t->rotation, m);
    const uint32_t col = THA(TK_AMBER_L, 192);
    const CLight* l = ecs_get(w, ed->selected, COMP_LIGHT);
    if (l) {
        if (l->type == LIGHT_POINT) {
            circle3(dl, v, t->location, m[0], m[1], l->attenuation_radius, col, 1.5f);
            circle3(dl, v, t->location, m[0], m[2], l->attenuation_radius, col, 1.5f);
            circle3(dl, v, t->location, m[1], m[2], l->attenuation_radius, col, 1.5f);
        } else if (l->type == LIGHT_SPOT) {
            float r = l->attenuation_radius, ca = cosf(l->outer_cone * DEG2RAD), sa = sinf(l->outer_cone * DEG2RAD);
            float c[3];
            for (int k = 0; k < 3; k++) c[k] = t->location[k] + m[0][k] * r * ca;
            circle3(dl, v, c, m[1], m[2], r * sa, col, 1.5f);
            for (int i = 0; i < 8; i++) {
                float a = (float)i / 8.0f * 6.2831853f, p[3];
                for (int k = 0; k < 3; k++) p[k] = c[k] + (m[1][k] * cosf(a) + m[2][k] * sinf(a)) * r * sa;
                line3(dl, v, t->location, p, col, 1.0f);
            }
        } else if (l->type == LIGHT_RECT) {
            float hw = l->source_width * 0.5f * t->scale[1], hh = l->source_height * 0.5f * t->scale[2], p[4][3];
            for (int i = 0; i < 4; i++) {
                float sx = (i == 1 || i == 2) ? 1.0f : -1.0f, sy = i >= 2 ? 1.0f : -1.0f;
                for (int k = 0; k < 3; k++) p[i][k] = t->location[k] + m[1][k] * hw * sx + m[2][k] * hh * sy;
            }
            for (int i = 0; i < 4; i++) line3(dl, v, p[i], p[(i + 1) % 4], col, 2.0f);
        }
        if (l->type != LIGHT_SKY && l->type != LIGHT_POINT) { // the direction it shines
            float tip[3];
            for (int k = 0; k < 3; k++) tip[k] = t->location[k] + m[0][k] * 150.0f;
            line3(dl, v, t->location, tip, TH(TK_AMBER), 3.0f);
        }
    }
    const CPostProcess* ppv = ecs_get(w, ed->selected, COMP_POST_PROCESS);
    if (ppv && !(ppv->flags & PPV_UNBOUND)) { // the volume's box (100 cm * scale each way), Unreal's pink wireframe
        float p[8][3];
        for (int c = 0; c < 8; c++)
            for (int k = 0; k < 3; k++)
                p[c][k] = t->location[k] + m[0][k] * t->scale[0] * ((c & 1) ? 100.0f : -100.0f) +
                          m[1][k] * t->scale[1] * ((c & 2) ? 100.0f : -100.0f) + m[2][k] * t->scale[2] * ((c & 4) ? 100.0f : -100.0f);
        for (int c = 0; c < 8; c++)
            for (int b = 1; b < 8; b <<= 1)
                if (!(c & b)) line3(dl, v, p[c], p[c | b], TH(TK_PINK_L), 1.5f);
    }
    if (ecs_get(w, ed->selected, COMP_DECAL)) { // a decal's box (100 cm * scale each way) and the way it projects (its X)
        float p[8][3];
        for (int c = 0; c < 8; c++)
            for (int k = 0; k < 3; k++)
                p[c][k] = t->location[k] + m[0][k] * t->scale[0] * ((c & 1) ? 100.0f : -100.0f) +
                          m[1][k] * t->scale[1] * ((c & 2) ? 100.0f : -100.0f) + m[2][k] * t->scale[2] * ((c & 4) ? 100.0f : -100.0f);
        for (int c = 0; c < 8; c++)
            for (int b = 1; b < 8; b <<= 1)
                if (!(c & b)) line3(dl, v, p[c], p[c | b], TH(TK_AMBER), 1.5f);
        float tip[3];
        for (int k = 0; k < 3; k++) tip[k] = t->location[k] + m[0][k] * t->scale[0] * 100.0f;
        line3(dl, v, t->location, tip, TH(TK_AMBER), 3.0f);
    }
    const CNav* nav = ecs_get(w, ed->selected, COMP_NAV);
    if (nav && nav->kind != NAV_LINK_PROXY) { // a Nav Mesh Bounds / Modifier Volume's box (100 cm * scale each way)
        float p[8][3];
        for (int c = 0; c < 8; c++)
            for (int k = 0; k < 3; k++)
                p[c][k] = t->location[k] + m[0][k] * t->scale[0] * ((c & 1) ? 100.0f : -100.0f) +
                          m[1][k] * t->scale[1] * ((c & 2) ? 100.0f : -100.0f) + m[2][k] * t->scale[2] * ((c & 4) ? 100.0f : -100.0f);
        uint32_t ncol = nav->kind == NAV_BOUNDS_VOLUME ? TH(TK_GREEN) : TH(g_nav_area_col[nav->area < NAV_AREA_COUNT ? nav->area : 1]);
        for (int c = 0; c < 8; c++)
            for (int b = 1; b < 8; b <<= 1)
                if (!(c & b)) line3(dl, v, p[c], p[c | b], ncol, 1.5f);
    }
    const CCheckpoint* ckp = ecs_get(w, ed->selected, COMP_CHECKPOINT);
    if (ckp) { // a Checkpoint's box (its extent * scale) and where the player comes back (save_progress.h)
        float p[8][3];
        for (int c = 0; c < 8; c++)
            for (int k = 0; k < 3; k++)
                p[c][k] = t->location[k] + m[0][k] * t->scale[0] * ((c & 1) ? ckp->extent[0] : -ckp->extent[0]) +
                          m[1][k] * t->scale[1] * ((c & 2) ? ckp->extent[1] : -ckp->extent[1]) + m[2][k] * t->scale[2] * ((c & 4) ? ckp->extent[2] : -ckp->extent[2]);
        for (int c = 0; c < 8; c++)
            for (int b = 1; b < 8; b <<= 1)
                if (!(c & b)) line3(dl, v, p[c], p[c | b], TH(TK_CYAN), 1.5f);
        float sp[3], up[3];
        for (int k = 0; k < 3; k++) sp[k] = t->location[k] + m[0][k] * ckp->spawn_offset[0] + m[1][k] * ckp->spawn_offset[1] + m[2][k] * ckp->spawn_offset[2];
        for (int k = 0; k < 3; k++) up[k] = sp[k] + m[2][k] * 180.0f;
        line3(dl, v, sp, up, TH(TK_AMBER), 2.5f);
    }
    const CCamera* cam = ecs_get(w, ed->selected, COMP_CAMERA);
    if (cam) {
        float d = 120.0f, hx = tanf(cam->fov_deg * 0.5f * DEG2RAD) * d, hy = hx * 9.0f / 16.0f, p[4][3];
        for (int i = 0; i < 4; i++) {
            float sx = (i == 1 || i == 2) ? 1.0f : -1.0f, sy = i >= 2 ? 1.0f : -1.0f;
            for (int k = 0; k < 3; k++) p[i][k] = t->location[k] + m[0][k] * d + m[1][k] * hx * sx + m[2][k] * hy * sy;
        }
        for (int i = 0; i < 4; i++) {
            line3(dl, v, p[i], p[(i + 1) % 4], col, 1.5f);
            line3(dl, v, t->location, p[i], col, 1.0f);
        }
    }
}

// --- The gizmo (copied and adapted from the Tlender's manipulators, manip.c / ui.c / view3d.c; 2026-11-23: its whole
// control layer too). Move: arrows, plane squares, the center ball (free in the view plane). Rotate: the front half of
// each axis ring, the outer ring (about the view axis), the trackball inside (free), the swept angle as a pie. Scale:
// lines with boxes, plane squares, the center and the outer ring (uniform). Handles fade out when their axis points at
// the viewer, and a handle seen edge-on drags by its screen direction (the Tlender's fallbacks).
// The dev's control, as the Tlender's: Transform Orientation World / Local / View (the toolbar; Ctrl+` World <-> Local;
// scale's World is the actor's own axes, like Unreal's), the Pivot Point (the active actor, the median, the bounding
// box's center, each actor's own origin; Only Locations), the Gizmo Size (Editor Preferences, +/- while dragging).
// While dragging: X / Y / Z constrain to that axis (again: the other orientation, again: off), Shift+X / Y / Z to the
// plane without it, digits type the value (Tab: the next one, Backspace, - flips), Ctrl inverts the snapping, Shift is
// a tenth of the motion, the right button or Esc cancels (back to where it began, no undo step), a click that didn't
// move pushes no undo step; the values show at the mouse, an axis constraint as a line through the pivot ---

// Layout in gizmo units (1 = the gizmo's size in pixels): the Tlender's single-tool proportions
static float g_gz_px = 100.0f; // (the frame's: Editor Preferences' Gizmo Size x the UI scale)
#define GIZMO_PX g_gz_px
#define GZ_BALL 0.17f
#define GZ_ARROW0 0.25f
#define GZ_ARROW1 1.0f
#define GZ_HEAD_LEN 0.22f
#define GZ_HEAD_W 0.075f
#define GZ_BOX0 0.25f
#define GZ_BOX_AT 0.92f
#define GZ_BOX_R 0.07f
#define GZ_PLANE_LO 0.32f
#define GZ_PLANE_HI 0.52f
#define GZ_RING 1.0f
#define GZ_VIEW_RING 1.22f
#define GZ_TRACKBALL 0.86f // (rotate: inside the rings, a free turn)
#define GZ_PICK 9.0f
#define GZ_RING_SEGS 96

typedef struct Gizmo {
    float c[3], ax[3][3]; // pivot, unit world axes
    float unit;           // world length of one gizmo unit at the pivot
    float px;             // world length of a pixel at the pivot
    ImVec2 pc;            // pivot on screen
    float to_eye[3];
    float axis_vis[3], plane_vis[3]; // 0..1: fading as the axis points at the viewer / the plane is seen edge-on
} Gizmo;

// The drag (one at a time): what it grabbed, its references, the delta it makes, the keys' state
static struct GzDrag {
    int part;             // the handle (GIZMO_*): the one grabbed, or the X / Y / Z keys' constraint
    int grabbed;          // the handle grabbed (the keys' constraint off: back to it)
    int key_axis, key_plane, key_alt; // X / Y / Z: the axis (-1), Shift: its plane, pressed again: the other orientation
    ImVec2 press, vm;     // the mouse at the press, the virtual mouse (Shift: a tenth of the motion)
    float ax[3][3], pivot[3]; // the gizmo's axes and pivot when it began (the drag's frame)
    float t0, start[3];   // the axis parameter, the plane point at the press
    bool fallback;        // seen edge-on: by the screen direction (sdx, sdy)
    float sdx, sdy;
    float last, angle;    // a ring's unwrapped angle
    bool pushed;          // its undo step (pushed at the first change)
    char num[3][24];      // the typed values (Tlender's numinput: Tab the next one)
    int num_i;
    bool typing;
    // the delta made: a move, a turn (axis, angle; the trackball's two), a scale (along ax)
    float d[3], n[3], ang, tb[2], f[3];
    char header[200];
} GD;

static bool g_gz_rmb_hold; // (the right button cancelled a drag: no flying or panning until it's let go)

static float smooth01(float x, float lo, float hi) {
    float t = fminf(fmaxf((x - lo) / (hi - lo), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float seg_dist(ImVec2 p, ImVec2 a, ImVec2 b) {
    float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    float l2 = vx * vx + vy * vy, t = l2 > 1e-9f ? fminf(fmaxf((wx * vx + wy * vy) / l2, 0.0f), 1.0f) : 0.0f;
    float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
    return sqrtf(dx * dx + dy * dy);
}

static bool in_polygon(ImVec2 p, const ImVec2* q, int n) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((q[i].y > p.y) != (q[j].y > p.y) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x) in = !in;
    return in;
}

// The orientation in effect: the toolbar's, its other one after X/Y/Z pressed twice (World <-> Local); scale's World is
// the actor's own axes (Unreal's: a scale along the world's axes would shear a turned actor)
static int gizmo_orient_now(const Editor* ed, bool alt) {
    int o = ed->gizmo_orient;
    if (alt) o = o == GIZMO_ORIENT_LOCAL ? GIZMO_ORIENT_WORLD : GIZMO_ORIENT_LOCAL;
    if (ed->tool == TOOL_SCALE && o == GIZMO_ORIENT_WORLD) o = GIZMO_ORIENT_LOCAL;
    return o;
}
static void gizmo_axes(const Editor* ed, const View* v, const CTransform* t, bool alt, float ax[3][3]) {
    int o = gizmo_orient_now(ed, alt);
    if (o == GIZMO_ORIENT_LOCAL) {
        rotator_axes(t->rotation, ax);
        for (int a = 0; a < 3; a++) normalize3(ax[a]);
    } else if (o == GIZMO_ORIENT_VIEW) { // X right, Y up, Z = X x Y (into the screen: the basis turns the rings' way)
        for (int k = 0; k < 3; k++) ax[0][k] = v->right[k], ax[1][k] = v->up[k], ax[2][k] = v->fwd[k];
    } else {
        static const float id[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        memcpy(ax, id, sizeof id);
    }
}

// The Pivot Point: the active actor's place, the selection's median or its box's center (Individual Origins: the
// gizmo at the active one; each actor turns about its own)
static void gizmo_pivot_point(const Editor* ed, const CTransform* active, float out[3]) {
    memcpy(out, active->location, sizeof(float) * 3);
    if (ed->nsel < 2 || (ed->gizmo_pivot != GIZMO_PIVOT_MEDIAN && ed->gizmo_pivot != GIZMO_PIVOT_BOUNDS)) return;
    float sum[3] = { 0, 0, 0 }, mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    uint32_t n = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        const CTransform* t = ecs_get((World*)&ed->world, ed->sel[i], COMP_TRANSFORM);
        if (!t) continue;
        for (int k = 0; k < 3; k++) sum[k] += t->location[k], mn[k] = fminf(mn[k], t->location[k]), mx[k] = fmaxf(mx[k], t->location[k]);
        n++;
    }
    if (!n) return;
    for (int k = 0; k < 3; k++) out[k] = ed->gizmo_pivot == GIZMO_PIVOT_MEDIAN ? sum[k] / (float)n : (mn[k] + mx[k]) * 0.5f;
}

static bool gizmo_setup(Gizmo* g, const View* v, const float c[3], const float ax[3][3]) {
    memcpy(g->c, c, sizeof g->c);
    memcpy(g->ax, ax, sizeof g->ax);
    if (!project_point(v, c, &g->pc)) return false;
    if (v->ppc > 0.0f) { // (orthographic: a pixel is the same everywhere; before, a pivot in front of the target hid the gizmo)
        g->px = 1.0f / v->ppc;
        for (int k = 0; k < 3; k++) g->to_eye[k] = -v->fwd[k];
    } else {
        float d[3] = { c[0] - v->pos[0], c[1] - v->pos[1], c[2] - v->pos[2] };
        float z = dot3(d, v->fwd);
        if (z < 5.0f) return false;
        g->px = z / v->focal;
        float l = sqrtf(dot3(d, d));
        for (int k = 0; k < 3; k++) g->to_eye[k] = -d[k] / l;
    }
    g->unit = GIZMO_PX * g->px;
    for (int i = 0; i < 3; i++) {
        float cs = fabsf(dot3(ax[i], g->to_eye));
        g->axis_vis[i] = smooth01(sqrtf(fmaxf(0.0f, 1.0f - cs * cs)), 0.12f, 0.26f);
        g->plane_vis[i] = smooth01(cs, 0.12f, 0.26f);
    }
    return true;
}

static bool gz_point(const Gizmo* g, const View* v, float a, float b, float c, ImVec2* out) {
    float p[3];
    for (int k = 0; k < 3; k++) p[k] = g->c[k] + (g->ax[0][k] * a + g->ax[1][k] * b + g->ax[2][k] * c) * g->unit;
    return project_point(v, p, out);
}
static bool gz_axis_point(const Gizmo* g, const View* v, int axis, float t, ImVec2* out) {
    return gz_point(g, v, axis == 0 ? t : 0.0f, axis == 1 ? t : 0.0f, axis == 2 ? t : 0.0f, out);
}

// A ring of radius r (gizmo units) about an axis: n points with a front-half flag each
static void gz_ring(const Gizmo* g, const View* v, int axis, float r, int n, ImVec2* pts, bool* front) {
    const float* u = g->ax[(axis + 1) % 3];
    const float* w = g->ax[(axis + 2) % 3];
    for (int k = 0; k < n; k++) {
        float t = 6.2831853f * (float)k / (float)n, d[3], p[3];
        for (int j = 0; j < 3; j++) d[j] = u[j] * cosf(t) + w[j] * sinf(t), p[j] = g->c[j] + d[j] * r * g->unit;
        bool ok = project_point(v, p, &pts[k]);
        front[k] = ok && dot3(d, g->to_eye) >= -0.02f; // the back half is hidden
    }
}

// The plane square of normal axis n (between the other two axes), counterclockwise
static bool gz_plane_quad(const Gizmo* g, const View* v, int n, ImVec2 q[4]) {
    static const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int a = (n + 1) % 3, b = (n + 2) % 3;
    for (int k = 0; k < 4; k++) {
        float off[3] = { 0, 0, 0 };
        off[a] = corners[k][0] ? GZ_PLANE_HI : GZ_PLANE_LO;
        off[b] = corners[k][1] ? GZ_PLANE_HI : GZ_PLANE_LO;
        if (!gz_point(g, v, off[0], off[1], off[2], &q[k])) return false;
    }
    return true;
}

// The handle under the mouse: center ball, arrows / boxes, plane squares, rings, outer ring, the trackball
static int gizmo_hit(const Editor* ed, const Gizmo* g, const View* v, ImVec2 m) {
    float dx = m.x - g->pc.x, dy = m.y - g->pc.y, dc = sqrtf(dx * dx + dy * dy);
    if (ed->tool != TOOL_ROTATE && dc <= GZ_BALL * GIZMO_PX + GZ_PICK * 0.4f) return GIZMO_VIEW;
    int best = GIZMO_NONE;
    float best_d = 1e30f;
    if (ed->tool != TOOL_ROTATE) {
        for (int i = 0; i < 3; i++) {
            if (g->axis_vis[i] <= 0.3f) continue;
            ImVec2 a, b;
            bool move = ed->tool == TOOL_MOVE;
            float t0 = move ? GZ_ARROW0 : GZ_BOX0, t1 = move ? GZ_ARROW1 : GZ_BOX_AT + GZ_BOX_R;
            float tol = move ? fmaxf(GZ_PICK, GZ_HEAD_W * GIZMO_PX + 3.0f) : fmaxf(GZ_PICK, GZ_BOX_R * GIZMO_PX * 1.3f);
            if (!gz_axis_point(g, v, i, t0, &a) || !gz_axis_point(g, v, i, t1, &b)) continue;
            float d = seg_dist(m, a, b);
            if (d <= tol && d < best_d) best_d = d, best = GIZMO_X + i;
        }
        if (best != GIZMO_NONE) return best;
        for (int n = 0; n < 3; n++) {
            ImVec2 q[4];
            if (g->plane_vis[n] <= 0.3f || !gz_plane_quad(g, v, n, q)) continue;
            bool in = in_polygon(m, q, 4);
            for (int k = 0; k < 4 && !in; k++) in = seg_dist(m, q[k], q[(k + 1) % 4]) <= 3.0f;
            if (in) return GIZMO_YZ + n;
        }
        if (ed->tool == TOOL_SCALE && fabsf(dc - GZ_VIEW_RING * GIZMO_PX) <= GZ_PICK * 1.3f) return GIZMO_VIEW;
        return GIZMO_NONE;
    }
    float band = GZ_PICK * 1.5f;
    for (int a = 0; a < 3; a++) {
        ImVec2 pts[GZ_RING_SEGS];
        bool front[GZ_RING_SEGS];
        gz_ring(g, v, a, GZ_RING, GZ_RING_SEGS, pts, front);
        for (int k = 0; k < GZ_RING_SEGS; k++) {
            int k1 = (k + 1) % GZ_RING_SEGS;
            if (!front[k] || !front[k1]) continue;
            float d = seg_dist(m, pts[k], pts[k1]);
            if (d <= band && d < best_d) best_d = d, best = GIZMO_X + a;
        }
    }
    if (best != GIZMO_NONE) return best;
    if (fabsf(dc - GZ_VIEW_RING * GIZMO_PX) <= band) return GIZMO_VIEW;
    if (dc <= GZ_TRACKBALL * GIZMO_PX) return GIZMO_TRACKBALL; // (the Tlender's: inside the rings, a free turn)
    return GIZMO_NONE;
}

// The Tlender's colors: the handle under the mouse lightens toward white
static ImU32 gz_col(int axis, bool hot, float alpha) {
    static const float cols[4][3] = { { 0.90f, 0.24f, 0.24f }, { 0.35f, 0.78f, 0.27f }, { 0.24f, 0.47f, 0.90f }, { 1, 1, 1 } };
    const float* c = cols[axis >= 0 && axis < 3 ? axis : 3];
    float k = hot ? 0.55f : 0.0f;
    float a = fminf(fmaxf(alpha, 0.0f), 1.0f);
    return IM_COL32((int)((c[0] + (1.0f - c[0]) * k) * 255.0f), (int)((c[1] + (1.0f - c[1]) * k) * 255.0f),
                    (int)((c[2] + (1.0f - c[2]) * k) * 255.0f), (int)(a * 255.0f));
}

static void gz_draw_ring(ImDrawList* dl, const Gizmo* g, const View* v, int axis, bool hot) {
    ImVec2 pts[GZ_RING_SEGS], run[GZ_RING_SEGS + 1];
    bool front[GZ_RING_SEGS];
    gz_ring(g, v, axis, GZ_RING, GZ_RING_SEGS, pts, front);
    ImU32 col = gz_col(axis, hot, 1.0f);
    float thick = hot ? 4.0f : 2.4f;
    int start = 0, n = 0;
    bool all = true;
    for (int k = 0; k < GZ_RING_SEGS; k++) all &= front[k];
    if (all) {
        ImDrawList_AddPolyline(dl, pts, GZ_RING_SEGS, col, thick, ImDrawFlags_Closed);
        return;
    }
    for (int k = 0; k < GZ_RING_SEGS; k++)
        if (!front[k]) { start = k; break; } // a run never wraps around the array's end
    for (int j = 0; j <= GZ_RING_SEGS; j++) {
        int k = (start + j) % GZ_RING_SEGS;
        if (j < GZ_RING_SEGS && front[k]) {
            run[n++] = pts[k];
            continue;
        }
        if (n >= 2) ImDrawList_AddPolyline(dl, run, n, col, thick, 0);
        n = 0;
    }
}

// The swept angle of a rotation about the world axis n (from the in-plane direction u0), a translucent pie
static void gz_draw_pie(ImDrawList* dl, const Gizmo* g, const View* v, int col_axis, const float n[3], float r, float angle) {
    int steps = (int)(fabsf(angle) / (4.0f * DEG2RAD)) + 2;
    if (steps > 180) steps = 180;
    float u[3], w[3];
    u[0] = fabsf(n[2]) < 0.9f ? -n[1] : 0.0f, u[1] = fabsf(n[2]) < 0.9f ? n[0] : -n[2], u[2] = fabsf(n[2]) < 0.9f ? 0.0f : n[1];
    if (GD.start[0] != 0.0f || GD.start[1] != 0.0f || GD.start[2] != 0.0f) { // (from where the ring was grabbed)
        float rel[3] = { GD.start[0] - GD.pivot[0], GD.start[1] - GD.pivot[1], GD.start[2] - GD.pivot[2] }, dn = dot3(rel, n);
        for (int k = 0; k < 3; k++) rel[k] -= n[k] * dn;
        if (dot3(rel, rel) > 1e-6f) memcpy(u, rel, sizeof u);
    }
    normalize3(u);
    cross3(n, u, w);
    ImVec2 pts[184];
    int k2 = 0;
    pts[k2++] = g->pc;
    for (int k = 0; k <= steps; k++) {
        float t = angle * (float)k / (float)steps, p[3];
        for (int j = 0; j < 3; j++) p[j] = g->c[j] + (u[j] * cosf(t) + w[j] * sinf(t)) * r * g->unit;
        if (project_point(v, p, &pts[k2])) k2++;
    }
    if (k2 < 3) return;
    if (fabsf(angle) < 6.2831853f) ImDrawList_AddConcavePolyFilled(dl, pts, k2, gz_col(col_axis, false, 0.22f));
    ImDrawList_AddLine(dl, pts[0], pts[1], gz_col(col_axis, true, 0.9f), 1.2f);
    ImDrawList_AddLine(dl, pts[0], pts[k2 - 1], gz_col(col_axis, true, 0.9f), 1.2f);
}

static void gizmo_draw(const Editor* ed, ImDrawList* dl, const Gizmo* g, const View* v, int hot, int active) {
#define SHOW(p) (active == GIZMO_NONE || active == (p))
    ImVec2 c = g->pc;
    if (ed->tool == TOOL_ROTATE) {
        if (SHOW(GIZMO_VIEW)) {
            bool h = hot == GIZMO_VIEW;
            ImDrawList_AddCircle(dl, c, GZ_VIEW_RING * GIZMO_PX, gz_col(-1, h, h ? 1.0f : 0.75f), 72, h ? 3.4f : 1.8f);
        }
        if (hot == GIZMO_TRACKBALL || active == GIZMO_TRACKBALL) // (the Tlender's: the disc inside the rings)
            ImDrawList_AddCircleFilled(dl, c, GZ_TRACKBALL * GIZMO_PX, gz_col(-1, false, active == GIZMO_TRACKBALL ? 0.16f : 0.09f), 64);
        if (active != GIZMO_NONE && active != GIZMO_TRACKBALL) {
            float n[3];
            memcpy(n, GD.n, sizeof n);
            gz_draw_pie(dl, g, v, active == GIZMO_VIEW ? -1 : active - GIZMO_X, n, active == GIZMO_VIEW ? GZ_VIEW_RING : GZ_RING, GD.ang);
        }
        for (int i = 0; i < 3; i++)
            if (SHOW(GIZMO_X + i) && active != GIZMO_TRACKBALL) gz_draw_ring(dl, g, v, i, hot == GIZMO_X + i);
        return;
    }
    if (ed->tool == TOOL_SCALE && SHOW(GIZMO_VIEW)) {
        bool h = hot == GIZMO_VIEW;
        ImDrawList_AddCircle(dl, c, GZ_VIEW_RING * GIZMO_PX, gz_col(-1, h, h ? 1.0f : 0.7f), 72, h ? 3.0f : 1.6f);
    }
    for (int n = 0; n < 3; n++) { // plane squares
        ImVec2 q[4];
        if (!SHOW(GIZMO_YZ + n) || g->plane_vis[n] <= 0.3f || !gz_plane_quad(g, v, n, q)) continue;
        bool h = hot == GIZMO_YZ + n;
        ImDrawList_AddConvexPolyFilled(dl, q, 4, gz_col(n, h, (h ? 0.75f : 0.45f) * g->plane_vis[n]));
        ImDrawList_AddPolyline(dl, q, 4, gz_col(n, h, g->plane_vis[n]), 1.2f, ImDrawFlags_Closed);
    }
    int order[3] = { 0, 1, 2 }; // from the farthest axis to the nearest
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (dot3(g->ax[order[j]], g->to_eye) < dot3(g->ax[order[i]], g->to_eye)) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    for (int k = 0; k < 3; k++) {
        int i = order[k];
        if (!SHOW(GIZMO_X + i) || g->axis_vis[i] <= 0.3f) continue;
        bool h = hot == GIZMO_X + i;
        ImU32 col = gz_col(i, h, g->axis_vis[i]);
        ImVec2 a, b, tip;
        if (ed->tool == TOOL_MOVE) { // a line and a cone head
            if (!gz_axis_point(g, v, i, GZ_ARROW0, &a) || !gz_axis_point(g, v, i, GZ_ARROW1 - GZ_HEAD_LEN, &b) ||
                !gz_axis_point(g, v, i, GZ_ARROW1, &tip))
                continue;
            ImDrawList_AddLine(dl, a, b, col, h ? 3.2f : 2.2f);
            float dx = tip.x - b.x, dy = tip.y - b.y, len = sqrtf(dx * dx + dy * dy);
            if (len < 0.5f) continue;
            float w = GZ_HEAD_W * GIZMO_PX * (h ? 1.2f : 1.0f), px = -dy / len * w, py = dx / len * w;
            ImDrawList_AddTriangleFilled(dl, tip, (ImVec2){ b.x + px, b.y + py }, (ImVec2){ b.x - px, b.y - py }, col);
        } else { // a line and a box aligned with it on screen
            if (!gz_axis_point(g, v, i, GZ_BOX0, &a) || !gz_axis_point(g, v, i, GZ_BOX_AT - GZ_BOX_R, &b) ||
                !gz_axis_point(g, v, i, GZ_BOX_AT, &tip))
                continue;
            ImDrawList_AddLine(dl, a, b, col, h ? 3.2f : 2.2f);
            float dx = tip.x - a.x, dy = tip.y - a.y, len = sqrtf(dx * dx + dy * dy);
            if (len < 0.5f) dx = 1.0f, dy = 0.0f, len = 1.0f;
            float r = GZ_BOX_R * GIZMO_PX * (h ? 1.2f : 1.0f), ux = dx / len * r, uy = dy / len * r, vx = -uy, vy = ux;
            ImDrawList_AddQuadFilled(dl, (ImVec2){ tip.x - ux - vx, tip.y - uy - vy }, (ImVec2){ tip.x + ux - vx, tip.y + uy - vy },
                                     (ImVec2){ tip.x + ux + vx, tip.y + uy + vy }, (ImVec2){ tip.x - ux + vx, tip.y - uy + vy }, col);
        }
    }
    if (SHOW(GIZMO_VIEW)) { // the center ball: free move, or uniform scale
        bool h = hot == GIZMO_VIEW;
        float r = GZ_BALL * GIZMO_PX * (h ? 1.12f : 1.0f);
        ImDrawList_AddCircleFilled(dl, c, r, THA(TK_N15, h ? 110 : 40), 40);
        ImDrawList_AddCircle(dl, c, r, THA(TK_N15, h ? 255 : 220), 40, h ? 2.6f : 1.6f);
    }
#undef SHOW
}

// The parameter along the axis line (through c) closest to the mouse ray; false: the ray runs along it
static bool axis_param(const View* v, const float c[3], const float axis[3], const float dir[3], float* t) {
    float w0[3] = { c[0] - v->ro[0], c[1] - v->ro[1], c[2] - v->ro[2] }; // (from the last mouse ray's origin)
    float b = dot3(axis, dir), d = dot3(axis, w0), e = dot3(dir, w0);
    float den = 1.0f - b * b;
    if (fabsf(den) < 1e-5f) return false;
    *t = (b * e - d) / den;
    return true;
}

static bool ray_plane(const View* v, const float dir[3], const float c[3], const float n[3], float out[3]) {
    float dn = dot3(dir, n);
    if (fabsf(dn) < 1e-6f) return false;
    float d[3] = { c[0] - v->ro[0], c[1] - v->ro[1], c[2] - v->ro[2] }; // (from the last mouse ray's origin)
    float t = dot3(d, n) / dn;
    if (t <= 0.0f) return false;
    for (int k = 0; k < 3; k++) out[k] = v->ro[k] + dir[k] * t;
    return true;
}

static float snapf(float x, float step) { return step > 0 ? roundf(x / step) * step : x; }

// The part's kind: an axis (0-2), a plane (its normal 0-2), the view / center, the trackball
static int part_axis(int p) { return p >= GIZMO_X && p <= GIZMO_Z ? p - GIZMO_X : -1; }
static int part_plane(int p) { return p >= GIZMO_YZ && p <= GIZMO_XY ? p - GIZMO_YZ : -1; }

// The ring's angle of a point on its plane (u, w: the in-plane axes)
static float ring_angle(const float ax[3][3], int a, const float pivot[3], const float hit[3]) {
    float rel[3] = { hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2] };
    return atan2f(dot3(rel, ax[(a + 2) % 3]), dot3(rel, ax[(a + 1) % 3]));
}

// A drag's references from the press (or from a constraint key: the same press, another part): the Tlender's
// manip_drag_begin, with its edge-on fallbacks
static void gizmo_begin_part(const Gizmo* g, const View* v, int part, int tool) {
    GD.part = part;
    GD.fallback = false;
    GD.t0 = 0.0f, GD.angle = 0.0f, GD.last = 0.0f, GD.sdx = 1.0f, GD.sdy = 0.0f;
    memset(GD.start, 0, sizeof GD.start);
    float dir[3];
    mouse_ray(v, GD.press, dir);
    int a = part_axis(part), pl = part_plane(part);
    if (GD.part == GIZMO_VIEW && tool == TOOL_ROTATE && !ray_plane(v, dir, GD.pivot, v->fwd, GD.start)) memset(GD.start, 0, sizeof GD.start);
    if (GD.part == GIZMO_TRACKBALL || GD.part == GIZMO_VIEW) return;
    if (pl >= 0) { // a plane: grabbed where the ray meets it (nearly edge-on: by the view plane)
        GD.fallback = fabsf(dot3(dir, GD.ax[pl])) < 0.08f || !ray_plane(v, dir, GD.pivot, GD.ax[pl], GD.start);
        return;
    }
    if (a < 0) return;
    // (the tool's: move and scale along the axis, rotate about it)
    if (g && v) {
        float hit[3];
        bool ring = fabsf(dot3(GD.ax[a], dir)) >= 0.2f && ray_plane(v, dir, GD.pivot, GD.ax[a], hit);
        if (ring && tool == TOOL_ROTATE) GD.last = ring_angle(GD.ax, a, GD.pivot, hit), memcpy(GD.start, hit, sizeof hit);
        GD.fallback = tool == TOOL_ROTATE && !ring;
        if (tool != TOOL_ROTATE) GD.fallback = !axis_param(v, GD.pivot, GD.ax[a], dir, &GD.t0) || fabsf(dot3(dir, GD.ax[a])) > 0.97f;
        // the axis's direction on screen (an arrow at the viewer, a ring seen edge-on: dragged along it)
        ImVec2 s0 = g->pc, s1;
        float p1[3];
        for (int k = 0; k < 3; k++) p1[k] = GD.pivot[k] + GD.ax[a][k] * g->unit;
        if (project_point(v, p1, &s1)) {
            float dx = s1.x - s0.x, dy = s1.y - s0.y, l = sqrtf(dx * dx + dy * dy);
            if (l > 1e-4f) GD.sdx = dx / l, GD.sdy = dy / l;
        }
        if (!ring && tool == TOOL_ROTATE) { // a ring seen edge-on: along its tangent (the axis's screen direction turned)
            float t = GD.sdx;
            GD.sdx = -GD.sdy, GD.sdy = t;
        }
    }
}

// The typed values (the Tlender's numinput): a value per component of the constraint, Tab the next one
static float gz_num(int i, float otherwise) { return GD.num[i][0] ? (float)atof(GD.num[i]) : otherwise; }

// The delta from the press to the (virtual) mouse now: the Tlender's manip_drag_update, the snapping the toolbar's
// (Ctrl inverts it): move by the grid (World: on the world grid; Local, View: in steps along the gizmo's axes, no
// drift), rotate by its angle step, scale's result by its step
static void gizmo_delta(const Editor* ed, const Gizmo* g, const View* v, bool snap) {
    float dir[3];
    mouse_ray(v, GD.vm, dir);
    float dx = GD.vm.x - GD.press.x, dy = GD.vm.y - GD.press.y;
    int a = part_axis(GD.part), pl = part_plane(GD.part);
    memset(GD.d, 0, sizeof GD.d), GD.ang = 0.0f, GD.tb[0] = GD.tb[1] = 0.0f;
    GD.f[0] = GD.f[1] = GD.f[2] = 1.0f;
    static const char* const an[3] = { "X", "Y", "Z" };
    if (ed->tool == TOOL_MOVE) {
        float d[3] = { 0, 0, 0 };
        if (GD.part == GIZMO_VIEW || (pl >= 0 && GD.fallback)) { // free: the view plane, by the pixels
            for (int k = 0; k < 3; k++) d[k] = (v->right[k] * dx - v->up[k] * dy) * g->px;
            if (pl >= 0) { // (on its plane all the same)
                float dn = dot3(d, GD.ax[pl]);
                for (int k = 0; k < 3; k++) d[k] -= GD.ax[pl][k] * dn;
            }
        } else if (pl >= 0) {
            float hit[3];
            if (ray_plane(v, dir, GD.pivot, GD.ax[pl], hit))
                for (int k = 0; k < 3; k++) d[k] = hit[k] - GD.start[k];
            float dn = dot3(d, GD.ax[pl]);
            for (int k = 0; k < 3; k++) d[k] -= GD.ax[pl][k] * dn;
        } else if (a >= 0) {
            float t = 0.0f;
            if (GD.fallback || !axis_param(v, GD.pivot, GD.ax[a], dir, &t)) // (an arrow at the viewer: the mouse along its screen direction)
                t = (dx * GD.sdx + dy * GD.sdy) * g->px;
            else t -= GD.t0;
            for (int k = 0; k < 3; k++) d[k] = GD.ax[a][k] * t;
        }
        float c[3]; // along the gizmo's axes: the typed values, the snapping
        for (int i = 0; i < 3; i++) c[i] = dot3(d, GD.ax[i]);
        if (GD.typing) {
            if (a >= 0) c[0] = c[1] = c[2] = 0.0f, c[a] = gz_num(0, 0.0f);
            else if (pl >= 0) c[(pl + 1) % 3] = gz_num(0, 0.0f), c[(pl + 2) % 3] = gz_num(1, 0.0f), c[pl] = 0.0f;
            else for (int i = 0; i < 3; i++) c[i] = gz_num(i, 0.0f);
        } else if (snap && gizmo_orient_now(ed, GD.key_alt) != GIZMO_ORIENT_WORLD)
            for (int i = 0; i < 3; i++) c[i] = snapf(c[i], ed->snap_move_step);
        for (int k = 0; k < 3; k++) GD.d[k] = GD.ax[0][k] * c[0] + GD.ax[1][k] * c[1] + GD.ax[2][k] * c[2];
        if (a >= 0) snprintf(GD.header, sizeof GD.header, "%s %s: %.2f cm", TR("Move"), an[a], c[a]);
        else snprintf(GD.header, sizeof GD.header, "%s  X %.2f  Y %.2f  Z %.2f cm", TR("Move"), GD.d[0], GD.d[1], GD.d[2]);
    } else if (ed->tool == TOOL_ROTATE) {
        if (GD.part == GIZMO_TRACKBALL) { // free: one ring radius of motion is a radian
            float k = 1.0f / fmaxf(GZ_RING * GIZMO_PX, 1.0f);
            GD.tb[0] = dx * k, GD.tb[1] = dy * k;
            if (GD.typing) GD.tb[0] = gz_num(0, 0.0f) * DEG2RAD, GD.tb[1] = gz_num(1, 0.0f) * DEG2RAD;
            else if (snap) GD.tb[0] = snapf(GD.tb[0] / DEG2RAD, ed->snap_rotate_step) * DEG2RAD, GD.tb[1] = snapf(GD.tb[1] / DEG2RAD, ed->snap_rotate_step) * DEG2RAD;
            snprintf(GD.header, sizeof GD.header, "%s  %.1f\xc2\xb0  %.1f\xc2\xb0", TR("Trackball"), GD.tb[0] / DEG2RAD, GD.tb[1] / DEG2RAD);
            return;
        }
        const float* n = GD.part == GIZMO_VIEW ? v->fwd : GD.ax[a >= 0 ? a : 0];
        memcpy(GD.n, n, sizeof GD.n);
        float angle;
        if (GD.part == GIZMO_VIEW) { // the outer ring: the angle about the pivot on screen
            float raw = atan2f(GD.vm.y - g->pc.y, GD.vm.x - g->pc.x), raw0 = atan2f(GD.press.y - g->pc.y, GD.press.x - g->pc.x);
            float step = raw - raw0 - GD.last;
            while (step > 3.14159265f) step -= 6.2831853f;
            while (step < -3.14159265f) step += 6.2831853f;
            GD.angle += step, GD.last = raw - raw0;
            angle = -GD.angle; // (screen y down: turned to the view axis's sense)
        } else if (GD.fallback) {
            angle = (dx * GD.sdx + dy * GD.sdy) / fmaxf(GZ_RING * GIZMO_PX, 1.0f);
        } else {
            float hit[3], raw = GD.last;
            if (ray_plane(v, dir, GD.pivot, n, hit)) raw = ring_angle(GD.ax, a, GD.pivot, hit);
            float step = raw - GD.last; // unwrapped: several turns accumulate
            while (step > 3.14159265f) step -= 6.2831853f;
            while (step < -3.14159265f) step += 6.2831853f;
            GD.angle += step, GD.last = raw;
            angle = GD.angle;
        }
        if (GD.typing) angle = gz_num(0, 0.0f) * DEG2RAD;
        else if (snap) angle = snapf(angle / DEG2RAD, ed->snap_rotate_step) * DEG2RAD;
        GD.ang = angle;
        snprintf(GD.header, sizeof GD.header, "%s%s%s  %.2f\xc2\xb0", TR("Rotate"), a >= 0 ? " " : "", a >= 0 ? an[a] : "", angle / DEG2RAD);
    } else if (ed->tool == TOOL_SCALE) {
        float sx0 = GD.press.x - g->pc.x, sy0 = GD.press.y - g->pc.y, cx = GD.vm.x - g->pc.x, cy = GD.vm.y - g->pc.y, f;
        if (a >= 0) { // along the axis on screen, by the ratio of the distances from the pivot
            float ref = sx0 * GD.sdx + sy0 * GD.sdy, cur = cx * GD.sdx + cy * GD.sdy;
            f = fabsf(ref) > 4.0f ? cur / ref : 1.0f + (cur - ref) / fmaxf(GIZMO_PX, 1.0f);
        } else { // from the ring or a square: by the distance; from the center ball: by the motion
            float ds = sqrtf(sx0 * sx0 + sy0 * sy0), dn = sqrtf(cx * cx + cy * cy);
            f = ds > 0.3f * GIZMO_PX ? dn / ds : fmaxf(0.0f, 1.0f + (dn - ds) / fmaxf(GIZMO_PX, 1.0f));
        }
        if (GD.typing) f = gz_num(0, 1.0f);
        f = fabsf(f) < 0.001f ? (f < 0.0f ? -0.001f : 0.001f) : f;
        for (int k = 0; k < 3; k++) GD.f[k] = a >= 0 ? (k == a ? f : 1.0f) : pl >= 0 ? (k == pl ? 1.0f : f) : f;
        snprintf(GD.header, sizeof GD.header, "%s  X %.3f  Y %.3f  Z %.3f", TR("Scale"), GD.f[0], GD.f[1], GD.f[2]);
    }
}

// A turn (axis n through the origin, angle a) as rm[col] = the image of world axis col
static void rot_matrix(const float n[3], float a, float rm[3][3]) {
    float c = cosf(a), s = sinf(a);
    for (int col = 0; col < 3; col++) {
        float e[3] = { (float)(col == 0), (float)(col == 1), (float)(col == 2) }, kxv[3];
        cross3(n, e, kxv);
        float kd = dot3(n, e);
        for (int j = 0; j < 3; j++) rm[col][j] = e[j] * c + kxv[j] * s + n[j] * kd * (1.0f - c);
    }
}
static void mat_vec(const float rm[3][3], const float x[3], float out[3]) {
    for (int j = 0; j < 3; j++) out[j] = rm[0][j] * x[0] + rm[1][j] * x[1] + rm[2][j] * x[2];
}

// The delta on every selected actor, from where each was when the drag began, about the Pivot Point (Individual
// Origins: each its own); Only Locations: the places alone. Move on the World grid snaps each actor's place
static void gizmo_apply(Editor* ed, const View* v, bool snap) {
    float rm[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    if (ed->tool == TOOL_ROTATE) {
        if (GD.part == GIZMO_TRACKBALL) {
            float r1[3][3], r2[3][3];
            // (the face toward the camera follows the mouse: in these axes, X forward, Y right, Z up, a positive turn
            // about up sends the front to the left and one about right sends it up; the Tlender's signs were Blender's)
            rot_matrix(v->up, -GD.tb[0], r1);
            rot_matrix(v->right, -GD.tb[1], r2);
            for (int col = 0; col < 3; col++) mat_vec(r1, r2[col], rm[col]);
        } else rot_matrix(GD.n, GD.ang, rm);
    }
    bool grid = ed->tool == TOOL_MOVE && snap && !GD.typing && gizmo_orient_now(ed, GD.key_alt) == GIZMO_ORIENT_WORLD;
    uint32_t n = ed->nsel ? ed->nsel : 1;
    for (uint32_t i = 0; i < n; i++) {
        Entity e = ed->nsel ? ed->sel[i] : ed->selected;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        if (!t) continue;
        const CTransform* s = ed->nsel ? &ed->sel_start[i] : &ed->gizmo_start;
        const float* P = ed->gizmo_pivot == GIZMO_PIVOT_INDIVIDUAL ? s->location : GD.pivot;
        float off[3] = { s->location[0] - P[0], s->location[1] - P[1], s->location[2] - P[2] };
        if (ed->tool == TOOL_MOVE) {
            for (int k = 0; k < 3; k++) {
                float x = s->location[k] + GD.d[k];
                t->location[k] = grid && fabsf(GD.d[k]) > 1e-4f ? snapf(x, ed->snap_move_step) : x;
            }
        } else if (ed->tool == TOOL_ROTATE) {
            float o2[3];
            mat_vec(rm, off, o2);
            for (int k = 0; k < 3; k++) t->location[k] = P[k] + o2[k];
            if (!ed->gizmo_only_loc) {
                float m[3][3], r[3][3];
                rotator_axes(s->rotation, m);
                for (int a = 0; a < 3; a++) mat_vec(rm, m[a], r[a]);
                axes_rotator(r, t->rotation);
            }
        } else if (ed->tool == TOOL_SCALE) {
            float o2[3] = { 0, 0, 0 };
            for (int a = 0; a < 3; a++) {
                float d = dot3(off, GD.ax[a]) * GD.f[a];
                for (int k = 0; k < 3; k++) o2[k] += GD.ax[a][k] * d;
            }
            for (int k = 0; k < 3; k++) t->location[k] = P[k] + o2[k];
            if (!ed->gizmo_only_loc) { // each of the actor's own axes scaled by the gizmo's scaling (exact along them)
                float m[3][3];
                rotator_axes(s->rotation, m);
                for (int k = 0; k < 3; k++) {
                    float img[3] = { 0, 0, 0 };
                    for (int a = 0; a < 3; a++) {
                        float d = dot3(m[k], GD.ax[a]) * GD.f[a];
                        for (int j = 0; j < 3; j++) img[j] += GD.ax[a][j] * d;
                    }
                    float fk = sqrtf(dot3(img, img)) * (dot3(img, m[k]) < 0.0f ? -1.0f : 1.0f), x = s->scale[k] * fk;
                    if (snap && !GD.typing && fabsf(fk - 1.0f) > 1e-5f) x = fabsf(snapf(x, ed->snap_scale_step)) > 0.0f ? snapf(x, ed->snap_scale_step) : ed->snap_scale_step;
                    t->scale[k] = x;
                }
            }
        }
        ecs_touch(&ed->world, e); // edited in place: the renderer re-reads it
    }
}

// Back to where the drag began (the right button, Esc)
static void gizmo_cancel(Editor* ed) {
    uint32_t n = ed->nsel ? ed->nsel : 1;
    for (uint32_t i = 0; i < n; i++) {
        Entity e = ed->nsel ? ed->sel[i] : ed->selected;
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        if (!t) continue;
        const CTransform* s = ed->nsel ? &ed->sel_start[i] : &ed->gizmo_start;
        memcpy(t->location, s->location, sizeof t->location);
        memcpy(t->rotation, s->rotation, sizeof t->rotation);
        memcpy(t->scale, s->scale, sizeof t->scale);
        ecs_touch(&ed->world, e);
    }
    if (GD.pushed) editor_undo_drop(ed); // (nothing changed: no step)
}

// The keys while dragging: X / Y / Z, Shift for the plane, again for the other orientation, again off; the typed value
static bool gizmo_keys(Editor* ed, const Gizmo* g, const View* v) {
    ImGuiIO* io = igGetIO_Nil();
    static const ImGuiKey axk[3] = { ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z };
    bool changed = false;
    for (int a = 0; a < 3; a++) {
        if (!igIsKeyPressed_Bool(axk[a], false)) continue;
        bool plane = io->KeyShift;
        if (GD.key_axis == a && GD.key_plane == plane) {
            if (!GD.key_alt) GD.key_alt = true; // again: the other orientation
            else GD.key_axis = -1, GD.key_alt = false; // again: off (back to the handle grabbed)
        } else GD.key_axis = a, GD.key_plane = plane, GD.key_alt = false;
        changed = true;
    }
    if (changed) {
        float ax[3][3];
        gizmo_axes(ed, v, &ed->gizmo_start, GD.key_alt, ax);
        memcpy(GD.ax, ax, sizeof ax);
        int part = GD.grabbed;
        if (GD.key_axis >= 0) part = GD.key_plane && ed->tool != TOOL_ROTATE ? GIZMO_YZ + GD.key_axis : GIZMO_X + GD.key_axis;
        gizmo_begin_part(g, v, part, ed->tool);
    }
    // typed values: digits, the point, minus flips, Backspace, Tab the next component
    static const ImGuiKey dig[10] = { ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9 };
    static const ImGuiKey kp[10] = { ImGuiKey_Keypad0, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3, ImGuiKey_Keypad4, ImGuiKey_Keypad5, ImGuiKey_Keypad6, ImGuiKey_Keypad7, ImGuiKey_Keypad8, ImGuiKey_Keypad9 };
    char* s = GD.num[GD.num_i];
    size_t l = strlen(s);
    for (int k = 0; k < 10; k++)
        if ((igIsKeyPressed_Bool(dig[k], true) || igIsKeyPressed_Bool(kp[k], true)) && l < 20) s[l++] = (char)('0' + k), s[l] = 0, GD.typing = true;
    if ((igIsKeyPressed_Bool(ImGuiKey_Period, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadDecimal, false) || igIsKeyPressed_Bool(ImGuiKey_Comma, false)) && !strchr(s, '.') && l < 20)
        s[l++] = '.', s[l] = 0, GD.typing = true;
    if (igIsKeyPressed_Bool(ImGuiKey_Minus, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadSubtract, false)) {
        if (s[0] == '-') memmove(s, s + 1, l);
        else if (l < 20) memmove(s + 1, s, l + 1), s[0] = '-';
        GD.typing = true;
    }
    if (igIsKeyPressed_Bool(ImGuiKey_Backspace, true) && l) s[--l] = 0;
    if (igIsKeyPressed_Bool(ImGuiKey_Tab, false)) GD.num_i = (GD.num_i + 1) % 3, GD.typing = true;
    if (GD.typing && !GD.num[0][0] && !GD.num[1][0] && !GD.num[2][0] && !igIsKeyPressed_Bool(ImGuiKey_Tab, false)) GD.typing = false;
    // the gizmo's size (the Tlender's +/-)
    if (igIsKeyPressed_Bool(ImGuiKey_KeypadAdd, true) || igIsKeyPressed_Bool(ImGuiKey_Equal, true)) ed->gizmo_size = fminf(ed->gizmo_size * 1.1f, 300.0f);
    if (igIsKeyPressed_Bool(ImGuiKey_KeypadSubtract, true) && !GD.typing) ed->gizmo_size = fmaxf(ed->gizmo_size / 1.1f, 40.0f);
    return changed;
}

// The constraint's line through the pivot (an axis: as far as the view goes), the values at the mouse
static void gizmo_overlay(const Editor* ed, ImDrawList* dl, const View* v, ImVec2 m) {
    int a = part_axis(GD.part);
    if (a >= 0 && ed->tool != TOOL_ROTATE) {
        float p0[3], p1[3];
        for (int k = 0; k < 3; k++) p0[k] = GD.pivot[k] - GD.ax[a][k] * 1e5f, p1[k] = GD.pivot[k] + GD.ax[a][k] * 1e5f;
        line3(dl, v, p0, p1, gz_col(a, false, 0.7f), 1.4f);
    }
    char line[320];
    const char* field = GD.typing ? GD.num[GD.num_i] : "";
    if (GD.typing) snprintf(line, sizeof line, "%s   [%s%s]", GD.header, field[0] ? field : "0", GD.num_i ? (GD.num_i == 1 ? " (2)" : " (3)") : "");
    else snprintf(line, sizeof line, "%s", GD.header);
    const char* hint = TR("X/Y/Z: axis (Shift: plane)  |  digits: value  |  Ctrl: snap  |  Shift: precise  |  right click / Esc: cancel");
    ImVec2 ts = igCalcTextSize(line, NULL, false, -1), hs = igCalcTextSize(hint, NULL, false, -1);
    ImVec2 p = { m.x + 18.0f, m.y + 20.0f };
    float w = fmaxf(ts.x, hs.x) + 12.0f;
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x - 6, p.y - 4 }, (ImVec2){ p.x - 6 + w, p.y + ts.y + hs.y + 8 }, THA(TK_N3, 200), 4.0f, 0);
    ImDrawList_AddText_Vec2(dl, p, THA(TK_N15, 255), line, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x, p.y + ts.y + 4 }, THA(TK_N12, 220), hint, NULL);
}

#include "gizmo_test.inc" // (2026-11-23) the gizmo's test by injected mouse and keys

// The mouse's cursor (2026-11-23, copied from the Tlender's ui.c draw_transform_cursor / viewport_cursor): over a
// handle one that says what it drags (four arrows: move, the hand: rotate, diagonal arrows: scale); while dragging the
// system's cursor hides and the transform's is drawn at the mouse, white over a dark outline - four arrows to move, a
// turn about the pivot to rotate (a ring of arrows for the trackball), arrows pointing away from the pivot to scale
static void cursor_arrow(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float thick, float head) {
    float dx = to.x - from.x, dy = to.y - from.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    dx /= len, dy /= len;
    ImVec2 base = { to.x - dx * head, to.y - dy * head };
    ImDrawList_AddLine(dl, from, base, col, thick);
    ImDrawList_AddTriangleFilled(dl, to, (ImVec2){ base.x - dy * head * 0.7f, base.y + dx * head * 0.7f },
                                 (ImVec2){ base.x + dy * head * 0.7f, base.y - dx * head * 0.7f }, col);
}
static void cursor_arc(ImDrawList* dl, float cx, float cy, float r, float a0, float a1, ImU32 col, float thick, float head) {
    ImVec2 pts[24];
    for (int k = 0; k < 24; k++) {
        float a = a0 + (a1 - a0) * (float)k / 23.0f;
        pts[k] = (ImVec2){ cx + cosf(a) * r, cy + sinf(a) * r };
    }
    ImDrawList_AddPolyline(dl, pts + 2, 20, col, thick, 0);
    cursor_arrow(dl, pts[3], pts[0], col, thick, head);
    cursor_arrow(dl, pts[20], pts[23], col, thick, head);
}
static void gizmo_cursor(const Editor* ed, ImDrawList* dl, ImVec2 pivot_px, ImVec2 m) {
    float s = ed->ui_scale > 0.25f ? ed->ui_scale : 1.0f;
    float ux = m.x - pivot_px.x, uy = m.y - pivot_px.y, ul = sqrtf(ux * ux + uy * uy);
    if (ul < 1.0f) ux = 1.0f, uy = 0.0f, ul = 1.0f;
    ux /= ul, uy /= ul;
    for (int pass = 0; pass < 2; pass++) { // a dark outline under the white shape
        ImU32 col = pass == 0 ? THA(TK_N0, 170) : TH(TK_N15);
        float t = (pass == 0 ? 3.6f : 1.6f) * s, head = (pass == 0 ? 6.5f : 5.0f) * s;
        if (ed->tool == TOOL_MOVE) {
            static const float dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++)
                cursor_arrow(dl, (ImVec2){ m.x + dirs[k][0] * 3 * s, m.y + dirs[k][1] * 3 * s },
                             (ImVec2){ m.x + dirs[k][0] * 13 * s, m.y + dirs[k][1] * 13 * s }, col, t, head);
        } else if (ed->tool == TOOL_SCALE) {
            cursor_arrow(dl, (ImVec2){ m.x + ux * 2 * s, m.y + uy * 2 * s }, (ImVec2){ m.x + ux * 13 * s, m.y + uy * 13 * s }, col, t, head);
            cursor_arrow(dl, (ImVec2){ m.x - ux * 2 * s, m.y - uy * 2 * s }, (ImVec2){ m.x - ux * 13 * s, m.y - uy * 13 * s }, col, t, head);
        } else if (GD.part == GIZMO_TRACKBALL) {
            ImDrawList_AddCircle(dl, m, 7 * s, col, 24, t);
            for (int k = 0; k < 4; k++) {
                float a = 1.5707963f * (float)k + 0.7853982f;
                cursor_arrow(dl, (ImVec2){ m.x + cosf(a) * 8 * s, m.y + sinf(a) * 8 * s }, (ImVec2){ m.x + cosf(a) * 15 * s, m.y + sinf(a) * 15 * s }, col, t, head);
            }
        } else { // the turn about the pivot, through the mouse
            float r = ul > 30.0f * s ? ul : 30.0f * s, a = atan2f(uy, ux), span = 16.0f * s / r;
            cursor_arc(dl, m.x - ux * r, m.y - uy * r, r, a - span, a + span, col, t, head);
        }
    }
}

// --- Several selected actors ---

static void store_selection_start(Editor* ed) {
    for (uint32_t i = 0; i < ed->nsel; i++) {
        const CTransform* t = ecs_get(&ed->world, ed->sel[i], COMP_TRANSFORM);
        if (t) ed->sel_start[i] = *t;
    }
}

// The box selection: the actors (shown in the editor) whose pivot projects inside the rectangle
static void box_select(Editor* ed, const View* v, float x0, float y0, float x1, float y1, SelMode mode) {
    if (mode == SEL_REPLACE) editor_select_none(ed);
    World* w = &ed->world;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (tr[i].flags & TRANSFORM_HIDDEN) continue;
            ImVec2 s;
            if (!project_point(v, tr[i].location, &s) || s.x < x0 || s.x > x1 || s.y < y0 || s.y > y1) continue;
            editor_select(ed, ents[i], mode == SEL_REMOVE ? SEL_REMOVE : SEL_ADD);
        }
    }
}

// Shift+D's grab (Blender's): the selection follows the mouse on the ground plane through its pivot (the view plane
// when the camera looks along the ground); a left click keeps it, a right click or Esc puts it back
void editor_grab_begin(Editor* ed) {
    if (!ed->nsel || ed->playing) return;
    store_selection_start(ed);
    ed->grab = true;
    ed->grab_started = false;
    ed->tool = TOOL_MOVE;
}

static void grab_update(Editor* ed, const View* v, ImVec2 m, ImDrawList* dl) {
    if (!ed->grab) return;
    const CTransform* pt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    if (!pt || !ed->nsel) {
        ed->grab = false;
        return;
    }
    uint32_t pi = ed->nsel - 1; // the primary one is the last
    const float* pivot = ed->sel_start[pi].location;
    float up[3] = { 0, 0, 1 }, dir[3], p[3];
    const float* n = fabsf(v->fwd[2]) > 0.2f ? up : v->fwd;
    mouse_ray(v, m, dir);
    bool hit = ray_plane(v, dir, pivot, n, p);
    if (!ed->grab_started) { // the plane point under the mouse when the grab began
        if (!hit) return;
        memcpy(ed->grab_p0, p, sizeof ed->grab_p0);
        ed->grab_started = true;
    }
    bool cancel = igIsMouseClicked_Bool(ImGuiMouseButton_Right, false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false);
    float delta[3] = { 0, 0, 0 };
    if (hit && !cancel)
        for (int k = 0; k < 3; k++) delta[k] = p[k] - ed->grab_p0[k];
    for (uint32_t i = 0; i < ed->nsel; i++) {
        CTransform* t = ecs_get(&ed->world, ed->sel[i], COMP_TRANSFORM);
        if (!t) continue;
        for (int k = 0; k < 3; k++) {
            float x = ed->sel_start[i].location[k] + delta[k];
            t->location[k] = ed->snap_move && fabsf(delta[k]) > 1e-4f ? snapf(x, ed->snap_move_step) : x;
        }
        ecs_touch(&ed->world, ed->sel[i]);
    }
    ImDrawList_AddText_Vec2(dl, (ImVec2){ m.x + 16, m.y + 16 }, THA(TK_N15, 230),
                            TR("Left click: place  |  Right click / Esc: cancel"), NULL);
    if (cancel || igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) ed->grab = false;
}

// --- Camera ---

static void fly(Editor* ed, EdCamera* cam, double dt, bool look) {
    double cx, cy;
    glfwGetCursorPos(ed->window, &cx, &cy);
    if (look) {
        cam->yaw += (float)(cx - ed->last_cursor[0]) * ed->mouse_sens; // (Editor Preferences > Viewport)
        cam->pitch -= (float)(cy - ed->last_cursor[1]) * ed->mouse_sens * (ed->invert_y ? -1.0f : 1.0f);
        if (cam->pitch > 89.0f) cam->pitch = 89.0f;
        if (cam->pitch < -89.0f) cam->pitch = -89.0f;
        cam->yaw = fmodf(cam->yaw, 360.0f);
    }
    ed->last_cursor[0] = cx;
    ed->last_cursor[1] = cy;
    float f[3], r[3], u[3];
    camera_basis(cam, f, r, u);
    float mx = (float)(igIsKeyDown_Nil(ImGuiKey_D) - igIsKeyDown_Nil(ImGuiKey_A));
    float mz = (float)(igIsKeyDown_Nil(ImGuiKey_W) - igIsKeyDown_Nil(ImGuiKey_S));
    float my = (float)(igIsKeyDown_Nil(ImGuiKey_E) - igIsKeyDown_Nil(ImGuiKey_Q));
    float step = cam->speed * (float)dt * (igGetIO_Nil()->KeyShift ? 3.0f : 1.0f);
    for (int k = 0; k < 3; k++) cam->pos[k] += (f[k] * mz + r[k] * mx) * step;
    cam->pos[2] += my * step; // Q/E: world down/up, like Unreal
}

// --- The orthographic views (sugestão #66: Unreal's Top / Bottom / Left / Right / Front / Back in the main viewport) ---

const char* const g_vp_proj_names[VP_PROJ_COUNT] = { "Perspective", "Top", "Bottom", "Left", "Right", "Front", "Back" };

// An orthographic view's axes, the camera's own basis (a mirrored one would turn the triangles' winding and show the
// meshes' insides): Top looks down with +X up the screen, Front looks along +X, Left along +Y; Bottom, Back and Right
// are their opposites (the same up: forward and right both turned, the winding kept)
static void ortho_axes(int proj, float fwd[3], float right[3], float up[3]) {
    int base = proj == VP_PROJ_BOTTOM ? VP_PROJ_TOP : proj == VP_PROJ_BACK ? VP_PROJ_FRONT : proj == VP_PROJ_RIGHT ? VP_PROJ_LEFT : proj;
    EdCamera c = { 0 };
    c.yaw = base == VP_PROJ_LEFT ? 90.0f : 0.0f;
    c.pitch = base == VP_PROJ_TOP ? -90.0f : 0.0f;
    camera_basis(&c, fwd, right, up);
    if (base != proj)
        for (int k = 0; k < 3; k++) fwd[k] = -fwd[k], right[k] = -right[k];
}

void viewport_set_projection(Editor* ed, int proj) {
    if (proj < 0 || proj >= VP_PROJ_COUNT || proj == ed->vp_proj) return;
    if (ed->vp_proj == VP_PROJ_PERSPECTIVE) { // from the perspective camera: what it looked at, where its view meets the ground
        float f[3], r[3], u[3];
        camera_basis(&ed->cam, f, r, u);
        float t = f[2] < -0.05f ? fminf(-ed->cam.pos[2] / f[2], 5000.0f) : 1000.0f;
        for (int k = 0; k < 3; k++) ed->vp_target[k] = ed->cam.pos[k] + f[k] * t;
        if (ed->vp_ortho < 50.0f) ed->vp_ortho = fmaxf(t * 0.6f, 500.0f);
    }
    ed->vp_proj = proj;
    if (ed->flying) {
        ed->flying = false;
        glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

void viewport_focus(Editor* ed, Entity e) {
    const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    if (!t) return;
    float s = fmaxf(fabsf(t->scale[0]), fmaxf(fabsf(t->scale[1]), fabsf(t->scale[2])));
    float dist = 250.0f * (s > 0.1f ? s : 0.1f);
    if (ed->vp_proj != VP_PROJ_PERSPECTIVE) { // orthographic: centered on it, its size filling about half the view
        memcpy(ed->vp_target, t->location, sizeof ed->vp_target);
        ed->vp_ortho = fmaxf(dist * 0.8f, 100.0f);
        return;
    }
    float f[3], r[3], u[3];
    camera_basis(&ed->cam, f, r, u);
    for (int k = 0; k < 3; k++) ed->cam.pos[k] = t->location[k] - f[k] * dist;
}

// The player's view while possessed: an auto-activated Camera actor if the level has one (fixed, like Unreal's),
// else the spectator camera that flies
// The possessed Blueprint pawn's camera (its boom), else an auto-activated Camera actor
static float g_pie_roll; // (Q12) the player's view's roll (a First Person Camera's lean, a shake): its basis turned
static bool player_camera(Editor* ed, EdCamera* out) {
    float pos[3], yaw, pitch, fov = 90.0f;
    g_pie_roll = 0.0f;
    if (bp_player_camera_ex(&ed->world, pos, &yaw, &pitch, &g_pie_roll, &fov)) {
        memcpy(out->pos, pos, sizeof out->pos);
        out->yaw = yaw;
        out->pitch = pitch;
        out->fov_deg = fov;
        out->speed = ed->cam.speed;
        return true;
    }
    return level_player_camera(&ed->world, out);
}

// --- (2026-11-10, "editar visualmente, não só nos Detalhes") handles on the selected actor, in the viewport ---
// A River's / a Rail's spline: its points as dots (drag: along the ground at their height; Shift: up and down; Alt at the
// start: a copy of the point inserted after it, the copy dragged, as Unreal's; the right button: add after / delete).
// A Prototyping shape: a handle on each side face and on top (drag: that face moves, the opposite one stays), and one on
// its door (drag: the door slides along the wall). Over a handle the gizmo and the selection wait.
enum { HK_NONE = 0, HK_SPLINE, HK_PROTO, HK_TANGENT }; // (Q14) HK_TANGENT: a Spline point's tangent handle (axis 0 arrive, 1 leave)
static struct {
    int kind, index;          // the handle under the mouse / being dragged
    Entity e;
    bool active;
    float start[3];           // the handle's world place when the drag began
    float grab[3];            // where the mouse ray met its plane / axis then
    CProto pstart;            // (a shape: its numbers then)
    float lstart[3];          // (its location then)
    float grab_t;             // (a shape: where along its axis the mouse ray was when it began)
    int menu_index;           // (the right button's point)
    Entity menu_e;
    int axis;                 // (a spline point's gizmo: the arrow grabbed, 0 X, 1 Y, 2 Z; -1 the point itself)
    int sel;                  // the spline point clicked last: it shows the 3 arrows (-1 none)
    Entity sel_e;
} g_hd = { .sel = -1, .axis = -1 };

// A selected spline point's gizmo, as Unreal's: an arrow along each world axis, its length the same on the screen
static bool spline_axis_ends(const View* v, const float wp[3], int a, ImVec2* s0, ImVec2* s1) {
    float d = sqrtf((wp[0] - v->ro[0]) * (wp[0] - v->ro[0]) + (wp[1] - v->ro[1]) * (wp[1] - v->ro[1]) + (wp[2] - v->ro[2]) * (wp[2] - v->ro[2]));
    float L = fmaxf(d, 1.0f) * 0.11f, e[3] = { wp[0], wp[1], wp[2] }, b[3] = { wp[0], wp[1], wp[2] };
    e[a] += L, b[a] += L * 0.12f;
    return project_point(v, b, s0) && project_point(v, e, s1);
}
static float seg_dist2(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy;
    float t = l > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l : 0.0f;
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    float x = a.x + dx * t - p.x, y = a.y + dy * t - p.y;
    return x * x + y * y;
}

static void local_to_world(const CTransform* t, const float l[3], float w[3]) {
    float ax[3][3];
    rotator_axes(t->rotation, ax);
    for (int k = 0; k < 3; k++) w[k] = t->location[k];
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) w[k] += ax[a][k] * l[a] * t->scale[a];
}
static void world_to_local(const CTransform* t, const float w[3], float l[3]) {
    float ax[3][3], d[3] = { w[0] - t->location[0], w[1] - t->location[1], w[2] - t->location[2] };
    rotator_axes(t->rotation, ax);
    for (int a = 0; a < 3; a++) l[a] = dot3(d, ax[a]) / (fabsf(t->scale[a]) > 1e-6f ? t->scale[a] : 1e-6f);
}

// The spline's points of e (a River or a Rail): their array, count and room; NULL: none
static float (*spline_points(World* w, Entity e, uint32_t** n, uint32_t* cap))[3] {
    CRiver* r = ecs_get(w, e, COMP_RIVER);
    if (r) {
        *n = &r->npoints, *cap = RIVER_MAX_POINTS;
        return r->points;
    }
    CRail* rl = ecs_get(w, e, COMP_RAIL);
    if (rl && spline_kind(rl) != SPLINE_KIND_CABLE) { // (Q14: a Rail or a Spline; a Cable has no points)
        *n = &rl->npoints, *cap = RAIL_MAX_POINTS;
        return rl->points;
    }
    CProto* pr = ecs_get(w, e, COMP_PROTO); // (2026-11-10) a prototyping shape along a spline: the same handles as a river's
    if (pr && pr->npoints >= 2) {
        *n = &pr->npoints, *cap = PROTO_MAX_POINTS;
        return pr->points;
    }
    return NULL;
}

// A point of the spline inserted after i (a copy of it; a River's width, depth and speed copied too). (Q14) A Spline's:
// halfway along the curve to the next (copy: on the point, the Alt drag's), its roll, scale and type with it
static void spline_insert(World* w, Entity e, int i, bool copy) {
    uint32_t* n;
    uint32_t cap;
    float(*pts)[3] = spline_points(w, e, &n, &cap);
    if (!pts || *n >= cap || i < 0 || (uint32_t)i >= *n) return;
    CRail* rl = ecs_get(w, e, COMP_RAIL);
    if (rl && !ecs_get(w, e, COMP_RIVER)) {
        float at[3];
        memcpy(at, rl->points[i], 12);
        if (spline_insert_point(rl, i) && copy) memcpy(rl->points[i + 1], at, 12);
        return;
    }
    memmove(&pts[i + 2], &pts[i + 1], sizeof(float[3]) * (*n - (uint32_t)i - 1));
    CRiver* r = ecs_get(w, e, COMP_RIVER);
    if (r) {
        memmove(&r->width[i + 2], &r->width[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
        memmove(&r->depth[i + 2], &r->depth[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
        memmove(&r->speed[i + 2], &r->speed[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
        r->width[i + 1] = r->width[i], r->depth[i + 1] = r->depth[i], r->speed[i + 1] = r->speed[i];
    }
    memcpy(pts[i + 1], pts[i], sizeof(float[3]));
    (*n)++;
}
static void spline_delete(World* w, Entity e, int i) {
    uint32_t* n;
    uint32_t cap;
    float(*pts)[3] = spline_points(w, e, &n, &cap);
    if (!pts || *n <= 2 || i < 0 || (uint32_t)i >= *n) return;
    CRail* rl = ecs_get(w, e, COMP_RAIL);
    if (rl && !ecs_get(w, e, COMP_RIVER)) {
        spline_remove_point(rl, i);
        return;
    }
    memmove(&pts[i], &pts[i + 1], sizeof(float[3]) * (*n - (uint32_t)i - 1));
    CRiver* r = ecs_get(w, e, COMP_RIVER);
    if (r) {
        memmove(&r->width[i], &r->width[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
        memmove(&r->depth[i], &r->depth[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
        memmove(&r->speed[i], &r->speed[i + 1], sizeof(float) * (*n - (uint32_t)i - 1));
    }
    (*n)--;
}

// A prototyping shape's handles (local): 0 +X, 1 -X, 2 +Y, 3 -Y, 4 top, 5 the door (when it has one); false: none
static bool proto_handle(const CProto* p, int i, float out[3]) {
    float L = p->size[0], W = p->size[1], H = p->size[2];
    switch (i) {
    case 0: out[0] = L * 0.5f, out[1] = 0, out[2] = H * 0.5f; return true;
    case 1: out[0] = -L * 0.5f, out[1] = 0, out[2] = H * 0.5f; return true;
    case 2: out[0] = 0, out[1] = W * 0.5f, out[2] = H * 0.5f; return p->kind != PROTO_WALL;
    case 3: out[0] = 0, out[1] = -W * 0.5f, out[2] = H * 0.5f; return p->kind != PROTO_WALL;
    case 4: out[0] = 0, out[1] = 0, out[2] = H; return true;
    case 5: {
        bool door = (p->kind == PROTO_WALL || p->kind == PROTO_ROOM || (p->kind == PROTO_CORRIDOR && (p->flags & PROTO_DOOR_FRONT))) && p->opening[0] >= 1.0f;
        if (!door) return false;
        out[0] = p->opening[2], out[1] = p->kind == PROTO_WALL ? 0.0f : -W * 0.5f, out[2] = fminf(p->opening[1], H) * 0.5f;
        return true;
    }
    }
    return false;
}

// (Q14) A Spline point's tangent handles (world): arrive behind it, leave ahead, at a third of each (the Hermite's Bezier
// control points). Rivers and Rails have auto tangents only: none
static bool tangent_handles(World* w, Entity e, const CTransform* t, int i, float out[2][3]) {
    CRail* rl = ecs_get(w, e, COMP_RAIL);
    if (!rl || ecs_get(w, e, COMP_RIVER) || spline_kind(rl) != SPLINE_KIND_SPLINE || i < 0 || (uint32_t)i >= rl->npoints) return false;
    static SplineCurve c;
    spline_curve_from(&c, rl, NULL);
    float a[3], b[3];
    for (int k = 0; k < 3; k++) a[k] = c.p[i][k] - c.arrive[i][k] / 3.0f, b[k] = c.p[i][k] + c.leave[i][k] / 3.0f;
    local_to_world(t, a, out[0]);
    local_to_world(t, b, out[1]);
    return true;
}

// The mouse ray's closest point to the line through o along d (unit): its parameter along d
static float ray_line_t(const View* v, const float dir[3], const float o[3], const float d[3]) {
    float w0[3] = { v->ro[0] - o[0], v->ro[1] - o[1], v->ro[2] - o[2] };
    float a = dot3(dir, dir), b = dot3(dir, d), c = dot3(d, d), dd = dot3(dir, w0), e = dot3(d, w0);
    float den = a * c - b * b;
    return fabsf(den) < 1e-9f ? 0.0f : (a * e - b * dd) / den;
}

// Draws the selected actor's handles and runs their drags; true while one is under the mouse or dragged (the gizmo and
// the click selection wait)
static bool handles_frame(Editor* ed, ImDrawList* dl, const View* v, ImVec2 mouse, bool can_click) {
    World* w = &ed->world;
    Entity e = ed->selected;
    if (!e || ed->nsel > 1 || ed->playing || ed->game_view) {
        g_hd.active = false, g_hd.kind = HK_NONE;
        return false;
    }
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!t) return false;
    ImGuiIO* io = igGetIO_Nil();
    float dir[3];
    mouse_ray(v, mouse, dir);
    uint32_t* np = NULL;
    uint32_t cap = 0;
    float(*pts)[3] = spline_points(w, e, &np, &cap);
    CProto* pr = ecs_get(w, e, COMP_PROTO);
    if (!pts && !pr) {
        g_hd.active = false, g_hd.kind = HK_NONE;
        return false;
    }
    if (g_hd.active && g_hd.e != e) g_hd.active = false;
    if (g_hd.sel_e != e || !pts || g_hd.sel >= (int)*np) g_hd.sel = -1;
    // the handle under the mouse (none while one is dragged: it keeps its own)
    if (!g_hd.active) {
        g_hd.kind = HK_NONE, g_hd.index = -1, g_hd.axis = -1;
        float best = 14.0f * 14.0f;
        int count = pts ? (int)*np : 6;
        for (int i = 0; i < count; i++) {
            float lp[3], wp[3];
            if (pts) memcpy(lp, pts[i], sizeof lp);
            else if (!proto_handle(pr, i, lp)) continue;
            local_to_world(t, lp, wp);
            ImVec2 sp;
            if (!project_point(v, wp, &sp)) continue;
            float dx = sp.x - mouse.x, dy = sp.y - mouse.y;
            if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, g_hd.kind = pts ? HK_SPLINE : HK_PROTO, g_hd.index = i;
        }
        if (g_hd.sel >= 0 && g_hd.kind == HK_NONE) { // the selected point's arrows
            float wp[3];
            local_to_world(t, pts[g_hd.sel], wp);
            float ab = 7.0f * 7.0f;
            for (int a = 0; a < 3; a++) {
                ImVec2 s0, s1;
                if (!spline_axis_ends(v, wp, a, &s0, &s1)) continue;
                float d2 = seg_dist2(mouse, s0, s1);
                if (d2 < ab) ab = d2, g_hd.kind = HK_SPLINE, g_hd.index = g_hd.sel, g_hd.axis = a;
            }
        }
        if (g_hd.sel >= 0 && g_hd.kind == HK_NONE) { // (Q14) its tangents' handles (a Spline's)
            float hw[2][3];
            if (tangent_handles(w, e, t, g_hd.sel, hw)) {
                float tb = 9.0f * 9.0f;
                for (int k = 0; k < 2; k++) {
                    ImVec2 sp;
                    if (!project_point(v, hw[k], &sp)) continue;
                    float dx = sp.x - mouse.x, dy = sp.y - mouse.y;
                    if (dx * dx + dy * dy < tb) tb = dx * dx + dy * dy, g_hd.kind = HK_TANGENT, g_hd.index = g_hd.sel, g_hd.axis = k;
                }
            }
        }
        g_hd.e = e;
    }
    bool hot = g_hd.kind != HK_NONE && ed->vp_hovered;
    // the drag begins
    if (!g_hd.active && hot && can_click && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        editor_undo_push(ed);
        if (g_hd.kind == HK_SPLINE) g_hd.sel = g_hd.index, g_hd.sel_e = e; // (clicked: its gizmo shows)
        if (g_hd.kind == HK_SPLINE && io->KeyAlt && *np < cap) { // Alt: a copy of the point, dragged
            spline_insert(w, e, g_hd.index, true);
            pts = spline_points(w, e, &np, &cap);
            g_hd.index++;
            g_hd.sel = g_hd.index;
        }
        float lp[3];
        if (g_hd.kind == HK_SPLINE) memcpy(lp, pts[g_hd.index], sizeof lp);
        else if (g_hd.kind == HK_PROTO) proto_handle(pr, g_hd.index, lp);
        if (g_hd.kind == HK_TANGENT) {
            float hw[2][3];
            tangent_handles(w, e, t, g_hd.index, hw);
            memcpy(g_hd.start, hw[g_hd.axis], sizeof g_hd.start);
            CRail* trl = ecs_get(w, e, COMP_RAIL);
            if (trl) spline_set_point_type(trl, g_hd.index, SPLINE_POINT_CUSTOM); // (as Unreal: a dragged tangent is a custom one)
        } else local_to_world(t, lp, g_hd.start);
        if (pr) g_hd.pstart = *pr;
        memcpy(g_hd.lstart, t->location, sizeof g_hd.lstart);
        g_hd.active = true;
        memcpy(g_hd.grab, g_hd.start, sizeof g_hd.grab);
        if (g_hd.kind == HK_TANGENT) {
            float n[3] = { 0, 0, 1 }, hit[3];
            if (ray_plane(v, dir, g_hd.start, n, hit)) memcpy(g_hd.grab, hit, sizeof hit);
        } else if (g_hd.kind == HK_SPLINE && g_hd.axis >= 0) {
            float ad[3] = { 0, 0, 0 };
            ad[g_hd.axis] = 1.0f;
            g_hd.grab_t = ray_line_t(v, dir, g_hd.start, ad);
        } else if (g_hd.kind == HK_SPLINE) {
            float n[3] = { 0, 0, 1 }, hit[3];
            if (ray_plane(v, dir, g_hd.start, n, hit)) memcpy(g_hd.grab, hit, sizeof hit);
        } else {
            float ax[3][3];
            rotator_axes(t->rotation, ax);
            int i = g_hd.index, a = i <= 1 ? 0 : i <= 3 ? 1 : i == 4 ? 2 : 0;
            g_hd.grab_t = ray_line_t(v, dir, g_hd.start, ax[a]);
        }
    }
    if (hot && !g_hd.active && igIsMouseClicked_Bool(ImGuiMouseButton_Right, false) && g_hd.kind == HK_SPLINE) {
        g_hd.menu_index = g_hd.index, g_hd.menu_e = e;
        igOpenPopup_Str("spline_point_menu", 0);
    }
    if (igBeginPopup("spline_point_menu", 0)) {
        if (g_hd.menu_e == e && pts) {
            if (ui_menu_item(ICON_SPLINE, TR("Add Spline Point"), NULL, false, *np < cap)) editor_undo_push(ed), spline_insert(w, e, g_hd.menu_index, false), ecs_touch(w, e), ed->dirty = true;
            if (ui_menu_item(ICON_DELETE, TR("Delete Spline Point"), NULL, false, *np > 2)) editor_undo_push(ed), spline_delete(w, e, g_hd.menu_index), ecs_touch(w, e), ed->dirty = true;
        }
        igEndPopup();
    }
    // the drag
    if (g_hd.active) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) g_hd.active = false;
        else if (g_hd.kind == HK_SPLINE && pts && g_hd.index < (int)*np) {
            float wp[3];
            memcpy(wp, g_hd.start, sizeof wp);
            if (g_hd.axis >= 0) { // an arrow of the gizmo: along that axis only
                float ad[3] = { 0, 0, 0 };
                ad[g_hd.axis] = 1.0f;
                wp[g_hd.axis] = g_hd.start[g_hd.axis] + ray_line_t(v, dir, g_hd.start, ad) - g_hd.grab_t;
            } else if (io->KeyShift) { // up and down: the vertical line through the point
                float up[3] = { 0, 0, 1 };
                wp[2] = g_hd.start[2] + ray_line_t(v, dir, g_hd.start, up);
            } else {
                float n[3] = { 0, 0, 1 }, hit[3];
                if (ray_plane(v, dir, g_hd.start, n, hit))
                    for (int k = 0; k < 2; k++) wp[k] = g_hd.start[k] + hit[k] - g_hd.grab[k];
            }
            world_to_local(t, wp, pts[g_hd.index]);
            ecs_touch(w, e), ed->dirty = true;
        } else if (g_hd.kind == HK_TANGENT && pts && g_hd.index < (int)*np) { // (Q14) the tangent: both sides together (Unreal's)
            CRail* trl = ecs_get(w, e, COMP_RAIL);
            float wp[3];
            memcpy(wp, g_hd.start, sizeof wp);
            if (io->KeyShift) {
                float up[3] = { 0, 0, 1 };
                wp[2] = g_hd.start[2] + ray_line_t(v, dir, g_hd.start, up);
            } else {
                float n[3] = { 0, 0, 1 }, hit[3];
                if (ray_plane(v, dir, g_hd.start, n, hit))
                    for (int k = 0; k < 2; k++) wp[k] = g_hd.start[k] + hit[k] - g_hd.grab[k];
            }
            float lh[3];
            world_to_local(t, wp, lh);
            if (trl) {
                float sg = g_hd.axis == 1 ? 3.0f : -3.0f; // (the handles sit at a third of the tangent: Hermite's control points)
                for (int k = 0; k < 3; k++) trl->leave[g_hd.index][k] = trl->arrive[g_hd.index][k] = (lh[k] - trl->points[g_hd.index][k]) * sg;
            }
            ecs_touch(w, e), ed->dirty = true;
        } else if (g_hd.kind == HK_PROTO && pr) {
            float ax[3][3];
            rotator_axes(t->rotation, ax);
            int i = g_hd.index;
            int a = i <= 1 ? 0 : i <= 3 ? 1 : i == 4 ? 2 : 0; // (the door slides along x)
            float sgn = (i == 1 || i == 3) ? -1.0f : 1.0f;
            float d = ray_line_t(v, dir, g_hd.start, ax[a]) - g_hd.grab_t; // (along the axis, from where it was grabbed)
            d = roundf(d); // (whole centimeters)
            *pr = g_hd.pstart;
            if (i == 5) pr->opening[2] = g_hd.pstart.opening[2] + d;
            else {
                float ns = fmaxf(g_hd.pstart.size[a] + sgn * d, 10.0f), grown = ns - g_hd.pstart.size[a];
                pr->size[a] = ns;
                if (a < 2) // the opposite face stays: the middle moves half the growth that way
                    for (int k = 0; k < 3; k++) t->location[k] = g_hd.lstart[k] + ax[a][k] * sgn * grown * 0.5f;
            }
            ecs_touch(w, e), ed->dirty = true;
        }
    }
    // drawn: the spline's line and points; a shape's handles (the hot / dragged one brighter)
    if (pts) { // (Q14) the curve itself (the engine's one evaluation: spline.h), 12 steps a segment
        static SplineCurve sc;
        CRail* crl = ecs_get(w, e, COMP_RAIL);
        if (crl && !ecs_get(w, e, COMP_RIVER)) spline_curve_from(&sc, crl, NULL);
        else spline_curve_points(&sc, (const float(*)[3])pts, *np, false, NULL);
        float prev[3];
        uint32_t ns = spline_segment_count(&sc), steps = ns * 12u;
        for (uint32_t i = 0; i <= steps; i++) {
            float lp[3], wp[3];
            spline_eval_key(&sc, (float)i / 12.0f, lp, NULL);
            local_to_world(t, lp, wp);
            if (i) line3(dl, v, prev, wp, THA(TK_AMBER, 220), 2.0f);
            memcpy(prev, wp, sizeof prev);
        }
        float hw[2][3], pw[3];
        if (g_hd.sel >= 0 && g_hd.sel < (int)*np && tangent_handles(w, e, t, g_hd.sel, hw)) { // the selected point's tangent
            local_to_world(t, pts[g_hd.sel], pw);
            for (int k = 0; k < 2; k++) {
                line3(dl, v, pw, hw[k], THA(TK_CYAN, 200), 1.5f);
                ImVec2 sp;
                if (!project_point(v, hw[k], &sp)) continue;
                bool on = (g_hd.kind == HK_TANGENT) && g_hd.axis == k;
                ImDrawList_AddRectFilled(dl, (ImVec2){ sp.x - 5, sp.y - 5 }, (ImVec2){ sp.x + 5, sp.y + 5 }, on ? TH(TK_N15) : TH(TK_CYAN), 1.0f, 0);
            }
        }
    }
    int count = pts ? (int)*np : 6;
    for (int i = 0; i < count; i++) {
        float lp[3], wp[3];
        if (pts) memcpy(lp, pts[i], sizeof lp);
        else if (!proto_handle(pr, i, lp)) continue;
        local_to_world(t, lp, wp);
        ImVec2 sp;
        if (!project_point(v, wp, &sp)) continue;
        bool on = g_hd.kind != HK_NONE && g_hd.index == i;
        ImU32 col = i == 5 && !pts ? TH(TK_CYAN) : pts ? TH(TK_AMBER) : TH(TK_ORANGE);
        float r = on ? 8.0f : 6.0f;
        if (pts) ImDrawList_AddCircleFilled(dl, sp, r, on ? TH(TK_N15) : col, 16);
        else ImDrawList_AddRectFilled(dl, (ImVec2){ sp.x - r, sp.y - r }, (ImVec2){ sp.x + r, sp.y + r }, on ? TH(TK_N15) : col, 2.0f, 0);
        ImDrawList_AddCircle(dl, sp, r + 1.5f, THA(TK_N0, 200), 16, 1.5f);
    }
    if (pts && g_hd.sel >= 0 && g_hd.sel < (int)*np) { // the selected point's gizmo: X red, Y green, Z blue
        float wp[3];
        local_to_world(t, pts[g_hd.sel], wp);
        static const ImU32 axc[3] = { TK_RED, TK_LIME, TK_BLUE };
        for (int a = 0; a < 3; a++) {
            ImVec2 s0, s1;
            if (!spline_axis_ends(v, wp, a, &s0, &s1)) continue;
            bool on = g_hd.kind == HK_SPLINE && g_hd.axis == a;
            ImU32 c = on ? TH(TK_AMBER) : TH(axc[a]);
            ImDrawList_AddLine(dl, s0, s1, c, on ? 4.0f : 3.0f);
            float dx = s1.x - s0.x, dy = s1.y - s0.y, l = sqrtf(dx * dx + dy * dy);
            if (l < 1.0f) continue;
            dx /= l, dy /= l;
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ s1.x + dx * 12, s1.y + dy * 12 }, (ImVec2){ s1.x - dy * 6, s1.y + dx * 6 }, (ImVec2){ s1.x + dy * 6, s1.y - dx * 6 }, c);
        }
    }
    if (hot && !g_hd.active) {
        const char* tip = g_hd.kind == HK_TANGENT ? TR("Drag: the point's tangent (it becomes a Curve Custom Tangent; Shift: up and down).") : g_hd.kind == HK_SPLINE && g_hd.axis >= 0 ? TR("Drag: move the point along this axis.") : g_hd.kind == HK_SPLINE ? TR("Drag: move the point (Shift: up and down, Alt: a new point). Right button: add / delete.")
                        : g_hd.index == 5 ? TR("Drag: slide the door along the wall.") : TR("Drag: move this face (the opposite one stays).");
        igSetTooltip("%s", tip);
    }
    return hot || g_hd.active;
}

void viewport_draw(Editor* ed, float x, float y, float w, float h, double dt) {
    ImGuiIO* io = igGetIO_Nil();
    ed->vp_rect[0] = x; ed->vp_rect[1] = y; ed->vp_rect[2] = w; ed->vp_rect[3] = h;
    // The whole area is one invisible button: ImGui's hover/click tests then respect the windows above it
    igSetCursorScreenPos((ImVec2){ x, y });
    igSetNextItemAllowOverlap(); // the viewport toolbar's items come after and take the clicks
    igInvisibleButton("##viewport", (ImVec2){ w > 1 ? w : 1, h > 1 ? h : 1 }, // tips: none (a canvas or a tile drawn with its own name: no description over the work)
                      ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ed->vp_hovered = igIsItemHovered(0);
    bool vp_active = igIsItemActive();

    bool pie_view = ed->playing && ed->possessed;
    EdCamera fixed;
    bool fixed_cam = pie_view && player_camera(ed, &fixed);
    EdCamera* cam = fixed_cam ? &fixed : pie_view ? &ed->pie_cam : &ed->cam;

    bool ortho = !pie_view && ed->vp_proj > VP_PROJ_PERSPECTIVE && ed->vp_proj < VP_PROJ_COUNT;
    if (ortho) { // Unreal's orthographic controls: the right or middle button pans, the wheel zooms toward the mouse
        if (ed->vp_ortho < 50.0f) ed->vp_ortho = 1500.0f;
        float f[3], r[3], u[3];
        ortho_axes(ed->vp_proj, f, r, u);
        float ppc = h * 0.5f / ed->vp_ortho;
        bool pan = igIsMouseDown_Nil(ImGuiMouseButton_Right) || igIsMouseDown_Nil(ImGuiMouseButton_Middle);
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Right)) g_gz_rmb_hold = false;
        if (pan && (ed->vp_hovered || vp_active) && !ed->gizmo_active && !g_gz_rmb_hold)
            for (int k = 0; k < 3; k++) ed->vp_target[k] += (-r[k] * io->MouseDelta.x + u[k] * io->MouseDelta.y) / ppc;
        if (ed->vp_hovered && io->MouseWheel != 0.0f) {
            float ox = (io->MousePos.x - (x + w * 0.5f)) / ppc, oy = ((y + h * 0.5f) - io->MousePos.y) / ppc;
            float half = fminf(fmaxf(ed->vp_ortho * powf(1.2f, -io->MouseWheel), 50.0f), 1.0e6f), s = 1.0f - half / ed->vp_ortho;
            for (int k = 0; k < 3; k++) ed->vp_target[k] += (r[k] * ox + u[k] * oy) * s; // (the point under the mouse stays)
            ed->vp_ortho = half;
        }
        if (ed->flying) {
            ed->flying = false;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    } else if (!pie_view) {
        bool rmb = glfwGetMouseButton(ed->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        if (!rmb) g_gz_rmb_hold = false;
        if (!ed->flying && rmb && ed->vp_hovered && !ed->gizmo_active && !g_gz_rmb_hold) {
            ed->flying = true;
            glfwGetCursorPos(ed->window, &ed->last_cursor[0], &ed->last_cursor[1]);
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else if (ed->flying && !rmb) {
            ed->flying = false;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        if (ed->flying) {
            if (io->MouseWheel != 0.0f) cam->speed = fminf(fmaxf(cam->speed * powf(1.25f, io->MouseWheel), 10.0f), 20000.0f);
            fly(ed, cam, dt, true);
        } else if (ed->vp_hovered) {
            float f[3], r[3], u[3];
            camera_basis(cam, f, r, u);
            if (io->MouseWheel != 0.0f)
                for (int k = 0; k < 3; k++) cam->pos[k] += f[k] * io->MouseWheel * 50.0f;
            if (igIsMouseDown_Nil(ImGuiMouseButton_Middle))
                for (int k = 0; k < 3; k++) cam->pos[k] += (-r[k] * io->MouseDelta.x + u[k] * io->MouseDelta.y) * 1.5f;
        }
    } else if (!ed->mouse_released && !fixed_cam) {
        fly(ed, cam, ed->paused ? 0.0 : dt, true); // the spectator (no pawn yet)
    }

    View v;
    camera_basis(cam, v.fwd, v.right, v.up);
    if (fixed_cam && g_pie_roll != 0.0f) level_roll_basis(g_pie_roll, v.fwd, v.right, v.up); // (Q12)
    memcpy(v.pos, cam->pos, sizeof v.pos);
    v.cx = x + w * 0.5f;
    v.cy = y + h * 0.5f;
    v.focal = (w * 0.5f) / tanf(cam->fov_deg * 0.5f * DEG2RAD); // horizontal FOV, like Unreal
    v.ppc = 0.0f;
    if (ortho) { // (the point looked at is the view's position: what is around the camera, the GI and the shadows, is there)
        ortho_axes(ed->vp_proj, v.fwd, v.right, v.up);
        memcpy(v.pos, ed->vp_target, sizeof v.pos);
        v.ppc = h * 0.5f / ed->vp_ortho;
    }
    memcpy(v.ro, v.pos, sizeof v.ro);

    // What main.c renders behind the UI (framebuffer pixels)
    RenderView* rv = &ed->render_view;
    float fs = io->DisplayFramebufferScale.x > 0 ? io->DisplayFramebufferScale.x : 1.0f;
    memcpy(rv->pos, v.pos, sizeof rv->pos);
    memcpy(rv->fwd, v.fwd, sizeof rv->fwd);
    memcpy(rv->right, v.right, sizeof rv->right);
    memcpy(rv->up, v.up, sizeof rv->up);
    rv->fov_deg = cam->fov_deg;
    rv->ortho_half = ortho ? ed->vp_ortho : 0.0f;
    ImGuiViewport* mvp = igGetMainViewport(); // (multi-viewport: the UI's coordinates are the desktop's; the framebuffer starts at the window)
    rv->x = floorf((x - mvp->Pos.x) * fs);
    rv->y = floorf((y - mvp->Pos.y) * fs);
    rv->w = floorf(w * fs);
    rv->h = floorf(h * fs);
    rv->view_mode = ed->view_mode;
    rv->show_grid = ed->show_grid && !ed->game_view && !pie_view;
    rv->editor = !ed->playing;
    rv->selected = pie_view ? ENTITY_NONE : ed->selected;
    rv->selection = pie_view ? NULL : ed->sel;
    rv->selection_count = pie_view ? 0 : ed->nsel;
    // the game's highlight while playing (interaction.h: the Interactable in the player's focus and its moving part)
    rv->highlight[0] = ed->playing ? interaction_highlight(&ed->world, &rv->highlight_mode) : ENTITY_NONE;
    rv->highlight[1] = rv->highlight[0] ? interaction_highlight_part(&ed->world) : ENTITY_NONE;
    if (!rv->highlight[0]) rv->highlight_mode = 0;
    rv->exposure_ev100 = ed->exposure_ev100;
    rv->exposure_comp = ed->exposure_comp;
    rv->auto_min_ev100 = ed->auto_min_ev100;
    rv->auto_max_ev100 = fmaxf(ed->auto_max_ev100, ed->auto_min_ev100);
    rv->auto_exposure = ed->auto_exposure;
    rv->aerial_scale = ed->aerial_scale;
    rv->msaa = ed->msaa;
    rv->gi = ed->gi;
    static PostSettings post; // (the Post Process Volumes at the camera)
    post_fill_view(&ed->world, rv->pos, ed->playing ? bp_player(&ed->world) : ENTITY_NONE, ed->playing, &post, rv);

    // A material (or a texture) dragged over a mesh: Unreal's drop onto the object - the one under the mouse outlined and
    // named while hovering. (2026-11-23, the user: "show where it will be applied": it never showed - it waited for the
    // payload not to be accepted, and this viewport accepts it while hovered)
    const ImGuiPayload* drag = !pie_view ? igGetDragDropPayload() : NULL;
    int dk = drag && !strcmp(drag->DataType, RE_PAYLOAD_ASSET) && drag->DataSize == (int)sizeof(AssetPayload) ? (int)((const AssetPayload*)drag->Data)->kind : -1;
    if ((dk == ASSET_MATERIAL || dk == ASSET_MATERIAL_INSTANCE || dk == ASSET_TEXTURE_SOURCE) && igIsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        Entity hit = pick(ed, &v, io->MousePos);
        if (hit && ecs_get(&ed->world, hit, COMP_STATIC_MESH)) {
            const CName* hn = ecs_get(&ed->world, hit, COMP_NAME);
            const char* mb = strrchr(((const AssetPayload*)drag->Data)->vpath, '/');
            if (dk == ASSET_TEXTURE_SOURCE) igSetTooltip(TR("Apply a material of %s to %s"), mb ? mb + 1 : "", hn ? hn->text : "");
            else igSetTooltip(TR("Apply %s to %s"), mb ? mb + 1 : "", hn ? hn->text : "");
            rv->selected = hit; // its outline while the material hovers it
        }
    }
    // An asset dropped from the Content Browser: placed where the mouse ray meets the ground (or 5 m ahead)
    if (!pie_view && igBeginDragDropTarget()) {
        const ImGuiPayload* pp = igAcceptDragDropPayload(RE_PAYLOAD_PLACE, 0); // (a class from Add / Place Actors)
        if (pp && pp->DataSize == (int)sizeof(PlacePayload)) {
            float dir[3], at[3], n[3] = { 0, 0, 1 }, o[3] = { 0, 0, 0 };
            mouse_ray(&v, io->MousePos, dir);
            if (!ray_plane(&v, dir, o, n, at) || fabsf(at[0] - v.ro[0]) + fabsf(at[1] - v.ro[1]) > 100000.0f)
                for (int k = 0; k < 3; k++) at[k] = v.ro[k] + dir[k] * (v.ppc > 0.0f ? ORTHO_BACK : 500.0f);
            editor_place_drop(ed, ((const PlacePayload*)pp->Data)->index, at);
        }
        const ImGuiPayload* pl = igAcceptDragDropPayload(RE_PAYLOAD_ASSET, 0);
        Entity tex_hit = pl && ((const AssetPayload*)pl->Data)->kind == ASSET_TEXTURE_SOURCE ? pick(ed, &v, io->MousePos) : ENTITY_NONE;
        if (tex_hit && !ecs_get(&ed->world, tex_hit, COMP_STATIC_MESH)) tex_hit = ENTITY_NONE;
        if (pl && (((const AssetPayload*)pl->Data)->kind == ASSET_MATERIAL || ((const AssetPayload*)pl->Data)->kind == ASSET_MATERIAL_INSTANCE || tex_hit)) {
            const AssetPayload* p = pl->Data;
            Entity hit = tex_hit ? tex_hit : pick(ed, &v, io->MousePos);
            CStaticMesh* sm = hit ? ecs_get(&ed->world, hit, COMP_STATIC_MESH) : NULL;
            char path[700], mvp[600] = "";
            snprintf(path, sizeof path, "%s%s", content_root(), p->vpath + 8);
            if (tex_hit && sm) { // (2026-11-23) a texture on a mesh: its material (made once beside it: M_<texture>, its sibling maps found)
                char folder[600], stem[200], made[600] = "", msg[300];
                snprintf(folder, sizeof folder, "%s", p->vpath);
                char* sl = strrchr(folder, '/');
                const char* bn = sl ? sl + 1 : p->vpath;
                snprintf(stem, sizeof stem, "%s", bn);
                char* dot = strrchr(stem, '.');
                if (dot) *dot = 0;
                if (sl) *sl = 0;
                snprintf(mvp, sizeof mvp, "%s/M_%s.rmat", folder, stem);
                char disk[700];
                snprintf(disk, sizeof disk, "%s%s", content_root(), mvp + 8);
                if (!re_file_stat(disk, NULL, NULL, NULL) && content_make(p->vpath, CM_MATERIAL, folder, made, sizeof made, msg, sizeof msg) && made[0])
                    snprintf(mvp, sizeof mvp, "%s", made);
                snprintf(path, sizeof path, "%s%s", content_root(), mvp + 8);
            }
            Material* m = sm ? mat_load(path) : NULL;
            if (m) {
                editor_undo_push(ed);
                sm->material = m->hash;
                ecs_touch(&ed->world, hit);
                editor_select(ed, hit, SEL_REPLACE);
                ed->dirty = true;
                ed->drawer_open = false;
            } else editor_status(ed, igGetTime(), TR("Drop the material on a mesh."));
            pl = NULL;
        }
        if (pl) {
            const AssetPayload* p = pl->Data;
            float dir[3], at[3];
            mouse_ray(&v, io->MousePos, dir);
            float n[3] = { 0, 0, 1 }, o[3] = { 0, 0, 0 };
            if (!ray_plane(&v, dir, o, n, at) || fabsf(at[0] - v.ro[0]) + fabsf(at[1] - v.ro[1]) > 100000.0f)
                for (int k = 0; k < 3; k++) at[k] = v.ro[k] + dir[k] * (v.ppc > 0.0f ? ORTHO_BACK : 500.0f); // (orthographic: on the plane looked at)
            if (p->kind == ASSET_BASIC_SHAPE) {
                if (p->shape != SHAPE_PLANE && fabsf(at[2]) < 1.0f) at[2] = 50.0f; // resting on the ground
                for (int k = 0; k < 2; k++) at[k] = roundf(at[k] / 10.0f) * 10.0f; // a 10 cm grid
                editor_undo_push(ed);
                editor_spawn_shape(ed, p->shape, at, igGetTime());
                ed->drawer_open = false; // like Unreal: the drawer gets out of the way after a placement
            } else if (p->kind == ASSET_STATIC_MESH || p->kind == ASSET_SKELETAL_MESH) { // an imported mesh / a skeletal one
                editor_undo_push(ed);
                World* w = &ed->world;
                bool skel = p->kind == ASSET_SKELETAL_MESH;
                Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(skel ? COMP_SKELETAL_MESH : COMP_STATIC_MESH));
                if (e) {
                    const char* base = strrchr(p->vpath, '/');
                    char name[RE_NAME_MAX];
                    snprintf(name, sizeof name, "%s", base ? base + 1 : p->vpath);
                    char* dot = strrchr(name, '.');
                    if (dot) *dot = 0;
                    char uname[RE_NAME_MAX];
                    editor_unique_name(ed, name, uname, sizeof uname);
                    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", uname);
                    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                    memcpy(t->location, at, 12);
                    t->flags = MOBILITY_MOVABLE;
                    if (skel) {
                        SkelMesh* m = anim_mesh_load(p->vpath);
                        CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
                        if (m) c->mesh = m->hash;
                    } else {
                        CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
                        sm->mesh = render_mesh_id_of(p->vpath);
                        sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f, sm->roughness = 0.5f, sm->cast_shadow = 1;
                        sm->material = content_mesh_material(p->vpath); // (its import's, as Unreal's mesh keeps its slots)
                    }
                    editor_select(ed, e, SEL_REPLACE);
                    ed->dirty = true;
                }
                ed->drawer_open = false;
            } else if (p->kind == ASSET_VEHICLE) { // a Vehicle: its actor standing on its wheels where it fell
                VehicleAsset* a = veh_load(p->vpath);
                if (a) {
                    editor_undo_push(ed);
                    float loc[3] = { at[0], at[1], at[2] + veh_rest_height(a) };
                    char uname[RE_NAME_MAX];
                    editor_unique_name(ed, a->name, uname, sizeof uname);
                    Entity e = vehicle_spawn(&ed->world, a->hash, loc, ed->cam.yaw, uname);
                    if (e) editor_select(ed, e, SEL_REPLACE), ed->dirty = true;
                }
                ed->drawer_open = false;
            } else if (p->kind == ASSET_PARTICLE_SYSTEM) { // a Particle System: an actor playing it where it fell
                ParticleSystemAsset* a = ps_load(p->vpath);
                if (a) {
                    editor_undo_push(ed);
                    Entity e = particles_spawn(&ed->world, a->hash, at, NULL, false);
                    if (e) {
                        char uname[RE_NAME_MAX];
                        editor_unique_name(ed, a->name, uname, sizeof uname);
                        snprintf(((CName*)ecs_get(&ed->world, e, COMP_NAME))->text, RE_NAME_MAX, "%s", uname);
                        editor_select(ed, e, SEL_REPLACE);
                        ed->dirty = true;
                    }
                }
                ed->drawer_open = false;
            } else {
                if (p->kind == ASSET_BLUEPRINT || p->kind == ASSET_SCRIPT) { // (a script's class: placed like a Blueprint's)
                    ScriptFile* sf = p->kind == ASSET_SCRIPT ? script_load(p->vpath) : NULL;
                    BpAsset* cls = p->kind == ASSET_SCRIPT ? (sf ? sf->cls : NULL) : bp_load(p->vpath);
                    if (cls) {
                        if (fabsf(at[2]) < 1.0f) at[2] = bp_native_of(cls) == BPN_CHARACTER ? 100.0f : 50.0f; // on the ground
                        editor_undo_push(ed);
                        Entity e = bp_spawn(&ed->world, cls, at, (float[3]){ 0, 0, 0 });
                        if (e) {
                            char name[RE_NAME_MAX];
                            editor_unique_name(ed, cls->name, name, sizeof name);
                            CName* nm = ecs_get(&ed->world, e, COMP_NAME);
                            if (nm) snprintf(nm->text, sizeof nm->text, "%s", name);
                            editor_select(ed, e, SEL_REPLACE);
                            ed->dirty = true;
                        }
                        ed->drawer_open = false;
                    }
                } else if (p->kind == ASSET_RASSET && !strncmp(p->vpath, "/Content", 8)) { // (#164) the exchange format: its assets
                    char src[1100], folder[512];                                          // next to it, its objects placed
                    snprintf(src, sizeof src, "%s%s", content_root(), p->vpath + 8);
                    snprintf(folder, sizeof folder, "%s", p->vpath);
                    char* dot = strrchr(folder, '.');
                    if (dot) *dot = 0;
                    for (char* c = strrchr(folder, '/'); c && *c; c++)
                        if (*c == ' ' || *c == '.') *c = '_';
                    if (editor_import_tlend(ed, src, folder, true)) editor_status(ed, igGetTime(), TR("Imported %s."), p->vpath);
                    else editor_status(ed, igGetTime(), TR("Could not import %s."), p->vpath);
                    ed->drawer_open = false;
                } else
                editor_status(ed, igGetTime(), TR("%s can't be placed yet: importing to .rasset comes in step 2."),
                              asset_kind_name(p->kind));
            }
        }
        igEndDragDropTarget();
    }

    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_PushClipRect(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, true);
    bool editing = !pie_view && !ed->game_view;
    if (editor_cost_on()) { // View Mode > Object Cost: the object under the mouse (else in the middle), 10 times a second
        static double next;
        double t = igGetTime();
        if (t >= next) {
            next = t + 0.1;
            ImVec2 at = ed->vp_hovered ? io->MousePos : (ImVec2){ x + w * 0.5f, y + h * 0.5f };
            editor_cost_set_hover(pick(ed, &v, at));
        }
    }
    if (editing && ed->show_icons) draw_icons(ed, dl, &v);
    if (ed->playing && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_Apostrophe, false)) ed->show_ai = !ed->show_ai;
    if (ed->playing && ed->show_ai) draw_ai(ed, dl, &v);
    if (ed->show_nav || editing) draw_navmesh(ed, dl, &v, ed->show_nav); // (the Nav Links always while editing, as Unreal's)
    if (!pie_view && ecs_alive(&ed->world, ed->selected)) draw_selected_shape(ed, dl, &v);

    // Gizmo: hover, drag, release (the undo history gets the state from before the drag). While dragging, the gizmo is
    // the one the drag began with (its pivot and axes): the handles follow the mouse, not the moving actor
    CTransform* sel_t = pie_view ? NULL : ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    bool gizmo_on = sel_t && ed->tool != TOOL_SELECT && !ed->flying;
    ImVec2 mouse = io->MousePos;
    // Landscape sculpting: the brush where the mouse meets the ground, the left button paints (Shift: the opposite)
    bool sculpt = !pie_view && ed->land_tool && ecs_get(&ed->world, ed->selected, COMP_LANDSCAPE) && !ed->flying;
    if (sculpt) {
        gizmo_on = false;
        float dir[3], far[3], hit[3];
        mouse_ray(&v, mouse, dir);
        for (int k = 0; k < 3; k++) far[k] = v.ro[k] + dir[k] * 1.0e6f;
        float t = landscape_trace(v.ro, far);
        bool on = t < 1.0f;
        for (int k = 0; k < 3; k++) hit[k] = v.ro[k] + (far[k] - v.ro[k]) * t;
        if (on && ed->vp_hovered) { // the brush's ring on the ground
            ImVec2 prev = { 0 }, q;
            bool have = false;
            for (int i = 0; i <= 48; i++) {
                float a = (float)i / 48.0f * 6.2831853f, p[3] = { hit[0] + cosf(a) * ed->land_radius, hit[1] + sinf(a) * ed->land_radius, hit[2] }, gz;
                if (landscape_height(p[0], p[1], &gz)) p[2] = gz + 5.0f;
                bool ok = project_point(&v, p, &q);
                if (ok && have) ImDrawList_AddLine(dl, prev, q, ed->land_tool == LAND_TOOL_LOWER ? TH(TK_ORANGE) : TH(TK_LIME), 2.0f);
                prev = q, have = ok;
            }
        }
        bool down = igIsMouseDown_Nil(ImGuiMouseButton_Left) && (ed->land_stroke || (ed->vp_hovered && igIsItemClicked(ImGuiMouseButton_Left)));
        if (down && on) {
            int tool = ed->land_tool;
            if (io->KeyShift) tool = tool == LAND_TOOL_RAISE ? LAND_TOOL_LOWER : tool == LAND_TOOL_LOWER ? LAND_TOOL_RAISE : tool == LAND_TOOL_PAINT ? LAND_TOOL_ERASE
                                    : tool == LAND_TOOL_ERASE ? LAND_TOOL_PAINT : tool;
            landscape_brush(&ed->world, ed->selected, tool, hit, ed->land_radius, ed->land_strength, io->DeltaTime);
            ed->land_stroke = true, ed->dirty = true;
        } else if (ed->land_stroke && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            landscape_stroke_end(&ed->world, ed->selected);
            ed->land_stroke = false;
        }
    } else if (ed->land_stroke) {
        landscape_stroke_end(&ed->world, ed->selected);
        ed->land_stroke = false;
    }
    // Foliage painting (Unreal's Foliage mode): the brush on the ground (the landscapes, else the floor's plane), the left
    // button paints the checked types up to their density, Shift (or the Erase tool) erases them
    bool painting = !pie_view && ed->foliage_mode && ecs_get(&ed->world, ed->selected, COMP_FOLIAGE) && !ed->flying;
    if (painting) {
        gizmo_on = false;
        float dir[3], far[3], hit[3];
        mouse_ray(&v, mouse, dir);
        for (int k = 0; k < 3; k++) far[k] = v.ro[k] + dir[k] * 1.0e6f;
        float t = landscape_trace(v.ro, far);
        bool on = t < 1.0f;
        if (on)
            for (int k = 0; k < 3; k++) hit[k] = v.ro[k] + (far[k] - v.ro[k]) * t;
        else if (dir[2] < -1e-4f) { // (no landscape there: the floor's plane)
            float s = -v.ro[2] / dir[2];
            for (int k = 0; k < 3; k++) hit[k] = v.ro[k] + dir[k] * s;
            on = s > 0.0f;
        }
        bool erase = ed->fol_erase != io->KeyShift;
        if (on && ed->vp_hovered) {
            ImVec2 prev = { 0 }, q;
            bool have = false;
            for (int i = 0; i <= 48; i++) {
                float a = (float)i / 48.0f * 6.2831853f, p[3] = { hit[0] + cosf(a) * ed->fol_radius, hit[1] + sinf(a) * ed->fol_radius, hit[2] }, gz;
                if (landscape_height(p[0], p[1], &gz)) p[2] = gz + 5.0f;
                bool ok = project_point(&v, p, &q);
                if (ok && have) ImDrawList_AddLine(dl, prev, q, erase ? TH(TK_RED) : TH(TK_GREEN), 2.0f);
                prev = q, have = ok;
            }
        }
        bool down = igIsMouseDown_Nil(ImGuiMouseButton_Left) && (ed->fol_stroke || (ed->vp_hovered && igIsItemClicked(ImGuiMouseButton_Left)));
        if (down && on) {
            foliage_paint(&ed->world, ed->selected, hit, ed->fol_radius, ed->fol_density, erase, io->DeltaTime);
            ed->fol_stroke = true, ed->dirty = true;
        } else if (ed->fol_stroke && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            foliage_stroke_end(&ed->world, ed->selected);
            ed->fol_stroke = false;
        }
    } else if (ed->fol_stroke) {
        foliage_stroke_end(&ed->world, ed->selected);
        ed->fol_stroke = false;
    }
    // the selected actor's handles (a spline's points, a shape's faces): over one, the gizmo and the selection wait
    const bool handle_busy = !pie_view && !sculpt && !ed->gizmo_active && handles_frame(ed, dl, &v, mouse, true);
    Gizmo gz;
    g_gz_px = fmaxf(ed->gizmo_size > 0.0f ? ed->gizmo_size : 100.0f, 40.0f) * (ed->ui_scale > 0.25f ? ed->ui_scale : 1.0f); // (2026-11-23: Editor Preferences' Gizmo Size)
    if (gizmo_on) { // while dragging: the drag's frame (its pivot and axes), else the selection's pivot and the orientation's axes
        float ax[3][3], piv[3];
        if (ed->gizmo_active) memcpy(ax, GD.ax, sizeof ax), memcpy(piv, GD.pivot, sizeof piv);
        else gizmo_axes(ed, &v, sel_t, false, ax), gizmo_pivot_point(ed, sel_t, piv);
        gizmo_on = gizmo_setup(&gz, &v, piv, ax);
    }
    if (gizmo_on) {
        if (!ed->gizmo_active) ed->gizmo_hot = ed->vp_hovered && !handle_busy ? gizmo_hit(ed, &gz, &v, mouse) : GIZMO_NONE;
        if (!ed->gizmo_active && ed->gizmo_hot && igIsItemClicked(ImGuiMouseButton_Left)) { // (the undo step at the first change)
            memset(&GD, 0, sizeof GD);
            GD.key_axis = -1;
            ed->gizmo_active = ed->gizmo_hot;
            ed->gizmo_start = *sel_t;
            store_selection_start(ed);
            memcpy(GD.ax, gz.ax, sizeof GD.ax), memcpy(GD.pivot, gz.c, sizeof GD.pivot);
            GD.press = GD.vm = mouse;
            int part = ed->gizmo_hot; // Shift on an axis: its plane (the Tlender's: the other two axes)
            if (io->KeyShift && part_axis(part) >= 0 && ed->tool != TOOL_ROTATE) part = GIZMO_YZ + part_axis(part);
            GD.grabbed = part;
            gizmo_begin_part(&gz, &v, part, ed->tool);
        }
        if (ed->gizmo_active) {
            bool cancel = igIsMouseClicked_Bool(ImGuiMouseButton_Right, false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false);
            bool held = vp_active || igIsMouseDown_Nil(ImGuiMouseButton_Left);
            if (cancel) {
                gizmo_cancel(ed);
                g_gz_rmb_hold = igIsMouseDown_Nil(ImGuiMouseButton_Right);
                ed->gizmo_active = GIZMO_NONE;
            } else {
                gizmo_keys(ed, &gz, &v);
                float k = io->KeyShift ? 0.1f : 1.0f; // (Shift: a tenth of the motion, from where it is: no jump)
                GD.vm.x += io->MouseDelta.x * k, GD.vm.y += io->MouseDelta.y * k;
                bool snap = (ed->tool == TOOL_MOVE ? ed->snap_move : ed->tool == TOOL_ROTATE ? ed->snap_rotate : ed->snap_scale) != io->KeyCtrl;
                gizmo_delta(ed, &gz, &v, snap);
                bool moved = GD.typing || fabsf(GD.vm.x - GD.press.x) + fabsf(GD.vm.y - GD.press.y) >= 1.0f;
                if (moved && !GD.pushed) editor_undo_push(ed), GD.pushed = true; // (the actors still where they began)
                if (GD.pushed) gizmo_apply(ed, &v, snap);
                if (!held) ed->gizmo_active = GIZMO_NONE; // let go: kept (a click that never moved: no undo step)
            }
        }
        // drawn around the actor where it is now (move: the pivot moved by the drag); the drag's part alone while dragging
        Gizmo shown = gz;
        if (ed->gizmo_active && ed->tool == TOOL_MOVE) {
            for (int k = 0; k < 3; k++) shown.c[k] = GD.pivot[k] + GD.d[k];
            project_point(&v, shown.c, &shown.pc);
        }
        int part = ed->gizmo_active ? GD.part : GIZMO_NONE;
        gizmo_draw(ed, dl, &shown, &v, ed->gizmo_active ? part : ed->gizmo_hot, part);
        if (ed->gizmo_active) gizmo_overlay(ed, dl, &v, mouse);
        if (ed->gizmo_active) { // (the Tlender's cursors: drawn while dragging, the system's over a handle)
            igSetMouseCursor(ImGuiMouseCursor_None);
            gizmo_cursor(ed, dl, shown.pc, mouse);
        } else if (ed->gizmo_hot)
            igSetMouseCursor(ed->tool == TOOL_ROTATE ? ImGuiMouseCursor_Hand : ed->tool == TOOL_SCALE ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeAll);
    } else {
        ed->gizmo_active = ed->gizmo_hot = GIZMO_NONE;
    }
    gizmo_test_frame(ed, gizmo_on ? &gz : NULL, &v);

    // A click on the scene selects (Shift adds, Ctrl toggles; on nothing: deselects); a drag from empty space is a box
    // selection (Shift adds, Ctrl removes), released over the actors whose pivot it encloses
    if (!pie_view && !ed->gizmo_active && !ed->grab && !sculpt && !handle_busy) {
        if (!ed->gizmo_hot && igIsItemClicked(ImGuiMouseButton_Left)) {
            ed->marquee = true;
            ed->marquee_p0[0] = mouse.x;
            ed->marquee_p0[1] = mouse.y;
        }
        if (ed->marquee) {
            float x0 = fminf(ed->marquee_p0[0], mouse.x), y0 = fminf(ed->marquee_p0[1], mouse.y);
            float x1 = fmaxf(ed->marquee_p0[0], mouse.x), y1 = fmaxf(ed->marquee_p0[1], mouse.y);
            bool dragged = x1 - x0 > 4.0f || y1 - y0 > 4.0f;
            if (dragged) {
                ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, THA(TK_BLUE, 40), 0, 0);
                ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, THA(TK_BLUE_L, 220), 0.0f, 1.0f, 0);
            }
            if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
                ed->marquee = false;
                if (dragged) {
                    box_select(ed, &v, x0, y0, x1, y1, io->KeyCtrl ? SEL_REMOVE : io->KeyShift ? SEL_ADD : SEL_REPLACE);
                } else {
                    Entity e = pick(ed, &v, mouse);
                    if (io->KeyCtrl) editor_select(ed, e, SEL_TOGGLE);
                    else if (io->KeyShift) editor_select(ed, e, SEL_ADD);
                    else editor_select(ed, e, SEL_REPLACE);
                }
            }
        }
    } else {
        ed->marquee = false;
    }
    if (!pie_view) grab_update(ed, &v, mouse, dl);

    // Overlays: while playing, the PIE hints and a border; the frame's costs (Stat)
    char info[200];
    if (ed->playing && !movie_render_stats()->active) { // (B9: a movie's frames without them)
        snprintf(info, sizeof info, "%s%s  |  %s", ed->possessed ? TR("Play In Editor (possessed)") : ed->simulating ? TR("Simulating") : TR("Play In Editor (ejected)"),
                 ed->paused ? TR("  -  PAUSED") : "",
                 ed->possessed ? (ed->mouse_released ? TR("Click the viewport to capture the mouse  |  F8 eject  |  Esc stop")
                                                     : TR("Shift+F1 release mouse  |  F8 eject  |  Pause  |  Esc stop"))
                               : TR("F8 possess  |  Esc stop"));
        ImDrawList_AddRect(dl, (ImVec2){ x + 1, y + 1 }, (ImVec2){ x + w - 1, y + h - 1 },
                           ed->paused ? TH(TK_AMBER) : TH(TK_GREEN), 0.0f, 3.0f, 0);
        ImVec2 ts = igCalcTextSize(info, NULL, false, -1);
        ImVec2 tp = { x + 12, y + h - ts.y - 14 };
        ImDrawList_AddRectFilled(dl, (ImVec2){ tp.x - 6, tp.y - 4 }, (ImVec2){ tp.x + ts.x + 6, tp.y + ts.y + 4 }, THA(TK_N2, 176), 4, 0);
        ImDrawList_AddText_Vec2(dl, tp, TH(TK_N15), info, NULL);
    }
    if (!pie_view && render_stats()->unbuilt) { // Unreal's warning: a Static mesh changed since the bake
        char warn[128];
        snprintf(warn, sizeof warn, TR("LIGHTING NEEDS TO BE REBUILT (%u unbuilt objects)"), render_stats()->unbuilt);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 12, y + igGetFrameHeight() + 18 }, TH(TK_RED), warn, NULL);
    }
    if (ed->show_stats) {
        const RenderStats* s = render_stats();
        const GiStats* g = gi_stats();
        char st[600];
        snprintf(st, sizeof st,
                 "GPU  scene %.3f  GI %.3f  shadow %.3f  main %.3f  mask %.3f  exposure %.3f  post %.3f  composite %.3f ms\n"
                 "CPU  render %.3f ms   %u/%u meshes  %u draws  %u lights  %ux%u  MSAA %ux\n"
                 "GI   %s  %u probes (%u per frame, %u rays)  TLAS %u  surface cache %u charts, %u texels (%u per frame)\n"
                 "Exposure  %s  EV100 %.1f  (scene mean %.0f nits)",
                 s->gpu_scene_ms, s->gpu_gi_ms, s->gpu_shadow_ms, s->gpu_main_ms, s->gpu_mask_ms, s->gpu_exposure_ms, s->gpu_post_ms, s->gpu_composite_ms, s->cpu_ms,
                 s->visible, s->instances, s->draws, s->lights, s->width, s->height, s->msaa,
                 gi_available() ? "ray tracing" : "off (no ray tracing)", g->probes, g->probes_per_frame, g->rays_per_frame,
                 g->tlas_instances, gi_cache_stats()->charts, gi_cache_stats()->texels, gi_cache_stats()->per_frame,
                 ed->auto_exposure ? "auto" : "manual",
                 s->auto_exposure > 0.0f ? log2f(exp2f(ed->exposure_comp) / (1.2f * s->auto_exposure)) : 0.0f, exp2f(s->avg_log_lum));
        if (ed->view_mode == VIEW_PATH_TRACING) {
            size_t len = strlen(st);
            snprintf(st + len, sizeof st - len, "\nPath tracing: %u samples", s->pt_samples);
        }
        ImVec2 ts = igCalcTextSize(st, NULL, false, -1);
        ImVec2 p = { x + w - ts.x - 14, y + igGetFrameHeight() + 16 };
        ImDrawList_AddRectFilled(dl, (ImVec2){ p.x - 6, p.y - 4 }, (ImVec2){ p.x + ts.x + 6, p.y + ts.y + 4 }, THA(TK_N2, 160), 4, 0);
        ImDrawList_AddText_Vec2(dl, p, TH(TK_GREEN_L), st, NULL);
    }
    { // (Q15) the PCG components' points (Show Points, the nodes marked Debug) and the selected PCG Volume's box
        const RenderView* prv = &ed->render_view;
        pcg_draw_debug(dl, prv->pos, prv->fwd, prv->right, prv->up, prv->fov_deg, x, y, x + w, y + h, &ed->world, ed->playing ? ENTITY_NONE : ed->selected);
    }
    if (ed->playing) { // UMG: the widgets on the screen (over the view, under the messages); their Buttons take the mouse
        ImGuiIO* uio = igGetIO_Nil();
        bool free_mouse = !ed->possessed || ed->mouse_released || ui_mouse_shown();
        if (ui_mouse_shown() && ed->possessed && !ed->mouse_released) { // (Set Show Mouse Cursor: the cursor out, the look stops)
            ed->mouse_released = true;
            glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        float mp[2] = { uio->MousePos.x, uio->MousePos.y };
        float rel[2] = { mp[0] - x, mp[1] - y }; // (Event Touch, Get Mouse Position: the view's pixels)
        bp_touch_update(&ed->world, free_mouse && ed->vp_hovered ? rel : NULL, igIsMouseDown_Nil(ImGuiMouseButton_Left));
        progress_set_view_rect(x * uio->DisplayFramebufferScale.x, y * uio->DisplayFramebufferScale.y, (x + w) * uio->DisplayFramebufferScale.x,
                               (y + h) * uio->DisplayFramebufferScale.y); // (a save's picture: the view, not the editor around it)
        hud_set_view(ed->render_view.pos, ed->render_view.fwd, ed->render_view.right, ed->render_view.up, ed->render_view.fov_deg); // (the HUD's markers: hud.h)
        ui_draw_viewport(&ed->world, dl, x, y, x + w, y + h, free_mouse && ed->vp_hovered ? mp : NULL, igIsMouseDown_Nil(ImGuiMouseButton_Left),
                         igIsMouseClicked_Bool(ImGuiMouseButton_Left, false));
        const RenderView* rv = &ed->render_view; // (the Health's damage numbers, over the widgets' layer)
        ui_draw_damage_numbers(dl, rv->pos, rv->fwd, rv->right, rv->up, rv->fov_deg, x, y, x + w, y + h);
        quest_draw_markers(dl, rv->pos, rv->fwd, rv->right, rv->up, rv->fov_deg, x, y, x + w, y + h); // (the quests' markers: quest_hud.c)
    }
    if (ed->playing) { // Print String's lines, newest on top, fading out (Unreal's)
        const char* lines[16];
        float ages[16];
        uint32_t n = bp_messages(lines, ages, 16);
        float ly = y + igGetFrameHeight() + 16;
        for (uint32_t i = 0; i < n; i++) {
            float alpha = ages[i] > 4.0f ? 5.0f - ages[i] : 1.0f;
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 13, ly + 1 }, THA(TK_N0, (int)(alpha * 200)), lines[i], NULL);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 12, ly }, THA(TK_CYAN, (int)(alpha * 255)), lines[i], NULL);
            ly += igGetTextLineHeight() + 2;
        }
    }
    ImDrawList_PopClipRect(dl);

    // Possessed with the mouse released: a click in the viewport captures it again
    if (pie_view && ed->mouse_released && !ui_mouse_shown() && ed->vp_hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        ed->mouse_released = false;
        glfwGetCursorPos(ed->window, &ed->last_cursor[0], &ed->last_cursor[1]);
        glfwSetInputMode(ed->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}
