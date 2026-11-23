// Interface languages (Edit > Editor Preferences > Language), switched while the editor runs. Copied from the Tlender
// (same scheme) with the msgids in English, Unreal's names: TR() looks them up in i18n_table.c and returns the
// current language's text, or the msgid itself when there is no translation.
#pragma once
#include <stdbool.h>

typedef enum Language { LANG_EN = 0, LANG_PT_BR, LANG_COUNT } Language;

typedef struct I18nEntry {
    const char* msgid;                 // English, exactly as written in the code
    const char* text[LANG_COUNT - 1]; // LANG_PT_BR ... (NULL or "": not translated)
} I18nEntry;

void i18n_set_language(int lang);
int i18n_language(void);
const char* i18n_language_name(int lang); // in the language itself: "English", "Português (Brasil)"
const char* i18n_language_code(int lang); // "en_US", "pt_BR"
int i18n_language_from_code(const char* code); // -1 when unknown

// Text of msgid in the current language. A label with an ImGui ID suffix ("Exposure##vp") keeps the suffix.
const char* tr(const char* msgid);
// ImGui label whose ID doesn't depend on the language ("Exposição###Exposure"): windows, tabs, popups. UI thread.
const char* tr_id(const char* msgid);
// Text of msgid in a given language (the game's widgets: their language may not be the editor's)
const char* tr_lang(const char* msgid, int lang);
int i18n_system_language(void); // the OS's interface language (LANG_EN when it isn't one of ours)

#define TR(s) tr(s)
#define TR_ID(s) tr_id(s)
#define N_(s) (s) // a msgid translated later, where it is shown

const I18nEntry* i18n_table(int* count);
bool i18n_has(const char* msgid);
const char* i18n_msgid(const char* text);
