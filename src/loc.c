// (2026-11-24, item 15) The game's localization: the table, its files, the compiled cultures, the run time (loc.h).
#include "loc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io_json.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"

const char* const g_loc_status_names[LOC_STATUS_COUNT] = { "Missing", "Automatic", "Translated", "Reviewed" };

static struct {
    char root[700]; // <Content>
    LocProject p;
    bool loaded;
} L;

// --- the table's files ---
static const char* content_dir(void) {
    if (L.root[0]) return L.root;
    const char* r = mat_root(); // (the editor's and the game's Content: the materials' root)
    return r ? r : "";
}
static void loc_dir(char* out, size_t cap) { snprintf(out, cap, "%s/Localization/Game", content_dir()); }
static void rloc_path(char* out, size_t cap) {
    char d[800];
    loc_dir(d, sizeof d);
    snprintf(out, cap, "%s/Game.rloc", d);
}

void loc_dir_file(char* out, size_t cap, const char* name, const char* ext) {
    char d[800];
    loc_dir(d, sizeof d);
    snprintf(out, cap, "%s/%s%s", d, name, ext);
}

uint64_t loc_hash(const char* ns, const char* key) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = ns ? ns : ""; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ 0u) * 1099511628211ull;
    for (const char* p = key ? key : ""; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return h ? h : 1;
}

const char* loc_tr(const LocEntry* e, int c) { return e && c >= 0 && c < LOC_MAX_CULTURES && e->tr[c].text ? e->tr[c].text : ""; }
void loc_tr_set(LocEntry* e, int c, const char* s) {
    if (!e || c < 0 || c >= LOC_MAX_CULTURES) return;
    free(e->tr[c].text);
    e->tr[c].text = s && s[0] ? _strdup(s) : NULL;
}

static void project_free(void) {
    for (uint32_t i = 0; i < L.p.n; i++)
        for (int c = 0; c < LOC_MAX_CULTURES; c++) free(L.p.e[i].tr[c].text);
    free(L.p.e);
    memset(&L.p, 0, sizeof L.p);
}

