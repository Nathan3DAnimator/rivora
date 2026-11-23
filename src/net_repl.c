// Replication against what the client confirmed (net_repl.h).
#include "net_repl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool differs(const float a[3], const float b[3], const float ar[3], const float br[3]) {
    for (int k = 0; k < 3; k++)
        if (fabsf(a[k] - b[k]) > 0.2f || fabsf(ar[k] - br[k]) > 0.1f) return true;
    return false;
}

bool net_repl_due(const NetReplSlot* s, const float pos[3], const float rot[3], double now, double resend_s) {
    if (!(s->flags & 2)) return true;                                   // never sent
    if (differs(s->pend_pos, pos, s->pend_rot, rot)) return true;       // moved since the last send
    bool confirmed = (s->flags & 1) && !differs(s->acked_pos, pos, s->acked_rot, rot);
    if (!confirmed && now - s->pend_t > resend_s) return true;          // the last send likely lost: again
    return now - s->pend_t > 5.0;                                       // (a slow refresh, whatever happened)
}

void net_repl_sent(NetReplSlot* s, const float pos[3], const float rot[3], double now, uint32_t tok) {
    memcpy(s->pend_pos, pos, sizeof s->pend_pos), memcpy(s->pend_rot, rot, sizeof s->pend_rot);
    s->pend_t = now, s->pend_tok = tok, s->flags |= 2;
}

void net_repl_reset(NetReplSlot* s) { memset(s, 0, sizeof *s); }

void net_repl_log(NetReplLog* log, uint32_t tok, const uint16_t* idx, int n) {
    if (!tok) return;
    int k = (int)(tok % NET_REPL_LOG);
    if (n > NET_REPL_PER_MSG) n = NET_REPL_PER_MSG;
    log->tok[k] = tok, log->n[k] = (uint16_t)n;
    memcpy(log->idx[k], idx, (size_t)n * sizeof idx[0]);
}

int net_repl_on_ack(NetReplLog* log, NetReplSlot* slots, int nslots, uint32_t tok) {
    int k = (int)(tok % NET_REPL_LOG), done = 0;
    if (log->tok[k] != tok) return 0; // (too old: the ring moved on)
    for (int i = 0; i < log->n[k]; i++) {
        int j = log->idx[k][i];
        if (j >= nslots) continue;
        NetReplSlot* s = &slots[j];
        if (s->pend_tok != tok) { // a newer send since: this one's state is still what the client has now, if nothing moved
            continue;
        }
        memcpy(s->acked_pos, s->pend_pos, sizeof s->acked_pos), memcpy(s->acked_rot, s->pend_rot, sizeof s->acked_rot);
        s->flags |= 1;
        done++;
    }
    log->tok[k] = 0;
    return done;
}

// --- the compact state ---

static void w_i16(NetWriter* w, float v) { nw_u16(w, (uint16_t)(int16_t)lrintf(v < -32767.0f ? -32767.0f : v > 32767.0f ? 32767.0f : v)); }

void net_repl_write_state(NetWriter* w, const float pos[3], const float rot[3], const float vel[3]) {
    float pr = fabsf(fmodf(rot[0], 360.0f)) + fabsf(fmodf(rot[2], 360.0f));
    bool tilt = pr > 0.01f;
    bool moving = fabsf(vel[0]) >= 2.0f || fabsf(vel[1]) >= 2.0f || fabsf(vel[2]) >= 2.0f;
    nw_u8(w, (uint8_t)((tilt ? 1 : 0) | (moving ? 2 : 0)));
    nw_pos(w, pos);
    nw_angle(w, rot[1]);
    if (tilt) nw_angle(w, rot[0]), nw_angle(w, rot[2]);
    if (moving)
        for (int k = 0; k < 3; k++) w_i16(w, vel[k] * 0.25f); // (4 cm/s steps)
}

void net_repl_read_state(NetReader* r, float pos[3], float rot[3], float vel[3]) {
    uint8_t f = nr_u8(r);
    nr_pos(r, pos);
    rot[1] = nr_angle(r);
    rot[0] = rot[2] = 0.0f;
    if (f & 1) rot[0] = nr_angle(r), rot[2] = nr_angle(r);
    vel[0] = vel[1] = vel[2] = 0.0f;
    if (f & 2)
        for (int k = 0; k < 3; k++) vel[k] = (float)(int16_t)nr_u16(r) * 4.0f;
}

void net_repl_write_id(NetWriter* w, uint32_t id) {
    if (id < 0x8000u) nw_u16(w, (uint16_t)id);
    else nw_u16(w, (uint16_t)(0x8000u | (id & 0x7FFFu))), nw_u16(w, (uint16_t)(id >> 15));
}
uint32_t net_repl_read_id(NetReader* r) {
    uint32_t lo = nr_u16(r);
    if (!(lo & 0x8000u)) return lo;
    return (lo & 0x7FFFu) | (uint32_t)nr_u16(r) << 15;
}

// --- lag compensation's history ---

