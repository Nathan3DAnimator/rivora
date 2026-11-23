// Online play (online.h): the session, the players, the replication over net.h, Steam (steam.h), the saves.
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#endif

#include "online.h"
#include "save_progress.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_files.h"
#include <ctype.h>

#include "blueprint.h"
#include "components.h"
#include "platform.h"
#include "project.h"
#include "inventory.h"
#include "dialogue.h"
#include "steam.h"
#include "level.h"
#include "debug_viewer.h"
#include "net_repl.h"
#include "game_rules.h"

#define MAX_REP 4096          // replicated actors
#define HIST 16               // states held per actor (the interpolation buffer)
#define DEFAULT_PORT 7777

enum { M_HELLO = 1, M_WELCOME, M_PLAYER, M_SPAWN, M_DESTROY, M_POSSESS, M_SNAP, M_OWNER, M_RPC, M_VAR, M_CHAT, M_KICK, M_CORRECT, M_TRAVEL,
       M_SESSION, // (M_SESSION: the session's settings and state, server -> clients)
       M_RULES, // (M_RULES: the Game Rules' match, server -> clients: game_rules.h)
       M_MOVE, M_MOVE_ACK }; // (B13: a client's moves of its Character, the server's applied one and the state it led to)
#define MAX_FOUND 64           // sessions a search holds (before its filters and Max Results)
enum { REP_MOVE = 1, REP_LEVEL = 2, REP_SPAWNED = 4 };

typedef struct State { double t; float pos[3], rot[3], vel[3]; } State;
typedef struct Rep {
    Entity e;
    uint32_t id;
    uint8_t flags;
    int8_t owner;              // the player who owns it (-1: the server)
    uint32_t cls;              // spawned: its class
    char name[RE_NAME_MAX];    // a level actor: its name (both sides have it)
    State hist[HIST];          // (a client: the server's states)
    int nhist, head;
    float shown[3], err[3];    // (a client: where it was drawn, the jump still being smoothed away)
    bool shown_ok;
    double last_owner_t;       // (the server: the owner's last state)
} Rep;

typedef struct Player { bool used; char name[64]; uint64_t steam; int peer; Entity pawn; uint32_t pawn_id; } Player;

static struct {
    OnlineSettings set;
    OnlineRole role;
    NetTransport tr;
    NetHost* host;
    bool steam_session, lan;
    Player players[ONLINE_MAX_PLAYERS];
    int local;                 // the local player's index (-1: none, a dedicated server)
    Rep reps[MAX_REP];
    int nreps;
    uint32_t next_id;
    // the server (net_repl.h): per player, per actor, what was sent and what the client confirmed (made when the player
    // arrives), which actors each tracked snapshot carried, where the snapshot round-robin starts (the budget fair to
    // all), each actor's last second (lag compensation) and each player's view delay (how far back they see)
    NetReplSlot* slots[ONLINE_MAX_PLAYERS];
    NetReplLog* logs[ONLINE_MAX_PLAYERS];
    int rr[ONLINE_MAX_PLAYERS];
    float view_delay_ms[ONLINE_MAX_PLAYERS];
    NetReplHist* hist;         // [MAX_REP]
    uint32_t resends;          // states sent again because the client hadn't confirmed them
    // a client: the interpolation's smooth clock, the delay it draws behind, the newest snapshot's time
    double play_t, last_interp, last_snap_st, delay_s;
    double tick_at, now, server_dt; // server time offset (client): server time = now + offset
    bool have_offset;
    float jitter_ms;
    uint32_t snapshots, corrections, refused; // (refused: players turned away at the door, the server)
    OnlineSession sessions[MAX_FOUND];
    int nsessions;
    double find_until, find_sent;
    bool finding, joining, kicked;
    // the advanced sessions: the session's settings (the host's; a client's copy without the password), its state, the
    // search being made, what the next create / search take, the bans, the join's password and invites
    OnlineSessionSettings cur;
    OnlineSessionState state;
    uint32_t session_id;
    OnlineSearch search;
    OnlineProp staged[ONLINE_MAX_PROPS];
    int nstaged;
    OnlineFilter filters[ONLINE_MAX_FILTERS];
    int nfilters;
    char last_error[128];
    struct { uint64_t steam; NetAddr addr; char name[64], reason[64]; } bans[ONLINE_MAX_BANS];
    int nbans;
    uint64_t join_pw;
    bool join_invite, no_auto_invites;
    uint64_t pending_invite;
    OnlineEvent ev[64];
    int nev, ev_at;
    // the last RPC's values
    float rpc_f, rpc_v[3];
    char rpc_text[128];
    int rpc_sender;
    World* w;
} O;

void online_configure(const OnlineSettings* s) {
    O.set = *s;
    if (!O.set.port) O.set.port = DEFAULT_PORT;
    if (O.set.max_players <= 0) O.set.max_players = 16;
    if (O.set.tick_rate <= 0) O.set.tick_rate = 30;
    if (O.set.interp_ms <= 0.0f) O.set.interp_ms = 100.0f;
}
static void ensure_defaults(void) { // (no project open: the defaults, port 7777)
    if (!O.set.port) {
        OnlineSettings d = { 0 };
        online_configure(&d);
    }
}
const OnlineSettings* online_settings(void) {
    ensure_defaults();
    return &O.set;
}

void online_init(void) {
    if (!O.set.port) {
        OnlineSettings d = { 0 };
        online_configure(&d);
    }
    O.local = -1;
    if (O.set.use_steam) steam_init(O.set.steam_app_id);
}
void online_shutdown(void) {
    if (O.host) net_host_destroy(O.host), O.host = NULL;
    steam_shutdown();
}

static void event(OnlineEventType t, int i, bool ok, const char* text) {
    // (the log)
    if (t == ONLINE_EV_ERROR || ((t == ONLINE_EV_JOINED || t == ONLINE_EV_SESSION_CREATED) && !ok))
        printf("[RealEngine] Online: %s\n", text && text[0] ? text : "failed");
    else if (t == ONLINE_EV_PLAYER_JOINED || t == ONLINE_EV_PLAYER_LEFT || t == ONLINE_EV_JOINED)
        printf("[RealEngine] Online: %s %d %s\n", t == ONLINE_EV_PLAYER_LEFT ? "left" : t == ONLINE_EV_JOINED ? "joined as player" : "player joined", i, text ? text : "");
    if (O.nev >= 64) return;
    OnlineEvent* e = &O.ev[O.nev++];
    e->type = t, e->i = i, e->ok = ok;
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
}
bool online_poll(OnlineEvent* ev) {
    if (O.ev_at >= O.nev) {
        O.nev = O.ev_at = 0;
        return false;
    }
    *ev = O.ev[O.ev_at++];
    return true;
}

OnlineRole online_role(void) { return O.role; }
bool online_active(void) { return O.role != ONLINE_STANDALONE && O.host; }
static bool is_server(void) { return O.role == ONLINE_LISTEN || O.role == ONLINE_DEDICATED; }

// --- advanced sessions: settings, properties, filters (online.h) --------------------------------------------------------
void online_settings_default(OnlineSessionSettings* st) {
    memset(st, 0, sizeof *st);
    snprintf(st->name, sizeof st->name, "Game");
    st->max_players = 4;
    st->advertise = st->allow_join_in_progress = st->allow_invites = st->joinable = true;
}
static void set_error(const char* why) { snprintf(O.last_error, sizeof O.last_error, "%s", why ? why : ""); }
const char* online_last_error(void) { return O.last_error; }
// The password as the clients send it (FNV-1a 64; 0: none): the text never travels
static uint64_t pw_hash(const char* pw) {
    if (!pw || !pw[0]) return 0;
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char* c = (const unsigned char*)pw; *c; c++) h ^= *c, h *= 1099511628211ull;
    return h ? h : 1;
}
// The game's build: the project's name and version (Same Build Only, the join's check); 0 without a project
static uint32_t game_build(void) {
    const ProjectSettings* pr = project();
    if (!pr || !pr->name[0]) return 0;
    uint32_t h = 2166136261u;
    for (const char* c = pr->name; *c; c++) h ^= (uint8_t)*c, h *= 16777619u;
    h ^= '/', h *= 16777619u;
    for (const char* c = pr->version; *c; c++) h ^= (uint8_t)*c, h *= 16777619u;
    return h ? h : 1;
}
static OnlineProp* prop_find(OnlineProp* props, int n, const char* key) {
    for (int i = 0; i < n; i++)
        if (!_stricmp(props[i].key, key)) return &props[i];
    return NULL;
}
static bool prop_set(OnlineProp* props, int* n, const char* key, const char* value) {
    if (!key || !key[0]) return false;
    OnlineProp* pr = prop_find(props, *n, key);
    if (!pr) {
        if (*n >= ONLINE_MAX_PROPS) return false;
        pr = &props[(*n)++];
        snprintf(pr->key, sizeof pr->key, "%s", key);
    }
    snprintf(pr->value, sizeof pr->value, "%s", value ? value : "");
    return true;
}
static bool prop_remove(OnlineProp* props, int* n, const char* key) {
    OnlineProp* pr = key ? prop_find(props, *n, key) : NULL;
    if (!pr) return false;
    *pr = props[--(*n)];
    return true;
}
static int used_players(void) {
    int n = 0;
    for (int i = 0; i < ONLINE_MAX_PLAYERS; i++) n += O.players[i].used;
    return n;
}
// Who may still come in: locked by the host, or the match started without Allow Join In Progress
static bool open_to_joins(void) { return O.cur.joinable && !(O.state == ONLINE_STATE_IN_PROGRESS && !O.cur.allow_join_in_progress); }
static void session_state_set(OnlineSessionState st) {
    if (O.state == st) return;
    O.state = st;
    event(ONLINE_EV_SESSION_STATE, (int)st, true, O.cur.name);
}

// --- replicated actors -------------------------------------------------------------------------------------------------
static Rep* rep_of(Entity e) {
    for (int i = 0; i < O.nreps; i++)
        if (O.reps[i].e == e) return &O.reps[i];
    return NULL;
}
static Rep* rep_id(uint32_t id) {
    for (int i = 0; i < O.nreps; i++)
        if (O.reps[i].id == id) return &O.reps[i];
    return NULL;
}
static Rep* rep_add(Entity e, uint32_t id, uint8_t flags, int owner) {
    if (O.nreps >= MAX_REP) return NULL;
    Rep* r = &O.reps[O.nreps++];
    memset(r, 0, sizeof *r);
    r->e = e, r->id = id, r->flags = flags, r->owner = (int8_t)owner;
    return r;
}
static void rep_remove(Rep* r) { // (the last takes its place: its server-side records move with it)
    int i = (int)(r - O.reps), last = --O.nreps;
    *r = O.reps[last];
    for (int p = 0; p < ONLINE_MAX_PLAYERS; p++)
        if (O.slots[p]) O.slots[p][i] = O.slots[p][last], net_repl_reset(&O.slots[p][last]);
    if (O.hist) O.hist[i] = O.hist[last], memset(&O.hist[last], 0, sizeof O.hist[last]);
}

static const char* name_of(World* w, Entity e) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    return n ? n->text : "";
}
static Entity by_name(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ents[i];
    }
    return ENTITY_NONE;
}

// The level's actors that replicate by themselves: Characters and Simulate Physics bodies (by their names: the same
// level on both sides). The server numbers them; a client maps the numbers to its own entities by name.
static void collect_level_reps(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        bool chr = (a->mask & COMP_BIT(COMP_CHARACTER)) != 0, body = (a->mask & COMP_BIT(COMP_RIGID_BODY)) != 0;
        if (!chr && !body) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CRigidBody* rb = body ? ecs_arch_column(a, COMP_RIGID_BODY) : NULL;
        for (uint32_t i = 0; i < a->count; i++) {
            if (!chr && !rb[i].simulate) continue;
            if (rep_of(ents[i])) continue;
            Rep* r = rep_add(ents[i], ++O.next_id, REP_MOVE | REP_LEVEL, -1);
            if (r) snprintf(r->name, sizeof r->name, "%s", name_of(w, ents[i]));
        }
    }
}

static void get_state(World* w, Entity e, float pos[3], float rot[3], float vel[3]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    memset(vel, 0, 3 * sizeof(float));
    if (t) memcpy(pos, t->location, 3 * sizeof(float)), memcpy(rot, t->rotation, 3 * sizeof(float));
    if (c) memcpy(vel, c->velocity, 3 * sizeof(float));
}
static void set_state(World* w, Entity e, const float pos[3], const float rot[3], const float vel[3]) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    if (!t) return;
    memcpy(t->location, pos, 3 * sizeof(float));
    memcpy(t->rotation, rot, 3 * sizeof(float));
    if (c) memcpy(c->velocity, vel, 3 * sizeof(float)), c->control_yaw = rot[1];
    ecs_touch(w, e);
}

// --- (B13) the prediction: the own Character moves at once here (its input, jump and frame recorded with where it led),
// the moves go to the server (each packet the ones not yet confirmed: a lost packet costs nothing), the server moves the
// pawn with them (its authority: speed, collisions, jumps as its world has them) and answers with the last one it
// applied and the state; a state off by more than PRED_TOL is taken, then the moves after it replayed (reconciliation) ---
#define PRED_RING 256
#define PRED_TOL 2.0f // cm
typedef struct PredMove { uint32_t seq; float dt, in[2]; uint8_t jump; float pos[3], vel[3]; } PredMove;
static struct {
    PredMove m[PRED_RING];
    uint32_t next, acked;          // the next move's number; the last the server confirmed
    Entity pawn;
    uint32_t last_applied[16];     // (the server) each player's last move applied
    uint32_t acks, corrections, replayed;
    float max_err, sum_err;
} P;
static void pred_record(World* w, Entity e, const float input[2], bool jump, float dt);
static void get_state(World* w, Entity e, float pos[3], float rot[3], float vel[3]);
static void set_state(World* w, Entity e, const float pos[3], const float rot[3], const float vel[3]);

