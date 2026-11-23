// The UI's fades ([Real Engine] patches in imgui.cpp) never hide a popup: a combo opened a second time (its popup
// window recycled) used to begin with alpha 0, ImGui skipped it and BeginComboPopup asserted (the 0.9 crashes in the
// Blueprint, Anim Graph and AI editors). Headless: no window, no GPU.
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

static int g_fail, g_asserts;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

void re_imgui_assert_fail(const char* expr, const char* file, int line) {
    printf("IM_ASSERT(%s) %s:%d\n", expr, file, line);
    g_asserts++;
}

// A long combo (sugestão #60): its search field, the rows that pass, Enter picking the first
bool re_imgui_combo_filter_pass(const char* label);
static ImGuiID g_long;
static int g_open_long, g_long_shown, g_long_pass;
static const char* g_picked;
static const char* const g_rows[] = { "Item00", "Item01", "Item02", "Item03", "Item04", "Item05", "Item06", "Item07",
                                      "Item08", "Item09", "Item10", "Item11", "Sphere", "Cube", "Cylinder", "Tube" };
static ImGuiID g_combo, g_popup;
static int g_open_combo, g_open_popup, g_close; // requests for the next frame
static int g_combo_shown, g_popup_shown;

static void frame(void) {
    ImGuiIO* io = igGetIO_Nil();
    io->DeltaTime = 1.0f / 300.0f;
    igNewFrame();
    igSetNextWindowPos((ImVec2){ 10, 10 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize((ImVec2){ 400, 300 }, ImGuiCond_Always);
    igBegin("Details", NULL, 0);
    g_combo = igImHashStr("##ComboPopup", 0, igGetID_Str("Parent")); // (BeginCombo's popup id)
    if (g_open_combo) igOpenPopup_ID(g_combo, 0), g_open_combo = 0;
    g_combo_shown = 0;
    if (igBeginCombo("Parent", "Root", 0)) {
        g_combo_shown = 1;
        igSelectable_Bool("Root", true, 0, (ImVec2){ 0, 0 });
        if (g_close) igCloseCurrentPopup();
        igEndCombo();
    }
    g_long = igImHashStr("##ComboPopup", 0, igGetID_Str("Mesh"));
    if (g_open_long) igOpenPopup_ID(g_long, 0), g_open_long = 0;
    g_long_shown = g_long_pass = 0;
    if (igBeginCombo("Mesh", "Cube", 0)) {
        g_long_shown = 1;
        for (int i = 0; i < (int)(sizeof g_rows / sizeof g_rows[0]); i++) {
            g_long_pass += re_imgui_combo_filter_pass(g_rows[i]);
            if (igSelectable_Bool(g_rows[i], false, 0, (ImVec2){ 0, 0 })) g_picked = g_rows[i];
        }
        igEndCombo();
    }
    if (g_open_popup) igOpenPopup_Str("menu", 0), g_open_popup = 0;
    g_popup_shown = 0;
    if (igBeginPopup("menu", 0)) {
        g_popup_shown = 1;
        igTextUnformatted("Item", NULL);
        if (g_close) igCloseCurrentPopup();
        igEndPopup();
    }
    g_close = 0;
    igEnd();
    igRender();
}

int main(void) {
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->DisplaySize = (ImVec2){ 1280, 720 };
    io->IniFilename = NULL; // (no imgui.ini left where it runs)
    io->BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // (the font atlas built on demand, never uploaded)
    for (int i = 0; i < 5; i++) frame();

    // A combo and a popup opened, closed and opened again (the second time their windows are recycled): every frame
    // they stay open, Begin shows them, and nothing asserts.
    for (int round = 0; round < 3; round++) {
        g_open_combo = 1;
        for (int i = 0; i < 40; i++) {
            frame();
            CHECK(g_combo_shown);
        }
        g_close = 1;
        frame();
        for (int i = 0; i < 5; i++) frame();
        CHECK(!igIsPopupOpen_ID(g_combo, 0));

        g_open_popup = 1;
        for (int i = 0; i < 40; i++) {
            frame();
            CHECK(g_popup_shown);
        }
        g_close = 1;
        frame();
        for (int i = 0; i < 5; i++) frame();
    }
    // The long combo: a search field after its first frame (16 rows), typing filters the rows, Enter picks the first
    g_open_long = 1;
    for (int i = 0; i < 4; i++) frame();
    CHECK(g_long_shown && g_long_pass == 16);
    ImGuiIO_AddInputCharactersUTF8(io, "UBE"); // (case-insensitive: Cube, Tube)
    for (int i = 0; i < 3; i++) frame();
    CHECK(g_long_pass == 2);
    ImGuiIO_AddKeyEvent(io, ImGuiKey_Enter, true);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiKey_Enter, false);
    for (int i = 0; i < 3; i++) frame();
    CHECK(g_picked && !strcmp(g_picked, "Cube"));
    CHECK(!igIsPopupOpen_ID(g_long, 0));
    g_open_long = 1; // opened again: a fresh search
    for (int i = 0; i < 4; i++) frame();
    CHECK(g_long_shown && g_long_pass == 16);
    CHECK(g_asserts == 0);
    igDestroyContext(NULL);
    printf(g_fail ? "ui_popups: %d failures\n" : "ui_popups: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
