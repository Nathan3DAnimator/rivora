// The IK Rig and the bodies' physics (ikrig.h, notas-dev/ikrig.md)
#include "ikrig.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "io_json.h"
#include "landscape.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "render_gpu.h"
#include "rig_detect.h" // (2026-11-23) what each bone is: bodies by role, chains

uint32_t bp_world_boxes(const float mn[3], const float mx[3], float (*out)[6], uint32_t cap); // (bp_vm.c: the level's boxes)

#define MAX_ASSETS 128
#define MAX_STATES 256
#define SUBSTEPS 8
#define SUB_H (1.0f / 960.0f) // (2026-11-19) a substep's length: SUBSTEPS at 120 Hz, at any frame rate
#define MAX_SUBS 32         // (a frame of 1/30 s at most)
#define LIMIT_PASSES 4u // (Q10: the limits after the contacts, Gauss-Seidel passes)

const char* const g_ik_solver_names[IKS_KIND_COUNT] = { "Chain IK", "Limb IK", "Full Body IK", "Look At" };

// --- Small vector math ---
typedef struct V3 { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { return (V3){ x, y, z }; }
static V3 add3(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub3(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul3(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross3(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len3(V3 a) { return sqrtf(dot3(a, a)); }
static V3 norm3(V3 a) {
    float l = len3(a);
    return l > 1e-8f ? mul3(a, 1.0f / l) : v3(0, 0, 1);
}
static V3 closest_on_seg(V3 a, V3 b, V3 p); // (below)
static V3 lerp3(V3 a, V3 b, float t) { return add3(a, mul3(sub3(b, a), t)); }

typedef struct M3 { float m[3][3]; } M3; // rows
static V3 m3v(const M3* a, V3 v) {
    return v3(a->m[0][0] * v.x + a->m[0][1] * v.y + a->m[0][2] * v.z, a->m[1][0] * v.x + a->m[1][1] * v.y + a->m[1][2] * v.z,
              a->m[2][0] * v.x + a->m[2][1] * v.y + a->m[2][2] * v.z);
}
static V3 m3tv(const M3* a, V3 v) { // transposed
    return v3(a->m[0][0] * v.x + a->m[1][0] * v.y + a->m[2][0] * v.z, a->m[0][1] * v.x + a->m[1][1] * v.y + a->m[2][1] * v.z,
              a->m[0][2] * v.x + a->m[1][2] * v.y + a->m[2][2] * v.z);
}
static M3 m3mul(const M3* a, const M3* b) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
    return r;
}
static M3 m3t(const M3* a) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a->m[j][i];
    return r;
}
// The rotation taking unit a onto unit b (the shortest), turned by t of its angle
static M3 rot_between(V3 a, V3 b, float t) {
    V3 ax = cross3(a, b);
    float s = len3(ax), c = fmaxf(fminf(dot3(a, b), 1.0f), -1.0f);
    float ang = atan2f(s, c) * t;
    if (s < 1e-7f) {
        if (c > 0.0f || t == 0.0f) return (M3){ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
        ax = fabsf(a.x) < 0.9f ? cross3(a, v3(1, 0, 0)) : cross3(a, v3(0, 1, 0)); // (opposite: around any perpendicular)
        ang = 3.14159265f * t;
    }
    ax = norm3(ax);
    float cs = cosf(ang), sn = sinf(ang), k = 1.0f - cs;
    return (M3){ { { cs + ax.x * ax.x * k, ax.x * ax.y * k - ax.z * sn, ax.x * ax.z * k + ax.y * sn },
                   { ax.y * ax.x * k + ax.z * sn, cs + ax.y * ax.y * k, ax.y * ax.z * k - ax.x * sn },
                   { ax.z * ax.x * k - ax.y * sn, ax.z * ax.y * k + ax.x * sn, cs + ax.z * ax.z * k } } };
}
static M3 rot_axis(V3 ax, float ang) {
    ax = norm3(ax);
    float cs = cosf(ang), sn = sinf(ang), k = 1.0f - cs;
    return (M3){ { { cs + ax.x * ax.x * k, ax.x * ax.y * k - ax.z * sn, ax.x * ax.z * k + ax.y * sn },
                   { ax.y * ax.x * k + ax.z * sn, cs + ax.y * ax.y * k, ax.y * ax.z * k - ax.x * sn },
                   { ax.z * ax.x * k - ax.y * sn, ax.z * ax.y * k + ax.x * sn, cs + ax.z * ax.z * k } } };
}

// A bone's rows: its place, its rotation (unit columns) and its scale (the columns' lengths)
static V3 row_pos(const float b[3][4]) { return v3(b[0][3], b[1][3], b[2][3]); }
static M3 row_rot(const float b[3][4], float s[3]) {
    M3 r;
    for (int c = 0; c < 3; c++) {
        float l = sqrtf(b[0][c] * b[0][c] + b[1][c] * b[1][c] + b[2][c] * b[2][c]);
        if (s) s[c] = l;
        for (int k = 0; k < 3; k++) r.m[k][c] = l > 1e-8f ? b[k][c] / l : (k == c ? 1.0f : 0.0f);
    }
    return r;
}
static void row_set(float b[3][4], const M3* r, const float s[3], V3 p) {
    for (int k = 0; k < 3; k++) {
        for (int c = 0; c < 3; c++) b[k][c] = r->m[k][c] * s[c];
    }
    b[0][3] = p.x, b[1][3] = p.y, b[2][3] = p.z;
}

// --- Quaternions (the bodies' turns) ---
typedef struct Q4 { float x, y, z, w; } Q4;
static Q4 qmul(Q4 a, Q4 b) {
    return (Q4){ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Q4 qnorm(Q4 q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12f ? (Q4){ q.x / l, q.y / l, q.z / l, q.w / l } : (Q4){ 0, 0, 0, 1 };
}
static M3 q_m3(Q4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return (M3){ { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                   { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                   { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } } };
}
static Q4 m3_q(const M3* m) {
    float tr = m->m[0][0] + m->m[1][1] + m->m[2][2];
    Q4 q;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q = (Q4){ (m->m[2][1] - m->m[1][2]) / s, (m->m[0][2] - m->m[2][0]) / s, (m->m[1][0] - m->m[0][1]) / s, 0.25f * s };
    } else if (m->m[0][0] > m->m[1][1] && m->m[0][0] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[0][0] - m->m[1][1] - m->m[2][2]) * 2.0f;
        q = (Q4){ 0.25f * s, (m->m[0][1] + m->m[1][0]) / s, (m->m[0][2] + m->m[2][0]) / s, (m->m[2][1] - m->m[1][2]) / s };
    } else if (m->m[1][1] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[1][1] - m->m[0][0] - m->m[2][2]) * 2.0f;
        q = (Q4){ (m->m[0][1] + m->m[1][0]) / s, 0.25f * s, (m->m[1][2] + m->m[2][1]) / s, (m->m[0][2] - m->m[2][0]) / s };
    } else {
        float s = sqrtf(1.0f + m->m[2][2] - m->m[0][0] - m->m[1][1]) * 2.0f;
        q = (Q4){ (m->m[0][2] + m->m[2][0]) / s, (m->m[1][2] + m->m[2][1]) / s, 0.25f * s, (m->m[1][0] - m->m[0][1]) / s };
    }
    return qnorm(q);
}
static Q4 qslerp(Q4 a, Q4 b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) b = (Q4){ -b.x, -b.y, -b.z, -b.w }, d = -d;
    if (d > 0.9995f) return qnorm((Q4){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    float th = acosf(d), s = sinf(th), wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return (Q4){ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}
static Q4 q_add_rot(Q4 q, V3 r, float s) { // q + 0.5 * (r * s, 0) * q: a small turn r (radians, world)
    Q4 d = qmul((Q4){ r.x * s, r.y * s, r.z * s, 0.0f }, q);
    return qnorm((Q4){ q.x + 0.5f * d.x, q.y + 0.5f * d.y, q.z + 0.5f * d.z, q.w + 0.5f * d.w });
}

// --- Assets ---
static struct {
    char root[600];
    bool scanned;
    IkRig* rigs[MAX_ASSETS];
    PhysicsAsset* phys[MAX_ASSETS];
    uint32_t nrigs, nphys;
    struct PState* states[MAX_STATES];
    uint32_t nstates;
    IkPhysStats stats, acc;
    bool measure; // (Q10: the joints' limits and overlaps measured each frame once someone asks: ikphys_info)
} K;

static uint32_t vpath_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}
static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", K.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(K.root);
    if (rl && !strncmp(disk, K.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}
void ikphys_vpath(const char* disk_path, char* out, size_t cap) { vpath_of(disk_path, out, cap); }
static void name_of(const char* path, char* out) {
    const char* s = strrchr(path, '/');
    snprintf(out, ANIM_NAME, "%s", s ? s + 1 : path);
    char* d = strrchr(out, '.');
    if (d) *d = 0;
}
void ikrig_set_root(const char* content_root) {
    snprintf(K.root, sizeof K.root, "%s", content_root ? content_root : "");
    for (char* c = K.root; *c; c++)
        if (*c == '\\') *c = '/';
    K.scanned = false;
}
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
    if (!strcmp(dot, ".rikrig")) ikrig_load(vpath);
    else if (!strcmp(dot, ".rphys")) phys_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth <= 12) re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (K.scanned || !K.root[0]) return;
    K.scanned = true;
    scan_dir(K.root, 0);
}

static char* read_all(const char* path, size_t* n) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = l > 0 ? malloc((size_t)l + 1) : NULL; // (a load)
    if (t && fread(t, 1, (size_t)l, f) != (size_t)l) free(t), t = NULL;
    fclose(f);
    if (t) t[l] = 0, *n = (size_t)l;
    return t;
}
static bool write_all(const char* path, JsonWriter* w) {
    if (w->fail) return false;
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(w->buf, 1, w->len, f) == w->len;
    ok = fclose(f) == 0 && ok;
    return ok && re_replace_file(tmp, path);
}
static void get_str(const JsonValue* o, const char* key, char* out) { snprintf(out, ANIM_NAME, "%s", json_get_str(o, key, "")); }

IkRig* ikrig_find(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < K.nrigs; i++)
        if (K.rigs[i]->hash == hash) return K.rigs[i];
    return NULL;
}
uint32_t ikrig_count(void) { scan(); return K.nrigs; }
IkRig* ikrig_at(uint32_t i) { return i < K.nrigs ? K.rigs[i] : NULL; }

IkRig* ikrig_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < K.nrigs; i++)
        if (K.rigs[i]->hash == h) return K.rigs[i];
    if (K.nrigs >= MAX_ASSETS) return NULL;
    char path[800];
    disk_of(vpath, path, sizeof path);
    size_t n;
    char* t = read_all(path, &n);
    if (!t) return NULL;
    JsonDoc doc;
    char err[160];
    IkRig* r = NULL;
    if (json_parse(t, n, &doc, err, sizeof err) && (r = calloc(1, sizeof *r))) { // (an asset)
        snprintf(r->path, sizeof r->path, "%s", path);
        name_of(path, r->name);
        r->hash = h, r->version = 1, r->skeleton = (uint32_t)json_get_num(&doc.root, "skeleton", 0);
        const JsonValue* gs = json_get(&doc.root, "goals");
        for (size_t i = 0; i < json_len(gs) && r->ngoals < IK_MAX_GOALS; i++) {
            const JsonValue* o = json_at(gs, i);
            IkRigGoal* g = &r->goals[r->ngoals++];
            get_str(o, "name", g->name), get_str(o, "bone", g->bone), get_str(o, "actor", g->target_actor);
            json_get_floats(o, "offset", g->offset, 3);
            g->alpha = (float)json_get_num(o, "alpha", 1.0), g->rotation = (uint8_t)json_get_bool(o, "rotation", false);
        }
        const JsonValue* ss = json_get(&doc.root, "solvers");
        for (size_t i = 0; i < json_len(ss) && r->nsolvers < IK_MAX_SOLVERS; i++) {
            const JsonValue* o = json_at(ss, i);
            IkRigSolver* s = &r->solvers[r->nsolvers++];
            const char* kind = json_get_str(o, "type", "Chain IK");
            s->kind = IKS_CHAIN;
            for (int k = 0; k < IKS_KIND_COUNT; k++)
                if (!strcmp(kind, g_ik_solver_names[k])) s->kind = (uint8_t)k;
            s->on = (uint8_t)json_get_bool(o, "on", true);
            get_str(o, "start", s->start), get_str(o, "goal", s->goal);
            s->goal_mask = (uint32_t)json_get_num(o, "goals", 0);
            json_get_floats(o, "pole", s->pole, 3);
            s->root_stiffness = (float)json_get_num(o, "root_stiffness", 0.5);
            s->axis[0] = 1.0f;
            json_get_floats(o, "axis", s->axis, 3);
            s->iterations = json_get_int(o, "iterations", 10);
        }
        const JsonValue* bs = json_get(&doc.root, "bones");
        for (size_t i = 0; i < json_len(bs) && r->nbones < IK_MAX_BONE_SETTINGS; i++) {
            const JsonValue* o = json_at(bs, i);
            IkBoneSetting* b = &r->bones[r->nbones++];
            get_str(o, "bone", b->bone);
            b->stiffness = (float)json_get_num(o, "stiffness", 0.0), b->limit = (float)json_get_num(o, "limit", 0.0);
        }
        K.rigs[K.nrigs++] = r;
        json_free(&doc);
    }
    free(t);
    return r;
}

bool ikrig_save(IkRig* r) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1), jw_kint(&w, "skeleton", r->skeleton);
    jw_key(&w, "goals");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < r->ngoals; i++) {
        const IkRigGoal* g = &r->goals[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "name", g->name), jw_kstr(&w, "bone", g->bone), jw_kstr(&w, "actor", g->target_actor);
        jw_kfloats(&w, "offset", g->offset, 3), jw_kfloat(&w, "alpha", g->alpha), jw_kbool(&w, "rotation", g->rotation);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "solvers");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < r->nsolvers; i++) {
        const IkRigSolver* s = &r->solvers[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "type", g_ik_solver_names[s->kind]), jw_kbool(&w, "on", s->on);
        jw_kstr(&w, "start", s->start), jw_kstr(&w, "goal", s->goal), jw_kint(&w, "goals", s->goal_mask);
        jw_kfloats(&w, "pole", s->pole, 3), jw_kfloat(&w, "root_stiffness", s->root_stiffness), jw_kfloats(&w, "axis", s->axis, 3);
        jw_kint(&w, "iterations", s->iterations);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "bones");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < r->nbones; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "bone", r->bones[i].bone), jw_kfloat(&w, "stiffness", r->bones[i].stiffness), jw_kfloat(&w, "limit", r->bones[i].limit);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = write_all(r->path, &w);
    jw_free(&w);
    if (ok) r->dirty = false;
    return ok;
}

static int bone_of(const SkelMesh* m, const char* name) {
    if (!name || !name[0]) return -1;
    for (uint32_t b = 0; b < m->nbones; b++)
        if (!_stricmp(m->bones[b].name, name)) return (int)b;
    return -1;
}
// A bone whose name has all the words (case-insensitive), the side's word when given ("hand" + "l"): the rig's guesses
static int bone_like(const SkelMesh* m, const char* word, int side) {
    int best = -1;
    for (uint32_t b = 0; b < m->nbones; b++) {
        char lo[ANIM_NAME];
        snprintf(lo, sizeof lo, "%s", m->bones[b].name);
        for (char* c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strstr(lo, word)) continue;
        if (side) {
            bool l = strstr(lo, "_l") || strstr(lo, "left") || strstr(lo, ".l") || strstr(lo, "l_") == lo || strstr(lo, "_l_");
            bool r = strstr(lo, "_r") || strstr(lo, "right") || strstr(lo, ".r") || strstr(lo, "r_") == lo || strstr(lo, "_r_");
            if ((side < 0 && !l) || (side > 0 && !r) || (l && r)) continue;
        }
        if (best < 0 || strlen(m->bones[b].name) < strlen(m->bones[best].name)) best = (int)b; // (the shortest: "hand_l", not "hand_l_twist")
    }
    return best;
}
// The ancestor of bone b by n steps (or the root)
static int up_n(const SkelMesh* m, int b, int n) {
    while (b >= 0 && n-- > 0 && m->bones[b].parent >= 0) b = m->bones[b].parent;
    return b;
}

uint32_t ikrig_create(const char* folder_vpath, const char* name, uint32_t skeleton) {
    const SkelMesh* m = anim_mesh(skeleton);
    IkRig* r = calloc(1, sizeof *r); // (an asset)
    if (!r || !m || K.nrigs >= MAX_ASSETS) return free(r), 0u;
    char vpath[700];
    snprintf(vpath, sizeof vpath, "%s/%s.rikrig", folder_vpath, name);
    disk_of(vpath, r->path, sizeof r->path);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->hash = vpath_hash(vpath), r->skeleton = skeleton, r->version = 1;
    static const struct { const char* goal; const char* word; int side; } guess[] = {
        { "LeftHand", "hand", -1 }, { "RightHand", "hand", 1 }, { "LeftFoot", "foot", -1 }, { "RightFoot", "foot", 1 }, { "Head", "head", 0 } };
    int pelvis = bone_like(m, "pelvis", 0);
    if (pelvis < 0) pelvis = bone_like(m, "hips", 0);
    if (pelvis < 0) pelvis = 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof guess / sizeof guess[0]; i++) {
        int b = bone_like(m, guess[i].word, guess[i].side);
        if (b < 0) continue;
        IkRigGoal* g = &r->goals[r->ngoals];
        snprintf(g->name, ANIM_NAME, "%s", guess[i].goal), snprintf(g->bone, ANIM_NAME, "%s", m->bones[b].name);
        g->alpha = 0.0f; // (off until a Blueprint or an actor moves it)
        if (i < 4) mask |= 1u << r->ngoals;
        r->ngoals++;
    }
    if (mask) {
        IkRigSolver* s = &r->solvers[r->nsolvers++];
        s->kind = IKS_FULL_BODY, s->on = 1, s->goal_mask = mask, s->root_stiffness = 0.7f, s->iterations = 12;
        snprintf(s->start, ANIM_NAME, "%s", m->bones[pelvis].name);
    }
    int head = bone_like(m, "head", 0);
    if (head >= 0) {
        IkRigSolver* s = &r->solvers[r->nsolvers++];
        s->kind = IKS_LOOK_AT, s->on = 1, s->axis[0] = 1.0f, s->iterations = 1;
        snprintf(s->start, ANIM_NAME, "%s", m->bones[head].name), snprintf(s->goal, ANIM_NAME, "Head");
    }
    if (!ikrig_save(r)) return free(r), 0u;
    K.rigs[K.nrigs++] = r;
    (void)up_n;
    return r->hash;
}

PhysicsAsset* phys_find(uint32_t hash) {
    scan();
    for (uint32_t i = 0; i < K.nphys; i++)
        if (K.phys[i]->hash == hash) return K.phys[i];
    return NULL;
}
uint32_t phys_count(void) { scan(); return K.nphys; }
PhysicsAsset* phys_at(uint32_t i) { return i < K.nphys ? K.phys[i] : NULL; }

PhysicsAsset* phys_load(const char* vpath) {
    uint32_t h = vpath_hash(vpath);
    for (uint32_t i = 0; i < K.nphys; i++)
        if (K.phys[i]->hash == h) return K.phys[i];
    if (K.nphys >= MAX_ASSETS) return NULL;
    char path[800];
    disk_of(vpath, path, sizeof path);
    size_t n;
    char* t = read_all(path, &n);
    if (!t) return NULL;
    JsonDoc doc;
    char err[160];
    PhysicsAsset* p = NULL;
    if (json_parse(t, n, &doc, err, sizeof err) && (p = calloc(1, sizeof *p))) { // (an asset)
        snprintf(p->path, sizeof p->path, "%s", path);
        name_of(path, p->name);
        p->hash = h, p->version = 1, p->skeleton = (uint32_t)json_get_num(&doc.root, "skeleton", 0);
        const JsonValue* bs = json_get(&doc.root, "bodies");
        for (size_t i = 0; i < json_len(bs) && p->nbodies < PHYS_MAX_BODIES; i++) {
            const JsonValue* o = json_at(bs, i);
            PhysBody* b = &p->bodies[p->nbodies++];
            get_str(o, "bone", b->bone);
            b->shape = (uint8_t)json_get_int(o, "shape", 0), b->simulate = (uint8_t)json_get_bool(o, "simulate", true);
            b->radius = (float)json_get_num(o, "radius", 5), b->length = (float)json_get_num(o, "length", 20);
            json_get_floats(o, "extent", b->extent, 3), json_get_floats(o, "offset", b->offset, 3);
            b->axis[1] = 1.0f;
            json_get_floats(o, "axis", b->axis, 3);
            b->mass = (float)json_get_num(o, "mass", 5), b->strength = (float)json_get_num(o, "strength", 0);
            b->muscle = (float)json_get_num(o, "muscle", 1);
        }
        const JsonValue* cs = json_get(&doc.root, "constraints");
        for (size_t i = 0; i < json_len(cs) && p->nconstraints < PHYS_MAX_CONSTRAINTS; i++) {
            const JsonValue* o = json_at(cs, i);
            PhysConstraint* c = &p->constraints[p->nconstraints++];
            c->parent = (uint8_t)json_get_int(o, "parent", 0), c->child = (uint8_t)json_get_int(o, "child", 0);
            c->swing = (float)json_get_num(o, "swing", 45), c->twist = (float)json_get_num(o, "twist", 30);
            // (Q10) the anatomical limits; a file from before: its symmetric cone and twist, no tone
            c->kind = (uint8_t)json_get_int(o, "kind", PJ_BALL);
            c->bend_min = (float)json_get_num(o, "bend_min", -c->swing), c->bend_max = (float)json_get_num(o, "bend_max", c->swing);
            c->side_min = (float)json_get_num(o, "side_min", -c->swing), c->side_max = (float)json_get_num(o, "side_max", c->swing);
            c->twist_min = (float)json_get_num(o, "twist_min", -c->twist), c->twist_max = (float)json_get_num(o, "twist_max", c->twist);
            json_get_floats(o, "bend", c->bend, 3);
            c->damping = (float)json_get_num(o, "damping", 6), c->tone = (float)json_get_num(o, "tone", 0);
        }
        const JsonValue* ch = json_get(&doc.root, "chains"); // (Q10) the dynamic bones
        for (size_t i = 0; i < json_len(ch) && p->nchains < PHYS_MAX_CHAINS; i++) {
            const JsonValue* o = json_at(ch, i);
            DynChain* c = &p->chains[p->nchains++];
            phys_chain_preset(c, json_get_int(o, "preset", DYN_HAIR));
            get_str(o, "bone", c->bone), get_str(o, "end", c->end);
            c->on = (uint8_t)json_get_bool(o, "on", true), c->collide = (uint8_t)json_get_bool(o, "collide", true);
            c->stiffness = (float)json_get_num(o, "stiffness", c->stiffness), c->damping = (float)json_get_num(o, "damping", c->damping);
            c->gravity = (float)json_get_num(o, "gravity", c->gravity), c->wind = (float)json_get_num(o, "wind", c->wind);
            c->inertia = (float)json_get_num(o, "inertia", c->inertia), c->radius = (float)json_get_num(o, "radius", c->radius);
            c->limit = (float)json_get_num(o, "limit", c->limit);
        }
        p->density = (float)json_get_num(&doc.root, "density", 1.0), p->tone = (float)json_get_num(&doc.root, "tone", 1.0);
        snprintf(p->getup_front, ANIM_NAME, "%s", json_get_str(&doc.root, "getup_front", ""));
        snprintf(p->getup_back, ANIM_NAME, "%s", json_get_str(&doc.root, "getup_back", ""));
        K.phys[K.nphys++] = p;
        json_free(&doc);
    }
    free(t);
    return p;
}

bool phys_save(PhysicsAsset* p) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1), jw_kint(&w, "skeleton", p->skeleton);
    jw_key(&w, "bodies");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < p->nbodies; i++) {
        const PhysBody* b = &p->bodies[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "bone", b->bone), jw_kint(&w, "shape", b->shape), jw_kbool(&w, "simulate", b->simulate);
        jw_kfloat(&w, "radius", b->radius), jw_kfloat(&w, "length", b->length);
        jw_kfloats(&w, "extent", b->extent, 3), jw_kfloats(&w, "offset", b->offset, 3), jw_kfloats(&w, "axis", b->axis, 3);
        jw_kfloat(&w, "mass", b->mass), jw_kfloat(&w, "strength", b->strength), jw_kfloat(&w, "muscle", b->muscle);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "constraints");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < p->nconstraints; i++) {
        const PhysConstraint* c = &p->constraints[i];
        jw_obj_begin(&w);
        jw_kint(&w, "parent", c->parent), jw_kint(&w, "child", c->child), jw_kfloat(&w, "swing", c->swing), jw_kfloat(&w, "twist", c->twist);
        jw_kint(&w, "kind", c->kind), jw_kfloat(&w, "bend_min", c->bend_min), jw_kfloat(&w, "bend_max", c->bend_max);
        jw_kfloat(&w, "side_min", c->side_min), jw_kfloat(&w, "side_max", c->side_max);
        jw_kfloat(&w, "twist_min", c->twist_min), jw_kfloat(&w, "twist_max", c->twist_max), jw_kfloats(&w, "bend", c->bend, 3);
        jw_kfloat(&w, "damping", c->damping), jw_kfloat(&w, "tone", c->tone);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "chains"); // (Q10)
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < p->nchains; i++) {
        const DynChain* c = &p->chains[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "bone", c->bone), jw_kstr(&w, "end", c->end), jw_kint(&w, "preset", c->preset);
        jw_kbool(&w, "on", c->on), jw_kbool(&w, "collide", c->collide);
        jw_kfloat(&w, "stiffness", c->stiffness), jw_kfloat(&w, "damping", c->damping), jw_kfloat(&w, "gravity", c->gravity);
        jw_kfloat(&w, "wind", c->wind), jw_kfloat(&w, "inertia", c->inertia), jw_kfloat(&w, "radius", c->radius), jw_kfloat(&w, "limit", c->limit);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_kfloat(&w, "density", p->density), jw_kfloat(&w, "tone", p->tone);
    jw_kstr(&w, "getup_front", p->getup_front), jw_kstr(&w, "getup_back", p->getup_back);
    jw_obj_end(&w);
    bool ok = write_all(p->path, &w);
    jw_free(&w);
    if (ok) p->dirty = false;
    return ok;
}

// The bodies from the skeleton (at its bind): a capsule from each long enough bone's head to its child's; the short
// ones (fingers, twists, toes) ride the nearest body above. Constraints to the nearest body above, their limits by
// the joint's kind (elbows and knees mostly swing one way, the spine little, the neck and the shoulders more)

