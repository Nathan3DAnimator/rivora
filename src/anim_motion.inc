// The engine's character animation (included by anim.c): the Mannequin's clips made the way an animator keys them, not
// as single sine waves. Every bone is a quaternion (QPose); the legs are solved by IK to feet that really plant.
//   * locomotion (gait_pose): each foot follows a path on the ground: in stance it stays still in the world (moving
//     back under the body as fast as the body goes), rolling heel -> flat -> toe; in swing it lifts (a run's heel kicks
//     up behind) and reaches forward on an eased path. The pelvis rises and falls twice a cycle (a walk is highest at
//     mid-stance, a run lowest), shifts over the standing foot, drops on the swinging side and turns with the legs; the
//     chest turns against it; the head stays level and looks ahead; the arms swing against the legs with their elbows
//     and hands following through (later in phase), the clavicles lift with them;
//   * idle: weight on one leg (contrapposto: the other knee soft), the weight drifting, breathing in the chest and
//     shoulders, the head looking around slowly;
//   * actions (jump, land, roll, vault, ladder, wave): key poses with anticipation, overshoot and settle, eased.
// The speeds come out of the stride: a walk of 140 cm/s, a run of 430, a sprint of 700 (Motion Matching measures them).

typedef struct QPose { float q[MB_COUNT][4]; float pelvis[3]; } QPose;
typedef void (*QPoseFn)(float t, float len, QPose* P);

#define MO_THIGH 42.0f  // the Mannequin's leg: thigh, calf (g_mbones), the ankle above the ground, the toes ahead of it
#define MO_CALF 41.0f
#define MO_ANKLE 8.5f
#define MO_TOE 13.0f
#define MO_HEEL 5.0f

