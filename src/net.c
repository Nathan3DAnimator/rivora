// The network layer's protocol (net.h): connections, acks, the three channels, the stats and the simulator.
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_BUF 1024       // sent / received packets remembered
#define REL_RING 1024      // reliable messages in flight a peer
#define REL_BYTES (256 * 1024) // their bytes (a ring)
#define RECV_HOLD 256      // reliable messages held out of order (the sender keeps its messages in flight within it)
#define UNREL_BYTES (32 * 1024) // the unreliable bytes queued until the flush
#define REL_PER_PACKET 48  // reliable message ids a packet records
#define EV_MAX 1024
#define EV_BYTES (512 * 1024)
#define SIM_MAX 2048
#define TIMEOUT_S 10.0
#define KEEPALIVE_S 0.1
#define CONNECT_RETRY_S 0.15

enum { PK_CONNECT = 1, PK_ACCEPT, PK_DENY, PK_DATA, PK_DISCONNECT, PK_DISCOVER, PK_INFO };
enum { HEADER = 11 };
#define PROTO_TAG(p) ((uint16_t)(((p) * 2654435761u) >> 16) ^ 0x5245u) // ("RE")

static bool seq_newer(uint16_t a, uint16_t b) { return (uint16_t)(a - b) != 0 && (uint16_t)(a - b) < 32768u; }

typedef struct RelMsg { uint16_t id, len; uint32_t off; double last_sent; uint8_t acked, sent_count; } RelMsg;
#define TOK_PER_PACKET 4  // tracked unreliable messages a packet records (net_send_tracked)
#define TOK_QUEUE 128      // tracked messages queued until the flush
#define TOK_ACKED 256      // acked tokens held until net_take_acked
typedef struct SentPkt { uint16_t seq; uint8_t used, acked; uint8_t nrel, ntok; double time; uint16_t rel[REL_PER_PACKET]; uint32_t tok[TOK_PER_PACKET]; uint32_t bytes; } SentPkt;
typedef struct Hold { uint8_t have; uint16_t len; uint8_t data[NET_MAX_MESSAGE]; } Hold;

typedef struct Peer {
    bool used, connected;
    NetAddr addr;
    double last_recv, last_send, last_try, created;
    uint16_t seq, remote_seq;
    uint32_t ack_bits;
    bool got_any;
    bool ack_due;                  // data came since our last packet: an ack goes at the next flush (the round trip honest)
    SentPkt sent[SEQ_BUF];
    // reliable out: a ring of messages and their bytes
    RelMsg rel[REL_RING];
    uint16_t rel_next, rel_oldest; // ids
    uint8_t* rel_bytes;
    uint32_t rel_head, rel_tail;   // byte offsets (mod REL_BYTES), the tail the oldest unacked message's
    // reliable in
    uint16_t rel_expect;
    Hold* hold;                    // RECV_HOLD
    // unreliable out (ch, len, data records)
    uint8_t unrel[UNREL_BYTES];
    int unrel_len;
    uint16_t sq_out, sq_in;        // sequenced ids
    bool sq_any;
    // tracked unreliable messages: their tokens by the record's offset in unrel, then the acked ones (net_take_acked)
    uint32_t tok_next;
    struct { int at; uint32_t tok; } utok[TOK_QUEUE];
    int nutok;
    uint32_t acked_tok[TOK_ACKED];
    uint16_t acked_head, acked_tail;
    // stats
    float srtt, jitter, loss;
    double win_t;
    uint32_t win_sent, win_lost;
    uint16_t loss_scan;            // the next sent packet to judge (acked in time, or lost)
    double bytes_out, bytes_in, kbps_t;
    float kbps_out, kbps_in;
    uint32_t n_sent, n_recv, n_resent, n_dropped;
} Peer;

typedef struct SimPkt { double at; NetAddr to; int len; uint8_t data[NET_MTU]; } SimPkt;

struct NetHost {
    NetTransport t;
    bool server;
    int max_peers;
    uint32_t protocol;
    uint16_t tag;
    Peer peers[NET_MAX_PEERS];
    NetEvent ev[EV_MAX];
    int nev, ev_at;
    uint8_t* ev_bytes;
    int ev_bytes_len;
    NetSim sim;
    SimPkt* simq;
    int nsim;
    uint32_t rng;
    uint8_t info[512];
    int info_len;
    double now;
};

