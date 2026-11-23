// Copied from the Tlender (src/ik_solver.c) for the importer's rig (tlend_import.c): Blender's Standard IK.
// Blender's Standard IK solver (see ik_solver.h). Matrices are 3x3 rows (M[r][c], column vectors).
#include "ik_solver.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPS 1e-20
#define STIFF_EPS 0.01           // stiffness is kept below 1 - this (a free axis never locks by stiffness)
#define STRETCH_MIN 0.001         // the stretch translation's limits along Y
#define STRETCH_MAX 1e10
#define MAX_ANGLE_CHANGE (3.14159265358979323846 / 4.0)

typedef struct { double x, y, z; } D3;
typedef struct { double m[3][3]; } D33;

static D3 d3(double x, double y, double z) { D3 r = { x, y, z }; return r; }
static D3 d3_add(D3 a, D3 b) { return d3(a.x + b.x, a.y + b.y, a.z + b.z); }
static D3 d3_sub(D3 a, D3 b) { return d3(a.x - b.x, a.y - b.y, a.z - b.z); }
static D3 d3_scale(D3 a, double s) { return d3(a.x * s, a.y * s, a.z * s); }
static double d3_dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static D3 d3_cross(D3 a, D3 b) { return d3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double d3_len(D3 a) { return sqrt(d3_dot(a, a)); }
static D3 d3_norm(D3 a) {
    double l = d3_len(a);
    return l > 0.0 ? d3_scale(a, 1.0 / l) : a;
}
static double d3_get(D3 a, int i) { return i == 0 ? a.x : i == 1 ? a.y : a.z; }
static void d3_set(D3* a, int i, double v) {
    if (i == 0) a->x = v;
    else if (i == 1) a->y = v;
    else a->z = v;
}

static D33 m3_identity(void) {
    D33 r = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
    return r;
}
static D33 m3_rows(double a, double b, double c, double d, double e, double f, double g, double h, double i) {
    D33 r = { { { a, b, c }, { d, e, f }, { g, h, i } } };
    return r;
}
static D33 m3_mul(D33 a, D33 b) {
    D33 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
static D33 m3_t(D33 a) {
    D33 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
    return r;
}
static D3 m3_v(D33 a, D3 v) {
    return d3(a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
              a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z);
}
static D3 m3_col(D33 a, int c) { return d3(a.m[0][c], a.m[1][c], a.m[2][c]); }
static D3 m3_row(D33 a, int r) { return d3(a.m[r][0], a.m[r][1], a.m[r][2]); }
static void m3_set_row(D33* a, int r, D3 v) {
    a->m[r][0] = v.x;
    a->m[r][1] = v.y;
    a->m[r][2] = v.z;
}
static D33 m3_inverse(D33 a) {
    double det = a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
                 a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
                 a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
    if (fabs(det) < 1e-300) return m3_identity();
    double id = 1.0 / det;
    D33 r;
    r.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) * id;
    r.m[0][1] = (a.m[0][2] * a.m[2][1] - a.m[0][1] * a.m[2][2]) * id;
    r.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) * id;
    r.m[1][0] = (a.m[1][2] * a.m[2][0] - a.m[1][0] * a.m[2][2]) * id;
    r.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) * id;
    r.m[1][2] = (a.m[0][2] * a.m[1][0] - a.m[0][0] * a.m[1][2]) * id;
    r.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) * id;
    r.m[2][1] = (a.m[0][1] * a.m[2][0] - a.m[0][0] * a.m[2][1]) * id;
    r.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) * id;
    return r;
}

static bool fuzzy_zero(double x) { return fabs(x) < EPS; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static D33 rotation_sc(double s, double c, int axis) {
    if (axis == 0) return m3_rows(1, 0, 0, 0, c, -s, 0, s, c);
    if (axis == 1) return m3_rows(c, 0, s, 0, 1, 0, -s, 0, c);
    return m3_rows(c, -s, 0, s, c, 0, 0, 0, 1);
}
static D33 rotation(double angle, int axis) { return rotation_sc(sin(angle), cos(angle), axis); }

static double euler_from_matrix(D33 R, int axis) {
    double t = sqrt(R.m[0][0] * R.m[0][0] + R.m[0][1] * R.m[0][1]);
    if (t > 16.0 * EPS) {
        if (axis == 0) return -atan2(R.m[1][2], R.m[2][2]);
        if (axis == 1) return atan2(-R.m[0][2], t);
        return -atan2(R.m[0][1], R.m[0][0]);
    }
    if (axis == 0) return -atan2(-R.m[2][1], R.m[1][1]);
    if (axis == 1) return atan2(-R.m[0][2], t);
    return 0.0;
}

static double safe_acos(double f) { return f <= -1.0 ? 3.14159265358979323846 : f >= 1.0 ? 0.0 : acos(f); }

static D3 matrix_to_axis_angle(D33 R) {
    D3 delta = d3(R.m[2][1] - R.m[1][2], R.m[0][2] - R.m[2][0], R.m[1][0] - R.m[0][1]);
    double c = safe_acos((R.m[0][0] + R.m[1][1] + R.m[2][2] - 1) / 2);
    double l = d3_len(delta);
    if (!fuzzy_zero(l)) delta = d3_scale(delta, c / l);
    return delta;
}

// Twist about Y (the y and w parts of the rotation's quaternion), its matrix, and the rotation without it.
static double compute_twist(D33 R) {
    double qy = R.m[0][2] - R.m[2][0];
    double qw = R.m[0][0] + R.m[1][1] + R.m[2][2] + 1;
    return 2.0 * atan2(qy, qw);
}
static D33 twist_matrix(double tau) { return rotation(tau, 1); }
static D33 remove_twist(D33 R) { return m3_mul(R, m3_t(twist_matrix(compute_twist(R)))); }

// Swing (x, z: sines of half angles, as axis-angle parameters) and twist of a rotation, and back.
static D3 spherical_range(D33 R) {
    double tau = compute_twist(R);
    double num = 2.0 * (1.0 + R.m[1][1]);
    if (fabs(num) < EPS) return d3(0.0, tau, 1.0);
    num = 1.0 / sqrt(num);
    return d3(-R.m[2][1] * num, tau, R.m[0][1] * num);
}
static D33 quat_matrix(double w, double x, double y, double z) {
    double xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    return m3_rows(1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
                   2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy));
}
static D33 swing_matrix(double ax, double az) {
    double sine2 = ax * ax + az * az;
    double cosine2 = sqrt(sine2 >= 1.0 ? 0.0 : 1.0 - sine2);
    return quat_matrix(-cosine2, ax, 0.0, az);
}