static void load(void) {
    L.loaded = true;
    project_free();
    snprintf(L.p.native, sizeof L.p.native, "%s", "en");
    char path[900];
    rloc_path(path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = text && fread(text, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc d;
    char err[160];
    if (!ok || !json_parse(text, (size_t)n, &d, err, sizeof err)) {
        free(text);
        printf("[RealEngine] Localization: %s couldn't be read\n", path);
        return;
    }
    free(text);
    snprintf(L.p.native, sizeof L.p.native, "%s", json_get_str(&d.root, "native", "en"));
    const JsonValue* tg = json_get(&d.root, "targets");
    for (size_t i = 0; i < json_len(tg) && L.p.ntargets < LOC_MAX_CULTURES; i++)
        snprintf(L.p.targets[L.p.ntargets++], LOC_CULTURE, "%s", json_str(json_at(tg, i), ""));
    const JsonValue* es = json_get(&d.root, "entries");
    for (size_t i = 0; i < json_len(es); i++) {
        const JsonValue* o = json_at(es, i);
        LocEntry* e = loc_gather_add(json_get_str(o, "ns", ""), json_get_str(o, "key", ""), json_get_str(o, "src", ""), json_get_str(o, "where", ""));
        if (!e) continue;
        e->stale = json_get_bool(o, "stale", false);
        const JsonValue* tr = json_get(o, "tr");
        for (int c = 0; c < L.p.ntargets; c++) {
            const JsonValue* t = json_get(tr, L.p.targets[c]);
            if (!t) continue;
            loc_tr_set(e, c, json_get_str(t, "t", ""));
            int st = json_get_int(t, "s", loc_tr(e, c)[0] ? LOC_TRANSLATED : LOC_MISSING);
            e->tr[c].status = (uint8_t)(st >= 0 && st < LOC_STATUS_COUNT ? st : LOC_MISSING);
        }
    }
    json_free(&d);
    L.p.dirty = false;
}

LocProject* loc_project(void) {
    if (!L.loaded && content_dir()[0]) load(); // (no Content yet: read when there is one)
    if (!L.p.native[0]) snprintf(L.p.native, sizeof L.p.native, "%s", "en");
    return &L.p;
}

void loc_set_content_root(const char* r) {
    snprintf(L.root, sizeof L.root, "%s", r ? r : "");
    L.loaded = false;
}

bool loc_save(void) {
    LocProject* p = loc_project();
    char dir[800], path[900];
    loc_dir(dir, sizeof dir);
    re_make_dirs(dir);
    rloc_path(path, sizeof path);
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "native", p->native);
    jw_key(&w, "targets");
    jw_arr_begin(&w);
    for (int c = 0; c < p->ntargets; c++) jw_str(&w, p->targets[c]);
    jw_arr_end(&w);
    jw_key(&w, "entries");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        jw_obj_begin(&w);
        jw_kstr(&w, "ns", e->ns), jw_kstr(&w, "key", e->key), jw_kstr(&w, "src", e->source), jw_kstr(&w, "where", e->where);
        if (e->stale) jw_kbool(&w, "stale", true);
        jw_key(&w, "tr");
        jw_obj_begin(&w);
        for (int c = 0; c < p->ntargets; c++) {
            if (!loc_tr(e, c)[0] && e->tr[c].status == LOC_MISSING) continue;
            jw_key(&w, p->targets[c]);
            jw_obj_begin(&w);
            jw_kstr(&w, "t", loc_tr(e, c));
            jw_kint(&w, "s", e->tr[c].status);
            jw_obj_end(&w);
        }
        jw_obj_end(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    FILE* f = re_fopen(path, "wb");
    bool ok = f && !w.fail && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) ok = fclose(f) == 0 && ok;
    jw_free(&w);
    if (ok) p->dirty = false;
    return ok;
}

int loc_add_culture(const char* code) {
    LocProject* p = loc_project();
    if (!code || !code[0] || !strcmp(code, p->native)) return -1;
    for (int c = 0; c < p->ntargets; c++)
        if (!_stricmp(p->targets[c], code)) return c;
    if (p->ntargets >= LOC_MAX_CULTURES) return -1;
    snprintf(p->targets[p->ntargets], LOC_CULTURE, "%s", code);
    for (uint32_t i = 0; i < p->n; i++) memset(&p->e[i].tr[p->ntargets], 0, sizeof(LocTr));
    p->dirty = true;
    return p->ntargets++;
}

void loc_remove_culture(int c) {
    LocProject* p = loc_project();
    if (c < 0 || c >= p->ntargets) return;
    for (int k = c; k < p->ntargets - 1; k++) memcpy(p->targets[k], p->targets[k + 1], LOC_CULTURE);
    for (uint32_t i = 0; i < p->n; i++) free(p->e[i].tr[c].text), p->e[i].tr[c].text = NULL;
    for (uint32_t i = 0; i < p->n; i++)
        memmove(&p->e[i].tr[c], &p->e[i].tr[c + 1], sizeof(LocTr) * (size_t)(p->ntargets - c - 1)), memset(&p->e[i].tr[p->ntargets - 1], 0, sizeof(LocTr));
    p->ntargets--, p->dirty = true;
}

LocEntry* loc_gather_add(const char* ns, const char* key, const char* source, const char* where) {
    LocProject* p = &L.p; // (no load here: load() calls it)
    if (!ns || !key || !source) return NULL;
    uint64_t h = loc_hash(ns, key);
    for (uint32_t i = 0; i < p->n; i++)
        if (p->e[i].hash == h && !strcmp(p->e[i].ns, ns) && !strcmp(p->e[i].key, key)) {
            LocEntry* e = &p->e[i];
            e->stale = false;
            if (strcmp(e->source, source)) { // (the source changed: its translations to look at again)
                snprintf(e->source, LOC_TEXT, "%s", source);
                for (int c = 0; c < LOC_MAX_CULTURES; c++)
                    if (e->tr[c].status > LOC_AUTO) e->tr[c].status = LOC_AUTO;
                p->dirty = true;
            }
            if (where && where[0]) snprintf(e->where, sizeof e->where, "%s", where);
            return e;
        }
    if (p->n == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 256;
        LocEntry* ne = realloc(p->e, sizeof(LocEntry) * cap);
        if (!ne) return NULL;
        p->e = ne, p->cap = cap;
    }
    LocEntry* e = &p->e[p->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->ns, LOC_NS, "%s", ns), snprintf(e->key, LOC_KEY, "%s", key), snprintf(e->source, LOC_TEXT, "%s", source);
    snprintf(e->where, sizeof e->where, "%s", where ? where : "");
    e->hash = h;
    p->dirty = true;
    return e;
}

void loc_counts(int c, uint32_t* done, uint32_t* total) {
    LocProject* p = loc_project();
    uint32_t d = 0, t = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        if (p->e[i].stale) continue;
        t++;
        if (c >= 0 && c < p->ntargets && loc_tr(&p->e[i], c)[0] && p->e[i].tr[c].status >= LOC_AUTO) d++;
    }
    if (done) *done = d;
    if (total) *total = t;
}