// (2026-11-23, the user: "the bones' sizes and the thickness of the mesh each bone takes, no overlap, the right size for
// the ragdoll, all automatic when the physics is generated") Each body as thick as its part of the skin: the vertices
// that bone moves most (weight >= 0.5) measured from the body's segment (its center for a sphere), the radius at their
// median (a histogram of 1 cm bins: one pass over the vertices); then the bodies not joined to each other
// pushed apart at the bind (both shrunk by their share of the overlap, never below 35% of what they had): the self
// collision of a ragdoll starts at rest. True: the skin was read.
static void closest_seg_seg(V3 p1, V3 q1, V3 p2, V3 q2, V3* c1, V3* c2); // (below)
static void body_segment(const SkelMesh* m, int bone, const PhysBody* bd, V3* a, V3* b) {
    M3 rr = row_rot(m->bind_world[bone], NULL);
    V3 c = add3(row_pos(m->bind_world[bone]), m3v(&rr, v3(bd->offset[0], bd->offset[1], bd->offset[2])));
    V3 ax = norm3(m3v(&rr, v3(bd->axis[0], bd->axis[1], bd->axis[2])));
    float h = bd->shape == PHYS_CAPSULE ? bd->length * 0.5f : 0.0f;
    *a = sub3(c, mul3(ax, h)), *b = add3(c, mul3(ax, h));
}
static bool bodies_joined(const PhysicsAsset* p, uint32_t i, uint32_t k) {
    for (uint32_t c = 0; c < p->nconstraints; c++)
        if ((p->constraints[c].parent == i && p->constraints[c].child == k) || (p->constraints[c].parent == k && p->constraints[c].child == i)) return true;
    return false;
}
#define FIT_BINS 96
static bool phys_fit_to_mesh(PhysicsAsset* p, const SkelMesh* m, const int* bone_of_body, PhysFitStats* st) {
    static uint32_t hist[PHYS_MAX_BODIES][FIT_BINS]; // (a generation, not a frame: the main thread)
    memset(hist, 0, sizeof hist);
    int body_of[ANIM_MAX_BONES];
    for (uint32_t b = 0; b < m->nbones; b++) body_of[b] = -1;
    for (uint32_t i = 0; i < p->nbodies; i++)
        if (bone_of_body[i] >= 0) body_of[bone_of_body[i]] = (int)i;
    static V3 seg[PHYS_MAX_BODIES][2];
    for (uint32_t i = 0; i < p->nbodies; i++) body_segment(m, bone_of_body[i], &p->bodies[i], &seg[i][0], &seg[i][1]);
    uint64_t verts = 0;
    for (uint32_t k = 0; k < m->nskins; k++) {
        const GpuVertex* v = NULL;
        const uint32_t* idx = NULL;
        uint32_t first = 0, count = 0;
        int32_t off = 0;
        render_mesh_cpu(m->skins[k].mesh, &v, &idx, &first, &count, &off);
        if (!v || !idx) continue;
        for (uint32_t i = 0; i < count; i++) {
            const GpuVertex* g = &v[(int64_t)idx[first + i] + off];
            uint32_t j, w;
            memcpy(&j, &g->uv2[0], 4), memcpy(&w, &g->uv2[1], 4);
            int best = -1;
            uint32_t bw = 0;
            for (int q = 0; q < 4; q++)
                if (((w >> (q * 8)) & 255u) > bw) bw = (w >> (q * 8)) & 255u, best = (int)((j >> (q * 8)) & 255u);
            if (bw < 128u || best < 0 || best >= (int)m->nbones) continue;
            int bi = body_of[best];
            if (bi < 0) continue;
            V3 pnt = v3(g->pos[0], g->pos[1], g->pos[2]), c = closest_on_seg(seg[bi][0], seg[bi][1], pnt);
            int bin = (int)len3(sub3(pnt, c));
            hist[bi][bin < FIT_BINS ? bin : FIT_BINS - 1]++, verts++;
        }
    }
    if (st) st->vertices = (uint32_t)verts;
    if (!verts) return false;
    for (uint32_t i = 0; i < p->nbodies; i++) {
        PhysBody* bd = &p->bodies[i];
        uint32_t n = 0;
        for (int b = 0; b < FIT_BINS; b++) n += hist[i][b];
        if (n < 12 || bd->shape == PHYS_BOX) continue; // (too few to say: as the bones gave it)
        uint32_t want = n / 2, acc = 0; // (the median: the skin's surface around the bone)
        int b = 0;
        while (b < FIT_BINS - 1 && acc + hist[i][b] < want) acc += hist[i][b++];
        float r = (float)b + (hist[i][b] ? (float)(want - acc) / (float)hist[i][b] : 0.5f); // (inside the bin)
        float full = bd->shape == PHYS_CAPSULE ? bd->length + 2.0f * bd->radius : 0.0f;
        r = fmaxf(r, 1.0f);
        if (bd->shape == PHYS_CAPSULE) r = fminf(r, full * 0.5f); // (a capsule never shorter than its two caps)
        bd->radius = r;
        if (bd->shape == PHYS_CAPSULE) bd->length = fmaxf(full - 2.0f * r, 0.0f);
        bd->extent[0] = bd->extent[1] = bd->extent[2] = r;
        if (st) st->fitted++;
    }
    // apart where they overlap (joined bodies may: their joint lets them)
    static float r0[PHYS_MAX_BODIES];
    for (uint32_t i = 0; i < p->nbodies; i++) r0[i] = p->bodies[i].radius, body_segment(m, bone_of_body[i], &p->bodies[i], &seg[i][0], &seg[i][1]);
    for (int round = 0; round < 6; round++) {
        bool any = false;
        for (uint32_t i = 0; i < p->nbodies; i++)
            for (uint32_t k = i + 1; k < p->nbodies; k++) {
                if (bodies_joined(p, i, k)) continue;
                PhysBody *a = &p->bodies[i], *b = &p->bodies[k];
                V3 c1, c2;
                closest_seg_seg(seg[i][0], seg[i][1], seg[k][0], seg[k][1], &c1, &c2);
                float d = len3(sub3(c1, c2)), over = a->radius + b->radius - d + 0.5f;
                if (over <= 0.0f) continue;
                float sa = a->radius / (a->radius + b->radius);
                float ra = fmaxf(a->radius - over * sa, r0[i] * 0.35f), rb = fmaxf(b->radius - over * (1.0f - sa), r0[k] * 0.35f);
                if (ra < a->radius - 1e-3f || rb < b->radius - 1e-3f) {
                    any = true;
                    if (st) st->shrunk++;
                }
                if (a->shape == PHYS_CAPSULE) a->length += 2.0f * (a->radius - ra);
                if (b->shape == PHYS_CAPSULE) b->length += 2.0f * (b->radius - rb);
                a->radius = ra, b->radius = rb;
                a->extent[0] = a->extent[1] = a->extent[2] = ra, b->extent[0] = b->extent[1] = b->extent[2] = rb;
            }
        if (!any) break;
    }
    if (st) { // (what's left overlapping: the measure)
        st->overlaps = 0;
        for (uint32_t i = 0; i < p->nbodies; i++)
            for (uint32_t k = i + 1; k < p->nbodies; k++) {
                if (bodies_joined(p, i, k)) continue;
                V3 c1, c2;
                closest_seg_seg(seg[i][0], seg[i][1], seg[k][0], seg[k][1], &c1, &c2);
                if (len3(sub3(c1, c2)) < p->bodies[i].radius + p->bodies[k].radius - 0.25f) st->overlaps++;
            }
    }
    return true;
}
static PhysFitStats g_fit;
const PhysFitStats* phys_fit_stats(void) { return &g_fit; }

