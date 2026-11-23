// --- FBX (binary 7.x or ASCII; fbx_read.c parses it), included by tlend_import.c: the same scene structure as a .tlend
// for the static meshes, the same Skeletal Mesh writer as glTF for the skinned ones (import_skin.inc). The objects and
// connections, the property templates and the Maya transform order (T Roff Rp Rpre R Rpost^-1 Rp^-1 Soff Sp S Sp^-1;
// the geometric transform not inherited) are the Tlender's io_fbx_import.c, copied and adapted. The file's axes
// (GlobalSettings Up / Coord) and unit (UnitScaleFactor, cm per unit) become Blender's Z up and meters, then the Engine's.
// - Static meshes: a Static Mesh per material slot (the model's materials in connection order), vertices split where the
//   file's normals differ (hard edges stay hard).
// - Skins: the clusters' bones (and the LimbNodes between them and their root) become a Skeletal Mesh per skeleton
//   root; rest = TransformLink (else derived from the parent's rest and the rest pose); the faces go to the bone most of
//   their corners lean on; a mesh model under a bone becomes a rigid part of it.
// - Animation: each AnimationStack (its first layer) baked at 30 fps through the transform order, the bones' worlds.
// - Materials (Phong / Lambert) as Principled BSDF graphs: DiffuseColor and its texture, Shininess as roughness,
//   Opacity / TransparencyFactor, ReflectionFactor as metallic, the normal map, emission; embedded textures (Video
//   Content) written to <folder>/Textures. ---

#include "fbx_read.h"

#define FX_KTIME_V7 46186158000ll
#define FX_KTIME_V8 141120000ll
#define FX_ANIM_FPS 30.0f
#define FX_DEG 0.017453292519943295f

typedef struct FxObj {
    const FbxNode* n;
    int64_t uid;
    const char* kind; // node name: Model, Geometry, Material...
    const char* sub;  // the third property: Mesh, LimbNode, Null, Cluster... ("" if none)
    char name[64];
} FxObj;
typedef struct FxConn {
    int src, dst;     // object indices; -2: the root (uid 0); -1: unknown
    bool op;          // OP (to a property) or OO
    const char* prop; // OP: the property name
} FxConn;
typedef struct FxTemplate {
    const char* type, *cls;
    const FbxNode* props;
} FxTemplate;
typedef struct FxProps {
    const FbxNode* own, *tmpl;
} FxProps;
typedef struct FxXform {
    float loc[3], rot[3], sca[3], geom_loc[3], geom_rot[3], geom_sca[3];
    float rot_ofs[3], rot_piv[3], sca_ofs[3], sca_piv[3], pre_rot[3], pst_rot[3];
    int order[3];
} FxXform;

typedef struct Fx {
    FbxDoc doc;
    FxObj* objs;
    int nobj, *hash, hash_cap;
    FxConn* conns;
    int nconn, *out_start, *out_list, *in_start, *in_list;
    FxTemplate* tmpls;
    int ntmpl;
    char dir[900], base[200], tex_dir[1000];
    float ax[3][3]; // file -> Blender: rows = Blender's x, y, z axes in the file's coordinates
    float unit;     // meters per file unit
    int64_t ktime;  // ticks per second
    int* parent;    // per object: its parent Model (-1)
    FxXform* xf;    // per Model: its rest transform
    int* mat_of;    // per object: its index among the materials (-1)
} Fx;

static uint32_t fx_uid_hash(int64_t uid) { return (uint32_t)(((uint64_t)uid * 0x9E3779B97F4A7C15ull) >> 32); }
static int fx_find(const Fx* c, int64_t uid) {
    if (!c->hash_cap) return -1;
    uint32_t mask = (uint32_t)c->hash_cap - 1;
    for (uint32_t h = fx_uid_hash(uid) & mask;; h = (h + 1) & mask) {
        int i = c->hash[h];
        if (i < 0) return -1;
        if (c->objs[i].uid == uid) return i;
    }
}
static bool fx_index_objects(Fx* c, const FbxNode* objects) {
    int n = 0;
    for (const FbxNode* o = objects ? objects->child : NULL; o; o = o->next) n++;
    c->objs = calloc((size_t)n + 1, sizeof *c->objs);
    c->hash_cap = 16;
    while (c->hash_cap < n * 2) c->hash_cap *= 2;
    c->hash = malloc(sizeof *c->hash * (size_t)c->hash_cap);
    if (!c->objs || !c->hash) return false;
    for (int i = 0; i < c->hash_cap; i++) c->hash[i] = -1;
    uint32_t mask = (uint32_t)c->hash_cap - 1;
    for (const FbxNode* o = objects ? objects->child : NULL; o; o = o->next) {
        const FbxProp* id = fbx_prop(o, 0);
        if (!id || (id->type != 'L' && id->type != 'I')) continue;
        FxObj* x = &c->objs[c->nobj];
        x->n = o, x->uid = id->i, x->kind = o->name, x->sub = fbx_prop_str(fbx_prop(o, 2));
        fbx_name_class(fbx_prop(o, 1), x->name, sizeof x->name, NULL, 0);
        if (fx_find(c, x->uid) >= 0) continue; // (a repeated id: the first one wins)
        uint32_t h = fx_uid_hash(x->uid) & mask;
        while (c->hash[h] >= 0) h = (h + 1) & mask;
        c->hash[h] = c->nobj++;
    }
    return true;
}
static bool fx_index_connections(Fx* c, const FbxNode* conns) {
    int n = 0;
    for (const FbxNode* k = conns ? conns->child : NULL; k; k = k->next) n++;
    c->conns = calloc((size_t)n + 1, sizeof *c->conns);
    c->out_start = calloc((size_t)c->nobj + 2, sizeof(int));
    c->in_start = calloc((size_t)c->nobj + 2, sizeof(int));
    c->out_list = malloc(sizeof(int) * ((size_t)n + 1));
    c->in_list = malloc(sizeof(int) * ((size_t)n + 1));
    if (!c->conns || !c->out_start || !c->in_start || !c->out_list || !c->in_list) return false;
    for (const FbxNode* k = conns ? conns->child : NULL; k; k = k->next) {
        if (strcmp(k->name, "C") && strcmp(k->name, "Connect")) continue;
        const char* type = fbx_prop_str(fbx_prop(k, 0));
        const FbxProp *a = fbx_prop(k, 1), *b = fbx_prop(k, 2);
        if (!a || !b || (a->type != 'L' && a->type != 'I') || (b->type != 'L' && b->type != 'I')) continue;
        FxConn* x = &c->conns[c->nconn];
        x->src = a->i == 0 ? -2 : fx_find(c, a->i);
        x->dst = b->i == 0 ? -2 : fx_find(c, b->i);
        x->op = !strcmp(type, "OP");
        x->prop = x->op ? fbx_prop_str(fbx_prop(k, 3)) : "";
        if (!x->op && strcmp(type, "OO")) continue;
        c->nconn++;
    }
    for (int i = 0; i < c->nconn; i++) { // (per object, its connections in file order)
        if (c->conns[i].src >= 0) c->out_start[c->conns[i].src + 1]++;
        if (c->conns[i].dst >= 0) c->in_start[c->conns[i].dst + 1]++;
    }
    for (int i = 0; i < c->nobj; i++) c->out_start[i + 1] += c->out_start[i], c->in_start[i + 1] += c->in_start[i];
    int* fo = calloc((size_t)c->nobj + 1, sizeof(int));
    int* fi = calloc((size_t)c->nobj + 1, sizeof(int));
    if (!fo || !fi) {
        free(fo), free(fi);
        return false;
    }
    for (int i = 0; i < c->nconn; i++) {
        int s = c->conns[i].src, d = c->conns[i].dst;
        if (s >= 0) c->out_list[c->out_start[s] + fo[s]++] = i;
        if (d >= 0) c->in_list[c->in_start[d] + fi[d]++] = i;
    }
    free(fo), free(fi);
    return true;
}
#define FX_OUT(c, o, k) for (int k##_i = (c)->out_start[o], k; k##_i < (c)->out_start[(o) + 1] && ((k = (c)->out_list[k##_i]), 1); k##_i++)
#define FX_IN(c, o, k) for (int k##_i = (c)->in_start[o], k; k##_i < (c)->in_start[(o) + 1] && ((k = (c)->in_list[k##_i]), 1); k##_i++)

