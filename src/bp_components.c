// A Blueprint's component tree (blueprint.h, notas-dev/blueprints.md): Unreal's Simple Construction Script. Each node
// of the class becomes a child entity of the actor with CAttach (its owner, its parent, its relative transform) and the
// node's component (a mesh, a light, a camera, a Spring Arm, a collision shape, an arrow). bp_components_update keeps
// them on their parents every frame: one walk of the attached rows per depth of the tree (parents before children),
// nothing allocated, nothing done when the level has no Blueprint components.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "fpcam.h"
#include "render.h"

#define DEG2RAD 0.017453293f

const char* const g_scs_names[SCS_KIND_COUNT] = {
    "Scene", "Static Mesh", "Point Light", "Spot Light", "Rect Light", "Camera", "Spring Arm",
    "Box Collision", "Sphere Collision", "Capsule Collision", "Arrow", "Blueprint Component", "Skeletal Mesh", "Audio",
};

// every component a node can hold fits its data (it didn't: the Skeletal Mesh grew past it and was dropped at spawn)
_Static_assert(sizeof(CStaticMesh) <= BP_SCS_DATA, "BP_SCS_DATA: CStaticMesh doesn't fit a Blueprint component node");
_Static_assert(sizeof(CLight) <= BP_SCS_DATA, "BP_SCS_DATA: CLight doesn't fit a Blueprint component node");
_Static_assert(sizeof(CCamera) <= BP_SCS_DATA, "BP_SCS_DATA: CCamera doesn't fit a Blueprint component node");
_Static_assert(sizeof(CSpringArm) <= BP_SCS_DATA, "BP_SCS_DATA: CSpringArm doesn't fit a Blueprint component node");
_Static_assert(sizeof(CCollision) <= BP_SCS_DATA, "BP_SCS_DATA: CCollision doesn't fit a Blueprint component node");
_Static_assert(sizeof(CArrow) <= BP_SCS_DATA, "BP_SCS_DATA: CArrow doesn't fit a Blueprint component node");
_Static_assert(offsetof(CBlueprint, ov) <= BP_SCS_DATA, "BP_SCS_DATA: CBlueprint's head doesn't fit a Blueprint component node"); // (its per-actor overrides stay at their defaults: the node keeps the head)
_Static_assert(sizeof(CSkeletalMesh) <= BP_SCS_DATA, "BP_SCS_DATA: CSkeletalMesh doesn't fit a Blueprint component node");
_Static_assert(sizeof(CAudio) <= BP_SCS_DATA, "BP_SCS_DATA: CAudio doesn't fit a Blueprint component node");

ComponentId bp_scs_component(uint32_t kind) {
    switch (kind) {
    case SCS_STATIC_MESH: return COMP_STATIC_MESH;
    case SCS_POINT_LIGHT: case SCS_SPOT_LIGHT: case SCS_RECT_LIGHT: return COMP_LIGHT;
    case SCS_CAMERA: return COMP_CAMERA;
    case SCS_SPRING_ARM: return COMP_SPRING_ARM;
    case SCS_BOX: case SCS_SPHERE: case SCS_CAPSULE: return COMP_COLLISION;
    case SCS_ARROW: return COMP_ARROW;
    case SCS_BLUEPRINT: return COMP_BLUEPRINT; // data: CBlueprint (the class's hash): its graph runs on the actor
    case SCS_SKELETAL_MESH: return COMP_SKELETAL_MESH;
    case SCS_AUDIO: return COMP_AUDIO;
    default: return COMP_COUNT;
    }
}

