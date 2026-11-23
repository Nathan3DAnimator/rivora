// Motion Matching's made animations (notas-dev/animacao.md), included by anim_mm.inc. From a few clips (an idle, a walk,
// a run; forward and to the side) the database gets the ones between them, in the clips' own style:
//   * directions between two clips of about the same speed (forward and to the side: every 22.5 degrees): the two at
//     the same phase (from their left foot's landing); a blend of 0 and 90 degrees is slower than either, so the cadence
//     and the stride each grow by the square root of what it lost
//   * starts (idle -> each locomotion) and stops (each locomotion -> idle, from 8 places of its cycle): the speed ramps
//     at the body's acceleration; the cadence and the stride grow as the square root of the speed (the walk ratio, step
//     length over step rate, stays the same), so the planted foot keeps pace with the body. A stop is as long as the body's
//     braking (the same distance as the Character's), into the idle (or its mirror) whose feet need the least
//     correction; a foot left behind takes a closing step
//   * speed changes (walk <-> run): the two at the same phase, the cycle between theirs
// Each one is baked (30 frames a second) and made physical: the body leans with its acceleration (atan(a / g) through a
// damped spring: it sways when it stops, slower when heavier), the pelvis dips with it; the planted feet are pinned to
// the ground (no sliding), the swinging ones carry the correction from one plant to the next (the stride warped), the
// pelvis drops where a leg would not reach, the legs by two bone IK. The acceleration comes from the clips' own timing:
// the steps to reach a speed grow with its Froude number (v^2 / g L), each as long as the clip's step: a heavy slow
// cadence starts slowly, a quick light one snaps. The user's settings (AnimState.mmgen) go over it. Baked on the
// workers (re_parallel_for): the directions first (the starts and stops of those directions blend them)

#define MMG_G 981.0f
#define MMG_MAX_SRC 64
#define MMG_MAX_PLANS 640
#define MMG_MAX_FRAMES 150

static void turn_bones(const SkelMesh* m, AnimXf* pose, const float rows[3][4], float bones[][3][4], const int16_t* bs, const float* share,
                       int n, const float axis1[3], float a1, const float axis2[3], float a2); // (anim_nodes.inc)
static void two_bone_to(const SkelMesh* m, AnimXf* pose, const float rows[3][4], float bones[][3][4], int16_t th, int16_t ca, int16_t fb,
                        const float T[3], float alpha);

static const float g_mmg_id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

typedef struct MmBody {
    int16_t foot[2], calf[2], thigh[2], pelvis, spine[3], head;
    float leg; // thigh to foot (cm): the Froude number's length
} MmBody;

// A steady motion the made ones blend: a clip's group at 1x (an idle, a locomotion) or a made direction
typedef struct MmSrc {
    int16_t group, note;   // its group; the clip source whose notifies it fires
    float speed, v[2];     // cm/s, its velocity (root space)
    float cycle, phase0;   // its cycle (s), its left foot's landing (s, its time)
    float pelvis;          // the pelvis' mean height (its posture: standing, crouched)
    float low[2];          // each foot's lowest (its height planted)
    bool idle, canon;      // canon: the first of its kind (direction, speed, posture)
    char name[48];
} MmSrc;

typedef struct MmPlan {
    uint8_t kind;
    int16_t a, b;          // DIRECTION: the two (b weighted w); START: the idle, the locomotion; STOP: the locomotion,
                           // the idle (a mirrored one tried too); SPEED: from, to
    float w, k;            // DIRECTION: b's weight, the cadence and stride factor
    float u0, dur;         // the first phase (0-1; START: < 0, picked), the ramp (s)
    float v[2];            // DIRECTION: its velocity
    uint32_t frames;       // its room (the most it may take)
    int32_t group;
} MmPlan;

typedef struct MmGen {
    MmDb* d;
    const SkelMesh* m;
    MmBody B;
    int16_t feet[2];
    float p[ANIM_MMGEN];
    MmSrc src[MMG_MAX_SRC];
    uint32_t nsrc, nplan, nlist;
    MmPlan plan[MMG_MAX_PLANS];
    uint16_t list[MMG_MAX_PLANS]; // (a pass' plans: the bake jobs')
} MmGen;
static MmGen g_mmg; // (made on the main thread, one database at a time)

// The speed's share along a ramp (u 0-1): a constant acceleration, its rise and fall eased over a quarter each (no jerk)
static float mmg_ramp(float u) {
    const float k = 0.25f, p = 1.0f / (1.0f - k);
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    if (u < k) return p * u * u / (2.0f * k);
    if (u <= 1.0f - k) return p * (k * 0.5f + (u - k));
    float x = u - (1.0f - k);
    return p * (k * 0.5f + (1.0f - 2.0f * k) + x - x * x / (2.0f * k));
}

