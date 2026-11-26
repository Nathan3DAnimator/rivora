// (2026-11-25, item 18) The kit's Level Design plugins (plugin_kit.h): Scatter, Snap to Ground, Align and Distribute,
// Array, Measure.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_kit.h"

#include "debug_draw.h"
#include "i18n.h"
#include "theme.h"
#include "ui_number.h"

static uint32_t rng_next(uint32_t* s) {
    uint32_t x = *s ? *s : 2654435769u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return *s = x;
}
static float rng01(uint32_t* s) { return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f); }

static CTransform* xf(Editor* ed, Entity e) { return ecs_get(&ed->world, e, COMP_TRANSFORM); }

// the selection copied (the plugins change it as they go)
static uint32_t sel_copy(Editor* ed, Entity* out, uint32_t cap) {
    uint32_t n = ed->nsel < cap ? ed->nsel : cap;
    memcpy(out, ed->sel, sizeof(Entity) * n);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Snap to Ground: the selected actors dropped on the surface below them (optionally turned to it)
static float g_snap_offset;
static bool g_snap_align;

static void align_to_normal(float rot[3], const float n[3]) {
    float ax[3][3];
    rotator_axes(rot, ax);
    float f[3] = { ax[0][0], ax[0][1], ax[0][2] };
    float d = f[0] * n[0] + f[1] * n[1] + f[2] * n[2];
    f[0] -= n[0] * d, f[1] -= n[1] * d, f[2] -= n[2] * d;
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (l < 1e-4f) return;
    f[0] /= l, f[1] /= l, f[2] /= l;
    float r[3] = { n[1] * f[2] - n[2] * f[1], n[2] * f[0] - n[0] * f[2], n[0] * f[1] - n[1] * f[0] }; // up x forward: right
    float out[3][3] = { { f[0], f[1], f[2] }, { r[0], r[1], r[2] }, { n[0], n[1], n[2] } };
    axes_rotator(out, rot);
}

static uint32_t snap_ground(Editor* ed, float offset, bool align) {
    Entity sel[4096];
    uint32_t ns = sel_copy(ed, sel, 4096), n = 0;
    if (!ns) return 0;
    editor_undo_push(ed);
    for (uint32_t i = 0; i < ns; i++) {
        CTransform* t = xf(ed, sel[i]);
        float hit[3], nrm[3];
        if (!t || !kit_ground_below(ed, t->location, sel[i], hit, nrm)) continue;
        t = xf(ed, sel[i]);
        t->location[2] = hit[2] + offset;
        if (align) align_to_normal(t->rotation, nrm);
        ecs_touch(&ed->world, sel[i]);
        n++;
    }
    ed->dirty = true;
    return n;
}
static void snap_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("Drops each selected actor straight down onto the first surface under its pivot (floors, the Landscape, other meshes)."));
    ui_num_float(TR("Height Offset"), &g_snap_offset, 0.5f, -200.0f, 200.0f, 0, 0, "%.1f cm", 0);
    igCheckbox(TR("Align to Surface"), &g_snap_align);
    igBeginDisabled(!ed->nsel || ed->playing);
    if (igButton(TR("Snap Selected to Ground"), (ImVec2){ -1, 0 })) {
        uint32_t n = snap_ground(ed, g_snap_offset, g_snap_align);
        editor_status(ed, now, TR("%u of %u actors snapped to the ground."), n, ed->nsel);
    }
    igEndDisabled();
}
static bool snap_test(Editor* ed, char* msg, size_t cap) {
    Entity floor = kit_spawn_test_cube(ed, "SnapFloor", 5000, 5000, -50);
    CTransform* ft = xf(ed, floor);
    if (!ft) return false;
    ft->scale[0] = ft->scale[1] = 20.0f, ft->scale[2] = 1.0f, ft->rotation[1] = 0;
    ecs_touch(&ed->world, floor);
    Entity e = kit_spawn_test_cube(ed, "SnapBox", 5100, 5050, 700);
    editor_select(ed, e, SEL_REPLACE);
    uint32_t n = snap_ground(ed, 0.0f, true);
    CTransform* t = xf(ed, e);
    float z = t ? t->location[2] : -1.0f;
    float ax[3][3];
    rotator_axes(t->rotation, ax);
    snprintf(msg, cap, "a box at 700 cm dropped to %.1f cm (the floor's top at 0), up (%.2f %.2f %.2f)", z, ax[2][0], ax[2][1], ax[2][2]);
    kit_destroy(ed, e), kit_destroy(ed, floor);
    return n == 1 && fabsf(z) < 2.0f && ax[2][2] > 0.99f;
}

