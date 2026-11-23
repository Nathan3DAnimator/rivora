// The Motion Matching examples (Tools > Animation Packs > Motion Matching Examples; notas-dev/animacao.md), included by
// anim.c: four characters of their own styles on the Mannequin's skeleton, each with only an idle, a walk and a run (the
// Soldier also strafes) and an Animation Blueprint whose Locomotion is Motion Matching over them, its made animations'
// settings set to the style. The starts, stops, speed changes and directions in between are made from those few clips
// (anim_mm_gen.inc), in each one's style: the Brute's slow heavy steps start and stop slowly and sway, the Runner snaps,
// the Sneak stays crouched, the Soldier gets every 15 degrees between forward and its strafes.
// In Content/Characters/MotionMatching/<Name>: SK_<Name>, <Name>_Idle, _Walk, _Run (, _StrafeLeft, _StrafeRight), ABP_<Name>.
// And LongTake: one long clip (MM_LongTake: idle, starts, walk, turns, run, stops, a pivot, its path and turn on the
// root) and ABP_LongTake, whose database is that one file: the search finds its moments, mirrored too.
// And JumpTake: one take with jumps in it (MM_JumpTake: a jump in place, one walking, one running, the flight on the
// root's height) and ABP_JumpTake: Jump finds its takeoffs, the clip carries the capsule up and down.
// And Authored: its own starts and stops (Authored_StartWalk, _StopWalk, _StopRun: files with root motion, as if made by
// hand) with an idle, a walk, a jog and a run in ABP_Authored: they play before the made ones.

// --- Their gaits (anim_motion.inc's Gait: speed, dir, duty, lift, heel kick, height, bob, sway, hip yaw / roll / pitch,
// lean, width, heel strike, toe off, arm forward / back / out, elbow, elbow swing, twist, head bob, run) ---

// the Brute: heavy, wide, slow steps, the arms out and the body rolling over each foot
static const Gait g_brute_walk = { 105.0f, { 1, 0 }, 0.65f, 6.0f, 0.0f, 92.0f, 2.6f, 4.5f, 8.0f, 6.0f, 4.0f, 6.0f, 15.0f, 10.0f, 16.0f,
                                   18.0f, 12.0f, 18.0f, 22.0f, 8.0f, 1.3f, 1.6f, false };
static const Gait g_brute_run = { 320.0f, { 1, 0 }, 0.45f, 9.0f, 10.0f, 90.0f, 3.6f, 3.0f, 9.0f, 7.0f, 7.0f, 10.0f, 14.0f, 6.0f, 22.0f,
                                  26.0f, 18.0f, 16.0f, 70.0f, 10.0f, 1.2f, 2.2f, true };
// the Runner: light, quick, high knees and a long flight
static const Gait g_runner_walk = { 150.0f, { 1, 0 }, 0.58f, 13.0f, 4.0f, 96.0f, 1.4f, 1.6f, 7.0f, 3.0f, 3.0f, 5.0f, 9.0f, 18.0f, 28.0f,
                                    26.0f, 18.0f, 6.0f, 22.0f, 14.0f, 1.0f, 0.8f, false };
static const Gait g_runner_run = { 540.0f, { 1, 0 }, 0.32f, 22.0f, 36.0f, 94.0f, 3.4f, 0.8f, 11.0f, 6.0f, 9.0f, 13.0f, 8.5f, 6.0f, 36.0f,
                                   58.0f, 44.0f, 5.0f, 86.0f, 18.0f, 1.3f, 2.2f, true };
// the Sneak: crouched all along, soft careful steps, a low quick dash
static const Gait g_sneak_walk = { 75.0f, { 1, 0 }, 0.68f, 7.0f, 0.0f, 64.0f, 1.0f, 2.6f, 5.0f, 3.0f, 16.0f, 18.0f, 13.5f, 6.0f, 12.0f,
                                   10.0f, 8.0f, 10.0f, 52.0f, 6.0f, 1.0f, 0.6f, false };
