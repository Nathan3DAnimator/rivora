// --- glTF 2.0 (.gltf + .bin / data URIs, or one .glb), included by tlend_import.c: the same scene structure as a
// .tlend, then the same assets. A node with a mesh becomes an object at the node's world transform (the hierarchy
// flattened); a mesh's primitives become its faces by material; a glTF material (metallic-roughness) becomes a
// Principled BSDF graph as Blender's importer builds it (base color x factor, the metallic-roughness texture through
// Separate Color, the normal map, emission). Y up becomes Blender's Z up (x, -z, y); the UVs' V flips. A skin becomes
// a Skeletal Mesh and its animations Animation Sequences (gl_skin). ---

#include "io_json.h"

typedef struct GlBuf {
    uint8_t* data; // NULL: missing
    size_t size;
    bool owned;
} GlBuf;

typedef struct Gl {
    const JsonValue* root;
    GlBuf* bufs;
    int nbufs;
    char dir[900];
} Gl;

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
// Base64 into a new buffer (padding and anything else skipped)
static uint8_t* b64_decode(const char* s, size_t* n) {
    size_t len = strlen(s);
    uint8_t* out = malloc(len / 4 * 3 + 3);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_val(s[i]);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v, bits += 6;
        if (bits >= 8) bits -= 8, out[o++] = (uint8_t)(acc >> bits);
    }
    *n = o;
    return out;
}

// A URI's file next to the glTF ("%20" decoded)
static void gl_uri_path(const Gl* g, const char* uri, char* out, size_t cap) {
    char dec[600];
    size_t k = 0;
    for (const char* p = uri; *p && k + 1 < sizeof dec; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            dec[k++] = (char)strtol(hex, NULL, 16), p += 2;
        } else
            dec[k++] = *p;
    }
    dec[k] = 0;
    snprintf(out, cap, "%s/%s", g->dir, dec);
}

// The bytes of a bufferView (NULL: missing / out of range)
static const uint8_t* gl_view(const Gl* g, int view, size_t* size, size_t* stride) {
    const JsonValue* v = json_at(json_get(g->root, "bufferViews"), (size_t)view);
    if (!v) return NULL;
    int b = json_get_int(v, "buffer", -1);
    size_t off = (size_t)json_get_num(v, "byteOffset", 0), len = (size_t)json_get_num(v, "byteLength", 0);
    if (b < 0 || b >= g->nbufs || !g->bufs[b].data || off + len > g->bufs[b].size) return NULL;
    *size = len;
    if (stride) *stride = (size_t)json_get_num(v, "byteStride", 0);
    return g->bufs[b].data + off;
}

static int gl_comps(const char* type) {
    if (!strcmp(type, "SCALAR")) return 1;
    if (!strcmp(type, "VEC2")) return 2;
    if (!strcmp(type, "VEC3")) return 3;
    if (!strcmp(type, "VEC4")) return 4;
    if (!strcmp(type, "MAT4")) return 16;
    return 0;
}

// An accessor's elements as floats (n comps each, the missing ones 0; normalized integers to 0..1 / -1..1). Its count,
// 0 when it can't be read. out: a new array (free)
static uint32_t gl_accessor(const Gl* g, int acc, int n, float** out);

// A light's or a camera's rotator from where it faces and its up (the Engine's actors face +X with Z up); glTF and FBX
static void imp_facing_rotator(float fwd[3], float up[3], float rot[3]) {
    float ax[3][3];
    float lf = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    for (int k = 0; k < 3; k++) fwd[k] /= lf > 1e-12f ? lf : 1.0f;
    float d = up[0] * fwd[0] + up[1] * fwd[1] + up[2] * fwd[2];
    for (int k = 0; k < 3; k++) up[k] -= fwd[k] * d;
    float lu = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (lu < 1e-6f) up[0] = -fwd[2], up[1] = 0.0f, up[2] = fwd[0], lu = sqrtf(up[0] * up[0] + up[2] * up[2]) + 1e-12f; // (up along the view)
    for (int k = 0; k < 3; k++) up[k] /= lu;
    memcpy(ax[0], fwd, 12), memcpy(ax[2], up, 12);
    ax[1][0] = up[1] * fwd[2] - up[2] * fwd[1], ax[1][1] = up[2] * fwd[0] - up[0] * fwd[2], ax[1][2] = up[0] * fwd[1] - up[1] * fwd[0];
    axes_rotator(ax, rot);
}

// Morph targets (blend shapes; render.h RMESH_MORPHS): kept, a Skeletal Mesh's (a mesh without a skin becomes one of a
// bone), set at run time (CSkeletalMesh.morph_weight, Set Morph Target). A mesh's target t: its name (mesh.extras.
// targetNames, the exporters' convention; else Target_t) and its default weight (mesh.weights)
static int gl_morph_count(const JsonValue* gm) {
    int n = 0;
    const JsonValue* prims = json_get(gm, "primitives");
    for (size_t p = 0; p < json_len(prims); p++) {
        int c = (int)json_len(json_get(json_at(prims, p), "targets"));
        if (c > n) n = c;
    }
    return n < RMESH_MAX_MORPHS ? n : RMESH_MAX_MORPHS;
}
static void gl_morph_name(const JsonValue* gm, int t, char* out, size_t cap, float* weight) {
    const char* nm = json_str(json_at(json_get(json_get(gm, "extras"), "targetNames"), (size_t)t), "");
    if (nm[0]) snprintf(out, cap, "%s", nm);
    else snprintf(out, cap, "Target_%d", t);
    *weight = (float)json_num(json_at(json_get(gm, "weights"), (size_t)t), 0.0);
}

// A vertex color (linear floats) as RGBA8 with sRGB color channels (the precision where the eye sees it) and linear alpha
static uint32_t gl_color_pack(const float c[4], bool rgb_only) {
    uint32_t o = 0;
    for (int k = 0; k < 4; k++) {
        float x = k == 3 ? (rgb_only ? 1.0f : c[3]) : c[k];
        x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
        if (k < 3) x = x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
        o |= (uint32_t)(x * 255.0f + 0.5f) << (8 * k);
    }
    return o;
}

static uint32_t gl_accessor(const Gl* g, int acc, int n, float** out) {
    *out = NULL;
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (size_t)acc);
    if (!a) return 0;
    uint32_t count = (uint32_t)json_get_num(a, "count", 0);
    int ct = json_get_int(a, "componentType", 0), nc = gl_comps(json_get_str(a, "type", ""));
    bool norm = json_get_bool(a, "normalized", false);
    int csize = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : ct == 5121 || ct == 5120 ? 1 : 0;
    if (!count || !nc || !csize) return 0;
    float* f = calloc((size_t)count * (size_t)n, sizeof(float));
    if (!f) return 0;
    *out = f;
    int view = json_get_int(a, "bufferView", -1);
    if (view < 0) return count; // (all zeros: a sparse accessor's base)
    size_t vsize = 0, stride = 0;
    const uint8_t* base = gl_view(g, view, &vsize, &stride);
    size_t off = (size_t)json_get_num(a, "byteOffset", 0), elem = (size_t)csize * (size_t)nc;
    if (!stride) stride = elem;
    if (!base || off + stride * (count - 1) + elem > vsize) {
        free(f), *out = NULL;
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = base + off + stride * i;
        for (int c = 0; c < nc && c < n; c++) {
            const uint8_t* q = p + (size_t)c * (size_t)csize;
            float v;
            switch (ct) {
            case 5126: memcpy(&v, q, 4); break;
            case 5125: { uint32_t u; memcpy(&u, q, 4); v = (float)u; break; }
            case 5123: { uint16_t u; memcpy(&u, q, 2); v = norm ? u / 65535.0f : u; break; }
            case 5122: { int16_t u; memcpy(&u, q, 2); v = norm ? fmaxf(u / 32767.0f, -1.0f) : u; break; }
            case 5121: v = norm ? q[0] / 255.0f : q[0]; break;
            default: v = norm ? fmaxf((int8_t)q[0] / 127.0f, -1.0f) : (int8_t)q[0]; break;
            }
            f[(size_t)i * n + c] = v;
        }
    }
    return count;
}

// An index accessor as uint32 (exact for any size)
static uint32_t gl_indices(const Gl* g, int acc, uint32_t** out) {
    *out = NULL;
    const JsonValue* a = json_at(json_get(g->root, "accessors"), (size_t)acc);
    if (!a) return 0;
    uint32_t count = (uint32_t)json_get_num(a, "count", 0);
    int ct = json_get_int(a, "componentType", 0), cs = ct == 5125 ? 4 : ct == 5123 ? 2 : ct == 5121 ? 1 : 0;
    size_t vsize = 0, stride = 0;
    const uint8_t* base = gl_view(g, json_get_int(a, "bufferView", -1), &vsize, &stride);
    size_t off = (size_t)json_get_num(a, "byteOffset", 0);
    if (!count || !cs || !base || off + (size_t)cs * count > vsize) return 0;
    uint32_t* r = malloc(sizeof(uint32_t) * count);
    if (!r) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* q = base + off + (size_t)cs * i;
        if (cs == 4) memcpy(&r[i], q, 4);
        else if (cs == 2) { uint16_t u; memcpy(&u, q, 2); r[i] = u; }
        else r[i] = q[0];
    }
    *out = r;
    return count;
}

// --- Materials ---

// A node with its definition's defaults (every input, the properties as the node's own)
static int gm_node(TMat* m, const char* idname, float x, float y) {
    const MatNodeDef* d = mat_node_def_by_idname(idname);
    if (!d || m->nnodes >= 16) return -1;
    TNode* n = &m->nodes[m->nnodes];
    memset(n, 0, sizeof *n);
    snprintf(n->idname, sizeof n->idname, "%s", idname);
    n->x = x, n->y = y, n->nin = d->nin;
    for (int q = 0; q < d->nin && q < 32; q++) memcpy(n->in[q], d->in[q].def, 16);
    for (int q = 0; q < 4; q++) n->prop[q] = -1;
    return m->nnodes++;
}
static void gm_link(TMat* m, int from, int out, int to, int in) {
    if (from < 0 || to < 0 || m->nlinks >= 256) return;
    int* l = m->links[m->nlinks++];
    l[0] = from, l[1] = out, l[2] = to, l[3] = in;
}

