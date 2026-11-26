// Animation (anim.h, notas-dev/animacao.md): the assets (Skeletal Mesh, Animation Sequence, Anim Montage), the Anim
// Graph of the Animation Blueprints, the Mannequin made by the engine, and the run time (state machines, blends,
// montages, notifies, the parts' transforms).
#include "anim.h"
#include "rig_detect.h" // (2026-11-23) what each bone is, on any rig
#include "material.h" // (mat_root: a Static Mesh's sockets file)
#include "fpcam.h"
#include "project.h"
#include "render.h"
#include "scene_query.h"
#include "debug_draw.h"
#include "audio.h"
#include "ai.h"
#include "ikrig.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "io_json.h"
#include "mem.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"

#define PI 3.14159265f
#define DEG (PI / 180.0f)
#define MAX_ASSETS 256
#define MAX_INST 65536u

static struct {
    char root[600];
    bool scanned;
    SkelMesh* meshes[MAX_ASSETS];
    AnimSeq* seqs[MAX_ASSETS];
    AnimMontage* montages[MAX_ASSETS];
    uint32_t nmeshes, nseqs, nmontages;
    // the run's
    ReVmBuf insts;
    uint32_t ninst;
    bool playing, ready;
    uint32_t frame;
    AnimStats stats;
    ReVmBuf index;           // PartRef: the parts by owner
    uint32_t nindex, index_changes, index_revision;
    bool index_valid, index_ready;
    uint32_t editor_changes, editor_revision; // the editor's poses: made again when the world changed
    uint64_t editor_xf;      // ... or a skeletal mesh moved (the gizmo edits the selection in place: no change logged)
    // the skins' palettes by the CSkeletalMesh's entity (an open table of entity -> record, made on this thread: the
    // pose jobs only write their own record)
    struct SkinPal* pals;
    uint32_t npals, pal_cap;
    uint32_t* pal_table;     // pal_tsize entries: record + 1 (0: empty)
    uint32_t pal_tsize;
    uint32_t mesh_generation, seen_generation; // (anim_mesh_reload: the actors make their parts again)
    bool editor_rt;  // (the viewport's Realtime) the editor plays each Skeletal Mesh's own animation, looping
    double editor_t; // ... its clock
} A;

void anim_set_editor_realtime(bool on) { A.editor_rt = on; }

typedef struct SkinPal {
    Entity e;
    uint32_t version, nbones;
    float m[ANIM_MAX_BONES][3][4];
} SkinPal;

// --- Quaternions and transforms ---

static void q_axis(const float axis[3], float angle, float q[4]) {
    float s = sinf(angle * 0.5f);
    q[0] = axis[0] * s, q[1] = axis[1] * s, q[2] = axis[2] * s, q[3] = cosf(angle * 0.5f);
}
static void q_mul(const float a[4], const float b[4], float o[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}
static void q_norm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) {
        q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
        return;
    }
    for (int k = 0; k < 4; k++) q[k] /= l;
}
// nlerp along the shorter arc (as good as slerp between frames 1/30 s apart, much cheaper)
static void q_nlerp(const float a[4], const float b[4], float t, float o[4]) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 4; k++) o[k] = a[k] + (b[k] * s - a[k]) * t;
    q_norm(o);
}
// rows: the local axes in the parent's space (rotator_axes' convention: a point = sum local[a] * R[a])
static void q_axes(const float q[4], float R[3][3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float M[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                      { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                      { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) R[a][k] = M[k][a];
}
// The rotation taking +Z to dir (the shapes' axis to a limb's)
static void q_from_z(const float dir[3], float q[4]) {
    float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    float d[3] = { dir[0] / l, dir[1] / l, dir[2] / l };
    float c = d[2];
    if (c > 0.9999f) {
        q[0] = q[1] = q[2] = 0.0f, q[3] = 1.0f;
        return;
    }
    if (c < -0.9999f) {
        q[0] = 1.0f, q[1] = q[2] = q[3] = 0.0f;
        return;
    }
    float axis[3] = { -d[1], d[0], 0.0f }, al = sqrtf(axis[0] * axis[0] + axis[1] * axis[1]);
    for (int k = 0; k < 3; k++) axis[k] /= al;
    q_axis(axis, acosf(c), q);
}

static void xf_identity(AnimXf* x) {
    memset(x, 0, sizeof *x);
    x->q[3] = 1.0f;
    x->s[0] = x->s[1] = x->s[2] = 1.0f;
}

// rows (3 x 4, affine: columns 0-2 the axes times the scale, 3 the origin) of a local transform under a parent's rows
static void xf_rows(const AnimXf* x, const float parent[3][4], float out[3][4]) {
    float R[3][3];
    q_axes(x->q, R);
    float l[3][4]; // the local rows
    for (int r = 0; r < 3; r++) {
        for (int a = 0; a < 3; a++) l[r][a] = R[a][r] * x->s[a];
        l[r][3] = x->t[r];
    }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            float v = parent[r][0] * l[0][c] + parent[r][1] * l[1][c] + parent[r][2] * l[2][c];
            out[r][c] = c == 3 ? v + parent[r][3] : v;
        }
}

static void pc_blend2(const AnimXf* a, const AnimXf* b, float f, const AnimXf* out); // (the morph curves, below)
static void pc_blendn(const AnimXf* const* poses, const float* wt, uint32_t count, const AnimXf* out);
static _Thread_local uint32_t t_npc;
void anim_blend(const AnimXf* a, const AnimXf* b, float w, uint32_t n, AnimXf* out) {
    if (t_npc) pc_blend2(a, b, w, out); // (the morph curves first: out may be a)
    for (uint32_t i = 0; i < n; i++) {
        AnimXf r;
        for (int k = 0; k < 3; k++) {
            r.t[k] = a[i].t[k] + (b[i].t[k] - a[i].t[k]) * w;
            r.s[k] = a[i].s[k] + (b[i].s[k] - a[i].s[k]) * w;
        }
        q_nlerp(a[i].q, b[i].q, w, r.q);
        out[i] = r;
    }
}

void anim_blend_n(const AnimXf* const* poses, const float* w, uint32_t count, uint32_t nbones, AnimXf* out) {
    if (t_npc) pc_blendn(poses, w, count, out);
    for (uint32_t i = 0; i < nbones; i++) {
        AnimXf r;
        memset(&r, 0, sizeof r);
        const float* ref = NULL;
        for (uint32_t k = 0; k < count; k++) {
            if (w[k] <= 0.0f) continue;
            const AnimXf* x = &poses[k][i];
            if (!ref) ref = x->q;
            float d = ref[0] * x->q[0] + ref[1] * x->q[1] + ref[2] * x->q[2] + ref[3] * x->q[3], s = d < 0.0f ? -w[k] : w[k];
            for (int c = 0; c < 4; c++) r.q[c] += x->q[c] * s; // (the shorter arc: signs aligned with the first)
            for (int c = 0; c < 3; c++) r.t[c] += x->t[c] * w[k], r.s[c] += x->s[c] * w[k];
        }
        if (!ref) xf_identity(&r);
        q_norm(r.q);
        out[i] = r;
    }
}

void anim_blend2d_weights(const float* sx, const float* sy, uint32_t n, float x, float y, float* w) {
    if (!n) return;
    // (the axes scaled to the samples' spans: speed in cm/s and direction in degrees weigh alike)
    float mnx = sx[0], mxx = sx[0], mny = sy[0], mxy = sy[0];
    for (uint32_t i = 1; i < n; i++) mnx = fminf(mnx, sx[i]), mxx = fmaxf(mxx, sx[i]), mny = fminf(mny, sy[i]), mxy = fmaxf(mxy, sy[i]);
    float kx = mxx - mnx > 1e-6f ? 1.0f / (mxx - mnx) : 1.0f, ky = mxy - mny > 1e-6f ? 1.0f / (mxy - mny) : 1.0f;
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) { // gradient band: the least of 1 - (p - pi).(pj - pi) / |pj - pi|^2 over the others
        float wi = 1.0f;
        for (uint32_t j = 0; j < n; j++) {
            if (j == i) continue;
            float vx = (sx[j] - sx[i]) * kx, vy = (sy[j] - sy[i]) * ky, d2 = vx * vx + vy * vy;
            if (d2 < 1e-9f) continue;
            float h = 1.0f - (((x - sx[i]) * kx) * vx + ((y - sy[i]) * ky) * vy) / d2;
            wi = fminf(wi, h);
        }
        w[i] = fmaxf(wi, 0.0f);
        sum += w[i];
    }
    if (sum > 1e-6f) {
        for (uint32_t i = 0; i < n; i++) w[i] /= sum;
        return;
    }
    uint32_t best = 0; // (outside of them all: the nearest)
    float bd = 1e30f;
    for (uint32_t i = 0; i < n; i++) {
        float dx = (x - sx[i]) * kx, dy = (y - sy[i]) * ky, d = dx * dx + dy * dy;
        if (d < bd) bd = d, best = i;
        w[i] = 0.0f;
    }
    w[best] = 1.0f;
}

void anim_two_bone_ik(const float a[3], const float b[3], const float c[3], const float target[3], float out_b[3], float out_c[3]) {
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, bc[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
    float at[3] = { target[0] - a[0], target[1] - a[1], target[2] - a[2] };
    float l1 = sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]), l2 = sqrtf(bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2]);
    float d = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
    if (l1 < 1e-4f || l2 < 1e-4f || d < 1e-4f) {
        memcpy(out_b, b, 12), memcpy(out_c, c, 12);
        return;
    }
    float dir[3] = { at[0] / d, at[1] / d, at[2] / d };
    d = fminf(fmaxf(d, fabsf(l1 - l2) + 1e-3f), l1 + l2 - 1e-3f); // (out of reach: the leg straight toward it)
    // the knee's side: where it was off the hip-end line (the way it bends), then square to the hip-target one
    float ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] }, lac = sqrtf(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
    float pole[3] = { ab[0], ab[1], ab[2] };
    if (lac > 1e-4f) {
        float e = (ab[0] * ac[0] + ab[1] * ac[1] + ab[2] * ac[2]) / (lac * lac);
        for (int k = 0; k < 3; k++) pole[k] = ab[k] - ac[k] * e;
    }
    float dp = pole[0] * dir[0] + pole[1] * dir[1] + pole[2] * dir[2];
    for (int k = 0; k < 3; k++) pole[k] -= dir[k] * dp;
    float pl = sqrtf(pole[0] * pole[0] + pole[1] * pole[1] + pole[2] * pole[2]);
    if (pl < 1e-4f) pole[0] = 1.0f, pole[1] = pole[2] = 0.0f, pl = 1.0f;
    float along = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d), h = sqrtf(fmaxf(l1 * l1 - along * along, 0.0f));
    for (int k = 0; k < 3; k++) {
        out_b[k] = a[k] + dir[k] * along + pole[k] / pl * h;
        out_c[k] = a[k] + dir[k] * d;
    }
}

// The rotation matrix (M[row][col], the columns the axes) of a bone's world rows, without their scale; and a matrix to
// a quaternion (q_axes' inverse)
static void rows_rot(const float r[3][4], float M[3][3]) {
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(r[0][a] * r[0][a] + r[1][a] * r[1][a] + r[2][a] * r[2][a]);
        for (int k = 0; k < 3; k++) M[k][a] = l > 1e-12f ? r[k][a] / l : (k == a ? 1.0f : 0.0f);
    }
}
static void m_to_q(const float M[3][3], float q[4]) {
    float tr = M[0][0] + M[1][1] + M[2][2];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (M[2][1] - M[1][2]) / s, q[1] = (M[0][2] - M[2][0]) / s, q[2] = (M[1][0] - M[0][1]) / s;
    } else if (M[0][0] > M[1][1] && M[0][0] > M[2][2]) {
        float s = sqrtf(1.0f + M[0][0] - M[1][1] - M[2][2]) * 2.0f;
        q[3] = (M[2][1] - M[1][2]) / s, q[0] = 0.25f * s, q[1] = (M[0][1] + M[1][0]) / s, q[2] = (M[0][2] + M[2][0]) / s;
    } else if (M[1][1] > M[2][2]) {
        float s = sqrtf(1.0f + M[1][1] - M[0][0] - M[2][2]) * 2.0f;
        q[3] = (M[0][2] - M[2][0]) / s, q[0] = (M[0][1] + M[1][0]) / s, q[1] = 0.25f * s, q[2] = (M[1][2] + M[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + M[2][2] - M[0][0] - M[1][1]) * 2.0f;
        q[3] = (M[1][0] - M[0][1]) / s, q[0] = (M[0][2] + M[2][0]) / s, q[1] = (M[1][2] + M[2][1]) / s, q[2] = 0.25f * s;
    }
    q_norm(q);
}
static void m_mul(const float A[3][3], const float B[3][3], float O[3][3]) {
    float T[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) T[r][c] = A[r][0] * B[0][c] + A[r][1] * B[1][c] + A[r][2] * B[2][c];
    memcpy(O, T, sizeof T);
}
// The rotation (a matrix) taking unit direction u to v
static void m_between(const float u[3], const float v[3], float M[3][3]) {
    float c[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2], q[4];
    if (d < -0.9999f) { // (opposite: half a turn about any perpendicular)
        float p[3] = { fabsf(u[0]) < 0.9f ? 1.0f : 0.0f, fabsf(u[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
        float ax[3] = { u[1] * p[2] - u[2] * p[1], u[2] * p[0] - u[0] * p[2], u[0] * p[1] - u[1] * p[0] };
        float l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (int k = 0; k < 3; k++) ax[k] /= l;
        q_axis(ax, 3.14159265f, q);
    } else
        q[0] = c[0], q[1] = c[1], q[2] = c[2], q[3] = 1.0f + d, q_norm(q);
    float R[3][3];
    q_axes(q, R); // (R[a]: axis a: the columns)
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) M[k][a] = R[a][k];
}
// A bone's local rotation for a world rotation W under its parent's world rows (or the actor's)
static void set_world_rot(const SkelMesh* m, AnimXf* pose, uint32_t b, const float W[3][3], float bones[][3][4], const float actor[3][4]) {
    int p = m->bones[b].parent;
    float P[3][3], L[3][3];
    rows_rot(p >= 0 ? bones[p] : actor, P);
    for (int r = 0; r < 3; r++) // L = P^T W
        for (int c = 0; c < 3; c++) L[r][c] = P[0][r] * W[0][c] + P[1][r] * W[1][c] + P[2][r] * W[2][c];
    m_to_q(L, pose[b].q);
}

void anim_world(const SkelMesh* m, const AnimXf* local, const float actor[3][4], float out[][3][4]) {
    for (uint32_t b = 0; b < m->nbones; b++) {
        int p = m->bones[b].parent;
        xf_rows(&local[b], p >= 0 ? out[p] : actor, out[b]);
    }
}

// The inverse of rows (affine 3 x 4: a general 3 x 3 and a translation)
static void rows_inverse(const float a[3][4], float o[3][4]) {
    float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1], c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2], c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    float id = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    o[0][0] = c00 * id, o[1][0] = c01 * id, o[2][0] = c02 * id;
    o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id, o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id,
    o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
    o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id, o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id,
    o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    for (int r = 0; r < 3; r++) o[r][3] = -(o[r][0] * a[0][3] + o[r][1] * a[1][3] + o[r][2] * a[2][3]);
}

static void rows_mul(const float a[3][4], const float b[3][4], float o[3][4]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
}

// The palette record of a CSkeletalMesh entity (make: added when missing; the main thread only)
static SkinPal* skin_pal(Entity e, bool make) {
    if (!A.pal_tsize) {
        if (!make) return NULL;
        A.pal_tsize = 1024;
        A.pal_table = calloc(A.pal_tsize, sizeof(uint32_t)); // (a table, not a frame's)
        if (!A.pal_table) return A.pal_tsize = 0, NULL;
    }
    uint32_t mask = A.pal_tsize - 1u, h = (e * 2654435761u) & mask;
    for (uint32_t k = 0; k < A.pal_tsize; k++, h = (h + 1u) & mask) {
        uint32_t r = A.pal_table[h];
        if (!r) break;
        if (A.pals[r - 1].e == e) return &A.pals[r - 1];
    }
    if (!make) return NULL;
    if ((A.npals + 1) * 2 > A.pal_tsize) { // (grown: a power of two, half full at most)
        uint32_t ns = A.pal_tsize * 2, *nt = calloc(ns, sizeof(uint32_t));
        if (!nt) return NULL;
        for (uint32_t i = 0; i < A.npals; i++) {
            uint32_t hh = (A.pals[i].e * 2654435761u) & (ns - 1u);
            while (nt[hh]) hh = (hh + 1u) & (ns - 1u);
            nt[hh] = i + 1;
        }
        free(A.pal_table);
        A.pal_table = nt, A.pal_tsize = ns, mask = ns - 1u;
    }
    if (A.npals >= A.pal_cap) {
        uint32_t nc = A.pal_cap ? A.pal_cap * 2 : 16;
        SkinPal* np = realloc(A.pals, sizeof(SkinPal) * nc);
        if (!np) return NULL;
        A.pals = np, A.pal_cap = nc;
    }
    SkinPal* p = &A.pals[A.npals++];
    memset(p, 0, sizeof *p);
    p->e = e;
    h = (e * 2654435761u) & mask;
    while (A.pal_table[h]) h = (h + 1u) & mask;
    A.pal_table[h] = A.npals;
    return p;
}

const float (*anim_skin_palette(Entity sk, uint32_t* nbones, uint32_t* version))[3][4] {
    SkinPal* p = skin_pal(sk, false);
    if (!p || !p->version) return NULL;
    if (nbones) *nbones = p->nbones;
    if (version) *version = p->version;
    return (const float (*)[3][4])p->m;
}

// --- Asset files ---

static uint32_t vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}

static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", A.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}

