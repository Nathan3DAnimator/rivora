// The creature packs (Tools > Animation Packs, included by anim.c before anim_pack.inc): a quadruped, a fish, a bird and
// a reptile, each made by code: its skeleton, its parts (shapes held by bones), its clips (poses as functions of time:
// gaits with the legs' phases of the real animal, the spine's waves) and an Animation Blueprint (Idle - move - fast by
// Speed). Folders: Content/Creatures/<name>.

typedef struct CBone { const char* name; int parent; float t[3]; } CBone;
typedef void (*CPoseFn)(float t, float len, float r[][3], float root[3]);
typedef struct CClip { const char* name; float len; bool loop; CPoseFn fn; } CClip;
typedef struct CPart { int bone, child, shape; float r; float at[3], size[3]; int color; } CPart; // child >= 0: a segment to it

static SkelMesh* creature_mesh(const char* path, const char* vpath, const CBone* bones, int nb, const CPart* parts, int np, const float colors[][3]) {
    SkelMesh* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    name_of(path, m->name);
    m->hash = vpath_hash(vpath);
    for (int b = 0; b < nb; b++) {
        snprintf(m->bones[b].name, ANIM_NAME, "%s", bones[b].name);
        m->bones[b].parent = (int16_t)bones[b].parent;
        xf_identity(&m->bones[b].bind);
        memcpy(m->bones[b].bind.t, bones[b].t, 12);
    }
    m->nbones = (uint32_t)nb;
    for (int i = 0; i < np; i++) {
        const CPart* p = &parts[i];
        if (p->child >= 0) { // a segment from the bone to its child, of radius r
            const float* d = bones[p->child].t;
            float at[3] = { d[0] * 0.5f, d[1] * 0.5f, d[2] * 0.5f }, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            add_part(m, p->bone, p->shape, at, d, (float[3]){ p->r * 2.0f, p->r * 2.0f, len + p->r * 0.5f }, colors[p->color]);
        } else
            add_part(m, p->bone, p->shape, p->at, NULL, p->size, colors[p->color]);
    }
    mesh_prepare(m);
    return m;
}

static AnimSeq* creature_seq(const SkelMesh* m, const char* path, const char* vpath, const CClip* c, int root) {
    uint32_t nf = (uint32_t)lroundf(c->len * 30.0f) + 1, nb = m->nbones;
    AnimSeq* s = seq_alloc(nf, nb);
    if (!s) return NULL;
    snprintf(s->path, sizeof s->path, "%s", path);
    name_of(path, s->name);
    s->hash = vpath_hash(vpath);
    snprintf(s->skeleton, sizeof s->skeleton, "%s", m->name);
    s->fps = 30.0f, s->length = c->len, s->loop = c->loop, s->generator = ANIM_GENERATOR;
    for (uint32_t b = 0; b < nb; b++) snprintf(s->track_bone[b], ANIM_NAME, "%s", m->bones[b].name);
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    for (uint32_t f = 0; f < nf; f++) {
        float rot[ANIM_MAX_BONES][3] = { { 0 } }, rp[3];
        memcpy(rp, m->bones[root].bind.t, 12);
        c->fn(c->loop ? (float)f / (float)(nf - 1) * c->len : (float)f / 30.0f, c->len, rot, rp);
        for (uint32_t b = 0; b < nb; b++) {
            float qx[4], qy[4], qz[4], q[4], tmp[4];
            q_axis(X, rot[b][0] * DEG, qx), q_axis(Y, rot[b][1] * DEG, qy), q_axis(Z, rot[b][2] * DEG, qz);
            q_mul(qz, qy, tmp), q_mul(tmp, qx, q);
            memcpy(&s->rot[((size_t)f * nb + b) * 4], q, 16);
            memcpy(&s->pos[((size_t)f * nb + b) * 3], m->bones[b].bind.t, 12);
        }
        memcpy(&s->pos[((size_t)f * nb + (uint32_t)root) * 3], rp, 12);
    }
    return s;
}

// --- The quadruped (a canine): diagonal pairs at a walk, the front and hind pairs at a gallop, the spine flexing ---

enum { Q_ROOT, Q_HIPS, Q_SPINE, Q_CHEST, Q_NECK, Q_HEAD, Q_JAW, Q_TAIL1, Q_TAIL2, Q_TAIL3,
       Q_FL1, Q_FL2, Q_FL3, Q_FR1, Q_FR2, Q_FR3, Q_BL1, Q_BL2, Q_BL3, Q_BR1, Q_BR2, Q_BR3, Q_COUNT };
