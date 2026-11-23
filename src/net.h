// The network layer (notas-dev/online.md): an own protocol over unreliable datagrams, the same on every transport (UDP
// sockets, Steam's relayed P2P messages, an in-process loopback for the tests). Built to hold up under lag and packet
// loss, as games need:
//   * every packet acks the last 33 it got (sequence + a 32-bit field): no ack packets, acks ride along with the data;
//   * channel RELIABLE: ordered and guaranteed (resent until acked, at the round trip's pace; delivered in order);
//   * channel UNRELIABLE: the latest state (snapshots, positions): never resent, the newer one replaces a lost one;
//   * channel SEQUENCED: unreliable, but an older message than the last delivered is dropped (no going back);
//   * per connection: the round trip (smoothed), its jitter, the loss, the bandwidth in and out;
//   * a network simulator (latency, jitter, loss, duplicates) on any transport: the developer tests the bad cases.
// No malloc per frame: fixed rings per peer, messages packed into packets of at most NET_MTU bytes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NET_MTU 1200          // bytes a packet (fits any path, Steam's relay too)
#define NET_MAX_PEERS 64      // connections a host holds (a listen server's clients)
#define NET_MAX_MESSAGE 1100  // bytes a message (larger: split by the caller)

typedef enum NetChannel { NET_RELIABLE = 0, NET_UNRELIABLE = 1, NET_SEQUENCED = 2 } NetChannel;

typedef enum NetAddrKind { NET_ADDR_NONE = 0, NET_ADDR_IP, NET_ADDR_STEAM, NET_ADDR_LOOP } NetAddrKind;
typedef struct NetAddr {
    uint8_t kind;
    uint16_t port;      // IP: host order
    uint32_t ip;        // IP: host order (127.0.0.1 = 0x7F000001)
    uint64_t id;        // Steam: the SteamID; loopback: the endpoint
} NetAddr;
bool net_addr_equal(const NetAddr* a, const NetAddr* b);
bool net_addr_parse(const char* s, uint16_t default_port, NetAddr* out); // "1.2.3.4:7777", "localhost"
void net_addr_text(const NetAddr* a, char* out, int cap);

// A transport: sends and receives datagrams (non-blocking)
typedef struct NetTransport {
    bool (*send)(void* ctx, const NetAddr* to, const void* data, int len);
    int (*recv)(void* ctx, NetAddr* from, void* buf, int cap); // bytes, 0: none waiting
    void (*close)(void* ctx);
    void* ctx;
} NetTransport;

// UDP (Winsock): bound to port (0: any); LAN discovery by broadcast on the same port
bool net_udp_open(NetTransport* t, uint16_t port, bool broadcast);
uint16_t net_udp_port(const NetTransport* t);
// In-process loopback (the tests): endpoint ids 1.. ; what one sends the other receives
bool net_loop_open(NetTransport* t, uint64_t endpoint);

// The network simulator: applied to what this host sends
typedef struct NetSim {
    float latency_ms;   // one way
    float jitter_ms;    // +- uniform
    float loss;         // 0..1
    float duplicate;    // 0..1
} NetSim;

typedef struct NetStats {
    float rtt_ms, jitter_ms;   // smoothed round trip and its variation
    float loss;                // 0..1 over the last second
    float kbps_out, kbps_in;   // kilobits per second
    uint32_t sent, received, resent, dropped_old; // packets, reliable messages resent, sequenced messages dropped
    uint32_t pending_reliable; // reliable messages not yet acked
} NetStats;

typedef enum NetEventType { NET_EV_NONE = 0, NET_EV_CONNECTED, NET_EV_DISCONNECTED, NET_EV_MESSAGE, NET_EV_DISCOVERED } NetEventType;
typedef enum NetReason { NET_REASON_NONE = 0, NET_REASON_TIMEOUT, NET_REASON_CLOSED, NET_REASON_FULL, NET_REASON_REFUSED, NET_REASON_VERSION } NetReason;
typedef struct NetEvent {
    NetEventType type;
    int peer;            // the connection (0..NET_MAX_PEERS-1); a client's server is peer 0
    uint8_t channel;
    uint8_t reason;      // NetReason (disconnected)
    uint16_t len;
    const uint8_t* data; // valid until the next net_poll
    NetAddr addr;        // (discovered: the server; its info in data)
} NetEvent;

