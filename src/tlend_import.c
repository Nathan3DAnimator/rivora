// The .tlend importer (tlend_import.h). The file is the Tlender's text format (its fileio.c): blocks in a fixed order,
// optional lines between them. This reads what it needs and skips the rest, so newer files keep importing:
//   "anim"       the frame rate
//   "meshes"     "mesh V F", V "v x y z", F "f n i..." + "t slot u v...", "vgroups", "vg n name" + "w i w..."
//   "actions"    "action ... name", "fc channel count", "fb bone", "k frame value lx ly rx ry interp ..."
//   "materials"  "material name", "tree", "node idname x y ...", "in", "value", "ramp", "image", "link"
//   "armatures"  "armature bones ...", "bone parent head tail roll ... name", "pose ..."
//   "objects"    "o type mesh loc rot scale ... name", "slots", "arm", "parent ... 16 floats", "modifier"
// Blender's space -> the Engine's: meters -> centimeters, Y mirrored (C = diag(100, -100, 100): M' = C M C^-1, and a
// mirrored triangle's winding reversed).
#include "tlend_import.h"

#include "mesh_build.h"
#include "ik_solver.h"
#include "i18n.h"          // (Q19) the report's reasons
#include "import_report.h" // (Q19) the conversion report
#include "driver_expr.h"   // (Q19) drivers
#include "meshoptimizer.h" // (Q19) the Decimate modifier
#include "vkctx.h"         // (Q19) generated images written as PNGs

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "components.h"
#include "content.h"
#include "groom.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "render_gpu.h"
#include "udim.h" // (Q18) UDIM sets by their names
#include "softbody.h" // (Q19) a Soft Body modifier

// (2026-11-10) the file being imported, written into its Skeletal Meshes' "source" (Content Browser > Reimport); rigid:
// the auto rig's parts by bone (on purpose: no "imported before the smooth skins" warning). One import at a time
static char g_import_src[512];
static bool g_import_rigid;
static void sk_stamp(SkelMesh* m) {
    char abs[512];
    snprintf(abs, sizeof abs, "%s", g_import_src); // (the editor's imports come with full paths: a drop, the file dialog)
    for (char* c = abs; *c; c++) if (*c == '\\') *c = '/';
    snprintf(m->source, sizeof m->source, "%s", abs);
    m->rigid = g_import_rigid;
}


// --- 4x4 matrices, Blender's layout: m[col * 4 + row] ---
typedef struct M4 { float m[16]; } M4;
static M4 m4_id(void) {
    M4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}
static M4 m4_mul(const M4* a, const M4* b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) {
            float s = 0.0f;
            for (int j = 0; j < 4; j++) s += a->m[j * 4 + k] * b->m[c * 4 + j];
            r.m[c * 4 + k] = s;
        }
    return r;
}
static void m4_point(const M4* a, const float p[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = a->m[k] * p[0] + a->m[4 + k] * p[1] + a->m[8 + k] * p[2] + a->m[12 + k];
}
static void m4_dir(const M4* a, const float p[3], float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = a->m[k] * p[0] + a->m[4 + k] * p[1] + a->m[8 + k] * p[2];
}
// The inverse of an affine matrix (rotation, scale, shear: the 3x3 by cofactors)
static M4 m4_inv(const M4* a) {
    const float* m = a->m;
    float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
    float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    float id = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    M4 r = m4_id();
    r.m[0] = c00 * id, r.m[4] = c01 * id, r.m[8] = c02 * id;
    r.m[1] = (m[2] * m[9] - m[1] * m[10]) * id, r.m[5] = (m[0] * m[10] - m[2] * m[8]) * id, r.m[9] = (m[1] * m[8] - m[0] * m[9]) * id;
    r.m[2] = (m[1] * m[6] - m[2] * m[5]) * id, r.m[6] = (m[2] * m[4] - m[0] * m[6]) * id, r.m[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    float t[3] = { m[12], m[13], m[14] }, it[3];
    m4_dir(&r, t, it);
    r.m[12] = -it[0], r.m[13] = -it[1], r.m[14] = -it[2];
    return r;
}
static M4 m4_quat(const float q[4]) { // w x y z (Blender's order)
    float w = q[0], x = q[1], y = q[2], z = q[3], l = sqrtf(w * w + x * x + y * y + z * z);
    if (l < 1e-12f) return m4_id();
    w /= l, x /= l, y /= l, z /= l;
    M4 r = m4_id();
    r.m[0] = 1 - 2 * (y * y + z * z), r.m[1] = 2 * (x * y + z * w), r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w), r.m[5] = 1 - 2 * (x * x + z * z), r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w), r.m[9] = 2 * (y * z - x * w), r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}
static M4 m4_trs(const float t[3], const M4* rot, const float s[3]) {
    M4 r = *rot;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++) r.m[c * 4 + k] *= s[c];
    r.m[12] = t[0], r.m[13] = t[1], r.m[14] = t[2];
    return r;
}
static M4 m4_euler_xyz(const float e[3]) { // Blender's XYZ: X first, then Y, then Z
    float cx = cosf(e[0]), sx = sinf(e[0]), cy = cosf(e[1]), sy = sinf(e[1]), cz = cosf(e[2]), sz = sinf(e[2]);
    M4 r = m4_id();
    r.m[0] = cy * cz, r.m[1] = cy * sz, r.m[2] = -sy;
    r.m[4] = sx * sy * cz - cx * sz, r.m[5] = sx * sy * sz + cx * cz, r.m[6] = sx * cy;
    r.m[8] = cx * sy * cz + sx * sz, r.m[9] = cx * sy * sz - sx * cz, r.m[10] = cx * cy;
    return r;
}
// Blender -> the Engine: C M C^-1, C = diag(100, -100, 100)
static M4 m4_to_engine(const M4* a) {
    static const float c[4] = { 100.0f, -100.0f, 100.0f, 1.0f };
    M4 r;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) r.m[col * 4 + row] = c[row] * a->m[col * 4 + row] / c[col];
    return r;
}
static void point_to_engine(const float p[3], float o[3]) { o[0] = p[0] * 100.0f, o[1] = -p[1] * 100.0f, o[2] = p[2] * 100.0f; }

// Blender's bone rest matrix (vec_roll_to_mat3): Y along head -> tail, X and Z by the roll
static M4 bone_rest(const float head[3], const float tail[3], float roll) {
    float v[3] = { tail[0] - head[0], tail[1] - head[1], tail[2] - head[2] }, l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float n[3] = { 0, 1, 0 };
    if (l > 1e-9f) n[0] = v[0] / l, n[1] = v[1] / l, n[2] = v[2] / l;
    float x = n[0], y = n[1], z = n[2], b[3][3] = { { 0 } }; // b[col][row]
    float theta = 1.0f + y, alt = x * x + z * z;
    const float SAFE = 6.1e-3f, CRIT = 2.5e-4f;
    if (theta > SAFE || alt > CRIT * CRIT) {
        if (theta <= SAFE) theta = alt * 0.5f + alt * alt * 0.125f;
        b[0][1] = -x, b[1][0] = x, b[1][1] = y, b[1][2] = z, b[2][1] = -z;
        if (theta > SAFE) {
            b[0][0] = 1 - x * x / theta, b[2][2] = 1 - z * z / theta, b[2][0] = b[0][2] = -x * z / theta;
        } else {
            b[0][0] = (x + z) * (x - z) / -alt, b[2][2] = -b[0][0], b[2][0] = b[0][2] = 2.0f * x * z / alt;
        }
    } else {
        b[0][0] = -1, b[1][1] = -1, b[2][2] = 1;
    }
    // the roll about n (axis-angle, column-major)
    float c = cosf(roll), s = sinf(roll), t = 1 - c, r[3][3];
    r[0][0] = t * x * x + c, r[0][1] = t * x * y + s * z, r[0][2] = t * x * z - s * y;
    r[1][0] = t * x * y - s * z, r[1][1] = t * y * y + c, r[1][2] = t * y * z + s * x;
    r[2][0] = t * x * z + s * y, r[2][1] = t * y * z - s * x, r[2][2] = t * z * z + c;
    M4 m = m4_id();
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) m.m[col * 4 + row] = r[0][row] * b[col][0] + r[1][row] * b[col][1] + r[2][row] * b[col][2];
    m.m[12] = head[0], m.m[13] = head[1], m.m[14] = head[2];
    return m;
}

// A transform's location, rotation (as axes) and scale
static void m4_split(const M4* a, float t[3], float axes[3][3], float s[3]) {
    for (int c = 0; c < 3; c++) {
        float l = sqrtf(a->m[c * 4] * a->m[c * 4] + a->m[c * 4 + 1] * a->m[c * 4 + 1] + a->m[c * 4 + 2] * a->m[c * 4 + 2]);
        s[c] = l;
        for (int k = 0; k < 3; k++) axes[c][k] = l > 1e-12f ? a->m[c * 4 + k] / l : (c == k ? 1.0f : 0.0f);
        t[c] = a->m[12 + c];
    }
}
static void axes_quat_xyzw(const float a[3][3], float q[4]) { // axes (columns) -> x y z w
    float m00 = a[0][0], m11 = a[1][1], m22 = a[2][2], tr = m00 + m11 + m22;
    float m01 = a[1][0], m10 = a[0][1], m02 = a[2][0], m20 = a[0][2], m12 = a[2][1], m21 = a[1][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; k++) q[k] /= l > 1e-12f ? l : 1.0f;
}

// --- The file's data ---

typedef struct TMesh {
    float* co;                // 3 per vertex
    uint32_t nv, nf;
    uint32_t* fstart;         // per face: its first corner
    uint32_t* fsize;
    uint32_t* loops;          // corners: vertex indices
    float* uv;                // 2 per corner
    uint32_t* col;            // the vertex colors, RGBA8 (sRGB) per corner (glTF's COLOR_0; NULL: none)
    uint16_t* fmat;           // the face's slot
    uint32_t ncorners;
    uint32_t ngroups;
    char (*gname)[64];
    float* gweight;           // ngroups x nv
    // shape keys (Relative, the Basis left out) or glTF's morph targets: the morph targets of a Skeletal Mesh
    uint32_t nkeys;
    char (*kname)[RMESH_MORPH_NAME];
    float* kvalue;            // their values (the default weights)
    float* kdelta;            // nv x nkeys x 3: each key's offset from the Basis (Blender's space; vertex-major)
    float (*krange)[2];       // (Q19) each key's slider range (a driven value is held in it, as the Tlender does)
    // (2026-11-19) the edges the Tlender marked (vertex pairs): creases (the Subdivision's, as sharpness: crease^2 * 10,
    // OpenSubdiv's and the Tlender's; 10 = infinitely sharp) and sharp edges (the normals split there). Kept through
    // the Subdivision (its children edges); a modifier that renumbers the vertices otherwise drops them
    uint32_t ncrease, nsharp;
    uint32_t (*crease_e)[2], (*sharp_e)[2];
    float* crease_s;
} TMesh;
static void tmesh_free_keys(TMesh* t) {
    free(t->kname), free(t->kvalue), free(t->kdelta), free(t->krange);
    t->kname = NULL, t->kvalue = NULL, t->kdelta = NULL, t->krange = NULL, t->nkeys = 0;
}
static void tmesh_free_marks(TMesh* t) {
    free(t->crease_e), free(t->crease_s), free(t->sharp_e);
    t->crease_e = NULL, t->crease_s = NULL, t->sharp_e = NULL, t->ncrease = t->nsharp = 0;
}
// "a b" pairs (and "a b value" triples when s) from a line into a growing list; returns how many
static uint32_t parse_edge_marks(const char* p, uint32_t nv, uint32_t (**e)[2], float** s) {
    uint32_t n = 0, cap = 0;
    for (;;) {
        char* q;
        unsigned long a = strtoul(p, &q, 10);
        if (q == p) break;
        p = q;
        unsigned long b = strtoul(p, &q, 10);
        if (q == p) break;
        p = q;
        float v = 1.0f;
        if (s) {
            v = strtof(p, &q);
            if (q == p) break;
            p = q;
        }
        if (a >= nv || b >= nv || a == b || v <= 0.0f) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            void* ne = realloc(*e, sizeof **e * cap);
            if (!ne) break;
            *e = ne;
            if (s) {
                float* ns = realloc(*s, sizeof(float) * cap);
                if (!ns) break;
                *s = ns;
            }
        }
        (*e)[n][0] = (uint32_t)a, (*e)[n][1] = (uint32_t)b;
        if (s) (*s)[n] = fminf(v * v * 10.0f, 10.0f); // (the crease as sharpness)
        n++;
    }
    return n;
}

typedef struct TNode {
    char idname[64];
    float x, y;
    int prop[4], nin;
    float in[32][4], value[4];
    int ramp_count;
    float ramp[16][5];
    char image[512], label[64];
    char attr[64]; // (Q19) the Attribute node's name ("attrname")
    // (Q19) outputs the Tlender baked into images on its GPU ("bake <output> <color space: 0 sRGB, 1 Non-Color> <file>"):
    // the node becomes those Image Textures (a node the Engine lacks, or one that needs the mesh: AO, Bevel...)
    int nbake, bake_sock[4], bake_cs[4];
    char bake_path[4][260];
} TNode;
typedef struct TMat {
    char name[64];
    TNode* nodes;
    int nnodes, nlinks;
    int links[256][4];
} TMat;

// Bone constraints (the Tlender's armature.h ConstraintType and ConSpace)
enum { TC_IK = 1, TC_COPY_LOC, TC_COPY_ROT, TC_COPY_SCALE, TC_COPY_XF, TC_DAMPED, TC_LIMIT_LOC, TC_LIMIT_ROT, TC_LIMIT_SCALE,
       TC_STRETCH, TC_CHILD_OF, TC_ARMATURE, TC_XFORM, TC_TRACK_TO, TC_LOCKED_TRACK, TC_LIMIT_DIST, TC_MAINTAIN_VOL, TC_ACTION,
       TC_SPLINE_IK };
enum { SP_WORLD = 0, SP_POSE, SP_LOCAL_PARENT, SP_LOCAL };
#define RIG_IK_MAX 64   // bones in a chain (the Tlender's IK_MAX)
#define RIG_LIST_MAX 8  // Spline IK controls (TL_CON_LIST_MAX)
#define RIG_CON_MAX 64  // constraints per bone, for the solvers' ids
typedef struct TCon {
    int type, enabled, chain, iterations, use_tail, stretch, axis[3], invert[3], offset, track;
    float influence, head_tail, pole_angle, min[3], max[3], volume;
    char target[64], pole[64], target_obj[64], name[64]; // (name: "cn", Q19: the drivers find it by it)
    int owner_space, target_space;
    unsigned channels;      // Child Of (cxc)
    float inverse[16];
    int from, to, map[3], extrapolate, xmix; // Transformation (cxx)
    float from_min[3], from_max[3], to_min[3], to_max[3];
    int euler_order, mix, uniform, scale_add, wrap_limit, remove_shear, stretch_keep, volume_mode; // Blender's options (cxb)
    int bulge_min_on, bulge_max_on;
    float power, rest_length, bulge_min, bulge_max, bulge_smooth;
    int up, target_z, dist_mode, vol_axis, vol_mode; // (Q19) Track To / Locked Track (cxx), Limit Distance (cxl), Maintain Volume (cxv)
    float dist;
    int ik_solver, ik_use_loc, ik_use_rot; // IK (cik): 0 Blender's Standard IK, 1 the FABRIK (files before it)
    float ik_weight, ik_orient_weight;
    int list_y, list_xz, list_even, list_ctrl_rot, nlist; // Spline IK (cxs, cxsn): the control bones
    char list_names[RIG_LIST_MAX][64];
    int tarm, tbone, tpole, list[RIG_LIST_MAX]; // resolved: the target's armature, bone, the pole's bone (-1: none)
} TCon;

typedef struct TBone {
    char name[64];
    int parent, connected;
    int relative_parent;                // (boneinh) Blender's Relative Parenting: a child object follows the bone's deformation
    float head[3], tail[3], roll;
    float ploc[3], pquat[4], pscale[3]; // the file's pose channels (what an action doesn't key)
    int ik_lock, ik_limit;              // the Standard IK's settings of the bone (boneik)
    float ik_min[3], ik_max[3], ik_stiff[3], ik_stretch;
    TCon* cons;
    int ncons;
} TBone;
typedef struct TArm {
    TBone* bones;
    int nbones;
    // the rig's evaluation (rig_prepare): rest matrices, offsets from the parents, lengths, this frame's pose
    // (armature space) and channel rotations, the object and its world matrix, the baked action's curves per bone, the
    // units (a bone alone, or the bones of IK / Spline IK chains that share bones) in their order: their members
    // parents first, their solvers (bone * RIG_CON_MAX + constraint) shallow chains first
    M4 *rest, *rel, *pose, world;
    float *length, (*cquat)[4];
    int obj, *curve_bone;
    int nunits, *uorder, *mem, *mem_start, *sol, *sol_start;
} TArm;

typedef struct TKey { float f, v, lx, ly, rx, ry; int interp; } TKey;
typedef struct TCurve { int channel; char bone[64]; TKey* keys; int nkeys; } TCurve;
// mcurves: the morph targets' weights it moves (the .rasset's MWGT: bone holds the morph's name, channel 0)
typedef struct TAction { char name[64]; TCurve* curves; int ncurves; TCurve* mcurves; int nmcurves; } TAction;

typedef struct TObj {
    int type, mesh, arm, parent, deform, action; // deform: the armature object of its Armature modifier (-1: none); action: its own
    int parent_type;       // 0 object, 1 bone (Blender's Bone parent: parent_bone of the armature object `parent`)
    char parent_bone[64];
    bool hidden;           // not rendered: only in collections excluded or off in render, or a bone's custom shape (a rig widget)
    float loc[3], rot[3], scale[3];
    float parent_inv[16];
    int nslots, slots[32];
    char name[64];
    M4 world;
    bool world_done;
    // (Q19) its modifier stack ("mods" line: its index; 0 none), what it became (sim 1 Cloth, 2 Soft Body; Collision), the
    // hard edges' angle (Edge Split: its cosine), the light / camera on the "o" line and the "light" line
    size_t mods_at;
    int sim, collide;
    float split_cos;
    float color[3], lens, light_power, light_color[3], light_radius;
    int light_type, light_shape;
    float spot_size, spot_blend, light_angle, light_size, light_size_y;
} TObj;

typedef struct TFile {
    float fps;
    TMesh* meshes;
    int nmeshes;
    TMat* mats;
    int nmats;
    TArm* arms;
    int narms;
    TAction* actions;
    int nactions;
    TObj* objs;
    int nobjs;
    char** lines;
    size_t nlines;
    // (2026-11-16) an OBJ's polylines ("l"): hair guides (engine space, cm)
    float* curve_pts;
    uint32_t* curve_cnt;
    uint32_t ncurves, ncurve_pts;
    struct TDrivers* drv; // (Q19) the rig block's drivers and custom properties (import_drivers.inc)
    int nhidden;          // (#164) meshes left out: hidden in render or a bone's custom shape (mark_hidden)
} TFile;

static bool starts(const char* l, const char* tag) {
    size_t n = strlen(tag);
    return !strncmp(l, tag, n) && (l[n] == ' ' || l[n] == 0);
}

static void read_lines(TFile* f, char* text) {
    size_t cap = 1024;
    f->lines = malloc(sizeof(char*) * cap);
    for (char* p = text; p && *p && f->lines;) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r') p[l - 1] = 0;
        if (f->nlines == cap) {
            char** nl = realloc(f->lines, sizeof(char*) * cap * 2);
            if (!nl) break;
            f->lines = nl, cap *= 2;
        }
        f->lines[f->nlines++] = p;
        p = e ? e + 1 : NULL;
    }
}

static size_t find(const TFile* f, size_t from, const char* tag) {
    for (size_t i = from; i < f->nlines; i++)
        if (starts(f->lines[i], tag)) return i;
    return f->nlines;
}

// A mesh's shape keys (the Tlender's v33 "shapekeys"; fileio.c): "shapekeys count active relative ...", per key "sk value
// min max relative mute lock interp pos n name", "skg group", "o" and n (vertex, offset from the mesh) quads. Each key but
// the Basis is a morph target: its offset less its relative key's, times its vertex group (Relative mode; Absolute's
// interpolated keys have no target)
static void parse_shapekeys(TFile* f, size_t from, size_t next, TMesh* t) {
    size_t at = find(f, from, "shapekeys");
    if (at >= next || !t->nv) return;
    int nk = 0, active = 0, rel = 1;
    if (sscanf(f->lines[at] + 10, "%d %d %d", &nk, &active, &rel) < 3 || nk < 2 || nk > 4096 || !rel) return;
    float* off = calloc((size_t)nk * t->nv * 3, sizeof(float)); // (each key's offsets, key-major)
    int* krel = calloc((size_t)nk, sizeof(int));
    float* kval = calloc((size_t)nk, sizeof(float));
    float(*krg)[2] = calloc((size_t)nk, sizeof *krg); // (Q19) the sliders' ranges
    char(*kn)[RMESH_MORPH_NAME] = calloc((size_t)nk, RMESH_MORPH_NAME);
    char(*kg)[64] = calloc((size_t)nk, 64);
    bool ok = off && krel && kval && kn && kg;
    size_t j = at + 1;
    for (int k = 0; ok && k < nk; k++) {
        if (j + 2 >= next || !starts(f->lines[j], "sk")) {
            ok = false;
            break;
        }
        float val = 0, lo = 0, hi = 1, pos = 0;
        int r = 0, mute = 0, lk = 0, interp = 0, used = 0;
        unsigned n = 0;
        if (sscanf(f->lines[j], "sk %f %f %f %d %d %d %d %f %u %n", &val, &lo, &hi, &r, &mute, &lk, &interp, &pos, &n, &used) < 9) {
            ok = false;
            break;
        }
        snprintf(kn[k], RMESH_MORPH_NAME, "%s", used ? f->lines[j] + used : "");
        // the value as the Tlender evaluates it (shapekey.c, Blender's): held inside its slider's range. The Delight's
        // "Hide" was saved at -1 with a range of 0..1 (0 there): taken raw it pushed the dress out over her body
        if (hi < lo) hi = lo;
        kval[k] = mute ? 0.0f : fminf(fmaxf(val, lo), hi), krel[k] = r >= 0 && r < nk && r != k ? r : 0;
        if (krg) krg[k][0] = mute ? 0.0f : lo, krg[k][1] = mute ? 0.0f : hi;
        if (starts(f->lines[j + 1], "skg")) snprintf(kg[k], 64, "%s", f->lines[j + 1][3] == ' ' ? f->lines[j + 1] + 4 : "");
        const char* p = f->lines[j + 2] + 1; // "o v dx dy dz ..."
        for (unsigned e = 0; e < n; e++) {
            char* end;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p) break;
            p = end;
            float d[3];
            for (int c = 0; c < 3; c++) d[c] = strtof(p, &end), p = end;
            if (v < t->nv) memcpy(&off[((size_t)k * t->nv + v) * 3], d, 12);
        }
        j += 3;
    }
    uint32_t nt = 0;
    for (int k = 1; ok && k < nk && nt < RMESH_MAX_MORPHS; k++) nt++;
    if (ok && nt) {
        t->kname = calloc(nt, RMESH_MORPH_NAME), t->kvalue = calloc(nt, sizeof(float));
        t->kdelta = calloc((size_t)t->nv * nt * 3, sizeof(float));
        t->krange = calloc(nt, sizeof *t->krange);
        ok = t->kname && t->kvalue && t->kdelta && t->krange && krg;
    }
    for (uint32_t q = 0; ok && q < nt; q++) {
        int k = (int)q + 1, r = krel[k];
        const float* gw = NULL; // (its vertex group: the weights)
        for (uint32_t g = 0; kg[k][0] && g < t->ngroups && t->gweight && !gw; g++)
            if (!strcmp(t->gname[g], kg[k])) gw = &t->gweight[(size_t)g * t->nv];
        snprintf(t->kname[q], RMESH_MORPH_NAME, "%s", kn[k][0] ? kn[k] : "Key");
        t->kvalue[q] = kval[k];
        t->krange[q][0] = krg[k][0], t->krange[q][1] = krg[k][1];
        for (uint32_t v = 0; v < t->nv; v++)
            for (int c = 0; c < 3; c++)
                t->kdelta[((size_t)v * nt + q) * 3 + c] = (off[((size_t)k * t->nv + v) * 3 + c] - off[((size_t)r * t->nv + v) * 3 + c]) * (gw ? gw[v] : 1.0f);
    }
    if (ok && nt) t->nkeys = nt;
    else tmesh_free_keys(t);
    free(off), free(krel), free(kval), free(kn), free(kg), free(krg);
}

static size_t parse_meshes(TFile* f, size_t i, int n) {
    f->meshes = calloc((size_t)(n > 0 ? n : 1), sizeof(TMesh));
    f->nmeshes = 0;
    for (int m = 0; m < n && f->meshes; m++) {
        i = find(f, i, "mesh");
        if (i >= f->nlines) break;
        TMesh* t = &f->meshes[f->nmeshes++];
        sscanf(f->lines[i] + 5, "%u %u", &t->nv, &t->nf);
        i++;
        t->co = calloc((size_t)t->nv * 3 + 3, sizeof(float));
        t->fstart = calloc((size_t)t->nf + 1, sizeof(uint32_t));
        t->fsize = calloc((size_t)t->nf + 1, sizeof(uint32_t));
        t->fmat = calloc((size_t)t->nf + 1, sizeof(uint16_t));
        size_t ccap = (size_t)t->nf * 4 + 16;
        t->loops = malloc(sizeof(uint32_t) * ccap);
        t->uv = calloc(ccap * 2, sizeof(float));
        if (!t->co || !t->fstart || !t->fsize || !t->fmat || !t->loops || !t->uv) return f->nlines;
        for (uint32_t v = 0; v < t->nv && i < f->nlines; v++, i++) sscanf(f->lines[i] + 2, "%f %f %f", &t->co[v * 3], &t->co[v * 3 + 1], &t->co[v * 3 + 2]);
        for (uint32_t k = 0; k < t->nf && i < f->nlines; k++) {
            const char* l = f->lines[i];
            if (!starts(l, "f")) break;
            char* p = (char*)l + 2;
            uint32_t cnt = (uint32_t)strtoul(p, &p, 10);
            if (t->ncorners + cnt > ccap) {
                ccap = (t->ncorners + cnt) * 2;
                uint32_t* nl = realloc(t->loops, sizeof(uint32_t) * ccap);
                float* nu = realloc(t->uv, sizeof(float) * ccap * 2);
                if (!nl || !nu) return f->nlines;
                t->loops = nl, t->uv = nu;
            }
            t->fstart[k] = t->ncorners, t->fsize[k] = cnt;
            for (uint32_t c = 0; c < cnt; c++) {
                uint32_t vi = (uint32_t)strtoul(p, &p, 10);
                t->loops[t->ncorners + c] = vi < t->nv ? vi : 0;
            }
            i++;
            if (i < f->nlines && starts(f->lines[i], "t")) {
                p = f->lines[i] + 2;
                t->fmat[k] = (uint16_t)strtoul(p, &p, 10);
                for (uint32_t c = 0; c < cnt; c++) {
                    t->uv[(t->ncorners + c) * 2] = strtof(p, &p);
                    t->uv[(t->ncorners + c) * 2 + 1] = strtof(p, &p);
                }
                i++;
            }
            t->ncorners += cnt;
        }
        // its vertex groups (until the next mesh or the next block)
        // (the next mesh, or whichever block comes next)
        size_t next = find(f, i, "mesh");
        static const char* const blocks[] = { "materials", "actions", "armatures", "objects", "world", "curves", "evalmesh", "evalmeshes", "end" };
        for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
            size_t at = find(f, i, blocks[b]);
            if (at < next) next = at;
        }
        size_t vg = find(f, i, "vgroups");
        if (vg < next) {
            int ng = 0, act = 0;
            sscanf(f->lines[vg] + 8, "%d %d", &ng, &act);
            if (ng > 0 && ng < 4096) {
                t->ngroups = (uint32_t)ng;
                t->gname = calloc((size_t)ng, sizeof *t->gname);
                t->gweight = calloc((size_t)ng * t->nv + 1, sizeof(float));
                size_t j = vg + 1;
                for (int g = 0; g < ng && j + 1 < f->nlines && t->gname && t->gweight; g++) {
                    if (!starts(f->lines[j], "vg")) break;
                    char* p = f->lines[j] + 3;
                    strtoul(p, &p, 10);
                    while (*p == ' ') p++;
                    snprintf(t->gname[g], 64, "%s", p);
                    p = f->lines[j + 1] + 1; // "w i w i w..."
                    while (*p) {
                        char* e;
                        unsigned long vi = strtoul(p, &e, 10);
                        if (e == p) break;
                        p = e;
                        float wv = strtof(p, &e);
                        if (e == p) break;
                        p = e;
                        if (vi < t->nv) t->gweight[(size_t)g * t->nv + vi] = wv;
                    }
                    j += 2;
                }
            }
        }
        parse_shapekeys(f, i, next, t);
        // (2026-11-19) the sharp edges ("shade" ... then "h a b a b...", v18) and the creases ("ecrease N" + "a b value"
        // triples, v29): the Subdivision's corners and the normals' hard edges, as the Tlender has them
        size_t sh = find(f, i, "shade");
        for (size_t j = sh + 1; sh < next && j < next && j <= sh + 2; j++)
            if (f->lines[j][0] == 'h' && (f->lines[j][1] == ' ' || !f->lines[j][1])) {
                t->nsharp = parse_edge_marks(f->lines[j] + 1, t->nv, &t->sharp_e, NULL);
                break;
            }
        size_t ec = find(f, i, "ecrease");
        if (ec < next && ec + 1 < f->nlines) t->ncrease = parse_edge_marks(f->lines[ec + 1], t->nv, &t->crease_e, &t->crease_s);
        i = next < f->nlines ? next : i;
    }
    return i;
}

