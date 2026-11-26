// The Assistant's search and its knowledge base (assistant.h, notas-dev/assistente.md).
// A base is built whole (the built-in sources + the downloaded articles) on a worker and swapped in on the main thread;
// a search reads the current one. Terms: FNV hashes of the stems, per document sorted (a binary search each), the
// document frequency in an open-addressing table, the vocabulary's words kept for the typos' trigram match.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assistant.h"
#include "async.h"
#include "endpoints.h"
#include "i18n.h"
#include "io_json.h"
#include "net_http.h"
#include "platform.h"
#include "platform_files.h"
#include "script.h"
#include "ui_tips.h"
#include "zip_read.h"

typedef struct Term { uint32_t h; float tf; } Term;
typedef struct Word { uint32_t h; char w[24]; uint32_t grams[24]; uint8_t ng; } Word; // (a vocabulary word and its trigrams)

typedef struct Base {
    AsDoc* docs;
    int ndocs, cap_docs;
    Term* terms;
    uint32_t nterms, cap_terms;
    // document frequency: hash -> count (open addressing; 0 = empty)
    uint32_t* df_h;
    uint32_t* df_n;
    uint32_t df_cap;
    Word* vocab;
    uint32_t nvocab, cap_vocab;
    // synonyms: per group the stems' hashes
    uint32_t* syn;   // all groups' hashes, back to back
    uint32_t* syn_g; // per hash in syn: its group
    uint32_t nsyn;
    float avg_len;
    char** strings; // the texts the documents point to (freed with the base)
    uint32_t nstrings, cap_strings;
    int narticles;
    char version[32];
    uint32_t bytes;
    double build_ms;
    char* raw;   // (2026-11-25) the index.json it was built from (built again when the editor's language changes)
    size_t raw_len;
    int lang;    // the editor's language it was built in (Spanish: the Spanish sections indexed too)
} Base;

static struct {
    Base* base;
    AsState state;
    char error[200];
    bool init;
    AsStats stats;
} S;

// --- Text: the words (lower case, no accents), the stop words, the stems ---
static char fold(const unsigned char** p) { // one character of UTF-8 as a plain lower-case letter or digit (0: other)
    unsigned char c = **p;
    if (c < 0x80) {
        (*p)++;
        return isalnum(c) ? (char)tolower(c) : 0;
    }
    if (c == 0xC3 && (*p)[1]) { // Latin-1 letters
        unsigned char d = (*p)[1];
        *p += 2;
        static const char map[64] = "aaaaaaaceeeeiiiidnooooo/ouuuuyps" "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
        char m = map[(d - 0x80) & 63];
        return m == '/' ? 0 : m;
    }
    // other multibyte characters: skipped whole
    (*p)++;
    while ((**p & 0xC0) == 0x80) (*p)++;
    return 0;
}

