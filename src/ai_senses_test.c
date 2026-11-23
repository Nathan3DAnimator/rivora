// (Q24) --scene ai_senses: the AI's damage sense and local avoidance (notas-dev/ia.md, notas-dev/auditoria-ia-programacao.md)
//   * the damage sense: a Victim and a Control (team 2, no tree or brain, facing +x, sight 3000 cm in a 120 degree cone)
//     and a Shooter (team 1) behind them; at 1 s the Shooter hits the Victim (Apply Damage). The Victim must know the
//     Shooter at once, turn and see it; the Control, not hit, must know nothing.
//   * the local avoidance: two Characters walking head-on along the same line (AI Move To, each to the other's start).
//     They must pass without walking through each other (the closest pass >= 80% of their two radii) and both arrive.
//     REALENGINE_AI_AVOID=0 shows the old way (they walk through each other; the check then only reports it).
#include "ai_senses_test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "anim.h"
#include "blueprint.h"
#include "components.h"
#include "ecs.h"
#include "health.h"

static struct {
    bool on, hit, ordered;
    float t;
    Entity victim, control, shooter, walker[2];
    float goal[2][3];
    float closest;   // cm: the walkers' closest pass
    float hit_at, saw_after; // s: when it was hit; how long until the Victim saw the Shooter (< 0: not yet)
    double ai_ms_sum;
    uint32_t frames, avoid_frames;
} S;

static Entity character(World* w, const char* name, float x, float y, float yaw, uint32_t team, bool health) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH) |
                                 COMP_BIT(COMP_AI_AGENT) | (health ? COMP_BIT(COMP_HEALTH) : 0));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 92.0f, t->rotation[1] = yaw;
    t->flags = MOBILITY_MOVABLE;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    c->max_walk_speed = 450.0f, c->jump_z_velocity = 520.0f, c->gravity_scale = 1.0f, c->radius = 34.0f, c->half_height = 88.0f;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Mannequin");
    sk->mesh = m ? m->hash : 0, sk->anim_class = abp ? abp->hash : 0;
    CAIAgent* a = ecs_get(w, e, COMP_AI_AGENT);
    a->mode = AI_MODE_NONE, a->team = team;
    a->sight_radius = 3000.0f, a->sight_angle = 120.0f, a->share_radius = 0.0f; // (the Control learns nothing from the Victim)
    if (health) {
        CHealth* h = ecs_get(w, e, COMP_HEALTH);
        h->max_health = h->health = 100.0f;
    }
    return e;
}

