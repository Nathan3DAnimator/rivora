// The async tasks' cards (async.h, notas-dev/async.md): at the bottom right, over everything, never taking the
// keyboard. Stacked (the newest at the bottom, the stack grows up), each with what it does, its bar, the time left and
// a Cancel. The header moves them (dragged: the offset from the corner kept in editor.ini) and makes them thin: one bar
// per task, the text on hover. A finished card turns green (red: failed) and fades.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "async.h"
#include "async_ui.h"
#include "editor.h"
#include "i18n.h"
#include "ui_icons.h"

#define MAX_VIEW 24
#define MAX_CARDS 5 // more running: "+N more" (all of them in the thin mode)

static struct {
    float ox, oy;   // the stack's bottom-right corner from the viewport's (px); < 0: the default
    bool compact;
    bool dragging;
    int shown;      // cards drawn last frame (the test)
} U = { -1.0f, -1.0f, false, false, 0 };

void async_ui_write_prefs(FILE* f) {
    fprintf(f, "async_x=%.0f\nasync_y=%.0f\nasync_compact=%d\n", U.ox, U.oy, U.compact ? 1 : 0);
}

bool async_ui_read_pref(const char* key, const char* v) {
    if (!strcmp(key, "async_x")) U.ox = (float)atof(v);
    else if (!strcmp(key, "async_y")) U.oy = (float)atof(v);
    else if (!strcmp(key, "async_compact")) U.compact = atoi(v) != 0;
    else return false;
    return true;
}

int async_ui_cards_shown(void) { return U.shown; }
void async_ui_set_compact(bool c) { U.compact = c; }

static uint32_t col(float r, float g, float b, float a) { return igGetColorU32_Vec4((ImVec4){ r, g, b, a }); }

static void fmt_time(char* out, size_t cap, double s) {
    if (s < 10.0) snprintf(out, cap, "%.1f s", s);
    else if (s < 60.0) snprintf(out, cap, "%.0f s", s);
    else snprintf(out, cap, "%d min %02d s", (int)(s / 60.0), (int)fmod(s, 60.0));
}

// the bar: known progress filled; unknown: a segment sliding back and forth
static void bar(ImDrawList* dl, float x0, float y0, float x1, float h, float p, uint32_t fill, double now, float alpha) {
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y0 + h }, col(1, 1, 1, 0.08f * alpha), h * 0.5f, 0);
    if (p >= 0.0f) {
        float w = (x1 - x0) * (p < 0.0f ? 0.0f : p > 1.0f ? 1.0f : p);
        if (w > 0.5f) ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + w, y0 + h }, fill, h * 0.5f, 0);
    } else {
        float seg = (x1 - x0) * 0.28f, t = (float)fmod(now * 0.8, 2.0);
        float s = t < 1.0f ? t : 2.0f - t; // (back and forth)
        s = s * s * (3.0f - 2.0f * s);
        float a = x0 + (x1 - x0 - seg) * s;
        ImDrawList_AddRectFilled(dl, (ImVec2){ a, y0 }, (ImVec2){ a + seg, y0 + h }, fill, h * 0.5f, 0);
    }
}

static float card_alpha(const AsyncView* v, double now) {
    float a = (float)((now - v->t_start) / 0.18); // (in: from below; out: the last 0.4 s of its linger)
    if (a > 1.0f) a = 1.0f;
    if (v->t_end > 0.0) {
        double linger = v->state == ASYNC_FAILED ? 8.0 : v->state == ASYNC_CANCELLED ? 1.5 : 2.5;
        double left = v->t_end + linger - now;
        if (left < 0.4) a *= (float)(left / 0.4);
    }
    return a < 0.0f ? 0.0f : a;
}

static void state_color(const AsyncView* v, float* r, float* g, float* b) {
    if (v->state == ASYNC_FAILED) *r = 0.92f, *g = 0.33f, *b = 0.30f;
    else if (v->state == ASYNC_CANCELLED) *r = 0.6f, *g = 0.6f, *b = 0.62f;
    else if (v->state == ASYNC_DONE) *r = 0.35f, *g = 0.78f, *b = 0.42f;
    else { // the theme's accent (the slider's grab)
        ImVec4 c = igGetStyle()->Colors[ImGuiCol_SliderGrabActive];
        *r = c.x, *g = c.y, *b = c.z;
    }
}

