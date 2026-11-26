// Character definition (plan item AM1, "charmap"): what each bone of a rig IS, whatever the rig calls it. A map gives
// the bones of an armature a role (hips, spine, upper arm, shin...) and names generic chains (tail, wings, tentacles,
// extra legs), so the tools that work on "a character" (pose by dragging, retargeting, capture, the export to the
// Real Engine, generated motion) work on any rig and not only on Tlender's auto-rig.
//
// The map lives on the Armature (Armature.charmap, NULL until one is made), is saved with it in the .tlend (optional
// lines "charmap", "cmr", "cmc") and can be written to and read from a .tlmap file (JSON) to reuse on rigs with the
// same names. It is plain data: nothing in the evaluation of an armature reads it, so a rig with a map animates
// exactly as one without (rule 15.0 of the plan: the pose and motion tools are optional).
//
// charmap_analyze.c fills a map by itself (names, the auto-rig's tags, topology and geometry, a VRM's humanoid map).
// Pure (no ImGui, no Scene).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rig_armature.h" // (the engine: the part of the Tlender's armature these read)

// The roles of a side, in the order of the left block; the right block repeats it (charrole_mirror).
#define TL_CHAR_SIDE_ROLES(X)                              \
    X(CLAVICLE, "clavicle", "Clavicle")                   \
    X(UPPER_ARM, "upper_arm", "Arm")                     \
    X(FOREARM, "forearm", "Forearm")                     \
    X(HAND, "hand", "Hand")                                 \
    X(THUMB1, "thumb1", "Thumb 1")                       \
    X(THUMB2, "thumb2", "Thumb 2")                       \
    X(THUMB3, "thumb3", "Thumb 3")                       \
    X(INDEX1, "index1", "Index 1")                     \
    X(INDEX2, "index2", "Index 2")                     \
    X(INDEX3, "index3", "Index 3")                     \
    X(MIDDLE1, "middle1", "Middle 1")                       \
    X(MIDDLE2, "middle2", "Middle 2")                       \
    X(MIDDLE3, "middle3", "Middle 3")                       \
    X(RING1, "ring1", "Ring 1")                          \
    X(RING2, "ring2", "Ring 2")                          \
    X(RING3, "ring3", "Ring 3")                          \
    X(PINKY1, "pinky1", "Pinky 1")                        \
    X(PINKY2, "pinky2", "Pinky 2")                        \
    X(PINKY3, "pinky3", "Pinky 3")                        \
    X(THIGH, "thigh", "Thigh")                              \
    X(SHIN, "shin", "Shin")                              \
    X(FOOT, "foot", "Foot")                                  \
    X(TOE, "toe", "Toe")                           \
    X(UPPER_ARM_TWIST, "upper_arm_twist", "Upper Arm Twist") \
    X(FOREARM_TWIST, "forearm_twist", "Forearm Twist") \
    X(THIGH_TWIST, "thigh_twist", "Thigh Twist")        \
    X(SHIN_TWIST, "shin_twist", "Shin Twist")

typedef enum CharRole {
    CR_NONE = 0,
    CR_ROOT, CR_HIPS, CR_SPINE1, CR_SPINE2, CR_SPINE3, CR_SPINE4, CR_CHEST, CR_NECK1, CR_NECK2, CR_HEAD, CR_JAW,
    CR_EYE_L, CR_EYE_R,
#define TL_CHAR_ENUM_L(id, code, label) CR_##id##_L,
    TL_CHAR_SIDE_ROLES(TL_CHAR_ENUM_L)
#undef TL_CHAR_ENUM_L
#define TL_CHAR_ENUM_R(id, code, label) CR_##id##_R,
    TL_CHAR_SIDE_ROLES(TL_CHAR_ENUM_R)
#undef TL_CHAR_ENUM_R
    CR_COUNT
} CharRole;
#define CR_SIDE_FIRST CR_CLAVICLE_L
#define CR_SIDE_COUNT (CR_CLAVICLE_R - CR_CLAVICLE_L)