// (B13) the possessed Character's move this tick (bp_vm.c's hook): numbered, with where it led (a client's own pawn only)
static void pred_record(World* w, Entity e, const float input[2], bool jump, float dt) {
    if (O.role != ONLINE_CLIENT || O.local < 0 || e != O.players[O.local].pawn) return;
    if (P.pawn != e) P.pawn = e, P.next = P.acked + 1; // (a new pawn: its moves start after the last confirmed)
    if (!P.next) P.next = 1;
    PredMove* q = &P.m[P.next % PRED_RING];
    q->seq = P.next++, q->dt = dt, q->in[0] = input[0], q->in[1] = input[1], q->jump = jump ? 1 : 0;
    float rot[3];
    get_state(w, e, q->pos, rot, q->vel);
}

// (net_repl.h's compact state: 12 bytes still, 18 walking)
static void w_state(NetWriter* nw, const float pos[3], const float rot[3], const float vel[3]) { net_repl_write_state(nw, pos, rot, vel); }
static void r_state(NetReader* nr, float pos[3], float rot[3], float vel[3]) { net_repl_read_state(nr, pos, rot, vel); }

// --- players -----------------------------------------------------------------------------------------------------------
static int player_of_peer(int peer) {
    for (int i = 0; i < ONLINE_MAX_PLAYERS; i++)
        if (O.players[i].used && O.players[i].peer == peer) return i;
    return -1;
}
static int player_new(const char* name, uint64_t steam, int peer) {
    for (int i = 0; i < ONLINE_MAX_PLAYERS; i++)
        if (!O.players[i].used) {
            memset(&O.players[i], 0, sizeof O.players[i]);
            O.players[i].used = true, O.players[i].steam = steam, O.players[i].peer = peer;
            if (i < 16) P.last_applied[i] = 0; // (B13: a new player's moves from 1)
            snprintf(O.players[i].name, sizeof O.players[i].name, "%s", name && name[0] ? name : "Player");
            return i;
        }
    return -1;
}
static const char* local_name(void) {
    if (steam_available() && steam_user_name()[0]) return steam_user_name();
    static char n[64];
#ifdef _WIN32
    DWORD sz = sizeof n;
    if (!n[0] && !GetUserNameA(n, &sz)) snprintf(n, sizeof n, "Player");
#else
    if (!n[0]) snprintf(n, sizeof n, "Player"); // (Android: no account name for an app)
#endif
    return n;
}

static void send_msg(int peer, NetChannel ch, const NetWriter* nw) {
    if (O.host && !nw->overflow) net_send(O.host, peer, ch, nw->p, nw->len);
}
static void broadcast(NetChannel ch, const NetWriter* nw, int except_peer) {
    if (O.host && !nw->overflow) net_broadcast(O.host, ch, nw->p, nw->len, except_peer);
}
#define MSG(name_) uint8_t name_##_b[NET_MAX_MESSAGE]; NetWriter name_ = { name_##_b, 0, NET_MAX_MESSAGE, false }
static uint32_t g_rules_serial; // (the Game Rules' state last sent, game_rules.h)
static double g_rules_at;

static void send_player(int to_peer, int p, bool joined) {
    MSG(m);
    nw_u8(&m, M_PLAYER), nw_u8(&m, (uint8_t)p), nw_u8(&m, joined), nw_str(&m, O.players[p].name), nw_u64(&m, O.players[p].steam);
    if (to_peer < 0) broadcast(NET_RELIABLE, &m, -1);
    else send_msg(to_peer, NET_RELIABLE, &m);
}
static void send_spawn(int to_peer, const Rep* r, World* w) {
    float pos[3], rot[3], vel[3];
    get_state(w, r->e, pos, rot, vel);
    MSG(m);
    nw_u8(&m, M_SPAWN), nw_u32(&m, r->id), nw_u32(&m, r->cls), nw_u8(&m, (uint8_t)(int8_t)r->owner);
    w_state(&m, pos, rot, vel);
    if (to_peer < 0) broadcast(NET_RELIABLE, &m, -1);
    else send_msg(to_peer, NET_RELIABLE, &m);
}

// The server: a player joined (a remote one): their pawn, a copy of the host's pawn class at the Player Start
static void spawn_pawn(World* w, int p) {
    Entity host_pawn = bp_player(w);
    const CBlueprint* b = host_pawn ? ecs_get(w, host_pawn, COMP_BLUEPRINT) : NULL;
    BpAsset* cls = b ? bp_find_hash(b->asset_hash) : NULL;
    if (!cls) return;
    float loc[3] = { 0, 0, 100 }, rot[3] = { 0, 0, 0 };
    const CTransform* ht = ecs_get(w, host_pawn, COMP_TRANSFORM);
    if (ht) memcpy(loc, ht->location, sizeof loc), rot[1] = ht->rotation[1];
    float a = (float)p * 2.39996f; // (around the start, apart)
    loc[0] += cosf(a) * 200.0f, loc[1] += sinf(a) * 200.0f, loc[2] += 20.0f;
    Entity e = bp_spawn(w, cls, loc, rot);
    if (!e) return;
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    if (c) c->auto_possess = 0;
    Rep* r = rep_add(e, ++O.next_id, REP_MOVE | REP_SPAWNED, p);
    if (!r) return;
    r->cls = b->asset_hash;
    O.players[p].pawn = e, O.players[p].pawn_id = r->id;
    send_spawn(-1, r, w);
    MSG(m);
    nw_u8(&m, M_POSSESS), nw_u32(&m, r->id);
    send_msg(O.players[p].peer, NET_RELIABLE, &m);
}

static void welcome(World* w, int peer, int p) {
    if (!O.slots[p]) O.slots[p] = calloc(MAX_REP, sizeof(NetReplSlot)); // (the player's arrival, not a frame)
    if (!O.logs[p]) O.logs[p] = calloc(1, sizeof(NetReplLog));
    if (O.slots[p]) memset(O.slots[p], 0, sizeof(NetReplSlot) * MAX_REP);
    if (O.logs[p]) memset(O.logs[p], 0, sizeof(NetReplLog));
    O.rr[p] = 0, O.view_delay_ms[p] = 100.0f;
    // the players, the level's replicated actors (name -> number), the spawned ones, then the new player to everyone
    MSG(m);
    nw_u8(&m, M_WELCOME), nw_u8(&m, (uint8_t)p), nw_u16(&m, (uint16_t)O.set.tick_rate), nw_f32(&m, (float)O.now);
    send_msg(peer, NET_RELIABLE, &m);
    g_rules_at = 0.0; // (the match's state to everyone on the next tick: the new player too)
    for (int i = 0; i < ONLINE_MAX_PLAYERS; i++)
        if (O.players[i].used && i != p) send_player(peer, i, true);
    for (int i = 0; i < O.nreps;) { // level actors' numbers, many to a message
        MSG(l);
        nw_u8(&l, M_SPAWN), nw_u32(&l, 0); // (id 0: a list of level actors follows)
        int n = 0;
        for (; i < O.nreps && l.len + 5 + RE_NAME_MAX < NET_MAX_MESSAGE - 8; i++) {
            if (!(O.reps[i].flags & REP_LEVEL)) continue;
            nw_u32(&l, O.reps[i].id), nw_str(&l, O.reps[i].name), nw_u8(&l, (uint8_t)O.reps[i].owner); // (owner: the host's pawn is a level actor)
            n++;
        }
        if (n) send_msg(peer, NET_RELIABLE, &l);
    }
    for (int i = 0; i < O.nreps; i++)
        if (O.reps[i].flags & REP_SPAWNED) send_spawn(peer, &O.reps[i], w);
    bool first = !O.players[p].pawn_id;
    if (first) send_player(-1, p, true);
    spawn_pawn(w, p);
    if (first) event(ONLINE_EV_PLAYER_JOINED, p, true, O.players[p].name);
}

// --- sessions ----------------------------------------------------------------------------------------------------------
static void reset_session(void) {
    if (O.host) net_host_destroy(O.host), O.host = NULL;
    memset(O.players, 0, sizeof O.players);
    O.nreps = 0, O.next_id = 0, O.local = -1, O.role = ONLINE_STANDALONE;
    O.have_offset = false, O.steam_session = false, O.joining = false, O.kicked = false;
    O.state = ONLINE_STATE_NONE;
    for (int p = 0; p < ONLINE_MAX_PLAYERS; p++) free(O.slots[p]), free(O.logs[p]), O.slots[p] = NULL, O.logs[p] = NULL;
    free(O.hist), O.hist = NULL;
    O.play_t = O.last_interp = O.last_snap_st = 0.0;
}

static uint8_t session_flags(void) { // (the LAN's answer and M_SESSION)
    return (uint8_t)((O.cur.dedicated ? 1 : 0) | (O.cur.password[0] ? 2 : 0) | (O.state == ONLINE_STATE_IN_PROGRESS ? 4 : 0) |
                     (open_to_joins() ? 8 : 0) | (O.cur.allow_join_in_progress ? 16 : 0) | (O.cur.lan ? 32 : 0) |
                     (O.cur.advertise ? 64 : 0) | (O.cur.allow_invites ? 128 : 0));
}
// The LAN's answer to a search: name, players, max, host (what older games read), then version 2: the session's id, flags,
// private slots, build, map, game mode and properties (as many as fit). Not advertised: no answer at all
static void server_info(void) {
    uint8_t b[512];
    NetWriter w = { b, 0, sizeof b, false };
    nw_str(&w, O.cur.name), nw_u8(&w, (uint8_t)used_players()), nw_u8(&w, (uint8_t)O.cur.max_players);
    nw_str(&w, O.cur.dedicated ? "Dedicated" : local_name());
    nw_u8(&w, 2), nw_u32(&w, O.session_id), nw_u8(&w, session_flags()), nw_u8(&w, (uint8_t)O.cur.private_slots);
    nw_u32(&w, O.cur.build_id), nw_str(&w, O.cur.map), nw_str(&w, O.cur.game_mode);
    int count_at = w.len, np = 0;
    nw_u8(&w, 0);
    for (int i = 0; i < O.cur.nprops; i++) {
        int need = 2 + (int)strlen(O.cur.props[i].key) + (int)strlen(O.cur.props[i].value);
        if (w.len + need > (int)sizeof b) break;
        nw_str(&w, O.cur.props[i].key), nw_str(&w, O.cur.props[i].value);
        np++;
    }
    b[count_at] = (uint8_t)np;
    bool hidden = !O.cur.advertise || O.cur.visibility == ONLINE_VIS_INVITE_ONLY;
    if (O.host) net_set_info(O.host, b, hidden || w.overflow ? 0 : w.len);
}

// The Steam lobby's data (the searches read and filter it): re_* for the engine's fields, p_<key> per property and their
// keys in re_props; the lobby's type, member limit and joinability follow the settings
static int lobby_type(void) {
    return !O.cur.advertise || O.cur.visibility == ONLINE_VIS_INVITE_ONLY ? 0 : O.cur.visibility == ONLINE_VIS_FRIENDS ? 1 : 2;
}
static void lobby_data(void) {
    if (!O.steam_session || !steam_lobby_current()) return;
    char t[64], keys[512];
    steam_lobby_set("re_version", "1");
    steam_lobby_set("re_name", O.cur.name);
    snprintf(t, sizeof t, "%llu", (unsigned long long)steam_user_id());
    steam_lobby_set("re_host", t);
    snprintf(t, sizeof t, "%u", O.cur.build_id), steam_lobby_set("re_build", t);
    steam_lobby_set("re_map", O.cur.map), steam_lobby_set("re_mode", O.cur.game_mode);
    steam_lobby_set("re_pw", O.cur.password[0] ? "1" : "0");
    snprintf(t, sizeof t, "%d", (int)O.state), steam_lobby_set("re_state", t);
    steam_lobby_set("re_jip", O.cur.allow_join_in_progress ? "1" : "0");
    steam_lobby_set("re_ded", O.cur.dedicated ? "1" : "0");
    snprintf(t, sizeof t, "%d", O.cur.private_slots), steam_lobby_set("re_priv", t);
    snprintf(t, sizeof t, "%u", O.session_id), steam_lobby_set("re_sid", t);
    snprintf(t, sizeof t, "%d", O.cur.max_players), steam_lobby_set("re_max", t);
    keys[0] = 0;
    for (int i = 0; i < O.cur.nprops; i++) {
        char k[48];
        snprintf(k, sizeof k, "p_%s", O.cur.props[i].key);
        steam_lobby_set(k, O.cur.props[i].value);
        if (strlen(keys) + strlen(O.cur.props[i].key) + 2 < sizeof keys) {
            if (keys[0]) strcat(keys, ",");
            strcat(keys, O.cur.props[i].key);
        }
    }
    steam_lobby_set("re_props", keys);
    steam_lobby_set_type(lobby_type());
    steam_lobby_set_limit(O.cur.max_players + O.cur.private_slots);
    steam_lobby_set_joinable(open_to_joins());
    steam_rich_presence("status", O.cur.name);
}

