// Online play (notas-dev/online.md): sessions, players, replication, RPCs, Steam, saves. Unreal's model, simplified:
//   * the server has the authority (a listen server: a player hosts; a dedicated one: --server); the clients send
//     their own pawn's movement (no input lag: it moves on their screen at once; the server checks its speed and
//     corrects a cheat or a glitch) and run their RPCs on the server;
//   * replicated actors: every Character, every Simulate Physics actor, and any actor set so (Set Replicates); the
//     server sends their state at the tick rate in quantized snapshots (position 1/8 cm, angles 16 bits), only what
//     changed (and everything once a second), by priority and distance within a bandwidth budget per client;
//   * the clients draw the others' actors in the past (an interpolation buffer as deep as the jitter needs, at least
//     two snapshots): smooth under lag and lost packets; a missing snapshot extrapolates by the velocity (250 ms at most);
//   * RPCs: Custom Events run on the server, on every machine (multicast) or on the owner; reliable or not; a float, a
//     vector, a text with them; replicated variables (Replicate Variable: the server's value to every client, then
//     OnRep_<Name>); chat;
//   * sessions: LAN (UDP, found by broadcast) or Steam (a lobby, P2P through Valve's relay, invites from the overlay);
//   * the network emulation (latency, jitter, loss) for the developer to test the bad cases.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"
#include "net.h"

#define ONLINE_MAX_PLAYERS 64
#define ONLINE_PROTOCOL 0x52450004u // (the game's network version: older builds are refused; 2: the advanced sessions' hello; 3: compact states, the confirmed baseline, the view delay; 4: B13's predicted moves)
#define ONLINE_MAX_PROPS 16   // a session's own properties (Advanced Sessions' Extra Settings)
#define ONLINE_MAX_FILTERS 8  // a search's property filters
#define ONLINE_MAX_BANS 64

typedef enum OnlineRole { ONLINE_STANDALONE = 0, ONLINE_LISTEN, ONLINE_CLIENT, ONLINE_DEDICATED } OnlineRole;

typedef struct OnlineSettings {
    uint16_t port;          // UDP (LAN, IP): 7777
    int max_players;
    int tick_rate;          // snapshots a second (30)
    float interp_ms;        // the interpolation delay's floor (100)
    float relevancy_cm;     // actors farther than this from a client's pawn aren't sent (0: all)
    int budget_kbps;        // per client (0: no cap)
    bool use_steam;         // sessions through Steam when it is up
    uint32_t steam_app_id;  // 0: steam_appid.txt / the launch by Steam (480: Spacewar, the tests)
    bool steam_cloud;       // Save Game also into Steam Cloud
    NetSim sim;             // the network emulation (the editor's Play)
} OnlineSettings;

// A session's own property (Advanced Sessions' Extra Settings): a key and a value, as text. Filters compare them as numbers
// when both sides are numbers ("3" >= "2"), else as text
typedef struct OnlineProp { char key[32], value[64]; } OnlineProp;

typedef enum OnlineVisibility { ONLINE_VIS_PUBLIC = 0, ONLINE_VIS_FRIENDS, ONLINE_VIS_INVITE_ONLY } OnlineVisibility;
// Unreal's EOnlineSessionState, simplified: Pending is the lobby before Start Session
typedef enum OnlineSessionState { ONLINE_STATE_NONE = 0, ONLINE_STATE_CREATING, ONLINE_STATE_PENDING, ONLINE_STATE_IN_PROGRESS, ONLINE_STATE_ENDED } OnlineSessionState;

// Everything a host decides (Create Advanced Session, Update Session): Unreal's FOnlineSessionSettings
typedef struct OnlineSessionSettings {
    char name[64];
    int max_players;              // public slots (the host counts)
    int private_slots;            // more slots only for who comes by an invite, a friend's session or the address
    bool lan;                     // the local network; off: a Steam lobby when Steam is up (else LAN)
    OnlineVisibility visibility;  // Steam: public / friends only / invite only lobby; LAN: invite only isn't advertised
    bool advertise;               // found by Find Sessions (off: only invites and the address)
    bool allow_join_in_progress;  // players may come after Start Session
    bool allow_invites;           // the overlay's invites and Invite Friend To Session
    bool joinable;                // Set Session Joinable: off locks the session (nobody else comes in)
    bool dedicated;               // (a dedicated server: set by online_host_dedicated)
    char password[64];            // empty: none (the clients send its hash, never the text; Steam's lobby only says "has one")
    char map[64], game_mode[64];  // shown in the searches, filterable
    uint32_t build_id;            // the game's build (Project Settings: name + version): Same Build Only hides the others
    OnlineProp props[ONLINE_MAX_PROPS];
    int nprops;
} OnlineSessionSettings;
void online_settings_default(OnlineSessionSettings* s); // 4 players, public, advertised, joinable, join in progress, invites