static size_t parse_materials(TFile* f, size_t i, int n) {
    f->mats = calloc((size_t)(n > 0 ? n : 1), sizeof(TMat));
    for (int m = 0; m < n && f->mats; m++) {
        i = find(f, i, "material");
        if (i >= f->nlines) break;
        TMat* t = &f->mats[f->nmats++];
        snprintf(t->name, sizeof t->name, "%s", f->lines[i][8] ? f->lines[i] + 9 : "Material");
        i++;
        int nn = 0, nl = 0;
        if (i < f->nlines && starts(f->lines[i], "tree")) sscanf(f->lines[i] + 5, "%d %d", &nn, &nl), i++;
        if (nn < 0 || nn > 512) nn = 0;
        t->nodes = calloc((size_t)(nn ? nn : 1), sizeof(TNode));
        if (!t->nodes) return f->nlines;
        while (i < f->nlines && t->nnodes < nn) {
            const char* l = f->lines[i];
            if (starts(l, "node")) {
                TNode* d = &t->nodes[t->nnodes++];
                float v[9] = { 0 };
                char id[64] = "";
                int cnt = sscanf(l + 5, "%63s %f %f %f %f %f %f %f %f", id, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
                snprintf(d->idname, sizeof d->idname, "%s", id);
                d->x = v[0], d->y = v[1];
                // newer files: panels, 4 props, nin; older: 4 props, nin
                if (cnt >= 9) d->prop[0] = (int)v[3], d->prop[1] = (int)v[4], d->prop[2] = (int)v[5], d->prop[3] = (int)v[6], d->nin = (int)v[7];
                else if (cnt >= 8) d->prop[0] = (int)v[2], d->prop[1] = (int)v[3], d->prop[2] = (int)v[4], d->prop[3] = (int)v[5], d->nin = (int)v[6];
                i++;
                int k = 0;
                while (i < f->nlines && starts(f->lines[i], "in")) {
                    if (k < 32) sscanf(f->lines[i] + 3, "%f %f %f %f", &d->in[k][0], &d->in[k][1], &d->in[k][2], &d->in[k][3]);
                    k++, i++;
                }
                // its other lines until the next node or link
                while (i < f->nlines && !starts(f->lines[i], "node") && !starts(f->lines[i], "link") && !starts(f->lines[i], "material") &&
                       !starts(f->lines[i], "world") && !starts(f->lines[i], "armatures")) {
                    const char* q = f->lines[i];
                    if (starts(q, "value")) sscanf(q + 6, "%f %f %f %f", &d->value[0], &d->value[1], &d->value[2], &d->value[3]);
                    else if (starts(q, "ramp")) {
                        char* p = (char*)q + 5;
                        int rc = (int)strtol(p, &p, 10);
                        for (int r = 0; r < rc && r < 16; r++)
                            for (int c = 0; c < 5; c++) d->ramp[r][c] = strtof(p, &p);
                        d->ramp_count = rc < 16 ? rc : 16;
                    } else if (starts(q, "image")) snprintf(d->image, sizeof d->image, "%s", q[5] ? q + 6 : "");
                    else if (starts(q, "label")) snprintf(d->label, sizeof d->label, "%s", q[5] ? q + 6 : "");
                    else if (starts(q, "attrname")) snprintf(d->attr, sizeof d->attr, "%s", q[8] ? q + 9 : "");
                    else if (starts(q, "bake") && d->nbake < 4) { // (Q19) "bake <output> <color space> <file>"
                        int so = 0, cs = 0, used = 0;
                        if (sscanf(q + 5, "%d %d %n", &so, &cs, &used) >= 2 && used) {
                            d->bake_sock[d->nbake] = so, d->bake_cs[d->nbake] = cs;
                            snprintf(d->bake_path[d->nbake], sizeof d->bake_path[0], "%s", q + 5 + used);
                            d->nbake++;
                        }
                    }
                    i++;
                }
                continue;
            }
            if (starts(l, "link") || starts(l, "material")) break;
            i++;
        }
        while (i < f->nlines && starts(f->lines[i], "link") && t->nlinks < 256) {
            int* lk = t->links[t->nlinks];
            if (sscanf(f->lines[i] + 5, "%d %d %d %d", &lk[0], &lk[1], &lk[2], &lk[3]) == 4) t->nlinks++;
            i++;
        }
    }
    return i;
}

static size_t parse_armatures(TFile* f, size_t i, int n) {
    f->arms = calloc((size_t)(n > 0 ? n : 1), sizeof(TArm));
    for (int a = 0; a < n && f->arms; a++) {
        i = find(f, i, "armature");
        if (i >= f->nlines) break;
        int nb = 0;
        sscanf(f->lines[i] + 9, "%d", &nb);
        TArm* t = &f->arms[f->narms++];
        t->bones = calloc((size_t)(nb > 0 ? nb : 1), sizeof(TBone));
        i++;
        TBone* b = NULL;
        while (i < f->nlines && t->bones) {
            const char* l = f->lines[i];
            if (starts(l, "armature") || starts(l, "objects") || starts(l, "curves")) break;
            if (starts(l, "bone") && t->nbones < nb) {
                b = &t->bones[t->nbones++];
                char* p = (char*)l + 5;
                b->parent = (int)strtol(p, &p, 10);
                for (int k = 0; k < 3; k++) b->head[k] = strtof(p, &p);
                for (int k = 0; k < 3; k++) b->tail[k] = strtof(p, &p);
                b->roll = strtof(p, &p);
                b->connected = (int)strtol(p, &p, 10);
                for (int k = 0; k < 5; k++) strtol(p, &p, 10);
                for (int k = 0; k < 4; k++) strtof(p, &p);
                while (*p == ' ') p++;
                snprintf(b->name, sizeof b->name, "%s", p);
                if (b->parent >= t->nbones - 1) b->parent = -1;
                b->pquat[0] = 1.0f, b->pscale[0] = b->pscale[1] = b->pscale[2] = 1.0f;
            } else if (b && starts(l, "pose")) {
                sscanf(l + 5, "%f %f %f %f %f %f %f %f %f %f", &b->ploc[0], &b->ploc[1], &b->ploc[2], &b->pquat[0], &b->pquat[1],
                       &b->pquat[2], &b->pquat[3], &b->pscale[0], &b->pscale[1], &b->pscale[2]);
            } else if (b && starts(l, "con")) { // a bone constraint (the Tlender's fileio.c save_con)
                TCon* nc = realloc(b->cons, sizeof(TCon) * (size_t)(b->ncons + 1));
                if (nc) {
                    b->cons = nc;
                    TCon* c = &b->cons[b->ncons];
                    memset(c, 0, sizeof *c);
                    if (sscanf(l + 4, "%d %d %f %f %f %d %d %d %d %d %d %d %d %d %d %d %f %f %f %f %f %f %d %f", &c->type, &c->enabled,
                               &c->influence, &c->head_tail, &c->pole_angle, &c->chain, &c->iterations, &c->use_tail, &c->stretch,
                               &c->axis[0], &c->axis[1], &c->axis[2], &c->invert[0], &c->invert[1], &c->invert[2], &c->offset, &c->min[0],
                               &c->min[1], &c->min[2], &c->max[0], &c->max[1], &c->max[2], &c->track, &c->volume) == 24) {
                        bool limit = c->type == TC_LIMIT_LOC || c->type == TC_LIMIT_ROT || c->type == TC_LIMIT_SCALE;
                        c->owner_space = limit ? SP_LOCAL : SP_POSE; // (before the "rig" block's spaces)
                        c->target_space = SP_POSE;
                        c->channels = 0x1ff;
                        memcpy(c->inverse, m4_id().m, sizeof c->inverse);
                        c->power = 1.0f;
                        c->map[0] = 0, c->map[1] = 1, c->map[2] = 2;
                        c->tarm = c->tbone = c->tpole = -1;
                        c->ik_solver = 1, c->ik_use_loc = 1, c->ik_weight = c->ik_orient_weight = 1.0f; // (no "cik": FABRIK)
                        c->list_y = 1;
                        b->ncons++;
                    }
                }
            } else if (b && b->ncons && starts(l, "cn")) { // (Q19) the constraint's name
                snprintf(b->cons[b->ncons - 1].name, sizeof b->cons[0].name, "%s", l[2] ? l + 3 : "");
            } else if (b && b->ncons && (starts(l, "ct") || starts(l, "cp"))) {
                TCon* c = &b->cons[b->ncons - 1];
                snprintf(l[1] == 't' ? c->target : c->pole, sizeof c->target, "%s", l[2] ? l + 3 : "");
            } else if (b && b->ncons && starts(l, "cik")) {
                TCon* c = &b->cons[b->ncons - 1];
                int solver = 0, loc = 1, rot = 0;
                if (sscanf(l + 4, "%d %d %d %f %f", &solver, &loc, &rot, &c->ik_weight, &c->ik_orient_weight) == 5)
                    c->ik_solver = solver == 1, c->ik_use_loc = loc != 0, c->ik_use_rot = rot != 0;
            } else if (b && starts(l, "boneinh")) { // "<inherit scale> <hinge> <no local location> <relative parent> <rot order>"
                int v[4] = { 0 };
                if (sscanf(l + 8, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4) b->relative_parent = v[3] != 0;
            } else if (b && starts(l, "boneik")) {
                float v[10] = { 0 };
                if (sscanf(l + 7, "%d %d %f %f %f %f %f %f %f %f %f %f", &b->ik_lock, &b->ik_limit, &v[0], &v[1], &v[2], &v[3], &v[4],
                           &v[5], &v[6], &v[7], &v[8], &v[9]) == 12) {
                    b->ik_lock &= 7, b->ik_limit &= 7;
                    for (int k = 0; k < 3; k++) b->ik_min[k] = v[k * 3], b->ik_max[k] = v[k * 3 + 1], b->ik_stiff[k] = v[k * 3 + 2];
                    b->ik_stretch = v[9];
                }
            }
            i++;
        }
    }
    return i;
}

static size_t parse_actions(TFile* f, size_t i, int n) {
    f->actions = calloc((size_t)(n > 0 ? n : 1), sizeof(TAction));
    for (int a = 0; a < n && f->actions; a++) {
        i = find(f, i, "action");
        if (i >= f->nlines) break;
        TAction* t = &f->actions[f->nactions++];
        int nc = 0;
        char* p = f->lines[i] + 7;
        nc = (int)strtol(p, &p, 10);
        for (int k = 0; k < 5; k++) strtof(p, &p);
        while (*p == ' ') p++;
        snprintf(t->name, sizeof t->name, "%s", p);
        t->curves = calloc((size_t)(nc > 0 ? nc : 1), sizeof(TCurve));
        i++;
        for (int c = 0; c < nc && t->curves; c++) {
            i = find(f, i, "fc");
            if (i >= f->nlines) break;
            TCurve* cu = &t->curves[t->ncurves++];
            int nk = 0;
            sscanf(f->lines[i] + 3, "%d %d", &cu->channel, &nk);
            i++;
            if (i < f->nlines && starts(f->lines[i], "fb")) snprintf(cu->bone, sizeof cu->bone, "%s", f->lines[i][2] ? f->lines[i] + 3 : ""), i++;
            if (i < f->nlines && starts(f->lines[i], "fp")) i++;
            cu->keys = calloc((size_t)(nk > 0 ? nk : 1), sizeof(TKey));
            while (i < f->nlines && cu->keys && cu->nkeys < nk && starts(f->lines[i], "k")) {
                TKey* k = &cu->keys[cu->nkeys++];
                sscanf(f->lines[i] + 2, "%f %f %f %f %f %f %d", &k->f, &k->v, &k->lx, &k->ly, &k->rx, &k->ry, &k->interp);
                i++;
            }
        }
    }
    return i;
}

static size_t parse_objects(TFile* f, size_t i, int n) {
    f->objs = calloc((size_t)(n > 0 ? n : 1), sizeof(TObj));
    for (int o = 0; o < n && f->objs; o++) {
        i = find(f, i, "o");
        if (i >= f->nlines) break;
        TObj* t = &f->objs[f->nobjs++];
        char* p = f->lines[i] + 2;
        t->type = (int)strtol(p, &p, 10);
        t->mesh = (int)strtol(p, &p, 10);
        for (int k = 0; k < 3; k++) t->loc[k] = strtof(p, &p);
        for (int k = 0; k < 3; k++) t->rot[k] = strtof(p, &p);
        for (int k = 0; k < 3; k++) t->scale[k] = strtof(p, &p);
        for (int k = 0; k < 3; k++) t->color[k] = strtof(p, &p);
        for (int k = 0; k < 2; k++) strtol(p, &p, 10);
        t->lens = strtof(p, &p), t->light_power = strtof(p, &p); // (Q19: the camera's lens, the light's watts, color, radius)
        for (int k = 0; k < 3; k++) t->light_color[k] = strtof(p, &p);
        t->light_radius = strtof(p, &p);
        t->split_cos = 0.866f; // (Auto Smooth's 30 degrees; an Edge Split modifier sets its own)
        while (*p == ' ') p++;
        snprintf(t->name, sizeof t->name, "%s", p);
        t->arm = -1, t->parent = -1, t->deform = -1, t->action = -1;
        memcpy(t->parent_inv, m4_id().m, sizeof t->parent_inv);
        i++;
        while (i < f->nlines && !starts(f->lines[i], "o")) {
            const char* l = f->lines[i];
            if (starts(l, "slots")) {
                char* q = (char*)l + 6;
                strtol(q, &q, 10), strtol(q, &q, 10);
                int ns = (int)strtol(q, &q, 10);
                for (int s = 0; s < ns && s < 32; s++) t->slots[t->nslots++] = (int)strtol(q, &q, 10);
            } else if (starts(l, "arm")) t->arm = atoi(l + 4);
            else if (starts(l, "light")) // (Q19) type, spot size and blend, the sun's angle, area shape and sizes
                sscanf(l + 6, "%d %f %f %f %d %f %f", &t->light_type, &t->spot_size, &t->spot_blend, &t->light_angle, &t->light_shape, &t->light_size,
                       &t->light_size_y);
            else if (starts(l, "mods")) t->mods_at = i; // (Q19) the modifier stack: "md" / "mp" pairs after it
            else if (starts(l, "anim")) { // "anim <graph> <on> <layers> <action per layer>": the first layer's action
                char* q = (char*)l + 5;
                strtol(q, &q, 10), strtol(q, &q, 10);
                int nl = (int)strtol(q, &q, 10);
                for (int k = 0; k < nl && t->action < 0; k++) t->action = (int)strtol(q, &q, 10);
            }
            else if (starts(l, "parent")) {
                char* q = (char*)l + 7;
                t->parent = (int)strtol(q, &q, 10);
                t->parent_type = (int)strtol(q, &q, 10);
                for (int k = 0; k < 16; k++) t->parent_inv[k] = strtof(q, &q);
                while (*q == ' ') q++;
                snprintf(t->parent_bone, sizeof t->parent_bone, "%s", q);
                if (t->parent_type == 1 && !t->parent_bone[0]) t->parent_type = 0;
            } else if (starts(l, "modifier")) {
                int on = 0, ob = -1;
                if (sscanf(l + 9, "%d %d", &on, &ob) == 2 && on && t->deform < 0) t->deform = ob;
            } else if (starts(l, "collections") || starts(l, "rigidworld") || starts(l, "lighting") || starts(l, "rig") || starts(l, "images") ||
                       starts(l, "evalmeshes"))
                break;
            i++;
        }
    }
    return i;
}

static int arm_bone_named(const TArm* a, const char* name) {
    for (int i = 0; name[0] && i < a->nbones; i++)
        if (!strcmp(a->bones[i].name, name)) return i;
    return -1;
}

// A bone's matrix in the armature's space: the rest (head, tail, roll) or posed by the file's channels (full inheritance:
// the parent's pose times the bone's rest offset from it times the channels), as Blender's pose_mat
static M4 bone_arm_matrix(const TArm* a, int b, bool posed, int depth) {
    const TBone* bn = &a->bones[b];
    M4 rest = bone_rest(bn->head, bn->tail, bn->roll), basis = m4_id();
    if (posed) {
        M4 r = m4_quat(bn->pquat);
        basis = m4_trs(bn->ploc, &r, bn->pscale);
    }
    if (bn->parent < 0 || bn->parent >= a->nbones || bn->parent == b || depth > 64) return m4_mul(&rest, &basis);
    const TBone* pb = &a->bones[bn->parent];
    M4 prest = bone_rest(pb->head, pb->tail, pb->roll), pinv = m4_inv(&prest), rel = m4_mul(&pinv, &rest);
    M4 pp = bone_arm_matrix(a, bn->parent, posed, depth + 1), t = m4_mul(&pp, &rel);
    return m4_mul(&t, &basis);
}

static M4 obj_world(TFile* f, int i, int depth) {
    TObj* o = &f->objs[i];
    if (o->world_done) return o->world;
    M4 r = m4_euler_xyz(o->rot);
    M4 local = m4_trs(o->loc, &r, o->scale);
    if (o->parent >= 0 && o->parent < f->nobjs && o->parent != i && depth < 32) {
        M4 pw = obj_world(f, o->parent, depth + 1), pi;
        const TObj* po = &f->objs[o->parent];
        if (o->parent_type == 1 && po->type == 3 && po->arm >= 0 && po->arm < f->narms) { // Bone parent: the posed bone at its tail
            const TArm* a = &f->arms[po->arm];
            int b = arm_bone_named(a, o->parent_bone);
            if (b >= 0 && a->bones[b].relative_parent) { // (Relative Parenting, as the Auto-Rig Pro's c_traj: the bone's deformation, no tail)
                M4 bp = bone_arm_matrix(a, b, true, 0), br = bone_arm_matrix(a, b, false, 0), bri = m4_inv(&br);
                M4 t = m4_mul(&bp, &bri);
                pw = m4_mul(&pw, &t);
            } else if (b >= 0) {
                M4 bp = bone_arm_matrix(a, b, true, 0);
                const TBone* bn = &a->bones[b];
                float d[3] = { bn->tail[0] - bn->head[0], bn->tail[1] - bn->head[1], bn->tail[2] - bn->head[2] };
                M4 tail = m4_id();
                tail.m[13] = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                M4 t = m4_mul(&bp, &tail);
                pw = m4_mul(&pw, &t);
            }
        }
        memcpy(pi.m, o->parent_inv, sizeof pi.m);
        M4 t = m4_mul(&pw, &pi);
        local = m4_mul(&t, &local);
    }
    o->world = local, o->world_done = true;
    return local;
}

// --- Writing the assets ---

static uint32_t oct_encode(const float n[3]) {
    float l1 = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    if (l1 < 1e-12f) return 0;
    float x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        float ox = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox, y = oy;
    }
    int16_t ix = (int16_t)lroundf(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f), iy = (int16_t)lroundf(fminf(fmaxf(y, -1.0f), 1.0f) * 32767.0f);
    return (uint16_t)ix | ((uint32_t)(uint16_t)iy << 16);
}

// A mesh's triangles into a .rmesh: each with its own corners first (the UV2 grid gives every triangle its cell), then
// mesh_build.c welds them (past MESH_ATLAS_MAX_TRIS), orders them and makes the LODs
typedef struct TriOut { float p[3][3], n[3][3], uv[3][2]; uint32_t col[3]; } TriOut; // (col: RGBA8, sRGB; white without)

static uint32_t g_vcol_meshes; // (this import's meshes written with vertex colors: the report's count)
static bool write_rmesh(const char* path, const TriOut* tris, uint32_t ntri, bool colors) {
    if (!ntri) return false;
    if (colors) g_vcol_meshes++;
    RMeshHeader h = { RMESH_MAGIC, RMESH_VERSION, ntri * 3, ntri * 3, { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, 0.0f, 0 };
    GpuVertex* v = calloc((size_t)ntri * 3, sizeof(GpuVertex));
    uint32_t* idx = NULL; // (the corners' order: mesh_build.c makes the indices)
    if (!v) return false;
    // UV2: the triangles in a grid, two to a square cell (a half each), a margin around them
    uint32_t side = (uint32_t)ceilf(sqrtf((float)ntri * 0.5f));
    if (side < 1) side = 1;
    float cell = 1.0f / (float)side, pad = cell * 0.12f;
    for (uint32_t t = 0; t < ntri; t++) {
        const TriOut* tr = &tris[t];
        float e1[3], e2[3], cr[3];
        for (int k = 0; k < 3; k++) e1[k] = tr->p[1][k] - tr->p[0][k], e2[k] = tr->p[2][k] - tr->p[0][k];
        cr[0] = e1[1] * e2[2] - e1[2] * e2[1], cr[1] = e1[2] * e2[0] - e1[0] * e2[2], cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
        h.area += 0.5f * sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        uint32_t c = t / 2, cx = c % side, cy = c / side;
        float x0 = (float)cx * cell, y0 = (float)cy * cell;
        static const float lo[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } }, hi[3][2] = { { 1, 1 }, { 0, 1 }, { 1, 0 } };
        const float(*corner)[2] = t & 1 ? hi : lo;
        for (int k = 0; k < 3; k++) {
            GpuVertex* g = &v[t * 3 + k];
            memcpy(g->pos, tr->p[k], 12);
            g->normal_oct = oct_encode(tr->n[k]);
            g->uv[0] = tr->uv[k][0], g->uv[1] = 1.0f - tr->uv[k][1]; // (Blender's V up; the bake's down)
            // inset toward the cell's middle so the triangles don't touch (a texel never straddles two)
            float u = corner[k][0], w = corner[k][1];
            g->uv2[0] = x0 + pad + u * (cell - 3.0f * pad), g->uv2[1] = y0 + pad + w * (cell - 3.0f * pad);
            if (t & 1) g->uv2[0] += pad, g->uv2[1] += pad;
            for (int q = 0; q < 3; q++) h.mn[q] = fminf(h.mn[q], tr->p[k][q]), h.mx[q] = fmaxf(h.mx[q], tr->p[k][q]);
        }
    }
    MeshBuilt mb;
    uint32_t* col = colors ? malloc(sizeof(uint32_t) * (size_t)ntri * 3) : NULL; // (the vertex colors: RMESH_COLORS)
    for (uint32_t t = 0; col && t < ntri; t++)
        for (int k = 0; k < 3; k++) col[t * 3 + k] = tris[t].col[k];
    bool ok = mesh_build_corners_col(v, col, ntri, true, &mb);
    free(col);
    if (ok) {
        mb.area = h.area;
        ok = rmesh_write(path, &mb);
        if (ntri >= 100000u)
            printf("[RealEngine] Mesh %s: %u triangles, %u vertices (%u corners), %u LODs (%u triangles last), %.0f ms\n", path, ntri,
                   mb.lods.index_count[0] ? mb.nv : 0u, ntri * 3u, mb.lods.count, mb.lods.index_count[mb.lods.count - 1] / 3u, mb.ms);
    }
    mesh_built_free(&mb);
    free(v), free(idx);
    return ok;
}

// --- Skins (RMESH_SKINNED): a vertex's 4 strongest bones and their weights, kept in its uv2's bits ---
typedef struct SkinW { int b[4]; float w[4]; } SkinW;

static void skin_add(SkinW* s, int bone, float wt) {
    if (bone < 0 || !(wt > 0.0f)) return;
    for (int k = 0; k < 4; k++)
        if (s->w[k] > 0.0f && s->b[k] == bone) { // (the same bone twice: summed)
            s->w[k] += wt;
            return;
        }
    int lo = 0;
    for (int k = 1; k < 4; k++)
        if (s->w[k] < s->w[lo]) lo = k;
    if (wt > s->w[lo]) s->b[lo] = bone, s->w[lo] = wt;
}

// The weights normalized to unorm8 summing to 255 (the rounding's rest on the strongest) into two floats' bits
static void skin_pack(const SkinW* s, int fallback, float uv2[2]) {
    float sum = s->w[0] + s->w[1] + s->w[2] + s->w[3];
    uint8_t j[4] = { 0, 0, 0, 0 }, q[4] = { 0, 0, 0, 0 };
    if (!(sum > 0.0f)) {
        j[0] = (uint8_t)(fallback > 0 ? fallback : 0), q[0] = 255;
    } else {
        int tot = 0, big = 0;
        for (int k = 0; k < 4; k++) {
            j[k] = (uint8_t)(s->w[k] > 0.0f && s->b[k] > 0 ? s->b[k] : 0);
            q[k] = (uint8_t)(s->w[k] > 0.0f ? (int)(s->w[k] / sum * 255.0f + 0.5f) : 0);
            tot += q[k];
            if (s->w[k] > s->w[big]) big = k;
        }
        q[big] = (uint8_t)(q[big] + (255 - tot));
    }
    uint32_t a = (uint32_t)j[0] | (uint32_t)j[1] << 8 | (uint32_t)j[2] << 16 | (uint32_t)j[3] << 24;
    uint32_t b = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    memcpy(&uv2[0], &a, 4), memcpy(&uv2[1], &b, 4);
}

// --- Morph targets on their way into the skins (render.h RMESH_MORPHS): the Skeletal Mesh's targets by name (two meshes'
// "Smile" one slider), the deltas in the Engine's space by source vertex (a scene's vertices numbered across its meshes),
// sorted by vertex at the end (MeshMorphs' CSR) ---
typedef struct MorphBuild {
    MeshMorphs m;         // names, default weights; first/d by source vertex after morph_finish
    uint32_t nsrc;        // the source vertices numbered so far
    RMeshMorphDelta* raw; // (the deltas as they come and their vertices)
    uint32_t* raw_v;
    size_t nraw, cap;
} MorphBuild;

static void morph_build_free(MorphBuild* b) {
    mesh_morphs_free(&b->m);
    free(b->raw), free(b->raw_v);
    memset(b, 0, sizeof *b);
}

static int morph_target(MorphBuild* b, const char* name, float weight) {
    char nm[RMESH_MORPH_NAME];
    snprintf(nm, sizeof nm, "%s", name && name[0] ? name : "Morph");
    for (uint32_t i = 0; i < b->m.count; i++)
        if (!strcmp(b->m.name[i], nm)) return (int)i;
    if (b->m.count >= RMESH_MAX_MORPHS) return -1;
    char(*nn)[RMESH_MORPH_NAME] = realloc(b->m.name, RMESH_MORPH_NAME * ((size_t)b->m.count + 1));
    if (nn) b->m.name = nn;
    float* nw = nn ? realloc(b->m.weight, sizeof(float) * ((size_t)b->m.count + 1)) : NULL;
    if (!nw) return -1;
    b->m.weight = nw;
    memcpy(b->m.name[b->m.count], nm, sizeof nm);
    b->m.weight[b->m.count] = weight;
    return (int)b->m.count++;
}

static void morph_add(MorphBuild* b, uint32_t v, int t, const float dp[3], const float dn[3]) {
    if (t < 0) return;
    float m = fabsf(dp[0]) + fabsf(dp[1]) + fabsf(dp[2]) + (fabsf(dn[0]) + fabsf(dn[1]) + fabsf(dn[2])) * 10.0f;
    if (m < 1e-4f) return; // (a vertex the target doesn't move: sparse)
    if (b->nraw >= b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1024;
        RMeshMorphDelta* nr = realloc(b->raw, sizeof *nr * c);
        if (nr) b->raw = nr;
        uint32_t* nv = nr ? realloc(b->raw_v, sizeof *nv * c) : NULL;
        if (!nv) return;
        b->raw_v = nv, b->cap = c;
    }
    RMeshMorphDelta* d = &b->raw[b->nraw];
    d->target = (uint32_t)t;
    memcpy(d->dp, dp, 12), memcpy(d->dn, dn, 12);
    b->raw_v[b->nraw++] = v;
}

static bool morph_finish(MorphBuild* b) {
    if (!b->m.count || !b->nraw || !b->nsrc) return false;
    free(b->m.first), free(b->m.d);
    b->m.first = calloc((size_t)b->nsrc + 1, sizeof(uint32_t));
    b->m.d = malloc(sizeof(RMeshMorphDelta) * b->nraw);
    if (!b->m.first || !b->m.d) return false;
    for (size_t k = 0; k < b->nraw; k++) b->m.first[b->raw_v[k] + 1]++;
    for (uint32_t v = 0; v < b->nsrc; v++) b->m.first[v + 1] += b->m.first[v];
    uint32_t* fill = calloc((size_t)b->nsrc, sizeof(uint32_t));
    if (!fill) return false;
    for (size_t k = 0; k < b->nraw; k++) {
        uint32_t v = b->raw_v[k];
        b->m.d[b->m.first[v] + fill[v]++] = b->raw[k];
    }
    free(fill);
    b->m.nv = b->nsrc, b->m.nd = (uint32_t)b->nraw;
    return true;
}

// The Skeletal Mesh's list of targets (the .rskm's "morphs")
static void morph_to_skel(const MorphBuild* b, SkelMesh* sk) {
    sk->nmorphs = 0;
    for (uint32_t i = 0; i < b->m.count && i < ANIM_MAX_MORPHS; i++) {
        snprintf(sk->morph_name[i], ANIM_MORPH_NAME, "%s", b->m.name[i]);
        sk->morph_default[i] = b->m.weight[i], sk->nmorphs++;
    }
}

// A skin's .rmesh: the triangles in the Skeletal Mesh's space at its bind, each corner's packed weights (skin_pack);
// morphs (may be NULL, morph_finish'd): its corners' source vertices csrc
static bool write_skin_rmesh(const char* path, const TriOut* tris, const float (*skin)[2], uint32_t ntri, const uint32_t* csrc,
                             const MeshMorphs* morphs) {
    if (!ntri) return false;
    GpuVertex* v = calloc((size_t)ntri * 3, sizeof(GpuVertex));
    if (!v) return false;
    float area = 0.0f;
    for (uint32_t t = 0; t < ntri; t++) {
        const TriOut* tr = &tris[t];
        float e1[3], e2[3], cr[3];
        for (int k = 0; k < 3; k++) e1[k] = tr->p[1][k] - tr->p[0][k], e2[k] = tr->p[2][k] - tr->p[0][k];
        cr[0] = e1[1] * e2[2] - e1[2] * e2[1], cr[1] = e1[2] * e2[0] - e1[0] * e2[2], cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
        area += 0.5f * sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        for (int k = 0; k < 3; k++) {
            GpuVertex* g = &v[t * 3 + k];
            memcpy(g->pos, tr->p[k], 12);
            g->normal_oct = oct_encode(tr->n[k]);
            g->uv[0] = tr->uv[k][0], g->uv[1] = 1.0f - tr->uv[k][1]; // (Blender's V up; the bake's down)
            memcpy(g->uv2, skin[t * 3 + k], 8);
        }
    }
    MeshBuilt mb;
    bool ok = mesh_build_skinned(v, ntri, csrc, morphs, &mb);
    if (ok) {
        mb.area = area;
        ok = rmesh_write(path, &mb);
    }
    mesh_built_free(&mb);
    free(v);
    return ok;
}

static uint32_t vhash(const char* vpath) { // the hash the assets are found by (anim.c's, blueprint.c's)
    uint32_t h = 2166136261u;
    for (const char* q = vpath; *q; q++) h = (h ^ (uint8_t)tolower((unsigned char)*q)) * 16777619u;
    return h ? h : 1u;
}

static void clean(char* s) { // a file name from a name
    for (char* c = s; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') *c = '_';
}

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)
#include "import_matconv.inc" // (Q19) the nodes the Engine lacks, the images

// (#164) The longest side of an image the material reads (a PNG's or JPEG's header, a .rudim's largest tile; 0: unknown),
// so the bake keeps a 4K map's detail (a fixed 2048 halved the user's 4096 UDIM: "blurred, low quality")
static uint32_t image_side(const char* disk) {
    const char* dot = strrchr(disk, '.');
    if (dot && !_stricmp(dot, ".rudim")) {
        UdimFile u;
        char err[160];
        if (!udim_open(disk, &u, err, sizeof err)) return 0;
        uint32_t m = 0;
        for (uint32_t i = 0; i < u.count && i < UDIM_MAX_TILES; i++) {
            if (u.tiles[i].width > m) m = u.tiles[i].width;
            if (u.tiles[i].height > m) m = u.tiles[i].height;
        }
        udim_close(&u);
        return m;
    }
    FILE* fp = re_fopen(disk, "rb");
    if (!fp) return 0;
    enum { CAP = 1 << 18 };
    uint8_t* b = malloc(CAP);
    size_t n = b ? fread(b, 1, CAP, fp) : 0;
    fclose(fp);
    uint32_t w = 0, h = 0;
    if (n >= 24 && !memcmp(b, "\x89PNG", 4)) // IHDR: width, height (big-endian) at 16
        w = (uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19], h = (uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23];
    else if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) // JPEG: the segments up to a SOFn
        for (size_t i = 2; i + 9 < n;) {
            if (b[i] != 0xFF) break;
            uint8_t mk = b[i + 1];
            if (mk == 0xFF) { i++; continue; }
            size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
                h = (uint32_t)b[i + 5] << 8 | b[i + 6], w = (uint32_t)b[i + 7] << 8 | b[i + 8];
                break;
            }
            i += 2 + len;
        }
    free(b);
    return w > h ? w : h;
}

