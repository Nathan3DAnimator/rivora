// Motion Matching (notas-dev/animacao.md), included by anim.c: a Motion Matching state's sequences become a database
// of frames (30 per second), each with its features: the trajectory (the body's velocity now and 0.3 s ahead, where it
// faces in 0.27 s and 0.5 s) and the pose (both feet's places and velocities), in the root's space. Every 0.1 s (at once
// when the query jumps) the instance looks for the frame nearest to what the pawn does (its Speed, its Direction, where
// it is asked to go and to face) and to its pose now; a better one starts there, blended in by inertialization (the
// jump between the poses eases out over 0.25 s on top of the new one alone: no two poses averaged). The database also
// holds animations made from the given ones: each moving loop time-warped (0.8x to 1.25x: other speeds in its own style)
// and every clip mirrored (left and right swapped: the other foot first, a turn the other way), and (anim_mm_gen.inc) the
// directions between two loops, the starts and stops from the idle and the speed changes, baked.
//   * any clip, a loop or one long take (start, walk, run, stop, turn, idle in one file): every frame is a candidate (but
//     a take's last 0.3 s, which have no future); its root motion (the root bone's path and turn, else the pelvis's
//     smoothed, else in place the planted feet's and the pelvis's turn) gives each frame its own trajectory, and is taken
//     out of the pose: the actor carries it. A clip's turn turns the actor (a turn in place, a pivot)
//   * a jump in a clip (its root's height, or a root still: the pelvis's arc while both feet are off the ground) is
//     vertical root motion: taken out of the pose, a feature (the vertical velocity now and in 0.3 s: no jump unasked),
//     and the capsule's: Jump asks for one, the clip carries the Character up and down (mm_vertical) until it lands
//   * a loop's play rate fits the real speed; a one-shot's time follows the distance the body went (Unreal's distance
//     matching), so the planted foot does not slide; the made one-shots give way to the user's own (a cost bias)
//   * mirroring needs a symmetric skeleton (bones in _l / _r, Left / Right pairs, binds mirrored): checked, else off
//   * the search skips a group whose features' box is farther than the best so far (most of them)

#define MM_HZ 30.0f
#define MM_FEAT ANIM_MM_FEAT
#define MM_MAX_DB 16
#define MM_MAX_GROUPS 2048
#define MM_BLEND 0.25f
#define MM_SEARCH 0.1f
// a clip's root motion: none (a loop in place), its root bone's, its pelvis's (the root still), in place (the feet's)
enum { MM_TRK_NONE = 0, MM_TRK_ROOT, MM_TRK_PELVIS, MM_TRK_FEET };

typedef struct MmGroup {
    uint8_t clip, mirror, kind; // kind: ANIM_MMG_*; clip: its clip (a made one's: the main source's)
    bool loop;
    uint8_t trk_mode;               // MM_TRK_*: its root motion (taken out of its poses; the frames' trajectories)
    uint8_t vert;                   // its track has a height (MmDb.tz: a jump; taken out of its poses, the capsule's)
    float rate, speed, dir[2], len; // the play rate, its speed (cm/s) and direction (root space), its length (s)
    uint32_t first, count, nsearch; // its entries (the first nsearch searched: a take's end has no future)
    int32_t trk;                    // its root track's first frame in MmDb.trk (x, y cm, yaw rad each 1/30 s), tframes
    uint32_t tframes;
    int32_t bake;                   // a made one: its first transform in MmDb.bake (bframes x the bones); -1: its clip
    uint32_t bframes, bv;           // its baked frames; their velocities' first (MmDb.bake_vel, 2 floats each)
    int32_t next;                   // a one-shot: the loop it ends in, at next_t
    float next_t, accel, slide;     // (accel: its mean, cm/s2; slide: a made one's feet sliding over its travel)
    float bias;                     // added to its frames' costs (a made start / stop when the user gave their own)
    uint8_t style;                  // (Q27.11) ANIM_STYLE_ by its clip's name (Injured, Tired, Drunk, Carry; else Normal)
    uint8_t nev;                    // a made one's notifies: a source clip's (ev_clip) notify ev_idx at ev_t
    uint8_t ev_clip[8], ev_idx[8];
    float ev_t[8];
    char label[96];
} MmGroup;

typedef struct MmDb {
    char name[ANIM_NAME]; // (the state's: the log)
    const SkelMesh* m;
    uint32_t sig, rev;
    AnimSeq* clips[ANIM_BLEND_SAMPLES];
    float clip_speed[ANIM_BLEND_SAMPLES];
    uint8_t styles; // (Q27.11) the ANIM_STYLE_ bits its groups have
    int16_t mirror_of[ANIM_MAX_BONES];
    bool can_mirror, drive, authored; // authored: the user gave one-shots of their own (starts, stops, turns, a take)
    MmGroup* groups;
    uint32_t ngroups, n;
    float* f;         // n x MM_FEAT, scaled (the weights folded in)
    float* vel;       // n x 2: the body's velocity at each frame (cm/s, root space)
    float* dist;      // n: a group's travel from its start to each frame (cm)
    float* box;       // ngroups x 2 x MM_FEAT: each group's features' bounds
    uint16_t* group;  // n: its group
    AnimXf* bake;     // the made animations' frames
    size_t nbake;
    float* bake_vel;  // the made frames' velocities (cm/s, root space)
    float* trk;       // the clips' root tracks (x, y cm, yaw rad per frame)
    float* tz;        // their heights (cm per frame, from the first: a jump's vertical root motion)
    float jump_fut;   // a jump's takeoff: the highest vertical velocity in 0.3 s from a frame on the ground (cm/s; 0: none)
    // the user's own starts and stops (one-shots from rest into a speed, or back): the made ones they cover are not made
    uint32_t ncover;
    struct { uint8_t kind; float speed, dir[2]; } cover[32];
    uint32_t ntrk;
    // the body's limits by speed (forward ones): the Character's movement follows them
    uint32_t nlim;
    float lim_speed[8], lim_acc[8], lim_dec[8];
    uint32_t made[ANIM_MMG_KINDS];
    double build_ms, gen_ms;
} MmDb;

static MmDb g_mm[MM_MAX_DB];
static uint32_t g_nmm, g_mm_next;
static struct { uint32_t searches, switches, entries, starts, stops, authored, jumps, jump_ends[4]; double ms, build_ms, turn_s, jump_s; float jump_rise; } g_mm_stats;

static const float g_mm_rates[] = { 0.8f, 0.9f, 1.0f, 1.12f, 1.25f };
// the features' scales: velocities in m/s (weighted 2), the facing's turn (a unit vector, weighted 1.2), the vertical
// velocities in m/s (weighted 2: a jump only when asked), feet places per 15 cm, feet velocities per 1.5 m/s
#define MM_F_VZ 8   // the vertical velocity now, in 0.3 s
#define MM_F_POSE 10 // the pose's first (the feet)
#define MM_F_TRAJ 10 // the trajectory's (the first search's)
static const float g_mm_scale[MM_FEAT] = { 0.02f, 0.02f, 0.02f, 0.02f, 1.2f, 1.2f, 1.2f, 1.2f, 0.02f, 0.02f, 1.0f / 15, 1.0f / 15, 1.0f / 15,
                                           1.0f / 15, 1.0f / 15, 1.0f / 15, 1.0f / 150, 1.0f / 150, 1.0f / 150, 1.0f / 150 };

// A bone's mirror by its name (_l / _r, .L / .R, Left / Right, l_ / r_); itself when none
static int16_t mm_pair(const SkelMesh* m, uint32_t b) {
    const char* n = m->bones[b].name;
    size_t l = strlen(n);
    char want[ANIM_NAME];
    static const char* const suf[][2] = { { "_l", "_r" }, { "_L", "_R" }, { ".l", ".r" }, { ".L", ".R" } };
    for (int k = 0; k < 4; k++)
        for (int d = 0; d < 2; d++)
            if (l > 2 && !strcmp(n + l - 2, suf[k][d])) {
                snprintf(want, sizeof want, "%.*s%s", (int)(l - 2), n, suf[k][!d]);
                int16_t p = bone_named(m, want);
                if (p >= 0) return p;
            }
    static const char* const mid[][2] = { { "Left", "Right" }, { "left", "right" } };
    for (int k = 0; k < 2; k++)
        for (int d = 0; d < 2; d++) {
            const char* at = strstr(n, mid[k][d]);
            if (!at) continue;
            snprintf(want, sizeof want, "%.*s%s%s", (int)(at - n), n, mid[k][!d], at + strlen(mid[k][d]));
            int16_t p = bone_named(m, want);
            if (p >= 0) return p;
        }
    if (l > 2 && (n[0] == 'l' || n[0] == 'r') && n[1] == '_') {
        snprintf(want, sizeof want, "%c%s", n[0] == 'l' ? 'r' : 'l', n + 1);
        int16_t p = bone_named(m, want);
        if (p >= 0) return p;
    }
    return (int16_t)b;
}

// A pose's mirror across the body's middle (y -> -y): each bone takes its pair's transform, mirrored
static void mm_mirror_xf(const AnimXf* s, AnimXf* o) {
    *o = *s;
    o->t[1] = -s->t[1];
    o->q[0] = -s->q[0], o->q[2] = -s->q[2];
}

static bool mm_symmetric(const SkelMesh* m, const int16_t* pair) {
    for (uint32_t b = 0; b < m->nbones; b++) {
        AnimXf x;
        mm_mirror_xf(&m->bones[b].bind, &x);
        const AnimXf* y = &m->bones[pair[b]].bind;
        for (int k = 0; k < 3; k++)
            if (fabsf(x.t[k] - y->t[k]) > 0.5f) return false;
        float dot = x.q[0] * y->q[0] + x.q[1] * y->q[1] + x.q[2] * y->q[2] + x.q[3] * y->q[3];
        if (fabsf(dot) < 0.999f) return false;
    }
    return true;
}

// A group's root track at its clip's time t (x, y cm, yaw rad: a loop's wraps, a take's held at its ends)
static void mm_trk_at(const MmDb* d, const MmGroup* g, float t, float o[3]) {
    const float* T = &d->trk[(size_t)g->trk * 3];
    float f = t * MM_HZ, span = (float)(g->tframes - 1);
    if (g->loop && span > 0.0f) {
        f = fmodf(f, span);
        if (f < 0.0f) f += span;
    } else
        f = fminf(fmaxf(f, 0.0f), span);
    uint32_t i0 = (uint32_t)f, i1 = i0 + 1 < g->tframes ? i0 + 1 : i0;
    float a = f - (float)i0;
    for (int k = 0; k < 3; k++) o[k] = T[i0 * 3 + k] + (T[i1 * 3 + k] - T[i0 * 3 + k]) * a;
}

// A group's height at its clip's time t (cm from its first frame: a jump's; held at a take's ends; 0 without one)
static float mm_trk_z(const MmDb* d, const MmGroup* g, float t) {
    if (!g->vert) return 0.0f;
    const float* Z = &d->tz[g->trk];
    float f = fminf(fmaxf(t * MM_HZ, 0.0f), (float)(g->tframes - 1));
    uint32_t i0 = (uint32_t)f, i1 = i0 + 1 < g->tframes ? i0 + 1 : i0;
    return Z[i0] + (Z[i1] - Z[i0]) * (f - (float)i0);
}
static float mm_trk_vz(const MmDb* d, const MmGroup* g, float t) { // (cm/s of its clip's time)
    if (!g->vert) return 0.0f;
    const float h = 1.0f / MM_HZ;
    float a = fmaxf(t - h, 0.0f), b = t + h;
    return (mm_trk_z(d, g, b) - mm_trk_z(d, g, a)) / (b - a);
}

// The root motion out of a pose: the body brought back to the actor (its path, or in place only its turn; its height,
// a jump's): the actor carries them (its movement, its turn, the capsule up and down)
static void mm_unroot(const MmDb* d, const MmGroup* g, float t, AnimXf* out) {
    float tr[3];
    mm_trk_at(d, g, t, tr);
    out[0].t[2] -= mm_trk_z(d, g, t);
    float c = cosf(-tr[2]), s = sinf(-tr[2]), x = out[0].t[0], y = out[0].t[1], q[4];
    if (g->trk_mode != MM_TRK_FEET) x -= tr[0], y -= tr[1];
    out[0].t[0] = c * x - s * y, out[0].t[1] = s * x + c * y;
    const float qz[4] = { 0.0f, 0.0f, sinf(-tr[2] * 0.5f), cosf(-tr[2] * 0.5f) };
    q_mul(qz, out[0].q, q);
    memcpy(out[0].q, q, 16);
}

