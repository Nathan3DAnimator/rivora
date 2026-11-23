// --- (Q19, notas-dev/importar-tlender.md) The Tlender's modifier stacks applied at the import (included by
// tlend_import.c): each object's "mods" lines ("md <Blender type> <flags> <name>", "mp key=value;...") run in order on a
// copy of its mesh in Blender's space (meters), the vertex groups and the shape keys carried through (each new vertex a
// weighted sum of the old ones), before the Static / Skeletal Mesh is written. What the Engine has its own of is
// converted (Armature: the Skeletal Mesh; Cloth, Soft Body: the Cloth component; Edge Split: the smoothing angle;
// Triangulate: the Engine's own); what it can't apply is lost with its reason unless the file brings the object's
// evaluated mesh (the "evalmeshes" block, defined here for the Tlender's export: notas-dev/importar-tlender.md). ---

// A modifier's settings ("key=value;key=value", Blender's names)
static bool mp_get(const char* mp, const char* key, char* out, size_t cap) {
    size_t kl = strlen(key);
    for (const char* p = mp; p && *p;) {
        const char* e = strchr(p, ';');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = n - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, p + kl + 1, vl), out[vl] = 0;
            return true;
        }
        p = e ? e + 1 : NULL;
    }
    return false;
}
static float mp_f(const char* mp, const char* key, float def) {
    char v[64];
    return mp_get(mp, key, v, sizeof v) ? strtof(v, NULL) : def;
}
static int mp_i(const char* mp, const char* key, int def) {
    char v[64];
    return mp_get(mp, key, v, sizeof v) ? atoi(v) : def;
}
static void mp_v3(const char* mp, const char* key, float o[3], float a, float b, float c) {
    char v[128];
    o[0] = a, o[1] = b, o[2] = c;
    if (mp_get(mp, key, v, sizeof v)) sscanf(v, "%f,%f,%f", &o[0], &o[1], &o[2]);
}
// an enum by its Blender name among names ("A,B,C"), or its number
static int mp_enum(const char* mp, const char* key, const char* names, int def) {
    char v[64];
    if (!mp_get(mp, key, v, sizeof v)) return def;
    int k = 0;
    for (const char* p = names; *p; k++) {
        const char* e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == n && !strncmp(v, p, n)) return k;
        if (!e) break;
        p = e + 1;
    }
    return (v[0] >= '0' && v[0] <= '9') ? atoi(v) : def;
}

// --- A mesh being built: vertices (positions, group weights vertex-major, shape key offsets), faces and corners ---
typedef struct MB {
    TMesh m;
    float* gw; // nv x ngroups (vertex-major while building)
    uint32_t vcap, fcap, ccap;
    bool fail;
} MB;

static bool mb_grow(void** p, uint32_t* cap, uint32_t need, size_t elem) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void* n = realloc(*p, (size_t)c * elem);
    if (!n) return false;
    *p = n;
    *cap = c;
    return true;
}

static void mb_init(MB* b, const TMesh* s) {
    memset(b, 0, sizeof *b);
    b->m.ngroups = s->ngroups;
    if (s->ngroups) {
        b->m.gname = malloc((size_t)s->ngroups * sizeof *s->gname);
        if (b->m.gname) memcpy(b->m.gname, s->gname, (size_t)s->ngroups * sizeof *s->gname);
        else b->m.ngroups = 0;
    }
    b->m.nkeys = s->nkeys;
    if (s->nkeys) {
        b->m.kname = malloc((size_t)s->nkeys * RMESH_MORPH_NAME), b->m.kvalue = malloc(sizeof(float) * s->nkeys);
        b->m.krange = s->krange ? malloc(sizeof *s->krange * s->nkeys) : NULL;
        if (b->m.kname && b->m.kvalue) memcpy(b->m.kname, s->kname, (size_t)s->nkeys * RMESH_MORPH_NAME), memcpy(b->m.kvalue, s->kvalue, sizeof(float) * s->nkeys);
        else b->m.nkeys = 0;
        if (b->m.krange) memcpy(b->m.krange, s->krange, sizeof *s->krange * s->nkeys);
    }
    if (s->col) b->m.col = (uint32_t*)1; // (marked: corners carry colors; allocated as they come)
}

// a new vertex: the weighted sum of source vertices (positions, weights, key offsets); M (3x3, row-major) turns the
// position (with t added) and the offsets; gmap remaps the groups (Mirror's left / right; NULL: the same)
static uint32_t mb_vert(MB* b, const TMesh* s, const uint32_t* sv, const float* w, int n, const float* M, const float* t, const int* gmap) {
    uint32_t v = b->m.nv;
    uint32_t c3 = b->vcap * 3;
    bool ok = !b->fail;
    if (ok && v + 1 > b->vcap) {
        uint32_t cap = b->vcap;
        ok = mb_grow((void**)&b->m.co, &c3, (v + 1) * 3, sizeof(float));
        uint32_t cg = cap * (b->m.ngroups ? b->m.ngroups : 1), ck = cap * (b->m.nkeys ? b->m.nkeys * 3 : 1);
        if (ok && b->m.ngroups) ok = mb_grow((void**)&b->gw, &cg, (v + 1) * b->m.ngroups, sizeof(float));
        if (ok && b->m.nkeys) ok = mb_grow((void**)&b->m.kdelta, &ck, (v + 1) * b->m.nkeys * 3, sizeof(float));
        b->vcap = c3 / 3;
        if (b->m.ngroups && cg / b->m.ngroups < b->vcap) b->vcap = cg / b->m.ngroups;
        if (b->m.nkeys && ck / (b->m.nkeys * 3) < b->vcap) b->vcap = ck / (b->m.nkeys * 3);
    }
    if (!ok) {
        b->fail = true;
        return 0;
    }
    float p[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) p[k] += s->co[sv[i] * 3 + k] * w[i];
    float* o = &b->m.co[v * 3];
    if (M) for (int k = 0; k < 3; k++) o[k] = M[k * 3] * p[0] + M[k * 3 + 1] * p[1] + M[k * 3 + 2] * p[2];
    else memcpy(o, p, 12);
    if (t) for (int k = 0; k < 3; k++) o[k] += t[k];
    for (uint32_t g = 0; g < b->m.ngroups; g++) {
        float a = 0.0f;
        int sg = gmap ? gmap[g] : (int)g; // (the group this one takes its weights from)
        for (int i = 0; i < n && sg >= 0 && (uint32_t)sg < s->ngroups && s->gweight; i++) a += s->gweight[(size_t)sg * s->nv + sv[i]] * w[i];
        b->gw[(size_t)v * b->m.ngroups + g] = a;
    }
    for (uint32_t k = 0; k < b->m.nkeys; k++) {
        float d[3] = { 0, 0, 0 };
        for (int i = 0; i < n && s->kdelta; i++)
            for (int c = 0; c < 3; c++) d[c] += s->kdelta[((size_t)sv[i] * s->nkeys + k) * 3 + c] * w[i];
        float* od = &b->m.kdelta[((size_t)v * b->m.nkeys + k) * 3];
        if (M) for (int c = 0; c < 3; c++) od[c] = M[c * 3] * d[0] + M[c * 3 + 1] * d[1] + M[c * 3 + 2] * d[2];
        else memcpy(od, d, 12);
    }
    b->m.nv++;
    return v;
}
static uint32_t mb_copy(MB* b, const TMesh* s, uint32_t sv) {
    float one = 1.0f;
    return mb_vert(b, s, &sv, &one, 1, NULL, NULL, NULL);
}

// a face of n corners (their vertices of b, their UVs and colors), material slot mat
static void mb_face(MB* b, const uint32_t* v, const float (*uv)[2], const uint32_t* col, uint32_t n, uint16_t mat) {
    if (b->fail || n < 3) return;
    uint32_t f = b->m.nf, c = b->m.ncorners;
    uint32_t c2 = b->ccap * 2, fcs = b->fcap, fcz = b->fcap, fcm = b->fcap;
    bool ok = mb_grow((void**)&b->m.fstart, &fcs, f + 1, sizeof(uint32_t)) && mb_grow((void**)&b->m.fsize, &fcz, f + 1, sizeof(uint32_t)) &&
              mb_grow((void**)&b->m.fmat, &fcm, f + 1, sizeof(uint16_t));
    b->fcap = fcs < fcz ? (fcs < fcm ? fcs : fcm) : (fcz < fcm ? fcz : fcm);
    uint32_t cl = b->ccap, cc = b->ccap;
    ok = ok && mb_grow((void**)&b->m.loops, &cl, c + n, sizeof(uint32_t)) && mb_grow((void**)&b->m.uv, &c2, (c + n) * 2, sizeof(float));
    if (ok && b->m.col) {
        if (b->m.col == (uint32_t*)1) b->m.col = NULL, cc = 0;
        ok = mb_grow((void**)&b->m.col, &cc, c + n, sizeof(uint32_t));
        if (ok && cc < cl) cl = cc;
    }
    b->ccap = cl < c2 / 2 ? cl : c2 / 2;
    if (!ok) {
        b->fail = true;
        return;
    }
    b->m.fstart[f] = c, b->m.fsize[f] = n, b->m.fmat[f] = mat;
    for (uint32_t k = 0; k < n; k++) {
        b->m.loops[c + k] = v[k];
        b->m.uv[(c + k) * 2] = uv ? uv[k][0] : 0.0f, b->m.uv[(c + k) * 2 + 1] = uv ? uv[k][1] : 0.0f;
        if (b->m.col) b->m.col[c + k] = col ? col[k] : 0xFFFFFFFFu;
    }
    b->m.nf++, b->m.ncorners += n;
}

