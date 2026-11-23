// Audio in the world (audio_world.c): Audio Components and Audio Volumes, the listener, occlusion. The editor's PIE
// and the packaged game call begin / end play and update every frame (the listener: their camera).
#pragma once
#include <stdbool.h>

#include "audio.h"
#include "ecs.h"

void audio_world_begin_play(World* w);   // the auto-activated Audio Components start
void audio_world_end_play(World* w);     // the game's sounds stop (the editor's previews go on), the mixes cleared
// Every frame: the listener (cm, unit axes), the volumes' reverb, then audio_update. playing: the game runs (PIE or
// the packaged game); else the editor (the listener only: previews)
void audio_world_update(World* w, float dt, bool playing, const float pos[3], const float fwd[3], const float up[3]);

// An Audio Component's calls (Blueprints: Play, Stop, Fade In / Out, Set Paused, Set Volume / Pitch Multiplier, Set
// Sound, Is Playing); e: the component's entity or its actor (its first Audio Component)
void audio_component_play(World* w, Entity e, float start_time);
void audio_component_stop(World* w, Entity e, float fade_out);
void audio_component_fade_in(World* w, Entity e, float seconds, float volume, float start_time);
void audio_component_set_paused(World* w, Entity e, bool paused);
void audio_component_set_volume(World* w, Entity e, float v);
void audio_component_set_pitch(World* w, Entity e, float p);
void audio_component_set_sound(World* w, Entity e, uint32_t sound);
bool audio_component_is_playing(World* w, Entity e);
AudioHandle audio_component_handle(World* w, Entity e);
bool audio_component_set_param(World* w, Entity e, const char* name, float value); // a MetaSound's input (Set Float Parameter)
// B8: the world's own sounds while playing (MetaSounds: each vehicle's engine by its RPM and throttle, the rain by its
// intensity, the wind by its speed); REALENGINE_NO_AUTO_SOUNDS or an actor's own Audio Component turns one off
typedef struct AudioAutoStats { uint32_t engines, rain, wind; float rpm, load, intensity, wind_speed; } AudioAutoStats;
const AudioAutoStats* audio_auto_stats(void);