static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(A.root);
    if (!strncmp(disk, A.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}

static void name_of(const char* path, char* out) {
    const char* s = strrchr(path, '/');
    s = s ? s + 1 : path;
    snprintf(out, ANIM_NAME, "%s", s);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static char* read_text(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n] = 0, *len = (size_t)n;
    return t;
}

static bool write_text(const char* path, JsonWriter* w) {
    if (w->fail) return false;
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(w->buf, 1, w->len, f) == w->len;
    ok = fclose(f) == 0 && ok;
    return ok && re_replace_file(tmp, path);
}

static void write_xf(JsonWriter* w, const char* key, const AnimXf* x) {
    jw_key(w, key);
    jw_obj_begin(w);
    jw_kfloats(w, "t", x->t, 3);
    jw_kfloats(w, "q", x->q, 4);
    jw_kfloats(w, "s", x->s, 3);
    jw_obj_end(w);
}
static void read_xf(const JsonValue* o, AnimXf* x) {
    xf_identity(x);
    if (!o) return;
    json_get_floats(o, "t", x->t, 3);
    json_get_floats(o, "q", x->q, 4);
    json_get_floats(o, "s", x->s, 3);
    q_norm(x->q);
}

static void mesh_prepare(SkelMesh* m) { // the bind pose in the mesh's space (the editors' preview)
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    AnimXf loc[ANIM_MAX_BONES];
    for (uint32_t b = 0; b < m->nbones; b++) loc[b] = m->bones[b].bind;
    anim_world(m, loc, id, m->bind_world);
    for (uint32_t b = 0; b < m->nbones; b++) rows_inverse(m->bind_world[b], m->bind_inv[b]);
}

bool anim_mesh_save(SkelMesh* m) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", m->nskins ? 2 : 1); // (2: skins)
    if (m->source[0]) jw_kstr(&w, "source", m->source);
    if (m->rigid) jw_kbool(&w, "rigid", true);
    jw_key(&w, "bones");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < m->nbones; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", m->bones[i].name);
        jw_kint(&w, "parent", m->bones[i].parent);
        write_xf(&w, "bind", &m->bones[i].bind);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "parts");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < m->nparts; i++) {
        const SkPart* p = &m->parts[i];
        jw_obj_begin(&w);
        jw_kint(&w, "bone", p->bone);
        jw_kint(&w, "shape", p->mesh); // (the key's old name: a BasicShape or an imported mesh's id)
        write_xf(&w, "local", &p->local);
        jw_kfloats(&w, "color", p->color, 3);
        jw_kfloat(&w, "roughness", p->roughness);
        jw_kfloat(&w, "metallic", p->metallic);
        jw_kint(&w, "material", p->material);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    if (m->nskins) {
        jw_key(&w, "skins");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < m->nskins; i++) {
            const SkSkin* k = &m->skins[i];
            jw_obj_begin(&w);
            jw_kint(&w, "mesh", k->mesh);
            jw_kfloats(&w, "color", k->color, 3);
            jw_kfloat(&w, "roughness", k->roughness);
            jw_kfloat(&w, "metallic", k->metallic);
            jw_kint(&w, "material", k->material);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    if (m->nsockets) { // (Q7) by its bone's name: a reimport that orders the bones another way keeps them
        jw_key(&w, "sockets");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < m->nsockets; i++) {
            const AnimSocket* k = &m->sockets[i];
            jw_obj_begin(&w);
            jw_kstr(&w, "name", k->name);
            jw_kstr(&w, "bone", k->bone >= 0 && k->bone < (int)m->nbones ? m->bones[k->bone].name : "");
            write_xf(&w, "local", &k->local);
            if (k->has_preview) jw_kint(&w, "preview_mesh", (int64_t)k->preview_mesh);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    if (m->nmorphs) {
        jw_key(&w, "morphs");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < m->nmorphs; i++) {
            jw_obj_begin(&w);
            jw_kstr(&w, "name", m->morph_name[i]);
            jw_kfloat(&w, "weight", m->morph_default[i]);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    jw_obj_end(&w);
    bool ok = write_text(m->path, &w);
    jw_free(&w);
    if (ok) m->dirty = false;
    return ok;
}

int anim_morph_index(const SkelMesh* m, const char* name) {
    if (!m || !name) return -1;
    for (uint32_t i = 0; i < m->nmorphs; i++)
        if (!_stricmp(m->morph_name[i], name)) return (int)i;
    return -1;
}

// The Skeletal Mesh component of e: its own, else its owner's (a skin or a part asked by its entity)
static CSkeletalMesh* morph_comp(World* w, Entity e) {
    CSkeletalMesh* c = e ? ecs_get(w, e, COMP_SKELETAL_MESH) : NULL;
    const CAttach* at = !c && e ? ecs_get(w, e, COMP_ATTACH) : NULL;
    return at ? ecs_get(w, at->owner, COMP_SKELETAL_MESH) : c;
}

bool anim_morph_set(World* w, Entity e, const char* name, float value) {
    CSkeletalMesh* c = morph_comp(w, e);
    int i = c ? anim_morph_index(anim_mesh(c->mesh), name) : -1;
    if (i < 0 || i >= ANIM_MAX_MORPHS) return false;
    c->morph_weight[i] = value == value ? value : 0.0f;
    c->morph_set[i >> 5] |= 1u << (i & 31);
    return true;
}

float anim_morph_get(World* w, Entity e, const char* name) {
    CSkeletalMesh* c = morph_comp(w, e);
    const SkelMesh* m = c ? anim_mesh(c->mesh) : NULL;
    int i = anim_morph_index(m, name);
    return i >= 0 ? anim_morph_weight(m, c, (uint32_t)i) : 0.0f;
}

float anim_morph_weight(const SkelMesh* m, const CSkeletalMesh* c, uint32_t i) {
    if (!m || i >= m->nmorphs) return 0.0f;
    float cw; // the animation's curve first (Unreal's: an animated morph target follows its curve), then the actor's, then the mesh's
    if (c && c->inst && anim_morph_from_curves(c, i, &cw)) return cw;
    if (c && i < ANIM_MAX_MORPHS && (c->morph_set[i >> 5] >> (i & 31u) & 1u)) return c->morph_weight[i];
    return m->morph_default[i];
}

static SkelMesh* mesh_read(const char* vpath, uint32_t h) {
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* t = read_text(path, &len);
    if (!t) return NULL;
    JsonDoc doc;
    char err[160];
    SkelMesh* m = NULL;
    if (json_parse(t, len, &doc, err, sizeof err)) {
        m = calloc(1, sizeof *m); // an asset, not a frame
        if (m) {
            snprintf(m->path, sizeof m->path, "%s", path);
            name_of(path, m->name);
            m->hash = h;
            snprintf(m->source, sizeof m->source, "%s", json_get_str(&doc.root, "source", ""));
            m->rigid = json_get_bool(&doc.root, "rigid", false);
            const JsonValue* bones = json_get(&doc.root, "bones");
            for (size_t i = 0; i < json_len(bones) && m->nbones < ANIM_MAX_BONES; i++) {
                const JsonValue* b = json_at(bones, i);
                AnimBone* ab = &m->bones[m->nbones];
                snprintf(ab->name, sizeof ab->name, "%s", json_get_str(b, "name", "bone"));
                int p = json_get_int(b, "parent", -1);
                ab->parent = (int16_t)(p >= 0 && p < (int)m->nbones ? p : -1);
                read_xf(json_get(b, "bind"), &ab->bind);
                m->nbones++;
            }
            const JsonValue* parts = json_get(&doc.root, "parts");
            for (size_t i = 0; i < json_len(parts) && m->nparts < ANIM_MAX_PARTS; i++) {
                const JsonValue* o = json_at(parts, i);
                SkPart* p = &m->parts[m->nparts];
                int b = json_get_int(o, "bone", 0);
                p->bone = (int16_t)(b >= 0 && b < (int)m->nbones ? b : 0);
                p->mesh = (uint32_t)json_get_num(o, "shape", SHAPE_CUBE); // a shape or an imported mesh's id
                read_xf(json_get(o, "local"), &p->local);
                p->color[0] = p->color[1] = p->color[2] = 0.6f;
                json_get_floats(o, "color", p->color, 3);
                p->roughness = (float)json_get_num(o, "roughness", 0.5);
                p->metallic = (float)json_get_num(o, "metallic", 0.0);
                p->material = (uint32_t)json_get_num(o, "material", 0);
                m->nparts++;
            }
            const JsonValue* skins = json_get(&doc.root, "skins");
            for (size_t i = 0; i < json_len(skins) && m->nskins < ANIM_MAX_SKINS; i++) {
                const JsonValue* o = json_at(skins, i);
                SkSkin* k = &m->skins[m->nskins++];
                k->mesh = (uint32_t)json_get_num(o, "mesh", 0);
                k->color[0] = k->color[1] = k->color[2] = 0.6f;
                json_get_floats(o, "color", k->color, 3);
                k->roughness = (float)json_get_num(o, "roughness", 0.5);
                k->metallic = (float)json_get_num(o, "metallic", 0.0);
                k->material = (uint32_t)json_get_num(o, "material", 0);
            }
            const JsonValue* so = json_get(&doc.root, "sockets"); // (Q7)
            for (size_t i = 0; i < json_len(so) && m->nsockets < ANIM_MAX_SOCKETS; i++) {
                const JsonValue* o = json_at(so, i);
                AnimSocket* k = &m->sockets[m->nsockets];
                snprintf(k->name, sizeof k->name, "%s", json_get_str(o, "name", "Socket"));
                const char* bn = json_get_str(o, "bone", "");
                k->bone = 0;
                for (uint32_t b = 0; b < m->nbones; b++)
                    if (!_stricmp(m->bones[b].name, bn)) k->bone = (int16_t)b;
                read_xf(json_get(o, "local"), &k->local);
                k->has_preview = json_get(o, "preview_mesh") != NULL;
                k->preview_mesh = (uint32_t)json_get_num(o, "preview_mesh", 0);
                m->nsockets++;
            }
            const JsonValue* mo = json_get(&doc.root, "morphs");
            for (size_t i = 0; i < json_len(mo) && m->nmorphs < ANIM_MAX_MORPHS; i++) {
                const JsonValue* o = json_at(mo, i);
                snprintf(m->morph_name[m->nmorphs], ANIM_MORPH_NAME, "%s", json_get_str(o, "name", "Morph"));
                m->morph_default[m->nmorphs++] = (float)json_get_num(o, "weight", 0.0);
            }
            mesh_prepare(m);
        }
        json_free(&doc);
    }
    free(t);
    return m;
}

SkelMesh* anim_mesh_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < A.nmeshes; i++)
        if (A.meshes[i]->hash == h) return A.meshes[i];
    if (A.nmeshes >= MAX_ASSETS) return NULL;
    SkelMesh* m = mesh_read(vpath, h);
    if (m) A.meshes[A.nmeshes++] = m, rig_detect(m); // (2026-11-23) what each bone is: any rig's names
    return m;
}

bool anim_mesh_reload(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < A.nmeshes; i++)
        if (A.meshes[i]->hash == h) {
            SkelMesh* m = mesh_read(vpath, h);
            if (!m) return false;
            rig_forget(A.meshes[i]);
            *A.meshes[i] = *m; // (in place: the pointers to it stay good)
            free(m);
            rig_detect(A.meshes[i]); // (2026-11-23)
            A.mesh_generation++;
            return true;
        }
    return anim_mesh_load(vpath) != NULL;
}

bool anim_mesh_vpath(const SkelMesh* m, char* out, size_t cap) {
    if (!m) return false;
    char p[700];
    snprintf(p, sizeof p, "%s", m->path);
    for (char* c = p; *c; c++)
        if (*c == '\\') *c = '/';
    vpath_of(p, out, cap);
    return !strncmp(out, "/Content", 8);
}

uint32_t anim_reload_folder(const char* folder_vpath) {
    if (!folder_vpath || !folder_vpath[0]) return 0;
    size_t fl = strlen(folder_vpath);
    uint32_t n = 0;
    for (uint32_t i = 0; i < A.nmeshes; i++) {
        char vp[700];
        if (!anim_mesh_vpath(A.meshes[i], vp, sizeof vp) || _strnicmp(vp, folder_vpath, fl) || (vp[fl] != '/' && vp[fl])) continue;
        n += anim_mesh_reload(vp);
    }
    return n;
}

bool anim_mesh_old_parts(const SkelMesh* m) {
    if (!m || m->nskins || !m->nparts || m->rigid) return false;
    for (uint32_t i = 0; i < m->nparts; i++)
        if (m->parts[i].mesh >= SHAPE_COUNT) return true; // (cut from an imported mesh: the engine's own use basic shapes)
    return false;
}

bool anim_seq_save(AnimSeq* s) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    if (s->generator) jw_kint(&w, "generator", s->generator); // (made by the engine: anim_motion.inc)
    jw_kstr(&w, "skeleton", s->skeleton);
    jw_kfloat(&w, "fps", s->fps);
    jw_kint(&w, "frames", s->nframes);
    jw_kbool(&w, "loop", s->loop);
    jw_key(&w, "tracks");
    jw_arr_begin(&w);
    for (uint32_t t = 0; t < s->ntracks; t++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "bone", s->track_bone[t]);
        jw_key(&w, "rot");
        jw_arr_begin(&w);
        for (uint32_t f = 0; f < s->nframes; f++)
            for (int k = 0; k < 4; k++) jw_float(&w, roundf(s->rot[((size_t)f * s->ntracks + t) * 4 + k] * 1e5f) / 1e5f);
        jw_arr_end(&w);
        jw_key(&w, "pos");
        jw_arr_begin(&w);
        for (uint32_t f = 0; f < s->nframes; f++)
            for (int k = 0; k < 3; k++) jw_float(&w, roundf(s->pos[((size_t)f * s->ntracks + t) * 3 + k] * 100.0f) / 100.0f);
        jw_arr_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "notifies");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < s->nnotifies; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", s->notifies[i].name);
        jw_kfloat(&w, "time", s->notifies[i].time);
        if (s->notifies[i].sound) { // (a Play Sound notify)
            jw_kint(&w, "sound", s->notifies[i].sound);
            jw_kfloat(&w, "volume", s->notifies[i].volume), jw_kfloat(&w, "pitch", s->notifies[i].pitch);
            if (s->notifies[i].bone[0]) jw_kstr(&w, "bone", s->notifies[i].bone);
            jw_kbool(&w, "follow", s->notifies[i].follow);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    if (s->ncurves) { // the morph target curves (keys: time, value, ...)
        jw_key(&w, "curves");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < s->ncurves; i++) {
            jw_obj_begin(&w);
            jw_kstr(&w, "name", s->curve_name[i]);
            jw_key(&w, "keys");
            jw_arr_begin(&w);
            for (uint32_t k = 0; k < s->curve_count[i]; k++) {
                const float* kv = s->curve_keys + (size_t)(s->curve_first[i] + k) * 2;
                jw_float(&w, roundf(kv[0] * 1e5f) / 1e5f), jw_float(&w, roundf(kv[1] * 1e5f) / 1e5f);
            }
            jw_arr_end(&w);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    jw_obj_end(&w);
    bool ok = write_text(s->path, &w);
    jw_free(&w);
    if (ok) s->dirty = false;
    return ok;
}

static AnimSeq* seq_alloc(uint32_t nframes, uint32_t ntracks) {
    AnimSeq* s = calloc(1, sizeof *s); // an asset, not a frame
    if (!s) return NULL;
    s->rot = calloc((size_t)nframes * ntracks * 4, sizeof(float));
    s->pos = calloc((size_t)nframes * ntracks * 3, sizeof(float));
    if (!s->rot || !s->pos) {
        free(s->rot), free(s->pos), free(s);
        return NULL;
    }
    s->nframes = nframes, s->ntracks = ntracks;
    return s;
}

AnimSeq* anim_seq_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < A.nseqs; i++)
        if (A.seqs[i]->hash == h) return A.seqs[i];
    if (A.nseqs >= MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* t = read_text(path, &len);
    if (!t) return NULL;
    JsonDoc doc;
    char err[160];
    AnimSeq* s = NULL;
    if (json_parse(t, len, &doc, err, sizeof err)) {
        const JsonValue* tracks = json_get(&doc.root, "tracks");
        uint32_t nf = (uint32_t)json_get_int(&doc.root, "frames", 1), nt = (uint32_t)json_len(tracks);
        if (nt > ANIM_MAX_BONES) nt = ANIM_MAX_BONES;
        if (nf < 1) nf = 1;
        s = seq_alloc(nf, nt ? nt : 1);
        if (s) {
            snprintf(s->path, sizeof s->path, "%s", path);
            name_of(path, s->name);
            s->hash = h;
            snprintf(s->skeleton, sizeof s->skeleton, "%s", json_get_str(&doc.root, "skeleton", ""));
            s->fps = (float)json_get_num(&doc.root, "fps", 30.0);
            if (s->fps <= 0.0f) s->fps = 30.0f;
            s->loop = json_get_bool(&doc.root, "loop", true);
            s->generator = (uint16_t)json_get_int(&doc.root, "generator", 0);
            s->ntracks = nt;
            s->length = (float)(nf > 1 ? nf - 1 : 1) / s->fps;
            for (uint32_t ti = 0; ti < nt; ti++) {
                const JsonValue* tr = json_at(tracks, ti);
                snprintf(s->track_bone[ti], ANIM_NAME, "%s", json_get_str(tr, "bone", ""));
                const JsonValue* rot = json_get(tr, "rot");
                const JsonValue* pos = json_get(tr, "pos");
                for (uint32_t f = 0; f < nf; f++) {
                    float* q = &s->rot[((size_t)f * nt + ti) * 4];
                    for (int k = 0; k < 4; k++) q[k] = (float)json_num(json_at(rot, (size_t)f * 4 + k), k == 3 ? 1.0 : 0.0);
                    q_norm(q);
                    for (int k = 0; k < 3; k++) s->pos[((size_t)f * nt + ti) * 3 + k] = (float)json_num(json_at(pos, (size_t)f * 3 + k), 0.0);
                }
            }
            const JsonValue* ns = json_get(&doc.root, "notifies");
            for (size_t i = 0; i < json_len(ns) && s->nnotifies < 32; i++) {
                AnimNotify* n = &s->notifies[s->nnotifies++];
                snprintf(n->name, sizeof n->name, "%s", json_get_str(json_at(ns, i), "name", "Notify"));
                n->time = (float)json_get_num(json_at(ns, i), "time", 0.0);
                n->sound = (uint32_t)json_get_num(json_at(ns, i), "sound", 0.0);
                n->volume = (float)json_get_num(json_at(ns, i), "volume", 1.0);
                n->pitch = (float)json_get_num(json_at(ns, i), "pitch", 1.0);
                snprintf(n->bone, sizeof n->bone, "%s", json_get_str(json_at(ns, i), "bone", ""));
                n->follow = (uint8_t)json_get_bool(json_at(ns, i), "follow", false);
            }
            const JsonValue* cv = json_get(&doc.root, "curves"); // (the morph target curves; files before: none)
            for (size_t i = 0; i < json_len(cv) && i < ANIM_MAX_MORPHS; i++) {
                const JsonValue* keys = json_get(json_at(cv, i), "keys");
                uint32_t nk = (uint32_t)(json_len(keys) / 2);
                float* kt = nk ? malloc(sizeof(float) * 2 * nk) : NULL;
                for (uint32_t k = 0; kt && k < nk; k++) kt[k] = (float)json_num(json_at(keys, k * 2), 0.0), kt[nk + k] = (float)json_num(json_at(keys, k * 2 + 1), 0.0);
                if (kt) anim_curve_set(s, json_get_str(json_at(cv, i), "name", "Curve"), kt, kt + nk, nk);
                free(kt);
            }
            A.seqs[A.nseqs++] = s;
        }
        json_free(&doc);
    }
    free(t);
    return s;
}

bool anim_montage_save(AnimMontage* m) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "anim", m->anim);
    jw_kstr(&w, "slot", m->slot);
    jw_kfloat(&w, "blend_in", m->blend_in);
    jw_kfloat(&w, "blend_out", m->blend_out);
    jw_kfloat(&w, "rate", m->rate);
    if (m->root_motion) jw_kbool(&w, "root_motion", true); // (2026-11-23)
    if (m->no_collision) jw_kbool(&w, "no_collision", true);
    if (m->nwarps) {
        jw_key(&w, "warps");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < m->nwarps; i++) {
            jw_obj_begin(&w);
            jw_kstr(&w, "target", m->warps[i].target);
            jw_kfloat(&w, "start", m->warps[i].start), jw_kfloat(&w, "end", m->warps[i].end);
            jw_kint(&w, "flags", m->warps[i].flags);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    if (m->nwindows) { // (2026-11-23)
        jw_key(&w, "windows");
        jw_arr_begin(&w);
        for (uint32_t i = 0; i < m->nwindows; i++) {
            const AnimMontageWindow* wn = &m->windows[i];
            jw_obj_begin(&w);
            jw_kstr(&w, "name", wn->name);
            jw_kfloat(&w, "start", wn->start), jw_kfloat(&w, "end", wn->end);
            jw_kint(&w, "kind", wn->kind);
            if (wn->next[0]) jw_kstr(&w, "next", wn->next);
            if (wn->bone[0]) jw_kstr(&w, "bone", wn->bone), jw_kfloat(&w, "radius", wn->radius);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    if (m->partner[0]) {
        jw_kstr(&w, "partner", m->partner);
        jw_kfloat(&w, "partner_x", m->partner_offset[0]), jw_kfloat(&w, "partner_y", m->partner_offset[1]), jw_kfloat(&w, "partner_z", m->partner_offset[2]);
        jw_kfloat(&w, "partner_yaw", m->partner_yaw), jw_kfloat(&w, "partner_align", m->partner_align);
    }
    jw_key(&w, "sections");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < m->nsections; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", m->sections[i].name);
        jw_kfloat(&w, "start", m->sections[i].start);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_text(m->path, &w);
    jw_free(&w);
    if (ok) m->dirty = false;
    return ok;
}

AnimMontage* anim_montage_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < A.nmontages; i++)
        if (A.montages[i]->hash == h) return A.montages[i];
    if (A.nmontages >= MAX_ASSETS) return NULL;
    char path[700];
    disk_of(vpath, path, sizeof path);
    size_t len;
    char* t = read_text(path, &len);
    if (!t) return NULL;
    JsonDoc doc;
    char err[160];
    AnimMontage* m = NULL;
    if (json_parse(t, len, &doc, err, sizeof err) && (m = calloc(1, sizeof *m))) {
        snprintf(m->path, sizeof m->path, "%s", path);
        name_of(path, m->name);
        m->hash = h;
        snprintf(m->anim, sizeof m->anim, "%s", json_get_str(&doc.root, "anim", ""));
        snprintf(m->slot, sizeof m->slot, "%s", json_get_str(&doc.root, "slot", "DefaultSlot"));
        m->blend_in = (float)json_get_num(&doc.root, "blend_in", 0.25);
        m->blend_out = (float)json_get_num(&doc.root, "blend_out", 0.25);
        m->rate = (float)json_get_num(&doc.root, "rate", 1.0);
        m->root_motion = json_get_bool(&doc.root, "root_motion", false); // (2026-11-23)
        m->no_collision = json_get_bool(&doc.root, "no_collision", false);
        const JsonValue* wa = json_get(&doc.root, "warps");
        for (size_t i = 0; i < json_len(wa) && m->nwarps < ANIM_MAX_WARPS; i++) {
            AnimWarpWindow* wn = &m->warps[m->nwarps++];
            snprintf(wn->target, ANIM_NAME, "%s", json_get_str(json_at(wa, i), "target", "Target"));
            wn->start = (float)json_get_num(json_at(wa, i), "start", 0.0), wn->end = (float)json_get_num(json_at(wa, i), "end", 0.0);
            wn->flags = (uint8_t)json_get_num(json_at(wa, i), "flags", ANIM_WARP_TRANSLATION | ANIM_WARP_ROTATION | ANIM_WARP_IGNORE_Z);
        }
        snprintf(m->partner, ANIM_NAME, "%s", json_get_str(&doc.root, "partner", ""));
        const JsonValue* wins = json_get(&doc.root, "windows"); // (2026-11-23)
        m->nwindows = 0;
        for (size_t i = 0; i < json_len(wins) && m->nwindows < ANIM_MAX_WINDOWS; i++) {
            const JsonValue* o = json_at(wins, i);
            AnimMontageWindow* wn = &m->windows[m->nwindows++];
            memset(wn, 0, sizeof *wn);
            snprintf(wn->name, ANIM_NAME, "%s", json_get_str(o, "name", "Window"));
            wn->start = (float)json_get_num(o, "start", 0.0), wn->end = (float)json_get_num(o, "end", 0.0);
            wn->kind = (uint8_t)json_get_num(o, "kind", 0);
            snprintf(wn->next, ANIM_NAME, "%s", json_get_str(o, "next", ""));
            snprintf(wn->bone, ANIM_NAME, "%s", json_get_str(o, "bone", ""));
            wn->radius = (float)json_get_num(o, "radius", 20.0);
        }
        m->partner_offset[0] = (float)json_get_num(&doc.root, "partner_x", 150.0), m->partner_offset[1] = (float)json_get_num(&doc.root, "partner_y", 0.0);
        m->partner_offset[2] = (float)json_get_num(&doc.root, "partner_z", 0.0);
        m->partner_yaw = (float)json_get_num(&doc.root, "partner_yaw", 180.0), m->partner_align = (float)json_get_num(&doc.root, "partner_align", 0.25);
        const JsonValue* sec = json_get(&doc.root, "sections");
        for (size_t i = 0; i < json_len(sec) && m->nsections < 8; i++) {
            snprintf(m->sections[m->nsections].name, ANIM_NAME, "%s", json_get_str(json_at(sec, i), "name", "Default"));
            m->sections[m->nsections].start = (float)json_get_num(json_at(sec, i), "start", 0.0);
            m->nsections++;
        }
        A.montages[A.nmontages++] = m;
    }
    if (m || doc.arena) json_free(&doc);
    free(t);
    return m;
}

AnimMontage* anim_montage_create(const char* folder_vpath, const AnimSeq* from) {
    if (A.nmontages >= MAX_ASSETS) return NULL;
    char vpath[700], path[800];
    for (int i = 0; i < 1000; i++) {
        snprintf(vpath, sizeof vpath, i ? "%s/AM_%s%d.rmontage" : "%s/AM_%s.rmontage", folder_vpath, from ? from->name : "New", i);
        disk_of(vpath, path, sizeof path);
        if (!re_file_stat(path, NULL, NULL, NULL)) break;
    }
    AnimMontage* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    name_of(path, m->name);
    m->hash = vpath_hash(vpath);
    if (from) snprintf(m->anim, sizeof m->anim, "%s", from->name);
    snprintf(m->slot, sizeof m->slot, "DefaultSlot");
    m->blend_in = m->blend_out = 0.25f, m->rate = 1.0f;
    snprintf(m->sections[0].name, ANIM_NAME, "Default");
    m->nsections = 1;
    if (!anim_montage_save(m)) {
        free(m);
        return NULL;
    }
    A.montages[A.nmontages++] = m;
    return m;
}

