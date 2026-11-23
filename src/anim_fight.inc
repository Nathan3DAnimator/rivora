// The fighting clips (sugestão #34; included by anim.c after anim_motion.inc): the Mannequin's guard, footwork, strikes,
// defense and reactions keyed the way an animator keys them — anticipation (a chamber, a dip, the hip loading the
// other way), a fast snap out, a short hold at full reach, the retraction back to the guard; the hips and the chest
// turning into each strike, the rear heel lifting on a cross, the standing leg solved by IK under a kick. Each clip
// starts and ends on the guard, so montages chain them into combos without a pop.
// anim_make_fight_pack: the clips in Characters/Mannequin/Fighting, a montage (DefaultSlot, the whole body) per action,
// and ABP_Fighter (Locomotion: the guard at rest, the fighting step, a jog; Jump; the montages' slots).

static float fenv(float u, float a, float b, float c) { // 0 before a, eased up to 1 at b, held until c, eased back to 0 at 1
    if (u <= a) return 0.0f;
    if (u < b) return smooth01((u - a) / (b - a));
    if (u < c) return 1.0f;
    return 1.0f - smooth01((u - c) / (1.0f - c));
}
static void farm(QPose* P, int side, float abduct, float raise, float twist, float elbow) {
    float s = side ? 1.0f : -1.0f;
    qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * abduct, raise, s * twist);
    qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0.0f, elbow, s * 6.0f);
    qp_set(P, side ? MB_HAND_R : MB_HAND_L, 0.0f, -8.0f, s * 4.0f);
}

// The guard (orthodox): left foot forward, the hips turned 25 degrees, knees soft, hands up (the rear one at the chin),
// chin down; the weight bouncing on the balls of the feet. turn: the hips' yaw added (a strike's rotation); dz: the
// hips lowered; heel: the rear heel lifted (cm)
static void fguard(float t, QPose* P, float turn, float dz, float heel, float fwd) {
    qp_reset(P);
    float b = sinf(2.0f * PI * 2.2f * t), sway = sinf(2.0f * PI * 1.1f * t);
    P->pelvis[0] = 2.0f + fwd, P->pelvis[1] = 1.5f * sway, P->pelvis[2] = 88.0f - dz - 1.6f * (0.5f + 0.5f * b);
    qp_set(P, MB_PELVIS, 1.5f * sway, 4.0f, 25.0f + turn);
    float lead[3] = { 17.0f, -15.0f, MO_ANKLE }, rear[3] = { -21.0f, 15.0f, MO_ANKLE + heel };
    leg_ik(P, 0, lead, 0.0f, -14.0f);
    leg_ik(P, 1, rear, -heel * 1.6f, 22.0f);
    // the chest faces the rival more than the hips do; leaning in a little
    qp_set(P, MB_SPINE1, 0.0f, 5.0f, -turn * 0.25f - 6.0f);
    qp_set(P, MB_SPINE2, 0.0f, 5.0f, -turn * 0.2f - 6.0f);
    qp_set(P, MB_SPINE3, 0.0f, 4.0f, -turn * 0.15f - 5.0f);
    qp_set(P, MB_NECK, 0.0f, 3.0f, -5.0f), qp_set(P, MB_HEAD, 0.0f, 6.0f, -6.0f - turn * 0.2f);
    farm(P, 0, 22.0f, -58.0f + 2.0f * b, -10.0f, -112.0f);
    farm(P, 1, 16.0f, -44.0f + 2.0f * b, -14.0f, -128.0f);
    qp_set(P, MB_CLAV_L, -3.0f, 0.0f, 0.0f), qp_set(P, MB_CLAV_R, 3.0f, 0.0f, 0.0f);
}

static void qpose_fguard(float t, float len, QPose* P) { (void)len; fguard(t, P, 0.0f, 0.0f, 0.0f, 0.0f); }

