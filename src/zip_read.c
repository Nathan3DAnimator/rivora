#include "zip_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re_zlib.h" // re_inflate_raw, re_deflate
#include "platform.h"
#include "platform_files.h"

uint32_t re_crc32(uint32_t crc, const void* data, size_t len) {
    static uint32_t table[256];
    static volatile int32_t ready = 0;
    if (!re_atomic_load(&ready)) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        re_atomic_store(&ready, 1);
    }
    const uint8_t* p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | rd16(p + 2) << 16; }

typedef struct ZipEntry {
    char name[512];
    uint32_t method, crc, csize, usize, local;
} ZipEntry;

// A name safe to put under the output folder: relative, no "..", no drive.
static bool safe_name(const char* n) {
    if (!n[0] || n[0] == '/' || n[0] == '\\' || strchr(n, ':')) return false;
    const char* s = n;
    while (*s) {
        const char* e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return false;
        s = *e ? e + 1 : e;
    }
    return true;
}

static void fail(char* err, size_t cap, const char* msg, const char* name) {
    if (err) snprintf(err, cap, "%s%s%s", msg, name ? ": " : "", name ? name : "");
}

bool zip_extract(const char* zip_path, const char* out_dir, bool strip_top, int* files, char* err, size_t err_cap) {
    if (files) *files = 0;
    const uint8_t* d = NULL;
    uint64_t size = 0;
    ReFileMap* map = re_map_file(zip_path, &d, &size);
    if (!map) {
        fail(err, err_cap, "não foi possível abrir o zip", NULL);
        return false;
    }
    bool ok = false;
    ZipEntry* ents = NULL;
    // End of central directory: the last "PK\5\6" in the last 64 KB
    int64_t eocd = -1;
    for (int64_t i = (int64_t)size - 22; i >= 0 && i >= (int64_t)size - 22 - 65535; i--)
        if (rd32(d + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        fail(err, err_cap, "não é um arquivo zip", NULL);
        goto done;
    }
    uint32_t count = rd16(d + eocd + 10), cd_size = rd32(d + eocd + 12), cd_off = rd32(d + eocd + 16);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu || (uint64_t)cd_off + cd_size > size) {
        fail(err, err_cap, "zip64 ou diretório central fora do arquivo", NULL);
        goto done;
    }
    ents = calloc(count ? count : 1, sizeof *ents);
    if (!ents) goto done;
    const uint8_t* p = d + cd_off;
    const uint8_t* end = d + cd_off + cd_size;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > end || rd32(p) != 0x02014b50u) {
            fail(err, err_cap, "diretório central danificado", NULL);
            goto done;
        }
        uint32_t nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        if (p + 46 + nlen > end || nlen >= sizeof ents[i].name) {
            fail(err, err_cap, "nome de arquivo inválido no zip", NULL);
            goto done;
        }
        ZipEntry* e = &ents[i];
        if (rd16(p + 8) & 1) {
            fail(err, err_cap, "zip com senha", NULL);
            goto done;
        }
        e->method = rd16(p + 10);
        e->crc = rd32(p + 16);
        e->csize = rd32(p + 20);
        e->usize = rd32(p + 24);
        e->local = rd32(p + 42);
        memcpy(e->name, p + 46, nlen);
        e->name[nlen] = 0;
        for (char* c = e->name; *c; c++)
            if (*c == '\\') *c = '/'; // Windows' PowerShell 5 writes backslashes
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->local == 0xFFFFFFFFu) {
            fail(err, err_cap, "entrada zip64 não suportada", e->name);
            goto done;
        }
        p += 46 + nlen + xlen + clen;
    }
    // The common top folder
    size_t top = 0;
    if (strip_top && count > 0) {
        const char* slash = strchr(ents[0].name, '/');
        if (slash) {
            top = (size_t)(slash - ents[0].name) + 1;
            for (uint32_t i = 0; i < count && top; i++)
                if (strncmp(ents[i].name, ents[0].name, top) != 0) top = 0;
        }
    }
    if (!re_make_dirs(out_dir)) {
        fail(err, err_cap, "não foi possível criar a pasta", out_dir);
        goto done;
    }
    for (uint32_t i = 0; i < count; i++) {
        ZipEntry* e = &ents[i];
        const char* rel = e->name + top;
        size_t rl = strlen(rel);
        if (!rl) continue; // the top folder itself
        if (!safe_name(rel)) {
            fail(err, err_cap, "caminho inseguro no zip", e->name);
            goto done;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", out_dir, rel);
        if (rel[rl - 1] == '/') { // a folder entry
            path[strlen(path) - 1] = 0;
            re_make_dirs(path);
            continue;
        }
        char* last = strrchr(path, '/');
        if (last) {
            *last = 0;
            re_make_dirs(path);
            *last = '/';
        }
        uint64_t lo = e->local;
        if (lo + 30 > size || rd32(d + lo) != 0x04034b50u) {
            fail(err, err_cap, "cabeçalho local danificado", e->name);
            goto done;
        }
        uint64_t data_at = lo + 30 + rd16(d + lo + 26) + rd16(d + lo + 28);
        if (data_at + e->csize > size) {
            fail(err, err_cap, "entrada além do fim do zip", e->name);
            goto done;
        }
        uint8_t* buf = malloc(e->usize ? e->usize : 1);
        if (!buf) {
            fail(err, err_cap, "sem memória", e->name);
            goto done;
        }
        bool good;
        if (e->method == 0) {
            good = e->csize == e->usize;
            if (good) memcpy(buf, d + data_at, e->usize);
        } else if (e->method == 8) {
            good = re_inflate_raw(d + data_at, e->csize, buf, e->usize);
        } else {
            free(buf);
            fail(err, err_cap, "método de compressão não suportado", e->name);
            goto done;
        }
        if (!good || re_crc32(0, buf, e->usize) != e->crc) {
            free(buf);
            fail(err, err_cap, "dados danificados (CRC)", e->name);
            goto done;
        }
        FILE* f = re_fopen(path, "wb");
        bool wrote = f && fwrite(buf, 1, e->usize, f) == e->usize;
        if (f && fclose(f) != 0) wrote = false;
        free(buf);
        if (!wrote) {
            fail(err, err_cap, "não foi possível gravar", path);
            goto done;
        }
        if (files) (*files)++;
    }
    ok = true;
done:
    free(ents);
    re_unmap_file(map);
    return ok;
}