// --- writers / readers -------------------------------------------------------------------------------------------------
void nw_bytes(NetWriter* w, const void* d, int n) {
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->p + w->len, d, (size_t)n);
    w->len += n;
}
void nw_u8(NetWriter* w, uint8_t v) { nw_bytes(w, &v, 1); }
void nw_u16(NetWriter* w, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; nw_bytes(w, b, 2); }
void nw_u32(NetWriter* w, uint32_t v) { nw_u16(w, (uint16_t)v), nw_u16(w, (uint16_t)(v >> 16)); }
void nw_u64(NetWriter* w, uint64_t v) { nw_u32(w, (uint32_t)v), nw_u32(w, (uint32_t)(v >> 32)); }
void nw_f32(NetWriter* w, float v) { uint32_t u; memcpy(&u, &v, 4); nw_u32(w, u); }
void nw_str(NetWriter* w, const char* s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 255) n = 255;
    nw_u8(w, (uint8_t)n);
    nw_bytes(w, s, (int)n);
}
void nr_bytes(NetReader* r, void* d, int n) {
    if (r->at + n > r->len) { r->overflow = true; memset(d, 0, (size_t)n); r->at = r->len; return; }
    memcpy(d, r->p + r->at, (size_t)n);
    r->at += n;
}
uint8_t nr_u8(NetReader* r) { uint8_t v; nr_bytes(r, &v, 1); return v; }
uint16_t nr_u16(NetReader* r) { uint8_t b[2]; nr_bytes(r, b, 2); return (uint16_t)(b[0] | b[1] << 8); }
uint32_t nr_u32(NetReader* r) { uint32_t lo = nr_u16(r); return lo | (uint32_t)nr_u16(r) << 16; }
uint64_t nr_u64(NetReader* r) { uint64_t lo = nr_u32(r); return lo | (uint64_t)nr_u32(r) << 32; }
float nr_f32(NetReader* r) { uint32_t u = nr_u32(r); float v; memcpy(&v, &u, 4); return v; }
void nr_str(NetReader* r, char* out, int cap) {
    int n = nr_u8(r), k = n < cap - 1 ? n : cap - 1;
    char tmp[256];
    nr_bytes(r, tmp, n);
    memcpy(out, tmp, (size_t)(k > 0 ? k : 0));
    if (cap > 0) out[k > 0 ? k : 0] = 0;
}
void nw_pos(NetWriter* w, const float p[3]) {
    for (int k = 0; k < 3; k++) {
        float v = p[k] * 8.0f;
        int32_t q = (int32_t)lrintf(v < -8388607.0f ? -8388607.0f : v > 8388607.0f ? 8388607.0f : v);
        uint32_t u = (uint32_t)q & 0xFFFFFFu;
        uint8_t b[3] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16) };
        nw_bytes(w, b, 3);
    }
}
void nr_pos(NetReader* r, float p[3]) {
    for (int k = 0; k < 3; k++) {
        uint8_t b[3];
        nr_bytes(r, b, 3);
        int32_t q = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16);
        if (q & 0x800000) q -= 0x1000000;
        p[k] = (float)q / 8.0f;
    }
}
void nw_angle(NetWriter* w, float deg) {
    float a = fmodf(deg, 360.0f);
    if (a < 0.0f) a += 360.0f;
    nw_u16(w, (uint16_t)lrintf(a / 360.0f * 65536.0f));
}
float nr_angle(NetReader* r) {
    float a = (float)nr_u16(r) / 65536.0f * 360.0f;
    return a > 180.0f ? a - 360.0f : a;
}

// --- addresses ---------------------------------------------------------------------------------------------------------
bool net_addr_equal(const NetAddr* a, const NetAddr* b) {
    if (a->kind != b->kind) return false;
    if (a->kind == NET_ADDR_IP) return a->ip == b->ip && a->port == b->port;
    return a->id == b->id;
}
bool net_addr_parse(const char* s, uint16_t default_port, NetAddr* out) {
    memset(out, 0, sizeof *out);
    out->kind = NET_ADDR_IP;
    out->port = default_port;
    if (!s || !s[0] || !strncmp(s, "localhost", 9)) {
        out->ip = 0x7F000001u;
        const char* c = s ? strchr(s, ':') : NULL;
        if (c) out->port = (uint16_t)atoi(c + 1);
        return true;
    }
    unsigned a, b, c, d, p = default_port;
    int n = sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || p > 65535) return false;
    out->ip = a << 24 | b << 16 | c << 8 | d;
    out->port = (uint16_t)p;
    return true;
}
void net_addr_text(const NetAddr* a, char* out, int cap) {
    if (a->kind == NET_ADDR_IP)
        snprintf(out, (size_t)cap, "%u.%u.%u.%u:%u", a->ip >> 24, a->ip >> 16 & 255, a->ip >> 8 & 255, a->ip & 255, a->port);
    else if (a->kind == NET_ADDR_STEAM)
        snprintf(out, (size_t)cap, "steam:%llu", (unsigned long long)a->id);
    else
        snprintf(out, (size_t)cap, "loop:%llu", (unsigned long long)a->id);
}