static const CBone g_qb[Q_COUNT] = {
    { "root", -1, { 0, 0, 0 } }, { "hips", 0, { -30, 0, 62 } }, { "spine", 1, { 30, 0, 2 } }, { "chest", 2, { 30, 0, 2 } },
    { "neck", 3, { 14, 0, 10 } }, { "head", 4, { 14, 0, 14 } }, { "jaw", 5, { 10, 0, -4 } },
    { "tail_1", 1, { -10, 0, 4 } }, { "tail_2", 7, { -16, 0, 4 } }, { "tail_3", 8, { -16, 0, 2 } },
    { "leg_fl_1", 3, { 4, -10, -6 } }, { "leg_fl_2", 10, { 2, 0, -26 } }, { "leg_fl_3", 11, { -2, 0, -26 } },
    { "leg_fr_1", 3, { 4, 10, -6 } }, { "leg_fr_2", 13, { 2, 0, -26 } }, { "leg_fr_3", 14, { -2, 0, -26 } },
    { "leg_bl_1", 1, { 0, -10, -4 } }, { "leg_bl_2", 16, { 6, 0, -28 } }, { "leg_bl_3", 17, { -6, 0, -26 } },
    { "leg_br_1", 1, { 0, 10, -4 } }, { "leg_br_2", 19, { 6, 0, -28 } }, { "leg_br_3", 20, { -6, 0, -26 } },
};
static void q_leg(float r[][3], int upper, float ph, float swing, float lift, bool hind) {
    // stance (the first 60% of the cycle): the leg sweeps back; swing: forward and up (eased: a smooth arc)
    float u = fmodf(ph / (2.0f * PI) + 1.0f, 1.0f), a;
    if (u < 0.6f) a = swing * (1.0f - 2.0f * (u / 0.6f));
    else {
        float v = (u - 0.6f) / 0.4f;
        a = swing * (-1.0f + 2.0f * v * v * (3.0f - 2.0f * v));
    }
    // swing: the joints fold (the elbow back, the wrist curls the paw under; the hock lifts the hind paw), peaking early
    // and opening before the paw lands; stance: a little give as the weight comes on (loading), then the push
    float knee = u >= 0.6f ? lift * sinf(powf((u - 0.6f) / 0.4f, 0.8f) * PI) : 0.0f;
    float load = u < 0.6f ? 7.0f * sinf(PI * fminf(u / 0.35f, 1.0f)) * (1.0f - smooth01((u - 0.35f) / 0.25f)) : 0.0f;
    r[upper][1] = -a;
    r[upper + 1][1] = hind ? -knee * 0.85f - 6.0f - load : knee * 0.9f + 4.0f + load * 0.5f;
    r[upper + 2][1] = hind ? knee * 1.05f + 6.0f + load : -knee * 1.1f - load * 0.6f;
}
static void q_gait(float t, float len, float r[][3], float p[3], float swing, float lift, float off_f, float off_b, float flex, float bob) {
    float ph = t / len * 2.0f * PI;
    q_leg(r, Q_BL1, ph, swing, lift, true), q_leg(r, Q_BR1, ph + PI + off_b, swing, lift, true);
    q_leg(r, Q_FL1, ph + off_f, swing, lift, false), q_leg(r, Q_FR1, ph + PI + off_f + off_b, swing, lift, false);
    r[Q_SPINE][1] = flex * sinf(ph * 2.0f), r[Q_CHEST][1] = -flex * 0.5f * sinf(ph * 2.0f);
    r[Q_HIPS][2] = 4.0f * sinf(ph), r[Q_CHEST][2] = -4.0f * sinf(ph);
    r[Q_NECK][1] = -6.0f + 3.0f * sinf(ph * 2.0f + off_f - 0.5f), r[Q_HEAD][1] = 4.0f - 2.0f * sinf(ph * 2.0f + off_f - 0.9f); // (the head nods with the forelegs)
    r[Q_NECK][2] = -3.0f * sinf(ph + off_f), r[Q_HEAD][2] = 2.0f * sinf(ph + off_f - 0.5f); // (and keeps looking ahead)
    r[Q_TAIL1][1] = -20.0f, r[Q_TAIL1][2] = 12.0f * sinf(ph), r[Q_TAIL2][2] = 14.0f * sinf(ph - 0.6f), r[Q_TAIL3][2] = 16.0f * sinf(ph - 1.2f);
    p[2] = 62.0f - bob * (0.5f + 0.5f * cosf(ph * 2.0f));
}
static void q_idle(float t, float len, float r[][3], float p[3]) {
    float b = sinf(t / len * 2.0f * PI);
    r[Q_SPINE][1] = 1.0f * b, r[Q_NECK][1] = -10.0f + 2.0f * b, r[Q_HEAD][2] = 8.0f * sinf(t / len * PI * 2.0f * 0.5f);
    r[Q_JAW][1] = 6.0f + 6.0f * fmaxf(b, 0.0f); // (panting)
    r[Q_TAIL1][1] = -25.0f, r[Q_TAIL1][2] = 25.0f * sinf(t / len * 2.0f * PI * 3.0f), r[Q_TAIL2][2] = 20.0f * sinf(t / len * 2.0f * PI * 3.0f - 0.7f);
    r[Q_FL2][1] = r[Q_FR2][1] = 4.0f, r[Q_BL2][1] = r[Q_BR2][1] = -6.0f, r[Q_BL3][1] = r[Q_BR3][1] = 6.0f;
    p[2] = 62.0f - 0.8f * (0.5f + 0.5f * b);
}
static void q_walk(float t, float len, float r[][3], float p[3]) { q_gait(t, len, r, p, 20.0f, 55.0f, PI * 0.5f, 0.0f, 2.0f, 1.4f); }
static void q_run(float t, float len, float r[][3], float p[3]) { // a gallop: the hind pair, then the front pair, spine flexing
    q_gait(t, len, r, p, 34.0f, 70.0f, PI * 0.9f, -PI * 0.85f, 9.0f, 5.0f);
}

