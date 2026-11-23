// The analyzer of the character definition (charmap.h, plan item AM1, v0): from an armature's rest pose to the roles
// of its bones. In order: (1) the naming convention that fits most bones (Tlender's auto-rig and Rigify, Mixamo, the
// UE mannequin, VRoid, Character Creator, 3ds Max Biped, Daz, SMPL, then common words), with the deforming bone
// preferred over the controls of a control rig and its FK control noted; (2) the shape of the skeleton for what the
// names did not give: the hips are where two mirrored legs and a spine meet, the chest where two mirrored arms and a
// neck do, the hand where the fingers branch; (3) the geometry of the rest pose: the side by X (+X is the left, as in
// Blender), a horizontal spine makes a quadruped; (4) what is left over in long chains becomes generic chains (tail,
// legs, tentacles). The tables of names are the public naming of each program's skeleton, written here from scratch.
#include "charmap.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rig_armature_math.h"
#include "rig_systems.h"

// --- Names --------------------------------------------------------------------------------------------------------------

typedef struct Parsed {
    char base[TL_BONE_NAME_LEN]; // lower case, letters and digits only, without prefix and side
    int side;                    // 0, 1 left, 2 right
    int seg;                     // the number after the side ("upper_arm.L.001": 1), 0
    bool def, helper;            // a DEF- bone; an ORG- / MCH- bone
} Parsed;

static bool starts_ci(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    return true;
}

static bool is_sep(char c) { return c == '.' || c == '_' || c == '-' || c == ' '; }

// The accented letters of Portuguese and Spanish names (UTF-8, "Cabeça", "Mão", "Pé") as plain ones, so that the words
// of the tables match them: out holds at most cap - 1 bytes.
static void fold_accents(const char* in, char* out, size_t cap) {
    static const char from[] = "\xa0\xa1\xa2\xa3\xa4\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb1\xb2\xb3\xb4\xb5\xb6\xb9\xba\xbb\xbc"
                               "\x80\x81\x82\x83\x84\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x91\x92\x93\x94\x95\x96\x99\x9a\x9b\x9c";
    static const char to[] = "aaaaaceeeeiiiinooooouuuuAAAAACEEEEIIIINOOOOOUUUU";
    size_t k = 0;
    for (const unsigned char* s = (const unsigned char*)in; *s && k + 1 < cap; s++) {
        if (*s == 0xc3 && s[1]) {
            const char* at = strchr(from, (char)s[1]);
            if (at) {
                out[k++] = to[at - from];
                s++;
                continue;
            }
        }
        out[k++] = (char)*s;
    }
    out[k] = 0;
}

static void parse_name(const char* name, Parsed* p, int sys) {
    memset(p, 0, sizeof *p);
    char buf[TL_BONE_NAME_LEN];
    const char* colon = strrchr(name, ':'); // a namespace ("mixamorig:Hips")
    fold_accents(colon ? colon + 1 : name, buf, sizeof buf);
    char* s = buf;
    // Maya's HumanIK names the skeleton after the character: "Character1_LeftUpLeg"
    if (starts_ci(s, "Character") && isdigit((unsigned char)s[9])) {
        char* u = s + 9;
        while (isdigit((unsigned char)*u)) u++;
        if (*u == '_') s = u + 1;
    }
    if (starts_ci(s, "DEF-") || starts_ci(s, "DEF_") || starts_ci(s, "DEF.")) { // DEF.: the rigs of some studios (Mario's)
        p->def = true;
        s += 4;
    } else if (starts_ci(s, "ORG-") || starts_ci(s, "MCH-") || starts_ci(s, "ORG.") || starts_ci(s, "MCH.")) {
        p->helper = true;
        s += 4;
    }
    static const char* const prefixes[] = { "mixamorig", "J_Bip_", "J_Adj_", "J_Sec_", "CC_Base_", "ValveBiped.Bip01_",
                                            "Bip001 ",   "Bip01 ", "Bip001_", "Bip01_", "JNT_", "bip_", "RL_" }; // JNT_: game rigs (JNT_L_Shoulder); bip_: Source / SFM; RL_: Reallusion
    // Auto-Rig Pro names some deforming bones after their control ("c_index2.l")
    if (sys == RIGSYS_ARP && strncmp(s, "c_", 2) == 0) s += 2;
    for (int i = 0; i < (int)(sizeof prefixes / sizeof prefixes[0]); i++)
        if (starts_ci(s, prefixes[i])) {
            s += strlen(prefixes[i]);
            while (i == 0 && (isdigit((unsigned char)*s) || *s == '_')) s++;
            break;
        }
    size_t len = strlen(s);
    // Maya's joints end in "_jnt" or "_joint" ("L_Shoulder_jnt", "spine_1_jnt": Woody, Roxanne, the Mimic)
    static const char* const joint_ends[] = { "jnt", "joint" };
    for (int i = 0; i < 2; i++) {
        size_t el = strlen(joint_ends[i]);
        if (len > el + 1 && is_sep(s[len - el - 1]) && starts_ci(s + len - el, joint_ends[i])) {
            len -= el + 1;
            s[len] = 0;
            break;
        }
    }
    // a number after the side: "upper_arm.L.001"; two digits in the legacy Rigify ("DEF-f_index.01.L.02", the two
    // halves of a finger's first bone)
    for (int digits = 3; digits >= 2; digits--) {
        if (len <= (size_t)digits + 3) continue;
        size_t at = len - (size_t)digits - 1; // the '.' before the number
        if (s[at] != '.' || !is_sep(s[at - 2]) || !strchr("LRlr", s[at - 1])) continue;
        bool num = true;
        for (size_t k = at + 1; k < len; k++) num = num && isdigit((unsigned char)s[k]);
        if (!num) continue;
        p->seg = atoi(s + at + 1);
        len = at;
        s[len] = 0;
        break;
    }
    // the middle at the end: Auto-Rig Pro's ".x", AdvancedSkeleton's "_M"
    if (len > 2 && ((s[len - 1] == 'x' && s[len - 2] == '.') || (s[len - 1] == 'M' && s[len - 2] == '_'))) {
        len -= 2;
        s[len] = 0;
    }
    // the side at the end: ".L", "_r", " R", "Left"
    if (len > 2 && is_sep(s[len - 2]) && strchr("LRlr", s[len - 1])) {
        p->side = tolower((unsigned char)s[len - 1]) == 'l' ? 1 : 2;
        len -= 2;
    } else if (len > 5 && starts_ci(s + len - 4, "left")) {
        p->side = 1;
        len -= 4;
    } else if (len > 6 && starts_ci(s + len - 5, "right")) {
        p->side = 2;
        len -= 5;
    }
    s[len] = 0;
    // the side at the start: "Left", "L_", "l" before a capital (Daz); "C_" is the center (VRoid)
    if (!p->side) {
        if (starts_ci(s, "left") && len > 4) {
            p->side = 1;
            s += 4;
        } else if (starts_ci(s, "right") && len > 5) {
            p->side = 2;
            s += 5;
        } else if (len > 2 && strchr("LRlr", s[0]) && (s[1] == '_' || s[1] == ' ')) {
            p->side = tolower((unsigned char)s[0]) == 'l' ? 1 : 2;
            s += 2;
        } else if (len > 2 && (s[0] == 'l' || s[0] == 'r') && isupper((unsigned char)s[1])) {
            p->side = s[0] == 'l' ? 1 : 2;
            s += 1;
        } else if (len > 2 && s[0] == 'C' && s[1] == '_') s += 2;
    }
    int n = 0;
    for (; *s && n < TL_BONE_NAME_LEN - 1; s++)
        if (isalnum((unsigned char)*s)) p->base[n++] = (char)tolower((unsigned char)*s);
    p->base[n] = 0;
}

typedef enum RuleKind {
    NR_EXACT = 0, // the base is the word
    NR_NUM,       // the word and a number: role + (number - first), for `count` numbers
    NR_SPINE,     // a bone of the spine chain (the word, or the word and a number): placed by its depth
    NR_HEADS,     // neck and head taken from the end of the spine chain (Rigify: spine.004 to spine.006)
} RuleKind;

typedef struct NameRule {
    const char* word;
    int role; // a center role, or the LEFT role of a side role
    uint8_t kind, first, count;
} NameRule;

typedef struct Convention {
    const char* name;
    bool first_spine_is_hips; // the chain starts at the hips (Rigify's "spine")
    float confidence;
    const NameRule* rules;
    int count;
} Convention;

#define FINGERS(thumb, index, middle, ring, pinky)                                                                              \
    { thumb, CR_THUMB1_L, NR_NUM, 1, 3 }, { index, CR_INDEX1_L, NR_NUM, 1, 3 }, { middle, CR_MIDDLE1_L, NR_NUM, 1, 3 },         \
    { ring, CR_RING1_L, NR_NUM, 1, 3 }, { pinky, CR_PINKY1_L, NR_NUM, 1, 3 }

