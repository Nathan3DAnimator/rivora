// JSON parser (DOM in an arena) and compact writer (io_json.h).
#include "io_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Arena -----------------------------------------------------------------------------------

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t used, cap;
    // data follows (16-byte aligned: the header is 32 bytes on 64-bit)
} ArenaBlock;

static void* arena_alloc(void** arena, size_t size) {
    size = (size + 15) & ~(size_t)15;
    ArenaBlock* b = *arena;
    if (!b || b->used + size > b->cap) {
        size_t cap = size > 65536 ? size : 65536;
        if (b && b->cap * 2 > cap && b->cap < (64u << 20)) cap = b->cap * 2 > size ? b->cap * 2 : size;
        ArenaBlock* nb = malloc(sizeof(ArenaBlock) + 16 + cap);
        if (!nb) return NULL;
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        *arena = nb;
        b = nb;
    }
    char* base = (char*)(((uintptr_t)(b + 1) + 15) & ~(uintptr_t)15);
    void* p = base + b->used;
    b->used += size;
    return p;
}

static void arena_free(void* arena) {
    ArenaBlock* b = arena;
    while (b) {
        ArenaBlock* n = b->next;
        free(b);
        b = n;
    }
}

// --- Parser ----------------------------------------------------------------------------------

typedef struct Parser {
    const char* p;
    const char* end;
    const char* start;
    void* arena;
    // Scratch stacks for the containers being read (their children are copied to the arena when they close)
    JsonValue* vals;
    size_t nvals, capvals;
    JsonMember* mems;
    size_t nmems, capmems;
    char* err;
    size_t err_cap;
    bool failed;
    int depth;
} Parser;

static void perr(Parser* ps, const char* msg) {
    if (ps->failed) return;
    ps->failed = true;
    int line = 1;
    for (const char* c = ps->start; c < ps->p && c < ps->end; c++) line += *c == '\n';
    if (ps->err && ps->err_cap) snprintf(ps->err, ps->err_cap, "JSON: %s (line %d)", msg, line);
}

static void skip_ws(Parser* ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t')) ps->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(Parser* ps, uint32_t* out) {
    if (ps->end - ps->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0) return false;
        v = v << 4 | (uint32_t)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static char* put_utf8(char* o, uint32_t cp) {
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) {
        *o++ = (char)(0xC0 | cp >> 6);
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | cp >> 12);
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | cp >> 18);
        *o++ = (char)(0x80 | (cp >> 12 & 0x3F));
        *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

// After the opening quote. The unescaped text is never longer than the escaped one.
static bool parse_string(Parser* ps, const char** out, uint32_t* out_len) {
    const char* s = ps->p;
    const char* q = s;
    bool plain = true;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            plain = false;
            q++;
        } else if ((unsigned char)*q < 0x20) {
            ps->p = q;
            perr(ps, "control character in a string");
            return false;
        }
        q++;
    }
    if (q >= ps->end) {
        perr(ps, "unterminated string");
        return false;
    }
    char* dst = arena_alloc(&ps->arena, (size_t)(q - s) + 1);
    if (!dst) {
        perr(ps, "out of memory");
        return false;
    }
    if (plain) {
        memcpy(dst, s, (size_t)(q - s));
        dst[q - s] = '\0';
        *out = dst;
        *out_len = (uint32_t)(q - s);
        ps->p = q + 1;
        return true;
    }
    char* o = dst;
    while (ps->p < q) {
        char c = *ps->p++;
        if (c != '\\') {
            *o++ = c;
            continue;
        }
        char e = *ps->p++;
        switch (e) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(ps, &cp)) {
                perr(ps, "bad \\u escape");
                return false;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->p + 6 <= q && ps->p[0] == '\\' && ps->p[1] == 'u') {
                const char* save = ps->p;
                ps->p += 2;
                uint32_t lo;
                if (read_hex4(ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else ps->p = save;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // lone surrogate
            o = put_utf8(o, cp);
            break;
        }
        default:
            perr(ps, "bad escape");
            return false;
        }
    }
    *o = '\0';
    *out = dst;
    *out_len = (uint32_t)(o - dst);
    ps->p = q + 1;
    return true;
}

static const double POW10[23] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

