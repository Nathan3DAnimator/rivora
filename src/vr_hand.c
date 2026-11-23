// The VR base hand and the hand solver (vr_hand.h). The hand is made in the FBX's space (Blender's: right-handed, Z up,
// cm; the right hand's fingers along +X, its thumb toward +Y, the back of the hand up) - the left one is its mirror in Y.
// The engine's importer turns it into the engine's space (Y mirrored), which is where the solver works
#include "vr_hand.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fbx_write.h"
#include "platform.h"
#include "platform_files.h"

const char* const g_vrh_joint_names[VRH_JOINTS] = {
    "Palm", "Wrist", "ThumbMetacarpal", "ThumbProximal", "ThumbDistal", "ThumbTip",
    "IndexMetacarpal", "IndexProximal", "IndexIntermediate", "IndexDistal", "IndexTip",
    "MiddleMetacarpal", "MiddleProximal", "MiddleIntermediate", "MiddleDistal", "MiddleTip",
    "RingMetacarpal", "RingProximal", "RingIntermediate", "RingDistal", "RingTip",
    "LittleMetacarpal", "LittleProximal", "LittleIntermediate", "LittleDistal", "LittleTip",
};
const char* const g_vrh_pose_names[VRH_POSES] = { "Open", "Relaxed", "Fist", "Grip", "Point", "Pinch", "ThumbsUp" };
static const int8_t g_parent[VRH_JOINTS] = { 1, -1, 1, 2, 3, 4, 1, 6, 7, 8, 9, 1, 11, 12, 13, 14, 1, 16, 17, 18, 19, 1, 21, 22, 23, 24 };
static int finger_of_joint(int j) { return j < 2 ? -1 : j < 6 ? 0 : 1 + (j - 6) / 5; }

#define VRH_DEG 0.017453292519943295f

// --- small vector and quaternion math (x y z w) ---
static float v_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v_cross(const float a[3], const float b[3], float o[3]) {
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, r, 12);
}
static float v_norm(float v[3]) {
    float l = sqrtf(v_dot(v, v));
    if (l > 1e-8f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}
static void v_sub(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static void q_mul(const float a[4], const float b[4], float o[4]) {
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, 16);
}
static void q_conj(const float a[4], float o[4]) { o[0] = -a[0], o[1] = -a[1], o[2] = -a[2], o[3] = a[3]; }
static void q_rot(const float q[4], const float v[3], float o[3]) {
    float t[3], u[3];
    v_cross(q, v, t);
    t[0] *= 2.0f, t[1] *= 2.0f, t[2] *= 2.0f;
    v_cross(q, t, u);
    float r[3] = { v[0] + q[3] * t[0] + u[0], v[1] + q[3] * t[1] + u[1], v[2] + q[3] * t[2] + u[2] };
    memcpy(o, r, 12);
}
static void q_axis(const float axis[3], float rad, float o[4]) {
    float a[3] = { axis[0], axis[1], axis[2] };
    v_norm(a);
    float s = sinf(rad * 0.5f);
    o[0] = a[0] * s, o[1] = a[1] * s, o[2] = a[2] * s, o[3] = cosf(rad * 0.5f);
}
static void q_norm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-8f)
        for (int k = 0; k < 4; k++) q[k] /= l;
}
// the shortest rotation taking direction a to b (unit)
static void q_from_to(const float a[3], const float b[3], float o[4]) {
    float c[3];
    v_cross(a, b, c);
    float d = v_dot(a, b);
    if (d < -0.9999f) { // (opposite: any axis across)
        float ax[3] = { 1, 0, 0 };
        if (fabsf(a[0]) > 0.9f) ax[0] = 0, ax[1] = 1;
        v_cross(a, ax, c), v_norm(c);
        o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = 0.0f;
        return;
    }
    o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = 1.0f + d;
    q_norm(o);
}
// FBX's XYZ Euler (R = Rz Ry Rx, degrees) of a rotation
static void q_euler_xyz(const float q[4], double e[3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3], l = sqrt(x * x + y * y + z * z + w * w); // (double: near +-90 degrees on Y the floats lost a centimeter)
    x /= l, y /= l, z /= l, w /= l;
    double m20 = 2.0 * (x * z - w * y), m21 = 2.0 * (y * z + w * x), m22 = 1.0 - 2.0 * (x * x + y * y);
    double m10 = 2.0 * (x * y + w * z), m00 = 1.0 - 2.0 * (y * y + z * z);
    double sy = -m20 > 1.0 ? 1.0 : -m20 < -1.0 ? -1.0 : -m20;
    const double deg = 57.29577951308232;
    e[0] = atan2(m21, m22) * deg, e[1] = asin(sy) * deg, e[2] = atan2(m10, m00) * deg;
    // the other solution (x + 180, 180 - y, z + 180) when nearer zero: a finger bent 100 degrees on Y is (0, 100, 0), not
    // (180, 80, 180) - the keys from 0 interpolate through it without spinning
    double o[3] = { e[0] + 180.0, 180.0 - e[1], e[2] + 180.0 };
    for (int k = 0; k < 3; k++) o[k] = o[k] > 180.0 ? o[k] - 360.0 : o[k] < -180.0 ? o[k] + 360.0 : o[k];
    if (fabs(o[0]) + fabs(o[1]) + fabs(o[2]) < fabs(e[0]) + fabs(e[1]) + fabs(e[2])) e[0] = o[0], e[1] = o[1], e[2] = o[2];
}

// --- the bones' names ---
static void name_key(const char* s, char* o, size_t cap) {
    size_t n = 0;
    for (; *s && n + 1 < cap; s++)
        if (isalnum((unsigned char)*s)) o[n++] = (char)tolower((unsigned char)*s);
    o[n] = 0;
    char* p = strstr(o, "pinky"); // ("Pinky" for Little: one letter longer)
    if (p && n + 2 < cap) memmove(p + 6, p + 5, strlen(p + 5) + 1), memcpy(p, "little", 6);
}
int vrh_joint_of_name(const char* bone_name) {
    char k[96], j[32];
    name_key(bone_name, k, sizeof k);
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < VRH_JOINTS; i++) {
        name_key(g_vrh_joint_names[i], j, sizeof j);
        size_t l = strlen(j);
        if (l > bl && strstr(k, j)) best = i, bl = l;
    }
    return best;
}

// --- the base hand (the right one, FBX space) ---
static float g_J[VRH_JOINTS][3]; // the joints at the bind (the flat open hand)
static bool g_J_made;
static void make_joints(void) {
    if (g_J_made) return;
    float (*J)[3] = g_J;
    J[VRH_WRIST][0] = J[VRH_WRIST][1] = J[VRH_WRIST][2] = 0.0f;
    // the four fingers: metacarpal base, knuckle, direction, phalanges (cm: an adult's hand, 18.8 cm wrist to tip)
    static const float base[4][3] = { { 1.2f, 1.4f, 0.15f }, { 1.2f, 0.35f, 0.2f }, { 1.2f, -0.75f, 0.15f }, { 1.2f, -1.7f, 0.0f } };
    static const float knuck[4][3] = { { 9.2f, 2.5f, 0.0f }, { 9.6f, 0.55f, 0.1f }, { 9.2f, -1.35f, 0.0f }, { 8.4f, -3.1f, -0.15f } };
    static const float dir[4][3] = { { 1.0f, 0.06f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, -0.05f, 0.0f }, { 1.0f, -0.13f, 0.0f } };
    static const float len[4][3] = { { 3.9f, 2.3f, 2.0f }, { 4.4f, 2.7f, 2.1f }, { 4.1f, 2.6f, 2.0f }, { 3.2f, 1.9f, 1.8f } };
    for (int f = 0; f < 4; f++) {
        int j0 = VRH_INDEX_META + f * 5;
        float d[3] = { dir[f][0], dir[f][1], dir[f][2] };
        v_norm(d);
        memcpy(J[j0], base[f], 12), memcpy(J[j0 + 1], knuck[f], 12);
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++) J[j0 + 2 + k][a] = J[j0 + 1 + k][a] + d[a] * len[f][k];
    }
    // the thumb: from the wrist's side, forward and out, a little under the palm
    const float tm[3] = { 1.4f, 1.9f, -0.8f }, tp[3] = { 4.4f, 4.4f, -1.5f };
    float d1[3] = { 0.62f, 0.74f, -0.25f }, d2[3] = { 0.70f, 0.66f, -0.25f };
    v_norm(d1), v_norm(d2);
    memcpy(J[VRH_THUMB_META], tm, 12), memcpy(J[VRH_THUMB_META + 1], tp, 12);
    for (int a = 0; a < 3; a++) J[4][a] = J[3][a] + d1[a] * 3.2f, J[5][a] = J[4][a] + d2[a] * 2.7f;
    // the palm joint: the middle of the middle finger's metacarpal
    for (int a = 0; a < 3; a++) J[VRH_PALM][a] = (J[VRH_MIDDLE_META][a] + J[VRH_MIDDLE_META + 1][a]) * 0.5f;
    g_J_made = true;
}