static float mmg_wrap(float a) { // (radians to -pi..pi)
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static int16_t mmg_find(const SkelMesh* m, const char* a, const char* b, int nth) {
    for (uint32_t i = 0; i < m->nbones; i++)
        if ((strstr(m->bones[i].name, a) || strstr(m->bones[i].name, b)) && nth-- == 0) return (int16_t)i;
    return -1;
}

static float mmg_dist(const float a[3][4], const float b[3][4]) {
    float x = a[0][3] - b[0][3], y = a[1][3] - b[1][3], z = a[2][3] - b[2][3];
    return sqrtf(x * x + y * y + z * z);
}

static bool mmg_body(const AnimGraph* graph, const SkelMesh* m, const int16_t feet[2], MmBody* B) {
    for (int f = 0; f < 2; f++) {
        B->foot[f] = feet[f], B->calf[f] = m->bones[feet[f]].parent;
        B->thigh[f] = B->calf[f] >= 0 ? m->bones[B->calf[f]].parent : -1;
        if (B->calf[f] < 0 || B->thigh[f] < 0) return false;
    }
    B->pelvis = m->bones[B->thigh[0]].parent;
    if (B->pelvis < 0) return false;
    for (int k = 0; k < 3; k++)
        B->spine[k] = graph && graph->aim_bones[k][0] ? bone_named(m, graph->aim_bones[k]) : mmg_find(m, "spine", "Spine", k);
    B->head = graph && graph->aim_bones[3][0] ? bone_named(m, graph->aim_bones[3]) : mmg_find(m, "head", "Head", 0);
    B->leg = mmg_dist(m->bind_world[B->thigh[0]], m->bind_world[B->calf[0]]) + mmg_dist(m->bind_world[B->calf[0]], m->bind_world[B->foot[0]]);
    return B->leg > 5.0f;
}

// A bone moved by d (root space): its local translation through its parent's rows
static void mmg_move_bone(const SkelMesh* m, AnimXf* pose, int16_t b, const float d[3], float bones[][3][4]) {
    int16_t p = m->bones[b].parent;
    for (int a = 0; a < 3; a++) {
        if (p < 0) {
            pose[b].t[a] += d[a];
            continue;
        }
        float l2 = bones[p][0][a] * bones[p][0][a] + bones[p][1][a] * bones[p][1][a] + bones[p][2][a] * bones[p][2][a];
        if (l2 > 1e-12f) pose[b].t[a] += (bones[p][0][a] * d[0] + bones[p][1][a] * d[1] + bones[p][2][a] * d[2]) / l2;
    }
}

// A source's pose: a locomotion at its phase u (0-1 from its left foot's landing), an idle at its time
static void mmg_src_pose(const MmGen* G, const MmSrc* s, float x, bool time, AnimXf* out) {
    const MmGroup* g = &G->d->groups[s->group];
    float t = time ? x : s->phase0 + x * s->cycle, len = g->len > 1e-3f ? g->len : 1.0f;
    t = fmodf(t, len);
    if (t < 0.0f) t += len;
    mm_sample(G->d, G->m, g, t, out);
}

// A steady source's cycle (its left foot's landings: on the ground within 2.5 cm of its lowest) and posture
static void mmg_cycle(const MmGen* G, MmSrc* s) {
    const MmGroup* g = &G->d->groups[s->group];
    uint32_t nf = (uint32_t)(g->len * MM_HZ + 0.5f);
    if (nf > 255) nf = 255;
    s->cycle = g->len, s->phase0 = 0.0f, s->pelvis = 0.0f;
    if (nf < 2) return;
    AnimXf pose[ANIM_MAX_BONES];
    static float bones[ANIM_MAX_BONES][3][4], z[256];
    float low = 1e30f;
    s->low[0] = s->low[1] = 1e30f;
    for (uint32_t f = 0; f < nf; f++) {
        mm_sample(G->d, G->m, g, (float)f / MM_HZ, pose);
        anim_world(G->m, pose, g_mmg_id, bones);
        z[f] = bones[G->B.foot[0]][2][3], low = fminf(low, z[f]);
        for (int k = 0; k < 2; k++) s->low[k] = fminf(s->low[k], bones[G->B.foot[k]][2][3]);
        s->pelvis += bones[G->B.pelvis][2][3] / (float)nf;
    }
    if (s->idle) return;
    int first = -1, n = 0;
    for (uint32_t f = 0; f < nf; f++)
        if (z[f] < low + 2.5f && !(z[(f + nf - 1) % nf] < low + 2.5f)) {
            if (first < 0) first = (int)f;
            n++;
        }
    if (n) s->cycle = g->len / (float)n, s->phase0 = (float)first / MM_HZ;
}

static float mmg_angle(const MmSrc* s) { return atan2f(s->v[1], s->v[0]); }

static bool mmg_similar(const MmGen* G, const MmSrc* a, const MmSrc* b, float deg, float ratio, float posture) {
    float r = fmaxf(a->speed, b->speed) / fmaxf(fminf(a->speed, b->speed), 1.0f);
    return fabsf(mmg_wrap(mmg_angle(a) - mmg_angle(b))) < deg * 0.0174533f && r < ratio && fabsf(a->pelvis - b->pelvis) < posture * G->B.leg;
}

// The body's mean acceleration starting and stopping at a source's speed: its steps to reach it grow with the Froude
// number (a walk ~1.6, a run ~3), each step half the clip's cycle, times the weight; in human ranges (0.08-0.75 g starting,
// 0.1-0.8 g stopping); the user's values over them
static void mmg_accel(const MmGen* G, const MmSrc* s, float* acc, float* dec) {
    float fr = fminf(s->speed * s->speed / (MMG_G * G->B.leg), 2.0f), step = fmaxf(s->cycle * 0.5f, 0.1f), wg = G->p[ANIM_MMGEN_WEIGHT];
    float a = s->speed / ((1.5f + 0.75f * fr) * step * wg), b = s->speed / ((1.0f + 0.6f * fr) * step * wg);
    *acc = G->p[ANIM_MMGEN_ACCEL] > 0.0f ? G->p[ANIM_MMGEN_ACCEL] : fminf(fmaxf(a, 0.08f * MMG_G), 0.75f * MMG_G);
    *dec = G->p[ANIM_MMGEN_DECEL] > 0.0f ? G->p[ANIM_MMGEN_DECEL] : fminf(fmaxf(b, 0.1f * MMG_G), 0.8f * MMG_G);
}

// The idles for a locomotion: the clip of the nearest posture (both of its mirrors); false: none within 15% of the leg
static bool mmg_idles(const MmGen* G, const MmSrc* s, int16_t out[2]) {
    float best = 0.15f * G->B.leg;
    out[0] = out[1] = -1;
    for (uint32_t i = 0; i < G->nsrc; i++) {
        const MmSrc* c = &G->src[i];
        if (!c->idle) continue;
        float e = fabsf(c->pelvis - s->pelvis);
        const MmGroup* g = &G->d->groups[c->group];
        if (out[0] >= 0 && G->d->groups[G->src[out[0]].group].clip == g->clip) {
            if (g->mirror) out[1] = (int16_t)i;
            continue;
        }
        if (e < best) best = e, out[0] = (int16_t)i, out[1] = -1;
    }
    return out[0] >= 0;
}

// A start or a stop of the user's own covering a locomotion's (its speed within 25%, its way within 30 degrees): not made
static bool mmg_covered(const MmDb* d, int kind, const MmSrc* S) {
    for (uint32_t i = 0; i < d->ncover; i++) {
        const float* v = d->cover[i].dir;
        if (d->cover[i].kind == kind && fabsf(d->cover[i].speed - S->speed) < 0.25f * S->speed &&
            (v[0] * S->v[0] + v[1] * S->v[1]) / fmaxf(S->speed, 1.0f) > 0.866f)
            return true;
    }
    return false;
}

static MmPlan* mmg_plan(MmGen* G, uint8_t kind, int a, int b) {
    if (G->nplan >= MMG_MAX_PLANS) return NULL;
    MmGroup* g = mm_add_group(G->d);
    if (!g) return NULL;
    MmPlan* p = &G->plan[G->nplan++];
    memset(p, 0, sizeof *p);
    p->kind = kind, p->a = (int16_t)a, p->b = (int16_t)b, p->group = (int32_t)(g - G->d->groups);
    g->kind = kind, g->clip = G->d->groups[G->src[kind == ANIM_MMG_START ? b : a].group].clip;
    G->d->made[kind]++;
    return p;
}

static uint32_t mmg_room(float s) {
    uint32_t n = (uint32_t)ceilf(s * MM_HZ) + 1;
    return n < MMG_MAX_FRAMES ? n : MMG_MAX_FRAMES;
}

// --- The bake ---

// The planted feet pinned (one foot's frames): each stay on the ground (within 2.5 cm of its height planted, H: the
// sources' blended - a run lands higher on the foot than an idle stands) keeps its place in the world (the body moves by
// X) - its frames' mean; one touching an end keeps that end's (a start begins, a stop ends exactly as their neighbors).
// A last stay that landed away from where the end stands takes a closing step there once the body has stopped and the
// other foot is down (lifted by L: an arc). In the air the correction eases from one plant's to the next's. The largest
static float mmg_pins(uint32_t nf, bool loop, int f, float (*P)[2][3], float (*Z)[2], float (*H)[2], float (*X)[2], float (*V)[2],
                      const float xc[2], float (*D)[2][2], float (*L)[2]) {
    bool c[MMG_MAX_FRAMES], co[MMG_MAX_FRAMES];
    float worst = 0.0f;
    uint32_t nc = 0;
    for (uint32_t i = 0; i < nf; i++) {
        c[i] = Z[i][f] < H[i][f] + 2.5f, nc += c[i];
        co[i] = Z[i][!f] < H[i][!f] + 2.5f && (f == 0 || L[i][0] == 0.0f); // (the other foot down and not stepping: the left one steps first)
        D[i][f][0] = D[i][f][1] = 0.0f, L[i][f] = 0.0f;
    }
    if (!nc || (loop && nc == nf)) return 0.0f; // (never down; a loop never up: nothing to pin)
    // the stays, in a sequence that does not start inside one (a loop: from a frame in the air)
    uint32_t st = 0;
    if (loop)
        while (c[st]) st++;
    int32_t rs[48], re[48];
    int nr = 0;
    for (uint32_t j = st; j < st + (loop ? nf : nf - st) && nr < 48; j++) {
        if (!c[j % nf]) continue;
        uint32_t e = j;
        while (e + 1 < st + nf && c[(e + 1) % nf] && (loop || e + 1 < nf)) e++;
        rs[nr] = (int32_t)j, re[nr++] = (int32_t)e;
        j = e;
    }
#define MMG_X(J, k) (X[(J) % nf][k] + ((J) >= (int32_t)nf ? xc[k] : 0.0f))
    for (int r = 0; r < nr; r++) {
        float pin[2] = { 0, 0 };
        if (!loop && re[r] == (int32_t)nf - 1) { // (the end's place; landed elsewhere: a closing step to it)
            float p0[2] = { X[rs[r]][0] + P[rs[r]][f][0], X[rs[r]][1] + P[rs[r]][f][1] };
            float p1[2] = { X[nf - 1][0] + P[nf - 1][f][0], X[nf - 1][1] + P[nf - 1][f][1] }, gap = hypotf(p1[0] - p0[0], p1[1] - p0[1]);
            int32_t sl = (int32_t)(0.35f * MM_HZ), e = rs[r] + sl + 2, s;
            bool ok = false;
            for (; gap > 3.0f && e <= re[r] - 2 && !ok; e++) { // (the body stopped, the other foot down all along)
                if (hypotf(V[e][0], V[e][1]) > 15.0f) continue;
                ok = true;
                for (int32_t J = e - sl; J <= e && ok; J++) ok = co[J];
            }
            if (ok) {
                e--, s = e - sl;
                float h = fminf(fmaxf(gap * 0.3f, 3.0f), 9.0f);
                for (int32_t J = rs[r]; J <= re[r]; J++) {
                    float x = J <= s ? 0.0f : J >= e ? 1.0f : (float)(J - s) / (float)(e - s), sm = x * x * (3.0f - 2.0f * x);
                    for (int k = 0; k < 2; k++) D[J][f][k] = p0[k] + (p1[k] - p0[k]) * sm - (X[J][k] + P[J][f][k]);
                    L[J][f] = h * sinf(3.14159265f * x);
                }
                continue;
            }
            memcpy(pin, p1, 8);
        } else if (!loop && rs[r] == 0)
            pin[0] = X[0][0] + P[0][f][0], pin[1] = X[0][1] + P[0][f][1];
        else {
            for (int32_t J = rs[r]; J <= re[r]; J++)
                for (int k = 0; k < 2; k++) pin[k] += MMG_X(J, k) + P[J % nf][f][k];
            for (int k = 0; k < 2; k++) pin[k] /= (float)(re[r] - rs[r] + 1);
        }
        for (int32_t J = rs[r]; J <= re[r]; J++)
            for (int k = 0; k < 2; k++) D[J % nf][f][k] = pin[k] - (MMG_X(J, k) + P[J % nf][f][k]);
    }
#undef MMG_X
    // the swings: from one stay's last correction to the next's first (a one-shot's ends: none)
    for (int r = 0; r <= nr; r++) {
        int32_t a, b;
        float da[2] = { 0, 0 }, db[2] = { 0, 0 };
        if (r < nr - 1 || (loop && r == nr - 1)) {
            a = re[r], b = r < nr - 1 ? rs[r + 1] : rs[0] + (int32_t)nf;
        } else if (!loop && r == nr - 1) {
            a = re[r], b = (int32_t)nf - 1;
            if (a >= b) continue;
        } else if (!loop && r == nr) {
            if (rs[0] == 0) continue;
            a = 0, b = rs[0];
            memcpy(db, D[b % nf][f], 8);
            for (int32_t J = a; J < b; J++) {
                float x = (float)(J - a) / (float)(b - a), s = x * x * (3.0f - 2.0f * x);
                for (int k = 0; k < 2; k++) D[J][f][k] = db[k] * s;
            }
            continue;
        } else
            continue;
        memcpy(da, D[a % nf][f], 8);
        if (!(!loop && r == nr - 1)) memcpy(db, D[b % nf][f], 8);
        for (int32_t J = a + 1; J < b; J++) {
            float x = (float)(J - a) / (float)(b - a), s = x * x * (3.0f - 2.0f * x);
            for (int k = 0; k < 2; k++) D[J % nf][f][k] = da[k] + (db[k] - da[k]) * s;
        }
        if (!loop && r == nr - 1) D[b][f][0] = D[b][f][1] = 0.0f;
    }
    for (uint32_t i = 0; i < nf; i++) worst = fmaxf(worst, hypotf(D[i][f][0], D[i][f][1]));
    return worst;
}

// A plan's frames: the sources blended along it, then made physical (out: its frames; NULL: only the feet, the
// correction they need returned - a stop's candidates). idle: the idle source (STOP), dur its ramp, nf its frames
static float mmg_run(const MmGen* G, const MmPlan* p, int16_t idle, float dur, uint32_t nf, AnimXf* out, float* vel, MmGroup* grp, float* u_end) {
    static _Thread_local float P[MMG_MAX_FRAMES][2][3], X[MMG_MAX_FRAMES][2], D[MMG_MAX_FRAMES][2][2], V[MMG_MAX_FRAMES][2];
    static _Thread_local float Z[MMG_MAX_FRAMES][2], H[MMG_MAX_FRAMES][2], Lf[MMG_MAX_FRAMES][2]; // (the feet's heights before the lean, planted; lifted)
    static _Thread_local float bones[ANIM_MAX_BONES][3][4];
    const SkelMesh* m = G->m;
    const MmBody* B = &G->B;
    uint32_t nb = m->nbones;
    const MmSrc* A = &G->src[p->a];
    const MmSrc* S = &G->src[p->b];
    const MmSrc* I = idle >= 0 ? &G->src[idle] : NULL;
    AnimXf pa[ANIM_MAX_BONES], pb[ANIM_MAX_BONES], tmp[ANIM_MAX_BONES];
    const float dt = 1.0f / MM_HZ;
    bool loop = p->kind == ANIM_MMG_DIRECTION;
    float u = p->u0, ct_prev = -1.0f;
    int dom_prev = -1;
    if (grp) grp->nev = 0;
    for (uint32_t i = 0; i < nf; i++) {
        float t = (float)i * dt, f = 1.0f, w = 1.0f, umid = 0.0f, rate = 0.0f;
        AnimXf* pose = out ? &out[(size_t)i * nb] : tmp;
        const MmSrc* dom = NULL; // (whose notifies, at phase udom)
        float udom = u;
        switch (p->kind) {
        case ANIM_MMG_DIRECTION:
            u = (float)i / (float)nf, udom = u;
            mmg_src_pose(G, A, u, false, pa), mmg_src_pose(G, S, u, false, pb);
            anim_blend(pa, pb, p->w, nb, pose);
            V[i][0] = p->v[0], V[i][1] = p->v[1];
            dom = p->w < 0.5f ? A : S;
            break;
        case ANIM_MMG_START: // the idle (a) into the locomotion (b): weight and cadence by the root of the speed
            f = mmg_ramp(t / dur), w = sqrtf(f);
            mmg_src_pose(G, A, t, true, pa), mmg_src_pose(G, S, u, false, pb);
            anim_blend(pa, pb, w, nb, pose);
            V[i][0] = S->v[0] * f, V[i][1] = S->v[1] * f;
            umid = mmg_ramp((t + dt * 0.5f) / dur), rate = sqrtf(umid) / S->cycle;
            dom = w >= 0.5f ? S : NULL;
            break;
        case ANIM_MMG_STOP: // the locomotion (a) into the idle
            f = 1.0f - mmg_ramp(t / dur), w = sqrtf(f);
            mmg_src_pose(G, A, u, false, pa), mmg_src_pose(G, I, t, true, pb);
            anim_blend(pa, pb, 1.0f - w, nb, pose);
            V[i][0] = A->v[0] * f, V[i][1] = A->v[1] * f;
            umid = 1.0f - mmg_ramp((t + dt * 0.5f) / dur), rate = sqrtf(fmaxf(umid, 0.0f)) / A->cycle;
            dom = w >= 0.4f ? A : NULL;
            break;
        default: // SPEED: a into b at the same phase, the cycle between theirs
            f = mmg_ramp(t / dur);
            mmg_src_pose(G, A, u, false, pa), mmg_src_pose(G, S, u, false, pb);
            anim_blend(pa, pb, f, nb, pose);
            V[i][0] = A->v[0] + (S->v[0] - A->v[0]) * f, V[i][1] = A->v[1] + (S->v[1] - A->v[1]) * f;
            umid = mmg_ramp((t + dt * 0.5f) / dur), rate = 1.0f / (A->cycle + (S->cycle - A->cycle) * umid);
            dom = f < 0.5f ? A : S;
            break;
        }
        if (u_end) *u_end = u;
        mm_feet(m, pose, G->feet, P[i]);
        for (int k = 0; k < 2; k++) {
            Z[i][k] = P[i][k][2];
            switch (p->kind) {
            case ANIM_MMG_DIRECTION: H[i][k] = A->low[k] + (S->low[k] - A->low[k]) * p->w; break;
            case ANIM_MMG_START: H[i][k] = A->low[k] + (S->low[k] - A->low[k]) * w; break;
            case ANIM_MMG_STOP: H[i][k] = I->low[k] + (A->low[k] - I->low[k]) * w; break;
            default: H[i][k] = A->low[k] + (S->low[k] - A->low[k]) * f; break;
            }
        }
        if (out && grp && dom) { // the dominant source's notifies, crossed since the last frame (a clip's times)
            const MmSrc* N = &G->src[dom->note];
            const MmGroup* ng = &G->d->groups[N->group];
            const AnimSeq* q = G->d->clips[ng->clip];
            float len = ng->len > 1e-3f ? ng->len : 1.0f, ct = fmodf(N->phase0 + udom * N->cycle, len);
            int di = (int)(dom - G->src);
            if (q && dom_prev == di && ct_prev >= 0.0f) {
                float a = ct_prev, b = ct >= ct_prev ? ct : ct + len;
                for (uint32_t k = 0; k < q->nnotifies && grp->nev < 8; k++) {
                    float nt = q->notifies[k].time;
                    if ((nt > a && nt <= b) || (nt + len > a && nt + len <= b))
                        grp->ev_t[grp->nev] = t, grp->ev_clip[grp->nev] = ng->clip, grp->ev_idx[grp->nev++] = (uint8_t)k;
                }
            }
            ct_prev = ct, dom_prev = di;
        } else
            dom_prev = -1;
        u += rate * dt;
    }
    // the lean with the acceleration (a one-shot's): a damped spring after atan(a / g), the pelvis takes 40%, the spine
    // the rest, the head turns back half of it (the eyes level); the pelvis dips with it
    if (out && !loop) {
        float lv[2] = { 0, 0 }, lr[2] = { 0, 0 }, wn = 6.2831853f * 1.8f / sqrtf(G->p[ANIM_MMGEN_WEIGHT]);
        float zeta = 1.0f - 0.7f * G->p[ANIM_MMGEN_SETTLE], gain = 0.75f * G->p[ANIM_MMGEN_LEAN];
        for (uint32_t i = 0; i < nf; i++) {
            uint32_t i0 = i ? i - 1 : 0, i1 = i + 1 < nf ? i + 1 : nf - 1;
            float a[2] = { (V[i1][0] - V[i0][0]) * MM_HZ / (float)(i1 - i0), (V[i1][1] - V[i0][1]) * MM_HZ / (float)(i1 - i0) };
            float am = hypotf(a[0], a[1]), th = fminf(atanf(am / MMG_G) * gain, 0.45f), target[2] = { 0, 0 };
            if (am > 1e-3f) target[0] = a[0] / am * th, target[1] = a[1] / am * th;
            for (int s = 0; s < 4; s++)
                for (int k = 0; k < 2; k++) {
                    float h = dt * 0.25f, acc = wn * wn * (target[k] - lv[k]) - 2.0f * zeta * wn * lr[k];
                    lr[k] += acc * h, lv[k] += lr[k] * h;
                }
            float ang = hypotf(lv[0], lv[1]);
            if (ang < 1e-4f) continue;
            AnimXf* pose = &out[(size_t)i * nb];
            float axis[3] = { -lv[1] / ang, lv[0] / ang, 0.0f }, dz[3] = { 0, 0, -ang * 0.2f * B->leg };
            turn_bones(m, pose, g_mmg_id, bones, &B->pelvis, NULL, 1, axis, ang * 0.4f, axis, 0.0f);
            turn_bones(m, pose, g_mmg_id, bones, B->spine, NULL, 3, axis, ang * 0.6f, axis, 0.0f);
            if (B->head >= 0) turn_bones(m, pose, g_mmg_id, bones, &B->head, NULL, 1, axis, -ang * 0.5f, axis, 0.0f);
            anim_world(m, pose, g_mmg_id, bones);
            mmg_move_bone(m, pose, B->pelvis, dz, bones);
        }
    }
    if (out)
        for (uint32_t i = 0; i < nf; i++) { // (where the lean put them; at their heights before it: the dip is the body's)
            mm_feet(m, &out[(size_t)i * nb], G->feet, P[i]);
            P[i][0][2] = Z[i][0], P[i][1][2] = Z[i][1];
        }
    // the body's travel; the feet pinned
    X[0][0] = X[0][1] = 0.0f;
    for (uint32_t i = 1; i < nf; i++)
        for (int k = 0; k < 2; k++) X[i][k] = X[i - 1][k] + (V[i - 1][k] + V[i][k]) * 0.5f * dt;
    float xc[2] = { V[0][0] * (float)nf * dt, V[0][1] * (float)nf * dt }, worst = 0.0f;
    for (int f = 0; f < 2; f++) worst = fmaxf(worst, mmg_pins(nf, loop, f, P, Z, H, X, V, xc, D, Lf));
    if (vel)
        for (uint32_t i = 0; i < nf; i++) vel[i * 2] = V[i][0], vel[i * 2 + 1] = V[i][1];
    if (!out) return worst;
    // where a leg would not reach its target, the pelvis drops (spread over a few frames)
    float need[MMG_MAX_FRAMES], drop[MMG_MAX_FRAMES];
    for (uint32_t i = 0; i < nf; i++) {
        need[i] = 0.0f;
        anim_world(m, &out[(size_t)i * nb], g_mmg_id, bones);
        for (int f = 0; f < 2; f++) {
            const float* hip = (const float[]){ bones[B->thigh[f]][0][3], bones[B->thigh[f]][1][3], bones[B->thigh[f]][2][3] };
            float reach = (mmg_dist(bones[B->thigh[f]], bones[B->calf[f]]) + mmg_dist(bones[B->calf[f]], bones[B->foot[f]])) * 0.995f;
            float tx = P[i][f][0] + D[i][f][0] - hip[0], ty = P[i][f][1] + D[i][f][1] - hip[1], down = hip[2] - P[i][f][2];
            float h2 = reach * reach - tx * tx - ty * ty, zmax = h2 > 0.0f ? sqrtf(h2) : 0.0f;
            if (down > zmax) need[i] = fmaxf(need[i], fminf(down - zmax, 0.25f * B->leg));
        }
    }
    for (uint32_t i = 0; i < nf; i++) {
        float mx = 0.0f;
        for (int o = -3; o <= 3; o++) {
            int32_t j = (int32_t)i + o;
            if (loop) j = (j + (int32_t)nf) % (int32_t)nf;
            else if (j < 0 || j >= (int32_t)nf) continue;
            mx = fmaxf(mx, need[j]);
        }
        drop[i] = mx;
    }
    for (uint32_t i = 0; i < nf; i++) {
        float s = 0.0f, c = 0.0f;
        for (int o = -2; o <= 2; o++) {
            int32_t j = (int32_t)i + o;
            if (loop) j = (j + (int32_t)nf) % (int32_t)nf;
            else if (j < 0 || j >= (int32_t)nf) continue;
            s += drop[j], c += 1.0f;
        }
        need[i] = s / c * (loop ? 1.0f : fminf(fminf((float)i, (float)(nf - 1 - i)) / 3.0f, 1.0f));
    }
    for (uint32_t i = 0; i < nf; i++) {
        AnimXf* pose = &out[(size_t)i * nb];
        bool moved = false;
        if (need[i] > 0.01f) {
            anim_world(m, pose, g_mmg_id, bones);
            float dz[3] = { 0, 0, -need[i] };
            mmg_move_bone(m, pose, B->pelvis, dz, bones);
            moved = true;
        }
        for (int f = 0; f < 2; f++) {
            if (!moved && fabsf(D[i][f][0]) + fabsf(D[i][f][1]) + Lf[i][f] < 0.05f) continue;
            float T[3] = { P[i][f][0] + D[i][f][0], P[i][f][1] + D[i][f][1], P[i][f][2] + Lf[i][f] };
            two_bone_to(m, pose, g_mmg_id, bones, B->thigh[f], B->calf[f], B->foot[f], T, 1.0f);
        }
    }
    return worst;
}

// A made group's sliding, as the game measures it (anim.c): the lower foot's drift on the ground (within 3 cm of its
// lowest, the same foot as the frame before) over the body's travel, the body moving by its velocity
static float mmg_slide(const MmGen* G, const MmGroup* g) {
    const MmDb* d = G->d;
    AnimXf pose[ANIM_MAX_BONES];
    float prev[2][3], cur[2][3], x[2] = { 0, 0 }, px[2] = { 0, 0 }, low = 1e30f, slide = 0.0f, travel = 0.0f;
    int plo = -1;
    uint32_t nf = g->bake >= 0 ? g->bframes : (uint32_t)(g->len * MM_HZ);
    for (uint32_t i = 0; i < nf; i++) {
        mm_sample(d, G->m, g, (float)i / MM_HZ, pose);
        mm_feet(G->m, pose, G->feet, cur);
        low = fminf(low, fminf(cur[0][2], cur[1][2]));
    }
    for (uint32_t i = 0; i < nf; i++) {
        mm_sample(d, G->m, g, (float)i / MM_HZ, pose);
        mm_feet(G->m, pose, G->feet, cur);
        if (i && g->bake >= 0) x[0] += d->bake_vel[(g->bv + i) * 2] / MM_HZ, x[1] += d->bake_vel[(g->bv + i) * 2 + 1] / MM_HZ;
        else if (i) x[0] += g->dir[0] * g->speed / MM_HZ, x[1] += g->dir[1] * g->speed / MM_HZ; // (a clip: steady)
        int lo = cur[0][2] < cur[1][2] ? 0 : 1;
        float step = hypotf(x[0] - px[0], x[1] - px[1]);
        if (lo == plo && cur[lo][2] < low + 3.0f && prev[lo][2] < low + 3.0f && step > 50.0f / MM_HZ) {
            slide += hypotf(cur[lo][0] + x[0] - prev[lo][0] - px[0], cur[lo][1] + x[1] - prev[lo][1] - px[1]);
            travel += step;
        }
        memcpy(prev, cur, sizeof prev), px[0] = x[0], px[1] = x[1], plo = lo;
    }
    return travel > 1.0f ? slide / travel : 0.0f;
}

// One plan baked into its group (the bake jobs: each its own frames)
static void mmg_bake_job(void* arg, int li) {
    MmGen* G = arg;
    MmPlan* p = &G->plan[G->list[li]];
    MmDb* d = G->d;
    MmGroup* g = &d->groups[p->group];
    AnimXf* out = &d->bake[g->bake];
    float* vel = &d->bake_vel[(size_t)g->bv * 2], u_end = 0.0f;
    const MmSrc* S = &G->src[p->kind == ANIM_MMG_START ? p->b : p->a];
    if (p->kind == ANIM_MMG_START && p->u0 < 0.0f) { // the phase whose feet are nearest the idle's: the free foot steps first
        AnimXf pose[ANIM_MAX_BONES];
        float fi[2][3], fl[2][3], best = 1e30f;
        mmg_src_pose(G, &G->src[p->a], 0.0f, true, pose);
        mm_feet(G->m, pose, G->feet, fi);
        for (int k = 0; k < 32; k++) {
            mmg_src_pose(G, S, (float)k / 32.0f, false, pose);
            mm_feet(G->m, pose, G->feet, fl);
            float e = 0.0f;
            for (int f = 0; f < 2; f++) e += (fi[f][0] - fl[f][0]) * (fi[f][0] - fl[f][0]) + (fi[f][1] - fl[f][1]) * (fi[f][1] - fl[f][1]);
            if (e < best) best = e, p->u0 = (float)k / 32.0f;
        }
    }
    if (p->kind == ANIM_MMG_DIRECTION) {
        mmg_run(G, p, -1, 1.0f, g->bframes, out, vel, g, NULL);
        g->slide = mmg_slide(G, g);
        return;
    }
    if (p->kind == ANIM_MMG_STOP) { // into the idle (or its mirror) whose feet need the least correction; its length the
        // body's braking (the movement brakes so: the same distance, the planted feet in place)
        int16_t idles[2] = { p->b, -1 }, bi = p->b;
        if (!mmg_idles(G, S, idles)) idles[0] = p->b, idles[1] = -1;
        float bc = 1e30f;
        g->bframes = mmg_room(p->dur + 0.8f);
        for (int c = 0; c < 2; c++) {
            float cost = idles[c] >= 0 ? mmg_run(G, p, idles[c], p->dur, g->bframes, NULL, NULL, NULL, NULL) : 1e30f;
            if (cost < bc) bc = cost, bi = idles[c];
        }
        p->b = bi;
        mmg_run(G, p, bi, p->dur, g->bframes, out, vel, g, &u_end);
        const MmSrc* I = &G->src[bi];
        const MmGroup* ig = &d->groups[I->group];
        g->next = I->group, g->next_t = fmodf((float)(g->bframes - 1) / MM_HZ, ig->len > 1e-3f ? ig->len : 1.0f);
    } else {
        mmg_run(G, p, -1, p->dur, g->bframes, out, vel, g, &u_end);
        const MmSrc* N = &G->src[p->b];
        const MmGroup* ng = &d->groups[N->group];
        g->next = N->group, g->next_t = fmodf(N->phase0 + u_end * N->cycle, ng->len > 1e-3f ? ng->len : 1.0f);
    }
    g->len = (float)(g->bframes - 1) / MM_HZ;
    g->slide = mmg_slide(G, g);
}

// A made one-shot's velocity at t (its baked frames'; past its end, the loop it leads to)
static void mmg_velocity(const MmDb* d, const MmGroup* g, float t, float out[2]) {
    float f = fmaxf(t, 0.0f) * MM_HZ;
    if (f <= (float)(g->bframes - 1)) {
        uint32_t i0 = (uint32_t)f, i1 = i0 + 1 < g->bframes ? i0 + 1 : i0;
        float x = f - (float)i0;
        for (int k = 0; k < 2; k++) out[k] = d->bake_vel[(g->bv + i0) * 2 + k] + (d->bake_vel[(g->bv + i1) * 2 + k] - d->bake_vel[(g->bv + i0) * 2 + k]) * x;
        return;
    }
    const MmGroup* n = g->next >= 0 ? &d->groups[g->next] : g;
    out[0] = n->dir[0] * n->speed, out[1] = n->dir[1] * n->speed;
}

// --- The plan: what to make from the database's clips ---

static void mmg_generate(MmDb* d, const AnimGraph* graph, const SkelMesh* m, const int16_t feet[2], const float* params) {
    MmGen* G = &g_mmg;
    memset(G, 0, sizeof *G);
    G->d = d, G->m = m, G->feet[0] = feet[0], G->feet[1] = feet[1];
    memcpy(G->p, params, sizeof G->p);
    int off = (int)params[ANIM_MMGEN_OFF];
    if (!mmg_body(graph, m, feet, &G->B)) return;
    // the sources: each loop clip's group at 1x (as made and mirrored)
    uint32_t nbase = d->ngroups;
    for (uint32_t gi = 0; gi < nbase && G->nsrc < MMG_MAX_SRC; gi++) {
        const MmGroup* g = &d->groups[gi];
        if (!g->loop || g->rate != 1.0f || !d->clips[g->clip]) continue;
        MmSrc* s = &G->src[G->nsrc];
        s->group = (int16_t)gi, s->note = (int16_t)G->nsrc, s->speed = g->speed, s->idle = g->speed <= 0.0f;
        s->v[0] = g->dir[0] * g->speed, s->v[1] = g->dir[1] * g->speed;
        snprintf(s->name, sizeof s->name, "%s", g->label);
        mmg_cycle(G, s);
        if (!s->idle) d->groups[gi].slide = mmg_slide(G, g); // (the clips' own, to compare)
        G->nsrc++;
    }
    uint32_t nclip = G->nsrc;
    for (uint32_t i = 0; i < nclip; i++) {
        MmSrc* s = &G->src[i];
        s->canon = !s->idle;
        for (uint32_t j = 0; j < i && s->canon; j++)
            if (G->src[j].canon && mmg_similar(G, s, &G->src[j], 12.0f, 1.12f, 0.1f)) s->canon = false;
    }
    // the body's limits by speed (the forward locomotions; the movement follows them)
    for (uint32_t pass = 0; pass < 2 && !d->nlim; pass++)
        for (uint32_t i = 0; i < nclip; i++) {
            MmSrc* s = &G->src[i];
            if (!s->canon || (pass == 0 && fabsf(mmg_angle(s)) > 0.5f) || d->nlim >= 8) continue;
            uint32_t at = d->nlim;
            while (at > 0 && d->lim_speed[at - 1] > s->speed) at--;
            memmove(&d->lim_speed[at + 1], &d->lim_speed[at], sizeof(float) * (d->nlim - at));
            memmove(&d->lim_acc[at + 1], &d->lim_acc[at], sizeof(float) * (d->nlim - at));
            memmove(&d->lim_dec[at + 1], &d->lim_dec[at], sizeof(float) * (d->nlim - at));
            d->lim_speed[at] = s->speed;
            mmg_accel(G, s, &d->lim_acc[at], &d->lim_dec[at]);
            d->nlim++;
        }
    d->drive = !(off & ANIM_MMGEN_NO_MOVEMENT) && !(off & ANIM_MMGEN_NO_STARTS);
    float step = params[ANIM_MMGEN_STEP] * 0.0174533f;
    // the directions between two of about the same speed and posture, with none between them
    if (!(off & ANIM_MMGEN_NO_DIRECTIONS))
        for (uint32_t i = 0; i < nclip; i++)
            for (uint32_t j = i + 1; j < nclip; j++) {
                const MmSrc *A = &G->src[i], *Bs = &G->src[j];
                if (!A->canon || !Bs->canon || !mmg_similar(G, A, Bs, 181.0f, 2.5f, 0.1f)) continue; // (a strafe is often slower)
                float aa = mmg_angle(A), gap = mmg_wrap(mmg_angle(Bs) - aa);
                if (fabsf(gap) < step * 1.5f || fabsf(gap) > 2.62f) continue; // (1.5 steps .. 150 degrees)
                bool between = false;
                for (uint32_t c = 0; c < nclip && !between; c++) {
                    const MmSrc* C = &G->src[c];
                    if (c == i || c == j || !C->canon || !mmg_similar(G, C, A, 181.0f, 2.5f, 0.1f) || !mmg_similar(G, C, Bs, 181.0f, 2.5f, 0.1f)) continue;
                    float r = mmg_wrap(mmg_angle(C) - aa);
                    between = r * gap > 0.0f && fabsf(r) > 0.09f && fabsf(r) < fabsf(gap) - 0.09f;
                }
                if (between) continue;
                int n = (int)lroundf(fabsf(gap) / step) - 1;
                for (int s = 1; s <= n && G->nsrc < MMG_MAX_SRC; s++) {
                    float fr = (float)s / (float)(n + 1), th = aa + gap * fr, lo = 0.0f, hi = 1.0f, w = 0.5f;
                    for (int it = 0; it < 24; it++) { // (b's weight whose blend points at th)
                        w = (lo + hi) * 0.5f;
                        float vx = A->v[0] + (Bs->v[0] - A->v[0]) * w, vy = A->v[1] + (Bs->v[1] - A->v[1]) * w;
                        if (mmg_wrap(atan2f(vy, vx) - aa) / gap < fr) lo = w;
                        else hi = w;
                    }
                    float vb = hypotf(A->v[0] + (Bs->v[0] - A->v[0]) * w, A->v[1] + (Bs->v[1] - A->v[1]) * w);
                    float sp = A->speed + (Bs->speed - A->speed) * fr, k = sp / fmaxf(vb, 1.0f);
                    float cyc = (A->cycle + (Bs->cycle - A->cycle) * w) / sqrtf(k);
                    MmPlan* p = mmg_plan(G, ANIM_MMG_DIRECTION, (int)i, (int)j);
                    if (!p) break;
                    p->w = w, p->k = k, p->v[0] = cosf(th) * sp, p->v[1] = sinf(th) * sp;
                    p->frames = mmg_room(cyc) - 1;
                    if (p->frames < 8) p->frames = 8;
                    MmGroup* g = &d->groups[p->group];
                    g->loop = true, g->speed = sp, g->dir[0] = cosf(th), g->dir[1] = sinf(th), g->bframes = p->frames;
                    g->len = (float)p->frames / MM_HZ;
                    snprintf(g->label, sizeof g->label, "%s / %s %+.0f deg", A->name, Bs->name, th * 57.29578f);
                    MmSrc* ns = &G->src[G->nsrc++];
                    memset(ns, 0, sizeof *ns);
                    ns->group = (int16_t)p->group, ns->note = w < 0.5f ? A->note : Bs->note, ns->speed = sp;
                    ns->v[0] = p->v[0], ns->v[1] = p->v[1], ns->cycle = g->len, ns->phase0 = 0.0f;
                    ns->pelvis = A->pelvis + (Bs->pelvis - A->pelvis) * w, ns->canon = true;
                    for (int f = 0; f < 2; f++) ns->low[f] = A->low[f] + (Bs->low[f] - A->low[f]) * w;
                    snprintf(ns->name, sizeof ns->name, "%.30s %+.0f", A->name, th * 57.29578f);
                }
            }
    // starts and stops: each locomotion (clips and made directions) from and to the idle of its posture
    if (!(off & ANIM_MMGEN_NO_STARTS))
        for (uint32_t i = 0; i < G->nsrc; i++) {
            const MmSrc* S = &G->src[i];
            int16_t idles[2];
            if (!S->canon || S->idle || !mmg_idles(G, S, idles)) continue;
            float acc, dec;
            mmg_accel(G, S, &acc, &dec);
            float tstart = fminf(S->speed / acc, 3.5f), tstop = fminf(S->speed / dec, 3.0f);
            for (int c = 0; c < 2 && !mmg_covered(d, ANIM_MMG_START, S); c++) {
                if (idles[c] < 0) continue;
                MmPlan* p = mmg_plan(G, ANIM_MMG_START, idles[c], (int)i);
                if (!p) break;
                p->u0 = -1.0f, p->dur = tstart, p->frames = mmg_room(tstart + 0.3f);
                MmGroup* g = &d->groups[p->group];
                g->speed = S->speed, g->dir[0] = S->v[0] / S->speed, g->dir[1] = S->v[1] / S->speed, g->accel = acc, g->bframes = p->frames;
                snprintf(g->label, sizeof g->label, "%s > %s", G->src[idles[c]].name, S->name);
            }
            for (int q = 0; q < 8 && !mmg_covered(d, ANIM_MMG_STOP, S); q++) {
                MmPlan* p = mmg_plan(G, ANIM_MMG_STOP, (int)i, idles[0]);
                if (!p) break;
                p->u0 = (float)q * 0.125f, p->dur = tstop, p->frames = mmg_room(tstop + 0.8f);
                MmGroup* g = &d->groups[p->group];
                g->speed = S->speed, g->dir[0] = S->v[0] / S->speed, g->dir[1] = S->v[1] / S->speed, g->accel = -dec, g->bframes = p->frames;
                snprintf(g->label, sizeof g->label, "%s > %s (%d/8)", S->name, G->src[idles[0]].name, q + 1);
            }
        }
    // speed changes: neighbors in speed of the same direction and posture, both ways, from two phases
    if (!(off & ANIM_MMGEN_NO_SPEEDS))
        for (uint32_t i = 0; i < nclip; i++)
            for (uint32_t j = 0; j < nclip; j++) {
                const MmSrc *A = &G->src[i], *Bs = &G->src[j];
                if (i == j || !A->canon || !Bs->canon || !mmg_similar(G, A, Bs, 15.0f, 100.0f, 0.15f)) continue;
                float lo = fminf(A->speed, Bs->speed), hi = fmaxf(A->speed, Bs->speed);
                if (hi < lo * 1.25f) continue;
                bool between = false;
                for (uint32_t c = 0; c < nclip && !between; c++) {
                    const MmSrc* C = &G->src[c];
                    between = c != i && c != j && C->canon && mmg_similar(G, C, A, 15.0f, 100.0f, 0.15f) && C->speed > lo * 1.05f && C->speed < hi * 0.95f;
                }
                if (between) continue;
                float acc, dec, ign;
                mmg_accel(G, Bs, &acc, &ign), mmg_accel(G, A, &ign, &dec);
                bool up = Bs->speed > A->speed;
                float dur = fminf((hi - lo) / (up ? acc : dec), 3.0f);
                for (int q = 0; q < 2; q++) {
                    MmPlan* p = mmg_plan(G, ANIM_MMG_SPEED, (int)i, (int)j);
                    if (!p) break;
                    p->u0 = (float)q * 0.5f, p->dur = dur, p->frames = mmg_room(dur + 0.3f);
                    MmGroup* g = &d->groups[p->group];
                    g->speed = Bs->speed, g->dir[0] = Bs->v[0] / Bs->speed, g->dir[1] = Bs->v[1] / Bs->speed;
                    g->accel = up ? acc : -dec, g->bframes = p->frames;
                    snprintf(g->label, sizeof g->label, "%s > %s (%d/2)", A->name, Bs->name, q + 1);
                }
            }
    if (!G->nplan) return;
    // their frames: room for each, then baked on the workers (the directions first: the others blend them)
    size_t nxf = 0, nvf = 0;
    for (uint32_t i = 0; i < G->nplan; i++) {
        MmGroup* g = &d->groups[G->plan[i].group];
        g->bake = (int32_t)nxf, g->bv = (uint32_t)nvf;
        nxf += (size_t)g->bframes * m->nbones, nvf += g->bframes;
    }
    d->bake = malloc(sizeof(AnimXf) * (nxf ? nxf : 1));
    d->bake_vel = malloc(sizeof(float) * 2 * (nvf ? nvf : 1));
    if (!d->bake || !d->bake_vel) { // (no room: no made ones)
        d->ngroups -= G->nplan;
        memset(d->made, 0, sizeof d->made);
        return;
    }
    d->nbake = nxf;
    for (int pass = 0; pass < 2; pass++) {
        G->nlist = 0;
        for (uint32_t i = 0; i < G->nplan; i++)
            if ((G->plan[i].kind == ANIM_MMG_DIRECTION) == (pass == 0)) G->list[G->nlist++] = (uint16_t)i;
        if (G->nlist > 1) re_parallel_for((int)G->nlist, mmg_bake_job, G);
        else if (G->nlist) mmg_bake_job(G, 0);
    }
    // each made direction also at 0.85x and 1.18x (its frames shared)
    for (uint32_t i = 0; i < G->nplan; i++) {
        if (G->plan[i].kind != ANIM_MMG_DIRECTION) continue;
        for (int r = 0; r < 2; r++) {
            MmGroup* g = mm_add_group(d);
            if (!g) break;
            *g = d->groups[G->plan[i].group];
            g->rate = r ? 1.18f : 0.85f, g->speed *= g->rate;
            size_t l = strlen(g->label);
            snprintf(g->label + l, sizeof g->label - l, " x%.2f", g->rate);
        }
    }
}