// Tlender's meta-rig and auto-rig, and Rigify: spine, spine.001...; in Rigify the neck and the head are the end of it.
// The legacy Rigify (Blender 2.7x) splits each limb bone in two (DEF-upper_arm.01.L, DEF-upper_arm.02.L: the bone and
// its twist) and the first bone of a finger too (DEF-f_index.01.L.01 and .02); its old meta-rig has hips and chest.
static const NameRule RULES_RIGIFY[] = {
    { "spine", 0, NR_SPINE, 0, 0 },       { "", 0, NR_HEADS, 0, 0 },           { "neck", CR_NECK1, NR_EXACT, 0, 0 },
    { "neck", CR_NECK1, NR_NUM, 0, 2 },   { "head", CR_HEAD, NR_EXACT, 0, 0 }, { "jaw", CR_JAW, NR_EXACT, 0, 0 },
    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 }, { "shin", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toe", CR_TOE_L, NR_EXACT, 0, 0 },  FINGERS("thumb", "findex", "fmiddle", "fring", "fpinky"),
    // split in numbered halves by some studios (Mario: DEF.upper_arm.01.L, DEF.lower_arm.01.L, DEF.head.01)
    { "lowerarm", CR_FOREARM_L, NR_NUM, 1, 1 }, { "lowerarm", CR_FOREARM_TWIST_L, NR_NUM, 2, 1 }, { "head", CR_HEAD, NR_NUM, 1, 1 },
    { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "hips", CR_HIPS, NR_EXACT, 0, 0 },  { "chest", 0, NR_SPINE, 0, 0 },      { "ribs", 0, NR_SPINE, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_NUM, 1, 1 }, { "upperarm", CR_UPPER_ARM_TWIST_L, NR_NUM, 2, 1 },
    { "forearm", CR_FOREARM_L, NR_NUM, 1, 1 },    { "forearm", CR_FOREARM_TWIST_L, NR_NUM, 2, 1 },
    { "thigh", CR_THIGH_L, NR_NUM, 1, 1 },        { "thigh", CR_THIGH_TWIST_L, NR_NUM, 2, 1 },
    { "shin", CR_SHIN_L, NR_NUM, 1, 1 },          { "shin", CR_SHIN_TWIST_L, NR_NUM, 2, 1 },
};
// Mixamo and HumanIK (Maya, MotionBuilder; also Rokoko, Xsens and other capture tools): the same names, HumanIK's
// without the prefix, its twist bones "LeftArmRoll", its top "Reference".
static const NameRule RULES_MIXAMO[] = {
    { "hips", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "neck", CR_NECK1, NR_EXACT, 0, 0 },
    { "neck", CR_NECK2, NR_NUM, 1, 1 },   { "head", CR_HEAD, NR_EXACT, 0, 0 }, { "eye", CR_EYE_L, NR_EXACT, 0, 0 },
    { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "arm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 },
    { "hand", CR_HAND_L, NR_EXACT, 0, 0 }, { "upleg", CR_THIGH_L, NR_EXACT, 0, 0 }, { "leg", CR_SHIN_L, NR_EXACT, 0, 0 },
    { "foot", CR_FOOT_L, NR_EXACT, 0, 0 }, { "toebase", CR_TOE_L, NR_EXACT, 0, 0 },
    FINGERS("handthumb", "handindex", "handmiddle", "handring", "handpinky"),
    { "reference", CR_ROOT, NR_EXACT, 0, 0 }, { "armroll", CR_UPPER_ARM_TWIST_L, NR_EXACT, 0, 0 },
    { "forearmroll", CR_FOREARM_TWIST_L, NR_EXACT, 0, 0 }, { "uplegroll", CR_THIGH_TWIST_L, NR_EXACT, 0, 0 },
    { "legroll", CR_SHIN_TWIST_L, NR_EXACT, 0, 0 },
};
// Unreal's mannequins (UE4: spine_01..03; UE5: spine_01..05, neck_01/02) and MetaHuman's body (the UE5 skeleton with
// metacarpals, more twist and corrective bones, and the face under FACIAL_C_FacialRoot).
static const NameRule RULES_UE[] = {
    { "root", CR_ROOT, NR_EXACT, 0, 0 },  { "pelvis", CR_HIPS, NR_EXACT, 0, 0 }, { "spine", 0, NR_SPINE, 0, 0 },
    { "neck", CR_NECK1, NR_NUM, 1, 2 },   { "head", CR_HEAD, NR_EXACT, 0, 0 },   { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "lowerarm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 }, { "calf", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "ball", CR_TOE_L, NR_EXACT, 0, 0 }, { "upperarmtwist", CR_UPPER_ARM_TWIST_L, NR_NUM, 1, 1 },
    { "lowerarmtwist", CR_FOREARM_TWIST_L, NR_NUM, 1, 1 }, { "thightwist", CR_THIGH_TWIST_L, NR_NUM, 1, 1 },
    { "calftwist", CR_SHIN_TWIST_L, NR_NUM, 1, 1 }, FINGERS("thumb", "index", "middle", "ring", "pinky"),
    { "facialleye", CR_EYE_L, NR_EXACT, 0, 0 }, { "facialreye", CR_EYE_L, NR_EXACT, 0, 0 }, { "facialcjaw", CR_JAW, NR_EXACT, 0, 0 },
};
// Unity's Humanoid (the names of its avatar: LeftUpperArm, LeftLowerLeg, LeftToes, Left Index Proximal...), which many
// exporters and VRM tools write as bone names. VRM 1.0 names the thumb Metacarpal / Proximal / Distal.
static const NameRule RULES_UNITY[] = {
    { "hips", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "chest", 0, NR_SPINE, 0, 0 },
    { "upperchest", 0, NR_SPINE, 0, 0 },  { "neck", CR_NECK1, NR_EXACT, 0, 0 }, { "head", CR_HEAD, NR_EXACT, 0, 0 },
    { "jaw", CR_JAW, NR_EXACT, 0, 0 },    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "lowerarm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "upperleg", CR_THIGH_L, NR_EXACT, 0, 0 }, { "lowerleg", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toes", CR_TOE_L, NR_EXACT, 0, 0 }, { "toebase", CR_TOE_L, NR_EXACT, 0, 0 },
    { "thumbmetacarpal", CR_THUMB1_L, NR_EXACT, 0, 0 }, { "thumbproximal", CR_THUMB1_L, NR_EXACT, 0, 0 },
    { "thumbintermediate", CR_THUMB2_L, NR_EXACT, 0, 0 }, { "thumbdistal", CR_THUMB3_L, NR_EXACT, 0, 0 },
    { "indexproximal", CR_INDEX1_L, NR_EXACT, 0, 0 },   { "indexintermediate", CR_INDEX2_L, NR_EXACT, 0, 0 },   { "indexdistal", CR_INDEX3_L, NR_EXACT, 0, 0 },
    { "middleproximal", CR_MIDDLE1_L, NR_EXACT, 0, 0 }, { "middleintermediate", CR_MIDDLE2_L, NR_EXACT, 0, 0 }, { "middledistal", CR_MIDDLE3_L, NR_EXACT, 0, 0 },
    { "ringproximal", CR_RING1_L, NR_EXACT, 0, 0 },     { "ringintermediate", CR_RING2_L, NR_EXACT, 0, 0 },     { "ringdistal", CR_RING3_L, NR_EXACT, 0, 0 },
    { "littleproximal", CR_PINKY1_L, NR_EXACT, 0, 0 },  { "littleintermediate", CR_PINKY2_L, NR_EXACT, 0, 0 },  { "littledistal", CR_PINKY3_L, NR_EXACT, 0, 0 },
};
static const NameRule RULES_VROID[] = {
    { "hips", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "chest", 0, NR_SPINE, 0, 0 },
    { "upperchest", 0, NR_SPINE, 0, 0 },  { "neck", CR_NECK1, NR_EXACT, 0, 0 }, { "head", CR_HEAD, NR_EXACT, 0, 0 },
    { "faceeye", CR_EYE_L, NR_EXACT, 0, 0 }, { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "lowerarm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "upperleg", CR_THIGH_L, NR_EXACT, 0, 0 }, { "lowerleg", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toebase", CR_TOE_L, NR_EXACT, 0, 0 }, FINGERS("thumb", "index", "middle", "ring", "little"),
};
static const NameRule RULES_CC[] = {
    { "hip", CR_HIPS, NR_EXACT, 0, 0 },   { "waist", 0, NR_SPINE, 0, 0 },      { "spine", 0, NR_SPINE, 0, 0 },
    { "necktwist", CR_NECK1, NR_NUM, 1, 2 }, { "head", CR_HEAD, NR_EXACT, 0, 0 }, { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 }, { "calf", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toebase", CR_TOE_L, NR_EXACT, 0, 0 }, { "upperarmtwist", CR_UPPER_ARM_TWIST_L, NR_NUM, 1, 1 },
    { "forearmtwist", CR_FOREARM_TWIST_L, NR_NUM, 1, 1 }, { "thightwist", CR_THIGH_TWIST_L, NR_NUM, 1, 1 },
    { "calftwist", CR_SHIN_TWIST_L, NR_NUM, 1, 1 }, FINGERS("thumb", "index", "mid", "ring", "pinky"),
    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "jawroot", CR_JAW, NR_EXACT, 0, 0 },
};
// 3ds Max Biped: the fingers are Finger0 (thumb) to Finger4, their next joints Finger01, Finger02...
static const NameRule RULES_BIPED[] = {
    { "pelvis", CR_HIPS, NR_EXACT, 0, 0 }, { "spine", 0, NR_SPINE, 0, 0 },      { "neck", CR_NECK1, NR_EXACT, 0, 0 },
    { "neck", CR_NECK2, NR_NUM, 1, 1 },    { "head", CR_HEAD, NR_EXACT, 0, 0 }, { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 }, { "calf", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toe0", CR_TOE_L, NR_EXACT, 0, 0 },
    { "finger0", CR_THUMB1_L, NR_EXACT, 0, 0 },  { "finger01", CR_THUMB2_L, NR_EXACT, 0, 0 },  { "finger02", CR_THUMB3_L, NR_EXACT, 0, 0 },
    { "finger1", CR_INDEX1_L, NR_EXACT, 0, 0 },  { "finger11", CR_INDEX2_L, NR_EXACT, 0, 0 },  { "finger12", CR_INDEX3_L, NR_EXACT, 0, 0 },
    { "finger2", CR_MIDDLE1_L, NR_EXACT, 0, 0 }, { "finger21", CR_MIDDLE2_L, NR_EXACT, 0, 0 }, { "finger22", CR_MIDDLE3_L, NR_EXACT, 0, 0 },
    { "finger3", CR_RING1_L, NR_EXACT, 0, 0 },   { "finger31", CR_RING2_L, NR_EXACT, 0, 0 },   { "finger32", CR_RING3_L, NR_EXACT, 0, 0 },
    { "finger4", CR_PINKY1_L, NR_EXACT, 0, 0 },  { "finger41", CR_PINKY2_L, NR_EXACT, 0, 0 },  { "finger42", CR_PINKY3_L, NR_EXACT, 0, 0 },
};
static const NameRule RULES_DAZ[] = {
    { "hip", CR_HIPS, NR_EXACT, 0, 0 },   { "abdomenlower", 0, NR_SPINE, 0, 0 }, { "abdomenupper", 0, NR_SPINE, 0, 0 },
    { "chestlower", 0, NR_SPINE, 0, 0 },  { "chestupper", 0, NR_SPINE, 0, 0 },   { "necklower", CR_NECK1, NR_EXACT, 0, 0 },
    { "neckupper", CR_NECK2, NR_EXACT, 0, 0 }, { "head", CR_HEAD, NR_EXACT, 0, 0 }, { "lowerjaw", CR_JAW, NR_EXACT, 0, 0 },
    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "collar", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "shldrbend", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "shldrtwist", CR_UPPER_ARM_TWIST_L, NR_EXACT, 0, 0 },
    { "forearmbend", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "forearmtwist", CR_FOREARM_TWIST_L, NR_EXACT, 0, 0 },
    { "hand", CR_HAND_L, NR_EXACT, 0, 0 }, { "thighbend", CR_THIGH_L, NR_EXACT, 0, 0 }, { "thightwist", CR_THIGH_TWIST_L, NR_EXACT, 0, 0 },
    { "shin", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 }, { "toe", CR_TOE_L, NR_EXACT, 0, 0 },
    FINGERS("thumb", "index", "mid", "ring", "pinky"),
    // Genesis 9: spine1..4, neck1/2, l_shoulder (the clavicle), l_upperarm with l_upperarmtwist1/2, l_thigh, l_toes
    { "spine", 0, NR_SPINE, 0, 0 },       { "neck", CR_NECK1, NR_NUM, 1, 2 },    { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "upperarmtwist", CR_UPPER_ARM_TWIST_L, NR_NUM, 1, 1 },
    { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 },    { "forearmtwist", CR_FOREARM_TWIST_L, NR_NUM, 1, 1 },
    { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 },        { "thightwist", CR_THIGH_TWIST_L, NR_NUM, 1, 1 }, { "toes", CR_TOE_L, NR_EXACT, 0, 0 },
};
static const NameRule RULES_SMPL[] = {
    { "pelvis", CR_HIPS, NR_EXACT, 0, 0 }, { "spine", 0, NR_SPINE, 0, 0 },      { "neck", CR_NECK1, NR_EXACT, 0, 0 },
    { "head", CR_HEAD, NR_EXACT, 0, 0 },   { "collar", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "shoulder", CR_UPPER_ARM_L, NR_EXACT, 0, 0 },
    { "elbow", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "wrist", CR_HAND_L, NR_EXACT, 0, 0 }, { "hip", CR_THIGH_L, NR_EXACT, 0, 0 },
    { "knee", CR_SHIN_L, NR_EXACT, 0, 0 }, { "ankle", CR_FOOT_L, NR_EXACT, 0, 0 }, { "foot", CR_TOE_L, NR_EXACT, 0, 0 },
};
// Auto-Rig Pro (Blender add-on): the deforming bones of its limbs are the "_stretch" ones, the middle ends in ".x",
// the hips are "root.x".
static const NameRule RULES_ARP[] = {
    { "root", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "neck", CR_NECK1, NR_EXACT, 0, 0 },
    { "head", CR_HEAD, NR_EXACT, 0, 0 },  { "jawbone", CR_JAW, NR_EXACT, 0, 0 }, { "eye", CR_EYE_L, NR_EXACT, 0, 0 },
    { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "armstretch", CR_UPPER_ARM_L, NR_EXACT, 0, 0 },
    { "forearmstretch", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "hand", CR_HAND_L, NR_EXACT, 0, 0 },
    { "thighstretch", CR_THIGH_L, NR_EXACT, 0, 0 }, { "legstretch", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toes01", CR_TOE_L, NR_EXACT, 0, 0 }, { "armtwist", CR_UPPER_ARM_TWIST_L, NR_EXACT, 0, 0 },
    { "forearmtwist", CR_FOREARM_TWIST_L, NR_EXACT, 0, 0 }, { "thightwist", CR_THIGH_TWIST_L, NR_EXACT, 0, 0 },
    { "legtwist", CR_SHIN_TWIST_L, NR_EXACT, 0, 0 }, FINGERS("thumb", "index", "middle", "ring", "pinky"),
};
// AdvancedSkeleton (Maya): joints named after the body's joints, "_M" in the middle ("Shoulder_L" is the upper arm).
static const NameRule RULES_ADVSKEL[] = {
    { "root", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "chest", 0, NR_SPINE, 0, 0 },
    { "neck", CR_NECK1, NR_EXACT, 0, 0 }, { "head", CR_HEAD, NR_EXACT, 0, 0 },  { "jaw", CR_JAW, NR_EXACT, 0, 0 },
    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "scapula", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "shoulder", CR_UPPER_ARM_L, NR_EXACT, 0, 0 },
    { "elbow", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "wrist", CR_HAND_L, NR_EXACT, 0, 0 }, { "hip", CR_THIGH_L, NR_EXACT, 0, 0 },
    { "knee", CR_SHIN_L, NR_EXACT, 0, 0 }, { "ankle", CR_FOOT_L, NR_EXACT, 0, 0 }, { "toes", CR_TOE_L, NR_EXACT, 0, 0 },
    FINGERS("thumbfinger", "indexfinger", "middlefinger", "ringfinger", "pinkyfinger"),
};
// MakeHuman / MPFB's default skeleton: two bones per limb segment (the second one is its twist), spine05 at the bottom.
static const NameRule RULES_MPFB[] = {
    { "root", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },      { "neck", CR_NECK1, NR_NUM, 1, 2 },
    { "head", CR_HEAD, NR_EXACT, 0, 0 },  { "jaw", CR_JAW, NR_EXACT, 0, 0 },   { "eye", CR_EYE_L, NR_EXACT, 0, 0 },
    { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "upperarm", CR_UPPER_ARM_L, NR_NUM, 1, 1 },
    { "upperarm", CR_UPPER_ARM_TWIST_L, NR_NUM, 2, 1 }, { "lowerarm", CR_FOREARM_L, NR_NUM, 1, 1 },
    { "lowerarm", CR_FOREARM_TWIST_L, NR_NUM, 2, 1 }, { "wrist", CR_HAND_L, NR_EXACT, 0, 0 },
    { "upperleg", CR_THIGH_L, NR_NUM, 1, 1 }, { "upperleg", CR_THIGH_TWIST_L, NR_NUM, 2, 1 }, { "lowerleg", CR_SHIN_L, NR_NUM, 1, 1 },
    { "lowerleg", CR_SHIN_TWIST_L, NR_NUM, 2, 1 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 }, { "toe11", CR_TOE_L, NR_EXACT, 0, 0 },
    { "finger1", CR_THUMB1_L, NR_NUM, 1, 3 }, { "finger2", CR_INDEX1_L, NR_NUM, 1, 3 }, { "finger3", CR_MIDDLE1_L, NR_NUM, 1, 3 },
    { "finger4", CR_RING1_L, NR_NUM, 1, 3 }, { "finger5", CR_PINKY1_L, NR_NUM, 1, 3 },
};
// Valve's Source and Source Filmmaker skeletons (bip_pelvis, bip_collar_L, bip_upperArm_L, bip_hip_L the thigh,
// bip_knee_L the shin); the fingers' numbering changes between files (bip_index_0 or _1): left to the shape.
static const NameRule RULES_VALVE[] = {
    { "pelvis", CR_HIPS, NR_EXACT, 0, 0 }, { "cog", CR_HIPS, NR_EXACT, 0, 0 },  { "spine", 0, NR_SPINE, 0, 0 },
    { "neck", CR_NECK1, NR_EXACT, 0, 0 },  { "neck", CR_NECK1, NR_NUM, 0, 2 },   { "head", CR_HEAD, NR_EXACT, 0, 0 },
    { "collar", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "lowerarm", CR_FOREARM_L, NR_EXACT, 0, 0 },
    { "hand", CR_HAND_L, NR_EXACT, 0, 0 }, { "hip", CR_THIGH_L, NR_EXACT, 0, 0 }, { "knee", CR_SHIN_L, NR_EXACT, 0, 0 },
    { "foot", CR_FOOT_L, NR_EXACT, 0, 0 }, { "toe", CR_TOE_L, NR_EXACT, 0, 0 },  { "toe", CR_TOE_L, NR_NUM, 0, 1 },
};
// Common words, when no convention above fits: lower confidence.
static const NameRule RULES_WORDS[] = {
    { "root", CR_ROOT, NR_EXACT, 0, 0 },  { "hips", CR_HIPS, NR_EXACT, 0, 0 },  { "hip", CR_HIPS, NR_EXACT, 0, 0 },
    { "pelvis", CR_HIPS, NR_EXACT, 0, 0 }, { "cog", CR_HIPS, NR_EXACT, 0, 0 }, // the center of gravity of game rigs
    { "spine", 0, NR_SPINE, 0, 0 },      { "torso", 0, NR_SPINE, 0, 0 },
    { "abdomen", 0, NR_SPINE, 0, 0 },     { "chest", 0, NR_SPINE, 0, 0 },       { "upperchest", 0, NR_SPINE, 0, 0 },
    { "neck", CR_NECK1, NR_EXACT, 0, 0 }, { "head", CR_HEAD, NR_EXACT, 0, 0 },  { "jaw", CR_JAW, NR_EXACT, 0, 0 },
    { "eye", CR_EYE_L, NR_EXACT, 0, 0 },  { "clavicle", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "collar", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "clav", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "shoulder", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "upperarm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "arm", CR_UPPER_ARM_L, NR_EXACT, 0, 0 },
    { "forearm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "lowerarm", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "elbow", CR_FOREARM_L, NR_EXACT, 0, 0 },
    { "hand", CR_HAND_L, NR_EXACT, 0, 0 }, { "wrist", CR_HAND_L, NR_EXACT, 0, 0 }, { "thigh", CR_THIGH_L, NR_EXACT, 0, 0 },
    { "upperleg", CR_THIGH_L, NR_EXACT, 0, 0 }, { "upleg", CR_THIGH_L, NR_EXACT, 0, 0 }, { "shin", CR_SHIN_L, NR_EXACT, 0, 0 },
    { "calf", CR_SHIN_L, NR_EXACT, 0, 0 }, { "lowerleg", CR_SHIN_L, NR_EXACT, 0, 0 }, { "leg", CR_SHIN_L, NR_EXACT, 0, 0 },
    { "knee", CR_SHIN_L, NR_EXACT, 0, 0 }, { "foot", CR_FOOT_L, NR_EXACT, 0, 0 }, { "ankle", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "toe", CR_TOE_L, NR_EXACT, 0, 0 },  { "toes", CR_TOE_L, NR_EXACT, 0, 0 }, { "toebase", CR_TOE_L, NR_EXACT, 0, 0 },
    { "ball", CR_TOE_L, NR_EXACT, 0, 0 }, FINGERS("thumb", "index", "middle", "ring", "pinky"),
    { "little", CR_PINKY1_L, NR_NUM, 1, 3 },
    // a limb in numbered pieces (rigs of a studio: "Arm1" the upper arm, "Arm2" the forearm, "Leg1" the thigh)
    { "arm", CR_UPPER_ARM_L, NR_NUM, 1, 2 }, { "leg", CR_THIGH_L, NR_NUM, 1, 2 },
    // Portuguese (the accents read as plain letters: "Cabeça" is "cabeca", "Mão" "mao")
    { "quadril", CR_HIPS, NR_EXACT, 0, 0 }, { "bacia", CR_HIPS, NR_EXACT, 0, 0 }, { "coluna", 0, NR_SPINE, 0, 0 },
    { "torax", 0, NR_SPINE, 0, 0 },       { "peito", 0, NR_SPINE, 0, 0 },     { "tronco", 0, NR_SPINE, 0, 0 },
    { "pescoco", CR_NECK1, NR_EXACT, 0, 0 }, { "cabeca", CR_HEAD, NR_EXACT, 0, 0 }, { "mandibula", CR_JAW, NR_EXACT, 0, 0 },
    { "queixo", CR_JAW, NR_EXACT, 0, 0 }, { "olho", CR_EYE_L, NR_EXACT, 0, 0 }, { "clavicula", CR_CLAVICLE_L, NR_EXACT, 0, 0 },
    { "ombro", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "braco", CR_UPPER_ARM_L, NR_EXACT, 0, 0 }, { "antebraco", CR_FOREARM_L, NR_EXACT, 0, 0 },
    { "cotovelo", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "mao", CR_HAND_L, NR_EXACT, 0, 0 }, { "pulso", CR_HAND_L, NR_EXACT, 0, 0 },
    { "coxa", CR_THIGH_L, NR_EXACT, 0, 0 }, { "perna", CR_THIGH_L, NR_EXACT, 0, 0 }, { "canela", CR_SHIN_L, NR_EXACT, 0, 0 },
    { "joelho", CR_SHIN_L, NR_EXACT, 0, 0 }, { "pe", CR_FOOT_L, NR_EXACT, 0, 0 }, { "tornozelo", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "dedao", CR_TOE_L, NR_EXACT, 0, 0 },
    FINGERS("polegar", "indicador", "medio", "anelar", "minimo"), { "mindinho", CR_PINKY1_L, NR_NUM, 1, 3 },
    // Spanish
    { "cadera", CR_HIPS, NR_EXACT, 0, 0 }, { "pecho", 0, NR_SPINE, 0, 0 },   { "cuello", CR_NECK1, NR_EXACT, 0, 0 },
    { "cabeza", CR_HEAD, NR_EXACT, 0, 0 }, { "hombro", CR_CLAVICLE_L, NR_EXACT, 0, 0 }, { "brazo", CR_UPPER_ARM_L, NR_EXACT, 0, 0 },
    { "antebrazo", CR_FOREARM_L, NR_EXACT, 0, 0 }, { "mano", CR_HAND_L, NR_EXACT, 0, 0 }, { "muslo", CR_THIGH_L, NR_EXACT, 0, 0 },
    { "pierna", CR_SHIN_L, NR_EXACT, 0, 0 }, { "rodilla", CR_SHIN_L, NR_EXACT, 0, 0 }, { "pie", CR_FOOT_L, NR_EXACT, 0, 0 },
    { "pulgar", CR_THUMB1_L, NR_NUM, 1, 3 }, { "indice", CR_INDEX1_L, NR_NUM, 1, 3 }, { "anular", CR_RING1_L, NR_NUM, 1, 3 },
    { "menique", CR_PINKY1_L, NR_NUM, 1, 3 },
};
#define RULES(x) x, (int)(sizeof x / sizeof x[0])
static const Convention CONVENTIONS[] = {
    { "rigify", true, 1.0f, RULES(RULES_RIGIFY) }, { "mixamo", false, 1.0f, RULES(RULES_MIXAMO) }, { "ue", false, 1.0f, RULES(RULES_UE) },
    { "unity", false, 1.0f, RULES(RULES_UNITY) }, // before VRoid: a tie of the two is a Unity rig (a VRoid one has J_Bip_)
    { "vroid", false, 1.0f, RULES(RULES_VROID) },  { "cc", false, 1.0f, RULES(RULES_CC) },         { "biped", false, 1.0f, RULES(RULES_BIPED) },
    { "daz", false, 1.0f, RULES(RULES_DAZ) },      { "smpl", false, 1.0f, RULES(RULES_SMPL) },     { "autorigpro", false, 1.0f, RULES(RULES_ARP) },
    { "advskel", false, 1.0f, RULES(RULES_ADVSKEL) }, { "mpfb", false, 1.0f, RULES(RULES_MPFB) },  { "valve", false, 1.0f, RULES(RULES_VALVE) },
    { "palavras", false, 0.7f, RULES(RULES_WORDS) },
};
#define CONVENTION_COUNT ((int)(sizeof CONVENTIONS / sizeof CONVENTIONS[0]))

int charmap_conventions(const char** out, int cap) {
    for (int i = 0; out && i < CONVENTION_COUNT && i < cap; i++) out[i] = i == 0 ? "tlender / rigify" : CONVENTIONS[i].name;
    return CONVENTION_COUNT;
}

bool charmap_outdated(const Armature* a, const CharMap* m) {
    if (!a || !m) return false;
    int sys = rigsys_detect(a);
    if (sys == RIGSYS_NONE || sys == RIGSYS_TLENDER || sys == RIGSYS_CONTROL) return false;
    if (strcmp(m->convention, rigsys_code(sys)) == 0) return false;
    // a map from the library of source skeletons ("ue5", "smplx"...) or a remembered one stays
    bool analyzer = !m->convention[0] || strcmp(m->convention, "palavras") == 0 || rigsys_from_code(m->convention) != RIGSYS_NONE;
    if (!analyzer) return false;
    for (int r = CR_NONE + 1; r < CR_COUNT; r++)
        if (m->roles[r].bone[0] && m->roles[r].source == CSRC_MANUAL) return false;
    return true;
}

int charmap_role_from_vrm(const char* v) {
    if (!v) return CR_NONE;
    static const struct { const char* name; int role; } center[] = {
        { "hips", CR_HIPS }, { "spine", CR_SPINE1 }, { "chest", CR_SPINE2 }, { "upperChest", CR_CHEST },
        { "neck", CR_NECK1 }, { "head", CR_HEAD },   { "jaw", CR_JAW },      { "leftEye", CR_EYE_L }, { "rightEye", CR_EYE_R },
    };
    for (int i = 0; i < (int)(sizeof center / sizeof center[0]); i++)
        if (strcmp(v, center[i].name) == 0) return center[i].role;
    static const struct { const char* name; int role; } side[] = {
        { "Shoulder", CR_CLAVICLE_L }, { "UpperArm", CR_UPPER_ARM_L }, { "LowerArm", CR_FOREARM_L }, { "Hand", CR_HAND_L },
        { "UpperLeg", CR_THIGH_L },    { "LowerLeg", CR_SHIN_L },      { "Foot", CR_FOOT_L },        { "Toes", CR_TOE_L },
        // VRM 1.0 names the thumb Metacarpal / Proximal / Distal, VRM 0.x Proximal / Intermediate / Distal
        { "ThumbMetacarpal", CR_THUMB1_L }, { "ThumbProximal", CR_THUMB2_L }, { "ThumbIntermediate", CR_THUMB2_L },
        { "ThumbDistal", CR_THUMB3_L },
        { "IndexProximal", CR_INDEX1_L },   { "IndexIntermediate", CR_INDEX2_L },  { "IndexDistal", CR_INDEX3_L },
        { "MiddleProximal", CR_MIDDLE1_L }, { "MiddleIntermediate", CR_MIDDLE2_L }, { "MiddleDistal", CR_MIDDLE3_L },
        { "RingProximal", CR_RING1_L },     { "RingIntermediate", CR_RING2_L },    { "RingDistal", CR_RING3_L },
        { "LittleProximal", CR_PINKY1_L },  { "LittleIntermediate", CR_PINKY2_L }, { "LittleDistal", CR_PINKY3_L },
    };
    int right = strncmp(v, "right", 5) == 0;
    if (!right && strncmp(v, "left", 4) != 0) return CR_NONE;
    const char* rest = v + (right ? 5 : 4);
    for (int i = 0; i < (int)(sizeof side / sizeof side[0]); i++)
        if (strcmp(rest, side[i].name) == 0) return right ? charrole_mirror(side[i].role) : side[i].role;
    return CR_NONE;
}

// --- The skeleton ---------------------------------------------------------------------------------------------------------

typedef struct An {
    const Armature* a;
    int n;
    bool* in;     // the bone is part of the skeleton looked at (the deforming bones of a control rig)
    int* par;     // its parent in the skeleton, -1
    int* depth;   // from its root
    int* size;    // bones of its subtree, itself included
    int* reach;   // the longest way down from it, in bones (1: a leaf)
    bool* acc;    // an accessory or below one (hat, hair, clothes, wings, a weapon): never a limb of the body
    Parsed* names;
    float height, tol;
    float stub_len; // most bones this long: imported without their tails (an FBX of a game skeleton), 0 when not
    vec3 lo, hi;
    CharMap* m;
    int sys; // RigSystem (rig_systems.h)
    const unsigned char* skin; // per bone: it deforms (charmap_analyze_ex), NULL: Bone.deform says
} An;

static bool an_deforms(const An* an, int b) { return an->skin ? an->skin[b] != 0 : an->a->bones[b].deform; }

static int an_children(const An* an, int b, int* out, int cap) {
    int n = 0;
    for (int i = 0; i < an->n; i++)
        if (an->in[i] && an->par[i] == b) {
            if (out && n < cap) out[n] = i;
            n++;
        }
    return n;
}

// The child the chain goes on through: the one with the longest way down (the biggest subtree on a tie); a part of the
// body before an accessory (a sleeve's chain longer than the forearm).
static int an_main_child(const An* an, int b) {
    int best = -1;
    for (int i = 0; i < an->n; i++) {
        if (!an->in[i] || an->par[i] != b) continue;
        if (best >= 0 && an->acc[i] != an->acc[best]) {
            if (!an->acc[i]) best = i;
            continue;
        }
        if (best < 0 || an->reach[i] > an->reach[best] || (an->reach[i] == an->reach[best] && an->size[i] > an->size[best])) best = i;
    }
    return best;
}

// The bones of the skeleton that hang from b, b in it or not: a hand that is a control (not skinned) still carries the
// fingers. The same as an_children when b is in the skeleton.
static int an_kids_full(const An* an, int b, int* out, int cap) {
    int n = 0;
    for (int c = 0; c < an->n; c++) {
        if (!an->in[c] || c == b) continue;
        int p = an->a->bones[c].parent;
        while (p >= 0 && p != b && !an->in[p]) p = an->a->bones[p].parent;
        if (p != b) continue;
        if (out && n < cap) out[n] = c;
        n++;
    }
    return n;
}

// The words of accessories, by the start of a name (lower case, plain letters), and whole names.
static bool accessory_name(const char* raw, const char* base) {
    if (starts_ci(raw, "J_Sec_")) return true; // VRoid's secondary bones (hair, clothes, springs)
    static const char* const starts[] = {
        "chapeu", "capacete", "helmet", "cabelo", "hair", "franja", "tranca", "roupa", "cloth", "camisa", "shirt", "manga", "sleeve",
        "saia", "skirt", "vestido", "dress", "capa", "cape", "manto", "cloak", "casaco", "coat", "jacket", "gravata", "necktie", "cachecol",
        "scarf", "cinto", "belt", "bolsa", "pochete", "mochila", "backpack", "weapon", "espada", "sword", "escudo", "shield", "prop",
        "acessorio", "accessory", "colar", "necklace", "brinco", "earring", "oculos", "glasses", "mascara", "mask", "asa", "wing", "pena",
        "feather", "fita", "ribbon", "laco", "jiggle", "physics", "spring", "breast", "bust", "seio", "armadura", "armor", "armour",
        "wire", "cable", "fio", // the wires hanging from the animatronics' limbs (the FNAF rigs of NovaBandit: under the thigh)
    };
    static const char* const whole[] = { "hat", "cap", "bag", "arma", "gun", "bow" };
    for (int i = 0; i < (int)(sizeof starts / sizeof starts[0]); i++)
        if (strncmp(base, starts[i], strlen(starts[i])) == 0) return true;
    for (int i = 0; i < (int)(sizeof whole / sizeof whole[0]); i++) {
        size_t n = strlen(whole[i]);
        if (strncmp(base, whole[i], n) != 0) continue;
        bool digits = true;
        for (const char* d = base + n; *d; d++) digits = digits && isdigit((unsigned char)*d);
        if (digits) return true;
    }
    return false;
}

static bool an_is_ancestor(const An* an, int anc, int b) {
    for (; b >= 0; b = an->par[b])
        if (b == anc) return true;
    return false;
}

// Two bones mirrored in X: heads at opposite X, the same Y and Z.
static bool an_mirrored(const An* an, int b1, int b2) {
    vec3 p = an->a->bones[b1].head, q = an->a->bones[b2].head;
    return fabsf(p.x) > an->tol * 0.5f && fabsf(p.x + q.x) < an->tol && fabsf(p.y - q.y) < an->tol && fabsf(p.z - q.z) < an->tol;
}

// The mirrored pairs among a bone's children with at least min_reach bones down each; the pair with the deepest
// chains first. Returns how many pairs (left bone, +X, in l[]).
static int an_pairs(const An* an, int b, int min_reach, int* l, int* r, int cap) {
    int kids[64], n = an_children(an, b, kids, 64), pairs = 0;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            if (an->acc[kids[i]] || an->acc[kids[j]]) continue;
            if (!an_mirrored(an, kids[i], kids[j]) || an->reach[kids[i]] < min_reach || an->reach[kids[j]] < min_reach) continue;
            if (pairs >= cap) continue;
            bool i_left = an->a->bones[kids[i]].head.x > 0.0f;
            l[pairs] = i_left ? kids[i] : kids[j];
            r[pairs] = i_left ? kids[j] : kids[i];
            pairs++;
        }
    for (int i = 1; i < pairs; i++) // by reach, the deepest first
        for (int j = i; j > 0 && an->reach[l[j]] > an->reach[l[j - 1]]; j--) {
            int t = l[j]; l[j] = l[j - 1]; l[j - 1] = t;
            t = r[j]; r[j] = r[j - 1]; r[j - 1] = t;
        }
    return pairs;
}

static bool an_build(An* an, const Armature* a, CharMap* m, const unsigned char* skin) {
    memset(an, 0, sizeof *an);
    an->a = a;
    an->skin = skin;
    an->n = a->bone_count;
    an->m = m;
    an->sys = rigsys_detect(a);
    size_t n = (size_t)a->bone_count;
    an->in = calloc(n, sizeof *an->in);
    an->par = malloc(n * sizeof *an->par);
    an->depth = calloc(n, sizeof *an->depth);
    an->size = calloc(n, sizeof *an->size);
    an->reach = calloc(n, sizeof *an->reach);
    an->acc = calloc(n, sizeof *an->acc);
    an->names = calloc(n, sizeof *an->names);
    if (!an->in || !an->par || !an->depth || !an->size || !an->reach || !an->acc || !an->names) return false;
    // a control rig keeps its skeleton in the deforming bones: look only at them when they are a real part of it
    int deform = 0;
    for (int b = 0; b < an->n; b++) deform += an_deforms(an, b);
    bool only_deform = deform >= 5 && deform < an->n;
    an->lo = v3(1e30f, 1e30f, 1e30f);
    an->hi = v3(-1e30f, -1e30f, -1e30f);
    for (int b = 0; b < an->n; b++) {
        an->in[b] = !only_deform || an_deforms(an, b);
        parse_name(a->bones[b].name, &an->names[b], an->sys);
        if (!an->in[b]) continue;
        an->lo = v3_min(an->lo, v3_min(a->bones[b].head, a->bones[b].tail));
        an->hi = v3_max(an->hi, v3_max(a->bones[b].head, a->bones[b].tail));
    }
    for (int b = 0; b < an->n; b++) {
        int p = a->bones[b].parent;
        while (p >= 0 && !an->in[p]) p = a->bones[p].parent;
        an->par[b] = an->in[b] ? p : -1;
    }
    // accessories: by the name of the bone or of one above it (in the whole armature)
    for (int b = 0; b < an->n; b++)
        for (int p = b, guard = 0; p >= 0 && guard <= an->n && !an->acc[b]; p = a->bones[p].parent, guard++)
            an->acc[b] = accessory_name(a->bones[p].name, an->names[p].base);
    vec3 ext = v3_sub(an->hi, an->lo);
    an->height = tl_maxf(ext.z, tl_maxf(ext.x, ext.y));
    if (!(an->height > 1e-6f)) an->height = 1.0f;
    an->tol = 0.02f * an->height;
    // depth, subtree size and reach: parents come before children in an armature only by habit, so iterate to a fixed point
    for (int b = 0; b < an->n; b++) {
        if (!an->in[b]) continue;
        an->size[b] = an->reach[b] = 1;
        for (int p = an->par[b]; p >= 0; p = an->par[p]) an->depth[b]++;
    }
    for (int d = an->n; d >= 0; d--) // deepest first
        for (int b = 0; b < an->n; b++) {
            if (!an->in[b] || an->depth[b] != d || an->par[b] < 0) continue;
            int p = an->par[b];
            an->size[p] += an->size[b];
            if (an->reach[b] + 1 > an->reach[p]) an->reach[p] = an->reach[b] + 1;
        }
    // bones imported without their tails (Valorant's agents, the Source and Maya rigs): every one the same short length
    // along an axis of its own, so the tail says nothing of where the next joint is
    int in_count = 0, best_count = 0;
    for (int b = 0; b < an->n; b++) in_count += an->in[b];
    for (int b = 0; b < an->n; b++) {
        float l = a->bones[b].length;
        if (!an->in[b] || !(l > 1e-6f) || (an->stub_len > 0.0f && fabsf(l - an->stub_len) <= 0.03f * l)) continue;
        int count = 0;
        for (int c = 0; c < an->n; c++) count += an->in[c] && fabsf(a->bones[c].length - l) <= 0.03f * l;
        if (count > best_count) best_count = count, an->stub_len = l;
    }
    if (best_count < 8 || best_count * 10 < in_count * 4 || an->stub_len > 0.1f * an->height) an->stub_len = 0.0f;
    return true;
}

// Two bones that start at the same point (a helper at its bone's joint: Woody's L_arm_1_jnt_is_helper).
static bool an_same_place(const An* an, int b, int c) { return v3_len(v3_sub(an->a->bones[b].head, an->a->bones[c].head)) < 0.05f * an->tol; }

static bool an_stub(const An* an, int b) { return an->stub_len > 0.0f && fabsf(an->a->bones[b].length - an->stub_len) <= 0.03f * an->stub_len; }

// Where a bone ends: its tail, or for a bone without its tail (an_stub) the head of the first bone down its main
// children that starts elsewhere (the next joint; a helper at the same place is passed through).
static vec3 an_tip(const An* an, int b) {
    const Bone* bb = &an->a->bones[b];
    if (!an_stub(an, b) || !an->in[b]) return bb->tail;
    for (int c = an_main_child(an, b), guard = 0; c >= 0 && guard < 8; c = an_main_child(an, c), guard++)
        if (!an_same_place(an, c, b)) return an->a->bones[c].head;
    return bb->tail;
}

static void an_free(An* an) {
    free(an->in);
    free(an->par);
    free(an->depth);
    free(an->size);
    free(an->reach);
    free(an->acc);
    free(an->names);
}

// --- (1) Names ----------------------------------------------------------------------------------------------------------

// The role a parsed name has in a convention (CR_NONE: none); *spine: it is a bone of the spine chain.
static int rule_role(const Convention* cv, const Parsed* p, bool* spine) {
    *spine = false;
    size_t len = strlen(p->base);
    if (!len) return CR_NONE;
    for (int i = 0; i < cv->count; i++) {
        const NameRule* r = &cv->rules[i];
        if (r->kind == NR_HEADS) continue;
        size_t wl = strlen(r->word);
        if (strncmp(p->base, r->word, wl) != 0) continue;
        bool digits = len > wl;
        for (size_t k = wl; k < len; k++) digits = digits && isdigit((unsigned char)p->base[k]);
        if (r->kind == NR_EXACT && len == wl) return r->role;
        if (r->kind == NR_SPINE && (len == wl || digits)) {
            *spine = true;
            return CR_NONE;
        }
        if (r->kind == NR_NUM && digits) {
            int num = atoi(p->base + wl);
            if (num >= r->first && num < r->first + r->count) return r->role + (num - r->first);
        }
    }
    return CR_NONE;
}

// A better bone for a role than the one it has: a DEF- bone, then a deforming one, then not a helper, then higher up.
static bool better_bone(const An* an, int b, int old) {
    if (old < 0) return true;
    const Parsed *p = &an->names[b], *q = &an->names[old];
    if (p->def != q->def) return p->def; // the legacy Rigify's DEF-f_index.01.L.01 over its ORG-f_index.01.L
    if (p->seg != q->seg) return p->seg < q->seg;
    if (an_deforms(an, b) != an_deforms(an, old)) return an_deforms(an, b);
    if (p->helper != q->helper) return !p->helper;
    return an->depth[b] < an->depth[old];
}

typedef struct NameFit {
    int bone[CR_COUNT];
    int spine[32], spine_count;
    int score;
    int extra; // the other roles it gives (clavicles, fingers, twist bones...): breaks a tie of the main ones
} NameFit;

// An ORG- bone that deforms and has no DEF- bone of the same name.
static bool org_alone(const An* an, int b) {
    const char* name = an->a->bones[b].name;
    if (!starts_ci(name, "ORG-") || !an_deforms(an, b)) return false;
    char twin[TL_BONE_NAME_LEN];
    snprintf(twin, sizeof twin, "DEF-%s", name + 4);
    return armature_find_bone(an->a, twin) < 0;
}

static void fit_convention(const An* an, const Convention* cv, NameFit* fit) {
    memset(fit, 0, sizeof *fit);
    for (int r = 0; r < CR_COUNT; r++) fit->bone[r] = -1;
    // a Rigify rig whose DEF- bones are all there (an FBX of it keeps the controls as bones too): its controls
    // ("hips", "chest", "torso") are not the skeleton
    int defs = 0;
    for (int b = 0; b < an->n && cv->first_spine_is_hips; b++) defs += an->in[b] && an->names[b].def;
    for (int b = 0; b < an->n; b++) {
        if (!an->in[b] || an->acc[b]) continue;
        const Parsed* p = &an->names[b];
        bool spine;
        int role = rule_role(cv, p, &spine);
        // (its ORG- / MCH- bones neither, but for the eyes and the jaw, which only some of them have, and an ORG- bone
        // that deforms with no DEF- twin: Plushtrap's head and neck are ORG-spine.005/.006, there is no DEF-spine.004)
        if (defs >= 8 && !p->def && (!p->helper || spine || (role != CR_EYE_L && role != CR_JAW)) && !(p->helper && org_alone(an, b))) continue;
        if (spine && !p->side) {
            if (p->seg == 0 && fit->spine_count < 32) fit->spine[fit->spine_count++] = b;
            continue;
        }
        if (role == CR_NONE) continue;
        bool sided = charrole_side(role) != 0;
        if (sided) {
            int side = p->side;
            if (!side) { // no side in the name: by the rest pose (+X is the left)
                float x = an->a->bones[b].head.x;
                side = x > an->tol * 0.5f ? 1 : x < -an->tol * 0.5f ? 2 : 0;
            }
            if (!side) continue;
            if (side == 2) role = charrole_mirror(role);
        } else if (p->side) continue;
        // the second segment of a limb bone ("upper_arm.L.001") is its twist bone; the first half of a finger bone of
        // the legacy Rigify ("f_index.01.L.01") is the finger bone
        if (p->seg > 0) {
            if (!sided || p->seg > 1) continue;
            if (!charrole_is_finger(role)) {
                int k = (role - CR_SIDE_FIRST) % CR_SIDE_COUNT;
                int twist = k == 1 ? 23 : k == 2 ? 24 : k == 19 ? 25 : k == 20 ? 26 : -1;
                if (twist < 0) continue;
                role = role - k + twist;
            }
        }
        if (better_bone(an, b, fit->bone[role])) fit->bone[role] = b;
    }
    // the spine chain, bottom to top: by depth; when its bones are not one chain (the bones of a control rig that are
    // not parented to each other: Roxanne's Spine1_jnt, Plushtrap's ORG-spine.005 under a control) by the number when
    // they are all "spine" and a number ("Spine", "Spine1"; MakeHuman numbers its chain from the top, spine05 at the
    // bottom, but that one is a chain)
    int key[32];
    bool chained = true;
    for (int i = 0; i < fit->spine_count && chained; i++)
        for (int j = i + 1; j < fit->spine_count && chained; j++)
            chained = an_is_ancestor(an, fit->spine[i], fit->spine[j]) || an_is_ancestor(an, fit->spine[j], fit->spine[i]);
    bool numbered = fit->spine_count > 1 && !chained;
    for (int i = 0; i < fit->spine_count; i++) {
        const char* base = an->names[fit->spine[i]].base;
        bool ok = strncmp(base, "spine", 5) == 0;
        for (const char* d = base + 5; ok && *d; d++) ok = isdigit((unsigned char)*d) != 0;
        numbered = numbered && ok;
        key[i] = ok ? atoi(base + 5) : 0;
    }
    for (int i = 0; i < fit->spine_count; i++)
        if (!numbered) key[i] = an->depth[fit->spine[i]];
    for (int i = 1; i < fit->spine_count; i++)
        for (int j = i; j > 0 && key[j] < key[j - 1]; j--) {
            int k = key[j];
            key[j] = key[j - 1];
            key[j - 1] = k;
            int t = fit->spine[j];
            fit->spine[j] = fit->spine[j - 1];
            fit->spine[j - 1] = t;
        }
    // by the number, a copy of a lower one in the same place is left out (Zelda's DEF-spine.007/.008 repeat .005/.006)
    for (int i = 0, kept = 0, n = fit->spine_count; numbered && i < n; i++) {
        const Bone* bi = &an->a->bones[fit->spine[i]];
        bool copy = false;
        for (int j = 0; j < kept && !copy; j++) {
            const Bone* bj = &an->a->bones[fit->spine[j]];
            copy = v3_len(v3_sub(bi->head, bj->head)) < 0.05f * an->tol && v3_len(v3_sub(bi->tail, bj->tail)) < 0.05f * an->tol;
        }
        if (!copy) fit->spine[kept++] = fit->spine[i];
        if (i == n - 1) fit->spine_count = kept;
    }
    for (int r = CR_NONE + 1; r < CR_COUNT; r++)
        if (charrole_main(r) && r != CR_SPINE1 && fit->bone[r] >= 0) fit->score++;
        else if (fit->bone[r] >= 0) fit->extra++;
    if (fit->spine_count > (cv->first_spine_is_hips && fit->bone[CR_HIPS] < 0 ? 1 : 0)) fit->score++;
    if (cv->first_spine_is_hips && fit->bone[CR_HIPS] < 0 && fit->spine_count > 0) fit->score++;
}

// The FK control of a skeleton bone in a control rig: the auto-rig's tags, else the rig system's naming (Rigify's
// "upper_arm_fk.L", Auto-Rig Pro's "c_arm_fk.l"...) or what the bone's constraints lead to (rig_systems.h). "" when the
// bone is posed itself.
static void find_control(const Armature* a, int bone, int sys, char* out, size_t cap) {
    out[0] = 0;
    if (a->bones[bone].rig_role == RROLE_DEF && a->bones[bone].rig_module >= 0) {
        static RigModuleInfo info[96];
        int n = autorig_modules(a, info, 96);
        for (int i = 0; i < n; i++)
            for (int k = 0; k < info[i].count && k < TL_RIG_CHAIN; k++)
                if (info[i].def[k] == bone && info[i].fk[k] >= 0) {
                    snprintf(out, cap, "%s", a->bones[info[i].fk[k]].name);
                    return;
                }
    }
    if (sys == RIGSYS_NONE || sys == RIGSYS_TLENDER) {
        // a plain skeleton: only Rigify's naming of a DEF- bone, as before the rig systems
        if (!starts_ci(a->bones[bone].name, "DEF-")) return;
        sys = RIGSYS_RIGIFY;
    }
    // only a bone the rig drives has a control: the same rig exported to an FBX keeps the controls' names but not the
    // constraints, and its controls then move nothing (the bone is posed itself)
    if (a->bones[bone].con_count <= 0) return;
    int c = rigsys_fk_control(a, sys, bone);
    if (c >= 0 && c != bone) snprintf(out, cap, "%s", a->bones[c].name);
}

void charmap_find_control(const Armature* a, int bone, char* out, size_t cap) {
    if (cap) out[0] = 0;
    if (a && bone >= 0 && bone < a->bone_count && cap) find_control(a, bone, rigsys_detect(a), out, cap);
}

static void an_set(An* an, int role, int bone, float conf, int source) {
    if (role <= CR_NONE || role >= CR_COUNT || bone < 0) return;
    if (an->m->roles[role].bone[0]) return; // what an earlier step (or the user) gave stays
    if (charmap_role_of_bone(an->m, an->a->bones[bone].name) != CR_NONE) return;
    char control[TL_BONE_NAME_LEN];
    find_control(an->a, bone, an->sys, control, sizeof control);
    charmap_set_role(an->m, role, an->a->bones[bone].name, control, conf, source);
}

// The spine chain's bones onto hips, spine 1..4 and chest; in Rigify's naming also the neck and the head from its end.
static void place_spine(An* an, const Convention* cv, NameFit* fit, int source) {
    int* sp = fit->spine;
    int n = fit->spine_count, at = 0;
    if (!n) return;
    float conf = cv->confidence * 0.95f;
    if (cv->first_spine_is_hips && !an->m->roles[CR_HIPS].bone[0]) an_set(an, CR_HIPS, sp[at++], conf, source);
    // the chain bone the arms hang from is the chest; what comes after it is neck and head
    int chest = n - 1;
    int arm = charmap_bone(an->a, an->m, CR_CLAVICLE_L);
    if (arm < 0) arm = charmap_bone(an->a, an->m, CR_UPPER_ARM_L);
    bool by_parent = false;
    for (int i = n - 1; arm >= 0 && i >= at; i--)
        if (an_is_ancestor(an, sp[i], arm)) {
            chest = i;
            by_parent = true;
            break;
        }
    if (arm >= 0 && !by_parent) {
        // the deforming bones of some control rigs (Rigify) are not parented to each other: the chest is the chain
        // bone beside the shoulder joint (the first, from the bottom, whose length the joint projects onto)
        int joint = charmap_bone(an->a, an->m, CR_UPPER_ARM_L);
        vec3 p = an->a->bones[joint >= 0 ? joint : arm].head;
        float best = 1e30f;
        for (int i = at; i < n; i++) {
            vec3 h = an->a->bones[sp[i]].head, d = v3_sub(an->a->bones[sp[i]].tail, h);
            float len2 = v3_dot(d, d), t = len2 > 1e-12f ? v3_dot(v3_sub(p, h), d) / len2 : 0.0f;
            float miss = t < 0.0f ? -t : t > 1.0f ? t - 1.0f : 0.0f;
            if (miss < best) {
                best = miss;
                chest = i;
            }
        }
    }
    bool heads = false;
    for (int i = 0; i < cv->count; i++) heads = heads || cv->rules[i].kind == NR_HEADS;
    if (heads && chest < n - 1 && !an->m->roles[CR_HEAD].bone[0]) {
        an_set(an, CR_HEAD, sp[n - 1], conf, source);
        for (int i = chest + 1, k = 0; i < n - 1 && k < 2; i++, k++) an_set(an, CR_NECK1 + k, sp[i], conf, source);
    } else if (!heads) chest = n - 1;
    int count = chest - at + 1; // the bones between the hips and the neck
    if (count <= 0) return;
    if (count == 1) {
        an_set(an, CR_SPINE1, sp[at], conf, source);
        return;
    }
    an_set(an, CR_CHEST, sp[chest], conf, source);
    for (int i = at, k = 0; i < chest && k < 4; i++, k++) an_set(an, CR_SPINE1 + k, sp[i], conf, source);
}

static int names_pass(An* an, char* convention, size_t cap) {
    static NameFit fit, best;
    int best_cv = -1;
    const char* sys_code = rigsys_code(an->sys);
    for (int c = 0; c < CONVENTION_COUNT; c++) {
        fit_convention(an, &CONVENTIONS[c], &fit);
        // the system the marker bones say wins over a convention that merely shares some words with it
        if (sys_code[0] && strcmp(CONVENTIONS[c].name, sys_code) == 0 && fit.score >= 3) fit.score += 100;
        if (best_cv < 0 || fit.score > best.score || (fit.score == best.score && fit.extra > best.extra)) {
            best = fit;
            best_cv = c;
        }
    }
    if (best_cv < 0 || best.score < 3) return 0;
    const Convention* cv = &CONVENTIONS[best_cv];
    bool tlender = an->a->autorig_rig;
    for (int b = 0; b < an->n && !tlender; b++) tlender = an->a->bones[b].rigmod.type != RMOD_NONE;
    int source = tlender && best_cv == 0 ? CSRC_AUTORIG : CSRC_NAMES;
    // the convention names the rig system when the marker bones found one (a CAT or control rig named with common words)
    bool named_sys = an->sys != RIGSYS_NONE && an->sys != RIGSYS_TLENDER;
    snprintf(convention, cap, "%s", best_cv == 0 && tlender ? "tlender" : named_sys ? sys_code : cv->name);
    for (int r = CR_NONE + 1; r < CR_COUNT; r++)
        if (best.bone[r] >= 0) an_set(an, r, best.bone[r], cv->confidence, source);
    place_spine(an, cv, &best, source);
    return best.score;
}

// --- (1b) The limbs as the hierarchy has them --------------------------------------------------------------------------------
// What the names got right as words but wrong as joints, by how the bones hang (onda RG 3: rigs of the user that did not
// retarget):
//   - a limb bone that hangs from its own twist bone: the twist one sits at the joint (Auto-Rig Pro's thigh_twist above
//     its thigh_stretch, which starts halfway down the thigh) and takes the role;
//   - no forearm but the bone after the upper arm carries the hand ("Arm.L.001", read as the arm's twist): it is the
//     forearm; the same for a missing shin between the thigh and the foot;
//   - no thigh but a "shin" whose next bone goes on down to the foot ("Leg" is the shin in Mixamo's naming, the thigh in
//     many others, with "Knee" or "LowerLeg" below it): the thigh is that one and the shin the next.

static void an_force(An* an, int role, int bone, float conf) {
    char control[TL_BONE_NAME_LEN];
    find_control(an->a, bone, an->sys, control, sizeof control);
    charmap_set_role(an->m, role, an->a->bones[bone].name, control, conf, CSRC_TOPOLOGY);
}

// c goes on from b: its head where b ends and of some length.
static bool an_continues(const An* an, int b, int c) {
    const Bone *bb = &an->a->bones[b], *cc = &an->a->bones[c];
    if (an_stub(an, b)) return an->in[b] && v3_len(v3_sub(cc->head, an_tip(an, b))) < 2.0f * an->tol && !an_same_place(an, b, c);
    if (v3_len(v3_sub(cc->head, bb->tail)) < 2.0f * an->tol && cc->length > 0.25f * bb->length) return true;
    // a bone that points at its child's head and stops short of it (some of Valorant's agents: L_Shoulder ends halfway
    // to L_Elbow): the child starts further down the same line
    vec3 dir = v3_sub(bb->tail, bb->head), to = v3_sub(cc->head, bb->head);
    float ld = v3_len(dir), lt = v3_len(to);
    return cc->parent == b && ld > 1e-6f && lt > ld && v3_dot(dir, to) > 0.97f * ld * lt;
}

// The child of b on the way down to d (d below b), else b's main child when it goes on from b; -1.
static int an_child_toward(const An* an, int b, int d) {
    for (int x = d; x >= 0; x = an->par[x])
        if (an->par[x] == b) {
            // a helper at b's own joint (Woody's L_arm_1_jnt_is_helper, which carries the forearm) is passed through
            if (x != d && an_same_place(an, x, b)) {
                int y = an_child_toward(an, x, d);
                if (y >= 0) return y;
            }
            return x;
        }
    int c = an_main_child(an, b);
    return c >= 0 && an_continues(an, b, c) ? c : -1;
}

static bool arm_is_ancestor(const Armature* a, int anc, int b);
static int an_chain(const An* an, int b, int* out, int cap, bool stop_at_branch);

static void limbs_pass(An* an) {
    const Armature* a = an->a;
    CharMap* m = an->m;
    for (int side = 0; side < 2; side++) {
        int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
        static const int pairs[4][2] = { { CR_UPPER_ARM_L, CR_UPPER_ARM_TWIST_L }, { CR_FOREARM_L, CR_FOREARM_TWIST_L }, { CR_THIGH_L, CR_THIGH_TWIST_L },
                                         { CR_SHIN_L, CR_SHIN_TWIST_L } };
        for (int k = 0; k < 4; k++) {
            int rm = pairs[k][0] + d, rt = pairs[k][1] + d;
            int bm = charmap_bone(a, m, rm), bt = charmap_bone(a, m, rt);
            if (bm < 0 || bt < 0 || m->roles[rm].source == CSRC_MANUAL || !an->in[bm] || !an->in[bt] || !an_is_ancestor(an, bt, bm)) continue;
            float conf = m->roles[rm].confidence;
            an_force(an, rm, bt, conf);
            an_force(an, rt, bm, conf);
        }
        // a missing middle of a limb: the bone from the top one down to the end one
        static const int limbs[2][3] = { { CR_UPPER_ARM_L, CR_FOREARM_L, CR_HAND_L }, { CR_THIGH_L, CR_SHIN_L, CR_FOOT_L } };
        for (int k = 0; k < 2; k++) {
            int top = charmap_bone(a, m, limbs[k][0] + d), mid = charmap_bone(a, m, limbs[k][1] + d), end = charmap_bone(a, m, limbs[k][2] + d);
            if (top < 0 || mid >= 0 || !an->in[top]) continue;
            int c = an_child_toward(an, top, end >= 0 && an->in[end] && an_is_ancestor(an, top, end) ? end : -1);
            if (c < 0 || c == end) continue;
            int had = charmap_role_of_bone(m, a->bones[c].name);
            if (had != CR_NONE && !charrole_is_twist(had)) continue;
            an_force(an, limbs[k][1] + d, c, 0.6f);
        }
        // a "shin" that is the thigh
        int thigh = charmap_bone(a, m, CR_THIGH_L + d), shin = charmap_bone(a, m, CR_SHIN_L + d), foot = charmap_bone(a, m, CR_FOOT_L + d);
        if (thigh < 0 && shin >= 0 && an->in[shin] && m->roles[CR_SHIN_L + d].source != CSRC_MANUAL) {
            int c = an_main_child(an, shin);
            // with the foot known, the child that goes to it (a longer chain beside the leg is not the leg)
            if (foot >= 0) {
                float best = 1e30f;
                for (int i = 0; i < an->n; i++) {
                    if (!an->in[i] || an->par[i] != shin || an->acc[i]) continue;
                    float dd = an_is_ancestor(an, i, foot) ? 0.0f : v3_len(v3_sub(a->bones[foot].head, a->bones[i].tail));
                    if (dd < best) best = dd, c = i;
                }
            }
            const Bone* cb = c >= 0 ? &a->bones[c] : NULL;
            bool down = cb && an_continues(an, shin, c) && cb->tail.z < cb->head.z - an->tol;
            int had = c >= 0 ? charmap_role_of_bone(m, a->bones[c].name) : CR_NONE;
            // the foot is below that next bone (or where it ends), not where the "shin" ends
            bool foot_below = foot < 0 || an_is_ancestor(an, c, foot) || v3_len(v3_sub(a->bones[foot].head, cb ? cb->tail : a->bones[shin].tail)) < 2.0f * an->tol;
            if (down && foot_below && (had == CR_NONE || charrole_is_twist(had) || had == CR_SHIN_L + d)) {
                float conf = m->roles[CR_SHIN_L + d].confidence;
                an_force(an, CR_THIGH_L + d, shin, conf);
                an_force(an, CR_SHIN_L + d, c, conf);
            }
        }
        // a missing top of a limb, named for something else ("EyeBrow.L" is the thigh of NovaBandit's Toy Freddy): the
        // bone with no role the shin (forearm) goes on from, below the hips (hanging from the clavicle), the thigh
        // (upper arm)
        static const int tops[2][3] = { { CR_THIGH_L, CR_SHIN_L, CR_HIPS }, { CR_UPPER_ARM_L, CR_FOREARM_L, CR_CLAVICLE_L } };
        for (int k = 0; k < 2; k++) {
            int top_role = tops[k][0] + d, mid = charmap_bone(a, m, tops[k][1] + d);
            int above = charmap_bone(a, m, tops[k][2] + (k == 1 ? d : 0));
            if (charmap_bone(a, m, top_role) >= 0 || mid < 0 || above < 0 || m->roles[tops[k][1] + d].source == CSRC_MANUAL) continue;
            int p = a->bones[mid].parent;
            if (p < 0 || p == above || !an_continues(an, p, mid) || charmap_role_of_bone(m, a->bones[p].name) != CR_NONE) continue;
            if (k == 0 ? !an_is_ancestor(an, above, p) || an_tip(an, p).z >= a->bones[p].head.z - an->tol
                       : a->bones[p].parent != above)
                continue;
            an_force(an, top_role, p, 0.6f);
        }
        // the "shoulder" that is the upper arm (Zaki's FNaF 1 rigs, the Pianosaurus: L_Shoulder > L_Elbow > L_Wrist,
        // no clavicle): the forearm hangs from the clavicle, which starts away from the trunk bone it hangs on (a
        // clavicle starts at the middle of the body)
        int clav = charmap_bone(a, m, CR_CLAVICLE_L + d), fore = charmap_bone(a, m, CR_FOREARM_L + d);
        if (charmap_bone(a, m, CR_UPPER_ARM_L + d) < 0 && clav >= 0 && fore >= 0 && a->bones[fore].parent == clav &&
            m->roles[CR_CLAVICLE_L + d].source != CSRC_MANUAL && an_continues(an, clav, fore)) {
            int q = a->bones[clav].parent;
            int qrole = q >= 0 ? charmap_role_of_bone(m, a->bones[q].name) : CR_NONE;
            if (q >= 0 && ((qrole >= CR_HIPS && qrole <= CR_CHEST) || qrole == CR_NECK1)) {
                vec3 h = a->bones[q].head, dq = v3_sub(an_tip(an, q), h), pc = a->bones[clav].head;
                float l2 = v3_dot(dq, dq), t = l2 > 1e-12f ? tl_minf(tl_maxf(v3_dot(v3_sub(pc, h), dq) / l2, 0.0f), 1.0f) : 0.0f;
                float off = v3_len(v3_sub(pc, v3_add(h, v3_scale(dq, t))));
                float len = v3_len(v3_sub(a->bones[fore].head, pc));
                if (off > 0.5f * len && off > an->tol) {
                    float conf = m->roles[CR_CLAVICLE_L + d].confidence;
                    memset(&m->roles[CR_CLAVICLE_L + d], 0, sizeof m->roles[0]);
                    an_force(an, CR_UPPER_ARM_L + d, clav, conf);
                }
            }
        }
    }
}

// A limb bone that starts past its joint (Auto-Rig Pro's arm_stretch starts two thirds down the upper arm, under the
// arm_twist that sits at the shoulder): the bone above it, in the whole armature (skinned or not), that starts where
// the role above ends and has no role takes it, so that turning it turns every piece of the segment.
static void joints_pass(An* an) {
    const Armature* a = an->a;
    CharMap* m = an->m;
    static const int limb[3][2] = { { CR_CLAVICLE_L, CR_UPPER_ARM_L }, { CR_UPPER_ARM_L, CR_FOREARM_L }, { CR_THIGH_L, CR_SHIN_L } };
    for (int side = 0; side < 2; side++)
        for (int k = 0; k < 3; k++) {
            int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
            int up = limb[k][0] + d, role = limb[k][1] + d;
            int q = charmap_bone(a, m, up), b = charmap_bone(a, m, role);
            if (q < 0 || b < 0 || m->roles[role].source == CSRC_MANUAL) continue;
            vec3 joint = an_tip(an, q);
            if (v3_len(v3_sub(a->bones[b].head, joint)) < an->tol) continue;
            for (int p = a->bones[b].parent, guard = 0; p >= 0 && p != q && guard < 8; p = a->bones[p].parent, guard++) {
                if (charmap_role_of_bone(m, a->bones[p].name) != CR_NONE) break;
                if (v3_len(v3_sub(a->bones[p].head, joint)) < an->tol) {
                    an_force(an, role, p, m->roles[role].confidence);
                    break;
                }
            }
        }
}

// The end of a limb where the limb ends, whatever hangs it (rigs of a studio, onda "retarget universal"): the hand is the
// forearm's IK target, without a parent or under a control (Child Of), so it is not below the forearm, and sometimes not
// skinned either (the fingers are); the foot under the heel's control. The bone, of the whole armature, whose head is at
// the end of the middle bone and that carries the most of the skeleton (the fingers), the IK target first; for the foot
// the one that points ahead (the heel's bone points back).
static bool an_ik_target(const Armature* a, int limb_bone, int b) {
    for (int x = limb_bone, up = 0; x >= 0 && up < 3; x = a->bones[x].parent, up++)
        for (int c = 0; c < a->bones[x].con_count; c++)
            if (a->bones[x].cons[c].type == CON_IK && strcmp(a->bones[x].cons[c].target, a->bones[b].name) == 0) return true;
    return false;
}

static void ends_pass(An* an) {
    const Armature* a = an->a;
    CharMap* m = an->m;
    for (int side = 0; side < 2; side++)
        for (int k = 0; k < 2; k++) {
            int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
            int mid_role = (k ? CR_SHIN_L : CR_FOREARM_L) + d, end_role = (k ? CR_FOOT_L : CR_HAND_L) + d;
            int mid = charmap_bone(a, m, mid_role);
            // an arm of one piece (BoxyBoo's spring, Arm.L): no forearm, the hand with its fingers where the arm ends
            bool one_piece = mid < 0 && !k && charmap_bone(a, m, CR_UPPER_ARM_L + d) >= 0;
            if (one_piece) mid = charmap_bone(a, m, CR_UPPER_ARM_L + d);
            if (mid < 0 || m->roles[end_role].bone[0]) continue;
            vec3 joint = an_tip(an, mid);
            int best = -1;
            float best_score = 0.0f;
            for (int b = 0; b < an->n; b++) {
                const Bone* bb = &a->bones[b];
                if (b == mid || an->acc[b] || v3_len(v3_sub(bb->head, joint)) > 2.0f * an->tol) continue;
                if (charmap_role_of_bone(m, bb->name) != CR_NONE) continue;
                bool above = false; // a bone above the limb is no end of it
                for (int p = a->bones[mid].parent; p >= 0 && !above; p = a->bones[p].parent) above = p == b;
                if (above) continue;
                int kids = an_kids_full(an, b, NULL, 0);
                if (!an->in[b] && kids == 0) continue; // a control that carries nothing of the skeleton
                if (one_piece && kids < 2) continue;
                float score = 2.0f * (float)(kids < 6 ? kids : 6) + (an->in[b] ? 3.0f : 0.0f) + (an_ik_target(a, mid, b) ? 4.0f : 0.0f);
                if (k) { // the foot goes ahead (-Y) and down to the ball
                    vec3 dir = v3_sub(bb->tail, bb->head);
                    float len = v3_len(dir);
                    if (len > 1e-6f) score += 6.0f * (-dir.y / len) - 2.0f * (dir.z / len);
                }
                if (best < 0 || score > best_score) best = b, best_score = score;
            }
            if (best >= 0 && best_score > 0.0f) an_set(an, end_role, best, 0.6f, CSRC_TOPOLOGY);
        }
    // the head where the neck ends, in the middle, the bone that carries the most of the skeleton (BoxyBoo's
    // head_main_JNT carries no weights of its own, its jaws do)
    int neck = charmap_bone(a, m, CR_NECK2) >= 0 ? charmap_bone(a, m, CR_NECK2) : charmap_bone(a, m, CR_NECK1);
    if (neck >= 0 && !m->roles[CR_HEAD].bone[0]) {
        vec3 joint = an_tip(an, neck);
        int best = -1, best_kids = 0;
        for (int b = 0; b < an->n; b++) {
            const Bone* bb = &a->bones[b];
            if (b == neck || an->acc[b] || fabsf(bb->head.x) > an->tol || v3_len(v3_sub(bb->head, joint)) > 2.0f * an->tol) continue;
            if (charmap_role_of_bone(m, bb->name) != CR_NONE || arm_is_ancestor(a, b, neck)) continue;
            int kids = an_kids_full(an, b, NULL, 0) + (an->in[b] ? 1 : 0);
            if (kids > best_kids) best = b, best_kids = kids;
        }
        if (best >= 0) an_set(an, CR_HEAD, best, 0.5f, CSRC_TOPOLOGY);
    }
}

// A finger with a gap in the names (VRM 1.0's thumb: Metacarpal, Proximal, Distal; a rig with "Index1" and "Index3"
// only): the bone between the two named ones, else the next one down, takes the missing role.
static void fingers_fill(An* an) {
    static const int first[5] = { CR_THUMB1_L, CR_INDEX1_L, CR_MIDDLE1_L, CR_RING1_L, CR_PINKY1_L };
    const Armature* a = an->a;
    for (int side = 0; side < 2; side++)
        for (int f = 0; f < 5; f++)
            for (int k = 1; k < 3; k++) {
                int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
                int role = first[f] + k + d, prev = charmap_bone(a, an->m, role - 1);
                if (an->m->roles[role].bone[0] || prev < 0 || !an->in[prev]) continue;
                int next = k < 2 ? charmap_bone(a, an->m, role + 1) : -1;
                int c = an_child_toward(an, prev, next >= 0 && an->in[next] && an_is_ancestor(an, prev, next) ? next : -1);
                if (c < 0 || c == next || an->acc[c]) continue;
                an_set(an, role, c, 0.6f, CSRC_TOPOLOGY);
            }
}

// The hips are where the legs and the spine meet: when the bone above both thighs and the chest is a spine bone and
// the hips are below it (a rig whose "Spine" carries the torso and a pelvis control under it, which carries the legs),
// it becomes the hips and the spine is what is above it.
static void trunk_pass(An* an) {
    const Armature* a = an->a;
    CharMap* m = an->m;
    int tl = charmap_bone(a, m, CR_THIGH_L), tr = charmap_bone(a, m, CR_THIGH_R), top = -1, hips = charmap_bone(a, m, CR_HIPS);
    for (int r = CR_SPINE1; r <= CR_CHEST; r++)
        if (charmap_bone(a, m, r) >= 0) top = charmap_bone(a, m, r);
    if (tl < 0 || tr < 0 || top < 0 || !an->in[tl] || !an->in[tr] || !an->in[top]) return;
    int lca = -1;
    for (int x = an->par[tl]; x >= 0 && lca < 0; x = an->par[x])
        if (an_is_ancestor(an, x, tr) && an_is_ancestor(an, x, top)) lca = x;
    if (lca < 0 || lca == hips || lca == top) return;
    int role = charmap_role_of_bone(m, a->bones[lca].name);
    if (role < CR_SPINE1 || role > CR_CHEST) return;
    if (hips >= 0 && !an_is_ancestor(an, lca, hips)) return;
    for (int r = CR_HIPS; r <= CR_CHEST; r++)
        if (m->roles[r].source == CSRC_MANUAL) return;
    float conf = m->roles[role].confidence;
    int sp[5], n = 0;
    for (int r = CR_SPINE1; r <= CR_CHEST; r++) {
        int b = charmap_bone(a, m, r);
        if (b >= 0 && b != lca && an_is_ancestor(an, lca, b) && n < 5) sp[n++] = b;
        memset(&m->roles[r], 0, sizeof m->roles[r]);
    }
    memset(&m->roles[CR_HIPS], 0, sizeof m->roles[CR_HIPS]);
    an_force(an, CR_HIPS, lca, conf);
    if (n == 1) an_force(an, CR_SPINE1, sp[0], conf);
    else if (n > 1) {
        an_force(an, CR_CHEST, sp[n - 1], conf);
        for (int i = 0; i < n - 1 && i < 4; i++) an_force(an, CR_SPINE1 + i, sp[i], conf);
    }
}

// Hips by name beside the spine ("Hip" under the Pelvis carries only the legs, the spine hangs from the Pelvis too: the
// Puppet of NovaBandit): with no spine yet, the bone above them that also carries the neck is the hips, and the bones
// from it to the neck the spine (the last one the chest).
static bool arm_is_ancestor(const Armature* a, int anc, int b) {
    for (int n = 0; b >= 0 && n <= a->bone_count; b = a->bones[b].parent, n++)
        if (b == anc) return true;
    return false;
}

static void shape_leg(An* an, int thigh, int side);

static void hips_beside_spine_pass(An* an) {
    const Armature* a = an->a; // the armature's own hierarchy: the hips and the trunk may carry no weights
    CharMap* m = an->m;
    int hips = charmap_bone(a, m, CR_HIPS), neck = charmap_bone(a, m, CR_NECK1);
    if (neck < 0) neck = charmap_bone(a, m, CR_HEAD);
    if (hips < 0 || neck < 0 || arm_is_ancestor(a, hips, neck)) return;
    for (int r = CR_HIPS; r <= CR_CHEST; r++)
        if ((r != CR_HIPS && m->roles[r].bone[0]) || m->roles[r].source == CSRC_MANUAL) return;
    int p = a->bones[hips].parent;
    if (p < 0 || !arm_is_ancestor(a, p, neck) || charmap_role_of_bone(m, a->bones[p].name) != CR_NONE) return;
    for (int side = 0; side < 2; side++) {
        int t = charmap_bone(a, m, side ? CR_THIGH_R : CR_THIGH_L);
        if (t >= 0 && !arm_is_ancestor(a, hips, t)) return;
    }
    int sp[8], n = 0;
    for (int x = a->bones[neck].parent; x >= 0 && x != p && n < 8; x = a->bones[x].parent) sp[n++] = x;
    if (n == 0 || n >= 8 || a->bones[sp[n - 1]].parent != p) return;
    float conf = m->roles[CR_HIPS].confidence;
    memset(&m->roles[CR_HIPS], 0, sizeof m->roles[CR_HIPS]);
    an_force(an, CR_HIPS, p, conf);
    // the legs the shape would have found under the old hips (its mirrored pair of chains)
    int l[8], r[8];
    if (charmap_bone(a, m, CR_THIGH_L) < 0 && charmap_bone(a, m, CR_THIGH_R) < 0 && an_pairs(an, hips, 2, l, r, 8) >= 1) {
        shape_leg(an, l[0], 1);
        shape_leg(an, r[0], 2);
    }
    if (n == 1) an_force(an, CR_SPINE1, sp[0], conf);
    else {
        an_force(an, CR_CHEST, sp[0], conf); // sp runs from the neck down
        for (int i = n - 1, k = 0; i >= 1 && k < 4; i--, k++) an_force(an, CR_SPINE1 + k, sp[i], conf);
    }
}

// A leg of many even pieces (the rubber hose of Kissy Missy and Withered Huggy: leg_L.001 to .009 from the hips to the
// ankle, the "leg" a bone at the ankle under the root): with no thigh and a shin not below the hips, the chain under the
// hips that ends where the shin starts is the leg, its first bone the thigh and the one halfway down the shin; the old
// shin is the foot, the old foot the toe. After the shape (the hips of those rigs are found by it) and the joints.
static void hose_legs_pass(An* an) {
    const Armature* a = an->a;
    CharMap* m = an->m;
    for (int side = 0; side < 2; side++) {
        int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
        int hips = charmap_bone(a, m, CR_HIPS), sh = charmap_bone(a, m, CR_SHIN_L + d);
        if (hips >= 0 && sh >= 0 && charmap_bone(a, m, CR_THIGH_L + d) < 0 && !arm_is_ancestor(a, hips, sh) &&
            m->roles[CR_SHIN_L + d].source != CSRC_MANUAL) {
            vec3 at = a->bones[sh].head;
            int best = -1, best_n = 0, chain[32];
            for (int c = 0; c < an->n; c++) {
                if (!an->in[c] || an->acc[c] || !arm_is_ancestor(a, hips, c) || c == hips) continue;
                int pr = an->par[c], prole = pr >= 0 ? charmap_role_of_bone(m, a->bones[pr].name) : CR_NONE;
                // it starts at the hips, at a spine bone or at a bone of no role below the hips
                if (pr < 0 || (pr != hips && (prole == CR_NONE ? !arm_is_ancestor(a, hips, pr) : !(prole >= CR_SPINE1 && prole <= CR_CHEST)))) continue;
                int own = charmap_role_of_bone(m, a->bones[c].name);
                if (own != CR_NONE && !charrole_is_twist(own)) continue;
                vec3 hc = a->bones[c].head;
                if ((d ? hc.x > -an->tol * 0.5f : hc.x < an->tol * 0.5f)) continue; // its side of the body
                int n = an_chain(an, c, chain, 32, false);
                if (n < 4 || v3_len(v3_sub(an_tip(an, chain[n - 1]), at)) > 2.0f * an->tol) continue;
                if (best < 0 || n > best_n) best = c, best_n = n;
            }
            if (best >= 0) {
                int n = an_chain(an, best, chain, 32, false);
                float total = 0.0f, run = 0.0f;
                for (int i = 0; i < n; i++) total += v3_len(v3_sub(an_tip(an, chain[i]), a->bones[chain[i]].head));
                int knee = 1;
                for (int i = 0; i < n; i++) {
                    if (run >= 0.5f * total) {
                        knee = i;
                        break;
                    }
                    run += v3_len(v3_sub(an_tip(an, chain[i]), a->bones[chain[i]].head));
                }
                if (knee < 1) knee = 1;
                int ft = charmap_bone(a, m, CR_FOOT_L + d);
                float conf = m->roles[CR_SHIN_L + d].confidence;
                for (int i = 0; i < n; i++) { // the roles the names gave the chain's pieces (a twist, a toe)
                    int r = charmap_role_of_bone(m, a->bones[chain[i]].name);
                    if (r != CR_NONE && m->roles[r].source != CSRC_MANUAL) memset(&m->roles[r], 0, sizeof m->roles[0]);
                }
                memset(&m->roles[CR_SHIN_L + d], 0, sizeof m->roles[0]);
                memset(&m->roles[CR_FOOT_L + d], 0, sizeof m->roles[0]);
                an_force(an, CR_THIGH_L + d, chain[0], conf);
                an_force(an, CR_SHIN_L + d, chain[knee], conf);
                an_force(an, CR_FOOT_L + d, sh, conf);
                if (ft >= 0) {
                    memset(&m->roles[CR_TOE_L + d], 0, sizeof m->roles[0]);
                    an_force(an, CR_TOE_L + d, ft, conf);
                }
            }
        }
    }
}

// --- (2) and (3) Shape ------------------------------------------------------------------------------------------------------

// The chain from a bone down its main children, at most cap bones; stops at a bone whose children branch in three or
// more (a hand) when stop_at_branch, that bone included. Returns how many.
static int an_chain(const An* an, int b, int* out, int cap, bool stop_at_branch) {
    int n = 0;
    while (b >= 0 && n < cap) {
        out[n++] = b;
        if (stop_at_branch && an_children(an, b, NULL, 0) >= 3) break;
        b = an_main_child(an, b);
    }
    return n;
}

static void shape_leg(An* an, int thigh, int side) {
    int chain[8], n = an_chain(an, thigh, chain, 8, false);
    static const int roles[4] = { CR_THIGH_L, CR_SHIN_L, CR_FOOT_L, CR_TOE_L };
    for (int i = 0; i < n && i < 4; i++) an_set(an, side == 2 ? charrole_mirror(roles[i]) : roles[i], chain[i], 0.5f, CSRC_TOPOLOGY);
}

static void shape_fingers(An* an, int hand, int side) {
    int all[16], kids[16], na = an_kids_full(an, hand, all, 16), n = 0;
    if (na > 16) na = 16;
    for (int i = 0; i < na; i++)
        if (!an->acc[all[i]]) kids[n++] = all[i];
    if (n < 2) return;
    // in a T or A pose the fingers sit side by side along Y: the thumb in front (-Y), the pinky behind; by where their
    // first bones end (metacarpals all start at the wrist)
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && an->a->bones[kids[j]].tail.y < an->a->bones[kids[j - 1]].tail.y; j--) {
            int t = kids[j];
            kids[j] = kids[j - 1];
            kids[j - 1] = t;
        }
    static const int first[5] = { CR_THUMB1_L, CR_INDEX1_L, CR_MIDDLE1_L, CR_RING1_L, CR_PINKY1_L };
    for (int f = 0; f < n && f < 5; f++) {
        // four bones or more from the wrist (nearer the hand's head than its tail): the first is a metacarpal, in the
        // palm, and the finger is the three after it; from the knuckles the fourth is the tip
        int chain[4], c = an_chain(an, kids[f], chain, 4, false);
        const Bone* hb = &an->a->bones[hand];
        vec3 fh = an->a->bones[kids[f]].head;
        int skip = c >= 4 && v3_len(v3_sub(fh, hb->head)) < v3_len(v3_sub(fh, hb->tail)) ? 1 : 0;
        for (int k = 0; k + skip < c && k < 3; k++)
            an_set(an, side == 2 ? charrole_mirror(first[f] + k) : first[f] + k, chain[k + skip], 0.4f, CSRC_TOPOLOGY);
    }
}

static void shape_arm(An* an, int root, int side) {
    int chain[12], n = an_chain(an, root, chain, 12, true);
    bool hand_found = n > 0 && an_children(an, chain[n - 1], NULL, 0) >= 3;
    int roles[4] = { CR_CLAVICLE_L, CR_UPPER_ARM_L, CR_FOREARM_L, CR_HAND_L };
    int pick[4] = { -1, -1, -1, -1 };
    if (hand_found && n >= 4) {
        pick[0] = chain[0], pick[1] = chain[1], pick[2] = chain[n - 2], pick[3] = chain[n - 1];
    } else if (hand_found && n == 3) {
        pick[1] = chain[0], pick[2] = chain[1], pick[3] = chain[2];
    } else if (n >= 4) {
        pick[0] = chain[0], pick[1] = chain[1], pick[2] = chain[2], pick[3] = chain[3];
    } else if (n == 3) {
        pick[1] = chain[0], pick[2] = chain[1], pick[3] = chain[2];
    } else if (n == 2) {
        pick[1] = chain[0], pick[2] = chain[1];
    }
    for (int i = 0; i < 4; i++)
        if (pick[i] >= 0) an_set(an, side == 2 ? charrole_mirror(roles[i]) : roles[i], pick[i], 0.5f, CSRC_TOPOLOGY);
    int hand = charmap_bone(an->a, an->m, side == 2 ? CR_HAND_R : CR_HAND_L);
    if (hand >= 0) shape_fingers(an, hand, side);
}

static void shape_neck(An* an, int first) {
    int chain[8], n = an_chain(an, first, chain, 8, false);
    // an end marker (a short leaf after the head) is not the head
    if (n >= 2 && an_children(an, chain[n - 1], NULL, 0) == 0 && an->a->bones[chain[n - 1]].length < 0.5f * an->a->bones[chain[n - 2]].length) n--;
    if (n <= 0) return;
    // the head is where the face branches off, else the end of the chain (at most two neck bones before it)
    int head = n - 1;
    for (int i = 0; i < n; i++)
        if (an_children(an, chain[i], NULL, 0) >= 2) {
            head = i;
            break;
        }
    if (head > 2) head = 2;
    an_set(an, CR_HEAD, chain[head], 0.5f, CSRC_TOPOLOGY);
    for (int i = 0; i < head; i++) an_set(an, CR_NECK1 + i, chain[i], 0.5f, CSRC_TOPOLOGY);
}

// The body by its shape: finds the hips, the legs, the spine, the chest, the arms and the neck that the names left out.
static void shape_pass(An* an) {
    CharMap* m = an->m;
    int hips = charmap_bone(an->a, m, CR_HIPS);
    int l[8], r[8];
    if (hips < 0) {
        // the highest bone (nearest the root) where two mirrored chains of at least two bones and something else meet
        int best = -1;
        for (int b = 0; b < an->n; b++) {
            if (!an->in[b] || an_children(an, b, NULL, 0) < 3) continue;
            if (an_pairs(an, b, 2, l, r, 8) < 1) continue;
            if (best < 0 || an->depth[b] < an->depth[best]) best = b;
        }
        if (best < 0) return;
        hips = best;
        an_set(an, CR_HIPS, hips, 0.5f, CSRC_TOPOLOGY);
        if (charmap_bone(an->a, m, CR_HIPS) != hips) return;
    }
    if (!an->in[hips]) return;
    // the root: the top of the hips' tree, when it is another bone
    int top = hips;
    while (an->par[top] >= 0) top = an->par[top];
    if (top != hips) an_set(an, CR_ROOT, top, 0.5f, CSRC_TOPOLOGY);
    // legs: the mirrored pair under the hips
    int thigh_l = charmap_bone(an->a, m, CR_THIGH_L), thigh_r = charmap_bone(an->a, m, CR_THIGH_R);
    if (thigh_l < 0 && thigh_r < 0 && an_pairs(an, hips, 2, l, r, 8) >= 1) {
        shape_leg(an, l[0], 1);
        shape_leg(an, r[0], 2);
        thigh_l = l[0];
        thigh_r = r[0];
    }
    // the spine: up from the hips through the child that is not a leg, to the bone where two mirrored arms meet
    bool have_spine = false;
    for (int role = CR_SPINE1; role <= CR_CHEST; role++) have_spine = have_spine || m->roles[role].bone[0];
    int chest = charmap_bone(an->a, m, CR_CHEST);
    if (chest < 0) chest = charmap_bone(an->a, m, CR_SPINE1);
    if (!have_spine) {
        int kids[32], n = an_children(an, hips, kids, 32), start = -1;
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            if (kids[i] == thigh_l || kids[i] == thigh_r || an->acc[kids[i]]) continue;
            if (charmap_role_of_bone(m, an->a->bones[kids[i]].name) != CR_NONE) continue;
            if (start < 0 || an->size[kids[i]] > an->size[start]) start = kids[i];
        }
        int path[16], count = 0;
        for (int b = start; b >= 0 && count < 16; b = an_main_child(an, b)) {
            path[count++] = b;
            if (an_pairs(an, b, 2, l, r, 8) >= 1) break; // the chest
        }
        bool found_chest = count > 0 && an_pairs(an, path[count - 1], 2, l, r, 8) >= 1;
        if (found_chest) {
            chest = path[count - 1];
            if (count == 1) an_set(an, CR_SPINE1, chest, 0.5f, CSRC_TOPOLOGY);
            else {
                an_set(an, CR_CHEST, chest, 0.5f, CSRC_TOPOLOGY);
                for (int i = 0; i < count - 1 && i < 4; i++) an_set(an, CR_SPINE1 + i, path[i], 0.5f, CSRC_TOPOLOGY);
            }
        }
    }
    if (chest < 0 || !an->in[chest]) return;
    // a horizontal spine is a quadruped's: its front legs are chains, not arms
    vec3 up = v3_sub(an->a->bones[chest].head, an->a->bones[hips].head);
    bool quadruped = v3_len(up) > an->tol && fabsf(up.z) < 0.5f * v3_len(up);
    if (quadruped) m->rig_class = CRC_QUADRUPED;
    int arm_l = charmap_bone(an->a, m, CR_UPPER_ARM_L), arm_r = charmap_bone(an->a, m, CR_UPPER_ARM_R);
    int pairs = an_pairs(an, chest, 2, l, r, 8);
    int used_l = -1, used_r = -1;
    if (arm_l < 0 && arm_r < 0 && pairs >= 1 && charmap_role_of_bone(m, an->a->bones[l[0]].name) == CR_NONE) {
        used_l = l[0], used_r = r[0];
        if (quadruped) {
            int cl[8], cr[8], nl = an_chain(an, l[0], cl, 8, false), nr = an_chain(an, r[0], cr, 8, false);
            charmap_chain_add(m, "front_leg.L", CCK_LEG, 1, an->a->bones[cl[0]].name, an->a->bones[cl[nl - 1]].name);
            charmap_chain_add(m, "front_leg.R", CCK_LEG, 2, an->a->bones[cr[0]].name, an->a->bones[cr[nr - 1]].name);
        } else {
            shape_arm(an, l[0], 1);
            shape_arm(an, r[0], 2);
        }
    } else {
        for (int side = 1; side <= 2; side++) { // the names gave the arm: the fingers may still be missing
            int hand = charmap_bone(an->a, m, side == 2 ? CR_HAND_R : CR_HAND_L);
            if (hand >= 0 && !m->roles[side == 2 ? CR_INDEX1_R : CR_INDEX1_L].bone[0]) shape_fingers(an, hand, side);
        }
    }
    // neck and head: the chest's child that is not an arm
    if (!m->roles[CR_HEAD].bone[0]) {
        int kids[32], n = an_children(an, chest, kids, 32), start = -1;
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            if (kids[i] == used_l || kids[i] == used_r || an->acc[kids[i]]) continue;
            int role = charmap_role_of_bone(m, an->a->bones[kids[i]].name);
            if (role != CR_NONE && role != CR_NECK1) continue;
            if (fabsf(an->a->bones[kids[i]].head.x) > an->tol) continue;
            if (start < 0 || an->size[kids[i]] > an->size[start]) start = kids[i];
        }
        if (start >= 0) shape_neck(an, start);
    }
}

