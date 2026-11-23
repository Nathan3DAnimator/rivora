// The Q3 tests of the levels (2026-11-17, notas-dev/niveis.md), run by --levels-test with REALENGINE_LEVELS_TEST:
//   sum: the user's case. A persistent level with no light; Lighting Levels Day and Night, each made current and given a
//        sky preset (Clear Day, Night): each gets its own sun, sky light, fog and clouds (Night's preset never changes
//        Day's sun). Build Lighting bakes each one with only what lights it; Day on and Night off, then the other way,
//        then both at half: the lights, the fog and the clouds follow the weights (the sum), the bakes as GPU layers. Saved
//        and opened again: every Static mesh finds its lightmap (no black level), the levels on and off as they were.
//   remove: three sublevels; the Outliner by level; one dragged to another place; one removed (its actors out, the
//        others' marks renumbered, its file kept); Ctrl+Z brings it back, Ctrl+Y removes it again; saved and opened:
//        the order kept.
//   panel: (2026-11-19) the Levels panel itself, open in the editor: three sublevels (two Lighting Levels, one of
//        props); its controls clicked by mouse events injected into ImGui where the cards drew them last frame and keys
//        pressed the same way - the eye, the lock, At start, double-click (current), the kind icon (Lighting / Data),
//        a click (the Outliner by level), Del (the confirmation), Enter (removed), Ctrl+Z (back), the trash and Esc
//        (kept). A screenshot after each step when REALENGINE_LEVELS_SHOTS names a folder.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "gi.h"
#include "gi_bake.h"
#include "level.h"
#include "lights.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "sublevels.h"
#include "weather.h"
#include "vkctx.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

static bool g_ok = true, g_baking;
static char g_level[1100];
static int g_mode; // 1 sum, 2 remove
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            printf("[RealEngine] levels-test FAIL: " __VA_ARGS__);                                                      \
            printf("\n");                                                                                               \
            g_ok = false;                                                                                               \
        }                                                                                                               \
    } while (0)

int levels_q3_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char* e = getenv("REALENGINE_LEVELS_TEST");
        m = !e ? 0 : !strcmp(e, "sum") ? 1 : !strcmp(e, "remove") ? 2 : !strcmp(e, "panel") ? 3 : 0;
    }
    return m;
}