// The settings and the state to the clients (their copy: Get Current Session Settings / Property; no password)
static void send_session(int peer) {
    MSG(m);
    nw_u8(&m, M_SESSION), nw_str(&m, O.cur.name), nw_u8(&m, (uint8_t)O.cur.max_players), nw_u8(&m, (uint8_t)O.cur.private_slots);
    nw_u8(&m, session_flags()), nw_u8(&m, (uint8_t)O.cur.visibility), nw_u8(&m, (uint8_t)O.state);
    nw_str(&m, O.cur.map), nw_str(&m, O.cur.game_mode), nw_u32(&m, O.cur.build_id);
    nw_u8(&m, (uint8_t)O.cur.nprops);
    for (int i = 0; i < O.cur.nprops; i++) nw_str(&m, O.cur.props[i].key), nw_str(&m, O.cur.props[i].value);
    if (peer < 0) broadcast(NET_RELIABLE, &m, -1);
    else send_msg(peer, NET_RELIABLE, &m);
}
// Everywhere the others see the session: the LAN's answer, the Steam lobby, the clients
static void publish(void) {
    if (!is_server()) return;
    server_info();
    lobby_data();
    send_session(-1);
}

static bool start_server(World* w, bool listen, bool lan, uint16_t port) {
    ensure_defaults();
    if (!port) port = O.set.port;
    reset_session();
    O.w = w;
    O.lan = lan || !steam_available() || !O.set.use_steam;
    O.cur.lan = O.lan;
    bool ok = O.lan ? net_udp_open(&O.tr, port, true) : steam_net_open(&O.tr);
    if (!ok) {
        set_error(O.lan ? "the port is in use" : "Steam networking failed");
        event(ONLINE_EV_SESSION_CREATED, 0, false, O.last_error);
        return false;
    }
    // every slot (public and private) and two more: a refused player still connects to hear why (online.c refuse)
    int slots = O.cur.max_players + O.cur.private_slots;
    O.host = net_host_create(&O.tr, true, slots + 2 < NET_MAX_PEERS ? (slots > 0 ? slots + 2 : 4) : NET_MAX_PEERS, ONLINE_PROTOCOL);
    O.hist = calloc(MAX_REP, sizeof *O.hist); // (a session's start, not a frame)
    if (!O.host || !O.hist) {
        reset_session();
        return false;
    }
    net_set_sim(O.host, &O.set.sim);
    O.role = listen ? ONLINE_LISTEN : ONLINE_DEDICATED;
    collect_level_reps(w);
    if (listen) {
        O.local = player_new(local_name(), steam_user_id(), -1);
        if (O.local >= 0) O.players[O.local].pawn = bp_player(w);
        Rep* r = O.local >= 0 ? rep_of(O.players[O.local].pawn) : NULL;
        if (r) r->owner = (int8_t)O.local, O.players[O.local].pawn_id = r->id;
    }
    O.session_id = (uint32_t)(re_time_seconds() * 1000.0) * 2654435761u ^ (uint32_t)re_thread_id() ^ (uint32_t)(uintptr_t)&O;
    if (!O.session_id) O.session_id = 1;
    set_error("");
    if (!O.lan) { // a Steam lobby: friends see it, the overlay invites into it (its data when it is made)
        O.steam_session = true;
        O.state = ONLINE_STATE_CREATING;
        steam_lobby_create(lobby_type(), O.cur.max_players + O.cur.private_slots);
        server_info();
    } else {
        O.state = ONLINE_STATE_PENDING;
        server_info();
        event(ONLINE_EV_SESSION_CREATED, 0, true, "LAN");
    }
    return true;
}

// The settings made valid: players within the engine's limit, the staged properties (Set Session Property before the
// session) added, the build filled
static void take_settings(const OnlineSessionSettings* st) {
    O.cur = *st;
    if (!O.cur.name[0]) snprintf(O.cur.name, sizeof O.cur.name, "Game");
    if (O.cur.max_players <= 0) O.cur.max_players = O.set.max_players > 0 ? O.set.max_players : 4;
    if (O.cur.max_players > ONLINE_MAX_PLAYERS) O.cur.max_players = ONLINE_MAX_PLAYERS;
    if (O.cur.private_slots < 0) O.cur.private_slots = 0;
    if (O.cur.max_players + O.cur.private_slots > ONLINE_MAX_PLAYERS) O.cur.private_slots = ONLINE_MAX_PLAYERS - O.cur.max_players;
    if (O.cur.nprops < 0 || O.cur.nprops > ONLINE_MAX_PROPS) O.cur.nprops = 0;
    for (int i = 0; i < O.nstaged; i++) prop_set(O.cur.props, &O.cur.nprops, O.staged[i].key, O.staged[i].value);
    if (!O.cur.build_id) O.cur.build_id = game_build();
}
bool online_host_advanced(World* w, const OnlineSessionSettings* st) {
    ensure_defaults();
    OnlineSessionSettings d;
    if (!st) online_settings_default(&d), st = &d;
    take_settings(st);
    return start_server(w, !O.cur.dedicated, O.cur.lan, O.set.port);
}
bool online_host(World* w, const char* name, int max_players, bool lan) {
    OnlineSessionSettings st;
    online_settings_default(&st);
    snprintf(st.name, sizeof st.name, "%s", name && name[0] ? name : "Game");
    st.max_players = max_players > 0 ? max_players : O.set.max_players;
    st.lan = lan;
    return online_host_advanced(w, &st);
}
bool online_host_dedicated(World* w, uint16_t port) {
    ensure_defaults();
    OnlineSessionSettings st;
    online_settings_default(&st);
    snprintf(st.name, sizeof st.name, "Dedicated");
    st.max_players = O.set.max_players;
    st.lan = st.dedicated = true;
    take_settings(&st);
    return start_server(w, false, true, port ? port : O.set.port);
}

