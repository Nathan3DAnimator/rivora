// The IK Rig and the bodies' physics (notas-dev/ikrig.md): Unreal's IK Rig and Physics Asset for the Skeletal Meshes.
//   - IK Rig (.rikrig): goals (a bone that reaches a place: set by Blueprint or following an actor) and solvers run in
//     order on the pose, in the world: Chain (FABRIK over any number of bones, cone limits, stiffness, a pole), Limb
//     (a chain of two with its pole), Full Body (chains from one root to several goals: the root moves too, by its
//     stiffness), Look At (a bone aimed at its goal). Bone settings: stiffness and a cone limit per bone.
//   - Physics Asset (.rphys): a body per bone (capsule, sphere or box, its size and offset, its mass) and the
//     constraints between them (swing cone, twist), like Unreal's Physics Asset Editor; made automatically from the
//     skeleton and edited in its own window.
//   - The simulation (XPBD, substeps): ragdoll (every body), the bodies below a bone (a tail, hair, a hanging arm), or
//     physical animation (each simulated body pulled toward the animated pose by its strength: jiggle without going
//     limp), blended with the animation by a weight. Hard joints by positions, so nothing explodes or stretches; the
//     bodies collide with the level's boxes and landscapes and with each other (not with their neighbors).
// Both run in anim.c after the pose and before the parts and skins are placed (pose jobs: one instance each).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"
#include "ecs.h"

#define IK_MAX_GOALS 16
#define IK_MAX_SOLVERS 16
#define IK_MAX_BONE_SETTINGS 64
#define PHYS_MAX_BODIES 48
#define PHYS_MAX_CONSTRAINTS 48

typedef enum IkSolverKind { IKS_CHAIN = 0, IKS_LIMB, IKS_FULL_BODY, IKS_LOOK_AT, IKS_KIND_COUNT } IkSolverKind;
extern const char* const g_ik_solver_names[IKS_KIND_COUNT];

typedef struct IkRigGoal {
    char name[ANIM_NAME], bone[ANIM_NAME];
    char target_actor[ANIM_NAME]; // follows an actor of the level by name (empty: set by Blueprint: Set IK Goal)
    float offset[3];              // added to the actor's place (cm)
    float alpha;                  // 0..1 (the default; Set IK Goal's alpha multiplies it)
    uint8_t rotation;             // the goal's bone takes the actor's rotation too
    uint8_t pad[3];
} IkRigGoal;

typedef struct IkRigSolver {
    uint8_t kind, on, pad[2];
    char start[ANIM_NAME];        // Chain / Limb: the chain's first bone; Full Body: the root; Look At: the bone
    char goal[ANIM_NAME];         // the goal (Chain, Limb, Look At)
    uint32_t goal_mask;           // Full Body: its goals (bits of the rig's goals)
    float pole[3];                // Chain / Limb: the bend toward this direction in the actor's space (0: kept as posed)
    float root_stiffness;         // Full Body: 0 (the root goes all the way) .. 1 (stays)
    float axis[3];                // Look At: the bone's axis that aims (its space)
    int iterations;               // FABRIK's (10)
} IkRigSolver;

typedef struct IkBoneSetting {
    char bone[ANIM_NAME];
    float stiffness;              // 0 (free) .. 1 (keeps its pose)
    float limit;                  // the cone from its posed direction (deg; 0: none)
} IkBoneSetting;

typedef struct IkRig {
    char name[ANIM_NAME], path[512];
    uint32_t hash, skeleton;      // its vpath's, the Skeletal Mesh it was made for
    uint32_t ngoals, nsolvers, nbones;
    IkRigGoal goals[IK_MAX_GOALS];
    IkRigSolver solvers[IK_MAX_SOLVERS];
    IkBoneSetting bones[IK_MAX_BONE_SETTINGS];
    uint32_t version;
    bool dirty;
} IkRig;

