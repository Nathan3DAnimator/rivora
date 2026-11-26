// The editor's Details of the interaction's components (interaction.h, notas-dev/interacao.md): an Interactable's prompt,
// key, hold and condition; an Interactor's reach and highlight; a Door / Chest / Lever / Button's motion, its moving part,
// its target and a chest's items. Shared by the level's Details (editor.c) and a Blueprint's components (bp_editor.c:
// no world, so no actor to pick).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "i18n.h"
#include "interaction.h"
#include "inventory.h"
#include "ui_tips.h"

static const char* const g_keys[] = { "Default", "E", "F", "R", "Q", "G", "Enter" };
static const uint16_t g_key_codes[] = { 0, 'E', 'F', 'R', 'Q', 'G', 257 };

static bool key_combo(const char* label, uint16_t* key, bool allow_default) {
    int k = 0;
    int n = (int)(sizeof g_key_codes / sizeof g_key_codes[0]);
    for (int i = 0; i < n; i++)
        if (g_key_codes[i] == *key) k = i;
    const char* kn[sizeof g_keys / sizeof g_keys[0]];
    for (int i = 0; i < n; i++) kn[i] = TR(g_keys[i]);
    int first = allow_default ? 0 : 1;
    int sel = k - first < 0 ? 0 : k - first;
    if (igCombo_Str_arr(label, &sel, kn + first, n - first, -1)) {
        *key = g_key_codes[sel + first];
        return true;
    }
    return false;
}

static bool flag(const char* label, uint16_t* flags, uint16_t bit) {
    bool on = (*flags & bit) != 0;
    if (!igCheckbox(label, &on)) return false;
    *flags = on ? (uint16_t)(*flags | bit) : (uint16_t)(*flags & ~bit);
    return true;
}
static bool flag32(const char* label, uint32_t* flags, uint32_t bit) {
    bool on = (*flags & bit) != 0;
    if (!igCheckbox(label, &on)) return false;
    *flags = on ? *flags | bit : *flags & ~bit;
    return true;
}

bool ia_ui_interactable(CInteractable* c) {
    bool ch = false;
    ch |= flag(PL(TR("Interaction Enabled")), &c->flags, IA_ENABLED);
    ch |= igInputTextWithHint(PL(TR("Prompt Text")), TR("Open {0} (empty: the device's verb)"), c->prompt, sizeof c->prompt, 0, NULL, NULL);
    ch |= igInputTextWithHint(PL(TR("Display Name")), TR("{0} (empty: the actor's name)"), c->name, sizeof c->name, 0, NULL, NULL);
    ch |= igDragFloat(PL(TR("Interaction Range")), &c->range, 1.0f, 0.0f, 10000.0f, c->range > 0.0f ? "%.0f cm" : TR("the Interactor's"), 0);
    ch |= igDragFloat(PL(TR("View Angle")), &c->angle, 0.5f, 0.0f, 90.0f, c->angle > 0.0f ? "%.0f\xc2\xb0" : TR("the Interactor's"), 0);
    ch |= key_combo(PL(TR("Interact Key")), &c->key, true);
    ch |= igInputTextWithHint(PL(TR("Input Action")), "IA_Interact", c->action, sizeof c->action, 0, NULL, NULL); // i18n: none (an asset name)
    ch |= igDragFloat(PL(TR("Hold Time")), &c->hold_time, 0.05f, 0.0f, 30.0f, c->hold_time > 0.0f ? "%.2f s" : TR("a press"), 0);
    bool again = !(c->flags & IA_ONCE);
    if (igCheckbox(PL(TR("Can Repeat")), &again)) c->flags = again ? (uint16_t)(c->flags & ~IA_ONCE) : (uint16_t)(c->flags | IA_ONCE), ch = true;
    ch |= igDragInt(PL(TR("Priority")), &c->priority, 0.1f, -100, 100, "%d", 0);
    igSeparatorText(TR("Condition"));
    ch |= igInputTextWithHint(PL(TR("Required Tag")), "Item.Key.Red", c->require, sizeof c->require, 0, NULL, NULL);
    if (c->require[0]) {
        ch |= flag(PL(TR("Item in the Inventory")), &c->flags, IA_REQ_INVENTORY);
        ch |= flag(PL(TR("Tag on the Actor")), &c->flags, IA_REQ_TAGS);
        if (c->flags & IA_REQ_INVENTORY) ch |= flag(PL(TR("Consume Item")), &c->flags, IA_CONSUME);
    }
    igSeparatorText(TR("Smart Object")); // (Q27.10, city.h)
    { int sl = (int)c->so_slots; if (igDragInt(PL(TR("Smart Object Slots")), &sl, 0.1f, 0, 64, "%d", 0)) c->so_slots = (uint8_t)(sl < 0 ? 0 : sl > 64 ? 64 : sl), ch = true; }
    if (c->so_slots) { // (the hours in quarters: Set Smart Object in the game)
        float oh = (float)c->open_q * 0.25f, chh = (float)c->close_q * 0.25f;
        if (igDragFloat(PL(TR("Opens At")), &oh, 0.25f, 0.0f, 24.0f, "%.2f h", 0)) c->open_q = (uint8_t)(fminf(fmaxf(oh, 0.0f), 24.0f) * 4.0f + 0.5f), ch = true;
        if (igDragFloat(PL(TR("Closes At")), &chh, 0.25f, 0.0f, 24.0f, "%.2f h", 0)) c->close_q = (uint8_t)(fminf(fmaxf(chh, 0.0f), 24.0f) * 4.0f + 0.5f), ch = true;
    }
    igSeparatorText(TR("VR")); // (Q25, vr_pawn.h)
    ch |= flag(PL(TR("Grabbable")), &c->flags, IA_GRAB);
    if (c->flags & IA_GRAB) ch |= flag(PL(TR("Snap to Hand")), &c->flags, IA_GRAB_SNAP);
    ch |= flag(PL(TR("Grab Socket")), &c->flags, IA_GRAB_SOCKET);
    return ch;
}