// the thumb's bones' local rotations for its angles (degrees): opposition, then the bends of its 3 bones
static void thumb_rots(const float a[4], int k, float q[4]) {
    float dt[3];
    v_sub(g_J[VRH_THUMB_META + 1], g_J[VRH_THUMB_META], dt), v_norm(dt);
    const float toward[3] = { 0.0f, -0.6f, -0.8f };
    float flex[3], qf[4];
    v_cross(dt, toward, flex), v_norm(flex);
    q_axis(flex, a[1 + k] * VRH_DEG, qf);
    if (k == 0) {
        const float opp[3] = { -1.0f, -0.25f, 0.0f };
        float qo[4];
        q_axis(opp, a[0] * VRH_DEG, qo);
        q_mul(qo, qf, q);
    } else
        memcpy(q, qf, 16);
}
// a chain's end from the wrist (its bones' local rotations; the wrist turned by nothing)
static void chain_tip(const int* js, int n, const float (*ql)[4], float out[3]) {
    float qw[4] = { 0, 0, 0, 1 }, p[3];
    memcpy(p, g_J[VRH_WRIST], 12);
    int prev = VRH_WRIST;
    for (int i = 0; i < n; i++) {
        float off[3], r[3], q2[4];
        v_sub(g_J[js[i]], g_J[prev], off);
        q_rot(qw, off, r);
        for (int a = 0; a < 3; a++) p[a] += r[a];
        if (i < n - 1) q_mul(qw, ql[i], q2), memcpy(qw, q2, 16);
        prev = js[i];
    }
    memcpy(out, p, 12);
}
static float g_pinch_q[3][4]; // (the pinch's thumb: its 3 bones' local rotations)
static bool g_pinch_made;
static void pose_rot(int pose, int j, float q[4]);
// the pinch: the thumb bent until its chord (base -> tip) is as long as the way to the index's tip, then its metacarpal
// turned to aim the tip there (the pads 1.3 cm apart: touching)
static void make_pinch(void) {
    if (g_pinch_made) return;
    g_pinch_made = true;
    const int ij[5] = { VRH_INDEX_META, VRH_INDEX_META + 1, VRH_INDEX_META + 2, VRH_INDEX_META + 3, VRH_INDEX_META + 4 };
    float iq[5][4], itip[3];
    for (int i = 0; i < 5; i++) pose_rot(VRH_POSE_PINCH, ij[i], iq[i]);
    chain_tip(ij, 5, (const float(*)[4])iq, itip);
    const float* cmc = g_J[VRH_THUMB_META];
    float back[3], target[3], d[3];
    v_sub(cmc, itip, back), v_norm(back);
    for (int a = 0; a < 3; a++) target[a] = itip[a] + back[a] * 1.3f;
    v_sub(target, cmc, d);
    float want = v_norm(d);
    const int tj[4] = { VRH_THUMB_META, VRH_THUMB_META + 1, VRH_THUMB_META + 2, VRH_THUMB_META + 3 };
    float tq[4][4], ttip[3], v[3];
    float bestf = 0.0f, beste = 1e9f;
    for (float f = 0.0f; f <= 100.0f; f += 0.5f) {
        const float a[4] = { 0.0f, f * 0.25f, f, f * 0.8f };
        for (int k = 0; k < 3; k++) thumb_rots(a, k, tq[k]);
        tq[3][0] = tq[3][1] = tq[3][2] = 0.0f, tq[3][3] = 1.0f;
        chain_tip(tj, 4, (const float(*)[4])tq, ttip);
        v_sub(ttip, cmc, v);
        float e = fabsf(sqrtf(v_dot(v, v)) - want);
        if (e < beste) beste = e, bestf = f;
    }
    const float a[4] = { 0.0f, bestf * 0.25f, bestf, bestf * 0.8f };
    for (int k = 0; k < 3; k++) thumb_rots(a, k, tq[k]);
    tq[3][0] = tq[3][1] = tq[3][2] = 0.0f, tq[3][3] = 1.0f;
    chain_tip(tj, 4, (const float(*)[4])tq, ttip);
    v_sub(ttip, cmc, v), v_norm(v);
    float aim[4];
    q_from_to(v, d, aim);
    q_mul(aim, tq[0], g_pinch_q[0]);
    memcpy(g_pinch_q[1], tq[1], 16), memcpy(g_pinch_q[2], tq[2], 16);
}

// a pose's local rotation of a joint's bone (the right hand, FBX space; the bind's rotations are all identity)
static void pose_rot(int pose, int j, float q[4]) {
    q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
    if (pose == VRH_POSE_OPEN || j < 2) return;
    // per finger (index..little): metacarpal, proximal, intermediate, distal (degrees, curling toward the palm)
    static const float curl[VRH_POSES][4][4] = {
        { { 0 } },
        { { 0, 12, 18, 10 }, { 0, 16, 22, 12 }, { 2, 20, 25, 14 }, { 4, 24, 28, 15 } },  // Relaxed
        { { 0, 85, 100, 60 }, { 0, 88, 100, 60 }, { 4, 90, 100, 60 }, { 8, 90, 100, 60 } }, // Fist
        { { 0, 55, 65, 35 }, { 0, 58, 68, 36 }, { 3, 60, 70, 38 }, { 6, 62, 72, 40 } },  // Grip (around a controller)
        { { 0, 4, 4, 0 }, { 0, 88, 100, 60 }, { 4, 90, 100, 60 }, { 8, 90, 100, 60 } },   // Point
        { { 0, 52, 58, 28 }, { 0, 34, 42, 20 }, { 2, 38, 46, 22 }, { 4, 42, 50, 24 } },   // Pinch (the index bent to meet the thumb)
        { { 0, 85, 100, 60 }, { 0, 88, 100, 60 }, { 4, 90, 100, 60 }, { 8, 90, 100, 60 } }, // Thumbs Up
    };
    // the thumb: opposition (its metacarpal rolled under the palm), then its three bends
    static const float thumb[VRH_POSES][4] = {
        { 0 }, { 8, 0, 10, 12 }, { 40, 15, 45, 40 }, { 30, 5, 25, 25 }, { 40, 12, 40, 35 }, { 35, 10, 20, 15 }, { -5, 0, 0, 0 },
    };
    if (j >= VRH_INDEX_META) {
        int f = (j - VRH_INDEX_META) / 5, k = (j - VRH_INDEX_META) % 5;
        if (k > 3) return;
        const float y[3] = { 0, 1, 0 }; // (the fingers along +X bend toward -Z: about +Y)
        q_axis(y, curl[pose][f][k] * VRH_DEG, q);
        return;
    }
    // the thumb: its axes from the bind's directions (toward the palm and the little finger); the pinch's searched
    int k = j - VRH_THUMB_META;
    if (k > 2) return;
    if (pose == VRH_POSE_PINCH) {
        make_pinch();
        memcpy(q, g_pinch_q[k], 16);
        return;
    }
    thumb_rots(thumb[pose], k, q);
}

// the joints' world places at a pose (FBX space, the right hand)
static void pose_joints(int pose, float out[VRH_JOINTS][3], float qw[VRH_JOINTS][4]) {
    make_joints();
    for (int j = 0; j < VRH_JOINTS; j++) { // (parents come first except the palm: done after the wrist below)
        int order = j == 0 ? 1 : j == 1 ? 0 : j; // wrist, palm, then the rest
        int b = order, p = g_parent[b];
        float ql[4];
        pose_rot(pose, b, ql);
        if (p < 0) {
            memcpy(out[b], g_J[b], 12), memcpy(qw[b], ql, 16);
            continue;
        }
        float off[3], r[3];
        v_sub(g_J[b], g_J[p], off);
        q_rot(qw[p], off, r);
        for (int a = 0; a < 3; a++) out[b][a] = out[p][a] + r[a];
        q_mul(qw[p], ql, qw[b]);
    }
}

void vrh_base_joints(int side, int pose, float out[VRH_JOINTS][3]) {
    float qw[VRH_JOINTS][4];
    pose_joints(pose, out, qw);
    for (int j = 0; j < VRH_JOINTS; j++) { // the left: the mirror; then the engine's space (Y mirrored): left keeps Y
        if (!side) out[j][1] = -out[j][1];
        out[j][1] = -out[j][1];
    }
}