// the fighting step: short shuffles, the feet never crossing, the guard kept
static void qpose_fstep(float t, float len, QPose* P) {
    fguard(t, P, 0.0f, 1.5f, 0.0f, 0.0f);
    float u = wrap01(t / len);
    for (int side = 0; side < 2; side++) {
        float ph = wrap01(u + (side ? 0.5f : 0.0f)), lift = fmaxf(sinf(2.0f * PI * ph), 0.0f), slide = cosf(2.0f * PI * ph);
        float a[3] = { (side ? -21.0f : 17.0f) + 7.0f * slide, side ? 15.0f : -15.0f, MO_ANKLE + 5.0f * lift };
        leg_ik(P, side, a, -6.0f * lift, side ? 22.0f : -14.0f);
    }
}

// the jab: the lead hand snaps straight out (0.1 s), the lead shoulder rolls in, back to the chin
static void qpose_jab(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), e = fenv(u, 0.05f, 0.3f, 0.45f);
    fguard(t, P, 10.0f * e, 0.0f, 0.0f, 3.0f * e);
    farm(P, 0, lerpf(22.0f, 4.0f, e), lerpf(-58.0f, -90.0f, e), lerpf(-10.0f, 8.0f, e), lerpf(-112.0f, -6.0f, e));
    qp_set(P, MB_CLAV_L, -3.0f - 8.0f * e, 0.0f, -6.0f * e);
}
// the cross: the rear hand drives through as the hips and chest turn over, the rear heel lifts, the weight forward
static void qpose_cross(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), e = fenv(u, 0.1f, 0.36f, 0.5f);
    fguard(t, P, -45.0f * e, 2.0f * e, 7.0f * e, 9.0f * e);
    farm(P, 1, lerpf(16.0f, 2.0f, e), lerpf(-44.0f, -92.0f, e), lerpf(-14.0f, 10.0f, e), lerpf(-128.0f, -5.0f, e));
    qp_set(P, MB_CLAV_R, 3.0f + 9.0f * e, 0.0f, 8.0f * e);
    qp_set(P, MB_SPINE2, 0.0f, 5.0f + 4.0f * e, 6.0f * e), qp_set(P, MB_SPINE3, 0.0f, 4.0f + 3.0f * e, 8.0f * e);
}
// the lead hook: loaded by turning away, then the elbow swings level across as the hips whip round
static void qpose_hook(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), load = fenv(u, 0.0f, 0.18f, 0.22f) * (1.0f - smooth01((u - 0.22f) / 0.1f)), e = fenv(u, 0.22f, 0.45f, 0.58f);
    fguard(t, P, -14.0f * load + 40.0f * e, 3.0f * (load + e), 0.0f, 2.0f * e);
    farm(P, 0, lerpf(22.0f, 82.0f, e), lerpf(-58.0f, -18.0f, e), lerpf(-10.0f, -40.0f, e), -95.0f + 10.0f * e);
    qp_set(P, MB_SPINE3, 0.0f, 4.0f, -5.0f + 20.0f * e - 10.0f * load);
}
// the rear uppercut: a dip (the knees load), then the fist drives up the middle as the legs extend
static void qpose_uppercut(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), dip = fenv(u, 0.0f, 0.22f, 0.26f) * (1.0f - smooth01((u - 0.26f) / 0.12f)), e = fenv(u, 0.26f, 0.48f, 0.6f);
    fguard(t, P, -30.0f * e, 10.0f * dip - 4.0f * e, 4.0f * e, 5.0f * e);
    farm(P, 1, lerpf(16.0f, 8.0f, e), lerpf(-44.0f + 25.0f * dip, -150.0f, e), lerpf(-14.0f, 20.0f, e), lerpf(-128.0f + 20.0f * dip, -72.0f, e));
    qp_set(P, MB_SPINE1, 0.0f, 5.0f + 10.0f * dip - 10.0f * e, -4.0f), qp_set(P, MB_SPINE2, 0.0f, 5.0f + 8.0f * dip - 8.0f * e, -6.0f);
}
// the rear roundhouse kick: the knee chambers up, the hips turn over the standing foot, the shin snaps out and back;
// the arms keep the guard (the rear one swings down for balance)
static void qpose_kick(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), ch = fenv(u, 0.05f, 0.28f, 0.62f), e = fenv(u, 0.26f, 0.4f, 0.5f);
    fguard(t, P, -70.0f * ch, 2.0f * ch, 0.0f, 0.0f);
    float stand[3] = { 12.0f, -8.0f, MO_ANKLE };
    leg_ik(P, 0, stand, 0.0f, -40.0f * ch);
    qp_set(P, MB_THIGH_R, -45.0f * ch, -88.0f * ch, 0.0f);
    qp_set(P, MB_CALF_R, 0.0f, lerpf(115.0f * ch, 8.0f, e), 0.0f);
    qp_set(P, MB_FOOT_R, 0.0f, 25.0f * ch, 0.0f);
    qp_set(P, MB_SPINE1, -12.0f * ch, -6.0f * ch, 0.0f), qp_set(P, MB_SPINE2, -10.0f * ch, -6.0f * ch, 0.0f);
    farm(P, 1, lerpf(16.0f, 30.0f, ch), lerpf(-44.0f, 10.0f, ch), -14.0f, lerpf(-128.0f, -40.0f, ch));
}
// the jump kick (in the air): legs tucked, then the lead leg shoots out, the body leaning back over it
static void qpose_airkick(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), tuck = fenv(u, 0.0f, 0.2f, 0.75f), e = fenv(u, 0.25f, 0.4f, 0.6f);
    fguard(t, P, -20.0f * e, 0.0f, 0.0f, 0.0f);
    P->pelvis[2] = 92.0f;
    qp_set(P, MB_THIGH_L, 0.0f, lerpf(-100.0f * tuck, -95.0f, e), 0.0f), qp_set(P, MB_CALF_L, 0.0f, lerpf(120.0f * tuck, 5.0f, e), 0.0f);
    qp_set(P, MB_FOOT_L, 0.0f, 20.0f * e, 0.0f);
    qp_set(P, MB_THIGH_R, 0.0f, -70.0f * tuck, 0.0f), qp_set(P, MB_CALF_R, 0.0f, 125.0f * tuck, 0.0f);
    qp_set(P, MB_SPINE1, 0.0f, -14.0f * e, 0.0f), qp_set(P, MB_SPINE2, 0.0f, -10.0f * e, 0.0f);
}
// the block: forearms up in front of the face, elbows in, a lower stance; a small bob (loops)
static void qpose_fblock(float t, float len, QPose* P) {
    (void)len;
    fguard(t, P, -5.0f, 4.0f, 0.0f, 0.0f);
    float b = 2.0f * sinf(2.0f * PI * 1.6f * t);
    farm(P, 0, 12.0f, -98.0f + b, -24.0f, -148.0f);
    farm(P, 1, 10.0f, -94.0f + b, -26.0f, -150.0f);
    qp_set(P, MB_SPINE2, 0.0f, 10.0f, -4.0f), qp_set(P, MB_HEAD, 0.0f, 14.0f, -4.0f);
}
// hit: the head and chest snap back, the hips pushed back, the guard drops a little, then recovers
static void qpose_fhit(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), h = fenv(u, 0.0f, 0.12f, 0.3f);
    fguard(t, P, 8.0f * h, 3.0f * h, 0.0f, -10.0f * h);
    qp_set(P, MB_SPINE2, 4.0f * h, 5.0f - 16.0f * h, -6.0f), qp_set(P, MB_SPINE3, 3.0f * h, 4.0f - 14.0f * h, -5.0f);
    qp_set(P, MB_HEAD, 6.0f * h, 6.0f - 24.0f * h, -6.0f + 10.0f * h);
    farm(P, 0, 22.0f + 18.0f * h, -58.0f + 30.0f * h, -10.0f, -112.0f + 30.0f * h);
}
// the dodge (a sway back and down out of reach, the guard kept)
static void qpose_fdodge(float t, float len, QPose* P) {
    float u = fminf(t / len, 1.0f), d = fenv(u, 0.0f, 0.25f, 0.6f);
    fguard(t, P, -10.0f * d, 10.0f * d, 0.0f, -14.0f * d);
    qp_set(P, MB_SPINE1, 0.0f, 5.0f - 14.0f * d, -6.0f), qp_set(P, MB_SPINE2, 0.0f, 5.0f - 12.0f * d, -6.0f);
    qp_set(P, MB_HEAD, 0.0f, 6.0f + 6.0f * d, -6.0f);
}
// knocked out: the legs give, the body falls back to the ground and stays there
static void qpose_fko(float t, float len, QPose* P) {
    qp_reset(P);
    float u = fminf(t / (len * 0.7f), 1.0f), e = smooth01(u), k = smooth01(u * 1.6f);
    P->pelvis[0] = -35.0f * e, P->pelvis[2] = lerpf(88.0f, 13.0f, e * e);
    qp_set(P, MB_PELVIS, 0.0f, -82.0f * e, 25.0f * (1.0f - e));
    qp_set(P, MB_THIGH_L, 6.0f, lerpf(0.0f, -15.0f, e) - 40.0f * k * (1.0f - e), 0.0f), qp_set(P, MB_CALF_L, 0.0f, 70.0f * k * (1.0f - e) + 10.0f * e, 0.0f);
    qp_set(P, MB_THIGH_R, -6.0f, lerpf(0.0f, -35.0f, e), 0.0f), qp_set(P, MB_CALF_R, 0.0f, 40.0f * e, 0.0f);
    qp_set(P, MB_SPINE2, 0.0f, -10.0f * e, 0.0f), qp_set(P, MB_HEAD, 6.0f * e, -20.0f * e, 18.0f * e);
    farm(P, 0, lerpf(22.0f, 70.0f, e), lerpf(-58.0f, 20.0f, e), 0.0f, lerpf(-112.0f, -20.0f, e));
    farm(P, 1, lerpf(16.0f, 55.0f, e), lerpf(-44.0f, 35.0f, e), 0.0f, lerpf(-128.0f, -30.0f, e));
}
// the win: both fists up, bouncing (loops)
static void qpose_fwin(float t, float len, QPose* P) {
    qp_reset(P);
    float u = wrap01(t / len), b = sinf(2.0f * PI * u);
    P->pelvis[2] = 94.0f + 2.0f * b;
    static const float feet[2][3] = { { 4.0f, -14.0f, 0.0f }, { -4.0f, 14.0f, 0.0f } };
    for (int side = 0; side < 2; side++) {
        float a[3] = { feet[side][0], feet[side][1], MO_ANKLE };
        leg_ik(P, side, a, 0.0f, side ? 8.0f : -8.0f);
        farm(P, side, 70.0f + 10.0f * b, -100.0f, 0.0f, -35.0f - 10.0f * b);
    }
    qp_set(P, MB_SPINE3, 0.0f, -6.0f, 0.0f), qp_set(P, MB_HEAD, 0.0f, -14.0f, 0.0f);
}

