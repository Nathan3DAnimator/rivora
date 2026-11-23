// (2026-11-23, notas-dev/animacao.md "Root motion, Motion Warping e montages pareados") Included by anim.c after
// anim_mm.inc (its mm_rel_yaw, mm_wrap_pi, mm_smooth).
//   - Root motion of a montage (AnimMontage.root_motion): the clip's root path (its root bone's, else its pelvis's,
//     smoothed: the Motion Matching's rules) and turn, 30 a second, relative to its first frame (rm_track). Each frame
//     the montage's step moves the actor by it (rm_step): a Character through its movement, with collision
//     (anim_root_motion_take in bp_vm.c's move_character), anything else in place; the pose has that path taken out
//     (rm_inplace: the root back where it began), so the mesh doesn't walk off its actor.
//   - Motion Warping (Unreal's): a montage's windows (AnimMontage.warps: a Warp Target's name, a start and an end, warp
//     the place / the turn / the height) bend the root motion inside them so the actor ends the window at that target
//     (Add or Update Warp Target: by actor and name). The correction is spread by the clip's own travel (where it
//     walks fast it corrects fast), by the time where it stands still; the turn by the time.
//   - Paired montages (Unreal's Contextual Animation, two roles): a montage names its partner's montage and where the
//     partner stands from it (AnimMontage.partner*). Play Paired Montage starts both on the same frame; each frame the
//     partner's time is the primary's and its place is the primary's (as moved by its root motion) times the offset
//     times its own clip's root motion: they stay together however the primary moved. It slides into place over Align
//     Time (from where it was). The primary's end (or its stop) ends the pair.

static int16_t bone_named(const SkelMesh* m, const char* name); // (anim.c, below)
#ifndef DEG2RAD
#define DEG2RAD 0.017453292519943295f
#endif

typedef struct RmTrack {
    const AnimSeq* seq;
    const SkelMesh* m;
    uint32_t nf;   // frames at RM_HZ
    int mode;      // 0 none, 1 the root's, 2 the pelvis's
    bool vert;     // its height moves the actor (the root's rises over 8 cm)
    float* T;      // nf x 4: x, y, z (cm, the clip's first facing), yaw (rad), from its first frame
} RmTrack;
#define RM_HZ 30.0f
#define RM_TRACKS 128
static RmTrack g_rmt[RM_TRACKS];
static uint32_t g_nrmt;

static int16_t rm_pelvis_of(const SkelMesh* m) {
    static const char* const names[] = { "pelvis", "Pelvis", "hips", "Hips", "mixamorig:Hips", "spine_00" };
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        int16_t b = bone_named(m, names[k]);
        if (b >= 0) return b;
    }
    return m->nbones > 1 ? 1 : -1;
}

