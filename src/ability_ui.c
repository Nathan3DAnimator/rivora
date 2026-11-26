// The editor's Details of the Ability System (ability.h, notas-dev/habilidades.md): its Starting Abilities (the
// project's Gameplay Ability classes), Starting Effects (its Gameplay Effects), Starting Attributes and the player's
// options; shared by the level's Details (editor.c) and a Blueprint's components (bp_editor.c).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <stdio.h>
#include <string.h>

#include "ability.h"
#include "components.h"
#include "i18n.h"
#include "ui_tips.h"

// One of the project's Gameplay Abilities (effect false) or Gameplay Effects (the lists read again when it opens)
static bool def_combo(const char* label, uint32_t* hash, bool effect) {
    const AbilityDef* ad = effect ? NULL : abil_ability_by_hash(*hash);
    const EffectDef* ed = effect ? abil_effect_by_hash(*hash) : NULL;
    const char* cur = ad ? ad->display : ed ? ed->display : *hash ? "?" : TR("None");
    bool ch = false;
    if (igBeginCombo(label, cur, 0)) {
        abil_defs_refresh();
        if (igSelectable_Bool(TR("None"), !*hash, 0, (ImVec2){ 0, 0 })) *hash = 0, ch = true;
        uint32_t n = effect ? abil_effect_count() : abil_ability_count();
        for (uint32_t i = 0; i < n; i++) {
            const char* disp = effect ? abil_effect_at(i)->display : abil_ability_at(i)->display;
            const char* name = effect ? abil_effect_at(i)->name : abil_ability_at(i)->name;
            uint32_t h = effect ? abil_effect_at(i)->hash : abil_ability_at(i)->hash;
            char lab[128];
            snprintf(lab, sizeof lab, "%s  (%s)##%u", disp, name, i);
            if (igSelectable_Bool(lab, h == *hash, 0, (ImVec2){ 0, 0 })) *hash = h, ch = true;
        }
        if (!n) igTextDisabled("%s", (effect ? TR("No Gameplay Effect yet: Content Browser > Add > Gameplay Effect.") : TR("No Gameplay Ability yet: Content Browser > Add > Gameplay Ability.")));
        igEndCombo();
    }
    return ch;
}

bool abil_ui_system(CAbilitySystem* c) {
    bool ch = false;
    igSeparatorText(TR("Starting Attributes"));
    igSetNextItemWidth(-1);
    ch |= igInputTextWithHint("##attrs", "Mana 100, MaxMana 100, Stamina 50", c->attributes, sizeof c->attributes, 0, NULL, NULL); // i18n: none (an example of the attributes)
    igSetItemTooltip("%s", TR("Name and value, separated by commas. Speed is the Character's Max Walk Speed; Damage multiplies the damage it deals; Defense reduces what it takes; X stays within 0 and MaxX."));
    igSeparatorText(TR("Starting Abilities"));
    for (int i = 0; i < ABIL_START_ABILITIES; i++) {
        if (i > 0 && !c->ability[i] && !c->ability[i - 1]) break; // (one empty row to add the next)
        igPushID_Int(i);
        char key[8];
        snprintf(key, sizeof key, "%d", i + 1);
        igTextDisabled("%s", key);
        igSameLine(0, 6);
        igSetNextItemWidth(-1);
        ch |= def_combo("##ability", &c->ability[i], false);
        igPopID();
    }
    igSeparatorText(TR("Starting Effects"));
    for (int i = 0; i < ABIL_START_EFFECTS; i++) {
        if (i > 0 && !c->effect[i] && !c->effect[i - 1]) break;
        igPushID_Int(100 + i);
        igSetNextItemWidth(-1);
        ch |= def_combo("##effect", &c->effect[i], true);
        igPopID();
    }
    bool keys = (c->flags & ABIL_KEYS) != 0, bar = (c->flags & ABIL_SHOW_BAR) != 0;
    if (igCheckbox(TR("Activate with Keys 1-6"), &keys)) c->flags = keys ? (c->flags | ABIL_KEYS) : (c->flags & ~ABIL_KEYS), ch = true;
    if (igCheckbox(TR("Show Ability Bar"), &bar)) c->flags = bar ? (c->flags | ABIL_SHOW_BAR) : (c->flags & ~ABIL_SHOW_BAR), ch = true;
    igTextDisabled("%s", TR("Play gives them; Give Ability, Apply Gameplay Effect and Set Attribute change them."));
    return ch;
}
