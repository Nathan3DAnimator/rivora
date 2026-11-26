// (2026-11-25, item 17) --scene selection: the selection's outline on every kind of actor (notas-dev/editor.md "Destaque
// da seleção"). A cube; an actor with no mesh of its own and two meshes hanging from it (a Blueprint's components, a
// Skeletal Mesh's parts, a vehicle's, a Landscape's chunks: CAttach); the Skeletal Mesh Actor of --import (animated by
// --play-eject: its outline read at two moments, two silhouettes); a point light and an Empty (their shapes, no mesh).
// Each one selected alone for 30 frames, the mask read back at the end of its turn (render_mask_read): its pixels set,
// FAIL at 0. The cost: the mask pass's GPU time at 1440p under 0.3 ms. Run with --game-view: the icons hidden (G)
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "crowd.h"
#include "ecs.h"
#include "editor.h"
#include "render.h"
#include "vkctx.h"

extern uint32_t g_sel_shapes_drawn, g_sel_fallback_drawn; // (editor_viewport.c)

enum { ST_CUBE, ST_GROUP, ST_SKIN_A, ST_LIGHT, ST_EMPTY, ST_SKIN_B, ST_DEEP, ST_CROWD, ST_NONE, ST_COUNT };
static const char* const g_st_name[ST_COUNT] = { "cube", "actor with components", "skin (first)", "light", "Empty", "skin (second)", "deep chain", "crowd", "nothing" };
static struct {
    Entity e[ST_COUNT];
    uint32_t set[ST_COUNT], draws[ST_COUNT], shapes[ST_COUNT], fallback[ST_COUNT];
    uint64_t hash[ST_COUNT];
    uint32_t frame, shapes0, fallback0;
    uint8_t* gpix; // the group's mask (its inner gaps counted at the check)
    uint32_t gw, gh;
    double mask_ms_max, turn_ms;
    bool done[ST_COUNT];
} S;

static Entity mesh_actor(World* w, const char* name, uint32_t mesh, const float at[3], float s) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = s, t->flags = MOBILITY_STATIC;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    ecs_component_defaults(COMP_STATIC_MESH, sm);
    sm->mesh = mesh, sm->cast_shadow = 1;
    sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.6f, sm->roughness = 0.5f;
    return e;
}

static Entity bare_actor(World* w, const char* name, const float at[3], uint64_t extra) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | extra);
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    return e;
}

// a mesh hanging from `owner` (a Blueprint's component): its CAttach and its place in the world
static void component(World* w, Entity owner, uint32_t mesh, float dx, float dz, float s) {
    const CTransform* ot = ecs_get(w, owner, COMP_TRANSFORM);
    float at[3] = { ot->location[0] + dx, ot->location[1], ot->location[2] + dz };
    Entity e = mesh_actor(w, "Sel Component", mesh, at, s);
    if (e == ENTITY_NONE) return;
    ecs_add(w, e, COMP_ATTACH);
    CAttach* a = ecs_get(w, e, COMP_ATTACH);
    memset(a, 0, sizeof *a);
    a->owner = owner, a->parent = owner, a->kind = ATTACH_SCS;
    a->location[0] = dx, a->location[2] = dz, a->scale[0] = a->scale[1] = a->scale[2] = s;
}