// The Content folder's animation assets, loaded once (the editors' lists, the names the graphs use)
static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        scan_dir(path, 1);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot) return;
    vpath_of(path, vpath, sizeof vpath);
    if (!strcmp(dot, ".rskm")) anim_mesh_load(vpath);
    else if (!strcmp(dot, ".ranim")) anim_seq_load(vpath);
    else if (!strcmp(dot, ".rmontage")) anim_montage_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth > 12) return;
    re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (A.scanned) return;
    A.scanned = true;
    scan_dir(A.root, 0);
}

SkelMesh* anim_mesh(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < A.nmeshes; i++)
        if (A.meshes[i]->hash == hash) return A.meshes[i];
    return NULL;
}
SkelMesh* anim_mesh_name(const char* name) {
    scan();
    for (uint32_t i = 0; i < A.nmeshes; i++)
        if (!strcmp(A.meshes[i]->name, name)) return A.meshes[i];
    return NULL;
}
AnimSeq* anim_seq(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < A.nseqs; i++)
        if (A.seqs[i]->hash == hash) return A.seqs[i];
    return NULL;
}
AnimSeq* anim_seq_name(const char* name) {
    scan();
    for (uint32_t i = 0; i < A.nseqs; i++)
        if (!strcmp(A.seqs[i]->name, name)) return A.seqs[i];
    return NULL;
}
AnimMontage* anim_montage_name(const char* name) {
    scan();
    for (uint32_t i = 0; i < A.nmontages; i++)
        if (!strcmp(A.montages[i]->name, name)) return A.montages[i];
    return NULL;
}
uint32_t anim_seq_count(void) { return scan(), A.nseqs; }
AnimSeq* anim_seq_at(uint32_t i) { return i < A.nseqs ? A.seqs[i] : NULL; }
uint32_t anim_montage_count(void) { return scan(), A.nmontages; }
AnimMontage* anim_montage_at(uint32_t i) { return i < A.nmontages ? A.montages[i] : NULL; }
uint32_t anim_mesh_count(void) { return scan(), A.nmeshes; }
SkelMesh* anim_mesh_at(uint32_t i) { return i < A.nmeshes ? A.meshes[i] : NULL; }

// --- Sampling ---

// The tracks of a sequence by bone of a skeleton (by name; -1: the bind pose)
static void track_map(const AnimSeq* s, const SkelMesh* m, int16_t map[ANIM_MAX_BONES]) {
    for (uint32_t b = 0; b < m->nbones; b++) {
        map[b] = -1;
        for (uint32_t t = 0; t < s->ntracks; t++)
            if (!strcmp(s->track_bone[t], m->bones[b].name)) {
                map[b] = (int16_t)t;
                break;
            }
    }
}

// The map of a sequence for a mesh: made on this thread (the poses read it from the workers)
static void seq_warm(AnimSeq* s, const SkelMesh* m) {
    if (!s || !m || s->map_for == m) return;
    track_map(s, m, s->map);
    s->map_for = m;
}

// --- Morph target curves: the sequences' keys, and their values carried with the poses (blended like them) ---

int anim_curve_index(const AnimSeq* s, const char* name) {
    for (uint32_t i = 0; s && name && i < s->ncurves; i++)
        if (!_stricmp(s->curve_name[i], name)) return (int)i;
    return -1;
}

float anim_curve_eval(const AnimSeq* s, uint32_t curve, float t) {
    if (!s || curve >= s->ncurves || !s->curve_count[curve]) return 0.0f;
    const float* k = s->curve_keys + (size_t)s->curve_first[curve] * 2;
    uint32_t n = s->curve_count[curve];
    if (t <= k[0] || n == 1) return k[1];
    if (t >= k[(n - 1) * 2]) return k[(n - 1) * 2 + 1];
    uint32_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        uint32_t m = (lo + hi) / 2;
        if (k[m * 2] <= t) lo = m;
        else hi = m;
    }
    float t0 = k[lo * 2], t1 = k[hi * 2], u = t1 - t0 > 1e-9f ? (t - t0) / (t1 - t0) : 0.0f;
    return k[lo * 2 + 1] + (k[hi * 2 + 1] - k[lo * 2 + 1]) * u;
}

bool anim_curve_set(AnimSeq* s, const char* name, const float* times, const float* values, uint32_t n) {
    if (!s || !name || !name[0]) return false;
    int ci = anim_curve_index(s, name);
    if (ci < 0 && !n) return true;
    if (ci < 0) {
        if (s->ncurves >= ANIM_MAX_MORPHS) return false;
        ci = (int)s->ncurves++;
        snprintf(s->curve_name[ci], ANIM_MORPH_NAME, "%s", name);
        s->curve_count[ci] = 0;
    }
    // the keys made again: every other curve's, then this one's (an asset's edit: not a frame)
    uint32_t total = n;
    for (uint32_t i = 0; i < s->ncurves; i++)
        if ((int)i != ci) total += s->curve_count[i];
    float* nk = total ? malloc(sizeof(float) * 2 * total) : NULL;
    if (total && !nk) return false;
    uint32_t at = 0;
    for (uint32_t i = 0; i < s->ncurves; i++) {
        uint32_t first = at;
        if ((int)i == ci) {
            for (uint32_t k = 0; k < n; k++) nk[(at + k) * 2] = times[k], nk[(at + k) * 2 + 1] = values[k];
            for (uint32_t a = 1; a < n; a++) // (sorted by time: an insertion sort, the keys are few)
                for (uint32_t b = a; b > 0 && nk[(at + b - 1) * 2] > nk[(at + b) * 2]; b--) {
                    float t0 = nk[(at + b) * 2], v0 = nk[(at + b) * 2 + 1];
                    nk[(at + b) * 2] = nk[(at + b - 1) * 2], nk[(at + b) * 2 + 1] = nk[(at + b - 1) * 2 + 1];
                    nk[(at + b - 1) * 2] = t0, nk[(at + b - 1) * 2 + 1] = v0;
                }
            at += n;
            s->curve_count[i] = n;
        } else {
            memcpy(nk + (size_t)at * 2, s->curve_keys + (size_t)s->curve_first[i] * 2, sizeof(float) * 2 * s->curve_count[i]);
            at += s->curve_count[i];
        }
        s->curve_first[i] = first;
    }
    free(s->curve_keys);
    s->curve_keys = nk, s->ncurve_keys = total;
    if (!n) { // (removed: the last moves into its place)
        uint32_t last = s->ncurves - 1;
        if ((uint32_t)ci != last) {
            memcpy(s->curve_name[ci], s->curve_name[last], ANIM_MORPH_NAME);
            s->curve_first[ci] = s->curve_first[last], s->curve_count[ci] = s->curve_count[last];
        }
        s->ncurves--;
    }
    s->curve_map_for = NULL;
    return true;
}

// The curves' values carried with the poses: a small table per thread, by the pose's buffer. A sequence sampled into a
// buffer puts its curves there (by the mesh's morph targets); a blend of two (or n) buffers blends them, a curve a pose
// lacks counting 0. The final pose's are the instance's (pose_one). Nothing to do while no sequence has curves
typedef struct PoseCurves { const AnimXf* buf; uint64_t set; float w[ANIM_MAX_MORPHS]; } PoseCurves;
static _Thread_local PoseCurves t_pc[16];

static PoseCurves* pc_find(const AnimXf* buf, bool make) {
    for (uint32_t i = 0; i < t_npc; i++)
        if (t_pc[i].buf == buf) return &t_pc[i];
    if (!make) return NULL;
    PoseCurves* p = &t_pc[t_npc < 16 ? t_npc++ : 15]; // (16 buffers in one pose: the last reused)
    p->buf = buf, p->set = 0;
    return p;
}
static void pc_clear(const AnimXf* buf) {
    PoseCurves* p = t_npc ? pc_find(buf, false) : NULL;
    if (p) p->set = 0;
}
static void pc_sample(const AnimSeq* s, const SkelMesh* m, float t, const AnimXf* out) {
    if (s->curve_map_for != m) { // (on a worker: a byte per curve, the same values whichever thread writes them)
        for (uint32_t i = 0; i < s->ncurves; i++) ((AnimSeq*)s)->curve_map[i] = (int8_t)anim_morph_index(m, s->curve_name[i]);
        ((AnimSeq*)s)->curve_map_for = m;
    }
    PoseCurves* p = pc_find(out, true);
    p->set = 0;
    for (uint32_t i = 0; i < s->ncurves; i++) {
        int mi = s->curve_map[i];
        if (mi < 0) continue;
        p->w[mi] = anim_curve_eval(s, i, t);
        p->set |= 1ull << mi;
    }
}
static void pc_blend2(const AnimXf* a, const AnimXf* b, float f, const AnimXf* out) {
    const PoseCurves *pa = pc_find(a, false), *pb = pc_find(b, false);
    if (!pa && !pb) {
        pc_clear(out);
        return;
    }
    float w[ANIM_MAX_MORPHS];
    uint64_t set = (pa ? pa->set : 0) | (pb ? pb->set : 0);
    for (uint32_t i = 0; i < ANIM_MAX_MORPHS; i++)
        if (set >> i & 1u) {
            float va = pa && (pa->set >> i & 1u) ? pa->w[i] : 0.0f, vb = pb && (pb->set >> i & 1u) ? pb->w[i] : 0.0f;
            w[i] = va + (vb - va) * f;
        }
    PoseCurves* po = pc_find(out, true);
    po->set = set;
    for (uint32_t i = 0; i < ANIM_MAX_MORPHS; i++)
        if (set >> i & 1u) po->w[i] = w[i];
}
static void pc_blendn(const AnimXf* const* poses, const float* wt, uint32_t count, const AnimXf* out) {
    float w[ANIM_MAX_MORPHS] = { 0 };
    uint64_t set = 0;
    for (uint32_t k = 0; k < count; k++) {
        const PoseCurves* p = pc_find(poses[k], false);
        if (!p || wt[k] <= 0.0f) continue;
        set |= p->set;
        for (uint32_t i = 0; i < ANIM_MAX_MORPHS; i++)
            if (p->set >> i & 1u) w[i] += p->w[i] * wt[k];
    }
    if (!set) {
        pc_clear(out);
        return;
    }
    PoseCurves* po = pc_find(out, true);
    po->set = set;
    memcpy(po->w, w, sizeof w);
}

static void sample_mapped(const AnimSeq* s, const SkelMesh* m, const int16_t* map, float t, AnimXf* out) {
    float f = s->nframes > 1 ? t * s->fps : 0.0f;
    float span = (float)(s->nframes - 1);
    if (s->loop && span > 0.0f) {
        f = fmodf(f, span);
        if (f < 0.0f) f += span;
    } else
        f = fminf(fmaxf(f, 0.0f), span);
    uint32_t i0 = (uint32_t)f, i1 = i0 + 1 < s->nframes ? i0 + 1 : (s->loop ? 0 : i0);
    float a = f - (float)i0;
    for (uint32_t b = 0; b < m->nbones; b++) {
        out[b] = m->bones[b].bind;
        int tr = map[b];
        if (tr < 0) continue;
        const float* q0 = &s->rot[((size_t)i0 * s->ntracks + (uint32_t)tr) * 4];
        const float* q1 = &s->rot[((size_t)i1 * s->ntracks + (uint32_t)tr) * 4];
        q_nlerp(q0, q1, a, out[b].q);
        const float* p0 = &s->pos[((size_t)i0 * s->ntracks + (uint32_t)tr) * 3];
        const float* p1 = &s->pos[((size_t)i1 * s->ntracks + (uint32_t)tr) * 3];
        for (int k = 0; k < 3; k++) out[b].t[k] = p0[k] + (p1[k] - p0[k]) * a;
    }
    if (s->ncurves && m->nmorphs) pc_sample(s, m, s->fps > 0.0f ? f / s->fps : 0.0f, out); // (the morph curves, at the same time)
    else if (t_npc) pc_clear(out);
}

void anim_sample(const AnimSeq* s, const SkelMesh* m, float t, AnimXf* out) {
    int16_t map[ANIM_MAX_BONES];
    track_map(s, m, map);
    sample_mapped(s, m, map, t, out);
}

// --- The Mannequin: made by the engine (bones, parts, animations, its Animation Blueprint and a montage) ---

typedef struct MBone { const char* name; int parent; float t[3]; } MBone;
static const MBone g_mbones[] = {
    { "root", -1, { 0, 0, 0 } },          { "pelvis", 0, { 0, 0, 95 } },        { "spine_01", 1, { 0, 0, 10 } },
    { "spine_02", 2, { 0, 0, 15 } },      { "spine_03", 3, { 0, 0, 15 } },      { "neck_01", 4, { 0, 0, 17 } },
    { "head", 5, { 0, 0, 9 } },           { "clavicle_l", 4, { 0, -4, 12 } },   { "upperarm_l", 7, { 0, -15, 0 } },
    { "lowerarm_l", 8, { 0, 0, -28 } },   { "hand_l", 9, { 0, 0, -26 } },       { "clavicle_r", 4, { 0, 4, 12 } },
    { "upperarm_r", 11, { 0, 15, 0 } },   { "lowerarm_r", 12, { 0, 0, -28 } },  { "hand_r", 13, { 0, 0, -26 } },
    { "thigh_l", 1, { 0, -10, -5 } },     { "calf_l", 15, { 0, 0, -42 } },      { "foot_l", 16, { 0, 0, -41 } },
    { "thigh_r", 1, { 0, 10, -5 } },      { "calf_r", 18, { 0, 0, -42 } },      { "foot_r", 19, { 0, 0, -41 } },
};
enum { MB_ROOT, MB_PELVIS, MB_SPINE1, MB_SPINE2, MB_SPINE3, MB_NECK, MB_HEAD, MB_CLAV_L, MB_UARM_L, MB_LARM_L, MB_HAND_L,
       MB_CLAV_R, MB_UARM_R, MB_LARM_R, MB_HAND_R, MB_THIGH_L, MB_CALF_L, MB_FOOT_L, MB_THIGH_R, MB_CALF_R, MB_FOOT_R, MB_COUNT };

static void add_part(SkelMesh* m, int bone, int shape, const float at[3], const float dir[3], const float size[3], const float color[3]) {
    if (m->nparts >= ANIM_MAX_PARTS) return;
    SkPart* p = &m->parts[m->nparts++];
    memset(p, 0, sizeof *p);
    p->bone = (int16_t)bone;
    p->mesh = (uint32_t)shape;
    xf_identity(&p->local);
    memcpy(p->local.t, at, 12);
    if (dir) q_from_z(dir, p->local.q);
    for (int k = 0; k < 3; k++) p->local.s[k] = size[k] / 100.0f;
    memcpy(p->color, color, 12);
    p->roughness = 0.42f;
}

// A limb: a cylinder from the bone to its child, of radius r (and a ball at the joint)
static void limb(SkelMesh* m, int bone, int child, float r, const float color[3], const float joint[3]) {
    const float* d = g_mbones[child].t;
    float at[3] = { d[0] * 0.5f, d[1] * 0.5f, d[2] * 0.5f }, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    add_part(m, bone, SHAPE_CYLINDER, at, d, (float[3]){ r * 2.0f, r * 2.0f, len }, color);
    if (joint) add_part(m, bone, SHAPE_SPHERE, (float[3]){ 0, 0, 0 }, NULL, (float[3]){ r * 2.1f, r * 2.1f, r * 2.1f }, joint);
}

static SkelMesh* make_mannequin(const char* path, const char* vpath) {
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
    const float body[3] = { 0.62f, 0.63f, 0.66f }, dark[3] = { 0.16f, 0.17f, 0.19f }, accent[3] = { 0.85f, 0.45f, 0.12f };
    add_part(m, MB_PELVIS, SHAPE_CUBE, (float[3]){ 0, 0, 2 }, NULL, (float[3]){ 22, 32, 18 }, dark);
    limb(m, MB_SPINE1, MB_SPINE2, 13.0f, body, NULL);
    limb(m, MB_SPINE2, MB_SPINE3, 15.0f, body, NULL);
    add_part(m, MB_SPINE3, SHAPE_CUBE, (float[3]){ 0, 0, 9 }, NULL, (float[3]){ 22, 40, 20 }, body); // the chest
    add_part(m, MB_SPINE3, SHAPE_CUBE, (float[3]){ 9, 0, 6 }, NULL, (float[3]){ 6, 24, 12 }, accent); // the chest plate
    limb(m, MB_NECK, MB_HEAD, 5.5f, dark, NULL);
    add_part(m, MB_HEAD, SHAPE_SPHERE, (float[3]){ 1, 0, 11 }, NULL, (float[3]){ 21, 19, 25 }, body);
    add_part(m, MB_HEAD, SHAPE_CUBE, (float[3]){ 9, 0, 13 }, NULL, (float[3]){ 4, 13, 4 }, accent); // the visor
    for (int side = 0; side < 2; side++) {
        int ua = side ? MB_UARM_R : MB_UARM_L, la = side ? MB_LARM_R : MB_LARM_L, ha = side ? MB_HAND_R : MB_HAND_L;
        int th = side ? MB_THIGH_R : MB_THIGH_L, ca = side ? MB_CALF_R : MB_CALF_L, fo = side ? MB_FOOT_R : MB_FOOT_L;
        limb(m, ua, la, 5.2f, body, dark);
        limb(m, la, ha, 4.4f, body, dark);
        add_part(m, ha, SHAPE_CUBE, (float[3]){ 1, 0, -8 }, NULL, (float[3]){ 9, 5, 16 }, dark);
        limb(m, th, ca, 7.8f, body, dark);
        limb(m, ca, fo, 6.0f, body, dark);
        add_part(m, fo, SHAPE_CUBE, (float[3]){ 5, 0, -4 }, NULL, (float[3]){ 26, 11, 9 }, dark);
    }
    mesh_prepare(m);
    return m;
}

// A sequence of the Mannequin by a function of the time (fills the frames)
static void add_notify(AnimSeq* s, const char* name, float t) {
    if (s->nnotifies >= 32) return;
    snprintf(s->notifies[s->nnotifies].name, ANIM_NAME, "%s", name);
    s->notifies[s->nnotifies++].time = t;
}

#include "anim_motion.inc"

#include "anim_creatures.inc"
#include "anim_pack.inc"

// A new Animation Blueprint of the Mannequin's skeleton: its Event Graph sets Speed and IsInAir from the pawn; its Anim
// Graph empty but for Foot IK, the lean and the chest-to-head chain (NULL: it exists, or could not be made)
static AnimGraph* abp_begin(const char* folder, const char* name, const char* skeleton, BpAsset** out) {
    if (bp_find_name(BPA_CLASS, name)) return NULL;
    BpAsset* a = bp_create(BPA_CLASS, folder, name, "Anim Instance");
    if (!a) return NULL;
    *out = a;
    while (a->graphs[0].nnodes) bp_remove_node(&a->graphs[0], a->graphs[0].nodes[0].id);
    BpGraph* g = &a->graphs[0];
    BpVar* sp = &a->vars[a->nvars++];
    memset(sp, 0, sizeof *sp);
    snprintf(sp->name, sizeof sp->name, "Speed"), sp->type = BPT_FLOAT;
    BpVar* air = &a->vars[a->nvars++];
    memset(air, 0, sizeof *air);
    snprintf(air->name, sizeof air->name, "IsInAir"), air->type = BPT_BOOL;
    BpNode* ev = bp_add_node(a, g, BPK_EVENT_UPDATE_ANIM, -700, 0, NULL, NULL);
    BpNode* own = bp_add_node(a, g, BPK_TRY_GET_PAWN_OWNER, -700, 160, NULL, NULL);
    BpNode* vel = bp_add_node(a, g, BPK_GET_ACTOR_VELOCITY, -420, 160, NULL, NULL);
    BpNode* brk = bp_add_node(a, g, BPK_BREAK_VECTOR, -180, 160, NULL, NULL);
    BpNode* mk = bp_add_node(a, g, BPK_MAKE_VECTOR, 40, 160, NULL, NULL);
    BpNode* len = bp_add_node(a, g, BPK_VLEN, 260, 160, NULL, NULL);
    BpNode* set1 = bp_add_node(a, g, BPK_VAR_SET, 420, 0, "Speed", NULL);
    BpNode* fall = bp_add_node(a, g, BPK_IS_FALLING, 420, 220, NULL, NULL);
    BpNode* set2 = bp_add_node(a, g, BPK_VAR_SET, 700, 0, "IsInAir", NULL);
    bp_link(g, own->id, 0, vel->id, 0);
    bp_link(g, vel->id, 1, brk->id, 0);
    bp_link(g, brk->id, 1, mk->id, 0);
    bp_link(g, brk->id, 2, mk->id, 1);
    bp_link(g, mk->id, 3, len->id, 0);
    bp_link(g, ev->id, 0, set1->id, 0);
    bp_link(g, len->id, 1, set1->id, 1);
    bp_link(g, own->id, 0, fall->id, 0);
    bp_link(g, set1->id, 2, set2->id, 0);
    bp_link(g, fall->id, 1, set2->id, 1);
    snprintf(ev->comment, sizeof ev->comment, "Speed and IsInAir from the pawn: the Anim Graph's transitions read them");
    AnimGraph* ag = anim_graph_of(a, true);
    if (!ag) return NULL;
    memset(ag, 0, sizeof *ag);
    snprintf(ag->skeleton, ANIM_NAME, "%s", skeleton);
    snprintf(ag->upper_bone, ANIM_NAME, "spine_01");
    // the feet on the ground (stairs, slopes) and the chest-to-head chain ready for an Aim Offset (set its Yaw / Pitch)
    ag->foot_ik = true, ag->foot_reach = 45.0f;
    snprintf(ag->foot_l, ANIM_NAME, "foot_l"), snprintf(ag->foot_r, ANIM_NAME, "foot_r");
    ag->lean = 0.5f;
    static const char* const aim[4] = { "spine_01", "spine_02", "spine_03", "head" };
    static const float share[4] = { 0.2f, 0.25f, 0.25f, 0.3f };
    for (int k = 0; k < 4; k++) snprintf(ag->aim_bones[k], ANIM_NAME, "%s", aim[k]), ag->aim_share[k] = share[k];
    return ag;
}

// Its Jump state (state 1, the Mannequin's MM_Jump) by IsInAir, from and back to state 0
static void abp_jump(AnimGraph* ag) {
    AnimState* ju = &ag->states[ag->nstates++];
    snprintf(ju->name, ANIM_NAME, "Jump");
    ju->kind = ANIM_STATE_SEQUENCE, ju->loop = true, ju->rate = 1.0f, ju->x = 320, ju->y = 0;
    snprintf(ju->clip, ANIM_NAME, "MM_Jump");
    ag->trans[ag->ntrans++] = (AnimTransition){ 0, 1, ANIM_OP_TRUE, "IsInAir", 0.0f, 0.15f };
    ag->trans[ag->ntrans++] = (AnimTransition){ 1, 0, ANIM_OP_FALSE, "IsInAir", 0.0f, 0.2f };
}

