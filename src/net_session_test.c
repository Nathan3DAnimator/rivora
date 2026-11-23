// The online test with two processes (--net-host-test, notas-dev/online.md): this editor plays and hosts a LAN session,
// starts a second one (--net-join-test localhost) that joins it, both through the network emulation (latency, jitter,
// loss). The host's Character walks; the client must get its own pawn (possessed), see the host's walk replicated and
// smooth (no step past a few frames of walking), and the host must see two players. The client's result is its exit
// code, which the host waits for.
// --net-adv (the advanced sessions, notas-dev/online.md): the host makes a LAN session with a password, a map, a game mode
// and two properties; the client finds it with property filters (one search that matches, one that doesn't), is refused
// with a wrong password, joins with the right one, sees the host's Update (a new property, Start Session) in its copy of
// the settings, is banned, and is refused when it comes back. No network emulation: the steps' order is what is checked.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "online.h"
#include "platform.h"

static struct {
    bool host, joined_once;
    int max_players;
    PROCESS_INFORMATION child;
    bool child_started;
    float last[3];
    bool have_last;
    float own_last[3]; // (B13) the client's own pawn
    bool own_have;
    int own_frames;
    double own_path;
    double path_cm, max_step_cm;
    int frames, steps_seen;
    // --net-adv
    bool adv;
    int phase;           // the client's step (adv_client); the host's (adv_host)
    double at;           // when the step began
    bool details_ok, filter_ok, wrong_pw_ok, joined_ok, update_ok, banned_ok, refused_ok;
    char fail[160];
} T;
static bool g_adv;
void net_session_set_advanced(bool on) { g_adv = on; }
static const char* ADV_PASSWORD = "abc123";

bool net_session_begin(World* w, bool host, const char* join_addr, double child_seconds, const NetSim* emu) {
    memset(&T, 0, sizeof T);
    T.host = host;
    T.adv = g_adv;
    if (emu) online_set_sim(emu);
    if (!host) return T.adv ? true : online_join_address(w, join_addr); // (advanced: the client finds the session first)
    bool hosted;
    if (T.adv) {
        OnlineSessionSettings st;
        online_settings_default(&st);
        snprintf(st.name, sizeof st.name, "AdvTest");
        snprintf(st.password, sizeof st.password, "%s", ADV_PASSWORD);
        snprintf(st.map, sizeof st.map, "Arena"), snprintf(st.game_mode, sizeof st.game_mode, "CTF");
        st.lan = true, st.max_players = 4, st.private_slots = 1;
        online_set_property("region", "sa"); // (staged: the session takes them when made)
        online_set_property("level", "3");
        hosted = online_host_advanced(w, &st);
    } else hosted = online_host(w, "Test", 4, true);
    if (!hosted) {
        printf("[RealEngine] FAIL: net-session: could not host\n");
        return false;
    }
    char exe[MAX_PATH], cmd[MAX_PATH + 256];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --bp-idle --bench %.1f --net-join-test localhost:%u --net-emu %.0f,%.0f,%.0f%s", exe, child_seconds,
             online_settings()->port, emu ? emu->latency_ms : 0.0f, emu ? emu->jitter_ms : 0.0f, emu ? emu->loss * 100.0f : 0.0f,
             T.adv ? " --net-adv" : "");
    STARTUPINFOA si = { sizeof si };
    // the client's lines in this one's output (the test's log shows both sides; a FAIL there fails the test too)
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE), si.hStdError = GetStdHandle(STD_ERROR_HANDLE), si.hStdInput = NULL;
    T.child_started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &T.child) != 0;
    if (!T.child_started) printf("[RealEngine] FAIL: net-session: could not start the client\n");
    return T.child_started;
}

