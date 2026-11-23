// Artificial intelligence (notas-dev/ia.md). Two systems that share everything below them:
//   * Unreal's: an AI agent runs a Behavior Tree (.rbt: Selectors, Sequences, Tasks, Decorators, Services) over a
//     Blackboard (its keys), fed by AI Perception (sight, hearing) and moving on the navigation grid.
//   * The Real Brain (.rbrain): utility AI, built from the best of today's games (The Sims' needs, Infinite Axis
//     Utility): each Behavior scores its Considerations (an input through a curve: distance, sight, time, noise, a
//     Blackboard value, a Blueprint variable...), the best one runs its Action (wander, chase, flee, keep distance,
//     investigate, patrol, attack, run a Behavior Tree, a Blueprint event). Every score is shown live in its editor.
// Both read and write the same Blackboard, so they talk: a Brain's action can run a Behavior Tree, a tree's task can
// call a Blueprint event, Blueprints read and set the keys (Get / Set Blackboard Value), Report Noise reaches both.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define AI_NAME 48
#define AI_MAX_KEYS 24
#define AI_MAX_NODES 64

// --- Blackboard ---
enum { BB_BOOL = 0, BB_FLOAT, BB_VECTOR, BB_ACTOR, BB_TYPE_COUNT };
typedef struct BbKey { char name[AI_NAME]; uint8_t type; } BbKey;

// --- Behavior Tree ---
enum {
    BT_ROOT = 0, BT_SELECTOR, BT_SEQUENCE,                                  // composites
    BT_MOVE_TO, BT_WAIT, BT_ROTATE_TO, BT_FIND_RANDOM, BT_SET_KEY, BT_CLEAR_KEY, BT_RUN_EVENT, BT_PLAY_MONTAGE, // tasks
    BT_PREDICT, BT_SURROUND, // where the target will be (intercept); its squad slot around the target
    BT_KIND_COUNT
};
enum { BTD_NONE = 0, BTD_BLACKBOARD, BTD_COOLDOWN, BTD_LOOP, BTD_TIME_LIMIT, BTD_FORCE_SUCCESS, BTD_KIND_COUNT }; // decorators
enum { BTA_NONE = 0, BTA_SELF, BTA_LOWER, BTA_BOTH }; // a Blackboard decorator's Observer Aborts (Unreal's)
#define BT_MAX_DECOS 4
#define BT_MAX_SVCS 3
#define BT_DETACHED (-2) // a node not wired under anything (the editor keeps it; the tree ignores it)
typedef struct BtDeco {
    uint8_t kind, op, aborts;
    bool inverse;            // Inverse Condition
    char key[AI_NAME];
    float value;             // the compared value / cooldown s / loop count / time limit s
} BtDeco;
typedef struct BtService { uint8_t kind; char key[AI_NAME]; float value; } BtService;
enum { BTS_NONE = 0, BTS_FOCUS, BTS_SPEED, BTS_KIND_COUNT }; // a node's service: face a key / the movement speed (value 0-1)
enum { BB_IS_SET = 0, BB_IS_NOT_SET, BB_GREATER, BB_LESS, BB_EQUAL };
typedef struct BtNode {
    uint8_t kind;
    int16_t parent;          // -1: the root, BT_DETACHED: not wired
    float x, y;              // in the editor
    char key[AI_NAME];       // the Blackboard key it reads or writes (Move To, Rotate To, Find Random, Set / Clear)
    float value, value2;     // Wait: seconds, deviation; Move To: acceptance; Find Random: radius; Set Key: the value
    char text[AI_NAME];      // Run Event: the event's name; Play Montage: the montage
    uint8_t ndecos, nsvcs;   // Unreal's: the decorators over the node, the services under them
    BtDeco decos[BT_MAX_DECOS];
    BtService svcs[BT_MAX_SVCS];
    char comment[64];
} BtNode;
typedef struct BehaviorTree {
    char name[AI_NAME], path[512];
    uint32_t hash;
    BbKey keys[AI_MAX_KEYS];
    uint32_t nkeys, nnodes;
    BtNode nodes[AI_MAX_NODES]; // [0]: the root (its one child is the tree)
} BehaviorTree;

// --- The Real Brain ---
enum {
    BRAIN_IN_DISTANCE = 0, BRAIN_IN_SEES_TARGET, BRAIN_IN_TIME_SINCE_SEEN, BRAIN_IN_HEARD_NOISE, BRAIN_IN_KEY_FLOAT,
    BRAIN_IN_BP_VAR, BRAIN_IN_RANDOM, BRAIN_IN_TIME_IN_BEHAVIOR, BRAIN_IN_HAS_TARGET, BRAIN_IN_SQUAD_SIZE, BRAIN_IN_TARGET_SPEED,
    BRAIN_IN_COUNT
};
enum { CURVE_LINEAR = 0, CURVE_INVERSE, CURVE_STEP, CURVE_SMOOTH, CURVE_BELL, CURVE_COUNT };
enum {
    BRAIN_ACT_IDLE = 0, BRAIN_ACT_WANDER, BRAIN_ACT_CHASE, BRAIN_ACT_FLEE, BRAIN_ACT_KEEP_DISTANCE, BRAIN_ACT_INVESTIGATE,
    BRAIN_ACT_PATROL, BRAIN_ACT_ATTACK, BRAIN_ACT_RUN_TREE, BRAIN_ACT_EVENT, BRAIN_ACT_INTERCEPT, BRAIN_ACT_SURROUND, BRAIN_ACT_COUNT
};
typedef struct Consideration {
    uint8_t input, curve;
    char key[AI_NAME];       // KEY_FLOAT: the Blackboard key; BP_VAR: the pawn's Blueprint variable
    float lo, hi;            // the input's range (mapped to 0-1 before the curve)
    float param;             // STEP: the threshold; BELL: the width
    bool invert;
} Consideration;
typedef struct BrainBehavior {
    char name[AI_NAME];
    uint8_t action, ncons;
    float weight;            // multiplies its score (priority)
    float speed;             // 0-1: walk .. run
    float param, param2;     // wander / patrol radius, keep distance min / max, attack range / cooldown
    char text[AI_NAME];      // Run Tree: the tree; Event: the event's name
    Consideration cons[4];
} BrainBehavior;
typedef struct Brain {
    char name[AI_NAME], path[512];
    uint32_t hash;
    BbKey keys[AI_MAX_KEYS]; // its Blackboard (a tree it runs adds its own keys)
    uint32_t nkeys, nbehaviors;
    BrainBehavior behaviors[16];
    float momentum;          // the running behavior's bonus (0-1): no flickering between two close scores
    float think_interval;    // s between decisions
} Brain;

