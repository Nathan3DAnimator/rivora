// --- The Real Engine's exchange format (.rasset, RealEngine.md "Compatibilidade com o Tlender"), included by
// tlend_import.c (#164): what the Tlender's Export > Real Engine writes (its io_rasset.h: version 1 and the chunks added
// after it; the records copied here with an Ra prefix) read and checked, then turned into the same scene structure as a
// .tlend and so into the same assets: the meshes welded back by the Tlender vertex each corner came from (hard edges where
// the file's normals split), a Principled BSDF of each material's PBR terms with the images the file carries, skeletons
// as armatures (the rest from the inverse binds), skins as vertex groups, the sampled animations as actions (a key per
// sample), morph targets as shape keys, lights and cameras. Axes and units are the Tlender's (Blender's). A newer record
// (grown at its end) is read by its own size; a chunk this reader doesn't know is skipped. ---

#define RA_VERSION 1

typedef struct RaHeader {
    char magic[8];
    uint32_t version, chunk_count;
    uint64_t file_size;
    uint32_t axes;
    float meters_per_unit;
    uint32_t source, flags;
    uint8_t reserved[24];
} RaHeader;
typedef struct RaChunk { char tag[4]; uint32_t count; uint64_t offset, size; } RaChunk;
enum { RA_EMPTY = 0, RA_MESH, RA_SKELETON, RA_LIGHT, RA_CAMERA };
typedef struct RaNode {
    uint32_t name;
    int32_t parent;
    uint32_t kind;
    int32_t ref;
    float t[3], r[4], s[3]; // r: x y z w
    uint32_t flags;         // bit 0: hidden
    uint32_t reserved;
} RaNode;
typedef struct RaSubmesh { uint32_t first_index, index_count; int32_t material; uint32_t reserved; } RaSubmesh;
enum { RA_NORMALS = 1u << 0, RA_UVS = 1u << 1, RA_SKIN = 1u << 2 };
typedef struct RaMesh {
    uint32_t name, vertex_count, index_count, submesh_count;
    int32_t skeleton;
    uint32_t flags;
    uint64_t positions, normals, uvs, joints, weights, indices, submeshes, source;
    float bind[16]; // column-major: the mesh's space -> the skeleton node's at bind time
    float bounds_min[3], bounds_max[3];
} RaMesh;
typedef struct RaMaterial {
    uint32_t name, flags;
    float base_color[4];
    float metallic, roughness, ior, alpha_cutoff;
    float emission[3], emission_strength;
    float normal_strength, transmission, coat, coat_roughness;
    float specular_ior_level, sheen, sheen_roughness, subsurface;
    int32_t tex[7]; // base color, metallic, roughness, normal, emission, alpha, transmission (-1: none)
    uint32_t reserved;
} RaMaterial;
enum { RA_TEX_BASE = 0, RA_TEX_METALLIC, RA_TEX_ROUGHNESS, RA_TEX_NORMAL, RA_TEX_EMISSION, RA_TEX_ALPHA, RA_TEX_TRANSMISSION, RA_TEX_COUNT };
enum { RA_TEX_NON_COLOR = 1u << 0, RA_TEX_CLOSEST = 1u << 1 };
typedef struct RaTexture {
    uint32_t name, path, ext;
    int32_t channel;    // -1 RGB, 0..2 R/G/B, 3 alpha
    uint32_t flags;     // RA_TEX_*
    uint32_t extension; // 0 repeat, 1 extend, 2 clip, 3 mirror
    float offset[2], scale[2], rotation;
    uint32_t reserved;
    uint64_t data, size; // the image file's bytes (0, 0: not found on export)
} RaTexture;
typedef struct RaBone {
    uint32_t name;
    int32_t parent;
    float t[3], r[4], s[3];
    float inv_bind[16]; // the inverse of the rest in the skeleton node's space
    float length;
    uint32_t flags;
} RaBone;
typedef struct RaSkeleton { uint32_t name, bone_count; uint64_t bones; } RaSkeleton;
typedef struct RaLight {
    uint32_t name, type; // 0 point, 1 sun, 2 spot, 3 area (the .tlend's light types)
    float color[3];
    float power, radius, spot_size, spot_blend;
    uint32_t area_shape;
    float size_x, size_y;
    uint32_t reserved[2];
} RaLight;
typedef struct RaCamera {
    uint32_t name, type;
    float lens_mm, sensor_w, sensor_h, yfov, aspect, ortho_half_w, ortho_half_h, clip_start, clip_end, shift_x, shift_y;
    float focus_distance, fstop;
    uint32_t reserved;
} RaCamera;
enum { RA_TRACK_NODE = 0, RA_TRACK_BONE = 1 };
typedef struct RaTrack { uint32_t kind, node, bone, sample_count; uint64_t samples; } RaTrack; // f32[10] per sample: t, r (xyzw), s
typedef struct RaAnim { uint32_t name; float fps; int32_t frame_start; uint32_t frame_count; int32_t frame_step; uint32_t track_count; uint64_t tracks; } RaAnim;
typedef struct RaMorph { uint32_t name, mesh, flags, reserved; uint64_t deltas; float weight, range_min, range_max; uint32_t reserved2; } RaMorph;
typedef struct RaMorphTrack { uint32_t anim, morph, sample_count, reserved; uint64_t samples; } RaMorphTrack;
_Static_assert(sizeof(RaHeader) == 64 && sizeof(RaChunk) == 24 && sizeof(RaNode) == 64 && sizeof(RaMesh) == 176, "rasset");
_Static_assert(sizeof(RaMaterial) == 120 && sizeof(RaTexture) == 64 && sizeof(RaBone) == 120 && sizeof(RaSkeleton) == 16, "rasset");
_Static_assert(sizeof(RaLight) == 56 && sizeof(RaCamera) == 64 && sizeof(RaTrack) == 24 && sizeof(RaAnim) == 32, "rasset");
_Static_assert(sizeof(RaMorph) == 40 && sizeof(RaMorphTrack) == 24, "rasset");

enum { RC_NODE, RC_MESH, RC_MATL, RC_TEXR, RC_SKEL, RC_LGHT, RC_CAMR, RC_ANIM, RC_MRPH, RC_MWGT, RC_N };
typedef struct RaView {
    const uint8_t* data;
    size_t size;
    const char* strs;
    size_t nstr;
    const uint8_t* rec[RC_N]; // each chunk's records (read with their own size: a newer version grows them at the end)
    size_t stride[RC_N];
    uint32_t count[RC_N];
} RaView;
#define RA_REC(v, k, T, i) ((const T*)((v)->rec[k] + (size_t)(i) * (v)->stride[k]))

