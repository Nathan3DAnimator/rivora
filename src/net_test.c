// The network layer's test (--net-test, notas-dev/online.md): a server and a client in this process over the loopback,
// in virtual time, through the emulator (150 ms round trip, +-20 ms jitter, 10% loss, 2% duplicates each way), eight
// times with other dice. The reliable channel must deliver every message once and in order, the sequenced one never an
// older state after a newer, the stats see the round trip and the loss; the time to deliver and the bandwidth printed.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

typedef struct Run { bool ok; int delivered, misorder, snap_got, snap_sent, seq_back, acked, false_acks; uint32_t resent, dropped; double connect_t, deliver_s; NetStats st; } Run;
#include "net_repl.h"
static uint8_t g_got[16384];      // (the snapshots the client got, by number)
static uint32_t g_snap_of[65536]; // (a tracked token's snapshot)

static Run run_once(uint32_t seed) {
    Run R;
    memset(&R, 0, sizeof R);
    NetTransport ts, tc;
    if (!net_loop_open(&ts, 1) || !net_loop_open(&tc, 2)) return R;
    NetHost* s = net_host_create(&ts, true, 4, 1234);
    NetHost* c = net_host_create(&tc, false, 1, 1234);
    NetSim sim = { 75.0f, 20.0f, 0.10f, 0.02f };
    net_set_sim(s, &sim);
    net_set_sim(c, &sim);
    net_set_seed(s, seed * 2u + 1u);
    net_set_seed(c, seed * 2u + 2u);
    NetAddr sa = { NET_ADDR_LOOP, 0, 0, 1 };
    net_connect(c, &sa);
    const int N = 3000;
    int next_send = 0, client_peer = -1;
    uint32_t last_seq = 0;
    bool connected = false;
    double t = 0.0, done_t = -1.0;
    R.connect_t = -1.0;
    const double dt = 1.0 / 120.0; // (a 120 Hz loop; the network ticks at 60)
    memset(g_got, 0, sizeof g_got), memset(g_snap_of, 0, sizeof g_snap_of);
    for (int step = 0; step < 120 * 60 && done_t < 0.0; step++, t += dt) {
        net_update(s, t);
        net_update(c, t);
        NetEvent ev;
        while (net_poll(s, &ev))
            if (ev.type == NET_EV_CONNECTED) client_peer = ev.peer;
        while (net_poll(c, &ev)) {
            if (ev.type == NET_EV_CONNECTED) connected = true, R.connect_t = t;
            if (ev.type != NET_EV_MESSAGE) continue;
            NetReader r = { ev.data, ev.len, 0, false };
            if (ev.channel == NET_RELIABLE) {
                int v = (int)nr_u32(&r);
                if (v != R.delivered) R.misorder++;
                R.delivered = v + 1;
                if (R.delivered == N && done_t < 0.0) done_t = t;
            } else if (ev.channel == NET_SEQUENCED) {
                uint32_t v = nr_u32(&r);
                if (v < sizeof g_got) g_got[v] = 1;
                if (R.snap_got && v <= last_seq) R.seq_back++;
                last_seq = v;
                R.snap_got++;
            }
        }
        if (step % 2 == 0 && client_peer >= 0 && connected) { // the server's 60 Hz tick: 40 reliable messages, a snapshot
            for (int k = 0; k < 40 && next_send < N; k++) {
                uint8_t b[64];
                NetWriter w = { b, 0, sizeof b, false };
                nw_u32(&w, (uint32_t)next_send);
                for (int pad = 0; pad < 12; pad++) nw_u32(&w, 0xABCDEF00u + (uint32_t)pad);
                if (!net_send(s, client_peer, NET_RELIABLE, b, w.len)) break;
                next_send++;
            }
            uint8_t b[400];
            NetWriter w = { b, 0, sizeof b, false };
            nw_u32(&w, (uint32_t)++R.snap_sent);
            for (int pad = 0; pad < 90; pad++) nw_u32(&w, (uint32_t)pad);
            uint32_t tk = net_send_tracked(s, client_peer, NET_SEQUENCED, b, w.len); // (acked back only when it arrived)
            if (tk) g_snap_of[tk & 0xFFFF] = (uint32_t)R.snap_sent;
            net_flush(s, t);
        }
        if (step % 2 == 1) net_flush(c, t);
        if (client_peer >= 0) { // the acked tokens: each one's snapshot must have arrived
            uint32_t acked[64];
            int na = net_take_acked(s, client_peer, acked, 64);
            for (int k = 0; k < na; k++) {
                uint32_t sn = g_snap_of[acked[k] & 0xFFFF];
                R.acked++;
                if (sn >= sizeof g_got || !g_got[sn]) R.false_acks++;
            }
        }
    }
    R.st = net_stats(s, client_peer);
    NetStats sc = net_stats(c, 0);
    R.resent = R.st.resent, R.dropped = sc.dropped_old;
    R.deliver_s = done_t >= 0.0 ? done_t - R.connect_t : -1.0;
    R.ok = connected && R.delivered == N && R.misorder == 0 && R.seq_back == 0 && (R.snap_got + (int)R.dropped) > R.snap_sent * 0.8 &&
           R.st.rtt_ms > 120.0f && R.st.rtt_ms < 230.0f && R.st.loss > 0.03f && R.st.loss < 0.25f && R.deliver_s > 0.0 && R.deliver_s < 30.0 && R.false_acks <= (int)R.dropped && R.acked > R.snap_got / 2; // (a sequenced message dropped as older still had its packet acked)
    net_host_destroy(c);
    net_host_destroy(s);
    return R;
}