static V3 skel_forward(const SkelMesh* m); // (below)
void phys_autogen(PhysicsAsset* p, const SkelMesh* m) {
    double fit_t0 = re_time_seconds();
    memset(&g_fit, 0, sizeof g_fit);
    p->nbodies = p->nconstraints = 0;
    int body_of[ANIM_MAX_BONES];
    // (2026-11-23) what each bone is (rig_detect.h), the skins' weights read now when the load had none: a bone with a
    // role is named the Unreal way below (thigh_l, calf_l, spine_02...), so the kinds, limits and muscles follow the
    // role on any rig; controls, IK handles, poles, mechanism, twist, finger and chain bones get no body
    const RigInfo* ri = rig_info(m);
    if (ri && !ri->had_skin) rig_detect((SkelMesh*)m), ri = rig_info(m);
    static char ue_of[ANIM_MAX_BONES][ANIM_NAME];
    for (uint32_t b = 0; b < m->nbones; b++) ue_of[b][0] = 0;
    static const struct { int role; const char* ue; } UEN[] = {
        { CR_HIPS, "pelvis" }, { CR_SPINE1, "spine_01" }, { CR_SPINE2, "spine_02" }, { CR_SPINE3, "spine_03" }, { CR_SPINE4, "spine_04" },
        { CR_CHEST, "spine_05" }, { CR_NECK1, "neck_01" }, { CR_NECK2, "neck_02" }, { CR_HEAD, "head" }, { CR_JAW, "jaw" },
        { CR_CLAVICLE_L, "clavicle_l" }, { CR_UPPER_ARM_L, "upperarm_l" }, { CR_FOREARM_L, "lowerarm_l" }, { CR_HAND_L, "hand_l" },
        { CR_THIGH_L, "thigh_l" }, { CR_SHIN_L, "calf_l" }, { CR_FOOT_L, "foot_l" }, { CR_TOE_L, "ball_l" },
        { CR_CLAVICLE_R, "clavicle_r" }, { CR_UPPER_ARM_R, "upperarm_r" }, { CR_FOREARM_R, "lowerarm_r" }, { CR_HAND_R, "hand_r" },
        { CR_THIGH_R, "thigh_r" }, { CR_SHIN_R, "calf_r" }, { CR_FOOT_R, "foot_r" }, { CR_TOE_R, "ball_r" } };
    for (size_t u = 0; ri && u < sizeof UEN / sizeof UEN[0]; u++)
        if (ri->role[UEN[u].role] >= 0) snprintf(ue_of[ri->role[UEN[u].role]], ANIM_NAME, "%s", UEN[u].ue);
    static int bone_of_body[PHYS_MAX_BODIES];
    float span = 0.0f; // the skeleton's height (the size of the small bones' threshold)
    for (uint32_t b = 0; b < m->nbones; b++) span = fmaxf(span, fabsf(m->bind_world[b][2][3]));
    float min_len = fmaxf(span * 0.04f, 2.0f);
    float ground_z = 1e30f; // (2026-11-19) the bind's ground (its lowest joint's height, less the ankle: the root's), the foot capsules'
    for (uint32_t b = 0; b < m->nbones; b++) ground_z = fminf(ground_z, m->bind_world[b][2][3]);
    ground_z = fminf(ground_z, m->nbones ? m->bind_world[0][2][3] : 0.0f);
    V3 fwd = skel_forward(m);
    for (uint32_t b = 0; b < m->nbones; b++) {
        body_of[b] = -1;
        // its child: the farthest of its children (the main one: a hand's middle finger, the spine's next)
        int child = -1;
        float best = 0.0f;
        V3 head = row_pos(m->bind_world[b]);
        for (uint32_t c = b + 1; c < m->nbones; c++)
            if (m->bones[c].parent == (int)b) {
                float d = len3(sub3(row_pos(m->bind_world[c]), head));
                if (d > best) best = d, child = (int)c;
            }
        char lo[ANIM_NAME];
        snprintf(lo, sizeof lo, "%s", ue_of[b][0] ? ue_of[b] : m->bones[b].name);
        for (char* c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
        int rk = ri ? ri->kind[b] : RIGB_DEFORM;
        bool not_body = ri && !ue_of[b][0] && (rk == RIGB_CONTROL || rk == RIGB_IK_HANDLE || rk == RIGB_POLE || rk == RIGB_MECH || rk == RIGB_TWIST ||
                                               rk == RIGB_FINGER || rk == RIGB_CHAIN || (rk == RIGB_HELPER && ri->had_skin));
        bool skip = not_body || strstr(lo, "twist") || strstr(lo, "ik_") || strstr(lo, "root") == lo || strstr(lo, "finger") || strstr(lo, "thumb") ||
                    strstr(lo, "index") || strstr(lo, "middle") || strstr(lo, "ring") || strstr(lo, "pinky") || strstr(lo, "eye") || strstr(lo, "jaw");
        bool head_bone = strstr(lo, "head") != NULL, foot = strstr(lo, "foot") != NULL;
        bool end = child < 0 && (strstr(lo, "hand") || foot || head_bone);
        if (skip || p->nbodies >= PHYS_MAX_BODIES || (best < min_len && !end && !head_bone)) continue;
        // a hub (3+ children: a pelvis, a chest) is a sphere at its children's middle
        V3 mid = v3(0, 0, 0);
        int nch = 0;
        for (uint32_t c = b + 1; c < m->nbones; c++)
            if (m->bones[c].parent == (int)b) mid = add3(mid, row_pos(m->bind_world[c])), nch++;
        bool hub = nch >= 3;
        M3 rr = row_rot(m->bind_world[b], NULL);
        PhysBody* bd = &p->bodies[p->nbodies];
        memset(bd, 0, sizeof *bd);
        snprintf(bd->bone, ANIM_NAME, "%s", m->bones[b].name);
        bool leaf_foot = foot && child < 0;
        bool sphere = head_bone || hub;
        bd->shape = sphere ? PHYS_SPHERE : PHYS_CAPSULE, bd->simulate = 1;
        int pb = m->bones[b].parent;
        V3 dir_w = child >= 0 ? sub3(row_pos(m->bind_world[child]), head)
                   : pb >= 0 ? sub3(head, row_pos(m->bind_world[pb])) : m3v(&rr, v3(0, 1, 0)); // (an end: on along its parent's line)
        float len = child >= 0 ? best : fmaxf(span * 0.06f, 5.0f);
        V3 dir_l = norm3(m3tv(&rr, dir_w));
        bd->axis[0] = dir_l.x, bd->axis[1] = dir_l.y, bd->axis[2] = dir_l.z;
        V3 c;
        if (hub) {
            mid = mul3(mid, 1.0f / (float)nch);
            V3 ml = m3tv(&rr, sub3(mid, head));
            bd->radius = fmaxf(len3(ml) * 0.9f, 3.0f), c = mul3(ml, 0.6f);
        } else if (head_bone) bd->radius = fmaxf(span * 0.06f, 4.0f), c = mul3(dir_l, bd->radius * 0.8f);
        else if (leaf_foot) { // (2026-11-19) along the foot (the skeleton's forward), its underside at the ground of the bind
            // (a human foot: the heel about the ankle's height behind it, the toes twice it ahead)
            float ankle = fminf(fmaxf(head.z - ground_z, 2.0f), span * 0.08f), heel = ankle * 0.95f, toe = ankle * 2.1f;
            bd->radius = fminf(fmaxf(span * 0.03f, 2.0f), ankle * 0.6f);
            // (its ends a little lower and in: the rounded ends cover the foot's front and back edges - a foot pointing down
            // lying on its front sank its toes 5 cm under the floor; standing it sits 1 cm deeper at the toes, in the ground's
            // 1 cm below the feet)
            V3 hs = v3(fwd.x * (bd->radius * 0.8f - heel), fwd.y * (bd->radius * 0.8f - heel), bd->radius * 0.8f - ankle);
            V3 ts = v3(fwd.x * (toe - bd->radius * 0.8f), fwd.y * (toe - bd->radius * 0.8f), bd->radius * 0.8f - ankle);
            V3 fl = norm3(m3tv(&rr, sub3(ts, hs)));
            len = len3(sub3(ts, hs)) + 2.0f * bd->radius;
            bd->axis[0] = fl.x, bd->axis[1] = fl.y, bd->axis[2] = fl.z;
            c = m3tv(&rr, mul3(add3(hs, ts), 0.5f));
        } else if (sphere) bd->radius = fmaxf(span * 0.035f, 3.0f), c = v3(0, 0, 0);
        else bd->radius = fminf(fmaxf(len * 0.22f, 2.0f), span * 0.08f), c = mul3(dir_l, len * 0.5f);
        bd->length = sphere ? 0.0f : fmaxf(len - 2.0f * bd->radius, 0.0f);
        bd->offset[0] = c.x, bd->offset[1] = c.y, bd->offset[2] = c.z;
        bd->extent[0] = bd->extent[1] = bd->extent[2] = bd->radius;
        if (p->density <= 0.0f) p->density = 1.0f;
        bd->mass = fmaxf(phys_body_volume_mass(p, bd), 0.3f); // (Q10: its shape's volume x a body's density, kg)
        bone_of_body[p->nbodies] = (int)b;
        body_of[b] = (int)p->nbodies++;
        // its constraint: the nearest body above
        int up = m->bones[b].parent;
        while (up >= 0 && body_of[up] < 0) up = m->bones[up].parent;
        if (up >= 0 && p->nconstraints < PHYS_MAX_CONSTRAINTS) {
            PhysConstraint* cs = &p->constraints[p->nconstraints++];
            cs->parent = (uint8_t)body_of[up], cs->child = (uint8_t)body_of[b];
            bool hinge = strstr(lo, "calf") || strstr(lo, "shin") || strstr(lo, "knee") || strstr(lo, "lowerarm") || strstr(lo, "forearm") ||
                         strstr(lo, "elbow") || strstr(lo, "lowerleg");
            bool spine = strstr(lo, "spine") || strstr(lo, "chest");
            cs->swing = hinge ? 70.0f : spine ? 20.0f : head_bone || strstr(lo, "neck") ? 40.0f : strstr(lo, "foot") || strstr(lo, "hand") ? 35.0f : 60.0f;
            cs->twist = hinge ? 10.0f : spine ? 10.0f : 30.0f;
            // (Q10) its anatomy: the kind its limits come from (phys_joint_defaults, below, once every body is made)
            bool shoulder = strstr(lo, "upperarm") || strstr(lo, "shoulder") || (strstr(lo, "arm") && !hinge && !strstr(lo, "fore"));
            bool hip = strstr(lo, "thigh") || strstr(lo, "upperleg") || strstr(lo, "hip");
            bool wrist = strstr(lo, "foot") || strstr(lo, "hand") || strstr(lo, "paw") || strstr(lo, "ankle") || strstr(lo, "wrist");
            cs->kind = hinge ? PJ_HINGE : spine ? PJ_SPINE : head_bone || strstr(lo, "neck") ? PJ_NECK : shoulder ? PJ_SHOULDER : hip ? PJ_HIP
                     : wrist ? PJ_WRIST : strstr(lo, "tail") ? PJ_TAIL : PJ_BALL;
        }
    }
    g_fit.bodies = p->nbodies;
    phys_fit_to_mesh(p, m, bone_of_body, &g_fit); // (2026-11-23) as thick as the skin, apart at the bind
    for (uint32_t c = 0; c < p->nconstraints; c++) phys_joint_defaults(p, m, c, p->constraints[c].kind);
    if (p->tone <= 0.0f) p->tone = 1.0f;
    // (2026-11-16) the active ragdoll's muscles by kind: the legs and the spine hold the body, the arms and the head less
    for (uint32_t i = 0; i < p->nbodies; i++) {
        char lo[ANIM_NAME];
        snprintf(lo, sizeof lo, "%s", ue_of[bone_of_body[i]][0] ? ue_of[bone_of_body[i]] : p->bodies[i].bone);
        for (char* c = lo; *c; c++) *c = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
        p->bodies[i].muscle = strstr(lo, "hand") || strstr(lo, "finger") || strstr(lo, "paw") ? 0.6f
                            : strstr(lo, "head") || strstr(lo, "neck") ? 0.8f
                            : strstr(lo, "arm") || strstr(lo, "clavicle") || strstr(lo, "shoulder") ? 0.75f
                            : strstr(lo, "tail") ? 0.5f : 1.0f;
    }
    phys_masses_from_volume(p);
    // (2026-11-23) the rig's hanging chains (tails, ears, hair, chains, capes, accessories) as dynamic bones with their
    // preset; one the asset has already (by its first bone) is kept as the user left it
    for (uint32_t c = 0; ri && c < ri->nchains; c++) {
        const RigChainInfo* ch = &ri->chains[c];
        bool have = false;
        for (uint32_t k = 0; k < p->nchains && !have; k++) have = !_stricmp(p->chains[k].bone, m->bones[ch->first].name);
        if (have) continue;
        int k = phys_add_chain(p, m->bones[ch->first].name, rig_chain_preset(ch->kind));
        if (k >= 0 && ch->last != ch->first) snprintf(p->chains[k].end, ANIM_NAME, "%s", m->bones[ch->last].name);
        if (k >= 0) g_fit.chains++;
    }
    g_fit.ms = (re_time_seconds() - fit_t0) * 1000.0;
    // (Q10) the get-up montages, when the project has them and their clip's bones are all in this skeleton (the
    // Mannequin's: AM_GetUp_Front / AM_GetUp_Back, anim_getup.inc); a set one is kept
    static const char* const getup[2] = { "AM_GetUp_Front", "AM_GetUp_Back" };
    for (int k = 0; k < 2; k++) {
        char* slot = k ? p->getup_back : p->getup_front;
        const AnimMontage* mo = slot[0] ? NULL : anim_montage_name(getup[k]);
        const AnimSeq* sq = mo ? anim_seq_name(mo->anim) : NULL;
        bool fits = sq && sq->ntracks > 0;
        for (uint32_t t = 0; fits && t < sq->ntracks; t++) fits = bone_of(m, sq->track_bone[t]) >= 0;
        if (fits) snprintf(slot, ANIM_NAME, "%s", getup[k]);
    }
    p->version++;
}

void phys_masses_from_volume(PhysicsAsset* p) {
    if (p->density <= 0.0f) p->density = 1.0f;
    for (uint32_t i = 0; i < p->nbodies; i++) p->bodies[i].mass = fmaxf(phys_body_volume_mass(p, &p->bodies[i]), 0.3f);
    // the masses evened out along the joints (Unreal's advice: neighbors within some times each other - a light link
    // between two heavy bodies makes any solver shake): the lighter raised to a quarter of the heavier, a few rounds
    for (int round = 0; round < 4; round++)
        for (uint32_t i = 0; i < p->nconstraints; i++) {
            PhysBody *a = &p->bodies[p->constraints[i].parent], *c = &p->bodies[p->constraints[i].child];
            if (a->mass < c->mass * 0.25f) a->mass = c->mass * 0.25f;
            if (c->mass < a->mass * 0.25f) c->mass = a->mass * 0.25f;
        }
    p->version++;
}

// --- (Q10) the anatomical limits, the masses by volume, the dynamic chains' presets ---
const char* const g_joint_kind_names[PJ_KIND_COUNT] = { "Ball", "Hinge (Knee, Elbow)", "Spine", "Neck", "Shoulder", "Hip", "Wrist / Ankle", "Tail" };
const char* const g_dyn_preset_names[DYN_PRESET_COUNT] = { "Earring", "Hair", "Tail", "Strap / Pouch", "Heavy Bag", "Ear", "Chain", "Cape / Skirt" };

static bool lower_has(const char* name, const char* word) {
    char lo[ANIM_NAME];
    snprintf(lo, sizeof lo, "%s", name);
    for (char* c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
    return strstr(lo, word) != NULL;
}

float phys_body_volume_mass(const PhysicsAsset* p, const PhysBody* b) {
    float r = fmaxf(b->radius, 0.1f), v;
    if (b->shape == PHYS_BOX) v = 8.0f * fmaxf(b->extent[0], 0.1f) * fmaxf(b->extent[1], 0.1f) * fmaxf(b->extent[2], 0.1f);
    else if (b->shape == PHYS_SPHERE) v = 4.18879f * r * r * r;
    else v = 3.14159265f * r * r * fmaxf(b->length, 0.0f) + 4.18879f * r * r * r; // (a cylinder and its two caps)
    return v * (p && p->density > 0.0f ? p->density : 1.0f) * 0.001f;              // (cm3 x g/cm3 -> kg)
}

// The skeleton's forward at its bind (level): the feet toward their toes; else (a quadruped, a snake) the root toward
// the head; else +Y
static V3 skel_forward(const SkelMesh* m) {
    V3 f = v3(0, 0, 0);
    for (uint32_t b = 0; b < m->nbones; b++) {
        if (!lower_has(m->bones[b].name, "foot") && !lower_has(m->bones[b].name, "ankle")) continue;
        for (uint32_t c = b + 1; c < m->nbones; c++)
            if (m->bones[c].parent == (int)b) {
                V3 d = sub3(row_pos(m->bind_world[c]), row_pos(m->bind_world[b]));
                f = add3(f, v3(d.x, d.y, 0.0f));
            }
    }
    if (len3(f) < 1.0f) { // (2026-11-19) no toes: from its sides - the "_r" bones' places minus the "_l" ones' is its right, and
        // forward = right x up (this engine's Mannequin faces +X, its right +Y; a mesh facing +Y has its right at -X: the
        // same rule). Before, the Mannequin fell back to +Y: its knees bent sideways and face down was its side.
        V3 right = v3(0, 0, 0);
        for (uint32_t b = 0; b < m->nbones; b++) {
            const char* n = m->bones[b].name;
            size_t l = strlen(n);
            if (l < 3 || n[l - 2] != '_' || (n[l - 1] != 'r' && n[l - 1] != 'R')) continue;
            char other[ANIM_NAME];
            snprintf(other, sizeof other, "%s", n);
            other[l - 1] = n[l - 1] == 'r' ? 'l' : 'L';
            for (uint32_t c = 0; c < m->nbones; c++)
                if (!strcmp(m->bones[c].name, other)) right = add3(right, sub3(row_pos(m->bind_world[b]), row_pos(m->bind_world[c])));
        }
        right.z = 0.0f;
        if (len3(right) > 1.0f) f = cross3(norm3(right), v3(0, 0, 1));
    }
    if (len3(f) < 1.0f) {
        int head = -1;
        for (uint32_t b = 0; b < m->nbones && head < 0; b++)
            if (lower_has(m->bones[b].name, "head")) head = (int)b;
        if (head >= 0) {
            V3 d = sub3(row_pos(m->bind_world[head]), row_pos(m->bind_world[0]));
            f = v3(d.x, d.y, 0.0f);
            if (len3(f) < fmaxf(fabsf(d.z), 1.0f) * 0.5f) f = v3(0, 0, 0); // (upright: the head is above, not ahead)
        }
    }
    return len3(f) < 1e-3f ? v3(0, 1, 0) : norm3(f);
}

void phys_joint_defaults(PhysicsAsset* p, const SkelMesh* m, uint32_t ci, int kind) {
    if (!p || !m || ci >= p->nconstraints) return;
    PhysConstraint* c = &p->constraints[ci];
    c->kind = (uint8_t)(kind >= 0 && kind < PJ_KIND_COUNT ? kind : PJ_BALL);
    if (c->parent >= p->nbodies || c->child >= p->nbodies) return;
    const PhysBody *bpb = &p->bodies[c->parent], *bcb = &p->bodies[c->child];
    int pb = bone_of(m, bpb->bone), cb = bone_of(m, bcb->bone);
    if (pb < 0 || cb < 0) return;
    M3 rp = row_rot(m->bind_world[pb], NULL), rc = row_rot(m->bind_world[cb], NULL);
    V3 axc = norm3(m3v(&rc, norm3(v3(bcb->axis[0], bcb->axis[1], bcb->axis[2]))));
    V3 axp = norm3(m3v(&rp, norm3(v3(bpb->axis[0], bpb->axis[1], bpb->axis[2]))));
    V3 fwd = skel_forward(m), up = v3(0, 0, 1);
    V3 joint = row_pos(m->bind_world[cb]), mid = row_pos(m->bind_world[0]);
    V3 out = sub3(joint, mid);
    out = sub3(out, mul3(fwd, dot3(out, fwd))), out.z = 0.0f; // (the side the limb is on, away from the middle)
    out = len3(out) > 1e-3f ? norm3(out) : v3(1, 0, 0);
    float L[6]; // bend min, max, side min, max, twist min, max (deg)
    V3 bend = fwd;
    float damping = 6.0f, tone = 0.1f;
    const char* nm = bcb->bone;
    switch (c->kind) {
    case PJ_HINGE: {
        bool knee = lower_has(nm, "calf") || lower_has(nm, "shin") || lower_has(nm, "knee") || lower_has(nm, "lowerleg") || lower_has(nm, "leg");
        bend = knee ? mul3(fwd, -1.0f) : fwd; // (a knee folds backward, an elbow forward)
        V3 already = sub3(axc, mul3(axp, dot3(axc, axp)));
        if (len3(already) > 0.14f) bend = norm3(already); // (bent at the bind already, 8 deg or more: that is its way)
        float lk[6] = { -2, 140, -4, 4, -10, 10 }, le[6] = { -2, 145, -5, 5, -45, 45 };
        memcpy(L, knee ? lk : le, sizeof L);
        damping = 8.0f, tone = 0.12f;
        break;
    }
    case PJ_SPINE: { float l[6] = { -15, 25, -15, 15, -10, 10 }; memcpy(L, l, sizeof L); damping = 10.0f, tone = 0.25f; break; }
    case PJ_NECK: { float l[6] = { -30, 40, -30, 30, -35, 35 }; memcpy(L, l, sizeof L); damping = 8.0f, tone = 0.2f; break; }
    case PJ_SHOULDER: {
        // flexion forward far (up in front), extension back little; abduction (out and up) from the arm's elevation at
        // the bind - hanging, an A pose or a T pose: the same reach, along the body to almost straight up - across the
        // body little
        bend = fwd;
        float e0 = asinf(fmaxf(fminf(axc.z, 1.0f), -1.0f)) * 57.29578f;
        V3 fb = sub3(fwd, mul3(axc, dot3(fwd, axc)));
        V3 hinge = cross3(axc, len3(fb) > 1e-3f ? norm3(fb) : up);
        V3 ab = sub3(out, mul3(axc, dot3(out, axc)));
        ab = len3(ab) > 0.3f ? norm3(ab) : up; // (the way it lifts out: outward, or up for a T pose's arm)
        bool out_pos = dot3(mul3(hinge, -1.0f), ab) > 0.0f; // (a positive side swing turns the arm toward -hinge)
        float abd = fminf(fmaxf(80.0f - e0, 20.0f), 170.0f), add = fminf(fmaxf(e0 + 115.0f, 20.0f), 120.0f);
        L[0] = -45.0f, L[1] = 150.0f, L[2] = out_pos ? -add : -abd, L[3] = out_pos ? abd : add, L[4] = -50.0f, L[5] = 50.0f;
        damping = 5.0f, tone = 0.1f;
        break;
    }
    case PJ_HIP: {
        bend = fwd; // (flexion forward far, extension back little; out to the side more than across)
        V3 hinge = cross3(axc, norm3(sub3(fwd, mul3(axc, dot3(fwd, axc)))));
        bool out_pos = dot3(mul3(hinge, -1.0f), out) > 0.0f;
        L[0] = -20.0f, L[1] = 110.0f, L[2] = out_pos ? -20.0f : -45.0f, L[3] = out_pos ? 45.0f : 20.0f, L[4] = -30.0f, L[5] = 30.0f;
        damping = 6.0f, tone = 0.12f;
        break;
    }
    case PJ_WRIST: { bend = up; float l[6] = { -40, 35, -20, 20, -15, 15 }; memcpy(L, l, sizeof L); damping = 6.0f, tone = 0.15f; break; }
    case PJ_TAIL: { bend = up; float l[6] = { -60, 60, -60, 60, -20, 20 }; memcpy(L, l, sizeof L); damping = 8.0f, tone = 0.1f; break; } // (a tail bends a lot: tight, it fought the ground it lay on)
    default: { float s = c->swing > 0.0f ? c->swing : 45.0f, t = c->twist > 0.0f ? c->twist : 30.0f, l[6] = { -s, s, -s, s, -t, t }; memcpy(L, l, sizeof L); break; }
    }
    // the bend's way in the parent's frame, across the child's axis
    bend = sub3(bend, mul3(axc, dot3(bend, axc)));
    if (len3(bend) < 1e-3f) bend = fabsf(axc.z) < 0.9f ? cross3(axc, up) : cross3(axc, v3(1, 0, 0));
    V3 bl = norm3(m3tv(&rp, norm3(bend)));
    c->bend[0] = bl.x, c->bend[1] = bl.y, c->bend[2] = bl.z;
    c->bend_min = L[0], c->bend_max = L[1], c->side_min = L[2], c->side_max = L[3], c->twist_min = L[4], c->twist_max = L[5];
    c->swing = fmaxf(fmaxf(-L[0], L[1]), fmaxf(-L[2], L[3])), c->twist = fmaxf(-L[4], L[5]);
    c->damping = damping, c->tone = tone;
}

void phys_chain_preset(DynChain* c, int preset) {
    static const float k[DYN_PRESET_COUNT][7] = {
        // stiffness, damping, gravity, wind, inertia, radius, limit
        { 0.02f, 0.06f, 1.0f, 0.6f, 1.0f, 0.8f, 75.0f },  // Earring: swings long, light
        { 0.10f, 0.16f, 1.0f, 1.0f, 0.85f, 2.0f, 85.0f }, // Hair
        { 0.35f, 0.14f, 0.6f, 0.3f, 0.9f, 4.0f, 60.0f },  // Tail: holds its shape, sways
        { 0.20f, 0.25f, 1.0f, 0.2f, 0.8f, 2.0f, 50.0f },  // Strap / Pouch
        { 0.45f, 0.35f, 1.0f, 0.05f, 0.6f, 8.0f, 25.0f }, // Heavy Bag: a backpack, a holster
        { 0.60f, 0.10f, 0.45f, 0.35f, 1.0f, 2.5f, 55.0f }, // (2026-11-23) Ear: a rabbit's stands up and flops, springs back
        { 0.00f, 0.03f, 1.0f, 0.05f, 1.0f, 1.2f, 0.0f },  // (2026-11-23) Chain: no shape of its own, swings long, links free
        { 0.12f, 0.22f, 1.0f, 0.9f, 0.9f, 3.0f, 70.0f },  // (2026-11-23) Cape / Skirt: drapes, the wind takes it
    };
    int p = preset >= 0 && preset < DYN_PRESET_COUNT ? preset : DYN_HAIR;
    c->preset = (uint8_t)p, c->on = 1, c->collide = 1;
    c->stiffness = k[p][0], c->damping = k[p][1], c->gravity = k[p][2], c->wind = k[p][3], c->inertia = k[p][4], c->radius = k[p][5], c->limit = k[p][6];
}
int phys_add_chain(PhysicsAsset* p, const char* bone, int preset) {
    if (!p || p->nchains >= PHYS_MAX_CHAINS || !bone || !bone[0]) return -1;
    DynChain* c = &p->chains[p->nchains];
    memset(c, 0, sizeof *c);
    snprintf(c->bone, ANIM_NAME, "%s", bone);
    phys_chain_preset(c, preset);
    p->version++;
    return (int)p->nchains++;
}

uint32_t phys_create(const char* folder_vpath, const char* name, uint32_t skeleton) {
    const SkelMesh* m = anim_mesh(skeleton);
    PhysicsAsset* p = calloc(1, sizeof *p); // (an asset)
    if (!p || !m || K.nphys >= MAX_ASSETS) return free(p), 0u;
    char vpath[700];
    snprintf(vpath, sizeof vpath, "%s/%s.rphys", folder_vpath, name);
    disk_of(vpath, p->path, sizeof p->path);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->hash = vpath_hash(vpath), p->skeleton = skeleton;
    phys_autogen(p, m);
    if (!phys_save(p)) return free(p), 0u;
    K.phys[K.nphys++] = p;
    return p->hash;
}

// --- Per-instance state (Blueprint's goals, the simulation) ---
typedef struct BodyState {
    V3 x, v, w, px;   // the center, velocities, last substep's center
    Q4 q, pq;         // the frame (the bone's), last substep's
    float inv_mass, inv_mass0; // (inv_mass0: its own; the active ragdoll's held root has 0 while it stands)
    M3 inv_i;         // the inverse inertia in the body's frame (a capsule's: around its own axis, not the bone's)
    V3 anchor_p;      // (constraints: the joint in the parent's frame; kept from the start)
    V3 ref_axis;      // (the child's axis in the parent's frame at the start: the swing's middle)
    V3 ref_perp;      // (the twist's reference: in the child's frame)
    V3 ref_perp_p;    // (the same at the start in the parent's frame)
    V3 ref_bend, ref_hinge; // (Q10: the joint's frame at the bind pose in the parent's: the bend's way, the hinge's axis)
    Q4 ref_rel;       // (Q10: the child's turn in the parent's frame at the bind: the twist is measured from it)
    float inner_cos;  // (Q10: the cos of the widest cone inside all its swing ranges: within it, no limit to test)
    int bone;         // the bone it moves (-1: missing)
    bool sim;         // simulated now (else kinematic: follows the animation)
    bool touch;       // touched the world this substep
    bool pushed;      // (Q10) a contact (the world's or another body's) moved it this substep: its joints' limits again
    M3 iw;            // the inverse inertia in the world (made at each substep's start: XPBD's usual approximation)
} BodyState;

typedef struct PState {
    Entity e;
    int mode;                         // 0 off, 1 all, 2 below a bone
    char bone[ANIM_NAME];
    float blend;                      // the physics' weight (1)
    bool started, asleep, begun;
    uint32_t version;                 // the asset's when started
    float still_t;
    float sub_acc;                    // (2026-11-19) time not yet stepped (less than a substep)
    V3 impulse_all;
    char impulse_bone[ANIM_NAME];
    V3 impulse;
    BodyState b[PHYS_MAX_BODIES];
    uint64_t no_col[PHYS_MAX_BODIES]; // (the pairs that do not collide: neighbors, and those touching at the start)
    float ik_error;                   // (the IK's largest miss this frame)
    // (2026-11-16) the active ragdoll (mode PHYS_ACTIVE)
    float act, act_target, act_rate;  // the overall strength now, where it goes, how fast (per second)
    float bk[PHYS_MAX_BODIES], bk_target[PHYS_MAX_BODIES], bk_rate[PHYS_MAX_BODIES]; // per body (a bone's strength, a hit)
    float hitk[PHYS_MAX_BODIES], hit_rate[PHYS_MAX_BODIES]; // a hit's weakening (1: none), its recovery per second
    int profile;
    int bal;                          // BAL_*
    float bal_t, support, lie_t, lie_v; // the time in the state, the balance's hold (0..1), lying still, the root's speed smoothed
    bool auto_getup;
    float getup_delay;
    int root;                         // the body without a parent (the pelvis)
    uint64_t feet;                    // the lowest ends (feet, paws): planted while it stands
    uint8_t kind[PHYS_MAX_BODIES];    // each body's part (legs, spine, arms, head, the rest: the profiles')
    float root_dz;                    // (the root's height over the feet at the start: a fall is lower than half of it)
    bool move_pending;                // (ikphys_main) the actor moved by move_d
    V3 move_d;
    char bone_req[ANIM_NAME];         // (Set Active Ragdoll Strength on a bone: applied by phys_apply, which has the skeleton)
    float bone_req_strength, bone_req_rate;
    char hit_bone[ANIM_NAME];         // (Apply Hit Reaction: the same)
    float hit_weaken, hit_recover;
    V3 hit_impulse;
    bool hit_pending, getup_now;
    // Set IK Goal
    struct { char name[ANIM_NAME]; float pos[3], rot[3], alpha; bool set; } goals[IK_MAX_GOALS];
    // (Q10) the plain ragdoll's tone: each joint's turn when it started; the bodies' own blend weights (Set All Bodies
    // Below Physics Blend Weight); lying face down; the get-up montage asked of the main thread, the actor's turn
    Q4 trest[PHYS_MAX_CONSTRAINTS];
    float rag_t;                      // (the time since the ragdoll started: its tone fades as it lies)
    float stun_t;                     // (an active ragdoll hit hard: its balance starts coming back after this long)
    float bw[PHYS_MAX_BODIES], bw_target[PHYS_MAX_BODIES], bw_rate[PHYS_MAX_BODIES];
    char bw_bone[ANIM_NAME];
    float bw_req, bw_req_rate;
    bool bw_pending, protect;
    bool landed;                      // (2026-11-19) this fall's chest on the ground: the arms stop bracing
    uint8_t arm_role[PHYS_MAX_CONSTRAINTS]; // (1 the upper arm, 2 the forearm, 3 the hand: the brace's; 0 not an arm)
    int face_down;                    // (-1: not down)
    const char* montage_req;          // (a get-up montage: ikphys_main plays it)
    int getup_phase;                  // (0 none; 1 the montage asked; 2 aligned: blending into it; 3 the montage playing, animated)
    const char* getup_mo;             // (the get-up montage asked: its name, the Physics Asset's)
    bool getup_done;                  // (ikphys_main: the montage is over, or a hit came: the physics back)
    bool blend_back;                  // (up: the blend back to 1 when the bodies start again from the animation)
    float move_yaw;                   // (deg: the actor turned around its place, with move_d)
    V3 align_anim, align_body;        // (the animated root and the body's, where the actor is turned and moved to)
    float align_dyaw;
    float limit_over, self_pen;       // (the stats: the most a joint went past its limits, two bodies' deepest overlap)
    int limit_worst;
    // (Q10) the dynamic bones (dyn_apply): their particles (world, cm), their bones, parents and rest lengths
    bool dyn_off, dyn_started;
    uint32_t dyn_version, ndyn;
    V3 dyn_wind;
    V3 dyn_root_prev;                 // (the skeleton's root last frame: its motion, the inertia's)
    struct DynNode { V3 p, pp; int16_t bone, parent_node, chain, tip; float len; V3 tip_local; } dyn[64];
    uint64_t dyn_bind_skip[PHYS_MAX_CHAINS]; // (the bodies a chain starts inside at the bind: not collided, as the ragdoll's pairs)
    // (Q10) the bodies' capsules at the last pose and the one before (the cloth's colliders)
    uint32_t ncaps;
    bool caps_ok;
    float caps[PHYS_MAX_BODIES][7], caps_prev[PHYS_MAX_BODIES][6];
    int8_t cap_body[PHYS_MAX_BODIES];
    int16_t cap_bone[PHYS_MAX_BODIES]; // (each body's bone, found once per asset version: no names compared a frame)
    uint32_t cap_version;
    const void* cap_mesh;
    // (Q27.5) GTA 6's kind of reactions: holding a wound (the other side's arm reaches for it), limping (the hit leg weak,
    // the pelvis down and leaning to it), writhing on the ground, flailing in a high fall; and the budget (dormant: past
    // the nearest ACTIVE_BUDGET active ones, its simulation paused: standing it plays the animation, lying it sleeps)
    char wound_bone[ANIM_NAME];
    V3 wound_world, wound_local;
    float wound_req;
    bool wound_pending;
    int wound_body, limp_body, limp_side;
    uint64_t limp_mask;
    float wound_t, wound_sev, limp_t, limp_sev, writhe_t, fall_t, act_time;
    bool highfall, dormant;
    uint8_t arm_side[PHYS_MAX_CONSTRAINTS]; // (0 left, 1 right, 2 unknown)
} PState;

static PState* state_of(Entity e, bool make) {
    if (e == ENTITY_NONE) return NULL;
    for (uint32_t i = 0; i < K.nstates; i++)
        if (K.states[i]->e == e) return K.states[i];
    if (!make || K.nstates >= MAX_STATES) return NULL;
    PState* s = calloc(1, sizeof *s); // (a character's state: made once, on the main thread)
    if (!s) return NULL;
    s->e = e, s->blend = 1.0f;
    s->act = s->act_target = 1.0f, s->act_rate = 4.0f, s->support = 1.0f, s->auto_getup = true, s->getup_delay = 1.5f, s->root = -1;
    for (int i = 0; i < PHYS_MAX_BODIES; i++) s->bk[i] = s->bk_target[i] = 1.0f, s->bk_rate[i] = 4.0f, s->hitk[i] = 1.0f, s->hit_rate[i] = 1.0f;
    for (int i = 0; i < PHYS_MAX_BODIES; i++) s->bw[i] = s->bw_target[i] = 1.0f, s->bw_rate[i] = 4.0f; // (Q10)
    s->face_down = -1;
    s->wound_body = s->limp_body = -1; // (Q27.5)
    K.states[K.nstates++] = s;
    return s;
}

void phys_set_simulate(Entity sk, int mode, const char* bone) {
    PState* s = state_of(sk, true);
    if (!s) return;
    if (s->mode != mode || (bone && strcmp(bone, s->bone))) s->started = false;
    if (mode == PHYS_ACTIVE && s->mode != PHYS_ACTIVE) s->bal = BAL_BALANCED, s->support = 1.0f, s->bal_t = s->lie_t = 0.0f, s->blend = 1.0f;
    s->mode = mode, s->asleep = false, s->still_t = 0.0f, s->begun = true;
    snprintf(s->bone, sizeof s->bone, "%s", bone ? bone : "");
}
void phys_set_blend(Entity sk, float weight) {
    PState* s = state_of(sk, true);
    if (s) s->blend = fminf(fmaxf(weight, 0.0f), 1.0f);
}
void phys_add_impulse(Entity sk, const char* bone, const float impulse[3]) {
    PState* s = state_of(sk, true);
    if (!s) return;
    if (bone && bone[0]) snprintf(s->impulse_bone, ANIM_NAME, "%s", bone), s->impulse = add3(s->impulse, v3(impulse[0], impulse[1], impulse[2]));
    else s->impulse_all = add3(s->impulse_all, v3(impulse[0], impulse[1], impulse[2]));
    s->asleep = false, s->still_t = 0.0f;
}
int phys_simulating(Entity sk) {
    PState* s = state_of(sk, false);
    return s ? s->mode : 0;
}

// --- The active ragdoll's controls (2026-11-16) ---
const char* const g_muscle_profile_names[MUSCLE_PROFILE_COUNT] = { "Default", "Relaxed", "Tense", "Protective", "Drunk", "Limp" };
// a profile's strength per kind of bone: legs, spine, arms, head, the rest (tails, wings, props)
static const float g_profile_k[MUSCLE_PROFILE_COUNT][5] = {
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },     // Default
    { 0.75f, 0.7f, 0.45f, 0.55f, 0.5f },  // Relaxed
    { 1.35f, 1.35f, 1.25f, 1.25f, 1.1f }, // Tense
    { 1.0f, 1.1f, 1.4f, 1.2f, 0.9f },     // Protective (the arms in front)
    { 0.55f, 0.65f, 0.35f, 0.45f, 0.4f }, // Drunk
    { 0.04f, 0.04f, 0.03f, 0.03f, 0.03f } // Limp
};
static int bone_kind(const char* name) {
    char lo[ANIM_NAME];
    snprintf(lo, sizeof lo, "%s", name);
    for (char* c = lo; *c; c++) *c = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    if (strstr(lo, "thigh") || strstr(lo, "calf") || strstr(lo, "leg") || strstr(lo, "knee") || strstr(lo, "foot") || strstr(lo, "toe") ||
        strstr(lo, "shin") || strstr(lo, "paw") || strstr(lo, "hoof")) return 0;
    if (strstr(lo, "pelvis") || strstr(lo, "hip") || strstr(lo, "spine") || strstr(lo, "chest") || strstr(lo, "root")) return 1;
    if (strstr(lo, "arm") || strstr(lo, "hand") || strstr(lo, "clavicle") || strstr(lo, "shoulder") || strstr(lo, "finger") || strstr(lo, "elbow")) return 2;
    if (strstr(lo, "head") || strstr(lo, "neck")) return 3;
    return 4;
}
void phys_set_active_strength(Entity sk, const char* bone, float strength, float blend_time) {
    PState* s = state_of(sk, true);
    if (!s) return;
    float rate = blend_time > 1e-3f ? 1.0f / blend_time : 1e6f;
    if (strength < 0.0f) strength = 0.0f;
    if (!bone || !bone[0]) {
        s->act_target = strength, s->act_rate = rate;
        return;
    }
    s->bone_req_strength = strength, s->bone_req_rate = rate; // (the bodies below the bone: phys_apply knows the skeleton)
    snprintf(s->bone_req, sizeof s->bone_req, "%s", bone);
}
bool phys_set_muscle_profile(Entity sk, const char* name) {
    PState* s = state_of(sk, true);
    if (!s || !name) return false;
    for (int i = 0; i < MUSCLE_PROFILE_COUNT; i++)
        if (!_stricmp(name, g_muscle_profile_names[i]) || (name[0] >= '0' && name[0] <= '9' && atoi(name) == i)) {
            s->profile = i;
            return true;
        }
    return false;
}
void phys_hit_reaction(Entity sk, const char* bone, const float impulse[3], float weaken, float recover_time) {
    PState* s = state_of(sk, true);
    if (!s) return;
    phys_add_impulse(sk, bone, impulse);
    snprintf(s->hit_bone, sizeof s->hit_bone, "%s", bone ? bone : "");
    s->hit_weaken = fminf(fmaxf(weaken, 0.0f), 1.0f), s->hit_recover = fmaxf(recover_time, 0.05f), s->hit_pending = true;
    s->hit_impulse = impulse ? v3(impulse[0], impulse[1], impulse[2]) : v3(0, 0, 0);
}
// (Q27.5) A wound at a world point on a bone, its severity 0..1: by the region, the hand of the other side holds it (the
// torso, an arm, the head) or the leg limps; past 0.7 the knees give (it falls) and lying it writhes a while before it
// gets up. Its muscles there weaken by the severity. The impulse is the caller's (Apply Hit Reaction)
void phys_wound(Entity sk, const char* bone, const float point[3], float severity) {
    PState* s = state_of(sk, true);
    if (!s || !bone || !point) return;
    snprintf(s->wound_bone, sizeof s->wound_bone, "%s", bone);
    s->wound_world = v3(point[0], point[1], point[2]);
    s->wound_req = fminf(fmaxf(severity, 0.0f), 1.0f), s->wound_pending = true;
    s->asleep = false, s->still_t = 0.0f;
}
float phys_limp(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->limp_t > 0.0f ? s->limp_sev : 0.0f;
}
bool phys_holding_wound(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->wound_t > 0.0f && s->wound_body >= 0;
}
bool phys_writhing(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->mode == PHYS_ACTIVE && s->bal == BAL_FALLEN && s->writhe_t > 0.0f;
}
bool phys_high_fall(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->highfall;
}
int phys_balance_state(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->mode == PHYS_ACTIVE ? s->bal : -1;
}
void phys_get_up(Entity sk) {
    PState* s = state_of(sk, false);
    if (s && s->mode == PHYS_ACTIVE && s->bal == BAL_FALLEN) s->getup_now = true;
}
void phys_set_auto_get_up(Entity sk, bool on, float delay) {
    PState* s = state_of(sk, true);
    if (s) s->auto_getup = on, s->getup_delay = fmaxf(delay, 0.0f);
}
// --- (Q10) the bodies' blend, the facing, the dynamic bones' controls, the capsules for the cloth ---
void phys_set_blend_below(Entity sk, const char* bone, float weight, float blend_time) {
    PState* s = state_of(sk, true);
    if (!s) return;
    snprintf(s->bw_bone, sizeof s->bw_bone, "%s", bone ? bone : "");
    s->bw_req = fminf(fmaxf(weight, 0.0f), 1.0f), s->bw_req_rate = blend_time > 1e-3f ? 1.0f / blend_time : 1e6f, s->bw_pending = true;
}
int phys_face_down(Entity sk) {
    PState* s = state_of(sk, false);
    return s && s->mode == PHYS_ACTIVE && (s->bal == BAL_FALLEN || s->bal == BAL_GETTING_UP) ? s->face_down : -1;
}
void dyn_set_enabled(Entity sk, bool on) {
    PState* s = state_of(sk, true);
    if (s) s->dyn_off = !on, s->dyn_started = false;
}
void dyn_set_wind(Entity sk, const float wind[3]) {
    PState* s = state_of(sk, true);
    if (s && wind) s->dyn_wind = v3(wind[0], wind[1], wind[2]);
}
void dyn_reset(Entity sk) {
    PState* s = state_of(sk, false);
    if (s) s->dyn_started = false;
}
uint32_t phys_capsules_near(const float center[3], float reach, PhysCapsule* out, uint32_t cap) {
    uint32_t n = 0;
    V3 c = v3(center[0], center[1], center[2]);
    for (uint32_t i = 0; i < K.nstates && n < cap; i++) {
        const PState* s = K.states[i];
        if (!s->caps_ok) continue;
        for (uint32_t k = 0; k < s->ncaps && n < cap; k++) {
            const float* q = s->caps[k];
            V3 a = v3(q[0], q[1], q[2]), b = v3(q[3], q[4], q[5]);
            float d = len3(sub3(closest_on_seg(a, b, c), c));
            if (d > reach + q[6]) continue;
            PhysCapsule* o = &out[n++];
            memcpy(o->a, q, 12), memcpy(o->b, q + 3, 12), o->r = q[6];
            memcpy(o->pa, s->caps_prev[k], 12), memcpy(o->pb, s->caps_prev[k] + 3, 12);
            o->owner = s->e, o->body = s->cap_body[k];
        }
    }
    return n;
}
void ikrig_set_goal(Entity sk, const char* goal, const float pos[3], const float rot[3], float alpha) {
    PState* s = state_of(sk, true);
    if (!s || !goal) return;
    int at = -1;
    for (int i = 0; i < IK_MAX_GOALS && at < 0; i++)
        if (s->goals[i].set && !strcmp(s->goals[i].name, goal)) at = i;
    for (int i = 0; i < IK_MAX_GOALS && at < 0; i++)
        if (!s->goals[i].set) at = i;
    if (at < 0) return;
    snprintf(s->goals[at].name, ANIM_NAME, "%s", goal);
    memcpy(s->goals[at].pos, pos, 12);
    if (rot) memcpy(s->goals[at].rot, rot, 12);
    s->goals[at].alpha = alpha, s->goals[at].set = true;
}
void ikrig_clear_goal(Entity sk, const char* goal) {
    PState* s = state_of(sk, false);
    for (int i = 0; s && i < IK_MAX_GOALS; i++)
        if (s->goals[i].set && (!goal || !goal[0] || !strcmp(s->goals[i].name, goal))) s->goals[i].set = false;
}
void ikphys_forget(Entity sk) {
    for (uint32_t i = 0; i < K.nstates; i++)
        if (K.states[i]->e == sk) {
            free(K.states[i]);
            K.states[i] = K.states[--K.nstates];
            return;
        }
}
void ikphys_rebase(World* w, const float d[3]) {
    (void)w;
    V3 dv = v3(d[0], d[1], d[2]);
    for (uint32_t i = 0; i < K.nstates; i++)
        for (int b = 0; b < PHYS_MAX_BODIES; b++) K.states[i]->b[b].x = add3(K.states[i]->b[b].x, dv), K.states[i]->b[b].px = add3(K.states[i]->b[b].px, dv);
}
void ikphys_reset(void) {
    for (uint32_t i = 0; i < K.nstates; i++) free(K.states[i]);
    K.nstates = 0;
}
const IkPhysStats* ikphys_stats(void) { return &K.stats; }

PhysicsAsset* phys_temp(uint32_t skeleton, const char* name) {
    const SkelMesh* m = anim_mesh(skeleton);
    if (!m || K.nphys >= MAX_ASSETS) return NULL;
    PhysicsAsset* p = calloc(1, sizeof *p); // (an asset)
    if (!p) return NULL;
    char vp[200];
    snprintf(vp, sizeof vp, "/Temp/%s.rphys", name);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->hash = vpath_hash(vp), p->skeleton = skeleton;
    phys_autogen(p, m);
    K.phys[K.nphys++] = p;
    return p;
}
IkRig* ikrig_temp(uint32_t skeleton, const char* name) {
    if (K.nrigs >= MAX_ASSETS) return NULL;
    IkRig* r = calloc(1, sizeof *r); // (an asset)
    if (!r) return NULL;
    char vp[200];
    snprintf(vp, sizeof vp, "/Temp/%s.rikrig", name);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->hash = vpath_hash(vp), r->skeleton = skeleton, r->version = 1;
    K.rigs[K.nrigs++] = r;
    return r;
}
bool ikphys_info(Entity sk, IkPhysInfo* out) {
    const PState* s = state_of(sk, false);
    K.measure = true;
    if (!s) return false;
    memset(out, 0, sizeof *out);
    out->zmin = 1e30f, out->zmax = -1e30f, out->ik_error = s->ik_error, out->asleep = s->asleep;
    out->balance = s->mode == PHYS_ACTIVE ? s->bal : -1, out->root_z = s->root >= 0 ? s->b[s->root].x.z : 0.0f;
    out->limit_over = s->limit_over, out->limit_worst = s->limit_worst, out->self_pen = s->self_pen, out->face_down = s->face_down; // (Q10)
    for (int i = 0; i < PHYS_MAX_BODIES && s->mode; i++) {
        if (!s->b[i].sim) continue;
        if (len3(s->b[i].v) > out->vmax) out->fastest = i;
        if (len3(s->b[i].w) > out->wmax) out->spinning = i;
        out->zmin = fminf(out->zmin, s->b[i].x.z), out->zmax = fmaxf(out->zmax, s->b[i].x.z), out->vmax = fmaxf(out->vmax, len3(s->b[i].v)), out->wmax = fmaxf(out->wmax, len3(s->b[i].w));
        out->bodies++;
    }
    return true;
}
void ikphys_prepare(Entity sk, uint32_t ik_rig, uint32_t physics_asset, int simulate, const char* bone, float blend) {
    if (ik_rig && ikrig_find(ik_rig)) K.acc.rigs++, state_of(sk, true); // (its state: where the IK's miss is kept)
    const PhysicsAsset* pa = physics_asset ? phys_find(physics_asset) : NULL;
    if (!pa) return;
    if (pa->nchains || pa->nbodies) state_of(sk, true); // (Q10: its dynamic bones and its bodies' capsules for the cloth run without a ragdoll)
    PState* s = state_of(sk, false);
    if ((s && s->begun) || !simulate) return; // (once: a Blueprint's changes win after)
    phys_set_simulate(sk, simulate, bone);
    phys_set_blend(sk, blend);
}
void ikphys_main(World* w, Entity sk) {
    PState* s = state_of(sk, false);
    if (!s || s->mode != PHYS_ACTIVE) return;
    Entity actor = sk;
    const CAttach* at = ecs_get(w, sk, COMP_ATTACH);
    if (at && at->owner && ecs_alive(w, at->owner)) actor = at->owner;
    if (s->montage_req) { // (Q10) the get-up montage for how it lies: held on its first pose until the body is matched to it
        if (anim_play_montage(w, actor, s->montage_req, 1.0f)) anim_set_montage_rate(w, actor, 0.0f);
        else s->getup_phase = 0; // (not found: blended back into the animation)
        s->montage_req = NULL;
    }
    if (s->bal == BAL_GETTING_UP && s->getup_phase == 3) { // (2026-11-19) the get-up montage over (or a hit): the physics back
        const char* mo = anim_montage_playing(w, actor, NULL);
        if (!mo || !s->getup_mo || strcmp(mo, s->getup_mo) || s->hit_pending) s->getup_done = true;
    }
    if (s->move_pending) { // (a step caught, a get up: the actor where its body is; the animation follows it there)
        CTransform* t = ecs_get(w, actor, COMP_TRANSFORM);
        if (t && s->getup_phase == 2) { // (Q10) a get-up montage: the actor turned by move_yaw around its place, then moved
            // so the montage's root lands on the body's: A' = body - Rz(yaw) (anim - A)
            static float yaw_sign = 0.0f; // (which way + yaw turns, from the transform's own rows: once)
            if (yaw_sign == 0.0f) {
                CTransform y90 = { { 0, 0, 0 }, { 0, 90, 0 }, { 1, 1, 1 }, 0 };
                float rows[3][4];
                render_transform_rows(&y90, rows);
                yaw_sign = rows[1][0] > 0.0f ? 1.0f : -1.0f; // (X to +Y: counterclockwise)
            }
            float a = s->move_yaw * 0.017453293f, ca = cosf(a), sa = sinf(a);
            float rx = s->align_anim.x - t->location[0], ry = s->align_anim.y - t->location[1];
            t->location[0] = s->align_body.x - (ca * rx - sa * ry), t->location[1] = s->align_body.y - (sa * rx + ca * ry);
            t->rotation[1] += s->move_yaw * yaw_sign;
            ecs_touch(w, actor);
            anim_set_montage_rate(w, actor, 1.0f); // (its first pose under the body now: it plays from there)
        } else if (t) t->location[0] += s->move_d.x, t->location[1] += s->move_d.y, ecs_touch(w, actor);
        s->move_pending = false;
    }
    if (s->bal != BAL_BALANCED) { // (down or going down: its own movement held)
        CCharacter* c = ecs_get(w, actor, COMP_CHARACTER);
        if (c) c->input[0] = c->input[1] = 0.0f, c->velocity[0] = c->velocity[1] = 0.0f;
    }
}

// (Q27.5) the active ragdolls' budget: the ACTIVE_BUDGET nearest the view simulate; the rest are dormant next frame (one
// stumbling or getting up finishes first). GTA 6's shot reactions on every NPC: here a crowd's hundreds of bodies cost
// as many as the nearest few
#define ACTIVE_BUDGET 32
static float K_view[3];
static bool K_view_ok;
void ikphys_set_view(const float p[3]) {
    if (p) memcpy(K_view, p, sizeof K_view);
    K_view_ok = p != NULL;
}
void ikphys_frame_end(void) {
    K.stats = K.acc;
    memset(&K.acc, 0, sizeof K.acc);
    static float d2[MAX_STATES];
    uint32_t nact = 0;
    for (uint32_t i = 0; i < K.nstates; i++) {
        PState* s = K.states[i];
        d2[i] = 3e38f;
        if (s->mode != PHYS_ACTIVE || s->root < 0 || (!s->started && !s->dormant)) continue;
        nact++;
        V3 x = s->b[s->root].x;
        d2[i] = K_view_ok ? (x.x - K_view[0]) * (x.x - K_view[0]) + (x.y - K_view[1]) * (x.y - K_view[1]) + (x.z - K_view[2]) * (x.z - K_view[2]) : 0.0f;
    }
    if (nact <= ACTIVE_BUDGET) {
        for (uint32_t i = 0; i < K.nstates; i++) K.states[i]->dormant = false;
        return;
    }
    // (the budget's distance: the ACTIVE_BUDGET-th smallest, by a partial selection)
    float cut = 0.0f;
    {
        static float tmp[MAX_STATES];
        uint32_t n = 0;
        for (uint32_t i = 0; i < K.nstates; i++)
            if (d2[i] < 3e38f) tmp[n++] = d2[i];
        uint32_t k = ACTIVE_BUDGET - 1, lo = 0, hi = n - 1;
        while (lo < hi) { // (quickselect)
            float pv = tmp[(lo + hi) / 2];
            uint32_t a = lo, b = hi;
            while (a <= b) {
                while (tmp[a] < pv) a++;
                while (tmp[b] > pv) b--;
                if (a <= b) {
                    float t = tmp[a];
                    tmp[a] = tmp[b], tmp[b] = t;
                    a++;
                    if (b) b--;
                }
            }
            if (k <= b) hi = b;
            else if (k >= a) lo = a;
            else break;
        }
        cut = tmp[k];
    }
    for (uint32_t i = 0; i < K.nstates; i++) {
        PState* s = K.states[i];
        bool far = d2[i] < 3e38f && d2[i] > cut;
        if (!far) s->dormant = false;
        else if (s->bal == BAL_BALANCED || s->bal == BAL_FALLEN) s->dormant = true; // (stumbling or getting up: it finishes first)
    }
}

// --- The IK solvers ---
// The bones moved by a solver get new rows; their descendants follow (their local transforms from before)
static void propagate(const SkelMesh* m, float (*old)[3][4], float (*bones)[3][4], const bool* moved) {
    bool ch[ANIM_MAX_BONES];
    memcpy(ch, moved, sizeof(bool) * m->nbones);
    for (uint32_t b = 0; b < m->nbones; b++) {
        int p = m->bones[b].parent;
        if (p < 0 || !ch[p] || moved[b]) continue;
        // local = inv(old parent) * old bone; new = new parent * local
        float sp[3], sb[3], sn[3];
        M3 rpo = row_rot(old[p], sp), rbo = row_rot(old[b], sb), rpn = row_rot(bones[p], sn);
        M3 tpo = m3t(&rpo), loc = m3mul(&tpo, &rbo);
        V3 lp = m3tv(&rpo, sub3(row_pos(old[b]), row_pos(old[p])));
        M3 nr = m3mul(&rpn, &loc);
        V3 np = add3(row_pos(bones[p]), m3v(&rpn, lp));
        row_set(bones[b], &nr, sb, np);
        ch[b] = true;
    }
}

static float setting(const IkRig* rig, const char* bone, bool limit) {
    for (uint32_t i = 0; i < rig->nbones; i++)
        if (!_stricmp(rig->bones[i].bone, bone)) return limit ? rig->bones[i].limit : rig->bones[i].stiffness;
    return 0.0f;
}

// Multi-end FABRIK over the tree of bones from root to each goal bone: the joints' heads move, the lengths kept; the
// root moves by (1 - root_stiff) toward where the chains want it (0 for a chain: fixed); each joint's direction kept
// within its cone from its posed one and held by its stiffness. Then each bone is turned so its head-to-child segment
// follows, and the descendants come along.
#define FB_MAX 64
static void fabrik_tree(const IkRig* rig, const SkelMesh* m, float bones[][3][4], int root, const int* tips, const V3* targets,
                        const float* alphas, int ntips, float root_stiff, const V3* poles, int iterations) {
    int joint[FB_MAX], parent_j[FB_MAX], nj = 0;
    int jof[ANIM_MAX_BONES];
    for (uint32_t b = 0; b < m->nbones; b++) jof[b] = -1;
    jof[root] = 0, joint[nj] = root, parent_j[nj++] = -1;
    int path[ANIM_MAX_BONES];
    for (int t = 0; t < ntips; t++) { // the path root .. tip (tips under the root only)
        int n = 0, b = tips[t];
        while (b >= 0 && b != root && n < ANIM_MAX_BONES) path[n++] = b, b = m->bones[b].parent;
        if (b != root) continue;
        for (int i = n - 1; i >= 0; i--) {
            int bb = path[i];
            if (jof[bb] >= 0 || nj >= FB_MAX) continue;
            jof[bb] = nj, joint[nj] = bb, parent_j[nj] = jof[m->bones[bb].parent], nj++;
        }
    }
    if (nj < 2) return;
    V3 p[FB_MAX], p0[FB_MAX], dir0[FB_MAX];
    float L[FB_MAX];
    for (int j = 0; j < nj; j++) p[j] = p0[j] = row_pos(bones[joint[j]]);
    for (int j = 1; j < nj; j++) L[j] = len3(sub3(p[j], p[parent_j[j]])), dir0[j] = norm3(sub3(p[j], p[parent_j[j]]));
    // the poles: the joints between a chain's root and tip turned around the root-target axis toward the pole
    for (int t = 0; t < ntips; t++) {
        if (!poles || len3(poles[t]) < 1e-4f || jof[tips[t]] < 0) continue;
        V3 a = p[0], ax = norm3(sub3(targets[t], a));
        V3 pole_dir = sub3(poles[t], mul3(ax, dot3(poles[t], ax)));
        if (len3(pole_dir) < 1e-4f) continue;
        V3 mid = v3(0, 0, 0);
        int cnt = 0;
        for (int b = m->bones[tips[t]].parent; b >= 0 && b != root; b = m->bones[b].parent)
            if (jof[b] >= 0) mid = add3(mid, sub3(p[jof[b]], a)), cnt++;
        if (!cnt) continue;
        V3 md = sub3(mid, mul3(ax, dot3(mid, ax)));
        if (len3(md) < 1e-4f) continue;
        M3 r = rot_between(norm3(md), norm3(pole_dir), 1.0f);
        for (int b = m->bones[tips[t]].parent; b >= 0 && b != root; b = m->bones[b].parent)
            if (jof[b] >= 0) p[jof[b]] = add3(a, m3v(&r, sub3(p[jof[b]], a)));
        p[jof[tips[t]]] = add3(a, m3v(&r, sub3(p[jof[tips[t]]], a)));
    }
    V3 sum[FB_MAX];
    int cnt[FB_MAX];
    for (int it = 0; it < (iterations > 0 ? iterations : 10); it++) {
        // backward: from each tip at its target toward the root; a shared joint averages its chains
        memset(sum, 0, sizeof sum), memset(cnt, 0, sizeof cnt);
        for (int t = 0; t < ntips; t++) {
            int j = jof[tips[t]];
            if (j < 0) continue;
            V3 q = lerp3(p[j], targets[t], alphas[t]);
            sum[j] = add3(sum[j], q), cnt[j]++;
            for (int c = j; parent_j[c] >= 0; c = parent_j[c]) {
                V3 d = norm3(sub3(p[parent_j[c]], q));
                q = add3(q, mul3(d, L[c]));
                sum[parent_j[c]] = add3(sum[parent_j[c]], q), cnt[parent_j[c]]++;
            }
        }
        V3 bp[FB_MAX];
        for (int j = 0; j < nj; j++) bp[j] = cnt[j] ? mul3(sum[j], 1.0f / (float)cnt[j]) : p[j];
        // the root: where the chains want it, held by its stiffness
        p[0] = lerp3(p0[0], bp[0], 1.0f - root_stiff);
        // forward: from the root, each joint at its length along its direction, within its cone and stiffness
        for (int j = 1; j < nj; j++) {
            V3 d = norm3(sub3(bp[j], p[parent_j[j]]));
            const char* bn = m->bones[joint[parent_j[j]]].name;
            float stiff = setting(rig, bn, false), lim = setting(rig, bn, true);
            if (stiff > 0.0f) d = norm3(lerp3(d, dir0[j], stiff));
            if (lim > 0.0f) {
                float a = acosf(fmaxf(fminf(dot3(d, dir0[j]), 1.0f), -1.0f)), lr = lim * 0.017453293f;
                if (a > lr) {
                    M3 r = rot_between(dir0[j], d, lr / a);
                    d = m3v(&r, dir0[j]);
                }
            }
            p[j] = add3(p[parent_j[j]], mul3(d, L[j]));
        }
        float err = 0.0f;
        for (int t = 0; t < ntips; t++)
            if (jof[tips[t]] >= 0) err = fmaxf(err, len3(sub3(p[jof[tips[t]]], lerp3(p0[jof[tips[t]]], targets[t], alphas[t]))));
        if (err < 0.05f) break;
    }
    // the bones turned: a joint with one child joint aims its segment; with several, their mean
    float old[ANIM_MAX_BONES][3][4];
    memcpy(old, bones, sizeof(float) * 12 * m->nbones);
    bool moved[ANIM_MAX_BONES];
    memset(moved, 0, sizeof(bool) * m->nbones);
    for (int j = 0; j < nj; j++) {
        V3 od = v3(0, 0, 0), nd = v3(0, 0, 0);
        for (int c = 1; c < nj; c++)
            if (parent_j[c] == j) od = add3(od, sub3(p0[c], p0[j])), nd = add3(nd, sub3(p[c], p[j]));
        int b = joint[j];
        float s[3];
        M3 r = row_rot(old[b], s);
        if (len3(od) > 1e-4f && len3(nd) > 1e-4f) {
            M3 q = rot_between(norm3(od), norm3(nd), 1.0f);
            r = m3mul(&q, &r);
        } else if (j > 0) { // a tip: its world rotation kept (its parent's turn doesn't carry it)
        }
        row_set(bones[b], &r, s, p[j]);
        moved[b] = true;
    }
    propagate(m, old, bones, moved);
}

// The goal's place this frame: the Blueprint's (Set IK Goal), else its actor's; alpha 0: none
static bool goal_target(World* w, const PState* st, const IkRigGoal* g, V3* pos, float* alpha, M3* rot, bool* use_rot) {
    *alpha = 0.0f, *use_rot = false;
    for (int i = 0; st && i < IK_MAX_GOALS; i++)
        if (st->goals[i].set && !strcmp(st->goals[i].name, g->name)) {
            *pos = v3(st->goals[i].pos[0], st->goals[i].pos[1], st->goals[i].pos[2]);
            *alpha = st->goals[i].alpha;
            return *alpha > 0.0f;
        }
    if (!g->target_actor[0]) return false;
    // the actor by name (a read of the world: the pose jobs only read others)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!ar->count || !(ar->mask & COMP_BIT(COMP_NAME)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CName* nm = ecs_arch_column(ar, COMP_NAME);
        const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t r = 0; r < ar->count; r++)
            if (!strcmp(nm[r].text, g->target_actor)) {
                *pos = v3(t[r].location[0] + g->offset[0], t[r].location[1] + g->offset[1], t[r].location[2] + g->offset[2]);
                *alpha = g->alpha > 0.0f ? g->alpha : 1.0f;
                if (g->rotation) {
                    float ax[3][3];
                    rotator_axes(t[r].rotation, ax);
                    for (int i = 0; i < 3; i++)
                        for (int k = 0; k < 3; k++) rot->m[k][i] = ax[i][k];
                    *use_rot = true;
                }
                return true;
            }
    }
    return false;
}

