// The Physical Material editor (physmat.h; notas-dev/fisica.md "Materiais físicos"): a small window like Unreal's
// asset Details: a preset to start from, the values with their descriptions, Save (Ctrl+S).
#include "editor_docs.h"
#include "physmat.h"

#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"
#include "ui_icons.h"
#include "ui_tips.h"

// --- The editor ---

static struct {
    bool open, unsaved;
    char vpath[600], status[128];
    PhysMaterial m;
} E;

void physmat_editor_open(const char* vpath) {
    snprintf(E.vpath, sizeof E.vpath, "%s", vpath);
    physmat_load(vpath, &E.m);
    physmat_remember(vpath);
    E.open = true, E.unsaved = false, E.status[0] = 0;
}

static bool combo_combine(const char* label, uint32_t* v, const char* tip) {
    const char* names[PCOMB_COUNT];
    for (int i = 0; i < PCOMB_COUNT; i++) names[i] = TR(g_phys_combine_names[i]);
    int c = (int)*v;
    bool ch = igCombo_Str_arr(label, &c, names, PCOMB_COUNT, -1);
    ui_item_tip(tip);
    if (ch) *v = (uint32_t)c;
    return ch;
}

void physmat_editor_draw(void) {
    if (!E.open) return;
    const char* name = strrchr(E.vpath, '/');
    char title[700];
    snprintf(title, sizeof title, "%s%s - %s###physmat_editor", name ? name + 1 : E.vpath, E.unsaved ? "*" : "", TR("Physical Material"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 30.0f, igGetFontSize() * 22.0f }, ImGuiCond_FirstUseEver);
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = physmat_save(E.vpath, &E.m);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        ui_item_tip(TR("Writes the material (Ctrl+S): every body using it takes the new values while playing too."));
        igSameLine(0, 10);
        if (igBeginCombo("##preset", TR("Preset"), 0)) {
            for (int i = 0; i < PHYSMAT_PRESETS; i++)
                if (igSelectable_Bool(TR(g_physmat_presets[i].name), false, 0, (ImVec2){ 0, 0 })) E.m = g_physmat_presets[i].m, ch = true;
            igEndCombo();
        }
        ui_item_tip(TR("Starts from a real material's values: wood, metal, stone, rubber, ice, plastic, glass."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        igSeparator();
        igPushItemWidth(-igGetFontSize() * 11.0f);
        ch |= igSliderFloat(TR("Friction"), &E.m.friction, 0.0f, 2.0f, "%.2f", 0);
        ui_item_tip(TR("How much it grips when sliding: 0 ice, 0.5 wood on wood, 1 rubber on concrete."));
        ch |= igSliderFloat(TR("Restitution"), &E.m.restitution, 0.0f, 1.0f, "%.2f", 0);
        ui_item_tip(TR("How much of the speed comes back from a hit: 0 a sandbag, 0.8 a rubber ball (hits slower than 1 m/s don't bounce)."));
        ch |= igDragFloat(TR("Density"), &E.m.density, 0.01f, 0.01f, 25.0f, "%.2f g/cm3", 0);
        ui_item_tip(TR("The mass of a body whose Mass is 0, by its volume: water 1, wood 0.6, stone 2.5, steel 7.8 g/cm3."));
        ch |= combo_combine(TR("Friction Combine"), &E.m.friction_combine,
                            TR("How two materials' frictions meet: Average, the smaller, multiplied or the larger. The pair uses the higher mode of the two (Max wins)."));
        ch |= combo_combine(TR("Restitution Combine"), &E.m.restitution_combine, TR("How two materials' bounces meet (as the friction's)."));
        igPopItemWidth();
        igSeparator();
        igTextWrapped("%s", TR("Use it in a body's Details > Physics > Physical Material, or with the Set Physical Material node."));
    }
    igEnd();
    if (ch) E.unsaved = true;
    if (!open) E.open = false;
}

// Content Browser > Add > Physics > Physical Material: one per preset, named after it, opened in its editor
bool physmat_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_PHYSICS_ASSET, TR("Physics"), ok)) return false;
    for (int i = 0; i < PHYSMAT_PRESETS; i++) {
        char label[96], name[64], vp[600];
        snprintf(label, sizeof label, "%s: %s", TR("Physical Material"), TR(g_physmat_presets[i].name));
        if (!ui_menu_item(ICON_PHYSICS_ASSET, label, NULL, false, true)) continue;
        snprintf(name, sizeof name, "PM_%s", g_physmat_presets[i].name);
        if (physmat_create(folder_vpath, name, vp, sizeof vp)) {
            physmat_save(vp, &g_physmat_presets[i].m);
            physmat_editor_open(vp);
            made = true;
        }
    }
    igEndMenu();
    return made;
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
static const char* physmat_editor_docs_base(const char* vpath) { const char* b = strrchr(vpath, '/'); return b ? b + 1 : vpath; }
int physmat_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    (void)ed;
    if (!E.open) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) return E.open = false, E.unsaved = false, 1;
    if (!E.unsaved) return 0;
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, physmat_editor_docs_base(E.vpath)), 1;
    bool ok = physmat_save(E.vpath, &E.m);
    if (ok) E.unsaved = false;
    return !ok;
}