static bool parse_number(Parser* ps, double* out) {
    const char* s = ps->p;
    const char* q = s;
    bool neg = false;
    if (q < ps->end && *q == '-') {
        neg = true;
        q++;
    }
    if (q >= ps->end || *q < '0' || *q > '9') {
        perr(ps, "bad number");
        return false;
    }
    uint64_t mant = 0;
    int digits = 0, exp10 = 0;
    if (*q == '0') q++;
    else
        while (q < ps->end && *q >= '0' && *q <= '9') {
            if (digits < 19) mant = mant * 10 + (uint64_t)(*q - '0'), digits += mant != 0;
            else exp10++;
            q++;
        }
    if (q < ps->end && *q == '.') {
        q++;
        if (q >= ps->end || *q < '0' || *q > '9') {
            ps->p = q;
            perr(ps, "bad number");
            return false;
        }
        while (q < ps->end && *q >= '0' && *q <= '9') {
            if (digits < 19) {
                mant = mant * 10 + (uint64_t)(*q - '0');
                digits += mant != 0;
                exp10--;
            }
            q++;
        }
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        q++;
        bool eneg = false;
        if (q < ps->end && (*q == '+' || *q == '-')) eneg = *q++ == '-';
        if (q >= ps->end || *q < '0' || *q > '9') {
            ps->p = q;
            perr(ps, "bad number");
            return false;
        }
        int e = 0;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            if (e < 100000) e = e * 10 + (*q - '0');
            q++;
        }
        exp10 += eneg ? -e : e;
    }
    double v;
    if (mant == 0) v = 0.0;
    else if (digits <= 15 && exp10 >= -22 && exp10 <= 22) {
        v = (double)mant; // exact below 2^53: one correctly rounded operation
        v = exp10 < 0 ? v / POW10[-exp10] : v * POW10[exp10];
    } else {
        char tmp[64];
        size_t n = (size_t)(q - s);
        if (n < sizeof tmp) {
            memcpy(tmp, s, n);
            tmp[n] = '\0';
            v = strtod(tmp, NULL);
            neg = false;
        } else {
            v = (double)mant * pow(10.0, exp10);
        }
    }
    *out = neg ? -v : v;
    ps->p = q;
    return true;
}

static bool parse_value(Parser* ps, JsonValue* out);

static bool push_val(Parser* ps, const JsonValue* v) {
    if (ps->nvals == ps->capvals) {
        size_t nc = ps->capvals ? ps->capvals * 2 : 256;
        JsonValue* n = realloc(ps->vals, nc * sizeof *n);
        if (!n) return false;
        ps->vals = n;
        ps->capvals = nc;
    }
    ps->vals[ps->nvals++] = *v;
    return true;
}

static bool push_mem(Parser* ps, const char* key, const JsonValue* v) {
    if (ps->nmems == ps->capmems) {
        size_t nc = ps->capmems ? ps->capmems * 2 : 256;
        JsonMember* n = realloc(ps->mems, nc * sizeof *n);
        if (!n) return false;
        ps->mems = n;
        ps->capmems = nc;
    }
    ps->mems[ps->nmems].key = key;
    ps->mems[ps->nmems].value = *v;
    ps->nmems++;
    return true;
}

static bool parse_array(Parser* ps, JsonValue* out) {
    size_t base = ps->nvals;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') ps->p++;
    else
        for (;;) {
            JsonValue v;
            if (!parse_value(ps, &v)) return false;
            if (!push_val(ps, &v)) {
                perr(ps, "out of memory");
                return false;
            }
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == ']') {
                ps->p++;
                break;
            }
            perr(ps, "expected ',' or ']'");
            return false;
        }
    size_t n = ps->nvals - base;
    out->type = JSON_ARRAY;
    out->count = (uint32_t)n;
    out->items = NULL;
    if (n) {
        out->items = arena_alloc(&ps->arena, n * sizeof(JsonValue));
        if (!out->items) {
            perr(ps, "out of memory");
            return false;
        }
        memcpy(out->items, ps->vals + base, n * sizeof(JsonValue));
    }
    ps->nvals = base;
    return true;
}