static bool ellipse_clamp(double* ax, double* az, const double* amin, const double* amax) {
    double xlim, zlim, x, z;
    if (*ax < 0.0) {
        x = -*ax;
        xlim = -amin[0];
    } else {
        x = *ax;
        xlim = amax[0];
    }
    if (*az < 0.0) {
        z = -*az;
        zlim = -amin[1];
    } else {
        z = *az;
        zlim = amax[1];
    }
    if (fuzzy_zero(xlim) || fuzzy_zero(zlim)) {
        if (x <= xlim && z <= zlim) return false;
        if (x > xlim) x = xlim;
        if (z > zlim) z = zlim;
    } else {
        double invx = 1.0 / (xlim * xlim), invz = 1.0 / (zlim * zlim);
        if (x * x * invx + z * z * invz <= 1.0) return false;
        if (fuzzy_zero(x)) {
            x = 0.0;
            z = zlim;
        } else {
            double rico = z / x, old_x = x;
            x = sqrt(1.0 / (invx + invz * rico * rico));
            if (old_x < 0.0) x = -x;
            z = rico * x;
        }
    }
    *ax = *ax < 0.0 ? -x : x;
    *az = *az < 0.0 ? -z : z;
    return true;
}

// --- Segments ---

typedef enum { SEG_NULL, SEG_SPHERICAL, SEG_SWING, SEG_ELBOW, SEG_REVOLUTE, SEG_TRANSLATE } SegKind;

typedef struct Seg {
    SegKind kind;
    bool translational;
    int parent, composite; // -1: none
    int ndof, dof_id;
    bool locked[3];
    double weight[3];
    double max_extension;
    D3 start, translation, orig_translation;
    D33 rest, basis, orig_basis, new_basis;
    D3 gstart, gpos; // the global start and end
    D33 glin;
    // revolute, elbow
    int axis;
    double angle, new_angle, twist, new_twist, cos_twist, sin_twist;
    bool limit, limit_twist;
    double lmin, lmax, min_twist, max_twist;
    // spherical, swing
    bool limit_x, limit_y, limit_z;
    double smin[2], smax[2], min_y, max_y, locked_ax, locked_ay, locked_az;
    // translation
    bool axis_enabled[3], tlimit[3];
    int axes[3];
    double tmin[3], tmax[3];
    D3 new_translation;
} Seg;

static void seg_init(Seg* s, SegKind kind, int ndof, bool translational) {
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->ndof = ndof;
    s->translational = translational;
    s->parent = s->composite = -1;
    s->weight[0] = s->weight[1] = s->weight[2] = 1.0;
    s->rest = s->basis = s->orig_basis = s->new_basis = s->glin = m3_identity();
    s->cos_twist = 1.0;
}

static void seg_set_basis(Seg* s, D33 basis) {
    switch (s->kind) {
    case SEG_SWING: s->basis = remove_twist(basis); break;
    case SEG_ELBOW:
        s->twist = compute_twist(basis);
        s->angle = euler_from_matrix(basis, s->axis);
        s->basis = m3_mul(rotation(s->angle, s->axis), twist_matrix(s->twist));
        break;
    case SEG_REVOLUTE:
        if (s->axis == 1) {
            s->angle = compute_twist(basis);
            s->basis = twist_matrix(s->angle);
        } else {
            s->angle = euler_from_matrix(basis, s->axis);
            s->basis = rotation(s->angle, s->axis);
        }
        break;
    default: s->basis = basis; break;
    }
}

static void seg_set_transform(Seg* s, D3 start, D33 rest, D33 basis, double length) {
    s->max_extension = d3_len(start) + length;
    s->start = start;
    s->rest = rest;
    s->orig_basis = basis;
    seg_set_basis(s, basis);
    s->translation = s->orig_translation = d3(0, length, 0);
}

static D3 seg_axis(const Seg* s, int dof) {
    switch (s->kind) {
    case SEG_SPHERICAL: return m3_col(s->glin, dof);
    case SEG_SWING: return m3_col(s->glin, dof == 0 ? 0 : 2);
    case SEG_ELBOW:
        if (dof == 0) {
            D3 v = s->axis == 0 ? d3(s->cos_twist, 0, s->sin_twist) : d3(-s->sin_twist, 0, s->cos_twist);
            return m3_v(s->glin, v);
        }
        return m3_col(s->glin, 1);
    case SEG_REVOLUTE: return m3_col(s->glin, s->axis);
    case SEG_TRANSLATE: return m3_col(s->glin, s->axes[dof]);
    default: return d3(0, 0, 0);
    }
}

static void seg_set_weight(Seg* s, int axis, double w) {
    switch (s->kind) {
    case SEG_SPHERICAL: s->weight[axis] = w; break;
    case SEG_SWING:
        if (axis == 0) s->weight[0] = w;
        else if (axis == 2) s->weight[1] = w;
        break;
    case SEG_ELBOW:
        if (axis == s->axis) s->weight[0] = w;
        else if (axis == 1) s->weight[1] = w;
        break;
    case SEG_REVOLUTE:
        if (axis == s->axis) s->weight[0] = w;
        break;
    case SEG_TRANSLATE:
        for (int i = 0; i < s->ndof; i++)
            if (s->axes[i] == axis) s->weight[i] = w;
        break;
    default: break;
    }
}