// The material: the Tlender's nodes by idname into the Engine's (the same sockets), its images copied next to it;
// (Q19) a node the Engine lacks rewritten into Engine nodes or the Tlender's baked images (import_matconv.inc), each
// source socket mapped to the Engine's, the links made through the maps, every node in the conversion report
static uint32_t import_material(const TMat* t, const char* folder_disk, const char* folder_vpath, const char* src_dir, TlendReport* r) {
    char name[96], path[900], vpath[900];
    snprintf(name, sizeof name, "M_%s", t->name);
    clean(name);
    snprintf(path, sizeof path, "%s/Materials/%s.rmat", folder_disk, name);
    snprintf(vpath, sizeof vpath, "%s/Materials/%s.rmat", folder_vpath, name);
    char dir[900];
    snprintf(dir, sizeof dir, "%s/Materials", folder_disk);
    re_make_dirs(dir);
    // written as a file only (the importers run on a worker: the materials' registry is the main thread's, which reads
    // it again when the import is done: mat_reload_file)
    Material* m = calloc(1, sizeof *m);
    MatGraph* g = calloc(1, sizeof *g);
    McMap* mm = calloc((size_t)(t->nnodes ? t->nnodes : 1), sizeof(McMap));
    if (!m || !g || !mm) return free(m), free(g), free(mm), 0u;
    snprintf(m->path, sizeof m->path, "%s", path);
    m->resolution = 1024, m->anim_frames = 16, m->anim_res = 512, m->opacity_clip = MAT_OPACITY_CLIP_DEFAULT;
    m->graph = g;
    for (int i = 0; i < t->nnodes; i++)
        for (int k = 0; k < 16; k++) mm[i].out[k].node = -1;
    for (int i = 0; i < t->nnodes && i < 512; i++) {
        const TNode* s = &t->nodes[i];
        const MatNodeDef* d = mat_node_def_by_idname(s->idname);
        if (s->nbake) { // (Q19) baked by the Tlender: its outputs are those images
            // (2026-11-19, the Tlender's exporter: notas-dev/importar-tlender.md) the file's name tells the map: a shader of
            // the Tlender's own comes as _basecolor/_roughness/_normal and _emission or _metallic, all on output 0 (a BSDF
            // is no single image): a Principled made of them; a _normal (Bevel's, the BSDF's) is tangent space, OpenGL:
            // through a Normal Map
            int kind[4], pbr = -1;
            for (int b = 0; b < s->nbake; b++) {
                static const char* const SUF[] = { "_basecolor", "_roughness", "_normal", "_emission", "_metallic" };
                char stem[260];
                snprintf(stem, sizeof stem, "%s", s->bake_path[b]);
                char* dot = strrchr(stem, '.');
                if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = 0;
                size_t sl = strlen(stem);
                kind[b] = -1;
                for (int q = 0; q < 5; q++) {
                    size_t ql = strlen(SUF[q]);
                    if (sl >= ql && !strcmp(stem + sl - ql, SUF[q])) kind[b] = q;
                }
                if (kind[b] == 0 && s->bake_sock[b] == 0) pbr = 0;
            }
            if (pbr == 0) pbr = mat_graph_add(g, NODE_BSDF_PRINCIPLED, s->x + 260.0f, -s->y);
            for (int b = 0; b < s->nbake; b++) {
                int k = mat_graph_add(g, NODE_TEX_IMAGE, s->x, -s->y + 40.0f * (float)b);
                if (k < 0) continue;
                MatNode* n = &g->nodes[k];
                n->prop[2] = s->bake_cs[b] ? IMAGE_COLORSPACE_NONCOLOR : IMAGE_COLORSPACE_SRGB;
                TNode tmp = *s;
                snprintf(tmp.image, sizeof tmp.image, "%s", s->bake_path[b]);
                mc_image(&tmp, n, folder_disk, folder_vpath, src_dir, t->name, r);
                int from = k;
                if (kind[b] == 2) { // tangent space -> the Normal Map's (OpenGL, the Tlender's convention)
                    int nm = mat_graph_add(g, NODE_NORMAL_MAP, s->x + 130.0f, -s->y + 40.0f * (float)b);
                    if (nm >= 0) g->nodes[nm].prop[0] = 0 /* Tangent */, g->nodes[nm].prop[1] = 0 /* OpenGL */, mat_graph_link(g, k, 0, nm, 1), from = nm;
                }
                if (pbr >= 0 && s->bake_sock[b] == 0 && kind[b] >= 0) {
                    static const int TO[] = { 0, 2, 5, 26, 1 }; // Base Color, Roughness, Normal, Emission Color, Metallic
                    mat_graph_link(g, from, 0, pbr, TO[kind[b]]);
                    if (kind[b] == 3) g->nodes[pbr].in[27][0] = 1.0f; // (Emission Strength: the image is the emitted color, held in 0..1)
                } else mc_out(&mm[i], s->bake_sock[b], from, 0);
            }
            if (pbr >= 0) mc_out(&mm[i], 0, pbr, 0);
            imprep_add(IMPREP_BAKED, IMPREP_CAT_NODE, t->name, s->idname, "%s",
                       pbr >= 0 ? TR("baked by the Tlender on its GPU into its maps (color, roughness, normal, emission or metallic): a Principled BSDF made of them")
                                : TR("baked by the Tlender on its GPU into an image: an Image Texture in its place"));
            continue;
        }
        if (!d) { // (Q19) a node the Engine lacks
            char why[200];
            int kind = mc_convert(g, m, t, i, &mm[i], why, sizeof why);
            if (kind == IMPREP_LOST) r->skipped_nodes++;
            imprep_add(kind, IMPREP_CAT_NODE, t->name, s->idname, "%s", why);
            int en = mm[i].out[0].node; // (an Environment Texture: its image into the Image Texture it became)
            if (!strcmp(s->idname, "ShaderNodeTexEnvironment") && en >= 0 && g->nodes[en].type == NODE_TEX_IMAGE && s->image[0])
                mc_image(s, &g->nodes[en], folder_disk, folder_vpath, src_dir, t->name, r);
            continue;
        }
        int k = mat_graph_add(g, d->type, s->x, -s->y);
        if (k < 0) continue;
        for (int o = 0; o < d->nout && o < 16; o++) mc_out(&mm[i], o, k, o);
        for (int q = 0; q < d->nin && q < 32; q++) mc_in(&mm[i], q, k, q);
        if (mc_baked_only(s->idname))
            imprep_add(IMPREP_BAKED, IMPREP_CAT_NODE, t->name, s->idname, "%s", TR("evaluated only in the material's GPU bake, into its textures (the same math as the Tlender's)"));
        else imprep_add(IMPREP_SAME, IMPREP_CAT_NODE, t->name, s->idname, "%s", TR("the same node"));
        MatNode* n = &g->nodes[k];
        for (int q = 0; q < 4; q++)
            if (s->prop[q] >= 0) n->prop[q] = s->prop[q]; // (-1: the node's default: the other importers' nodes)
        int nin = s->nin < d->nin ? s->nin : d->nin;
        for (int q = 0; q < nin && q < MAT_NODE_MAX_IN && q < 32; q++) memcpy(n->in[q], s->in[q], 16);
        memcpy(n->value, s->value, 16);
        n->ramp_count = s->ramp_count < RAMP_MAX_STOPS ? s->ramp_count : RAMP_MAX_STOPS;
        for (int q = 0; q < n->ramp_count; q++) memcpy(n->ramp[q], s->ramp[q], 20);
        snprintf(n->label, sizeof n->label, "%s", s->label);
        if (s->image[0]) mc_image(s, n, folder_disk, folder_vpath, src_dir, t->name, r); // (Q19: names, generated, UDIM, found near)
    }
    for (int i = 0; i < t->nlinks; i++) { // through the maps: a source output into every Engine input its target feeds
        const int* l = t->links[i];
        if (l[0] < 0 || l[2] < 0 || l[0] >= t->nnodes || l[2] >= t->nnodes || l[1] < 0 || l[1] >= 16 || l[3] < 0 || l[3] >= 32) continue;
        McSock from = mm[l[0]].out[l[1]];
        if (from.node < 0) continue;
        for (int k = 0; k < mm[l[2]].nt[l[3]]; k++) mat_graph_link(g, from.node, from.sock, mm[l[2]].in[l[3]][k].node, mm[l[2]].in[l[3]][k].sock);
    }
    free(mm);
    bool auto_maps = mc_auto_maps(g, m, t, folder_disk, folder_vpath, src_dir, r); // (2026-11-23: by name, around the file)
    // A Principled BSDF whose Alpha (input 4) is linked or under 1: cut by it, both sides seen (Blender draws the back
    // faces; hair cards, lashes, leaves are made that way). Images: baked at 2048 (1024 blurred the Tlender's 4K maps), or
    // at the largest image's size up to 4096 (#164: a 4096 UDIM baked at 2048 looked blurred)
    bool images = false;
    for (int i = 0; i < t->nnodes; i++) {
        images |= t->nodes[i].image[0] != 0;
        if (strcmp(t->nodes[i].idname, "ShaderNodeBsdfPrincipled")) continue;
        bool cut = t->nodes[i].nin > 4 && t->nodes[i].in[4][0] < 0.999f;
        for (int k = 0; k < t->nlinks; k++) cut |= t->links[k][2] == i && t->links[k][3] == 4;
        if (cut) m->blend_mode = MAT_BLEND_MASKED, m->two_sided = true, m->opacity_clip = MAT_OPACITY_CLIP_DEFAULT;
    }
    images |= auto_maps;
    if (images) {
        uint32_t side = 0;
        size_t vl = strlen(folder_vpath);
        for (int k = 0; k < g->node_count; k++) {
            const MatNode* n = &g->nodes[k];
            if (n->type != NODE_TEX_IMAGE || strncmp(n->image, folder_vpath, vl) || n->image[vl] != '/') continue;
            char disk[1100];
            snprintf(disk, sizeof disk, "%s%s", folder_disk, n->image + vl);
            uint32_t sd = image_side(disk);
            if (sd > side) side = sd;
        }
        m->resolution = 2048;
        while (m->resolution < 4096 && (uint32_t)m->resolution < side) m->resolution *= 2;
    }
    bool saved = mat_save(m);
    free(g), free(m);
    if (!saved) return 0;
    if (r->nmat_paths < TLEND_MAX_MAT_PATHS) snprintf(r->mat_paths[r->nmat_paths++], sizeof r->mat_paths[0], "%s", path);
    r->materials++;
    return mat_vpath_hash(vpath);
}

// Bezier F-curve value at a frame (Blender's handles; linear and constant segments too)
static float curve_eval(const TCurve* c, float fr) {
    if (!c->nkeys) return 0.0f;
    if (fr <= c->keys[0].f) return c->keys[0].v;
    if (fr >= c->keys[c->nkeys - 1].f) return c->keys[c->nkeys - 1].v;
    int i = 0;
    while (i + 1 < c->nkeys && c->keys[i + 1].f < fr) i++;
    const TKey *a = &c->keys[i], *b = &c->keys[i + 1];
    if (a->interp == 2) return a->v;
    float span = b->f - a->f;
    if (a->interp != 0 || span <= 1e-6f) return a->v + (b->v - a->v) * (span > 1e-6f ? (fr - a->f) / span : 0.0f);
    // x(t) = fr by bisection on the cubic (the handles clamped inside the segment: x stays monotonic)
    float x0 = a->f, x1 = fminf(fmaxf(a->rx, a->f), b->f), x2 = fminf(fmaxf(b->lx, a->f), b->f), x3 = b->f;
    float lo = 0.0f, hi = 1.0f, t = 0.5f;
    for (int it = 0; it < 30; it++) {
        t = (lo + hi) * 0.5f;
        float u = 1 - t, x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        if (x < fr) lo = t;
        else hi = t;
    }
    float u = 1 - t;
    return u * u * u * a->v + 3 * u * u * t * a->ry + 3 * u * t * t * b->ly + t * t * t * b->v;
}

typedef struct BonePose { float loc[3], quat[4], scale[3]; } BonePose;

static int bone_index(const TArm* a, const char* name) {
    for (int i = 0; i < a->nbones; i++)
        if (!strcmp(a->bones[i].name, name)) return i;
    return -1;
}

// The armature's bones in the Engine's space (world): rest, or posed by an action at a frame
static void bones_world(const TArm* a, const M4* arm_world, const BonePose* pose, M4* out) {
    M4* rest = malloc(sizeof(M4) * (size_t)a->nbones);
    M4* posed = malloc(sizeof(M4) * (size_t)a->nbones);
    if (!rest || !posed) {
        free(rest), free(posed);
        return;
    }
    for (int b = 0; b < a->nbones; b++) {
        const TBone* bn = &a->bones[b];
        rest[b] = bone_rest(bn->head, bn->tail, bn->roll);
        M4 basis = m4_id();
        if (pose) {
            M4 r = m4_quat(pose[b].quat);
            basis = m4_trs(pose[b].loc, &r, pose[b].scale);
        }
        if (bn->parent >= 0 && bn->parent < b) {
            M4 pinv = m4_inv(&rest[bn->parent]), rel = m4_mul(&pinv, &rest[b]), t = m4_mul(&posed[bn->parent], &rel);
            posed[b] = m4_mul(&t, &basis);
        } else
            posed[b] = m4_mul(&rest[b], &basis);
        M4 wb = m4_mul(arm_world, &posed[b]);
        out[b] = m4_to_engine(&wb);
    }
    free(rest), free(posed);
}

static void local_xf(const M4* parent, const M4* child, AnimXf* x) {
    M4 pi = parent ? m4_inv(parent) : m4_id(), l = parent ? m4_mul(&pi, child) : *child;
    float axes[3][3];
    m4_split(&l, x->t, axes, x->s);
    axes_quat_xyzw(axes, x->q);
}

static void write_name(char* out, size_t cap, const char* prefix, const char* a, const char* b) {
    snprintf(out, cap, b ? "%s%s_%s" : "%s%s", prefix, a, b ? b : "");
    clean(out);
}

// --- The rig: the Tlender's bone constraints (its rig_eval.c, copied and adapted to these matrices), evaluated frame by
// frame so an action that moves a control rig bakes into the bones that deform the meshes (a deform armature whose
// bones Copy Transforms from the control rig's, IK legs, Child Of space switches, Transformation foot rolls...).
// Not evaluated: drivers (their properties keep the file's values), Spline IK, Armature, Action, Track To, Locked
// Track, Limit Distance, Maintain Volume (the bone keeps what the ones before gave it).


static void v3sub(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v3len(const float a[3]) { return sqrtf(v3dot(a, a)); }
static void v3cross(const float a[3], const float b[3], float o[3]) {
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, r, sizeof r);
}
static void v3norm(float v[3]) {
    float l = v3len(v);
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
}
static void m4_col(const M4* m, int c, float o[3]) { o[0] = m->m[c * 4], o[1] = m->m[c * 4 + 1], o[2] = m->m[c * 4 + 2]; }
static void m4_set_col(M4* m, int c, const float v[3]) { m->m[c * 4] = v[0], m->m[c * 4 + 1] = v[1], m->m[c * 4 + 2] = v[2]; }
static float m4_det3(const M4* a) {
    const float* m = a->m;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
}
static void m4_scale_of(const M4* m, float s[3]) {
    for (int c = 0; c < 3; c++) {
        float v[3];
        m4_col(m, c, v);
        s[c] = v3len(v);
    }
}
// Blender's mat4_to_loc_rot_size: the axes normalized (a mirroring matrix: negative sizes) and their lengths
static void m4_decomp(const M4* m, float loc[3], M4* rot, float size[3]) {
    M4 r = m4_id();
    for (int c = 0; c < 3; c++) {
        float v[3];
        m4_col(m, c, v);
        float l = v3len(v);
        size[c] = l;
        if (l > 1e-20f) v[0] /= l, v[1] /= l, v[2] /= l;
        m4_set_col(&r, c, v);
    }
    if (m4_det3(&r) < 0.0f)
        for (int c = 0; c < 3; c++) {
            float v[3];
            m4_col(&r, c, v);
            v[0] = -v[0], v[1] = -v[1], v[2] = -v[2];
            m4_set_col(&r, c, v);
            size[c] = -size[c];
        }
    if (loc) loc[0] = m->m[12], loc[1] = m->m[13], loc[2] = m->m[14];
    if (rot) *rot = r;
}
static M4 m4_compose(const float loc[3], const M4* rot, const float size[3]) {
    M4 r = *rot;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++) r.m[c * 4 + k] = rot->m[c * 4 + k] * size[c];
    r.m[12] = loc[0], r.m[13] = loc[1], r.m[14] = loc[2], r.m[15] = 1.0f;
    return r;
}
// Euler XYZ of a rotation: of Blender's two solutions, the one with the smaller sum of angles
static void euler_of(const M4* r, float e[3]) {
    float cy = hypotf(r->m[0], r->m[1]), e1[3], e2[3];
    if (cy > 16.0f * 1.1920929e-7f) {
        e1[0] = atan2f(r->m[6], r->m[10]), e1[1] = atan2f(-r->m[2], cy), e1[2] = atan2f(r->m[1], r->m[0]);
        e2[0] = atan2f(-r->m[6], -r->m[10]), e2[1] = atan2f(-r->m[2], -cy), e2[2] = atan2f(-r->m[1], -r->m[0]);
    } else {
        e1[0] = atan2f(-r->m[9], r->m[5]), e1[1] = atan2f(-r->m[2], cy), e1[2] = 0.0f;
        memcpy(e2, e1, sizeof e2);
    }
    float s1 = fabsf(e1[0]) + fabsf(e1[1]) + fabsf(e1[2]), s2 = fabsf(e2[0]) + fabsf(e2[1]) + fabsf(e2[2]);
    memcpy(e, s1 > s2 ? e2 : e1, sizeof e1);
}
static M4 rot_of(const M4* m) {
    M4 r;
    float s[3];
    m4_decomp(m, NULL, &r, s);
    return r;
}
static void quat_of(const M4* rot, float q[4]) { // w x y z
    float axes[3][3], x[4];
    for (int c = 0; c < 3; c++) m4_col(rot, c, axes[c]);
    axes_quat_xyzw(axes, x);
    q[0] = x[3], q[1] = x[0], q[2] = x[1], q[3] = x[2];
}
static void quat_between(const float a0[3], const float b0[3], float q[4]) { // the shortest turn of a onto b (w x y z)
    float a[3] = { a0[0], a0[1], a0[2] }, b[3] = { b0[0], b0[1], b0[2] }, c[3];
    v3norm(a), v3norm(b);
    float d = v3dot(a, b);
    if (d < -0.999999f) { // opposite: half a turn about any perpendicular axis
        float ax[3] = { 1, 0, 0 };
        v3cross(a, ax, c);
        if (v3len(c) < 1e-6f) ax[0] = 0, ax[1] = 1, v3cross(a, ax, c);
        v3norm(c);
        q[0] = 0, q[1] = c[0], q[2] = c[1], q[3] = c[2];
        return;
    }
    v3cross(a, b, c);
    q[0] = 1.0f + d, q[1] = c[0], q[2] = c[1], q[3] = c[2];
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; k++) q[k] /= l;
}
static void quat_slerp(const float a[4], const float b0[4], float t, float o[4]) {
    float b[4] = { b0[0], b0[1], b0[2], b0[3] }, d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) d = -d, b[0] = -b[0], b[1] = -b[1], b[2] = -b[2], b[3] = -b[3];
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
    }
    float l = 0.0f;
    for (int k = 0; k < 4; k++) o[k] = a[k] * wa + b[k] * wb, l += o[k] * o[k];
    l = sqrtf(l);
    for (int k = 0; k < 4; k++) o[k] /= l > 1e-12f ? l : 1.0f;
}
static M4 m4_translate(const float p[3]) {
    M4 r = m4_id();
    r.m[12] = p[0], r.m[13] = p[1], r.m[14] = p[2];
    return r;
}
// Rotation q about point p, applied to m
static M4 rotate_about(const M4* m, const float q[4], const float p[3]) {
    float np[3] = { -p[0], -p[1], -p[2] };
    M4 a = m4_translate(p), r = m4_quat(q), b = m4_translate(np), t = m4_mul(&r, &b), u = m4_mul(&a, &t);
    return m4_mul(&u, m);
}
// Between two transforms (the constraints' influence): location and scale linear, rotation by slerp
static M4 blend_m4(const M4* m0, const M4* m1, float t) {
    if (t >= 1.0f) return *m1;
    if (t <= 0.0f) return *m0;
    float l0[3], l1[3], s0[3], s1[3], q0[4], q1[4], q[4], l[3], s[3];
    M4 r0, r1;
    m4_decomp(m0, l0, &r0, s0);
    m4_decomp(m1, l1, &r1, s1);
    quat_of(&r0, q0), quat_of(&r1, q1);
    quat_slerp(q0, q1, t, q);
    for (int k = 0; k < 3; k++) l[k] = l0[k] + (l1[k] - l0[k]) * t, s[k] = s0[k] + (s1[k] - s0[k]) * t;
    M4 r = m4_quat(q);
    return m4_compose(l, &r, s);
}
// The axes made orthogonal around Y (Blender's mat4_ortho_stable on Y): lengths kept, or 1 when normalize
static void ortho_y(M4* m, bool normalize) {
    float x[3], y[3], z[3], s[3];
    m4_scale_of(m, s);
    m4_col(m, 0, x), m4_col(m, 1, y), m4_col(m, 2, z);
    v3norm(y);
    float d = v3dot(z, y);
    for (int k = 0; k < 3; k++) z[k] -= y[k] * d;
    v3norm(z);
    v3cross(y, z, x);
    v3norm(x);
    for (int k = 0; k < 3; k++) {
        float fx = normalize ? 1.0f : s[0], fy = normalize ? 1.0f : s[1], fz = normalize ? 1.0f : s[2];
        m->m[k] = x[k] * fx, m->m[4 + k] = y[k] * fy, m->m[8 + k] = z[k] * fz;
    }
}


// The pose channels (a bone's loc, quat w x y z, scale) as a matrix
static M4 chan_matrix(const float loc[3], const float quat[4], const float scale[3]) {
    M4 r = m4_quat(quat);
    return m4_trs(loc, &r, scale);
}

// A bone's local (channels) matrix into the armature's pose space and back (full inheritance: the parent's pose times
// the rest offset from it)
static M4 local_to_pose(const TArm* a, int i, const M4* local) {
    const TBone* b = &a->bones[i];
    if (b->parent >= 0) {
        M4 t = m4_mul(&a->pose[b->parent], &a->rel[i]);
        return m4_mul(&t, local);
    }
    return m4_mul(&a->rest[i], local);
}
static M4 pose_to_local(const TArm* a, int i, const M4* pose) {
    const TBone* b = &a->bones[i];
    M4 base = b->parent >= 0 ? m4_mul(&a->pose[b->parent], &a->rel[i]) : a->rest[i], inv = m4_inv(&base);
    return m4_mul(&inv, pose);
}
static M4 bone_to_space(const TArm* a, int i, const M4* w, const M4* m, int space) {
    switch (space) {
    case SP_WORLD: return m4_mul(w, m);
    case SP_LOCAL_PARENT: {
        M4 ri = m4_inv(&a->rest[i]);
        return m4_mul(&ri, m);
    }
    case SP_LOCAL: return pose_to_local(a, i, m);
    default: return *m;
    }
}
static M4 bone_from_space(const TArm* a, int i, const M4* w, const M4* m, int space) {
    switch (space) {
    case SP_WORLD: {
        M4 wi = m4_inv(w);
        return m4_mul(&wi, m);
    }
    case SP_LOCAL_PARENT: return m4_mul(&a->rest[i], m);
    case SP_LOCAL: return local_to_pose(a, i, m);
    default: return *m;
    }
}

typedef struct RigCtx {
    TFile* f;
    const TAction* act; // the action baked (on the armatures whose bones it moves)
    float frame;
} RigCtx;

static bool uses_head_tail(int type) {
    return type == TC_COPY_LOC || type == TC_COPY_XF || type == TC_DAMPED || type == TC_STRETCH || type == TC_TRACK_TO ||
           type == TC_LOCKED_TRACK || type == TC_LIMIT_DIST || type == TC_IK;
}

// The target as a matrix in a space (its head moved toward its tail by head_tail)
static bool target_matrix(const RigCtx* x, const TCon* c, int space, M4* out) {
    if (c->tarm < 0 || c->tbone < 0) return false;
    const TArm* ta = &x->f->arms[c->tarm];
    if (!ta->pose || c->tbone >= ta->nbones) return false;
    M4 p = ta->pose[c->tbone];
    if (uses_head_tail(c->type) && c->head_tail != 0.0f) {
        float tail[3], y[3] = { 0, ta->length[c->tbone], 0 };
        m4_point(&p, y, tail);
        for (int k = 0; k < 3; k++) p.m[12 + k] += (tail[k] - p.m[12 + k]) * c->head_tail;
    }
    *out = bone_to_space(ta, c->tbone, &ta->world, &p, space);
    return true;
}
// The target in the owner armature's pose space (through the world when it is another armature's)
static bool target_pose_matrix(const RigCtx* x, const TArm* a, int ai, const TCon* c, M4* out) {
    if (c->tarm == ai) return target_matrix(x, c, SP_POSE, out);
    M4 w;
    if (!target_matrix(x, c, SP_WORLD, &w)) return false;
    M4 inv = m4_inv(&a->world);
    *out = m4_mul(&inv, &w);
    return true;
}