static const char* ra_str(const RaView* v, uint32_t off) { return off < v->nstr ? v->strs + off : ""; }
static const void* ra_at(const RaView* v, uint64_t off) { return off && off < v->size ? v->data + off : NULL; }
static bool ra_range(const RaView* v, uint64_t off, uint64_t n) { return off <= v->size && n <= v->size - off; }

// The header, the chunk table and every offset and index of the records against the size (the Tlender's rasset_view)
static bool ra_view(const uint8_t* data, size_t size, RaView* v, char* err, size_t cap) {
    memset(v, 0, sizeof *v);
    v->data = data, v->size = size;
#define RA_FAIL(what) return snprintf(err, cap, TR("damaged .rasset file: %s"), what), false
    if (size < sizeof(RaHeader) || memcmp(data, "RASSET\0\0", 8)) RA_FAIL(TR("not a .rasset"));
    const RaHeader* h = (const RaHeader*)data;
    if (h->version < 1 || h->version > RA_VERSION)
        return snprintf(err, cap, TR("version %u of the .rasset is newer than this one (%d): update the Rivora Engine"), h->version, RA_VERSION), false;
    if (h->file_size > size || !ra_range(v, sizeof *h, (uint64_t)h->chunk_count * sizeof(RaChunk))) RA_FAIL(TR("size"));
    static const char* const TAGS[RC_N] = { "NODE", "MESH", "MATL", "TEXR", "SKEL", "LGHT", "CAMR", "ANIM", "MRPH", "MWGT" };
    static const size_t RECS[RC_N] = { sizeof(RaNode), sizeof(RaMesh), sizeof(RaMaterial), sizeof(RaTexture), sizeof(RaSkeleton),
                                       sizeof(RaLight), sizeof(RaCamera), sizeof(RaAnim), sizeof(RaMorph), sizeof(RaMorphTrack) };
    const RaChunk* ch = (const RaChunk*)(data + sizeof *h);
    for (uint32_t i = 0; i < h->chunk_count; i++) {
        const RaChunk* c = &ch[i];
        if (!ra_range(v, c->offset, c->size) || (c->offset & 15)) RA_FAIL(TR("chunk table"));
        if (!memcmp(c->tag, "STR ", 4)) {
            v->strs = (const char*)data + c->offset, v->nstr = (size_t)c->size;
            continue;
        }
        for (int k = 0; k < RC_N; k++) {
            if (memcmp(c->tag, TAGS[k], 4)) continue;
            if (c->count && c->size / c->count < RECS[k]) RA_FAIL(TAGS[k]);
            v->rec[k] = c->count ? data + c->offset : NULL, v->count[k] = c->count, v->stride[k] = c->count ? (size_t)(c->size / c->count) : RECS[k];
        }
    }
    if (!v->strs || !v->nstr || v->strs[v->nstr - 1]) RA_FAIL("STR");
#define NAME_OK(o) ((o) < v->nstr)
#define ARR_OK(o, n) ((o) == 0 ? (n) == 0 : ((o) & 3) == 0 && ra_range(v, (o), (n)))
    for (uint32_t i = 0; i < v->count[RC_NODE]; i++) {
        const RaNode* n = RA_REC(v, RC_NODE, RaNode, i);
        if (!NAME_OK(n->name) || n->parent >= (int32_t)i || n->parent < -1 || n->kind > RA_CAMERA) RA_FAIL("NODE");
        uint32_t lim = n->kind == RA_MESH ? v->count[RC_MESH] : n->kind == RA_SKELETON ? v->count[RC_SKEL] : n->kind == RA_LIGHT ? v->count[RC_LGHT]
                     : n->kind == RA_CAMERA ? v->count[RC_CAMR] : 0;
        if (n->kind != RA_EMPTY && (n->ref < 0 || (uint32_t)n->ref >= lim)) RA_FAIL("NODE");
    }
    for (uint32_t i = 0; i < v->count[RC_SKEL]; i++) {
        const RaSkeleton* k = RA_REC(v, RC_SKEL, RaSkeleton, i);
        if (!NAME_OK(k->name) || !ARR_OK(k->bones, (uint64_t)k->bone_count * sizeof(RaBone))) RA_FAIL("SKEL");
        const RaBone* b = ra_at(v, k->bones);
        for (uint32_t j = 0; j < k->bone_count; j++)
            if (!NAME_OK(b[j].name) || b[j].parent >= (int32_t)k->bone_count || b[j].parent < -1) RA_FAIL("SKEL");
    }
    for (uint32_t i = 0; i < v->count[RC_TEXR]; i++) {
        const RaTexture* t = RA_REC(v, RC_TEXR, RaTexture, i);
        if (!NAME_OK(t->name) || !NAME_OK(t->path) || !NAME_OK(t->ext) || (t->data && !ra_range(v, t->data, t->size))) RA_FAIL("TEXR");
    }
    for (uint32_t i = 0; i < v->count[RC_MATL]; i++) {
        const RaMaterial* m = RA_REC(v, RC_MATL, RaMaterial, i);
        if (!NAME_OK(m->name)) RA_FAIL("MATL");
        for (int k = 0; k < RA_TEX_COUNT; k++)
            if (m->tex[k] < -1 || m->tex[k] >= (int32_t)v->count[RC_TEXR]) RA_FAIL("MATL");
    }
    for (uint32_t i = 0; i < v->count[RC_MESH]; i++) {
        const RaMesh* m = RA_REC(v, RC_MESH, RaMesh, i);
        uint64_t nv = m->vertex_count;
        bool skin = m->flags & RA_SKIN;
        if (!NAME_OK(m->name) || m->index_count % 3 || !m->positions || !ARR_OK(m->positions, nv * 12) || !ARR_OK(m->normals, m->normals ? nv * 12 : 0) ||
            !ARR_OK(m->uvs, m->uvs ? nv * 8 : 0) || !ARR_OK(m->indices, (uint64_t)m->index_count * 4) ||
            !ARR_OK(m->submeshes, (uint64_t)m->submesh_count * sizeof(RaSubmesh)) || !ARR_OK(m->source, m->source ? nv * 4 : 0) ||
            (skin && (m->skeleton < 0 || (uint32_t)m->skeleton >= v->count[RC_SKEL] || !ARR_OK(m->joints, nv * 8) || !ARR_OK(m->weights, nv * 16))))
            RA_FAIL("MESH");
        const uint32_t* idx = ra_at(v, m->indices);
        for (uint32_t k = 0; k < m->index_count; k++)
            if (idx[k] >= m->vertex_count) RA_FAIL("MESH");
        const RaSubmesh* sm = ra_at(v, m->submeshes);
        for (uint32_t k = 0; k < m->submesh_count; k++)
            if ((uint64_t)sm[k].first_index + sm[k].index_count > m->index_count || sm[k].material < -1 || sm[k].material >= (int32_t)v->count[RC_MATL])
                RA_FAIL("MESH");
        if (skin) {
            const uint16_t* j = ra_at(v, m->joints);
            uint32_t bc = RA_REC(v, RC_SKEL, RaSkeleton, m->skeleton)->bone_count;
            for (uint64_t k = 0; k < nv * 4; k++)
                if (j[k] >= bc && bc) RA_FAIL("MESH");
        }
    }
    for (uint32_t i = 0; i < v->count[RC_LGHT]; i++)
        if (!NAME_OK(RA_REC(v, RC_LGHT, RaLight, i)->name) || RA_REC(v, RC_LGHT, RaLight, i)->type > 3) RA_FAIL("LGHT");
    for (uint32_t i = 0; i < v->count[RC_CAMR]; i++)
        if (!NAME_OK(RA_REC(v, RC_CAMR, RaCamera, i)->name)) RA_FAIL("CAMR");
    for (uint32_t i = 0; i < v->count[RC_ANIM]; i++) {
        const RaAnim* a = RA_REC(v, RC_ANIM, RaAnim, i);
        if (!NAME_OK(a->name) || !ARR_OK(a->tracks, (uint64_t)a->track_count * sizeof(RaTrack))) RA_FAIL("ANIM");
        const RaTrack* t = ra_at(v, a->tracks);
        for (uint32_t k = 0; k < a->track_count; k++) {
            bool ok = t[k].node < v->count[RC_NODE] && (t[k].sample_count == 1 || t[k].sample_count == a->frame_count) &&
                      ARR_OK(t[k].samples, (uint64_t)t[k].sample_count * 40);
            if (ok && t[k].kind == RA_TRACK_BONE) {
                const RaNode* n = RA_REC(v, RC_NODE, RaNode, t[k].node);
                ok = n->kind == RA_SKELETON && t[k].bone < RA_REC(v, RC_SKEL, RaSkeleton, n->ref)->bone_count;
            } else if (ok)
                ok = t[k].kind == RA_TRACK_NODE;
            if (!ok) RA_FAIL("ANIM");
        }
    }
    for (uint32_t i = 0; i < v->count[RC_MRPH]; i++) {
        const RaMorph* m = RA_REC(v, RC_MRPH, RaMorph, i);
        if (!NAME_OK(m->name) || m->mesh >= v->count[RC_MESH] || !ARR_OK(m->deltas, (uint64_t)RA_REC(v, RC_MESH, RaMesh, m->mesh)->vertex_count * 12))
            RA_FAIL("MRPH");
    }
    for (uint32_t i = 0; i < v->count[RC_MWGT]; i++) {
        const RaMorphTrack* t = RA_REC(v, RC_MWGT, RaMorphTrack, i);
        if (t->anim >= v->count[RC_ANIM] || t->morph >= v->count[RC_MRPH] ||
            (t->sample_count != 1 && t->sample_count != RA_REC(v, RC_ANIM, RaAnim, t->anim)->frame_count) || !ARR_OK(t->samples, (uint64_t)t->sample_count * 4))
            RA_FAIL("MWGT");
    }
#undef NAME_OK
#undef ARR_OK
#undef RA_FAIL
    return true;
}