typedef struct FightClip { CoreClip c; const char* montage; float blend_in, blend_out; } FightClip;
static const FightClip g_fight_clips[] = {
    { { "MM_Fight_Guard", 1.0f, true, qpose_fguard, 0 }, NULL, 0, 0 },
    { { "MM_Fight_Step", 0.7f, true, qpose_fstep, 2 }, NULL, 0, 0 },
    { { "MM_Fight_Jab", 0.36f, false, qpose_jab, 0 }, "AM_Fight_Jab", 0.05f, 0.1f },
    { { "MM_Fight_Cross", 0.44f, false, qpose_cross, 0 }, "AM_Fight_Cross", 0.05f, 0.12f },
    { { "MM_Fight_Hook", 0.5f, false, qpose_hook, 0 }, "AM_Fight_Hook", 0.05f, 0.12f },
    { { "MM_Fight_Uppercut", 0.58f, false, qpose_uppercut, 0 }, "AM_Fight_Uppercut", 0.06f, 0.14f },
    { { "MM_Fight_Kick", 0.66f, false, qpose_kick, 0 }, "AM_Fight_Kick", 0.06f, 0.14f },
    { { "MM_Fight_AirKick", 0.55f, false, qpose_airkick, 0 }, "AM_Fight_AirKick", 0.05f, 0.12f },
    { { "MM_Fight_Block", 1.0f, true, qpose_fblock, 0 }, "AM_Fight_Block", 0.06f, 0.1f },
    { { "MM_Fight_Hit", 0.4f, false, qpose_fhit, 0 }, "AM_Fight_Hit", 0.03f, 0.12f },
    { { "MM_Fight_Dodge", 0.45f, false, qpose_fdodge, 0 }, "AM_Fight_Dodge", 0.05f, 0.12f },
    { { "MM_Fight_Roll", 0.95f, false, qpose_roll, 0 }, "AM_Fight_Roll", 0.05f, 0.15f },
    { { "MM_Fight_KO", 1.4f, false, qpose_fko, 0 }, NULL, 0, 0 },
    { { "MM_Fight_Win", 1.2f, true, qpose_fwin, 0 }, NULL, 0, 0 },
};