// --- .po ---
static void po_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f), fputc(*s, f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\t') fputs("\\t", f);
        else fputc(*s, f);
    }
    fputc('"', f);
}

bool loc_export_po(int c, const char* path) {
    LocProject* p = loc_project();
    if (c < 0 || c >= p->ntargets) return false;
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# Rivora Engine: the game's texts, %s -> %s\nmsgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Language: %s\\n\"\n\n",
            p->native, p->targets[c], p->targets[c]);
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        if (e->stale) continue;
        if (e->where[0]) fprintf(f, "#. %s\n", e->where);
        if (e->tr[c].status == LOC_AUTO) fputs("#, fuzzy\n", f);
        char ctx[LOC_NS + LOC_KEY + 2];
        snprintf(ctx, sizeof ctx, "%s,%s", e->ns, e->key);
        fputs("msgctxt ", f), po_str(f, ctx), fputs("\nmsgid ", f), po_str(f, e->source), fputs("\nmsgstr ", f), po_str(f, loc_tr(e, c)), fputs("\n\n", f);
    }
    return fclose(f) == 0;
}

// a quoted .po string (possibly continued on the next lines) into out
static const char* po_read_str(const char* s, char* out, size_t cap) {
    size_t n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '"') break;
        s++;
        while (*s && *s != '"') {
            char ch = *s++;
            if (ch == '\\' && *s) {
                char e = *s++;
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            if (n + 1 < cap) out[n++] = ch;
        }
        if (*s == '"') s++;
        while (*s == '\r' || *s == '\n' || *s == ' ') s++; // (a continued string on the next line)
    }
    out[n] = 0;
    return s;
}

static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (t && n > 0 && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n > 0 ? n : 0] = 0, *len = (size_t)(n > 0 ? n : 0);
    return t;
}

static LocEntry* find_entry(const char* ns, const char* key) {
    LocProject* p = loc_project();
    uint64_t h = loc_hash(ns, key);
    for (uint32_t i = 0; i < p->n; i++)
        if (p->e[i].hash == h && !strcmp(p->e[i].ns, ns) && !strcmp(p->e[i].key, key)) return &p->e[i];
    return NULL;
}

bool loc_import_po(int c, const char* path, uint32_t* changed) {
    LocProject* p = loc_project();
    if (changed) *changed = 0;
    if (c < 0 || c >= p->ntargets) return false;
    size_t len = 0;
    char* t = read_all(path, &len);
    if (!t) return false;
    static char ctx[LOC_NS + LOC_KEY + 2], id[LOC_TEXT * 2], str[LOC_TEXT * 2];
    bool fuzzy = false;
    ctx[0] = id[0] = str[0] = 0;
    for (const char* s = t; *s;) {
        const char* eol = strchr(s, '\n');
        if (!strncmp(s, "#,", 2) && strstr(s, "fuzzy") && (!eol || strstr(s, "fuzzy") < eol)) fuzzy = true;
        if (!strncmp(s, "msgctxt ", 8)) s = po_read_str(s + 8, ctx, sizeof ctx);
        else if (!strncmp(s, "msgid ", 6)) s = po_read_str(s + 6, id, sizeof id);
        else if (!strncmp(s, "msgstr ", 7)) {
            s = po_read_str(s + 7, str, sizeof str);
            char* comma = strchr(ctx, ',');
            if (comma) {
                *comma = 0;
                LocEntry* e = find_entry(ctx, comma + 1);
                if (e && strcmp(loc_tr(e, c), str)) {
                    loc_tr_set(e, c, str);
                    e->tr[c].status = (uint8_t)(!str[0] ? LOC_MISSING : fuzzy ? LOC_AUTO : LOC_TRANSLATED);
                    p->dirty = true;
                    if (changed) (*changed)++;
                }
            }
            ctx[0] = id[0] = str[0] = 0, fuzzy = false;
            continue;
        } else s = eol ? eol + 1 : s + strlen(s);
    }
    free(t);
    return true;
}

