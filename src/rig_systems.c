// The rig systems (rig_systems.h): markers, controls, IK/FK switches and IK handles of each one.
#include "rig_systems.h"

#include "rig_armature_math.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct SysInfo {
    const char* code;
    const char* name;
    const char* origin; // msgid
    bool control;
} SysInfo;

static const SysInfo SYS[RIGSYS_COUNT] = {
    [RIGSYS_NONE] = { "", "", "", false },
    [RIGSYS_TLENDER] = { "tlender", "Tlender Auto-Rig", "auto-rig do Tlender", true },
    [RIGSYS_RIGIFY] = { "rigify", "Rigify", "add-on do Blender", true },
    [RIGSYS_ARP] = { "autorigpro", "Auto-Rig Pro", "add-on do Blender", true },
    [RIGSYS_ADVSKEL] = { "advskel", "AdvancedSkeleton", "plugin do Maya", true },
    [RIGSYS_MIXAMO] = { "mixamo", "Mixamo / HumanIK", "Mixamo, Maya, MotionBuilder e captura de movimento", false },
    [RIGSYS_UE] = { "ue", "Unreal Mannequin / MetaHuman", "Unreal Engine", false },
    [RIGSYS_VROID] = { "vroid", "VRoid / VRM", "VRoid Studio", false },
    [RIGSYS_CC] = { "cc", "Character Creator / AccuRig", "Reallusion", false },
    [RIGSYS_DAZ] = { "daz", "Daz Genesis", "Daz Studio", false },
    [RIGSYS_BIPED] = { "biped", "Biped", "3ds Max", false },
    [RIGSYS_CAT] = { "cat", "CAT", "3ds Max", false },
    [RIGSYS_SMPL] = { "smpl", "SMPL", "pesquisa (corpo paramétrico)", false },
    [RIGSYS_VALVE] = { "valve", "Valve Biped", "Source / Source Filmmaker", false },
    [RIGSYS_MPFB] = { "mpfb", "MakeHuman / MPFB", "MakeHuman", false },
    [RIGSYS_CONTROL] = { "controle", "Rig de controle", "rig com controles (sistema não identificado)", true },
};

const char* rigsys_code(int sys) { return sys > RIGSYS_NONE && sys < RIGSYS_COUNT ? SYS[sys].code : ""; }
const char* rigsys_name(int sys) { return sys > RIGSYS_NONE && sys < RIGSYS_COUNT ? SYS[sys].name : ""; }
const char* rigsys_origin(int sys) { return sys > RIGSYS_NONE && sys < RIGSYS_COUNT ? SYS[sys].origin : ""; }
bool rigsys_is_control_rig(int sys) { return sys > RIGSYS_NONE && sys < RIGSYS_COUNT && SYS[sys].control; }

int rigsys_from_code(const char* code) {
    if (!code || !code[0]) return RIGSYS_NONE;
    for (int i = 1; i < RIGSYS_COUNT; i++)
        if (strcmp(code, SYS[i].code) == 0) return i;
    if (strcmp(code, "palavras") == 0) return RIGSYS_NONE;
    return RIGSYS_NONE;
}

// --- Names -------------------------------------------------------------------------------------------------------------

static bool starts_ci(const char* s, const char* p) {
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    return true;
}

static bool contains_ci(const char* s, const char* p) {
    size_t n = strlen(p);
    for (; *s; s++)
        if (strlen(s) >= n && starts_ci(s, p)) return true;
    return false;
}

static int find(const Armature* a, const char* name) { return name && name[0] ? armature_find_bone(a, name) : -1; }

// "upper_arm.L" -> core "upper_arm", suffix ".L" (the last two characters when they are a separator and a side or
// center letter); suffix "" when there is none.
static void split_side(const char* name, char* core, size_t cap, char sfx[4]) {
    size_t n = strlen(name);
    sfx[0] = 0;
    if (n > 2 && strchr("._", name[n - 2]) && strchr("LRlrxXMC", name[n - 1])) {
        snprintf(sfx, 4, "%s", name + n - 2);
        snprintf(core, cap, "%.*s", (int)(n - 2), name);
    } else {
        snprintf(core, cap, "%s", name);
    }
}