static const char* fx_strip_k(const char* s) { return s[0] == 'K' && s[1] == 'F' && s[2] == 'b' && s[3] == 'x' ? s + 1 : s; }
static void fx_read_templates(Fx* c) {
    const FbxNode* defs = fbx_find(&c->doc.root, "Definitions");
    int n = 0;
    for (const FbxNode* t = defs ? defs->child : NULL; t; t = t->next)
        if (!strcmp(t->name, "ObjectType"))
            for (const FbxNode* p = t->child; p; p = p->next) n += !strcmp(p->name, "PropertyTemplate");
    c->tmpls = calloc((size_t)n + 1, sizeof *c->tmpls);
    for (const FbxNode* t = defs ? defs->child : NULL; c->tmpls && t; t = t->next) {
        if (strcmp(t->name, "ObjectType")) continue;
        for (const FbxNode* p = t->child; p; p = p->next) {
            if (strcmp(p->name, "PropertyTemplate")) continue;
            FxTemplate* x = &c->tmpls[c->ntmpl++];
            x->type = fbx_prop_str(fbx_prop(t, 0));
            x->cls = fx_strip_k(fbx_prop_str(fbx_prop(p, 0)));
            x->props = fbx_find(p, "Properties70");
        }
    }
}
static FxProps fx_props(const Fx* c, const FbxNode* n, const char* type, const char* cls) {
    FxProps p = { fbx_find(n, "Properties70"), NULL };
    for (int i = 0; i < c->ntmpl; i++)
        if (!strcmp(c->tmpls[i].type, type) && !strcmp(c->tmpls[i].cls, fx_strip_k(cls))) p.tmpl = c->tmpls[i].props;
    return p;
}
// The P entry of a property (the object's own first, then its template's; user properties 'U' are not read)
static const FbxNode* fx_p_find(FxProps p, const char* name) {
    for (int k = 0; k < 2; k++)
        for (const FbxNode* e = (k ? p.tmpl : p.own) ? (k ? p.tmpl : p.own)->child : NULL; e; e = e->next) {
            if (e->nprops < 5 || strcmp(fbx_prop_str(&e->props[0]), name)) continue;
            if (strchr(fbx_prop_str(&e->props[3]), 'U')) continue;
            return e;
        }
    return NULL;
}
static double fx_p_num(FxProps p, const char* name, double def) { return fbx_prop_num(fbx_prop(fx_p_find(p, name), 4), def); }
static int64_t fx_p_int(FxProps p, const char* name, int64_t def) { return fbx_prop_int(fbx_prop(fx_p_find(p, name), 4), def); }
static void fx_p_vec3(FxProps p, const char* name, float* out, float dx, float dy, float dz) {
    const FbxNode* e = fx_p_find(p, name);
    out[0] = (float)fbx_prop_num(fbx_prop(e, 4), dx), out[1] = (float)fbx_prop_num(fbx_prop(e, 5), dy), out[2] = (float)fbx_prop_num(fbx_prop(e, 6), dz);
}

// --- Transforms ---

static void fx_read_xform(FxProps p, FxXform* x) {
    fx_p_vec3(p, "Lcl Translation", x->loc, 0, 0, 0);
    fx_p_vec3(p, "Lcl Rotation", x->rot, 0, 0, 0);
    fx_p_vec3(p, "Lcl Scaling", x->sca, 1, 1, 1);
    fx_p_vec3(p, "GeometricTranslation", x->geom_loc, 0, 0, 0);
    fx_p_vec3(p, "GeometricRotation", x->geom_rot, 0, 0, 0);
    fx_p_vec3(p, "GeometricScaling", x->geom_sca, 1, 1, 1);
    fx_p_vec3(p, "RotationOffset", x->rot_ofs, 0, 0, 0);
    fx_p_vec3(p, "RotationPivot", x->rot_piv, 0, 0, 0);
    fx_p_vec3(p, "ScalingOffset", x->sca_ofs, 0, 0, 0);
    fx_p_vec3(p, "ScalingPivot", x->sca_piv, 0, 0, 0);
    static const int orders[7][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 2, 0 }, { 1, 0, 2 }, { 2, 0, 1 }, { 2, 1, 0 }, { 0, 1, 2 } };
    int ord = 0;
    memset(x->pre_rot, 0, sizeof x->pre_rot), memset(x->pst_rot, 0, sizeof x->pst_rot);
    if (fx_p_int(p, "RotationActive", 0)) {
        fx_p_vec3(p, "PreRotation", x->pre_rot, 0, 0, 0);
        fx_p_vec3(p, "PostRotation", x->pst_rot, 0, 0, 0);
        ord = (int)fx_p_int(p, "RotationOrder", 0);
        if (ord < 0 || ord > 6) ord = 0;
    }
    memcpy(x->order, orders[ord], sizeof x->order);
}
static M4 fx_translate(float x, float y, float z) {
    M4 r = m4_id();
    r.m[12] = x, r.m[13] = y, r.m[14] = z;
    return r;
}
static M4 fx_rot_axis(int axis, float a) {
    float c = cosf(a), s = sinf(a);
    M4 r = m4_id();
    if (axis == 0) r.m[5] = c, r.m[6] = s, r.m[9] = -s, r.m[10] = c;
    else if (axis == 1) r.m[0] = c, r.m[2] = -s, r.m[8] = s, r.m[10] = c;
    else r.m[0] = c, r.m[1] = s, r.m[4] = -s, r.m[5] = c;
    return r;
}
// Degrees about order[0] first, then order[1], then order[2]
static M4 fx_euler(const int order[3], const float deg[3]) {
    M4 r = m4_id();
    for (int k = 0; k < 3; k++) {
        M4 a = fx_rot_axis(order[k], deg[order[k]] * FX_DEG);
        r = m4_mul(&a, &r);
    }
    return r;
}
static M4 fx_mul3(M4 a, M4 b) { return m4_mul(&a, &b); }
// base (what the children see) and geom (the node's own: base * geom)
static void fx_matrices(const FxXform* x, M4* base, M4* geom) {
    static const int XYZ[3] = { 0, 1, 2 };
    M4 m = fx_translate(x->loc[0], x->loc[1], x->loc[2]);
    m = fx_mul3(m, fx_translate(x->rot_ofs[0], x->rot_ofs[1], x->rot_ofs[2]));
    m = fx_mul3(m, fx_translate(x->rot_piv[0], x->rot_piv[1], x->rot_piv[2]));
    m = fx_mul3(m, fx_euler(XYZ, x->pre_rot));
    m = fx_mul3(m, fx_euler(x->order, x->rot));
    M4 pst = fx_euler(XYZ, x->pst_rot);
    m = fx_mul3(m, m4_inv(&pst));
    m = fx_mul3(m, fx_translate(-x->rot_piv[0], -x->rot_piv[1], -x->rot_piv[2]));
    m = fx_mul3(m, fx_translate(x->sca_ofs[0], x->sca_ofs[1], x->sca_ofs[2]));
    m = fx_mul3(m, fx_translate(x->sca_piv[0], x->sca_piv[1], x->sca_piv[2]));
    M4 s = m4_id();
    s.m[0] = x->sca[0], s.m[5] = x->sca[1], s.m[10] = x->sca[2];
    m = fx_mul3(m, s);
    *base = fx_mul3(m, fx_translate(-x->sca_piv[0], -x->sca_piv[1], -x->sca_piv[2]));
    M4 g = fx_mul3(fx_translate(x->geom_loc[0], x->geom_loc[1], x->geom_loc[2]), fx_euler(XYZ, x->geom_rot));
    M4 gs = m4_id();
    gs.m[0] = x->geom_sca[0], gs.m[5] = x->geom_sca[1], gs.m[10] = x->geom_sca[2];
    *geom = fx_mul3(g, gs);
}
// A Model's world (its base, the parents' bases; memo per pose)
static M4 fx_world(const Fx* c, const FxXform* xf, M4* memo, uint8_t* done, int o, int depth) {
    if (done[o]) return memo[o];
    M4 base, geom;
    fx_matrices(&xf[o], &base, &geom);
    if (c->parent[o] >= 0 && depth < 256) {
        M4 p = fx_world(c, xf, memo, done, c->parent[o], depth + 1);
        base = m4_mul(&p, &base);
    }
    memo[o] = base, done[o] = 1;
    return base;
}
// The file's space -> Blender's (axes, meters): A M A^T, the translation times the unit
static M4 fx_to_blender(const Fx* c, const M4* m) {
    M4 r = m4_id();
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) {
            float s = 0.0f;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) s += c->ax[row][i] * m->m[j * 4 + i] * c->ax[col][j];
            r.m[col * 4 + row] = s;
        }
    for (int k = 0; k < 3; k++) r.m[12 + k] = (c->ax[k][0] * m->m[12] + c->ax[k][1] * m->m[13] + c->ax[k][2] * m->m[14]) * c->unit;
    return r;
}
static M4 fx_to_engine_m(const Fx* c, const M4* m) {
    M4 b = fx_to_blender(c, m);
    return m4_to_engine(&b);
}
static void fx_to_blender_p(const Fx* c, const float* p, float* o) {
    for (int k = 0; k < 3; k++) o[k] = (c->ax[k][0] * p[0] + c->ax[k][1] * p[1] + c->ax[k][2] * p[2]) * c->unit;
}
static void fx_to_engine_p(const Fx* c, const float* p, float* o) {
    float b[3];
    fx_to_blender_p(c, p, b);
    point_to_engine(b, o);
}
static void fx_to_engine_n(const Fx* c, const float* n, float* o) {
    float b[3];
    for (int k = 0; k < 3; k++) b[k] = c->ax[k][0] * n[0] + c->ax[k][1] * n[1] + c->ax[k][2] * n[2];
    o[0] = b[0], o[1] = -b[1], o[2] = b[2];
}