// --- .csv (every culture: Namespace, Key, Source, then one column per target) ---
static void csv_field(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}
bool loc_export_csv(const char* path) {
    LocProject* p = loc_project();
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    fputs("\xEF\xBB\xBFNamespace,Key,Source", f); // (a BOM: Excel reads it as UTF-8)
    for (int c = 0; c < p->ntargets; c++) fprintf(f, ",%s", p->targets[c]);
    fputs("\r\n", f);
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        if (e->stale) continue;
        csv_field(f, e->ns), fputc(',', f), csv_field(f, e->key), fputc(',', f), csv_field(f, e->source);
        for (int c = 0; c < p->ntargets; c++) fputc(',', f), csv_field(f, loc_tr(e, c));
        fputs("\r\n", f);
    }
    return fclose(f) == 0;
}

// one CSV field at s into out (quoted or not); returns past its comma / line end; *eol set at a line's end
static const char* csv_read(const char* s, char* out, size_t cap, bool* eol) {
    size_t n = 0;
    *eol = false;
    if (*s == '"') {
        s++;
        while (*s) {
            if (*s == '"' && s[1] == '"') s += 2, n + 1 < cap ? (void)(out[n++] = '"') : (void)0;
            else if (*s == '"') {
                s++;
                break;
            } else {
                if (n + 1 < cap) out[n++] = *s;
                s++;
            }
        }
    } else
        while (*s && *s != ',' && *s != '\r' && *s != '\n') {
            if (n + 1 < cap) out[n++] = *s;
            s++;
        }
    out[n] = 0;
    if (*s == ',') return s + 1;
    while (*s == '\r' || *s == '\n') s++, *eol = true;
    if (!*s) *eol = true;
    return s;
}

bool loc_import_csv(const char* path, uint32_t* changed) {
    LocProject* p = loc_project();
    if (changed) *changed = 0;
    size_t len = 0;
    char* t = read_all(path, &len);
    if (!t) return false;
    const char* s = t;
    if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;
    // the header: which column is which culture
    int col[LOC_MAX_CULTURES + 3], ncol = 0;
    static char f[LOC_TEXT * 2];
    bool eol = false;
    while (*s && !eol && ncol < LOC_MAX_CULTURES + 3) {
        s = csv_read(s, f, sizeof f, &eol);
        int c = -1;
        for (int k = 0; k < p->ntargets; k++)
            if (!_stricmp(p->targets[k], f)) c = k;
        col[ncol++] = ncol < 3 ? -2 : c;
    }
    while (*s) {
        char ns[LOC_NS] = "", key[LOC_KEY] = "";
        LocEntry* e = NULL;
        eol = false;
        for (int k = 0; *s && !eol; k++) {
            s = csv_read(s, f, sizeof f, &eol);
            if (k == 0) snprintf(ns, sizeof ns, "%s", f);
            else if (k == 1) snprintf(key, sizeof key, "%s", f), e = find_entry(ns, key);
            else if (k < ncol && col[k] >= 0 && e && strcmp(loc_tr(e, col[k]), f)) {
                loc_tr_set(e, col[k], f);
                e->tr[col[k]].status = (uint8_t)(f[0] ? LOC_TRANSLATED : LOC_MISSING);
                p->dirty = true;
                if (changed) (*changed)++;
            }
        }
    }
    free(t);
    return true;
}