// A group's pose at t (its clip's time, its root motion taken out; a made one's baked frames, looped or held at the ends)
static void mm_sample(const MmDb* d, const SkelMesh* m, const MmGroup* g, float t, AnimXf* out) {
    if (g->bake >= 0) {
        const AnimXf* b = &d->bake[g->bake];
        uint32_t nf = g->bframes, nb = m->nbones, i0, i1;
        float f = t * MM_HZ;
        if (g->loop) {
            f = fmodf(f, (float)nf);
            if (f < 0.0f) f += (float)nf;
            i0 = (uint32_t)f % nf, i1 = (i0 + 1) % nf;
        } else {
            f = fminf(fmaxf(f, 0.0f), (float)(nf - 1));
            i0 = (uint32_t)f, i1 = i0 + 1 < nf ? i0 + 1 : i0;
        }
        anim_blend(&b[(size_t)i0 * nb], &b[(size_t)i1 * nb], f - (float)i0, nb, out);
        return;
    }
    const AnimSeq* q = d->clips[g->clip];
    if (!g->mirror) {
        sample_mapped(q, m, q->map, t, out);
    } else {
        AnimXf tmp[ANIM_MAX_BONES];
        sample_mapped(q, m, q->map, t, tmp);
        for (uint32_t b = 0; b < m->nbones; b++) mm_mirror_xf(&tmp[d->mirror_of[b]], &out[b]);
    }
    if (g->trk_mode != MM_TRK_NONE) mm_unroot(d, g, t, out);
}

static void mm_feet_bones(const AnimGraph* g, const SkelMesh* m, int16_t feet[2]) {
    feet[0] = bone_named(m, g ? g->foot_l : "foot_l"), feet[1] = bone_named(m, g ? g->foot_r : "foot_r"); // (2026-11-23: the roles' feet on any rig)
    for (uint32_t b = 0; b < m->nbones && (feet[0] < 0 || feet[1] < 0); b++) { // (no Foot IK bones: by name)
        const char* n = m->bones[b].name;
        if (!strstr(n, "foot") && !strstr(n, "Foot")) continue;
        int16_t p = mm_pair(m, b);
        if (p == (int16_t)b) continue;
        bool left = strstr(n, "_l") || strstr(n, "Left") || strstr(n, ".L") || strstr(n, "_L") || n[0] == 'l';
        feet[left ? 0 : 1] = (int16_t)b, feet[left ? 1 : 0] = p;
    }
}

static void mm_feet(const SkelMesh* m, const AnimXf* pose, const int16_t feet[2], float out[2][3]) {
    static _Thread_local float bones[ANIM_MAX_BONES][3][4];
    const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    anim_world(m, pose, id, bones);
    for (int f = 0; f < 2; f++)
        for (int k = 0; k < 3; k++) out[f][k] = bones[feet[f]][k][3];
}

// A clip's speed and direction: its root's motion over the cycle, else the planted foot's: a foot within 2.5 cm of
// the lowest any foot gets in the clip is on the ground, and moves back under the body as fast as the body goes
static float mm_clip_speed(const SkelMesh* m, AnimSeq* q, const int16_t feet[2], float dir[2]) {
    dir[0] = 1.0f, dir[1] = 0.0f;
    uint32_t nf = (uint32_t)(q->length * MM_HZ);
    if (nf < 2) return 0.0f;
    static AnimXf pose[ANIM_MAX_BONES];
    static float p[256][2][3];
    if (nf > 255) nf = 255;
    float lowest = 1e30f, sum[2] = { 0, 0 };
    uint32_t cnt = 0;
    for (uint32_t f = 0; f <= nf; f++) {
        sample_mapped(q, m, q->map, (float)f / MM_HZ, pose);
        mm_feet(m, pose, feet, p[f]);
        lowest = fminf(lowest, fminf(p[f][0][2], p[f][1][2]));
    }
    float r0[3];
    sample_mapped(q, m, q->map, 0.0f, pose);
    memcpy(r0, pose[0].t, 12);
    sample_mapped(q, m, q->map, q->length, pose);
    for (uint32_t f = 0; f < nf; f++)
        for (int c = 0; c < 2; c++)
            if (p[f][c][2] < lowest + 2.5f && p[f + 1][c][2] < lowest + 2.5f) { // (on the ground at both frames)
                sum[0] += (p[f + 1][c][0] - p[f][c][0]) * MM_HZ, sum[1] += (p[f + 1][c][1] - p[f][c][1]) * MM_HZ;
                cnt++;
            }
    float root[2] = { (pose[0].t[0] - r0[0]) / q->length, (pose[0].t[1] - r0[1]) / q->length };
    float rs = sqrtf(root[0] * root[0] + root[1] * root[1]);
    if (rs > 20.0f && !q->loop) { // (root motion)
        dir[0] = root[0] / rs, dir[1] = root[1] / rs;
        return rs;
    }
    if (!cnt) return 0.0f;
    float v[2] = { -sum[0] / (float)cnt, -sum[1] / (float)cnt }, s = sqrtf(v[0] * v[0] + v[1] * v[1]);
    if (s < 25.0f) return 0.0f; // (standing: the feet only shift)
    dir[0] = v[0] / s, dir[1] = v[1] / s;
    return s;
}

// A bone's turn since a first rotation (radians: its first x axis now, seen from above)
static float mm_rel_yaw(const float now[3][4], const float first[3][4]) {
    float A[3][3], B[3][3];
    rows_rot(now, A), rows_rot(first, B);
    float w[3] = { B[0][0], B[0][1], B[0][2] }, v[2] = { 0, 0 }; // (the first one's transpose times x: its first row)
    for (int k = 0; k < 3; k++) v[0] += A[0][k] * w[k], v[1] += A[1][k] * w[k];
    return atan2f(v[1], v[0]);
}

static float mm_wrap_pi(float a) { // (radians to -pi..pi)
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// A box filter over n values (stride s, a loop's wraps, a take's ends held) of radius r
static void mm_smooth(float* x, uint32_t n, int s, int r, bool loop) {
    float* tmp = malloc(sizeof(float) * n);
    if (!tmp) return;
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (int o = -r; o <= r; o++) {
            int32_t j = (int32_t)i + o;
            j = loop ? ((j % (int32_t)(n - 1)) + (int32_t)(n - 1)) % (int32_t)(n - 1) : (j < 0 ? 0 : j >= (int32_t)n ? (int32_t)n - 1 : j);
            sum += x[(size_t)j * s];
        }
        tmp[i] = sum / (float)(2 * r + 1);
    }
    for (uint32_t i = 0; i < n; i++) x[(size_t)i * s] = tmp[i];
    free(tmp);
}

// A clip's jumps with its root still (a mocap take): each time both feet are off the ground (4 cm over their lowest)
// for 4 frames or more, between two contacts, the pelvis's rise over the line from its height at the takeoff to its
// height at the landing - the body's flight (a run's float, under 10 cm, stays in the pose); Z: the heights (cm). Found
static bool mm_air_arcs(const float* P, const float* F, const float low[2], uint32_t nf, float* Z) {
    bool found = false;
    for (uint32_t i = 0; i < nf;) {
        if (F[i * 6 + 2] <= low[0] + 4.0f || F[i * 6 + 5] <= low[1] + 4.0f) {
            i++;
            continue;
        }
        uint32_t a = i;
        while (i < nf && F[i * 6 + 2] > low[0] + 4.0f && F[i * 6 + 5] > low[1] + 4.0f) i++;
        if (i - a < 4 || a == 0 || i >= nf) continue; // (a take begun or ended in the air: left in the pose)
        uint32_t c0 = a - 1, c1 = i;
        float p0 = P[c0 * 3 + 2], p1 = P[c1 * 3 + 2], peak = 0.0f;
        for (uint32_t j = c0; j <= c1; j++) peak = fmaxf(peak, P[j * 3 + 2] - (p0 + (p1 - p0) * (float)(j - c0) / (float)(c1 - c0)));
        if (peak < 10.0f) continue;
        for (uint32_t j = c0; j <= c1; j++) Z[j] = fmaxf(P[j * 3 + 2] - (p0 + (p1 - p0) * (float)(j - c0) / (float)(c1 - c0)), 0.0f);
        found = true;
    }
    return found;
}