static int slot_named(const char* name) {
    SublevelSet* s = levels_set();
    for (uint32_t i = 0; i < s->n; i++)
        if (!strcmp(s->s[i].name, name)) return (int)i;
    return -1;
}
static size_t count_in(Editor* ed, int slot) {
    SublevelSet* s = levels_set();
    return slot >= 0 && slot < (int)s->n ? sublevels_count_actors(&ed->world, &s->s[slot]) : 0;
}
static Entity find_named(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ents[i];
    }
    return ENTITY_NONE;
}
static void name_it(World* w, Entity e, const char* name) {
    if (!ecs_get(w, e, COMP_NAME)) ecs_add(w, e, COMP_NAME);
    CName* n = ecs_get(w, e, COMP_NAME);
    if (n) snprintf(n->text, sizeof n->text, "%s", name);
}
static void sibling(const char* name, const char* ext, char* out, size_t cap) {
    snprintf(out, cap, "%s", g_level);
    char* sl = strrchr(out, '\\');
    char* sl2 = strrchr(out, '/');
    if (sl2 > sl) sl = sl2;
    if (sl) sl[1] = 0;
    strncat(out, name, cap - strlen(out) - 1);
    strncat(out, ext, cap - strlen(out) - 1);
}
static Entity static_cube(Editor* ed, const char* name, float x, float y, double now) {
    Entity c = editor_spawn_shape(ed, SHAPE_CUBE, (float[]){ x, y, 100.0f }, now);
    name_it(&ed->world, c, name);
    CTransform* t = ecs_get(&ed->world, c, COMP_TRANSFORM);
    if (t) t->scale[0] = t->scale[1] = t->scale[2] = 1.5f, t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_STATIC;
    levels_assign_now(ed);
    return c;
}
// what in a level lights: suns, sky lights, fogs, clouds (slot -1: the persistent level)
typedef struct Lit { int sun, sky, fog, cloud; Entity sun_e, fog_e; } Lit;
static Lit lit_in(Editor* ed, int slot) {
    Lit r = { 0 };
    World* w = &ed->world;
    for (uint32_t i = 1; i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        if (e == ENTITY_NONE || sublevel_of(w, levels_set(), e) != slot) continue;
        const CLight* l = ecs_get(w, e, COMP_LIGHT);
        if (l && l->type == LIGHT_DIRECTIONAL) r.sun++, r.sun_e = e;
        if (l && l->type == LIGHT_SKY) r.sky++;
        if (ecs_get(w, e, COMP_FOG)) r.fog++, r.fog_e = e;
        if (ecs_get(w, e, COMP_CLOUD)) r.cloud++;
    }
    return r;
}
static float sun_lux(Editor* ed, Entity e) {
    const CLight* l = e ? ecs_get(&ed->world, e, COMP_LIGHT) : NULL;
    return l ? l->intensity : -1.0f;
}
static float fog_density(Editor* ed, Entity e) {
    const CFog* f = e ? ecs_get(&ed->world, e, COMP_FOG) : NULL;
    return f ? f->density : -1.0f;
}
static GpuFrame g_fr; // (weather_fill_frame's output: the fog the frame would use)
static float frame_fog(Editor* ed) {
    memset(&g_fr, 0, sizeof g_fr);
    weather_fill_frame(&g_fr, &ed->world, false);
    return g_fr.fog0[0] * 100.0f; // (1/cm -> 1/m)
}
static bool lit_gpu(Editor* ed, Entity e) {
    GpuLight g;
    return e && lights_gpu(&ed->world, e, true, &g);
}
static size_t statics_unlit(Editor* ed, size_t* total) {
    World* w = &ed->world;
    size_t miss = 0;
    *total = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if ((tr[i].flags & TRANSFORM_MOBILITY_MASK) != MOBILITY_STATIC) continue;
            uint32_t slot = render_mesh_slot(sm[i].mesh);
            if (!render_mesh_has_atlas(slot)) continue;
            float st[4];
            (*total)++;
            if (!gi_lightmap_for(ents[i], &tr[i], slot, st)) miss++;
        }
    }
    return miss;
}
static double rlight_energy(const char* path);

// --- sum ---
static Entity g_day_cube_before;

