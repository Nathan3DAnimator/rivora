// Water on the camera's lens (2026-11-15, "as gotas têm que escorrer na tela de forma realista, dependendo do ângulo, se
// está olhando para frente, de lado, cima ou baixo, e ter inércia, poder mexer a câmera rápido para tirar a chuva; tudo
// deve poder ser controlado e ajustado pelo dev"; notas-dev/agua.md). A few hundred drops simulated on the CPU in the
// screen's plane (x 0..aspect, y 0..1 down, in screen heights): gravity projected on the lens by the camera's tilt,
// sticking until the force passes what the drop's size holds, inertia from the camera's moves and turns, a quick turn
// throwing them off, the air of moving forward pushing them out, merging, trails that dry. Packed for composite.frag
// (lens_water): the drops, the trails' segments and a grid of tiles listing what touches each.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LENS_MAX 160        // drops
#define LENS_TRAIL 8        // points of a drop's trail (7 segments)
#define LENS_TX 32          // the tiles across and down the screen
#define LENS_TY 18
#define LENS_TILE_ITEMS 24  // what one tile lists (drops and trail segments)
#define LENS_TILE_VEC4 7    // a tile: its count and items as floats, 1 + 24 in 7 vec4
// the packed buffer, in vec4: [0] count of drops, of segments, tiles across, down; then 2 per drop (x, y, radius,
// alpha; motion direction xy, its stretch, how much of it is still a snowflake), 2 per segment (x0, y0, x1, y1; width, alpha), then the tiles
#define LENS_DROP_BASE 1u
#define LENS_SEG_BASE (LENS_DROP_BASE + 2u * LENS_MAX)
#define LENS_TILE_BASE (LENS_SEG_BASE + 2u * LENS_MAX * (LENS_TRAIL - 1))
#define LENS_VEC4 (LENS_TILE_BASE + LENS_TX * LENS_TY * LENS_TILE_VEC4)

typedef struct LensParams {
    float amount;     // 0..2: how many drops arrive (x the rain's rate)
    float size;       // x the drops' size
    float gravity;    // 0..3: how fast they run down
    float stickiness; // 0..3: how big a drop must be to start running (more: they stay longer)
    float inertia;    // 0..3: how much the camera's moves and turns push them
    float shake_off;  // 0..3: how easily a quick move throws them off (0: never)
    float airflow;    // 0..3: how much moving forward pushes them out toward the edges
    float dry_time;   // s for them to dry once out of the rain
    float trail;      // 0..2: how long the wet trail they leave lasts
} LensParams;

typedef struct LensCamera {
    float pos[3], fwd[3], right[3], up[3]; // cm, unit vectors
    float aspect;                          // width / height
} LensCamera;

void lens_reset(void);
// One step: arrive = drops a second hitting the lens now (0: none; drying = out of the rain: they shrink and dry);
// snow: what arrives are flakes (they stick, then melt into drops in a few seconds)
void lens_step(const LensParams* p, const LensCamera* c, float arrive, bool drying, bool snow, float dt);
uint32_t lens_drop_count(void);
// The drops, trails and tiles into dst (LENS_VEC4 vec4); false if dst is too small
bool lens_pack(float* dst, size_t floats);
// The simulation on its own: gravity by the tilt, sticking, inertia, the shake, merging, drying; 0 = ok
int lens_selftest(void);