// A clip's root motion (as made or mirrored), into d->trk (x, y cm, yaw rad per 1/30 s from its first frame): the root
// bone's own path and turn; else a clip carried by its pelvis (the root still), the pelvis's path and turn smoothed;
// else a one-shot in place: its velocity from its planted feet (a foot within 2.5 cm of its lowest moves back under the
// body), its turn the pelvis's smoothed (a turn in place). A loop in place has none (its velocity: the clip's). Its
// height into d->tz (vert): a root on the floor, its own (it rises over 8 cm); else a one-shot's flights (mm_air_arcs).
// Its mode; speed and dir: its mean velocity (in its first facing)
static int mm_track_build(MmDb* d, const SkelMesh* m, const int16_t feet[2], uint8_t clip, uint8_t mirror, bool loop, float len,
                          int32_t* first, uint32_t* nframes, float* speed, float dir[2], bool* vert) {
    *vert = false;
    uint32_t nf = (uint32_t)lroundf(len * MM_HZ) + 1;
    int16_t calf = m->bones[feet[0]].parent, thigh = calf >= 0 ? m->bones[calf].parent : -1, pelvis = thigh >= 0 ? m->bones[thigh].parent : -1;
    if (nf < 4 || nf > 18000 || pelvis < 0) return MM_TRK_NONE;
    MmGroup tg = { 0 };
    tg.clip = clip, tg.mirror = mirror, tg.bake = -1, tg.loop = loop, tg.len = len;
    float* R = malloc(sizeof(float) * 9 * nf); // the root's place, the pelvis's, the yaws (root, pelvis)
    float* F = malloc(sizeof(float) * 6 * nf); // the feet
    float* T = malloc(sizeof(float) * 3 * nf);
    float* Z = calloc(nf, sizeof(float)); // the height (a jump)
    if (!R || !F || !T || !Z) return free(R), free(F), free(T), free(Z), MM_TRK_NONE;
    float *P = R + 3 * nf, *Y = R + 6 * nf;
    static _Thread_local float bones[ANIM_MAX_BONES][3][4], first_rows[2][3][4];
    AnimXf pose[ANIM_MAX_BONES];
    const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    float low[2] = { 1e30f, 1e30f };
    for (uint32_t i = 0; i < nf; i++) {
        mm_sample(d, m, &tg, (float)i / MM_HZ, pose);
        anim_world(m, pose, id, bones);
        if (!i) memcpy(first_rows[0], bones[0], sizeof first_rows[0]), memcpy(first_rows[1], bones[pelvis], sizeof first_rows[1]);
        for (int k = 0; k < 3; k++) R[i * 3 + k] = bones[0][k][3], P[i * 3 + k] = bones[pelvis][k][3];
        Y[i * 3] = mm_rel_yaw(bones[0], first_rows[0]), Y[i * 3 + 1] = mm_rel_yaw(bones[pelvis], first_rows[1]);
        for (int f = 0; f < 2; f++) {
            for (int k = 0; k < 3; k++) F[i * 6 + f * 3 + k] = bones[feet[f]][k][3];
            low[f] = fminf(low[f], F[i * 6 + f * 3 + 2]);
        }
    }
    for (uint32_t i = 1; i < nf; i++) // (the turns unwrapped)
        for (int k = 0; k < 2; k++) Y[i * 3 + k] = Y[(i - 1) * 3 + k] + mm_wrap_pi(Y[i * 3 + k] - Y[(i - 1) * 3 + k]);
    float rmax = 0.0f, ymax = 0.0f;
    for (uint32_t i = 0; i < nf; i++)
        rmax = fmaxf(rmax, hypotf(R[i * 3] - R[0], R[i * 3 + 1] - R[1])), ymax = fmaxf(ymax, fabsf(Y[i * 3]));
    float pnet = hypotf(P[(nf - 1) * 3] - P[0], P[(nf - 1) * 3 + 1] - P[1]);
    int mode = MM_TRK_NONE;
    bool feet_track = false; // (the in-place track made from the feet)
    if (rmax > 5.0f || ymax > 0.17f) { // (the root moves)
        mode = MM_TRK_ROOT;
        for (uint32_t i = 0; i < nf; i++) T[i * 3] = R[i * 3] - R[0], T[i * 3 + 1] = R[i * 3 + 1] - R[1], T[i * 3 + 2] = Y[i * 3];
    } else if (pnet > 30.0f) { // (carried by the pelvis: its path and turn, smoothed of the steps' sway)
        mode = MM_TRK_PELVIS;
        for (uint32_t i = 0; i < nf; i++) T[i * 3] = P[i * 3] - P[0], T[i * 3 + 1] = P[i * 3 + 1] - P[1], T[i * 3 + 2] = Y[i * 3 + 1];
        for (int k = 0; k < 3; k++) mm_smooth(T + k, nf, 3, 7, false);
    } else if (!loop) { // (in place: the turn from the pelvis, the velocity from the planted feet in the turned frame)
        float* V = malloc(sizeof(float) * 2 * nf);
        bool* ok = malloc(sizeof(bool) * nf);
        if (V && ok) {
            for (uint32_t i = 0; i < nf; i++) T[i * 3 + 2] = Y[i * 3 + 1];
            mm_smooth(T + 2, nf, 3, 7, false);
            float ty = 0.0f, vmax = 0.0f, y0 = T[2];
            for (uint32_t i = 0; i < nf; i++) ty = fmaxf(ty, fabsf(T[i * 3 + 2] - y0));
            for (uint32_t i = 0; i < nf; i++) T[i * 3 + 2] = ty > 0.17f ? T[i * 3 + 2] - y0 : 0.0f;
            for (uint32_t i = 0; i + 1 < nf; i++) {
                float sum[2] = { 0, 0 }, c0 = cosf(-T[i * 3 + 2]), s0 = sinf(-T[i * 3 + 2]), c1 = cosf(-T[(i + 1) * 3 + 2]), s1 = sinf(-T[(i + 1) * 3 + 2]);
                int cnt = 0;
                for (int f = 0; f < 2; f++) {
                    const float *a = &F[i * 6 + f * 3], *b = &F[(i + 1) * 6 + f * 3];
                    if (a[2] > low[f] + 2.5f || b[2] > low[f] + 2.5f) continue;
                    float ax = c0 * a[0] - s0 * a[1], ay = s0 * a[0] + c0 * a[1], bx = c1 * b[0] - s1 * b[1], by = s1 * b[0] + c1 * b[1];
                    sum[0] -= (bx - ax) * MM_HZ, sum[1] -= (by - ay) * MM_HZ, cnt++;
                }
                ok[i] = cnt > 0;
                if (cnt) V[i * 2] = sum[0] / (float)cnt, V[i * 2 + 1] = sum[1] / (float)cnt;
            }
            ok[nf - 1] = false;
            for (uint32_t i = 0; i < nf; i++) { // (in the air: between its neighbors on the ground)
                if (ok[i]) continue;
                int32_t a = (int32_t)i - 1, b = (int32_t)i + 1;
                while (a >= 0 && !ok[a]) a--;
                while (b < (int32_t)nf && !ok[b]) b++;
                for (int k = 0; k < 2; k++) {
                    float va = a >= 0 ? V[a * 2 + k] : (b < (int32_t)nf ? V[b * 2 + k] : 0.0f), vb = b < (int32_t)nf ? V[b * 2 + k] : va;
                    V[i * 2 + k] = a >= 0 && b < (int32_t)nf ? va + (vb - va) * (float)((int32_t)i - a) / (float)(b - a) : va;
                }
            }
            for (int k = 0; k < 2; k++) mm_smooth(V + k, nf, 2, 4, false);
            T[0] = T[1] = 0.0f;
            for (uint32_t i = 0; i + 1 < nf; i++) {
                float c = cosf(T[i * 3 + 2]), s = sinf(T[i * 3 + 2]);
                T[(i + 1) * 3] = T[i * 3] + (c * V[i * 2] - s * V[i * 2 + 1]) / MM_HZ;
                T[(i + 1) * 3 + 1] = T[i * 3 + 1] + (s * V[i * 2] + c * V[i * 2 + 1]) / MM_HZ;
                vmax = fmaxf(vmax, hypotf(V[i * 2], V[i * 2 + 1]));
            }
            if (vmax > 10.0f || ty > 0.17f) mode = MM_TRK_FEET;
            feet_track = true;
        }
        free(V), free(ok);
    }
    // the height: a root on the floor (by the lowest foot, as Unreal's) that rises carries it; else (a root at the hips,
    // still, or on the floor without the jump) the flights
    float zlo = 1e30f, zhi = -1e30f, flo = fminf(low[0], low[1]);
    for (uint32_t i = 0; i < nf; i++) zlo = fminf(zlo, R[i * 3 + 2]), zhi = fmaxf(zhi, R[i * 3 + 2]);
    if (mode == MM_TRK_ROOT && fabsf(R[2] - flo) < 25.0f && zhi - zlo > 8.0f) {
        for (uint32_t i = 0; i < nf; i++) Z[i] = R[i * 3 + 2] - R[2];
        *vert = true;
    } else if (!loop && (mode != MM_TRK_NONE || feet_track) && mm_air_arcs(P, F, low, nf, Z)) {
        *vert = true;
        if (mode == MM_TRK_NONE) mode = MM_TRK_FEET; // (a jump in place: its track is the feet's)
    }
    if (mode != MM_TRK_NONE) {
        float* nt = realloc(d->trk, sizeof(float) * 3 * (d->ntrk + nf));
        float* nz = nt ? realloc(d->tz, sizeof(float) * (d->ntrk + nf)) : NULL;
        if (nt) d->trk = nt;
        if (nz) d->tz = nz;
        if (!nt || !nz) mode = MM_TRK_NONE, *vert = false;
        else {
            memcpy(&d->trk[(size_t)d->ntrk * 3], T, sizeof(float) * 3 * nf);
            memcpy(&d->tz[d->ntrk], Z, sizeof(float) * nf);
            *first = (int32_t)d->ntrk, *nframes = nf, d->ntrk += nf;
            float dx = T[(nf - 1) * 3] / len, dy = T[(nf - 1) * 3 + 1] / len;
            *speed = hypotf(dx, dy);
            if (*speed > 1.0f) dir[0] = dx / *speed, dir[1] = dy / *speed;
            if (*speed < 25.0f) *speed = 0.0f;
        }
    }
    free(R), free(F), free(T), free(Z);
    if (mode == MM_TRK_NONE) *vert = false;
    return mode;
}

void anim_mm_gen_defaults(float g[ANIM_MMGEN]) {
    memset(g, 0, sizeof(float) * ANIM_MMGEN);
    g[ANIM_MMGEN_WEIGHT] = 1.0f, g[ANIM_MMGEN_LEAN] = 1.0f, g[ANIM_MMGEN_SETTLE] = 0.4f, g[ANIM_MMGEN_STEP] = 22.5f;
    g[ANIM_MMGEN_SET] = 1.0f;
}

// A state's settings, the defaults where unset, in their ranges
static void mm_gen_params(const AnimState* s, float p[ANIM_MMGEN]) {
    if (s->mmgen[ANIM_MMGEN_SET] != 1.0f) anim_mm_gen_defaults(p);
    else memcpy(p, s->mmgen, sizeof(float) * ANIM_MMGEN);
    p[ANIM_MMGEN_ACCEL] = fmaxf(p[ANIM_MMGEN_ACCEL], 0.0f), p[ANIM_MMGEN_DECEL] = fmaxf(p[ANIM_MMGEN_DECEL], 0.0f);
    p[ANIM_MMGEN_WEIGHT] = fminf(fmaxf(p[ANIM_MMGEN_WEIGHT], 0.25f), 4.0f);
    p[ANIM_MMGEN_LEAN] = fminf(fmaxf(p[ANIM_MMGEN_LEAN], 0.0f), 3.0f);
    p[ANIM_MMGEN_SETTLE] = fminf(fmaxf(p[ANIM_MMGEN_SETTLE], 0.0f), 1.0f);
    p[ANIM_MMGEN_STEP] = fminf(fmaxf(p[ANIM_MMGEN_STEP], 10.0f), 45.0f);
}

