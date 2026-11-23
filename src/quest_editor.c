// The Quest editor (quest.h): the quest's texts in three languages, its stages (one after another or all at once) with
// their objectives on the left, the picked objective's (or the quest's own) Details on the right, the rewards; a summary
// line of what the quest asks. Also the Quest Log component's Details.
#include "quest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "blueprint.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "platform.h"
#include "ui_icons.h"
#include "ui_tips.h"

static struct {
    Quest* q;
    bool unsaved;
    int sel;                   // the objective picked (-1: the quest; -2 - s: stage s)
    int lang;                  // the texts' tab
    char status[120];
} E = { .sel = -1 };

static const char* const g_lang_names[QST_LANGS] = { "English", "Português", "Español" };
static const char* const g_kind_hints[QOBJ_KIND_COUNT] = {
    "An actor's name or a Gameplay Tag on it (a Trigger Box, a place): the player walks into its Collision, or within the Radius.",
    "An actor's name or a Gameplay Tag on it with an Interactable (a Door, a Chest, a Lever): the player uses it.",
    "An Item Definition (DA_Herb): the player's Inventory holds Count of them.",
    "A Gameplay Tag of who dies (Enemy.Wolf covers Enemy.Wolf.Grey) or an actor's name: Count of them die (Health's On Death).",
    "An NPC's name or tag, or a Dialogue's name: a conversation with it ends.",
    "Nothing to watch: Advance Objective / Set Objective Progress move it (a puzzle, a cinematic)."
};

void quest_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    Quest* q = quest_load(vpath);
    if (!q) return;
    E.q = q, E.unsaved = false, E.sel = -1, E.status[0] = 0;
}

bool quest_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_DATA, TR("Quest"), ok)) return false;
    Quest* q = NULL;
    if (ui_menu_item(ICON_DATA, TR("Quest"), NULL, false, true)) q = quest_create(folder_vpath, "Q_New", false);
    igSetItemTooltip("%s", TR("A quest: stages with objectives (go to, interact, collect, kill, talk, manual) and its rewards. Start Quest starts it."));
    if (ui_menu_item(ICON_DATA, TR("Quest (Lost Amulet Example)"), NULL, false, true)) q = quest_create(folder_vpath, "Q_LostAmulet", true);
    igSetItemTooltip("%s", TR("Three stages with every kind of objective: go to the well, search the chest, gather herbs (optional), kill two wolves, talk to the elder; a reward."));
    igEndMenu();
    if (q) {
        char vp[700];
        snprintf(vp, sizeof vp, "%s/%s.rquest", folder_vpath, q->name);
        quest_tracker_asset(); // (WBP_QuestTracker next to it in Content/UI)
        quest_editor_open(vp);
        if (!E.q) E.q = q;
        made = true;
    }
    return made;
}

static bool field(const char* label, char* s, size_t cap, const char* hint) {
    return igInputTextWithHint(PL(label), hint ? hint : "", s, cap, 0, NULL, NULL);
}
// a text in the three languages (tabs); stride: the bytes between them
static bool langs(const char* id, char* t, size_t stride, bool multiline) {
    bool ch = false;
    igPushID_Str(id);
    if (igBeginTabBar("langs", 0)) {
        for (int l = 0; l < QST_LANGS; l++) {
            if (!igBeginTabItem(g_lang_names[l], NULL, 0)) continue;
            E.lang = l;
            char* s = t + (size_t)l * stride;
            igSetNextItemWidth(-1);
            if (multiline) ch |= igInputTextMultiline("##t", s, stride, (ImVec2){ -1, igGetFontSize() * 4.0f }, 0, NULL, NULL);
            else ch |= igInputText("##t", s, stride, 0, NULL, NULL);
            if (!s[0]) igTextDisabled("%s", l == 0 ? TR("(the text: English is the msgid)") : TR("(empty: the English text, or its translation in the engine's table)"));
            igEndTabItem();
        }
        igEndTabBar();
    }
    igPopID();
    return ch;
}