static void qe(float rx, float ry, float rz, float q[4]) { // degrees: yaw, then pitch, then roll (make_seq's order)
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    float qx[4], qy[4], qz[4], t[4];
    q_axis(X, rx * DEG, qx), q_axis(Y, ry * DEG, qy), q_axis(Z, rz * DEG, qz);
    q_mul(qz, qy, t), q_mul(t, qx, q);
}
static void qp_set(QPose* P, int b, float rx, float ry, float rz) { qe(rx, ry, rz, P->q[b]); }
static void qp_reset(QPose* P) {
    for (int b = 0; b < MB_COUNT; b++) P->q[b][0] = P->q[b][1] = P->q[b][2] = 0.0f, P->q[b][3] = 1.0f;
    P->pelvis[0] = P->pelvis[1] = 0.0f, P->pelvis[2] = 95.0f;
}
static void qrot(const float q[4], const float v[3], float o[3]) { // q v q*
    float t[3] = { 2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
static void qconj(const float q[4], float o[4]) { o[0] = -q[0], o[1] = -q[1], o[2] = -q[2], o[3] = q[3]; }
static float smooth01(float x) { x = fminf(fmaxf(x, 0.0f), 1.0f); return x * x * (3.0f - 2.0f * x); }
static float wrap01(float u) { return u - floorf(u); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// A looping curve through keys (u in [0, 1), value), Catmull-Rom with the keys before and after wrapped around
static float kcurve(const float (*k)[2], int n, float u) {
    u = wrap01(u);
    int i = n - 1;
    for (int j = 0; j < n; j++)
        if (k[j][0] > u) {
            i = j - 1;
            break;
        }
    if (i < 0) i = n - 1;
    int i0 = (i - 1 + n) % n, i2 = (i + 1) % n, i3 = (i + 2) % n;
    float u1 = k[i][0], u2 = k[i2][0] <= u1 ? k[i2][0] + 1.0f : k[i2][0], uu = u < u1 ? u + 1.0f : u;
    float s = u2 - u1 > 1e-5f ? (uu - u1) / (u2 - u1) : 0.0f;
    float p0 = k[i0][1], p1 = k[i][1], p2 = k[i2][1], p3 = k[i3][1];
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * s + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * s * s + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * s * s * s);
}

// The world (root space) turn of a bone from the pelvis down a chain (the bones' own binds have no turn)
static void chain_q(const QPose* P, const int* bones, int n, float q[4]) {
    q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
    for (int i = 0; i < n; i++) {
        float t[4];
        q_mul(q, P->q[bones[i]], t);
        memcpy(q, t, sizeof t);
    }
}

// A leg to an ankle place (root space) with the foot turned to (pitch: + toe down, yaw), by IK: the thigh's roll and
// pitch, the knee in the leg's plane, then the foot's turn under them
static void leg_ik(QPose* P, int side, const float ankle[3], float foot_pitch, float foot_yaw) {
    int th = side ? MB_THIGH_R : MB_THIGH_L, ca = side ? MB_CALF_R : MB_CALF_L, fo = side ? MB_FOOT_R : MB_FOOT_L;
    const float hip_off[3] = { 0.0f, side ? 10.0f : -10.0f, -5.0f };
    float hip[3], d[3], dl[3], qpc[4];
    qrot(P->q[MB_PELVIS], hip_off, hip);
    for (int k = 0; k < 3; k++) hip[k] += P->pelvis[k], d[k] = ankle[k] - hip[k];
    qconj(P->q[MB_PELVIS], qpc);
    qrot(qpc, d, dl); // (the hip's frame: the pelvis's)
    float len = sqrtf(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]), reach = (MO_THIGH + MO_CALF) * 0.999f;
    if (len > reach) {
        for (int k = 0; k < 3; k++) dl[k] *= reach / len;
        len = reach;
    }
    float ck = (len * len - MO_THIGH * MO_THIGH - MO_CALF * MO_CALF) / (2.0f * MO_THIGH * MO_CALF);
    float knee = acosf(fminf(fmaxf(ck, -1.0f), 1.0f)); // (0: straight)
    float ax = -MO_CALF * sinf(knee), az = -MO_THIGH - MO_CALF * cosf(knee);
    float roll = asinf(fminf(fmaxf(dl[1] / (-az > 1e-3f ? -az : 1e-3f), -1.0f), 1.0f)); // (Rx(roll) puts the leg's end at y = -az sin roll)
    float azr = az * cosf(roll);
    float pitch = atan2f(ax, -azr) - atan2f(dl[0], -dl[2]);
    qe(roll / DEG, pitch / DEG, 0.0f, P->q[th]);
    qe(0.0f, knee / DEG, 0.0f, P->q[ca]);
    // the foot: its world turn (yaw, then pitch) under the pelvis, thigh and calf
    int chain[3] = { MB_PELVIS, th, ca };
    float qw[4], qwc[4], want[4];
    chain_q(P, chain, 3, qw);
    qconj(qw, qwc);
    qe(0.0f, foot_pitch, foot_yaw, want);
    q_mul(qwc, want, P->q[fo]);
}

// --- Locomotion ---

typedef struct Gait {
    float speed, dir[2];   // cm/s along dir (root space: x forward, y right)
    float duty;            // the share of the cycle a foot stands (walk ~0.6, run ~0.35)
    float lift, heel_kick; // the swing's clearance; a run's heel coming up behind (cm)
    float height, bob, sway; // the pelvis: its height (lowered when the stride asks), up-down, side to side (cm)
    float hip_yaw, hip_roll, hip_pitch, lean; // degrees
    float width;           // the feet's distance from the middle (cm)
    float heel_strike, toe_off; // the foot's pitch at contact (toe up) and at push off (toe down), degrees
    float arm_fwd, arm_back, arm_out, elbow, elbow_swing, twist;
    float head_bob;        // the head's nod against the bounce (degrees)
    bool run;              // the bounce's phase (lowest at mid-stance), a flight
} Gait;

// one foot's place and pitch at its phase v (0: contact)
static void gait_foot(const Gait* G, float stride, float v, int side, float out[3], float* pitch) {
    float half = stride * G->duty * 0.5f, x, z = MO_ANKLE, pt;
    if (v < G->duty) { // stance: still in the world, rolling heel -> flat -> toe
        float s = v / G->duty;
        x = half * (1.0f - 2.0f * s);
        float strike = 1.0f - smooth01(s / 0.12f), off = smooth01((s - 0.62f) / 0.38f);
        pt = -G->heel_strike * strike + G->toe_off * off;
        if (pt > 0.0f) { // the heel up: the toes stay where they are, the ankle rises over them
            float a = pt * DEG;
            x -= MO_TOE * (1.0f - cosf(a)), z += MO_TOE * sinf(a);
        } else { // the toes up on the heel
            float a = -pt * DEG;
            z += MO_HEEL * sinf(a);
        }
    } else { // swing: up (a run kicks the heel back first), forward on an eased path
        float s = (v - G->duty) / (1.0f - G->duty), e = smooth01(s * 1.08f - 0.04f);
        float x0 = -half - MO_TOE * (1.0f - cosf(G->toe_off * DEG)), x1 = half;
        x = lerpf(x0, x1, e);
        z = MO_ANKLE + MO_TOE * sinf(G->toe_off * DEG) * (1.0f - smooth01(s / 0.25f)) + G->lift * sinf(PI * powf(s, 0.8f));
        if (G->heel_kick > 0.0f) {
            float k = sinf(PI * fminf(s / 0.62f, 1.0f));
            z += G->heel_kick * k, x -= G->heel_kick * 0.45f * k;
        }
        pt = lerpf(G->toe_off * 0.7f, -G->heel_strike, smooth01((s - 0.35f) / 0.6f));
    }
    float sy = side ? G->width : -G->width;
    out[0] = x * G->dir[0], out[1] = x * G->dir[1] + sy, out[2] = z;
    *pitch = pt;
}

// The pelvis's height where every foot can be reached with its knee still a little soft (an inverted pendulum: a walk
// is highest over the standing foot and lowest when both feet are down)
static float pelvis_reach(const QPose* P, const float feet[2][3]) {
    float best = 1e9f, R = (MO_THIGH + MO_CALF) * 0.995f;
    for (int side = 0; side < 2; side++) {
        const float off[3] = { 0.0f, side ? 10.0f : -10.0f, -5.0f };
        float hip[3];
        qrot(P->q[MB_PELVIS], off, hip);
        float dx = feet[side][0] - (P->pelvis[0] + hip[0]), dy = feet[side][1] - (P->pelvis[1] + hip[1]);
        best = fminf(best, feet[side][2] + sqrtf(fmaxf(R * R - dx * dx - dy * dy, 1.0f)) - hip[2]);
    }
    return best;
}

static void gait_pose(float t, float len, const Gait* G, QPose* P) {
    qp_reset(P);
    float u = wrap01(t / len), stride = G->speed * len;
    float mid = G->duty * 0.5f; // (mid-stance of the left foot)
    P->pelvis[1] = -G->sway * cosf(2.0f * PI * (u - mid)); // over the standing foot (left: -y)
    float yaw = G->hip_yaw * cosf(2.0f * PI * u), roll = -G->hip_roll * cosf(2.0f * PI * (u - mid));
    float pitch = G->hip_pitch + (G->run ? 1.5f : 0.8f) * cosf(4.0f * PI * (u - mid - 0.06f));
    qp_set(P, MB_PELVIS, roll, pitch, yaw);
    // the feet (the right half a cycle later), then the pelvis's height: a walk's from the legs' reach (the pendulum);
    // a run's lowest at mid-stance (the knee takes the landing) and never past what the feet allow
    float feet[2][3], fp[2];
    for (int side = 0; side < 2; side++) gait_foot(G, stride, wrap01(u + (side ? 0.5f : 0.0f)), side, feet[side], &fp[side]);
    float reach = pelvis_reach(P, feet);
    if (G->run) {
        float bounce = cosf(4.0f * PI * (u - mid));
        P->pelvis[2] = fminf(G->height - G->bob - G->bob * bounce, reach - 0.5f);
    } else
        P->pelvis[2] = fminf(G->height, reach - 0.6f) - G->bob * 0.5f * (1.0f - cosf(4.0f * PI * (u - mid))) * 0.25f;
    for (int side = 0; side < 2; side++) leg_ik(P, side, feet[side], fp[side], 0.0f);
    // the chest against the hips, leaning into the speed, a little bounce after the pelvis's
    float sp = G->lean + 1.2f * cosf(4.0f * PI * (u - mid - 0.1f)) * (G->run ? 1.0f : 0.5f);
    float cyaw = -yaw * G->twist, croll = -roll * 0.8f;
    qp_set(P, MB_SPINE1, croll * 0.3f, sp * 0.35f - G->hip_pitch * 0.5f, cyaw * 0.25f);
    qp_set(P, MB_SPINE2, croll * 0.35f, sp * 0.35f, cyaw * 0.35f);
    qp_set(P, MB_SPINE3, croll * 0.35f, sp * 0.3f, cyaw * 0.4f);
    // the head: level and looking ahead (the neck takes most of it), a nod against the bounce
    float tyaw = yaw + cyaw, troll = roll + croll, tpitch = pitch + sp;
    float nod = G->head_bob * cosf(4.0f * PI * (u - mid - 0.14f));
    qp_set(P, MB_NECK, -troll * 0.5f, -tpitch * 0.45f, -tyaw * 0.5f);
    qp_set(P, MB_HEAD, -troll * 0.4f, -tpitch * 0.45f + nod, -tyaw * 0.45f);
    // the arms against the legs: forward when the other leg is; the elbow and the hand later (follow through)
    for (int side = 0; side < 2; side++) {
        int cl = side ? MB_CLAV_R : MB_CLAV_L, ua = side ? MB_UARM_R : MB_UARM_L, la = side ? MB_LARM_R : MB_LARM_L, ha = side ? MB_HAND_R : MB_HAND_L;
        float a = wrap01(u + (side ? 0.0f : 0.5f) - 0.05f), f = cosf(2.0f * PI * a); // (1: the arm forward)
        float swing = f > 0.0f ? G->arm_fwd * f : G->arm_back * f;
        float fe = cosf(2.0f * PI * (a - 0.07f)), el = G->elbow + G->elbow_swing * (0.5f + 0.5f * fe);
        float out = side ? G->arm_out : -G->arm_out, inward = G->run ? (side ? -4.0f : 4.0f) * fmaxf(f, 0.0f) : 0.0f;
        qp_set(P, cl, (side ? 1.0f : -1.0f) * 2.0f * fmaxf(f, 0.0f), -2.0f * f, 0.0f); // (lifts with the forward swing)
        qp_set(P, ua, out + inward, -swing, side ? -4.0f : 4.0f);
        qp_set(P, la, 0.0f, -el, 0.0f);
        qp_set(P, ha, 0.0f, -4.0f * sinf(2.0f * PI * (a - 0.15f)) - 3.0f, (side ? 1.0f : -1.0f) * 4.0f);
    }
}

static const Gait g_walk = { 125.0f, { 1, 0 }, 0.61f, 10.0f, 0.0f, 95.0f, 1.6f, 2.2f, 6.0f, 4.0f, 2.0f, 2.5f, 10.5f, 14.0f, 22.0f,
                             22.0f, 14.0f, 8.0f, 16.0f, 12.0f, 1.1f, 1.0f, false };
static const Gait g_run = { 430.0f, { 1, 0 }, 0.37f, 16.0f, 26.0f, 95.0f, 3.2f, 1.2f, 10.0f, 6.0f, 6.0f, 9.0f, 9.5f, 8.0f, 32.0f,
                            42.0f, 30.0f, 6.0f, 78.0f, 16.0f, 1.25f, 2.0f, true };
static const Gait g_sprint = { 700.0f, { 1, 0 }, 0.3f, 20.0f, 38.0f, 95.0f, 3.8f, 0.8f, 12.0f, 7.0f, 10.0f, 15.0f, 8.5f, 6.0f, 36.0f,
                               62.0f, 48.0f, 5.0f, 88.0f, 20.0f, 1.3f, 2.5f, true };

// the jog: between the walk and the run (a short flight, the arms bent and swinging low)
static const Gait g_jog = { 260.0f, { 1, 0 }, 0.42f, 12.0f, 14.0f, 95.0f, 2.4f, 1.6f, 8.0f, 5.0f, 4.0f, 5.5f, 10.0f, 10.0f, 26.0f,
                            30.0f, 22.0f, 7.0f, 62.0f, 14.0f, 1.2f, 1.5f, true };
static void qpose_walk(float t, float len, QPose* P) { gait_pose(t, len, &g_walk, P); }
static void qpose_jog(float t, float len, QPose* P) { gait_pose(t, len, &g_jog, P); }
static void qpose_run(float t, float len, QPose* P) { gait_pose(t, len, &g_run, P); }
static void qpose_sprint(float t, float len, QPose* P) { gait_pose(t, len, &g_sprint, P); }
static void qpose_walk_back(float t, float len, QPose* P) { // the walk backward: the stance carries the foot forward, leaning back
    Gait G = g_walk;
    G.speed = 115.0f, G.lean = -3.0f, G.hip_pitch = -1.0f, G.heel_strike = -10.0f, G.toe_off = 4.0f, G.lift = 8.0f;
    gait_pose(len - wrap01(t / len) * len, len, &G, P); // (the cycle reversed: toes land first, the heel lifts last)
}
static void strafe_pose(float t, float len, QPose* P, float dir, float q) { // side steps (q: half of one, cm): the feet apart and together, never crossing
    qp_reset(P);
    float u = wrap01(t / len), h = 92.0f; // (the feet 19 cm out: never crossing)
    P->pelvis[2] = h - 1.4f * (0.5f + 0.5f * cosf(4.0f * PI * (u - 0.25f)));
    P->pelvis[1] = dir * 2.0f * sinf(2.0f * PI * u);
    qp_set(P, MB_PELVIS, -dir * 3.0f * cosf(2.0f * PI * u), 2.0f, 0.0f);
    for (int side = 0; side < 2; side++) {
        // the leading leg (the side it moves to) steps out, the trailing one closes in; each lifts in its swing
        float v = wrap01(u + (side ? 0.5f : 0.0f)), y, z = MO_ANKLE, base = side ? 19.0f : -19.0f;
        if (v < 0.55f) y = base + dir * q * (1.0f - 2.0f * v / 0.55f); // (planted: the body moves over it toward dir)
        else {
            float s = (v - 0.55f) / 0.45f;
            y = base + dir * q * (2.0f * smooth01(s) - 1.0f);
            z += 9.0f * sinf(PI * s);
        }
        float a[3] = { side ? -2.0f : 2.0f, y, z };
        leg_ik(P, side, a, 0.0f, side ? 6.0f : -6.0f);
    }
    qp_set(P, MB_SPINE2, dir * 2.0f * cosf(2.0f * PI * u), 3.0f, 0.0f);
    qp_set(P, MB_SPINE3, dir * 1.5f * cosf(2.0f * PI * u), 2.0f, 0.0f);
    qp_set(P, MB_HEAD, -dir * 2.0f * cosf(2.0f * PI * u), -4.0f, 0.0f);
    for (int side = 0; side < 2; side++) {
        float s = side ? 1.0f : -1.0f;
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * (10.0f + 3.0f * sinf(2.0f * PI * (u - 0.08f))), -6.0f, 0.0f);
        qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0.0f, -24.0f - 4.0f * sinf(2.0f * PI * (u - 0.15f)), 0.0f);
    }
}
static void qpose_strafe_l(float t, float len, QPose* P) { strafe_pose(t, len, P, -1.0f, 14.0f); }
static void qpose_strafe_r(float t, float len, QPose* P) { strafe_pose(t, len, P, 1.0f, 14.0f); }

