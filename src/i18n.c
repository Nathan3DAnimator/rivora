#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "i18n.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const I18nEntry g_i18n_entries[];
extern const int g_i18n_entry_count;
extern const I18nEs g_i18n_es[]; // (2026-11-25, item 12: i18n_table_es.c)
extern const int g_i18n_es_count;

static int g_lang = LANG_EN;

// ---------------------------------------------------------------------------
// msgid -> entry: open addressing over the static table, built on first use
// ---------------------------------------------------------------------------

#define INDEX_CAP 32768 // power of two, more than twice the table (16384 until item 12: the nodes' names pass 8192) (a full index would make lookups of missing keys loop)
static int32_t g_index[INDEX_CAP]; // entry + 1, 0 = empty
static int32_t g_index_es[INDEX_CAP]; // the Spanish table's
static volatile bool g_index_ready;

static uint32_t hash_n(const char* s, size_t n) {
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void build_index(void) {
    if (g_index_ready) return;
    memset(g_index, 0, sizeof g_index);
    for (int i = 0; i < g_i18n_entry_count && i < INDEX_CAP / 2; i++) { // past half: untranslated, the i18n test fails
        const char* m = g_i18n_entries[i].msgid;
        uint32_t k = hash_n(m, strlen(m)) & (INDEX_CAP - 1);
        while (g_index[k]) k = (k + 1) & (INDEX_CAP - 1);
        g_index[k] = (int32_t)(i + 1);
    }
    memset(g_index_es, 0, sizeof g_index_es);
    for (int i = 0; i < g_i18n_es_count && i < INDEX_CAP / 2; i++) {
        const char* m = g_i18n_es[i].msgid;
        uint32_t k = hash_n(m, strlen(m)) & (INDEX_CAP - 1);
        while (g_index_es[k]) k = (k + 1) & (INDEX_CAP - 1);
        g_index_es[k] = (int32_t)(i + 1);
    }
    g_index_ready = true;
}

static const I18nEntry* find_n(const char* s, size_t n);
static const I18nEs* find_es_n(const char* s, size_t n) {
    build_index();
    uint32_t k = hash_n(s, n) & (INDEX_CAP - 1);
    while (g_index_es[k]) {
        const I18nEs* e = &g_i18n_es[g_index_es[k] - 1];
        if (strncmp(e->msgid, s, n) == 0 && e->msgid[n] == '\0') return e;
        k = (k + 1) & (INDEX_CAP - 1);
    }
    return NULL;
}

// (2026-11-25) the misses, when asked (REALENGINE_I18N_MISSING): msgids looked up in pt or es and not found
#define MISS_CAP 8192
static char* g_miss[MISS_CAP];
static int g_nmiss;
static char g_miss_path[600];
static void miss_write(void) {
    FILE* f = g_miss_path[0] ? fopen(g_miss_path, "wb") : NULL;
    if (!f) return;
    for (int i = 0; i < g_nmiss; i++) fprintf(f, "%s\n", g_miss[i]);
    fclose(f);
}
void i18n_record_missing(const char* path) {
    snprintf(g_miss_path, sizeof g_miss_path, "%s", path ? path : "");
    if (g_miss_path[0]) atexit(miss_write);
}
int i18n_missing_count(void) { return g_nmiss; } // (item 21, the UI sweep: the misses a control brought)
const char* i18n_missing_at(int i) { return i >= 0 && i < g_nmiss ? g_miss[i] : ""; }
static void miss(const char* s, size_t n) {
    if (!g_miss_path[0] || g_nmiss >= MISS_CAP || !n) return;
    bool letters = false;
    for (size_t i = 0; i < n && !letters; i++) letters = (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z');
    if (!letters) return;
    for (int i = 0; i < g_nmiss; i++)
        if (!strncmp(g_miss[i], s, n) && !g_miss[i][n]) return;
    char* c = malloc(n + 1); // (a test's run only)
    if (!c) return;
    memcpy(c, s, n), c[n] = 0;
    for (char* p = c; *p; p++)
        if (*p == '\n') *p = ' ';
    g_miss[g_nmiss++] = c;
}

// the text of msgid (n chars) in the current language; NULL when that language has none
static const char* lookup_n(const char* s, size_t n) {
    if (g_lang == LANG_ES) {
        const I18nEs* e = find_es_n(s, n);
        if (e && e->text && e->text[0]) return e->text;
    } else {
        const I18nEntry* e = find_n(s, n);
        if (e && e->text[0] && e->text[0][0]) return e->text[0];
    }
    miss(s, n);
    return NULL;
}

static const I18nEntry* find_n(const char* s, size_t n) {
    build_index();
    uint32_t k = hash_n(s, n) & (INDEX_CAP - 1);
    while (g_index[k]) {
        const I18nEntry* e = &g_i18n_entries[g_index[k] - 1];
        if (strncmp(e->msgid, s, n) == 0 && e->msgid[n] == '\0') return e;
        k = (k + 1) & (INDEX_CAP - 1);
    }
    return NULL;
}

static const char* entry_text(const I18nEntry* e) {
    if (g_lang <= LANG_EN || g_lang >= LANG_COUNT) return e->msgid;
    if (g_lang == LANG_ES) {
        const I18nEs* s = find_es_n(e->msgid, strlen(e->msgid));
        return s && s->text && s->text[0] ? s->text : e->msgid;
    }
    const char* t = e->text[0];
    return t && t[0] ? t : e->msgid;
}

// ---------------------------------------------------------------------------
// Built labels ("text##id", "text###msgid"), cached until the language changes
// ---------------------------------------------------------------------------

#define CACHE_CAP 2048
typedef struct CacheSlot { char* key; char* value; } CacheSlot;
static CacheSlot g_cache[CACHE_CAP];
static int g_cache_used;

static void cache_clear(void) {
    for (int i = 0; i < CACHE_CAP; i++) {
        free(g_cache[i].key);
        free(g_cache[i].value);
        g_cache[i].key = g_cache[i].value = NULL;
    }
    g_cache_used = 0;
}

// kind tells tr() and tr_id() apart for the same msgid.
static const char* cache_get(char kind, const char* s, const char* (*build)(const char*, char*, size_t)) {
    size_t n = strlen(s);
    uint32_t k = (hash_n(s, n) ^ (uint8_t)kind * 0x9E3779B9u) & (CACHE_CAP - 1);
    while (g_cache[k].key) {
        if (g_cache[k].key[0] == kind && strcmp(g_cache[k].key + 1, s) == 0) return g_cache[k].value;
        k = (k + 1) & (CACHE_CAP - 1);
    }
    char buf[2048]; // (a description passes through i18n_terms: up to a long paragraph)
    const char* value = build(s, buf, sizeof buf); // may insert into the cache itself (tr_id of a "##" label)
    if (value != buf) return value;
    if (g_cache_used >= CACHE_CAP * 3 / 4) {
        // Full (only translated labels get here, so it shouldn't happen): rotating buffers, valid for a few calls.
        static char spare[8][2048];
        static int next;
        char* out = spare[next];
        next = (next + 1) % 8;
        snprintf(out, sizeof spare[0], "%s", buf);
        return out;
    }
    k = (hash_n(s, n) ^ (uint8_t)kind * 0x9E3779B9u) & (CACHE_CAP - 1);
    while (g_cache[k].key) k = (k + 1) & (CACHE_CAP - 1);
    char* key = malloc(n + 2);
    char* val = malloc(strlen(buf) + 1);
    if (!key || !val) {
        free(key);
        free(val);
        return s;
    }
    key[0] = kind;
    memcpy(key + 1, s, n + 1);
    strcpy(val, buf);
    g_cache[k].key = key;
    g_cache[k].value = val;
    g_cache_used++;
    return val;
}

static const char* build_suffixed(const char* s, char* buf, size_t cap) {
    const char* hash = strstr(s, "##");
    const char* t = lookup_n(s, (size_t)(hash - s));
    if (!t) return s;
    snprintf(buf, cap, "%s%s", t, hash);
    return buf;
}

static const char* build_stable_id(const char* s, char* buf, size_t cap) {
    snprintf(buf, cap, "%s###%s", tr(s), s);
    return buf;
}

// ---------------------------------------------------------------------------
// (2026-11-25, item 13) "Blueprint" -> "Graph" / "Grafo" on screen
// ---------------------------------------------------------------------------

typedef struct TermRule { const char* from; const char* to; } TermRule;
// in order: the longer phrases first, then the word (whole words only)
static const TermRule g_terms_en[] = {
    { "Blueprint Function Library", "Graph Function Library" }, { "Blueprint Interface", "Graph Interface" },
    { "Blueprint Class", "Graph Class" }, { "Animation Blueprint", "Animation Graph" }, { "Anim Blueprint", "Anim Graph" },
    { "Widget Blueprint", "Widget Graph" }, { "Level Blueprint", "Level Graph" }, { "Blueprints", "Graphs" }, { "Blueprint", "Graph" },
    { "blueprints", "graphs" }, { "blueprint", "graph" }, { NULL, NULL } };
static const TermRule g_terms_pt[] = {
    { "Blueprint Function Library", "Graph Function Library" }, { "Blueprint Interface", "Graph Interface" },
    { "Blueprint Class", "Classe de Grafo" }, { "Animation Blueprint", "Animation Graph" }, { "Anim Blueprint", "Anim Graph" },
    { "Widget Blueprint", "Widget Graph" }, { "Level Blueprint", "Level Graph" },
    { "Classes Blueprint", "Classes de Grafo" }, { "Classe Blueprint", "Classe de Grafo" }, { "classe Blueprint", "classe de Grafo" },
    { "Interface Blueprint", "Interface de Grafo" }, { "Componentes Blueprint", "Componentes de Grafo" },
    { "Componente Blueprint", "Componente de Grafo" }, { "atores Blueprint", "atores de Grafo" }, { "ator Blueprint", "ator de Grafo" },
    { "Editor de Blueprint", "Editor de Grafo" }, { "Blueprints", "Grafos" }, { "Blueprint", "Grafo" },
    { "blueprints", "grafos" }, { "blueprint", "grafo" }, { NULL, NULL } };
static const TermRule g_terms_es[] = {
    { "Blueprint Function Library", "Graph Function Library" }, { "Blueprint Interface", "Graph Interface" },
    { "Blueprint Class", "Clase de Grafo" }, { "Animation Blueprint", "Animation Graph" }, { "Anim Blueprint", "Anim Graph" },
    { "Widget Blueprint", "Widget Graph" }, { "Level Blueprint", "Level Graph" },
    { "Clases Blueprint", "Clases de Grafo" }, { "Clase Blueprint", "Clase de Grafo" }, { "clase Blueprint", "clase de Grafo" },
    { "Interfaz Blueprint", "Interfaz de Grafo" }, { "macros de Blueprint", "macros de Grafo" }, { "Componente Blueprint", "Componente de Grafo" },
    { "actores Blueprint", "actores de Grafo" }, { "actor Blueprint", "actor de Grafo" }, { "Editor de Blueprint", "Editor de Grafo" },
    { "Blueprints", "Grafos" }, { "Blueprint", "Grafo" }, { "blueprints", "grafos" }, { "blueprint", "grafo" }, { NULL, NULL } };

static bool word_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static const char* build_terms(const char* s, char* buf, size_t cap) {
    const TermRule* rules = g_lang == LANG_PT_BR ? g_terms_pt : g_lang == LANG_ES ? g_terms_es : g_terms_en;
    size_t o = 0;
    for (const char* p = s; *p && o + 1 < cap;) {
        const TermRule* hit = NULL;
        if (p == s || !word_char(p[-1]))
            for (const TermRule* r = rules; r->from && !hit; r++) {
                size_t n = strlen(r->from);
                if (!strncmp(p, r->from, n) && !word_char(p[n])) hit = r;
            }
        if (hit) {
            o += (size_t)snprintf(buf + o, cap - o, "%s", hit->to);
            if (o >= cap) o = cap - 1;
            p += strlen(hit->from);
        } else buf[o++] = *p++;
    }
    buf[o] = 0;
    return buf;
}

bool g_i18n_raw_terms;
const char* i18n_terms(const char* s) {
    if (!s || g_i18n_raw_terms || !strstr(s, "lueprint")) return s; // (nearly every text: one search)
    return cache_get('t', s, build_terms);
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void i18n_set_language(int lang) {
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    build_index();
    if (lang != g_lang) cache_clear();
    g_lang = lang;
}

int i18n_language(void) { return g_lang; }

const char* i18n_language_name(int lang) {
    static const char* const names[LANG_COUNT] = { "English", "Português (Brasil)", "Español" };
    return lang >= 0 && lang < LANG_COUNT ? names[lang] : "?";
}

const char* i18n_language_code(int lang) {
    static const char* const codes[LANG_COUNT] = { "en_US", "pt_BR", "es_ES" };
    return lang >= 0 && lang < LANG_COUNT ? codes[lang] : "";
}

static int lang_code_eq(const char* a, const char* b, size_t n) { // (ASCII, case-insensitive; n = SIZE_MAX: to the end)
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

int i18n_language_from_code(const char* code) {
    // (2026-11-25, the tester's block of item 12) "pt_BR", "pt-BR", "pt", "PT" and "es", "en" all count: the short
    // code is the language's two letters, case and the separator don't matter
    if (!code || !code[0]) return -1;
    for (int i = 0; i < LANG_COUNT; i++) {
        const char* c = i18n_language_code(i);
        if (!lang_code_eq(code, c, 2)) continue;
        if (!code[2]) return i;
        if ((code[2] == '_' || code[2] == '-') && lang_code_eq(code + 3, c + 3, SIZE_MAX)) return i;
    }
    return -1;
}

const char* tr(const char* msgid) {
    if (!msgid || !msgid[0]) return msgid;
    if (g_lang == LANG_EN) return i18n_terms(msgid);
    const char* hash = strstr(msgid, "##");
    if (hash && hash != msgid) { // (a key with its ## in the table: as it is; else the visible part's text and the id)
        const I18nEntry* e = find_n(msgid, strlen(msgid));
        if (e) return i18n_terms(entry_text(e));
        return i18n_terms(cache_get('s', msgid, build_suffixed));
    }
    const char* t = lookup_n(msgid, strlen(msgid));
    return i18n_terms(t ? t : msgid);
}

const char* tr_lang(const char* msgid, int lang) {
    if (!msgid || !msgid[0] || lang <= LANG_EN || lang >= LANG_COUNT) return msgid;
    const char* t = NULL;
    if (lang == LANG_ES) {
        const I18nEs* s = find_es_n(msgid, strlen(msgid));
        t = s ? s->text : NULL;
    } else {
        const I18nEntry* e = find_n(msgid, strlen(msgid));
        t = e ? e->text[0] : NULL;
    }
    return t && t[0] ? t : msgid;
}

int i18n_system_language(void) {
#ifdef _WIN32
    unsigned short id = GetUserDefaultUILanguage();
    if ((id & 0x3FF) == 0x16) return LANG_PT_BR; // (Portuguese: Brazil's and Portugal's)
    if ((id & 0x3FF) == 0x0A) return LANG_ES;    // (Spanish: every country's)
#endif
    return LANG_EN;
}

const char* tr_id(const char* msgid) {
    if (!msgid || !msgid[0]) return msgid;
    return cache_get('i', msgid, build_stable_id);
}

const I18nEntry* i18n_table(int* count) {
    if (count) *count = g_i18n_entry_count;
    return g_i18n_entries;
}

bool i18n_has(const char* msgid) { return msgid && find_n(msgid, strlen(msgid)) != NULL; }
bool i18n_has_es(const char* msgid) { return msgid && find_es_n(msgid, strlen(msgid)) != NULL; }
const I18nEs* i18n_table_es(int* count) {
    if (count) *count = g_i18n_es_count;
    return g_i18n_es;
}

const char* i18n_msgid(const char* text) {
    if (!text || !text[0] || g_lang == LANG_EN) return text;
    if (g_lang == LANG_ES) {
        for (int i = 0; i < g_i18n_es_count; i++)
            if (g_i18n_es[i].text && strcmp(g_i18n_es[i].text, text) == 0) return g_i18n_es[i].msgid;
        return text;
    }
    for (int i = 0; i < g_i18n_entry_count; i++)
        if (strcmp(entry_text(&g_i18n_entries[i]), text) == 0) return g_i18n_entries[i].msgid;
    return text;
}