void bp_scs_defaults(BpScsNode* n, uint32_t kind) {
    memset(n->data, 0, sizeof n->data);
    n->kind = (uint16_t)kind;
    n->scale[0] = n->scale[1] = n->scale[2] = 1.0f;
    ComponentId c = bp_scs_component(kind);
    if (c != COMP_COUNT) { // (a component bigger than the node, CBlueprint: its head)
        uint8_t full[4096];
        if (g_components[c].size <= sizeof full) {
            memset(full, 0, g_components[c].size);
            ecs_component_defaults(c, full);
            memcpy(n->data, full, g_components[c].size < sizeof n->data ? g_components[c].size : sizeof n->data);
        }
    }
    switch (kind) {
    case SCS_STATIC_MESH: {
        CStaticMesh* m = (CStaticMesh*)n->data;
        m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.7f, m->roughness = 0.5f, m->cast_shadow = 1;
        break;
    }
    case SCS_POINT_LIGHT: case SCS_SPOT_LIGHT: case SCS_RECT_LIGHT: {
        CLight* l = (CLight*)n->data;
        l->type = kind == SCS_POINT_LIGHT ? LIGHT_POINT : kind == SCS_SPOT_LIGHT ? LIGHT_SPOT : LIGHT_RECT;
        l->color[0] = l->color[1] = l->color[2] = 1.0f;
        l->intensity = 800.0f, l->attenuation_radius = 1000.0f, l->outer_cone = 44.0f, l->cast_shadows = 1;
        l->source_width = l->source_height = 64.0f;
        break;
    }
    case SCS_CAMERA: ((CCamera*)n->data)->fov_deg = 90.0f; break; // not auto-activated: the possessed pawn's view
    case SCS_SPHERE: ((CCollision*)n->data)->shape = COLLISION_SPHERE; break;
    case SCS_CAPSULE: {
        CCollision* q = (CCollision*)n->data;
        q->shape = COLLISION_CAPSULE, q->extent[0] = 22.0f, q->extent[1] = 44.0f;
        break;
    }
    default: break;
    }
}

int bp_scs_add(BpAsset* a, uint32_t kind, int parent, const char* name) {
    if (a->nscs >= BP_MAX_SCS || kind >= SCS_KIND_COUNT) return -1;
    int i = (int)a->nscs++;
    BpScsNode* n = &a->scs[i];
    memset(n, 0, sizeof *n);
    bp_scs_defaults(n, kind);
    n->parent = (int16_t)(parent >= 0 && parent < i ? parent : BP_SCS_ROOT);
    // a unique name: Unreal's (the kind without spaces, then 1, 2, ...)
    char base[BP_NAME];
    uint32_t k = 0;
    for (const char* s = name && name[0] ? name : g_scs_names[kind]; *s && k + 1 < sizeof base; s++)
        if (*s != ' ') base[k++] = *s;
    base[k] = 0;
    snprintf(n->name, sizeof n->name, "%s", base);
    for (int sfx = 1;; sfx++) {
        bool taken = false;
        for (int j = 0; j < i && !taken; j++) taken = strcmp(a->scs[j].name, n->name) == 0;
        if (!taken) break;
        snprintf(n->name, sizeof n->name, "%.40s%d", base, sfx);
    }
    return i;
}

void bp_scs_remove(BpAsset* a, int i) {
    if (i < 0 || i >= (int)a->nscs) return;
    int up = a->scs[i].parent;
    for (uint32_t j = (uint32_t)i + 1; j < a->nscs; j++) {
        BpScsNode* n = &a->scs[j];
        if (n->parent == i) n->parent = (int16_t)up; // its children go to its parent
        else if (n->parent > i) n->parent--;
        a->scs[j - 1] = *n;
    }
    a->nscs--;
}

// --- Transforms: axes (rows: the world direction of local X, Y, Z, times the scale) and an origin ---

typedef struct Xf { float ax[3][3], o[3]; } Xf;

static void xf_of(const float loc[3], const float rot[3], const float sc[3], Xf* x) {
    float m[3][3];
    rotator_axes(rot, m);
    for (int a = 0; a < 3; a++) {
        for (int k = 0; k < 3; k++) x->ax[a][k] = m[a][k] * sc[a];
        x->o[a] = loc[a];
    }
}

static void xf_mul(const Xf* p, const Xf* l, Xf* out) { // a point: p(l(x))
    Xf r;
    for (int k = 0; k < 3; k++) r.o[k] = p->o[k] + l->o[0] * p->ax[0][k] + l->o[1] * p->ax[1][k] + l->o[2] * p->ax[2][k];
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) r.ax[a][k] = l->ax[a][0] * p->ax[0][k] + l->ax[a][1] * p->ax[1][k] + l->ax[a][2] * p->ax[2][k];
    *out = r;
}

static void xf_transform(const Xf* x, CTransform* t) {
    float u[3][3];
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(x->ax[a][0] * x->ax[a][0] + x->ax[a][1] * x->ax[a][1] + x->ax[a][2] * x->ax[a][2]);
        t->scale[a] = l;
        for (int k = 0; k < 3; k++) u[a][k] = l > 1e-8f ? x->ax[a][k] / l : (a == k ? 1.0f : 0.0f);
        t->location[a] = x->o[a];
    }
    axes_rotator(u, t->rotation);
}