const KitPlugin kit_snap_ground = { "SnapToGround", "Snap to Ground", "Level Design", "FLOOR",
    "Drops the selected actors onto the surface below them (floor, Landscape, meshes), with a height offset and optionally turned to the slope. Undoable.",
    true, snap_window, NULL, NULL, NULL, snap_test };

// ---------------------------------------------------------------------------------------------------------------
// Align and Distribute: the selection's pivots lined up on an axis, or spread evenly between the two at the ends
enum { AL_MIN, AL_CENTER, AL_MAX, AL_DISTRIBUTE };
static int g_al_axis;

static int cmp_axis_val(const void* a, const void* b) {
    float x = ((const float*)a)[0], y = ((const float*)b)[0];
    return x < y ? -1 : x > y;
}
static uint32_t align_apply(Editor* ed, int axis, int op) {
    Entity sel[4096];
    uint32_t ns = sel_copy(ed, sel, 4096);
    if (ns < 2) return 0;
    float mn = 1e30f, mx = -1e30f, sum = 0;
    for (uint32_t i = 0; i < ns; i++) {
        CTransform* t = xf(ed, sel[i]);
        if (!t) continue;
        float v = t->location[axis];
        mn = v < mn ? v : mn, mx = v > mx ? v : mx, sum += v;
    }
    editor_undo_push(ed);
    if (op == AL_DISTRIBUTE) { // (by their order along the axis: the ends stay, the rest evenly between)
        typedef struct { float v; Entity e; } AxisVal;
        AxisVal* order = kit_alloc(sizeof(AxisVal) * ns);
        if (!order) return 0;
        uint32_t k = 0;
        for (uint32_t i = 0; i < ns; i++)
            if (xf(ed, sel[i])) order[k].v = xf(ed, sel[i])->location[axis], order[k++].e = sel[i];
        qsort(order, k, sizeof(AxisVal), cmp_axis_val);
        for (uint32_t i = 0; i < k; i++) {
            xf(ed, order[i].e)->location[axis] = k > 1 ? mn + (mx - mn) * (float)i / (float)(k - 1) : mn;
            ecs_touch(&ed->world, order[i].e);
        }
        kit_free(order, sizeof(AxisVal) * ns);
    } else {
        float to = op == AL_MIN ? mn : op == AL_MAX ? mx : sum / (float)ns;
        for (uint32_t i = 0; i < ns; i++)
            if (xf(ed, sel[i])) xf(ed, sel[i])->location[axis] = to, ecs_touch(&ed->world, sel[i]);
    }
    ed->dirty = true;
    return ns;
}
static void align_window(Editor* ed, double now) {
    (void)now;
    igTextWrapped("%s", TR("Lines up the selected actors' pivots on one axis, or spreads them evenly between the two at the ends."));
    igRadioButton_IntPtr("X", &g_al_axis, 0), igSameLine(0, -1), igRadioButton_IntPtr("Y", &g_al_axis, 1), igSameLine(0, -1),
        igRadioButton_IntPtr("Z", &g_al_axis, 2);
    igBeginDisabled(ed->nsel < 2 || ed->playing);
    float w = (igGetContentRegionAvail().x - 12.0f) / 3.0f;
    if (igButton(TR("Align Min"), (ImVec2){ w, 0 })) align_apply(ed, g_al_axis, AL_MIN);
    igSameLine(0, 6.0f);
    if (igButton(TR("Align Center"), (ImVec2){ w, 0 })) align_apply(ed, g_al_axis, AL_CENTER);
    igSameLine(0, 6.0f);
    if (igButton(TR("Align Max"), (ImVec2){ w, 0 })) align_apply(ed, g_al_axis, AL_MAX);
    if (igButton(TR("Distribute Evenly"), (ImVec2){ -1, 0 })) align_apply(ed, g_al_axis, AL_DISTRIBUTE);
    igEndDisabled();
    if (ed->nsel < 2) igTextDisabled("%s", TR("Select two actors or more."));
}
static bool align_test(Editor* ed, char* msg, size_t cap) {
    Entity e[3] = { kit_spawn_test_cube(ed, "AlA", 0, 0, 0), kit_spawn_test_cube(ed, "AlB", 1000, 30, 0), kit_spawn_test_cube(ed, "AlC", 300, 90, 0) };
    editor_select(ed, e[0], SEL_REPLACE), editor_select(ed, e[1], SEL_ADD), editor_select(ed, e[2], SEL_ADD);
    align_apply(ed, 0, AL_DISTRIBUTE);
    float c = xf(ed, e[2])->location[0];
    align_apply(ed, 1, AL_MAX);
    bool ok = fabsf(c - 500.0f) < 0.01f && xf(ed, e[0])->location[1] == 90.0f && xf(ed, e[1])->location[0] == 1000.0f;
    snprintf(msg, cap, "the middle one distributed to x %.0f, all aligned to y 90", c);
    for (int i = 0; i < 3; i++) kit_destroy(ed, e[i]);
    return ok;
}

