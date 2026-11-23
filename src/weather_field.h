// (Q27.6) The regional weather (notas-dev/agua.md "Clima regional"): GTA 6's "sunny where you are while a storm hits
// somewhere nearby". A few storm cells over the world (a center, a radius, a peak rate, a life: they grow, hold and fade)
// drift with the prevailing wind; the rain, the wind and the clouds at any point are their sum, the same on the CPU (the
// camera's rain, the clouds, the ocean, Get Weather At) and on the GPU (the persistent wet map's cells far from the
// camera: water_sim.comp pass 15). No per-cell state: nothing to stream, any place answers at once.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WFIELD_MAX_STORMS 8

typedef struct WeatherSample {
    float rain;    // mm/h
    float wind[2]; // m/s (x, y)
    float clouds;  // 0..1: how overcast
    float gust;    // m/s: the storms' outflow alone (the sea it raises)
} WeatherSample;

// Each frame from the Rain actor with Regional Weather on: the storms' rate (0 only the authored ones, 1 stormy), the
// seed, the prevailing wind (m/s, degrees); off: no field (the Rain actor's own intensity everywhere)
void wfield_configure(bool on, float storminess, uint32_t seed, float wind_speed, float wind_dir);
bool wfield_on(void);
// The storms aged and moved, new ones born upwind of the camera to keep the rate's count (cm)
void wfield_update(float dt, float cam_x, float cam_y);
void wfield_sample(float x, float y, WeatherSample* s);
// An authored storm (Blueprint Add Storm): at x, y (cm), radius (cm), peak (mm/h), for seconds (it grows and fades in
// them); its index, -1: no room
int wfield_add_storm(float x, float y, float radius, float peak, float seconds);
void wfield_clear(void);
// The storms now for the GPU: x, y, radius (cm), the rate now (peak x its envelope, mm/h); their count
uint32_t wfield_pack(float out[WFIELD_MAX_STORMS][4]);
int wfield_selftest(void); // the field's sums, the drift, the birth upwind; 0 = ok