static void seg_set_limit(Seg* s, int axis, double lmin, double lmax) {
    switch (s->kind) {
    case SEG_SPHERICAL:
        if (lmin > lmax) return;
        lmin = clampd(lmin, -3.14159265358979323846, 3.14159265358979323846);
        lmax = clampd(lmax, -3.14159265358979323846, 3.14159265358979323846);
        if (axis == 1) {
            s->min_y = lmin;
            s->max_y = lmax;
            s->limit_y = true;
        } else {
            lmin = sin(lmin * 0.5);
            lmax = sin(lmax * 0.5);
            int k = axis == 0 ? 0 : 1;
            s->smin[k] = -lmax;
            s->smax[k] = -lmin;
            if (axis == 0) s->limit_x = true;
            else s->limit_z = true;
        }
        break;
    case SEG_SWING:
        if (lmin > lmax || axis == 1) return;
        lmin = sin(clampd(lmin, -3.14159265358979323846, 3.14159265358979323846) * 0.5);
        lmax = sin(clampd(lmax, -3.14159265358979323846, 3.14159265358979323846) * 0.5);
        if (axis == 0) {
            s->smin[0] = -lmax;
            s->smax[0] = -lmin;
            s->limit_x = true;
        } else {
            s->smin[1] = -lmax;
            s->smax[1] = -lmin;
            s->limit_z = true;
        }
        break;
    case SEG_ELBOW:
        if (lmin > lmax) return;
        lmin = clampd(lmin, -3.14159265358979323846, 3.14159265358979323846);
        lmax = clampd(lmax, -3.14159265358979323846, 3.14159265358979323846);
        if (axis == 1) {
            s->min_twist = lmin;
            s->max_twist = lmax;
            s->limit_twist = true;
        } else if (axis == s->axis) {
            s->lmin = lmin;
            s->lmax = lmax;
            s->limit = true;
        }
        break;
    case SEG_REVOLUTE:
        if (lmin > lmax || s->axis != axis) return;
        s->lmin = clampd(lmin, -3.14159265358979323846, 3.14159265358979323846);
        s->lmax = clampd(lmax, -3.14159265358979323846, 3.14159265358979323846);
        s->limit = true;
        break;
    case SEG_TRANSLATE:
        if (lmax < lmin) return;
        s->tlimit[axis] = true;
        s->tmin[axis] = lmin;
        s->tmax[axis] = lmax;
        break;
    default: break;
    }
}

static void seg_scale(Seg* s, double scale) {
    s->start = d3_scale(s->start, scale);
    s->translation = d3_scale(s->translation, scale);
    s->orig_translation = d3_scale(s->orig_translation, scale);
    s->gstart = d3_scale(s->gstart, scale);
    s->gpos = d3_scale(s->gpos, scale);
    s->max_extension *= scale;
    if (s->kind == SEG_TRANSLATE) {
        // as Blender does it: the minimum of X and the maximum of Y, three times each
        for (int i = 0; i < 3; i++) {
            s->tmin[0] *= scale;
            s->tmax[1] *= scale;
        }
        s->new_translation = d3_scale(s->new_translation, scale);
    }
}

// --- Jacobian ---

typedef struct Jac {
    int dof, task;
    bool transpose;
    double* jac;   // task x dof
    double* beta;  // task
    double *weight, *weight_sqrt, *d_theta, *d_theta_tmp, *d_norm_weight, *norm;
    double *U, *W, *V, *A, *AV; // SVD: U task x k, W k, V dof x k (k = min); A, AV scratch
    int k;
} Jac;

static bool jac_alloc(Jac* j, int dof, int task) {
    memset(j, 0, sizeof *j);
    j->dof = dof;
    j->task = task;
    j->transpose = task < dof;
    j->k = task < dof ? task : dof;
    int rows = task > dof ? task : dof;
    size_t nd = (size_t)dof, nt = (size_t)task, nk = (size_t)j->k, nr = (size_t)rows;
    j->jac = calloc(nt * nd + 1, sizeof(double));
    j->beta = calloc(nt + 1, sizeof(double));
    j->weight = calloc(nd * 6, sizeof(double));
    j->U = calloc(nt * nk + nk + nd * nk + nr * nk + nk * nk + 1, sizeof(double));
    if (!j->jac || !j->beta || !j->weight || !j->U) return false;
    j->weight_sqrt = j->weight + nd;
    j->d_theta = j->weight + 2 * nd;
    j->d_theta_tmp = j->weight + 3 * nd;
    j->d_norm_weight = j->weight + 4 * nd;
    j->norm = j->weight + 5 * nd;
    j->W = j->U + nt * nk;
    j->V = j->W + nk;
    j->A = j->V + nd * nk;
    j->AV = j->A + nr * nk;
    for (int i = 0; i < dof; i++) j->weight[i] = j->weight_sqrt[i] = 1.0;
    return true;
}

static void jac_free(Jac* j) {
    free(j->jac);
    free(j->beta);
    free(j->weight);
    free(j->U);
}

#define JAC(j, r, c) (j)->jac[(size_t)(r) * (size_t)(j)->dof + (size_t)(c)]

static void jac_set_betas(Jac* j, int id, D3 v) {
    j->beta[id] = v.x;
    j->beta[id + 1] = v.y;
    j->beta[id + 2] = v.z;
}

static void jac_set_derivatives(Jac* j, int id, int dof_id, D3 v, double norm_weight) {
    double w = j->weight_sqrt[dof_id];
    JAC(j, id, dof_id) = v.x * w;
    JAC(j, id + 1, dof_id) = v.y * w;
    JAC(j, id + 2, dof_id) = v.z * w;
    j->d_norm_weight[dof_id] = norm_weight;
}

// Thin SVD of the tall matrix A (rows x cols, rows >= cols, row-major) by one-sided Jacobi: A = U W V^T, the singular
// values sorted from the largest. U (rows x cols) overwrites A; V is cols x cols.
static void svd_tall(double* A, int rows, int cols, double* W, double* V) {
    for (int i = 0; i < cols; i++)
        for (int k = 0; k < cols; k++) V[i * cols + k] = i == k ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 60; sweep++) {
        double off = 0.0;
        for (int p = 0; p < cols - 1; p++)
            for (int q = p + 1; q < cols; q++) {
                double alpha = 0, beta = 0, gamma = 0;
                for (int i = 0; i < rows; i++) {
                    double ap = A[i * cols + p], aq = A[i * cols + q];
                    alpha += ap * ap;
                    beta += aq * aq;
                    gamma += ap * aq;
                }
                if (gamma == 0.0 || fabs(gamma) <= 1e-12 * sqrt(alpha * beta)) continue;
                off = fmax(off, fabs(gamma) / sqrt(alpha * beta));
                double zeta = (beta - alpha) / (2.0 * gamma);
                double t = (zeta >= 0 ? 1.0 : -1.0) / (fabs(zeta) + sqrt(1.0 + zeta * zeta));
                double c = 1.0 / sqrt(1.0 + t * t), s = c * t;
                for (int i = 0; i < rows; i++) {
                    double ap = A[i * cols + p], aq = A[i * cols + q];
                    A[i * cols + p] = c * ap - s * aq;
                    A[i * cols + q] = s * ap + c * aq;
                }
                for (int i = 0; i < cols; i++) {
                    double vp = V[i * cols + p], vq = V[i * cols + q];
                    V[i * cols + p] = c * vp - s * vq;
                    V[i * cols + q] = s * vp + c * vq;
                }
            }
        if (off < 1e-12) break;
    }
    for (int k = 0; k < cols; k++) {
        double n = 0;
        for (int i = 0; i < rows; i++) n += A[i * cols + k] * A[i * cols + k];
        n = sqrt(n);
        W[k] = n;
        if (n > 0)
            for (int i = 0; i < rows; i++) A[i * cols + k] /= n;
    }
    // sort descending (selection: tiny sizes)
    for (int a = 0; a < cols; a++) {
        int best = a;
        for (int b = a + 1; b < cols; b++)
            if (W[b] > W[best]) best = b;
        if (best == a) continue;
        double t = W[a];
        W[a] = W[best];
        W[best] = t;
        for (int i = 0; i < rows; i++) {
            t = A[i * cols + a];
            A[i * cols + a] = A[i * cols + best];
            A[i * cols + best] = t;
        }
        for (int i = 0; i < cols; i++) {
            t = V[i * cols + a];
            V[i * cols + a] = V[i * cols + best];
            V[i * cols + best] = t;
        }
    }
}

