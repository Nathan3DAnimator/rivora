// The Timeline editor (bp_timeline.h bp_timeline_editor): Unreal's, a window of its own opened from a Timeline node
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "bp_timeline.h"
#include "theme.h"
#include "i18n.h"
#include "ui_icons.h"

#ifndef IM_COL32
#define IM_COL32(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#endif

// --- The Timeline editor ---
static const uint32_t g_comp_col[4] = { TK_RED, TK_GREEN, TK_BLUE, TK_N14 };
static struct { int track, key; bool dragging, shown; int bring; float view_t0, view_t1, view_v0, view_v1; char last[BP_NAME * 2]; } TE = { .key = -1, .view_t1 = 1, .view_v1 = 1 };

// the smallest 1, 2 or 5 times a power of ten not under x (a grid's step)
static float nice_step(float x) {
    if (!(x > 0.0f)) return 1.0f;
    float p = powf(10.0f, floorf(log10f(x))), m = x / p;
    return (m <= 1.0f ? 1.0f : m <= 2.0f ? 2.0f : m <= 5.0f ? 5.0f : 10.0f) * p;
}

static void fit_view(const BpTimeline* tl) {
    float len = fmaxf(bp_timeline_length(tl), 0.1f);
    TE.view_t0 = -0.05f * len, TE.view_t1 = len * 1.05f;
    float lo = 1e30f, hi = -1e30f;
    for (uint8_t i = 0; i < tl->ntracks; i++) {
        const BpTlTrack* trk = &tl->tracks[i];
        int nc = trk->type == TLT_FLOAT ? 1 : trk->type == TLT_VECTOR ? 3 : trk->type == TLT_COLOR ? 4 : 0;
        for (uint8_t k = 0; k < trk->nkeys; k++)
            for (int c = 0; c < nc; c++) lo = fminf(lo, trk->keys[k].v[c]), hi = fmaxf(hi, trk->keys[k].v[c]);
    }
    if (lo > hi) lo = 0.0f, hi = 1.0f;
    float pad = fmaxf((hi - lo) * 0.15f, 0.1f);
    TE.view_v0 = lo - pad, TE.view_v1 = hi + pad;
}

