// The rig systems the pose tools know (pose by dragging, character definition, retarget): where a rig came from and
// how it is driven. A character made in another program arrives with that program's skeleton names and, for a control
// rig, its own controls, IK/FK switches and IK handles; this table says, per system, how to find them:
//
//   Tlender's auto-rig, Rigify (Blender), Auto-Rig Pro (Blender), AdvancedSkeleton (Maya), Mixamo and HumanIK
//   (Maya / MotionBuilder, also Rokoko, Xsens and other capture tools), Unreal's mannequin and MetaHuman, VRoid / VRM,
//   Character Creator / iClone / AccuRig, Daz Genesis, 3ds Max Biped and CAT, SMPL, Valve / Source, MakeHuman / MPFB.
//
// A plain skeleton (Mixamo, UE, VRoid...) is posed on its own bones. A control rig (Rigify, Auto-Rig Pro,
// AdvancedSkeleton, Tlender) is posed through its FK controls, moves the body by its body control and, where a limb is
// in IK, through its IK handle and pole. A rig no table knows is detected as a control rig when its deforming bones
// are driven by constraints: rigsys_constraint_control follows the constraints to the bone an animator would pose.
//
// Detection looks at marker bones (names only a given system makes). The names are the systems' public naming,
// written here from scratch. Pure: no Scene, no ImGui.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "rig_armature.h" // (the engine: the part of the Tlender's armature these read)

typedef enum RigSystem {
    RIGSYS_NONE = 0,   // not recognized: names and shape only
    RIGSYS_TLENDER,    // Tlender's auto-rig
    RIGSYS_RIGIFY,
    RIGSYS_ARP,        // Auto-Rig Pro
    RIGSYS_ADVSKEL,    // AdvancedSkeleton
    RIGSYS_MIXAMO,     // Mixamo / HumanIK names
    RIGSYS_UE,         // Unreal mannequin / MetaHuman
    RIGSYS_VROID,
    RIGSYS_CC,         // Character Creator / iClone / AccuRig
    RIGSYS_DAZ,
    RIGSYS_BIPED,      // 3ds Max Biped
    RIGSYS_CAT,        // 3ds Max CAT
    RIGSYS_SMPL,
    RIGSYS_VALVE,
    RIGSYS_MPFB,       // MakeHuman / MPFB
    RIGSYS_CONTROL,    // an unknown control rig: deforming bones driven by constraints
    RIGSYS_COUNT
} RigSystem;

// "autorigpro"... (the code a CharMap keeps as its convention); "" for RIGSYS_NONE.
const char* rigsys_code(int sys);
int rigsys_from_code(const char* code); // RIGSYS_NONE when unknown
// The name on screen ("Auto-Rig Pro") and where it comes from (msgid: "add-on do Blender").
const char* rigsys_name(int sys);
const char* rigsys_origin(int sys);
// A control rig: the tools pose its controls, not its skeleton.
bool rigsys_is_control_rig(int sys);

// The system an armature was made with, from its marker bones (and the auto-rig's tags); RIGSYS_CONTROL for an
// unknown rig whose deforming bones constraints drive; RIGSYS_NONE otherwise.
int rigsys_detect(const Armature* a);

// The bone an animator turns to pose a skeleton bone (FK): the system's control for it, else the bone itself when
// nothing drives it, else what its constraints lead to (rigsys_constraint_control). -1 when out of reach.
int rigsys_fk_control(const Armature* a, int sys, int bone);
// Following the constraints of a driven bone (Copy Rotation / Transforms, Child Of...) to a bone nothing drives: the
// control (an FK one when the bone follows both an IK and an FK chain). -1 when none.
int rigsys_constraint_control(const Armature* a, int bone);
// The control that carries the whole body (moves the hips with everything above): -1 when none.
int rigsys_body_control(const Armature* a, int sys);

// The limbs: 0 left arm, 1 right arm, 2 left leg, 3 right leg.
typedef struct RigLimbSwitch {
    char bone[TL_BONE_NAME_LEN]; // the bone holding the IK/FK property ("" = the armature object)
    char prop[32];
    float fk, ik;                // the property's value in FK and in IK
} RigLimbSwitch;
// The limb's IK/FK switch in this system; false when it has none (or the rig lacks the bone).
bool rigsys_limb_switch(const Armature* a, int sys, int limb, RigLimbSwitch* out);
// The limb's IK handle (hand or foot control) and pole; -1 when none.
int rigsys_ik_control(const Armature* a, int sys, int limb);
int rigsys_pole_control(const Armature* a, int sys, int limb);
// The control at the top of the limb's IK chain, whose turn sets the side the elbow / knee bends to when the pole is
// off (Rigify's "thigh_ik" / "upper_arm_ik" with pole_vector 0, its default); -1 when the system has none.
int rigsys_ik_top_control(const Armature* a, int sys, int limb);
// A bone the tools may key in this system (its controls; the skeleton only when the pose is baked onto it).
bool rigsys_keyable(const Armature* a, int sys, int bone, bool baked);