// The fighting pack (made once per project; again when an older engine made the clips): its clips, montages and
// ABP_Fighter. Returns the files made.
uint32_t anim_make_fight_pack(void) {
    char dir[800], path[900], vpath[900];
    snprintf(dir, sizeof dir, "%s/Characters/Mannequin/SK_Mannequin.rskm", A.root);
    vpath_of(dir, vpath, sizeof vpath);
    SkelMesh* m = re_file_stat(dir, NULL, NULL, NULL) ? anim_mesh_load(vpath) : NULL;
    if (!m) return 0;
    snprintf(dir, sizeof dir, "%s/Characters/Mannequin/Fighting", A.root);
    re_make_dirs(dir);
    uint32_t made = 0;
    for (size_t i = 0; i < sizeof g_fight_clips / sizeof g_fight_clips[0]; i++) {
        const FightClip* f = &g_fight_clips[i];
        made += engine_clip(m, dir, &f->c, CLIP_MAKE);
        if (!f->montage) continue;
        snprintf(path, sizeof path, "%s/%s.rmontage", dir, f->montage);
        if (re_file_stat(path, NULL, NULL, NULL)) continue;
        AnimMontage mo = { 0 };
        snprintf(mo.path, sizeof mo.path, "%s", path);
        snprintf(mo.anim, ANIM_NAME, "%s", f->c.name);
        snprintf(mo.slot, ANIM_NAME, "DefaultSlot"); // (the whole body: kicks, rolls, the stance)
        mo.blend_in = f->blend_in, mo.blend_out = f->blend_out, mo.rate = 1.0f;
        snprintf(mo.sections[0].name, ANIM_NAME, "Default");
        mo.nsections = 1;
        made += anim_montage_save(&mo);
    }
    // ABP_Fighter: the guard at rest, the fighting step up to 220 cm/s, a jog past it; Jump; the montages over all
    BpAsset* a = NULL;
    AnimGraph* ag = abp_begin("/Content/Characters/Mannequin/Fighting", "ABP_Fighter", "SK_Mannequin", &a);
    if (ag) {
        ag->foot_ik = false; // (the stance's feet are keyed: the IK pulled them together)
        AnimState* lo = &ag->states[ag->nstates++];
        snprintf(lo->name, ANIM_NAME, "Footwork");
        lo->kind = ANIM_STATE_BLEND1D, lo->loop = true, lo->rate = 1.0f, lo->x = 0, lo->y = 0;
        snprintf(lo->param, ANIM_NAME, "Speed");
        lo->nsamples = 3;
        snprintf(lo->samples[0], ANIM_NAME, "MM_Fight_Guard"), lo->sample_at[0] = 0.0f;
        snprintf(lo->samples[1], ANIM_NAME, "MM_Fight_Step"), lo->sample_at[1] = 160.0f;
        snprintf(lo->samples[2], ANIM_NAME, "MM_Jog"), lo->sample_at[2] = 330.0f;
        abp_jump(ag);
        made++;
    }
    if (a) bp_save(a);
    anim_rescan(); // (m: the loaded one, cached: not freed)
    return made;
}
