// Sublevels and stackable Lighting Levels played (--levels-test, the test levels; notas-dev/niveis.md). In a new project:
//   * the template's level saved as LvTest; a sublevel LvTest_Art made (current): 3 cubes placed go into it
//   * a Lighting Level LvTest_Night made: a point light placed goes into it
//   * saved and opened again: the actors back in their levels, the files (.rlevel each, the .rlevels list) there, the
//     persistent file without the sublevels' actors, a sublevel's file without the marks
//   * the eye: a sublevel hidden and shown (an actor hidden by itself stays hidden), a locked level not selected
//   * an actor moved to the persistent level, saved, opened: it stays there
//   * Build Lighting (with ray tracing): the persistent level's bake and the Lighting Level's alone, each in its file; the
//     night's light only in its own; the sum loaded and timed; Set Lighting Level Visible off and on, timed
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
#include "sublevels.h"

static bool g_ok = true, g_baking;
static char g_level[1100];
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            printf("[RealEngine] levels-test FAIL: " __VA_ARGS__);                                                      \
            printf("\n");                                                                                               \
            g_ok = false;                                                                                               \
        }                                                                                                               \
    } while (0)

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
static size_t streamed_in_file(const char* path, size_t* actors) {
    static World w;
    size_t marked = 0;
    *actors = 0;
    if (!ecs_init(&w)) return 0;
    if (level_load(&w, path, NULL))
        for (uint32_t ai = 0; ai < w.archetype_count; ai++) {
            const EcsArchetype* a = &w.archetypes[ai];
            if (!a->count) continue;
            if (a->mask & COMP_BIT(COMP_STREAMED)) marked += a->count;
            if ((a->mask & COMP_BIT(COMP_TRANSFORM)) && !(a->mask & COMP_BIT(COMP_ATTACH))) *actors += a->count;
        }
    ecs_shutdown(&w);
    return marked;
}
static bool exists(const char* p) { return re_file_stat(p, NULL, NULL, NULL); }
static void sibling(const char* name, const char* ext, char* out, size_t cap) {
    snprintf(out, cap, "%s", g_level);
    char* sl = strrchr(out, '\\');
    char* sl2 = strrchr(out, '/');
    if (sl2 > sl) sl = sl2;
    if (sl) sl[1] = 0;
    strncat(out, name, cap - strlen(out) - 1);
    strncat(out, ext, cap - strlen(out) - 1);
}

int levels_q3_mode(void); // (2026-11-17, levels_q3_test.c: REALENGINE_LEVELS_TEST=sum or remove)
bool levels_q3_setup(Editor* ed);
bool levels_q3_check(Editor* ed);