// --- the host ----------------------------------------------------------------------------------------------------------
static float frand(NetHost* h) {
    h->rng = h->rng * 1664525u + 1013904223u;
    return (float)(h->rng >> 8) / 16777216.0f;
}

static void raw_send(NetHost* h, const NetAddr* to, const uint8_t* data, int len) {
    if (h->sim.latency_ms > 0.0f || h->sim.jitter_ms > 0.0f || h->sim.loss > 0.0f || h->sim.duplicate > 0.0f) {
        if (frand(h) < h->sim.loss) return;
        int copies = frand(h) < h->sim.duplicate ? 2 : 1;
        for (int c = 0; c < copies && h->simq && h->nsim < SIM_MAX; c++) {
            SimPkt* s = &h->simq[h->nsim++];
            float j = (frand(h) * 2.0f - 1.0f) * h->sim.jitter_ms;
            s->at = h->now + fmax(0.0, (h->sim.latency_ms + j) * 0.001);
            s->to = *to;
            s->len = len;
            memcpy(s->data, data, (size_t)len);
        }
        return;
    }
    h->t.send(h->t.ctx, to, data, len);
}

static void sim_pump(NetHost* h) {
    for (int i = 0; i < h->nsim;) {
        if (h->simq[i].at <= h->now) {
            h->t.send(h->t.ctx, &h->simq[i].to, h->simq[i].data, h->simq[i].len);
            h->simq[i] = h->simq[--h->nsim];
        } else {
            i++;
        }
    }
}

static void push_event(NetHost* h, NetEventType type, int peer, uint8_t ch, uint8_t reason, const void* data, int len, const NetAddr* addr) {
    if (h->nev >= EV_MAX) return;
    if (len > 0 && h->ev_bytes_len + len > EV_BYTES) return;
    NetEvent* e = &h->ev[h->nev++];
    memset(e, 0, sizeof *e);
    e->type = type, e->peer = peer, e->channel = ch, e->reason = reason, e->len = (uint16_t)len;
    if (len > 0) {
        memcpy(h->ev_bytes + h->ev_bytes_len, data, (size_t)len);
        e->data = h->ev_bytes + h->ev_bytes_len;
        h->ev_bytes_len += len;
    }
    if (addr) e->addr = *addr;
}

NetHost* net_host_create(const NetTransport* t, bool server, int max_peers, uint32_t protocol) {
    NetHost* h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->t = *t;
    h->server = server;
    h->max_peers = server ? (max_peers < 1 ? 1 : max_peers > NET_MAX_PEERS ? NET_MAX_PEERS : max_peers) : 1;
    h->protocol = protocol;
    h->tag = PROTO_TAG(protocol);
    h->ev_bytes = malloc(EV_BYTES);
    h->rng = 0x9E3779B9u ^ (uint32_t)(uintptr_t)h;
    if (!h->ev_bytes) {
        free(h);
        return NULL;
    }
    return h;
}

static void peer_free(Peer* p) {
    free(p->rel_bytes);
    free(p->hold);
    memset(p, 0, sizeof *p);
}

static int peer_new(NetHost* h, const NetAddr* a) {
    for (int i = 0; i < h->max_peers; i++) {
        Peer* p = &h->peers[i];
        if (p->used) continue;
        memset(p, 0, sizeof *p);
        p->rel_bytes = malloc(REL_BYTES);
        p->hold = calloc(RECV_HOLD, sizeof(Hold));
        if (!p->rel_bytes || !p->hold) {
            peer_free(p);
            return -1;
        }
        p->used = true;
        p->addr = *a;
        p->created = p->last_recv = -1.0; // (stamped by the next net_update: h->now may not be the clock yet)
        p->win_t = p->kbps_t = -1.0;
        return i;
    }
    return -1;
}

static int peer_of(NetHost* h, const NetAddr* a) {
    for (int i = 0; i < h->max_peers; i++)
        if (h->peers[i].used && net_addr_equal(&h->peers[i].addr, a)) return i;
    return -1;
}