void ikrig_apply(World* w, Entity sk, const IkRig* rig, const SkelMesh* m, float bones[][3][4]) {
    if (!rig || !m || !rig->ngoals) return;
    PState* st = state_of(sk, false);
    if (st) st->ik_error = -1.0f;
    V3 gpos[IK_MAX_GOALS];
    float galpha[IK_MAX_GOALS];
    M3 grot[IK_MAX_GOALS];
    bool guse[IK_MAX_GOALS], any = false;
    int gbone[IK_MAX_GOALS];
    for (uint32_t g = 0; g < rig->ngoals; g++) {
        gbone[g] = bone_of(m, rig->goals[g].bone);
        bool on = gbone[g] >= 0 && goal_target(w, st, &rig->goals[g], &gpos[g], &galpha[g], &grot[g], &guse[g]);
        if (!on) galpha[g] = 0.0f;
        any |= galpha[g] > 0.0f;
    }
    if (!any) return;
    for (uint32_t si = 0; si < rig->nsolvers; si++) {
        const IkRigSolver* s = &rig->solvers[si];
        if (!s->on) continue;
        int tips[IK_MAX_GOALS], n = 0;
        V3 tg[IK_MAX_GOALS], poles[IK_MAX_GOALS];
        float al[IK_MAX_GOALS];
        if (s->kind == IKS_LOOK_AT) {
            int b = bone_of(m, s->start), g = -1;
            for (uint32_t k = 0; k < rig->ngoals && g < 0; k++)
                if (!strcmp(rig->goals[k].name, s->goal)) g = (int)k;
            if (b < 0 || g < 0 || galpha[g] <= 0.0f) continue;
            float old[ANIM_MAX_BONES][3][4];
            memcpy(old, bones, sizeof(float) * 12 * m->nbones);
            float sc[3];
            M3 r = row_rot(bones[b], sc);
            V3 aim = norm3(m3v(&r, norm3(v3(s->axis[0], s->axis[1], s->axis[2]))));
            V3 to = norm3(sub3(gpos[g], row_pos(bones[b])));
            M3 q = rot_between(aim, to, galpha[g]);
            r = m3mul(&q, &r);
            row_set(bones[b], &r, sc, row_pos(bones[b]));
            bool moved[ANIM_MAX_BONES];
            memset(moved, 0, sizeof(bool) * m->nbones);
            moved[b] = true;
            propagate(m, old, bones, moved);
            continue;
        }
        int root = bone_of(m, s->start);
        if (root < 0) continue;
        for (uint32_t k = 0; k < rig->ngoals; k++) {
            bool mine = s->kind == IKS_FULL_BODY ? (s->goal_mask >> k) & 1u : !strcmp(rig->goals[k].name, s->goal);
            if (!mine || galpha[k] <= 0.0f) continue;
            tips[n] = gbone[k], tg[n] = gpos[k], al[n] = fminf(galpha[k], 1.0f);
            // the pole in the actor's space (the root's parent's turn ignored: the actor's)
            poles[n] = v3(s->pole[0], s->pole[1], s->pole[2]);
            n++;
        }
        if (!n) continue;
        if (s->kind == IKS_LIMB) // two bones: the start must be the tip's grandparent
            for (int t = 0; t < n; t++)
                if (up_n(m, tips[t], 2) != root) n = 0;
        fabrik_tree(rig, m, bones, root, tips, tg, al, n, s->kind == IKS_FULL_BODY ? s->root_stiffness : 1.0f, poles, s->iterations);
        for (int t = 0; st && t < n; t++) // (the miss of the goals reached all the way)
            if (al[t] >= 1.0f) st->ik_error = fmaxf(st->ik_error, len3(sub3(row_pos(bones[tips[t]]), tg[t])));
        // the goals with a rotation: their bone takes it (its children follow)
        for (uint32_t k = 0; k < rig->ngoals; k++) {
            if (!guse[k] || galpha[k] <= 0.0f) continue;
            float old[ANIM_MAX_BONES][3][4];
            memcpy(old, bones, sizeof(float) * 12 * m->nbones);
            float sc[3];
            row_rot(bones[gbone[k]], sc);
            row_set(bones[gbone[k]], &grot[k], sc, row_pos(bones[gbone[k]]));
            bool moved[ANIM_MAX_BONES];
            memset(moved, 0, sizeof(bool) * m->nbones);
            moved[gbone[k]] = true;
            propagate(m, old, bones, moved);
        }
    }
}

// --- The bodies' simulation (XPBD: positions and orientations corrected, the velocities from them) ---
static V3 world_inv_i(const BodyState* b, V3 v) { return m3v(&b->iw, v); } // I^-1 v in the world
static void make_iw(BodyState* b) { // R I^-1 R^T
    M3 r = q_m3(b->q), rt = m3t(&r), t = m3mul(&r, &b->inv_i);
    b->iw = m3mul(&t, &rt);
}
// A positional correction between two bodies' points (r: from their centers, world) toward each other by c along n
static void apply_pos(BodyState* a, BodyState* b, V3 ra, V3 rb, V3 n, float c) {
    float wa = 0.0f, wb = 0.0f;
    V3 ca = cross3(ra, n), cb = cross3(rb, n);
    if (a && a->inv_mass > 0.0f) wa = a->inv_mass + dot3(ca, world_inv_i(a, ca));
    if (b && b->inv_mass > 0.0f) wb = b->inv_mass + dot3(cb, world_inv_i(b, cb));
    float wsum = wa + wb;
    if (wsum < 1e-9f) return;
    V3 p = mul3(n, c / wsum); // (moves a by +p, b by -p)
    if (a && a->inv_mass > 0.0f) a->x = add3(a->x, mul3(p, a->inv_mass)), a->q = q_add_rot(a->q, world_inv_i(a, cross3(ra, p)), 1.0f);
    if (b && b->inv_mass > 0.0f) b->x = sub3(b->x, mul3(p, b->inv_mass)), b->q = q_add_rot(b->q, world_inv_i(b, cross3(rb, p)), -1.0f);
}
// An angular correction: turns a by +theta around axis, b by -theta (shared by their inverse inertias)
static void apply_rot(BodyState* a, BodyState* b, V3 axis, float theta) {
    float wa = a && a->inv_mass > 0.0f ? dot3(axis, world_inv_i(a, axis)) : 0.0f;
    float wb = b && b->inv_mass > 0.0f ? dot3(axis, world_inv_i(b, axis)) : 0.0f;
    if (wa + wb < 1e-9f) return;
    float l = theta / (wa + wb);
    if (wa > 0.0f) a->q = q_add_rot(a->q, world_inv_i(a, mul3(axis, l)), 1.0f);
    if (wb > 0.0f) b->q = q_add_rot(b->q, world_inv_i(b, mul3(axis, l)), -1.0f);
}

// The body's frame from its bone's rows (the frame: the bone's rotation; the center: its head + offset)
static void body_from_bone(const PhysBody* pb, const float bone[3][4], V3* x, Q4* q) {
    M3 r = row_rot(bone, NULL);
    *q = m3_q(&r);
    *x = add3(row_pos(bone), m3v(&r, v3(pb->offset[0], pb->offset[1], pb->offset[2])));
}