// --- the compiled cultures (.rlocres): "RLOC", version, count, pool bytes; count x { hash, offset, length } sorted by
// hash; the texts (UTF-8, each NUL-ended) ---
#define RLOC_MAGIC 0x434F4C52u
typedef struct RlocEntry { uint64_t hash; uint32_t off, len; } RlocEntry;
static int cmp_rloc(const void* a, const void* b) {
    uint64_t x = ((const RlocEntry*)a)->hash, y = ((const RlocEntry*)b)->hash;
    return x < y ? -1 : x > y;
}
static bool write_culture(const char* code, int c) {
    LocProject* p = loc_project();
    char dir[800], path[900];
    loc_dir(dir, sizeof dir);
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/%s.rlocres", dir, code);
    RlocEntry* es = malloc(sizeof(RlocEntry) * (p->n ? p->n : 1));
    size_t pool = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; es && i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        const char* t = c < 0 ? e->source : loc_tr(e, c);
        if (e->stale || !t[0]) continue; // (missing: the source shows)
        es[n].hash = e->hash, es[n].off = (uint32_t)pool, es[n].len = (uint32_t)strlen(t);
        pool += es[n].len + 1, n++;
    }
    if (!es) return false;
    qsort(es, n, sizeof *es, cmp_rloc);
    FILE* f = re_fopen(path, "wb");
    bool ok = f != NULL;
    uint32_t head[4] = { RLOC_MAGIC, 1, n, (uint32_t)pool };
    ok = ok && fwrite(head, sizeof head, 1, f) == 1 && fwrite(es, sizeof *es, n, f) == n;
    // the pool: each entry's text at its offset (offsets were given in table order)
    char* buf = ok && pool ? calloc(pool, 1) : NULL;
    if (pool && !buf) ok = false;
    for (uint32_t i = 0, o = 0; ok && i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        const char* t = c < 0 ? e->source : loc_tr(e, c);
        if (e->stale || !t[0]) continue;
        size_t l = strlen(t);
        memcpy(buf + o, t, l), o += (uint32_t)l + 1;
    }
    if (ok && pool) ok = fwrite(buf, 1, pool, f) == pool;
    free(buf), free(es);
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

static void cache_clear(void);
static const char* rt_culture(void);
int loc_compile(char* msg, size_t cap) {
    LocProject* p = loc_project();
    char cur[LOC_CULTURE];
    snprintf(cur, sizeof cur, "%s", rt_culture());
    cache_clear(); // (the tables read before: stale; the current culture read again below)
    int n = 0;
    double t0 = re_time_seconds();
    if (write_culture(p->native, -1)) n++;
    for (int c = 0; c < p->ntargets; c++) n += write_culture(p->targets[c], c);
    if (msg) snprintf(msg, cap, "%d culture(s) compiled in %.1f ms", n, (re_time_seconds() - t0) * 1000.0);
    if (cur[0] && _stricmp(cur, p->native)) loc_set_culture(cur);
    return n;
}

// --- run time ---
static struct {
    char culture[LOC_CULTURE];
    RlocEntry* e;
    char* pool;
    uint32_t n;
    size_t pool_n;
    uint32_t revision;
    double switch_ms;
} R;

// (the tables read stay in memory, one per culture: a switch back is a pointer's swap; a compile drops them)
typedef struct CultTable { char code[LOC_CULTURE]; RlocEntry* e; char* pool; uint32_t n; size_t pool_n; } CultTable;
static CultTable g_cache[LOC_MAX_CULTURES + 1];
static int g_ncache;
static void table_free(void) { R.e = NULL, R.pool = NULL, R.n = 0, R.pool_n = 0; }
static const char* rt_culture(void) { return R.culture; }
static void cache_clear(void) {
    for (int i = 0; i < g_ncache; i++) free(g_cache[i].e), free(g_cache[i].pool);
    memset(g_cache, 0, sizeof g_cache), g_ncache = 0;
    table_free();
}