// A textureInfo ({"index": t}) as an Image Texture: its image's path, the sampler's wrap and filter. -1: none
static int gm_texture(TMat* m, const Gl* g, const JsonValue* info, char (*img_path)[512], int nimg, bool color, float x, float y) {
    if (!info) return -1;
    const JsonValue* t = json_at(json_get(g->root, "textures"), (size_t)json_get_int(info, "index", -1));
    int src = t ? json_get_int(t, "source", -1) : -1;
    if (src < 0 && t) { // (the image of an extension: WebP / KTX2 fallbacks name it there)
        const JsonValue* ext = json_get(t, "extensions");
        for (uint32_t k = 0; ext && ext->type == JSON_OBJECT && k < ext->count && src < 0; k++) src = json_get_int(&ext->members[k].value, "source", -1);
    }
    if (src < 0 || src >= nimg || !img_path[src][0]) return -1;
    int n = gm_node(m, "ShaderNodeTexImage", x, y);
    if (n < 0) return -1;
    TNode* tn = &m->nodes[n];
    snprintf(tn->image, sizeof tn->image, "%s", img_path[src]);
    tn->prop[2] = color ? IMAGE_COLORSPACE_SRGB : IMAGE_COLORSPACE_NONCOLOR;
    const JsonValue* s = json_at(json_get(g->root, "samplers"), (size_t)json_get_int(t, "sampler", -1));
    if (s) {
        int wrap = json_get_int(s, "wrapS", 10497);
        tn->prop[1] = wrap == 33071 ? IMAGE_EXT_EXTEND : wrap == 33648 ? IMAGE_EXT_MIRROR : IMAGE_EXT_REPEAT;
        if (json_get_int(s, "magFilter", 9729) == 9728) tn->prop[0] = IMAGE_INTERP_CLOSEST;
    }
    return n;
}

// out of node `from` times f (a Mix multiply for colors, a Math multiply for values) when f isn't 1; the node and
// output to link from
static void gm_scale(TMat* m, int* from, int* out, const float* f, bool color, float x, float y) {
    if (color ? (f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f) : f[0] == 1.0f) return;
    int k = gm_node(m, color ? "ShaderNodeMix" : "ShaderNodeMath", x, y);
    if (k < 0) return;
    TNode* n = &m->nodes[k];
    if (color) {
        n->prop[0] = MIX_DATA_COLOR, n->prop[1] = BLEND_MUL;
        n->in[0][0] = 1.0f;                                              // Factor
        n->in[6][0] = f[0], n->in[6][1] = f[1], n->in[6][2] = f[2];      // B_Color
        gm_link(m, *from, *out, k, 5);                                   // -> A_Color
        *from = k, *out = 2;                                             // Result_Color
    } else {
        n->prop[0] = MATH_MULTIPLY;
        n->in[1][0] = f[0];
        gm_link(m, *from, *out, k, 0);
        *from = k, *out = 0;
    }
}

static void gl_material(TMat* m, const Gl* g, const JsonValue* gm, int index, char (*img_path)[512], int nimg) {
    memset(m, 0, sizeof *m);
    const char* name = json_get_str(gm, "name", "");
    if (name[0]) snprintf(m->name, sizeof m->name, "%s", name);
    else snprintf(m->name, sizeof m->name, "Material_%d", index);
    m->nodes = calloc(16, sizeof(TNode));
    if (!m->nodes) return;
    int out = gm_node(m, "ShaderNodeOutputMaterial", 400, 0);
    int p = gm_node(m, "ShaderNodeBsdfPrincipled", 100, 0);
    gm_link(m, p, 0, out, 0);
    if (!gm) return; // (the default material: Principled's defaults)
    TNode* bsdf = &m->nodes[p];
    const JsonValue* pbr = json_get(gm, "pbrMetallicRoughness");
    float bc[4] = { 1, 1, 1, 1 }, metal = 1.0f, rough = 1.0f, em[3] = { 0, 0, 0 };
    json_get_floats(pbr, "baseColorFactor", bc, 4);
    metal = (float)json_get_num(pbr, "metallicFactor", 1.0);
    rough = (float)json_get_num(pbr, "roughnessFactor", 1.0);
    json_get_floats(gm, "emissiveFactor", em, 3);
    const char* alpha_mode = json_get_str(gm, "alphaMode", "OPAQUE");
    bool alpha = strcmp(alpha_mode, "OPAQUE") != 0;
    // base color (and alpha)
    memcpy(bsdf->in[0], bc, 12);
    bsdf->in[4][0] = alpha ? bc[3] : 1.0f;
    int t = gm_texture(m, g, json_get(pbr, "baseColorTexture"), img_path, nimg, true, -500, 200);
    if (t >= 0) {
        int from = t, o = 0;
        gm_scale(m, &from, &o, bc, true, -200, 250);
        gm_link(m, from, o, p, 0);
        if (alpha) gm_link(m, t, 1, p, 4);
    }
    // metallic (B) and roughness (G)
    bsdf = &m->nodes[p];
    bsdf->in[1][0] = metal, bsdf->in[2][0] = rough;
    t = gm_texture(m, g, json_get(pbr, "metallicRoughnessTexture"), img_path, nimg, false, -700, -50);
    int sep = t >= 0 ? gm_node(m, "ShaderNodeSeparateColor", -400, -50) : -1;
    if (sep >= 0) {
        gm_link(m, t, 0, sep, 0);
        int from = sep, o = 2;
        gm_scale(m, &from, &o, &metal, false, -200, 0);
        gm_link(m, from, o, p, 1);
        from = sep, o = 1;
        gm_scale(m, &from, &o, &rough, false, -200, -100);
        gm_link(m, from, o, p, 2);
    }
    // normal map
    const JsonValue* ni = json_get(gm, "normalTexture");
    t = gm_texture(m, g, ni, img_path, nimg, false, -700, -300);
    int nm = t >= 0 ? gm_node(m, "ShaderNodeNormalMap", -300, -300) : -1;
    if (nm >= 0) {
        m->nodes[nm].in[0][0] = (float)json_get_num(ni, "scale", 1.0);
        gm_link(m, t, 0, nm, 1);
        gm_link(m, nm, 0, p, 5);
    }
    // emission (KHR_materials_emissive_strength), IOR, transmission
    const JsonValue* ext = json_get(gm, "extensions");
    float strength = (float)json_get_num(json_get(ext, "KHR_materials_emissive_strength"), "emissiveStrength", 1.0);
    bsdf = &m->nodes[p];
    memcpy(bsdf->in[26], em, 12);
    bsdf->in[27][0] = em[0] + em[1] + em[2] > 0.0f ? strength : 0.0f;
    t = gm_texture(m, g, json_get(gm, "emissiveTexture"), img_path, nimg, true, -500, -550);
    if (t >= 0 && em[0] + em[1] + em[2] > 0.0f) {
        int from = t, o = 0;
        gm_scale(m, &from, &o, em, true, -200, -550);
        gm_link(m, from, o, p, 26);
        m->nodes[p].in[27][0] = strength;
    }
    const JsonValue* ior = json_get(ext, "KHR_materials_ior");
    if (ior) m->nodes[p].in[3][0] = (float)json_get_num(ior, "ior", 1.5);
    const JsonValue* tr = json_get(ext, "KHR_materials_transmission");
    if (tr) m->nodes[p].in[17][0] = (float)json_get_num(tr, "transmissionFactor", 0.0);
    const JsonValue* cc = json_get(ext, "KHR_materials_clearcoat");
    if (cc) m->nodes[p].in[18][0] = (float)json_get_num(cc, "clearcoatFactor", 0.0), m->nodes[p].in[19][0] = (float)json_get_num(cc, "clearcoatRoughnessFactor", 0.0);
    if ((tr && m->nodes[p].in[17][0] > 0.0f) || (cc && m->nodes[p].in[18][0] > 0.0f) || json_get(ext, "KHR_materials_sheen") ||
        json_get(ext, "KHR_materials_specular") || json_get(ext, "KHR_materials_anisotropy") || json_get(ext, "KHR_materials_iridescence") ||
        json_get(ext, "KHR_materials_volume"))
        printf("[RealEngine] Aviso: glTF: o material %s usa %s%s%s%s%s%s, ainda sem efeito na Real Engine (o valor fica guardado no Principled BSDF)\n", m->name,
               tr ? "transmissao " : "", cc ? "verniz (clearcoat) " : "", json_get(ext, "KHR_materials_sheen") ? "aveludado (sheen) " : "",
               json_get(ext, "KHR_materials_specular") ? "especular " : "", json_get(ext, "KHR_materials_anisotropy") ? "anisotropia " : "",
               json_get(ext, "KHR_materials_iridescence") || json_get(ext, "KHR_materials_volume") ? "iridescencia/volume" : "");
    const JsonValue* sh = json_get(ext, "KHR_materials_sheen");
    float sheen[3] = { 0, 0, 0 };
    if (sh && json_get_floats(sh, "sheenColorFactor", sheen, 3) && sheen[0] + sheen[1] + sheen[2] > 0.0f) {
        m->nodes[p].in[23][0] = 1.0f, memcpy(m->nodes[p].in[25], sheen, 12);
        m->nodes[p].in[24][0] = (float)json_get_num(sh, "sheenRoughnessFactor", 0.0);
    }
    const JsonValue* sp = json_get(ext, "KHR_materials_specular"); // (Blender's: IOR Level 0.5 is the factor 1)
    if (sp) {
        m->nodes[p].in[12][0] = 0.5f * (float)json_get_num(sp, "specularFactor", 1.0);
        json_get_floats(sp, "specularColorFactor", m->nodes[p].in[13], 3);
    }
}

// --- The scene ---

// glTF -> Blender: (x, y, z) -> (x, -z, y)
static void gl_to_blender(const float* p, float* o) { o[0] = p[0], o[1] = -p[2], o[2] = p[1]; }

static M4 gl_node_local(const JsonValue* n) {
    float mat[16];
    if (json_get_floats(n, "matrix", mat, 16) == 16) {
        M4 r;
        memcpy(r.m, mat, sizeof mat); // (column-major, as M4)
        return r;
    }
    float t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
    json_get_floats(n, "translation", t, 3);
    json_get_floats(n, "rotation", q, 4);
    json_get_floats(n, "scale", s, 3);
    float wxyz[4] = { q[3], q[0], q[1], q[2] };
    M4 r = m4_quat(wxyz);
    return m4_trs(t, &r, s);
}

