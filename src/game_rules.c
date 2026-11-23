// Game Rules (game_rules.h, notas-dev/regras-de-jogo.md; R7 módulo 8): the match's phases, rounds, scores, win and lose,
// lives and respawn, the clock. One match a play: the first Game Rules component found (a level's actor, or the Game
// Mode's class spawned after BeginPlay: looked for each frame until one is there). The server (or a game offline)
// decides; a client reads the server's state (rules_net_read) and fires the same events from what changed.
#include "game_rules.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "debug_viewer.h"
#include "game_settings.h"
#include "gameplay_tags.h"
#include "health.h"
#include "level.h"
#include "online.h"
#include "platform.h"
#include "save_progress.h"
#include "ui_widget.h"

_Static_assert(sizeof(CGameRules) == 128, "CGameRules must fit a Blueprint class's component defaults (128 bytes)");

const char* const g_match_phase_names[MATCH_PHASE_COUNT] = { "Waiting", "Countdown", "Playing", "Round Over", "Game Over" };
const char* const g_rules_time_up_names[RULES_TIME_UP_COUNT] = { "Nothing", "Defeat", "Victory", "Most Points Win" };

enum { EV_PHASE = 0, EV_SCORE, EV_WON, EV_LOST, EV_ROUND_START, EV_ROUND_END, EV_RESPAWNED, EV_TIMER, EV_COUNT };
static const uint16_t g_ev_kind[EV_COUNT] = { BPK_EVENT_MATCH_PHASE, BPK_EVENT_SCORE_CHANGED, BPK_EVENT_MATCH_WON, BPK_EVENT_MATCH_LOST,
                                              BPK_EVENT_ROUND_STARTED, BPK_EVENT_ROUND_ENDED, BPK_EVENT_PLAYER_RESPAWNED, BPK_EVENT_TIMER_EXPIRED };

#define REASON 64
static struct {
    bool found;               // a Game Rules runs
    bool test;                // the self-test: no VM, no widgets
    Entity owner;             // its actor
    CGameRules cfg;           // its configuration (read again each frame: the Details and Blueprints may change it)
    int phase;
    float phase_t;            // seconds in the phase
    int round;                // 1..
    float clock, limit;       // the round's seconds; the limit (the Time Limit, or Set Match Timer's)
    bool clock_stopped;       // Set Match Timer's
    bool expired;             // the limit reached this round (On Timer Expired once)
    RulesSlot slot[RULES_MAX_PLAYERS];
    uint32_t nslots;
    int local;                // the local player's slot (-1: none yet)
    uint8_t human[RULES_MAX_PLAYERS];
    int32_t team_score[RULES_MAX_TEAMS];
    int team_rounds[RULES_MAX_TEAMS]; // rounds won by each team (no teams: by the slot's index, the first 4)
    int lost_rounds;          // rounds nobody of the players won
    // the round's / the match's result
    int win_team, win_slot;   // -1: none
    bool decided;             // this round has its result
    bool over;                // Game Over
    char reason[REASON];
    // elimination
    bool elim_dirty;
    float elim_t;
    uint32_t elim_max;        // the most Eliminate Tag actors seen alive this round
    // the widgets
    Entity hud, end;
    bool mouse_before;
    // a load's state waiting for the match (save_progress.c reads it before BeginPlay)
    uint8_t pending[512];
    uint32_t pending_n;
    // online
    bool client;
    uint32_t serial;
    Entity test_player;       // the self-test's local player
    uint32_t evn[EV_COUNT];   // the events fired (the tests)
    RulesStats st;
} R;

// --- Helpers ---
static Entity local_player(World* w) { return R.test ? R.test_player : bp_player(w); }
static bool authority(void) { return !R.client; }
static void touch(void) { R.serial++; }

static void ent_val(BpValue* v, Entity e) { memcpy(&v->v[0], &e, 4); } // (an actor value: the entity's bits, as the VM's set_ent)
static void fire(World* w, int ev, const BpValue* args, uint32_t n) {
    R.evn[ev]++;
    if (!R.test) bp_fire_event(w, g_ev_kind[ev], args, n);
}
static void str_val(BpValue* v, const char* s) {
    if (!R.test) bp_make_string(v, s);
}

static int slot_index(World* w, Entity e) {
    (void)w;
    if (!e) return -1;
    for (uint32_t i = 0; i < R.nslots; i++)
        if (R.slot[i].e == e) return (int)i;
    return -1;
}
static int team_for(int i, bool human) {
    int t = R.cfg.teams;
    if (t >= 2) return i % (t > RULES_MAX_TEAMS ? RULES_MAX_TEAMS : t);
    return t == 1 && human ? 0 : -1; // (co-op: the players one team; a bot that scores plays alone)
}
static int add_slot(World* w, Entity e, bool human) {
    if (!e || !ecs_alive(w, e) || R.nslots >= RULES_MAX_PLAYERS) return -1;
    int i = (int)R.nslots++;
    RulesSlot* s = &R.slot[i];
    memset(s, 0, sizeof *s);
    s->e = e, s->net = -1, s->lives = R.cfg.lives;
    R.human[i] = human;
    s->team = (int8_t)team_for(i, human);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) memcpy(s->start, t->location, 12), s->start_yaw = t->rotation[1];
    const CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
    s->cls = b ? b->asset_hash : 0;
    if (online_active())
        for (int p = 0; p < ONLINE_MAX_PLAYERS; p++)
            if (online_player_pawn(w, p) == e) s->net = (int8_t)p;
    touch();
    return i;
}
static int slot_of(World* w, Entity e, bool create) {
    int i = slot_index(w, e);
    if (i < 0 && create) i = add_slot(w, e, false);
    return i;
}
static bool is_local(int i) { return i >= 0 && i == R.local; }
static int local_team(void) { return R.local >= 0 ? R.slot[R.local].team : (R.cfg.teams == 1 ? 0 : -1); }
// the result for the local player: its slot or its team won
static bool local_won(void) {
    if (R.win_slot >= 0) return is_local(R.win_slot) || (R.win_team >= 0 && R.win_team == local_team());
    return R.win_team >= 0 && R.win_team == local_team();
}

// --- The phases ---
static void show_end(World* w);
static void enter_phase(World* w, int p, bool from_net);
static void new_round_state(World* w, bool reset_all);

static void set_phase(World* w, int p) {
    if (p < 0 || p >= MATCH_PHASE_COUNT) return;
    enter_phase(w, p, false);
}

// What a phase starting does here (the server's choice, or what a client heard): the events, the widgets
static void enter_phase(World* w, int p, bool from_net) {
    int old = R.phase;
    if (old == p && !from_net) return;
    R.phase = p, R.phase_t = 0.0f;
    R.st.phase_changes++;
    touch();
    if (!R.test) printf("[RealEngine] Game Rules: %s (round %d%s%s)\n", g_match_phase_names[p], R.round, R.reason[0] ? ": " : "", R.reason);
    BpValue a[3];
    memset(a, 0, sizeof a);
    a[0].v[0] = (float)p, a[1].v[0] = (float)old;
    str_val(&a[2], g_match_phase_names[p]);
    fire(w, EV_PHASE, a, 3);
    if (p == MATCH_PLAYING && old != MATCH_PLAYING) { // a round begins
        memset(a, 0, sizeof a);
        a[0].v[0] = (float)R.round;
        fire(w, EV_ROUND_START, a, 1);
    }
    if (p == MATCH_ROUND_OVER || (p == MATCH_GAME_OVER && old == MATCH_PLAYING)) {
        memset(a, 0, sizeof a);
        a[0].v[0] = (float)R.round, a[1].v[0] = local_won() ? 1.0f : 0.0f;
        str_val(&a[2], R.reason);
        fire(w, EV_ROUND_END, a, 3);
    }
    if (p == MATCH_GAME_OVER) {
        R.over = true;
        bool won = local_won();
        memset(a, 0, sizeof a);
        Entity winner = R.win_slot >= 0 ? R.slot[R.win_slot].e : ENTITY_NONE;
        ent_val(&a[0], winner);
        str_val(&a[1], R.reason);
        fire(w, won ? EV_WON : EV_LOST, a, 2);
        show_end(w);
    }
}