// --- (4) Chains --------------------------------------------------------------------------------------------------------------

static bool an_bone_has_role(const An* an, int b) { return charmap_role_of_bone(an->m, an->a->bones[b].name) != CR_NONE; }

static bool an_in_chain(const An* an, int b) {
    for (int i = 0; i < an->m->chain_count; i++) {
        int first = armature_find_bone(an->a, an->m->chains[i].first), last = armature_find_bone(an->a, an->m->chains[i].last);
        if (first >= 0 && last >= 0 && an_is_ancestor(an, first, b) && an_is_ancestor(an, b, last)) return true;
    }
    return false;
}

static bool an_role_is_trunk(const An* an, int b) {
    int role = charmap_role_of_bone(an->m, an->a->bones[b].name);
    return role >= CR_SPINE1 && role <= CR_CHEST;
}

// What is left in chains of three or more bones (two for a tail by name) becomes generic chains. Chains under the
// head (hair, ears by name only) and under the hands are left out.
static float an_chain_length(const An* an, const int* chain, int n) {
    float len = 0.0f;
    for (int i = 0; i < n; i++) len += v3_len(v3_sub(an->a->bones[chain[i]].tail, an->a->bones[chain[i]].head));
    return len;
}

// The body's arm, from the clavicle (else the upper arm) to the wrist; a third of the height when it is not mapped.
// Some rigs split the upper arm in two bones (Auto-Rig Pro's twist and stretch): the clavicle gives the whole arm.
static float an_arm_length(const An* an) {
    int u = charmap_bone(an->a, an->m, CR_CLAVICLE_L), h = charmap_bone(an->a, an->m, CR_HAND_L);
    if (u < 0) u = charmap_bone(an->a, an->m, CR_UPPER_ARM_L);
    if (u < 0 || h < 0) {
        h = charmap_bone(an->a, an->m, CR_HAND_R);
        u = charmap_bone(an->a, an->m, CR_CLAVICLE_R);
        if (u < 0) u = charmap_bone(an->a, an->m, CR_UPPER_ARM_R);
    }
    if (u < 0 || h < 0) return an->height / 3.0f;
    return v3_len(v3_sub(an->a->bones[h].head, an->a->bones[u].head));
}