const KitPlugin kit_align = { "AlignDistribute", "Align and Distribute", "Level Design", "GRID",
    "Lines up the selected actors on X, Y or Z (min, center, max) or spreads them evenly between the two at the ends: fences, lamps, columns. Undoable.",
    true, align_window, NULL, NULL, NULL, align_test };

// ---------------------------------------------------------------------------------------------------------------
// Array: the selected actor copied in a grid (counts and spacing per axis)
static int g_arr_n[3] = { 4, 1, 1 };
static float g_arr_step[3] = { 200.0f, 200.0f, 200.0f };

static uint32_t array_apply(Editor* ed, Entity src, const int n[3], const float step[3]) {
    CTransform* st = xf(ed, src);
    if (!st) return 0;
    float base[3];
    memcpy(base, st->location, sizeof base);
    editor_undo_push(ed);
    uint32_t made = 0;
    for (int z = 0; z < n[2]; z++)
        for (int y = 0; y < n[1]; y++)
            for (int x = 0; x < n[0]; x++) {
                if (!x && !y && !z) continue; // (the source is the first)
                Entity c = editor_duplicate_actor(ed, src);
                CTransform* t = c ? xf(ed, c) : NULL;
                if (!t) continue;
                t->location[0] = base[0] + step[0] * (float)x, t->location[1] = base[1] + step[1] * (float)y, t->location[2] = base[2] + step[2] * (float)z;
                ecs_touch(&ed->world, c);
                made++;
            }
    ed->dirty = true;
    return made;
}
static void array_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("Copies the selected actor in a grid: how many along each axis and how far apart (shelves, crates, pillars, seats)."));
    ui_num_int(TR("Count X"), &g_arr_n[0], 0.1f, 1, 50, 1, 1000, "%d", 0);
    ui_num_int(TR("Count Y"), &g_arr_n[1], 0.1f, 1, 50, 1, 1000, "%d", 0);
    ui_num_int(TR("Count Z"), &g_arr_n[2], 0.1f, 1, 20, 1, 1000, "%d", 0);
    ui_num_float3(TR("Spacing"), g_arr_step, 1.0f, -2000.0f, 2000.0f, 0, 0, "%.0f cm", 0);
    long long total = (long long)g_arr_n[0] * g_arr_n[1] * g_arr_n[2] - 1;
    igText(TR("%lld copies"), total);
    igBeginDisabled(ed->selected == ENTITY_NONE || ed->playing || total < 1 || total > 100000);
    if (igButton(TR("Make Array"), (ImVec2){ -1, 0 })) editor_status(ed, now, TR("%u copies made."), array_apply(ed, ed->selected, g_arr_n, g_arr_step));
    igEndDisabled();
}
static bool array_test(Editor* ed, char* msg, size_t cap) {
    uint32_t before = ed->world.alive;
    Entity src = kit_spawn_test_cube(ed, "ArrSrc", 0, 0, 0);
    uint32_t made = array_apply(ed, src, (int[3]){ 3, 2, 1 }, (float[3]){ 150, 300, 0 });
    // the farthest copy at (300, 300, 0)
    bool far = false;
    World* w = &ed->world;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CTransform* t = e && e != src && !strncmp(kit_actor_name(w, e), "ArrSrc", 6) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t && t->location[0] == 300.0f && t->location[1] == 300.0f) far = true;
        if (t) kit_destroy(ed, e);
    }
    kit_destroy(ed, src);
    snprintf(msg, cap, "3 x 2 grid: %u copies, the last at (300, 300); %u actors after cleanup (%u before)", made, ed->world.alive, before);
    return made == 5 && far && ed->world.alive == before;
}