// C G C^-1 (C: glTF -> Blender axes)
static M4 gl_matrix_to_blender(const M4* g) {
    // C maps rows (x, y, z) -> (x, -z, y): r = C g C^T
    static const float C[3][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }; // C[row][col]
    M4 r = m4_id();
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) {
            float s = 0.0f;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) s += C[row][i] * g->m[j * 4 + i] * C[col][j];
            r.m[col * 4 + row] = s;
        }
    float t[3] = { g->m[12], g->m[13], g->m[14] };
    gl_to_blender(t, &r.m[12]);
    return r;
}

typedef struct GlBuild {
    Gl* g;
    TFile* f;
    int* mesh_of;         // per glTF mesh: its TMesh (-1: not made yet / none)
    int (*mesh_slots)[33]; // per glTF mesh: its slot count, then the materials (f->mats indices)
    size_t cap_objs;
    bool fail, default_used;
    // KHR_lights_punctual: the lights (made after the assets, in the Engine's space)
    struct { CLight l; float loc[3], rot[3]; } lights[256];
    int nlights;
    struct { float fov, loc[3], rot[3]; char name[64]; } cams[64]; // the cameras (a Camera actor each)
    int ncams;
    // the mesh nodes with a skin, and the ones under a joint (bone_node: that joint, -1: skinned): gl_skin, after the assets
    struct { int node, mesh, skin, bone_node; } skinned[64];
    int nskinned;
    int* node_skin; // per node: the skin it is a joint of (-1)
} GlBuild;

// A glTF mesh's primitives (triangles, strips, fans) into a TMesh; its index (-1: no faces)
static int gl_mesh(GlBuild* b, int mi) {
    if (b->mesh_of[mi] != -2) return b->mesh_of[mi];
    b->mesh_of[mi] = -1;
    const JsonValue* gm = json_at(json_get(b->g->root, "meshes"), (size_t)mi);
    const JsonValue* prims = json_get(gm, "primitives");
    TFile* f = b->f;
    TMesh me;
    memset(&me, 0, sizeof me);
    size_t cap_v = 0, cap_f = 0, cap_c = 0, cap_uv = 0, cap_f2 = 0, cap_f3 = 0, cap_k = 0, cap_col = 0;
    int* slots = b->mesh_slots[mi];
    slots[0] = 0;
    uint32_t nkeys = (uint32_t)gl_morph_count(gm);
    if (nkeys) {
        me.kname = calloc(nkeys, RMESH_MORPH_NAME), me.kvalue = calloc(nkeys, sizeof(float));
        if (!me.kname || !me.kvalue) nkeys = 0, free(me.kname), free(me.kvalue), me.kname = NULL, me.kvalue = NULL;
        for (uint32_t t = 0; t < nkeys; t++) gl_morph_name(gm, (int)t, me.kname[t], RMESH_MORPH_NAME, &me.kvalue[t]);
    }
    for (size_t pi = 0; pi < json_len(prims) && !b->fail; pi++) {
        const JsonValue* pr = json_at(prims, pi);
        int mode = json_get_int(pr, "mode", 4);
        if (mode != 4 && mode != 5 && mode != 6) continue; // (points and lines: no faces)
        const JsonValue* at = json_get(pr, "attributes");
        float *pos = NULL, *uv = NULL, *vc = NULL;
        uint32_t nv = gl_accessor(b->g, json_get_int(at, "POSITION", -1), 3, &pos);
        uint32_t nuv = json_get(at, "TEXCOORD_0") ? gl_accessor(b->g, json_get_int(at, "TEXCOORD_0", -1), 2, &uv) : 0;
        // the vertex colors (COLOR_0, linear: kept as sRGB bytes, the material's Color Attribute; VEC3: alpha 1)
        uint32_t nvc = json_get(at, "COLOR_0") ? gl_accessor(b->g, json_get_int(at, "COLOR_0", -1), 4, &vc) : 0;
        bool vc_rgb = nvc && !strcmp(json_get_str(json_at(json_get(b->g->root, "accessors"), (size_t)json_get_int(at, "COLOR_0", -1)), "type", ""), "VEC3");
        uint32_t* idx = NULL;
        uint32_t ni = json_get(pr, "indices") ? gl_indices(b->g, json_get_int(pr, "indices", -1), &idx) : 0;
        if (!json_get(pr, "indices") && nv) { // (no indices: the vertices in order)
            idx = malloc(sizeof(uint32_t) * nv);
            if (idx)
                for (uint32_t k = 0; k < nv; k++) idx[k] = k;
            ni = idx ? nv : 0;
        }
        uint32_t ntri = mode == 4 ? ni / 3 : ni >= 3 ? ni - 2 : 0;
        if (!nv || !ntri) {
            free(pos), free(uv), free(idx);
            continue;
        }
        // the slot: this primitive's material among the mesh's
        int mat = json_get_int(pr, "material", -1);
        if (mat < 0 || mat >= f->nmats - 1) mat = f->nmats - 1, b->default_used = true; // (the last: the default material)
        int slot = -1;
        for (int q = 0; q < slots[0]; q++)
            if (slots[1 + q] == mat) slot = q;
        if (slot < 0 && slots[0] < 32) slot = slots[0], slots[1 + slots[0]++] = mat;
        if (slot < 0) slot = 0;
        uint32_t v0 = me.nv;
        bool ok = obj_grow((void**)&me.co, &cap_v, ((size_t)me.nv + nv) * 3, sizeof(float)) &&
                  obj_grow((void**)&me.fstart, &cap_f, (size_t)me.nf + ntri, sizeof(uint32_t)) &&
                  obj_grow((void**)&me.fsize, &cap_f2, (size_t)me.nf + ntri, sizeof(uint32_t)) &&
                  obj_grow((void**)&me.fmat, &cap_f3, (size_t)me.nf + ntri, sizeof(uint16_t)) &&
                  obj_grow((void**)&me.loops, &cap_c, (size_t)me.ncorners + ntri * 3, sizeof(uint32_t)) &&
                  obj_grow((void**)&me.uv, &cap_uv, ((size_t)me.ncorners + ntri * 3) * 2, sizeof(float));
        if (ok && (nvc || me.col)) { // (a primitive with colors: the mesh's corners all get one, white where it had none)
            bool first = !me.col;
            ok = obj_grow((void**)&me.col, &cap_col, (size_t)me.ncorners + ntri * 3, sizeof(uint32_t));
            if (ok && first)
                for (uint32_t q = 0; q < me.ncorners; q++) me.col[q] = 0xFFFFFFFFu;
        }
        if (!ok) {
            b->fail = true;
            free(pos), free(uv), free(idx), free(vc);
            break;
        }
        for (uint32_t k = 0; k < nv; k++) gl_to_blender(&pos[k * 3], &me.co[(v0 + k) * 3]);
        if (nkeys) { // its morph targets' offsets (Blender's space, as the vertices), the targets it lacks none
            if (!obj_grow((void**)&me.kdelta, &cap_k, ((size_t)me.nv + nv) * nkeys * 3, sizeof(float))) {
                b->fail = true;
                free(pos), free(uv), free(idx);
                break;
            }
            memset(&me.kdelta[(size_t)v0 * nkeys * 3], 0, sizeof(float) * nv * nkeys * 3);
            const JsonValue* tg = json_get(pr, "targets");
            for (uint32_t t = 0; t < nkeys && t < json_len(tg); t++) {
                float* d = NULL;
                const JsonValue* tt = json_at(tg, t);
                uint32_t n = json_get(tt, "POSITION") ? gl_accessor(b->g, json_get_int(tt, "POSITION", -1), 3, &d) : 0;
                for (uint32_t k = 0; k < n && k < nv; k++) gl_to_blender(&d[k * 3], &me.kdelta[(((size_t)v0 + k) * nkeys + t) * 3]);
                free(d);
            }
        }
        me.nv += nv;
        for (uint32_t t = 0; t < ntri; t++) {
            uint32_t a, c1, c2; // the triangle's vertices (strips alternate the winding; fans turn about the first)
            if (mode == 4) a = idx[t * 3], c1 = idx[t * 3 + 1], c2 = idx[t * 3 + 2];
            else if (mode == 5) a = idx[t], c1 = idx[t + 1 + (t & 1)], c2 = idx[t + 2 - (t & 1)];
            else a = idx[0], c1 = idx[t + 1], c2 = idx[t + 2];
            if (a >= nv || c1 >= nv || c2 >= nv) continue;
            uint32_t tri[3] = { a, c1, c2 }, c = me.ncorners;
            me.fstart[me.nf] = c, me.fsize[me.nf] = 3, me.fmat[me.nf] = (uint16_t)slot;
            for (int q = 0; q < 3; q++) {
                me.loops[c + q] = v0 + tri[q];
                bool has = tri[q] < nuv;
                me.uv[(c + q) * 2] = has ? uv[tri[q] * 2] : 0.0f;
                me.uv[(c + q) * 2 + 1] = has ? 1.0f - uv[tri[q] * 2 + 1] : 0.0f; // (glTF's V runs down)
                if (me.col) me.col[c + q] = tri[q] < nvc ? gl_color_pack(&vc[tri[q] * 4], vc_rgb) : 0xFFFFFFFFu;
            }
            me.ncorners += 3, me.nf++;
        }
        free(pos), free(uv), free(idx), free(vc);
    }
    if (nkeys && me.kdelta) me.nkeys = nkeys;
    else tmesh_free_keys(&me);
    if (!me.nf || b->fail) {
        free(me.co), free(me.fstart), free(me.fsize), free(me.fmat), free(me.loops), free(me.uv), free(me.col), tmesh_free_keys(&me);
        return -1;
    }
    size_t cap = (size_t)f->nmeshes;
    if (!obj_grow((void**)&f->meshes, &cap, (size_t)f->nmeshes + 1, sizeof(TMesh))) {
        b->fail = true;
        free(me.co), free(me.fstart), free(me.fsize), free(me.fmat), free(me.loops), free(me.uv), free(me.col), tmesh_free_keys(&me);
        return -1;
    }
    f->meshes[f->nmeshes] = me;
    return b->mesh_of[mi] = f->nmeshes++;
}

