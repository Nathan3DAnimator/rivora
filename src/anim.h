// Animation (notas-dev/animacao.md): Unreal's Skeletal Mesh, Animation Sequence, Anim Montage and Animation Blueprint.
//   .rskm      Skeletal Mesh: its skeleton (bones, bind pose) and its parts (a mesh held by a bone: segmented skinning,
//              so every part is an ordinary instance: culled, LODs, ray traced shadows and GI, thousands of characters)
//   .ranim     Animation Sequence: a track per bone (rotation; translation for the root and the pelvis) sampled at a
//              fixed rate, notifies (events at a time)
//   .rmontage  Anim Montage: an animation played over the Anim Graph in a slot (the whole body or from a bone up),
//              sections, blend in / out
// An Animation Blueprint is a Blueprint Class whose parent is "Anim Instance": its Event Graph (the VM) computes
// variables; its Anim Graph (AnimGraph below) is a state machine whose states play sequences or 1D blend spaces, with
// transitions on those variables, then the montage slot.
// Its skins (.rskm v2: meshes weighted per vertex to up to 4 bones, RMESH_SKINNED) are deformed on the GPU each frame
// (render.c "skin"): the Tlender's, glTF's and FBX's characters bend smoothly where the parts were cut by bone.
// At run time an actor with CSkeletalMesh gets child entities: one per part (CAttach kind ATTACH_SK_PART), one per skin
// (ATTACH_SK_SKIN: at the actor, the bones' palette from anim_skin_palette) and, with an
// Anim Blueprint, its instance (CAttach kind ATTACH_ANIM_BP, the VM's). anim_update samples the poses in parallel
// (jobs.h), slower for what the camera doesn't see.
#pragma once
struct AnimGraph; // (declared before their first use in a prototype: clang gives a tag first seen there its own scope)
struct BpAsset;
struct JsonWriter;
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define ANIM_NAME 48
#define ANIM_MAX_BONES 128
#define ANIM_MAX_PARTS 256
#define ANIM_MAX_SKINS 32
// Morph targets (blend shapes, render.h RMESH_MORPHS): the Skeletal Mesh's list (its skins' targets by name), each
// actor's weights in CSkeletalMesh.morph_weight by this list's index
#define ANIM_MAX_MORPHS 64
#define ANIM_MORPH_NAME 48

typedef struct AnimXf {
    float t[3];  // cm
    float q[4];  // x y z w
    float s[3];
} AnimXf;

typedef struct AnimBone {
    char name[ANIM_NAME];
    int16_t parent; // an earlier bone, -1 for the root
    AnimXf bind;    // relative to the parent
} AnimBone;

typedef struct SkPart {
    int16_t bone;
    uint32_t mesh;      // a BasicShape or an imported mesh's id (render.h)
    AnimXf local;       // relative to the bone (scale: the shape's, 1 = 100 cm)
    float color[3], roughness, metallic;
    uint32_t material;  // a material's hash (0: the color)
} SkPart;

// A skin: a mesh in the Skeletal Mesh's space at its bind pose, weighted per vertex to its bones (RMESH_SKINNED)
typedef struct SkSkin {
    uint32_t mesh;      // an imported mesh's id (render.h)
    float color[3], roughness, metallic;
    uint32_t material;  // a material's hash (0: the color)
} SkSkin;

// (Q7, 2026-11-17) Unreal's sockets: a named place on a bone (an offset in its space) where things attach: a weapon in
// the hand, a hat, a muzzle. Made, renamed and moved in the Skeleton tab of the Skeletal Mesh Editor (its gizmo); a
// Static Mesh may be shown on it there as a preview only. Saved in the .rskm ("sockets"); anim_bone_entity takes a
// socket's name as its bone's
#define ANIM_MAX_SOCKETS 32
typedef struct AnimSocket {
    char name[ANIM_NAME];
    int16_t bone;
    uint8_t has_preview, pad;
    AnimXf local;          // in the bone's space (cm, its scale 1)
    uint32_t preview_mesh; // a Static Mesh shown on it in the editors (when has_preview: the cube is id 0)
} AnimSocket;