const KitPlugin kit_array = { "ArrayTool", "Array", "Level Design", "DUPLICATE",
    "Copies the selected actor in a grid of rows, columns and layers with the spacing you give: shelves, crates, pillars, seats. Undoable.",
    false, array_window, NULL, NULL, NULL, array_test };

// ---------------------------------------------------------------------------------------------------------------
// Scatter: copies of the selected actors spread at random around the first one, on the ground, never too close
static int g_sc_count = 20;
static float g_sc_radius = 1500.0f, g_sc_min_gap = 120.0f, g_sc_scale[2] = { 0.8f, 1.25f };
static bool g_sc_yaw = true, g_sc_ground = true;
static int g_sc_seed = 1;

static uint32_t scatter_apply(Editor* ed, int count, float radius, float gap, const float scale[2], bool yaw, bool ground, uint32_t seed) {
    Entity src[256];
    uint32_t ns = sel_copy(ed, src, 256);
    CTransform* c0 = ns ? xf(ed, src[0]) : NULL;
    if (!c0 || count < 1) return 0;
    float center[3];
    memcpy(center, c0->location, sizeof center);
    float(*placed)[2] = kit_alloc(sizeof(float[2]) * (size_t)count);
    if (!placed) return 0;
    editor_undo_push(ed);
    uint32_t made = 0, s = seed * 2654435761u + 1u;
    for (int i = 0; i < count; i++) {
        float p[2];
        bool fits = false;
        for (int tries = 0; tries < 30 && !fits; tries++) { // (a point in the disk not too close to the ones before)
            float a = rng01(&s) * 6.2831853f, r = sqrtf(rng01(&s)) * radius;
            p[0] = center[0] + cosf(a) * r, p[1] = center[1] + sinf(a) * r;
            fits = true;
            for (uint32_t k = 0; k < made && fits; k++) fits = (p[0] - placed[k][0]) * (p[0] - placed[k][0]) + (p[1] - placed[k][1]) * (p[1] - placed[k][1]) >= gap * gap;
        }
        if (!fits) continue;
        Entity from = src[rng_next(&s) % ns];
        Entity e = editor_duplicate_actor(ed, from);
        CTransform* t = e ? xf(ed, e) : NULL;
        if (!t) continue;
        t->location[0] = p[0], t->location[1] = p[1];
        if (yaw) t->rotation[1] = rng01(&s) * 360.0f;
        float k = scale[0] + (scale[1] - scale[0]) * rng01(&s);
        t->scale[0] *= k, t->scale[1] *= k, t->scale[2] *= k;
        if (ground) {
            float hit[3], nrm[3], from_p[3] = { p[0], p[1], center[2] + radius };
            if (kit_ground_below(ed, from_p, e, hit, nrm)) xf(ed, e)->location[2] = hit[2];
        }
        ecs_touch(&ed->world, e);
        placed[made][0] = p[0], placed[made][1] = p[1];
        made++;
    }
    kit_free(placed, sizeof(float[2]) * (size_t)count);
    ed->dirty = true;
    return made;
}
static void scatter_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("Spreads copies of the selected actors (rocks, trees, debris) at random around the first one, on the ground, never closer than the gap."));
    ui_num_int(TR("Count"), &g_sc_count, 0.2f, 1, 500, 1, 20000, "%d", 0);
    ui_num_float(TR("Radius"), &g_sc_radius, 5.0f, 50.0f, 20000.0f, 1.0f, 1e7f, "%.0f cm", 0);
    ui_num_float(TR("Minimum Gap"), &g_sc_min_gap, 1.0f, 0.0f, 2000.0f, 0.0f, 1e6f, "%.0f cm", 0);
    ui_num_float_range(TR("Scale Range"), &g_sc_scale[0], &g_sc_scale[1], 0.01f, 0.1f, 4.0f, 0.01f, 100.0f, "%.2f", "%.2f", 0);
    igCheckbox(TR("Random Yaw"), &g_sc_yaw);
    igSameLine(0, -1);
    igCheckbox(TR("On the Ground"), &g_sc_ground);
    ui_num_int(TR("Seed"), &g_sc_seed, 0.2f, 0, 9999, 0, 0, "%d", 0);
    igBeginDisabled(!ed->nsel || ed->playing);
    if (igButton(TR("Scatter"), (ImVec2){ -1, 0 })) {
        uint32_t n = scatter_apply(ed, g_sc_count, g_sc_radius, g_sc_min_gap, g_sc_scale, g_sc_yaw, g_sc_ground, (uint32_t)g_sc_seed);
        editor_status(ed, now, TR("%u copies scattered."), n);
        g_sc_seed++;
    }
    igEndDisabled();
}
static void scatter_overlay(Editor* ed, struct ImDrawList* dl) { // the disk around the first selected actor while the window is used
    (void)dl;
    CTransform* t = ed->selected ? xf(ed, ed->selected) : NULL;
    if (!t || ed->playing || !kit_window_open("ScatterTool")) return;
    float prev[3] = { 0 };
    for (int i = 0; i <= 48; i++) {
        float a = (float)i / 48.0f * 6.2831853f, p[3] = { t->location[0] + cosf(a) * g_sc_radius, t->location[1] + sinf(a) * g_sc_radius, t->location[2] };
        if (i) dd_line(prev, p, dd_rgba(0.3f, 0.9f, 0.4f, 0.8f), 2.0f, 0.0f);
        memcpy(prev, p, sizeof prev);
    }
}
static bool scatter_test(Editor* ed, char* msg, size_t cap) {
    uint32_t before = ed->world.alive;
    Entity src = kit_spawn_test_cube(ed, "ScatterRock", -8000, -8000, 0);
    editor_select(ed, src, SEL_REPLACE);
    uint32_t made = scatter_apply(ed, 30, 1000.0f, 100.0f, (float[2]){ 1, 1 }, true, false, 7);
    // every copy inside the disk and no two closer than the gap
    World* w = &ed->world;
    float pts[64][2];
    uint32_t np = 0;
    bool inside = true;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CTransform* t = e && e != src && !strncmp(kit_actor_name(w, e), "ScatterRock", 11) ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t) continue;
        float dx = t->location[0] + 8000.0f, dy = t->location[1] + 8000.0f;
        inside &= dx * dx + dy * dy <= 1000.0f * 1000.0f + 1.0f;
        if (np < 64) pts[np][0] = t->location[0], pts[np++][1] = t->location[1];
        kit_destroy(ed, e);
    }
    float minsq = 1e30f;
    for (uint32_t i = 0; i < np; i++)
        for (uint32_t j = i + 1; j < np; j++) {
            float d = (pts[i][0] - pts[j][0]) * (pts[i][0] - pts[j][0]) + (pts[i][1] - pts[j][1]) * (pts[i][1] - pts[j][1]);
            minsq = d < minsq ? d : minsq;
        }
    kit_destroy(ed, src);
    snprintf(msg, cap, "%u copies in a 10 m disk, the closest two %.0f cm apart", made, sqrtf(minsq));
    return made >= 25 && inside && minsq >= 100.0f * 100.0f - 1.0f && ed->world.alive == before;
}

