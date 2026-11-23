// The demos' game modes (demo_play.h): the dispatch and what they share
#include "demo_play.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "anim.h"
#include "blueprint.h"
#include "components.h"
#include "platform.h"
#include "ui_widget.h"

#define DP_DEG 0.017453292519943295f

enum { DP_NONE = 0, DP_FIGHTING, DP_HORROR, DP_RPG };
static int g_mode;
static char g_report[512];

// --- Shared ---

static uint32_t dp_rng = 0x9E3779B9u;
static float dp_rand(void) { // [0, 1)
    dp_rng ^= dp_rng << 13, dp_rng ^= dp_rng >> 17, dp_rng ^= dp_rng << 5;
    return (float)(dp_rng >> 8) * (1.0f / 16777216.0f);
}

static Entity dp_find(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || !a->count) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ents[i];
    }
    return ENTITY_NONE;
}

// edges of the keys (GLFW codes; 500/501 the mouse buttons, as the Blueprints name them)
static uint8_t dp_prev[512];
static bool dp_pressed(int key) {
    bool now = bp_key_held(key), was = dp_prev[key] != 0;
    dp_prev[key] = now;
    return now && !was;
}

static Entity dp_hud(World* w, const char* cls) {
    BpAsset* c = (BpAsset*)bp_find_name(BPA_CLASS, cls);
    Entity e = c ? ui_create_widget(w, c) : ENTITY_NONE;
    if (e) ui_add_to_viewport(e, 0);
    return e;
}

#include "demo_fight.inc"
#include "terror.h" // (Q13: the horror kit)
#include "demo_horror.inc"
#include "demo_rpg.inc"

void demo_play_begin(World* w, const char* game_mode) {
    g_mode = DP_NONE;
    g_report[0] = 0;
    memset(dp_prev, 0, sizeof dp_prev);
    if (!game_mode) return;
    if (!strcmp(game_mode, "DemoFighting")) g_mode = DP_FIGHTING, fight_begin(w);
    else if (!strcmp(game_mode, "DemoHorror")) g_mode = DP_HORROR, horror_begin(w);
    else if (!strcmp(game_mode, "DemoRPG")) g_mode = DP_RPG, rpg_begin(w);
}

void demo_play_tick(World* w, float dt) {
    if (g_mode == DP_FIGHTING) fight_tick(w, dt);
    else if (g_mode == DP_HORROR) horror_tick(w, dt);
    else if (g_mode == DP_RPG) rpg_tick(w, dt);
}

void demo_play_end(void) {
    if (g_mode == DP_RPG) rpg_shop_close();
    ui_set_native_events(NULL);
    g_mode = DP_NONE;
}

bool demo_play_view(World* w, float pos[3], float* yaw, float* pitch) {
    return g_mode == DP_FIGHTING && fight_view(w, pos, yaw, pitch);
}

const char* demo_play_report(World* w) {
    if (g_mode == DP_FIGHTING) fight_report(w, g_report, sizeof g_report);
    else if (g_mode == DP_HORROR) horror_report(w, g_report, sizeof g_report);
    else if (g_mode == DP_RPG) rpg_report(w, g_report, sizeof g_report);
    else g_report[0] = 0;
    return g_report;
}

bool demo_play_check(World* w) {
    (void)w;
    if (g_mode == DP_FIGHTING) {
        // (a blocked blow landed too: 15% of its damage. Two minds that block well traded only blocked blows in some runs: the
        // test failed with both fighters hurt)
        bool ok = FG.f[0].hits + FG.f[1].hits + FG.f[0].blocked + FG.f[1].blocked > 0 && FG.montages > 0 && FG.ai_attacks > 0;
        if (!ok) printf("[RealEngine] FAIL: the fight: no blow landed or no montage played\n");
        return ok;
    }
    if (g_mode == DP_HORROR) return horror_check(w);
    if (g_mode == DP_RPG) return rpg_check(w);
    return true;
}