// The Mannequin's Animation Blueprint: Speed and IsInAir from the pawn; Locomotion (Idle / Walk / Run by Speed) and
// Jump; the UpperBody slot from spine_01
static void make_abp(const char* folder, const char* name, bool motion_matching) {
    BpAsset* a = NULL;
    AnimGraph* ag = abp_begin(folder, name, "SK_Mannequin", &a);
    if (ag) {
        AnimState* lo = &ag->states[ag->nstates++];
        snprintf(lo->name, ANIM_NAME, "Locomotion");
        lo->kind = motion_matching ? ANIM_STATE_MOTION_MATCH : ANIM_STATE_BLEND1D, lo->loop = true, lo->rate = 1.0f, lo->x = 0, lo->y = 0;
        snprintf(lo->param, ANIM_NAME, "Speed");
        lo->nsamples = 4; // (idle, walk, jog, run at their speeds: anim_motion.inc)
        snprintf(lo->samples[0], ANIM_NAME, "MM_Idle"), lo->sample_at[0] = 0.0f;
        snprintf(lo->samples[1], ANIM_NAME, "MM_Walk"), lo->sample_at[1] = 120.0f;
        snprintf(lo->samples[2], ANIM_NAME, "MM_Jog"), lo->sample_at[2] = 260.0f;
        snprintf(lo->samples[3], ANIM_NAME, "MM_Run"), lo->sample_at[3] = 420.0f;
        abp_jump(ag);
    }
    if (a) bp_save(a);
}

#include "anim_fight.inc" // the fighting clips, montages and ABP_Fighter (sugestão #34)
#include "anim_getup.inc" // (Q10) the get-up clips of the Active Ragdoll (face down, on its back) and their montages
#include "anim_mm_examples.inc"

void anim_init(const char* content_root, bool make_defaults) {
    snprintf(A.root, sizeof A.root, "%s", content_root);
    for (char* c = A.root; *c; c++)
        if (*c == '\\') *c = '/';
    A.scanned = false;
    // the Mannequin, its animations, its Animation Blueprint and a montage (made once, then the project's to edit)
    char dir[700], path[800], vpath[800];
    snprintf(dir, sizeof dir, "%s/Characters/Mannequin", A.root);
    snprintf(path, sizeof path, "%s/SK_Mannequin.rskm", dir);
    if (!make_defaults) return;
    if (!re_file_stat(path, NULL, NULL, NULL) && re_make_dirs(dir)) {
        vpath_of(path, vpath, sizeof vpath);
        SkelMesh* m = make_mannequin(path, vpath);
        if (m && anim_mesh_save(m)) {
            for (size_t i = 0; i < sizeof g_core_clips / sizeof g_core_clips[0]; i++) engine_clip(m, dir, &g_core_clips[i], CLIP_MAKE);
            snprintf(path, sizeof path, "%s/AM_Wave.rmontage", dir);
            AnimMontage mo = { 0 };
            snprintf(mo.path, sizeof mo.path, "%s", path);
            snprintf(mo.anim, ANIM_NAME, "MM_Wave");
            snprintf(mo.slot, ANIM_NAME, "UpperBody");
            mo.blend_in = mo.blend_out = 0.25f, mo.rate = 1.0f;
            snprintf(mo.sections[0].name, ANIM_NAME, "Default");
            mo.nsections = 1;
            anim_montage_save(&mo);
        }
        free(m);
    }
    make_bodies(dir); // the male and the female Mannequins (its skeleton, their own builds)
    make_abp("/Content/Characters/Mannequin", "ABP_Mannequin", false);
    make_abp("/Content/Characters/Mannequin", "ABP_Mannequin_MM", true); // (Locomotion by Motion Matching)
    engine_clips_update(dir); // (a project made by an older engine: its engine-made clips made again, notifies kept)
}

// --- The Anim Graph inside an Animation Blueprint ---

AnimGraph* anim_graph_of(BpAsset* a, bool create) {
    if (!a || a->kind != BPA_CLASS || !bp_is_child_of(a, "Anim Instance")) return NULL;
    if (!a->anim && create) {
        a->anim = calloc(1, sizeof(AnimGraph)); // an asset, not a frame
        if (a->anim) {
            AnimGraph* g = a->anim;
            snprintf(g->upper_bone, ANIM_NAME, "spine_01");
            AnimState* s = &g->states[g->nstates++];
            snprintf(s->name, ANIM_NAME, "Idle");
            s->kind = ANIM_STATE_SEQUENCE, s->loop = true, s->rate = 1.0f;
            anim_graph_ensure_nodes(g); // (State Machine -> the montages' slots -> Output Pose)
        }
    }
    return a->anim;
}

static void nodes_write(const AnimGraph* g, JsonWriter* w); // (anim_nodes.inc)
static void nodes_read(AnimGraph* g, const JsonValue* v);

void anim_graph_write(const AnimGraph* g_in, JsonWriter* w) {
    static AnimGraph tmp; // (the caller's graph is const: an old one gets its nodes in a copy)
    const AnimGraph* g = g_in;
    if (!g_in->nnodes) {
        tmp = *g_in;
        anim_graph_ensure_nodes(&tmp);
        g = &tmp;
    }
    jw_obj_begin(w);
    jw_kstr(w, "skeleton", g->skeleton);
    jw_kstr(w, "upper_bone", g->upper_bone);
    jw_kint(w, "entry", g->entry);
    jw_key(w, "states");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < g->nstates; i++) {
        const AnimState* s = &g->states[i];
        jw_obj_begin(w);
        jw_kstr(w, "name", s->name);
        jw_kfloats(w, "pos", (float[]){ s->x, s->y }, 2);
        jw_kint(w, "kind", s->kind);
        if (s->machine) jw_kint(w, "machine", s->machine);
        jw_kbool(w, "loop", s->loop);
        jw_kfloat(w, "rate", s->rate);
        jw_kstr(w, "clip", s->clip);
        jw_kstr(w, "clip2", s->clip2);
        jw_kstr(w, "param", s->param);
        if (s->kind == ANIM_STATE_BLEND2D || s->kind == ANIM_STATE_MOTION_MATCH) jw_kstr(w, "param2", s->param2);
        if (s->kind == ANIM_STATE_MOTION_MATCH && s->mmgen[ANIM_MMGEN_SET] == 1.0f) jw_kfloats(w, "mmgen", s->mmgen, ANIM_MMGEN);
        if (s->axis[1] != s->axis[0] || s->axis[4] != 0.0f) jw_kfloats(w, "axis", s->axis, 6);
        jw_key(w, "samples");
        jw_arr_begin(w);
        for (uint32_t k = 0; k < s->nsamples; k++) {
            jw_obj_begin(w);
            jw_kstr(w, "clip", s->samples[k]);
            jw_kfloat(w, "at", s->sample_at[k]);
            if (s->kind == ANIM_STATE_BLEND2D) jw_kfloat(w, "y", s->sample_y[k]);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "transitions");
    jw_arr_begin(w);
    for (uint32_t i = 0; i < g->ntrans; i++) {
        const AnimTransition* t = &g->trans[i];
        jw_obj_begin(w);
        jw_kint(w, "from", t->from);
        jw_kint(w, "to", t->to);
        jw_kint(w, "op", t->op);
        jw_kstr(w, "var", t->var);
        jw_kfloat(w, "value", t->value);
        jw_kfloat(w, "blend", t->blend);
        if (t->curve) jw_kint(w, "curve", t->curve);
        if (t->priority) jw_kint(w, "priority", t->priority);
        if (t->nmore) {
            jw_key(w, "more");
            jw_arr_begin(w);
            for (uint32_t k = 0; k < t->nmore && k < 2; k++) {
                jw_obj_begin(w);
                jw_kint(w, "op", t->more[k].op);
                jw_kstr(w, "var", t->more[k].var);
                jw_kfloat(w, "value", t->more[k].value);
                jw_obj_end(w);
            }
            jw_arr_end(w);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_kstr(w, "aim_yaw", g->aim_yaw);
    jw_kstr(w, "aim_pitch", g->aim_pitch);
    jw_key(w, "aim_bones");
    jw_arr_begin(w);
    for (int k = 0; k < 4; k++)
        if (g->aim_bones[k][0]) {
            jw_obj_begin(w);
            jw_kstr(w, "bone", g->aim_bones[k]);
            jw_kfloat(w, "share", g->aim_share[k]);
            jw_obj_end(w);
        }
    jw_arr_end(w);
    jw_kbool(w, "foot_ik", g->foot_ik);
    jw_kstr(w, "foot_l", g->foot_l);
    jw_kstr(w, "foot_r", g->foot_r);
    jw_kfloat(w, "foot_reach", g->foot_reach);
    jw_kfloat(w, "lean", g->lean);
    nodes_write(g, w);
    jw_obj_end(w);
}

void anim_graph_read(AnimGraph* g, const JsonValue* v) {
    static uint32_t revs;
    memset(g, 0, sizeof *g);
    g->rev = ++revs;
    snprintf(g->skeleton, ANIM_NAME, "%s", json_get_str(v, "skeleton", ""));
    snprintf(g->upper_bone, ANIM_NAME, "%s", json_get_str(v, "upper_bone", "spine_01"));
    const JsonValue* st = json_get(v, "states");
    for (size_t i = 0; i < json_len(st) && g->nstates < ANIM_MAX_STATES; i++) {
        const JsonValue* o = json_at(st, i);
        AnimState* s = &g->states[g->nstates++];
        snprintf(s->name, ANIM_NAME, "%s", json_get_str(o, "name", "State"));
        float pos[2] = { 0, 0 };
        json_get_floats(o, "pos", pos, 2);
        s->x = pos[0], s->y = pos[1];
        s->kind = (uint8_t)json_get_int(o, "kind", 0);
        s->machine = (uint8_t)json_get_int(o, "machine", 0);
        if (s->machine >= ANIM_MAX_MACHINES) s->machine = 0;
        s->loop = json_get_bool(o, "loop", true);
        s->rate = (float)json_get_num(o, "rate", 1.0);
        snprintf(s->clip, ANIM_NAME, "%s", json_get_str(o, "clip", ""));
        snprintf(s->clip2, ANIM_NAME, "%s", json_get_str(o, "clip2", ""));
        snprintf(s->param, ANIM_NAME, "%s", json_get_str(o, "param", ""));
        snprintf(s->param2, ANIM_NAME, "%s", json_get_str(o, "param2", ""));
        json_get_floats(o, "mmgen", s->mmgen, ANIM_MMGEN);
        json_get_floats(o, "axis", s->axis, 6);
        const JsonValue* sm = json_get(o, "samples");
        for (size_t k = 0; k < json_len(sm) && s->nsamples < ANIM_BLEND_SAMPLES; k++) {
            snprintf(s->samples[s->nsamples], ANIM_NAME, "%s", json_get_str(json_at(sm, k), "clip", ""));
            s->sample_y[s->nsamples] = (float)json_get_num(json_at(sm, k), "y", 0.0);
            s->sample_at[s->nsamples++] = (float)json_get_num(json_at(sm, k), "at", 0.0);
        }
    }
    const JsonValue* tr = json_get(v, "transitions");
    for (size_t i = 0; i < json_len(tr) && g->ntrans < ANIM_MAX_TRANS; i++) {
        const JsonValue* o = json_at(tr, i);
        AnimTransition* t = &g->trans[g->ntrans];
        memset(t, 0, sizeof *t);
        int from = json_get_int(o, "from", 0), to = json_get_int(o, "to", 0);
        if (from < 0 || to < 0 || (from >= (int)g->nstates && from != ANIM_ANY_STATE) || to >= (int)g->nstates) continue;
        t->curve = (uint8_t)json_get_int(o, "curve", 0);
        t->priority = (uint8_t)json_get_int(o, "priority", 0);
        const JsonValue* mr = json_get(o, "more");
        for (size_t k = 0; k < json_len(mr) && t->nmore < 2; k++) {
            t->more[t->nmore].op = (uint8_t)json_get_int(json_at(mr, k), "op", 0);
            snprintf(t->more[t->nmore].var, ANIM_NAME, "%s", json_get_str(json_at(mr, k), "var", ""));
            t->more[t->nmore++].value = (float)json_get_num(json_at(mr, k), "value", 0.0);
        }
        t->from = (uint8_t)from, t->to = (uint8_t)to;
        t->op = (uint8_t)json_get_int(o, "op", 0);
        snprintf(t->var, ANIM_NAME, "%s", json_get_str(o, "var", ""));
        t->value = (float)json_get_num(o, "value", 0.0);
        t->blend = (float)json_get_num(o, "blend", 0.2);
        g->ntrans++;
    }
    int e = json_get_int(v, "entry", 0);
    g->entry = (uint32_t)(e >= 0 && e < (int)g->nstates ? e : 0);
    snprintf(g->aim_yaw, ANIM_NAME, "%s", json_get_str(v, "aim_yaw", ""));
    snprintf(g->aim_pitch, ANIM_NAME, "%s", json_get_str(v, "aim_pitch", ""));
    const JsonValue* ab = json_get(v, "aim_bones");
    for (size_t k = 0; k < json_len(ab) && k < 4; k++) {
        snprintf(g->aim_bones[k], ANIM_NAME, "%s", json_get_str(json_at(ab, k), "bone", ""));
        g->aim_share[k] = (float)json_get_num(json_at(ab, k), "share", 0.25);
    }
    g->foot_ik = json_get_bool(v, "foot_ik", false);
    snprintf(g->foot_l, ANIM_NAME, "%s", json_get_str(v, "foot_l", "foot_l"));
    snprintf(g->foot_r, ANIM_NAME, "%s", json_get_str(v, "foot_r", "foot_r"));
    g->foot_reach = (float)json_get_num(v, "foot_reach", 45.0);
    g->lean = (float)json_get_num(v, "lean", 0.0);
    nodes_read(g, v);
    if (!g->nmachines) g->machine_entry[0] = (uint8_t)g->entry;
    anim_graph_ensure_nodes(g); // (an old file: its settings as nodes)
}

// --- Run time ---

typedef struct Clip { AnimSeq* seq; int16_t map[ANIM_MAX_BONES]; } Clip;
// A state machine's run: its state, the one it blends from, their times, the blend (AnimTransition.curve)
typedef struct MachineRun { uint32_t state, prev; float state_t, prev_t, blend_t, blend_len; uint8_t curve; } MachineRun;
// The view point (the camera, set each frame by the editor / the game): the Look At nodes turn toward it
static float A_view[3];
static bool A_view_ok;
void anim_set_view_point(const float p[3]) {
    if (p) memcpy(A_view, p, sizeof A_view);
    A_view_ok = p != NULL;
    ikphys_set_view(p); // (Q27.5: the active ragdolls' budget)
}

typedef struct AnimInst {
    Entity e;               // the entity with CSkeletalMesh (an actor, or a Blueprint's component)
    Entity actor;           // the actor it belongs to (the pawn the Anim Blueprint asks for)
    Entity abp;             // its Anim Blueprint's instance (0: none)
    SkelMesh* mesh;
    AnimGraph* graph;
    uint32_t nparts;
    Entity parts[ANIM_MAX_PARTS];
    uint32_t nskins;
    Entity skins[ANIM_MAX_SKINS];
    // the state machines (AnimGraph.machines), the pose graph's nodes: their times, the bool / int blends' weights,
    // their bones and masks (by graph revision), the mirror's pairs, the inertialization's poses (allocated on use)
    MachineRun sm[ANIM_MAX_MACHINES];
    float node_t[ANIM_MAX_NODES], node_w[ANIM_MAX_NODES][ANIM_NODE_IN];
    int16_t node_bone[ANIM_MAX_NODES][4];
    uint64_t node_mask[ANIM_MAX_NODES][(ANIM_MAX_BONES + 63) / 64];
    int16_t mirror_of[ANIM_MAX_BONES];
    uint64_t cache_visited;
    AnimXf* inert; // last input, last output, offset (3 x ANIM_MAX_BONES)
    int inert_node;
    bool inert_ok;
    // a single animation (Play Animation / the component's Animation)
    Clip single;
    float time, rate;
    bool loop;
    // the montage
    AnimMontage* montage;
    Clip mclip;
    float mtime, mweight, mrate, mblend_out;
    bool mstopping, mcut; // (mcut: stopped before its end: its root motion fades with its weight)
    const struct RmTrack* rm_tr; // (2026-11-23) its root path (anim_warp.inc), when it has root motion
    uint64_t upper_mask[(ANIM_MAX_BONES + 63) / 64]; // the UpperBody slot's bones
    bool has_upper;
    // the frame
    float acc;              // the time waiting (a far mesh updates every few frames)
    bool update, dirty;
    float ms;
    const char* state_name;
    float var_cache[16];    // the graph's variables this frame (by state / transition order)
    // Aim Offset and Foot IK: the bones found once per graph revision; the feet's ground offsets (smoothed)
    const AnimGraph* ik_for;
    uint32_t ik_rev;
    int16_t aim_b[4], leg_b[2][3], pelvis_b; // (-1: none) leg: thigh, calf, foot
    float foot_off[2], pelvis_off;
    float lean_yaw, lean_rate, lean_speed, lean_accel; // (the turn rate deg/s and the speed's change, smoothed)
    // (2026-11-23) Orientation / Stride Warping: the Character's move (its angle from the facing, deg; its speed cm/s) and
    // their states (the warp's turn, the animation's speed from its planted foot, the stride's scale)
    float mv_angle, mv_speed, ow_angle, sw_anim_speed, sw_prev[2], sw_scale;
    float flinch_dir[2], flinch_x, flinch_v; // (2026-11-23) a hit's reaction: away from it (the world's x, y), a spring's place and speed
    bool sw_has;
    // Motion Matching (anim_mm.inc): the group playing, the one fading out
    struct { struct MmDb* db; AnimMmPlay p; float speed_prev, accel, brake_from; bool drove; } mm; // drove: it set the Character's limits; brake_from: the speed a stop began at
    // a jump the clip carries (anim_mm.inc, mm_vertical): the capsule's height and the clip's when it began, it has left the ground, for how long
    struct { bool on, air; int32_t group; float cap0, clip0, time, top; } mmv;
    // the feet's sliding (the stats): the lower foot and the actor where they were at the last pose
    float slide_foot[3], slide_actor[3], slide_sum, travel_sum, slide_low, slide_made, travel_made;
    float pose_dt;
    bool slide_ok;
    uint8_t slide_lo;
    // an editor's preview (anim_preview_pose): no actor, its variables as the editor sets them
    const AnimPreviewVar* pv;
    uint32_t npv;
    // the morph target curves of its last pose (by the mesh's morph targets; bit set: driven by the animation)
    uint64_t curve_set;
    float curve_w[ANIM_MAX_MORPHS];
    // (Q10) what hangs on its bones and sockets (g_follow's), its kept pose (g_kept's slot; -1: none): set each frame
    uint16_t nfollow, follow[16];
    int16_t kept;
    // (Q27.11) the locomotion style asked (ANIM_STYLE_*), each style's weight easing toward it (no pop), the bones the
    // procedural layer turns (found once per mesh: pelvis, 2 spine, head, upper and lower arms, calves), the mesh's
    // forward (from its feet to its toes)
    uint8_t style;
    float style_w[ANIM_STYLE_COUNT], style_v[ANIM_STYLE_COUNT], style_t;
    const SkelMesh* style_mesh;
    int16_t sb[11];
    float sfwd[3];
} AnimInst;

static AnimInst* insts(void) { return (AnimInst*)A.insts.base; }
bool anim_morph_from_curves(const CSkeletalMesh* c, uint32_t i, float* w) {
    if (!c || !c->inst || c->inst > A.ninst || i >= ANIM_MAX_MORPHS) return false;
    const AnimInst* in = &insts()[c->inst - 1];
    if (!(in->curve_set >> i & 1u)) return false;
    *w = in->curve_w[i];
    return true;
}
static void nodes_logic(World* w, AnimInst* in, float dt); // (anim_nodes.inc)
static void nodes_bind(AnimInst* in, const SkelMesh* m);
static int anim_nodes_selftest(void);

static bool clip_set(Clip* c, AnimSeq* s, const SkelMesh* m) {
    c->seq = s;
    if (s && m) track_map(s, m, c->map);
    return s != NULL;
}

// The CSkeletalMesh entities' parts: made when missing or when the mesh changed
static void destroy_parts(World* w, Entity sk) {
    Entity dead[ANIM_MAX_PARTS + ANIM_MAX_SKINS + 4];
    uint32_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count && n < ANIM_MAX_PARTS + ANIM_MAX_SKINS + 4; r++)
            if (at[r].owner == sk && at[r].kind != ATTACH_SCS) dead[n++] = ents[r];
    }
    for (uint32_t i = 0; i < n; i++) ecs_destroy(w, dead[i]);
}