static int limb_side(int limb) { return limb & 1; } // 0 left, 1 right

// --- Detection -----------------------------------------------------------------------------------------------------------

typedef struct Marker {
    int sys;
    const char* name; // exact bone name, or a prefix when it ends in '*'
    int weight;
} Marker;

static const Marker MARKERS[] = {
    { RIGSYS_ARP, "c_traj", 5 },        { RIGSYS_ARP, "c_pos", 3 },          { RIGSYS_ARP, "c_root_master.x", 5 },
    { RIGSYS_ARP, "c_hand_ik.l", 2 },   { RIGSYS_ARP, "c_foot_ik.l", 2 },    { RIGSYS_ARP, "arm_stretch.l", 2 },
    { RIGSYS_RIGIFY, "torso", 3 },      { RIGSYS_RIGIFY, "MCH-*", 2 },       { RIGSYS_RIGIFY, "ORG-*", 2 },
    { RIGSYS_RIGIFY, "DEF-*", 2 },      { RIGSYS_RIGIFY, "upper_arm_parent.L", 3 }, { RIGSYS_RIGIFY, "properties", 1 },
    { RIGSYS_ADVSKEL, "Main", 2 },      { RIGSYS_ADVSKEL, "RootX_M", 5 },    { RIGSYS_ADVSKEL, "FKShoulder_L", 4 },
    { RIGSYS_ADVSKEL, "Root_M", 2 },    { RIGSYS_ADVSKEL, "FKIKArm_L", 4 },
    { RIGSYS_MIXAMO, "mixamorig*", 5 }, { RIGSYS_MIXAMO, "LeftUpLeg", 3 },   { RIGSYS_MIXAMO, "LeftForeArm", 3 },
    { RIGSYS_UE, "pelvis", 2 },         { RIGSYS_UE, "spine_01", 2 },        { RIGSYS_UE, "upperarm_l", 3 },
    { RIGSYS_UE, "calf_l", 3 },         { RIGSYS_UE, "ik_foot_root", 2 },
    { RIGSYS_VROID, "J_Bip_*", 5 },
    { RIGSYS_CC, "CC_Base_*", 5 },      { RIGSYS_CC, "RL_BoneRoot", 3 },
    { RIGSYS_DAZ, "lShldrBend", 5 },    { RIGSYS_DAZ, "l_upperarm", 3 },     { RIGSYS_DAZ, "abdomenLower", 3 },
    { RIGSYS_BIPED, "Bip01*", 5 },      { RIGSYS_BIPED, "Bip001*", 5 },
    { RIGSYS_CAT, "*Ribcage", 3 },      { RIGSYS_CAT, "*Palm", 2 },
    { RIGSYS_SMPL, "left_hip", 3 },     { RIGSYS_SMPL, "L_Hip", 3 },
    { RIGSYS_VALVE, "ValveBiped.*", 5 },
    { RIGSYS_MPFB, "upperarm01.L", 4 }, { RIGSYS_MPFB, "lowerarm01.L", 3 }, { RIGSYS_MPFB, "spine05", 2 },
};

static bool marker_hit(const Armature* a, const char* m) {
    size_t n = strlen(m);
    bool prefix = n && m[n - 1] == '*', suffix = m[0] == '*';
    if (!prefix && !suffix) return armature_find_bone(a, m) >= 0;
    for (int b = 0; b < a->bone_count; b++) {
        const char* bn = a->bones[b].name;
        if (prefix && strncmp(bn, m, n - 1) == 0) return true;
        if (suffix && strlen(bn) >= n - 1 && strcmp(bn + strlen(bn) - (n - 1), m + 1) == 0) return true;
    }
    return false;
}