bool ai_senses_test_scene(Editor* ed, const char* name) {
    if (strcmp(name, "ai_senses")) return false;
    memset(&S, 0, sizeof S);
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    World* w = &ed->world;
    Entity fl = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    CTransform* ft = ecs_get(w, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 40.0f, ft->scale[2] = 1.0f;
    ft->flags = MOBILITY_STATIC;
    CStaticMesh* fm = ecs_get(w, fl, COMP_STATIC_MESH);
    fm->mesh = SHAPE_PLANE, fm->roughness = 0.75f, fm->cast_shadow = 1;
    fm->base_color[0] = fm->base_color[1] = fm->base_color[2] = 0.5f;
    Entity sun = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    CTransform* st = ecs_get(w, sun, COMP_TRANSFORM);
    st->location[2] = 800.0f, st->rotation[0] = -50.0f, st->rotation[1] = 30.0f;
    ((CLight*)ecs_get(w, sun, COMP_LIGHT))->type = LIGHT_DIRECTIONAL;
    Entity sky = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    ((CTransform*)ecs_get(w, sky, COMP_TRANSFORM))->location[2] = 600.0f;
    ((CLight*)ecs_get(w, sky, COMP_LIGHT))->type = LIGHT_SKY;
    // the damage sense: both face +x, the Shooter is 10 m behind them
    S.victim = character(w, "Victim", 0.0f, 700.0f, 0.0f, 2, true);
    S.control = character(w, "Control", 0.0f, -100.0f, 0.0f, 2, true);
    S.shooter = character(w, "Shooter", -1000.0f, 300.0f, 0.0f, 1, false);
    // the avoidance: head-on along y = -1300 (team 3: no targets among them)
    S.walker[0] = character(w, "WalkerA", -500.0f, -1300.0f, 0.0f, 3, false);
    S.walker[1] = character(w, "WalkerB", 500.0f, -1300.0f, 180.0f, 3, false);
    S.goal[0][0] = 500.0f, S.goal[0][1] = -1300.0f, S.goal[0][2] = 92.0f;
    S.goal[1][0] = -500.0f, S.goal[1][1] = -1300.0f, S.goal[1][2] = 92.0f;
    S.closest = 1e9f, S.saw_after = -1.0f;
    S.on = true;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -2600.0f, ed->cam.pos[2] = 2000.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -45.0f;
    return true;
}

static bool knows(Entity e, bool* sees) {
    static AiDebug d[16];
    uint32_t n = ai_debug(d, 16);
    for (uint32_t k = 0; k < n; k++)
        if (d[k].pawn == e) {
            if (sees) *sees = d[k].sees;
            return d[k].has_target;
        }
    if (sees) *sees = false;
    return false;
}

void ai_senses_test_tick(Editor* ed, float dt) {
    if (!S.on || !ed->playing) return;
    World* w = &ed->world;
    S.t += dt;
    if (!S.ordered && S.t > 0.5f) { // (the agents exist after the AI's first update)
        S.ordered = ai_move_to(w, S.walker[0], S.goal[0], 40.0f) && ai_move_to(w, S.walker[1], S.goal[1], 40.0f);
        if (!S.ordered) printf("[RealEngine] FAIL: ai_senses: AI Move To refused\n");
        S.ordered = true;
    }
    if (!S.hit && S.t > 1.0f) {
        bool before = knows(S.victim, NULL);
        float taken = health_apply_damage(w, S.victim, 10.0f, "", S.shooter, S.shooter);
        printf("[RealEngine] ai_senses: the Shooter hits the Victim from behind (%.0f damage; knew it before: %s)\n", taken, before ? "yes" : "no");
        S.hit = true, S.hit_at = S.t;
    }
    bool sees = false;
    if (S.hit && S.saw_after < 0.0f && knows(S.victim, &sees) && sees) S.saw_after = S.t - S.hit_at;
    const CTransform* a = ecs_get(w, S.walker[0], COMP_TRANSFORM);
    const CTransform* b = ecs_get(w, S.walker[1], COMP_TRANSFORM);
    if (a && b && S.ordered) S.closest = fminf(S.closest, hypotf(a->location[0] - b->location[0], a->location[1] - b->location[1]));
    S.ai_ms_sum += ai_stats()->ms, S.frames++;
    S.avoid_frames += ai_stats()->avoiding > 0;
}

bool ai_senses_test_check(Editor* ed) {
    if (!S.on) return true;
    S.on = false;
    World* w = &ed->world;
    bool vsees = false, csees = false;
    bool vknows = knows(S.victim, &vsees), cknows = knows(S.control, &csees);
    float arrive[2] = { 1e9f, 1e9f };
    for (int i = 0; i < 2; i++) {
        const CTransform* t = ecs_get(w, S.walker[i], COMP_TRANSFORM);
        if (t) arrive[i] = hypotf(t->location[0] - S.goal[i][0], t->location[1] - S.goal[i][1]);
    }
    bool avoid = ai_avoidance();
    printf("[RealEngine] ai_senses: damage sense: Victim knows %s, sees %s (%.2f s after the hit), Control knows %s; %u hits sensed\n",
           vknows ? "yes" : "no", vsees ? "yes" : "no", S.saw_after, cknows ? "yes" : "no", ai_stats()->damage_sensed);
    printf("[RealEngine] ai_senses: avoidance %s: closest pass %.0f cm (radii 68), %u frames steering, ends %.0f / %.0f cm from the goals; AI %.4f ms a frame\n",
           avoid ? "on" : "off", S.closest, S.avoid_frames, arrive[0], arrive[1], S.frames ? S.ai_ms_sum / S.frames : 0.0);
    bool ok = true;
    if (!S.hit || !vknows || S.saw_after < 0.0f || S.saw_after > 2.0f || cknows || ai_stats()->damage_sensed < 1)
        printf("[RealEngine] FAIL: ai_senses: the hit Victim should know and see the Shooter within 2 s; the Control nothing\n"), ok = false;
    if (avoid && S.closest < 0.8f * 68.0f)
        printf("[RealEngine] FAIL: ai_senses: the walkers went through each other (%.0f cm)\n", S.closest), ok = false;
    if (arrive[0] > 150.0f || arrive[1] > 150.0f) printf("[RealEngine] FAIL: ai_senses: a walker didn't arrive\n"), ok = false;
    if (ok) printf("[RealEngine] ai_senses: PASS\n");
    return ok;
}