static uint32_t mm_sig(const AnimState* s) {
    uint32_t h = 2166136261u ^ s->nsamples;
    for (uint32_t k = 0; k < s->nsamples; k++)
        for (const char* c = s->samples[k]; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    float p[ANIM_MMGEN];
    mm_gen_params(s, p);
    const uint8_t* b = (const uint8_t*)p;
    for (size_t k = 0; k < sizeof p; k++) h = (h ^ b[k]) * 16777619u;
    return h;
}

static MmGroup* mm_add_group(MmDb* d) {
    if (d->ngroups >= MM_MAX_GROUPS) return NULL;
    MmGroup* g = &d->groups[d->ngroups++];
    memset(g, 0, sizeof *g);
    g->rate = 1.0f, g->bake = -1, g->next = -1, g->dir[0] = 1.0f;
    return g;
}

#include "anim_mm_gen.inc"

// A group's root track at its clip's time t, a loop's run on past its end into its next cycle (turned and moved by the
// cycle's own): the trajectory ahead
static void mm_trk_ahead(const MmDb* d, const MmGroup* g, float t, float o[3]) {
    float len = (float)(g->tframes - 1) / MM_HZ;
    t = fmaxf(t, 0.0f);
    if (!g->loop || t < len) {
        mm_trk_at(d, g, fminf(t, len), o);
        return;
    }
    const float* L = &d->trk[(size_t)(g->trk + (int32_t)g->tframes - 1) * 3];
    float r[3], c = cosf(L[2]), s = sinf(L[2]);
    mm_trk_at(d, g, fminf(t - len, len), r);
    o[0] = L[0] + c * r[0] - s * r[1], o[1] = L[1] + s * r[0] + c * r[1], o[2] = L[2] + r[2];
}

// a track's velocity at t (cm/s of its clip's time, the clip's frame: as moving the root)
static void mm_trk_vel(const MmDb* d, const MmGroup* g, float t, float v[2]) {
    const float h = 1.0f / MM_HZ;
    float a = fmaxf(t - h, 0.0f), b = t + h, pa[3], pb[3];
    mm_trk_ahead(d, g, a, pa), mm_trk_ahead(d, g, b, pb);
    v[0] = (pb[0] - pa[0]) / (b - a), v[1] = (pb[1] - pa[1]) / (b - a);
}

// A group's features at its entry i (a frame at i / 30 s of its clip's time): the trajectory (the velocity now and in
// 0.3 s, the facing's turn in 0.27 s and 0.5 s: a clip's from its root motion, in its frame's facing; a loop in place's
// steady; a made one's own) and the feet (places, velocities), scaled; the velocity (d->vel), the travel (d->dist)
typedef struct MmFeatJob { MmDb* d; const SkelMesh* m; int16_t feet[2]; } MmFeatJob;
static void mm_features_job(void* arg, int gi) {
    MmFeatJob* J = arg;
    MmDb* d = J->d;
    const MmGroup* g = &d->groups[gi];
    AnimXf pose[ANIM_MAX_BONES];
    float a[2][3], b[2][3];
    for (uint32_t i = 0; i < g->count; i++) {
        float t = (float)i / MM_HZ, *f = &d->f[(size_t)(g->first + i) * MM_FEAT], *v = &d->vel[(size_t)(g->first + i) * 2];
        mm_sample(d, J->m, g, t, pose);
        mm_feet(J->m, pose, J->feet, a);
        mm_sample(d, J->m, g, t + 1.0f / MM_HZ, pose);
        mm_feet(J->m, pose, J->feet, b);
        float now[2], fut[2], turn[2] = { 0.0f, 0.0f };
        if (g->trk_mode != MM_TRK_NONE) { // (its root motion, seen from where it faces now; real time is clip time / rate)
            float here[3], w[2], k1[3], k2[3], r = g->rate;
            mm_trk_ahead(d, g, t, here);
            float c = cosf(-here[2]), s = sinf(-here[2]);
            mm_trk_vel(d, g, t, w);
            now[0] = (c * w[0] - s * w[1]) * r, now[1] = (s * w[0] + c * w[1]) * r;
            mm_trk_vel(d, g, t + 0.3f * r, w);
            fut[0] = (c * w[0] - s * w[1]) * r, fut[1] = (s * w[0] + c * w[1]) * r;
            mm_trk_ahead(d, g, t + 0.267f * r, k1), mm_trk_ahead(d, g, t + 0.5f * r, k2);
            turn[0] = k1[2] - here[2], turn[1] = k2[2] - here[2];
        } else if (g->bake < 0 || g->kind == ANIM_MMG_DIRECTION) { // (a steady one)
            now[0] = fut[0] = g->dir[0] * g->speed, now[1] = fut[1] = g->dir[1] * g->speed;
        } else
            mmg_velocity(d, g, t, now), mmg_velocity(d, g, t + 0.3f, fut);
        f[0] = now[0], f[1] = now[1], f[2] = fut[0], f[3] = fut[1];
        f[4] = cosf(turn[0]), f[5] = sinf(turn[0]), f[6] = cosf(turn[1]), f[7] = sinf(turn[1]);
        f[MM_F_VZ] = mm_trk_vz(d, g, t) * g->rate, f[MM_F_VZ + 1] = mm_trk_vz(d, g, t + 0.3f * g->rate) * g->rate; // (a jump's)
        v[0] = now[0], v[1] = now[1];
        for (int k = 0; k < 3; k++) f[MM_F_POSE + k] = a[0][k], f[MM_F_POSE + 3 + k] = a[1][k];
        for (int k = 0; k < 2; k++)
            f[MM_F_POSE + 6 + k] = (b[0][k] - a[0][k]) * MM_HZ * g->rate, f[MM_F_POSE + 8 + k] = (b[1][k] - a[1][k]) * MM_HZ * g->rate;
        for (int k = 0; k < MM_FEAT; k++) f[k] *= g_mm_scale[k];
        d->group[g->first + i] = (uint16_t)gi;
    }
    for (uint32_t i = 0; i < g->count; i++) { // (the travel: the speed summed over the frames, in real time)
        const float* v = &d->vel[(size_t)(g->first + i) * 2], *pv = i ? v - 2 : v;
        d->dist[g->first + i] = i ? d->dist[g->first + i - 1] + (hypotf(v[0], v[1]) + hypotf(pv[0], pv[1])) * 0.5f / MM_HZ / g->rate : 0.0f;
    }
    float* box = &d->box[(size_t)gi * 2 * MM_FEAT];
    for (int k = 0; k < MM_FEAT; k++) box[k] = 1e30f, box[MM_FEAT + k] = -1e30f;
    for (uint32_t i = 0; i < g->nsearch; i++)
        for (int k = 0; k < MM_FEAT; k++) {
            float x = d->f[(size_t)(g->first + i) * MM_FEAT + k];
            box[k] = fminf(box[k], x), box[MM_FEAT + k] = fmaxf(box[MM_FEAT + k], x);
        }
}

static void mm_db_free(MmDb* d) {
    free(d->f), free(d->group), free(d->vel), free(d->dist), free(d->box), free(d->bake), free(d->bake_vel), free(d->groups), free(d->trk), free(d->tz);
}

// The database of a state's (or a node's) clips for a mesh: made once (on the main thread: step_logic, the editor;
// the made animations and the features on the workers), again when its clips, its settings or its graph changed. Found
// by its clips (their names, the settings), the mesh and the graph's revision: a node's stand-in state and the editor's
// find the same one
static MmDb* mm_db_for(const AnimGraph* graph, const SkelMesh* m, const AnimState* s) {
    uint32_t sig = mm_sig(s), rev = graph ? graph->rev : 0;
    for (uint32_t i = 0; i < g_nmm; i++)
        if (g_mm[i].m == m && g_mm[i].sig == sig && g_mm[i].rev == rev) return &g_mm[i];
    MmDb* d = g_nmm < MM_MAX_DB ? &g_mm[g_nmm++] : &g_mm[g_mm_next++ % MM_MAX_DB];
    double t0 = re_time_seconds();
    mm_db_free(d);
    memset(d, 0, sizeof *d);
    snprintf(d->name, ANIM_NAME, "%s", s->name[0] ? s->name : "Motion Matching");
    d->m = m, d->sig = sig, d->rev = rev;
    int16_t feet[2];
    mm_feet_bones(graph, m, feet);
    if (feet[0] < 0 || feet[1] < 0) return d; // (no feet: the state plays its first clip)
    d->groups = malloc(sizeof(MmGroup) * MM_MAX_GROUPS); // (a database's build: not a frame)
    if (!d->groups) return d;
    for (uint32_t b = 0; b < m->nbones; b++) d->mirror_of[b] = mm_pair(m, b);
    d->can_mirror = mm_symmetric(m, d->mirror_of);
    uint32_t nclips = s->nsamples < ANIM_BLEND_SAMPLES ? s->nsamples : ANIM_BLEND_SAMPLES;
    float clip_dir[ANIM_BLEND_SAMPLES][2];
    for (uint32_t k = 0; k < nclips; k++) {
        AnimSeq* q = anim_seq_name(s->samples[k]);
        if (!q || q->length < 0.05f) continue;
        seq_warm(q, m);
        d->clips[k] = q;
        d->clip_speed[k] = mm_clip_speed(m, q, feet, clip_dir[k]);
        // its root motion (as made, mirrored): the speed of a clip that moves its root or its pelvis is that motion's
        int32_t trk[2] = { -1, -1 };
        uint32_t tfr[2] = { 0, 0 };
        int mode[2] = { MM_TRK_NONE, MM_TRK_NONE };
        bool vert[2] = { false, false };
        for (uint32_t mi = 0; mi < (d->can_mirror ? 2u : 1u); mi++) {
            float sp = 0.0f, dr[2] = { 1.0f, 0.0f };
            mode[mi] = mm_track_build(d, m, feet, (uint8_t)k, (uint8_t)mi, q->loop, q->length, &trk[mi], &tfr[mi], &sp, dr, &vert[mi]);
            if (!mi && (mode[0] == MM_TRK_ROOT || mode[0] == MM_TRK_PELVIS)) d->clip_speed[k] = sp, clip_dir[k][0] = dr[0], clip_dir[k][1] = dr[1];
        }
        if (!q->loop && (mode[0] != MM_TRK_NONE || d->clip_speed[k] > 0.0f)) d->authored = true; // (a one-shot of the user's)
        uint32_t nr = q->loop && d->clip_speed[k] > 0.0f ? (uint32_t)(sizeof g_mm_rates / sizeof g_mm_rates[0]) : 1u;
        for (uint32_t mi = 0; mi < (d->can_mirror ? 2u : 1u); mi++)
            for (uint32_t r = 0; r < nr; r++) {
                MmGroup* g = mm_add_group(d);
                if (!g) break;
                g->clip = (uint8_t)k, g->mirror = (uint8_t)mi, g->kind = ANIM_MMG_CLIP, g->loop = q->loop;
                g->trk_mode = (uint8_t)mode[mi], g->trk = trk[mi], g->tframes = tfr[mi], g->vert = vert[mi];
                g->rate = nr > 1 ? g_mm_rates[r] : 1.0f;
                g->speed = d->clip_speed[k] * g->rate;
                g->dir[0] = clip_dir[k][0], g->dir[1] = mi ? -clip_dir[k][1] : clip_dir[k][1];
                g->len = q->length;
                snprintf(g->label, sizeof g->label, "%s%s", q->name, mi ? " (m)" : "");
            }
    }
    // what the user's one-shots cover: a start (from rest into a speed) or a stop (from a speed to rest), each way it goes
    for (uint32_t gi = 0; gi < d->ngroups && d->ncover < 32; gi++) {
        const MmGroup* g = &d->groups[gi];
        if (g->loop || g->trk_mode == MM_TRK_NONE || g->len < 0.3f) continue;
        float a[2], b[2], e = g->len - 0.1f;
        mm_trk_vel(d, g, 0.05f, a), mm_trk_vel(d, g, e, b);
        float sa = hypotf(a[0], a[1]), sb = hypotf(b[0], b[1]), here[3];
        mm_trk_ahead(d, g, sa > sb ? 0.05f : e, here); // (its moving end's velocity, seen from that frame's facing)
        const float* v = sa > sb ? a : b;
        float c = cosf(-here[2]), s = sinf(-here[2]), vx = c * v[0] - s * v[1], vy = s * v[0] + c * v[1], sp = hypotf(vx, vy);
        if (fminf(sa, sb) > 20.0f || sp < 50.0f) continue;
        d->cover[d->ncover].kind = sb > sa ? ANIM_MMG_START : ANIM_MMG_STOP, d->cover[d->ncover].speed = sp;
        d->cover[d->ncover].dir[0] = vx / sp, d->cover[d->ncover++].dir[1] = vy / sp;
    }
    double tg = re_time_seconds();
    float params[ANIM_MMGEN];
    mm_gen_params(s, params);
    mmg_generate(d, graph, m, feet, params); // (the made ones: more groups, their baked frames)
    d->gen_ms = (re_time_seconds() - tg) * 1000.0;
    uint32_t total = 0;
    for (uint32_t gi = 0; gi < d->ngroups; gi++) {
        MmGroup* g = &d->groups[gi];
        g->first = total, g->count = g->bake >= 0 ? g->bframes : (uint32_t)(g->len * MM_HZ);
        if (!g->count) g->count = 1;
        g->nsearch = !g->loop && g->bake < 0 && g->count > 15 ? g->count - 9 : g->count; // (a take's last 0.3 s: no future)
        g->bias = d->authored && g->bake >= 0 && g->kind != ANIM_MMG_DIRECTION ? 0.12f : 0.0f; // (the user's own one-shots first)
        total += g->count;
    }
    d->f = malloc(sizeof(float) * MM_FEAT * (total ? total : 1));
    d->vel = malloc(sizeof(float) * 2 * (total ? total : 1));
    d->dist = malloc(sizeof(float) * (total ? total : 1));
    d->group = malloc(sizeof(uint16_t) * (total ? total : 1));
    d->box = malloc(sizeof(float) * 2 * MM_FEAT * (d->ngroups ? d->ngroups : 1));
    if (!d->f || !d->group || !d->vel || !d->dist || !d->box) return d->ngroups = 0, d;
    MmFeatJob job = { d, m, { feet[0], feet[1] } };
    if (d->ngroups > 1) re_parallel_for((int)d->ngroups, mm_features_job, &job);
    else if (d->ngroups) mm_features_job(&job, 0);
    d->n = total;
    for (uint32_t gi = 0; gi < d->ngroups; gi++) { // a jump's takeoff: what Jump asks for (the vertical velocity in 0.3 s)
        const MmGroup* g = &d->groups[gi];
        if (!g->vert) continue;
        for (uint32_t i = g->first; i < g->first + g->nsearch; i++) {
            const float* f = &d->f[(size_t)i * MM_FEAT];
            if (fabsf(f[MM_F_VZ] / g_mm_scale[MM_F_VZ]) < 30.0f) d->jump_fut = fmaxf(d->jump_fut, f[MM_F_VZ + 1] / g_mm_scale[MM_F_VZ + 1]);
        }
    }
    if (d->jump_fut < 150.0f) d->jump_fut = 0.0f; // (a hop, not a jump)
    if (!d->nlim && d->authored) { // (no loops to make starts from: the body's limits from the user's own one-shots, a take)
        float acc = 0.0f, dec = 0.0f, ca = 0.0f, cd = 0.0f, top = 0.0f;
        for (uint32_t gi = 0; gi < d->ngroups; gi++) {
            const MmGroup* g = &d->groups[gi];
            if (g->kind != ANIM_MMG_CLIP || g->loop || g->mirror || g->count < 8) continue;
            for (uint32_t i = g->first + 3; i + 3 < g->first + g->count; i++) {
                float s0 = hypotf(d->vel[(i - 3) * 2], d->vel[(i - 3) * 2 + 1]), s1 = hypotf(d->vel[(i + 3) * 2], d->vel[(i + 3) * 2 + 1]), a = (s1 - s0) * MM_HZ / 6.0f;
                top = fmaxf(top, s1);
                if (a > 50.0f) acc += a, ca += 1.0f;
                else if (a < -50.0f) dec -= a, cd += 1.0f;
            }
        }
        if (ca > 0.0f && cd > 0.0f) {
            float p[ANIM_MMGEN];
            mm_gen_params(s, p);
            d->nlim = 1, d->lim_speed[0] = top;
            d->lim_acc[0] = p[ANIM_MMGEN_ACCEL] > 0.0f ? p[ANIM_MMGEN_ACCEL] : acc / ca, d->lim_dec[0] = p[ANIM_MMGEN_DECEL] > 0.0f ? p[ANIM_MMGEN_DECEL] : dec / cd;
            d->drive = !((int)p[ANIM_MMGEN_OFF] & ANIM_MMGEN_NO_MOVEMENT);
        }
    }
    // (Q27.11) each group's style by its clip's name
    d->styles = 0;
    for (uint32_t gi = 0; gi < d->ngroups; gi++) {
        MmGroup* g = &d->groups[gi];
        char nm[ANIM_NAME] = "";
        if (g->clip < ANIM_BLEND_SAMPLES && d->clips[g->clip]) snprintf(nm, sizeof nm, "%s", d->clips[g->clip]->name);
        for (char* q = nm; *q; q++) *q = (char)tolower((unsigned char)*q);
        g->style = strstr(nm, "injur") || strstr(nm, "limp") ? ANIM_STYLE_INJURED : strstr(nm, "tired") ? ANIM_STYLE_TIRED
                 : strstr(nm, "drunk") ? ANIM_STYLE_DRUNK : strstr(nm, "carry") ? ANIM_STYLE_CARRY : ANIM_STYLE_NORMAL;
        d->styles |= (uint8_t)(1u << g->style);
    }
    d->build_ms = (re_time_seconds() - t0) * 1000.0;
    g_mm_stats.build_ms += d->build_ms;
    char speeds[160];
    int at = 0;
    for (uint32_t k = 0; k < nclips && at < (int)sizeof speeds - 40; k++)
        if (d->clips[k]) at += snprintf(speeds + at, sizeof speeds - (size_t)at, "%s%s %.0f cm/s", at ? ", " : "", d->clips[k]->name, d->clip_speed[k]);
    float sl[ANIM_MMG_KINDS] = { 0 }, sc[ANIM_MMG_KINDS] = { 0 };
    for (uint32_t gi = 0; gi < d->ngroups; gi++)
        if (d->groups[gi].rate == 1.0f && d->groups[gi].speed > 0.0f) sl[d->groups[gi].kind] += d->groups[gi].slide, sc[d->groups[gi].kind] += 1.0f;
    for (int k = 0; k < ANIM_MMG_KINDS; k++) sl[k] = sc[k] > 0.0f ? sl[k] / sc[k] * 100.0f : 0.0f;
    printf("[RealEngine] Motion Matching %s: %u frames in %u groups (%s%s; made: %u directions, %u starts, %u stops, %u speed changes in "
           "%.1f ms, feet sliding %.1f%% / %.1f%% / %.1f%% / %.1f%% of their travel, the clips' own %.1f%%; jumps: takeoff %.0f cm/s), %.1f ms\n",
           d->name, total, d->ngroups, speeds, d->can_mirror ? "; mirrored" : "", d->made[ANIM_MMG_DIRECTION], d->made[ANIM_MMG_START],
           d->made[ANIM_MMG_STOP], d->made[ANIM_MMG_SPEED], d->gen_ms, sl[ANIM_MMG_DIRECTION], sl[ANIM_MMG_START], sl[ANIM_MMG_STOP],
           sl[ANIM_MMG_SPEED], sl[ANIM_MMG_CLIP], d->jump_fut, d->build_ms);
    return d;
}
static MmDb* mm_db(const AnimInst* in, const AnimState* s) { return mm_db_for(in->graph, in->mesh, s); }

static float mm_cost(const float* a, const float* b) {
    float c = 0.0f;
    for (int k = 0; k < MM_FEAT; k++) c += (a[k] - b[k]) * (a[k] - b[k]);
    return c;
}

// The nearest frame to the query (its first nf features) among the searched ones, each group's bias added: the groups
// whose bounds are farther than the best so far are skipped (start: a frame to beat, its cost start_cost)
static uint32_t mm_search(const MmDb* d, const float* q, int nf, uint32_t start, float start_cost, float* cost, int style) {
    uint32_t best = start;
    float bc = start_cost;
    for (uint32_t gi = 0; gi < d->ngroups; gi++) {
        const MmGroup* g = &d->groups[gi];
        if (style >= 0 && g->style != style) continue; // (Q27.11: the style asked, when the database has it)
        const float* box = &d->box[(size_t)gi * 2 * MM_FEAT];
        float lb = g->bias;
        for (int k = 0; k < nf && lb < bc; k++) {
            float o = q[k] < box[k] ? box[k] - q[k] : q[k] > box[MM_FEAT + k] ? q[k] - box[MM_FEAT + k] : 0.0f;
            lb += o * o;
        }
        if (lb >= bc) continue;
        for (uint32_t i = g->first; i < g->first + g->nsearch; i++) {
            const float* f = &d->f[(size_t)i * MM_FEAT];
            float c = g->bias;
            for (int k = 0; k < nf && c < bc; k++) c += (q[k] - f[k]) * (q[k] - f[k]);
            if (c < bc) bc = c, best = i;
        }
    }
    *cost = bc;
    return best;
}

static uint32_t mm_entry(const MmGroup* g, float t) { return g->first + (uint32_t)fminf(fmaxf(t, 0.0f) * MM_HZ, (float)(g->count - 1)); }

// A group's travel from its start to t (cm)
static float mm_dist_at(const MmDb* d, const MmGroup* g, float t) {
    float f = fminf(fmaxf(t, 0.0f) * MM_HZ, (float)(g->count - 1));
    uint32_t i0 = g->first + (uint32_t)f, i1 = i0 + 1 < g->first + g->count ? i0 + 1 : i0;
    return d->dist[i0] + (d->dist[i1] - d->dist[i0]) * (f - floorf(f));
}

// A one-shot's next time (Unreal's distance matching): where it has traveled as far as the body (dist, cm, since it
// began) - its planted foot then keeps its place whatever the body's acceleration; where it stands still (the start's
// first moment, the stop's settling, a turn in place), real time. 0.25x-3x
static float mm_follow(const MmDb* d, const MmGroup* g, float t, float dist, float dt) {
    uint32_t i = mm_entry(g, t), last = g->first + g->count - 1;
    float here = mm_dist_at(d, g, t);
    if (dist <= here + 0.01f) // (the body has not gone past this frame: still here, or the clip at rest)
        return t + dt * (hypotf(d->vel[i * 2], d->vel[i * 2 + 1]) < 5.0f ? 1.0f : 0.25f);
    while (i < last && d->dist[i + 1] < dist) i++;
    float a = d->dist[i], b = i < last ? d->dist[i + 1] : a, x = b > a + 1e-4f ? (dist - a) / (b - a) : 1.0f;
    float to = ((float)(i - g->first) + fminf(fmaxf(x, 0.0f), 1.0f)) / MM_HZ;
    return fminf(fmaxf(to, t + dt * 0.25f), t + dt * 3.0f);
}

// A group's turn rate at t (radians a second of real time: its root motion's, times its play rate)
static float mm_yaw_rate(const MmDb* d, const MmGroup* g, float t, float rate) {
    if (g->trk_mode == MM_TRK_NONE) return 0.0f;
    const float h = 1.0f / MM_HZ;
    float a[3], b[3], t0 = fmaxf(t - h, 0.0f);
    mm_trk_ahead(d, g, t0, a), mm_trk_ahead(d, g, t + h, b);
    return (b[2] - a[2]) / (t + h - t0) * rate;
}

// A group's notifies between t0 and t1: its clip's, a made one's own (w: the game's, NULL in the editor)
static void mm_events(World* w, AnimInst* in, const MmDb* d, const MmGroup* g, float t0, float t1) {
    if (!w || t1 <= t0) return;
    if (g->bake < 0) {
        fire_notifies(w, in, d->clips[g->clip], t0, t1);
        return;
    }
    float len = g->len > 1e-3f ? g->len : 1.0f, a = g->loop ? fmodf(t0, len) : t0, b = a + (t1 - t0);
    for (uint32_t e = 0; e < g->nev; e++) {
        float et = g->ev_t[e];
        const AnimSeq* q = d->clips[g->ev_clip[e]];
        if (q && g->ev_idx[e] < q->nnotifies && ((et > a && et <= b) || (g->loop && et + len > a && et + len <= b)))
            fire_notify(w, in, &q->notifies[g->ev_idx[e]]); // (its sound too)
    }
}

// A switch: the new group from its time, the old one kept where it was for the inertialization (mm_play_pose)
static void mm_switch(const MmDb* d, AnimMmPlay* p, int32_t to, float t, float blend) {
    p->prev = p->cur, p->pt = p->t, p->prate = p->rate, p->blend = 0.0f, p->blend_len = blend;
    p->cur = to, p->t = t, p->st = t, p->srate = d->groups[to].rate;
    p->dist = mm_dist_at(d, &d->groups[to], t);
}

// One step of a play (the game's and the editor's): the times (a loop's rate fitted to the speed, a one-shot's - a made
// start or stop, the user's, a long take - following the distance), a made one-shot's end on into its loop, the search.
// The query: speed and dir (radians against the facing) now, fut the speed in 0.3 s, face the facing's turn asked in
// 0.27 s and 0.5 s (radians), vz the vertical velocity now and in 0.3 s (cm/s: a jump asked, a fall). A jump the clip
// carries (p->vlock): its time real, no switch but at a take's end
static void mm_play(MmDb* d, AnimMmPlay* p, bool fresh, float speed, float dir, float fut, const float face[2], const float vz[2],
                    float base_rate, float dt, World* w, AnimInst* in, AnimMmMatch* out) {
    if (fresh) memset(p, 0, sizeof *p), p->prev = -1;
    const MmGroup* g = &d->groups[p->cur];
    float rate = g->rate * base_rate, t0 = p->t;
    bool tail = false;
    if (g->loop && g->next < 0) { // (a loop: its rate fitted to the real speed)
        if (g->speed > 30.0f && speed > 1.0f) rate *= fminf(fmaxf(speed / g->speed, 0.85f), 1.18f);
        mm_events(w, in, d, g, t0, t0 + dt * rate);
        p->t = fmodf(t0 + dt * rate, g->len > 1e-3f ? g->len : 1.0f);
    } else {
        p->dist += speed * dt;
        float t1 = speed < 1.0f || p->vlock ? t0 + dt * base_rate : mm_follow(d, g, t0, p->dist, dt * base_rate); // (stopped, a jump: real time)
        rate = dt > 1e-6f ? (t1 - t0) / dt : base_rate;
        mm_events(w, in, d, g, t0, t1);
        p->t = fminf(t1, g->len);
        if (t1 >= g->len && g->next >= 0) { // a made one-shot's end: on into the loop it leads to (from the same place)
            const MmGroup* n = &d->groups[g->next];
            p->rate = rate;
            mm_switch(d, p, g->next, fmodf(g->next_t + (t1 - g->len), n->len > 1e-3f ? n->len : 1.0f), 0.15f);
            g = n;
        }
        tail = g->next < 0 && !g->loop && mm_entry(g, p->t) >= g->first + g->nsearch; // (a take's end: something else now)
    }
    p->rate = rate, p->yaw_rate = mm_yaw_rate(d, g, p->t, rate), p->vz = mm_trk_vz(d, g, p->t) * rate;
    if (p->prev >= 0 && (p->blend += dt) >= p->blend_len) p->prev = -1;
    fut = fminf(fmaxf(fut, 0.0f), speed * 2.0f + 600.0f); // (the speed in 0.3 s)
    bool jump = fabsf(fut - p->qlast) > 40.0f || fabsf(face[1] - p->qface) > 0.5f || fabsf(vz[1] - p->qvz) > 60.0f; // (the query moved a lot: search now)
    if (!fresh && !tail && p->vlock) return;
    if (!fresh && !jump && !tail && (p->next -= dt) > 0.0f) return;
    p->next = MM_SEARCH, p->qlast = fut, p->qface = face[1], p->qvz = vz[1];
    double ts = re_time_seconds();
    // the query: the velocity now and in 0.3 s, the facing's turn, the pose now (the playing frame's)
    float q[MM_FEAT];
    q[0] = cosf(dir) * speed, q[1] = sinf(dir) * speed, q[2] = cosf(dir) * fut, q[3] = sinf(dir) * fut;
    q[4] = cosf(face[0]), q[5] = sinf(face[0]), q[6] = cosf(face[1]), q[7] = sinf(face[1]);
    q[MM_F_VZ] = vz[0], q[MM_F_VZ + 1] = vz[1];
    for (int k = 0; k < MM_F_POSE; k++) q[k] *= g_mm_scale[k];
    uint32_t cur_i = mm_entry(g, p->t);
    const float* cf = &d->f[(size_t)cur_i * MM_FEAT];
    for (int k = MM_F_POSE; k < MM_FEAT; k++) q[k] = fresh ? 0.0f : cf[k];
    float cur_cost = fresh || tail ? 1e30f : mm_cost(q, cf) + g->bias, best_cost;
    // (Q27.11) the style's clips when the database has them (else the normal ones: the layer does the style); a group of
    // another style is left at once (a transition, not a pop)
    int sty = in && in->style < ANIM_STYLE_COUNT && (d->styles >> in->style & 1u) ? in->style : (d->styles & 1u) ? 0 : -1;
    if (sty >= 0 && g->style != sty) cur_cost = 1e30f;
    uint32_t best = mm_search(d, q, fresh ? MM_F_TRAJ : MM_FEAT, cur_i, cur_cost, &best_cost, sty);
    g_mm_stats.searches++, p->searches++;
    uint32_t bg = d->group[best];
    bool same = (int32_t)bg == p->cur && labs((long)best - (long)cur_i) < 6; // (where it already is)
    // (a start or a stop under way is kept unless another is much better: switching halfway slides the feet)
    float keep = g->next >= 0 && p->t < g->len - 0.2f ? 0.5f : 0.85f;
    bool sw = fresh || (tail && !same) || (!same && best_cost < cur_cost * keep - 0.01f);
    if (sw) {
        float to = (float)(best - d->groups[bg].first) / MM_HZ;
        if (fresh) p->cur = (int32_t)bg, p->t = to, p->dist = mm_dist_at(d, &d->groups[bg], to);
        else mm_switch(d, p, (int32_t)bg, to, MM_BLEND);
        g_mm_stats.switches += !fresh, p->switches += !fresh;
        if (w) g_mm_stats.starts += d->groups[bg].kind == ANIM_MMG_START, g_mm_stats.stops += d->groups[bg].kind == ANIM_MMG_STOP,
               g_mm_stats.authored += d->groups[bg].kind == ANIM_MMG_CLIP && !d->groups[bg].loop;
    }
    if (out) out->group = (int)bg, out->t = (float)(best - d->groups[bg].first) / MM_HZ, out->cost = best_cost, out->cur_cost = cur_cost, out->switch_to = sw;
    g_mm_stats.ms += (re_time_seconds() - ts) * 1000.0;
}

// The body's limit (cm/s2) at a speed, from the table (interpolated, the ends held)
static float mm_lim_at(const MmDb* d, const float* lim, float speed) {
    if (!d->nlim) return 0.0f;
    if (speed <= d->lim_speed[0]) return lim[0];
    for (uint32_t i = 1; i < d->nlim; i++)
        if (speed <= d->lim_speed[i]) {
            float x = (speed - d->lim_speed[i - 1]) / fmaxf(d->lim_speed[i] - d->lim_speed[i - 1], 1.0f);
            return lim[i - 1] + (lim[i] - lim[i - 1]) * x;
        }
    return lim[d->nlim - 1];
}

// The facing's turn asked in 0.27 s and 0.5 s: toward the way it is asked to go (from its facing yaw, radians), at a
// person's turn rate (360 degrees a second)
static void mm_face_ahead(float facing, const float want[2], float face[2]) {
    face[0] = face[1] = 0.0f;
    if (want[0] * want[0] + want[1] * want[1] < 1.0f) return;
    float d = mm_wrap_pi(atan2f(want[1], want[0]) - facing), r = 6.2831853f;
    face[0] = fminf(fmaxf(d, -r * 0.267f), r * 0.267f), face[1] = fminf(fmaxf(d, -r * 0.5f), r * 0.5f);
}

// A jump the clip carries (its vertical root motion): it begins when the frame playing, on the ground, rises within
// 0.6 s (Jump asked for it: the search found a takeoff; or the take went on into one); then the clip's vertical velocity
// is the capsule's (anim_vz, a spring holding it to the clip's height against a frame's lag) and nothing switches. It
// ends when the clip lands (back down, or the capsule on the ground going down), when the capsule cannot follow (a
// ceiling, a ledge: 20 cm off the clip's height; gravity goes on from the clip's velocity), at the take's end, or
// when it never left the ground in a second
enum { MMV_LANDED = 0, MMV_BLOCKED, MMV_SWITCHED, MMV_NEVER };
static void mm_vertical(AnimInst* in, const MmDb* d, CCharacter* ch, const CTransform* pt, float dt) {
    AnimMmPlay* p = &in->mm.p;
    if (!ch || !pt || p->cur < 0 || (uint32_t)p->cur >= d->ngroups || ch->swimming) {
        in->mmv.on = p->vlock = false;
        return;
    }
    if (d->jump_fut > 0.0f) ch->anim_jump_ok = 1, in->mm.drove = true; // (Jump asks the animation for one)
    const MmGroup* g = &d->groups[p->cur];
    float z = mm_trk_z(d, g, p->t);
    if (!in->mmv.on || !p->vlock) {
        in->mmv.on = p->vlock = false;
        if (ch->jump_wait <= 0.0f || !g->vert || !ch->on_ground || mm_trk_z(d, g, p->t + 0.6f) - z < 5.0f) return;
        in->mmv.on = p->vlock = true, in->mmv.air = false, in->mmv.time = 0.0f, in->mmv.group = p->cur;
        in->mmv.cap0 = pt->location[2], in->mmv.clip0 = z, in->mmv.top = 0.0f;
        ch->jump_wait = 0.0f; // (the jump asked: this one)
        g_mm_stats.jumps++;
    }
    in->mmv.time += dt;
    float rise = z - in->mmv.clip0, cap = pt->location[2] - in->mmv.cap0;
    if (rise > 3.0f) in->mmv.air = true;
    in->mmv.top = fmaxf(in->mmv.top, cap);
    int why = p->cur != in->mmv.group                                                   ? MMV_SWITCHED
              : fabsf(rise - cap) > 20.0f                                               ? MMV_BLOCKED
              : in->mmv.air && p->vz <= 0.0f && (ch->on_ground || rise < 1.0f)          ? MMV_LANDED
              : !in->mmv.air && in->mmv.time > 1.0f                                     ? MMV_NEVER
                                                                                        : -1;
    if (why >= 0) {
        in->mmv.on = p->vlock = false;
        p->dist = mm_dist_at(d, g, p->t); // (distance matching again from here)
        if (!ch->on_ground && why != MMV_LANDED) ch->velocity[2] = why == MMV_BLOCKED && p->vz > 0.0f ? 0.0f : p->vz; // (a ceiling stops it)
        g_mm_stats.jump_ends[why]++, g_mm_stats.jump_rise = fmaxf(g_mm_stats.jump_rise, in->mmv.top);
        return;
    }
    ch->anim_vz = p->vz + fminf(fmaxf((rise - cap) * 12.0f, -300.0f), 300.0f), ch->anim_vertical = 1, in->mm.drove = true;
    g_mm_stats.jump_s += dt;
}

// The serial part (step_logic): the times, the notifies, the search; the Character's acceleration made the body's, its
// turn the clip's while a clip turns it, its height the clip's while a clip jumps (mm_vertical)
static void mm_step(World* w, AnimInst* in, const AnimState* s, float dt) {
    MmDb* d = mm_db(in, s);
    if (!d->n) {
        in->mm.db = d;
        return;
    }
    Entity pe = in->actor ? in->actor : in->e;
    CCharacter* ch = ecs_get(w, pe, COMP_CHARACTER);
    const CTransform* pt = ecs_get(w, pe, COMP_TRANSFORM);
    float speed = var_of(w, in, s->param[0] ? s->param : "Speed"), dir = s->param2[0] ? var_of(w, in, s->param2) * 0.0174533f : 0.0f;
    if (!s->param2[0]) { // (no Direction variable: the pawn's velocity against its facing, Unreal's Calculate Direction)
        if (ch && pt && ch->velocity[0] * ch->velocity[0] + ch->velocity[1] * ch->velocity[1] > 100.0f)
            dir = atan2f(ch->velocity[1], ch->velocity[0]) - pt->rotation[1] * 0.0174533f;
    }
    if (dt > 1e-4f) in->mm.accel += ((speed - in->mm.speed_prev) / dt - in->mm.accel) * fminf(dt * 8.0f, 1.0f);
    in->mm.speed_prev = speed;
    bool fresh = in->mm.db != d || in->mm.p.cur < 0 || (uint32_t)in->mm.p.cur >= d->ngroups;
    in->mm.db = d;
    // where the speed goes in 0.3 s: toward the one asked (the Character's input) at the body's limits - Unreal's
    // trajectory; without a Character, the speed's change so far
    // (a stop brakes at the rate of the speed it began at all the way down, as its made stop does)
    float fut = speed + in->mm.accel * 0.3f, want = ch ? hypotf(ch->desired[0], ch->desired[1]) : speed, face[2] = { 0.0f, 0.0f };
    if (want >= speed - 1.0f || in->mm.brake_from < speed) in->mm.brake_from = speed;
    float acc = d->nlim ? mm_lim_at(d, d->lim_acc, ch ? ch->max_walk_speed : speed) : 0.0f, dec = d->nlim ? mm_lim_at(d, d->lim_dec, in->mm.brake_from) : 0.0f;
    if (ch) {
        float lim = !d->drive || !d->nlim ? 4096.0f : want >= speed ? acc : fmaxf(dec, acc);
        fut = fabsf(want - speed) < lim * 0.3f ? want : speed + (want > speed ? lim : -lim) * 0.3f;
        if (pt) mm_face_ahead(pt->rotation[1] * 0.0174533f, ch->desired, face);
    }
    // the vertical: a jump asked (Jump: the database's takeoff in 0.3 s), a fall (the capsule's velocity, gravity on it),
    // a jump under way (the clip's own), else on the ground (no jump unasked)
    float vz[2] = { 0.0f, 0.0f };
    if (fresh) in->mmv.on = false;
    if (ch && in->mmv.on && in->mm.p.vlock) vz[0] = vz[1] = in->mm.p.vz;
    else if (ch && ch->jump_wait > 0.0f && ch->on_ground && d->jump_fut > 0.0f) vz[1] = d->jump_fut;
    else if (ch && !ch->on_ground && !ch->swimming)
        vz[0] = ch->velocity[2], vz[1] = ch->velocity[2] - project()->gravity_z * ch->gravity_scale * 0.3f;
    mm_play(d, &in->mm.p, fresh, speed, dir, fut, face, vz, s->rate > 0.0f ? s->rate : 1.0f, dt, w, in, NULL);
    mm_vertical(in, d, ch, pt, dt);
    if (ch && d->drive && d->nlim) { // the body's limits: it starts and stops as fast as its made starts and stops do
        ch->anim_accel = acc, ch->anim_decel = dec;
        in->mm.drove = true;
    }
    if (ch && fabsf(in->mm.p.yaw_rate) > 0.25f) { // a clip that turns (a turn, a pivot): the actor turns with it
        ch->anim_yaw_rate = in->mm.p.yaw_rate * 57.29578f, ch->anim_turning = 1, g_mm_stats.turn_s += dt;
        in->mm.drove = true;
    }
}

// q's rotation vector (axis times angle, the shorter way) and back
static void mm_qlog(const float q[4], float v[3]) {
    float s = q[3] < 0.0f ? -1.0f : 1.0f, l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    float a = 2.0f * atan2f(l, fabsf(q[3])), k = l > 1e-7f ? a / l * s : 2.0f * s;
    v[0] = q[0] * k, v[1] = q[1] * k, v[2] = q[2] * k;
}
static void mm_qexp(const float v[3], float q[4]) {
    float a = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), k = a > 1e-7f ? sinf(a * 0.5f) / a : 0.5f;
    q[0] = v[0] * k, q[1] = v[1] * k, q[2] = v[2] * k, q[3] = cosf(a * 0.5f);
}

