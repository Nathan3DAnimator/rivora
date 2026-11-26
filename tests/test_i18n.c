// The translation table: no msgid twice, every entry translated, the label suffixes kept, the language switch.
#include <stdio.h>
#include <string.h>

#include "i18n.h"

int main(void) {
    int n = 0, fail = 0;
    const I18nEntry* t = i18n_table(&n);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < i; k++)
            if (!strcmp(t[i].msgid, t[k].msgid)) printf("FAIL: msgid twice: \"%s\"\n", t[i].msgid), fail++;
        if (!t[i].text[0] || !t[i].text[0][0]) printf("FAIL: \"%s\" without %s\n", t[i].msgid, i18n_language_code(LANG_PT_BR)), fail++;
        if (!i18n_has_es(t[i].msgid)) printf("FAIL: \"%s\" without %s\n", t[i].msgid, i18n_language_code(LANG_ES)), fail++;
    }
    // (2026-11-25, item 12) the Spanish table: no msgid twice, each one a msgid of the main table, every text there
    int ne = 0;
    const I18nEs* es = i18n_table_es(&ne);
    for (int i = 0; i < ne; i++) {
        if (!es[i].text || !es[i].text[0]) printf("FAIL: es \"%s\" empty\n", es[i].msgid), fail++;
        if (!i18n_has(es[i].msgid)) printf("FAIL: es \"%s\" isn't a msgid of i18n_table.c\n", es[i].msgid), fail++;
    }
    i18n_set_language(LANG_PT_BR);
    if (strcmp(tr("File"), "Arquivo") != 0) printf("FAIL: tr(File)\n"), fail++;
    if (strcmp(tr("Delete##row"), "Excluir##row") != 0) printf("FAIL: suffix kept\n"), fail++;
    if (strcmp(tr("not in the table"), "not in the table") != 0) printf("FAIL: untranslated\n"), fail++;
    i18n_set_language(LANG_ES);
    if (strcmp(tr("File"), "Archivo") != 0) printf("FAIL: tr(File) in Spanish\n"), fail++;
    if (strcmp(tr("File##m"), "Archivo##m") != 0) printf("FAIL: suffix kept in Spanish\n"), fail++;
    if (strcmp(tr_lang("File", LANG_PT_BR), "Arquivo") != 0) printf("FAIL: tr_lang pt while in Spanish\n"), fail++;
    i18n_set_language(LANG_EN);
    if (strcmp(tr("File"), "File") != 0) printf("FAIL: back to English\n"), fail++;
    // (2026-11-25, item 13) Unreal's Blueprints are "Graph" / "Grafo" on screen; identifiers and other words untouched
    if (strcmp(tr("Blueprint Class"), "Graph Class") || strcmp(tr("Open Blueprints here"), "Open Graphs here") || strcmp(i18n_terms("BlueprintCallable"), "BlueprintCallable"))
        printf("FAIL: English Graph: \"%s\"\n", tr("Blueprint Class")), fail++;
    i18n_set_language(LANG_PT_BR);
    if (!strstr(tr("Blueprint Class"), "Grafo") || strstr(tr("Blueprint Class"), "Blueprint")) printf("FAIL: pt Grafo: \"%s\"\n", tr("Blueprint Class")), fail++;
    i18n_set_language(LANG_ES);
    if (!strstr(tr("Blueprint Class"), "Grafo") || strstr(tr("Blueprint Class"), "Blueprint")) printf("FAIL: es Grafo: \"%s\"\n", tr("Blueprint Class")), fail++;
    i18n_set_language(LANG_EN);
    printf(fail ? "%d FAILED\n" : "i18n: %d entries, all passed\n", fail ? fail : n);
    return fail ? 1 : 0;
}