// --- Standing still: weight on one leg (contrapposto), breathing, the weight and the gaze drifting ---

static void stand_pose(float t, float len, QPose* P, float crouch) {
    qp_reset(P);
    float u = wrap01(t / len), drift = sinf(2.0f * PI * u), breath = sinf(2.0f * PI * 2.0f * u);
    float h = lerpf(94.0f, 60.0f, crouch);
    P->pelvis[0] = lerpf(0.0f, -6.0f, crouch);
    P->pelvis[1] = lerpf(2.4f, 1.0f, crouch) + 0.9f * drift; // (over the right foot)
    P->pelvis[2] = h - 0.35f * breath - 0.3f * (0.5f + 0.5f * drift);
    float proll = lerpf(2.6f, 1.0f, crouch) + 0.7f * drift;
    qp_set(P, MB_PELVIS, proll, lerpf(2.0f, 18.0f, crouch), 3.0f + 1.5f * sinf(2.0f * PI * u + 1.0f));
    static const float feet[2][3] = { { 5.0f, -12.0f, 0.0f }, { -3.0f, 12.5f, 0.0f } };
    for (int side = 0; side < 2; side++) {
        float a[3] = { feet[side][0] + crouch * (side ? -6.0f : 8.0f), feet[side][1] * (1.0f + crouch * 0.3f), MO_ANKLE };
        leg_ik(P, side, a, 0.0f, side ? 7.0f : -9.0f);
    }
    // the chest level again over the tilted pelvis, rising with each breath
    float lean = lerpf(0.0f, 14.0f, crouch);
    qp_set(P, MB_SPINE1, -proll * 0.45f, lean * 0.3f - 0.3f * breath, -1.2f);
    qp_set(P, MB_SPINE2, -proll * 0.35f, lean * 0.35f - 0.6f * breath, -1.0f);
    qp_set(P, MB_SPINE3, -proll * 0.25f, lean * 0.35f - 0.9f * breath, -0.8f);
    for (int side = 0; side < 2; side++) {
        float s = side ? 1.0f : -1.0f;
        qp_set(P, side ? MB_CLAV_R : MB_CLAV_L, s * 1.4f * (0.5f + 0.5f * breath), 0.0f, 0.0f);
        float lag = sinf(2.0f * PI * 2.0f * u - 0.6f);
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * (7.5f + 0.8f * lag) + s * crouch * 6.0f, -3.0f - crouch * 20.0f, s * -6.0f);
        qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0.0f, -13.0f - 1.5f * lag - crouch * 40.0f, s * 4.0f);
        qp_set(P, side ? MB_HAND_R : MB_HAND_L, 0.0f, -6.0f, s * 6.0f);
    }
    float look = 5.0f * sinf(2.0f * PI * u + 2.2f), nod = 1.8f * sinf(2.0f * PI * 2.0f * u + 0.8f) + 0.6f * breath;
    qp_set(P, MB_NECK, proll * 0.3f, -lean * 0.4f + nod * 0.4f, look * 0.4f - 1.0f);
    qp_set(P, MB_HEAD, proll * 0.25f - 2.0f, -lean * 0.4f + nod * 0.6f, look * 0.6f);
}
static void qpose_idle(float t, float len, QPose* P) { stand_pose(t, len, P, 0.0f); }
static void qpose_crouch_idle(float t, float len, QPose* P) { stand_pose(t, len, P, 1.0f); }
static void qpose_crouch_walk(float t, float len, QPose* P) {
    Gait G = g_walk;
    G.speed = 95.0f, G.height = 66.0f, G.bob = 1.2f, G.sway = 3.0f, G.lift = 8.0f, G.hip_pitch = 16.0f, G.lean = 16.0f;
    G.arm_fwd = 14.0f, G.arm_back = 8.0f, G.elbow = 48.0f, G.elbow_swing = 10.0f, G.width = 13.5f, G.duty = 0.66f;
    gait_pose(t, len, &G, P);
}