// the built mesh into t (the old one freed): the weights back to group-major
static bool mb_finish(MB* b, TMesh* t) {
    if (b->fail || !b->m.nf) {
        free(b->m.co), free(b->gw), free(b->m.kdelta), free(b->m.fstart), free(b->m.fsize), free(b->m.fmat), free(b->m.loops), free(b->m.uv);
        if (b->m.col != (uint32_t*)1) free(b->m.col);
        free(b->m.gname), free(b->m.kname), free(b->m.kvalue), free(b->m.krange);
        return false;
    }
    if (b->m.col == (uint32_t*)1) b->m.col = NULL;
    if (b->m.ngroups) {
        b->m.gweight = malloc(sizeof(float) * (size_t)b->m.ngroups * b->m.nv + 4);
        for (uint32_t g = 0; b->m.gweight && g < b->m.ngroups; g++)
            for (uint32_t v = 0; v < b->m.nv; v++) b->m.gweight[(size_t)g * b->m.nv + v] = b->gw[(size_t)v * b->m.ngroups + g];
        if (!b->m.gweight) free(b->m.gname), b->m.gname = NULL, b->m.ngroups = 0;
    }
    free(b->gw);
    free(t->co), free(t->fstart), free(t->fsize), free(t->loops), free(t->uv), free(t->col), free(t->fmat), free(t->gname), free(t->gweight);
    tmesh_free_keys(t), tmesh_free_marks(t); // (the marked edges: renumbered vertices; the Subdivision sets its own after)
    *t = b->m;
    return true;
}

// a copy of a mesh (an object's own: the modifiers change it, the others sharing the mesh keep theirs)
static bool tmesh_clone(const TMesh* s, TMesh* t) {
    MB b;
    mb_init(&b, s);
    for (uint32_t v = 0; v < s->nv; v++) mb_copy(&b, s, v);
    for (uint32_t f = 0; f < s->nf; f++) {
        uint32_t st = s->fstart[f], n = s->fsize[f];
        mb_face(&b, &s->loops[st], (const float(*)[2]) & s->uv[st * 2], s->col ? &s->col[st] : NULL, n, s->fmat[f]);
    }
    memset(t, 0, sizeof *t);
    if (!mb_finish(&b, t)) return false;
    // (the same vertices: the marked edges as they are)
    if (s->ncrease && (t->crease_e = malloc(sizeof *t->crease_e * s->ncrease)) && (t->crease_s = malloc(sizeof(float) * s->ncrease)))
        memcpy(t->crease_e, s->crease_e, sizeof *t->crease_e * s->ncrease), memcpy(t->crease_s, s->crease_s, sizeof(float) * s->ncrease), t->ncrease = s->ncrease;
    if (s->nsharp && (t->sharp_e = malloc(sizeof *t->sharp_e * s->nsharp))) memcpy(t->sharp_e, s->sharp_e, sizeof *t->sharp_e * s->nsharp), t->nsharp = s->nsharp;
    return true;
}

// --- Welding (Weld, Array's and Mirror's merge): vertices within dist made one, faces that collapse dropped ---
static uint32_t* weld_map(const TMesh* m, float dist) {
    uint32_t* map = malloc(sizeof(uint32_t) * (m->nv + 1));
    uint32_t nb = 1;
    while (nb < m->nv * 2) nb <<= 1;
    int32_t* head = malloc(sizeof(int32_t) * nb);
    int32_t* next = malloc(sizeof(int32_t) * (m->nv + 1));
    if (!map || !head || !next) return free(map), free(head), free(next), NULL;
    for (uint32_t i = 0; i < nb; i++) head[i] = -1;
    float cell = dist > 1e-6f ? dist * 2.0f : 1e-5f;
    for (uint32_t v = 0; v < m->nv; v++) {
        const float* p = &m->co[v * 3];
        int c[3] = { (int)floorf(p[0] / cell), (int)floorf(p[1] / cell), (int)floorf(p[2] / cell) };
        map[v] = v;
        for (int dz = -1; dz <= 1 && map[v] == v; dz++)
            for (int dy = -1; dy <= 1 && map[v] == v; dy++)
                for (int dx = -1; dx <= 1 && map[v] == v; dx++) {
                    uint32_t h = ((uint32_t)(c[0] + dx) * 73856093u ^ (uint32_t)(c[1] + dy) * 19349663u ^ (uint32_t)(c[2] + dz) * 83492791u) & (nb - 1);
                    for (int32_t u = head[h]; u >= 0; u = next[u]) {
                        const float* q = &m->co[(uint32_t)u * 3];
                        float d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
                        if (d2 <= dist * dist) {
                            map[v] = map[u];
                            break;
                        }
                    }
                }
        uint32_t h = ((uint32_t)c[0] * 73856093u ^ (uint32_t)c[1] * 19349663u ^ (uint32_t)c[2] * 83492791u) & (nb - 1);
        next[v] = head[h], head[h] = (int32_t)v;
    }
    free(head), free(next);
    return map;
}

// the mesh with vertex v replaced by map[v] (unused vertices dropped, faces with fewer than 3 distinct corners too)
static bool tmesh_remap(TMesh* m, const uint32_t* map) {
    MB b;
    mb_init(&b, m);
    uint32_t* nv = malloc(sizeof(uint32_t) * (m->nv + 1));
    if (!nv) return false;
    for (uint32_t v = 0; v < m->nv; v++) nv[v] = UINT32_MAX;
    uint32_t vv[64];
    float uv[64][2];
    uint32_t col[64];
    for (uint32_t f = 0; f < m->nf; f++) {
        uint32_t st = m->fstart[f], n = m->fsize[f] < 64 ? m->fsize[f] : 64, k = 0;
        for (uint32_t c = 0; c < n; c++) {
            uint32_t v = map[m->loops[st + c]];
            if (k && vv[k - 1] == v) continue;
            if (nv[v] == UINT32_MAX) nv[v] = mb_copy(&b, m, v);
            vv[k] = v, uv[k][0] = m->uv[(st + c) * 2], uv[k][1] = m->uv[(st + c) * 2 + 1], col[k] = m->col ? m->col[st + c] : 0xFFFFFFFFu;
            k++;
        }
        if (k > 1 && vv[k - 1] == vv[0]) k--;
        if (k < 3) continue;
        for (uint32_t c = 0; c < k; c++) vv[c] = nv[vv[c]];
        mb_face(&b, vv, (const float(*)[2])uv, col, k, m->fmat[f]);
    }
    free(nv);
    return mb_finish(&b, m);
}

static uint32_t tmesh_weld(TMesh* m, float dist) {
    uint32_t* map = weld_map(m, dist);
    if (!map) return 0;
    uint32_t merged = 0;
    for (uint32_t v = 0; v < m->nv; v++) merged += map[v] != v;
    if (merged) tmesh_remap(m, map);
    free(map);
    return merged;
}

// --- The modifiers ---

// Blender's flip of a side name (Mirror's vertex groups): .L / .R, _L / _R, Left / Right, left / right
static bool flip_side_name(const char* in, char* out, size_t cap) {
    snprintf(out, cap, "%s", in);
    size_t n = strlen(out);
    if (n >= 2 && (out[n - 2] == '.' || out[n - 2] == '_' || out[n - 2] == '-' || out[n - 2] == ' ')) {
        char c = out[n - 1];
        if (c == 'L' || c == 'R' || c == 'l' || c == 'r') {
            out[n - 1] = c == 'L' ? 'R' : c == 'R' ? 'L' : c == 'l' ? 'r' : 'l';
            return true;
        }
    }
    static const char* const pairs[][2] = { { "Left", "Right" }, { "left", "right" }, { "LEFT", "RIGHT" } };
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < 2; s++) {
            const char* a = pairs[k][s];
            const char* at = strstr(in, a);
            if (!at) continue;
            snprintf(out, cap, "%.*s%s%s", (int)(at - in), in, pairs[k][!s], at + strlen(a));
            return true;
        }
    return false;
}