// --- Geometry ---

typedef struct FxLayer {
    const char *mapping, *ref;
} FxLayer;
static FxLayer fx_layer(const FbxNode* n) {
    FxLayer l = { fbx_child_str(n, "MappingInformationType"), fbx_child_str(n, "ReferenceInformationType") };
    return l;
}
// The value index of corner `loop` (polygon `poly`, control point `vert`) in a layer (-1: none)
static long fx_layer_index(const FxLayer* l, const int32_t* index, size_t nindex, size_t loop, size_t poly, size_t vert) {
    size_t at;
    if (!strcmp(l->mapping, "ByPolygonVertex")) at = loop;
    else if (!strcmp(l->mapping, "ByPolygon")) at = poly;
    else if (!strcmp(l->mapping, "ByVertice") || !strcmp(l->mapping, "ByVertex")) at = vert;
    else if (!strcmp(l->mapping, "AllSame")) at = 0;
    else return -1;
    if (index && strcmp(l->ref, "Direct")) return at < nindex ? index[at] : -1;
    return (long)at;
}

typedef struct FxGeom {
    double *cp, *uv, *nrm;
    int32_t *pvi, *uvi, *mats, *nrmi;
    size_t ncp, npvi, nuv, nuvi, nmat, nnrm, nnrmi;
    FxLayer uvl, matl, nrml;
} FxGeom;
static void fx_geom_free(FxGeom* g) { free(g->cp), free(g->uv), free(g->nrm), free(g->pvi), free(g->uvi), free(g->mats), free(g->nrmi); }
static bool fx_geom_read(const FbxNode* gn, FxGeom* g) {
    memset(g, 0, sizeof *g);
    const FbxNode* uv_n = fbx_find(gn, "LayerElementUV");
    const FbxNode* mat_n = fbx_find(gn, "LayerElementMaterial");
    const FbxNode* nrm_n = fbx_find(gn, "LayerElementNormal");
    g->uvl = fx_layer(uv_n), g->matl = fx_layer(mat_n), g->nrml = fx_layer(nrm_n);
    g->cp = fbx_array_f64(fbx_prop(fbx_find(gn, "Vertices"), 0), &g->ncp);
    g->pvi = fbx_array_i32(fbx_prop(fbx_find(gn, "PolygonVertexIndex"), 0), &g->npvi);
    if (uv_n) g->uv = fbx_array_f64(fbx_prop(fbx_find(uv_n, "UV"), 0), &g->nuv), g->uvi = fbx_array_i32(fbx_prop(fbx_find(uv_n, "UVIndex"), 0), &g->nuvi);
    if (mat_n) g->mats = fbx_array_i32(fbx_prop(fbx_find(mat_n, "Materials"), 0), &g->nmat);
    if (nrm_n)
        g->nrm = fbx_array_f64(fbx_prop(fbx_find(nrm_n, "Normals"), 0), &g->nnrm),
        g->nrmi = fbx_array_i32(fbx_prop(fbx_find(nrm_n, "NormalsIndex"), 0), &g->nnrmi);
    g->ncp /= 3;
    return g->cp && g->pvi && g->ncp && g->npvi;
}
// A corner's UV, normal (file space, (0,0,0) when none) and material slot
static void fx_corner(const FxGeom* g, size_t loop, size_t poly, size_t cp, float uv[2], float n[3], int* slot) {
    long at = g->uv ? fx_layer_index(&g->uvl, g->uvi, g->nuvi, loop, poly, cp) : -1;
    uv[0] = at >= 0 && (size_t)at * 2 + 1 < g->nuv ? (float)g->uv[at * 2] : 0.0f;
    uv[1] = at >= 0 && (size_t)at * 2 + 1 < g->nuv ? (float)g->uv[at * 2 + 1] : 0.0f;
    at = g->nrm ? fx_layer_index(&g->nrml, g->nrmi, g->nnrmi, loop, poly, cp) : -1;
    for (int k = 0; k < 3; k++) n[k] = at >= 0 && (size_t)at * 3 + 2 < g->nnrm ? (float)g->nrm[at * 3 + k] : 0.0f;
    if (slot) {
        at = g->mats ? fx_layer_index(&g->matl, NULL, 0, loop, poly, cp) : -1;
        *slot = at >= 0 && (size_t)at < g->nmat ? g->mats[at] : 0;
        if (*slot < 0) *slot = 0;
    }
}

// A geometry into a TMesh in Blender's space (the control points times the unit; a vertex per control point and
// distinct normal); false when it has no faces
static bool fx_tmesh(const Fx* c, const FxGeom* g, int nslots, TMesh* me) {
    memset(me, 0, sizeof *me);
    size_t ncorner = g->npvi;
    // (the vertices: open addressing on (control point, quantized normal))
    size_t cap = 64;
    while (cap < ncorner * 2) cap *= 2;
    struct FxKey { int32_t cp; int16_t q[3]; int32_t v; }* keys = malloc(sizeof(struct FxKey) * cap);
    me->co = malloc(sizeof(float) * 3 * (ncorner + 1));
    me->loops = malloc(sizeof(uint32_t) * (ncorner + 1));
    me->uv = malloc(sizeof(float) * 2 * (ncorner + 1));
    me->fstart = malloc(sizeof(uint32_t) * (ncorner + 1));
    me->fsize = malloc(sizeof(uint32_t) * (ncorner + 1));
    me->fmat = malloc(sizeof(uint16_t) * (ncorner + 1));
    bool ok = keys && me->co && me->loops && me->uv && me->fstart && me->fsize && me->fmat;
    for (size_t i = 0; ok && i < cap; i++) keys[i].cp = -1;
    size_t start = 0, poly = 0;
    for (size_t i = 0; ok && i < g->npvi; i++) {
        if (g->pvi[i] >= 0) continue;
        size_t n = i + 1 - start;
        bool good = n >= 3;
        for (size_t k = 0; k < n && good; k++) {
            int32_t v = g->pvi[start + k];
            good = (size_t)(v < 0 ? ~v : v) < g->ncp;
        }
        if (good) {
            int slot = 0;
            me->fstart[me->nf] = me->ncorners, me->fsize[me->nf] = (uint32_t)n;
            for (size_t k = 0; k < n; k++) {
                int32_t cp = g->pvi[start + k];
                if (cp < 0) cp = ~cp;
                float uv[2], nf[3];
                fx_corner(g, start + k, poly, (size_t)cp, uv, nf, k == 0 ? &slot : NULL);
                int16_t q[3] = { (int16_t)(nf[0] * 1000.0f), (int16_t)(nf[1] * 1000.0f), (int16_t)(nf[2] * 1000.0f) };
                uint32_t h = ((uint32_t)cp * 2654435761u ^ (uint32_t)(int32_t)q[0] * 73856093u ^ (uint32_t)(int32_t)q[1] * 19349663u ^ (uint32_t)(int32_t)q[2] * 83492791u) & (uint32_t)(cap - 1);
                while (keys[h].cp >= 0 && !(keys[h].cp == cp && !memcmp(keys[h].q, q, sizeof q))) h = (h + 1) & (uint32_t)(cap - 1);
                if (keys[h].cp < 0) {
                    keys[h].cp = cp, memcpy(keys[h].q, q, sizeof q), keys[h].v = (int32_t)me->nv;
                    float p[3] = { (float)g->cp[cp * 3], (float)g->cp[cp * 3 + 1], (float)g->cp[cp * 3 + 2] };
                    fx_to_blender_p(c, p, &me->co[me->nv * 3]);
                    me->nv++;
                }
                me->loops[me->ncorners] = (uint32_t)keys[h].v;
                me->uv[me->ncorners * 2] = uv[0], me->uv[me->ncorners * 2 + 1] = uv[1];
                me->ncorners++;
            }
            me->fmat[me->nf] = (uint16_t)(slot < nslots ? slot : 0);
            me->nf++;
        }
        start = i + 1, poly++;
    }
    free(keys);
    if (!ok || !me->nf) {
        free(me->co), free(me->loops), free(me->uv), free(me->fstart), free(me->fsize), free(me->fmat);
        memset(me, 0, sizeof *me);
        return false;
    }
    return true;
}

// --- Materials ---