// one card: icon, label, Cancel / close; status; bar, percent, time
static void card(const AsyncView* v, float w, double now) {
    ImDrawList* dl = igGetWindowDrawList();
    float fs = igGetFontSize(), pad = fs * 0.55f, a = card_alpha(v, now);
    float r, g, b;
    state_color(v, &r, &g, &b);
    ImVec2 p0 = igGetCursorScreenPos();
    float h = pad * 2.0f + fs * 2.0f + fs * 0.5f + 6.0f;
    ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p0.x + w, p0.y + h }, col(0.13f, 0.135f, 0.145f, 0.97f * a), 5.0f, 0);
    ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p0.x + 3.0f, p0.y + h }, col(r, g, b, a), 5.0f, ImDrawFlags_RoundCornersLeft);
    float x = p0.x + pad + 3.0f, y = p0.y + pad, ic = fs * 0.95f;
    uint32_t icol = col(r, g, b, a);
    if (v->state == ASYNC_DONE) ui_icon(dl, ICON_CHECK, x, y, ic, icol);
    else if (v->state == ASYNC_FAILED || v->state == ASYNC_CANCELLED) ui_icon(dl, ICON_CROSS, x, y, ic, icol);
    else ui_spinner(dl, x + ic * 0.5f, y + ic * 0.5f, ic * 0.42f, icol, now);
    // the close / Cancel button (top right)
    float bx = p0.x + w - pad - fs, by = y;
    bool running = v->t_end <= 0.0;
    bool can_x = !running || (v->flags & ASYNC_CANCELLABLE);
    if (can_x) {
        igSetCursorScreenPos((ImVec2){ bx - 2.0f, by - 2.0f });
        char id[32];
        snprintf(id, sizeof id, "##ax%u", v->id);
        if (igInvisibleButton(id, (ImVec2){ fs + 4.0f, fs + 4.0f }, 0)) {
            if (running) async_cancel(v->id);
            else async_dismiss(v->id);
        }
        bool hov = igIsItemHovered(0);
        if (hov) igSetTooltip("%s", running ? TR("Cancel") : TR("Close"));
        ui_icon(dl, ICON_CROSS, bx, by, fs * 0.8f, col(1, 1, 1, (hov ? 0.9f : 0.45f) * a));
    }
    // the label and the status (cut to the card)
    float tx = x + ic + pad * 0.8f, tw = (can_x ? bx - pad : p0.x + w - pad) - tx;
    ImDrawList_PushClipRect(dl, (ImVec2){ tx, p0.y }, (ImVec2){ tx + tw, p0.y + h }, true);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ tx, y - 1.0f }, col(0.93f, 0.93f, 0.94f, a), v->label, NULL);
    ImDrawList_PopClipRect(dl);
    char st[200];
    char tbuf[32];
    if (v->state == ASYNC_DONE) {
        fmt_time(tbuf, sizeof tbuf, v->t_end - v->t_start);
        snprintf(st, sizeof st, TR("Done in %s"), tbuf);
    } else if (v->state == ASYNC_CANCELLED) snprintf(st, sizeof st, "%s", TR("Cancelled"));
    else if (v->state == ASYNC_QUEUED) snprintf(st, sizeof st, "%s", TR("Waiting for a free worker..."));
    else snprintf(st, sizeof st, "%s", v->status[0] ? v->status : TR("Working..."));
    if (v->state == ASYNC_FAILED && v->status[0]) snprintf(st, sizeof st, "%s", v->status);
    float sx = x + 0.0f, sw = p0.x + w - pad - sx;
    ImDrawList_PushClipRect(dl, (ImVec2){ sx, p0.y }, (ImVec2){ sx + sw, p0.y + h }, true);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ sx, y + fs + 3.0f }, col(0.70f, 0.71f, 0.74f, a), st, NULL);
    ImDrawList_PopClipRect(dl);
    // the bar and its numbers: "38%" or "4 / 12", the time so far and the time left
    char right[96] = "";
    double el = (running ? now : v->t_end) - v->t_start;
    fmt_time(tbuf, sizeof tbuf, el);
    if (running && v->progress > 0.04f && el > 0.5) {
        char left[32];
        fmt_time(left, sizeof left, el / v->progress - el);
        if (v->total > 0) snprintf(right, sizeof right, "%d / %d  %s  ~%s", v->done, v->total, tbuf, left);
        else snprintf(right, sizeof right, "%.0f%%  %s  ~%s", v->progress * 100.0f, tbuf, left);
    } else if (running) {
        if (v->total > 0) snprintf(right, sizeof right, "%d / %d  %s", v->done, v->total, tbuf);
        else if (v->progress >= 0.0f) snprintf(right, sizeof right, "%.0f%%  %s", v->progress * 100.0f, tbuf);
        else snprintf(right, sizeof right, "%s", tbuf);
    }
    float by2 = y + fs * 2.0f + 6.0f;
    float rw = right[0] ? igCalcTextSize(right, NULL, false, -1).x : 0.0f;
    float bx1 = p0.x + w - pad - (rw > 0.0f ? rw + pad : 0.0f);
    float prog = v->state == ASYNC_DONE ? 1.0f : v->state == ASYNC_FAILED || v->state == ASYNC_CANCELLED ? (v->progress < 0.0f ? 0.0f : v->progress) : v->progress;
    bar(dl, x, by2 + fs * 0.5f - 3.0f, bx1, 5.0f, prog, col(r, g, b, a), now, a);
    if (right[0]) ImDrawList_AddText_Vec2(dl, (ImVec2){ bx1 + pad, by2 }, col(0.78f, 0.79f, 0.82f, a), right, NULL);
    igSetCursorScreenPos(p0);
    igDummy((ImVec2){ w, h });
}