static void wr16(FILE* f, uint32_t v) {
    fputc((int)(v & 255), f);
    fputc((int)(v >> 8 & 255), f);
}
static void wr32(FILE* f, uint32_t v) {
    wr16(f, v & 0xFFFF);
    wr16(f, v >> 16);
}

bool zip_write(const char* zip_path, const char* const* names, const uint8_t* const* data, const size_t* sizes, int count,
               bool deflate) {
    FILE* f = re_fopen(zip_path, "wb");
    if (!f) return false;
    uint32_t* offs = calloc((size_t)(count ? count : 1), sizeof *offs);
    uint32_t* csz = calloc((size_t)(count ? count : 1), sizeof *csz);
    uint32_t* crcs = calloc((size_t)(count ? count : 1), sizeof *crcs);
    bool ok = offs && csz && crcs;
    for (int i = 0; ok && i < count; i++) {
        crcs[i] = re_crc32(0, data[i], sizes[i]);
        uint8_t* z = NULL;
        size_t zn = 0;
        const uint8_t* body = data[i];
        size_t bn = sizes[i];
        if (deflate && sizes[i]) {
            z = re_deflate(data[i], sizes[i], &zn);
            if (z && zn > 6) { // zlib -> raw deflate: no 2-byte header, no Adler-32
                body = z + 2;
                bn = zn - 6;
            }
        }
        bool deflated = body != data[i];
        offs[i] = (uint32_t)ftell(f);
        csz[i] = (uint32_t)bn;
        size_t nl = strlen(names[i]);
        wr32(f, 0x04034b50u);
        wr16(f, 20);
        wr16(f, 0x800); // UTF-8 names
        wr16(f, deflated ? 8 : 0);
        wr16(f, 0);
        wr16(f, 0x21);
        wr32(f, crcs[i]);
        wr32(f, (uint32_t)bn);
        wr32(f, (uint32_t)sizes[i]);
        wr16(f, (uint32_t)nl);
        wr16(f, 0);
        fwrite(names[i], 1, nl, f);
        if (bn) fwrite(body, 1, bn, f);
        // the method for the central directory: the high bit of the compressed size (sizes here stay below 2 GB)
        if (deflated) csz[i] |= 0x80000000u;
        free(z);
    }
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; ok && i < count; i++) {
        bool deflated = (csz[i] & 0x80000000u) != 0;
        uint32_t c = csz[i] & 0x7FFFFFFFu;
        size_t nl = strlen(names[i]);
        wr32(f, 0x02014b50u);
        wr16(f, 20);
        wr16(f, 20);
        wr16(f, 0x800);
        wr16(f, deflated ? 8 : 0);
        wr16(f, 0);
        wr16(f, 0x21);
        wr32(f, crcs[i]);
        wr32(f, c);
        wr32(f, (uint32_t)sizes[i]);
        wr16(f, (uint32_t)nl);
        wr16(f, 0);
        wr16(f, 0);
        wr16(f, 0);
        wr16(f, 0);
        wr32(f, 0);
        wr32(f, offs[i]);
        fwrite(names[i], 1, nl, f);
    }
    uint32_t cd_end = (uint32_t)ftell(f);
    wr32(f, 0x06054b50u);
    wr16(f, 0);
    wr16(f, 0);
    wr16(f, (uint32_t)count);
    wr16(f, (uint32_t)count);
    wr32(f, cd_end - cd);
    wr32(f, cd);
    wr16(f, 0);
    free(offs);
    free(csz);
    free(crcs);
    if (fclose(f) != 0) ok = false;
    return ok;
}