static void build_parts(World* w, Entity sk, CSkeletalMesh* c) {
    destroy_parts(w, sk);
    c->built = c->mesh;
    const SkelMesh* m = anim_mesh(c->mesh);
    const CTransform* ot = ecs_get(w, sk, COMP_TRANSFORM);
    uint32_t flags = ot ? ot->flags : MOBILITY_MOVABLE;
    // the contact AO's occluders: its 4 biggest parts (a character's dozens of limbs would take every slot, and each
    // costs every pixel); the others still cast ray traced shadows
    float vol[ANIM_MAX_PARTS], fourth = 0.0f;
    for (uint32_t i = 0; m && i < m->nparts; i++) {
        vol[i] = m->parts[i].local.s[0] * m->parts[i].local.s[1] * m->parts[i].local.s[2];
        uint32_t bigger = 0;
        for (uint32_t j = 0; j < m->nparts; j++)
            bigger += m->parts[j].local.s[0] * m->parts[j].local.s[1] * m->parts[j].local.s[2] > vol[i];
        if (bigger == 3) fourth = vol[i];
    }
    if (m)
        for (uint32_t i = 0; i < m->nparts; i++) {
            Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
            if (e == ENTITY_NONE) break;
            const SkPart* p = &m->parts[i];
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", m->bones[p->bone].name);
            CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
            sm->mesh = p->mesh;
            memcpy(sm->base_color, p->color, 12);
            sm->roughness = p->roughness, sm->metallic = p->metallic, sm->material = p->material;
            sm->cast_shadow = 1u | CAST_SHADOW_ROUND_OCCLUDER | (vol[i] < fourth ? CAST_SHADOW_NO_OCCLUDER : 0u);
            CAttach* at = ecs_get(w, e, COMP_ATTACH);
            at->owner = sk, at->parent = sk, at->node = i, at->kind = ATTACH_SK_PART;
            at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
            ((CTransform*)ecs_get(w, e, COMP_TRANSFORM))->flags = flags;
        }
    // its skins: at the actor (the palette deforms them, render.c), dynamic like the parts
    for (uint32_t i = 0; m && i < m->nskins; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
        if (e == ENTITY_NONE) break;
        const SkSkin* k = &m->skins[i];
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Skin%u", i);
        CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
        sm->mesh = k->mesh;
        memcpy(sm->base_color, k->color, 12);
        sm->roughness = k->roughness, sm->metallic = k->metallic, sm->material = k->material;
        sm->cast_shadow = 1u | CAST_SHADOW_ROUND_OCCLUDER | (i ? CAST_SHADOW_NO_OCCLUDER : 0u); // (the contact AO: its first skin, round)
        CAttach* at = ecs_get(w, e, COMP_ATTACH);
        at->owner = sk, at->parent = sk, at->node = i, at->kind = ATTACH_SK_SKIN;
        at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        // (deformed every pose: the dynamic set; 2026-11-20: an actor without collision, its skins too - the VR hands' skin was a
        // static collider the thrown cube hit)
        t->flags = MOBILITY_MOVABLE | (flags & TRANSFORM_NO_COLLISION);
    }
    if (m && m->nskins) skin_pal(sk, true);
    c = ecs_get(w, sk, COMP_SKELETAL_MESH); // (the creations may have moved its row)
    if (c && c->anim_class) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_ATTACH) | COMP_BIT(COMP_BLUEPRINT));
        if (e != ENTITY_NONE) {
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "AnimInstance");
            CAttach* at = ecs_get(w, e, COMP_ATTACH);
            at->owner = sk, at->parent = sk, at->kind = ATTACH_ANIM_BP;
            at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
            ((CBlueprint*)ecs_get(w, e, COMP_BLUEPRINT))->asset_hash = c->anim_class;
            bp_start_instance(w, e); // (made while playing: a spawned Character's)
        }
    }
}

// --- (Q10) things hung on bones and sockets, the poses kept for anim_socket_world ---
#define MAX_FOLLOW 1024
#define MAX_KEPT 64
static struct Follow { Entity child, mesh, actor; int16_t bone, sock; uint32_t inst; bool own, moved; float loc[3], rot[3]; } g_follow[MAX_FOLLOW]; // (own: an actor, its offset here)
static uint32_t g_nfollow;
static struct Kept { Entity mesh; uint32_t inst; bool posed; float rows[ANIM_MAX_BONES][3][4]; } g_kept[MAX_KEPT];
static uint32_t g_nkept;

void anim_rows_transform(const float rows[3][4], CTransform* nt) {
    float ax[3][3];
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(rows[0][a] * rows[0][a] + rows[1][a] * rows[1][a] + rows[2][a] * rows[2][a]);
        nt->scale[a] = l;
        for (int k = 0; k < 3; k++) ax[a][k] = l > 1e-8f ? rows[k][a] / l : (a == k ? 1.0f : 0.0f);
        nt->location[a] = rows[a][3];
    }
    axes_rotator(ax, nt->rotation);
}
// a name as a socket (its bone and offset) or a bone; false: neither
static bool socket_of(const SkelMesh* m, const char* name, int16_t* bone, int16_t* sock) {
    if (!m || !name || !name[0]) return false;
    int s = anim_socket_index(m, name);
    if (s >= 0 && m->sockets[s].bone >= 0 && m->sockets[s].bone < (int)m->nbones) return *bone = m->sockets[s].bone, *sock = (int16_t)s, true;
    for (uint32_t i = 0; i < m->nbones; i++)
        if (!_stricmp(m->bones[i].name, name)) return *bone = (int16_t)i, *sock = -1, true;
    return false;
}
// a socket's or a bone's rows from the pose's
static void socket_rows(const SkelMesh* m, int16_t bone, int16_t sock, const float (*bones)[3][4], float out[3][4]) {
    if (sock >= 0 && sock < (int)m->nsockets) anim_socket_rows(m, &m->sockets[sock], bones, out);
    else memcpy(out, bones[bone], sizeof(float) * 12);
}
static AnimInst* inst_of_mesh(World* w, Entity mesh, uint32_t* cache) {
    if (*cache < A.ninst && insts()[*cache].e == mesh) return &insts()[*cache];
    for (uint32_t i = 0; i < A.ninst; i++)
        if (insts()[i].e == mesh) return *cache = i, (ecs_alive(w, mesh) ? &insts()[i] : NULL);
    return NULL;
}
static AnimInst* inst_of_actor(World* w, Entity actor);

bool anim_attach_to_socket(World* w, Entity child, Entity actor, const char* name) {
    AnimInst* in = inst_of_actor(w, actor);
    CAttach* at = ecs_get(w, child, COMP_ATTACH);
    int16_t bone, sock;
    if (!in || !in->mesh || !at || !socket_of(in->mesh, name, &bone, &sock)) return false;
    uint32_t k = 0;
    while (k < g_nfollow && g_follow[k].child != child) k++;
    if (k == g_nfollow) {
        if (g_nfollow >= MAX_FOLLOW) return false;
        g_nfollow++;
    }
    g_follow[k] = (struct Follow){ child, in->e, actor, bone, sock, (uint32_t)(in - insts()), false, false, { 0, 0, 0 }, { 0, 0, 0 } };
    at->kind = ATTACH_BONE, at->node = 0;
    if (!at->owner) at->owner = actor;
    if (!at->parent || !ecs_alive(w, at->parent)) at->parent = in->e; // (gone with the mesh; the part's when the caller set one)
    if (at->scale[0] == 0.0f && at->scale[1] == 0.0f && at->scale[2] == 0.0f) at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    return true;
}
#include "anim_meshsock.inc" // (2026-11-23) a Static Mesh's sockets

// (2026-11-23) actors hung on a Static Mesh's socket (anim_meshsock.inc): placed each frame after the poses
#define MAX_SFOLLOW 256
static struct SFollow { Entity child, parent; char name[ANIM_NAME]; float loc[3], rot[3]; } g_sfollow[MAX_SFOLLOW];
static uint32_t g_nsfollow;
static void sfollow_update(World* w) {
    for (uint32_t k = 0; k < g_nsfollow;) {
        struct SFollow* f = &g_sfollow[k];
        CTransform* t = ecs_get(w, f->child, COMP_TRANSFORM);
        float sr[3][4], lr[3][4], out[3][4];
        if (!t || !ecs_alive(w, f->parent) || !static_socket_world(w, f->parent, f->name, sr)) { // (gone: let go)
            g_sfollow[k] = g_sfollow[--g_nsfollow];
            continue;
        }
        CTransform loc = { { f->loc[0], f->loc[1], f->loc[2] }, { f->rot[0], f->rot[1], f->rot[2] }, { 1, 1, 1 }, 0 };
        render_transform_rows(&loc, lr);
        rows_mul(sr, lr, out);
        CTransform nt;
        anim_rows_transform(out, &nt);
        nt.flags = t->flags;
        memcpy(nt.scale, t->scale, 12); // (an actor keeps its own size)
        if (memcmp(&nt, t, sizeof nt)) *t = nt, ecs_touch(w, f->child);
        k++;
    }
}

bool anim_attach_actor_to_socket(World* w, Entity child, Entity parent, const char* name, const float loc[3], const float rot[3]) {
    AnimInst* in = inst_of_actor(w, parent);
    int16_t bone, sock;
    if ((!in || !in->mesh) && child != parent && ecs_get(w, child, COMP_TRANSFORM) && name) { // (2026-11-23) a Static Mesh's socket
        float probe[3][4];
        if (!static_socket_world(w, parent, name, probe)) return false;
        uint32_t k = 0;
        while (k < g_nsfollow && g_sfollow[k].child != child) k++;
        if (k == g_nsfollow) {
            if (g_nsfollow >= MAX_SFOLLOW) return false;
            g_nsfollow++;
        }
        struct SFollow* f = &g_sfollow[k];
        memset(f, 0, sizeof *f);
        f->child = child, f->parent = parent;
        snprintf(f->name, sizeof f->name, "%s", name);
        if (loc) memcpy(f->loc, loc, 12);
        if (rot) memcpy(f->rot, rot, 12);
        sfollow_update(w);
        return true;
    }
    if (!in || !in->mesh || child == parent || !ecs_get(w, child, COMP_TRANSFORM) || !socket_of(in->mesh, name, &bone, &sock)) return false;
    const CAttach* at = ecs_get(w, child, COMP_ATTACH);
    if (at && at->kind != ATTACH_BONE) return false; // (a component: its actor is the one to attach)
    uint32_t k = 0;
    while (k < g_nfollow && g_follow[k].child != child) k++;
    if (k == g_nfollow) {
        if (g_nfollow >= MAX_FOLLOW) return false;
        g_nfollow++;
    }
    g_follow[k] = (struct Follow){ child, in->e, parent, bone, sock, (uint32_t)(in - insts()), true, false, { 0, 0, 0 }, { 0, 0, 0 } };
    if (loc) memcpy(g_follow[k].loc, loc, 12);
    if (rot) memcpy(g_follow[k].rot, rot, 12);
    return true;
}
void anim_detach_socket(World* w, Entity child) {
    for (uint32_t k = 0; k < g_nsfollow; k++) // (2026-11-23: a Static Mesh's)
        if (g_sfollow[k].child == child) g_sfollow[k] = g_sfollow[--g_nsfollow], k--;
    for (uint32_t k = 0; k < g_nfollow; k++)
        if (g_follow[k].child == child) {
            g_follow[k] = g_follow[--g_nfollow];
            CAttach* at = ecs_get(w, child, COMP_ATTACH);
            if (at && at->kind == ATTACH_BONE) at->kind = ATTACH_SCS;
            return;
        }
}
// (2026-11-23) the warping nodes' states of an actor's instance (the test's: anim_warping)
bool anim_warp_state(World* w, Entity actor, float* move_angle, float* orient, float* anim_speed, float* stride) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in) return false;
    if (move_angle) *move_angle = in->mv_angle;
    if (orient) *orient = in->ow_angle;
    if (anim_speed) *anim_speed = in->sw_anim_speed;
    if (stride) *stride = in->sw_scale;
    return true;
}

bool anim_socket_world(World* w, Entity actor, const char* name, float out[3][4]) {
    AnimInst* in = inst_of_actor(w, actor);
    int16_t bone, sock;
    if (!in || !in->mesh) return static_socket_world(w, actor, name, out); // (2026-11-23: a Static Mesh's)
    if (!in || !in->mesh || !socket_of(in->mesh, name, &bone, &sock)) return false;
    uint32_t k = 0;
    while (k < g_nkept && g_kept[k].mesh != in->e) k++;
    if (k < g_nkept && g_kept[k].posed) {
        socket_rows(in->mesh, bone, sock, (const float(*)[3][4])g_kept[k].rows, out);
        return true;
    }
    if (k == g_nkept && g_nkept < MAX_KEPT) g_kept[g_nkept].mesh = in->e, g_kept[g_nkept].inst = (uint32_t)(in - insts()), g_kept[g_nkept++].posed = false; // (kept from the next pose on)
    const CTransform* t = ecs_get(w, in->e, COMP_TRANSFORM); // (until then: the bind pose under the actor)
    if (!t) return false;
    float rows[3][4];
    static float bw[ANIM_MAX_BONES][3][4]; // (the main thread's)
    render_transform_rows(t, rows);
    const CCharacter* chr = ecs_get(w, in->e, COMP_CHARACTER);
    if (chr) rows[2][3] -= chr->half_height;
    for (uint32_t b = 0; b < in->mesh->nbones; b++) rows_mul(rows, in->mesh->bind_world[b], bw[b]);
    socket_rows(in->mesh, bone, sock, (const float(*)[3][4])bw, out);
    return true;
}
// (the main thread, before the poses) each instance's hung things and kept pose; the gone ones dropped
static void follow_assign(World* w) {
    for (uint32_t i = 0; i < A.ninst; i++) insts()[i].nfollow = 0, insts()[i].kept = -1;
    for (uint32_t k = 0; k < g_nfollow;) {
        struct Follow* f = &g_follow[k];
        const CAttach* at = ecs_alive(w, f->child) ? ecs_get(w, f->child, COMP_ATTACH) : NULL;
        bool held = f->own ? ecs_alive(w, f->child) : at && at->kind == ATTACH_BONE;
        if (!held || !ecs_alive(w, f->mesh)) { // (the thing or the mesh gone, or detached another way)
            *f = g_follow[--g_nfollow];
            continue;
        }
        AnimInst* in = inst_of_mesh(w, f->mesh, &f->inst);
        if (in && in->nfollow < 16) in->follow[in->nfollow++] = (uint16_t)k;
        k++;
    }
    for (uint32_t k = 0; k < g_nkept;) {
        if (!ecs_alive(w, g_kept[k].mesh)) {
            g_kept[k] = g_kept[--g_nkept];
            continue;
        }
        AnimInst* in = inst_of_mesh(w, g_kept[k].mesh, &g_kept[k].inst);
        if (in) in->kept = (int16_t)k;
        k++;
    }
}
// (a pose job) its hung things at their sockets, its pose kept
static void place_followers(World* w, const AnimInst* in, const SkelMesh* m, const float (*bones)[3][4]) {
    for (uint32_t k = 0; k < in->nfollow; k++) {
        const struct Follow* f = &g_follow[in->follow[k]];
        CTransform* t = ecs_get(w, f->child, COMP_TRANSFORM);
        const CAttach* at = f->own ? NULL : ecs_get(w, f->child, COMP_ATTACH);
        if (!t || (!at && !f->own) || f->bone >= (int)m->nbones) continue;
        float sr[3][4], lr[3][4], out[3][4];
        socket_rows(m, f->bone, f->sock, bones, sr);
        CTransform loc = { { f->loc[0], f->loc[1], f->loc[2] }, { f->rot[0], f->rot[1], f->rot[2] }, { t->scale[0], t->scale[1], t->scale[2] }, 0 };
        if (at) loc = (CTransform){ { at->location[0], at->location[1], at->location[2] }, { at->rotation[0], at->rotation[1], at->rotation[2] },
                                    { at->scale[0], at->scale[1], at->scale[2] }, 0 };
        render_transform_rows(&loc, lr);
        rows_mul(sr, lr, out);
        CTransform nt;
        anim_rows_transform(out, &nt);
        nt.flags = t->flags;
        if (f->own) memcpy(nt.scale, t->scale, 12); // (an actor keeps its own size)
        if (memcmp(&nt, t, sizeof nt)) {
            *t = nt;
            if (f->own) g_follow[in->follow[k]].moved = true; // (an actor: touched on the main thread after the poses)
        }
    }
    if (in->kept >= 0 && in->kept < (int)g_nkept) {
        memcpy(g_kept[in->kept].rows, bones, sizeof(float) * 12 * m->nbones);
        g_kept[in->kept].posed = true;
    }
}

// The parts at a pose (rows of every bone): true when one of them moved
static bool place_parts(World* w, const AnimInst* in, const SkelMesh* m, float bones[][3][4]) {
    bool moved = false;
    for (uint32_t i = 0; i < in->nparts && i < m->nparts; i++) {
        CTransform* t = ecs_get(w, in->parts[i], COMP_TRANSFORM);
        if (!t) continue;
        const SkPart* p = &m->parts[i];
        float rows[3][4];
        xf_rows(&p->local, bones[p->bone], rows);
        CTransform nt;
        float ax[3][3];
        for (int a = 0; a < 3; a++) {
            float l = sqrtf(rows[0][a] * rows[0][a] + rows[1][a] * rows[1][a] + rows[2][a] * rows[2][a]);
            nt.scale[a] = l;
            for (int k = 0; k < 3; k++) ax[a][k] = l > 1e-8f ? rows[k][a] / l : (a == k ? 1.0f : 0.0f);
            nt.location[a] = rows[a][3];
        }
        axes_rotator(ax, nt.rotation);
        nt.flags = t->flags;
        if (memcmp(&nt, t, sizeof nt)) *t = nt, moved = true;
    }
    return moved;
}

static void actor_rows(const CTransform* t, float rows[3][4]) { render_transform_rows(t, rows); }

// The skins at the actor (its rows) and the palette: each bone's pose in the mesh's space times its bind's inverse.
// True when the pose or the place changed (the record's version moves: render.c deforms them again)
static bool place_skins(World* w, const AnimInst* in, const SkelMesh* m, const float rows[3][4], float bones[][3][4]) {
    SkinPal* p = skin_pal(in->e, false); // (made on the main thread: build_parts, the editor's loop)
    bool moved = false;
    if (p) {
        float inv[3][4], mb[3][4], pal[3][4];
        rows_inverse(rows, inv);
        bool same = p->nbones == m->nbones && p->version;
        for (uint32_t b = 0; b < m->nbones; b++) {
            rows_mul(inv, bones[b], mb);
            rows_mul(mb, m->bind_inv[b], pal);
            if (same && memcmp(pal, p->m[b], sizeof pal)) same = false;
            memcpy(p->m[b], pal, sizeof pal);
        }
        if (!same) p->nbones = m->nbones, p->version = p->version + 1u ? p->version + 1u : 1u, moved = true;
    }
    for (uint32_t i = 0; i < in->nskins; i++) {
        CTransform* t = ecs_get(w, in->skins[i], COMP_TRANSFORM);
        if (!t) continue;
        CTransform nt;
        float ax[3][3];
        for (int a = 0; a < 3; a++) {
            float l = sqrtf(rows[0][a] * rows[0][a] + rows[1][a] * rows[1][a] + rows[2][a] * rows[2][a]);
            nt.scale[a] = l;
            for (int k = 0; k < 3; k++) ax[a][k] = l > 1e-8f ? rows[k][a] / l : (a == k ? 1.0f : 0.0f);
            nt.location[a] = rows[a][3];
        }
        axes_rotator(ax, nt.rotation);
        nt.flags = t->flags;
        if (memcmp(&nt, t, sizeof nt)) *t = nt, moved = true;
    }
    return moved;
}

// The parts and Anim Blueprint instances of every skeletal mesh, sorted by owner (made again when the world changed):
// a mesh finds its own by a binary search instead of walking every attached entity
typedef struct PartRef { Entity owner, e; uint32_t kind, node; } PartRef;

static int by_owner(const void* x, const void* y) {
    const PartRef* a = x;
    const PartRef* b = y;
    return a->owner < b->owner ? -1 : a->owner > b->owner ? 1 : 0;
}

static void index_parts(World* w) {
    if (A.index_valid && A.index_changes == w->changes && A.index_revision == w->revision) return;
    A.nindex = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            if (at[r].kind == ATTACH_SCS) continue;
            if (!re_vm_ensure(&A.index, (size_t)(A.nindex + 1) * sizeof(PartRef))) return;
            ((PartRef*)A.index.base)[A.nindex++] = (PartRef){ at[r].owner, ents[r], at[r].kind, at[r].node };
        }
    }
    qsort(A.index.base, A.nindex, sizeof(PartRef), by_owner);
    A.index_changes = w->changes, A.index_revision = w->revision, A.index_valid = true;
}

static void find_parts(World* w, AnimInst* in) {
    index_parts(w);
    in->nparts = in->nskins = 0;
    in->abp = ENTITY_NONE;
    const PartRef* ix = (const PartRef*)A.index.base;
    uint32_t lo = 0, hi = A.nindex;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ix[mid].owner < in->e) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < A.nindex && ix[i].owner == in->e; i++) {
        if (ix[i].kind == ATTACH_SK_PART && ix[i].node < ANIM_MAX_PARTS) {
            in->parts[ix[i].node] = ix[i].e;
            if (ix[i].node + 1 > in->nparts) in->nparts = ix[i].node + 1;
        } else if (ix[i].kind == ATTACH_SK_SKIN && ix[i].node < ANIM_MAX_SKINS) {
            in->skins[ix[i].node] = ix[i].e;
            if (ix[i].node + 1 > in->nskins) in->nskins = ix[i].node + 1;
        } else if (ix[i].kind == ATTACH_ANIM_BP)
            in->abp = ix[i].e;
    }
}

#define MASK_HAS(m, b) (((m)[(b) >> 6] >> ((b) & 63)) & 1ull)
static bool bone_mask(const SkelMesh* m, const char* from, uint64_t* mask) {
    memset(mask, 0, sizeof(uint64_t) * ((ANIM_MAX_BONES + 63) / 64));
    bool any = false;
    for (uint32_t b = 0; b < m->nbones; b++) {
        int p = m->bones[b].parent;
        if (!strcmp(m->bones[b].name, from) || (p >= 0 && MASK_HAS(mask, (uint32_t)p))) mask[b >> 6] |= 1ull << (b & 63), any = true;
    }
    return any;
}

// (Q12) Unreal's Hide Bone by Name: a bone and the ones below it collapsed to its root in the pose (its skin's vertices
// and its parts shrink to that point): the head of a first-person body (fpcam.c). A few actors at most: a small table
// the pose jobs read (written on the main thread, between the frames' jobs)
#define HIDE_MAX 16
static struct { Entity actor; char bone[ANIM_NAME]; } g_hide[HIDE_MAX];
static uint32_t g_nhide;
bool anim_hide_bone(World* w, Entity actor, const char* bone, bool hide) {
    (void)w;
    for (uint32_t i = 0; i < g_nhide; i++)
        if (g_hide[i].actor == actor) {
            if (hide && bone && bone[0]) snprintf(g_hide[i].bone, ANIM_NAME, "%s", bone);
            else g_hide[i] = g_hide[--g_nhide];
            return true;
        }
    if (!hide || !bone || !bone[0] || g_nhide >= HIDE_MAX) return !hide;
    g_hide[g_nhide].actor = actor;
    snprintf(g_hide[g_nhide].bone, ANIM_NAME, "%s", bone);
    g_nhide++;
    return true;
}
static void hide_bones(const AnimInst* in, const SkelMesh* m, float bones[][3][4]) {
    const char* name = NULL;
    for (uint32_t i = 0; i < g_nhide && !name; i++)
        if (g_hide[i].actor == in->actor || g_hide[i].actor == in->e) name = g_hide[i].bone;
    if (!name) return;
    int hb = -1;
    for (uint32_t b = 0; b < m->nbones && hb < 0; b++)
        if (!_stricmp(m->bones[b].name, name)) hb = (int)b;
    if (hb < 0) return;
    uint8_t hidden[ANIM_MAX_BONES];
    memset(hidden, 0, sizeof hidden);
    float at[3] = { bones[hb][0][3], bones[hb][1][3], bones[hb][2][3] };
    for (uint32_t b = (uint32_t)hb; b < m->nbones; b++) { // (parents come first: a child after its parent)
        int p = m->bones[b].parent;
        hidden[b] = (int)b == hb || (p >= 0 && hidden[p]);
        if (!hidden[b]) continue;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) bones[b][r][c] *= 1e-3f;
            bones[b][r][3] = at[r];
        }
    }
}