static const char* const g_stop[] = {
    "a", "o", "os", "as", "um", "uma", "uns", "umas", "de", "do", "da", "dos", "das", "no", "na", "nos", "nas", "em", "para",
    "pra", "por", "que", "e", "eu", "meu", "minha", "se", "com", "qual", "quais", "onde", "como", "posso", "consigo", "faco",
    "fazer", "faz", "ao", "aos", "ou", "isso", "esse", "essa", "este", "esta", "tem", "ter", "ha", "the", "an", "to", "in",
    "of", "my", "can", "what", "where", "is", "it", "how", "do", "does", "i", "and", "or", "for", "with", "on", "at", "make",
    "el", "la", "los", "las", "un", "una", "del", "al", "en", "y", "mi", "puedo", "hacer", "hago", "que", "quero", "want",
    "preciso", "need", "algo", "something", "coisa", "jeito", "modo", "forma",
    // the way people talk (Portuguese): fillers, slang, abbreviations
    "tipo", "assim", "ai", "ae", "ne", "mano", "cara", "velho", "veio", "po", "ta", "to", "vc", "vcs", "voce", "voces", "pq", "porque",
    "oq", "q", "eh", "tb", "tbm", "tambem", "msm", "mesmo", "entao", "dai", "ja", "so", "bem", "muito", "mt", "mto", "ainda",
    "agora", "aqui", "la", "isto", "aquilo", "alguem", "gente", "nos", "me", "mim", "queria", "gostaria", "da", "dar", "pode",
    "podem", "poderia", "seria", "sera", "favor", "obrigado", "valeu", "vlw", "oi", "ola",
    "alguma", "algum", "nao", "sei", "saber", "sabe", "ajuda", "ajudar", "duvida", "pergunta", "quando", "porfavor", "ate",
    "seu", "sua", "meus", "minhas", "pro", "pros", "num", "numa", "dum", "duma", "pelo", "pela", "sobre", "toda", "todo",
    // English: fillers, slang
    "hello", "hi", "hey", "please", "pls", "plz", "thanks", "thx", "guys", "bro", "dude", "like", "just", "really", "wanna",
    "gonna", "gotta", "im", "ur", "u", "you", "your", "yo", "be", "am", "are", "was", "this", "that", "there", "some", "any",
    "get", "got", "know", "help", "question", "way", "would", "could", "should", "will", "into", "from", "about", "when",
    // Spanish
    "quiero", "necesito", "hola", "oye", "tio", "por", "favor", "gracias", "usted", "tengo", "puede", "lo", "le", "es", "son",
};
static bool stop(const char* w) {
    for (size_t i = 0; i < sizeof g_stop / sizeof g_stop[0]; i++)
        if (!strcmp(w, g_stop[i])) return true;
    return false;
}
static bool ends(const char* w, size_t n, const char* s) {
    size_t k = strlen(s);
    return n > k + 2 && !memcmp(w + n - k, s, k);
}
// a light stem (Portuguese, Spanish, English): the plural, the verb endings, a last vowel
static void stem(char* w) {
    size_t n = strlen(w);
    static const char* const suf[] = { "amentos", "amento", "imentos", "imento", "mente", "acoes", "icoes", "acao", "icao", "ando",
                                       "endo", "indo", "aram", "eram", "iram", "ados", "idos", "adas", "idas", "ado", "ido", "ada",
                                       "ida", "ing", "ers", "er", "ed", "ar", "ir", "es", "s" };
    for (size_t i = 0; i < sizeof suf / sizeof suf[0]; i++)
        if (ends(w, n, suf[i])) {
            n -= strlen(suf[i]);
            w[n] = 0;
            break;
        }
    if (n > 3 && (w[n - 1] == 'a' || w[n - 1] == 'e' || w[n - 1] == 'o')) w[--n] = 0;
}
static uint32_t hash(const char* w) {
    uint32_t h = 2166136261u;
    for (; *w; w++) h = (h ^ (uint8_t)*w) * 16777619u;
    return h ? h : 1u;
}

// The words of a text, each stemmed (stop words dropped): fn(word, stem, user). camelCase splits too (PlaySound2D).
typedef void (*WordFn)(const char* word, const char* st, void* user);
static void words(const char* text, WordFn fn, void* user) {
    if (!text) return;
    const unsigned char* p = (const unsigned char*)text;
    char w[48];
    int n = 0;
    unsigned char prev = 0;
    while (*p) {
        unsigned char raw = *p;
        bool split = n > 0 && isupper(raw) && islower(prev); // (camelCase)
        char c = fold(&p);
        if (!c || split || n >= (int)sizeof w - 1) {
            if (n) {
                w[n] = 0;
                if (!stop(w) && (n > 1 || isdigit((unsigned char)w[0]))) {
                    char st[48];
                    memcpy(st, w, (size_t)n + 1);
                    stem(st);
                    fn(w, st, user);
                }
                n = 0;
            }
            if (split && c) w[n++] = c;
            prev = raw;
            continue;
        }
        w[n++] = c;
        prev = raw;
    }
    if (n) {
        w[n] = 0;
        if (!stop(w) && (n > 1 || isdigit((unsigned char)w[0]))) {
            char st[48];
            memcpy(st, w, (size_t)n + 1);
            stem(st);
            fn(w, st, user);
        }
    }
}

// --- Building a base ---
static void* grow(void* p, uint32_t* cap, uint32_t need, size_t size) {
    if (need <= *cap) return p;
    uint32_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void* q = realloc(p, (size_t)c * size);
    if (!q) return NULL;
    *cap = c;
    return q;
}
static const char* keep(Base* b, const char* s) {
    if (!s) return "";
    uint32_t cap = b->cap_strings;
    char** q = grow(b->strings, &cap, b->nstrings + 1, sizeof *q);
    if (!q) return "";
    b->strings = q, b->cap_strings = cap;
    size_t n = strlen(s);
    char* c = malloc(n + 1);
    if (!c) return "";
    memcpy(c, s, n + 1);
    b->strings[b->nstrings++] = c;
    return c;
}