// The SVD of the Jacobian (of its transpose when it is wide), then the selectively damped least squares update.
static void jac_invert(Jac* j) {
    int k = j->k;
    if (!j->transpose) { // J (task x dof) = U W V^T
        for (int r = 0; r < j->task; r++)
            for (int c = 0; c < j->dof; c++) j->A[r * k + c] = JAC(j, r, c);
        svd_tall(j->A, j->task, k, j->W, j->V);
        memcpy(j->U, j->A, sizeof(double) * (size_t)j->task * (size_t)k);
    } else { // J^T (dof x task) = U' W V'^T, so J = V' W U'^T: U = V', V = U'
        for (int r = 0; r < j->dof; r++)
            for (int c = 0; c < j->task; c++) j->A[r * k + c] = JAC(j, c, r);
        svd_tall(j->A, j->dof, k, j->W, j->AV);
        memcpy(j->V, j->A, sizeof(double) * (size_t)j->dof * (size_t)k);
        memcpy(j->U, j->AV, sizeof(double) * (size_t)j->task * (size_t)k);
    }
    // SDLS
    const double epsilon = 1e-10;
    for (int i = 0; i < j->dof; i++) {
        j->d_theta[i] = 0.0;
        j->norm[i] = 0.0;
        for (int t = 0; t + 2 < j->task; t += 3) {
            double n = JAC(j, t, i) * JAC(j, t, i) + JAC(j, t + 1, i) * JAC(j, t + 1, i) + JAC(j, t + 2, i) * JAC(j, t + 2, i);
            j->norm[i] += sqrt(n);
        }
    }
    for (int i = 0; i < k; i++) {
        if (j->W[i] <= epsilon) continue;
        double w_inv = 1.0 / j->W[i], alpha = 0.0, N = 0.0;
        for (int t = 0; t + 2 < j->task; t += 3) {
            double u0 = j->U[t * k + i], u1 = j->U[(t + 1) * k + i], u2 = j->U[(t + 2) * k + i];
            alpha += u0 * j->beta[t] + u1 * j->beta[t + 1] + u2 * j->beta[t + 2];
            N += sqrt(u0 * u0 + u1 * u1 + u2 * u2);
        }
        alpha *= w_inv;
        double M = 0.0, max_dtheta = 0.0;
        for (int d = 0; d < j->dof; d++) {
            double v = j->V[d * k + i];
            M += fabs(v) * j->norm[d];
            j->d_theta_tmp[d] = v * alpha;
            double a = fabs(j->d_theta_tmp[d]) * j->weight_sqrt[d];
            if (a > max_dtheta) max_dtheta = a;
        }
        M *= w_inv;
        double gamma = MAX_ANGLE_CHANGE;
        if (N < M) gamma *= N / M;
        double damp = gamma < max_dtheta ? gamma / max_dtheta : 1.0;
        for (int d = 0; d < j->dof; d++) {
            double dofdamp = damp / j->weight[d];
            if (dofdamp > 1.0) dofdamp = 1.0;
            j->d_theta[d] += 0.80 * dofdamp * j->d_theta_tmp[d];
        }
    }
    double max_angle = 0.0;
    for (int d = 0; d < j->dof; d++) {
        j->d_theta[d] *= j->weight[d];
        double a = fabs(j->d_theta[d]);
        if (a > max_angle) max_angle = a;
    }
    if (max_angle > MAX_ANGLE_CHANGE) {
        double damp = MAX_ANGLE_CHANGE / (MAX_ANGLE_CHANGE + max_angle);
        for (int d = 0; d < j->dof; d++) j->d_theta[d] *= damp;
    }
}

static void jac_lock(Jac* j, int dof_id, double delta) {
    for (int t = 0; t < j->task; t++) {
        j->beta[t] -= JAC(j, t, dof_id) * delta;
        JAC(j, t, dof_id) = 0.0;
    }
    j->norm[dof_id] = 0.0;
    j->d_theta[dof_id] = 0.0;
}

static double jac_update_norm(const Jac* j) {
    double mx = 0.0;
    for (int d = 0; d < j->dof; d++) mx = fmax(mx, fabs(j->d_theta[d] * j->d_norm_weight[d]));
    return mx;
}


// --- Segment updates ---

static D33 rodrigues(D3 dq, bool swing) {
    double theta = d3_len(dq);
    if (fuzzy_zero(theta)) return m3_identity();
    D3 w = d3_scale(dq, 1.0 / theta);
    double sine = sin(theta), cosine = cos(theta), ci = 1 - cosine;
    double xs = w.x * sine, ys = w.y * sine, zs = w.z * sine;
    double xx = w.x * w.x * ci, xy = w.x * w.y * ci, xz = w.x * w.z * ci, yy = w.y * w.y * ci, yz = w.y * w.z * ci,
           zz = w.z * w.z * ci;
    if (swing) return m3_rows(cosine + xx, -zs, xz, zs, cosine, -xs, xz, xs, cosine + zz);
    return m3_rows(cosine + xx, -zs + xy, ys + xz, zs + xy, cosine + yy, -xs + yz, -ys + xz, xs + yz, cosine + zz);
}

