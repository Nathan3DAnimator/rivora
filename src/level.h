// Levels (.rlevel): a header (magic, version, the editor's camera) and the ECS's serialized world; the baked lighting
// in a .rlight next to it. Shared by the editor and the packaged game (game_main.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

typedef struct LevelCamera {
    float pos[3];
    float yaw, pitch;
    float fov_deg;
    float speed; // cm/s when flying
} LevelCamera;

// Reads a level into w (replacing it); cam: the camera it was saved with (may be NULL). False: not a level this
// version opens (w untouched on a bad header).
bool level_load(World* w, const char* path, LevelCamera* cam);
bool level_load_memory(World* w, const uint8_t* buf, size_t size, LevelCamera* cam); // a level file's bytes
// Writes w (all or nothing: a temp file, then replaced); *bytes: the file's size
bool level_save(World* w, const LevelCamera* cam, const char* path, size_t* bytes);
// The level's .rlight (baked lighting): the same path with that extension
void level_rlight_path(const char* level, char* out, size_t cap);
// The camera's axes from its yaw and pitch (degrees; Z up, X forward at yaw 0, like Unreal)
void level_camera_basis(const LevelCamera* c, float fwd[3], float right[3], float up[3]);
// (Q12) the basis rolled by roll degrees around its forward (Unreal's: + turns the view clockwise, its right side down)
void level_roll_basis(float roll_deg, const float fwd[3], float right[3], float up[3]);
// The level's auto-activated camera (a Camera actor with Auto Activate), as the player's view
bool level_player_camera(World* w, LevelCamera* out);
// Where the player starts: the Player Start at eye height (70 cm above it), looking its way; false: none
bool level_player_start(World* w, LevelCamera* out);

// --- Level streaming and the Level Blueprint (level_stream.c): Unreal's Load / Unload Stream Level, Open Level ---
void level_stream_init(const char* content_root, const char* persistent_name); // a new play (clears every request)
const char* level_stream_persistent(void);
bool level_find(const char* name, char* out, size_t cap); // Content/**/<name>.rlevel (or a path)
uint32_t level_stream_request(const char* name, bool load, bool visible); // the Blueprints' hook (its id)
bool level_stream_done(uint32_t id);
// (2026-11-24) a request's progress 0..1 and its end (result: 0 going, 1 loaded, 2 failed); by the level's name too
float level_stream_progress(uint32_t id, int* result);
float level_stream_state(const char* name, int* result);
void level_stream_update(World* w, bool playing); // each frame: a read level's actors in, an unload's out
// (2026-11-16, sublevels.c) a level file's bytes copied in under the id given (0: the next streaming id); out by id; a level
// loaded by someone else named to Load / Unload Stream Level
uint32_t level_stream_copy(World* w, const uint8_t* data, size_t size, const char* name, uint32_t id, bool visible, bool playing);
void level_stream_unload_id(World* w, uint32_t id);
void level_stream_register(const char* name, uint32_t id);
void level_stream_forget(const char* name);
void level_open_request(const char* name); // Open Level: the host switches between frames
const char* level_open_pending(void);      // the level to switch to (NULL: none)
void level_open_clear(void);
void level_bp_path(const char* level_path, char* out, size_t cap); // <level>.rlevelbp
Entity level_bp_spawn(World* w, const char* level_path);            // its Level Script Actor (when it has one)
struct BpAsset* level_bp_open_or_create(const char* level_path);     // the editor's Blueprints > Open Level Blueprint
