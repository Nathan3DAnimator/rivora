// Quests and objectives (notas-dev/missoes.md; módulo 7 do R7): made from scratch, modular, every game's quest log.
//   * the Quest asset (.rquest, JSON; Content Browser > Add > Quest): its title and description in English / Portuguese /
//     Spanish, stages one after another or all at once, the objectives of each stage — Go To (a place or a volume),
//     Interact (an Interactable, interaction.h), Collect N items (the Inventory, inventory.h), Kill N (Health's On Death,
//     health.h, filtered by a Gameplay Tag), Talk (a Dialogue with an NPC, dialogue.h) and Manual (a Blueprint completes
//     it). Each objective may be optional, may need a Gameplay Tag on the player and may mark an actor on the HUD. The
//     rewards when it completes: items, tags, a Custom Event and the next quest
//   * the Quest Log component (CQuestLog, components.h: on the player or a Game State actor): which quests start with the
//     play, the HUD's tracker and markers. The log's contents (active, completed and failed quests, every objective's
//     progress, the tracked quest) live here in fixed pools, kept across Open Level until play stops
//   * the nodes Start Quest, Complete / Fail Quest, Advance Objective, Set Objective Progress, Is Quest Active / Completed,
//     Get Quest State, Get Objective Progress, Track Quest, Get Tracked Quest and the events On Quest Started, On Objective
//     Updated, On Objective Completed, On Quest Completed, On Quest Failed (bp_vm.c through the hooks below)
//   * WBP_QuestTracker (Content/UI, made when missing): the tracked quest's title and objectives with their counts; the
//     objectives' markers drawn over the view (on the actor, or at the screen's edge when out of view) with the distance
//   * saved with Save Progress (a chunk QSTL, save_progress.c) and with Save Game to Slot (a "#Quests" record)
// No allocation while playing: the assets are read when loaded; the logs are fixed arrays; the events are queued and sent
// once the change is done (an event may start or complete another quest).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define QST_NAME 48
#define QST_TITLE 96          // a title's / objective's text bytes in one language (UTF-8)
#define QST_DESC 256          // the description's
#define QST_LANGS 3           // 0 English (the msgid), 1 Portuguese, 2 Spanish (as dialogue.h)
#define QST_MAX_STAGES 8
#define QST_MAX_OBJ 16        // objectives of a quest (all its stages)
#define QST_REWARDS 3
#define QST_MAX_ASSETS 512
#define QST_MAX_ENTRIES 256   // quests a log holds (active, completed, failed)
#define QST_MAX_LOGS 8
#define QST_MAX_MARKERS 16

enum { QOBJ_GO_TO = 0, QOBJ_INTERACT, QOBJ_COLLECT, QOBJ_KILL, QOBJ_TALK, QOBJ_MANUAL, QOBJ_KIND_COUNT };
enum { QST_NOT_STARTED = 0, QST_ACTIVE, QST_COMPLETED, QST_FAILED, QST_STATE_COUNT };
extern const char* const g_qobj_kind_names[QOBJ_KIND_COUNT];  // "Go To", "Interact", "Collect", "Kill", "Talk", "Manual"
extern const char* const g_qst_state_names[QST_STATE_COUNT];  // "Not Started", "Active", "Completed", "Failed"

#define QO_OPTIONAL 1u        // not needed for its stage (it can still be done while the stage is)
#define QO_COND_NOT 2u        // the condition passes when the owner lacks the tag
#define QO_BY_OWNER 4u        // Kill: only the kills by the log's owner count
#define QO_TAKE_ITEMS 8u      // Collect: the items are handed in (taken) when the quest completes

typedef struct QuestObjective {
    uint8_t kind, stage, flags, pad;
    int32_t count;              // how many (Collect, Kill, Interact; the others: 1)
    float radius;               // Go To: cm around the target (0: inside its box / its Collision)
    char id[QST_NAME];          // what the nodes call it ("find_key"; "": its index, "0" the first)
    char target[QST_NAME];      // Go To / Interact / Talk: an actor's name or a Gameplay Tag on it (Talk: also a Dialogue's
                                // name); Collect: the Item Definition; Kill: the Gameplay Tag of who dies (or a name)
    char condition[QST_NAME];   // a Gameplay Tag the owner needs for it to count ("": none)
    char marker[QST_NAME];      // the actor the HUD marks ("": the target when it is an actor; "-": none)
    char text[QST_LANGS][QST_TITLE];
} QuestObjective;

typedef struct QuestStage {
    uint8_t in_order;           // its objectives one after another (else all at once)
    char text[QST_LANGS][QST_TITLE]; // under the title on the tracker ("": none)
} QuestStage;

typedef struct Quest {
    char name[QST_NAME], path[512];
    uint32_t hash;
    uint32_t version;           // bumped by every change
    char title[QST_LANGS][QST_TITLE];
    char desc[QST_LANGS][QST_DESC];
    char category[QST_NAME];    // "Main", "Side" (free text)
    uint8_t parallel;           // every stage at once (else one after another)
    uint8_t repeatable;         // Start Quest again once completed
    uint8_t nstages, nobj;
    float time_limit;           // seconds active before it fails (0: none)
    QuestStage stages[QST_MAX_STAGES];
    QuestObjective obj[QST_MAX_OBJ]; // in the stages' order
    char reward_item[QST_REWARDS][QST_NAME];
    int32_t reward_count[QST_REWARDS];
    char reward_tag[QST_REWARDS][QST_NAME];
    char reward_event[QST_NAME]; // a Custom Event called on the owner (and on Event Target)
    char event_target[QST_NAME]; // an actor's name ("": the owner only)
    char next_quest[QST_NAME];  // started when this one completes
} Quest;