static bool quest_combo(const char* label, char* name, size_t cap, uint32_t skip) {
    bool ch = false;
    if (igBeginCombo(PL(label), name[0] ? name : TR("None"), 0)) {
        if (igSelectable_Bool(TR("None"), !name[0], 0, (ImVec2){ 0, 0 })) name[0] = 0, ch = true;
        for (uint32_t i = 0; i < quest_count(); i++) {
            Quest* o = quest_at(i);
            if (!o || o->hash == skip) continue;
            if (igSelectable_Bool(o->name, !_stricmp(o->name, name), 0, (ImVec2){ 0, 0 })) snprintf(name, cap, "%s", o->name), ch = true;
        }
        igEndCombo();
    }
    return ch;
}

static bool quest_details(Quest* q) {
    bool ch = false;
    igSeparatorText(TR("Quest"));
    igTextDisabled("%s", TR("Title"));
    ch |= langs("title", q->title[0], QST_TITLE, false);
    igTextDisabled("%s", TR("Description"));
    ch |= langs("desc", q->desc[0], QST_DESC, true);
    ch |= field(TR("Category"), q->category, sizeof q->category, "Main");
    bool par = q->parallel != 0, rep = q->repeatable != 0;
    if (igCheckbox(PL(TR("Stages at Once")), &par)) q->parallel = par, ch = true;
    if (igCheckbox(PL(TR("Repeatable")), &rep)) q->repeatable = rep, ch = true;
    ch |= igDragFloat(PL(TR("Fail After")), &q->time_limit, 1.0f, 0.0f, 86400.0f, q->time_limit > 0 ? "%.0f s" : TR("none"), 0);
    igSeparatorText(TR("Rewards"));
    for (int k = 0; k < QST_REWARDS; k++) {
        igPushID_Int(k);
        char lab[48];
        snprintf(lab, sizeof lab, "%s %d", TR("Item"), k + 1);
        ch |= field(lab, q->reward_item[k], sizeof q->reward_item[k], "DA_Coin");
        if (q->reward_item[k][0]) {
            if (q->reward_count[k] < 1) q->reward_count[k] = 1;
            ch |= igDragInt(PL(TR("Count")), &q->reward_count[k], 0.2f, 1, 9999, "%d", 0);
        }
        igPopID();
    }
    for (int k = 0; k < QST_REWARDS; k++) {
        igPushID_Int(10 + k);
        char lab[48];
        snprintf(lab, sizeof lab, "%s %d", TR("Gameplay Tag"), k + 1);
        ch |= field(lab, q->reward_tag[k], sizeof q->reward_tag[k], "Quest.Done");
        igPopID();
    }
    ch |= field(TR("Custom Event"), q->reward_event, sizeof q->reward_event, "OnQuestReward");
    ch |= field(TR("Event Target"), q->event_target, sizeof q->event_target, TR("(an actor's name; the Quest Log's always)"));
    ch |= quest_combo(TR("Next Quest"), q->next_quest, sizeof q->next_quest, q->hash);
    return ch;
}