// T * R * S (r: x y z w)
static M4 ra_trs(const float t[3], const float r[4], const float s[3]) {
    float q[4] = { r[3], r[0], r[1], r[2] };
    M4 rot = m4_quat(q);
    return m4_trs(t, &rot, s);
}

// --- Welding: the corners the Tlender split (a seam of the UVs, a hard edge) back on their vertex ---
typedef struct RaWeld {
    uint32_t* slot; // open addressing: welded vertex + 1 (0: empty)
    uint32_t cap;
} RaWeld;

static uint64_t ra_hash(const uint32_t* k, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) h = (h ^ k[i]) * 1099511628211ull, h ^= h >> 29;
    return h;
}

// The key of a file vertex: the Tlender vertex it came from, or its position (and its skin) bit for bit
static int ra_weld_key(const RaMesh* m, const RaView* v, uint32_t i, uint32_t k[10]) {
    const uint32_t* src = ra_at(v, m->source);
    if (src) return k[0] = src[i], 1;
    const float* p = ra_at(v, m->positions);
    memcpy(k, &p[i * 3], 12);
    if (!(m->flags & RA_SKIN)) return 3;
    const uint16_t* j = ra_at(v, m->joints);
    const float* w = ra_at(v, m->weights);
    k[3] = (uint32_t)j[i * 4] | (uint32_t)j[i * 4 + 1] << 16, k[4] = (uint32_t)j[i * 4 + 2] | (uint32_t)j[i * 4 + 3] << 16;
    memcpy(&k[5], &w[i * 4], 16);
    return 9;
}

