// (Q19, notas-dev/importar-tlender.md) Window > Import Report: the last import's conversion report (import_report.h) as
// Unreal's Message Log shows an import: a row per item (what came in the same, was converted, baked or lost, and why),
// filtered by result and by text, the file it was written to shown in the Explorer. Opened by itself when an import
// lost something.
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"
#include "import_report.h"
#include "platform_files.h"
#include "theme.h"

static bool g_show;
static bool g_filter[IMPREP_KIND_COUNT] = { false, true, true, true }; // (the same ones hidden at first: the list is about changes)
static char g_search[96];

bool* imprep_show_flag(void) { return &g_show; }

static bool contains(const char* s, const char* q) { // (case ignored, ASCII)
    if (!q[0]) return true;
    for (const char* p = s; *p; p++) {
        size_t k = 0;
        while (q[k] && p[k] && ((p[k] | 32) == (q[k] | 32) || p[k] == q[k])) k++;
        if (!q[k]) return true;
    }
    return false;
}

void imprep_window(void) {
    if (!g_show) return;
    int n = 0;
    const char *source = "", *file = "";
    const ImpRepItem* items = imprep_items(&n, &source, &file);
    char title[160];
    snprintf(title, sizeof title, "%s###import_report", TR("Import Report"));
    igSetNextWindowSize((ImVec2){ 900, 420 }, ImGuiCond_FirstUseEver);
    if (!igBegin(title, &g_show, 0)) {
        igEnd();
        return;
    }
    unsigned count[IMPREP_KIND_COUNT] = { 0 };
    for (int i = 0; i < n; i++) count[items[i].kind] += items[i].count;
    static const int colors[IMPREP_KIND_COUNT] = { TK_TEXT_DIM, TK_INFO, TK_OK, TK_WARN };
    if (!source[0]) igTextDisabled("%s", TR("No import yet: Content > Import (.tlend, .obj, .gltf, .fbx)."));
    else igText("%s", source);
    for (int k = 0; k < IMPREP_KIND_COUNT; k++) {
        char lab[64];
        snprintf(lab, sizeof lab, "%s (%u)##imprep_k%d", TR(imprep_kind_name(k)), count[k], k);
        igPushStyleColor_U32(ImGuiCol_Text, TH(colors[k]));
        igCheckbox(lab, &g_filter[k]);
        igPopStyleColor(1);
        igSameLine(0, -1);
    }
    igSetNextItemWidth(igGetFontSize() * 14.0f);
    igInputTextWithHint("##imprep_search", TR("Search"), g_search, sizeof g_search, 0, NULL, NULL);
    igSameLine(0, -1);
    if (igButton(TR("Show Report File"), (ImVec2){ 0, 0 }) && file[0]) re_show_in_explorer(file);
    if (igBeginTable("##imprep_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                     (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Result"), ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 6.0f, 0);
        igTableSetupColumn(TR("Category"), ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 7.0f, 0);
        igTableSetupColumn(TR("Item"), ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 11.0f, 0);
        igTableSetupColumn(TR("Owner"), ImGuiTableColumnFlags_WidthFixed, igGetFontSize() * 9.0f, 0);
        igTableSetupColumn(TR("Reason"), ImGuiTableColumnFlags_WidthStretch, 0, 0);
        igTableHeadersRow();
        for (int i = 0; i < n; i++) {
            const ImpRepItem* it = &items[i];
            if (!g_filter[it->kind]) continue;
            if (g_search[0] && !contains(it->item, g_search) && !contains(it->owner, g_search) && !contains(it->reason, g_search)) continue;
            igTableNextRow(0, 0);
            igTableNextColumn();
            igTextColored(THV(colors[it->kind]), "%s", TR(imprep_kind_name(it->kind)));
            igTableNextColumn();
            igTextUnformatted(TR(imprep_cat_name(it->cat)), NULL);
            igTableNextColumn();
            if (it->count > 1) igText("%s x%u", it->item, it->count);
            else igTextUnformatted(it->item, NULL);
            igTableNextColumn();
            igTextUnformatted(it->owner, NULL);
            igTableNextColumn();
            igTextWrapped("%s", it->reason);
        }
        igEndTable();
    }
    igEnd();
}