// Mirror: each axis in turn (X, Y, Z), the mesh and its mirror image (in the mirror object's space when it has one)
static bool mod_mirror(TFile* f, int oi, TMesh* m, const char* mp) {
    int ax[3] = { 1, 0, 0 };
    char v[64];
    if (mp_get(mp, "use_axis", v, sizeof v)) sscanf(v, "%d,%d,%d", &ax[0], &ax[1], &ax[2]);
    bool merge = mp_i(mp, "use_mirror_merge", 1) != 0, vgroups = mp_i(mp, "use_mirror_vertex_groups", 1) != 0;
    bool mu = mp_i(mp, "use_mirror_u", 0) != 0, mv = mp_i(mp, "use_mirror_v", 0) != 0;
    float dist = mp_f(mp, "merge_threshold", 0.001f), mou = mp_f(mp, "mirror_offset_u", 0), mov = mp_f(mp, "mirror_offset_v", 0);
    float ou = mp_f(mp, "offset_u", 0), ov = mp_f(mp, "offset_v", 0);
    int mobj = mp_i(mp, "mirror_object", -1);
    // the mirror's frame in the object's space (identity: its own axes)
    M4 P = m4_id(), Pi = m4_id();
    if (mobj >= 0 && mobj < f->nobjs && mobj != oi) {
        M4 ow = obj_world(f, oi, 0), mw = obj_world(f, mobj, 0), oinv = m4_inv(&ow);
        P = m4_mul(&oinv, &mw), Pi = m4_inv(&P);
    }
    int gmap[4096];
    for (uint32_t g = 0; g < m->ngroups && g < 4096; g++) { // each group takes its flipped side's weights in the copy
        gmap[g] = (int)g;
        char fl[64];
        if (vgroups && flip_side_name(m->gname[g], fl, sizeof fl))
            for (uint32_t h = 0; h < m->ngroups; h++)
                if (!strcmp(m->gname[h], fl)) gmap[g] = (int)h;
    }
    for (int a = 0; a < 3; a++) {
        if (!ax[a]) continue;
        M4 R = m4_id();
        R.m[a * 5] = -1.0f;
        M4 PR = m4_mul(&P, &R), X = m4_mul(&PR, &Pi); // the reflection in the object's space
        float M[9] = { X.m[0], X.m[4], X.m[8], X.m[1], X.m[5], X.m[9], X.m[2], X.m[6], X.m[10] }, t[3] = { X.m[12], X.m[13], X.m[14] };
        MB b;
        mb_init(&b, m);
        for (uint32_t i = 0; i < m->nv; i++) mb_copy(&b, m, i);
        uint32_t* cp = malloc(sizeof(uint32_t) * (m->nv + 1));
        if (!cp) return false;
        for (uint32_t i = 0; i < m->nv; i++) {
            float q[3];
            m4_point(&Pi, &m->co[i * 3], q); // (on the plane: merged with itself)
            float one = 1.0f;
            cp[i] = merge && fabsf(q[a]) * 2.0f <= dist ? i : mb_vert(&b, m, &i, &one, 1, M, t, m->ngroups <= 4096 ? gmap : NULL);
        }
        for (uint32_t fi = 0; fi < m->nf; fi++) // the original (its vertices copied first, in order)
            mb_face(&b, &m->loops[m->fstart[fi]], (const float(*)[2]) & m->uv[m->fstart[fi] * 2], m->col ? &m->col[m->fstart[fi]] : NULL,
                    m->fsize[fi], m->fmat[fi]);
        for (uint32_t fi = 0; fi < m->nf; fi++) { // the image: its corners reversed (the faces still point out)
            uint32_t st = m->fstart[fi], n = m->fsize[fi] < 64 ? m->fsize[fi] : 64, vv[64], cc[64];
            float uv[64][2];
            for (uint32_t c = 0; c < n; c++) {
                uint32_t k = st + (n - 1 - c);
                vv[c] = cp[m->loops[k]];
                uv[c][0] = (mu ? 1.0f - m->uv[k * 2] + mou : m->uv[k * 2]) + ou;
                uv[c][1] = (mv ? 1.0f - m->uv[k * 2 + 1] + mov : m->uv[k * 2 + 1]) + ov;
                cc[c] = m->col ? m->col[k] : 0xFFFFFFFFu;
            }
            mb_face(&b, vv, (const float(*)[2])uv, cc, n, m->fmat[fi]);
        }
        free(cp);
        if (!mb_finish(&b, m)) return false;
    }
    return true;
}

static void tmesh_bounds(const TMesh* m, float mn[3], float mx[3]) {
    for (int k = 0; k < 3; k++) mn[k] = 1e30f, mx[k] = -1e30f;
    for (uint32_t v = 0; v < m->nv; v++)
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], m->co[v * 3 + k]), mx[k] = fmaxf(mx[k], m->co[v * 3 + k]);
    if (!m->nv) for (int k = 0; k < 3; k++) mn[k] = mx[k] = 0.0f;
}

// Array: copies offset by the bounds (relative), a distance (constant) and another object's transform (object), merged
static bool mod_array(TFile* f, int oi, TMesh* m, const char* mp, const char* owner) {
    int fit = mp_enum(mp, "fit_type", "FIXED_COUNT,FIT_LENGTH,FIT_CURVE", 0);
    int count = mp_i(mp, "count", 2);
    bool rel = mp_i(mp, "use_relative_offset", 1) != 0, cst = mp_i(mp, "use_constant_offset", 0) != 0, obj = mp_i(mp, "use_object_offset", 0) != 0;
    float r[3], c[3];
    mp_v3(mp, "relative_offset_displace", r, 1, 0, 0);
    mp_v3(mp, "constant_offset_displace", c, 0.1f, 0, 0);
    bool merge = mp_i(mp, "use_merge_vertices", 0) != 0;
    float dist = mp_f(mp, "merge_threshold", 0.01f), ou = mp_f(mp, "offset_u", 0), ov = mp_f(mp, "offset_v", 0);
    int oobj = mp_i(mp, "offset_object", -1);
    float mn[3], mx[3], off[3] = { 0, 0, 0 };
    tmesh_bounds(m, mn, mx);
    for (int k = 0; k < 3; k++) off[k] = (rel ? r[k] * (mx[k] - mn[k]) : 0.0f) + (cst ? c[k] : 0.0f);
    M4 step = m4_id();
    if (obj && oobj >= 0 && oobj < f->nobjs && oobj != oi) {
        M4 ow = obj_world(f, oi, 0), xw = obj_world(f, oobj, 0), oinv = m4_inv(&ow);
        step = m4_mul(&oinv, &xw);
    }
    if (fit == 1) { // Fit Length: as many as fit
        float len = mp_f(mp, "fit_length", 0.0f), ol = sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
        count = ol > 1e-6f ? (int)floorf(len / ol + 1e-4f) + 1 : 1;
    } else if (fit == 2)
        imprep_add(IMPREP_LOST, IMPREP_CAT_MODIFIER, owner, "Array", TR("Fit Curve: the curve's length is not known here, the Count was used"));
    if (mp_i(mp, "start_cap", -1) >= 0 || mp_i(mp, "end_cap", -1) >= 0)
        imprep_add(IMPREP_LOST, IMPREP_CAT_MODIFIER, owner, "Array", TR("its start and end caps were not added"));
    if (count < 1) count = 1;
    if ((size_t)count * m->nv > 8000000u) count = (int)(8000000u / (m->nv ? m->nv : 1));
    MB b;
    mb_init(&b, m);
    M4 acc = m4_id();
    uint32_t* map = malloc(sizeof(uint32_t) * (m->nv + 1));
    if (!map) return false;
    for (int k = 0; k < count; k++) {
        // copy k: acc (the object offset k times) then k times the offsets
        float M[9] = { acc.m[0], acc.m[4], acc.m[8], acc.m[1], acc.m[5], acc.m[9], acc.m[2], acc.m[6], acc.m[10] };
        float t[3] = { acc.m[12] + off[0] * k, acc.m[13] + off[1] * k, acc.m[14] + off[2] * k };
        float one = 1.0f;
        for (uint32_t i = 0; i < m->nv; i++) map[i] = mb_vert(&b, m, &i, &one, 1, M, t, NULL);
        for (uint32_t fi = 0; fi < m->nf; fi++) {
            uint32_t st = m->fstart[fi], n = m->fsize[fi] < 64 ? m->fsize[fi] : 64, vv[64];
            float uv[64][2];
            for (uint32_t q = 0; q < n; q++)
                vv[q] = map[m->loops[st + q]], uv[q][0] = m->uv[(st + q) * 2] + ou * k, uv[q][1] = m->uv[(st + q) * 2 + 1] + ov * k;
            mb_face(&b, vv, (const float(*)[2])uv, m->col ? &m->col[st] : NULL, n, m->fmat[fi]);
        }
        acc = m4_mul(&acc, &step);
    }
    free(map);
    if (!mb_finish(&b, m)) return false;
    if (merge) tmesh_weld(m, dist);
    return true;
}

// a vertex's normal (the faces' areas), Blender's space
static float* tmesh_vnormals(const TMesh* m) {
    float* vn = calloc((size_t)m->nv * 3 + 3, sizeof(float));
    if (!vn) return NULL;
    for (uint32_t k = 0; k < m->nf; k++) {
        uint32_t s = m->fstart[k], n = m->fsize[k];
        for (uint32_t c = 1; c + 1 < n; c++) {
            const float *a = &m->co[m->loops[s] * 3], *b = &m->co[m->loops[s + c] * 3], *d = &m->co[m->loops[s + c + 1] * 3];
            float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] }, cr[3];
            v3cross(e1, e2, cr);
            for (int q = 0; q < 3; q++) vn[m->loops[s] * 3 + q] += cr[q], vn[m->loops[s + c] * 3 + q] += cr[q], vn[m->loops[s + c + 1] * 3 + q] += cr[q];
        }
    }
    for (uint32_t v = 0; v < m->nv; v++) v3norm(&vn[v * 3]);
    return vn;
}

// an edge key (the smaller vertex in the high bits)
static uint64_t edge_key(uint32_t a, uint32_t b) { return a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a; }