typedef struct SkelMesh {
    char name[ANIM_NAME], path[512];
    uint32_t hash;      // of its vpath
    uint32_t nbones, nparts;
    AnimBone bones[ANIM_MAX_BONES];
    SkPart parts[ANIM_MAX_PARTS];
    float bind_world[ANIM_MAX_BONES][3][4]; // (made at load)
    float bind_inv[ANIM_MAX_BONES][3][4];   // their inverses (the skins' palette: pose x bind inverse)
    uint32_t nskins;
    SkSkin skins[ANIM_MAX_SKINS];
    bool dirty;
    uint32_t nmorphs;                                    // (.rskm "morphs": name and default weight)
    char morph_name[ANIM_MAX_MORPHS][ANIM_MORPH_NAME];
    float morph_default[ANIM_MAX_MORPHS];
    char source[512]; // (.rskm "source") the file it was imported from (Reimport), empty: unknown or made by the engine
    bool rigid;       // (.rskm "rigid") parts by bone on purpose (the auto rig, the engine's own): no smooth skin to miss
    uint32_t nsockets; // (Q7) its sockets
    AnimSocket sockets[ANIM_MAX_SOCKETS];
    void* rig; // (2026-11-23, rig_detect.h) what each bone is (RigInfo, made when it loads; not saved)
} SkelMesh;
// (Q7) a socket by name (case ignored; -1: none); a new one on a bone ("<bone>Socket", made unique; NULL: full); its
// place at a pose (bones: the pose's rows, the mesh's space or the actor's)
int anim_socket_index(const SkelMesh* m, const char* name);
AnimSocket* anim_socket_add(SkelMesh* m, int bone);
// (2026-11-23, anim_meshsock.inc) A Static Mesh's sockets (its own space; bone 0), in ".<file>.sockets" beside it: read
// (cached; reload: its file again), the count (out: up to cap of them); saved (none: its file removed)
uint32_t anim_mesh_sockets(const char* mesh_vpath, bool reload, AnimSocket* out, uint32_t cap);
bool anim_mesh_sockets_save(const char* mesh_vpath, const AnimSocket* s, uint32_t n);
void anim_socket_rows(const SkelMesh* m, const AnimSocket* s, const float (*bones)[3][4], float out[3][4]);
// A transform under a parent's rows (a part's, a socket's): parent x (t, q, s)
void anim_xf_rows(const AnimXf* x, const float parent[3][4], float out[3][4]);
// Imported before the smooth skins (2026-11-07): rigid parts by bone cut from an imported mesh (a dress in strips, seams
// at the joints) where a reimport gives the skin weighted like the source; the editor warns and offers Reimport
bool anim_mesh_old_parts(const SkelMesh* m);
// The file read again into the loaded asset (after a reimport): the actors using it make their parts / skins again
bool anim_mesh_reload(const char* vpath);
// Every loaded Skeletal Mesh under that folder ("/Content/...") read again (an import into it finished); their count
uint32_t anim_reload_folder(const char* folder_vpath);
// The viewport's Realtime: while not playing, each Skeletal Mesh with an animation of its own plays it, looping
void anim_set_editor_realtime(bool on);
// The asset's "/Content/..." path (false: outside the project)
bool anim_mesh_vpath(const SkelMesh* m, char* out, size_t cap);
// A morph target's index in the Skeletal Mesh by name (case ignored; -1: none)
int anim_morph_index(const SkelMesh* m, const char* name);
// An actor's weight of morph i (its own when set, Set Morph Target; else the Skeletal Mesh's default)
struct CSkeletalMesh;
float anim_morph_weight(const SkelMesh* m, const struct CSkeletalMesh* c, uint32_t i);
// Unreal's Set Morph Target / Get Morph Target on the actor e's Skeletal Mesh (a Character's too): the weight by the
// target's name (0 none, 1 the whole shape; any value). Set false: no such target. The skin bends again on the GPU
// the next frame (render.c "skin"), nothing runs while the weights stay the same
bool anim_morph_set(World* w, Entity e, const char* name, float value);
float anim_morph_get(World* w, Entity e, const char* name);

typedef struct AnimNotify {
    char name[ANIM_NAME];
    float time;          // seconds
    // Unreal's Play Sound notify (audio.h): a sound of its own, played where the mesh is (its bone: bone; follow: it
    // moves with it), whatever the Anim Blueprint does; 0: none (only the event AnimNotify_<Name>)
    uint32_t sound;
    float volume, pitch;
    char bone[ANIM_NAME];
    uint8_t follow;
} AnimNotify;

typedef struct AnimSeq {
    char name[ANIM_NAME], path[512];
    uint32_t hash;
    char skeleton[ANIM_NAME]; // the Skeletal Mesh it was made for (its name)
    float length, fps;
    uint32_t nframes, ntracks;
    bool loop;
    uint16_t generator;  // made by the engine (its generation: an older one is made again); 0: imported / authored
    char track_bone[ANIM_MAX_BONES][ANIM_NAME];
    float* rot;          // nframes x ntracks x 4
    float* pos;          // nframes x ntracks x 3 (bind translation where it doesn't move)
    AnimNotify notifies[32];
    uint32_t nnotifies;
    bool dirty;
    const void* map_for;               // (the run's: the Skeletal Mesh map was made for)
    int16_t map[ANIM_MAX_BONES];       // its track per bone of that mesh (-1: the bind pose)
    // Morph target curves (Unreal's curves on a sequence; a glTF's "weights" channel): a target's weight over time, by
    // its name. Keys (time s, value) in curve_keys, curve i's curve_count[i] of them from curve_first[i] (sorted by
    // time, linear between). They blend with the poses and drive the actor's morph targets (anim_morph_weight)
    uint32_t ncurves;
    char curve_name[ANIM_MAX_MORPHS][ANIM_MORPH_NAME];
    uint32_t curve_first[ANIM_MAX_MORPHS], curve_count[ANIM_MAX_MORPHS];
    float* curve_keys;
    uint32_t ncurve_keys;
    const void* curve_map_for;            // (the run's: the mesh curve_map is for)
    int8_t curve_map[ANIM_MAX_MORPHS];    // each curve's morph target in that mesh (-1: it has none so named)
} AnimSeq;

// (2026-11-23) A Motion Warping window of a montage (Unreal's Motion Warping notify state): from start to end (s) its
// root motion is bent so the actor ends it at its Warp Target (by name: Add or Update Warp Target)
enum { ANIM_WARP_TRANSLATION = 1, ANIM_WARP_ROTATION = 2, ANIM_WARP_IGNORE_Z = 4 };
#define ANIM_MAX_WARPS 4
typedef struct AnimWarpWindow {
    char target[ANIM_NAME];
    float start, end;
    uint8_t flags; // ANIM_WARP_*
} AnimWarpWindow;

// (2026-11-23, anim_combo.inc) A montage's window (Unreal's Anim Notify State): State (its begin and end told), Combo (the
// input inside chains to next), Hit (a sphere of radius along bone: an event per actor touched)
enum { ANIM_WIN_STATE = 0, ANIM_WIN_COMBO, ANIM_WIN_HIT, ANIM_WIN_KINDS };
#define ANIM_MAX_WINDOWS 8
typedef struct AnimMontageWindow {
    char name[ANIM_NAME];
    float start, end;
    uint8_t kind;
    char next[ANIM_NAME]; // Combo: the section it chains to
    char bone[ANIM_NAME]; // Hit: the bone swept (a socket works too)
    float radius;         // Hit: cm
} AnimMontageWindow;

