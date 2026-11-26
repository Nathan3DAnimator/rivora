// (2026-11-24, item 15) Tools > Localization Dashboard (Unreal's): the project's cultures, Gather Text, the automatic
// translation (a provider the user sets; off by default), the table (search, the status of each text, counts per
// culture), .po and .csv out and in, Compile, and the culture shown in Play (loc.h).
#include "loc.h"
#include "loc_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"
#include "theme.h"
#include "ui_icons.h"

static struct {
    bool show;
    int culture;      // the target shown in the table
    char find[96];
    int filter;       // -1 every status, else LOC_*
    bool all;         // Translate Automatically: every text not Reviewed (else the missing ones)
    char status[240];
    int* rows;        // the table's rows after the search (indices)
    uint32_t nrows, cap_rows;
    char last_find[96];
    int last_filter, last_culture;
    uint32_t last_n;
    bool rows_dirty;
} U = { .filter = -1, .last_filter = -2, .last_culture = -2 };

bool* loc_ui_show_flag(void) { return &U.show; }


static const char* const k_codes[] = { "en", "en-US", "en-GB", "pt-BR", "pt-PT", "es", "es-419", "fr", "de", "it", "ja", "ko", "zh-Hans", "zh-Hant",
                                       "ru", "pl", "tr", "ar", "nl", "sv", "uk", "cs", "hi", "id" };

static bool stristr(const char* h, const char* n) {
    size_t l = strlen(n);
    for (; *h; h++)
        if (!_strnicmp(h, n, l)) return true;
    return false;
}

static void rebuild_rows(LocProject* p) {
    if (U.cap_rows < p->n) {
        int* nr = realloc(U.rows, sizeof(int) * p->n);
        if (!nr) return;
        U.rows = nr, U.cap_rows = p->n;
    }
    U.nrows = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        const LocEntry* e = &p->e[i];
        if (e->stale) continue;
        if (U.filter >= 0 && U.culture < p->ntargets && e->tr[U.culture].status != U.filter) continue;
        if (U.find[0] && !stristr(e->source, U.find) && !stristr(e->where, U.find) && !stristr(e->key, U.find) &&
            !(U.culture < p->ntargets && stristr(loc_tr(e, U.culture), U.find)))
            continue;
        U.rows[U.nrows++] = (int)i;
    }
    snprintf(U.last_find, sizeof U.last_find, "%s", U.find);
    U.last_filter = U.filter, U.last_culture = U.culture, U.last_n = p->n, U.rows_dirty = false;
}

static void cultures_panel(LocProject* p) {
    igSeparatorText(TR("Cultures"));
    igSetNextItemWidth(igGetFontSize() * 7);
    if (igBeginCombo(TR("Native Culture"), p->native, 0)) {
        for (size_t i = 0; i < sizeof k_codes / sizeof k_codes[0]; i++)
            if (igSelectable_Bool(k_codes[i], !strcmp(p->native, k_codes[i]), 0, (ImVec2){ 0, 0 })) snprintf(p->native, LOC_CULTURE, "%s", k_codes[i]), p->dirty = true; // tips: none (a culture code)
        igEndCombo();
    }
    igSameLine(0, -1);
    igTextDisabled("%s", loc_culture_name(p->native));
    for (int c = 0; c < p->ntargets; c++) {
        uint32_t done = 0, total = 0;
        loc_counts(c, &done, &total);
        igPushID_Int(c);
        char lb[96];
        snprintf(lb, sizeof lb, "%s  %u / %u (%.0f%%)##tgt", p->targets[c], done, total, total ? 100.0 * done / total : 0.0);
        if (igSelectable_Bool(lb, U.culture == c, 0, (ImVec2){ igGetFontSize() * 16, 0 })) U.culture = c, U.rows_dirty = true; // tips: none (a culture and its count)
        igSameLine(0, -1);
        igTextDisabled("%s", loc_culture_name(p->targets[c]));
        igSameLine(0, -1);
        if (igSmallButton(TR("Remove Culture"))) {
            loc_remove_culture(c);
            if (U.culture >= p->ntargets) U.culture = p->ntargets - 1;
            U.rows_dirty = true;
        }
        igPopID();
    }
    if (igBeginCombo(TR("Add Culture"), "+", 0)) {
        for (size_t i = 0; i < sizeof k_codes / sizeof k_codes[0]; i++) {
            char lb[96];
            snprintf(lb, sizeof lb, "%s  %s", k_codes[i], loc_culture_name(k_codes[i]));
            if (igSelectable_Bool(lb, false, 0, (ImVec2){ 0, 0 })) { // tips: none (a culture code and its name)
                int c = loc_add_culture(k_codes[i]);
                if (c >= 0) U.culture = c, U.rows_dirty = true;
            }
        }
        igEndCombo();
    }
}