// --- the mesh ---
typedef struct HV {
    float p[3], n[3], uv[2];
    int8_t b[4];
    float w[4];
} HV;
typedef struct HMesh {
    HV* v;
    uint32_t nv, cv;
    uint32_t (*t)[3];
    uint32_t nt, ct;
    bool oom;
} HMesh;
static uint32_t hv_add(HMesh* m, const float p[3], float u, float v) {
    if (m->nv == m->cv) {
        uint32_t c = m->cv ? m->cv * 2 : 1024;
        HV* nv = realloc(m->v, sizeof(HV) * c);
        if (!nv) return m->oom = true, 0;
        m->v = nv, m->cv = c;
    }
    HV* h = &m->v[m->nv];
    memset(h, 0, sizeof *h);
    memcpy(h->p, p, 12), h->uv[0] = u, h->uv[1] = v;
    for (int k = 0; k < 4; k++) h->b[k] = -1;
    return m->nv++;
}
static void ht_add(HMesh* m, uint32_t a, uint32_t b, uint32_t c) {
    if (m->nt == m->ct) {
        uint32_t cc = m->ct ? m->ct * 2 : 2048;
        void* nt = realloc(m->t, sizeof(*m->t) * cc);
        if (!nt) {
            m->oom = true;
            return;
        }
        m->t = nt, m->ct = cc;
    }
    m->t[m->nt][0] = a, m->t[m->nt][1] = b, m->t[m->nt][2] = c, m->nt++;
}
// a weight on a vertex (the 4 strongest kept)
static void hw_add(HV* v, int bone, float w) {
    if (w <= 1e-4f || bone < 0) return;
    for (int k = 0; k < 4; k++)
        if (v->b[k] == bone) {
            v->w[k] += w;
            return;
        }
    int lo = 0;
    for (int k = 1; k < 4; k++)
        if (v->w[k] < v->w[lo]) lo = k;
    if (v->b[lo] < 0 || v->w[lo] < w) v->b[lo] = (int8_t)bone, v->w[lo] = w;
}
static float smooth01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}
// one part's faces turned outward (its signed volume positive) and its smooth normals
static void part_finish(HMesh* m, uint32_t v0, uint32_t t0) {
    double vol = 0.0;
    for (uint32_t t = t0; t < m->nt; t++) {
        const float *a = m->v[m->t[t][0]].p, *b = m->v[m->t[t][1]].p, *c = m->v[m->t[t][2]].p;
        float bc[3];
        v_cross(b, c, bc);
        vol += v_dot(a, bc);
    }
    if (vol < 0.0)
        for (uint32_t t = t0; t < m->nt; t++) {
            uint32_t s = m->t[t][1];
            m->t[t][1] = m->t[t][2], m->t[t][2] = s;
        }
    for (uint32_t t = t0; t < m->nt; t++) {
        float e1[3], e2[3], n[3];
        v_sub(m->v[m->t[t][1]].p, m->v[m->t[t][0]].p, e1), v_sub(m->v[m->t[t][2]].p, m->v[m->t[t][0]].p, e2);
        v_cross(e1, e2, n);
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++) m->v[m->t[t][k]].n[a] += n[a];
    }
    for (uint32_t i = v0; i < m->nv; i++) {
        v_norm(m->v[i].n);
        float s = 0.0f;
        for (int k = 0; k < 4; k++) s += m->v[i].b[k] >= 0 ? m->v[i].w[k] : 0.0f;
        for (int k = 0; k < 4; k++) m->v[i].w[k] = s > 0.0f && m->v[i].b[k] >= 0 ? m->v[i].w[k] / s : 0.0f;
    }
}
// rings between two ring starts (n around), a quad each
static void ring_quads(HMesh* m, uint32_t a, uint32_t b, int n) {
    for (int j = 0; j < n; j++) {
        int k = (j + 1) % n;
        ht_add(m, a + (uint32_t)j, b + (uint32_t)j, b + (uint32_t)k);
        ht_add(m, a + (uint32_t)j, b + (uint32_t)k, a + (uint32_t)k);
    }
}
static void ring_fan(HMesh* m, uint32_t ring, uint32_t center, int n) {
    for (int j = 0; j < n; j++) ht_add(m, ring + (uint32_t)j, ring + (uint32_t)((j + 1) % n), center);
}

#define TUBE_SEG 12
// a finger (or the thumb, or the wrist): a tube along the points, its radius at each, its back (the nail's side) toward
// up, bone[k] the bone of segment k (to point k + 1), blended over 0.5 cm at the joints; a round cap at the end
static void tube(HMesh* m, const float (*pts)[3], const float* rad, int np, const float up[3], float squash, const int* bone, float u0, bool cap_end) {
    uint32_t v0 = m->nv, t0 = m->nt;
    float seglen[8], total = 0.0f;
    for (int i = 0; i + 1 < np; i++) {
        float d[3];
        v_sub(pts[i + 1], pts[i], d);
        seglen[i] = sqrtf(v_dot(d, d)), total += seglen[i];
    }
    int nr = (int)(total / 0.45f) + 2;
    uint32_t first = 0, last = 0;
    float tip_r = rad[np - 1], tdir[3] = { 0 }, tc[3] = { 0 }, tU[3] = { 0 }, tV[3] = { 0 };
    for (int r = 0; r < nr; r++) {
        float s = total * (float)r / (float)(nr - 1);
        int i = 0;
        float acc = 0.0f;
        while (i < np - 2 && acc + seglen[i] < s) acc += seglen[i], i++;
        float t = seglen[i] > 0.0f ? (s - acc) / seglen[i] : 0.0f;
        t = t > 1.0f ? 1.0f : t;
        float c[3], T[3];
        for (int a = 0; a < 3; a++) c[a] = pts[i][a] + (pts[i + 1][a] - pts[i][a]) * t;
        // the tangent: the segments' directions blended near the joints (no kink in the rings)
        float d0[3], d1[3];
        v_sub(pts[i + 1], pts[i], d0), v_norm(d0);
        memcpy(T, d0, 12);
        if (t < 0.25f && i > 0) {
            v_sub(pts[i], pts[i - 1], d1), v_norm(d1);
            float k = 0.5f - t * 2.0f;
            for (int a = 0; a < 3; a++) T[a] = d0[a] * (1.0f - k) + d1[a] * k;
        } else if (t > 0.75f && i + 2 < np) {
            v_sub(pts[i + 2], pts[i + 1], d1), v_norm(d1);
            float k = (t - 0.75f) * 2.0f;
            for (int a = 0; a < 3; a++) T[a] = d0[a] * (1.0f - k) + d1[a] * k;
        }
        v_norm(T);
        float V[3], U[3];
        float dd = v_dot(up, T);
        for (int a = 0; a < 3; a++) V[a] = up[a] - T[a] * dd;
        v_norm(V);
        v_cross(T, V, U);
        float rr = rad[i] + (rad[i + 1] - rad[i]) * t;
        uint32_t ring = m->nv;
        if (r == 0) first = ring;
        last = ring;
        for (int j = 0; j < TUBE_SEG; j++) {
            float th = 6.2831853f * (float)j / (float)TUBE_SEG, p[3];
            for (int a = 0; a < 3; a++) p[a] = c[a] + U[a] * cosf(th) * rr + V[a] * sinf(th) * rr * squash;
            uint32_t vi = hv_add(m, p, u0 + (float)j / (float)TUBE_SEG * 0.1f, s / 20.0f);
            if (m->oom) return;
            // the weights: this segment's bone, the neighbor's near a joint
            HV* h = &m->v[vi];
            float into = s - acc, left = seglen[i] - into;
            if (i > 0 && into < 0.5f) {
                float k = smooth01(-0.5f, 0.5f, into);
                hw_add(h, bone[i], k), hw_add(h, bone[i - 1], 1.0f - k);
            } else if (i + 2 < np && left < 0.5f) {
                float k = smooth01(-0.5f, 0.5f, left);
                hw_add(h, bone[i], k), hw_add(h, bone[i + 1], 1.0f - k);
            } else
                hw_add(h, bone[i], 1.0f);
        }
        if (r > 0) ring_quads(m, ring - TUBE_SEG, ring, TUBE_SEG);
        if (r == nr - 1) memcpy(tdir, T, 12), memcpy(tc, c, 12), memcpy(tU, U, 12), memcpy(tV, V, 12);
    }
    // the start: closed flat (inside the palm); the end: a dome
    float cs[3];
    memcpy(cs, pts[0], 12);
    uint32_t cc = hv_add(m, cs, u0, 0.0f);
    hw_add(&m->v[cc], bone[0], 1.0f);
    ring_fan(m, first, cc, TUBE_SEG);
    if (cap_end) {
        uint32_t prev = last;
        for (int r = 1; r <= 3; r++) {
            float a = (float)r / 4.0f * 1.5707963f, rr = tip_r * cosf(a), adv = tip_r * sinf(a);
            uint32_t ring = m->nv;
            for (int j = 0; j < TUBE_SEG; j++) {
                float th = 6.2831853f * (float)j / (float)TUBE_SEG, p[3];
                for (int a2 = 0; a2 < 3; a2++) p[a2] = tc[a2] + tdir[a2] * adv + tU[a2] * cosf(th) * rr + tV[a2] * sinf(th) * rr * squash;
                uint32_t vi = hv_add(m, p, u0 + (float)j / (float)TUBE_SEG * 0.1f, (total + adv) / 20.0f);
                hw_add(&m->v[vi], bone[np - 2], 1.0f);
            }
            ring_quads(m, prev, ring, TUBE_SEG);
            prev = ring;
        }
        float tip[3];
        for (int a = 0; a < 3; a++) tip[a] = tc[a] + tdir[a] * tip_r * 0.98f;
        uint32_t ti = hv_add(m, tip, u0, (total + tip_r) / 20.0f);
        hw_add(&m->v[ti], bone[np - 2], 1.0f);
        ring_fan(m, prev, ti, TUBE_SEG);
    } else {
        uint32_t ce = hv_add(m, pts[np - 1], u0, total / 20.0f);
        hw_add(&m->v[ce], bone[np - 2], 1.0f);
        ring_fan(m, last, ce, TUBE_SEG);
    }
    part_finish(m, v0, t0);
}