// --- Actions: key poses blended with eases (anticipation, overshoot, settle) ---

// A pose from keys: per key a time (0..1) and, for the listed bones, their euler (deg); the pelvis's place; between keys
// eased (smoothstep), the turns slerped by nlerp
typedef struct QKey { float u; float pelvis[3]; float r[MB_COUNT][3]; } QKey;
static void keys_pose(const QKey* k, int n, float u, QPose* P, const QPose* base) {
    int i = 0;
    while (i + 1 < n && k[i + 1].u <= u) i++;
    int j = i + 1 < n ? i + 1 : i;
    float s = j != i && k[j].u > k[i].u ? smooth01((u - k[i].u) / (k[j].u - k[i].u)) : 0.0f;
    for (int b = 0; b < MB_COUNT; b++) {
        float qa[4], qb[4], q[4], t[4];
        qe(k[i].r[b][0], k[i].r[b][1], k[i].r[b][2], qa);
        qe(k[j].r[b][0], k[j].r[b][1], k[j].r[b][2], qb);
        q_nlerp(qa, qb, s, q);
        if (base) q_mul(base->q[b], q, t), memcpy(P->q[b], t, sizeof t);
        else memcpy(P->q[b], q, sizeof q);
    }
    for (int a = 0; a < 3; a++) P->pelvis[a] = lerpf(k[i].pelvis[a], k[j].pelvis[a], s);
}