typedef struct AnimMontage {
    char name[ANIM_NAME], path[512];
    uint32_t hash;
    char anim[ANIM_NAME];  // the sequence's name
    char slot[ANIM_NAME];  // "DefaultSlot": the whole body; "UpperBody": from the graph's upper bone up
    float blend_in, blend_out, rate;
    struct { char name[ANIM_NAME]; float start; } sections[8];
    uint32_t nsections;
    // (2026-11-23, anim_warp.inc) Root motion: the clip's root path moves the actor (a Character with collision), the
    // mesh stays on it; Motion Warping windows; a paired montage's partner: its montage, where it stands from this one's
    // actor (cm along its forward, right, up; its yaw from this one's), the seconds it slides into place
    bool root_motion;
    bool no_collision; // (traversal) its root motion passes through what's in the way (Unreal's Flying mode)
    AnimWarpWindow warps[ANIM_MAX_WARPS];
    uint32_t nwarps;
    char partner[ANIM_NAME];
    float partner_offset[3], partner_yaw, partner_align;
    AnimMontageWindow windows[ANIM_MAX_WINDOWS]; // (2026-11-23)
    uint32_t nwindows;
    bool dirty;
} AnimMontage;

// --- The Anim Graph of an Animation Blueprint (stored in its .rbp) ---
// MOTION_MATCH (anim_mm.inc): samples = the database's sequences, param = the Speed variable, param2 = the Direction one
enum { ANIM_STATE_SEQUENCE = 0, ANIM_STATE_BLEND1D, ANIM_STATE_BLEND_BOOL, ANIM_STATE_BLEND2D, ANIM_STATE_MOTION_MATCH, ANIM_STATE_KIND_COUNT };
#define ANIM_BLEND_SAMPLES 9
// Motion Matching's made animations (anim_mm_gen.inc): starts and stops from the idle, speed changes, the directions
// between two clips, in the clips' own style. Settings (AnimState / AnimNode .mmgen; SET not 1: the defaults):
//   OFF     bits: NO_STARTS (starts and stops), NO_SPEEDS (speed changes), NO_DIRECTIONS, NO_MOVEMENT (the Character's
//           acceleration is its own, not the body's)
//   ACCEL, DECEL  the body's mean acceleration starting / stopping (cm/s2; 0: from the clips' timing)
//   WEIGHT  heavier (> 1: slower starts and stops, more sway) or lighter than the clips look
//   LEAN    the lean with the acceleration (1: physical, atan(a / g)); SETTLE the sway when it stops (0-1)
//   STEP    degrees between the made directions; SET 1 once the values are set
#define ANIM_MMGEN 8
// Motion Matching's features per frame: velocity now (x y), in 0.3 s, the facing's turn in 0.27 s and 0.5 s (cos sin),
// the vertical velocity now and in 0.3 s (a jump's root motion), the feet's places (x y z each), their velocities (x y each)
#define ANIM_MM_FEAT 20
enum { ANIM_MMGEN_OFF = 0, ANIM_MMGEN_ACCEL, ANIM_MMGEN_DECEL, ANIM_MMGEN_WEIGHT, ANIM_MMGEN_LEAN, ANIM_MMGEN_SETTLE, ANIM_MMGEN_STEP, ANIM_MMGEN_SET };
enum { ANIM_MMGEN_NO_STARTS = 1, ANIM_MMGEN_NO_SPEEDS = 2, ANIM_MMGEN_NO_DIRECTIONS = 4, ANIM_MMGEN_NO_MOVEMENT = 8 };
void anim_mm_gen_defaults(float g[ANIM_MMGEN]);
// a transition's rule: a bool true / false, a float > / < a value, the state's animation ending (its time remaining
// under the blend), an int == a value, the time in the state over a value
enum { ANIM_OP_TRUE = 0, ANIM_OP_FALSE, ANIM_OP_GREATER, ANIM_OP_LESS, ANIM_OP_ANIM_END, ANIM_OP_INT_EQUAL, ANIM_OP_STATE_TIME,
       ANIM_OP_COUNT };
enum { ANIM_CURVE_LINEAR = 0, ANIM_CURVE_CUBIC, ANIM_CURVE_EASE_OUT, ANIM_CURVE_COUNT }; // a transition's blend (Unreal's)
#define ANIM_ANY_STATE 255 // a transition's from: any state of its machine (Unreal's State Alias "any")
#define ANIM_MAX_STATES 32
#define ANIM_MAX_TRANS 64
#define ANIM_MAX_MACHINES 4
typedef struct AnimState {
    char name[ANIM_NAME];
    uint8_t machine;       // its state machine (AnimGraph.machines)
    float x, y;            // in the editor's canvas
    uint8_t kind;
    bool loop;
    float rate;
    char clip[ANIM_NAME];  // SEQUENCE; BLEND_BOOL: when the variable is true
    char clip2[ANIM_NAME]; // BLEND_BOOL: when false
    char param[ANIM_NAME]; // BLEND1D / BLEND2D: the (X) float variable; BLEND_BOOL: the bool
    char param2[ANIM_NAME]; // BLEND2D: the Y float variable
    uint8_t nsamples;      // BLEND1D: sequences along the parameter; BLEND2D: at points (X, Y) (gradient band weights)
    char samples[ANIM_BLEND_SAMPLES][ANIM_NAME];
    float sample_at[ANIM_BLEND_SAMPLES], sample_y[ANIM_BLEND_SAMPLES];
    float mmgen[ANIM_MMGEN]; // MOTION_MATCH: its made animations' settings (above)
    float axis[6];         // BLEND1D / BLEND2D: the Blend Space editor's axes: X min, max, Y min, max, X, Y grid divisions (0: from the samples)
} AnimState;
typedef struct AnimTransition {
    uint8_t from, to, op;  // from: ANIM_ANY_STATE for any state of the machine
    char var[ANIM_NAME];
    float value, blend;    // the compared value; the cross-fade (s)
    // more rules, all true together (Unreal's rule graph with AND), the blend's curve, the order it is tried in
    uint8_t nmore, curve, priority;
    struct { uint8_t op; char var[ANIM_NAME]; float value; } more[2];
} AnimTransition;