static bool is_number(const char* t, double* v) {
    if (!t || !t[0]) return false;
    char* end;
    *v = strtod(t, &end);
    while (*end == ' ') end++;
    return *end == 0;
}
static bool compare(const char* a, const char* b, OnlineCompare op) {
    double x, y;
    int c;
    if (op == ONLINE_CMP_CONTAINS) { // (any case)
        char la[64], lb[64];
        snprintf(la, sizeof la, "%s", a), snprintf(lb, sizeof lb, "%s", b);
        for (char* c = la; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (char* c = lb; *c; c++) *c = (char)tolower((unsigned char)*c);
        return strstr(la, lb) != NULL;
    }
    if (is_number(a, &x) && is_number(b, &y)) c = x < y ? -1 : x > y ? 1 : 0;
    else c = _stricmp(a, b);
    switch (op) {
    case ONLINE_CMP_EQ: return c == 0;
    case ONLINE_CMP_NE: return c != 0;
    case ONLINE_CMP_LT: return c < 0;
    case ONLINE_CMP_LE: return c <= 0;
    case ONLINE_CMP_GT: return c > 0;
    case ONLINE_CMP_GE: return c >= 0;
    default: return false;
    }
}
// A found session's value for a key: its own property, else the engine's names (name, map, game_mode, host)
static const char* result_value(const OnlineSession* ses, const char* key, bool* found) {
    for (int i = 0; i < ses->nprops; i++)
        if (!_stricmp(ses->props[i].key, key)) return *found = true, ses->props[i].value;
    *found = true;
    if (!_stricmp(key, "name")) return ses->name;
    if (!_stricmp(key, "map")) return ses->level;
    if (!_stricmp(key, "game_mode")) return ses->game_mode;
    if (!_stricmp(key, "host")) return ses->host;
    *found = false;
    return "";
}
static bool passes(const OnlineSession* ses, const OnlineSearch* q) {
    int open = ses->max_players - ses->players, others = ses->players - (ses->dedicated ? 0 : 1); // (a listen server's host)
    if (q->server_type == ONLINE_SERVERS_LISTEN && ses->dedicated) return false;
    if (q->server_type == ONLINE_SERVERS_DEDICATED && !ses->dedicated) return false;
    if (!ses->joinable) return false; // (locked, or started without join in progress: joining would only fail)
    if (q->exclude_full && open <= 0) return false;
    if (q->min_open_slots > 0 && open < q->min_open_slots) return false;
    if (q->empty_only && others > 0) return false;
    if (q->non_empty_only && others <= 0) return false;
    if (q->hide_passworded && ses->has_password) return false;
    if (q->hide_in_progress && ses->in_progress) return false;
    if (q->same_build_only && ses->build_id != game_build()) return false;
    if (q->max_ping_ms > 0.0f && ses->ping_ms >= 0.0f && ses->ping_ms > q->max_ping_ms) return false;
    for (int i = 0; i < q->nfilters; i++) {
        bool found;
        const char* v = result_value(ses, q->filters[i].key, &found);
        if (!found) {
            if (q->filters[i].op != ONLINE_CMP_NE) return false;
            continue;
        }
        if (!compare(v, q->filters[i].value, q->filters[i].op)) return false;
    }
    return true;
}
static int by_ping(const void* a, const void* b) { // the nearest first (not measured: last), then the fullest
    const OnlineSession *x = a, *y = b;
    float px = x->ping_ms < 0.0f ? 1e9f : x->ping_ms, py = y->ping_ms < 0.0f ? 1e9f : y->ping_ms;
    if (px != py) return px < py ? -1 : 1;
    return y->players - x->players;
}
// The search's end: its filters, the order, Max Results, then the event
static void finish_search(void) {
    int n = 0;
    for (int i = 0; i < O.nsessions; i++)
        if (passes(&O.sessions[i], &O.search)) O.sessions[n++] = O.sessions[i];
    qsort(O.sessions, (size_t)n, sizeof O.sessions[0], by_ping);
    O.nsessions = n < O.search.max_results ? n : O.search.max_results;
    O.finding = false;
    event(ONLINE_EV_SESSIONS_FOUND, O.nsessions, true, NULL);
}
bool online_find_advanced(const OnlineSearch* q) {
    ensure_defaults();
    O.nsessions = 0;
    O.search = *q;
    if (O.search.max_results <= 0 || O.search.max_results > MAX_FOUND) O.search.max_results = O.search.max_results <= 0 ? 50 : MAX_FOUND;
    if (O.search.nfilters < 0 || O.search.nfilters > ONLINE_MAX_FILTERS) O.search.nfilters = 0;
    set_error("");
    if (!q->lan && steam_available() && O.set.use_steam) { // Steam's own copy of the filters (online.c checks them all again)
        SteamLobbyFilter f[ONLINE_MAX_FILTERS + 6];
        char keys[ONLINE_MAX_FILTERS][40], build[16];
        int nf = 0;
        f[nf++] = (SteamLobbyFilter){ "re_version", "1", 0, 0, false };
        uint32_t b = game_build();
        if (q->same_build_only && b) snprintf(build, sizeof build, "%u", b), f[nf++] = (SteamLobbyFilter){ "re_build", build, 0, 0, false };
        if (q->server_type != ONLINE_SERVERS_ALL) f[nf++] = (SteamLobbyFilter){ "re_ded", q->server_type == ONLINE_SERVERS_DEDICATED ? "1" : "0", 0, 0, false };
        if (q->hide_passworded) f[nf++] = (SteamLobbyFilter){ "re_pw", "0", 0, 0, false };
        if (q->hide_in_progress) f[nf++] = (SteamLobbyFilter){ "re_state", "3", 0, 3, false };
        static const int cmp[] = { 0, 3, -1, -2, 1, 2 }; // (ONLINE_CMP_* to ELobbyComparison)
        for (int i = 0; i < O.search.nfilters; i++) {
            const OnlineFilter* fl = &O.search.filters[i];
            double v;
            if (fl->op == ONLINE_CMP_CONTAINS) continue;
            snprintf(keys[i], sizeof keys[i], "p_%s", fl->key);
            if (is_number(fl->value, &v) && v == (double)(int)v) f[nf++] = (SteamLobbyFilter){ keys[i], fl->value, (int)v, cmp[fl->op], true };
            else if (fl->op == ONLINE_CMP_EQ || fl->op == ONLINE_CMP_NE) f[nf++] = (SteamLobbyFilter){ keys[i], fl->value, 0, cmp[fl->op], false };
        }
        int slots = q->min_open_slots > 0 ? q->min_open_slots : q->exclude_full ? 1 : 0;
        O.finding = steam_lobby_find_ex(f, nf, slots, O.search.max_results);
        if (!O.finding) set_error("Steam's search failed");
        return O.finding;
    }
    // LAN: a client host on any port asks the broadcast address (and this machine); answers for a second
    if (!O.host) {
        if (!net_udp_open(&O.tr, 0, true)) return set_error("no network"), false;
        O.host = net_host_create(&O.tr, false, 1, ONLINE_PROTOCOL);
        if (!O.host) return set_error("no network"), false;
    }
    O.finding = net_discover(O.host, O.set.port);
    O.find_sent = re_time_seconds();
    O.find_until = O.find_sent + 1.0;
    return O.finding;
}
bool online_find(bool lan) {
    OnlineSearch q;
    memset(&q, 0, sizeof q);
    q.lan = lan, q.max_results = 32;
    return online_find_advanced(&q);
}
bool online_finding(void) { return O.finding; }
bool online_search_filter_add(const char* key, const char* value, OnlineCompare op) {
    if (!key || !key[0] || O.nfilters >= ONLINE_MAX_FILTERS || op < ONLINE_CMP_EQ || op > ONLINE_CMP_CONTAINS) return false;
    OnlineFilter* f = &O.filters[O.nfilters++];
    snprintf(f->key, sizeof f->key, "%s", key), snprintf(f->value, sizeof f->value, "%s", value ? value : "");
    f->op = op;
    return true;
}
void online_search_filters_clear(void) { O.nfilters = 0; }
void online_search_filters(OnlineFilter* out, int* n) {
    memcpy(out, O.filters, sizeof O.filters[0] * (size_t)O.nfilters);
    *n = O.nfilters;
}
const char* online_session_property(int i, const char* key, bool* found) {
    bool f = false;
    const char* v = i >= 0 && i < O.nsessions && key ? result_value(&O.sessions[i], key, &f) : "";
    if (found) *found = f;
    return v;
}
int online_session_count(void) { return O.nsessions; }
const OnlineSession* online_session(int i) { return i >= 0 && i < O.nsessions ? &O.sessions[i] : NULL; }

static bool connect_to(World* w, const NetAddr* a, bool steam) {
    ensure_defaults();
    if (O.host && O.role != ONLINE_STANDALONE) reset_session();
    if (O.host) net_host_destroy(O.host), O.host = NULL;
    O.w = w;
    bool ok = steam ? steam_net_open(&O.tr) : net_udp_open(&O.tr, 0, false);
    if (!ok) return false;
    O.host = net_host_create(&O.tr, false, 1, ONLINE_PROTOCOL);
    if (!O.host) return false;
    net_set_sim(O.host, &O.set.sim);
    O.role = ONLINE_CLIENT;
    O.joining = true;
    O.steam_session = steam;
    return net_connect(O.host, a) == 0;
}

static bool join_failed(const char* why) {
    set_error(why);
    event(ONLINE_EV_JOINED, 0, false, why);
    return false;
}
bool online_join_ex(World* w, int i, const char* password) {
    const OnlineSession* ses = online_session(i);
    if (!ses) return join_failed("no such session (Find Sessions first)");
    // what the search already knows: no trip to the server to hear it
    uint32_t b = game_build();
    if (ses->build_id && b && ses->build_id != b) return join_failed("a different version of the game");
    if (!ses->joinable) return join_failed(ses->in_progress ? "the match already started" : "the session is locked");
    if (ses->players >= ses->max_players) return join_failed("the session is full");
    if (ses->has_password && (!password || !password[0])) return join_failed("the session needs a password");
    set_error("");
    O.join_pw = pw_hash(password), O.join_invite = false;
    if (ses->steam) return steam_lobby_join(ses->addr.id); // (the lobby entered: the connection to its owner)
    return connect_to(w, &ses->addr, false);
}
bool online_join(World* w, int i) { return online_join_ex(w, i, ""); }
bool online_join_address_ex(World* w, const char* address, const char* password) {
    memset(&P, 0, sizeof P); // (B13: a new session's moves from 1)
    ensure_defaults();
    NetAddr a;
    if (!net_addr_parse(address, O.set.port, &a)) {
        set_error("bad address");
        event(ONLINE_EV_ERROR, 0, false, "bad address");
        return false;
    }
    set_error("");
    O.join_pw = pw_hash(password), O.join_invite = true; // (by the address: the private slots too)
    return connect_to(w, &a, false);
}
bool online_join_address(World* w, const char* address) { return online_join_address_ex(w, address, ""); }

void online_leave(World* w) {
    (void)w;
    if (O.steam_session) steam_lobby_leave();
    reset_session();
}
void online_end_play(World* w) { online_leave(w); }

// --- receiving ---------------------------------------------------------------------------------------------------------
static Entity spawn_class(World* w, uint32_t cls, const float pos[3], const float rot[3]) {
    BpAsset* a = bp_find_hash(cls);
    if (!a) return ENTITY_NONE;
    Entity e = bp_spawn(w, a, pos, rot);
    CCharacter* c = e ? ecs_get(w, e, COMP_CHARACTER) : NULL;
    if (c) c->auto_possess = 0;
    return e;
}

static void push_state(Rep* r, double t, const float pos[3], const float rot[3], const float vel[3]) {
    if (r->nhist && t <= r->hist[(r->head + HIST - 1) % HIST].t) return; // (older or the same: an old snapshot)
    State* s = &r->hist[r->head];
    s->t = t;
    memcpy(s->pos, pos, sizeof s->pos), memcpy(s->rot, rot, sizeof s->rot), memcpy(s->vel, vel, sizeof s->vel);
    r->head = (r->head + 1) % HIST;
    if (r->nhist < HIST) r->nhist++;
}

static void run_rpc(World* w, Entity e, const char* evname, int sender, NetReader* rd) {
    O.rpc_f = nr_f32(rd);
    for (int k = 0; k < 3; k++) O.rpc_v[k] = nr_f32(rd);
    nr_str(rd, O.rpc_text, sizeof O.rpc_text);
    O.rpc_sender = sender;
    if (e && ecs_alive(w, e)) bp_call_event(w, e, evname);
}

// --- who may come in (the server, at a player's HELLO) ---------------------------------------------------------------------
static int ban_of(uint64_t steam, const NetAddr* a) {
    for (int i = 0; i < O.nbans; i++)
        if ((steam && O.bans[i].steam == steam) || (!O.bans[i].steam && a && O.bans[i].addr.kind == NET_ADDR_IP && a->kind == NET_ADDR_IP && O.bans[i].addr.ip == a->ip))
            return i;
    return -1;
}
static const char* refuse_reason(int peer, uint64_t steam, uint64_t pw, uint32_t build, bool invited) {
    NetAddr a = net_peer_addr(O.host, peer);
    if (ban_of(steam, &a) >= 0) return "banned from this session";
    if (O.cur.build_id && build && build != O.cur.build_id) return "a different version of the game";
    if (pw_hash(O.cur.password) != pw) return pw ? "wrong password" : "the session needs a password";
    if (!O.cur.joinable) return "the session is locked";
    if (O.state == ONLINE_STATE_IN_PROGRESS && !O.cur.allow_join_in_progress) return "the match already started";
    int used = used_players();
    if (used >= O.cur.max_players + (invited ? O.cur.private_slots : 0)) return "the session is full";
    return NULL;
}
// Tells the player why (it reaches them as Event On Join Session Complete with the reason), then closes
static void refuse(int peer, const char* name, const char* why) {
    MSG(m);
    nw_u8(&m, M_KICK), nw_str(&m, why);
    send_msg(peer, NET_RELIABLE, &m);
    net_flush(O.host, O.now);
    net_disconnect(O.host, peer);
    char t[128];
    snprintf(t, sizeof t, "%s: %s", name && name[0] ? name : "a player", why);
    event(ONLINE_EV_JOIN_REFUSED, 0, false, t);
    O.refused++;
}

static void on_message(World* w, int peer, const uint8_t* d, int len) {
    NetReader rd = { d, len, 0, false };
    uint8_t type = nr_u8(&rd);
    switch (type) {
    case M_RULES: // (client) the server's match: phase, clock, scores, lives (game_rules.c fires the events)
        if (!is_server() && len > 1) rules_net_read(w, d + 1, (uint32_t)(len - 1));
        break;
    case M_HELLO: { // (server) a player arrives
        if (!is_server()) return;
        char name[64];
        nr_str(&rd, name, sizeof name);
        uint64_t steam = nr_u64(&rd);
        uint64_t pw = nr_u64(&rd);
        uint32_t build = nr_u32(&rd);
        bool invited = (nr_u8(&rd) & 1) != 0;
        int p = player_of_peer(peer);
        if (p >= 0) { // back after a level change (Open Level on the server): its world again, its new pawn
            welcome(w, peer, p);
            break;
        }
        const char* why = refuse_reason(peer, steam, pw, build, invited);
        if (!why && (p = player_new(name, steam, peer)) < 0) why = "the session is full";
        if (why) {
            refuse(peer, name, why);
            return;
        }
        welcome(w, peer, p);
        send_session(peer);
        server_info();
        lobby_data();
        break;
    }
    case M_WELCOME: { // (client) the session is ours
        O.local = nr_u8(&rd);
        O.set.tick_rate = nr_u16(&rd);
        float st = nr_f32(&rd);
        O.server_dt = st - O.now, O.have_offset = true;
        O.players[O.local].used = true, O.players[O.local].peer = -1;
        snprintf(O.players[O.local].name, sizeof O.players[O.local].name, "%s", local_name());
        if (O.joining) event(ONLINE_EV_JOINED, O.local, true, NULL); // (not again after a level change)
        O.joining = false;
        break;
    }
    case M_PLAYER: { // (client) a player came or went
        int p = nr_u8(&rd);
        bool joined = nr_u8(&rd) != 0;
        if (p >= ONLINE_MAX_PLAYERS) return;
        char name[64];
        nr_str(&rd, name, sizeof name);
        if (joined && p != O.local) {
            O.players[p].used = true, O.players[p].peer = -1, O.players[p].steam = nr_u64(&rd);
            snprintf(O.players[p].name, sizeof O.players[p].name, "%s", name);
        }
        if (p != O.local) event(joined ? ONLINE_EV_PLAYER_JOINED : ONLINE_EV_PLAYER_LEFT, p, true, name);
        if (!joined && p != O.local) O.players[p].used = false;
        break;
    }
    case M_SPAWN: { // (client) a replicated actor: the level's list (id 0) or a spawned one
        uint32_t id = nr_u32(&rd);
        if (!id) {
            while (rd.at < rd.len && !rd.overflow) {
                uint32_t lid = nr_u32(&rd);
                char name[RE_NAME_MAX];
                nr_str(&rd, name, sizeof name);
                int owner = (int8_t)nr_u8(&rd);
                Entity e = by_name(w, name);
                if (e && !rep_id(lid)) {
                    Rep* r = rep_add(e, lid, REP_MOVE | REP_LEVEL, owner);
                    if (r) snprintf(r->name, sizeof r->name, "%s", name);
                    if (owner >= 0 && owner < ONLINE_MAX_PLAYERS && owner != O.local) O.players[owner].pawn = e, O.players[owner].pawn_id = lid;
                }
            }
            break;
        }
        uint32_t cls = nr_u32(&rd);
        int owner = (int8_t)nr_u8(&rd);
        float pos[3], rot[3], vel[3];
        r_state(&rd, pos, rot, vel);
        if (rep_id(id)) break;
        Entity e = spawn_class(w, cls, pos, rot);
        if (e) {
            Rep* r = rep_add(e, id, REP_MOVE | REP_SPAWNED, owner);
            if (r) r->cls = cls;
            if (owner >= 0 && owner < ONLINE_MAX_PLAYERS) O.players[owner].pawn = e, O.players[owner].pawn_id = id;
        }
        break;
    }
    case M_DESTROY: {
        Rep* r = rep_id(nr_u32(&rd));
        if (r) {
            if (ecs_alive(w, r->e)) ecs_destroy(w, r->e);
            rep_remove(r);
        }
        break;
    }
    case M_POSSESS: { // (client) this pawn is ours: the input drives it, the server stops moving it here
        Rep* r = rep_id(nr_u32(&rd));
        if (r && O.local >= 0) {
            r->owner = (int8_t)O.local;
            O.players[O.local].pawn = r->e, O.players[O.local].pawn_id = r->id;
            bp_set_player(w, r->e);
        }
        break;
    }
    case M_SNAP: { // (client) the server's states
        float st = nr_f32(&rd);
        uint16_t n = nr_u16(&rd);
        // the server's clock: its time minus ours, smoothed (half the round trip ahead); its jitter. Only from the newest
        // snapshot (they may arrive out of order: each actor keeps its states by time)
        if (st > O.last_snap_st) {
            O.last_snap_st = st;
            double off = st - O.now + (net_stats(O.host, 0).rtt_ms * 0.0005);
            if (!O.have_offset) O.server_dt = off, O.have_offset = true;
            else {
                O.jitter_ms += ((float)fabs(off - O.server_dt) * 1000.0f - O.jitter_ms) * 0.1f;
                O.server_dt += (off - O.server_dt) * 0.05;
            }
        }
        O.snapshots++;
        for (uint16_t i = 0; i < n && !rd.overflow; i++) {
            uint32_t id = net_repl_read_id(&rd);
            float pos[3], rot[3], vel[3];
            r_state(&rd, pos, rot, vel);
            Rep* r = rep_id(id);
            if (r && r->owner != O.local) push_state(r, st, pos, rot, vel);
        }
        break;
    }
    case M_OWNER: { // (server) a client's own pawn: taken if it is theirs and its speed is plausible
        uint32_t id = nr_u32(&rd);
        float pos[3], rot[3], vel[3];
        r_state(&rd, pos, rot, vel);
        uint16_t view_ms = nr_u16(&rd); // (how far back this player sees the others: the lag compensation rewinds by it)
        Rep* r = rep_id(id);
        int p = player_of_peer(peer);
        if (p >= 0 && view_ms) O.view_delay_ms[p] = view_ms;
        if (!r || p < 0 || r->owner != p || !ecs_alive(w, r->e)) break;
        const CTransform* t = ecs_get(w, r->e, COMP_TRANSFORM);
        const CCharacter* c = ecs_get(w, r->e, COMP_CHARACTER);
        float maxv = c ? fmaxf(c->max_walk_speed, 600.0f) * 2.5f + 1500.0f : 4000.0f; // (falls, launches: generous)
        double dt = O.now - r->last_owner_t;
        if (r->last_owner_t > 0.0 && t && dt > 0.0) {
            float dx = pos[0] - t->location[0], dy = pos[1] - t->location[1], dz = pos[2] - t->location[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > maxv * (float)fmin(dt + 0.1, 1.0) + 50.0f) { // too far too fast: back to where the server has it
                O.corrections++;
                float sp[3], sr[3], sv[3];
                get_state(w, r->e, sp, sr, sv);
                MSG(m);
                nw_u8(&m, M_CORRECT), nw_u32(&m, id);
                w_state(&m, sp, sr, sv);
                send_msg(peer, NET_RELIABLE, &m);
                r->last_owner_t = O.now;
                break;
            }
        }
        r->last_owner_t = O.now;
        set_state(w, r->e, pos, rot, vel);
        break;
    }
    case M_MOVE: { // (server, B13) a client's moves: the new ones applied to its Character (the tick leaves it alone)
        uint32_t id = nr_u32(&rd);
        uint16_t view_ms = nr_u16(&rd);
        uint8_t n = nr_u8(&rd);
        uint32_t base = nr_u32(&rd);
        Rep* r = rep_id(id);
        int p = player_of_peer(peer);
        if (p >= 0 && view_ms) O.view_delay_ms[p] = view_ms;
        bool mine = r && p >= 0 && p < 16 && r->owner == p && ecs_alive(w, r->e) && ecs_get(w, r->e, COMP_CHARACTER);
        if (mine) bp_character_skip(r->e, true);
        for (uint8_t i = 0; i < n && !rd.overflow; i++) {
            uint32_t seq = base + i;
            float dt = nr_u16(&rd) / 10000.0f, in[2];
            in[0] = (int8_t)nr_u8(&rd) / 127.0f, in[1] = (int8_t)nr_u8(&rd) / 127.0f;
            uint8_t jump = nr_u8(&rd);
            if (!mine || seq <= P.last_applied[p]) continue;
            bp_character_step(w, r->e, in, jump != 0, dt);
            P.last_applied[p] = seq;
        }
        if (!mine) break;
        float pos[3], rot[3], vel[3];
        get_state(w, r->e, pos, rot, vel);
        const CCharacter* c = ecs_get(w, r->e, COMP_CHARACTER);
        MSG(m);
        nw_u8(&m, M_MOVE_ACK), nw_u32(&m, id), nw_u32(&m, P.last_applied[p]);
        for (int k = 0; k < 3; k++) nw_f32(&m, pos[k]); // (exact: a quantized state would be a correction by itself)
        for (int k = 0; k < 3; k++) nw_f32(&m, vel[k]);
        nw_u8(&m, c && c->on_ground ? 1 : 0);
        send_msg(peer, NET_SEQUENCED, &m);
        break;
    }
    case M_MOVE_ACK: { // (client, B13) the server's state after our move seq: off from ours? take it, replay the rest
        uint32_t id = nr_u32(&rd), seq = nr_u32(&rd);
        float pos[3], vel[3];
        for (int k = 0; k < 3; k++) pos[k] = nr_f32(&rd);
        for (int k = 0; k < 3; k++) vel[k] = nr_f32(&rd);
        uint8_t ground = nr_u8(&rd);
        Rep* r = rep_id(id);
        if (rd.overflow || !r || !ecs_alive(w, r->e) || seq <= P.acked || seq >= P.next) break;
        P.acked = seq, P.acks++;
        const PredMove* pm = &P.m[seq % PRED_RING];
        float err = pm->seq == seq ? sqrtf((pm->pos[0] - pos[0]) * (pm->pos[0] - pos[0]) + (pm->pos[1] - pos[1]) * (pm->pos[1] - pos[1]) +
                                           (pm->pos[2] - pos[2]) * (pm->pos[2] - pos[2]))
                                   : 1e9f;
        if (err < 1e8f) P.max_err = fmaxf(P.max_err, err), P.sum_err += err;
        if (err <= PRED_TOL) break;
        // the server's state, then the moves it hasn't seen yet again from there
        P.corrections++;
        CTransform* t = ecs_get(w, r->e, COMP_TRANSFORM);
        CCharacter* c = ecs_get(w, r->e, COMP_CHARACTER);
        if (!t || !c) break;
        memcpy(t->location, pos, 12), memcpy(c->velocity, vel, 12), c->on_ground = ground;
        for (uint32_t s = seq + 1; s < P.next; s++) {
            PredMove* q = &P.m[s % PRED_RING];
            if (q->seq != s) continue;
            bp_character_step(w, r->e, q->in, q->jump != 0, q->dt);
            memcpy(q->pos, t->location, 12), memcpy(q->vel, c->velocity, 12);
            P.replayed++;
        }
        ecs_touch(w, r->e);
        break;
    }
    case M_CORRECT: { // (client) the server put our pawn back
        Rep* r = rep_id(nr_u32(&rd));
        float pos[3], rot[3], vel[3];
        r_state(&rd, pos, rot, vel);
        if (r && ecs_alive(w, r->e)) set_state(w, r->e, pos, rot, vel);
        break;
    }
    case M_RPC: {
        uint32_t id = nr_u32(&rd);
        uint8_t mode = nr_u8(&rd), reliable = nr_u8(&rd);
        char evname[64], actor[RE_NAME_MAX];
        nr_str(&rd, evname, sizeof evname);
        nr_str(&rd, actor, sizeof actor); // (an actor without a network number: by its name, the same level on both sides)
        int sender = is_server() ? player_of_peer(peer) : (int)nr_u8(&rd);
        (void)reliable;
        Rep* r = id ? rep_id(id) : NULL;
        Entity e = r ? r->e : actor[0] ? by_name(w, actor) : ENTITY_NONE;
        // (the server runs what a client asks of it; a client what the server sends; a client can't make the server multicast)
        if ((is_server() && mode == ONLINE_RPC_SERVER) || !is_server()) run_rpc(w, e, evname, sender, &rd);
        break;
    }
    case M_VAR: { // (client) a replicated variable's value, then OnRep_<Name>
        uint32_t vid = nr_u32(&rd);
        char actor[RE_NAME_MAX];
        nr_str(&rd, actor, sizeof actor);
        Rep* r = vid ? rep_id(vid) : NULL;
        Entity ve = r ? r->e : actor[0] ? by_name(w, actor) : ENTITY_NONE;
        char var[64];
        nr_str(&rd, var, sizeof var);
        uint8_t kind = nr_u8(&rd);
        BpValue v[4];
        memset(v, 0, sizeof v);
        uint8_t width = nr_u8(&rd);
        if (width > 4) width = 4;
        char text[256] = { 0 };
        if (kind == 1) nr_str(&rd, text, sizeof text);
        else
            for (int i = 0; i < width; i++)
                for (int k = 0; k < 4; k++) v[i].v[k] = nr_f32(&rd);
        if (!ve || !ecs_alive(w, ve)) break;
        if (kind == 1) bp_set_instance_text(w, ve, var, text);
        else bp_set_instance_var(w, ve, var, v, width);
        char rep[80];
        snprintf(rep, sizeof rep, "OnRep_%s", var);
        bp_call_event(w, ve, rep);
        break;
    }
    case M_CHAT: {
        char text[200];
        int from = is_server() ? player_of_peer(peer) : nr_u8(&rd);
        nr_str(&rd, text, sizeof text);
        if (is_server()) { // relayed to everyone (with who said it)
            MSG(m);
            nw_u8(&m, M_CHAT), nw_u8(&m, (uint8_t)from), nw_str(&m, text);
            broadcast(NET_RELIABLE, &m, peer);
        }
        event(ONLINE_EV_CHAT, from, true, text);
        break;
    }
    case M_TRAVEL: { // (client) the server opened another level: the same here, then HELLO again (online_level_loaded)
        char level[260];
        nr_str(&rd, level, sizeof level);
        O.nreps = 0;
        level_open_request(level);
        break;
    }
    case M_KICK: { // refused while joining (Event On Join Session Complete), or sent away later (On Network Error)
        char why[128];
        nr_str(&rd, why, sizeof why);
        set_error(why[0] ? why : "kicked");
        event(O.joining ? ONLINE_EV_JOINED : ONLINE_EV_ERROR, 0, false, O.last_error);
        O.kicked = true; // (the disconnection that follows says nothing more)
        break;
    }
    case M_SESSION: { // (client) the session's settings and state
        if (is_server()) return;
        OnlineSessionState before = O.state;
        nr_str(&rd, O.cur.name, sizeof O.cur.name);
        O.cur.max_players = nr_u8(&rd), O.cur.private_slots = nr_u8(&rd);
        uint8_t fl = nr_u8(&rd);
        O.cur.visibility = (OnlineVisibility)nr_u8(&rd);
        OnlineSessionState st = (OnlineSessionState)nr_u8(&rd);
        nr_str(&rd, O.cur.map, sizeof O.cur.map), nr_str(&rd, O.cur.game_mode, sizeof O.cur.game_mode);
        O.cur.build_id = nr_u32(&rd);
        O.cur.dedicated = fl & 1, O.cur.allow_join_in_progress = (fl & 16) != 0, O.cur.lan = (fl & 32) != 0;
        O.cur.advertise = (fl & 64) != 0, O.cur.allow_invites = (fl & 128) != 0, O.cur.joinable = (fl & 8) != 0 || (fl & 4);
        O.cur.password[0] = 0; // (a client never has it; Has Password reads the flag below)
        if (fl & 2) snprintf(O.cur.password, sizeof O.cur.password, "*");
        int np = nr_u8(&rd);
        O.cur.nprops = 0;
        for (int i = 0; i < np && i < ONLINE_MAX_PROPS && !rd.overflow; i++, O.cur.nprops++)
            nr_str(&rd, O.cur.props[i].key, sizeof O.cur.props[i].key), nr_str(&rd, O.cur.props[i].value, sizeof O.cur.props[i].value);
        event(ONLINE_EV_SESSION_UPDATED, 0, true, O.cur.name);
        if (st != before) O.state = before, session_state_set(st);
        break;
    }
    default: break;
    }
}

static int g_pre_ticks;
static double g_cpu_ms; // the network's CPU time last frame (the Debug Viewer)
int online_debug_ticks(void) { return g_pre_ticks; }
static void pre_tick(World* w, double now) {
    g_pre_ticks++;
    O.now = now;
    O.w = w;
    if (steam_available()) {
        steam_update();
        SteamEvent se;
        while (steam_poll(&se)) {
            switch (se.type) {
            case STEAM_EV_LOBBY_CREATED:
                if (se.a) O.state = ONLINE_STATE_PENDING, lobby_data();
                else set_error("Steam lobby failed"), O.state = ONLINE_STATE_NONE;
                event(ONLINE_EV_SESSION_CREATED, 0, se.a != 0, se.a ? "Steam" : "Steam lobby failed");
                break;
            case STEAM_EV_LOBBY_LIST:
                O.nsessions = 0;
                for (int i = 0; i < se.i && O.nsessions < MAX_FOUND; i++) {
                    uint64_t l = steam_lobby_found(i);
                    OnlineSession* ses = &O.sessions[O.nsessions++];
                    memset(ses, 0, sizeof *ses);
                    snprintf(ses->name, sizeof ses->name, "%s", steam_lobby_get(l, "re_name"));
                    ses->players = steam_lobby_members(l);
                    int maxp = atoi(steam_lobby_get(l, "re_max"));
                    ses->max_players = maxp > 0 ? maxp : steam_lobby_max(l);
                    ses->addr.kind = NET_ADDR_STEAM, ses->addr.id = l, ses->steam = true, ses->ping_ms = -1.0f;
                    uint64_t owner = steam_lobby_owner(l);
                    snprintf(ses->host, sizeof ses->host, "%s", owner ? steam_friend_name(owner) : "");
                    snprintf(ses->level, sizeof ses->level, "%s", steam_lobby_get(l, "re_map"));
                    snprintf(ses->game_mode, sizeof ses->game_mode, "%s", steam_lobby_get(l, "re_mode"));
                    ses->private_slots = atoi(steam_lobby_get(l, "re_priv"));
                    ses->build_id = (uint32_t)strtoul(steam_lobby_get(l, "re_build"), NULL, 10);
                    ses->session_id = (uint32_t)strtoul(steam_lobby_get(l, "re_sid"), NULL, 10);
                    ses->has_password = !strcmp(steam_lobby_get(l, "re_pw"), "1");
                    ses->dedicated = !strcmp(steam_lobby_get(l, "re_ded"), "1");
                    ses->in_progress = atoi(steam_lobby_get(l, "re_state")) == ONLINE_STATE_IN_PROGRESS;
                    ses->allow_join_in_progress = strcmp(steam_lobby_get(l, "re_jip"), "0") != 0;
                    ses->joinable = !(ses->in_progress && !ses->allow_join_in_progress); // (a locked lobby isn't listed by Steam)
                    char keys[512];
                    snprintf(keys, sizeof keys, "%s", steam_lobby_get(l, "re_props"));
                    for (char* k = strtok(keys, ","); k && ses->nprops < ONLINE_MAX_PROPS; k = strtok(NULL, ",")) {
                        char pk[48];
                        snprintf(pk, sizeof pk, "p_%s", k);
                        prop_set(ses->props, &ses->nprops, k, steam_lobby_get(l, pk));
                    }
                }
                finish_search();
                break;
            case STEAM_EV_LOBBY_ENTERED:
                if (se.i && !is_server()) { // a lobby joined: connect to its owner (the host) through the relay
                    NetAddr a = { NET_ADDR_STEAM, 0, 0, steam_lobby_owner(se.a) };
                    if (a.id && a.id != steam_user_id()) connect_to(w, &a, true);
                } else if (!se.i) event(ONLINE_EV_JOINED, 0, false, "the lobby refused");
                break;
            case STEAM_EV_JOIN_REQUESTED: { // a friend's invite accepted in the overlay: joined now, or when the game says
                const char* from = se.b ? steam_friend_name(se.b) : "";
                if (!O.no_auto_invites) {
                    O.join_pw = 0, O.join_invite = true;
                    steam_lobby_join(se.a);
                } else O.pending_invite = se.a;
                event(ONLINE_EV_INVITE, 0, !O.no_auto_invites, from);
                break;
            }
            case STEAM_EV_LEADERBOARD_SCORES: event(ONLINE_EV_LEADERBOARD, se.i, true, se.text); break;
            case STEAM_EV_OVERLAY: event(ONLINE_EV_OVERLAY, se.i, se.i != 0, NULL); break;
            default: break;
            }
        }
    }
    if (!O.host) return;
    net_update(O.host, now);
    NetEvent ne;
    while (net_poll(O.host, &ne)) {
        switch (ne.type) {
        case NET_EV_CONNECTED:
            if (!is_server()) { // (client) say who we are
                MSG(m);
                nw_u8(&m, M_HELLO), nw_str(&m, local_name()), nw_u64(&m, steam_user_id());
                nw_u64(&m, O.join_pw), nw_u32(&m, game_build()), nw_u8(&m, O.join_invite ? 1 : 0);
                send_msg(0, NET_RELIABLE, &m);
            }
            break;
        case NET_EV_DISCONNECTED:
            if (is_server()) {
                int p = player_of_peer(ne.peer);
                if (p >= 0) {
                    event(ONLINE_EV_PLAYER_LEFT, p, true, O.players[p].name);
                    if (O.players[p].pawn && ecs_alive(w, O.players[p].pawn)) online_destroy(w, O.players[p].pawn);
                    send_player(-1, p, false);
                    O.players[p].used = false;
                    server_info();
                }
            } else {
                bool was_joining = O.joining;
                const char* why = ne.reason == NET_REASON_TIMEOUT ? "the connection timed out" : ne.reason == NET_REASON_FULL ? "the session is full"
                                  : ne.reason == NET_REASON_VERSION ? "a different version of the game" : "disconnected";
                if (!O.kicked) set_error(why), event(was_joining ? ONLINE_EV_JOINED : ONLINE_EV_ERROR, 0, false, why); // (a kick told why already)
                reset_session();
                return;
            }
            break;
        case NET_EV_DISCOVERED: { // (a LAN search) a server answered
            if (O.nsessions >= MAX_FOUND || !O.finding) break;
            NetReader rd = { ne.data, ne.len, 0, false };
            OnlineSession ses;
            memset(&ses, 0, sizeof ses);
            nr_str(&rd, ses.name, sizeof ses.name);
            ses.players = nr_u8(&rd), ses.max_players = nr_u8(&rd);
            nr_str(&rd, ses.host, sizeof ses.host);
            ses.addr = ne.addr;
            ses.ping_ms = (float)((re_time_seconds() - O.find_sent) * 1000.0);
            ses.joinable = ses.allow_join_in_progress = true; // (an older game's answer stops here)
            if (rd.at < rd.len && nr_u8(&rd) >= 2) {
                ses.session_id = nr_u32(&rd);
                uint8_t fl = nr_u8(&rd);
                ses.private_slots = nr_u8(&rd);
                ses.build_id = nr_u32(&rd);
                nr_str(&rd, ses.level, sizeof ses.level), nr_str(&rd, ses.game_mode, sizeof ses.game_mode);
                ses.dedicated = fl & 1, ses.has_password = (fl & 2) != 0, ses.in_progress = (fl & 4) != 0;
                ses.joinable = (fl & 8) != 0, ses.allow_join_in_progress = (fl & 16) != 0;
                int np = nr_u8(&rd);
                for (int i = 0; i < np && ses.nprops < ONLINE_MAX_PROPS && !rd.overflow; i++, ses.nprops++)
                    nr_str(&rd, ses.props[i].key, sizeof ses.props[i].key), nr_str(&rd, ses.props[i].value, sizeof ses.props[i].value);
            }
            bool dup = false; // (the same server through the broadcast and through this machine: one)
            for (int i = 0; i < O.nsessions && ses.session_id; i++) dup |= O.sessions[i].session_id == ses.session_id;
            if (!dup) O.sessions[O.nsessions++] = ses;
            break;
        }
        case NET_EV_MESSAGE: on_message(w, ne.peer, ne.data, ne.len); break;
        default: break;
        }
    }
    if (O.finding && O.role == ONLINE_STANDALONE && now >= O.find_until) finish_search();
}

// --- sending, interpolating ----------------------------------------------------------------------------------------------
static void interpolate(World* w) {
    // the render time: the server's now minus the delay (two ticks, more with loss, plus the jitter it needs), on a clock
    // that speeds up or slows down a little instead of jumping (net_repl.h)
    double tick = 1.0 / (O.set.tick_rate > 0 ? O.set.tick_rate : 30);
    double delay = net_repl_delay(tick, O.jitter_ms, net_stats(O.host, 0).loss, O.set.interp_ms * 0.001);
    O.delay_s += (delay - O.delay_s) * (O.delay_s <= 0.0 ? 1.0 : 0.05); // (the delay itself eased: no jump when the loss changes)
    double dt = O.last_interp > 0.0 ? O.now - O.last_interp : 0.0;
    O.last_interp = O.now;
    double rt = net_repl_clock(&O.play_t, O.now + O.server_dt - O.delay_s, dt);
    float decay = (float)exp(-dt / 0.08); // (a jump's leftover gone in ~0.1 s)
    for (int i = 0; i < O.nreps; i++) {
        Rep* r = &O.reps[i];
        if (r->owner == O.local || !r->nhist || !ecs_alive(w, r->e)) continue;
        const State* a = NULL, *b = NULL;
        for (int k = 0; k < r->nhist; k++) {
            const State* s = &r->hist[(r->head + HIST - 1 - k) % HIST]; // newest first
            if (s->t <= rt) {
                a = s;
                break;
            }
            b = s;
        }
        float pos[3], rot[3], vel[3];
        if (a && b) { // between two: positions by Hermite (their velocities), angles the short way
            float t = (float)((rt - a->t) / fmax(b->t - a->t, 1e-4));
            float span = (float)(b->t - a->t), t2 = t * t, t3 = t2 * t;
            float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
            for (int k = 0; k < 3; k++) {
                pos[k] = h00 * a->pos[k] + h10 * span * a->vel[k] + h01 * b->pos[k] + h11 * span * b->vel[k];
                float d = fmodf(b->rot[k] - a->rot[k] + 540.0f, 360.0f) - 180.0f;
                rot[k] = a->rot[k] + d * t;
                vel[k] = a->vel[k] + (b->vel[k] - a->vel[k]) * t;
            }
        } else if (a) { // past the newest: extrapolated by its velocity, a quarter second at most (then it holds)
            float dt = (float)fmin(rt - a->t, 0.25);
            for (int k = 0; k < 3; k++) pos[k] = a->pos[k] + a->vel[k] * dt, rot[k] = a->rot[k], vel[k] = a->vel[k];
        } else { // before the oldest (just joined): the oldest
            const State* s = b;
            memcpy(pos, s->pos, sizeof pos), memcpy(rot, s->rot, sizeof rot), memcpy(vel, s->vel, sizeof vel);
        }
        // a jump (an extrapolation the new states contradict): kept as an offset that fades, never a pop. A teleport
        // (over 3 m off) is taken at once
        if (r->shown_ok) {
            float jump[3], expect = 0.0f, j2 = 0.0f;
            for (int k = 0; k < 3; k++) {
                jump[k] = (r->shown[k] - (pos[k] + r->err[k]));
                expect += vel[k] * vel[k];
            }
            expect = sqrtf(expect) * (float)dt * 1.5f + 2.0f;
            for (int k = 0; k < 3; k++) j2 += jump[k] * jump[k];
            float jl = sqrtf(j2);
            if (jl > 300.0f) memset(r->err, 0, sizeof r->err);
            else if (jl > expect)
                for (int k = 0; k < 3; k++) r->err[k] += jump[k] * (1.0f - expect / jl);
            for (int k = 0; k < 3; k++) r->err[k] *= decay, pos[k] += r->err[k];
        }
        memcpy(r->shown, pos, sizeof r->shown), r->shown_ok = true;
        set_state(w, r->e, pos, rot, vel);
    }
}

static void post_tick(World* w, double now) {
    O.now = now;
    if (!O.host || O.role == ONLINE_STANDALONE) return;
    if (O.role == ONLINE_CLIENT) interpolate(w);
    double tick = 1.0 / (O.set.tick_rate > 0 ? O.set.tick_rate : 30);
    if (now >= O.tick_at) {
        O.tick_at = now + tick;
        if (is_server() && rules_active() && (rules_net_serial() != g_rules_serial || now >= g_rules_at)) {
            // the Game Rules' match (game_rules.h): when it changed, and each second for the clock (reliable, small)
            MSG(m);
            nw_u8(&m, M_RULES);
            uint32_t n = rules_net_write(w, m.p + m.len, (uint32_t)(m.cap - m.len));
            if (n) {
                m.len += (int)n;
                broadcast(NET_RELIABLE, &m, -1);
            }
            g_rules_serial = rules_net_serial(), g_rules_at = now + 1.0;
        }
        if (is_server()) {
            // each actor's last second (lag compensation: where a player saw it when they fired)
            if (O.hist)
                for (int i = 0; i < O.nreps; i++) {
                    Rep* r = &O.reps[i];
                    if (!(r->flags & REP_MOVE) || !ecs_alive(w, r->e)) continue;
                    const CTransform* t = ecs_get(w, r->e, COMP_TRANSFORM);
                    if (t) net_repl_hist_push(&O.hist[i], now, t->location);
                }
            // a snapshot to each player (net_repl.h): an actor that moved since its last send, or one the player hasn't
            // confirmed whose send is older than a round trip (lost: again). Tracked: the acks say what arrived. Starting
            // where the last tick's budget stopped (every actor gets its turn)
            for (int p = 0; p < ONLINE_MAX_PLAYERS; p++) {
                Player* pl = &O.players[p];
                if (!pl->used || pl->peer < 0 || !net_connected(O.host, pl->peer) || !O.slots[p] || !O.logs[p]) continue;
                NetReplSlot* slots = O.slots[p];
                uint32_t acked[64];
                int na;
                while ((na = net_take_acked(O.host, pl->peer, acked, 64)) > 0)
                    for (int k = 0; k < na; k++) net_repl_on_ack(O.logs[p], slots, O.nreps, acked[k]);
                NetStats ns = net_stats(O.host, pl->peer);
                double resend_s = fmax(0.05, (ns.rtt_ms > 0.0f ? ns.rtt_ms * 1.25f + ns.jitter_ms * 2.0f : 150.0f) * 0.001) + tick;
                const CTransform* pt = pl->pawn ? ecs_get(w, pl->pawn, COMP_TRANSFORM) : NULL;
                int budget = O.set.budget_kbps > 0 ? (int)(O.set.budget_kbps * 1000.0 / 8.0 * tick) : 1 << 30;
                MSG(m);
                int count_at = 0, n = 0;
                uint16_t idx[NET_REPL_PER_MSG];
#define SNAP_BEGIN() (m.len = 0, nw_u8(&m, M_SNAP), nw_f32(&m, (float)now), count_at = m.len, nw_u16(&m, 0), n = 0)
#define SNAP_END() do { if (n) { \
        m.p[count_at] = (uint8_t)n, m.p[count_at + 1] = (uint8_t)(n >> 8); \
        uint32_t tok = net_send_tracked(O.host, pl->peer, NET_UNRELIABLE, m.p, m.len); \
        for (int q = 0; q < n; q++) net_repl_sent(&slots[idx[q]], snap_pos[q], snap_rot[q], now, tok); \
        net_repl_log(O.logs[p], tok, idx, n); \
        budget -= m.len; } } while (0)
                float snap_pos[NET_REPL_PER_MSG][3], snap_rot[NET_REPL_PER_MSG][3];
                SNAP_BEGIN();
                int start = O.nreps ? O.rr[p] % O.nreps : 0, i = start, seen = 0;
                for (; seen < O.nreps && budget > 0; seen++, i = (i + 1) % O.nreps) {
                    Rep* r = &O.reps[i];
                    if (!(r->flags & REP_MOVE) || r->owner == p || !ecs_alive(w, r->e)) continue;
                    float pos[3], rot[3], vel[3];
                    get_state(w, r->e, pos, rot, vel);
                    if (pt && O.set.relevancy_cm > 0.0f) {
                        float dx = pos[0] - pt->location[0], dy = pos[1] - pt->location[1];
                        if (dx * dx + dy * dy > O.set.relevancy_cm * O.set.relevancy_cm) continue;
                    }
                    NetReplSlot* s = &slots[i];
                    if (!net_repl_due(s, pos, rot, now, resend_s)) continue;
                    if ((s->flags & 2) && !(s->pend_pos[0] != pos[0] || s->pend_pos[1] != pos[1] || s->pend_pos[2] != pos[2])) O.resends++;
                    if (m.len + 4 + 18 + 4 > NET_MAX_MESSAGE || n >= NET_REPL_PER_MSG) {
                        SNAP_END();
                        SNAP_BEGIN();
                    }
                    net_repl_write_id(&m, r->id);
                    w_state(&m, pos, rot, vel);
                    memcpy(snap_pos[n], pos, sizeof pos), memcpy(snap_rot[n], rot, sizeof rot);
                    idx[n++] = (uint16_t)i;
                }
                SNAP_END();
                O.rr[p] = i; // (the next tick starts where this one's budget ran out)
#undef SNAP_BEGIN
#undef SNAP_END
            }
        } else if (O.local >= 0 && O.players[O.local].pawn && ecs_alive(w, O.players[O.local].pawn) &&
                   ecs_get(w, O.players[O.local].pawn, COMP_CHARACTER) && P.pawn == O.players[O.local].pawn) {
            // (B13) a Character: its moves since the last one confirmed (up to 40 a packet), the server moves it
            uint32_t first = P.acked + 1 > P.next - (P.next > 40 ? 40 : P.next) ? P.acked + 1 : P.next - 40;
            if (first < 1) first = 1;
            uint32_t n = P.next > first ? P.next - first : 0;
            if (n) {
                MSG(m);
                nw_u8(&m, M_MOVE), nw_u32(&m, O.players[O.local].pawn_id);
                nw_u16(&m, (uint16_t)fmin(O.delay_s * 1000.0, 60000.0));
                nw_u8(&m, (uint8_t)n), nw_u32(&m, first);
                for (uint32_t s = first; s < first + n; s++) {
                    const PredMove* q = &P.m[s % PRED_RING];
                    nw_u16(&m, (uint16_t)fmin(q->dt * 10000.0f, 65535.0f));
                    nw_u8(&m, (uint8_t)(int8_t)lroundf(fminf(fmaxf(q->in[0], -1.0f), 1.0f) * 127.0f));
                    nw_u8(&m, (uint8_t)(int8_t)lroundf(fminf(fmaxf(q->in[1], -1.0f), 1.0f) * 127.0f));
                    nw_u8(&m, q->jump);
                }
                send_msg(0, NET_SEQUENCED, &m);
            }
        } else if (O.local >= 0 && O.players[O.local].pawn && ecs_alive(w, O.players[O.local].pawn)) { // our pawn to the server (not a Character: its state)
            float pos[3], rot[3], vel[3];
            get_state(w, O.players[O.local].pawn, pos, rot, vel);
            MSG(m);
            nw_u8(&m, M_OWNER), nw_u32(&m, O.players[O.local].pawn_id);
            w_state(&m, pos, rot, vel);
            nw_u16(&m, (uint16_t)fmin(O.delay_s * 1000.0, 60000.0));
            send_msg(0, NET_SEQUENCED, &m);
        }
    }
    net_flush(O.host, now);
}