// A node and its children: an object per node with a mesh, at its world transform (a skinned one: kept for gl_skin) (a skinned one: kept for gl_skin)
static void gl_visit(GlBuild* b, int ni, const M4* parent, int depth, int joint_anc) {
    const JsonValue* n = json_at(json_get(b->g->root, "nodes"), (size_t)ni);
    if (!n || depth > 64 || b->fail) return;
    M4 local = gl_node_local(n), world = m4_mul(parent, &local);
    int mi = json_get_int(n, "mesh", -1), si = json_get_int(n, "skin", -1), nmeshes = (int)json_len(json_get(b->g->root, "meshes"));
    const JsonValue* skins = json_get(b->g->root, "skins");
    int anc = b->node_skin && b->node_skin[ni] >= 0 ? ni : joint_anc; // (the nearest joint at or over it)
    if (si < 0 && anc >= 0) si = b->node_skin[anc]; // a mesh under a joint (a hat on the head): a part of that bone
    else anc = -1;
    if (mi >= 0 && mi < nmeshes && si >= 0 && si < (int)json_len(skins) && b->nskinned < 64 &&
        json_len(json_get(json_at(skins, (size_t)si), "joints")) <= ANIM_MAX_BONES) { // (over the bones a Skeletal Mesh holds: static)
        b->skinned[b->nskinned].node = ni, b->skinned[b->nskinned].mesh = mi, b->skinned[b->nskinned].skin = si;
        b->skinned[b->nskinned].bone_node = anc, b->nskinned++;
        const JsonValue* prims = json_get(json_at(json_get(b->g->root, "meshes"), (size_t)mi), "primitives");
        for (size_t k = 0; k < json_len(prims); k++)
            if (json_get_int(json_at(prims, k), "material", -1) < 0) b->default_used = true;
        mi = -1;
    }
    if (mi >= 0 && mi < nmeshes) {
        int me = gl_mesh(b, mi);
        TFile* f = b->f;
        if (me >= 0 && obj_grow((void**)&f->objs, &b->cap_objs, (size_t)f->nobjs + 1, sizeof(TObj))) {
            TObj* o = &f->objs[f->nobjs];
            memset(o, 0, sizeof *o);
            o->mesh = me, o->arm = o->parent = o->deform = o->action = -1;
            o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
            memcpy(o->parent_inv, m4_id().m, sizeof o->parent_inv);
            o->world = gl_matrix_to_blender(&world), o->world_done = true;
            const int* s = b->mesh_slots[mi];
            o->nslots = s[0];
            memcpy(o->slots, s + 1, sizeof(int) * (size_t)s[0]);
            // its name: the node's (or its mesh's), made unique (the assets are named after it)
            const char* nm = json_get_str(n, "name", json_get_str(json_at(json_get(b->g->root, "meshes"), (size_t)mi), "name", "Mesh"));
            snprintf(o->name, sizeof o->name, "%s", nm[0] ? nm : "Mesh");
            for (int k = 2, dup = 1; dup && k < 10000; k++) {
                dup = 0;
                for (int q = 0; q < f->nobjs && !dup; q++) dup = !strcmp(f->objs[q].name, o->name);
                if (dup) snprintf(o->name, sizeof o->name, "%.56s_%d", nm[0] ? nm : "Mesh", k);
            }
            f->nobjs++;
        } else if (me >= 0)
            b->fail = true;
    }
    // its light: glTF's shines down -Z with Y up; the Engine's down +X with Z up
    const JsonValue* lt = json_at(json_get(json_get(json_get(b->g->root, "extensions"), "KHR_lights_punctual"), "lights"),
                                  (size_t)json_get_int(json_get(json_get(n, "extensions"), "KHR_lights_punctual"), "light", -1));
    if (lt && b->nlights < 256) {
        const char* type = json_get_str(lt, "type", "point");
        CLight* l = &b->lights[b->nlights].l;
        memset(l, 0, sizeof *l);
        l->type = !strcmp(type, "directional") ? LIGHT_DIRECTIONAL : !strcmp(type, "spot") ? LIGHT_SPOT : LIGHT_POINT;
        l->color[0] = l->color[1] = l->color[2] = 1.0f;
        json_get_floats(lt, "color", l->color, 3);
        l->intensity = (float)json_get_num(lt, "intensity", 1.0); // (glTF's: candela, lux for directional: the Engine's)
        // its reach: the range, else where it falls under 0.05 lux (5..50 m)
        float range = (float)json_get_num(lt, "range", 0.0);
        l->attenuation_radius = range > 0.0f ? range * 100.0f : fminf(fmaxf(sqrtf(fmaxf(l->intensity, 0.0f) / 0.05f), 5.0f), 50.0f) * 100.0f;
        const JsonValue* spot = json_get(lt, "spot");
        l->inner_cone = (float)json_get_num(spot, "innerConeAngle", 0.0) * 57.29578f;
        l->outer_cone = (float)json_get_num(spot, "outerConeAngle", 0.7853982) * 57.29578f;
        l->cast_shadows = 1;
        M4 bl = gl_matrix_to_blender(&world), e = m4_to_engine(&bl);
        float fwd[3], up[3];
        m4_dir(&e, (float[]){ 0, -1, 0 }, fwd), m4_dir(&e, (float[]){ 0, 0, 1 }, up);
        memcpy(b->lights[b->nlights].loc, &e.m[12], 12);
        imp_facing_rotator(fwd, up, b->lights[b->nlights].rot);
        b->nlights++;
    }
    const JsonValue* cam = json_at(json_get(b->g->root, "cameras"), (size_t)json_get_int(n, "camera", -1));
    if (cam && b->ncams < 64) {
        const JsonValue* per = json_get(cam, "perspective");
        float yfov = (float)json_get_num(per, "yfov", 0.8), aspect = (float)json_get_num(per, "aspectRatio", 16.0 / 9.0);
        b->cams[b->ncams].fov = per ? 2.0f * atanf(tanf(yfov * 0.5f) * aspect) * 57.29578f : 60.0f; // (orthographic: a perspective one)
        snprintf(b->cams[b->ncams].name, 64, "%s", json_get_str(n, "name", json_get_str(cam, "name", "CameraActor")));
        M4 bl = gl_matrix_to_blender(&world), e = m4_to_engine(&bl);
        float fwd[3], up[3];
        m4_dir(&e, (float[]){ 0, -1, 0 }, fwd), m4_dir(&e, (float[]){ 0, 0, 1 }, up);
        memcpy(b->cams[b->ncams].loc, &e.m[12], 12);
        imp_facing_rotator(fwd, up, b->cams[b->ncams].rot);
        b->ncams++;
    }
    const JsonValue* ch = json_get(n, "children");
    int below = b->node_skin && b->node_skin[ni] >= 0 ? ni : joint_anc;
    for (size_t k = 0; k < json_len(ch); k++) gl_visit(b, json_int(json_at(ch, k), -1), &world, depth + 1, below);
}

// --- Skins: a Skeletal Mesh per skin. Its joints are the bones (each one's parent: its nearest ancestor that is a
// joint), their rest the inverse bind matrices; a part per joint and material (the faces whose corners lean most on
// that joint, in its space: the Engine's skeletal meshes are rigid parts); the animations that move its joints (or
// their ancestors) baked at 30 fps from the node hierarchy, each joint's world relative to its parent joint's. ---

#define GL_ANIM_FPS 30.0f

// glTF's space -> the Engine's (Blender's axes, then centimeters and Y mirrored)
static M4 gl_to_engine_m(const M4* g) {
    M4 bl = gl_matrix_to_blender(g);
    return m4_to_engine(&bl);
}
static void gl_to_engine_p(const float* p, float* o) {
    float b[3];
    gl_to_blender(p, b);
    point_to_engine(b, o);
}

typedef struct GlSampler {
    float* in;  // times
    float* out; // values (CUBICSPLINE: in-tangent, value, out-tangent per key)
    uint32_t n;
    int interp; // 0 linear, 1 step, 2 cubic
} GlSampler;

// A sampler's value at t (n components; a rotation renormalized)
static void gl_sample(const GlSampler* s, float t, int n, bool rot, float* o) {
    uint32_t k = s->n;
    int stride = s->interp == 2 ? 3 : 1, mid = s->interp == 2 ? 1 : 0;
    const float* v = s->out;
#define GL_KEY(i) (v + ((size_t)(i) * stride + mid) * n)
    if (!k) return;
    if (t <= s->in[0] || k == 1) {
        memcpy(o, GL_KEY(0), sizeof(float) * n);
    } else if (t >= s->in[k - 1]) {
        memcpy(o, GL_KEY(k - 1), sizeof(float) * n);
    } else {
        uint32_t lo = 0, hi = k - 1;
        while (hi - lo > 1) {
            uint32_t m = (lo + hi) / 2;
            if (s->in[m] <= t) lo = m;
            else hi = m;
        }
        float dt = s->in[hi] - s->in[lo], u = dt > 1e-9f ? (t - s->in[lo]) / dt : 0.0f;
        const float *a = GL_KEY(lo), *b = GL_KEY(hi);
        if (s->interp == 1) {
            memcpy(o, a, sizeof(float) * n);
        } else if (s->interp == 2) { // Hermite with the keys' tangents (out of lo, in of hi)
            const float* ta = v + ((size_t)lo * 3 + 2) * n;
            const float* tb = v + ((size_t)hi * 3) * n;
            float u2 = u * u, u3 = u2 * u;
            for (int c = 0; c < n; c++)
                o[c] = (2 * u3 - 3 * u2 + 1) * a[c] + (u3 - 2 * u2 + u) * dt * ta[c] + (-2 * u3 + 3 * u2) * b[c] + (u3 - u2) * dt * tb[c];
        } else {
            float sign = 1.0f;
            if (rot && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f) sign = -1.0f; // (the short way)
            for (int c = 0; c < n; c++) o[c] = a[c] + (b[c] * sign - a[c]) * u;
        }
    }
#undef GL_KEY
    if (rot) {
        float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
        for (int c = 0; c < 4; c++) o[c] /= l > 1e-12f ? l : 1.0f;
    }
}

typedef struct GlPose {
    float t[3], r[4], s[3];
    bool has_matrix;
    M4 m;
} GlPose;