// the airborne pose: knees tucked unevenly, arms up and out for balance, a slow drift
static void qpose_jump(float t, float len, QPose* P) {
    qp_reset(P);
    float u = wrap01(t / len), b = sinf(2.0f * PI * u), c = cosf(2.0f * PI * u);
    P->pelvis[2] = 95.0f;
    qp_set(P, MB_PELVIS, 1.0f * b, 6.0f, 2.0f * c);
    qp_set(P, MB_THIGH_L, -2.0f, -48.0f + 4.0f * b, 0.0f), qp_set(P, MB_CALF_L, 0.0f, 82.0f - 3.0f * b, 0.0f), qp_set(P, MB_FOOT_L, 0.0f, 22.0f, 0.0f);
    qp_set(P, MB_THIGH_R, 2.0f, -18.0f - 4.0f * b, 0.0f), qp_set(P, MB_CALF_R, 0.0f, 58.0f + 4.0f * b, 0.0f), qp_set(P, MB_FOOT_R, 0.0f, 28.0f, 0.0f);
    qp_set(P, MB_SPINE1, 0.0f, 4.0f, 0.0f), qp_set(P, MB_SPINE2, 0.0f, 3.0f, -2.0f * c), qp_set(P, MB_SPINE3, 0.0f, 2.0f, -2.0f * c);
    qp_set(P, MB_HEAD, 0.0f, -8.0f, 2.0f * c);
    for (int side = 0; side < 2; side++) {
        float s = side ? 1.0f : -1.0f, ph = side ? 0.6f : 0.0f;
        qp_set(P, side ? MB_CLAV_R : MB_CLAV_L, s * 6.0f, 0.0f, 0.0f);
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * (58.0f + 5.0f * sinf(2.0f * PI * u + ph)), -22.0f, s * -8.0f);
        qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0.0f, -32.0f - 6.0f * sinf(2.0f * PI * u + ph - 0.7f), 0.0f);
        qp_set(P, side ? MB_HAND_R : MB_HAND_L, 0.0f, -10.0f, s * 8.0f);
    }
}
static void qpose_fall(float t, float len, QPose* P) { // falling: arms reaching out, wheeling slowly; legs reaching down
    qpose_jump(t, len, P);
    float u = wrap01(t / len), b = sinf(2.0f * PI * u);
    qp_set(P, MB_THIGH_L, -2.0f, -30.0f + 7.0f * b, 0.0f), qp_set(P, MB_CALF_L, 0.0f, 48.0f + 5.0f * b, 0.0f);
    qp_set(P, MB_THIGH_R, 2.0f, -12.0f - 7.0f * b, 0.0f), qp_set(P, MB_CALF_R, 0.0f, 34.0f - 5.0f * b, 0.0f);
    for (int side = 0; side < 2; side++) { // (out and a little forward, the elbows soft, each arm on its own beat)
        float s = side ? 1.0f : -1.0f, ph = side * 1.4f;
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * (52.0f + 9.0f * sinf(2.0f * PI * u + ph)), -24.0f + 12.0f * sinf(2.0f * PI * u + ph + 0.6f), s * -10.0f);
        qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0.0f, -38.0f - 10.0f * sinf(2.0f * PI * u + ph - 0.8f), 0.0f);
    }
}

// a key's bones from a pose function's frame (the actions start from the standing pose)
static void key_from(QKey* k, float u, float px, float py, float pz) {
    memset(k->r, 0, sizeof k->r);
    k->u = u, k->pelvis[0] = px, k->pelvis[1] = py, k->pelvis[2] = pz;
}
#define KR(k, b, x, y, z) ((k).r[b][0] = (x), (k).r[b][1] = (y), (k).r[b][2] = (z))

