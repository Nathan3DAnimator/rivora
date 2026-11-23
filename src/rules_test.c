// Game Rules played (--rules-test, the test rules; notas-dev/regras-de-jogo.md): first rules_selftest, then in a new
// project, away from the template's level (y 6000): BP_GameMode, a Blueprint class with the Game Rules in its class
// defaults (a 1 s countdown, 60 s counting down that lose, 30 points win, 10 a kill, 2 lives, back in 0.5 s at the
// Player Start, 0.5 s invincible) whose graph counts the eight events into variables; a Character (the player) with a
// Health; three enemies tagged Enemy; a Player Start; 300 more tagged actors with a Health (the measure).
// Step by step: the countdown on WBP_MatchHUD, Playing; the clock paused by Set Game Paused; a kill: 10 points on the
// HUD; the player killed: a life less, respawned at the Player Start; the score target: Round Over, Game Over, Victory on
// WBP_MatchEnd; Play Again: the match from the start; the clock set to 1 s: Timer Expired, Defeat. The rules' cost.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "game_rules.h"
#include "game_settings.h"
#include "gameplay_tags.h"
#include "health.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static Entity g_player, g_mode, g_enemy[3], g_start;
static int g_state, g_wait = 2;
static double g_until, g_t0;
static bool g_ok = true, g_done;
static double g_tick_ms, g_tick_max;
static int g_tick_n;
static float g_clock;

static const float Y = 6000.0f; // (away from the template's level)
enum { MEASURE = 1000 }; // (the last step: the frames measured; the steps are below it)

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: rules: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: rules: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: rules: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
// on event ev: Var = Var + 1
static void count_into(BpAsset* a, uint16_t kind, float y, const char* var) {
    BpGraph* g = &a->graphs[0];
    bp_add_var(a, var, BPT_INT);
    uint32_t ev = node(a, kind, 0, y, NULL);
    uint32_t get = node(a, BPK_VAR_GET, 300, y + 60, var), add = node(a, BPK_IADD, 500, y + 60, NULL), set = node(a, BPK_VAR_SET, 700, y, var);
    BpNode* an = bp_node(g, add);
    if (an) an->pins[1].lit.v[0] = 1.0f;
    link(g, ev, "", set, ""), link(g, get, var, add, an ? an->pins[0].name : "A"), link(g, add, an ? an->pins[2].name : "", set, var);
}
static int var_num(World* w, Entity e, const char* name) {
    BpValue v;
    return bp_instance_var(w, e, name, &v) ? (int)v.v[0] : -12345;
}
static const char* text_of(const char* widget) {
    const char* t = ui_test_text(widget);
    return t ? t : "(none)";
}

static Entity tagged(World* w, float x, float y, const char* name) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_STATIC_MESH));
    if (!e) return e;
    if (name) snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 50.0f, t->scale[0] = t->scale[1] = t->scale[2] = 0.5f;
    t->flags = MOBILITY_MOVABLE;
    ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh = SHAPE_CUBE;
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    ecs_component_defaults(COMP_HEALTH, h);
    h->max_health = h->health = 10.0f;
    gt_add(w, e, "Enemy");
    return e;
}