static void chains_pass(An* an) {
    CharMap* m = an->m;
    int head = charmap_bone(an->a, m, CR_HEAD), hips = charmap_bone(an->a, m, CR_HIPS);
    int hand_l = charmap_bone(an->a, m, CR_HAND_L), hand_r = charmap_bone(an->a, m, CR_HAND_R);
    bool body = charmap_mapped_count(m) > 0;
    float ground = an->lo.z + 0.1f * (an->hi.z - an->lo.z);
    for (int b = 0; b < an->n && m->chain_count < TL_CHARMAP_CHAINS; b++) {
        if (!an->in[b] || an_bone_has_role(an, b) || an_in_chain(an, b)) continue;
        int p = an->par[b];
        // a chain starts at a bone whose parent is not part of the same free chain
        if (p >= 0 && !an_bone_has_role(an, p) && !an_in_chain(an, p) && an_children(an, p, NULL, 0) == 1) continue;
        int chain[64], n = 0;
        for (int c = b; c >= 0 && n < 64 && !an_bone_has_role(an, c); c = an_children(an, c, NULL, 0) == 1 ? an_main_child(an, c) : -1) chain[n++] = c;
        if (n < 2) continue;
        const Parsed* pn = &an->names[b];
        bool named_tail = strncmp(pn->base, "tail", 4) == 0, named_ear = strncmp(pn->base, "ear", 3) == 0;
        bool named_wing = strncmp(pn->base, "wing", 4) == 0, named_tent = strncmp(pn->base, "tentacle", 8) == 0;
        bool named_leg = strstr(pn->base, "thigh") != NULL || strstr(pn->base, "leg") != NULL;
        bool named = named_tail || named_ear || named_wing || named_tent || named_leg;
        if (n < 3 && !named) continue;
        if (head >= 0 && an_is_ancestor(an, head, b) && !named_ear) continue;
        if ((hand_l >= 0 && an_is_ancestor(an, hand_l, b)) || (hand_r >= 0 && an_is_ancestor(an, hand_r, b))) continue;
        vec3 h = an->a->bones[b].head, t = an->a->bones[chain[n - 1]].tail;
        int side = h.x > an->tol * 0.5f ? 1 : h.x < -an->tol * 0.5f ? 2 : 0;
        if (pn->side) side = pn->side;
        int kind = CCK_GENERIC;
        if (named_tail) kind = CCK_TAIL;
        else if (named_ear) kind = CCK_EAR;
        else if (named_wing) kind = CCK_WING;
        else if (named_tent) kind = CCK_TENTACLE;
        else if (named_leg) kind = CCK_LEG;
        else if (!side && p >= 0 && p == hips) kind = CCK_TAIL;         // a center chain off the hips
        else if (side && t.z < ground && h.z > t.z) kind = CCK_LEG;      // a side chain that reaches the ground
        else if (side && p >= 0 && an_role_is_trunk(an, p) && n >= 3 && an_chain_length(an, chain, n) >= 0.75f * an_arm_length(an) && v3_len(v3_sub(t, h)) >= 0.6f * an_arm_length(an))
            kind = CCK_ARM; // a side chain off the trunk as long as an arm and reaching as far: one more arm (armor plates and straps that hug the body are not)
        else if (!body && n >= 4) kind = CCK_TENTACLE;
        char name[TL_CHARMAP_CHAIN_NAME];
        const char* stem = named_leg && strncmp(pn->base, "front", 5) == 0 ? "front_leg" : charmap_chain_kind_code(kind);
        if (side) snprintf(name, sizeof name, "%s.%c", stem, side == 1 ? 'L' : 'R');
        else snprintf(name, sizeof name, "%s", stem);
        charmap_chain_add(m, name, kind, side, an->a->bones[b].name, an->a->bones[chain[n - 1]].name);
    }
}

