// The get-up clips (Q10's leftover, 2026-11-19; included by anim.c after anim_fight.inc): the Mannequin getting up
// lying face down (MM_GetUp_Front: the push on the hands, the knees drawn in to all fours, a kneeling lunge on the right
// foot, up) and on its back (MM_GetUp_Back: the chin in and the chest curled up onto the hands, sitting back on straight
// arms with the feet drawn in, the rock forward with the arms reaching ahead into a squat, up). Made the way the
// fighting clips are (keyed in code, no outside asset): key poses of the pelvis, the spine and the head, and for each
// hand and foot a place in the actor's space solved by a two-bone IK with a pole (where the elbow / the knee goes) - so a
// hand or a foot keyed at the same place on the floor stays planted, and the keys were checked for the body's parts
// against the floor (none more than ~1 cm in) and for its speed. Both end on the idle's first pose at the actor's
// origin, facing +X: the Active Ragdoll matches the clip's first pose to the lying body (ikrig.c align_getup), so the
// clips lie where they lie: face down with the head toward +X, on the back with the head toward -X.
// Montages AM_GetUp_Front / AM_GetUp_Back (DefaultSlot: the whole body); a Physics Asset made for a skeleton they fit
// takes them as its get-up montages (phys_autogen).

typedef struct GKey {
    float u;                         // the time (0..1 of the clip)
    float pel[3], pr[3];             // the pelvis's place and turn (euler: roll, pitch, yaw - qe's)
    float sp[3], hd[3];              // each spine bone's turn, the neck's and the head's
    float hand[2][3], elbow[2][3];   // the wrists' places (actor space), the elbows' pole points
    float hrot[2][3];                // the hands' turn in the actor's space
    float ank[2][3], knee[2][3];     // the ankles' places, the knees' pole points
    float frot[2][2];                // the feet's turn in the actor's space (pitch: + toe down, yaw)
} GKey;

#define GK_PALMS { -90.0f, 0.0f, 90.0f }, { 90.0f, 0.0f, -90.0f } // (the hands flat, palms down, fingers forward)
// the idle's first pose (stand_pose at 0), its hands and feet where its FK puts them
#define GK_STAND(u_)                                                                                                                                 \
    { u_, { 0.0f, 2.4f, 93.8f }, { 2.6f, 2.0f, 4.3f }, { -0.9f, 0.0f, -1.0f }, { -1.3f, 0.8f, 2.4f }, { { 9.5f, -24.1f, 93.3f }, { 8.3f, 27.2f, 93.1f } }, \
      { { 3.2f, -21.1f, 118.3f }, { 2.2f, 24.1f, 118.1f } }, { { -6.7f, -20.1f, -0.4f }, { 6.1f, -20.1f, 3.1f } },                                     \
      { { 5.0f, -12.0f, 8.5f }, { -3.0f, 12.5f, 8.5f } }, { { 21.5f, -9.3f, 48.5f }, { 15.4f, 13.1f, 48.2f } }, { { 0.0f, -9.0f }, { 0.0f, 7.0f } } }