static void actions_panel(LocProject* p) {
    igSeparatorText(TR("Gather and Compile"));
    if (igButton(TR("Gather Text"), (ImVec2){ 0, 0 })) {
        LocGatherStats s = loc_gather();
        snprintf(U.status, sizeof U.status, TR("Gathered %u texts in %.1f ms: %u from Widgets, %u from Blueprints, %u from Dialogues, %u from Quests, %u from items (%u new, %u no longer found)."),
                 s.total, s.ms, s.widgets, s.blueprints, s.dialogues, s.quests, s.items, s.added, s.stale);
        U.rows_dirty = true;
    }
    igSameLine(0, -1);
    if (igButton(TR("Compile"), (ImVec2){ 0, 0 })) {
        loc_save();
        loc_compile(U.status, sizeof U.status);
    }
    igSameLine(0, -1);
    if (igButton(TR("Save"), (ImVec2){ 0, 0 })) snprintf(U.status, sizeof U.status, "%s", loc_save() ? TR("Saved.") : TR("Could not save."));
    if (p->ntargets) {
        const char* c = p->targets[U.culture < p->ntargets ? U.culture : 0];
        igSameLine(0, -1);
        if (igButton(TR("Export .po"), (ImVec2){ 0, 0 })) {
            char path[900];
            loc_dir_file(path, sizeof path, c, ".po");
            snprintf(U.status, sizeof U.status, loc_export_po(U.culture, path) ? TR("Exported to %s") : TR("Could not write %s."), path);
        }
        igSameLine(0, -1);
        if (igButton(TR("Import .po"), (ImVec2){ 0, 0 })) {
            char path[900];
            uint32_t n = 0;
            loc_dir_file(path, sizeof path, c, ".po");
            if (loc_import_po(U.culture, path, &n)) snprintf(U.status, sizeof U.status, TR("%u translation(s) read from %s"), n, path);
            else snprintf(U.status, sizeof U.status, TR("Could not read %s."), path);
            U.rows_dirty = true;
        }
        igSameLine(0, -1);
        if (igButton(TR("Export .csv"), (ImVec2){ 0, 0 })) {
            char path[900];
            loc_dir_file(path, sizeof path, "Game", ".csv");
            snprintf(U.status, sizeof U.status, loc_export_csv(path) ? TR("Exported to %s") : TR("Could not write %s."), path);
        }
        igSameLine(0, -1);
        if (igButton(TR("Import .csv"), (ImVec2){ 0, 0 })) {
            char path[900];
            uint32_t n = 0;
            loc_dir_file(path, sizeof path, "Game", ".csv");
            if (loc_import_csv(path, &n)) snprintf(U.status, sizeof U.status, TR("%u translation(s) read from %s"), n, path);
            else snprintf(U.status, sizeof U.status, TR("Could not read %s."), path);
            U.rows_dirty = true;
        }
    }
}

