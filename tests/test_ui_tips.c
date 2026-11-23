// The descriptions (ui_tips.h): the table (no label twice, both languages), the lookup (English, translated, a menu's
// padded label, an icon button's ##id), and headless ImGui: the mouse resting on a button shows its description
// beside it (under it, not over it) after the delay, and not when the button has its own tooltip.
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"
#include "ui_tips.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

void re_imgui_assert_fail(const char* expr, const char* file, int line) {
    printf("IM_ASSERT(%s) %s:%d\n", expr, file, line);
    g_fail++;
}

static ImVec2 g_btn_min, g_btn_max;
static bool g_own_tip;

static ImGuiWindow* tooltip(void) {
    ImGuiWindow* t = igFindWindowByName("##Tooltip_00");
    return t && t->Active && !t->Hidden ? t : NULL;
}

static void frame(float dt) {
    ImGuiIO* io = igGetIO_Nil();
    io->DeltaTime = dt;
    igNewFrame();
    igSetNextWindowPos((ImVec2){ 10, 10 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize((ImVec2){ 400, 300 }, ImGuiCond_Always);
    igBegin("Toolbar", NULL, 0);
    igButton("Save", (ImVec2){ 0, 0 });
    g_btn_min = igGetItemRectMin(), g_btn_max = igGetItemRectMax();
    if (g_own_tip) igSetItemTooltip("its own");
    igEnd();
    ui_tips_frame();
    igRender();
}

int main(void) {
    // the table
    int n = 0;
    const UiTip* t = ui_tips_table(&n);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < i; k++)
            if (!strcmp(t[i].label, t[k].label)) printf("FAIL: label twice: \"%s\"\n", t[i].label), g_fail++;
        for (int l = 0; l < 2; l++)
            if (!t[i].text[l] || !t[i].text[l][0]) printf("FAIL: \"%s\" without text %d\n", t[i].label, l), g_fail++;
    }
    // the lookup
    const char* save = ui_tip_find("Save");
    CHECK(save && strstr(save, "Saves"));
    CHECK(ui_tip_find("      Save##menu") == save); // (a menu item: its icon's room, its id)
    CHECK(ui_tip_find("##tb_open") && strstr(ui_tip_find("##tb_open"), "level"));
    CHECK(ui_tip_find("not a widget") == NULL);
    i18n_set_language(LANG_PT_BR);
    CHECK(ui_tip_find(tr("Save")) && strstr(ui_tip_find(tr("Save")), "Salva")); // "Salvar": its msgid found
    i18n_set_language(LANG_EN);

    // headless ImGui: the mouse resting on the button
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->DisplaySize = (ImVec2){ 1280, 720 };
    io->IniFilename = NULL;
    io->BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    frame(0.016f);
    io->MousePos = (ImVec2){ (g_btn_min.x + g_btn_max.x) * 0.5f, (g_btn_min.y + g_btn_max.y) * 0.5f };
    frame(0.016f);
    CHECK(!strcmp(g_ui_hover.label, "Save"));
    frame(0.1f);
    CHECK(tooltip() == NULL); // (not at once)
    for (int i = 0; i < 8; i++) frame(0.1f);
    ImGuiWindow* tw = tooltip();
    CHECK(tw != NULL);
    if (tw) CHECK(tw->Pos.y >= g_btn_max.y); // beside it: under the button, never over it
    // a button with its own tooltip: no description over it
    g_own_tip = true;
    for (int i = 0; i < 10; i++) frame(0.1f);
    CHECK(g_ui_hover.tooltip_frame == igGetFrameCount());
    igDestroyContext(NULL);
    printf(g_fail ? "%d FAILED\n" : "ui_tips: %d descriptions, all passed\n", g_fail ? g_fail : n);
    return g_fail ? 1 : 0;
}
