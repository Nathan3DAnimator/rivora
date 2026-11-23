// Vehicles (notas-dev/veiculos.md): Unreal's Chaos Vehicles, wider: cars, motorcycles, trucks, tanks, boats, trains,
// airplanes and helicopters from one model. A Vehicle asset (.rvehicle) is a chassis (mass, center of mass, box, drag)
// and a list of parts; each part is physics (a wheel with its spring and damper, an engine, a gearbox, a wing...) and
// may move a component of the actor by name (the wheel's mesh spins, steers and rides its suspension; a door swings on
// its hinge; a piston stretches between two parts; a propeller turns).
//   - The simulation is the vehicle's own (SI units inside, fixed substeps): a rigid chassis; raycast suspension with
//     springs, dampers and anti-roll bars; tires by a magic formula with a friction ellipse; engine torque curve, clutch,
//     automatic / manual gearbox, open / locked / limited-slip differentials; skid steering (tanks); lean assist
//     (motorcycles); buoyancy on Water Bodies, propellers and rudders (boats); wings with lift, stall and control
//     surfaces (airplanes); rotors with collective, cyclic and a stability assist (helicopters); rails (trains, their
//     cars coupled). The chassis collides with the level's boxes and landscapes.
//   - Inputs: the player (W/S throttle and brake, A/D steer, Space handbrake, Shift/Ctrl up/down for aircraft, Q/E yaw,
//     F a door) when possessed, or Blueprint nodes (Set Throttle Input...).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define VEH_NAME 32
#define VEH_MAX_PARTS 48
#define VEH_MAX_GEARS 8
#define VEH_PARAMS 10

typedef enum VehKind { VK_CAR = 0, VK_MOTORCYCLE, VK_TRUCK, VK_TANK, VK_BOAT, VK_TRAIN, VK_AIRPLANE, VK_HELICOPTER, VK_KIND_COUNT } VehKind;
extern const char* const g_veh_kind_names[VK_KIND_COUNT];

typedef enum VehPartKind {
    VP_WHEEL = 0, VP_ENGINE, VP_GEARBOX, VP_DIFFERENTIAL, VP_HINGE, VP_SLIDER, VP_PISTON, VP_GEAR, VP_PROPELLER, VP_WING,
    VP_ROTOR, VP_FLOAT, VP_BOGIE, VP_KIND_COUNT
} VehPartKind;

// What drives a part (a wing's control surface, a door, a slider, a piston)
typedef enum VehInput {
    VI_NONE = 0, VI_THROTTLE, VI_STEER, VI_BRAKE, VI_HANDBRAKE, VI_PITCH, VI_ROLL, VI_YAW, VI_COLLECTIVE, VI_ACTION1, VI_ACTION2,
    VI_ACTION3, VI_ACTION4, VI_COUNT
} VehInput;
extern const char* const g_veh_input_names[VI_COUNT];

typedef struct VehPartInfo {
    const char* name;
    const char* tip;
    int nparams;
    const char* params[VEH_PARAMS];
    float def[VEH_PARAMS], lo[VEH_PARAMS], hi[VEH_PARAMS];
} VehPartInfo;
const VehPartInfo* veh_part_info(int kind);

typedef struct VehPart {
    uint8_t kind, input, mirror, pad;  // mirror: a control surface on the left (-y) deflects the other way (ailerons)
    char name[VEH_NAME];               // its name (Blueprint: Open Part "Door_L")
    char attach[VEH_NAME];             // the actor's component it moves (by name; empty: none)
    char link[VEH_NAME];               // a piston: the part it reaches; a gear: the part it turns with ("Engine", "Steer")
    float pos[3];                      // in the vehicle (cm, from the actor)
    float axis[3];                     // hinge / slider / gear / propeller / rotor: its axis; wing: its normal (up)
    float p[VEH_PARAMS];               // its numbers (veh_part_info(kind)->params)
    // its own model (2026-11-08, a vehicle from the user's meshes): a Static Mesh (render.h id; 0: the basic shape
    // the kind draws, or none on a body mesh), its material (0: the mesh's own), its turn and scale as imported
    uint32_t mesh, material;
    float mloc[3], mrot[3], mscale[3]; // (mloc: where its model sits, cm from the actor; a wheel's hangs from pos)
} VehPart;

typedef struct VehicleAsset {
    char name[VEH_NAME], path[512];
    uint32_t hash, version;
    uint8_t kind;
    float mass;                        // kg
    float com[3];                      // the center of mass (cm, from the actor)
    float extent[3];                   // the chassis' half size (cm): its collision box
    float drag;                        // Cd x A (m2)
    float downforce;                   // N per (m/s)^2
    float gears[VEH_MAX_GEARS];        // forward ratios (1st ...)
    uint32_t ngears;
    float reverse;                     // the reverse ratio
    float cam_distance, cam_height;    // the chase camera (cm)
    float max_speed;                   // km/h (the limiter; 0: none)
    VehPart parts[VEH_MAX_PARTS];
    uint32_t nparts;
    // the body's model (0: the boxes of the kind): a Static Mesh at body_loc (cm from the actor), turned and scaled
    uint32_t body_mesh, body_material;
    float body_loc[3], body_rot[3], body_scale[3];
    bool dirty;
} VehicleAsset;

typedef enum VehTemplate {
    VT_CAR = 0, VT_SPORTS, VT_TRUCK, VT_MOTORCYCLE, VT_TANK, VT_BOAT, VT_LOCOMOTIVE, VT_WAGON, VT_AIRPLANE, VT_HELICOPTER, VT_COUNT
} VehTemplate;
extern const char* const g_veh_template_names[VT_COUNT];