// the palm: a rounded slab from the wrist to the knuckles, wider at the knuckles, weighted to the wrist near it and to
// the metacarpals (the nearest two across), the knuckles to the fingers, the thumb's side to its metacarpal
static void palm(HMesh* m) {
    uint32_t v0 = m->nv, t0 = m->nt;
    const int NX = 18, NS = 24;
    const float x0 = 0.3f, x1 = 9.9f;
    uint32_t prev = 0;
    for (int i = 0; i < NX; i++) {
        float t = (float)i / (float)(NX - 1), x = x0 + (x1 - x0) * t;
        float e = fabsf(2.0f * t - 1.0f), f = powf(1.0f - powf(e, 5.0f), 0.2f);
        if (i == 0 || i == NX - 1) f = 0.0f;
        float hy = (3.0f + 1.2f * smooth01(0.0f, 8.5f, x) - 0.2f * smooth01(8.5f, 9.9f, x)) * f;
        float hz = (1.25f + 0.2f * smooth01(0.0f, 4.0f, x) - 0.35f * smooth01(5.0f, 9.9f, x)) * f;
        float yc = -0.3f * smooth01(0.0f, 9.0f, x), zc = 0.1f * smooth01(0.0f, 9.0f, x);
        uint32_t ring = m->nv;
        for (int j = 0; j < NS; j++) {
            float th = 6.2831853f * (float)j / (float)NS, cy = cosf(th), sz = sinf(th);
            float py = (cy < 0 ? -1.0f : 1.0f) * powf(fabsf(cy), 2.0f / 3.0f), pz = (sz < 0 ? -1.0f : 1.0f) * powf(fabsf(sz), 2.0f / 3.0f);
            float thenar = py > 0.0f ? 1.0f + 0.32f * smooth01(0.3f, 2.2f, x) * (1.0f - smooth01(4.5f, 7.5f, x)) * py : 1.0f;
            float p[3] = { x, yc + py * hy * thenar, zc + pz * hz * (pz < 0 ? 1.05f + 0.25f * (thenar - 1.0f) : 1.0f) };
            uint32_t vi = hv_add(m, p, 0.5f + p[1] / 20.0f, p[0] / 20.0f);
            if (m->oom) return;
            HV* h = &m->v[vi];
            float ww = 1.0f - smooth01(1.5f, 4.0f, x);
            hw_add(h, VRH_WRIST, ww);
            // the metacarpals across: each one's line at this x
            float yl[4];
            for (int fg = 0; fg < 4; fg++) {
                const float *a = g_J[VRH_INDEX_META + fg * 5], *b = g_J[VRH_INDEX_META + fg * 5 + 1];
                float k = (x - a[0]) / (b[0] - a[0]);
                k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
                yl[fg] = a[1] + (b[1] - a[1]) * k;
            }
            int lo = 3;
            while (lo > 0 && p[1] > yl[lo - 1]) lo--; // (yl falls from index to little)
            int hi = lo > 0 ? lo - 1 : 0;
            float span = yl[hi] - yl[lo], k = span > 1e-3f ? (p[1] - yl[lo]) / span : 0.0f;
            k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
            float rest = 1.0f - ww, kn = smooth01(8.3f, 9.9f, x) * 0.6f;
            float thumb = smooth01(0.8f, 2.5f, p[1] - yl[0]) * (1.0f - smooth01(5.0f, 8.0f, x)) * 0.7f;
            hw_add(h, VRH_THUMB_META, rest * thumb);
            rest *= 1.0f - thumb;
            int fl = VRH_INDEX_META + lo * 5, fh = VRH_INDEX_META + hi * 5;
            hw_add(h, fl, rest * (1.0f - k) * (1.0f - kn)), hw_add(h, fl + 1, rest * (1.0f - k) * kn);
            hw_add(h, fh, rest * k * (1.0f - kn)), hw_add(h, fh + 1, rest * k * kn);
        }
        if (i > 0) ring_quads(m, prev, ring, NS);
        prev = ring;
    }
    part_finish(m, v0, t0);
}

static bool build_mesh(HMesh* m) {
    make_joints();
    memset(m, 0, sizeof *m);
    palm(m);
    const float up[3] = { 0, 0, 1 };
    // the wrist's stub (the forearm cut 4 cm back)
    {
        const float pts[2][3] = { { -4.0f, 0.0f, 0.0f }, { 1.6f, 0.0f, 0.05f } }, rad[2] = { 2.55f, 2.7f };
        const int bone[1] = { VRH_WRIST };
        tube(m, pts, rad, 2, up, 0.72f, bone, 0.0f, false);
    }
    for (int f = 0; f < 4; f++) { // the fingers, from inside the palm to the tips
        int j0 = VRH_INDEX_META + f * 5;
        float pts[5][3], d[3];
        v_sub(g_J[j0 + 2], g_J[j0 + 1], d), v_norm(d);
        for (int a = 0; a < 3; a++) pts[0][a] = g_J[j0 + 1][a] - d[a] * 1.6f;
        for (int k = 1; k < 5; k++) memcpy(pts[k], g_J[j0 + k], 12);
        const float w = f == 3 ? 0.85f : 1.0f;
        const float rad[5] = { 1.0f * w, 0.95f * w, 0.85f * w, 0.75f * w, 0.66f * w };
        const int bone[4] = { j0, j0 + 1, j0 + 2, j0 + 3 };
        tube(m, pts, rad, 5, up, 0.85f, bone, 0.2f + 0.1f * (float)f, true);
    }
    { // the thumb: from its base in the palm's heel, its nail turned out
        float pts[5][3], d[3];
        v_sub(g_J[VRH_THUMB_META + 1], g_J[VRH_THUMB_META], d), v_norm(d);
        for (int a = 0; a < 3; a++) pts[0][a] = g_J[VRH_THUMB_META][a] - d[a] * 1.0f; // (from inside the heel)
        for (int k = 0; k < 4; k++) memcpy(pts[k + 1], g_J[VRH_THUMB_META + k], 12);
        const float rad[5] = { 1.5f, 1.45f, 1.0f, 0.88f, 0.78f };
        const int bone[4] = { VRH_THUMB_META, VRH_THUMB_META, VRH_THUMB_META + 1, VRH_THUMB_META + 2 };
        const float tup[3] = { -0.3f, 0.45f, 0.85f };
        tube(m, pts, rad, 5, tup, 0.85f, bone, 0.7f, true);
    }
    return !m->oom;
}

// --- the FBX ---
#define KTIME_SEC 46186158000LL
#define POSE_FRAMES 10
const char* vrh_file_name(int side) { return side ? "RealEngine_BaseHand_R.fbx" : "RealEngine_BaseHand_L.fbx"; }