// The edges of a mesh: a hash of edge -> its index, the faces on each (up to 2 kept, the count), each edge's vertices
typedef struct Edges {
    uint64_t* key;
    int32_t* slot; // hash table: edge index (-1 empty)
    uint32_t nslot, n;
    uint32_t (*v)[2];
    uint32_t (*face)[2];
    uint32_t* nface;
} Edges;
static void edges_free(Edges* e) { free(e->key), free(e->slot), free(e->v), free(e->face), free(e->nface), memset(e, 0, sizeof *e); }
static int32_t edges_find(const Edges* e, uint32_t a, uint32_t b) {
    uint64_t k = edge_key(a, b);
    for (uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (e->nslot - 1);; h = (h + 1) & (e->nslot - 1)) {
        if (e->slot[h] < 0) return -1;
        if (e->key[e->slot[h]] == k) return e->slot[h];
    }
}
static bool edges_build(const TMesh* m, Edges* e) {
    memset(e, 0, sizeof *e);
    uint32_t cap = m->ncorners + 1;
    e->nslot = 1;
    while (e->nslot < cap * 2) e->nslot <<= 1;
    e->slot = malloc(sizeof(int32_t) * e->nslot), e->key = malloc(sizeof(uint64_t) * cap), e->v = malloc(sizeof *e->v * cap);
    e->face = malloc(sizeof *e->face * cap), e->nface = calloc(cap, sizeof(uint32_t));
    if (!e->slot || !e->key || !e->v || !e->face || !e->nface) return edges_free(e), false;
    for (uint32_t i = 0; i < e->nslot; i++) e->slot[i] = -1;
    for (uint32_t f = 0; f < m->nf; f++)
        for (uint32_t c = 0; c < m->fsize[f]; c++) {
            uint32_t a = m->loops[m->fstart[f] + c], b = m->loops[m->fstart[f] + (c + 1) % m->fsize[f]];
            uint64_t k = edge_key(a, b);
            uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (e->nslot - 1);
            while (e->slot[h] >= 0 && e->key[e->slot[h]] != k) h = (h + 1) & (e->nslot - 1);
            if (e->slot[h] < 0) {
                e->slot[h] = (int32_t)e->n, e->key[e->n] = k, e->v[e->n][0] = a, e->v[e->n][1] = b, e->n++;
            }
            uint32_t ei = (uint32_t)e->slot[h];
            if (e->nface[ei] < 2) e->face[ei][e->nface[ei]] = f;
            e->nface[ei]++;
        }
    return true;
}

// Subdivision Surface: Catmull-Clark (Simple: the same points, only split) a level at a time; each new point a weighted
// sum of the old ones (so the groups and shape keys follow); the UVs and colors split linearly
static bool subdivide_once(TMesh* m, bool simple, bool keep_corners) {
    Edges e;
    if (!edges_build(m, &e)) return false;
    // (2026-11-19) the creases (sharpness per edge, OpenSubdiv's: a semi-sharp edge blends its smooth and sharp points,
    // its children 1 less: Uniform creasing) and the sharp edges (carried to the children)
    float* es = m->ncrease ? calloc((size_t)e.n + 1, sizeof(float)) : NULL;
    uint8_t* esh = m->nsharp ? calloc((size_t)e.n + 1, 1) : NULL;
    for (uint32_t k = 0; es && k < m->ncrease; k++) {
        int32_t ei = edges_find(&e, m->crease_e[k][0], m->crease_e[k][1]);
        if (ei >= 0) es[ei] = fmaxf(es[ei], m->crease_s[k]);
    }
    for (uint32_t k = 0; esh && k < m->nsharp; k++) {
        int32_t ei = edges_find(&e, m->sharp_e[k][0], m->sharp_e[k][1]);
        if (ei >= 0) esh[ei] = 1;
    }
    // per vertex: its creased edges (the vertex rule: Smooth with 0-1, Crease with 2, Corner with more; those over 1
    // decide the children's rule, the ones at most 1 its blend)
    typedef struct { uint8_t k, kc, tn; float ts; uint32_t pn[2], cn[2]; } VCr;
    VCr* vc = es && !simple ? calloc((size_t)m->nv + 1, sizeof(VCr)) : NULL;
    for (uint32_t i = 0; vc && i < e.n; i++) {
        float sv = es[i];
        if (sv <= 0.0f) continue;
        for (int s2 = 0; s2 < 2; s2++) {
            VCr* c = &vc[e.v[i][s2]];
            uint32_t other = e.v[i][1 - s2];
            if (c->k < 2) c->pn[c->k] = other;
            if (c->k < 255) c->k++;
            if (sv > 1.0f) {
                if (c->kc < 2) c->cn[c->kc] = other;
                if (c->kc < 255) c->kc++;
            } else if (c->tn < 255) c->ts += sv, c->tn++;
        }
    }
    // the old vertices' edges (valence, boundary neighbors)
    uint32_t* val = calloc((size_t)m->nv + 1, sizeof(uint32_t));
    uint32_t* nbnd = calloc((size_t)m->nv + 1, sizeof(uint32_t));
    uint32_t(*bnb)[2] = calloc((size_t)m->nv + 1, sizeof *bnb);
    uint32_t* vfaces = calloc((size_t)m->nv + 1, sizeof(uint32_t));
    MB b;
    mb_init(&b, m);
    bool ok = val && nbnd && bnb && vfaces;
    for (uint32_t i = 0; ok && i < e.n; i++) {
        uint32_t a = e.v[i][0], c = e.v[i][1];
        val[a]++, val[c]++;
        if (e.nface[i] == 1) {
            if (nbnd[a] < 2) bnb[a][nbnd[a]] = c;
            if (nbnd[c] < 2) bnb[c][nbnd[c]] = a;
            nbnd[a]++, nbnd[c]++;
        }
    }
    for (uint32_t f = 0; ok && f < m->nf; f++)
        for (uint32_t c = 0; c < m->fsize[f]; c++) vfaces[m->loops[m->fstart[f] + c]]++;
    // scratch for the weights (a vertex point: its faces' and edges' vertices)
    uint32_t wcap = 4096, *wi = malloc(sizeof(uint32_t) * wcap);
    float* ww = malloc(sizeof(float) * wcap);
    ok = ok && wi && ww;
    // per vertex, its faces (for the vertex points): a CSR list
    uint32_t* vf0 = calloc((size_t)m->nv + 2, sizeof(uint32_t));
    uint32_t* vfl = malloc(sizeof(uint32_t) * (m->ncorners + 1));
    ok = ok && vf0 && vfl;
    for (uint32_t v = 0; ok && v < m->nv; v++) vf0[v + 1] = vf0[v] + vfaces[v];
    uint32_t* fill = ok ? calloc((size_t)m->nv + 1, sizeof(uint32_t)) : NULL;
    ok = ok && fill;
    for (uint32_t f = 0; ok && f < m->nf; f++)
        for (uint32_t c = 0; c < m->fsize[f]; c++) {
            uint32_t v = m->loops[m->fstart[f] + c];
            vfl[vf0[v] + fill[v]++] = f;
        }
    free(fill);
    uint32_t *fp = ok ? malloc(sizeof(uint32_t) * (m->nf + 1)) : NULL, *ep = ok ? malloc(sizeof(uint32_t) * (e.n + 1)) : NULL,
             *vp = ok ? malloc(sizeof(uint32_t) * (m->nv + 1)) : NULL;
    ok = ok && fp && ep && vp;
#define WPUT(idx, wt)                                                                                                      \
    do {                                                                                                                   \
        if (nw == wcap) {                                                                                                  \
            wcap *= 2;                                                                                                     \
            uint32_t* a_ = realloc(wi, sizeof(uint32_t) * wcap);                                                           \
            float* b_ = realloc(ww, sizeof(float) * wcap);                                                                 \
            if (!a_ || !b_) {                                                                                              \
                ok = false;                                                                                                \
                if (a_) wi = a_;                                                                                           \
                if (b_) ww = b_;                                                                                           \
                break;                                                                                                     \
            }                                                                                                              \
            wi = a_, ww = b_;                                                                                              \
        }                                                                                                                  \
        wi[nw] = (idx), ww[nw] = (wt), nw++;                                                                               \
    } while (0)
    // face points
    for (uint32_t f = 0; ok && f < m->nf; f++) {
        uint32_t nw = 0, n = m->fsize[f];
        for (uint32_t c = 0; c < n; c++) WPUT(m->loops[m->fstart[f] + c], 1.0f / (float)n);
        fp[f] = mb_vert(&b, m, wi, ww, (int)nw, NULL, NULL, NULL);
    }
    // edge points: the midpoint (boundary, simple, or more than 2 faces), else (v0 + v1 + F1 + F2) / 4
    for (uint32_t i = 0; ok && i < e.n; i++) {
        uint32_t nw = 0;
        float sv = es ? fminf(es[i], 1.0f) : 0.0f; // (a crease: sharp at 1 and over, a blend below)
        if (simple || e.nface[i] != 2 || sv >= 1.0f) {
            WPUT(e.v[i][0], 0.5f);
            WPUT(e.v[i][1], 0.5f);
        } else {
            WPUT(e.v[i][0], 0.25f + 0.25f * sv);
            WPUT(e.v[i][1], 0.25f + 0.25f * sv);
            for (int s = 0; s < 2; s++) {
                uint32_t f = e.face[i][s], n = m->fsize[f];
                for (uint32_t c = 0; c < n; c++) WPUT(m->loops[m->fstart[f] + c], 0.25f * (1.0f - sv) / (float)n);
            }
        }
        ep[i] = mb_vert(&b, m, wi, ww, (int)nw, NULL, NULL, NULL);
    }
    // vertex points: interior (Q + 2R + (n - 3)P) / n; boundary 3/4 P + 1/8 each boundary neighbor; corners and
    // non-manifold ones stay
    for (uint32_t v = 0; ok && v < m->nv; v++) {
        uint32_t nw = 0, n = val[v];
        if (simple || !n) WPUT(v, 1.0f);
        else if (nbnd[v]) {
            if (nbnd[v] == 2 && !(keep_corners && n == 2)) {
                WPUT(v, 0.75f);
                WPUT(bnb[v][0], 0.125f);
                WPUT(bnb[v][1], 0.125f);
            } else
                WPUT(v, 1.0f);
        } else {
            // the rule by the creased edges (none: Smooth); a rule changing at the children: both, by the fractional
            // edges' mean sharpness (the Tlender's vertex_point)
            const VCr* c = vc ? &vc[v] : NULL;
            int prule = !c || c->k < 2 ? 0 : c->k == 2 ? 1 : 2, crule = !c || c->kc < 2 ? 0 : c->kc == 2 ? 1 : 2;
            float frac = c && c->tn ? c->ts / (float)c->tn : 0.0f;
            int rules[2] = { prule, crule };
            float fac[2] = { 1.0f, 0.0f };
            if (prule != 0 && crule != prule) fac[0] = frac, fac[1] = 1.0f - frac;
            for (int r = 0; r < 2 && ok; r++) {
                float k = fac[r];
                if (k <= 0.0f) continue;
                if (rules[r] == 1) { // Crease: 6/8 itself, 1/8 each crease neighbor
                    const uint32_t* nb = r == 0 ? c->pn : c->cn;
                    WPUT(v, 0.75f * k);
                    WPUT(nb[0], 0.125f * k);
                    WPUT(nb[1], 0.125f * k);
                    continue;
                }
                if (rules[r] == 2) { // Corner: stays
                    WPUT(v, k);
                    continue;
                }
                float fn = (float)n;
                uint32_t nf = vf0[v + 1] - vf0[v];
                for (uint32_t q = vf0[v]; q < vf0[v + 1] && ok; q++) { // Q: the faces' points' mean, / n
                    uint32_t f = vfl[q], sz = m->fsize[f];
                    for (uint32_t cc = 0; cc < sz; cc++) WPUT(m->loops[m->fstart[f] + cc], k / (fn * (float)nf * (float)sz));
                }
                // 2R / n: the edges' midpoints' mean (each edge of v: (v + other) / 2)
                for (uint32_t q = vf0[v]; q < vf0[v + 1] && ok; q++) {
                    uint32_t f = vfl[q], sz = m->fsize[f], st = m->fstart[f];
                    for (uint32_t cc = 0; cc < sz; cc++) // (each edge of v is in 2 of its faces: half from each)
                        if (m->loops[st + cc] == v) {
                            uint32_t nb2[2] = { m->loops[st + (cc + 1) % sz], m->loops[st + (cc + sz - 1) % sz] };
                            for (int s = 0; s < 2; s++) {
                                WPUT(v, k * 0.5f * 2.0f / (fn * fn) * 0.5f);
                                WPUT(nb2[s], k * 0.5f * 2.0f / (fn * fn) * 0.5f);
                            }
                        }
                }
                WPUT(v, k * (fn - 3.0f) / fn);
            }
        }
        vp[v] = mb_vert(&b, m, wi, ww, (int)nw, NULL, NULL, NULL);
    }
#undef WPUT
    // the faces: an n-gon into n quads (v_i, edge i, the face point, edge i-1)
    for (uint32_t f = 0; ok && f < m->nf; f++) {
        uint32_t st = m->fstart[f], n = m->fsize[f];
        float cu[2] = { 0, 0 };
        float ca[4] = { 0, 0, 0, 0 };
        for (uint32_t c = 0; c < n; c++) {
            cu[0] += m->uv[(st + c) * 2] / (float)n, cu[1] += m->uv[(st + c) * 2 + 1] / (float)n;
            uint32_t col = m->col ? m->col[st + c] : 0xFFFFFFFFu;
            for (int k = 0; k < 4; k++) ca[k] += (float)((col >> (k * 8)) & 255u) / (float)n;
        }
        uint32_t cmid = (uint32_t)(ca[0] + 0.5f) | (uint32_t)(ca[1] + 0.5f) << 8 | (uint32_t)(ca[2] + 0.5f) << 16 | (uint32_t)(ca[3] + 0.5f) << 24;
        for (uint32_t c = 0; c < n; c++) {
            uint32_t cn = (c + 1) % n, cp = (c + n - 1) % n;
            uint32_t a = m->loops[st + c];
            int32_t en = edges_find(&e, a, m->loops[st + cn]), ep2 = edges_find(&e, m->loops[st + cp], a);
            if (en < 0 || ep2 < 0) continue;
            uint32_t q[4] = { vp[a], ep[en], fp[f], ep[ep2] };
            float uv[4][2] = { { m->uv[(st + c) * 2], m->uv[(st + c) * 2 + 1] },
                               { (m->uv[(st + c) * 2] + m->uv[(st + cn) * 2]) * 0.5f, (m->uv[(st + c) * 2 + 1] + m->uv[(st + cn) * 2 + 1]) * 0.5f },
                               { cu[0], cu[1] },
                               { (m->uv[(st + c) * 2] + m->uv[(st + cp) * 2]) * 0.5f, (m->uv[(st + c) * 2 + 1] + m->uv[(st + cp) * 2 + 1]) * 0.5f } };
            uint32_t col[4] = { m->col ? m->col[st + c] : 0xFFFFFFFFu, m->col ? m->col[st + cn] : 0xFFFFFFFFu, cmid, m->col ? m->col[st + cp] : 0xFFFFFFFFu };
            mb_face(&b, q, (const float(*)[2])uv, col, 4, m->fmat[f]);
        }
    }
    // the children's marks: an edge's two halves (v0, its edge point) and (its edge point, v1), a crease 1 less sharp
    uint32_t nce = 0, nsh = 0;
    uint32_t(*ce)[2] = NULL, (*she)[2] = NULL;
    float* cs = NULL;
    for (uint32_t i = 0; ok && es && i < e.n; i++) nce += es[i] > 1.0f ? 2u : 0u;
    for (uint32_t i = 0; ok && esh && i < e.n; i++) nsh += esh[i] ? 2u : 0u;
    if (nce && (ce = malloc(sizeof *ce * nce)) && (cs = malloc(sizeof(float) * nce))) {
        nce = 0;
        for (uint32_t i = 0; i < e.n; i++)
            if (es[i] > 1.0f)
                for (int s = 0; s < 2; s++) ce[nce][0] = vp[e.v[i][s]], ce[nce][1] = ep[i], cs[nce++] = es[i] - 1.0f;
    } else free(ce), ce = NULL, nce = 0;
    if (nsh && (she = malloc(sizeof *she * nsh))) {
        nsh = 0;
        for (uint32_t i = 0; i < e.n; i++)
            if (esh[i])
                for (int s = 0; s < 2; s++) she[nsh][0] = vp[e.v[i][s]], she[nsh][1] = ep[i], nsh++;
    } else nsh = 0;
    free(val), free(nbnd), free(bnb), free(vfaces), free(wi), free(ww), free(vf0), free(vfl), free(fp), free(ep), free(vp);
    free(es), free(esh), free(vc);
    edges_free(&e);
    if (!ok) b.fail = true;
    if (!mb_finish(&b, m)) return free(ce), free(cs), free(she), false;
    m->crease_e = ce, m->crease_s = cs, m->ncrease = nce, m->sharp_e = she, m->nsharp = nsh;
    return true;
}

