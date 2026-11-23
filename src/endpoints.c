#include "endpoints.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static const unsigned char k_embedded[] = {
#include "endpoints.ini.inc"
};

#define EP_MAX 48
static struct {
    bool loaded;
    char* text;
    struct {
        char section[32], key[48];
        char value[512];
    } cache[EP_MAX];
    int count;
} g;

static bool name_eq(const char* a, size_t an, const char* b) {
    size_t bn = strlen(b);
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool ini_lookup(const char* text, const char* section, const char* key, char* out, size_t cap) {
    if (cap) out[0] = 0;
    if (!text) return false;
    bool in = false;
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        const char* s = p;
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || (unsigned char)*s == 0xEF || (unsigned char)*s == 0xBB ||
                         (unsigned char)*s == 0xBF))
            s++; // spaces and a UTF-8 BOM
        const char* t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) t--;
        if (s < t && *s != ';' && *s != '#') {
            if (*s == '[') {
                const char* c = memchr(s, ']', (size_t)(t - s));
                in = c && name_eq(s + 1, (size_t)(c - s - 1), section);
            } else if (in) {
                const char* eq = memchr(s, '=', (size_t)(t - s));
                if (eq) {
                    const char* ke = eq;
                    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                    if (name_eq(s, (size_t)(ke - s), key)) {
                        const char* v = eq + 1;
                        while (v < t && (*v == ' ' || *v == '\t')) v++;
                        size_t n = (size_t)(t - v);
                        if (n >= cap) n = cap - 1;
                        memcpy(out, v, n);
                        out[n] = 0;
                        return true;
                    }
                }
            }
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

static char* read_file(const char* path) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (t) {
        size_t got = fread(t, 1, (size_t)n, f);
        t[got] = 0;
    }
    fclose(f);
    return t;
}

void endpoints_reload(const char* text) {
    free(g.text);
    g.text = NULL;
    g.count = 0;
    if (text) {
        g.text = malloc(strlen(text) + 1);
        if (g.text) strcpy(g.text, text);
    } else {
        const char* over = getenv("REALENGINE_ENDPOINTS");
        if (over && over[0]) g.text = read_file(over);
        if (!g.text) {
            g.text = malloc(sizeof k_embedded);
            if (g.text) memcpy(g.text, k_embedded, sizeof k_embedded);
        }
    }
    g.loaded = true;
}

// Values are copied out once and kept: callers on other threads hold the pointer (no reload while uploading).
const char* endpoints_get(const char* section, const char* key) {
    if (!g.loaded) endpoints_reload(NULL);
    for (int i = 0; i < g.count; i++)
        if (!strcmp(g.cache[i].section, section) && !strcmp(g.cache[i].key, key)) return g.cache[i].value;
    if (g.count >= EP_MAX) return "";
    int i = g.count++;
    snprintf(g.cache[i].section, sizeof g.cache[i].section, "%s", section);
    snprintf(g.cache[i].key, sizeof g.cache[i].key, "%s", key);
    char v[512];
    ini_lookup(g.text, section, key, v, sizeof v);
    endpoints_expand(v, g.cache[i].value, sizeof g.cache[i].value);
    return g.cache[i].value;
}

void endpoints_expand(const char* s, char* out, size_t cap) {
    char owner[128], repo[128];
    ini_lookup(g.text, "github", "owner", owner, sizeof owner);
    ini_lookup(g.text, "github", "repo", repo, sizeof repo);
    size_t o = 0;
    for (const char* c = s; *c && o + 1 < cap;) {
        const char* rep = NULL;
        size_t skip = 0;
        if (!strncmp(c, "{owner}", 7)) rep = owner, skip = 7;
        else if (!strncmp(c, "{repo}", 6)) rep = repo, skip = 6;
        if (rep) {
            for (const char* r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
            c += skip;
        } else {
            out[o++] = *c++;
        }
    }
    out[o] = 0;
}