static void wait_s(Editor* ed, double s) { g_until = ed->play_time + s; }

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= MEASURE) { // the measure: the frame's rules with 303 tagged actors (the elimination's walk on)
        g_tick_ms += rules_stats()->tick_ms, g_tick_n++;
        if (rules_stats()->tick_ms > g_tick_max) g_tick_max = rules_stats()->tick_ms;
        return;
    }
    if (g_wait-- > 0 || ed->play_time < g_until) return;
    if (ed->play_time > 40.0) {
        CHECK(false, "stuck at step %d", g_state);
        g_state = MEASURE;
        return;
    }
    float left = 0.0f;
    int ph = rules_phase(&left);
    switch (g_state) {
    case 0: // the Game Mode's rules found: the countdown on the HUD
        bp_set_player(w, g_player);
        if (!rules_active() && ed->play_time < 2.0) return;
        CHECK(rules_active(), "a Game Rules found (BP_GameMode)");
        CHECK(ph == MATCH_COUNTDOWN, "the countdown first (phase %d)", ph);
        CHECK(ui_test_shown("CountdownText") && !strcmp(text_of("CountdownText"), "1"), "WBP_MatchHUD shows the countdown \"%s\"", text_of("CountdownText"));
        CHECK(var_num(w, g_mode, "Phases") >= 2, "On Match Phase Changed: Waiting, Countdown (%d)", var_num(w, g_mode, "Phases"));
        g_state = 1, g_wait = 2;
        break;
    case 1: // Playing after it (the HUD's Tick a frame later); the clock counting down from 1:00
        if (ph != MATCH_PLAYING) return;
        g_state = 11, g_wait = 2;
        break;
    case 11:
        CHECK(var_num(w, g_mode, "RoundsStarted") == 1, "On Round Started (%d)", var_num(w, g_mode, "RoundsStarted"));
        CHECK(!ui_test_shown("CountdownText"), "the countdown hidden while playing");
        CHECK(!strcmp(text_of("TimeText"), "1:00") || !strcmp(text_of("TimeText"), "0:59"), "the clock \"%s\" (1:00)", text_of("TimeText"));
        g_clock = rules_time_remaining(NULL);
        game_set_paused(true);
        g_t0 = re_time_seconds();
        g_state = 2;
        break;
    case 2: // paused 0.5 s (the wall's clock: the play's waits too): the clock waited
        if (re_time_seconds() - g_t0 < 0.5) return;
        CHECK(fabsf(rules_time_remaining(NULL) - g_clock) < 0.001f, "paused 0.5 s: the clock waited (%.3f, %.3f)", rules_time_remaining(NULL), g_clock);
        game_set_paused(false);
        health_apply_damage(w, g_enemy[0], 50, NULL, g_player, g_player); // a kill: 10 points
        g_state = 3, g_wait = 3;
        break;
    case 3:
        CHECK(rules_score(w, g_player) == 10 && !strcmp(text_of("ScoreValue"), "10"), "a kill: 10 points (%d, the HUD \"%s\")", rules_score(w, g_player), text_of("ScoreValue"));
        CHECK(var_num(w, g_mode, "Scores") == 1, "On Score Changed (%d)", var_num(w, g_mode, "Scores"));
        health_apply_damage(w, g_player, 500, NULL, g_enemy[1], g_enemy[1]); // the player killed
        g_state = 4, g_wait = 2;
        break;
    case 4:
        CHECK(health_is_dead(w, g_player) && rules_lives(w, g_player, NULL) == 1 && !strcmp(text_of("LivesValue"), "1"), "dead: 1 life left (HUD \"%s\")", text_of("LivesValue"));
        g_state = 5;
        break;
    case 5: { // respawned at the Player Start (300 cm ahead, facing 45 degrees), invincible a moment (waited for: a loaded
              // machine's frames are long and the play's step is capped, the rules' clock lags the editor's)
        if (health_is_dead(w, g_player)) return;
        const CTransform* t = ecs_get(w, g_player, COMP_TRANSFORM);
        CHECK(!health_is_dead(w, g_player) && t && fabsf(t->location[0] - 300.0f) < 1.0f && fabsf(t->location[1] - Y) < 1.0f, "respawned at the Player Start (%.0f, %.0f)",
              t ? t->location[0] : 0, t ? t->location[1] - Y : 0);
        CHECK(var_num(w, g_mode, "Respawns") == 1, "On Player Respawned (%d)", var_num(w, g_mode, "Respawns"));
        health_apply_damage(w, g_enemy[1], 50, NULL, g_player, g_player);
        health_apply_damage(w, g_enemy[2], 50, NULL, g_player, g_player); // 30: the target
        g_state = 6, g_wait = 2;
        break;
    }
    case 6:
        if (ph == MATCH_PLAYING) return;
        CHECK(ph == MATCH_ROUND_OVER && rules_score(w, g_player) == 30, "the target: Round Over (phase %d, %d points)", ph, rules_score(w, g_player));
        CHECK(ui_test_shown("StatusText"), "the HUD shows the round's end (\"%s\")", text_of("StatusText"));
        g_state = 70;
        break;
    case 70: // Game Over (WBP_MatchEnd's Tick a frame later)
        if (ph != MATCH_GAME_OVER) return;
        g_state = 7, g_wait = 2;
        break;
    case 7: { // Game Over: Victory on WBP_MatchEnd
        bool won = false;
        char why[64];
        CHECK(rules_result(&won, why, sizeof why, NULL) && won && ph == MATCH_GAME_OVER, "won: Game Over (%d, \"%s\")", ph, why);
        CHECK(var_num(w, g_mode, "Won") == 1 && var_num(w, g_mode, "Lost") == 0, "On Match Won (%d, lost %d)", var_num(w, g_mode, "Won"), var_num(w, g_mode, "Lost"));
        CHECK(rules_widget(true) && ui_test_shown("VictoryText") && !ui_test_shown("DefeatText"), "WBP_MatchEnd shows Victory");
        CHECK(!strcmp(text_of("ReasonText"), "Score Target Reached") && !strcmp(text_of("ScoreValue"), "30"), "the end's reason \"%s\"", text_of("ReasonText"));
        CHECK(ui_mouse_shown(), "the cursor shown for the buttons");
        CHECK(bp_call_event(w, rules_widget(true), "OnClicked_PlayAgainButton"), "Play Again clicked");
        g_state = 8, g_wait = 3;
        break;
    }
    case 8: // the match again: 0 points, 2 lives, the end screen gone
        CHECK(rules_score(w, g_player) == 0 && rules_lives(w, g_player, NULL) == 2 && !rules_widget(true), "Play Again: the match from the start (%d, %d)",
              rules_score(w, g_player), rules_lives(w, g_player, NULL));
        g_state = 12;
        break;
    case 12:
        if (ph != MATCH_PLAYING) return;
        rules_set_timer(w, 1.0f, false); // (Set Match Timer: 1 s left)
        g_state = 9;
        break;
    case 9:
        if (ph == MATCH_PLAYING && var_num(w, g_mode, "Timers") < 1) return;
        CHECK(var_num(w, g_mode, "Timers") == 1, "On Timer Expired (%d)", var_num(w, g_mode, "Timers"));
        g_state = 100;
        break;
    case 100:
        if (ph != MATCH_GAME_OVER) return;
        g_state = 10, g_wait = 2;
        break;
    case 10: { // lost by the time: Defeat
        bool won = true;
        char why[64];
        CHECK(rules_result(&won, why, sizeof why, NULL) && !won && !strcmp(why, "Time Up"), "lost by time (\"%s\")", why);
        CHECK(var_num(w, g_mode, "Lost") == 1 && ui_test_shown("DefeatText") && !ui_test_shown("VictoryText"), "On Match Lost and Defeat shown (%d)", var_num(w, g_mode, "Lost"));
        CHECK(var_num(w, g_mode, "RoundsStarted") == 2, "two rounds started (%d)", var_num(w, g_mode, "RoundsStarted"));
        // the measure: the match again, playing with the elimination's walk over the 303 tagged actors, no clock
        CGameRules* r = ecs_get(w, g_mode, COMP_GAME_RULES);
        if (r) r->flags |= GR_WIN_ELIMINATE, r->countdown = 0.0f, r->time_limit = 0.0f;
        CHECK(r && rules_restart(w, false), "Restart Match for the measure");
        g_state = MEASURE;
        break;
    }
    default: g_state = MEASURE; break;
    }
}