typedef enum PhysShape { PHYS_CAPSULE = 0, PHYS_SPHERE, PHYS_BOX, PHYS_SHAPE_COUNT } PhysShape;

typedef struct PhysBody {
    char bone[ANIM_NAME];
    uint8_t shape, simulate, pad[2]; // simulate: 0 kinematic (follows the animation), 1 simulated (when the mesh simulates)
    float radius, length;         // capsule / sphere: radius; capsule: its segment's length (along the bone)
    float extent[3];              // box: half sizes
    float offset[3];              // the shape's center from the bone's head, in the bone's space (cm)
    float axis[3];                // the capsule's direction in the bone's space (to its child)
    float mass;                   // kg
    float strength;               // physical animation: 0 free .. 1 holds the animated pose
    float muscle;                 // (2026-11-16) active ragdoll: its joint's muscle (1 as strong as the profile says)
} PhysBody;

// (Q10, 2026-11-18, "melhorar o sistema de ragdoll para ser realista") a joint's kind: what anatomy its limits were made
// for (the defaults of Generate Bodies; any can be edited)
enum { PJ_BALL = 0, PJ_HINGE, PJ_SPINE, PJ_NECK, PJ_SHOULDER, PJ_HIP, PJ_WRIST, PJ_TAIL, PJ_KIND_COUNT };
extern const char* const g_joint_kind_names[PJ_KIND_COUNT]; // Ball, Hinge (knee, elbow), Spine, Neck, Shoulder, Hip, Wrist / Ankle, Tail
typedef struct PhysConstraint {
    uint8_t parent, child, kind, pad; // bodies; kind: PJ_*
    float swing, twist;           // (the symmetric limits of before (deg): a file without the ones below opens with them)
    // (Q10) anatomical limits, asymmetric, from the BIND pose (the same whatever pose the ragdoll starts in): the child's
    // swing split into the bend plane (toward bend: a knee folds backward only, an elbow forward only, the spine bends
    // forward more than back) and across it (to the sides), an ellipse between these; the twist around the child's axis
    float bend_min, bend_max;     // deg: away from bend (-) .. toward it (+)
    float side_min, side_max;     // deg: to one side .. the other
    float twist_min, twist_max;   // deg
    float bend[3];                // the bend's direction in the parent body's frame (a knee: back; an elbow: forward)
    float damping;                // 1/s: the joint's damping (its relative turn slowed: settles instead of rattling)
    float tone;                   // 0..1: a plain ragdoll's muscle tone (it keeps some of the pose it fell in: not a rag)
} PhysConstraint;

// (Q10) DYNAMIC BONES (Unreal's AnimDynamics / a jiggle bone): a chain of the skeleton's bones moved by springs and
// gravity, the character's own motion and the wind, colliding with the Physics Asset's bodies: earrings, a ponytail, a
// tail, a backpack's straps, a belt's pouches, a holster. It runs whenever the game plays, ragdoll or not (after it)
#define PHYS_MAX_CHAINS 16
typedef struct DynChain {
    char bone[ANIM_NAME];         // its first bone (it turns; its head stays where the skeleton has it)
    char end[ANIM_NAME];          // its last bone (empty: down to the ends, branches too)
    uint8_t on, collide, preset, pad; // collide: against the Physics Asset's bodies (not its own bones')
    float stiffness;              // 0 (hangs free) .. 1 (keeps the animated shape)
    float damping;                // 0..1 (the share of its speed lost a frame at 60 fps)
    float gravity;                // its scale (1: real)
    float wind;                   // 0..1: how much the wind moves it
    float inertia;                // 0..1: how much it feels the character's own motion (0: carried rigidly)
    float radius;                 // cm: its bones' thickness against the bodies
    float limit;                  // deg: the most a bone turns from its animated direction (0: free)
} DynChain;