// --- players, replication, RPCs ------------------------------------------------------------------------------------------
int online_player_count(void) {
    int n = 0;
    for (int i = 0; i < ONLINE_MAX_PLAYERS; i++) n += O.players[i].used;
    return O.role == ONLINE_STANDALONE ? 1 : n;
}
int online_local_player(void) { return O.role == ONLINE_STANDALONE ? 0 : O.local; }
const char* online_player_name(int p) {
    if (O.role == ONLINE_STANDALONE) return p == 0 ? local_name() : "";
    return p >= 0 && p < ONLINE_MAX_PLAYERS && O.players[p].used ? O.players[p].name : "";
}
Entity online_player_pawn(World* w, int p) {
    if (O.role == ONLINE_STANDALONE) return p == 0 ? bp_player(w) : ENTITY_NONE;
    return p >= 0 && p < ONLINE_MAX_PLAYERS && O.players[p].used && ecs_alive(w, O.players[p].pawn) ? O.players[p].pawn : ENTITY_NONE;
}
static int peer_for(int p) {
    if (!O.host) return -1;
    if (O.role == ONLINE_CLIENT) return 0;
    return p >= 0 && p < ONLINE_MAX_PLAYERS && O.players[p].used ? O.players[p].peer : -1;
}
float online_player_ping(int p) {
    if (!O.host) return 0.0f;
    int peer = O.role == ONLINE_CLIENT ? 0 : peer_for(p);
    return peer >= 0 ? net_stats(O.host, peer).rtt_ms : 0.0f;
}
float online_player_loss(int p) {
    if (!O.host) return 0.0f;
    int peer = O.role == ONLINE_CLIENT ? 0 : peer_for(p);
    return peer >= 0 ? net_stats(O.host, peer).loss : 0.0f;
}
bool online_kick(int p, const char* reason) {
    int peer = is_server() ? peer_for(p) : -1;
    if (peer < 0) return false;
    MSG(m);
    nw_u8(&m, M_KICK), nw_str(&m, reason ? reason : "");
    send_msg(peer, NET_RELIABLE, &m);
    net_flush(O.host, O.now);
    net_disconnect(O.host, peer);
    return true;
}

