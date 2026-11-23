// (A9.5a: copied from the Tlender, ../tlender/src/vdb.c, adapted: the engine's zlib and files; the format unchanged)
// OpenVDB files (vdb.h) without the OpenVDB library: the file format of OpenVDB's io::File / Archive (file versions
// 222 and later), read into and written from VolumeGridSet.
//
// Layout (little endian): magic, file version, library version, "has grid offsets", a 36-character uuid, the file's
// MetaMap, the grid count, then per grid a descriptor (unique name, type name "Tree_float_5_4_3"[_HalfFloat], instance
// parent, the grid / block / end positions), the grid's compression flags (zip 1, active mask 2, blosc 4), its MetaMap
// ("class", "vector_type"...), its transform (a map type name and the map's doubles), the tree topology (root:
// background, tiles and 32^3 internal children; each internal node: child mask, value mask, its tile values compressed;
// leaves: their value mask) and then the leaf buffers in the same order (value mask again + the voxel values,
// compressed). Values of a node go through OpenVDB's "mask compression" (only the active values are stored, the
// inactive ones are one or two values plus a selection mask) and then zip (zlib), blosc (LZ4 + byte shuffle) or none;
// half grids store 16-bit floats. Leaves index voxels with z fastest (offset = x * 64 + y * 8 + z); VolumeGrid's leaves
// are x fastest.
#include "vdb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re_zlib.h" // re_inflate / re_deflate (zlib)
#include "platform.h"
#include "platform_files.h"

#define VDB_MAGIC 0x56444220LL
#define VDB_FILE_VERSION 224
#define VDB_MIN_VERSION 222 // node mask compression: every file of OpenVDB 2.x+
#define VDB_GRID_INSTANCING 216

#define COMPRESS_ZIP 1u
#define COMPRESS_ACTIVE_MASK 2u
#define COMPRESS_BLOSC 4u

enum {
    NO_MASK_OR_INACTIVE_VALS = 0,
    NO_MASK_AND_MINUS_BG = 1,
    NO_MASK_AND_ONE_INACTIVE_VAL = 2,
    MASK_AND_NO_INACTIVE_VALS = 3,
    MASK_AND_ONE_INACTIVE_VAL = 4,
    MASK_AND_TWO_INACTIVE_VALS = 5,
    NO_MASK_AND_ALL_VALS = 6,
};

// Scalar storage of a grid's values in the file.
typedef enum { ST_F32, ST_F64, ST_I32, ST_F16, ST_BOOL } StoreType;

static void set_err(char* err, int cap, const char* msg) {
    if (err && cap > 0) snprintf(err, (size_t)cap, "%s", msg);
}