bool ia_ui_interactor(CInteractor* r) {
    bool ch = false;
    ch |= igDragFloat(PL(TR("Interaction Range")), &r->range, 1.0f, 10.0f, 10000.0f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("View Angle")), &r->angle, 0.5f, 1.0f, 90.0f, "%.0f\xc2\xb0", 0);
    ch |= key_combo(PL(TR("Interact Key")), &r->key, false);
    ch |= igInputTextWithHint(PL(TR("Input Action")), "IA_Interact", r->action, sizeof r->action, 0, NULL, NULL); // i18n: none (an asset name)
    ch |= flag(PL(TR("Outline")), &r->flags, IAR_OUTLINE);
    ch |= flag(PL(TR("Glow")), &r->flags, IAR_GLOW);
    ch |= flag(PL(TR("Show Prompt")), &r->flags, IAR_PROMPT);
    ch |= flag(PL(TR("Needs Line of Sight")), &r->flags, IAR_SIGHT);
    return ch;
}

// An actor of the level (any with a name and a transform but self): the moving part, the target
static bool actor_combo(World* w, Entity self, const char* label, uint32_t* who, const char* none) {
    const CName* cur = *who && ecs_alive(w, *who) ? ecs_get(w, *who, COMP_NAME) : NULL;
    bool ch = false;
    if (igBeginCombo(label, cur ? cur->text : none, 0)) {
        if (igSelectable_Bool(none, !*who, 0, (ImVec2){ 0, 0 })) *who = ENTITY_NONE, ch = true;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM);
            if ((ar->mask & need) != need || (ar->mask & COMP_BIT(COMP_ATTACH))) continue; // (the actors, not their components)
            const CName* nm = ecs_arch_column(ar, COMP_NAME);
            const Entity* ae = ecs_arch_entities(ar);
            for (uint32_t i = 0; i < ar->count; i++) {
                if (ae[i] == self) continue;
                igPushID_Int((int)ae[i]);
                if (igSelectable_Bool(nm[i].text, *who == ae[i], 0, (ImVec2){ 0, 0 })) *who = ae[i], ch = true;
                igPopID();
            }
        }
        igEndCombo();
    }
    return ch;
}

