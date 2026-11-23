// The Camera Shake editor (camshake.h; notas-dev/camera-primeira-pessoa.md): a small window like Unreal's Camera Shake
// asset: a preset to start from, its timing (duration, blend in / out, scale), each channel's oscillation (amplitude,
// frequency, waveform, a curve's keys) and a plot of the seven channels over its life (envelope included); Play shakes
// the player's view while the editor plays. Save: Ctrl+S.
#include "camshake.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "fpcam.h"
#include "i18n.h"
#include "theme.h"
#include "ui_icons.h"
#include "ui_tips.h"

static struct {
    bool open, unsaved;
    char vpath[600], status[128];
    CamShake s;
} E;

void camshake_editor_open(const char* vpath) {
    snprintf(E.vpath, sizeof E.vpath, "%s", vpath);
    camshake_load(vpath, &E.s);
    camshake_remember(vpath);
    E.open = true, E.unsaved = false, E.status[0] = 0;
}

// The seven channels over the shake's life (a few seconds when it lasts until stopped), each its own color
static void plot(const CamShake* s) {
    ImVec2 p0 = igGetCursorScreenPos(), avail = igGetContentRegionAvail();
    float w = avail.x > 50.0f ? avail.x : 50.0f, h = igGetFontSize() * 7.0f;
    igInvisibleButton("##shake_plot", (ImVec2){ w, h }, 0); // tips: none (a plot drawn under its own title)
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p0.x + w, p0.y + h }, TH(TK_GRAPH_BG), 4.0f, 0);
    ImDrawList_AddLine(dl, (ImVec2){ p0.x, p0.y + h * 0.5f }, (ImVec2){ p0.x + w, p0.y + h * 0.5f }, TH(TK_GRAPH_GRID_MAJOR), 1.0f);
    float life = s->duration > 0.0f ? s->duration : 3.0f, peak = 1e-3f;
    enum { N = 160 };
    static float v[CS_CHANNELS][N];
    for (int i = 0; i < N; i++) {
        float t = life * (float)i / (float)(N - 1), out[CS_CHANNELS], e = camshake_envelope(s, t, -1.0f) * (s->scale > 0.0f ? s->scale : 1.0f);
        camshake_sample(s, t, 12345u, out);
        for (int c = 0; c < CS_CHANNELS; c++) v[c][i] = out[c] * e, peak = fmaxf(peak, fabsf(v[c][i]));
    }
    static const int col[CS_CHANNELS] = { TK_RED, TK_GREEN, TK_BLUE, TK_ORANGE, TK_CYAN, TK_MAGENTA, TK_AMBER };
    for (int c = 0; c < CS_CHANNELS; c++) {
        if (s->ch[c].amplitude == 0.0f) continue;
        for (int i = 1; i < N; i++) {
            float x0 = p0.x + w * (float)(i - 1) / (N - 1), x1 = p0.x + w * (float)i / (N - 1);
            float y0 = p0.y + h * 0.5f - v[c][i - 1] / peak * h * 0.45f, y1 = p0.y + h * 0.5f - v[c][i] / peak * h * 0.45f;
            ImDrawList_AddLine(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, TH(col[c]), 1.5f);
        }
    }
    char lab[64];
    snprintf(lab, sizeof lab, "%.2f s", life);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p0.x + w - igCalcTextSize(lab, NULL, false, -1).x - 4, p0.y + 2 }, TH(TK_TEXT_DIM), lab, NULL);
}