// --- Half floats (IEEE binary16, round to nearest even as OpenEXR's half) ---------------------------------------------

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, bits;
    if (e == 0) {
        if (m == 0) bits = s;
        else { // subnormal
            e = 127 - 15 + 1;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            m &= 0x3ff;
            bits = s | (e << 23) | (m << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7f800000u | (m << 13);
    } else {
        bits = s | ((e + 127 - 15) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static uint16_t float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;
    if (e == 0xff) return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
    int ee = (int)e - 127 + 15;
    if (ee >= 31) return (uint16_t)(s | 0x7c00);
    if (ee <= 0) {
        if (ee < -10) return (uint16_t)s;
        m |= 0x800000;
        int shift = 14 - ee;
        uint32_t hm = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1fff;
    uint32_t h = s | ((uint32_t)ee << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) h++; // may carry into the exponent (correct)
    return (uint16_t)h;
}

static float truncate_half(float v) { return half_to_float(float_to_half(v)); }

// --- LZ4 blocks and blosc frames --------------------------------------------------------------------------------------

static bool lz4_decode(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (i + lit > n || o + lit > cap) return false;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break; // the last sequence has literals only
        if (i + 2 > n) return false;
        size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        size_t ml = (tok & 15) + 4;
        if ((tok & 15) == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = src[i++];
                ml += b;
            } while (b == 255);
        }
        if (off == 0 || off > o || o + ml > cap) return false;
        for (size_t k = 0; k < ml; k++, o++) dst[o] = dst[o - off];
    }
    return o == cap;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// c-blosc 1.x frames (OpenVDB's bloscToStream: LZ4, byte shuffle, blocks of 256 bytes).
static bool blosc_decode(const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstlen) {
    if (srclen < 16) return false;
    uint8_t flags = src[2];
    uint32_t typesize = src[3], nbytes = le32(src + 4), blocksize = le32(src + 8), cbytes = le32(src + 12);
    if (nbytes != dstlen || cbytes > srclen) return false;
    if (nbytes == 0) return true;
    if (flags & 0x2) { // memcpyed
        if (srclen < 16 + (size_t)nbytes) return false;
        memcpy(dst, src + 16, nbytes);
        return true;
    }
    if (flags & 0x4) return false; // bitshuffle: not written by OpenVDB
    int codec = (flags >> 5) & 7;  // 0 blosclz, 1 lz4/lz4hc, 2 snappy, 3 zlib, 4 zstd
    if (codec != 1 && codec != 3) return false;
    if (blocksize == 0 || typesize == 0) return false;
    uint32_t nblocks = nbytes / blocksize + (nbytes % blocksize ? 1 : 0), leftover = nbytes % blocksize;
    if (16 + (size_t)nblocks * 4 > srclen) return false;
    bool dont_split = (flags & 0x10) != 0, shuffle = (flags & 0x1) != 0 && typesize > 1;
    uint8_t* tmp = malloc(blocksize);
    if (!tmp) return false;
    bool ok = true;
    for (uint32_t b = 0; b < nblocks && ok; b++) {
        bool last_left = b == nblocks - 1 && leftover;
        uint32_t bsize = last_left ? leftover : blocksize;
        uint32_t nsplits = (!dont_split && typesize <= 16 && bsize / typesize >= 128 && !last_left) ? typesize : 1;
        uint32_t neblock = bsize / nsplits;
        size_t at = le32(src + 16 + (size_t)b * 4);
        uint8_t* out = shuffle ? tmp : dst + (size_t)b * blocksize;
        for (uint32_t s = 0; s < nsplits; s++) {
            if (at + 4 > srclen) {
                ok = false;
                break;
            }
            uint32_t cs = le32(src + at);
            at += 4;
            if (at + cs > srclen) {
                ok = false;
                break;
            }
            if (cs == neblock) memcpy(out + (size_t)s * neblock, src + at, neblock);
            else if (codec == 1) ok = lz4_decode(src + at, cs, out + (size_t)s * neblock, neblock);
            else ok = re_inflate(src + at, cs, out + (size_t)s * neblock, neblock);
            if (!ok) break;
            at += cs;
        }
        if (ok && shuffle) {
            uint8_t* d = dst + (size_t)b * blocksize;
            uint32_t q = bsize / typesize, r = bsize % typesize;
            for (uint32_t i = 0; i < q; i++)
                for (uint32_t j = 0; j < typesize; j++) d[i * typesize + j] = tmp[j * q + i];
            memcpy(d + bsize - r, tmp + bsize - r, r);
        }
    }
    free(tmp);
    return ok;
}

// --- Reader -----------------------------------------------------------------------------------------------------------

typedef struct Rd {
    const uint8_t* p;
    size_t n, pos;
    bool bad;
    uint32_t version;
    uint32_t compression; // of the grid being read
} Rd;

static bool rd_need(Rd* r, size_t k) {
    if (r->bad || r->pos + k > r->n || r->pos + k < r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}
static void rd_bytes(Rd* r, void* out, size_t k) {
    if (!rd_need(r, k)) {
        memset(out, 0, k);
        return;
    }
    memcpy(out, r->p + r->pos, k);
    r->pos += k;
}
static uint8_t rd_u8(Rd* r) {
    uint8_t v = 0;
    rd_bytes(r, &v, 1);
    return v;
}
static uint32_t rd_u32(Rd* r) {
    uint32_t v = 0;
    rd_bytes(r, &v, 4);
    return v;
}
static int32_t rd_i32(Rd* r) { return (int32_t)rd_u32(r); }
static int64_t rd_i64(Rd* r) {
    int64_t v = 0;
    rd_bytes(r, &v, 8);
    return v;
}
static double rd_f64(Rd* r) {
    double v = 0;
    rd_bytes(r, &v, 8);
    return v;
}
static void rd_skip(Rd* r, size_t k) {
    if (rd_need(r, k)) r->pos += k;
}
// A string (uint32 length + bytes) into out (truncated, always terminated).
static void rd_string(Rd* r, char* out, size_t cap) {
    uint32_t len = rd_u32(r);
    if (!rd_need(r, len)) {
        if (cap) out[0] = 0;
        return;
    }
    size_t k = len < cap - 1 ? len : cap - 1;
    memcpy(out, r->p + r->pos, k);
    out[k] = 0;
    r->pos += len;
}

// Metadata of a MetaMap that the reader keeps.
typedef struct GridMeta {
    char grid_class[32];
    char vector_type[32];
} GridMeta;

static void rd_metamap(Rd* r, GridMeta* gm) {
    int32_t count = rd_i32(r);
    if (count < 0 || count > 100000) {
        r->bad = true;
        return;
    }
    for (int i = 0; i < count && !r->bad; i++) {
        char name[256], type[64];
        rd_string(r, name, sizeof name);
        rd_string(r, type, sizeof type);
        uint32_t size = rd_u32(r);
        if (!rd_need(r, size)) return;
        if (gm && strcmp(type, "string") == 0) {
            char* dst = strcmp(name, "class") == 0 ? gm->grid_class : strcmp(name, "vector_type") == 0 ? gm->vector_type : NULL;
            if (dst) {
                size_t k = size < 31 ? size : 31;
                memcpy(dst, r->p + r->pos, k);
                dst[k] = 0;
            }
        }
        r->pos += size; // unknown types too (OpenVDB's UnknownMetadata)
    }
}

// The transform: a map type name and its doubles, as an index -> world mat4.
static bool rd_transform(Rd* r, mat4* out, char* err, int cap) {
    char type[64];
    rd_string(r, type, sizeof type);
    double v[18]; // ScaleTranslateMap: 18 doubles
    mat4 m = mat4_identity();
    if (strcmp(type, "UniformScaleMap") == 0 || strcmp(type, "ScaleMap") == 0) {
        for (int i = 0; i < 15; i++) v[i] = rd_f64(r); // scale, voxel size, inverse, inverse squared, inverse twice
        m.m[0] = (float)v[0], m.m[5] = (float)v[1], m.m[10] = (float)v[2];
    } else if (strcmp(type, "UniformScaleTranslateMap") == 0 || strcmp(type, "ScaleTranslateMap") == 0) {
        for (int i = 0; i < 18; i++) v[i] = rd_f64(r); // translation, then the five vectors of the scale map
        m.m[12] = (float)v[0], m.m[13] = (float)v[1], m.m[14] = (float)v[2];
        m.m[0] = (float)v[3], m.m[5] = (float)v[4], m.m[10] = (float)v[5];
    } else if (strcmp(type, "TranslationMap") == 0) {
        for (int i = 0; i < 3; i++) v[i] = rd_f64(r);
        m.m[12] = (float)v[0], m.m[13] = (float)v[1], m.m[14] = (float)v[2];
    } else if (strcmp(type, "AffineMap") == 0 || strcmp(type, "UnitaryMap") == 0) {
        // OpenVDB's Mat4d with row vectors: row i is the image of index axis i, row 3 the translation; that is the
        // column-major layout of mat4.
        for (int i = 0; i < 16; i++) m.m[i] = (float)rd_f64(r);
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "Transformação do OpenVDB não suportada: %s", type);
        set_err(err, cap, msg);
        return false;
    }
    *out = m;
    return !r->bad;
}

typedef struct ValueKind {
    StoreType st;  // in memory (the file's ValueType: f32, f64, i32, bool)
    bool half;     // stored as half floats in the buffers
    int comps;     // 1 or 3
} ValueKind;

static size_t value_bytes(ValueKind k) {
    size_t s = k.st == ST_F64 ? 8 : k.st == ST_BOOL ? 1 : 4;
    return s * (size_t)k.comps;
}
static size_t store_bytes(ValueKind k) { return k.half ? 2u * (size_t)k.comps : value_bytes(k); }

// One value of the in-memory type (not half) into float[comps].
static void rd_value(Rd* r, ValueKind k, float* out) {
    for (int c = 0; c < k.comps; c++) {
        if (k.st == ST_F64) out[c] = (float)rd_f64(r);
        else if (k.st == ST_I32) out[c] = (float)rd_i32(r);
        else if (k.st == ST_BOOL) out[c] = rd_u8(r) ? 1.0f : 0.0f;
        else {
            uint32_t b = rd_u32(r);
            memcpy(&out[c], &b, 4);
        }
    }
}

// Converts count stored elements (half or the value type) to floats.
static void decode_store(const uint8_t* src, ValueKind k, size_t count, float* out) {
    size_t n = count * (size_t)k.comps;
    if (k.half) {
        for (size_t i = 0; i < n; i++) out[i] = half_to_float((uint16_t)(src[2 * i] | (src[2 * i + 1] << 8)));
    } else if (k.st == ST_F64) {
        for (size_t i = 0; i < n; i++) {
            double d;
            memcpy(&d, src + 8 * i, 8);
            out[i] = (float)d;
        }
    } else if (k.st == ST_I32) {
        for (size_t i = 0; i < n; i++) {
            int32_t d;
            memcpy(&d, src + 4 * i, 4);
            out[i] = (float)d;
        }
    } else if (k.st == ST_BOOL) {
        for (size_t i = 0; i < n; i++) out[i] = src[i] ? 1.0f : 0.0f;
    } else {
        memcpy(out, src, n * 4);
    }
}

// OpenVDB's readData: count stored elements, compressed with the grid's settings.
static bool rd_data(Rd* r, ValueKind k, size_t count, float* out, uint8_t** scratch, size_t* scratch_cap) {
    size_t bytes = count * store_bytes(k);
    // no values: nothing stored without compression, and nothing at all for half grids (OpenVDB's HalfReader returns
    // before reading the size)
    if (count == 0 && (k.half || !(r->compression & (COMPRESS_ZIP | COMPRESS_BLOSC)))) return true;
    const uint8_t* raw = NULL;
    if (r->compression & (COMPRESS_BLOSC | COMPRESS_ZIP)) {
        int64_t cs = rd_i64(r);
        if (cs <= 0) {
            if ((size_t)(-cs) != bytes || !rd_need(r, bytes)) return r->bad = true, false;
            raw = r->p + r->pos;
            r->pos += bytes;
        } else {
            if (!rd_need(r, (size_t)cs)) return false;
            if (*scratch_cap < bytes) {
                uint8_t* nb = realloc(*scratch, bytes);
                if (!nb) return r->bad = true, false;
                *scratch = nb;
                *scratch_cap = bytes;
            }
            bool ok = (r->compression & COMPRESS_BLOSC) ? blosc_decode(r->p + r->pos, (size_t)cs, *scratch, bytes)
                                                        : re_inflate(r->p + r->pos, (size_t)cs, *scratch, bytes);
            r->pos += (size_t)cs;
            if (!ok) return r->bad = true, false;
            raw = *scratch;
        }
    } else {
        if (!rd_need(r, bytes)) return false;
        raw = r->p + r->pos;
        r->pos += bytes;
    }
    if (count) decode_store(raw, k, count, out);
    return true;
}

static bool mask_on(const uint64_t* m, size_t i) { return (m[i >> 6] >> (i & 63)) & 1; }
static size_t mask_count(const uint64_t* m, size_t words) {
    size_t c = 0;
    for (size_t w = 0; w < words; w++) {
        uint64_t x = m[w];
        while (x) {
            x &= x - 1;
            c++;
        }
    }
    return c;
}

// OpenVDB's readCompressedValues: n values (float[comps] each) of a node with its value mask.
typedef struct Scratch {
    float* tmp;
    size_t tmp_cap;
    uint8_t* raw;
    size_t raw_cap;
} Scratch;

static bool rd_compressed_values(Rd* r, ValueKind k, const float* background, size_t n, const uint64_t* value_mask, float* dest, Scratch* sc) {
    int nc = k.comps;
    int8_t meta = (int8_t)rd_u8(r);
    float inactive0[3], inactive1[3];
    for (int c = 0; c < nc; c++) {
        inactive1[c] = background[c];
        inactive0[c] = meta == NO_MASK_OR_INACTIVE_VALS ? background[c] : -background[c];
    }
    if (meta == NO_MASK_AND_ONE_INACTIVE_VAL || meta == MASK_AND_ONE_INACTIVE_VAL || meta == MASK_AND_TWO_INACTIVE_VALS) {
        ValueKind full = k;
        full.half = false;
        rd_value(r, full, inactive0);
        if (meta == MASK_AND_TWO_INACTIVE_VALS) rd_value(r, full, inactive1);
    }
    uint64_t selection[512]; // up to 32^3 values
    size_t words = (n + 63) / 64;
    memset(selection, 0, words * 8);
    if (meta == MASK_AND_NO_INACTIVE_VALS || meta == MASK_AND_ONE_INACTIVE_VAL || meta == MASK_AND_TWO_INACTIVE_VALS)
        rd_bytes(r, selection, words * 8);
    size_t count = n;
    bool masked = (r->compression & COMPRESS_ACTIVE_MASK) && meta != NO_MASK_AND_ALL_VALS;
    if (masked) count = mask_count(value_mask, words);
    float* buf = dest;
    if (masked && count != n) {
        size_t need = count * (size_t)nc;
        if (sc->tmp_cap < need) {
            float* nb = realloc(sc->tmp, need * sizeof *nb + 4);
            if (!nb) return r->bad = true, false;
            sc->tmp = nb;
            sc->tmp_cap = need;
        }
        buf = sc->tmp;
    }
    if (!rd_data(r, k, count, buf, &sc->raw, &sc->raw_cap)) return false;
    if (masked && count != n) {
        for (size_t d = 0, t = 0; d < n; d++) {
            const float* v;
            if (mask_on(value_mask, d)) v = &buf[(t++) * nc];
            else v = mask_on(selection, d) ? inactive1 : inactive0;
            for (int c = 0; c < nc; c++) dest[d * nc + c] = v[c];
        }
    }
    return !r->bad;
}

// The tree as read: leaves (origin, mask, values in VDB order) and tiles to expand.
typedef struct RdLeaf {
    int32_t origin[3];
    uint64_t mask[8];
} RdLeaf;

typedef struct RdTile {
    int32_t origin[3];
    int size; // voxels per side (8, 128, 4096)
    float value[3];
    bool active;
} RdTile;

typedef struct RdTree {
    RdLeaf* leaves;
    int leaf_count, leaf_cap;
    RdTile* tiles;
    int tile_count, tile_cap;
} RdTree;

static bool push_leaf(RdTree* t, const int32_t o[3], const uint64_t m[8]) {
    if (t->leaf_count == t->leaf_cap) {
        int cap = t->leaf_cap ? t->leaf_cap * 2 : 256;
        RdLeaf* n = realloc(t->leaves, (size_t)cap * sizeof *n);
        if (!n) return false;
        t->leaves = n;
        t->leaf_cap = cap;
    }
    RdLeaf* l = &t->leaves[t->leaf_count++];
    memcpy(l->origin, o, sizeof l->origin);
    memcpy(l->mask, m, sizeof l->mask);
    return true;
}

static bool push_tile(RdTree* t, const int32_t o[3], int size, const float* v, int nc, bool active) {
    if (t->tile_count == t->tile_cap) {
        int cap = t->tile_cap ? t->tile_cap * 2 : 64;
        RdTile* n = realloc(t->tiles, (size_t)cap * sizeof *n);
        if (!n) return false;
        t->tiles = n;
        t->tile_cap = cap;
    }
    RdTile* tl = &t->tiles[t->tile_count++];
    memcpy(tl->origin, o, sizeof tl->origin);
    tl->size = size;
    for (int c = 0; c < 3; c++) tl->value[c] = c < nc ? v[c] : 0.0f;
    tl->active = active;
    return true;
}

// An internal node (log2 dim 5 or 4) of the topology at origin.
static bool rd_internal(Rd* r, ValueKind k, const float* bg, int log2dim, const int32_t origin[3], RdTree* t, Scratch* sc) {
    int dim = 1 << log2dim;
    size_t nvals = (size_t)dim * dim * dim, words = nvals / 64;
    int child_total = log2dim == 5 ? 7 : 3; // the child's extent: 2^7 for node 4, 2^3 for leaves
    uint64_t* masks = malloc(words * 2 * 8);
    float* values = malloc(nvals * (size_t)k.comps * sizeof(float));
    if (!masks || !values) {
        free(masks);
        free(values);
        return r->bad = true, false;
    }
    uint64_t *child = masks, *vmask = masks + words;
    rd_bytes(r, child, words * 8);
    rd_bytes(r, vmask, words * 8);
    bool ok = rd_compressed_values(r, k, bg, nvals, vmask, values, sc);
    int csize = 1 << child_total;
    // Tiles (no child): active ones, and inactive ones that differ from the background (a level set's inside)
    for (size_t i = 0; ok && i < nvals; i++) {
        if (mask_on(child, i)) continue;
        const float* v = &values[i * k.comps];
        bool active = mask_on(vmask, i), differs = false;
        for (int c = 0; c < k.comps; c++) differs |= v[c] != bg[c];
        if (!active && !differs) continue;
        int32_t o[3] = { origin[0] + (int32_t)((i >> (2 * log2dim)) & (dim - 1)) * csize,
                         origin[1] + (int32_t)((i >> log2dim) & (dim - 1)) * csize, origin[2] + (int32_t)(i & (dim - 1)) * csize };
        ok = push_tile(t, o, csize, v, k.comps, active);
    }
    for (size_t i = 0; ok && i < nvals; i++) {
        if (!mask_on(child, i)) continue;
        int32_t o[3] = { origin[0] + (int32_t)((i >> (2 * log2dim)) & (dim - 1)) * csize,
                         origin[1] + (int32_t)((i >> log2dim) & (dim - 1)) * csize, origin[2] + (int32_t)(i & (dim - 1)) * csize };
        if (log2dim == 4) {
            uint64_t lm[8];
            rd_bytes(r, lm, sizeof lm);
            ok = !r->bad && push_leaf(t, o, lm);
        } else {
            ok = rd_internal(r, k, bg, 4, o, t, sc);
        }
    }
    free(masks);
    free(values);
    return ok && !r->bad;
}

static uint32_t hash_name(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// Writes one voxel value (VDB leaf offset) of a read leaf into the VolumeGrid leaf.
static void put_voxel(VolumeLeaf* vl, int nc, int vdb_off, const float* v, bool active) {
    int x = vdb_off >> 6, y = (vdb_off >> 3) & 7, z = vdb_off & 7;
    int o = x + VG_LEAF_DIM * (y + VG_LEAF_DIM * z);
    for (int c = 0; c < nc; c++) vl->values[o * nc + c] = v[c];
    if (active) vl->active[o >> 6] |= 1ull << (o & 63);
    else vl->active[o >> 6] &= ~(1ull << (o & 63));
}

// Active tiles become leaves: a damaged file with a few huge active tiles asked for gigabytes and 20+ s (a 400^3 block of
// active constant voxels still reads)
#define VDB_MAX_TILE_VOXELS (64ll * 1024 * 1024)

static bool expand_tiles(VolumeGrid* g, const RdTree* t, int nc) {
    long long voxels = 0;
    for (int i = 0; i < t->tile_count; i++) {
        const RdTile* tl = &t->tiles[i];
        // inactive big tiles (a level set's inside beyond the narrow band) are not expanded: they read as background
        if (!tl->active && tl->size > VG_LEAF_DIM) continue;
        voxels += (long long)tl->size * tl->size * tl->size;
        if (voxels > VDB_MAX_TILE_VOXELS) return false;
        for (int x = 0; x < tl->size; x += VG_LEAF_DIM)
            for (int y = 0; y < tl->size; y += VG_LEAF_DIM)
                for (int z = 0; z < tl->size; z += VG_LEAF_DIM) {
                    VolumeLeaf* vl = volume_grid_touch_leaf(g, tl->origin[0] + x, tl->origin[1] + y, tl->origin[2] + z);
                    if (!vl) return false;
                    for (int v = 0; v < VG_LEAF_VOXELS; v++) {
                        for (int c = 0; c < nc; c++) vl->values[v * nc + c] = tl->value[c];
                        if (tl->active) vl->active[v >> 6] |= 1ull << (v & 63);
                    }
                }
    }
    return true;
}

typedef struct GridDesc {
    char name[VG_NAME_LEN];
    char type[96];
    char parent[VG_NAME_LEN];
    int64_t grid_pos, block_pos, end_pos;
} GridDesc;

static bool parse_type(const char* type, ValueKind* k) {
    char base[96];
    snprintf(base, sizeof base, "%s", type);
    size_t n = strlen(base);
    const char* hs = "_HalfFloat";
    size_t hn = strlen(hs);
    k->half = false;
    if (n > hn && strcmp(base + n - hn, hs) == 0) {
        k->half = true;
        base[n - hn] = 0;
    }
    k->comps = 1;
    if (strcmp(base, "Tree_float_5_4_3") == 0) k->st = ST_F32;
    else if (strcmp(base, "Tree_double_5_4_3") == 0) k->st = ST_F64;
    else if (strcmp(base, "Tree_int32_5_4_3") == 0) k->st = ST_I32;
    else if (strcmp(base, "Tree_bool_5_4_3") == 0) k->st = ST_BOOL;
    else if (strcmp(base, "Tree_vec3s_5_4_3") == 0) k->st = ST_F32, k->comps = 3;
    else if (strcmp(base, "Tree_vec3d_5_4_3") == 0) k->st = ST_F64, k->comps = 3;
    else if (strcmp(base, "Tree_vec3i_5_4_3") == 0) k->st = ST_I32, k->comps = 3;
    else return false;
    // half applies to floating point grids only
    if (k->half && k->st != ST_F32 && k->st != ST_F64) k->half = false;
    if (k->half && k->st == ST_F64) k->st = ST_F64;
    return true;
}

static int class_of(const char* s) {
    if (strcmp(s, "fog volume") == 0) return VG_CLASS_FOG;
    if (strcmp(s, "level set") == 0) return VG_CLASS_LEVEL_SET;
    if (strcmp(s, "staggered") == 0) return VG_CLASS_STAGGERED;
    return VG_CLASS_UNKNOWN;
}

// Reads the grid whose descriptor was just read (r at the grid position) into g.
static bool rd_grid(Rd* r, ValueKind k, VolumeGrid* g, Scratch* sc, char* err, int cap) {
    r->compression = r->version >= VDB_MIN_VERSION ? rd_u32(r) : 0;
    GridMeta gm;
    memset(&gm, 0, sizeof gm);
    rd_metamap(r, &gm);
    g->grid_class = gm.grid_class[0] ? class_of(gm.grid_class) : VG_CLASS_UNKNOWN;
    if (!rd_transform(r, &g->xform, err, cap)) return false;
    int32_t buffers = rd_i32(r);
    if (buffers != 1 && !r->bad) {
        set_err(err, cap, "Árvore do OpenVDB com vários buffers (arquivo antigo)");
        return false;
    }
    float bg[3] = { 0, 0, 0 };
    rd_value(r, k, bg);
    for (int c = 0; c < k.comps; c++) g->background[c] = bg[c];
    uint32_t ntiles = rd_u32(r), nchildren = rd_u32(r);
    if (r->bad || ntiles > 100000000u || nchildren > 100000000u) return r->bad = true, false;
    RdTree t;
    memset(&t, 0, sizeof t);
    bool ok = true;
    for (uint32_t i = 0; ok && i < ntiles; i++) {
        int32_t o[3] = { rd_i32(r), rd_i32(r), rd_i32(r) };
        float v[3] = { 0, 0, 0 };
        rd_value(r, k, v);
        bool active = rd_u8(r) != 0;
        if (active) ok = push_tile(&t, o, 4096, v, k.comps, true);
    }
    for (uint32_t i = 0; ok && i < nchildren && !r->bad; i++) {
        int32_t o[3] = { rd_i32(r), rd_i32(r), rd_i32(r) };
        ok = rd_internal(r, k, bg, 5, o, &t, sc);
    }
    // Leaf buffers, in the topology's order
    int nc = k.comps;
    float vals[VG_LEAF_VOXELS * 3];
    for (int i = 0; ok && i < t.leaf_count; i++) {
        RdLeaf* l = &t.leaves[i];
        rd_bytes(r, l->mask, sizeof l->mask);
        ok = rd_compressed_values(r, k, bg, VG_LEAF_VOXELS, l->mask, vals, sc);
        if (!ok) break;
        VolumeLeaf* vl = volume_grid_touch_leaf(g, l->origin[0], l->origin[1], l->origin[2]);
        if (!vl) {
            ok = false;
            break;
        }
        for (int v = 0; v < VG_LEAF_VOXELS; v++) put_voxel(vl, nc, v, &vals[v * nc], mask_on(l->mask, (size_t)v));
    }
    if (ok && !expand_tiles(g, &t, nc)) {
        set_err(err, cap, "Volume grande demais (blocos constantes)");
        ok = false;
    }
    free(t.leaves);
    free(t.tiles);
    if (!ok && (!err || !err[0])) set_err(err, cap, r->bad ? "Arquivo OpenVDB danificado" : "Sem memória");
    return ok && !r->bad;
}

static bool load_file(const char* path, uint8_t** data, size_t* size, char* err, int cap) {
    FILE* f = re_fopen(path, "rb");
    if (!f) {
        set_err(err, cap, "Não foi possível abrir o arquivo");
        return false;
    }
    size_t c = 1 << 20, n = 0;
    uint8_t* buf = malloc(c);
    while (buf) {
        if (n == c) {
            uint8_t* nb = realloc(buf, c * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            c *= 2;
        }
        size_t got = fread(buf + n, 1, c - n, f);
        if (got == 0) break;
        n += got;
    }
    fclose(f);
    if (!buf) {
        set_err(err, cap, "Sem memória");
        return false;
    }
    *data = buf;
    *size = n;
    return true;
}

// Header up to the grid count; false (with a message) for something that isn't a VDB of a supported version.
static bool rd_header(Rd* r, bool* has_offsets, char* err, int cap) {
    int64_t magic = rd_i64(r);
    if (r->bad || (magic & 0xffffffffLL) != VDB_MAGIC) {
        set_err(err, cap, "Não é um arquivo OpenVDB");
        return false;
    }
    r->version = rd_u32(r);
    if (r->version < VDB_MIN_VERSION) {
        set_err(err, cap, "Versão do OpenVDB antiga demais (anterior à 2.0)");
        return false;
    }
    rd_u32(r), rd_u32(r); // library version
    *has_offsets = rd_u8(r) != 0;
    rd_skip(r, 36); // uuid
    rd_metamap(r, NULL);
    if (r->bad) set_err(err, cap, "Arquivo OpenVDB danificado");
    return !r->bad;
}

static void rd_desc(Rd* r, GridDesc* d) {
    char uname[256];
    rd_string(r, uname, sizeof uname);
    char* sep = strchr(uname, '\x1e'); // unique names of duplicate grids: name + "\x1e" + n
    if (sep) *sep = 0;
    snprintf(d->name, sizeof d->name, "%s", uname);
    rd_string(r, d->type, sizeof d->type);
    d->parent[0] = 0;
    if (r->version >= VDB_GRID_INSTANCING) rd_string(r, d->parent, sizeof d->parent);
    d->grid_pos = rd_i64(r);
    d->block_pos = rd_i64(r);
    d->end_pos = rd_i64(r);
}

bool vdb_probe(const char* path, VdbInfo* out, char* err, int cap) {
    memset(out, 0, sizeof *out);
    uint8_t* data;
    size_t size;
    if (!load_file(path, &data, &size, err, cap)) return false;
    Rd r = { data, size, 0, false, 0, 0 };
    bool offsets;
    bool ok = rd_header(&r, &offsets, err, cap);
    out->file_version = (int)r.version;
    if (ok) {
        int32_t count = rd_i32(&r);
        if (count < 0 || count > 100000) count = 0;
        out->grid_count = count;
        for (int i = 0; i < count && !r.bad; i++) {
            GridDesc d;
            rd_desc(&r, &d);
            ValueKind k;
            bool known = parse_type(d.type, &k);
            if (i < 16) {
                snprintf(out->names[i], VG_NAME_LEN, "%s", d.name);
                out->types[i] = !known ? -1 : k.comps == 3 ? VG_VEC3 : VG_FLOAT;
            }
            if (offsets && d.grid_pos > 0 && (size_t)d.grid_pos + 4 <= size) {
                r.pos = (size_t)d.grid_pos;
                uint32_t c = rd_u32(&r);
                out->compressed |= (c & (COMPRESS_ZIP | COMPRESS_BLOSC)) != 0;
                if (d.end_pos > 0 && (size_t)d.end_pos <= size) r.pos = (size_t)d.end_pos;
                else break;
            } else {
                break; // a stream without offsets: the names read so far
            }
        }
        if (r.bad) {
            set_err(err, cap, "Arquivo OpenVDB danificado");
            ok = false;
        }
    }
    free(data);
    return ok;
}

static bool wanted(const char* name, const char* const* names, int count) {
    if (!names) return true;
    for (int i = 0; i < count; i++)
        if (names[i] && strcmp(names[i], name) == 0) return true;
    return false;
}

bool vdb_read(const char* path, VolumeGridSet* out, const char* const* names, int name_count, char* err, int cap) {
    memset(out, 0, sizeof *out);
    if (err && cap) err[0] = 0;
    uint8_t* data;
    size_t size;
    if (!load_file(path, &data, &size, err, cap)) return false;
    Rd r = { data, size, 0, false, 0, 0 };
    bool offsets;
    bool ok = rd_header(&r, &offsets, err, cap);
    Scratch sc;
    memset(&sc, 0, sizeof sc);
    if (ok) {
        int32_t count = rd_i32(&r);
        if (r.bad || count < 0 || count > 100000) {
            set_err(err, cap, "Arquivo OpenVDB danificado");
            ok = false;
        }
        for (int i = 0; ok && i < count; i++) {
            GridDesc d;
            rd_desc(&r, &d);
            if (r.bad) {
                ok = false;
                break;
            }
            if (offsets && d.grid_pos > 0) {
                if ((size_t)d.grid_pos > size) {
                    ok = false;
                    break;
                }
                r.pos = (size_t)d.grid_pos;
            }
            ValueKind k;
            bool known = parse_type(d.type, &k);
            bool skip = !known || !wanted(d.name, names, name_count);
            if (skip) {
                if (!offsets || d.end_pos <= 0 || (size_t)d.end_pos > size) {
                    if (!known) continue; // nothing more to learn from this stream: keep what was read
                    ok = false;
                    break;
                }
                r.pos = (size_t)d.end_pos;
                continue;
            }
            if (d.parent[0]) { // an instance: metadata and transform of its own, the tree of its parent
                const VolumeGrid* parent = volume_set_find(out, d.parent);
                r.compression = rd_u32(&r);
                GridMeta gm;
                memset(&gm, 0, sizeof gm);
                rd_metamap(&r, &gm);
                mat4 xf;
                if (!rd_transform(&r, &xf, err, cap)) {
                    ok = false;
                    break;
                }
                if (parent) {
                    VolumeGrid copy;
                    if (!volume_grid_copy(&copy, parent)) {
                        ok = false;
                        break;
                    }
                    VolumeGrid* g = volume_set_add(out, d.name, copy.type, 1.0f);
                    if (!g) {
                        volume_grid_free(&copy);
                        ok = false;
                        break;
                    }
                    *g = copy;
                    snprintf(g->name, sizeof g->name, "%s", d.name);
                    g->xform = xf;
                    if (gm.grid_class[0]) g->grid_class = class_of(gm.grid_class);
                }
                if (offsets && d.end_pos > 0 && (size_t)d.end_pos <= size) r.pos = (size_t)d.end_pos;
                continue;
            }
            VolumeGrid* g = volume_set_add(out, d.name, k.comps == 3 ? VG_VEC3 : VG_FLOAT, 1.0f);
            if (!g) {
                set_err(err, cap, "Sem memória");
                ok = false;
                break;
            }
            ok = rd_grid(&r, k, g, &sc, err, cap);
            if (ok && offsets && d.end_pos > 0 && (size_t)d.end_pos <= size) r.pos = (size_t)d.end_pos;
        }
    }
    (void)hash_name;
    free(sc.tmp);
    free(sc.raw);
    free(data);
    if (!ok) {
        if (err && cap && !err[0]) set_err(err, cap, "Arquivo OpenVDB danificado");
        volume_set_free(out);
    }
    return ok;
}

// --- Writer -----------------------------------------------------------------------------------------------------------

typedef struct Wr {
    uint8_t* p;
    size_t n, cap;
    bool bad;
} Wr;

static void wr_bytes(Wr* w, const void* src, size_t k) {
    if (w->bad) return;
    if (w->n + k > w->cap) {
        size_t c = w->cap ? w->cap : 1 << 16;
        while (c < w->n + k) c *= 2;
        uint8_t* nb = realloc(w->p, c);
        if (!nb) {
            w->bad = true;
            return;
        }
        w->p = nb;
        w->cap = c;
    }
    memcpy(w->p + w->n, src, k);
    w->n += k;
}
static void wr_u8(Wr* w, uint8_t v) { wr_bytes(w, &v, 1); }
static void wr_u32(Wr* w, uint32_t v) { wr_bytes(w, &v, 4); }
static void wr_i32(Wr* w, int32_t v) { wr_bytes(w, &v, 4); }
static void wr_i64(Wr* w, int64_t v) { wr_bytes(w, &v, 8); }
static void wr_f32(Wr* w, float v) { wr_bytes(w, &v, 4); }
static void wr_f64(Wr* w, double v) { wr_bytes(w, &v, 8); }
static void wr_string(Wr* w, const char* s) {
    uint32_t n = (uint32_t)strlen(s);
    wr_u32(w, n);
    wr_bytes(w, s, n);
}
static void wr_meta_string(Wr* w, const char* name, const char* value) {
    wr_string(w, name);
    wr_string(w, "string");
    wr_string(w, value);
}
static void wr_meta_vec3i(Wr* w, const char* name, const int v[3]) {
    wr_string(w, name);
    wr_string(w, "vec3i");
    wr_u32(w, 12);
    for (int i = 0; i < 3; i++) wr_i32(w, v[i]);
}
static void wr_meta_i64(Wr* w, const char* name, int64_t v) {
    wr_string(w, name);
    wr_string(w, "int64");
    wr_u32(w, 8);
    wr_i64(w, v);
}
static void wr_meta_bool(Wr* w, const char* name, bool v) {
    wr_string(w, name);
    wr_string(w, "bool");
    wr_u32(w, 1);
    wr_u8(w, v ? 1 : 0);
}

// One stored data block (OpenVDB's writeData): count values (float[nc] each), half or float, zip or not.
static void wr_data(Wr* w, const float* v, size_t count, int nc, bool half, bool zip) {
    if (count == 0 && half) return; // OpenVDB's HalfWriter writes nothing for no values
    size_t n = count * (size_t)nc, bytes = n * (half ? 2 : 4);
    uint8_t stackbuf[VG_LEAF_VOXELS * 3 * 4];
    uint8_t* raw = bytes <= sizeof stackbuf ? stackbuf : malloc(bytes);
    if (!raw) {
        w->bad = true;
        return;
    }
    if (half) {
        for (size_t i = 0; i < n; i++) {
            uint16_t h = float_to_half(v[i]);
            raw[2 * i] = (uint8_t)h, raw[2 * i + 1] = (uint8_t)(h >> 8);
        }
    } else {
        memcpy(raw, v, bytes);
    }
    if (zip) {
        size_t zn = 0;
        uint8_t* z = bytes ? re_deflate(raw, bytes, &zn) : NULL;
        if (z && zn < bytes) {
            wr_i64(w, (int64_t)zn);
            wr_bytes(w, z, zn);
        } else {
            wr_i64(w, -(int64_t)bytes);
            wr_bytes(w, raw, bytes);
        }
        free(z);
    } else {
        wr_bytes(w, raw, bytes);
    }
    if (raw != stackbuf) free(raw);
}

static bool veq(const float* a, const float* b, int nc) {
    for (int c = 0; c < nc; c++)
        if (a[c] != b[c]) return false;
    return true;
}

// OpenVDB's writeCompressedValues with active mask compression: n values in VDB order and their value mask.
static void wr_compressed_values(Wr* w, const float* vals, size_t n, const uint64_t* vmask, const uint64_t* cmask,
                                 const float* bg, int nc, bool half, bool zip) {
    float inactive[2][3] = { { 0 } };
    int unique = 0;
    for (size_t i = 0; i < n && unique < 3; i++) {
        if (mask_on(vmask, i) || (cmask && mask_on(cmask, i))) continue;
        const float* v = &vals[i * nc];
        bool u = !((unique > 0 && veq(v, inactive[0], nc)) || (unique > 1 && veq(v, inactive[1], nc)));
        if (u) {
            if (unique < 2) memcpy(inactive[unique], v, (size_t)nc * 4);
            unique++;
        }
    }
    float nbg[3];
    for (int c = 0; c < nc; c++) nbg[c] = -bg[c];
    int meta = NO_MASK_OR_INACTIVE_VALS;
    if (unique == 1) {
        if (!veq(inactive[0], bg, nc)) meta = veq(inactive[0], nbg, nc) ? NO_MASK_AND_MINUS_BG : NO_MASK_AND_ONE_INACTIVE_VAL;
    } else if (unique == 2) {
        if (!veq(inactive[0], bg, nc) && !veq(inactive[1], bg, nc)) {
            meta = MASK_AND_TWO_INACTIVE_VALS;
        } else if (veq(inactive[1], bg, nc)) {
            meta = veq(inactive[0], nbg, nc) ? MASK_AND_NO_INACTIVE_VALS : MASK_AND_ONE_INACTIVE_VAL;
        } else if (veq(inactive[0], bg, nc)) {
            float t[3];
            memcpy(t, inactive[0], sizeof t);
            memcpy(inactive[0], inactive[1], sizeof t);
            memcpy(inactive[1], t, sizeof t);
            meta = veq(inactive[0], nbg, nc) ? MASK_AND_NO_INACTIVE_VALS : MASK_AND_ONE_INACTIVE_VAL;
        }
    } else if (unique > 2) {
        meta = NO_MASK_AND_ALL_VALS;
    }
    wr_u8(w, (uint8_t)meta);
    if (meta == NO_MASK_AND_ONE_INACTIVE_VAL || meta == MASK_AND_ONE_INACTIVE_VAL || meta == MASK_AND_TWO_INACTIVE_VALS) {
        for (int c = 0; c < nc; c++) wr_f32(w, half ? truncate_half(inactive[0][c]) : inactive[0][c]);
        if (meta == MASK_AND_TWO_INACTIVE_VALS)
            for (int c = 0; c < nc; c++) wr_f32(w, half ? truncate_half(inactive[1][c]) : inactive[1][c]);
    }
    if (meta == NO_MASK_AND_ALL_VALS) {
        wr_data(w, vals, n, nc, half, zip);
        return;
    }
    float* tmp = malloc((n ? n : 1) * (size_t)nc * sizeof *tmp);
    if (!tmp) {
        w->bad = true;
        return;
    }
    size_t count = 0;
    if (meta == NO_MASK_OR_INACTIVE_VALS || meta == NO_MASK_AND_MINUS_BG || meta == NO_MASK_AND_ONE_INACTIVE_VAL) {
        for (size_t i = 0; i < n; i++)
            if (mask_on(vmask, i)) memcpy(&tmp[(count++) * nc], &vals[i * nc], (size_t)nc * 4);
    } else {
        uint64_t sel[512];
        size_t words = (n + 63) / 64;
        memset(sel, 0, words * 8);
        for (size_t i = 0; i < n; i++) {
            if (mask_on(vmask, i)) memcpy(&tmp[(count++) * nc], &vals[i * nc], (size_t)nc * 4);
            else if (veq(&vals[i * nc], inactive[1], nc)) sel[i >> 6] |= 1ull << (i & 63);
        }
        wr_bytes(w, sel, words * 8);
    }
    wr_data(w, tmp, count, nc, half, zip);
    free(tmp);
}

// A leaf of the grid in VDB order: its values and mask (z fastest).
static void leaf_to_vdb(const VolumeLeaf* l, int nc, float* vals, uint64_t mask[8]) {
    memset(mask, 0, 64);
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            for (int z = 0; z < 8; z++) {
                int o = x + VG_LEAF_DIM * (y + VG_LEAF_DIM * z), d = (x << 6) | (y << 3) | z;
                for (int c = 0; c < nc; c++) vals[d * nc + c] = l->values[o * nc + c];
                if ((l->active[o >> 6] >> (o & 63)) & 1) mask[d >> 6] |= 1ull << (d & 63);
            }
}

typedef struct WLeaf {
    int32_t o[3];
    int src;
} WLeaf;

static int32_t key5(int32_t v) { return v & ~4095; }
static int cmp_wleaf(const void* a, const void* b) {
    const WLeaf *x = a, *y = b;
    for (int c = 0; c < 3; c++) { // root children: lexicographic (x, y, z) as OpenVDB's Coord ordering
        int32_t p = key5(x->o[c]), q = key5(y->o[c]);
        if (p != q) return p < q ? -1 : 1;
    }
    // inside a node 5: by node 4 index, then by leaf index (x most significant, z fastest)
    uint32_t i4x = (uint32_t)(((x->o[0] & 4095) >> 7) << 10 | ((x->o[1] & 4095) >> 7) << 5 | ((x->o[2] & 4095) >> 7));
    uint32_t i4y = (uint32_t)(((y->o[0] & 4095) >> 7) << 10 | ((y->o[1] & 4095) >> 7) << 5 | ((y->o[2] & 4095) >> 7));
    if (i4x != i4y) return i4x < i4y ? -1 : 1;
    uint32_t lx = (uint32_t)(((x->o[0] & 127) >> 3) << 8 | ((x->o[1] & 127) >> 3) << 4 | ((x->o[2] & 127) >> 3));
    uint32_t ly = (uint32_t)(((y->o[0] & 127) >> 3) << 8 | ((y->o[1] & 127) >> 3) << 4 | ((y->o[2] & 127) >> 3));
    return lx < ly ? -1 : lx > ly;
}

static bool same5(const int32_t a[3], const int32_t b[3]) {
    return key5(a[0]) == key5(b[0]) && key5(a[1]) == key5(b[1]) && key5(a[2]) == key5(b[2]);
}
static uint32_t idx4(const int32_t o[3]) {
    return (uint32_t)(((o[0] & 4095) >> 7) << 10 | ((o[1] & 4095) >> 7) << 5 | ((o[2] & 4095) >> 7));
}
static uint32_t idx_leaf(const int32_t o[3]) {
    return (uint32_t)(((o[0] & 127) >> 3) << 8 | ((o[1] & 127) >> 3) << 4 | ((o[2] & 127) >> 3));
}

static void wr_transform(Wr* w, const mat4* m) {
    const float* a = m->m;
    bool diag = a[1] == 0 && a[2] == 0 && a[4] == 0 && a[6] == 0 && a[8] == 0 && a[9] == 0 && a[3] == 0 && a[7] == 0 &&
                a[11] == 0 && a[15] == 1;
    bool uniform = diag && a[0] == a[5] && a[0] == a[10] && a[0] > 0;
    if (uniform) {
        double s = a[0];
        wr_string(w, "UniformScaleTranslateMap");
        double t[3] = { a[12], a[13], a[14] };
        for (int i = 0; i < 3; i++) wr_f64(w, t[i]);
        double vals[5] = { s, s, 1.0 / s, 1.0 / (s * s), 0.5 / s };
        for (int k = 0; k < 5; k++)
            for (int i = 0; i < 3; i++) wr_f64(w, vals[k]);
    } else {
        wr_string(w, "AffineMap");
        for (int i = 0; i < 16; i++) wr_f64(w, a[i]);
    }
}

static void make_uuid(char out[37], const VolumeGridSet* set) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(uintptr_t)set;
    for (int i = 0; i < set->count; i++) {
        h = (h ^ (uint64_t)set->grids[i].leaf_count) * 1099511628211ull;
        h = (h ^ hash_name(set->grids[i].name)) * 1099511628211ull;
    }
    h ^= (uint64_t)(re_time_seconds() * 1e6);
    const char* hex = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out[p++] = '-';
        h = h * 6364136223846793005ull + 1442695040888963407ull;
        out[p++] = hex[(h >> 59) & 15];
    }
    out[p] = 0;
}

static void wr_grid(Wr* w, const VolumeGrid* g, const VdbWriteOptions* opt) {
    int nc = volume_grid_components(g);
    bool half = opt && opt->half_float, zip = opt && opt->compress;
    const char* base = nc == 3 ? "Tree_vec3s_5_4_3" : "Tree_float_5_4_3";
    char type[64];
    snprintf(type, sizeof type, "%s%s", base, half ? "_HalfFloat" : "");
    // Leaves with something in them, sorted in the tree's order
    WLeaf* leaves = malloc((size_t)(g->leaf_count ? g->leaf_count : 1) * sizeof *leaves);
    if (!leaves) {
        w->bad = true;
        return;
    }
    int nl = 0;
    int lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    bool any = false;
    long long voxels = 0;
    for (int i = 0; i < g->leaf_count; i++) {
        const VolumeLeaf* l = &g->leaves[i];
        size_t act = mask_count(l->active, 8);
        bool keep = act > 0;
        for (int v = 0; !keep && v < VG_LEAF_VOXELS * nc; v++) keep = l->values[v] != g->background[v % nc];
        if (!keep) continue;
        voxels += (long long)act;
        leaves[nl].o[0] = l->origin[0], leaves[nl].o[1] = l->origin[1], leaves[nl].o[2] = l->origin[2];
        leaves[nl].src = i;
        nl++;
        if (act) {
            for (int v = 0; v < VG_LEAF_VOXELS; v++) {
                if (!((l->active[v >> 6] >> (v & 63)) & 1)) continue;
                int p[3] = { l->origin[0] + (v & 7), l->origin[1] + ((v >> 3) & 7), l->origin[2] + (v >> 6) };
                for (int a = 0; a < 3; a++) {
                    if (!any || p[a] < lo[a]) lo[a] = p[a];
                    if (!any || p[a] > hi[a]) hi[a] = p[a];
                }
                any = true;
            }
        }
    }
    qsort(leaves, (size_t)nl, sizeof *leaves, cmp_wleaf);

    // Descriptor
    wr_string(w, g->name);
    wr_string(w, type);
    wr_string(w, "");
    size_t pos_at = w->n;
    wr_i64(w, 0), wr_i64(w, 0), wr_i64(w, 0);
    int64_t grid_pos = (int64_t)w->n;
    uint32_t comp = COMPRESS_ACTIVE_MASK | (zip ? COMPRESS_ZIP : 0);
    wr_u32(w, comp);
    // Metadata
    const char* cls = g->grid_class == VG_CLASS_FOG ? "fog volume" : g->grid_class == VG_CLASS_LEVEL_SET ? "level set"
                      : g->grid_class == VG_CLASS_STAGGERED ? "staggered" : "unknown";
    wr_u32(w, nc == 3 ? 9 : 8);
    wr_meta_string(w, "class", cls);
    wr_meta_vec3i(w, "file_bbox_max", hi);
    wr_meta_vec3i(w, "file_bbox_min", lo);
    wr_meta_string(w, "file_compression", zip ? "zip + active values" : "active values");
    wr_meta_i64(w, "file_mem_bytes", (int64_t)volume_grid_memory(g));
    wr_meta_i64(w, "file_voxel_count", voxels);
    wr_meta_bool(w, "is_saved_as_half_float", half);
    wr_meta_string(w, "name", g->name);
    if (nc == 3) wr_meta_string(w, "vector_type", "invariant");
    wr_transform(w, &g->xform);
    // Topology
    wr_i32(w, 1);
    for (int c = 0; c < nc; c++) wr_f32(w, half ? truncate_half(g->background[c]) : g->background[c]);
    int n5 = 0;
    for (int i = 0; i < nl; i++)
        if (i == 0 || !same5(leaves[i].o, leaves[i - 1].o)) n5++;
    wr_u32(w, 0);
    wr_u32(w, (uint32_t)n5);
    float zero_vals[1] = { 0 };
    (void)zero_vals;
    uint64_t cmask5[512], vmask5[512], cmask4[64], vmask4[64];
    float* tile_vals = calloc(32768u * (size_t)nc, sizeof *tile_vals);
    if (!tile_vals) {
        free(leaves);
        w->bad = true;
        return;
    }
    for (int i = 0; i < 32768 * nc; i++) tile_vals[i] = g->background[i % nc];
    for (int a = 0; a < nl;) {
        int b = a;
        while (b < nl && same5(leaves[b].o, leaves[a].o)) b++;
        for (int c = 0; c < 3; c++) wr_i32(w, key5(leaves[a].o[c]));
        memset(cmask5, 0, sizeof cmask5);
        memset(vmask5, 0, sizeof vmask5);
        for (int i = a; i < b; i++) {
            uint32_t k = idx4(leaves[i].o);
            cmask5[k >> 6] |= 1ull << (k & 63);
        }
        wr_bytes(w, cmask5, sizeof cmask5);
        wr_bytes(w, vmask5, sizeof vmask5);
        wr_compressed_values(w, tile_vals, 32768, vmask5, cmask5, g->background, nc, half, zip);
        for (int i = a; i < b;) {
            int j = i;
            while (j < b && idx4(leaves[j].o) == idx4(leaves[i].o)) j++;
            memset(cmask4, 0, sizeof cmask4);
            memset(vmask4, 0, sizeof vmask4);
            for (int q = i; q < j; q++) {
                uint32_t k = idx_leaf(leaves[q].o);
                cmask4[k >> 6] |= 1ull << (k & 63);
            }
            wr_bytes(w, cmask4, sizeof cmask4);
            wr_bytes(w, vmask4, sizeof vmask4);
            wr_compressed_values(w, tile_vals, 4096, vmask4, cmask4, g->background, nc, half, zip);
            for (int q = i; q < j; q++) {
                float vals[VG_LEAF_VOXELS * 3];
                uint64_t mask[8];
                leaf_to_vdb(&g->leaves[leaves[q].src], nc, vals, mask);
                wr_bytes(w, mask, sizeof mask);
            }
            i = j;
        }
        a = b;
    }
    free(tile_vals);
    int64_t block_pos = (int64_t)w->n;
    for (int q = 0; q < nl; q++) {
        float vals[VG_LEAF_VOXELS * 3];
        uint64_t mask[8];
        leaf_to_vdb(&g->leaves[leaves[q].src], nc, vals, mask);
        wr_bytes(w, mask, sizeof mask);
        wr_compressed_values(w, vals, VG_LEAF_VOXELS, mask, NULL, g->background, nc, half, zip);
    }
    int64_t end_pos = (int64_t)w->n;
    if (!w->bad) {
        memcpy(w->p + pos_at, &grid_pos, 8);
        memcpy(w->p + pos_at + 8, &block_pos, 8);
        memcpy(w->p + pos_at + 16, &end_pos, 8);
    }
    free(leaves);
}

bool vdb_write(const char* path, const VolumeGridSet* set, const VdbWriteOptions* opt, char* err, int cap) {
    if (err && cap) err[0] = 0;
    Wr w;
    memset(&w, 0, sizeof w);
    wr_i64(&w, VDB_MAGIC);
    wr_u32(&w, VDB_FILE_VERSION);
    wr_u32(&w, 10), wr_u32(&w, 0); // library version
    wr_u8(&w, 1);                  // has grid offsets
    char uuid[37];
    make_uuid(uuid, set);
    wr_bytes(&w, uuid, 36);
    wr_u32(&w, 1);
    wr_meta_string(&w, "creator", "Tlender");
    wr_i32(&w, set->count);
    for (int i = 0; i < set->count; i++) wr_grid(&w, &set->grids[i], opt);
    if (w.bad) {
        free(w.p);
        set_err(err, cap, "Sem memória");
        return false;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) {
        free(w.p);
        set_err(err, cap, "Não foi possível gravar o arquivo");
        return false;
    }
    bool ok = fwrite(w.p, 1, w.n, f) == w.n;
    ok &= fclose(f) == 0;
    free(w.p);
    if (!ok || !(remove(path), re_move_path(tmp, path))) {
        remove(tmp);
        set_err(err, cap, "Não foi possível gravar o arquivo");
        return false;
    }
    return true;
}

void vdb_sequence_path(const char* first, int frame, char* out, int cap) {
    // The last run of digits before the extension becomes the frame (same width).
    int n = (int)strlen(first), end = -1, start = -1;
    for (int i = n - 1; i >= 0; i--) {
        if (first[i] == '/' || first[i] == '\\') break;
        if (first[i] >= '0' && first[i] <= '9') {
            if (end < 0) end = i;
            start = i;
        } else if (end >= 0) {
            break;
        }
    }
    if (end < 0) {
        snprintf(out, (size_t)cap, "%s", first);
        return;
    }
    int width = end - start + 1;
    snprintf(out, (size_t)cap, "%.*s%0*d%s", start, first, width, frame, first + end + 1);
}
