// Steam (notas-dev/online.md): steam_api64.dll loaded at run time through Valve's flat C API (the engine builds without
// the Steamworks SDK; the game ships the DLL from the developer's SDK next to its .exe). Without the DLL or without the
// Steam client running everything here answers "not available" and the game goes on (the network falls back to UDP,
// the saves to the disk). The callbacks are dispatched by hand once a frame (steam_update) into events.
//   * the user, friends, the overlay, rich presence;
//   * lobbies (matchmaking) and P2P messages through Valve's relay (no ports to open, the IP hidden): a transport for
//     net.h (steam_net_open);
//   * Steam Cloud (remote storage): the saves;
//   * achievements, stats, leaderboards;
//   * apps: DLC owned, the game's language, the App ID.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "net.h"

typedef enum SteamEventType {
    STEAM_EV_NONE = 0,
    STEAM_EV_LOBBY_CREATED,      // a: the lobby (0: failed)
    STEAM_EV_LOBBY_ENTERED,      // a: the lobby, i: 1 ok / 0 refused
    STEAM_EV_LOBBY_LIST,         // i: lobbies found (steam_lobby_found)
    STEAM_EV_LOBBY_MEMBER,       // a: the lobby, b: the member, i: 1 joined / 0 left
    STEAM_EV_LOBBY_DATA,         // a: the lobby
    STEAM_EV_LOBBY_CHAT,         // a: the lobby, b: the sender, text: the message
    STEAM_EV_JOIN_REQUESTED,     // a: the lobby, b: the friend (a friend's invite accepted in the overlay)
    STEAM_EV_OVERLAY,            // i: 1 shown / 0 hidden
    STEAM_EV_STATS_STORED,       // i: 1 ok
    STEAM_EV_ACHIEVEMENT,        // text: the achievement stored (unlocked or progress shown)
    STEAM_EV_LEADERBOARD_FOUND,  // a: the leaderboard handle (0: none), text: its name
    STEAM_EV_LEADERBOARD_SCORES, // i: the entries downloaded (steam_leaderboard_entry), text: the board's name
    STEAM_EV_LEADERBOARD_UPLOADED, // i: 1 ok, b: the new global rank
    STEAM_EV_P2P_REQUEST,        // a: the user (accepted automatically when in the same lobby)
} SteamEventType;

typedef struct SteamEvent {
    SteamEventType type;
    uint64_t a, b;
    int i;
    char text[128];
} SteamEvent;

bool steam_init(uint32_t app_id); // app_id 0: from steam_appid.txt / the launch by Steam; writes steam_appid.txt when given
void steam_shutdown(void);
bool steam_available(void);
const char* steam_status(void);   // why not available (or "ok")
void steam_update(void);          // the callbacks (once a frame)
bool steam_poll(SteamEvent* ev);

uint64_t steam_user_id(void);
const char* steam_user_name(void);
uint32_t steam_app_id(void);
const char* steam_language(void);   // the game's language (Steam's setting)
bool steam_dlc_installed(uint32_t app);
bool steam_overlay_enabled(void);
void steam_overlay(const char* dialog); // "friends", "community", "players", "settings", "achievements", "stats"
void steam_overlay_store(uint32_t app);
void steam_overlay_invite(void);        // invite friends into the current lobby
bool steam_rich_presence(const char* key, const char* value);

int steam_friend_count(void);
uint64_t steam_friend(int i);
const char* steam_friend_name(uint64_t id);
int steam_friend_state(uint64_t id); // 0 offline, 1 online, 2 busy, 3 away, 4 snooze, 5 looking to trade, 6 to play
bool steam_invite_friend(uint64_t id); // into the current lobby

// Lobbies: type 0 private, 1 friends only, 2 public, 3 invisible
bool steam_lobby_create(int type, int max_members);
bool steam_lobby_join(uint64_t lobby);
void steam_lobby_leave(void);
uint64_t steam_lobby_current(void);
bool steam_lobby_find(const char* key, const char* value, int max_results); // STEAM_EV_LOBBY_LIST
uint64_t steam_lobby_found(int i);
bool steam_lobby_set(const char* key, const char* value);           // (the owner)
const char* steam_lobby_get(uint64_t lobby, const char* key);
int steam_lobby_members(uint64_t lobby);
uint64_t steam_lobby_member(uint64_t lobby, int i);
uint64_t steam_lobby_owner(uint64_t lobby);
int steam_lobby_max(uint64_t lobby);
bool steam_lobby_chat(const char* text);
bool steam_lobby_set_joinable(bool on);
bool steam_lobby_set_type(int type);        // (the owner) 0 private, 1 friends only, 2 public, 3 invisible
bool steam_lobby_set_limit(int max_members); // (the owner)
// The advanced search: Steam's own filters (numeric or text, its ELobbyComparison: -2 <=, -1 <, 0 =, 1 >, 2 >=, 3 !=),
// lobbies with at least min_slots open (0: any)
typedef struct SteamLobbyFilter { const char* key; const char* value; int number; int cmp; bool numeric; } SteamLobbyFilter;
bool steam_lobby_find_ex(const SteamLobbyFilter* f, int nf, int min_slots, int max_results); // STEAM_EV_LOBBY_LIST
uint64_t steam_friend_lobby(uint64_t id); // the lobby a friend is in while playing this game (0: none)

// P2P through Valve's relay as a net.h transport (addresses: NET_ADDR_STEAM, the SteamID)
bool steam_net_open(NetTransport* t);

// Steam Cloud
bool steam_cloud_enabled(void);
bool steam_cloud_write(const char* file, const void* data, int len);
int steam_cloud_read(const char* file, void* data, int cap); // bytes read (0: none)
int steam_cloud_size(const char* file);
bool steam_cloud_exists(const char* file);
bool steam_cloud_delete(const char* file);
bool steam_cloud_quota(uint64_t* total, uint64_t* available);

// Achievements and stats (Steamworks' names; stored with steam_stats_store)
bool steam_achievement_unlock(const char* name);
bool steam_achievement_clear(const char* name);
bool steam_achievement_get(const char* name, bool* achieved);
bool steam_achievement_progress(const char* name, uint32_t current, uint32_t max);
bool steam_stat_set_int(const char* name, int32_t v);
bool steam_stat_set_float(const char* name, float v);
bool steam_stat_get_int(const char* name, int32_t* v);
bool steam_stat_get_float(const char* name, float* v);
bool steam_stats_store(void);
bool steam_stats_reset(bool achievements_too);

// Leaderboards (asynchronous: the events)
bool steam_leaderboard_find(const char* name, bool create); // created descending, numeric
bool steam_leaderboard_upload(const char* name, int32_t score, bool keep_best);
bool steam_leaderboard_download(const char* name, int kind, int start, int end); // kind 0 global, 1 around the user, 2 friends
bool steam_leaderboard_entry(int i, uint64_t* user, int32_t* rank, int32_t* score); // of the last download