bool ia_ui_device(World* w, Entity self, CDevice* d) {
    bool ch = false;
    const char* kn[DEV_KIND_COUNT];
    for (int k = 0; k < DEV_KIND_COUNT; k++) kn[k] = TR(g_device_names[k]);
    int kind = (int)(d->kind < DEV_KIND_COUNT ? d->kind : 0);
    if (igCombo_Str_arr(PL(TR("Device")), &kind, kn, DEV_KIND_COUNT, -1)) d->kind = (uint32_t)kind, ch = true;
    const char* mn[DEV_MOTION_COUNT];
    for (int k = 0; k < DEV_MOTION_COUNT; k++) mn[k] = TR(g_device_motions[k]);
    int mo = (int)(d->motion < DEV_MOTION_COUNT ? d->motion : 0);
    if (igCombo_Str_arr(PL(TR("Motion")), &mo, mn, DEV_MOTION_COUNT, -1)) d->motion = (uint32_t)mo, ch = true;
    bool slide = d->motion == DEV_SLIDE;
    ch |= igDragFloat(PL((slide ? TR("Slide Distance") : TR("Open Angle"))), &d->amount, slide ? 1.0f : 0.5f, slide ? -1000.0f : -180.0f, slide ? 1000.0f : 180.0f,
                      slide ? "%.0f cm" : "%.0f\xc2\xb0", 0);
    ch |= igDragFloat(PL(TR("Open Time")), &d->time, 0.01f, 0.0f, 30.0f, "%.2f s", 0);
    ch |= igDragFloat(PL(TR("Auto Close")), &d->auto_close, 0.05f, 0.0f, 600.0f, d->auto_close > 0.0f ? "%.2f s" : TR("stays"), 0);
    ch |= flag32(PL(TR(slide ? "Reverse Direction" : "Hinge on the Right")), &d->flags, DEV_HINGE_RIGHT);
    ch |= flag32(PL(TR("Starts Open")), &d->flags, DEV_START_ACTIVE);
    if (d->motion == DEV_SWING) ch |= flag32(PL(TR("Opens Away")), &d->flags, DEV_AWAY);
    if (w) {
        ch |= actor_combo(w, self, PL(TR("Moving Part")), &d->part, TR("Self"));
        if (d->kind == DEV_LEVER || d->kind == DEV_BUTTON) {
            ch |= actor_combo(w, self, PL(TR("Target Actor")), &d->target, TR("None"));
            ch |= igInputTextWithHint(PL(TR("Custom Event")), TR("(empty: switch it)"), d->event, sizeof d->event, 0, NULL, NULL);
        }
    } else if (d->kind == DEV_LEVER || d->kind == DEV_BUTTON)
        ch |= igInputTextWithHint(PL(TR("Custom Event")), TR("(empty: switch it)"), d->event, sizeof d->event, 0, NULL, NULL);
    if (d->kind == DEV_CHEST) {
        igSeparatorText(TR("Chest Items"));
        for (int i = 0; i < DEV_ITEMS; i++) {
            if (i > 0 && !d->item[i] && !d->item[i - 1]) break; // (one empty row to add the next)
            igPushID_Int(i);
            float wd = igGetContentRegionAvail().x;
            igSetNextItemWidth(wd * 0.68f);
            if (inv_ui_item_combo("##item", &d->item[i])) {
                if (d->item[i] && !d->count[i]) d->count[i] = 1;
                ch = true;
            }
            igSameLine(0, 4);
            igSetNextItemWidth(-1);
            int n = d->count[i] ? (int)d->count[i] : 1;
            if (igDragInt("##count", &n, 0.2f, 1, 9999, "x %d", 0)) d->count[i] = (uint32_t)(n < 1 ? 1 : n), ch = true;
            igPopID();
        }
        igTextDisabled("%s", TR("Opened, the chest gives them to who opened it (what doesn't fit stays)."));
    }
    if (d->kind == DEV_LEVER || d->kind == DEV_BUTTON)
        igTextDisabled("%s", TR("Switch: a door opens or closes, a light goes on or off, any other actor shows or hides."));
    return ch;
}