void selection_test_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    World* w = &ed->world;
    ed->selected = ENTITY_NONE;
    editor_sky_preset(ed, 0, 0.0);
    memset(&S, 0, sizeof S);
    Entity floor = mesh_actor(w, "Sel Floor", SHAPE_PLANE, (float[]){ 0, 300, 0 }, 30.0f);
    (void)floor;
    S.e[ST_CUBE] = mesh_actor(w, "Sel Cube", SHAPE_CUBE, (float[]){ -420, 250, 60 }, 1.2f);
    S.e[ST_GROUP] = bare_actor(w, "Sel Actor With Components", (float[]){ -170, 250, 0 }, 0);
    // (two cubes side by side, 1.5 cm apart: a gap of ~2 px between the parts of one actor, a rig's neck, a Mannequin's
    // joints; one silhouette, no line inside)
    component(w, S.e[ST_GROUP], SHAPE_CUBE, -50.75f, 50.0f, 1.0f);
    component(w, S.e[ST_GROUP], SHAPE_CUBE, 50.75f, 50.0f, 1.0f);
    // (2026-11-25) 12 parts each hanging from the one before (a rig's pelvis, spine, neck, head...): all of them in the
    // actor's outline, the actor alone selected
    S.e[ST_DEEP] = bare_actor(w, "Sel Deep Chain", (float[]){ 650, 250, 0 }, 0);
    {
        // (an inner component M, hanging from the actor, not selected: the parts' owner, as a Skeletal Mesh component is
        // its parts'; parts without a live owner are cleared away)
        Entity m = bare_actor(w, "Sel Chain Mesh", (float[]){ 650, 250, 0 }, 0);
        if (m != ENTITY_NONE && ecs_add(w, m, COMP_ATTACH)) {
            CAttach* ma = ecs_get(w, m, COMP_ATTACH);
            memset(ma, 0, sizeof *ma);
            ma->owner = S.e[ST_DEEP], ma->parent = S.e[ST_DEEP], ma->kind = ATTACH_SCS, ma->scale[0] = ma->scale[1] = ma->scale[2] = 1.0f;
        }
        Entity prev = m != ENTITY_NONE ? m : S.e[ST_DEEP];
        for (int k = 0; k < 12; k++) {
            const CTransform* pt = ecs_get(w, S.e[ST_DEEP], COMP_TRANSFORM);
            float at[3] = { pt->location[0], pt->location[1], pt->location[2] + 12.0f + 18.0f * (float)k };
            Entity e = mesh_actor(w, "Sel Chain Part", SHAPE_CUBE, at, 0.16f);
            if (e == ENTITY_NONE || !ecs_add(w, e, COMP_ATTACH)) break;
            CAttach* a = ecs_get(w, e, COMP_ATTACH);
            memset(a, 0, sizeof *a);
            a->owner = m, a->parent = prev, a->kind = ATTACH_SCS; // (its owner M not selected: only the chain, 13 deep, leads to the actor)
            a->location[2] = 18.0f, a->scale[0] = a->scale[1] = a->scale[2] = 0.16f;
            prev = e;
        }
    }
    S.e[ST_LIGHT] = bare_actor(w, "Sel Point Light", (float[]){ 220, 250, 180 }, COMP_BIT(COMP_LIGHT));
    CLight* l = ecs_get(w, S.e[ST_LIGHT], COMP_LIGHT);
    ecs_component_defaults(COMP_LIGHT, l);
    l->type = LIGHT_POINT, l->intensity = 8000.0f, l->attenuation_radius = 400.0f;
    S.e[ST_EMPTY] = bare_actor(w, "Sel Empty", (float[]){ 420, 250, 100 }, 0);
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -650.0f, ed->cam.pos[2] = 260.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -12.0f;
    printf("[RealEngine] Selection scene: a cube, an actor with 2 components, a light, an Empty (and the imported Skeletal Mesh)\n");
}