// A file mesh into a TMesh: its welded vertices (by bind into the skeleton node's space when skinned), triangles by
// slot (slots[]: its materials, -1 the default), the vertex groups of its skin, its morph targets as shape keys and the
// sharp edges where the file's normals part. false: out of memory
static bool ra_mesh(const RaView* v, uint32_t mi, int default_mat, TMesh* me, int* slots, int* nslots) {
    const RaMesh* m = RA_REC(v, RC_MESH, RaMesh, mi);
    memset(me, 0, sizeof *me);
    *nslots = 0;
    uint32_t nv = m->vertex_count, nt = m->index_count / 3;
    const float* pos = ra_at(v, m->positions);
    const float* nrm = ra_at(v, m->normals);
    const float* uv = ra_at(v, m->uvs);
    const uint32_t* idx = ra_at(v, m->indices);
    const RaSubmesh* sm = ra_at(v, m->submeshes);
    bool skin = (m->flags & RA_SKIN) && m->skeleton >= 0;
    uint32_t* weld = malloc(sizeof(uint32_t) * (nv ? nv : 1)); // file vertex -> welded
    uint32_t* first = malloc(sizeof(uint32_t) * (nv ? nv : 1)); // welded -> a file vertex
    RaWeld h = { 0 };
    for (h.cap = 64; h.cap < nv * 2u + 2u; h.cap <<= 1) {}
    h.slot = calloc(h.cap, sizeof(uint32_t));
    if (!weld || !first || !h.slot) return free(weld), free(first), free(h.slot), false; // (an import, not a frame)
    uint32_t nw = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t k[10], k2[10];
        int nk = ra_weld_key(m, v, i, k);
        uint32_t at = (uint32_t)ra_hash(k, nk) & (h.cap - 1);
        for (;; at = (at + 1) & (h.cap - 1)) {
            uint32_t s = h.slot[at];
            if (!s) {
                h.slot[at] = nw + 1, first[nw] = i, weld[i] = nw++;
                break;
            }
            ra_weld_key(m, v, first[s - 1], k2);
            if (!memcmp(k, k2, sizeof(uint32_t) * (size_t)nk)) {
                weld[i] = s - 1;
                break;
            }
        }
    }
    free(h.slot);
    M4 bind;
    memcpy(bind.m, m->bind, sizeof bind.m);
    me->nv = nw;
    me->co = malloc(sizeof(float) * 3 * (nw ? nw : 1));
    me->fstart = malloc(sizeof(uint32_t) * (nt ? nt : 1)), me->fsize = malloc(sizeof(uint32_t) * (nt ? nt : 1));
    me->fmat = malloc(sizeof(uint16_t) * (nt ? nt : 1));
    me->loops = malloc(sizeof(uint32_t) * 3 * (nt ? nt : 1)), me->uv = calloc((size_t)nt * 6 + 2, sizeof(float));
    uint32_t* ftri = malloc(sizeof(uint32_t) * (nt ? nt : 1)); // face -> the file's triangle
    bool ok = me->co && me->fstart && me->fsize && me->fmat && me->loops && me->uv && ftri;
    for (uint32_t w = 0; ok && w < nw; w++) {
        const float* p = &pos[first[w] * 3];
        if (skin) m4_point(&bind, p, &me->co[w * 3]);
        else memcpy(&me->co[w * 3], p, 12);
    }
    // the triangles: a slot per material of the submeshes (none: the whole mesh in the default's)
    int smat_slot[64];
    uint32_t nsub = m->submesh_count ? m->submesh_count : 1;
    for (uint32_t s = 0; s < nsub && s < 64; s++) {
        int mat = m->submesh_count && sm[s].material >= 0 ? sm[s].material : default_mat, sl = -1;
        for (int q = 0; q < *nslots; q++)
            if (slots[q] == mat) sl = q;
        if (sl < 0 && *nslots < 32) sl = *nslots, slots[(*nslots)++] = mat;
        smat_slot[s] = sl < 0 ? 0 : sl;
    }
    for (uint32_t s = 0; ok && s < nsub; s++) {
        uint32_t t0 = m->submesh_count ? sm[s].first_index / 3 : 0, t1 = m->submesh_count ? t0 + sm[s].index_count / 3 : nt;
        for (uint32_t t = t0; t < t1 && t < nt && me->nf < nt; t++) {
            uint32_t c = me->ncorners;
            ftri[me->nf] = t, me->fstart[me->nf] = c, me->fsize[me->nf] = 3, me->fmat[me->nf] = (uint16_t)smat_slot[s < 64 ? s : 63];
            for (int q = 0; q < 3; q++) {
                uint32_t fv = idx[t * 3 + q];
                me->loops[c + q] = weld[fv];
                if (uv) me->uv[(c + q) * 2] = uv[fv * 2], me->uv[(c + q) * 2 + 1] = uv[fv * 2 + 1];
            }
            me->ncorners += 3, me->nf++;
        }
    }
    // the sharp edges: an edge whose two faces see different normals at one of its ends (the file split it there)
    if (ok && nrm && me->nf) {
        uint32_t ecap = 64;
        while (ecap < me->nf * 4u) ecap <<= 1;
        struct RaEdge { uint32_t a, b, fa, fb; } *eh = calloc(ecap, sizeof *eh); // fa/fb: the file vertices of the first face + 1
        uint32_t ns = 0;
        size_t scap = 0;
        for (uint32_t t = 0; eh && t < me->nf; t++)
            for (int q = 0; q < 3; q++) {
                uint32_t f0 = idx[ftri[t] * 3 + q], f1 = idx[ftri[t] * 3 + (q + 1) % 3];
                uint32_t a = weld[f0], b = weld[f1];
                if (a == b) continue;
                if (a > b) {
                    uint32_t x = a; a = b, b = x;
                    x = f0, f0 = f1, f1 = x;
                }
                uint32_t k[2] = { a, b }, at = (uint32_t)ra_hash(k, 2) & (ecap - 1);
                while (eh[at].fa && (eh[at].a != a || eh[at].b != b)) at = (at + 1) & (ecap - 1);
                if (!eh[at].fa) {
                    eh[at].a = a, eh[at].b = b, eh[at].fa = f0 + 1, eh[at].fb = f1 + 1;
                    continue;
                }
                if (eh[at].fa == 0xFFFFFFFFu) continue; // (already sharp)
                const float *n0 = &nrm[(eh[at].fa - 1) * 3], *n1 = &nrm[f0 * 3], *n2 = &nrm[(eh[at].fb - 1) * 3], *n3 = &nrm[f1 * 3];
                bool hard = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.9995f || n2[0] * n3[0] + n2[1] * n3[1] + n2[2] * n3[2] < 0.9995f;
                if (!hard) continue;
                eh[at].fa = 0xFFFFFFFFu;
                if (!obj_grow((void**)&me->sharp_e, &scap, (size_t)ns + 1, sizeof me->sharp_e[0])) continue;
                me->sharp_e[ns][0] = a, me->sharp_e[ns][1] = b, ns++;
            }
        me->nsharp = me->sharp_e ? ns : 0;
        free(eh);
    }
    // the skin: a vertex group per bone it uses
    if (ok && skin) {
        const RaSkeleton* sk = RA_REC(v, RC_SKEL, RaSkeleton, m->skeleton);
        const RaBone* bones = ra_at(v, sk->bones);
        const uint16_t* jn = ra_at(v, m->joints);
        const float* wt = ra_at(v, m->weights);
        int* group = malloc(sizeof(int) * (sk->bone_count ? sk->bone_count : 1));
        for (uint32_t b = 0; group && b < sk->bone_count; b++) group[b] = -1;
        uint32_t ng = 0;
        for (uint32_t w = 0; group && w < nw; w++)
            for (int q = 0; q < 4; q++)
                if (wt[first[w] * 4 + q] > 0.0f && group[jn[first[w] * 4 + q]] < 0) group[jn[first[w] * 4 + q]] = (int)ng++;
        me->gname = ng ? calloc(ng, sizeof me->gname[0]) : NULL;
        me->gweight = ng ? calloc((size_t)ng * nw, sizeof(float)) : NULL;
        if (ng && me->gname && me->gweight) {
            me->ngroups = ng;
            for (uint32_t b = 0; b < sk->bone_count; b++)
                if (group[b] >= 0) snprintf(me->gname[group[b]], sizeof me->gname[0], "%s", ra_str(v, bones[b].name));
            for (uint32_t w = 0; w < nw; w++)
                for (int q = 0; q < 4; q++) {
                    float x = wt[first[w] * 4 + q];
                    if (x > 0.0f) me->gweight[(size_t)group[jn[first[w] * 4 + q]] * nw + w] += x;
                }
        } else free(me->gname), free(me->gweight), me->gname = NULL, me->gweight = NULL;
        free(group);
    }
    // the morph targets: shape keys (their offsets by the bind's turn too when skinned)
    uint32_t nk = 0;
    for (uint32_t i = 0; i < v->count[RC_MRPH]; i++) nk += RA_REC(v, RC_MRPH, RaMorph, i)->mesh == mi;
    if (ok && nk) {
        me->kname = calloc(nk, RMESH_MORPH_NAME), me->kvalue = calloc(nk, sizeof(float)), me->krange = calloc(nk, sizeof me->krange[0]);
        me->kdelta = calloc((size_t)nw * nk * 3, sizeof(float));
        if (me->kname && me->kvalue && me->krange && me->kdelta) {
            uint32_t k = 0;
            for (uint32_t i = 0; i < v->count[RC_MRPH]; i++) {
                const RaMorph* mo = RA_REC(v, RC_MRPH, RaMorph, i);
                if (mo->mesh != mi) continue;
                snprintf(me->kname[k], RMESH_MORPH_NAME, "%s", ra_str(v, mo->name));
                me->kvalue[k] = mo->weight, me->krange[k][0] = mo->range_min, me->krange[k][1] = mo->range_max > mo->range_min ? mo->range_max : 1.0f;
                const float* d = ra_at(v, mo->deltas);
                for (uint32_t w = 0; d && w < nw; w++) {
                    float* o = &me->kdelta[((size_t)w * nk + k) * 3];
                    if (skin) m4_dir(&bind, &d[first[w] * 3], o);
                    else memcpy(o, &d[first[w] * 3], 12);
                }
                k++;
            }
            me->nkeys = nk;
        } else tmesh_free_keys(me);
    }
    free(weld), free(first), free(ftri);
    return ok;
}