// A capsule's segment ends (world)
static void segment(const PhysBody* pb, const BodyState* b, V3* a, V3* c) {
    M3 r = q_m3(b->q);
    V3 ax = m3v(&r, norm3(v3(pb->axis[0], pb->axis[1], pb->axis[2])));
    float h = pb->shape == PHYS_CAPSULE ? pb->length * 0.5f : 0.0f;
    *a = sub3(b->x, mul3(ax, h)), *c = add3(b->x, mul3(ax, h));
}
static V3 closest_on_seg(V3 a, V3 b, V3 p) {
    V3 ab = sub3(b, a);
    float t = dot3(sub3(p, a), ab) / fmaxf(dot3(ab, ab), 1e-9f);
    return add3(a, mul3(ab, fminf(fmaxf(t, 0.0f), 1.0f)));
}
static void closest_seg_seg(V3 p1, V3 q1, V3 p2, V3 q2, V3* c1, V3* c2) {
    V3 d1 = sub3(q1, p1), d2 = sub3(q2, p2), r = sub3(p1, p2);
    float a = dot3(d1, d1), e = dot3(d2, d2), f = dot3(d2, r), s = 0, t = 0;
    if (a < 1e-9f && e < 1e-9f) { *c1 = p1, *c2 = p2; return; }
    if (a < 1e-9f) t = fminf(fmaxf(f / e, 0), 1);
    else {
        float c = dot3(d1, r);
        if (e < 1e-9f) s = fminf(fmaxf(-c / a, 0), 1);
        else {
            float b = dot3(d1, d2), den = a * e - b * b;
            s = den > 1e-9f ? fminf(fmaxf((b * f - c * e) / den, 0), 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = fminf(fmaxf(-c / a, 0), 1);
            else if (t > 1) t = 1, s = fminf(fmaxf((b - c) / a, 0), 1);
        }
    }
    *c1 = add3(p1, mul3(d1, s)), *c2 = add3(p2, mul3(d2, t));
}

// A sphere at c (radius r) against the boxes and the ground: pushed out (and its slide held by friction)
static int collide_sphere(BodyState* b, V3 c, float r, const float (*boxes)[6], uint32_t nb, float ground, float friction) {
    int hits = 0;
    for (uint32_t i = 0; i <= nb; i++) {
        V3 q, n;
        float d;
        if (i == nb) { // the ground (a landscape's height or the level's floor)
            if (c.z - r >= ground) continue;
            q = v3(c.x, c.y, ground), n = v3(0, 0, 1), d = r - (c.z - ground);
        } else {
            const float* bx = boxes[i];
            q = v3(fminf(fmaxf(c.x, bx[0]), bx[3]), fminf(fmaxf(c.y, bx[1]), bx[4]), fminf(fmaxf(c.z, bx[2]), bx[5]));
            V3 dv = sub3(c, q);
            float dl = len3(dv);
            if (dl >= r) continue;
            if (dl > 1e-5f) n = mul3(dv, 1.0f / dl), d = r - dl;
            else { // the center inside: out by the nearest face
                float pen[6] = { c.x - bx[0], bx[3] - c.x, c.y - bx[1], bx[4] - c.y, c.z - bx[2], bx[5] - c.z };
                int k = 0;
                for (int j = 1; j < 6; j++)
                    if (pen[j] < pen[k]) k = j;
                static const V3 nn[6] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
                n = nn[k], d = pen[k] + r;
            }
        }
        V3 rr = sub3(sub3(c, mul3(n, r)), b->x); // the contact point from the center
        // (Unreal's max depenetration: out by what it moved in this substep and at most 0.5 cm more - a body found deep
        // in a box comes out over some substeps instead of being shot out)
        float push = fminf(d, 0.5f + fmaxf(-dot3(sub3(b->x, b->px), n), 0.0f));
        apply_pos(b, NULL, rr, v3(0, 0, 0), n, push);
        // friction (XPBD's static friction): the contact point's slide since the last substep - the body's point, turned
        // with it, not its center - held back, up to the push times the friction (and always the slow creeps: at rest a
        // body lying down stays put)
        M3 rq = q_m3(b->q), rpq = q_m3(b->pq);
        V3 local = m3tv(&rq, rr);
        V3 slide = sub3(add3(b->x, rr), add3(b->px, m3v(&rpq, local)));
        slide = sub3(slide, mul3(n, dot3(slide, n)));
        float sl = len3(slide);
        if (sl > 1e-6f) apply_pos(b, NULL, rr, v3(0, 0, 0), mul3(slide, -1.0f / sl), fminf(sl, fmaxf(d * friction, 0.05f)));
        hits++;
    }
    return hits;
}

// (Q10) A joint's swing against its anatomical limits: the child's axis in the parent's frame split by the log map into
// the bend plane (toward ref_bend: + ) and across it, each within its min and max (a knee: 0..140 one way, +-4 to the
// sides). Past them: the axis (world) now and where the limits have it; the degrees past (0: in)
static float joint_swing(const PhysConstraint* pc, const PhysBody* pbc, const BodyState* bp, const BodyState* bc, V3* from, V3* to) {
    M3 rp = q_m3(bp->q), rc = q_m3(bc->q);
    V3 axc = m3v(&rc, norm3(v3(pbc->axis[0], pbc->axis[1], pbc->axis[2])));
    V3 d = m3tv(&rp, axc), X = bc->ref_axis;
    float cs = fmaxf(fminf(dot3(X, d), 1.0f), -1.0f);
    if (cs > bc->inner_cos) return 0.0f; // (within the cone every range holds: no log map, no acos)
    float ang = acosf(cs);
    V3 ax = cross3(X, d);
    float la = len3(ax), sb = 0.0f, ss = 0.0f;
    if (la > 1e-6f) ax = mul3(ax, 1.0f / la), sb = dot3(ax, bc->ref_hinge) * ang, ss = dot3(ax, bc->ref_bend) * ang;
    else if (cs < 0.0f) sb = 3.14159265f; // (straight back: half a turn, out of any limb's range)
    const float D = 0.017453293f;
    float bmin = pc->bend_min * D, bmax = pc->bend_max * D, smin = pc->side_min * D, smax = pc->side_max * D;
    float nb, ns;
    if (fabsf(bmin + bmax) < 1e-4f && fabsf(smin + smax) < 1e-4f && fabsf(bmax - smax) < 1e-4f) { // (a cone all round, a file from before: its circle)
        float r = sqrtf(sb * sb + ss * ss);
        if (r <= bmax) return 0.0f;
        nb = sb * bmax / r, ns = ss * bmax / r;
    } else { // (each way within its range - PhysX's pyramid: the nearest point inside, no jump along a long thin range)
        nb = fminf(fmaxf(sb, bmin), bmax), ns = fminf(fmaxf(ss, smin), smax);
        if (nb == sb && ns == ss) return 0.0f;
    }
    V3 s = add3(mul3(bc->ref_hinge, nb), mul3(bc->ref_bend, ns));
    float th = len3(s);
    V3 dn = X;
    if (th > 1e-6f) { // (Rodrigues: X turned around s by its length)
        V3 a = mul3(s, 1.0f / th);
        dn = add3(add3(mul3(X, cosf(th)), mul3(cross3(a, X), sinf(th))), mul3(a, dot3(a, X) * (1.0f - cosf(th))));
    }
    *from = axc, *to = norm3(m3v(&rp, dn));
    return sqrtf((sb - nb) * (sb - nb) + (ss - ns) * (ss - ns)) / D;
}
// (Q10) A joint's twist (its child's perpendicular around its axis against the parent's) within [min, max]: the turn to
// apply (rad, apply_rot's sign); the degrees past
// (the swing-twist split of the child's turn since the bind, in the parent's frame: its part around the bind's axis -
// no flip however far it swings, as a perpendicular projected on the child's plane did past 90 deg)
static float joint_twist(const PhysConstraint* pc, const PhysBody* pbc, const BodyState* bp, const BodyState* bc, V3* axis, float* turn) {
    M3 rc = q_m3(bc->q);
    V3 axc = m3v(&rc, norm3(v3(pbc->axis[0], pbc->axis[1], pbc->axis[2])));
    *axis = axc, *turn = 0.0f;
    Q4 rel = qmul((Q4){ -bp->q.x, -bp->q.y, -bp->q.z, bp->q.w }, bc->q);
    Q4 d = qmul(rel, (Q4){ -bc->ref_rel.x, -bc->ref_rel.y, -bc->ref_rel.z, bc->ref_rel.w });
    V3 X = bc->ref_axis;
    float px = d.x * X.x + d.y * X.y + d.z * X.z;
    if (fabsf(px) < 1e-7f && fabsf(d.w) < 1e-7f) return 0.0f; // (swung half a turn: no twist to tell)
    float tw = 2.0f * atan2f(px, d.w);
    if (tw > 3.14159265f) tw -= 6.2831853f;
    if (tw < -3.14159265f) tw += 6.2831853f;
    float lo = pc->twist_min * 0.017453293f, hi = pc->twist_max * 0.017453293f;
    if (tw > hi) *turn = tw - hi;
    else if (tw < lo) *turn = tw - lo;
    return fabsf(*turn) * 57.29578f;
}

static float now_ground(World* w, Entity sk, V3 at) {
    float gz;
    if (landscape_height(at.x, at.y, &gz)) return gz;
    const CTransform* t = ecs_get(w, sk, COMP_TRANSFORM);
    const CCharacter* c = ecs_get(w, sk, COMP_CHARACTER);
    float feet = t ? t->location[2] - (c ? c->half_height : 0.0f) : 0.0f;
    return feet - 1.0f; // (no box under it: its feet's level, a little under)
}

// (Q10) The chest (the highest spine body at the bind; else the root): its front from the skeleton's forward
static int chest_body(const PhysicsAsset* pa, const SkelMesh* m, const PState* st) {
    int best = st->root;
    float bz = -1e30f;
    for (uint32_t i = 0; i < pa->nbodies; i++) {
        if (st->b[i].bone < 0 || st->kind[i] != 1) continue;
        float z = m->bind_world[st->b[i].bone][2][3];
        if (z > bz) bz = z, best = (int)i;
    }
    return best;
}
// (Q10) Lying face down (1) or on its back (0): the chest's front, as it was at the bind, points down or up now
static int body_face_down(const PhysicsAsset* pa, const SkelMesh* m, const PState* st) {
    int c = chest_body(pa, m, st);
    if (c < 0 || st->b[c].bone < 0) return 0;
    M3 rb = row_rot(m->bind_world[st->b[c].bone], NULL), rn = q_m3(st->b[c].q);
    V3 f = m3v(&rn, m3tv(&rb, skel_forward(m)));
    return f.z < 0.0f ? 1 : 0;
}
// (Q10) The get-up montage's first pose is in anim (lying as its clip lies): the actor turned around its place and moved
// so that pose's root and heading (root to chest) land on the body's; ikphys_main applies it
static void align_getup(const PhysicsAsset* pa, const SkelMesh* m, PState* st, float (*anim)[3][4], const BodyState* r) {
    int c = chest_body(pa, m, st);
    V3 ax, cx;
    Q4 q;
    body_from_bone(&pa->bodies[st->root], anim[st->b[st->root].bone], &ax, &q);
    body_from_bone(&pa->bodies[c], anim[st->b[c].bone], &cx, &q);
    V3 ha = sub3(cx, ax), hb = sub3(st->b[c].x, r->x);
    float dyaw = (atan2f(hb.y, hb.x) - atan2f(ha.y, ha.x)) * 57.29578f;
    if (len3(v3(ha.x, ha.y, 0)) < 2.0f || len3(v3(hb.x, hb.y, 0)) < 2.0f) dyaw = 0.0f; // (sitting up: no heading to match)
    st->align_anim = ax, st->align_body = r->x, st->move_yaw = dyaw, st->move_pending = true;
}

void phys_apply(World* w, Entity sk, const PhysicsAsset* pa, const SkelMesh* m, float bones[][3][4], float dt) {
    PState* st = state_of(sk, false);
    if (!st || !st->mode || !pa || !pa->nbodies || !m) return;
    // (2026-11-19, a measure) REALENGINE_RAGDOLL_DT=<s>: every frame steps this long, whatever the real one (a slow PC's
    // frames on a fast one: the test ikphys_ragdoll_30fps)
    static float forced_dt = -1.0f;
    if (forced_dt < 0.0f) {
        const char* e = getenv("REALENGINE_RAGDOLL_DT");
        forced_dt = e ? (!strcmp(e, "rand") ? 1e9f : fmaxf((float)atof(e), 0.0f)) : 0.0f;
    }
    if (forced_dt > 1e8f) { // "rand": a busy PC's frames, 2.5 to 40 ms each
        static uint32_t seed = 12345u;
        seed = seed * 1664525u + 1013904223u;
        dt = 0.0025f + (float)(seed >> 8) / 16777216.0f * 0.0375f;
    } else if (forced_dt > 0.0f) dt = forced_dt;
    double t0 = re_time_seconds();
    // (Q27.5) past the budget: standing (or still getting set), the animation's pose, nothing simulated (back in the budget
    // it starts again from the animation); lying, asleep where it lies
    if (st->mode == PHYS_ACTIVE && st->dormant && (st->bal == BAL_BALANCED || st->bal == BAL_FALLEN)) {
        if (st->bal == BAL_BALANCED) {
            if (st->root >= 0 && st->b[st->root].bone >= 0) st->b[st->root].x = row_pos(bones[st->b[st->root].bone]); // (its place for the ranking)
            st->started = false;
            K.acc.ms += (re_time_seconds() - t0) * 1000.0;
            K.acc.dormant++;
            return;
        }
        st->asleep = true;
        K.acc.dormant++;
    }
    float anim[ANIM_MAX_BONES][3][4];
    memcpy(anim, bones, sizeof(float) * 12 * m->nbones);
    int below = st->mode == 2 ? bone_of(m, st->bone) : -1;
    // (2026-11-19) the get-up montage playing: the pose is the animation's, nothing simulated (as cheap as no ragdoll);
    // over (or hit: ikphys_main), the Active Ragdoll back with its bodies made from the animation now
    if (st->mode == PHYS_ACTIVE && st->bal == BAL_GETTING_UP && st->getup_phase == 3) {
        if (!st->getup_done) {
            K.acc.ms += (re_time_seconds() - t0) * 1000.0;
            return;
        }
        st->bal = BAL_BALANCED, st->started = false, st->blend = 1.0f, st->blend_back = false, st->support = 1.0f, st->bal_t = 0.0f;
        st->getup_phase = 0, st->face_down = -1, st->getup_done = false;
    }
    // started: the bodies where the animation has them, their mass and inertia, the joints' references
    if (!st->started || st->version != pa->version) {
        if (st->blend_back) st->blend = 1.0f, st->blend_back = false; // (up again: the bodies are the animation from here)
        for (uint32_t i = 0; i < pa->nbodies; i++) {
            const PhysBody* pb = &pa->bodies[i];
            BodyState* b = &st->b[i];
            memset(b, 0, sizeof *b);
            b->bone = bone_of(m, pb->bone);
            if (b->bone < 0) continue;
            body_from_bone(pb, anim[b->bone], &b->x, &b->q);
            b->px = b->x, b->pq = b->q;
            bool sim = pb->simulate && (st->mode == 1 || st->mode == PHYS_ACTIVE || (below >= 0 && (b->bone == below || up_n(m, b->bone, 0) >= 0)));
            if (st->mode == 2) { // a descendant of the bone (or the bone)
                sim = false;
                for (int u = b->bone; u >= 0 && !sim; u = m->bones[u].parent) sim = u == below;
                sim = sim && pb->simulate;
            }
            b->sim = sim;
            b->inv_mass = b->inv_mass0 = sim ? 1.0f / fmaxf(pb->mass, 0.05f) : 0.0f;
            // the inertia: a box's along the bone's axes; a capsule's (a cylinder of its length + its caps) around its
            // own axis small, across it large; a sphere's the same every way
            float mass = fmaxf(pb->mass, 0.05f), r = fmaxf(pb->radius, 0.5f);
            memset(&b->inv_i, 0, sizeof b->inv_i);
            if (pb->shape == PHYS_BOX) {
                float hx = pb->extent[0], hy = pb->extent[1], hz = pb->extent[2];
                b->inv_i.m[0][0] = 3.0f / fmaxf(mass * (hy * hy + hz * hz), 1e-3f);
                b->inv_i.m[1][1] = 3.0f / fmaxf(mass * (hx * hx + hz * hz), 1e-3f);
                b->inv_i.m[2][2] = 3.0f / fmaxf(mass * (hx * hx + hy * hy), 1e-3f);
            } else {
                float L = pb->shape == PHYS_CAPSULE ? pb->length + r : 0.0f; // (the caps: about a radius more)
                float il = 1.0f / fmaxf(mass * (pb->shape == PHYS_SPHERE ? 0.4f : 0.5f) * r * r, 1e-3f);
                float it = pb->shape == PHYS_SPHERE ? il : 1.0f / fmaxf(mass * (3.0f * r * r + L * L) / 12.0f, 1e-3f);
                // (Q10) around its own axis at least a share of across it (Unreal's Inertia Tensor Scale for a ragdoll: a
                // thin capsule - a hand, a tail's tip - spun at 40 rad/s on the ground under each correction)
                il = fminf(il, it / 0.35f);
                V3 a = norm3(v3(pb->axis[0], pb->axis[1], pb->axis[2]));
                float aa[3] = { a.x, a.y, a.z };
                for (int i = 0; i < 3; i++) // (inv = it (I - a a^T) + il a a^T)
                    for (int j = 0; j < 3; j++) b->inv_i.m[i][j] = (i == j ? it : 0.0f) + (il - it) * aa[i] * aa[j];
            }
            if (!sim) memset(&b->inv_i, 0, sizeof b->inv_i);
            // a character's speed: the ragdoll keeps it
            const CCharacter* ch = ecs_get(w, sk, COMP_CHARACTER);
            if (ch && sim) b->v = v3(ch->velocity[0], ch->velocity[1], ch->velocity[2]);
        }
        for (uint32_t c = 0; c < pa->nconstraints; c++) {
            const PhysConstraint* pc = &pa->constraints[c];
            BodyState *bp = &st->b[pc->parent], *bc = &st->b[pc->child];
            if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies || bp->bone < 0 || bc->bone < 0) continue;
            M3 rp = q_m3(bp->q), rc = q_m3(bc->q);
            V3 joint = row_pos(anim[bc->bone]); // (the child's head)
            bc->anchor_p = m3tv(&rp, sub3(joint, bp->x));
            st->trest[c] = qmul((Q4){ -bp->q.x, -bp->q.y, -bp->q.z, bp->q.w }, bc->q); // (Q10: the pose it fell in, the tone's)
            // (Q10) the limits' frame from the BIND pose (anatomical: the same whatever the pose it starts in)
            V3 xb;
            Q4 qpb, qcb;
            body_from_bone(&pa->bodies[pc->parent], m->bind_world[bp->bone], &xb, &qpb);
            body_from_bone(&pa->bodies[pc->child], m->bind_world[bc->bone], &xb, &qcb);
            rp = q_m3(qpb), rc = q_m3(qcb);
            V3 axc = m3v(&rc, norm3(v3(pa->bodies[pc->child].axis[0], pa->bodies[pc->child].axis[1], pa->bodies[pc->child].axis[2])));
            bc->ref_axis = norm3(m3tv(&rp, axc));
            V3 perp = fabsf(axc.z) < 0.9f ? norm3(cross3(axc, v3(0, 0, 1))) : norm3(cross3(axc, v3(1, 0, 0)));
            bc->ref_perp = m3tv(&rc, perp), bc->ref_perp_p = m3tv(&rp, perp);
            V3 bd = v3(pc->bend[0], pc->bend[1], pc->bend[2]);
            bd = sub3(bd, mul3(bc->ref_axis, dot3(bd, bc->ref_axis)));
            if (len3(bd) < 1e-3f) bd = m3tv(&rp, perp); // (a file from before: any way across, its limits the same all round)
            bc->ref_bend = norm3(bd), bc->ref_hinge = norm3(cross3(bc->ref_axis, bc->ref_bend));
            bc->ref_rel = qmul((Q4){ -qpb.x, -qpb.y, -qpb.z, qpb.w }, qcb);
            {
                float in = fminf(fminf(-pc->bend_min, pc->bend_max), fminf(-pc->side_min, pc->side_max)); // (deg; <= 0: the bind is at an edge)
                bc->inner_cos = in > 0.0f ? cosf(in * 0.017453293f) : 2.0f;
            }
        }
        // (2026-11-16) a sphere's inertia at least a sphere reaching its joints: a pelvis "hub" of a few cm with its hips
        // and spine 15 cm away turned wildly at each joint's correction (2000 cm/s on the ground, never at rest)
        for (uint32_t c = 0; c < pa->nconstraints; c++) {
            const PhysConstraint* pc = &pa->constraints[c];
            if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies) continue;
            uint32_t ends[2] = { pc->parent, pc->child };
            float lever[2] = { len3(st->b[pc->child].anchor_p),
                               len3(v3(pa->bodies[pc->child].offset[0], pa->bodies[pc->child].offset[1], pa->bodies[pc->child].offset[2])) };
            for (int k = 0; k < 2; k++) {
                const PhysBody* pb = &pa->bodies[ends[k]];
                BodyState* b = &st->b[ends[k]];
                if (pb->shape != PHYS_SPHERE || !b->sim || b->bone < 0) continue;
                float r = fmaxf(pb->radius, 0.5f), re = lever[k] * 0.8f;
                if (re <= r) continue;
                float inv = 1.0f / fmaxf(fmaxf(pb->mass, 0.05f) * 0.4f * re * re, 1e-3f);
                if (inv < b->inv_i.m[0][0]) b->inv_i.m[0][0] = b->inv_i.m[1][1] = b->inv_i.m[2][2] = inv;
            }
        }
        // the pairs left out: the constraint's two bodies, and any two already touching in the starting pose (a chest and
        // its upper arms: pushed apart they would fly off)
        memset(st->no_col, 0, sizeof st->no_col);
        for (uint32_t c = 0; c < pa->nconstraints; c++) {
            uint32_t a = pa->constraints[c].parent, b = pa->constraints[c].child;
            if (a < PHYS_MAX_BODIES && b < PHYS_MAX_BODIES) st->no_col[a] |= 1ull << b, st->no_col[b] |= 1ull << a;
        }
        for (uint32_t c = 0; c < pa->nconstraints; c++) // (and two joints apart along the chain: a body and its grandparent)
            for (uint32_t d = 0; d < pa->nconstraints; d++)
                if (pa->constraints[d].child == pa->constraints[c].parent) {
                    uint32_t a = pa->constraints[d].parent, b = pa->constraints[c].child;
                    if (a < PHYS_MAX_BODIES && b < PHYS_MAX_BODIES) st->no_col[a] |= 1ull << b, st->no_col[b] |= 1ull << a;
                }
        // (Q10: touching at the BIND pose, not the one it starts in: two legs crossed as it fell, a hand on the thigh, still
        // collide - the limbs never pass through each other)
        for (uint32_t i = 0; i < pa->nbodies; i++)
            for (uint32_t j = i + 1; j < pa->nbodies; j++) {
                if (st->b[i].bone < 0 || st->b[j].bone < 0) continue;
                BodyState bi = st->b[i], bj = st->b[j];
                body_from_bone(&pa->bodies[i], m->bind_world[bi.bone], &bi.x, &bi.q);
                body_from_bone(&pa->bodies[j], m->bind_world[bj.bone], &bj.x, &bj.q);
                V3 a1, b1, a2, b2, c1, c2;
                segment(&pa->bodies[i], &bi, &a1, &b1), segment(&pa->bodies[j], &bj, &a2, &b2);
                closest_seg_seg(a1, b1, a2, b2, &c1, &c2);
                if (len3(sub3(c1, c2)) < (pa->bodies[i].radius + pa->bodies[j].radius) * 1.05f) st->no_col[i] |= 1ull << j, st->no_col[j] |= 1ull << i;
            }
        st->started = true, st->version = pa->version, st->asleep = false, st->still_t = 0.0f, st->rag_t = 0.0f;
        if (st->mode == PHYS_ACTIVE) { // (2026-11-16) the root (no parent) and the feet (the lowest ends: 2 feet, 4 paws...)
            bool is_child[PHYS_MAX_BODIES] = { 0 }, is_parent[PHYS_MAX_BODIES] = { 0 };
            for (uint32_t c = 0; c < pa->nconstraints; c++)
                if (pa->constraints[c].child < PHYS_MAX_BODIES && pa->constraints[c].parent < PHYS_MAX_BODIES)
                    is_child[pa->constraints[c].child] = is_parent[pa->constraints[c].parent] = true;
            st->root = -1;
            for (uint32_t i = 0; i < pa->nbodies && st->root < 0; i++)
                if (st->b[i].bone >= 0 && st->b[i].sim && !is_child[i]) st->root = (int)i;
            // (Q10: from the BIND pose - standing - not the one it starts in: after a get up it starts crouched, mid-montage)
            float zlo = 1e30f, zhi = -1e30f, bz[PHYS_MAX_BODIES];
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                if (st->b[i].bone < 0) continue;
                V3 bx;
                Q4 bq;
                body_from_bone(&pa->bodies[i], m->bind_world[st->b[i].bone], &bx, &bq);
                bz[i] = bx.z;
                zhi = fmaxf(zhi, bz[i]);
                if (!is_parent[i]) zlo = fminf(zlo, bz[i]);
            }
            st->feet = 0;
            for (uint32_t i = 0; i < pa->nbodies; i++)
                if (st->b[i].bone >= 0 && !is_parent[i] && bz[i] < zlo + 0.15f * (zhi - zlo) + 2.0f) st->feet |= 1ull << i;
            st->root_dz = st->root >= 0 ? fmaxf(bz[st->root] - zlo, 1.0f) : 1.0f;
            for (uint32_t i = 0; i < pa->nbodies; i++) st->kind[i] = (uint8_t)bone_kind(pa->bodies[i].bone); // (once: the profile's part)
            // (Q10) the arms' joints from the shoulder down (a clavicle is not one: it stays as animated)
            memset(st->arm_role, 0, sizeof st->arm_role);
            for (int round = 0; round < 4; round++)
                for (uint32_t c = 0; c < pa->nconstraints && c < PHYS_MAX_CONSTRAINTS; c++) {
                    uint32_t ch = pa->constraints[c].child, par = pa->constraints[c].parent;
                    if (ch >= pa->nbodies || par >= pa->nbodies || st->kind[ch] != 2 || lower_has(pa->bodies[ch].bone, "clavicle")) continue;
                    int up = 0;
                    for (uint32_t d = 0; d < pa->nconstraints && d < PHYS_MAX_CONSTRAINTS; d++)
                        if (pa->constraints[d].child == par) up = st->arm_role[d];
                    st->arm_role[c] = (uint8_t)(up ? (up < 3 ? up + 1 : 3) : 1);
                }
            for (uint32_t c = 0; c < pa->nconstraints && c < PHYS_MAX_CONSTRAINTS; c++) { // (Q27.5) each arm's side by its bone's name
                const char* bn = pa->constraints[c].child < pa->nbodies ? pa->bodies[pa->constraints[c].child].bone : "";
                size_t L = strlen(bn);
                bool left = lower_has(bn, "left") || (L >= 2 && (bn[L - 2] == '_' || bn[L - 2] == '.') && (bn[L - 1] == 'l' || bn[L - 1] == 'L')) || lower_has(bn, "_l_");
                bool right = lower_has(bn, "right") || (L >= 2 && (bn[L - 2] == '_' || bn[L - 2] == '.') && (bn[L - 1] == 'r' || bn[L - 1] == 'R')) || lower_has(bn, "_r_");
                st->arm_side[c] = left ? 0 : right ? 1 : 2;
            }
        }
    }
    // (2026-11-16) the active ragdoll's frame: the strengths blended over time, a bone's request and a hit applied (the
    // skeleton is known here), each joint's target (the child's turn in its parent's frame, as animated), the root's
    const bool active = st->mode == PHYS_ACTIVE && st->root >= 0;
    static _Thread_local Q4 trel[PHYS_MAX_CONSTRAINTS];
    float kb[PHYS_MAX_BODIES];
    V3 root_tx = v3(0, 0, 0);
    Q4 root_tq = { 0, 0, 0, 1 };
    if (active) {
        float fdt = fminf(dt, 1.0f / 30.0f);
        if (st->bone_req[0]) {
            int rb = bone_of(m, st->bone_req);
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                bool under = false;
                for (int u = st->b[i].bone; u >= 0 && !under; u = m->bones[u].parent) under = u == rb;
                if (under) st->bk_target[i] = st->bone_req_strength, st->bk_rate[i] = st->bone_req_rate;
            }
            st->bone_req[0] = 0;
        }
        if (st->hit_pending) { // (a hit: the whole body pushed a little too, the balance shaken by how hard)
            float j = len3(st->hit_impulse);
            st->support *= fmaxf(0.0f, 1.0f - j / 900.0f);
            st->stun_t = fmaxf(st->stun_t, 0.3f * fminf(j / 900.0f, 1.5f)); // (Q10: stunned a moment by a hard one)
            for (uint32_t i = 0; i < pa->nbodies; i++)
                if (st->b[i].sim) st->b[i].v = add3(st->b[i].v, mul3(st->hit_impulse, 0.35f));
            int hb = st->hit_bone[0] ? bone_of(m, st->hit_bone) : -1;
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                bool under = hb < 0;
                for (int u = st->b[i].bone; u >= 0 && !under; u = m->bones[u].parent) under = u == hb;
                if (under) st->hitk[i] = fminf(st->hitk[i], st->hit_weaken), st->hit_rate[i] = 1.0f / st->hit_recover;
            }
            st->hit_pending = false;
        }
        if (st->wound_pending) { // (Q27.5) the wound: its body (the bone's, or its nearest ancestor's), its region's reaction
            st->wound_pending = false;
            int wb = -1;
            for (int u = bone_of(m, st->wound_bone); u >= 0 && wb < 0; u = m->bones[u].parent)
                for (uint32_t i = 0; i < pa->nbodies && wb < 0; i++)
                    if (st->b[i].bone == u) wb = (int)i;
            if (wb >= 0) {
                float sev = st->wound_req;
                const BodyState* bw = &st->b[wb];
                M3 ri = q_m3(bw->q);
                if (st->kind[wb] == 0) { // a leg: it limps (its bodies from the hit one down weaker)
                    st->limp_body = wb, st->limp_sev = fmaxf(st->limp_sev, sev), st->limp_t = 8.0f + 12.0f * sev, st->limp_mask = 0, st->limp_side = 2;
                    for (uint32_t c = 0; c < pa->nconstraints && c < PHYS_MAX_CONSTRAINTS; c++)
                        if ((int)pa->constraints[c].child == wb) st->limp_side = st->arm_side[c];
                    for (uint32_t i = 0; i < pa->nbodies; i++)
                        for (int u = st->b[i].bone; u >= 0; u = m->bones[u].parent)
                            if (u == bw->bone) {
                                st->limp_mask |= 1ull << i;
                                break;
                            }
                } else { // the torso, an arm, the head: a hand holds it
                    st->wound_body = wb, st->wound_local = m3tv(&ri, sub3(st->wound_world, bw->x));
                    st->wound_sev = sev, st->wound_t = 4.0f + 8.0f * sev;
                }
                if (sev > 0.7f) st->support *= 0.15f, st->stun_t = fmaxf(st->stun_t, 1.0f), st->writhe_t = 3.0f + 5.0f * sev; // (the knees give)
            }
        }
        st->wound_t = fmaxf(st->wound_t - fdt, 0.0f), st->limp_t = fmaxf(st->limp_t - fdt, 0.0f);
        if (st->bal == BAL_FALLEN) st->writhe_t = fmaxf(st->writhe_t - fdt, 0.0f);
        st->act_time += fdt;
        { // (Q27.5) a high fall: falling fast (5 m/s down) for a quarter second, the arms flail until it lands
            const BodyState* r = &st->b[st->root];
            st->fall_t = r->v.z < -500.0f ? st->fall_t + fdt : 0.0f;
            if (st->fall_t > 0.25f) st->highfall = true;
            if (st->highfall && r->v.z > -150.0f) st->highfall = false;
        }
        float da = st->act_target - st->act, sa = st->act_rate * fdt;
        st->act += fabsf(da) <= sa ? da : (da > 0 ? sa : -sa);
        const float* prof = g_profile_k[st->profile >= 0 && st->profile < MUSCLE_PROFILE_COUNT ? st->profile : 0];
        for (uint32_t i = 0; i < pa->nbodies; i++) {
            float d = st->bk_target[i] - st->bk[i], sb = st->bk_rate[i] * fdt;
            st->bk[i] += fabsf(d) <= sb ? d : (d > 0 ? sb : -sb);
            st->hitk[i] = fminf(st->hitk[i] + st->hit_rate[i] * fdt * (1.0f - st->hitk[i] + 0.05f), 1.0f); // (back, faster at first)
            kb[i] = st->act * st->bk[i] * st->hitk[i] * pa->bodies[i].muscle * prof[st->kind[i]];
            if (st->limp_t > 0.0f && ((st->limp_mask >> i) & 1ull)) kb[i] *= 1.0f - 0.55f * st->limp_sev; // (Q27.5: the hit leg weaker)
            if (st->bal == BAL_FALLEN && st->writhe_t > 0.0f && st->bal_t > 0.5f) kb[i] *= 0.3f; // (Q27.5: writhing, not limp)
            else if (st->bal == BAL_FALLEN) kb[i] *= st->bal_t < 0.5f ? 0.6f : st->bal_t < 1.2f ? 0.15f : 0.03f; // (going down: the limbs keep their intent; lying: they let go)
            if (st->bal == BAL_GETTING_UP && st->getup_phase >= 1) kb[i] *= 0.03f; // (Q10: a get-up montage coming in: still lying loose - also while the body blends into it (2026-11-19: the muscles back at full pulled a hand to the clip at 12 m/s))
        }
        for (uint32_t c = 0; c < pa->nconstraints && c < PHYS_MAX_CONSTRAINTS; c++) {
            const PhysConstraint* pc = &pa->constraints[c];
            if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies || st->b[pc->parent].bone < 0 || st->b[pc->child].bone < 0) continue;
            V3 xp, xc;
            Q4 qp, qc;
            body_from_bone(&pa->bodies[pc->parent], anim[st->b[pc->parent].bone], &xp, &qp);
            body_from_bone(&pa->bodies[pc->child], anim[st->b[pc->child].bone], &xc, &qc);
            trel[c] = qmul((Q4){ -qp.x, -qp.y, -qp.z, qp.w }, qc);
        }
        body_from_bone(&pa->bodies[st->root], anim[st->b[st->root].bone], &root_tx, &root_tq);
        // (standing, it follows the animation: never asleep; 2026-11-19: handing over to a get-up montage the body lies as it
        // came to rest - simulated with its muscles let go, a joint now and then snapped 5 cm in a frame mid-blend)
        if (st->bal != BAL_FALLEN && !(st->bal == BAL_GETTING_UP && st->getup_phase >= 1)) st->asleep = false;
    }
    // (Q10) falling, the arms reach out to break the fall (the hands ahead of the head and the chest, the way it goes
    // down), their muscles stronger for it; lying still they let go as before
    // (on two feet only: a quadruped's "arms" are its front legs, its feet)
    int nfeet = 0;
    for (uint64_t f = st->feet; f; f &= f - 1) nfeet++;
    // (2026-11-19) until the chest lands: bracing on after it, the arms pushed the chest back up and rolled a body that
    // fell face down onto its back about one fall in three
    if (st->bal != BAL_FALLEN) st->landed = false;
    else if (!st->landed && st->root >= 0) {
        int cb = chest_body(pa, m, st);
        float floor_z = root_tx.z - st->root_dz; // (the animation's feet: the ground it stood on)
        if (cb >= 0 && st->b[cb].x.z - floor_z < pa->bodies[cb].radius + 18.0f && st->b[cb].v.z <= 0.0f) st->landed = true;
    }
    // (Q27.5) limping: the pelvis lower and leaning to the hit leg (the animation's root target itself: standing, the root
    // is kinematic on it); the hurt knee bends to reach the ground (the muscles below)
    if (active && st->bal == BAL_BALANCED && st->limp_t > 0.0f && st->limp_body >= 0 && st->root >= 0) {
        V3 lat = sub3(st->b[st->limp_body].x, st->b[st->root].x);
        lat.z = 0.0f;
        root_tx.z -= 6.0f * st->limp_sev;
        if (len3(lat) > 1e-3f) {
            M3 lean = rot_axis(norm3(cross3(v3(0, 0, 1), norm3(lat))), 0.105f * st->limp_sev), rq = q_m3(root_tq), lq = m3mul(&lean, &rq);
            root_tq = m3_q(&lq);
        }
    }
    st->protect = active && nfeet <= 2 && !st->landed && (st->bal == BAL_STUMBLING || (st->bal == BAL_FALLEN && st->bal_t < 0.9f));
    // (Q27.5) the arm that holds the wound: the other side's (the torso's or the head's wound: the right hand; an arm's:
    // the other arm), stronger; flailing in a high fall: both arms
    int hold_side = -1;
    V3 wound_w = v3(0, 0, 0);
    if (active && st->wound_t > 0.0f && st->wound_body >= 0 && !st->protect && !st->highfall) {
        const BodyState* bw = &st->b[st->wound_body];
        M3 rw = q_m3(bw->q);
        wound_w = add3(bw->x, m3v(&rw, st->wound_local));
        const char* wn = pa->bodies[st->wound_body].bone;
        size_t L = strlen(wn);
        bool wl = lower_has(wn, "left") || (L >= 2 && (wn[L - 2] == '_' || wn[L - 2] == '.') && (wn[L - 1] == 'l' || wn[L - 1] == 'L'));
        hold_side = st->kind[st->wound_body] == 2 ? (wl ? 1 : 0) : 1;
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->kind[i] == 2) kb[i] = fmaxf(kb[i], 0.7f) * 1.4f;
    }
    if (active && st->highfall)
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->kind[i] == 2) kb[i] = fmaxf(kb[i], 0.8f) * 1.6f;
    // (Q27.5) limping: the hurt leg's knee bends (its thigh forward, its calf back: the foot stays, the pelvis drops on
    // that side); ahead from the feet's bodies (their foot-to-toe axis, level)
    V3 limp_fwd = v3(0, 0, 0);
    if (active && st->limp_t > 0.0f && st->limp_side < 2 && st->bal == BAL_BALANCED) {
        for (uint32_t i = 0; i < pa->nbodies; i++) {
            if (!((st->feet >> i) & 1ull)) continue;
            M3 rf = q_m3(st->b[i].q);
            V3 ax = m3v(&rf, v3(pa->bodies[i].axis[0], pa->bodies[i].axis[1], pa->bodies[i].axis[2]));
            ax.z = 0.0f;
            limp_fwd = add3(limp_fwd, ax);
        }
        limp_fwd = len3(limp_fwd) > 0.2f ? norm3(limp_fwd) : v3(0, 0, 0);
    }
    V3 brace = v3(0, 0, -1);
    if (st->protect) {
        const BodyState* r = &st->b[st->root];
        V3 lean = v3(0, 0, 0);
        for (uint32_t i = 0; i < pa->nbodies; i++) // (the upper body's lean from the root: where it topples)
            if (st->b[i].bone >= 0 && st->kind[i] != 0 && st->b[i].x.z > r->x.z) lean = add3(lean, sub3(st->b[i].x, r->x));
        V3 hv = v3(r->v.x + lean.x * 2.0f, r->v.y + lean.y * 2.0f, 0.0f);
        brace = len3(hv) > 1.0f ? norm3(add3(mul3(norm3(hv), 0.75f), v3(0, 0, -0.66f))) : v3(0, 0, -1);
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->kind[i] == 2) kb[i] = fmaxf(kb[i], 0.9f) * 1.8f;
    }
    // (Q10) the bodies' own blend weights (Set All Bodies Below Physics Blend Weight), any mode
    {
        float fdt = fminf(dt, 1.0f / 30.0f);
        if (st->bw_pending) {
            int rb = st->bw_bone[0] ? bone_of(m, st->bw_bone) : -1;
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                bool under = rb < 0;
                for (int u = st->b[i].bone; u >= 0 && !under; u = m->bones[u].parent) under = u == rb;
                if (under) st->bw_target[i] = st->bw_req, st->bw_rate[i] = st->bw_req_rate;
            }
            st->bw_pending = false;
        }
        for (uint32_t i = 0; i < pa->nbodies; i++) {
            float d = st->bw_target[i] - st->bw[i], sb = st->bw_rate[i] * fdt;
            st->bw[i] += fabsf(d) <= sb ? d : (d > 0 ? sb : -sb);
        }
    }
    // (Q10) a plain ragdoll's muscle tone: it falls as a body, not a rag (its spine, knees and neck keep some of the pose
    // it had), then lets go as it lies (an unconscious body on the ground is limp: no tremor against the floor)
    st->rag_t += fminf(dt, 1.0f / 30.0f);
    const float tone_k = pa->tone * expf(-st->rag_t / 1.2f);
    const bool tone_on = st->mode == PHYS_RAGDOLL && tone_k > 0.01f;
    // the impulses (Add Impulse to Bone): a velocity change
    if (len3(st->impulse_all) > 0.0f || len3(st->impulse) > 0.0f) {
        int ib = bone_of(m, st->impulse_bone);
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->b[i].sim) {
                st->b[i].v = add3(st->b[i].v, st->impulse_all);
                if (st->b[i].bone == ib) st->b[i].v = add3(st->b[i].v, st->impulse);
            }
        st->impulse_all = st->impulse = v3(0, 0, 0), st->impulse_bone[0] = 0, st->asleep = false;
    }
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
    // the boxes around it (once a frame), the ground under its first body
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < pa->nbodies; i++) {
        if (st->b[i].bone < 0) continue;
        float r = pa->bodies[i].radius + pa->bodies[i].length + 60.0f;
        V3 x = st->b[i].x;
        mn[0] = fminf(mn[0], x.x - r), mn[1] = fminf(mn[1], x.y - r), mn[2] = fminf(mn[2], x.z - r);
        mx[0] = fmaxf(mx[0], x.x + r), mx[1] = fmaxf(mx[1], x.y + r), mx[2] = fmaxf(mx[2], x.z + r);
    }
    static _Thread_local float boxes[128][6];
    uint32_t nb = bp_world_boxes(mn, mx, boxes, 128);
    float ground = nb ? -1e30f : now_ground(w, sk, st->b[0].x);
    float gz;
    if (landscape_height(st->b[0].x.x, st->b[0].x.y, &gz)) ground = fmaxf(ground, gz);
    // the bodies' pairs that may touch this frame (their bounds within what they can travel): the substeps test only those
    uint16_t pairs[PHYS_MAX_BODIES * 8][2];
    uint32_t npairs = 0;
    {
        float reach = 0.0f;
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->b[i].sim) reach = fmaxf(reach, len3(st->b[i].v) * dt);
        for (uint32_t i = 0; i < pa->nbodies; i++)
            for (uint32_t j = i + 1; j < pa->nbodies && npairs < PHYS_MAX_BODIES * 8; j++) {
                const BodyState *bi = &st->b[i], *bj = &st->b[j];
                if (bi->bone < 0 || bj->bone < 0 || (!bi->sim && !bj->sim) || ((st->no_col[i] >> j) & 1ull)) continue;
                float ri = pa->bodies[i].radius + pa->bodies[i].length * 0.5f, rj = pa->bodies[j].radius + pa->bodies[j].length * 0.5f;
                float lim = ri + rj + 2.0f * reach + 2.0f;
                V3 d = sub3(bi->x, bj->x);
                if (dot3(d, d) < lim * lim) pairs[npairs][0] = (uint16_t)i, pairs[npairs][1] = (uint16_t)j, npairs++;
            }
    }
    uint32_t contacts = 0;
    if (!st->asleep) {
        // (2026-11-16) an active ragdoll standing on its full balance (its root held, its limbs near their pose): half the
        // substeps, stable and half the cost; a hit, a stumble, a fall: all of them
        // (2026-11-19) fixed substeps: as many as the frame holds, the rest kept for the next one. dt / 8 made the bodies
        // a different simulation at each frame rate, and a busy PC's uneven frames (5 to 40 ms) pumped energy in: 12
        // ragdolls at rest flailed at 20 m/s (ikphys_ragdoll at 54-108 fps); now 60 or 300 fps step the same
        float h = active && st->bal == BAL_BALANCED && st->support > 0.98f ? SUB_H * 2.0f : SUB_H, g = -980.0f; // (cm/s^2)
        st->sub_acc += dt;
        int nsub = (int)(st->sub_acc / h);
        if (nsub > MAX_SUBS) nsub = MAX_SUBS, st->sub_acc = 0.0f;
        else st->sub_acc -= (float)nsub * h;
        for (int sub = 0; sub < nsub; sub++) {
            // predict (the kinematic bodies take the animation's place)
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                BodyState* b = &st->b[i];
                if (b->bone < 0) continue;
                b->px = b->x, b->pq = b->q;
                if (!b->sim) {
                    body_from_bone(&pa->bodies[i], anim[b->bone], &b->x, &b->q);
                    continue;
                }
                // (2026-11-16) the active ragdoll's root while it stands on its full balance: where the animation has it,
                // kinematic (the limbs hang from it with their muscles; a hit or a stumble lets it go)
                if (active && (int)i == st->root) {
                    bool held = st->bal == BAL_BALANCED && st->support > 0.5f; // (a light push: the body sways on it; a hard one lets it go)
                    b->inv_mass = held ? 0.0f : b->inv_mass0;
                    if (held) {
                        b->x = lerp3(b->x, root_tx, (float)(sub + 1) / (float)nsub), b->q = qslerp(b->q, root_tq, (float)(sub + 1) / (float)nsub);
                        continue;
                    }
                }
                // (2026-11-16) the active ragdoll's muscles hold their body's weight too (gravity compensation, as the
                // Euphoria-like controllers do), by their strength: standing it keeps its posture; a weakened limb sags, a
                // fallen body feels its whole weight
                float hold = st->bal == BAL_BALANCED ? 1.0f : st->bal == BAL_STUMBLING ? st->support : 0.0f; // (losing its balance: its weight back)
                float gk = active ? 1.0f - fminf(fmaxf(kb[i], 0.0f), 1.0f) * hold : 1.0f;
                b->v.z += g * gk * h;
                b->x = add3(b->x, mul3(b->v, h));
                b->q = q_add_rot(b->q, b->w, h);
                make_iw(b);
            }
            // the joints: the child's head on the parent's anchor, then its swing within the cone, its twist
            for (int pass = 0; pass < 1; pass++) // (one pass: XPBD converges by its substeps, not by iterations)
                for (uint32_t c = 0; c < pa->nconstraints; c++) {
                    const PhysConstraint* pc = &pa->constraints[c];
                    if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies) continue;
                    BodyState *bp = &st->b[pc->parent], *bc = &st->b[pc->child];
                    if (bp->bone < 0 || bc->bone < 0 || (!bp->sim && !bc->sim)) continue;
                    const PhysBody* pbc = &pa->bodies[pc->child];
                    M3 rp = q_m3(bp->q), rc = q_m3(bc->q);
                    V3 ra = m3v(&rp, bc->anchor_p);
                    V3 rb = m3v(&rc, mul3(v3(pbc->offset[0], pbc->offset[1], pbc->offset[2]), -1.0f)); // (the child's head)
                    V3 d = sub3(add3(bc->x, rb), add3(bp->x, ra));
                    float dl = len3(d);
                    if (dl > 1e-5f) apply_pos(bp, bc, ra, rb, mul3(d, 1.0f / dl), dl);
                    // (Q10) swing: the child's axis within its anatomical ellipse (bend plane x across), from the bind
                    V3 from, to;
                    if (joint_swing(pc, pbc, bp, bc, &from, &to) > 0.0f) {
                        V3 ax = cross3(to, from);
                        float la = len3(ax), ang = acosf(fmaxf(fminf(dot3(from, to), 1.0f), -1.0f));
                        if (la > 1e-7f) apply_rot(bp, bc, mul3(ax, 1.0f / la), ang);
                    }
                    // twist: the child's perpendicular around its axis against the parent's frame, within [min, max]
                    V3 tax;
                    float turn;
                    if (joint_twist(pc, pbc, bp, bc, &tax, &turn) > 0.0f) apply_rot(bp, bc, tax, turn);
                }
            // (2026-11-16) the active ragdoll's muscles: each joint turns its child toward the animated turn in its
            // parent's frame, a share per substep (its strength), at most a speed (its torque: a hit wins for a moment)
            // (Q10: a plain ragdoll's tone the same way, weak, toward the pose it fell in)
            if (active || tone_on)
                for (uint32_t c = 0; c < pa->nconstraints && c < PHYS_MAX_CONSTRAINTS; c++) {
                    const PhysConstraint* pc = &pa->constraints[c];
                    if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies) continue;
                    BodyState *bp = &st->b[pc->parent], *bc = &st->b[pc->child];
                    float k = active ? kb[pc->child] : tone_k * pc->tone;
                    if (bp->bone < 0 || bc->bone < 0 || !bc->sim || k <= 1e-3f) continue;
                    Q4 want = qmul(bp->q, active ? trel[c] : st->trest[c]);
                    if (st->protect && st->arm_role[c] && st->arm_role[c] < 3) { // (Q10) bracing: the arm toward the ground ahead
                        M3 rc = q_m3(bc->q);
                        V3 axw = norm3(m3v(&rc, norm3(v3(pa->bodies[pc->child].axis[0], pa->bodies[pc->child].axis[1], pa->bodies[pc->child].axis[2]))));
                        V3 dir = st->arm_role[c] == 1 ? brace : norm3(add3(brace, v3(0, 0, 0.25f))); // (the elbow a little bent)
                        M3 turn = rot_between(axw, dir, 1.0f), wq = m3mul(&turn, &rc);
                        want = m3_q(&wq);
                    }
                    // (Q27.5) holding the wound: the other side's arm toward it (the forearm aims at it, the upper arm halfway)
                    if (hold_side >= 0 && st->arm_role[c] && st->arm_role[c] < 3 && st->arm_side[c] == (uint8_t)hold_side) {
                        M3 rc = q_m3(bc->q);
                        V3 axw = norm3(m3v(&rc, norm3(v3(pa->bodies[pc->child].axis[0], pa->bodies[pc->child].axis[1], pa->bodies[pc->child].axis[2]))));
                        V3 to = norm3(sub3(wound_w, bc->x));
                        V3 dir = st->arm_role[c] == 1 ? norm3(add3(to, v3(0, 0, -0.8f))) : to; // (the elbow down: a hand pressed on it)
                        M3 turn = rot_between(axw, dir, 1.0f), wq = m3mul(&turn, &rc);
                        want = m3_q(&wq);
                    }
                    // (Q27.5) the limping knee: the thigh's axis turned toward ahead, the calf's back (by up to 30 degrees)
                    if (len3(limp_fwd) > 0.5f && st->kind[pc->child] == 0 && !((st->feet >> pc->child) & 1ull) && st->arm_side[c] == (uint8_t)st->limp_side) {
                        M3 rw = q_m3(want);
                        V3 axw = norm3(m3v(&rw, norm3(v3(pa->bodies[pc->child].axis[0], pa->bodies[pc->child].axis[1], pa->bodies[pc->child].axis[2]))));
                        bool calf = (int)pc->parent != st->root && st->kind[pc->parent] == 0;
                        float th = tanf(0.52f * st->limp_sev) * (calf ? -1.0f : 0.5f);
                        M3 turn = rot_between(axw, norm3(add3(axw, mul3(limp_fwd, th))), 1.0f), wq = m3mul(&turn, &rw);
                        want = m3_q(&wq);
                        k = fmaxf(k, 0.6f); // (the bend held, the leg weak only in how much it carries)
                    }
                    // (Q27.5) a high fall: the upper arms windmill (a circle in the plane of up and ahead, the two out of phase)
                    if (st->highfall && st->arm_role[c] == 1) {
                        M3 rc = q_m3(bc->q);
                        V3 axw = norm3(m3v(&rc, norm3(v3(pa->bodies[pc->child].axis[0], pa->bodies[pc->child].axis[1], pa->bodies[pc->child].axis[2]))));
                        V3 side = sub3(bc->x, st->b[st->root].x);
                        side.z = 0.0f;
                        side = len3(side) > 1e-3f ? norm3(side) : v3(1, 0, 0);
                        V3 ahead = norm3(cross3(v3(0, 0, 1), side));
                        float ph = st->act_time * 9.0f + (st->arm_side[c] == 1 ? 3.14159f : 0.0f);
                        V3 dir = norm3(add3(add3(mul3(v3(0, 0, 1), cosf(ph)), mul3(ahead, sinf(ph))), mul3(side, 0.6f)));
                        M3 turn = rot_between(axw, dir, 1.0f), wq = m3mul(&turn, &rc);
                        want = m3_q(&wq);
                    }
                    // (Q27.5) writhing on the ground: the spine and the legs turn back and forth about their own axes from
                    // where they lie (not toward the standing pose)
                    if (st->bal == BAL_FALLEN && st->writhe_t > 0.0f && st->bal_t > 0.5f && (st->kind[pc->child] == 0 || st->kind[pc->child] == 1)) {
                        M3 rc = q_m3(bc->q);
                        V3 ax = norm3(m3v(&rc, v3(1, 0, 0)));
                        M3 wig = rot_axis(ax, 0.35f * sinf(st->act_time * 3.0f + (float)c * 1.3f)), wq = m3mul(&wig, &rc);
                        want = m3_q(&wq);
                    }
                    Q4 dq = qmul(want, (Q4){ -bc->q.x, -bc->q.y, -bc->q.z, bc->q.w });
                    if (dq.w < 0.0f) dq = (Q4){ -dq.x, -dq.y, -dq.z, -dq.w };
                    float sw = sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
                    if (sw < 1e-6f) continue;
                    float ang = 2.0f * atan2f(sw, dq.w), step = ang * (1.0f - expf(-k * 45.0f * h)), maxs = k * 14.0f * h;
                    // (the root, held by the balance while it stands, takes no reaction: the legs and the spine would spin
                    // a pelvis; below it the reaction goes up the chain as usual)
                    bool held = (int)pc->parent == st->root && st->support > 0.3f;
                    apply_rot(bc, held ? NULL : bp, mul3(v3(dq.x, dq.y, dq.z), 1.0f / sw), fminf(step, maxs));
                }
            // (2026-11-16) the balance: the root held up and upright toward the animation's (stumbling: only up, where it
            // stepped), the feet planted while it stands; each pull limited (a push strong enough moves it)
            if (active && st->support > 0.0f && st->b[st->root].sim) {
                BodyState* r = &st->b[st->root];
                float sp = st->support * fminf(st->act, 1.5f);
                V3 tgt = root_tx;
                if (st->bal == BAL_STUMBLING) tgt.x = r->x.x, tgt.y = r->x.y;
                Q4 rtq = root_tq;
                float kz = 1.0f - expf(-sp * 60.0f * h), kxy = 1.0f - expf(-sp * 10.0f * h), kr = 1.0f - expf(-sp * 160.0f * h); // (upright held firmly: the legs' joints twist a pelvis)
                V3 d = sub3(tgt, r->x);
                float cap = 220.0f * h * sp; // (cm a substep)
                V3 mv = v3(d.x * kxy, d.y * kxy, d.z * kz);
                float lm = len3(mv);
                if (lm > cap) mv = mul3(mv, cap / lm);
                r->x = add3(r->x, mv);
                r->q = qslerp(r->q, rtq, kr);
                if (st->bal == BAL_BALANCED)
                    for (uint32_t i = 0; i < pa->nbodies; i++) {
                        if (!((st->feet >> i) & 1ull) || !st->b[i].sim) continue;
                        V3 fx;
                        Q4 fq;
                        body_from_bone(&pa->bodies[i], anim[st->b[i].bone], &fx, &fq);
                        float kf = 1.0f - expf(-sp * 35.0f * h);
                        st->b[i].x = lerp3(st->b[i].x, fx, kf), st->b[i].q = qslerp(st->b[i].q, fq, kf);
                    }
            }
            // the collisions: the world (boxes, ground), then the bodies against each other (not neighbors)
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                BodyState* b = &st->b[i];
                if (b->bone < 0 || !b->sim) continue;
                const PhysBody* pb = &pa->bodies[i];
                uint32_t before = contacts;
                V3 a, c;
                segment(pb, b, &a, &c);
                float r = pb->shape == PHYS_BOX ? fmaxf(fmaxf(pb->extent[0], pb->extent[1]), pb->extent[2]) : pb->radius;
                contacts += (uint32_t)collide_sphere(b, a, r, (const float(*)[6])boxes, nb, ground, 0.8f);
                if (pb->shape == PHYS_CAPSULE) {
                    segment(pb, b, &a, &c);
                    contacts += (uint32_t)collide_sphere(b, c, r, (const float(*)[6])boxes, nb, ground, 0.8f);
                    segment(pb, b, &a, &c);
                    contacts += (uint32_t)collide_sphere(b, mul3(add3(a, c), 0.5f), r, (const float(*)[6])boxes, nb, ground, 0.8f);
                }
                b->touch = contacts != before;
                b->pushed = b->touch;
            }
            for (uint32_t pi = 0; pi < npairs; pi++) {
                    uint32_t i = pairs[pi][0], j = pairs[pi][1];
                    BodyState *bi = &st->b[i], *bj = &st->b[j];
                    V3 a1, b1, a2, b2, c1, c2;
                    segment(&pa->bodies[i], bi, &a1, &b1), segment(&pa->bodies[j], bj, &a2, &b2);
                    closest_seg_seg(a1, b1, a2, b2, &c1, &c2);
                    V3 d = sub3(c1, c2);
                    float dl = len3(d), rr = pa->bodies[i].radius + pa->bodies[j].radius;
                    if (dl >= rr || dl < 1e-5f) continue;
                    V3 n = mul3(d, 1.0f / dl);
                    apply_pos(bi, bj, sub3(c1, bi->x), sub3(c2, bj->x), n, fminf(rr - dl, 1.0f)); // (apart over some substeps)
                    bi->pushed = bj->pushed = true; // (Q10: their joints' limits again after the contacts)
                }
            // (Q10) the limits again after the contacts, twice (Gauss-Seidel: a hub's joints pull on each other): a limb
            // pushed by the ground or a body stays in its range
            // (an Active Ragdoll standing on its balance: only the joints a contact moved - its muscles keep the rest in range)
            const bool limits_all = !active || st->bal != BAL_BALANCED || st->support < 0.98f;
            for (uint32_t pass = 0; pass < LIMIT_PASSES; pass++) { // (until a pass fixes under a third of a degree: the contacts, the muscles and the tone moved them)
                float fixed = 0.0f;
                for (uint32_t c = 0; c < pa->nconstraints; c++) {
                    const PhysConstraint* pc = &pa->constraints[c];
                    if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies) continue;
                    BodyState *bp = &st->b[pc->parent], *bc = &st->b[pc->child];
                    if (bp->bone < 0 || bc->bone < 0 || (!bp->sim && !bc->sim)) continue;
                    if (!limits_all && !bp->pushed && !bc->pushed) continue;
                    const PhysBody* pbc = &pa->bodies[pc->child];
                    V3 from, to, tax;
                    float turn, o = joint_swing(pc, pbc, bp, bc, &from, &to);
                    if (o > 0.0f) {
                        V3 ax = cross3(to, from);
                        float la = len3(ax), ang = acosf(fmaxf(fminf(dot3(from, to), 1.0f), -1.0f));
                        if (la > 1e-7f) apply_rot(bp, bc, mul3(ax, 1.0f / la), ang);
                    }
                    float ot = joint_twist(pc, pbc, bp, bc, &tax, &turn);
                    if (ot > 0.0f) apply_rot(bp, bc, tax, turn);
                    fixed = fmaxf(fixed, fmaxf(o, ot));
                }
                if (fixed < 0.33f) break;
            }
            // physical animation: the simulated bodies pulled toward the animated pose by their strength
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                BodyState* b = &st->b[i];
                float s = pa->bodies[i].strength;
                if (b->bone < 0 || !b->sim || s <= 0.0f) continue;
                V3 tx;
                Q4 tq;
                body_from_bone(&pa->bodies[i], anim[b->bone], &tx, &tq);
                float k = 1.0f - powf(1.0f - fminf(s, 0.999f), h * 60.0f);
                b->x = lerp3(b->x, tx, k), b->q = qslerp(b->q, tq, k);
            }
            // the velocities from the corrections (damped a little: air)
            for (uint32_t i = 0; i < pa->nbodies; i++) {
                BodyState* b = &st->b[i];
                if (b->bone < 0 || !b->sim) continue;
                b->v = mul3(sub3(b->x, b->px), 1.0f / h);
                Q4 dq = qmul(b->q, (Q4){ -b->pq.x, -b->pq.y, -b->pq.z, b->pq.w });
                b->w = mul3(v3(dq.x, dq.y, dq.z), (dq.w >= 0.0f ? 2.0f : -2.0f) / h);
                // (damped a little in the air; on the ground much more: rolling and sliding friction, so a body lying
                // down comes to rest instead of trembling)
                b->v = mul3(b->v, expf((b->touch ? -4.0f : -0.3f) * h)), b->w = mul3(b->w, expf((b->touch ? -8.0f : -1.5f) * h));
                // (a guard, like Unreal's max velocities: nothing a ragdoll does goes past 40 m/s or 8 turns a second)
                float lv = len3(b->v), lw = len3(b->w);
                if (lv > 4000.0f) b->v = mul3(b->v, 4000.0f / lv);
                if (lw > 50.0f) b->w = mul3(b->w, 50.0f / lw);
            }
            // the joints' damping (their angular drives' in Unreal): the relative turn of the two bodies slowed, so the
            // limbs swing and settle instead of rattling at their limits (Q10: each substep - once a frame it was an
            // eighth of itself: the next substeps' speeds come from the positions; a tail swung for seconds)
            for (uint32_t c = 0; c < pa->nconstraints; c++) {
                if (pa->constraints[c].parent >= pa->nbodies || pa->constraints[c].child >= pa->nbodies) continue;
                BodyState *bp = &st->b[pa->constraints[c].parent], *bc = &st->b[pa->constraints[c].child];
                if (!bc->sim || bc->bone < 0) continue;
                V3 dw = sub3(bc->w, bp->sim ? bp->w : v3(0, 0, 0));
                float k = 1.0f - expf(-(pa->constraints[c].damping > 0.0f ? pa->constraints[c].damping : 6.0f) * h); // (Q10: its own)
                if (bp->sim) bc->w = sub3(bc->w, mul3(dw, k * 0.5f)), bp->w = add3(bp->w, mul3(dw, k * 0.5f));
                else bc->w = sub3(bc->w, mul3(dw, k));
            }
        }
        // asleep after a second without moving (nothing runs then; an impulse or a mode wakes it)
        float vmax = 0.0f;
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->b[i].sim) vmax = fmaxf(vmax, len3(st->b[i].v));
        st->still_t = vmax < 6.0f ? st->still_t + dt : 0.0f;
        if (st->still_t > 1.0f && (!active || st->bal == BAL_FALLEN)) st->asleep = true;
    }
    // (Q10) the measures: the most a joint is past its limits, the deepest overlap of two bodies that collide
    if (!st->asleep) {
        st->limit_over = 0.0f, st->limit_worst = -1, st->self_pen = 0.0f;
        if (K.measure)
        for (uint32_t c = 0; c < pa->nconstraints; c++) {
            const PhysConstraint* pc = &pa->constraints[c];
            if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies) continue;
            const BodyState *bp = &st->b[pc->parent], *bc = &st->b[pc->child];
            if (bp->bone < 0 || bc->bone < 0 || !bc->sim) continue;
            V3 f, t, ax;
            float turn, o = fmaxf(joint_swing(pc, &pa->bodies[pc->child], bp, bc, &f, &t), joint_twist(pc, &pa->bodies[pc->child], bp, bc, &ax, &turn));
            if (o > st->limit_over) st->limit_over = o, st->limit_worst = (int)c;
        }
        for (uint32_t pi = 0; K.measure && pi < npairs; pi++) {
            uint32_t i = pairs[pi][0], j = pairs[pi][1];
            V3 a1, b1, a2, b2, c1, c2;
            segment(&pa->bodies[i], &st->b[i], &a1, &b1), segment(&pa->bodies[j], &st->b[j], &a2, &b2);
            closest_seg_seg(a1, b1, a2, b2, &c1, &c2);
            st->self_pen = fmaxf(st->self_pen, pa->bodies[i].radius + pa->bodies[j].radius - len3(sub3(c1, c2)));
        }
    }
    // (2026-11-16) the active ragdoll's balance: standing -> stumbling (pushed off its place or tilted: a step, its hold
    // fading) -> back standing (where it stepped) or fallen -> lying still -> getting up (blended back into the animation
    // where it lies) -> standing
    if (active) {
        BodyState* r = &st->b[st->root];
        float dxy = sqrtf((r->x.x - root_tx.x) * (r->x.x - root_tx.x) + (r->x.y - root_tx.y) * (r->x.y - root_tx.y));
        Q4 dq = qmul(root_tq, (Q4){ -r->q.x, -r->q.y, -r->q.z, r->q.w });
        float tilt = 2.0f * acosf(fminf(fabsf(dq.w), 1.0f)) * 57.29578f;
        float above = r->x.z - (root_tx.z - st->root_dz); // (the root over where the animation's feet are)
        float vmax = 0.0f;
        int vfast = -1;
        for (uint32_t i = 0; i < pa->nbodies; i++)
            if (st->b[i].sim && len3(st->b[i].v) > vmax) vmax = len3(st->b[i].v), vfast = (int)i;
        st->bal_t += dt;
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("REALENGINE_AR_DEBUG") != NULL;
        int bal0 = st->bal;
        switch (st->bal) {
        case BAL_BALANCED:
            st->stun_t -= dt;
            if (st->stun_t <= 0.0f) st->support += (1.0f - st->support) * fminf(dt * 4.0f, 1.0f);
            if (dxy > 28.0f || tilt > 35.0f) st->bal = BAL_STUMBLING, st->bal_t = 0.0f;
            break;
        case BAL_STUMBLING:
            st->support = fmaxf(st->support - dt / 0.7f, 0.0f);
            if (st->bal_t > 0.25f && tilt < 12.0f && vmax < 90.0f && above > st->root_dz * 0.8f) { // (it caught itself: stands where it is)
                st->bal = BAL_BALANCED, st->bal_t = 0.0f, st->support = 1.0f;
                st->move_d = v3(r->x.x - root_tx.x, r->x.y - root_tx.y, 0.0f), st->move_pending = true;
            } else if (above < st->root_dz * 0.55f || st->bal_t > 1.0f)
                st->bal = BAL_FALLEN, st->bal_t = st->lie_t = 0.0f;
            break;
        case BAL_FALLEN:
            st->support = 0.0f;
            st->lie_v += (len3(r->v) - st->lie_v) * fminf(dt * 3.0f, 1.0f); // (its root's speed, smoothed: the limbs may still settle)
            st->lie_t = st->lie_v < 45.0f && st->bal_t > 0.5f ? st->lie_t + dt : 0.0f;
            if (st->bal_t > st->getup_delay + 4.0f) st->lie_t = st->getup_delay + 1.0f; // (never stuck down: up after a while)
            if (st->writhe_t > 0.0f) st->lie_t = 0.0f; // (Q27.5: writhing first)
            if (dbg && fmodf(st->bal_t, 0.5f) < dt) printf("[RealEngine] active ragdoll: lying %.1f s, root speed %.0f (z %.1f), lie %.2f, fastest %s %.0f\n", st->bal_t, len3(r->v), r->x.z, st->lie_t,
                                                            vfast >= 0 ? pa->bodies[vfast].bone : "-", vmax);
            st->face_down = body_face_down(pa, m, st);
            if ((st->auto_getup && st->lie_t > st->getup_delay) || st->getup_now) {
                st->bal = BAL_GETTING_UP, st->bal_t = 0.0f, st->getup_now = false, st->asleep = false;
                // (Q10) a get-up montage for how it lies (face down / on its back): asked of the main thread; the next pose
                // has it, its root is matched to the body's (place and heading), then the body blends into it
                const char* mont = st->face_down == 1 ? pa->getup_front : pa->getup_back;
                if (mont[0]) st->montage_req = st->getup_mo = mont, st->getup_phase = 1, st->getup_done = false, st->asleep = true;
                else st->getup_phase = 0, st->move_d = v3(r->x.x - root_tx.x, r->x.y - root_tx.y, 0.0f), st->move_pending = true; // (the actor where it lies)
            }
            break;
        case BAL_GETTING_UP:
            if (st->getup_phase == 1) { // (the montage's first pose is in anim now: the actor turned and moved under the body)
                if (st->bal_t < 0.3f) break; // (the montage starts on the main thread and blends in: its first pose after that)
                align_getup(pa, m, st, anim, r);
                st->getup_phase = 2, st->bal_t = 0.0f;
                break;
            }
            { // the body into the animation: eased (2026-11-19: linear, its first frames were the fastest)
                float u = fminf(st->bal_t / (st->getup_phase == 2 ? 0.5f : 0.8f), 1.0f);
                st->blend = 1.0f - u * u * (3.0f - 2.0f * u);
            }
            // (2026-11-19) blended in: the get-up montage plays as authored, the body its pose (the physics following a clip
            // pushed the feet under the floor and bent it over) until it ends (the fast path at the top)
            if (st->blend <= 0.0f && st->getup_phase == 2) st->getup_phase = 3, st->blend = 0.0f;
            else if (st->blend <= 0.0f) // (the blend back to 1 once the bodies are made again from the animation: the next frame's start)
                st->bal = BAL_BALANCED, st->started = false, st->blend = 0.0f, st->blend_back = true, st->support = 1.0f, st->bal_t = 0.0f, st->getup_phase = 0, st->face_down = -1;
            break;
        }
        if (dbg && st->bal != bal0)
            printf("[RealEngine] active ragdoll: %d -> %d (moved %.1f cm, tilt %.1f deg, root %.1f cm over the feet of %.1f, fastest %.0f cm/s)\n", bal0, st->bal, dxy, tilt,
                   above, st->root_dz, vmax);
    }
    // the pose: each body's bone at its frame, the others following their parents as animated; blended
    bool moved[ANIM_MAX_BONES];
    memset(moved, 0, sizeof(bool) * m->nbones);
    for (uint32_t i = 0; i < pa->nbodies; i++) {
        const BodyState* b = &st->b[i];
        if (b->bone < 0 || !b->sim) continue;
        float s[3];
        row_rot(anim[b->bone], s);
        M3 r = q_m3(b->q);
        V3 head = sub3(b->x, m3v(&r, v3(pa->bodies[i].offset[0], pa->bodies[i].offset[1], pa->bodies[i].offset[2])));
        float bl = st->blend * st->bw[i]; // (Q10: the body's own weight too)
        if (bl < 1.0f) {
            M3 ra = row_rot(anim[b->bone], NULL);
            Q4 qa = m3_q(&ra);
            r = q_m3(qslerp(qa, b->q, bl));
            head = lerp3(row_pos(anim[b->bone]), head, bl);
        }
        row_set(bones[b->bone], &r, s, head);
        moved[b->bone] = true;
    }
    propagate(m, anim, bones, moved);
    // (the stats: summed by the pose jobs' last writer; a frame's counts, not exact under threads)
    K.acc.ragdolls++, K.acc.bodies += pa->nbodies, K.acc.contacts += contacts, K.acc.ms += (re_time_seconds() - t0) * 1000.0;
}