bool bp_timeline_editor(BpAsset* a, char* name, bool* open) {
    if (!*open) {
        TE.shown = false;
        return false;
    }
    BpTimeline* tl = bp_timeline_find(a, name);
    if (!tl) {
        *open = false;
        return false;
    }
    bool changed = false;
    char key[BP_NAME * 2 + 8];
    snprintf(key, sizeof key, "%s/%s", a->name, name);
    if (strcmp(TE.last, key)) snprintf(TE.last, sizeof TE.last, "%s", key), TE.track = 0, TE.key = -1, TE.bring = 6, fit_view(tl);
    if (!TE.shown) TE.bring = 6, TE.shown = true; // (opened again: in front)
    char title[160];
    snprintf(title, sizeof title, "%s: %s###timeline_editor", TR("Timeline"), name);
    // (in front of the Blueprint's window, under the middle of the screen the first time; brought forward when opened)
    const ImGuiViewport* mv = igGetMainViewport();
    ImVec2 wsz = { fminf(igGetFontSize() * 64, mv->Size.x * 0.7f), fminf(igGetFontSize() * 26, mv->Size.y * 0.45f) };
    igSetNextWindowSize(wsz, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + (mv->Size.x - wsz.x) * 0.5f, mv->Pos.y + mv->Size.y - wsz.y - igGetFontSize() * 3 }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    if (TE.bring > 0) igSetNextWindowFocus(), TE.bring--; // (a few frames: the Blueprint window takes the focus in its first ones)
    if (!igBegin(title, open, 0)) {
        igEnd();
        return false;
    }
    // the timeline's settings (Unreal's toolbar: Length, Use Last Keyframe, AutoPlay, Loop)
    char nm[BP_NAME];
    snprintf(nm, sizeof nm, "%s", tl->name);
    igSetNextItemWidth(igGetFontSize() * 10);
    if (igInputText("##tlname", nm, sizeof nm, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL) && nm[0] && !bp_timeline_find(a, nm)) {
        snprintf(tl->name, sizeof tl->name, "%s", nm), snprintf(name, BP_NAME, "%s", nm), changed = true;
        snprintf(TE.last, sizeof TE.last, "%s/%s", a->name, nm);
    }
    igSameLine(0, 8);
    igSetNextItemWidth(igGetFontSize() * 5);
    changed |= igDragFloat(TR("Length"), &tl->length, 0.01f, 0.01f, 3600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
    changed |= igCheckbox(TR("Use Last Keyframe"), &tl->use_last_key);
    igSameLine(0, 8);
    changed |= igCheckbox(TR("AutoPlay"), &tl->autoplay);
    igSameLine(0, 8);
    changed |= igCheckbox(TR("Loop"), &tl->loop);
    igSameLine(0, 8);
    igSetNextItemWidth(igGetFontSize() * 4);
    changed |= igDragFloat(TR("Play Rate"), &tl->rate, 0.01f, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    // the tracks: add, pick, rename, remove
    if (igButton(TR("+ Float Track"), (ImVec2){ 0, 0 }) && bp_timeline_add_track(tl, TLT_FLOAT)) changed = true, TE.track = tl->ntracks - 1;
    igSameLine(0, 4);
    if (igButton(TR("+ Vector Track"), (ImVec2){ 0, 0 }) && bp_timeline_add_track(tl, TLT_VECTOR)) changed = true, TE.track = tl->ntracks - 1;
    igSameLine(0, 4);
    if (igButton(TR("+ Color Track"), (ImVec2){ 0, 0 }) && bp_timeline_add_track(tl, TLT_COLOR)) changed = true, TE.track = tl->ntracks - 1;
    igSameLine(0, 4);
    if (igButton(TR("+ Event Track"), (ImVec2){ 0, 0 }) && bp_timeline_add_track(tl, TLT_EVENT)) changed = true, TE.track = tl->ntracks - 1;
    igSameLine(0, 4);
    if (igButton(TR("Zoom to Fit"), (ImVec2){ 0, 0 })) fit_view(tl);
    if (TE.track >= tl->ntracks) TE.track = tl->ntracks - 1, TE.key = -1;
    float left = igGetFontSize() * 13;
    igBeginChild_Str("##tltracks", (ImVec2){ left, 0 }, ImGuiChildFlags_Borders, 0);
    for (int i = 0; i < tl->ntracks; i++) {
        BpTlTrack* trk = &tl->tracks[i];
        igPushID_Int(i);
        char lab[96];
        snprintf(lab, sizeof lab, "%s  (%s)", trk->name, TR(g_tl_track_type_names[trk->type]));
        if (igSelectable_Bool(lab, TE.track == i, 0, (ImVec2){ 0, 0 })) TE.track = i, TE.key = -1;
        if (igBeginPopupContextItem("tltrack", ImGuiPopupFlags_MouseButtonRight)) {
            char tn[BP_NAME];
            snprintf(tn, sizeof tn, "%s", trk->name);
            igSetNextItemWidth(igGetFontSize() * 10);
            if (igInputText(TR("Name"), tn, sizeof tn, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL) && tn[0])
                snprintf(trk->name, sizeof trk->name, "%s", tn), changed = true;
            if (ui_menu_item(ICON_DELETE, TR("Delete Track"), NULL, false, true)) {
                memmove(&tl->tracks[i], &tl->tracks[i + 1], sizeof tl->tracks[0] * (size_t)(tl->ntracks - i - 1));
                tl->ntracks--, changed = true, TE.key = -1;
            }
            igEndPopup();
        }
        igPopID();
    }
    igTextDisabled("%s", TR("Right-click a track: rename, delete."));
    igEndChild();
    igSameLine(0, 6);
    // the curve: the selected track's keys over the time
    igBeginChild_Str("##tlcurve", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    BpTlTrack* trk = TE.track >= 0 && TE.track < tl->ntracks ? &tl->tracks[TE.track] : NULL;
    ImGuiIO* io = igGetIO_Nil();
    ImVec2 o = igGetCursorScreenPos(), sz = igGetContentRegionAvail();
    float bottom = igGetFontSize() * 2.4f; // (the selected key's fields under the curve)
    sz.y = fmaxf(sz.y - bottom, 60.0f);
    igInvisibleButton("##tlcanvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas: no description over the work)
    bool hov = igIsItemHovered(0);
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + sz.x, o.y + sz.y }, TH(TK_N4), 0, 0);
    float tspan = fmaxf(TE.view_t1 - TE.view_t0, 1e-3f), vspan = fmaxf(TE.view_v1 - TE.view_v0, 1e-4f);
#define TX(t) (o.x + ((t) - TE.view_t0) / tspan * sz.x)
#define VY(v) (o.y + sz.y - ((v) - TE.view_v0) / vspan * sz.y)
    // the grid: seconds and the value's lines, the length's end
    float step = nice_step(tspan * igGetFontSize() * 5.0f / fmaxf(sz.x, 1.0f)); // (a label every ~5 letters' width)
    for (float t = ceilf(TE.view_t0 / step) * step; t <= TE.view_t1; t += step) {
        ImDrawList_AddLine(dl, (ImVec2){ TX(t), o.y }, (ImVec2){ TX(t), o.y + sz.y }, TH(TK_N9), 1.0f);
        char s[24];
        snprintf(s, sizeof s, "%.2f", fabsf(t) < 1e-6f ? 0.0f : t);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ TX(t) + 2, o.y + 2 }, TH(TK_N12), s, NULL);
    }
    float vstep = nice_step(vspan * igGetFontSize() * 2.2f / fmaxf(sz.y, 1.0f)); // (a line every ~2 rows)
    for (float v = ceilf(TE.view_v0 / vstep) * vstep; v <= TE.view_v1; v += vstep) {
        ImDrawList_AddLine(dl, (ImVec2){ o.x, VY(v) }, (ImVec2){ o.x + sz.x, VY(v) }, TH(TK_N7), 1.0f);
        char s[24];
        snprintf(s, sizeof s, "%.2f", fabsf(v) < 1e-6f ? 0.0f : v);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 2, VY(v) - igGetFontSize() }, TH(TK_N11), s, NULL);
    }
    float len = bp_timeline_length(tl);
    ImDrawList_AddRectFilled(dl, (ImVec2){ TX(len), o.y }, (ImVec2){ o.x + sz.x, o.y + sz.y }, THA(TK_N0, 70), 0, 0);
    ImDrawList_AddLine(dl, (ImVec2){ TX(len), o.y }, (ImVec2){ TX(len), o.y + sz.y }, TH(TK_AMBER), 1.5f);
    if (trk) {
        int nc = trk->type == TLT_FLOAT ? 1 : trk->type == TLT_VECTOR ? 3 : trk->type == TLT_COLOR ? 4 : 0;
        // the curves, sampled along the view (one polyline per component)
        ImVec2 pts[256];
        for (int c = 0; c < nc; c++) {
            int np = 0;
            for (int s = 0; s < 256; s++) {
                float t = TE.view_t0 + tspan * (float)s / 255.0f, v[4];
                bp_timeline_eval(trk, t, v);
                pts[np++] = (ImVec2){ TX(t), VY(v[c]) };
            }
            ImDrawList_AddPolyline(dl, pts, np, nc == 1 ? TH(TK_AMBER) : TH(g_comp_col[c]), 2.0f, 0);
        }
        // the keys: picked by a click, dragged; Shift+click adds one; an Event's keys on the middle line
        int hot = -1;
        for (int k = 0; k < trk->nkeys; k++)
            for (int c = 0; c < (nc ? nc : 1); c++) {
                float y = nc ? VY(trk->keys[k].v[c]) : o.y + sz.y * 0.5f;
                float dx = io->MousePos.x - TX(trk->keys[k].t), dy = io->MousePos.y - y;
                bool on = dx * dx + dy * dy < 49.0f;
                if (on) hot = k;
                ImDrawList_AddCircleFilled(dl, (ImVec2){ TX(trk->keys[k].t), y }, k == TE.key ? 6.0f : on ? 5.5f : 4.5f,
                                           k == TE.key ? TH(TK_N15) : TH(TK_AMBER), 12);
            }
        if (hov && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            if (io->KeyShift && hot < 0) { // a key here (its value on the curve: the shape kept)
                float t = TE.view_t0 + (io->MousePos.x - o.x) / sz.x * tspan, v[4];
                bp_timeline_eval(trk, t, v);
                if (nc == 1) v[0] = TE.view_v0 + (o.y + sz.y - io->MousePos.y) / sz.y * vspan;
                TE.key = bp_timeline_add_key(trk, fmaxf(t, 0.0f), v), changed = true;
            } else TE.key = hot;
            TE.dragging = TE.key >= 0;
        }
        if (TE.dragging && igIsMouseDragging(ImGuiMouseButton_Left, 1.0f) && TE.key >= 0 && TE.key < trk->nkeys) {
            BpTlKey* k = &trk->keys[TE.key];
            k->t = fmaxf(k->t + io->MouseDelta.x / sz.x * tspan, 0.0f);
            if (nc == 1) k->v[0] -= io->MouseDelta.y / sz.y * vspan;
            float t = k->t;
            bp_timeline_sort(trk); // (passing another key: still the same one picked)
            for (int i = 0; i < trk->nkeys; i++)
                if (trk->keys[i].t == t) TE.key = i;
            changed = true;
        }
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) TE.dragging = false;
        if (hov && io->MouseWheel != 0.0f) { // the wheel: zoom the time around the mouse (Ctrl: the values)
            float f = io->MouseWheel > 0 ? 0.85f : 1.0f / 0.85f;
            if (io->KeyCtrl) {
                float mv = TE.view_v0 + (o.y + sz.y - io->MousePos.y) / sz.y * vspan;
                TE.view_v0 = mv + (TE.view_v0 - mv) * f, TE.view_v1 = mv + (TE.view_v1 - mv) * f;
            } else {
                float mt = TE.view_t0 + (io->MousePos.x - o.x) / sz.x * tspan;
                TE.view_t0 = mt + (TE.view_t0 - mt) * f, TE.view_t1 = mt + (TE.view_t1 - mt) * f;
            }
        }
        if (hov && igIsMouseDragging(ImGuiMouseButton_Right, 2.0f)) { // pan
            TE.view_t0 -= io->MouseDelta.x / sz.x * tspan, TE.view_t1 -= io->MouseDelta.x / sz.x * tspan;
            TE.view_v0 += io->MouseDelta.y / sz.y * vspan, TE.view_v1 += io->MouseDelta.y / sz.y * vspan;
        }
        if (TE.key >= 0 && TE.key < trk->nkeys && igIsWindowFocused(0) && igIsKeyPressed_Bool(ImGuiKey_Delete, false)) {
            memmove(&trk->keys[TE.key], &trk->keys[TE.key + 1], sizeof trk->keys[0] * (size_t)(trk->nkeys - TE.key - 1));
            trk->nkeys--, TE.key = -1, changed = true;
        }
        if (hot >= 0 && hov && igIsMouseReleased_Nil(ImGuiMouseButton_Right) && !igIsMouseDragging(ImGuiMouseButton_Right, 2.0f))
            TE.key = hot, igOpenPopup_Str("tlkey", 0);
        if (igBeginPopup("tlkey", 0)) {
            for (int i = 0; i < TLI_COUNT && TE.key >= 0 && TE.key < trk->nkeys; i++)
                if (ui_menu_item(ICON_CURVE, TR(g_tl_interp_names[i]), NULL, trk->keys[TE.key].interp == i, true)) trk->keys[TE.key].interp = (uint8_t)i, changed = true;
            if (ui_menu_item(ICON_DELETE, TR("Delete Key"), NULL, false, TE.key >= 0)) {
                memmove(&trk->keys[TE.key], &trk->keys[TE.key + 1], sizeof trk->keys[0] * (size_t)(trk->nkeys - TE.key - 1));
                trk->nkeys--, TE.key = -1, changed = true;
            }
            igEndPopup();
        }
        // the picked key's numbers
        if (TE.key >= 0 && TE.key < trk->nkeys) {
            BpTlKey* k = &trk->keys[TE.key];
            igSetNextItemWidth(igGetFontSize() * 6);
            if (igDragFloat(TR("Time"), &k->t, 0.01f, 0.0f, 3600.0f, "%.3f", 0)) {
                float t = k->t;
                bp_timeline_sort(trk);
                for (int i = 0; i < trk->nkeys; i++)
                    if (trk->keys[i].t == t) TE.key = i;
                changed = true;
            }
            k = &trk->keys[TE.key];
            igSameLine(0, 8);
            if (nc == 1) igSetNextItemWidth(igGetFontSize() * 6), changed |= igDragFloat(TR("Value"), &k->v[0], 0.01f, 0, 0, "%.3f", 0);
            else if (nc == 3) igSetNextItemWidth(igGetFontSize() * 16), changed |= igDragFloat3(TR("Value"), k->v, 0.01f, 0, 0, "%.3f", 0);
            else if (nc == 4) igSetNextItemWidth(igGetFontSize() * 16), changed |= igColorEdit4(TR("Value"), k->v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        } else igTextDisabled("%s", TR("Shift+click: a key. Drag: move it. Right-click a key: its interpolation. Wheel: zoom (Ctrl: values). Right drag: pan."));
    } else igTextDisabled("%s", TR("Add a track."));
#undef TX
#undef VY
    igEndChild();
    igEnd();
    if (changed) a->dirty = true;
    return changed;
}