void net_repl_hist_push(NetReplHist* h, double t, const float pos[3]) {
    h->t[h->head] = t;
    memcpy(h->pos[h->head], pos, sizeof h->pos[0]);
    h->head = (h->head + 1) % NET_REPL_HIST;
    if (h->n < NET_REPL_HIST) h->n++;
}

bool net_repl_hist_at(const NetReplHist* h, double t, float pos[3]) {
    if (!h->n) return false;
    int newest = (h->head + NET_REPL_HIST - 1) % NET_REPL_HIST;
    if (t >= h->t[newest]) { memcpy(pos, h->pos[newest], sizeof h->pos[0]); return true; }
    for (int k = 1; k < h->n; k++) {
        int b = (h->head + NET_REPL_HIST - k) % NET_REPL_HIST, a = (b + NET_REPL_HIST - 1) % NET_REPL_HIST;
        if (h->t[a] <= t) {
            double span = h->t[b] - h->t[a];
            float u = span > 1e-6 ? (float)((t - h->t[a]) / span) : 0.0f;
            for (int c = 0; c < 3; c++) pos[c] = h->pos[a][c] + (h->pos[b][c] - h->pos[a][c]) * u;
            return true;
        }
    }
    int oldest = (h->head + NET_REPL_HIST - h->n) % NET_REPL_HIST; // (older than the history: its oldest)
    memcpy(pos, h->pos[oldest], sizeof h->pos[0]);
    return true;
}

// --- the client's clock and delay ---

double net_repl_clock(double* play_t, double target, double dt) {
    double err = target - *play_t;
    if (*play_t <= 0.0 || fabs(err) > 0.25) *play_t = target; // (far off: jump)
    else {
        double rate = 1.0 + err * 2.0; // (50 ms behind: 10% faster)
        rate = rate < 0.9 ? 0.9 : rate > 1.1 ? 1.1 : rate;
        *play_t += dt * rate;
    }
    return *play_t;
}

double net_repl_delay(double tick_s, float jitter_ms, float loss, double project_s) {
    double ticks = 2.0 + (loss > 0.02f ? 1.0 : 0.0) + (loss > 0.10f ? 1.0 : 0.0);
    double d = ticks * tick_s + 2.0 * jitter_ms * 0.001;
    return d > project_s ? d : project_s;
}

// --- the test: an actor walks then stops under 20% loss; the client must end where the server is ---