// --- (Q10) The dynamic bones (AnimDynamics): a particle at each chain bone's head (and one past each end), Verlet in
// the world, pulled toward the animated shape from its simulated parent, its length kept, its angle limited, pushed
// out of the Physics Asset's capsules; then each bone turned toward its particle (the lengths kept by the skeleton) ---
static void body_capsule(const PhysBody* pb, const float bone[3][4], V3* a, V3* b) {
    BodyState bs;
    body_from_bone(pb, bone, &bs.x, &bs.q);
    segment(pb, &bs, a, b);
}

static V3 dyn_point(const struct DynNode* n, const float (*bones)[3][4]); // (below)
// The nodes of every chain, from the skeleton (once per asset version): bones in order, parents first
static void dyn_build(PState* st, const PhysicsAsset* pa, const SkelMesh* m, float (*bones)[3][4]) {
    st->ndyn = 0;
    const uint32_t cap = (uint32_t)(sizeof st->dyn / sizeof st->dyn[0]);
    for (uint32_t ci = 0; ci < pa->nchains; ci++) {
        const DynChain* c = &pa->chains[ci];
        int root = c->on ? bone_of(m, c->bone) : -1;
        if (root < 0) continue;
        int endb = c->end[0] ? bone_of(m, c->end) : -1;
        bool in[ANIM_MAX_BONES] = { 0 }, has_child[ANIM_MAX_BONES] = { 0 };
        int node_of[ANIM_MAX_BONES];
        in[root] = true;
        for (uint32_t b = (uint32_t)root + 1; b < m->nbones; b++) {
            int p = m->bones[b].parent;
            if (p >= 0 && in[p] && p != endb) in[b] = true, has_child[p] = true;
        }
        for (uint32_t b = 0; b < m->nbones; b++) node_of[b] = -1;
        for (uint32_t b = (uint32_t)root + 1; b < m->nbones && st->ndyn < cap; b++) {
            if (!in[b]) continue;
            int p = m->bones[b].parent;
            struct DynNode* n = &st->dyn[st->ndyn];
            memset(n, 0, sizeof *n);
            n->bone = (int16_t)b, n->parent_node = (int16_t)(p == root ? -1 : node_of[p]), n->chain = (int16_t)ci, n->tip = 0;
            n->len = fmaxf(len3(sub3(row_pos(m->bind_world[b]), row_pos(m->bind_world[p]))), 0.1f);
            node_of[b] = (int)st->ndyn++;
        }
        for (uint32_t b = (uint32_t)root; b < m->nbones && st->ndyn < cap; b++) { // a tip past each end: the last bone swings too
            if (!in[b] || has_child[b]) continue;
            int p = m->bones[b].parent;
            V3 dir = p >= 0 ? sub3(row_pos(m->bind_world[b]), row_pos(m->bind_world[p])) : v3(0, 0, -1);
            float L = fmaxf(fminf(len3(dir), 12.0f), 3.0f);
            M3 rb = row_rot(m->bind_world[b], NULL);
            struct DynNode* n = &st->dyn[st->ndyn];
            memset(n, 0, sizeof *n);
            n->bone = (int16_t)b, n->parent_node = (int16_t)node_of[b], n->chain = (int16_t)ci, n->tip = 1, n->len = L;
            n->tip_local = mul3(norm3(m3tv(&rb, len3(dir) > 1e-3f ? dir : v3(0, 0, -1))), L);
            st->ndyn++;
        }
    }
    // the bodies a chain is inside at the bind (an earring in a big chest sphere): left out for it
    memset(st->dyn_bind_skip, 0, sizeof st->dyn_bind_skip);
    for (uint32_t i = 0; i < st->ndyn; i++) {
        const struct DynNode* n = &st->dyn[i];
        V3 p = dyn_point(n, (const float(*)[3][4])m->bind_world);
        for (uint32_t k = 0; k < pa->nbodies && k < 64; k++) {
            int b = bone_of(m, pa->bodies[k].bone);
            if (b < 0) continue;
            V3 a, c;
            body_capsule(&pa->bodies[k], m->bind_world[b], &a, &c);
            float r = pa->bodies[k].shape == PHYS_BOX ? fmaxf(fmaxf(pa->bodies[k].extent[0], pa->bodies[k].extent[1]), pa->bodies[k].extent[2]) : pa->bodies[k].radius;
            if (len3(sub3(p, closest_on_seg(a, c, p))) < r + pa->chains[n->chain].radius) st->dyn_bind_skip[n->chain] |= 1ull << k;
        }
    }
    (void)bones;
}