static M4 con_copy_location(const TCon* c, M4 m, const M4* t) {
    for (int k = 0; k < 3; k++) {
        if (!c->axis[k]) continue;
        float v = c->invert[k] ? -t->m[12 + k] : t->m[12 + k];
        m.m[12 + k] = c->offset ? m.m[12 + k] + v : v;
    }
    return m;
}
static M4 con_copy_rotation(const TCon* c, const M4* m, const M4* t) {
    int mix = c->mix;
    bool all = c->axis[0] && c->axis[1] && c->axis[2] && !c->invert[0] && !c->invert[1] && !c->invert[2];
    if (mix == 0 && c->offset) mix = all ? 3 : 2; // (old files' offset: before for all axes, else add)
    float loc[3], size[3], obeul[3], eul[3];
    M4 oldrot;
    m4_decomp(m, loc, &oldrot, size);
    euler_of(&oldrot, obeul);
    M4 tr = *t;
    ortho_y(&tr, true);
    euler_of(&tr, eul);
    for (int k = 0; k < 3; k++) {
        if (!c->axis[k]) {
            eul[k] = mix == 0 || mix == 1 ? obeul[k] : 0.0f;
            continue;
        }
        if (mix == 1) eul[k] += obeul[k];
        if (c->invert[k]) eul[k] = -eul[k];
    }
    if (mix == 2)
        for (int k = 0; k < 3; k++) eul[k] += obeul[k];
    M4 nr = m4_euler_xyz(eul);
    if (mix == 3) nr = m4_mul(&nr, &oldrot);
    else if (mix == 4) nr = m4_mul(&oldrot, &nr);
    return m4_compose(loc, &nr, size);
}
static M4 con_copy_scale(const TCon* c, const M4* m, const M4* t) {
    float obsize[3], size[3];
    m4_scale_of(m, obsize);
    if (c->uniform) {
        float total = 1.0f, ts[3];
        if (c->axis[0] && c->axis[1] && c->axis[2]) total = fabsf(m4_det3(t));
        else {
            m4_scale_of(t, ts);
            for (int k = 0; k < 3; k++)
                if (c->axis[k]) total *= ts[k];
        }
        size[0] = size[1] = size[2] = cbrtf(total);
    } else
        m4_scale_of(t, size);
    float power = c->power > 0.0f ? c->power : 1.0f;
    for (int k = 0; k < 3; k++) {
        if (power != 1.0f) size[k] = powf(size[k], power);
        if (c->offset) size[k] = c->scale_add ? size[k] + obsize[k] - 1.0f : size[k] * obsize[k];
    }
    M4 r = *m;
    for (int k = 0; k < 3; k++)
        if ((c->axis[k] || c->uniform) && obsize[k] != 0.0f)
            for (int q = 0; q < 3; q++) r.m[k * 4 + q] *= size[k] / obsize[k];
    return r;
}
// Blender's split channels (locations add, rotations and scales multiply) and aligned scale (b's location through a)
static M4 mul_split(const M4* a, const M4* b, bool aligned) {
    float la[3], lb[3], sa[3], sb[3], l[3], s[3];
    M4 ra, rb;
    m4_decomp(a, la, &ra, sa);
    m4_decomp(b, lb, &rb, sb);
    M4 r = m4_mul(&ra, &rb);
    if (aligned) m4_point(a, lb, l);
    else
        for (int k = 0; k < 3; k++) l[k] = la[k] + lb[k];
    for (int k = 0; k < 3; k++) s[k] = sa[k] * sb[k];
    return m4_compose(l, &r, s);
}
static M4 con_copy_transforms(const TCon* c, const M4* m, M4 t) {
    if (c->remove_shear) ortho_y(&t, false);
    switch (c->mix) {
    case 5: return m4_mul(&t, m);
    case 6: return m4_mul(m, &t);
    case 1: return mul_split(&t, m, true);
    case 2: return mul_split(m, &t, true);
    case 3: return mul_split(&t, m, false);
    case 4: return mul_split(m, &t, false);
    default: return t;
    }
}
static M4 con_damped_track(const TCon* c, const M4* m, const M4* t) {
    int axis = c->track < 0 || c->track > 5 ? 1 : c->track;
    float now[3], head[3] = { m->m[12], m->m[13], m->m[14] }, want[3], q[4];
    m4_col(m, axis % 3, now);
    if (axis >= 3) now[0] = -now[0], now[1] = -now[1], now[2] = -now[2];
    v3sub(&t->m[12], head, want);
    if (v3len(want) < 1e-8f || v3len(now) < 1e-8f) return *m;
    quat_between(now, want, q);
    return rotate_about(m, q, head);
}
static float limit_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
// Stretch To (Blender's stretchto_evaluate): in Swing mode the owner's axes made orthogonal around Y first (their
// lengths kept); Y pointed at the target and scaled to its distance over the rest length, X / Z by the bulge (its
// limits), the other axes by the mode: swing (the shortest turn of Y), keep X or keep Z
static M4 con_stretch_to(const TCon* c, M4 m, const M4* t, float length) {
    if (c->stretch_keep == 0) ortho_y(&m, false);
    float size[3], head[3] = { m.m[12], m.m[13], m.m[14] }, vec[3];
    m4_scale_of(&m, size);
    v3sub(&t->m[12], head, vec);
    float dist = v3len(vec);
    if (dist < 1e-8f || size[1] < 1e-12f) return m;
    for (int k = 0; k < 3; k++) vec[k] /= dist;
    dist /= size[1];
    float rest = c->rest_length > 0.0f ? c->rest_length : length;
    if (rest <= 0.0f) return m;
    float bulge = powf(rest / dist, c->volume);
    if (bulge > 1.0f && c->bulge_max_on) {
        float bmax = fmaxf(c->bulge_max, 1.0f), hard = fminf(bulge, bmax), range = bmax - 1.0f;
        float soft = 1.0f + range * atanf((bulge - 1.0f) * (range > 0.0f ? 1.0f / range : 0.0f)) / 1.57079633f;
        bulge = c->bulge_smooth * soft + (1.0f - c->bulge_smooth) * hard;
    }
    if (bulge < 1.0f && c->bulge_min_on) {
        float bmin = limit_clamp(c->bulge_min, 0.0f, 1.0f), hard = fmaxf(bulge, bmin), range = 1.0f - bmin;
        float soft = 1.0f - range * atanf((1.0f - bulge) * (range > 0.0f ? 1.0f / range : 0.0f)) / 1.57079633f;
        bulge = c->bulge_smooth * soft + (1.0f - c->bulge_smooth) * hard;
    }
    float sc[3] = { 1.0f, dist / rest, 1.0f };
    if (c->volume_mode == 1) sc[0] = bulge;
    else if (c->volume_mode == 2) sc[2] = bulge;
    else if (c->volume_mode != 3) sc[0] = sc[2] = sqrtf(bulge);
    float nx[3], ny[3], nz[3];
    m4_col(&m, 0, nx), m4_col(&m, 1, ny), m4_col(&m, 2, nz);
    for (int k = 0; k < 3; k++) nx[k] /= fmaxf(size[0], 1e-30f), ny[k] /= size[1], nz[k] /= fmaxf(size[2], 1e-30f);
    M4 rot = m4_id();
    if (c->stretch_keep == 1) { // keep X: Z orthogonal to the old X and the new Y
        float orth[3], x[3];
        v3cross(nx, vec, orth), v3norm(orth);
        v3cross(vec, orth, x), v3norm(x);
        m4_set_col(&rot, 0, x), m4_set_col(&rot, 1, vec), m4_set_col(&rot, 2, orth);
    } else if (c->stretch_keep == 2) { // keep Z
        float orth[3], z[3];
        v3cross(nz, vec, orth), v3norm(orth);
        orth[0] = -orth[0], orth[1] = -orth[1], orth[2] = -orth[2];
        v3cross(orth, vec, z), v3norm(z);
        m4_set_col(&rot, 0, orth), m4_set_col(&rot, 1, vec), m4_set_col(&rot, 2, z);
    } else { // swing
        float q[4];
        m4_set_col(&rot, 0, nx), m4_set_col(&rot, 1, ny), m4_set_col(&rot, 2, nz);
        quat_between(ny, vec, q);
        M4 qm = m4_quat(q);
        rot = m4_mul(&qm, &rot);
    }
    for (int k = 0; k < 3; k++) size[k] *= sc[k];
    return m4_compose(head, &rot, size);
}
static M4 con_limit(const TCon* c, M4 m) {
    if (c->type == TC_LIMIT_LOC) {
        for (int k = 0; k < 3; k++)
            if (c->axis[k]) m.m[12 + k] = limit_clamp(m.m[12 + k], c->min[k], c->max[k]);
        return m;
    }
    if (c->type == TC_LIMIT_SCALE) {
        float s[3];
        m4_scale_of(&m, s);
        for (int k = 0; k < 3; k++)
            if (c->axis[k] && s[k] != 0.0f)
                for (int q = 0; q < 3; q++) m.m[k * 4 + q] *= limit_clamp(s[k], c->min[k], c->max[k]) / s[k];
        return m;
    }
    float loc[3], size[3], e[3];
    M4 r;
    m4_decomp(&m, loc, &r, size);
    ortho_y(&r, true);
    euler_of(&r, e);
    for (int k = 0; k < 3; k++)
        if (c->axis[k]) e[k] = limit_clamp(e[k], c->min[k], c->max[k]);
    M4 nr = m4_euler_xyz(e);
    return m4_compose(loc, &nr, size);
}
// Child Of: parent * the stored inverse * owner (all channels; a partial Child Of keeps the owner's location axes left out)
static M4 con_child_of(const TCon* c, const M4* par, const M4* m) {
    M4 inv;
    memcpy(inv.m, c->inverse, sizeof inv.m);
    M4 t = m4_mul(&inv, m), r = m4_mul(par, &t);
    for (int k = 0; k < 3; k++)
        if (!(c->channels & (1u << k))) r.m[12 + k] = m->m[12 + k];
    return r;
}
// Transformation: a range of the target's location / rotation / scale mapped onto a range of the owner's
static M4 con_transformation(const TCon* c, const M4* m, const M4* t) {
    float dvec[3], sval[3], loc[3], size[3], nv[3];
    M4 rot;
    if (c->from == 2) {
        m4_scale_of(t, dvec);
        if (m4_det3(t) < 0.0f) dvec[0] = -dvec[0], dvec[1] = -dvec[1], dvec[2] = -dvec[2];
    } else if (c->from == 1) {
        M4 r = rot_of(t);
        euler_of(&r, dvec);
    } else
        memcpy(dvec, &t->m[12], 12);
    for (int i = 0; i < 3; i++) {
        float lo = c->from_min[i], hi = c->from_max[i], v = dvec[i];
        if (!c->extrapolate) v = v < lo ? lo : v > hi ? hi : v;
        sval[i] = hi - lo != 0.0f ? (v - lo) / (hi - lo) : 0.0f;
    }
    m4_decomp(m, loc, &rot, size);
    for (int i = 0; i < 3; i++) {
        int src = c->map[i] >= 0 && c->map[i] < 3 ? c->map[i] : i;
        nv[i] = c->to_min[i] + sval[src] * (c->to_max[i] - c->to_min[i]);
    }
    if (c->to == 2) {
        for (int k = 0; k < 3; k++) size[k] = c->xmix == 2 ? size[k] * nv[k] : nv[k];
    } else if (c->to == 1) {
        M4 er = m4_euler_xyz(nv);
        if (c->xmix == 0) rot = er;
        else if (c->xmix == 3) rot = m4_mul(&er, &rot);
        else if (c->xmix == 4) rot = m4_mul(&rot, &er);
        else {
            float e[3];
            euler_of(&rot, e);
            for (int k = 0; k < 3; k++) e[k] += nv[k];
            rot = m4_euler_xyz(e);
        }
    } else
        for (int k = 0; k < 3; k++) loc[k] = c->xmix == 0 ? nv[k] : loc[k] + nv[k];
    return m4_compose(loc, &rot, size);
}

// (Q19) Track To, Locked Track, Limit Distance, Maintain Volume (the Tlender's rig_eval.c, Blender's)
static int basis_cross(int n, int m) { return n - m == 1 || n - m == -2 ? 1 : n - m == -1 || n - m == 2 ? -1 : 0; }
static M4 vectomat(const float vec[3], const float tup[3], int axis, int upflag, bool target_z) {
    float n[3] = { vec[0], vec[1], vec[2] };
    if (v3len(n) > 0.0f) v3norm(n);
    else n[0] = n[1] = 0.0f, n[2] = 1.0f;
    if (axis > 2) axis -= 3;
    else n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    float u[3] = { target_z ? tup[0] : 0.0f, target_z ? tup[1] : 0.0f, target_z ? tup[2] : 1.0f }, nn = v3dot(n, n);
    float k = nn > 0.0f ? v3dot(u, n) / nn : 0.0f, proj[3] = { u[0] - n[0] * k, u[1] - n[1] * k, u[2] - n[2] * k }, right[3];
    if (v3len(proj) > 0.0f) v3norm(proj);
    else proj[0] = 0, proj[1] = 1, proj[2] = 0;
    v3cross(proj, n, right);
    v3norm(right);
    M4 m = m4_id();
    if (axis != upflag) {
        int ri = 3 - axis - upflag;
        float bc = (float)basis_cross(axis, upflag), rs[3] = { right[0] * bc, right[1] * bc, right[2] * bc };
        m4_set_col(&m, ri, rs), m4_set_col(&m, upflag, proj), m4_set_col(&m, axis, n);
    }
    return m;
}
static M4 con_track_to(const TCon* c, const M4* m, const M4* t) {
    float sz[3], d[3] = { m->m[12] - t->m[12], m->m[13] - t->m[13], m->m[14] - t->m[14] }, tz[3];
    m4_scale_of(m, sz);
    m4_col(t, 2, tz);
    M4 tot = vectomat(d, tz, c->track, c->up < 3 ? c->up : 2, c->target_z != 0);
    for (int k = 0; k < 3; k++)
        for (int r = 0; r < 3; r++) tot.m[k * 4 + r] *= sz[k];
    tot.m[12] = m->m[12], tot.m[13] = m->m[13], tot.m[14] = m->m[14];
    return tot;
}
static M4 con_locked_track(const TCon* c, const M4* m, const M4* t) {
    int lock = c->up < 3 ? c->up : 2, track = c->track % 3;
    if (track == lock) return *m;
    float vec[3] = { t->m[12] - m->m[12], t->m[13] - m->m[13], t->m[14] - m->m[14] }, lax[3];
    m4_col(m, lock, lax);
    float ll = v3dot(lax, lax);
    if (ll <= 0.0f) return *m;
    float k = v3dot(vec, lax) / ll, tr[3] = { vec[0] - lax[0] * k, vec[1] - lax[1] * k, vec[2] - lax[2] * k };
    v3norm(tr);
    if (c->track >= 3) tr[0] = -tr[0], tr[1] = -tr[1], tr[2] = -tr[2];
    float sz[3], ln[3] = { lax[0], lax[1], lax[2] }, third[3];
    v3norm(ln);
    m4_scale_of(m, sz);
    M4 r = m4_id();
    m4_set_col(&r, lock, ln), m4_set_col(&r, track, tr);
    int th = 3 - lock - track;
    float a[3], b[3];
    m4_col(&r, (th + 1) % 3, a), m4_col(&r, (th + 2) % 3, b);
    v3cross(a, b, third);
    m4_set_col(&r, th, third);
    for (int q = 0; q < 3; q++)
        for (int w = 0; w < 3; w++) r.m[q * 4 + w] *= sz[q];
    r.m[12] = m->m[12], r.m[13] = m->m[13], r.m[14] = m->m[14];
    return r;
}
static M4 con_limit_distance(TCon* c, M4 m, const M4* t) {
    float d3[3] = { m.m[12] - t->m[12], m.m[13] - t->m[13], m.m[14] - t->m[14] }, dist = v3len(d3);
    if (c->dist == 0.0f) c->dist = dist; // (Blender keeps the first distance)
    bool clamp = c->dist_mode == 1 ? dist <= c->dist : c->dist_mode == 2 ? fabsf(dist - c->dist) > 1e-6f * fmaxf(1.0f, c->dist) : dist >= c->dist;
    if (!clamp || dist == 0.0f) return m;
    float sf = c->dist / dist;
    for (int k = 0; k < 3; k++) m.m[12 + k] = t->m[12 + k] + d3[k] * sf;
    return m;
}
static M4 con_maintain_volume(const TCon* c, M4 m) {
    float s[3];
    m4_scale_of(&m, s);
    int fa = c->vol_axis < 3 ? c->vol_axis : 1;
    float total = c->vol_mode == 1 ? s[fa] * s[fa] * s[fa] : c->vol_mode == 2 ? s[fa] : s[0] * s[1] * s[2];
    if (total == 0.0f) return m;
    float fac = sqrtf(fabsf((c->volume != 0.0f ? c->volume : 1.0f) / total));
    for (int k = 0; k < 3; k++)
        if (k != fa)
            for (int r = 0; r < 3; r++) m.m[k * 4 + r] *= fac;
    return m;
}

static void apply_constraint(const RigCtx* x, TArm* a, int i, TCon* c) {
    if (!c->enabled || c->influence <= 0.0f || c->type == TC_IK || c->type == TC_SPLINE_IK) return;
    M4 m = a->pose[i], t = m4_id(), res;
    bool has_target = c->type != TC_LIMIT_LOC && c->type != TC_LIMIT_ROT && c->type != TC_LIMIT_SCALE && c->type != TC_MAINTAIN_VOL;
    if (has_target && !target_matrix(x, c, c->target_space, &t)) return;
    M4 mo = bone_to_space(a, i, &a->world, &m, c->owner_space);
    bool world = false;
    switch (c->type) {
    case TC_COPY_LOC: res = con_copy_location(c, mo, &t); break;
    case TC_COPY_ROT: res = con_copy_rotation(c, &mo, &t); break;
    case TC_COPY_SCALE: res = con_copy_scale(c, &mo, &t); break;
    case TC_COPY_XF: res = con_copy_transforms(c, &mo, t); break;
    case TC_DAMPED: res = con_damped_track(c, &mo, &t); break;
    case TC_STRETCH: res = con_stretch_to(c, mo, &t, a->length[i]); break;
    case TC_LIMIT_LOC:
    case TC_LIMIT_ROT:
    case TC_LIMIT_SCALE: res = con_limit(c, mo); break;
    case TC_CHILD_OF: { // the result in world space whatever the owner's
        M4 wm = m4_mul(&a->world, &m);
        res = con_child_of(c, &t, &wm);
        world = true;
        break;
    }
    case TC_XFORM: res = con_transformation(c, &mo, &t); break;
    case TC_TRACK_TO: res = con_track_to(c, &mo, &t); break; // (Q19)
    case TC_LOCKED_TRACK: res = con_locked_track(c, &mo, &t); break;
    case TC_LIMIT_DIST: res = con_limit_distance(c, mo, &t); break;
    case TC_MAINTAIN_VOL: res = con_maintain_volume(c, mo); break;
    default: return;
    }
    M4 out;
    if (world) {
        M4 wi = m4_inv(&a->world);
        out = m4_mul(&wi, &res);
    } else
        out = bone_from_space(a, i, &a->world, &res, c->owner_space);
    a->pose[i] = blend_m4(&m, &out, c->influence > 1.0f ? 1.0f : c->influence);
}

#include "import_drivers.inc" // (Q19) the drivers, evaluated with the rig

// A bone's channels at the frame: the action's curves where it keys them, the file's pose elsewhere (the rotation kept:
// the Spline IK's twist; a connected bone's location ignored, Blender's BKE_pchan_to_mat4)
static void bone_channels(const RigCtx* x, TArm* a, int ai, int i, M4* out) {
    const TBone* b = &a->bones[i];
    float loc[3], q[4], s[3];
    memcpy(loc, b->ploc, sizeof loc), memcpy(q, b->pquat, sizeof q), memcpy(s, b->pscale, sizeof s);
    if (x->act && a->curve_bone)
        for (int c = 0; c < x->act->ncurves; c++) {
            if (a->curve_bone[c] != i) continue;
            const TCurve* cu = &x->act->curves[c];
            float v = curve_eval(cu, x->frame);
            if (cu->channel <= 2) loc[cu->channel] = v;
            else if (cu->channel <= 5) q[cu->channel - 2] = v;
            else if (cu->channel <= 8) s[cu->channel - 6] = v;
            else if (cu->channel == 9) q[0] = v;
        }
    drv_bone_override(x->f, ai, i, loc, q, s); // (Q19) the drivers' channels
    float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 1e-12f)
        for (int k = 0; k < 4; k++) q[k] /= ql;
    else q[0] = 1.0f, q[1] = q[2] = q[3] = 0.0f;
    memcpy(a->cquat[i], q, sizeof q);
    if (b->connected && b->parent >= 0) loc[0] = loc[1] = loc[2] = 0.0f;
    *out = chan_matrix(loc, q, s);
}

static void bone_fk(const RigCtx* x, TArm* a, int ai, int i) {
    M4 ch;
    bone_channels(x, a, ai, i, &ch);
    a->pose[i] = local_to_pose(a, i, &ch);
}

static void apply_stack(const RigCtx* x, TArm* a, int i) {
    TBone* b = &a->bones[i];
    float head[3] = { a->pose[i].m[12], a->pose[i].m[13], a->pose[i].m[14] };
    for (int k = 0; k < b->ncons; k++) apply_constraint(x, a, i, &b->cons[k]);
    if (b->ncons && b->connected && b->parent >= 0) memcpy(&a->pose[i].m[12], head, sizeof head); // (a chain stays joined)
}

static int chain_bones(const TArm* a, int eff, int chain, int* out) {
    int n = 0, j = eff, limit = chain > 0 ? chain : a->nbones;
    while (j >= 0 && n < limit && n < RIG_IK_MAX) out[n++] = j, j = a->bones[j].parent;
    for (int k = 0; k < n / 2; k++) {
        int t = out[k];
        out[k] = out[n - 1 - k], out[n - 1 - k] = t;
    }
    return n;
}
static bool is_solver(int type) { return type == TC_IK || type == TC_SPLINE_IK; }
// The bones of a solver's chain, root first, the owner last. The Standard IK counts the chain from its tip as Blender
// does: without "use tail" the owner is not in the solved chain, so it has one bone more (the owner follows it).
static int ik_chain(const TArm* a, int eff, const TCon* c, int* out) {
    if (c->type == TC_IK && c->ik_solver == 0 && !c->use_tail) return chain_bones(a, eff, c->chain > 0 ? c->chain + 1 : 0, out);
    return chain_bones(a, eff, c->chain, out);
}

