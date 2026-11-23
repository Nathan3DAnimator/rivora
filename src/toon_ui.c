// (Q11) The Toon and Outline sections of the Material Editor's Details and of a Material Instance (toon_ui.h)
#include "toon_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "i18n.h"
#include "platform.h"
#include "ui_tips.h"

// The Auto Adaptive curve: the width over the distance (near .. far and a bit past), the near and far marks
static void curve_plot(const MatToon* t) {
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x, h = igGetFontSize() * 4.0f;
    if (w < 40.0f) return;
    igInvisibleButton("toon_curve", (ImVec2){ w, h }, 0); // tips: none (a drawing of the curve the rows above set)
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + w, o.y + h }, TH(TK_N3), 3.0f, 0);
    float fd = t->far_dist > t->near_dist ? t->far_dist : t->near_dist + 1.0f, span = fd * 1.2f;
    float smax = fmaxf(fmaxf(t->near_scale, t->far_scale), 0.1f) * 1.15f;
    ImVec2 pts[48];
    for (int i = 0; i < 48; i++) {
        float d = span * (float)i / 47.0f;
        float x = t->adaptive ? fminf(fmaxf((d - t->near_dist) / (fd - t->near_dist), 0.0f), 1.0f) : 0.0f;
        x = powf(x, fminf(fmaxf(t->curve_exp, 0.1f), 8.0f));
        float sc = t->adaptive ? t->near_scale + (t->far_scale - t->near_scale) * x : 1.0f;
        pts[i] = (ImVec2){ o.x + w * (float)i / 47.0f, o.y + h - 4.0f - (h - 8.0f) * sc / smax };
    }
    ImDrawList_AddPolyline(dl, pts, 48, TH(TK_ACCENT), 2.0f, 0);
    for (int k = 0; k < 2 && t->adaptive; k++) {
        float x = o.x + w * (k ? fd : t->near_dist) / span;
        ImDrawList_AddLine(dl, (ImVec2){ x, o.y + 2.0f }, (ImVec2){ x, o.y + h - 2.0f }, TH(TK_N8), 1.0f);
    }
    char lab[64];
    snprintf(lab, sizeof lab, "%.0f m", span);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + w - igCalcTextSize(lab, NULL, false, 0).x - 4.0f, o.y + 2.0f }, TH(TK_TEXT_DIM), lab, NULL);
}

bool toon_ui_draw(MatToon* t, bool toon_shading) {
    bool ch = false;
    if (toon_shading) {
        igSeparatorText(TR("Toon"));
        ch |= igSliderInt(PL(TR("Toon Bands")), &t->bands, 0, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Toon Softness")), &t->softness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Shadow Threshold")), &t->shadow_threshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igColorEdit3(PL(TR("Shadow Color")), t->shadow_color, 0);
        ch |= igColorEdit3(PL(TR("Received Shadow Color")), t->recv_color, 0);
        // the ramp: its points (position and color), or an image read from left to right
        ch |= igSliderInt(PL(TR("Ramp Points")), &t->nstops, 1, TOON_STOPS, "%d", ImGuiSliderFlags_AlwaysClamp);
        for (int i = 0; i < t->nstops && i < TOON_STOPS; i++) {
            igPushID_Int(i + 900);
            ch |= igSliderFloat(PL(TR("Point Position")), &t->stop_pos[i], 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ch |= igColorEdit3(PL(TR("Point Color")), t->stop_col[i], ImGuiColorEditFlags_Float);
            igPopID();
        }
        igSetNextItemWidth(-igGetFontSize() * 2.5f);
        ch |= igInputTextWithHint(PL(TR("Ramp Image")), TR("none: the points"), t->ramp_image, sizeof t->ramp_image, 0, NULL, NULL);
        igSameLine(0, 4);
        if (igButton("...##ramp", (ImVec2){ 0, 0 })) { // tips: none (the file dialog of Ramp Image, the row above)
            char path[1024] = "";
            if (platform_file_dialog_ext(NULL, false, TR("Images"), "png;jpg;jpeg;tga;bmp", path, sizeof path))
                snprintf(t->ramp_image, sizeof t->ramp_image, "%s", path), ch = true;
        }
        igSeparatorText(TR("Toon Highlight and Rim"));
        ch |= igColorEdit3(PL(TR("Highlight Color")), t->spec_color, 0);
        ch |= igSliderFloat(PL(TR("Highlight Strength")), &t->spec_strength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Highlight Size")), &t->spec_size, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Highlight Hardness")), &t->spec_hardness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igColorEdit3(PL(TR("Rim Light Color")), t->rim_color, 0);
        ch |= igSliderFloat(PL(TR("Rim Light Strength")), &t->rim_strength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Rim Light Width")), &t->rim_width, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Rim Light Softness")), &t->rim_softness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        igSeparatorText(TR("Toon Hatching"));
        ch |= igSliderFloat(PL(TR("Hatching")), &t->hatch_strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (t->hatch_strength > 0.0f) {
            ch |= igColorEdit3(PL(TR("Hatch Color")), t->hatch_color, 0);
            ch |= igSliderFloat(PL(TR("Hatch Spacing")), &t->hatch_scale, 2.0f, 64.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
            ch |= igSliderFloat(PL(TR("Hatch Angle")), &t->hatch_angle, -90.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
            ch |= igSliderFloat(PL(TR("Hatch Threshold")), &t->hatch_threshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        ch |= igSliderFloat(PL(TR("Toon Ambient")), &t->ambient, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }
    igSeparatorText(TR("Outline"));
    ch |= igCheckbox(PL(TR("Draw Outline")), &t->outline);
    if (t->outline) {
        ch |= igColorEdit3(PL(TR("Line Color")), t->outline_color, 0);
        ch |= igSliderFloat(PL(TR("Line Width")), &t->outline_width, 0.0f, 12.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp);
        ch |= igCheckbox(PL(TR("Auto Adaptive")), &t->adaptive);
        if (t->adaptive) {
            ch |= igDragFloat(PL(TR("Near Distance")), &t->near_dist, 0.1f, 0.0f, 1000.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            ch |= igDragFloat(PL(TR("Far Distance")), &t->far_dist, 0.5f, 0.1f, 5000.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            ch |= igSliderFloat(PL(TR("Near Width Scale")), &t->near_scale, 0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            ch |= igSliderFloat(PL(TR("Far Width Scale")), &t->far_scale, 0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            ch |= igSliderFloat(PL(TR("Width Curve")), &t->curve_exp, 0.1f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            curve_plot(t);
        }
        ch |= igSliderFloat(PL(TR("Depth Sensitivity")), &t->depth_sens, 0.002f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ch |= igSliderFloat(PL(TR("Crease Angle")), &t->crease_deg, 0.0f, 120.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
    }
    return ch;
}