const KitPlugin kit_scatter = { "ScatterTool", "Scatter", "Level Design", "FOLIAGE",
    "Spreads copies of the selected actors at random around the first one: a count, a radius, a minimum gap, random yaw and scale, dropped on the ground. Undoable.",
    false, scatter_window, scatter_overlay, NULL, NULL, scatter_test };

// ---------------------------------------------------------------------------------------------------------------
// Measure: the distance between the two last selected actors (a line and its length in the viewport), the selection's span
static bool measure_two(Editor* ed, float a[3], float b[3]) {
    if (ed->nsel < 2) return false;
    CTransform* ta = xf(ed, ed->sel[ed->nsel - 2]);
    CTransform* tb = xf(ed, ed->sel[ed->nsel - 1]);
    if (!ta || !tb) return false;
    memcpy(a, ta->location, sizeof(float) * 3), memcpy(b, tb->location, sizeof(float) * 3);
    return true;
}
static void fmt_len(float cm, char* out, size_t cap) {
    if (fabsf(cm) >= 100.0f) snprintf(out, cap, "%.2f m", cm / 100.0f);
    else snprintf(out, cap, "%.1f cm", cm);
}
static void measure_window(Editor* ed, double now) {
    (void)now;
    float a[3], b[3];
    if (measure_two(ed, a, b)) {
        float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        char s[32], sx[32], sy[32], sz[32], sh[32];
        fmt_len(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), s, sizeof s);
        fmt_len(d[0], sx, sizeof sx), fmt_len(d[1], sy, sizeof sy), fmt_len(d[2], sz, sizeof sz);
        fmt_len(sqrtf(d[0] * d[0] + d[1] * d[1]), sh, sizeof sh);
        igText("%s: %s", TR("Distance"), s);
        igText("X %s   Y %s   Z %s", sx, sy, sz);
        igText("%s: %s", TR("Horizontal"), sh);
        if (igButton(TR("Copy Distance"), (ImVec2){ 0, 0 })) igSetClipboardText(s);
    } else igTextDisabled("%s", TR("Select two actors to measure between them (the line shows in the viewport)."));
    if (ed->nsel) {
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t i = 0; i < ed->nsel; i++) {
            CTransform* t = xf(ed, ed->sel[i]);
            for (int k = 0; t && k < 3; k++) mn[k] = fminf(mn[k], t->location[k]), mx[k] = fmaxf(mx[k], t->location[k]);
        }
        char sx[32], sy[32], sz[32];
        fmt_len(mx[0] - mn[0], sx, sizeof sx), fmt_len(mx[1] - mn[1], sy, sizeof sy), fmt_len(mx[2] - mn[2], sz, sizeof sz);
        igSeparatorText(TR("Selection Span"));
        igText(TR("%u actors over X %s, Y %s, Z %s"), ed->nsel, sx, sy, sz);
    }
}
static void measure_overlay(Editor* ed, struct ImDrawList* dl) {
    float a[3], b[3], sa[2], sb[2];
    if (!measure_two(ed, a, b) || !kit_world_to_screen(ed, a, sa) || !kit_world_to_screen(ed, b, sb)) return;
    ImDrawList_AddLine(dl, (ImVec2){ sa[0], sa[1] }, (ImVec2){ sb[0], sb[1] }, TH(TK_AMBER), 2.0f);
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, mid[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f };
    char s[32];
    fmt_len(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), s, sizeof s);
    kit_label(dl, ed, mid, TH(TK_AMBER), s);
}
static bool measure_test(Editor* ed, char* msg, size_t cap) {
    Entity a = kit_spawn_test_cube(ed, "MeasA", 0, 0, 0), b = kit_spawn_test_cube(ed, "MeasB", 300, 400, 0);
    editor_select(ed, a, SEL_REPLACE), editor_select(ed, b, SEL_ADD);
    float p[3], q[3];
    bool ok = measure_two(ed, p, q);
    float d = ok ? sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1])) : 0;
    char s[32];
    fmt_len(d, s, sizeof s);
    snprintf(msg, cap, "(0,0) to (300,400): %s", s);
    kit_destroy(ed, a), kit_destroy(ed, b);
    return ok && fabsf(d - 500.0f) < 0.01f && !strcmp(s, "5.00 m");
}

const KitPlugin kit_measure = { "MeasureTool", "Measure", "Level Design", "MEASURE",
    "The distance between the two last selected actors (a line and its length in the viewport, X/Y/Z apart, horizontal) and the span of the whole selection.",
    false, measure_window, measure_overlay, NULL, NULL, measure_test };