static AnimInst* inst_of_actor(World* w, Entity actor) {
    for (uint32_t i = 0; i < A.ninst; i++)
        if (insts()[i].e == actor || insts()[i].actor == actor) return ecs_alive(w, insts()[i].e) ? &insts()[i] : NULL;
    return NULL;
}

Entity anim_mesh_entity(World* w, Entity actor) {
    AnimInst* in = inst_of_actor(w, actor);
    return in ? in->e : ENTITY_NONE;
}

Entity anim_bone_entity(World* w, Entity actor, const char* bone) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->mesh || !bone || !bone[0]) return ENTITY_NONE;
    int sk = anim_socket_index(in->mesh, bone); // (Q7) a socket: its bone's
    if (sk >= 0 && in->mesh->sockets[sk].bone >= 0 && in->mesh->sockets[sk].bone < (int)in->mesh->nbones)
        bone = in->mesh->bones[in->mesh->sockets[sk].bone].name;
    int b = -1;
    for (uint32_t i = 0; i < in->mesh->nbones && b < 0; i++)
        if (!_stricmp(in->mesh->bones[i].name, bone)) b = (int)i;
    for (int up = b, d = 0; up >= 0 && d < ANIM_MAX_BONES; up = in->mesh->bones[up].parent, d++) // (its part, else the nearest above)
        for (uint32_t k = 0; k < in->nparts && k < in->mesh->nparts; k++)
            if (in->mesh->parts[k].bone == up && in->parts[k] && ecs_alive(w, in->parts[k])) return in->parts[k];
    return ENTITY_NONE;
}

static uint32_t make_inst(World* w, Entity e, CSkeletalMesh* c) {
    if (A.ninst >= MAX_INST || !re_vm_ensure(&A.insts, (size_t)(A.ninst + 1) * sizeof(AnimInst))) return 0;
    AnimInst* in = &insts()[A.ninst];
    memset(in, 0, sizeof *in);
    in->kept = -1;
    in->e = e;
    in->actor = bp_component_owner(w, e);
    in->mesh = anim_mesh(c->mesh);
    BpAsset* cls = c->anim_class ? bp_find_hash(c->anim_class) : NULL;
    in->graph = anim_graph_of(cls, false);
    in->rate = c->play_rate > 0.0f ? c->play_rate : 1.0f;
    in->loop = c->loop != 0;
    if (in->mesh) clip_set(&in->single, c->anim ? anim_seq(c->anim) : NULL, in->mesh);
    for (uint32_t k = 0; in->graph && k < ANIM_MAX_MACHINES; k++) in->sm[k].state = in->sm[k].prev = in->graph->machine_entry[k];
    in->inert_node = -1;
    if (in->mesh && in->graph) in->has_upper = bone_mask(in->mesh, in->graph->upper_bone, in->upper_mask);
    find_parts(w, in);
    if (in->nskins) skin_pal(e, true); // (the pose jobs only write it)
    in->acc = 0.0f;
    in->update = true;
    return ++A.ninst;
}

void anim_begin_play(World* w) {
    g_nhide = 0; // (Q12: no bone hidden from a play before)
    if (!A.ready) {
        if (!re_vm_reserve(&A.insts, (size_t)MAX_INST * sizeof(AnimInst))) return;
        A.ready = true;
    }
    A.ninst = 0;
    A.playing = true;
    A.frame = 0;
    ikphys_reset();
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH))) continue;
        CSkeletalMesh* c = ecs_arch_column(ar, COMP_SKELETAL_MESH);
        for (uint32_t r = 0; r < ar->count; r++) c[r].inst = 0;
    }
}

static void warp_end_play(void); // (anim_warp.inc)
void anim_end_play(World* w) {
    (void)w;
    A.playing = false;
    A.ninst = 0;
    ikphys_reset();
    warp_end_play(); // (2026-11-23) the warp targets, the pairs, the waiting root motion
}

// A variable of the instance's Anim Blueprint as a float (bools: 0 / 1)
static float var_of(World* w, const AnimInst* in, const char* name) {
    BpValue v;
    if (!in->abp && in->pv) { // (a preview: the editor's values)
        for (uint32_t i = 0; name[0] && i < in->npv; i++)
            if (!strcmp(in->pv[i].name, name)) return in->pv[i].value;
        return 0.0f;
    }
    if (!in->abp || !name[0] || !bp_instance_var(w, in->abp, name, &v)) return 0.0f;
    return v.v[0];
}

static int16_t bone_named(const SkelMesh* m, const char* name);
static void fire_notifies(World* w, AnimInst* in, const AnimSeq* s, float t0, float t1);
static void fire_notify(World* w, AnimInst* in, const AnimNotify* n);
#include "anim_mm.inc"
#include "anim_warp.inc" // (2026-11-23) root motion of montages, Motion Warping, paired montages
#include "anim_traversal.inc" // (2026-11-23) Try Traversal: Vault, Mantle, Climb
#include "anim_combo.inc" // (2026-11-23) montage windows: State, Combo, Hit

// A clip's speed over the ground (its planted foot's, anim_mm.inc): the crowds' phases follow it (main thread)
float anim_seq_ground_speed(AnimSeq* s, const SkelMesh* m) {
    if (!s || !m) return 0.0f;
    int16_t feet[2];
    mm_feet_bones(NULL, m, feet);
    if (feet[0] < 0 || feet[1] < 0) return 0.0f;
    seq_warm(s, m);
    float dir[2];
    return mm_clip_speed(m, s, feet, dir);
}

static float state_length(const AnimInst* in, const AnimState* s) {
    bool many = s->kind == ANIM_STATE_BLEND1D || s->kind == ANIM_STATE_BLEND2D || s->kind == ANIM_STATE_MOTION_MATCH;
    AnimSeq* q = anim_seq_name(many && s->nsamples ? s->samples[0] : s->clip);
    (void)in;
    return q ? q->length : 1.0f;
}

// A state's pose at its time
static void state_pose(World* w, const AnimInst* in, const AnimState* s, float t, AnimXf* out) {
    const SkelMesh* m = in->mesh;
    AnimXf tmp[ANIM_MAX_BONES];
    int16_t map[ANIM_MAX_BONES];
#define MAP_OF(q) ((q)->map_for == m ? (q)->map : (track_map((q), m, map), map))
    if (s->kind == ANIM_STATE_MOTION_MATCH) {
        if (mm_pose(in, out)) return;
        AnimSeq* q = s->nsamples ? anim_seq_name(s->samples[0]) : NULL; // (no database: its first clip)
        if (q) sample_mapped(q, m, MAP_OF(q), t, out);
        else
            for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
        return;
    }
    if (s->kind == ANIM_STATE_BLEND2D && s->nsamples) { // Blend Space 2D: every sample by its weight at (X, Y)
        float wgt[ANIM_BLEND_SAMPLES], len = 0.0f;
        uint32_t n = s->nsamples < ANIM_BLEND_SAMPLES ? s->nsamples : ANIM_BLEND_SAMPLES;
        anim_blend2d_weights(s->sample_at, s->sample_y, n, var_of(w, in, s->param), var_of(w, in, s->param2), wgt);
        AnimSeq* q[ANIM_BLEND_SAMPLES];
        for (uint32_t k = 0; k < n; k++) {
            q[k] = wgt[k] > 0.005f ? anim_seq_name(s->samples[k]) : NULL;
            if (!q[k]) wgt[k] = 0.0f;
            else len += wgt[k] * q[k]->length;
        }
        // synchronized: every cycle at the same phase (the feet agree), its length the weighted one
        float phase = len > 1e-3f ? fmodf(t, len) / len : 0.0f, sum = 0.0f;
        static _Thread_local AnimXf bufs[ANIM_BLEND_SAMPLES][ANIM_MAX_BONES];
        const AnimXf* poses[ANIM_BLEND_SAMPLES];
        uint32_t used = 0;
        float uw[ANIM_BLEND_SAMPLES];
        for (uint32_t k = 0; k < n; k++) {
            if (!q[k]) continue;
            sample_mapped(q[k], m, MAP_OF(q[k]), phase * q[k]->length, bufs[used]);
            poses[used] = bufs[used], uw[used] = wgt[k], sum += wgt[k], used++;
        }
        if (!used) {
            for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
            return;
        }
        for (uint32_t k = 0; k < used; k++) uw[k] /= sum;
        anim_blend_n(poses, uw, used, m->nbones, out);
        return;
    }
    if (s->kind == ANIM_STATE_BLEND1D && s->nsamples) {
        float x = var_of(w, in, s->param);
        uint32_t k = 0;
        while (k + 1 < s->nsamples && x > s->sample_at[k + 1]) k++;
        AnimSeq* a = anim_seq_name(s->samples[k]);
        AnimSeq* b = k + 1 < s->nsamples ? anim_seq_name(s->samples[k + 1]) : NULL;
        float span = b ? s->sample_at[k + 1] - s->sample_at[k] : 1.0f;
        float f = b && span > 1e-3f ? fminf(fmaxf((x - s->sample_at[k]) / span, 0.0f), 1.0f) : 0.0f;
        // synchronized: both at the same phase of their cycles (feet together while walk turns to run)
        float la = a ? a->length : 1.0f, lb = b ? b->length : 1.0f, len = la + (lb - la) * f;
        float phase = len > 1e-3f ? fmodf(t, len) / len : 0.0f;
        if (a) sample_mapped(a, m, MAP_OF(a), phase * la, out);
        else
            for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
        if (b && f > 0.0f) {
            sample_mapped(b, m, MAP_OF(b), phase * lb, tmp);
            anim_blend(out, tmp, f, m->nbones, out);
        }
        return;
    }
    const char* name = s->clip;
    if (s->kind == ANIM_STATE_BLEND_BOOL && var_of(w, in, s->param) == 0.0f) name = s->clip2;
    AnimSeq* q = anim_seq_name(name);
    if (!q) {
        for (uint32_t i = 0; i < m->nbones; i++) out[i] = m->bones[i].bind;
        return;
    }
    sample_mapped(q, m, MAP_OF(q), s->loop ? t : fminf(t, q->length), out);
#undef MAP_OF
}

// The maps of the sequences a state reads (on this thread, before the parallel poses)
static void state_warm(const AnimInst* in, const AnimState* s) {
    if (s->kind == ANIM_STATE_BLEND1D || s->kind == ANIM_STATE_BLEND2D || s->kind == ANIM_STATE_MOTION_MATCH)
        for (uint32_t k = 0; k < s->nsamples; k++) seq_warm(anim_seq_name(s->samples[k]), in->mesh);
    else {
        seq_warm(anim_seq_name(s->clip), in->mesh);
        if (s->kind == ANIM_STATE_BLEND_BOOL) seq_warm(anim_seq_name(s->clip2), in->mesh);
    }
}

static void fire_notify(World* w, AnimInst* in, const AnimNotify* n) {
    const char* name = n->name;
    if (n->sound) { // Unreal's Play Sound notify: at the bone (or the mesh), following it when asked
        Entity at = n->bone[0] ? anim_bone_entity(w, in->actor ? in->actor : in->e, n->bone) : ENTITY_NONE;
        if (!at) at = in->e;
        const CTransform* t = ecs_get(w, at, COMP_TRANSFORM);
        AudioPlay p;
        memset(&p, 0, sizeof p);
        p.sound = n->sound, p.volume = n->volume > 0.0f ? n->volume : 1.0f, p.pitch = n->pitch > 0.0f ? n->pitch : 1.0f;
        p.spatial = true, p.bone = -1, p.owner = in->actor ? in->actor : in->e, p.follow = n->follow ? at : 0;
        if (t) memcpy(p.pos, t->location, 12);
        audio_play(&p);
    }
    char ev[ANIM_NAME + 16];
    snprintf(ev, sizeof ev, "AnimNotify_%s", name);
    if (!_strnicmp(name, "Foot", 4)) fpcam_footstep(w, in->actor ? in->actor : in->e); // (Q12) the First Person Camera's bob locks onto the steps
    if (!strcmp(name, "Footstep")) { // a footstep is a noise the AIs hear
        const CTransform* at = ecs_get(w, in->actor, COMP_TRANSFORM);
        if (at) ai_report_noise(w, at->location, 0.5f, in->actor);
    }
    if (in->abp) bp_call_event(w, in->abp, ev);
    bp_call_event(w, in->actor, ev);
}

static void fire_notifies(World* w, AnimInst* in, const AnimSeq* s, float t0, float t1) {
    if (!s || !s->nnotifies || t1 <= t0 || !w) return; // (a preview: no world to tell)
    float len = s->length > 1e-3f ? s->length : 1.0f;
    float a = s->loop ? fmodf(t0, len) : t0, b = a + (t1 - t0);
    for (uint32_t i = 0; i < s->nnotifies; i++) {
        float nt = s->notifies[i].time;
        if ((nt > a && nt <= b) || (s->loop && nt + len > a && nt + len <= b)) fire_notify(w, in, &s->notifies[i]);
    }
}

// The serial part of an instance's frame: the state machine, the montage's weight, the notifies (they call the VM)
static void step_logic(World* w, AnimInst* in, float dt) {
    AnimGraph* g = in->graph;
    if (g && !g->nnodes && g->nstates) anim_graph_ensure_nodes(g); // (a graph made in code: its nodes now)
    if (in->mm.drove) { // (Motion Matching sets the Character's limits again below while it plays)
        CCharacter* ch = ecs_get(w, in->actor ? in->actor : in->e, COMP_CHARACTER);
        if (ch) ch->anim_accel = ch->anim_decel = ch->anim_yaw_rate = ch->anim_vz = 0.0f, ch->anim_turning = 0, ch->anim_vertical = ch->anim_jump_ok = 0;
        in->mm.drove = false;
    }
    if (g && g->lean > 0.0f && dt > 1e-4f) { // the lean's inputs: how fast it turns, how its speed changes
        const CTransform* at = ecs_get(w, in->actor ? in->actor : in->e, COMP_TRANSFORM);
        const CCharacter* ch = ecs_get(w, in->actor ? in->actor : in->e, COMP_CHARACTER);
        if (at) {
            float d = remainderf(at->rotation[1] - in->lean_yaw, 360.0f), k = fminf(dt * 6.0f, 1.0f);
            in->lean_rate += (d / dt - in->lean_rate) * k;
            in->lean_yaw = at->rotation[1];
            float sp = ch ? sqrtf(ch->velocity[0] * ch->velocity[0] + ch->velocity[1] * ch->velocity[1]) : 0.0f;
            in->lean_accel += ((sp - in->lean_speed) / dt - in->lean_accel) * fminf(dt * 4.0f, 1.0f);
            in->lean_speed = sp;
        }
    }
    if (g && g->nnodes) { // (2026-11-23) the move the warping nodes read (its angle from the facing, its speed)
        const CTransform* at = ecs_get(w, in->actor ? in->actor : in->e, COMP_TRANSFORM);
        const CCharacter* ch = ecs_get(w, in->actor ? in->actor : in->e, COMP_CHARACTER);
        if (at && ch) {
            float vx = ch->velocity[0], vy = ch->velocity[1], yaw = at->rotation[1] * 0.0174533f;
            in->mv_speed = sqrtf(vx * vx + vy * vy);
            float fx = cosf(yaw), fy = sinf(yaw);
            if (in->mv_speed > 1.0f) in->mv_angle = atan2f(vx * -fy + vy * fx, vx * fx + vy * fy) / 0.0174533f; // (+: to the right)
        } else
            in->mv_speed = 0.0f;
    }
    if (g && g->nnodes) {
        if (in->inert_node >= 0 && !in->inert) in->inert = calloc(3 * ANIM_MAX_BONES, sizeof(AnimXf)); // (once)
        nodes_logic(w, in, dt); // the machines, the nodes' times and weights (anim_nodes.inc)
    } else if (in->single.seq) {
        fire_notifies(w, in, in->single.seq, in->time, in->time + dt * in->rate);
        in->time += dt * in->rate;
        in->state_name = in->single.seq->name;
    } else
        in->state_name = "-";
    if (in->montage && in->mclip.seq) {
        float len = in->mclip.seq->length;
        fire_notifies(w, in, in->mclip.seq, in->mtime, in->mtime + dt * in->mrate);
        float t0 = in->mtime;
        in->mtime += dt * in->mrate;
        // (2026-11-23) its root motion and warps: whole through its blend in and its own blend out (the clip's travel and a
        // warp's target reached exactly), faded with its weight only when stopped before its end (Stop Montage)
        rm_step(w, in, fminf(t0, len), fminf(in->mtime, len), in->mcut ? in->mweight : 1.0f);
        combo_step(w, in, fminf(t0, len), fminf(in->mtime, len)); // (2026-11-23) its windows: states, combos, hits
        float bin = fmaxf(in->montage->blend_in, 1e-3f), bout = fmaxf(in->mstopping ? in->mblend_out : in->montage->blend_out, 1e-3f);
        if (!in->mstopping && in->mtime >= len - bout) in->mstopping = true, in->mblend_out = bout, in->mtime = fminf(in->mtime, len);
        if (in->mstopping && !in->mcut) { // (2026-11-23) its own end: the weight by the time left, ended at the clip's end
            // (a long frame's step never left the last bit of root motion, or a warp window's close, unplayed)
            in->mweight = fminf(in->mweight, fmaxf((len - fminf(in->mtime, len)) / bout, 0.0f));
            if (in->mtime >= len - 1e-6f) in->montage = NULL, in->mweight = 0.0f;
        } else if (in->mstopping) {
            in->mweight -= dt / bout;
            if (in->mweight <= 0.0f) in->montage = NULL, in->mweight = 0.0f;
        } else
            in->mweight = fminf(in->mweight + dt / bin, 1.0f);
    }
}

// The parallel part: the pose and the parts' transforms
typedef struct PoseJob { World* w; uint32_t first, count, chunk; } PoseJob;
float bp_trace_boxes(const float a[3], const float b[3], float radius); // (bp_vm.c: the world's boxes while playing)

// (2026-11-23) the exact name, else the bone with that Unreal name's role on any rig (rig_detect.h: "foot_l" is
// Mixamo's LeftFoot, Rigify's DEF-foot.L...): the AnimGraph's Leg IK, Foot IK and Aim Offset chain work on every rig
static int16_t bone_named(const SkelMesh* m, const char* name) { return name && name[0] ? rig_bone_ue(m, name) : (int16_t)-1; }

// The graph's bones by name, once per revision of it (the editor bumps it)
static void ik_bind(AnimInst* in, const SkelMesh* m) {
    const AnimGraph* g = in->graph;
    if (in->ik_for == g && in->ik_rev == g->rev) return;
    in->ik_for = g, in->ik_rev = g->rev;
    nodes_bind(in, m);
    in->inert_ok = false;
    for (int k = 0; k < 4; k++) in->aim_b[k] = bone_named(m, g->aim_bones[k]);
    in->pelvis_b = -1;
    const char* feet[2] = { g->foot_l, g->foot_r }; // (the Leg IK node's feet when there is one: the sliding stats)
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == AN_LEG_IK) feet[0] = g->nodes[i].bone[0], feet[1] = g->nodes[i].bone[1];
    for (int f = 0; f < 2; f++) {
        int16_t foot = bone_named(m, feet[f]);
        int16_t calf = foot >= 0 ? m->bones[foot].parent : -1, thigh = calf >= 0 ? m->bones[calf].parent : -1;
        in->leg_b[f][0] = thigh, in->leg_b[f][1] = calf, in->leg_b[f][2] = foot;
        if (thigh >= 0 && m->bones[thigh].parent >= 0) in->pelvis_b = m->bones[thigh].parent;
    }
}

#include "anim_nodes.inc"

// (2026-11-20) poses given from outside (anim_pose_override): written on the main thread before anim_update, read by
// the pose jobs
#define POSE_OVERRIDES 4
static struct {
    Entity e;
    uint32_t n;
    AnimXf pose[ANIM_MAX_BONES];
} g_ovr[POSE_OVERRIDES];
bool anim_pose_override(Entity e, const AnimXf* pose, uint32_t nbones) {
    int free_i = -1;
    for (int i = 0; i < POSE_OVERRIDES; i++) {
        if (g_ovr[i].e == e && e) {
            if (!pose) g_ovr[i].e = 0;
            else {
                g_ovr[i].n = nbones < ANIM_MAX_BONES ? nbones : ANIM_MAX_BONES;
                memcpy(g_ovr[i].pose, pose, sizeof(AnimXf) * g_ovr[i].n);
            }
            return true;
        }
        if (!g_ovr[i].e && free_i < 0) free_i = i;
    }
    if (!pose) return true;
    if (free_i < 0 || !e) return false;
    g_ovr[free_i].e = e, g_ovr[free_i].n = nbones < ANIM_MAX_BONES ? nbones : ANIM_MAX_BONES;
    memcpy(g_ovr[free_i].pose, pose, sizeof(AnimXf) * g_ovr[free_i].n);
    return true;
}