// A node made for the graph of a material (gm_node's, with room for a whole PBR graph)
static int ra_node(TMat* m, const char* idname, float x, float y) {
    const MatNodeDef* d = mat_node_def_by_idname(idname);
    if (!d || m->nnodes >= 48) return -1;
    TNode* n = &m->nodes[m->nnodes];
    memset(n, 0, sizeof *n);
    snprintf(n->idname, sizeof n->idname, "%s", idname);
    n->x = x, n->y = y, n->nin = d->nin;
    for (int q = 0; q < d->nin && q < 32; q++) memcpy(n->in[q], d->in[q].def, 16);
    for (int q = 0; q < 4; q++) n->prop[q] = -1;
    return m->nnodes++;
}

// A material input's image: an Image Texture (through a Mapping when the file moves it), the channel it reads -> the
// node and output to link from (-1: none)
static int ra_texture(TMat* m, const RaView* v, int ti, char (*tpath)[512], float y, int* uvmap, int* out) {
    if (ti < 0 || !tpath[ti][0]) return -1;
    const RaTexture* t = RA_REC(v, RC_TEXR, RaTexture, ti);
    int n = ra_node(m, "ShaderNodeTexImage", -700, y);
    if (n < 0) return -1;
    TNode* tn = &m->nodes[n];
    snprintf(tn->image, sizeof tn->image, "%s", tpath[ti]);
    tn->prop[2] = (t->flags & RA_TEX_NON_COLOR) ? IMAGE_COLORSPACE_NONCOLOR : IMAGE_COLORSPACE_SRGB;
    tn->prop[1] = t->extension == 1 ? IMAGE_EXT_EXTEND : t->extension == 3 ? IMAGE_EXT_MIRROR : IMAGE_EXT_REPEAT;
    if (t->flags & RA_TEX_CLOSEST) tn->prop[0] = IMAGE_INTERP_CLOSEST;
    bool moved = t->offset[0] != 0.0f || t->offset[1] != 0.0f || t->scale[0] != 1.0f || t->scale[1] != 1.0f || t->rotation != 0.0f;
    if (moved) { // uv' = R * S * uv + offset: Blender's Mapping (Point)
        if (*uvmap < 0) *uvmap = ra_node(m, "ShaderNodeTexCoord", -1200, 0);
        int mp = ra_node(m, "ShaderNodeMapping", -950, y);
        if (mp >= 0 && *uvmap >= 0) {
            TNode* mn = &m->nodes[mp];
            mn->in[1][0] = t->offset[0], mn->in[1][1] = t->offset[1];
            mn->in[2][2] = t->rotation;
            mn->in[3][0] = t->scale[0], mn->in[3][1] = t->scale[1], mn->in[3][2] = 1.0f;
            gm_link(m, *uvmap, 2, mp, 0); // (UV)
            gm_link(m, mp, 0, n, 0);
        }
    }
    *out = 0;
    if (t->channel == 3) *out = 1; // (Alpha)
    else if (t->channel >= 0 && t->channel <= 2) {
        int sep = ra_node(m, "ShaderNodeSeparateColor", -450, y);
        if (sep < 0) return n;
        gm_link(m, n, 0, sep, 0);
        *out = t->channel;
        return sep;
    }
    return n;
}