static bool channel(int c) {
    CamShakeChannel* k = &E.s.ch[c];
    bool ch = false;
    igPushID_Int(c);
    if (igTreeNodeEx_Str(TR(g_camshake_channel_names[c]), k->amplitude != 0.0f ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ch |= igDragFloat(TR("Amplitude"), &k->amplitude, 0.02f, -100.0f, 100.0f, c >= CS_PITCH ? "%.2f\xc2\xb0" : "%.2f cm", 0);
        const char* names[CSW_COUNT];
        for (int i = 0; i < CSW_COUNT; i++) names[i] = TR(g_camshake_wave_names[i]);
        int wv = (int)k->wave;
        if (igCombo_Str_arr(TR("Waveform"), &wv, names, CSW_COUNT, -1)) k->wave = (uint32_t)wv, ch = true;
        if (k->wave != CSW_CURVE) ch |= igDragFloat(TR("Frequency"), &k->frequency, 0.05f, 0.0f, 120.0f, "%.2f Hz", 0);
        if (k->wave == CSW_SINE) {
            bool ro = k->random_offset != 0;
            if (igCheckbox(TR("Random Start"), &ro)) k->random_offset = ro, ch = true;
        }
        if (k->wave == CSW_CURVE) { // the keys: time (s) and value (-1..1, times the amplitude); the curve loops at its last key
            for (uint32_t i = 0; i < k->nkeys && i < CS_MAX_KEYS; i++) {
                igPushID_Int((int)i);
                ch |= igDragFloat2("##key", k->keys[i], 0.005f, -10.0f, 10.0f, "%.3f", 0); // tips: none (a key's time and value: the Keys row's)
                igSameLine(0, 4);
                if (igSmallButton("x##delkey")) {
                    memmove(&k->keys[i], &k->keys[i + 1], sizeof k->keys[0] * (k->nkeys - i - 1));
                    k->nkeys--, ch = true;
                }
                igPopID();
            }
            if (k->nkeys < CS_MAX_KEYS && igSmallButton(TR("Add Curve Key"))) {
                float t = k->nkeys ? k->keys[k->nkeys - 1][0] + 0.1f : 0.0f;
                k->keys[k->nkeys][0] = t, k->keys[k->nkeys][1] = 0.0f, k->nkeys++, ch = true;
            }
        }
        igTreePop();
    }
    igPopID();
    return ch;
}

void camshake_editor_draw(void) {
    if (!E.open) return;
    const char* name = strrchr(E.vpath, '/');
    char title[700];
    snprintf(title, sizeof title, "%s%s - %s###camshake_editor", name ? name + 1 : E.vpath, E.unsaved ? "*" : "", TR("Camera Shake"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 32.0f, igGetFontSize() * 34.0f }, ImGuiCond_FirstUseEver);
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        if (igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
            (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false))) {
            bool ok = camshake_save(E.vpath, &E.s);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        ui_item_tip(TR("Writes the Camera Shake (Ctrl+S): the next Start Camera Shake plays the new values."));
        igSameLine(0, 10);
        if (igBeginCombo("##shake_preset", TR("Preset"), 0)) {
            const CamShakePreset* ps = camshake_presets();
            for (int i = 0; i < CAMSHAKE_PRESETS; i++)
                if (igSelectable_Bool(TR(ps[i].name), false, 0, (ImVec2){ 0, 0 })) E.s = ps[i].s, ch = true;
            igEndCombo();
        }
        ui_item_tip(TR("Starts from a built-in shake: Explosion, Earthquake, Handheld, Hit, Gunshot, Footstep, Heartbeat, Dread."));
        igSameLine(0, 10);
        if (igButton(TR("Play Shake"), (ImVec2){ 0, 0 })) fpcam_start_shake(E.vpath, 1.0f, 0);
        ui_item_tip(TR("Shakes the player's view with these values while the editor plays (Save first: Play reads the file)."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        igSeparator();
        plot(&E.s);
        igPushItemWidth(-igGetFontSize() * 11.0f);
        ch |= igDragFloat(TR("Duration"), &E.s.duration, 0.01f, 0.0f, 60.0f, "%.2f s", 0);
        ch |= igDragFloat(TR("Blend In Time"), &E.s.blend_in, 0.01f, 0.0f, 10.0f, "%.2f s", 0);
        ch |= igDragFloat(TR("Blend Out Time"), &E.s.blend_out, 0.01f, 0.0f, 10.0f, "%.2f s", 0);
        ch |= igDragFloat(TR("Shake Scale"), &E.s.scale, 0.01f, 0.0f, 10.0f, "%.2f", 0);
        bool si = E.s.single_instance != 0;
        if (igCheckbox(TR("Single Instance"), &si)) E.s.single_instance = si, ch = true;
        igSeparatorText(TR("Channels"));
        for (int c = 0; c < CS_CHANNELS; c++) ch |= channel(c);
        igPopItemWidth();
        igSeparator();
        igTextWrapped("%s", TR("Play it with Start Camera Shake (its name) or Play World Camera Shake (from a place)."));
    }
    igEnd();
    if (ch) E.unsaved = true;
    if (!open) E.open = false;
}

// Content Browser > Add > Camera > Camera Shake: one per preset, named after it, opened in its editor
bool camshake_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_CAMERA, TR("Camera"), ok)) return false;
    const CamShakePreset* ps = camshake_presets();
    for (int i = 0; i < CAMSHAKE_PRESETS; i++) {
        char label[96], name[64], vp[600];
        snprintf(label, sizeof label, "%s: %s", TR("Camera Shake"), TR(ps[i].name));
        if (!ui_menu_item(ICON_CAMERA, label, NULL, false, true)) continue;
        snprintf(name, sizeof name, "CS_%s", ps[i].name);
        if (camshake_create(folder_vpath, name, &ps[i].s, vp, sizeof vp)) {
            camshake_editor_open(vp);
            made = true;
        }
    }
    igEndMenu();
    return made;
}