static void adv_fail(const char* why) {
    if (!T.fail[0]) snprintf(T.fail, sizeof T.fail, "%s (step %d; last error \"%s\")", why, T.phase, online_last_error());
    T.phase = 99;
}
static void adv_step(int phase) {
    T.phase = phase;
    T.at = re_time_seconds();
}
// The host: when the client is in, a property and Start Session; then it bans the client (who comes back and is refused)
static void adv_host(World* w) {
    (void)w;
    double now = re_time_seconds();
    if (T.phase == 0 && online_player_count() >= 2) adv_step(1);
    else if (T.phase == 1 && now - T.at > 0.5) {
        online_set_property("phase", "playing");
        online_start_session();
        adv_step(2);
    } else if (T.phase == 2 && now - T.at > 2.0) {
        for (int p = 0; p < ONLINE_MAX_PLAYERS; p++)
            if (p != online_local_player() && online_player_name(p)[0]) {
                online_ban(p, "test ban");
                break;
            }
        adv_step(3);
    }
}
static void adv_find(const char* key, const char* value, OnlineCompare op, const char* key2, const char* value2, OnlineCompare op2) {
    OnlineSearch q;
    memset(&q, 0, sizeof q);
    q.lan = true, q.max_results = 10, q.server_type = ONLINE_SERVERS_LISTEN, q.same_build_only = true;
    online_search_filters_clear();
    if (key) online_search_filter_add(key, value, op);
    if (key2) online_search_filter_add(key2, value2, op2);
    online_search_filters(q.filters, &q.nfilters);
    if (!online_find_advanced(&q)) adv_fail("the search didn't start");
}
// The client's steps: find (filters that match), find (a filter that doesn't), a wrong password, the right one, the
// host's update seen, banned, refused when back
static void adv_client(World* w) {
    double now = re_time_seconds();
    if (now - T.at > 6.0 && T.phase > 0 && T.phase < 50) return adv_fail("a step took more than 6 s");
    switch (T.phase) {
    case 0:
        adv_find("region", "sa", ONLINE_CMP_EQ, "level", "2", ONLINE_CMP_GE);
        adv_step(1);
        break;
    case 1:
        if (online_finding()) break;
        if (online_session_count() != 1) return adv_fail("the matching search didn't find exactly one session");
        {
            const OnlineSession* ses = online_session(0);
            bool found;
            const char* lvl = online_session_property(0, "level", &found);
            T.details_ok = !strcmp(ses->name, "AdvTest") && !strcmp(ses->level, "Arena") && !strcmp(ses->game_mode, "CTF") &&
                           ses->has_password && ses->players == 1 && ses->max_players == 4 && ses->private_slots == 1 &&
                           ses->ping_ms >= 0.0f && found && !strcmp(lvl, "3") && !ses->dedicated;
            printf("[RealEngine] Net adv: found \"%s\" by %s, %d/%d (+%d private), map %s, mode %s, password %d, ping %.1f ms, level %s\n",
                   ses->name, ses->host, ses->players, ses->max_players, ses->private_slots, ses->level, ses->game_mode,
                   (int)ses->has_password, ses->ping_ms, lvl);
            if (!T.details_ok) return adv_fail("the session's details are wrong");
        }
        adv_find("region", "eu", ONLINE_CMP_EQ, NULL, NULL, 0);
        adv_step(2);
        break;
    case 2:
        if (online_finding()) break;
        T.filter_ok = online_session_count() == 0;
        if (!T.filter_ok) return adv_fail("a filter that doesn't match still found the session");
        adv_find(NULL, NULL, 0, NULL, NULL, 0);
        adv_step(3);
        break;
    case 3:
        if (online_finding()) break;
        if (online_session_count() < 1) return adv_fail("the session wasn't found again");
        online_join_ex(w, 0, "wrong");
        adv_step(4);
        break;
    case 4: // refused: back to standalone with the reason
        if (online_role() != ONLINE_STANDALONE || !online_last_error()[0]) break;
        T.wrong_pw_ok = strstr(online_last_error(), "password") != NULL;
        printf("[RealEngine] Net adv: a wrong password: \"%s\"\n", online_last_error());
        if (!T.wrong_pw_ok) return adv_fail("the wrong password wasn't the reason");
        online_join_ex(w, 0, ADV_PASSWORD);
        adv_step(5);
        break;
    case 5:
        if (online_role() != ONLINE_CLIENT || online_local_player() < 0 || online_session_state() == ONLINE_STATE_NONE) break;
        T.joined_ok = true;
        printf("[RealEngine] Net adv: joined as player %d with the password\n", online_local_player());
        adv_step(6);
        break;
    case 6: { // the host's update reaches the client's copy
        bool f1, f2;
        const char* ph = online_get_property("phase", &f1);
        const char* rg = online_get_property("region", &f2);
        if (online_session_state() != ONLINE_STATE_IN_PROGRESS || !f1 || strcmp(ph, "playing")) break;
        T.update_ok = f2 && !strcmp(rg, "sa") && !strcmp(online_current_settings()->map, "Arena");
        printf("[RealEngine] Net adv: the host's update: state %d, phase \"%s\", region \"%s\"\n", (int)online_session_state(), ph, rg);
        if (!T.update_ok) return adv_fail("the client's copy of the settings is wrong");
        adv_step(7);
        break;
    }
    case 7: // banned
        if (online_role() != ONLINE_STANDALONE) break;
        T.banned_ok = strstr(online_last_error(), "banned") != NULL;
        printf("[RealEngine] Net adv: sent away: \"%s\"\n", online_last_error());
        if (!T.banned_ok) return adv_fail("the ban wasn't the reason");
        adv_find(NULL, NULL, 0, NULL, NULL, 0);
        adv_step(8);
        break;
    case 8:
        if (online_finding()) break;
        if (online_session_count() < 1) return adv_fail("the session wasn't found after the ban");
        online_join_ex(w, 0, ADV_PASSWORD);
        adv_step(9);
        break;
    case 9:
        if (online_role() != ONLINE_STANDALONE || now - T.at < 0.2 || !online_last_error()[0]) break;
        T.refused_ok = strstr(online_last_error(), "banned") != NULL;
        printf("[RealEngine] Net adv: back after the ban: \"%s\"\n", online_last_error());
        if (!T.refused_ok) return adv_fail("a banned player came back in");
        adv_step(50); // (done)
        break;
    default: break;
    }
}