// --- The fish: a traveling wave down its body (amplitude growing toward the tail), the fins sculling ---

enum { F_ROOT, F_BODY, F_S1, F_S2, F_S3, F_S4, F_TAIL, F_HEAD, F_FIN_L, F_FIN_R, F_DORSAL, F_COUNT };
static const CBone g_fb[F_COUNT] = {
    { "root", -1, { 0, 0, 0 } }, { "body", 0, { 0, 0, 40 } }, { "spine_1", 1, { -10, 0, 0 } }, { "spine_2", 2, { -10, 0, 0 } },
    { "spine_3", 3, { -10, 0, 0 } }, { "spine_4", 4, { -8, 0, 0 } }, { "tail", 5, { -8, 0, 0 } }, { "head", 1, { 12, 0, 0 } },
    { "fin_l", 1, { 4, -6, -2 } }, { "fin_r", 1, { 4, 6, -2 } }, { "dorsal", 2, { 0, 0, 7 } },
};
static void f_swim(float t, float len, float r[][3], float p[3], float amp) {
    float ph = t / len * 2.0f * PI;
    static const int chain[5] = { F_S1, F_S2, F_S3, F_S4, F_TAIL };
    for (int i = 0; i < 5; i++) r[chain[i]][2] = amp * (0.35f + 0.25f * (float)i) * sinf(ph - 0.75f * (float)(i + 1));
    r[F_HEAD][2] = -amp * 0.2f * sinf(ph);
    r[F_FIN_L][0] = -25.0f + 18.0f * sinf(ph * 2.0f), r[F_FIN_R][0] = 25.0f - 18.0f * sinf(ph * 2.0f);
    r[F_DORSAL][0] = 6.0f * sinf(ph - 1.0f);
    p[1] = amp * 0.08f * sinf(ph), p[2] = 40.0f;
}
static void f_idle(float t, float len, float r[][3], float p[3]) { f_swim(t, len, r, p, 6.0f), p[2] = 40.0f + 1.5f * sinf(t / len * 2.0f * PI); }
static void f_slow(float t, float len, float r[][3], float p[3]) { f_swim(t, len, r, p, 14.0f); }
static void f_fast(float t, float len, float r[][3], float p[3]) { f_swim(t, len, r, p, 24.0f); }

// --- The bird: wings in three segments (the downstroke strong, the upstroke folded), a hop on the ground ---

