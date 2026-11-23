// (2026-11-23) Included by rig_detect.c: the dynamic chains' presets and the self test (--rig-test)

// --- The dynamic chains' presets (ikrig.h DYN_*) --------------------------------------------------------------------------
int rig_chain_preset(int kind) {
    switch (kind) {
    case RIGC_TAIL: return DYN_TAIL;
    case RIGC_EAR: return DYN_EAR;
    case RIGC_HAIR: return DYN_HAIR;
    case RIGC_CHAIN: return DYN_CHAIN;
    case RIGC_CAPE: return DYN_CAPE;
    case RIGC_ACCESSORY: return DYN_EARRING;
    default: return DYN_STRAP;
    }
}

// --- The self test ----------------------------------------------------------------------------------------------------
// A humanoid in a T pose facing -Y (its left at +X), cm; each convention names its bones (NULL: it has none)
enum { H_HIPS, H_SP1, H_SP2, H_CHEST, H_NECK, H_HEAD, H_HEADEND, H_CLA_L, H_UA_L, H_FA_L, H_HAND_L, H_HEND_L, H_TH_L, H_SH_L, H_FOOT_L, H_TOE_L, H_TEND_L,
       H_CLA_R, H_UA_R, H_FA_R, H_HAND_R, H_HEND_R, H_TH_R, H_SH_R, H_FOOT_R, H_TOE_R, H_TEND_R, H_COUNT };
static const int H_PARENT[H_COUNT] = { -1, H_HIPS, H_SP1, H_SP2, H_CHEST, H_NECK, H_HEAD, H_CHEST, H_CLA_L, H_UA_L, H_FA_L, H_HAND_L, H_HIPS, H_TH_L, H_SH_L, H_FOOT_L, H_TOE_L,
                                       H_CHEST, H_CLA_R, H_UA_R, H_FA_R, H_HAND_R, H_HIPS, H_TH_R, H_SH_R, H_FOOT_R, H_TOE_R };
static const float H_POS[H_TEND_L + 1][3] = { { 0, 0, 100 }, { 0, 0, 112 }, { 0, 0, 126 }, { 0, 0, 140 }, { 0, 0, 155 }, { 0, 0, 165 }, { 0, 0, 185 },
                                              { 3, 0, 150 }, { 18, 0, 150 }, { 45, 0, 150 }, { 70, 0, 150 }, { 80, 0, 150 },
                                              { 10, 0, 95 }, { 10, 0, 52 }, { 10, 0, 8 }, { 10, -12, 2 }, { 10, -20, 2 } };
typedef struct TestBone { const char* name; const char* parent; float pos[3]; } TestBone;

static void test_place(SkelMesh* m, const float p[3], float turn) {
    float c = cosf(turn), s = sinf(turn), (*w)[4] = m->bind_world[m->nbones];
    memset(w, 0, sizeof(float) * 12);
    w[0][0] = w[1][1] = w[2][2] = 1.0f;
    w[0][3] = p[0] * c - p[1] * s, w[1][3] = p[0] * s + p[1] * c, w[2][3] = p[2];
}
static void test_mesh(SkelMesh* m, const char* const names[H_COUNT], const TestBone* extra, int nextra, float turn) {
    memset(m, 0, sizeof *m);
    int idx[H_COUNT];
    for (int h = 0; h < H_COUNT; h++) {
        idx[h] = -1;
        if (!names[h]) continue;
        bool right = h >= H_CLA_R;
        int src = right ? h - (H_CLA_R - H_CLA_L) : h;
        float p[3] = { H_POS[src][0] * (right ? -1.0f : 1.0f), H_POS[src][1], H_POS[src][2] };
        int par = H_PARENT[h];
        while (par >= 0 && idx[par] < 0) par = H_PARENT[par];
        AnimBone* b = &m->bones[m->nbones];
        snprintf(b->name, sizeof b->name, "%s", names[h]);
        b->parent = (int16_t)(par >= 0 ? idx[par] : -1);
        test_place(m, p, turn);
        idx[h] = (int)m->nbones++;
    }
    for (int e = 0; e < nextra; e++) {
        AnimBone* b = &m->bones[m->nbones];
        snprintf(b->name, sizeof b->name, "%s", extra[e].name);
        b->parent = -1;
        for (uint32_t k = 0; extra[e].parent && k < m->nbones; k++)
            if (!strcmp(m->bones[k].name, extra[e].parent)) b->parent = (int16_t)k;
        test_place(m, extra[e].pos, turn);
        m->nbones++;
    }
}

static bool expect_bone(const SkelMesh* m, const char* ue, const char* want, const char* rig) {
    int16_t b = rig_bone_ue(m, ue);
    const char* got = b >= 0 ? m->bones[b].name : "(none)";
    if (strcmp(got, want)) {
        printf("[RealEngine] FAIL: rig_detect: %s: %s is %s, wanted %s\n", rig, ue, got, want);
        return false;
    }
    return true;
}

