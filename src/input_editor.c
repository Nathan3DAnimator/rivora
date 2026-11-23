// The Enhanced Input assets' editors (Unreal's): an Input Action (its value type, Consume Input, its triggers and
// modifiers) and an Input Mapping Context (its mappings: an action, a key, each one's modifiers and triggers).
#include "input_editor.h"

#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "i18n.h"
#include "input.h"
#include "ui_icons.h"
#include "ui_tips.h"

static struct {
    InputAction* action;
    InputContext* context;
    bool unsaved;
    char status[96];
} E;

void input_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    const char* dot = strrchr(vpath, '.');
    if (!dot) return;
    E.action = NULL, E.context = NULL, E.unsaved = false, E.status[0] = 0;
    if (!_stricmp(dot, ".rinputaction")) E.action = input_action_load(vpath);
    else if (!_stricmp(dot, ".rimc")) E.context = input_context_load(vpath);
}

bool input_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_GAMEPAD, TR("Input"), ok)) return false;
    char vp[700];
    if (ui_menu_item(ICON_GAMEPAD, TR("Input Action"), NULL, false, true)) {
        InputAction* a = input_action_create(folder_vpath, "IA_NewAction");
        if (a) {
            made = true;
            snprintf(vp, sizeof vp, "%s/%s.rinputaction", folder_vpath, a->name);
            input_editor_open(vp);
        }
    }
    if (ui_menu_item(ICON_SLIDERS, TR("Input Mapping Context"), NULL, false, true)) {
        InputContext* c = input_context_create(folder_vpath, "IMC_NewContext");
        if (c) {
            made = true;
            snprintf(vp, sizeof vp, "%s/%s.rimc", folder_vpath, c->name);
            input_editor_open(vp);
        }
    }
    igEndMenu();
    return made;
}

static bool mods_ui(InMod* mods, uint8_t* n) {
    bool ch = false;
    for (uint8_t i = 0; i < *n; i++) {
        InMod* m = &mods[i];
        igPushID_Int(i);
        int k = m->kind;
        igSetNextItemWidth(igGetFontSize() * 11.0f);
        if (igCombo_Str_arr("##mk", &k, g_in_mod_names, IMOD_COUNT, -1)) m->kind = (uint8_t)k, ch = true;
        igSameLine(0, 6);
        if (m->kind == IMOD_NEGATE) {
            bool x = m->flags & 1, y = m->flags & 2, z = m->flags & 4;
            if (igCheckbox("X", &x)) ch = true;
            igSameLine(0, 4);
            if (igCheckbox("Y", &y)) ch = true;
            igSameLine(0, 4);
            if (igCheckbox("Z", &z)) ch = true;
            m->flags = (uint8_t)(x | y << 1 | z << 2);
        } else if (m->kind == IMOD_SWIZZLE) {
            static const char* const orders[5] = { "YXZ", "ZYX", "XZY", "YZX", "ZXY" };
            int o = m->flags % 5;
            igSetNextItemWidth(igGetFontSize() * 5.0f);
            if (igCombo_Str_arr("##sw", &o, orders, 5, -1)) m->flags = (uint8_t)o, ch = true;
        } else if (m->kind == IMOD_DEADZONE) {
            igSetNextItemWidth(igGetFontSize() * 9.0f);
            ch |= igDragFloat2("##dz", m->v, 0.01f, 0.0f, 1.0f, "%.2f", 0);
            igSetItemTooltip("%s", TR("Lower and upper threshold"));
        } else {
            igSetNextItemWidth(igGetFontSize() * 12.0f);
            ch |= igDragFloat3("##sc", m->v, 0.01f, 0, 0, "%.2f", 0);
        }
        igSameLine(0, 6);
        if (igSmallButton("x")) {
            memmove(&mods[i], &mods[i + 1], sizeof(InMod) * (*n - i - 1));
            (*n)--, ch = true;
            igPopID();
            break;
        }
        igPopID();
    }
    if (*n < IN_MAX_MODS && igSmallButton(TR("+ Modifier"))) {
        InMod* m = &mods[(*n)++];
        memset(m, 0, sizeof *m);
        m->kind = IMOD_NEGATE, m->flags = 1, m->v[0] = m->v[1] = m->v[2] = 1.0f;
        ch = true;
    }
    return ch;
}