// The round decided (the server's): who won and why; Round Over next (or Game Over with no Round Over time and one round)
static void decide(World* w, int team, int slot, const char* reason) {
    if (R.decided || R.over || R.phase != MATCH_PLAYING) return;
    R.decided = true;
    R.win_team = team, R.win_slot = slot;
    snprintf(R.reason, sizeof R.reason, "%s", reason ? reason : "");
    int t = team >= 0 ? team : slot >= 0 && slot < RULES_MAX_TEAMS && R.cfg.teams == 0 ? slot : -1;
    if (t >= 0 && t < RULES_MAX_TEAMS) R.team_rounds[t]++;
    else R.lost_rounds++;
    set_phase(w, MATCH_ROUND_OVER);
}

// After Round Over: the match won (more than half the rounds), lost, or the next round
static void after_round(World* w) {
    int rounds = R.cfg.rounds > 0 ? R.cfg.rounds : 1;
    int need = rounds / 2 + 1, best = -1, best_n = 0;
    for (int t = 0; t < RULES_MAX_TEAMS; t++)
        if (R.team_rounds[t] > best_n) best = t, best_n = R.team_rounds[t];
    bool done = rounds <= 1 || best_n >= need || R.lost_rounds >= need || R.round >= rounds;
    if (!done) {
        R.round++;
        new_round_state(w, false);
        set_phase(w, R.cfg.countdown > 0.0f ? MATCH_COUNTDOWN : MATCH_PLAYING);
        return;
    }
    if (rounds > 1) { // the match's winner: the most rounds (the last round's reason kept)
        if (best_n > R.lost_rounds && best >= 0) {
            R.win_team = R.cfg.teams >= 1 ? best : -1;
            R.win_slot = R.cfg.teams == 0 ? best : -1;
        } else
            R.win_team = R.win_slot = -1;
    }
    set_phase(w, MATCH_GAME_OVER);
}

// --- Respawn ---
// The Player Start nearest to where the player began (each player back on its own side); false: none in the level
static bool player_start(World* w, const float near[3], float pos[3], float* yaw) {
    float best = 3.4e38f;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_PLAYER_START) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t k = 0; k < a->count; k++) {
            float dx = t[k].location[0] - near[0], dy = t[k].location[1] - near[1], dz = t[k].location[2] - near[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < best) best = d, memcpy(pos, t[k].location, 12), *yaw = t[k].rotation[1];
        }
    }
    return best < 3.4e38f;
}

static bool respawn_slot(World* w, int i) {
    RulesSlot* s = &R.slot[i];
    Entity e = s->e;
    if (!ecs_alive(w, e)) { // (destroyed by its Health's Destroy: a new one of its class, possessed again when it was the player)
        BpAsset* cls = s->cls ? bp_find_hash(s->cls) : NULL;
        e = cls && !R.test ? bp_spawn(w, cls, s->start, (float[3]){ 0, s->start_yaw, 0 }) : ENTITY_NONE;
        if (!e) return false;
        if (is_local(i)) bp_set_player(w, e);
        s->e = e;
    }
    bool moved = false;
    if ((R.cfg.flags & GR_AT_CHECKPOINT) && progress_checkpoint(w) != ENTITY_NONE) moved = progress_respawn(w, e, false);
    if (!moved) {
        float pos[3], yaw;
        if (!player_start(w, s->start, pos, &yaw)) memcpy(pos, s->start, 12), yaw = s->start_yaw;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (t) memcpy(t->location, pos, 12), t->rotation[1] = yaw;
        CCharacter* ch = ecs_get(w, e, COMP_CHARACTER);
        if (ch) ch->control_yaw = yaw, memset(ch->velocity, 0, sizeof ch->velocity);
        ecs_touch(w, e);
        if (ecs_get(w, e, COMP_HEALTH)) health_revive(w, e, 0.0f);
    }
    s->respawn_t = 0.0f;
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    if (h && R.cfg.invincible_time > 0.0f) {
        if (s->invincible_t <= 0.0f) s->health_inv = (h->flags & HEALTH_INVINCIBLE) != 0;
        health_set_invincible(w, e, true);
        s->invincible_t = R.cfg.invincible_time;
    }
    R.st.respawns++;
    touch();
    BpValue a[2];
    memset(a, 0, sizeof a);
    ent_val(&a[0], e);
    a[1].v[0] = (float)s->lives;
    fire(w, EV_RESPAWNED, a, 2);
    return true;
}

// A new round (or the match again): the clock, the result, the elimination; the players back at their starts
static void new_round_state(World* w, bool reset_all) {
    R.clock = 0.0f, R.limit = R.cfg.time_limit, R.clock_stopped = false, R.expired = false;
    R.decided = false, R.win_team = R.win_slot = -1, R.reason[0] = 0;
    R.elim_max = 0, R.elim_dirty = true, R.elim_t = 0.0f;
    if (reset_all) {
        R.round = 1, R.over = false, R.lost_rounds = 0;
        memset(R.team_rounds, 0, sizeof R.team_rounds);
    }
    bool reset_score = reset_all || (R.cfg.flags & GR_RESET_SCORE);
    if (reset_score) memset(R.team_score, 0, sizeof R.team_score);
    for (uint32_t i = 0; i < R.nslots; i++) {
        RulesSlot* s = &R.slot[i];
        if (reset_score) s->score = s->kills = s->deaths = 0;
        s->lives = R.cfg.lives, s->out = 0;
        if (R.human[i] && (R.round > 1 || reset_all) && ecs_alive(w, s->e)) respawn_slot(w, (int)i);
        else if (R.human[i] && !ecs_alive(w, s->e) && s->cls) respawn_slot(w, (int)i);
    }
    touch();
}

// --- Win and lose ---
static void check_score(World* w, int i) {
    if (!(R.cfg.flags & GR_WIN_SCORE) || R.cfg.score_target <= 0 || R.phase != MATCH_PLAYING) return;
    const RulesSlot* s = &R.slot[i];
    if (R.cfg.teams >= 1 && s->team >= 0) {
        if (R.team_score[s->team] >= R.cfg.score_target) decide(w, s->team, -1, "Score Target Reached"); // (a team reached it)
    } else if (s->score >= R.cfg.score_target)
        decide(w, s->team, i, "Score Target Reached");
}

// Who is still in: none of the players -> lost; one player / one team left among several -> it wins
static void check_standing(World* w) {
    if (R.phase != MATCH_PLAYING) return;
    int humans = 0, in = 0, last = -1, teams_in = 0, last_team = -1;
    bool team_in[RULES_MAX_TEAMS] = { false };
    for (uint32_t i = 0; i < R.nslots; i++) {
        if (!R.human[i]) continue;
        humans++;
        if (R.slot[i].out) continue;
        in++, last = (int)i;
        int t = R.slot[i].team;
        if (t >= 0 && t < RULES_MAX_TEAMS && !team_in[t]) team_in[t] = true, teams_in++, last_team = t;
    }
    if (!humans) return;
    const char* why = (R.cfg.flags & GR_LOSE_LIVES) && R.cfg.lives > 0 ? "Out of Lives" : "Too Many Deaths";
    if (!in) decide(w, -1, -1, why);
    else if (humans > 1 && R.cfg.teams >= 2 && teams_in == 1) decide(w, last_team, -1, "Last Team Standing");
    else if (humans > 1 && R.cfg.teams == 0 && in == 1) decide(w, -1, last, "Last One Standing");
}

// The players' side (a co-op's team, else the best of the players) wins
static void players_win(World* w, const char* why) {
    if (R.cfg.teams >= 1) {
        decide(w, R.local >= 0 && R.slot[R.local].team >= 0 ? R.slot[R.local].team : 0, -1, why);
        return;
    }
    int best = -1;
    for (uint32_t i = 0; i < R.nslots; i++)
        if (R.human[i] && (best < 0 || R.slot[i].score > R.slot[best].score)) best = (int)i;
    decide(w, -1, best, why);
}

