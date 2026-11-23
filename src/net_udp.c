// The network layer's transports (net.h): UDP over Winsock and an in-process loopback for the tests.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#define NET_WOULD_IGNORE(e) ((e) == WSAECONNRESET || (e) == WSAEMSGSIZE)
#define net_errno() WSAGetLastError()
typedef int sock_len;
#else // Android, POSIX: BSD sockets under Winsock's names
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define NET_WOULD_IGNORE(e) ((e) == ECONNREFUSED || (e) == EMSGSIZE)
#define net_errno() errno
typedef socklen_t sock_len;
#endif

#include <stdlib.h>
#include <string.h>

#include "net.h"

typedef struct Udp { SOCKET s; uint16_t port; } Udp;

static bool udp_send(void* ctx, const NetAddr* to, const void* data, int len) {
    Udp* u = ctx;
    if (to->kind != NET_ADDR_IP) return false;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(to->port);
    a.sin_addr.s_addr = htonl(to->ip);
    return sendto(u->s, (const char*)data, len, 0, (struct sockaddr*)&a, sizeof a) == len;
}

static int udp_recv(void* ctx, NetAddr* from, void* buf, int cap) {
    Udp* u = ctx;
    for (;;) {
        struct sockaddr_in a;
        sock_len al = sizeof a;
        int n = (int)recvfrom(u->s, (char*)buf, cap, 0, (struct sockaddr*)&a, &al);
        if (n == SOCKET_ERROR) {
            int e = net_errno();
            if (NET_WOULD_IGNORE(e)) continue; // (an ICMP "port unreachable" from an old send; a giant)
            return 0;
        }
        memset(from, 0, sizeof *from);
        from->kind = NET_ADDR_IP;
        from->ip = ntohl(a.sin_addr.s_addr);
        from->port = ntohs(a.sin_port);
        return n;
    }
}

static void udp_close(void* ctx) {
    Udp* u = ctx;
    closesocket(u->s);
    free(u);
}

bool net_udp_open(NetTransport* t, uint16_t port, bool broadcast) {
#ifdef _WIN32
    static bool started;
    if (!started) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        started = true;
    }
#endif
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL); // (no WSAECONNRESET storms)
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&big, sizeof big);
    if (broadcast) {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    }
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    sock_len al = sizeof a;
    getsockname(s, (struct sockaddr*)&a, &al);
    Udp* u = calloc(1, sizeof *u);
    if (!u) {
        closesocket(s);
        return false;
    }
    u->s = s;
    u->port = ntohs(a.sin_port);
    *t = (NetTransport){ udp_send, udp_recv, udp_close, u };
    return true;
}

uint16_t net_udp_port(const NetTransport* t) { return t && t->send == udp_send ? ((Udp*)t->ctx)->port : 0; }

// --- loopback: endpoints in this process (the tests), a ring of packets each -------------------------------------------
#define LOOP_MAX 8
#define LOOP_RING 2048
typedef struct LoopPkt { uint64_t from; int len; uint8_t data[NET_MTU]; } LoopPkt;
typedef struct Loop { uint64_t id; LoopPkt* ring; volatile long head, tail; } Loop;
static Loop* g_loops[LOOP_MAX];

static Loop* loop_find(uint64_t id) {
    for (int i = 0; i < LOOP_MAX; i++)
        if (g_loops[i] && g_loops[i]->id == id) return g_loops[i];
    return NULL;
}

static bool loop_send(void* ctx, const NetAddr* to, const void* data, int len) {
    Loop* me = ctx;
    if (to->kind != NET_ADDR_LOOP || len > NET_MTU) return false;
    Loop* l = loop_find(to->id);
    if (!l || l->head - l->tail >= LOOP_RING) return false;
    LoopPkt* p = &l->ring[l->head % LOOP_RING];
    p->from = me->id, p->len = len;
    memcpy(p->data, data, (size_t)len);
    l->head++;
    return true;
}

static int loop_recv(void* ctx, NetAddr* from, void* buf, int cap) {
    Loop* l = ctx;
    if (l->tail == l->head) return 0;
    LoopPkt* p = &l->ring[l->tail % LOOP_RING];
    int n = p->len < cap ? p->len : cap;
    memcpy(buf, p->data, (size_t)n);
    memset(from, 0, sizeof *from);
    from->kind = NET_ADDR_LOOP, from->id = p->from;
    l->tail++;
    return n;
}

static void loop_close(void* ctx) {
    Loop* l = ctx;
    for (int i = 0; i < LOOP_MAX; i++)
        if (g_loops[i] == l) g_loops[i] = NULL;
    free(l->ring);
    free(l);
}

bool net_loop_open(NetTransport* t, uint64_t endpoint) {
    for (int i = 0; i < LOOP_MAX; i++)
        if (!g_loops[i]) {
            Loop* l = calloc(1, sizeof *l);
            if (!l) return false;
            l->ring = malloc(sizeof(LoopPkt) * LOOP_RING);
            if (!l->ring) {
                free(l);
                return false;
            }
            l->id = endpoint;
            g_loops[i] = l;
            *t = (NetTransport){ loop_send, loop_recv, loop_close, l };
            return true;
        }
    return false;
}