static bool parse_object(Parser* ps, JsonValue* out) {
    size_t base = ps->nmems;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') ps->p++;
    else
        for (;;) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                perr(ps, "expected a key");
                return false;
            }
            ps->p++;
            const char* key;
            uint32_t klen;
            if (!parse_string(ps, &key, &klen)) return false;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                perr(ps, "expected ':'");
                return false;
            }
            ps->p++;
            JsonValue v;
            if (!parse_value(ps, &v)) return false;
            if (!push_mem(ps, key, &v)) {
                perr(ps, "out of memory");
                return false;
            }
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == '}') {
                ps->p++;
                break;
            }
            perr(ps, "expected ',' or '}'");
            return false;
        }
    size_t n = ps->nmems - base;
    out->type = JSON_OBJECT;
    out->count = (uint32_t)n;
    out->members = NULL;
    if (n) {
        out->members = arena_alloc(&ps->arena, n * sizeof(JsonMember));
        if (!out->members) {
            perr(ps, "out of memory");
            return false;
        }
        memcpy(out->members, ps->mems + base, n * sizeof(JsonMember));
    }
    ps->nmems = base;
    return true;
}

static bool literal(Parser* ps, const char* word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        perr(ps, "bad literal");
        return false;
    }
    ps->p += n;
    return true;
}

static bool parse_value(Parser* ps, JsonValue* out) {
    skip_ws(ps);
    memset(out, 0, sizeof *out);
    if (ps->p >= ps->end) {
        perr(ps, "unexpected end");
        return false;
    }
    char c = *ps->p;
    switch (c) {
    case '{':
    case '[': {
        if (++ps->depth > 512) {
            perr(ps, "nested too deep");
            return false;
        }
        ps->p++;
        bool ok = c == '{' ? parse_object(ps, out) : parse_array(ps, out);
        ps->depth--;
        return ok;
    }
    case '"': {
        ps->p++;
        out->type = JSON_STRING;
        return parse_string(ps, &out->str, &out->count);
    }
    case 't': out->type = JSON_TRUE; return literal(ps, "true");
    case 'f': out->type = JSON_FALSE; return literal(ps, "false");
    case 'n': out->type = JSON_NULL; return literal(ps, "null");
    default:
        out->type = JSON_NUMBER;
        return parse_number(ps, &out->num);
    }
}

bool json_parse(const char* text, size_t len, JsonDoc* doc, char* err, size_t err_cap) {
    memset(doc, 0, sizeof *doc);
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = ps.start = text;
    ps.end = text + len;
    ps.err = err;
    ps.err_cap = err_cap;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    bool ok = parse_value(&ps, &doc->root);
    if (ok) {
        skip_ws(&ps);
        while (ps.p < ps.end && *ps.p == '\0') ps.p++; // GLB chunks may be padded with zeros (spaces per spec)
        skip_ws(&ps);
        if (ps.p != ps.end) {
            perr(&ps, "text after the value");
            ok = false;
        }
    }
    free(ps.vals);
    free(ps.mems);
    doc->arena = ps.arena;
    if (!ok) {
        json_free(doc);
        return false;
    }
    return true;
}

void json_free(JsonDoc* doc) {
    arena_free(doc->arena);
    memset(doc, 0, sizeof *doc);
}

// --- Lookups ---------------------------------------------------------------------------------

const JsonValue* json_get(const JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (uint32_t i = 0; i < obj->count; i++)
        if (strcmp(obj->members[i].key, key) == 0) return &obj->members[i].value;
    return NULL;
}

const JsonValue* json_at(const JsonValue* arr, size_t i) {
    if (!arr || arr->type != JSON_ARRAY || i >= arr->count) return NULL;
    return &arr->items[i];
}

size_t json_len(const JsonValue* arr) { return arr && arr->type == JSON_ARRAY ? arr->count : 0; }

double json_num(const JsonValue* v, double def) { return v && v->type == JSON_NUMBER ? v->num : def; }

int json_int(const JsonValue* v, int def) {
    if (!v || v->type != JSON_NUMBER || !(v->num >= -2147483648.0 && v->num <= 2147483647.0)) return def;
    return (int)v->num;
}

bool json_bool(const JsonValue* v, bool def) {
    if (!v) return def;
    if (v->type == JSON_TRUE) return true;
    if (v->type == JSON_FALSE) return false;
    return def;
}

const char* json_str(const JsonValue* v, const char* def) { return v && v->type == JSON_STRING ? v->str : def; }

double json_get_num(const JsonValue* obj, const char* key, double def) { return json_num(json_get(obj, key), def); }
int json_get_int(const JsonValue* obj, const char* key, int def) { return json_int(json_get(obj, key), def); }
bool json_get_bool(const JsonValue* obj, const char* key, bool def) { return json_bool(json_get(obj, key), def); }
const char* json_get_str(const JsonValue* obj, const char* key, const char* def) { return json_str(json_get(obj, key), def); }

