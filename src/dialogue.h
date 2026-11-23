// Dialogue (notas-dev/dialogo.md; módulo 6 do R7): conversations as data, reusable by any game.
//   * the Dialogue asset (.rdlg, Content Browser > Add > Dialogue): a graph of nodes — Start, Line (who speaks, the text
//     in English / Portuguese / Spanish, a voice, an animation or Montage, a duration or wait for the player), Choice (the
//     player's Options, its children), Option, Jump (to another node) and End. Every node may carry Conditions (a Gameplay
//     Tag, an item in the Inventory, a Blueprint variable, the health, a node said before) and Actions run when passed
//     (give / take an item, add / remove a tag, a Custom Event with inputs, set a variable, open the shop)
//   * following a node: the first of its links whose node's conditions pass; a Choice shows every Option that passes
//     (a failed condition hides it)
//   * the Dialogue Speaker component on the NPC (components.h CDialogueSpeaker): which Dialogue, the name shown, the
//     conversation camera, whether the game pauses, the widget
//   * the nodes Start Dialogue, Choose Option, Skip Line, End Dialogue, Is In Dialogue, Set Dialogue Language and the events
//     On Dialogue Line / Choice / End (bp_vm.c through the hooks below)
//   * WBP_Dialogue (Content/UI, made when missing): the box with the name, the text appearing letter by letter, the Options
//     as Buttons (keyboard, mouse and gamepad through UMG's navigation)
//   * what was said and chosen is remembered (the Said Before condition, Once options) and saved with Save Game to Slot
// One conversation runs at a time (the player's); the editor's preview runs its own (dialogue_editor.c). No allocation
// while playing: the assets are read when loaded, the run's state is fixed arrays.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define DLG_NAME 48
#define DLG_TEXT 200          // a text's bytes in one language (UTF-8)
#define DLG_PATH 128
#define DLG_MAX_NODES 128
#define DLG_MAX_LINKS 8       // a node's outgoing links (a Choice: its Options)
#define DLG_MAX_CONDS 3
#define DLG_MAX_ACTS 3
#define DLG_MAX_OPTIONS DLG_MAX_LINKS
#define DLG_MAX_ASSETS 128
#define DLG_LANGS 3           // the texts' languages: 0 English (the msgid, i18n.h), 1 Portuguese, 2 Spanish

enum { DLG_START = 0, DLG_LINE, DLG_CHOICE, DLG_OPTION, DLG_JUMP, DLG_END, DLG_KIND_COUNT };
enum { DLC_NONE = 0, DLC_HAS_TAG, DLC_HAS_ITEM, DLC_VARIABLE, DLC_HEALTH, DLC_SAID, DLC_KIND_COUNT };
enum { DLA_NONE = 0, DLA_GIVE_ITEM, DLA_TAKE_ITEM, DLA_ADD_TAG, DLA_REMOVE_TAG, DLA_EVENT, DLA_SET_VAR, DLA_OPEN_SHOP, DLA_KIND_COUNT };
enum { DLG_OP_GE = 0, DLG_OP_GT, DLG_OP_EQ, DLG_OP_NE, DLG_OP_LT, DLG_OP_LE, DLG_OP_COUNT };
enum { DLG_WHO_PLAYER = 0, DLG_WHO_SPEAKER, DLG_WHO_COUNT }; // whom a condition reads / an action changes
extern const char* const g_dlg_kind_names[DLG_KIND_COUNT];
extern const char* const g_dlg_cond_names[DLC_KIND_COUNT];
extern const char* const g_dlg_act_names[DLA_KIND_COUNT];
extern const char* const g_dlg_op_names[DLG_OP_COUNT];
extern const char* const g_dlg_who_names[DLG_WHO_COUNT];