// The stable name of a role in files ("hips", "upper_arm.L"), the role of a name (CR_NONE when unknown).
const char* charrole_code(int role);
int charrole_from_code(const char* code);
// Its name on screen in the interface's language ("Braço (E)"), into out.
void charrole_label(int role, char* out, size_t cap);
// The msgid of its name without the side ("Braço").
const char* charrole_label_base(int role);
int charrole_side(int role);    // 0 center, 1 left, 2 right
int charrole_mirror(int role);  // the same role of the other side (the role itself in the center)
int charrole_parent(int role);  // the role it hangs from in the body (CR_NONE for the root)
// A main role: what a humanoid must have (hips, a spine bone, head; per side upper arm, forearm, hand, thigh, shin
// and foot). The analyzer's "mapped without error" counts these.
bool charrole_main(int role);
bool charrole_is_finger(int role);
bool charrole_is_twist(int role);

typedef enum CharRigClass {
    CRC_UNKNOWN = 0,
    CRC_HUMANOID,    // every main role
    CRC_NEAR_HUMAN,  // a biped with something missing or extra (no hands, extra arms, other proportions)
    CRC_QUADRUPED,   // four legs and a horizontal spine
    CRC_NON_HUMAN,   // many legs, a snake, tentacles: only generic chains
    CRC_MECHANICAL,  // a mechanical rig (mech_rig.h)
    CRC_COUNT
} CharRigClass;
const char* charmap_class_name(int rig_class); // msgid
const char* charmap_class_code(int rig_class); // "humanoid"... in files
int charmap_class_from_code(const char* code);

// Where a role's bone came from.
typedef enum CharSource {
    CSRC_NONE = 0,
    CSRC_MANUAL,    // the user picked it
    CSRC_AUTORIG,   // the tags of a rig generated by Tlender's auto-rig
    CSRC_NAMES,     // a known naming convention
    CSRC_VRM,       // the humanoid map of a VRM file
    CSRC_TOPOLOGY,  // the shape of the skeleton (branches, symmetry, directions)
    CSRC_COUNT
} CharSource;
const char* charmap_source_name(int source); // msgid

typedef struct CharRoleMap {
    char bone[TL_BONE_NAME_LEN];    // the skeleton bone of the role ("": not mapped)
    char control[TL_BONE_NAME_LEN]; // in a control rig, the FK control that moves it ("": the bone is posed directly)
    float confidence;               // 0..1
    uint8_t source;                 // CharSource
    // The reference pose (pose_ref.h, plan item AM2): the orientation of the bone in the canonical T pose, armature
    // space (a quaternion w x y z); has_ref false until it is measured.
    bool has_ref;
    float ref_quat[4];
    // Joint limits and mass (joint_limits.h, plan item AM3): the range of the joint's flexion, abduction and twist
    // (radians, measured against the T pose) and the share of the body's mass the role carries, with the place of
    // its center along the bone (0 its joint, 1 the next joint).
    bool has_limit;
    float lim_min[3], lim_max[3];
    float mass, mass_center;
} CharRoleMap;

// Where the masses of a map came from (joint_limits.h).
typedef enum CharMassSource { CMS_NONE = 0, CMS_PRESET, CMS_MESH, CMS_COUNT } CharMassSource;

// The pose a rig rests in (pose_ref.h): by how far its arms point below the horizontal.
typedef enum CharRestPose { CRP_UNKNOWN = 0, CRP_T, CRP_A, CRP_OTHER, CRP_COUNT } CharRestPose;

typedef enum CharChainKind { CCK_GENERIC = 0, CCK_TAIL, CCK_LEG, CCK_WING, CCK_TENTACLE, CCK_EAR, CCK_ARM, CCK_COUNT } CharChainKind;
const char* charmap_chain_kind_name(int kind); // msgid
const char* charmap_chain_kind_code(int kind);
int charmap_chain_kind_from_code(const char* code);

#define TL_CHARMAP_CHAINS 32
#define TL_CHARMAP_CHAIN_NAME 32
// A generic chain: the bones from `first` down the hierarchy to `last` (first is an ancestor of last, or the same).
typedef struct CharChain {
    char name[TL_CHARMAP_CHAIN_NAME]; // "tail", "leg_1", "tentacle_3"
    uint8_t kind;                     // CharChainKind
    uint8_t side;                     // 0 center, 1 left, 2 right
    char first[TL_BONE_NAME_LEN], last[TL_BONE_NAME_LEN];
} CharChain;