static void send_ctrl(NetHost* h, const NetAddr* to, uint8_t type, const void* body, int blen) {
    uint8_t b[NET_MTU];
    NetWriter w = { b, 0, sizeof b, false };
    nw_u16(&w, h->tag);
    nw_u8(&w, type);
    nw_u16(&w, 0), nw_u16(&w, 0), nw_u32(&w, 0);
    if (blen > 0) nw_bytes(&w, body, blen);
    raw_send(h, to, b, w.len);
}

void net_host_destroy(NetHost* h) {
    if (!h) return;
    for (int i = 0; i < h->max_peers; i++)
        if (h->peers[i].used) {
            for (int k = 0; k < 3; k++) h->t.send(h->t.ctx, &h->peers[i].addr, (uint8_t[]){ (uint8_t)h->tag, (uint8_t)(h->tag >> 8), PK_DISCONNECT, 0, 0, 0, 0, 0, 0, 0, 0 }, HEADER);
            peer_free(&h->peers[i]);
        }
    if (h->t.close) h->t.close(h->t.ctx);
    free(h->simq);
    free(h->ev_bytes);
    free(h);
}

void net_set_sim(NetHost* h, const NetSim* sim) {
    h->sim = sim ? *sim : (NetSim){ 0 };
    if (!h->simq && (h->sim.latency_ms > 0 || h->sim.jitter_ms > 0 || h->sim.loss > 0 || h->sim.duplicate > 0)) h->simq = malloc(sizeof(SimPkt) * SIM_MAX);
}

void net_set_info(NetHost* h, const void* data, int len) {
    if (len < 0) len = 0;
    h->info_len = len < (int)sizeof h->info ? len : (int)sizeof h->info;
    if (h->info_len) memcpy(h->info, data, (size_t)h->info_len);
}

int net_connect(NetHost* h, const NetAddr* server) {
    if (h->server) return -1;
    if (h->peers[0].used) peer_free(&h->peers[0]);
    int p = peer_new(h, server);
    if (p != 0) return -1;
    h->peers[0].last_try = -1.0;
    return 0;
}

void net_disconnect(NetHost* h, int peer) {
    if (peer < 0 || peer >= h->max_peers || !h->peers[peer].used) return;
    for (int k = 0; k < 3; k++) send_ctrl(h, &h->peers[peer].addr, PK_DISCONNECT, NULL, 0);
    bool was = h->peers[peer].connected;
    peer_free(&h->peers[peer]);
    if (was) push_event(h, NET_EV_DISCONNECTED, peer, 0, NET_REASON_CLOSED, NULL, 0, NULL);
}

bool net_discover(NetHost* h, uint16_t port) {
    NetAddr b = { NET_ADDR_IP, port, 0xFFFFFFFFu, 0 };
    uint8_t body[4];
    NetWriter w = { body, 0, 4, false };
    nw_u32(&w, h->protocol);
    send_ctrl(h, &b, PK_DISCOVER, body, w.len);
    // this machine too (a second instance, a dedicated server beside the game): a broadcast doesn't always come back to
    // the sender's own sockets (no network adapter, a firewall). The same server may answer twice: online.c keeps one
    NetAddr lo = { NET_ADDR_IP, port, 0x7F000001u, 0 };
    send_ctrl(h, &lo, PK_DISCOVER, body, w.len);
    return true;
}

bool net_send(NetHost* h, int peer, NetChannel ch, const void* data, int len) {
    if (peer < 0 || peer >= h->max_peers || len <= 0 || len > NET_MAX_MESSAGE) return false;
    Peer* p = &h->peers[peer];
    if (!p->used || !p->connected) return false;
    if (ch == NET_RELIABLE) {
        uint16_t inflight = (uint16_t)(p->rel_next - p->rel_oldest);
        uint32_t used = (p->rel_head - p->rel_tail + REL_BYTES) % REL_BYTES;
        if (inflight >= REL_RING - 1 || used + (uint32_t)len + 1 >= REL_BYTES) return false;
        RelMsg* m = &p->rel[p->rel_next % REL_RING];
        m->id = p->rel_next++;
        m->len = (uint16_t)len;
        m->off = p->rel_head;
        m->last_sent = -1.0;
        m->acked = 0, m->sent_count = 0;
        for (int i = 0; i < len; i++) p->rel_bytes[(p->rel_head + (uint32_t)i) % REL_BYTES] = ((const uint8_t*)data)[i];
        p->rel_head = (p->rel_head + (uint32_t)len) % REL_BYTES;
        return true;
    }
    int need = 3 + (ch == NET_SEQUENCED ? 2 : 0) + len;
    if (p->unrel_len + need > UNREL_BYTES) return false;
    NetWriter w = { p->unrel + p->unrel_len, 0, UNREL_BYTES - p->unrel_len, false };
    nw_u8(&w, (uint8_t)ch);
    nw_u16(&w, (uint16_t)len);
    if (ch == NET_SEQUENCED) nw_u16(&w, p->sq_out++);
    nw_bytes(&w, data, len);
    p->unrel_len += w.len;
    return true;
}