static void trigrams(Word* v) {
    char pad[32];
    snprintf(pad, sizeof pad, " %s ", v->w);
    size_t n = strlen(pad);
    v->ng = 0;
    for (size_t i = 0; i + 3 <= n && v->ng < 24; i++) v->grams[v->ng++] = ((uint32_t)(uint8_t)pad[i] << 16) | ((uint32_t)(uint8_t)pad[i + 1] << 8) | (uint8_t)pad[i + 2];
}
static uint32_t df_slot(Base* b, uint32_t h) {
    uint32_t m = b->df_cap - 1, i = (h * 2654435761u) & m;
    while (b->df_h[i] && b->df_h[i] != h) i = (i + 1) & m;
    return i;
}
static uint32_t df_of(const Base* b, uint32_t h) {
    if (!b->df_cap) return 0;
    uint32_t m = b->df_cap - 1, i = (h * 2654435761u) & m;
    while (b->df_h[i]) {
        if (b->df_h[i] == h) return b->df_n[i];
        i = (i + 1) & m;
    }
    return 0;
}

typedef struct Coll { Base* b; float weight; Term* t; uint32_t n, cap; } Coll;
static void coll_word(const char* word, const char* st, void* user) {
    Coll* c = user;
    uint32_t h = hash(st);
    for (uint32_t i = 0; i < c->n; i++)
        if (c->t[i].h == h) {
            c->t[i].tf += c->weight;
            return;
        }
    if (c->n == c->cap) {
        uint32_t cap = c->cap ? c->cap * 2 : 64;
        Term* t = realloc(c->t, sizeof *t * cap);
        if (!t) return;
        c->t = t, c->cap = cap;
    }
    c->t[c->n++] = (Term){ h, c->weight };
    // the vocabulary (the typos' match): the stem, spelled as its first word
    Base* b = c->b;
    (void)word;
    if (strlen(st) < 3 || strlen(st) >= sizeof b->vocab[0].w) return;
    for (uint32_t i = b->nvocab; i-- > 0 && i + 4096 > b->nvocab;) // (recent words: a cheap duplicate check, the exact one below)
        if (b->vocab[i].h == h) return;
    uint32_t cap = b->cap_vocab;
    Word* v = grow(b->vocab, &cap, b->nvocab + 1, sizeof *v);
    if (!v) return;
    b->vocab = v, b->cap_vocab = cap;
    Word* nw = &b->vocab[b->nvocab++];
    nw->h = h;
    snprintf(nw->w, sizeof nw->w, "%s", st);
    trigrams(nw);
}
static int by_hash(const void* a, const void* b) {
    uint32_t x = ((const Term*)a)->h, y = ((const Term*)b)->h;
    return x < y ? -1 : x > y;
}

// a document: its fields' words with their weights (title, keys 3; the rest 1)
static void add_doc(Base* b, AsDoc d, const char* const* fields, const float* weights, int nf) {
    uint32_t cap = (uint32_t)b->cap_docs;
    AsDoc* docs = grow(b->docs, &cap, (uint32_t)b->ndocs + 1, sizeof *docs);
    if (!docs) return;
    b->docs = docs, b->cap_docs = (int)cap;
    Coll c = { b, 1.0f, NULL, 0, 0 };
    for (int i = 0; i < nf; i++) c.weight = weights[i], words(fields[i], coll_word, &c);
    qsort(c.t, c.n, sizeof *c.t, by_hash);
    uint32_t tc = b->cap_terms;
    Term* t = grow(b->terms, &tc, b->nterms + c.n, sizeof *t);
    if (!t) return free(c.t);
    b->terms = t, b->cap_terms = tc;
    d.t0 = b->nterms, d.nt = c.n, d.len = 0.0f;
    for (uint32_t i = 0; i < c.n; i++) b->terms[b->nterms++] = c.t[i], d.len += c.t[i].tf;
    free(c.t);
    b->docs[b->ndocs++] = d;
}

static void synonyms(Base* b, const JsonValue* groups) {
    uint32_t total = 0;
    for (size_t g = 0; g < json_len(groups); g++) total += (uint32_t)json_len(json_at(groups, g));
    b->syn = calloc(total + 1, sizeof *b->syn), b->syn_g = calloc(total + 1, sizeof *b->syn_g);
    if (!b->syn || !b->syn_g) return;
    for (size_t g = 0; g < json_len(groups); g++) {
        const JsonValue* gr = json_at(groups, g);
        for (size_t k = 0; k < json_len(gr); k++) {
            const char* w = json_str(json_at(gr, k), "");
            if (strchr(w, ' ')) continue; // (a phrase: its words are searched as they are)
            Coll c = { b, 1.0f, NULL, 0, 0 };
            words(w, coll_word, &c);
            if (c.n == 1) b->syn[b->nsyn] = c.t[0].h, b->syn_g[b->nsyn++] = (uint32_t)g;
            free(c.t);
        }
    }
}