// --- The AnimGraph's pose nodes (Unreal's AnimGraph: poses flow from the sources through blends and skeletal controls
// to the Output Pose). Each node's inputs are other nodes' poses (in[], -1: none) ---
enum {
    AN_OUTPUT = 0,      // Output Pose: in[0]
    AN_STATE_MACHINE,   // machine: one of the graph's state machines
    AN_SEQUENCE,        // Sequence Player: clip, f[0] play rate, f[1] start position (s), flags & 1 loop
    AN_BLEND_SPACE_1D,  // Blend Space 1D Player: samples at sample_at by var[0]; f[0] play rate
    AN_BLEND_SPACE_2D,  // Blend Space Player (2D): samples at (sample_at, sample_y) by var[0], var[1]; f[0] play rate
    AN_BLEND_BOOL,      // Blend Poses by bool: in[0] when var[0] is true, in[1] when false; f[0] blend time (s)
    AN_BLEND_INT,       // Blend Poses by int: in[var[0]] (0-3); f[0] blend time (s)
    AN_BLEND,           // Blend: in[0] to in[1] by var[0] (or f[0] without it)
    AN_LAYERED_BLEND,   // Layered blend per bone: in[1] over in[0] from bone[0] down its branch; f[0] weight
    AN_APPLY_ADDITIVE,  // Apply Additive: clip's motion from its first frame added on in[0]; var[0] (or f[0]) alpha
    AN_SLOT,            // Slot: in[0], a montage of slot name plays over it
    AN_TWO_BONE_IK,     // Two Bone IK: bone[0] (the end) reaches bone[1]'s place + f[0..2] (cm, actor space); f[3] alpha
    AN_LEG_IK,          // Leg IK: feet bone[0], bone[1] on the ground under them (while playing); f[0] reach (cm)
    AN_AIM_OFFSET,      // Aim Offset: yaw var[0], pitch var[1] (degrees) shared along bone[0..3] by f[0..3]
    AN_LEAN,            // Lean: into turns and with speeding up, along bone[0..2]; f[0] amount
    AN_MODIFY_BONE,     // Transform (Modify) Bone: bone[0] turned by f[0..2] (deg) and moved by f[3..5] (cm); var[0] / f[6] alpha
    AN_LOOK_AT,         // Look At: bone[0] toward the player (an AI) or the camera; f[0] limit (deg), f[1] alpha
    AN_SAVE_CACHE,      // Save Cached Pose: in[0] once a frame, by name
    AN_USE_CACHE,       // Use Cached Pose: the saved one of that name
    AN_MOTION_MATCH,    // Motion Matching: samples (its database), var[0] Speed, var[1] Direction (optional)
    AN_INERTIALIZATION, // Inertialization: a jump in in[0] eases over f[0] seconds
    AN_MIRROR,          // Mirror: in[0] with left and right swapped (a symmetric skeleton); var[0] / flags & 1
    AN_REROUTE,         // Reroute: a knot on a pose wire (in[0] passes through), to organize the graph
    // (2026-11-23) Unreal's Animation Warping: the legs turned to where the Character moves, the chest kept (bone[0] the
    // pelvis, bone[1..3] the spine; f[0] max angle, f[1] alpha, f[2] speed it turns at (/s), flags & 1: four directions;
    // var[0]: an angle variable instead of the movement's)
    AN_ORIENT_WARP,
    // ... the stride stretched to the Character's speed: the feet (bone[0], bone[1]) placed along the move by the speed /
    // the animation's (from its planted foot) and reached by IK; f[0] min, f[1] max scale, f[2] alpha, f[3] pelvis drop
    // (cm per scale above 1); var[0]: a scale variable instead
    AN_STRIDE_WARP,
    AN_KIND_COUNT
};
#define ANIM_MAX_NODES 48
#define ANIM_NODE_IN 4
typedef struct AnimNode {
    uint8_t kind, machine, nsamples, flags;
    float x, y;
    int16_t in[ANIM_NODE_IN];
    char name[ANIM_NAME];    // Slot: the slot; Save / Use Cached Pose: the cache
    char var[2][ANIM_NAME];  // the variables it reads (by kind)
    char clip[ANIM_NAME];
    char bone[4][ANIM_NAME];
    float f[8];
    char samples[ANIM_BLEND_SAMPLES][ANIM_NAME];
    float sample_at[ANIM_BLEND_SAMPLES], sample_y[ANIM_BLEND_SAMPLES];
    float mmgen[ANIM_MMGEN]; // AN_MOTION_MATCH: its made animations' settings
    float axis[6];           // the blend spaces: the Blend Space editor's axes (AnimState.axis)
} AnimNode;
// A node kind's look and pins (the editor) and the defaults it starts with
typedef struct AnimNodeInfo { const char* name; const char* category; uint32_t color; uint8_t inputs; const char* pins[ANIM_NODE_IN]; } AnimNodeInfo;
const AnimNodeInfo* anim_node_info(int kind);
void anim_node_init(AnimNode* n, int kind);
// The old graphs (one state machine, the montages, Lean, Aim Offset, Foot IK as settings) as nodes; a graph without
// nodes gets them (on load, before saving, at run time)
void anim_graph_ensure_nodes(struct AnimGraph* g);
typedef struct AnimGraph {
    char skeleton[ANIM_NAME];  // the Skeletal Mesh (its name)
    char upper_bone[ANIM_NAME]; // the UpperBody slot's bone ("spine_01")
    uint32_t nstates, ntrans, entry; // entry: machine 0's (machine_entry[0])
    AnimState states[ANIM_MAX_STATES];
    AnimTransition trans[ANIM_MAX_TRANS];
    // the state machines (a State Machine node shows one), the pose graph
    uint32_t nmachines;
    char machines[ANIM_MAX_MACHINES][ANIM_NAME];
    uint8_t machine_entry[ANIM_MAX_MACHINES];
    uint32_t nnodes;
    AnimNode nodes[ANIM_MAX_NODES];
    // Aim Offset: the upper body turned toward yaw / pitch (degrees, float variables; + yaw to the right, + pitch up),
    // shared along up to 4 bones (spine to head), after the montage
    char aim_yaw[ANIM_NAME], aim_pitch[ANIM_NAME];
    char aim_bones[4][ANIM_NAME];
    float aim_share[4];
    // Foot IK (while playing): each foot on the ground under it (two bones: thigh, calf -> foot), the pelvis lowered
    // for the lower one, smoothed; a foot moves at most foot_reach cm
    bool foot_ik;
    char foot_l[ANIM_NAME], foot_r[ANIM_NAME];
    float foot_reach;
    // Lean: the body tilts into its turns and leans with its speeding up / slowing down (0: off, 1: strong), along the
    // Aim Offset's bones (the spine)
    float lean;
    uint32_t rev; // bumped when it changes (a load, the editor): the instances find its bones again
} AnimGraph;

