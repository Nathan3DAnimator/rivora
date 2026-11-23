// (2026-11-23, the user: "a rig detection system: every rig the Tlender is compatible with, the Real compatible too;
// it comes ready, knowing what is IK, what is FK and the hierarchy, the foot IK found by itself: only switch it on in
// the AnimGraph") What each bone of a Skeletal Mesh IS, whatever its rig calls it: the Tlender's character definition
// (charmap_analyze.c: names of the Tlender's auto-rig, Rigify, Auto-Rig Pro, AdvancedSkeleton, Mixamo / HumanIK, UE /
// MetaHuman, VRoid, Character Creator / AccuRig, Daz, Biped, CAT, SMPL, Valve, MPFB; then the skeleton's shape for an
// unknown rig) and its rig systems (rig_systems.c: the controls, the IK handles and poles), run on the mesh's bind
// pose when it loads (anim.c), kept on the SkelMesh (m->rig). Everything that names a bone the Unreal way ("foot_l",
// "spine_02", "upperarm_r": the AnimGraph's Leg IK, Foot IK, the Aim Offset chain, the styles, the physics) finds the
// role's bone on any rig through rig_bone_ue. notas-dev/ikrig.md "Detecção de rig".
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "anim.h"
#include "charmap.h"

// What a bone is for (RigInfo.kind)
enum { RIGB_DEFORM = 0, RIGB_CONTROL, RIGB_IK_HANDLE, RIGB_POLE, RIGB_MECH, RIGB_TWIST, RIGB_FINGER, RIGB_CHAIN, RIGB_HELPER, RIGB_KIND_COUNT };
// A dynamic chain's kind (rig chains with a body of their own: physics presets, ikrig.h DYN_*)
enum { RIGC_TAIL = 0, RIGC_EAR, RIGC_HAIR, RIGC_CHAIN, RIGC_ACCESSORY, RIGC_CAPE, RIGC_OTHER, RIGC_KIND_COUNT };

#define RIG_MAX_CHAINS 24
typedef struct RigChainInfo {
    int16_t first, last; // bones (first an ancestor of last)
    uint8_t kind, side;  // RIGC_*, 0 center 1 left 2 right
    char name[32];
} RigChainInfo;

typedef struct RigInfo {
    uint32_t mesh_hash;
    int rig_class;       // CharRigClass (humanoid, near human, quadruped, non human, mechanical)
    int system;          // RigSystem (rig_systems.h)
    char convention[32]; // the naming convention ("mixamo", "rigify", "ue"...; "": the skeleton's shape)
    float confidence;    // 0..1 (the main roles' mean)
    int main_mapped, main_total, deform_bones, control_bones;
    int16_t role[CR_COUNT];     // a role's bone (-1)
    int16_t control[CR_COUNT];  // its FK control in a control rig (-1: the bone is posed itself)
    int16_t ik_handle[4], pole[4]; // the limbs' IK handle and pole (0 arm L, 1 arm R, 2 leg L, 3 leg R; -1)
    uint8_t kind[ANIM_MAX_BONES];  // RIGB_*
    uint32_t nchains;
    RigChainInfo chains[RIG_MAX_CHAINS];
    float forward[3];    // the character's forward in the mesh's space (from the heels to the toes)
    double ms;           // the analysis' time
    bool had_skin;       // the skins' weights were loaded (what deforms known): else redone when they are
} RigInfo;

// The rig found when the mesh loaded (anim.c calls rig_detect on load and reload; NULL until then)
const RigInfo* rig_info(const SkelMesh* m);
// Analyzes the mesh's skeleton (its skins' weights tell what deforms when their vertices are loaded), m->rig made or
// redone. True: something recognized.
bool rig_detect(SkelMesh* m);
void rig_forget(SkelMesh* m);
// A bone by its Unreal name ("foot_l", "calf_r", "spine_03", "head", "ball_l", "upperarm_twist_01_l"...) on any rig:
// the exact name first, else the role's bone (a missing spine bone falls to its nearest). -1 none.
int16_t rig_bone_ue(const SkelMesh* m, const char* ue_name);
int rig_role_of_ue(const char* ue_name);       // CR_NONE when the name is no Unreal bone
int16_t rig_role_bone(const SkelMesh* m, int role); // -1
const char* rig_class_label(int rig_class);    // msgid ("Humanoid"...)
const char* rig_system_label(int system);      // the system's name ("Auto-Rig Pro"...; "Unknown")
const char* rig_kind_label(int kind);          // RIGB_* msgid
const char* rig_chain_kind_label(int kind);    // RIGC_* msgid
int rig_chain_preset(int chain_kind);          // the ikrig.h DYN_* preset that fits a chain
// The self test (--rig-test): Mixamo, Rigify (DEF + controls), Auto-Rig Pro, UE, CC, Daz, a rig with no names, a
// tail and ears; prints and returns the result
bool rig_detect_selftest(void);
