// The editor's Details of the inventory's components (inventory.h, notas-dev/inventario.md): an Inventory's settings
// and Starting Items, a Pickup's item (its mesh follows), shared by the level's Details (editor.c) and a Blueprint's
// components (bp_editor.c).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdio.h>
#include <string.h>

#include "components.h"
#include "i18n.h"
#include "inventory.h"
#include "ui_tips.h"

// An Item Definition chosen among the project's (the list read again when it opens: one just made is there)
bool inv_ui_item_combo(const char* label, uint32_t* item) {
    const ItemDef* cur = inv_item_by_hash(*item);
    bool ch = false;
    if (igBeginCombo(label, cur ? cur->display : *item ? "?" : TR("None"), 0)) {
        inv_items_refresh();
        if (igSelectable_Bool(TR("None"), !*item, 0, (ImVec2){ 0, 0 })) *item = 0, ch = true;
        for (uint32_t i = 0; i < inv_item_count(); i++) {
            const ItemDef* d = inv_item_at(i);
            char lab[128];
            snprintf(lab, sizeof lab, "%s  (%s)##%u", d->display, d->name, i);
            if (igSelectable_Bool(lab, d->hash == *item, 0, (ImVec2){ 0, 0 })) *item = d->hash, ch = true;
        }
        if (!inv_item_count()) igTextDisabled("%s", TR("No Item Definition yet: Content Browser > Add > Item Definition."));
        igEndCombo();
    }
    return ch;
}

static const char* const g_keys[] = { "None", "E", "F", "R", "Q", "G", "Enter" };
static const uint32_t g_key_codes[] = { 0, 'E', 'F', 'R', 'Q', 'G', 257 };

bool inv_ui_inventory(CInventory* c) {
    bool ch = false;
    int s = (int)c->slots;
    if (igDragInt(PL(TR("Slots")), &s, 0.2f, 1, INV_MAX_SLOTS, "%d", 0)) c->slots = (uint32_t)(s < 1 ? 1 : s > INV_MAX_SLOTS ? INV_MAX_SLOTS : s), ch = true;
    ch |= igDragFloat(PL(TR("Max Weight")), &c->max_weight, 0.5f, 0.0f, 100000.0f, c->max_weight > 0.0f ? "%.1f kg" : TR("no limit"), 0);
    bool ap = c->auto_pickup != 0;
    if (igCheckbox(PL(TR("Auto Pickup")), &ap)) c->auto_pickup = ap, ch = true;
    ch |= igDragFloat(PL(TR("Pickup Radius")), &c->pickup_radius, 1.0f, 10.0f, 5000.0f, "%.0f cm", 0);
    int k = 0;
    for (int i = 0; i < (int)(sizeof g_key_codes / sizeof g_key_codes[0]); i++)
        if (g_key_codes[i] == c->interact_key) k = i;
    const char* kn[sizeof g_keys / sizeof g_keys[0]];
    for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) kn[i] = TR(g_keys[i]);
    if (igCombo_Str_arr(PL(TR("Interact Key")), &k, kn, (int)(sizeof g_keys / sizeof g_keys[0]), -1)) c->interact_key = g_key_codes[k], ch = true;
    igSeparatorText(TR("Starting Items"));
    for (int i = 0; i < INV_START_ITEMS; i++) {
        if (i > 0 && !c->start_item[i] && !c->start_item[i - 1]) break; // (one empty row to add the next)
        igPushID_Int(i);
        float wd = igGetContentRegionAvail().x;
        igSetNextItemWidth(wd * 0.68f);
        if (inv_ui_item_combo("##item", &c->start_item[i])) {
            if (c->start_item[i] && !c->start_count[i]) c->start_count[i] = 1;
            ch = true;
        }
        igSameLine(0, 4);
        igSetNextItemWidth(-1);
        int n = c->start_count[i] ? (int)c->start_count[i] : 1;
        if (igDragInt("##count", &n, 0.2f, 1, 9999, "x %d", 0)) c->start_count[i] = (uint32_t)(n < 1 ? 1 : n), ch = true;
        igPopID();
    }
    igTextDisabled("%s", TR("Play gives them; Add Item, Pickups and WBP_Inventory change them."));
    return ch;
}

// A Pickup: its item (the actor's Static Mesh becomes the item's World Mesh, color and size), how many, how it's taken
bool inv_ui_pickup(CPickup* p, CStaticMesh* sm, float scale[3]) {
    bool ch = false;
    if (inv_ui_item_combo(PL(TR("Item")), &p->item)) {
        const ItemDef* d = inv_item_by_hash(p->item);
        if (d && sm) sm->mesh = d->mesh, memcpy(sm->base_color, d->color, 12);
        if (d && scale) scale[0] = scale[1] = scale[2] = d->attach_scale;
        ch = true;
    }
    int n = p->count ? (int)p->count : 1;
    if (igDragInt(PL(TR("Count")), &n, 0.2f, 1, 9999, "%d", 0)) p->count = (uint32_t)(n < 1 ? 1 : n), ch = true;
    int m = (int)p->mode;
    const char* ms[2] = { TR("On Touch"), TR("On Interact") };
    if (igCombo_Str_arr(PL(TR("Pick Up")), &m, ms, 2, -1)) p->mode = (uint32_t)m, ch = true;
    ch |= igDragFloat(PL(TR("Radius")), &p->radius, 1.0f, 0.0f, 5000.0f, p->radius > 0.0f ? "%.0f cm" : TR("the taker's"), 0);
    bool sp = p->spin != 0;
    if (igCheckbox(PL(TR("Spin")), &sp)) p->spin = sp, ch = true;
    ch |= igDragFloat(PL(TR("Respawn Time")), &p->respawn_time, 0.1f, 0.0f, 3600.0f, p->respawn_time > 0.0f ? "%.1f s" : TR("never"), 0);
    return ch;
}
