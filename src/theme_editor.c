// Editor Preferences > Appearance (Q1, notas-dev/temas-e-janelas.md): the themes as cards (a click applies one, the
// next frame), and the Theme Editor: a copy of a theme edited live (each color as you drag it), its sizes and
// durations, the contrast checked as you go, duplicated, saved as the user's, exported and imported (.rtheme).
#include "theme_editor.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "platform.h"

static char g_new_name[THEME_NAME];
static char g_msg[256];
static bool g_msg_bad;
static int g_group_open = 0;

static void say(bool bad, const char* fmt, const char* arg) {
    snprintf(g_msg, sizeof g_msg, fmt, arg ? arg : "");
    g_msg_bad = bad;
}

static ImVec4 v4_of(uint32_t c) { return (ImVec4){ (float)(c & 255u) / 255.0f, (float)((c >> 8) & 255u) / 255.0f, (float)((c >> 16) & 255u) / 255.0f, (float)(c >> 24) / 255.0f }; }
static uint32_t u32_of(const float* v) {
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        float x = v[i] < 0.0f ? 0.0f : v[i] > 1.0f ? 1.0f : v[i];
        o |= (uint32_t)(x * 255.0f + 0.5f) << (8 * i);
    }
    return o;
}

// A theme as a card: its surfaces, accents and text; selected: the accent's outline
static bool theme_card(int i, float w) {
    const Theme* t = theme_at(i);
    ImDrawList* dl = igGetWindowDrawList();
    float fs = igGetFontSize(), h = fs * 3.6f, r = TMV(TM_RADIUS_PANEL);
    ImVec2 p = igGetCursorScreenPos();
    igPushID_Int(i);
    bool clicked = igInvisibleButton("##card", (ImVec2){ w, h }, 0); // tips: none (a canvas or a tile drawn with its own name: no description over the work)
    bool hov = igIsItemHovered(0), cur = theme_current() == i && !theme_editing();
    igPopID();
    const uint32_t* c = t->col;
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, c[TK_BG_APP], r, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x + 6, p.y + 6 }, (ImVec2){ p.x + w * 0.62f, p.y + h - 6 }, c[TK_BG_PANEL], r * 0.6f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 12, p.y + 9 }, c[TK_TEXT], t->name, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 12, p.y + 9 + fs * 1.2f }, c[TK_TEXT_DIM], t->builtin ? TR("Built-in") : TR("Yours"), NULL);
    float sx = p.x + w * 0.62f + 8, sw = (p.x + w - 8 - sx) / 4.0f;
    const int sw_tok[4] = { TK_ACCENT, TK_ACCENT2, TK_OK, TK_ERROR };
    for (int k = 0; k < 4; k++)
        ImDrawList_AddRectFilled(dl, (ImVec2){ sx + sw * k + 1, p.y + 8 }, (ImVec2){ sx + sw * (k + 1) - 1, p.y + h - 8 }, c[sw_tok[k]], TMV(TM_RADIUS_FRAME), 0);
    if (cur || hov) ImDrawList_AddRect(dl, p, (ImVec2){ p.x + w, p.y + h }, cur ? TH(TK_ACCENT) : TH(TK_BORDER_STRONG), r, cur ? 2.0f : 1.0f, 0);
    return clicked;
}

