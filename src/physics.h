// Physics (notas-dev/fisica.md): Unreal's Simulate Physics on actors. Rigid bodies (boxes and spheres from their meshes)
// fall, collide, stack, roll and rest; the level's other meshes are static colliders, the Characters push them;
// Fracture breaks a body into pieces on a hard hit (Unreal's Chaos Geometry Collection, simplified). A fixed 120 Hz step
// (substeps), sequential impulses with friction, sleeping; nothing allocated per frame, nothing done without bodies.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

typedef struct PhysicsStats {
    uint32_t bodies, awake, statics, contacts, pieces_spawned;
    uint32_t joints, joints_broken; // Physics Constraints this frame, broken since play began
    float joint_error_max;          // the farthest a constraint's anchors drifted apart since play began (cm)
    double ms;            // the last frame's step (CPU)
    double ms_peak;       // the costliest frame since play started
    double busy_ms;       // the frames' total while at least half the bodies were awake (the measure: busy_ms / busy_steps)
    uint32_t busy_frames, busy_steps;
    float settle_time;    // seconds of play until at most a quarter was still awake (0: not yet)
    float play_time;
    // the collision shapes (physics_shapes.c): hulls and triangle trees made this play (a cache: once a mesh), their cost,
    // GJK and EPA runs since play began (EPA's fallback: the least overlap of a few axes)
    uint32_t hulls, tri_meshes;
    double shapes_build_ms, shapes_build_peak_ms;
    uint32_t gjk, epa, epa_fallback;
    // (Q8) the step's phases while busy (summed like busy_ms), the frames' total, CCD sweeps, islands and threads
    double busy_broad_ms, busy_narrow_ms, busy_solve_ms;
    double ms_sum;
    uint32_t play_frames;
    uint32_t ccd_sweeps, islands, threads;
    double phase_ms[12];  // (busy, summed) each part of the step: PHYS_PHASE_ names (physics_phase_name)
    uint32_t colors, overflow, stages; // the most graph colors a step used this play; the last step's contacts in the
                                       // overflow and its solver's stages (each ends at a barrier)
    uint32_t pairs;       // the last step's pairs (bounds overlapping: the narrowphase's)
    double solver_ms[8];  // the solver's stages summed since play began (pack, integrate, warm, solve, relax, restitution,
    uint32_t solver_steps; // store, joints) over these steps
    double all_phase_ms[12]; // the phases summed over every step (busy or not), and those steps
    uint32_t all_steps;
    double frame_ms[2];   // summed since play began: the frames' work outside the steps (gather and sync; write back and fracture)
} PhysicsStats;
const char* physics_phase_name(int i);

// Play starts / ends (the static colliders gathered; the run's state dropped)
void physics_begin_play(World* w);
void physics_end_play(World* w);
// Each frame while playing (after the Blueprints: their impulses this frame count)
void physics_update(World* w, float dt);
const PhysicsStats* physics_stats(void);
// (Q8) the tests': sleeping off (every body stays awake: a pile that trembles shows it); a body's continuous collision
void physics_set_sleep_enabled(bool on);
void physics_set_ccd(World* w, Entity e, bool on);
// (Q8) Blueprints' bodies (Unreal's names): forces over the frame (kg cm/s2; Accel Change: cm/s2 whatever the mass),
// torques (kg cm2/s2; deg/s2), angular impulses (Vel Change: deg/s added), an impulse at a point (it spins too),
// velocities (cm/s, deg/s), the mass (kg; 0: from the volume and the material), sleep, gravity, the Physical Material
void physics_add_force(World* w, Entity e, const float force[3], bool accel_change);
void physics_add_torque(World* w, Entity e, const float torque[3], bool accel_change);
void physics_add_angular_impulse(World* w, Entity e, const float impulse[3], bool vel_change);
void physics_add_impulse_at(World* w, Entity e, const float impulse[3], const float location[3]);
void physics_set_linear_velocity(World* w, Entity e, const float v[3], bool add);
void physics_set_angular_velocity(World* w, Entity e, const float deg_s[3], bool add);
void physics_get_angular_velocity(World* w, Entity e, float deg_s[3]);
float physics_get_mass(World* w, Entity e);
void physics_set_mass(World* w, Entity e, float kg);
void physics_wake(World* w, Entity e, bool awake); // false: Put Rigid Body to Sleep
bool physics_is_awake(World* w, Entity e);
void physics_set_gravity(World* w, Entity e, bool on);
void physics_set_material(World* w, Entity e, uint32_t material); // physmat_vpath_hash; 0: the body's own values
// (Q8) the constraints' (e: the constraint's actor): a drive (angular: target a rotator in degrees, velocity deg/s
// around X Y Z; linear: cm, cm/s), its bits (CDRIVE_), spring Hz (< 0: kept) and damping ratio (<= 0: kept), the most
// it pushes (0: no limit); the limits (cm, degrees); an axis' motion (0-2 linear X Y Z, 3 twist, 4 swing 1, 5 swing 2);
// the last step's force and torque (true: broken)
void physics_constraint_drive(World* w, Entity e, bool angular, const float target[3], const float velocity[3], uint32_t bits, float hz,
                              float damping, float max);
void physics_constraint_limits(World* w, Entity e, float linear, float twist, float swing1, float swing2);
void physics_constraint_motion(World* w, Entity e, uint32_t axis, uint32_t motion);
bool physics_constraint_force(World* w, Entity e, float* force, float* torque);
// Blueprints' nodes: Add Impulse (cm/s * kg), Set Simulate Physics
void physics_add_impulse(World* w, Entity e, const float impulse[3], bool vel_change);
void physics_set_simulate(World* w, Entity e, bool on);
void physics_radial_impulse(World* w, const float origin[3], float radius, float strength); // strength: cm/s at the middle
// Physics Constraints (Blueprints' Break Constraint, Set Constraint Motor): e is the constraint's actor
void physics_constraint_break(World* w, Entity e);
void physics_constraint_motor(World* w, Entity e, bool on, float speed);
// The Debug Viewer's list: each awake body's share of the step (the costliest first)
uint32_t physics_profile_items(Entity* e, float* ms, uint32_t* contacts, uint32_t cap);
// (2026-11-19) a mesh's triangles in their BVH (made on first use, the main thread): the traces' Trace Complex
struct PhysMesh;
const struct PhysMesh* physics_mesh_tris(uint32_t mesh);