// The whole base: the built-in sources and the articles of json (NULL: none). Any thread (it reads the API and the
// descriptions, made once at the start).
static Base* build(const char* json, size_t len, char* err, size_t err_cap) {
    double t0 = re_time_seconds();
    Base* b = calloc(1, sizeof *b);
    if (!b) return NULL;
    JsonDoc doc;
    bool have = false;
    if (json) {
        char e[160];
        if (!json_parse(json, len, &doc, e, sizeof e)) snprintf(err, err_cap, "index.json: %s", e);
        else if (strcmp(json_get_str(&doc.root, "format", ""), "RealEngine Assistant")) snprintf(err, err_cap, "index.json: not the Assistant's");
        else have = true;
    }
    if (have) {
        const JsonValue* arts = json_get(&doc.root, "articles");
        for (size_t i = 0; i < json_len(arts); i++) {
            const JsonValue* a = json_at(arts, i);
            const JsonValue* t = json_get(a, "title");
            const JsonValue* bd = json_get(a, "body");
            const char* kind = json_get_str(a, "kind", "howto");
            AsDoc d = { AS_SRC_ARTICLE, !strcmp(kind, "recipe") ? AS_KIND_RECIPE : !strcmp(kind, "error") ? AS_KIND_ERROR
                                       : !strcmp(kind, "concept") ? AS_KIND_CONCEPT : AS_KIND_HOWTO };
            d.id = keep(b, json_get_str(a, "id", ""));
            d.title[0] = keep(b, json_get_str(t, "en", "")), d.title[1] = keep(b, json_get_str(t, "pt", ""));
            d.body[0] = keep(b, json_get_str(bd, "en", "")), d.body[1] = keep(b, json_get_str(bd, "pt", ""));
            d.title[2] = keep(b, json_get_str(t, "es", json_get_str(t, "en", ""))); // (an older base without "es": the English)
            d.body[2] = keep(b, json_get_str(bd, "es", json_get_str(bd, "en", "")));
            char keys[1024] = "", match[512] = "";
            const JsonValue* ks = json_get(a, "keys");
            for (size_t k = 0; k < json_len(ks); k++) {
                size_t n = strlen(keys);
                snprintf(keys + n, sizeof keys - n, "%s%s", n ? " " : "", json_str(json_at(ks, k), ""));
            }
            const JsonValue* ms = json_get(a, "match");
            for (size_t k = 0; k < json_len(ms); k++) {
                size_t n = strlen(match);
                snprintf(match + n, sizeof match - n, "%s%s", n ? "|" : "", json_str(json_at(ms, k), ""));
            }
            d.extra = keep(b, match);
            // (its Spanish: indexed in a Spanish editor only, so the Portuguese and English answers rank as before)
            const char* f[8] = { d.title[0], d.title[1], keys, d.id, d.body[0], d.body[1], d.title[2], d.body[2] };
            static const float wt[8] = { 3.0f, 3.0f, 3.0f, 2.0f, 1.0f, 1.0f, 3.0f, 1.0f };
            add_doc(b, d, f, wt, i18n_language() == LANG_ES ? 8 : 6);
            b->narticles++;
        }
        synonyms(b, json_get(&doc.root, "synonyms"));
        b->bytes = (uint32_t)len;
        b->raw = malloc(len + 1); // (a copy: built again in another language)
        if (b->raw) memcpy(b->raw, json, len), b->raw[len] = 0, b->raw_len = len;
    }
    // the engine's functions: the name's words, the category, the description
    for (uint32_t i = 0; i < script_api_count(); i++) {
        const ScriptApi* api = script_api_at(i);
        AsDoc d = { AS_SRC_API, AS_KIND_CONCEPT };
        d.id = keep(b, api->name);
        d.title[0] = d.title[1] = d.title[2] = d.id;
        d.body[0] = d.body[1] = d.body[2] = keep(b, api->doc);
        d.extra = keep(b, api->sig);
        const char* f[3] = { api->name, api->category, api->doc };
        static const float wt[3] = { 3.0f, 1.5f, 1.0f };
        add_doc(b, d, f, wt, 3);
    }
    // every control's description (the label in English and as translated)
    int ntips = 0;
    const UiTip* tips = ui_tips_table(&ntips);
    for (int i = 0; i < ntips; i++) {
        AsDoc d = { AS_SRC_CONTROL, AS_KIND_CONCEPT };
        d.id = keep(b, tips[i].label);
        d.title[0] = d.id, d.title[1] = keep(b, TR(tips[i].label));
        d.body[0] = keep(b, tips[i].text[0]), d.body[1] = keep(b, tips[i].text[1] ? tips[i].text[1] : tips[i].text[0]);
        const char* es = ui_tip_es(tips[i].label); // (2026-11-25: the Spanish label and description)
        d.title[2] = keep(b, tr_lang(tips[i].label, LANG_ES)), d.body[2] = keep(b, es && es[0] ? es : tips[i].text[0]);
        // (their Spanish indexed only in a Spanish editor: 2400 descriptions' Spanish words moved the Portuguese answers'
        // weights - "mapa" in hundreds of them)
        const char* f[6] = { d.title[0], d.title[1], d.body[0], d.body[1], d.title[2], d.body[2] };
        static const float wt[6] = { 3.0f, 3.0f, 1.0f, 1.0f, 3.0f, 1.0f };
        add_doc(b, d, f, wt, i18n_language() == LANG_ES ? 6 : 4);
    }
    // the document frequencies, the mean length
    b->df_cap = 1;
    while (b->df_cap < b->nterms * 2 + 16) b->df_cap *= 2;
    b->df_h = calloc(b->df_cap, sizeof *b->df_h), b->df_n = calloc(b->df_cap, sizeof *b->df_n);
    double total = 0.0;
    for (int i = 0; b->df_h && b->df_n && i < b->ndocs; i++) {
        const AsDoc* d = &b->docs[i];
        total += d->len;
        for (uint32_t k = 0; k < d->nt; k++) {
            uint32_t s = df_slot(b, b->terms[d->t0 + k].h);
            b->df_h[s] = b->terms[d->t0 + k].h, b->df_n[s]++;
        }
    }
    b->avg_len = b->ndocs ? (float)(total / b->ndocs) : 1.0f;
    if (have) {
        snprintf(b->version, sizeof b->version, "%08x", re_crc32(0, json, len));
        json_free(&doc);
    }
    b->build_ms = (re_time_seconds() - t0) * 1000.0;
    return b;
}