// The new angles from the Jacobian; true when a limit clamped them (delta: the clamped change per DOF).
static bool seg_update_angle(Seg* s, const Jac* j, D3* delta, bool* clamp) {
    switch (s->kind) {
    case SEG_SPHERICAL: {
        if (s->locked[0] && s->locked[1] && s->locked[2]) return false;
        D3 dq = d3(j->d_theta[s->dof_id], j->d_theta[s->dof_id + 1], j->d_theta[s->dof_id + 2]);
        s->new_basis = fuzzy_zero(d3_len(dq)) ? s->basis : m3_mul(s->basis, rodrigues(dq, false));
        if (!s->limit_y && !s->limit_x && !s->limit_z) return false;
        D3 a = spherical_range(s->new_basis);
        if (s->locked[0]) a.x = s->locked_ax;
        if (s->locked[1]) a.y = s->locked_ay;
        if (s->locked[2]) a.z = s->locked_az;
        double ax = a.x, ay = a.y, az = a.z;
        clamp[0] = clamp[1] = clamp[2] = false;
        if (s->limit_y) {
            if (a.y > s->max_y) {
                ay = s->max_y;
                clamp[1] = true;
            } else if (a.y < s->min_y) {
                ay = s->min_y;
                clamp[1] = true;
            }
        }
        if (s->limit_x && s->limit_z) {
            if (ellipse_clamp(&ax, &az, s->smin, s->smax)) clamp[0] = clamp[2] = true;
        } else if (s->limit_x) {
            if (ax < s->smin[0]) {
                ax = s->smin[0];
                clamp[0] = true;
            } else if (ax > s->smax[0]) {
                ax = s->smax[0];
                clamp[0] = true;
            }
        } else if (s->limit_z) {
            if (az < s->smin[1]) {
                az = s->smin[1];
                clamp[2] = true;
            } else if (az > s->smax[1]) {
                az = s->smax[1];
                clamp[2] = true;
            }
        }
        if (!clamp[0] && !clamp[1] && !clamp[2]) {
            if (s->locked[0] || s->locked[1] || s->locked[2]) s->new_basis = m3_mul(swing_matrix(ax, az), twist_matrix(ay));
            return false;
        }
        s->new_basis = m3_mul(swing_matrix(ax, az), twist_matrix(ay));
        *delta = matrix_to_axis_angle(m3_mul(m3_t(s->basis), s->new_basis));
        if (!(s->locked[0] || s->locked[2]) && (clamp[0] || clamp[2])) {
            s->locked_ax = ax;
            s->locked_az = az;
        }
        if (!s->locked[1] && clamp[1]) s->locked_ay = ay;
        return true;
    }
    case SEG_SWING: {
        if (s->locked[0] && s->locked[1]) return false;
        D3 dq = d3(j->d_theta[s->dof_id], 0.0, j->d_theta[s->dof_id + 1]);
        s->new_basis = fuzzy_zero(d3_len(dq)) ? s->basis : remove_twist(m3_mul(s->basis, rodrigues(dq, true)));
        if (!s->limit_x && !s->limit_z) return false;
        D3 a = spherical_range(s->new_basis);
        double ax = 0, az = 0;
        clamp[0] = clamp[1] = false;
        if (s->limit_x && s->limit_z) {
            ax = a.x;
            az = a.z;
            if (ellipse_clamp(&ax, &az, s->smin, s->smax)) clamp[0] = clamp[1] = true;
        } else if (s->limit_x) {
            if (ax < s->smin[0]) {
                ax = s->smin[0];
                clamp[0] = true;
            } else if (ax > s->smax[0]) {
                ax = s->smax[0];
                clamp[0] = true;
            }
        } else if (s->limit_z) {
            if (az < s->smin[1]) {
                az = s->smin[1];
                clamp[1] = true;
            } else if (az > s->smax[1]) {
                az = s->smax[1];
                clamp[1] = true;
            }
        }
        if (!clamp[0] && !clamp[1]) return false;
        s->new_basis = swing_matrix(ax, az);
        *delta = matrix_to_axis_angle(m3_mul(m3_t(s->basis), s->new_basis));
        delta->y = delta->z;
        delta->z = 0.0;
        return true;
    }
    case SEG_ELBOW: {
        if (s->locked[0] && s->locked[1]) return false;
        clamp[0] = clamp[1] = false;
        if (!s->locked[0]) {
            s->new_angle = s->angle + j->d_theta[s->dof_id];
            if (s->limit) {
                if (s->new_angle > s->lmax) {
                    delta->x = s->lmax - s->angle;
                    s->new_angle = s->lmax;
                    clamp[0] = true;
                } else if (s->new_angle < s->lmin) {
                    delta->x = s->lmin - s->angle;
                    s->new_angle = s->lmin;
                    clamp[0] = true;
                }
            }
        }
        if (!s->locked[1]) {
            s->new_twist = s->twist + j->d_theta[s->dof_id + 1];
            if (s->limit_twist) {
                if (s->new_twist > s->max_twist) {
                    delta->y = s->max_twist - s->twist;
                    s->new_twist = s->max_twist;
                    clamp[1] = true;
                } else if (s->new_twist < s->min_twist) {
                    delta->y = s->min_twist - s->twist;
                    s->new_twist = s->min_twist;
                    clamp[1] = true;
                }
            }
        }
        return clamp[0] || clamp[1];
    }
    case SEG_REVOLUTE: {
        if (s->locked[0]) return false;
        s->new_angle = s->angle + j->d_theta[s->dof_id];
        clamp[0] = false;
        if (!s->limit) return false;
        if (s->new_angle > s->lmax) delta->x = s->lmax - s->angle;
        else if (s->new_angle < s->lmin) delta->x = s->lmin - s->angle;
        else return false;
        clamp[0] = true;
        s->new_angle = s->angle + delta->x;
        return true;
    }
    case SEG_TRANSLATE: {
        int dof_id = s->dof_id, dof = 0;
        bool clamped = false;
        for (int i = 0; i < 3; i++) {
            if (!s->axis_enabled[i]) {
                d3_set(&s->new_translation, i, d3_get(s->translation, i));
                continue;
            }
            clamp[dof] = false;
            if (!s->locked[dof]) {
                double t = d3_get(s->translation, i), nt = t + j->d_theta[dof_id];
                if (s->tlimit[i]) {
                    if (nt > s->tmax[i]) {
                        d3_set(delta, dof, s->tmax[i] - t);
                        nt = s->tmax[i];
                        clamped = clamp[dof] = true;
                    } else if (nt < s->tmin[i]) {
                        d3_set(delta, dof, s->tmin[i] - t);
                        nt = s->tmin[i];
                        clamped = clamp[dof] = true;
                    }
                }
                d3_set(&s->new_translation, i, nt);
            }
            dof_id++;
            dof++;
        }
        return clamped;
    }
    default: return false;
    }
}

