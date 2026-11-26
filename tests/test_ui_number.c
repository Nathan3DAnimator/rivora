// (2026-11-25, item 20 of the user's list) The number fields (src/ui_number.c) as Unreal's UI Min/Max against Clamp
// Min/Max, headless (no window, no GPU): a value typed past the slider's range (Ctrl+click, the digits, Enter) is kept
// as typed; dragging stays in the range; past the hard limit a typed value is held at it. A drag (int and float) and a
// slider (int). Before, the fields passed ImGuiSliderFlags_AlwaysClamp and a typed 384 became 128.
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "../src/ui_number.h"
#include "../src/toon.h"
#include "../src/mat_pack.h"

static int g_fail, g_asserts;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__), printf(__VA_ARGS__), printf("\n"), g_fail++; } } while (0)

void re_imgui_assert_fail(const char* expr, const char* file, int line) {
    printf("IM_ASSERT(%s) %s:%d\n", expr, file, line);
    g_asserts++;
}

enum { F_DRAG_INT, F_DRAG_FLOAT, F_SLIDER_INT, F_MAT, F_COUNT };
static int g_i[F_COUNT];
static float g_f;
static float g_mat, g_mat_lo, g_mat_hi = 3.4e38f; // (F_MAT: a Material Editor's slider, its hard limits the field's)
static ImVec2 g_center[F_COUNT];

static void center_of(int f) { // (the field just drawn: its middle)
    ImVec2_c a = igGetItemRectMin(), b = igGetItemRectMax();
    g_center[f] = (ImVec2){ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
}

static void frame(void) {
    ImGuiIO* io = igGetIO_Nil();
    io->DeltaTime = 1.0f / 60.0f;
    igNewFrame();
    igSetNextWindowPos((ImVec2){ 10, 10 }, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize((ImVec2){ 500, 300 }, ImGuiCond_Always);
    igBegin("Details", NULL, 0);
    // (the facility's Rooms: the slider 1..128, nothing hard below 100 000)
    ui_num_int("Rooms", &g_i[F_DRAG_INT], 1.0f, 1, 128, 1, 100000, "%d", 0);
    center_of(F_DRAG_INT);
    ui_num_float("Density", &g_f, 0.1f, 0.0f, 10.0f, 0.0f, 0.0f, "%.2f", 0); // (no hard limit)
    center_of(F_DRAG_FLOAT);
    ui_slider_int("Bands", &g_i[F_SLIDER_INT], 0, 8, 0, 64, "%d", 0); // (an array of 64: the hard limit)
    center_of(F_SLIDER_INT);
    ui_slider_float("Mat", &g_mat, 0.0f, 4.0f, g_mat_lo, g_mat_hi, "%.4f", 0); // (mat_editor.c's: the same limits per case)
    center_of(F_MAT);
    igEnd();
    igRender();
}

// Ctrl+click on the field, the text typed, Enter
static void type_into(int f, const char* text) {
    ImGuiIO* io = igGetIO_Nil();
    ImGuiIO_AddMousePosEvent(io, g_center[f].x, g_center[f].y);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, true);
    ImGuiIO_AddKeyEvent(io, ImGuiKey_LeftCtrl, true);
    frame();
    ImGuiIO_AddMouseButtonEvent(io, 0, true);
    frame();
    ImGuiIO_AddMouseButtonEvent(io, 0, false);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiKey_LeftCtrl, false);
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, false);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, true), ImGuiIO_AddKeyEvent(io, ImGuiKey_LeftCtrl, true), ImGuiIO_AddKeyEvent(io, ImGuiKey_A, true); // (all of it selected)
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiKey_A, false), ImGuiIO_AddKeyEvent(io, ImGuiKey_LeftCtrl, false), ImGuiIO_AddKeyEvent(io, ImGuiMod_Ctrl, false);
    frame();
    ImGuiIO_AddInputCharactersUTF8(io, text);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiKey_Enter, true);
    frame();
    ImGuiIO_AddKeyEvent(io, ImGuiKey_Enter, false);
    frame(), frame();
}