void vehicle_set_root(const char* content_root);
VehicleAsset* veh_load(const char* vpath);
VehicleAsset* veh_find(uint32_t hash);
bool veh_save(VehicleAsset* a);
uint32_t veh_count(void);
VehicleAsset* veh_at(uint32_t i);
void veh_template(VehicleAsset* a, int tmpl);                       // a template's chassis and parts
uint32_t veh_create(const char* folder_vpath, const char* name, int tmpl); // a new asset from a template (its hash, 0: failed)
VehicleAsset* veh_temp(const char* name, int tmpl);                 // in memory only (the tests' scenes)

// The actor of a vehicle: CVehicle and its components made from the asset's parts (basic shapes: a body box, wheels,
// doors, wings, rotors...) at a place. Its entity
Entity vehicle_spawn(World* w, uint32_t asset, const float loc[3], float yaw, const char* name);
// A vehicle from the user's own meshes (Actor > Make Vehicle from Selected): the selected Static Mesh actors become one
// vehicle of a kind (VehTemplate). The biggest is the body; the wheels are found by their names (wheel, tire, roda,
// pneu, rueda...) or, failing that, by their shape (round, low, at the corners), measured (radius, width, place) and
// given the template's suspension and tire; doors (door, porta, puerta) swing on their front edge; the rest ride on
// the body. No wheel meshes on a wheeled kind: the wheels are placed under the body's corners (invisible: the body
// carries their look). front: the model's forward axis (0 +X, 1 -X, 2 +Y, 3 -Y; -1: the longest side). The asset is
// saved in folder_vpath; the selected actors are destroyed and the vehicle's actor returned (ENTITY_NONE: failed, err)
Entity vehicle_from_actors(World* w, const Entity* sel, uint32_t n, int tmpl, int front, const char* folder_vpath, char* err, size_t err_cap);
// The asset's wheels refitted to its body mesh's bounds (the editor's Fit to Body Mesh)
void veh_fit_to_body(VehicleAsset* a);
float veh_rest_height(const VehicleAsset* a); // cm above the ground its actor stands at (its wheels down, or its box)
// A Water Body actor (a plane of size x size cm at z) and a Rail actor (a loop through points)
Entity water_spawn(World* w, const float center[3], float size_x, float size_y);
Entity rail_spawn(World* w, const float (*points)[3], uint32_t n, bool closed, const char* name);

// --- Play ---
void vehicle_begin_play(World* w);
void vehicle_end_play(World* w);
// Each frame while playing (after the Blueprints and the physics): the vehicles simulate, their components follow
void vehicle_update(World* w, float dt);
// The editor (not playing): the rails' components rebuilt when their points changed
void vehicle_editor_update(World* w);
void vehicle_rebase(World* w, const float d[3]); // (procgen.c: the origin moved by d cm)

// Inputs (Blueprint nodes, the tests); the player's override them while it drives
typedef struct VehControls { float throttle, brake, steer, handbrake, pitch, roll, yaw, collective; } VehControls;
void vehicle_set_controls(World* w, Entity v, const VehControls* c);
void vehicle_set_control(World* w, Entity v, int input, float value); // one input (VehInput)
void vehicle_shift(World* w, Entity v, int gear);                     // a gear (-1 reverse, 0 neutral, 1..), automatic off
void vehicle_set_engine(World* w, Entity v, bool on);
void vehicle_part_target(World* w, Entity v, const char* part, float t); // a hinge / slider / piston: 0 closed .. 1 open
void vehicle_part_toggle(World* w, Entity v, const char* part);
float vehicle_part_value(World* w, Entity v, const char* part);       // where it is now (0..1)
bool vehicle_enter(World* w, Entity v);                               // the player drives it (its pawn kept to come back)
bool vehicle_exit(World* w);                                          // back to the pawn, next to the vehicle
// Its state: speed (km/h, signed forward), engine rpm, gear, wheels on the ground, in water, altitude above the ground
typedef struct VehInfo {
    float speed_kmh, rpm, altitude_m, submerged;
    int gear;
    uint32_t wheels_down, wheels;
    bool engine_on, upright;
    float pos[3];               // cm
    float rail_s;               // a train: meters along its rail
    float pitch_deg, roll_deg, yaw_deg; // its attitude (nose up, right wing down, heading)
    float throttle;             // 0..1: the throttle now (the player's or the Blueprint's; the engine's load: B8's sound)
    float damage, engine_damage, toe_deg[2]; // (Q27.4) 0..1 the whole; the engine's power lost; the front wheels' toe (deg)
    uint32_t hits;
} VehInfo;
bool vehicle_info(World* w, Entity v, VehInfo* out);
// The chase camera of a driven vehicle (bp_player_camera)
bool vehicle_camera(World* w, Entity v, float pos[3], float* yaw, float* pitch);
// The water's surface at x, y (cm): its height; false: no Water Body there
bool water_height(World* w, float x, float y, float* z);
// (B12f) The water at a point (cm): its surface, current (cm/s), density, depth; false: none. water_gather once a frame
void water_gather(World* w);
bool water_query(World* w, const float p[3], float* level, float cur[2], float* rho, float* depth);

typedef struct VehStats { uint32_t vehicles, wheels, contacts; double ms; uint32_t crashes; } VehStats; // (crashes: Q27.4, this frame's)
const VehStats* vehicle_stats(void);
int vehicle_mesh_selftest(const char* content_root); // a vehicle made from meshes (the editor: --vehicle-mesh-test), 0 = ok
int vehicle_selftest(void); // the tire, the engine and the gearbox, buoyancy, a wing's lift; 0 = ok