// Solidify (Simple): a second shell along the vertex normals, the rim closing the open edges
static bool mod_solidify(TMesh* m, const char* mp, const char* owner) {
    float t = mp_f(mp, "thickness", 0.01f), off = mp_f(mp, "offset", -1.0f);
    bool rim = mp_i(mp, "use_rim", 1) != 0, flip = mp_i(mp, "use_flip_normals", 0) != 0;
    int mo = mp_i(mp, "material_offset", 0), mor = mp_i(mp, "material_offset_rim", 0);
    if (mp_i(mp, "use_even_offset", 0) || mp_i(mp, "use_rim_only", 0))
        imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, owner, "Solidify", TR("Even Thickness / Only Rim: the plain shell (the thickness may differ at sharp corners)"));
    float ofs_orig = -((1.0f - off) * 0.5f) * t, ofs_new = ofs_orig + t;
    float* vn = tmesh_vnormals(m);
    if (!vn) return false;
    MB b;
    mb_init(&b, m);
    float one = 1.0f;
    uint32_t* cp = malloc(sizeof(uint32_t) * (m->nv + 1));
    uint32_t* org = malloc(sizeof(uint32_t) * (m->nv + 1));
    if (!cp || !org) return free(vn), free(cp), free(org), false;
    for (uint32_t i = 0; i < m->nv; i++) {
        float ta[3] = { vn[i * 3] * ofs_orig, vn[i * 3 + 1] * ofs_orig, vn[i * 3 + 2] * ofs_orig };
        float tb[3] = { vn[i * 3] * ofs_new, vn[i * 3 + 1] * ofs_new, vn[i * 3 + 2] * ofs_new };
        org[i] = mb_vert(&b, m, &i, &one, 1, NULL, ta, NULL);
        cp[i] = mb_vert(&b, m, &i, &one, 1, NULL, tb, NULL);
    }
    // the shell further out keeps the winding (faces out), the other one reversed; Flip Normals turns both
    bool new_out = ofs_new >= ofs_orig;
    for (int shell = 0; shell < 2; shell++) {
        const uint32_t* map = shell ? cp : org;
        bool reversed = ((shell == 1) != new_out) != flip;
        for (uint32_t fi = 0; fi < m->nf; fi++) {
            uint32_t st = m->fstart[fi], n = m->fsize[fi] < 64 ? m->fsize[fi] : 64, vv[64], cc[64];
            float uv[64][2];
            for (uint32_t c = 0; c < n; c++) {
                uint32_t k = reversed ? st + (n - 1 - c) : st + c;
                vv[c] = map[m->loops[k]], uv[c][0] = m->uv[k * 2], uv[c][1] = m->uv[k * 2 + 1], cc[c] = m->col ? m->col[k] : 0xFFFFFFFFu;
            }
            mb_face(&b, vv, (const float(*)[2])uv, cc, n, (uint16_t)(m->fmat[fi] + (shell ? mo : 0)));
        }
    }
    if (rim) { // a quad per open edge, wound as the face it borders
        Edges e;
        if (edges_build(m, &e)) {
            for (uint32_t fi = 0; fi < m->nf; fi++) {
                uint32_t st = m->fstart[fi], n = m->fsize[fi];
                for (uint32_t c = 0; c < n; c++) {
                    uint32_t a = m->loops[st + c], d = m->loops[st + (c + 1) % n];
                    int32_t ei = edges_find(&e, a, d);
                    if (ei < 0 || e.nface[ei] != 1) continue;
                    uint32_t q[4] = { org[d], org[a], cp[a], cp[d] };
                    if (new_out == flip) q[0] = org[a], q[1] = org[d], q[2] = cp[d], q[3] = cp[a];
                    float uv[4][2] = { { m->uv[(st + c) * 2], m->uv[(st + c) * 2 + 1] }, { m->uv[(st + c) * 2], m->uv[(st + c) * 2 + 1] },
                                       { m->uv[(st + c) * 2], m->uv[(st + c) * 2 + 1] }, { m->uv[(st + c) * 2], m->uv[(st + c) * 2 + 1] } };
                    mb_face(&b, q, (const float(*)[2])uv, NULL, 4, (uint16_t)(m->fmat[fi] + mor));
                }
            }
            edges_free(&e);
        }
    }
    free(vn), free(cp), free(org);
    return mb_finish(&b, m);
}

