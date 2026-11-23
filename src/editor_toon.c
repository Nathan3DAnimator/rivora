// (Q11) --scene toon: the Toon shading model and the Auto Adaptive Outline (notas-dev/toon.md). A stage in toon bands
// (M_ToonGround, no outline), in front of the camera the examples (M_Toon, M_ToonRamp with four points and a rim,
// M_ToonHatch hatched in its shadows, M_ToonSkin soft with a red cast shadow, MI_ToonBlue: an instance's own toon), a
// realistic sphere with its actor's own red outline under a Force Toon spot, a warm fill light set to Never Toon, and a
// row of toon actors going away to 60 m (the outline's width by the distance). The sun casts shadows on all of it.
// REALENGINE_NO_OUTLINE / REALENGINE_NO_TOON: the measure's "before" (no outline pass / the materials Default Lit).
// The check (end of the measure): the materials are Toon, the outlines drawn (their share of the view in range), their
// GPU time under 0.5 ms at 1440p
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "material.h"
#include "outline.h"
#include "render.h"
#include "toon_actor.h"

static const char* const g_toon_mats[6] = { "Examples/Toon/M_Toon.rmat", "Examples/Toon/M_ToonRamp.rmat", "Examples/Toon/M_ToonHatch.rmat",
                                            "Examples/Toon/M_ToonSkin.rmat", "Examples/Toon/MI_ToonBlue.rmi", "Examples/Toon/M_ToonGround.rmat" };

static uint32_t mat_hash(const char* rel) {
    char path[700];
    snprintf(path, sizeof path, "%s/Materials/%s", content_root(), rel);
    Material* m = mat_load(path);
    return m ? m->hash : 0u;
}

static Entity shape(World* w, const char* name, uint32_t mesh, float x, float y, float z, float s, uint32_t mat) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = s, t->flags = MOBILITY_STATIC;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    ecs_component_defaults(COMP_STATIC_MESH, sm);
    sm->mesh = mesh, sm->material = mat, sm->cast_shadow = 1;
    sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.6f, sm->roughness = 0.5f;
    return e;
}

static Entity light(World* w, const char* name, uint32_t type, const float at[3], float pitch, float yaw, float intensity, const float color[3]) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->rotation[0] = pitch, t->rotation[1] = yaw;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    CLight* l = ecs_get(w, e, COMP_LIGHT);
    ecs_component_defaults(COMP_LIGHT, l);
    l->type = type, l->intensity = intensity, l->attenuation_radius = 2500.0f, l->cast_shadows = 1;
    l->inner_cone = 14.0f, l->outer_cone = 20.0f;
    memcpy(l->color, color, 12);
    return e;
}