static void gl_pose_rest(const JsonValue* n, GlPose* p) {
    p->t[0] = p->t[1] = p->t[2] = 0, p->r[0] = p->r[1] = p->r[2] = 0, p->r[3] = 1, p->s[0] = p->s[1] = p->s[2] = 1;
    float mat[16];
    p->has_matrix = json_get_floats(n, "matrix", mat, 16) == 16;
    if (p->has_matrix) memcpy(p->m.m, mat, sizeof mat);
    json_get_floats(n, "translation", p->t, 3);
    json_get_floats(n, "rotation", p->r, 4);
    json_get_floats(n, "scale", p->s, 3);
}
static M4 gl_pose_matrix(const GlPose* p) {
    if (p->has_matrix) return p->m;
    float wxyz[4] = { p->r[3], p->r[0], p->r[1], p->r[2] };
    M4 r = m4_quat(wxyz);
    return m4_trs(p->t, &r, p->s);
}
// A node's world from the poses (memo: world_done per node)
static M4 gl_pose_world(const int* parent, const GlPose* pose, M4* world, uint8_t* done, int i, int depth) {
    if (done[i]) return world[i];
    M4 l = gl_pose_matrix(&pose[i]);
    if (parent[i] >= 0 && depth < 128) {
        M4 pw = gl_pose_world(parent, pose, world, done, parent[i], depth + 1);
        l = m4_mul(&pw, &l);
    }
    world[i] = l, done[i] = 1;
    return l;
}