void net_session_frame(World* w) {
    int n = online_player_count();
    if (n > T.max_players) T.max_players = n;
    if (T.adv) {
        if (T.host) adv_host(w);
        else adv_client(w);
        return;
    }
    if (T.host || online_role() != ONLINE_CLIENT) return;
    { // (B13) the client walks its own Character in a square, jumping now and then: predicted here, moved by the server
        int me = online_local_player();
        Entity mine = me >= 0 ? online_player_pawn(w, me) : ENTITY_NONE;
        CCharacter* c = mine && bp_player(w) == mine ? ecs_get(w, mine, COMP_CHARACTER) : NULL;
        const CTransform* mt = c ? ecs_get(w, mine, COMP_TRANSFORM) : NULL;
        if (c && mt) {
            T.own_frames++;
            int side = (T.own_frames / 90) % 4;
            c->input[0] = side == 0 ? 1.0f : side == 2 ? -1.0f : 0.0f, c->input[1] = side == 1 ? 1.0f : side == 3 ? -1.0f : 0.0f;
            if (T.own_frames % 150 == 75 && c->on_ground) c->velocity[2] = c->jump_z_velocity, c->on_ground = 0, c->jumps++;
            if (T.own_have) {
                float dx = mt->location[0] - T.own_last[0], dy = mt->location[1] - T.own_last[1];
                T.own_path += sqrt(dx * dx + dy * dy);
            }
            memcpy(T.own_last, mt->location, sizeof T.own_last), T.own_have = true;
        }
    }
    // the client: the host's pawn (player 0) as drawn here, frame to frame
    Entity hp = online_player_pawn(w, 0);
    const CTransform* t = hp ? ecs_get(w, hp, COMP_TRANSFORM) : NULL;
    if (!t) return;
    T.frames++;
    if (T.have_last && online_stats().snapshots > 10) { // (after the first states: the first one moves it from the level's start to the host's place)
        float dx = t->location[0] - T.last[0], dy = t->location[1] - T.last[1];
        double d = sqrt(dx * dx + dy * dy);
        T.path_cm += d;
        if (d > 0.01) T.steps_seen++;
        if (d > T.max_step_cm) T.max_step_cm = d;
    }
    memcpy(T.last, t->location, sizeof T.last);
    T.have_last = true;
}

