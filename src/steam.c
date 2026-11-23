// Steam through the flat API of steam_api64.dll, loaded at run time (steam.h). The interfaces' accessors carry their
// version in the name (SteamAPI_SteamUser_v023...): each is looked up from v030 down, so any recent SDK's DLL works.
// The callbacks are dispatched by hand (SteamAPI_ManualDispatch_*): their structs are read at the offsets Valve's
// headers give them (8-byte packing on Windows).
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#endif

#include <stdio.h>
#include <string.h>

#include "steam.h"

typedef uint64_t u64;
typedef void* Iface;

// callbacks (k_i*Callbacks + n) and their structs' offsets
enum {
    CB_PERSONA_CHANGE = 304, CB_OVERLAY = 331, CB_LOBBY_JOIN_REQUESTED = 333, CB_RICH_JOIN_REQUESTED = 337,
    CB_LOBBY_ENTER = 504, CB_LOBBY_DATA = 505, CB_LOBBY_CHAT_UPDATE = 506, CB_LOBBY_CHAT_MSG = 507, CB_LOBBY_MATCH_LIST = 510,
    CB_LOBBY_CREATED = 513, CB_CALL_COMPLETED = 703, CB_STATS_RECEIVED = 1101, CB_STATS_STORED = 1102,
    CB_ACHIEVEMENT_STORED = 1103, CB_LB_FIND = 1104, CB_LB_SCORES = 1105, CB_LB_UPLOADED = 1106, CB_NETMSG_REQUEST = 1251,
};

typedef struct CallbackMsg { int32_t user; int32_t id; uint8_t* param; int32_t size; } CallbackMsg;
typedef struct NetIdentity { uint8_t raw[136]; } NetIdentity;
typedef struct NetMessage { void* data; int32_t size; uint32_t conn; NetIdentity peer; } NetMessage; // (its first fields)
typedef struct LbEntry { u64 user; int32_t rank, score, details; u64 ugc; } LbEntry;

static struct {
    void* dll; // (HMODULE on Windows; none elsewhere)
    bool ok;
    char status[160];
    int32_t pipe;
    Iface user, friends, utils, mm, stats, apps, storage, netmsg, netutils;
    // the flat functions
    bool (*Init)(void);
    int (*InitFlat)(char* err);
    void (*Shutdown)(void);
    bool (*IsSteamRunning)(void);
    int32_t (*GetPipe)(void);
    void (*MD_Init)(void);
    void (*MD_RunFrame)(int32_t);
    bool (*MD_Next)(int32_t, CallbackMsg*);
    void (*MD_Free)(int32_t);
    bool (*MD_CallResult)(int32_t, u64, void*, int, int, bool*);
    u64 (*User_GetSteamID)(Iface);
    const char* (*Fr_GetPersonaName)(Iface);
    int (*Fr_GetFriendCount)(Iface, int);
    u64 (*Fr_GetFriendByIndex)(Iface, int, int);
    const char* (*Fr_GetFriendPersonaName)(Iface, u64);
    int (*Fr_GetFriendPersonaState)(Iface, u64);
    void (*Fr_ActivateGameOverlay)(Iface, const char*);
    void (*Fr_ActivateGameOverlayInviteDialog)(Iface, u64);
    void (*Fr_ActivateGameOverlayToStore)(Iface, uint32_t, int);
    bool (*Fr_SetRichPresence)(Iface, const char*, const char*);
    uint32_t (*Ut_GetAppID)(Iface);
    bool (*Ut_IsOverlayEnabled)(Iface);
    bool (*Ap_BIsDlcInstalled)(Iface, uint32_t);
    const char* (*Ap_GetCurrentGameLanguage)(Iface);
    u64 (*Mm_CreateLobby)(Iface, int, int);
    u64 (*Mm_JoinLobby)(Iface, u64);
    void (*Mm_LeaveLobby)(Iface, u64);
    u64 (*Mm_RequestLobbyList)(Iface);
    void (*Mm_AddStringFilter)(Iface, const char*, const char*, int);
    void (*Mm_AddResultCountFilter)(Iface, int);
    void (*Mm_AddDistanceFilter)(Iface, int);
    u64 (*Mm_GetLobbyByIndex)(Iface, int);
    bool (*Mm_SetLobbyData)(Iface, u64, const char*, const char*);
    const char* (*Mm_GetLobbyData)(Iface, u64, const char*);
    int (*Mm_GetNumLobbyMembers)(Iface, u64);
    u64 (*Mm_GetLobbyMemberByIndex)(Iface, u64, int);
    u64 (*Mm_GetLobbyOwner)(Iface, u64);
    int (*Mm_GetLobbyMemberLimit)(Iface, u64);
    bool (*Mm_SendLobbyChatMsg)(Iface, u64, const void*, int);
    int (*Mm_GetLobbyChatEntry)(Iface, u64, int, u64*, void*, int, int*);
    bool (*Mm_SetLobbyJoinable)(Iface, u64, bool);
    bool (*Mm_InviteUserToLobby)(Iface, u64, u64);
    void (*Mm_AddNumericalFilter)(Iface, const char*, int, int);
    void (*Mm_AddSlotsFilter)(Iface, int);
    bool (*Mm_SetLobbyType)(Iface, u64, int);
    bool (*Mm_SetLobbyMemberLimit)(Iface, u64, int);
    bool (*Fr_GetFriendGamePlayed)(Iface, u64, void*);
    bool (*St_SetAchievement)(Iface, const char*);
    bool (*St_ClearAchievement)(Iface, const char*);
    bool (*St_GetAchievement)(Iface, const char*, bool*);
    bool (*St_IndicateAchievementProgress)(Iface, const char*, uint32_t, uint32_t);
    bool (*St_SetStatInt32)(Iface, const char*, int32_t);
    bool (*St_SetStatFloat)(Iface, const char*, float);
    bool (*St_GetStatInt32)(Iface, const char*, int32_t*);
    bool (*St_GetStatFloat)(Iface, const char*, float*);
    bool (*St_StoreStats)(Iface);
    bool (*St_RequestCurrentStats)(Iface);
    bool (*St_ResetAllStats)(Iface, bool);
    u64 (*St_FindOrCreateLeaderboard)(Iface, const char*, int, int);
    u64 (*St_FindLeaderboard)(Iface, const char*);
    u64 (*St_UploadLeaderboardScore)(Iface, u64, int, int32_t, const int32_t*, int);
    u64 (*St_DownloadLeaderboardEntries)(Iface, u64, int, int, int);
    bool (*St_GetDownloadedLeaderboardEntry)(Iface, u64, int, LbEntry*, int32_t*, int);
    const char* (*St_GetLeaderboardName)(Iface, u64);
    bool (*Rs_FileWrite)(Iface, const char*, const void*, int32_t);
    int32_t (*Rs_FileRead)(Iface, const char*, void*, int32_t);
    bool (*Rs_FileExists)(Iface, const char*);
    bool (*Rs_FileDelete)(Iface, const char*);
    int32_t (*Rs_GetFileSize)(Iface, const char*);
    bool (*Rs_GetQuota)(Iface, u64*, u64*);
    bool (*Rs_IsCloudEnabledForAccount)(Iface);
    bool (*Rs_IsCloudEnabledForApp)(Iface);
    int (*Nm_SendMessageToUser)(Iface, const NetIdentity*, const void*, uint32_t, int, int);
    int (*Nm_ReceiveMessagesOnChannel)(Iface, int, NetMessage**, int);
    bool (*Nm_AcceptSessionWithUser)(Iface, const NetIdentity*);
    void (*Nu_InitRelayNetworkAccess)(Iface);
    void (*Id_SetSteamID64)(NetIdentity*, u64);
    u64 (*Id_GetSteamID64)(const NetIdentity*);
    void (*Msg_Release)(NetMessage*);
    // state
    u64 lobby;
    u64 found[64];
    int nfound;
    struct { u64 call; int kind; char name[64]; } calls[32];
    struct { char name[64]; u64 handle; } boards[16];
    int nboards;
    char pending_upload[64];
    int32_t pending_score;
    bool pending_keep_best, pending_download;
    int pending_kind, pending_start, pending_end;
    LbEntry entries[100];
    int nentries;
    SteamEvent ev[128];
    int nev;
    NetMessage* inbox[64];
    int ninbox, inbox_at;
} S;

