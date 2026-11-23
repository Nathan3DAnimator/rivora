// Game Rules (notas-dev/regras-de-jogo.md; módulo 8 do R7): a Game Mode of rules made from scratch, configured in the
// Details and reused by any game (nothing from the demos). One Game Rules component in the level (the ready-made actor
// "Game Rules", Place Actors; or in a Blueprint class: a BP_GameMode with it) runs the match:
//   * phases: Waiting -> Countdown -> Playing -> Round Over -> (the next round's Countdown, or) Game Over, each with its
//     duration (Waiting without Auto Start waits for Start Match)
//   * rounds (best of N: the match is won with more than half of them)
//   * the score of each player and of each team: points per kill (the Health's On Death, health.h: Killer), more for a
//     victim with a Gameplay Tag; Add Score from a Blueprint
//   * win and lose: a score target, the time running out (a loss, a win or the most points), every actor with a Gameplay
//     Tag dead, the player dead N times or out of lives, the manual objective (End Match from a Blueprint)
//   * lives and respawn: after a delay, at a Player Start or at the last Checkpoint (save_progress.h), invincible for N s
//   * the clock: counting down from the Time Limit or up, stopped by Set Game Paused (game_settings.h) and Set Match Timer
//   * events in every Blueprint: On Match Phase Changed, On Score Changed, On Match Won / Lost, On Round Started / Ended,
//     On Player Respawned, On Timer Expired; WBP_MatchHUD and WBP_MatchEnd (the example widgets, Content/UI)
//   * online (online.h): the server decides; the phase, the clock, the scores and the lives go to the clients (M_RULES)
//   * the save (save_progress.h): the match's state is a chunk of its own (RULE) in Save Progress
// Data only and no allocation: the state is a fixed table of RULES_MAX_PLAYERS slots.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "ecs.h"

#define RULES_MAX_PLAYERS 16 // the slots: the players (and the bots that score)
#define RULES_MAX_TEAMS 4
enum { MATCH_WAITING = 0, MATCH_COUNTDOWN, MATCH_PLAYING, MATCH_ROUND_OVER, MATCH_GAME_OVER, MATCH_PHASE_COUNT };
extern const char* const g_match_phase_names[MATCH_PHASE_COUNT]; // "Waiting", "Countdown", "Playing", "Round Over", "Game Over"
extern const char* const g_rules_time_up_names[RULES_TIME_UP_COUNT]; // "Nothing", "Defeat", "Victory", "Most Points Win"

typedef struct RulesSlot {
    Entity e;          // the pawn (ENTITY_NONE: free)
    int32_t score, kills, deaths;
    int32_t lives;     // left (the rules' Lives 0: infinite, not counted)
    int8_t team;       // 0..teams-1
    int8_t net;        // its online player (-1: none / offline)
    uint8_t out;       // out of lives (waits for the next round)
    uint8_t health_inv; // the Health's own Invincible before the respawn's (restored after)
    float respawn_t;   // > 0: dead, respawned when it reaches 0
    float invincible_t; // > 0: invincible after a respawn
    float start[3], start_yaw; // where it began (no Player Start: back here)
    uint32_t cls;      // its Blueprint class (destroyed: spawned again from it)
} RulesSlot;