// The game's pattern (online.c): the server's loop at 250 Hz, the client's at 120 Hz, a 30 Hz sequenced state each way,
// a flush every frame; 100 ms each way, +-20 ms, 5% loss. The loss the server measures must be the emulated one.
static bool game_pattern(void) {
    NetTransport ts, tc;
    if (!net_loop_open(&ts, 3) || !net_loop_open(&tc, 4)) return false;
    NetHost* s = net_host_create(&ts, true, 4, 99);
    NetHost* c = net_host_create(&tc, false, 1, 99);
    NetSim sim = { 100.0f, 20.0f, 0.05f, 0.0f };
    net_set_sim(s, &sim), net_set_sim(c, &sim);
    net_set_seed(s, 77), net_set_seed(c, 78);
    NetAddr sa = { NET_ADDR_LOOP, 0, 0, 3 };
    net_connect(c, &sa);
    int peer = -1;
    double ts_next = 0.0, tc_next = 0.0, stick = 0.0, ctick = 0.0;
    uint8_t b[200];
    memset(b, 7, sizeof b);
    for (double t = 0.0; t < 12.0;) {
        bool srv = ts_next <= tc_next;
        double now = srv ? ts_next : tc_next;
        t = now;
        NetEvent ev;
        if (srv) {
            net_update(s, now);
            while (net_poll(s, &ev))
                if (ev.type == NET_EV_CONNECTED) peer = ev.peer;
            if (peer >= 0 && now >= stick) stick = now + 1.0 / 30.0, net_send(s, peer, NET_SEQUENCED, b, 150);
            net_flush(s, now);
            ts_next += 1.0 / 250.0;
        } else {
            net_update(c, now);
            while (net_poll(c, &ev)) {}
            if (net_connected(c, 0) && now >= ctick) ctick = now + 1.0 / 30.0, net_send(c, 0, NET_SEQUENCED, b, 40);
            net_flush(c, now);
            tc_next += 1.0 / 120.0;
        }
    }
    NetStats st = net_stats(s, peer), sc = net_stats(c, 0);
    printf("[RealEngine] Net test (the game's pattern): server RTT %.0f ms, loss %.1f%%; client RTT %.0f ms, loss %.1f%%; %.0f / %.0f kbps\n",
           st.rtt_ms, st.loss * 100.0f, sc.rtt_ms, sc.loss * 100.0f, st.kbps_out, sc.kbps_out);
    bool ok = peer >= 0 && st.loss < 0.15f && sc.loss < 0.15f && st.rtt_ms > 150.0f && st.rtt_ms < 300.0f;
    net_host_destroy(c), net_host_destroy(s);
    return ok;
}

int net_selftest(void) {
    printf("[RealEngine] Net test: 150 ms RTT, +-20 ms jitter, 10%% loss, 2%% duplicates each way; 8 runs\n");
    bool all = true;
    uint32_t only = getenv("RE_NETSEED") ? (uint32_t)atoi(getenv("RE_NETSEED")) : 0;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        if (only && seed != only) continue;
        Run R = run_once(seed);
        printf("[RealEngine] Net test %u: %s connected %.0f ms; %d reliable in order in %.2f s (%d out of order), %u resent; "
               "snapshots %d/%d (%u dropped as old, %d back; %d acked, %d falsely); RTT %.0f ms (jitter %.0f), loss %.1f%%, %.0f kbps\n",
               seed, R.ok ? "ok" : "FAILED", R.connect_t * 1000.0, R.delivered, R.deliver_s, R.misorder, R.resent, R.snap_got,
               R.snap_sent, R.dropped, R.seq_back, R.acked, R.false_acks, R.st.rtt_ms, R.st.jitter_ms, R.st.loss * 100.0f, R.st.kbps_out);
        all &= R.ok;
    }
    if (!only) all &= game_pattern();
    if (!only) all &= net_repl_selftest() == 0; // (the replication's baseline, its compact state, its clock)
    printf("[RealEngine] %s\n", all ? "Net test: OK" : "FAIL: net-test");
    return all ? 0 : 1;
}