bool levels_test_setup(Editor* ed) {
    if (levels_q3_mode()) return levels_q3_setup(ed);
    double now = re_time_seconds();
    char dir[900];
    snprintf(dir, sizeof dir, "%s\\Maps", content_root());
    re_make_dirs(dir);
    snprintf(g_level, sizeof g_level, "%s\\LvTest.rlevel", dir);
    CHECK(editor_save_level(ed, g_level, now), "the persistent level not saved");
    size_t persistent_actors = 0;
    streamed_in_file(g_level, &persistent_actors);

    // a sublevel, current: what is placed goes into it
    CHECK(levels_create(ed, "LvTest_Art", now), "LvTest_Art not made");
    int art = slot_named("LvTest_Art");
    CHECK(art >= 0 && levels_set()->current == art, "LvTest_Art not current");
    for (int i = 0; i < 3; i++) {
        Entity c = editor_spawn_shape(ed, SHAPE_CUBE, (float[]){ -250.0f + 250.0f * (float)i, 250.0f, 100.0f }, now);
        char nm[32];
        snprintf(nm, sizeof nm, "ArtCube%d", i);
        name_it(&ed->world, c, nm);
        CTransform* ct = ecs_get(&ed->world, c, COMP_TRANSFORM); // (Static: they bounce the night's lamp onto the floor)
        if (ct) ct->scale[0] = ct->scale[1] = ct->scale[2] = 2.0f, ct->flags = (ct->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_STATIC;
        levels_assign_now(ed);
    }
    CHECK(count_in(ed, art) == 3, "LvTest_Art has %zu actors, not 3", count_in(ed, art));

    // a Lighting Level with a point light
    CHECK(levels_create(ed, "LvTest_Night", now), "LvTest_Night not made");
    int night = slot_named("LvTest_Night");
    if (night >= 0) levels_set()->s[night].lighting = true;
    Entity l = ecs_create(&ed->world, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    if (l) {
        CTransform* t = ecs_get(&ed->world, l, COMP_TRANSFORM);
        t->location[0] = 0, t->location[1] = 0, t->location[2] = 150;
        t->scale[0] = t->scale[1] = t->scale[2] = 1;
        t->flags = MOBILITY_STATIC;
        CLight* li = ecs_get(&ed->world, l, COMP_LIGHT);
        li->type = LIGHT_POINT;
        li->color[0] = 0.4f, li->color[1] = 0.6f, li->color[2] = 1.0f;
        li->intensity = 200000.0f;
        li->attenuation_radius = 1500.0f;
        li->cast_shadows = 1;
        name_it(&ed->world, l, "NightLamp");
    }
    levels_assign_now(ed);
    CHECK(count_in(ed, night) == 1, "LvTest_Night has %zu actors, not 1", count_in(ed, night));
    levels_set_current(ed, -1);

    // saved, then opened again
    CHECK(editor_save_level(ed, g_level, now), "save with sublevels failed");
    char p_art[1200], p_night[1200], p_list[1200];
    sibling("LvTest_Art", ".rlevel", p_art, sizeof p_art);
    sibling("LvTest_Night", ".rlevel", p_night, sizeof p_night);
    sublevels_list_path(g_level, p_list, sizeof p_list);
    CHECK(exists(p_art) && exists(p_night) && exists(p_list), "the sublevels' files or the list missing");
    size_t a_p = 0, a_art = 0;
    CHECK(streamed_in_file(g_level, &a_p) == 0 && a_p == persistent_actors, "the persistent file has %zu actors (want %zu) or marks", a_p, persistent_actors);
    CHECK(streamed_in_file(p_art, &a_art) == 0 && a_art == 3, "LvTest_Art's file: %zu actors (want 3) or marks", a_art);
    uint32_t alive = ed->world.alive;
    CHECK(editor_open_level(ed, g_level, now), "open failed");
    art = slot_named("LvTest_Art"), night = slot_named("LvTest_Night");
    CHECK(art >= 0 && night >= 0 && levels_set()->s[night].lighting, "the list not read back");
    CHECK(count_in(ed, art) == 3 && count_in(ed, night) == 1, "after opening: %zu and %zu actors", count_in(ed, art), count_in(ed, night));
    CHECK(ed->world.alive == alive, "after opening: %u entities, before %u", ed->world.alive, alive);

    // the eye: hidden and shown; an actor hidden by itself stays hidden
    Entity c0 = find_named(&ed->world, "ArtCube0"), c1 = find_named(&ed->world, "ArtCube1");
    CTransform* t1 = c1 ? ecs_get(&ed->world, c1, COMP_TRANSFORM) : NULL;
    if (t1) t1->flags |= TRANSFORM_HIDDEN;
    if (art >= 0) sublevel_set_visible(&ed->world, &levels_set()->s[art], false, false);
    CTransform* t0 = c0 ? ecs_get(&ed->world, c0, COMP_TRANSFORM) : NULL;
    CHECK(t0 && (t0->flags & TRANSFORM_HIDDEN), "the hidden level's actor shown");
    if (art >= 0) sublevel_set_visible(&ed->world, &levels_set()->s[art], true, false);
    t0 = c0 ? ecs_get(&ed->world, c0, COMP_TRANSFORM) : NULL;
    t1 = c1 ? ecs_get(&ed->world, c1, COMP_TRANSFORM) : NULL;
    CHECK(t0 && !(t0->flags & (TRANSFORM_HIDDEN | TRANSFORM_LEVEL_HIDDEN)), "the shown level's actor still hidden");
    CHECK(t1 && (t1->flags & TRANSFORM_HIDDEN), "an actor hidden by itself was shown with its level");
    if (t1) t1->flags &= ~TRANSFORM_HIDDEN;

    // locked: not selected
    if (art >= 0) levels_set()->s[art].locked = true;
    editor_select_none(ed);
    editor_select(ed, c0, SEL_REPLACE);
    CHECK(!editor_is_selected(ed, c0), "a locked level's actor was selected");
    if (art >= 0) levels_set()->s[art].locked = false;

    // moved to the persistent level, saved, opened: it stays
    editor_select(ed, c0, SEL_REPLACE);
    levels_move_selected(ed, -1, now);
    CHECK(sublevel_of(&ed->world, levels_set(), c0) == -1 && count_in(ed, art) == 2, "the move to the persistent level");
    CHECK(editor_save_level(ed, g_level, now) && editor_open_level(ed, g_level, now), "save/open after the move");
    art = slot_named("LvTest_Art"), night = slot_named("LvTest_Night");
    c0 = find_named(&ed->world, "ArtCube0");
    CHECK(c0 && sublevel_of(&ed->world, levels_set(), c0) == -1 && count_in(ed, art) == 2, "the moved actor not in the persistent level after opening");
    streamed_in_file(g_level, &a_p);
    CHECK(a_p == persistent_actors + 1, "the persistent file: %zu actors, want %zu", a_p, persistent_actors + 1);

    // Build Lighting: one bake per group of lights
    if (gi_available()) {
        g_baking = levels_build_lighting(ed, BAKE_PREVIEW, now) && levels_baking();
        CHECK(g_baking, "the Lighting Levels' bake did not start");
    } else
        printf("[RealEngine] levels-test: no ray tracing: the bake part skipped\n");
    printf("[RealEngine] levels-test: setup %s\n", g_ok ? "ok" : "FAILED");
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

bool levels_test_check(Editor* ed) {
    if (levels_q3_mode()) return levels_q3_check(ed);
    if (g_baking) {
        CHECK(!levels_baking(), "the bake still going at the end (raise --bench)");
        char base[1200], nl[1200], p_night[1200];
        level_rlight_path(g_level, base, sizeof base);
        sibling("LvTest_Night", ".rlevel", p_night, sizeof p_night);
        level_rlight_path(p_night, nl, sizeof nl);
        double eb = rlight_energy(base), en = rlight_energy(nl);
        printf("[RealEngine] levels-test: base bake %.1f, night bake %.1f (L0 summed)\n", eb, en);
        CHECK(eb > 0.0 && en > 0.0, "a bake is empty or missing (base %.1f, night %.1f)", eb, en);
        CHECK(en < eb, "the night's bake has the sun or the sky in it (night %.1f >= base %.1f)", en, eb);
        // the sum shown, and a Lighting Level switched off and on as a Blueprint does
        double t0 = re_time_seconds();
        bool ok = sublevels_load_lighting(ed->vk, g_level, levels_set());
        double sum_ms = (re_time_seconds() - t0) * 1000.0;
        CHECK(ok, "the sum did not load");
        sublevels_set_active(levels_set(), g_level);
        t0 = re_time_seconds();
        bool off = sublevels_set_lighting_visible(ed->vk, &ed->world, "LvTest_Night", false, false);
        double off_ms = (re_time_seconds() - t0) * 1000.0;
        CHECK(off && !sublevels_lighting_visible("LvTest_Night"), "Set Lighting Level Visible off");
        Entity lamp = find_named(&ed->world, "NightLamp");
        const CTransform* lt = lamp ? ecs_get(&ed->world, lamp, COMP_TRANSFORM) : NULL;
        CHECK(lt && (lt->flags & TRANSFORM_HIDDEN), "the night's lamp still on");
        t0 = re_time_seconds();
        bool on = sublevels_set_lighting_visible(ed->vk, &ed->world, "LvTest_Night", true, false);
        double on_ms = (re_time_seconds() - t0) * 1000.0;
        CHECK(on && sublevels_lighting_visible("LvTest_Night"), "Set Lighting Level Visible on");
        printf("[RealEngine] levels-test: lighting loaded in %.2f ms (base + 1 level as GPU layers), a level switched off in %.3f ms, on in %.3f ms\n", sum_ms, off_ms, on_ms);
        // the blend: a weight on the GPU layers and on the level's lights, no file and no CPU sum
        CHECK(gi_lightmaps_layers() == 2, "%d GPU layers, want 2", gi_lightmaps_layers());
        t0 = re_time_seconds();
        bool bl = sublevels_set_lighting_blend(ed->vk, &ed->world, "LvTest_Night", 0.5f, false);
        double blend_ms = (re_time_seconds() - t0) * 1000.0;
        CHECK(bl && fabsf(sublevels_lighting_blend("LvTest_Night") - 0.5f) < 1e-4f, "Set Lighting Level Blend 0.5");
        CHECK(lamp && fabsf(lights_level_weight(&ed->world, lamp) - 0.5f) < 1e-4f, "the night's lamp not at half its intensity");
        CHECK(blend_ms < 0.5, "Set Lighting Level Blend took %.3f ms (want < 0.5: only weights)", blend_ms);
        sublevels_set_lighting_blend(ed->vk, &ed->world, "LvTest_Night", 0.0f, false);
        lt = lamp ? ecs_get(&ed->world, lamp, COMP_TRANSFORM) : NULL;
        CHECK(lt && (lt->flags & TRANSFORM_HIDDEN), "blend 0: the lamp still on");
        sublevels_set_lighting_blend(ed->vk, &ed->world, "LvTest_Night", 1.0f, false);
        printf("[RealEngine] levels-test: Set Lighting Level Blend in %.3f ms (the GPU blends the layers next frame)\n", blend_ms);
    }
    printf("[RealEngine] levels-test: %s\n", g_ok ? "all passed" : "FAILED");
    return g_ok;
}