// A clip's root path for a mesh: made once (on this thread)
static const RmTrack* rm_track(const AnimSeq* s, const SkelMesh* m) {
    if (!s || !m || s->length <= 0.0f) return NULL;
    for (uint32_t i = 0; i < g_nrmt; i++)
        if (g_rmt[i].seq == s && g_rmt[i].m == m) return &g_rmt[i];
    if (g_nrmt >= RM_TRACKS) return NULL;
    uint32_t nf = (uint32_t)lroundf(s->length * RM_HZ) + 1;
    if (nf < 2 || nf > 36000) return NULL;
    RmTrack* tr = &g_rmt[g_nrmt];
    memset(tr, 0, sizeof *tr);
    tr->seq = s, tr->m = m, tr->nf = nf;
    tr->T = calloc((size_t)nf * 4, sizeof(float));
    float* R = malloc(sizeof(float) * 8 * nf); // the root's place, the pelvis's, the yaws (root, pelvis)
    int16_t* map = malloc(sizeof(int16_t) * ANIM_MAX_BONES);
    AnimXf* pose = malloc(sizeof(AnimXf) * ANIM_MAX_BONES);
    float(*bones)[3][4] = malloc(sizeof(float) * 12 * ANIM_MAX_BONES);
    if (!tr->T || !R || !map || !pose || !bones) return free(tr->T), free(R), free(map), free(pose), free(bones), NULL;
    float *P = R + 3 * nf, *Y = R + 6 * nf;
    int16_t pel = rm_pelvis_of(m);
    track_map(s, m, map);
    const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    float first[2][3][4];
    for (uint32_t i = 0; i < nf; i++) {
        sample_mapped(s, m, map, fminf((float)i / RM_HZ, s->length), pose);
        anim_world(m, pose, id, bones);
        if (!i) memcpy(first[0], bones[0], sizeof first[0]), memcpy(first[1], bones[pel >= 0 ? pel : 0], sizeof first[1]);
        for (int k = 0; k < 3; k++) R[i * 3 + k] = bones[0][k][3], P[i * 3 + k] = bones[pel >= 0 ? pel : 0][k][3];
        Y[i * 2] = mm_rel_yaw(bones[0], first[0]), Y[i * 2 + 1] = mm_rel_yaw(bones[pel >= 0 ? pel : 0], first[1]);
    }
    for (uint32_t i = 1; i < nf; i++) // (the turns unwrapped)
        for (int k = 0; k < 2; k++) Y[i * 2 + k] = Y[(i - 1) * 2 + k] + mm_wrap_pi(Y[i * 2 + k] - Y[(i - 1) * 2 + k]);
    float rmax = 0.0f, ymax = 0.0f, zmin = 1e30f, zmax = -1e30f;
    for (uint32_t i = 0; i < nf; i++) {
        rmax = fmaxf(rmax, hypotf(R[i * 3] - R[0], R[i * 3 + 1] - R[1])), ymax = fmaxf(ymax, fabsf(Y[i * 2]));
        zmin = fminf(zmin, R[i * 3 + 2]), zmax = fmaxf(zmax, R[i * 3 + 2]);
    }
    float pnet = hypotf(P[(nf - 1) * 3] - P[0], P[(nf - 1) * 3 + 1] - P[1]);
    if (rmax > 5.0f || ymax > 0.17f || zmax - zmin > 8.0f) { // the root moves (its own path and turn)
        tr->mode = 1, tr->vert = zmax - zmin > 8.0f;
        for (uint32_t i = 0; i < nf; i++)
            tr->T[i * 4] = R[i * 3] - R[0], tr->T[i * 4 + 1] = R[i * 3 + 1] - R[1], tr->T[i * 4 + 2] = tr->vert ? R[i * 3 + 2] - R[2] : 0.0f,
            tr->T[i * 4 + 3] = Y[i * 2];
    } else if (pnet > 30.0f) { // carried by the pelvis: its path and turn, smoothed of the steps' sway
        tr->mode = 2;
        for (uint32_t i = 0; i < nf; i++) tr->T[i * 4] = P[i * 3] - P[0], tr->T[i * 4 + 1] = P[i * 3 + 1] - P[1], tr->T[i * 4 + 3] = Y[i * 2 + 1];
        for (int k = 0; k < 4; k++)
            if (k != 2) mm_smooth(tr->T + k, nf, 4, 7, false);
    }
    free(R), free(map), free(pose), free(bones);
    g_nrmt++;
    return tr;
}

// The path at t (linear between the frames), out x y z yaw
static void rm_at(const RmTrack* tr, float t, float out[4]) {
    float f = fminf(fmaxf(t * RM_HZ, 0.0f), (float)(tr->nf - 1));
    uint32_t i0 = (uint32_t)f, i1 = i0 + 1 < tr->nf ? i0 + 1 : i0;
    float a = f - (float)i0;
    for (int k = 0; k < 4; k++) out[k] = tr->T[i0 * 4 + k] + (tr->T[i1 * 4 + k] - tr->T[i0 * 4 + k]) * a;
}
// Its travel's length between two times (the warp spreads its correction by it)
static float rm_path(const RmTrack* tr, float t0, float t1) {
    if (!tr || t1 <= t0) return 0.0f;
    float len = 0.0f, p[4], q[4];
    rm_at(tr, t0, p);
    for (float t = t0 + 1.0f / RM_HZ;; t += 1.0f / RM_HZ) {
        float tt = fminf(t, t1);
        rm_at(tr, tt, q);
        len += hypotf(q[0] - p[0], q[1] - p[1]);
        memcpy(p, q, sizeof p);
        if (tt >= t1) break;
    }
    return len;
}