static bool sum_setup(Editor* ed, double now) {
    World* w = &ed->world;
    // the persistent level with no light: the template's sun, sky, fog, clouds and Post Process out
    for (int pass = 0; pass < 4; pass++)
        for (uint32_t i = 1; i < w->slot_count; i++) {
            Entity e = ecs_entity_at(w, i);
            if (e == ENTITY_NONE) continue;
            uint64_t m = ecs_mask(w, e);
            if (m & (COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_FOG) | COMP_BIT(COMP_CLOUD) | COMP_BIT(COMP_POST_PROCESS) | COMP_BIT(COMP_FOG_VOLUME)))
                ecs_destroy(w, e);
        }
    for (int i = 0; i < 3; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "BaseCube%d", i);
        static_cube(ed, nm, -300.0f + 300.0f * (float)i, 0.0f, now);
    }
    CHECK(editor_save_level(ed, g_level, now), "the persistent level not saved");
    Lit p = lit_in(ed, -1);
    CHECK(p.sun + p.sky + p.fog + p.cloud == 0, "the persistent level still has light (%d suns, %d skies)", p.sun, p.sky);

    // Day: made current, a sky preset
    CHECK(levels_create(ed, "LsTest_Day", now), "LsTest_Day not made");
    int day = slot_named("LsTest_Day");
    if (day >= 0) levels_set()->s[day].lighting = true;
    editor_sky_preset(ed, 0, now); // Clear Day
    static_cube(ed, "DayCube", 0.0f, 300.0f, now);
    Lit d = lit_in(ed, day);
    CHECK(d.sun == 1 && d.sky == 1 && d.fog == 1 && d.cloud == 1, "Day's sky preset: %d suns, %d skies, %d fogs, %d clouds (want 1 each)", d.sun, d.sky, d.fog, d.cloud);
    float day_lux = sun_lux(ed, d.sun_e), day_fog = fog_density(ed, d.fog_e);

    // Night: made current, the night preset: its own sun (the moon), Day's untouched
    CHECK(levels_create(ed, "LsTest_Night", now), "LsTest_Night not made");
    int night = slot_named("LsTest_Night");
    if (night >= 0) levels_set()->s[night].lighting = true;
    editor_sky_preset(ed, SKY_PRESET_COUNT - 1, now); // Night (Moonlight)
    Lit n = lit_in(ed, night);
    d = lit_in(ed, day);
    CHECK(n.sun == 1 && n.sky == 1 && n.fog == 1 && n.cloud == 1, "Night's sky preset: %d suns, %d skies, %d fogs, %d clouds (want 1 each)", n.sun, n.sky, n.fog, n.cloud);
    CHECK(sun_lux(ed, d.sun_e) == day_lux && fog_density(ed, d.fog_e) == day_fog, "the Night preset changed Day's sun or fog (%.1f lux, was %.1f)", sun_lux(ed, d.sun_e), day_lux);
    CHECK(sun_lux(ed, n.sun_e) < 1.0f, "Night's moon at %.2f lux", sun_lux(ed, n.sun_e));
    levels_set_current(ed, -1);
    g_day_cube_before = find_named(w, "DayCube");

    // Day on, Night off (as the user had it)
    if (night >= 0) sublevel_set_visible(w, &levels_set()->s[night], false, false);
    CHECK(editor_save_level(ed, g_level, now), "save with Day and Night failed");
    if (gi_available()) {
        g_baking = levels_build_lighting(ed, BAKE_PREVIEW, now) && levels_baking();
        CHECK(g_baking, "the Lighting Levels' bake did not start");
    } else
        printf("[RealEngine] levels-test: no ray tracing: the bake part skipped\n");
    return g_ok;
}