typedef struct OnlineSession {
    char name[64], level[64], host[64]; // (level: the map name)
    int players, max_players;
    float ping_ms;          // LAN: the answer's time; Steam: -1 (not measured)
    NetAddr addr;           // LAN: the server; Steam: the lobby in addr.id
    bool steam;
    // what the advanced search reads (older servers: zeros)
    char game_mode[64];
    int private_slots;
    bool dedicated, has_password, in_progress, joinable, allow_join_in_progress;
    uint32_t build_id, session_id;
    OnlineProp props[ONLINE_MAX_PROPS];
    int nprops;
} OnlineSession;

// The advanced search (Find Sessions Advanced): every filter is applied to the results the same way on the LAN and on
// Steam (Steam also gets its own copy of them, so fewer lobbies come back)
typedef enum OnlineCompare { ONLINE_CMP_EQ = 0, ONLINE_CMP_NE, ONLINE_CMP_LT, ONLINE_CMP_LE, ONLINE_CMP_GT, ONLINE_CMP_GE, ONLINE_CMP_CONTAINS } OnlineCompare;
typedef struct OnlineFilter { char key[32], value[64]; OnlineCompare op; } OnlineFilter;
typedef enum OnlineServerType { ONLINE_SERVERS_ALL = 0, ONLINE_SERVERS_LISTEN, ONLINE_SERVERS_DEDICATED } OnlineServerType;
typedef struct OnlineSearch {
    bool lan;
    int max_results;            // 0: 50
    OnlineServerType server_type;
    int min_open_slots;         // public slots open (0: any)
    bool exclude_full, empty_only, non_empty_only, hide_passworded, hide_in_progress, same_build_only;
    float max_ping_ms;          // 0: any (a session whose ping isn't known passes)
    OnlineFilter filters[ONLINE_MAX_FILTERS];
    int nfilters;
} OnlineSearch;

typedef enum OnlineEventType {
    ONLINE_EV_NONE = 0, ONLINE_EV_SESSION_CREATED, ONLINE_EV_SESSIONS_FOUND, ONLINE_EV_JOINED, ONLINE_EV_PLAYER_JOINED,
    ONLINE_EV_PLAYER_LEFT, ONLINE_EV_ERROR, ONLINE_EV_CHAT, ONLINE_EV_LEADERBOARD, ONLINE_EV_OVERLAY,
    ONLINE_EV_SESSION_UPDATED,  // ok: the settings reached the session (the host; the clients when they arrive)
    ONLINE_EV_SESSION_STATE,    // i: OnlineSessionState (Start / End Session, on the host and every client)
    ONLINE_EV_INVITE,           // text: who invited, ok: joining by itself (Set Auto Accept Invites)
    ONLINE_EV_JOIN_REFUSED,     // (the server) text: the player's name, then ": " and the reason
} OnlineEventType;
typedef struct OnlineEvent { OnlineEventType type; int i; bool ok; char text[128]; } OnlineEvent;

void online_configure(const OnlineSettings* s); // the project's (Project Settings > Online)
const OnlineSettings* online_settings(void);
void online_init(void);      // Steam when set (at startup)
void online_shutdown(void);

// Sessions
bool online_host(World* w, const char* name, int max_players, bool lan); // a listen server (lan false: Steam if up)
bool online_host_dedicated(World* w, uint16_t port);                     // no local player
bool online_find(bool lan);                                               // ONLINE_EV_SESSIONS_FOUND
int online_session_count(void);
const OnlineSession* online_session(int i);
bool online_join(World* w, int i);
bool online_join_address(World* w, const char* address);                 // "1.2.3.4:7777", "localhost"
void online_leave(World* w);
OnlineRole online_role(void);
bool online_active(void); // in a session (hosting or connected)