// --- Warp targets (by actor and name) ---
#define WARP_TARGETS 128
static struct WarpTarget { Entity e; char name[ANIM_NAME]; float loc[3], yaw; } g_wt[WARP_TARGETS];
static uint32_t g_nwt;
static struct WarpTarget* warp_find(Entity e, const char* name) {
    for (uint32_t i = 0; i < g_nwt; i++)
        if (g_wt[i].e == e && !strcmp(g_wt[i].name, name)) return &g_wt[i];
    return NULL;
}
bool anim_warp_target_set(World* w, Entity actor, const char* name, const float loc[3], float yaw_deg) {
    (void)w;
    if (!actor || !name || !name[0]) return false;
    struct WarpTarget* t = warp_find(actor, name);
    if (!t) {
        for (uint32_t i = 0; i < g_nwt && !t; i++) // (a dead actor's slot: reused)
            if (!ecs_alive(w, g_wt[i].e)) t = &g_wt[i];
        if (!t && g_nwt < WARP_TARGETS) t = &g_wt[g_nwt++];
        if (!t) return false;
    }
    t->e = actor;
    snprintf(t->name, sizeof t->name, "%s", name);
    memcpy(t->loc, loc, sizeof t->loc);
    t->yaw = yaw_deg;
    return true;
}
void anim_warp_target_remove(World* w, Entity actor, const char* name) {
    (void)w;
    for (uint32_t i = 0; i < g_nwt; i++)
        if (g_wt[i].e == actor && (!name || !name[0] || !strcmp(g_wt[i].name, name))) g_wt[i] = g_wt[--g_nwt], i--;
}

// --- The root motion handed to the Characters' movement (bp_vm.c move_character takes it, with collision) ---
#define RM_PENDING 128
static struct RmPending { Entity e; float d[3], yaw; bool vert, end, nocol; } g_rmp[RM_PENDING]; // (end: the clip's travel ended; nocol: through what's in the way)
static uint32_t g_nrmp;
static struct RmPending* rm_pending(Entity e, bool make) {
    for (uint32_t i = 0; i < g_nrmp; i++)
        if (g_rmp[i].e == e) return &g_rmp[i];
    if (!make || g_nrmp >= RM_PENDING) return NULL;
    struct RmPending* p = &g_rmp[g_nrmp++];
    memset(p, 0, sizeof *p);
    p->e = e;
    return p;
}
bool anim_root_motion_take(Entity e, float d[3], float* yaw_deg, bool* vert, bool* end, bool* nocol) {
    for (uint32_t i = 0; i < g_nrmp; i++)
        if (g_rmp[i].e == e) {
            memcpy(d, g_rmp[i].d, sizeof g_rmp[i].d);
            *yaw_deg = g_rmp[i].yaw, *vert = g_rmp[i].vert, *end = g_rmp[i].end, *nocol = g_rmp[i].nocol;
            g_rmp[i] = g_rmp[--g_nrmp];
            return true;
        }
    return false;
}

// --- Paired montages ---
#define PAIRS 32
static struct Pair {
    Entity p, s;               // the primary (the center) and its partner
    const AnimMontage* mp;     // the primary's montage (its end ends the pair)
    const AnimMontage* ms;     // the partner's (NULL: none)
    float align, start_t;      // the slide into place (s), the primary's montage time when it began
    float s0[4];               // the partner's place when it began (x y z yaw deg)
    bool on;
} g_pairs[PAIRS];