typedef struct CharMap {
    uint8_t rig_class;   // CharRigClass
    uint8_t source;      // CharSource of the map as a whole (how most of it was found)
    char convention[32]; // the naming convention recognized ("tlender", "rigify", "mixamo", "ue", "vroid"...; "": none)
    CharRoleMap roles[CR_COUNT];
    CharChain chains[TL_CHARMAP_CHAINS];
    int chain_count;
    // The reference pose (pose_ref.h): the rest pose detected and the angle of each upper arm (left, right) below the
    // horizontal, in degrees.
    uint8_t rest_pose;   // CharRestPose
    float rest_arm_angle[2];
    uint8_t mass_source; // CharMassSource
} CharMap;

// --- The switch of the pose and motion tools (Preferences > Animation > Pose and motion) ---------------------------------
// Off (the default): no panel, button or menu of them shows and nothing of them runs by itself (an import does not
// make a map). The program sets it from the preferences; the tests turn it on for what they test.
void posemotion_set_enabled(bool on);
bool posemotion_enabled(void);

// --- Maps --------------------------------------------------------------------------------------------------------------
CharMap* charmap_new(void);                  // empty; NULL without memory
void charmap_free(CharMap* m);               // NULL is fine
CharMap* charmap_copy(const CharMap* m);     // NULL for NULL or without memory
void charmap_clear(CharMap* m);
// The armature's map, made when missing (NULL without memory); armature_free and armature_copy take care of it.
CharMap* armature_charmap_ensure(Armature* a);
void armature_charmap_remove(Armature* a);

// Gives a role its bone (and control); a bone has one role only, so the role that had it loses it. bone "" clears.
void charmap_set_role(CharMap* m, int role, const char* bone, const char* control, float confidence, int source);
int charmap_role_of_bone(const CharMap* m, const char* bone); // by the skeleton bone or by the control; CR_NONE
int charmap_bone(const Armature* a, const CharMap* m, int role);    // the role's bone index in the armature, -1
int charmap_control(const Armature* a, const CharMap* m, int role); // the bone that is posed: the control, else the bone
int charmap_mapped_count(const CharMap* m);
// The main roles without a bone; returns how many (at most cap written). out may be NULL.
int charmap_missing_main(const CharMap* m, int* out, int cap);
int charmap_main_total(void);
float charmap_confidence(const CharMap* m); // the mean over the mapped main roles (0 without any)
// The nearest mapped role up the body from a role (CR_NONE: none).
int charmap_parent_role(const CharMap* m, int role);
// The class from what is mapped: humanoid with every main role, near human with the legs and a spine but something
// missing; keeps quadruped / non human / mechanical when the analyzer said so.
void charmap_update_class(CharMap* m);

// Chains: a new one (its name made unique; -1 when full), removing, the bones of one in order (first to last).
int charmap_chain_add(CharMap* m, const char* name, int kind, int side, const char* first, const char* last);
void charmap_chain_remove(CharMap* m, int chain);
int charmap_chain_bones(const Armature* a, const CharChain* c, int* out, int cap);

// Keeping the map true to the armature: a renamed bone, and dropping what names bones that no longer exist.
void charmap_bone_renamed(CharMap* m, const char* from, const char* to);
int charmap_validate(CharMap* m, const Armature* a); // returns how many roles and chains were dropped

// --- Files ---------------------------------------------------------------------------------------------------------------
// The .tlmap (JSON): the roles and chains by bone name. Loading replaces the map; bones the armature does not have are
// kept in the map (charmap_validate drops them). err says why on false.
char* charmap_to_json(const CharMap* m, size_t* len); // malloc'ed
bool charmap_from_json(CharMap* m, const char* text, size_t len, char* err, size_t err_cap);
bool charmap_save_file(const CharMap* m, const char* path, char* err, size_t err_cap);
bool charmap_load_file(CharMap* m, const char* path, char* err, size_t err_cap);
// The .tlend's lines of an armature's map (fileio.c): "charmap <lines> <class> <source> <convention>", then that many
// lines: a "cmr" per mapped role (a "cmk" after it when it has a control, a "cmq" when it has a reference
// orientation, a "cml" when it has joint limits, a "cmm" when it has mass), a "cmc" per chain, a "cmp" with the rest
// pose and a "cmw" with where the masses came from. A line of a kind this version does not know (a
// newer file's) is skipped.
void charmap_write_lines(FILE* f, const CharMap* m);
// The first line says how many lines follow; each of them is handed to charmap_parse_line.
bool charmap_parse_header(CharMap* m, const char* line, int* lines);
bool charmap_parse_line(CharMap* m, const char* line);