// --- (Q27.11) Locomotion styles: GTA 6's archetypes of movement (injured, tired, drunk, carrying) as data on top of the
// Motion Matching. A style the database has clips for (their names have Injured / Tired / Drunk / Carry) is searched
// among those (anim_mm.inc mm_search); else this layer turns a few bones in the mesh's space (each bone turned in its
// parent's frame by a rotation about the character's axes), weighted by an eased weight: switching never pops
static double g_style_ms;
static uint32_t g_style_n;
static int16_t style_bone(const SkelMesh* m, const char* a, const char* side, int nth) {
    { // (2026-11-23) the role first (rig_detect.h): "calf" + "l" is calf_l's bone on any rig, the nth spine spine_0<n+1>'s
        char ue[ANIM_NAME];
        if (side) snprintf(ue, sizeof ue, "%s_%s", a, side);
        else if (!strcmp(a, "spine")) snprintf(ue, sizeof ue, "spine_%02d", nth + 1);
        else if (!strcmp(a, "neck")) snprintf(ue, sizeof ue, "neck_%02d", nth + 1);
        else snprintf(ue, sizeof ue, "%s", a);
        int16_t r = m->rig ? rig_bone_ue(m, ue) : (int16_t)-1;
        if (r >= 0) return r;
    }
    int seen = 0;
    for (uint32_t b = 0; b < m->nbones; b++) {
        char nm[ANIM_NAME];
        snprintf(nm, sizeof nm, "%s", m->bones[b].name);
        for (char* q = nm; *q; q++) *q = (char)tolower((unsigned char)*q);
        if (!strstr(nm, a)) continue;
        if (side) {
            size_t L = strlen(nm);
            bool l = strstr(nm, "left") || (L >= 2 && (nm[L - 2] == '_' || nm[L - 2] == '.') && nm[L - 1] == 'l');
            bool r = strstr(nm, "right") || (L >= 2 && (nm[L - 2] == '_' || nm[L - 2] == '.') && nm[L - 1] == 'r');
            if ((side[0] == 'l' && !l) || (side[0] == 'r' && !r)) continue;
        }
        if (seen++ == nth) return (int16_t)b;
    }
    return -1;
}
static void style_bind(AnimInst* in, const SkelMesh* m) {
    in->style_mesh = m;
    static const char* const nm[11] = { "pelvis", "spine", "spine", "head", "upperarm", "upperarm", "lowerarm", "lowerarm", "calf", "calf", "neck" };
    static const char* const sd[11] = { NULL, NULL, NULL, NULL, "l", "r", "l", "r", "l", "r", NULL };
    for (int i = 0; i < 11; i++) in->sb[i] = style_bone(m, nm[i], sd[i], i == 2 ? 1 : 0);
    // forward in the mesh's space: from a foot to its toes in the bind (else +Y, Unreal's mannequin)
    int16_t foot = style_bone(m, "foot", "l", 0), ball = style_bone(m, "ball", "l", 0);
    in->sfwd[0] = 0.0f, in->sfwd[1] = 1.0f, in->sfwd[2] = 0.0f;
    if (foot >= 0 && ball >= 0) {
        float dx = m->bind_world[ball][0][3] - m->bind_world[foot][0][3], dy = m->bind_world[ball][1][3] - m->bind_world[foot][1][3], l = sqrtf(dx * dx + dy * dy);
        if (l > 1e-3f) in->sfwd[0] = dx / l, in->sfwd[1] = dy / l;
    }
}
// a bone turned by angle (radians) about axis (the mesh's space): in its parent's frame
static void style_turn(const SkelMesh* m, AnimXf* pose, int16_t b, const float axis[3], float ang) {
    if (b < 0 || fabsf(ang) < 1e-5f) return;
    float pw[4] = { 0, 0, 0, 1 }; // (the parent's rotation in the mesh's space: its chain composed)
    int16_t chain[ANIM_MAX_BONES], n = 0;
    for (int16_t p = m->bones[b].parent; p >= 0 && n < ANIM_MAX_BONES; p = m->bones[p].parent) chain[n++] = p;
    for (int16_t k = n - 1; k >= 0; k--) q_mul(pw, pose[chain[k]].q, pw);
    float s = sinf(ang * 0.5f), dq[4] = { axis[0] * s, axis[1] * s, axis[2] * s, cosf(ang * 0.5f) }, ip[4] = { -pw[0], -pw[1], -pw[2], pw[3] }, t[4];
    q_mul(dq, pw, t), q_mul(ip, t, t); // (inv(P) dq P)
    q_mul(t, pose[b].q, pose[b].q);
}
#include "anim_cover.inc" // (2026-11-23) third-person cover: in, along the wall, its ends, crouch, peek

// (2026-11-23, the user: "hit reactions by direction") A hit's layer: an underdamped spring (the jolt, a little past, back
// in ~0.5 s) bending the spine and the head away from where the hit came from, in the mesh's space
static void flinch_apply(AnimInst* in, const SkelMesh* m, AnimXf* pose, const float rows[3][4]) {
    float dt = fminf(fmaxf(in->pose_dt, 0.0f), 0.05f);
    if (fabsf(in->flinch_x) < 1e-4f && fabsf(in->flinch_v) < 1e-3f) return;
    float k = 16.0f, z = 0.45f; // (stiffness, damping ratio)
    in->flinch_v += (-k * k * in->flinch_x - 2.0f * z * k * in->flinch_v) * dt;
    in->flinch_x += in->flinch_v * dt;
    if (in->style_mesh != m) style_bind(in, m);
    // the push and the world's up into the mesh's space (the rows' turn transposed: whatever way the mesh faces)
    float sc[3], d[3], up[3];
    for (int c = 0; c < 3; c++) sc[c] = sqrtf(rows[0][c] * rows[0][c] + rows[1][c] * rows[1][c] + rows[2][c] * rows[2][c]) + 1e-12f;
    for (int c = 0; c < 3; c++) d[c] = (rows[0][c] * in->flinch_dir[0] + rows[1][c] * in->flinch_dir[1]) / sc[c], up[c] = rows[2][c] / sc[c];
    float axis[3] = { up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0] };
    float l = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (l < 1e-4f) return;
    axis[0] /= l, axis[1] /= l, axis[2] /= l;
    float a = fminf(fmaxf(in->flinch_x, -0.6f), 0.6f); // (radians: about lat takes up toward d)
    style_turn(m, pose, in->sb[1], axis, a * 0.3f), style_turn(m, pose, in->sb[2], axis, a * 0.35f);
    style_turn(m, pose, in->sb[10], axis, a * 0.15f), style_turn(m, pose, in->sb[3], axis, a * 0.25f);
}
// The hit's direction against the actor's facing; its layer kicked (strength 0..1+); the montage of that direction played
// when the set has one (prefix_Front / _Back / _Left / _Right). The direction's name
const char* anim_hit_reaction(World* w, Entity actor, const float loc[3], float strength, const char* prefix) {
    AnimInst* in = inst_of_actor(w, actor);
    const CTransform* t = ecs_get(w, actor, COMP_TRANSFORM);
    if (!t) return "None";
    float yaw = t->rotation[1] * 0.0174533f, dx = loc[0] - t->location[0], dy = loc[1] - t->location[1];
    float fw = dx * cosf(yaw) + dy * sinf(yaw), rt = -dx * sinf(yaw) + dy * cosf(yaw), l = hypotf(fw, rt);
    const char* dir = fabsf(fw) >= fabsf(rt) ? (fw >= 0.0f ? "Front" : "Back") : (rt >= 0.0f ? "Right" : "Left");
    if (in && l > 1e-3f) {
        in->flinch_dir[0] = -dx / hypotf(dx, dy), in->flinch_dir[1] = -dy / hypotf(dx, dy); // (away from it, in the world)
        in->flinch_v += 9.0f * fminf(fmaxf(strength, 0.0f), 2.0f);
    }
    if (prefix && prefix[0]) {
        char name[ANIM_NAME * 2];
        snprintf(name, sizeof name, "%s_%s", prefix, dir);
        if (!anim_montage_name(name)) snprintf(name, sizeof name, "%s_Front", prefix);
        if (anim_montage_name(name)) anim_play_montage(w, actor, name, 1.0f);
    }
    return dir;
}

static void style_apply(World* w, AnimInst* in, const SkelMesh* m, AnimXf* pose) {
    float dt = fminf(fmaxf(in->pose_dt, 0.0f), 0.1f);
    in->style_t += dt;
    // the targets: the style asked, the injured one as much as the active ragdoll limps (Q27.5)
    float limp = phys_limp(in->e), want[ANIM_STYLE_COUNT] = { 0 };
    want[in->style < ANIM_STYLE_COUNT ? in->style : 0] = 1.0f;
    if (limp > want[ANIM_STYLE_INJURED]) want[ANIM_STYLE_INJURED] = fminf(limp * 1.5f, 1.0f);
    bool any = false;
    for (int s = 1; s < ANIM_STYLE_COUNT; s++) { // (critically damped, ~0.6 s: the weight and its rate continuous)
        float k = 7.0f, x = in->style_w[s] - want[s];
        in->style_v[s] += (-k * k * x - 2.0f * k * in->style_v[s]) * dt;
        in->style_w[s] = fminf(fmaxf(in->style_w[s] + in->style_v[s] * dt, 0.0f), 1.0f);
        any |= in->style_w[s] > 1e-3f;
    }
    if (!any) return;
    double t0 = re_time_seconds();
    if (in->style_mesh != m) style_bind(in, m);
    const float up[3] = { 0, 0, 1 }, *f = in->sfwd, lat[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    // (bow: about lat takes up toward forward; lean right: about forward)
    float wi = in->style_w[ANIM_STYLE_INJURED], wt = in->style_w[ANIM_STYLE_TIRED], wd = in->style_w[ANIM_STYLE_DRUNK], wc = in->style_w[ANIM_STYLE_CARRY];
    float t = in->style_t, R = 0.0174533f;
    // the injured: the pelvis dropped to the hurt (left) side, the torso leaning away over the good leg, the hurt knee bent
    style_turn(m, pose, in->sb[0], f, -5.0f * R * wi);
    style_turn(m, pose, in->sb[1], f, 4.0f * R * wi);
    style_turn(m, pose, in->sb[8], lat, -14.0f * R * wi);
    // the tired: bowed, head down
    style_turn(m, pose, in->sb[1], lat, 7.0f * R * wt), style_turn(m, pose, in->sb[2], lat, 6.0f * R * wt);
    style_turn(m, pose, in->sb[3], lat, 9.0f * R * wt);
    // the drunk: the torso and the head swaying side to side out of step, the pelvis turning
    style_turn(m, pose, in->sb[0], up, 5.0f * R * wd * sinf(t * 0.9f));
    style_turn(m, pose, in->sb[1], f, 7.0f * R * wd * sinf(t * 1.3f));
    style_turn(m, pose, in->sb[3], f, 9.0f * R * wd * sinf(t * 1.7f + 1.0f));
    // the carrying: the arms forward holding a box, the forearms bent up, leaning back against its weight
    style_turn(m, pose, in->sb[1], lat, -6.0f * R * wc);
    style_turn(m, pose, in->sb[4], lat, 50.0f * R * wc), style_turn(m, pose, in->sb[5], lat, 50.0f * R * wc);
    style_turn(m, pose, in->sb[6], lat, 55.0f * R * wc), style_turn(m, pose, in->sb[7], lat, 55.0f * R * wc);
    g_style_ms += (re_time_seconds() - t0) * 1000.0, g_style_n++;
    (void)w;
}
bool anim_set_locomotion_style(World* w, Entity actor, int style) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || style < 0 || style >= ANIM_STYLE_COUNT) return false;
    in->style = (uint8_t)style;
    return true;
}
int anim_locomotion_style(World* w, Entity actor) {
    AnimInst* in = inst_of_actor(w, actor);
    return in ? in->style : -1;
}
void anim_style_cost(double* ms, uint32_t* n) {
    *ms = g_style_ms, *n = g_style_n;
}

static void pose_one(World* w, AnimInst* in) {
    const SkelMesh* m = in->mesh;
    if (!m || !in->update) return;
    double t0 = re_time_seconds();
    AnimXf pose[ANIM_MAX_BONES], tmp[ANIM_MAX_BONES];
    t_npc = 0; // (the morph curves: this pose's)
    AnimGraph* g = in->graph;
    const CTransform* at = ecs_get(w, in->e, COMP_TRANSFORM);
    if (!at) return;
    float rows[3][4], bones[ANIM_MAX_BONES][3][4];
    actor_rows(at, rows);
    const CCharacter* chr = ecs_get(w, in->e, COMP_CHARACTER);
    if (chr) rows[2][3] -= chr->half_height; // on a Character itself: the feet at its capsule's bottom
    if (g && g->nnodes) { // the pose graph from its Output Pose (anim_nodes.inc)
        ik_bind(in, m);
        nodes_pose(w, in, pose, rows, bones, chr);
    } else {
        if (in->single.seq) sample_mapped(in->single.seq, m, in->single.map, in->time, pose);
        else
            for (uint32_t i = 0; i < m->nbones; i++) pose[i] = m->bones[i].bind;
        if (in->montage && in->mclip.seq && in->mweight > 0.0f) { // (no graph: a montage over the whole body)
            sample_mapped(in->mclip.seq, m, in->mclip.map, in->mtime, tmp);
            rm_inplace(in, m, tmp); // (2026-11-23) its root path off: the actor carries it
            float wgt = in->mweight * in->mweight * (3.0f - 2.0f * in->mweight);
            anim_blend(pose, tmp, wgt, m->nbones, pose);
        }
    }
    for (int i = 0; i < POSE_OVERRIDES; i++) // (the VR hands: their bones set from outside)
        if (g_ovr[i].e && g_ovr[i].e == in->e && g_ovr[i].n == m->nbones) memcpy(pose, g_ovr[i].pose, sizeof(AnimXf) * m->nbones);
    style_apply(w, in, m, pose); // (Q27.11) the locomotion style's layer
    flinch_apply(in, m, pose, (const float(*)[4])rows); // (2026-11-23)
    cover_apply(in, m, pose, (const float(*)[4])rows); // (2026-11-23) crouched behind a low cover, leaning out to peek a hit's reaction: the spine and the head thrown away from it, springing back
    { // the morph target curves the pose ended with: the actor's morph targets follow them (anim_morph_weight)
        const PoseCurves* pc = t_npc ? pc_find(pose, false) : NULL;
        uint64_t set = pc ? pc->set : 0;
        if (set) memcpy(in->curve_w, pc->w, sizeof in->curve_w);
        in->curve_set = set;
    }
    anim_world(m, pose, rows, bones);
    // the sliding: the planted (lower, on the ground) foot's drift against the actor's travel since the last pose
    // (the same foot as at the last pose, still the lower one: a swap of feet is a step, not a slide)
    if (g) ik_bind(in, m);
    if (g && in->leg_b[0][2] >= 0 && in->leg_b[1][2] >= 0 && (!chr || chr->on_ground)) {
        int lo = bones[in->leg_b[0][2]][2][3] < bones[in->leg_b[1][2]][2][3] ? 0 : 1;
        int16_t fb = in->leg_b[lo][2];
        float foot[3] = { bones[fb][0][3], bones[fb][1][3], bones[fb][2][3] };
        // (on the ground: within 3 cm of the foot's planted height - its lowest above the floor lately, which rises 2
        // cm/s: an ankle stands some cm up, and higher on another skeleton or crouched)
        float h = foot[2] - rows[2][3];
        if (!in->slide_ok) in->slide_low = h;
        in->slide_low = fminf(in->slide_low + 2.0f * in->pose_dt, h);
        bool down = h < in->slide_low + 3.0f;
        if (in->slide_ok && lo == in->slide_lo && down && in->slide_foot[2] - in->slide_actor[2] < in->slide_low + 3.0f) {
            float dx = foot[0] - in->slide_foot[0], dy = foot[1] - in->slide_foot[1];
            float ax = rows[0][3] - in->slide_actor[0], ay = rows[1][3] - in->slide_actor[1];
            float travel = sqrtf(ax * ax + ay * ay);
            // (walking: over 50 cm/s; turning in place is not travel)
            if (travel > 50.0f * in->pose_dt && travel < 60.0f) {
                float sl = sqrtf(dx * dx + dy * dy);
                in->slide_sum += sl, in->travel_sum += travel;
                if (mm_made_playing(in)) in->slide_made += sl, in->travel_made += travel; // (Motion Matching's made start or stop)
            }
        }
        memcpy(in->slide_foot, foot, 12);
        in->slide_actor[0] = rows[0][3], in->slide_actor[1] = rows[1][3], in->slide_actor[2] = rows[2][3];
        in->slide_ok = true, in->slide_lo = (uint8_t)lo;
    }
    // the IK Rig, then the bodies' physics (ikrig.c: after the pose and its foot IK, before the parts and skins)
    const CSkeletalMesh* skc = ecs_get(w, in->e, COMP_SKELETAL_MESH);
    if (skc && skc->ik_rig) ikrig_apply(w, in->e, ikrig_find(skc->ik_rig), m, bones);
    if (skc && skc->physics_asset && A.playing) phys_apply(w, in->e, phys_find(skc->physics_asset), m, bones, in->pose_dt);
    if (skc && skc->physics_asset) { // (Q10) the dynamic bones (playing), the bodies' capsules for the cloth
        const PhysicsAsset* pa = phys_find(skc->physics_asset);
        if (A.playing) dyn_apply(w, in->e, pa, m, bones, in->pose_dt);
        phys_publish_bodies(in->e, pa, m, (const float(*)[3][4])bones);
    }
    if (g_nhide) hide_bones(in, m, bones); // (Q12: a first-person body's head)
    if (in->nfollow || in->kept >= 0) place_followers(w, in, m, (const float(*)[3][4])bones); // (Q10)
    in->dirty = place_parts(w, in, m, bones);
    if (in->nskins) in->dirty |= place_skins(w, in, m, rows, bones);
    in->ms = (float)((re_time_seconds() - t0) * 1000.0);
}

static void pose_job(void* arg, int index) {
    PoseJob* j = arg;
    uint32_t a = j->first + (uint32_t)index * j->chunk, b = a + j->chunk;
    if (b > j->first + j->count) b = j->first + j->count;
    for (uint32_t i = a; i < b; i++) pose_one(j->w, &insts()[i]);
}