// A Spring Arm's end in its parent's space (the editor's: full length, no lag, no collision test)
static void arm_end(const CSpringArm* s, const Xf* at, Xf* end) {
    float u[3][3];
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(at->ax[a][0] * at->ax[a][0] + at->ax[a][1] * at->ax[a][1] + at->ax[a][2] * at->ax[a][2]);
        for (int k = 0; k < 3; k++) u[a][k] = l > 1e-8f ? at->ax[a][k] / l : 0.0f;
    }
    for (int k = 0; k < 3; k++)
        end->o[k] = at->o[k] + s->target_offset[k] - u[0][k] * s->target_arm_length + u[0][k] * s->socket_offset[0] +
                    u[1][k] * s->socket_offset[1] + u[2][k] * s->socket_offset[2];
    memcpy(end->ax, u, sizeof u);
}

// Whether the actor's root scale reaches its components (a Character's root scale is its body's size: not passed on)
static bool root_scale_passes(const BpAsset* a) { return bp_native_of(a) != BPN_CHARACTER; }

void bp_scs_actor_space(const BpAsset* a, int i, bool socket, float axes[3][3], float origin[3]) {
    Xf chain[BP_MAX_SCS + 1];
    int path[BP_MAX_SCS], np = 0;
    for (int j = i; j >= 0 && np < BP_MAX_SCS; j = a->scs[j].parent) path[np++] = j;
    const CTransform* rt = (const CTransform*)a->comp_data[COMP_TRANSFORM];
    float one[3] = { 1, 1, 1 }, zero[3] = { 0, 0, 0 };
    xf_of(zero, zero, root_scale_passes(a) ? rt->scale : one, &chain[0]);
    Xf cur = chain[0];
    for (int p = np - 1; p >= 0; p--) {
        const BpScsNode* n = &a->scs[path[p]];
        Xf l, w;
        xf_of(n->location, n->rotation, n->scale, &l);
        xf_mul(&cur, &l, &w);
        if ((p > 0 || socket) && n->kind == SCS_SPRING_ARM) arm_end((const CSpringArm*)n->data, &w, &w); // children hang at its end
        cur = w;
    }
    memcpy(axes, cur.ax, sizeof cur.ax);
    memcpy(origin, cur.o, sizeof cur.o);
}

// --- The actors' child entities ---

bool bp_is_component(const World* w, Entity e) { return (ecs_mask(w, e) & COMP_BIT(COMP_ATTACH)) != 0; }

Entity bp_component_owner(const World* w, Entity e) {
    for (int depth = 0; depth < 8 && bp_is_component(w, e); depth++) { // a part of a mesh component: the actor above
        const CAttach* at = ecs_get((World*)w, e, COMP_ATTACH);
        if (!at || !ecs_alive(w, at->owner)) break;
        e = at->owner;
    }
    return e;
}

// The components of an actor (up to cap), any depth
static uint32_t children_of(World* w, Entity actor, Entity* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count && n < cap; r++)
            if (at[r].owner == actor && at[r].kind == ATTACH_SCS) out[n++] = ents[r];
    }
    return n;
}

void bp_components_build(World* w, Entity actor, const BpAsset* cls) {
    if (!ecs_alive(w, actor)) return;
    if (!cls) {
        const CBlueprint* b = ecs_get(w, actor, COMP_BLUEPRINT);
        cls = b ? bp_find_hash(b->asset_hash) : NULL;
    }
    Entity old[BP_MAX_SCS * 2];
    uint32_t nold = children_of(w, actor, old, BP_MAX_SCS * 2);
    for (uint32_t i = 0; i < nold; i++) ecs_destroy(w, old[i]);
    if (!cls || cls->kind != BPA_CLASS || !cls->nscs) return;
    const CTransform* rt = ecs_get(w, actor, COMP_TRANSFORM);
    uint32_t flags = rt ? rt->flags : MOBILITY_MOVABLE;
    Entity made[BP_MAX_SCS];
    for (uint32_t i = 0; i < cls->nscs; i++) {
        const BpScsNode* n = &cls->scs[i];
        ComponentId c = bp_scs_component(n->kind);
        uint64_t mask = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ATTACH);
        if (c != COMP_COUNT) mask |= COMP_BIT(c);
        Entity e = ecs_create(w, mask);
        made[i] = e;
        if (e == ENTITY_NONE) continue;
        CName* nm = ecs_get(w, e, COMP_NAME);
        snprintf(nm->text, sizeof nm->text, "%s", n->name);
        CAttach* at = ecs_get(w, e, COMP_ATTACH);
        at->owner = actor;
        at->parent = n->parent >= 0 && made[n->parent] ? made[n->parent] : actor;
        memcpy(at->location, n->location, 12);
        memcpy(at->rotation, n->rotation, 12);
        memcpy(at->scale, n->scale, 12);
        at->node = i;
        if (c != COMP_COUNT) memcpy(ecs_get(w, e, c), n->data, g_components[c].size < sizeof n->data ? g_components[c].size : sizeof n->data); // (the rest: the component's defaults, ecs_create)
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->flags = flags;
        if (rt) memcpy(t->location, rt->location, 12); // until the first update places it
    }
    bp_components_update(w, 0.0f, false);
}