static AnimInst* inst_of_actor(World* w, Entity actor);
static bool inst_is_partner(Entity actor) {
    for (int i = 0; i < PAIRS; i++)
        if (g_pairs[i].on && g_pairs[i].s == actor) return true;
    return false;
}

// The world yaw (degrees) and scale of an instance's mesh entity
static float rm_mesh_yaw(World* w, const AnimInst* in, float* scale) {
    const CTransform* t = ecs_get(w, in->e, COMP_TRANSFORM);
    *scale = t ? fabsf(t->scale[0]) : 1.0f;
    if (*scale < 1e-4f) *scale = 1.0f;
    return t ? t->rotation[1] : 0.0f;
}

// A montage's step from t0 to t1: its root motion (and its warp windows' corrections) moved onto the actor
static void rm_step(World* w, AnimInst* in, float t0, float t1, float weight) {
    const AnimMontage* mo = in->montage;
    if (!mo || t1 <= t0) return;
    Entity actor = in->actor ? in->actor : in->e;
    if (inst_is_partner(actor)) return; // (a pair's partner: placed by the pair)
    bool any_warp = false;
    for (uint32_t k = 0; k < mo->nwarps; k++) any_warp |= mo->warps[k].start < t1 && mo->warps[k].end > t0 && warp_find(actor, mo->warps[k].target);
    if (!mo->root_motion && !any_warp) return;
    const RmTrack* tr = mo->root_motion ? in->rm_tr : NULL;
    float a[4] = { 0, 0, 0, 0 }, b[4] = { 0, 0, 0, 0 };
    if (tr) rm_at(tr, t0, a), rm_at(tr, t1, b);
    float sc, myaw = rm_mesh_yaw(w, in, &sc);
    // the clip's frame turned to the world: the mesh's yaw now is the clip's at t0 (its turn so far already applied)
    float base = myaw * DEG2RAD - a[3], cb = cosf(base), sb = sinf(base);
    float dx = (b[0] - a[0]) * sc, dy = (b[1] - a[1]) * sc;
    float d[3] = { (cb * dx - sb * dy) * weight, (sb * dx + cb * dy) * weight, (b[2] - a[2]) * sc * weight };
    float dyaw = (b[3] - a[3]) / DEG2RAD * weight;
    bool vert = tr && tr->vert;
    // the warp windows: what's left of the clip's travel to the window's end against what's left to the target
    CTransform* at = ecs_get(w, actor, COMP_TRANSFORM);
    struct RmPending* pend = rm_pending(actor, false);
    for (uint32_t k = 0; at && k < mo->nwarps; k++) {
        const AnimWarpWindow* wn = &mo->warps[k];
        float w0 = fmaxf(t0, wn->start), w1 = fminf(t1, wn->end);
        struct WarpTarget* tg = warp_find(actor, wn->target);
        if (!tg || w1 <= w0 || wn->end <= wn->start) continue;
        float pos[3], yaw = at->rotation[1];
        for (int j = 0; j < 3; j++) pos[j] = at->location[j] + (pend ? pend->d[j] : 0.0f);
        if (pend) yaw += pend->yaw;
        float timef = (w1 - w0) / fmaxf(wn->end - w0, 1e-4f);
        if (wn->flags & ANIM_WARP_TRANSLATION) {
            float e0[4] = { 0, 0, 0, 0 }, e1[4] = { 0, 0, 0, 0 };
            if (tr) rm_at(tr, w0, e0), rm_at(tr, wn->end, e1);
            float rx = (e1[0] - e0[0]) * sc, ry = (e1[1] - e0[1]) * sc;
            float rem[3] = { cb * rx - sb * ry, sb * rx + cb * ry, (e1[2] - e0[2]) * sc };
            float need[3] = { tg->loc[0] - pos[0], tg->loc[1] - pos[1], tg->loc[2] - pos[2] };
            float total = tr ? rm_path(tr, w0, wn->end) : 0.0f, seg = tr ? rm_path(tr, w0, w1) : 0.0f;
            float f = total > 1.0f ? seg / total : timef; // (by its own travel; standing still: by the time)
            if (w1 >= wn->end) f = 1.0f;
            for (int j = 0; j < 2; j++) d[j] += (need[j] - rem[j]) * f;
            if (!(wn->flags & ANIM_WARP_IGNORE_Z)) d[2] += (need[2] - rem[2]) * f, vert = true;
        }
        if (wn->flags & ANIM_WARP_ROTATION) {
            float ry0[4] = { 0, 0, 0, 0 }, ry1[4] = { 0, 0, 0, 0 };
            if (tr) rm_at(tr, w0, ry0), rm_at(tr, wn->end, ry1);
            float need = remainderf(tg->yaw - yaw - (ry1[3] - ry0[3]) / DEG2RAD, 360.0f);
            dyaw += need * (w1 >= wn->end ? 1.0f : timef);
        }
    }
    // (its clip's end reached, not cut short: the Character stops there, no glide on its last speed that would turn it to
    // its velocity)
    bool end = !in->mcut && in->mclip.seq && t1 >= in->mclip.seq->length - 1e-5f;
    if (fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) + fabsf(dyaw) < 1e-6f && !end) return;
    if (ecs_get(w, actor, COMP_CHARACTER)) { // a Character: its movement takes it (with collision)
        struct RmPending* p = rm_pending(actor, true);
        if (!p) return;
        for (int j = 0; j < 3; j++) p->d[j] += d[j];
        p->yaw += dyaw, p->vert |= vert, p->end |= end, p->nocol |= mo->no_collision;
    } else if (at) { // anything else: moved now
        for (int j = 0; j < 3; j++) at->location[j] += d[j];
        at->rotation[1] = remainderf(at->rotation[1] + dyaw, 360.0f);
        ecs_touch(w, actor);
    }
}