uint32_t net_send_tracked(NetHost* h, int peer, NetChannel ch, const void* data, int len) {
    if (ch == NET_RELIABLE || peer < 0 || peer >= h->max_peers) return 0;
    Peer* p = &h->peers[peer];
    if (!p->used || p->nutok >= TOK_QUEUE) return 0;
    int at = p->unrel_len;
    if (!net_send(h, peer, ch, data, len)) return 0;
    if (++p->tok_next == 0) p->tok_next = 1;
    p->utok[p->nutok].at = at, p->utok[p->nutok].tok = p->tok_next, p->nutok++;
    return p->tok_next;
}

int net_take_acked(NetHost* h, int peer, uint32_t* out, int cap) {
    if (peer < 0 || peer >= h->max_peers) return 0;
    Peer* p = &h->peers[peer];
    int n = 0;
    while (p->acked_tail != p->acked_head && n < cap) out[n++] = p->acked_tok[p->acked_tail++ % TOK_ACKED];
    return n;
}

void net_broadcast(NetHost* h, NetChannel ch, const void* data, int len, int except) {
    for (int i = 0; i < h->max_peers; i++)
        if (i != except && h->peers[i].used && h->peers[i].connected) net_send(h, i, ch, data, len);
}

// a packet's acks: each newly acked packet gives a round trip and acks its reliable messages
static void on_acks(NetHost* h, Peer* p, uint16_t ack, uint32_t bits) {
    for (int i = 0; i <= 32; i++) {
        if (i > 0 && !(bits >> (i - 1) & 1u)) continue;
        uint16_t s = (uint16_t)(ack - i);
        SentPkt* sp = &p->sent[s % SEQ_BUF];
        if (!sp->used || sp->seq != s || sp->acked) continue;
        sp->acked = 1;
        float rtt = (float)((h->now - sp->time) * 1000.0);
        if (p->srtt <= 0.0f) p->srtt = rtt;
        else {
            p->jitter += (fabsf(rtt - p->srtt) - p->jitter) * 0.1f;
            p->srtt += (rtt - p->srtt) * 0.1f;
        }
        for (int k = 0; k < sp->nrel; k++) {
            RelMsg* m = &p->rel[sp->rel[k] % REL_RING];
            if (m->id == sp->rel[k]) m->acked = 1;
        }
        for (int k = 0; k < sp->ntok; k++) { // (the tracked unreliable messages it carried: arrived)
            if ((uint16_t)(p->acked_head - p->acked_tail) >= TOK_ACKED) p->acked_tail++; // (nobody reads them: the oldest go)
            p->acked_tok[p->acked_head++ % TOK_ACKED] = sp->tok[k];
        }
    }
    // the ring's tail past every acked message at its front
    while (p->rel_oldest != p->rel_next) {
        RelMsg* m = &p->rel[p->rel_oldest % REL_RING];
        if (!m->acked) break;
        p->rel_tail = (m->off + m->len) % REL_BYTES;
        p->rel_oldest++;
    }
}

static void deliver_reliable(NetHost* h, int pi, Peer* p, uint16_t id, const uint8_t* d, int len) {
    if (id == p->rel_expect) {
        push_event(h, NET_EV_MESSAGE, pi, NET_RELIABLE, 0, d, len, NULL);
        p->rel_expect++;
        for (;;) { // the held ones that follow
            Hold* hd = &p->hold[p->rel_expect % RECV_HOLD];
            if (!hd->have) break;
            push_event(h, NET_EV_MESSAGE, pi, NET_RELIABLE, 0, hd->data, hd->len, NULL);
            hd->have = 0;
            p->rel_expect++;
        }
    } else if (seq_newer(id, p->rel_expect) && (uint16_t)(id - p->rel_expect) < RECV_HOLD) {
        Hold* hd = &p->hold[id % RECV_HOLD];
        if (!hd->have) {
            hd->have = 1;
            hd->len = (uint16_t)len;
            memcpy(hd->data, d, (size_t)len);
        }
    } // (older: a duplicate)
}