typedef struct DlgCond {
    uint8_t kind, op, who;
    bool invert;               // Not: passes when the test fails
    char key[DLG_NAME];        // the tag, the item, the variable (Health: unused)
    float value;               // the item's count, the compared number (Health: percent 0-100), Said: the node's index
} DlgCond;
typedef struct DlgAct {
    uint8_t kind, who;
    char key[DLG_NAME];        // the item, the tag, the Custom Event, the variable, the shop's Widget Blueprint
    char arg[2][DLG_NAME];     // the event's inputs (text, converted to its inputs' types); Set Variable: the value
    float value;               // the item's count
} DlgAct;
typedef struct DlgNode {
    uint8_t kind, nlinks, nconds, nacts;
    uint8_t once;              // an Option: hidden once chosen (remembered, saved)
    uint8_t wait;              // a Line: waits for the player (else Duration seconds after it is all shown)
    int16_t jump;              // a Jump: the node it goes to (-1: none)
    float x, y;                // in the editor
    char speaker[DLG_NAME];    // a Line: "" the NPC (its Display Name), "Player", or a name
    char text[DLG_LANGS][DLG_TEXT]; // English (the msgid), Portuguese, Spanish ("": English / its i18n translation)
    char voice[DLG_PATH];      // a Sound Wave or Sound Cue ("/Content/...", "": none)
    char anim[DLG_NAME];       // an Anim Montage or Animation Sequence played on who speaks ("": none)
    float duration;            // a Line without wait: seconds it stays once all shown (0: the voice's / 2 s)
    int16_t links[DLG_MAX_LINKS];
    DlgCond conds[DLG_MAX_CONDS];
    DlgAct acts[DLG_MAX_ACTS];
    char comment[64];
} DlgNode;
typedef struct Dialogue {
    char name[DLG_NAME], path[512];
    uint32_t hash;
    uint32_t version;          // bumped by every change (the preview, a running conversation re-reads)
    float chars_per_second;    // the text appearing (0: at once)
    uint8_t pause;             // the game pauses while it runs (the speaker may say otherwise)
    char player_name[DLG_NAME]; // the "Player" lines' name ("": "You", translated)
    uint32_t nnodes;
    DlgNode nodes[DLG_MAX_NODES]; // [0]: the Start
} Dialogue;

// --- Assets ---
void dlg_init(const char* content_root);    // the Content folder (scanned on demand)
Dialogue* dlg_load(const char* vpath);      // cached
Dialogue* dlg_find(uint32_t hash);          // by its vpath's hash (the Content folder scanned once)
Dialogue* dlg_find_name(const char* name);  // "DLG_Merchant" (case-insensitive)
uint32_t dlg_count(void);
Dialogue* dlg_at(uint32_t i);
bool dlg_save(Dialogue* d);
Dialogue* dlg_create(const char* folder_vpath, const char* name, bool example); // example: a merchant's small conversation
uint32_t dlg_vpath_hash(const char* vpath);
bool dlg_is_path(const char* vpath);        // a .rdlg
int dlg_add_node(Dialogue* d, uint8_t kind, float x, float y); // its index (-1: full)
void dlg_remove_node(Dialogue* d, int n);   // its links and jumps to it go (the Start stays)
bool dlg_link(Dialogue* d, int from, int to); // false: full / a loop on itself / already
void dlg_unlink(Dialogue* d, int from, int to);

// --- Languages: the texts' (0 en, 1 pt, 2 es) ---
// The language the conversations show: Set Dialogue Language's, else the game's (game_settings.h Language; the system's
// Spanish when it follows the system)
int dlg_language(void);
void dlg_set_language(int lang);            // -1: the game's
int dlg_language_from_code(const char* code); // "es", "es_ES", "pt_BR", "en"... (-1: "" or unknown)
const char* dlg_node_text(const Dialogue* d, const DlgNode* n, int lang); // its text in lang (never NULL)

// --- The conversation (the game's: one at a time) ---
typedef struct DlgStartInfo {
    Entity speaker, player;    // player ENTITY_NONE: the possessed pawn
    const char* display_name;  // NULL: the speaker's Dialogue Speaker / its name
    int pause;                 // -1: the Dialogue's, 0 no, 1 yes
    bool camera;               // the conversation camera (the Speaker's settings)
    float cam_distance, cam_height, cam_angle;
    uint32_t widget;           // a Widget Blueprint's hash (0: WBP_Dialogue)
} DlgStartInfo;
bool dlg_start(World* w, Dialogue* d, const DlgStartInfo* info); // false: none / no line to show
bool dlg_start_speaker(World* w, Entity speaker, const char* dialogue, Entity player); // its Dialogue Speaker's settings (dialogue: "" theirs)
bool dlg_choose(World* w, int option);      // the shown Options' index (0 the first); false: not choosing / none
void dlg_skip(World* w);                    // the text all at once, or the next node when it is all shown
void dlg_end(World* w);                     // On Dialogue End
bool dlg_active(Entity* speaker);           // a conversation runs (its speaker)
// What the screen shows now (the widget's and the tests')
typedef struct DlgView {
    bool active, choosing;
    const char* speaker;       // the name shown
    const char* text;          // all of the line
    uint32_t shown_bytes;      // how much of it is shown (letter by letter)
    uint32_t noptions;
    const char* options[DLG_MAX_OPTIONS];
    int node;                  // the node shown (-1: none)
} DlgView;
void dlg_view_get(DlgView* v);