static int tmesh_group(const TMesh* m, const char* name) {
    for (uint32_t g = 0; name && name[0] && g < m->ngroups; g++)
        if (!strcmp(m->gname[g], name)) return (int)g;
    return -1;
}

// Displace (no texture in the Tlender's): (1 - Midlevel) x Strength along X, Y, Z or the normal, by the group's weights
static bool mod_displace(TFile* f, int oi, TMesh* m, const char* mp) {
    int dir = mp_enum(mp, "direction", "X,Y,Z,NORMAL", 3), glob = mp_enum(mp, "space", "LOCAL,GLOBAL", 0);
    float d = (1.0f - mp_f(mp, "mid_level", 0.5f)) * mp_f(mp, "strength", 1.0f);
    char vg[64] = "";
    mp_get(mp, "vertex_group", vg, sizeof vg);
    int g = tmesh_group(m, vg);
    bool inv = mp_i(mp, "invert_vertex_group", 0) != 0;
    float axis[3] = { dir == 0 ? 1.0f : 0.0f, dir == 1 ? 1.0f : 0.0f, dir == 2 ? 1.0f : 0.0f };
    if (glob && dir < 3) { // a world axis in the object's space
        M4 ow = obj_world(f, oi, 0), oi4 = m4_inv(&ow);
        float a2[3];
        m4_dir(&oi4, axis, a2);
        memcpy(axis, a2, 12);
    }
    float* vn = dir == 3 ? tmesh_vnormals(m) : NULL;
    if (dir == 3 && !vn) return false;
    for (uint32_t v = 0; v < m->nv; v++) {
        float w = g >= 0 && m->gweight ? m->gweight[(size_t)g * m->nv + v] : 1.0f;
        if (g >= 0 && inv) w = 1.0f - w;
        const float* a = vn ? &vn[v * 3] : axis;
        for (int k = 0; k < 3; k++) m->co[v * 3 + k] += a[k] * d * w;
    }
    free(vn);
    return true;
}

// Mask (Vertex Group): the faces whose vertices are all in the group (weight at or over the threshold; Invert: under)
static bool mod_mask(TMesh* m, const char* mp, const char* owner) {
    if (mp_enum(mp, "mode", "VERTEX_GROUP,ARMATURE", 0) != 0) {
        imprep_add(IMPREP_LOST, IMPREP_CAT_MODIFIER, owner, "Mask", TR("the Armature mode (the selected bones' groups) is not applied: the whole mesh came in"));
        return false;
    }
    char vg[64] = "";
    mp_get(mp, "vertex_group", vg, sizeof vg);
    int g = tmesh_group(m, vg);
    if (g < 0 || !m->gweight) return false;
    float th = mp_f(mp, "threshold", 0.0f);
    bool inv = mp_i(mp, "invert_vertex_group", 0) != 0;
    MB b;
    mb_init(&b, m);
    uint32_t* nv = malloc(sizeof(uint32_t) * (m->nv + 1));
    if (!nv) return false;
    for (uint32_t v = 0; v < m->nv; v++) nv[v] = UINT32_MAX;
    for (uint32_t fi = 0; fi < m->nf; fi++) {
        uint32_t st = m->fstart[fi], n = m->fsize[fi] < 64 ? m->fsize[fi] : 64, vv[64];
        bool keep = true;
        for (uint32_t c = 0; c < n && keep; c++) {
            float w = m->gweight[(size_t)g * m->nv + m->loops[st + c]];
            keep = inv ? !(w > th || (th == 0.0f && w > 0.0f)) : (w > th || (th == 0.0f && w > 0.0f));
        }
        if (!keep) continue;
        for (uint32_t c = 0; c < n; c++) {
            uint32_t v = m->loops[st + c];
            if (nv[v] == UINT32_MAX) nv[v] = mb_copy(&b, m, v);
            vv[c] = nv[v];
        }
        mb_face(&b, vv, (const float(*)[2]) & m->uv[st * 2], m->col ? &m->col[st] : NULL, n, m->fmat[fi]);
    }
    free(nv);
    return mb_finish(&b, m);
}

// The Tlender's Nonlinear deformer (Bend, Twist, Flare, Squash, Sine, Wave: its mod_nonlinear.c), in its handle's space
static void nl_point(int mode, const float* p, float lo, float hi, float* pa, float* pb, float* pc) {
    // p: curvature, start_angle, end_angle, start_flare_b, start_flare_c, end_flare_b, end_flare_c, curve, factor,
    // expand, max_expand_pos, amplitude, wavelength, offset, dropoff
    float a = *pa, b = *pb, c = *pc, tc = a < lo ? lo : a > hi ? hi : a, u = hi - lo > 1e-6f ? (tc - lo) / (hi - lo) : 0.0f;
    float drop = p[14];
#define NL_DROP(e) (drop >= 0.0f ? 1.0f - drop * (e) : 1.0f + drop * (1.0f - (e)))
    switch (mode) {
    case 0: { // Bend
        float k = p[0] / (hi - lo);
        if (fabsf(k) < 1e-6f) return;
        float th = k * tc, r = 1.0f / k, sn = sinf(th), cs = cosf(th), rest = a - tc;
        *pa = (r - b) * sn + rest * cs, *pb = r - (r - b) * cs + rest * sn;
        return;
    }
    case 1: { // Twist
        float ang = p[1] + (p[2] - p[1]) * u, sn = sinf(ang), cs = cosf(ang);
        *pb = b * cs - c * sn, *pc = b * sn + c * cs;
        return;
    }
    case 2: { // Flare
        float bulge = p[7] * sinf(3.14159265f * u);
        *pb = b * (p[3] + (p[5] - p[3]) * u + bulge), *pc = c * (p[4] + (p[6] - p[4]) * u + bulge);
        return;
    }
    case 3: { // Squash
        float sa = p[8] >= 0.0f ? 1.0f + p[8] : 1.0f / (1.0f - p[8]);
        float mm = p[10] < 0.01f ? 0.01f : p[10] > 0.99f ? 0.99f : p[10];
        float d = u < mm ? (u - mm) / mm : (u - mm) / (1.0f - mm);
        float sc = 1.0f + p[9] * (1.0f - d * d) * (1.0f / sqrtf(sa) - 1.0f);
        *pa = tc * sa + (a - tc), *pb = b * sc, *pc = c * sc;
        return;
    }
    case 4: { // Sine
        float wl = fabsf(p[12]) < 1e-6f ? 1e-6f : p[12];
        *pb = b + p[11] * NL_DROP(fabsf(2.0f * u - 1.0f)) * sinf(2.0f * 3.14159265f * (tc / wl + p[13]));
        return;
    }
    default: { // Wave
        float rlo = lo > 0.0f ? lo : 0.0f, r = sqrtf(b * b + c * c);
        if (hi - rlo < 1e-6f) return;
        float rc = r < rlo ? rlo : r > hi ? hi : r, wl = fabsf(p[12]) < 1e-6f ? 1e-6f : p[12];
        *pa = a + p[11] * NL_DROP((rc - rlo) / (hi - rlo)) * sinf(2.0f * 3.14159265f * (rc / wl + p[13]));
        return;
    }
    }
#undef NL_DROP
}
static bool mod_nonlinear(TFile* f, int oi, TMesh* m, const char* mp) {
    int mode = mp_enum(mp, "mode", "BEND,TWIST,FLARE,SQUASH,SINE,WAVE", 0), axis = mp_enum(mp, "deform_axis", "X,Y,Z", 2);
    float lo = mp_f(mp, "low_bound", -1.0f), hi = mp_f(mp, "high_bound", 1.0f), env = mp_f(mp, "envelope", 1.0f);
    static const char* const keys[15] = { "curvature", "start_angle", "end_angle", "start_flare_b", "start_flare_c", "end_flare_b", "end_flare_c",
                                          "curve", "factor", "expand", "max_expand_position", "amplitude", "wavelength", "offset", "dropoff" };
    static const float defs[15] = { 1.5707963f, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0.5f, 0.1f, 1, 0, 0 };
    float p[15];
    for (int k = 0; k < 15; k++) p[k] = mp_f(mp, keys[k], defs[k]);
    if (env == 0.0f || (mode != 5 && hi - lo < 1e-6f)) return false;
    char vg[64] = "";
    mp_get(mp, "vertex_group", vg, sizeof vg);
    int g = tmesh_group(m, vg);
    bool inv = mp_i(mp, "invert_vertex_group", 0) != 0;
    M4 H = m4_id(); // the handle's frame in the object's space
    int ho = mp_i(mp, "origin", -1);
    if (ho >= 0 && ho < f->nobjs && ho != oi) {
        M4 ow = obj_world(f, oi, 0), hw = obj_world(f, ho, 0), oinv = m4_inv(&ow);
        H = m4_mul(&oinv, &hw);
    }
    M4 Hi = m4_inv(&H);
    int ia = axis % 3, ib = (ia + 1) % 3, ic = (ia + 2) % 3;
    for (uint32_t v = 0; v < m->nv; v++) {
        float w = env * (g >= 0 && m->gweight ? (inv ? 1.0f - m->gweight[(size_t)g * m->nv + v] : m->gweight[(size_t)g * m->nv + v]) : 1.0f);
        if (w == 0.0f) continue;
        float q[3], r[3];
        m4_point(&Hi, &m->co[v * 3], q);
        nl_point(mode, p, lo, hi, &q[ia], &q[ib], &q[ic]);
        m4_point(&H, q, r);
        for (int k = 0; k < 3; k++) m->co[v * 3 + k] += (r[k] - m->co[v * 3 + k]) * w;
    }
    return true;
}