// A small sample of the UI with the current tokens (the live preview beside the colors)
static void preview(void) {
    float fs = igGetFontSize();
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 10, 8 });
    igBeginChild_Str("theme_preview", (ImVec2){ 0, fs * 14.0f }, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    igTextUnformatted(TR("Preview"), NULL);
    static bool chk = true;
    static float sl = 0.4f;
    static char txt[32] = "Text";
    igButton(TR("Button"), (ImVec2){ 0, 0 });
    igSameLine(0, -1);
    igCheckbox(TR("Option"), &chk);
    igSetNextItemWidth(fs * 8);
    igSliderFloat("##pv_sl", &sl, 0, 1, "%.2f", 0);
    igSameLine(0, -1);
    igSetNextItemWidth(fs * 6);
    igInputText("##pv_tx", txt, sizeof txt, 0, NULL, NULL);
    igSelectable_Bool(TR("Selected row"), true, 0, (ImVec2){ 0, 0 });
    igTextColored(THV(TK_OK), "%s", TR("Success"));
    igSameLine(0, -1);
    igTextColored(THV(TK_WARN), "%s", TR("Warning"));
    igSameLine(0, -1);
    igTextColored(THV(TK_ERROR), "%s", TR("Error"));
    igSameLine(0, -1);
    igTextDisabled("%s", TR("Disabled"));
    // a node: header, title, pins by family
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float w = fs * 12, hh = fs * 1.5f, r = TMV(TM_RADIUS_NODE);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + fs * 4.2f }, TH(TK_BG_RAISED), r, 0);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + hh }, TH(TK_NODE_FUNCTION), r, ImDrawFlags_RoundCornersTop);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 8, p.y + (hh - fs) * 0.5f }, TH(TK_NODE_TITLE), "Print String", NULL);
    const int pins[4] = { TK_PIN_EXEC, TK_PIN_STRING, TK_PIN_FLOAT, TK_PIN_BOOL };
    for (int k = 0; k < 4; k++)
        ImDrawList_AddCircleFilled(dl, (ImVec2){ p.x + 12 + k * fs * 1.4f, p.y + hh + fs * 1.2f }, fs * 0.3f, TH(pins[k]), 12);
    ImVec2 q = { p.x + w + fs, p.y };
    ImDrawList_AddRectFilled(dl, q, (ImVec2){ q.x + w * 0.8f, q.y + fs * 4.2f }, TH(TK_BG_RAISED), r, 0);
    ImDrawList_AddRectFilled(dl, q, (ImVec2){ q.x + w * 0.8f, q.y + hh }, TH(TK_NODE_EVENT), r, ImDrawFlags_RoundCornersTop);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ q.x + 8, q.y + (hh - fs) * 0.5f }, TH(TK_NODE_TITLE), "Event BeginPlay", NULL);
    igDummy((ImVec2){ w * 2, fs * 4.4f });
    igEndChild();
    igPopStyleVar(1);
}

static bool edit_metrics(Theme* e) {
    bool ch = false;
    static const float lo[TM_COUNT] = { 0, 0, 0, 0, 0, 0, 1, 2, 2, 0, 0, 0, 0, 11, 12 };
    static const float hi[TM_COUNT] = { 16, 16, 12, 16, 12, 16, 12, 16, 12, 3, 400, 600, 800, 22, 30 };
    for (int m = 0; m < TM_COUNT; m++) {
        bool ms = m >= TM_DUR_FAST && m <= TM_DUR_ENTER;
        ch |= igSliderFloat(TR(theme_metric_label(m)), &e->metric[m], lo[m], hi[m], ms ? "%.0f ms" : "%.1f", 0);
    }
    return ch;
}

static bool edit_colors(Theme* e) {
    bool ch = false;
    float fs = igGetFontSize();
    for (int g = 0; g < theme_group_count(); g++) {
        igSetNextItemOpen(g == g_group_open, ImGuiCond_Once);
        if (!igCollapsingHeader_TreeNodeFlags(TR(theme_group_name(g)), 0)) continue;
        if (g == 4 || g == 5) igTextDisabled("%s", TR("Derived from the surfaces and texts unless you set one (the old fixed colors follow these)."));
        if (igBeginTable("##toks", 2, ImGuiTableFlags_SizingStretchProp, (ImVec2){ 0, 0 }, 0)) {
            for (int k = 0; k < TK_COUNT; k++) {
                if (theme_tok_group(k) != g) continue;
                igPushID_Int(k);
                igTableNextColumn();
                float v[4];
                ImVec4 c = v4_of(e->col[k]);
                v[0] = c.x, v[1] = c.y, v[2] = c.z, v[3] = c.w;
                if (igColorEdit4("##c", v, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_AlphaBar)) {
                    e->col[k] = u32_of(v), e->set[k] = 1, ch = true;
                }
                igSameLine(0, -1);
                if (e->set[k]) igTextUnformatted(TR(theme_tok_label(k)), NULL);
                else igTextDisabled("%s (%s)", TR(theme_tok_label(k)), TR("derived"));
                igTableNextColumn();
                char hex[10];
                theme_to_hex(e->col[k], hex);
                igTextDisabled("%s", hex);
                if (e->set[k]) {
                    igSameLine(0, -1);
                    if (igSmallButton(TR("Derive"))) e->set[k] = 0, ch = true;
                    igSetItemTooltip("%s", TR("Goes back to the value derived from the others"));
                }
                igPopID();
            }
            igEndTable();
        }
        (void)fs;
    }
    return ch;
}

