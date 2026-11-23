// The themes (theme.h, notas-dev/temas.md): the ten built-in themes read, resolved and different from each other; each
// passes the contrast checks (WCAG: text 4.5:1, icons and controls 3:1); a .rtheme written and read back is the same
// theme; editing a copy changes the live table at once (the next frame) and cancelling restores it; duplicate, save,
// import and export in a folder of the test's own; ImGui's style follows the tokens; the cost of a color read and of a
// theme change. With --contrast [files.rtheme...]: the contrast report (tools/theme_contrast.py calls it).
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "platform.h"
#include "platform_files.h"
#include "theme.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

void re_imgui_assert_fail(const char* expr, const char* file, int line) {
    printf("IM_ASSERT(%s) %s:%d\n", expr, file, line);
    g_fail++;
}

static int contrast_report(int argc, char** argv) {
    static char out[16384];
    int bad = 0, n = 0;
    if (argc <= 2) { // the built-in themes
        for (int i = 0; i < THEME_BUILTIN_COUNT; i++, n++) {
            Theme t;
            theme_builtin(&t, i);
            int b = theme_check_contrast(&t, out, sizeof out);
            printf("%-20s %s\n", t.name, b ? "FAIL" : "ok");
            if (b) printf("%s", out);
            bad += b;
        }
    }
    for (int a = 2; a < argc; a++, n++) {
        Theme t;
        if (!theme_read_file(&t, argv[a])) {
            printf("%-20s cannot read %s\n", "?", argv[a]);
            bad++;
            continue;
        }
        int b = theme_check_contrast(&t, out, sizeof out);
        printf("%-20s %s (%s)\n", t.name, b ? "FAIL" : "ok", argv[a]);
        if (b) printf("%s", out);
        bad += b;
    }
    printf("contrast: %d theme(s), %d failing pair(s)\n", n, bad);
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    theme_init();
    if (argc > 1 && !strcmp(argv[1], "--contrast")) return contrast_report(argc, argv);

    // a folder of the test's own for the user's themes (never %APPDATA%)
    char dir[600];
    if (!re_cache_dir(dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
    char udir[700];
    snprintf(udir, sizeof udir, "%s\\test_themes", dir);
    re_make_dirs(udir);
    theme_set_user_dir(udir);
    theme_init();

    // the eleven built-ins, by name, resolved, with the plan's names (Q26: Azul Real, the default since 0.44)
    static const char* const names[THEME_BUILTIN_COUNT] = { "Órbita Escuro", "Grafite", "Meia-noite", "Claro", "Alto Contraste",
                                                            "Brasa", "Floresta", "Neon", "Sépia", "Daltônico seguro", "Azul Real" };
    CHECK(theme_count() >= THEME_BUILTIN_COUNT);
    for (int i = 0; i < THEME_BUILTIN_COUNT; i++) {
        const Theme* t = theme_at(i);
        CHECK(t && t->builtin);
        if (!t) continue;
        CHECK(!strcmp(t->name, names[i]));
        CHECK(theme_find(names[i]) == i);
        int opaque = 0;
        for (int k = 0; k < TK_COUNT; k++) opaque += (t->col[k] >> 24) == 255u;
        CHECK(opaque == TK_COUNT); // every token resolved (no hole left black-transparent)
        static char out[8192];
        int bad = theme_check_contrast(t, out, sizeof out);
        if (bad) printf("%s", out);
        CHECK(bad == 0);
    }
    CHECK(!strcmp(theme_current_name(), THEME_DEFAULT_NAME) && theme_current() == THEME_DEFAULT); // the default (Q26)
    CHECK(theme_find(THEME_PREVIOUS_DEFAULT_NAME) == 0); // (the old default: still there, the families' reference)
    // not Unreal's: no #151515 surfaces, no #0070E0 accent
    CHECK(TH(TK_BG_APP) != 0xFF151515u && TH(TK_ACCENT) != 0xFFE07000u);
    // the themes differ (surfaces, accent, the ramp) and a light theme turns the ramp around
    for (int i = 1; i < THEME_BUILTIN_COUNT; i++) {
        const Theme *a = theme_at(0), *b = theme_at(i);
        int diff = 0;
        for (int k = 0; k < TK_COUNT; k++) diff += a->col[k] != b->col[k];
        CHECK(diff > TK_COUNT / 4);
    }
    {
        const Theme *dark = theme_at(0), *light = theme_at(theme_find("Claro"));
        CHECK(light->light);
        float ld0 = (float)(dark->col[TK_N0] & 255u), ld15 = (float)(dark->col[TK_N15] & 255u);
        float ll0 = (float)(light->col[TK_N0] & 255u), ll15 = (float)(light->col[TK_N15] & 255u);
        CHECK(ld0 < ld15 && ll0 > ll15);
    }
    // selecting a theme changes the table at once (the next frame reads it) and asks ImGui's style again
    uint32_t g0 = theme_generation(), panel0 = TH(TK_BG_PANEL);
    CHECK(theme_select_name("Claro"));
    CHECK(theme_generation() != g0 && TH(TK_BG_PANEL) != panel0 && theme_style_dirty());

    // the patched ImGui's structs as C sees them (cimgui.h mirrors the [Real Engine] fields: the dock code reads windows)
    {
        extern size_t RE_SizeofImGuiWindow, RE_OffsetofWindowDockNode, RE_OffsetofWindowHidden, RE_SizeofImGuiContext, RE_SizeofImGuiDockNode;
        CHECK(RE_SizeofImGuiWindow == sizeof(ImGuiWindow));
        CHECK(RE_OffsetofWindowDockNode == offsetof(ImGuiWindow, DockNode));
        CHECK(RE_OffsetofWindowHidden == offsetof(ImGuiWindow, Hidden));
        CHECK(RE_SizeofImGuiContext == sizeof(ImGuiContext));
        CHECK(RE_SizeofImGuiDockNode == sizeof(ImGuiDockNode));
    }
    // ImGui's style from the tokens
    igCreateContext(NULL);
    ImGuiIO* io = igGetIO_Nil();
    io->IniFilename = NULL;
    theme_load_fonts(15.0f);
    CHECK(g_font_ui && g_font_mono && g_font_title && g_font_bold);
    theme_apply_imgui(1.0f);
    CHECK(!theme_style_dirty());
    ImVec4 wb = igGetStyle()->Colors[ImGuiCol_WindowBg];
    CHECK(fabsf(wb.x - (float)(TH(TK_BG_APP) & 255u) / 255.0f) < 0.01f);
    CHECK(igGetStyle()->ChildRounding == TMV(TM_RADIUS_PANEL) && igGetStyle()->DockingSeparatorSize == TMV(TM_SPACE_CARD));
    theme_select(0);

    // a .rtheme written and read back: the same theme
    {
        static char text[16384];
        Theme a, b;
        theme_builtin(&a, theme_find("Neon"));
        memset(a.set, 1, sizeof a.set); // (every token written)
        CHECK(theme_write_text(&a, text, sizeof text));
        CHECK(theme_read_text(&b, text));
        CHECK(!strcmp(a.name, b.name));
        int same = 0;
        for (int k = 0; k < TK_COUNT; k++) same += a.col[k] == b.col[k];
        CHECK(same == TK_COUNT);
        for (int m = 0; m < TM_COUNT; m++) CHECK(fabsf(a.metric[m] - b.metric[m]) < 1e-3f);
        CHECK(!theme_read_text(&b, "not a theme\nname = x\n"));
        // a user's theme gives a few tokens over its base: the rest comes from the base
        CHECK(theme_read_text(&b, "rtheme 1\nname = Meu\nbase = Brasa\naccent = #FF0000\nradius_panel = 2\n"));
        CHECK(b.col[TK_ACCENT] == 0xFF0000FFu && b.col[TK_BG_PANEL] == theme_at(theme_find("Brasa"))->col[TK_BG_PANEL]);
        CHECK(fabsf(b.metric[TM_RADIUS_PANEL] - 2.0f) < 1e-3f);
    }

    // editing: a copy, live; cancel restores; save makes a user theme; duplicate, export, import, delete
    {
        uint32_t before = TH(TK_ACCENT);
        Theme* e = theme_edit_begin(0);
        CHECK(e && theme_editing());
        e->col[TK_ACCENT] = 0xFF00FF00u, e->set[TK_ACCENT] = 1;
        theme_edit_changed();
        CHECK(TH(TK_ACCENT) == 0xFF00FF00u); // live: the next frame draws it
        theme_edit_cancel();
        CHECK(TH(TK_ACCENT) == before && !theme_editing());
        e = theme_edit_begin(0);
        e->col[TK_ACCENT] = 0xFF00FF00u, e->set[TK_ACCENT] = 1;
        theme_edit_changed();
        CHECK(theme_edit_save("Órbita Escuro") < 0); // (a built-in's name: never replaced)
        int mine = theme_edit_save("Teste Verde");
        CHECK(mine >= THEME_BUILTIN_COUNT && theme_current() == mine && TH(TK_ACCENT) == 0xFF00FF00u);
        int dup = theme_duplicate(mine, "Teste Verde 2");
        CHECK(dup > mine);
        char path[800];
        snprintf(path, sizeof path, "%s\\exported.rtheme", dir);
        CHECK(theme_export(mine, path));
        CHECK(theme_delete_user(dup) && theme_delete_user(mine));
        CHECK(theme_find("Teste Verde") < 0);
        int imp = theme_import(path);
        CHECK(imp >= THEME_BUILTIN_COUNT && !strcmp(theme_at(imp)->name, "Teste Verde"));
        CHECK(theme_at(imp)->col[TK_ACCENT] == 0xFF00FF00u);
        // read again from the folder (the next start)
        theme_init();
        CHECK(theme_find("Teste Verde") >= THEME_BUILTIN_COUNT);
        CHECK(theme_delete_user(theme_find("Teste Verde")));
        re_remove_file(path);
    }

    // the cost: a color read (the UI's every draw) and a theme change
    {
        volatile uint32_t sink = 0;
        double t0 = re_time_seconds();
        for (int i = 0; i < 10000000; i++) sink += TH((ThemeTok)(i % TK_COUNT));
        double read_ns = (re_time_seconds() - t0) * 1e9 / 1e7;
        t0 = re_time_seconds();
        for (int i = 0; i < 100; i++) theme_select(i % THEME_BUILTIN_COUNT);
        double sel_ms = (re_time_seconds() - t0) * 1000.0 / 100.0;
        t0 = re_time_seconds();
        for (int i = 0; i < 100; i++) theme_apply_imgui(1.0f);
        double imgui_ms = (re_time_seconds() - t0) * 1000.0 / 100.0;
        printf("[theme] a color read %.2f ns; a theme change %.4f ms + ImGui's style %.4f ms (once, between frames)\n", read_ns, sel_ms, imgui_ms);
        CHECK(read_ns < 5.0 && sel_ms < 1.0 && imgui_ms < 1.0);
        (void)sink;
    }
    igDestroyContext(NULL);
    printf(g_fail ? "theme: %d FAILED\n" : "theme: OK (%d built-in themes, %d tokens, contrast checked)\n", g_fail ? g_fail : THEME_BUILTIN_COUNT, TK_COUNT);
    return g_fail ? 1 : 0;
}