// The runtime component: on an actor (a Character's mesh, a placed Skeletal Mesh Actor)
// (in components.h: CSkeletalMesh)

// --- Assets ---
// The Content folder; make_defaults (the editor): the Mannequin, its animations and Anim Blueprint when missing
void anim_init(const char* content_root, bool make_defaults);
SkelMesh* anim_mesh(uint32_t hash);       // loaded on demand by the hash of its vpath (NULL: none)
SkelMesh* anim_mesh_name(const char* name);
AnimSeq* anim_seq(uint32_t hash);
AnimSeq* anim_seq_name(const char* name);
AnimMontage* anim_montage_name(const char* name);
uint32_t anim_seq_count(void);
AnimSeq* anim_seq_at(uint32_t i);
uint32_t anim_montage_count(void);
AnimMontage* anim_montage_at(uint32_t i);
uint32_t anim_mesh_count(void);
SkelMesh* anim_mesh_at(uint32_t i);
SkelMesh* anim_mesh_load(const char* vpath);
AnimSeq* anim_seq_load(const char* vpath);
void anim_rescan(void); // new files in Content (an import): found on the next lookup
// The entity that moves with a bone of an actor's Skeletal Mesh (its part held by the bone or the nearest one above it;
// ENTITY_NONE: no such bone / no mesh): sounds and effects attached to bones follow it
Entity anim_bone_entity(World* w, Entity actor, const char* bone);
// The Skeletal Mesh entity of an actor (itself or its component; ENTITY_NONE: none playing)
Entity anim_mesh_entity(World* w, Entity actor);
// (Q11) the entities an actor's Skeletal Mesh draws with (its parts and skins), up to cap; 0: none playing
uint32_t anim_mesh_parts(World* w, Entity actor, Entity* out, uint32_t cap);
// (Q12) Unreal's Hide Bone by Name / Unhide Bone by Name on the actor's Skeletal Mesh: bone and the bones below it shrunk
// to its root (a first-person body's head: the camera doesn't see inside it); hide false: shown again. False: no room
bool anim_hide_bone(World* w, Entity actor, const char* bone, bool hide);
// (Q10, the Q7 sockets in the game) an entity hung on an actor's Skeletal Mesh at a socket (its bone and its offset) or a
// bone: its CAttach becomes kind ATTACH_BONE (its location, rotation, scale an offset more, in the socket's space) and
// the pose that moves that bone places it, every frame, ragdoll and dynamic bones included. False: no Skeletal Mesh
// playing, no such socket or bone. Detach: its CAttach back to the Blueprint's way (ATTACH_SCS)
bool anim_attach_to_socket(World* w, Entity child, Entity actor, const char* socket_or_bone);
// (Q10, Blueprint Attach Actor To Socket) an actor (no CAttach: its own transform is set) on another actor's socket or
// bone, at loc (cm) and rot (deg) in the socket's space; Detach From Socket lets it go where it is
bool anim_attach_actor_to_socket(World* w, Entity actor, Entity parent, const char* socket_or_bone, const float loc[3], const float rot[3]);
void anim_detach_socket(World* w, Entity child);
// (Q10) a socket's (or a bone's) place at the actor's last pose, world rows (the first ask: at the bind pose under the
// actor; then each pose keeps its bones). False: no such socket or bone
bool anim_socket_world(World* w, Entity actor, const char* socket_or_bone, float out[3][4]);
// (Q27.11) Locomotion styles (GTA 6's archetypes of movement): Normal, Injured (a limp: the active ragdoll's Get Limp too),
// Tired, Drunk, Carrying. The Motion Matching searches the clips whose names have the style (Injured, Tired, Drunk,
// Carry) when it has them; else a layer turns a few bones. The weight eases (no pop)
enum { ANIM_STYLE_NORMAL = 0, ANIM_STYLE_INJURED, ANIM_STYLE_TIRED, ANIM_STYLE_DRUNK, ANIM_STYLE_CARRY, ANIM_STYLE_COUNT };
bool anim_set_locomotion_style(World* w, Entity actor, int style);
int anim_locomotion_style(World* w, Entity actor);
void anim_style_cost(double* ms, uint32_t* n); // the layer's time and calls since the start
// (Q10) rows (a bone's, a socket's) as a transform (location, rotator, scale)
struct CTransform;
void anim_rows_transform(const float rows[3][4], struct CTransform* out);
AnimMontage* anim_montage_load(const char* vpath);
bool anim_mesh_save(SkelMesh* m);
bool anim_seq_save(AnimSeq* s);
bool anim_montage_save(AnimMontage* m);
AnimMontage* anim_montage_create(const char* folder_vpath, const AnimSeq* from);
// The Anim Graph of a class (NULL when it is not an Animation Blueprint); made with a default one on first use
AnimGraph* anim_graph_of(struct BpAsset* a, bool create);
// JSON of the graph inside the .rbp (blueprint.c calls them)
struct JsonWriter;
struct JsonValue;
void anim_graph_write(const AnimGraph* g, struct JsonWriter* w);
void anim_graph_read(AnimGraph* g, const struct JsonValue* v);