static void auto_panel(LocProject* p) {
    igSeparatorText(TR("Automatic Translation"));
    LocProvider* pv = loc_provider();
    igSetNextItemWidth(igGetFontSize() * 14);
    if (igBeginCombo(TR("Provider"), TR(g_loc_provider_names[pv->kind]), 0)) {
        for (int k = 0; k < LOC_PROV_COUNT; k++)
            if (igSelectable_Bool(TR(g_loc_provider_names[k]), pv->kind == k, 0, (ImVec2){ 0, 0 })) pv->kind = k; // tips: none (a translation provider)
        igEndCombo();
    }
    if (pv->kind == LOC_PROV_LIBRE || pv->kind == LOC_PROV_DEEPL || pv->kind == LOC_PROV_GOOGLE) {
        igSetNextItemWidth(igGetFontSize() * 20);
        igInputTextWithHint(TR("Server URL"), pv->kind == LOC_PROV_LIBRE ? "http://localhost:5000" : TR("(the provider's)"), pv->url, sizeof pv->url, 0, NULL, NULL);
        igSetNextItemWidth(igGetFontSize() * 20);
        igInputText(TR("API Key"), pv->key, sizeof pv->key, ImGuiInputTextFlags_Password, NULL, NULL);
        igTextDisabled("%s", TR("Your key stays in your editor settings, never in the project. Nothing is sent until you click Translate Automatically."));
    } else if (pv->kind == LOC_PROV_PSEUDO)
        igTextDisabled("%s", TR("Offline: every text accented and bracketed, to see what isn't localized and what overflows (Unreal's pseudo-localization)."));
    else
        igTextDisabled("%s", TR("Off: pick a provider to translate automatically."));
    igCheckbox(TR("Retranslate All"), &U.all);
    float prog = 0.0f;
    bool busy = loc_auto_busy(&prog);
    igBeginDisabled(busy || !p->ntargets || pv->kind == LOC_PROV_OFF);
    char lb[120];
    snprintf(lb, sizeof lb, "%s (%s)###auto", TR("Translate Automatically"), p->ntargets ? p->targets[U.culture < p->ntargets ? U.culture : 0] : "-");
    if (igButton(lb, (ImVec2){ 0, 0 })) { // tips: none (Translate Automatically, its culture in the label)
        if (!loc_auto_translate(U.culture, U.all)) snprintf(U.status, sizeof U.status, "%s", TR("Not started: set the provider (its key) first."));
        U.rows_dirty = true;
    }
    igEndDisabled();
    if (busy) {
        igSameLine(0, -1);
        igProgressBar(prog, (ImVec2){ igGetFontSize() * 10, 0 }, NULL);
    }
}