static void time_up(World* w) {
    switch (R.cfg.time_up) {
    case RULES_TIME_UP_LOSE: decide(w, -1, -1, "Time Up"); break;
    case RULES_TIME_UP_WIN: players_win(w, "Survived"); break;
    case RULES_TIME_UP_SCORE: {
        if (R.cfg.teams >= 2) {
            int best = -1, ties = 0;
            for (int t = 0; t < R.cfg.teams && t < RULES_MAX_TEAMS; t++) {
                if (best < 0 || R.team_score[t] > R.team_score[best]) best = t, ties = 0;
                else if (R.team_score[t] == R.team_score[best]) ties++;
            }
            if (ties) decide(w, -1, -1, "Draw");
            else decide(w, best, -1, "Most Points");
        } else {
            int best = -1, ties = 0;
            for (uint32_t i = 0; i < R.nslots; i++) {
                if (best < 0 || R.slot[i].score > R.slot[best].score) best = (int)i, ties = 0;
                else if (R.slot[i].score == R.slot[best].score) ties++;
            }
            if (best < 0 || ties) decide(w, -1, -1, "Draw");
            else if (R.cfg.teams == 1 && R.human[best]) decide(w, 0, -1, "Most Points");
            else decide(w, R.slot[best].team, best, "Most Points");
        }
        break;
    }
    default: break;
    }
}

// The actors with the Eliminate Tag alive (Health not dead; no Health: alive while it exists)
static uint32_t tagged_alive(World* w, uint32_t* seen) {
    uint32_t alive = 0, n = 0;
    const char* tag = R.cfg.eliminate_tag;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)) || !a->count) continue;
        const CGameplayTags* gt = ecs_arch_column(a, COMP_GAMEPLAY_TAGS);
        const CHealth* h = (a->mask & COMP_BIT(COMP_HEALTH)) ? ecs_arch_column(a, COMP_HEALTH) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            bool has = false;
            for (uint32_t k = 0; k < gt[i].count && k < GT_MAX_TAGS && !has; k++) has = gt_matches(gt[i].tags[k], tag, false);
            if (!has) continue;
            n++;
            if (!h || !(h[i].flags & HEALTH_DEAD)) alive++;
        }
    }
    *seen = n;
    return alive;
}

static void check_elimination(World* w, float dt) {
    if (!(R.cfg.flags & GR_WIN_ELIMINATE) || !R.cfg.eliminate_tag[0] || R.phase != MATCH_PLAYING) return;
    R.elim_t -= dt;
    if (!R.elim_dirty && R.elim_t > 0.0f) return; // (a death checks now; spawns and destroys every quarter second)
    R.elim_dirty = false, R.elim_t = 0.25f;
    uint32_t seen = 0, alive = tagged_alive(w, &seen);
    R.st.alive_tagged = alive, R.st.tagged = seen;
    if (alive > R.elim_max) R.elim_max = alive;
    if (R.elim_max > 0 && alive == 0) players_win(w, "Enemies Eliminated");
}

// --- Finding the Game Rules and starting the match ---
static void widgets_begin(World* w);

static bool find_rules(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_GAME_RULES)) || !a->count) continue;
        R.owner = ecs_arch_entities(a)[0];
        memcpy(&R.cfg, ecs_arch_column(a, COMP_GAME_RULES), sizeof R.cfg);
        return true;
    }
    return false;
}

static void apply_pending(World* w);

// The component's settings now (the Details and Blueprints may change them while playing); its actor gone: another
// Game Rules, else the last settings go on
static void refresh_cfg(World* w) {
    const CGameRules* c = ecs_alive(w, R.owner) ? ecs_get(w, R.owner, COMP_GAME_RULES) : NULL;
    if (c) memcpy(&R.cfg, c, sizeof R.cfg);
    else if (!find_rules(w)) R.owner = ENTITY_NONE;
}

static void start(World* w) {
    R.found = true;
    R.client = online_role() == ONLINE_CLIENT;
    R.phase = MATCH_WAITING, R.phase_t = 0.0f;
    R.nslots = 0, R.local = -1;
    new_round_state(w, true);
    Entity p = local_player(w);
    if (p) R.local = add_slot(w, p, true);
    if (R.pending_n) apply_pending(w);
    widgets_begin(w);
    BpValue a[3];
    memset(a, 0, sizeof a);
    a[0].v[0] = (float)R.phase, a[1].v[0] = (float)R.phase;
    str_val(&a[2], g_match_phase_names[R.phase]);
    fire(w, EV_PHASE, a, 3); // (Waiting: the match's first phase, heard by every Blueprint)
    touch();
}

void rules_begin_play(World* w) {
    (void)w;
    bool test = R.test;
    Entity tp = R.test_player;
    uint8_t keep[sizeof R.pending];
    uint32_t keep_n = R.pending_n;
    memcpy(keep, R.pending, keep_n);
    memset(&R, 0, sizeof R);
    R.test = test, R.test_player = tp;
    memcpy(R.pending, keep, keep_n), R.pending_n = keep_n; // (a load read before: Load Progress opens the level, then this)
    R.local = -1, R.win_team = R.win_slot = -1;
}

void rules_end_play(World* w) {
    if (R.end && !R.test) ui_show_mouse(R.mouse_before);
    for (uint32_t i = 0; i < R.nslots && w; i++) // (a respawn's invincibility given back)
        if (R.slot[i].invincible_t > 0.0f && ecs_alive(w, R.slot[i].e)) health_set_invincible(w, R.slot[i].e, R.slot[i].health_inv);
    R.found = false, R.nslots = 0, R.hud = R.end = ENTITY_NONE, R.pending_n = 0;
}

bool rules_active(void) { return R.found; }

// --- Every frame ---
static void track_players(World* w) {
    Entity p = local_player(w);
    if (p && slot_index(w, p) < 0) {
        if (R.local >= 0 && R.slot[R.local].respawn_t <= 0.0f) R.slot[R.local].e = p, touch(); // (another pawn possessed)
        else if (R.local < 0) R.local = add_slot(w, p, true);
    }
    if (!online_active() || R.client) return;
    for (int n = 0; n < ONLINE_MAX_PLAYERS; n++) { // (the server: every player's pawn has its slot)
        Entity e = online_player_pawn(w, n);
        if (!e) continue;
        int i = slot_index(w, e);
        if (i < 0) i = add_slot(w, e, true);
        if (i >= 0 && R.slot[i].net != n) R.slot[i].net = (int8_t)n, R.human[i] = 1, touch();
    }
}

