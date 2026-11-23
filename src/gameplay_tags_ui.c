// The Gameplay Tags' editor side (gameplay_tags.h): the picker (Unreal's tag drop-down: a search box over the project's
// tree) used by the Details and the tag nodes' pins, and the Project Settings page that keeps the list
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "gameplay_tags.h"
#include "i18n.h"

// The depth of a tag (dots) and its last part
static int depth_of(const char* t) {
    int d = 0;
    for (; *t; t++) d += *t == '.';
    return d;
}
static const char* leaf_of(const char* t) {
    const char* d = strrchr(t, '.');
    return d ? d + 1 : t;
}
static bool contains_ci(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && hay[i] && (hay[i] | 32) == (needle[i] | 32)) i++;
        if (!needle[i]) return true;
    }
    return false;
}

// The tree rows: each tag of the list with its missing parents shown above it (a list of "A.B.C" shows A, A.B, A.B.C)
// as one sorted pass. The rows the search keeps: a tag that matches and its parents
typedef struct Row { char tag[GT_TAG_LEN]; bool listed; } Row;
static uint32_t tree_rows(Row* rows, uint32_t cap) {
    uint32_t n = 0, count = gt_project_count();
    for (uint32_t i = 0; i < count; i++) {
        const char* t = gt_project_at(i);
        // the parents first (once: the list is sorted, so a parent shown is the previous rows')
        for (const char* d = strchr(t, '.'); d && n < cap; d = strchr(d + 1, '.')) {
            char parent[GT_TAG_LEN];
            size_t l = (size_t)(d - t);
            memcpy(parent, t, l), parent[l] = 0;
            bool seen = false;
            for (uint32_t k = n; k-- > 0 && !seen;) seen = !_stricmp(rows[k].tag, parent);
            if (!seen) snprintf(rows[n].tag, GT_TAG_LEN, "%s", parent), rows[n++].listed = false;
        }
        bool seen = false;
        for (uint32_t k = n; k-- > 0 && !seen;)
            if (!_stricmp(rows[k].tag, t)) seen = true, rows[k].listed = true;
        if (!seen && n < cap) snprintf(rows[n].tag, GT_TAG_LEN, "%s", t), rows[n++].listed = true;
    }
    return n;
}
static bool row_shown(const Row* rows, uint32_t n, uint32_t i, const char* search) {
    if (!search[0]) return true;
    for (uint32_t k = i; k < n; k++) { // it, or a tag under it, matches
        if (k > i && !gt_matches(rows[k].tag, rows[i].tag, false)) break;
        if (contains_ci(rows[k].tag, search)) return true;
    }
    return false;
}

static Row g_rows[GT_PROJECT_MAX * 2];