// Advanced sessions (Unreal's Advanced Sessions plugin): every setting when creating, searching and joining
bool online_host_advanced(World* w, const OnlineSessionSettings* s);
bool online_find_advanced(const OnlineSearch* s);
bool online_finding(void);
bool online_join_ex(World* w, int i, const char* password);              // a found session (refused: ONLINE_EV_JOINED with why)
bool online_join_address_ex(World* w, const char* address, const char* password);
bool online_update_session(const OnlineSessionSettings* s); // (the host) everything at once; the clients get it too
const OnlineSessionSettings* online_current_settings(void); // the host's; a client's copy of the server's (no password)
int online_open_slots(void);                                 // the current session's public slots left
OnlineSessionState online_session_state(void);
bool online_start_session(void); // (the host) the match began: without Allow Join In Progress nobody else comes in
bool online_end_session(void);   // (the host) back to the lobby
bool online_set_joinable(bool on); // (the host) off: the session is locked
// Properties: in a session the host's change it live (the clients and the searches see it); out of one they wait for
// the next Create Advanced Session
bool online_set_property(const char* key, const char* value);
bool online_remove_property(const char* key);
const char* online_get_property(const char* key, bool* found);              // the current session's
const char* online_session_property(int i, const char* key, bool* found);   // a found session's
// The next Find Sessions Advanced's property filters
bool online_search_filter_add(const char* key, const char* value, OnlineCompare op);
void online_search_filters_clear(void);
void online_search_filters(OnlineFilter* out, int* n); // (copied into an OnlineSearch)
const char* online_last_error(void); // why the last create / search / join failed ("" when it didn't)
// Bans (the host): kicked and refused when they come back (their Steam ID, else their address)
bool online_ban(int p, const char* reason);
int online_ban_count(void);
const char* online_ban_name(int i);
bool online_unban(int i);
void online_unban_all(void);
const char* online_player_unique_id(int p); // "steam:<id>" or "lan:<name>"
// Friends (Steam): a friend playing this game in a session; invites
bool online_friend_in_session(int friend_index);
bool online_join_friend(World* w, int friend_index, const char* password);
bool online_invite_friend(int friend_index);
void online_set_auto_accept_invites(bool on); // (on by default) off: ONLINE_EV_INVITE only, Accept Session Invite joins
bool online_accept_invite(World* w);

// The frame: before the Blueprints (received states, RPCs, spawns), after the movement (the others interpolated over
// what the systems did, then what this machine sends)
void online_pre_tick(World* w, double now);
void online_post_tick(World* w, double now);
bool online_poll(OnlineEvent* ev);
void online_end_play(World* w); // (Play stopped: the session closed)

// Players
int online_player_count(void);
int online_local_player(void);
const char* online_player_name(int p);
Entity online_player_pawn(World* w, int p);
float online_player_ping(int p);
float online_player_loss(int p); // 0..1
bool online_kick(int p, const char* reason);

// Replication
bool online_has_authority(World* w, Entity e);
// Lag compensation (Unreal's server-side rewind): on the server, a shot from player `shooter` tested against where the
// replicated actors were when that player saw them (half their round trip plus their interpolation delay back, a second at
// most), the level in between as it is now; Characters as their capsules, other actors as 50 cm spheres. On a client or
// alone: against them as they are. The actor hit (none: the wall or nothing) and where
bool online_lag_trace(World* w, int shooter, const float a[3], const float b[3], float radius, Entity* hit, float hit_pos[3]);
bool online_is_locally_controlled(World* w, Entity e);
void online_set_replicates(World* w, Entity e, bool on);
Entity online_spawn(World* w, uint32_t class_hash, const float loc[3], const float rot[3], int owner);
void online_destroy(World* w, Entity e);
typedef enum OnlineRpc { ONLINE_RPC_SERVER = 0, ONLINE_RPC_MULTICAST, ONLINE_RPC_OWNER } OnlineRpc;
bool online_rpc(World* w, Entity e, OnlineRpc mode, const char* event, bool reliable, float f, const float vec[3], const char* text);
void online_rpc_args(float* f, float vec[3], const char** text, int* sender); // the last RPC's values (its event reads them)
bool online_replicate_var(World* w, Entity e, const char* var);
bool online_chat(const char* text);
void online_set_sim(const NetSim* sim);

// Saves (Saved/SaveGames/<slot>.sav, and Steam Cloud when on): the actor's Blueprint variables by name
bool online_save(World* w, Entity e, const char* slot);
bool online_load(World* w, Entity e, const char* slot);
bool online_save_exists(const char* slot);
bool online_save_delete(const char* slot);
bool online_cloud_saves(void); // Steam Cloud keeps the saves (Project Settings > Online and Steam's own setting)

// The Debug Viewer and the tests
typedef struct OnlineStats {
    int peers, replicated;
    float rtt_ms, jitter_ms, loss, kbps_out, kbps_in, interp_ms;
    uint32_t snapshots, corrections, refused, resends;
    // (B13) the own Character's prediction: moves made, confirmed by the server, states taken back, moves replayed, and the
    // gap between the predicted and the server's state at a confirmation (cm); the server: moves applied
    uint32_t pred_moves, pred_acks, pred_corrections, pred_replayed, moves_applied;
    float pred_err_max, pred_err_mean;
} OnlineStats;
OnlineStats online_stats(void);
void online_apply_project(void); // Project Settings > Online into the settings
void online_level_loaded(World* w, const char* level); // Open Level in a session (the server's sends the clients there)
int online_debug_ticks(void); // (the tests: online_pre_tick's calls)
void online_register_debug(void); // the Debug Viewer's Online list
