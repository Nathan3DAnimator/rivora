// Crowds (crowd.c, notas-dev/multidao.md): a Crowd Spawner actor (CCrowd) brings thousands of animated agents with AI,
// simulated and drawn as one thing, not as actors:
//   * the agents are arrays (structure of arrays), stepped in parallel; their AI is a flow field (a breadth first
//     search of the navigation grid from where the player WILL be: every agent follows the shortest way there, taking
//     the shortcuts and cutting the player off) plus roles (chase, surround on a ring of slots, roam, flee) and local
//     separation (a spatial hash);
//   * two minds: Hive (the group decides at once: one sees the player, all know; the ring's slots shared out by angle so
//     they close in from every side) and Individual (each one sees, remembers, reacts after its own delay);
//   * difficulty is plain numbers on the component: prediction (s ahead), reaction (s), aggression (the share that
//     charges), sight, speeds, separation, ring radius;
//   * drawing: the Skeletal Mesh's parts baked per animation frame (idle, walk, run: rows in the mesh's space); each
//     frame a compute shader (crowd.comp) writes every agent's parts into the scene's instances (render.c), which the
//     GPU culling, LODs and shadows then treat like any other (far agents drop their small parts and their shadows).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "anim.h"
#include "ecs.h"

#define CROWD_MAX_RUNS 16
#define CROWD_FRAMES 32                // the baked clips: frames per cycle (resampled)
#define CROWD_MAX_INSTANCES (1u << 21) // parts drawn at most (2 M)

// One agent for the GPU (crowd.comp): 36 bytes
typedef struct CrowdAgentGpu {
    float pos[3], yaw;  // cm, degrees
    float pa, pb;       // the two clips' phases (0-1)
    uint32_t clips;     // clip a | clip b << 8 | LOD << 16 (0: all parts and shadows, 1: no shadows, 2: the big parts only, 3: the two biggest, Q27.1)
    float blend;        // b's weight
    uint32_t look;      // (Q27.9) 0: as the mesh is; else skin tone (bits 0-2), top (3-7), bottom (8-11), build (12-15), on (31)
} CrowdAgentGpu;

// A crowd to draw this frame
typedef struct CrowdDraw {
    Entity e;                    // the spawner
    const SkelMesh* mesh;
    uint32_t nagents, nparts;
    const CrowdAgentGpu* agents;
    const float* baked;          // [3 clips][CROWD_FRAMES][nparts][12]: each part's rows in the mesh's space
    const uint8_t* big;          // per part: kept at LOD 2
    uint32_t version;            // bumped when the baked data or the parts changed
} CrowdDraw;

void crowd_begin_play(World* w);
void crowd_end_play(World* w);
// Every frame: playing, the agents think and move; in the editor they stand where they start (and follow its edits)
void crowd_update(World* w, float dt, bool playing, const float camera[3]);
// (Q27.1) An alarm (a shot, an explosion) at a point: the agents within radius hear it as a wave (15 m/s from the point)
// and panic, running from it 6-12 s, promoted at most 64 a frame, nearest first (a shot into thousands never wakes them
// all in one frame). Playing only; the Blueprint node Crowd Alarm
void crowd_alarm(const float at[3], float radius);
uint32_t crowd_count_near(const float at[3], float radius); // (the tests: the agents within radius of a point)
bool crowd_nearest(const float at[3], float radius, float out[3]); // (Q27.10: a crime's witness)
uint32_t crowd_draws(const CrowdDraw** out);
uint32_t crowd_instances(void); // the parts of every crowd (the scene's room for them)

// The Debug Viewer and the tests
typedef struct CrowdStats {
    uint32_t crowds, agents, chasing, surrounding, knowing;
    double sim_ms, flow_ms, think_ms, hash_ms, step_ms, pack_ms; // (the parts of sim_ms)
    uint32_t flow_builds;
    uint32_t panicked, promoted, promote_peak, alarms; // (Q27.1) panicking now, promoted this frame, the most in a frame, alarms raised
    uint32_t looks; // (Q27.9) distinct looks among the agents (counted when a crowd is made)
} CrowdStats;
const CrowdStats* crowd_stats(void);
// The mean distance of a crowd's agents to a point (the tests: did they come?)
float crowd_mean_distance(World* w, Entity spawner, const float at[3]);
