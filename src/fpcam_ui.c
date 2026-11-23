// (Q12) The First Person Camera's Details (fpcam.h): a section under the Camera's (a Camera actor's, a Blueprint's Camera
// component's): on or off, a preset, its parts and their values, the body seen. The editor only.
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "fpcam.h"
#include "i18n.h"
#include "ui_tips.h"

static bool part(const char* label, uint32_t* fp, uint32_t bit) {
    bool on = (*fp & bit) != 0;
    if (!igCheckbox(label, &on)) return false;
    *fp = on ? *fp | bit : *fp & ~bit;
    return true;
}

bool fpcam_ui(CCamera* c) {
    bool ch = false;
    char lab[96];
    snprintf(lab, sizeof lab, "%s###cam_fp", TR("First Person Camera"));
    if (!igCollapsingHeader_TreeNodeFlags(lab, 0)) return false;
    igPushID_Str("cam_fp");
    bool on = (c->fp & FP_ON) != 0;
    if (igCheckbox(PL(TR("Use First Person Camera")), &on)) {
        if (on && c->bob_speed <= 0.0f) fpcam_apply_preset(c, FPP_REALISTIC); // (never set: the realistic values)
        c->fp = on ? c->fp | FP_ON : c->fp & ~FP_ON, ch = true;
    }
    igBeginDisabled(!on);
    uint32_t p = c->fp_preset < FPP_COUNT ? c->fp_preset : FPP_CUSTOM;
    if (igBeginCombo(PL(TR("First Person Preset")), TR(g_fpcam_preset_names[p]), 0)) {
        for (uint32_t i = 1; i < FPP_COUNT; i++)
            if (igSelectable_Bool(TR(g_fpcam_preset_names[i]), i == p, 0, (ImVec2){ 0, 0 })) fpcam_apply_preset(c, i), ch = true;
        igEndCombo();
    }
    uint32_t before = c->fp;
    float vals_before[19];
    memcpy(vals_before, &c->bob_up_cm, sizeof vals_before);
    igSeparatorText(TR("Head Bob"));
    ch |= part(PL(TR("Head Bob")), &c->fp, FP_BOB);
    ch |= igDragFloat(PL(TR("Bob Up / Down")), &c->bob_up_cm, 0.05f, 0.0f, 10.0f, "%.2f cm", 0);
    ch |= igDragFloat(PL(TR("Bob Side")), &c->bob_side_cm, 0.05f, 0.0f, 10.0f, "%.2f cm", 0);
    ch |= igDragFloat(PL(TR("Bob Roll")), &c->bob_roll_deg, 0.02f, 0.0f, 5.0f, "%.2f\xc2\xb0", 0);
    ch |= igDragFloat(PL(TR("Bob Speed")), &c->bob_speed, 5.0f, 50.0f, 2000.0f, "%.0f cm/s", 0);
    ch |= igDragFloat(PL(TR("Stride")), &c->stride_cm, 1.0f, 20.0f, 300.0f, "%.0f cm", 0);
    igSeparatorText(TR("Look and Lean"));
    ch |= part(PL(TR("Look Lag")), &c->fp, FP_LOOK_LAG);
    ch |= igDragFloat(PL(TR("Look Lag Speed")), &c->look_lag_hz, 0.1f, 0.0f, 40.0f, "%.1f Hz", 0);
    ch |= igDragFloat(PL(TR("Look Damping")), &c->look_damping, 0.01f, 0.1f, 2.0f, "%.2f", 0);
    ch |= part(PL(TR("Camera Lean")), &c->fp, FP_LEAN);
    ch |= igDragFloat(PL(TR("Lean Into Turns")), &c->lean_turn_deg, 0.05f, 0.0f, 10.0f, "%.2f\xc2\xb0", 0);
    ch |= igDragFloat(PL(TR("Lean Into Strafe")), &c->lean_strafe_deg, 0.05f, 0.0f, 10.0f, "%.2f\xc2\xb0", 0);
    igSeparatorText(TR("Body"));
    ch |= part(PL(TR("Breathing")), &c->fp, FP_BREATH);
    ch |= igDragFloat(PL(TR("Breath Amount")), &c->breath_deg, 0.01f, 0.0f, 3.0f, "%.2f\xc2\xb0", 0);
    ch |= igDragFloat(PL(TR("Breaths per Minute")), &c->breath_rate, 0.2f, 4.0f, 40.0f, "%.1f", 0);
    ch |= part(PL(TR("Landing")), &c->fp, FP_LAND);
    ch |= igDragFloat(PL(TR("Landing Dip per Meter")), &c->land_cm_per_m, 0.05f, 0.0f, 20.0f, "%.2f cm", 0);
    ch |= igDragFloat(PL(TR("Max Landing Dip")), &c->land_max_cm, 0.1f, 0.0f, 60.0f, "%.1f cm", 0);
    ch |= igDragFloat(PL(TR("Recoil Return")), &c->recoil_hz, 0.05f, 0.2f, 20.0f, "%.2f Hz", 0);
    ch |= part(PL(TR("Show Own Body")), &c->fp, FP_BODY);
    char bone[sizeof c->hide_bone];
    snprintf(bone, sizeof bone, "%s", c->hide_bone[0] ? c->hide_bone : "head");
    if (igInputText(PL(TR("Hidden Bone")), bone, sizeof bone, 0, NULL, NULL)) memcpy(c->hide_bone, bone, sizeof bone), ch = true;
    igSeparatorText(TR("Focus and Vignette"));
    ch |= part(PL(TR("Autofocus")), &c->fp, FP_AUTOFOCUS);
    ch |= igDragFloat(PL(TR("Focus Speed")), &c->focus_speed, 0.05f, 0.2f, 30.0f, "%.2f /s", 0);
    ch |= igDragFloat(PL(TR("Min Focus Distance")), &c->focus_min_cm, 1.0f, 5.0f, 1000.0f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("Max Focus Distance")), &c->focus_max_cm, 10.0f, 100.0f, 100000.0f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("Focus f-stop")), &c->focus_fstop, 0.05f, 0.7f, 32.0f, "f/%.1f", 0);
    ch |= part(PL(TR("Effort Vignette")), &c->fp, FP_VIGNETTE);
    ch |= igDragFloat(PL(TR("Vignette at Full Effort")), &c->vignette_effort, 0.01f, 0.0f, 1.0f, "%.2f", 0);
    igEndDisabled();
    // a value changed by hand: no preset any more (the menu says Custom)
    if (ch && ((before & ~(FP_ON | FP_BODY)) != (c->fp & ~(FP_ON | FP_BODY)) || memcmp(vals_before, &c->bob_up_cm, sizeof vals_before)))
        c->fp_preset = FPP_CUSTOM;
    igPopID();
    return ch;
}