static void gl_skin(Gl* g, GlBuild* b, int skin, const int* parent, const uint32_t* mhash, int nmh, bool multi, const char* folder_disk,
                    const char* folder_vpath, World* w, TlendReport* r) {
    const JsonValue* sj = json_at(json_get(g->root, "skins"), (size_t)skin);
    const JsonValue* jl = json_get(sj, "joints");
    const JsonValue* nodes = json_get(g->root, "nodes");
    int nb = (int)json_len(jl), nn = (int)json_len(nodes);
    if (nb < 1 || nb > ANIM_MAX_BONES) return;
    int* joint_of = malloc(sizeof(int) * (size_t)(nn + 1)); // per node: its joint (-1)
    M4* rest = malloc(sizeof(M4) * (size_t)nb);
    SkScene* sk = calloc(1, sizeof *sk);
    SkTri* tris = NULL;
    size_t ntris = 0, cap = 0;
    float* ibm = NULL;
    if (!joint_of || !rest || !sk) goto done;
    for (int i = 0; i < nn; i++) joint_of[i] = -1;
    int joints[ANIM_MAX_BONES];
    for (int j = 0; j < nb; j++) {
        joints[j] = json_int(json_at(jl, (size_t)j), -1);
        if (joints[j] < 0 || joints[j] >= nn) goto done;
        joint_of[joints[j]] = j;
    }
    // the bones: rest = inverse bind (in order: a parent before its children, as the Skeletal Mesh wants)
    uint32_t nibm = json_get(sj, "inverseBindMatrices") ? gl_accessor(g, json_get_int(sj, "inverseBindMatrices", -1), 16, &ibm) : 0;
    int order[ANIM_MAX_BONES], bone_of[ANIM_MAX_BONES], jparent[ANIM_MAX_BONES], no = 0;
    for (int j = 0; j < nb; j++) {
        int p = parent[joints[j]];
        while (p >= 0 && joint_of[p] < 0) p = parent[p];
        jparent[j] = p >= 0 ? joint_of[p] : -1;
        bone_of[j] = -1;
    }
    for (int pass = 0; pass < nb && no < nb; pass++) // (roots first, then each joint after its parent)
        for (int j = 0; j < nb; j++)
            if (bone_of[j] < 0 && (jparent[j] < 0 || bone_of[jparent[j]] >= 0)) bone_of[j] = no, order[no++] = j;
    if (no < nb) goto done; // (a cycle)
    sk->nb = nb;
    for (int k = 0; k < nb; k++) {
        int j = order[k];
        M4 ib = m4_id(), gw;
        if ((uint32_t)j < nibm) memcpy(ib.m, &ibm[(size_t)j * 16], sizeof ib.m);
        gw = m4_inv(&ib);
        rest[k] = sk->rest[k] = gl_to_engine_m(&gw);
        AnimBone* ab = &sk->bones[k];
        const char* nm = json_get_str(json_at(nodes, (size_t)joints[j]), "name", "");
        if (nm[0]) snprintf(ab->name, sizeof ab->name, "%s", nm);
        else snprintf(ab->name, sizeof ab->name, "Joint_%d", j);
        for (int q = 0; q < k; q++) // (unique: the animations find their bones by name)
            if (!strcmp(sk->bones[q].name, ab->name)) {
                snprintf(ab->name, sizeof ab->name, "%.40s_%d", nm[0] ? nm : "Joint", j);
                break;
            }
        ab->parent = (int16_t)(jparent[j] >= 0 ? bone_of[jparent[j]] : -1);
    }
    // the faces of every mesh node using this skin (their vertices are in the bind space: the node's transform is
    // not used, as glTF says)
    for (int u = 0; u < b->nskinned; u++) {
        if (b->skinned[u].skin != skin) continue;
        // a mesh under a joint: its vertices from its node's space into the bind space, rigidly on that joint
        // (its rest relative to the joint's, on the joint's bind)
        int bn = b->skinned[u].bone_node, fixed = bn >= 0 && joint_of[bn] >= 0 ? bone_of[joint_of[bn]] : -1;
        M4 am = m4_id();
        if (fixed >= 0) {
            GlPose* rp = malloc(sizeof(GlPose) * (size_t)nn);
            M4* rw = malloc(sizeof(M4) * (size_t)nn);
            uint8_t* rd = calloc((size_t)nn, 1);
            if (rp && rw && rd) {
                for (int i = 0; i < nn; i++) gl_pose_rest(json_at(nodes, (size_t)i), &rp[i]);
                M4 wn = gl_pose_world(parent, rp, rw, rd, b->skinned[u].node, 0), wj = gl_pose_world(parent, rp, rw, rd, bn, 0);
                M4 ib = m4_id();
                if ((uint32_t)joint_of[bn] < nibm) memcpy(ib.m, &ibm[(size_t)joint_of[bn] * 16], sizeof ib.m);
                M4 jb = m4_inv(&ib), wji = m4_inv(&wj), t = m4_mul(&jb, &wji);
                am = m4_mul(&t, &wn);
            }
            free(rp), free(rw), free(rd);
        }
        const JsonValue* prims = json_get(json_at(json_get(g->root, "meshes"), (size_t)b->skinned[u].mesh), "primitives");
        for (size_t pi = 0; pi < json_len(prims); pi++) {
            const JsonValue* pr = json_at(prims, pi);
            int mode = json_get_int(pr, "mode", 4);
            if (mode != 4 && mode != 5 && mode != 6) continue;
            const JsonValue* at = json_get(pr, "attributes");
            float *pos = NULL, *nor = NULL, *uv = NULL, *jo = NULL, *we = NULL;
            uint32_t* idx = NULL;
            uint32_t nv = gl_accessor(g, json_get_int(at, "POSITION", -1), 3, &pos);
            uint32_t nnor = json_get(at, "NORMAL") ? gl_accessor(g, json_get_int(at, "NORMAL", -1), 3, &nor) : 0;
            // its morph targets: the Skeletal Mesh's by name, the deltas in its space (an attached mesh's moved with it)
            uint32_t mbase = sk->morph.nsrc;
            sk->morph.nsrc += nv;
            {
                const JsonValue* gm = json_at(json_get(g->root, "meshes"), (size_t)b->skinned[u].mesh);
                const JsonValue* tg = json_get(pr, "targets");
                for (uint32_t t = 0; t < json_len(tg) && t < RMESH_MAX_MORPHS; t++) {
                    char tn[RMESH_MORPH_NAME];
                    float tw;
                    gl_morph_name(gm, (int)t, tn, sizeof tn, &tw);
                    int ti = morph_target(&sk->morph, tn, tw);
                    const JsonValue* tt = json_at(tg, t);
                    float *dpv = NULL, *dnv = NULL;
                    uint32_t np = json_get(tt, "POSITION") ? gl_accessor(g, json_get_int(tt, "POSITION", -1), 3, &dpv) : 0;
                    uint32_t nn2 = json_get(tt, "NORMAL") ? gl_accessor(g, json_get_int(tt, "NORMAL", -1), 3, &dnv) : 0;
                    for (uint32_t v = 0; ti >= 0 && v < nv; v++) {
                        float a[3] = { 0, 0, 0 }, c[3] = { 0, 0, 0 }, t3[3], dp[3], dn[3], nb3[3];
                        if (v < np) memcpy(a, &dpv[v * 3], 12);
                        if (v < nn2) memcpy(c, &dnv[v * 3], 12);
                        if (fixed >= 0) m4_dir(&am, a, t3), memcpy(a, t3, 12), m4_dir(&am, c, t3), memcpy(c, t3, 12);
                        gl_to_engine_p(a, dp);
                        gl_to_blender(c, nb3);
                        dn[0] = nb3[0], dn[1] = -nb3[1], dn[2] = nb3[2];
                        morph_add(&sk->morph, mbase + v, ti, dp, dn);
                    }
                    free(dpv), free(dnv);
                }
            }
            uint32_t nuv = json_get(at, "TEXCOORD_0") ? gl_accessor(g, json_get_int(at, "TEXCOORD_0", -1), 2, &uv) : 0;
            uint32_t njo = json_get(at, "JOINTS_0") ? gl_accessor(g, json_get_int(at, "JOINTS_0", -1), 4, &jo) : 0;
            uint32_t nwe = json_get(at, "WEIGHTS_0") ? gl_accessor(g, json_get_int(at, "WEIGHTS_0", -1), 4, &we) : 0;
            uint32_t ni = json_get(pr, "indices") ? gl_indices(g, json_get_int(pr, "indices", -1), &idx) : 0;
            if (!json_get(pr, "indices") && nv && (idx = malloc(sizeof(uint32_t) * nv)))
                for (uint32_t k = 0; k < nv; k++) idx[k] = k, ni = nv;
            uint32_t ntri = mode == 4 ? ni / 3 : ni >= 3 ? ni - 2 : 0;
            int mat = json_get_int(pr, "material", -1);
            if (mat < 0 || mat >= nmh) mat = nmh - 1;
            for (uint32_t v = 0; fixed >= 0 && v < nv; v++) { // (an attached mesh: into the bind space)
                float t3[3];
                m4_point(&am, &pos[v * 3], t3), memcpy(&pos[v * 3], t3, 12);
                if (v < nnor) m4_dir(&am, &nor[v * 3], t3), memcpy(&nor[v * 3], t3, 12);
            }
            // per vertex: the joint it leans on most
            int* vb = nv ? malloc(sizeof(int) * nv) : NULL;
            for (uint32_t v = 0; vb && v < nv; v++) {
                float best = -1.0f;
                vb[v] = fixed >= 0 ? fixed : 0;
                for (int c = 0; fixed < 0 && c < 4 && v < njo && v < nwe; c++)
                    if (we[v * 4 + c] > best && (int)jo[v * 4 + c] < nb) best = we[v * 4 + c], vb[v] = bone_of[(int)jo[v * 4 + c]];
            }
            if (vb && ntri && obj_grow((void**)&tris, &cap, ntris + ntri, sizeof(SkTri)))
                for (uint32_t t = 0; t < ntri; t++) {
                    uint32_t c3[3];
                    if (mode == 4) c3[0] = idx[t * 3], c3[1] = idx[t * 3 + 1], c3[2] = idx[t * 3 + 2];
                    else if (mode == 5) c3[0] = idx[t], c3[1] = idx[t + 1 + (t & 1)], c3[2] = idx[t + 2 - (t & 1)];
                    else c3[0] = idx[0], c3[1] = idx[t + 1], c3[2] = idx[t + 2];
                    if (c3[0] >= nv || c3[1] >= nv || c3[2] >= nv) continue;
                    SkTri* st = &tris[ntris++];
                    // its bone: the one most of its corners lean on (else the first corner's)
                    st->bone = vb[c3[1]] == vb[c3[2]] ? vb[c3[1]] : vb[c3[0]];
                    st->mat = mat;
                    st->skinned = fixed < 0 && njo >= nv && nwe >= nv;
                    float fn[3]; // (the face's normal when the file has none)
                    {
                        const float *a = &pos[c3[0] * 3], *bb = &pos[c3[1] * 3], *cc = &pos[c3[2] * 3];
                        float e1[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] }, e2[3] = { cc[0] - a[0], cc[1] - a[1], cc[2] - a[2] };
                        v3cross(e1, e2, fn);
                    }
                    for (int q = 0; q < 3; q++) {
                        int qs = q == 0 ? 0 : 3 - q; // (mirrored: the winding reversed)
                        uint32_t vi = c3[qs];
                        gl_to_engine_p(&pos[vi * 3], st->p[q]);
                        const float* n0 = vi < nnor ? &nor[vi * 3] : fn;
                        float nbl[3];
                        gl_to_blender(n0, nbl);
                        st->n[q][0] = nbl[0], st->n[q][1] = -nbl[1], st->n[q][2] = nbl[2];
                        st->uv[q][0] = vi < nuv ? uv[vi * 2] : 0.0f;
                        st->uv[q][1] = vi < nuv ? 1.0f - uv[vi * 2 + 1] : 0.0f;
                        st->src[q] = mbase + vi;
                        if (st->skinned) { // its 4 joints' bones and weights
                            SkinW sw;
                            memset(&sw, 0, sizeof sw);
                            for (int c = 0; c < 4; c++)
                                if ((int)jo[vi * 4 + c] < nb) skin_add(&sw, bone_of[(int)jo[vi * 4 + c]], we[vi * 4 + c]);
                            skin_pack(&sw, vb[vi], st->skin[q]);
                        }
                    }
                }
            free(vb), free(pos), free(nor), free(uv), free(jo), free(we), free(idx);
        }
    }
    if (!ntris) goto done;
    // the names: the skin's (else its first mesh node's)
    char name[200];
    const char* sn = json_get_str(sj, "name", "");
    for (int u = 0; !sn[0] && u < b->nskinned; u++)
        if (b->skinned[u].skin == skin) sn = json_get_str(json_at(nodes, (size_t)b->skinned[u].node), "name", "");
    snprintf(sk->base, sizeof sk->base, "%s", sn[0] ? sn : "Skin");
    sk->tris = tris, sk->ntris = ntris, tris = NULL;
    if (!sk_write_mesh(sk, mhash, nmh, folder_disk, folder_vpath, name, sizeof name, r)) goto done;
    // the animations that move a joint or a joint's ancestor
    uint32_t first_anim = 0;
    uint8_t* affects = calloc((size_t)nn + 1, 1);
    for (int j = 0; affects && j < nb; j++)
        for (int p = joints[j]; p >= 0 && !affects[p]; p = parent[p]) affects[p] = 1;
    const JsonValue* anims = json_get(g->root, "animations");
    GlPose* pose = malloc(sizeof(GlPose) * (size_t)(nn + 1));
    M4* world = malloc(sizeof(M4) * (size_t)(nn + 1));
    uint8_t* wdone = malloc((size_t)nn + 1);
    for (size_t ai = 0; affects && pose && world && wdone && ai < json_len(anims); ai++) {
        const JsonValue* an = json_at(anims, ai);
        const JsonValue* chs = json_get(an, "channels");
        const JsonValue* sms = json_get(an, "samplers");
        int nch = (int)json_len(chs), nsm = (int)json_len(sms);
        bool use = false;
        for (int c = 0; c < nch && !use; c++) {
            int tn = json_get_int(json_get(json_at(chs, (size_t)c), "target"), "node", -1);
            use = tn >= 0 && tn < nn && affects[tn];
        }
        // its "weights" channels on this Skeletal Mesh's meshes: the morph target curves (notas-dev/animacao.md)
        int wch[16], nwch = 0;
        for (int c = 0; c < nch && nwch < 16; c++) {
            const JsonValue* tg = json_get(json_at(chs, (size_t)c), "target");
            int tn = json_get_int(tg, "node", -1);
            if (strcmp(json_get_str(tg, "path", ""), "weights")) continue;
            bool mine = false;
            for (int u = 0; u < b->nskinned && !mine; u++) mine = b->skinned[u].skin == skin && b->skinned[u].node == tn;
            if (mine) wch[nwch++] = c, use = true;
        }
        if (!use || !nsm) continue;
        GlSampler* sm = calloc((size_t)nsm, sizeof(GlSampler));
        if (!sm) continue;
        float dur = 0.0f;
        for (int s = 0; s < nsm; s++) {
            const JsonValue* sv = json_at(sms, (size_t)s);
            const char* ip = json_get_str(sv, "interpolation", "LINEAR");
            sm[s].interp = !strcmp(ip, "STEP") ? 1 : !strcmp(ip, "CUBICSPLINE") ? 2 : 0;
            sm[s].n = gl_accessor(g, json_get_int(sv, "input", -1), 1, &sm[s].in);
            float* tmp = NULL;
            gl_accessor(g, json_get_int(sv, "output", -1), 4, &tmp); // (VEC3 / VEC4 into 4: repacked below per path)
            sm[s].out = tmp;
            if (sm[s].n) dur = fmaxf(dur, sm[s].in[sm[s].n - 1]);
        }
        uint32_t nframes = (uint32_t)(dur * GL_ANIM_FPS + 0.5f) + 1;
        if (nframes < 2) nframes = 2;
        if (nframes > 20000) nframes = 20000;
        M4* wall = malloc(sizeof(M4) * (size_t)nframes * (size_t)nb);
        SkCurves cv = { 0 };
        if (nwch) cv.v = calloc((size_t)nframes * ANIM_MAX_MORPHS, sizeof(float));
        for (int q = 0; cv.v && q < nwch; q++) { // each weights channel sampled at the frames into its targets' curves
            const JsonValue* ch = json_at(chs, (size_t)wch[q]);
            int tn = json_get_int(json_get(ch, "target"), "node", -1), s = json_get_int(ch, "sampler", -1);
            const JsonValue* gm = json_at(json_get(g->root, "meshes"), (size_t)json_get_int(json_at(nodes, (size_t)tn), "mesh", -1));
            int nt = gl_morph_count(gm);
            if (s < 0 || s >= nsm || !sm[s].n || !sm[s].out || nt <= 0) continue;
            size_t nval = (size_t)sm[s].n * (sm[s].interp == 2 ? 3u : 1u) * (size_t)nt;
            float* wv = malloc(sizeof(float) * nval); // (the outputs were read 4 wide: a scalar each, repacked)
            if (!wv) continue;
            for (size_t i = 0; i < nval; i++) wv[i] = sm[s].out[i * 4];
            GlSampler ws = sm[s];
            ws.out = wv;
            int ci[RMESH_MAX_MORPHS];
            for (int t = 0; t < nt; t++) {
                char tname[RMESH_MORPH_NAME];
                float tw;
                gl_morph_name(gm, t, tname, sizeof tname, &tw);
                ci[t] = -1;
                for (uint32_t k = 0; k < cv.n && ci[t] < 0; k++)
                    if (!strcmp(cv.name[k], tname)) ci[t] = (int)k;
                if (ci[t] < 0 && cv.n < ANIM_MAX_MORPHS) snprintf(cv.name[cv.n], ANIM_MORPH_NAME, "%s", tname), ci[t] = (int)cv.n++;
            }
            float o[RMESH_MAX_MORPHS];
            for (uint32_t fr = 0; fr < nframes; fr++) {
                gl_sample(&ws, (float)fr / GL_ANIM_FPS, nt, false, o);
                for (int t = 0; t < nt; t++)
                    if (ci[t] >= 0) cv.v[(size_t)fr * ANIM_MAX_MORPHS + (size_t)ci[t]] = o[t];
            }
            free(wv);
        }
        if (wall) {
            char an_name[200], label[120];
            const char* anm = json_get_str(an, "name", "");
            if (anm[0]) snprintf(label, sizeof label, "%s", anm);
            else snprintf(label, sizeof label, "Anim_%d", (int)ai);
            write_name(an_name, sizeof an_name, "A_", multi ? sk->base : label, multi ? label : NULL);
            for (uint32_t fr = 0; fr < nframes; fr++) {
                float t = (float)fr / GL_ANIM_FPS;
                for (int i = 0; i < nn; i++) gl_pose_rest(json_at(nodes, (size_t)i), &pose[i]);
                for (int c = 0; c < nch; c++) {
                    const JsonValue* ch = json_at(chs, (size_t)c);
                    const JsonValue* tg = json_get(ch, "target");
                    int tn = json_get_int(tg, "node", -1), s = json_get_int(ch, "sampler", -1);
                    const char* path_ = json_get_str(tg, "path", "");
                    if (tn < 0 || tn >= nn || s < 0 || s >= nsm || !sm[s].n || !sm[s].out) continue;
                    float v4[4];
                    if (!strcmp(path_, "rotation")) gl_sample(&sm[s], t, 4, true, pose[tn].r), pose[tn].has_matrix = false;
                    else if (!strcmp(path_, "translation") || !strcmp(path_, "scale")) {
                        // (the outputs were read 4 wide: a VEC3 key's 4th is 0, so sampling 4 and keeping 3 is exact)
                        gl_sample(&sm[s], t, 4, false, v4);
                        memcpy(!strcmp(path_, "scale") ? pose[tn].s : pose[tn].t, v4, 12);
                        pose[tn].has_matrix = false;
                    }
                }
                memset(wdone, 0, (size_t)nn);
                for (int bi = 0; bi < nb; bi++) {
                    M4 gw = gl_pose_world(parent, pose, world, wdone, joints[order[bi]], 0);
                    wall[(size_t)fr * (size_t)nb + (size_t)bi] = gl_to_engine_m(&gw);
                }
            }
            uint32_t h = sk_write_anim(sk, name, an_name, GL_ANIM_FPS, nframes, wall, folder_disk, folder_vpath, r, &cv);
            if (!first_anim) first_anim = h;
        }
        free(wall), free(cv.v);
        for (int s = 0; s < nsm; s++) free(sm[s].in), free(sm[s].out);
        free(sm);
    }
    free(affects), free(pose), free(world), free(wdone);
    sk_actor(w, sk->base, name, first_anim, folder_vpath, r);