// A node's target (where the animation has its point) and the point it hangs from (animated), from the rows
static V3 dyn_point(const struct DynNode* n, const float (*bones)[3][4]) {
    if (!n->tip) return row_pos(bones[n->bone]);
    M3 r = row_rot(bones[n->bone], NULL);
    return add3(row_pos(bones[n->bone]), m3v(&r, n->tip_local));
}
static int dyn_seg_bone(const struct DynNode* n, const SkelMesh* m) { return n->tip ? n->bone : m->bones[n->bone].parent; }

void dyn_apply(World* w, Entity sk, const PhysicsAsset* pa, const SkelMesh* m, float bones[][3][4], float dt) {
    (void)w;
    PState* st = state_of(sk, false);
    if (!st || !pa || !pa->nchains || !m || st->dyn_off) return;
    double t0 = re_time_seconds();
    if (dt > 1.0f / 20.0f) dt = 1.0f / 20.0f;
    V3 root = row_pos(bones[0]);
    if (!st->dyn_started || st->dyn_version != pa->version || len3(sub3(root, st->dyn_root_prev)) > 250.0f) { // (new, edited, or a teleport)
        dyn_build(st, pa, m, bones);
        for (uint32_t i = 0; i < st->ndyn; i++) st->dyn[i].p = st->dyn[i].pp = dyn_point(&st->dyn[i], (const float(*)[3][4])bones);
        st->dyn_started = true, st->dyn_version = pa->version, st->dyn_root_prev = root;
    }
    if (!st->ndyn || dt <= 0.0f) return;
    // the capsules it collides with: the bodies at this pose (the ragdoll's when it simulates), not the chains' own nor
    // the body each chain hangs from (an earring starts inside the head's sphere)
    float caps[PHYS_MAX_BODIES][7];
    uint64_t cap_chain_skip[PHYS_MAX_CHAINS] = { 0 };
    uint32_t ncap = 0;
    for (uint32_t ci = 0; ci < pa->nchains && ci < PHYS_MAX_CHAINS; ci++) cap_chain_skip[ci] = st->dyn_bind_skip[ci];
    for (uint32_t i = 0; i < pa->nbodies && ncap < PHYS_MAX_BODIES; i++) {
        int b = bone_of(m, pa->bodies[i].bone);
        if (b < 0) {
            caps[ncap++][6] = -1e9f; // (no bone: never touched; the indices stay the bodies')
            continue;
        }
        V3 a, c;
        body_capsule(&pa->bodies[i], (const float(*)[4])bones[b], &a, &c);
        float r = pa->bodies[i].shape == PHYS_BOX ? fmaxf(fmaxf(pa->bodies[i].extent[0], pa->bodies[i].extent[1]), pa->bodies[i].extent[2]) : pa->bodies[i].radius;
        float* q = caps[ncap];
        q[0] = a.x, q[1] = a.y, q[2] = a.z, q[3] = c.x, q[4] = c.y, q[5] = c.z, q[6] = r;
        for (uint32_t ci = 0; ci < pa->nchains; ci++) { // (skip: a body on the chain's bones or the nearest above its first)
            int cr = bone_of(m, pa->chains[ci].bone);
            bool skip = false;
            for (int u = cr; u >= 0 && !skip; u = m->bones[u].parent) { // (the nearest body above or on the chain's first bone)
                bool has = false;
                for (uint32_t j = 0; j < pa->nbodies && !has; j++) has = !strcmp(pa->bodies[j].bone, m->bones[u].name);
                if (has) { skip = u == b; break; }
            }
            for (int u = b; u >= 0 && !skip; u = m->bones[u].parent) skip = u == cr; // (inside the chain)
            if (skip) cap_chain_skip[ci] |= 1ull << ncap;
        }
        ncap++;
    }
    // the character's own motion (its root's), felt by each chain by its inertia
    V3 move = sub3(root, st->dyn_root_prev);
    st->dyn_root_prev = root;
    const int nsub = 2;
    float h = dt / (float)nsub;
    V3 wind = st->dyn_wind;
    V3 tgt[64], anc[64];
    for (uint32_t i = 0; i < st->ndyn; i++) {
        const struct DynNode* n = &st->dyn[i];
        tgt[i] = dyn_point(n, (const float(*)[3][4])bones);
        anc[i] = row_pos(bones[dyn_seg_bone(n, m)]);
    }
    for (int sub = 0; sub < nsub; sub++) {
        for (uint32_t i = 0; i < st->ndyn; i++) {
            struct DynNode* n = &st->dyn[i];
            const DynChain* c = &pa->chains[n->chain];
            V3 P = n->parent_node >= 0 ? st->dyn[n->parent_node].p : anc[i]; // (the point it hangs from, simulated)
            V3 carried = mul3(move, (1.0f - fminf(fmaxf(c->inertia, 0.0f), 1.0f)) / (float)nsub);
            n->p = add3(n->p, carried), n->pp = add3(n->pp, carried);
            float keep = powf(1.0f - fminf(fmaxf(c->damping, 0.0f), 0.99f), h * 60.0f);
            V3 v = mul3(sub3(n->p, n->pp), keep);
            V3 acc = add3(v3(0, 0, -980.0f * c->gravity), mul3(wind, c->wind * 3.0f)); // (the wind's drag: a few times its speed a second)
            V3 np = add3(add3(n->p, v), mul3(acc, h * h));
            // the animated shape from the simulated parent: a spring toward it
            V3 goal = add3(P, sub3(tgt[i], anc[i]));
            float ks = 1.0f - powf(1.0f - fminf(fmaxf(c->stiffness, 0.0f), 0.999f), h * 60.0f);
            np = lerp3(np, goal, ks);
            // its length from the point it hangs from
            V3 d = sub3(np, P);
            float dl = len3(d);
            np = dl > 1e-5f ? add3(P, mul3(d, n->len / dl)) : goal;
            // its angle from the animated direction
            if (c->limit > 0.0f) {
                V3 da = norm3(sub3(tgt[i], anc[i])), dn = norm3(sub3(np, P));
                float ang = acosf(fmaxf(fminf(dot3(da, dn), 1.0f), -1.0f)), lim = c->limit * 0.017453293f;
                if (ang > lim) {
                    M3 back = rot_between(dn, da, (ang - lim) / ang);
                    np = add3(P, mul3(m3v(&back, dn), n->len));
                }
            }
            // the bodies: out of their capsules (its radius thick)
            if (c->collide)
                for (uint32_t k = 0; k < ncap; k++) {
                    if ((cap_chain_skip[n->chain] >> k) & 1ull) continue;
                    V3 a = v3(caps[k][0], caps[k][1], caps[k][2]), b = v3(caps[k][3], caps[k][4], caps[k][5]);
                    V3 q = closest_on_seg(a, b, np), dq = sub3(np, q);
                    float l = len3(dq), r = caps[k][6] + c->radius;
                    if (l >= r) continue;
                    V3 nrm = l > 1e-5f ? mul3(dq, 1.0f / l) : norm3(sub3(np, P));
                    np = add3(q, mul3(nrm, r));
                    n->pp = lerp3(n->pp, np, 0.3f); // (friction: the slide along the body slowed)
                }
            n->pp = n->p, n->p = np;
        }
    }
    // the bones turned toward their particles, parents first (their descendants carried by their animated local turns)
    float old[ANIM_MAX_BONES][3][4];
    memcpy(old, bones, sizeof(float) * 12 * m->nbones);
    bool done[ANIM_MAX_BONES] = { 0 };
    for (uint32_t i = 0; i < st->ndyn; i++) {
        const struct DynNode* n = &st->dyn[i];
        int sb = dyn_seg_bone(n, m);
        if (sb < 0 || done[sb]) continue; // (a branch: its first segment turns the bone)
        done[sb] = true;
        V3 head = row_pos(bones[sb]);
        V3 cur = sub3(dyn_point(n, (const float(*)[3][4])bones), head), want = sub3(n->p, head);
        if (len3(cur) < 1e-4f || len3(want) < 1e-4f) continue;
        M3 turn = rot_between(norm3(cur), norm3(want), 1.0f);
        float sc[3];
        M3 r = row_rot(bones[sb], sc), nr = m3mul(&turn, &r);
        row_set(bones[sb], &nr, sc, head);
        bool moved[ANIM_MAX_BONES] = { 0 };
        moved[sb] = true;
        propagate(m, old, bones, moved);
        memcpy(old, bones, sizeof(float) * 12 * m->nbones); // (the new places are the base of the next bones' local turns)
    }
    K.acc.chains += pa->nchains, K.acc.dyn_bones += st->ndyn, K.acc.dyn_ms += (re_time_seconds() - t0) * 1000.0;
}

void phys_publish_bodies(Entity sk, const PhysicsAsset* pa, const SkelMesh* m, const float bones[][3][4]) {
    PState* st = state_of(sk, false);
    if (!st || !pa || !m) return;
    if (st->cap_version != pa->version + 1u || st->cap_mesh != m) {
        for (uint32_t i = 0; i < pa->nbodies; i++) st->cap_bone[i] = (int16_t)bone_of(m, pa->bodies[i].bone);
        st->cap_version = pa->version + 1u, st->cap_mesh = m;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < pa->nbodies && n < PHYS_MAX_BODIES; i++) {
        int b = st->cap_bone[i];
        if (b < 0 || b >= (int)m->nbones) continue;
        V3 a, c;
        body_capsule(&pa->bodies[i], bones[b], &a, &c);
        float* q = st->caps[n];
        if (st->caps_ok && n < st->ncaps) memcpy(st->caps_prev[n], q, sizeof st->caps_prev[n]);
        q[0] = a.x, q[1] = a.y, q[2] = a.z, q[3] = c.x, q[4] = c.y, q[5] = c.z;
        q[6] = pa->bodies[i].shape == PHYS_BOX ? fmaxf(pa->bodies[i].extent[1], pa->bodies[i].extent[2]) : pa->bodies[i].radius;
        if (!st->caps_ok || n >= st->ncaps) memcpy(st->caps_prev[n], q, sizeof st->caps_prev[n]);
        st->cap_body[n] = (int8_t)i;
        n++;
    }
    st->ncaps = n, st->caps_ok = true;
}

// --- The self-test ---
// (2026-11-16) The active ragdoll's test: a biped and a quadruped made here, standing on the ground (their bind pose as
// the animation): they stand still, fall to a strong hit, get up by themselves, and take a light push without falling
typedef struct TBone { const char* name; int parent; float t[3]; } TBone;
static SkelMesh* make_skel(const TBone* tb, int n) {
    static SkelMesh m;
    memset(&m, 0, sizeof m);
    m.nbones = (uint32_t)n;
    AnimXf loc[ANIM_MAX_BONES];
    for (int b = 0; b < n; b++) {
        snprintf(m.bones[b].name, ANIM_NAME, "%s", tb[b].name);
        m.bones[b].parent = (int16_t)tb[b].parent;
        memset(&m.bones[b].bind, 0, sizeof m.bones[b].bind);
        m.bones[b].bind.q[3] = 1.0f, m.bones[b].bind.s[0] = m.bones[b].bind.s[1] = m.bones[b].bind.s[2] = 1.0f;
        memcpy(m.bones[b].bind.t, tb[b].t, 12);
        loc[b] = m.bones[b].bind;
    }
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    anim_world(&m, loc, id, m.bind_world);
    return &m;
}
static int bits64(uint64_t x) { int n = 0; for (; x; x &= x - 1) n++; return n; } // (portable: clang has no __popcnt64)
static int active_run(const char* what, const TBone* tb, int n) {
    int fails = 0;
    SkelMesh* m = make_skel(tb, n);
    static PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    Entity fake = 0x7FFFFFE0u;
    static World w0;
    if (getenv("REALENGINE_AR_PLAIN")) { // (a diagnosis: the same skeleton as a plain ragdoll, pushed: does it settle?)
        phys_set_simulate(fake, PHYS_RAGDOLL, NULL);
        phys_add_impulse(fake, tb[2].name, (float[3]){ 600.0f, 0.0f, 0.0f });
        float bones2[ANIM_MAX_BONES][3][4];
        for (int f = 0; f < 300; f++) {
            memcpy(bones2, m->bind_world, sizeof(float) * 12 * m->nbones);
            phys_apply(&w0, fake, &pa, m, bones2, 1.0f / 60.0f);
            IkPhysInfo pi;
            ikphys_info(fake, &pi);
            if (f % 30 == 29) printf("[RealEngine] plain ragdoll (%s) t %.1f: fastest body %d at %.0f cm/s, z %.1f..%.1f\n", what, (f + 1) / 60.0f, pi.fastest, pi.vmax, pi.zmin, pi.zmax);
        }
        ikphys_forget(fake);
    }
    phys_set_simulate(fake, PHYS_ACTIVE, NULL);
    phys_set_auto_get_up(fake, true, 1.0f);
    float bones[ANIM_MAX_BONES][3][4];
    IkPhysInfo info;
    double ms = 0.0;
    int frames = 0;
#define STEP(nf)                                                                                                           \
    for (int f = 0; f < (nf); f++) {                                                                                       \
        memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);                                                      \
        double t0 = re_time_seconds();                                                                                     \
        phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);                                                                \
        ms += (re_time_seconds() - t0) * 1000.0, frames++;                                                                \
    }
    STEP(180); // 3 s standing
    ikphys_info(fake, &info);
    float rest_z = m->bind_world[0][2][3];
    PState* st = state_of(fake, false);
    printf("[RealEngine] active ragdoll (%s, %u bodies, %d feet): standing 3 s: balance %d, root at %.1f cm (animated %.1f)\n", what, pa.nbodies,
           st ? bits64(st->feet) : 0, info.balance, info.root_z, rest_z);
    if (info.balance != BAL_BALANCED || fabsf(info.root_z - rest_z) > 8.0f) printf("[RealEngine] FAIL: active ragdoll (%s): it didn't stand\n", what), fails++;
    // a strong hit on the chest: it falls
    phys_hit_reaction(fake, tb[2].name, (float[3]){ 900.0f, 0.0f, 150.0f }, 0.1f, 1.0f);
    int fell = 0;
    for (int f = 0; f < 150 && !fell; f++) {
        STEP(1);
        ikphys_info(fake, &info);
        fell = info.balance == BAL_FALLEN;
        if (getenv("REALENGINE_Q10_DEBUG") && f % 6 == 0 && st) printf("[RealEngine] dbg: %s f %d root %.1f %.1f %.1f support %.2f bal %d v %.0f\n", what, f, st->b[st->root].x.x, st->b[st->root].x.y, st->b[st->root].x.z, st->support, st->bal, len3(st->b[st->root].v));
    }
    printf("[RealEngine] active ragdoll (%s): a strong hit: %s (root at %.1f cm)\n", what, fell ? "fell" : "still up", info.root_z);
    if (!fell) printf("[RealEngine] FAIL: active ragdoll (%s): a strong hit didn't knock it down\n", what), fails++;
    // lying still, then up by itself (Auto Get Up after 1 s), standing again
    int up = 0;
    for (int f = 0; f < 600 && !up; f++) {
        STEP(1);
        ikphys_info(fake, &info);
        up = info.balance == BAL_BALANCED;
    }
    STEP(60);
    ikphys_info(fake, &info);
    printf("[RealEngine] active ragdoll (%s): got up: %s, root at %.1f cm\n", what, up ? "yes" : "no", info.root_z);
    if (!up || info.balance != BAL_BALANCED || fabsf(info.root_z - rest_z) > 10.0f) printf("[RealEngine] FAIL: active ragdoll (%s): it didn't get up\n", what), fails++;
    // a light push: a sway, still standing (or a step and standing)
    phys_hit_reaction(fake, tb[2].name, (float[3]){ 120.0f, 0.0f, 0.0f }, 0.7f, 0.5f);
    int worst = 0;
    for (int f = 0; f < 120; f++) {
        STEP(1);
        ikphys_info(fake, &info);
        if (info.balance > worst) worst = info.balance;
    }
    printf("[RealEngine] active ragdoll (%s): a light push: %s, %.3f ms a frame\n", what, worst >= BAL_FALLEN ? "fell" : worst ? "stumbled, stood" : "swayed", ms / frames);
    if (worst >= BAL_FALLEN || info.balance != BAL_BALANCED) printf("[RealEngine] FAIL: active ragdoll (%s): a light push knocked it down\n", what), fails++;
#undef STEP
    ikphys_forget(fake);
    return fails;
}
static int active_selftest(void) {
    static const TBone biped[] = {
        { "pelvis", -1, { 0, 0, 95 } },     { "spine_01", 0, { 0, 0, 20 } },   { "spine_02", 1, { 0, 0, 22 } },
        { "head", 2, { 0, 0, 26 } },        { "thigh_l", 0, { -12, 0, -6 } },  { "calf_l", 4, { 0, 0, -42 } },
        { "foot_l", 5, { 0, 0, -41 } },     { "ball_l", 6, { 0, 12, -5 } },    { "thigh_r", 0, { 12, 0, -6 } },
        { "calf_r", 8, { 0, 0, -42 } },     { "foot_r", 9, { 0, 0, -41 } },    { "ball_r", 10, { 0, 12, -5 } },
        { "upperarm_l", 2, { -18, 0, 18 } }, { "lowerarm_l", 12, { 0, 0, -28 } }, { "hand_l", 13, { 0, 0, -26 } },
        { "upperarm_r", 2, { 18, 0, 18 } }, { "lowerarm_r", 15, { 0, 0, -28 } }, { "hand_r", 16, { 0, 0, -26 } },
    };
    static const TBone quad[] = {
        { "pelvis", -1, { 0, -40, 70 } },   { "spine_01", 0, { 0, 30, 0 } },   { "spine_02", 1, { 0, 30, 0 } },
        { "neck", 2, { 0, 20, 10 } },       { "head", 3, { 0, 14, 6 } },       { "upperarm_l", 2, { -12, 8, -5 } },
        { "lowerarm_l", 5, { 0, 0, -30 } }, { "paw_fl", 6, { 0, 0, -30 } },    { "upperarm_r", 2, { 12, 8, -5 } },
        { "lowerarm_r", 8, { 0, 0, -30 } }, { "paw_fr", 9, { 0, 0, -30 } },    { "thigh_l", 0, { -12, 0, -5 } },
        { "calf_l", 11, { 0, 0, -30 } },    { "paw_bl", 12, { 0, 0, -30 } },   { "thigh_r", 0, { 12, 0, -5 } },
        { "calf_r", 14, { 0, 0, -30 } },    { "paw_br", 15, { 0, 0, -30 } },   { "tail", 0, { 0, -15, 5 } },
    };
    return active_run("biped", biped, (int)(sizeof biped / sizeof biped[0])) + active_run("quadruped", quad, (int)(sizeof quad / sizeof quad[0]));
}

// --- (Q10) the self-tests of the realistic ragdoll, the active ragdoll's bracing and the dynamic bones ---
// The biped of the tests: arms hanging, knees and elbows straight; earrings on the head and a tail on the pelvis
static const TBone k_q10_biped[] = {
    { "pelvis", -1, { 0, 0, 95 } },      { "spine_01", 0, { 0, 0, 20 } },     { "spine_02", 1, { 0, 0, 22 } },
    { "head", 2, { 0, 0, 26 } },         { "thigh_l", 0, { -12, 0, -6 } },    { "calf_l", 4, { 0, 0, -42 } },
    { "foot_l", 5, { 0, 0, -41 } },      { "ball_l", 6, { 0, 12, -5 } },      { "thigh_r", 0, { 12, 0, -6 } },
    { "calf_r", 8, { 0, 0, -42 } },      { "foot_r", 9, { 0, 0, -41 } },      { "ball_r", 10, { 0, 12, -5 } },
    { "upperarm_l", 2, { -18, 0, 18 } }, { "lowerarm_l", 12, { 0, 0, -28 } }, { "hand_l", 13, { 0, 0, -26 } },
    { "upperarm_r", 2, { 18, 0, 18 } },  { "lowerarm_r", 15, { 0, 0, -28 } }, { "hand_r", 16, { 0, 0, -26 } },
    { "earring_l", 3, { -9, 0, -3 } },   { "earring_l_01", 18, { 0, 0, -5 } }, { "earring_l_02", 19, { 0, 0, -5 } },
    { "earring_l_03", 20, { 0, 0, -5 } }, { "tail_01", 0, { 0, -12, -2 } },   { "tail_02", 22, { 0, -12, 0 } },
    { "tail_03", 23, { 0, -12, 0 } },    { "tail_04", 24, { 0, -12, 0 } },
};
#define Q10_BIPED_N ((int)(sizeof k_q10_biped / sizeof k_q10_biped[0]))
static int body_named(const PhysicsAsset* pa, const char* bone) {
    for (uint32_t i = 0; i < pa->nbodies; i++)
        if (!strcmp(pa->bodies[i].bone, bone)) return (int)i;
    return -1;
}
static int con_of_child(const PhysicsAsset* pa, int body) {
    for (uint32_t c = 0; c < pa->nconstraints; c++)
        if (pa->constraints[c].child == body) return (int)c;
    return -1;
}