void bp_components_rebuild(World* w, const BpAsset* cls) {
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CBlueprint* b = e ? ecs_get(w, e, COMP_BLUEPRINT) : NULL;
        if (!b || bp_is_component(w, e)) continue;
        const BpAsset* a = bp_find_hash(b->asset_hash);
        if (cls && a != cls) continue;
        bp_components_build(w, e, a);
    }
}

// The Spring Arm: from its start (its own transform plus the target offset) back along its X by the arm's length; the
// collision test (while playing) pulls the end in front of what stands between; the lag follows the end smoothly
static void spring_arm(World* w, const CAttach* at, CSpringArm* s, Xf* x, float dt, bool playing) {
    float rot[3];
    const CCharacter* ch = s->use_pawn_control_rotation ? ecs_get(w, at->owner, COMP_CHARACTER) : NULL;
    float u[3][3];
    if (ch) {
        rot[0] = ch->control_pitch, rot[1] = ch->control_yaw, rot[2] = 0.0f;
        rotator_axes(rot, u);
    } else
        for (int a = 0; a < 3; a++) {
            float l = sqrtf(x->ax[a][0] * x->ax[a][0] + x->ax[a][1] * x->ax[a][1] + x->ax[a][2] * x->ax[a][2]);
            for (int k = 0; k < 3; k++) u[a][k] = l > 1e-8f ? x->ax[a][k] / l : 0.0f;
        }
    float start[3], end[3];
    for (int k = 0; k < 3; k++) start[k] = x->o[k] + s->target_offset[k];
    float len = s->target_arm_length;
    for (int k = 0; k < 3; k++)
        end[k] = start[k] - u[0][k] * len + u[0][k] * s->socket_offset[0] + u[1][k] * s->socket_offset[1] + u[2][k] * s->socket_offset[2];
    if (playing && s->do_collision_test) {
        float f = bp_trace_boxes(start, end, fmaxf(s->probe_size, 0.0f));
        if (f < 1.0f)
            for (int k = 0; k < 3; k++) end[k] = start[k] + (end[k] - start[k]) * f;
        len *= f;
    }
    s->current_length = len;
    if (playing && s->enable_camera_lag && dt > 0.0f) {
        float d2 = 0.0f;
        for (int k = 0; k < 3; k++) d2 += (s->lag_pos[k] - end[k]) * (s->lag_pos[k] - end[k]);
        if (d2 < 2000.0f * 2000.0f) { // (a teleport: no lag)
            float k1 = 1.0f - expf(-fmaxf(s->camera_lag_speed, 0.0f) * dt);
            for (int k = 0; k < 3; k++) end[k] = s->lag_pos[k] + (end[k] - s->lag_pos[k]) * k1;
        }
    } else if (playing && dt > 0.0f && s->target_arm_length < 1.0f && fabsf(s->lag_pos[2] - end[2]) < 60.0f) {
        // a first person view without lag: its height eases over a step (a step up snaps the body up to 45 cm: the
        // view jumped). A third person one: the templates' and demos' Camera Lag eases it (camera_lag_on)
        float k1 = 1.0f - expf(-20.0f * dt);
        end[2] = s->lag_pos[2] + (end[2] - s->lag_pos[2]) * k1;
    }
    memcpy(s->lag_pos, end, 12);
    memcpy(x->ax, u, sizeof u);
    memcpy(x->o, end, 12);
}