bool rules_test_setup(Editor* ed) {
    if (rules_selftest()) {
        printf("[RealEngine] FAIL: rules: the self-test failed\n");
        return false;
    }
    World* w = &ed->world;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/RulesTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    if (!rules_hud_asset() || !rules_end_asset()) {
        printf("[RealEngine] FAIL: rules: the widgets (WBP_MatchHUD, WBP_MatchEnd)\n");
        return false;
    }
    // BP_GameMode: the Game Rules in its class defaults, its events counted
    BpAsset* gm = bp_create(BPA_CLASS, "/Content/RulesTest", "BP_GameMode", "Actor");
    if (!gm) return false;
    gm->comp_mask |= COMP_BIT(COMP_GAME_RULES);
    CGameRules* r = (CGameRules*)gm->comp_data[COMP_GAME_RULES];
    memset(r, 0, sizeof *r);
    ecs_component_defaults(COMP_GAME_RULES, r);
    r->flags &= ~GR_WIN_ELIMINATE;
    r->wait_time = 0.0f, r->countdown = 1.0f, r->time_limit = 60.0f, r->score_target = 30, r->kill_points = 10, r->lives = 2;
    r->respawn_delay = 0.5f, r->invincible_time = 0.5f, r->round_over_time = 0.5f;
    BpGraph* g = &gm->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    count_into(gm, BPK_EVENT_MATCH_PHASE, 0, "Phases");
    count_into(gm, BPK_EVENT_SCORE_CHANGED, 250, "Scores");
    count_into(gm, BPK_EVENT_MATCH_WON, 500, "Won");
    count_into(gm, BPK_EVENT_MATCH_LOST, 750, "Lost");
    count_into(gm, BPK_EVENT_ROUND_STARTED, 1000, "RoundsStarted");
    count_into(gm, BPK_EVENT_PLAYER_RESPAWNED, 1250, "Respawns");
    count_into(gm, BPK_EVENT_TIMER_EXPIRED, 1500, "Timers");
    if (!g_ok || !bp_save(gm) || !bp_compile(gm)) {
        printf("[RealEngine] FAIL: rules: BP_GameMode does not compile: %s\n", gm->error);
        return false;
    }
    g_mode = bp_spawn(w, gm, (float[3]){ 0, Y, 300 }, (float[3]){ 0, 0, 0 });
    // the floor, the player (first person, no gravity), the Player Start, the enemies, 300 more tagged actors
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, Y, 0 }, 0.0);
    CTransform* ft = floor ? ecs_get(w, floor, COMP_TRANSFORM) : NULL;
    if (ft) ft->scale[0] = 40.0f, ft->scale[1] = 40.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    g_player = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_HEALTH));
    if (!g_player) return false;
    CTransform* pt = ecs_get(w, g_player, COMP_TRANSFORM);
    pt->location[0] = 0, pt->location[1] = Y, pt->location[2] = 100.0f, pt->flags = MOBILITY_MOVABLE;
    CCharacter* cc = ecs_get(w, g_player, COMP_CHARACTER);
    cc->max_walk_speed = 300.0f, cc->gravity_scale = 0.0f, cc->radius = 34.0f, cc->half_height = 88.0f, cc->camera_distance = 0.0f, cc->auto_possess = 1;
    ecs_component_defaults(COMP_HEALTH, ecs_get(w, g_player, COMP_HEALTH));
    snprintf(((CName*)ecs_get(w, g_player, COMP_NAME))->text, RE_NAME_MAX, "RulesPlayer");
    g_start = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PLAYER_START));
    if (!g_start) return false;
    CTransform* st = ecs_get(w, g_start, COMP_TRANSFORM);
    st->location[0] = 300.0f, st->location[1] = Y, st->location[2] = 100.0f, st->rotation[1] = 45.0f, st->scale[0] = st->scale[1] = st->scale[2] = 1.0f;
    for (int i = 0; i < 3; i++) g_enemy[i] = tagged(w, 600.0f, Y - 200.0f + 200.0f * (float)i, NULL);
    for (int i = 0; i < 300; i++) tagged(w, -1500.0f + (float)(i % 20) * 150.0f, Y + 1200.0f + (float)(i / 20) * 150.0f, NULL);
    if (!g_mode || !g_enemy[0] || !g_enemy[1] || !g_enemy[2]) {
        printf("[RealEngine] FAIL: rules: the actors\n");
        return false;
    }
    ed->cam.pos[0] = -400.0f, ed->cam.pos[1] = Y - 700.0f, ed->cam.pos[2] = 400.0f;
    ed->cam.yaw = 50.0f, ed->cam.pitch = -25.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

bool rules_test_check(Editor* ed) {
    if (g_state < MEASURE) printf("[RealEngine] FAIL: rules: stopped at step %d\n", g_state), g_ok = false;
    const RulesStats* st = rules_stats();
    double tick = g_tick_n ? g_tick_ms / g_tick_n : 0.0;
    printf("[RealEngine] Game Rules cost: %.4f ms a frame, the worst %.4f (%u players, %u tagged actors; %u kills, %u deaths, %u respawns, %u phase changes)\n", tick, g_tick_max, st->players,
           st->tagged, st->kills, st->deaths, st->respawns, st->phase_changes);
    CHECK(g_tick_n > 0, "the measure ran no frame");
    CHECK(tick < 0.05, "the rules cost %.4f ms a frame (0.05 at most)", tick);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] Game Rules test: OK\n");
    return g_ok;
}