// ragdoll_limits: the anatomy made by Generate Bodies (knees fold back, elbows forward, masses by volume) and, as a
// plain ragdoll falls pushed four ways and kicked at a knee, no joint past its limits and no two bodies through each other
static int limits_selftest(void) {
    int fails = 0;
    SkelMesh* m = make_skel(k_q10_biped, Q10_BIPED_N);
    static PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    V3 fwd = skel_forward(m);
    float total = 0.0f;
    for (uint32_t i = 0; i < pa.nbodies; i++) total += pa.bodies[i].mass;
    int knee = con_of_child(&pa, body_named(&pa, "calf_l")), elbow = con_of_child(&pa, body_named(&pa, "lowerarm_l"));
    printf("[RealEngine] ragdoll_limits: %u bodies (%.1f kg by volume), %u joints; forward (%.2f, %.2f)\n", pa.nbodies, total, pa.nconstraints, fwd.x, fwd.y);
    if (knee < 0 || elbow < 0) return printf("[RealEngine] FAIL: ragdoll_limits: no knee or elbow joint\n"), 1;
    const int which[2] = { knee, elbow };
    for (int k = 0; k < 2; k++) {
        const PhysConstraint* c = &pa.constraints[which[k]];
        M3 rp = row_rot(m->bind_world[bone_of(m, pa.bodies[c->parent].bone)], NULL);
        V3 bw = m3v(&rp, v3(c->bend[0], c->bend[1], c->bend[2]));
        float way = dot3(bw, fwd);
        printf("[RealEngine] ragdoll_limits: %s: %s, bends %.0f..%.0f deg %s, sides %.0f..%.0f, twist %.0f..%.0f\n", k ? "elbow" : "knee",
               g_joint_kind_names[c->kind], c->bend_min, c->bend_max, way > 0 ? "forward" : "backward", c->side_min, c->side_max, c->twist_min, c->twist_max);
        if (c->kind != PJ_HINGE || (k == 0 ? way > -0.5f : way < 0.5f) || c->bend_max < 120.0f || c->bend_min < -5.0f || c->side_max > 10.0f)
            printf("[RealEngine] FAIL: ragdoll_limits: the %s is not a hinge folding %s only\n", k ? "elbow" : "knee", k ? "forward" : "backward"), fails++;
    }
    if (total < 30.0f || total > 140.0f) printf("[RealEngine] FAIL: ragdoll_limits: %.1f kg for a person\n", total), fails++;
    Entity fake = 0x7FFFFFD0u;
    static World w0;
    float bones[ANIM_MAX_BONES][3][4];
    float worst = 0.0f, pen = 0.0f, vend = 0.0f;
    int worst_c = -1;
    double ms = 0.0;
    int frames = 0;
    for (int dir = 0; dir < 5; dir++) {
        phys_set_simulate(fake, PHYS_RAGDOLL, NULL);
        float a = (float)dir * 1.5707963f;
        if (dir < 4) phys_add_impulse(fake, "spine_02", (float[3]){ cosf(a) * 500.0f, sinf(a) * 500.0f, 80.0f });
        else phys_add_impulse(fake, "calf_l", (float[3]){ fwd.x * 900.0f, fwd.y * 900.0f, 0.0f }), phys_add_impulse(fake, "lowerarm_r", (float[3]){ -fwd.x * 900.0f, -fwd.y * 900.0f, 0.0f }); // (a knee kicked forward, an elbow back: the wrong ways)
        for (int f = 0; f < 240; f++) {
            memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
            double t0 = re_time_seconds();
            phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
            ms += (re_time_seconds() - t0) * 1000.0, frames++;
            IkPhysInfo info;
            ikphys_info(fake, &info);
            if (f > 1 && info.limit_over > worst) {
                worst = info.limit_over, worst_c = info.limit_worst;
                if (getenv("REALENGINE_Q10_DEBUG")) {
                    PState* sd = state_of(fake, false);
                    const PhysConstraint* pc = &pa.constraints[worst_c];
                    V3 fr, to, ax;
                    float turn, sw = joint_swing(pc, &pa.bodies[pc->child], &sd->b[pc->parent], &sd->b[pc->child], &fr, &to), tw = joint_twist(pc, &pa.bodies[pc->child], &sd->b[pc->parent], &sd->b[pc->child], &ax, &turn);
                    printf("[RealEngine] dbg: fall %d frame %d: %s swing over %.1f twist over %.1f (kind %d)\n", dir, f, pa.bodies[pc->child].bone, sw, tw, pc->kind);
                }
            }
            if (f > 1) pen = fmaxf(pen, info.self_pen);
            if (f == 239) vend = fmaxf(vend, info.vmax);
            if (dir == 1 && f % 20 == 0 && getenv("REALENGINE_Q10_DEBUG")) {
                PState* sd = state_of(fake, false);
                int t3 = body_named(&pa, "tail_03"), t1 = body_named(&pa, "tail_01");
                printf("[RealEngine] dbg: f %d tail_03 at %.1f %.1f %.1f v %.0f w %.1f; tail_01 v %.0f; limit %.1f\n", f, sd->b[t3].x.x, sd->b[t3].x.y, sd->b[t3].x.z, len3(sd->b[t3].v), len3(sd->b[t3].w), len3(sd->b[t1].v), info.limit_over);
            }
            if (f == 239 && getenv("REALENGINE_Q10_DEBUG")) {
                PState* sd = state_of(fake, false);
                for (uint32_t i = 0; i < pa.nbodies; i++)
                    if (len3(sd->b[i].w) > 3.0f || len3(sd->b[i].v) > 20.0f) printf("[RealEngine] dbg:   %s v %.0f w %.1f touch %d z %.1f mass %.2f\n", pa.bodies[i].bone, len3(sd->b[i].v), len3(sd->b[i].w), sd->b[i].touch, sd->b[i].x.z, pa.bodies[i].mass);
            }
            if (f == 239 && getenv("REALENGINE_Q10_DEBUG")) printf("[RealEngine] dbg: fall %d end: fastest %s %.0f cm/s, spinning %s %.1f rad/s, asleep %d\n", dir, pa.bodies[info.fastest].bone, info.vmax, pa.bodies[info.spinning].bone, info.wmax, info.asleep);
        }
        ikphys_forget(fake);
    }
    const char* wn = worst_c >= 0 ? pa.bodies[pa.constraints[worst_c].child].bone : "-";
    printf("[RealEngine] ragdoll_limits: 5 falls: the worst joint %.2f deg past its limits (%s), two bodies %.2f cm into each other at most, %.0f cm/s at the end, %.3f ms a frame\n",
           worst, wn, fmaxf(pen, 0.0f), vend, ms / frames);
    if (worst > 3.0f) printf("[RealEngine] FAIL: ragdoll_limits: a joint went %.1f deg past its limits (%s)\n", worst, wn), fails++;
    if (pen > 2.5f) printf("[RealEngine] FAIL: ragdoll_limits: two bodies %.1f cm through each other\n", pen), fails++;
    if (vend > 40.0f) printf("[RealEngine] FAIL: ragdoll_limits: still moving at %.0f cm/s after 4 s\n", vend), fails++;
    // a plain ragdoll's tone: it falls as a body, not a rag (its knees and spine keep some of the pose it had)
    printf("[RealEngine] ragdoll_limits: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

// ragdoll_active: the active ragdoll stands, falls to a strong hit, gets up (as before); pushed forward it braces on
// its hands (they reach the ground before the head and the chest) and lies face down, pushed back it lies on its back;
// an arm let go below the shoulder (Set All Bodies Below Simulate Physics) swings and is handed back to the animation
// (Set All Bodies Below Physics Blend Weight 0) where it was animated
static int ragdoll_active_selftest(void) {
    int fails = active_selftest();
    SkelMesh* m = make_skel(k_q10_biped, Q10_BIPED_N);
    static PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    V3 fwd = skel_forward(m);
    Entity fake = 0x7FFFFFC0u;
    static World w0;
    float bones[ANIM_MAX_BONES][3][4];
    int hl = body_named(&pa, "hand_l"), hr = body_named(&pa, "hand_r"), hd = body_named(&pa, "head"), ch = body_named(&pa, "spine_02");
    for (int way = 0; way < 2; way++) {
        phys_set_simulate(fake, PHYS_ACTIVE, NULL);
        phys_set_auto_get_up(fake, true, 1.0f);
        for (int f = 0; f < 60; f++) memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones), phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
        float s = way ? -1.0f : 1.0f;
        phys_hit_reaction(fake, "spine_02", (float[3]){ fwd.x * 1100.0f * s, fwd.y * 1100.0f * s, 60.0f }, 0.15f, 1.0f);
        int first = -1, face = -1, fell = 0;
        float t_hand = -1.0f, t_head = -1.0f;
        PState* st = state_of(fake, false);
        for (int f = 0; f < 240 && st; f++) {
            memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
            phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
            float tt = (float)f / 60.0f;
            const int who[4] = { hl, hr, hd, ch };
            for (int k = 0; k < 4; k++) {
                if (who[k] < 0) continue;
                bool down = st->b[who[k]].x.z - pa.bodies[who[k]].radius < 1.0f; // (the ground at -1 cm)
                if (down && first < 0) first = k;
                if (down && k < 2 && t_hand < 0.0f) t_hand = tt;
                if (down && k >= 2 && t_head < 0.0f) t_head = tt;
            }
            if (st->bal == BAL_FALLEN) fell = 1, face = phys_face_down(fake);
            if (fell && st->bal == BAL_FALLEN && st->bal_t > 0.8f) break;
        }
        printf("[RealEngine] ragdoll_active: pushed %s: %s, lies %s; first on the ground: %s (hands at %.2f s, head/chest at %.2f s)\n", way ? "back" : "forward",
               fell ? "fell" : "still up", face == 1 ? "face down" : face == 0 ? "on its back" : "-", first < 0 ? "-" : first < 2 ? "a hand" : first == 2 ? "the head" : "the chest", t_hand, t_head);
        if (!fell) printf("[RealEngine] FAIL: ragdoll_active: pushed %s it didn't fall\n", way ? "back" : "forward"), fails++;
        if (face != (way ? 0 : 1)) printf("[RealEngine] FAIL: ragdoll_active: pushed %s it should lie %s\n", way ? "back" : "forward", way ? "on its back" : "face down"), fails++;
        if (!way && first >= 2) printf("[RealEngine] FAIL: ragdoll_active: falling forward the %s hit the ground before the hands\n", first == 2 ? "head" : "chest"), fails++;
        int up = 0;
        for (int f = 0; f < 600 && !up && st; f++) {
            memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
            phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
            up = st->bal == BAL_BALANCED;
        }
        for (int f = 0; f < 60; f++) memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones), phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
        IkPhysInfo info;
        ikphys_info(fake, &info);
        float rest = m->bind_world[0][2][3];
        printf("[RealEngine] ragdoll_active: got up %s (root at %.1f cm, animated %.1f)\n", up ? "yes" : "no", info.root_z, rest);
        if (!up || fabsf(info.root_z - rest) > 10.0f) printf("[RealEngine] FAIL: ragdoll_active: it didn't get back to the animation\n"), fails++;
        ikphys_forget(fake);
    }
    // an arm alone: simulated from the shoulder down, pushed, then handed back to the animation over 0.3 s
    phys_set_simulate(fake, PHYS_BELOW, "upperarm_l");
    phys_add_impulse(fake, "lowerarm_l", (float[3]){ -400.0f, 200.0f, 0.0f });
    int hb = bone_of(m, "hand_l"), pb = bone_of(m, "pelvis");
    float swing = 0.0f, body_moved = 0.0f, back = 0.0f;
    for (int f = 0; f < 150; f++) {
        if (f == 90) phys_set_blend_below(fake, "upperarm_l", 0.0f, 0.3f);
        memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
        phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
        float d = len3(sub3(row_pos(bones[hb]), row_pos(m->bind_world[hb])));
        if (f < 90) swing = fmaxf(swing, d);
        else back = d;
        body_moved = fmaxf(body_moved, len3(sub3(row_pos(bones[pb]), row_pos(m->bind_world[pb]))));
    }
    printf("[RealEngine] ragdoll_active: the left arm alone: swung %.1f cm, the body moved %.2f cm; blended back: %.2f cm from the animation\n", swing, body_moved, back);
    if (swing < 5.0f || body_moved > 0.01f || back > 0.5f) printf("[RealEngine] FAIL: ragdoll_active: the arm alone (bodies below a bone, their blend)\n"), fails++;
    ikphys_forget(fake);
    printf("[RealEngine] ragdoll_active: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

// dyn_bones: an earring (3 bones) on the head and a tail (4 bones) on the pelvis as dynamic chains; the body sways side
// to side 1.5 s and stops: they swing behind it, settle, and the earring never goes into the arm or the chest
static int dyn_selftest(void) {
    int fails = 0;
    SkelMesh* m = make_skel(k_q10_biped, Q10_BIPED_N);
    static PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    int ce = phys_add_chain(&pa, "earring_l", DYN_EARRING), ct = phys_add_chain(&pa, "tail_01", DYN_TAIL);
    Entity fake = 0x7FFFFFB0u;
    PState* st = state_of(fake, true);
    static World w0;
    if (ce < 0 || ct < 0 || !st) return printf("[RealEngine] FAIL: dyn_bones: the chains\n"), 1;
    int tip_e = bone_of(m, "earring_l_03"), tip_t = bone_of(m, "tail_04");
    int arm = body_named(&pa, "upperarm_l"), chest = body_named(&pa, "spine_02");
    float bones[ANIM_MAX_BONES][3][4], anim_tip[3];
    float swing_e = 0.0f, swing_t = 0.0f, pen = 1e9f, v_end = 0.0f, v_tail = 0.0f, rest_e = 0.0f, rest_t = 0.0f;
    V3 prev_e = v3(0, 0, 0), prev_t = v3(0, 0, 0);
    double ms = 0.0;
    int frames = 0;
    const float dt = 1.0f / 60.0f;
    for (int f = 0; f < 300; f++) {
        float t = (float)f * dt, x = t < 1.5f ? 25.0f * sinf(6.2831853f * 1.2f * t) : 25.0f * sinf(6.2831853f * 1.2f * 1.5f);
        memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
        for (uint32_t b = 0; b < m->nbones; b++) bones[b][0][3] += x;
        anim_tip[0] = bones[tip_e][0][3], anim_tip[1] = bones[tip_e][1][3], anim_tip[2] = bones[tip_e][2][3];
        float anim_tail[3] = { bones[tip_t][0][3], bones[tip_t][1][3], bones[tip_t][2][3] };
        double t0 = re_time_seconds();
        dyn_apply(&w0, fake, &pa, m, bones, dt);
        ms += (re_time_seconds() - t0) * 1000.0, frames++;
        V3 pe = row_pos(bones[tip_e]), pt = row_pos(bones[tip_t]);
        float de = len3(sub3(pe, v3(anim_tip[0], anim_tip[1], anim_tip[2]))), dtl = len3(sub3(pt, v3(anim_tail[0], anim_tail[1], anim_tail[2])));
        if (t < 1.6f) swing_e = fmaxf(swing_e, de), swing_t = fmaxf(swing_t, dtl);
        if (f > 0 && t > 4.0f) v_end = fmaxf(v_end, de), v_tail = fmaxf(v_tail, dtl); // (settled: near where it rests, the last second)
        if (f == 299) rest_e = de, rest_t = dtl;
        if (getenv("REALENGINE_Q10_DEBUG") && f > 285) printf("[RealEngine] dbg: f %d earring tip %.4f %.4f %.4f anim %.4f %.4f %.4f\n", f, pe.x, pe.y, pe.z, anim_tip[0], anim_tip[1], anim_tip[2]);
        prev_e = pe, prev_t = pt;
        // the earring's particles against the arm's and the chest's capsules (its radius thick)
        for (uint32_t i = 0; i < st->ndyn; i++) {
            if (st->dyn[i].chain != ce) continue;
            const int bodies2[2] = { arm, chest };
            for (int k = 0; k < 2; k++) {
                if (bodies2[k] < 0 || ((st->dyn_bind_skip[ce] >> bodies2[k]) & 1ull)) continue; // (one it starts inside: left out)
                V3 a, c;
                body_capsule(&pa.bodies[bodies2[k]], (const float(*)[4])bones[bone_of(m, pa.bodies[bodies2[k]].bone)], &a, &c);
                float d = len3(sub3(st->dyn[i].p, closest_on_seg(a, c, st->dyn[i].p))) - pa.bodies[bodies2[k]].radius - pa.chains[ce].radius;
                pen = fminf(pen, d);
            }
        }
    }
    printf("[RealEngine] dyn_bones: %u particles; swaying: the earring %.1f cm and the tail %.1f cm from the animation; stopped 2.5 s: the earring within %.2f cm of its rest and the tail %.2f cm (the last second), the earring %.2f cm from its rest; the earring's closest to the arm and chest %.2f cm; %.4f ms a frame\n",
           st->ndyn, swing_e, swing_t, v_end, v_tail, rest_e, pen, ms / frames);
    v_tail -= rest_t; // (the tail rests sagging a little under its weight: its swing about that)
    if (swing_e < 2.0f || swing_t < 1.0f) printf("[RealEngine] FAIL: dyn_bones: they should swing behind the body\n"), fails++;
    if (v_end > 0.5f || v_tail > 0.5f) printf("[RealEngine] FAIL: dyn_bones: still swinging (%.2f, %.2f cm) 2.5 s after the body stopped\n", v_end, v_tail), fails++;
    if (pen < -0.5f) printf("[RealEngine] FAIL: dyn_bones: the earring went %.2f cm into the body\n", -pen), fails++;
    if (pen > 0.5f) printf("[RealEngine] FAIL: dyn_bones: the earring never touched the body (the test's swing is meant to reach it)\n"), fails++;
    // the measure: 50 characters each with the earring and the tail, a frame of them all (one thread)
    {
        double t50 = 0.0;
        int f50 = 0;
        for (int f = 0; f < 120; f++) {
            float x = 20.0f * sinf((float)f * 0.12f);
            double t1 = re_time_seconds();
            for (int c = 0; c < 50; c++) {
                Entity e = (Entity)(0x7FFF0000u + (uint32_t)c);
                if (f == 0) state_of(e, true);
                memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
                for (uint32_t b = 0; b < m->nbones; b++) bones[b][0][3] += x + (float)c * 200.0f;
                dyn_apply(&w0, e, &pa, m, bones, dt);
            }
            if (f >= 20) t50 += (re_time_seconds() - t1) * 1000.0, f50++;
        }
        for (int c = 0; c < 50; c++) ikphys_forget((Entity)(0x7FFF0000u + (uint32_t)c));
        printf("[RealEngine] dyn_bones: 50 characters (8 particles each): %.3f ms a frame on one thread\n", t50 / f50);
    }
    // off: the bones are the animation's
    dyn_set_enabled(fake, false);
    memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
    dyn_apply(&w0, fake, &pa, m, bones, dt);
    if (len3(sub3(row_pos(bones[tip_e]), row_pos(m->bind_world[tip_e]))) > 1e-4f) printf("[RealEngine] FAIL: dyn_bones: off, they still moved\n"), fails++;
    ikphys_forget(fake);
    printf("[RealEngine] dyn_bones: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

// --- (Q27.5) the reactions of GTA 6's kind and the budget ---
// The test biped of active_selftest: a wounded calf limps (the pelvis lower, still standing), a chest wound brings the
// right hand to it, a grave one knocks it down and it writhes before it gets up, a fall from 8 m flails the arms, and of
// 200 active ragdolls in a line only the 32 nearest the view simulate
static const TBone q275_biped[] = {
    { "pelvis", -1, { 0, 0, 95 } },     { "spine_01", 0, { 0, 0, 20 } },   { "spine_02", 1, { 0, 0, 22 } },
    { "head", 2, { 0, 0, 26 } },        { "thigh_l", 0, { -12, 0, -6 } },  { "calf_l", 4, { 0, 0, -42 } },
    { "foot_l", 5, { 0, 0, -41 } },     { "ball_l", 6, { 0, 12, -5 } },    { "thigh_r", 0, { 12, 0, -6 } },
    { "calf_r", 8, { 0, 0, -42 } },     { "foot_r", 9, { 0, 0, -41 } },    { "ball_r", 10, { 0, 12, -5 } },
    { "upperarm_l", 2, { -18, 0, 18 } }, { "lowerarm_l", 12, { 0, 0, -28 } }, { "hand_l", 13, { 0, 0, -26 } },
    { "upperarm_r", 2, { 18, 0, 18 } }, { "lowerarm_r", 15, { 0, 0, -28 } }, { "hand_r", 16, { 0, 0, -26 } },
};
static int wounds_selftest(void) {
    int fails = 0;
    SkelMesh* m = make_skel(q275_biped, (int)(sizeof q275_biped / sizeof q275_biped[0]));
    static PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    static World w0;
    float bones[ANIM_MAX_BONES][3][4];
    IkPhysInfo info;
    const float rest_z = m->bind_world[0][2][3];
    Entity fake = 0x7FFFFF00u;
#define WSTEP(nf)                                                                                                          \
    for (int f = 0; f < (nf); f++) {                                                                                       \
        memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);                                                      \
        phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);                                                                \
    }
#define FRESH()                                                                                                            \
    ikphys_forget(fake), fake++;                                                                                           \
    phys_set_simulate(fake, PHYS_ACTIVE, NULL);                                                                            \
    phys_set_auto_get_up(fake, true, 1.0f);                                                                                \
    WSTEP(120)
    // 1. a wounded calf: it limps
    FRESH();
    float calf[3] = { m->bind_world[5][0][3], m->bind_world[5][1][3] + 6.0f, m->bind_world[5][2][3] - 15.0f };
    phys_wound(fake, "calf_l", calf, 0.6f);
    WSTEP(150);
    ikphys_info(fake, &info);
    float limp = phys_limp(fake);
    printf("[RealEngine] ragdoll_wounds: a wounded calf: limp %.2f, balance %d, pelvis %.1f cm (standing %.1f)\n", limp, info.balance, info.root_z, rest_z);
    if (limp <= 0.0f || info.balance != BAL_BALANCED || info.root_z > rest_z - 1.0f) printf("[RealEngine] FAIL: ragdoll_wounds: the wounded leg should limp standing\n"), fails++;
    // 2. a chest wound: the right hand holds it
    FRESH();
    float chest[3] = { 0.0f, 12.0f, m->bind_world[2][2][3] + 8.0f };
    V3 cw = v3(chest[0], chest[1], chest[2]);
    float d0 = len3(sub3(row_pos(bones[17]), cw));
    phys_wound(fake, "spine_02", chest, 0.5f);
    float dmin = d0;
    bool held = false;
    for (int k = 0; k < 120; k++) {
        WSTEP(1);
        dmin = fminf(dmin, len3(sub3(row_pos(bones[17]), cw)));
        held |= phys_holding_wound(fake);
    }
    ikphys_info(fake, &info);
    printf("[RealEngine] ragdoll_wounds: a chest wound: the right hand from %.0f to %.0f cm of it (held %d), balance %d\n", d0, dmin, held, info.balance);
    if (!held || dmin > fminf(d0 * 0.6f, 30.0f)) printf("[RealEngine] FAIL: ragdoll_wounds: the hand should go to the wound\n"), fails++;
    // 3. a grave wound: it falls, writhes, then gets up (later than a plain fall's 1 s)
    FRESH();
    phys_wound(fake, "spine_02", chest, 0.9f);
    phys_hit_reaction(fake, "spine_02", (float[3]){ 0.0f, -500.0f, 0.0f }, 0.2f, 1.0f);
    int fell_f = -1, up_f = -1, writhe = 0;
    for (int k = 0; k < 900 && up_f < 0; k++) {
        WSTEP(1);
        ikphys_info(fake, &info);
        if (fell_f < 0 && info.balance == BAL_FALLEN) fell_f = k;
        writhe += phys_writhing(fake);
        if (fell_f >= 0 && info.balance >= BAL_GETTING_UP) up_f = k;
    }
    printf("[RealEngine] ragdoll_wounds: a grave wound: fell at %.2f s, writhed %.1f s, getting up at %.2f s\n", fell_f / 60.0f, writhe / 60.0f, up_f / 60.0f);
    if (fell_f < 0 || writhe < 120 || up_f < 0 || up_f - fell_f < 240) printf("[RealEngine] FAIL: ragdoll_wounds: a grave wound should knock it down and writhe before getting up\n"), fails++;
    // 4. a fall from 8 m: the arms flail, they stop when it lands
    FRESH();
    phys_hit_reaction(fake, "spine_02", (float[3]){ 900.0f, 0.0f, 150.0f }, 0.1f, 1.0f);
    WSTEP(90);
    {
        PState* st = state_of(fake, false);
        for (uint32_t i = 0; st && i < pa.nbodies; i++) st->b[i].x.z += 800.0f, st->b[i].v = v3(0, 0, 0), st->b[i].w = v3(0, 0, 0);
        if (st) st->asleep = false, st->still_t = 0.0f;
    }
    int flail = 0, after = 0;
    float hand_sp = 0.0f;
    V3 hp = row_pos(bones[17]);
    for (int k = 0; k < 180; k++) {
        WSTEP(1);
        bool hf = phys_high_fall(fake);
        flail += hf;
        V3 h = row_pos(bones[17]);
        if (hf) hand_sp = fmaxf(hand_sp, len3(sub3(sub3(h, hp), v3(0, 0, -9.8f * 100.0f * (float)k / 3600.0f))) * 60.0f);
        hp = h;
        if (k > 150) after += hf;
    }
    printf("[RealEngine] ragdoll_wounds: an 8 m fall: flailing %.2f s (the hand at up to %.0f cm/s besides the fall), %s after landing\n", flail / 60.0f, hand_sp,
           after ? "still flailing" : "stopped");
    if (flail < 20 || after) printf("[RealEngine] FAIL: ragdoll_wounds: a high fall should flail the arms until it lands\n"), fails++;
    ikphys_forget(fake);
    // 5. the budget: 200 standing in a line 2 m apart, the view at the first: only the 32 nearest simulate
    {
        enum { N = 200 };
        static float row[ANIM_MAX_BONES][3][4];
        ikphys_set_view((float[3]){ 0.0f, -300.0f, 150.0f });
        double ms = 0.0;
        uint32_t dormant = 0;
        for (int f = 0; f < 60; f++) {
            double t0 = re_time_seconds();
            for (int i = 0; i < N; i++) {
                Entity e = 0x7FFFF000u + (Entity)i;
                if (f == 0) phys_set_simulate(e, PHYS_ACTIVE, NULL);
                memcpy(row, m->bind_world, sizeof(float) * 12 * m->nbones);
                for (uint32_t b = 0; b < m->nbones; b++) row[b][0][3] += (float)i * 200.0f;
                phys_apply(&w0, e, &pa, m, row, 1.0f / 60.0f);
            }
            if (f >= 30) ms += (re_time_seconds() - t0) * 1000.0;
            ikphys_frame_end();
            dormant = ikphys_stats()->dormant;
        }
        ikphys_set_view(NULL);
        printf("[RealEngine] ragdoll_wounds: 200 active ragdolls: %u dormant, %u simulated, %.3f ms a frame\n", dormant, N - dormant, ms / 30.0);
        if (dormant != N - ACTIVE_BUDGET) printf("[RealEngine] FAIL: ragdoll_wounds: past the budget they should be dormant\n"), fails++;
        if (ms / 30.0 > 4.0) printf("[RealEngine] FAIL: ragdoll_wounds: %.3f ms > 4 ms\n", ms / 30.0), fails++;
        for (int i = 0; i < N; i++) ikphys_forget(0x7FFFF000u + (Entity)i);
    }
#undef WSTEP
#undef FRESH
    printf("[RealEngine] ragdoll_wounds: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

int ikphys_selftest_named(const char* name) {
    int r = -1;
    if (!strcmp(name, "ragdoll_limits")) r = limits_selftest();
    if (!strcmp(name, "ragdoll_active")) r = ragdoll_active_selftest();
    if (!strcmp(name, "dyn_bones")) r = dyn_selftest();
    if (!strcmp(name, "ragdoll_wounds")) r = wounds_selftest(); // (Q27.5)
    fflush(stdout);
    if (r >= 0) return r;
    printf("[RealEngine] FAIL: --ik-test: no test \"%s\" (ragdoll_limits, ragdoll_active, dyn_bones, ragdoll_wounds)\n", name);
    return 1;
}

// A skeleton of 4 bones in a line along +Z (60 cm each), from scratch
static SkelMesh* test_skeleton(void) {
    static SkelMesh m;
    memset(&m, 0, sizeof m);
    m.nbones = 4;
    for (int b = 0; b < 4; b++) {
        snprintf(m.bones[b].name, ANIM_NAME, "b%d", b);
        m.bones[b].parent = (int16_t)(b - 1);
        memset(&m.bones[b].bind, 0, sizeof m.bones[b].bind);
        m.bones[b].bind.q[3] = 1.0f, m.bones[b].bind.s[0] = m.bones[b].bind.s[1] = m.bones[b].bind.s[2] = 1.0f;
        m.bones[b].bind.t[2] = b ? 60.0f : 100.0f;
    }
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    AnimXf loc[4];
    for (int b = 0; b < 4; b++) loc[b] = m.bones[b].bind;
    anim_world(&m, loc, id, m.bind_world);
    return &m;
}

int ikphys_selftest(void) {
    int fails = 0;
    SkelMesh* m = test_skeleton();
    // IK: a 3-bone chain (b0 .. b3's head) reaches a point within its length, the bones keep their lengths
    IkRig rig;
    memset(&rig, 0, sizeof rig);
    rig.ngoals = 1, rig.nsolvers = 1;
    snprintf(rig.goals[0].name, ANIM_NAME, "Tip"), snprintf(rig.goals[0].bone, ANIM_NAME, "b3");
    rig.solvers[0].kind = IKS_CHAIN, rig.solvers[0].on = 1, rig.solvers[0].iterations = 20;
    snprintf(rig.solvers[0].start, ANIM_NAME, "b0"), snprintf(rig.solvers[0].goal, ANIM_NAME, "Tip");
    float bones[ANIM_MAX_BONES][3][4];
    memcpy(bones, m->bind_world, sizeof(float) * 12 * 4);
    Entity fake = 0x7FFFFFF0u;
    float target[3] = { 90, 30, 160 };
    ikrig_set_goal(fake, "Tip", target, NULL, 1.0f);
    static World w0; // (empty: no actors, no boxes; the ground under the feet at 0)
    ikrig_apply(&w0, fake, &rig, m, bones);
    V3 tip = row_pos(bones[3]);
    float err = len3(sub3(tip, v3(target[0], target[1], target[2])));
    float l1 = len3(sub3(row_pos(bones[1]), row_pos(bones[0]))), l2 = len3(sub3(row_pos(bones[2]), row_pos(bones[1])));
    printf("[RealEngine] ik: chain of 3 to (90, 30, 160): off by %.2f cm, lengths %.2f %.2f\n", err, l1, l2);
    if (err > 0.5f || fabsf(l1 - 60.0f) > 0.05f || fabsf(l2 - 60.0f) > 0.05f) printf("[RealEngine] FAIL: ik: the chain\n"), fails++;
    // a cone limit holds: b1 within 20 deg of its posed direction
    memcpy(bones, m->bind_world, sizeof(float) * 12 * 4);
    rig.nbones = 1;
    snprintf(rig.bones[0].bone, ANIM_NAME, "b0"), rig.bones[0].limit = 20.0f;
    ikrig_apply(&w0, fake, &rig, m, bones);
    V3 d1 = norm3(sub3(row_pos(bones[1]), row_pos(bones[0])));
    float a1 = acosf(fminf(d1.z, 1.0f)) * 57.29578f;
    printf("[RealEngine] ik: with a 20 deg limit on b0: it turned %.1f deg\n", a1);
    if (a1 > 20.5f) printf("[RealEngine] FAIL: ik: the limit\n"), fails++;
    ikrig_clear_goal(fake, NULL);
    // physics: the 4 bones as capsules, ragdolled from 100 cm up: they fall, rest on the ground, joints together
    PhysicsAsset pa;
    memset(&pa, 0, sizeof pa);
    phys_autogen(&pa, m);
    printf("[RealEngine] phys: %u bodies, %u constraints from the test skeleton\n", pa.nbodies, pa.nconstraints);
    if (pa.nbodies < 3 || pa.nconstraints < 2) printf("[RealEngine] FAIL: phys: the bodies\n"), fails++;
    phys_set_simulate(fake, 1, NULL);
    phys_add_impulse(fake, "b2", (float[3]){ 300.0f, 0.0f, 0.0f }); // (a push at the top: upright, it would balance)
    memcpy(bones, m->bind_world, sizeof(float) * 12 * 4);
    float vmax = 0.0f;
    for (int f = 0; f < 240; f++) { // 4 s at 60 fps
        memcpy(bones, m->bind_world, sizeof(float) * 12 * 4);
        phys_apply(&w0, fake, &pa, m, bones, 1.0f / 60.0f);
        PState* st = state_of(fake, false);
        for (uint32_t i = 0; st && i < pa.nbodies; i++) vmax = fmaxf(vmax, len3(st->b[i].v));
    }
    float zmin = 1e30f, zmax = -1e30f, gap = 0.0f;
    for (int b = 0; b < 4; b++) zmin = fminf(zmin, bones[b][2][3]), zmax = fmaxf(zmax, bones[b][2][3]);
    for (int b = 1; b < 4; b++) { // each head within reach of its parent's (the lengths kept)
        float l = len3(sub3(row_pos(bones[b]), row_pos(bones[b - 1])));
        gap = fmaxf(gap, fabsf(l - 60.0f));
    }
    PState* st = state_of(fake, false);
    float over = 0.0f; // the joints' swings past their limits
    for (uint32_t c = 0; st && c < pa.nconstraints; c++) {
        const BodyState *bp = &st->b[pa.constraints[c].parent], *bc = &st->b[pa.constraints[c].child];
        M3 rp = q_m3(bp->q), rc = q_m3(bc->q);
        const float* ax = pa.bodies[pa.constraints[c].child].axis;
        V3 a = m3v(&rc, norm3(v3(ax[0], ax[1], ax[2]))), r = m3v(&rp, bc->ref_axis);
        over = fmaxf(over, acosf(fmaxf(fminf(dot3(a, r), 1.0f), -1.0f)) * 57.29578f - pa.constraints[c].swing);
    }
    printf("[RealEngine] phys: the swings within their limits (past by %.1f deg at most)\n", fmaxf(over, 0.0f));
    if (over > 5.0f) printf("[RealEngine] FAIL: phys: a swing limit\n"), fails++;
    printf("[RealEngine] phys: after 4 s: heads between %.1f and %.1f cm, joints off by %.2f cm, fastest %.0f cm/s, asleep %d\n", zmin, zmax,
           gap, vmax, st ? st->asleep : -1);
    if (zmin < -20.0f || zmax > 60.0f || gap > 3.0f || vmax > 2000.0f) printf("[RealEngine] FAIL: phys: the ragdoll\n"), fails++;
    ikphys_forget(fake);
    fails += active_selftest();
    printf("[RealEngine] ik/phys self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