int json_get_floats(const JsonValue* obj, const char* key, float* out, int n) {
    const JsonValue* a = json_get(obj, key);
    int k = 0;
    for (; k < n && k < (int)json_len(a); k++) out[k] = (float)json_num(json_at(a, (size_t)k), out[k]);
    return k;
}

// --- Writer ----------------------------------------------------------------------------------

void jw_init(JsonWriter* w) {
    memset(w, 0, sizeof *w);
    w->first[0] = 1;
}

void jw_free(JsonWriter* w) {
    free(w->buf);
    memset(w, 0, sizeof *w);
}

static void put(JsonWriter* w, const char* s, size_t n) {
    if (w->fail) return;
    if (w->len + n + 1 > w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 4096;
        while (nc < w->len + n + 1) nc *= 2;
        char* b = realloc(w->buf, nc);
        if (!b) {
            w->fail = true;
            return;
        }
        w->buf = b;
        w->cap = nc;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

// Before a value or a key: the comma between siblings (a value after its key needs none)
static void sep(JsonWriter* w) {
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->depth > 0 && !w->first[w->depth]) put(w, ",", 1);
    w->first[w->depth] = 0;
}

static void open_(JsonWriter* w, char c) {
    sep(w);
    put(w, &c, 1);
    if (w->depth < 255) w->depth++;
    w->first[w->depth] = 1;
}

static void close_(JsonWriter* w, char c) {
    put(w, &c, 1);
    if (w->depth > 0) w->depth--;
}

void jw_obj_begin(JsonWriter* w) { open_(w, '{'); }
void jw_obj_end(JsonWriter* w) { close_(w, '}'); }
void jw_arr_begin(JsonWriter* w) { open_(w, '['); }
void jw_arr_end(JsonWriter* w) { close_(w, ']'); }

static void put_escaped(JsonWriter* w, const char* s) {
    put(w, "\"", 1);
    const char* run = s;
    for (const char* c = s; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (u >= 0x20 && u != '"' && u != '\\') continue;
        put(w, run, (size_t)(c - run));
        char esc[8];
        switch (u) {
        case '"': put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2); break;
        case '\r': put(w, "\\r", 2); break;
        case '\t': put(w, "\\t", 2); break;
        default:
            snprintf(esc, sizeof esc, "\\u%04x", u);
            put(w, esc, 6);
        }
        run = c + 1;
    }
    put(w, run, strlen(run));
    put(w, "\"", 1);
}

void jw_key(JsonWriter* w, const char* key) {
    sep(w);
    put_escaped(w, key);
    put(w, ":", 1);
    w->after_key = true;
}

void jw_str(JsonWriter* w, const char* s) {
    sep(w);
    put_escaped(w, s ? s : "");
}

void jw_float(JsonWriter* w, double v) {
    sep(w);
    char tmp[40];
    if (!isfinite(v)) v = 0.0; // JSON has no NaN / infinity
    if (fabs(v) < 1e15 && v == (double)(long long)v) snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    else snprintf(tmp, sizeof tmp, "%.9g", v);
    put(w, tmp, strlen(tmp));
}

void jw_int(JsonWriter* w, long long v) {
    sep(w);
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    put(w, tmp, strlen(tmp));
}

void jw_bool(JsonWriter* w, bool v) {
    sep(w);
    put(w, v ? "true" : "false", v ? 4 : 5);
}

void jw_null(JsonWriter* w) {
    sep(w);
    put(w, "null", 4);
}

void jw_raw(JsonWriter* w, const char* text) {
    sep(w);
    put(w, text, strlen(text));
}

void jw_kstr(JsonWriter* w, const char* key, const char* s) { jw_key(w, key), jw_str(w, s); }
void jw_kfloat(JsonWriter* w, const char* key, double v) { jw_key(w, key), jw_float(w, v); }
void jw_kint(JsonWriter* w, const char* key, long long v) { jw_key(w, key), jw_int(w, v); }
void jw_kbool(JsonWriter* w, const char* key, bool v) { jw_key(w, key), jw_bool(w, v); }

void jw_kfloats(JsonWriter* w, const char* key, const float* v, int n) {
    jw_key(w, key);
    jw_arr_begin(w);
    for (int i = 0; i < n; i++) jw_float(w, v[i]);
    jw_arr_end(w);
}