enum { B_ROOT, B_BODY, B_NECK, B_HEAD, B_TAIL, B_WL1, B_WL2, B_WL3, B_WR1, B_WR2, B_WR3, B_LL1, B_LL2, B_LR1, B_LR2, B_COUNT };
static const CBone g_bb[B_COUNT] = {
    { "root", -1, { 0, 0, 0 } }, { "body", 0, { 0, 0, 24 } }, { "neck", 1, { 9, 0, 5 } }, { "head", 2, { 4, 0, 7 } },
    { "tail", 1, { -10, 0, 1 } }, { "wing_l_1", 1, { 2, -5, 4 } }, { "wing_l_2", 5, { 0, -12, 0 } }, { "wing_l_3", 6, { -2, -14, 0 } },
    { "wing_r_1", 1, { 2, 5, 4 } }, { "wing_r_2", 8, { 0, 12, 0 } }, { "wing_r_3", 9, { -2, 14, 0 } },
    { "leg_l_1", 1, { 0, -3, -6 } }, { "leg_l_2", 11, { 1, 0, -10 } }, { "leg_r_1", 1, { 0, 3, -6 } }, { "leg_r_2", 13, { 1, 0, -10 } },
};
static void b_wings(float r[][3], float ph, float span, float fold) {
    // the stroke above the body more than below it (up 60, down 40 degrees), the outer wing a beat behind (it whips
    // through), the wrist folding the hand in on the upstroke so it cuts back up through the air
    float down = sinf(ph), f = fold * fmaxf(-sinf(ph - 0.3f), 0.0f), sh = 10.0f - span * 0.9f * down;
    r[B_WL1][0] = -sh, r[B_WR1][0] = sh;
    r[B_WL2][0] = span * 0.35f * sinf(ph - 0.7f) + f * 0.6f, r[B_WR2][0] = -span * 0.35f * sinf(ph - 0.7f) - f * 0.6f;
    r[B_WL2][2] = f * 0.5f, r[B_WR2][2] = -f * 0.5f;
    r[B_WL3][2] = f * 1.1f, r[B_WR3][2] = -f * 1.1f;
}
static void b_fly(float t, float len, float r[][3], float p[3]) {
    float ph = t / len * 2.0f * PI;
    b_wings(r, ph, 55.0f, 40.0f);
    r[B_LL1][1] = r[B_LR1][1] = 60.0f, r[B_LL2][1] = r[B_LR2][1] = -70.0f; // (legs tucked back)
    r[B_TAIL][1] = 6.0f * sinf(ph + 1.0f), r[B_HEAD][1] = -3.0f * sinf(ph);
    p[2] = 24.0f + 2.0f * sinf(ph + PI);
}
static void b_glide(float t, float len, float r[][3], float p[3]) {
    float b = sinf(t / len * 2.0f * PI);
    r[B_WL1][0] = -8.0f - 3.0f * b, r[B_WR1][0] = 8.0f + 3.0f * b;
    r[B_LL1][1] = r[B_LR1][1] = 60.0f, r[B_LL2][1] = r[B_LR2][1] = -70.0f;
    r[B_BODY][0] = 6.0f * b, p[2] = 24.0f;
}
static void b_idle(float t, float len, float r[][3], float p[3]) { // standing: wings folded, the head looking around in jerks
    float u = fmodf(t / len * 4.0f, 1.0f), step = floorf(t / len * 4.0f);
    static const float looks[4] = { 0.0f, 35.0f, -20.0f, 10.0f };
    float a = looks[(int)step & 3], prev = looks[((int)step + 3) & 3], k = fminf(u * 6.0f, 1.0f);
    r[B_HEAD][2] = prev + (a - prev) * k;
    r[B_WL1][0] = 70.0f, r[B_WR1][0] = -70.0f, r[B_WL2][0] = -120.0f, r[B_WR2][0] = 120.0f; // (folded along the body)
    r[B_TAIL][1] = -4.0f;
    p[2] = 24.0f;
}
static void b_hop(float t, float len, float r[][3], float p[3]) { // two-footed hops
    float u = fmodf(t / len, 1.0f), air = sinf(u * PI);
    b_idle(t, len, r, p);
    r[B_LL1][1] = r[B_LR1][1] = -30.0f * air, r[B_LL2][1] = r[B_LR2][1] = 40.0f * air;
    r[B_WL1][0] = 70.0f - 25.0f * air, r[B_WR1][0] = -70.0f + 25.0f * air;
    p[2] = 24.0f + 8.0f * air;
}

// --- The reptile (a lizard): sprawled legs to the side, the spine's S-wave carrying them, diagonal pairs ---

enum { R_ROOT, R_HIPS, R_S1, R_S2, R_CHEST, R_NECK, R_HEAD, R_T1, R_T2, R_T3, R_T4,
       R_FL1, R_FL2, R_FR1, R_FR2, R_BL1, R_BL2, R_BR1, R_BR2, R_COUNT };