static bool objective_details(Quest* q, QuestObjective* o) {
    bool ch = false;
    igSeparatorText(TR(g_qobj_kind_names[o->kind % QOBJ_KIND_COUNT]));
    int kind = o->kind;
    const char* kinds[QOBJ_KIND_COUNT];
    for (int i = 0; i < QOBJ_KIND_COUNT; i++) kinds[i] = TR(g_qobj_kind_names[i]);
    if (igCombo_Str_arr(PL(TR("Objective")), &kind, kinds, QOBJ_KIND_COUNT, -1)) o->kind = (uint8_t)kind, ch = true;
    igTextWrapped("%s", TR(g_kind_hints[o->kind % QOBJ_KIND_COUNT]));
    ch |= field(TR("ID"), o->id, sizeof o->id, "find_key");
    igTextDisabled("%s", TR("Text"));
    ch |= langs("otext", o->text[0], QST_TITLE, false);
    if (!o->text[0][0]) igTextDisabled("%s: %s", TR("Shown"), quest_obj_text(q, o, E.lang));
    if (o->kind != QOBJ_MANUAL) {
        static const char* const hints[QOBJ_KIND_COUNT] = { "OldWell", "Treasure Chest", "DA_Herb", "Enemy.Wolf", "Elder", "" };
        ch |= field(o->kind == QOBJ_COLLECT ? TR("Item") : TR("Target"), o->target, sizeof o->target, hints[o->kind % QOBJ_KIND_COUNT]);
    }
    if (o->kind == QOBJ_COLLECT || o->kind == QOBJ_KILL || o->kind == QOBJ_INTERACT || o->kind == QOBJ_MANUAL)
        ch |= igDragInt(PL(TR("Count")), &o->count, 0.2f, 1, 30000, "%d", 0);
    if (o->kind == QOBJ_GO_TO) ch |= igDragFloat(PL(TR("Radius")), &o->radius, 5.0f, 0.0f, 1e6f, o->radius > 0 ? "%.0f cm" : TR("its Collision"), 0);
    bool opt = (o->flags & QO_OPTIONAL) != 0;
    if (igCheckbox(PL(TR("Optional")), &opt)) o->flags = opt ? o->flags | QO_OPTIONAL : o->flags & ~QO_OPTIONAL, ch = true;
    if (o->kind == QOBJ_KILL) {
        bool by = (o->flags & QO_BY_OWNER) != 0;
        if (igCheckbox(PL(TR("Only the Player's Kills")), &by)) o->flags = by ? o->flags | QO_BY_OWNER : o->flags & ~QO_BY_OWNER, ch = true;
    }
    if (o->kind == QOBJ_COLLECT) {
        bool take = (o->flags & QO_TAKE_ITEMS) != 0;
        if (igCheckbox(PL(TR("Hand In the Items")), &take)) o->flags = take ? o->flags | QO_TAKE_ITEMS : o->flags & ~QO_TAKE_ITEMS, ch = true;
    }
    igSeparatorText(TR("Condition"));
    ch |= field(TR("Condition Tag"), o->condition, sizeof o->condition, "Quest.Elder.Trusts");
    if (o->condition[0]) {
        bool nt = (o->flags & QO_COND_NOT) != 0;
        if (igCheckbox(PL(TR("Not")), &nt)) o->flags = nt ? o->flags | QO_COND_NOT : o->flags & ~QO_COND_NOT, ch = true;
    }
    igSeparatorText(TR("Marker"));
    ch |= field(TR("Marker Actor"), o->marker, sizeof o->marker, TR("(the target; - none)"));
    igSeparatorText(TR("Stage"));
    int st = o->stage;
    if (q->nstages > 1 && igSliderInt(PL(TR("Stage")), &st, 1, q->nstages, "%d", 0) && st != o->stage) { // (moved: kept in the stages' order)
        QuestObjective keep = *o;
        int at = (int)(o - q->obj);
        quest_remove_objective(q, at);
        int n = quest_add_objective(q, st - 1, keep.kind);
        if (n >= 0) keep.stage = (uint8_t)(st - 1), q->obj[n] = keep, E.sel = n;
        return true;
    }
    return ch;
}

static bool stage_details(Quest* q, int s) {
    bool ch = false;
    char t[64];
    snprintf(t, sizeof t, "%s %d", TR("Stage"), s + 1);
    igSeparatorText(t);
    igTextDisabled("%s", TR("Text"));
    ch |= langs("stext", q->stages[s].text[0], QST_TITLE, false);
    bool io = q->stages[s].in_order != 0;
    if (igCheckbox(PL(TR("Objectives in Order")), &io)) q->stages[s].in_order = io, ch = true;
    igTextWrapped("%s", q->parallel ? TR("Stages at Once: every stage counts from the start; the quest completes when all are done.")
                                    : TR("The stages come one after another: this one starts when the one before is done."));
    return ch;
}