// --- Poses ---
// A sequence's local pose at time t (seconds) for a skeleton (bones without a track keep the bind pose)
void anim_sample(const AnimSeq* s, const SkelMesh* m, float t, AnimXf* out);
void anim_blend(const AnimXf* a, const AnimXf* b, float w, uint32_t n, AnimXf* out);
// (2026-11-20) A pose given from outside for an actor's Skeletal Mesh (the VR hands, vr_pawn.c: the fingers by the
// tracked joints or the controller): its local pose (the mesh's bones, nbones of them) used in place of its animation
// from the next anim_update on; NULL lets it go. A handful at a time (false: full)
bool anim_pose_override(Entity e, const AnimXf* pose, uint32_t nbones);
// Several poses weighted (the weights summing 1): rotations aligned and normalized, positions and scales averaged
void anim_blend_n(const AnimXf* const* poses, const float* w, uint32_t count, uint32_t nbones, AnimXf* out);
// A 2D blend space's weights at (x, y) for samples at (sx, sy): gradient band interpolation (any layout of points)
void anim_blend2d_weights(const float* sx, const float* sy, uint32_t n, float x, float y, float* w);
// Two bone IK in world space: hip a, knee b, end c moved so c reaches target (the knee bends toward where it was);
// out_b, out_c: the new knee and end
void anim_two_bone_ik(const float a[3], const float b[3], const float c[3], const float target[3], float out_b[3], float out_c[3]);
// The bones' world rows (affine, 3 x 4) from a local pose and the actor's rows
void anim_world(const SkelMesh* m, const AnimXf* local, const float actor[3][4], float out[][3][4]);

// A part's rows under its bone's; a rotator (pitch, yaw, roll degrees) as a quaternion and back (the editors' fields)
void anim_part_rows(const SkPart* p, const float bone[3][4], float out[3][4]);
// The bones' palette of a Skeletal Mesh's skins (its entity: the actor or the component with CSkeletalMesh): per bone
// its pose in the mesh's space times the inverse of its bind (rows), what the GPU deforms the skins with; version
// changes with the pose. NULL when it has none yet
const float (*anim_skin_palette(Entity sk, uint32_t* nbones, uint32_t* version))[3][4];
void anim_euler_quat(const float rot_deg[3], float q[4]);
void anim_quat_euler(const float q[4], float rot_deg[3]);

// --- Run time ---
void anim_begin_play(World* w);
void anim_end_play(World* w);
// Every frame (editor and game): the parts of the skeletal meshes made (once), the poses sampled while playing (the
// editor shows the bind pose or the preview animation)
void anim_update(World* w, float dt, bool playing);
// The camera (the editor's or the game's, each frame; NULL: none): the Look At nodes turn toward it
void anim_set_view_point(const float p[3]);

// The editors' previews of an Anim Blueprint (its Viewport, the Anim Graph's corner): the graph run by itself on a
// skeleton (no actor: its variables are the editor's values; no notifies), slot: the preview's own instance. dt < 0
// starts it over from the entry states. The pose (local) after dt; the state its first machine is in
#define ANIM_PREVIEW_SLOTS 4
typedef struct AnimPreviewVar { char name[ANIM_NAME]; float value; } AnimPreviewVar;
bool anim_preview_pose(int slot, AnimGraph* g, SkelMesh* m, float dt, const AnimPreviewVar* vars, uint32_t nvars, AnimXf* pose);
const char* anim_preview_state(int slot);