static void matrix_t(double m[16], const float t[3]) { // (column-major: the translation at 12..14)
    memset(m, 0, sizeof(double) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0;
    m[12] = t[0], m[13] = t[1], m[14] = t[2];
}

bool vrh_export_fbx(int side, const char* path, char* err, size_t cap) {
    HMesh m;
    if (!build_mesh(&m)) {
        snprintf(err, cap, "out of memory");
        free(m.v), free(m.t);
        return false;
    }
    const char* sfx = side ? "_R" : "_L";
    float J[VRH_JOINTS][3];
    memcpy(J, g_J, sizeof J);
    if (!side) { // the left: the mirror (positions, normals, the faces' winding)
        for (int j = 0; j < VRH_JOINTS; j++) J[j][1] = -J[j][1];
        for (uint32_t i = 0; i < m.nv; i++) m.v[i].p[1] = -m.v[i].p[1], m.v[i].n[1] = -m.v[i].n[1];
        for (uint32_t t = 0; t < m.nt; t++) {
            uint32_t s = m.t[t][1];
            m.t[t][1] = m.t[t][2], m.t[t][2] = s;
        }
    }
    FbxW* w = fbxw_new();
    if (!w) return snprintf(err, cap, "out of memory"), free(m.v), free(m.t), false;
    // ids: 0 the scene; the rest counted
    int64_t id = 1000000;
    const int64_t id_null = id++, id_null_attr = id++, id_mesh_model = id++, id_geom = id++, id_mat = id++, id_skin = id++, id_pose = id++;
    int64_t id_bone[VRH_JOINTS], id_attr[VRH_JOINTS], id_cluster[VRH_JOINTS];
    for (int j = 0; j < VRH_JOINTS; j++) id_bone[j] = id++, id_attr[j] = id++, id_cluster[j] = id++;
    int64_t id_stack[VRH_POSES], id_layer[VRH_POSES];
    for (int p = 0; p < VRH_POSES; p++) id_stack[p] = id++, id_layer[p] = id++;
    const int64_t id_curves0 = id; // (each pose x bone: a curve node and its 3 curves)
    const int64_t stop = KTIME_SEC * POSE_FRAMES / 30;
    char nm[96];

    // header
    fbxw_open(w, "FBXHeaderExtension");
    fbxw_node_i(w, "FBXHeaderVersion", 1003), fbxw_node_i(w, "FBXVersion", 7400), fbxw_node_i(w, "EncryptionType", 0);
    fbxw_open(w, "CreationTimeStamp");
    fbxw_node_i(w, "Version", 1000), fbxw_node_i(w, "Year", 1970), fbxw_node_i(w, "Month", 1), fbxw_node_i(w, "Day", 1);
    fbxw_node_i(w, "Hour", 10), fbxw_node_i(w, "Minute", 0), fbxw_node_i(w, "Second", 0), fbxw_node_i(w, "Millisecond", 0);
    fbxw_close(w);
    fbxw_node_s(w, "Creator", "Real Engine (VR base hand)");
    fbxw_open(w, "SceneInfo"), fbxw_objname(w, "GlobalInfo", "SceneInfo"), fbxw_s(w, "UserData");
    fbxw_node_s(w, "Type", "UserData"), fbxw_node_i(w, "Version", 100);
    fbxw_open(w, "MetaData"), fbxw_node_i(w, "Version", 100), fbxw_node_s(w, "Title", "Real Engine VR base hand");
    fbxw_node_s(w, "Subject", ""), fbxw_node_s(w, "Author", "Real Engine"), fbxw_node_s(w, "Keywords", "");
    fbxw_node_s(w, "Revision", ""), fbxw_node_s(w, "Comment", ""), fbxw_close(w);
    fbxw_close(w);
    fbxw_close(w);
    static const uint8_t file_id[16] = { 0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2, 0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1 };
    fbxw_open(w, "FileId"), fbxw_raw(w, file_id, 16), fbxw_close(w);
    fbxw_node_s(w, "CreationTime", "1970-01-01 10:00:00:000");
    fbxw_node_s(w, "Creator", "Real Engine (VR base hand)");
    // the axes: Z up, cm (the importers convert to theirs)
    fbxw_open(w, "GlobalSettings");
    fbxw_node_i(w, "Version", 1000);
    fbxw_open(w, "Properties70");
    fbxw_p_int(w, "UpAxis", 2), fbxw_p_int(w, "UpAxisSign", 1), fbxw_p_int(w, "FrontAxis", 1), fbxw_p_int(w, "FrontAxisSign", 1);
    fbxw_p_int(w, "CoordAxis", 0), fbxw_p_int(w, "CoordAxisSign", 1), fbxw_p_int(w, "OriginalUpAxis", 2), fbxw_p_int(w, "OriginalUpAxisSign", 1);
    fbxw_p_double(w, "UnitScaleFactor", 1.0), fbxw_p_double(w, "OriginalUnitScaleFactor", 1.0);
    fbxw_p_enum(w, "TimeMode", 6); // (30 fps)
    fbxw_p_ktime(w, "TimeSpanStart", 0), fbxw_p_ktime(w, "TimeSpanStop", stop);
    fbxw_close(w);
    fbxw_close(w);
    fbxw_open(w, "Documents");
    fbxw_node_i(w, "Count", 1);
    fbxw_open(w, "Document"), fbxw_l(w, 999999), fbxw_s(w, "Scene"), fbxw_s(w, "Scene");
    fbxw_open(w, "Properties70");
    fbxw_open(w, "P"), fbxw_s(w, "SourceObject"), fbxw_s(w, "object"), fbxw_s(w, ""), fbxw_s(w, ""), fbxw_close(w);
    fbxw_p_str(w, "ActiveAnimStackName", side ? "Open_R" : "Open_L");
    fbxw_close(w);
    fbxw_open(w, "RootNode"), fbxw_l(w, 0), fbxw_close(w);
    fbxw_close(w);
    fbxw_close(w);
    fbxw_open(w, "References"), fbxw_close(w);
    // the definitions (counts)
    int ncurve_nodes = VRH_POSES * VRH_JOINTS;
    struct { const char* type; int count; } defs[] = {
        { "GlobalSettings", 1 }, { "NodeAttribute", 1 + VRH_JOINTS }, { "Model", 2 + VRH_JOINTS }, { "Geometry", 1 }, { "Material", 1 },
        { "Deformer", 1 + VRH_JOINTS }, { "Pose", 1 }, { "AnimationStack", VRH_POSES }, { "AnimationLayer", VRH_POSES },
        { "AnimationCurveNode", ncurve_nodes }, { "AnimationCurve", ncurve_nodes * 3 },
    };
    int total = 0;
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) total += defs[i].count;
    fbxw_open(w, "Definitions");
    fbxw_node_i(w, "Version", 100), fbxw_node_i(w, "Count", total);
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        fbxw_open(w, "ObjectType"), fbxw_s(w, defs[i].type);
        fbxw_node_i(w, "Count", defs[i].count);
        // the animation's templates (the FBX SDK makes a curve node's channels from its "d" compound: without it Maya
        // dropped every curve)
        if (!strcmp(defs[i].type, "AnimationCurveNode")) {
            fbxw_open(w, "PropertyTemplate"), fbxw_s(w, "FbxAnimCurveNode"), fbxw_open(w, "Properties70");
            fbxw_open(w, "P"), fbxw_s(w, "d"), fbxw_s(w, "Compound"), fbxw_s(w, ""), fbxw_s(w, ""), fbxw_close(w);
            fbxw_close(w), fbxw_close(w);
        } else if (!strcmp(defs[i].type, "AnimationLayer")) {
            fbxw_open(w, "PropertyTemplate"), fbxw_s(w, "FbxAnimLayer"), fbxw_open(w, "Properties70");
            fbxw_open(w, "P"), fbxw_s(w, "Weight"), fbxw_s(w, "Number"), fbxw_s(w, ""), fbxw_s(w, "A"), fbxw_d(w, 100.0), fbxw_close(w);
            fbxw_close(w), fbxw_close(w);
        } else if (!strcmp(defs[i].type, "AnimationStack")) {
            fbxw_open(w, "PropertyTemplate"), fbxw_s(w, "FbxAnimStack"), fbxw_open(w, "Properties70");
            fbxw_p_str(w, "Description", "");
            fbxw_p_ktime(w, "LocalStart", 0), fbxw_p_ktime(w, "LocalStop", 0), fbxw_p_ktime(w, "ReferenceStart", 0), fbxw_p_ktime(w, "ReferenceStop", 0);
            fbxw_close(w), fbxw_close(w);
        }
        fbxw_close(w);
    }
    fbxw_close(w);

    // --- the objects ---
    fbxw_open(w, "Objects");
    snprintf(nm, sizeof nm, "BaseHand%s", sfx);
    fbxw_open(w, "NodeAttribute"), fbxw_l(w, id_null_attr), fbxw_objname(w, nm, "NodeAttribute"), fbxw_s(w, "Null");
    fbxw_node_s(w, "TypeFlags", "Null"), fbxw_close(w);
    fbxw_open(w, "Model"), fbxw_l(w, id_null), fbxw_objname(w, nm, "Model"), fbxw_s(w, "Null");
    fbxw_node_i(w, "Version", 232);
    fbxw_open(w, "Properties70"), fbxw_p_enum(w, "InheritType", 1), fbxw_close(w);
    fbxw_node_s(w, "Culling", "CullingOff");
    fbxw_close(w);
    for (int j = 0; j < VRH_JOINTS; j++) {
        snprintf(nm, sizeof nm, "%s%s", g_vrh_joint_names[j], sfx);
        fbxw_open(w, "NodeAttribute"), fbxw_l(w, id_attr[j]), fbxw_objname(w, nm, "NodeAttribute"), fbxw_s(w, "LimbNode");
        fbxw_open(w, "Properties70"), fbxw_p_double(w, "Size", 1.0), fbxw_close(w);
        fbxw_node_s(w, "TypeFlags", "Skeleton");
        fbxw_close(w);
        fbxw_open(w, "Model"), fbxw_l(w, id_bone[j]), fbxw_objname(w, nm, "Model"), fbxw_s(w, "LimbNode");
        fbxw_node_i(w, "Version", 232);
        float off[3];
        if (g_parent[j] >= 0) v_sub(J[j], J[g_parent[j]], off);
        else memcpy(off, J[j], 12);
        fbxw_open(w, "Properties70");
        fbxw_p_enum(w, "InheritType", 1), fbxw_p_int(w, "DefaultAttributeIndex", 0);
        fbxw_p_vec(w, "Lcl Translation", "Lcl Translation", "", "A", off[0], off[1], off[2]);
        fbxw_p_vec(w, "Lcl Rotation", "Lcl Rotation", "", "A+", 0, 0, 0); // ("+": animated, as the FBX SDK writes it)
        fbxw_p_vec(w, "Lcl Scaling", "Lcl Scaling", "", "A", 1, 1, 1);
        fbxw_close(w);
        fbxw_node_i(w, "MultiLayer", 0), fbxw_node_i(w, "MultiTake", 0);
        fbxw_open(w, "Shading"), fbxw_c(w, 1), fbxw_close(w);
        fbxw_node_s(w, "Culling", "CullingOff");
        fbxw_close(w);
    }
    // the mesh: its model, geometry (positions, normals and UVs by corner), material
    snprintf(nm, sizeof nm, "HandMesh%s", sfx);
    fbxw_open(w, "Model"), fbxw_l(w, id_mesh_model), fbxw_objname(w, nm, "Model"), fbxw_s(w, "Mesh");
    fbxw_node_i(w, "Version", 232);
    fbxw_open(w, "Properties70"), fbxw_p_enum(w, "InheritType", 1), fbxw_p_int(w, "DefaultAttributeIndex", 0), fbxw_close(w);
    fbxw_node_s(w, "Culling", "CullingOff");
    fbxw_close(w);
    {
        double* vtx = malloc(sizeof(double) * 3 * m.nv);
        int32_t* pvi = malloc(sizeof(int32_t) * 3 * m.nt);
        double* nrm = malloc(sizeof(double) * 9 * m.nt);
        double* uv = malloc(sizeof(double) * 2 * m.nv);
        int32_t* uvi = malloc(sizeof(int32_t) * 3 * m.nt);
        if (!vtx || !pvi || !nrm || !uv || !uvi) {
            free(vtx), free(pvi), free(nrm), free(uv), free(uvi), fbxw_free(w), free(m.v), free(m.t);
            return snprintf(err, cap, "out of memory"), false;
        }
        for (uint32_t i = 0; i < m.nv; i++)
            for (int a = 0; a < 3; a++) vtx[i * 3 + a] = m.v[i].p[a];
        for (uint32_t i = 0; i < m.nv; i++) uv[i * 2] = m.v[i].uv[0], uv[i * 2 + 1] = m.v[i].uv[1];
        for (uint32_t t = 0; t < m.nt; t++)
            for (int k = 0; k < 3; k++) {
                uint32_t v = m.t[t][k];
                pvi[t * 3 + k] = k == 2 ? ~(int32_t)v : (int32_t)v;
                uvi[t * 3 + k] = (int32_t)v;
                for (int a = 0; a < 3; a++) nrm[(t * 3 + k) * 3 + a] = m.v[v].n[a];
            }
        snprintf(nm, sizeof nm, "HandMesh%s", sfx);
        fbxw_open(w, "Geometry"), fbxw_l(w, id_geom), fbxw_objname(w, nm, "Geometry"), fbxw_s(w, "Mesh");
        fbxw_open(w, "Vertices"), fbxw_ad(w, vtx, m.nv * 3), fbxw_close(w);
        fbxw_open(w, "PolygonVertexIndex"), fbxw_ai(w, pvi, m.nt * 3), fbxw_close(w);
        fbxw_node_i(w, "GeometryVersion", 124);
        fbxw_open(w, "LayerElementNormal"), fbxw_i(w, 0);
        fbxw_node_i(w, "Version", 102), fbxw_node_s(w, "Name", "");
        fbxw_node_s(w, "MappingInformationType", "ByPolygonVertex"), fbxw_node_s(w, "ReferenceInformationType", "Direct");
        fbxw_open(w, "Normals"), fbxw_ad(w, nrm, m.nt * 9), fbxw_close(w);
        fbxw_close(w);
        fbxw_open(w, "LayerElementUV"), fbxw_i(w, 0);
        fbxw_node_i(w, "Version", 101), fbxw_node_s(w, "Name", "UVMap");
        fbxw_node_s(w, "MappingInformationType", "ByPolygonVertex"), fbxw_node_s(w, "ReferenceInformationType", "IndexToDirect");
        fbxw_open(w, "UV"), fbxw_ad(w, uv, m.nv * 2), fbxw_close(w);
        fbxw_open(w, "UVIndex"), fbxw_ai(w, uvi, m.nt * 3), fbxw_close(w);
        fbxw_close(w);
        fbxw_open(w, "LayerElementMaterial"), fbxw_i(w, 0);
        fbxw_node_i(w, "Version", 101), fbxw_node_s(w, "Name", "");
        fbxw_node_s(w, "MappingInformationType", "AllSame"), fbxw_node_s(w, "ReferenceInformationType", "IndexToDirect");
        int32_t zero = 0;
        fbxw_open(w, "Materials"), fbxw_ai(w, &zero, 1), fbxw_close(w);
        fbxw_close(w);
        fbxw_open(w, "Layer"), fbxw_i(w, 0);
        fbxw_node_i(w, "Version", 100);
        const char* els[3] = { "LayerElementNormal", "LayerElementUV", "LayerElementMaterial" };
        for (int e = 0; e < 3; e++) {
            fbxw_open(w, "LayerElement"), fbxw_node_s(w, "Type", els[e]), fbxw_node_i(w, "TypedIndex", 0), fbxw_close(w);
        }
        fbxw_close(w);
        fbxw_close(w);
        free(vtx), free(pvi), free(nrm), free(uv), free(uvi);
    }
    fbxw_open(w, "Material"), fbxw_l(w, id_mat), fbxw_objname(w, "M_BaseHand", "Material"), fbxw_s(w, "");
    fbxw_node_i(w, "Version", 102), fbxw_node_s(w, "ShadingModel", "phong"), fbxw_node_i(w, "MultiLayer", 0);
    fbxw_open(w, "Properties70");
    fbxw_p_vec(w, "DiffuseColor", "Color", "", "A", 0.80, 0.58, 0.47);
    fbxw_p_double(w, "DiffuseFactor", 1.0);
    fbxw_p_vec(w, "SpecularColor", "Color", "", "A", 0.2, 0.2, 0.2);
    fbxw_p_double(w, "Shininess", 12.0);
    fbxw_close(w);
    fbxw_close(w);
    // the skin: one cluster per bone (the tips and the palm empty: the bones still come along), the bind pose
    fbxw_open(w, "Deformer"), fbxw_l(w, id_skin), fbxw_objname(w, "HandSkin", "Deformer"), fbxw_s(w, "Skin");
    fbxw_node_i(w, "Version", 101), fbxw_node_d(w, "Link_DeformAcuracy", 50.0);
    fbxw_close(w);
    int32_t* idx = malloc(sizeof(int32_t) * m.nv);
    double* wts = malloc(sizeof(double) * m.nv);
    for (int j = 0; j < VRH_JOINTS && idx && wts; j++) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < m.nv; i++)
            for (int k = 0; k < 4; k++)
                if (m.v[i].b[k] == j && m.v[i].w[k] > 0.0f) idx[n] = (int32_t)i, wts[n++] = m.v[i].w[k];
        snprintf(nm, sizeof nm, "%s%s", g_vrh_joint_names[j], sfx);
        fbxw_open(w, "Deformer"), fbxw_l(w, id_cluster[j]), fbxw_objname(w, nm, "SubDeformer"), fbxw_s(w, "Cluster");
        fbxw_node_i(w, "Version", 100);
        fbxw_open(w, "UserData"), fbxw_s(w, ""), fbxw_s(w, ""), fbxw_close(w);
        fbxw_open(w, "Indexes"), fbxw_ai(w, idx, n), fbxw_close(w);
        fbxw_open(w, "Weights"), fbxw_ad(w, wts, n), fbxw_close(w);
        double tl[16], tf[16];
        float neg[3] = { -J[j][0], -J[j][1], -J[j][2] };
        matrix_t(tl, J[j]), matrix_t(tf, neg); // (TransformLink: the bone at the bind; Transform: its inverse x the mesh's)
        fbxw_open(w, "Transform"), fbxw_ad(w, tf, 16), fbxw_close(w);
        fbxw_open(w, "TransformLink"), fbxw_ad(w, tl, 16), fbxw_close(w);
        fbxw_close(w);
    }
    free(idx), free(wts);
    fbxw_open(w, "Pose"), fbxw_l(w, id_pose), fbxw_objname(w, "BindPose", "Pose"), fbxw_s(w, "BindPose");
    fbxw_node_s(w, "Type", "BindPose"), fbxw_node_i(w, "Version", 100), fbxw_node_i(w, "NbPoseNodes", 1 + VRH_JOINTS);
    {
        double mm[16];
        const float z[3] = { 0, 0, 0 };
        matrix_t(mm, z);
        fbxw_open(w, "PoseNode"), fbxw_node_i(w, "Version", 100);
        fbxw_open(w, "Node"), fbxw_l(w, id_mesh_model), fbxw_close(w);
        fbxw_open(w, "Matrix"), fbxw_ad(w, mm, 16), fbxw_close(w);
        fbxw_close(w);
        for (int j = 0; j < VRH_JOINTS; j++) {
            matrix_t(mm, J[j]);
            fbxw_open(w, "PoseNode");
            fbxw_open(w, "Node"), fbxw_l(w, id_bone[j]), fbxw_close(w);
            fbxw_open(w, "Matrix"), fbxw_ad(w, mm, 16), fbxw_close(w);
            fbxw_close(w);
        }
    }
    fbxw_close(w);
    // the poses: a stack each (Open at frame 0 -> the pose at frame 10), every bone's Lcl Rotation keyed
    for (int p = 0; p < VRH_POSES; p++) {
        snprintf(nm, sizeof nm, "%s%s", g_vrh_pose_names[p], sfx); // (by side: both hands' animations in one folder)
        fbxw_open(w, "AnimationStack"), fbxw_l(w, id_stack[p]), fbxw_objname(w, nm, "AnimStack"), fbxw_s(w, "");
        fbxw_open(w, "Properties70");
        fbxw_p_ktime(w, "LocalStart", 0), fbxw_p_ktime(w, "LocalStop", stop);
        fbxw_p_ktime(w, "ReferenceStart", 0), fbxw_p_ktime(w, "ReferenceStop", stop);
        fbxw_close(w);
        fbxw_close(w);
        fbxw_open(w, "AnimationLayer"), fbxw_l(w, id_layer[p]), fbxw_objname(w, "BaseLayer", "AnimLayer"), fbxw_s(w, "");
        fbxw_block(w); // (an empty nested list: the FBX SDK reads the layer only so)
        fbxw_close(w);
        for (int j = 0; j < VRH_JOINTS; j++) {
            float q[4];
            pose_rot(p, j, q);
            if (!side) q[0] = -q[0], q[2] = -q[2]; // (the mirror's rotation)
            double e[3];
            q_euler_xyz(q, e);
            int64_t cn = id_curves0 + (int64_t)(p * VRH_JOINTS + j) * 4;
            fbxw_open(w, "AnimationCurveNode"), fbxw_l(w, cn), fbxw_objname(w, "R", "AnimCurveNode"), fbxw_s(w, "");
            fbxw_open(w, "Properties70");
            const char* ch[3] = { "d|X", "d|Y", "d|Z" }; // (animatable numbers, as the FBX SDK writes them: Maya reads the curves by them)
            for (int a = 0; a < 3; a++) fbxw_open(w, "P"), fbxw_s(w, ch[a]), fbxw_s(w, "Number"), fbxw_s(w, ""), fbxw_s(w, "A"), fbxw_d(w, e[a]), fbxw_close(w);
            fbxw_close(w);
            fbxw_close(w);
            for (int a = 0; a < 3; a++) {
                int64_t kt[2] = { 0, stop };
                float kv[2] = { 0.0f, (float)e[a] };
                int32_t flags = 24836, refc = 2; // (KeyVer 4008 and these flags: as Blender writes them, which Maya reads)
                const float data[4] = { 0.0f, 0.0f, 9.419963346924634e-30f, 0.0f };
                fbxw_open(w, "AnimationCurve"), fbxw_l(w, cn + 1 + a), fbxw_objname(w, "", "AnimCurve"), fbxw_s(w, "");
                fbxw_node_d(w, "Default", 0.0), fbxw_node_i(w, "KeyVer", 4008);
                fbxw_open(w, "KeyTime"), fbxw_al(w, kt, 2), fbxw_close(w);
                fbxw_open(w, "KeyValueFloat"), fbxw_af(w, kv, 2), fbxw_close(w);
                fbxw_open(w, "KeyAttrFlags"), fbxw_ai(w, &flags, 1), fbxw_close(w);
                fbxw_open(w, "KeyAttrDataFloat"), fbxw_af(w, data, 4), fbxw_close(w);
                fbxw_open(w, "KeyAttrRefCount"), fbxw_ai(w, &refc, 1), fbxw_close(w);
                fbxw_close(w);
            }
        }
    }
    fbxw_close(w); // Objects

    // --- the connections ---
    fbxw_open(w, "Connections");