// --- The analyzer (charmap_analyze.c) -----------------------------------------------------------------------------------
typedef struct CharAnalysis {
    int rig_class;        // CharRigClass
    int source;           // CharSource: how most roles were found
    char convention[32];
    int mapped;           // roles with a bone
    int main_mapped, main_total;
    int chains;
    int deform_bones, control_bones; // how the bones of the armature split (a control rig has both)
    float confidence;
} CharAnalysis;
// Fills out from the armature's rest pose: the auto-rig's tags when it is a generated rig, else the naming convention
// that fits most bones, then the shape of the skeleton for what the names did not give. Roles the user picked
// (CSRC_MANUAL) in `keep` stay as they are (keep may be NULL or out itself). False when the armature has no bones.
bool charmap_analyze(const Armature* a, const CharMap* keep, CharMap* out, CharAnalysis* rep);
// The same with what deforms given (per bone, 1: the skin uses it): a file that marks every bone as deforming (an FBX
// exported with its controls) but skins only some of them. skin NULL: Bone.deform.
bool charmap_analyze_ex(const Armature* a, const CharMap* keep, const unsigned char* skin, CharMap* out, CharAnalysis* rep);
// A VRM's humanoid map: the VRM bone name ("hips", "leftUpperArm"...) to a role, CR_NONE when the name is not one.
int charmap_role_from_vrm(const char* vrm_bone);
// A map made before its rig's system was known (an older analysis, or a different rig system now): Analyze Rig would
// find another convention. The roles picked by hand count as up to date.
bool charmap_outdated(const Armature* a, const CharMap* m);
// The names of the conventions the analyzer knows, for the report ("tlender", "rigify", "mixamo"...); returns how many.
int charmap_conventions(const char** out, int cap);

// --- The class of the rig and the report (plan item AM10: the analyzer v1) ---------------------------------------------------
// Why a rig got its class.
typedef enum CharClassReason {
    CCR_NONE = 0,
    CCR_ALL_MAIN,        // humanoid: every main role, human proportions, no extra limbs
    CCR_MISSING_MAIN,    // near human: a biped with main roles missing (no hands, no head...)
    CCR_EXTRA_LIMBS,     // near human: a biped with more limbs than a person (arms, legs, a tail, wings)
    CCR_PROPORTIONS,     // near human: a biped whose arms and legs are far from a person's proportions
    CCR_FOUR_LEGS,       // quadruped: two pairs of legs under a horizontal spine
    CCR_MANY_LEGS,       // non human: three or more pairs of legs
    CCR_ONLY_CHAINS,     // non human: no body, only chains (a snake, tentacles, a tail)
    CCR_MECH_NAMES,      // mechanical: the bones are parts of a machine (piston, gear, chain, cable...)
    CCR_NOTHING,         // unknown: nothing recognized
    CCR_COUNT
} CharClassReason;
const char* charmap_class_reason_name(int reason); // msgid, a sentence
// What the class means for generated motion (the plan's E.3): direct, with adjustments or procedural. msgid.
const char* charmap_class_path(int rig_class);

typedef struct CharClassInfo {
    int rig_class, reason;
    bool biped_body;        // hips, a spine and the two legs
    bool spine_horizontal;
    bool mech_names;
    int leg_pairs;          // mirrored pairs of limbs that reach the ground
    int extra_limbs;        // chains of arms, legs, wings, tentacles and tails beside the body's own limbs
    float arm_leg_ratio;    // arm length / leg length (0: not measured)
    float leg_height_ratio; // leg length / the skeleton's height (0: not measured)
} CharClassInfo;
// The class of the armature's map from its bones, its roles and its chains (the map's rig_class is set). The analyzer
// ends with it; call it again after the map is edited by hand. info may be NULL.
int charmap_classify(const Armature* a, CharMap* m, CharClassInfo* info);