static const Gait g_sneak_run = { 230.0f, { 1, 0 }, 0.45f, 12.0f, 8.0f, 70.0f, 1.8f, 1.6f, 7.0f, 4.0f, 18.0f, 20.0f, 12.0f, 6.0f, 20.0f,
                                  20.0f, 14.0f, 9.0f, 70.0f, 10.0f, 1.0f, 1.0f, true };
// the Soldier: upright, the arms close (as if carrying), even steps
static const Gait g_soldier_walk = { 125.0f, { 1, 0 }, 0.61f, 9.0f, 0.0f, 95.0f, 1.4f, 1.8f, 5.0f, 3.0f, 2.0f, 3.0f, 11.0f, 14.0f, 20.0f,
                                     12.0f, 8.0f, 5.0f, 34.0f, 6.0f, 1.0f, 0.8f, false };
static const Gait g_soldier_run = { 420.0f, { 1, 0 }, 0.37f, 15.0f, 22.0f, 95.0f, 3.0f, 1.0f, 9.0f, 5.0f, 6.0f, 9.0f, 9.5f, 8.0f, 30.0f,
                                    30.0f, 22.0f, 6.0f, 80.0f, 12.0f, 1.2f, 1.8f, true };

static void qx_brute_idle(float t, float len, QPose* P) { stand_pose(t, len, P, 0.12f); } // (the knees a little bent)
static void qx_brute_walk(float t, float len, QPose* P) { gait_pose(t, len, &g_brute_walk, P); }
static void qx_brute_run(float t, float len, QPose* P) { gait_pose(t, len, &g_brute_run, P); }
static void qx_runner_walk(float t, float len, QPose* P) { gait_pose(t, len, &g_runner_walk, P); }
static void qx_runner_run(float t, float len, QPose* P) { gait_pose(t, len, &g_runner_run, P); }
static void qx_sneak_walk(float t, float len, QPose* P) { gait_pose(t, len, &g_sneak_walk, P); }
static void qx_sneak_run(float t, float len, QPose* P) { gait_pose(t, len, &g_sneak_run, P); }
static void qx_soldier_walk(float t, float len, QPose* P) { gait_pose(t, len, &g_soldier_walk, P); }
static void qx_soldier_run(float t, float len, QPose* P) { gait_pose(t, len, &g_soldier_run, P); }
static void qx_soldier_strafe_l(float t, float len, QPose* P) { strafe_pose(t, len, P, -1.0f, 24.0f); }
static void qx_soldier_strafe_r(float t, float len, QPose* P) { strafe_pose(t, len, P, 1.0f, 24.0f); }

typedef struct MmExample {
    const char* name;
    MBody body;
    CoreClip clips[5]; // (named <name>_ + the clip's)
    int nclips;
    float gen[ANIM_MMGEN]; // its made animations' settings (AnimState.mmgen)
} MmExample;

