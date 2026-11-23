// (2026-11-23) The vec3 helpers of the Tlender's math3d.h the copied rig files use (charmap.c, charmap_analyze.c,
// rig_systems.c, rig_detect.c): apart from rig_armature.h so the engine files that include rig_detect.h keep their own
// v3() and friends
#pragma once
#include "rig_armature.h"

static inline vec3 v3(float x, float y, float z) { vec3 r = { x, y, z }; return r; }
static inline float tl_minf(float a, float b) { return a < b ? a : (b == b ? b : a); }
static inline float tl_maxf(float a, float b) { return a > b ? a : (b == b ? b : a); }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_min(vec3 a, vec3 b) { return v3(tl_minf(a.x, b.x), tl_minf(a.y, b.y), tl_minf(a.z, b.z)); }
static inline vec3 v3_max(vec3 a, vec3 b) { return v3(tl_maxf(a.x, b.x), tl_maxf(a.y, b.y), tl_maxf(a.z, b.z)); }