void bp_components_update(World* w, float dt, bool playing) {
    // the components whose actor (or parent) is gone go too
    Entity dead[256];
    uint32_t ndead = 0, max_node = 0;
    bool any = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        any = true;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            if ((!ecs_alive(w, at[r].owner) || !ecs_alive(w, at[r].parent)) && ndead < 256) dead[ndead++] = ents[r];
            if (at[r].node > max_node && at[r].kind == ATTACH_SCS) max_node = at[r].node;
        }
    }
    if (!any) return;
    for (uint32_t i = 0; i < ndead; i++) ecs_destroy(w, dead[i]);
    if (max_node >= BP_MAX_SCS) max_node = BP_MAX_SCS - 1;
    // parents come first in a class's tree: node 0 of every actor, then node 1, ...
    for (uint32_t node = 0; node <= max_node; node++)
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
            const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
            CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
            CSpringArm* arms = (ar->mask & COMP_BIT(COMP_SPRING_ARM)) ? ecs_arch_column(ar, COMP_SPRING_ARM) : NULL;
            const CCamera* cams = playing && dt > 0.0f && (ar->mask & COMP_BIT(COMP_CAMERA)) ? ecs_arch_column(ar, COMP_CAMERA) : NULL;
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t r = 0; r < ar->count; r++) {
                if (at[r].node != node || at[r].kind != ATTACH_SCS) continue; // (anim.c places the skeletal parts)
                const CTransform* pt = ecs_get(w, at[r].parent, COMP_TRANSFORM);
                const CTransform* ot = ecs_get(w, at[r].owner, COMP_TRANSFORM);
                if (!pt || !ot) continue;
                Xf p, l, x;
                float one[3] = { 1, 1, 1 };
                bool root = at[r].parent == at[r].owner;
                xf_of(pt->location, pt->rotation, root && ecs_get(w, at[r].owner, COMP_CHARACTER) ? one : pt->scale, &p);
                xf_of(at[r].location, at[r].rotation, at[r].scale, &l);
                xf_mul(&p, &l, &x);
                if (arms) spring_arm(w, &at[r], &arms[r], &x, dt, playing);
                if (cams) fpcam_component(w, ents[r], at[r].owner, &cams[r], x.o, x.ax, dt); // (Q12: the player's First Person Camera, the shakes; its children follow)
                CTransform nt;
                xf_transform(&x, &nt);
                nt.flags = (ot->flags & (TRANSFORM_MOBILITY_MASK | TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME));
                if (memcmp(&nt, &tr[r], sizeof nt)) {
                    tr[r] = nt;
                    ecs_touch(w, ents[r]);
                }
            }
        }
}

// One actor's components placed now (a vehicle moved them after the frame's placing: its wheels, its doors)
void bp_components_place_actor(World* w, Entity actor) {
    const CTransform* ot = ecs_get(w, actor, COMP_TRANSFORM);
    if (!ot) return;
    for (uint32_t node = 1; node < BP_MAX_SCS; node++) {
        bool any = false;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
            const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
            CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
            const Entity* ents = ecs_arch_entities(ar);
            for (uint32_t r = 0; r < ar->count; r++) {
                if (at[r].owner != actor || at[r].kind != ATTACH_SCS) continue;
                any |= at[r].node >= node;
                if (at[r].node != node) continue;
                const CTransform* pt = ecs_get(w, at[r].parent, COMP_TRANSFORM);
                if (!pt) continue;
                Xf p, l, x;
                xf_of(pt->location, pt->rotation, pt->scale, &p);
                xf_of(at[r].location, at[r].rotation, at[r].scale, &l);
                xf_mul(&p, &l, &x);
                CTransform nt;
                xf_transform(&x, &nt);
                nt.flags = (ot->flags & (TRANSFORM_MOBILITY_MASK | TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME));
                if (memcmp(&nt, &tr[r], sizeof nt)) {
                    tr[r] = nt;
                    ecs_touch(w, ents[r]);
                }
            }
        }
        if (!any) break;
    }
}

bool bp_component_camera(World* w, Entity actor, float pos[3], float* yaw, float* pitch, float* fov) {
    const CCamera* c = NULL;
    if (!bp_component_camera_ex(w, actor, pos, yaw, pitch, NULL, &c)) return false;
    if (fov) *fov = c->fov_deg > 1.0f ? c->fov_deg : 90.0f;
    return true;
}

// (Q12) ... with its roll (the First Person Camera's lean, a shake's) and the camera itself
bool bp_component_camera_ex(World* w, Entity actor, float pos[3], float* yaw, float* pitch, float* roll, const CCamera** cam_out) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_CAMERA) | COMP_BIT(COMP_TRANSFORM);
        if ((ar->mask & need) != need) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
        const CCamera* cam = ecs_arch_column(ar, COMP_CAMERA);
        for (uint32_t r = 0; r < ar->count; r++)
            if (at[r].owner == actor) {
                memcpy(pos, tr[r].location, 12);
                *pitch = tr[r].rotation[0];
                *yaw = tr[r].rotation[1];
                if (roll) *roll = tr[r].rotation[2];
                if (cam_out) *cam_out = &cam[r];
                return true;
            }
    }
    return false;
}