// The report: what is found, what is missing and why, what is in doubt. Shown with Check Rig.
typedef enum CharCheckKind {
    CCHK_MISSING_MAIN = 0, // a main role without a bone (why: CharCheckWhy)
    CCHK_BONE_GONE,        // a role or a chain names a bone the armature no longer has (fix: dropped from the map)
    CCHK_LOW_CONFIDENCE,   // a role found with little certainty (below 60%)
    CCHK_ONE_SIDED,        // a role mapped on one side only
    CCHK_REST_POSE,        // the rig rests in neither a T nor an A pose
    CCHK_NO_FINGERS,       // hands without fingers in the map
    CCHK_EXTRA_LIMB,       // a limb beside the body's (a chain)
    CCHK_NO_LIMITS,        // no joint limits in the map (the solver bends any way)
    CCHK_COUNT
} CharCheckKind;
typedef enum CharCheckWhy {
    CCW_NONE = 0,
    CCW_NO_PARENT,     // the role above it in the body has no bone either
    CCW_NO_CANDIDATE,  // the bone of the role above has no free bone below it
    CCW_AMBIGUOUS,     // more than one free bone below the role above: pick one in the correspondence screen
    CCW_COUNT
} CharCheckWhy;
typedef struct CharCheckItem {
    int kind;   // CharCheckKind
    int role;   // CR_NONE when the item is not about a role
    int chain;  // -1
    int why;    // CharCheckWhy
    int bone;   // the bone to select (the role's, or the bone of the role above a missing one), -1
    bool error; // a missing main role or a bone that is gone; the rest are warnings and notes
    bool fixable;
} CharCheckItem;
const char* charmap_check_kind_name(int kind); // msgid
const char* charmap_check_why_name(int why);   // msgid
// The items of the armature's map, errors first. Returns how many (at most cap; out may be NULL to count).
int charmap_check(const Armature* a, const CharMap* m, CharCheckItem* out, int cap);
// One line for an item in the interface's language.
void charmap_check_text(const Armature* a, const CharMap* m, const CharCheckItem* it, char* out, size_t cap);

// --- Editing a map by hand (plan item AM7: the correspondence screen; charmap_edit.c) -------------------------------------------
// Gives a role a bone picked by the user (confidence 1, CSRC_MANUAL; in a control rig its FK control is found too);
// bone -1 clears the role. The role's limits and mass stay. The reference pose and the class are brought up to date.
// false when nothing changed.
bool charmap_assign(CharMap* m, const Armature* a, int role, int bone);
// The bone on the other side of a bone: by its name (".L" / ".R", "Left" / "Right"...), else the bone whose head and
// tail are this one's mirrored in X. -1: none (a bone of the middle).
int charmap_mirror_bone(const Armature* a, int bone);
// Every role of a side (1 left, 2 right) gives the other side the mirrored bone. Returns how many roles changed.
int charmap_mirror_side(CharMap* m, const Armature* a, int from_side);
// The guide of the correspondence screen: the user picks these bones in order (hips, chest, head, the left upper arm,
// hand, thigh and foot); charmap_fill_from_picks then gives the bones between them their roles (spine, neck, clavicle,
// forearm, shin and the twist bones, toe) and the right side by the mirror. Returns how many roles it filled.
#define CHARMAP_GUIDE_COUNT 7
extern const int CHARMAP_GUIDE_ROLES[CHARMAP_GUIDE_COUNT];
int charmap_fill_from_picks(CharMap* m, const Armature* a);
// The FK control of a skeleton bone in a control rig ("": the bone is posed directly). charmap_analyze.c.
void charmap_find_control(const Armature* a, int bone, char* out, size_t cap);
// How sure a role is, for the screen's colors: 0 no bone (red), 1 in doubt (yellow, below 80%), 2 sure (green).
int charmap_confidence_level(const CharMap* m, int role);
// Where a role's dot sits on a silhouette, 0..1 in a square, y down. The person is seen from the front (its left on
// the right of the screen), the quadruped from its left side.
typedef enum CharSilhouette { CSIL_HUMAN = 0, CSIL_QUADRUPED, CSIL_COUNT } CharSilhouette;
bool charmap_silhouette_place(int silhouette, int role, float* x, float* y);
int charmap_silhouette_for(const CharMap* m); // the quadruped's for a quadruped, else the person's
// Maps remembered for rigs with the same bone names: a .tlmap in `dir` named after the signature of the armature's
// bone names (the same set of names, in any order, gives the same number).
uint64_t charmap_names_signature(const Armature* a);
bool charmap_remember_save(const char* dir, const Armature* a, const CharMap* m, char* err, size_t err_cap);
bool charmap_remember_load(const char* dir, const Armature* a, CharMap* m); // false: none remembered for these names
bool charmap_remember_forget(const char* dir, const Armature* a);

