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
        for (int l = 1; l < LANG_COUNT; l++)
            if (!t[i].text[l - 1] || !t[i].text[l - 1][0]) printf("FAIL: \"%s\" without %s\n", t[i].msgid, i18n_language_code(l)), fail++;
    }
    i18n_set_language(LANG_PT_BR);
    if (strcmp(tr("File"), "Arquivo") != 0) printf("FAIL: tr(File)\n"), fail++;
    if (strcmp(tr("Delete##row"), "Excluir##row") != 0) printf("FAIL: suffix kept\n"), fail++;
    if (strcmp(tr("not in the table"), "not in the table") != 0) printf("FAIL: untranslated\n"), fail++;
    i18n_set_language(LANG_EN);
    if (strcmp(tr("File"), "File") != 0) printf("FAIL: back to English\n"), fail++;
    printf(fail ? "%d FAILED\n" : "i18n: %d entries, all passed\n", fail ? fail : n);
    return fail ? 1 : 0;
}