static bool trigs_ui(InTrig* t, uint8_t* n) {
    bool ch = false;
    for (uint8_t i = 0; i < *n; i++) {
        igPushID_Int(100 + i);
        int k = t[i].kind;
        igSetNextItemWidth(igGetFontSize() * 9.0f);
        if (igCombo_Str_arr("##tk", &k, g_in_trig_names, ITRG_COUNT, -1)) t[i].kind = (uint8_t)k, ch = true;
        if (t[i].kind >= ITRG_HOLD) {
            igSameLine(0, 6);
            igSetNextItemWidth(igGetFontSize() * 6.0f);
            ch |= igDragFloat("##tt", &t[i].time, 0.01f, 0.0f, 0.0f, "%.2f s", 0);
            igSetItemTooltip("%s", t[i].kind == ITRG_TAP ? TR("Released within this time") : t[i].kind == ITRG_PULSE ? TR("Every so often while held") : TR("Held this long"));
        }
        if (t[i].kind == ITRG_HOLD) {
            igSameLine(0, 6);
            ch |= igCheckbox(PL(TR("Once")), &t[i].once);
        }
        igSameLine(0, 6);
        if (igSmallButton("x")) {
            memmove(&t[i], &t[i + 1], sizeof(InTrig) * (*n - i - 1));
            (*n)--, ch = true;
            igPopID();
            break;
        }
        igPopID();
    }
    if (*n < IN_MAX_TRIGS && igSmallButton(TR("+ Trigger"))) {
        t[(*n)++] = (InTrig){ ITRG_PRESSED, true, 0.5f, 0.5f };
        ch = true;
    }
    return ch;
}

static bool action_ui(InputAction* a) {
    bool ch = false;
    igSeparatorText(TR("Action"));
    int vt = a->value_type;
    if (igCombo_Str_arr(PL(TR("Value Type")), &vt, g_in_value_names, IAV_COUNT, -1)) a->value_type = (uint8_t)vt, ch = true;
    ch |= igCheckbox(PL(TR("Consume Input")), &a->consume);
    igSetItemTooltip("%s", TR("Its keys don't reach the lower priority contexts."));
    igSeparatorText(TR("Triggers"));
    igTextDisabled("%s", TR("None: Down (triggers every frame it is held)."));
    ch |= trigs_ui(a->trig, &a->ntrig);
    igSeparatorText(TR("Modifiers"));
    ch |= mods_ui(a->mods, &a->nmods);
    return ch;
}

static bool context_ui(InputContext* c) {
    bool ch = false;
    igTextDisabled("%s", TR("Each mapping: an action and the key that drives it, its own modifiers and triggers."));
    for (uint32_t i = 0; i < c->nmaps; i++) {
        InputMapping* m = &c->maps[i];
        igPushID_Int((int)i);
        igSeparator();
        igSetNextItemWidth(igGetFontSize() * 12.0f);
        if (igBeginCombo("##act", m->action[0] ? m->action : TR("Action..."), 0)) {
            for (uint32_t k = 0; k < input_action_count(); k++) {
                const InputAction* a = input_action_at(k);
                if (igSelectable_Bool(a->name, !strcmp(a->name, m->action), 0, (ImVec2){ 0, 0 })) snprintf(m->action, IN_NAME, "%s", a->name), ch = true;
            }
            igEndCombo();
        }
        igSameLine(0, 6);
        igSetNextItemWidth(igGetFontSize() * 15.0f);
        if (igBeginCombo("##key", m->key[0] ? m->key : TR("Key..."), ImGuiComboFlags_HeightLarge)) {
            for (uint32_t k = 0; k < input_key_count(); k++)
                if (igSelectable_Bool(input_key_name(k), !strcmp(input_key_name(k), m->key), 0, (ImVec2){ 0, 0 }))
                    snprintf(m->key, IN_NAME, "%s", input_key_name(k)), ch = true;
            igEndCombo();
        }
        igSameLine(0, 6);
        if (igSmallButton(TR("Delete"))) {
            memmove(&c->maps[i], &c->maps[i + 1], sizeof(InputMapping) * (c->nmaps - i - 1));
            c->nmaps--, ch = true;
            igPopID();
            break;
        }
        igIndent(igGetFontSize());
        ch |= mods_ui(m->mods, &m->nmods);
        ch |= trigs_ui(m->trig, &m->ntrig);
        igUnindent(igGetFontSize());
        igPopID();
    }
    igSeparator();
    if (c->nmaps < IN_MAX_MAPS && igButton(TR("+ Mapping"), (ImVec2){ 0, 0 })) {
        InputMapping* m = &c->maps[c->nmaps++];
        memset(m, 0, sizeof *m);
        ch = true;
    }
    return ch;
}

void input_editor_draw(void) {
    if (!E.action && !E.context) return;
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###input_editor", E.action ? E.action->name : E.context->name, E.unsaved ? "*" : "",
             E.action ? TR("Input Action") : TR("Input Mapping Context"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 40, igGetFontSize() * 32 }, ImGuiCond_FirstUseEver);
    bool open = true;
    if (igBegin(title, &open, 0)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = E.action ? input_action_save(E.action) : input_context_save(E.context);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        E.unsaved |= E.action ? action_ui(E.action) : context_ui(E.context);
    }
    igEnd();
    if (!open) {
        if (E.unsaved) E.action ? input_action_save(E.action) : input_context_save(E.context);
        E.action = NULL, E.context = NULL;
    }
}