#define OO(a, b) (fbxw_open(w, "C"), fbxw_s(w, "OO"), fbxw_l(w, a), fbxw_l(w, b), fbxw_close(w))
#define OP(a, b, prop) (fbxw_open(w, "C"), fbxw_s(w, "OP"), fbxw_l(w, a), fbxw_l(w, b), fbxw_s(w, prop), fbxw_close(w))
    OO(id_null, 0), OO(id_null_attr, id_null);
    for (int j = 0; j < VRH_JOINTS; j++) {
        OO(id_bone[j], g_parent[j] >= 0 ? id_bone[g_parent[j]] : id_null);
        OO(id_attr[j], id_bone[j]);
    }
    OO(id_mesh_model, 0), OO(id_geom, id_mesh_model), OO(id_mat, id_mesh_model), OO(id_skin, id_geom);
    for (int j = 0; j < VRH_JOINTS; j++) OO(id_cluster[j], id_skin), OO(id_bone[j], id_cluster[j]);
    for (int p = 0; p < VRH_POSES; p++) {
        OO(id_layer[p], id_stack[p]);
        for (int j = 0; j < VRH_JOINTS; j++) {
            int64_t cn = id_curves0 + (int64_t)(p * VRH_JOINTS + j) * 4;
            OO(cn, id_layer[p]);
            OP(cn, id_bone[j], "Lcl Rotation");
            OP(cn + 1, cn, "d|X"), OP(cn + 2, cn, "d|Y"), OP(cn + 3, cn, "d|Z");
        }
    }
