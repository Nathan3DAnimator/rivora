// The engine's own characters and the optional animation packs (notas-dev/animacao.md), included by anim.c: made by
// code (the Mannequin's skeleton, shapes for its parts, poses as functions of time), so they are the engine's, not
// anyone else's.
//   * SK_Mannequin_Male / SK_Mannequin_Female: the Mannequin's skeleton (every MM_ animation plays on them) with their
//     own build: shoulders, chest, waist, hips, limbs, the head (and the female's hair)
//   * the Locomotion pack (Tools > Animation Packs): walking backward, strafing, crouching, sprinting, a jump in three
//     parts, a roll, climbing a ladder, a vault; added to ABP_Mannequin_MM's Motion Matching database

typedef struct MBody {
    float chest[3], pelvis[3], spine_r[2], limb, neck, head[3];
    bool hair;
    float body[3], dark[3], accent[3];
} MBody;

static SkelMesh* make_mannequin_body(const char* path, const char* vpath, const MBody* B) {
    SkelMesh* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    name_of(path, m->name);
    m->hash = vpath_hash(vpath);
    for (int b = 0; b < MB_COUNT; b++) {
        snprintf(m->bones[b].name, ANIM_NAME, "%s", g_mbones[b].name);
        m->bones[b].parent = (int16_t)g_mbones[b].parent;
        xf_identity(&m->bones[b].bind);
        memcpy(m->bones[b].bind.t, g_mbones[b].t, 12);
    }
    m->nbones = MB_COUNT;
    float k = B->limb;
    add_part(m, MB_PELVIS, SHAPE_CUBE, (float[3]){ 0, 0, 2 }, NULL, B->pelvis, B->dark);
    limb(m, MB_SPINE1, MB_SPINE2, B->spine_r[0], B->body, NULL);
    limb(m, MB_SPINE2, MB_SPINE3, B->spine_r[1], B->body, NULL);
    add_part(m, MB_SPINE3, SHAPE_SPHERE, (float[3]){ 0, 0, 9 }, NULL, B->chest, B->body); // (a rounded chest)
    add_part(m, MB_SPINE3, SHAPE_CUBE, (float[3]){ B->chest[0] * 0.35f, 0, 6 }, NULL, (float[3]){ 6, B->chest[1] * 0.55f, 10 }, B->accent);
    limb(m, MB_NECK, MB_HEAD, B->neck, B->dark, NULL);
    add_part(m, MB_HEAD, SHAPE_SPHERE, (float[3]){ 1, 0, 11 }, NULL, B->head, B->body);
    add_part(m, MB_HEAD, SHAPE_CUBE, (float[3]){ B->head[0] * 0.42f, 0, 13 }, NULL, (float[3]){ 4, B->head[1] * 0.66f, 4 }, B->accent);
    if (B->hair) { // a tied-back tail of hair
        add_part(m, MB_HEAD, SHAPE_SPHERE, (float[3]){ -9, 0, 15 }, NULL, (float[3]){ 9, 8, 9 }, B->dark);
        add_part(m, MB_HEAD, SHAPE_SPHERE, (float[3]){ -14, 0, 5 }, NULL, (float[3]){ 7, 6, 16 }, B->dark);
    }
    for (int side = 0; side < 2; side++) {
        int ua = side ? MB_UARM_R : MB_UARM_L, la = side ? MB_LARM_R : MB_LARM_L, ha = side ? MB_HAND_R : MB_HAND_L;
        int th = side ? MB_THIGH_R : MB_THIGH_L, ca = side ? MB_CALF_R : MB_CALF_L, fo = side ? MB_FOOT_R : MB_FOOT_L;
        limb(m, ua, la, 5.2f * k, B->body, B->dark);
        limb(m, la, ha, 4.4f * k, B->body, B->dark);
        add_part(m, ha, SHAPE_CUBE, (float[3]){ 1, 0, -8 }, NULL, (float[3]){ 9 * k, 5, 16 }, B->dark);
        limb(m, th, ca, 7.8f * k, B->body, B->dark);
        limb(m, ca, fo, 6.0f * k, B->body, B->dark);
        add_part(m, fo, SHAPE_CUBE, (float[3]){ 5, 0, -4 }, NULL, (float[3]){ 26, 11 * k, 9 }, B->dark);
    }
    mesh_prepare(m);
    return m;
}