// --- (5) The class (plan item AM10) ---------------------------------------------------------------------------------------------

// The end of the limb that starts at b: down through the main child to a leaf.
static int an_limb_end(const An* an, int b) {
    for (int c = an_main_child(an, b), guard = 0; c >= 0 && guard < an->n; c = an_main_child(an, c), guard++) b = c;
    return b;
}

// The mirrored pairs of limbs (two bones or more each) that end at the ground: a person has one (the legs), a horse
// two, a spider four. The front pair first (the character faces -Y). Returns how many (at most cap written).
static int an_ground_pairs(const An* an, int* l, int* r, int cap) {
    float hz = an->hi.z - an->lo.z;
    float ground = an->lo.z + 0.12f * tl_maxf(hz, 0.05f * an->height);
    int n = 0, pl[8], pr[8];
    vec3 ends[64]; // where each counted pair's left limb ends: the copies of one leg (a control rig's IK, FK and twist
                   // chains in the same place, all exported) end where it does and are one leg
    for (int b = 0; b < an->n; b++) {
        if (!an->in[b] || an->acc[b]) continue;
        int pairs = an_pairs(an, b, 2, pl, pr, 8);
        for (int i = 0; i < pairs; i++) {
            const Bone* e = &an->a->bones[an_limb_end(an, pl[i])];
            if (tl_minf(e->head.z, e->tail.z) > ground) continue;
            vec3 end = e->tail;
            bool same = false;
            for (int k = 0; k < n && k < 64; k++) same = same || v3_len(v3_sub(ends[k], end)) < an->tol;
            if (same) continue;
            if (n < 64) ends[n] = end;
            if (l && r && n < cap) l[n] = pl[i], r[n] = pr[i];
            n++;
        }
    }
    int m = n < cap ? n : cap;
    for (int i = 1; l && r && i < m; i++) // by Y, the front first
        for (int j = i; j > 0 && an->a->bones[l[j]].head.y < an->a->bones[l[j - 1]].head.y; j--) {
            int t = l[j];
            l[j] = l[j - 1], l[j - 1] = t;
            t = r[j];
            r[j] = r[j - 1], r[j - 1] = t;
        }
    return n;
}