static void base_free(Base* b) {
    if (!b) return;
    for (uint32_t i = 0; i < b->nstrings; i++) free(b->strings[i]);
    free(b->strings), free(b->docs), free(b->terms), free(b->df_h), free(b->df_n), free(b->vocab), free(b->syn), free(b->syn_g);
    free(b->raw);
    free(b);
}

static void install(Base* b) {
    base_free(S.base);
    b->lang = i18n_language();
    S.base = b;
    S.stats.docs = (uint32_t)b->ndocs, S.stats.terms = b->nterms, S.stats.vocab = b->nvocab, S.stats.build_ms = b->build_ms;
}

// --- Where the base is ---
const char* assistant_source_url(void) {
    const char* env = getenv("REALENGINE_ASSISTANT_URL");
    if (env && env[0]) return env;
    const char* u = endpoints_get("assistant", "base_url");
    return u ? u : "";
}
static bool offline_path(char* out, size_t cap) {
    const char* env = getenv("REALENGINE_ASSISTANT_DIR"); // (the test's folder: not the user's)
    char dir[512];
    if (env && env[0]) snprintf(dir, sizeof dir, "%s", env);
    else if (re_config_dir(dir, sizeof dir)) {
        size_t n = strlen(dir);
        snprintf(dir + n, sizeof dir - n, "\\assistant");
    } else return false;
    snprintf(out, cap, "%s\\index.json", dir);
    return true;
}
static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (64l << 20) ? malloc((size_t)n + 1) : NULL;
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) free(t), t = NULL;
    fclose(f);
    if (t) t[n] = 0, *len = (size_t)n;
    return t;
}

bool assistant_offline_present(void) {
    char p[600];
    return offline_path(p, sizeof p) && re_file_stat(p, NULL, NULL, NULL);
}

// (2026-11-25, bug-fixer) The offline base built on a worker when the editor starts (assistant_init_async): opening the
// Assistant built it in that frame, 0.4 s in the UI sweep. One who asks before it is in (the tests) waits for the worker
// and puts it in place (the same Base, never built twice)
static struct {
    uint32_t id;
    volatile int32_t ready;
    Base* b;
    char err[200];
} L;
static void local_work(AsyncTask* t, void* arg) {
    (void)t, (void)arg;
    char p[600];
    size_t len = 0;
    char* text = offline_path(p, sizeof p) ? read_all(p, &len) : NULL;
    L.b = build(text, len, L.err, sizeof L.err);
    free(text);
    re_atomic_store(&L.ready, 1);
}
static void local_install(void) { // (the main thread: the worker's base in, once)
    if (!re_atomic_load(&L.ready)) return;
    if (L.b) {
        if (!S.base) install(L.b), S.state = S.base && S.base->narticles ? AS_OFFLINE : AS_NONE;
        else base_free(L.b);
        L.b = NULL;
    }
    if (L.err[0]) snprintf(S.error, sizeof S.error, "%s", L.err), L.err[0] = 0;
    L.id = 0;
}
static void local_done(AsyncTask* t, void* arg, bool ok) {
    (void)t, (void)arg, (void)ok;
    local_install();
}
void assistant_init_async(void) {
    if (S.init || L.id || !async_ready()) return;
    S.init = true;
    re_atomic_store(&L.ready, 0);
    L.id = async_run(TR("Assistant: reading its base"), local_work, local_done, NULL, ASYNC_QUIET | ASYNC_HIDDEN);
}