// the field dragged dx pixels (a press, moves, a release)
static void drag(int f, float dx) {
    ImGuiIO* io = igGetIO_Nil();
    ImGuiIO_AddMousePosEvent(io, g_center[f].x, g_center[f].y);
    frame();
    ImGuiIO_AddMouseButtonEvent(io, 0, true);
    frame();
    for (int k = 1; k <= 40; k++) {
        ImGuiIO_AddMousePosEvent(io, g_center[f].x + dx * (float)k / 40.0f, g_center[f].y);
        frame();
    }
    ImGuiIO_AddMouseButtonEvent(io, 0, false);
    frame(), frame();
}

int main(void) {
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->DisplaySize = (ImVec2){ 1280, 720 };
    io->IniFilename = NULL;
    io->BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    g_i[F_DRAG_INT] = 30, g_f = 1.0f, g_i[F_SLIDER_INT] = 4;
    for (int i = 0; i < 5; i++) frame();

    // typed past the range: kept as typed
    type_into(F_DRAG_INT, "384");
    printf("ui_number: Rooms (slider 1..128) typed 384 -> %d\n", g_i[F_DRAG_INT]);
    CHECK(g_i[F_DRAG_INT] == 384, "Rooms typed 384 should stay 384, is %d", g_i[F_DRAG_INT]);
    type_into(F_DRAG_FLOAT, "25.5");
    printf("ui_number: Density (slider 0..10) typed 25.5 -> %.2f\n", g_f);
    CHECK(g_f > 25.49f && g_f < 25.51f, "Density typed 25.5 should stay 25.5, is %.2f", g_f);
    type_into(F_SLIDER_INT, "20");
    printf("ui_number: Bands (slider 0..8, an array of 64) typed 20 -> %d\n", g_i[F_SLIDER_INT]);
    CHECK(g_i[F_SLIDER_INT] == 20, "Bands typed 20 should stay 20, is %d", g_i[F_SLIDER_INT]);
    // past the hard limit: held at it
    type_into(F_DRAG_INT, "500000");
    printf("ui_number: Rooms typed 500000 (hard limit 100 000) -> %d\n", g_i[F_DRAG_INT]);
    CHECK(g_i[F_DRAG_INT] == 100000, "Rooms typed 500000 should be held at 100000, is %d", g_i[F_DRAG_INT]);
    type_into(F_SLIDER_INT, "999");
    CHECK(g_i[F_SLIDER_INT] == 64, "Bands typed 999 should be held at 64, is %d", g_i[F_SLIDER_INT]);

    // dragging stays in the slider's range
    g_i[F_DRAG_INT] = 30, g_f = 1.0f;
    frame();
    drag(F_DRAG_INT, 900.0f);
    printf("ui_number: Rooms dragged far right from 30 -> %d (the slider's 128)\n", g_i[F_DRAG_INT]);
    CHECK(g_i[F_DRAG_INT] == 128, "Rooms dragged right should stop at 128, is %d", g_i[F_DRAG_INT]);
    drag(F_DRAG_INT, -900.0f);
    CHECK(g_i[F_DRAG_INT] == 1, "Rooms dragged left should stop at 1, is %d", g_i[F_DRAG_INT]);
    drag(F_DRAG_FLOAT, 900.0f);
    printf("ui_number: Density dragged far right from 1 -> %.2f (the slider's 10)\n", g_f);
    CHECK(g_f == 10.0f, "Density dragged right should stop at 10, is %.2f", g_f);
    g_i[F_SLIDER_INT] = 4;
    frame();
    drag(F_SLIDER_INT, 900.0f);
    printf("ui_number: Bands slid far right from 4 -> %d (the slider's 8)\n", g_i[F_SLIDER_INT]);
    CHECK(g_i[F_SLIDER_INT] == 8, "Bands slid right should stop at 8, is %d", g_i[F_SLIDER_INT]);

    // (2026-11-25, the tester: the Toon's fields took a typed value, toon_pack held it at the slider's: 8 bands, a hatch of
    // 2..64 px, an ambient of 4) 20 bands typed reach the GPU's pack as 20; a hatch of 1 px and an ambient of 6 as they are
    g_i[F_SLIDER_INT] = 4;
    frame();
    type_into(F_SLIDER_INT, "20");
    MatToon toon;
    memset(&toon, 0, sizeof toon);
    toon.bands = g_i[F_SLIDER_INT], toon.hatch_scale = 1.0f, toon.ambient = 6.0f, toon.nstops = 1;
    float pack[12][4];
    toon_pack(&toon, 0, pack);
    printf("ui_number: Toon Bands typed 20 -> the field %d, the GPU's pack %.0f; Hatch Spacing 1 px -> %.1f, Toon Ambient 6 -> %.1f\n", toon.bands,
           pack[0][3], pack[5][0], pack[6][1]);
    CHECK(pack[0][3] == 20.0f, "20 bands reach the GPU as %.0f", pack[0][3]);
    CHECK(pack[5][0] == 1.0f, "a hatch of 1 px reaches the GPU as %.1f", pack[5][0]);
    CHECK(pack[6][1] == 6.0f, "an ambient of 6 reaches the GPU as %.1f", pack[6][1]);
    toon.bands = 100000, toon.hatch_scale = 0.0f, toon.ambient = -1.0f;
    toon_pack(&toon, 0, pack);
    CHECK(pack[0][3] == (float)TOON_BANDS_MAX && pack[5][0] > 0.0f && pack[6][1] == 0.0f, "the real limits: %.0f bands, hatch %.2f, ambient %.1f", pack[0][3],
          pack[5][0], pack[6][1]);

    // (2026-11-25, the tester: the Material Editor's fields took a typed value, material_gpu.inc's pack held it at its
    // old tops) each field typed past its slider, through the pack's rule (mat_pack.h): a float as typed, an 8 bit one held
    // at its top by the field itself (the screen shows what the GPU gets)
    struct { const char* name; float lo, hi; const char* typed; float want_field, want_gpu; int which; } mc[] = {
        { "Iris Depth", 0.0f, 3.4e38f, "5", 5.0f, 5.0f, 0 },
        { "Metallic Flakes", 0.0f, 3.4e38f, "9", 9.0f, 9.0f, 1 },
        { "Detail Strength", 0.0f, 3.4e38f, "7", 7.0f, 7.0f, 2 },
        { "Dispersion", 0.0f, 3.4e38f, "3", 3.0f, 3.0f, 3 },
        { "Refraction", 0.0f, 3.4e38f, "10", 10.0f, 10.0f, 4 },
        { "Paint Flake Size", MAT_FLAKE_SIZE_MIN, 3.4e38f, "0.001", MAT_FLAKE_SIZE_MIN, MAT_FLAKE_SIZE_MIN, 5 },
        { "Eye Glow", 0.0f, MAT_GLOW8_MAX, "40", MAT_GLOW8_MAX, MAT_GLOW8_MAX, 6 },
        { "Retroreflection", 0.0f, MAT_GLOW8_MAX, "40", MAT_GLOW8_MAX, MAT_GLOW8_MAX, 6 },
    };
    for (size_t i = 0; i < sizeof mc / sizeof mc[0]; i++) {
        g_mat = 1.0f, g_mat_lo = mc[i].lo, g_mat_hi = mc[i].hi;
        frame();
        type_into(F_MAT, mc[i].typed);
        float v = g_mat, gpu;
        switch (mc[i].which) {
        case 0: gpu = mat_pack_iris_depth(v); break;
        case 1: gpu = mat_pack_flakes(v); break;
        case 2: gpu = mat_pack_detail(v); break;
        case 3: gpu = mat_pack_dispersion(v); break;
        case 4: gpu = mat_pack_refraction(v); break;
        case 5: gpu = mat_pack_flake_size(v); break;
        default: gpu = (float)mat_pack_glow8(v) / 16.0f; break; // (what the shader reads back: the 8 bits / 16)
        }
        printf("ui_number: %s typed %s -> the field %.4f, the GPU's pack %.4f\n", mc[i].name, mc[i].typed, v, gpu);
        CHECK(fabsf(v - mc[i].want_field) < 1e-3f && fabsf(gpu - mc[i].want_gpu) < 1e-3f && fabsf(gpu - v) < 1e-3f,
              "%s: typed %s, the field %.4f, the GPU %.4f (want %.4f both)", mc[i].name, mc[i].typed, v, gpu, mc[i].want_gpu);
    }

    igDestroyContext(NULL);
    CHECK(g_asserts == 0, "%d ImGui asserts", g_asserts);
    printf("ui_number test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
