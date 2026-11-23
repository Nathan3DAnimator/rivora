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

static int g_lang = LANG_EN;

// ---------------------------------------------------------------------------
// msgid -> entry: open addressing over the static table, built on first use
// ---------------------------------------------------------------------------

#define INDEX_CAP 16384 // power of two, more than twice the table (8192 until Q7: the table passed 4096) (a full index would make lookups of missing keys loop)
static int32_t g_index[INDEX_CAP]; // entry + 1, 0 = empty
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
    g_index_ready = true;
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
    const char* t = e->text[g_lang - 1];
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
    char buf[512];
    const char* value = build(s, buf, sizeof buf); // may insert into the cache itself (tr_id of a "##" label)
    if (value != buf) return value;
    if (g_cache_used >= CACHE_CAP * 3 / 4) {
        // Full (only translated labels get here, so it shouldn't happen): rotating buffers, valid for a few calls.
        static char spare[8][512];
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
    const I18nEntry* e = find_n(s, (size_t)(hash - s));
    if (!e) return s;
    snprintf(buf, cap, "%s%s", entry_text(e), hash);
    return buf;
}

static const char* build_stable_id(const char* s, char* buf, size_t cap) {
    snprintf(buf, cap, "%s###%s", tr(s), s);
    return buf;
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
    static const char* const names[LANG_COUNT] = { "English", "Português (Brasil)" };
    return lang >= 0 && lang < LANG_COUNT ? names[lang] : "?";
}

const char* i18n_language_code(int lang) {
    static const char* const codes[LANG_COUNT] = { "en_US", "pt_BR" };
    return lang >= 0 && lang < LANG_COUNT ? codes[lang] : "";
}

int i18n_language_from_code(const char* code) {
    for (int i = 0; i < LANG_COUNT; i++)
        if (code && strcmp(code, i18n_language_code(i)) == 0) return i;
    return -1;
}

const char* tr(const char* msgid) {
    if (!msgid || !msgid[0] || g_lang == LANG_EN) return msgid;
    const I18nEntry* e = find_n(msgid, strlen(msgid));
    if (e) return entry_text(e);
    const char* hash = strstr(msgid, "##");
    if (hash && hash != msgid) return cache_get('s', msgid, build_suffixed);
    return msgid;
}

const char* tr_lang(const char* msgid, int lang) {
    if (!msgid || !msgid[0] || lang <= LANG_EN || lang >= LANG_COUNT) return msgid;
    const I18nEntry* e = find_n(msgid, strlen(msgid));
    const char* t = e ? e->text[lang - 1] : NULL;
    return t && t[0] ? t : msgid;
}

int i18n_system_language(void) {
#ifdef _WIN32
    unsigned short id = GetUserDefaultUILanguage();
    if ((id & 0x3FF) == 0x16) return LANG_PT_BR; // (Portuguese: Brazil's and Portugal's)
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

const char* i18n_msgid(const char* text) {
    if (!text || !text[0] || g_lang == LANG_EN) return text;
    for (int i = 0; i < g_i18n_entry_count; i++)
        if (strcmp(entry_text(&g_i18n_entries[i]), text) == 0) return g_i18n_entries[i].msgid;
    return text;
}