// each turn 30 frames: selected at its start, read at its 25th (the mask of the frame before, the GPU idled)
void selection_test_tick(Editor* ed, VkCtx* vk) {
    World* w = &ed->world;
    uint32_t f = S.frame++;
    if (f < 60) return; // (the warm-up: the skin found, its animation going)
    if (!S.e[ST_SKIN_A]) // the Skeletal Mesh Actor --import made (after the scene)
        for (uint32_t s = 1; s < w->slot_count && !S.e[ST_SKIN_A]; s++) {
            Entity e = ecs_entity_at(w, s);
            if (e && ecs_get(w, e, COMP_SKELETAL_MESH)) S.e[ST_SKIN_A] = S.e[ST_SKIN_B] = e;
        }
    if (!S.e[ST_CROWD]) { // (2026-11-25, item 17 part 2) a Crowd Spawner: 30 agents of its default, the Mannequin in parts
        // (a crowd draws a Skeletal Mesh made of parts, one per bone; the imported rig is a skin: its crowd bakes no parts
        // and draws nothing - the skins' crowds by VAT are still to come)
        Entity c = bare_actor(w, "Sel Crowd", (float[]){ -200, 900, 0 }, COMP_BIT(COMP_CROWD));
        if (c != ENTITY_NONE) {
            CCrowd* cc = ecs_get(w, c, COMP_CROWD);
            ecs_component_defaults(COMP_CROWD, cc);
            cc->count = 30, cc->mesh = 0, cc->radius = 300.0f;
            S.e[ST_CROWD] = c;
        }
    }
    uint32_t k = (f - 60) / 30, at = (f - 60) % 30;
    if (k >= ST_COUNT) return;
    if (at == 0) {
        if (k == ST_NONE || !S.e[k]) editor_select_none(ed);
        else editor_select(ed, S.e[k], SEL_REPLACE);
        S.shapes0 = g_sel_shapes_drawn, S.fallback0 = g_sel_fallback_drawn;
        if (k == ST_DEEP) { // (the chain's parts: alive, hanging where)
            uint32_t alive = 0, first_parent = 0;
            for (uint32_t s2 = 1; s2 < w->slot_count; s2++) {
                Entity e = ecs_entity_at(w, s2);
                const CName* nm = e ? ecs_get(w, e, COMP_NAME) : NULL;
                const CAttach* a = e ? ecs_get(w, e, COMP_ATTACH) : NULL;
                if (nm && !strcmp(nm->text, "Sel Chain Part")) {
                    alive++;
                    if (a && !first_parent) first_parent = entity_index((Entity)a->parent) | (a->kind << 24);
                }
            }
            printf("[RealEngine] Selection: deep chain: %u parts alive, the first's parent slot %u kind %u, the actor's slot %u\n", alive,
                   first_parent & 0xFFFFFFu, first_parent >> 24, entity_index(S.e[ST_DEEP]));
        }
    }
    if (at == 15) S.turn_ms = 1e9;
    if (at > 15 && at < 25) S.turn_ms = fmin(S.turn_ms, render_stats()->gpu_mask_ms);
    if (at == 25 && !S.done[k]) {
        if (k == ST_GROUP && !S.gpix) S.gpix = malloc((size_t)8192 * 4608);
        render_mask_read(vk, &S.set[k], &S.hash[k], &S.draws[k], k == ST_GROUP ? S.gpix : NULL, k == ST_GROUP ? (size_t)8192 * 4608 : 0, &S.gw, &S.gh);
        S.shapes[k] = g_sel_shapes_drawn - S.shapes0, S.fallback[k] = g_sel_fallback_drawn - S.fallback0;
        S.done[k] = true;
        // (the turn's least of its frames 15-25: its cost alone; one frame's read caught a hitch of the GPU in the suite,
        // 0.322 ms at 1440p after skin_5000 against 0.028 alone)
        double ms = fmin(S.turn_ms, render_stats()->gpu_mask_ms);
        if (ms > S.mask_ms_max) S.mask_ms_max = ms;
        printf("[RealEngine] Selection: %s: %u mask draws, %u pixels, %u shapes (%u of no mesh), the mask %.3f ms\n", g_st_name[k], S.draws[k], S.set[k],
               S.shapes[k], S.fallback[k], ms);
        if (k == ST_CROWD) { // (what the crowds draw: the spawner's run, its agents and parts, the room the scene holds for them)
            const CrowdDraw* cd;
            uint32_t nc = crowd_draws(&cd);
            for (uint32_t i = 0; i < nc; i++)
                printf("[RealEngine] Selection: crowd run %u: spawner slot %u (ours %u), %u agents x %u parts, mesh %s; the scene's room for crowds %u\n", i,
                       entity_index(cd[i].e), entity_index(S.e[ST_CROWD]), cd[i].nagents, cd[i].nparts, cd[i].mesh ? "yes" : "none",
                       crowd_instances());
            if (!nc) printf("[RealEngine] Selection: no crowd draws (%u crowds, %u agents)\n", crowd_stats()->crowds, crowd_stats()->agents);
        }
    }
}