bool net_session_check(World* w) {
    OnlineStats s = online_stats();
    printf("[RealEngine] Net session: role %d, active %d, ticks %d\n", (int)online_role(), (int)online_active(), online_debug_ticks());
    bool ok;
    if (T.adv && T.host) {
        DWORD code = 1;
        if (T.child_started) {
            WaitForSingleObject(T.child.hProcess, 30000);
            GetExitCodeProcess(T.child.hProcess, &code);
            CloseHandle(T.child.hProcess);
            CloseHandle(T.child.hThread);
        }
        // the refusals: the wrong password and the banned player's return
        printf("[RealEngine] Net adv (host): %d players at most, %u refused, %d banned, state %d; the client exited with %lu\n",
               T.max_players, s.refused, online_ban_count(), (int)online_session_state(), (unsigned long)code);
        ok = T.max_players >= 2 && s.refused >= 2 && online_ban_count() == 1 && T.phase >= 3 && code == 0;
        printf("[RealEngine] %s\n", ok ? "Net session: OK" : "FAIL: net-session (advanced, host)");
        return ok;
    }
    if (T.adv) {
        printf("[RealEngine] Net adv (client): details %d, filters %d, wrong password %d, joined %d, update %d, banned %d, refused back %d%s%s\n",
               T.details_ok, T.filter_ok, T.wrong_pw_ok, T.joined_ok, T.update_ok, T.banned_ok, T.refused_ok, T.fail[0] ? "; " : "", T.fail);
        ok = T.phase == 50;
        printf("[RealEngine] %s\n", ok ? "Net session: OK" : "FAIL: net-session (advanced, client)");
        return ok;
    }
    if (T.host) {
        printf("[RealEngine] Net session (host): %d players at most, %d replicated, RTT %.0f ms, loss %.1f%%, %.0f kbps out\n",
               T.max_players, s.replicated, s.rtt_ms, s.loss * 100.0f, s.kbps_out);
        DWORD code = 1;
        if (T.child_started) {
            WaitForSingleObject(T.child.hProcess, 30000);
            GetExitCodeProcess(T.child.hProcess, &code);
            CloseHandle(T.child.hProcess);
            CloseHandle(T.child.hThread);
        }
        printf("[RealEngine] Net session: the client exited with %lu; the server applied %u of its moves (B13)\n", (unsigned long)code, s.moves_applied);
        ok = T.max_players >= 2 && code == 0 && s.moves_applied > 100;
    } else {
        int me = online_local_player();
        Entity mine = me >= 0 ? online_player_pawn(w, me) : ENTITY_NONE;
        bool possessed = mine && bp_player(w) == mine;
        printf("[RealEngine] Net session (client): player %d, own pawn %s, %d players; the host's pawn walked %.0f cm here in %d "
               "frames, largest step %.1f cm; RTT %.0f ms, interpolation %.0f ms, %u snapshots, %u corrections\n",
               me, possessed ? "possessed" : "MISSING", T.max_players, T.path_cm, T.frames, T.max_step_cm, s.rtt_ms, s.interp_ms,
               s.snapshots, s.corrections);
        // (walking 600 cm/s: ~10 cm a frame at 60 fps; a step of 60 cm is a jump the interpolation failed to hide)
        ok = me >= 0 && possessed && T.max_players >= 2 && T.path_cm > 200.0 && T.max_step_cm < 60.0 && s.snapshots > 10;
        // (B13) its own walk: predicted (it moved here), confirmed by the server, states taken back rarely and small
        printf("[RealEngine] Net prediction (client): walked %.0f cm, %u moves, %u confirmed, %u corrections (%u moves replayed), gap %.2f cm mean, %.2f cm worst\n",
               T.own_path, s.pred_moves, s.pred_acks, s.pred_corrections, s.pred_replayed, s.pred_err_mean, s.pred_err_max);
        bool pred = T.own_path > 500.0 && s.pred_acks > 20 && s.pred_err_mean < 5.0f && s.pred_corrections * 4 <= s.pred_acks;
        if (!pred) printf("[RealEngine] FAIL: net prediction: the own Character's moves, their confirmation or the corrections\n");
        ok = ok && pred;
    }
    printf("[RealEngine] %s\n", ok ? "Net session: OK" : "FAIL: net-session");
    return ok;
}

// Steam (--steam-test): up with the App ID 480 (Spacewar) when the DLL and the client are there; reads only (the user,
// friends, the cloud's state, an achievement): nothing written, no lobby made
#include "steam.h"
int steam_selftest(void) {
    bool up = steam_init(480);
    printf("[RealEngine] Steam: %s\n", steam_status());
    if (!up) {
        printf("[RealEngine] Steam test: skipped (no steam_api64.dll or Steam closed)\n");
        return 0;
    }
    for (int i = 0; i < 20; i++) steam_update(), Sleep(50);
    bool ach = false, got = steam_achievement_get("ACH_WIN_ONE_GAME", &ach);
    uint64_t total = 0, avail = 0;
    bool quota = steam_cloud_quota(&total, &avail);
    printf("[RealEngine] Steam: user \"%s\" (%llu), app %u, language \"%s\", %d friends, overlay %s, cloud %s (%llu of %llu bytes free), "
           "achievement ACH_WIN_ONE_GAME %s\n",
           steam_user_name(), (unsigned long long)steam_user_id(), steam_app_id(), steam_language(), steam_friend_count(),
           steam_overlay_enabled() ? "on" : "off", steam_cloud_enabled() ? "on" : "off", (unsigned long long)avail,
           (unsigned long long)total, got ? (ach ? "unlocked" : "locked") : "unknown");
    NetTransport t;
    bool p2p = steam_net_open(&t);
    printf("[RealEngine] Steam: P2P (networking messages) %s\n", p2p ? "ready" : "MISSING");
    bool ok = steam_user_id() != 0 && steam_app_id() == 480 && p2p && quota;
    steam_shutdown();
    printf("[RealEngine] %s\n", ok ? "Steam test: OK" : "FAIL: steam-test");
    return ok ? 0 : 1;
}