// the jump's start: a quick dip (anticipation), arms back, then the push: legs and arms extend, toes last
static void qpose_jump_start(float t, float len, QPose* P) {
    QKey k[4];
    key_from(&k[0], 0.0f, 0, 0, 94.0f);
    KR(k[0], MB_UARM_L, -8, -4, 0), KR(k[0], MB_UARM_R, 8, -4, 0), KR(k[0], MB_LARM_L, 0, -14, 0), KR(k[0], MB_LARM_R, 0, -14, 0);
    key_from(&k[1], 0.45f, -2, 0, 76.0f); // (the dip: knees, hips back, chest forward, arms swung back)
    KR(k[1], MB_PELVIS, 0, 22, 0), KR(k[1], MB_SPINE1, 0, 10, 0), KR(k[1], MB_SPINE2, 0, 8, 0), KR(k[1], MB_HEAD, 0, -20, 0);
    KR(k[1], MB_UARM_L, -12, 48, 0), KR(k[1], MB_UARM_R, 12, 48, 0), KR(k[1], MB_LARM_L, 0, -20, 0), KR(k[1], MB_LARM_R, 0, -20, 0);
    key_from(&k[2], 0.8f, 1, 0, 99.0f); // (the push: long body, arms thrown up)
    KR(k[2], MB_PELVIS, 0, -4, 0), KR(k[2], MB_SPINE2, 0, -4, 0), KR(k[2], MB_HEAD, 0, -6, 0);
    KR(k[2], MB_UARM_L, -18, -120, 0), KR(k[2], MB_UARM_R, 18, -120, 0), KR(k[2], MB_LARM_L, 0, -26, 0), KR(k[2], MB_LARM_R, 0, -26, 0);
    k[3] = k[2], k[3].u = 1.0f, k[3].pelvis[2] = 100.0f;
    keys_pose(k, 4, fminf(t / len, 1.0f), P, NULL);
    // the feet: planted until the push, then toes last (the ankle rises over them)
    float u = fminf(t / len, 1.0f), off = smooth01((u - 0.62f) / 0.3f) * 38.0f;
    for (int side = 0; side < 2; side++) {
        float a[3] = { 2.0f - MO_TOE * (1.0f - cosf(off * DEG)), side ? 12.0f : -12.0f, MO_ANKLE + MO_TOE * sinf(off * DEG) + 14.0f * smooth01((u - 0.88f) / 0.12f) };
        leg_ik(P, side, a, off, side ? 5.0f : -5.0f);
    }
}
// landing: the knees take the impact (past the dip: overshoot), the arms forward and down for balance, then up
static void qpose_land(float t, float len, QPose* P) {
    QKey k[4];
    key_from(&k[0], 0.0f, 0, 0, 93.0f);
    KR(k[0], MB_UARM_L, -40, -20, 0), KR(k[0], MB_UARM_R, 40, -20, 0), KR(k[0], MB_LARM_L, 0, -25, 0), KR(k[0], MB_LARM_R, 0, -25, 0);
    key_from(&k[1], 0.22f, -3, 0, 70.0f); // (the impact's deepest point)
    KR(k[1], MB_PELVIS, 0, 20, 0), KR(k[1], MB_SPINE1, 0, 12, 0), KR(k[1], MB_SPINE2, 0, 9, 0), KR(k[1], MB_HEAD, 0, -16, 0);
    KR(k[1], MB_UARM_L, -22, -40, 0), KR(k[1], MB_UARM_R, 22, -40, 0), KR(k[1], MB_LARM_L, 0, -50, 0), KR(k[1], MB_LARM_R, 0, -50, 0);
    key_from(&k[2], 0.55f, -1, 0, 89.0f); // (coming up: a little past standing in the chest)
    KR(k[2], MB_PELVIS, 0, 6, 0), KR(k[2], MB_SPINE2, 0, -2, 0), KR(k[2], MB_HEAD, 0, 3, 0);
    KR(k[2], MB_UARM_L, -10, -6, 0), KR(k[2], MB_UARM_R, 10, -6, 0), KR(k[2], MB_LARM_L, 0, -18, 0), KR(k[2], MB_LARM_R, 0, -18, 0);
    key_from(&k[3], 1.0f, 0, 2.0f, 94.0f);
    KR(k[3], MB_PELVIS, 2, 2, 3), KR(k[3], MB_UARM_L, -8, -3, 0), KR(k[3], MB_UARM_R, 8, -3, 0), KR(k[3], MB_LARM_L, 0, -13, 0), KR(k[3], MB_LARM_R, 0, -13, 0);
    keys_pose(k, 4, fminf(t / len, 1.0f), P, NULL);
    for (int side = 0; side < 2; side++) {
        float a[3] = { side ? -3.0f : 5.0f, side ? 12.5f : -12.0f, MO_ANKLE };
        leg_ik(P, side, a, 0.0f, side ? 7.0f : -9.0f);
    }
}
// the forward roll: tucked into a ball, the body turns over once, opening up at the end
static void qpose_roll(float t, float len, QPose* P) {
    qp_reset(P);
    float u = fminf(t / len, 1.0f), turn = smooth01((u - 0.12f) / 0.72f), tuck = sinf(PI * smooth01(u / 0.92f)); // (tucked first, then over)
    P->pelvis[0] = lerpf(-30.0f, 40.0f, smooth01(u)), P->pelvis[2] = 95.0f - 58.0f * tuck;
    qp_set(P, MB_PELVIS, 0.0f, 360.0f * turn, 0.0f);
    qp_set(P, MB_SPINE1, 0, 26.0f * tuck, 0), qp_set(P, MB_SPINE2, 0, 24.0f * tuck, 0), qp_set(P, MB_SPINE3, 0, 18.0f * tuck, 0);
    qp_set(P, MB_NECK, 0, 20.0f * tuck, 0), qp_set(P, MB_HEAD, 0, 26.0f * tuck, 0);
    for (int side = 0; side < 2; side++) {
        float s = side ? 1.0f : -1.0f, lag = sinf(PI * smooth01((u - 0.05f) / 0.9f));
        qp_set(P, side ? MB_THIGH_R : MB_THIGH_L, s * 6.0f * tuck, -118.0f * tuck, 0), qp_set(P, side ? MB_CALF_R : MB_CALF_L, 0, 135.0f * lag, 0);
        qp_set(P, side ? MB_FOOT_R : MB_FOOT_L, 0, 30.0f * lag, 0);
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * 20.0f * tuck, -75.0f * tuck, 0), qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0, -95.0f * lag, 0);
    }
}
// the ladder: hand over hand with the opposite knee, the body close to the rungs, the head looking up a little
static void qpose_climb(float t, float len, QPose* P) {
    qp_reset(P);
    float u = wrap01(t / len), s = sinf(2.0f * PI * u), c = cosf(2.0f * PI * u);
    P->pelvis[0] = -4.0f, P->pelvis[2] = 95.0f + 3.0f * cosf(4.0f * PI * u);
    qp_set(P, MB_PELVIS, 2.0f * s, -4.0f, 3.0f * s);
    qp_set(P, MB_SPINE2, -1.5f * s, -5.0f, -3.0f * s), qp_set(P, MB_SPINE3, -1.0f * s, -3.0f, -2.0f * s), qp_set(P, MB_HEAD, 0, -16.0f, 2.0f * s);
    for (int side = 0; side < 2; side++) {
        float sg = side ? 1.0f : -1.0f, ph = side ? -1.0f : 1.0f, reach = fmaxf(ph * s, 0.0f), lag = fmaxf(ph * sinf(2.0f * PI * (u - 0.08f)), 0.0f);
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, sg * 12.0f, -150.0f - 22.0f * reach + 8.0f * c * ph, 0);
        qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0, -35.0f - 30.0f * lag, 0);
        qp_set(P, side ? MB_HAND_R : MB_HAND_L, 0, -20.0f, 0);
        float lift = fmaxf(-ph * s, 0.0f);
        float a[3] = { 18.0f + 4.0f * lift, side ? 13.0f : -13.0f, MO_ANKLE + 34.0f * smooth01(lift) };
        leg_ik(P, side, a, -10.0f * lift, 0.0f);
    }
}
// the vault: hands planted on the obstacle, the hips swung up and over to one side, the legs tucked, landing stride
static void qpose_vault(float t, float len, QPose* P) {
    qp_reset(P);
    float u = fminf(t / len, 1.0f), over = sinf(PI * smooth01(u)), lean = smooth01(u / 0.4f) * (1.0f - smooth01((u - 0.7f) / 0.3f));
    P->pelvis[0] = lerpf(-35.0f, 35.0f, smooth01(u)), P->pelvis[1] = -18.0f * over, P->pelvis[2] = 95.0f + 58.0f * over;
    qp_set(P, MB_PELVIS, 24.0f * over, 10.0f * lean, -10.0f * over);
    qp_set(P, MB_SPINE1, -6.0f * over, 16.0f * lean, 0), qp_set(P, MB_SPINE2, -6.0f * over, 14.0f * lean, 0), qp_set(P, MB_HEAD, 0, -14.0f * lean, 0);
    for (int side = 0; side < 2; side++) {
        float s = side ? 1.0f : -1.0f;
        qp_set(P, side ? MB_UARM_R : MB_UARM_L, s * 18.0f * over, -78.0f * lean, 0), qp_set(P, side ? MB_LARM_R : MB_LARM_L, 0, -8.0f * lean, 0);
        qp_set(P, side ? MB_HAND_R : MB_HAND_L, 0, 40.0f * lean, 0);
        qp_set(P, side ? MB_THIGH_R : MB_THIGH_L, -22.0f * over, -(side ? 72.0f : 90.0f) * over, 0);
        qp_set(P, side ? MB_CALF_R : MB_CALF_L, 0, (side ? 70.0f : 46.0f) * over, 0);
        qp_set(P, side ? MB_FOOT_R : MB_FOOT_L, 0, 20.0f * over, 0);
    }
}
// the wave: weight to the left foot, the arm dips (anticipation), rises past the top (overshoot) and settles; the
// forearm waves with the hand following a beat later; the head tilts toward it; back down easing out
static void qpose_wave(float t, float len, QPose* P) {
    stand_pose(t, len * 2.0f, P, 0.0f);
    float u = fminf(t / len, 1.0f), up = smooth01((u - 0.08f) / 0.2f), down = smooth01((u - 0.8f) / 0.2f), k = up * (1.0f - down);
    float dip = sinf(PI * smooth01(u / 0.1f)) * (1.0f - up), over = sinf(PI * smooth01((u - 0.2f) / 0.15f)) * 0.12f;
    float wave = sinf(2.0f * PI * 2.4f * (t - 0.3f)), wave_lag = sinf(2.0f * PI * 2.4f * (t - 0.36f));
    P->pelvis[1] -= 1.5f * k;
    float qa[4], qb[4];
    qe(9.0f + 132.0f * (k + over) - 6.0f * dip, -16.0f * k, -10.0f * k, qa);
    memcpy(P->q[MB_UARM_R], qa, 16);
    qe(22.0f * k * wave, -38.0f * k - 10.0f, 10.0f * k, qb);
    memcpy(P->q[MB_LARM_R], qb, 16);
    qp_set(P, MB_HAND_R, 14.0f * k * wave_lag, -8.0f, 6.0f);
    qp_set(P, MB_CLAV_R, 10.0f * k, 0.0f, 0.0f);
    qp_set(P, MB_SPINE3, -3.0f * k, 0.0f, 3.0f * k);
    qp_set(P, MB_HEAD, 7.0f * k, -2.0f * k, 12.0f * k);
}