// --- Motion Matching's database, as its editor shows it (anim_mm.inc): a state's (or a node's stand-in state's) clips on
// a Skeletal Mesh. Each clip makes groups: time-warped (0.8x-1.25x) and mirrored copies, 30 frames a second; and the
// made ones (anim_mm_gen.inc): directions in between, starts, stops, speed changes ---
enum { ANIM_MMG_CLIP = 0, ANIM_MMG_DIRECTION, ANIM_MMG_START, ANIM_MMG_STOP, ANIM_MMG_SPEED, ANIM_MMG_KINDS };
typedef struct AnimMmGroup {
    char clip[ANIM_NAME];
    uint8_t clip_index, kind;
    bool mirror, loop;
    float rate, speed, dir_deg, length; // the play rate, its speed (cm/s) and direction (degrees, root space), seconds
    uint32_t frames;
    char label[96];     // a made one's: "from > to"
    int next;           // a one-shot's: the loop it ends in (-1: none)
    float accel;        // a one-shot's mean acceleration (cm/s2; - slowing down)
    float slide;        // a made one's feet sliding over its travel (0: none)
    uint8_t root_motion; // a clip's: 0 none (a loop in place), 1 its root bone's, 2 its pelvis's, 3 in place (its feet's, a turn)
} AnimMmGroup;
typedef struct AnimMmInfo {
    uint32_t frames, ngroups;
    bool can_mirror, drives;
    double build_ms, gen_ms;
    float clip_speed[ANIM_BLEND_SAMPLES];
    uint32_t made[ANIM_MMG_KINDS];
    // the body's limits by speed (the forward ones: what the Character's movement follows)
    uint32_t nlim;
    float lim_speed[8], lim_acc[8], lim_dec[8];
} AnimMmInfo;
typedef struct AnimMmMatch { int group; float t, cost, cur_cost; bool switch_to; } AnimMmMatch;
// A play of the database (the game's and the editor's Query Debugger): the group playing and its time, the one fading out
typedef struct AnimMmPlay {
    int32_t cur, prev;
    float t, pt, rate, prate, blend, blend_len, next, qlast, qface, dist; // dist: a one-shot's travel so far (cm)
    float st, srate;  // the new group's time and rate at the switch (the inertialization's jump is measured there)
    float yaw_rate;   // the playing frame's turn (radians a second: a clip that turns turns the actor)
    float vz, qvz;    // the playing frame's vertical velocity (cm/s: a jump's root motion), the last search's asked one in 0.3 s
    bool vlock;       // a jump the clip carries under way: no switches, its time real (the game's, mm_step)
    uint32_t searches, switches;
} AnimMmPlay;
bool anim_mm_info(const AnimGraph* g, const SkelMesh* m, const AnimState* s, AnimMmInfo* info, AnimMmGroup* groups, uint32_t cap);
// The game's step on a play (speed cm/s, direction degrees, acceleration cm/s2, the facing's turn asked in degrees; cur
// < 0: starts fresh): its times (a
// one-shot's following the speed), a one-shot's end into its loop, the search (every 0.1 s, or at once when the query
// jumps); last: the latest search's
void anim_mm_sim_step(const AnimGraph* g, const SkelMesh* m, const AnimState* s, AnimMmPlay* p, float speed, float dir_deg, float accel, float turn_deg,
                      float dt, AnimMmMatch* last);