// A Texture's file: an embedded Video's content (written to <folder>/Textures), RelativeFilename next to the file,
// FileName, the same name next to the file or in <file>.fbm
static void fx_texture_path(Fx* c, int ti, char* out, size_t cap) {
    const FbxNode* t = c->objs[ti].n;
    char path[1100] = "";
    const char* rel = fbx_child_str(t, "RelativeFilename");
    const char* abs = fbx_child_str(t, "FileName");
    if (!abs[0]) abs = fbx_child_str(t, "Filename");
    if (rel[0] && rel[1] == ':') snprintf(path, sizeof path, "%s", rel); // (#164: on another drive Blender writes it whole)
    else if (rel[0]) {
        while (*rel == '/' || *rel == '\\') rel++;
        snprintf(path, sizeof path, "%s/%s", c->dir, rel);
        if (abs[0] && !re_file_stat(path, NULL, NULL, NULL) && re_file_stat(abs, NULL, NULL, NULL)) snprintf(path, sizeof path, "%s", abs);
    } else snprintf(path, sizeof path, "%s", abs[0] ? abs : c->objs[ti].name);
    const char* bn = path + strlen(path);
    while (bn > path && bn[-1] != '/' && bn[-1] != '\\') bn--;
    FX_IN(c, ti, k) { // (the embedded data: a Video connected to the texture)
        int v = c->conns[k].src;
        if (v < 0 || strcmp(c->objs[v].kind, "Video")) continue;
        const FbxProp* content = fbx_prop(fbx_find(c->objs[v].n, "Content"), 0);
        if (!content || content->type != 'R' || !content->count) continue;
        char name[200];
        snprintf(name, sizeof name, "%s", bn[0] ? bn : "texture.png");
        re_make_dirs(c->tex_dir);
        snprintf(out, cap, "%s/%s", c->tex_dir, name);
        FILE* f = re_fopen(out, "wb");
        bool ok = f && fwrite(content->p, 1, content->count, f) == content->count;
        if (f) ok = fclose(f) == 0 && ok;
        if (ok) return;
    }
    snprintf(out, cap, "%s", path);
    if (re_file_stat(out, NULL, NULL, NULL)) return;
    snprintf(out, cap, "%s/%s", c->dir, bn);
    if (re_file_stat(out, NULL, NULL, NULL)) return;
    snprintf(out, cap, "%s/%s.fbm/%s", c->dir, c->base, bn);
    if (re_file_stat(out, NULL, NULL, NULL)) return;
    snprintf(out, cap, "%s", path); // (missing: left where it was)
}

static int fx_tex_node(TMat* m, const char* path, bool color, float x, float y) {
    int n = gm_node(m, "ShaderNodeTexImage", x, y);
    if (n < 0) return -1;
    snprintf(m->nodes[n].image, sizeof m->nodes[n].image, "%s", path);
    m->nodes[n].prop[2] = color ? IMAGE_COLORSPACE_SRGB : IMAGE_COLORSPACE_NONCOLOR;
    return n;
}

static void fx_material(Fx* c, int mo, TMat* m) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", c->objs[mo].name[0] ? c->objs[mo].name : "Material");
    m->nodes = calloc(16, sizeof(TNode));
    if (!m->nodes) return;
    int out = gm_node(m, "ShaderNodeOutputMaterial", 400, 0), p = gm_node(m, "ShaderNodeBsdfPrincipled", 100, 0);
    gm_link(m, p, 0, out, 0);
    FxProps pr = fx_props(c, c->objs[mo].n, "Material", "FbxSurfacePhong");
    FxProps own = { pr.own, NULL };
    TNode* b = &m->nodes[p];
    float col[3], em[3];
    fx_p_vec3(pr, "DiffuseColor", col, 0.8f, 0.8f, 0.8f);
    memcpy(b->in[0], col, 12);
    double shin = fx_p_num(pr, "Shininess", 20.0);
    b->in[2][0] = (float)fmin(fmax(1.0 - sqrt(fmax(shin, 0.0)) / 10.0, 0.0), 1.0); // (Blender's importer)
    double alpha = 1.0 - fx_p_num(pr, "TransparencyFactor", 0.0);
    if (alpha == 1.0 || alpha == 0.0) alpha = fx_p_find(own, "Opacity") ? fx_p_num(own, "Opacity", 1.0) : 1.0;
    b->in[4][0] = (float)alpha;
    b->in[1][0] = (float)fmin(fmax(fx_p_num(pr, "ReflectionFactor", 0.0), 0.0), 1.0);
    fx_p_vec3(pr, "EmissiveColor", em, 0, 0, 0);
    float es = (float)fx_p_num(pr, "EmissiveFactor", 1.0);
    if (em[0] + em[1] + em[2] > 0.0f) memcpy(b->in[26], em, 12), b->in[27][0] = es;
    // the textures connected to its properties
    FX_IN(c, mo, k) {
        const FxConn* cn = &c->conns[k];
        if (cn->src < 0 || !cn->op || strcmp(c->objs[cn->src].kind, "Texture")) continue;
        const char* t = cn->prop;
        char path[1100];
        fx_texture_path(c, cn->src, path, sizeof path);
        if (!strcmp(t, "DiffuseColor") || !strcmp(t, "3dsMax|maps|texmap_diffuse")) {
            int n = fx_tex_node(m, path, true, -400, 200);
            gm_link(m, n, 0, p, 0);
        } else if (!strcmp(t, "NormalMap") || !strcmp(t, "Bump") || !strcmp(t, "3dsMax|maps|texmap_bump")) {
            int n = fx_tex_node(m, path, false, -600, -300), nm = gm_node(m, "ShaderNodeNormalMap", -250, -300);
            if (nm >= 0) m->nodes[nm].in[0][0] = (float)fx_p_num(pr, "BumpFactor", 1.0);
            gm_link(m, n, 0, nm, 1), gm_link(m, nm, 0, p, 5);
        } else if (!strcmp(t, "EmissiveColor")) {
            int n = fx_tex_node(m, path, true, -400, -550);
            gm_link(m, n, 0, p, 26);
            m->nodes[p].in[27][0] = es;
        } else if (!strcmp(t, "ShininessExponent")) { // (gloss: rough = 1 - it)
            int n = fx_tex_node(m, path, false, -600, -50), inv = gm_node(m, "ShaderNodeInvert", -300, -50);
            gm_link(m, n, 0, inv, 1), gm_link(m, inv, 0, p, 2);
        } else if (!strcmp(t, "ReflectionFactor") || !strcmp(t, "ReflectionColor")) {
            int n = fx_tex_node(m, path, false, -400, 50);
            gm_link(m, n, 0, p, 1);
        }
    }
}

// --- The import ---

// The bones of a skeleton (one Skeletal Mesh): its root and the meshes it deforms
typedef struct FxSkel {
    int root;           // the top LimbNode
    int geoms[64], models[64], ngeom; // the skinned geometries and their models
} FxSkel;

// A light or a camera: its actor
typedef struct FxEmit {
    bool camera;
    CLight l;
    float fov, loc[3], rot[3];
    char name[64];
} FxEmit;

static int fx_conn_model(const Fx* c, int o) { // the Model a Geometry / NodeAttribute belongs to
    FX_OUT(c, o, k) {
        int d = c->conns[k].dst;
        if (d >= 0 && !c->conns[k].op && !strcmp(c->objs[d].kind, "Model")) return d;
    }
    return -1;
}
static int fx_child_of_kind(const Fx* c, int o, const char* kind, const char* sub) { // the first source of a kind connected to o
    FX_IN(c, o, k) {
        int s = c->conns[k].src;
        if (s >= 0 && !c->conns[k].op && !strcmp(c->objs[s].kind, kind) && (!sub || !strcmp(c->objs[s].sub, sub))) return s;
    }
    return -1;
}
static bool fx_is_bone(const Fx* c, int o) { return o >= 0 && !strcmp(c->objs[o].kind, "Model") && !strcmp(c->objs[o].sub, "LimbNode"); }
static int fx_bone_root(const Fx* c, int o) {
    for (int d = 0; d < 256 && c->parent[o] >= 0 && fx_is_bone(c, c->parent[o]); d++) o = c->parent[o];
    return o;
}
static double* fx_matrix_child(const FbxNode* n, const char* name, M4* out) {
    size_t cnt = 0;
    double* d = fbx_array_f64(fbx_prop(fbx_find(n, name), 0), &cnt);
    if (d && cnt == 16)
        for (int k = 0; k < 16; k++) out->m[k] = (float)d[k];
    else
        free(d), d = NULL;
    return d;
}

typedef struct FxCurve {
    float *t, *v; // seconds, values
    size_t n;
} FxCurve;
static float fx_curve_at(const FxCurve* k, float t) {
    if (t <= k->t[0]) return k->v[0];
    if (t >= k->t[k->n - 1]) return k->v[k->n - 1];
    size_t lo = 0, hi = k->n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (k->t[mid] <= t) lo = mid;
        else hi = mid;
    }
    return k->v[lo] + (k->v[hi] - k->v[lo]) * (t - k->t[lo]) / (k->t[hi] - k->t[lo]);
}