static const CBone g_rb[R_COUNT] = {
    { "root", -1, { 0, 0, 0 } }, { "hips", 0, { -15, 0, 12 } }, { "spine_1", 1, { 10, 0, 0 } }, { "spine_2", 2, { 10, 0, 0 } },
    { "chest", 3, { 10, 0, 0 } }, { "neck", 4, { 8, 0, 1 } }, { "head", 5, { 6, 0, 1 } },
    { "tail_1", 1, { -10, 0, 0 } }, { "tail_2", 7, { -12, 0, -1 } }, { "tail_3", 8, { -12, 0, -1 } }, { "tail_4", 9, { -10, 0, -1 } },
    { "leg_fl_1", 4, { 0, -5, -1 } }, { "leg_fl_2", 11, { 0, -9, 0 } }, { "leg_fr_1", 4, { 0, 5, -1 } }, { "leg_fr_2", 13, { 0, 9, 0 } },
    { "leg_bl_1", 1, { 0, -5, -1 } }, { "leg_bl_2", 15, { 0, -10, 0 } }, { "leg_br_1", 1, { 0, 5, -1 } }, { "leg_br_2", 17, { 0, 10, 0 } },
};
static void r_leg(float r[][3], int upper, float ph, float swing, float side) {
    float u = fmodf(ph / (2.0f * PI) + 1.0f, 1.0f), a = u < 0.55f ? swing * (1.0f - 2.0f * u / 0.55f) : swing * (-1.0f + 2.0f * (u - 0.55f) / 0.45f);
    float lift = u >= 0.55f ? 30.0f * sinf((u - 0.55f) / 0.45f * PI) : 0.0f;
    r[upper][2] = side * a;                  // (forward and back about the vertical: sprawled)
    r[upper][0] = side * (8.0f - lift);      // (up when it swings)
    r[upper + 1][0] = -side * (55.0f - lift * 0.5f); // (the lower leg down to the ground)
}
static void r_gait(float t, float len, float r[][3], float p[3], float swing, float wave) {
    float ph = t / len * 2.0f * PI;
    r_leg(r, R_FL1, ph, swing, -1.0f), r_leg(r, R_BR1, ph, swing, 1.0f);       // (the diagonal pairs)
    r_leg(r, R_FR1, ph + PI, swing, 1.0f), r_leg(r, R_BL1, ph + PI, swing, -1.0f);
    static const int chain[4] = { R_S1, R_S2, R_CHEST, R_NECK };
    for (int i = 0; i < 4; i++) r[chain[i]][2] = wave * sinf(ph - 0.6f * (float)i) * (i == 3 ? -0.6f : 1.0f);
    static const int tail[4] = { R_T1, R_T2, R_T3, R_T4 };
    for (int i = 0; i < 4; i++) r[tail[i]][2] = wave * 1.2f * sinf(ph + 0.8f + 0.7f * (float)i);
    r[R_HEAD][2] = -wave * 0.4f * sinf(ph);
    p[2] = 12.0f;
}
static void r_idle(float t, float len, float r[][3], float p[3]) {
    r_gait(0.0f, len, r, p, 0.0f, 0.0f);
    float b = sinf(t / len * 2.0f * PI);
    r[R_NECK][1] = -8.0f + 4.0f * fmaxf(b, 0.0f) * fmaxf(b, 0.0f), r[R_T3][2] = 6.0f * sinf(t / len * PI);
    p[2] = 12.0f + 0.3f * b; // (breathing)
}
static void r_walk(float t, float len, float r[][3], float p[3]) { r_gait(t, len, r, p, 28.0f, 10.0f); }
static void r_run(float t, float len, float r[][3], float p[3]) { r_gait(t, len, r, p, 40.0f, 18.0f); }

// --- Making a pack: the mesh, the clips, an Animation Blueprint (Speed from the pawn; Locomotion by it) ---

static void creature_abp(const char* folder, const char* name, const char* skeleton, const char* const clip[3], const float speed[3]) {
    if (bp_find_name(BPA_CLASS, name)) return;
    BpAsset* a = bp_create(BPA_CLASS, folder, name, "Anim Instance");
    if (!a) return;
    while (a->graphs[0].nnodes) bp_remove_node(&a->graphs[0], a->graphs[0].nodes[0].id);
    BpGraph* g = &a->graphs[0];
    BpVar* sp = &a->vars[a->nvars++];
    memset(sp, 0, sizeof *sp);
    snprintf(sp->name, sizeof sp->name, "Speed"), sp->type = BPT_FLOAT;
    BpNode* ev = bp_add_node(a, g, BPK_EVENT_UPDATE_ANIM, -700, 0, NULL, NULL);
    BpNode* own = bp_add_node(a, g, BPK_TRY_GET_PAWN_OWNER, -700, 160, NULL, NULL);
    BpNode* vel = bp_add_node(a, g, BPK_GET_ACTOR_VELOCITY, -420, 160, NULL, NULL);
    BpNode* len = bp_add_node(a, g, BPK_VLEN, -180, 160, NULL, NULL);
    BpNode* set = bp_add_node(a, g, BPK_VAR_SET, 120, 0, "Speed", NULL);
    bp_link(g, own->id, 0, vel->id, 0);
    bp_link(g, vel->id, 1, len->id, 0);
    bp_link(g, ev->id, 0, set->id, 0);
    bp_link(g, len->id, 1, set->id, 1);
    AnimGraph* ag = anim_graph_of(a, true);
    if (ag) {
        memset(ag, 0, sizeof *ag);
        snprintf(ag->skeleton, ANIM_NAME, "%s", skeleton);
        AnimState* lo = &ag->states[ag->nstates++];
        snprintf(lo->name, ANIM_NAME, "Locomotion");
        lo->kind = ANIM_STATE_BLEND1D, lo->loop = true, lo->rate = 1.0f;
        snprintf(lo->param, ANIM_NAME, "Speed");
        lo->nsamples = 3;
        for (int k = 0; k < 3; k++) snprintf(lo->samples[k], ANIM_NAME, "%s", clip[k]), lo->sample_at[k] = speed[k];
    }
    bp_save(a);
}