static void seg_lock(Seg* s, int dof, Jac* j, D3 delta) {
    switch (s->kind) {
    case SEG_SPHERICAL:
        if (dof == 1) {
            s->locked[1] = true;
            jac_lock(j, s->dof_id + 1, delta.y);
        } else {
            s->locked[0] = s->locked[2] = true;
            jac_lock(j, s->dof_id, delta.x);
            jac_lock(j, s->dof_id + 2, delta.z);
        }
        break;
    case SEG_SWING:
        s->locked[0] = s->locked[1] = true;
        jac_lock(j, s->dof_id, delta.x);
        jac_lock(j, s->dof_id + 1, delta.y);
        break;
    case SEG_ELBOW:
        if (dof == 0) {
            s->locked[0] = true;
            jac_lock(j, s->dof_id, delta.x);
        } else {
            s->locked[1] = true;
            jac_lock(j, s->dof_id + 1, delta.y);
        }
        break;
    case SEG_REVOLUTE:
        s->locked[0] = true;
        jac_lock(j, s->dof_id, delta.x);
        break;
    case SEG_TRANSLATE:
        s->locked[dof] = true;
        jac_lock(j, s->dof_id + dof, d3_get(delta, dof));
        break;
    default: break;
    }
}

static void seg_apply(Seg* s) {
    switch (s->kind) {
    case SEG_SPHERICAL:
    case SEG_SWING: s->basis = s->new_basis; break;
    case SEG_ELBOW:
        s->angle = s->new_angle;
        s->twist = s->new_twist;
        s->sin_twist = sin(s->twist);
        s->cos_twist = cos(s->twist);
        s->basis = m3_mul(rotation(s->angle, s->axis), rotation_sc(s->sin_twist, s->cos_twist, 1));
        break;
    case SEG_REVOLUTE:
        s->angle = s->new_angle;
        s->basis = rotation(s->angle, s->axis);
        break;
    case SEG_TRANSLATE: s->translation = s->new_translation; break;
    default: break;
    }
}

// --- Tasks ---

typedef struct Task {
    bool position;
    int seg, id;
    double weight;
    D3 goal;
    D33 goal_rot;
    double clamp_length;
} Task;

typedef struct Solver {
    Seg* segs;
    int nseg;
    Task tasks[2 * IK_SOLVER_MAX_GOALS];
    int ntask;
    D33 root_lin;
    D3 root_pos;
    bool pole;
    bool get_angle;
    int pole_tip;
    D3 pole_goal, pole_pos;
    double pole_angle;
} Solver;

static void update_transform(Solver* sv) {
    for (int i = 0; i < sv->nseg; i++) { // parents first
        Seg* s = &sv->segs[i];
        D33 plin = s->parent >= 0 ? sv->segs[s->parent].glin : sv->root_lin;
        D3 ppos = s->parent >= 0 ? sv->segs[s->parent].gpos : sv->root_pos;
        s->gstart = d3_add(ppos, m3_v(plin, s->start));
        s->glin = m3_mul(plin, m3_mul(s->rest, s->basis));
        s->gpos = d3_add(s->gstart, m3_v(s->glin, s->translation));
    }
}

static void task_jacobian(const Solver* sv, const Task* t, Jac* j) {
    const Seg* tip = &sv->segs[t->seg];
    if (t->position) {
        D3 pos = tip->gpos, d_pos = d3_sub(t->goal, pos);
        double length = d3_len(d_pos);
        if (length > t->clamp_length) d_pos = d3_scale(d_pos, t->clamp_length / length);
        jac_set_betas(j, t->id, d3_scale(d_pos, t->weight));
        for (int si = t->seg; si >= 0; si = sv->segs[si].parent) {
            const Seg* s = &sv->segs[si];
            D3 p = d3_sub(s->gstart, pos);
            for (int i = 0; i < s->ndof; i++) {
                D3 axis = d3_scale(seg_axis(s, i), t->weight);
                if (s->translational) jac_set_derivatives(j, t->id, s->dof_id + i, axis, 1e2);
                else jac_set_derivatives(j, t->id, s->dof_id + i, d3_cross(p, axis), 1e0);
            }
        }
    } else {
        D33 d_rotm = m3_t(m3_mul(t->goal_rot, m3_t(tip->glin)));
        D3 d_rot = d3_scale(d3(d_rotm.m[2][1] - d_rotm.m[1][2], d_rotm.m[0][2] - d_rotm.m[2][0], d_rotm.m[1][0] - d_rotm.m[0][1]), -0.5);
        jac_set_betas(j, t->id, d3_scale(d_rot, t->weight));
        for (int si = t->seg; si >= 0; si = sv->segs[si].parent) {
            const Seg* s = &sv->segs[si];
            for (int i = 0; i < s->ndof; i++) {
                if (s->translational) jac_set_derivatives(j, t->id, s->dof_id + i, d3(0, 0, 0), 1e2);
                else jac_set_derivatives(j, t->id, s->dof_id + i, d3_scale(seg_axis(s, i), t->weight), 1e0);
            }
        }
    }
}

// The pole: the chain turned about its root so the root's X/Z axes (by the pole angle) face the pole and its tip
// points at the goal, before solving (Blender solves from there and does not hold the pole afterwards).
static void constrain_pole(Solver* sv) {
    if (!sv->pole) return;
    int position_tasks = 0;
    for (int i = 0; i < sv->ntask; i++) position_tasks += sv->tasks[i].position;
    if (position_tasks >= 2) {
        sv->pole = false;
        return;
    }
    update_transform(sv);
    const Seg* root = &sv->segs[0];
    D3 rootpos = root->gstart, endpos = sv->segs[sv->pole_tip].gpos;
    D33 rootbasis = root->glin;
    D3 dir = d3_norm(d3_sub(endpos, rootpos));
    D3 up = d3_add(d3_scale(m3_col(rootbasis, 0), cos(sv->pole_angle)), d3_scale(m3_col(rootbasis, 2), sin(sv->pole_angle)));
    D3 poledir = sv->get_angle ? dir : d3_norm(d3_sub(sv->pole_goal, rootpos));
    D3 poleup = d3_norm(d3_sub(sv->pole_pos, rootpos));
    D33 mat, polemat;
    m3_set_row(&mat, 0, d3_norm(d3_cross(dir, up)));
    m3_set_row(&mat, 1, dir);
    m3_set_row(&mat, 2, d3_cross(m3_row(mat, 0), m3_row(mat, 1)));
    m3_set_row(&polemat, 0, d3_norm(d3_cross(poledir, poleup)));
    m3_set_row(&polemat, 1, poledir);
    m3_set_row(&polemat, 2, d3_cross(m3_row(polemat, 0), m3_row(polemat, 1)));
    sv->root_lin = m3_mul(m3_t(polemat), mat);
}