#undef OO
#undef OP
    fbxw_close(w);
    fbxw_open(w, "Takes");
    snprintf(nm, sizeof nm, "Open%s", sfx);
    fbxw_node_s(w, "Current", nm);
    for (int p = 0; p < VRH_POSES; p++) {
        char tak[64];
        snprintf(nm, sizeof nm, "%s%s", g_vrh_pose_names[p], sfx);
        snprintf(tak, sizeof tak, "%s.tak", nm);
        fbxw_open(w, "Take"), fbxw_s(w, nm);
        fbxw_node_s(w, "FileName", tak);
        fbxw_open(w, "LocalTime"), fbxw_l(w, 0), fbxw_l(w, stop), fbxw_close(w);
        fbxw_open(w, "ReferenceTime"), fbxw_l(w, 0), fbxw_l(w, stop), fbxw_close(w);
        fbxw_close(w);
    }
    fbxw_close(w);
    bool ok = fbxw_save(w, path);
    if (!ok) snprintf(err, cap, "could not write %s", path);
    fbxw_free(w), free(m.v), free(m.t);
    return ok;
}

bool vrh_export_folder(const char* dir, char* err, size_t cap) {
    re_make_dirs(dir);
    char p[1024];
    for (int s = 0; s < 2; s++) {
        snprintf(p, sizeof p, "%s/%s", dir, vrh_file_name(s));
        if (!vrh_export_fbx(s, p, err, cap)) return false;
    }
    snprintf(p, sizeof p, "%s/LEIA-ME_BaseHand.txt", dir);
    FILE* f = re_fopen(p, "wb");
    if (!f) return snprintf(err, cap, "could not write %s", p), false;
    fputs("Real Engine - mao base do VR / VR base hand\r\n"
          "=============================================\r\n\r\n"
          "PT\r\n"
          "- RealEngine_BaseHand_L.fbx e _R.fbx: malha com pele (skin), esqueleto com as 26 juntas do OpenXR e 7 poses.\r\n"
          "- Escala: centimetros, Z para cima. A mao direita aponta para +X com a palma para baixo.\r\n"
          "- Ossos: Wrist (raiz), Palm, Thumb/Index/Middle/Ring/Little + Metacarpal, Proximal, Intermediate (nao no polegar),\r\n"
          "  Distal, Tip, com o sufixo _L ou _R. Mantenha esses nomes (pode trocar o sufixo; \"Pinky\" vale por Little):\r\n"
          "  a engine acha os ossos pelo nome. Os Tips marcam a ponta dos dedos (sem peso).\r\n"
          "- Animacoes (30 fps, quadro 0 = Open, quadro 10 = a pose): Open, Relaxed, Fist, Grip, Point, Pinch, ThumbsUp\r\n"
          "  (com _L ou _R). A engine acha Open e Fist pelo nome em qualquer parte do da animacao.\r\n"
          "  Com o controle, a engine mistura Open e Fist por dedo (gatilho = indicador, grip = os outros, botoes = polegar).\r\n"
          "  As outras ficam como referencia e para o seu Anim Blueprint.\r\n"
          "- Para usar a mao nova: importe o FBX no Content Browser e escolha a Skeletal Mesh em\r\n"
          "  Project Settings > VR > Malha da Mao Esquerda/Direita. Maos rastreadas seguem as juntas reais.\r\n\r\n"
          "EN\r\n"
          "- RealEngine_BaseHand_L.fbx and _R.fbx: a skinned mesh, a skeleton of OpenXR's 26 hand joints and 7 poses.\r\n"
          "- Scale: centimeters, Z up. The right hand points along +X, palm down.\r\n"
          "- Bones: Wrist (root), Palm, Thumb/Index/Middle/Ring/Little + Metacarpal, Proximal, Intermediate (not on the thumb),\r\n"
          "  Distal, Tip, suffixed _L or _R. Keep these names (the suffix may change; \"Pinky\" counts as Little): the engine\r\n"
          "  finds the bones by name. The Tips mark the fingertips (no weights).\r\n"
          "- Animations (30 fps, frame 0 = Open, frame 10 = the pose): Open, Relaxed, Fist, Grip, Point, Pinch, ThumbsUp\r\n"
          "  (with _L or _R). The engine finds Open and Fist by the name anywhere in the animation's.\r\n"
          "  With controllers the engine blends Open and Fist per finger (trigger = index, grip = the rest, buttons = thumb).\r\n"
          "  The others are references and for your own Anim Blueprint.\r\n"
          "- To use the new hand: import the FBX in the Content Browser and pick the Skeletal Mesh in\r\n"
          "  Project Settings > VR > Left/Right Hand Mesh. Tracked hands follow the real joints.\r\n",
          f);
    fclose(f);
    return true;
}

// --- the solver (the engine's space: the imported Skeletal Mesh's) ---
static void bind_model(const SkelMesh* m, float qm[][4], float pm[][3]) {
    for (uint32_t b = 0; b < m->nbones; b++) {
        const AnimBone* ab = &m->bones[b];
        int p = ab->parent;
        if (p < 0) {
            memcpy(qm[b], ab->bind.q, 16), memcpy(pm[b], ab->bind.t, 12);
            continue;
        }
        float r[3];
        q_rot(qm[p], ab->bind.t, r);
        for (int a = 0; a < 3; a++) pm[b][a] = pm[p][a] + r[a];
        q_mul(qm[p], ab->bind.q, qm[b]);
    }
}
static bool ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
static bool icontains(const char* s, const char* what) {
    size_t wl = strlen(what);
    for (; *s; s++) {
        size_t k = 0;
        while (k < wl && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)what[k])) k++;
        if (k == wl) return true;
    }
    return false;
}
const AnimSeq* vrh_find_clip(const SkelMesh* m, const char* pose_name) {
    for (uint32_t i = 0; i < anim_seq_count(); i++) {
        const AnimSeq* s = anim_seq_at(i);
        if (s && ieq(s->skeleton, m->name) && icontains(s->name, pose_name)) return s;
    }
    return NULL;
}
// a pose clip's last frame (they loop: their length would be frame 0 again)
float vrh_clip_end(const AnimSeq* s) { return s->nframes > 1 ? (float)(s->nframes - 1) / s->fps * 0.9999f : 0.0f; }

