// A binary FBX 7.4 writer (fbx_write.h). A node record (7.4: 32-bit fields): its end offset, its property count, the
// properties' byte length, its name, the properties, its children and a 13-byte null record when it has children or no
// properties (the FBX SDK's own rule). The file: "Kaydara FBX Binary  \0\x1a\0" + version, the top nodes, a null
// record, then the footer (an id, zeros to 16 bytes, the version, 120 zeros, the closing magic) - Blender's io_scene_fbx
// writes these same bytes and the FBX SDK (Maya) reads its files
#include "fbx_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define FBXW_VERSION 7400
#define FBXW_DEPTH 32

struct FbxW {
    uint8_t* b;
    size_t n, cap;
    bool oom;
    int depth;
    struct {
        size_t head;       // the record's start (end offset, count, list length)
        size_t props;      // its first property's
        uint32_t nprops;
        bool closed;       // its property list closed (a child began, or it ended)
        bool child;        // a child written
    } st[FBXW_DEPTH];
};

static void put(FbxW* w, const void* p, size_t n) {
    if (w->oom) return;
    if (w->n + n > w->cap) {
        size_t c = w->cap ? w->cap * 2 : 1 << 16;
        while (c < w->n + n) c *= 2;
        uint8_t* nb = realloc(w->b, c);
        if (!nb) {
            w->oom = true;
            return;
        }
        w->b = nb, w->cap = c;
    }
    memcpy(w->b + w->n, p, n), w->n += n;
}
static void put_u32(FbxW* w, uint32_t v) { put(w, &v, 4); }
static void patch_u32(FbxW* w, size_t at, uint32_t v) {
    if (!w->oom) memcpy(w->b + at, &v, 4);
}

FbxW* fbxw_new(void) {
    FbxW* w = calloc(1, sizeof(FbxW));
    if (!w) return NULL;
    // the header first: the records' end offsets count from the file's start
    static const char head[23] = "Kaydara FBX Binary  \0\x1a";
    uint32_t ver = FBXW_VERSION;
    put(w, head, 23), put_u32(w, ver); // (the 20 letters, 0, 0x1a, 0: 23 bytes)
    return w;
}
void fbxw_free(FbxW* w) {
    if (w) free(w->b), free(w);
}

// the open node's property list closed (its length known) before a child or its end
static void close_props(FbxW* w) {
    if (w->depth <= 0) return;
    int d = w->depth - 1;
    if (w->st[d].closed) return;
    w->st[d].closed = true;
    patch_u32(w, w->st[d].head + 4, w->st[d].nprops);
    patch_u32(w, w->st[d].head + 8, (uint32_t)(w->n - w->st[d].props));
}

void fbxw_open(FbxW* w, const char* name) {
    if (w->depth > 0) close_props(w), w->st[w->depth - 1].child = true;
    if (w->depth >= FBXW_DEPTH) {
        w->oom = true;
        return;
    }
    int d = w->depth++;
    w->st[d].head = w->n;
    w->st[d].nprops = 0;
    w->st[d].closed = w->st[d].child = false;
    put_u32(w, 0), put_u32(w, 0), put_u32(w, 0);
    uint8_t ln = (uint8_t)strlen(name);
    put(w, &ln, 1), put(w, name, ln);
    w->st[d].props = w->n;
}

void fbxw_close(FbxW* w) {
    if (w->depth <= 0) return;
    close_props(w);
    int d = w->depth - 1;
    if (w->st[d].child || w->st[d].nprops == 0) {
        static const uint8_t zero[13] = { 0 };
        put(w, zero, 13);
    }
    patch_u32(w, w->st[d].head, (uint32_t)w->n);
    w->depth--;
}

void fbxw_block(FbxW* w) {
    if (w->depth > 0) close_props(w), w->st[w->depth - 1].child = true;
}