// --- Writing a clip from a QPose function (30 frames a second; a loop's last frame equals its first) ---

#define ANIM_GENERATOR 5 // the engine's clips' generation: older ones are made again (keeping their notifies)
static AnimSeq* make_seq_q(const SkelMesh* m, const char* path, const char* vpath, float len, bool loop, QPoseFn fn) {
    uint32_t nf = (uint32_t)lroundf(len * 30.0f) + 1;
    AnimSeq* s = seq_alloc(nf, MB_COUNT);
    if (!s) return NULL;
    snprintf(s->path, sizeof s->path, "%s", path);
    name_of(path, s->name);
    s->hash = vpath_hash(vpath);
    snprintf(s->skeleton, sizeof s->skeleton, "%s", m->name);
    s->fps = 30.0f, s->length = len, s->loop = loop, s->generator = ANIM_GENERATOR;
    for (int b = 0; b < MB_COUNT; b++) snprintf(s->track_bone[b], ANIM_NAME, "%s", m->bones[b].name);
    for (uint32_t f = 0; f < nf; f++) {
        QPose P;
        fn(loop ? (float)f / (float)(nf - 1) * len : (float)f / 30.0f, len, &P);
        for (int b = 0; b < MB_COUNT; b++) {
            q_norm(P.q[b]);
            memcpy(&s->rot[((size_t)f * MB_COUNT + (uint32_t)b) * 4], P.q[b], 16);
            memcpy(&s->pos[((size_t)f * MB_COUNT + (uint32_t)b) * 3], m->bones[b].bind.t, 12);
        }
        memcpy(&s->pos[((size_t)f * MB_COUNT + MB_PELVIS) * 3], P.pelvis, 12);
    }
    return s;
}

// --- The engine's clips: made when missing, made again when an older engine made them (their notifies kept) ---

typedef struct CoreClip { const char* name; float len; bool loop; QPoseFn fn; int steps; } CoreClip; // steps: footstep notifies
static const CoreClip g_core_clips[] = {
    { "MM_Idle", 4.8f, true, qpose_idle, 0 }, { "MM_Walk", 1.06f, true, qpose_walk, 2 }, { "MM_Jog", 0.78f, true, qpose_jog, 2 },
    { "MM_Run", 0.7f, true, qpose_run, 2 },
    { "MM_Jump", 0.8f, true, qpose_jump, 0 }, { "MM_Wave", 1.8f, false, qpose_wave, 0 },
};
enum { CLIP_MAKE = 0, CLIP_ONLY_OLD, CLIP_ALWAYS };

static bool engine_clip(const SkelMesh* m, const char* dir, const CoreClip* c, int mode) {
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s.ranim", dir, c->name);
    vpath_of(path, vpath, sizeof vpath);
    AnimSeq* old = NULL;
    if (re_file_stat(path, NULL, NULL, NULL)) {
        old = anim_seq_load(vpath);
        if (mode != CLIP_ALWAYS && old && old->generator >= ANIM_GENERATOR) return false;
        if (old && old->generator == 0 && mode == CLIP_ONLY_OLD && old->nframes > 0 && old->skeleton[0] && strcmp(old->skeleton, m->name))
            return false; // (an animation of another skeleton by that name: not ours)
    } else if (mode == CLIP_ONLY_OLD)
        return false;
    AnimSeq* s = make_seq_q(m, path, vpath, c->len, c->loop, c->fn);
    if (!s) return false;
    if (old && old->nnotifies) {
        memcpy(s->notifies, old->notifies, sizeof s->notifies);
        s->nnotifies = old->nnotifies;
    } else if (c->steps == 2)
        add_notify(s, "Footstep", 0.0f), add_notify(s, "Footstep", c->len * 0.5f);
    bool ok = anim_seq_save(s);
    if (old) { // the loaded one takes the new frames: whoever holds it sees them
        free(old->rot), free(old->pos);
        old->rot = s->rot, old->pos = s->pos, old->nframes = s->nframes, old->ntracks = s->ntracks;
        old->length = s->length, old->fps = s->fps, old->loop = s->loop, old->generator = s->generator;
        memcpy(old->track_bone, s->track_bone, sizeof old->track_bone);
        old->map_for = NULL;
        free(s);
    } else
        free(s->rot), free(s->pos), free(s);
    return ok;
}