static bool sum_check(Editor* ed) {
    World* w = &ed->world;
    double now = re_time_seconds();
    int day = slot_named("LsTest_Day"), night = slot_named("LsTest_Night");
    CHECK(day >= 0 && night >= 0, "the levels gone");
    if (day < 0 || night < 0) return false;
    Lit d = lit_in(ed, day), n = lit_in(ed, night);
    float dfog = fog_density(ed, d.fog_e), nfog = fog_density(ed, n.fog_e);
    if (g_baking) {
        CHECK(!levels_baking(), "the bake still going at the end (raise --bench)");
        char base[1200], dl[1200], nl[1200], p[1200];
        level_rlight_path(g_level, base, sizeof base);
        sibling("LsTest_Day", ".rlevel", p, sizeof p), level_rlight_path(p, dl, sizeof dl);
        sibling("LsTest_Night", ".rlevel", p, sizeof p), level_rlight_path(p, nl, sizeof nl);
        double eb = rlight_energy(base), edy = rlight_energy(dl), en = rlight_energy(nl);
        printf("[RealEngine] levels-test: bakes (L0 summed): persistent %.1f, Day %.1f, Night %.1f\n", eb, edy, en);
        CHECK(eb >= 0.0 && edy > 0.0 && en > 0.0, "a bake is missing or empty (persistent %.1f, Day %.1f, Night %.1f)", eb, edy, en);
        CHECK(edy > en * 10.0, "Day's bake (%.1f) isn't far brighter than Night's (%.1f): the other level's sun got in", edy, en);
        CHECK(eb < en, "the persistent level (no light) baked %.1f, more than the night %.1f", eb, en);
        CHECK(gi_lightmaps_layers() == 3, "%d GPU layers, want 3 (persistent, Day, Night)", gi_lightmaps_layers());
    }
    // Day on, Night off: the moon, Night's fog and clouds out
    sublevels_set_active(levels_set(), g_level);
    sublevels_set_lighting_visible(ed->vk, w, "LsTest_Day", true, false);
    sublevels_set_lighting_visible(ed->vk, w, "LsTest_Night", false, false);
    CHECK(lit_gpu(ed, d.sun_e) && !lit_gpu(ed, n.sun_e), "Day on / Night off: Day's sun %d, the moon %d", lit_gpu(ed, d.sun_e), lit_gpu(ed, n.sun_e));
    float f1 = frame_fog(ed);
    CHECK(fabsf(f1 - dfog) < 1e-5f, "Day on / Night off: the frame's fog %.5f, Day's %.5f", f1, dfog);
    // the other way round
    sublevels_set_lighting_visible(ed->vk, w, "LsTest_Day", false, false);
    sublevels_set_lighting_visible(ed->vk, w, "LsTest_Night", true, false);
    CHECK(!lit_gpu(ed, d.sun_e) && lit_gpu(ed, n.sun_e), "Day off / Night on: Day's sun %d, the moon %d", lit_gpu(ed, d.sun_e), lit_gpu(ed, n.sun_e));
    float f2 = frame_fog(ed);
    CHECK(fabsf(f2 - nfog) < 1e-5f, "Day off / Night on: the frame's fog %.5f, Night's %.5f", f2, nfog);
    // both, at half: the sum
    sublevels_set_lighting_blend(ed->vk, w, "LsTest_Day", 0.5f, false);
    sublevels_set_lighting_blend(ed->vk, w, "LsTest_Night", 0.5f, false);
    float f3 = frame_fog(ed);
    CHECK(fabsf(f3 - 0.5f * (dfog + nfog)) < 1e-5f, "Day and Night at 0.5: the frame's fog %.5f, want %.5f", f3, 0.5f * (dfog + nfog));
    CHECK(fabsf(lights_level_weight(w, d.sun_e) - 0.5f) < 1e-4f && fabsf(lights_level_weight(w, n.sun_e) - 0.5f) < 1e-4f, "the suns not at half");
    CHECK(g_fr.cloud0[0] > 0.0f, "no clouds with both at half");
    // as the user had it, saved, opened again
    sublevels_set_lighting_blend(ed->vk, w, "LsTest_Day", 1.0f, false);
    sublevels_set_lighting_visible(ed->vk, w, "LsTest_Night", false, false);
    CHECK(editor_save_level(ed, g_level, now) && editor_open_level(ed, g_level, now), "save/open failed");
    day = slot_named("LsTest_Day"), night = slot_named("LsTest_Night");
    CHECK(day >= 0 && night >= 0 && levels_set()->s[day].visible && !levels_set()->s[night].visible, "after opening: Day on %d, Night on %d",
          day >= 0 && levels_set()->s[day].visible, night >= 0 && levels_set()->s[night].visible);
    Entity dc = find_named(w, "DayCube");
    printf("[RealEngine] levels-test: Day's cube was entity %u, after opening %u\n", (unsigned)g_day_cube_before, (unsigned)dc);
    if (g_baking) {
        size_t total = 0, miss = statics_unlit(ed, &total);
        CHECK(total >= 4 && miss == 0, "after opening: %zu of %zu Static meshes without their lightmap (black)", miss, total);
        CHECK(gi_lightmaps_layers() == 3, "after opening: %d GPU layers, want 3", gi_lightmaps_layers());
        printf("[RealEngine] levels-test: after opening, %zu Static meshes, every one with its lightmap\n", total);
    }
    d = lit_in(ed, day), n = lit_in(ed, night);
    CHECK(lit_gpu(ed, d.sun_e) && !lit_gpu(ed, n.sun_e), "after opening: Day's sun %d, the moon %d", lit_gpu(ed, d.sun_e), lit_gpu(ed, n.sun_e));
    float f4 = frame_fog(ed);
    CHECK(fabsf(f4 - dfog) < 1e-5f, "after opening: the frame's fog %.5f, Day's %.5f", f4, dfog);
    return g_ok;
}

