// The audio assets' editor (audio_editor.c): Sound Wave (its waveform, preview, settings), Sound Cue (its graph),
// Sound Attenuation, Sound Class, Sound Mix, Sound Concurrency, Submix; the Content Browser's Add > Sounds menu, the
// Sound Waves' thumbnails (their waveform; played under the pointer's play button) and the importer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Editor;
struct ImDrawList;

bool audio_editor_handles(const char* vpath);
void audio_editor_open(const char* vpath);
bool audio_editor_add_menu(const char* folder_vpath);
void audio_editor_draw(struct Editor* ed, double now, float dt);
bool audio_editor_thumb(struct ImDrawList* dl, float x0, float y0, float x1, float y1, const char* vpath, bool hovered);
bool audio_import_cb(const char* src, const char* folder_vpath, bool place, char* msg, size_t cap);
// A picker of an audio asset (Details rows: an Audio Component's sound, a notify's): kind -1 sounds (waves and cues),
// else AUA_*. True when changed
bool audio_pick(const char* label, uint32_t* hash, int kind);
// The Details of an Audio Component and an Audio Volume (the level's actors, a Blueprint's components)
struct CAudio;
struct CAudioVolume;
bool audio_component_details(struct CAudio* a);
bool audio_volume_details(struct CAudioVolume* v);