// The pose playing; after a switch, inertialized: the jump from the old pose (where it was) to the new one, and how
// fast it was changing, ease out on top of the new pose alone (a critically damped spring: x(s) = (x0 + (v0 + k x0) s)
// e^(-k s)) - the new motion is never averaged with the old one, the feet keep its contacts
static void mm_play_pose(const MmDb* d, const SkelMesh* m, const AnimMmPlay* p, AnimXf* out) {
    const MmGroup* g = &d->groups[p->cur];
    mm_sample(d, m, g, p->t, out);
    if (p->prev < 0 || p->blend >= p->blend_len) return;
    const MmGroup* pg = &d->groups[p->prev];
    const float h = 1.0f / 60.0f;
    AnimXf o0[ANIM_MAX_BONES], o1[ANIM_MAX_BONES], n0[ANIM_MAX_BONES], n1[ANIM_MAX_BONES];
    mm_sample(d, m, pg, p->pt, o0), mm_sample(d, m, pg, fmaxf(p->pt - h * p->prate, 0.0f), o1);
    mm_sample(d, m, g, p->st, n0), mm_sample(d, m, g, fmaxf(p->st - h * p->srate, 0.0f), n1);
    float k = 8.0f / p->blend_len, s = p->blend, e = expf(-k * s), k0 = (1.0f + k * s) * e, k1 = s * e;
    for (uint32_t b = 0; b < m->nbones; b++) {
        float qa[4], qb[4], ci[4], x0[3], x1[3], r[3], off[4], q[4];
        ci[0] = -n0[b].q[0], ci[1] = -n0[b].q[1], ci[2] = -n0[b].q[2], ci[3] = n0[b].q[3];
        q_mul(o0[b].q, ci, qa);
        ci[0] = -n1[b].q[0], ci[1] = -n1[b].q[1], ci[2] = -n1[b].q[2], ci[3] = n1[b].q[3];
        q_mul(o1[b].q, ci, qb);
        mm_qlog(qa, x0), mm_qlog(qb, x1);
        for (int c = 0; c < 3; c++) {
            float dx = x0[c] - x1[c];
            if (fabsf(dx) > 1.0f) dx = 0.0f; // (a flip of the log's branch: no velocity)
            r[c] = x0[c] * k0 + dx / h * k1;
        }
        mm_qexp(r, off);
        q_mul(off, out[b].q, q);
        q_norm(q);
        memcpy(out[b].q, q, 16);
        for (int c = 0; c < 3; c++) {
            float t0 = o0[b].t[c] - n0[b].t[c], t1 = o1[b].t[c] - n1[b].t[c];
            out[b].t[c] += t0 * k0 + (t0 - t1) / h * k1;
        }
    }
}

