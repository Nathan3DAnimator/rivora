#include "rpak.h"

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "platform_files.h"
#include "re_zlib.h"

// File layout (little endian):
//   header  "RPAK" u32 version (1), u32 count, u32 pad, u64 index offset, u64 index bytes, u64 FNV-1a of the index,
//           u64 FNV-1a of the data (every file's stored bytes in order)
//   data    the files' stored bytes, one after another
//   index   per file (sorted by path, case-insensitive): u16 path bytes, path (UTF-8, '/'), u64 offset, u64 stored, u64 size,
//           u32 flags (1: deflated)
#define RPAK_MAGIC 0x4B415052u // "RPAK"

typedef struct Entry { char* path; uint64_t off, stored, size; uint32_t flags; } Entry;

static struct {
    ReFileMap* map;
    const uint8_t* data;
    uint64_t bytes;
    Entry* e;
    uint32_t n;
    char root[1100]; // normalized: '/', no trailing '/' (compared without case)
    size_t root_len;
} P;

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static void norm(const char* in, char* out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; in[i] && k + 1 < cap; i++) {
        char c = in[i] == '\\' ? '/' : in[i];
        if (c == '/' && k > 0 && out[k - 1] == '/') continue; // (a doubled separator)
        out[k++] = c;
    }
    while (k > 0 && out[k - 1] == '/') k--;
    out[k] = 0;
}
// paths compared without case (Windows' files: the case kept in the index, the hashes of the paths are of it)
static int ci(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}
static int cin(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
    return 0;
}
static int cmp_entry(const void* a, const void* b) { return ci(((const Entry*)a)->path, ((const Entry*)b)->path); }

// --- Writing ---
typedef struct Walk { char base[1100]; char rel[1100]; Entry* e; uint32_t n, cap; } Walk;
static void walk_dir(Walk* w);
static void on_entry(void* user, const ReDirEntry* d) {
    Walk* w = user;
    size_t rl = strlen(w->rel);
    char save[1100];
    memcpy(save, w->rel, rl + 1);
    if (rl) snprintf(w->rel + rl, sizeof w->rel - rl, "/%s", d->name);
    else snprintf(w->rel, sizeof w->rel, "%s", d->name);
    if (d->is_dir) walk_dir(w);
    else if (w->n < 1000000) {
        if (w->n == w->cap) {
            uint32_t nc = w->cap ? w->cap * 2 : 256;
            Entry* ne = realloc(w->e, sizeof *ne * nc);
            if (!ne) { memcpy(w->rel, save, rl + 1); return; }
            w->e = ne, w->cap = nc;
        }
        Entry* e = &w->e[w->n++];
        memset(e, 0, sizeof *e);
        e->path = _strdup(w->rel);
        e->size = d->size;
    }
    memcpy(w->rel, save, rl + 1);
}
static void walk_dir(Walk* w) {
    char full[2300];
    snprintf(full, sizeof full, w->rel[0] ? "%s/%s" : "%s", w->base, w->rel);
    re_dir_walk(full, on_entry, w);
}

