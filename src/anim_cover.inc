// (2026-11-23, notas-dev/animacao.md "Cobertura") Included by anim.c after anim_combo.inc. Third-person cover:
// Enter Cover finds the nearest wall around a Character (lines at its chest height), takes it there with its back to the
// wall (glided in ~0.3 s) and reads the cover's height (a line over it at 125 cm: a low wall's top under it). In cover,
// its movement goes only along the wall (bp_vm.c move_character asks anim_cover_input) and stops at the wall's ends
// (lines into the wall a step ahead); a low cover crouches it (the pelvis down, the feet kept by IK). Cover Peek leans it
// out at an end, or stands it up over a low one. Exit Cover lets it go.

#define COVERS 64
#define COVER_EDGE 30.0f // (how far past the body the wall must go on: the end is that close)
static struct Cover {
    Entity e;
    float n[2];       // the wall's normal (out of it)
    float point[3];   // where the body stands against it (the glide's target, then kept there)
    float glide;      // 0..1 of the way in
    bool low, peek, at_left, at_right;
    float crouch_w, peek_w, lean_side; // (the pose's weights, eased; lean_side: -1 left, +1 right)
} g_cover[COVERS];
static uint32_t g_ncover;
static struct Cover* cover_of(Entity e) {
    for (uint32_t i = 0; i < g_ncover; i++)
        if (g_cover[i].e == e) return &g_cover[i];
    return NULL;
}
static bool cover_ray(World* w, Entity self, const float a[3], const float b[3], SqHit* h) {
    SqQuery q;
    memset(&q, 0, sizeof q);
    memcpy(q.start, a, 12), memcpy(q.end, b, 12);
    q.shape = SQ_LINE, q.objects = SQ_OBJ_STATIC | SQ_OBJ_DYNAMIC | SQ_OBJ_LANDSCAPE, q.nignore = 1, q.ignore = &self;
    return sq_trace(w, &q, h);
}
// the wall behind a point (into -n) at chest height: whether it goes on there
static bool cover_wall_at(World* w, Entity self, const float p[3], const float n[2], float reach) {
    SqHit h;
    float a[3] = { p[0], p[1], p[2] }, b[3] = { p[0] - n[0] * reach, p[1] - n[1] * reach, p[2] };
    return cover_ray(w, self, a, b, &h) && fabsf(h.impact_normal[2]) < 0.5f;
}

bool anim_cover_enter(World* w, Entity e, float search) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    if (!t || !c) return false;
    if (search <= 1.0f) search = 200.0f;
    float rad = c->radius > 1.0f ? c->radius : 34.0f, hh = c->half_height > 1.0f ? c->half_height : 88.0f;
    float feet = t->location[2] - hh, chest = feet + 70.0f; // (a low wall's height too)
    float best = 1e30f, hit[3] = { 0, 0, 0 }, nrm[2] = { 0, 0 };
    for (int k = 0; k < 16; k++) { // around it
        float a = (float)k * 0.3926991f, s[3] = { t->location[0], t->location[1], chest }, b[3] = { s[0] + cosf(a) * (search + rad), s[1] + sinf(a) * (search + rad), chest };
        SqHit h;
        if (!cover_ray(w, e, s, b, &h) || fabsf(h.impact_normal[2]) > 0.5f || h.distance >= best) continue;
        float l = hypotf(h.impact_normal[0], h.impact_normal[1]);
        if (l < 1e-3f) continue;
        best = h.distance, memcpy(hit, h.impact_point, 12), nrm[0] = h.impact_normal[0] / l, nrm[1] = h.impact_normal[1] / l;
    }
    if (best > 1e29f) return false;
    struct Cover* cv = cover_of(e);
    if (!cv) {
        if (g_ncover >= COVERS) return false;
        cv = &g_cover[g_ncover++];
        memset(cv, 0, sizeof *cv);
        cv->e = e;
    }
    cv->n[0] = nrm[0], cv->n[1] = nrm[1], cv->glide = 0.0f, cv->peek = false;
    cv->point[0] = hit[0] + nrm[0] * (rad + 2.0f), cv->point[1] = hit[1] + nrm[1] * (rad + 2.0f), cv->point[2] = t->location[2];
    // its height: a line toward the wall at 125 cm hits a high one; a low one's top is under it
    float hp[3] = { cv->point[0], cv->point[1], feet + 125.0f }, hq[3] = { hp[0] - nrm[0] * (rad + 30.0f), hp[1] - nrm[1] * (rad + 30.0f), hp[2] };
    SqHit h;
    cv->low = !cover_ray(w, e, hp, hq, &h);
    return true;
}
void anim_cover_exit(World* w, Entity e) {
    (void)w;
    for (uint32_t i = 0; i < g_ncover; i++)
        if (g_cover[i].e == e) g_cover[i] = g_cover[--g_ncover], i--;
}
bool anim_cover_state(World* w, Entity e, bool* low, bool* left, bool* right) {
    (void)w;
    struct Cover* cv = cover_of(e);
    if (low) *low = cv && cv->low;
    if (left) *left = cv && cv->at_left;
    if (right) *right = cv && cv->at_right;
    return cv != NULL;
}
void anim_cover_peek(World* w, Entity e, bool on) {
    (void)w;
    struct Cover* cv = cover_of(e);
    if (cv) cv->peek = on;
}

