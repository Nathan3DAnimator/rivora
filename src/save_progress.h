// Save and load the progress (notas-dev/salvar-progresso.md): the R7 module 4, made to be reused by any game.
//   * a Save Game like Unreal's USaveGame: a Blueprint Class whose parent is "Save Game" holds the progress's variables
//     (Create Save Game Object; Project Settings > Save Game > Save Game Class: the progress's own, carried from level
//     to level and saved with it: Get Progress Save Game)
//   * the world's state the user marks: the Saveable component (Details > Add) saves the actor's place, its Health
//     (health.h), its Inventory (inventory.h), its variables marked SaveGame (or all of them) and whether it was
//     destroyed (a chest opened, an enemy killed, a door that stays open). The actors spawned while playing with a
//     Saveable are spawned again by their class. The player's pawn is always saved (place, Health, Inventory, variables)
//   * slots with their metadata: a name, the date and time, the play time, the level, a small picture of the screen
//     (taken from the next frame, the widgets hidden). List Save Slots, Get Save Slot Info, Does Save Game Exist
//   * auto save: every N seconds (Project Settings; the player's AutoSave option), on an event (the node Auto Save, the
//     level changing), at a Checkpoint (a volume: saves when the player walks in, the point it respawns at)
//   * Load Progress opens the slot's level (Open Level) and applies its state before the level's BeginPlay (the player
//     and the spawned actors right after it)
//   * the file (Saved/SaveGames/<slot>.sav; Steam Cloud too when on): a versioned header, a 64-bit checksum, chunks
//     (unknown ones skipped); a corrupted file is refused, never trusted; version 1 (the old Save Game to Slot: the
//     variables only) opens migrated
//   * WBP_SaveSlots (save_menu.c): Continue, Load, New Game, Save, Delete with the picture, in the pause menu too
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

#define SAVE_SLOT_NAME 64        // a slot's file name (letters, digits, - _ . and spaces; the rest become '_')
#define SAVE_DISPLAY_NAME 64
#define SAVE_LEVEL_NAME 128
#define SAVE_VERSION 2u          // the file's (1: the old Save Game to Slot, read migrated)
#define SAVE_THUMB_W 192         // the picture's size (16:9, RGB)
#define SAVE_THUMB_H 108
#define SAVE_MAX_SLOTS 64        // listed (the newest first)
enum { SAVE_KIND_PROGRESS = 0, SAVE_KIND_OBJECT }; // a whole progress; one actor's (or Save Game object's) variables
#define SAVE_FLAG_AUTO 1u        // an auto save
#define SAVE_FLAG_CHECKPOINT 2u  // a Checkpoint's

// A slot's metadata (read from its header only: listing is cheap)
typedef struct SaveSlotInfo {
    char slot[SAVE_SLOT_NAME];
    char display[SAVE_DISPLAY_NAME];
    char level[SAVE_LEVEL_NAME];
    int64_t time;                // when it was saved (seconds since 1970, UTC)
    double play_time;            // seconds played
    uint32_t kind, flags, version, actors;
    uint32_t bytes;              // the file's size
    uint16_t thumb_w, thumb_h;   // 0: no picture
} SaveSlotInfo;

// --- The progress (the nodes Save Progress, Load Progress, New Game, Auto Save) ---
// Everything into the slot: the metadata, the picture (from the next frame: the file is written then; a read of the
// slot before waits for it), the Save Game object, the Saveables, the destroyed, the player, the checkpoint. display
// NULL / "": the level's name. False: no slot name, or the world could not be written
bool progress_save(World* w, const char* slot, const char* display, uint32_t flags);
// The slot's level opened (Open Level) and its state applied when it begins (the same level: opened again too). False:
// no such slot, corrupted, made by a newer engine (nothing changes)
bool progress_load(World* w, const char* slot);
bool progress_new_game(World* w, const char* level); // the play time, the checkpoint, the Save Game object cleared; level "" : this one
bool progress_auto_save(World* w);                   // the next auto save slot ("AutoSave_1".."_N", in turn)
// The newest progress slot (Continue); false: none
bool progress_latest(char* slot, size_t cap);
double progress_play_time(void);
void progress_set_autosave_interval(float seconds); // until play stops (<0: Project Settings' again; 0: off)
void progress_format_date(int64_t unix_time, char* out, size_t cap);   // "2026-11-08 21:45" (local time)
void progress_format_duration(double seconds, char* out, size_t cap); // "1:02:03", "12:05"
const char* progress_current_slot(void); // the slot last saved or loaded ("": none)