bool rig_detect_selftest(void) {
    static SkelMesh m;
    bool ok = true;
    // 1. Mixamo
    static const char* const MIX[H_COUNT] = { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2", "mixamorig:Neck", "mixamorig:Head",
                                              "mixamorig:HeadTop_End", "mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand",
                                              "mixamorig:LeftHandEnd", "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot", "mixamorig:LeftToeBase",
                                              "mixamorig:LeftToe_End", "mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm", "mixamorig:RightHand",
                                              "mixamorig:RightHandEnd", "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot", "mixamorig:RightToeBase",
                                              "mixamorig:RightToe_End" };
    test_mesh(&m, MIX, NULL, 0, 0.0f);
    rig_detect(&m);
    const RigInfo* ri = rig_info(&m);
    if (!ri || ri->rig_class != CRC_HUMANOID) printf("[RealEngine] FAIL: rig_detect: Mixamo isn't a humanoid\n"), ok = false;
    ok &= expect_bone(&m, "foot_l", "mixamorig:LeftFoot", "Mixamo") & expect_bone(&m, "calf_r", "mixamorig:RightLeg", "Mixamo") &
          expect_bone(&m, "thigh_l", "mixamorig:LeftUpLeg", "Mixamo") & expect_bone(&m, "ball_r", "mixamorig:RightToeBase", "Mixamo") &
          expect_bone(&m, "pelvis", "mixamorig:Hips", "Mixamo") & expect_bone(&m, "upperarm_l", "mixamorig:LeftArm", "Mixamo") &
          expect_bone(&m, "clavicle_r", "mixamorig:RightShoulder", "Mixamo") & expect_bone(&m, "head", "mixamorig:Head", "Mixamo");
    printf("[RealEngine] rig_detect: Mixamo: '%s', %s, %d of %d main roles, forward (%.2f, %.2f), %.2f ms\n", ri ? ri->convention : "-",
           ri ? rig_class_label(ri->rig_class) : "-", ri ? ri->main_mapped : 0, ri ? ri->main_total : 0, ri ? ri->forward[0] : 0.0f, ri ? ri->forward[1] : 0.0f,
           ri ? ri->ms : 0.0);
    rig_forget(&m);
    // 2. Rigify's deforming bones with its controls, a tail and two ears
    static const char* const RIG[H_COUNT] = { "DEF-spine", "DEF-spine.001", "DEF-spine.002", "DEF-spine.003", "DEF-spine.004", "DEF-spine.006", NULL,
                                              "DEF-shoulder.L", "DEF-upper_arm.L", "DEF-forearm.L", "DEF-hand.L", NULL, "DEF-thigh.L", "DEF-shin.L", "DEF-foot.L", "DEF-toe.L", NULL,
                                              "DEF-shoulder.R", "DEF-upper_arm.R", "DEF-forearm.R", "DEF-hand.R", NULL, "DEF-thigh.R", "DEF-shin.R", "DEF-foot.R", "DEF-toe.R", NULL };
    static const TestBone RIGX[] = {
        { "root", NULL, { 0, 0, 0 } }, { "torso", "root", { 0, 0, 105 } }, { "foot_ik.L", "root", { 10, 5, 4 } }, { "foot_ik.R", "root", { -10, 5, 4 } },
        { "thigh_ik_target.L", "root", { 10, -40, 52 } }, { "thigh_ik_target.R", "root", { -10, -40, 52 } }, { "MCH-thigh_ik.L", "DEF-spine", { 10, 0, 95 } },
        { "upper_arm_fk.L", "DEF-spine.003", { 18, 0, 150 } },
        { "DEF-tail", "DEF-spine", { 0, 12, 100 } }, { "DEF-tail.001", "DEF-tail", { 0, 30, 95 } }, { "DEF-tail.002", "DEF-tail.001", { 0, 48, 90 } },
        { "DEF-tail.003", "DEF-tail.002", { 0, 64, 88 } },
        { "DEF-ear.L", "DEF-spine.006", { 5, 0, 180 } }, { "DEF-ear.L.001", "DEF-ear.L", { 7, 0, 195 } }, { "DEF-ear.L.002", "DEF-ear.L.001", { 8, 0, 208 } },
        { "DEF-ear.R", "DEF-spine.006", { -5, 0, 180 } }, { "DEF-ear.R.001", "DEF-ear.R", { -7, 0, 195 } }, { "DEF-ear.R.002", "DEF-ear.R.001", { -8, 0, 208 } },
    };
    test_mesh(&m, RIG, RIGX, (int)(sizeof RIGX / sizeof RIGX[0]), 0.0f);
    rig_detect(&m);
    ri = rig_info(&m);
    ok &= expect_bone(&m, "foot_l", "DEF-foot.L", "Rigify") & expect_bone(&m, "calf_r", "DEF-shin.R", "Rigify") &
          expect_bone(&m, "pelvis", "DEF-spine", "Rigify") & expect_bone(&m, "hand_r", "DEF-hand.R", "Rigify");
    int tails = 0, ears = 0, ctrls = 0;
    for (uint32_t c = 0; ri && c < ri->nchains; c++) tails += ri->chains[c].kind == RIGC_TAIL, ears += ri->chains[c].kind == RIGC_EAR;
    for (uint32_t b = 0; ri && b < m.nbones; b++) ctrls += ri->kind[b] == RIGB_CONTROL || ri->kind[b] == RIGB_IK_HANDLE || ri->kind[b] == RIGB_POLE || ri->kind[b] == RIGB_MECH;
    int fik = -1;
    for (uint32_t b = 0; b < m.nbones; b++)
        if (!strcmp(m.bones[b].name, "foot_ik.L")) fik = (int)b;
    if (!ri || tails != 1 || ears != 2) printf("[RealEngine] FAIL: rig_detect: Rigify: %d tails, %d ears (wanted 1, 2)\n", tails, ears), ok = false;
    if (!ri || fik < 0 || ri->kind[fik] == RIGB_DEFORM) printf("[RealEngine] FAIL: rig_detect: Rigify: foot_ik.L taken for a deforming bone\n"), ok = false;
    printf("[RealEngine] rig_detect: Rigify: '%s' (%s), %d chains (%d tail, %d ears), %d controls, %.2f ms\n", ri ? ri->convention : "-",
           ri ? rig_system_label(ri->system) : "-", ri ? (int)ri->nchains : 0, tails, ears, ctrls, ri ? ri->ms : 0.0);
    rig_forget(&m);
    // 3. Unreal's names turned to face +X (the engine's Mannequin): found, the forward right, a missing spine to the nearest
    static const char* const UEN[H_COUNT] = { "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head", NULL, "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", NULL,
                                              "thigh_l", "calf_l", "foot_l", "ball_l", NULL, "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r", NULL, "thigh_r", "calf_r",
                                              "foot_r", "ball_r", NULL };
    test_mesh(&m, UEN, NULL, 0, 1.5707963f);
    rig_detect(&m);
    ri = rig_info(&m);
    ok &= expect_bone(&m, "spine_05", "spine_03", "UE") & expect_bone(&m, "foot_r", "foot_r", "UE");
    if (!ri || ri->forward[0] < 0.9f) printf("[RealEngine] FAIL: rig_detect: UE turned: forward (%.2f, %.2f), wanted +X\n", ri ? ri->forward[0] : 0.0f, ri ? ri->forward[1] : 0.0f), ok = false;
    rig_forget(&m);
    // 4. No names at all (Bone, Bone.001...): the skeleton's shape
    static char nm[H_COUNT][16];
    static const char* NN[H_COUNT];
    for (int h = 0; h < H_COUNT; h++) {
        if (h == 0) snprintf(nm[h], sizeof nm[h], "Bone");
        else snprintf(nm[h], sizeof nm[h], "Bone.%03d", h);
        NN[h] = nm[h];
    }
    test_mesh(&m, NN, NULL, 0, 0.0f);
    rig_detect(&m);
    ri = rig_info(&m);
    ok &= expect_bone(&m, "foot_l", nm[H_FOOT_L], "no names") & expect_bone(&m, "thigh_r", nm[H_TH_R], "no names") & expect_bone(&m, "hand_l", nm[H_HAND_L], "no names");
    printf("[RealEngine] rig_detect: no names: %s, %d of %d main roles\n", ri ? rig_class_label(ri->rig_class) : "-", ri ? ri->main_mapped : 0, ri ? ri->main_total : 0);
    rig_forget(&m);
    // 5. Character Creator / AccuRig
    static const char* const CC[H_COUNT] = { "CC_Base_Hip", "CC_Base_Waist", "CC_Base_Spine01", "CC_Base_Spine02", "CC_Base_NeckTwist01", "CC_Base_Head", NULL,
                                             "CC_Base_L_Clavicle", "CC_Base_L_Upperarm", "CC_Base_L_Forearm", "CC_Base_L_Hand", NULL, "CC_Base_L_Thigh", "CC_Base_L_Calf",
                                             "CC_Base_L_Foot", "CC_Base_L_ToeBase", NULL, "CC_Base_R_Clavicle", "CC_Base_R_Upperarm", "CC_Base_R_Forearm", "CC_Base_R_Hand",
                                             NULL, "CC_Base_R_Thigh", "CC_Base_R_Calf", "CC_Base_R_Foot", "CC_Base_R_ToeBase", NULL };
    test_mesh(&m, CC, NULL, 0, 0.0f);
    rig_detect(&m);
    ok &= expect_bone(&m, "foot_l", "CC_Base_L_Foot", "CC") & expect_bone(&m, "calf_r", "CC_Base_R_Calf", "CC");
    rig_forget(&m);
    printf("[RealEngine] rig_detect: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