void rules_tick(World* w, float dt) {
    if (!w) return;
    double t0 = re_time_seconds();
    if (!R.found) {
        if (!find_rules(w)) return;
        start(w);
    }
    refresh_cfg(w);
    if (dt < 0.0f) dt = 0.0f;
    bool paused = game_paused();
    track_players(w);
    if (!paused) {
        R.phase_t += dt;
        if (R.phase == MATCH_PLAYING && !R.clock_stopped) R.clock += dt;
    }
    if (paused || R.client) { // (paused: the clock and the phases wait; a client: the server decides, its state comes)
        R.st.players = R.nslots;
        R.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
        return;
    }
    switch (R.phase) {
    case MATCH_WAITING:
        if ((R.cfg.flags & GR_AUTO_START) && R.phase_t >= R.cfg.wait_time) rules_start_match(w);
        break;
    case MATCH_COUNTDOWN:
        if (R.phase_t >= R.cfg.countdown) set_phase(w, MATCH_PLAYING);
        break;
    case MATCH_PLAYING: {
        for (uint32_t i = 0; i < R.nslots; i++) { // the respawns' and the invincibilities' timers
            RulesSlot* s = &R.slot[i];
            if (s->respawn_t > 0.0f && (s->respawn_t -= dt) <= 0.0f && !s->out) {
                if (ecs_alive(w, s->e) && !health_is_dead(w, s->e)) s->respawn_t = 0.0f, touch(); // (a Checkpoint brought it back already)
                else respawn_slot(w, (int)i);
            }
            if (s->invincible_t > 0.0f && (s->invincible_t -= dt) <= 0.0f) {
                if (ecs_alive(w, s->e)) health_set_invincible(w, s->e, s->health_inv);
                touch();
            }
        }
        if (R.limit > 0.0f && R.clock >= R.limit && !R.expired) {
            R.expired = true;
            R.clock = R.limit;
            touch();
            fire(w, EV_TIMER, NULL, 0);
            if (R.phase == MATCH_PLAYING) time_up(w);
        }
        check_elimination(w, dt);
        break;
    }
    case MATCH_ROUND_OVER:
        if (R.phase_t >= R.cfg.round_over_time) after_round(w);
        break;
    default: break;
    }
    R.st.players = R.nslots;
    R.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

// --- Deaths ---
void rules_on_death(World* w, Entity victim, Entity killer, const char* type) {
    (void)type;
    if (!R.found || !w || !authority()) return;
    R.elim_dirty = true;
    if (R.phase != MATCH_PLAYING) return; // (a death before the start or after the end counts for nothing)
    R.st.deaths++;
    if (killer && killer != victim && ecs_alive(w, killer)) { // the killer's points: the victim's tag's, else a kill's
        int32_t pts = R.cfg.kill_points;
        for (int k = 0; k < GR_SCORE_TAGS; k++)
            if (R.cfg.score_tag[k][0] && gt_has(w, victim, R.cfg.score_tag[k], false)) {
                pts = R.cfg.score_tag_points[k];
                break;
            }
        int ki = slot_of(w, killer, true);
        if (ki >= 0) {
            R.slot[ki].kills++, R.st.kills++;
            if (pts) rules_add_score(w, killer, pts);
            else touch();
        }
    }
    int vi = slot_index(w, victim);
    if (vi < 0 || R.phase != MATCH_PLAYING) return;
    RulesSlot* s = &R.slot[vi];
    s->deaths++;
    if (R.cfg.lives > 0 && s->lives > 0) s->lives--;
    bool out = ((R.cfg.flags & GR_LOSE_LIVES) && R.cfg.lives > 0 && s->lives <= 0) ||
               ((R.cfg.flags & GR_LOSE_DEATHS) && R.cfg.max_deaths > 0 && s->deaths >= R.cfg.max_deaths);
    if (out) s->out = 1;
    else if (R.cfg.flags & GR_RESPAWN) s->respawn_t = R.cfg.respawn_delay > 0.001f ? R.cfg.respawn_delay : 0.001f;
    touch();
    if (out) check_standing(w);
}

// --- The nodes ---
int rules_phase(float* phase_left) {
    if (phase_left) {
        float len = R.phase == MATCH_WAITING && (R.cfg.flags & GR_AUTO_START) ? R.cfg.wait_time : R.phase == MATCH_COUNTDOWN ? R.cfg.countdown
                    : R.phase == MATCH_ROUND_OVER ? R.cfg.round_over_time : 0.0f;
        *phase_left = len > R.phase_t ? len - R.phase_t : 0.0f;
    }
    return R.phase;
}
bool rules_set_phase(World* w, int phase) {
    if (!R.found || !authority() || phase < 0 || phase >= MATCH_PHASE_COUNT) return false;
    if (phase == MATCH_PLAYING && R.over) return false; // (Restart Match first)
    if (phase == MATCH_ROUND_OVER && !R.decided) R.decided = true, R.lost_rounds++, snprintf(R.reason, sizeof R.reason, "%s", "Round Ended");
    if (phase == MATCH_PLAYING && R.phase == MATCH_ROUND_OVER) { // (the next round now)
        R.round++;
        new_round_state(w, false);
    }
    set_phase(w, phase);
    return true;
}
bool rules_start_match(World* w) {
    if (!R.found || !authority() || R.phase != MATCH_WAITING) return false;
    set_phase(w, R.cfg.countdown > 0.0f ? MATCH_COUNTDOWN : MATCH_PLAYING);
    return true;
}
bool rules_end_match(World* w, bool won, const char* reason, Entity winner) {
    if (!R.found || !authority() || R.over) return false;
    int ws = winner ? slot_of(w, winner, true) : -1;
    R.win_slot = ws;
    R.win_team = ws >= 0 ? R.slot[ws].team : won ? (local_team() >= 0 ? local_team() : 0) : -1;
    if (ws < 0 && won && R.cfg.teams == 0) R.win_slot = R.local; // (no teams: the local player is the side)
    snprintf(R.reason, sizeof R.reason, "%s", reason && reason[0] ? reason : won ? "Objective Complete" : "Objective Failed");
    R.decided = true;
    set_phase(w, MATCH_GAME_OVER);
    return true;
}
int32_t rules_add_score(World* w, Entity player, int32_t points) {
    if (!R.found || !authority()) return 0;
    Entity e = player ? player : local_player(w);
    int i = slot_of(w, e, true);
    if (i < 0) return 0;
    RulesSlot* s = &R.slot[i];
    s->score += points;
    if (s->team >= 0 && s->team < RULES_MAX_TEAMS) R.team_score[s->team] += points;
    R.st.score_events++;
    touch();
    BpValue a[5];
    memset(a, 0, sizeof a);
    ent_val(&a[0], s->e);
    a[1].v[0] = (float)s->score, a[2].v[0] = (float)points, a[3].v[0] = (float)s->team;
    a[4].v[0] = s->team >= 0 ? (float)R.team_score[s->team] : 0.0f;
    fire(w, EV_SCORE, a, 5);
    check_score(w, i);
    return s->score;
}
int32_t rules_score(World* w, Entity player) {
    int i = slot_index(w, player ? player : local_player(w));
    return i >= 0 ? R.slot[i].score : 0;
}
int32_t rules_team_score(int team) { return team >= 0 && team < RULES_MAX_TEAMS ? R.team_score[team] : 0; }
int rules_team_rounds(int team) { return team >= 0 && team < RULES_MAX_TEAMS ? R.team_rounds[team] : 0; }
bool rules_counts_down(void) { return (R.cfg.flags & GR_TIMER_DOWN) != 0; }
int rules_team_of(World* w, Entity player) {
    int i = slot_index(w, player ? player : local_player(w));
    return i >= 0 ? R.slot[i].team : -1;
}
float rules_time_remaining(float* elapsed) {
    if (elapsed) *elapsed = R.clock;
    return R.limit > 0.0f ? fmaxf(R.limit - R.clock, 0.0f) : 0.0f;
}
bool rules_set_timer(World* w, float seconds, bool paused) {
    (void)w;
    if (!R.found || !authority()) return false;
    if (seconds < 0.0f) seconds = 0.0f;
    if (R.cfg.flags & GR_TIMER_DOWN) R.limit = R.clock + seconds;
    else R.clock = seconds;
    if (R.limit <= 0.0f || R.clock < R.limit) R.expired = false;
    R.clock_stopped = paused;
    touch();
    return true;
}
void rules_format_time(float seconds, char* out, uint32_t cap) {
    if (!out || !cap) return;
    int s = (int)ceilf(seconds > 0.0f ? seconds - 0.0005f : 0.0f);
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}
int32_t rules_lives(World* w, Entity player, bool* infinite) {
    if (infinite) *infinite = R.cfg.lives == 0;
    int i = slot_index(w, player ? player : local_player(w));
    return i >= 0 ? R.slot[i].lives : R.cfg.lives;
}
int32_t rules_add_lives(World* w, Entity player, int32_t lives) {
    if (!R.found || !authority()) return 0;
    int i = slot_of(w, player ? player : local_player(w), false);
    if (i < 0) return 0;
    RulesSlot* s = &R.slot[i];
    s->lives += lives;
    if (s->lives < 0) s->lives = 0;
    if (s->out && s->lives > 0 && !((R.cfg.flags & GR_LOSE_DEATHS) && R.cfg.max_deaths && s->deaths >= R.cfg.max_deaths)) { // (back in the match)
        s->out = 0;
        if (R.phase == MATCH_PLAYING) s->respawn_t = R.cfg.respawn_delay > 0.001f ? R.cfg.respawn_delay : 0.001f;
    }
    touch();
    return s->lives;
}
bool rules_respawn(World* w, Entity player) {
    if (!R.found || !authority()) return false;
    int i = slot_of(w, player ? player : local_player(w), false);
    if (i < 0) return false;
    R.slot[i].out = 0;
    return respawn_slot(w, i);
}
bool rules_restart(World* w, bool reload_level) {
    if (!R.found || !authority()) return false;
    refresh_cfg(w);
    if (R.end && !R.test) ui_remove_from_parent(R.end), ui_show_mouse(R.mouse_before);
    R.end = ENTITY_NONE;
    if (reload_level && !R.test) {
        const char* lv = level_stream_persistent();
        if (lv && lv[0]) {
            level_open_request(lv); // (the level again: its BeginPlay starts the match over)
            return true;
        }
    }
    R.phase = MATCH_WAITING; // (straight back: its Phase Changed below)
    new_round_state(w, true); // (the players back at their starts with it)
    enter_phase(w, MATCH_WAITING, true);
    return true;
}
int rules_round(int* rounds) {
    if (rounds) *rounds = R.cfg.rounds > 0 ? R.cfg.rounds : 1;
    return R.round > 0 ? R.round : 1;
}
bool rules_result(bool* won, char* reason, uint32_t cap, Entity* winner) {
    if (won) *won = (R.decided || R.over) && local_won();
    if (reason && cap) snprintf(reason, cap, "%s", R.reason);
    if (winner) *winner = R.win_slot >= 0 && R.win_slot < (int)R.nslots ? R.slot[R.win_slot].e : ENTITY_NONE;
    return R.over;
}
const RulesSlot* rules_slot(World* w, Entity player) {
    int i = slot_index(w, player ? player : local_player(w));
    return i >= 0 ? &R.slot[i] : NULL;
}

// --- Bytes (the network's message and the save's chunk: little-endian, read with limits) ---
typedef struct Wr { uint8_t* p; uint32_t n, cap; bool bad; } Wr;
typedef struct Rd { const uint8_t* p; uint32_t n, at; bool bad; } Rd;
static void wb(Wr* w, const void* d, uint32_t n) {
    if (w->bad || w->n + n > w->cap) {
        w->bad = true;
        return;
    }
    memcpy(w->p + w->n, d, n), w->n += n;
}
static void w8(Wr* w, int v) { uint8_t b = (uint8_t)v; wb(w, &b, 1); }
static void w32(Wr* w, int32_t v) { wb(w, &v, 4); }
static void wf(Wr* w, float v) { wb(w, &v, 4); }
static bool rb(Rd* r, void* d, uint32_t n) {
    if (r->bad || r->at + n > r->n) {
        r->bad = true;
        memset(d, 0, n);
        return false;
    }
    memcpy(d, r->p + r->at, n), r->at += n;
    return true;
}
static int r8(Rd* r) { uint8_t b; rb(r, &b, 1); return b; }
static int s8(Rd* r) { int8_t b; rb(r, &b, 1); return b; }
static int32_t r32(Rd* r) { int32_t v; rb(r, &v, 4); return v; }
static float rf(Rd* r) { float v; rb(r, &v, 4); return isfinite(v) ? v : 0.0f; }

#define RULES_NET_VERSION 1
#define RULES_SAVE_VERSION 1

// The state every machine shares: the phase, the round, the clock, the result, the teams, each slot
static void write_state(Wr* o) {
    w8(o, R.phase), w8(o, R.round), wf(o, R.phase_t), wf(o, R.clock), wf(o, R.limit);
    w8(o, (R.clock_stopped ? 1 : 0) | (R.expired ? 2 : 0) | (R.decided ? 4 : 0) | (R.over ? 8 : 0));
    w8(o, R.win_team), w8(o, R.win_slot), w8(o, R.lost_rounds);
    uint8_t len = (uint8_t)strnlen(R.reason, REASON - 1);
    w8(o, len), wb(o, R.reason, len);
    for (int t = 0; t < RULES_MAX_TEAMS; t++) w32(o, R.team_score[t]), w8(o, R.team_rounds[t]);
}
static void read_state(Rd* r) {
    R.phase = r8(r) % MATCH_PHASE_COUNT, R.round = r8(r), R.phase_t = rf(r), R.clock = rf(r), R.limit = rf(r);
    int f = r8(r);
    R.clock_stopped = f & 1, R.expired = (f & 2) != 0, R.decided = (f & 4) != 0, R.over = (f & 8) != 0;
    R.win_team = s8(r), R.win_slot = s8(r), R.lost_rounds = r8(r);
    int len = r8(r);
    char reason[256];
    rb(r, reason, (uint32_t)len), reason[len] = 0;
    snprintf(R.reason, sizeof R.reason, "%s", reason);
    for (int t = 0; t < RULES_MAX_TEAMS; t++) R.team_score[t] = r32(r), R.team_rounds[t] = r8(r);
    if (R.round < 1) R.round = 1;
}

uint32_t rules_net_serial(void) { return R.serial; }

uint32_t rules_net_write(World* w, uint8_t* out, uint32_t cap) {
    (void)w;
    if (!R.found || !out) return 0;
    Wr o = { out, 0, cap, false };
    w8(&o, RULES_NET_VERSION);
    write_state(&o);
    w8(&o, (int)R.nslots);
    for (uint32_t i = 0; i < R.nslots; i++) {
        const RulesSlot* s = &R.slot[i];
        w8(&o, s->net), w8(&o, s->team), w8(&o, s->out | (R.human[i] ? 2 : 0));
        w32(&o, s->score), w32(&o, s->kills), w32(&o, s->deaths), w32(&o, s->lives), wf(&o, s->respawn_t);
    }
    R.st.net_sent++;
    return o.bad ? 0 : o.n;
}

// A client: the server's state; the events from what changed (the phase, the scores, a respawn, the clock's end)
void rules_net_read(World* w, const uint8_t* d, uint32_t n) {
    if (!w || !d || !n) return;
    if (!R.found) { // (the server's Game Rules: the match starts here too, with this level's settings)
        if (!find_rules(w)) memset(&R.cfg, 0, sizeof R.cfg), R.cfg.rounds = 1;
        start(w);
    }
    R.client = true;
    Rd r = { d, n, 0, false };
    if (r8(&r) != RULES_NET_VERSION) return;
    int old_phase = R.phase;
    bool old_expired = R.expired;
    int32_t old_score[RULES_MAX_PLAYERS], old_net[RULES_MAX_PLAYERS];
    float old_resp[RULES_MAX_PLAYERS];
    uint32_t old_n = R.nslots;
    for (uint32_t i = 0; i < old_n; i++) old_score[i] = R.slot[i].score, old_net[i] = R.slot[i].net, old_resp[i] = R.slot[i].respawn_t;
    read_state(&r);
    int ns = r8(&r);
    if (ns > RULES_MAX_PLAYERS) ns = RULES_MAX_PLAYERS;
    int me = online_local_player();
    RulesSlot slots[RULES_MAX_PLAYERS];
    uint8_t human[RULES_MAX_PLAYERS];
    memset(slots, 0, sizeof slots);
    for (int i = 0; i < ns && !r.bad; i++) {
        RulesSlot* s = &slots[i];
        s->net = (int8_t)s8(&r), s->team = (int8_t)s8(&r);
        int f = r8(&r);
        s->out = (uint8_t)(f & 1), human[i] = (f & 2) != 0;
        s->score = r32(&r), s->kills = r32(&r), s->deaths = r32(&r), s->lives = r32(&r), s->respawn_t = rf(&r);
        s->e = s->net >= 0 ? online_player_pawn(w, s->net) : ENTITY_NONE;
    }
    if (r.bad) return;
    R.st.net_read++;
    R.nslots = (uint32_t)ns, R.local = -1;
    memcpy(R.slot, slots, sizeof slots), memcpy(R.human, human, sizeof human);
    for (int i = 0; i < ns; i++)
        if (me >= 0 && R.slot[i].net == me) R.local = i;
    if (R.phase != old_phase) { // (Phase Changed, a round's start / end, the match's end: the same events as the server's)
        int p = R.phase;
        R.phase = old_phase;
        float pt = R.phase_t;
        enter_phase(w, p, true);
        R.phase_t = pt;
        if (p == MATCH_GAME_OVER) R.over = true;
    }
    for (int i = 0; i < ns; i++) { // a score that changed (by the same net player), a respawn
        int k = -1;
        for (uint32_t j = 0; j < old_n; j++)
            if (old_net[j] == R.slot[i].net) k = (int)j;
        int32_t before = k >= 0 ? old_score[k] : 0;
        if (R.slot[i].score != before) {
            BpValue a[5];
            memset(a, 0, sizeof a);
            ent_val(&a[0], R.slot[i].e);
            a[1].v[0] = (float)R.slot[i].score, a[2].v[0] = (float)(R.slot[i].score - before), a[3].v[0] = (float)R.slot[i].team;
            a[4].v[0] = R.slot[i].team >= 0 && R.slot[i].team < RULES_MAX_TEAMS ? (float)R.team_score[R.slot[i].team] : 0.0f;
            fire(w, EV_SCORE, a, 5);
        }
        if (k >= 0 && old_resp[k] > 0.0f && R.slot[i].respawn_t <= 0.0f && !R.slot[i].out) {
            BpValue a[2];
            memset(a, 0, sizeof a);
            ent_val(&a[0], R.slot[i].e);
            a[1].v[0] = (float)R.slot[i].lives;
            fire(w, EV_RESPAWNED, a, 2);
        }
    }
    if (R.expired && !old_expired) fire(w, EV_TIMER, NULL, 0);
}

// --- The save's chunk: the match (the phase, the clock, the teams) and the local player's slot ---
uint32_t rules_save_write(World* w, uint8_t* out, uint32_t cap) {
    (void)w;
    if (!R.found || !out) return 0;
    Wr o = { out, 0, cap, false };
    w8(&o, RULES_SAVE_VERSION);
    write_state(&o);
    bool has = R.local >= 0;
    w8(&o, has);
    if (has) {
        const RulesSlot* s = &R.slot[R.local];
        w32(&o, s->score), w32(&o, s->kills), w32(&o, s->deaths), w32(&o, s->lives), w8(&o, s->out), wf(&o, s->respawn_t);
    }
    return o.bad ? 0 : o.n;
}

static void apply_pending(World* w) {
    Rd r = { R.pending, R.pending_n, 0, false };
    R.pending_n = 0;
    int ver = r8(&r);
    if (ver < 1 || ver > RULES_SAVE_VERSION) return; // (a newer engine's: the match starts fresh)
    int phase0 = R.phase;
    read_state(&r);
    if (r8(&r) && R.local >= 0) {
        RulesSlot* s = &R.slot[R.local];
        s->score = r32(&r), s->kills = r32(&r), s->deaths = r32(&r), s->lives = r32(&r), s->out = (uint8_t)r8(&r), s->respawn_t = rf(&r);
    }
    if (r.bad) printf("[RealEngine] Game Rules: the save's match state is damaged (read what was there)\n");
    if (R.phase != phase0) {
        int p = R.phase;
        R.phase = phase0;
        float pt = R.phase_t;
        bool over = R.over;
        enter_phase(w, p, true);
        R.phase_t = pt, R.over = over || p == MATCH_GAME_OVER;
    }
    touch();
}

void rules_save_read(World* w, const uint8_t* d, uint32_t n) {
    if (!d || !n || n > sizeof R.pending) return;
    memcpy(R.pending, d, n), R.pending_n = n;
    if (R.found && w) apply_pending(w);
}

// --- The widgets ---
static void widgets_begin(World* w) {
    (void)w;
    if (R.test || !(R.cfg.flags & GR_SHOW_HUD)) return;
    uint32_t h = rules_hud_asset();
    BpAsset* a = h ? bp_find_hash(h) : NULL;
    if (a && (a->compiled || bp_compile(a))) R.hud = ui_create_widget(w, a);
    if (R.hud) ui_add_to_viewport(R.hud, 5);
}
static void show_end(World* w) {
    if (R.test || !(R.cfg.flags & GR_SHOW_END) || R.end) return;
    uint32_t h = rules_end_asset();
    BpAsset* a = h ? bp_find_hash(h) : NULL;
    if (a && (a->compiled || bp_compile(a))) R.end = ui_create_widget(w, a);
    if (!R.end) return;
    ui_add_to_viewport(R.end, 100);
    R.mouse_before = ui_mouse_shown();
    ui_show_mouse(true);
}

Entity rules_widget(bool end) { return end ? R.end : R.hud; }

// --- The Debug Viewer ---
const RulesStats* rules_stats(void) { return &R.st; }

static void dv_rules(DvList* l, const DvContext* c) {
    World* w = c ? c->world : NULL;
    l->ms = R.st.tick_ms;
    if (!R.found) {
        snprintf(l->note, sizeof l->note, "%s", "No Game Rules in the level (Place Actors > Game Rules).");
        return;
    }
    char tm[24];
    float el = 0.0f, left = rules_time_remaining(&el);
    rules_format_time(R.cfg.flags & GR_TIMER_DOWN ? left : el, tm, sizeof tm);
    snprintf(l->note, sizeof l->note, "%s, round %d/%d, %s%s; %u players, %u of %u tagged alive; %u kills, %u respawns", g_match_phase_names[R.phase],
             R.round, R.cfg.rounds > 0 ? R.cfg.rounds : 1, tm, R.client ? " (from the server)" : "", R.nslots, R.st.alive_tagged, R.st.tagged,
             R.st.kills, R.st.respawns);
    for (uint32_t i = 0; i < R.nslots; i++) {
        const RulesSlot* s = &R.slot[i];
        const CName* nm = w && ecs_alive(w, s->e) ? ecs_get(w, s->e, COMP_NAME) : NULL;
        char name[64], info[96];
        snprintf(name, sizeof name, "%s%s", nm ? nm->text : "(gone)", is_local((int)i) ? " (local)" : "");
        snprintf(info, sizeof info, "score %d, team %d, lives %d, deaths %d%s%s", s->score, s->team, s->lives, s->deaths,
                 s->respawn_t > 0.0f ? ", respawning" : "", s->out ? ", out" : "");
        dv_add(l, name, info, 0.0f, s->e);
    }
}
void rules_register_debug(void) { dv_register("Game Rules", dv_rules); }

// --- The self-test ---
static uint32_t g_died;
static void test_health_event(World* w, Entity e, int ev, float amount, const char* type, Entity a, Entity b) {
    (void)amount, (void)b;
    if (ev == HEALTH_EV_DEATH) g_died++, rules_on_death(w, e, a, type);
}

static Entity t_actor(World* w, float x, float hp, const char* tag) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_HEALTH));
    if (!e) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    h->max_health = h->health = hp;
    if (tag) gt_add(w, e, tag);
    return e;
}
static void t_run(World* w, float seconds) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += 0.05f) health_update(w, 0.05f), rules_tick(w, 0.05f);
}