done:
    if (sk) free(sk->tris), morph_build_free(&sk->morph);
    free(joint_of), free(rest), free(sk), free(tris), free(ibm);
}

// The animations of the meshes with morph targets and no skin (each a one-bone Skeletal Mesh, SK_<node>: tlend_import.c):
// their "weights" channels become the morph target curves of a sequence of its own (A_<node>_<animation>), and the
// actor plays it (Use Animation Asset)
static void gl_morph_anims(Gl* g, const GlBuild* b, const char* folder_disk, const char* folder_vpath, World* w, TlendReport* r) {
    const JsonValue* anims = json_get(g->root, "animations");
    const JsonValue* nodes = json_get(g->root, "nodes");
    int nn = (int)json_len(nodes);
    for (int tn = 0; tn < nn; tn++) {
        const JsonValue* node = json_at(nodes, (size_t)tn);
        const JsonValue* gm = json_at(json_get(g->root, "meshes"), (size_t)json_get_int(node, "mesh", -1));
        int nt = gm ? gl_morph_count(gm) : 0;
        bool skinned = false;
        for (int u = 0; u < b->nskinned && !skinned; u++) skinned = b->skinned[u].node == tn;
        if (nt <= 0 || skinned) continue;
        char sk_name[200], vp[1100];
        const char* nname = json_get_str(node, "name", "");
        write_name(sk_name, sizeof sk_name, "SK_", nname[0] ? nname : json_get_str(gm, "name", "Mesh"), NULL);
        snprintf(vp, sizeof vp, "%s/%s.rskm", folder_vpath, sk_name);
        uint32_t mesh = vhash(vp), first = 0;
        for (size_t ai = 0; ai < json_len(anims); ai++) {
            const JsonValue* an = json_at(anims, ai);
            const JsonValue* chs = json_get(an, "channels");
            const JsonValue* sms = json_get(an, "samplers");
            int ch = -1;
            for (int c = 0; c < (int)json_len(chs) && ch < 0; c++) {
                const JsonValue* tg = json_get(json_at(chs, (size_t)c), "target");
                if (json_get_int(tg, "node", -1) == tn && !strcmp(json_get_str(tg, "path", ""), "weights")) ch = c;
            }
            int si = ch >= 0 ? json_get_int(json_at(chs, (size_t)ch), "sampler", -1) : -1;
            if (si < 0 || si >= (int)json_len(sms)) continue;
            const JsonValue* sv = json_at(sms, (size_t)si);
            GlSampler s = { 0 };
            const char* ip = json_get_str(sv, "interpolation", "LINEAR");
            s.interp = !strcmp(ip, "STEP") ? 1 : !strcmp(ip, "CUBICSPLINE") ? 2 : 0;
            s.n = gl_accessor(g, json_get_int(sv, "input", -1), 1, &s.in);
            float* raw = NULL;
            uint32_t nraw = gl_accessor(g, json_get_int(sv, "output", -1), 1, &raw);
            size_t need = (size_t)s.n * (s.interp == 2 ? 3u : 1u) * (size_t)nt;
            AnimSeq* sq = calloc(1, sizeof *sq);
            float* rot = calloc(2 * 4, sizeof(float));
            float* pos = calloc(2 * 3, sizeof(float));
            float dur = s.n ? s.in[s.n - 1] : 0.0f;
            uint32_t nframes = (uint32_t)(dur * GL_ANIM_FPS + 0.5f) + 1;
            if (nframes < 2) nframes = 2;
            if (nframes > 20000) nframes = 20000;
            float* kt = malloc(sizeof(float) * 2 * nframes);
            float o[RMESH_MAX_MORPHS];
            if (sq && rot && pos && kt && s.n && nraw >= need) {
                s.out = raw;
                char an_name[200], label[120], adir[950];
                const char* anm = json_get_str(an, "name", "");
                if (anm[0]) snprintf(label, sizeof label, "%s", anm);
                else snprintf(label, sizeof label, "Anim_%d", (int)ai);
                write_name(an_name, sizeof an_name, "A_", nname[0] ? nname : "Mesh", label);
                snprintf(adir, sizeof adir, "%s/Animations", folder_disk);
                re_make_dirs(adir);
                snprintf(sq->path, sizeof sq->path, "%s/%s.ranim", adir, an_name);
                snprintf(sq->name, sizeof sq->name, "%s", an_name);
                snprintf(sq->skeleton, sizeof sq->skeleton, "%s", sk_name);
                // the one bone still (its rest), two frames; the curves carry the animation
                rot[3] = rot[7] = 1.0f;
                sq->fps = GL_ANIM_FPS, sq->nframes = 2, sq->ntracks = 1, sq->loop = true, sq->rot = rot, sq->pos = pos;
                sq->length = dur > 0.0f ? dur : 1.0f / GL_ANIM_FPS;
                sq->fps = 1.0f / sq->length; // (2 frames over its length)
                snprintf(sq->track_bone[0], ANIM_NAME, "Root");
                for (int t = 0; t < nt && t < RMESH_MAX_MORPHS; t++) { // each target's curve: a key a frame, the still runs trimmed
                    char tname[RMESH_MORPH_NAME];
                    float tw;
                    gl_morph_name(gm, t, tname, sizeof tname, &tw);
                    uint32_t nk = 0;
                    float prev = 0.0f;
                    for (uint32_t fr = 0; fr < nframes; fr++) {
                        float tt = (float)fr / GL_ANIM_FPS;
                        gl_sample(&s, tt, nt, false, o);
                        float next = o[t];
                        if (fr + 1 < nframes) {
                            float o2[RMESH_MAX_MORPHS];
                            gl_sample(&s, (float)(fr + 1) / GL_ANIM_FPS, nt, false, o2);
                            next = o2[t];
                        }
                        if (fr && fr + 1 < nframes && o[t] == prev && o[t] == next) continue;
                        kt[nk] = tt, kt[nframes + nk] = o[t], nk++;
                        prev = o[t];
                    }
                    anim_curve_set(sq, tname, kt, kt + nframes, nk);
                }
                if (anim_seq_save(sq)) {
                    r->animations++;
                    char avp[1100];
                    snprintf(avp, sizeof avp, "%s/Animations/%s.ranim", folder_vpath, an_name);
                    if (!first) first = vhash(avp);
                }
            }
            if (sq) free(sq->curve_keys);
            free(sq), free(rot), free(pos), free(kt), free(s.in), free(raw);
        }
        for (uint32_t sl = 1; w && first && sl < w->slot_count; sl++) { // the actor plays its first one
            Entity e = ecs_entity_at(w, sl);
            CSkeletalMesh* c = e ? ecs_get(w, e, COMP_SKELETAL_MESH) : NULL;
            if (c && c->mesh == mesh && !c->anim) c->anim = first, c->play_rate = 1.0f, c->loop = 1;
        }
    }
}