// --- Slots ---
uint32_t progress_new_id(void); // a unique key for a Saveable added in the editor (CSaveable.id)
bool progress_slot_exists(const char* slot);
bool progress_slot_delete(const char* slot); // (and its Steam Cloud copy)
bool progress_slot_info(const char* slot, SaveSlotInfo* out); // false: missing or corrupted
// The slots, the newest first (all: the actors' variable saves too, not only the progress). How many
uint32_t progress_list(SaveSlotInfo* out, uint32_t cap, bool all);
// A slot's picture (RGB, SAVE_THUMB_W x SAVE_THUMB_H at most); gen changes when the slot is saved again. NULL: none
const uint8_t* progress_thumb(const char* slot, uint32_t* w, uint32_t* h, uint32_t* gen);
// The folder (REALENGINE_SAVE_DIR over "Saved/SaveGames") and a slot's file
const char* progress_dir(void);
void progress_slot_path(const char* slot, char* out, size_t cap);

// --- A Save Game object's or an actor's variables (Save Game to Slot / Load Game from Slot) ---
bool progress_save_object(World* w, Entity e, const char* slot);
bool progress_load_object(World* w, Entity e, const char* slot);
Entity progress_create_save_object(World* w, uint32_t class_hash); // Create Save Game Object (a "Save Game" class)
Entity progress_save_game(World* w);                                // Get Progress Save Game (made from Project Settings' class)
bool progress_set_save_game(World* w, Entity obj);                 // Set Progress Save Game

// --- Checkpoints (CCheckpoint, components.h) ---
Entity progress_checkpoint(World* w);       // the last reached (NONE: none)
bool progress_set_checkpoint(World* w, Entity cp); // as reached (no save)
// The player back at the last checkpoint (none: where the level started it): reload the checkpoint's save (the world as it
// was then), or only move it there and Revive its Health
bool progress_respawn(World* w, Entity player, bool reload);

// --- The engine's hooks ---
struct VkCtx;
void progress_set_vk(struct VkCtx* vk);        // the pictures (NULL: none)
void progress_set_view_rect(float x0, float y0, float x1, float y1); // the game's view in the window, pixels (0s: the whole window)
bool progress_hide_ui(void);                  // this frame is the picture's: the widgets aren't drawn
void progress_begin_play(World* w);           // bp_begin_play, before BeginPlay: the Saveables' keys, the Save Game object back, a load's level actors
void progress_update(World* w, float dt);     // bp_tick: the rest of a load, the play time, auto save, checkpoints, the picture
void progress_end_play(World* w);             // bp_end_play: the Save Game object kept for the next level
void progress_session_end(void);              // the Play stopped / the game closes: everything forgotten
void progress_flush(void);                    // a save waiting for its picture written now (without it)

// --- WBP_SaveSlots (save_menu.c) ---
uint32_t save_menu_asset(void);               // made in Content/UI when missing; its hash
bool save_menu_open(World* w, bool save_mode); // on the screen (save mode: Save / Overwrite; else Continue / Load)
void save_menu_close(World* w);
bool save_menu_is_open(void);
void save_menu_pause_buttons(void);           // WBP_PauseMenu gets its Save Game / Load Game buttons (when missing)
// The widgets bound to "Save.*" (a row's name, info, picture, buttons): read each frame, clicked
bool save_menu_bind(void* widget);            // UiWidget*: true when the name was ours
bool save_menu_event(World* w, Entity live, void* widget, const char* event);
uint32_t save_menu_serial(void); // changes with the slot list (the bound widgets read again)
// A bound Image's picture ("Save.N.Thumb"): its pixels (RGB), a key for its texture ("save:<slot>"), its size and generation
const uint8_t* save_menu_thumb(const char* bind, char* key, size_t cap, uint32_t* w, uint32_t* h, uint32_t* gen);

typedef struct ProgressStats {
    double save_ms, load_ms;     // the last save: build_ms + write_ms; the last load's read, checksum and apply
    double build_ms, write_ms;   // the world into the buffer; its file handed to the writer (the checksum, a copy; Steam Cloud too)
    double disk_ms;              // the writer thread's last file (a temp file replaced, written through): the game doesn't wait for it
    double apply_ms;             // its apply (the level's actors, then the player and the spawned)
    double thumb_ms;             // the last picture's copy and scale (the GPU waited for)
    double update_ms;            // the last progress_update
    uint32_t bytes, actors, destroyed, spawned, saveables, checkpoints;
    uint32_t saves, loads, auto_saves, refused; // since the session started
} ProgressStats;
const ProgressStats* progress_stats(void);
void progress_register_debug(void); // the Debug Viewer's "Save Game" list

// The self-test (--save-test, before the play part in save_test.c): the format (checksum, a corrupted file refused, a
// version 1 file migrated, unknown chunks skipped), slots and their metadata. Failures
int progress_selftest(void);
