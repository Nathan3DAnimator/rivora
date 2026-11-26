// The Assistant (notas-dev/assistente.md): questions answered without AI. A search of its own (Portuguese, English,
// Spanish: no accents, plurals and verb endings cut, synonyms, typos by trigrams, BM25) over
//   - the knowledge base's articles (how-tos, recipes in the 4 languages, the errors explained): NOT in the program; a
//     public GitHub repository's index.json, fetched on the first question (online) or kept on this computer (offline),
//   - what the editor already has: the engine's functions (the API reference) and every control's description.
// No network, no base: the built-in sources answer alone.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { AS_SRC_ARTICLE, AS_SRC_API, AS_SRC_CONTROL };
enum { AS_KIND_HOWTO, AS_KIND_RECIPE, AS_KIND_ERROR, AS_KIND_CONCEPT };

typedef struct AsDoc {
    uint8_t src, kind;
    const char* id;       // an article's id; a function's name; a control's label
    const char* title[3]; // English, Portuguese, Spanish (the editor's language picks; 2026-11-25: Spanish)
    const char* body[3];  // Markdown (an article), the description (a function, a control)
    const char* extra;    // a function's signature; an article's error patterns ("|" between)
    uint32_t t0, nt;      // its terms (the pool)
    float len;
} AsDoc;

typedef struct AsHit { int doc; float score; } AsHit;

typedef enum AsState { AS_NONE, AS_FETCHING, AS_ONLINE, AS_OFFLINE, AS_FAILED } AsState;

void assistant_init_async(void); // (2026-11-25) the same on a worker, once the editor settled (assistant_init waits for it)
void assistant_init(void);     // the built-in sources (after the scripts' API: script_init); the offline copy if any
void assistant_shutdown(void);
void assistant_relang(void); // (2026-11-25) after the editor's language changed: the index again (Spanish indexed in Spanish only)
// The question's best answers (most first); n: how many. *weak: nothing really matched (offer to send it as a suggestion)
int assistant_search(const char* query, AsHit* out, int cap, bool* weak);
const AsDoc* assistant_doc(int i);
int assistant_doc_count(void);
int assistant_find_article(const char* id); // its doc index, -1: none
// An error message (the scripts' compiler): the article explaining it, -1: none
int assistant_explain_error(const char* message);

// The base: its state, version, articles; fetched (online: kept in memory) or downloaded (offline: kept on disk)
AsState assistant_state(void);
const char* assistant_version(void);
int assistant_article_count(void);
uint32_t assistant_base_bytes(void);
const char* assistant_error(void);
void assistant_fetch(bool keep_offline); // in the background (async.h); keep_offline: written to the computer too
bool assistant_offline_present(void);
void assistant_remove_offline(void);
const char* assistant_source_url(void); // where the base comes from (REALENGINE_ASSISTANT_URL: a folder or URL instead)

// Measures and the test
typedef struct AsStats { uint32_t docs, terms, vocab, queries; double build_ms, last_query_ms; } AsStats;
AsStats assistant_stats(void);