// The pose's root path taken out (on the workers: only reads): the root (or the pelvis) back where the clip began
static void rm_inplace(const AnimInst* in, const SkelMesh* m, AnimXf* pose) {
    const RmTrack* tr = in->rm_tr;
    if (!in->montage || !in->montage->root_motion || !tr || !tr->mode) return;
    float c[4];
    rm_at(tr, in->mtime, c);
    float hq[4] = { 0.0f, 0.0f, sinf(-c[3] * 0.5f), cosf(-c[3] * 0.5f) }, r[4];
    if (tr->mode == 1) { // the root (no parent): back at its first frame's place, its turn since then taken off
        q_mul(hq, pose[0].q, r);
        memcpy(pose[0].q, r, sizeof r);
        pose[0].t[0] -= c[0], pose[0].t[1] -= c[1], pose[0].t[2] -= c[2]; // (z: only a rising root's track has it)
        return;
    }
    int16_t pel = rm_pelvis_of(m); // the pelvis: under the root (taken as it is: its place in the root's frame)
    if (pel < 0) return;
    float rq[4] = { pose[0].q[0], pose[0].q[1], pose[0].q[2], pose[0].q[3] }, rqi[4] = { -rq[0], -rq[1], -rq[2], rq[3] };
    float dw[4] = { -c[0], -c[1], 0.0f, 0.0f }, tmp[4], dl[4];
    q_mul(rqi, dw, tmp), q_mul(tmp, rq, dl); // (the world offset into the root's frame)
    pose[pel].t[0] += dl[0], pose[pel].t[1] += dl[1], pose[pel].t[2] += dl[2];
    float t2[4], t3[4]; // the turn: root^-1 * turn * root * pelvis
    q_mul(rqi, hq, t2), q_mul(t2, rq, t3), q_mul(t3, pose[pel].q, r);
    memcpy(pose[pel].q, r, sizeof r);
}

