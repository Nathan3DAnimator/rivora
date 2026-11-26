// (2026-11-24, item 15) The game's texts gathered for the localization (loc.h): the Widgets' Text (Unreal's UMG), the
// Blueprints' and scripts' Text literals, the Dialogues, the Quests, the items. Each with a stable key: its asset's
// namespace ("W:", "BP:", "DLG:", "Q:", "ITEM:" + its name) and where it is in it. And the Blueprints' Text literals
// swapped in their classes' string tables when the culture changes (the originals kept here).
#include "loc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "dialogue.h"
#include "inventory.h"
#include "platform.h"
#include "quest.h"
#include "ui_widget.h"

// a Text literal's key: its text's hash (Unreal keys by a GUID kept in the asset; the text is what doesn't move here)
void loc_text_key(const char* text, char* out, size_t cap) {
    uint64_t h = loc_hash("T", text);
    snprintf(out, cap, "T_%016llx", (unsigned long long)h);
}

static uint32_t gather_widgets(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        const BpAsset* a = bp_asset_at(i);
        if (!a || !a->ui) continue;
        char ns[LOC_NS], key[LOC_KEY], where[160];
        snprintf(ns, sizeof ns, "W:%s", a->name);
        for (uint32_t k = 0; k < a->ui->n; k++) {
            const UiWidget* w = &a->ui->w[k];
            if (!w->text[0] || w->bind_text[0] || w->bind_setting[0]) continue; // (bound: the game writes it)
            if (w->kind != UIW_TEXT && w->kind != UIW_RICH_TEXT && w->kind != UIW_BUTTON) continue;
            snprintf(key, sizeof key, "%s.Text", w->name);
            snprintf(where, sizeof where, "Widget %s > %s", a->name, w->name);
            if (loc_gather_add(ns, key, w->text, where)) n++;
        }
    }
    return n;
}

static uint32_t gather_blueprints(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        const BpAsset* a = bp_asset_at(i);
        if (!a || a->kind != BPA_CLASS) continue;
        char ns[LOC_NS], key[LOC_KEY], where[160];
        snprintf(ns, sizeof ns, "BP:%s", a->name);
        for (uint32_t g = 0; g < a->ngraphs; g++)
            for (uint32_t k = 0; k < a->graphs[g].nnodes; k++) {
                const BpNode* nd = &a->graphs[g].nodes[k];
                for (int p = 0; p < nd->npins; p++) {
                    const BpPin* pin = &nd->pins[p];
                    if (pin->out || pin->type != BPT_TEXT || !pin->slit[0] || bp_pin_linked(&a->graphs[g], nd->id, (uint8_t)p)) continue;
                    loc_text_key(pin->slit, key, sizeof key);
                    snprintf(where, sizeof where, "Blueprint %s > %s > %s", a->name, a->graphs[g].name, pin->name);
                    if (loc_gather_add(ns, key, pin->slit, where)) n++;
                }
            }
        for (uint32_t v = 0; v < a->nvars; v++) // (a Text variable's default)
            if (a->vars[v].type == BPT_TEXT && a->vars[v].sdef[0]) {
                loc_text_key(a->vars[v].sdef, key, sizeof key);
                snprintf(where, sizeof where, "Blueprint %s > variable %s", a->name, a->vars[v].name);
                if (loc_gather_add(ns, key, a->vars[v].sdef, where)) n++;
            }
    }
    return n;
}

void loc_dlg_key(const Dialogue* d, const DlgNode* nd, char* ns, size_t ncap, char* key, size_t kcap) {
    snprintf(ns, ncap, "DLG:%s", d->name);
    snprintf(key, kcap, "n%d", (int)(nd - d->nodes));
}

static uint32_t gather_dialogues(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < dlg_count(); i++) {
        const Dialogue* d = dlg_at(i);
        for (uint32_t k = 0; d && k < d->nnodes; k++) {
            const DlgNode* nd = &d->nodes[k];
            if (!nd->text[0][0]) continue;
            char ns[LOC_NS], key[LOC_KEY], where[160];
            loc_dlg_key(d, nd, ns, sizeof ns, key, sizeof key);
            snprintf(where, sizeof where, "Dialogue %s > %s %u%s%s", d->name, nd->kind == DLG_OPTION ? "choice" : "line", k, nd->speaker[0] ? ": " : "",
                     nd->speaker);
            if (loc_gather_add(ns, key, nd->text[0], where)) n++;
        }
    }
    return n;
}

static uint32_t gather_quests(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < quest_count(); i++) {
        const Quest* q = quest_at(i);
        if (!q) continue;
        char ns[LOC_NS], key[LOC_KEY], where[160];
        snprintf(ns, sizeof ns, "Q:%s", q->name);
        snprintf(where, sizeof where, "Quest %s", q->name);
        if (q->title[0][0] && loc_gather_add(ns, "title", q->title[0], where)) n++;
        if (q->desc[0][0] && loc_gather_add(ns, "desc", q->desc[0], where)) n++;
        for (int s = 0; s < 8; s++)
            if (q->stages[s].text[0][0]) {
                snprintf(key, sizeof key, "stage%d", s);
                if (loc_gather_add(ns, key, q->stages[s].text[0], where)) n++;
            }
        for (int o = 0; o < 16; o++)
            if (q->obj[o].text[0][0]) {
                snprintf(key, sizeof key, "obj%d", o);
                if (loc_gather_add(ns, key, q->obj[o].text[0], where)) n++;
            }
    }
    return n;
}

