// (2026-11-23, the user: "every rig the Tlender is compatible with, the Real compatible too") The part of the Tlender's
// armature that its character definition (charmap.c, charmap_analyze.c) and rig systems (rig_systems.c) read, copied
// so those files work here unchanged in substance: bones with a name, a parent, head and tail in the rest pose, deform,
// constraints (IK, copy, child of...) and the auto-rig's tags. rig_detect.c fills one from a Skeletal Mesh (its bind
// pose, the importer's rig info) and asks them what each bone is. Pure: no engine headers.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// --- math3d.h's vec3 (the few the copied files use) --------------------------------------------------------------------
typedef struct vec3 { float x, y, z; } vec3;

// --- armature.h ------------------------------------------------------------------------------------------------------
#define TL_BONE_NAME_LEN 64
#define TL_BONE_MAX_CONSTRAINTS 8
enum {
    CON_NONE = 0, CON_IK, CON_COPY_LOCATION, CON_COPY_ROTATION, CON_COPY_SCALE, CON_COPY_TRANSFORMS, CON_DAMPED_TRACK,
    CON_LIMIT_LOCATION, CON_LIMIT_ROTATION, CON_LIMIT_SCALE, CON_STRETCH_TO, CON_CHILD_OF_RESERVED, CON_CHILD_OF,
    CON_ARMATURE, CON_TRANSFORMATION, CON_TRACK_TO, CON_LOCKED_TRACK, CON_LIMIT_DISTANCE, CON_MAINTAIN_VOLUME, CON_ACTION,
    CON_SPLINE_IK
};
typedef struct BoneConstraint {
    int type;
    bool enabled;
    float influence;
    char target[TL_BONE_NAME_LEN]; // a target bone ("": none)
    char target_obj[TL_BONE_NAME_LEN]; // another object ("": the bone's own armature)
    char pole[TL_BONE_NAME_LEN];   // IK: the pole bone
    int chain;                     // IK: bones in the chain (0: up to the root)
} BoneConstraint;

// --- autorig.h (the Tlender's generated rig: its bones' tags) ----------------------------------------------------------
enum { RMOD_NONE = 0 };
typedef enum RigRole { RROLE_NONE = 0, RROLE_CTRL, RROLE_DEF, RROLE_MCH } RigRole;
#define TL_RIG_CHAIN 12
typedef struct RigModuleInfo {
    int id, type, side;
    char name[64];
    int count;
    int fk[TL_RIG_CHAIN], ik[TL_RIG_CHAIN], def[TL_RIG_CHAIN];
    int ik_ctrl, pole, holder, collections;
} RigModuleInfo;

typedef struct Bone {
    char name[TL_BONE_NAME_LEN];
    int parent;      // -1: a root
    vec3 head, tail; // the rest pose, the armature's space (Blender's: +X left, Z up; meters)
    float length;
    bool deform;     // weighted to the skin (else a control or a helper)
    BoneConstraint cons[TL_BONE_MAX_CONSTRAINTS];
    int con_count;
    int rig_role, rig_module; // the auto-rig's tags (RROLE_NONE, -1 here: the engine has none)
    struct { int type; } rigmod;
} Bone;

struct CharMap;
typedef struct Armature {
    Bone* bones;
    int bone_count;
    bool autorig_rig;         // made by the Tlender's auto-rig (its tags)
    struct CharMap* charmap;  // (unused here: the engine keeps its maps in rig_detect.c)
} Armature;

static inline int armature_find_bone(const Armature* a, const char* name) {
    for (int i = 0; name && name[0] && i < a->bone_count; i++) {
        const char *p = a->bones[i].name, *q = name;
        while (*p && *p == *q) p++, q++;
        if (!*p && !*q) return i;
    }
    return -1;
}
// (the engine keeps no module list: a generated rig's chains are found by their names like any other rig's)
static inline int autorig_modules(const Armature* rig, RigModuleInfo* out, int cap) { (void)rig, (void)out, (void)cap; return 0; }
// (charmap.c's file helpers)
FILE* re_fopen(const char* path_utf8, const char* mode); // (platform.h)
#define tl_fopen re_fopen