static void on_packet(NetHost* h, const NetAddr* from, const uint8_t* b, int len) {
    if (len < HEADER) return;
    NetReader r = { b, len, 0, false };
    if (nr_u16(&r) != h->tag) return;
    uint8_t type = nr_u8(&r);
    bool has_acks = (type & 0x80) != 0; // (a peer that got nothing from us yet has nothing to ack)
    type &= 0x7F;
    uint16_t seq = nr_u16(&r), ack = nr_u16(&r);
    uint32_t bits = nr_u32(&r);
    int pi = peer_of(h, from);
    switch (type) {
    case PK_DISCOVER: {
        // (no info: a session that isn't advertised, Should Advertise off or invite only, doesn't answer)
        if (!h->server || nr_u32(&r) != h->protocol || !h->info_len) return;
        send_ctrl(h, from, PK_INFO, h->info, h->info_len);
        return;
    }
    case PK_INFO:
        if (!h->server) push_event(h, NET_EV_DISCOVERED, -1, 0, 0, b + HEADER, len - HEADER, from);
        return;
    case PK_CONNECT: {
        if (!h->server) return;
        uint32_t proto = nr_u32(&r);
        if (proto != h->protocol) {
            uint8_t why = NET_REASON_VERSION;
            send_ctrl(h, from, PK_DENY, &why, 1);
            return;
        }
        if (pi < 0) {
            pi = peer_new(h, from);
            if (pi < 0) {
                uint8_t why = NET_REASON_FULL;
                send_ctrl(h, from, PK_DENY, &why, 1);
                return;
            }
            h->peers[pi].connected = true;
            push_event(h, NET_EV_CONNECTED, pi, 0, 0, NULL, 0, from);
        }
        h->peers[pi].last_recv = h->now;
        uint8_t idx = (uint8_t)pi;
        send_ctrl(h, from, PK_ACCEPT, &idx, 1); // (again for every CONNECT: the first ACCEPT may be lost)
        return;
    }
    case PK_ACCEPT:
        if (h->server || pi != 0 || h->peers[0].connected) {
            if (pi == 0) h->peers[0].last_recv = h->now;
            return;
        }
        h->peers[0].connected = true;
        h->peers[0].last_recv = h->now;
        push_event(h, NET_EV_CONNECTED, 0, 0, 0, NULL, 0, from);
        return;
    case PK_DENY:
        if (!h->server && pi == 0 && !h->peers[0].connected) {
            uint8_t why = nr_u8(&r);
            peer_free(&h->peers[0]);
            push_event(h, NET_EV_DISCONNECTED, 0, 0, why ? why : NET_REASON_REFUSED, NULL, 0, from);
        }
        return;
    case PK_DISCONNECT:
        if (pi >= 0) {
            bool was = h->peers[pi].connected;
            peer_free(&h->peers[pi]);
            if (was) push_event(h, NET_EV_DISCONNECTED, pi, 0, NET_REASON_CLOSED, NULL, 0, NULL);
        }
        return;
    case PK_DATA: break;
    default: return;
    }
    if (pi < 0 || !h->peers[pi].connected) return;
    Peer* p = &h->peers[pi];
    // the packet's own sequence: a duplicate or one too late is dropped
    if (!p->got_any) {
        p->got_any = true;
        p->remote_seq = seq;
        p->ack_bits = 0;
    } else if (seq_newer(seq, p->remote_seq)) {
        uint16_t d = (uint16_t)(seq - p->remote_seq);
        p->ack_bits = d >= 32 ? (d == 32 ? 1u << 31 : 0u) : (p->ack_bits << d) | (1u << (d - 1));
        p->remote_seq = seq;
    } else {
        uint16_t d = (uint16_t)(p->remote_seq - seq);
        if (d == 0 || d > 32 || (p->ack_bits >> (d - 1) & 1u)) return;
        p->ack_bits |= 1u << (d - 1);
    }
    p->last_recv = h->now;
    p->n_recv++;
    p->bytes_in += len;
    if (len > HEADER) p->ack_due = true;
    if (has_acks) on_acks(h, p, ack, bits);
    while (r.at + 3 <= r.len) {
        uint8_t ch = nr_u8(&r);
        uint16_t mlen = nr_u16(&r);
        uint16_t id = ch == NET_UNRELIABLE ? 0 : nr_u16(&r);
        if (r.at + mlen > r.len || mlen > NET_MAX_MESSAGE) break;
        const uint8_t* d = b + r.at;
        r.at += mlen;
        if (ch == NET_RELIABLE) deliver_reliable(h, pi, p, id, d, mlen);
        else if (ch == NET_SEQUENCED) {
            if (p->sq_any && !seq_newer(id, p->sq_in)) {
                p->n_dropped++;
                continue;
            }
            p->sq_any = true, p->sq_in = id;
            push_event(h, NET_EV_MESSAGE, pi, NET_SEQUENCED, 0, d, mlen, NULL);
        } else {
            push_event(h, NET_EV_MESSAGE, pi, NET_UNRELIABLE, 0, d, mlen, NULL);
        }
    }
}

