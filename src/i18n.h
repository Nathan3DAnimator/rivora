// Interface languages (Edit > Editor Preferences > Language), switched while the editor runs. Copied from the Tlender
// (same scheme) with the msgids in English, Unreal's names: TR() looks them up in i18n_table.c and returns the
// current language's text, or the msgid itself when there is no translation.
#pragma once
#include <stdbool.h>

typedef enum Language { LANG_EN = 0, LANG_PT_BR, LANG_ES, LANG_COUNT } Language;

typedef struct I18nEntry {
    const char* msgid;   // English, exactly as written in the code
    const char* text[1]; // Português (Brasil) (NULL or "": not translated)
} I18nEntry;
// (2026-11-25, item 12) Spanish: its own table (i18n_table_es.c), msgid -> text, the same msgids as i18n_table.c's
typedef struct I18nEs { const char* msgid; const char* text; } I18nEs;
const I18nEs* i18n_table_es(int* count);
bool i18n_has_es(const char* msgid);
// (2026-11-25) the misses: every msgid looked up in a translated language and not found, written to path at exit
// (REALENGINE_I18N_MISSING=path: the audit's runtime half)
void i18n_record_missing(const char* path);
int i18n_missing_count(void);       // (item 21) the misses recorded so far
const char* i18n_missing_at(int i);

void i18n_set_language(int lang);
int i18n_language(void);
const char* i18n_language_name(int lang); // in the language itself: "English", "Português (Brasil)", "Español"
const char* i18n_language_code(int lang); // "en_US", "pt_BR", "es_ES"
int i18n_language_from_code(const char* code); // -1 when unknown

// Text of msgid in the current language. A label with an ImGui ID suffix ("Exposure##vp") keeps the suffix.
const char* tr(const char* msgid);
// ImGui label whose ID doesn't depend on the language ("Exposição###Exposure"): windows, tabs, popups. UI thread.
const char* tr_id(const char* msgid);
// Text of msgid in a given language (the game's widgets: their language may not be the editor's)
const char* tr_lang(const char* msgid, int lang);
int i18n_system_language(void); // the OS's interface language (LANG_EN when it isn't one of ours)

// (2026-11-25, item 13) The engine's own name for Unreal's Blueprints on screen: "Graph" (pt and es "Grafo"). Every text
// shown (tr, tr_id, the descriptions) passes through it; the identifiers, the msgids, the files keep "Blueprint"
const char* i18n_terms(const char* text);
extern bool g_i18n_raw_terms; // (the --i18n-dump: the msgids as they are)

#define TR(s) tr(s)
#define TR_ID(s) tr_id(s)
#define N_(s) (s) // a msgid translated later, where it is shown

const I18nEntry* i18n_table(int* count);
bool i18n_has(const char* msgid);
const char* i18n_msgid(const char* text);