static uint32_t anim_make_creatures(int pack) {
    static const float q_col[3][3] = { { 0.45f, 0.32f, 0.2f }, { 0.12f, 0.1f, 0.08f }, { 0.85f, 0.75f, 0.6f } };
    static const float f_col[3][3] = { { 0.2f, 0.45f, 0.65f }, { 0.9f, 0.55f, 0.2f }, { 0.9f, 0.9f, 0.85f } };
    static const float b_col[3][3] = { { 0.25f, 0.3f, 0.45f }, { 0.9f, 0.6f, 0.15f }, { 0.85f, 0.85f, 0.85f } };
    static const float r_col[3][3] = { { 0.3f, 0.45f, 0.2f }, { 0.15f, 0.2f, 0.1f }, { 0.75f, 0.7f, 0.4f } };
    static const CPart q_parts[] = {
        { Q_HIPS, -1, SHAPE_SPHERE, 0, { -4, 0, 0 }, { 30, 26, 26 }, 0 }, { Q_HIPS, Q_SPINE, SHAPE_CYLINDER, 11, { 0 }, { 0 }, 0 },
        { Q_SPINE, Q_CHEST, SHAPE_CYLINDER, 12, { 0 }, { 0 }, 0 },
        { Q_CHEST, -1, SHAPE_SPHERE, 0, { 4, 0, 0 }, { 34, 30, 32 }, 0 }, { Q_NECK, Q_HEAD, SHAPE_CYLINDER, 7, { 0 }, { 0 }, 0 },
        { Q_HEAD, -1, SHAPE_SPHERE, 0, { 2, 0, 2 }, { 18, 16, 15 }, 0 }, { Q_HEAD, -1, SHAPE_CUBE, 0, { 12, 0, -2 }, { 14, 9, 8 }, 2 },
        { Q_JAW, -1, SHAPE_CUBE, 0, { 4, 0, -1 }, { 12, 7, 3 }, 1 }, { Q_HEAD, -1, SHAPE_CONE, 0, { -2, -6, 10 }, { 6, 4, 9 }, 1 },
        { Q_HEAD, -1, SHAPE_CONE, 0, { -2, 6, 10 }, { 6, 4, 9 }, 1 }, { Q_TAIL1, Q_TAIL2, SHAPE_CYLINDER, 3.5f, { 0 }, { 0 }, 0 },
        { Q_TAIL2, Q_TAIL3, SHAPE_CYLINDER, 3.0f, { 0 }, { 0 }, 0 }, { Q_TAIL3, -1, SHAPE_SPHERE, 0, { -6, 0, 1 }, { 12, 6, 6 }, 2 },
        { Q_FL1, Q_FL2, SHAPE_CYLINDER, 4.5f, { 0 }, { 0 }, 0 }, { Q_FL2, Q_FL3, SHAPE_CYLINDER, 3.2f, { 0 }, { 0 }, 0 }, { Q_FL3, -1, SHAPE_CUBE, 0, { 3, 0, -1 }, { 9, 6, 4 }, 1 },
        { Q_FR1, Q_FR2, SHAPE_CYLINDER, 4.5f, { 0 }, { 0 }, 0 }, { Q_FR2, Q_FR3, SHAPE_CYLINDER, 3.2f, { 0 }, { 0 }, 0 }, { Q_FR3, -1, SHAPE_CUBE, 0, { 3, 0, -1 }, { 9, 6, 4 }, 1 },
        { Q_BL1, Q_BL2, SHAPE_CYLINDER, 5.5f, { 0 }, { 0 }, 0 }, { Q_BL2, Q_BL3, SHAPE_CYLINDER, 3.4f, { 0 }, { 0 }, 0 }, { Q_BL3, -1, SHAPE_CUBE, 0, { 3, 0, -1 }, { 9, 6, 4 }, 1 },
        { Q_BR1, Q_BR2, SHAPE_CYLINDER, 5.5f, { 0 }, { 0 }, 0 }, { Q_BR2, Q_BR3, SHAPE_CYLINDER, 3.4f, { 0 }, { 0 }, 0 }, { Q_BR3, -1, SHAPE_CUBE, 0, { 3, 0, -1 }, { 9, 6, 4 }, 1 },
    };
    static const CPart f_parts[] = {
        { F_BODY, -1, SHAPE_SPHERE, 0, { 0, 0, 0 }, { 26, 9, 18 }, 0 }, { F_HEAD, -1, SHAPE_SPHERE, 0, { 2, 0, 0 }, { 14, 8, 13 }, 0 },
        { F_S1, -1, SHAPE_SPHERE, 0, { -4, 0, 0 }, { 14, 8, 15 }, 0 }, { F_S2, -1, SHAPE_SPHERE, 0, { -4, 0, 0 }, { 12, 7, 12 }, 0 },
        { F_S3, -1, SHAPE_SPHERE, 0, { -4, 0, 0 }, { 10, 5, 9 }, 0 }, { F_S4, -1, SHAPE_SPHERE, 0, { -3, 0, 0 }, { 8, 4, 6 }, 0 },
        { F_TAIL, -1, SHAPE_CUBE, 0, { -5, 0, 0 }, { 8, 1.5f, 18 }, 1 }, { F_DORSAL, -1, SHAPE_CUBE, 0, { -2, 0, 3 }, { 12, 1.2f, 7 }, 1 },
        { F_FIN_L, -1, SHAPE_CUBE, 0, { -2, -2, 0 }, { 7, 4, 1 }, 1 }, { F_FIN_R, -1, SHAPE_CUBE, 0, { -2, 2, 0 }, { 7, 4, 1 }, 1 },
        { F_HEAD, -1, SHAPE_SPHERE, 0, { 4, -3.5f, 2 }, { 2.5f, 2.5f, 2.5f }, 2 }, { F_HEAD, -1, SHAPE_SPHERE, 0, { 4, 3.5f, 2 }, { 2.5f, 2.5f, 2.5f }, 2 },
    };
    static const CPart b_parts[] = {
        { B_BODY, -1, SHAPE_SPHERE, 0, { 0, 0, 0 }, { 22, 13, 13 }, 0 }, { B_NECK, B_HEAD, SHAPE_CYLINDER, 3, { 0 }, { 0 }, 0 },
        { B_HEAD, -1, SHAPE_SPHERE, 0, { 1, 0, 1 }, { 8, 7, 7 }, 0 }, { B_HEAD, -1, SHAPE_CONE, 0, { 6, 0, 0 }, { 3, 3, 6 }, 1 },
        { B_TAIL, -1, SHAPE_CUBE, 0, { -5, 0, 0 }, { 12, 9, 1.5f }, 0 },
        { B_WL1, B_WL2, SHAPE_CUBE, 2.5f, { 0 }, { 0 }, 0 }, { B_WL2, B_WL3, SHAPE_CUBE, 2.2f, { 0 }, { 0 }, 0 }, { B_WL3, -1, SHAPE_CUBE, 0, { -2, -6, 0 }, { 10, 14, 1 }, 2 },
        { B_WR1, B_WR2, SHAPE_CUBE, 2.5f, { 0 }, { 0 }, 0 }, { B_WR2, B_WR3, SHAPE_CUBE, 2.2f, { 0 }, { 0 }, 0 }, { B_WR3, -1, SHAPE_CUBE, 0, { -2, 6, 0 }, { 10, 14, 1 }, 2 },
        { B_LL1, B_LL2, SHAPE_CYLINDER, 1.0f, { 0 }, { 0 }, 1 }, { B_LR1, B_LR2, SHAPE_CYLINDER, 1.0f, { 0 }, { 0 }, 1 },
        { B_LL2, -1, SHAPE_CUBE, 0, { 2, 0, -1 }, { 5, 3, 1 }, 1 }, { B_LR2, -1, SHAPE_CUBE, 0, { 2, 0, -1 }, { 5, 3, 1 }, 1 },
    };
    static const CPart r_parts[] = {
        { R_HIPS, -1, SHAPE_SPHERE, 0, { 0, 0, 0 }, { 14, 11, 7 }, 0 }, { R_S1, R_S2, SHAPE_CYLINDER, 5, { 0 }, { 0 }, 0 },
        { R_S2, R_CHEST, SHAPE_CYLINDER, 5.5f, { 0 }, { 0 }, 0 }, { R_CHEST, -1, SHAPE_SPHERE, 0, { 0, 0, 0 }, { 14, 12, 7 }, 0 },
        { R_NECK, R_HEAD, SHAPE_CYLINDER, 3.5f, { 0 }, { 0 }, 0 }, { R_HEAD, -1, SHAPE_SPHERE, 0, { 4, 0, 0 }, { 12, 8, 5 }, 0 },
        { R_T1, R_T2, SHAPE_CYLINDER, 3.5f, { 0 }, { 0 }, 0 }, { R_T2, R_T3, SHAPE_CYLINDER, 2.6f, { 0 }, { 0 }, 0 },
        { R_T3, R_T4, SHAPE_CYLINDER, 1.8f, { 0 }, { 0 }, 0 }, { R_T4, -1, SHAPE_CONE, 0, { -5, 0, 0 }, { 2, 2, 10 }, 0 },
        { R_FL1, R_FL2, SHAPE_CYLINDER, 1.8f, { 0 }, { 0 }, 1 }, { R_FR1, R_FR2, SHAPE_CYLINDER, 1.8f, { 0 }, { 0 }, 1 },
        { R_BL1, R_BL2, SHAPE_CYLINDER, 2.0f, { 0 }, { 0 }, 1 }, { R_BR1, R_BR2, SHAPE_CYLINDER, 2.0f, { 0 }, { 0 }, 1 },
        { R_HEAD, -1, SHAPE_CUBE, 0, { 3, 0, 2.5f }, { 8, 6, 1 }, 2 },
    };
    static const CClip q_clips[3] = { { "Dog_Idle", 2.0f, true, q_idle }, { "Dog_Walk", 0.9f, true, q_walk }, { "Dog_Run", 0.5f, true, q_run } };
    static const CClip f_clips[3] = { { "Fish_Idle", 1.6f, true, f_idle }, { "Fish_Swim", 1.0f, true, f_slow }, { "Fish_SwimFast", 0.5f, true, f_fast } };
    static const CClip b_clips[4] = { { "Bird_Idle", 2.4f, true, b_idle }, { "Bird_Hop", 0.4f, true, b_hop }, { "Bird_Fly", 0.36f, true, b_fly },
                                      { "Bird_Glide", 1.5f, true, b_glide } };
    static const CClip r_clips[3] = { { "Lizard_Idle", 2.5f, true, r_idle }, { "Lizard_Walk", 0.8f, true, r_walk }, { "Lizard_Run", 0.4f, true, r_run } };
    const char* name;
    const CBone* bones;
    int nb, np, nc, root;
    const CPart* parts;
    const float(*cols)[3];
    const CClip* clips;
    float speeds[3];
    switch (pack) {
    case ANIM_PACK_QUADRUPED: name = "Dog", bones = g_qb, nb = Q_COUNT, parts = q_parts, np = (int)(sizeof q_parts / sizeof q_parts[0]), cols = q_col, clips = q_clips, nc = 3, root = Q_HIPS, speeds[0] = 0, speeds[1] = 130, speeds[2] = 600; break;
    case ANIM_PACK_FISH: name = "Fish", bones = g_fb, nb = F_COUNT, parts = f_parts, np = (int)(sizeof f_parts / sizeof f_parts[0]), cols = f_col, clips = f_clips, nc = 3, root = F_BODY, speeds[0] = 0, speeds[1] = 80, speeds[2] = 300; break;
    case ANIM_PACK_BIRD: name = "Bird", bones = g_bb, nb = B_COUNT, parts = b_parts, np = (int)(sizeof b_parts / sizeof b_parts[0]), cols = b_col, clips = b_clips, nc = 4, root = B_BODY, speeds[0] = 0, speeds[1] = 60, speeds[2] = 500; break;
    case ANIM_PACK_REPTILE: name = "Lizard", bones = g_rb, nb = R_COUNT, parts = r_parts, np = (int)(sizeof r_parts / sizeof r_parts[0]), cols = r_col, clips = r_clips, nc = 3, root = R_HIPS, speeds[0] = 0, speeds[1] = 70, speeds[2] = 280; break;
    default: return 0;
    }
    char dir[700], path[800], vpath[800], folder[300];
    snprintf(dir, sizeof dir, "%s/Creatures/%s", A.root, name);
    snprintf(folder, sizeof folder, "/Content/Creatures/%s", name);
    if (!re_make_dirs(dir)) return 0;
    snprintf(path, sizeof path, "%s/SK_%s.rskm", dir, name);
    vpath_of(path, vpath, sizeof vpath);
    SkelMesh* m = creature_mesh(path, vpath, bones, nb, parts, np, cols);
    if (!m) return 0;
    uint32_t made = anim_mesh_save(m);
    for (int i = 0; i < nc; i++) {
        snprintf(path, sizeof path, "%s/%s.ranim", dir, clips[i].name);
        vpath_of(path, vpath, sizeof vpath);
        AnimSeq* s = creature_seq(m, path, vpath, &clips[i], root);
        if (!s) continue;
        made += anim_seq_save(s);
        free(s->rot), free(s->pos), free(s);
    }
    char skn[64];
    snprintf(skn, sizeof skn, "SK_%s", name);
    free(m);
    anim_rescan();
    char abpn[64];
    snprintf(abpn, sizeof abpn, "ABP_%s", name);
    // (the bird's Locomotion: idle, hop on the ground, fly when fast)
    const char* c3[3] = { clips[0].name, clips[1].name, clips[2].name };
    creature_abp(folder, abpn, skn, c3, speeds);
    return made;
}
