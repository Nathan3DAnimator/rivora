// (2026-11-24, item 15) Included by loc.c: --loc-test <folder> (ctest loc, no window). A table of a few texts in a folder
// of its own (native pt-BR, target en): the offline pseudo-localization fills the target; saved and read back the same;
// .po out, the translations cleared, .po in: the same again (and the .csv); compiled; the culture switched: the texts
// translated, back to the native: the sources; a table of 20 000 texts compiled and switched (its time and memory).
#include <assert.h>

static int t_fails;
#define T(cond, ...) ((cond) ? (void)0 : (void)(printf("[RealEngine] FAIL: loc: " __VA_ARGS__), printf("\n"), t_fails++))

// (a copy of the target's texts and statuses: the table's own are freed when it is read again)
static char g_saved[8][LOC_TEXT];
static uint8_t g_saved_st[8];
static bool same_table(const LocProject* a, uint32_t n, int c) {
    if (a->n != n) return false;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(loc_tr(&a->e[i], c), g_saved[i]) || a->e[i].tr[c].status != g_saved_st[i]) return false;
    return true;
}

bool loc_selftest(const char* dir) {
    t_fails = 0;
    char root[800];
    snprintf(root, sizeof root, "%s/Content", dir && dir[0] ? dir : ".");
    re_make_dirs(root);
    char path[900];
    loc_set_content_root(root);
    rloc_path(path, sizeof path);
    remove(path);
    LocProject* p = loc_project();
    snprintf(p->native, LOC_CULTURE, "pt-BR");
    int en = loc_add_culture("en");
    T(en == 0, "the target culture (%d)", en);
    loc_gather_add("W:WBP_Menu", "Title.Text", "Bem-vindo", "Widget WBP_Menu > Title");
    loc_gather_add("DLG:DLG_Guarda", "n1", "Pare! Quem vem lá?", "Dialogue DLG_Guarda > line 1");
    loc_gather_add("DLG:DLG_Guarda", "n2", "Um amigo.", "Dialogue DLG_Guarda > choice 2");
    char key[LOC_KEY];
    loc_text_key("Pontos: {0}", key, sizeof key);
    loc_gather_add("BP:BP_Hud", key, "Pontos: {0}", "Blueprint BP_Hud > Format Text");
    // the offline provider fills the target
    loc_provider()->kind = LOC_PROV_PSEUDO;
    T(loc_auto_translate(en, false), "the pseudo-localization didn't run");
    char want[LOC_TEXT];
    loc_pseudo("Bem-vindo", want, sizeof want);
    T(!strcmp(loc_tr(&p->e[0], en), want) && p->e[0].tr[en].status == LOC_AUTO, "pseudo: \"%s\" (wanted \"%s\")", loc_tr(&p->e[0], en), want);
    T(strstr(loc_tr(&p->e[3], en), "{0}") != NULL, "pseudo changed the format argument: %s", loc_tr(&p->e[3], en));
    loc_tr_set(&p->e[1], en, "Halt! Who goes there?"), p->e[1].tr[en].status = LOC_REVIEWED; // (a translator's)
    // saved and read back
    T(loc_save(), "saving %s", path);
    uint32_t n = p->n;
    for (uint32_t i = 0; i < n && i < 8; i++) snprintf(g_saved[i], LOC_TEXT, "%s", loc_tr(&p->e[i], 0)), g_saved_st[i] = p->e[i].tr[0].status;
    loc_set_content_root(root);
    p = loc_project();
    T(!strcmp(p->native, "pt-BR") && p->ntargets == 1 && same_table(p, n, 0), "the table read back differs (%u entries)", p->n);
    // .po out, cleared, in: the same (statuses: Reviewed / Translated stay translated, the automatic come back fuzzy)
    char po[900], csv[900];
    loc_dir_file(po, sizeof po, "en", ".po"), loc_dir_file(csv, sizeof csv, "Game", ".csv");
    T(loc_export_po(0, po), "export .po");
    for (uint32_t i = 0; i < p->n; i++) loc_tr_set(&p->e[i], 0, ""), p->e[i].tr[0].status = LOC_MISSING;
    uint32_t got = 0;
    T(loc_import_po(0, po, &got), "import .po");
    bool po_same = p->n == n;
    for (uint32_t i = 0; po_same && i < n; i++)
        po_same = !strcmp(loc_tr(&p->e[i], 0), g_saved[i]) &&
                  (p->e[i].tr[0].status == g_saved_st[i] || (g_saved_st[i] == LOC_REVIEWED && p->e[i].tr[0].status == LOC_TRANSLATED));
    printf("[RealEngine] loc: .po exported and read back: %u texts, %s\n", got, po_same ? "the same" : "DIFFERENT");
    T(po_same && got == n, ".po round trip (%u of %u)", got, n);
    T(loc_export_csv(csv), "export .csv");
    for (uint32_t i = 0; i < p->n; i++) loc_tr_set(&p->e[i], 0, "");
    T(loc_import_csv(csv, &got) && got == n && !strcmp(loc_tr(&p->e[1], 0), "Halt! Who goes there?") && !strcmp(loc_tr(&p->e[0], 0), want), ".csv round trip (%u)", got);
    // compiled; switched
    char msg[200];
    T(loc_compile(msg, sizeof msg) == 2, "compile: %s", msg);
    T(loc_set_culture("en"), "the en table didn't load");
    const char* t0 = loc_text("DLG:DLG_Guarda", "n1", "Pare! Quem vem lá?");
    const char* t1 = loc_text("W:WBP_Menu", "Title.Text", "Bem-vindo");
    const char* none = loc_text("W:WBP_Menu", "Nope.Text", NULL);
    LocStats st = loc_stats();
    printf("[RealEngine] loc: en: \"%s\", \"%s\"; switched in %.3f ms, %u texts, %zu bytes\n", t0, t1, st.last_switch_ms, st.entries, st.bytes);
    T(!strcmp(t0, "Halt! Who goes there?") && !strcmp(t1, want) && !none && loc_active() && !strcmp(loc_culture(), "en"), "the en texts");
    uint32_t rev = loc_revision();
    T(loc_set_culture("pt-BR") && !loc_active() && !strcmp(loc_text("W:WBP_Menu", "Title.Text", "Bem-vindo"), "Bem-vindo") && loc_revision() != rev, "back to the native");
    // a big table: 20 000 texts compiled and switched
    for (int i = 0; i < 20000; i++) {
        char k[32], s[64];
        snprintf(k, sizeof k, "k%05d", i), snprintf(s, sizeof s, "Texto número %d do jogo", i);
        LocEntry* e = loc_gather_add("BIG", k, s, "");
        char tt[64];
        snprintf(tt, sizeof tt, "Game text number %d", i);
        if (e) loc_tr_set(e, 0, tt), e->tr[0].status = LOC_TRANSLATED;
    }
    double c0 = re_time_seconds();
    loc_compile(msg, sizeof msg);
    double cms = (re_time_seconds() - c0) * 1000.0;
    T(loc_set_culture("en"), "the big en table");
    st = loc_stats();
    double first = st.last_switch_ms;
    loc_set_culture("pt-BR"), loc_set_culture("en"); // (read once: a switch back swaps the table kept in memory)
    double again = loc_stats().last_switch_ms;
    double l0 = re_time_seconds();
    const char* probe = NULL;
    for (int i = 0; i < 100000; i++) {
        char k[32];
        snprintf(k, sizeof k, "k%05d", i % 20000);
        probe = loc_text("BIG", k, "");
    }
    double lus = (re_time_seconds() - l0) * 1e6 / 100000.0;
    printf("[RealEngine] loc: 20004 texts: compiled in %.1f ms, the switch %.3f ms (read) / %.4f ms (kept), the table %.0f KB, a lookup %.3f us (\"%s\")\n", cms,
           first, again, (double)st.bytes / 1024.0, lus, probe ? probe : "");
    T(st.entries == 20004 && first < 50.0 && again < 0.5 && lus < 2.0, "the big table (%u entries, %.3f / %.4f ms)", st.entries, first, again);
    loc_set_culture("");
    loc_provider()->kind = LOC_PROV_OFF;
    printf("[RealEngine] loc: %s\n", t_fails ? "FAILED" : "ok");
    return t_fails == 0;
}
#undef T