bool selection_test_check(Editor* ed) {
    (void)ed;
    bool ok = true;
    for (int k = 0; k < ST_COUNT; k++)
        if (!S.done[k]) printf("[RealEngine] FAIL: selection: the %s's turn never came (the measure too short)\n", g_st_name[k]), ok = false;
    if (!ok) return false;
    const int mesh_kinds[5] = { ST_CUBE, ST_GROUP, ST_SKIN_A, ST_SKIN_B, ST_DEEP };
    for (int i = 0; i < 5; i++)
        if (!S.set[mesh_kinds[i]]) printf("[RealEngine] FAIL: selection: no outline on the %s (0 pixels in the mask)\n", g_st_name[mesh_kinds[i]]), ok = false;
    if (S.draws[ST_CROWD] < 30 || !S.set[ST_CROWD]) printf("[RealEngine] FAIL: selection: the crowd's agents drew %u (30 agents expected), %u px\n", S.draws[ST_CROWD], S.set[ST_CROWD]), ok = false;
    if (S.draws[ST_DEEP] < 12) printf("[RealEngine] FAIL: selection: the deep chain drew %u of its 12 parts\n", S.draws[ST_DEEP]), ok = false;
    if (S.draws[ST_GROUP] < 2) printf("[RealEngine] FAIL: selection: the actor with components drew %u meshes (its 2 expected)\n", S.draws[ST_GROUP]), ok = false;
    if (S.set[ST_SKIN_A] && S.hash[ST_SKIN_A] == S.hash[ST_SKIN_B])
        printf("[RealEngine] FAIL: selection: the skin's outline the same at two moments (its bind pose, not its animation)\n"), ok = false;
    { // (2026-11-25) the actor with two parts: composite.frag's rule on its mask. The old one drew a line in the gap between
        // the parts (an outside pixel with a selected id 2 px away); the new one leaves a pixel alone when the same id stands
        // on both opposite sides (its taps in pairs): the gap is inside, the outer silhouette alone outlined
        static const int tp[8][2] = { { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 } };
        uint32_t old_gap = 0, outline = 0, side[2] = { 0, 0 };
        double cx = 0.0; // (the mask's middle column: the two parts each side of it, each with its own outer border)
        uint64_t cn = 0;
        for (uint32_t y = 0; S.gpix && y < S.gh; y++)
            for (uint32_t x = 0; x < S.gw; x++)
                if (S.gpix[(size_t)y * S.gw + x]) cx += x, cn++;
        cx = cn ? cx / (double)cn : 0.0;
        for (uint32_t y = 0; S.gpix && y < S.gh; y++)
            for (uint32_t x = 0; x < S.gw; x++) {
                if (S.gpix[(size_t)y * S.gw + x]) continue;
                uint8_t tv[8];
                bool by = false, gap = false;
                for (int i = 0; i < 8; i++) {
                    int xx = (int)x + tp[i][0], yy = (int)y + tp[i][1];
                    xx = xx < 0 ? 0 : xx >= (int)S.gw ? (int)S.gw - 1 : xx, yy = yy < 0 ? 0 : yy >= (int)S.gh ? (int)S.gh - 1 : yy;
                    tv[i] = S.gpix[(size_t)yy * S.gw + (size_t)xx];
                    by |= tv[i] != 0;
                }
                for (int i = 0; i < 8; i += 2) gap |= tv[i] != 0 && tv[i] == tv[i + 1];
                if (by && gap) old_gap++;
                if (by && !gap) outline++, side[(double)x > cx ? 1 : 0]++;
            }
        printf("[RealEngine] Selection: the actor with two parts: %u outline pixels, %u gap pixels between its parts left unlined (the old rule lined them)\n",
               outline, old_gap);
        if (!old_gap) printf("[RealEngine] FAIL: selection: no gap between the two parts on screen (the test's own setup)\n"), ok = false;
        if (!outline) printf("[RealEngine] FAIL: selection: the actor with two parts has no outer outline\n"), ok = false;
        printf("[RealEngine] Selection: the two parts' outer borders: %u px left, %u px right\n", side[0], side[1]);
        if (!side[0] || !side[1]) printf("[RealEngine] FAIL: selection: a part of the actor lost its outline (left %u, right %u px)\n", side[0], side[1]), ok = false;
    }
    if (!S.fallback[ST_EMPTY]) printf("[RealEngine] FAIL: selection: the Empty showed no shape\n"), ok = false;
    if (!S.shapes[ST_LIGHT]) printf("[RealEngine] FAIL: selection: the light showed no shape\n"), ok = false;
    if (S.set[ST_NONE] || S.draws[ST_NONE]) printf("[RealEngine] FAIL: selection: %u pixels with nothing selected\n", S.set[ST_NONE]), ok = false;
    const RenderStats* rs = render_stats();
    double px = (double)rs->width * rs->height, at1440 = px > 0.0 ? S.mask_ms_max * (2560.0 * 1440.0) / px : S.mask_ms_max;
    printf("[RealEngine] Selection: the mask pass %.3f ms of GPU at most (%.3f at 1440p)\n", S.mask_ms_max, at1440);
    if (at1440 >= 0.3) printf("[RealEngine] FAIL: selection: the mask costs %.3f ms at 1440p (< 0.3)\n", at1440), ok = false;
    if (ok) printf("[RealEngine] Selection OK\n");
    return ok;
}