int rules_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: rules: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
    World w;
    if (!ecs_init(&w)) return 1;
    health_set_hooks(&(HealthHooks){ test_health_event, NULL, NULL });
    health_begin_play(&w);
    R.test = true;
    Entity player = t_actor(&w, 0, 100, NULL);
    Entity start = ecs_create(&w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PLAYER_START));
    ((CTransform*)ecs_get(&w, start, COMP_TRANSFORM))->location[0] = 500.0f;
    ((CTransform*)ecs_get(&w, start, COMP_TRANSFORM))->rotation[1] = 90.0f;
    Entity gm = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_GAME_RULES));
    CGameRules* cfg = ecs_get(&w, gm, COMP_GAME_RULES);
    ecs_component_defaults(COMP_GAME_RULES, cfg);
    cfg->flags &= ~(GR_SHOW_HUD | GR_SHOW_END);
    cfg->wait_time = 1.0f, cfg->countdown = 2.0f, cfg->time_limit = 10.0f, cfg->score_target = 50, cfg->kill_points = 10;
    snprintf(cfg->score_tag[0], GR_TAG, "Enemy.Boss"), cfg->score_tag_points[0] = 40;
    R.test_player = player;
    rules_begin_play(&w);
    memset(R.evn, 0, sizeof R.evn);
    // 1) the phases: Waiting 1 s (Auto Start), the Countdown 3-2-1, Playing
    rules_tick(&w, 0.0f);
    CHECK(rules_active() && rules_phase(NULL) == MATCH_WAITING && R.evn[EV_PHASE] == 1, "the match found, Waiting (%d, %u events)", rules_phase(NULL), R.evn[EV_PHASE]);
    CHECK(R.local == 0 && rules_slot(&w, player), "the local player has slot 0");
    t_run(&w, 1.0f);
    float left = 0.0f;
    CHECK(rules_phase(&left) == MATCH_COUNTDOWN && left > 1.9f, "after Wait Time the Countdown (%d, %.2f s left)", rules_phase(NULL), left);
    t_run(&w, 1.0f);
    rules_phase(&left);
    CHECK(rules_phase(NULL) == MATCH_COUNTDOWN && fabsf(left - 1.0f) < 0.06f, "the countdown at 1 s (%.2f)", left);
    t_run(&w, 1.05f);
    CHECK(rules_phase(NULL) == MATCH_PLAYING && R.evn[EV_ROUND_START] == 1, "Playing after the countdown, On Round Started (%d, %u)", rules_phase(NULL), R.evn[EV_ROUND_START]);
    // 2) the score: kill points by tag (On Death's Killer), Add Score, the target wins
    Entity grunt = t_actor(&w, 100, 10, "Enemy.Grunt"), boss = t_actor(&w, 200, 10, "Enemy.Boss");
    health_apply_damage(&w, grunt, 50, NULL, player, player);
    CHECK(rules_score(&w, player) == 10 && R.slot[0].kills == 1, "a grunt's kill: 10 points (%d)", rules_score(&w, player));
    health_apply_damage(&w, boss, 50, NULL, player, player);
    CHECK(rules_score(&w, player) == 50, "the boss (Enemy.Boss): 40 more (%d)", rules_score(&w, player));
    CHECK(R.evn[EV_SCORE] == 2 && rules_team_score(0) == 50, "On Score Changed twice, the team's score 50 (%u, %d)", R.evn[EV_SCORE], rules_team_score(0));
    t_run(&w, 0.1f);
    CHECK(rules_phase(NULL) == MATCH_ROUND_OVER, "the score target reached: Round Over (%d)", rules_phase(NULL));
    t_run(&w, 3.1f);
    bool won = false;
    char why[64];
    CHECK(rules_result(&won, why, sizeof why, NULL) && won && !strcmp(why, "Score Target Reached") && R.evn[EV_WON] == 1 && R.evn[EV_LOST] == 0,
          "won by points: Game Over, On Match Won (%d, \"%s\", %u)", won, why, R.evn[EV_WON]);
    CHECK(rules_phase(NULL) == MATCH_GAME_OVER && R.evn[EV_ROUND_END] == 1, "Game Over and one On Round Ended");
    CHECK(rules_add_score(&w, player, 5) == 55, "Add Score after the end still counts (55)");
    // 3) lose by time: Restart Match, no countdown, a 4 s clock counting down
    cfg->countdown = 0.0f, cfg->wait_time = 0.0f, cfg->time_limit = 4.0f, cfg->score_target = 0;
    memset(R.evn, 0, sizeof R.evn);
    CHECK(rules_restart(&w, false), "Restart Match");
    CHECK(rules_score(&w, player) == 0 && rules_team_score(0) == 0 && rules_round(NULL) == 1, "the scores back to 0");
    rules_tick(&w, 0.0f), rules_tick(&w, 0.0f);
    CHECK(rules_phase(NULL) == MATCH_PLAYING, "no wait, no countdown: Playing (%d)", rules_phase(NULL));
    t_run(&w, 2.0f);
    float el = 0.0f, rem = rules_time_remaining(&el);
    char tm[16];
    rules_format_time(rem, tm, sizeof tm);
    CHECK(fabsf(rem - 2.0f) < 0.06f && !strcmp(tm, "0:02"), "2 s left (%.2f, \"%s\")", rem, tm);
    game_set_paused(true); // (Set Game Paused: the clock waits)
    t_run(&w, 1.0f);
    game_set_paused(false);
    CHECK(fabsf(rules_time_remaining(NULL) - rem) < 0.001f, "paused: the clock waited (%.2f)", rules_time_remaining(NULL));
    t_run(&w, 2.1f);
    CHECK(R.evn[EV_TIMER] == 1 && rules_phase(NULL) == MATCH_ROUND_OVER, "the clock out: On Timer Expired, Round Over (%u, %d)", R.evn[EV_TIMER], rules_phase(NULL));
    t_run(&w, 3.1f);
    CHECK(rules_result(&won, why, sizeof why, NULL) && !won && !strcmp(why, "Time Up") && R.evn[EV_LOST] == 1, "lost by time (\"%s\", %u)", why, R.evn[EV_LOST]);
    // 4) lives and respawn: 2 lives, 1.5 s, invincible 1 s at the Player Start; out of lives loses
    cfg->time_limit = 0.0f, cfg->lives = 2, cfg->respawn_delay = 1.5f, cfg->invincible_time = 1.0f;
    memset(R.evn, 0, sizeof R.evn);
    rules_restart(&w, false);
    rules_tick(&w, 0.0f), rules_tick(&w, 0.0f);
    bool inf = true;
    CHECK(rules_lives(&w, player, &inf) == 2 && !inf, "2 lives (%d)", rules_lives(&w, player, NULL));
    CHECK(health_apply_damage(&w, player, 1, NULL, player, player) == 0.0f, "the match's start: invincible (the respawn's protection)");
    t_run(&w, 1.1f);
    CHECK(R.evn[EV_RESPAWNED] == 1, "Restart Match respawned the player (%u)", R.evn[EV_RESPAWNED]);
    R.evn[EV_RESPAWNED] = 0;
    Entity enemy = t_actor(&w, 300, 10, "Enemy");
    health_apply_damage(&w, player, 500, NULL, enemy, enemy);
    CHECK(health_is_dead(&w, player) && rules_lives(&w, player, NULL) == 1 && R.slot[0].respawn_t > 1.4f, "dead: 1 life left, the respawn waiting");
    CHECK(rules_score(&w, enemy) == 10 && R.slot[slot_index(&w, enemy)].team == -1 && rules_team_score(0) == 0, "the enemy scored alone, not for the players' team");
    t_run(&w, 1.0f);
    CHECK(health_is_dead(&w, player), "still dead after 1 s (the delay 1.5)");
    t_run(&w, 0.6f);
    const CTransform* pt = ecs_get(&w, player, COMP_TRANSFORM);
    CHECK(!health_is_dead(&w, player) && R.evn[EV_RESPAWNED] == 1 && pt && fabsf(pt->location[0] - 500.0f) < 0.1f && fabsf(pt->rotation[1] - 90.0f) < 0.1f,
          "respawned at the Player Start (x %.0f, yaw %.0f), On Player Respawned", pt ? pt->location[0] : 0, pt ? pt->rotation[1] : 0);
    CHECK(health_apply_damage(&w, player, 10, NULL, enemy, enemy) == 0.0f, "invincible right after");
    t_run(&w, 1.1f);
    CHECK(health_apply_damage(&w, player, 10, NULL, enemy, enemy) > 9.9f, "not invincible after 1 s");
    CHECK(rules_add_lives(&w, player, 1) == 2, "Add Lives: 2");
    health_apply_damage(&w, player, 500, NULL, enemy, enemy);
    t_run(&w, 1.6f);
    health_apply_damage(&w, player, 500, NULL, enemy, enemy); // (invincible: nothing)
    t_run(&w, 1.1f);
    health_apply_damage(&w, player, 500, NULL, enemy, enemy);
    t_run(&w, 0.1f);
    CHECK(R.slot[0].out && rules_phase(NULL) == MATCH_ROUND_OVER, "out of lives: the round lost (%d, out %d)", rules_phase(NULL), R.slot[0].out);
    t_run(&w, 3.1f);
    CHECK(rules_result(&won, why, sizeof why, NULL) && !won && !strcmp(why, "Out of Lives"), "lost: \"%s\"", why);
    // 5) elimination by tag over best of 3 rounds; End Match; Set Match Phase
    cfg->lives = 0, cfg->rounds = 3, cfg->round_over_time = 0.5f;
    cfg->flags |= GR_RESET_SCORE;
    memset(R.evn, 0, sizeof R.evn);
    rules_restart(&w, false);
    rules_tick(&w, 0.0f), rules_tick(&w, 0.0f);
    CHECK(!health_is_dead(&w, player), "the player back for the match");
    health_revive(&w, enemy, 0.0f);
    Entity e2 = t_actor(&w, 400, 10, "Enemy.Grunt");
    t_run(&w, 0.3f);
    health_apply_damage(&w, enemy, 50, NULL, player, player);
    t_run(&w, 0.1f);
    CHECK(rules_phase(NULL) == MATCH_PLAYING && R.st.alive_tagged == 1, "one of two Enemy left: still Playing (%u alive)", R.st.alive_tagged);
    health_apply_damage(&w, e2, 50, NULL, player, player);
    health_apply_damage(&w, grunt, 1, NULL, player, player); // (dead already: nothing)
    t_run(&w, 0.1f);
    uint32_t seen = 0;
    CHECK(rules_phase(NULL) == MATCH_ROUND_OVER && tagged_alive(&w, &seen) == 0, "every Enemy dead: the round won (%d)", rules_phase(NULL));
    t_run(&w, 0.6f);
    CHECK(rules_round(NULL) == 2 && rules_phase(NULL) == MATCH_PLAYING && rules_score(&w, player) == 0, "round 2 (scores reset): %d, %d", rules_round(NULL), rules_phase(NULL));
    health_revive(&w, enemy, 0.0f), health_revive(&w, e2, 0.0f), health_revive(&w, boss, 0.0f), health_revive(&w, grunt, 0.0f);
    t_run(&w, 0.3f);
    CHECK(rules_set_phase(&w, MATCH_ROUND_OVER), "Set Match Phase: Round Over (the round lost)");
    t_run(&w, 0.6f);
    CHECK(rules_round(NULL) == 3 && rules_phase(NULL) == MATCH_PLAYING, "round 3 (%d)", rules_round(NULL));
    CHECK(rules_end_match(&w, true, "Escaped", ENTITY_NONE) && rules_result(&won, why, sizeof why, NULL) && won && !strcmp(why, "Escaped"), "End Match won: \"%s\"", why);
    CHECK(R.evn[EV_WON] == 1 && R.evn[EV_ROUND_START] == 3, "On Match Won once, 3 rounds started (%u, %u)", R.evn[EV_WON], R.evn[EV_ROUND_START]);
    // 6) the save's chunk and the network's message: the state back the same
    uint8_t buf[1024];
    rules_add_score(&w, player, 7);
    uint32_t n = rules_save_write(&w, buf, sizeof buf);
    int32_t sc = rules_score(&w, player);
    rules_restart(&w, false);
    CHECK(rules_score(&w, player) == 0, "restarted before the load");
    rules_save_read(&w, buf, n);
    CHECK(n > 0 && rules_score(&w, player) == sc && rules_phase(NULL) == MATCH_GAME_OVER && rules_round(NULL) == 3, "the save's chunk read back (%u bytes, score %d, phase %d)", n,
          rules_score(&w, player), rules_phase(NULL));
    uint8_t cut[8];
    memcpy(cut, buf, sizeof cut);
    rules_save_read(&w, cut, sizeof cut); // (cut short: read with limits, nothing past it)
    CHECK(rules_active(), "a damaged chunk is read safely");
    uint32_t nn = rules_net_write(&w, buf, sizeof buf);
    CHECK(nn > 0 && nn < 200, "the network's message: %u bytes (under 200)", nn);
    { // a client reading it: the server's phase, round, team score and the slot's score come back, with their events
        int32_t ts = rules_team_score(0), ps = R.slot[0].score;
        int round = R.round;
        R.phase = MATCH_PLAYING, R.team_score[0] = 0, R.slot[0].score = 0, R.round = 1;
        memset(R.evn, 0, sizeof R.evn);
        rules_net_read(&w, buf, nn);
        CHECK(R.client && rules_phase(NULL) == MATCH_GAME_OVER && rules_team_score(0) == ts && R.slot[0].score == ps && rules_round(NULL) == round,
              "the client has the server's state (phase %d, team %d / %d, round %d)", rules_phase(NULL), rules_team_score(0), ts, rules_round(NULL));
        CHECK(R.evn[EV_PHASE] == 1 && R.evn[EV_SCORE] == 1 && R.evn[EV_WON] + R.evn[EV_LOST] == 1, "the client's events: Phase Changed, Score Changed, the end (%u, %u)",
              R.evn[EV_PHASE], R.evn[EV_SCORE]);
        CHECK(rules_add_score(&w, player, 5) == 0 && !rules_start_match(&w), "a client decides nothing (Add Score, Start Match refused)");
        R.client = false;
        R.local = 0, R.slot[0].e = player, R.human[0] = 1; // (offline again: the slot's pawn back)
    }
    // 7) 1000 deaths (enemies with tags, the player killing) timed
    rules_restart(&w, false);
    cfg->rounds = 1, cfg->flags &= ~(GR_WIN_ELIMINATE | GR_WIN_SCORE);
    rules_tick(&w, 0.0f), rules_tick(&w, 0.0f);
    Entity many[1000];
    for (int i = 0; i < 1000; i++) many[i] = t_actor(&w, (float)i, 1, i & 1 ? "Enemy.Boss" : "Enemy");
    double t0 = re_time_seconds();
    for (int i = 0; i < 1000; i++) health_apply_damage(&w, many[i], 5, NULL, player, player);
    double ms = (re_time_seconds() - t0) * 1000.0;
    t0 = re_time_seconds();
    for (int i = 0; i < 100; i++) rules_tick(&w, 0.01f);
    double tick = (re_time_seconds() - t0) * 1000.0 / 100.0;
    printf("[RealEngine] Game Rules self-test: 1000 deaths (damage + the rules) %.3f ms, a frame's rules %.4f ms (%u slots); the network's message %u bytes, the save's chunk %u\n",
           ms, tick, R.nslots, nn, n);
    CHECK(rules_score(&w, player) == 500 * 10 + 500 * 40, "1000 kills scored (%d)", rules_score(&w, player));
    CHECK(ms < 5.0 && tick < 0.05, "the cost: 1000 deaths %.3f ms (5), a frame %.4f ms (0.05)", ms, tick);
    rules_end_play(&w);
    health_end_play();
    health_set_hooks(NULL);
    ecs_shutdown(&w);
    memset(&R, 0, sizeof R);
    g_died = 0;
#undef CHECK
    if (!fails) printf("[RealEngine] Game Rules self-test: OK\n");
    return fails;
}