// Most bones are named as parts of a machine (the mechanical presets of mech_rig.h and the usual words). The
// controls (CTRL-) do not count either way.
static bool an_mech_names(const An* an) {
    static const char* const words[] = { "piston", "cylinder", "rod",   "gear", "chainlink", "chain",    "cable",  "wheel", "axle",
                                         "hydraulic", "hinge",  "crank", "cog",  "tread",     "sprocket", "pulley", "shaft" };
    int total = 0, mech = 0;
    for (int b = 0; b < an->n; b++) {
        if (starts_ci(an->a->bones[b].name, "CTRL-")) continue;
        total++;
        char base[TL_BONE_NAME_LEN];
        snprintf(base, sizeof base, "%s", an->names[b].base);
        size_t len = strlen(base);
        while (len > 0 && isdigit((unsigned char)base[len - 1])) base[--len] = 0;
        for (int w = 0; w < (int)(sizeof words / sizeof words[0]); w++)
            if (strcmp(base, words[w]) == 0) {
                mech++;
                break;
            }
    }
    return mech >= 2 && mech * 10 >= total * 6;
}

// Three pairs of legs or more: not the body of a person. The roles found (not the ones picked by hand) are dropped
// and every leg becomes a chain, leg_1 the front pair.
// A body with a head and two hands (each with fingers) is a person's: what else reaches the ground in pairs is cloth (a
// skirt's chains hanging from the hips) or a control rig's copies of the legs, not more legs.
static bool an_person_body(const An* an, const CharMap* m) {
    int hl = charmap_bone(an->a, m, CR_HAND_L), hr = charmap_bone(an->a, m, CR_HAND_R);
    return charmap_bone(an->a, m, CR_HEAD) >= 0 && hl >= 0 && hr >= 0 && an_kids_full(an, hl, NULL, 0) >= 3 && an_kids_full(an, hr, NULL, 0) >= 3;
}