void net_update(NetHost* h, double now) {
    h->now = now;
    if (h->ev_at >= h->nev) h->nev = h->ev_at = 0, h->ev_bytes_len = 0; // (the last update's events were all taken)
    if (h->simq) sim_pump(h);
    uint8_t buf[2048];
    NetAddr from;
    for (int n = 0; n < 4096; n++) {
        int len = h->t.recv(h->t.ctx, &from, buf, sizeof buf);
        if (len <= 0) break;
        on_packet(h, &from, buf, len);
    }
    for (int i = 0; i < h->max_peers; i++) {
        Peer* p = &h->peers[i];
        if (!p->used) continue;
        if (p->created < 0.0) p->created = p->last_recv = p->win_t = p->kbps_t = now;
        if (!p->connected) { // a client trying: CONNECT again until the ACCEPT comes (or 10 s)
            if (now - p->created > TIMEOUT_S) {
                peer_free(p);
                push_event(h, NET_EV_DISCONNECTED, i, 0, NET_REASON_TIMEOUT, NULL, 0, NULL);
            } else if (now - p->last_try > CONNECT_RETRY_S) {
                uint8_t body[4];
                NetWriter w = { body, 0, 4, false };
                nw_u32(&w, h->protocol);
                send_ctrl(h, &p->addr, PK_CONNECT, body, w.len);
                p->last_try = now;
            }
            continue;
        }
        if (now - p->last_recv > TIMEOUT_S) {
            peer_free(p);
            push_event(h, NET_EV_DISCONNECTED, i, 0, NET_REASON_TIMEOUT, NULL, 0, NULL);
            continue;
        }
        // each sent packet judged once it is 3 round trips old (half a second at least): acked, or lost
        double judge = fmax(0.5, p->srtt * 0.003);
        for (int k = 0; k < 4096 && p->loss_scan != p->seq; k++) {
            const SentPkt* sp = &p->sent[p->loss_scan % SEQ_BUF];
            if (sp->used && sp->seq == p->loss_scan) {
                if (now - sp->time < judge) break;
                p->win_sent++;
                if (!sp->acked) p->win_lost++;
            }
            p->loss_scan++;
        }
        // the loss over the last second: the packets sent then that no ack has covered
        if (now - p->win_t >= 1.0) {
            float l = p->win_sent ? (float)p->win_lost / (float)p->win_sent : 0.0f;
            p->loss = p->loss * 0.5f + l * 0.5f;
            p->win_sent = p->win_lost = 0;
            p->win_t = now;
        }
        if (now - p->kbps_t >= 0.5) {
            double dt = now - p->kbps_t;
            p->kbps_out = (float)(p->bytes_out * 8.0 / 1000.0 / dt);
            p->kbps_in = (float)(p->bytes_in * 8.0 / 1000.0 / dt);
            p->bytes_out = p->bytes_in = 0.0;
            p->kbps_t = now;
        }
    }
}

bool net_poll(NetHost* h, NetEvent* ev) {
    if (h->ev_at >= h->nev) return false;
    *ev = h->ev[h->ev_at++];
    return true;
}