static const MmExample g_mm_examples[] = {
    { "Brute",
      { { 30, 54, 28 }, { 24, 36, 20 }, { 17, 20 }, 1.35f, 8.0f, { 23, 20, 26 }, false, { 0.42f, 0.48f, 0.36f }, { 0.16f, 0.18f, 0.12f }, { 0.85f, 0.55f, 0.15f } },
      { { "Idle", 4.8f, true, qx_brute_idle, 0 }, { "Walk", 1.35f, true, qx_brute_walk, 2 }, { "Run", 0.9f, true, qx_brute_run, 2 } },
      3, { 0, 0, 0, 1.5f, 1.0f, 0.6f, 22.5f, 1.0f } },
    { "Runner",
      { { 19, 32, 20 }, { 19, 30, 17 }, { 10, 12.5f }, 0.85f, 4.8f, { 19, 17, 23 }, true, { 0.2f, 0.62f, 0.66f }, { 0.08f, 0.14f, 0.16f }, { 0.95f, 0.85f, 0.25f } },
      { { "Idle", 4.8f, true, qpose_idle, 0 }, { "Walk", 0.9f, true, qx_runner_walk, 2 }, { "Run", 0.6f, true, qx_runner_run, 2 } },
      3, { 0, 0, 0, 0.7f, 1.3f, 0.25f, 22.5f, 1.0f } },
    { "Sneak",
      { { 22, 40, 22 }, { 20, 32, 18 }, { 12, 15 }, 1.0f, 5.5f, { 21, 18, 25 }, false, { 0.16f, 0.16f, 0.2f }, { 0.06f, 0.06f, 0.08f }, { 0.75f, 0.1f, 0.15f } },
      { { "Idle", 4.0f, true, qpose_crouch_idle, 0 }, { "Walk", 1.4f, true, qx_sneak_walk, 2 }, { "Run", 0.8f, true, qx_sneak_run, 2 } },
      3, { 0, 0, 0, 1.0f, 0.6f, 0.15f, 22.5f, 1.0f } },
    { "Soldier",
      { { 25, 46, 24 }, { 21, 32, 18 }, { 14, 17 }, 1.12f, 6.5f, { 22, 19, 26 }, false, { 0.42f, 0.45f, 0.32f }, { 0.18f, 0.2f, 0.14f }, { 0.2f, 0.65f, 0.35f } },
      { { "Idle", 4.8f, true, qpose_idle, 0 }, { "Walk", 1.05f, true, qx_soldier_walk, 2 }, { "Run", 0.7f, true, qx_soldier_run, 2 },
        { "StrafeLeft", 0.8f, true, qx_soldier_strafe_l, 2 }, { "StrafeRight", 0.8f, true, qx_soldier_strafe_r, 2 } },
      5, { 0, 0, 0, 1.0f, 0.8f, 0.35f, 15.0f, 1.0f } },
};

// The pack: each character's mesh and clips (made again: the engine's), its Animation Blueprint (when missing: then the
// project's to edit). The files made
static bool mm_record_take(const char* path); // (anim_mm.inc)
static bool mm_record_jump_take(const char* path);
static uint32_t mm_make_authored(const char* dir);