void assistant_init(void) {
    if (L.id || re_atomic_load(&L.ready)) { // (the start's worker: waited for, its base put in place here)
        while (!re_atomic_load(&L.ready)) re_sleep_seconds(0.002);
        local_install();
    }
    if (S.init) return;
    S.init = true;
    char p[600], err[200] = "";
    size_t len = 0;
    char* text = offline_path(p, sizeof p) ? read_all(p, &len) : NULL;
    Base* b = build(text, len, err, sizeof err);
    free(text);
    if (b) install(b);
    S.state = S.base && S.base->narticles ? AS_OFFLINE : AS_NONE;
    if (err[0]) snprintf(S.error, sizeof S.error, "%s", err);
}

// (2026-11-25) the editor's language changed: the base built again (its Spanish sections in or out of the index)
void assistant_relang(void) {
    if (!S.base || S.base->lang == i18n_language()) return;
    char err[200] = "";
    Base* b = build(S.base->raw, S.base->raw_len, err, sizeof err);
    if (b) {
        int narts = S.base->narticles;
        install(b);
        if (!b->narticles && narts) S.state = AS_NONE;
    }
}

void assistant_shutdown(void) {
    base_free(S.base);
    memset(&S, 0, sizeof S);
}

// --- Fetching (async.h: a worker downloads and builds; the main thread swaps it in) ---
typedef struct Fetch { bool keep; Base* b; char err[200]; } Fetch;
static void fetch_work(AsyncTask* t, void* arg) {
    Fetch* f = arg;
    const char* base = assistant_source_url();
    if (!base[0]) {
        snprintf(f->err, sizeof f->err, "%s", TR("No address for the Assistant's base (endpoints.ini [assistant])."));
        async_fail(t, "%s", f->err);
        return;
    }
    char url[1200];
    snprintf(url, sizeof url, "%s/index.json", base);
    char* text = NULL;
    size_t len = 0;
    if (!strncmp(base, "http://", 7) || !strncmp(base, "https://", 8)) {
        async_status(t, "%s", url);
        HttpRequest rq = { 0 };
        rq.url = url, rq.timeout_ms = 15000;
        HttpResponse rs = { 0 };
        if (http_request(&rq, &rs) && rs.status == 200 && rs.body) text = (char*)rs.body, len = rs.len, rs.body = NULL;
        else snprintf(f->err, sizeof f->err, "%s (%d %s)", TR("The Assistant's base could not be fetched"), rs.status, rs.error);
        http_response_free(&rs);
    } else {
        text = read_all(url, &len);
        if (!text) snprintf(f->err, sizeof f->err, "%s: %s", TR("The Assistant's base could not be read"), url);
    }
    if (!text) {
        async_fail(t, "%s", f->err);
        return;
    }
    async_status(t, "%s", TR("Indexing the articles..."));
    f->b = build(text, len, f->err, sizeof f->err);
    if (f->b && !f->b->narticles) {
        if (!f->err[0]) snprintf(f->err, sizeof f->err, "%s", TR("The base has no articles."));
        base_free(f->b), f->b = NULL;
    }
    if (f->b && f->keep) { // offline: written to the computer (the next starts read it)
        char p[600];
        if (offline_path(p, sizeof p)) {
            char dir[600];
            snprintf(dir, sizeof dir, "%s", p);
            char* cut = strrchr(dir, '\\');
            if (cut) *cut = 0, re_make_dirs(dir);
            FILE* o = re_fopen(p, "wb");
            if (o) fwrite(text, 1, len, o), fclose(o);
            else snprintf(f->err, sizeof f->err, "%s %s", TR("Could not write"), p);
        }
    }
    free(text);
    if (!f->b) async_fail(t, "%s", f->err);
}
static void fetch_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    Fetch* f = arg;
    if (ok && f->b) {
        install(f->b);
        S.state = f->keep ? AS_OFFLINE : AS_ONLINE;
        S.error[0] = 0;
    } else {
        base_free(f->b);
        if (S.state == AS_FETCHING) S.state = S.base && S.base->narticles ? (assistant_offline_present() ? AS_OFFLINE : AS_ONLINE) : AS_FAILED;
        snprintf(S.error, sizeof S.error, "%s", f->err[0] ? f->err : TR("Cancelled"));
    }
    free(f);
}
void assistant_fetch(bool keep_offline) {
    if (S.state == AS_FETCHING) return;
    Fetch* f = calloc(1, sizeof *f);
    if (!f) return;
    f->keep = keep_offline;
    S.state = AS_FETCHING;
    async_run(keep_offline ? TR("Assistant: downloading the base") : TR("Assistant: fetching the base"), fetch_work, fetch_done, f,
              ASYNC_QUIET | ASYNC_CANCELLABLE);
}
void assistant_remove_offline(void) {
    char p[600];
    if (offline_path(p, sizeof p)) re_remove_file(p);
    if (S.state == AS_OFFLINE) S.state = S.base && S.base->narticles ? AS_ONLINE : AS_NONE;
}