// IK (the Tlender's FABRIK): the joints solved toward the target, the bend turned to face the pole, each bone turned
// about its head so its segment follows the solved one
static void solve_ik(const RigCtx* x, TArm* a, int ai, int eff, const TCon* c) {
    if (!c->enabled || c->influence <= 0.0f || c->tarm < 0 || c->tbone < 0) return;
    int ch[RIG_IK_MAX], m = chain_bones(a, eff, c->chain, ch);
    if (m < 1) return;
    for (int j = 1; j < m; j++) bone_fk(x, a, ai, ch[j]), apply_stack(x, a, ch[j]);
    int segs = c->use_tail ? m : m - 1;
    if (segs < 1) return;
    float J[RIG_IK_MAX + 1][3], P[RIG_IK_MAX + 1][3], L[RIG_IK_MAX], T[3];
    const TArm* ta = &x->f->arms[c->tarm];
    { // the target in this armature's space
        M4 tp = ta->pose[c->tbone];
        if (c->head_tail != 0.0f) {
            float tail[3], y[3] = { 0, ta->length[c->tbone], 0 };
            m4_point(&tp, y, tail);
            for (int k = 0; k < 3; k++) tp.m[12 + k] += (tail[k] - tp.m[12 + k]) * c->head_tail;
        }
        float w[3];
        m4_point(&ta->world, &tp.m[12], w);
        M4 inv = m4_inv(&a->world);
        m4_point(&inv, w, T);
    }
    for (int j = 0; j < m; j++) memcpy(J[j], &a->pose[ch[j]].m[12], 12);
    if (c->use_tail) {
        float y[3] = { 0, a->length[eff], 0 };
        m4_point(&a->pose[eff], y, J[m]);
    }
    float total = 0.0f;
    for (int j = 0; j < segs; j++) {
        float d[3];
        v3sub(J[j + 1], J[j], d);
        L[j] = v3len(d), total += L[j];
    }
    if (total < 1e-8f) return;
    float dr[3];
    v3sub(T, J[0], dr);
    float reach = v3len(dr), stretch = 1.0f;
    if (c->stretch && reach > total) {
        stretch = reach / total;
        for (int j = 0; j < segs; j++) L[j] *= stretch;
        total = reach;
    }
    memcpy(P, J, sizeof P[0] * (size_t)(segs + 1));
    if (reach >= total) {
        v3norm(dr);
        for (int j = 0; j < segs; j++)
            for (int k = 0; k < 3; k++) P[j + 1][k] = P[j][k] + dr[k] * L[j];
    } else {
        int iters = c->iterations > 0 ? (c->iterations < 64 ? c->iterations : 64) : 32;
        for (int it = 0; it < iters; it++) {
            memcpy(P[segs], T, 12);
            for (int j = segs - 1; j >= 0; j--) {
                float d[3];
                v3sub(P[j], P[j + 1], d);
                v3norm(d);
                for (int k = 0; k < 3; k++) P[j][k] = P[j + 1][k] + d[k] * L[j];
            }
            memcpy(P[0], J[0], 12);
            for (int j = 0; j < segs; j++) {
                float d[3];
                v3sub(P[j + 1], P[j], d);
                v3norm(d);
                for (int k = 0; k < 3; k++) P[j + 1][k] = P[j][k] + d[k] * L[j];
            }
            float e[3];
            v3sub(P[segs], T, e);
            if (v3len(e) < 1e-6f) break;
        }
    }
    if (c->tpole >= 0 && segs >= 2) { // the bend faces the pole (plus the pole angle)
        float pw[3], pole[3], axis[3], v[3], wv[3];
        m4_point(&ta->world, &ta->pose[c->tpole].m[12], pw);
        M4 inv = m4_inv(&a->world);
        m4_point(&inv, pw, pole);
        v3sub(P[segs], P[0], axis);
        if (v3len(axis) > 1e-8f) {
            v3norm(axis);
            v3sub(P[1], P[0], v), v3sub(pole, P[0], wv);
            float dv = v3dot(v, axis), dw = v3dot(wv, axis);
            for (int k = 0; k < 3; k++) v[k] -= axis[k] * dv, wv[k] -= axis[k] * dw;
            if (v3len(v) > 1e-6f && v3len(wv) > 1e-6f) {
                v3norm(v), v3norm(wv);
                float cr[3];
                v3cross(v, wv, cr);
                float ang = atan2f(v3dot(cr, axis), v3dot(v, wv)) + c->pole_angle;
                float q[4] = { cosf(ang * 0.5f), axis[0] * sinf(ang * 0.5f), axis[1] * sinf(ang * 0.5f), axis[2] * sinf(ang * 0.5f) };
                M4 rm = m4_quat(q);
                for (int j = 1; j < segs; j++) {
                    float d[3], o[3];
                    v3sub(P[j], P[0], d);
                    m4_dir(&rm, d, o);
                    for (int k = 0; k < 3; k++) P[j][k] = P[0][k] + o[k];
                }
            }
        }
    }
    float local_next[RIG_IK_MAX][3]; // each segment's far joint in its bone's space (the FK pose)
    for (int j = 0; j < segs; j++) {
        M4 inv = m4_inv(&a->pose[ch[j]]);
        m4_point(&inv, J[j + 1], local_next[j]);
    }
    float infl = c->influence > 1.0f ? 1.0f : c->influence, id[4] = { 1, 0, 0, 0 };
    for (int j = 0; j < m; j++) {
        if (j > 0) bone_fk(x, a, ai, ch[j]), apply_stack(x, a, ch[j]); // follows its solved parent
        if (j >= segs) continue;
        M4* bp = &a->pose[ch[j]];
        float head[3] = { bp->m[12], bp->m[13], bp->m[14] }, nw[3], now[3], want[3], q[4], qi[4];
        m4_point(bp, local_next[j], nw);
        v3sub(nw, head, now), v3sub(P[j + 1], head, want);
        quat_between(now, want, q);
        quat_slerp(id, q, infl, qi);
        *bp = rotate_about(bp, qi, head);
        if (stretch != 1.0f) {
            float s[3] = { 1.0f, 1.0f + (stretch - 1.0f) * infl, 1.0f }, z[3] = { 0, 0, 0 };
            M4 sm = m4_trs(z, &(M4){ { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }, s);
            *bp = m4_mul(bp, &sm);
        }
    }
}

// --- Blender's Standard IK (the Tlender's solve_ik_standard, ik_solver.c copied): the chain from its tip up, after the
// FK and the stacks of its bones, solved in the space of the root's parent with the root's head at the origin; the
// result as Blender applies it: pose = parent's new pose * the bone's offset from its parent before solving * the basis
// change (stretch as a uniform scale, the children compensating their parent's; non-uniform scale kept by axis) ---

static M4 normalized_axes(M4 m) { // Blender's normalize_m4: no orthogonalization
    for (int c = 0; c < 3; c++) {
        float v[3];
        m4_col(&m, c, v);
        float l = v3len(v);
        if (l > 0.0f) v[0] /= l, v[1] /= l, v[2] /= l, m4_set_col(&m, c, v);
    }
    return m;
}
static M4 rot3_transpose(const M4* m) { // the transpose of the 3x3 part (no translation)
    M4 r = m4_id();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[j * 4 + i] = m->m[i * 4 + j];
    return r;
}
static void m3d_from(const M4* m, double r[3][3]) { // 3x3 rows (the solver works in double)
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = m->m[j * 4 + i];
}
static void rot_quat(const M4* m, float q[4]) {
    M4 n = normalized_axes(*m);
    quat_of(&n, q);
}
// Blender's blend_m3_m3m3: rotation by quaternion, scale linear
static M4 blend_m3(const M4* dst, const M4* src, float t) {
    float qd[4], qs[4], q[4], sd[3], ss[3], s[3], z[3] = { 0, 0, 0 };
    rot_quat(dst, qd), rot_quat(src, qs);
    quat_slerp(qd, qs, t, q);
    m4_scale_of(dst, sd), m4_scale_of(src, ss);
    for (int k = 0; k < 3; k++) s[k] = sd[k] + (ss[k] - sd[k]) * t;
    M4 r = m4_quat(q);
    return m4_compose(z, &r, s);
}
static void set_col_len(M4* m, int c, float len) {
    float v[3];
    m4_col(m, c, v);
    v3norm(v);
    v[0] *= len, v[1] *= len, v[2] *= len;
    m4_set_col(m, c, v);
}
static void bone_tail(const TArm* a, int i, float o[3]) {
    float y[3] = { 0, a->length[i], 0 };
    m4_point(&a->pose[i], y, o);
}

static void solve_ik_standard(const RigCtx* x, TArm* a, int ai, int eff, const TCon* c) {
    if (!c->enabled || c->influence <= 0.0f || c->tarm < 0 || c->tbone < 0) return;
    int ch[RIG_IK_MAX], m = ik_chain(a, eff, c, ch);
    int segs = c->use_tail ? m : m - 1;
    if (segs < 1) return;
    for (int j = 1; j < m; j++) bone_fk(x, a, ai, ch[j]), apply_stack(x, a, ch[j]);
    M4 tm;
    if (!target_pose_matrix(x, a, ai, c, &tm)) return;
    // the tree's space
    const TBone* root = &a->bones[ch[0]];
    M4 rootmat = root->parent >= 0 ? normalized_axes(a->pose[root->parent]) : m4_id();
    memcpy(&rootmat.m[12], &a->pose[ch[0]].m[12], 12);
    M4 goalinv = m4_inv(&rootmat);
    IkBone ib[RIG_IK_MAX];
    M4 pre[RIG_IK_MAX];
    float lengths[RIG_IK_MAX];
    bool hasstretch = false;
    for (int j = 0; j < segs; j++) {
        const TBone* b = &a->bones[ch[j]];
        IkBone* s = &ib[j];
        memset(s, 0, sizeof *s);
        pre[j] = a->pose[ch[j]];
        s->parent = j - 1;
        unsigned dof = 0;
        for (int ax = 0; ax < 3; ax++)
            if (!(b->ik_lock & (1 << ax))) dof |= 1u << ax;
        if (c->stretch && b->ik_stretch > 0.0f) dof |= IKS_TRANS_Y, hasstretch = true;
        s->dof = dof;
        s->limit = (unsigned)b->ik_limit & 7u;
        for (int ax = 0; ax < 3; ax++) s->min[ax] = b->ik_min[ax], s->max[ax] = b->ik_max[ax], s->stiffness[ax] = b->ik_stiff[ax];
        s->stretch_stiffness = 1.0 - (double)(b->ik_stretch * b->ik_stretch);
        M4 rbone = a->pose[ch[j]], rpar = b->parent >= 0 ? a->pose[b->parent] : m4_id();
        float ycol[3];
        m4_col(&rbone, 1, ycol);
        lengths[j] = a->length[ch[j]] * v3len(ycol);
        s->length = lengths[j];
        rbone = normalized_axes(rbone), rpar = normalized_axes(rpar);
        rbone.m[12] = rbone.m[13] = rbone.m[14] = rpar.m[12] = rpar.m[13] = rpar.m[14] = 0.0f;
        M4 rest = a->rest[ch[j]], prest = b->parent >= 0 ? a->rest[b->parent] : m4_id();
        rest.m[12] = rest.m[13] = rest.m[14] = prest.m[12] = prest.m[13] = prest.m[14] = 0.0f;
        M4 np = normalized_axes(prest), tp = rot3_transpose(&np), nr = normalized_axes(rest), bone_mat = m4_mul(&tp, &nr);
        M4 ipar = m4_inv(&rpar), bt = rot3_transpose(&bone_mat), pr = m4_mul(&ipar, &rbone), basis = m4_mul(&bt, &pr);
        m3d_from(&bone_mat, s->rest);
        m3d_from(&basis, s->basis);
        if (j > 0 && b->parent >= 0) {
            float ptail[3], d[3], st[3];
            bone_tail(a, b->parent, ptail);
            v3sub(&a->pose[ch[j]].m[12], ptail, d);
            M4 ni = normalized_axes(ipar);
            m4_dir(&ni, d, st);
            for (int k = 0; k < 3; k++) s->start[k] = st[k];
        }
    }
    // the goal
    float infl = c->influence > 1.0f ? 1.0f : c->influence;
    IkGoal g;
    memset(&g, 0, sizeof g);
    g.tip = segs - 1;
    g.use_pos = c->ik_use_loc != 0, g.use_rot = c->ik_use_rot != 0;
    g.weight = c->ik_weight, g.orient_weight = c->ik_orient_weight;
    M4 goal = m4_mul(&goalinv, &tm);
    float gpos[3] = { goal.m[12], goal.m[13], goal.m[14] };
    M4 grot = normalized_axes(goal);
    bool resultblend = false;
    if (c->tpole >= 0) {
        const TArm* ta = &x->f->arms[c->tarm];
        float ph[3] = { ta->pose[c->tpole].m[12], ta->pose[c->tpole].m[13], ta->pose[c->tpole].m[14] }, pt[3], pole[3], gp[3];
        bone_tail(ta, c->tpole, pt);
        for (int k = 0; k < 3; k++) pole[k] = ph[k] + (pt[k] - ph[k]) * c->head_tail;
        if (c->tarm != ai) {
            float w[3];
            M4 inv = m4_inv(&a->world);
            m4_point(&ta->world, pole, w);
            m4_point(&inv, w, pole);
        }
        m4_point(&goalinv, pole, gp);
        g.use_pole = true;
        for (int k = 0; k < 3; k++) g.pole[k] = gp[k];
        g.pole_angle = c->pole_angle;
        resultblend = true;
    } else if (infl != 1.0f) { // the goal blended toward the tip's own place
        M4 end = a->pose[ch[segs - 1]];
        bone_tail(a, ch[segs - 1], &end.m[12]);
        M4 wp = m4_mul(&goalinv, &end);
        for (int k = 0; k < 3; k++) gpos[k] = gpos[k] * infl + wp.m[12 + k] * (1.0f - infl);
        float q1[4], q2[4], q[4];
        rot_quat(&grot, q1), rot_quat(&wp, q2);
        quat_slerp(q1, q2, 1.0f - infl, q);
        grot = m4_quat(q);
    }
    for (int k = 0; k < 3; k++) g.pos[k] = gpos[k];
    m3d_from(&grot, g.rot);
    ik_solve(ib, segs, &g, 1, c->iterations);
    // applied (Blender's make_dmats + where_is_ik_bone)
    M4 chan[RIG_IK_MAX];
    for (int j = 0; j < segs; j++) {
        const TBone* b = &a->bones[ch[j]];
        M4 par = j > 0 ? pre[j - 1] : b->parent >= 0 ? a->pose[b->parent] : m4_id(), pinv = m4_inv(&par);
        chan[j] = b->parent >= 0 ? m4_mul(&pinv, &pre[j]) : pre[j];
    }
    float stretch_of[RIG_IK_MAX];
    for (int j = 0; j < segs; j++) {
        const TBone* b = &a->bones[ch[j]];
        M4 ik = m4_id();
        for (int i = 0; i < 3; i++)
            for (int jj = 0; jj < 3; jj++) ik.m[jj * 4 + i] = (float)ib[j].basis_change[i][jj];
        if (hasstretch) {
            float parent_stretch = j > 0 ? stretch_of[j - 1] : 1.0f;
            stretch_of[j] = c->stretch && b->ik_stretch > 0.0f && lengths[j] != 0.0f ? ((float)ib[j].trans_change[1] + lengths[j]) / lengths[j] : 1.0f;
            float st = parent_stretch == 0.0f ? 1.0f : stretch_of[j] / parent_stretch;
            for (int k = 0; k < 12; k++)
                if (k % 4 != 3) ik.m[k] *= st;
        }
        if (resultblend && infl != 1.0f) {
            M4 id = m4_id();
            ik = blend_m3(&id, &ik, infl);
        }
        M4 p = b->parent >= 0 ? m4_mul(&a->pose[b->parent], &chan[j]) : chan[j];
        float sc[3], iks[3];
        m4_scale_of(&p, sc);
        // as if the bone had a uniform scale (the solver doesn't know non-uniform scale), then its own back
        set_col_len(&p, 0, sc[1]), set_col_len(&p, 2, sc[1]);
        p = m4_mul(&p, &ik);
        m4_scale_of(&ik, iks);
        set_col_len(&p, 0, sc[0] * iks[0]), set_col_len(&p, 2, sc[2] * iks[2]);
        a->pose[ch[j]] = p;
    }
    for (int j = segs; j < m; j++) bone_fk(x, a, ai, ch[j]), apply_stack(x, a, ch[j]); // the owner outside the chain follows it
}

// --- Spline IK (the Tlender's solve_spline): the chain laid along a cubic Bezier through the heads of the control
// bones (handles along each control's Y axis, or Catmull-Rom without them), each bone turned from its FK pose by the
// shortest rotation onto the curve (its twist kept), plus the controls' twist about their Y axes spread along the
// chain. Y scale: fit the curve, none or the original; X/Z: none, original, inverse or volume preserving ---

#define SPLINE_STEPS 16
#define SPLINE_PTS (SPLINE_STEPS * (RIG_LIST_MAX - 1) + 1)
static float twist_y(const float q_in[4]) { // the twist of a rotation about its Y (Blender's swing-twist split)
    float q[4];
    memcpy(q, q_in, sizeof q);
    if (q[0] < 0.0f)
        for (int k = 0; k < 4; k++) q[k] = -q[k];
    return 2.0f * atan2f(q[2], q[0]);
}
static void solve_spline(const RigCtx* x, TArm* a, int ai, int eff, const TCon* c) {
    if (!c->enabled || c->influence <= 0.0f || c->tarm < 0 || c->nlist < 2) return;
    int nc = c->nlist < RIG_LIST_MAX ? c->nlist : RIG_LIST_MAX;
    for (int j = 0; j < nc; j++)
        if (c->list[j] < 0) return;
    int ch[RIG_IK_MAX], m = chain_bones(a, eff, c->chain, ch);
    if (m < 1) return;
    // the controls in the owner's space
    const TArm* ta = &x->f->arms[c->tarm];
    M4 to_owner = m4_id();
    if (c->tarm != ai) {
        M4 inv = m4_inv(&a->world);
        to_owner = m4_mul(&inv, &ta->world);
    }
    float P[RIG_LIST_MAX][3], Y[RIG_LIST_MAX][3], tw[RIG_LIST_MAX];
    for (int j = 0; j < nc; j++) {
        M4 cm = m4_mul(&to_owner, &ta->pose[c->list[j]]);
        memcpy(P[j], &cm.m[12], 12);
        m4_col(&cm, 1, Y[j]);
        tw[j] = twist_y(ta->cquat[c->list[j]]);
    }
    // the handles
    float hin[RIG_LIST_MAX][3], hout[RIG_LIST_MAX][3];
    for (int j = 0; j < nc; j++) {
        float dir[3];
        if (c->list_ctrl_rot && v3len(Y[j]) > 1e-8f) memcpy(dir, Y[j], 12), v3norm(dir);
        else {
            if (j == 0) v3sub(P[1], P[0], dir);
            else if (j == nc - 1) v3sub(P[j], P[j - 1], dir);
            else v3sub(P[j + 1], P[j - 1], dir);
            if (v3len(dir) > 1e-8f) v3norm(dir);
            else dir[0] = dir[1] = 0.0f, dir[2] = 1.0f;
        }
        float d[3], lin = 0.0f, lout = 0.0f;
        if (j > 0) v3sub(P[j], P[j - 1], d), lin = v3len(d) / 3.0f;
        if (j < nc - 1) v3sub(P[j + 1], P[j], d), lout = v3len(d) / 3.0f;
        for (int k = 0; k < 3; k++) hin[j][k] = P[j][k] - dir[k] * lin, hout[j][k] = P[j][k] + dir[k] * lout;
    }
    // the arc length table
    float pts[SPLINE_PTS][3], arc[SPLINE_PTS], ctrl_arc[RIG_LIST_MAX];
    int np = 0;
    for (int sgm = 0; sgm < nc - 1; sgm++) {
        for (int i = sgm == 0 ? 0 : 1; i <= SPLINE_STEPS; i++) {
            float t = (float)i / SPLINE_STEPS, u = 1.0f - t, d[3];
            for (int k = 0; k < 3; k++)
                pts[np][k] = P[sgm][k] * u * u * u + hout[sgm][k] * 3.0f * u * u * t + hin[sgm + 1][k] * 3.0f * u * t * t + P[sgm + 1][k] * t * t * t;
            if (np) v3sub(pts[np], pts[np - 1], d);
            arc[np] = np ? arc[np - 1] + v3len(d) : 0.0f;
            np++;
        }
        ctrl_arc[sgm] = arc[sgm * SPLINE_STEPS];
    }
    ctrl_arc[nc - 1] = arc[np - 1];
    float curve_len = arc[np - 1], end_dir[3];
    if (curve_len < 1e-8f) return;
    v3sub(pts[np - 1], pts[np - 2], end_dir);
    v3norm(end_dir);
    // the joints along the curve
    float rest_len[RIG_IK_MAX], total = 0.0f, u[RIG_IK_MAX + 1], cum = 0.0f, J[RIG_IK_MAX + 1][3];
    for (int i = 0; i < m; i++) rest_len[i] = a->length[ch[i]], total += rest_len[i];
    if (total < 1e-8f) return;
    for (int i = 0; i <= m; i++) {
        u[i] = c->list_y == 1 ? (c->list_even ? (float)i / (float)m : cum / total) * curve_len : cum;
        if (i < m) cum += rest_len[i];
    }
    int seg = 0;
    for (int i = 0; i <= m; i++) {
        if (u[i] >= curve_len) {
            for (int k = 0; k < 3; k++) J[i][k] = pts[np - 1][k] + end_dir[k] * (u[i] - curve_len);
            continue;
        }
        while (seg < np - 2 && arc[seg + 1] < u[i]) seg++;
        float span = arc[seg + 1] - arc[seg], f = span > 0.0f ? (u[i] - arc[seg]) / span : 0.0f;
        for (int k = 0; k < 3; k++) J[i][k] = pts[seg][k] + (pts[seg + 1][k] - pts[seg][k]) * f;
    }
    float infl = c->influence > 1.0f ? 1.0f : c->influence, prev_twist = 0.0f;
    for (int i = 0; i < m; i++) {
        if (i > 0) bone_fk(x, a, ai, ch[i]), apply_stack(x, a, ch[i]); // follows its solved parent
        M4 fk = a->pose[ch[i]], r0;
        float loc0[3], sc0[3], d0[3], d[3], q[4];
        m4_decomp(&fk, loc0, &r0, sc0);
        m4_col(&r0, 1, d0);
        v3sub(J[i + 1], J[i], d);
        float len = v3len(d);
        if (len < 1e-9f) continue;
        for (int k = 0; k < 3; k++) d[k] /= len;
        quat_between(d0, d, q);
        M4 qm = m4_quat(q), rot = m4_mul(&qm, &r0);
        if (c->list_ctrl_rot) {
            float mid = 0.5f * (u[i] + u[i + 1]), twist = tw[nc - 1] - tw[0];
            for (int j = 0; j < nc - 1; j++)
                if (mid <= ctrl_arc[j + 1] || j == nc - 2) {
                    float span = ctrl_arc[j + 1] - ctrl_arc[j], f = span > 0.0f ? (mid - ctrl_arc[j]) / span : 0.0f;
                    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                    twist = tw[j] + (tw[j + 1] - tw[j]) * f - tw[0];
                    break;
                }
            float h = (twist - prev_twist) * 0.5f, tq[4] = { cosf(h), d[0] * sinf(h), d[1] * sinf(h), d[2] * sinf(h) };
            M4 tm = m4_quat(tq);
            rot = m4_mul(&tm, &rot);
            prev_twist = twist;
        }
        float sy = c->list_y == 1 ? (rest_len[i] > 1e-9f ? len / rest_len[i] : 1.0f) : c->list_y == 2 ? sc0[1] : 1.0f, sx = 1.0f, sz = 1.0f;
        if (c->list_xz == 1) sx = sc0[0], sz = sc0[2];
        else if (c->list_xz == 2) sx = sz = sy > 1e-6f ? 1.0f / sy : 1.0f;
        else if (c->list_xz == 3) sx = sz = sy > 1e-6f ? 1.0f / sqrtf(sy) : 1.0f;
        float sz3[3] = { sx, sy, sz };
        M4 res = m4_compose(J[i], &rot, sz3);
        a->pose[ch[i]] = blend_m4(&fk, &res, infl);
    }
}

// The units (the Tlender's rigarm_build): a bone alone, or every bone of the IK / Spline IK chains that share bones (a
// chain is solved at once); their members parents first, their solvers shallow chain roots first, the units ordered by
// parents, targets, poles and Spline IK controls in this armature (Kahn's algorithm; a cycle: the rest by index)
static int uf_root(int* uf, int v) {
    while (uf[v] != v) uf[v] = uf[uf[v]], v = uf[v];
    return v;
}
static void rig_units(TArm* a, int self) {
    int nb = a->nbones, n = nb ? nb : 1, nsol = 0, nu = 0, ne = 0, ecap = 0, maxd = 0;
    int *uf = malloc(sizeof(int) * (size_t)n), *depth = calloc((size_t)n, sizeof(int)), *unit_of = malloc(sizeof(int) * (size_t)n);
    int *root_unit = malloc(sizeof(int) * (size_t)n), *count = calloc((size_t)n + 1, sizeof(int)), *indeg = calloc((size_t)n, sizeof(int));
    for (int b = 0; b < nb; b++) ecap += 1 + a->bones[b].ncons * (2 + RIG_LIST_MAX);
    int(*edge)[2] = malloc(sizeof(int[2]) * (size_t)(ecap ? ecap : 1));
    a->mem = malloc(sizeof(int) * (size_t)n), a->mem_start = calloc((size_t)n + 2, sizeof(int)), a->uorder = malloc(sizeof(int) * (size_t)n);
    a->sol_start = calloc((size_t)n + 2, sizeof(int));
    if (!uf || !depth || !unit_of || !root_unit || !count || !indeg || !edge || !a->mem || !a->mem_start || !a->uorder || !a->sol_start) goto out;
    for (int b = 0; b < nb; b++) {
        uf[b] = b;
        for (int j = a->bones[b].parent; j >= 0 && depth[b] <= nb; j = a->bones[j].parent) depth[b]++;
        maxd = depth[b] > maxd ? depth[b] : maxd;
    }
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < a->bones[b].ncons && k < RIG_CON_MAX; k++) {
            const TCon* c = &a->bones[b].cons[k];
            if (!c->enabled || !is_solver(c->type)) continue;
            nsol++;
            int ch[RIG_IK_MAX], m = ik_chain(a, b, c, ch);
            for (int j = 1; j < m; j++) {
                int r0 = uf_root(uf, ch[0]), r1 = uf_root(uf, ch[j]);
                if (r0 != r1) uf[r1] = r0;
            }
        }
    for (int b = 0; b < nb; b++) root_unit[b] = -1;
    for (int b = 0; b < nb; b++) {
        int r = uf_root(uf, b);
        if (root_unit[r] < 0) root_unit[r] = nu++;
        unit_of[b] = root_unit[r];
        count[unit_of[b]]++;
    }
    for (int u = 0; u < nu; u++) a->mem_start[u + 1] = a->mem_start[u] + count[u];
    memset(count, 0, sizeof(int) * (size_t)(n + 1));
    for (int d = 0; d <= maxd; d++)
        for (int b = 0; b < nb; b++)
            if (depth[b] == d) a->mem[a->mem_start[unit_of[b]] + count[unit_of[b]]++] = b;
    a->sol = malloc(sizeof(int) * (size_t)(nsol ? nsol : 1));
    if (!a->sol) goto out;
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < a->bones[b].ncons && k < RIG_CON_MAX; k++)
            if (a->bones[b].cons[k].enabled && is_solver(a->bones[b].cons[k].type)) a->sol_start[unit_of[b] + 1]++;
    for (int u = 0; u < nu; u++) a->sol_start[u + 1] += a->sol_start[u];
    memset(count, 0, sizeof(int) * (size_t)(n + 1));
    for (int d = 0; d <= maxd; d++)
        for (int b = 0; b < nb; b++)
            for (int k = 0; k < a->bones[b].ncons && k < RIG_CON_MAX; k++) {
                const TCon* c = &a->bones[b].cons[k];
                if (!c->enabled || !is_solver(c->type)) continue;
                int ch[RIG_IK_MAX];
                ik_chain(a, b, c, ch);
                if (depth[ch[0]] != d) continue;
                a->sol[a->sol_start[unit_of[b]] + count[unit_of[b]]++] = b * RIG_CON_MAX + k;
            }
    // the order
#define UEDGE(from_bone)                                                                                                   \
    do {                                                                                                                   \
        int f_ = (from_bone);                                                                                              \
        if (f_ >= 0 && f_ < nb && unit_of[f_] != unit_of[b] && ne < ecap) edge[ne][0] = unit_of[f_], edge[ne][1] = unit_of[b], ne++; \
    } while (0)
    for (int b = 0; b < nb; b++) {
        UEDGE(a->bones[b].parent);
        for (int k = 0; k < a->bones[b].ncons; k++) {
            const TCon* c = &a->bones[b].cons[k];
            if (!c->enabled || c->tarm != self) continue;
            UEDGE(c->tbone);
            UEDGE(c->tpole);
            for (int j = 0; j < c->nlist && j < RIG_LIST_MAX; j++) UEDGE(c->list[j]);
        }
    }
#undef UEDGE
    for (int k = 0; k < ne; k++) indeg[edge[k][1]]++;
    uint8_t* placed = calloc((size_t)n, 1);
    int head = 0, tail = 0;
    for (int u = 0; u < nu; u++)
        if (!indeg[u]) a->uorder[tail++] = u;
    while (head < tail) {
        int v = a->uorder[head++];
        if (placed) placed[v] = 1;
        for (int k = 0; k < ne; k++)
            if (edge[k][0] == v && --indeg[edge[k][1]] == 0) a->uorder[tail++] = edge[k][1];
    }
    for (int u = 0; u < nu && tail < nu && placed; u++) // (a cycle: the rest by index)
        if (!placed[u] && indeg[u] > 0) a->uorder[tail++] = u, placed[u] = 1;
    free(placed);
    a->nunits = tail;
out:
    free(uf), free(depth), free(unit_of), free(root_unit), free(count), free(indeg), free(edge);
}

// Ready to evaluate: the rest matrices, the lengths, the offsets from the parents, the targets resolved, the units
static void rig_prepare(TFile* f) {
    for (int ai = 0; ai < f->narms; ai++) {
        TArm* a = &f->arms[ai];
        int n = a->nbones ? a->nbones : 1;
        a->rest = malloc(sizeof(M4) * (size_t)n), a->pose = malloc(sizeof(M4) * (size_t)n), a->rel = malloc(sizeof(M4) * (size_t)n);
        a->length = calloc((size_t)n, sizeof(float)), a->cquat = calloc((size_t)n, sizeof(float[4]));
        if (!a->rest || !a->pose || !a->rel || !a->length || !a->cquat) continue;
        for (int i = 0; i < a->nbones; i++) {
            const TBone* b = &a->bones[i];
            a->rest[i] = bone_rest(b->head, b->tail, b->roll);
            float d[3];
            v3sub(b->tail, b->head, d);
            a->length[i] = v3len(d);
            if (b->parent >= 0) {
                M4 pi = m4_inv(&a->rest[b->parent]);
                a->rel[i] = m4_mul(&pi, &a->rest[i]);
            } else
                a->rel[i] = m4_id();
        }
        a->obj = -1;
        for (int o = 0; o < f->nobjs; o++)
            if (f->objs[o].type == 3 && f->objs[o].arm == ai) {
                a->obj = o;
                break;
            }
        a->world = a->obj >= 0 ? obj_world(f, a->obj, 0) : m4_id();
    }
    for (int ai = 0; ai < f->narms; ai++) { // the targets: an armature by its object's name (none: this one), bones by name
        TArm* a = &f->arms[ai];
        for (int i = 0; i < a->nbones; i++)
            for (int k = 0; k < a->bones[i].ncons; k++) {
                TCon* c = &a->bones[i].cons[k];
                c->tarm = ai;
                if (c->target_obj[0]) {
                    c->tarm = -1;
                    for (int o = 0; o < f->nobjs; o++)
                        if (!strcmp(f->objs[o].name, c->target_obj) && f->objs[o].type == 3) c->tarm = f->objs[o].arm;
                    if (c->tarm >= f->narms) c->tarm = -1;
                }
                c->tbone = c->tarm >= 0 && c->target[0] ? bone_index(&f->arms[c->tarm], c->target) : -1;
                c->tpole = c->tarm >= 0 && c->pole[0] ? bone_index(&f->arms[c->tarm], c->pole) : -1;
                for (int j = 0; j < c->nlist; j++) c->list[j] = c->tarm >= 0 ? bone_index(&f->arms[c->tarm], c->list_names[j]) : -1;
            }
        rig_units(a, ai);
    }
}

// Every armature posed at a frame of an action (on the armatures whose bones it moves; the others hold the file's
// pose): the armatures a constraint reads first
static void rig_eval_pass(TFile* f, const TAction* act, float frame);
// (Q19) the drivers first (their bones' channels, the constraints' influences), the rig; the drivers that read the posed
// rig once more with it, and the rig again
static void rig_eval(TFile* f, const TAction* act, float frame) {
    drivers_eval(f, act, frame, false);
    rig_eval_pass(f, act, frame);
    bool posed = false;
    for (int i = 0; f->drv && i < f->drv->n && !posed; i++) posed = f->drv->d[i].posed && f->drv->d[i].kind != DK_NONE;
    if (posed) drivers_eval(f, act, frame, true), rig_eval_pass(f, act, frame);
}

static void rig_eval_pass(TFile* f, const TAction* act, float frame) {
    RigCtx x = { f, act, frame };
    uint8_t done[64] = { 0 };
    int na = f->narms < 64 ? f->narms : 64;
    for (int pass = 0; pass < na; pass++)
        for (int ai = 0; ai < na; ai++) {
            TArm* a = &f->arms[ai];
            if (done[ai] || !a->pose || !a->cquat || !a->uorder || !a->sol) continue;
            bool ready = true; // the other armatures it reads are done (or wait for it: a cycle goes by index)
            for (int i = 0; i < a->nbones && ready; i++)
                for (int k = 0; k < a->bones[i].ncons; k++) {
                    int t = a->bones[i].cons[k].tarm;
                    if (t >= 0 && t != ai && t < na && !done[t] && pass < na - 1) ready = false;
                }
            if (!ready) continue;
            for (int o = 0; o < a->nunits; o++) { // a unit: its bones' FK and constraints, then its chains
                int u = a->uorder[o];
                for (int k = a->mem_start[u]; k < a->mem_start[u + 1]; k++) bone_fk(&x, a, ai, a->mem[k]), apply_stack(&x, a, a->mem[k]);
                for (int k = a->sol_start[u]; k < a->sol_start[u + 1]; k++) {
                    int bone = a->sol[k] / RIG_CON_MAX;
                    const TCon* c = &a->bones[bone].cons[a->sol[k] % RIG_CON_MAX];
                    if (c->type == TC_IK && c->ik_solver == 0) solve_ik_standard(&x, a, ai, bone, c);
                    else if (c->type == TC_IK) solve_ik(&x, a, ai, bone, c);
                    else solve_spline(&x, a, ai, bone, c);
                }
            }
            done[ai] = 1;
        }
}