// A pair started: both montages on the same frame
bool anim_play_paired(World* w, Entity primary, Entity partner, const char* montage, float rate) {
    AnimMontage* mp = anim_montage_name(montage);
    if (!mp || !primary || !partner || primary == partner) return false;
    AnimMontage* ms = mp->partner[0] ? anim_montage_name(mp->partner) : NULL;
    if (!anim_play_montage(w, primary, montage, rate)) return false;
    if (ms) anim_play_montage(w, partner, ms->name, rate);
    int slot = -1;
    for (int i = 0; i < PAIRS; i++) {
        if (g_pairs[i].on && (g_pairs[i].p == primary || g_pairs[i].s == partner || g_pairs[i].s == primary)) g_pairs[i].on = false; // (a new one replaces it)
        if (!g_pairs[i].on && slot < 0) slot = i;
    }
    if (slot < 0) return false;
    struct Pair* p = &g_pairs[slot];
    memset(p, 0, sizeof *p);
    const CTransform* st = ecs_get(w, partner, COMP_TRANSFORM);
    if (!st) return false;
    p->p = primary, p->s = partner, p->mp = mp, p->ms = ms, p->align = fmaxf(mp->partner_align, 0.0f), p->on = true;
    memcpy(p->s0, st->location, sizeof(float) * 3), p->s0[3] = st->rotation[1];
    AnimInst* pin = inst_of_actor(w, primary);
    p->start_t = pin ? pin->mtime : 0.0f;
    return true;
}
void anim_stop_paired(World* w, Entity actor, float blend_out) {
    for (int i = 0; i < PAIRS; i++)
        if (g_pairs[i].on && (g_pairs[i].p == actor || g_pairs[i].s == actor)) {
            anim_stop_montage(w, g_pairs[i].p, blend_out), anim_stop_montage(w, g_pairs[i].s, blend_out);
            g_pairs[i].on = false;
        }
}
bool anim_paired_partner(World* w, Entity actor, Entity* other) {
    (void)w;
    for (int i = 0; i < PAIRS; i++)
        if (g_pairs[i].on && (g_pairs[i].p == actor || g_pairs[i].s == actor)) {
            if (other) *other = g_pairs[i].p == actor ? g_pairs[i].s : g_pairs[i].p;
            return true;
        }
    return false;
}