static bool update_angles(Solver* sv, Jac* j, double* norm) {
    int minseg = -1, mindof = 0;
    double minabsdelta = 1e10;
    D3 mindelta = d3(0, 0, 0);
    bool locked = false;
    for (int i = 0; i < sv->nseg; i++) {
        Seg* s = &sv->segs[i];
        D3 delta = d3(0, 0, 0);
        bool clamp[3] = { false, false, false };
        if (!seg_update_angle(s, j, &delta, clamp)) continue;
        for (int d = 0; d < s->ndof; d++) {
            if (!clamp[d] || s->locked[d]) continue;
            double absdelta = fabs(d3_get(delta, d));
            if (absdelta < EPS) {
                seg_lock(s, d, j, delta);
                locked = true;
            } else if (absdelta < minabsdelta) {
                minabsdelta = absdelta;
                mindelta = delta;
                minseg = i;
                mindof = d;
            }
        }
    }
    if (minseg >= 0) {
        seg_lock(&sv->segs[minseg], mindof, j, mindelta);
        locked = true;
        if (minabsdelta > *norm) *norm = minabsdelta;
    }
    if (!locked)
        for (int i = 0; i < sv->nseg; i++) {
            Seg* s = &sv->segs[i];
            s->locked[0] = s->locked[1] = s->locked[2] = false;
            seg_apply(s);
        }
    return locked;
}

// A segment for a bone's free axes (rotation or translation); -1 for none.
static void make_segment(Seg* s, unsigned flag, bool translate, bool* made) {
    int ndof = (flag & 1 ? 1 : 0) + (flag & 2 ? 1 : 0) + (flag & 4 ? 1 : 0);
    *made = ndof > 0;
    if (!ndof) return;
    if (ndof == 1) {
        int axis = flag & 1 ? 0 : flag & 2 ? 1 : 2;
        if (translate) {
            seg_init(s, SEG_TRANSLATE, 1, true);
            s->axis_enabled[axis] = true;
            s->axes[0] = axis;
        } else {
            seg_init(s, SEG_REVOLUTE, 1, false);
            s->axis = axis;
        }
    } else if (ndof == 2) {
        int axis1, axis2;
        if (flag & 1) {
            axis1 = 0;
            axis2 = flag & 2 ? 1 : 2;
        } else {
            axis1 = 1;
            axis2 = 2;
        }
        if (translate) {
            seg_init(s, SEG_TRANSLATE, 2, true);
            s->axis_enabled[axis1] = s->axis_enabled[axis2] = true;
            s->axes[0] = axis1;
            s->axes[1] = axis2;
        } else if (axis1 + axis2 == 2) {
            seg_init(s, SEG_SWING, 2, false);
        } else {
            seg_init(s, SEG_ELBOW, 2, false);
            s->axis = axis1 == 0 ? 0 : 2;
        }
    } else if (translate) {
        seg_init(s, SEG_TRANSLATE, 3, true);
        for (int i = 0; i < 3; i++) {
            s->axis_enabled[i] = true;
            s->axes[i] = i;
        }
    } else {
        seg_init(s, SEG_SPHERICAL, 3, false);
    }
}

static D33 from_rows(const double m[3][3]) {
    D33 r;
    memcpy(r.m, m, sizeof r.m);
    return r;
}

static void set_stiffness(Solver* sv, int seg, int axis, double stiffness) {
    if (stiffness < 0.0) return;
    if (stiffness > 1.0 - STIFF_EPS) stiffness = 1.0 - STIFF_EPS;
    Seg* s = &sv->segs[seg];
    if (axis >= 3) {
        if (!s->translational) {
            if (s->composite >= 0 && sv->segs[s->composite].translational) s = &sv->segs[s->composite];
            else return;
        }
        axis -= 3;
    }
    seg_set_weight(s, axis, 1.0 - stiffness);
}

static void set_limit(Solver* sv, int seg, int axis, double lmin, double lmax) {
    Seg* s = &sv->segs[seg];
    if (axis >= 3) {
        if (!s->translational) {
            if (s->composite >= 0 && sv->segs[s->composite].translational) s = &sv->segs[s->composite];
            else return;
        }
        axis -= 3;
    }
    seg_set_limit(s, axis, lmin, lmax);
}