// thin: one bar per task (the text on hover)
static void thin(const AsyncView* v, float w, double now) {
    ImDrawList* dl = igGetWindowDrawList();
    float a = card_alpha(v, now), r, g, b;
    state_color(v, &r, &g, &b);
    ImVec2 p0 = igGetCursorScreenPos();
    char id[32];
    snprintf(id, sizeof id, "##at%u", v->id);
    igInvisibleButton(id, (ImVec2){ w, 9.0f }, 0);
    bool hov = igIsItemHovered(0);
    bar(dl, p0.x, p0.y + 2.0f, p0.x + w, hov ? 6.0f : 4.0f, v->state == ASYNC_DONE ? 1.0f : v->progress, col(r, g, b, a), now, a);
    if (hov) {
        igBeginTooltip();
        igTextUnformatted(v->label, NULL);
        if (v->status[0]) igTextDisabled("%s", v->status);
        if (v->progress >= 0.0f) igText("%.0f%%", v->progress * 100.0f);
        igEndTooltip();
    }
}

void async_ui_draw(double now) {
    static AsyncView views[MAX_VIEW];
    int n = async_snapshot(views, MAX_VIEW, now);
    U.shown = 0;
    if (n == 0) {
        U.dragging = false;
        return;
    }
    const ImGuiViewport* vp = igGetMainViewport();
    float fs = igGetFontSize();
    if (U.ox < 0.0f || U.oy < 0.0f) U.ox = fs * 0.9f, U.oy = fs * 2.6f; // (above the status bar)
    float w = U.compact ? fs * 13.0f : fs * 24.0f;
    // kept inside the window (the editor resized, or a smaller monitor)
    if (U.ox > vp->WorkSize.x - w) U.ox = fmaxf(vp->WorkSize.x - w, 0.0f);
    if (U.oy > vp->WorkSize.y - fs * 3.0f) U.oy = fmaxf(vp->WorkSize.y - fs * 3.0f, 0.0f);
    igSetNextWindowPos((ImVec2){ vp->WorkPos.x + vp->WorkSize.x - U.ox, vp->WorkPos.y + vp->WorkSize.y - U.oy }, ImGuiCond_Always,
                       (ImVec2){ 1.0f, 1.0f });
    igSetNextWindowViewport(vp->ID);
    igSetNextWindowBgAlpha(0.0f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 0, 0 });
    igPushStyleVar_Vec2(ImGuiStyleVar_ItemSpacing, (ImVec2){ 0, U.compact ? 2.0f : 5.0f });
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (igBegin("##async_cards", NULL, fl)) {
        igBringWindowToDisplayFront(igGetCurrentWindow()); // (over the panels; the keyboard stays where it was)
        ImDrawList* dl = igGetWindowDrawList();
        int running = 0;
        for (int i = 0; i < n; i++) running += views[i].t_end <= 0.0;
        // the header: the grip (drag: moves the stack), the count, thin / full
        ImVec2 h0 = igGetCursorScreenPos();
        float hh = fs * 1.35f;
        ImDrawList_AddRectFilled(dl, h0, (ImVec2){ h0.x + w, h0.y + hh }, col(0.10f, 0.105f, 0.115f, 0.92f), 5.0f, 0);
        igInvisibleButton("##async_drag", (ImVec2){ w - hh - 2.0f, hh }, 0);
        if (igIsItemActive() && igIsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            ImVec2 d = igGetIO_Nil()->MouseDelta;
            U.ox -= d.x, U.oy -= d.y;
            U.ox = fmaxf(U.ox, 0.0f), U.oy = fmaxf(U.oy, 0.0f);
            U.dragging = true;
        } else if (U.dragging && !igIsItemActive()) {
            U.dragging = false;
            editor_prefs_changed();
        }
        if (igIsItemHovered(0)) {
            igSetMouseCursor(ImGuiMouseCursor_ResizeAll);
            igSetTooltip("%s", TR("Background tasks: drag to move them. The editor keeps working while they run."));
        }
        // the grip's dots
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 2; j++)
                ImDrawList_AddCircleFilled(dl, (ImVec2){ h0.x + fs * 0.55f + j * 4.0f, h0.y + hh * 0.5f - 4.0f + k * 4.0f }, 1.2f,
                                           col(1, 1, 1, 0.35f), 6);
        char hdr[64];
        if (running) snprintf(hdr, sizeof hdr, running == 1 ? TR("%d task") : TR("%d tasks"), running);
        else snprintf(hdr, sizeof hdr, "%s", TR("Finished"));
        ImDrawList_AddText_Vec2(dl, (ImVec2){ h0.x + fs * 1.3f, h0.y + (hh - fs) * 0.5f }, col(0.78f, 0.79f, 0.82f, 1.0f), hdr, NULL);
        igSameLine(0, 2.0f);
        if (igInvisibleButton("##async_compact", (ImVec2){ hh, hh }, 0)) U.compact = !U.compact, editor_prefs_changed();
        bool hov = igIsItemHovered(0);
        if (hov) igSetTooltip("%s", U.compact ? TR("Show the cards") : TR("Make them thin (only the progress)"));
        {
            ImVec2 b0 = igGetItemRectMin();
            float cx = b0.x + hh * 0.5f, cy = b0.y + hh * 0.5f, s = fs * 0.32f;
            uint32_t c = col(1, 1, 1, hov ? 0.95f : 0.6f);
            if (U.compact) ImDrawList_AddRect(dl, (ImVec2){ cx - s, cy - s }, (ImVec2){ cx + s, cy + s }, c, 1.5f, 1.4f, 0);
            else ImDrawList_AddLine(dl, (ImVec2){ cx - s, cy + s * 0.6f }, (ImVec2){ cx + s, cy + s * 0.6f }, c, 1.6f);
        }
        if (U.compact) {
            for (int i = 0; i < n; i++) thin(&views[i], w, now);
            U.shown = n;
        } else {
            int first = n > MAX_CARDS ? n - MAX_CARDS : 0; // (the newest at the bottom: the corner)
            if (first > 0) {
                char more[64];
                snprintf(more, sizeof more, TR("+%d more (thin mode shows all)"), first);
                ImVec2 m0 = igGetCursorScreenPos();
                float mh = fs * 1.3f;
                ImDrawList_AddRectFilled(dl, m0, (ImVec2){ m0.x + w, m0.y + mh }, col(0.10f, 0.105f, 0.115f, 0.92f), 5.0f, 0);
                ImDrawList_AddText_Vec2(dl, (ImVec2){ m0.x + fs * 0.7f, m0.y + (mh - fs) * 0.5f }, col(0.62f, 0.63f, 0.66f, 1.0f), more, NULL);
                igDummy((ImVec2){ w, mh });
            }
            for (int i = first; i < n; i++) {
                igPushID_Int((int)views[i].id);
                card(&views[i], w, now);
                igPopID();
            }
            U.shown = n - first;
        }
    }
    igEnd();
    igPopStyleVar(3);
}