// A skeleton into a Skeletal Mesh: its bones, the faces of its skinned meshes and of the meshes under its bones, then
// every animation stack baked
static void fx_skeleton(Fx* c, const FxSkel* sk, const uint8_t* attached_to, const int* model_slots, const int* model_nslots,
                        const uint32_t* mhash, int nmh, bool multi, const char* folder_disk, const char* folder_vpath, World* w, TlendReport* r) {
    int n = c->nobj;
    SkScene* s = calloc(1, sizeof *s);
    int* bone_of = malloc(sizeof(int) * (size_t)(n + 1));
    M4* rw = malloc(sizeof(M4) * (size_t)(n + 1)); // the rest worlds
    uint8_t* rd = calloc((size_t)n + 1, 1);
    int* order = malloc(sizeof(int) * (size_t)(n + 1));
    M4* tlink = malloc(sizeof(M4) * (size_t)(n + 1));
    uint8_t* has_link = calloc((size_t)n + 1, 1);
    if (!s || !bone_of || !rw || !rd || !order || !tlink || !has_link) goto done;
    for (int i = 0; i < n; i++) bone_of[i] = -1;
    // the bones: the clusters' and every LimbNode up to the root, by depth (parents first)
    uint8_t* in = calloc((size_t)n + 1, 1);
    if (!in) goto done;
    for (int gi = 0; gi < sk->ngeom; gi++) {
        int skin = fx_child_of_kind(c, sk->geoms[gi], "Deformer", "Skin");
        FX_IN(c, skin, k) {
            int cl = c->conns[k].src;
            if (cl < 0 || c->conns[k].op || strcmp(c->objs[cl].sub, "Cluster")) continue;
            int bone = fx_child_of_kind(c, cl, "Model", NULL);
            if (bone < 0) continue;
            M4 m;
            double* d = fx_matrix_child(c->objs[cl].n, "TransformLink", &m);
            if (d && !has_link[bone]) tlink[bone] = m, has_link[bone] = 1;
            free(d);
            for (int o = bone, dd = 0; o >= 0 && dd < 256 && !in[o]; o = c->parent[o], dd++) {
                in[o] = 1;
                if (o == sk->root) break;
            }
        }
    }
    int no = 0;
    for (int depth = 0; depth < 256 && no < ANIM_MAX_BONES; depth++)
        for (int o = 0; o < n && no < ANIM_MAX_BONES; o++) {
            if (!in[o]) continue;
            int d = 0;
            for (int p = c->parent[o]; p >= 0 && d < 300; p = c->parent[p]) d += in[p];
            if (d == depth) bone_of[o] = no, order[no++] = o;
        }
    free(in);
    if (!no) goto done;
    s->nb = no;
    // the rests: TransformLink, else the parent bone's rest times the rest pose between them
    for (int k = 0; k < no; k++) {
        int o = order[k], pb = -1;
        for (int p = c->parent[o]; p >= 0 && pb < 0; p = c->parent[p]) pb = bone_of[p];
        AnimBone* ab = &s->bones[k];
        snprintf(ab->name, sizeof ab->name, "%s", c->objs[o].name[0] ? c->objs[o].name : "Bone");
        for (int q = 0; q < k; q++)
            if (!strcmp(s->bones[q].name, ab->name)) snprintf(ab->name, sizeof ab->name, "%.40s_%d", c->objs[o].name, k);
        ab->parent = (int16_t)pb;
        M4 wo = fx_world(c, c->xf, rw, rd, o, 0);
        if (has_link[o]) s->rest[k] = fx_to_engine_m(c, &tlink[o]);
        else if (pb >= 0) {
            M4 wp = fx_world(c, c->xf, rw, rd, order[pb], 0), wpi = m4_inv(&wp), rel = m4_mul(&wpi, &wo), rel_e = fx_to_engine_m(c, &rel);
            M4 pe = s->rest[pb];
            // (rel in the engine's space: conjugation keeps products, so rest = parent rest * rel)
            s->rest[k] = m4_mul(&pe, &rel_e);
            // the translation of rel_e is already in centimeters (fx_to_engine_m scaled it): the product is right
        } else
            s->rest[k] = fx_to_engine_m(c, &wo);
    }
    // the faces: the skinned meshes at their bind (the cluster's Transform), each on the bone its corners lean on most
    for (int gi = 0; gi < sk->ngeom; gi++) {
        FxGeom g;
        if (!fx_geom_read(c->objs[sk->geoms[gi]].n, &g)) {
            fx_geom_free(&g);
            continue;
        }
        float* bw = calloc(g.ncp, sizeof(float));
        int* vb = malloc(sizeof(int) * g.ncp);
        SkinW* sw = calloc(g.ncp ? g.ncp : 1, sizeof(SkinW)); // (per control point: its 4 strongest bones)
        M4 bind = m4_id();
        bool got_bind = false;
        int skin = fx_child_of_kind(c, sk->geoms[gi], "Deformer", "Skin");
        for (size_t v = 0; vb && v < g.ncp; v++) vb[v] = -1;
        FX_IN(c, skin, k) {
            int cl = c->conns[k].src;
            if (!bw || !vb || cl < 0 || c->conns[k].op || strcmp(c->objs[cl].sub, "Cluster")) continue;
            int bone = fx_child_of_kind(c, cl, "Model", NULL);
            if (bone < 0 || bone_of[bone] < 0) continue;
            if (!got_bind) { // (the mesh at bind = TransformLink x Transform, as Blender reads it: the files store Transform in the bone's space)
                M4 tm, tl;
                double* d = fx_matrix_child(c->objs[cl].n, "Transform", &tm);
                double* e = fx_matrix_child(c->objs[cl].n, "TransformLink", &tl);
                if (d) bind = e ? m4_mul(&tl, &tm) : tm, got_bind = true;
                free(d), free(e);
            }
            size_t ni = 0, nw = 0;
            int32_t* idx = fbx_array_i32(fbx_prop(fbx_find(c->objs[cl].n, "Indexes"), 0), &ni);
            double* wt = fbx_array_f64(fbx_prop(fbx_find(c->objs[cl].n, "Weights"), 0), &nw);
            for (size_t q = 0; idx && wt && q < ni && q < nw; q++)
                if (idx[q] >= 0 && (size_t)idx[q] < g.ncp) {
                    if (sw) skin_add(&sw[idx[q]], bone_of[bone], (float)wt[q]);
                    if (wt[q] > bw[idx[q]]) bw[idx[q]] = (float)wt[q], vb[idx[q]] = bone_of[bone];
                }
            free(idx), free(wt);
        }
        if (!got_bind) { // (no Transform: the mesh model's rest world)
            int mo = sk->models[gi];
            M4 base, geom, wm = fx_world(c, c->xf, rw, rd, mo, 0);
            fx_matrices(&c->xf[mo], &base, &geom);
            bind = m4_mul(&wm, &geom);
        }
        int mo = sk->models[gi];
        size_t start = 0, poly = 0;
        for (size_t i = 0; bw && vb && i < g.npvi; i++) {
            if (g.pvi[i] >= 0) continue;
            size_t cnt = i + 1 - start;
            if (cnt >= 3 && obj_grow((void**)&s->tris, &s->cap, s->ntris + cnt - 2, sizeof(SkTri))) {
                int slot = 0;
                for (size_t t = 1; t + 1 < cnt; t++) {
                    size_t cs[3] = { start, start + t, start + t + 1 };
                    int32_t cp[3];
                    bool ok = true;
                    for (int q = 0; q < 3; q++) cp[q] = g.pvi[cs[q]] < 0 ? ~g.pvi[cs[q]] : g.pvi[cs[q]], ok &= (size_t)cp[q] < g.ncp;
                    if (!ok) continue;
                    SkTri* st = &s->tris[s->ntris++];
                    int b0 = vb[cp[0]], b1 = vb[cp[1]], b2 = vb[cp[2]];
                    st->bone = b1 == b2 && b1 >= 0 ? b1 : b0 >= 0 ? b0 : b1 >= 0 ? b1 : b2 >= 0 ? b2 : 0;
                    st->skinned = sw != NULL;
                    for (int q = 0; q < 3; q++) {
                        int qs = q == 0 ? 0 : 3 - q; // (mirrored: the winding reversed)
                        float uv[2], nf[3], pf[3] = { (float)g.cp[cp[qs] * 3], (float)g.cp[cp[qs] * 3 + 1], (float)g.cp[cp[qs] * 3 + 2] }, pb[3], nb[3];
                        fx_corner(&g, cs[qs], poly, (size_t)cp[qs], uv, nf, q == 0 ? &slot : NULL);
                        m4_point(&bind, pf, pb);
                        m4_dir(&bind, nf, nb);
                        fx_to_engine_p(c, pb, st->p[q]);
                        fx_to_engine_n(c, nb, st->n[q]);
                        st->uv[q][0] = uv[0], st->uv[q][1] = uv[1];
                        if (sw) skin_pack(&sw[cp[qs]], vb[cp[qs]] >= 0 ? vb[cp[qs]] : st->bone, st->skin[q]);
                    }
                    st->mat = slot < model_nslots[mo] ? model_slots[mo * 32 + slot] : -1;
                }
            }
            start = i + 1, poly++;
        }
        free(bw), free(vb), free(sw);
        fx_geom_free(&g);
    }
    // the meshes under its bones: rigid parts (their rest relative to the bone's, on the bone's bind)
    for (int mo = 0; mo < n; mo++) {
        if (!attached_to || attached_to[mo] != 1) continue;
        int bone = -1;
        for (int p = c->parent[mo]; p >= 0 && bone < 0; p = c->parent[p]) bone = bone_of[p] >= 0 ? p : -1;
        if (bone < 0) continue;
        int geo = fx_child_of_kind(c, mo, "Geometry", "Mesh");
        FxGeom g;
        if (geo < 0 || !fx_geom_read(c->objs[geo].n, &g)) {
            if (geo >= 0) fx_geom_free(&g);
            continue;
        }
        M4 base, geom, wm = fx_world(c, c->xf, rw, rd, mo, 0), wb = fx_world(c, c->xf, rw, rd, bone, 0);
        fx_matrices(&c->xf[mo], &base, &geom);
        M4 mg = m4_mul(&wm, &geom), wbi = m4_inv(&wb), rel = m4_mul(&wbi, &mg);
        M4 rel_e = fx_to_engine_m(c, &rel), at = m4_mul(&s->rest[bone_of[bone]], &rel_e);
        size_t start = 0, poly = 0;
        for (size_t i = 0; i < g.npvi; i++) {
            if (g.pvi[i] >= 0) continue;
            size_t cnt = i + 1 - start;
            if (cnt >= 3 && obj_grow((void**)&s->tris, &s->cap, s->ntris + cnt - 2, sizeof(SkTri))) {
                int slot = 0;
                for (size_t t = 1; t + 1 < cnt; t++) {
                    size_t cs[3] = { start, start + t, start + t + 1 };
                    int32_t cp[3];
                    bool ok = true;
                    for (int q = 0; q < 3; q++) cp[q] = g.pvi[cs[q]] < 0 ? ~g.pvi[cs[q]] : g.pvi[cs[q]], ok &= (size_t)cp[q] < g.ncp;
                    if (!ok) continue;
                    SkTri* st = &s->tris[s->ntris++];
                    st->bone = bone_of[bone];
                    st->skinned = 0; // (a mesh held by a bone: on it alone)
                    for (int q = 0; q < 3; q++) {
                        int qs = q == 0 ? 0 : 3 - q;
                        float uv[2], nf[3], pf[3] = { (float)g.cp[cp[qs] * 3], (float)g.cp[cp[qs] * 3 + 1], (float)g.cp[cp[qs] * 3 + 2] }, pe[3], ne[3];
                        fx_corner(&g, cs[qs], poly, (size_t)cp[qs], uv, nf, q == 0 ? &slot : NULL);
                        fx_to_engine_p(c, pf, pe), fx_to_engine_n(c, nf, ne);
                        m4_point(&at, pe, st->p[q]);
                        m4_dir(&at, ne, st->n[q]);
                        st->uv[q][0] = uv[0], st->uv[q][1] = uv[1];
                    }
                    st->mat = slot < model_nslots[mo] ? model_slots[mo * 32 + slot] : -1;
                }
            }
            start = i + 1, poly++;
        }
        fx_geom_free(&g);
    }
    // the name: the armature (the Null over the root bone), else the root bone's
    int arm = c->parent[sk->root];
    snprintf(s->base, sizeof s->base, "%s", arm >= 0 && c->objs[arm].name[0] ? c->objs[arm].name : c->objs[sk->root].name);
    char name[200];
    if (!sk_write_mesh(s, mhash, nmh, folder_disk, folder_vpath, name, sizeof name, r)) goto done;
    // the animations: each stack's first layer, the curves of the bones and their ancestors, at 30 fps
    uint8_t* affects = calloc((size_t)n + 1, 1);
    FxXform* cur = malloc(sizeof(FxXform) * (size_t)(n + 1));
    uint32_t first_anim = 0;
    for (int k = 0; affects && k < no; k++)
        for (int o = order[k], d = 0; o >= 0 && d < 256; o = c->parent[o], d++) affects[o] = 1;
    for (int st = 0; affects && cur && st < n; st++) {
        if (strcmp(c->objs[st].kind, "AnimationStack")) continue;
        int layer = -1;
        FX_IN(c, st, k) {
            int l = c->conns[k].src;
            if (layer < 0 && l >= 0 && !c->conns[k].op && !strcmp(c->objs[l].kind, "AnimationLayer")) layer = l;
        }
        if (layer < 0) continue;
        // its curves: per (model, channel 0..8)
        int nitem = 0, cap_item = 0;
        struct FxItem { int model; FxCurve cv[9]; }* items = NULL;
        float tmax = 0.0f;
        FX_IN(c, layer, kn) {
            int cn = c->conns[kn].src;
            if (cn < 0 || c->conns[kn].op || strcmp(c->objs[cn].kind, "AnimationCurveNode")) continue;
            FX_OUT(c, cn, kt) {
                const FxConn* to = &c->conns[kt];
                if (!to->op || to->dst < 0 || !affects[to->dst]) continue;
                int p = !strcmp(to->prop, "Lcl Translation") ? 0 : !strcmp(to->prop, "Lcl Rotation") ? 1 : !strcmp(to->prop, "Lcl Scaling") ? 2 : -1;
                if (p < 0) continue;
                int it = -1;
                for (int i = 0; i < nitem; i++)
                    if (items[i].model == to->dst) it = i;
                if (it < 0) {
                    if (nitem == cap_item) {
                        cap_item = cap_item ? cap_item * 2 : 32;
                        void* ni = realloc(items, sizeof *items * (size_t)cap_item);
                        if (!ni) break;
                        items = ni;
                    }
                    it = nitem++;
                    memset(&items[it], 0, sizeof items[it]);
                    items[it].model = to->dst;
                }
                FX_IN(c, cn, kc) {
                    const FxConn* cc = &c->conns[kc];
                    if (cc->src < 0 || !cc->op || strcmp(c->objs[cc->src].kind, "AnimationCurve")) continue;
                    int ch = !strcmp(cc->prop, "d|X") ? 0 : !strcmp(cc->prop, "d|Y") ? 1 : !strcmp(cc->prop, "d|Z") ? 2 : -1;
                    FxCurve* cv = ch >= 0 ? &items[it].cv[p * 3 + ch] : NULL;
                    if (!cv || cv->n) continue;
                    size_t nt = 0, nv = 0;
                    int64_t* kt_ = fbx_array_i64(fbx_prop(fbx_find(c->objs[cc->src].n, "KeyTime"), 0), &nt);
                    double* kv = fbx_array_f64(fbx_prop(fbx_find(c->objs[cc->src].n, "KeyValueFloat"), 0), &nv);
                    size_t cnt = nt < nv ? nt : nv;
                    cv->t = malloc(sizeof(float) * (cnt + 1)), cv->v = malloc(sizeof(float) * (cnt + 1));
                    for (size_t q = 0; cv->t && cv->v && kt_ && kv && q < cnt; q++) {
                        float tt = (float)((double)kt_[q] / (double)c->ktime);
                        if (cv->n && tt <= cv->t[cv->n - 1]) continue; // (strictly increasing: the first value at a time)
                        cv->t[cv->n] = tt, cv->v[cv->n] = (float)kv[q], cv->n++;
                    }
                    if (cv->n) tmax = fmaxf(tmax, cv->t[cv->n - 1]);
                    free(kt_), free(kv);
                }
            }
        }
        FxProps sp = fx_props(c, c->objs[st].n, "AnimationStack", "FbxAnimStack");
        float t0 = (float)((double)fx_p_int(sp, "LocalStart", 0) / (double)c->ktime), t1 = (float)((double)fx_p_int(sp, "LocalStop", 0) / (double)c->ktime);
        if (t1 <= t0) t0 = 0.0f, t1 = tmax;
        uint32_t nframes = (uint32_t)((t1 - t0) * FX_ANIM_FPS + 0.5f) + 1;
        if (nframes < 2) nframes = 2;
        if (nframes > 20000) nframes = 20000;
        M4* wall = nitem ? malloc(sizeof(M4) * (size_t)nframes * (size_t)no) : NULL;
        M4* memo = malloc(sizeof(M4) * (size_t)(n + 1));
        uint8_t* md = malloc((size_t)n + 1);
        if (wall && memo && md) {
            for (uint32_t fr = 0; fr < nframes; fr++) {
                float t = t0 + (float)fr / FX_ANIM_FPS;
                memcpy(cur, c->xf, sizeof(FxXform) * (size_t)n);
                for (int i = 0; i < nitem; i++) {
                    FxXform* x = &cur[items[i].model];
                    float* dst[3] = { x->loc, x->rot, x->sca };
                    for (int ch = 0; ch < 9; ch++)
                        if (items[i].cv[ch].n) dst[ch / 3][ch % 3] = fx_curve_at(&items[i].cv[ch], t);
                }
                memset(md, 0, (size_t)n);
                for (int k = 0; k < no; k++) {
                    M4 wo = fx_world(c, cur, memo, md, order[k], 0);
                    wall[(size_t)fr * (size_t)no + (size_t)k] = fx_to_engine_m(c, &wo);
                }
            }
            char an_name[200], label[120];
            char sn[64];
            fbx_name_class(fbx_prop(c->objs[st].n, 1), sn, sizeof sn, NULL, 0);
            snprintf(label, sizeof label, "%s", sn[0] ? sn : "Take");
            write_name(an_name, sizeof an_name, "A_", multi ? s->base : label, multi ? label : NULL);
            uint32_t h = sk_write_anim(s, name, an_name, FX_ANIM_FPS, nframes, wall, folder_disk, folder_vpath, r, NULL);
            if (!first_anim) first_anim = h;
        }
        free(wall), free(memo), free(md);
        for (int i = 0; i < nitem; i++)
            for (int ch = 0; ch < 9; ch++) free(items[i].cv[ch].t), free(items[i].cv[ch].v);
        free(items);
    }
    free(affects), free(cur);
    sk_actor(w, s->base, name, first_anim, folder_vpath, r);
done:
    if (s) free(s->tris);
    free(s), free(bone_of), free(rw), free(rd), free(order), free(tlink), free(has_link);
}