// A made one-shot (a start, a stop, a speed change) playing (the stats' sliding while they play)
static bool mm_made_playing(const AnimInst* in) {
    const MmDb* d = in->mm.db;
    return d && in->mm.p.cur >= 0 && (uint32_t)in->mm.p.cur < d->ngroups && d->groups[in->mm.p.cur].next >= 0;
}

// The parallel part (state_pose): the frame playing, the jump of a switch easing out on it
static bool mm_pose(const AnimInst* in, AnimXf* out) {
    const MmDb* d = in->mm.db;
    const SkelMesh* m = in->mesh;
    if (!d || !d->n || d->m != m || in->mm.p.cur < 0 || (uint32_t)in->mm.p.cur >= d->ngroups) return false;
    mm_play_pose(d, m, &in->mm.p, out);
    return true;
}

// --- The Motion Matching editor's view of a database (anim.h): its groups, a play of it, a group's pose, travel and
// features. The same database the game uses (mm_db_for), made here on the main thread when missing ---

static MmDb* mm_editor_db(const AnimGraph* g, const SkelMesh* m, const AnimState* s) { return m && s ? mm_db_for(g, m, s) : NULL; }

bool anim_mm_info(const AnimGraph* g, const SkelMesh* m, const AnimState* s, AnimMmInfo* info, AnimMmGroup* groups, uint32_t cap) {
    MmDb* d = mm_editor_db(g, m, s);
    memset(info, 0, sizeof *info);
    if (!d) return false;
    info->frames = d->n, info->ngroups = d->ngroups, info->can_mirror = d->can_mirror, info->build_ms = d->build_ms, info->gen_ms = d->gen_ms;
    info->drives = d->drive, info->nlim = d->nlim;
    memcpy(info->made, d->made, sizeof info->made);
    memcpy(info->lim_speed, d->lim_speed, sizeof info->lim_speed);
    memcpy(info->lim_acc, d->lim_acc, sizeof info->lim_acc);
    memcpy(info->lim_dec, d->lim_dec, sizeof info->lim_dec);
    for (uint32_t i = 0; i < d->ngroups && i < cap; i++) {
        const MmGroup* gr = &d->groups[i];
        AnimMmGroup* o = &groups[i];
        snprintf(o->clip, ANIM_NAME, "%s", d->clips[gr->clip] ? d->clips[gr->clip]->name : "-");
        snprintf(o->label, sizeof o->label, "%s", gr->label);
        o->clip_index = gr->clip, o->kind = gr->kind, o->mirror = gr->mirror != 0, o->loop = gr->loop, o->rate = gr->rate, o->speed = gr->speed;
        o->dir_deg = atan2f(gr->dir[1], gr->dir[0]) * 57.29578f, o->length = gr->len, o->frames = gr->count;
        o->next = gr->next, o->accel = gr->accel, o->slide = gr->slide, o->root_motion = gr->trk_mode;
    }
    for (uint32_t k = 0; k < ANIM_BLEND_SAMPLES; k++) info->clip_speed[k] = d->clip_speed[k];
    return d->n > 0;
}