// The Character's movement in cover (move_character, before it moves): the input along the wall only, none past its
// ends; true when in cover (its facing is then cover's: anim_cover_after)
bool anim_cover_input(World* w, Entity e, CTransform* t, CCharacter* c, float dt) {
    struct Cover* cv = cover_of(e);
    if (!cv) return false;
    (void)dt;
    float tan_[2] = { cv->n[1], -cv->n[0] }; // (along the wall: the right of a body with its back to it, facing n: n x up)
    float along = c->input[0] * tan_[0] + c->input[1] * tan_[1];
    float rad = c->radius > 1.0f ? c->radius : 34.0f, feet = t->location[2] - (c->half_height > 1.0f ? c->half_height : 88.0f);
    // its ends: the wall a step to each side
    for (int s = -1; s <= 1; s += 2) {
        float p[3] = { t->location[0] + tan_[0] * (float)s * (rad * 0.5f + COVER_EDGE * 0.5f), t->location[1] + tan_[1] * (float)s * (rad * 0.5f + COVER_EDGE * 0.5f), feet + 70.0f };
        bool wall = cover_wall_at(w, e, p, cv->n, rad + 40.0f);
        if (s < 0) cv->at_left = !wall;
        else cv->at_right = !wall;
    }
    if ((along > 0.0f && cv->at_right) || (along < 0.0f && cv->at_left)) along = 0.0f;
    float slow = cv->low ? 0.55f : 0.7f; // (pressed to the wall: slower; crouched: slower still)
    if (cv->glide < 1.0f) along = 0.0f; // (getting there first)
    c->input[0] = tan_[0] * along * slow, c->input[1] = tan_[1] * along * slow, c->input[2] = 0.0f;
    return true;
}
// ... after it moved: glided into place, kept against the wall, its back to it
void anim_cover_after(World* w, Entity e, CTransform* t, float dt) {
    (void)w;
    struct Cover* cv = cover_of(e);
    if (!cv) return;
    if (cv->glide < 1.0f) {
        cv->glide = fminf(cv->glide + dt / 0.3f, 1.0f);
        float k = fminf(dt / fmaxf(0.3f * (1.0f - cv->glide) + dt, 1e-4f), 1.0f);
        t->location[0] += (cv->point[0] - t->location[0]) * k, t->location[1] += (cv->point[1] - t->location[1]) * k;
    } else { // (along the wall it may slide; its distance from it held)
        float off = (t->location[0] - cv->point[0]) * cv->n[0] + (t->location[1] - cv->point[1]) * cv->n[1];
        t->location[0] -= cv->n[0] * off, t->location[1] -= cv->n[1] * off;
        cv->point[0] = t->location[0], cv->point[1] = t->location[1];
    }
    float face = atan2f(cv->n[1], cv->n[0]) / DEG2RAD, d = remainderf(face - t->rotation[1], 360.0f), turn = 720.0f * dt;
    t->rotation[1] = remainderf(t->rotation[1] + (fabsf(d) < turn ? d : (d > 0 ? turn : -turn)), 360.0f);
}

