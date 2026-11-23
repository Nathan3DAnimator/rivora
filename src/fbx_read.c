// FBX documents (copied from the Tlender's io_fbx_read.c and adapted): the binary format (7.x, 32- and 64-bit records, zlib arrays) and the ASCII one, parsed into a tree of
// nodes with typed properties. Arrays stay in the file until read (fbx_array_*), so a big file costs little to parse.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "fbx_read.h"
#include "platform.h"
#include "re_zlib.h"

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

// --- Arena ---------------------------------------------------------------------------------------------------------------

struct FbxChunk {
    FbxChunk* next;
    size_t used, cap;
    double data[]; // aligned for doubles
};

static void* arena_alloc(FbxDoc* d, size_t n) {
    n = (n + 7) & ~(size_t)7;
    FbxChunk* c = d->arena;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        FbxChunk* nc = malloc(sizeof *nc + cap);
        if (!nc) return NULL;
        nc->next = c;
        nc->used = 0;
        nc->cap = cap;
        d->arena = nc;
        c = nc;
    }
    void* p = (uint8_t*)c->data + c->used;
    c->used += n;
    return p;
}

static char* arena_str(FbxDoc* d, const char* s, size_t n) {
    char* r = arena_alloc(d, n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

void fbx_doc_free(FbxDoc* d) {
    FbxChunk* c = d->arena;
    while (c) {
        FbxChunk* n = c->next;
        free(c);
        c = n;
    }
    free(d->data);
    memset(d, 0, sizeof *d);
}

// --- Binary ---------------------------------------------------------------------------------------------------------

static const char BIN_MAGIC[] = "Kaydara FBX Binary  ";

typedef struct BinCtx {
    FbxDoc* d;
    const uint8_t* buf;
    size_t size;
    bool wide; // 7500+: 64-bit record fields
    bool bad, oom;
} BinCtx;

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int array_elem_size(char t) {
    switch (t) {
    case 'f': case 'i': return 4;
    case 'd': case 'l': return 8;
    case 'b': return 1;
    default: return 0;
    }
}

// One property at *pos (before `end`); false when it does not fit.
static bool bin_prop(BinCtx* c, size_t* pos, size_t end, FbxProp* p) {
    const uint8_t* b = c->buf;
    size_t at = *pos;
    if (at >= end) return false;
    memset(p, 0, sizeof *p);
    p->type = (char)b[at++];
    size_t need;
    switch (p->type) {
    case 'Y': need = 2; break;
    case 'C': need = 1; break;
    case 'I': case 'F': need = 4; break;
    case 'D': case 'L': need = 8; break;
    case 'S': case 'R': need = 4; break;
    case 'f': case 'd': case 'l': case 'i': case 'b': need = 12; break;
    default: return false;
    }
    if (end - at < need) return false;
    const uint8_t* q = b + at;
    switch (p->type) {
    case 'Y': { int16_t v; memcpy(&v, q, 2); p->i = v; break; }
    case 'C': p->i = q[0]; break;
    case 'I': p->i = (int32_t)rd32(q); break;
    case 'F': { float v; memcpy(&v, q, 4); p->d = v; break; }
    case 'D': memcpy(&p->d, q, 8); break;
    case 'L': p->i = (int64_t)rd64(q); break;
    case 'S': case 'R': {
        uint32_t len = rd32(q);
        at += 4;
        if (end - at < len) return false;
        p->count = len;
        if (p->type == 'S') {
            p->p = (const uint8_t*)arena_str(c->d, (const char*)b + at, len);
            if (!p->p) c->oom = true;
        } else {
            p->p = b + at;
        }
        *pos = at + len;
        return !c->oom;
    }
    default: { // arrays
        uint32_t count = rd32(q), enc = rd32(q + 4), clen = rd32(q + 8);
        at += 12;
        if (end - at < clen || enc > 1) return false;
        if (enc == 0 && (uint64_t)count * (uint64_t)array_elem_size(p->type) > clen) return false;
        p->count = count;
        p->encoding = (uint8_t)enc;
        p->comp_len = clen;
        p->p = b + at;
        *pos = at + clen;
        return true;
    }
    }
    *pos = at + need;
    return true;
}

// The node record at *pos, or NULL for the null record ending a list (*null set) or an error (c->bad).
static FbxNode* bin_node(BinCtx* c, size_t* pos, size_t limit, int depth, bool* null) {
    const uint8_t* b = c->buf;
    size_t hdr = c->wide ? 25 : 13;
    *null = false;
    if (*pos + hdr > limit || depth > 64) {
        c->bad = true;
        return NULL;
    }
    const uint8_t* q = b + *pos;
    uint64_t end, nprops, plen;
    if (c->wide) {
        end = rd64(q);
        nprops = rd64(q + 8);
        plen = rd64(q + 16);
    } else {
        end = rd32(q);
        nprops = rd32(q + 4);
        plen = rd32(q + 8);
    }
    uint8_t name_len = q[hdr - 1];
    if (end == 0 && nprops == 0 && plen == 0 && name_len == 0) {
        *pos += hdr;
        *null = true;
        return NULL;
    }
    size_t at = *pos + hdr;
    if (end > limit || end < at + name_len || nprops > plen || at + name_len + plen > end) {
        c->bad = true;
        return NULL;
    }
    FbxNode* n = arena_alloc(c->d, sizeof *n);
    if (!n) {
        c->oom = true;
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->name = arena_str(c->d, (const char*)b + at, name_len);
    at += name_len;
    if (nprops) {
        n->props = arena_alloc(c->d, sizeof *n->props * (size_t)nprops);
        if (!n->props || !n->name) {
            c->oom = true;
            return NULL;
        }
        size_t pend = at + (size_t)plen;
        for (uint64_t i = 0; i < nprops; i++)
            if (!bin_prop(c, &at, pend, &n->props[i])) {
                c->bad = !c->oom;
                return NULL;
            }
        n->nprops = (uint32_t)nprops;
        at = pend;
    }
    FbxNode** tail = &n->child;
    while (at < end) {
        bool is_null;
        FbxNode* ch = bin_node(c, &at, (size_t)end, depth + 1, &is_null);
        if (is_null) break;
        if (!ch) return NULL;
        *tail = ch;
        tail = &ch->next;
    }
    *pos = (size_t)end;
    return n;
}

static bool parse_binary(FbxDoc* d, char* err, size_t err_cap) {
    BinCtx c = { d, d->data, d->size, false, false, false };
    if (d->size < 27) {
        set_err(err, err_cap, "The FBX file is invalid or damaged.");
        return false;
    }
    d->version = (int)rd32(d->data + 23);
    c.wide = d->version >= 7500;
    size_t pos = 27;
    FbxNode** tail = &d->root.child;
    while (pos < d->size) {
        bool is_null;
        FbxNode* n = bin_node(&c, &pos, d->size, 0, &is_null);
        if (is_null || !n) break;
        *tail = n;
        tail = &n->next;
    }
    if (c.oom) {
        set_err(err, err_cap, "Out of memory.");
        return false;
    }
    if (c.bad || !d->root.child) {
        set_err(err, err_cap, "The FBX file is invalid or damaged.");
        return false;
    }
    return true;
}

// --- ASCII ----------------------------------------------------------------------------------------------------------

typedef struct AscCtx {
    FbxDoc* d;
    const char* p;
    const char* end;
    bool bad, oom;
} AscCtx;

static void asc_ws(AscCtx* c, bool newlines) {
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ';') {
            while (c->p < c->end && *c->p != '\n') c->p++;
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || (newlines && ch == '\n')) {
            c->p++;
        } else {
            break;
        }
    }
}

static bool is_ident(char ch) { return isalnum((unsigned char)ch) || ch == '_' || ch == '|' || ch == '-' || ch == '.'; }

// A number token: integers fast, the rest by strtod. *is_int tells which.
static double asc_number(AscCtx* c, bool* is_int) {
    const char* s = c->p;
    bool neg = false;
    if (s < c->end && (*s == '-' || *s == '+')) neg = *s++ == '-';
    int64_t v = 0;
    const char* digits = s;
    while (s < c->end && *s >= '0' && *s <= '9' && s - digits < 18) v = v * 10 + (*s++ - '0');
    if (s > digits && (s >= c->end || (*s != '.' && *s != 'e' && *s != 'E' && !(*s >= '0' && *s <= '9')))) {
        *is_int = true;
        c->p = s;
        return (double)(neg ? -v : v);
    }
    *is_int = false;
    char tmp[64];
    size_t n = 0;
    s = c->p;
    while (s < c->end && n < sizeof tmp - 1 && (isdigit((unsigned char)*s) || *s == '.' || *s == 'e' || *s == 'E' ||
                                                 *s == '-' || *s == '+' || *s == 'i' || *s == 'n' || *s == 'f' ||
                                                 *s == 'a' || *s == 'I' || *s == 'N' || *s == 'F' || *s == 'A'))
        tmp[n++] = *s++;
    tmp[n] = '\0';
    char* e;
    double r = strtod(tmp, &e);
    if (e == tmp) {
        c->bad = true;
        return 0.0;
    }
    c->p += e - tmp;
    return r;
}

static bool asc_prop(AscCtx* c, FbxProp* p) {
    memset(p, 0, sizeof *p);
    char ch = *c->p;
    if (ch == '"') {
        const char* s = ++c->p;
        while (c->p < c->end && *c->p != '"') c->p++;
        if (c->p >= c->end) return false;
        size_t n = (size_t)(c->p - s);
        char* out = arena_alloc(c->d, n + 1);
        if (!out) {
            c->oom = true;
            return false;
        }
        size_t k = 0;
        for (size_t i = 0; i < n; i++) { // &quot; is how the SDK writes a quote inside a string
            if (s[i] == '&' && i + 6 <= n && memcmp(s + i, "&quot;", 6) == 0) {
                out[k++] = '"';
                i += 5;
            } else {
                out[k++] = s[i];
            }
        }
        out[k] = '\0';
        c->p++;
        p->type = 'S';
        p->count = (uint32_t)k;
        p->p = (const uint8_t*)out;
        return true;
    }
    if (ch == '*') { // array: *N { a: v,v,v }
        c->p++;
        bool is_int;
        double nd = asc_number(c, &is_int);
        if (c->bad || nd < 0 || nd > 4e9) return false;
        size_t n = (size_t)nd;
        asc_ws(c, true);
        if (c->p >= c->end || *c->p != '{') return false;
        c->p++;
        asc_ws(c, true);
        if (c->p < c->end && *c->p == 'a') {
            c->p++;
            asc_ws(c, true);
            if (c->p < c->end && *c->p == ':') c->p++;
        }
        double* v = n ? arena_alloc(c->d, n * sizeof(double)) : NULL;
        if (n && !v) {
            c->oom = true;
            return false;
        }
        size_t got = 0;
        for (;;) {
            asc_ws(c, true);
            if (c->p >= c->end) return false;
            if (*c->p == '}') {
                c->p++;
                break;
            }
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            double x = asc_number(c, &is_int);
            if (c->bad) return false;
            if (got < n) v[got] = x;
            got++;
        }
        p->type = 'd';
        p->count = (uint32_t)(got < n ? got : n);
        p->comp_len = p->count * 8u;
        p->p = (const uint8_t*)v;
        return true;
    }
    if (ch == '-' || ch == '+' || ch == '.' || isdigit((unsigned char)ch)) {
        bool is_int;
        double x = asc_number(c, &is_int);
        if (c->bad) return false;
        if (is_int) {
            p->type = 'L';
            p->i = (int64_t)x;
        } else {
            p->type = 'D';
            p->d = x;
        }
        return true;
    }
    if (isalpha((unsigned char)ch)) { // bare letters: Shading: T, a flag char
        p->type = 'C';
        p->i = ch;
        while (c->p < c->end && isalnum((unsigned char)*c->p)) c->p++;
        return true;
    }
    return false;
}

static FbxNode* asc_node(AscCtx* c, int depth);

// Children until '}' (or the end of the file at the top level).
static FbxNode* asc_children(AscCtx* c, int depth, bool top) {
    FbxNode* first = NULL;
    FbxNode** tail = &first;
    for (;;) {
        asc_ws(c, true);
        if (c->p >= c->end) {
            if (!top) c->bad = true;
            break;
        }
        if (*c->p == '}') {
            if (top) c->bad = true;
            c->p++;
            break;
        }
        FbxNode* n = asc_node(c, depth);
        if (!n) break;
        *tail = n;
        tail = &n->next;
    }
    return first;
}

static FbxNode* asc_node(AscCtx* c, int depth) {
    if (depth > 64) {
        c->bad = true;
        return NULL;
    }
    const char* s = c->p;
    while (c->p < c->end && is_ident(*c->p)) c->p++;
    if (c->p == s || c->p >= c->end || *c->p != ':') {
        c->bad = true;
        return NULL;
    }
    FbxNode* n = arena_alloc(c->d, sizeof *n);
    if (!n) {
        c->oom = true;
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->name = arena_str(c->d, s, (size_t)(c->p - s));
    c->p++;
    // Properties until the end of the line or '{' (a line ending with ',' goes on)
    FbxProp tmp[64];
    FbxProp* props = tmp;
    uint32_t np = 0, cap = 64;
    bool heap = false;
    for (;;) {
        asc_ws(c, false);
        if (c->p >= c->end || *c->p == '\n' || *c->p == '{' || *c->p == '}') break;
        if (*c->p == ',') {
            c->p++;
            asc_ws(c, false);
            if (c->p < c->end && *c->p == '\r') c->p++;
            if (c->p < c->end && *c->p == '\n') c->p++;
            continue;
        }
        if (np == cap) {
            FbxProp* nprops = malloc(sizeof *nprops * cap * 2);
            if (!nprops) {
                c->oom = true;
                break;
            }
            memcpy(nprops, props, sizeof *props * np);
            if (heap) free(props);
            props = nprops;
            heap = true;
            cap *= 2;
        }
        if (!asc_prop(c, &props[np])) {
            if (!c->oom) c->bad = true;
            break;
        }
        np++;
    }
    if (np && !c->bad && !c->oom) {
        n->props = arena_alloc(c->d, sizeof *n->props * np);
        if (n->props) {
            memcpy(n->props, props, sizeof *props * np);
            n->nprops = np;
        } else {
            c->oom = true;
        }
    }
    if (heap) free(props);
    if (c->bad || c->oom || !n->name) return NULL;
    if (c->p < c->end && *c->p == '{') {
        c->p++;
        n->child = asc_children(c, depth + 1, false);
        if (c->bad || c->oom) return NULL;
    }
    return n;
}

static bool parse_ascii(FbxDoc* d, char* err, size_t err_cap) {
    AscCtx c = { d, (const char*)d->data, (const char*)d->data + d->size, false, false };
    if (d->size >= 3 && memcmp(d->data, "\xEF\xBB\xBF", 3) == 0) c.p += 3;
    d->root.child = asc_children(&c, 0, true);
    if (c.oom) {
        set_err(err, err_cap, "Out of memory.");
        return false;
    }
    if (c.bad || !d->root.child) {
        set_err(err, err_cap, "The FBX file is invalid or damaged.");
        return false;
    }
    const FbxNode* hdr = fbx_find(&d->root, "FBXHeaderExtension");
    d->version = (int)fbx_child_int(hdr, "FBXVersion", 7400);
    return true;
}

bool fbx_doc_parse(FbxDoc* d, uint8_t* data, size_t size, char* err, size_t err_cap) {
    memset(d, 0, sizeof *d);
    d->data = data;
    d->size = size;
    d->root.name = "";
    if (size >= 23 && memcmp(data, BIN_MAGIC, 20) == 0 && data[20] == 0 && data[21] == 0x1a) {
        d->ascii = false;
        return parse_binary(d, err, err_cap);
    }
    d->ascii = true;
    // An ASCII file starts with comments and "FBXHeaderExtension:" (or another node): text, no NUL bytes up front
    size_t check = size < 4096 ? size : 4096;
    for (size_t i = 0; i < check; i++)
        if (data[i] == 0) {
            set_err(err, err_cap, "Not an FBX file.");
            return false;
        }
    return parse_ascii(d, err, err_cap);
}

bool fbx_doc_load(FbxDoc* d, const char* path, char* err, size_t err_cap) {
    memset(d, 0, sizeof *d);
    FILE* f = re_fopen(path, "rb");
    if (!f) {
        set_err(err, err_cap, "Could not open the FBX file.");
        return false;
    }
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint8_t* data = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!data) {
        fclose(f);
        set_err(err, err_cap, size > 0 ? "Out of memory." : "The FBX file is invalid or damaged.");
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        set_err(err, err_cap, "Could not read the FBX file.");
        return false;
    }
    data[size] = 0;
    if (!fbx_doc_parse(d, data, (size_t)size, err, err_cap)) {
        fbx_doc_free(d);
        return false;
    }
    return true;
}

// --- Access ---------------------------------------------------------------------------------------------------------

const FbxNode* fbx_find(const FbxNode* n, const char* name) {
    if (!n) return NULL;
    for (const FbxNode* c = n->child; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

const FbxNode* fbx_find_next(const FbxNode* n, const char* name) {
    for (const FbxNode* c = n ? n->next : NULL; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

int64_t fbx_prop_int(const FbxProp* p, int64_t def) {
    if (!p) return def;
    switch (p->type) {
    case 'Y': case 'C': case 'I': case 'L': return p->i;
    case 'F': case 'D': return (int64_t)p->d;
    default: return def;
    }
}

double fbx_prop_num(const FbxProp* p, double def) {
    if (!p) return def;
    switch (p->type) {
    case 'Y': case 'C': case 'I': case 'L': return (double)p->i;
    case 'F': case 'D': return p->d;
    default: return def;
    }
}

const char* fbx_prop_str(const FbxProp* p) { return p && p->type == 'S' && p->p ? (const char*)p->p : ""; }

int64_t fbx_child_int(const FbxNode* n, const char* name, int64_t def) { return fbx_prop_int(fbx_prop(fbx_find(n, name), 0), def); }
double fbx_child_num(const FbxNode* n, const char* name, double def) { return fbx_prop_num(fbx_prop(fbx_find(n, name), 0), def); }
const char* fbx_child_str(const FbxNode* n, const char* name) { return fbx_prop_str(fbx_prop(fbx_find(n, name), 0)); }

bool fbx_is_array(const FbxProp* p) { return p && array_elem_size(p->type) > 0; }

// The raw elements of an array (decompressed into *tmp when needed), NULL on error.
static const uint8_t* array_bytes(const FbxProp* p, uint8_t** tmp) {
    *tmp = NULL;
    size_t es = (size_t)array_elem_size(p->type), bytes = (size_t)p->count * es;
    if (!es || !p->count) return NULL;
    if (p->encoding == 0) return p->p;
    uint8_t* out = malloc(bytes);
    if (!out) return NULL;
    if (!re_inflate(p->p, p->comp_len, out, bytes)) {
        free(out);
        return NULL;
    }
    *tmp = out;
    return out;
}

#define ARRAY_CONVERT(T, out, raw, type, n)                                                         \
    do {                                                                                            \
        switch (type) {                                                                             \
        case 'f': for (size_t k = 0; k < n; k++) { float v; memcpy(&v, raw + k * 4, 4); out[k] = (T)v; } break;   \
        case 'd': for (size_t k = 0; k < n; k++) { double v; memcpy(&v, raw + k * 8, 8); out[k] = (T)v; } break; \
        case 'i': for (size_t k = 0; k < n; k++) { int32_t v; memcpy(&v, raw + k * 4, 4); out[k] = (T)v; } break; \
        case 'l': for (size_t k = 0; k < n; k++) { int64_t v; memcpy(&v, raw + k * 8, 8); out[k] = (T)v; } break; \
        case 'b': for (size_t k = 0; k < n; k++) out[k] = (T)raw[k]; break;                         \
        }                                                                                           \
    } while (0)

double* fbx_array_f64(const FbxProp* p, size_t* count) {
    *count = 0;
    if (!fbx_is_array(p)) return NULL;
    uint8_t* tmp;
    const uint8_t* raw = array_bytes(p, &tmp);
    if (!raw) return NULL;
    size_t n = p->count;
    double* out = malloc(sizeof *out * n);
    if (out) {
        if (p->type == 'd') memcpy(out, raw, n * 8);
        else ARRAY_CONVERT(double, out, raw, p->type, n);
        *count = n;
    }
    free(tmp);
    return out;
}

int32_t* fbx_array_i32(const FbxProp* p, size_t* count) {
    *count = 0;
    if (!fbx_is_array(p)) return NULL;
    uint8_t* tmp;
    const uint8_t* raw = array_bytes(p, &tmp);
    if (!raw) return NULL;
    size_t n = p->count;
    int32_t* out = malloc(sizeof *out * n);
    if (out) {
        if (p->type == 'i') memcpy(out, raw, n * 4);
        else ARRAY_CONVERT(int32_t, out, raw, p->type, n);
        *count = n;
    }
    free(tmp);
    return out;
}

int64_t* fbx_array_i64(const FbxProp* p, size_t* count) {
    *count = 0;
    if (!fbx_is_array(p)) return NULL;
    uint8_t* tmp;
    const uint8_t* raw = array_bytes(p, &tmp);
    if (!raw) return NULL;
    size_t n = p->count;
    int64_t* out = malloc(sizeof *out * n);
    if (out) {
        if (p->type == 'l') memcpy(out, raw, n * 8);
        else ARRAY_CONVERT(int64_t, out, raw, p->type, n);
        *count = n;
    }
    free(tmp);
    return out;
}

void fbx_name_class(const FbxProp* p, char* name, size_t cap, char* cls, size_t cls_cap) {
    const char* s = fbx_prop_str(p);
    size_t len = p && p->type == 'S' ? p->count : 0;
    size_t first = strlen(s);
    if (cls && cls_cap) cls[0] = '\0';
    if (first + 2 <= len && s[first + 1] == '\x01') { // binary: name \0 \1 class
        snprintf(name, cap, "%s", s);
        if (cls && cls_cap) snprintf(cls, cls_cap, "%s", s + first + 2);
        return;
    }
    const char* sep = strstr(s, "::");
    if (sep) { // ASCII: class::name
        snprintf(name, cap, "%s", sep + 2);
        if (cls && cls_cap) snprintf(cls, cls_cap, "%.*s", (int)(sep - s), s);
        return;
    }
    snprintf(name, cap, "%s", s);
}