bool vrh_rig_build(VrHandRig* r, const SkelMesh* m, int side) {
    memset(r, 0, sizeof *r);
    r->side = side, r->nbones = m->nbones;
    for (int j = 0; j < VRH_JOINTS; j++) r->bone_of[j] = -1;
    for (uint32_t b = 0; b < m->nbones && b < ANIM_MAX_BONES; b++) {
        int j = vrh_joint_of_name(m->bones[b].name);
        r->joint_of[b] = (int8_t)j, r->finger_of[b] = (int8_t)(j >= 0 ? finger_of_joint(j) : -1), r->aim_child[b] = -1;
        if (j >= 0 && r->bone_of[j] < 0) r->bone_of[j] = (int16_t)b;
    }
    bind_model(m, r->qm, r->pm);
    // each named finger bone's next named bone down its finger (the aim's target)
    for (uint32_t b = 0; b < m->nbones; b++) {
        int j = r->joint_of[b];
        if (j < 2) continue;
        for (uint32_t c = b + 1; c < m->nbones; c++) {
            int jc = r->joint_of[c], p = m->bones[c].parent;
            bool under = false;
            for (int d = 0; p >= 0 && d < 64; p = m->bones[p].parent, d++)
                if (p == (int)b) under = true;
            if (under && jc > j && finger_of_joint(jc) == finger_of_joint(j)) {
                r->aim_child[b] = (int16_t)c;
                break;
            }
        }
    }
    int w = r->bone_of[VRH_WRIST], ip = r->bone_of[VRH_INDEX_META + 1], lp = r->bone_of[VRH_LITTLE_META + 1], mp = r->bone_of[VRH_MIDDLE_META + 1];
    int fingers = 0;
    for (int f = 0; f < 5; f++)
        for (uint32_t b = 0; b < m->nbones; b++)
            if (r->finger_of[b] == f && r->aim_child[b] >= 0) {
                fingers++;
                break;
            }
    r->tracked_ok = w >= 0 && ip >= 0 && lp >= 0 && mp >= 0;
    r->ok = fingers >= 4 && ip >= 0 && lp >= 0 && w >= 0;
    if (!r->ok) return false;
    // the procedural curl: the palm's normal (out of the palm: its sign by the side, the engine's space mirrors the FBX's)
    float a[3], c[3], n[3];
    v_sub(r->pm[ip], r->pm[w], a), v_sub(r->pm[lp], r->pm[w], c);
    v_cross(a, c, n), v_norm(n);
    if (side) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    float across[3];
    v_sub(r->pm[lp], r->pm[ip], across), v_norm(across);
    for (uint32_t b = 0; b < m->nbones; b++) {
        int j = r->joint_of[b], ch = r->aim_child[b];
        if (j < 2 || ch < 0) continue;
        float d[3], ax[3], toward[3];
        v_sub(r->pm[ch], r->pm[b], d), v_norm(d);
        if (r->finger_of[b] == 0)
            for (int k = 0; k < 3; k++) toward[k] = n[k] * 0.8f + across[k] * 0.6f;
        else
            memcpy(toward, n, 12);
        v_cross(d, toward, ax), v_norm(ax);
        float inv[4];
        q_conj(r->qm[b], inv);
        q_rot(inv, ax, r->axis[b]); // (in the bone's own frame)
        int k = r->finger_of[b] == 0 ? j - VRH_THUMB_META : (j - VRH_INDEX_META) % 5;
        static const float fist_f[4] = { 0.0f, 85.0f, 100.0f, 60.0f }, fist_t[3] = { 30.0f, 45.0f, 40.0f };
        r->fist_deg[b] = r->finger_of[b] == 0 ? (k < 3 ? fist_t[k] : 0.0f) : (k < 4 ? fist_f[k] : 0.0f);
        if (r->finger_of[b] >= 3 && k == 0) r->fist_deg[b] = r->finger_of[b] == 3 ? 4.0f : 8.0f; // (the ring's and little's palm cups)
    }
    r->open = vrh_find_clip(m, "Open"), r->fist = vrh_find_clip(m, "Fist");
    if (!r->open || !r->fist) r->open = r->fist = NULL;
    return true;
}

void vrh_pose_curl(const VrHandRig* r, const SkelMesh* m, const float curl[5], AnimXf* out) {
    for (uint32_t b = 0; b < m->nbones; b++) out[b] = m->bones[b].bind;
    if (!r->ok) return;
    if (r->open && r->fist) { // the rig's own poses: Open -> Fist per finger
        static AnimXf po[ANIM_MAX_BONES], pf[ANIM_MAX_BONES]; // (the main thread's: vr_pawn's late tick)
        anim_sample(r->open, m, vrh_clip_end(r->open), po);
        anim_sample(r->fist, m, vrh_clip_end(r->fist), pf);
        for (uint32_t b = 0; b < m->nbones; b++) {
            int f = r->finger_of[b];
            float k = f >= 0 ? curl[f] : 0.0f;
            k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
            float s = po[b].q[0] * pf[b].q[0] + po[b].q[1] * pf[b].q[1] + po[b].q[2] * pf[b].q[2] + po[b].q[3] * pf[b].q[3] < 0.0f ? -1.0f : 1.0f;
            out[b] = po[b];
            for (int a = 0; a < 4; a++) out[b].q[a] = po[b].q[a] * (1.0f - k) + pf[b].q[a] * s * k;
            q_norm(out[b].q);
            for (int a = 0; a < 3; a++) out[b].t[a] = po[b].t[a] + (pf[b].t[a] - po[b].t[a]) * k;
        }
        return;
    }
    for (uint32_t b = 0; b < m->nbones; b++) {
        int f = r->finger_of[b];
        if (f < 0 || r->fist_deg[b] == 0.0f) continue;
        float k = curl[f] < 0.0f ? 0.0f : curl[f] > 1.0f ? 1.0f : curl[f], d[4];
        q_axis(r->axis[b], r->fist_deg[b] * k * VRH_DEG, d);
        q_mul(m->bones[b].bind.q, d, out[b].q);
    }
}

bool vrh_pose_tracked(const VrHandRig* r, const SkelMesh* m, const float joints[VRH_JOINTS][3], float scale, float rot[3][3],
                      float pos[3], AnimXf* out) {
    for (uint32_t b = 0; b < m->nbones; b++) out[b] = m->bones[b].bind;
    if (!r->tracked_ok) return false;
    // the hand's frame: the wrist -> the middle knuckle (x), the little -> the index knuckle (y): the bind's and the real's
    int w = r->bone_of[VRH_WRIST], ip = r->bone_of[VRH_INDEX_META + 1], lp = r->bone_of[VRH_LITTLE_META + 1], mp = r->bone_of[VRH_MIDDLE_META + 1];
    float B[3][3], T[3][3];
    for (int pass = 0; pass < 2; pass++) {
        const float *pw = pass ? joints[VRH_WRIST] : r->pm[w], *pmid = pass ? joints[VRH_MIDDLE_META + 1] : r->pm[mp];
        const float *pi = pass ? joints[VRH_INDEX_META + 1] : r->pm[ip], *pl = pass ? joints[VRH_LITTLE_META + 1] : r->pm[lp];
        float (*F)[3] = pass ? T : B;
        v_sub(pmid, pw, F[0]), v_norm(F[0]);
        v_sub(pi, pl, F[1]);
        float d = v_dot(F[1], F[0]);
        for (int a = 0; a < 3; a++) F[1][a] -= F[0][a] * d;
        v_norm(F[1]);
        v_cross(F[0], F[1], F[2]);
    }
    // R = T B^T: rot[k] (the mesh's axis k in the world) = sum_i B[i][k] T[i]
    for (int k = 0; k < 3; k++)
        for (int a = 0; a < 3; a++) rot[k][a] = B[0][k] * T[0][a] + B[1][k] * T[1][a] + B[2][k] * T[2][a];
    for (int a = 0; a < 3; a++)
        pos[a] = joints[VRH_WRIST][a] - (rot[0][a] * r->pm[w][0] + rot[1][a] * r->pm[w][1] + rot[2][a] * r->pm[w][2]) * scale;
    // each named bone turned toward its next joint's real place (mesh space), its parents' turns first
    static float qn[ANIM_MAX_BONES][4];
    for (uint32_t b = 0; b < m->nbones; b++) {
        int p = m->bones[b].parent;
        if (p < 0) memcpy(qn[b], m->bones[b].bind.q, 16);
        else q_mul(qn[p], m->bones[b].bind.q, qn[b]);
        int j = r->joint_of[b], c = r->aim_child[b];
        if (j < 2 || c < 0) continue;
        int jc = r->joint_of[c];
        float dw[3], d[3];
        v_sub(joints[jc], joints[j], dw);
        for (int k = 0; k < 3; k++) d[k] = rot[k][0] * dw[0] + rot[k][1] * dw[1] + rot[k][2] * dw[2]; // (world -> mesh: R^T)
        if (v_norm(d) < 1e-4f) continue;
        float vb[3], inv[4], v[3], s[4], q2[4];
        v_sub(r->pm[c], r->pm[b], vb);
        q_conj(r->qm[b], inv);
        q_rot(inv, vb, v); // (in the bone's frame)
        q_rot(qn[b], v, v), v_norm(v);
        q_from_to(v, d, s);
        q_mul(s, qn[b], q2);
        memcpy(qn[b], q2, 16);
        float pinv[4];
        if (p < 0) memcpy(out[b].q, q2, 16);
        else q_conj(qn[p], pinv), q_mul(pinv, q2, out[b].q);
        q_norm(out[b].q);
    }
    return true;
}
