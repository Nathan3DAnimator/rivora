// Decals (decal.h)
#include "decal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "material.h"
#include "render.h"

static DecalStats g_stats;
const DecalStats* decal_stats(void) { return &g_stats; }

typedef struct Cand { GpuDecal g; int32_t sort; float size, dist; } Cand;
static Cand g_cand[DECAL_MAX * 2];

static int cand_cmp(const void* a, const void* b) {
    const Cand *x = a, *y = b;
    if (x->sort != y->sort) return x->sort < y->sort ? -1 : 1;
    return x->size > y->size ? -1 : x->size < y->size ? 1 : 0; // (the bigger first: the small ones over it)
}
static int dist_cmp(const void* a, const void* b) {
    const Cand *x = a, *y = b;
    return x->dist < y->dist ? -1 : x->dist > y->dist ? 1 : 0;
}

static float smooth01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

uint32_t decals_gather(World* w, const float vp[16], uint32_t vw, uint32_t vh, const float cam[3], GpuDecal* out, uint32_t cap) {
    uint32_t nc = 0, total = 0;
    const uint32_t ccap = (uint32_t)(sizeof g_cand / sizeof g_cand[0]);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_DECAL) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CDecal* dc = ecs_arch_column(a, COMP_DECAL);
        const CTransform* tc = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count && nc < ccap; i++) {
            const CDecal* d = &dc[i];
            const CTransform* t = &tc[i];
            total++;
            if (!d->material || d->opacity <= 0.0f) continue;
            Material* m = mat_find_hash(d->material);
            if (!m || !m->slot) continue;
            // the fades: its life's end, the distance
            float op = d->opacity > 1.0f ? 1.0f : d->opacity;
            if (d->life_span > 0.0f && d->fade_out > 0.0f) {
                float left = d->life_span - d->age;
                op *= left <= 0.0f ? 0.0f : left >= d->fade_out ? 1.0f : left / d->fade_out;
            }
            float dv[3] = { t->location[0] - cam[0], t->location[1] - cam[1], t->location[2] - cam[2] };
            float dist = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            if (d->fade_distance > 0.0f) op *= 1.0f - smooth01(d->fade_distance * 0.8f, d->fade_distance, dist);
            if (op < 0.002f) continue;
            float ax[3][3];
            rotator_axes(t->rotation, ax);
            float he[3];
            for (int k = 0; k < 3; k++) he[k] = fmaxf(fabsf(t->scale[k]) * 100.0f, 0.1f);
            // its screen rectangle from the box's corners (one behind the camera: the whole screen)
            float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
            bool behind = false;
            for (int c = 0; c < 8 && !behind; c++) {
                float p[3];
                for (int k = 0; k < 3; k++)
                    p[k] = t->location[k] + ax[0][k] * he[0] * ((c & 1) ? 1.0f : -1.0f) + ax[1][k] * he[1] * ((c & 2) ? 1.0f : -1.0f) +
                           ax[2][k] * he[2] * ((c & 4) ? 1.0f : -1.0f);
                float cx = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12], cy = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
                float cw = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
                if (cw <= 1.0f) { behind = true; break; }
                float sx = (cx / cw * 0.5f + 0.5f) * (float)vw, sy = (cy / cw * 0.5f + 0.5f) * (float)vh;
                lo[0] = fminf(lo[0], sx), lo[1] = fminf(lo[1], sy), hi[0] = fmaxf(hi[0], sx), hi[1] = fmaxf(hi[1], sy);
            }
            if (behind) lo[0] = lo[1] = 0.0f, hi[0] = (float)vw, hi[1] = (float)vh;
            if (hi[0] < 0.0f || hi[1] < 0.0f || lo[0] > (float)vw || lo[1] > (float)vh) continue; // (out of view)
            Cand* c = &g_cand[nc++];
            GpuDecal* g = &c->g;
            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 3; k++) g->row[r][k] = ax[r][k] / he[r];
                g->row[r][3] = -(ax[r][0] * t->location[0] + ax[r][1] * t->location[1] + ax[r][2] * t->location[2]) / he[r];
            }
            g->rect[0] = lo[0] - 1.0f, g->rect[1] = lo[1] - 1.0f, g->rect[2] = hi[0] + 1.0f, g->rect[3] = hi[1] + 1.0f; // (a pixel of margin)
            memcpy(g->axis, ax[0], 12), g->axis[3] = op;
            memcpy(g->u_axis, ax[1], 12), g->u_axis[3] = (float)m->slot;
            memcpy(g->v_axis, ax[2], 12);
            float af = d->angle_fade <= 0.0f || d->angle_fade >= 89.0f ? 89.0f : d->angle_fade;
            g->v_axis[3] = cosf(af * 3.14159265f / 180.0f);
            // (the flags in the slot's sign would cost a branch per decal: all three always, as Unreal's default decal)
            c->sort = d->sort_order, c->size = he[1] * he[2], c->dist = dist;
        }
    }
    if (nc > cap) { // (too many in view: the nearest)
        qsort(g_cand, nc, sizeof g_cand[0], dist_cmp);
        nc = cap;
    }
    qsort(g_cand, nc, sizeof g_cand[0], cand_cmp);
    for (uint32_t i = 0; i < nc; i++) out[i] = g_cand[i].g;
    g_stats.total = total, g_stats.drawn = nc;
    return nc;
}

void decals_update(World* w, float dt, bool playing) {
    if (!playing || dt <= 0.0f) return;
    Entity gone[256];
    uint32_t ng = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_DECAL)) || !a->count) continue;
        CDecal* dc = ecs_arch_column(a, COMP_DECAL);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (dc[i].life_span <= 0.0f) continue;
            dc[i].age += dt;
            if (dc[i].age >= dc[i].life_span && ng < 256) gone[ng++] = ents[i];
        }
    }
    for (uint32_t i = 0; i < ng; i++) ecs_destroy(w, gone[i]); // (after the walk: the rows move)
}

Entity decal_spawn(World* w, uint32_t material, const float loc[3], const float rot[3], const float size[3], float life_span) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_DECAL));
    if (e == ENTITY_NONE) return e;
    CName* n = ecs_get(w, e, COMP_NAME);
    memcpy(n->text, "Decal", 6);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (loc) memcpy(t->location, loc, 12);
    if (rot) memcpy(t->rotation, rot, 12);
    for (int k = 0; k < 3; k++) t->scale[k] = size && size[k] > 0.0f ? size[k] / 100.0f : (k ? 1.0f : 0.5f);
    t->flags = MOBILITY_MOVABLE;
    CDecal* d = ecs_get(w, e, COMP_DECAL);
    d->material = material, d->life_span = life_span > 0.0f ? life_span : 0.0f;
    return e;
}