bool rpak_write_dir(const char* dir, const char* out, RpakStats* st) {
    double t0 = re_time_seconds();
    Walk w;
    memset(&w, 0, sizeof w);
    snprintf(w.base, sizeof w.base, "%s", dir);
    walk_dir(&w);
    // (sorted by the normalized path: the reader's binary search)
    for (uint32_t i = 0; i < w.n; i++) {
        char nm[1100];
        norm(w.e[i].path, nm, sizeof nm);
        free(w.e[i].path);
        w.e[i].path = _strdup(nm);
    }
    qsort(w.e, w.n, sizeof *w.e, cmp_entry);
    FILE* f = re_fopen(out, "wb");
    if (!f) { for (uint32_t i = 0; i < w.n; i++) free(w.e[i].path); free(w.e); return false; }
    uint8_t hdr[48] = { 0 };
    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    uint64_t at = sizeof hdr, dh = 1469598103934665603ull, raw = 0;
    for (uint32_t i = 0; ok && i < w.n; i++) {
        char full[2300];
        snprintf(full, sizeof full, "%s/%s", dir, w.e[i].path);
        const uint8_t* d = NULL;
        uint64_t sz = 0;
        ReFileMap* m = re_map_file(full, &d, &sz);
        uint8_t* z = NULL;
        size_t zn = 0;
        if (m && sz) z = re_deflate(d, (size_t)sz, &zn);
        bool deflated = z && zn < sz - sz / 10; // (deflated only when it saves 10%)
        const uint8_t* put = deflated ? z : d;
        uint64_t pn = deflated ? zn : (m ? sz : 0);
        if (pn) ok = fwrite(put, 1, (size_t)pn, f) == pn, dh = fnv(dh, put, (size_t)pn);
        w.e[i].off = at, w.e[i].stored = pn, w.e[i].size = m ? sz : 0, w.e[i].flags = deflated ? 1u : 0u;
        at += pn, raw += w.e[i].size;
        free(z);
        re_unmap_file(m);
    }
    // the index
    uint64_t idx_off = at, ih = 1469598103934665603ull, ib = 0;
    for (uint32_t i = 0; ok && i < w.n; i++) {
        uint16_t pl = (uint16_t)strlen(w.e[i].path);
        uint8_t rec[2 + 1024 + 28];
        memcpy(rec, &pl, 2), memcpy(rec + 2, w.e[i].path, pl);
        memcpy(rec + 2 + pl, &w.e[i].off, 8), memcpy(rec + 10 + pl, &w.e[i].stored, 8), memcpy(rec + 18 + pl, &w.e[i].size, 8);
        memcpy(rec + 26 + pl, &w.e[i].flags, 4);
        size_t rn = 30u + pl;
        ok = fwrite(rec, 1, rn, f) == rn;
        ih = fnv(ih, rec, rn), ib += rn;
    }
    uint32_t magic = RPAK_MAGIC, ver = 1, cnt = w.n;
    memcpy(hdr, &magic, 4), memcpy(hdr + 4, &ver, 4), memcpy(hdr + 8, &cnt, 4);
    memcpy(hdr + 16, &idx_off, 8), memcpy(hdr + 24, &ib, 8), memcpy(hdr + 32, &ih, 8), memcpy(hdr + 40, &dh, 8);
    ok = ok && fseek(f, 0, SEEK_SET) == 0 && fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    ok = fclose(f) == 0 && ok;
    if (st) st->files = w.n, st->raw = raw, st->packed = at + ib, st->ms = (re_time_seconds() - t0) * 1000.0;
    for (uint32_t i = 0; i < w.n; i++) free(w.e[i].path);
    free(w.e);
    return ok;
}

// --- Reading ---
bool rpak_mounted(void) { return P.map != NULL; }

bool rpak_mount(const char* pack, const char* root) {
    const uint8_t* d = NULL;
    uint64_t sz = 0;
    ReFileMap* m = re_map_file(pack, &d, &sz);
    if (!m) return false;
    uint32_t magic, ver, cnt;
    uint64_t io, ib, ih, dh;
    if (sz < 48) { re_unmap_file(m); return false; }
    memcpy(&magic, d, 4), memcpy(&ver, d + 4, 4), memcpy(&cnt, d + 8, 4);
    memcpy(&io, d + 16, 8), memcpy(&ib, d + 24, 8), memcpy(&ih, d + 32, 8), memcpy(&dh, d + 40, 8);
    if (magic != RPAK_MAGIC || ver != 1 || io + ib > sz || io < 48 || fnv(1469598103934665603ull, d + io, (size_t)ib) != ih ||
        fnv(1469598103934665603ull, d + 48, (size_t)(io - 48)) != dh) { // (the index's and the data's check)
        printf("[RealEngine] aviso: %s nao e um pacote valido (ou foi alterado)\n", pack);
        re_unmap_file(m);
        return false;
    }
    Entry* e = calloc(cnt ? cnt : 1, sizeof *e);
    if (!e) { re_unmap_file(m); return false; }
    uint64_t at = io;
    for (uint32_t i = 0; i < cnt; i++) {
        uint16_t pl;
        memcpy(&pl, d + at, 2);
        e[i].path = malloc(pl + 1u);
        if (!e[i].path) break;
        memcpy(e[i].path, d + at + 2, pl), e[i].path[pl] = 0;
        memcpy(&e[i].off, d + at + 2 + pl, 8), memcpy(&e[i].stored, d + at + 10 + pl, 8), memcpy(&e[i].size, d + at + 18 + pl, 8);
        memcpy(&e[i].flags, d + at + 26 + pl, 4);
        at += 30u + pl;
    }
    P.map = m, P.data = d, P.bytes = sz, P.e = e, P.n = cnt;
    norm(root, P.root, sizeof P.root);
    P.root_len = strlen(P.root);
    printf("[RealEngine] Pacote: %s (%u arquivos)\n", pack, cnt);
    return true;
}