// --- What the nodes do (on the server, or offline; a client only reads: its Add Score, Start Match... do nothing) ---
int rules_phase(float* phase_left);           // MATCH_*; phase_left: the phase's seconds left (Countdown: 3, 2, 1; 0: none)
bool rules_set_phase(World* w, int phase);
bool rules_start_match(World* w);             // Waiting -> Countdown (no countdown: Playing)
// The match over now: won (the players / Winner's side) or lost, why ("Objective Complete"); winner: a player (its slot
// and team win; none: the local player's side)
bool rules_end_match(World* w, bool won, const char* reason, Entity winner);
int32_t rules_add_score(World* w, Entity player, int32_t points); // the player's new score (its team's too); none: the local player
int32_t rules_score(World* w, Entity player);
int32_t rules_team_score(int team);
int rules_team_rounds(int team);             // the rounds the team won
bool rules_counts_down(void);                // the clock counts down (the Game Rules' Timer Counts Down)
int rules_team_of(World* w, Entity player);
float rules_time_remaining(float* elapsed);   // the clock: counting down, what is left; up: the limit's (no limit: 0)
bool rules_set_timer(World* w, float seconds, bool paused); // the clock to seconds (down: left; up: elapsed), stopped or running
void rules_format_time(float seconds, char* out, uint32_t cap); // "1:05", "12:00", "0:09"
int32_t rules_lives(World* w, Entity player, bool* infinite);
int32_t rules_add_lives(World* w, Entity player, int32_t lives); // what it has now (a player out comes back: respawned)
bool rules_respawn(World* w, Entity player); // now, at its Player Start / the Checkpoint, Revived (and invincible)
bool rules_restart(World* w, bool reload_level); // scores, lives, the clock, round 1 back; reload: the level opened again
int rules_round(int* rounds);                 // 1.. (rounds: how many)
// The match's end: won (for the local player), why, the winner (a player; NONE: a side), over (Game Over)
bool rules_result(bool* won, char* reason, uint32_t cap, Entity* winner);
bool rules_active(void);                      // a Game Rules runs this play
const RulesSlot* rules_slot(World* w, Entity player); // NULL: not one of the match's

// --- The engine's hooks ---
void rules_begin_play(World* w);        // bp_begin_play, before progress_begin_play (a loaded match's state goes over it)
// bp_tick (also paused: the clock waits, the widgets don't). The first frame finds the Game Rules (the Game Mode's class
// is spawned after BeginPlay), the players' slots, the widgets, a load's state
void rules_tick(World* w, float dt);
void rules_end_play(World* w);
// The Health's On Death (bp_vm.c's health_event): points to the killer, a death (and a life) to a player, the
// elimination checked
void rules_on_death(World* w, Entity victim, Entity killer, const char* type);

// --- Online (online.c): the server's state into a message, the client's from it ---
uint32_t rules_net_serial(void);        // changes with what the clients must hear (not the clock: online sends it each second)
uint32_t rules_net_write(World* w, uint8_t* out, uint32_t cap); // bytes (0: no Game Rules)
void rules_net_read(World* w, const uint8_t* d, uint32_t n);

// --- The save (save_progress.c): the match's chunk (the local player's slot, the teams, the phase, the clock) ---
uint32_t rules_save_write(World* w, uint8_t* out, uint32_t cap); // 0: no Game Rules
void rules_save_read(World* w, const uint8_t* d, uint32_t n);    // applied now, or when the match begins (a load's level)

// --- The example widgets (Content/UI, made when missing; their hash, 0: failed) ---
uint32_t rules_hud_asset(void);   // WBP_MatchHUD: the clock, the score, the lives, the round, the countdown
uint32_t rules_end_asset(void);   // WBP_MatchEnd: Victory / Defeat, why, the score, Play Again, Main Menu
Entity rules_widget(bool end);    // the live WBP_MatchHUD (false) / WBP_MatchEnd (true) of this match (NONE: none)

typedef struct RulesStats {
    double tick_ms;                        // the last frame's rules
    uint32_t players, alive_tagged, tagged; // the last frame's slots, the Eliminate Tag's actors alive / seen
    uint32_t kills, deaths, respawns, phase_changes, score_events, net_sent, net_read; // since play started
} RulesStats;
const RulesStats* rules_stats(void);
void rules_register_debug(void); // the Debug Viewer's "Game Rules" list

// The self-test (--rules-test, before the play part in rules_test.c): a world without the VM: the phases and their
// times, win by score, lose by time, a kill's points by tag, lives and respawn (delay, invincible), elimination by tag,
// rounds, the save chunk and the network message round trip, 1000 deaths timed. Failures
int rules_selftest(void);

// --- The editor's Details (rules_ui.c; the level's and a Blueprint's components) ---
bool rules_ui_component(CGameRules* r);