// The pose (after the hit's layer): crouched behind a low cover (the pelvis down, the feet where they were), leaning out
// at an end or standing up over a low one while peeking
static void cover_apply(AnimInst* in, const SkelMesh* m, AnimXf* pose, const float rows[3][4]) {
    struct Cover* cv = g_ncover ? cover_of(in->actor ? in->actor : in->e) : NULL;
    float dt = fminf(fmaxf(in->pose_dt, 0.0f), 0.05f);
    if (!cv) return;
    float want_c = cv->low && !cv->peek ? 1.0f : 0.0f, want_p = cv->peek && !cv->low && (cv->at_left || cv->at_right) ? 1.0f : 0.0f;
    cv->crouch_w += (want_c - cv->crouch_w) * fminf(dt * 8.0f, 1.0f);
    cv->peek_w += (want_p - cv->peek_w) * fminf(dt * 8.0f, 1.0f);
    if (cv->at_left != cv->at_right) cv->lean_side = cv->at_right ? 1.0f : -1.0f;
    if (cv->crouch_w < 1e-3f && cv->peek_w < 1e-3f) return;
    if (in->style_mesh != m) style_bind(in, m);
    int16_t pel = in->sb[0];
    float bones[ANIM_MAX_BONES][3][4];
    anim_world(m, pose, rows, bones);
    if (cv->crouch_w > 1e-3f && pel >= 0) { // the pelvis down (the world's down into its parent's frame), the feet kept
        int16_t fl = style_bone(m, "foot", "l", 0), fr = style_bone(m, "foot", "r", 0);
        float keep[2][3] = { { 0 } };
        if (fl >= 0) keep[0][0] = bones[fl][0][3], keep[0][1] = bones[fl][1][3], keep[0][2] = bones[fl][2][3];
        if (fr >= 0) keep[1][0] = bones[fr][0][3], keep[1][1] = bones[fr][1][3], keep[1][2] = bones[fr][2][3];
        int p = m->bones[pel].parent;
        const float(*P)[4] = p >= 0 ? bones[p] : rows;
        float drop = -40.0f * cv->crouch_w;
        for (int ax = 0; ax < 3; ax++) {
            float l2 = P[0][ax] * P[0][ax] + P[1][ax] * P[1][ax] + P[2][ax] * P[2][ax];
            pose[pel].t[ax] += l2 > 1e-12f ? P[2][ax] * drop / l2 : 0.0f;
        }
        for (int s = 0; s < 2; s++) {
            int16_t fb = s ? fr : fl, ca = fb >= 0 ? m->bones[fb].parent : -1, th = ca >= 0 ? m->bones[ca].parent : -1;
            if (th >= 0) two_bone_to(m, pose, rows, bones, th, ca, fb, keep[s], 1.0f);
        }
    }
    if (cv->peek_w > 1e-3f) { // leaning out past the end: about the forward axis (the mesh's space)
        float sc[3], fw[3];
        for (int c = 0; c < 3; c++) sc[c] = sqrtf(rows[0][c] * rows[0][c] + rows[1][c] * rows[1][c] + rows[2][c] * rows[2][c]) + 1e-12f;
        float wf[2] = { cv->n[0], cv->n[1] }; // (it faces away from the wall)
        for (int c = 0; c < 3; c++) fw[c] = (rows[0][c] * wf[0] + rows[1][c] * wf[1]) / sc[c];
        float a = -0.35f * cv->peek_w * cv->lean_side; // (~20 degrees, toward the open end: about forward, + leans left)
        style_turn(m, pose, in->sb[1], fw, a * 0.5f), style_turn(m, pose, in->sb[2], fw, a * 0.5f);
    }
}
static void cover_end_play(void) { g_ncover = 0; }