static uint32_t gather_items(void) {
    uint32_t n = 0;
    inv_items_refresh();
    for (uint32_t i = 0; i < inv_item_count(); i++) {
        const ItemDef* it = inv_item_at(i);
        if (!it) continue;
        char ns[LOC_NS], where[160];
        snprintf(ns, sizeof ns, "ITEM:%s", it->name);
        snprintf(where, sizeof where, "Item %s", it->name);
        if (it->display[0] && loc_gather_add(ns, "name", it->display, where)) n++;
        if (it->description[0] && loc_gather_add(ns, "desc", it->description, where)) n++;
    }
    return n;
}

LocGatherStats loc_gather(void) {
    LocGatherStats s;
    memset(&s, 0, sizeof s);
    double t0 = re_time_seconds();
    LocProject* p = loc_project();
    uint32_t before = p->n;
    for (uint32_t i = 0; i < p->n; i++) p->e[i].stale = true; // (found again: live; else kept as stale)
    for (uint32_t i = 0; i < bp_asset_count(); i++) bp_asset_at(i); // (bp_asset_count scans the Content once)
    s.widgets = gather_widgets();
    s.blueprints = gather_blueprints();
    s.dialogues = gather_dialogues();
    s.quests = gather_quests();
    s.items = gather_items();
    s.total = s.widgets + s.blueprints + s.dialogues + s.quests + s.items;
    s.added = p->n - before;
    for (uint32_t i = 0; i < p->n; i++) s.stale += p->e[i].stale;
    p->dirty = true;
    s.ms = (re_time_seconds() - t0) * 1000.0;
    return s;
}

// A widget's text in the current culture: its class's authored text (not one Set Text wrote) by its key; NULL: none
const char* loc_widget_text(const struct BpAsset* cls, const struct UiWidget* w, int i) {
    if (!cls || !cls->ui || i < 0 || (uint32_t)i >= cls->ui->n || !loc_active() || w->bind_text[0] || w->bind_setting[0] ||
        strcmp(cls->ui->w[i].text, w->text))
        return NULL;
    char ns[LOC_NS], key[LOC_KEY];
    snprintf(ns, sizeof ns, "W:%s", cls->name), snprintf(key, sizeof key, "%s.Text", w->name);
    return loc_text(ns, key, NULL);
}

// An item's name or description in the current culture (ITEM:<name>, name / desc)
const char* loc_item(const struct ItemDef* it, bool desc) {
    if (!it) return "";
    const char* src = desc ? it->description : it->display;
    if (!loc_active()) return src;
    char ns[LOC_NS];
    snprintf(ns, sizeof ns, "ITEM:%s", it->name);
    return loc_text(ns, desc ? "desc" : "name", src);
}

bool loc_switch(const char* code) {
    bool ok = loc_set_culture(code);
    loc_bp_apply();
    return ok;
}

// --- the Blueprints' Text literals in the current culture: each class's string table swapped (its originals kept) ---
typedef struct BpOrig { const BpAsset* a; const void* code; uint32_t n; char (*s)[96]; } BpOrig;
static BpOrig* g_orig;
static uint32_t g_norig, g_corig;

void loc_bp_apply(void) {
    for (uint32_t i = 0; i < bp_asset_count(); i++) {
        BpAsset* a = bp_asset_at(i);
        if (!a || a->kind != BPA_CLASS || !a->nstrings) continue;
        BpOrig* o = NULL;
        for (uint32_t k = 0; k < g_norig && !o; k++)
            if (g_orig[k].a == a) o = &g_orig[k];
        if (!o) {
            if (g_norig == g_corig) {
                uint32_t cap = g_corig ? g_corig * 2 : 64;
                BpOrig* no = realloc(g_orig, sizeof *no * cap);
                if (!no) return;
                g_orig = no, g_corig = cap;
            }
            o = &g_orig[g_norig++];
            memset(o, 0, sizeof *o);
            o->a = a;
        }
        if (o->code != (const void*)a->code || o->n != a->nstrings) { // (compiled again: its table is the sources now)
            free(o->s);
            o->s = malloc(sizeof *o->s * a->nstrings);
            if (!o->s) {
                o->n = 0;
                continue;
            }
            memcpy(o->s, a->strings, sizeof *o->s * a->nstrings);
            o->code = a->code, o->n = a->nstrings;
        }
        char ns[LOC_NS], key[LOC_KEY];
        snprintf(ns, sizeof ns, "BP:%s", a->name);
        for (uint32_t k = 0; k < o->n; k++) {
            loc_text_key(o->s[k], key, sizeof key);
            snprintf(a->strings[k], sizeof a->strings[k], "%s", loc_text(ns, key, o->s[k]));
        }
    }
}
