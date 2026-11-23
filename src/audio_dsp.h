// The mixer's signal processing (audio_dsp.c): the submixes' effects and the master's limiter. Blocks of float, one
// array per channel (2 to 8: the device's speakers, their roles below), processed in place; every state allocated
// before the mixer needs it, nothing in its loop.
#pragma once
#include <stdint.h>

#include "audio_internal.h"

#define AU_BLOCK 256 // frames per mixer block
#define AU_MAX_CH 8  // the mixer's channels at most (7.1)
// The speakers' roles (WASAPI's channel mask order: FL FR FC LFE BL BR SL SR)
enum { AU_SPK_FL = 0, AU_SPK_FR, AU_SPK_FC, AU_SPK_LFE, AU_SPK_BL, AU_SPK_BR, AU_SPK_SL, AU_SPK_SR, AU_SPK_OTHER };

typedef struct AuBiquad { float b0, b1, b2, a1, a2, z1[AU_MAX_CH], z2[AU_MAX_CH]; } AuBiquad;
void au_biquad_lowpass(AuBiquad* f, float hz, float q, float rate);
void au_biquad_highpass(AuBiquad* f, float hz, float q, float rate);
void au_biquad_bandpass(AuBiquad* f, float hz, float q, float rate);
void au_biquad_peak(AuBiquad* f, float hz, float q, float db, float rate);
void au_biquad_lowshelf(AuBiquad* f, float hz, float db, float rate);
void au_biquad_highshelf(AuBiquad* f, float hz, float db, float rate);
void au_biquad_run(AuBiquad* f, float* const* ch, int nch, uint32_t n);

// An effect's state (one per effect slot of a bus); the reverb's and the delay's lines from a pool made at init
typedef struct AuFxState AuFxState;
AuFxState* au_fx_create(float rate);
void au_fx_destroy(AuFxState* s);
// The effect on nch channels of the given roles (its parameters may change every block); kind changed: the state
// cleared. The reverb's output: decorrelated per speaker (front, back, sides from different lines of its network)
void au_fx_process(AuFxState* s, const AuEffect* e, float* const* ch, const uint8_t* roles, int nch, uint32_t n);

// The master's limiter: look-ahead peak limiting to the ceiling (no overs, no clicks); returns the gain reduction (dB)
typedef struct AuLimiter AuLimiter;
AuLimiter* au_limiter_create(float rate, float ceiling_db);
float au_limiter_process(AuLimiter* L, float* const* ch, int nch, uint32_t n); // (linked: every channel the same gain)
void au_limiter_destroy(AuLimiter* L);
