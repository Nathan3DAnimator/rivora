// The VR base hand (vr_hand.c, notas-dev/xr.md "Mão base", 2026-11-20): a hand made by the engine for a modeler and a
// rigger to start from, and the solver that moves any hand rigged on its bones.
//  - The base hand: a skinned mesh (palm, fingers, thumb; ~3k triangles), a skeleton of OpenXR's 26 hand joints (their
//    names + "_L"/"_R": Wrist_R, IndexProximal_R...; the Tips are the fingertips' ends) and 7 poses as animations
//    (Open, Relaxed, Fist, Grip, Point, Pinch, ThumbsUp: from Open at frame 0 to the pose at frame 10, 30 fps).
//    Project Settings > VR > Export Base Hand (FBX) writes both hands and a read-me; Use the Base Hand imports them into
//    the project (/Content/VR/Hands) and sets them as the VR hands.
//  - The solver: a Skeletal Mesh set as a VR hand has its bones found by name (OpenXR's joint names anywhere in the
//    bone's, case and separators ignored; "Pinky" for Little). Tracked hands: each bone turned toward the next joint the
//    headset tracks (the hand placed by the wrist and the knuckles). Controllers: each finger curled by the trigger
//    (index), the grip (the rest) and the buttons (thumb) - between the rig's own "Open" and "Fist" animations when it
//    has them, else around the axes its bones give (the palm's normal and the finger's direction)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim.h"

#define VRH_JOINTS 26 // OpenXR's order: palm, wrist, thumb (metacarpal, proximal, distal, tip), index..little (metacarpal, proximal, intermediate, distal, tip)
enum { VRH_PALM = 0, VRH_WRIST = 1, VRH_THUMB_META = 2, VRH_INDEX_META = 6, VRH_MIDDLE_META = 11, VRH_RING_META = 16, VRH_LITTLE_META = 21 };
extern const char* const g_vrh_joint_names[VRH_JOINTS]; // "Palm", "Wrist", "ThumbMetacarpal", ...
enum { VRH_POSE_OPEN = 0, VRH_POSE_RELAXED, VRH_POSE_FIST, VRH_POSE_GRIP, VRH_POSE_POINT, VRH_POSE_PINCH, VRH_POSE_THUMBS_UP, VRH_POSES };
extern const char* const g_vrh_pose_names[VRH_POSES]; // the animations' names in the FBX

// The OpenXR joint a bone's name stands for (-1: none)
int vrh_joint_of_name(const char* bone_name);
// The base hand's FBX (side 0 left, 1 right) / both hands and the read-me (pt and en) into a folder; false with the reason
bool vrh_export_fbx(int side, const char* path, char* err, size_t cap);
bool vrh_export_folder(const char* dir, char* err, size_t cap);
// The files' names in that folder ("RealEngine_BaseHand_L.fbx"...)
const char* vrh_file_name(int side);

// A Skeletal Mesh's bones read as a hand (made once per mesh, then only read)
typedef struct VrHandRig {
    bool ok, tracked_ok;            // its fingers found / the wrist and three knuckles found (tracked hands)
    int side;                       // 0 left, 1 right
    uint32_t nbones;
    int16_t bone_of[VRH_JOINTS];    // joint -> bone (-1)
    int8_t joint_of[ANIM_MAX_BONES];
    int8_t finger_of[ANIM_MAX_BONES]; // 0 thumb .. 4 little (-1: the wrist, the palm, unnamed)
    int16_t aim_child[ANIM_MAX_BONES]; // the next named bone along its finger (-1)
    float axis[ANIM_MAX_BONES][3];  // the curl's axis in the bone's own frame (procedural)
    float fist_deg[ANIM_MAX_BONES]; // its angle in a fist (procedural)
    float qm[ANIM_MAX_BONES][4];    // the bind's rotations and places in the mesh's space
    float pm[ANIM_MAX_BONES][3];
    const AnimSeq *open, *fist;     // the rig's own Open / Fist animations (NULL: the procedural curl)
} VrHandRig;
bool vrh_rig_build(VrHandRig* r, const SkelMesh* m, int side);
// Controllers: the local pose with each finger curled (0 straight .. 1 the fist): thumb, index, middle, ring, little
void vrh_pose_curl(const VrHandRig* r, const SkelMesh* m, const float curl[5], AnimXf* out);
// Tracked: the joints (world, cm) -> the actor's rows (rotation rows: rot[k] = the mesh's axis k in the world; place;
// the scale applied by the caller) and the local pose. False: the rig lacks the wrist or the knuckles
bool vrh_pose_tracked(const VrHandRig* r, const SkelMesh* m, const float joints[VRH_JOINTS][3], float scale, float rot[3][3],
                      float pos[3], AnimXf* out);
// A rig's pose clip by the pose's name anywhere in the animation's ("A_Fist_R", "Fist"; case ignored; its skeleton the
// mesh's) and the time of its last frame (the pose)
const AnimSeq* vrh_find_clip(const SkelMesh* m, const char* pose_name);
float vrh_clip_end(const AnimSeq* s);
// The base hand's own joints at a pose (world = the hand's FBX space converted to the engine's: cm, the wrist at 0),
// for the tests: what a tracked hand in that pose would report
void vrh_base_joints(int side, int pose, float out[VRH_JOINTS][3]);