int net_repl_selftest(void) {
    int fails = 0;
#define RCHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: net-repl: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    // the compact state round trip
    uint8_t buf[64];
    NetWriter w = { buf, 0, sizeof buf, false };
    float p[3] = { 1234.5f, -77.25f, 90.0f }, r[3] = { 0, 135.0f, 0 }, v[3] = { 600.0f, -4.0f, 0 };
    net_repl_write_state(&w, p, r, v);
    int walking = w.len;
    float p2[3], r2[3], v2[3];
    NetReader rd = { buf, w.len, 0, false };
    net_repl_read_state(&rd, p2, r2, v2);
    RCHECK(fabsf(p2[0] - p[0]) < 0.07f && fabsf(p2[1] - p[1]) < 0.07f && fabsf(r2[1] - 135.0f) < 0.01f && fabsf(v2[0] - 600.0f) < 2.1f,
           "the state came back changed");
    w.len = 0;
    float still[3] = { 0, 0, 0 };
    net_repl_write_state(&w, p, r, still);
    RCHECK(walking == 18 && w.len == 12, "sizes %d / %d (want 18 walking, 12 still)", walking, w.len);
    w.len = 0;
    net_repl_write_id(&w, 70000u);
    rd = (NetReader){ buf, w.len, 0, false };
    RCHECK(net_repl_read_id(&rd) == 70000u && w.len == 4, "a long id");
    // the convergence: 64 actors walk for 2 s then stop; 30 Hz ticks; each packet lost 20%, acks 20% too, 120 ms RTT.
    // The old rule (sent when changed, everything each second) against the confirmed baseline
    enum { NA = 64 };
    double old_worst = 0.0;
    for (int rule = 0; rule < 2; rule++) {
      double worst = 0.0;
      for (uint32_t seed = 1; seed <= 40; seed++) {
        static NetReplSlot slot[NA];
        static NetReplLog log;
        memset(slot, 0, sizeof slot), memset(&log, 0, sizeof log);
        float client[NA][3], last_sent[NA][3];
        double last_t[NA];
        memset(client, 0, sizeof client), memset(last_sent, 0, sizeof last_sent);
        for (int a = 0; a < NA; a++) last_t[a] = -10.0;
        struct { double at; uint32_t tok; int n; uint16_t idx[NA]; float pos[NA][3]; bool lost, ack_lost; } fl[256];
        int nfl = 0;
        uint32_t rng = 1234u + seed * 7919u, tok = 1;
        double converged = -1.0;
        for (int tick = 0; tick < 30 * 6; tick++) {
            double now = tick / 30.0;
            float pos[NA][3];
            for (int a = 0; a < NA; a++) {
                double tt = now < 2.0 ? now : 2.0; // (walking, then still from 2 s)
                pos[a][0] = (float)(tt * 600.0) + a * 100.0f, pos[a][1] = (float)a, pos[a][2] = 0.0f;
            }
            // arrivals: the client takes what came; the server the acks
            for (int i = 0; i < nfl; i++) {
                if (fl[i].at > now) continue;
                if (!fl[i].lost) {
                    for (int k = 0; k < fl[i].n; k++) memcpy(client[fl[i].idx[k]], fl[i].pos[k], 12);
                    if (!fl[i].ack_lost && rule == 1) net_repl_on_ack(&log, slot, NA, fl[i].tok);
                }
                fl[i--] = fl[--nfl];
            }
            // the snapshot
            uint16_t idx[NA];
            int n = 0;
            float rot0[3] = { 0, 0, 0 };
            for (int a = 0; a < NA; a++) {
                bool due;
                if (rule == 0) {
                    bool moved = fabsf(last_sent[a][0] - pos[a][0]) > 0.2f || fabsf(last_sent[a][1] - pos[a][1]) > 0.2f;
                    due = moved || now - last_t[a] >= 1.0;
                } else due = net_repl_due(&slot[a], pos[a], rot0, now, 0.12 * 1.25 + 0.02 + 1.0 / 30.0);
                if (!due) continue;
                idx[n++] = (uint16_t)a;
                memcpy(last_sent[a], pos[a], 12), last_t[a] = now;
                if (rule == 1) net_repl_sent(&slot[a], pos[a], rot0, now, tok);
            }
            if (n && nfl < 256) {
                rng = rng * 1664525u + 1013904223u;
                bool lost = (rng >> 8) % 100 < 20;
                rng = rng * 1664525u + 1013904223u;
                bool ack_lost = (rng >> 8) % 100 < 20;
                fl[nfl].at = now + 0.06, fl[nfl].tok = tok, fl[nfl].n = n, fl[nfl].lost = lost, fl[nfl].ack_lost = ack_lost;
                memcpy(fl[nfl].idx, idx, (size_t)n * sizeof idx[0]);
                for (int k = 0; k < n; k++) memcpy(fl[nfl].pos[k], pos[idx[k]], 12);
                // (the ack comes back with the client's next packet: half a round trip later)
                if (!lost) fl[nfl].at = now + 0.06;
                nfl++;
                if (rule == 1) net_repl_log(&log, tok, idx, n);
                tok++;
            }
            if (now > 2.0 && converged < 0.0) {
                bool all = true;
                for (int a = 0; a < NA && all; a++) all = fabsf(client[a][0] - pos[a][0]) < 0.5f;
                if (all) converged = now - 2.0;
            }
        }
        if (converged < 0.0) converged = 9.9;
        if (converged > worst) worst = converged;
      }
      printf("[RealEngine] net-repl: %s: every actor where the server is at most %.0f ms after they stopped (40 runs, 20%% loss)\n",
             rule ? "confirmed baseline" : "old rule (changed + 1 s refresh)", worst * 1000.0);
      if (rule == 0) old_worst = worst;
      else RCHECK(worst < 0.8 && worst * 3.0 < old_worst, "converged in %.0f ms at worst (the old rule %.0f)", worst * 1000.0, old_worst * 1000.0);
    }
    // the clock: 40 ms behind its target, caught up within half a second without a jump
    double play = 10.0, target = 10.04, maxstep = 0.0;
    for (int i = 0; i < 60; i++) {
        double before = play;
        target += 1.0 / 120.0;
        net_repl_clock(&play, target, 1.0 / 120.0);
        if (play - before > maxstep) maxstep = play - before;
    }
    RCHECK(fabs(target - play) < 0.01 && maxstep < 1.0 / 120.0 * 1.11, "the clock: %.1f ms off, a step of %.2f ms", (target - play) * 1000.0, maxstep * 1000.0);
    RCHECK(net_repl_delay(1.0 / 30.0, 5.0f, 0.05f, 0.1) > 0.1 - 1e-9 && net_repl_delay(1.0 / 30.0, 20.0f, 0.12f, 0.05) > 4.0 / 30.0,
           "the delay doesn't grow with the loss");
    // the history: a second back, interpolated
    NetReplHist hh;
    memset(&hh, 0, sizeof hh);
    for (int i = 0; i <= 30; i++) {
        float hp[3] = { (float)i * 20.0f, 0, 0 };
        net_repl_hist_push(&hh, i / 30.0, hp);
    }
    float at[3];
    RCHECK(net_repl_hist_at(&hh, 0.5 + 1.0 / 60.0, at) && fabsf(at[0] - 310.0f) < 0.01f, "the history gave %.1f (want 310)", at[0]);
    if (!fails) printf("[RealEngine] net-repl self-test: ok\n");
#undef RCHECK
    return fails;
}
