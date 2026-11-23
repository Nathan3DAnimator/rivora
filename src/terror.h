// The horror kit (Q13, notas-dev/terror.md), from the analysis of a horror short (plano 0.43, section 2): the picture
// kept dark, one motivated light, silhouettes, eyes that glow in the black, image shocks, a hallucination filter, a
// shallow lens, short shots and fades to black.
//   - the Tension Director: one value 0..1 a Blueprint sets (Set / Add / Get Tension), followed smoothly, that drives by
//     editable curves (a Post Process Volume's Horror group, else the defaults) the grain, the vignette, the saturation,
//     the letterbox, the field of view, a heartbeat pulse, the lights' flicker and a Sound Class's volume and low-pass
//     (and a MetaSound input on every playing Audio Component);
//   - the filter stack: Hallucination (coarse colored noise + red mono + posterized, the blacks crushed), Black & White
//     blown out, White Flash, TV Static, VHS, Chromatic Aberration, Distortion; each faded in and out by an event (a
//     Blueprint node, a Sequencer track), all of them in the composite pass (render.c fill_terror, composite.frag);
//   - the letterbox (animated to an aspect), Dip to Black / White, the scare rigs (dolly zoom, whip pan, Dutch angle) on
//     the player's view (post.c post_fill_view);
//   - the horror exposure (a Post Process Volume's: metering by the lit part, an EV floor, a toe that never lifts the
//     black: exposure.comp, composite.frag);
//   - the lights' side: Light Flicker patterns (candle, faulty bulb, strobe, lightning, pulse; a sound in sync), Rim
//     Lights kept behind a character as seen from the camera, Lighting Channels (lights.c, mesh.frag), the beam's dust.
// Nothing allocated per frame; everything here is the game thread's.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef enum TerrorFilter {
    TF_HALLUCINATION = 0, TF_BW_BLOWN, TF_WHITE_FLASH, TF_TV_STATIC, TF_VHS, TF_CHROMATIC, TF_DISTORTION, TF_COUNT
} TerrorFilter;
const char* terror_filter_name(int f);   // English ("Hallucination", "Black & White Blown Out", ...; TR it)
int terror_filter_by_name(const char* s); // -1: none (case and spaces don't matter; "BW", "Flash", "Static" too)

typedef enum TerrorRig { TR_DOLLY_ZOOM = 0, TR_WHIP_PAN, TR_DUTCH, TR_COUNT } TerrorRig;
const char* terror_rig_name(int r);
int terror_rig_by_name(const char* s);

typedef enum TensionChannel {
    TC_GRAIN = 0, TC_VIGNETTE, TC_SATURATION, TC_LETTERBOX, TC_FOV, TC_PULSE, TC_FLICKER, TC_VOLUME, TC_LOWPASS
} TensionChannel; // (TENSION_CHANNELS: components.h)
typedef struct TensionChannelInfo { const char* name; float def[4], lo, hi; } TensionChannelInfo;
extern const TensionChannelInfo g_tension_channels[]; // [TENSION_CHANNELS]
const char* light_flicker_name(int f);   // "None", "Candle", "Faulty Bulb", "Strobe", "Lightning", "Pulse"

// --- The Tension Director ---
void terror_set_tension(float value, float blend_time); // the target (0..1), reached in blend_time s (< 0: the volume's rise/fall rates)
void terror_add_tension(float delta, float blend_time);
float terror_tension(void);            // now (the smoothed value)
float terror_tension_target(void);
float terror_tension_channel(int c);   // a channel's value at the tension now (the curves in use)

// --- The filter stack ---
// An event: up to intensity in fade_in s, held hold s, gone in fade_out s (a flash: 0, 0.05, 0.1)
void terror_filter_trigger(int filter, float intensity, float fade_in, float hold, float fade_out);
void terror_filter_set(int filter, float intensity, float fade);   // held at intensity (0: faded off) over fade s
void terror_filter_clear(float fade);                              // every filter off
float terror_filter_value(int filter);                             // its intensity now (the events', the Sequencer's)
void terror_seq_filter(int filter, float intensity);               // a Sequencer track's this frame (taken with the rest)
void terror_seq_letterbox(float amount);                           // a Sequencer track's letterbox (0..1: none .. the 2.39:1 bars)
void terror_seq_clear(void);                                       // the Sequencer's let go (a sequence stopped, its preview closed)
// --- The letterbox, the dips, the rigs ---
void terror_set_letterbox(float aspect, float blend_time);         // bars to this aspect (2.39, 2.2...; 0: none)
float terror_letterbox(void);                                      // each bar's share of the height now
void terror_dip(const float color[3], float fade_in, float hold, float fade_out); // Dip to Black / White
void terror_scare_rig(int rig, float intensity, float duration);   // dolly zoom, whip pan, Dutch angle (on the player's view)
void terror_set_time_frozen(bool frozen, double t);               // (tests) the noise's clock held at t

// --- Per frame (the game thread) ---
// Play's frame: the tension and the filters advanced by dt, the tension's sound, the flickering lights' sounds and the
// Rim Lights moved. playing false: everything back to rest (the editor's view shows none of it)
void terror_tick(World* w, float dt, bool playing);
void terror_reset(void);                                           // Play's start and end
// The player's view (post.c post_fill_view): the FOV, the rigs' offsets, the volume's horror exposure and the frame's
// numbers for the composite (rv->terror)
struct RenderView;
void terror_view(World* w, struct RenderView* rv, bool playing);
// lights.c: a light's flicker now (x its radiance; 1: none) and whether it changes every frame
float terror_light_factor(const CLight* l, Entity e);
bool terror_light_animated(const CLight* l);
double terror_time(void);

typedef struct TerrorStats {
    float tension, letterbox, dip;
    uint32_t filters_on, flicker_lights, rim_lights, sounds;
    double cpu_ms;
} TerrorStats;
const TerrorStats* terror_stats(void);
void terror_register_debug(void); // the Debug Viewer's "Horror Kit" list (debug_viewer.c)
