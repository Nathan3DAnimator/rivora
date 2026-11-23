// Replication against what the client confirmed (notas-dev/online.md "Lag e perda"): for each client and each actor
// the server keeps the last state the client acked (its packet came back acked: net_send_tracked / net_take_acked) and
// the last one sent. A state is sent when the actor moved since the last send, or when the client hasn't confirmed what
// it shows and the last send is older than a round trip (lost: sent again). A lost packet no longer leaves an actor
// stopped in the wrong place until the next full refresh: it converges a round trip later. Plus the states' compact
// form (yaw only when the actor stands upright, no velocity when still, short ids) and the lag compensation's history.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "net.h"

typedef struct NetReplSlot {
    float acked_pos[3], acked_rot[3]; // what the client confirmed
    float pend_pos[3], pend_rot[3];   // the last sent
    double pend_t;
    uint32_t pend_tok;
    uint8_t flags;                    // 1: acked valid, 2: pend valid
} NetReplSlot;

// Due this tick? resend_s: the time after which an unconfirmed send counts as lost (a round trip and its jitter)
bool net_repl_due(const NetReplSlot* s, const float pos[3], const float rot[3], double now, double resend_s);
void net_repl_sent(NetReplSlot* s, const float pos[3], const float rot[3], double now, uint32_t tok);
void net_repl_reset(NetReplSlot* s); // (a new client, an actor gone)

// Which slots a tracked message carried: when its token comes back acked, those slots take their sent state as confirmed
#define NET_REPL_LOG 64
#define NET_REPL_PER_MSG 96
typedef struct NetReplLog {
    uint32_t tok[NET_REPL_LOG];
    uint16_t n[NET_REPL_LOG];
    uint16_t idx[NET_REPL_LOG][NET_REPL_PER_MSG];
} NetReplLog;
void net_repl_log(NetReplLog* log, uint32_t tok, const uint16_t* idx, int n);
int net_repl_on_ack(NetReplLog* log, NetReplSlot* slots, int nslots, uint32_t tok); // slots confirmed

// The compact state: a flags byte, the position (1/8 cm), the yaw (pitch and roll only when not zero), the velocity
// (4 cm/s steps; none when still). 12 bytes standing still, 18 walking (21 before)
void net_repl_write_state(NetWriter* w, const float pos[3], const float rot[3], const float vel[3]);
void net_repl_read_state(NetReader* r, float pos[3], float rot[3], float vel[3]);
void net_repl_write_id(NetWriter* w, uint32_t id); // 2 bytes under 32768, else 4
uint32_t net_repl_read_id(NetReader* r);

// Lag compensation: where each actor was, a second back (the server's ticks)
#define NET_REPL_HIST 64
typedef struct NetReplHist { double t[NET_REPL_HIST]; float pos[NET_REPL_HIST][3]; int head, n; } NetReplHist;
void net_repl_hist_push(NetReplHist* h, double t, const float pos[3]);
bool net_repl_hist_at(const NetReplHist* h, double t, float pos[3]); // interpolated; false: no history then

// The interpolation's clock on a client: follows the server's time minus the delay, sped up or slowed down a few
// percent instead of jumping (a stutter-free buffer); snaps when far off (a level change, a long stall)
double net_repl_clock(double* play_t, double target, double dt);
// The delay a client draws behind: two ticks, one more past 2% loss and another past 10% (a lost snapshot is covered by
// interpolation, not extrapolation), twice the jitter, never under the project's
double net_repl_delay(double tick_s, float jitter_ms, float loss, double project_s);

int net_repl_selftest(void); // failures (in --net-test)