bool gltf_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r) {
    imprep_begin(src); // (Q19) the conversion report
    snprintf(g_import_src, sizeof g_import_src, "%s", src), g_import_rigid = false;
    memset(r, 0, sizeof *r);
    g_vcol_meshes = 0;
    FILE* fp = re_fopen(src, "rb");
    if (!fp) {
        snprintf(r->text, sizeof r->text, "Could not open %s.", src);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* file = size > 0 ? malloc((size_t)size) : NULL; // an import, not a frame
    bool ok = file && fread(file, 1, (size_t)size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) {
        free(file);
        snprintf(r->text, sizeof r->text, "Could not read %s.", src);
        return false;
    }
    // GLB: the header, the JSON chunk, the BIN chunk
    const char* json = (const char*)file;
    size_t json_len_ = (size_t)size;
    uint8_t* bin = NULL;
    size_t bin_size = 0;
    if (size >= 20 && !memcmp(file, "glTF", 4)) {
        uint32_t total, clen, ctype;
        memcpy(&total, file + 8, 4);
        size_t at = 12;
        json_len_ = 0;
        while (at + 8 <= (size_t)size && at + 8 <= total) {
            memcpy(&clen, file + at, 4), memcpy(&ctype, file + at + 4, 4);
            if (at + 8 + clen > (size_t)size) break;
            if (ctype == 0x4E4F534A && !json_len_) json = (const char*)file + at + 8, json_len_ = clen;
            else if (ctype == 0x004E4942 && !bin) bin = file + at + 8, bin_size = clen;
            at += 8 + ((clen + 3) & ~3u);
        }
    }
    JsonDoc doc;
    char err[200];
    if (!json_len_ || !json_parse(json, json_len_, &doc, err, sizeof err)) {
        snprintf(r->text, sizeof r->text, "%s is not a glTF file (%s).", src, json_len_ ? err : "no JSON chunk");
        free(file);
        return false;
    }
    Gl g;
    memset(&g, 0, sizeof g);
    g.root = &doc.root;
    snprintf(g.dir, sizeof g.dir, "%s", src);
    char* sl = strrchr(g.dir, '/');
    char* bs = strrchr(g.dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = 0;
    else snprintf(g.dir, sizeof g.dir, ".");
    // the buffers
    const JsonValue* bufs = json_get(g.root, "buffers");
    g.nbufs = (int)json_len(bufs);
    g.bufs = calloc((size_t)(g.nbufs ? g.nbufs : 1), sizeof(GlBuf));
    for (int i = 0; g.bufs && i < g.nbufs; i++) {
        const char* uri = json_get_str(json_at(bufs, (size_t)i), "uri", NULL);
        GlBuf* b = &g.bufs[i];
        if (!uri) {
            b->data = bin, b->size = bin_size;
        } else if (!strncmp(uri, "data:", 5)) {
            const char* c = strstr(uri, ";base64,");
            if (c) b->data = b64_decode(c + 8, &b->size), b->owned = true;
        } else {
            char path[1100];
            gl_uri_path(&g, uri, path, sizeof path);
            b->data = (uint8_t*)obj_read_file(path, &b->size), b->owned = true;
        }
    }
    // the images: a file next to the glTF, or written from the file (a bufferView / data URI) into <folder>/Textures
    char folder_disk[900], stem[128];
    snprintf(folder_disk, sizeof folder_disk, "%s%s/Textures", content_root, folder_vpath + 8);
    const char* base = src + strlen(src);
    while (base > src && base[-1] != '/' && base[-1] != '\\') base--;
    snprintf(stem, sizeof stem, "%s", base);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    const JsonValue* imgs = json_get(g.root, "images");
    int nimg = (int)json_len(imgs);
    char(*img_path)[512] = calloc((size_t)(nimg ? nimg : 1), 512);
    for (int i = 0; img_path && i < nimg; i++) {
        const JsonValue* im = json_at(imgs, (size_t)i);
        const char* uri = json_get_str(im, "uri", NULL);
        const char* mime = json_get_str(im, "mimeType", "");
        uint8_t* data = NULL;
        size_t n = 0;
        bool own = false;
        if (uri && strncmp(uri, "data:", 5)) {
            gl_uri_path(&g, uri, img_path[i], 512);
            continue;
        }
        if (uri) {
            const char* c = strstr(uri, ";base64,");
            if (c) data = b64_decode(c + 8, &n), own = true;
            if (strstr(uri, "image/jpeg")) mime = "image/jpeg";
        } else
            data = (uint8_t*)gl_view(&g, json_get_int(im, "bufferView", -1), &n, NULL);
        if (data && n) {
            char name[96];
            snprintf(name, sizeof name, "%s", json_get_str(im, "name", ""));
            if (!name[0]) snprintf(name, sizeof name, "%s_%d", stem, i);
            clean(name);
            re_make_dirs(folder_disk);
            snprintf(img_path[i], 512, "%s/%s.%s", folder_disk, name, strstr(mime, "jpeg") ? "jpg" : "png");
            FILE* o = re_fopen(img_path[i], "wb");
            if (!o || fwrite(data, 1, n, o) != n) img_path[i][0] = 0;
            if (o) fclose(o);
        }
        if (own) free(data);
    }
    // the materials (and a default one, last, for primitives without)
    TFile f;
    memset(&f, 0, sizeof f);
    f.fps = 24.0f;
    const JsonValue* mats = json_get(g.root, "materials");
    f.nmats = (int)json_len(mats) + 1;
    f.mats = calloc((size_t)f.nmats, sizeof(TMat));
    for (int i = 0; f.mats && i < f.nmats; i++) gl_material(&f.mats[i], &g, json_at(mats, (size_t)i), i, img_path, nimg);
    if (f.mats) snprintf(f.mats[f.nmats - 1].name, sizeof f.mats[0].name, "%s_Default", stem);
    // the scene's nodes
    int nmesh = (int)json_len(json_get(g.root, "meshes"));
    static GlBuild b; // (its lights: 25 KB)
    memset(&b, 0, sizeof b);
    b.g = &g, b.f = &f, b.mesh_of = calloc((size_t)nmesh + 1, sizeof(int)), b.mesh_slots = calloc((size_t)nmesh + 1, sizeof(int[33]));
    if (!f.mats || !b.mesh_of || !b.mesh_slots) b.fail = true;
    {
        int nn = (int)json_len(json_get(g.root, "nodes"));
        const JsonValue* skins = json_get(g.root, "skins");
        b.node_skin = malloc(sizeof(int) * (size_t)(nn + 1));
        for (int i = 0; b.node_skin && i < nn; i++) b.node_skin[i] = -1;
        for (size_t k = 0; b.node_skin && k < json_len(skins); k++) {
            const JsonValue* jl = json_get(json_at(skins, k), "joints");
            for (size_t q = 0; q < json_len(jl); q++) {
                int jn = json_int(json_at(jl, q), -1);
                if (jn >= 0 && jn < nn && b.node_skin[jn] < 0) b.node_skin[jn] = (int)k;
            }
        }
    }
    for (int i = 0; i < nmesh && !b.fail; i++) b.mesh_of[i] = -2;
    M4 id = m4_id();
    const JsonValue* scenes = json_get(g.root, "scenes");
    const JsonValue* scene = json_at(scenes, (size_t)json_get_int(g.root, "scene", 0));
    if (scene) {
        const JsonValue* roots = json_get(scene, "nodes");
        for (size_t k = 0; k < json_len(roots); k++) gl_visit(&b, json_int(json_at(roots, k), -1), &id, 0, -1);
    } else { // (no scene: every node no other has as a child)
        const JsonValue* nodes = json_get(g.root, "nodes");
        size_t nn = json_len(nodes);
        bool* child = calloc(nn + 1, 1);
        for (size_t i = 0; child && i < nn; i++) {
            const JsonValue* ch = json_get(json_at(nodes, i), "children");
            for (size_t k = 0; k < json_len(ch); k++) {
                int c = json_int(json_at(ch, k), -1);
                if (c >= 0 && (size_t)c < nn) child[c] = true;
            }
        }
        for (size_t i = 0; child && i < nn; i++)
            if (!child[i]) gl_visit(&b, (int)i, &id, 0, -1);
        free(child);
    }
    if (f.mats && !b.default_used) free(f.mats[--f.nmats].nodes); // (no primitive without a material: no default)
    // the materials' hashes (the skins' parts use them; tfile_assets frees the scene)
    int nmh = f.nmats;
    uint32_t* mhash = calloc((size_t)nmh + 1, sizeof(uint32_t));
    for (int k = 0; mhash && k < nmh; k++) {
        char mn[96], mv[1100];
        snprintf(mn, sizeof mn, "M_%s", f.mats[k].name);
        clean(mn);
        snprintf(mv, sizeof mv, "%s/Materials/%s.rmat", folder_vpath, mn);
        mhash[k] = vhash(mv);
    }
    bool done = false;
    if (b.fail || !mhash || (!f.nobjs && !b.nlights && !b.nskinned && !b.ncams)) {
        snprintf(r->text, sizeof r->text, b.fail || !mhash ? "Out of memory reading %s." : "%s has no meshes.", src);
        obj_free_meshes(&f);
    } else {
        done = tfile_assets(f, src, folder_vpath, content_root, w, r);
        // the skins: a Skeletal Mesh each (the nodes' parents first)
        const JsonValue* nodes = json_get(g.root, "nodes");
        int nn = (int)json_len(nodes);
        int* parent = malloc(sizeof(int) * (size_t)(nn + 1));
        for (int i = 0; parent && i < nn; i++) parent[i] = -1;
        for (int i = 0; parent && i < nn; i++) {
            const JsonValue* ch = json_get(json_at(nodes, (size_t)i), "children");
            for (size_t k = 0; k < json_len(ch); k++) {
                int c = json_int(json_at(ch, k), -1);
                if (c >= 0 && c < nn && c != i) parent[c] = i;
            }
        }
        char asset_dir[900];
        snprintf(asset_dir, sizeof asset_dir, "%s%s", content_root, folder_vpath + 8);
        int nsk = 0, seen[64];
        for (int u = 0; u < b.nskinned; u++) {
            bool dup = false;
            for (int q = 0; q < nsk; q++) dup |= seen[q] == b.skinned[u].skin;
            if (!dup) seen[nsk++] = b.skinned[u].skin;
        }
        unsigned sk0 = r->skeletal;
        for (int q = 0; parent && q < nsk; q++) gl_skin(&g, &b, seen[q], parent, mhash, nmh, nsk > 1, asset_dir, folder_vpath, w, r);
        unsigned an0 = r->animations;
        gl_morph_anims(&g, &b, asset_dir, folder_vpath, w, r); // (the morph targets' animations of the meshes without a skin)
        if (r->animations > an0) done = true;
        free(parent);
        if (r->skeletal > sk0) done = true;
        if (done) // (the report again, with them; the morph targets after the lights)
            snprintf(r->text, sizeof r->text, "%u meshes, %u materials (%u textures, %u nodes skipped), %u skeletal meshes, %u animations, %u actors",
                     r->meshes, r->materials, r->textures, r->skipped_nodes, r->skeletal, r->animations, r->actors);
    }
    free(mhash);
    // (the file's memory)
    for (int i = 0; i < g.nbufs; i++)
        if (g.bufs[i].owned) free(g.bufs[i].data);
    free(g.bufs), free(img_path), free(b.mesh_of), free(b.mesh_slots), free(b.node_skin);
    json_free(&doc);
    free(file);
    if (!done && !b.nlights && !b.ncams) return false;
    for (int i = 0; w && i < b.ncams; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CAMERA));
        if (e == ENTITY_NONE) break;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", b.cams[i].name);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        memcpy(t->location, b.cams[i].loc, 12), memcpy(t->rotation, b.cams[i].rot, 12);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        ((CCamera*)ecs_get(w, e, COMP_CAMERA))->fov_deg = fminf(fmaxf(b.cams[i].fov, 10.0f), 170.0f);
        r->cameras++;
    }
    for (int i = 0; w && i < b.nlights; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
        if (e == ENTITY_NONE) break;
        CLight* l = &b.lights[i].l;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", l->type == LIGHT_DIRECTIONAL ? "DirectionalLight" : l->type == LIGHT_SPOT ? "SpotLight" : "PointLight");
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        memcpy(t->location, b.lights[i].loc, 12), memcpy(t->rotation, b.lights[i].rot, 12);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
        t->flags = MOBILITY_STATIONARY;
        *(CLight*)ecs_get(w, e, COMP_LIGHT) = *l;
        r->lights++;
    }
    if (r->lights) {
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u lights", r->lights);
    }
    if (r->morphs) {
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u morph targets", r->morphs);
    }
    if (r->cameras) {
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u cameras", r->cameras);
    }
    if (g_vcol_meshes) { // (COLOR_0 kept: the materials' Color Attribute, M5)
        size_t n = strlen(r->text);
        snprintf(r->text + n, sizeof r->text - n, ", %u with vertex colors", g_vcol_meshes);
    }
    return done || r->lights > 0 || r->cameras > 0;
}