// the left side: the quest, its stages and their objectives
static bool tree(Quest* q) {
    bool ch = false;
    if (igSelectable_Bool(quest_title(q, E.lang), E.sel == -1, 0, (ImVec2){ 0, 0 })) E.sel = -1;
    for (int s = 0; s < q->nstages; s++) {
        igPushID_Int(s);
        char lab[160];
        const char* st = q->stages[s].text[0];
        snprintf(lab, sizeof lab, "%s %d%s%s", TR("Stage"), s + 1, st[0] ? ": " : "", st);
        igSpacing();
        if (igSelectable_Bool(lab, E.sel == -2 - s, 0, (ImVec2){ 0, 0 })) E.sel = -2 - s;
        igIndent(14.0f);
        for (int i = 0; i < q->nobj; i++) {
            if (q->obj[i].stage != s) continue;
            const QuestObjective* o = &q->obj[i];
            char ol[200];
            snprintf(ol, sizeof ol, "%s: %s%s##o%d", TR(g_qobj_kind_names[o->kind % QOBJ_KIND_COUNT]), quest_obj_text(q, o, E.lang),
                     (o->flags & QO_OPTIONAL) ? TR(" (Optional)") : "", i);
            if (igSelectable_Bool(ol, E.sel == i, 0, (ImVec2){ 0, 0 })) E.sel = i;
        }
        if (q->nobj < QST_MAX_OBJ && igSmallButton(TR("+ Objective"))) igOpenPopup_Str("add_obj", 0);
        if (igBeginPopup("add_obj", 0)) {
            for (int k = 0; k < QOBJ_KIND_COUNT; k++)
                if (ui_menu_item(ICON_QUEST, TR(g_qobj_kind_names[k]), NULL, false, true)) {
                    int n = quest_add_objective(q, s, (uint8_t)k);
                    if (n >= 0) E.sel = n, ch = true;
                }
            igEndPopup();
        }
        igSameLine(0, 6);
        if (igSmallButton(TR("Remove Stage"))) {
            quest_remove_stage(q, s), E.sel = -1, ch = true;
            igUnindent(14.0f);
            igPopID();
            break;
        }
        igUnindent(14.0f);
        igPopID();
    }
    igSpacing();
    if (q->nstages < QST_MAX_STAGES && igSmallButton(TR("+ Stage"))) {
        int s = quest_add_stage(q);
        if (s >= 0) E.sel = -2 - s, ch = true;
    }
    return ch;
}

void quest_editor_draw(struct Editor* ed) {
    (void)ed;
    if (!E.q) return;
    Quest* q = E.q;
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###quest_editor", q->name, E.unsaved ? "*" : "", TR("Quest"));
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.62f, mv->Size.y * 0.74f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.19f, mv->Pos.y + mv->Size.y * 0.1f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = quest_save(q);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        igSameLine(0, 10);
        int req = 0, opt = 0;
        for (int i = 0; i < q->nobj; i++) (q->obj[i].flags & QO_OPTIONAL) ? opt++ : req++;
        igTextDisabled(TR("%d stages, %d objectives (%d optional). Start Quest (\"%s\") starts it; WBP_QuestTracker shows it."), q->nstages, req + opt, opt, q->name);
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        ImVec2 avail = igGetContentRegionAvail();
        float left = avail.x * 0.42f;
        igBeginChild_Str("q_tree", (ImVec2){ left, avail.y }, ImGuiChildFlags_Borders, 0);
        ch |= tree(q);
        igEndChild();
        igSameLine(0, 4);
        igBeginChild_Str("q_details", (ImVec2){ 0, avail.y }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-igGetFontSize() * 8.0f);
        if (E.sel >= 0 && E.sel < q->nobj) {
            ch |= objective_details(q, &q->obj[E.sel]);
            igSpacing();
            if (E.sel >= 0 && E.sel < q->nobj && igButton(TR("Remove Objective"), (ImVec2){ 0, 0 })) quest_remove_objective(q, E.sel), E.sel = -1, ch = true;
        } else if (E.sel <= -2 && -2 - E.sel < q->nstages) ch |= stage_details(q, -2 - E.sel);
        else ch |= quest_details(q);
        igPopItemWidth();
        igEndChild();
    }
    igEnd();
    if (ch) E.unsaved = true, q->version++;
    if (!open) E.q = NULL;
}