static const MBody g_male = { { 24, 46, 24 }, { 21, 30, 18 }, { 14, 16.5f }, 1.1f, 6.5f, { 22, 19, 26 }, false,
                              { 0.55f, 0.6f, 0.66f }, { 0.12f, 0.14f, 0.18f }, { 0.15f, 0.55f, 0.95f } };
static const MBody g_female = { { 20, 34, 21 }, { 23, 37, 19 }, { 10.5f, 13.5f }, 0.88f, 4.6f, { 20, 17.5f, 24 }, true,
                                { 0.66f, 0.6f, 0.6f }, { 0.2f, 0.12f, 0.14f }, { 0.95f, 0.35f, 0.45f } };

// (the Locomotion pack's poses and its clips' table: anim_motion.inc)

// The engine's two characters (when missing): next to the Mannequin, its animations play on them
static void make_bodies(const char* dir) {
    static const struct { const char* file; const MBody* b; } bodies[2] = { { "SK_Mannequin_Male", &g_male }, { "SK_Mannequin_Female", &g_female } };
    for (int i = 0; i < 2; i++) {
        char path[800], vpath[800];
        snprintf(path, sizeof path, "%s/%s.rskm", dir, bodies[i].file);
        if (re_file_stat(path, NULL, NULL, NULL)) continue;
        vpath_of(path, vpath, sizeof vpath);
        SkelMesh* m = make_mannequin_body(path, vpath, bodies[i].b);
        if (m) anim_mesh_save(m), free(m);
    }
}

static uint32_t anim_make_mm_examples(void); // (anim_mm_examples.inc)

uint32_t anim_make_pack(int pack) {
    if (pack == ANIM_PACK_MM_EXAMPLES) return anim_make_mm_examples();
    if (pack != ANIM_PACK_LOCOMOTION) return anim_make_creatures(pack);
    char dir[700], path[800], vpath[800];
    snprintf(dir, sizeof dir, "%s/Characters/Mannequin/Locomotion", A.root);
    if (!re_make_dirs(dir)) return 0;
    SkelMesh* m = anim_mesh_name("SK_Mannequin");
    if (!m) return 0;
    uint32_t made = 0;
    for (size_t i = 0; i < sizeof g_locomotion_clips / sizeof g_locomotion_clips[0]; i++) made += engine_clip(m, dir, &g_locomotion_clips[i], CLIP_ALWAYS);
    anim_rescan();
    // the moving ones into the Motion Matching Animation Blueprint's database (the strafes and the walk back: by direction)
    BpAsset* abp = (BpAsset*)bp_find_name(BPA_CLASS, "ABP_Mannequin_MM");
    AnimGraph* g = abp ? anim_graph_of(abp, false) : NULL;
    for (uint32_t si = 0; g && si < g->nstates; si++) {
        AnimState* st = &g->states[si];
        if (st->kind != ANIM_STATE_MOTION_MATCH) continue;
        for (size_t i = 0; i < sizeof g_locomotion_clips / sizeof g_locomotion_clips[0] && st->nsamples < ANIM_BLEND_SAMPLES; i++) {
            const CoreClip* c = &g_locomotion_clips[i];
            if (c->steps != 2 || !strcmp(c->name, "MM_CrouchWalk")) continue; // (the moving ones, standing up)
            bool have = false;
            for (uint32_t k = 0; k < st->nsamples && !have; k++) have = !strcmp(st->samples[k], c->name);
            if (!have) snprintf(st->samples[st->nsamples++], ANIM_NAME, "%s", c->name);
        }
        g->rev++;
        bp_save(abp);
    }
    return made;
}