void anim_mm_sim_step(const AnimGraph* g, const SkelMesh* m, const AnimState* s, AnimMmPlay* p, float speed, float dir_deg, float accel, float turn_deg,
                      float dt, AnimMmMatch* last) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->n) return;
    bool fresh = p->cur < 0 || (uint32_t)p->cur >= d->ngroups;
    AnimMmMatch tmp = { 0 };
    float want[2] = { cosf(turn_deg * 0.0174533f), sinf(turn_deg * 0.0174533f) }, face[2];
    mm_face_ahead(0.0f, want, face);
    if (fabsf(turn_deg) < 0.5f) face[0] = face[1] = 0.0f;
    const float vz[2] = { 0.0f, 0.0f };
    mm_play(d, p, fresh, speed, dir_deg * 0.0174533f, speed + accel * 0.3f, face, vz, s->rate > 0.0f ? s->rate : 1.0f, dt, NULL, NULL, &tmp);
    if (last && tmp.cost + tmp.cur_cost > 0.0f) *last = tmp;
}

bool anim_mm_sim_pose(const AnimGraph* g, const SkelMesh* m, const AnimState* s, const AnimMmPlay* p, AnimXf* out) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->n || p->cur < 0 || (uint32_t)p->cur >= d->ngroups) return false;
    mm_play_pose(d, m, p, out);
    return true;
}

bool anim_mm_pose(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, AnimXf* out) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->n || group < 0 || (uint32_t)group >= d->ngroups) return false;
    const MmGroup* gr = &d->groups[group];
    mm_sample(d, m, gr, gr->loop ? fmodf(fmaxf(t, 0.0f), gr->len > 1e-3f ? gr->len : 1.0f) : fminf(fmaxf(t, 0.0f), gr->len), out);
    return true;
}

bool anim_mm_travel(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, float out[2], float* speed) {
    MmDb* d = mm_editor_db(g, m, s);
    out[0] = out[1] = 0.0f, *speed = 0.0f;
    if (!d || !d->n || group < 0 || (uint32_t)group >= d->ngroups) return false;
    const MmGroup* gr = &d->groups[group];
    float v[2];
    for (float at = 0.0f; at < t; at += 1.0f / MM_HZ) {
        float h = fminf(1.0f / MM_HZ, t - at);
        uint32_t i = mm_entry(gr, gr->loop ? fmodf(at, gr->len) : at);
        out[0] += d->vel[i * 2] * h / gr->rate, out[1] += d->vel[i * 2 + 1] * h / gr->rate; // (clip time: the real one / its rate)
    }
    uint32_t i = mm_entry(gr, gr->loop ? fmodf(t, gr->len) : t);
    v[0] = d->vel[i * 2], v[1] = d->vel[i * 2 + 1];
    *speed = hypotf(v[0], v[1]);
    return true;
}

float anim_mm_limit(const AnimGraph* g, const SkelMesh* m, const AnimState* s, float from, float to) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->nlim) return 0.0f;
    return to >= from ? mm_lim_at(d, d->lim_acc, to) : fmaxf(mm_lim_at(d, d->lim_dec, from), mm_lim_at(d, d->lim_acc, to));
}

// A frame's features unscaled: velocity now (cm/s, x y), in 0.3 s, the feet's places (cm, x y z) and velocities (x y)
bool anim_mm_features(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, float t, float f[ANIM_MM_FEAT]) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->n || group < 0 || (uint32_t)group >= d->ngroups) return false;
    const MmGroup* gr = &d->groups[group];
    uint32_t i = mm_entry(gr, gr->loop ? fmodf(fmaxf(t, 0.0f), gr->len) : t);
    for (int k = 0; k < MM_FEAT; k++) f[k] = d->f[(size_t)i * MM_FEAT + k] / g_mm_scale[k];
    return true;
}

// Frames of the mesh's bones (nf local poses, 30 a second) saved as a one-shot Animation Sequence at path (on disk): the
// user's to keep (generation 0)
static bool mm_save_frames(const SkelMesh* m, const AnimXf* frames, uint32_t nf, const char* path) {
    char vpath[800];
    vpath_of(path, vpath, sizeof vpath);
    AnimSeq* s = seq_alloc(nf, m->nbones);
    if (!s) return false;
    snprintf(s->path, sizeof s->path, "%s", path);
    name_of(path, s->name);
    s->hash = vpath_hash(vpath);
    snprintf(s->skeleton, sizeof s->skeleton, "%s", m->name);
    s->fps = MM_HZ, s->length = (float)(nf - 1) / MM_HZ, s->loop = false;
    for (uint32_t b = 0; b < m->nbones; b++) snprintf(s->track_bone[b], ANIM_NAME, "%s", m->bones[b].name);
    for (uint32_t f = 0; f < nf; f++)
        for (uint32_t b = 0; b < m->nbones; b++) {
            const AnimXf* x = &frames[(size_t)f * m->nbones + b];
            memcpy(&s->rot[((size_t)f * m->nbones + b) * 4], x->q, 16);
            memcpy(&s->pos[((size_t)f * m->nbones + b) * 3], x->t, 12);
        }
    bool ok = anim_seq_save(s);
    free(s->rot), free(s->pos), free(s);
    anim_rescan();
    return ok;
}

// A group saved as an Animation Sequence at path (on disk; root_motion: its travel on the root bone)
static bool mm_export_group(const MmDb* d, const SkelMesh* m, const MmGroup* gr, bool root_motion, const char* path) {
    uint32_t nf = gr->loop ? (uint32_t)lroundf(gr->len * MM_HZ) + 1 : gr->count;
    AnimXf* fr = malloc(sizeof(AnimXf) * nf * m->nbones);
    if (!fr) return false;
    float x = 0.0f, y = 0.0f;
    for (uint32_t i = 0; i < nf; i++) {
        AnimXf* p = &fr[(size_t)i * m->nbones];
        mm_sample(d, m, gr, (float)i / MM_HZ, p);
        if (root_motion) { // (its travel on the root: the velocity summed)
            uint32_t e = mm_entry(gr, (float)i / MM_HZ);
            if (i) x += d->vel[e * 2] / MM_HZ / gr->rate, y += d->vel[e * 2 + 1] / MM_HZ / gr->rate;
            p[0].t[0] += x, p[0].t[1] += y;
        }
    }
    bool ok = mm_save_frames(m, fr, nf, path);
    free(fr);
    return ok;
}