// --- The Quest Log's Details ---

bool quest_ui_log(CQuestLog* c) {
    bool ch = false;
    bool trk = (c->flags & QL_TRACKER) != 0, mk = (c->flags & QL_MARKERS) != 0, at = (c->flags & QL_AUTO_TRACK) != 0;
    if (igCheckbox(PL(TR("Show Quest Tracker")), &trk)) c->flags = trk ? c->flags | QL_TRACKER : c->flags & ~QL_TRACKER, ch = true;
    if (igCheckbox(PL(TR("Show Markers")), &mk)) c->flags = mk ? c->flags | QL_MARKERS : c->flags & ~QL_MARKERS, ch = true;
    if (igCheckbox(PL(TR("Track New Quests")), &at)) c->flags = at ? c->flags | QL_AUTO_TRACK : c->flags & ~QL_AUTO_TRACK, ch = true;
    if (mk) ch |= igDragFloat(PL(TR("Marker Distance")), &c->marker_distance, 10.0f, 0.0f, 1e7f, c->marker_distance > 0 ? "%.0f cm" : TR("always"), 0);
    for (int k = 0; k < QL_START_QUESTS; k++) {
        igPushID_Int(k);
        Quest* cur = quest_find(c->start[k]);
        char lab[48];
        snprintf(lab, sizeof lab, "%s %d", TR("Starting Quest"), k + 1);
        if (igBeginCombo(PL(lab), cur ? cur->name : c->start[k] ? "?" : TR("None"), 0)) {
            if (igSelectable_Bool(TR("None"), !c->start[k], 0, (ImVec2){ 0, 0 })) c->start[k] = 0, ch = true;
            for (uint32_t i = 0; i < quest_count(); i++) {
                Quest* q = quest_at(i);
                if (q && igSelectable_Bool(q->name, q->hash == c->start[k], 0, (ImVec2){ 0, 0 })) c->start[k] = q->hash, ch = true;
            }
            if (!quest_count()) igTextDisabled("%s", TR("No Quest yet: Content Browser > Add > Quest."));
            igEndCombo();
        }
        if (cur) {
            igSameLine(0, 4);
            if (igSmallButton(TR("Edit"))) {
                char vp[700];
                snprintf(vp, sizeof vp, "%s", cur->path);
                const char* root = content_root();
                size_t rl = strlen(root);
                if (rl && !strncmp(cur->path, root, rl)) snprintf(vp, sizeof vp, "/Content%s", cur->path + rl);
                quest_editor_open(vp);
            }
        }
        igPopID();
        if (!c->start[k]) break; // (one empty row at a time)
    }
    static char wname[64];
    static uint32_t shown = UINT32_MAX;
    static const CQuestLog* shown_for;
    if (shown != c->widget || shown_for != c) {
        BpAsset* wb = c->widget ? bp_find_hash(c->widget) : NULL;
        snprintf(wname, sizeof wname, "%s", wb ? wb->name : "");
        shown = c->widget, shown_for = c;
    }
    if (igInputTextWithHint(PL(TR("Tracker Widget")), "WBP_QuestTracker", wname, sizeof wname, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
        BpAsset* f = wname[0] ? bp_find_name(BPA_CLASS, wname) : NULL;
        c->widget = f ? f->hash : 0u, ch = true;
    }
    igSetItemTooltip("%s", TR("A Widget Blueprint of yours with the same widgets' names (Panel, Title, Stage, Objective1..8); empty: WBP_QuestTracker."));
    return ch;
}