void anim_update(World* w, float dt, bool playing) {
    double t0 = re_time_seconds();
    A.stats.meshes = A.stats.sampled = A.stats.parts = 0;
    // the parts of the skeletal meshes (made when missing, again when the mesh changed or was read again: a reimport)
    if (A.seen_generation != A.mesh_generation) {
        A.seen_generation = A.mesh_generation;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH))) continue;
            CSkeletalMesh* c = ecs_arch_column(ar, COMP_SKELETAL_MESH);
            for (uint32_t r = 0; r < ar->count; r++) c[r].built = 0;
        }
        A.editor_xf = 0;
    }
    bool any = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH)) || !ar->count) continue;
        any = true;
        CSkeletalMesh* c = ecs_arch_column(ar, COMP_SKELETAL_MESH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++)
            if (c[r].built != c[r].mesh) {
                build_parts(w, ents[r], &c[r]);
                ai = (uint32_t)-1; // the archetypes changed: from the start
                break;
            }
    }
    if (!any) {
        A.stats.ms = 0.0;
        return;
    }
    A.frame++;
    if (!A.index_ready) {
        if (!re_vm_reserve(&A.index, (size_t)RE_ECS_MAX_ENTITIES * sizeof(PartRef))) return;
        A.index_ready = true;
    }
    if (!playing) { // the editor: the bind pose (or the component's animation's first frame) wherever the actor is
        // the skeletal meshes' own transforms too: the gizmo drags the selected actor in place (the renderer re-reads
        // the selection every frame) without logging a change, and its parts stayed where they were while the gizmo
        // and the selection moved (relatório #114). A hash of their places: a few ns each
        uint64_t xf = 1469598103934665603ull;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
            const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
            for (uint32_t r = 0; r < ar->count; r++) {
                uint32_t v[9];
                memcpy(v, t[r].location, sizeof v); // (location, rotation, scale: contiguous)
                for (int k = 0; k < 9; k++) xf = (xf ^ v[k]) * 1099511628211ull;
            }
        }
        // Realtime (the viewport's menu): the actors with an animation of their own play it, looping, without Play
        bool rt = false;
        if (A.editor_rt) {
            A.editor_t += dt;
            for (uint32_t ai = 0; ai < w->archetype_count && !rt; ai++) {
                const EcsArchetype* ar = &w->archetypes[ai];
                if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH))) continue;
                const CSkeletalMesh* c = ecs_arch_column(ar, COMP_SKELETAL_MESH);
                for (uint32_t r = 0; r < ar->count && !rt; r++) rt = c[r].anim != 0;
            }
        }
        if (!rt && A.editor_changes == w->changes && A.editor_revision == w->revision && A.editor_xf == xf) {
            A.stats.ms = 0.0;
            return; // nothing moved
        }
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH)) || !ar->count) continue;
            for (uint32_t r = 0; r < ar->count; r++) {
                Entity e = ecs_arch_entities(ar)[r];
                CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
                AnimInst tmp;
                memset(&tmp, 0, sizeof tmp);
                tmp.e = e, tmp.mesh = anim_mesh(c->mesh), tmp.update = true, tmp.kept = -1;
                if (!tmp.mesh) continue;
                if (c->anim) clip_set(&tmp.single, anim_seq(c->anim), tmp.mesh);
                if (rt && tmp.single.seq && tmp.single.seq->fps > 0.0f && tmp.single.seq->nframes > 1) {
                    double len = tmp.single.seq->length > 0.0f ? tmp.single.seq->length : (double)(tmp.single.seq->nframes - 1) / tmp.single.seq->fps;
                    tmp.time = (float)fmod(A.editor_t, len);
                }
                find_parts(w, &tmp);
                if ((!tmp.nparts && tmp.mesh->nparts) || (!tmp.nskins && tmp.mesh->nskins)) { // a copy saved without its parts (a pasted one, an older Ctrl+D): made next frame
                    c->built = 0;
                    continue;
                }
                if (tmp.nskins) skin_pal(e, true);
                pose_one(w, &tmp);
                if (tmp.dirty)
                    for (uint32_t i = 0; i < tmp.nparts; i++) ecs_touch(w, tmp.parts[i]);
                if (tmp.dirty)
                    for (uint32_t i = 0; i < tmp.nskins; i++) ecs_touch(w, tmp.skins[i]);
                A.stats.meshes++, A.stats.parts += tmp.nparts;
            }
        }
        A.editor_changes = w->changes, A.editor_revision = w->revision, A.editor_xf = xf; // (the touches above count as seen)
        A.stats.ms = (re_time_seconds() - t0) * 1000.0;
        return;
    }
    if (!A.ready) return;
    // new skeletal meshes (spawned while playing) get their instance
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_SKELETAL_MESH)) || !ar->count) continue;
        CSkeletalMesh* c = ecs_arch_column(ar, COMP_SKELETAL_MESH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++)
            if (!c[r].inst) c[r].inst = make_inst(w, ents[r], &c[r]);
    }
    // the logic on this thread (the VM, the notifies); far meshes every 2-4 frames (their time adds up)
    uint32_t live = 0;
    // (2026-11-10) the Stylized style's own animation frame rate (Project Settings: 12 = stop-motion, "on twos"): a
    // pose every 1/fps seconds, held between (the actors still move smoothly; their bodies step)
    int step_fps = render_style() == RENDER_STYLE_STYLIZED ? project()->stylized_anim_fps : 0;
    static int uro_on = -1; // (2026-11-24, item 4) the update rate by distance
    if (uro_on < 0) uro_on = getenv("REALENGINE_NO_URO") ? 0 : 1;
    float cam[3];
    bool have_cam = render_last_camera(cam);
    float step_dt = step_fps > 0 ? 1.0f / (float)step_fps : 0.0f;
    for (uint32_t i = 0; i < A.ninst; i++) {
        AnimInst* in = &insts()[i];
        in->update = false;
        if (!ecs_alive(w, in->e) || !in->mesh) continue;
        live++;
        in->acc += dt;
        if (step_dt > 0.0f && in->acc < step_dt) continue;
        // what the camera may see every frame; the rest every 4th (its parts' relevance: the renderer's)
        uint32_t every = (!in->nparts && !in->nskins) || ecs_relevant(w, in->nparts ? in->parts[0] : in->skins[0]) ? 1u : 4u;
        // (2026-11-24, item 4: Unreal's Update Rate Optimizations) and by distance: 15 m every frame, then every 2nd, 4th,
        // 8th (the pose's time adds up: it moves as fast, in steps). REALENGINE_NO_URO=1: off
        // (2026-11-25: not a Character's: the player's or an AI's pose drives its traversal, its IK, its montages' hits every
        // frame; a 2-frame step left the Climb's hands 19 cm off the ledge in the traversal test)
        bool character = ecs_get(w, in->e, COMP_CHARACTER) != NULL;
        if (!character) { // (a Character Blueprint's Skeletal Mesh component: its owner is the Character)
            const CAttach* ca = ecs_get(w, in->e, COMP_ATTACH);
            character = ca && ca->owner && ecs_alive(w, (Entity)ca->owner) && ecs_get(w, (Entity)ca->owner, COMP_CHARACTER);
        }
        if (uro_on && have_cam && every < 8u && !character) {
            const CTransform* et = ecs_get(w, in->e, COMP_TRANSFORM);
            if (et) {
                float dx = et->location[0] - cam[0], dy = et->location[1] - cam[1], dz = et->location[2] - cam[2], d2 = dx * dx + dy * dy + dz * dz;
                uint32_t by = d2 < 1500.0f * 1500.0f ? 1u : d2 < 4000.0f * 4000.0f ? 2u : d2 < 8000.0f * 8000.0f ? 4u : 8u;
                if (by > every) every = by;
            }
        }
        if ((A.frame + i) % every) continue;
        step_logic(w, in, in->acc);
        in->pose_dt = in->acc;
        in->acc = 0.0f;
        in->update = true;
        A.stats.sampled++;
    }
    pairs_update(w); // (2026-11-23) the paired montages' partners: their time and place from their primaries
    sfollow_update(w); // (2026-11-23) the actors on Static Meshes' sockets
    // the IK Rigs and Physics Assets loaded and the simulations started by the components here (the jobs only read)
    for (uint32_t i = 0; i < A.ninst; i++) {
        AnimInst* in = &insts()[i];
        const CSkeletalMesh* c = in->update ? ecs_get(w, in->e, COMP_SKELETAL_MESH) : NULL;
        if (c && (c->ik_rig || c->physics_asset)) ikphys_prepare(in->e, c->ik_rig, c->physics_asset, (int)c->simulate, c->simulate_bone, c->physics_blend);
        if (c && c->physics_asset && A.playing) ikphys_main(w, in->e); // (2026-11-16) the active ragdoll's actor moves, held while down
    }
    follow_assign(w); // (Q10) what hangs on the bones and sockets: each instance's
    // the poses in parallel (each instance writes its own parts)
    // (Q10: a few dozen characters with ragdolls are heavy poses - 0.05..0.1 ms each: 8 a task spread them over the
    // workers; thousands of light ones keep 64 a task)
    PoseJob job = { w, 0, A.ninst, A.ninst <= 512u ? 8u : 64u };
    int tasks = (int)((A.ninst + job.chunk - 1u) / job.chunk);
    if (tasks > 1) re_parallel_for(tasks, pose_job, &job);
    else
        for (uint32_t i = 0; i < A.ninst; i++) pose_one(w, &insts()[i]);
    for (uint32_t k = 0; k < g_nfollow; k++) // (Q10) the actors hung on sockets that moved: their renderer and collision told
        if (g_follow[k].moved) g_follow[k].moved = false, ecs_touch(w, g_follow[k].child);
    // (a Movable part needs no touch: the renderer compares its dynamic set every frame; touching thousands would
    // overflow the change log and rebuild the whole scene)
    for (uint32_t i = 0; i < A.ninst; i++) {
        AnimInst* in = &insts()[i];
        A.stats.parts += in->nparts;
        if (in->update && in->dirty)
            for (uint32_t k = 0; k < in->nskins; k++) {
                // (2026-11-24, item 4) a Movable skin neither: the dynamic set compares its palette's version every frame
                // (render.c skin_still); 2 touches a character overflowed the log of 4096 at ~2048 animated characters
                const CTransform* st = ecs_get(w, in->skins[k], COMP_TRANSFORM);
                if (st && (st->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE) continue;
                ecs_touch(w, in->skins[k]);
            }
        if (!in->update || !in->dirty || !in->nparts) continue;
        const CTransform* pt = ecs_get(w, in->parts[0], COMP_TRANSFORM);
        if (pt && (pt->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE) continue;
        for (uint32_t p = 0; p < in->nparts; p++) ecs_touch(w, in->parts[p]);
    }
    A.stats.meshes = live;
    ikphys_frame_end();
    float slide = 0.0f, travel = 0.0f;
    float slide_m = 0.0f, travel_m = 0.0f;
    for (uint32_t i = 0; i < A.ninst; i++)
        slide += insts()[i].slide_sum, travel += insts()[i].travel_sum, slide_m += insts()[i].slide_made, travel_m += insts()[i].travel_made;
    A.stats.mm_play_slide = travel_m > 1.0f ? slide_m / travel_m : 0.0f, A.stats.mm_play_travel = travel_m;
    A.stats.foot_slide = travel > 1.0f ? slide / travel : 0.0f, A.stats.travel_cm = travel;
    A.stats.mm_searches = g_mm_stats.searches, A.stats.mm_switches = g_mm_stats.switches, A.stats.mm_ms = g_mm_stats.ms;
    A.stats.mm_frames = 0;
    A.stats.mm_made = 0, A.stats.mm_made_slide = 0.0f, A.stats.mm_starts = g_mm_stats.starts, A.stats.mm_stops = g_mm_stats.stops;
    A.stats.mm_turn_s = (float)g_mm_stats.turn_s, A.stats.mm_authored = g_mm_stats.authored;
    A.stats.mm_jumps = g_mm_stats.jumps, A.stats.mm_jump_s = (float)g_mm_stats.jump_s, A.stats.mm_jump_rise = g_mm_stats.jump_rise;
    memcpy(A.stats.mm_jump_ends, g_mm_stats.jump_ends, sizeof A.stats.mm_jump_ends);
    for (uint32_t i = 0; i < g_nmm; i++) {
        A.stats.mm_frames += g_mm[i].n;
        for (uint32_t k = 0; k < g_mm[i].ngroups; k++)
            if (g_mm[i].groups[k].bake >= 0 && g_mm[i].groups[k].rate == 1.0f) A.stats.mm_made++, A.stats.mm_made_slide += g_mm[i].groups[k].slide;
    }
    if (A.stats.mm_made) A.stats.mm_made_slide /= (float)A.stats.mm_made;
    A.stats.ms = (re_time_seconds() - t0) * 1000.0;
}

bool anim_play_montage(World* w, Entity actor, const char* montage, float rate) {
    AnimInst* in = inst_of_actor(w, actor);
    AnimMontage* m = anim_montage_name(montage);
    if (!in || !m || !in->mesh) return false;
    AnimSeq* s = anim_seq_name(m->anim);
    if (!clip_set(&in->mclip, s, in->mesh)) return false;
    in->montage = m;
    in->mtime = 0.0f;
    in->rm_tr = m->root_motion ? rm_track(s, in->mesh) : NULL; // (2026-11-23)
    in->mrate = (rate > 0.0f ? rate : 1.0f) * (m->rate > 0.0f ? m->rate : 1.0f);
    in->mweight = 0.0f;
    in->mstopping = false, in->mcut = false;
    if (!in->has_upper) in->has_upper = bone_mask(in->mesh, in->graph ? in->graph->upper_bone : "spine_01", in->upper_mask);
    return true;
}

void anim_stop_montage(World* w, Entity actor, float blend_out) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->montage) return;
    in->mstopping = true, in->mcut = true;
    in->mblend_out = fmaxf(blend_out, 0.01f);
}

// (Q10) the montage playing on a skeletal mesh at another rate (0: held on its frame; Unreal's Montage Set Play Rate)
void anim_set_montage_rate(World* w, Entity actor, float rate) {
    AnimInst* in = inst_of_actor(w, actor);
    if (in && in->montage) in->mrate = fmaxf(rate, 0.0f) * (in->montage->rate > 0.0f ? in->montage->rate : 1.0f);
}
// (Q10) the montage playing (its name), NULL when none; *t its time
const char* anim_montage_playing(World* w, Entity actor, float* t) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->montage) return NULL;
    if (t) *t = in->mtime;
    return in->montage->name;
}

void anim_play(World* w, Entity actor, const char* sequence, bool loop) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->mesh) return;
    AnimSeq* s = anim_seq_name(sequence);
    if (!s) return;
    clip_set(&in->single, s, in->mesh);
    in->graph = NULL; // Unreal's: Play Animation switches the mesh to the Animation mode
    in->time = 0.0f;
    in->loop = loop;
}

// Back from the Animation mode (anim_play) to the mesh's Animation Blueprint (Unreal's Set Animation Mode: Use Animation Blueprint)
void anim_use_blueprint(World* w, Entity actor) {
    AnimInst* in = inst_of_actor(w, actor);
    const CSkeletalMesh* c = in ? ecs_get(w, in->e, COMP_SKELETAL_MESH) : NULL;
    if (!c) return;
    BpAsset* cls = c->anim_class ? bp_find_hash(c->anim_class) : NULL;
    in->graph = anim_graph_of(cls, false);
    in->time = 0.0f, in->loop = true;
    in->montage = NULL;
}

bool anim_uses_blueprint(World* w, Entity actor) {
    AnimInst* in = inst_of_actor(w, actor);
    return in && in->graph;
}

const AnimStats* anim_stats(void) { return &A.stats; }

uint32_t anim_profile_items(Entity* e, float* ms, const char** state, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < A.ninst && n < cap; i++) {
        const AnimInst* in = &insts()[i];
        e[n] = in->actor, ms[n] = in->ms, state[n] = in->state_name ? in->state_name : "-";
        n++;
    }
    return n;
}

void anim_part_rows(const SkPart* p, const float bone[3][4], float out[3][4]) { xf_rows(&p->local, bone, out); }
void anim_xf_rows(const AnimXf* x, const float parent[3][4], float out[3][4]) { xf_rows(x, parent, out); }

// --- (Q7) the sockets ---
int anim_socket_index(const SkelMesh* m, const char* name) {
    if (!m || !name || !name[0]) return -1;
    for (uint32_t i = 0; i < m->nsockets; i++)
        if (!_stricmp(m->sockets[i].name, name)) return (int)i;
    return -1;
}
AnimSocket* anim_socket_add(SkelMesh* m, int bone) {
    if (!m || m->nsockets >= ANIM_MAX_SOCKETS || bone < 0 || bone >= (int)m->nbones) return NULL;
    AnimSocket* k = &m->sockets[m->nsockets];
    memset(k, 0, sizeof *k);
    k->bone = (int16_t)bone;
    xf_identity(&k->local);
    char base[ANIM_NAME];
    snprintf(base, sizeof base, "%.40sSocket", m->bones[bone].name);
    snprintf(k->name, sizeof k->name, "%s", base);
    for (int n = 1; anim_socket_index(m, k->name) >= 0 && n < 1000; n++) snprintf(k->name, sizeof k->name, "%.40s%d", base, n);
    m->nsockets++;
    m->dirty = true;
    return k;
}
void anim_socket_rows(const SkelMesh* m, const AnimSocket* s, const float (*bones)[3][4], float out[3][4]) {
    int b = s->bone >= 0 && s->bone < (int)m->nbones ? s->bone : 0;
    xf_rows(&s->local, bones[b], out);
}

// --- The editors' previews of an Anim Blueprint (anim.h): an instance of their own, no world (no actor, no notifies) ---

static struct { AnimInst in; const AnimGraph* g; const SkelMesh* m; } g_pv[ANIM_PREVIEW_SLOTS];

bool anim_preview_pose(int slot, AnimGraph* g, SkelMesh* m, float dt, const AnimPreviewVar* vars, uint32_t nvars, AnimXf* pose) {
    if (slot < 0 || slot >= ANIM_PREVIEW_SLOTS || !g || !m) return false;
    AnimInst* in = &g_pv[slot].in;
    if (g_pv[slot].g != g || g_pv[slot].m != m || dt < 0.0f) { // (another graph or mesh, or asked to: from its entry states)
        free(in->inert);
        memset(in, 0, sizeof *in);
        in->mesh = m, in->graph = g, in->rate = 1.0f, in->loop = true, in->inert_node = -1;
        for (uint32_t k = 0; k < ANIM_MAX_MACHINES; k++) in->sm[k].state = in->sm[k].prev = g->machine_entry[k];
        in->has_upper = bone_mask(m, g->upper_bone, in->upper_mask);
        g_pv[slot].g = g, g_pv[slot].m = m;
        dt = 0.0f;
    }
    in->pv = vars, in->npv = nvars, in->update = true;
    step_logic(NULL, in, fminf(dt, 0.1f));
    float rows[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } }, bones[ANIM_MAX_BONES][3][4];
    if (g->nnodes) {
        ik_bind(in, m);
        nodes_pose(NULL, in, pose, rows, bones, NULL);
    } else
        for (uint32_t i = 0; i < m->nbones; i++) pose[i] = m->bones[i].bind;
    return true;
}

const char* anim_preview_state(int slot) {
    return slot >= 0 && slot < ANIM_PREVIEW_SLOTS && g_pv[slot].in.state_name ? g_pv[slot].in.state_name : "";
}

bool anim_foot_offsets(World* w, Entity actor, float feet[2], float* pelvis) {
    AnimInst* in = inst_of_actor(w, actor);
    if (!in || !in->graph || !in->graph->foot_ik) return false;
    feet[0] = in->foot_off[0], feet[1] = in->foot_off[1];
    if (pelvis) *pelvis = in->pelvis_off;
    return true;
}

// A skeletal mesh moved in place in the editor, as the gizmo drags the selection (no change logged): its parts must be
// at the new place after the next update (relatório #114: the gizmo moved, the Delight stayed), and an update with
// nothing moved must skip the poses
bool anim_test_editor_move(World* w) {
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    Entity e = m && m->nparts ? ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH)) : ENTITY_NONE;
    if (e == ENTITY_NONE) {
        printf("[RealEngine] FAIL: skeletal mesh moved in the editor (no SK_Mannequin)\n");
        return false;
    }
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memset(t, 0, sizeof *t);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    ((CSkeletalMesh*)ecs_get(w, e, COMP_SKELETAL_MESH))->mesh = m->hash;
    anim_update(w, 0.016f, false); // (its parts made and posed)
    AnimInst in;
    memset(&in, 0, sizeof in);
    in.e = e;
    find_parts(w, &in);
    const CTransform* pt = in.nparts ? ecs_get(w, in.parts[0], COMP_TRANSFORM) : NULL;
    float x0 = pt ? pt->location[0] : 0.0f;
    t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] += 300.0f; // the gizmo's drag: in place, untouched
    double t0 = re_time_seconds();
    anim_update(w, 0.016f, false);
    double ms = (re_time_seconds() - t0) * 1000.0;
    pt = in.nparts ? ecs_get(w, in.parts[0], COMP_TRANSFORM) : NULL;
    float dx = pt ? pt->location[0] - x0 : 0.0f;
    bool followed = fabsf(dx - 300.0f) < 0.5f;
    anim_update(w, 0.016f, false); // nothing moved
    bool idle = A.stats.meshes == 0;
    printf("[RealEngine] Skeletal mesh moved in the editor: %u parts, followed %d (%.1f of 300 cm, %.3f ms), idle %d\n", in.nparts, followed, dx,
           ms, idle);
    destroy_parts(w, e);
    ecs_destroy(w, e);
    bool pass = in.nparts && followed && idle;
    if (!pass) printf("[RealEngine] FAIL: skeletal mesh moved in the editor (its parts must follow)\n");
    return pass;
}

bool anim_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: anim selftest: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
    // a 3 x 3 grid (speed 0..600 x direction -90..90): on a sample its weight is 1; the weights sum 1; in a cell's middle
    // its 4 corners share it
    float sx[9], sy[9], wv[9];
    for (int i = 0; i < 9; i++) sx[i] = (float)(i % 3) * 300.0f, sy[i] = (float)(i / 3) * 90.0f - 90.0f;
    for (int i = 0; i < 9; i++) {
        anim_blend2d_weights(sx, sy, 9, sx[i], sy[i], wv);
        CHECK(fabsf(wv[i] - 1.0f) < 1e-4f, "sample %d weighs %.4f on itself", i, wv[i]);
    }
    anim_blend2d_weights(sx, sy, 9, 150.0f, -45.0f, wv);
    float sum = 0.0f;
    for (int i = 0; i < 9; i++) sum += wv[i];
    CHECK(fabsf(sum - 1.0f) < 1e-4f, "the weights sum %.4f", sum);
    CHECK(fabsf(wv[0] - wv[1]) < 1e-3f && fabsf(wv[0] - wv[3]) < 1e-3f && fabsf(wv[0] - wv[4]) < 1e-3f && wv[0] > 0.2f,
          "a cell's middle: %.3f %.3f %.3f %.3f", wv[0], wv[1], wv[3], wv[4]);
    anim_blend2d_weights(sx, sy, 9, 9000.0f, 0.0f, wv); // (far outside: the nearest edge)
    sum = 0.0f;
    for (int i = 0; i < 9; i++) sum += wv[i];
    CHECK(fabsf(sum - 1.0f) < 1e-4f && wv[5] > 0.99f, "outside: sum %.3f, the nearest %.3f", sum, wv[5]);
    // two bone IK: a leg 42 + 41 cm reaching a target 20 cm up and ahead: the end there, the bones their lengths
    float a[3] = { 0, 0, 90 }, b[3] = { 2, 0, 48 }, c[3] = { 0, 0, 7 }, t[3] = { 10, 0, 27 }, nb[3], nc[3];
    anim_two_bone_ik(a, b, c, t, nb, nc);
    float l1 = sqrtf((nb[0] - a[0]) * (nb[0] - a[0]) + (nb[1] - a[1]) * (nb[1] - a[1]) + (nb[2] - a[2]) * (nb[2] - a[2]));
    float l2 = sqrtf((nc[0] - nb[0]) * (nc[0] - nb[0]) + (nc[1] - nb[1]) * (nc[1] - nb[1]) + (nc[2] - nb[2]) * (nc[2] - nb[2]));
    float d = sqrtf((nc[0] - t[0]) * (nc[0] - t[0]) + (nc[1] - t[1]) * (nc[1] - t[1]) + (nc[2] - t[2]) * (nc[2] - t[2]));
    float o1 = sqrtf(2.0f * 2.0f + 42.0f * 42.0f), o2 = sqrtf(2.0f * 2.0f + 41.0f * 41.0f);
    CHECK(d < 0.05f, "the foot %.3f cm from its target", d);
    CHECK(fabsf(l1 - o1) < 0.05f && fabsf(l2 - o2) < 0.05f, "the bones %.2f / %.2f (were %.2f / %.2f)", l1, l2, o1, o2);
    CHECK(nb[0] > 0.0f, "the knee bent backward (%.2f)", nb[0]);
    // out of reach: straight toward it
    float far_t[3] = { 0, 0, -200 };
    anim_two_bone_ik(a, b, c, far_t, nb, nc);
    CHECK(fabsf(nc[2] - (a[2] - o1 - o2)) < 0.1f && fabsf(nc[0]) < 0.1f, "out of reach: the foot at %.2f, %.2f", nc[0], nc[2]);
    // blending the same pose with itself: the same pose
    AnimXf p[2];
    memset(p, 0, sizeof p);
    p[0].q[0] = 0.3f, p[0].q[3] = 0.95393920f, p[0].t[2] = 5.0f, p[0].s[0] = p[0].s[1] = p[0].s[2] = 1.0f;
    p[1] = p[0];
    const AnimXf* ps[2] = { &p[0], &p[1] };
    float bw[2] = { 0.3f, 0.7f };
    AnimXf o;
    anim_blend_n(ps, bw, 2, 1, &o);
    CHECK(fabsf(o.q[0] - p[0].q[0]) < 1e-4f && fabsf(o.t[2] - 5.0f) < 1e-4f, "a pose blended with itself changed");
    fails += anim_nodes_selftest();
#undef CHECK
    printf("[RealEngine] Animation self test: %s\n", fails ? "FAILED" : "passed (blend space 2D weights, two bone IK, blend N, AnimGraph nodes)");
    return fails == 0;
}

void anim_euler_quat(const float rot_deg[3], float q[4]) {
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    float qx[4], qy[4], qz[4], t[4];
    // (rotator_axes' signs: a positive pitch raises X, a positive roll lowers Y's far side)
    q_axis(X, -rot_deg[2] * DEG, qx), q_axis(Y, -rot_deg[0] * DEG, qy), q_axis(Z, rot_deg[1] * DEG, qz);
    q_mul(qz, qy, t), q_mul(t, qx, q);
}

void anim_quat_euler(const float q[4], float rot_deg[3]) {
    float R[3][3];
    q_axes(q, R);
    axes_rotator(R, rot_deg);
}

void anim_rescan(void) { A.scanned = false; } // (the assets already loaded stay: a load finds them by their hash)

// (Q11) the entities an actor's Skeletal Mesh draws with (its parts and its skins): the outline set on the actor reaches them
uint32_t anim_mesh_parts(World* w, Entity actor, Entity* out, uint32_t cap) {
    AnimInst* in = inst_of_actor(w, actor);
    uint32_t n = 0;
    for (uint32_t k = 0; in && k < in->nparts && n < cap; k++)
        if (in->parts[k] && ecs_alive(w, in->parts[k])) out[n++] = in->parts[k];
    for (uint32_t k = 0; in && k < in->nskins && n < cap; k++)
        if (in->skins[k] && ecs_alive(w, in->skins[k])) out[n++] = in->skins[k];
    return n;
}