bool gt_tag_picker(const char* id, char* tag, size_t cap, float width) {
    bool changed = false;
    igPushID_Str(id);
    if (width > 0.0f) igSetNextItemWidth(width);
    static char search[GT_TAG_LEN];
    if (igBeginCombo("##tagpick", tag[0] ? tag : TR("None"), ImGuiComboFlags_HeightLarge)) {
        if (igIsWindowAppearing()) igSetKeyboardFocusHere(0), search[0] = 0;
        igSetNextItemWidth(-1);
        bool enter = igInputTextWithHint("##tagsearch", TR("Search or add a tag"), search, sizeof search, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        char clean[GT_TAG_LEN];
        bool typed = gt_clean(search, clean, sizeof clean);
        if (typed && gt_project_index(clean) < 0) { // a new name: added to the project's list and chosen
            char label[GT_TAG_LEN + 40];
            snprintf(label, sizeof label, "+ %s \"%s\"", TR("Add Tag"), clean);
            if (igSelectable_Bool(label, false, 0, (ImVec2){ 0, 0 }) || enter) {
                gt_project_add(clean);
                snprintf(tag, cap, "%s", clean), changed = true;
                igCloseCurrentPopup();
            }
        } else if (enter && typed) snprintf(tag, cap, "%s", gt_project_at((uint32_t)gt_project_index(clean))), changed = true, igCloseCurrentPopup();
        if (igSelectable_Bool(TR("None"), !tag[0], 0, (ImVec2){ 0, 0 })) tag[0] = 0, changed = true;
        uint32_t n = tree_rows(g_rows, sizeof g_rows / sizeof g_rows[0]);
        if (!n) igTextDisabled("%s", TR("The project has no tags yet: type one above (Project Settings > Gameplay Tags keeps the list)."));
        for (uint32_t i = 0; i < n; i++) {
            if (!row_shown(g_rows, n, i, search)) continue;
            igPushID_Int((int)i);
            igIndent(igGetFontSize() * (float)depth_of(g_rows[i].tag) + 1.0f);
            bool sel = !_stricmp(tag, g_rows[i].tag);
            if (igSelectable_Bool(leaf_of(g_rows[i].tag), sel, 0, (ImVec2){ 0, 0 })) snprintf(tag, cap, "%s", g_rows[i].tag), changed = true;
            if (igIsItemHovered(0)) igSetTooltip("%s", g_rows[i].tag);
            if (sel && igIsWindowAppearing()) igSetScrollHereY(0.5f);
            igUnindent(igGetFontSize() * (float)depth_of(g_rows[i].tag) + 1.0f);
            igPopID();
        }
        igEndCombo();
    }
    if (tag[0] && igIsItemHovered(0)) igSetTooltip("%s", tag);
    igPopID();
    return changed;
}

void gt_settings_page(World* w) {
    static char add[GT_TAG_LEN], search[GT_TAG_LEN], renaming[GT_TAG_LEN], rename_to[GT_TAG_LEN];
    static char status[160];
    igSeparatorText(TR("Gameplay Tag List"));
    igTextDisabled("%s", TR("The project's tags: the dots make the tree (Damage.Fire is under Damage). The Details and the tag nodes choose from it."));
    igSetNextItemWidth(igGetFontSize() * 16.0f);
    bool enter = igInputTextWithHint("##gtadd", "Damage.Fire", add, sizeof add, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSameLine(0, -1);
    if ((igButton(TR("Add New Gameplay Tag"), (ImVec2){ 0, 0 }) || enter) && add[0]) {
        if (gt_project_add(add)) add[0] = 0;
    }
    igSameLine(0, -1);
    if (igButton(TR("Add the Level's Tags"), (ImVec2){ 0, 0 })) {
        uint32_t n = gt_project_import_level(w);
        snprintf(status, sizeof status, TR("%u tags of the level's actors added."), n);
    }
    igSetNextItemWidth(igGetFontSize() * 16.0f);
    igInputTextWithHint("##gtfind", TR("Search..."), search, sizeof search, 0, NULL, NULL);
    if (status[0]) igSameLine(0, -1), igTextDisabled("%s", status);
    igBeginChild_Str("gttree", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, 0);
    uint32_t n = tree_rows(g_rows, sizeof g_rows / sizeof g_rows[0]);
    if (!n) igTextDisabled("%s", TR("No tags yet."));
    const char* remove = NULL;
    static char removing[GT_TAG_LEN];
    for (uint32_t i = 0; i < n; i++) {
        if (!row_shown(g_rows, n, i, search)) continue;
        igPushID_Int((int)i);
        float ind = igGetFontSize() * (float)depth_of(g_rows[i].tag) + 1.0f;
        igIndent(ind);
        if (renaming[0] && !_stricmp(renaming, g_rows[i].tag)) {
            igSetNextItemWidth(igGetFontSize() * 14.0f);
            if (igIsWindowAppearing() || !igIsAnyItemActive()) igSetKeyboardFocusHere(0);
            bool done = igInputText("##rn", rename_to, sizeof rename_to, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
            igSameLine(0, -1);
            if (igButton(TR("Rename"), (ImVec2){ 0, 0 }) || done) {
                uint32_t k = gt_project_rename(w, renaming, rename_to);
                snprintf(status, sizeof status, TR("Renamed: %u tags and actors."), k);
                renaming[0] = 0;
            }
            igSameLine(0, -1);
            if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) renaming[0] = 0;
        } else {
            if (g_rows[i].listed) igText("%s", leaf_of(g_rows[i].tag));
            else igTextDisabled("%s", leaf_of(g_rows[i].tag)); // (only a parent of listed tags)
            if (igIsItemHovered(0)) igSetTooltip("%s", g_rows[i].tag);
            igSameLine(igGetWindowWidth() - igGetFontSize() * 11.0f, -1);
            if (igSmallButton(TR("Rename"))) snprintf(renaming, sizeof renaming, "%s", g_rows[i].tag), snprintf(rename_to, sizeof rename_to, "%s", g_rows[i].tag);
            igSameLine(0, -1);
            if (igSmallButton(TR("Add Sub-tag"))) snprintf(add, sizeof add, "%s.", g_rows[i].tag);
            igSameLine(0, -1);
            if (igSmallButton("x")) snprintf(removing, sizeof removing, "%s", g_rows[i].tag), remove = removing;
            if (igIsItemHovered(0)) igSetTooltip("%s", TR("Delete the tag and the tags under it from the list (the actors keep theirs)"));
        }
        igUnindent(ind);
        igPopID();
    }
    igEndChild();
    if (remove) {
        uint32_t k = gt_project_remove(remove);
        snprintf(status, sizeof status, TR("Deleted: %u tags."), k);
    }
}