// A file material as Blender's Principled BSDF: its terms, its images where it has them
static void ra_material(TMat* m, const RaView* v, const RaMaterial* rm, char (*tpath)[512]) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", rm && ra_str(v, rm->name)[0] ? ra_str(v, rm->name) : "Material");
    m->nodes = calloc(48, sizeof(TNode));
    if (!m->nodes) return;
    int out = ra_node(m, "ShaderNodeOutputMaterial", 400, 0), p = ra_node(m, "ShaderNodeBsdfPrincipled", 100, 0);
    gm_link(m, p, 0, out, 0);
    if (!rm || p < 0) return;
    float* in0 = m->nodes[p].in[0];
    memcpy(in0, rm->base_color, 12);
    TNode* b = &m->nodes[p];
    b->in[4][0] = rm->base_color[3];
    b->in[1][0] = rm->metallic, b->in[2][0] = rm->roughness, b->in[3][0] = rm->ior > 0.0f ? rm->ior : 1.5f;
    b->in[7][0] = rm->subsurface, b->in[12][0] = rm->specular_ior_level, b->in[17][0] = rm->transmission;
    b->in[18][0] = rm->coat, b->in[19][0] = rm->coat_roughness, b->in[23][0] = rm->sheen, b->in[24][0] = rm->sheen_roughness;
    memcpy(b->in[26], rm->emission, 12);
    b->in[27][0] = rm->emission[0] + rm->emission[1] + rm->emission[2] > 0.0f ? rm->emission_strength : 0.0f;
    static const int TO[RA_TEX_COUNT] = { 0, 1, 2, 5, 26, 4, 17 }; // the Principled's inputs (Blender 4)
    int uvmap = -1;
    for (int k = 0; k < RA_TEX_COUNT; k++) {
        int o = 0, from = ra_texture(m, v, rm->tex[k], tpath, 300.0f - 220.0f * (float)k, &uvmap, &o);
        if (from < 0) continue;
        if (k == RA_TEX_NORMAL) { // tangent space (the Tlender's OpenGL convention) through a Normal Map
            int nm = ra_node(m, "ShaderNodeNormalMap", -250, 300.0f - 220.0f * (float)k);
            if (nm < 0) continue;
            m->nodes[nm].in[0][0] = rm->normal_strength > 0.0f ? rm->normal_strength : 1.0f;
            gm_link(m, from, o, nm, 1);
            from = nm, o = 0;
        }
        gm_link(m, from, o, p, TO[k]);
        if (k == RA_TEX_EMISSION && m->nodes[p].in[27][0] <= 0.0f) m->nodes[p].in[27][0] = rm->emission_strength > 0.0f ? rm->emission_strength : 1.0f;
    }
}

// The skeleton's bones as an armature: the rest of each (the inverse of its inverse bind) as Blender's head, tail and
// roll (Y along the bone), the pose channels at rest
static bool ra_armature(const RaView* v, uint32_t si, TArm* a) {
    const RaSkeleton* sk = RA_REC(v, RC_SKEL, RaSkeleton, si);
    const RaBone* rb = ra_at(v, sk->bones);
    memset(a, 0, sizeof *a);
    a->obj = -1;
    a->bones = calloc(sk->bone_count ? sk->bone_count : 1, sizeof(TBone));
    if (!a->bones) return false;
    a->nbones = (int)sk->bone_count;
    for (uint32_t i = 0; i < sk->bone_count; i++) {
        TBone* b = &a->bones[i];
        snprintf(b->name, sizeof b->name, "%s", ra_str(v, rb[i].name));
        b->parent = rb[i].parent;
        M4 ib, rest;
        memcpy(ib.m, rb[i].inv_bind, sizeof ib.m);
        rest = m4_inv(&ib);
        float y[3] = { rest.m[4], rest.m[5], rest.m[6] }, yl = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        float len = rb[i].length > 1e-5f ? rb[i].length : 0.1f;
        for (int k = 0; k < 3; k++) b->head[k] = rest.m[12 + k], b->tail[k] = b->head[k] + (yl > 1e-12f ? y[k] / yl : (k == 1)) * len;
        M4 r0 = bone_rest(b->head, b->tail, 0.0f);
        float x0[3] = { r0.m[0], r0.m[1], r0.m[2] }, x[3] = { rest.m[0], rest.m[1], rest.m[2] }, yn[3] = { r0.m[4], r0.m[5], r0.m[6] }, cr[3];
        v3cross(x0, x, cr);
        b->roll = atan2f(cr[0] * yn[0] + cr[1] * yn[1] + cr[2] * yn[2], x0[0] * x[0] + x0[1] * x[1] + x0[2] * x[2]);
        b->pquat[0] = 1.0f, b->pscale[0] = b->pscale[1] = b->pscale[2] = 1.0f;
    }
    return true;
}