// The armatures an action moves: a curve per bone mapped once (-1: not this armature's)
static int rig_bind_action(TFile* f, const TAction* act) {
    int hit = 0;
    for (int ai = 0; ai < f->narms; ai++) {
        TArm* a = &f->arms[ai];
        free(a->curve_bone);
        a->curve_bone = act ? malloc(sizeof(int) * (size_t)(act->ncurves ? act->ncurves : 1)) : NULL;
        int n = 0;
        for (int c = 0; act && a->curve_bone && c < act->ncurves; c++) {
            a->curve_bone[c] = act->curves[c].bone[0] ? bone_index(a, act->curves[c].bone) : -1;
            n += a->curve_bone[c] >= 0;
        }
        if (!n) free(a->curve_bone), a->curve_bone = NULL;
        else hit++;
    }
    return hit;
}

// Whether armature ai reads armature t (directly or through others)
static bool rig_reads(const TFile* f, int ai, int t, int depth) {
    if (ai == t) return true;
    if (depth > 8) return false;
    const TArm* a = &f->arms[ai];
    for (int i = 0; i < a->nbones; i++)
        for (int k = 0; k < a->bones[i].ncons; k++) {
            int u = a->bones[i].cons[k].tarm;
            if (u >= 0 && u != ai && rig_reads(f, u, t, depth + 1)) return true;
        }
    return false;
}

// --- A Skeletal Mesh per armature: every mesh its Armature modifier deforms, a part per bone and material (their
// faces in the bone's space), the actions that move it (directly or through a control rig it reads) baked per frame ---

typedef struct SkSrc {
    int obj, ride; // ride: the bone it follows whole (Bone parent, no Armature modifier); -1: weighted by its groups
    M4 world;
    float* vn;  // smooth normals (Blender's space)
    int* fbone; // per face: its bone (the strongest group of most of its corners)
    int* fmat;  // per face: the file's material (-1: none)
} SkSrc;

// The bone a mesh object rides rigidly when no Armature modifier bends it (#164: a robot, a capsule, a machine): parented
// to a bone of an armature object (Blender's Bone parent), itself or through the objects it hangs from (a piece on a
// piece) -> the bone and *arm_obj; -1: none
static int rides_bone(const TFile* f, int oi, int* arm_obj) {
    for (int d = 0, i = oi; d < 32 && i >= 0 && i < f->nobjs; d++) {
        const TObj* o = &f->objs[i];
        if (o->parent < 0 || o->parent >= f->nobjs || o->parent == i) return -1;
        const TObj* p = &f->objs[o->parent];
        if (p->type == 3) { // the armature: a bone of it, or the armature object itself (no bone)
            int b = o->parent_type == 1 && p->arm >= 0 && p->arm < f->narms ? arm_bone_named(&f->arms[p->arm], o->parent_bone) : -1;
            if (b >= 0 && arm_obj) *arm_obj = o->parent;
            return b;
        }
        i = o->parent;
    }
    return -1;
}

// A mesh object deformed by this armature with vertex groups that name its bones
static bool skinned_by(const TFile* f, const TObj* o, int arm_obj) {
    if (o->type != 0 || o->mesh < 0 || o->mesh >= f->nmeshes || o->deform != arm_obj || o->hidden) return false;
    const TObj* ao = &f->objs[arm_obj];
    if (ao->type != 3 || ao->arm < 0 || ao->arm >= f->narms) return false;
    const TMesh* me = &f->meshes[o->mesh];
    if (!me->nf || !me->ngroups || !me->gweight) return false;
    for (uint32_t g = 0; g < me->ngroups; g++)
        if (bone_index(&f->arms[ao->arm], me->gname[g]) >= 0) return true;
    return false;
}

// A part of this armature's Skeletal Mesh: bent by it (its Armature modifier, 1) or riding one of its bones (2); 0: not
static int sk_source(const TFile* f, int oi, int arm_obj, int* bone) {
    const TObj* o = &f->objs[oi];
    if (o->type != 0 || o->mesh < 0 || o->mesh >= f->nmeshes || o->hidden || !f->meshes[o->mesh].nf) return 0;
    if (o->deform >= 0 && o->deform < f->nobjs) return skinned_by(f, o, arm_obj) ? 1 : 0; // (an Armature modifier decides)
    int a = -1, b = rides_bone(f, oi, &a);
    if (b < 0 || a != arm_obj || f->objs[a].arm < 0 || f->objs[a].arm >= f->narms) return 0;
    if (bone) *bone = b;
    return 2;
}

// The Skeletal Mesh an object goes to (its armature object), -1: none
static int sk_owner(const TFile* f, int oi) {
    const TObj* o = &f->objs[oi];
    if (o->deform >= 0 && o->deform < f->nobjs) return skinned_by(f, o, o->deform) ? o->deform : -1;
    int a = -1;
    return rides_bone(f, oi, &a) >= 0 && sk_source(f, oi, a, NULL) ? a : -1;
}

static float* smooth_normals(const TMesh* me) {
    float* vn = calloc((size_t)me->nv * 3 + 3, sizeof(float));
    if (!vn) return NULL;
    for (uint32_t k = 0; k < me->nf; k++) {
        uint32_t s = me->fstart[k], n = me->fsize[k];
        for (uint32_t c = 1; c + 1 < n; c++) {
            const float* a = &me->co[me->loops[s] * 3];
            const float* b = &me->co[me->loops[s + c] * 3];
            const float* d = &me->co[me->loops[s + c + 1] * 3];
            float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] }, cr[3];
            v3cross(e1, e2, cr);
            for (int q = 0; q < 3; q++)
                vn[me->loops[s] * 3 + q] += cr[q], vn[me->loops[s + c] * 3 + q] += cr[q], vn[me->loops[s + c + 1] * 3 + q] += cr[q];
        }
    }
    return vn;
}

// A mesh's shape keys as morph targets (its vertices from base on): each key's offset (by the matrix m, Blender's world or
// the identity, then the Engine's space) and the normals' change (the smooth normals of the mesh moved by the whole key,
// less the mesh's: a vertex whose neighbor moved turns too), mirrored as the corners' are
static void morph_from_keys(MorphBuild* b, const TMesh* me, uint32_t base, const M4* m, const float* vn) {
    if (!me->nkeys || !me->kdelta || !vn) return;
    TMesh mv = *me;
    mv.co = malloc(sizeof(float) * ((size_t)me->nv * 3 + 3));
    if (!mv.co) return;
    for (uint32_t k = 0; k < me->nkeys; k++) {
        int t = morph_target(b, me->kname[k], me->kvalue[k]);
        if (t < 0) continue;
        for (uint32_t v = 0; v < me->nv; v++)
            for (int c = 0; c < 3; c++) mv.co[v * 3 + c] = me->co[v * 3 + c] + me->kdelta[((size_t)v * me->nkeys + k) * 3 + c];
        float* vk = smooth_normals(&mv);
        for (uint32_t v = 0; vk && v < me->nv; v++) {
            float d[3], dw[3], dp[3], n0[3], n1[3], dn[3];
            memcpy(d, &me->kdelta[((size_t)v * me->nkeys + k) * 3], 12);
            m4_dir(m, d, dw);
            point_to_engine(dw, dp);
            m4_dir(m, &vn[v * 3], n0), m4_dir(m, &vk[v * 3], n1);
            n0[1] = -n0[1], n1[1] = -n1[1];
            float l0 = v3len(n0), l1 = v3len(n1);
            for (int c = 0; c < 3; c++) dn[c] = l0 > 1e-12f && l1 > 1e-12f ? n1[c] / l1 - n0[c] / l0 : 0.0f;
            morph_add(b, base + v, t, dp, dn);
        }
        free(vk);
    }
    free(mv.co);
}

static void import_skeletal(TFile* f, int arm_obj, const uint32_t* mat_hash, const char* folder_disk, const char* folder_vpath,
                            World* w, TlendReport* r) {
    TObj* ao = &f->objs[arm_obj];
    TArm* arm = &f->arms[ao->arm];
    int ai = ao->arm;
    int nsrc = 0;
    for (int oi = 0; oi < f->nobjs; oi++) nsrc += sk_source(f, oi, arm_obj, NULL) != 0;
    if (!nsrc) return;
    if (arm->nbones > ANIM_MAX_BONES) {
        r->skipped_rigs++;
        return;
    }
    SkSrc* src = calloc((size_t)nsrc, sizeof(SkSrc));
    int nm = f->nmats + 1, nb = arm->nbones;
    uint32_t* bucket = calloc((size_t)nb * (size_t)nm, sizeof(uint32_t)); // faces per (bone, material + 1)
    int* part_of = malloc(sizeof(int) * (size_t)nb * (size_t)nm);
    char path[1100], vpath[1100], name[200];
    if (!src || !bucket || !part_of) goto done;
    nsrc = 0;
    for (int oi = 0; oi < f->nobjs; oi++) {
        TObj* o = &f->objs[oi];
        int ride = -1, kind = sk_source(f, oi, arm_obj, &ride);
        if (!kind) continue;
        TMesh* me = &f->meshes[o->mesh];
        SkSrc* sc = &src[nsrc++];
        sc->obj = oi, sc->world = obj_world(f, oi, 0), sc->vn = smooth_normals(me), sc->ride = kind == 2 ? ride : -1;
        if (kind == 2) { // riding a bone: where it is with the bone at rest (the bind), the file's pose taken back out
            M4 rb = bone_arm_matrix(arm, ride, false, 0), pb = bone_arm_matrix(arm, ride, true, 0), pbi = m4_inv(&pb);
            M4 aw = obj_world(f, arm_obj, 0), awi = m4_inv(&aw);
            M4 t0 = m4_mul(&aw, &rb), t1 = m4_mul(&t0, &pbi), t2 = m4_mul(&t1, &awi);
            sc->world = m4_mul(&t2, &sc->world);
        }
        sc->fbone = malloc(sizeof(int) * me->nf), sc->fmat = malloc(sizeof(int) * me->nf);
        int* gbone = calloc(me->ngroups + 1, sizeof(int));
        int* vbone = malloc(sizeof(int) * me->nv);
        if (!sc->vn || !sc->fbone || !sc->fmat || !gbone || !vbone) {
            free(gbone), free(vbone);
            goto done;
        }
        for (uint32_t g = 0; g < me->ngroups; g++) gbone[g] = kind == 2 ? -1 : bone_index(arm, me->gname[g]);
        for (uint32_t v = 0; v < me->nv; v++) {
            float best = 0.0f;
            vbone[v] = sc->ride;
            for (uint32_t g = 0; sc->ride < 0 && g < me->ngroups; g++) {
                float wv = me->gweight[(size_t)g * me->nv + v];
                if (wv > best && gbone[g] >= 0) best = wv, vbone[v] = gbone[g];
            }
        }
        for (uint32_t k = 0; k < me->nf; k++) {
            int best = -1, bestc = 0;
            for (uint32_t c = 0; c < me->fsize[k]; c++) {
                int b = vbone[me->loops[me->fstart[k] + c]], cnt = 0;
                for (uint32_t d = 0; d < me->fsize[k]; d++) cnt += vbone[me->loops[me->fstart[k] + d]] == b;
                if (b >= 0 && cnt > bestc) bestc = cnt, best = b;
            }
            sc->fbone[k] = best >= 0 ? best : 0;
            int slot = me->fmat[k] < o->nslots ? o->slots[me->fmat[k]] : -1;
            sc->fmat[k] = slot >= 0 && slot < f->nmats ? slot : -1;
            bucket[(size_t)sc->fbone[k] * (size_t)nm + (size_t)(sc->fmat[k] + 1)] += me->fsize[k];
        }
        free(gbone), free(vbone);
    }
    // the parts: a (bone, material) each; over the Skeletal Mesh's room, a bone's faces go with its main material,
    // then the smallest bones ride their nearest ancestor that has a part
    int nparts = 0;
    for (int k = 0; k < nb * nm; k++) nparts += bucket[k] != 0;
    if (nparts > ANIM_MAX_PARTS)
        for (int b = 0; b < nb; b++) {
            int best = 0;
            for (int m = 1; m < nm; m++)
                if (bucket[b * nm + m] > bucket[b * nm + best]) best = m;
            for (int m = 0; m < nm; m++)
                if (m != best) bucket[b * nm + best] += bucket[b * nm + m], bucket[b * nm + m] = 0;
            for (int s2 = 0; s2 < nsrc; s2++) // (the faces follow)
                for (uint32_t k = 0; k < f->meshes[f->objs[src[s2].obj].mesh].nf; k++)
                    if (src[s2].fbone[k] == b) src[s2].fmat[k] = best - 1;
        }
    for (;;) {
        nparts = 0;
        int small = -1;
        uint32_t small_n = 0;
        for (int b = 0; b < nb; b++) {
            uint32_t tot = 0;
            for (int m = 0; m < nm; m++) tot += bucket[b * nm + m], nparts += bucket[b * nm + m] != 0;
            if (tot && arm->bones[b].parent >= 0 && (small < 0 || tot < small_n)) small = b, small_n = tot;
        }
        if (nparts <= ANIM_MAX_PARTS || small < 0) break;
        int p = arm->bones[small].parent;
        for (int m = 0; m < nm; m++) bucket[p * nm + m] += bucket[small * nm + m], bucket[small * nm + m] = 0;
        for (int s2 = 0; s2 < nsrc; s2++)
            for (uint32_t k = 0; k < f->meshes[f->objs[src[s2].obj].mesh].nf; k++)
                if (src[s2].fbone[k] == small) src[s2].fbone[k] = p;
    }
    SkelMesh* sk = calloc(1, sizeof *sk);
    M4* rest = malloc(sizeof(M4) * (size_t)nb);
    size_t tri_cap = 0;
    for (int s2 = 0; s2 < nsrc; s2++) {
        const TMesh* me = &f->meshes[f->objs[src[s2].obj].mesh];
        for (uint32_t k = 0; k < me->nf; k++) tri_cap += me->fsize[k] >= 3 ? me->fsize[k] - 2 : 0;
    }
    TriOut* tris = malloc(sizeof(TriOut) * (tri_cap ? tri_cap : 1));
    if (!sk || !rest || !tris) {
        free(sk), free(rest), free(tris);
        goto done;
    }
    write_name(name, sizeof name, "SK_", ao->name, NULL);
    snprintf(sk->path, sizeof sk->path, "%s/%s.rskm", folder_disk, name);
    snprintf(sk->name, sizeof sk->name, "%s", name);
    float skmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, skmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX }; // (its skins at rest: the log's bounds)
    bones_world(arm, &arm->world, NULL, rest);
    sk->nbones = (uint32_t)nb;
    for (int b = 0; b < nb; b++) {
        AnimBone* ab = &sk->bones[b];
        snprintf(ab->name, sizeof ab->name, "%s", arm->bones[b].name);
        ab->parent = (int16_t)arm->bones[b].parent;
        local_xf(ab->parent >= 0 ? &rest[ab->parent] : NULL, &rest[b], &ab->bind);
    }
    char mdir[950];
    snprintf(mdir, sizeof mdir, "%s/Meshes", folder_disk);
    re_make_dirs(mdir);
    // a skin per material: every face of the meshes it deforms, at the bind (the Engine's space), each corner weighted to
    // its vertex's 4 strongest bones (the GPU bends them, render.c): the dress and the layers of cloth move as one, as in
    // the Tlender (the parts cut by bone opened seams and crossed layers)
    float(*skw)[2] = malloc(sizeof(float[2]) * (tri_cap ? tri_cap : 1) * 3);
    // the meshes' shape keys: the Skeletal Mesh's morph targets (each source's vertices numbered from vbase)
    MorphBuild morph = { 0 };
    uint32_t* vbase = calloc((size_t)nsrc + 1, sizeof(uint32_t));
    for (int s2 = 0; vbase && s2 < nsrc; s2++) {
        const TMesh* me = &f->meshes[f->objs[src[s2].obj].mesh];
        vbase[s2] = morph.nsrc, morph.nsrc += me->nv;
        morph_from_keys(&morph, me, vbase[s2], &src[s2].world, src[s2].vn);
    }
    bool morphs = vbase && morph_finish(&morph);
    uint32_t* csrc = morphs ? malloc(sizeof(uint32_t) * (tri_cap ? tri_cap : 1) * 3) : NULL;
    if (morphs && csrc) morph_to_skel(&morph, sk);
    for (int m = 0; skw && m < nm && sk->nskins < ANIM_MAX_SKINS; m++) {
        uint32_t nt = 0;
        for (int s2 = 0; s2 < nsrc; s2++) {
            const SkSrc* sc = &src[s2];
            const TObj* o = &f->objs[sc->obj];
            const TMesh* me = &f->meshes[o->mesh];
            SkinW* vw = calloc(me->nv ? me->nv : 1, sizeof(SkinW));
            if (!vw) continue;
            for (uint32_t v = 0; sc->ride >= 0 && v < me->nv; v++) skin_add(&vw[v], sc->ride, 1.0f);
            for (uint32_t g = 0; sc->ride < 0 && g < me->ngroups; g++) {
                int bone = bone_index(arm, me->gname[g]);
                if (bone < 0) continue;
                for (uint32_t v = 0; v < me->nv; v++) skin_add(&vw[v], bone, me->gweight[(size_t)g * me->nv + v]);
            }
            for (uint32_t k = 0; k < me->nf; k++) {
                int slot = me->fmat[k] < o->nslots ? o->slots[me->fmat[k]] : -1;
                int fm = slot >= 0 && slot < f->nmats ? slot : -1;
                if (fm != m - 1) continue;
                uint32_t s = me->fstart[k], n = me->fsize[k];
                for (uint32_t c = 1; c + 1 < n && nt < tri_cap; c++) {
                    TriOut* t = &tris[nt];
                    uint32_t cs[3] = { s, s + c + 1, s + c }; // (mirrored: the winding reversed)
                    for (int q = 0; q < 3; q++) {
                        uint32_t vi = me->loops[cs[q]];
                        float pb[3], nb3[3], nw[3];
                        m4_point(&sc->world, &me->co[vi * 3], pb);
                        point_to_engine(pb, t->p[q]);
                        m4_dir(&sc->world, &sc->vn[vi * 3], nb3); // Blender's world, mirrored
                        nw[0] = nb3[0], nw[1] = -nb3[1], nw[2] = nb3[2];
                        float l = v3len(nw);
                        for (int z = 0; z < 3; z++) t->n[q][z] = l > 1e-12f ? nw[z] / l : 0.0f;
                        t->uv[q][0] = me->uv[cs[q] * 2], t->uv[q][1] = me->uv[cs[q] * 2 + 1];
                        skin_pack(&vw[vi], sc->fbone[k], skw[nt * 3 + q]);
                        if (csrc) csrc[nt * 3 + q] = vbase[s2] + vi;
                    }
                    nt++;
                }
            }
            free(vw);
        }
        if (!nt) continue;
        char pname[200];
        write_name(pname, sizeof pname, "SKS_", ao->name, NULL);
        size_t pl = strlen(pname);
        snprintf(pname + pl, sizeof pname - pl, "_%d", m);
        snprintf(path, sizeof path, "%s/%s.rmesh", mdir, pname);
        if (!write_skin_rmesh(path, tris, (const float(*)[2])skw, nt, csrc, csrc ? &morph.m : NULL)) continue;
        for (uint32_t t = 0; t < nt; t++)
            for (int c = 0; c < 3; c++)
                for (int q = 0; q < 3; q++) skmn[q] = fminf(skmn[q], tris[t].p[c][q]), skmx[q] = fmaxf(skmx[q], tris[t].p[c][q]);
        snprintf(vpath, sizeof vpath, "%s/Meshes/%s.rmesh", folder_vpath, pname);
        SkSkin* k = &sk->skins[sk->nskins++];
        memset(k, 0, sizeof *k);
        k->mesh = render_mesh_id_of(vpath);
        k->color[0] = k->color[1] = k->color[2] = 0.7f, k->roughness = 0.5f;
        if (m > 0 && mat_hash) k->material = mat_hash[m - 1];
        r->meshes++;
    }
    if (csrc) r->morphs += sk->nmorphs;
    free(skw), free(csrc), free(vbase);
    morph_build_free(&morph);
    sk_stamp(sk);
    anim_mesh_save(sk);
    r->skeletal++;
    if (sk->nskins) // (the rest pose's place: a piece on a bone at its tail, or by its deformation with Relative Parenting)
        printf("[RealEngine] Skeletal Mesh %s: %u bones, %u skins, rest bounds (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f) cm\n", sk->name, sk->nbones,
               sk->nskins, skmn[0], skmn[1], skmn[2], skmx[0], skmx[1], skmx[2]);
    // the actions: the ones the armature objects it reads play (its control rig's), and the ones that key its bones
    uint32_t first_anim = 0;
    for (int ac_i = 0; ac_i < f->nactions; ac_i++) {
        const TAction* ac = &f->actions[ac_i];
        bool use = false;
        for (int o = 0; o < f->nobjs && !use; o++)
            if (f->objs[o].type == 3 && f->objs[o].action == ac_i && f->objs[o].arm >= 0 && f->objs[o].arm < f->narms &&
                rig_reads(f, ai, f->objs[o].arm, 0))
                use = true;
        for (int c = 0; c < ac->ncurves && !use; c++) use = bone_index(arm, ac->curves[c].bone) >= 0;
        if (!use || !rig_bind_action(f, ac)) continue;
        float f0 = 1e30f, f1 = -1e30f;
        for (int c = 0; c < ac->ncurves; c++)
            for (int k = 0; k < ac->curves[c].nkeys; k++) f0 = fminf(f0, ac->curves[c].keys[k].f), f1 = fmaxf(f1, ac->curves[c].keys[k].f);
        if (f1 <= f0) continue;
        uint32_t nframes = (uint32_t)(f1 - f0) + 1;
        if (nframes > 20000) nframes = 20000;
        AnimSeq* sq = calloc(1, sizeof *sq);
        float* rot = calloc((size_t)nframes * (size_t)nb * 4, sizeof(float));
        float* pos = calloc((size_t)nframes * (size_t)nb * 3, sizeof(float));
        M4* wb = malloc(sizeof(M4) * (size_t)nb);
        if (sq && rot && pos && wb) {
            char an[200], adir[950];
            write_name(an, sizeof an, "A_", ac->name, NULL);
            snprintf(sq->path, sizeof sq->path, "%s/Animations/%s.ranim", folder_disk, an);
            snprintf(adir, sizeof adir, "%s/Animations", folder_disk);
            re_make_dirs(adir);
            snprintf(sq->name, sizeof sq->name, "%s", an);
            snprintf(sq->skeleton, sizeof sq->skeleton, "%s", sk->name);
            sq->fps = f->fps, sq->nframes = nframes, sq->ntracks = (uint32_t)nb, sq->loop = true;
            sq->rot = rot, sq->pos = pos;
            sq->length = (float)(nframes - 1) / f->fps;
            for (int b = 0; b < nb; b++) snprintf(sq->track_bone[b], ANIM_NAME, "%s", arm->bones[b].name);
            // (Q19) the shape keys of this mesh that drivers move: a morph curve each (their values per frame)
            int nkd = 0, kdrv[64];
            float* kv = NULL;
            for (int i = 0; f->drv && i < f->drv->n && nkd < 64; i++) {
                const TDriver* d = &f->drv->d[i];
                if (d->kind == DK_KEY && anim_morph_index(sk, f->meshes[d->mesh].kname[d->key]) >= 0) kdrv[nkd++] = i;
            }
            if (nkd) kv = malloc(sizeof(float) * (size_t)nframes * ((size_t)nkd + 1));
            for (uint32_t fr = 0; fr < nframes; fr++) {
                rig_eval(f, ac, f0 + (float)fr);
                if (kv) {
                    kv[fr] = (float)fr / f->fps;
                    for (int q = 0; q < nkd; q++) kv[(size_t)(q + 1) * nframes + fr] = f->drv->d[kdrv[q]].value;
                }
                for (int b = 0; b < nb; b++) {
                    M4 t = m4_mul(&arm->world, &arm->pose[b]);
                    wb[b] = m4_to_engine(&t);
                }
                for (int b = 0; b < nb; b++) {
                    AnimXf x;
                    int pb = arm->bones[b].parent;
                    local_xf(pb >= 0 ? &wb[pb] : NULL, &wb[b], &x);
                    memcpy(&rot[((size_t)fr * (size_t)nb + (size_t)b) * 4], x.q, 16);
                    memcpy(&pos[((size_t)fr * (size_t)nb + (size_t)b) * 3], x.t, 12);
                }
            }
            for (int q = 0; kv && q < nkd; q++) { // (a curve only where the value moves)
                const float* v = &kv[(size_t)(q + 1) * nframes];
                float lo = v[0], hi = v[0];
                for (uint32_t fr = 1; fr < nframes; fr++) lo = fminf(lo, v[fr]), hi = fmaxf(hi, v[fr]);
                const TDriver* d = &f->drv->d[kdrv[q]];
                if (hi - lo > 1e-5f) anim_curve_set(sq, f->meshes[d->mesh].kname[d->key], kv, v, nframes);
            }
            free(kv);
            // (#164) the morph targets' weights the action keys (the .rasset's MWGT): a curve each, sampled per frame
            float* mv = ac->nmcurves ? malloc(sizeof(float) * (size_t)nframes * 2) : NULL;
            for (int q = 0; mv && q < ac->nmcurves; q++) {
                const TCurve* cu = &ac->mcurves[q];
                if (anim_morph_index(sk, cu->bone) < 0) continue;
                for (uint32_t fr = 0; fr < nframes; fr++) mv[fr] = (float)fr / f->fps, mv[nframes + fr] = curve_eval(cu, f0 + (float)fr);
                anim_curve_set(sq, cu->bone, mv, mv + nframes, nframes);
            }
            free(mv);
            if (anim_seq_save(sq)) {
                r->animations++;
                snprintf(vpath, sizeof vpath, "%s/Animations/%s.ranim", folder_vpath, an);
                if (!first_anim || f->objs[arm_obj].action == ac_i) first_anim = vhash(vpath);
            }
        }
        if (sq) free(sq->curve_keys);
        free(rot), free(pos), free(wb), free(sq);
    }
    rig_bind_action(f, NULL);
    // the actor: the Skeletal Mesh where the armature is (the bones carry its transform), playing its first animation
    if (w) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
        if (e != ENTITY_NONE) {
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", ao->name);
            CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
            t->flags = MOBILITY_MOVABLE;
            CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
            snprintf(vpath, sizeof vpath, "%s/%s.rskm", folder_vpath, name);
            c->mesh = vhash(vpath);
            c->anim = first_anim, c->play_rate = 1.0f, c->loop = 1;
            r->actors++;
        }
    }
    free(sk), free(rest), free(tris);
done:
    for (int s2 = 0; src && s2 < nsrc; s2++) free(src[s2].vn), free(src[s2].fbone), free(src[s2].fmat);
    free(src), free(bucket), free(part_of);
}

static bool tfile_assets(TFile f, const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);

#include "import_mods.inc" // (Q19) the modifier stacks

// (2026-11-19) the sharp edges as a set (open addressing, key + 1: 0 is empty) and a corner's fan: the faces around
// vertex v reached from face k without crossing one (Blender's and the Tlender's split normals)
static uint64_t* sharp_set(const TMesh* me, uint32_t* mask) {
    uint32_t n = 1;
    while (n < me->nsharp * 2 + 2) n <<= 1;
    uint64_t* t = calloc(n, sizeof(uint64_t));
    for (uint32_t i = 0; t && i < me->nsharp; i++) {
        uint64_t k = edge_key(me->sharp_e[i][0], me->sharp_e[i][1]) + 1;
        uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (n - 1);
        while (t[h] && t[h] != k) h = (h + 1) & (n - 1);
        t[h] = k;
    }
    *mask = n - 1;
    return t;
}
static bool sharp_has(const uint64_t* t, uint32_t mask, uint32_t a, uint32_t b) {
    uint64_t k = edge_key(a, b) + 1;
    for (uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & mask; t[h]; h = (h + 1) & mask)
        if (t[h] == k) return true;
    return false;
}
static void sharp_fan(const TMesh* me, const uint32_t* fl, uint32_t n, uint32_t v, uint32_t k, uint8_t* reach, const uint64_t* t, uint32_t mask) {
    uint32_t stack[64], ns = 0;
    memset(reach, 0, n);
    for (uint32_t i = 0; i < n; i++)
        if (fl[i] == k) reach[i] = 1, stack[ns++] = i;
    while (ns) {
        uint32_t fa = fl[stack[--ns]], st = me->fstart[fa], sz = me->fsize[fa];
        for (uint32_t c = 0; c < sz; c++) {
            if (me->loops[st + c] != v) continue;
            uint32_t nb[2] = { me->loops[st + (c + 1) % sz], me->loops[st + (c + sz - 1) % sz] };
            for (int s = 0; s < 2; s++) {
                if (sharp_has(t, mask, v, nb[s])) continue;
                for (uint32_t j = 0; j < n; j++) { // the faces across that edge
                    if (reach[j]) continue;
                    uint32_t fb = fl[j], sb = me->fstart[fb], zb = me->fsize[fb];
                    for (uint32_t cb = 0; cb < zb; cb++)
                        if (me->loops[sb + cb] == v && (me->loops[sb + (cb + 1) % zb] == nb[s] || me->loops[sb + (cb + zb - 1) % zb] == nb[s])) {
                            reach[j] = 1, stack[ns++] = j;
                            break;
                        }
                }
            }
        }
    }
}