static void many_legs_pass(An* an) {
    if (an_person_body(an, an->m)) return;
    int l[16], r[16], n = an_ground_pairs(an, l, r, 16);
    if (n < 3) return;
    if (n > 16) n = 16;
    CharMap* m = an->m;
    for (int role = CR_NONE + 1; role < CR_COUNT; role++)
        if (m->roles[role].source != CSRC_MANUAL) memset(&m->roles[role], 0, sizeof m->roles[role]);
    m->chain_count = 0;
    for (int i = 0; i < n; i++) {
        char name[TL_CHARMAP_CHAIN_NAME];
        snprintf(name, sizeof name, "leg_%d.L", i + 1);
        charmap_chain_add(m, name, CCK_LEG, 1, an->a->bones[l[i]].name, an->a->bones[an_limb_end(an, l[i])].name);
        snprintf(name, sizeof name, "leg_%d.R", i + 1);
        charmap_chain_add(m, name, CCK_LEG, 2, an->a->bones[r[i]].name, an->a->bones[an_limb_end(an, r[i])].name);
    }
    m->rig_class = CRC_NON_HUMAN;
}

static float an_limb_length(const Armature* a, const CharMap* m, int r0, int r1, int r2) {
    int b0 = charmap_bone(a, m, r0), b1 = charmap_bone(a, m, r1), b2 = charmap_bone(a, m, r2);
    if (b0 < 0 || b1 < 0) return 0.0f;
    vec3 end = b2 >= 0 ? a->bones[b2].head : a->bones[b1].tail;
    return v3_len(v3_sub(a->bones[b1].head, a->bones[b0].head)) + v3_len(v3_sub(end, a->bones[b1].head));
}