// The partner placed from its primary at the pair's time t: S(t) = P(t) o Rp(t)^-1 o Offset o Rs(t), slid in over Align Time
static void pair_place(World* w, struct Pair* p, AnimInst* pin, AnimInst* sin, CTransform* pt, CTransform* st, float t) {
    // the primary's place as its root motion has it now (what it took this frame is still waiting for its movement)
    struct RmPending* pd = rm_pending(p->p, false);
    float P[4] = { pt->location[0], pt->location[1], pt->location[2], pt->rotation[1] * DEG2RAD };
    if (pd) P[0] += pd->d[0], P[1] += pd->d[1], P[2] += pd->d[2], P[3] += pd->yaw * DEG2RAD;
    // P(t) o Rp(t)^-1: where the primary would be with no root motion (its clip's start); then the offset, then the
    // partner's own root motion
    float rp[4] = { 0, 0, 0, 0 }, rs[4] = { 0, 0, 0, 0 }, sc;
    rm_mesh_yaw(w, pin, &sc);
    if (p->mp->root_motion && pin->rm_tr) {
        float r0[4];
        rm_at(pin->rm_tr, p->start_t, r0), rm_at(pin->rm_tr, t, rp);
        for (int k = 0; k < 4; k++) rp[k] -= r0[k];
        rp[0] *= sc, rp[1] *= sc, rp[2] *= sc;
    }
    if (p->ms && p->ms->root_motion && sin->rm_tr) {
        float r0[4], ss;
        rm_mesh_yaw(w, sin, &ss);
        rm_at(sin->rm_tr, p->start_t, r0), rm_at(sin->rm_tr, t, rs);
        for (int k = 0; k < 4; k++) rs[k] -= r0[k];
        rs[0] *= ss, rs[1] *= ss, rs[2] *= ss;
    }
    float by = P[3] - rp[3], cb = cosf(by), sb = sinf(by); // (the primary's start facing)
    float B[3] = { P[0] - (cb * rp[0] - sb * rp[1]), P[1] - (sb * rp[0] + cb * rp[1]), P[2] - rp[2] };
    const AnimMontage* mo = p->mp;
    float ox = mo->partner_offset[0], oy = mo->partner_offset[1];
    float O[3] = { B[0] + cb * ox - sb * oy, B[1] + sb * ox + cb * oy, 0.0f };
    float oyaw = by + mo->partner_yaw * DEG2RAD, co = cosf(oyaw), so = sinf(oyaw);
    float S[4] = { O[0] + co * rs[0] - so * rs[1], O[1] + so * rs[0] + co * rs[1], p->s0[2] + mo->partner_offset[2] + rs[2], oyaw + rs[3] };
    float a = p->align > 1e-4f ? fminf((t - p->start_t) / p->align, 1.0f) : 1.0f; // (sliding into place)
    a = a * a * (3.0f - 2.0f * a);
    float syaw = S[3] / DEG2RAD, dy = remainderf(syaw - p->s0[3], 360.0f);
    st->location[0] = p->s0[0] + (S[0] - p->s0[0]) * a, st->location[1] = p->s0[1] + (S[1] - p->s0[1]) * a;
    st->location[2] = p->s0[2] + (S[2] - p->s0[2]) * a;
    st->rotation[1] = remainderf(p->s0[3] + dy * a, 360.0f);
    CCharacter* sc2 = ecs_get(w, p->s, COMP_CHARACTER); // (its movement held: no input, no fall)
    if (sc2) sc2->velocity[0] = sc2->velocity[1] = sc2->velocity[2] = 0.0f, sc2->input[0] = sc2->input[1] = sc2->input[2] = 0.0f;
    ecs_touch(w, p->s);
}

// Each frame after the logic, before the poses: the partners' time and place from their primaries
static void pairs_update(World* w) {
    for (int i = 0; i < PAIRS; i++) {
        struct Pair* p = &g_pairs[i];
        if (!p->on) continue;
        AnimInst* pin = inst_of_actor(w, p->p);
        AnimInst* sin = inst_of_actor(w, p->s);
        CTransform* pt = ecs_get(w, p->p, COMP_TRANSFORM);
        CTransform* st = ecs_get(w, p->s, COMP_TRANSFORM);
        if (!pin || !sin || !pt || !st || pin->montage != p->mp) { // (its primary ended)
            // its own end this frame (not stopped): the partner placed at the clip's end once more (this frame's last
            // step of the primary's root motion is in its pending move)
            if (pin && sin && pt && st && !pin->montage && !pin->mcut && pin->mclip.seq) pair_place(w, p, pin, sin, pt, st, pin->mclip.seq->length);
            if (sin && sin->montage && !sin->mstopping) anim_stop_montage(w, p->s, p->mp ? fmaxf(p->mp->blend_out, 0.05f) : 0.25f);
            p->on = false;
            continue;
        }
        if (sin->montage) { // the same frame, the same rate
            sin->mtime = pin->mtime, sin->mrate = pin->mrate;
            if (pin->mstopping && !sin->mstopping) sin->mstopping = true, sin->mcut = pin->mcut, sin->mblend_out = pin->mblend_out;
        }
        pair_place(w, p, pin, sin, pt, st, pin->mtime);
    }
}
static void combo_end_play(void); // (anim_combo.inc)
static void cover_end_play(void); // (anim_cover.inc)
static void warp_end_play(void) {
    combo_end_play(), cover_end_play();
    g_nwt = 0, g_nrmp = 0;
    memset(g_pairs, 0, sizeof g_pairs);
}