AsState assistant_state(void) { return S.state; }
const char* assistant_version(void) { return S.base ? S.base->version : ""; }
int assistant_article_count(void) { return S.base ? S.base->narticles : 0; }
uint32_t assistant_base_bytes(void) { return S.base ? S.base->bytes : 0; }
const char* assistant_error(void) { return S.error; }
const AsDoc* assistant_doc(int i) { return S.base && i >= 0 && i < S.base->ndocs ? &S.base->docs[i] : NULL; }
int assistant_doc_count(void) { return S.base ? S.base->ndocs : 0; }
AsStats assistant_stats(void) { return S.stats; }

int assistant_find_article(const char* id) {
    for (int i = 0; S.base && i < S.base->ndocs; i++)
        if (S.base->docs[i].src == AS_SRC_ARTICLE && !strcmp(S.base->docs[i].id, id)) return i;
    return -1;
}

int assistant_explain_error(const char* message) {
    if (!S.base || !message) return -1;
    for (int i = 0; i < S.base->ndocs; i++) {
        const AsDoc* d = &S.base->docs[i];
        if (d->src != AS_SRC_ARTICLE || d->kind != AS_KIND_ERROR || !d->extra[0]) continue;
        char pats[512];
        snprintf(pats, sizeof pats, "%s", d->extra);
        for (char* tok = strtok(pats, "|"); tok; tok = strtok(NULL, "|"))
            if (strstr(message, tok)) return i;
    }
    return -1;
}

// --- The search ---
typedef struct QTerm { uint32_t h; float w; } QTerm;
typedef struct Query { QTerm t[48]; int n; const Base* b; } Query;
static void q_add(Query* q, uint32_t h, float w) {
    for (int i = 0; i < q->n; i++)
        if (q->t[i].h == h) {
            if (w > q->t[i].w) q->t[i].w = w;
            return;
        }
    if (q->n < 48) q->t[q->n++] = (QTerm){ h, w };
}
static void q_synonyms(Query* q, uint32_t h, float w) {
    const Base* b = q->b;
    for (uint32_t i = 0; i < b->nsyn; i++) {
        if (b->syn[i] != h) continue;
        for (uint32_t k = 0; k < b->nsyn; k++)
            if (b->syn_g[k] == b->syn_g[i]) q_add(q, b->syn[k], w * 0.75f);
    }
}
static void q_word(const char* word, const char* st, void* user) {
    (void)word;
    Query* q = user;
    const Base* b = q->b;
    uint32_t h = hash(st);
    if (df_of(b, h) || !b->nvocab) {
        q_add(q, h, 1.0f);
        q_synonyms(q, h, 1.0f);
        return;
    }
    // not a word of the base: the closest by trigrams (a typo: "pakage", "empacotra")
    Word w = { 0 };
    snprintf(w.w, sizeof w.w, "%s", st);
    trigrams(&w);
    float best = 0.0f;
    uint32_t bh = 0, bdf = 0;
    for (uint32_t i = 0; i < b->nvocab; i++) {
        const Word* v = &b->vocab[i];
        int common = 0;
        for (int x = 0; x < w.ng; x++)
            for (int y = 0; y < v->ng; y++)
                if (w.grams[x] == v->grams[y]) {
                    common++;
                    break;
                }
        float j = (float)common / (float)(w.ng + v->ng - common);
        // (a tie: the more common word, the likelier one meant; not the first the vocabulary lists)
        if (j > best + 1e-6f || (j > best - 1e-6f && j > 0.0f && df_of(b, v->h) > bdf)) best = j, bh = v->h, bdf = df_of(b, v->h);
    }
    if (getenv("REALENGINE_ASSISTANT_DEBUG")) // (which word a typo became)
        for (uint32_t i = 0; i < b->nvocab; i++)
            if (b->vocab[i].h == bh) printf("[RealEngine] Assistant: \"%s\" -> \"%s\" (%.2f, df %u)\n", st, b->vocab[i].w, best, bdf);
    if (best >= 0.35f) { // (a letter missing from a short word scores ~0.38: "pakage" -> package)
        q_add(q, bh, 0.8f);
        q_synonyms(q, bh, 0.8f);
    } else
        q_add(q, h, 1.0f); // (kept: it counts as unmatched)
}

