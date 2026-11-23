// (2026-11-19) Debug drawing in the world (Unreal's Draw Debug Line / Point / Box / Sphere / Capsule and the traces'
// Draw Debug Type): lines and points kept on the CPU as 32-byte primitives, sent to the GPU once a frame and drawn over
// the picture as screen-space quads (anti-aliased, a pixel width each), dimmed where the scene hides them
// (render_debug_draw). Hundreds of thousands a frame: the batches fill their room from the workers (dd_reserve), no
// lock, no malloc in the frame (notas-dev/traces.md).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Unreal's EDrawDebugTrace
enum { DD_NONE = 0, DD_FOR_ONE_FRAME = 1, DD_FOR_DURATION = 2, DD_PERSISTENT = 3 };

typedef struct DdPrim {
    float a[3];
    uint32_t rgba; // 0xAABBGGRR (bytes r, g, b, a in memory)
    float b[3];
    float size;    // > 0: a line from a to b this many pixels wide; < 0: a point (a square) at a, -size pixels
} DdPrim;

uint32_t dd_rgba(float r, float g, float b, float a); // a Linear Color's (0-1) as sRGB bytes
// seconds: 0 this frame only; > 0 that long; < 0 until Flush Persistent Debug Lines
void dd_line(const float a[3], const float b[3], uint32_t rgba, float thickness, float seconds);
void dd_point(const float p[3], uint32_t rgba, float size, float seconds);
void dd_box(const float mn[3], const float mx[3], uint32_t rgba, float thickness, float seconds);
void dd_sphere(const float c[3], float r, uint32_t rgba, float thickness, float seconds);
void dd_capsule(const float c[3], float half_height, float r, uint32_t rgba, float thickness, float seconds);
// Room for n primitives (this frame's, or kept for seconds when seconds != 0): NULL when full (counted, the room grows
// next frame). Any thread
DdPrim* dd_reserve(uint32_t n, float seconds);
void dd_flush_persistent(void);
void dd_clear_all(void); // every one, kept or not (the play ended: the main thread)
// The frame's start (the main loop, before the game): this frame's list emptied, the expired ones dropped, the room
// grown where it ran out
void dd_frame_begin(double now);
// What render_debug_draw draws: this frame's and the kept ones (two runs)
const DdPrim* dd_frame_prims(uint32_t* n);
const DdPrim* dd_kept_prims(uint32_t* n);
void dd_set_enabled(bool on); // (the console's / the game's switch; on by default)
bool dd_enabled(void);
uint32_t dd_dropped_last(void); // primitives that found no room last frame

// A trace as the GPU draws it (debug_line.vert expands it: the line to the hit in the trace color, after it in the hit
// color, a point on the hit; a miss: the line in the trace color). 32 bytes, written by the batches' workers
typedef struct DdTrace {
    float start[3];
    float t;           // where it hit (0-1); >= 1: a miss
    float end[3];
    uint32_t colors;   // the trace color (RGB565) << 16 | the hit color (RGB565)
} DdTrace;
uint32_t dd_rgb565(uint32_t rgba);
// Room for n trace records this frame (NULL: full, counted; any thread)
DdTrace* dd_reserve_traces(uint32_t n);
const DdTrace* dd_frame_traces(uint32_t* n);
// (2026-11-19) an async batch's records (scene_query.c, its own room, kept two frames): drawn this frame, not copied here
void dd_add_trace_block(const DdTrace* p, uint32_t n);
uint32_t dd_trace_blocks(const DdTrace* const** p, const uint32_t** n); // the frame's blocks

// (2026-11-19) The traces as Unreal draws them: a hit's trace in trace_rgba to the hit, hit_rgba after it, a point on
// the impact; a miss in trace_rgba to the end. n of them at once, filled on the workers (any count)
struct SqQuery;
struct SqHit;
void dd_traces(const struct SqQuery* qs, const struct SqHit* hits, uint32_t n, uint32_t trace_rgba, uint32_t hit_rgba, float seconds);