// --- Assets ---
void quest_init(const char* content_root);  // the Content folder (scanned on demand)
Quest* quest_load(const char* vpath);       // cached
Quest* quest_find(uint32_t hash);
Quest* quest_find_name(const char* name);   // "Q_LostAmulet" (case-insensitive)
uint32_t quest_count(void);
Quest* quest_at(uint32_t i);
bool quest_save(Quest* q);
Quest* quest_create(const char* folder_vpath, const char* name, bool example); // example: the lost amulet (every kind)
uint32_t quest_vpath_hash(const char* vpath);
bool quest_is_path(const char* vpath);      // a .rquest
int quest_add_objective(Quest* q, int stage, uint8_t kind); // its index (-1: full); kept in the stages' order
void quest_remove_objective(Quest* q, int o);
int quest_add_stage(Quest* q);
void quest_remove_stage(Quest* q, int s);   // its objectives go too
const char* quest_title(const Quest* q, int lang);                  // never NULL ("": its name)
const char* quest_obj_text(const Quest* q, const QuestObjective* o, int lang); // never NULL ("": made from its kind)
const char* quest_desc(const Quest* q, int lang);
int quest_obj_index(const Quest* q, const char* id); // by its id, or a number; -1 none

// --- Play (bp_vm.c) ---
enum { QST_EV_STARTED = 0, QST_EV_OBJ_UPDATED, QST_EV_OBJ_COMPLETED, QST_EV_COMPLETED, QST_EV_FAILED, QST_EV_COUNT };
typedef struct QuestHooks {
    // to every Blueprint that has the event: the log's owner, the quest's name, the objective's id and index, its
    // progress and count. It may run game code (start another quest)
    void (*event)(World* w, int ev, Entity owner, const char* quest, const char* obj, int index, int progress, int count);
    bool (*call)(World* w, Entity e, const char* event, const char* const* args, uint32_t nargs); // a Custom Event (false: none)
    Entity (*player)(World* w);
} QuestHooks;
void quest_set_hooks(const QuestHooks* h);
void quest_begin_play(World* w);            // before the Save Progress load (a level change keeps the logs)
void quest_after_begin_play(World* w);      // the Quest Logs' starting quests, WBP_QuestTracker
void quest_tick(World* w, float dt);        // Go To, Collect, the time limits, the tracker, the markers
void quest_end_play(World* w);
void quest_session_end(void);               // play stopped (save_progress.c progress_session_end): the logs forgotten
void quest_clear(void);                     // New Game: nothing started
// what happened in the world (bp_vm.c's hooks of Health, Interaction and Dialogue)
void quest_notify_death(World* w, Entity dead, Entity killer);
void quest_notify_interact(World* w, Entity target, Entity instigator);
void quest_notify_talk(World* w, Entity speaker, const char* dialogue);

// What the nodes do. log: the Quest Log's actor (ENTITY_NONE: the player's, else the first one, else the game's own)
bool quest_start(World* w, Entity log, const char* quest);
bool quest_complete(World* w, Entity log, const char* quest); // (its rewards)
bool quest_fail(World* w, Entity log, const char* quest);
bool quest_advance(World* w, Entity log, const char* quest, const char* obj, int amount, bool set); // true: the objective is done
int quest_state(World* w, Entity log, const char* quest, int* stage); // QST_*
bool quest_objective(World* w, Entity log, const char* quest, const char* obj, int* progress, int* count, bool* done, const char** text);
bool quest_track(World* w, Entity log, const char* quest);
const Quest* quest_tracked(World* w, Entity log);
int quest_language(void);                   // the texts' language (dialogue.h's: Set Dialogue Language or the game's)

// The HUD: the tracked quest's objectives with markers (quest_tick fills them; quest_hud.c draws them over the view)
typedef struct QuestMarker {
    float pos[3];               // the world's (over the actor)
    float dist;                 // cm from the player
    uint8_t optional;
} QuestMarker;
const QuestMarker* quest_markers(uint32_t* count);
bool quest_markers_shown(void);             // some to draw (the game opens its ImGui frame for them)
struct ImDrawList;
void quest_draw_markers(struct ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg,
                        float x0, float y0, float x1, float y1);
uint32_t quest_tracker_asset(void);         // WBP_QuestTracker (Content/UI, made when missing): its hash

// --- Saves: a "#Quests" record in a variables' stream ("RESAV001" + records; save_progress.c's VARS and QSTL) ---
uint32_t quest_save_append(uint8_t* buf, uint32_t len, uint32_t cap); // the new length (len: nothing to save / no room)
bool quest_save_read(const uint8_t* buf, uint32_t len);               // false: no record (nothing changed)

// --- Editor (quest_editor.c) ---
struct Editor;
struct CQuestLog;
void quest_editor_open(const char* vpath);
bool quest_editor_add_menu(const char* folder_vpath); // Content Browser > Add > Quest
void quest_editor_draw(struct Editor* ed);
bool quest_ui_log(struct CQuestLog* c);              // the component's Details (the level's, a Blueprint's)

// --- Cost ---
typedef struct QuestStats {
    double tick_ms;             // quest_tick's last frame
    double op_us;               // the last operation (start, advance, a notification)
    uint32_t logs, active, completed, failed; // now
    uint32_t objectives;        // active objectives now
    uint32_t events;            // sent since play started
    uint32_t assets;
} QuestStats;
const QuestStats* quest_stats(void);
void quest_register_debug(void);            // its Debug Viewer list ("Quests")
int quest_selftest(void);                   // the rules on made-up quests (no world), 200 timed: failures