#ifdef _WIN32
static void* sym(const char* name) { return S.dll ? (void*)GetProcAddress((HMODULE)S.dll, name) : NULL; }
#else
static void* sym(const char* name) { (void)name; return NULL; }
#endif
// an interface accessor whatever its version: SteamAPI_<name>_v030 .. _v001
static Iface iface(const char* name) {
    char fn[96];
    for (int v = 30; v >= 1; v--) {
        snprintf(fn, sizeof fn, "SteamAPI_%s_v%03d", name, v);
        Iface (*f)(void) = (Iface (*)(void))sym(fn);
        if (f) return f();
    }
    return NULL;
}
#define LOAD(field, name) (*(void**)&S.field = sym(name))

static void push(SteamEventType t, u64 a, u64 b, int i, const char* text) {
    if (S.nev >= (int)(sizeof S.ev / sizeof S.ev[0])) return;
    SteamEvent* e = &S.ev[S.nev++];
    memset(e, 0, sizeof *e);
    e->type = t, e->a = a, e->b = b, e->i = i;
    if (text) snprintf(e->text, sizeof e->text, "%s", text);
}

static void track(u64 call, int kind, const char* name) {
    if (!call) return;
    for (int i = 0; i < 32; i++)
        if (!S.calls[i].call) {
            S.calls[i].call = call, S.calls[i].kind = kind;
            snprintf(S.calls[i].name, sizeof S.calls[i].name, "%s", name ? name : "");
            return;
        }
}