void editor_toon_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    World* w = &ed->world;
    ed->selected = ENTITY_NONE;
    editor_sky_preset(ed, 0, 0.0); // (the sun and the sky)
    for (uint32_t i = 1; i < w->slot_count; i++) { // the sun: shadows, a little lower for longer bands
        Entity e = ecs_entity_at(w, i);
        CLight* l = e != ENTITY_NONE ? ecs_get(w, e, COMP_LIGHT) : NULL;
        CTransform* t = e != ENTITY_NONE ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (l && t && l->type == LIGHT_DIRECTIONAL) l->cast_shadows = 1, t->rotation[0] = -35.0f, t->rotation[1] = 120.0f;
    }
    for (uint32_t i = 1; i < w->slot_count; i++) { // (no clouds: a clear sky, nothing drifting over the measure's flicker probe)
        Entity e = ecs_entity_at(w, i);
        if (e != ENTITY_NONE && ecs_get(w, e, COMP_CLOUD)) ecs_destroy(w, e);
    }
    bool no_toon = getenv("REALENGINE_NO_TOON") != NULL;
    uint32_t m[6];
    for (int k = 0; k < 6; k++) {
        m[k] = mat_hash(g_toon_mats[k]);
        Material* mm = m[k] ? mat_find_hash(m[k]) : NULL;
        if (!mm || !mat_toon(mm)) printf("[RealEngine] FAIL: toon: %s missing or not Toon\n", g_toon_mats[k]);
        if (no_toon) m[k] = 0; // (the "before": the shapes' own colors, Default Lit)
    }
    Entity floor = shape(w, "Toon Stage", SHAPE_PLANE, 0, 2500, 0, 80.0f, m[5]);
    ((CStaticMesh*)ecs_get(w, floor, COMP_STATIC_MESH))->cast_shadow = 0;
    // the examples in front of the camera
    shape(w, "M_Toon", SHAPE_SPHERE, -270, 0, 70, 1.4f, m[0]);
    Entity cube = shape(w, "M_ToonRamp", SHAPE_CUBE, -90, 40, 55, 1.1f, m[1]);
    ((CTransform*)ecs_get(w, cube, COMP_TRANSFORM))->rotation[1] = 32.0f;
    shape(w, "M_ToonHatch", SHAPE_CYLINDER, 85, -10, 65, 1.3f, m[2]);
    shape(w, "M_ToonSkin", SHAPE_SPHERE, 250, 20, 70, 1.4f, m[3]);
    shape(w, "MI_ToonBlue", SHAPE_CONE, 420, 90, 60, 1.2f, m[4]);
    Entity real = shape(w, "Default Lit + Actor Outline", SHAPE_SPHERE, -450, 160, 60, 1.2f, 0);
    toon_actor_set_outline(w, real, OUTLINE_ACTOR_ON, (float[]){ 0.85f, 0.05f, 0.03f }, 3.0f);
    // a row going away (1.5 m .. 60 m): the outline's width by the distance (Auto Adaptive)
    static const uint32_t shapes[4] = { SHAPE_SPHERE, SHAPE_CUBE, SHAPE_CYLINDER, SHAPE_CONE };
    for (int i = 0; i < 16; i++) {
        float y = 450.0f + powf((float)i / 15.0f, 1.4f) * 5600.0f;
        char nm[48];
        snprintf(nm, sizeof nm, "Toon Row %d", i + 1);
        shape(w, nm, shapes[i & 3], (i & 1) ? 300.0f : -300.0f, y, 60, 1.2f, m[(i % 3 == 2) ? 4 : (i % 3)]);
    }
    // a fill that keeps the toon soft (Never Toon), a spot that forces the toon on the realistic sphere
    Entity fill = light(w, "Fill (Never Toon)", LIGHT_POINT, (float[]){ 350, -350, 260 }, 0, 0, 12000.0f, (float[]){ 1.0f, 0.82f, 0.6f });
    toon_light_set_mode(w, fill, 2);
    ((CLight*)ecs_get(w, fill, COMP_LIGHT))->cast_shadows = 0;
    Entity spot = light(w, "Spot (Force Toon)", LIGHT_SPOT, (float[]){ -450, -150, 420 }, -49.0f, 90.0f, 3000000.0f, (float[]){ 1.0f, 1.0f, 1.0f });
    toon_light_set_mode(w, spot, 1);
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -760.0f, ed->cam.pos[2] = 230.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -9.0f;
    printf("[RealEngine] Toon scene: 5 toon examples, an instance, %d actors in a row to 60 m, a Force Toon spot, a Never Toon fill%s\n", 16,
           no_toon ? " (REALENGINE_NO_TOON: Default Lit)" : "");
}

bool editor_toon_check(Editor* ed) {
    (void)ed;
    bool ok = true;
    for (int k = 0; k < 6; k++) {
        Material* mm = mat_find_hash(mat_hash(g_toon_mats[k]));
        const MatToon* tt = mm ? mat_toon_slot(mm->slot) : NULL;
        if (!mm || !mat_toon(mm) || !tt) printf("[RealEngine] FAIL: toon: %s not drawn as Toon\n", g_toon_mats[k]), ok = false;
    }
    const OutlineStats* os = outline_stats();
    if (getenv("REALENGINE_NO_OUTLINE") || getenv("REALENGINE_NO_TOON")) {
        printf("[RealEngine] Toon: the measure's before (no outline pass: %u draws)\n", os->draws);
        return ok;
    }
    // its GPU time at 1440p: the passes go by the pixel, a render of another size scaled to 2560 x 1440
    double px = (double)os->width * os->height, at1440 = px > 0.0 ? os->gpu_avg_ms * (2560.0 * 1440.0) / px : 0.0;
    printf("[RealEngine] Toon: outline %.3f ms GPU (avg; %.3f ms at 1440p, worst %.3f), %u actors, %u looks, render %ux%u, lines %.2f%% of the view, CPU %.3f ms\n",
           os->gpu_avg_ms, at1440, os->gpu_max_ms, os->draws, os->entries, os->width, os->height, os->line_share * 100.0, os->cpu_ms);
    if (os->draws < 20 || os->entries < 5) printf("[RealEngine] FAIL: toon: %u outlined actors drawn, %u looks (20 and 5 expected)\n", os->draws, os->entries), ok = false;
    if (os->line_share < 0.002 || os->line_share > 0.15)
        printf("[RealEngine] FAIL: toon: the lines cover %.2f%% of the view (0.2%% .. 15%%)\n", os->line_share * 100.0), ok = false;
    if (at1440 >= 0.5) printf("[RealEngine] FAIL: toon: the outline costs %.3f ms of GPU at 1440p (< 0.5)\n", at1440), ok = false;
    return ok;
}
