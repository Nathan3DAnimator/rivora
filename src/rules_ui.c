// The editor's Details of the Game Rules (game_rules.h, notas-dev/regras-de-jogo.md): the phases' times, the rounds, the
// teams, the score, the win and lose conditions, the lives and the respawn, the clock, the widgets. Shared by the level's
// Details (editor.c) and a Blueprint's components (bp_editor.c); while playing, the match's state under it.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdio.h>
#include <string.h>

#include "components.h"
#include "game_rules.h"
#include "i18n.h"
#include "ui_tips.h"

static bool flag(const char* label, uint32_t* flags, uint32_t bit) {
    bool on = (*flags & bit) != 0;
    if (!igCheckbox(label, &on)) return false;
    *flags = on ? *flags | bit : *flags & ~bit;
    return true;
}
static bool drag_u16(const char* label, uint16_t* v, int lo, int hi, const char* fmt) {
    int x = *v;
    if (!igDragInt(label, &x, 0.1f, lo, hi, fmt, 0)) return false;
    *v = (uint16_t)(x < lo ? lo : x > hi ? hi : x);
    return true;
}
static bool drag_u8(const char* label, uint8_t* v, int lo, int hi, const char* fmt) {
    int x = *v;
    if (!igDragInt(label, &x, 0.1f, lo, hi, fmt, 0)) return false;
    *v = (uint8_t)(x < lo ? lo : x > hi ? hi : x);
    return true;
}

bool rules_ui_component(CGameRules* r) {
    bool ch = false;
    igSeparatorText(TR("Match Phases"));
    ch |= flag(PL(TR("Auto Start")), &r->flags, GR_AUTO_START);
    ch |= igDragFloat(PL(TR("Wait Time")), &r->wait_time, 0.1f, 0.0f, 600.0f, "%.1f s", 0);
    ch |= igDragFloat(PL(TR("Countdown")), &r->countdown, 0.1f, 0.0f, 60.0f, r->countdown > 0.0f ? "%.1f s" : TR("none"), 0);
    ch |= igDragFloat(PL(TR("Round Over Time")), &r->round_over_time, 0.1f, 0.0f, 60.0f, "%.1f s", 0);
    ch |= drag_u8(PL(TR("Rounds")), &r->rounds, 1, 99, r->rounds > 1 ? TR("best of %d") : "%d");
    int teams = r->teams;
    const char* tn[5] = { TR("Each for Themself"), TR("One Team (Co-op)"), TR("2 Teams"), TR("3 Teams"), TR("4 Teams") };
    if (igCombo_Str_arr(PL(TR("Teams")), &teams, tn, 5, -1)) r->teams = (uint8_t)teams, ch = true;
    igSeparatorText(TR("Score"));
    ch |= igDragInt(PL(TR("Kill Points")), &r->kill_points, 0.2f, -1000, 100000, "%d", 0);
    for (int k = 0; k < GR_SCORE_TAGS; k++) {
        igPushID_Int(k);
        ch |= igInputTextWithHint(PL(TR("Victim Tag")), k ? "Enemy.Elite" : "Enemy.Boss", r->score_tag[k], sizeof r->score_tag[k], 0, NULL, NULL);
        if (r->score_tag[k][0]) ch |= igDragInt(PL(TR("Tag Points")), &r->score_tag_points[k], 0.2f, -1000, 100000, "%d", 0);
        igPopID();
    }
    igSeparatorText(TR("Win and Lose"));
    ch |= flag(PL(TR("Win by Score")), &r->flags, GR_WIN_SCORE);
    if (r->flags & GR_WIN_SCORE) ch |= igDragInt(PL(TR("Score Target")), &r->score_target, 0.5f, 1, 1000000, "%d", 0);
    ch |= flag(PL(TR("Win by Elimination")), &r->flags, GR_WIN_ELIMINATE);
    if (r->flags & GR_WIN_ELIMINATE) ch |= igInputTextWithHint(PL(TR("Eliminate Tag")), "Enemy", r->eliminate_tag, sizeof r->eliminate_tag, 0, NULL, NULL);
    ch |= flag(PL(TR("Lose Out of Lives")), &r->flags, GR_LOSE_LIVES);
    ch |= flag(PL(TR("Lose by Deaths")), &r->flags, GR_LOSE_DEATHS);
    if (r->flags & GR_LOSE_DEATHS) ch |= drag_u16(PL(TR("Max Deaths")), &r->max_deaths, 1, 9999, "%d");
    igTextDisabled("%s", TR("The manual objective: the End Match node."));
    igSeparatorText(TR("Clock"));
    ch |= flag(PL(TR("Timer Counts Down")), &r->flags, GR_TIMER_DOWN);
    ch |= igDragFloat(PL(TR("Round Time Limit")), &r->time_limit, 1.0f, 0.0f, 36000.0f, r->time_limit > 0.0f ? "%.0f s" : TR("none"), 0);
    int tu = r->time_up < RULES_TIME_UP_COUNT ? r->time_up : 0;
    const char* tun[RULES_TIME_UP_COUNT];
    for (int k = 0; k < RULES_TIME_UP_COUNT; k++) tun[k] = TR(g_rules_time_up_names[k]);
    if (igCombo_Str_arr(PL(TR("When Time Runs Out")), &tu, tun, RULES_TIME_UP_COUNT, -1)) r->time_up = (uint8_t)tu, ch = true;
    igSeparatorText(TR("Lives and Respawn"));
    ch |= drag_u16(PL(TR("Lives")), &r->lives, 0, 999, r->lives ? "%d" : TR("infinite"));
    ch |= flag(PL(TR("Respawn")), &r->flags, GR_RESPAWN);
    if (r->flags & GR_RESPAWN) {
        ch |= igDragFloat(PL(TR("Respawn Delay")), &r->respawn_delay, 0.05f, 0.0f, 120.0f, "%.1f s", 0);
        ch |= flag(PL(TR("At the Last Checkpoint")), &r->flags, GR_AT_CHECKPOINT);
    }
    ch |= igDragFloat(PL(TR("Invincible After Respawn")), &r->invincible_time, 0.05f, 0.0f, 60.0f, r->invincible_time > 0.0f ? "%.1f s" : TR("none"), 0);
    ch |= flag(PL(TR("Reset Score Each Round")), &r->flags, GR_RESET_SCORE);
    igSeparatorText(TR("Widgets"));
    ch |= flag(PL(TR("Show Match HUD")), &r->flags, GR_SHOW_HUD);
    ch |= flag(PL(TR("Show Match End")), &r->flags, GR_SHOW_END);
    if (rules_active()) { // (while playing: the match as it is now)
        igSeparatorText(TR("Match"));
        float left = 0.0f, el = 0.0f;
        int rounds = 1, ph = rules_phase(&left);
        float rem = rules_time_remaining(&el);
        char tm[24];
        rules_format_time(rules_counts_down() ? rem : el, tm, sizeof tm);
        igText("%s: %s   %s %d / %d   %s", TR("Phase"), TR(g_match_phase_names[ph]), TR("Round"), rules_round(&rounds), rounds, tm);
        bool won = false;
        char why[64];
        if (rules_result(&won, why, sizeof why, NULL)) igText("%s: %s", won ? TR("Victory") : TR("Defeat"), TR(why));
    }
    return ch;
}