// --- the advanced sessions' host side and the rest (online.h) -----------------------------------------------------------
const OnlineSessionSettings* online_current_settings(void) { return &O.cur; }
OnlineSessionState online_session_state(void) { return online_active() ? O.state : ONLINE_STATE_NONE; }
int online_open_slots(void) {
    if (!online_active()) return 0;
    int n = O.cur.max_players - used_players();
    return n > 0 ? n : 0;
}
bool online_update_session(const OnlineSessionSettings* st) {
    if (!is_server() || !st) return false;
    OnlineSessionSettings keep = O.cur;
    O.cur = *st;
    O.cur.lan = keep.lan, O.cur.dedicated = keep.dedicated, O.cur.build_id = keep.build_id; // (the transport stays)
    if (!O.cur.name[0]) snprintf(O.cur.name, sizeof O.cur.name, "%s", keep.name);
    if (O.cur.max_players <= 0) O.cur.max_players = keep.max_players;
    if (O.cur.max_players > ONLINE_MAX_PLAYERS) O.cur.max_players = ONLINE_MAX_PLAYERS;
    if (O.cur.private_slots < 0) O.cur.private_slots = 0;
    if (O.cur.max_players + O.cur.private_slots > ONLINE_MAX_PLAYERS) O.cur.private_slots = ONLINE_MAX_PLAYERS - O.cur.max_players;
    if (O.cur.nprops < 0 || O.cur.nprops > ONLINE_MAX_PROPS) O.cur.nprops = keep.nprops, memcpy(O.cur.props, keep.props, sizeof keep.props);
    publish();
    event(ONLINE_EV_SESSION_UPDATED, 0, true, O.cur.name);
    return true;
}
bool online_start_session(void) {
    if (!is_server()) return false;
    session_state_set(ONLINE_STATE_IN_PROGRESS);
    publish();
    return true;
}
bool online_end_session(void) {
    if (!is_server()) return false;
    session_state_set(ONLINE_STATE_ENDED);
    publish();
    return true;
}
bool online_set_joinable(bool on) {
    if (!is_server()) return false;
    O.cur.joinable = on;
    publish();
    return true;
}
bool online_set_property(const char* key, const char* value) {
    if (online_active()) {
        if (!is_server() || !prop_set(O.cur.props, &O.cur.nprops, key, value)) return false;
        publish();
        event(ONLINE_EV_SESSION_UPDATED, 0, true, O.cur.name);
        return true;
    }
    return prop_set(O.staged, &O.nstaged, key, value); // (the next Create Advanced Session's)
}
bool online_remove_property(const char* key) {
    if (online_active()) {
        if (!is_server() || !prop_remove(O.cur.props, &O.cur.nprops, key)) return false;
        if (O.steam_session) { // (its value blanked in the lobby: re_props no longer lists it)
            char k[48];
            snprintf(k, sizeof k, "p_%s", key);
            steam_lobby_set(k, "");
        }
        publish();
        event(ONLINE_EV_SESSION_UPDATED, 0, true, O.cur.name);
        return true;
    }
    return prop_remove(O.staged, &O.nstaged, key);
}
const char* online_get_property(const char* key, bool* found) {
    OnlineProp* pr = key ? (online_active() ? prop_find(O.cur.props, O.cur.nprops, key) : prop_find(O.staged, O.nstaged, key)) : NULL;
    if (found) *found = pr != NULL;
    return pr ? pr->value : "";
}
bool online_ban(int p, const char* reason) {
    int peer = is_server() && p != O.local ? peer_for(p) : -1;
    if (peer < 0 || O.nbans >= ONLINE_MAX_BANS) return false;
    O.bans[O.nbans].steam = O.players[p].steam;
    O.bans[O.nbans].addr = net_peer_addr(O.host, peer);
    snprintf(O.bans[O.nbans].name, sizeof O.bans[O.nbans].name, "%s", O.players[p].name);
    snprintf(O.bans[O.nbans].reason, sizeof O.bans[O.nbans].reason, "%s", reason ? reason : "");
    O.nbans++;
    char t[128];
    snprintf(t, sizeof t, "banned%s%s", reason && reason[0] ? ": " : "", reason ? reason : "");
    return online_kick(p, t);
}
int online_ban_count(void) { return O.nbans; }
const char* online_ban_name(int i) { return i >= 0 && i < O.nbans ? O.bans[i].name : ""; }
bool online_unban(int i) {
    if (i < 0 || i >= O.nbans) return false;
    O.bans[i] = O.bans[--O.nbans];
    return true;
}
void online_unban_all(void) { O.nbans = 0; }
const char* online_player_unique_id(int p) {
    static char t[96];
    if (p < 0 || p >= ONLINE_MAX_PLAYERS || (!O.players[p].used && !(O.role == ONLINE_STANDALONE && p == 0))) return "";
    uint64_t id = O.role == ONLINE_STANDALONE ? steam_user_id() : O.players[p].steam;
    if (id) snprintf(t, sizeof t, "steam:%llu", (unsigned long long)id);
    else snprintf(t, sizeof t, "lan:%s", O.role == ONLINE_STANDALONE ? local_name() : O.players[p].name);
    return t;
}
bool online_friend_in_session(int f) {
    uint64_t id = steam_friend(f);
    return id && steam_friend_lobby(id) != 0;
}
bool online_join_friend(World* w, int f, const char* password) {
    (void)w;
    uint64_t lobby = steam_friend_lobby(steam_friend(f));
    if (!lobby) return join_failed("the friend isn't in a session of this game");
    set_error("");
    O.join_pw = pw_hash(password), O.join_invite = true;
    return steam_lobby_join(lobby);
}
bool online_invite_friend(int f) {
    if (!online_active() || !O.cur.allow_invites) return false;
    return steam_invite_friend(steam_friend(f));
}
void online_set_auto_accept_invites(bool on) { O.no_auto_invites = !on; }
bool online_accept_invite(World* w) {
    (void)w;
    if (!O.pending_invite) return false;
    O.join_pw = 0, O.join_invite = true;
    bool ok = steam_lobby_join(O.pending_invite);
    O.pending_invite = 0;
    return ok;
}

