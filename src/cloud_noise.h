// The clouds' noise and tables, made once on the CPU (cloud_noise.c; the Tlender's sky_clouds.c model)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLOUD_SHAPE_N 64  // the shape texture's side (RGBA16: Perlin-Worley, Worley fbm at 4, 8, 16 cells)
#define CLOUD_DETAIL_N 32 // the detail texture's side (RGBA8: Worley fbm at 2, 4, 8 cells)
#define CLOUD_QUANT_N 65  // the coverage thresholds (quantiles of the shape)
#define CLOUD_MEAN_N 17   // the prefiltered means: coverage x height gradient
#define CLOUD_TABLES (CLOUD_QUANT_N + CLOUD_MEAN_N * CLOUD_MEAN_N) // floats
#define CLOUD_WEATHER_N 1024  // the coverage map's texture (R16), tiling
#define CLOUD_WEATHER_CELLS 10 // the first octave's cells across it (22 km each: 220 km before it repeats)

// shape: N^3 x 4 uint16; detail: N^3 x 4 uint8; tables: CLOUD_TABLES floats (the thresholds, then the means [cov][grad])
bool cloud_noise_make(uint16_t* shape, uint8_t* detail, float* tables);
// the coverage map: CLOUD_WEATHER_N^2 uint16 (the value-noise fbm, tiling)
void cloud_weather_make(uint16_t* out);