// (#164) What the game doesn't show is not imported: objects only in collections excluded from the view layer or off in
// render (Blender's camera icon, the parents' too), and the bones' custom shapes ("bsobj": a rig's widgets, which
// Auto-Rig Pro and Rigify keep in a hidden collection) -> TObj.hidden; how many
static int mark_hidden(TFile* f) {
    int ncol = 0, *par = NULL, *off = NULL;
    for (size_t i = 0; i + 1 < f->nlines && !par; i++) {
        if (!starts(f->lines[i], "collections") || !starts(f->lines[i + 1], "col")) continue; // (a bone's "collections" has "bc")
        ncol = atoi(f->lines[i] + 12);
        if (ncol <= 0 || ncol > 100000) break;
        par = malloc(sizeof(int) * (size_t)ncol), off = calloc((size_t)ncol, sizeof(int));
        uint8_t* in = calloc((size_t)(f->nobjs > 0 ? f->nobjs : 1), 1); // bit 0: in a collection, bit 1: in a shown one
        if (!par || !off || !in) {
            free(in);
            break;
        }
        int k = 0;
        for (size_t j = i + 1; j < f->nlines && k < ncol; j++) {
            int p = -1, color = 0, ex = 0, hi = 0, hv = 0, hr = 0;
            if (!starts(f->lines[j], "col")) continue;
            if (sscanf(f->lines[j] + 4, "%d %d %d %d %d %d", &p, &color, &ex, &hi, &hv, &hr) < 6) break;
            par[k] = p, off[k] = ex || hr, k++;
        }
        for (int c = 0; c < k; c++) // (a collection inside a hidden one is hidden)
            for (int q = par[c], d = 0; q >= 0 && q < k && d < 64 && !off[c]; q = par[q], d++) off[c] = off[q];
        for (size_t j = i + 1; j < f->nlines; j++) {
            const char* l = f->lines[j];
            if (starts(l, "col") || starts(l, "coln")) continue;
            if (!starts(l, "cl")) {
                if (j > i + 2 * (size_t)ncol + 1) break;
                continue;
            }
            char* q = (char*)l + 3;
            for (;;) {
                char* e;
                long c = strtol(q, &e, 10);
                if (e == q) break;
                q = e;
                long o = strtol(q, &e, 10);
                if (e == q) break;
                q = e;
                if (o >= 0 && o < f->nobjs && c >= 0 && c < k) in[o] |= (uint8_t)(1 | (off[c] ? 0 : 2));
            }
        }
        for (int o = 0; o < f->nobjs; o++)
            if (in[o] == 1) f->objs[o].hidden = true;
        free(in);
    }
    free(par), free(off);
    for (size_t i = 0; i < f->nlines; i++) {
        if (!starts(f->lines[i], "bsobj") || !f->lines[i][5]) continue;
        for (int o = 0; o < f->nobjs; o++)
            if (!strcmp(f->objs[o].name, f->lines[i] + 6)) f->objs[o].hidden = true;
    }
    int n = 0;
    for (int o = 0; o < f->nobjs; o++) n += f->objs[o].hidden && f->objs[o].type == 0;
    return n;
}

bool tlend_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    memset(r, 0, sizeof *r);
    imprep_begin(src); // (Q19) the conversion report
    double t_start = re_time_seconds();
    snprintf(g_import_src, sizeof g_import_src, "%s", src), g_import_rigid = false;
    FILE* fp = re_fopen(src, "rb");
    if (!fp) {
        snprintf(r->text, sizeof r->text, "Could not open %s.", src);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* text = size > 0 ? malloc((size_t)size + 1) : NULL; // an import, not a frame
    bool ok = text && fread(text, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    if (!ok || strncmp(text, "TLENDER", 7)) {
        free(text);
        snprintf(r->text, sizeof r->text, "%s is not a .tlend file.", src);
        return false;
    }
    text[size] = 0;
    TFile f;
    memset(&f, 0, sizeof f);
    f.fps = 24.0f;
    read_lines(&f, text);
    size_t i = 0;
    for (; i < f.nlines; i++) {
        const char* l = f.lines[i];
        int n = 0;
        if (starts(l, "anim")) {
            int a, b, c;
            float fps;
            if (sscanf(l + 5, "%d %d %d %f", &a, &b, &c, &fps) == 4 && fps > 0) f.fps = fps;
        } else if (starts(l, "meshes") && sscanf(l + 7, "%d", &n) == 1 && !f.meshes) i = parse_meshes(&f, i + 1, n) - 1;
        else if (starts(l, "actions") && sscanf(l + 8, "%d", &n) == 1 && !f.actions) i = parse_actions(&f, i + 1, n) - 1;
        else if (starts(l, "materials") && sscanf(l + 10, "%d", &n) == 1 && !f.mats) i = parse_materials(&f, i + 1, n) - 1;
        else if (starts(l, "armatures") && sscanf(l + 10, "%d", &n) == 1 && !f.arms) i = parse_armatures(&f, i + 1, n) - 1;
        else if (starts(l, "objects") && sscanf(l + 8, "%d", &n) == 1 && !f.objs) i = parse_objects(&f, i + 1, n) - 1;
        if (i >= f.nlines) break;
    }
    f.nhidden = mark_hidden(&f);
    for (size_t k = 0; k < f.nlines; k++) {
        int a = -1, b = -1, c = -1;
        if (!starts(f.lines[k], "cb") || sscanf(f.lines[k] + 3, "%d %d %d", &a, &b, &c) != 3) continue;
        if (a < 0 || a >= f.narms || b < 0 || b >= f.arms[a].nbones || c < 0 || c >= f.arms[a].bones[b].ncons) continue;
        TCon* t = &f.arms[a].bones[b].cons[c];
        for (size_t q = k + 1; q < f.nlines && !starts(f.lines[q], "cb") && !starts(f.lines[q], "rigend"); q++) {
            const char* l = f.lines[q];
            if (starts(l, "cx")) sscanf(l + 3, "%d %d", &t->owner_space, &t->target_space);
            else if (starts(l, "cxt")) snprintf(t->target_obj, sizeof t->target_obj, "%s", l[3] ? l + 4 : "");
            else if (starts(l, "cxc")) {
                char* p = (char*)l + 4;
                t->channels = (unsigned)strtoul(p, &p, 10);
                strtol(p, &p, 10);
                for (int e = 0; e < 16; e++) t->inverse[e] = strtof(p, &p);
            } else if (starts(l, "cxx")) {
                char* p = (char*)l + 4;
                t->from = (int)strtol(p, &p, 10), t->to = (int)strtol(p, &p, 10);
                for (int e = 0; e < 3; e++) t->map[e] = (int)strtol(p, &p, 10);
                t->extrapolate = (int)strtol(p, &p, 10), t->xmix = (int)strtol(p, &p, 10);
                t->up = (int)strtol(p, &p, 10), t->target_z = (int)strtol(p, &p, 10); // (up axis, target Z: Track To's)
                for (int e = 0; e < 3; e++)
                    t->from_min[e] = strtof(p, &p), t->from_max[e] = strtof(p, &p), t->to_min[e] = strtof(p, &p), t->to_max[e] = strtof(p, &p);
            } else if (starts(l, "cxl")) sscanf(l + 4, "%f %d", &t->dist, &t->dist_mode); // (Q19) Limit Distance
            else if (starts(l, "cxv")) sscanf(l + 4, "%d %d", &t->vol_axis, &t->vol_mode); // (Q19) Maintain Volume
            else if (starts(l, "cxs")) // Spline IK (and Armature): the scale modes; the controls' names next
                sscanf(l + 4, "%d %d %d %d", &t->list_y, &t->list_xz, &t->list_even, &t->list_ctrl_rot);
            else if (starts(l, "cxsn")) {
                const char* p = l[4] ? l + 5 : "";
                t->nlist = 0;
                while (*p && t->nlist < RIG_LIST_MAX) {
                    size_t n = strcspn(p, "|");
                    snprintf(t->list_names[t->nlist++], 64, "%.*s", (int)(n < 63 ? n : 63), p);
                    p += n + (p[n] == '|');
                }
            } else if (starts(l, "cxb")) {
                sscanf(l + 4, "%d %d %f %d %d %d %d %f %d %d %d %d %f %f %f", &t->euler_order, &t->mix, &t->power, &t->uniform, &t->scale_add,
                       &t->wrap_limit, &t->remove_shear, &t->rest_length, &t->stretch_keep, &t->volume_mode, &t->bulge_min_on,
                       &t->bulge_max_on, &t->bulge_min, &t->bulge_max, &t->bulge_smooth);
            }
        }
    }
    imprep_time("read", (re_time_seconds() - t_start) * 1000.0);
    // (Q19) the image list, the drivers (their constant results into the objects and shape keys before anything reads
    // them), each object's modifier stack on its own copy of its mesh
    double t_mods = re_time_seconds();
    parse_images(&f);
    drivers_parse(&f);
    drivers_resolve(&f);
    drivers_static(&f);
    for (int oi = 0; oi < f.nobjs; oi++) apply_modifiers(&f, oi);
    imprep_time("modifiers", (re_time_seconds() - t_mods) * 1000.0);
    rig_prepare(&f);
    bool done = tfile_assets(f, src, folder_vpath, content_root, w, r);
    free(text);
    imprep_time("total", (re_time_seconds() - t_start) * 1000.0);
    return done;
}

// The parsed scene (a .tlend, an OBJ...) into assets in folder_vpath and actors in w; frees the scene. src: the file
// (its folder: where relative image paths start)
// (2026-11-16) Hair: the guides into <folder>/Hair/<name>.rgroom and a Groom on the actor named `actor` (made when missing:
// an OBJ's curves), its look from the hair's settings
static Entity named_actor(World* w, const char* name) {
    for (uint32_t i = 1; w && i < w->slot_count; i++) {
        Entity e = ecs_entity_at(w, i);
        const CName* n = e ? ecs_get(w, e, COMP_NAME) : NULL;
        if (n && !strcmp(n->text, name)) return e;
    }
    return ENTITY_NONE;
}
static void hair_asset(const char* folder_disk, const char* folder_vpath, World* w, TlendReport* r, const char* actor, const char* asset,
                       const float* pts, const uint32_t* cnt, uint32_t ng, const GroomGuidesInfo* gi, int dynamic, float stiff, float damp) {
    if (!ng) return;
    char dir[950], path[1200], vpath[1200], nm[200];
    write_name(nm, sizeof nm, "GR_", asset, NULL);
    snprintf(dir, sizeof dir, "%s/Hair", folder_disk);
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/%s.rgroom", dir, nm);
    if (!groom_guides_write(path, pts, cnt, ng, gi)) return;
    snprintf(vpath, sizeof vpath, "%s/Hair/%s.rgroom", folder_vpath, nm);
    r->grooms++;
    if (!w) return;
    Entity e = named_actor(w, actor);
    if (e == ENTITY_NONE) { // (an OBJ's curves: an actor of their own)
        e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
        if (e == ENTITY_NONE) return;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", actor);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
        r->actors++;
    }
    if (!ecs_get(w, e, COMP_GROOM) && !ecs_add(w, e, COMP_GROOM)) return;
    CGroom* g = ecs_get(w, e, COMP_GROOM);
    g->guides = groom_guides_hash(vpath);
    g->strands = gi->strands, g->segments = gi->segments;
    g->root_width = gi->root_width, g->tip_width = gi->tip_width;
    g->dye[0] = gi->color[0], g->dye[1] = gi->color[1], g->dye[2] = gi->color[2];
    g->melanin = 0.15f, g->redness = 0.0f, g->clump = 0.3f, g->length_var = 0.15f;
    g->dynamic = (uint8_t)(dynamic != 0);
    if (stiff >= 0.0f) g->stiffness = stiff;
    if (damp >= 0.0f) g->damping = damp;
}
// The .tlend's "hairs N" block (the Tlender's hair.c text form): per hair its "hr" settings, "hsurf" (the mesh object it
// grows on: the guides are in its space), "hdyn" and its guides ("hg n tri b0 b1 u v sel x y z ..."); a hair of
// instanced objects is left out
static void import_hairs(TFile* f, const char* folder_disk, const char* folder_vpath, World* w, TlendReport* r) {
    size_t i = 0;
    while (i < f->nlines && !starts(f->lines[i], "hairs")) i++;
    if (i >= f->nlines) return;
    int nh = atoi(f->lines[i] + 6);
    i++;
    for (int h = 0; h < nh; h++) {
        while (i < f->nlines && !starts(f->lines[i], "hr")) {
            if (f->lines[i][0] != 'h') return; // (the block's end)
            i++;
        }
        if (i >= f->nlines) return;
        int kind = 0, dm, count = 0, seed, inv, sv = 8, sr = 8;
        float dens, mind, rr = 0.005f, rt = 0.002f, frac, col[3] = { 0.3f, 0.2f, 0.12f };
        int sg, ch, lod;
        sscanf(f->lines[i], "hr %d %d %d %f %f %d %d %f %f %d %d %f %d %d %d %f %f %f", &kind, &dm, &count, &dens, &mind, &seed, &inv,
               &rr, &rt, &sv, &sr, &frac, &sg, &ch, &lod, &col[0], &col[1], &col[2]);
        i++;
        char surf[64] = "";
        int don = 0;
        float stiff = -1.0f, damp = -1.0f, grav;
        float* pts = NULL;
        uint32_t* cnt = NULL;
        uint32_t ng = 0, np = 0, cg = 0, cp = 0;
        for (; i < f->nlines && f->lines[i][0] == 'h' && !starts(f->lines[i], "hr"); i++) {
            const char* l = f->lines[i];
            if (starts(l, "hsurf")) snprintf(surf, sizeof surf, "%s", l + 6);
            else if (starts(l, "hdyn")) sscanf(l + 5, "%d %f %f %f", &don, &stiff, &damp, &grav);
            else if (starts(l, "hg") && kind == 0) {
                char* q = (char*)l + 3;
                long n = strtol(q, &q, 10);
                if (n < 2 || n > 4096) continue;
                strtol(q, &q, 10);
                for (int k = 0; k < 5; k++) strtof(q, &q);
                if (ng + 1 > cg) {
                    cg = cg ? cg * 2 : 256;
                    uint32_t* nc = realloc(cnt, sizeof(uint32_t) * cg);
                    if (!nc) break;
                    cnt = nc;
                }
                if (np + (uint32_t)n > cp) {
                    while (np + (uint32_t)n > cp) cp = cp ? cp * 2 : 4096;
                    float* nq = realloc(pts, sizeof(float) * 3 * cp);
                    if (!nq) break;
                    pts = nq;
                }
                for (long k = 0; k < n; k++) {
                    float p[3];
                    for (int a = 0; a < 3; a++) p[a] = strtof(q, &q);
                    point_to_engine(p, pts + 3 * (np + (uint32_t)k));
                }
                cnt[ng++] = (uint32_t)n, np += (uint32_t)n;
            }
        }
        if (ng && surf[0]) {
            GroomGuidesInfo gi;
            memset(&gi, 0, sizeof gi);
            uint32_t want = count > 0 ? (uint32_t)count : ng * 20;
            gi.strands = want < ng ? ng : want > 200000 ? 200000 : want;
            gi.segments = (uint32_t)(sr < 2 ? 2 : sr > 32 ? 32 : sr);
            // (the Tlender's radius in meters, often thick for the viewport: kept within real hair, mm)
            gi.root_width = fminf(fmaxf(rr * 2000.0f, 0.03f), 0.5f), gi.tip_width = fminf(fmaxf(rt * 2000.0f, 0.01f), gi.root_width);
            memcpy(gi.color, col, sizeof col);
            char an[96];
            snprintf(an, sizeof an, "%s_%d", surf, h);
            // (the Tlender's stiffness 0..1 and its damping: ours alike)
            hair_asset(folder_disk, folder_vpath, w, r, surf, an, pts, cnt, ng, &gi, don,
                       stiff >= 0.0f ? fminf(stiff, 1.0f) : -1.0f, damp >= 0.0f ? fminf(damp, 0.5f) : -1.0f);
        }
        free(pts), free(cnt);
    }
}

// (Q19) a Cloth / Soft Body modifier: the actor's mesh a Cloth (its top band pinned, as Make Cloth from Selected) or a
// soft body (a cushion held at its bottom, as Make Soft Body from Selected); the static actor hidden behind the cloth
static void import_sim(World* w, Entity e, int sim, const TriOut* tris, uint32_t nt) {
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    CTransform* st = ecs_get(w, e, COMP_TRANSFORM);
    if (!sm || !st) return;
    float zmin = 1e30f, zmax = -1e30f;
    for (uint32_t i = 0; i < nt; i++)
        for (int q = 0; q < 3; q++) zmin = fminf(zmin, tris[i].p[q][2]), zmax = fmaxf(zmax, tris[i].p[q][2]);
    if (sim == 2) { // a soft body: the Cloth component of kind Soft on the actor itself
        if (!ecs_add(w, e, COMP_CLOTH)) return;
        CCloth* c = ecs_get(w, e, COMP_CLOTH);
        ecs_component_defaults(COMP_CLOTH, c);
        c->kind = CLOTH_KIND_SOFT;
        soft_apply_preset(c, SOFT_CUSHION);
        c->attach = SOFT_ATTACH_BOTTOM, c->pin_axis[0] = c->pin_axis[1] = 0.0f, c->pin_axis[2] = 1.0f, c->pin_band = 1.0f;
        c->thickness = 0.4f, c->iterations = 6, c->self_collision = 1;
        st->flags = (st->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
        return;
    }
    Entity ce = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
    if (ce == ENTITY_NONE) return;
    const CName* sn = ecs_get(w, e, COMP_NAME);
    snprintf(((CName*)ecs_get(w, ce, COMP_NAME))->text, RE_NAME_MAX, "%.40s_Cloth", sn ? sn->text : "Mesh");
    CTransform* t = ecs_get(w, ce, COMP_TRANSFORM);
    memcpy(t->location, st->location, 12), memcpy(t->rotation, st->rotation, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
    CCloth* c = ecs_get(w, ce, COMP_CLOTH);
    ecs_component_defaults(COMP_CLOTH, c);
    c->mesh = sm->mesh;
    memcpy(c->mesh_scale, st->scale, 12);
    memcpy(c->color, sm->base_color, 12);
    c->pin_axis[0] = c->pin_axis[1] = 0.0f, c->pin_axis[2] = 1.0f;
    c->pin_band = fmaxf((zmax - zmin) * fabsf(st->scale[2]) * 0.05f, 2.0f);
    st->flags |= TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME;
}

// (Q19) the Tlender's lights and cameras (their objects: the "o" line's power, color, radius, lens; the "light" line):
// Blender's lights and cameras look down their -Z with +Y up; the watts into candela (683 / 4 pi), a sun's W/m2 into lux
static void imp_facing_rotator(float fwd[3], float up[3], float rot[3]);
static void import_light_camera(TFile* f, int oi, World* w, TlendReport* r) {
    TObj* o = &f->objs[oi];
    M4 wm = obj_world(f, oi, 0);
    float fl[3], ul[3], fwd[3], up[3], loc[3];
    m4_dir(&wm, (float[]){ 0, 0, -1 }, fl), m4_dir(&wm, (float[]){ 0, 1, 0 }, ul);
    fwd[0] = fl[0], fwd[1] = -fl[1], fwd[2] = fl[2], up[0] = ul[0], up[1] = -ul[1], up[2] = ul[2];
    v3norm(fwd), v3norm(up);
    point_to_engine(&wm.m[12], loc);
    bool cam = o->type == 1;
    Entity e = w ? ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(cam ? COMP_CAMERA : COMP_LIGHT)) : ENTITY_NONE;
    if (cam) {
        float lens = o->lens > 0.5f ? o->lens : 50.0f, fov = 2.0f * atanf(36.0f * 0.5f / lens) * 57.29578f;
        if (e != ENTITY_NONE) ((CCamera*)ecs_get(w, e, COMP_CAMERA))->fov_deg = fminf(fmaxf(fov, 10.0f), 170.0f), r->cameras++;
        imprep_add(IMPREP_CONVERTED, IMPREP_CAT_SCENE, o->name, "Camera", TR("a Camera actor (field of view %.1f from the %.0f mm lens on a 36 mm sensor)"), fov, lens);
    } else {
        CLight l;
        memset(&l, 0, sizeof l);
        l.type = o->light_type == 1 ? LIGHT_DIRECTIONAL : o->light_type == 2 ? LIGHT_SPOT : o->light_type == 3 ? LIGHT_RECT : LIGHT_POINT;
        memcpy(l.color, o->light_color, 12);
        float watts = o->light_power;
        l.intensity = watts * (l.type == LIGHT_DIRECTIONAL ? 683.0f : l.type == LIGHT_RECT ? 683.0f / 3.14159265f : 683.0f / 12.566371f);
        l.attenuation_radius = fminf(fmaxf(sqrtf(fmaxf(l.intensity, 0.0f) / 0.05f), 5.0f), 50.0f) * 100.0f;
        l.outer_cone = o->spot_size > 0.0f ? o->spot_size * 57.29578f * 0.5f : 22.5f;
        l.inner_cone = l.outer_cone * (1.0f - fminf(fmaxf(o->spot_blend, 0.0f), 1.0f));
        l.source_radius = o->light_radius * 100.0f;
        l.source_width = o->light_size * 100.0f, l.source_height = (o->light_shape == 1 || o->light_shape == 3 ? o->light_size_y : o->light_size) * 100.0f;
        l.cast_shadows = 1;
        if (e != ENTITY_NONE) *(CLight*)ecs_get(w, e, COMP_LIGHT) = l, r->lights++;
        imprep_add(IMPREP_CONVERTED, IMPREP_CAT_SCENE, o->name, l.type == LIGHT_DIRECTIONAL ? "Sun" : l.type == LIGHT_SPOT ? "Spot" : l.type == LIGHT_RECT ? "Area" : "Point",
                   TR("a light actor: %.0f W into %.1f %s (Stationary, casts shadows)"), watts, l.intensity, l.type == LIGHT_DIRECTIONAL ? "lux" : "cd");
    }
    if (e == ENTITY_NONE) return;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", o->name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, loc, 12);
    imp_facing_rotator(fwd, up, t->rotation);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    if (!cam) t->flags = MOBILITY_STATIONARY;
}

// (Q19) the bone constraints the rig evaluates (the report: the ones it can't)
static void report_constraints(const TFile* f) {
    static const char* const names[] = { "", "IK", "Copy Location", "Copy Rotation", "Copy Scale", "Copy Transforms", "Damped Track",
                                         "Limit Location", "Limit Rotation", "Limit Scale", "Stretch To", "Child Of", "Armature",
                                         "Transformation", "Track To", "Locked Track", "Limit Distance", "Maintain Volume", "Action",
                                         "Spline IK" };
    for (int a = 0; a < f->narms; a++)
        for (int b = 0; b < f->arms[a].nbones; b++)
            for (int k = 0; k < f->arms[a].bones[b].ncons; k++) {
                const TCon* c = &f->arms[a].bones[b].cons[k];
                const char* nm = c->type > 0 && c->type <= TC_SPLINE_IK ? names[c->type] : "?";
                const char* owner = f->arms[a].bones[b].name;
                if (c->type == TC_ARMATURE)
                    imprep_add(IMPREP_LOST, IMPREP_CAT_RIG, owner, nm, "%s", TR("not evaluated: the bone follows its parent (an Armature constraint mixes other bones: skin the mesh to them instead)"));
                else if (c->type == TC_ACTION)
                    imprep_add(IMPREP_LOST, IMPREP_CAT_RIG, owner, nm, "%s", TR("not evaluated: an action driven by a bone's channel (bake it into the action in the Tlender)"));
                else imprep_add(IMPREP_SAME, IMPREP_CAT_RIG, owner, nm, "%s", TR("evaluated as the Tlender does, baked into the animations"));
            }
}

static bool tfile_assets(TFile f, const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    g_near_dir[0] = 0; // (2026-11-23: the images around indexed again for each import)
    // where the assets go
    char folder_disk[900], src_dir[900];
    snprintf(folder_disk, sizeof folder_disk, "%s%s", content_root, folder_vpath + 8);
    re_make_dirs(folder_disk);
    snprintf(src_dir, sizeof src_dir, "%s", src);
    char* sl = strrchr(src_dir, '/');
    char* bs = strrchr(src_dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = 0;
    // the materials
    uint32_t* mat_hash = calloc((size_t)(f.nmats ? f.nmats : 1), sizeof(uint32_t));
    for (int m = 0; m < f.nmats && mat_hash; m++) mat_hash[m] = import_material(&f.mats[m], folder_disk, folder_vpath, src_dir, r);
    // the objects: a mesh deformed by an armature -> a Skeletal Mesh; any other mesh -> Static Meshes (a part per slot)
    if (f.nhidden)
        imprep_add(IMPREP_SAME, IMPREP_CAT_SCENE, "", "Hidden", TR("%d meshes hidden in render or used as bone shapes (a rig's widgets): not imported"), f.nhidden);
    char path[1100], vpath[1100];
    TriOut* tris = NULL;
    size_t tris_cap = 0;
    for (int oi = 0; oi < f.nobjs; oi++) {
        TObj* o = &f.objs[oi];
        if (o->type != 0 || o->mesh < 0 || o->mesh >= f.nmeshes || o->hidden) continue;
        TMesh* me = &f.meshes[o->mesh];
        if (!me->nf) continue;
        M4 world = obj_world(&f, oi, 0);
        size_t need = 0;
        for (uint32_t k = 0; k < me->nf; k++) need += me->fsize[k] >= 3 ? me->fsize[k] - 2 : 0;
        if (need > tris_cap) {
            TriOut* nt = realloc(tris, sizeof(TriOut) * need);
            if (!nt) continue;
            tris = nt, tris_cap = need;
        }
        // smooth normals (the faces' areas), in Blender's space
        float* vn = calloc((size_t)me->nv * 3 + 3, sizeof(float));
        if (!vn) continue;
        for (uint32_t k = 0; k < me->nf; k++) {
            uint32_t s = me->fstart[k], n = me->fsize[k];
            for (uint32_t c = 1; c + 1 < n; c++) {
                const float* a = &me->co[me->loops[s] * 3];
                const float* b = &me->co[me->loops[s + c] * 3];
                const float* d = &me->co[me->loops[s + c + 1] * 3];
                float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
                float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int q = 0; q < 3; q++)
                    vn[me->loops[s] * 3 + q] += cr[q], vn[me->loops[s + c] * 3 + q] += cr[q], vn[me->loops[s + c + 1] * 3 + q] += cr[q];
            }
        }
        if (sk_owner(&f, oi) >= 0) { // (its armature's Skeletal Mesh, below: bent by it or riding one of its bones)
            free(vn);
            continue;
        }
        // --- Static Meshes: a mesh per material slot, in the object's space; an actor each at the object's transform
        M4 obj_e = m4_to_engine(&world);
        float loc[3], axes[3][3], sc[3], rot[3];
        m4_split(&obj_e, loc, axes, sc);
        axes_rotator(axes, rot);
        uint32_t nslots = o->nslots > 0 ? (uint32_t)o->nslots : 1;
        // Auto Smooth (Blender's, 30 degrees): each corner's normal from the faces at its vertex that turn less than 30
        // degrees from its own (a hard edge stays hard: a box's faces are flat, a sphere stays round)
        float* fnrm = calloc((size_t)me->nf * 3 + 3, sizeof(float));
        uint32_t* vf_start = calloc((size_t)me->nv + 2, sizeof(uint32_t));
        uint32_t nloops = 0;
        for (uint32_t k = 0; k < me->nf; k++) nloops += me->fsize[k];
        uint32_t* vf = malloc(sizeof(uint32_t) * (nloops + 1));
        bool autosmooth = fnrm && vf_start && vf;
        for (uint32_t k = 0; autosmooth && k < me->nf; k++) {
            uint32_t s = me->fstart[k], n = me->fsize[k];
            for (uint32_t c = 1; c + 1 < n; c++) {
                const float* a = &me->co[me->loops[s] * 3];
                const float* b = &me->co[me->loops[s + c] * 3];
                const float* d = &me->co[me->loops[s + c + 1] * 3];
                float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
                fnrm[k * 3] += e1[1] * e2[2] - e1[2] * e2[1], fnrm[k * 3 + 1] += e1[2] * e2[0] - e1[0] * e2[2], fnrm[k * 3 + 2] += e1[0] * e2[1] - e1[1] * e2[0];
            }
            for (uint32_t c = 0; c < n; c++) vf_start[me->loops[s + c] + 1]++;
        }
        for (uint32_t i = 0; autosmooth && i < me->nv; i++) vf_start[i + 1] += vf_start[i];
        uint32_t* fill = autosmooth ? calloc((size_t)me->nv + 1, sizeof(uint32_t)) : NULL;
        autosmooth = autosmooth && fill;
        for (uint32_t k = 0; autosmooth && k < me->nf; k++)
            for (uint32_t c = 0; c < me->fsize[k]; c++) {
                uint32_t vi = me->loops[me->fstart[k] + c];
                vf[vf_start[vi] + fill[vi]++] = k;
            }
        free(fill);
        // (2026-11-19) the sharp edges the Tlender marked: their vertices' corners take only their fan's faces
        uint32_t smask = 0;
        uint64_t* sset = autosmooth && me->nsharp ? sharp_set(me, &smask) : NULL;
        uint8_t* vsharp = sset ? calloc((size_t)me->nv + 1, 1) : NULL;
        for (uint32_t q = 0; vsharp && q < me->nsharp; q++) vsharp[me->sharp_e[q][0]] = vsharp[me->sharp_e[q][1]] = 1;
        // shape keys (or glTF's morph targets): a Skeletal Mesh of one bone instead, its skins bent by the morphs on the
        // GPU (render.c "skin"), a skin per slot; the actor a Skeletal Mesh at the object's transform
        MorphBuild morph = { 0 };
        uint32_t* csrc = NULL;
        float(*skw1)[2] = NULL;
        SkelMesh* msk = NULL;
        if (me->nkeys) {
            M4 idm = m4_id();
            morph.nsrc = me->nv;
            morph_from_keys(&morph, me, 0, &idm, vn);
            if (morph_finish(&morph)) {
                csrc = malloc(sizeof(uint32_t) * need * 3), skw1 = malloc(sizeof(float[2]) * need * 3), msk = calloc(1, sizeof *msk);
                if (!csrc || !skw1 || !msk) free(csrc), free(skw1), free(msk), csrc = NULL, skw1 = NULL, msk = NULL;
            }
            SkinW one = { { 0, 0, 0, 0 }, { 1.0f, 0, 0, 0 } };
            for (size_t q = 0; skw1 && q < need * 3; q++) skin_pack(&one, 0, skw1[q]);
        }
        for (uint32_t slot = 0; slot < nslots && slot < 32; slot++) {
            uint32_t nt = 0;
            for (uint32_t k = 0; k < me->nf; k++) {
                if (me->fmat[k] != slot && !(slot == 0 && me->fmat[k] >= nslots)) continue;
                uint32_t s = me->fstart[k], n = me->fsize[k];
                for (uint32_t c = 1; c + 1 < n; c++) {
                    TriOut* t = &tris[nt++];
                    uint32_t cs[3] = { s, s + c + 1, s + c };
                    for (int q = 0; q < 3; q++) {
                        uint32_t vi = me->loops[cs[q]];
                        // object space -> the Engine's object space (C v; the scale stays on the actor)
                        point_to_engine(&me->co[vi * 3], t->p[q]);
                        float sn[3] = { vn[vi * 3], vn[vi * 3 + 1], vn[vi * 3 + 2] };
                        if (autosmooth) {
                            const float* fk = &fnrm[k * 3];
                            float lk = sqrtf(fk[0] * fk[0] + fk[1] * fk[1] + fk[2] * fk[2]) + 1e-20f;
                            sn[0] = sn[1] = sn[2] = 0.0f;
                            uint8_t reach[64];
                            uint32_t nfv = vf_start[vi + 1] - vf_start[vi];
                            bool fan = vsharp && vsharp[vi] && nfv <= 64;
                            if (fan) sharp_fan(me, vf + vf_start[vi], nfv, vi, k, reach, sset, smask);
                            for (uint32_t j = vf_start[vi]; j < vf_start[vi + 1]; j++) {
                                if (fan && !reach[j - vf_start[vi]]) continue; // (across a sharp edge)
                                const float* fj = &fnrm[vf[j] * 3];
                                float lj = sqrtf(fj[0] * fj[0] + fj[1] * fj[1] + fj[2] * fj[2]) + 1e-20f;
                                if ((fk[0] * fj[0] + fk[1] * fj[1] + fk[2] * fj[2]) / (lk * lj) < (o->split_cos > 0.0f ? o->split_cos : 0.866f)) continue; // (over 30 degrees, or the Edge Split's: a hard edge)
                                sn[0] += fj[0], sn[1] += fj[1], sn[2] += fj[2];
                            }
                        }
                        float nb[3] = { sn[0], -sn[1], sn[2] }, l = sqrtf(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]);
                        for (int z = 0; z < 3; z++) t->n[q][z] = l > 1e-12f ? nb[z] / l : 0.0f;
                        t->uv[q][0] = me->uv[cs[q] * 2], t->uv[q][1] = me->uv[cs[q] * 2 + 1];
                        t->col[q] = me->col ? me->col[cs[q]] : 0xFFFFFFFFu;
                        if (csrc) csrc[(size_t)(t - tris) * 3 + q] = vi;
                    }
                }
            }
            if (!nt) continue;
            if (msk) { // (a skin of the one-bone Skeletal Mesh)
                char sname[200], mdir[950];
                write_name(sname, sizeof sname, "SKS_", o->name, NULL);
                size_t sl = strlen(sname);
                snprintf(sname + sl, sizeof sname - sl, "_%u", slot);
                snprintf(mdir, sizeof mdir, "%s/Meshes", folder_disk);
                re_make_dirs(mdir);
                snprintf(path, sizeof path, "%s/%s.rmesh", mdir, sname);
                if (msk->nskins >= ANIM_MAX_SKINS || !write_skin_rmesh(path, tris, (const float(*)[2])skw1, nt, csrc, &morph.m)) continue;
                snprintf(vpath, sizeof vpath, "%s/Meshes/%s.rmesh", folder_vpath, sname);
                SkSkin* k = &msk->skins[msk->nskins++];
                k->mesh = render_mesh_id_of(vpath);
                k->color[0] = k->color[1] = k->color[2] = 0.7f, k->roughness = 0.5f;
                int ms = (int)slot < o->nslots ? o->slots[slot] : -1;
                if (ms >= 0 && ms < f.nmats && mat_hash) k->material = mat_hash[ms];
                r->meshes++;
                continue;
            }
            char mname[200];
            if (nslots > 1) {
                char sfx[16];
                snprintf(sfx, sizeof sfx, "%u", slot);
                write_name(mname, sizeof mname, "SM_", o->name, sfx);
            } else
                write_name(mname, sizeof mname, "SM_", o->name, NULL);
            char mdir[950];
            snprintf(mdir, sizeof mdir, "%s/Meshes", folder_disk);
            re_make_dirs(mdir);
            snprintf(path, sizeof path, "%s/%s.rmesh", mdir, mname);
            if (!write_rmesh(path, tris, nt, me->col != NULL)) continue;
            r->meshes++;
            snprintf(vpath, sizeof vpath, "%s/Meshes/%s.rmesh", folder_vpath, mname);
            if (w) {
                Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
                if (e == ENTITY_NONE) continue;
                snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", o->name);
                CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                memcpy(t->location, loc, 12), memcpy(t->rotation, rot, 12), memcpy(t->scale, sc, 12);

                t->flags = MOBILITY_STATIC;
                CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
                sm->mesh = render_mesh_id_of(vpath);
                sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f, sm->roughness = 0.5f, sm->cast_shadow = 1;
                int ms = (int)slot < o->nslots ? o->slots[slot] : -1;
                if (ms >= 0 && ms < f.nmats && mat_hash) sm->material = mat_hash[ms];
                content_mesh_material_set_disk(path, sm->material); // (the mesh's own: its viewer, a drag into the level)
                r->actors++;
                if (o->sim) import_sim(w, e, o->sim, tris, nt); // (Q19) the Cloth / Soft Body modifier
            }
        }
        if (msk && msk->nskins) { // the one-bone Skeletal Mesh and its actor
            char sname[200];
            write_name(sname, sizeof sname, "SK_", o->name, NULL);
            snprintf(msk->path, sizeof msk->path, "%s/%s.rskm", folder_disk, sname);
            snprintf(msk->name, sizeof msk->name, "%s", sname);
            msk->nbones = 1;
            snprintf(msk->bones[0].name, ANIM_NAME, "Root");
            msk->bones[0].parent = -1;
            msk->bones[0].bind.q[3] = 1.0f, msk->bones[0].bind.s[0] = msk->bones[0].bind.s[1] = msk->bones[0].bind.s[2] = 1.0f;
            morph_to_skel(&morph, msk);
            sk_stamp(msk);
            if (anim_mesh_save(msk)) {
                r->skeletal++, r->morphs += msk->nmorphs;
                Entity e = w ? ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH)) : ENTITY_NONE;
                if (e != ENTITY_NONE) {
                    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", o->name);
                    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                    memcpy(t->location, loc, 12), memcpy(t->rotation, rot, 12), memcpy(t->scale, sc, 12);
                    t->flags = MOBILITY_MOVABLE;
                    CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
                    snprintf(vpath, sizeof vpath, "%s/%s.rskm", folder_vpath, sname);
                    c->mesh = vhash(vpath);
                    r->actors++;
                }
            }
        }
        free(csrc), free(skw1), free(msk);
        morph_build_free(&morph);
        free(vn), free(fnrm), free(vf_start), free(vf), free(sset), free(vsharp);
    }
    free(tris);
    // a Skeletal Mesh per armature object (every mesh it deforms, its actions and its control rig's baked)
    for (int oi = 0; oi < f.nobjs; oi++)
        if (f.objs[oi].type == 3) import_skeletal(&f, oi, mat_hash, folder_disk, folder_vpath, w, r);
    import_hairs(&f, folder_disk, folder_vpath, w, r); // (2026-11-16)
    for (int oi = 0; oi < f.nobjs; oi++) { // (Q19) lights, cameras; what has no Engine counterpart said so
        const TObj* o = &f.objs[oi];
        if (o->type == 1 || o->type == 2) import_light_camera(&f, oi, w, r);
        else if (o->type == 4) imprep_add(IMPREP_SAME, IMPREP_CAT_SCENE, o->name, "Empty", "%s", TR("an Empty: no actor (its transform carries its children)"));
        else if (o->type == 5) imprep_add(IMPREP_LOST, IMPREP_CAT_SCENE, o->name, "Curve", "%s", TR("a curve object: make it a mesh in the Tlender, or draw a Spline Component in the Engine"));
        else if (o->type == 7) imprep_add(IMPREP_LOST, IMPREP_CAT_SCENE, o->name, "Volume", "%s", TR("a volume object (VDB): the Engine's volumes are the fog, the clouds and the fire"));
    }
    report_constraints(&f);
    drivers_report(&f);
    if (f.ncurves) { // an OBJ's polylines
        GroomGuidesInfo gi = { .strands = f.ncurves * 20 < 200000 ? (f.ncurves * 20 < 1000 ? 1000 : f.ncurves * 20) : 200000,
                               .segments = 14, .root_width = 0.08f, .tip_width = 0.04f, .color = { 0.35f, 0.22f, 0.12f } };
        const char* base = strrchr(src, '/');
        const char* b2 = strrchr(src, '\\');
        if (b2 > base) base = b2;
        base = base ? base + 1 : src;
        char nm[96];
        snprintf(nm, sizeof nm, "%s", base);
        char* dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        hair_asset(folder_disk, folder_vpath, w, r, "Hair", nm, f.curve_pts, f.curve_cnt, f.ncurves, &gi, 0, -1.0f, -1.0f);
        free(f.curve_pts), free(f.curve_cnt);
    }
    snprintf(r->text, sizeof r->text, "%u meshes, %u materials (%u textures, %u nodes skipped), %u skeletal meshes, %u animations, %u actors%s",
             r->meshes, r->materials, r->textures, r->skipped_nodes, r->skeletal, r->animations, r->actors,
             r->skipped_rigs ? " (an armature over 128 bones deforms meshes: skipped)" : "");
    if (r->morphs) {
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u morph targets", r->morphs);
    }
    if (r->grooms) {
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u hair grooms", r->grooms);
    }
    if (r->lights || r->cameras) { // (Q19: the .tlend's too)
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u lights, %u cameras", r->lights, r->cameras);
    }
    imprep_finish(folder_disk, r->text, sizeof r->text); // (Q19) the conversion report next to the assets
    drivers_free(&f);
    free(g_mc_images), g_mc_images = NULL, g_mc_nimages = 0;
    // (the parsed file's memory)
    for (int m = 0; m < f.nmeshes; m++) {
        TMesh* t = &f.meshes[m];
        free(t->co), free(t->fstart), free(t->fsize), free(t->loops), free(t->uv), free(t->col), free(t->fmat), free(t->gname), free(t->gweight);
        tmesh_free_keys(t), tmesh_free_marks(t);
    }
    for (int m = 0; m < f.nmats; m++) free(f.mats[m].nodes);
    for (int a = 0; a < f.narms; a++) {
        TArm* t = &f.arms[a];
        for (int b = 0; b < t->nbones; b++) free(t->bones[b].cons);
        free(t->bones), free(t->rest), free(t->rel), free(t->pose), free(t->length), free(t->cquat), free(t->curve_bone);
        free(t->uorder), free(t->mem), free(t->mem_start), free(t->sol), free(t->sol_start);
    }
    for (int a = 0; a < f.nactions; a++) {
        for (int c = 0; c < f.actions[a].ncurves; c++) free(f.actions[a].curves[c].keys);
        for (int c = 0; c < f.actions[a].nmcurves; c++) free(f.actions[a].mcurves[c].keys);
        free(f.actions[a].curves), free(f.actions[a].mcurves);
    }
    free(f.meshes), free(f.mats), free(f.arms), free(f.actions), free(f.objs), free(f.lines), free(mat_hash);
    return r->meshes > 0 || r->materials > 0 || r->grooms > 0;
}