// --- Lag compensation (notas-dev/online.md): the server tests a shot against where the shooter saw the others ---------
float bp_trace_boxes(const float a[3], const float b[3], float radius); // (bp_vm.c: the level's boxes and landscapes)

// The segment a + (b - a) t, t in 0..1, against a capsule (axis p0..p1, radius r): the first t inside, -1 none
static float seg_capsule(const float a[3], const float d[3], const float p0[3], const float p1[3], float r) {
    // sampled along the segment, then refined: exact enough for a shot, never a root that misses a grazing hit
    float best = -1.0f, len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float ax[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, al2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
    if (len2 <= 1e-6f) return -1.0f;
    int steps = (int)fminf(256.0f, sqrtf(len2) / fmaxf(r * 0.5f, 1.0f)) + 2;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps, q[3] = { a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t };
        float u = al2 > 1e-6f ? ((q[0] - p0[0]) * ax[0] + (q[1] - p0[1]) * ax[1] + (q[2] - p0[2]) * ax[2]) / al2 : 0.0f;
        u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        float c[3] = { p0[0] + ax[0] * u - q[0], p0[1] + ax[1] * u - q[1], p0[2] + ax[2] * u - q[2] };
        if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] <= r * r) {
            float lo = i ? (float)(i - 1) / steps : 0.0f, hi = t; // (the entry between the last step outside and this one)
            for (int k = 0; k < 12 && i; k++) {
                float m = (lo + hi) * 0.5f, mq[3] = { a[0] + d[0] * m, a[1] + d[1] * m, a[2] + d[2] * m };
                float mu = al2 > 1e-6f ? ((mq[0] - p0[0]) * ax[0] + (mq[1] - p0[1]) * ax[1] + (mq[2] - p0[2]) * ax[2]) / al2 : 0.0f;
                mu = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu;
                float mc[3] = { p0[0] + ax[0] * mu - mq[0], p0[1] + ax[1] * mu - mq[1], p0[2] + ax[2] * mu - mq[2] };
                if (mc[0] * mc[0] + mc[1] * mc[1] + mc[2] * mc[2] <= r * r) hi = m;
                else lo = m;
            }
            best = hi;
            break;
        }
    }
    return best;
}

bool online_lag_trace(World* w, int shooter, const float a[3], const float b[3], float radius, Entity* hit, float hit_pos[3]) {
    *hit = ENTITY_NONE;
    // the moment the shooter saw: the server's now minus half their round trip and their view's delay (the server's own
    // player, a client, a game alone: now)
    double t = O.now;
    if (is_server() && shooter >= 0 && shooter < ONLINE_MAX_PLAYERS && O.players[shooter].used && O.players[shooter].peer >= 0) {
        NetStats ns = net_stats(O.host, O.players[shooter].peer);
        t -= fmin((ns.rtt_ms * 0.5 + O.view_delay_ms[shooter]) * 0.001, 1.0); // (a second at most: no shots into the past)
    }
    Entity own = shooter >= 0 && shooter < ONLINE_MAX_PLAYERS ? O.players[shooter].pawn : ENTITY_NONE;
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float wall = bp_trace_boxes(a, b, radius); // (the level in the way: as it is now)
    float best = wall;
    for (int i = 0; i < O.nreps; i++) {
        Rep* r = &O.reps[i];
        if (!(r->flags & REP_MOVE) || r->e == own || !ecs_alive(w, r->e)) continue;
        const CTransform* tr = ecs_get(w, r->e, COMP_TRANSFORM);
        if (!tr) continue;
        float at[3];
        memcpy(at, tr->location, sizeof at);
        if (is_server() && O.hist && t < O.now) net_repl_hist_at(&O.hist[i], t, at);
        const CCharacter* c = ecs_get(w, r->e, COMP_CHARACTER);
        float cr = c && c->radius > 0.0f ? c->radius : 50.0f, hh = c && c->half_height > cr ? c->half_height - cr : 0.0f;
        float p0[3] = { at[0], at[1], at[2] - hh }, p1[3] = { at[0], at[1], at[2] + hh };
        float f = seg_capsule(a, d, p0, p1, cr + radius);
        if (f >= 0.0f && f < best) best = f, *hit = r->e;
    }
    for (int k = 0; k < 3; k++) hit_pos[k] = a[k] + d[k] * best;
    return *hit != ENTITY_NONE;
}