int rigsys_detect(const Armature* a) {
    if (!a || a->bone_count <= 0) return RIGSYS_NONE;
    if (a->autorig_rig) return RIGSYS_TLENDER;
    for (int b = 0; b < a->bone_count; b++)
        if (a->bones[b].rigmod.type != RMOD_NONE) return RIGSYS_TLENDER;
    int score[RIGSYS_COUNT] = { 0 };
    for (size_t i = 0; i < sizeof MARKERS / sizeof MARKERS[0]; i++)
        if (marker_hit(a, MARKERS[i].name)) score[MARKERS[i].sys] += MARKERS[i].weight;
    int best = RIGSYS_NONE;
    for (int s = 1; s < RIGSYS_COUNT; s++)
        if (score[s] > (best ? score[best] : 0)) best = s;
    if (best && score[best] >= 5) return best;
    // an unknown control rig: deforming bones that constraints drive
    int deform = 0, driven = 0;
    for (int b = 0; b < a->bone_count; b++) {
        if (!a->bones[b].deform) continue;
        deform++;
        if (rigsys_constraint_control(a, b) >= 0) driven++;
    }
    return deform >= 5 && driven * 3 >= deform ? RIGSYS_CONTROL : RIGSYS_NONE;
}

// --- Controls ----------------------------------------------------------------------------------------------------------

static bool follow_type(int type) {
    return type == CON_COPY_ROTATION || type == CON_COPY_TRANSFORMS || type == CON_CHILD_OF || type == CON_COPY_LOCATION ||
           type == CON_ARMATURE;
}

int rigsys_constraint_control(const Armature* a, int bone) {
    if (!a || bone < 0 || bone >= a->bone_count) return -1;
    int cur = bone;
    for (int depth = 0; depth < 10; depth++) {
        const Bone* b = &a->bones[cur];
        int next = -1;
        float best = 0.0f;
        bool best_fk = false;
        for (int c = 0; c < b->con_count; c++) {
            const BoneConstraint* k = &b->cons[c];
            if (!k->enabled || !follow_type(k->type) || !k->target[0] || k->target_obj[0]) continue;
            int t = armature_find_bone(a, k->target);
            if (t < 0 || t == cur) continue;
            bool fk = contains_ci(k->target, "fk");
            // an FK target wins (a limb that follows both an IK and an FK chain), then the strongest one; a Copy
            // Location only when nothing turns the bone
            float w = k->influence + (k->type == CON_COPY_LOCATION ? -1.0f : 0.0f);
            if (next < 0 || (fk && !best_fk) || (fk == best_fk && w > best)) next = t, best = w, best_fk = fk;
        }
        if (next < 0) return cur == bone ? -1 : cur;
        cur = next;
    }
    return -1;
}