// Face down: the head toward +X, the pelvis 75 cm behind where it will stand
static const GKey g_getup_front[] = {
    // lying, the hands by the chest; the legs straight back, the tops of the feet on the floor (held a moment)
    { 0.000f, { -75.0f, 0.0f, 15.0f }, { 0.0f, 88.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, -12.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -30.0f, -48.0f, 30.0f }, { -30.0f, 48.0f, 30.0f } }, { GK_PALMS }, { { -163.0f, -13.0f, 12.0f }, { -163.0f, 13.0f, 12.0f } },
      { { -120.0f, -12.0f, -40.0f }, { -120.0f, 12.0f, -40.0f } }, { { 150.0f, 0.0f }, { 150.0f, 0.0f } } },
    { 0.080f, { -75.0f, 0.0f, 15.0f }, { 0.0f, 88.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, -12.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -30.0f, -48.0f, 30.0f }, { -30.0f, 48.0f, 30.0f } }, { GK_PALMS }, { { -163.0f, -13.0f, 12.0f }, { -163.0f, 13.0f, 12.0f } },
      { { -120.0f, -12.0f, -40.0f }, { -120.0f, 12.0f, -40.0f } }, { { 150.0f, 0.0f }, { 150.0f, 0.0f } } },
    // the knees start to bend on the floor
    { 0.170f, { -75.0f, 0.0f, 15.5f }, { 0.0f, 78.0f, 0.0f }, { 0.0f, -3.0f, 0.0f }, { 0.0f, -13.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -30.0f, -48.0f, 30.0f }, { -30.0f, 48.0f, 30.0f } }, { GK_PALMS }, { { -159.5f, -13.0f, 25.9f }, { -159.5f, 13.0f, 25.9f } },
      { { -121.5f, -12.0f, -21.0f }, { -121.5f, 12.0f, -21.0f } }, { { 185.0f, 0.0f }, { 185.0f, 0.0f } } },
    // the push: the chest up on the arms, the knees bent on the floor, the feet up
    { 0.260f, { -74.0f, 0.0f, 16.0f }, { 0.0f, 62.0f, 0.0f }, { 0.0f, -5.0f, 0.0f }, { 0.0f, -14.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -30.0f, -48.0f, 30.0f }, { -30.0f, 48.0f, 30.0f } }, { GK_PALMS }, { { -146.5f, -13.0f, 41.9f }, { -146.5f, 13.0f, 41.9f } },
      { { -120.2f, -12.0f, -21.0f }, { -120.2f, 12.0f, -21.0f } }, { { 215.0f, 0.0f }, { 215.0f, 0.0f } } },
    // the hips going back, the knees sliding in under them
    { 0.350f, { -69.0f, 0.0f, 31.0f }, { 0.0f, 74.0f, 0.0f }, { 0.0f, -2.0f, 0.0f }, { 0.0f, -18.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -25.0f, -45.0f, 35.0f }, { -25.0f, 45.0f, 35.0f } }, { GK_PALMS }, { { -145.6f, -13.0f, 30.5f }, { -145.6f, 13.0f, 30.5f } },
      { { -105.1f, -12.0f, -21.5f }, { -105.1f, 12.0f, -21.5f } }, { { 190.0f, 0.0f }, { 190.0f, 0.0f } } },
    // on the hands and knees
    { 0.440f, { -60.0f, 0.0f, 48.0f }, { 0.0f, 86.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, -22.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -20.0f, -40.0f, 40.0f }, { -20.0f, 40.0f, 40.0f } }, { GK_PALMS }, { { -118.3f, -13.0f, 10.4f }, { -118.3f, 13.0f, 10.4f } },
      { { -67.3f, -12.0f, -22.5f }, { -67.3f, 12.0f, -22.5f } }, { { 160.0f, 0.0f }, { 160.0f, 0.0f } } },
    // the right knee drawn up under the body
    { 0.545f, { -60.0f, -2.0f, 49.0f }, { 0.0f, 84.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 0.0f, -22.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -20.0f, -40.0f, 40.0f }, { -20.0f, 40.0f, 40.0f } }, { GK_PALMS }, { { -118.3f, -13.0f, 10.4f }, { -60.0f, 15.0f, 32.0f } },
      { { -67.3f, -12.0f, -22.5f }, { -25.0f, 14.0f, 18.0f } }, { { 160.0f, 0.0f }, { 60.0f, 0.0f } } },
    // the right foot planted where it will stand, the left knee down: a kneeling lunge, the hands still on the floor
    { 0.640f, { -58.0f, -3.0f, 48.0f }, { 0.0f, 80.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 0.0f, -20.0f, 0.0f }, { { -8.0f, -28.0f, 3.5f }, { -8.0f, 28.0f, 3.5f } },
      { { -20.0f, -40.0f, 40.0f }, { -20.0f, 40.0f, 40.0f } }, { GK_PALMS }, { { -118.3f, -13.0f, 10.4f }, { -3.0f, 12.5f, 8.5f } },
      { { -67.3f, -12.0f, -22.5f }, { -10.0f, 15.0f, 70.0f } }, { { 160.0f, 0.0f }, { 0.0f, 7.0f } } },
    // the weight onto the right foot, the chest up, the right hand on the knee, the left pushing off the floor
    { 0.750f, { -36.0f, -2.0f, 60.0f }, { 0.0f, 48.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, -10.0f, 0.0f }, { { -8.0f, -28.0f, 3.0f }, { -6.0f, 22.0f, 52.0f } },
      { { -30.0f, -48.0f, 30.0f }, { -30.0f, 45.0f, 50.0f } }, { { -90.0f, 0.0f, 90.0f }, { 60.0f, 40.0f, -60.0f } }, { { -80.5f, -15.0f, 13.3f }, { -3.0f, 12.5f, 8.5f } },
      { { -29.7f, -14.0f, -22.5f }, { 10.0f, 15.0f, 70.0f } }, { { 150.0f, 0.0f }, { 0.0f, 7.0f } } },
    // rising on the right leg, the left foot swinging forward under the body
    { 0.850f, { -10.0f, 2.0f, 80.0f }, { 0.0f, 16.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, -4.0f, 0.0f }, { { -14.0f, -30.0f, 62.0f }, { -6.0f, 28.0f, 64.0f } },
      { { -30.0f, -40.0f, 80.0f }, { -30.0f, 40.0f, 80.0f } }, { { -20.0f, 0.0f, -40.0f }, { 20.0f, 0.0f, -40.0f } }, { { -22.0f, -13.0f, 26.0f }, { -3.0f, 12.5f, 8.5f } },
      { { 10.0f, -14.0f, 60.0f }, { 10.0f, 15.0f, 70.0f } }, { { 30.0f, -6.0f }, { 0.0f, 7.0f } } },
    GK_STAND(0.95f),
    GK_STAND(1.0f),
};