bool online_has_authority(World* w, Entity e) {
    (void)w, (void)e;
    if (O.role == ONLINE_STANDALONE || is_server()) return true;
    Rep* r = rep_of(e);
    return !r; // (a client: authority over what isn't replicated, its own local things)
}
bool online_is_locally_controlled(World* w, Entity e) {
    if (O.role == ONLINE_STANDALONE) return e == bp_player(w);
    Rep* r = rep_of(e);
    return r ? r->owner == O.local && O.local >= 0 : e == bp_player(w);
}
void online_set_replicates(World* w, Entity e, bool on) {
    if (!is_server()) return;
    Rep* r = rep_of(e);
    if (on && !r) {
        r = rep_add(e, ++O.next_id, REP_MOVE | REP_LEVEL, -1);
        if (r) {
            snprintf(r->name, sizeof r->name, "%s", name_of(w, e));
            MSG(l);
            nw_u8(&l, M_SPAWN), nw_u32(&l, 0), nw_u32(&l, r->id), nw_str(&l, r->name), nw_u8(&l, 0xFF);
            broadcast(NET_RELIABLE, &l, -1);
        }
    } else if (!on && r) {
        rep_remove(r);
    }
}
Entity online_spawn(World* w, uint32_t class_hash, const float loc[3], const float rot[3], int owner) {
    Entity e = spawn_class(w, class_hash, loc, rot);
    if (!e || !is_server()) return e;
    Rep* r = rep_add(e, ++O.next_id, REP_MOVE | REP_SPAWNED, owner);
    if (r) r->cls = class_hash, send_spawn(-1, r, w);
    return e;
}
void online_destroy(World* w, Entity e) {
    Rep* r = rep_of(e);
    if (r && is_server()) {
        MSG(m);
        nw_u8(&m, M_DESTROY), nw_u32(&m, r->id);
        broadcast(NET_RELIABLE, &m, -1);
    }
    if (r) rep_remove(r);
    if (ecs_alive(w, e)) ecs_destroy(w, e);
}

bool online_rpc(World* w, Entity e, OnlineRpc mode, const char* evname, bool reliable, float f, const float vec[3], const char* text) {
    float v0[3] = { 0, 0, 0 };
    if (!vec) vec = v0;
    // standalone, or the server running it on itself: right here
    bool here = O.role == ONLINE_STANDALONE || (is_server() && (mode == ONLINE_RPC_SERVER || mode == ONLINE_RPC_MULTICAST)) ||
                (mode == ONLINE_RPC_OWNER && online_is_locally_controlled(w, e));
    Rep* r = rep_of(e);
    if (O.role != ONLINE_STANDALONE && O.host) {
        MSG(m);
        nw_u8(&m, M_RPC), nw_u32(&m, r ? r->id : 0), nw_u8(&m, (uint8_t)mode), nw_u8(&m, reliable), nw_str(&m, evname);
        nw_str(&m, r ? "" : name_of(w, e));
        if (is_server()) nw_u8(&m, (uint8_t)(O.local >= 0 ? O.local : 0));
        nw_f32(&m, f);
        for (int k = 0; k < 3; k++) nw_f32(&m, vec[k]);
        nw_str(&m, text ? text : "");
        NetChannel ch = reliable ? NET_RELIABLE : NET_UNRELIABLE;
        if (O.role == ONLINE_CLIENT && mode == ONLINE_RPC_SERVER) send_msg(0, ch, &m);
        else if (is_server() && mode == ONLINE_RPC_MULTICAST) broadcast(ch, &m, -1);
        else if (is_server() && mode == ONLINE_RPC_OWNER && r && r->owner >= 0 && r->owner != O.local) send_msg(peer_for(r->owner), ch, &m);
    }
    if (here) {
        O.rpc_f = f;
        memcpy(O.rpc_v, vec, sizeof O.rpc_v);
        snprintf(O.rpc_text, sizeof O.rpc_text, "%s", text ? text : "");
        O.rpc_sender = online_local_player();
        bp_call_event(w, e, evname);
    }
    return true;
}
void online_rpc_args(float* f, float vec[3], const char** text, int* sender) {
    if (f) *f = O.rpc_f;
    if (vec) memcpy(vec, O.rpc_v, sizeof O.rpc_v);
    if (text) *text = O.rpc_text;
    if (sender) *sender = O.rpc_sender;
}

bool online_replicate_var(World* w, Entity e, const char* var) {
    if (!is_server()) return false;
    Rep* r = rep_of(e); // (none: the actor by its name)
    BpValue v[4];
    uint32_t width = 0;
    char text[256];
    bool is_text = false;
    if (!bp_get_instance_var(w, e, var, v, 4, &width, text, sizeof text, &is_text)) return false;
    MSG(m);
    nw_u8(&m, M_VAR), nw_u32(&m, r ? r->id : 0), nw_str(&m, r ? "" : name_of(w, e)), nw_str(&m, var), nw_u8(&m, is_text ? 1 : 0), nw_u8(&m, (uint8_t)width);
    if (is_text) nw_str(&m, text);
    else
        for (uint32_t i = 0; i < width; i++)
            for (int k = 0; k < 4; k++) nw_f32(&m, v[i].v[k]);
    broadcast(NET_RELIABLE, &m, -1);
    char rep[80];
    snprintf(rep, sizeof rep, "OnRep_%s", var);
    if (O.role == ONLINE_LISTEN) bp_call_event(w, e, rep); // (the host sees it too)
    return true;
}

bool online_chat(const char* text) {
    if (!O.host || !text || !text[0]) return false;
    MSG(m);
    nw_u8(&m, M_CHAT);
    if (is_server()) nw_u8(&m, (uint8_t)(O.local >= 0 ? O.local : 0));
    nw_str(&m, text);
    if (is_server()) broadcast(NET_RELIABLE, &m, -1);
    else send_msg(0, NET_RELIABLE, &m);
    event(ONLINE_EV_CHAT, online_local_player(), true, text);
    return true;
}

void online_set_sim(const NetSim* sim) {
    ensure_defaults();
    O.set.sim = sim ? *sim : (NetSim){ 0 };
    if (O.host) net_set_sim(O.host, &O.set.sim);
}

OnlineStats online_stats(void) {
    OnlineStats s = { 0 };
    s.refused = O.refused;
    s.pred_moves = P.next ? P.next - 1 : 0, s.pred_acks = P.acks, s.pred_corrections = P.corrections, s.pred_replayed = P.replayed;
    s.pred_err_max = P.max_err, s.pred_err_mean = P.acks ? P.sum_err / (float)P.acks : 0.0f;
    for (int i = 0; i < 16; i++) s.moves_applied += P.last_applied[i];
    if (!O.host) return s;
    s.peers = net_peer_count(O.host);
    s.replicated = O.nreps;
    s.snapshots = O.snapshots, s.corrections = O.corrections;
    double tick = 1.0 / (O.set.tick_rate > 0 ? O.set.tick_rate : 30);
    s.interp_ms = O.delay_s > 0.0 ? (float)(O.delay_s * 1000.0) : (float)fmax(O.set.interp_ms, (2.0 * tick + 2.0 * O.jitter_ms * 0.001) * 1000.0);
    s.resends = O.resends;
    int n = 0;
    for (int p = 0; p < NET_MAX_PEERS; p++) {
        if (!net_connected(O.host, p)) continue;
        NetStats ns = net_stats(O.host, p);
        s.rtt_ms += ns.rtt_ms, s.jitter_ms += ns.jitter_ms, s.loss += ns.loss, s.kbps_out += ns.kbps_out, s.kbps_in += ns.kbps_in;
        n++;
    }
    if (n) s.rtt_ms /= n, s.jitter_ms /= n, s.loss /= n;
    return s;
}

// --- saves: the file, its checksum, its versions, Steam Cloud live in save_progress.c (the R7 module 4); these keep the
// old names (Save Game to Slot / Load Game from Slot on an actor or a Save Game object) ---
bool online_cloud_saves(void) { return O.set.steam_cloud && steam_cloud_enabled(); }
bool online_save(World* w, Entity e, const char* slot) { return progress_save_object(w, e, slot); }
bool online_load(World* w, Entity e, const char* slot) { return progress_load_object(w, e, slot); }
bool online_save_exists(const char* slot) { return progress_slot_exists(slot); }
bool online_save_delete(const char* slot) { return progress_slot_delete(slot); }

// Project Settings > Online into the settings (the editor's Play emulates the network as set there)
void online_apply_project(void) {
    const ProjectSettings* p = project();
    OnlineSettings s = { 0 };
    s.port = (uint16_t)(p->net_port > 0 && p->net_port < 65536 ? p->net_port : DEFAULT_PORT);
    s.max_players = p->net_max_players, s.tick_rate = p->net_tick_rate, s.interp_ms = p->net_interp_ms;
    s.relevancy_cm = p->net_relevancy_m * 100.0f, s.budget_kbps = p->net_budget_kbps;
    s.use_steam = p->online_steam, s.steam_app_id = p->steam_app_id, s.steam_cloud = p->steam_cloud;
    s.sim = (NetSim){ p->emu_latency_ms, p->emu_jitter_ms, p->emu_loss, 0.0f };
    online_configure(&s);
    if (O.host) net_set_sim(O.host, &O.set.sim);
}

// A level was opened while in a session (Open Level): the old level's actors are gone. The server numbers the new
// level's and sends every client there (Unreal's ServerTravel); a client, arrived, says HELLO again (its new pawn)
void online_level_loaded(World* w, const char* level) {
    if (!O.host || O.role == ONLINE_STANDALONE) return;
    O.nreps = 0;
    for (int p = 0; p < ONLINE_MAX_PLAYERS; p++) {
        if (O.slots[p]) memset(O.slots[p], 0, sizeof(NetReplSlot) * MAX_REP);
        if (O.logs[p]) memset(O.logs[p], 0, sizeof(NetReplLog));
    }
    if (O.hist) memset(O.hist, 0, sizeof(NetReplHist) * MAX_REP);
    if (is_server()) {
        O.next_id = 0;
        collect_level_reps(w);
        for (int p = 0; p < ONLINE_MAX_PLAYERS; p++) {
            Player* pl = &O.players[p];
            if (!pl->used) continue;
            if (pl->peer < 0) { // the host's own pawn
                pl->pawn = bp_player(w);
                Rep* r = rep_of(pl->pawn);
                if (r) r->owner = (int8_t)p, pl->pawn_id = r->id;
                continue;
            }
            pl->pawn = ENTITY_NONE, pl->pawn_id = 1; // (not 0: a re-join, not a new player)
            MSG(m);
            nw_u8(&m, M_TRAVEL), nw_str(&m, level ? level : "");
            send_msg(pl->peer, NET_RELIABLE, &m);
        }
    } else {
        MSG(m);
        nw_u8(&m, M_HELLO), nw_str(&m, local_name()), nw_u64(&m, steam_user_id());
        send_msg(0, NET_RELIABLE, &m);
    }
}

// The frame's two calls, timed (the Debug Viewer's Online list)
static double g_pre_ms;
void online_pre_tick(World* w, double now) {
    double t0 = re_time_seconds();
    pre_tick(w, now);
    g_pre_ms = (re_time_seconds() - t0) * 1000.0;
}
void online_post_tick(World* w, double now) {
    static bool hooked; // (B13: the possessed Character's moves recorded for the prediction)
    if (!hooked) bp_set_move_hook(pred_record), hooked = true;
    double t0 = re_time_seconds();
    post_tick(w, now);
    g_cpu_ms = g_pre_ms + (re_time_seconds() - t0) * 1000.0;
}

static void dv_online(DvList* l, const DvContext* c) {
    (void)c;
    l->ms = g_cpu_ms;
    if (!O.host) {
        snprintf(l->note, sizeof l->note, "%s", "Not in a session (Create Session / Join Session).");
        return;
    }
    OnlineStats s = online_stats();
    snprintf(l->note, sizeof l->note, "%s, %d replicated, interpolation %.0f ms, %u corrections; CPU of the network a frame",
             O.role == ONLINE_CLIENT ? "Client" : O.role == ONLINE_LISTEN ? "Listen server" : "Dedicated server", s.replicated, s.interp_ms,
             s.corrections);
    for (int p = 0; p < ONLINE_MAX_PLAYERS; p++) {
        if (!O.players[p].used) continue;
        int peer = O.role == ONLINE_CLIENT ? (p == O.local ? -1 : 0) : O.players[p].peer;
        char info[48] = "this machine";
        if (peer >= 0 && net_connected(O.host, peer)) {
            NetStats ns = net_stats(O.host, peer);
            snprintf(info, sizeof info, "%.0f ms, %.1f%% lost, %.0f/%.0f kbps", ns.rtt_ms, ns.loss * 100.0f, ns.kbps_out, ns.kbps_in);
        }
        dv_add(l, O.players[p].name, info, 0.0f, O.players[p].pawn);
    }
}
void online_register_debug(void) { dv_register("Online", dv_online); }