// Decimate (Collapse): the triangles simplified by meshoptimizer to the ratio (UV seams and borders kept: the corners'
// vertices split by UV, the border locked); Un-Subdivide and Planar keep the mesh as it is (the same look, more faces)
static bool mod_decimate(TMesh* m, const char* mp, const char* owner) {
    int mode = mp_enum(mp, "decimate_type", "COLLAPSE,UNSUBDIV,DISSOLVE", 0);
    if (mode != 0) {
        imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, owner, "Decimate", TR("Un-Subdivide / Planar: the mesh kept as it is (the same look with more faces; the Engine makes its own LODs)"));
        return false;
    }
    float ratio = mp_f(mp, "ratio", 1.0f);
    if (ratio >= 0.999f) return false;
    // corners -> vertices split by (vertex, UV)
    uint32_t ntri = 0;
    for (uint32_t f = 0; f < m->nf; f++) ntri += m->fsize[f] >= 3 ? m->fsize[f] - 2 : 0;
    uint32_t* idx = malloc(sizeof(uint32_t) * ntri * 3 + 4);
    uint32_t* corner_of = malloc(sizeof(uint32_t) * ntri * 3 + 4);
    uint16_t* tmat = malloc(sizeof(uint16_t) * ntri + 2);
    float* pos = malloc(sizeof(float) * 3 * (m->ncorners + 1));
    uint32_t* remap = malloc(sizeof(uint32_t) * (m->ncorners + 1));
    if (!idx || !corner_of || !tmat || !pos || !remap) return free(idx), free(corner_of), free(tmat), free(pos), free(remap), false;
    // a corner's key: its vertex and UV (the same: one vertex for the simplifier)
    typedef struct { uint32_t v; float u, w; } CK;
    CK* ck = malloc(sizeof(CK) * (m->ncorners + 1));
    if (!ck) return free(idx), free(corner_of), free(tmat), free(pos), free(remap), false;
    for (uint32_t c = 0; c < m->ncorners; c++) ck[c].v = m->loops[c], ck[c].u = m->uv[c * 2], ck[c].w = m->uv[c * 2 + 1];
    size_t nu = meshopt_generateVertexRemap(remap, NULL, m->ncorners, ck, m->ncorners, sizeof(CK));
    uint32_t* first = malloc(sizeof(uint32_t) * (nu + 1)); // a corner of each unique vertex
    if (!first) return free(ck), free(idx), free(corner_of), free(tmat), free(pos), free(remap), false;
    for (uint32_t c = 0; c < m->ncorners; c++) first[remap[c]] = c;
    for (size_t u = 0; u < nu; u++) memcpy(&pos[u * 3], &m->co[m->loops[first[u]] * 3], 12);
    uint32_t t = 0;
    for (uint32_t f = 0; f < m->nf; f++) {
        uint32_t st = m->fstart[f];
        for (uint32_t c = 1; c + 1 < m->fsize[f]; c++, t++) {
            idx[t * 3] = remap[st], idx[t * 3 + 1] = remap[st + c], idx[t * 3 + 2] = remap[st + c + 1];
            tmat[t] = m->fmat[f];
        }
    }
    uint32_t* out = malloc(sizeof(uint32_t) * ntri * 3 + 4);
    size_t nout = 0;
    if (out) {
        // per material separately (a material border stays)
        for (uint32_t s = 0; s < 1024; s++) {
            uint32_t* sub = malloc(sizeof(uint32_t) * ntri * 3 + 4);
            size_t ns = 0;
            for (uint32_t q = 0; sub && q < ntri; q++)
                if (tmat[q] == s) memcpy(&sub[ns], &idx[q * 3], 12), ns += 3;
            if (sub && ns) {
                size_t target = (size_t)((float)ns * ratio / 3.0f) * 3;
                if (target < 3) target = 3;
                size_t r = meshopt_simplify(sub, sub, ns, pos, nu, 12, target, 0.2f, 0, NULL); // (UV seams: kept consistent by meshoptimizer)
                for (size_t q = 0; q < r; q++) out[nout + q] = sub[q];
                for (size_t q = 0; q < r / 3; q++) corner_of[(nout / 3) + q] = s; // (the material)
                nout += r;
            }
            free(sub);
            bool more = false;
            for (uint32_t q = 0; q < ntri && !more; q++) more = tmat[q] > s;
            if (!more) break;
        }
    }
    bool ok = false;
    if (out && nout) {
        MB b;
        mb_init(&b, m);
        uint32_t* nv = malloc(sizeof(uint32_t) * (m->nv + 1));
        if (nv) {
            for (uint32_t v = 0; v < m->nv; v++) nv[v] = UINT32_MAX;
            for (size_t q = 0; q < nout / 3; q++) {
                uint32_t vv[3], cc[3];
                float uv[3][2];
                for (int k = 0; k < 3; k++) {
                    uint32_t c = first[out[q * 3 + k]], v = m->loops[c];
                    if (nv[v] == UINT32_MAX) nv[v] = mb_copy(&b, m, v);
                    vv[k] = nv[v], uv[k][0] = m->uv[c * 2], uv[k][1] = m->uv[c * 2 + 1], cc[k] = m->col ? m->col[c] : 0xFFFFFFFFu;
                }
                mb_face(&b, vv, (const float(*)[2])uv, cc, 3, (uint16_t)corner_of[q]);
            }
            free(nv);
            ok = mb_finish(&b, m);
        }
    }
    free(out), free(first), free(ck), free(idx), free(corner_of), free(tmat), free(pos), free(remap);
    return ok;
}

// What each of the Tlender's modifiers becomes (its Blender type name, "md <type>")
typedef enum { MK_APPLY = 0, MK_SAME, MK_CONVERT, MK_LOST } ModKindResult;

// The object's evaluated mesh written by the Tlender ("evalmeshes <n>", then per object "evalmesh <object> <nv> <nf>"
// and the "v" / "f" / "t" lines of a mesh block, its vertex groups as the mesh's "vgroups"): the whole stack before the
// Armature, exactly as the Tlender shows it. Defined here (2026-11-18, Q19) for the Tlender's export.
static int find_evalmesh(TFile* f, int oi, TMesh* out) {
    size_t at = find(f, 0, "evalmeshes");
    if (at >= f->nlines) return 0;
    for (size_t i = at + 1; i < f->nlines && !starts(f->lines[i], "end"); i++) {
        int o = -1;
        unsigned nv = 0, nf = 0;
        if (!starts(f->lines[i], "evalmesh") || sscanf(f->lines[i] + 9, "%d %u %u", &o, &nv, &nf) != 3 || o != oi) continue;
        // parse it as a mesh block: the line made "mesh <nv> <nf>" in a scratch copy of the lines
        char head[64];
        snprintf(head, sizeof head, "mesh %u %u", nv, nf);
        char* saved = f->lines[i];
        f->lines[i] = head;
        TMesh* keep = f->meshes;
        int kn = f->nmeshes;
        f->meshes = NULL, f->nmeshes = 0;
        parse_meshes(f, i, 1);
        int got = f->nmeshes == 1 && f->meshes[0].nf > 0;
        if (got) *out = f->meshes[0];
        else if (f->meshes) free(f->meshes[0].co), free(f->meshes[0].fstart), free(f->meshes[0].fsize), free(f->meshes[0].loops), free(f->meshes[0].uv), free(f->meshes[0].fmat);
        free(f->meshes);
        f->meshes = keep, f->nmeshes = kn;
        f->lines[i] = saved;
        return got;
    }
    return 0;
}