// --- Assets ---
void ai_init(const char* content_root, bool make_defaults); // the examples (BT_Chase, BB keys, Brain_Guard) when missing
BehaviorTree* ai_tree_load(const char* vpath);
Brain* ai_brain_load(const char* vpath);
BehaviorTree* ai_tree(uint32_t hash);
Brain* ai_brain(uint32_t hash);
BehaviorTree* ai_tree_name(const char* name);
uint32_t ai_tree_count(void);
BehaviorTree* ai_tree_at(uint32_t i);
uint32_t ai_brain_count(void);
Brain* ai_brain_at(uint32_t i);
bool ai_tree_save(BehaviorTree* t);
bool ai_brain_save(Brain* b);
BehaviorTree* ai_tree_create(const char* folder_vpath, const char* name);
Brain* ai_brain_create(const char* folder_vpath, const char* name);
extern const char* const g_bt_kind_names[BT_KIND_COUNT];
extern const char* const g_btd_names[BTD_KIND_COUNT];
extern const char* const g_bts_names[BTS_KIND_COUNT];
extern const char* const g_brain_input_names[BRAIN_IN_COUNT];
extern const char* const g_curve_names[CURVE_COUNT];
extern const char* const g_brain_action_names[BRAIN_ACT_COUNT];
extern const char* const g_bb_type_names[BB_TYPE_COUNT];
float ai_curve(uint8_t curve, float x, float param); // x 0-1 -> 0-1

// --- Navigation: the NavMesh (navmesh.h) once it is built; until then, and where it is not made (a Procedural World), a
// walkable grid built from the level's colliders (2.5D: one floor height per cell) ---
bool ai_nav_build(World* w);                 // the grid: play start (and the editor's Build Paths); false: nothing walkable
bool ai_nav_path(const float from[3], const float to[3], float (*out)[3], uint32_t cap, uint32_t* n); // smoothed
bool ai_nav_path_ex(const float from[3], const float to[3], float (*out)[3], uint8_t* flags, uint32_t cap, uint32_t* n); // + NAV_PT_ per point
bool ai_nav_random_point(const float around[3], float radius, float out[3]);
bool ai_nav_project(const float p[3], const float extent[3], float out[3]); // the nearest walkable point within extent
float ai_nav_path_length(const float from[3], const float to[3]);            // along the path (< 0: none)
typedef struct NavInfo { float origin[2], cell; uint32_t w, h, walkable; double build_ms; const float* height; } NavInfo;
const NavInfo* ai_nav_info(void);            // (height: w x h, < -1e29: not walkable)

// --- Run time ---
void ai_begin_play(World* w);
void ai_end_play(World* w);
void ai_update(World* w, float dt);          // after the Blueprints, before the movement (it sets the pawns' input)
void ai_report_noise(World* w, const float at[3], float loudness, Entity instigator);
// The damage sense (Unreal's AISense_Damage; Q24): an agent hit knows who hit it at once, from anywhere (health.c calls it
// from Apply Damage): the instigator becomes its target, its place the last known one and a noise there, it turns to it
void ai_report_damage(World* w, Entity victim, Entity instigator, float amount);
// Local avoidance between pawns (Unreal's RVO, Q24): on by default; REALENGINE_AI_AVOID=0 turns it off (to compare)
void ai_set_avoidance(bool on);
bool ai_avoidance(void);
bool ai_bb_get(World* w, Entity pawn, const char* key, float out[4]);
bool ai_bb_set(World* w, Entity pawn, const char* key, const float v[4]);
bool ai_run_tree(World* w, Entity pawn, const char* tree);
bool ai_move_to(World* w, Entity pawn, const float at[3], float accept);
// The editors' live view (scores, the running node) and the overlay (paths, sight)
typedef struct AiDebug {
    Entity pawn;
    const char* what;        // the running behavior / tree node
    float scores[16];        // the Brain's last scores (per behavior)
    uint32_t nscores, active;
    const float (*path)[3];
    uint32_t npath;
    float sight_radius, sight_angle, yaw;
    bool sees;
    bool has_target, flanks;  // a target; going round it (its squad's flank) rather than straight at it
    float predicted[3], flank[3]; // where the target will be; its slot around it
    uint32_t squad;           // the team's agents after the same target
} AiDebug;
uint32_t ai_debug(AiDebug* out, uint32_t cap);
typedef struct AiStats {
    uint32_t agents, paths_this_frame;
    uint32_t avoiding;       // agents that steered round another this frame (the local avoidance)
    uint32_t damage_sensed;  // hits sensed since the Play began (the damage sense)
    double ms;
} AiStats;
const AiStats* ai_stats(void);