// the path relative to the root (normalized) or NULL when it isn't under it
static const char* rel_of(const char* path, char* buf, size_t cap) {
    if (!P.map) return NULL;
    norm(path, buf, cap);
    if (cin(buf, P.root, P.root_len)) return NULL;
    if (buf[P.root_len] == 0) return buf + P.root_len; // (the root itself: "")
    if (buf[P.root_len] != '/') return NULL;
    return buf + P.root_len + 1;
}
static const Entry* find(const char* rel) {
    uint32_t lo = 0, hi = P.n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = ci(P.e[mid].path, rel);
        if (!c) return &P.e[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
// the first entry whose path starts with prefix + "/" (a folder in the pack)
static uint32_t first_under(const char* rel) {
    size_t rl = strlen(rel);
    uint32_t lo = 0, hi = P.n;
    char key[1100];
    snprintf(key, sizeof key, rl ? "%s/" : "%s", rel);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ci(P.e[mid].path, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool rpak_has(const char* path, uint64_t* size, bool* is_dir) {
    char buf[1100];
    const char* rel = rel_of(path, buf, sizeof buf);
    if (!rel) return false;
    const Entry* e = rel[0] ? find(rel) : NULL;
    if (e) {
        if (size) *size = e->size;
        if (is_dir) *is_dir = false;
        return true;
    }
    uint32_t i = first_under(rel);
    size_t rl = strlen(rel);
    if (i < P.n && (!rl || (!cin(P.e[i].path, rel, rl) && P.e[i].path[rl] == '/'))) {
        if (size) *size = 0;
        if (is_dir) *is_dir = true;
        return true;
    }
    return false;
}

uint8_t* rpak_read(const char* path, uint64_t* size) {
    char buf[1100];
    const char* rel = rel_of(path, buf, sizeof buf);
    const Entry* e = rel && rel[0] ? find(rel) : NULL;
    if (!e) return NULL;
    uint8_t* out = malloc(e->size ? (size_t)e->size : 1u);
    if (!out) return NULL;
    bool ok = e->flags & 1u ? re_inflate(P.data + e->off, (size_t)e->stored, out, (size_t)e->size) : (memcpy(out, P.data + e->off, (size_t)e->size), true);
    if (!ok) { free(out); return NULL; }
    *size = e->size;
    return out;
}

bool rpak_walk(const char* dir, void (*fn)(void* user, const ReDirEntry* e), void* user) {
    char buf[1100];
    const char* rel = rel_of(dir, buf, sizeof buf);
    if (!rel) return false;
    size_t rl = strlen(rel);
    uint32_t i = first_under(rel);
    if (i >= P.n || (rl && (cin(P.e[i].path, rel, rl) || P.e[i].path[rl] != '/'))) return false;
    char last_dir[1100] = "";
    for (; i < P.n; i++) {
        const char* p = P.e[i].path;
        if (rl && (cin(p, rel, rl) || p[rl] != '/')) break;
        const char* rest = p + (rl ? rl + 1 : 0);
        const char* slash = strchr(rest, '/');
        char name[1100];
        if (slash) { // a folder: once
            size_t nl = (size_t)(slash - rest);
            memcpy(name, rest, nl), name[nl] = 0;
            if (!ci(name, last_dir)) continue;
            snprintf(last_dir, sizeof last_dir, "%s", name);
            ReDirEntry e = { name, true, 0, 0 };
            fn(user, &e);
        } else {
            snprintf(name, sizeof name, "%s", rest);
            ReDirEntry e = { name, false, P.e[i].size, 0 };
            fn(user, &e);
        }
    }
    return true;
}