static void table_panel(LocProject* p) {
    if (!p->ntargets) {
        igTextDisabled("%s", TR("Add a culture to translate to."));
        return;
    }
    if (U.culture >= p->ntargets) U.culture = 0;
    igSetNextItemWidth(igGetFontSize() * 18);
    igInputTextWithHint("##loc_find", TR("Search texts"), U.find, sizeof U.find, 0, NULL, NULL);
    igSameLine(0, -1);
    igSetNextItemWidth(igGetFontSize() * 9);
    const char* fname = U.filter < 0 ? TR("Every Status") : TR(g_loc_status_names[U.filter]);
    if (igBeginCombo("##loc_filter", fname, 0)) {
        if (igSelectable_Bool(TR("Every Status"), U.filter < 0, 0, (ImVec2){ 0, 0 })) U.filter = -1; // tips: none (a status filter)
        for (int s = 0; s < LOC_STATUS_COUNT; s++)
            if (igSelectable_Bool(TR(g_loc_status_names[s]), U.filter == s, 0, (ImVec2){ 0, 0 })) U.filter = s; // tips: none (a status filter)
        igEndCombo();
    }
    if (U.rows_dirty || strcmp(U.find, U.last_find) || U.filter != U.last_filter || U.culture != U.last_culture || U.last_n != p->n) rebuild_rows(p);
    int c = U.culture;
    // (2026-11-25, item 21: the UI sweep's assert) the columns' weights need a stretch policy: without one ImGui's
    // TableSetupColumn asserted on every frame the panel was open
    if (!igBeginTable("loc_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_SizingStretchProp,
                      (ImVec2){ 0, igGetContentRegionAvail().y - igGetFrameHeightWithSpacing() }, 0))
        return;
    igTableSetupScrollFreeze(0, 1);
    igTableSetupColumn(TR("Where"), 0, 1.0f, 0), igTableSetupColumn(TR("Source"), 0, 1.4f, 0);
    igTableSetupColumn(p->targets[c], 0, 1.4f, 0), igTableSetupColumn(TR("Status"), 0, 0.6f, 0);
    igTableHeadersRow();
    ImGuiListClipper* clip = ImGuiListClipper_ImGuiListClipper();
    ImGuiListClipper_Begin(clip, (int)U.nrows, -1.0f);
    while (ImGuiListClipper_Step(clip))
        for (int r = clip->DisplayStart; r < clip->DisplayEnd; r++) {
            LocEntry* e = &p->e[U.rows[r]];
            igPushID_Int(U.rows[r]);
            igTableNextRow(0, 0);
            igTableNextColumn(), igTextDisabled("%s", e->where[0] ? e->where : e->key);
            igSetItemTooltip("%s / %s", e->ns, e->key);
            igTableNextColumn(), igTextWrapped("%s", e->source);
            igTableNextColumn();
            igSetNextItemWidth(-1);
            char buf[LOC_TEXT];
            snprintf(buf, sizeof buf, "%s", loc_tr(e, c));
            if (igInputText("##tr", buf, sizeof buf, 0, NULL, NULL)) {
                loc_tr_set(e, c, buf);
                e->tr[c].status = (uint8_t)(buf[0] ? LOC_TRANSLATED : LOC_MISSING);
                p->dirty = true;
            }
            igTableNextColumn();
            int st = e->tr[c].status;
            igSetNextItemWidth(-1);
            const char* sn[LOC_STATUS_COUNT];
            for (int s = 0; s < LOC_STATUS_COUNT; s++) sn[s] = TR(g_loc_status_names[s]);
            if (igCombo_Str_arr("##st", &st, sn, LOC_STATUS_COUNT, -1)) e->tr[c].status = (uint8_t)st, p->dirty = true;
            igPopID();
        }
    ImGuiListClipper_End(clip);
    ImGuiListClipper_destroy(clip);
    igEndTable();
    igTextDisabled(TR("%u text(s) shown"), U.nrows);
}

void loc_ui_draw(void) {
    if (!U.show) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 62, igGetFontSize() * 40 }, ImGuiCond_FirstUseEver);
    char title[128];
    snprintf(title, sizeof title, "%s###loc_dashboard", TR("Localization Dashboard"));
    if (!igBegin(title, &U.show, 0)) {
        igEnd();
        return;
    }
    LocProject* p = loc_project();
    igTextWrapped("%s", TR("The game's texts (Widgets, Blueprint and script Text, Dialogues, Quests, items) in other languages: gather them, translate them by hand or automatically, compile; the game switches with Set Current Culture or its Options' Language."));
    // the culture shown in Play / the editor's previews
    const char* cs[LOC_MAX_CULTURES + 1];
    int nc = loc_cultures(cs, LOC_MAX_CULTURES + 1);
    igSetNextItemWidth(igGetFontSize() * 9);
    if (igBeginCombo(TR("Preview Culture"), loc_culture(), 0)) {
        for (int i = 0; i < nc; i++)
            if (igSelectable_Bool(cs[i], !strcmp(cs[i], loc_culture()), 0, (ImVec2){ 0, 0 })) { // tips: none (a culture code)
                if (!loc_switch(cs[i])) snprintf(U.status, sizeof U.status, "%s", TR("Compile first: that culture has no compiled table."));
            }
        igEndCombo();
    }
    if (p->dirty) {
        igSameLine(0, -1);
        igTextDisabled("%s", TR("(unsaved)"));
    }
    if (igBeginTable("loc_top", 2, 0, (ImVec2){ 0, 0 }, 0)) {
        igTableNextRow(0, 0);
        igTableNextColumn();
        cultures_panel(p);
        igTableNextColumn();
        actions_panel(p);
        auto_panel(p);
        igEndTable();
    }
    if (U.status[0]) igTextWrapped("%s", U.status);
    igSeparatorText(TR("Texts"));
    table_panel(p);
    igEnd();
}