bool loc_set_culture(const char* code) {
    double t0 = re_time_seconds();
    code = code ? code : "";
    LocProject* p = loc_project();
    bool native = !code[0] || !_stricmp(code, p->native);
    table_free();
    bool ok = true;
    const CultTable* hit = NULL;
    for (int i = 0; !native && i < g_ncache; i++)
        if (!_stricmp(g_cache[i].code, code)) hit = &g_cache[i];
    if (hit) R.e = hit->e, R.pool = hit->pool, R.n = hit->n, R.pool_n = hit->pool_n;
    if (!native && !hit) {
        char dir[800], path[900];
        loc_dir(dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s.rlocres", dir, code);
        FILE* f = re_fopen(path, "rb");
        uint32_t head[4] = { 0 };
        ok = f && fread(head, sizeof head, 1, f) == 1 && head[0] == RLOC_MAGIC && head[1] == 1;
        if (ok) {
            R.e = malloc(sizeof(RlocEntry) * (head[2] ? head[2] : 1));
            R.pool = malloc(head[3] + 1u);
            ok = R.e && R.pool && fread(R.e, sizeof(RlocEntry), head[2], f) == head[2] && fread(R.pool, 1, head[3], f) == head[3];
            if (ok) R.n = head[2], R.pool_n = head[3], R.pool[head[3]] = 0;
            for (uint32_t i = 0; ok && i < R.n; i++) ok = R.e[i].off + R.e[i].len <= R.pool_n;
        }
        if (f) fclose(f);
        if (!ok) {
            free(R.e), free(R.pool);
            table_free();
            printf("[RealEngine] Localization: no compiled table for \"%s\" (%s)\n", code, path);
        } else if (g_ncache <= LOC_MAX_CULTURES) {
            CultTable* c = &g_cache[g_ncache++];
            snprintf(c->code, sizeof c->code, "%s", code);
            c->e = R.e, c->pool = R.pool, c->n = R.n, c->pool_n = R.pool_n;
        }
    }
    snprintf(R.culture, sizeof R.culture, "%s", ok ? (native ? p->native : code) : p->native);
    R.revision++;
    R.switch_ms = (re_time_seconds() - t0) * 1000.0;
    return ok;
}

const char* loc_culture(void) { return R.culture[0] ? R.culture : loc_project()->native; }
uint32_t loc_revision(void) { return R.revision; }
bool loc_active(void) { return R.n > 0; }

const char* loc_text(const char* ns, const char* key, const char* source) {
    if (!R.n) return source;
    uint64_t h = loc_hash(ns, key);
    uint32_t lo = 0, hi = R.n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (R.e[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    return lo < R.n && R.e[lo].hash == h ? R.pool + R.e[lo].off : source;
}

int loc_cultures(const char** out, int cap) {
    LocProject* p = loc_project();
    int n = 0;
    if (n < cap) out[n++] = p->native;
    for (int c = 0; c < p->ntargets && n < cap; c++) out[n++] = p->targets[c];
    return n;
}

const char* loc_culture_name(const char* code) {
    static const struct { const char* code; const char* name; } N[] = {
        { "en", "English" }, { "en-US", "English (US)" }, { "en-GB", "English (UK)" }, { "pt", "Português" }, { "pt-BR", "Português (Brasil)" },
        { "pt-PT", "Português (Portugal)" }, { "es", "Español" }, { "es-419", "Español (Latinoamérica)" }, { "es-MX", "Español (México)" },
        { "fr", "Français" }, { "de", "Deutsch" }, { "it", "Italiano" }, { "ja", "日本語" }, { "ko", "한국어" }, { "zh-Hans", "简体中文" },
        { "zh-Hant", "繁體中文" }, { "ru", "Русский" }, { "pl", "Polski" }, { "tr", "Türkçe" }, { "ar", "العربية" }, { "nl", "Nederlands" },
        { "sv", "Svenska" }, { "uk", "Українська" }, { "cs", "Čeština" }, { "hi", "हिन्दी" }, { "id", "Bahasa Indonesia" },
    };
    for (size_t i = 0; code && i < sizeof N / sizeof N[0]; i++)
        if (!_stricmp(N[i].code, code)) return N[i].name;
    return code ? code : "";
}

LocStats loc_stats(void) {
    LocStats s = { R.n, sizeof(RlocEntry) * R.n + R.pool_n, R.switch_ms };
    return s;
}

// --- Unreal's pseudo-localization: every vowel accented, the text bracketed (what isn't localized shows plain; a longer
// text shows what would overflow) ---
void loc_pseudo(const char* in, char* out, size_t cap) {
    static const char* const map[128] = {
        ['a'] = "\xC3\xA1", ['e'] = "\xC3\xA9", ['i'] = "\xC3\xAD", ['o'] = "\xC3\xB3", ['u'] = "\xC3\xBA", ['A'] = "\xC3\x81",
        ['E'] = "\xC3\x89", ['I'] = "\xC3\x8D", ['O'] = "\xC3\x93", ['U'] = "\xC3\x9A", ['n'] = "\xC3\xB1", ['c'] = "\xC3\xA7",
    };
    size_t n = 0;
    if (cap < 4) {
        if (cap) out[0] = 0;
        return;
    }
    out[n++] = '[';
    bool arg = false; // (inside a {format} argument: kept as it is)
    for (const char* s = in; *s && n + 4 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '{') arg = true;
        else if (ch == '}') arg = false;
        const char* m = ch < 128 && !arg ? map[ch] : NULL;
        if (m) {
            size_t l = strlen(m);
            memcpy(out + n, m, l), n += l;
        } else
            out[n++] = (char)ch;
    }
    out[n++] = ']';
    out[n] = 0;
}

#include "loc_test.inc" // (2026-11-24) --loc-test