bool anim_mm_export(const AnimGraph* g, const SkelMesh* m, const AnimState* s, int group, bool root_motion, char* out, size_t cap) {
    MmDb* d = mm_editor_db(g, m, s);
    if (!d || !d->n || group < 0 || (uint32_t)group >= d->ngroups || !d->clips[0]) return false;
    const MmGroup* gr = &d->groups[group];
    // the name: the group's label made a file name, next to the database's first clip
    char name[96], dir[512], path[800];
    int n = 0;
    for (const char* c = gr->label; *c && n < (int)sizeof name - 1; c++)
        name[n++] = (isalnum((unsigned char)*c) || *c == '_') ? *c : (*c == '>' ? 't' : '_');
    name[n] = 0;
    snprintf(dir, sizeof dir, "%s", d->clips[0]->path);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    snprintf(path, sizeof path, "%s/%s.ranim", dir, name);
    bool ok = mm_export_group(d, m, gr, root_motion, path);
    if (ok && out) name_of(path, out), (void)cap;
    return ok;
}

// The Mannequin's Motion Matching database (ABP_Mannequin_MM's), made if missing (the examples: NULL without it)
static MmDb* mm_mannequin_db(SkelMesh** mesh) {
    BpAsset* abp = (BpAsset*)bp_find_name(BPA_CLASS, "ABP_Mannequin_MM");
    AnimGraph* g = abp ? anim_graph_of(abp, false) : NULL;
    SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const AnimState* s = NULL;
    for (uint32_t i = 0; g && i < g->nstates && !s; i++)
        if (g->states[i].kind == ANIM_STATE_MOTION_MATCH) s = &g->states[i];
    if (!m || !s) return NULL;
    MmDb* d = mm_db_for(g, m, s);
    *mesh = m;
    return d->n ? d : NULL;
}

// The Authored example's own one-shots, as a user would have made them: the Mannequin's made walk start, walk stop and
// run stop saved with their path on the root (Authored_StartWalk, _StopWalk, _StopRun in dir); the files made
static uint32_t mm_make_authored(const char* dir) {
    SkelMesh* m = NULL;
    MmDb* d = mm_mannequin_db(&m);
    if (!d) return 0;
    static const struct { const char* label; const char* file; } pick[3] = {
        { "MM_Idle > MM_Walk", "Authored_StartWalk" }, { "MM_Walk > MM_Idle (1/8)", "Authored_StopWalk" }, { "MM_Run > MM_Idle (1/8)", "Authored_StopRun" },
    };
    uint32_t made = 0;
    for (int k = 0; k < 3; k++)
        for (uint32_t gi = 0; gi < d->ngroups; gi++)
            if (!strcmp(d->groups[gi].label, pick[k].label)) {
                char path[800];
                snprintf(path, sizeof path, "%s/%s.ranim", dir, pick[k].file);
                made += mm_export_group(d, m, &d->groups[gi], true, path);
                break;
            }
    return made;
}

// A jump in a recorded take (MM_JumpTake), from its start u seconds: the dip and push (the Mannequin's JumpStart, its
// feet leaving at 0.35 s), the flight (the airborne pose; the root on a ballistic arc at MM_JUMP_VZ), the landing (the
// knees taking it), back into the locomotion. The pose over the Motion Matching one (base) by a weight; the root's
// height (cm); the whole jump's length
#define MM_JUMP_VZ 400.0f
#define MM_JUMP_UP 0.35f
#define MM_JUMP_AIR (2.0f * MM_JUMP_VZ / 980.0f)
#define MM_JUMP_LEN (MM_JUMP_UP + MM_JUMP_AIR + 0.85f)
static void mm_qpose_xf(const SkelMesh* m, const QPose* P, AnimXf* out) { // (over out: the Mannequin's bones in MB order)
    for (uint32_t b = 0; b < MB_COUNT && b < m->nbones; b++) {
        memcpy(out[b].q, P->q[b], 16), q_norm(out[b].q);
        memcpy(out[b].t, b == MB_PELVIS ? P->pelvis : m->bones[b].bind.t, 12);
    }
}
static float mm_jump_pose(const SkelMesh* m, float u, const AnimXf* base, AnimXf* out) {
    AnimXf a[ANIM_MAX_BONES], b[ANIM_MAX_BONES];
    QPose P;
    memcpy(a, base, sizeof(AnimXf) * m->nbones), memcpy(b, base, sizeof(AnimXf) * m->nbones);
    float land = MM_JUMP_UP + MM_JUMP_AIR, z = 0.0f, w;
    if (u < MM_JUMP_UP) { // the dip and the push, into the air pose by the takeoff (its height begins there)
        qpose_jump_start(u, 0.4f, &P), mm_qpose_xf(m, &P, a);
        qpose_jump(0.0f, 0.8f, &P), mm_qpose_xf(m, &P, b);
        anim_blend(a, b, smooth01((u - MM_JUMP_UP + 0.08f) / 0.08f), m->nbones, out);
        w = smooth01(u / 0.12f);
    } else if (u < land) { // the flight; the landing's first frame over its last 0.1 s (the legs reaching down)
        float tau = u - MM_JUMP_UP;
        qpose_jump(tau, 0.8f, &P), mm_qpose_xf(m, &P, a);
        qpose_land(0.0f, 0.55f, &P), mm_qpose_xf(m, &P, b);
        anim_blend(a, b, smooth01((u - land + 0.12f) / 0.12f), m->nbones, out);
        z = fmaxf(MM_JUMP_VZ * tau - 490.0f * tau * tau, 0.0f), w = 1.0f;
    } else { // the landing, then back into the locomotion
        qpose_land(fminf(u - land, 0.55f), 0.55f, &P), mm_qpose_xf(m, &P, out);
        w = 1.0f - smooth01((u - land - 0.4f) / 0.4f);
    }
    for (uint32_t k = MB_COUNT; k < m->nbones; k++) out[k] = base[k]; // (the fingers and the rest: the locomotion's)
    anim_blend(base, out, w, m->nbones, a);
    memcpy(out, a, sizeof(AnimXf) * m->nbones);
    return z;
}

// A long take recorded (the Motion Matching examples' LongTake): the Mannequin's Motion Matching (ABP_Mannequin_MM, its
// made starts and stops) played by a script - idle, walk, a left turn walking, run, stop, a pivot back, jog, a turn,
// stop, idle - its frames with the body's path and turn put on the root (as a mocap take carries them), saved at path:
// one file that does it all. jumps: when the take jumps (JumpTake: in place, walking, running; its speed kept in the air)
typedef struct MmTakeStep { float until, speed, dir; } MmTakeStep;
static bool mm_record_plan(const char* path, const MmTakeStep* plan, size_t nplan, const float* jumps, int njumps);
static bool mm_record_take(const char* path) {
    static const MmTakeStep plan[] = { { 1.2f, 0, 0 },    { 4.0f, 116, 0 }, { 6.0f, 116, 90 },  { 8.5f, 411, 90 }, { 10.5f, 0, 90 },
                                       { 11.5f, 0, 90 },  { 14.0f, 116, -90 }, { 16.0f, 250, 0 }, { 18.0f, 0, 0 },   { 19.5f, 0, 0 } };
    return mm_record_plan(path, plan, sizeof plan / sizeof plan[0], NULL, 0);
}
static bool mm_record_jump_take(const char* path) {
    static const MmTakeStep plan[] = { { 3.6f, 0, 0 }, { 7.6f, 116, 0 }, { 11.0f, 411, 0 }, { 12.6f, 0, 0 }, { 13.5f, 0, 0 } };
    static const float jumps[3] = { 1.0f, 5.2f, 8.6f }; // (in place, walking, running)
    return mm_record_plan(path, plan, sizeof plan / sizeof plan[0], jumps, 3);
}
static bool mm_record_plan(const char* path, const MmTakeStep* plan, size_t nplan, const float* jumps, int njumps) {
    BpAsset* abp = (BpAsset*)bp_find_name(BPA_CLASS, "ABP_Mannequin_MM");
    AnimGraph* g = abp ? anim_graph_of(abp, false) : NULL;
    SkelMesh* m = anim_mesh_name("SK_Mannequin");
    const AnimState* s = NULL;
    for (uint32_t i = 0; g && i < g->nstates && !s; i++)
        if (g->states[i].kind == ANIM_STATE_MOTION_MATCH) s = &g->states[i];
    if (!m || !s) return false;
    MmDb* d = mm_db_for(g, m, s);
    if (!d->n) return false;
    const float dt = 1.0f / MM_HZ;
    uint32_t nf = (uint32_t)(plan[nplan - 1].until * MM_HZ) + 1;
    AnimXf* fr = malloc(sizeof(AnimXf) * nf * m->nbones);
    if (!fr) return false;
    float pos[2] = { 0, 0 }, vel[2] = { 0, 0 }, yaw = 0.0f, brake = 0.0f;
    AnimMmPlay p;
    memset(&p, 0, sizeof p), p.cur = p.prev = -1;
    for (uint32_t i = 0; i < nf; i++) {
        float t = (float)i * dt, sp = 0.0f, dd = 0.0f, ju = -1.0f;
        for (size_t k = 0; k < nplan; k++)
            if (t < plan[k].until) {
                sp = plan[k].speed, dd = plan[k].dir * 0.0174533f;
                break;
            }
        for (int k = 0; k < njumps; k++)
            if (t >= jumps[k] && t < jumps[k] + MM_JUMP_LEN) ju = t - jumps[k];
        bool flying = ju >= MM_JUMP_UP && ju < MM_JUMP_UP + MM_JUMP_AIR;
        if (flying) sp = hypotf(vel[0], vel[1]), dd = atan2f(vel[1], vel[0]); // (in the air: its speed kept)
        float want[2] = { cosf(dd) * sp, sinf(dd) * sp }, spd = hypotf(vel[0], vel[1]);
        // the body: toward what is asked at its limits (the Character's movement), facing its velocity (360 degrees a second)
        if (sp >= spd - 1.0f || brake < spd) brake = spd;
        float acc = d->nlim ? mm_lim_at(d, d->lim_acc, sp > 0.0f ? sp : spd) : 4096.0f, dec = d->nlim ? mm_lim_at(d, d->lim_dec, brake) : 4096.0f;
        bool braking = sp < 0.01f || want[0] * vel[0] + want[1] * vel[1] < 0.0f;
        float lim = (braking ? fmaxf(dec, acc) : acc) * dt, dv[2] = { want[0] - vel[0], want[1] - vel[1] }, dl = hypotf(dv[0], dv[1]);
        if (dl > lim) dv[0] *= lim / dl, dv[1] *= lim / dl;
        vel[0] += dv[0], vel[1] += dv[1], pos[0] += vel[0] * dt, pos[1] += vel[1] * dt, spd = hypotf(vel[0], vel[1]);
        if (spd > 10.0f) {
            float turn = mm_wrap_pi(atan2f(vel[1], vel[0]) - yaw), mx = 6.2831853f * dt;
            yaw += fminf(fmaxf(turn, -mx), mx);
        }
        // the query, as the game makes it
        float limq = sp >= spd ? acc : fmaxf(dec, acc), fut = fabsf(sp - spd) < limq * 0.3f ? sp : spd + (sp > spd ? limq : -limq) * 0.3f, face[2];
        mm_face_ahead(yaw, want, face);
        const float vz0[2] = { 0.0f, 0.0f };
        mm_play(d, &p, i == 0, spd, spd > 10.0f ? atan2f(vel[1], vel[0]) - yaw : 0.0f, fut, face, vz0, 1.0f, dt, NULL, NULL, NULL);
        AnimXf* f = &fr[(size_t)i * m->nbones];
        mm_play_pose(d, m, &p, f);
        float jz = 0.0f;
        if (ju >= 0.0f) {
            AnimXf base[ANIM_MAX_BONES];
            memcpy(base, f, sizeof(AnimXf) * m->nbones);
            jz = mm_jump_pose(m, ju, base, f);
        }
        f[0].t[2] += jz;
        float c = cosf(yaw), sn = sinf(yaw), x = f[0].t[0], y = f[0].t[1], q[4];
        const float qz[4] = { 0.0f, 0.0f, sinf(yaw * 0.5f), cosf(yaw * 0.5f) };
        f[0].t[0] = c * x - sn * y + pos[0], f[0].t[1] = sn * x + c * y + pos[1];
        q_mul(qz, f[0].q, q);
        memcpy(f[0].q, q, 16);
    }
    bool ok = mm_save_frames(m, fr, nf, path);
    free(fr);
    printf("[RealEngine] Motion Matching long take recorded: %s (%u frames, %.1f s, %.0f cm)\n", path, nf, (float)(nf - 1) / MM_HZ, hypotf(pos[0], pos[1]));
    return ok;
}