// --- OBJ (+ MTL): the same scene structure as a .tlend (an object per "o", or per "g" when the file has no "o", its
// faces by material), then the same assets: Static Meshes per material slot, materials as Principled BSDF node graphs
// (the base color and its image, roughness, metallic, alpha), actors in the level. The file's Y up becomes Blender's
// Z up (x, -z, y), then the Engine's as for a .tlend. ---

static char* obj_read_file(const char* path, size_t* n) { // (n: its size, NULL: not wanted)
    FILE* fp = re_fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* t = size >= 0 ? malloc((size_t)size + 1) : NULL; // an import, not a frame
    if (t && fread(t, 1, (size_t)size, fp) != (size_t)size) free(t), t = NULL;
    fclose(fp);
    if (t) t[size] = 0;
    if (t && n) *n = (size_t)size;
    return t;
}

// *p holds at least need elements (doubling)
static bool obj_grow(void** p, size_t* cap, size_t need, size_t elem) {
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void* q = realloc(*p, c * elem);
    if (!q) return false;
    *p = q, *cap = c;
    return true;
}

// The line's value when it starts with key and a blank (NULL otherwise)
static const char* obj_key(const char* line, const char* key) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) || (line[n] != ' ' && line[n] != '\t')) return NULL;
    line += n;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

typedef struct ObjMtl {
    char name[64], map[512];
    float kd[3], rough, metal, alpha;
} ObjMtl;

// A material as a Principled BSDF (and its Image Texture) into the Material Output
static void obj_material(TMat* m, const ObjMtl* o) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", o->name);
    m->nodes = calloc(3, sizeof(TNode));
    if (!m->nodes) return;
    TNode* out = &m->nodes[0];
    snprintf(out->idname, sizeof out->idname, "ShaderNodeOutputMaterial");
    out->x = 300.0f;
    TNode* b = &m->nodes[1];
    snprintf(b->idname, sizeof b->idname, "ShaderNodeBsdfPrincipled");
    b->nin = 5;
    b->in[0][0] = o->kd[0], b->in[0][1] = o->kd[1], b->in[0][2] = o->kd[2], b->in[0][3] = 1.0f;
    b->in[1][0] = o->metal, b->in[2][0] = o->rough, b->in[3][0] = 1.5f, b->in[4][0] = o->alpha;
    for (int q = 0; q < 4; q++) out->prop[q] = b->prop[q] = -1;
    m->nnodes = 2;
    int* l = m->links[m->nlinks++];
    l[0] = 1, l[1] = 0, l[2] = 0, l[3] = 0; // BSDF -> Surface
    if (o->map[0]) {
        TNode* t = &m->nodes[2];
        snprintf(t->idname, sizeof t->idname, "ShaderNodeTexImage");
        t->x = -320.0f;
        for (int q = 0; q < 4; q++) t->prop[q] = -1;
        snprintf(t->image, sizeof t->image, "%s", o->map);
        m->nnodes = 3;
        l = m->links[m->nlinks++];
        l[0] = 2, l[1] = 0, l[2] = 1, l[3] = 0; // Color -> Base Color
    }
}

// The MTL's materials (newmtl, Kd, map_Kd, Ns or Pr, Pm, d or Tr) after the ones in out; their new count
static int obj_read_mtl(const char* path, ObjMtl* out, int count, int cap) {
    char* text = obj_read_file(path, NULL);
    if (!text) return count;
    ObjMtl* m = NULL;
    for (char* line = text; line;) {
        char* e = strpbrk(line, "\r\n");
        if (e) *e = 0;
        while (*line == ' ' || *line == '\t') line++;
        for (char* t = line + strlen(line); t > line && (t[-1] == ' ' || t[-1] == '\t'); t--) t[-1] = 0;
        const char* v;
        if ((v = obj_key(line, "newmtl"))) {
            m = count < cap ? &out[count++] : NULL;
            if (m) {
                memset(m, 0, sizeof *m);
                snprintf(m->name, sizeof m->name, "%s", v);
                m->kd[0] = m->kd[1] = m->kd[2] = 0.8f, m->rough = 0.5f, m->alpha = 1.0f;
            }
        } else if (!m) {
        } else if ((v = obj_key(line, "Kd"))) {
            sscanf(v, "%f %f %f", &m->kd[0], &m->kd[1], &m->kd[2]);
        } else if ((v = obj_key(line, "Ns"))) { // the Phong exponent (0..1000) as roughness, as Blender's importer reads it
            m->rough = 1.0f - sqrtf(fminf(fmaxf((float)atof(v), 0.0f), 1000.0f) / 1000.0f);
        } else if ((v = obj_key(line, "Pr"))) {
            m->rough = (float)atof(v);
        } else if ((v = obj_key(line, "Pm"))) {
            m->metal = (float)atof(v);
        } else if ((v = obj_key(line, "d"))) {
            m->alpha = (float)atof(v);
        } else if ((v = obj_key(line, "Tr"))) {
            m->alpha = 1.0f - (float)atof(v);
        } else if ((v = obj_key(line, "map_Kd"))) { // the options (-s 1 1 1, -bm 0.5, -clamp on...) come before the file
            while (*v == '-') {
                while (*v && *v != ' ' && *v != '\t') v++;
                for (;;) {
                    while (*v == ' ' || *v == '\t') v++;
                    char* p;
                    strtod(v, &p);
                    bool word = !strncmp(v, "on ", 3) || !strncmp(v, "off ", 4);
                    if (p == v && !word) break;
                    v = word ? strchr(v, ' ') : p;
                }
            }
            snprintf(m->map, sizeof m->map, "%s", v);
        }
        line = e ? e + 1 : NULL;
    }
    free(text);
    return count;
}

// A face corner's indices (v, v/vt, v//vn, v/vt/vn; negative: from the end; 0: none)
static void obj_corner(const char* s, long nv, long nt, long* v, long* t) {
    char* p;
    *v = strtol(s, &p, 10), *t = 0;
    if (*p == '/' && p[1] != '/') *t = strtol(p + 1, &p, 10);
    if (*v < 0) *v += nv + 1;
    if (*t < 0) *t += nt + 1;
}

static void obj_free_meshes(TFile* f) {
    for (int m = 0; m < f->nmeshes; m++)
        free(f->meshes[m].co), free(f->meshes[m].fstart), free(f->meshes[m].fsize), free(f->meshes[m].loops), free(f->meshes[m].uv), free(f->meshes[m].col),
            free(f->meshes[m].fmat), tmesh_free_keys(&f->meshes[m]), tmesh_free_marks(&f->meshes[m]);
    for (int m = 0; m < f->nmats; m++) free(f->mats[m].nodes);
    free(f->meshes), free(f->objs), free(f->mats);
}

// The capacities of the object being read
typedef struct ObjCaps {
    size_t co, faces, corners, uv, curves, curve_pts;
} ObjCaps;

// A new object (and its mesh); an object with no faces yet is reused (just renamed)
static bool obj_begin(TFile* f, size_t* cap, ObjCaps* caps, const char* name) {
    if (f->nobjs && !f->meshes[f->nobjs - 1].nf) {
        snprintf(f->objs[f->nobjs - 1].name, sizeof f->objs[0].name, "%s", name);
        return true;
    }
    size_t c = *cap;
    if (!obj_grow((void**)&f->objs, &c, (size_t)f->nobjs + 1, sizeof(TObj))) return false;
    c = *cap;
    if (!obj_grow((void**)&f->meshes, &c, (size_t)f->nmeshes + 1, sizeof(TMesh))) return false;
    *cap = c;
    TMesh* me = &f->meshes[f->nmeshes];
    memset(me, 0, sizeof *me);
    TObj* ob = &f->objs[f->nobjs];
    memset(ob, 0, sizeof *ob);
    ob->mesh = f->nmeshes++, f->nobjs++;
    ob->arm = ob->parent = ob->deform = ob->action = -1;
    ob->scale[0] = ob->scale[1] = ob->scale[2] = 1.0f;
    memcpy(ob->parent_inv, m4_id().m, sizeof ob->parent_inv);
    snprintf(ob->name, sizeof ob->name, "%s", name);
    memset(caps, 0, sizeof *caps);
    return true;
}

bool obj_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    imprep_begin(src); // (Q19)
    memset(r, 0, sizeof *r);
    char* text = obj_read_file(src, NULL);
    if (!text) {
        snprintf(r->text, sizeof r->text, "Could not open %s.", src);
        return false;
    }
    TFile f;
    memset(&f, 0, sizeof f);
    f.fps = 24.0f;
    char dir[900];
    snprintf(dir, sizeof dir, "%s", src);
    char* sl = strrchr(dir, '/');
    char* bs = strrchr(dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = 0;
    else snprintf(dir, sizeof dir, ".");
    // the file's positions and UVs (indexed by every object); per position, its index in the mesh of the object that
    // last used it
    float *pos = NULL, *tex = NULL;
    struct { int local, obj; }* remap = NULL;
    size_t npos = 0, ntex = 0, cpos = 0, ctex = 0, cremap = 0, cobj = 0;
    ObjMtl* mtl = calloc(256, sizeof(ObjMtl));
    int nmtl = 0, cur_mtl = -1;
    ObjCaps caps = { 0 };
    bool any_o = !strncmp(text, "o ", 2) || strstr(text, "\no ");
    bool ok = mtl != NULL;
    for (char* line = text; line && ok;) {
        char* e = strpbrk(line, "\r\n");
        if (e) *e = 0;
        while (*line == ' ' || *line == '\t') line++;
        for (char* t = line + strlen(line); t > line && (t[-1] == ' ' || t[-1] == '\t'); t--) t[-1] = 0;
        const char* v;
        if ((v = obj_key(line, "v"))) {
            ok = obj_grow((void**)&pos, &cpos, (npos + 1) * 3, sizeof(float)) && obj_grow((void**)&remap, &cremap, npos + 1, sizeof *remap);
            if (!ok) break;
            float x = 0, y = 0, z = 0;
            sscanf(v, "%f %f %f", &x, &y, &z);
            pos[npos * 3] = x, pos[npos * 3 + 1] = -z, pos[npos * 3 + 2] = y; // Y up -> Z up
            remap[npos].obj = -1;
            npos++;
        } else if ((v = obj_key(line, "l"))) { // (2026-11-16) a polyline: a hair guide
            long idx[4096];
            int k = 0;
            for (const char* p = v; *p && k < 4096;) {
                long a = strtol(p, NULL, 10);
                if (a < 0) a += (long)npos + 1;
                if (a >= 1 && a <= (long)npos) idx[k++] = a - 1;
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t') p++;
            }
            if (k >= 2) {
                size_t cc = f.ncurves, cpp = f.ncurve_pts; // (grown by obj_grow's doubling: capacities kept in caps)
                ok = obj_grow((void**)&f.curve_cnt, &caps.curves, cc + 1, sizeof(uint32_t)) &&
                     obj_grow((void**)&f.curve_pts, &caps.curve_pts, (cpp + (size_t)k) * 3, sizeof(float));
                if (!ok) break;
                for (int q = 0; q < k; q++) {
                    const float* s3 = &pos[idx[q] * 3];
                    float* d3 = &f.curve_pts[(cpp + (size_t)q) * 3];
                    d3[0] = s3[0] * 100.0f, d3[1] = -s3[1] * 100.0f, d3[2] = s3[2] * 100.0f; // (as point_to_engine)
                }
                f.curve_cnt[f.ncurves++] = (uint32_t)k, f.ncurve_pts += (uint32_t)k;
            }
        } else if ((v = obj_key(line, "vt"))) {
            if (!(ok = obj_grow((void**)&tex, &ctex, (ntex + 1) * 2, sizeof(float)))) break;
            float u = 0, t = 0;
            sscanf(v, "%f %f", &u, &t);
            tex[ntex * 2] = u, tex[ntex * 2 + 1] = t;
            ntex++;
        } else if ((v = obj_key(line, "o")) || (!any_o && (v = obj_key(line, "g")))) {
            ok = obj_begin(&f, &cobj, &caps, v);
        } else if ((v = obj_key(line, "mtllib"))) {
            char mp[1100];
            snprintf(mp, sizeof mp, "%s/%s", dir, v);
            nmtl = obj_read_mtl(mp, mtl, nmtl, 256);
        } else if ((v = obj_key(line, "usemtl"))) {
            cur_mtl = -1;
            for (int k = 0; k < nmtl; k++)
                if (!strcmp(mtl[k].name, v)) cur_mtl = k;
        } else if ((v = obj_key(line, "f"))) {
            if (!f.nobjs && !(ok = obj_begin(&f, &cobj, &caps, "Mesh"))) break;
            TObj* ob = &f.objs[f.nobjs - 1];
            TMesh* me = &f.meshes[ob->mesh];
            long cv[64], ct[64];
            int k = 0;
            for (const char* p = v; *p && k < 64;) {
                obj_corner(p, (long)npos, (long)ntex, &cv[k], &ct[k]);
                if (cv[k] >= 1 && cv[k] <= (long)npos) k++;
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t') p++;
            }
            if (k >= 3) {
                // the slot: the object's own list of the file's materials
                int slot = 0;
                if (cur_mtl >= 0) {
                    slot = -1;
                    for (int q = 0; q < ob->nslots; q++)
                        if (ob->slots[q] == cur_mtl) slot = q;
                    if (slot < 0 && ob->nslots < 32) slot = ob->nslots, ob->slots[ob->nslots++] = cur_mtl;
                    if (slot < 0) slot = 0;
                }
                size_t fc = caps.faces, nc = me->ncorners + (size_t)k;
                ok = obj_grow((void**)&me->fstart, &fc, (size_t)me->nf + 1, sizeof(uint32_t));
                fc = caps.faces, ok = ok && obj_grow((void**)&me->fsize, &fc, (size_t)me->nf + 1, sizeof(uint32_t));
                fc = caps.faces, ok = ok && obj_grow((void**)&me->fmat, &fc, (size_t)me->nf + 1, sizeof(uint16_t));
                caps.faces = fc;
                ok = ok && obj_grow((void**)&me->loops, &caps.corners, nc, sizeof(uint32_t)) && obj_grow((void**)&me->uv, &caps.uv, nc * 2, sizeof(float));
                if (!ok) break;
                me->fstart[me->nf] = me->ncorners, me->fsize[me->nf] = (uint32_t)k, me->fmat[me->nf] = (uint16_t)slot;
                for (int q = 0; q < k && ok; q++) {
                    long gv = cv[q] - 1, gt = ct[q] - 1;
                    if (remap[gv].obj != f.nobjs - 1) { // a new vertex of this object
                        if (!(ok = obj_grow((void**)&me->co, &caps.co, ((size_t)me->nv + 1) * 3, sizeof(float)))) break;
                        memcpy(&me->co[me->nv * 3], &pos[gv * 3], 3 * sizeof(float));
                        remap[gv].obj = f.nobjs - 1, remap[gv].local = (int)me->nv++;
                    }
                    uint32_t c = me->ncorners + (uint32_t)q;
                    me->loops[c] = (uint32_t)remap[gv].local;
                    bool has = gt >= 0 && gt < (long)ntex;
                    me->uv[c * 2] = has ? tex[gt * 2] : 0.0f, me->uv[c * 2 + 1] = has ? tex[gt * 2 + 1] : 0.0f;
                }
                me->ncorners += (uint32_t)k;
                me->nf++;
            }
        }
        line = e ? e + 1 : NULL;
    }
    free(pos), free(tex), free(remap), free(text);
    // the materials (all of the MTL's: the objects' slots index them)
    f.mats = calloc((size_t)(nmtl ? nmtl : 1), sizeof(TMat));
    for (int k = 0; f.mats && k < nmtl; k++) obj_material(&f.mats[k], &mtl[k]);
    f.nmats = f.mats ? nmtl : 0;
    free(mtl);
    bool faces = false;
    for (int m = 0; m < f.nmeshes; m++) faces |= f.meshes[m].nf > 0;
    if (!ok || (!faces && !f.ncurves)) {
        snprintf(r->text, sizeof r->text, ok ? "%s has no faces." : "Out of memory reading %s.", src);
        obj_free_meshes(&f);
        free(f.curve_pts), free(f.curve_cnt);
        return false;
    }
    return tfile_assets(f, src, folder_vpath, content_root, w, r);
}

#include "import_skin.inc"
#include "import_gltf.inc"
#include "import_fbx.inc"
#include "import_rasset.inc" // (#164) the Real Engine's exchange format, the Tlender's Export > Real Engine
#include "autorig.inc"