bool ik_solve(IkBone* bones, int n, const IkGoal* goals, int ngoals, int max_iterations) {
    if (n < 1 || n > IK_SOLVER_MAX_BONES || ngoals < 1) return false;
    if (ngoals > IK_SOLVER_MAX_GOALS) ngoals = IK_SOLVER_MAX_GOALS;
    Seg* segs = calloc((size_t)n * 2, sizeof(Seg));
    int* seg_of = malloc(sizeof(int) * (size_t)n);
    if (!segs || !seg_of) {
        free(segs);
        free(seg_of);
        return false;
    }
    Solver sv;
    memset(&sv, 0, sizeof sv);
    sv.segs = segs;
    sv.root_lin = m3_identity();
    for (int a = 0; a < n; a++) {
        const IkBone* b = &bones[a];
        int si = sv.nseg;
        bool rot_made, trans_made;
        make_segment(&segs[sv.nseg], b->dof & 7, false, &rot_made);
        if (rot_made) sv.nseg++;
        int ti = sv.nseg;
        make_segment(&segs[sv.nseg], b->dof & IKS_TRANS_Y ? IKS_Y : 0, true, &trans_made);
        if (trans_made) sv.nseg++;
        if (!rot_made && !trans_made) seg_init(&segs[sv.nseg++], SEG_NULL, 0, false);
        if (rot_made && trans_made) {
            segs[si].composite = ti;
            segs[ti].parent = si;
        }
        seg_of[a] = si;
        int p = -1;
        if (a > 0 && b->parent >= 0 && b->parent < a) {
            p = seg_of[b->parent];
            if (segs[p].composite >= 0) p = segs[p].composite;
        }
        segs[si].parent = p;
        D3 start = d3(b->start[0], b->start[1], b->start[2]);
        if (rot_made && trans_made) {
            seg_set_transform(&segs[si], start, from_rows(b->rest), from_rows(b->basis), 0.0);
            seg_set_transform(&segs[ti], d3(0, 0, 0), m3_identity(), m3_identity(), b->length);
        } else {
            seg_set_transform(&segs[si], start, from_rows(b->rest), from_rows(b->basis), b->length);
        }
        for (int ax = 0; ax < 3; ax++)
            if (b->limit & (1u << ax)) set_limit(&sv, si, ax, b->min[ax], b->max[ax]);
        for (int ax = 0; ax < 3; ax++) set_stiffness(&sv, si, ax, b->stiffness[ax]);
        if (b->dof & IKS_TRANS_Y) {
            set_stiffness(&sv, si, 4, b->stretch_stiffness);
            set_limit(&sv, si, 4, STRETCH_MIN, STRETCH_MAX);
        }
    }
    // Tasks (position and rotation; the pole only once)
    for (int g = 0; g < ngoals; g++) {
        const IkGoal* goal = &goals[g];
        if (goal->tip < 0 || goal->tip >= n) continue;
        int tip = seg_of[goal->tip];
        if (segs[tip].composite >= 0) tip = segs[tip].composite;
        if (goal->use_pos && goal->weight != 0.0) {
            if (goal->use_pole) {
                sv.pole = true;
                sv.pole_tip = tip;
                sv.pole_goal = d3(goal->pos[0], goal->pos[1], goal->pos[2]);
                sv.pole_pos = d3(goal->pole[0], goal->pole[1], goal->pole[2]);
                sv.pole_angle = goal->pole_angle;
            }
            Task* t = &sv.tasks[sv.ntask++];
            t->position = true;
            t->seg = tip;
            t->weight = goal->weight;
            t->goal = d3(goal->pos[0], goal->pos[1], goal->pos[2]);
            int num = 0;
            for (int si = tip; si >= 0; si = segs[si].parent) {
                t->clamp_length += segs[si].max_extension;
                num++;
            }
            t->clamp_length /= 2 * num;
        }
        if (goal->use_rot && goal->orient_weight != 0.0) {
            Task* t = &sv.tasks[sv.ntask++];
            t->position = false;
            t->seg = tip;
            t->weight = goal->orient_weight;
            t->goal_rot = from_rows(goal->rot);
        }
    }
    // Setup: DOF ids, task ids, weights summing to 1
    int num_dof = 0;
    for (int i = 0; i < sv.nseg; i++) {
        segs[i].dof_id = num_dof;
        num_dof += segs[i].ndof;
    }
    int task_size = 0;
    double task_weight = 0.0;
    for (int i = 0; i < sv.ntask; i++) {
        sv.tasks[i].id = task_size;
        task_size += 3;
        task_weight += sv.tasks[i].weight;
    }
    bool ok = num_dof > 0 && task_size > 0;
    Jac j;
    if (ok && !jac_alloc(&j, num_dof, task_size)) {
        jac_free(&j);
        ok = false;
    }
    if (ok) {
        double rescale = task_weight == 0.0 ? 1.0 : 1.0 / task_weight;
        for (int i = 0; i < sv.ntask; i++) sv.tasks[i].weight *= rescale;
        for (int i = 0; i < sv.nseg; i++)
            for (int d = 0; d < segs[i].ndof; d++) {
                j.weight[segs[i].dof_id + d] = segs[i].weight[d];
                j.weight_sqrt[segs[i].dof_id + d] = sqrt(segs[i].weight[d]);
            }
        // Solve, scaled to a chain of length 1
        double length = 0.0;
        for (int i = 0; i < sv.nseg; i++) length += segs[i].max_extension;
        double scale = length == 0.0 ? 1.0 : (double)(float)(1.0 / length);
        for (int i = 0; i < sv.ntask; i++)
            if (sv.tasks[i].position) {
                sv.tasks[i].goal = d3_scale(sv.tasks[i].goal, scale);
                sv.tasks[i].clamp_length *= scale;
            }
        for (int i = 0; i < sv.nseg; i++) seg_scale(&segs[i], scale);
        sv.root_pos = d3_scale(sv.root_pos, scale);
        sv.pole_goal = d3_scale(sv.pole_goal, scale);
        sv.pole_pos = d3_scale(sv.pole_pos, scale);
        constrain_pole(&sv);
        update_transform(&sv);
        int iters = max_iterations > 0 ? max_iterations : 500;
        for (int it = 0; it < iters; it++) {
            update_transform(&sv);
            for (int t = 0; t < sv.ntask; t++) task_jacobian(&sv, &sv.tasks[t], &j);
            double norm = 0.0;
            do {
                jac_invert(&j);
            } while (update_angles(&sv, &j, &norm));
            for (int i = 0; i < sv.nseg; i++) segs[i].locked[0] = segs[i].locked[1] = segs[i].locked[2] = false;
            double maxnorm = jac_update_norm(&j);
            if (maxnorm > norm) norm = maxnorm;
            if (norm < 1e-3 && it > 10) break;
        }
        if (sv.pole) { // the root keeps the pole's turn (PrependBasis)
            Seg* root = &segs[0];
            root->basis = m3_mul(m3_inverse(root->rest), m3_mul(sv.root_lin, m3_mul(root->rest, root->basis)));
        }
        double inv = (double)(1.0f / (float)scale);
        for (int i = 0; i < sv.nseg; i++) seg_scale(&segs[i], inv);
        jac_free(&j);
    }
    // Results (also when there was nothing to solve: Blender still reads the bases back)
    {
        for (int a = 0; a < n; a++) {
            const Seg* s = &segs[seg_of[a]];
            D33 change = m3_mul(m3_t(s->orig_basis), s->basis);
            memcpy(bones[a].basis_change, change.m, sizeof change.m);
            const Seg* ts = s->translational || s->composite < 0 ? s : &segs[s->composite];
            D3 tc = d3_sub(ts->translation, ts->orig_translation);
            bones[a].trans_change[0] = tc.x;
            bones[a].trans_change[1] = tc.y;
            bones[a].trans_change[2] = tc.z;
        }
    }
    free(segs);
    free(seg_of);
    return ok;
}
