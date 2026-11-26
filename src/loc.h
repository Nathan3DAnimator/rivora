// (2026-11-24, item 15 of the user's list: "um sistema de tradução automática que traduz o jogo inteiro, igual ao de
// localização da Unreal: Widgets, textos e conversas") The game's localization (Unreal's Localization Dashboard and
// FText), apart from the editor's own i18n (TR, i18n.h):
//  - Gather: every text the game shows (the Widgets' Text, the Blueprints' and scripts' Text literals, the Dialogues'
//    lines and choices, the Quests' names and objectives, the items' names and descriptions), each with a stable key
//    (a namespace: its asset; a key: where in it), in <Content>/Localization/Game/Game.rloc (JSON: the native and target
//    cultures, the entries, each culture's translation and status);
//  - Translate: by hand in the dashboard's table, or automatically by a provider the user sets and starts (LibreTranslate,
//    DeepL, Google: HTTP with the user's key, off by default; nothing is sent without its settings and a click) or the
//    offline Pseudo-localization (Unreal's: the text accented and bracketed, to see what isn't localized and what
//    overflows); .po and .csv out and in;
//  - Compile: one binary per culture (<Content>/Localization/Game/<culture>.rlocres: the keys' hashes sorted, the texts),
//    read by the game (packaged with the Content) and by Play;
//  - At run time: loc_text(namespace, key, source) gives the current culture's text (the source when none); Set Current
//    Culture swaps the table live (loc_revision() moves: the widgets and the dialogue write their texts again).
// notas-dev/localizacao.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ItemDef; struct BpAsset; struct UiWidget; struct Dialogue; struct DlgNode; // (file scope: clang sees a struct first named inside a prototype as a different type)

#define LOC_NS 64
#define LOC_KEY 96
#define LOC_TEXT 256
#define LOC_CULTURE 16
#define LOC_MAX_CULTURES 16

enum { LOC_MISSING = 0, LOC_AUTO, LOC_TRANSLATED, LOC_REVIEWED, LOC_STATUS_COUNT }; // a translation's status
extern const char* const g_loc_status_names[LOC_STATUS_COUNT];

typedef struct LocTr { char* text; uint8_t status; } LocTr; // (text: malloc'd, NULL when empty: loc_tr / loc_tr_set)
typedef struct LocEntry {
    char ns[LOC_NS], key[LOC_KEY];
    char source[LOC_TEXT];
    char where[160];     // what it is ("Widget WBP_Menu > Title", "Dialogue DLG_Guard > line 3")
    uint64_t hash;       // of ns '\0' key (the compiled tables' key)
    bool stale;          // not found by the last gather (kept until the next Compact)
    LocTr tr[LOC_MAX_CULTURES]; // by the target culture's index
} LocEntry;

typedef struct LocProject {
    char native[LOC_CULTURE];
    char targets[LOC_MAX_CULTURES][LOC_CULTURE];
    int ntargets;
    LocEntry* e;
    uint32_t n, cap;
    bool dirty;
} LocProject;

// --- the project's table (the editor) ---
LocProject* loc_project(void);
const char* loc_tr(const LocEntry* e, int culture);           // its translation ("" none)
void loc_tr_set(LocEntry* e, int culture, const char* text); // (copied; "" frees it)                    // loaded from <Content>/Localization/Game/Game.rloc on first use
void loc_set_content_root(const char* content_root); // (a project opened: its table is read again)
bool loc_save(void);
void loc_dir_file(char* out, size_t cap, const char* name, const char* ext); // <Content>/Localization/Game/<name><ext>
int loc_add_culture(const char* code);            // its index (-1: full / bad)
void loc_remove_culture(int i);
uint64_t loc_hash(const char* ns, const char* key);
// one text found (the gatherers): added, or its source updated (a changed source: its translations marked Missing/Auto)
LocEntry* loc_gather_add(const char* ns, const char* key, const char* source, const char* where);
typedef struct LocGatherStats { uint32_t widgets, blueprints, dialogues, quests, items, total, added, changed, stale; double ms; } LocGatherStats;
LocGatherStats loc_gather(void);                  // every gatherer over the project (loc_gather.c)
void loc_counts(int culture, uint32_t* done, uint32_t* total); // translated or better / every live entry
// .po (gettext, one culture) and .csv (every culture) out and in; false with the reason
bool loc_export_po(int culture, const char* path);
bool loc_import_po(int culture, const char* path, uint32_t* changed);
bool loc_export_csv(const char* path);
bool loc_import_csv(const char* path, uint32_t* changed);
// The binaries: one per target culture (and the native's, its own texts); how many written
int loc_compile(char* msg, size_t cap);

// --- automatic translation ---
enum { LOC_PROV_OFF = 0, LOC_PROV_PSEUDO, LOC_PROV_LIBRE, LOC_PROV_DEEPL, LOC_PROV_GOOGLE, LOC_PROV_COUNT };
extern const char* const g_loc_provider_names[LOC_PROV_COUNT];
typedef struct LocProvider { int kind; char url[256], key[160]; } LocProvider; // (the user's: editor.ini; never in the project)
LocProvider* loc_provider(void);
// Fills culture's missing (or every not Reviewed, all) texts with the provider, on a worker (async.h card); false: busy
// or off. The pseudo one runs at once (offline)
bool loc_auto_translate(int culture, bool all);
bool loc_auto_busy(float* progress);
void loc_pseudo(const char* in, char* out, size_t cap); // Unreal's pseudo-localization of one text
void loc_frame(void);                               // the editor's frame: a finished auto translation merged
void loc_write_prefs(void* file);                   // editor.ini (the provider; its key)
bool loc_read_pref(const char* key, const char* v);

// --- run time ---
bool loc_set_culture(const char* code);           // its compiled table loaded ("" or the native: the sources); false: none
const char* loc_culture(void);
const char* loc_text(const char* ns, const char* key, const char* source); // the current culture's, else source
bool loc_active(void);                             // a culture other than the native is loaded (the hooks look then)
void loc_text_key(const char* text, char* out, size_t cap); // a Text literal's key ("T_" + its hash; loc_gather.c)
void loc_bp_apply(void);                           // the classes' Text literals in the current culture (loc_gather.c)
bool loc_switch(const char* code);                 // loc_set_culture + loc_bp_apply: what the game and the nodes call
struct BpAsset;
struct UiWidget;
struct ItemDef;
const char* loc_item(const struct ItemDef* it, bool desc); // an item's name / description in the culture
const char* loc_widget_text(const struct BpAsset* cls, const struct UiWidget* w, int i); // a widget's, NULL: none
void loc_dlg_key(const struct Dialogue* d, const struct DlgNode* n, char* ns, size_t ncap, char* key, size_t kcap);
uint32_t loc_revision(void);                       // moves when the culture changes
int loc_cultures(const char** out, int cap);       // the native and the targets (for a language menu)
const char* loc_culture_name(const char* code);    // its name in its own language ("pt-BR": "Português (Brasil)"; else the code)
typedef struct LocStats { uint32_t entries; size_t bytes; double last_switch_ms; } LocStats;
LocStats loc_stats(void);                          // the loaded table's
// the tests
bool loc_selftest(const char* dir);                // --loc-test <folder>: the table, pseudo, compile, lookup, .po round trip