// --- Play (bp_vm.c) ---
typedef struct DlgHooks {
    // the events: 0 Line (a: speaker; s0 name, s1 text; i: node), 1 Choice (a: speaker; s1: text; i: option), 2 End (a:
    // speaker; s0: the Dialogue's name)
    void (*event)(World* w, int ev, Entity speaker, const char* s0, const char* s1, int i);
    // a Custom Event on an actor with its inputs as text (converted to the event's types); false: none
    bool (*call)(World* w, Entity e, const char* event, const char* const* args, uint32_t nargs);
    bool (*get_var)(World* w, Entity e, const char* var, float* num, char* text, size_t cap); // a variable as a number or text
    bool (*set_var)(World* w, Entity e, const char* var, const char* value);
    Entity (*player)(World* w);
    bool (*camera)(World* w, float pos[3], float* yaw, float* pitch); // the gameplay camera (before the conversation's)
} DlgHooks;
void dlg_set_hooks(const DlgHooks* h);
enum { DLG_EV_LINE = 0, DLG_EV_CHOICE, DLG_EV_END };
void dlg_begin_play(World* w);              // the memory cleared (a save loads it back)
void dlg_tick(World* w, float dt);          // the text appearing, the durations, the widget (also while paused)
void dlg_end_play(World* w);
bool dlg_view_camera(World* w, float pos[3], float* yaw, float* pitch); // the conversation camera (bp_player_camera)
uint32_t dlg_widget_asset(void);            // WBP_Dialogue (Content/UI, made when missing): its hash

// --- Memory: what was said and chosen (Said Before, Once), saved with Save Game to Slot (online.c) ---
bool dlg_said(uint32_t dialogue, int node);
void dlg_memory_clear(void);
uint32_t dlg_save_append(uint8_t* buf, uint32_t len, uint32_t cap); // a record after the variables' (len: nothing to save)
bool dlg_save_read(const uint8_t* buf, uint32_t len);

// --- The editor's preview (dialogue_editor.c): the same rules on a made-up player ---
typedef struct DlgPreview {
    const Dialogue* d;
    int node;                  // the node shown (-1: ended)
    bool choosing;
    float t, shown;            // seconds on the node; the letters shown
    int nopts;
    int16_t opts[DLG_MAX_OPTIONS];
    int16_t last_line;         // the last Line shown (a Choice without text shows it: the question)
    uint32_t said[DLG_MAX_NODES / 32];
    // the made-up player: its tags, items, health (percent) and variables (name=value)
    char tags[8][DLG_NAME];
    char items[8][DLG_NAME];
    int counts[8];
    float health;
    char vars[6][DLG_NAME], var_values[6][DLG_NAME];
    char log[16][160];         // what happened (the actions, the events), newest last
    int nlog;
    int lang;
} DlgPreview;
void dlg_preview_start(DlgPreview* p, const Dialogue* d);
void dlg_preview_tick(DlgPreview* p, float dt);
void dlg_preview_skip(DlgPreview* p);
void dlg_preview_choose(DlgPreview* p, int option);
int dlg_shown_node(const DlgPreview* p);   // the node whose speaker and text show (a Choice without text: the last Line)

// --- Editor (dialogue_editor.c) ---
struct Editor;
struct CDialogueSpeaker;
void dlg_editor_open(const char* vpath);
bool dlg_editor_add_menu(const char* folder_vpath); // Content Browser > Add > Dialogue
void dlg_editor_draw(struct Editor* ed);
bool dlg_ui_speaker(struct CDialogueSpeaker* s);   // the component's Details (the level's, a Blueprint's)
void dlg_editor_test_preview(void);                 // (the tests: the open editor's preview started)

// --- Cost ---
typedef struct DlgStats {
    double tick_ms;            // dlg_tick's last frame
    double advance_us;         // the last node passed (conditions, actions, events)
    uint32_t lines, choices, conversations; // since play started
    uint32_t assets;
} DlgStats;
const DlgStats* dlg_stats(void);
int dlg_selftest(void);        // the rules on a made-up dialogue (preview): failures (the test's first part)