typedef struct PhysicsAsset {
    char name[ANIM_NAME], path[512];
    uint32_t hash, skeleton;
    uint32_t nbodies, nconstraints;
    PhysBody bodies[PHYS_MAX_BODIES];
    PhysConstraint constraints[PHYS_MAX_CONSTRAINTS];
    uint32_t nchains;             // (Q10) its dynamic bones
    DynChain chains[PHYS_MAX_CHAINS];
    float density;                // (Q10) g/cm3: Generate Bodies' masses from their volumes (1.0: a body's)
    float tone;                   // (Q10) the plain ragdoll's muscle tone, all joints (x each joint's)
    char getup_front[ANIM_NAME];  // (Q10) the montages an Active Ragdoll gets up with: lying face down, on its back
    char getup_back[ANIM_NAME];   // (empty: blended back into the animation)
    uint32_t version;
    bool dirty;
} PhysicsAsset;

void ikrig_set_root(const char* content_root);
void ikphys_vpath(const char* disk_path, char* out, size_t cap); // an asset's /Content path from its file's
IkRig* ikrig_load(const char* vpath);
IkRig* ikrig_find(uint32_t hash);
bool ikrig_save(IkRig* r);
uint32_t ikrig_count(void);
IkRig* ikrig_at(uint32_t i);
// A new rig for a Skeletal Mesh: its arms, legs and head found by their names (hand, foot, head...), a Full Body solver
// from the pelvis to the hands and feet and a Look At for the head. Its hash (0: failed)
uint32_t ikrig_create(const char* folder_vpath, const char* name, uint32_t skeleton);

PhysicsAsset* phys_load(const char* vpath);
PhysicsAsset* phys_find(uint32_t hash);
bool phys_save(PhysicsAsset* p);
uint32_t phys_count(void);
PhysicsAsset* phys_at(uint32_t i);
// A Physics Asset made from a Skeletal Mesh's bones (Unreal's: a capsule per long enough bone, the small ones riding
// their parent; constraints to the nearest body above, their limits by the kind of joint). Its hash (0: failed)
uint32_t phys_create(const char* folder_vpath, const char* name, uint32_t skeleton);
void phys_autogen(PhysicsAsset* p, const SkelMesh* m);
// (2026-11-23) the last Generate Bodies' fit to the mesh: bodies, fitted by the skin, pairs shrunk apart, overlaps left,
// the vertices read, the dynamic chains added from the rig, its time
typedef struct PhysFitStats { uint32_t bodies, fitted, shrunk, overlaps, vertices, chains; double ms; } PhysFitStats;
const PhysFitStats* phys_fit_stats(void);
// (Q10) a constraint's anatomical limits by its kind (the skeleton's forward and the bodies' bind places: the bend's way)
void phys_joint_defaults(PhysicsAsset* p, const SkelMesh* m, uint32_t constraint, int kind);
// (Q10) a dynamic chain on a bone with the defaults for the kind of thing it is; its index (-1: full)
enum { DYN_EARRING = 0, DYN_HAIR, DYN_TAIL, DYN_STRAP, DYN_BAG, DYN_EAR, DYN_CHAIN, DYN_CAPE, DYN_PRESET_COUNT };
extern const char* const g_dyn_preset_names[DYN_PRESET_COUNT]; // Earring, Hair, Tail, Strap / Pouch, Heavy Bag, Ear, Chain, Cape / Skirt
int phys_add_chain(PhysicsAsset* p, const char* bone, int preset);
void phys_chain_preset(DynChain* c, int preset);
// (Q10) a body's mass from its shape's volume and the asset's density (kg)
float phys_body_volume_mass(const PhysicsAsset* p, const PhysBody* b);
void phys_masses_from_volume(PhysicsAsset* p); // (every body's, then evened out along the joints)