bool theme_editor_page(struct GLFWwindow* win) {
    bool ch = false;
    float fs = igGetFontSize();
    igSeparatorText(TR("Theme"));
    float avail = igGetContentRegionAvail().x, cw = fmaxf(fs * 14.0f, (avail - fs * 1.2f) / 3.0f);
    int per_row = (int)fmaxf(1.0f, floorf((avail + 8) / (cw + 8)));
    for (int i = 0; i < theme_count(); i++) {
        if (i % per_row) igSameLine(0, 8);
        if (theme_card(i, cw)) {
            theme_select(i), ch = true;
            g_msg[0] = 0;
        }
    }
    igSpacing();
    int cur = theme_current();
    bool editing = theme_editing();
    if (!editing) {
        if (igButton(TR("Edit a copy"), (ImVec2){ 0, 0 })) {
            Theme* e = theme_edit_begin(cur);
            if (e) snprintf(g_new_name, sizeof g_new_name, "%.50s (%s)", theme_at(cur)->name, TR("mine"));
        }
        igSetItemTooltip("%s", TR("Edits a copy of this theme: every change shows at once; Save keeps it as one of yours"));
        igSameLine(0, -1);
        if (igButton(TR("Duplicate"), (ImVec2){ 0, 0 })) {
            char nm[THEME_NAME];
            for (int n = 2; n < 100; n++) {
                snprintf(nm, sizeof nm, "%.50s %d", theme_at(cur)->name, n);
                if (theme_find(nm) < 0) break;
            }
            int d = theme_duplicate(cur, nm);
            if (d >= 0) theme_select(d), ch = true, say(false, TR("Saved as \"%s\"."), nm);
            else say(true, TR("Could not save \"%s\"."), nm);
        }
        igSameLine(0, -1);
        if (igButton(TR("Import..."), (ImVec2){ 0, 0 })) {
            char path[1024] = "";
            if (platform_file_dialog_ext(win, false, TR("Rivora Engine Theme"), "rtheme", path, sizeof path)) {
                int i = theme_import(path);
                if (i >= 0) theme_select(i), ch = true, say(false, TR("Imported \"%s\"."), theme_at(i)->name);
                else say(true, TR("Could not read %s."), path);
            }
        }
        igSameLine(0, -1);
        if (igButton(TR("Export..."), (ImVec2){ 0, 0 })) {
            char path[1024];
            snprintf(path, sizeof path, "%s.rtheme", theme_at(cur)->name);
            if (platform_file_dialog_ext(win, true, TR("Rivora Engine Theme"), "rtheme", path, sizeof path)) {
                if (theme_export(cur, path)) say(false, TR("Exported to %s."), path);
                else say(true, TR("Could not write %s."), path);
            }
        }
        if (!theme_at(cur)->builtin) {
            igSameLine(0, -1);
            if (igButton(TR("Delete"), (ImVec2){ 0, 0 })) igOpenPopup_Str("del_theme", 0);
            if (igBeginPopup("del_theme", 0)) {
                igText(TR("Delete the theme \"%s\"? Its file goes too."), theme_at(cur)->name);
                if (igButton(TR("Delete"), (ImVec2){ 0, 0 })) theme_delete_user(cur), ch = true, igCloseCurrentPopup();
                igSameLine(0, -1);
                if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) igCloseCurrentPopup();
                igEndPopup();
            }
        }
    }
    if (g_msg[0]) igTextColored(g_msg_bad ? THV(TK_ERROR) : THV(TK_OK), "%s", g_msg);
    {
        char dir[600];
        theme_user_dir(dir, sizeof dir);
        igTextDisabled(TR("Your themes: %s"), dir);
    }
    if (!editing) {
        preview();
        return ch;
    }

    // --- the Theme Editor ---
    Theme* e = theme_edit_theme();
    igSeparatorText(TR("Theme Editor"));
    igInputText(TR("Name"), g_new_name, sizeof g_new_name, 0, NULL, NULL);
    bool exists_builtin = theme_find(g_new_name) >= 0 && theme_find(g_new_name) < THEME_BUILTIN_COUNT;
    igBeginDisabled(!g_new_name[0] || exists_builtin);
    if (igButton(TR("Save"), (ImVec2){ 0, 0 })) {
        int i = theme_edit_save(g_new_name);
        if (i >= 0) ch = true, say(false, TR("Saved as \"%s\"."), g_new_name);
        else say(true, TR("Could not save \"%s\"."), g_new_name);
    }
    igEndDisabled();
    if (exists_builtin) igSetItemTooltip("%s", TR("A built-in theme's name: choose another"));
    igSameLine(0, -1);
    if (igButton(TR("Cancel"), (ImVec2){ 0, 0 })) {
        theme_edit_cancel();
        return true;
    }
    igSameLine(0, -1);
    if (igButton(TR("Export..."), (ImVec2){ 0, 0 })) {
        char path[1024];
        snprintf(path, sizeof path, "%s.rtheme", g_new_name);
        if (platform_file_dialog_ext(win, true, TR("Rivora Engine Theme"), "rtheme", path, sizeof path)) {
            snprintf(e->name, sizeof e->name, "%s", g_new_name);
            if (theme_export(-1, path)) say(false, TR("Exported to %s."), path);
            else say(true, TR("Could not write %s."), path);
        }
    }
    if (e->base[0]) igTextDisabled(TR("Based on %s: what you don't set comes from it."), e->base);
    bool live = false;
    live |= igCheckbox(TR("Light theme"), &e->light);
    igSetItemTooltip("%s", TR("The ramp of grays and the hues' tones turn around: dark text on light surfaces"));
    live |= igSliderFloat(TR("Hues' saturation"), &e->hue_sat, 0.0f, 1.0f, "%.2f", 0);
    live |= igSliderFloat(TR("Hues' lightness"), &e->hue_light, -0.3f, 0.3f, "%+.2f", 0);
    bool okabe = e->palette == 1;
    if (igCheckbox(TR("Color-blind safe hues (Okabe-Ito)"), &okabe)) e->palette = okabe ? 1 : 0, live = true;

    // the contrast as you go (the same checks as the test and tools/theme_contrast.py)
    static char out[4096];
    int bad = theme_check_contrast(e, out, sizeof out);
    if (bad) {
        igTextColored(THV(TK_WARN), TR("Contrast: %d pair(s) below the minimum (text 4.5:1, icons 3:1)"), bad);
        if (igIsItemHovered(0)) igSetTooltip("%s", out);
    } else {
        igTextColored(THV(TK_OK), "%s", TR("Contrast: every text and icon readable (WCAG)"));
    }
    preview();
    igSeparatorText(TR("Colors"));
    live |= edit_colors(e);
    igSeparatorText(TR("Sizes and animations"));
    live |= edit_metrics(e);
    if (live) theme_edit_changed(); // (resolved and applied: the next frame draws with it)
    return ch || live;
}