int charmap_classify(const Armature* a, CharMap* m, CharClassInfo* info) {
    CharClassInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.reason = CCR_NOTHING;
    An an;
    if (!a || !m || a->bone_count <= 0 || !an_build(&an, a, m, NULL)) {
        if (a && m && a->bone_count > 0) an_free(&an);
        if (m) m->rig_class = CRC_UNKNOWN;
        if (info) *info = ci;
        return CRC_UNKNOWN;
    }
    ci.leg_pairs = an_ground_pairs(&an, NULL, NULL, 0);
    ci.mech_names = an_mech_names(&an);
    bool has_spine = false;
    int top = -1; // the highest spine bone
    for (int role = CR_SPINE1; role <= CR_CHEST; role++)
        if (charmap_bone(a, m, role) >= 0) has_spine = true, top = charmap_bone(a, m, role);
    int hips = charmap_bone(a, m, CR_HIPS);
    bool legs = charmap_bone(a, m, CR_THIGH_L) >= 0 && charmap_bone(a, m, CR_THIGH_R) >= 0 && charmap_bone(a, m, CR_SHIN_L) >= 0 &&
                charmap_bone(a, m, CR_SHIN_R) >= 0;
    ci.biped_body = legs && hips >= 0 && has_spine;
    if (hips >= 0 && top >= 0) {
        vec3 up = v3_sub(a->bones[top].tail, a->bones[hips].head);
        ci.spine_horizontal = v3_len(up) > an.tol && fabsf(up.z) < 0.5f * v3_len(up);
    }
    for (int i = 0; i < m->chain_count; i++) {
        int k = m->chains[i].kind;
        if (k == CCK_LEG || k == CCK_ARM || k == CCK_WING || k == CCK_TENTACLE || k == CCK_TAIL) ci.extra_limbs++;
    }
    float arm = 0.0f, leg = 0.0f;
    int arms_n = 0, legs_n = 0;
    for (int side = 0; side < 2; side++) {
        int d = side ? CR_CLAVICLE_R - CR_CLAVICLE_L : 0;
        float la = an_limb_length(a, m, CR_UPPER_ARM_L + d, CR_FOREARM_L + d, CR_HAND_L + d);
        float ll = an_limb_length(a, m, CR_THIGH_L + d, CR_SHIN_L + d, CR_FOOT_L + d);
        if (la > 0.0f) arm += la, arms_n++;
        if (ll > 0.0f) leg += ll, legs_n++;
    }
    if (arms_n) arm /= (float)arms_n;
    if (legs_n) leg /= (float)legs_n;
    float hz = an.hi.z - an.lo.z;
    if (arm > 0.0f && leg > 0.0f) ci.arm_leg_ratio = arm / leg;
    if (leg > 0.0f && hz > 1e-6f) ci.leg_height_ratio = leg / hz;
    int mapped = charmap_mapped_count(m), missing = charmap_missing_main(m, NULL, 0);
    if (ci.mech_names && !ci.biped_body) ci.rig_class = CRC_MECHANICAL, ci.reason = CCR_MECH_NAMES;
    else if (ci.leg_pairs >= 3 && !an_person_body(&an, m)) ci.rig_class = CRC_NON_HUMAN, ci.reason = CCR_MANY_LEGS;
    else if (ci.spine_horizontal) ci.rig_class = CRC_QUADRUPED, ci.reason = CCR_FOUR_LEGS;
    else if (ci.biped_body) {
        bool odd = (ci.arm_leg_ratio > 0.0f && (ci.arm_leg_ratio < 0.45f || ci.arm_leg_ratio > 1.25f)) ||
                   (ci.leg_height_ratio > 0.0f && (ci.leg_height_ratio < 0.3f || ci.leg_height_ratio > 0.65f));
        if (missing) ci.rig_class = CRC_NEAR_HUMAN, ci.reason = CCR_MISSING_MAIN;
        else if (ci.extra_limbs) ci.rig_class = CRC_NEAR_HUMAN, ci.reason = CCR_EXTRA_LIMBS;
        else if (odd) ci.rig_class = CRC_NEAR_HUMAN, ci.reason = CCR_PROPORTIONS;
        else ci.rig_class = CRC_HUMANOID, ci.reason = CCR_ALL_MAIN;
    } else if (mapped == 0 && m->chain_count > 0) ci.rig_class = CRC_NON_HUMAN, ci.reason = CCR_ONLY_CHAINS;
    else ci.rig_class = CRC_UNKNOWN, ci.reason = CCR_NOTHING;
    an_free(&an);
    m->rig_class = (uint8_t)ci.rig_class;
    if (info) *info = ci;
    return ci.rig_class;
}

// --- The analysis -----------------------------------------------------------------------------------------------------------

bool charmap_analyze(const Armature* a, const CharMap* keep, CharMap* out, CharAnalysis* rep) { return charmap_analyze_ex(a, keep, NULL, out, rep); }

bool charmap_analyze_ex(const Armature* a, const CharMap* keep, const unsigned char* skin, CharMap* out, CharAnalysis* rep) {
    if (rep) memset(rep, 0, sizeof *rep);
    if (!a || a->bone_count <= 0) return false;
    CharMap* m = charmap_new();
    An an;
    if (!m || !an_build(&an, a, m, skin)) {
        charmap_free(m);
        if (m) an_free(&an);
        return false;
    }
    // what the user picked stays
    for (int r = CR_NONE + 1; keep && r < CR_COUNT; r++)
        if (keep->roles[r].source == CSRC_MANUAL && keep->roles[r].bone[0] && armature_find_bone(a, keep->roles[r].bone) >= 0)
            m->roles[r] = keep->roles[r];
    char convention[32] = "";
    int named = names_pass(&an, convention, sizeof convention);
    limbs_pass(&an);
    ends_pass(&an);
    fingers_fill(&an);
    int after_names = charmap_mapped_count(m);
    hips_beside_spine_pass(&an);
    shape_pass(&an);
    for (int side = 1; side <= 2; side++) { // the fingers of a hand the names or the joints gave, whatever the shape found
        int hand = charmap_bone(a, m, side == 2 ? CR_HAND_R : CR_HAND_L);
        if (hand >= 0 && !m->roles[side == 2 ? CR_INDEX1_R : CR_INDEX1_L].bone[0]) shape_fingers(&an, hand, side);
    }
    trunk_pass(&an);
    // a quadruped named as Tlender's meta-rig: the spine runs along Y
    int hips = charmap_bone(a, m, CR_HIPS), chest = charmap_bone(a, m, CR_CHEST);
    if (chest < 0) chest = charmap_bone(a, m, CR_SPINE1);
    if (hips >= 0 && chest >= 0) {
        vec3 up = v3_sub(a->bones[chest].tail, a->bones[hips].head);
        if (v3_len(up) > an.tol && fabsf(up.z) < 0.5f * v3_len(up)) m->rig_class = CRC_QUADRUPED;
    }
    many_legs_pass(&an);
    joints_pass(&an);
    hose_legs_pass(&an); // after the joints: a leg of even pieces has no joint where its thigh ends
    chains_pass(&an);
    int mapped = charmap_mapped_count(m);
    snprintf(m->convention, sizeof m->convention, "%s", named ? convention : "");
    m->source = (uint8_t)(mapped == 0 ? (m->chain_count ? CSRC_TOPOLOGY : CSRC_NONE)
                                      : after_names * 2 >= mapped ? (strcmp(convention, "tlender") == 0 ? CSRC_AUTORIG : CSRC_NAMES) : CSRC_TOPOLOGY);
    charmap_classify(a, m, NULL);
    if (rep) {
        rep->rig_class = m->rig_class;
        rep->source = m->source;
        snprintf(rep->convention, sizeof rep->convention, "%s", m->convention);
        rep->mapped = mapped;
        rep->main_total = charmap_main_total();
        rep->main_mapped = rep->main_total - charmap_missing_main(m, NULL, 0);
        rep->chains = m->chain_count;
        rep->confidence = charmap_confidence(m);
        for (int b = 0; b < a->bone_count; b++) {
            if (an_deforms(&an, b)) rep->deform_bones++;
            else rep->control_bones++;
        }
    }
    *out = *m;
    an_free(&an);
    charmap_free(m);
    return true;
}