// --- remove ---
static bool remove_run(Editor* ed, double now) {
    World* w = &ed->world;
    CHECK(editor_save_level(ed, g_level, now), "the persistent level not saved");
    const char* names[3] = { "LrTest_A", "LrTest_B", "LrTest_C" };
    for (int k = 0; k < 3; k++) {
        CHECK(levels_create(ed, names[k], now), "%s not made", names[k]);
        for (int i = 0; i <= k; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "%c_Cube%d", 'A' + k, i);
            static_cube(ed, nm, 200.0f * (float)i, 200.0f * (float)k, now);
        }
    }
    levels_set_current(ed, -1);
    int a = slot_named("LrTest_A"), b = slot_named("LrTest_B"), c = slot_named("LrTest_C");
    CHECK(count_in(ed, a) == 1 && count_in(ed, b) == 2 && count_in(ed, c) == 3, "the counts %zu %zu %zu", count_in(ed, a), count_in(ed, b), count_in(ed, c));
    // the Outliner by level
    Entity ea = find_named(w, "A_Cube0"), eb = find_named(w, "B_Cube1"), ec = find_named(w, "C_Cube2");
    levels_outliner_set_level(b);
    CHECK(levels_outliner_shows(ed, eb) && !levels_outliner_shows(ed, ea) && !levels_outliner_shows(ed, ec), "the Outliner by level B");
    levels_outliner_set_level(-1);
    CHECK(!levels_outliner_shows(ed, eb), "the Outliner by the persistent level shows B's actor");
    levels_outliner_set_level(-2);
    CHECK(levels_outliner_shows(ed, ea) && levels_outliner_shows(ed, eb) && levels_outliner_shows(ed, ec), "All Levels");
    // C dragged to the top: C, A, B
    levels_move_slot(ed, c, 0);
    a = slot_named("LrTest_A"), b = slot_named("LrTest_B"), c = slot_named("LrTest_C");
    CHECK(c == 0 && a == 1 && b == 2, "after the drag: C %d, A %d, B %d", c, a, b);
    CHECK(count_in(ed, a) == 1 && count_in(ed, b) == 2 && count_in(ed, c) == 3, "after the drag, the counts %zu %zu %zu", count_in(ed, a), count_in(ed, b), count_in(ed, c));
    CHECK(sublevel_of(w, levels_set(), ec) == c && sublevel_of(w, levels_set(), ea) == a, "after the drag, the actors' levels");
    // A removed
    double t0 = re_time_seconds();
    levels_remove(ed, a, now);
    double rm_ms = (re_time_seconds() - t0) * 1000.0;
    b = slot_named("LrTest_B"), c = slot_named("LrTest_C");
    CHECK(levels_set()->n == 2 && slot_named("LrTest_A") < 0, "A not removed (%u levels)", levels_set()->n);
    CHECK(find_named(w, "A_Cube0") == ENTITY_NONE, "A's actor still in the world");
    CHECK(count_in(ed, b) == 2 && count_in(ed, c) == 3, "after the remove, the counts B %zu C %zu", count_in(ed, b), count_in(ed, c));
    char pa[1200];
    sibling("LrTest_A", ".rlevel", pa, sizeof pa);
    CHECK(re_file_stat(pa, NULL, NULL, NULL), "A's file was deleted");
    // Ctrl+Z: back, in its place; Ctrl+Y: removed again; Ctrl+Z
    editor_undo(ed, now);
    a = slot_named("LrTest_A"), b = slot_named("LrTest_B"), c = slot_named("LrTest_C");
    ea = find_named(w, "A_Cube0");
    CHECK(levels_set()->n == 3 && a == 1 && ea && sublevel_of(w, levels_set(), ea) == a, "undo: A not back in its place (slot %d, %u levels)", a, levels_set()->n);
    CHECK(count_in(ed, a) == 1 && count_in(ed, b) == 2 && count_in(ed, c) == 3, "undo: the counts %zu %zu %zu", count_in(ed, a), count_in(ed, b), count_in(ed, c));
    editor_redo(ed, now);
    CHECK(levels_set()->n == 2 && slot_named("LrTest_A") < 0 && find_named(w, "A_Cube0") == ENTITY_NONE, "redo: A not removed again");
    editor_undo(ed, now);
    CHECK(levels_set()->n == 3 && slot_named("LrTest_A") == 1, "undo again");
    // saved, opened: C, A, B
    CHECK(editor_save_level(ed, g_level, now) && editor_open_level(ed, g_level, now), "save/open failed");
    a = slot_named("LrTest_A"), b = slot_named("LrTest_B"), c = slot_named("LrTest_C");
    CHECK(c == 0 && a == 1 && b == 2, "after opening, the order C %d, A %d, B %d", c, a, b);
    CHECK(count_in(ed, a) == 1 && count_in(ed, b) == 2 && count_in(ed, c) == 3, "after opening, the counts %zu %zu %zu", count_in(ed, a), count_in(ed, b), count_in(ed, c));
    // removed for good and saved: the list without it, its file kept
    levels_remove(ed, slot_named("LrTest_A"), now);
    CHECK(editor_save_level(ed, g_level, now) && editor_open_level(ed, g_level, now), "save/open after the remove failed");
    CHECK(levels_set()->n == 2 && slot_named("LrTest_A") < 0 && re_file_stat(pa, NULL, NULL, NULL), "after the remove and opening: %u levels", levels_set()->n);
    printf("[RealEngine] levels-test: a sublevel removed in %.3f ms\n", rm_ms);
    return g_ok;
}