bool steam_init(uint32_t app_id) {
    if (S.ok) return true;
    memset(&S, 0, sizeof S);
    snprintf(S.status, sizeof S.status, "steam_api64.dll not found next to the game");
#ifdef _WIN32
    S.dll = (void*)LoadLibraryA("steam_api64.dll");
#else
    S.dll = NULL; // (Steam: Windows only)
#endif
    if (!S.dll) return false;
    if (app_id) { // run outside Steam (the editor, a test): Steam reads the App ID from this file in the working folder
        FILE* f = fopen("steam_appid.txt", "wb");
        if (f) fprintf(f, "%u", app_id), fclose(f);
    }
    LOAD(InitFlat, "SteamAPI_InitFlat");
    LOAD(Init, "SteamAPI_Init");
    if (!S.Init) LOAD(Init, "SteamAPI_InitSafe");
    LOAD(Shutdown, "SteamAPI_Shutdown");
    LOAD(IsSteamRunning, "SteamAPI_IsSteamRunning");
    LOAD(GetPipe, "SteamAPI_GetHSteamPipe");
    LOAD(MD_Init, "SteamAPI_ManualDispatch_Init");
    LOAD(MD_RunFrame, "SteamAPI_ManualDispatch_RunFrame");
    LOAD(MD_Next, "SteamAPI_ManualDispatch_GetNextCallback");
    LOAD(MD_Free, "SteamAPI_ManualDispatch_FreeLastCallback");
    LOAD(MD_CallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    if (S.MD_Init) S.MD_Init(); // (before the init: the callbacks come to us, not to registered C++ objects)
    bool up = false;
    char err[1024] = { 0 };
    if (S.InitFlat) up = S.InitFlat(err) == 0;
    else if (S.Init) up = S.Init();
    if (!up) {
        snprintf(S.status, sizeof S.status, "Steam not running or no App ID (%s)", err[0] ? err : "init failed");
#ifdef _WIN32
        FreeLibrary((HMODULE)S.dll);
#endif
        S.dll = NULL;
        return false;
    }
    S.pipe = S.GetPipe ? S.GetPipe() : 0;
    S.user = iface("SteamUser"), S.friends = iface("SteamFriends"), S.utils = iface("SteamUtils");
    S.mm = iface("SteamMatchmaking"), S.stats = iface("SteamUserStats"), S.apps = iface("SteamApps");
    S.storage = iface("SteamRemoteStorage"), S.netmsg = iface("SteamNetworkingMessages_SteamAPI"), S.netutils = iface("SteamNetworkingUtils_SteamAPI");
    LOAD(User_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    LOAD(Fr_GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    LOAD(Fr_GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    LOAD(Fr_GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    LOAD(Fr_GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    LOAD(Fr_GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    LOAD(Fr_ActivateGameOverlay, "SteamAPI_ISteamFriends_ActivateGameOverlay");
    LOAD(Fr_ActivateGameOverlayInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    LOAD(Fr_ActivateGameOverlayToStore, "SteamAPI_ISteamFriends_ActivateGameOverlayToStore");
    LOAD(Fr_SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    LOAD(Ut_GetAppID, "SteamAPI_ISteamUtils_GetAppID");
    LOAD(Ut_IsOverlayEnabled, "SteamAPI_ISteamUtils_IsOverlayEnabled");
    LOAD(Ap_BIsDlcInstalled, "SteamAPI_ISteamApps_BIsDlcInstalled");
    LOAD(Ap_GetCurrentGameLanguage, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    LOAD(Mm_CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    LOAD(Mm_JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    LOAD(Mm_LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    LOAD(Mm_RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    LOAD(Mm_AddStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    LOAD(Mm_AddResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    LOAD(Mm_AddDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    LOAD(Mm_GetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    LOAD(Mm_SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    LOAD(Mm_GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    LOAD(Mm_GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    LOAD(Mm_GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    LOAD(Mm_GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    LOAD(Mm_GetLobbyMemberLimit, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
    LOAD(Mm_SendLobbyChatMsg, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
    LOAD(Mm_GetLobbyChatEntry, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
    LOAD(Mm_SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    LOAD(Mm_InviteUserToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    LOAD(Mm_AddNumericalFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter");
    LOAD(Mm_AddSlotsFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    LOAD(Mm_SetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    LOAD(Mm_SetLobbyMemberLimit, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit");
    LOAD(Fr_GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    LOAD(St_SetAchievement, "SteamAPI_ISteamUserStats_SetAchievement");
    LOAD(St_ClearAchievement, "SteamAPI_ISteamUserStats_ClearAchievement");
    LOAD(St_GetAchievement, "SteamAPI_ISteamUserStats_GetAchievement");
    LOAD(St_IndicateAchievementProgress, "SteamAPI_ISteamUserStats_IndicateAchievementProgress");
    LOAD(St_SetStatInt32, "SteamAPI_ISteamUserStats_SetStatInt32");
    LOAD(St_SetStatFloat, "SteamAPI_ISteamUserStats_SetStatFloat");
    LOAD(St_GetStatInt32, "SteamAPI_ISteamUserStats_GetStatInt32");
    LOAD(St_GetStatFloat, "SteamAPI_ISteamUserStats_GetStatFloat");
    LOAD(St_StoreStats, "SteamAPI_ISteamUserStats_StoreStats");
    LOAD(St_RequestCurrentStats, "SteamAPI_ISteamUserStats_RequestCurrentStats"); // (gone from SDK 1.61: the stats come by themselves)
    LOAD(St_ResetAllStats, "SteamAPI_ISteamUserStats_ResetAllStats");
    LOAD(St_FindOrCreateLeaderboard, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
    LOAD(St_FindLeaderboard, "SteamAPI_ISteamUserStats_FindLeaderboard");
    LOAD(St_UploadLeaderboardScore, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    LOAD(St_DownloadLeaderboardEntries, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries");
    LOAD(St_GetDownloadedLeaderboardEntry, "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry");
    LOAD(St_GetLeaderboardName, "SteamAPI_ISteamUserStats_GetLeaderboardName");
    LOAD(Rs_FileWrite, "SteamAPI_ISteamRemoteStorage_FileWrite");
    LOAD(Rs_FileRead, "SteamAPI_ISteamRemoteStorage_FileRead");
    LOAD(Rs_FileExists, "SteamAPI_ISteamRemoteStorage_FileExists");
    LOAD(Rs_FileDelete, "SteamAPI_ISteamRemoteStorage_FileDelete");
    LOAD(Rs_GetFileSize, "SteamAPI_ISteamRemoteStorage_GetFileSize");
    LOAD(Rs_GetQuota, "SteamAPI_ISteamRemoteStorage_GetQuota");
    LOAD(Rs_IsCloudEnabledForAccount, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
    LOAD(Rs_IsCloudEnabledForApp, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
    LOAD(Nm_SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    LOAD(Nm_ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    LOAD(Nm_AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    LOAD(Nu_InitRelayNetworkAccess, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    LOAD(Id_SetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    LOAD(Id_GetSteamID64, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    LOAD(Msg_Release, "SteamAPI_SteamNetworkingMessage_t_Release");
    if (S.stats && S.St_RequestCurrentStats) S.St_RequestCurrentStats(S.stats);
    if (S.netutils && S.Nu_InitRelayNetworkAccess) S.Nu_InitRelayNetworkAccess(S.netutils); // (the relay ready before the first message)
    S.ok = S.user && S.friends && S.utils;
    snprintf(S.status, sizeof S.status, S.ok ? "ok" : "steam_api64.dll too old (interfaces missing)");
    return S.ok;
}

void steam_shutdown(void) {
    if (!S.dll) return;
    if (S.lobby && S.mm && S.Mm_LeaveLobby) S.Mm_LeaveLobby(S.mm, S.lobby);
    if (S.Shutdown) S.Shutdown();
#ifdef _WIN32
    FreeLibrary((HMODULE)S.dll);
#endif
    memset(&S, 0, sizeof S);
}

bool steam_available(void) { return S.ok; }
const char* steam_status(void) { return S.status[0] ? S.status : "not started"; }

static u64 board_handle(const char* name) {
    for (int i = 0; i < S.nboards; i++)
        if (!strcmp(S.boards[i].name, name)) return S.boards[i].handle;
    return 0;
}

static void on_call_result(u64 call, int id, const uint8_t* p) {
    for (int i = 0; i < 32; i++) {
        if (S.calls[i].call != call) continue;
        const char* name = S.calls[i].name;
        S.calls[i].call = 0;
        switch (id) {
        case CB_LOBBY_CREATED: { // { EResult; u64 lobby }
            int32_t res;
            u64 lobby;
            memcpy(&res, p, 4), memcpy(&lobby, p + 8, 8);
            if (res != 1) lobby = 0;
            if (lobby) S.lobby = lobby;
            push(STEAM_EV_LOBBY_CREATED, lobby, 0, res, NULL);
            break;
        }
        case CB_LOBBY_MATCH_LIST: { // { u32 count }
            uint32_t n;
            memcpy(&n, p, 4);
            S.nfound = 0;
            for (uint32_t k = 0; k < n && k < 64 && S.Mm_GetLobbyByIndex; k++) S.found[S.nfound++] = S.Mm_GetLobbyByIndex(S.mm, (int)k);
            push(STEAM_EV_LOBBY_LIST, 0, 0, S.nfound, NULL);
            break;
        }
        case CB_LOBBY_ENTER: { // { u64 lobby; u32 perms; bool locked; u32 response }
            u64 lobby;
            uint32_t resp;
            memcpy(&lobby, p, 8), memcpy(&resp, p + 16, 4);
            if (resp == 1) S.lobby = lobby;
            push(STEAM_EV_LOBBY_ENTERED, lobby, 0, resp == 1, NULL);
            break;
        }
        case CB_LB_FIND: { // { u64 handle; u8 found }
            u64 h;
            memcpy(&h, p, 8);
            if (!p[8]) h = 0;
            if (h && S.nboards < 16 && !board_handle(name)) {
                snprintf(S.boards[S.nboards].name, sizeof S.boards[S.nboards].name, "%s", name);
                S.boards[S.nboards++].handle = h;
            }
            push(STEAM_EV_LEADERBOARD_FOUND, h, 0, h != 0, name);
            // what waited for the board
            if (h && S.pending_upload[0] && !strcmp(S.pending_upload, name)) {
                S.pending_upload[0] = 0;
                steam_leaderboard_upload(name, S.pending_score, S.pending_keep_best);
            }
            if (h && S.pending_download) {
                S.pending_download = false;
                steam_leaderboard_download(name, S.pending_kind, S.pending_start, S.pending_end);
            }
            break;
        }
        case CB_LB_SCORES: { // { u64 board; u64 entries; int count }
            u64 board, entries;
            int32_t count;
            memcpy(&board, p, 8), memcpy(&entries, p + 8, 8), memcpy(&count, p + 16, 4);
            S.nentries = 0;
            for (int k = 0; k < count && k < 100 && S.St_GetDownloadedLeaderboardEntry; k++)
                if (S.St_GetDownloadedLeaderboardEntry(S.stats, entries, k, &S.entries[S.nentries], NULL, 0)) S.nentries++;
            push(STEAM_EV_LEADERBOARD_SCORES, board, 0, S.nentries, name);
            break;
        }
        case CB_LB_UPLOADED: { // { u8 ok; u64 board; i32 score; u8 changed; int new rank; int old rank }
            int32_t rank;
            memcpy(&rank, p + 20, 4);
            push(STEAM_EV_LEADERBOARD_UPLOADED, 0, (u64)rank, p[0] != 0, name);
            break;
        }
        default: break;
        }
        return;
    }
}

void steam_update(void) {
    if (!S.ok || !S.MD_RunFrame) return;
    S.MD_RunFrame(S.pipe);
    CallbackMsg m;
    for (int n = 0; n < 256 && S.MD_Next(S.pipe, &m); n++) {
        const uint8_t* p = m.param;
        switch (m.id) {
        case CB_CALL_COMPLETED: { // { u64 call; int id; u32 size }: the result of an asynchronous call
            u64 call;
            int32_t id;
            uint32_t size;
            memcpy(&call, p, 8), memcpy(&id, p + 8, 4), memcpy(&size, p + 12, 4);
            uint8_t res[512];
            bool failed = false;
            if (size <= sizeof res && S.MD_CallResult && S.MD_CallResult(S.pipe, call, res, (int)size, id, &failed) && !failed)
                on_call_result(call, id, res);
            break;
        }
        case CB_OVERLAY: push(STEAM_EV_OVERLAY, 0, 0, p[0] != 0, NULL); break;
        case CB_LOBBY_JOIN_REQUESTED: { // { u64 lobby; u64 friend }
            u64 lobby, from;
            memcpy(&lobby, p, 8), memcpy(&from, p + 8, 8);
            push(STEAM_EV_JOIN_REQUESTED, lobby, from, 0, NULL);
            break;
        }
        case CB_LOBBY_ENTER: on_call_result(0, CB_LOBBY_ENTER, p); break; // (also a callback: a lobby joined from the overlay)
        case CB_LOBBY_DATA: {
            u64 lobby;
            memcpy(&lobby, p, 8);
            push(STEAM_EV_LOBBY_DATA, lobby, 0, 0, NULL);
            break;
        }
        case CB_LOBBY_CHAT_UPDATE: { // { u64 lobby; u64 changed; u64 making; u32 state }: 1 entered, 2 left, 4 disconnected, 8 kicked
            u64 lobby, who;
            uint32_t st;
            memcpy(&lobby, p, 8), memcpy(&who, p + 8, 8), memcpy(&st, p + 24, 4);
            push(STEAM_EV_LOBBY_MEMBER, lobby, who, (st & 1u) ? 1 : 0, NULL);
            break;
        }
        case CB_LOBBY_CHAT_MSG: { // { u64 lobby; u64 user; u8 type; u32 chat id }
            u64 lobby, user;
            uint32_t id;
            memcpy(&lobby, p, 8), memcpy(&user, p + 8, 8), memcpy(&id, p + 20, 4);
            char text[128] = { 0 };
            int type = 0;
            if (S.Mm_GetLobbyChatEntry) S.Mm_GetLobbyChatEntry(S.mm, lobby, (int)id, &user, text, (int)sizeof text - 1, &type);
            push(STEAM_EV_LOBBY_CHAT, lobby, user, 0, text);
            break;
        }
        case CB_STATS_STORED: { // { u64 game; EResult }
            int32_t res;
            memcpy(&res, p + 8, 4);
            push(STEAM_EV_STATS_STORED, 0, 0, res == 1, NULL);
            break;
        }
        case CB_ACHIEVEMENT_STORED: push(STEAM_EV_ACHIEVEMENT, 0, 0, 0, (const char*)p + 9); break; // { u64 game; bool group; char name[128]; ... }
        case CB_NETMSG_REQUEST: { // { SteamNetworkingIdentity }: someone wants to talk: accepted (the game decides who may join)
            const NetIdentity* id = (const NetIdentity*)p;
            if (S.Nm_AcceptSessionWithUser) S.Nm_AcceptSessionWithUser(S.netmsg, id);
            push(STEAM_EV_P2P_REQUEST, S.Id_GetSteamID64 ? S.Id_GetSteamID64(id) : 0, 0, 0, NULL);
            break;
        }
        default: break;
        }
        S.MD_Free(S.pipe);
    }
}

bool steam_poll(SteamEvent* ev) {
    static int at;
    if (at >= S.nev) {
        at = S.nev = 0;
        return false;
    }
    *ev = S.ev[at++];
    return true;
}

u64 steam_user_id(void) { return S.ok && S.User_GetSteamID ? S.User_GetSteamID(S.user) : 0; }
const char* steam_user_name(void) { return S.ok && S.Fr_GetPersonaName ? S.Fr_GetPersonaName(S.friends) : ""; }
uint32_t steam_app_id(void) { return S.ok && S.Ut_GetAppID ? S.Ut_GetAppID(S.utils) : 0; }
const char* steam_language(void) { return S.ok && S.apps && S.Ap_GetCurrentGameLanguage ? S.Ap_GetCurrentGameLanguage(S.apps) : ""; }
bool steam_dlc_installed(uint32_t app) { return S.ok && S.apps && S.Ap_BIsDlcInstalled && S.Ap_BIsDlcInstalled(S.apps, app); }
bool steam_overlay_enabled(void) { return S.ok && S.Ut_IsOverlayEnabled && S.Ut_IsOverlayEnabled(S.utils); }
void steam_overlay(const char* dialog) { if (S.ok && S.Fr_ActivateGameOverlay) S.Fr_ActivateGameOverlay(S.friends, dialog && dialog[0] ? dialog : "friends"); }
void steam_overlay_store(uint32_t app) { if (S.ok && S.Fr_ActivateGameOverlayToStore) S.Fr_ActivateGameOverlayToStore(S.friends, app, 0); }
void steam_overlay_invite(void) { if (S.ok && S.lobby && S.Fr_ActivateGameOverlayInviteDialog) S.Fr_ActivateGameOverlayInviteDialog(S.friends, S.lobby); }
bool steam_rich_presence(const char* key, const char* value) { return S.ok && S.Fr_SetRichPresence && S.Fr_SetRichPresence(S.friends, key, value ? value : ""); }

int steam_friend_count(void) { return S.ok && S.Fr_GetFriendCount ? S.Fr_GetFriendCount(S.friends, 4) : 0; } // (k_EFriendFlagImmediate)
u64 steam_friend(int i) { return S.ok && S.Fr_GetFriendByIndex ? S.Fr_GetFriendByIndex(S.friends, i, 4) : 0; }
const char* steam_friend_name(u64 id) { return S.ok && S.Fr_GetFriendPersonaName ? S.Fr_GetFriendPersonaName(S.friends, id) : ""; }
int steam_friend_state(u64 id) { return S.ok && S.Fr_GetFriendPersonaState ? S.Fr_GetFriendPersonaState(S.friends, id) : 0; }
bool steam_invite_friend(u64 id) { return S.ok && S.lobby && S.Mm_InviteUserToLobby && S.Mm_InviteUserToLobby(S.mm, S.lobby, id); }

bool steam_lobby_create(int type, int max_members) {
    if (!S.ok || !S.mm || !S.Mm_CreateLobby) return false;
    u64 call = S.Mm_CreateLobby(S.mm, type, max_members);
    track(call, CB_LOBBY_CREATED, NULL);
    return call != 0;
}
bool steam_lobby_join(u64 lobby) {
    if (!S.ok || !S.mm || !S.Mm_JoinLobby || !lobby) return false;
    u64 call = S.Mm_JoinLobby(S.mm, lobby);
    track(call, CB_LOBBY_ENTER, NULL);
    return call != 0;
}
void steam_lobby_leave(void) {
    if (S.ok && S.lobby && S.Mm_LeaveLobby) S.Mm_LeaveLobby(S.mm, S.lobby);
    S.lobby = 0;
}
u64 steam_lobby_current(void) { return S.lobby; }
bool steam_lobby_find(const char* key, const char* value, int max_results) {
    if (!S.ok || !S.mm || !S.Mm_RequestLobbyList) return false;
    if (key && key[0] && S.Mm_AddStringFilter) S.Mm_AddStringFilter(S.mm, key, value ? value : "", 0); // (k_ELobbyComparisonEqual)
    if (S.Mm_AddDistanceFilter) S.Mm_AddDistanceFilter(S.mm, 3); // (k_ELobbyDistanceFilterWorldwide)
    if (max_results > 0 && S.Mm_AddResultCountFilter) S.Mm_AddResultCountFilter(S.mm, max_results);
    u64 call = S.Mm_RequestLobbyList(S.mm);
    track(call, CB_LOBBY_MATCH_LIST, NULL);
    return call != 0;
}
// The advanced search (online.c, Find Sessions Advanced): the filters Steam applies itself (online.c applies them all again
// on the results, the same on the LAN). cmp: Steam's ELobbyComparison (-2 <=, -1 <, 0 =, 1 >, 2 >=, 3 !=)
bool steam_lobby_find_ex(const SteamLobbyFilter* f, int nf, int min_slots, int max_results) {
    if (!S.ok || !S.mm || !S.Mm_RequestLobbyList) return false;
    for (int i = 0; i < nf; i++) {
        if (f[i].numeric && S.Mm_AddNumericalFilter) S.Mm_AddNumericalFilter(S.mm, f[i].key, f[i].number, f[i].cmp);
        else if (!f[i].numeric && S.Mm_AddStringFilter) S.Mm_AddStringFilter(S.mm, f[i].key, f[i].value, f[i].cmp);
    }
    if (min_slots > 0 && S.Mm_AddSlotsFilter) S.Mm_AddSlotsFilter(S.mm, min_slots);
    if (S.Mm_AddDistanceFilter) S.Mm_AddDistanceFilter(S.mm, 3); // (k_ELobbyDistanceFilterWorldwide)
    if (max_results > 0 && S.Mm_AddResultCountFilter) S.Mm_AddResultCountFilter(S.mm, max_results);
    u64 call = S.Mm_RequestLobbyList(S.mm);
    track(call, CB_LOBBY_MATCH_LIST, NULL);
    return call != 0;
}
bool steam_lobby_set_type(int type) { return S.ok && S.lobby && S.Mm_SetLobbyType && S.Mm_SetLobbyType(S.mm, S.lobby, type); }
bool steam_lobby_set_limit(int max_members) { return S.ok && S.lobby && S.Mm_SetLobbyMemberLimit && S.Mm_SetLobbyMemberLimit(S.mm, S.lobby, max_members); }
// The lobby a friend is in while playing this game (0: none): FriendGameInfo_t { CGameID; u32 ip; u16 port, query port;
// CSteamID lobby } (24 bytes, the lobby at 16)
u64 steam_friend_lobby(u64 id) {
    uint8_t info[32];
    memset(info, 0, sizeof info);
    if (!S.ok || !S.Fr_GetFriendGamePlayed || !S.Fr_GetFriendGamePlayed(S.friends, id, info)) return 0;
    u64 game, lobby;
    memcpy(&game, info, 8), memcpy(&lobby, info + 16, 8);
    return ((uint32_t)game & 0xFFFFFFu) == steam_app_id() ? lobby : 0; // (the low 24 bits of a CGameID: the App ID)
}

u64 steam_lobby_found(int i) { return i >= 0 && i < S.nfound ? S.found[i] : 0; }
bool steam_lobby_set(const char* key, const char* value) { return S.ok && S.lobby && S.Mm_SetLobbyData && S.Mm_SetLobbyData(S.mm, S.lobby, key, value ? value : ""); }
const char* steam_lobby_get(u64 lobby, const char* key) { return S.ok && S.Mm_GetLobbyData ? S.Mm_GetLobbyData(S.mm, lobby ? lobby : S.lobby, key) : ""; }
int steam_lobby_members(u64 lobby) { return S.ok && S.Mm_GetNumLobbyMembers ? S.Mm_GetNumLobbyMembers(S.mm, lobby ? lobby : S.lobby) : 0; }
u64 steam_lobby_member(u64 lobby, int i) { return S.ok && S.Mm_GetLobbyMemberByIndex ? S.Mm_GetLobbyMemberByIndex(S.mm, lobby ? lobby : S.lobby, i) : 0; }
u64 steam_lobby_owner(u64 lobby) { return S.ok && S.Mm_GetLobbyOwner ? S.Mm_GetLobbyOwner(S.mm, lobby ? lobby : S.lobby) : 0; }
int steam_lobby_max(u64 lobby) { return S.ok && S.Mm_GetLobbyMemberLimit ? S.Mm_GetLobbyMemberLimit(S.mm, lobby ? lobby : S.lobby) : 0; }
bool steam_lobby_chat(const char* text) {
    return S.ok && S.lobby && S.Mm_SendLobbyChatMsg && text && S.Mm_SendLobbyChatMsg(S.mm, S.lobby, text, (int)strlen(text) + 1);
}
bool steam_lobby_set_joinable(bool on) { return S.ok && S.lobby && S.Mm_SetLobbyJoinable && S.Mm_SetLobbyJoinable(S.mm, S.lobby, on); }

// --- P2P (ISteamNetworkingMessages, channel 0, unreliable: net.c does the rest) ---------------------------------------
static bool sn_send(void* ctx, const NetAddr* to, const void* data, int len) {
    (void)ctx;
    if (!S.ok || to->kind != NET_ADDR_STEAM || !S.Nm_SendMessageToUser || !S.Id_SetSteamID64) return false;
    NetIdentity id;
    memset(&id, 0, sizeof id);
    S.Id_SetSteamID64(&id, to->id);
    // (unreliable | no Nagle | the session restarted by itself when broken)
    return S.Nm_SendMessageToUser(S.netmsg, &id, data, (uint32_t)len, 1 | 32, 0) == 1;
}
static int sn_recv(void* ctx, NetAddr* from, void* buf, int cap) {
    (void)ctx;
    if (!S.ok || !S.Nm_ReceiveMessagesOnChannel) return 0;
    if (S.inbox_at >= S.ninbox) {
        S.ninbox = S.Nm_ReceiveMessagesOnChannel(S.netmsg, 0, S.inbox, 64);
        S.inbox_at = 0;
        if (S.ninbox <= 0) {
            S.ninbox = 0;
            return 0;
        }
    }
    NetMessage* m = S.inbox[S.inbox_at++];
    int n = m->size < cap ? m->size : cap;
    memcpy(buf, m->data, (size_t)n);
    memset(from, 0, sizeof *from);
    from->kind = NET_ADDR_STEAM;
    from->id = S.Id_GetSteamID64 ? S.Id_GetSteamID64(&m->peer) : 0;
    if (S.Msg_Release) S.Msg_Release(m);
    return n;
}
static void sn_close(void* ctx) { (void)ctx; }
bool steam_net_open(NetTransport* t) {
    if (!S.ok || !S.netmsg || !S.Nm_SendMessageToUser || !S.Nm_ReceiveMessagesOnChannel) return false;
    *t = (NetTransport){ sn_send, sn_recv, sn_close, NULL };
    return true;
}

// --- Steam Cloud ----------------------------------------------------------------------------------------------------------
bool steam_cloud_enabled(void) {
    return S.ok && S.storage && S.Rs_IsCloudEnabledForAccount && S.Rs_IsCloudEnabledForAccount(S.storage) &&
           (!S.Rs_IsCloudEnabledForApp || S.Rs_IsCloudEnabledForApp(S.storage));
}
bool steam_cloud_write(const char* file, const void* data, int len) { return S.ok && S.storage && S.Rs_FileWrite && S.Rs_FileWrite(S.storage, file, data, len); }
int steam_cloud_read(const char* file, void* data, int cap) { return S.ok && S.storage && S.Rs_FileRead ? S.Rs_FileRead(S.storage, file, data, cap) : 0; }
int steam_cloud_size(const char* file) { return S.ok && S.storage && S.Rs_GetFileSize ? S.Rs_GetFileSize(S.storage, file) : 0; }
bool steam_cloud_exists(const char* file) { return S.ok && S.storage && S.Rs_FileExists && S.Rs_FileExists(S.storage, file); }
bool steam_cloud_delete(const char* file) { return S.ok && S.storage && S.Rs_FileDelete && S.Rs_FileDelete(S.storage, file); }
bool steam_cloud_quota(u64* total, u64* available) { return S.ok && S.storage && S.Rs_GetQuota && S.Rs_GetQuota(S.storage, total, available); }

// --- achievements, stats ------------------------------------------------------------------------------------------------
bool steam_achievement_unlock(const char* n) { return S.ok && S.stats && S.St_SetAchievement && S.St_SetAchievement(S.stats, n) && (!S.St_StoreStats || S.St_StoreStats(S.stats)); }
bool steam_achievement_clear(const char* n) { return S.ok && S.stats && S.St_ClearAchievement && S.St_ClearAchievement(S.stats, n) && (!S.St_StoreStats || S.St_StoreStats(S.stats)); }
bool steam_achievement_get(const char* n, bool* a) { return S.ok && S.stats && S.St_GetAchievement && S.St_GetAchievement(S.stats, n, a); }
bool steam_achievement_progress(const char* n, uint32_t c, uint32_t m) { return S.ok && S.stats && S.St_IndicateAchievementProgress && S.St_IndicateAchievementProgress(S.stats, n, c, m); }
bool steam_stat_set_int(const char* n, int32_t v) { return S.ok && S.stats && S.St_SetStatInt32 && S.St_SetStatInt32(S.stats, n, v); }
bool steam_stat_set_float(const char* n, float v) { return S.ok && S.stats && S.St_SetStatFloat && S.St_SetStatFloat(S.stats, n, v); }
bool steam_stat_get_int(const char* n, int32_t* v) { return S.ok && S.stats && S.St_GetStatInt32 && S.St_GetStatInt32(S.stats, n, v); }
bool steam_stat_get_float(const char* n, float* v) { return S.ok && S.stats && S.St_GetStatFloat && S.St_GetStatFloat(S.stats, n, v); }
bool steam_stats_store(void) { return S.ok && S.stats && S.St_StoreStats && S.St_StoreStats(S.stats); }
bool steam_stats_reset(bool ach) { return S.ok && S.stats && S.St_ResetAllStats && S.St_ResetAllStats(S.stats, ach) && steam_stats_store(); }

// --- leaderboards ---------------------------------------------------------------------------------------------------------
bool steam_leaderboard_find(const char* name, bool create) {
    if (!S.ok || !S.stats || !name || !name[0]) return false;
    u64 call = create && S.St_FindOrCreateLeaderboard ? S.St_FindOrCreateLeaderboard(S.stats, name, 2, 1) // (descending, numeric)
                                                       : S.St_FindLeaderboard ? S.St_FindLeaderboard(S.stats, name) : 0;
    track(call, CB_LB_FIND, name);
    return call != 0;
}
bool steam_leaderboard_upload(const char* name, int32_t score, bool keep_best) {
    if (!S.ok || !S.stats || !S.St_UploadLeaderboardScore) return false;
    u64 h = board_handle(name);
    if (!h) { // found first, uploaded then (on_call_result)
        snprintf(S.pending_upload, sizeof S.pending_upload, "%s", name);
        S.pending_score = score, S.pending_keep_best = keep_best;
        return steam_leaderboard_find(name, true);
    }
    u64 call = S.St_UploadLeaderboardScore(S.stats, h, keep_best ? 1 : 2, score, NULL, 0); // (KeepBest / ForceUpdate)
    track(call, CB_LB_UPLOADED, name);
    return call != 0;
}
bool steam_leaderboard_download(const char* name, int kind, int start, int end) {
    if (!S.ok || !S.stats || !S.St_DownloadLeaderboardEntries) return false;
    u64 h = board_handle(name);
    if (!h) {
        S.pending_download = true, S.pending_kind = kind, S.pending_start = start, S.pending_end = end;
        return steam_leaderboard_find(name, false);
    }
    u64 call = S.St_DownloadLeaderboardEntries(S.stats, h, kind, start, end);
    track(call, CB_LB_SCORES, name);
    return call != 0;
}
bool steam_leaderboard_entry(int i, u64* user, int32_t* rank, int32_t* score) {
    if (i < 0 || i >= S.nentries) return false;
    if (user) *user = S.entries[i].user;
    if (rank) *rank = S.entries[i].rank;
    if (score) *score = S.entries[i].score;
    return true;
}