// A sampled animation as an action: per bone track, its local pose relative to the rest (Blender's channels) keyed at
// every sample (linear), the quaternions kept on one side
static bool ra_action(const RaView* v, uint32_t ai, const TFile* f, const int* skel_of_node, TAction* act, int* node_tracks) {
    const RaAnim* an = RA_REC(v, RC_ANIM, RaAnim, ai);
    const RaTrack* tr = ra_at(v, an->tracks);
    memset(act, 0, sizeof *act);
    snprintf(act->name, sizeof act->name, "%s", ra_str(v, an->name)[0] ? ra_str(v, an->name) : "Animation");
    uint32_t nb = 0;
    for (uint32_t k = 0; k < an->track_count; k++) {
        if (tr[k].kind == RA_TRACK_BONE) {
            nb++;
            continue;
        }
        const RaNode* tn = RA_REC(v, RC_NODE, RaNode, tr[k].node); // (a skinned mesh's node moves with its bones)
        if (tn->kind == RA_MESH && (RA_REC(v, RC_MESH, RaMesh, tn->ref)->flags & RA_SKIN)) continue;
        const float* sp = ra_at(v, tr[k].samples); // (an object track that moves: the one not imported yet)
        bool moves = false;
        for (uint32_t i = 1; sp && i < tr[k].sample_count && !moves; i++)
            for (int c = 0; c < 10 && !moves; c++) moves = fabsf(sp[i * 10 + c] - sp[c]) > 1e-6f;
        *node_tracks += moves;
    }
    act->curves = calloc((size_t)nb * 10 + 1, sizeof(TCurve));
    if (!act->curves) return false;
    for (uint32_t k = 0; k < an->track_count; k++) {
        if (tr[k].kind != RA_TRACK_BONE) continue;
        int s = skel_of_node[tr[k].node];
        if (s < 0 || s >= f->narms || (int)tr[k].bone >= f->arms[s].nbones) continue;
        const TArm* a = &f->arms[s];
        const TBone* b = &a->bones[tr[k].bone];
        M4 rest = bone_rest(b->head, b->tail, b->roll), rel = rest;
        if (b->parent >= 0 && b->parent < a->nbones) {
            const TBone* pb = &a->bones[b->parent];
            M4 pr = bone_rest(pb->head, pb->tail, pb->roll), pi = m4_inv(&pr);
            rel = m4_mul(&pi, &rest);
        }
        M4 reli = m4_inv(&rel);
        uint32_t ns = tr[k].sample_count;
        TCurve* cu = &act->curves[act->ncurves];
        bool fine = true;
        for (int c = 0; c < 10; c++) {
            snprintf(cu[c].bone, sizeof cu[c].bone, "%s", b->name);
            cu[c].channel = c;
            cu[c].keys = calloc(ns ? ns : 1, sizeof(TKey));
            fine = fine && cu[c].keys;
        }
        if (!fine) {
            for (int c = 0; c < 10; c++) free(cu[c].keys);
            continue;
        }
        const float* sp = ra_at(v, tr[k].samples);
        float prev[4] = { 1, 0, 0, 0 };
        for (uint32_t i = 0; i < ns; i++) {
            M4 local = ra_trs(&sp[i * 10], &sp[i * 10 + 3], &sp[i * 10 + 7]), basis = m4_mul(&reli, &local);
            float loc[3], axes[3][3], sc[3], q[4];
            m4_split(&basis, loc, axes, sc);
            axes_quat_xyzw(axes, q); // x y z w
            float qw[4] = { q[3], q[0], q[1], q[2] };
            if (i && qw[0] * prev[0] + qw[1] * prev[1] + qw[2] * prev[2] + qw[3] * prev[3] < 0.0f)
                for (int z = 0; z < 4; z++) qw[z] = -qw[z];
            memcpy(prev, qw, sizeof prev);
            float val[10] = { loc[0], loc[1], loc[2], qw[1], qw[2], qw[3], sc[0], sc[1], sc[2], qw[0] };
            float fr = (float)an->frame_start + (float)i * (float)(an->frame_step > 0 ? an->frame_step : 1);
            for (int c = 0; c < 10; c++) {
                TKey* key = &cu[c].keys[cu[c].nkeys++];
                key->f = fr, key->v = val[c], key->lx = key->rx = fr, key->ly = key->ry = val[c], key->interp = 1;
            }
        }
        act->ncurves += 10;
    }
    // the morph targets' weights (MWGT): a curve per morph, a key per sample (the weight's own frames)
    uint32_t nm = 0;
    for (uint32_t i = 0; i < v->count[RC_MWGT]; i++) nm += RA_REC(v, RC_MWGT, RaMorphTrack, i)->anim == ai;
    act->mcurves = nm ? calloc(nm, sizeof(TCurve)) : NULL;
    for (uint32_t i = 0; act->mcurves && i < v->count[RC_MWGT]; i++) {
        const RaMorphTrack* mt = RA_REC(v, RC_MWGT, RaMorphTrack, i);
        if (mt->anim != ai) continue;
        const float* w = ra_at(v, mt->samples);
        TCurve* cu = &act->mcurves[act->nmcurves];
        snprintf(cu->bone, sizeof cu->bone, "%s", ra_str(v, RA_REC(v, RC_MRPH, RaMorph, mt->morph)->name));
        cu->keys = calloc(mt->sample_count ? mt->sample_count : 1, sizeof(TKey));
        if (!cu->keys || !w) continue;
        for (uint32_t s = 0; s < mt->sample_count; s++) {
            TKey* key = &cu->keys[cu->nkeys++];
            key->f = (float)an->frame_start + (float)s * (float)(an->frame_step > 0 ? an->frame_step : 1);
            key->v = key->ly = key->ry = w[s], key->lx = key->rx = key->f, key->interp = 1;
        }
        act->nmcurves++;
    }
    return true;
}