// --- panel ---
extern void (*levels_test_hook)(Editor* ed, double now); // (editor_levels.c)
bool levels_test_rect(int slot, int what, float out[4]);
bool levels_test_confirm_open(void);
enum { LV_R_CARD, LV_R_EYE, LV_R_KIND, LV_R_BLEND, LV_R_START, LV_R_LOCK, LV_R_TRASH };
typedef struct PEv { uint64_t frame; int kind; float x, y; int key; bool down; } PEv; // kind 0 mouse place, 1 left button, 2 key
static struct {
    PEv ev[64];
    int nev, step, shots;
    uint64_t frame, step_at;
    bool done;
} PT;
static void ev_add(int kind, float x, float y, int key, bool down, int after) {
    if (PT.nev < 64) PT.ev[PT.nev++] = (PEv){ PT.frame + (uint64_t)after, kind, x, y, key, down };
}
// a click (or two: a double-click) in the middle of a control the panel drew last frame (frac: where across it)
static bool click_at(int slot, int what, int clicks, float frac_x, float frac_y) {
    float r[4];
    if (!levels_test_rect(slot, what, r)) {
        CHECK(false, "panel: control %d of slot %d not drawn", what, slot);
        return false;
    }
    float x = r[0] + (r[2] - r[0]) * frac_x, y = r[1] + (r[3] - r[1]) * frac_y;
    ev_add(0, x, y, 0, false, 1);
    for (int c = 0; c < clicks; c++) ev_add(1, 0, 0, 0, true, 2 + c * 2), ev_add(1, 0, 0, 0, false, 3 + c * 2);
    return true;
}
static void key_press(int key, bool ctrl) {
    if (ctrl) ev_add(2, 0, 0, ImGuiMod_Ctrl, true, 1);
    ev_add(2, 0, 0, key, true, 2), ev_add(2, 0, 0, key, false, 3);
    if (ctrl) ev_add(2, 0, 0, ImGuiMod_Ctrl, false, 4);
}
static void shot(Editor* ed, const char* name) {
    const char* dir = getenv("REALENGINE_LEVELS_SHOTS");
    if (!dir || !ed->vk) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%02d_%s.png", dir, ++PT.shots, name);
    vkctx_request_shot(ed->vk, path);
}
static Sublevel* lv(const char* name) {
    int i = slot_named(name);
    return i >= 0 ? &levels_set()->s[i] : NULL;
}
// each step: its action, then 40 frames later its check and screenshot
static void panel_hook(Editor* ed, double now) {
    ImGuiIO* io = igGetIO_Nil();
    PT.frame++;
    for (int i = 0; i < PT.nev; i++) { // the events due now (ImGui takes them at the next frame)
        PEv* e = &PT.ev[i];
        if (e->frame != PT.frame) continue;
        if (e->kind == 0) ImGuiIO_AddMousePosEvent(io, e->x, e->y);
        else if (e->kind == 1) ImGuiIO_AddMouseButtonEvent(io, 0, e->down);
        else ImGuiIO_AddKeyEvent(io, (ImGuiKey)e->key, e->down);
    }
    if (PT.done || PT.frame < 120 || PT.frame - PT.step_at < 40) return; // (the editor up and the panel drawn first)
    PT.step_at = PT.frame, PT.nev = 0;
    int night = slot_named("Lighting_Night"), day = slot_named("Lighting_Day"), props = slot_named("Props");
    SublevelSet* set = levels_set();
    switch (PT.step++) {
    case 0: // the panel in front (docked beside the Outliner: its tab)
        *levels_show_flag() = true;
        igSetWindowFocus_Str("###re_levels");
        break;
    case 1:
        shot(ed, "cards");
        click_at(night, LV_R_EYE, 1, 0.5f, 0.5f);
        break;
    case 2:
        CHECK(lv("Lighting_Night") && !lv("Lighting_Night")->visible, "panel: the eye didn't hide Lighting_Night");
        shot(ed, "eye_off");
        click_at(props, LV_R_LOCK, 1, 0.5f, 0.5f);
        break;
    case 3:
        CHECK(lv("Props") && lv("Props")->locked, "panel: the lock didn't lock Props");
        click_at(props, LV_R_START, 1, 0.12f, 0.5f);
        break;
    case 4:
        CHECK(lv("Props") && !lv("Props")->initially_loaded, "panel: At start didn't turn off");
        shot(ed, "lock_at_start");
        click_at(day, LV_R_CARD, 2, 0.62f, 0.25f); // (the card's empty space: the first line, left of the actor count)
        break;
    case 5:
        CHECK(set->current == day, "panel: the double-click didn't make Lighting_Day current (current %d)", set->current);
        shot(ed, "current");
        click_at(night, LV_R_KIND, 1, 0.5f, 0.5f);
        break;
    case 6:
        CHECK(lv("Lighting_Night") && !lv("Lighting_Night")->lighting, "panel: the kind icon didn't make it a Data level");
        shot(ed, "night_data");
        click_at(night, LV_R_KIND, 1, 0.5f, 0.5f);
        break;
    case 7:
        CHECK(lv("Lighting_Night") && lv("Lighting_Night")->lighting, "panel: the kind icon didn't make it a Lighting Level again");
        click_at(night, LV_R_EYE, 1, 0.5f, 0.5f);
        break;
    case 8:
        CHECK(lv("Lighting_Night") && lv("Lighting_Night")->visible, "panel: the eye didn't show it again");
        shot(ed, "two_lighting_on");
        { // the persistent level's cube moved into Props (the card's menu does the same: Move Selected Actors to Level)
            Entity c = find_named(&ed->world, "Persist_Cube");
            if (lv("Props")) lv("Props")->locked = false;
            if (c) editor_select(ed, c, SEL_REPLACE), levels_move_selected(ed, props, now);
            CHECK(c && sublevel_of(&ed->world, set, c) == props, "panel: the actor not moved into Props");
        }
        click_at(props, LV_R_CARD, 1, 0.62f, 0.25f);
        break;
    case 9:
        CHECK(levels_outliner_level() == props, "panel: a click on Props didn't show its actors in the Outliner (%d)", levels_outliner_level());
        shot(ed, "outliner_props");
        key_press(ImGuiKey_Delete, false);
        break;
    case 10:
        CHECK(levels_test_confirm_open(), "panel: Del didn't ask to remove Props");
        shot(ed, "confirm_remove");
        key_press(ImGuiKey_Enter, false);
        break;
    case 11:
        CHECK(props < 0 && set->n == 2 && !find_named(&ed->world, "Props_Cube0"), "panel: Enter didn't remove Props (%u levels)", set->n);
        shot(ed, "removed");
        key_press(ImGuiKey_Z, true);
        break;
    case 12:
        CHECK(props >= 0 && set->n == 3 && find_named(&ed->world, "Props_Cube0"), "panel: Ctrl+Z didn't bring Props back (%u levels)", set->n);
        shot(ed, "undo");
        click_at(night, LV_R_TRASH, 1, 0.5f, 0.5f);
        break;
    case 13:
        CHECK(levels_test_confirm_open(), "panel: the trash didn't ask");
        shot(ed, "trash_asks");
        key_press(ImGuiKey_Escape, false);
        break;
    case 14:
        CHECK(!levels_test_confirm_open() && night >= 0 && set->n == 3, "panel: Esc didn't keep Lighting_Night");
        shot(ed, "kept");
        PT.done = true;
        break;
    }
}
static bool panel_setup(Editor* ed, double now) {
    CHECK(editor_save_level(ed, g_level, now), "the persistent level not saved");
    static_cube(ed, "Persist_Cube", -300.0f, 0.0f, now);
    const char* names[3] = { "Lighting_Day", "Lighting_Night", "Props" };
    for (int k = 0; k < 3; k++) {
        CHECK(levels_create(ed, names[k], now), "%s not made", names[k]);
        for (int i = 0; i <= k; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "%s_Cube%d", k == 2 ? "Props" : k ? "Night" : "Day", i);
            static_cube(ed, nm, 200.0f * (float)i, 250.0f * (float)k, now);
        }
        if (k < 2) levels_set()->s[slot_named(names[k])].lighting = true;
    }
    levels_set_current(ed, -1);
    levels_outliner_set_level(-2);
    memset(&PT, 0, sizeof PT);
    levels_test_hook = panel_hook;
    return g_ok;
}