typedef struct NetHost NetHost;
// server: accepts up to max_peers; a client connects to one server. protocol: the game's version (mismatch: refused)
NetHost* net_host_create(const NetTransport* t, bool server, int max_peers, uint32_t protocol);
void net_host_destroy(NetHost* h); // (closes every connection politely, then the transport)
void net_set_sim(NetHost* h, const NetSim* sim);
void net_set_seed(NetHost* h, uint32_t seed); // the emulator's random numbers from a seed (the tests repeat exactly)
void net_set_info(NetHost* h, const void* data, int len); // a server's answer to LAN discovery (name, map, players)
int net_connect(NetHost* h, const NetAddr* server);          // a client: its peer (0), -1 on failure
void net_disconnect(NetHost* h, int peer);
bool net_discover(NetHost* h, uint16_t port);                // a client: asks the LAN (answers: NET_EV_DISCOVERED)
// queues a message (copied); false when its ring is full (reliable: the peer is too far behind)
bool net_send(NetHost* h, int peer, NetChannel ch, const void* data, int len);
void net_broadcast(NetHost* h, NetChannel ch, const void* data, int len, int except);
// An unreliable / sequenced message whose arrival the sender learns: its token (0: not queued), given back by
// net_take_acked once the packet that carried it is acked (a lost one never comes back). The replication's baseline
// (net_repl.h): what the client surely has. (A sequenced one may still have been dropped there as older than one
// already delivered: the baseline uses the unreliable channel and orders by time itself)
uint32_t net_send_tracked(NetHost* h, int peer, NetChannel ch, const void* data, int len);
int net_take_acked(NetHost* h, int peer, uint32_t* out, int cap);
// receives, resends, keeps alive, times out (now: seconds); then net_poll for the events
void net_update(NetHost* h, double now);
bool net_poll(NetHost* h, NetEvent* ev);
// packs the queued messages into packets and sends them (once per network tick)
void net_flush(NetHost* h, double now);
bool net_connected(const NetHost* h, int peer);
int net_peer_count(const NetHost* h);
NetStats net_stats(const NetHost* h, int peer);
NetAddr net_peer_addr(const NetHost* h, int peer);
bool net_is_server(const NetHost* h);

// Little-endian writers and readers for the messages (bounds-checked: a short message reads zeros, never past it)
typedef struct NetWriter { uint8_t* p; int len, cap; bool overflow; } NetWriter;
typedef struct NetReader { const uint8_t* p; int len, at; bool overflow; } NetReader;
void nw_u8(NetWriter* w, uint8_t v);
void nw_u16(NetWriter* w, uint16_t v);
void nw_u32(NetWriter* w, uint32_t v);
void nw_u64(NetWriter* w, uint64_t v);
void nw_f32(NetWriter* w, float v);
void nw_str(NetWriter* w, const char* s); // length-prefixed (u8), at most 255 bytes
void nw_bytes(NetWriter* w, const void* d, int n);
uint8_t nr_u8(NetReader* r);
uint16_t nr_u16(NetReader* r);
uint32_t nr_u32(NetReader* r);
uint64_t nr_u64(NetReader* r);
float nr_f32(NetReader* r);
void nr_str(NetReader* r, char* out, int cap);
void nr_bytes(NetReader* r, void* d, int n);
// Quantized: a position (cm, 1/8 cm steps in +-2^20 cm: 3 x 24 bits), an angle (degrees, 16 bits)
void nw_pos(NetWriter* w, const float p[3]);
void nr_pos(NetReader* r, float p[3]);
void nw_angle(NetWriter* w, float deg);
float nr_angle(NetReader* r);