// On the back: the head toward -X, the legs toward +X
static const GKey g_getup_back[] = {
    // lying, the arms by the sides (palms down), the legs straight (held a moment)
    { 0.000f, { -60.0f, 0.0f, 14.0f }, { 0.0f, -88.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { { -62.0f, -30.0f, 4.5f }, { -62.0f, 30.0f, 4.5f } },
      { { -87.0f, -45.0f, 6.0f }, { -87.0f, 45.0f, 6.0f } }, { GK_PALMS }, { { 25.0f, -15.0f, 10.0f }, { 25.0f, 15.0f, 10.0f } },
      { { -15.0f, -13.0f, 40.0f }, { -15.0f, 13.0f, 40.0f } }, { { -70.0f, -8.0f }, { -70.0f, 8.0f } } },
    { 0.070f, { -60.0f, 0.0f, 14.0f }, { 0.0f, -88.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { { -62.0f, -30.0f, 4.5f }, { -62.0f, 30.0f, 4.5f } },
      { { -87.0f, -45.0f, 6.0f }, { -87.0f, 45.0f, 6.0f } }, { GK_PALMS }, { { 25.0f, -15.0f, 10.0f }, { 25.0f, 15.0f, 10.0f } },
      { { -15.0f, -13.0f, 40.0f }, { -15.0f, 13.0f, 40.0f } }, { { -70.0f, -8.0f }, { -70.0f, 8.0f } } },
    // the chin in, the chest curling up onto the hands' push, the knees drawing up
    { 0.240f, { -60.0f, 0.0f, 14.0f }, { 0.0f, -62.0f, 0.0f }, { 0.0f, 7.0f, 0.0f }, { 0.0f, 18.0f, 0.0f }, { { -96.0f, -31.0f, 3.0f }, { -96.0f, 31.0f, 3.0f } },
      { { -125.0f, -42.0f, 25.0f }, { -125.0f, 42.0f, 25.0f } }, { GK_PALMS }, { { -2.0f, -14.0f, 11.0f }, { -8.0f, 14.0f, 11.0f } },
      { { -25.0f, -13.0f, 70.0f }, { -25.0f, 13.0f, 70.0f } }, { { -25.0f, -9.0f }, { -25.0f, 7.0f } } },
    // sitting, leaning back on straight arms, the feet planted where it will stand
    { 0.400f, { -58.0f, 0.0f, 13.0f }, { 0.0f, -55.0f, 0.0f }, { 0.0f, 6.0f, 0.0f }, { 0.0f, 8.0f, 0.0f }, { { -97.0f, -30.0f, 3.0f }, { -97.0f, 30.0f, 3.0f } },
      { { -125.0f, -40.0f, 35.0f }, { -125.0f, 40.0f, 35.0f } }, { GK_PALMS }, { { 5.0f, -12.0f, 8.5f }, { -3.0f, 12.5f, 8.5f } },
      { { -20.0f, -14.0f, 70.0f }, { -20.0f, 14.0f, 70.0f } }, { { 0.0f, -9.0f }, { 0.0f, 7.0f } } },
    // the rock forward: the arms reaching ahead, the hips off the floor over the feet
    { 0.560f, { -36.0f, 0.0f, 30.0f }, { 0.0f, 30.0f, 0.0f }, { 0.0f, 6.0f, 0.0f }, { 0.0f, -12.0f, 0.0f }, { { 38.0f, -18.0f, 60.0f }, { 38.0f, 18.0f, 60.0f } },
      { { 20.0f, -40.0f, 40.0f }, { 20.0f, 40.0f, 40.0f } }, { { -90.0f, -60.0f, 90.0f }, { 90.0f, -60.0f, -90.0f } }, { { 5.0f, -12.0f, 8.5f }, { -3.0f, 12.5f, 8.5f } },
      { { 30.0f, -14.0f, 60.0f }, { 30.0f, 14.0f, 60.0f } }, { { 0.0f, -9.0f }, { 0.0f, 7.0f } } },
    // the squat rising
    { 0.710f, { -20.0f, 0.0f, 54.0f }, { 0.0f, 26.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 0.0f, -8.0f, 0.0f }, { { 32.0f, -20.0f, 78.0f }, { 32.0f, 20.0f, 78.0f } },
      { { 10.0f, -40.0f, 60.0f }, { 10.0f, 40.0f, 60.0f } }, { { -90.0f, -40.0f, 90.0f }, { 90.0f, -40.0f, -90.0f } }, { { 5.0f, -12.0f, 8.5f }, { -3.0f, 12.5f, 8.5f } },
      { { 30.0f, -14.0f, 70.0f }, { 30.0f, 14.0f, 70.0f } }, { { 0.0f, -9.0f }, { 0.0f, 7.0f } } },
    // almost up, the arms coming down
    { 0.850f, { -5.0f, 1.5f, 85.0f }, { 0.0f, 9.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, -3.0f, 0.0f }, { { 8.0f, -27.0f, 62.0f }, { 8.0f, 27.0f, 62.0f } },
      { { -10.0f, -36.0f, 95.0f }, { -10.0f, 36.0f, 95.0f } }, { { -20.0f, -30.0f, 20.0f }, { 20.0f, -30.0f, -20.0f } }, { { 5.0f, -12.0f, 8.5f }, { -3.0f, 12.5f, 8.5f } },
      { { 30.0f, -14.0f, 60.0f }, { 30.0f, 14.0f, 60.0f } }, { { 0.0f, -9.0f }, { 0.0f, 7.0f } } },
    GK_STAND(0.95f),
    GK_STAND(1.0f),
};

// The root-space places and turns of the bones up to (not including) bone `upto` (parents come first in g_mbones)
static void qp_fk(const QPose* P, int upto, float pos[MB_COUNT][3], float rot[MB_COUNT][4]) {
    for (int b = 0; b < upto; b++) {
        int par = g_mbones[b].parent;
        if (par < 0) {
            pos[b][0] = pos[b][1] = pos[b][2] = 0.0f;
            memcpy(rot[b], P->q[b], 16);
            continue;
        }
        float t[3];
        qrot(rot[par], b == MB_PELVIS ? P->pelvis : g_mbones[b].t, t);
        for (int k = 0; k < 3; k++) pos[b][k] = pos[par][k] + t[k];
        q_mul(rot[par], P->q[b], rot[b]);
    }
}
static void gk_norm(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}
static void gk_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static void gk_m3q(const float x[3], const float y[3], const float z[3], float q[4]) { // a frame's axes (columns) -> quaternion
    float m00 = x[0], m01 = y[0], m02 = z[0], m10 = x[1], m11 = y[1], m12 = z[1], m20 = x[2], m21 = y[2], m22 = z[2], tr = m00 + m11 + m22, s;
    if (tr > 0.0f) s = sqrtf(tr + 1.0f) * 2.0f, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s, q[3] = 0.25f * s;
    else if (m00 > m11 && m00 > m22) s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s, q[3] = (m21 - m12) / s;
    else if (m11 > m22) s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s, q[3] = (m02 - m20) / s;
    else s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s, q[3] = (m10 - m01) / s;
    q_norm(q);
}
// A two-bone limb (thigh + calf, upper arm + forearm) to an end place T (root space), its middle joint toward the pole
// point K; s = +1 a leg (the calf bends back), -1 an arm (the forearm bends forward). The end bone takes the world turn
static void limb_ik2(QPose* P, int upper, int lower, int end, float L1, float L2, const float T[3], const float K[3], float s, const float end_world[4]) {
    float pos[MB_COUNT][3], rot[MB_COUNT][4];
    qp_fk(P, upper + 1, pos, rot);
    const float* S = pos[upper];
    const float* prot = rot[g_mbones[upper].parent];
    float d[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    dist = fminf(fmaxf(dist, fabsf(L1 - L2) + 0.5f), (L1 + L2) * 0.999f);
    float k = acosf(fminf(fmaxf((dist * dist - L1 * L1 - L2 * L2) / (2.0f * L1 * L2), -1.0f), 1.0f));
    // the limb unturned (straight down, bent in the X-Z plane) and where it has to go: one frame onto the other
    float x0[3] = { -L2 * sinf(s * k), 0.0f, -L1 - L2 * cosf(s * k) }, y0[3], z0[3], x1[3] = { d[0], d[1], d[2] }, y1[3], z1[3];
    gk_norm(x0), gk_norm(x1);
    float j = -L1 * x0[2]; // (the middle joint (0,0,-L1) off the end's line)
    y0[0] = -j * x0[0], y0[1] = -j * x0[1], y0[2] = -L1 - j * x0[2];
    gk_norm(y0), gk_cross(x0, y0, z0);
    float p[3] = { K[0] - S[0], K[1] - S[1], K[2] - S[2] }, pd = p[0] * x1[0] + p[1] * x1[1] + p[2] * x1[2];
    for (int a = 0; a < 3; a++) y1[a] = p[a] - pd * x1[a];
    if (y1[0] * y1[0] + y1[1] * y1[1] + y1[2] * y1[2] < 1e-6f) y1[0] = y1[1] = 0.0f, y1[2] = 1.0f;
    gk_norm(y1), gk_cross(x1, y1, z1);
    float qa[4], qb[4], qbc[4], w[4], pc[4], wl[4], wlc[4];
    gk_m3q(x1, y1, z1, qa), gk_m3q(x0, y0, z0, qb);
    qconj(qb, qbc), q_mul(qa, qbc, w);
    qconj(prot, pc), q_mul(pc, w, P->q[upper]);
    qe(0.0f, s * k / DEG, 0.0f, P->q[lower]);
    q_mul(w, P->q[lower], wl), qconj(wl, wlc), q_mul(wlc, end_world, P->q[end]);
}

static void gk_pose(const GKey* k, int n, float u, QPose* P) {
    qp_reset(P);
    int i = 0;
    while (i + 1 < n && k[i + 1].u <= u) i++;
    int j = i + 1 < n ? i + 1 : i;
    float s = j != i && k[j].u > k[i].u ? smooth01((u - k[i].u) / (k[j].u - k[i].u)) : 0.0f;
    const GKey *a = &k[i], *b = &k[j];
    float qa[4], qb[4];
#define GK_Q(ea, eb, out) (qe((ea)[0], (ea)[1], (ea)[2], qa), qe((eb)[0], (eb)[1], (eb)[2], qb), q_nlerp(qa, qb, s, out))
#define GK_P(pa, pb, out) ((out)[0] = lerpf((pa)[0], (pb)[0], s), (out)[1] = lerpf((pa)[1], (pb)[1], s), (out)[2] = lerpf((pa)[2], (pb)[2], s))
    GK_P(a->pel, b->pel, P->pelvis);
    GK_Q(a->pr, b->pr, P->q[MB_PELVIS]);
    GK_Q(a->sp, b->sp, P->q[MB_SPINE1]);
    memcpy(P->q[MB_SPINE2], P->q[MB_SPINE1], 16), memcpy(P->q[MB_SPINE3], P->q[MB_SPINE1], 16);
    GK_Q(a->hd, b->hd, P->q[MB_NECK]);
    memcpy(P->q[MB_HEAD], P->q[MB_NECK], 16);
    for (int side = 0; side < 2; side++) { // the legs (under the pelvis), then the arms (under the chest)
        float t[3], pole[3], fq[4], fa[3] = { 0.0f, a->frot[side][0], a->frot[side][1] }, fb[3] = { 0.0f, b->frot[side][0], b->frot[side][1] };
        GK_P(a->ank[side], b->ank[side], t), GK_P(a->knee[side], b->knee[side], pole), GK_Q(fa, fb, fq);
        limb_ik2(P, side ? MB_THIGH_R : MB_THIGH_L, side ? MB_CALF_R : MB_CALF_L, side ? MB_FOOT_R : MB_FOOT_L, MO_THIGH, MO_CALF, t, pole, 1.0f, fq);
    }
    for (int side = 0; side < 2; side++) {
        float t[3], pole[3], hq[4];
        GK_P(a->hand[side], b->hand[side], t), GK_P(a->elbow[side], b->elbow[side], pole), GK_Q(a->hrot[side], b->hrot[side], hq);
        limb_ik2(P, side ? MB_UARM_R : MB_UARM_L, side ? MB_LARM_R : MB_LARM_L, side ? MB_HAND_R : MB_HAND_L, 28.0f, 26.0f, t, pole, -1.0f, hq);
    }
#undef GK_Q
#undef GK_P
}
static void qpose_getup_front(float t, float len, QPose* P) { gk_pose(g_getup_front, (int)(sizeof g_getup_front / sizeof g_getup_front[0]), fminf(t / len, 1.0f), P); }
static void qpose_getup_back(float t, float len, QPose* P) { gk_pose(g_getup_back, (int)(sizeof g_getup_back / sizeof g_getup_back[0]), fminf(t / len, 1.0f), P); }

static const struct { CoreClip c; const char* montage; } g_getup_clips[] = {
    { { "MM_GetUp_Front", 3.2f, false, qpose_getup_front, 0 }, "AM_GetUp_Front" },
    { { "MM_GetUp_Back", 3.0f, false, qpose_getup_back, 0 }, "AM_GetUp_Back" },
};

// The get-up clips and montages in <Mannequin>/GetUp, made when missing (or made by an older generation); the files made
static uint32_t getup_pack(const SkelMesh* m, const char* dir) {
    char gdir[800], path[900];
    snprintf(gdir, sizeof gdir, "%s/GetUp", dir);
    uint32_t made = 0;
    for (size_t i = 0; i < sizeof g_getup_clips / sizeof g_getup_clips[0]; i++) {
        if (!re_file_stat(gdir, NULL, NULL, NULL)) re_make_dirs(gdir);
        made += engine_clip(m, gdir, &g_getup_clips[i].c, CLIP_MAKE);
        snprintf(path, sizeof path, "%s/%s.rmontage", gdir, g_getup_clips[i].montage);
        if (re_file_stat(path, NULL, NULL, NULL)) continue;
        AnimMontage mo = { 0 };
        snprintf(mo.path, sizeof mo.path, "%s", path);
        snprintf(mo.anim, ANIM_NAME, "%s", g_getup_clips[i].c.name);
        snprintf(mo.slot, ANIM_NAME, "DefaultSlot"); // (the whole body)
        mo.blend_in = 0.2f, mo.blend_out = 0.3f, mo.rate = 1.0f; // (the blend in done before the ragdoll matches its first pose: 0.3 s)
        snprintf(mo.sections[0].name, ANIM_NAME, "Default");
        mo.nsections = 1;
        made += anim_montage_save(&mo);
    }
    return made;
}