static int by_score(const void* a, const void* b) {
    float x = ((const AsHit*)a)->score, y = ((const AsHit*)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

int assistant_search(const char* query, AsHit* out, int cap, bool* weak) {
    if (weak) *weak = true;
    if (!S.base || !query || !query[0] || cap <= 0) return 0;
    double t0 = re_time_seconds();
    const Base* b = S.base;
    Query q = { { { 0 } }, 0, b };
    words(query, q_word, &q);
    int err_doc = assistant_explain_error(query); // (a compiler's message pasted: its article first)
    // the question is a name (a function's, a control's: "PlaySound2D", "play_sound_2d", "Simulate Physics"): that one first
    char qn[96];
    size_t qk = 0;
    for (const char* c = query; *c && qk + 1 < sizeof qn; c++)
        if (isalnum((unsigned char)*c)) qn[qk++] = (char)tolower((unsigned char)*c);
    qn[qk] = 0;
    int n = 0;
    AsHit* all = malloc(sizeof *all * (size_t)b->ndocs);
    if (!all) return 0;
    const float k1 = 1.2f, bb = 0.75f, N = (float)b->ndocs;
    int matched_terms = 0;
    for (int i = 0; i < q.n; i++) matched_terms += df_of(b, q.t[i].h) > 0;
    for (int i = 0; i < b->ndocs; i++) {
        const AsDoc* d = &b->docs[i];
        float s = 0.0f;
        int hits = 0;
        for (int k = 0; k < q.n; k++) {
            // the term in the document (its terms sorted: a binary search)
            uint32_t lo = d->t0, hi = d->t0 + d->nt;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                if (b->terms[mid].h < q.t[k].h) lo = mid + 1;
                else hi = mid;
            }
            if (lo >= d->t0 + d->nt || b->terms[lo].h != q.t[k].h) continue;
            float tf = b->terms[lo].tf, df = (float)df_of(b, q.t[k].h);
            float idf = logf(1.0f + (N - df + 0.5f) / (df + 0.5f));
            s += q.t[k].w * idf * tf * (k1 + 1.0f) / (tf + k1 * (1.0f - bb + bb * d->len / b->avg_len));
            hits++;
        }
        if (qk >= 3 && d->src != AS_SRC_ARTICLE) {
            char dn[96];
            size_t dk = 0;
            for (const char* c = d->id; *c && dk + 1 < sizeof dn; c++)
                if (isalnum((unsigned char)*c)) dn[dk++] = (char)tolower((unsigned char)*c);
            dn[dk] = 0;
            if (!strcmp(dn, qn)) s += 100.0f, hits = q.n;
        }
        if (s <= 0.0f) continue;
        // the more of the question's words a document has, the better; the articles first (they explain), then the controls
        s *= 0.5f + 0.5f * (float)hits / (float)(matched_terms ? matched_terms : 1);
        s *= d->src == AS_SRC_ARTICLE ? 1.35f : d->src == AS_SRC_CONTROL ? 0.85f : 0.9f;
        all[n++] = (AsHit){ i, s };
    }
    if (err_doc >= 0) { // (on top whatever the words)
        bool found = false;
        for (int i = 0; i < n; i++)
            if (all[i].doc == err_doc) all[i].score += 1000.0f, found = true;
        if (!found) all[n++] = (AsHit){ err_doc, 1000.0f };
    }
    qsort(all, (size_t)n, sizeof *all, by_score);
    // the weak ones after a strong answer dropped (a word in common isn't an answer)
    while (n > 1 && all[n - 1].score < all[0].score * 0.2f) n--;
    int m = n < cap ? n : cap;
    memcpy(out, all, sizeof *out * (size_t)m);
    if (weak) *weak = m == 0 || (all[0].score < 2.0f && err_doc < 0);
    free(all);
    S.stats.queries++;
    S.stats.last_query_ms = (re_time_seconds() - t0) * 1000.0;
    return m;
}