bool rasset_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    imprep_begin(src);
    snprintf(g_import_src, sizeof g_import_src, "%s", src), g_import_rigid = false;
    memset(r, 0, sizeof *r);
    g_vcol_meshes = 0;
    size_t size = 0;
    uint8_t* data = (uint8_t*)obj_read_file(src, &size); // (an import, not a frame)
    if (!data) {
        snprintf(r->text, sizeof r->text, "Could not open %s.", src);
        return false;
    }
    RaView v;
    char err[300];
    if (!ra_view(data, size, &v, err, sizeof err)) {
        snprintf(r->text, sizeof r->text, "%s: %s", src, err);
        free(data);
        return false;
    }
    char folder_disk[900], tdir[960];
    snprintf(folder_disk, sizeof folder_disk, "%s%s", content_root, folder_vpath + 8);
    snprintf(tdir, sizeof tdir, "%s/Textures", folder_disk);
    // the images the file carries, written as they were (PNG, JPEG, EXR...); one it lacks: where the Tlender had it
    uint32_t ntex = v.count[RC_TEXR];
    char(*tpath)[512] = calloc(ntex + 1, 512);
    for (uint32_t i = 0; tpath && i < ntex; i++) {
        const RaTexture* t = RA_REC(&v, RC_TEXR, RaTexture, i);
        const void* bytes = ra_at(&v, t->data);
        if (!bytes || !t->size) {
            snprintf(tpath[i], 512, "%s", ra_str(&v, t->path));
            continue;
        }
        char nm[200];
        snprintf(nm, sizeof nm, "%s", ra_str(&v, t->name)[0] ? ra_str(&v, t->name) : "Texture");
        for (char* c = nm; *c; c++)
            if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') *c = '_';
        if (!strrchr(nm, '.') && ra_str(&v, t->ext)[0]) {
            size_t l = strlen(nm);
            snprintf(nm + l, sizeof nm - l, ".%s", ra_str(&v, t->ext));
        }
        re_make_dirs(tdir);
        snprintf(tpath[i], 512, "%s/%s", tdir, nm);
        FILE* o = re_fopen(tpath[i], "wb");
        if (!o || fwrite(bytes, 1, (size_t)t->size, o) != t->size) tpath[i][0] = 0;
        if (o) fclose(o);
    }
    TFile f;
    memset(&f, 0, sizeof f);
    f.fps = v.count[RC_ANIM] && RA_REC(&v, RC_ANIM, RaAnim, 0)->fps > 0.0f ? RA_REC(&v, RC_ANIM, RaAnim, 0)->fps : 24.0f;
    // the materials (a default one last, for submeshes without)
    const char* base = src + strlen(src);
    while (base > src && base[-1] != '/' && base[-1] != '\\') base--;
    char stem[128];
    snprintf(stem, sizeof stem, "%s", base);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    f.nmats = (int)v.count[RC_MATL] + 1;
    f.mats = calloc((size_t)f.nmats, sizeof(TMat));
    bool fail = !f.mats || !tpath;
    for (int i = 0; !fail && i < f.nmats; i++) ra_material(&f.mats[i], &v, i < f.nmats - 1 ? RA_REC(&v, RC_MATL, RaMaterial, i) : NULL, tpath);
    if (!fail) snprintf(f.mats[f.nmats - 1].name, sizeof f.mats[0].name, "%s_Default", stem);
    // the skeletons
    f.narms = (int)v.count[RC_SKEL];
    f.arms = calloc((size_t)f.narms + 1, sizeof(TArm));
    fail = fail || !f.arms;
    for (int i = 0; !fail && i < f.narms; i++) fail = !ra_armature(&v, (uint32_t)i, &f.arms[i]);
    // the nodes: their world matrices (parents first), an object each with a mesh, a skeleton, a light or a camera
    uint32_t nn = v.count[RC_NODE], nm = v.count[RC_MESH];
    M4* world = malloc(sizeof(M4) * (nn + 1));
    int* obj_of = malloc(sizeof(int) * (nn + 1));      // node -> its object (-1)
    int* skel_of_node = malloc(sizeof(int) * (nn + 1)); // node -> the armature it shows (-1)
    int* mesh_of = malloc(sizeof(int) * (nm + 1));      // file mesh -> its TMesh (-1: not made, -2: none)
    int(*mesh_slots)[33] = calloc(nm + 1, sizeof(int[33]));
    f.objs = calloc(nn + 1, sizeof(TObj));
    f.meshes = calloc(nm + 1, sizeof(TMesh));
    fail = fail || !world || !obj_of || !skel_of_node || !mesh_of || !mesh_slots || !f.objs || !f.meshes;
    for (uint32_t i = 0; !fail && i < nm; i++) mesh_of[i] = -1;
    int default_used = 0, hidden = 0;
    for (uint32_t i = 0; !fail && i < nn; i++) {
        const RaNode* n = RA_REC(&v, RC_NODE, RaNode, i);
        M4 local = ra_trs(n->t, n->r, n->s);
        world[i] = n->parent >= 0 ? m4_mul(&world[n->parent], &local) : local;
        obj_of[i] = -1, skel_of_node[i] = n->kind == RA_SKELETON ? n->ref : -1;
        if (n->kind == RA_EMPTY) continue;
        TObj* o = &f.objs[f.nobjs];
        memset(o, 0, sizeof *o);
        o->mesh = o->arm = o->parent = o->deform = o->action = -1;
        o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
        memcpy(o->parent_inv, m4_id().m, sizeof o->parent_inv);
        o->world = world[i], o->world_done = true;
        o->split_cos = 0.866f;
        snprintf(o->name, sizeof o->name, "%s", ra_str(&v, n->name)[0] ? ra_str(&v, n->name) : "Object");
        if (n->kind == RA_SKELETON) o->type = 3, o->arm = n->ref;
        else if (n->kind == RA_LIGHT) {
            const RaLight* l = RA_REC(&v, RC_LGHT, RaLight, n->ref);
            o->type = 2, o->light_type = (int)l->type, o->light_power = l->power, o->light_radius = l->radius;
            memcpy(o->light_color, l->color, 12);
            o->spot_size = l->spot_size, o->spot_blend = l->spot_blend, o->light_shape = (int)l->area_shape;
            o->light_size = l->size_x, o->light_size_y = l->size_y;
        } else if (n->kind == RA_CAMERA) o->type = 1, o->lens = RA_REC(&v, RC_CAMR, RaCamera, n->ref)->lens_mm;
        else { // a mesh: its TMesh made once (a skinned one in the skeleton node's space, by its bind)
            uint32_t mi = (uint32_t)n->ref;
            if (mesh_of[mi] == -1) {
                int ns = 0;
                if (ra_mesh(&v, mi, f.nmats - 1, &f.meshes[f.nmeshes], mesh_slots[mi] + 1, &ns) && f.meshes[f.nmeshes].nf) {
                    mesh_slots[mi][0] = ns, mesh_of[mi] = f.nmeshes++;
                    for (int q = 0; q < ns; q++) default_used |= mesh_slots[mi][1 + q] == f.nmats - 1;
                } else {
                    TMesh* t = &f.meshes[f.nmeshes];
                    free(t->co), free(t->fstart), free(t->fsize), free(t->loops), free(t->uv), free(t->fmat), free(t->gname), free(t->gweight);
                    tmesh_free_keys(t), tmesh_free_marks(t);
                    memset(t, 0, sizeof *t);
                    mesh_of[mi] = -2;
                }
            }
            if (mesh_of[mi] < 0) continue;
            const RaMesh* rm = RA_REC(&v, RC_MESH, RaMesh, mi);
            o->type = 0, o->mesh = mesh_of[mi], o->nslots = mesh_slots[mi][0];
            memcpy(o->slots, mesh_slots[mi] + 1, sizeof(int) * (size_t)o->nslots);
            if (rm->normals) o->split_cos = 1e-4f; // (the file's normals: its hard edges as sharp edges, the angle only past 90 degrees)
            hidden += o->hidden;
            if ((rm->flags & RA_SKIN) && rm->skeleton >= 0) { // bent by its skeleton's node (the first that shows it)
                for (uint32_t k = 0; k < nn && o->deform < 0; k++) {
                    const RaNode* sn = RA_REC(&v, RC_NODE, RaNode, k);
                    if (sn->kind == RA_SKELETON && sn->ref == rm->skeleton && k < i) o->deform = obj_of[k];
                }
                if (o->deform >= 0) o->world = f.objs[o->deform].world;
            }
        }
        obj_of[i] = f.nobjs++;
    }
    f.nhidden = hidden;
    // the animations
    int node_tracks = 0;
    f.actions = calloc(v.count[RC_ANIM] + 1, sizeof(TAction));
    for (uint32_t i = 0; !fail && f.actions && i < v.count[RC_ANIM]; i++)
        if (ra_action(&v, i, &f, skel_of_node, &f.actions[f.nactions], &node_tracks)) f.nactions++;
    if (node_tracks)
        imprep_add(IMPREP_LOST, IMPREP_CAT_SCENE, stem, "Animation", TR("%d object tracks (an object moved, not a bone): not imported yet"), node_tracks);
    free(world), free(obj_of), free(skel_of_node), free(mesh_of), free(mesh_slots), free(tpath);
    bool done = false;
    if (fail || (!f.nmeshes && !f.nobjs)) {
        snprintf(r->text, sizeof r->text, fail ? "Out of memory reading %s." : "%s has no meshes.", src);
        obj_free_meshes(&f);
        for (int a = 0; f.arms && a < f.narms; a++) free(f.arms[a].bones);
        for (int a = 0; f.actions && a < f.nactions; a++) {
            for (int c = 0; c < f.actions[a].ncurves; c++) free(f.actions[a].curves[c].keys);
            free(f.actions[a].curves);
        }
        free(f.arms), free(f.actions);
    } else {
        if (!default_used) free(f.mats[--f.nmats].nodes); // (every submesh has its material: no default)
        rig_prepare(&f); // (the rests, the offsets, the evaluation order: FK only, the file has no constraints)
        done = tfile_assets(f, src, folder_vpath, content_root, w, r);
    }
    free(data);
    return done;
}