static void prop_begin(FbxW* w, char type) {
    if (w->depth <= 0) return;
    w->st[w->depth - 1].nprops++;
    put(w, &type, 1);
}
void fbxw_i(FbxW* w, int32_t v) { prop_begin(w, 'I'), put(w, &v, 4); }
void fbxw_l(FbxW* w, int64_t v) { prop_begin(w, 'L'), put(w, &v, 8); }
void fbxw_d(FbxW* w, double v) { prop_begin(w, 'D'), put(w, &v, 8); }
void fbxw_c(FbxW* w, uint8_t v) { prop_begin(w, 'C'), put(w, &v, 1); }
void fbxw_sn(FbxW* w, const char* s, uint32_t n) { prop_begin(w, 'S'), put_u32(w, n), put(w, s, n); }
void fbxw_s(FbxW* w, const char* s) { fbxw_sn(w, s, (uint32_t)strlen(s)); }
void fbxw_raw(FbxW* w, const void* p, uint32_t n) { prop_begin(w, 'R'), put_u32(w, n), put(w, p, n); }
static void arr(FbxW* w, char type, const void* v, uint32_t n, uint32_t size) {
    prop_begin(w, type);
    put_u32(w, n), put_u32(w, 0), put_u32(w, n * size); // (no compression)
    put(w, v, (size_t)n * size);
}
void fbxw_ad(FbxW* w, const double* v, uint32_t n) { arr(w, 'd', v, n, 8); }
void fbxw_af(FbxW* w, const float* v, uint32_t n) { arr(w, 'f', v, n, 4); }
void fbxw_ai(FbxW* w, const int32_t* v, uint32_t n) { arr(w, 'i', v, n, 4); }
void fbxw_al(FbxW* w, const int64_t* v, uint32_t n) { arr(w, 'l', v, n, 8); }

void fbxw_objname(FbxW* w, const char* name, const char* cls) {
    char b[256];
    size_t a = strlen(name), c = strlen(cls);
    if (a + c + 2 > sizeof b) a = sizeof b - c - 2;
    memcpy(b, name, a), b[a] = 0, b[a + 1] = 1, memcpy(b + a + 2, cls, c);
    fbxw_sn(w, b, (uint32_t)(a + 2 + c));
}

void fbxw_node_i(FbxW* w, const char* name, int32_t v) { fbxw_open(w, name), fbxw_i(w, v), fbxw_close(w); }
void fbxw_node_s(FbxW* w, const char* name, const char* v) { fbxw_open(w, name), fbxw_s(w, v), fbxw_close(w); }
void fbxw_node_d(FbxW* w, const char* name, double v) { fbxw_open(w, name), fbxw_d(w, v), fbxw_close(w); }

static void p_head(FbxW* w, const char* name, const char* type, const char* label, const char* flags) {
    fbxw_open(w, "P");
    fbxw_s(w, name), fbxw_s(w, type), fbxw_s(w, label), fbxw_s(w, flags);
}
void fbxw_p_int(FbxW* w, const char* name, int32_t v) { p_head(w, name, "int", "Integer", ""), fbxw_i(w, v), fbxw_close(w); }
void fbxw_p_enum(FbxW* w, const char* name, int32_t v) { p_head(w, name, "enum", "", ""), fbxw_i(w, v), fbxw_close(w); }
void fbxw_p_double(FbxW* w, const char* name, double v) { p_head(w, name, "double", "Number", ""), fbxw_d(w, v), fbxw_close(w); }
void fbxw_p_ktime(FbxW* w, const char* name, int64_t v) { p_head(w, name, "KTime", "Time", ""), fbxw_l(w, v), fbxw_close(w); }
void fbxw_p_str(FbxW* w, const char* name, const char* v) { p_head(w, name, "KString", "", ""), fbxw_s(w, v), fbxw_close(w); }
void fbxw_p_vec(FbxW* w, const char* name, const char* type, const char* label, const char* flags, double x, double y, double z) {
    p_head(w, name, type, label, flags), fbxw_d(w, x), fbxw_d(w, y), fbxw_d(w, z), fbxw_close(w);
}

bool fbxw_save(FbxW* w, const char* path) {
    if (w->depth != 0 || w->oom) return false;
    static const uint8_t foot_id[16] = { 0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e };
    static const uint8_t magic[16] = { 0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b };
    static const uint8_t zero[128] = { 0 };
    size_t body = w->n;
    put(w, zero, 13); // (the top list's null record)
    put(w, foot_id, 16);
    size_t pad = ((w->n + 15) & ~(size_t)15) - w->n;
    put(w, zero, pad ? pad : 16);
    put_u32(w, FBXW_VERSION);
    put(w, zero, 120);
    put(w, magic, 16);
    bool ok = !w->oom;
    FILE* fp = ok ? re_fopen(path, "wb") : NULL;
    ok = fp && fwrite(w->b, 1, w->n, fp) == w->n;
    if (fp) ok &= fclose(fp) == 0;
    w->n = body; // (saved again later: the same body)
    return ok;
}