static const CoreClip g_locomotion_clips[] = {
    { "MM_WalkBack", 1.1f, true, qpose_walk_back, 2 },   { "MM_StrafeLeft", 0.9f, true, qpose_strafe_l, 2 },
    { "MM_StrafeRight", 0.9f, true, qpose_strafe_r, 2 }, { "MM_Sprint", 0.56f, true, qpose_sprint, 2 },
    { "MM_CrouchIdle", 4.0f, true, qpose_crouch_idle, 0 }, { "MM_CrouchWalk", 1.3f, true, qpose_crouch_walk, 2 },
    { "MM_JumpStart", 0.4f, false, qpose_jump_start, 0 }, { "MM_Fall", 1.2f, true, qpose_fall, 0 },
    { "MM_Land", 0.55f, false, qpose_land, 0 },          { "MM_Roll", 0.95f, false, qpose_roll, 0 },
    { "MM_Climb", 1.0f, true, qpose_climb, 0 },          { "MM_Vault", 0.8f, false, qpose_vault, 0 },
};

static uint32_t anim_make_creatures(int pack); // (anim_creatures.inc)
static uint32_t getup_pack(const SkelMesh* m, const char* dir); // (anim_getup.inc)
static uint32_t traversal_pack(const SkelMesh* m, const char* dir); // (anim_traversal.inc)

// A project's engine-made clips from an older generation made again (the Mannequin's, the Locomotion pack's, the
// creatures'); the Mannequin's Blend Space thresholds follow the new walk's and run's speeds
static void engine_clips_update(const char* dir) {
    static const struct { int pack; const char* clip; } creatures[] = {
        { ANIM_PACK_QUADRUPED, "Dog/Dog_Walk" }, { ANIM_PACK_FISH, "Fish/Fish_Swim" }, { ANIM_PACK_BIRD, "Bird/Bird_Fly" }, { ANIM_PACK_REPTILE, "Lizard/Lizard_Walk" },
    };
    for (size_t i = 0; i < sizeof creatures / sizeof creatures[0]; i++) {
        char cp[800], cv[800];
        snprintf(cp, sizeof cp, "%s/Creatures/%s.ranim", A.root, creatures[i].clip);
        if (!re_file_stat(cp, NULL, NULL, NULL)) continue;
        vpath_of(cp, cv, sizeof cv);
        AnimSeq* q = anim_seq_load(cv);
        if (q && q->generator < ANIM_GENERATOR) printf("[RealEngine] %s: made again (%u files)\n", creatures[i].clip, anim_make_creatures(creatures[i].pack));
    }
    char path[800], vpath[800], pdir[800];
    snprintf(path, sizeof path, "%s/SK_Mannequin.rskm", dir);
    vpath_of(path, vpath, sizeof vpath);
    SkelMesh* m = re_file_stat(path, NULL, NULL, NULL) ? anim_mesh_load(vpath) : NULL;
    if (!m) return;
    uint32_t made = 0;
    for (size_t i = 0; i < sizeof g_core_clips / sizeof g_core_clips[0]; i++) made += engine_clip(m, dir, &g_core_clips[i], CLIP_MAKE);
    if (getup_pack(m, dir)) anim_rescan(); // (Q10: the Active Ragdoll's get-up clips and montages, made when missing)
    if (traversal_pack(m, dir)) anim_rescan(); // (2026-11-23: Try Traversal's clips and montages)
    snprintf(pdir, sizeof pdir, "%s/Locomotion", dir);
    for (size_t i = 0; i < sizeof g_locomotion_clips / sizeof g_locomotion_clips[0]; i++)
        made += engine_clip(m, pdir, &g_locomotion_clips[i], CLIP_ONLY_OLD);
    if (!made) return;
    anim_rescan();
    static const char* const abps[2] = { "ABP_Mannequin", "ABP_Mannequin_MM" };
    for (int k = 0; k < 2; k++) {
        BpAsset* abp = (BpAsset*)bp_find_name(BPA_CLASS, abps[k]);
        AnimGraph* g = abp ? anim_graph_of(abp, false) : NULL;
        bool changed = false;
        for (uint32_t i = 0; g && i < g->nstates; i++) {
            AnimState* s = &g->states[i];
            // the Blend Space: idle, walk, jog, run at their speeds now (the old 160 / 500 made the feet slide)
            if (s->kind == ANIM_STATE_BLEND1D && s->nsamples == 3 && !strcmp(s->samples[1], "MM_Walk") && !strcmp(s->samples[2], "MM_Run")) {
                snprintf(s->samples[2], ANIM_NAME, "MM_Jog"), snprintf(s->samples[3], ANIM_NAME, "MM_Run");
                s->sample_at[1] = 120.0f, s->sample_at[2] = 260.0f, s->sample_at[3] = 420.0f, s->nsamples = 4, changed = true;
            }
            // Motion Matching: the jog in its database
            if (s->kind == ANIM_STATE_MOTION_MATCH && s->nsamples < ANIM_BLEND_SAMPLES) {
                bool have = false;
                for (uint32_t c = 0; c < s->nsamples; c++) have |= !strcmp(s->samples[c], "MM_Jog");
                if (!have) snprintf(s->samples[s->nsamples++], ANIM_NAME, "MM_Jog"), changed = true;
            }
        }
        if (changed) g->rev++, bp_save(abp);
    }
    printf("[RealEngine] Animations of the engine made again (a newer generation): %u\n", made);
}