// one packet of a peer: its header (acks), the reliable messages due (new or not acked in time), then what fits of the
// unreliable ones
static bool flush_packet(NetHost* h, Peer* p, int* unrel_at, bool keepalive) {
    uint8_t b[NET_MTU];
    NetWriter w = { b, 0, NET_MTU, false };
    uint16_t seq = p->seq;
    nw_u16(&w, h->tag);
    nw_u8(&w, (uint8_t)(PK_DATA | (p->got_any ? 0x80 : 0))); // (acks only once something came: an ack of "0" before that acked a lost packet)
    nw_u16(&w, seq);
    nw_u16(&w, p->remote_seq);
    nw_u32(&w, p->ack_bits);
    SentPkt* sp = &p->sent[seq % SEQ_BUF];
    sp->used = 1, sp->acked = 0, sp->seq = seq, sp->time = h->now, sp->nrel = 0, sp->ntok = 0;
    // reliable: resent after the round trip (and its jitter) without an ack
    double rto = fmax(0.03, (p->srtt > 0.0f ? p->srtt * 1.25f + p->jitter * 2.0f : 100.0f) * 0.001);
    int body = 0;
    // (only within the receiver's hold window from the oldest unacked: a message past it would be dropped there while its
    // packet got acked: never resent, the channel stuck)
    for (uint16_t id = p->rel_oldest; id != p->rel_next && sp->nrel < REL_PER_PACKET && (uint16_t)(id - p->rel_oldest) < RECV_HOLD; id++) {
        RelMsg* m = &p->rel[id % REL_RING];
        if (m->acked || (m->last_sent >= 0.0 && h->now - m->last_sent < rto)) continue;
        if (w.len + 5 + m->len > NET_MTU) break;
        nw_u8(&w, NET_RELIABLE);
        nw_u16(&w, m->len);
        nw_u16(&w, m->id);
        for (int i = 0; i < m->len; i++) b[w.len + i] = p->rel_bytes[(m->off + (uint32_t)i) % REL_BYTES];
        w.len += m->len;
        if (m->sent_count) p->n_resent++;
        m->sent_count++;
        m->last_sent = h->now;
        sp->rel[sp->nrel++] = m->id;
        body++;
    }
    // unreliable: whole records while they fit
    while (*unrel_at < p->unrel_len) {
        NetReader r = { p->unrel + *unrel_at, p->unrel_len - *unrel_at, 0, false };
        uint8_t ch = nr_u8(&r);
        uint16_t mlen = nr_u16(&r);
        int rec = 3 + (ch == NET_SEQUENCED ? 2 : 0) + mlen;
        if (w.len + rec > NET_MTU) break;
        for (int k = 0; k < p->nutok && sp->ntok < TOK_PER_PACKET; k++)
            if (p->utok[k].at == *unrel_at) sp->tok[sp->ntok++] = p->utok[k].tok;
        nw_bytes(&w, p->unrel + *unrel_at, rec);
        *unrel_at += rec;
        body++;
    }
    if (!body && !keepalive) {
        sp->used = 0;
        return false;
    }
    p->seq++;
    p->n_sent++;
    p->bytes_out += w.len;
    p->last_send = h->now;
    p->ack_due = false;
    raw_send(h, &p->addr, b, w.len);
    return body > 0;
}

void net_flush(NetHost* h, double now) {
    h->now = now;
    for (int i = 0; i < h->max_peers; i++) {
        Peer* p = &h->peers[i];
        if (!p->used || !p->connected) continue;
        int at = 0;
        bool any = false;
        for (int k = 0; k < 16; k++) { // (at most 16 packets a peer a flush)
            bool more = flush_packet(h, p, &at, !any && (p->ack_due || now - p->last_send > KEEPALIVE_S));
            if (!more) break;
            any = true;
        }
        p->unrel_len = 0, p->nutok = 0; // (what didn't fit is dropped: unreliable)
    }
    if (h->simq) sim_pump(h);
}

bool net_connected(const NetHost* h, int peer) { return peer >= 0 && peer < h->max_peers && h->peers[peer].used && h->peers[peer].connected; }
int net_peer_count(const NetHost* h) {
    int n = 0;
    for (int i = 0; i < h->max_peers; i++) n += h->peers[i].used && h->peers[i].connected;
    return n;
}
NetStats net_stats(const NetHost* h, int peer) {
    NetStats s = { 0 };
    if (peer < 0 || peer >= h->max_peers || !h->peers[peer].used) return s;
    const Peer* p = &h->peers[peer];
    s.rtt_ms = p->srtt, s.jitter_ms = p->jitter, s.loss = p->loss, s.kbps_out = p->kbps_out, s.kbps_in = p->kbps_in;
    s.sent = p->n_sent, s.received = p->n_recv, s.resent = p->n_resent, s.dropped_old = p->n_dropped;
    s.pending_reliable = (uint16_t)(p->rel_next - p->rel_oldest);
    return s;
}
NetAddr net_peer_addr(const NetHost* h, int peer) {
    NetAddr a = { 0 };
    if (peer >= 0 && peer < h->max_peers && h->peers[peer].used) a = h->peers[peer].addr;
    return a;
}
bool net_is_server(const NetHost* h) { return h->server; }
void net_set_seed(NetHost* h, uint32_t seed) { h->rng = seed * 2654435761u + 12345u; } // (the emulator's dice: the tests repeat)