// The stack of object oi applied to its own copy of its mesh (the report says what became what)
static void apply_modifiers(TFile* f, int oi) {
    TObj* o = &f->objs[oi];
    if (o->mods_at == 0 || o->mesh < 0 || o->mesh >= f->nmeshes) return;
    int nm = 0;
    sscanf(f->lines[o->mods_at] + 5, "%d", &nm);
    // the evaluated mesh, when the Tlender wrote it: the stack is in it
    TMesh ev;
    bool has_eval = find_evalmesh(f, oi, &ev) != 0;
    TMesh work;
    bool cloned = false;
    if (has_eval) {
        TMesh* nmz = realloc(f->meshes, sizeof(TMesh) * (size_t)(f->nmeshes + 1));
        if (nmz) {
            f->meshes = nmz;
            f->meshes[f->nmeshes] = ev;
            o->mesh = f->nmeshes++;
        }
    }
    for (int k = 0; k < nm && o->mods_at + 1 + (size_t)k * 2 + 1 < f->nlines; k++) {
        const char* md = f->lines[o->mods_at + 1 + (size_t)k * 2];
        const char* mpl = f->lines[o->mods_at + 2 + (size_t)k * 2];
        if (!starts(md, "md") || !starts(mpl, "mp")) break;
        char type[48] = "", name[64] = "";
        unsigned flags = 0;
        int used = 0;
        if (sscanf(md + 3, "%47s %u %n", type, &flags, &used) < 2) continue;
        snprintf(name, sizeof name, "%s", used ? md + 3 + used : type);
        const char* mp = mpl[2] ? mpl + 3 : "";
        char item[128];
        snprintf(item, sizeof item, "%s (%s)", name, type);
        if (!(flags & 1u)) { // off in the viewport: as the Tlender shows it
            imprep_add(IMPREP_SAME, IMPREP_CAT_MODIFIER, o->name, item, TR("off in the Tlender's viewport: not applied"));
            continue;
        }
        if (!strcmp(type, "ARMATURE")) {
            imprep_add(IMPREP_SAME, IMPREP_CAT_MODIFIER, o->name, item, TR("the Skeletal Mesh: the mesh follows the armature's bones"));
            continue;
        }
        if (!strcmp(type, "TRIANGULATE")) {
            imprep_add(IMPREP_SAME, IMPREP_CAT_MODIFIER, o->name, item, TR("the Engine draws triangles: the faces are split the same way"));
            continue;
        }
        if (!strcmp(type, "CLOTH") || !strcmp(type, "SOFT_BODY")) {
            o->sim = !strcmp(type, "CLOTH") ? 1 : 2;
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item,
                       o->sim == 1 ? TR("the Cloth component on the mesh (its top band pinned; Details > Cloth)") : TR("the Cloth component as a Soft Body (Details > Soft Body)"));
            continue;
        }
        if (!strcmp(type, "COLLISION")) {
            o->collide = 1;
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("the actor blocks and the cloths collide with it (Collision Complexity: the mesh)"));
            continue;
        }
        if (!strcmp(type, "PARTICLE_SYSTEM")) {
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("its hair is a Groom (the hair objects' guides)"));
            continue;
        }
        if (!strcmp(type, "EDGE_SPLIT")) {
            if (mp_i(mp, "use_edge_angle", 1)) o->split_cos = cosf(mp_f(mp, "split_angle", 0.523599f));
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("the hard edges at its angle (the normals split as the Edge Split does)"));
            continue;
        }
        if (!strcmp(type, "WEIGHTED_NORMAL")) {
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("the normals weighted by the faces' areas (Face Area; its weight and mode are not kept)"));
            continue;
        }
        if (!strcmp(type, "TL_DELTA_MUSH") || !strcmp(type, "TL_TENSION")) {
            imprep_add(IMPREP_SAME, IMPREP_CAT_MODIFIER, o->name, item, TR("no change at rest (it acts on the deformation, which the Skeletal Mesh does by its skin)"));
            continue;
        }
        if (has_eval) { // in the Tlender's evaluated mesh
            imprep_add(IMPREP_BAKED, IMPREP_CAT_MODIFIER, o->name, item, TR("in the evaluated mesh the Tlender wrote"));
            continue;
        }
        // the ones applied here, on the object's own copy of its mesh
        bool geom = !strcmp(type, "MIRROR") || !strcmp(type, "ARRAY") || !strcmp(type, "SUBSURF") || !strcmp(type, "SOLIDIFY") ||
                    !strcmp(type, "WELD") || !strcmp(type, "DISPLACE") || !strcmp(type, "MASK") || !strcmp(type, "TL_NONLINEAR") ||
                    !strcmp(type, "DECIMATE");
        if (geom) {
            if (!cloned) {
                if (!tmesh_clone(&f->meshes[o->mesh], &work)) continue;
                cloned = true;
            }
            double t0 = re_time_seconds();
            bool done = false;
            uint32_t nf0 = 0, nf1 = 0; // (triangles: the faces as the Engine draws them)
            for (uint32_t q = 0; q < work.nf; q++) nf0 += work.fsize[q] > 2 ? work.fsize[q] - 2 : 0;
            if (!strcmp(type, "MIRROR")) done = mod_mirror(f, oi, &work, mp);
            else if (!strcmp(type, "ARRAY")) done = mod_array(f, oi, &work, mp, o->name);
            else if (!strcmp(type, "SUBSURF")) {
                int lv = mp_i(mp, "levels", 1);
                bool simple = mp_enum(mp, "subdivision_type", "CATMULL_CLARK,SIMPLE", 0) != 0;
                bool corners = mp_enum(mp, "boundary_smooth", "ALL,PRESERVE_CORNERS", 1) == 1;
                // (2026-11-19) the creases from the file, unless Use Creases is off
                if (!mp_i(mp, "use_creases", 1)) free(work.crease_e), free(work.crease_s), work.crease_e = NULL, work.crease_s = NULL, work.ncrease = 0;
                uint32_t ncr = work.ncrease;
                for (int l = 0; l < lv && l < 4 && work.ncorners < 4000000u; l++) done = subdivide_once(&work, simple, corners) || done;
                if (mp_enum(mp, "uv_smooth", "NONE,PRESERVE_CORNERS,PRESERVE_BOUNDARIES,SMOOTH_ALL", 2) != 0)
                    imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("the UVs split linearly (the Tlender smooths them: a slight difference inside the islands)"));
                if (ncr)
                    imprep_add(IMPREP_SAME, IMPREP_CAT_MODIFIER, o->name, item, TR("its %u creased edges as the Tlender's (sharpness crease^2 x 10, 1 less a level)"), ncr);
            } else if (!strcmp(type, "SOLIDIFY")) done = mod_solidify(&work, mp, o->name);
            else if (!strcmp(type, "WELD")) {
                done = tmesh_weld(&work, mp_f(mp, "merge_threshold", 0.001f)) > 0;
                if (mp_enum(mp, "mode", "ALL,CONNECTED", 0) == 1)
                    imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item, TR("Connected: merged as All (the vertices near each other, connected or not)"));
            } else if (!strcmp(type, "DISPLACE")) done = mod_displace(f, oi, &work, mp);
            else if (!strcmp(type, "MASK")) done = mod_mask(&work, mp, o->name);
            else if (!strcmp(type, "TL_NONLINEAR")) done = mod_nonlinear(f, oi, &work, mp);
            else if (!strcmp(type, "DECIMATE")) done = mod_decimate(&work, mp, o->name);
            char tm[64];
            snprintf(tm, sizeof tm, "%s %s", o->name, type);
            imprep_time(tm, (re_time_seconds() - t0) * 1000.0);
            for (uint32_t q = 0; q < work.nf; q++) nf1 += work.fsize[q] > 2 ? work.fsize[q] - 2 : 0;
            imprep_add(IMPREP_CONVERTED, IMPREP_CAT_MODIFIER, o->name, item,
                       done ? TR("applied to the mesh (%u triangles before, %u after)") : TR("applied: no change to the mesh (%u triangles)"), nf0, nf1);
            continue;
        }
        // the rest: the Engine can't apply it here
        const char* why = TR("the Engine can't apply it: export the evaluated mesh from the Tlender (Export to Real Engine)");
        if (!strcmp(type, "MESH_SEQUENCE_CACHE")) why = TR("an Alembic cache: the Engine doesn't read .abc (the mesh at rest came in)");
        else if (!strcmp(type, "LIQUID") || !strcmp(type, "TL_VOXEL_FLUID") || !strcmp(type, "GRANULAR_TERRAIN"))
            why = TR("a simulation of the Tlender's: the Engine has its own (Water Body, particles); the mesh at rest came in");
        else if (!strcmp(type, "BURN") || !strcmp(type, "WET_LAYER") || !strcmp(type, "TL_TISSUE"))
            why = TR("a simulation of the Tlender's surfaces: use the Wet Surface material node / the Engine's fire");
        else if (!strcmp(type, "GRASS")) why = TR("the Tlender's grass field: paint the Engine's Foliage on the mesh");
        else if (!strcmp(type, "ROPE")) why = TR("the Tlender's rope: use the Engine's Cable Component");
        imprep_add(IMPREP_LOST, IMPREP_CAT_MODIFIER, o->name, item, "%s", why);
    }
    if (cloned) { // the object's own mesh from now on
        TMesh* nmz = realloc(f->meshes, sizeof(TMesh) * (size_t)(f->nmeshes + 1));
        if (nmz) {
            f->meshes = nmz;
            f->meshes[f->nmeshes] = work;
            o->mesh = f->nmeshes++;
        }
    }
}