// the L0 image's summed luminance (the .rlight's first image)
static float hf(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 0.0f : ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}
static double rlight_energy(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return -1.0;
    uint32_t h[4];
    double sum = -1.0;
    if (fread(h, sizeof h, 1, f) == 1 && h[2] >= 16 && h[2] <= 8192) {
        fseek(f, (long)(16 + (size_t)h[3] * 32), SEEK_SET);
        size_t texels = (size_t)h[2] * h[2];
        uint16_t* px = malloc(texels * 8);
        if (px && fread(px, 8, texels, f) == texels) {
            sum = 0.0;
            for (size_t t = 0; t < texels; t++) sum += 0.2126 * hf(px[t * 4]) + 0.7152 * hf(px[t * 4 + 1]) + 0.0722 * hf(px[t * 4 + 2]);
        }
        free(px);
    }
    fclose(f);
    return sum;
}

bool levels_q3_setup(Editor* ed) {
    g_mode = levels_q3_mode();
    double now = re_time_seconds();
    char dir[900];
    snprintf(dir, sizeof dir, "%s\\Maps", content_root());
    re_make_dirs(dir);
    snprintf(g_level, sizeof g_level, "%s\\%s.rlevel", dir, g_mode == 1 ? "LsTest" : g_mode == 3 ? "LpTest" : "LrTest");
    bool ok = g_mode == 1 ? sum_setup(ed, now) : g_mode == 3 ? panel_setup(ed, now) : remove_run(ed, now);
    printf("[RealEngine] levels-test: %s setup %s\n", g_mode == 1 ? "lighting sum" : g_mode == 3 ? "panel" : "remove", ok ? "ok" : "FAILED");
    return ok;
}
bool levels_q3_check(Editor* ed) {
    if (g_mode == 1) sum_check(ed);
    if (g_mode == 3) {
        CHECK(PT.done, "panel: only %d of 15 steps ran", PT.step);
        levels_test_hook = NULL;
        if (PT.done) printf("[RealEngine] levels-test: panel: 14 steps by mouse and keys, %d screenshots\n", PT.shots);
    }
    printf("[RealEngine] levels-test: %s\n", g_ok ? "all passed" : "FAILED");
    return g_ok;
}