// Auto-Rig Pro: the FK control of a deforming bone ("arm_stretch.l" -> "c_arm_fk.l").
static int arp_control(const Armature* a, int bone) {
    const char* name = a->bones[bone].name;
    if (strncmp(name, "c_", 2) == 0) return bone;
    char core[TL_BONE_NAME_LEN], sfx[4], cand[TL_BONE_NAME_LEN + 8];
    split_side(name, core, sizeof core, sfx);
    static const struct { const char* from; const char* to; } T[] = {
        { "root", "c_root" },         { "neck", "c_neck" },           { "head", "c_head" },           { "shoulder", "c_shoulder" },
        { "arm_stretch", "c_arm_fk" }, { "forearm_stretch", "c_forearm_fk" }, { "hand", "c_hand_fk" },
        { "thigh_stretch", "c_thigh_fk" }, { "leg_stretch", "c_leg_fk" }, { "foot", "c_foot_fk" }, { "toes_01", "c_toes_fk" },
        { "jawbone", "c_jawbone" },
        // the joint the analyzer maps as the upper arm / thigh (arm_twist sits at the shoulder, arm_stretch starts lower):
        // posed by the FK control as the arm itself
        { "arm_twist", "c_arm_fk" }, { "thigh_twist", "c_thigh_fk" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (strcmp(core, T[i].from) == 0) {
            snprintf(cand, sizeof cand, "%s%s", T[i].to, sfx);
            return find(a, cand);
        }
    // the twist and stretch helpers follow the limb: nobody poses them
    if (strstr(core, "_twist") || strstr(core, "_stretch")) return -2;
    snprintf(cand, sizeof cand, "c_%s%s", core, sfx); // spine_01.x -> c_spine_01.x, index1.l -> c_index1.l
    return find(a, cand);
}

// Rigify: "DEF-spine.001" is posed by "spine_fk.001", "DEF-upper_arm.L" by "upper_arm_fk.L", "DEF-shoulder.L" by
// "shoulder.L"; the neck and the head by "neck" and "head".
static int rigify_control(const Armature* a, int bone) {
    const char* name = a->bones[bone].name;
    bool internal = strncmp(name, "DEF-", 4) == 0 || strncmp(name, "MCH-", 4) == 0 || strncmp(name, "ORG-", 4) == 0;
    if (!internal) return a->bones[bone].con_count == 0 ? bone : -1;
    const char* base = name + 4;
    const char* dot = strchr(base, '.');
    char cand[TL_BONE_NAME_LEN + 8];
    if (dot) snprintf(cand, sizeof cand, "%.*s_fk%s", (int)(dot - base), base, dot);
    else snprintf(cand, sizeof cand, "%s_fk", base);
    int k = find(a, cand);
    if (k < 0) k = find(a, base);
    if (k >= 0 && k != bone) return k;
    return -1;
}

// AdvancedSkeleton: "FK" + the joint ("Shoulder_L" -> "FKShoulder_L"); the root by "RootX_M".
static int advskel_control(const Armature* a, int bone) {
    const char* name = a->bones[bone].name;
    if (starts_ci(name, "FK") || starts_ci(name, "IK") || starts_ci(name, "Pole") || strcmp(name, "RootX_M") == 0) return bone;
    char cand[TL_BONE_NAME_LEN + 8];
    snprintf(cand, sizeof cand, "FK%s", name);
    int k = find(a, cand);
    if (k >= 0) return k;
    if (strcmp(name, "Root_M") == 0) return find(a, "RootX_M");
    return -1;
}

int rigsys_fk_control(const Armature* a, int sys, int bone) {
    if (!a || bone < 0 || bone >= a->bone_count) return -1;
    int k = -1;
    switch (sys) {
    case RIGSYS_ARP: k = arp_control(a, bone); break;
    case RIGSYS_RIGIFY: k = rigify_control(a, bone); break;
    case RIGSYS_ADVSKEL: k = advskel_control(a, bone); break;
    default: break;
    }
    if (k >= 0) return k;
    if (k == -2) return -1; // the system says nobody poses this bone
    if (a->bones[bone].con_count == 0) return bone;
    // driven by constraints: the bone they lead to (Limit constraints alone leave the bone free)
    int c = rigsys_constraint_control(a, bone);
    if (c >= 0) return c;
    bool follows = false;
    for (int i = 0; i < a->bones[bone].con_count; i++) follows = follows || follow_type(a->bones[bone].cons[i].type) ||
                                                                    a->bones[bone].cons[i].type == CON_IK ||
                                                                    a->bones[bone].cons[i].type == CON_STRETCH_TO ||
                                                                    a->bones[bone].cons[i].type == CON_DAMPED_TRACK;
    return follows ? -1 : bone;
}

int rigsys_body_control(const Armature* a, int sys) {
    if (!a) return -1;
    switch (sys) {
    case RIGSYS_ARP: return find(a, "c_root_master.x");
    case RIGSYS_RIGIFY: return find(a, "torso");
    case RIGSYS_ADVSKEL: return find(a, "RootX_M");
    default: return -1;
    }
}

bool rigsys_limb_switch(const Armature* a, int sys, int limb, RigLimbSwitch* out) {
    if (!a || !out || limb < 0 || limb > 3) return false;
    memset(out, 0, sizeof *out);
    bool arm = limb < 2, right = limb_side(limb);
    switch (sys) {
    case RIGSYS_ARP:
        snprintf(out->bone, sizeof out->bone, "%s.%c", arm ? "c_hand_ik" : "c_foot_ik", right ? 'r' : 'l');
        snprintf(out->prop, sizeof out->prop, "ik_fk_switch");
        out->fk = 1.0f, out->ik = 0.0f;
        break;
    case RIGSYS_RIGIFY:
        snprintf(out->bone, sizeof out->bone, "%s.%c", arm ? "upper_arm_parent" : "thigh_parent", right ? 'R' : 'L');
        snprintf(out->prop, sizeof out->prop, "IK_FK");
        out->fk = 1.0f, out->ik = 0.0f;
        break;
    case RIGSYS_ADVSKEL:
        snprintf(out->bone, sizeof out->bone, "%s_%c", arm ? "FKIKArm" : "FKIKLeg", right ? 'R' : 'L');
        snprintf(out->prop, sizeof out->prop, "FKIKBlend");
        out->fk = 0.0f, out->ik = 10.0f;
        break;
    default: return false;
    }
    return armature_find_bone(a, out->bone) >= 0;
}

int rigsys_ik_control(const Armature* a, int sys, int limb) {
    if (!a || limb < 0 || limb > 3) return -1;
    bool arm = limb < 2, right = limb_side(limb);
    char n[TL_BONE_NAME_LEN];
    switch (sys) {
    case RIGSYS_ARP: snprintf(n, sizeof n, "%s.%c", arm ? "c_hand_ik" : "c_foot_ik", right ? 'r' : 'l'); break;
    case RIGSYS_RIGIFY: snprintf(n, sizeof n, "%s.%c", arm ? "hand_ik" : "foot_ik", right ? 'R' : 'L'); break;
    case RIGSYS_ADVSKEL: snprintf(n, sizeof n, "%s_%c", arm ? "IKArm" : "IKLeg", right ? 'R' : 'L'); break;
    default: return -1;
    }
    return find(a, n);
}

int rigsys_pole_control(const Armature* a, int sys, int limb) {
    if (!a || limb < 0 || limb > 3) return -1;
    bool arm = limb < 2, right = limb_side(limb);
    char n[TL_BONE_NAME_LEN];
    switch (sys) {
    case RIGSYS_ARP: snprintf(n, sizeof n, "%s.%c", arm ? "c_arms_pole" : "c_leg_pole", right ? 'r' : 'l'); break;
    case RIGSYS_RIGIFY: snprintf(n, sizeof n, "%s.%c", arm ? "upper_arm_ik_target" : "thigh_ik_target", right ? 'R' : 'L'); break;
    case RIGSYS_ADVSKEL: snprintf(n, sizeof n, "%s_%c", arm ? "PoleArm" : "PoleLeg", right ? 'R' : 'L'); break;
    default: return -1;
    }
    return find(a, n);
}

int rigsys_ik_top_control(const Armature* a, int sys, int limb) {
    if (!a || limb < 0 || limb > 3 || sys != RIGSYS_RIGIFY) return -1;
    bool arm = limb < 2, right = limb_side(limb);
    char n[TL_BONE_NAME_LEN];
    snprintf(n, sizeof n, "%s.%c", arm ? "upper_arm_ik" : "thigh_ik", right ? 'R' : 'L');
    return find(a, n);
}

bool rigsys_keyable(const Armature* a, int sys, int bone, bool baked) {
    if (!a || bone < 0 || bone >= a->bone_count) return false;
    const Bone* b = &a->bones[bone];
    switch (sys) {
    case RIGSYS_ARP: return strncmp(b->name, "c_", 2) == 0 || (baked && b->deform);
    case RIGSYS_RIGIFY:
        if (strncmp(b->name, "MCH-", 4) == 0 || strncmp(b->name, "ORG-", 4) == 0) return false;
        return strncmp(b->name, "DEF-", 4) != 0 || baked;
    case RIGSYS_ADVSKEL:
    case RIGSYS_CONTROL: return !b->deform || baked || b->con_count == 0;
    default: return true;
    }
}