static uint32_t anim_make_mm_examples(void) {
    uint32_t made = 0;
    for (size_t i = 0; i < sizeof g_mm_examples / sizeof g_mm_examples[0]; i++) {
        const MmExample* x = &g_mm_examples[i];
        char dir[700], path[800], vpath[800], folder[300], sk[64], abp[64];
        snprintf(dir, sizeof dir, "%s/Characters/MotionMatching/%s", A.root, x->name);
        snprintf(folder, sizeof folder, "/Content/Characters/MotionMatching/%s", x->name);
        if (!re_make_dirs(dir)) continue;
        snprintf(sk, sizeof sk, "SK_%s", x->name), snprintf(abp, sizeof abp, "ABP_%s", x->name);
        snprintf(path, sizeof path, "%s/%s.rskm", dir, sk);
        vpath_of(path, vpath, sizeof vpath);
        SkelMesh* m = make_mannequin_body(path, vpath, &x->body);
        if (!m) continue;
        made += anim_mesh_save(m);
        char names[5][ANIM_NAME];
        for (int c = 0; c < x->nclips; c++) {
            CoreClip clip = x->clips[c];
            snprintf(names[c], ANIM_NAME, "%s_%s", x->name, x->clips[c].name);
            clip.name = names[c];
            made += engine_clip(m, dir, &clip, CLIP_ALWAYS);
        }
        free(m);
        anim_rescan();
        BpAsset* a = NULL;
        AnimGraph* ag = abp_begin(folder, abp, sk, &a);
        if (!ag) continue;
        AnimState* lo = &ag->states[ag->nstates++];
        snprintf(lo->name, ANIM_NAME, "Locomotion");
        lo->kind = ANIM_STATE_MOTION_MATCH, lo->loop = true, lo->rate = 1.0f;
        snprintf(lo->param, ANIM_NAME, "Speed");
        for (int c = 0; c < x->nclips; c++) snprintf(lo->samples[lo->nsamples++], ANIM_NAME, "%s", names[c]);
        memcpy(lo->mmgen, x->gen, sizeof lo->mmgen);
        abp_jump(ag);
        made += bp_save(a);
    }
    // the long take: one file that does it all (idle, starts, walk, turns, run, stops, a pivot), recorded from the
    // Mannequin's Motion Matching with its path and turn on the root; its Animation Blueprint's database is that file alone
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s/Characters/MotionMatching/LongTake", A.root);
    snprintf(path, sizeof path, "%s/MM_LongTake.ranim", dir);
    if (re_make_dirs(dir) && mm_record_take(path)) {
        made++;
        BpAsset* a = NULL;
        AnimGraph* ag = abp_begin("/Content/Characters/MotionMatching/LongTake", "ABP_LongTake", "SK_Mannequin", &a);
        if (ag) {
            AnimState* lo = &ag->states[ag->nstates++];
            snprintf(lo->name, ANIM_NAME, "Locomotion");
            lo->kind = ANIM_STATE_MOTION_MATCH, lo->loop = true, lo->rate = 1.0f;
            snprintf(lo->param, ANIM_NAME, "Speed");
            snprintf(lo->samples[lo->nsamples++], ANIM_NAME, "MM_LongTake");
            abp_jump(ag);
            made += bp_save(a);
        }
    }
    // the jump take: jumps in one file (in place, walking, running) with their height on the root - vertical root motion
    snprintf(dir, sizeof dir, "%s/Characters/MotionMatching/JumpTake", A.root);
    snprintf(path, sizeof path, "%s/MM_JumpTake.ranim", dir);
    if (re_make_dirs(dir) && mm_record_jump_take(path)) {
        made++;
        BpAsset* a = NULL;
        AnimGraph* ag = abp_begin("/Content/Characters/MotionMatching/JumpTake", "ABP_JumpTake", "SK_Mannequin", &a);
        if (ag) {
            AnimState* lo = &ag->states[ag->nstates++];
            snprintf(lo->name, ANIM_NAME, "Locomotion");
            lo->kind = ANIM_STATE_MOTION_MATCH, lo->loop = true, lo->rate = 1.0f;
            snprintf(lo->param, ANIM_NAME, "Speed");
            snprintf(lo->samples[lo->nsamples++], ANIM_NAME, "MM_JumpTake");
            abp_jump(ag); // (its Jump state by IsInAir: a jump the clip carries is not falling, it stays in Motion Matching)
            made += bp_save(a);
        }
    }
    // Authored: the user's own starts and stops (files with their path on the root, as if made by hand) next to an idle,
    // a walk, a jog and a run: Motion Matching plays them before its made ones
    snprintf(dir, sizeof dir, "%s/Characters/MotionMatching/Authored", A.root);
    uint32_t own = re_make_dirs(dir) ? mm_make_authored(dir) : 0;
    if (own) {
        made += own;
        anim_rescan();
        BpAsset* a = NULL;
        AnimGraph* ag = abp_begin("/Content/Characters/MotionMatching/Authored", "ABP_Authored", "SK_Mannequin", &a);
        if (ag) {
            static const char* const clips[7] = { "MM_Idle", "MM_Walk", "MM_Jog", "MM_Run", "Authored_StartWalk", "Authored_StopWalk", "Authored_StopRun" };
            AnimState* lo = &ag->states[ag->nstates++];
            snprintf(lo->name, ANIM_NAME, "Locomotion");
            lo->kind = ANIM_STATE_MOTION_MATCH, lo->loop = true, lo->rate = 1.0f;
            snprintf(lo->param, ANIM_NAME, "Speed");
            for (int k = 0; k < 7; k++) snprintf(lo->samples[lo->nsamples++], ANIM_NAME, "%s", clips[k]);
            abp_jump(ag);
            made += bp_save(a);
        }
    }
    return made;
}