// --- Runtime (anim.c calls these from its pose jobs: one instance each, nothing shared written) ---
// The IK rig on bones (the world's rows, the mesh's bones): the goals from the Blueprint's values and the actors
void ikrig_apply(World* w, Entity sk, const IkRig* rig, const SkelMesh* m, float bones[][3][4]);
// The bodies' simulation on bones (dt: since the last pose); none when the mesh doesn't simulate
void phys_apply(World* w, Entity sk, const PhysicsAsset* pa, const SkelMesh* m, float bones[][3][4], float dt);
// Blueprint (Set Simulate Physics on the mesh, Set All Bodies Below..., Physics Blend Weight, Add Impulse to Bone,
// Set IK Goal / Clear): the state of a Skeletal Mesh's entity (made here, on the main thread)
void phys_set_simulate(Entity sk, int mode, const char* bone);  // mode 0 off, 1 all (ragdoll), 2 the bodies below bone
void phys_set_blend(Entity sk, float weight);
void phys_add_impulse(Entity sk, const char* bone, const float impulse[3]); // cm/s added to the bone's body (all: NULL)
int phys_simulating(Entity sk);
// (2026-11-16, "um sistema de Ragdoll tipo Euphoria ... que ainda tenta seguir a animação de acordo com um peso") the
// ACTIVE RAGDOLL, simulate mode 3: every body simulated, and each joint's muscle turning its body toward the animated
// pose (a drive with a limited torque: a hit wins, then the muscle brings it back), the root held up and upright by the
// balance while it stands (the feet planted: any skeleton, the lowest ends found at the start: two feet, four paws).
// A push too strong: it stumbles (a step), then falls; lying still, it gets up (Auto Get Up) blending back into the
// animation where it lies. Its strength: overall and per bone (with the bones below), blended over time; profiles.
enum { PHYS_OFF = 0, PHYS_RAGDOLL, PHYS_BELOW, PHYS_ACTIVE };
enum { BAL_BALANCED = 0, BAL_STUMBLING, BAL_FALLEN, BAL_GETTING_UP };
#define MUSCLE_PROFILE_COUNT 6
extern const char* const g_muscle_profile_names[MUSCLE_PROFILE_COUNT]; // Default, Relaxed, Tense, Protective, Drunk, Limp
void phys_set_active_strength(Entity sk, const char* bone, float strength, float blend_time); // bone empty: all
bool phys_set_muscle_profile(Entity sk, const char* name_or_index);
// A hit: the impulse (cm/s) on the bone's body (a little on the rest), its muscles (and those below it) down to weaken
// (0..1), back over recover_time seconds
void phys_hit_reaction(Entity sk, const char* bone, const float impulse[3], float weaken, float recover_time);
int phys_balance_state(Entity sk); // BAL_* (-1: not an active ragdoll)
// (Q27.5) GTA 6's kind of reactions: a wound at a world point on a bone (severity 0..1): the hand of the other side holds it
// (torso, arm, head) or the leg limps; past 0.7 it falls and writhes a while on the ground before it gets up
void phys_wound(Entity sk, const char* bone, const float point[3], float severity);
float phys_limp(Entity sk);           // the limp's severity now (0: none): the locomotion's Injured style
bool phys_holding_wound(Entity sk);
bool phys_writhing(Entity sk);
bool phys_high_fall(Entity sk);       // falling fast: the arms flail
void ikphys_set_view(const float p[3]); // (anim.c) the view: the active ragdolls' budget (the 32 nearest simulate)
void phys_get_up(Entity sk);       // lying: gets up now (else Auto Get Up after its delay)
void phys_set_auto_get_up(Entity sk, bool on, float delay);
// (anim.c, the main thread, before the pose jobs) the actor moved where its ragdoll stands or lies (a step taken, a get
// up), its Character's movement held while it is down
void ikphys_main(World* w, Entity sk);
void ikrig_set_goal(Entity sk, const char* goal, const float pos[3], const float rot[3], float alpha);
void ikrig_clear_goal(Entity sk, const char* goal);
// (anim.c, the main thread, before the pose jobs: the assets loaded, a component's simulation started once)
void ikphys_prepare(Entity sk, uint32_t ik_rig, uint32_t physics_asset, int simulate, const char* bone, float blend);
void ikphys_frame_end(void);                                     // (the frame's stats)
void ikphys_forget(Entity sk);                                  // (its state dropped: the entity went)
void ikphys_reset(void);
void ikphys_rebase(World* w, const float d[3]);                  // (procgen.c: the origin moved by d cm)
// (Q10) Set All Bodies Below Physics Blend Weight: the physics' weight of the bodies from a bone down (empty: all), over
// blend_time seconds: an arm let go limp while the rest plays, a body handed back to the animation part by part
void phys_set_blend_below(Entity sk, const char* bone, float weight, float blend_time);
// (Q10) a fallen Active Ragdoll lies face down (1), on its back (0); -1: not down
int phys_face_down(Entity sk);
// (Q10) the dynamic bones of a Skeletal Mesh: on or off, the wind they feel (cm/s, added to the level's), started over
// where the animation is (a teleport)
void dyn_set_enabled(Entity sk, bool on);
void dyn_set_wind(Entity sk, const float wind[3]);
void dyn_reset(Entity sk);
// (anim.c pose_one, after phys_apply) the dynamic chains on bones (dt: since the last pose); runs while playing
void dyn_apply(World* w, Entity sk, const PhysicsAsset* pa, const SkelMesh* m, float bones[][3][4], float dt);
// (anim.c pose_one, last) the bodies' capsules at the final pose, kept for the cloth: a garment on a skinned character
// collides with its Physics Asset (this frame's and last frame's place: a fast arm is swept, not jumped)
void phys_publish_bodies(Entity sk, const PhysicsAsset* pa, const SkelMesh* m, const float bones[][3][4]);
// (cloth.c) the published capsules within reach of a point (cm, world): a, b, radius, last frame's a and b, the
// Skeletal Mesh's entity; their count
typedef struct PhysCapsule { float a[3], b[3], r, pa[3], pb[3]; uint32_t owner; int body; } PhysCapsule; // (body: its index in the owner's Physics Asset)
uint32_t phys_capsules_near(const float center[3], float reach, PhysCapsule* out, uint32_t cap);
// Assets in memory only (no file: the tests' scenes), from a Skeletal Mesh: a Physics Asset made from its bones, an
// empty IK Rig (its goals and solvers set by the caller). NULL: failed
PhysicsAsset* phys_temp(uint32_t skeleton, const char* name);
IkRig* ikrig_temp(uint32_t skeleton, const char* name);
// A Skeletal Mesh's simulation now (the tests): its bodies' lowest and highest centers, the fastest, asleep; the IK's
// largest distance from a reached goal (cm, -1: none this frame). False: no state
typedef struct IkPhysInfo { float zmin, zmax, vmax, ik_error; bool asleep; uint32_t bodies; int fastest, spinning; float wmax; int balance; float root_z;
    float limit_over; int limit_worst; float self_pen; int face_down; } IkPhysInfo; // (fastest: its body; Q10: the most a joint is past its limits (deg) and which, the deepest overlap of two bodies not neighbors (cm))
bool ikphys_info(Entity sk, IkPhysInfo* out);
// The stats of the last frame
typedef struct IkPhysStats { uint32_t rigs, ragdolls, bodies, contacts; double ms; uint32_t chains, dyn_bones; double dyn_ms; uint32_t dormant; } IkPhysStats; // (Q10: the dynamic bones'; Q27.5: the active ragdolls past the budget)
const IkPhysStats* ikphys_stats(void);
// The self-test (--ik-test): a chain reaches its goal within its limits; a ragdoll falls, rests on the ground, its
// joints hold. 0 = ok
int ikphys_selftest(void);
// (Q10) one self-test by name (--ik-test <name>): ragdoll_limits, ragdoll_active, dyn_bones; 0 = ok
int ikphys_selftest_named(const char* name);