bool anim_mm_sim_pose(const AnimGraph* g, const SkelMesh* m, const AnimState* s, const AnimMmPlay* p, AnimXf* out);
bool anim_mm_pose(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, AnimXf* out);
bool anim_mm_features(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, float f[ANIM_MM_FEAT]);
// A group's travel from its start to t (cm, root space: x forward) and its speed at t (cm/s)
bool anim_mm_travel(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, float out[2], float* speed);
// The body's acceleration going from one speed to another (cm/s2: the start's or the stop's limit); 0: none
float anim_mm_limit(const AnimGraph* g, const SkelMesh* m, const AnimState* s, float from, float to);
// A group saved as an Animation Sequence next to the database's first clip, named after it (root_motion: its travel on the
// root bone, as a clip of the user's would carry it); out (ANIM_NAME): its name
bool anim_mm_export(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, bool root_motion, char* out, size_t cap);
// A node's stand-in state (Blend Spaces, Motion Matching: the code the states share)
void anim_node_state(const AnimNode* n, AnimState* s);
// Blueprints: Play Montage / Stop Montage / Play Animation on an actor
bool anim_play_montage(World* w, Entity actor, const char* montage, float rate);
void anim_stop_montage(World* w, Entity actor, float blend_out);
// (2026-11-23, anim_warp.inc) Motion Warping's targets by actor and name; paired (synced) montages; the root motion the
// Characters' movement takes (with collision)
bool anim_warp_target_set(World* w, Entity actor, const char* name, const float loc[3], float yaw_deg);
void anim_warp_target_remove(World* w, Entity actor, const char* name); // (name empty: all of the actor's)
bool anim_play_paired(World* w, Entity primary, Entity partner, const char* montage, float rate);
void anim_stop_paired(World* w, Entity actor, float blend_out);
bool anim_paired_partner(World* w, Entity actor, Entity* other);
// (2026-11-23) the paired montage of a set (its name starting with prefix, "": any) whose partner place fits the partner
// best (within max_dist cm and max_angle deg; 0: no limit; NULL: none fits); whether one can play (the fit within the
// limits, nothing in between)
const char* anim_find_paired(World* w, Entity primary, Entity partner, const char* prefix, float max_dist, float max_angle);
// (2026-11-23, anim_combo.inc) the attack button while a combo montage plays (true: inside a Combo window, kept); whether a
// window of that name is open; the hits counted (the tests)
bool anim_montage_combo_input(World* w, Entity actor);
bool anim_montage_window_open(World* w, Entity actor, const char* name);
uint32_t anim_montage_hit_count(Entity actor);
// (2026-11-23) a hit's reaction: by where it came from (Front, Back, Left, Right: the name returned), a layer throwing the
// spine and head away from it (strength 0..2) and, when prefix names a set (prefix_Front...), its montage
const char* anim_hit_reaction(World* w, Entity actor, const float loc[3], float strength, const char* prefix);
// (2026-11-23, anim_cover.inc) third-person cover: in (the nearest wall within search cm: false none), out, its state (low,
// at its left / right end), peek; the Character's movement in it (bp_vm.c move_character: before it moves, after)
bool anim_cover_enter(World* w, Entity e, float search);
void anim_cover_exit(World* w, Entity e);
bool anim_cover_state(World* w, Entity e, bool* low, bool* left, bool* right);
void anim_cover_peek(World* w, Entity e, bool on);
struct CTransform;
struct CCharacter;
bool anim_cover_input(World* w, Entity e, struct CTransform* t, struct CCharacter* c, float dt);
void anim_cover_after(World* w, Entity e, struct CTransform* t, float dt);
bool anim_can_play_paired(World* w, Entity primary, Entity partner, const char* montage, float max_dist, float max_angle);
bool anim_root_motion_take(Entity e, float d[3], float* yaw_deg, bool* vert, bool* end, bool* nocol);
bool anim_warp_state(World* w, Entity actor, float* move_angle, float* orient, float* anim_speed, float* stride); // (2026-11-23: the test's)
// (2026-11-23, anim_traversal.inc) Traversal: what's ahead of a Character traced, the action picked (Vault, Mantle,
// Climb), its warp targets set and its montage played; the action (0: none) and the obstacle's height
int anim_try_traversal(World* w, Entity e, float max_dist, float max_h, bool draw, float* height);
const char* anim_traversal_name(int action);
void anim_traversal_cost(double* ms, uint32_t* checks);
void anim_set_montage_rate(World* w, Entity actor, float rate);   // (Q10) 0 holds it on its frame
const char* anim_montage_playing(World* w, Entity actor, float* t); // (Q10) its name (NULL: none) and time
void anim_play(World* w, Entity actor, const char* sequence, bool loop);
// The fighting pack (anim_fight.inc, the Fighting demo): the Mannequin's guard, strikes, block, dodge, roll, KO, win
// clips, their montages (AM_Fight_*) and ABP_Fighter, under Characters/Mannequin/Fighting. Returns the files made.
uint32_t anim_make_fight_pack(void);
void anim_use_blueprint(World* w, Entity actor); // back from anim_play's Animation mode to its Animation Blueprint
bool anim_uses_blueprint(World* w, Entity actor); // (Q23) its Animation Blueprint drives it (false: anim_play's Animation mode, or none)
// A sequence's speed over the ground for a mesh: its root motion, or its planted foot's (0: in place, or no feet found)
float anim_seq_ground_speed(AnimSeq* s, const SkelMesh* m);
// The packs of animations (anim_pack.inc): the Locomotion one for the Mannequins, the creatures (each a skeleton, its
// parts and its clips); the files made (0: none)
// ANIM_PACK_MM_EXAMPLES (anim_mm_examples.inc): characters of their own styles, each with only an idle, a walk and a run
// (one also strafes) and a Motion Matching Animation Blueprint: the rest made from them
enum { ANIM_PACK_LOCOMOTION = 0, ANIM_PACK_QUADRUPED, ANIM_PACK_FISH, ANIM_PACK_BIRD, ANIM_PACK_REPTILE, ANIM_PACK_MM_EXAMPLES, ANIM_PACK_COUNT };
uint32_t anim_make_pack(int pack);
// The Foot IK's ground offsets of an actor's feet (left, right; cm, smoothed) and its pelvis; false without a pose
bool anim_foot_offsets(World* w, Entity actor, float feet[2], float* pelvis);
// The blend spaces' and the IK's math checked (--anim-selftest): false with the failures printed
bool anim_selftest(void);
// A skeletal mesh moved in place in the editor (the gizmo's way): its parts follow (--sk-move-test; relatório #114)
bool anim_test_editor_move(World* w);
// The Debug Viewer
// foot_slide: the planted feet's drift over the bodies' travel (all instances, since the play started: 0 = no sliding);
// mm_*: Motion Matching's searches, switches and their time (since the play started)
typedef struct AnimStats {
    uint32_t meshes, sampled, parts;
    double ms;
    float foot_slide, travel_cm;
    uint32_t mm_searches, mm_switches, mm_frames;
    double mm_ms;
    // the made animations (anim_mm_gen.inc): how many, their feet sliding (mean, as made), the starts / stops played
    uint32_t mm_made, mm_starts, mm_stops;
    float mm_made_slide;
    float mm_play_slide, mm_play_travel; // while they play: the feet's drift over the travel (cm)
    float mm_turn_s; // the seconds a clip turned an actor (a turn, a pivot: the clip's root motion)
    uint32_t mm_authored; // the user's own one-shots played (starts, stops, turns, a take's moments)
    // jumps a clip carried (its vertical root motion): how many, how they ended (landed, blocked, switched, never left),
    // the seconds, the capsule's highest rise (cm)
    uint32_t mm_jumps, mm_jump_ends[4];
    float mm_jump_s, mm_jump_rise;
} AnimStats;
const AnimStats* anim_stats(void);
uint32_t anim_profile_items(Entity* e, float* ms, const char** state, uint32_t cap);

// --- Morph target curves (AnimSeq.curve_*: notas-dev/animacao.md "Curvas de morph") ---
int anim_curve_index(const AnimSeq* s, const char* name);           // case ignored; -1: none
float anim_curve_eval(const AnimSeq* s, uint32_t curve, float t);    // linear between the keys, held past the ends
// A curve's keys replaced (n 0: the curve removed); times sorted here. False: 64 curves already / no memory
bool anim_curve_set(AnimSeq* s, const char* name, const float* times, const float* values, uint32_t n);
// The weight the animation gives morph i of the actor's mesh this frame (false: no curve of what it plays drives it)
bool anim_morph_from_curves(const struct CSkeletalMesh* c, uint32_t i, float* w);