bool fbx_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    imprep_begin(src); // (Q19) the conversion report
    snprintf(g_import_src, sizeof g_import_src, "%s", src), g_import_rigid = false;
    memset(r, 0, sizeof *r);
    Fx* c = calloc(1, sizeof *c);
    char err[200] = "";
    if (!c || !fbx_doc_load(&c->doc, src, err, sizeof err)) {
        snprintf(r->text, sizeof r->text, "%s: %s", src, c ? err : "Out of memory.");
        free(c);
        return false;
    }
    if (c->doc.version < 7100) {
        snprintf(r->text, sizeof r->text, "%s: FBX %d is too old (7.1 or newer).", src, c->doc.version);
        fbx_doc_free(&c->doc), free(c);
        return false;
    }
    snprintf(c->dir, sizeof c->dir, "%s", src);
    char* sl = strrchr(c->dir, '/');
    char* bs = strrchr(c->dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = 0;
    else snprintf(c->dir, sizeof c->dir, ".");
    const char* bn = src + strlen(src);
    while (bn > src && bn[-1] != '/' && bn[-1] != '\\') bn--;
    snprintf(c->base, sizeof c->base, "%s", bn);
    char* dot = strrchr(c->base, '.');
    if (dot) *dot = 0;
    char folder_disk[900];
    snprintf(folder_disk, sizeof folder_disk, "%s%s", content_root, folder_vpath + 8);
    snprintf(c->tex_dir, sizeof c->tex_dir, "%s/Textures", folder_disk);
    // the axes and the unit
    const FbxNode* root = &c->doc.root;
    FxProps gs = { fbx_find(fbx_find(root, "GlobalSettings"), "Properties70"), NULL };
    int ua = (int)fx_p_int(gs, "UpAxis", 1), us = (int)fx_p_int(gs, "UpAxisSign", 1);
    int ca = (int)fx_p_int(gs, "CoordAxis", 0), cs = (int)fx_p_int(gs, "CoordAxisSign", 1);
    if (ua < 0 || ua > 2 || ca < 0 || ca > 2 || ua == ca) ua = 1, us = 1, ca = 0, cs = 1;
    float up[3] = { 0, 0, 0 }, co[3] = { 0, 0, 0 };
    up[ua] = us < 0 ? -1.0f : 1.0f, co[ca] = cs < 0 ? -1.0f : 1.0f;
    memcpy(c->ax[0], co, 12), memcpy(c->ax[2], up, 12); // Blender's X = the coord axis, Z = up, Y = Z x X
    c->ax[1][0] = up[1] * co[2] - up[2] * co[1], c->ax[1][1] = up[2] * co[0] - up[0] * co[2], c->ax[1][2] = up[0] * co[1] - up[1] * co[0];
    c->unit = (float)(fx_p_num(gs, "UnitScaleFactor", 1.0) / 100.0);
    if (!(c->unit > 0.0f)) c->unit = 0.01f;
    c->ktime = c->doc.version >= 8000 ? FX_KTIME_V8 : FX_KTIME_V7;
    const FbxNode* hdr = fbx_find(root, "FBXHeaderExtension");
    if (fbx_child_int(hdr, "FBXHeaderVersion", 0) >= 1004) {
        const FbxNode* tc = fbx_find(fbx_find(hdr, "OtherFlags"), "TCDefinition");
        if (tc) c->ktime = fbx_prop_int(fbx_prop(tc, 0), 0) == 127 ? FX_KTIME_V7 : FX_KTIME_V8;
    }
    bool ok = fx_index_objects(c, fbx_find(root, "Objects")) && fx_index_connections(c, fbx_find(root, "Connections"));
    int n = c->nobj;
    TFile f;
    memset(&f, 0, sizeof f);
    f.fps = FX_ANIM_FPS;
    int* model_slots = NULL; // per Model: up to 32 material indices
    int* model_nslots = NULL;
    uint8_t* attached = NULL; // per Model: 1 under a bone (a rigid part), 2 skinned
    uint32_t* mhash = NULL;
    FxSkel* skels = NULL;
    int nskel = 0;
    FxEmit* em = NULL; // the lights and the cameras
    int nem = 0;
    if (ok) {
        fx_read_templates(c);
        c->parent = malloc(sizeof(int) * (size_t)(n + 1));
        c->xf = calloc((size_t)n + 1, sizeof(FxXform));
        c->mat_of = malloc(sizeof(int) * (size_t)(n + 1));
        model_slots = calloc((size_t)(n + 1) * 32, sizeof(int));
        model_nslots = calloc((size_t)n + 1, sizeof(int));
        attached = calloc((size_t)n + 1, 1);
        skels = calloc(64, sizeof(FxSkel));
        em = calloc(256, sizeof(FxEmit));
        ok = c->parent && c->xf && c->mat_of && model_slots && model_nslots && attached && skels;
    }
    if (ok) {
        // the Models: their parents and rest transforms
        for (int i = 0; i < n; i++) {
            c->parent[i] = -1, c->mat_of[i] = -1;
            if (strcmp(c->objs[i].kind, "Model")) continue;
            fx_read_xform(fx_props(c, c->objs[i].n, "Model", "FbxNode"), &c->xf[i]);
            FX_OUT(c, i, k) {
                int d = c->conns[k].dst;
                if (d >= 0 && !c->conns[k].op && !strcmp(c->objs[d].kind, "Model") && c->parent[i] < 0) c->parent[i] = d;
            }
        }
        // the materials
        int nm = 0;
        for (int i = 0; i < n; i++) nm += !strcmp(c->objs[i].kind, "Material");
        f.mats = calloc((size_t)nm + 1, sizeof(TMat));
        for (int i = 0; f.mats && i < n; i++)
            if (!strcmp(c->objs[i].kind, "Material")) c->mat_of[i] = f.nmats, fx_material(c, i, &f.mats[f.nmats++]);
        for (int i = 0; i < n; i++) { // (a model's slots: its materials in connection order)
            if (strcmp(c->objs[i].kind, "Model")) continue;
            FX_IN(c, i, k) {
                int s = c->conns[k].src;
                if (s >= 0 && !c->conns[k].op && c->mat_of[s] >= 0 && model_nslots[i] < 32) model_slots[i * 32 + model_nslots[i]++] = c->mat_of[s];
            }
        }
        // the skeletons: per skinned geometry, the root of its clusters' bones
        for (int gi = 0; gi < n; gi++) {
            if (strcmp(c->objs[gi].kind, "Geometry") || strcmp(c->objs[gi].sub, "Mesh")) continue;
            int skin = fx_child_of_kind(c, gi, "Deformer", "Skin"), mo = fx_conn_model(c, gi), bone = -1;
            FX_IN(c, skin >= 0 ? skin : gi, k) {
                int cl = c->conns[k].src;
                if (skin >= 0 && bone < 0 && cl >= 0 && !c->conns[k].op && !strcmp(c->objs[cl].sub, "Cluster")) bone = fx_child_of_kind(c, cl, "Model", NULL);
            }
            if (bone < 0 || mo < 0) continue;
            int rt = fx_bone_root(c, bone), si = -1;
            for (int q = 0; q < nskel; q++)
                if (skels[q].root == rt) si = q;
            if (si < 0 && nskel < 64) si = nskel++, skels[si].root = rt;
            if (si < 0 || skels[si].ngeom >= 64) continue;
            skels[si].geoms[skels[si].ngeom] = gi, skels[si].models[skels[si].ngeom++] = mo;
            attached[mo] = 2;
        }
        // the meshes under a bone of a skeleton: its parts
        for (int mo = 0; mo < n; mo++) {
            if (strcmp(c->objs[mo].kind, "Model") || strcmp(c->objs[mo].sub, "Mesh") || attached[mo]) continue;
            for (int p = c->parent[mo], d = 0; p >= 0 && d < 256 && !attached[mo]; p = c->parent[p], d++)
                for (int q = 0; q < nskel && fx_is_bone(c, p); q++)
                    if (fx_bone_root(c, p) == skels[q].root) attached[mo] = 1;
        }
        // the static meshes: an object per mesh model
        size_t cap_obj = 0, cap_mesh = 0;
        M4* rw = malloc(sizeof(M4) * (size_t)(n + 1));
        uint8_t* rd = calloc((size_t)n + 1, 1);
        for (int mo = 0; rw && rd && mo < n; mo++) {
            if (strcmp(c->objs[mo].kind, "Model") || strcmp(c->objs[mo].sub, "Mesh") || attached[mo]) continue;
            int geo = fx_child_of_kind(c, mo, "Geometry", "Mesh");
            FxGeom g;
            if (geo < 0 || !fx_geom_read(c->objs[geo].n, &g)) {
                if (geo >= 0) fx_geom_free(&g);
                continue;
            }
            TMesh me;
            bool made = fx_tmesh(c, &g, model_nslots[mo] ? model_nslots[mo] : 1, &me);
            fx_geom_free(&g);
            if (!made) continue;
            if (!obj_grow((void**)&f.meshes, &cap_mesh, (size_t)f.nmeshes + 1, sizeof(TMesh)) ||
                !obj_grow((void**)&f.objs, &cap_obj, (size_t)f.nobjs + 1, sizeof(TObj))) {
                free(me.co), free(me.loops), free(me.uv), free(me.fstart), free(me.fsize), free(me.fmat);
                break;
            }
            f.meshes[f.nmeshes] = me;
            TObj* o = &f.objs[f.nobjs];
            memset(o, 0, sizeof *o);
            o->mesh = f.nmeshes++, o->arm = o->parent = o->deform = o->action = -1;
            o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
            memcpy(o->parent_inv, m4_id().m, sizeof o->parent_inv);
            M4 base, geom, wm = fx_world(c, c->xf, rw, rd, mo, 0);
            fx_matrices(&c->xf[mo], &base, &geom);
            M4 wg = m4_mul(&wm, &geom);
            o->world = fx_to_blender(c, &wg), o->world_done = true;
            o->nslots = model_nslots[mo];
            for (int q = 0; q < o->nslots; q++) o->slots[q] = model_slots[mo * 32 + q];
            snprintf(o->name, sizeof o->name, "%s", c->objs[mo].name[0] ? c->objs[mo].name : "Mesh");
            for (int k = 2, dup = 1; dup && k < 10000; k++) {
                dup = 0;
                for (int q = 0; q < f.nobjs && !dup; q++) dup = !strcmp(f.objs[q].name, o->name);
                if (dup) snprintf(o->name, sizeof o->name, "%.56s_%d", c->objs[mo].name, k);
            }
            f.nobjs++;
        }
        // the lights and the cameras: a Model with a NodeAttribute; FBX's lights shine down their -Y, its cameras look down
        // their +X with Y up (Blender's exporter turns its own to that)
        for (int mo = 0; rw && rd && em && mo < n && nem < 256; mo++) {
            bool light = !strcmp(c->objs[mo].sub, "Light"), cam = !strcmp(c->objs[mo].sub, "Camera");
            if (strcmp(c->objs[mo].kind, "Model") || (!light && !cam)) continue;
            int at = fx_child_of_kind(c, mo, "NodeAttribute", light ? "Light" : "Camera");
            FxProps p = fx_props(c, at >= 0 ? c->objs[at].n : NULL, "NodeAttribute", light ? "FbxLight" : "FbxCamera");
            FxEmit* x = &em[nem];
            memset(x, 0, sizeof *x);
            x->camera = cam;
            snprintf(x->name, sizeof x->name, "%s", c->objs[mo].name[0] ? c->objs[mo].name : cam ? "CameraActor" : "Light");
            M4 wm = fx_world(c, c->xf, rw, rd, mo, 0);
            float fl[3], ul[3], fwd[3], up[3];
            m4_dir(&wm, cam ? (float[]){ 1, 0, 0 } : (float[]){ 0, -1, 0 }, fl), m4_dir(&wm, cam ? (float[]){ 0, 1, 0 } : (float[]){ 1, 0, 0 }, ul);
            fx_to_engine_n(c, fl, fwd), fx_to_engine_n(c, ul, up);
            fx_to_engine_p(c, &wm.m[12], x->loc);
            imp_facing_rotator(fwd, up, x->rot);
            if (cam) { // the horizontal field of view: from the focal length and the film's width (inches), else FieldOfView
                double focal = fx_p_num(p, "FocalLength", 0.0), film = fx_p_num(p, "FilmWidth", 0.0);
                double fov = focal > 0.0 && film > 0.0 ? 2.0 * atan(film * 25.4 * 0.5 / focal) * 57.29578 : 60.0;
                if (fx_p_find(p, "FieldOfView") && (fx_p_int(p, "ApertureMode", 2) != 2 || focal <= 0.0)) fov = fx_p_num(p, "FieldOfView", fov);
                x->fov = (float)fov;
            } else {
                CLight* l = &x->l;
                int64_t ty = fx_p_int(p, "LightType", 0); // 0 point, 1 directional, 2 spot (3 area, 4 volume: points)
                l->type = ty == 1 ? LIGHT_DIRECTIONAL : ty == 2 ? LIGHT_SPOT : LIGHT_POINT;
                fx_p_vec3(p, "Color", l->color, 1, 1, 1);
                // the intensity: FBX's is a percentage, Blender writes its watts (W/m² for a sun) times 100; turned into
                // candela and lux as Blender's glTF exporter does (683 lm/W, over 4π for the points and the spots)
                float watts = (float)fx_p_num(p, "Intensity", 100.0) / 100.0f;
                l->intensity = watts * (l->type == LIGHT_DIRECTIONAL ? 683.0f : 683.0f / 12.566371f);
                l->attenuation_radius = fminf(fmaxf(sqrtf(fmaxf(l->intensity, 0.0f) / 0.05f), 5.0f), 50.0f) * 100.0f;
                if (fx_p_int(p, "EnableFarAttenuation", 0)) // (its end in the file's units)
                    l->attenuation_radius = (float)fx_p_num(p, "FarAttenuationEnd", 0.0) * c->unit * 100.0f;
                // the cone: FBX's angles are the whole cone, the Engine's from the middle
                l->outer_cone = (float)fx_p_num(p, "OuterAngle", fx_p_num(p, "Cone angle", 45.0)) * 0.5f;
                l->inner_cone = fminf((float)fx_p_num(p, "InnerAngle", 0.0) * 0.5f, l->outer_cone);
                l->cast_shadows = fx_p_int(p, "CastShadows", 1) != 0;
            }
            nem++;
        }
        free(rw), free(rd);
    }
    // the materials' hashes (the skeletons' parts use them; tfile_assets frees the scene)
    int nmh = f.nmats;
    mhash = calloc((size_t)nmh + 1, sizeof(uint32_t));
    for (int k = 0; mhash && k < nmh; k++) {
        char mn[96], mv[1100];
        snprintf(mn, sizeof mn, "M_%s", f.mats[k].name);
        clean(mn);
        snprintf(mv, sizeof mv, "%s/Materials/%s.rmat", folder_vpath, mn);
        mhash[k] = vhash(mv);
    }
    bool done = false;
    if (!ok || !mhash || (!f.nobjs && !nskel && !nem)) {
        snprintf(r->text, sizeof r->text, ok && mhash ? "%s has no meshes." : "Out of memory reading %s.", src);
        obj_free_meshes(&f);
    } else {
        done = tfile_assets(f, src, folder_vpath, content_root, w, r);
        unsigned sk0 = r->skeletal;
        for (int q = 0; q < nskel; q++)
            fx_skeleton(c, &skels[q], attached, model_slots, model_nslots, mhash, nmh, nskel > 1, folder_disk, folder_vpath, w, r);
        if (r->skeletal > sk0) {
            snprintf(r->text, sizeof r->text, "%u meshes, %u materials (%u textures, %u nodes skipped), %u skeletal meshes, %u animations, %u actors",
                     r->meshes, r->materials, r->textures, r->skipped_nodes, r->skeletal, r->animations, r->actors);
            done = true;
        }
        // the lights and the cameras: their actors
        for (int i = 0; w && i < nem; i++) {
            Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(em[i].camera ? COMP_CAMERA : COMP_LIGHT));
            if (e == ENTITY_NONE) break;
            snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", em[i].name);
            CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            memcpy(t->location, em[i].loc, 12), memcpy(t->rotation, em[i].rot, 12);
            t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
            if (em[i].camera) ((CCamera*)ecs_get(w, e, COMP_CAMERA))->fov_deg = fminf(fmaxf(em[i].fov, 10.0f), 170.0f), r->cameras++;
            else t->flags = MOBILITY_STATIONARY, *(CLight*)ecs_get(w, e, COMP_LIGHT) = em[i].l, r->lights++;
        }
        if (r->lights) {
            size_t ln = strlen(r->text);
            snprintf(r->text + ln, sizeof r->text - ln, ", %u lights", r->lights);
        }
        if (r->cameras) {
            size_t ln = strlen(r->text);
            snprintf(r->text + ln, sizeof r->text - ln, ", %u cameras", r->cameras);
        }
        done = done || r->lights || r->cameras;
    }
    free(em);
    free(mhash), free(model_slots), free(model_nslots), free(attached), free(skels);
    free(c->objs), free(c->hash), free(c->conns), free(c->out_start), free(c->out_list), free(c->in_start), free(c->in_list), free(c->tmpls);
    free(c->parent), free(c->xf), free(c->mat_of);
    fbx_doc_free(&c->doc);
    free(c);
    return done;
}
