#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "ui_icons.h"
#include "content.h"
#include "theme.h"
#include "ui_tips.h"

static ImVec2 P(float x, float y) { return (ImVec2){ x, y }; }

#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

// col's alpha applied to a fixed color (the view modes' icons keep their colors, dimmed when disabled)
static uint32_t A(uint32_t c, uint32_t col) { return (c & 0x00FFFFFFu) | ((((c >> 24) * (col >> 24)) / 255u) << 24); }

// A sphere lit from the upper left: dark body, a lighter offset disc, the light side, an optional highlight
static void shaded_sphere(ImDrawList* dl, float cx, float cy, float r, uint32_t dark, uint32_t mid, uint32_t light, bool spec,
                          uint32_t col) {
    ImDrawList_AddCircleFilled(dl, P(cx, cy), r, dark, 28);
    ImDrawList_AddCircleFilled(dl, P(cx - r * 0.16f, cy - r * 0.16f), r * 0.80f, mid, 28);
    ImDrawList_AddCircleFilled(dl, P(cx - r * 0.30f, cy - r * 0.30f), r * 0.50f, light, 24);
    if (spec) ImDrawList_AddCircleFilled(dl, P(cx - r * 0.38f, cy - r * 0.38f), r * 0.17f, A(RGBA(255, 255, 255, 255), col), 12);
}

// (Q5) The duotone (notas-dev/icones-e-tooltips.md): a body is filled at ~18% in its family's color (g_fill) and
// outlined in the stroke color (col: the text's); details (dots, arrow heads) stay solid in the stroke color
static uint32_t g_fill;
static void body_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, uint32_t col, float t, ImDrawFlags f) {
    ImDrawList_AddRectFilled(dl, a, b, g_fill, r, f);
    ImDrawList_AddRect(dl, a, b, col, r, t, f);
}
static void body_circle(ImDrawList* dl, ImVec2 c, float r, uint32_t col, float t) {
    ImDrawList_AddCircleFilled(dl, c, r, g_fill, 0);
    ImDrawList_AddCircle(dl, c, r, col, 0, t);
}
static void body_poly(ImDrawList* dl, const ImVec2* pts, int n, uint32_t col, float t) {
    ImDrawList_AddConvexPolyFilled(dl, pts, n, g_fill);
    ImDrawList_AddPolyline(dl, pts, n, col, t, ImDrawFlags_Closed);
}
static void body_tri(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, uint32_t col, float t) {
    ImVec2 pts[3] = { a, b, c };
    body_poly(dl, pts, 3, col, t);
}

// A filled pie slice from angle a0 over da (radians)
static void wedge(ImDrawList* dl, float cx, float cy, float r, float a0, float da, uint32_t c) {
    ImDrawList_PathLineTo(dl, P(cx, cy));
    ImDrawList_PathArcTo(dl, P(cx, cy), r, a0, a0 + da, 16);
    ImDrawList_PathFillConvex(dl, c);
}


// --- (Q5) The icons added for every asset kind, tool, state and node family (the same grammar: 16 grid, duotone) ---

static void stroke_path(ImDrawList* dl, const ImVec2* pts, int n, uint32_t col, float t) { ImDrawList_AddPolyline(dl, pts, n, col, t, 0); }
static void orbit_ring(ImDrawList* dl, float cx, float cy, float rx, float ry, uint32_t col, float t) { // the brand's motif
    ImDrawList_AddEllipse(dl, P(cx, cy), P(rx, ry), col, -0.45f, 28, t);
}
static void wave(ImDrawList* dl, float x0, float x1, float cy, float amp, float cycles, uint32_t col, float t) {
    ImVec2 pts[25];
    for (int i = 0; i < 25; i++) {
        float k = (float)i / 24.0f;
        pts[i] = P(x0 + (x1 - x0) * k, cy + sinf(k * cycles * 6.2831853f) * amp * (0.35f + 0.65f * sinf(k * 3.14159f)));
    }
    stroke_path(dl, pts, 25, col, t);
}

static bool icon_more(ImDrawList* dl, UiIcon icon, float x, float y, float s, uint32_t col, float t) {
    float u = s / 16.0f, cx = x + s * 0.5f, cy = y + s * 0.5f;
    switch (icon) {
    // --- assets ---
    case ICON_MATERIAL: // a sphere with its shading: a lit cap and a rim
        body_circle(dl, P(cx, cy), 6.5f * u, col, t);
        ImDrawList_PathArcTo(dl, P(cx, cy), 4.2f * u, 3.6f, 4.9f, 10);
        ImDrawList_PathStroke(dl, col, t, 0);
        ImDrawList_AddCircleFilled(dl, P(cx - 2.6f * u, cy - 2.6f * u), 1.1f * u, col, 8);
        break;
    case ICON_MATERIAL_INSTANCE: // the material and a small one made from it
        body_circle(dl, P(x + 6.5f * u, y + 6.5f * u), 5.0f * u, col, t);
        body_circle(dl, P(x + 11.5f * u, y + 11.5f * u), 3.2f * u, col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 4.6f * u, y + 4.6f * u), 0.9f * u, col, 8);
        break;
    case ICON_TEXTURE: // a checker tile
        body_rect(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), 1.5f * u, col, t, 0);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++)
                ImDrawList_AddRectFilled(dl, P(x + (2 + 6 * (float)i + (j ? 3 : 0)) * u, y + (2 + 6 * (float)j) * u),
                                         P(x + (5 + 6 * (float)i + (j ? 3 : 0)) * u, y + (5 + 6 * (float)j) * u), col, 0, 0);
        break;
    case ICON_STATIC_MESH:
    case ICON_RASSET: { // an isometric box (a .rasset: with the Tlender's arrow into it)
        ImVec2 a = P(cx, y + 1.5f * u), b = P(x + 14 * u, y + 5 * u), c = P(cx, y + 8.5f * u), d = P(x + 2 * u, y + 5 * u);
        ImVec2 e = P(x + 14 * u, y + 11.5f * u), f = P(cx, y + 15 * u), g = P(x + 2 * u, y + 11.5f * u);
        ImVec2 top[4] = { a, b, c, d }, out[6] = { a, b, e, f, g, d };
        ImDrawList_AddConvexPolyFilled(dl, out, 6, g_fill);
        ImDrawList_AddConvexPolyFilled(dl, top, 4, g_fill); // (the top a little lighter)
        ImDrawList_AddPolyline(dl, out, 6, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, d, c, col, t), ImDrawList_AddLine(dl, c, b, col, t), ImDrawList_AddLine(dl, c, f, col, t);
        if (icon == ICON_RASSET) ImDrawList_AddCircleFilled(dl, P(x + 13.5f * u, y + 2.5f * u), 1.6f * u, col, 10);
        break;
    }
    case ICON_SKELETAL_MESH: // a figure with its bones' joints
        body_circle(dl, P(cx, y + 3 * u), 2.3f * u, col, t);
        body_rect(dl, P(x + 5.5f * u, y + 6 * u), P(x + 10.5f * u, y + 10.5f * u), 1.6f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 6.6f * u, y + 10.5f * u), P(x + 6 * u, y + 15 * u), col, t * 1.4f);
        ImDrawList_AddLine(dl, P(x + 9.4f * u, y + 10.5f * u), P(x + 10 * u, y + 15 * u), col, t * 1.4f);
        ImDrawList_AddLine(dl, P(x + 5.5f * u, y + 7 * u), P(x + 2.5f * u, y + 10.5f * u), col, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 7 * u), P(x + 13.5f * u, y + 10.5f * u), col, t * 1.3f);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 8.2f * u), 1.0f * u, col, 8);
        break;
    case ICON_SKELETON: { // a bone
        ImDrawList_AddLine(dl, P(x + 4.5f * u, y + 11.5f * u), P(x + 11.5f * u, y + 4.5f * u), col, t * 2.4f);
        float k[4][2] = { { 3, 10.5f }, { 5.5f, 13 }, { 10.5f, 3 }, { 13, 5.5f } };
        for (int i = 0; i < 4; i++) body_circle(dl, P(x + k[i][0] * u, y + k[i][1] * u), 2.0f * u, col, t);
        break;
    }
    case ICON_ANIMATION: // a figure running and its motion lines
        body_circle(dl, P(x + 9.5f * u, y + 2.8f * u), 2.0f * u, col, t);
        { ImVec2 b[3] = { P(x + 9 * u, y + 5.5f * u), P(x + 7.5f * u, y + 10 * u), P(x + 10.5f * u, y + 15 * u) }; stroke_path(dl, b, 3, col, t * 1.5f); }
        { ImVec2 l[2] = { P(x + 7.5f * u, y + 10 * u), P(x + 4.5f * u, y + 14.5f * u) }; stroke_path(dl, l, 2, col, t * 1.5f); }
        { ImVec2 a[3] = { P(x + 5 * u, y + 7 * u), P(x + 8.6f * u, y + 6.5f * u), P(x + 12.5f * u, y + 9 * u) }; stroke_path(dl, a, 3, col, t * 1.3f); }
        ImDrawList_AddLine(dl, P(x + 1 * u, y + 8 * u), P(x + 3.5f * u, y + 8 * u), col, t);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 11 * u), P(x + 4 * u, y + 11 * u), col, t);
        break;
    case ICON_MONTAGE: // a film strip in sections
        body_rect(dl, P(x + 1.5f * u, y + 4 * u), P(x + 14.5f * u, y + 12 * u), 1.0f * u, col, t, 0);
        for (int i = 0; i < 4; i++) {
            ImDrawList_AddRectFilled(dl, P(x + (2.6f + 3.2f * (float)i) * u, y + 4.8f * u), P(x + (4.0f + 3.2f * (float)i) * u, y + 6 * u), col, 0, 0);
            ImDrawList_AddRectFilled(dl, P(x + (2.6f + 3.2f * (float)i) * u, y + 10 * u), P(x + (4.0f + 3.2f * (float)i) * u, y + 11.2f * u), col, 0, 0);
        }
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 6.5f * u), P(x + 6 * u, y + 9.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 6.5f * u), P(x + 10.5f * u, y + 9.5f * u), col, t);
        break;
    case ICON_BLEND_SPACE: // a grid with the sample between its points
        body_rect(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), 1.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(cx, y + 14 * u), col, t * 0.7f);
        ImDrawList_AddLine(dl, P(x + 2 * u, cy), P(x + 14 * u, cy), col, t * 0.7f);
        ImDrawList_AddCircleFilled(dl, P(x + 10.5f * u, y + 5.5f * u), 1.9f * u, col, 10);
        break;
    case ICON_ANIM_BP: // the Blueprint sheet with a figure's head and the state's arrow
        body_rect(dl, P(x + 1.5f * u, y + 1.5f * u), P(x + 14.5f * u, y + 14.5f * u), 2.0f * u, col, t, 0);
        ImDrawList_AddCircle(dl, P(x + 5.5f * u, y + 5.5f * u), 2.0f * u, col, 12, t);
        ImDrawList_AddLine(dl, P(x + 5.5f * u, y + 8 * u), P(x + 5.5f * u, y + 12 * u), col, t);
        ImDrawList_AddRect(dl, P(x + 9 * u, y + 9 * u), P(x + 13 * u, y + 12.5f * u), col, 1.0f * u, t, 0);
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 10.7f * u), P(x + 9 * u, y + 10.7f * u), col, t);
        break;
    case ICON_BEHAVIOR_TREE: // a root and its two children
        body_rect(dl, P(x + 5.5f * u, y + 1.5f * u), P(x + 10.5f * u, y + 5.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 1.5f * u, y + 10.5f * u), P(x + 6.5f * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 9.5f * u, y + 10.5f * u), P(x + 14.5f * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        { ImVec2 l[4] = { P(x + 4 * u, y + 10.5f * u), P(x + 4 * u, y + 8 * u), P(x + 12 * u, y + 8 * u), P(x + 12 * u, y + 10.5f * u) }; stroke_path(dl, l, 4, col, t); }
        ImDrawList_AddLine(dl, P(cx, y + 5.5f * u), P(cx, y + 8 * u), col, t);
        break;
    case ICON_BRAIN: // utility AI: a head with its scores
        body_circle(dl, P(cx, y + 7.5f * u), 6.0f * u, col, t);
        for (int i = 0; i < 3; i++)
            ImDrawList_AddRectFilled(dl, P(x + (4.5f + 2.6f * (float)i) * u, y + (9.5f - 2.0f * (float)i) * u), P(x + (6 + 2.6f * (float)i) * u, y + 11 * u), col, 0, 0);
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 13.5f * u), P(x + 6 * u, y + 15.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 13.5f * u), P(x + 10 * u, y + 15.5f * u), col, t);
        break;
    case ICON_INPUT_ACTION: // a key being pressed
        body_rect(dl, P(x + 3 * u, y + 3 * u), P(x + 13 * u, y + 11 * u), 2.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 3.5f * u, y + 13.5f * u), P(x + 12.5f * u, y + 13.5f * u), col, t * 1.2f);
        ImDrawList_AddTriangleFilled(dl, P(cx, y + 9 * u), P(cx - 2.2f * u, y + 5.5f * u), P(cx + 2.2f * u, y + 5.5f * u), col);
        break;
    case ICON_INPUT_CONTEXT: // a keyboard
    case ICON_KEYBOARD:
        body_rect(dl, P(x + 1 * u, y + 4 * u), P(x + 15 * u, y + 12.5f * u), 1.5f * u, col, t, 0);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 5; i++)
                ImDrawList_AddRectFilled(dl, P(x + (2.6f + 2.3f * (float)i) * u, y + (5.6f + 2.2f * (float)j) * u),
                                         P(x + (3.8f + 2.3f * (float)i) * u, y + (6.8f + 2.2f * (float)j) * u), col, 0, 0);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 10.6f * u), P(x + 11 * u, y + 10.6f * u), col, t);
        if (icon == ICON_INPUT_CONTEXT) ImDrawList_AddCircleFilled(dl, P(x + 13.5f * u, y + 3 * u), 1.6f * u, col, 10);
        break;
    case ICON_MACRO_LIB:
    case ICON_MACRO: { // braces around a node
        ImVec2 l[4] = { P(x + 4.5f * u, y + 2 * u), P(x + 2.5f * u, y + 3.5f * u), P(x + 2.5f * u, y + 12.5f * u), P(x + 4.5f * u, y + 14 * u) };
        ImVec2 r[4] = { P(x + 11.5f * u, y + 2 * u), P(x + 13.5f * u, y + 3.5f * u), P(x + 13.5f * u, y + 12.5f * u), P(x + 11.5f * u, y + 14 * u) };
        stroke_path(dl, l, 4, col, t * 1.2f), stroke_path(dl, r, 4, col, t * 1.2f);
        body_rect(dl, P(x + 5.5f * u, y + 5.5f * u), P(x + 10.5f * u, y + 10.5f * u), 1.0f * u, col, t, 0);
        if (icon == ICON_MACRO_LIB) ImDrawList_AddLine(dl, P(x + 6.5f * u, y + 8 * u), P(x + 9.5f * u, y + 8 * u), col, t);
        break;
    }
    case ICON_SEQUENCE: // a clapperboard
        body_rect(dl, P(x + 1.5f * u, y + 6 * u), P(x + 14.5f * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 1.5f * u, y + 2 * u), P(x + 14.5f * u, y + 5.5f * u), 0.8f * u, col, t, 0); // (the clapper)
        for (int i = 0; i < 3; i++) ImDrawList_AddLine(dl, P(x + (4 + 3.5f * (float)i) * u, y + 5.5f * u), P(x + (6 + 3.5f * (float)i) * u, y + 2 * u), col, t);
        ImDrawList_AddTriangleFilled(dl, P(x + 6.5f * u, y + 8 * u), P(x + 10.5f * u, y + 10.25f * u), P(x + 6.5f * u, y + 12.5f * u), col);
        break;
    case ICON_SOUND_WAVE: // a waveform in its frame
        body_rect(dl, P(x + 1 * u, y + 3 * u), P(x + 15 * u, y + 13 * u), 1.5f * u, col, t, 0);
        for (int i = 0; i < 7; i++) {
            static const float h[7] = { 1.0f, 2.5f, 4.0f, 2.0f, 3.2f, 1.5f, 0.8f };
            float xx = x + (3.0f + 1.7f * (float)i) * u;
            ImDrawList_AddLine(dl, P(xx, cy - h[i] * u), P(xx, cy + h[i] * u), col, t);
        }
        break;
    case ICON_SOUND_CUE: // a speaker with a node beside it
    case ICON_METASOUND: { // (a MetaSound: a wave from a node)
        ImVec2 body[6] = { P(x + 1.5f * u, y + 6 * u), P(x + 4 * u, y + 6 * u), P(x + 7.5f * u, y + 3 * u), P(x + 7.5f * u, y + 13 * u),
                           P(x + 4 * u, y + 10 * u), P(x + 1.5f * u, y + 10 * u) };
        if (icon == ICON_SOUND_CUE) body_poly(dl, body, 6, col, t);
        else wave(dl, x + 1 * u, x + 8.5f * u, cy, 4.0f * u, 1.5f, col, t * 1.2f);
        body_rect(dl, P(x + 10 * u, y + 5.5f * u), P(x + 15 * u, y + 10.5f * u), 1.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 8.5f * u, cy), P(x + 10 * u, cy), col, t);
        break;
    }
    case ICON_ATTENUATION: // a source and its rings fading with the distance
        ImDrawList_AddCircleFilled(dl, P(x + 3 * u, cy), 1.8f * u, col, 10);
        for (int i = 0; i < 3; i++) {
            ImDrawList_PathArcTo(dl, P(x + 3 * u, cy), (4.0f + 3.6f * (float)i) * u, -0.9f, 0.9f, 12);
            ImDrawList_PathStroke(dl, theme_alpha(col, (unsigned)((col >> 24) * (1.0f - 0.28f * (float)i))), t, 0);
        }
        break;
    case ICON_SOUND_CLASS: // a tag with a note
    case ICON_TAG: {
        ImVec2 q[5] = { P(x + 1.5f * u, y + 3 * u), P(x + 9.5f * u, y + 3 * u), P(x + 14.5f * u, cy), P(x + 9.5f * u, y + 13 * u), P(x + 1.5f * u, y + 13 * u) };
        body_poly(dl, q, 5, col, t);
        if (icon == ICON_TAG) ImDrawList_AddCircle(dl, P(x + 10 * u, cy), 1.3f * u, col, 10, t);
        else {
            ImDrawList_AddCircleFilled(dl, P(x + 5.5f * u, y + 10 * u), 1.4f * u, col, 10);
            ImDrawList_AddLine(dl, P(x + 6.8f * u, y + 10 * u), P(x + 6.8f * u, y + 5 * u), col, t);
            ImDrawList_AddLine(dl, P(x + 6.8f * u, y + 5 * u), P(x + 9 * u, y + 6 * u), col, t);
        }
        break;
    }
    case ICON_SOUND_MIX: // three faders
        for (int i = 0; i < 3; i++) {
            float xx = x + (3.5f + 4.5f * (float)i) * u, k = y + (float)(i == 0 ? 10 : i == 1 ? 5 : 8) * u;
            ImDrawList_AddLine(dl, P(xx, y + 2 * u), P(xx, y + 14 * u), col, t);
            body_rect(dl, P(xx - 1.8f * u, k - 1.3f * u), P(xx + 1.8f * u, k + 1.3f * u), 0.6f * u, col, t, 0);
        }
        break;
    case ICON_CONCURRENCY: // voices stacked, the last cut
        for (int i = 0; i < 3; i++) body_rect(dl, P(x + 2 * u, y + (2.5f + 4 * (float)i) * u), P(x + (14 - 3 * (float)i) * u, y + (5 + 4 * (float)i) * u), 1.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 11.5f * u), P(x + 14 * u, y + 14.5f * u), col, t * 1.3f);
        break;
    case ICON_SUBMIX: // two lines merging into one
        ImDrawList_AddBezierCubic(dl, P(x + 1.5f * u, y + 3.5f * u), P(x + 6 * u, y + 3.5f * u), P(x + 6 * u, cy), P(x + 10 * u, cy), col, t * 1.2f, 12);
        ImDrawList_AddBezierCubic(dl, P(x + 1.5f * u, y + 12.5f * u), P(x + 6 * u, y + 12.5f * u), P(x + 6 * u, cy), P(x + 10 * u, cy), col, t * 1.2f, 12);
        body_circle(dl, P(x + 12.5f * u, cy), 2.4f * u, col, t);
        break;
    case ICON_SCRIPT: // code: </>
        { ImVec2 l[3] = { P(x + 5 * u, y + 4 * u), P(x + 1.5f * u, cy), P(x + 5 * u, y + 12 * u) }; stroke_path(dl, l, 3, col, t * 1.4f); }
        { ImVec2 r[3] = { P(x + 11 * u, y + 4 * u), P(x + 14.5f * u, cy), P(x + 11 * u, y + 12 * u) }; stroke_path(dl, r, 3, col, t * 1.4f); }
        ImDrawList_AddLine(dl, P(x + 9.5f * u, y + 2.5f * u), P(x + 6.5f * u, y + 13.5f * u), col, t * 1.4f);
        break;
    case ICON_PARTICLES: { // a spark and its smaller ones
        ImVec2 st[8];
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f - 1.5708f, r = (i & 1) ? 1.8f : 5.5f;
            st[i] = P(x + 6.5f * u + cosf(a) * r * u, y + 9.5f * u + sinf(a) * r * u);
        }
        ImDrawList_AddConcavePolyFilled(dl, st, 8, g_fill);
        ImDrawList_AddPolyline(dl, st, 8, col, t, ImDrawFlags_Closed);
        ImDrawList_AddCircleFilled(dl, P(x + 13 * u, y + 3.5f * u), 1.5f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 13.5f * u, y + 9 * u), 0.9f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 9.5f * u, y + 2 * u), 0.9f * u, col, 8);
        break;
    }
    case ICON_IK_RIG: { // a limb: three joints and the goal it reaches
        ImVec2 j[3] = { P(x + 2.5f * u, y + 13.5f * u), P(x + 6 * u, y + 5 * u), P(x + 12 * u, y + 9 * u) };
        stroke_path(dl, j, 3, col, t * 1.5f);
        for (int i = 0; i < 3; i++) body_circle(dl, j[i], 1.7f * u, col, t);
        ImDrawList_AddCircle(dl, P(x + 13 * u, y + 3.5f * u), 2.2f * u, col, 12, t);
        ImDrawList_AddLine(dl, P(x + 13 * u, y + 1 * u), P(x + 13 * u, y + 6 * u), col, t * 0.8f);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 3.5f * u), P(x + 15.5f * u, y + 3.5f * u), col, t * 0.8f);
        break;
    }
    case ICON_PHYSICS_ASSET: // two capsules and their joint
        body_rect(dl, P(x + 2 * u, y + 1.5f * u), P(x + 7 * u, y + 9 * u), 2.5f * u, col, t, 0);
        body_rect(dl, P(x + 9 * u, y + 7 * u), P(x + 14 * u, y + 14.5f * u), 2.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 8 * u), P(x + 10 * u, y + 8.5f * u), col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 8 * u, y + 8.2f * u), 1.4f * u, col, 10);
        break;
    case ICON_VEHICLE: // a car: body, cabin, wheels
        body_rect(dl, P(x + 1 * u, y + 7 * u), P(x + 15 * u, y + 11.5f * u), 1.6f * u, col, t, 0);
        { ImVec2 q[4] = { P(x + 4 * u, y + 7 * u), P(x + 5.8f * u, y + 3.5f * u), P(x + 10.5f * u, y + 3.5f * u), P(x + 12.5f * u, y + 7 * u) }; body_poly(dl, q, 4, col, t); }
        body_circle(dl, P(x + 4.5f * u, y + 12 * u), 2.0f * u, col, t);
        body_circle(dl, P(x + 11.5f * u, y + 12 * u), 2.0f * u, col, t);
        break;
    case ICON_DIALOGUE: // two speech bubbles
    case ICON_COMMENT:
        body_rect(dl, P(x + 1 * u, y + 2 * u), P(x + 10.5f * u, y + 8.5f * u), 2.0f * u, col, t, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 3 * u, y + 8.3f * u), P(x + 6 * u, y + 8.3f * u), P(x + 2.6f * u, y + 11 * u), col);
        if (icon == ICON_DIALOGUE) {
            body_rect(dl, P(x + 6.5f * u, y + 7 * u), P(x + 15 * u, y + 12.5f * u), 2.0f * u, col, t, 0);
            ImDrawList_AddTriangleFilled(dl, P(x + 10.5f * u, y + 12.3f * u), P(x + 13 * u, y + 12.3f * u), P(x + 13.6f * u, y + 15 * u), col);
        } else
            for (int i = 0; i < 2; i++) ImDrawList_AddLine(dl, P(x + 3 * u, y + (4.3f + 2.0f * (float)i) * u), P(x + 8.5f * u, y + (4.3f + 2.0f * (float)i) * u), col, t * 0.9f);
        break;
    case ICON_QUEST: // a flag on a marker
        ImDrawList_AddLine(dl, P(x + 4 * u, y + 1.5f * u), P(x + 4 * u, y + 14.5f * u), col, t * 1.4f);
        { ImVec2 q[5] = { P(x + 4 * u, y + 2 * u), P(x + 13.5f * u, y + 2 * u), P(x + 11 * u, y + 5 * u), P(x + 13.5f * u, y + 8 * u), P(x + 4 * u, y + 8 * u) };
          ImDrawList_AddConcavePolyFilled(dl, q, 5, g_fill), ImDrawList_AddPolyline(dl, q, 5, col, t, ImDrawFlags_Closed); }
        ImDrawList_AddEllipse(dl, P(x + 4 * u, y + 14.5f * u), P(3 * u, 1 * u), col, 0, 14, t);
        break;
    case ICON_WIDGET: // a window with a button and a slider
        body_rect(dl, P(x + 1.5f * u, y + 2 * u), P(x + 14.5f * u, y + 14 * u), 1.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 4.8f * u), P(x + 14.5f * u, y + 4.8f * u), col, t);
        ImDrawList_AddRect(dl, P(x + 3.5f * u, y + 7 * u), P(x + 8.5f * u, y + 9.5f * u), col, 1.0f * u, t, 0);
        ImDrawList_AddLine(dl, P(x + 3.5f * u, y + 12 * u), P(x + 12.5f * u, y + 12 * u), col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 9.5f * u, y + 12 * u), 1.2f * u, col, 8);
        break;
    case ICON_DATA_TABLE: // rows and columns
        body_rect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 13.5f * u), 1.0f * u, col, t, 0);
        for (int i = 0; i < 2; i++) ImDrawList_AddLine(dl, P(x + 1.5f * u, y + (6.2f + 3.6f * (float)i) * u), P(x + 14.5f * u, y + (6.2f + 3.6f * (float)i) * u), col, t * 0.8f);
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 2.5f * u), P(x + 6 * u, y + 13.5f * u), col, t * 0.8f);
        ImDrawList_AddLine(dl, P(x + 10.3f * u, y + 2.5f * u), P(x + 10.3f * u, y + 13.5f * u), col, t * 0.8f);
        break;
    case ICON_CURVE: // a curve through its keys
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 14.5f * u), P(x + 14.5f * u, y + 14.5f * u), g_fill, t);
        ImDrawList_AddBezierCubic(dl, P(x + 2 * u, y + 13 * u), P(x + 7 * u, y + 13 * u), P(x + 8 * u, y + 3 * u), P(x + 14 * u, y + 3 * u), col, t * 1.3f, 16);
        ImDrawList_AddCircleFilled(dl, P(x + 2 * u, y + 13 * u), 1.4f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 14 * u, y + 3 * u), 1.4f * u, col, 8);
        break;
    case ICON_THEME: { // a palette
        body_circle(dl, P(cx, cy), 6.5f * u, col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 10.5f * u, y + 10.5f * u), 1.6f * u, TH(TK_BG_PANEL), 10);
        ImDrawList_AddCircle(dl, P(x + 10.5f * u, y + 10.5f * u), 1.6f * u, col, 10, t);
        static const int tk[3] = { TK_ACCENT, TK_ACCENT2, TK_OK };
        for (int i = 0; i < 3; i++) ImDrawList_AddCircleFilled(dl, P(x + (4.8f + 2.8f * (float)i) * u, y + (6 - (i == 1 ? 1.5f : 0)) * u), 1.3f * u, theme_alpha(TH(tk[i]), col >> 24), 8);
        break;
    }
    case ICON_PLUGIN: { // a puzzle piece
        body_rect(dl, P(x + 2 * u, y + 5 * u), P(x + 12 * u, y + 14 * u), 1.0f * u, col, t, 0);
        body_circle(dl, P(x + 7 * u, y + 3.5f * u), 2.0f * u, col, t);
        body_circle(dl, P(x + 13.2f * u, y + 9.5f * u), 2.0f * u, col, t);
        break;
    }
    case ICON_LANDSCAPE: { // mountains
        ImVec2 q[5] = { P(x + 1 * u, y + 14 * u), P(x + 6 * u, y + 4 * u), P(x + 9 * u, y + 9 * u), P(x + 11 * u, y + 6.5f * u), P(x + 15 * u, y + 14 * u) };
        ImDrawList_AddConcavePolyFilled(dl, q, 5, g_fill);
        ImDrawList_AddPolyline(dl, q, 5, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, P(x + 4.6f * u, y + 6.8f * u), P(x + 7.4f * u, y + 6.8f * u), col, t * 0.8f);
        break;
    }
    case ICON_FOLIAGE: { // a leaf
        ImDrawList_PathArcTo(dl, P(x + 4 * u, y + 12 * u), 10.0f * u, -1.5708f, 0.0f, 12);
        ImDrawList_PathArcTo(dl, P(x + 14 * u, y + 2 * u), 10.0f * u, 1.5708f, 3.1416f, 12);
        ImDrawList_PathFillConvex(dl, g_fill);
        ImDrawList_PathArcTo(dl, P(x + 4 * u, y + 12 * u), 10.0f * u, -1.5708f, 0.0f, 12);
        ImDrawList_PathArcTo(dl, P(x + 14 * u, y + 2 * u), 10.0f * u, 1.5708f, 3.1416f, 12);
        ImDrawList_PathStroke(dl, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 14.5f * u), P(x + 11 * u, y + 5 * u), col, t);
        break;
    }
    case ICON_GROOM: // strands of hair
        for (int i = 0; i < 4; i++) {
            float x0 = x + (3 + 3 * (float)i) * u;
            ImDrawList_AddBezierCubic(dl, P(x0, y + 2 * u), P(x0 + 3 * u, y + 6 * u), P(x0 - 3 * u, y + 10 * u), P(x0 + 0.5f * u, y + 14.5f * u), col, t, 12);
        }
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 2 * u), col, t * 1.4f);
        break;
    case ICON_CLOTH: { // a cloth hanging from two pins
        ImVec2 q[6] = { P(x + 2 * u, y + 2.5f * u), P(x + 14 * u, y + 2.5f * u), P(x + 13 * u, y + 13 * u), P(x + 10 * u, y + 11.5f * u),
                        P(x + 6 * u, y + 14 * u), P(x + 3 * u, y + 12 * u) };
        ImDrawList_AddConcavePolyFilled(dl, q, 6, g_fill);
        ImDrawList_AddPolyline(dl, q, 6, col, t, ImDrawFlags_Closed);
        ImDrawList_AddCircleFilled(dl, P(x + 2 * u, y + 2.5f * u), 1.3f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 14 * u, y + 2.5f * u), 1.3f * u, col, 8);
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 3 * u), P(x + 6.5f * u, y + 11 * u), col, t * 0.7f);
        break;
    }
    case ICON_WATER: // a drop over waves
        ImDrawList_PathArcTo(dl, P(cx, y + 8 * u), 3.5f * u, -0.3f, 3.4416f, 12);
        ImDrawList_PathLineTo(dl, P(cx, y + 1.5f * u));
        ImDrawList_PathFillConvex(dl, g_fill);
        ImDrawList_PathArcTo(dl, P(cx, y + 8 * u), 3.5f * u, -0.3f, 3.4416f, 12);
        ImDrawList_PathLineTo(dl, P(cx, y + 1.5f * u));
        ImDrawList_PathStroke(dl, col, t, ImDrawFlags_Closed);
        wave(dl, x + 1 * u, x + 15 * u, y + 14 * u, 1.0f * u, 2.0f, col, t);
        break;
    case ICON_ITEM: // a gem
    {
        ImVec2 q[5] = { P(x + 4 * u, y + 3 * u), P(x + 12 * u, y + 3 * u), P(x + 15 * u, y + 6.5f * u), P(cx, y + 14.5f * u), P(x + 1 * u, y + 6.5f * u) };
        body_poly(dl, q, 5, col, t);
        ImDrawList_AddLine(dl, P(x + 1 * u, y + 6.5f * u), P(x + 15 * u, y + 6.5f * u), col, t * 0.8f);
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 6.5f * u), P(cx, y + 14.5f * u), col, t * 0.8f);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 6.5f * u), P(cx, y + 14.5f * u), col, t * 0.8f);
        break;
    }
    case ICON_ABILITY: { // a star in a ring: a power
        ImVec2 st[10];
        for (int i = 0; i < 10; i++) {
            float a = (float)i * 0.628318f - 1.5708f, r = (i & 1) ? 2.4f : 5.6f;
            st[i] = P(cx + cosf(a) * r * u, cy + 0.5f * u + sinf(a) * r * u);
        }
        ImDrawList_AddConcavePolyFilled(dl, st, 10, g_fill);
        ImDrawList_AddPolyline(dl, st, 10, col, t, ImDrawFlags_Closed);
        break;
    }
    // --- the engine's own: the orbit as their motif ---
    case ICON_PLAY:
        body_tri(dl, P(x + 4.5f * u, y + 2.5f * u), P(x + 13.5f * u, cy), P(x + 4.5f * u, y + 13.5f * u), col, t * 1.1f);
        break;
    case ICON_PAUSE:
        body_rect(dl, P(x + 3.5f * u, y + 2.5f * u), P(x + 7 * u, y + 13.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 9 * u, y + 2.5f * u), P(x + 12.5f * u, y + 13.5f * u), 1.0f * u, col, t, 0);
        break;
    case ICON_STOP:
        body_rect(dl, P(x + 3 * u, y + 3 * u), P(x + 13 * u, y + 13 * u), 1.5f * u, col, t, 0);
        break;
    case ICON_SIMULATE: // Play inside the orbit: the world runs without the player
        orbit_ring(dl, cx, cy, 7.0f * u, 3.6f * u, col, t);
        body_tri(dl, P(x + 6 * u, y + 4.5f * u), P(x + 11.5f * u, cy), P(x + 6 * u, y + 11.5f * u), col, t);
        break;
    case ICON_COMPILE: { // a gear crossed by the orbit, its dot on it
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f, c = cosf(a), sn = sinf(a);
            ImDrawList_AddLine(dl, P(cx + c * 3.6f * u, cy + sn * 3.6f * u), P(cx + c * 5.6f * u, cy + sn * 5.6f * u), col, t * 1.8f);
        }
        body_circle(dl, P(cx, cy), 4.0f * u, col, t);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 1.4f * u, col, 10);
        break;
    }
    case ICON_BUILD: // a hammer
        { ImVec2 q[4] = { P(x + 2 * u, y + 4 * u), P(x + 9 * u, y + 2 * u), P(x + 10.5f * u, y + 5.5f * u), P(x + 3.5f * u, y + 7.5f * u) }; body_poly(dl, q, 4, col, t); }
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 5.5f * u), P(x + 13.5f * u, y + 14.5f * u), col, t * 2.0f);
        break;
    case ICON_PACKAGE: { // a box with its tape
        ImVec2 a = P(cx, y + 1.5f * u), b = P(x + 14 * u, y + 5 * u), c = P(cx, y + 8.5f * u), d = P(x + 2 * u, y + 5 * u);
        ImVec2 e = P(x + 14 * u, y + 11.5f * u), f = P(cx, y + 15 * u), g = P(x + 2 * u, y + 11.5f * u);
        ImVec2 out[6] = { a, b, e, f, g, d };
        ImDrawList_AddConvexPolyFilled(dl, out, 6, g_fill);
        ImDrawList_AddPolyline(dl, out, 6, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, d, c, col, t), ImDrawList_AddLine(dl, c, b, col, t), ImDrawList_AddLine(dl, c, f, col, t);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 3.3f * u), P(x + 11 * u, y + 6.7f * u), col, t * 1.6f);
        break;
    }
    case ICON_PROJECT: // the brand: a planet in its orbit
        body_circle(dl, P(cx, cy), 3.8f * u, col, t);
        orbit_ring(dl, cx, cy, 7.2f * u, 2.6f * u, col, t);
        ImDrawList_AddCircleFilled(dl, P(cx + 5.6f * u, cy - 3.4f * u), 1.3f * u, col, 10);
        break;
    case ICON_CONTENT: // folders stacked: the Content Browser
        body_rect(dl, P(x + 3.5f * u, y + 2 * u), P(x + 14.5f * u, y + 10 * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 1.5f * u, y + 5.5f * u), P(x + 12.5f * u, y + 14 * u), 1.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 8 * u), P(x + 12.5f * u, y + 8 * u), col, t);
        break;
    case ICON_OUTLINER: // a tree list
        ImDrawList_AddLine(dl, P(x + 3 * u, y + 4 * u), P(x + 3 * u, y + 13 * u), col, t);
        for (int i = 0; i < 3; i++) {
            float yy = y + (4 + 4.5f * (float)i) * u, xx = x + (i ? 6 : 1.5f) * u;
            if (i) ImDrawList_AddLine(dl, P(x + 3 * u, yy), P(xx, yy), col, t);
            body_rect(dl, P(xx, yy - 1.5f * u), P(xx + 3 * u, yy + 1.5f * u), 0.5f * u, col, t, 0);
            ImDrawList_AddLine(dl, P(xx + 4.5f * u, yy), P(x + 14.5f * u, yy), col, t);
        }
        break;
    case ICON_DETAILS: // a property sheet: names and fields
        body_rect(dl, P(x + 1.5f * u, y + 1.5f * u), P(x + 14.5f * u, y + 14.5f * u), 1.5f * u, col, t, 0);
        for (int i = 0; i < 3; i++) {
            float yy = y + (4.5f + 3.5f * (float)i) * u;
            ImDrawList_AddLine(dl, P(x + 3.5f * u, yy), P(x + 6.5f * u, yy), col, t);
            ImDrawList_AddRect(dl, P(x + 8 * u, yy - 1.1f * u), P(x + 12.5f * u, yy + 1.1f * u), col, 0.6f * u, t * 0.8f, 0);
        }
        break;
    case ICON_ASSISTANT: // a bubble with a spark: help that answers
        body_rect(dl, P(x + 1.5f * u, y + 2 * u), P(x + 14.5f * u, y + 11.5f * u), 3.0f * u, col, t, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 4 * u, y + 11.3f * u), P(x + 7.5f * u, y + 11.3f * u), P(x + 3.5f * u, y + 14.8f * u), col);
        { ImVec2 st[8];
          for (int i = 0; i < 8; i++) { float a = (float)i * 0.785398f - 1.5708f, r = (i & 1) ? 1.0f : 3.3f; st[i] = P(cx + cosf(a) * r * u, y + 6.75f * u + sinf(a) * r * u); }
          ImDrawList_AddConcavePolyFilled(dl, st, 8, col); }
        break;
    case ICON_GIT: // a branch
        ImDrawList_AddLine(dl, P(x + 4.5f * u, y + 4 * u), P(x + 4.5f * u, y + 12 * u), col, t * 1.2f);
        ImDrawList_AddBezierCubic(dl, P(x + 11.5f * u, y + 6 * u), P(x + 11.5f * u, y + 9 * u), P(x + 4.5f * u, y + 8 * u), P(x + 4.5f * u, y + 11 * u), col, t * 1.2f, 12);
        body_circle(dl, P(x + 4.5f * u, y + 3 * u), 1.8f * u, col, t);
        body_circle(dl, P(x + 4.5f * u, y + 13 * u), 1.8f * u, col, t);
        body_circle(dl, P(x + 11.5f * u, y + 4.5f * u), 1.8f * u, col, t);
        break;
    case ICON_LOCK:
    case ICON_UNLOCK:
        body_rect(dl, P(x + 3 * u, y + 7 * u), P(x + 13 * u, y + 14.5f * u), 1.5f * u, col, t, 0);
        ImDrawList_PathArcTo(dl, P(cx, y + 6 * u), 3.3f * u, 3.1416f, icon == ICON_LOCK ? 6.2832f : 5.4f, 12);
        if (icon == ICON_LOCK) ImDrawList_PathLineTo(dl, P(cx + 3.3f * u, y + 7 * u));
        ImDrawList_PathStroke(dl, col, t * 1.3f, 0);
        ImDrawList_AddLine(dl, P(x + 4.7f * u, y + 6 * u), P(x + 4.7f * u, y + 7 * u), col, t * 1.3f);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 10.5f * u), 1.2f * u, col, 8);
        break;
    case ICON_COPY:
        body_rect(dl, P(x + 5 * u, y + 5 * u), P(x + 14 * u, y + 14.5f * u), 1.2f * u, col, t, 0);
        { ImVec2 l[4] = { P(x + 11 * u, y + 3 * u), P(x + 11 * u, y + 1.5f * u), P(x + 2 * u, y + 1.5f * u), P(x + 2 * u, y + 11 * u) }; stroke_path(dl, l, 4, col, t); }
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 11 * u), P(x + 3.5f * u, y + 11 * u), col, t);
        break;
    case ICON_PASTE: // a clipboard
        body_rect(dl, P(x + 2.5f * u, y + 3 * u), P(x + 13.5f * u, y + 15 * u), 1.2f * u, col, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 5.5f * u, y + 1.5f * u), P(x + 10.5f * u, y + 4.5f * u), col, 1.0f * u, 0);
        for (int i = 0; i < 2; i++) ImDrawList_AddLine(dl, P(x + 5 * u, y + (8 + 3 * (float)i) * u), P(x + 11 * u, y + (8 + 3 * (float)i) * u), col, t);
        break;
    case ICON_CUT: // scissors
        body_circle(dl, P(x + 4 * u, y + 12 * u), 2.4f * u, col, t);
        body_circle(dl, P(x + 12 * u, y + 12 * u), 2.4f * u, col, t);
        ImDrawList_AddLine(dl, P(x + 5.5f * u, y + 10 * u), P(x + 11.5f * u, y + 1.5f * u), col, t * 1.2f);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 10 * u), P(x + 4.5f * u, y + 1.5f * u), col, t * 1.2f);
        break;
    case ICON_LINK: // two links of a chain
        ImDrawList_AddRect(dl, P(x + 1.5f * u, y + 5.5f * u), P(x + 9 * u, y + 10.5f * u), col, 2.5f * u, t * 1.3f, 0);
        ImDrawList_AddRect(dl, P(x + 7 * u, y + 5.5f * u), P(x + 14.5f * u, y + 10.5f * u), col, 2.5f * u, t * 1.3f, 0);
        break;
    case ICON_FILTER: { // a funnel
        ImVec2 q[6] = { P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 2.5f * u), P(x + 9.5f * u, y + 8.5f * u), P(x + 9.5f * u, y + 14 * u),
                        P(x + 6.5f * u, y + 12.5f * u), P(x + 6.5f * u, y + 8.5f * u) };
        ImDrawList_AddConcavePolyFilled(dl, q, 6, g_fill);
        ImDrawList_AddPolyline(dl, q, 6, col, t, ImDrawFlags_Closed);
        break;
    }
    case ICON_SORT: // bars from long to short, an arrow down
        for (int i = 0; i < 3; i++) ImDrawList_AddLine(dl, P(x + 1.5f * u, y + (4 + 4 * (float)i) * u), P(x + (10 - 2.5f * (float)i) * u, y + (4 + 4 * (float)i) * u), col, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 13 * u, y + 2.5f * u), P(x + 13 * u, y + 12 * u), col, t);
        ImDrawList_AddTriangleFilled(dl, P(x + 13 * u, y + 14.5f * u), P(x + 10.8f * u, y + 11 * u), P(x + 15.2f * u, y + 11 * u), col);
        break;
    case ICON_REFRESH: // two arrows chasing each other
        ImDrawList_PathArcTo(dl, P(cx, cy), 5.5f * u, 3.6f, 5.9f, 12);
        ImDrawList_PathStroke(dl, col, t * 1.3f, 0);
        ImDrawList_PathArcTo(dl, P(cx, cy), 5.5f * u, 0.5f, 2.8f, 12);
        ImDrawList_PathStroke(dl, col, t * 1.3f, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 14.8f * u, y + 6.5f * u), P(x + 11.5f * u, y + 6 * u), P(x + 14 * u, y + 3 * u), col);
        ImDrawList_AddTriangleFilled(dl, P(x + 1.2f * u, y + 9.5f * u), P(x + 4.5f * u, y + 10 * u), P(x + 2 * u, y + 13 * u), col);
        break;
    case ICON_DOWNLOAD:
    case ICON_UPLOAD: {
        bool down = icon == ICON_DOWNLOAD;
        ImDrawList_AddLine(dl, P(cx, y + (down ? 1.5f : 4.5f) * u), P(cx, y + (down ? 9 : 11) * u), col, t * 1.4f);
        if (down) ImDrawList_AddTriangleFilled(dl, P(x + 4.5f * u, y + 7.5f * u), P(x + 11.5f * u, y + 7.5f * u), P(cx, y + 11.5f * u), col);
        else ImDrawList_AddTriangleFilled(dl, P(x + 4.5f * u, y + 5.5f * u), P(cx, y + 1.5f * u), P(x + 11.5f * u, y + 5.5f * u), col);
        { ImVec2 l[4] = { P(x + 2 * u, y + 11 * u), P(x + 2 * u, y + 14.5f * u), P(x + 14 * u, y + 14.5f * u), P(x + 14 * u, y + 11 * u) }; stroke_path(dl, l, 4, col, t * 1.2f); }
        break;
    }
    case ICON_RECORD:
        body_circle(dl, P(cx, cy), 6.5f * u, col, t);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 3.5f * u, TH(TK_ERROR), 16);
        break;
    case ICON_KEY: { // a keyframe: a diamond
        ImVec2 q[4] = { P(cx, y + 1.5f * u), P(x + 14.5f * u, cy), P(cx, y + 14.5f * u), P(x + 1.5f * u, cy) };
        body_poly(dl, q, 4, col, t);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 1.4f * u, col, 8);
        break;
    }
    case ICON_SNAP: // a magnet
        ImDrawList_PathArcTo(dl, P(cx, y + 7 * u), 5.0f * u, 0.0f, 3.1416f, 14);
        ImDrawList_PathStroke(dl, col, t * 2.6f, 0);
        ImDrawList_AddLine(dl, P(x + 3 * u, y + 7 * u), P(x + 3 * u, y + 2 * u), col, t * 2.6f);
        ImDrawList_AddLine(dl, P(x + 13 * u, y + 7 * u), P(x + 13 * u, y + 2 * u), col, t * 2.6f);
        ImDrawList_AddLine(dl, P(x + 1.6f * u, y + 3 * u), P(x + 4.4f * u, y + 3 * u), TH(TK_ERROR), t * 1.4f);
        ImDrawList_AddLine(dl, P(x + 11.6f * u, y + 3 * u), P(x + 14.4f * u, y + 3 * u), TH(TK_INFO), t * 1.4f);
        break;
    case ICON_BRUSH: // a brush: the handle and its tip
        ImDrawList_AddLine(dl, P(x + 14 * u, y + 2 * u), P(x + 7.5f * u, y + 8.5f * u), col, t * 2.0f);
        { ImVec2 q[4] = { P(x + 6 * u, y + 7 * u), P(x + 9 * u, y + 10 * u), P(x + 5 * u, y + 14.5f * u), P(x + 1.5f * u, y + 14.5f * u) }; body_poly(dl, q, 4, col, t); }
        break;
    case ICON_LAYERS: { // stacked sheets
        for (int i = 2; i >= 0; i--) {
            float yy = y + (4 + 3.5f * (float)i) * u;
            ImVec2 q[4] = { P(cx, yy - 3 * u), P(x + 14.5f * u, yy), P(cx, yy + 3 * u), P(x + 1.5f * u, yy) };
            body_poly(dl, q, 4, col, t);
        }
        break;
    }
    case ICON_WINDOW: // a window: its bar and its buttons
        body_rect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 13.5f * u), 1.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 5.5f * u), P(x + 14.5f * u, y + 5.5f * u), col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 11 * u, y + 4 * u), 0.8f * u, col, 6);
        ImDrawList_AddCircleFilled(dl, P(x + 13 * u, y + 4 * u), 0.8f * u, col, 6);
        break;
    case ICON_BREAKPOINT: { // a hexagon (the debugger's breakpoint)
        ImVec2 q[6];
        for (int i = 0; i < 6; i++) { float a = (float)i * 1.0472f + 0.5236f; q[i] = P(cx + cosf(a) * 6.0f * u, cy + sinf(a) * 6.0f * u); }
        ImDrawList_AddConvexPolyFilled(dl, q, 6, theme_alpha(TH(TK_ERROR), (col >> 24) * 3 / 4));
        ImDrawList_AddPolyline(dl, q, 6, col, t, ImDrawFlags_Closed);
        break;
    }
    case ICON_STEP: // step over: an arc over a dot
        ImDrawList_PathArcTo(dl, P(cx, y + 9 * u), 5.0f * u, 3.4f, 5.9f, 12);
        ImDrawList_PathStroke(dl, col, t * 1.3f, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 14.5f * u, y + 9.5f * u), P(x + 10.5f * u, y + 8.5f * u), P(x + 13.5f * u, y + 5.5f * u), col);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 13 * u), 1.6f * u, col, 10);
        break;
    case ICON_VR: // a headset
        body_rect(dl, P(x + 1 * u, y + 4.5f * u), P(x + 15 * u, y + 11.5f * u), 3.0f * u, col, t, 0);
        ImDrawList_AddCircle(dl, P(x + 5 * u, y + 8 * u), 2.0f * u, col, 12, t);
        ImDrawList_AddCircle(dl, P(x + 11 * u, y + 8 * u), 2.0f * u, col, 12, t);
        break;
    case ICON_PHONE:
        body_rect(dl, P(x + 4 * u, y + 1 * u), P(x + 12 * u, y + 15 * u), 2.0f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 13 * u), P(x + 9 * u, y + 13 * u), col, t * 1.2f);
        break;
    case ICON_NETWORK: // a server and two clients
        body_rect(dl, P(x + 5.5f * u, y + 1.5f * u), P(x + 10.5f * u, y + 5.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 1 * u, y + 10.5f * u), P(x + 6 * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        body_rect(dl, P(x + 10 * u, y + 10.5f * u), P(x + 15 * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        { ImVec2 l[4] = { P(x + 3.5f * u, y + 10.5f * u), P(x + 3.5f * u, y + 8 * u), P(x + 12.5f * u, y + 8 * u), P(x + 12.5f * u, y + 10.5f * u) }; stroke_path(dl, l, 4, col, t); }
        ImDrawList_AddLine(dl, P(cx, y + 5.5f * u), P(cx, y + 8 * u), col, t);
        break;
    case ICON_HEALTH: { // a heart
        ImDrawList_PathArcTo(dl, P(x + 5.2f * u, y + 6 * u), 3.3f * u, 2.6f, 5.6f, 10);
        ImDrawList_PathArcTo(dl, P(x + 10.8f * u, y + 6 * u), 3.3f * u, 3.8f, 6.8f, 10);
        ImDrawList_PathLineTo(dl, P(cx, y + 14 * u));
        ImDrawList_PathFillConcave(dl, g_fill);
        ImDrawList_PathArcTo(dl, P(x + 5.2f * u, y + 6 * u), 3.3f * u, 2.6f, 5.6f, 10);
        ImDrawList_PathArcTo(dl, P(x + 10.8f * u, y + 6 * u), 3.3f * u, 3.8f, 6.8f, 10);
        ImDrawList_PathLineTo(dl, P(cx, y + 14 * u));
        ImDrawList_PathStroke(dl, col, t, ImDrawFlags_Closed);
        break;
    }
    case ICON_FOG: // layers of mist
        for (int i = 0; i < 3; i++) wave(dl, x + (1.5f + (float)(i & 1)) * u, x + (14.5f - (float)(i & 1)) * u, y + (4.5f + 3.5f * (float)i) * u, 0.9f * u, 1.5f, col, t * 1.2f);
        ImDrawList_AddLine(dl, P(x + 4 * u, y + 14.5f * u), P(x + 12 * u, y + 14.5f * u), g_fill, t * 1.4f);
        break;
    case ICON_DECAL: // a sticker on a surface, its corner peeling
        { ImVec2 q[5] = { P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 2 * u), P(x + 14 * u, y + 10 * u), P(x + 10 * u, y + 14 * u), P(x + 2 * u, y + 14 * u) }; body_poly(dl, q, 5, col, t); }
        ImDrawList_AddTriangleFilled(dl, P(x + 14 * u, y + 10 * u), P(x + 10 * u, y + 14 * u), P(x + 10 * u, y + 10 * u), col);
        break;
    case ICON_VOLUME: { // a box drawn dashed: a volume in the level
        ImVec2 c4[4] = { P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), P(x + 2 * u, y + 14 * u) };
        ImDrawList_AddRectFilled(dl, c4[0], c4[2], g_fill, 1.0f * u, 0);
        for (int e = 0; e < 4; e++) {
            ImVec2 a = c4[e], b = c4[(e + 1) & 3];
            for (int k = 0; k < 3; k++) {
                float t0 = (float)k / 3.0f, t1 = t0 + 0.2f;
                ImDrawList_AddLine(dl, P(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), P(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), col, t);
            }
        }
        break;
    }
    case ICON_SPLINE: // a curve through points
        ImDrawList_AddBezierCubic(dl, P(x + 2 * u, y + 13 * u), P(x + 4 * u, y + 2 * u), P(x + 12 * u, y + 14 * u), P(x + 14 * u, y + 3 * u), col, t * 1.2f, 16);
        body_circle(dl, P(x + 2 * u, y + 13 * u), 1.6f * u, col, t);
        body_circle(dl, P(cx, cy), 1.6f * u, col, t);
        body_circle(dl, P(x + 14 * u, y + 3 * u), 1.6f * u, col, t);
        break;
    case ICON_GROUP: // a dashed box around two
        body_rect(dl, P(x + 3.5f * u, y + 3.5f * u), P(x + 8.5f * u, y + 8.5f * u), 0.8f * u, col, t, 0);
        body_rect(dl, P(x + 7.5f * u, y + 7.5f * u), P(x + 12.5f * u, y + 12.5f * u), 0.8f * u, col, t, 0);
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;
            float px = (i & 1) ? x + 15 * u : x + 1 * u, py = (i & 2) ? y + 15 * u : y + 1 * u;
            ImDrawList_AddLine(dl, P(px, py), P(px + sx * 3 * u, py), col, t), ImDrawList_AddLine(dl, P(px, py), P(px, py + sy * 3 * u), col, t);
        }
        break;
    case ICON_LANGUAGE: // A and a speech
        body_rect(dl, P(x + 1 * u, y + 1.5f * u), P(x + 15 * u, y + 12 * u), 2.0f * u, col, t, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 4 * u, y + 11.8f * u), P(x + 7 * u, y + 11.8f * u), P(x + 3.5f * u, y + 15 * u), col);
        { ImVec2 a[3] = { P(x + 5 * u, y + 10 * u), P(cx, y + 3.5f * u), P(x + 11 * u, y + 10 * u) }; stroke_path(dl, a, 3, col, t * 1.2f); }
        ImDrawList_AddLine(dl, P(x + 6.3f * u, y + 7.8f * u), P(x + 9.7f * u, y + 7.8f * u), col, t);
        break;
    case ICON_MEASURE: // a ruler
        { ImVec2 q[4] = { P(x + 1 * u, y + 11 * u), P(x + 11 * u, y + 1 * u), P(x + 15 * u, y + 5 * u), P(x + 5 * u, y + 15 * u) }; body_poly(dl, q, 4, col, t); }
        for (int i = 1; i < 5; i++) {
            float k = (float)i / 5.0f;
            ImVec2 a = P(x + (1 + 10 * k) * u, y + (11 - 10 * k) * u);
            ImDrawList_AddLine(dl, a, P(a.x + (i & 1 ? 1.5f : 2.6f) * u, a.y + (i & 1 ? 1.5f : 2.6f) * u), col, t);
        }
        break;
    case ICON_CROWD: // three heads
        for (int i = 0; i < 3; i++) {
            float xx = x + (3.5f + 4.5f * (float)i) * u, yy = y + (i == 1 ? 4.5f : 6.5f) * u;
            body_circle(dl, P(xx, yy), 2.0f * u, col, t);
            ImDrawList_PathArcTo(dl, P(xx, yy + 7 * u), 3.0f * u, 3.1416f, 6.2832f, 10);
            ImDrawList_PathStroke(dl, col, t, 0);
        }
        break;
    case ICON_NAVMESH: { // tiles on the ground and a path over them
        ImVec2 q[4] = { P(x + 4 * u, y + 3 * u), P(x + 15 * u, y + 3 * u), P(x + 12 * u, y + 13 * u), P(x + 1 * u, y + 13 * u) };
        body_poly(dl, q, 4, col, t);
        ImDrawList_AddLine(dl, P(x + 9.5f * u, y + 3 * u), P(x + 6.5f * u, y + 13 * u), col, t * 0.7f);
        ImDrawList_AddLine(dl, P(x + 2.5f * u, y + 8 * u), P(x + 13.5f * u, y + 8 * u), col, t * 0.7f);
        ImDrawList_AddBezierCubic(dl, P(x + 3.5f * u, y + 11.5f * u), P(x + 6 * u, y + 6 * u), P(x + 10 * u, y + 10 * u), P(x + 12.5f * u, y + 4.5f * u), TH(TK_OK), t * 1.2f, 10);
        break;
    }
    // --- states ---
    case ICON_WARNING: {
        ImVec2 q[3] = { P(cx, y + 1.5f * u), P(x + 15 * u, y + 14 * u), P(x + 1 * u, y + 14 * u) };
        ImDrawList_AddConvexPolyFilled(dl, q, 3, theme_alpha(TH(TK_WARN), (col >> 24) / 4));
        ImDrawList_AddPolyline(dl, q, 3, theme_alpha(TH(TK_WARN), col >> 24), t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, P(cx, y + 6 * u), P(cx, y + 9.5f * u), col, t * 1.4f);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 11.8f * u), 0.95f * u, col, 8);
        break;
    }
    case ICON_ERROR:
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 6.5f * u, theme_alpha(TH(TK_ERROR), (col >> 24) / 4), 0);
        ImDrawList_AddCircle(dl, P(cx, cy), 6.5f * u, theme_alpha(TH(TK_ERROR), col >> 24), 0, t);
        ImDrawList_AddLine(dl, P(x + 5.5f * u, y + 5.5f * u), P(x + 10.5f * u, y + 10.5f * u), col, t * 1.4f);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 5.5f * u), P(x + 5.5f * u, y + 10.5f * u), col, t * 1.4f);
        break;
    case ICON_UNSAVED: // a sheet with the dot of a change
        { ImVec2 pts[5] = { P(x + 3 * u, y + 1 * u), P(x + 10 * u, y + 1 * u), P(x + 13 * u, y + 4 * u), P(x + 13 * u, y + 15 * u), P(x + 3 * u, y + 15 * u) }; body_poly(dl, pts, 5, col, t); }
        ImDrawList_AddCircleFilled(dl, P(cx, y + 9 * u), 2.4f * u, theme_alpha(TH(TK_ACCENT2), col >> 24), 12);
        break;
    case ICON_OK:
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 6.5f * u, theme_alpha(TH(TK_OK), (col >> 24) / 4), 0);
        ImDrawList_AddCircle(dl, P(cx, cy), 6.5f * u, theme_alpha(TH(TK_OK), col >> 24), 0, t);
        { ImVec2 pts[3] = { P(x + 4.5f * u, y + 8.3f * u), P(x + 7 * u, y + 10.8f * u), P(x + 11.5f * u, y + 5.5f * u) }; stroke_path(dl, pts, 3, col, t * 1.5f); }
        break;
    // --- the nodes' families ---
    case ICON_PURE: { // a diamond: no execution, only a value
        ImVec2 q[4] = { P(cx, y + 2.5f * u), P(x + 13.5f * u, cy), P(cx, y + 13.5f * u), P(x + 2.5f * u, cy) };
        body_poly(dl, q, 4, col, t * 1.2f);
        break;
    }
    case ICON_FLOW: // a branch: one way in, two out
        ImDrawList_AddLine(dl, P(x + 1.5f * u, cy), P(x + 6 * u, cy), col, t * 1.3f);
        ImDrawList_AddBezierCubic(dl, P(x + 6 * u, cy), P(x + 9 * u, cy), P(x + 9 * u, y + 4 * u), P(x + 12 * u, y + 4 * u), col, t * 1.3f, 10);
        ImDrawList_AddBezierCubic(dl, P(x + 6 * u, cy), P(x + 9 * u, cy), P(x + 9 * u, y + 12 * u), P(x + 12 * u, y + 12 * u), col, t * 1.3f, 10);
        ImDrawList_AddTriangleFilled(dl, P(x + 15 * u, y + 4 * u), P(x + 12 * u, y + 2 * u), P(x + 12 * u, y + 6 * u), col);
        ImDrawList_AddTriangleFilled(dl, P(x + 15 * u, y + 12 * u), P(x + 12 * u, y + 10 * u), P(x + 12 * u, y + 14 * u), col);
        break;
    case ICON_CAST: // an arrow going into a class's box
        body_rect(dl, P(x + 7.5f * u, y + 3 * u), P(x + 14.5f * u, y + 13 * u), 1.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, cy), P(x + 9 * u, cy), col, t * 1.4f);
        ImDrawList_AddTriangleFilled(dl, P(x + 11.5f * u, cy), P(x + 8.5f * u, cy - 2.5f * u), P(x + 8.5f * u, cy + 2.5f * u), col);
        break;
    // --- (Q26) the target platforms and the frame's readout ---
    case ICON_MONITOR: // a PC: a screen on its stand
        body_rect(dl, P(x + 1.5f * u, y + 2 * u), P(x + 14.5f * u, y + 11 * u), 1.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(cx, y + 11 * u), P(cx, y + 13.5f * u), col, t * 1.2f);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 14 * u), P(x + 11 * u, y + 14 * u), col, t * 1.2f);
        break;
    case ICON_ANDROID: { // the Android robot's head: a dome, two antennae, two eyes
        ImDrawList_PathArcTo(dl, P(cx, y + 11 * u), 6.0f * u, 3.14159f, 6.28318f, 16);
        ImDrawList_PathFillConvex(dl, g_fill);
        ImDrawList_PathArcTo(dl, P(cx, y + 11 * u), 6.0f * u, 3.14159f, 6.28318f, 16);
        ImDrawList_PathLineTo(dl, P(x + 2 * u, y + 11 * u));
        ImDrawList_PathStroke(dl, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, P(x + 4.5f * u, y + 6.2f * u), P(x + 3 * u, y + 3.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 11.5f * u, y + 6.2f * u), P(x + 13 * u, y + 3.5f * u), col, t);
        ImDrawList_AddCircleFilled(dl, P(x + 5.8f * u, y + 8.6f * u), 0.9f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 10.2f * u, y + 8.6f * u), 0.9f * u, col, 8);
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 13 * u), P(x + 14 * u, y + 13 * u), col, t);
        break;
    }
    case ICON_SIGNAL: // four bars rising: the frame rate's readout
        for (int i = 0; i < 4; i++) {
            float bx = x + (2.0f + 3.4f * (float)i) * u, top = y + (11.5f - 3.0f * (float)i) * u;
            body_rect(dl, P(bx, top), P(bx + 2.2f * u, y + 14 * u), 0.6f * u, col, t * 0.8f, 0);
        }
        break;
    default: return false;
    }
    return true;
}

// --- Families (theme tokens) ---

int ui_icon_family_token(UiIcon icon) {
    switch (icon) {
    case ICON_FOLDER: case ICON_OPEN: case ICON_CONTENT: return TK_ASSET_FOLDER;
    case ICON_CUBE: case ICON_SPHERE: case ICON_CYLINDER: case ICON_CONE: case ICON_PLANE: case ICON_STATIC_MESH: case ICON_RASSET:
    case ICON_SKELETAL_MESH: case ICON_LANDSCAPE: case ICON_FOLIAGE: case ICON_GROOM: case ICON_CLOTH: case ICON_VEHICLE:
    case ICON_PHYSICS_ASSET: case ICON_CAPSULE: case ICON_SPLINE: case ICON_DECAL: case ICON_WATER: return TK_ASSET_MESH;
    case ICON_MATERIAL: case ICON_MATERIAL_INSTANCE: case ICON_VM_TEXTURE: return TK_ASSET_MATERIAL;
    case ICON_IMAGE: case ICON_TEXTURE: return TK_ASSET_TEXTURE;
    case ICON_BLUEPRINT: case ICON_CHARACTER: case ICON_PAWN: case ICON_GAMEMODE: case ICON_INTERFACE: case ICON_FUNCTION:
    case ICON_EVENT: case ICON_VARIABLE: case ICON_COMPONENT: case ICON_GRAPH: case ICON_MACRO_LIB: case ICON_SCRIPT: case ICON_BEHAVIOR_TREE:
    case ICON_BRAIN: case ICON_PURE: case ICON_MACRO: case ICON_FLOW: case ICON_CAST: case ICON_ABILITY: case ICON_CROWD: case ICON_NAVMESH:
        return TK_ASSET_BLUEPRINT;
    case ICON_ANIMATION: case ICON_MONTAGE: case ICON_BLEND_SPACE: case ICON_ANIM_BP: case ICON_SKELETON: case ICON_IK_RIG: case ICON_KEY:
    case ICON_SEQUENCE: case ICON_CURVE: case ICON_RECORD: return TK_ASSET_ANIMATION;
    case ICON_SOUND: case ICON_SOUND_WAVE: case ICON_SOUND_CUE: case ICON_ATTENUATION: case ICON_SOUND_CLASS: case ICON_SOUND_MIX:
    case ICON_CONCURRENCY: case ICON_SUBMIX: case ICON_METASOUND: return TK_ASSET_AUDIO;
    case ICON_PARTICLES: case ICON_FOG: case ICON_SKY: case ICON_SUN: case ICON_BULB: case ICON_SPOT: case ICON_RECT: case ICON_BAKE:
    case ICON_EXPOSURE: case ICON_VOLUME: return TK_ASSET_FX;
    case ICON_WIDGET: case ICON_WINDOW: case ICON_PANEL: case ICON_THEME: case ICON_HEALTH: case ICON_PHONE: case ICON_VR: case ICON_MONITOR:
    case ICON_ANDROID:
    case ICON_GAMEPAD: case ICON_INPUT_ACTION: case ICON_INPUT_CONTEXT: case ICON_KEYBOARD: return TK_ASSET_UI;
    case ICON_STRUCT: case ICON_ENUM: case ICON_DATA: case ICON_DATA_TABLE: case ICON_QUEST: case ICON_DIALOGUE: case ICON_ITEM: case ICON_TAG:
    case ICON_FILE: case ICON_LOG: case ICON_SLIDERS: case ICON_STATS: case ICON_SAVE: case ICON_UNSAVED: return TK_ASSET_DATA;
    case ICON_LEVEL: case ICON_ACTOR: case ICON_PLAYER: case ICON_CAMERA: case ICON_LAYERS: case ICON_GROUP: case ICON_GLOBE: return TK_ASSET_LEVEL;
    case ICON_PLAY: case ICON_PAUSE: case ICON_STOP: case ICON_SIMULATE: case ICON_COMPILE: case ICON_BUILD: case ICON_PACKAGE:
    case ICON_PROJECT: return TK_ACCENT2;
    case ICON_WARNING: return TK_WARN;
    case ICON_ERROR: case ICON_BUG: case ICON_BREAKPOINT: case ICON_DELETE: return TK_ERROR;
    case ICON_OK: case ICON_CHECK: return TK_OK;
    default: return TK_ACCENT;
    }
}

uint32_t ui_icon_family_color(UiIcon icon) { return TH(ui_icon_family_token(icon)); }

static const char* const g_icon_names[] = {
    "FOLDER", "CUBE", "SPHERE", "CYLINDER", "CONE", "PLANE", "ACTOR", "SUN", "BULB", "SPOT", "RECT", "SKY", "CAMERA",
    "PLAYER", "FILE", "IMAGE", "LEVEL", "EYE", "EYE_CLOSED", "SEARCH", "SAVE", "PLUS", "IMPORT", "BACK", "FORWARD",
    "SETTINGS", "SELECT", "MOVE", "ROTATE", "SCALE", "MENU", "GRID", "ANGLE", "LOG", "OPEN", "VM_LIT", "VM_UNLIT",
    "VM_WIREFRAME", "VM_LIGHTING", "VM_NORMALS", "VM_SOLID", "VM_RANDOM", "VM_TEXTURE", "VM_ROUGHNESS", "VM_METALLIC",
    "VM_PATH_TRACING", "VM_GI", "VM_LOD", "VM_TRIANGLES", "VM_MOBILITY", "VM_SURFACE_CACHE", "VM_GI_SOURCE",
    "VM_DENSITY", "VM_LIGHT_COMPLEXITY", "VM_SHADER_COST", "VM_OBJECT_COST", "PERSPECTIVE", "NEW", "UNDO", "REDO",
    "DUPLICATE", "DELETE", "RENAME", "FOCUS", "FLOOR", "BAKE", "STATS", "FULLSCREEN", "TIMER", "EXPOSURE", "GAMEPAD",
    "GLOBE", "EXIT", "INFO", "HELP", "BUG", "UPDATE", "PANEL", "CHEVRON", "BLUEPRINT", "CHARACTER", "PAWN",
    "GAMEMODE", "STRUCT", "ENUM", "INTERFACE", "DATA", "FUNCTION", "EVENT", "VARIABLE", "COMPONENT", "ARROW",
    "CAPSULE", "MOVEMENT", "SLIDERS", "CHECK", "CROSS", "GRAPH", "SOUND", "MATERIAL", "MATERIAL_INSTANCE", "TEXTURE",
    "STATIC_MESH", "SKELETAL_MESH", "SKELETON", "ANIMATION", "MONTAGE", "BLEND_SPACE", "ANIM_BP", "BEHAVIOR_TREE",
    "BRAIN", "INPUT_ACTION", "INPUT_CONTEXT", "MACRO_LIB", "SEQUENCE", "SOUND_WAVE", "SOUND_CUE", "ATTENUATION",
    "SOUND_CLASS", "SOUND_MIX", "CONCURRENCY", "SUBMIX", "METASOUND", "SCRIPT", "PARTICLES", "IK_RIG",
    "PHYSICS_ASSET", "VEHICLE", "DIALOGUE", "QUEST", "WIDGET", "DATA_TABLE", "CURVE", "THEME", "PLUGIN", "LANDSCAPE",
    "FOLIAGE", "GROOM", "CLOTH", "WATER", "RASSET", "ITEM", "ABILITY", "PLAY", "PAUSE", "STOP", "SIMULATE", "COMPILE",
    "BUILD", "PACKAGE", "PROJECT", "CONTENT", "OUTLINER", "DETAILS", "ASSISTANT", "GIT", "LOCK", "UNLOCK", "COPY",
    "PASTE", "CUT", "LINK", "TAG", "FILTER", "SORT", "REFRESH", "DOWNLOAD", "UPLOAD", "RECORD", "KEY", "SNAP",
    "BRUSH", "LAYERS", "WINDOW", "KEYBOARD", "COMMENT", "BREAKPOINT", "STEP", "VR", "PHONE", "NETWORK", "HEALTH",
    "FOG", "DECAL", "VOLUME", "SPLINE", "GROUP", "LANGUAGE", "MEASURE", "CROWD", "NAVMESH", "WARNING", "ERROR",
    "UNSAVED", "OK", "PURE", "MACRO", "FLOW", "CAST", "MONITOR", "ANDROID", "SIGNAL",
};
_Static_assert(sizeof g_icon_names / sizeof g_icon_names[0] == ICON_COUNT, "an icon without its name");
const char* ui_icon_name(UiIcon icon) { return icon >= 0 && icon < ICON_COUNT ? g_icon_names[icon] : ""; }

// The asset kinds (content.h AssetKind), each its icon and its family
UiIcon ui_asset_icon(int k) {
    switch (k) {
    case ASSET_FOLDER: return ICON_FOLDER;
    case ASSET_BASIC_SHAPE: return ICON_CUBE;
    case ASSET_RASSET: return ICON_RASSET;
    case ASSET_LEVEL: return ICON_LEVEL;
    case ASSET_TEXTURE_SOURCE: return ICON_TEXTURE;
    case ASSET_MESH_SOURCE: return ICON_STATIC_MESH;
    case ASSET_SOUND_SOURCE: return ICON_SOUND_WAVE;
    case ASSET_BLUEPRINT: return ICON_BLUEPRINT;
    case ASSET_BP_STRUCT: return ICON_STRUCT;
    case ASSET_BP_ENUM: return ICON_ENUM;
    case ASSET_BP_INTERFACE: return ICON_INTERFACE;
    case ASSET_DATA_ASSET: return ICON_DATA;
    case ASSET_MATERIAL: return ICON_MATERIAL;
    case ASSET_MATERIAL_INSTANCE: return ICON_MATERIAL_INSTANCE;
    case ASSET_SKELETAL_MESH: return ICON_SKELETAL_MESH;
    case ASSET_ANIMATION: return ICON_ANIMATION;
    case ASSET_ANIM_MONTAGE: return ICON_MONTAGE;
    case ASSET_STATIC_MESH: return ICON_STATIC_MESH;
    case ASSET_BEHAVIOR_TREE: return ICON_BEHAVIOR_TREE;
    case ASSET_BRAIN: return ICON_BRAIN;
    case ASSET_INPUT_ACTION: return ICON_INPUT_ACTION;
    case ASSET_INPUT_CONTEXT: return ICON_INPUT_CONTEXT;
    case ASSET_MACRO_LIB: return ICON_MACRO_LIB;
    case ASSET_LEVEL_SEQUENCE: return ICON_SEQUENCE;
    case ASSET_SOUND_WAVE: return ICON_SOUND_WAVE;
    case ASSET_SOUND_CUE: return ICON_SOUND_CUE;
    case ASSET_SOUND_ATTENUATION: return ICON_ATTENUATION;
    case ASSET_SOUND_CLASS: return ICON_SOUND_CLASS;
    case ASSET_SOUND_MIX: return ICON_SOUND_MIX;
    case ASSET_SOUND_CONCURRENCY: return ICON_CONCURRENCY;
    case ASSET_SUBMIX: return ICON_SUBMIX;
    case ASSET_SCRIPT: return ICON_SCRIPT;
    case ASSET_PARTICLE_SYSTEM: return ICON_PARTICLES;
    case ASSET_IK_RIG: return ICON_IK_RIG;
    case ASSET_PHYSICS_ASSET: return ICON_PHYSICS_ASSET;
    case ASSET_PHYS_MATERIAL: return ICON_PHYSICS_ASSET; // (Q8: the physics' icon, its own color)
    case ASSET_CAMERA_SHAKE: return ICON_CAMERA; // (Q12)
    case ASSET_PCG_GRAPH: return ICON_FOLIAGE; // (Q15)
    case ASSET_VEHICLE: return ICON_VEHICLE;
    case ASSET_DIALOGUE: return ICON_DIALOGUE;
    case ASSET_QUEST: return ICON_QUEST;
    case ASSET_METASOUND: return ICON_METASOUND;
    default: return ICON_FILE;
    }
}

int ui_asset_family_token(int k) { return k == ASSET_OTHER ? TK_ASSET_DATA : ui_icon_family_token(ui_asset_icon(k)); }

void ui_icon(ImDrawList* dl, UiIcon icon, float x, float y, float s, uint32_t col) {
    float cx = x + s * 0.5f, cy = y + s * 0.5f, u = s / 16.0f; // a 16-unit grid
    float t = fmaxf(1.0f, 1.5f * s / 16.0f);                     // the stroke: 1.5 px at 16, following the scale
    unsigned fa = (unsigned)(51u * (col >> 24) / 255u);         // (20%: dimmer with a disabled stroke)
    g_fill = theme_alpha(ui_icon_family_color(icon), fa);
    uint32_t dim = g_fill;
    switch (icon) {
    case ICON_FOLDER:
        body_rect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 7 * u, y + 5.5f * u), 1.0f * u, col, t, ImDrawFlags_RoundCornersTop);
        body_rect(dl, P(x + 1.5f * u, y + 5 * u), P(x + 14.5f * u, y + 13.5f * u), 1.5f * u, col, t, 0);
        break;
    case ICON_CUBE: {
        ImVec2 a = P(cx, y + 2 * u), b = P(x + 14 * u, y + 5 * u), c = P(cx, y + 8 * u), d = P(x + 2 * u, y + 5 * u);
        ImVec2 e = P(x + 14 * u, y + 11 * u), f = P(cx, y + 14 * u), g = P(x + 2 * u, y + 11 * u);
        ImDrawList_AddQuadFilled(dl, a, b, c, d, dim);
        ImDrawList_AddQuad(dl, a, b, c, d, col, t);
        ImDrawList_AddLine(dl, b, e, col, t); ImDrawList_AddLine(dl, e, f, col, t);
        ImDrawList_AddLine(dl, f, g, col, t); ImDrawList_AddLine(dl, g, d, col, t);
        ImDrawList_AddLine(dl, c, f, col, t);
        break;
    }
    case ICON_SPHERE:
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 6.5f * u, dim, 24);
        ImDrawList_AddCircle(dl, P(cx, cy), 6.5f * u, col, 24, t);
        ImDrawList_AddEllipse(dl, P(cx, cy), P(6.5f * u, 2.2f * u), col, 0, 24, t);
        break;
    case ICON_CYLINDER:
        ImDrawList_AddRectFilled(dl, P(x + 3 * u, y + 4 * u), P(x + 13 * u, y + 12 * u), dim, 0, 0);
        ImDrawList_AddEllipse(dl, P(cx, y + 4 * u), P(5 * u, 1.8f * u), col, 0, 20, t);
        ImDrawList_AddEllipse(dl, P(cx, y + 12 * u), P(5 * u, 1.8f * u), col, 0, 20, t);
        ImDrawList_AddLine(dl, P(x + 3 * u, y + 4 * u), P(x + 3 * u, y + 12 * u), col, t);
        ImDrawList_AddLine(dl, P(x + 13 * u, y + 4 * u), P(x + 13 * u, y + 12 * u), col, t);
        break;
    case ICON_CONE:
        ImDrawList_AddTriangleFilled(dl, P(cx, y + 2 * u), P(x + 13 * u, y + 12 * u), P(x + 3 * u, y + 12 * u), dim);
        ImDrawList_AddEllipse(dl, P(cx, y + 12 * u), P(5 * u, 1.8f * u), col, 0, 20, t);
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(x + 3 * u, y + 12 * u), col, t);
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(x + 13 * u, y + 12 * u), col, t);
        break;
    case ICON_PLANE: {
        ImVec2 a = P(x + 5 * u, y + 5 * u), b = P(x + 15 * u, y + 5 * u), c = P(x + 11 * u, y + 11 * u), d = P(x + 1 * u, y + 11 * u);
        ImDrawList_AddQuadFilled(dl, a, b, c, d, dim);
        ImDrawList_AddQuad(dl, a, b, c, d, col, t);
        break;
    }
    case ICON_ACTOR:
        ImDrawList_AddLine(dl, P(cx, cy), P(x + 14 * u, cy), TH(TK_RED), t * 1.4f);
        ImDrawList_AddLine(dl, P(cx, cy), P(cx, y + 2 * u), TH(TK_BLUE), t * 1.4f);
        ImDrawList_AddLine(dl, P(cx, cy), P(x + 3 * u, y + 13 * u), TH(TK_GREEN), t * 1.4f);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 1.6f * u, col, 10);
        break;
    case ICON_SUN:
        body_circle(dl, P(cx, cy), 3.5f * u, col, t);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f, c = cosf(a), sn = sinf(a);
            ImDrawList_AddLine(dl, P(cx + c * 5.2f * u, cy + sn * 5.2f * u), P(cx + c * 7.2f * u, cy + sn * 7.2f * u), col, t);
        }
        break;
    case ICON_BULB:
        body_circle(dl, P(cx, y + 6.5f * u), 4.8f * u, col, t);
        body_rect(dl, P(x + 6 * u, y + 10.5f * u), P(x + 10 * u, y + 13 * u), 0.5f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 6.8f * u, y + 14.6f * u), P(x + 9.2f * u, y + 14.6f * u), col, t);
        break;
    case ICON_SPOT:
        ImDrawList_AddTriangleFilled(dl, P(cx, y + 2 * u), P(x + 14 * u, y + 13 * u), P(x + 2 * u, y + 13 * u), dim);
        body_rect(dl, P(x + 5.5f * u, y + 1 * u), P(x + 10.5f * u, y + 5 * u), 1.0f * u, col, t, 0);
        ImDrawList_AddEllipse(dl, P(cx, y + 13 * u), P(6 * u, 1.8f * u), col, 0, 20, t);
        break;
    case ICON_RECT:
        body_rect(dl, P(x + 2 * u, y + 4 * u), P(x + 14 * u, y + 10 * u), 1.0f * u, col, t, 0);
        for (int i = 0; i < 3; i++)
            ImDrawList_AddLine(dl, P(x + (4 + 4 * i) * u, y + 11.5f * u), P(x + (4 + 4 * i) * u, y + 14.5f * u), col, t);
        break;
    case ICON_SKY:
        { // a cloud: one outline around three lobes
            ImDrawList_PathArcTo(dl, P(x + 5.5f * u, y + 10 * u), 3.3f * u, 1.5708f, 4.0f, 10);
            ImDrawList_PathArcTo(dl, P(x + 10 * u, y + 8 * u), 4.3f * u, 3.6f, 6.0f, 12);
            ImDrawList_PathArcTo(dl, P(x + 12.2f * u, y + 11 * u), 2.4f * u, -0.9f, 1.5708f, 8);
            ImDrawList_PathFillConcave(dl, g_fill);
            ImDrawList_PathArcTo(dl, P(x + 5.5f * u, y + 10 * u), 3.3f * u, 1.5708f, 4.0f, 10);
            ImDrawList_PathArcTo(dl, P(x + 10 * u, y + 8 * u), 4.3f * u, 3.6f, 6.0f, 12);
            ImDrawList_PathArcTo(dl, P(x + 12.2f * u, y + 11 * u), 2.4f * u, -0.9f, 1.5708f, 8);
            ImDrawList_PathStroke(dl, col, t, ImDrawFlags_Closed);
        }
        break;
    case ICON_CAMERA:
        body_rect(dl, P(x + 1.5f * u, y + 6 * u), P(x + 11 * u, y + 13.5f * u), 1.5f * u, col, t, 0);
        body_tri(dl, P(x + 11 * u, y + 9.75f * u), P(x + 14.5f * u, y + 6.5f * u), P(x + 14.5f * u, y + 13 * u), col, t);
        body_circle(dl, P(x + 4 * u, y + 3.6f * u), 2.1f * u, col, t);
        body_circle(dl, P(x + 8.5f * u, y + 3.6f * u), 2.1f * u, col, t);
        break;
    case ICON_PLAYER:
        body_circle(dl, P(cx, y + 4 * u), 2.6f * u, col, t);
        body_rect(dl, P(x + 5 * u, y + 8 * u), P(x + 11 * u, y + 14.5f * u), 2.0f * u, col, t, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddLine(dl, P(x + 2.5f * u, y + 14.8f * u), P(x + 13.5f * u, y + 14.8f * u), col, t); // (the start's floor mark)
        break;
    case ICON_FILE:
    case ICON_IMAGE:
    case ICON_LEVEL: {
        ImVec2 pts[5] = { P(x + 3 * u, y + 1 * u), P(x + 10 * u, y + 1 * u), P(x + 13 * u, y + 4 * u),
                          P(x + 13 * u, y + 15 * u), P(x + 3 * u, y + 15 * u) };
        body_poly(dl, pts, 5, col, t);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 1 * u), P(x + 10 * u, y + 4 * u), col, t);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 4 * u), P(x + 13 * u, y + 4 * u), col, t);
        if (icon == ICON_IMAGE) {
            ImDrawList_AddTriangleFilled(dl, P(x + 4.5f * u, y + 13 * u), P(x + 8 * u, y + 8 * u), P(x + 11.5f * u, y + 13 * u), col);
            ImDrawList_AddCircleFilled(dl, P(x + 10 * u, y + 5.5f * u), 1.3f * u, col, 8);
        } else if (icon == ICON_LEVEL) {
            ImDrawList_AddTriangleFilled(dl, P(x + 4.5f * u, y + 13 * u), P(x + 7.5f * u, y + 7 * u), P(x + 10.5f * u, y + 13 * u), col);
            ImDrawList_AddTriangleFilled(dl, P(x + 7.5f * u, y + 13 * u), P(x + 10 * u, y + 9 * u), P(x + 12 * u, y + 13 * u), dim);
        }
        break;
    }
    case ICON_EYE:
    case ICON_EYE_CLOSED:
        ImDrawList_AddEllipse(dl, P(cx, cy), P(6.5f * u, 3.8f * u), col, 0, 20, t);
        if (icon == ICON_EYE) ImDrawList_AddCircleFilled(dl, P(cx, cy), 2.2f * u, col, 12);
        else ImDrawList_AddLine(dl, P(x + 2 * u, y + 13 * u), P(x + 14 * u, y + 3 * u), col, t);
        break;
    case ICON_SEARCH:
        ImDrawList_AddCircle(dl, P(x + 6.5f * u, y + 6.5f * u), 4.2f * u, col, 16, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 9.5f * u, y + 9.5f * u), P(x + 14 * u, y + 14 * u), col, t * 1.6f);
        break;
    case ICON_SAVE:
        body_rect(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), 1.5f * u, col, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 5 * u, y + 2 * u), P(x + 11 * u, y + 6 * u), col, 0, 0);
        ImDrawList_AddRect(dl, P(x + 4.5f * u, y + 9 * u), P(x + 11.5f * u, y + 14 * u), col, 0.5f * u, t, 0);
        break;
    case ICON_PLUS:
        ImDrawList_AddLine(dl, P(cx, y + 3 * u), P(cx, y + 13 * u), col, t * 1.6f);
        ImDrawList_AddLine(dl, P(x + 3 * u, cy), P(x + 13 * u, cy), col, t * 1.6f);
        break;
    case ICON_IMPORT:
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(cx, y + 10 * u), col, t * 1.5f);
        ImDrawList_AddTriangleFilled(dl, P(x + 4.5f * u, y + 8 * u), P(x + 11.5f * u, y + 8 * u), P(cx, y + 12 * u), col);
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 14 * u), P(x + 14 * u, y + 14 * u), col, t * 1.5f);
        break;
    case ICON_BACK:
    case ICON_FORWARD: {
        float d = icon == ICON_BACK ? 1.0f : -1.0f; // the tip points the way it goes
        ImDrawList_AddLine(dl, P(cx + d * 3 * u, y + 3 * u), P(cx - d * 2 * u, cy), col, t * 1.6f);
        ImDrawList_AddLine(dl, P(cx - d * 2 * u, cy), P(cx + d * 3 * u, y + 13 * u), col, t * 1.6f);
        break;
    }
    case ICON_SETTINGS:
        ImDrawList_AddCircle(dl, P(cx, cy), 4.5f * u, col, 16, t * 1.6f);
        for (int i = 0; i < 6; i++) {
            float a = (float)i * 1.047198f, c = cosf(a), sn = sinf(a);
            ImDrawList_AddLine(dl, P(cx + c * 5 * u, cy + sn * 5 * u), P(cx + c * 7 * u, cy + sn * 7 * u), col, t * 2.0f);
        }
        break;
    case ICON_SELECT: {
        ImVec2 pts[7] = { P(x + 4 * u, y + 2 * u), P(x + 12 * u, y + 10 * u), P(x + 8.5f * u, y + 10.5f * u),
                          P(x + 10.5f * u, y + 14.5f * u), P(x + 9 * u, y + 15 * u), P(x + 7 * u, y + 11.2f * u),
                          P(x + 4 * u, y + 13.5f * u) };
        ImDrawList_AddConcavePolyFilled(dl, pts, 7, g_fill);
        ImDrawList_AddPolyline(dl, pts, 7, col, t, ImDrawFlags_Closed);
        break;
    }
    case ICON_MOVE:
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(cx, y + 14 * u), col, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 2 * u, cy), P(x + 14 * u, cy), col, t * 1.3f);
        ImDrawList_AddTriangleFilled(dl, P(cx, y + 1 * u), P(cx + 2.5f * u, y + 4 * u), P(cx - 2.5f * u, y + 4 * u), col);
        ImDrawList_AddTriangleFilled(dl, P(cx, y + 15 * u), P(cx - 2.5f * u, y + 12 * u), P(cx + 2.5f * u, y + 12 * u), col);
        ImDrawList_AddTriangleFilled(dl, P(x + 1 * u, cy), P(x + 4 * u, cy - 2.5f * u), P(x + 4 * u, cy + 2.5f * u), col);
        ImDrawList_AddTriangleFilled(dl, P(x + 15 * u, cy), P(x + 12 * u, cy + 2.5f * u), P(x + 12 * u, cy - 2.5f * u), col);
        break;
    case ICON_ROTATE: {
        ImVec2 pts[20];
        for (int i = 0; i < 20; i++) {
            float a = 0.6f + (float)i / 19.0f * 4.6f;
            pts[i] = P(cx + cosf(a) * 5.5f * u, cy + sinf(a) * 5.5f * u);
        }
        ImDrawList_AddPolyline(dl, pts, 20, col, t * 1.4f, 0);
        ImVec2 e = pts[19];
        ImDrawList_AddTriangleFilled(dl, P(e.x + 2.5f * u, e.y - 1.0f * u), P(e.x - 2.0f * u, e.y - 2.5f * u), P(e.x - 0.5f * u, e.y + 2.5f * u), col);
        break;
    }
    case ICON_SCALE:
        body_rect(dl, P(x + 2 * u, y + 7 * u), P(x + 9 * u, y + 14 * u), 1.0f * u, col, t * 1.2f, 0);
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 9 * u), P(x + 13.5f * u, y + 2.5f * u), col, t * 1.3f);
        ImDrawList_AddTriangleFilled(dl, P(x + 14.5f * u, y + 1.5f * u), P(x + 14.5f * u, y + 6 * u), P(x + 10 * u, y + 1.5f * u), col);
        break;
    case ICON_MENU:
        for (int i = 0; i < 3; i++) ImDrawList_AddLine(dl, P(x + 3 * u, y + (4 + 4 * i) * u), P(x + 13 * u, y + (4 + 4 * i) * u), col, t * 1.5f);
        break;
    case ICON_GRID:
        for (int i = 0; i < 4; i++) {
            ImDrawList_AddLine(dl, P(x + (2 + 4 * i) * u, y + 2 * u), P(x + (2 + 4 * i) * u, y + 14 * u), col, t);
            ImDrawList_AddLine(dl, P(x + 2 * u, y + (2 + 4 * i) * u), P(x + 14 * u, y + (2 + 4 * i) * u), col, t);
        }
        break;
    case ICON_ANGLE:
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 14 * u), P(x + 14 * u, y + 14 * u), col, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 14 * u), P(x + 11 * u, y + 3 * u), col, t * 1.3f);
        ImDrawList_PathArcTo(dl, P(x + 2 * u, y + 14 * u), 7 * u, -0.87f, 0.0f, 10);
        ImDrawList_PathStroke(dl, col, t, 0);
        break;
    case ICON_LOG:
        body_rect(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), 1.5f * u, col, t, 0);
        for (int i = 0; i < 3; i++) ImDrawList_AddLine(dl, P(x + 4.5f * u, y + (5.5f + 2.5f * i) * u), P(x + 11.5f * u, y + (5.5f + 2.5f * i) * u), col, t);
        break;
    case ICON_OPEN:
        ImDrawList_AddRect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 7 * u, y + 5.5f * u), col, 1.0f * u, t, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddRect(dl, P(x + 1.5f * u, y + 5 * u), P(x + 13 * u, y + 13.5f * u), col, 1.0f * u, t, 0);
        { ImVec2 q[4] = { P(x + 4 * u, y + 8 * u), P(x + 15.2f * u, y + 8 * u), P(x + 13 * u, y + 13.5f * u), P(x + 1.5f * u, y + 13.5f * u) };
          body_poly(dl, q, 4, col, t); }
        break;
    // --- The view modes: a sphere shaded the way each mode shows the scene ---
    case ICON_VM_LIT: // lit: warm light from the upper left, a highlight
        shaded_sphere(dl, cx, cy, 6.5f * u, A(RGBA(120, 62, 22, 255), col), A(RGBA(214, 128, 52, 255), col),
                      A(RGBA(255, 206, 140, 255), col), true, col);
        break;
    case ICON_VM_UNLIT: // the base color alone: flat
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 6.5f * u, A(RGBA(214, 128, 52, 255), col), 28);
        break;
    case ICON_VM_WIREFRAME:
        ImDrawList_AddCircle(dl, P(cx, cy), 6.5f * u, col, 28, t);
        ImDrawList_AddEllipse(dl, P(cx, cy), P(6.5f * u, 2.4f * u), col, 0, 24, t);
        ImDrawList_AddEllipse(dl, P(cx, cy), P(2.4f * u, 6.5f * u), col, 0, 24, t);
        break;
    case ICON_VM_LIGHTING: // the light on a grey material, and where it comes from
        shaded_sphere(dl, cx + 1 * u, cy + 1 * u, 5.5f * u, A(RGBA(52, 52, 52, 255), col), A(RGBA(128, 128, 128, 255), col),
                      A(RGBA(222, 222, 222, 255), col), false, col);
        ImDrawList_AddCircleFilled(dl, P(x + 3 * u, y + 3 * u), 1.8f * u, A(RGBA(255, 220, 120, 255), col), 10);
        break;
    case ICON_VM_NORMALS: { // a normal map's colors: X red, Y green, Z blue
        static const uint32_t rgb[3] = { RGBA(226, 96, 96, 255), RGBA(104, 196, 104, 255), RGBA(104, 128, 236, 255) };
        for (int i = 0; i < 3; i++) wedge(dl, cx, cy, 6.5f * u, -1.5708f + (float)i * 2.0944f, 2.0944f, A(rgb[i], col));
        break;
    }
    case ICON_VM_SOLID: // Blender's studio light: matte grey
        shaded_sphere(dl, cx, cy, 6.5f * u, A(RGBA(88, 88, 88, 255), col), A(RGBA(156, 156, 156, 255), col),
                      A(RGBA(214, 214, 214, 255), col), false, col);
        break;
    case ICON_VM_RANDOM: { // a color per actor
        static const uint32_t hues[4] = { RGBA(230, 120, 90, 255), RGBA(110, 190, 120, 255), RGBA(100, 140, 230, 255),
                                          RGBA(220, 200, 90, 255) };
        for (int i = 0; i < 4; i++) wedge(dl, cx, cy, 6.5f * u, -1.5708f + (float)i * 1.5708f, 1.5708f, A(hues[i], col));
        break;
    }
    case ICON_VM_TEXTURE: // a checker
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                ImDrawList_AddRectFilled(dl, P(x + (2 + 3 * i) * u, y + (2 + 3 * j) * u), P(x + (5 + 3 * i) * u, y + (5 + 3 * j) * u),
                                         ((i + j) & 1) ? dim : col, 0, 0);
        break;
    case ICON_VM_ROUGHNESS: // a sphere half glossy, half rough (flat)
        shaded_sphere(dl, cx, cy, 6.5f * u, A(RGBA(40, 40, 40, 255), col), A(RGBA(110, 110, 110, 255), col),
                      A(RGBA(200, 200, 200, 255), col), true, col);
        wedge(dl, cx, cy, 6.5f * u, -1.5708f, 3.1416f, A(RGBA(150, 150, 150, 255), col));
        break;
    case ICON_VM_METALLIC: // chrome: dark body, bright rim and highlight
        shaded_sphere(dl, cx, cy, 6.5f * u, A(RGBA(44, 50, 60, 255), col), A(RGBA(126, 138, 156, 255), col),
                      A(RGBA(236, 242, 250, 255), col), true, col);
        ImDrawList_AddCircle(dl, P(cx, cy), 6.3f * u, A(RGBA(200, 210, 225, 255), col), 28, t);
        break;
    case ICON_VM_PATH_TRACING: { // a ray bouncing off the ground toward the eye
        ImDrawList_AddLine(dl, P(x + 1 * u, y + 14 * u), P(x + 15 * u, y + 14 * u), dim, t * 1.4f);
        ImVec2 pts[3] = { P(x + 2 * u, y + 3 * u), P(x + 7 * u, y + 13 * u), P(x + 12.5f * u, y + 5 * u) };
        ImDrawList_AddPolyline(dl, pts, 3, col, t * 1.3f, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 14 * u, y + 2.5f * u), P(x + 10.5f * u, y + 4.5f * u), P(x + 13.5f * u, y + 6.5f * u), col);
        ImDrawList_AddCircleFilled(dl, P(x + 7 * u, y + 13 * u), 1.4f * u, col, 10);
        break;
    }
    case ICON_VM_GI: // the light that bounced: a sphere lit from below by the ground
        ImDrawList_AddLine(dl, P(x + 1 * u, y + 15 * u), P(x + 15 * u, y + 15 * u), A(RGBA(110, 200, 140, 255), col), t * 1.6f);
        shaded_sphere(dl, cx, y + 7 * u, 5.5f * u, A(RGBA(40, 60, 70, 255), col), A(RGBA(70, 120, 120, 255), col),
                      A(RGBA(150, 220, 170, 255), col), false, col);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 9.5f * u), 2.6f * u, A(RGBA(150, 220, 170, 160), col), 14);
        break;
    case ICON_VM_LOD: { // rings from near (green, fine) to far (red, coarse)
        static const uint32_t rc[4] = { RGBA(232, 40, 30, 255), RGBA(255, 140, 20, 255), RGBA(180, 216, 30, 255), RGBA(40, 200, 70, 255) };
        for (int i = 0; i < 4; i++) ImDrawList_AddCircleFilled(dl, P(cx, cy), (6.5f - 1.6f * (float)i) * u, A(rc[i], col), 24);
        break;
    }
    case ICON_VM_TRIANGLES: { // a strip of triangles, each its own color
        static const uint32_t tc[4] = { RGBA(230, 120, 90, 255), RGBA(110, 190, 120, 255), RGBA(100, 140, 230, 255), RGBA(220, 200, 90, 255) };
        for (int i = 0; i < 4; i++) {
            float x0 = x + (1.5f + 3.2f * (float)i) * u;
            if (i & 1) ImDrawList_AddTriangleFilled(dl, P(x0, y + 3 * u), P(x0 + 6.4f * u, y + 3 * u), P(x0 + 3.2f * u, y + 13 * u), A(tc[i], col));
            else ImDrawList_AddTriangleFilled(dl, P(x0, y + 13 * u), P(x0 + 3.2f * u, y + 3 * u), P(x0 + 6.4f * u, y + 13 * u), A(tc[i], col));
        }
        break;
    }
    case ICON_VM_MOBILITY: // Static white, Movable blue, moving orange
        ImDrawList_AddCircleFilled(dl, P(x + 4 * u, cy), 2.6f * u, A(RGBA(235, 235, 235, 255), col), 12);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 2.6f * u, A(RGBA(50, 115, 255, 255), col), 12);
        ImDrawList_AddCircleFilled(dl, P(x + 12 * u, cy), 2.6f * u, A(RGBA(255, 115, 15, 255), col), 12);
        break;
    case ICON_VM_SURFACE_CACHE: // a lit card divided in texels
        ImDrawList_AddRectFilled(dl, P(x + 2 * u, y + 2 * u), P(x + 14 * u, y + 14 * u), A(RGBA(214, 150, 80, 255), col), 1.5f * u, 0);
        for (int i = 1; i < 4; i++) {
            ImDrawList_AddLine(dl, P(x + (2 + 3 * i) * u, y + 2 * u), P(x + (2 + 3 * i) * u, y + 14 * u), A(RGBA(90, 50, 20, 255), col), t);
            ImDrawList_AddLine(dl, P(x + 2 * u, y + (2 + 3 * i) * u), P(x + 14 * u, y + (2 + 3 * i) * u), A(RGBA(90, 50, 20, 255), col), t);
        }
        break;
    case ICON_VM_GI_SOURCE: { // the four sources: lightmap, cache, probes, sky
        static const uint32_t gc[4] = { RGBA(50, 216, 76, 255), RGBA(50, 128, 255, 255), RGBA(255, 216, 40, 255), RGBA(240, 50, 40, 255) };
        for (int i = 0; i < 4; i++) wedge(dl, cx, cy, 6.5f * u, -1.5708f + (float)i * 1.5708f, 1.5708f, A(gc[i], col));
        break;
    }
    case ICON_VM_DENSITY: // a texel grid, dense (red) to coarse (blue)
        ImDrawList_AddRectFilled(dl, P(x + 2 * u, y + 2 * u), P(cx, y + 14 * u), A(RGBA(232, 50, 40, 255), col), 0, 0);
        ImDrawList_AddRectFilled(dl, P(cx, y + 2 * u), P(x + 14 * u, y + 14 * u), A(RGBA(50, 90, 250, 255), col), 0, 0);
        for (int i = 0; i < 3; i++) ImDrawList_AddLine(dl, P(x + 2 * u, y + (5 + 3 * i) * u), P(x + 14 * u, y + (5 + 3 * i) * u), dim, t);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 2 * u), P(x + 5 * u, y + 14 * u), dim, t);
        ImDrawList_AddLine(dl, P(x + 11 * u, y + 2 * u), P(x + 11 * u, y + 14 * u), dim, t);
        break;
    case ICON_VM_LIGHT_COMPLEXITY:
    case ICON_VM_OBJECT_COST: // (an object over it: a box)
    case ICON_VM_SHADER_COST: { // a heat bar (the light complexity a bulb on it)
        static const uint32_t hc[4] = { RGBA(10, 60, 255, 255), RGBA(20, 216, 76, 255), RGBA(255, 228, 20, 255), RGBA(255, 30, 20, 255) };
        for (int i = 0; i < 4; i++)
            ImDrawList_AddRectFilled(dl, P(x + (2 + 3 * i) * u, y + (12 - 2.5f * i) * u), P(x + (5 + 3 * i) * u, y + 14 * u), A(hc[i], col), 0, 0);
        if (icon == ICON_VM_LIGHT_COMPLEXITY) ImDrawList_AddCircleFilled(dl, P(x + 4 * u, y + 4.5f * u), 2.2f * u, A(RGBA(255, 220, 120, 255), col), 10);
        else if (icon == ICON_VM_OBJECT_COST) ImDrawList_AddRect(dl, P(x + 2 * u, y + 2 * u), P(x + 7 * u, y + 7 * u), col, 0.0f, t, 0);
        else ImDrawList_AddCircle(dl, P(x + 4 * u, y + 4.5f * u), 2.2f * u, col, 10, t);
        break;
    }
    // --- Commands ---
    case ICON_PERSPECTIVE: { // a box seen in perspective: the far face smaller
        ImVec2 f0 = P(x + 2 * u, y + 5 * u), f1 = P(x + 10 * u, y + 5 * u), f2 = P(x + 10 * u, y + 14 * u), f3 = P(x + 2 * u, y + 14 * u);
        ImVec2 b0 = P(x + 7 * u, y + 2 * u), b1 = P(x + 14 * u, y + 2 * u), b2 = P(x + 14 * u, y + 9 * u);
        ImDrawList_AddQuad(dl, f0, f1, f2, f3, col, t);
        ImDrawList_AddLine(dl, f0, b0, col, t);
        ImDrawList_AddLine(dl, b0, b1, col, t);
        ImDrawList_AddLine(dl, f1, b1, col, t);
        ImDrawList_AddLine(dl, b1, b2, col, t);
        ImDrawList_AddLine(dl, f2, b2, col, t);
        break;
    }
    case ICON_NEW: {
        ImVec2 pts[5] = { P(x + 3 * u, y + 1 * u), P(x + 10 * u, y + 1 * u), P(x + 13 * u, y + 4 * u), P(x + 13 * u, y + 15 * u),
                          P(x + 3 * u, y + 15 * u) };
        ImDrawList_AddPolyline(dl, pts, 5, col, t, ImDrawFlags_Closed);
        ImDrawList_AddLine(dl, P(cx, y + 6 * u), P(cx, y + 12 * u), col, t * 1.4f);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 9 * u), P(x + 11 * u, y + 9 * u), col, t * 1.4f);
        break;
    }
    case ICON_UNDO:
    case ICON_REDO: { // a hook arrow turning back (undo) or forward (redo)
        float d = icon == ICON_UNDO ? 1.0f : -1.0f;
        ImDrawList_PathArcTo(dl, P(cx, y + 9 * u), 4.5f * u, d > 0 ? -1.5708f : 4.7124f, 1.5708f, 14);
        ImDrawList_PathStroke(dl, col, t * 1.4f, 0);
        ImDrawList_AddLine(dl, P(cx, y + 13.5f * u), P(cx - d * 4 * u, y + 13.5f * u), col, t * 1.4f);
        ImDrawList_AddTriangleFilled(dl, P(cx - d * 3.5f * u, y + 4.5f * u), P(cx + d * 0.5f * u, y + 2 * u), P(cx + d * 0.5f * u, y + 7 * u), col);
        break;
    }
    case ICON_DUPLICATE:
        ImDrawList_AddRect(dl, P(x + 2 * u, y + 2 * u), P(x + 10 * u, y + 10 * u), col, 1.5f * u, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 6 * u, y + 6 * u), P(x + 14 * u, y + 14 * u), dim, 1.5f * u, 0);
        ImDrawList_AddRect(dl, P(x + 6 * u, y + 6 * u), P(x + 14 * u, y + 14 * u), col, 1.5f * u, t, 0);
        break;
    case ICON_DELETE: // a bin
        ImDrawList_AddLine(dl, P(x + 2 * u, y + 4 * u), P(x + 14 * u, y + 4 * u), col, t * 1.4f);
        ImDrawList_AddRect(dl, P(x + 6 * u, y + 1.5f * u), P(x + 10 * u, y + 4 * u), col, 0.8f * u, t, 0);
        ImDrawList_AddQuad(dl, P(x + 3.5f * u, y + 5.5f * u), P(x + 12.5f * u, y + 5.5f * u), P(x + 11.5f * u, y + 14.5f * u),
                           P(x + 4.5f * u, y + 14.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 6.5f * u, y + 7.5f * u), P(x + 6.8f * u, y + 12.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 9.5f * u, y + 7.5f * u), P(x + 9.2f * u, y + 12.5f * u), col, t);
        break;
    case ICON_RENAME: { // a pencil
        ImVec2 q[4] = { P(x + 11 * u, y + 2 * u), P(x + 14 * u, y + 5 * u), P(x + 5 * u, y + 14 * u), P(x + 2 * u, y + 11 * u) };
        body_poly(dl, q, 4, col, t);
        ImDrawList_AddTriangleFilled(dl, P(x + 2 * u, y + 11 * u), P(x + 5 * u, y + 14 * u), P(x + 1 * u, y + 15 * u), col);
        ImDrawList_AddLine(dl, P(x + 9.5f * u, y + 3.5f * u), P(x + 12.5f * u, y + 6.5f * u), dim, t);
        break;
    }
    case ICON_FOCUS: // a crosshair on its target
        ImDrawList_AddCircle(dl, P(cx, cy), 4.5f * u, col, 20, t * 1.2f);
        ImDrawList_AddCircleFilled(dl, P(cx, cy), 1.3f * u, col, 10);
        ImDrawList_AddLine(dl, P(cx, y + 1 * u), P(cx, y + 4 * u), col, t * 1.2f);
        ImDrawList_AddLine(dl, P(cx, y + 12 * u), P(cx, y + 15 * u), col, t * 1.2f);
        ImDrawList_AddLine(dl, P(x + 1 * u, cy), P(x + 4 * u, cy), col, t * 1.2f);
        ImDrawList_AddLine(dl, P(x + 12 * u, cy), P(x + 15 * u, cy), col, t * 1.2f);
        break;
    case ICON_FLOOR: // a box dropping onto the floor
        ImDrawList_AddRectFilled(dl, P(x + 4.5f * u, y + 1.5f * u), P(x + 11.5f * u, y + 7.5f * u), dim, 1.0f * u, 0);
        ImDrawList_AddRect(dl, P(x + 4.5f * u, y + 1.5f * u), P(x + 11.5f * u, y + 7.5f * u), col, 1.0f * u, t, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 5.5f * u, y + 9 * u), P(x + 10.5f * u, y + 9 * u), P(cx, y + 12.5f * u), col);
        ImDrawList_AddLine(dl, P(x + 1 * u, y + 14.5f * u), P(x + 15 * u, y + 14.5f * u), col, t * 1.5f);
        break;
    case ICON_BAKE: // a bulb and its light: Build Lighting
        body_circle(dl, P(cx, y + 7 * u), 3.8f * u, col, t);
        body_rect(dl, P(x + 6.5f * u, y + 10.6f * u), P(x + 9.5f * u, y + 13.5f * u), 0.5f * u, col, t, 0);
        for (int i = 0; i < 5; i++) {
            float a = 3.1416f + (float)i * 0.7854f, c = cosf(a), sn = sinf(a);
            ImDrawList_AddLine(dl, P(cx + c * 5.4f * u, y + 7 * u + sn * 5.4f * u), P(cx + c * 7.2f * u, y + 7 * u + sn * 7.2f * u), col, t);
        }
        break;
    case ICON_STATS: // a bar chart
        body_rect(dl, P(x + 2 * u, y + 9 * u), P(x + 5 * u, y + 14 * u), 0.6f * u, col, t, 0);
        body_rect(dl, P(x + 6.5f * u, y + 4 * u), P(x + 9.5f * u, y + 14 * u), 0.6f * u, col, t, 0);
        body_rect(dl, P(x + 11 * u, y + 7 * u), P(x + 14 * u, y + 14 * u), 0.6f * u, col, t, 0);
        break;
    case ICON_FULLSCREEN: // four corners
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;
            float px = (i & 1) ? x + 14 * u : x + 2 * u, py = (i & 2) ? y + 14 * u : y + 2 * u;
            ImDrawList_AddLine(dl, P(px, py), P(px + sx * 4 * u, py), col, t * 1.4f);
            ImDrawList_AddLine(dl, P(px, py), P(px, py + sy * 4 * u), col, t * 1.4f);
        }
        break;
    case ICON_SOUND: { // a speaker and two arcs
        ImVec2 body[6] = { P(x + 2 * u, y + 6 * u), P(x + 5 * u, y + 6 * u), P(x + 9 * u, y + 2.5f * u), P(x + 9 * u, y + 13.5f * u),
                           P(x + 5 * u, y + 10 * u), P(x + 2 * u, y + 10 * u) };
        body_poly(dl, body, 6, col, t);
        ImDrawList_PathArcTo(dl, P(x + 9 * u, cy), 3.2f * u, -0.9f, 0.9f, 10);
        ImDrawList_PathStroke(dl, col, t, 0);
        ImDrawList_PathArcTo(dl, P(x + 9 * u, cy), 5.8f * u, -0.95f, 0.95f, 14);
        ImDrawList_PathStroke(dl, col, t, 0);
        break;
    }
    case ICON_TIMER: // a stopwatch
        body_circle(dl, P(cx, y + 9.5f * u), 5.5f * u, col, t * 1.1f);
        ImDrawList_AddLine(dl, P(x + 6 * u, y + 1.5f * u), P(x + 10 * u, y + 1.5f * u), col, t * 1.4f);
        ImDrawList_AddLine(dl, P(cx, y + 2 * u), P(cx, y + 4 * u), col, t);
        ImDrawList_AddLine(dl, P(cx, y + 9.5f * u), P(cx + 2.8f * u, y + 6.5f * u), col, t * 1.3f);
        break;
    case ICON_EXPOSURE: // half bright, half dark: the camera's exposure
        ImDrawList_AddCircle(dl, P(cx, cy), 6.5f * u, col, 24, t);
        wedge(dl, cx, cy, 6.5f * u, -2.3562f, 3.1416f, col);
        break;
    case ICON_GAMEPAD:
        ImDrawList_AddRectFilled(dl, P(x + 1 * u, y + 5 * u), P(x + 15 * u, y + 12.5f * u), dim, 3.5f * u, 0);
        ImDrawList_AddRect(dl, P(x + 1 * u, y + 5 * u), P(x + 15 * u, y + 12.5f * u), col, 3.5f * u, t, 0);
        ImDrawList_AddLine(dl, P(x + 3.5f * u, y + 8.75f * u), P(x + 6.5f * u, y + 8.75f * u), col, t * 1.3f);
        ImDrawList_AddLine(dl, P(x + 5 * u, y + 7.25f * u), P(x + 5 * u, y + 10.25f * u), col, t * 1.3f);
        ImDrawList_AddCircleFilled(dl, P(x + 11 * u, y + 7.8f * u), 0.9f * u, col, 8);
        ImDrawList_AddCircleFilled(dl, P(x + 12.8f * u, y + 9.7f * u), 0.9f * u, col, 8);
        break;
    case ICON_GLOBE:
        body_circle(dl, P(cx, cy), 6.5f * u, col, t);
        ImDrawList_AddEllipse(dl, P(cx, cy), P(2.8f * u, 6.5f * u), col, 0, 20, t);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, cy), P(x + 14.5f * u, cy), col, t);
        break;
    case ICON_EXIT: // a door and an arrow leaving it
        body_rect(dl, P(x + 2 * u, y + 2 * u), P(x + 9 * u, y + 14 * u), 0.8f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 6 * u, cy), P(x + 13 * u, cy), col, t * 1.4f);
        ImDrawList_AddTriangleFilled(dl, P(x + 15 * u, cy), P(x + 12 * u, cy - 2.5f * u), P(x + 12 * u, cy + 2.5f * u), col);
        break;
    case ICON_INFO:
    case ICON_HELP:
        body_circle(dl, P(cx, cy), 6.5f * u, col, t);
        if (icon == ICON_INFO) {
            ImDrawList_AddCircleFilled(dl, P(cx, y + 4.8f * u), 1.0f * u, col, 8);
            ImDrawList_AddLine(dl, P(cx, y + 7 * u), P(cx, y + 12 * u), col, t * 1.6f);
        } else {
            ImDrawList_PathArcTo(dl, P(cx, y + 6.2f * u), 2.4f * u, -3.1416f, 0.9f, 10);
            ImDrawList_PathLineTo(dl, P(cx, y + 10 * u));
            ImDrawList_PathStroke(dl, col, t * 1.4f, 0);
            ImDrawList_AddCircleFilled(dl, P(cx, y + 12.4f * u), 1.0f * u, col, 8);
        }
        break;
    case ICON_BUG:
        ImDrawList_AddEllipseFilled(dl, P(cx, y + 9.5f * u), P(4 * u, 5 * u), dim, 0, 16);
        ImDrawList_AddEllipse(dl, P(cx, y + 9.5f * u), P(4 * u, 5 * u), col, 0, 16, t);
        ImDrawList_AddCircleFilled(dl, P(cx, y + 4 * u), 2.2f * u, col, 10);
        for (int i = 0; i < 3; i++) {
            float yy = y + (7.5f + 2.5f * (float)i) * u;
            ImDrawList_AddLine(dl, P(x + 4 * u, yy), P(x + 1.5f * u, yy - 1 * u), col, t);
            ImDrawList_AddLine(dl, P(x + 12 * u, yy), P(x + 14.5f * u, yy - 1 * u), col, t);
        }
        break;
    case ICON_UPDATE: // a circular arrow
        ImDrawList_PathArcTo(dl, P(cx, cy), 5.5f * u, 0.4f, 5.6f, 18);
        ImDrawList_PathStroke(dl, col, t * 1.4f, 0);
        ImDrawList_AddTriangleFilled(dl, P(x + 14.5f * u, y + 7.5f * u), P(x + 10.5f * u, y + 6 * u), P(x + 14 * u, y + 3 * u), col);
        break;
    case ICON_PANEL: // a window with a side panel
        ImDrawList_AddRect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 13.5f * u), col, 1.0f * u, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 10 * u, y + 2.5f * u), P(x + 14.5f * u, y + 13.5f * u), dim, 0, 0);
        ImDrawList_AddLine(dl, P(x + 10 * u, y + 2.5f * u), P(x + 10 * u, y + 13.5f * u), col, t);
        break;
    case ICON_CHEVRON:
        ImDrawList_AddTriangleFilled(dl, P(x + 4 * u, y + 6 * u), P(x + 12 * u, y + 6 * u), P(cx, y + 11 * u), col);
        break;
    case ICON_BLUEPRINT: // a sheet holding two nodes and their wire (Unreal's Blueprint Class)
        ImDrawList_AddRectFilled(dl, P(x + 1.5f * u, y + 1.5f * u), P(x + 14.5f * u, y + 14.5f * u), dim, 2.0f * u, 0);
        ImDrawList_AddRect(dl, P(x + 1.5f * u, y + 1.5f * u), P(x + 14.5f * u, y + 14.5f * u), col, 2.0f * u, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 3.5f * u, y + 4 * u), P(x + 7.5f * u, y + 8 * u), col, 0.8f * u, 0);
        ImDrawList_AddRectFilled(dl, P(x + 8.5f * u, y + 8 * u), P(x + 12.5f * u, y + 12 * u), col, 0.8f * u, 0);
        ImDrawList_AddBezierCubic(dl, P(x + 7.5f * u, y + 6 * u), P(x + 9.5f * u, y + 6 * u), P(x + 6.5f * u, y + 10 * u),
                                  P(x + 8.5f * u, y + 10 * u), col, t, 8);
        break;
    case ICON_CHARACTER: // a figure: head, body, arms, legs
        body_circle(dl, P(cx, y + 3 * u), 2.3f * u, col, t);
        body_rect(dl, P(x + 5.5f * u, y + 6 * u), P(x + 10.5f * u, y + 10.5f * u), 1.6f * u, col, t, 0);
        ImDrawList_AddLine(dl, P(x + 5.5f * u, y + 6.8f * u), P(x + 3 * u, y + 10 * u), col, t * 1.6f);
        ImDrawList_AddLine(dl, P(x + 10.5f * u, y + 6.8f * u), P(x + 13 * u, y + 10 * u), col, t * 1.6f);
        ImDrawList_AddLine(dl, P(x + 6.6f * u, y + 10 * u), P(x + 6 * u, y + 15 * u), col, t * 1.9f);
        ImDrawList_AddLine(dl, P(x + 9.4f * u, y + 10 * u), P(x + 10 * u, y + 15 * u), col, t * 1.9f);
        break;
    case ICON_PAWN: // the chess pawn
        body_tri(dl, P(cx, y + 5 * u), P(x + 11 * u, y + 12 * u), P(x + 5 * u, y + 12 * u), col, t);
        body_circle(dl, P(cx, y + 4 * u), 2.6f * u, col, t);
        body_rect(dl, P(x + 3.5f * u, y + 12 * u), P(x + 12.5f * u, y + 14.5f * u), 1.0f * u, col, t, 0);
        break;
    case ICON_GAMEMODE: // a flag on its pole
        ImDrawList_AddLine(dl, P(x + 3.5f * u, y + 1.5f * u), P(x + 3.5f * u, y + 15 * u), col, t * 1.4f);
        { ImVec2 q[4] = { P(x + 4 * u, y + 2 * u), P(x + 14 * u, y + 3.5f * u), P(x + 12 * u, y + 6.5f * u), P(x + 4 * u, y + 8.5f * u) };
          body_poly(dl, q, 4, col, t); }
        break;
    case ICON_STRUCT: // a table: a header row and two columns
        body_rect(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 13.5f * u), 1.0f * u, col, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 1.5f * u, y + 2.5f * u), P(x + 14.5f * u, y + 5.5f * u), col, 1.0f * u, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddLine(dl, P(x + 1.5f * u, y + 9.5f * u), P(x + 14.5f * u, y + 9.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 7 * u, y + 5.5f * u), P(x + 7 * u, y + 13.5f * u), col, t);
        break;
    case ICON_ENUM: // a list: three rows with their bullets
        for (int i = 0; i < 3; i++) {
            float yy = y + (3.5f + 4.5f * (float)i) * u;
            ImDrawList_AddRectFilled(dl, P(x + 1.5f * u, yy - 1.3f * u), P(x + 4.1f * u, yy + 1.3f * u), col, 0.5f * u, 0);
            ImDrawList_AddLine(dl, P(x + 6 * u, yy), P(x + 14.5f * u, yy), i == 1 ? col : dim, t * 1.5f);
        }
        break;
    case ICON_INTERFACE: // a plug and its socket
        body_circle(dl, P(x + 5 * u, cy), 3.2f * u, col, t);
        ImDrawList_AddLine(dl, P(x + 1 * u, cy), P(x + 1.8f * u, cy), col, t * 1.4f);
        ImDrawList_PathArcTo(dl, P(x + 11.5f * u, cy), 3.8f * u, 1.5708f, 4.7124f, 14);
        ImDrawList_PathStroke(dl, col, t * 1.4f, 0);
        ImDrawList_AddLine(dl, P(x + 11.5f * u, cy), P(x + 15 * u, cy), col, t * 1.4f);
        break;
    case ICON_DATA: // a database: three stacked discs
        ImDrawList_AddRectFilled(dl, P(x + 3 * u, y + 3.5f * u), P(x + 13 * u, y + 12.5f * u), dim, 0, 0);
        for (int i = 0; i < 3; i++) ImDrawList_AddEllipse(dl, P(cx, y + (3.5f + 4.5f * (float)i) * u), P(5 * u, 1.8f * u), col, 0, 18, t);
        ImDrawList_AddLine(dl, P(x + 3 * u, y + 3.5f * u), P(x + 3 * u, y + 12.5f * u), col, t);
        ImDrawList_AddLine(dl, P(x + 13 * u, y + 3.5f * u), P(x + 13 * u, y + 12.5f * u), col, t);
        break;
    case ICON_FUNCTION: // an italic f
        ImDrawList_PathArcTo(dl, P(x + 10.5f * u, y + 4.5f * u), 2.5f * u, 3.1416f, 6.0f, 10);
        ImDrawList_PathStroke(dl, col, t * 1.5f, 0);
        ImDrawList_AddLine(dl, P(x + 8 * u, y + 4.5f * u), P(x + 6.5f * u, y + 14.5f * u), col, t * 1.5f);
        ImDrawList_AddLine(dl, P(x + 4.5f * u, y + 8 * u), P(x + 11 * u, y + 8 * u), col, t * 1.5f);
        break;
    case ICON_EVENT: { // (Q5) Ignition: a bolt, the moment something happens
        ImVec2 pts[6] = { P(x + 9.5f * u, y + 1 * u), P(x + 3 * u, y + 9 * u), P(x + 7.5f * u, y + 9 * u), P(x + 6 * u, y + 15 * u),
                          P(x + 13 * u, y + 6.5f * u), P(x + 8.5f * u, y + 6.5f * u) };
        ImDrawList_AddConcavePolyFilled(dl, pts, 6, g_fill);
        ImDrawList_AddPolyline(dl, pts, 6, col, t, ImDrawFlags_Closed);
        break;
    }
    case ICON_VARIABLE: // a pill
        body_rect(dl, P(x + 1.5f * u, y + 5 * u), P(x + 14.5f * u, y + 11 * u), 3.0f * u, col, t, 0);
        ImDrawList_AddCircleFilled(dl, P(x + 4.6f * u, y + 8 * u), 1.2f * u, col, 8);
        break;
    case ICON_COMPONENT: // two stacked blocks: a part of an actor
        ImDrawList_AddRectFilled(dl, P(x + 5 * u, y + 2 * u), P(x + 14 * u, y + 9 * u), dim, 1.0f * u, 0);
        ImDrawList_AddRect(dl, P(x + 5 * u, y + 2 * u), P(x + 14 * u, y + 9 * u), col, 1.0f * u, t, 0);
        body_rect(dl, P(x + 2 * u, y + 7 * u), P(x + 11 * u, y + 14 * u), 1.0f * u, col, t, 0);
        break;
    case ICON_ARROW: // the forward arrow
        ImDrawList_AddLine(dl, P(x + 1.5f * u, cy), P(x + 10 * u, cy), col, t * 2.0f);
        ImDrawList_AddTriangleFilled(dl, P(x + 15 * u, cy), P(x + 9 * u, y + 3.5f * u), P(x + 9 * u, y + 12.5f * u), col);
        break;
    case ICON_CAPSULE:
        ImDrawList_AddRectFilled(dl, P(x + 4.5f * u, y + 1 * u), P(x + 11.5f * u, y + 15 * u), dim, 3.5f * u, 0);
        ImDrawList_AddRect(dl, P(x + 4.5f * u, y + 1 * u), P(x + 11.5f * u, y + 15 * u), col, 3.5f * u, t, 0);
        break;
    case ICON_MOVEMENT: // two chevrons: going somewhere
        for (int i = 0; i < 2; i++) {
            float xx = x + (3 + 6 * (float)i) * u;
            ImVec2 pts[3] = { P(xx, y + 3 * u), P(xx + 5 * u, cy), P(xx, y + 13 * u) };
            ImDrawList_AddPolyline(dl, pts, 3, col, t * 1.8f, 0);
        }
        break;
    case ICON_SLIDERS: // the class defaults: three sliders
        for (int i = 0; i < 3; i++) {
            float yy = y + (3.5f + 4.5f * (float)i) * u, k = x + (float)(i == 1 ? 10 : i == 0 ? 5 : 8) * u;
            ImDrawList_AddLine(dl, P(x + 1.5f * u, yy), P(x + 14.5f * u, yy), dim, t * 1.4f);
            ImDrawList_AddCircleFilled(dl, P(k, yy), 1.8f * u, col, 10);
        }
        break;
    case ICON_CHECK: {
        ImVec2 pts[3] = { P(x + 2.5f * u, y + 8.5f * u), P(x + 6.5f * u, y + 12.5f * u), P(x + 13.5f * u, y + 3.5f * u) };
        ImDrawList_AddPolyline(dl, pts, 3, col, t * 2.2f, 0);
        break;
    }
    case ICON_CROSS:
        ImDrawList_AddLine(dl, P(x + 3.5f * u, y + 3.5f * u), P(x + 12.5f * u, y + 12.5f * u), col, t * 2.2f);
        ImDrawList_AddLine(dl, P(x + 12.5f * u, y + 3.5f * u), P(x + 3.5f * u, y + 12.5f * u), col, t * 2.2f);
        break;
    case ICON_GRAPH: // two nodes and a wire: an event graph
        ImDrawList_AddRect(dl, P(x + 1 * u, y + 2.5f * u), P(x + 6.5f * u, y + 7.5f * u), col, 1.0f * u, t, 0);
        ImDrawList_AddRectFilled(dl, P(x + 9.5f * u, y + 8.5f * u), P(x + 15 * u, y + 13.5f * u), col, 1.0f * u, 0);
        ImDrawList_AddBezierCubic(dl, P(x + 6.5f * u, y + 5 * u), P(x + 10 * u, y + 5 * u), P(x + 6 * u, y + 11 * u),
                                  P(x + 9.5f * u, y + 11 * u), col, t, 10);
        break;
    default: icon_more(dl, icon, x, y, s, col, t); break;
    }
}

void ui_icon_item(UiIcon icon, float size, uint32_t col) {
    ImVec2 p = igGetCursorScreenPos();
    igDummy((ImVec2){ size, size });
    ui_icon(igGetWindowDrawList(), icon, p.x, p.y, size, col);
}

// (Q5) An icon's button's tooltip: its name is its label (or a short tip: "Grid", "Move (W)"); a longer tip is its text
static void tip_parts(UiIcon icon, const char* label, const char* tip) {
    if (!igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    bool has_label = label && label[0], short_tip = tip && tip[0] && strlen(tip) < 40 && !strchr(tip, '.') && !strchr(tip, ':');
    const char* name = has_label ? label : short_tip ? tip : NULL;
    const char* desc = tip && tip[0] && !(short_tip && !has_label) ? tip : NULL;
    ui_tip_item((int)icon, name, desc, NULL);
}

bool ui_icon_button(const char* id, UiIcon icon, const char* label, uint32_t col, bool active, const char* tip) {
    float h = igGetFrameHeight();
    float icon_s = h * 0.62f;
    float w = h;
    char bid[96];
    snprintf(bid, sizeof bid, "##%s", id); // the label is drawn below, next to the icon
    if (label && label[0]) w = h * 0.5f + icon_s + 6.0f + igCalcTextSize(label, NULL, true, -1.0f).x + h * 0.4f;
    if (active) igPushStyleColor_Vec4(ImGuiCol_Button, THVA(TK_ACCENT, 0.75f));
    bool pressed = igButton(bid, (ImVec2){ w, h });
    if (active) igPopStyleColor(1);
    if (label && label[0]) ui_tip_last_item(label); // (its description by the label drawn on it; no label: by its id)
    tip_parts(icon, label, tip); // (Q5: the standard tooltip: the icon, the name, the text, the shortcut)
    ImVec2 a = igGetItemRectMin();
    ImDrawList* dl = igGetWindowDrawList();
    float ix = label && label[0] ? a.x + h * 0.35f : a.x + (w - icon_s) * 0.5f;
    ui_icon(dl, icon, ix, a.y + (h - icon_s) * 0.5f, icon_s, col);
    if (label && label[0])
        ImDrawList_AddText_Vec2(dl, (ImVec2){ ix + icon_s + 6.0f, a.y + igGetStyle()->FramePadding.y },
                                igGetColorU32_Col(ImGuiCol_Text, 1.0f), label, NULL);
    return pressed;
}

bool ui_dropdown_button(const char* id, UiIcon icon, const char* label, uint32_t col, const char* tip) {
    float h = igGetFrameHeight();
    float icon_s = h * 0.62f, cs = h * 0.45f, tw = igCalcTextSize(label, NULL, true, -1.0f).x;
    float w = h * 0.35f + icon_s + 6.0f + tw + 4.0f + cs + h * 0.3f;
    char bid[96];
    snprintf(bid, sizeof bid, "##%s", id);
    bool pressed = igButton(bid, (ImVec2){ w, h });
    ui_tip_last_item(label);
    tip_parts(icon, label, tip);
    ImVec2 a = igGetItemRectMin();
    ImDrawList* dl = igGetWindowDrawList();
    float ix = a.x + h * 0.35f;
    ui_icon(dl, icon, ix, a.y + (h - icon_s) * 0.5f, icon_s, col);
    uint32_t text = igGetColorU32_Col(ImGuiCol_Text, 1.0f);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ ix + icon_s + 6.0f, a.y + igGetStyle()->FramePadding.y }, text, label, NULL);
    ui_icon(dl, ICON_CHEVRON, ix + icon_s + 6.0f + tw + 4.0f, a.y + (h - cs) * 0.5f, cs, (text & 0x00FFFFFFu) | 0xA0000000u);
    return pressed;
}

// Menu items with an icon: the label is pushed right by enough spaces for it, and the icon is drawn in that gap after
// the item (ImGui's menu items have no icon slot). The id keeps the plain label (## part): renaming nothing.
static void icon_label(char* out, size_t n, const char* label) {
    float sp = igCalcTextSize(" ", NULL, false, -1.0f).x;
    int k = (int)ceilf((igGetFontSize() + 6.0f) / (sp > 0.0f ? sp : 4.0f));
    if (k > 24) k = 24;
    size_t len = 0;
    for (; len < (size_t)k && len + 1 < n; len++) out[len] = ' ';
    snprintf(out + len, n - len, "%s", label);
}
static void icon_after(UiIcon icon, bool enabled) {
    ImVec2 a = igGetItemRectMin(), b = igGetItemRectMax();
    float s = igGetFontSize();
    uint32_t col = igGetColorU32_Col(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled, 1.0f);
    ui_icon(igGetWindowDrawList(), icon, a.x, a.y + (b.y - a.y - s) * 0.5f, s, col);
}

bool ui_menu_item(UiIcon icon, const char* label, const char* shortcut, bool selected, bool enabled) {
    char buf[256];
    icon_label(buf, sizeof buf, label);
    bool r = igMenuItem_Bool(buf, shortcut, selected, enabled);
    icon_after(icon, enabled);
    ui_tip_item((int)icon, NULL, NULL, shortcut);
    return r;
}

bool ui_menu_item_ptr(UiIcon icon, const char* label, const char* shortcut, bool* selected, bool enabled) {
    char buf[256];
    icon_label(buf, sizeof buf, label);
    bool r = igMenuItem_BoolPtr(buf, shortcut, selected, enabled);
    icon_after(icon, enabled);
    ui_tip_item((int)icon, NULL, NULL, shortcut);
    return r;
}

bool ui_begin_menu(UiIcon icon, const char* label, bool enabled) {
    char buf[256];
    icon_label(buf, sizeof buf, label);
    bool r = igBeginMenu(buf, enabled);
    icon_after(icon, enabled);
    ui_tip_item((int)icon, NULL, NULL, NULL);
    return r;
}

// --- Animated (asset_viewer.c's loading tiles, the status bar's work, unsaved and playing states) ---

// (Q26) A folder drawn solid, as the Content Browser's big tiles show it: the back with its tab a shade darker, the
// front lower and lighter, a faint line of light on its top edge
void ui_folder_solid(ImDrawList* dl, float x, float y, float s, uint32_t col) {
    float w = s, h = s * 0.78f, y0 = y + (s - h) * 0.5f, r = s * 0.06f;
    uint32_t back = theme_scale(col, 0.78f), front = col, light = theme_alpha(theme_mix(col, THEME_IMAGE_TINT, 0.45f), (col >> 24) / 2);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y0 }, (ImVec2){ x + w * 0.42f, y0 + h * 0.3f }, back, r, ImDrawFlags_RoundCornersTop);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y0 + h * 0.12f }, (ImVec2){ x + w, y0 + h }, back, r, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x, y0 + h * 0.26f }, (ImVec2){ x + w, y0 + h }, front, r, 0);
    ImDrawList_AddLine(dl, (ImVec2){ x + r, y0 + h * 0.26f + 1.0f }, (ImVec2){ x + w - r, y0 + h * 0.26f + 1.0f }, light, 1.0f);
}

void ui_spinner(ImDrawList* dl, float x, float y, float s, uint32_t col, double t) {
    float cx = x + s * 0.5f, cy = y + s * 0.5f, r = s * 0.38f, w = fmaxf(1.5f, s / 9.0f);
    float a0 = (float)fmod(t * 5.5, 6.2831853), len = 1.2f + 1.6f * (0.5f + 0.5f * sinf((float)t * 3.1f)); // (the arc breathes)
    ImDrawList_PathArcTo(dl, P(cx, cy), r, a0, a0 + len, 24);
    ImDrawList_PathStroke(dl, col, w, 0);
    uint32_t faint = (col & 0x00FFFFFFu) | ((uint32_t)((col >> 24) * 0.18f) << 24);
    ImDrawList_AddCircle(dl, P(cx, cy), r, faint, 32, w);
}

void ui_pulse_dot(ImDrawList* dl, float x, float y, float s, uint32_t col, double t) {
    float cx = x + s * 0.5f, cy = y + s * 0.5f, ph = (float)fmod(t, 1.4) / 1.4f; // a ring growing out of the dot every 1.4 s
    float a = (float)(col >> 24) / 255.0f;
    uint32_t ring = (col & 0x00FFFFFFu) | ((uint32_t)(a * (1.0f - ph) * 0.7f * 255.0f) << 24);
    ImDrawList_AddCircle(dl, P(cx, cy), s * (0.18f + 0.32f * ph), ring, 24, fmaxf(1.0f, s / 12.0f));
    float breathe = 0.85f + 0.15f * sinf((float)t * 4.0f);
    ImDrawList_AddCircleFilled(dl, P(cx, cy), s * 0.2f * breathe, col, 16);
}

void ui_icon_anim(ImDrawList* dl, UiIcon icon, float x, float y, float s, uint32_t col, UiAnim anim, double t) {
    int v0 = dl->VtxBuffer.Size;
    ui_icon(dl, icon, x, y, s, col);
    float cx = x + s * 0.5f, cy = y + s * 0.5f, k = 1.0f, ang = 0.0f, dy = 0.0f;
    if (anim == UI_ANIM_SPIN) ang = (float)fmod(t * 4.0, 6.2831853);
    else if (anim == UI_ANIM_PULSE) k = 1.0f + 0.12f * sinf((float)t * 5.0f);
    else if (anim == UI_ANIM_POP) { // (t from 0: overshoots to 1.25, settles at 1 by 0.35 s)
        float u = fminf(fmaxf((float)t / 0.35f, 0.0f), 1.0f);
        k = 1.0f + 0.6f * sinf(u * 3.14159265f) * (1.0f - u);
        k *= 0.55f + 0.45f * fminf(u * 3.0f, 1.0f);
    } else if (anim == UI_ANIM_BOUNCE)
        dy = -fabsf(sinf((float)t * 6.0f)) * s * 0.12f;
    float cs = cosf(ang), sn = sinf(ang);
    for (int i = v0; i < dl->VtxBuffer.Size; i++) { // (the icon's vertices: turned, scaled about its center, lifted)
        ImDrawVert* v = &dl->VtxBuffer.Data[i];
        float px = v->pos.x - cx, py = v->pos.y - cy;
        v->pos.x = cx + (px * cs - py * sn) * k;
        v->pos.y = cy + (px * sn + py * cs) * k + dy;
    }
}

bool ui_splitter(const char* id, bool vertical, float* size, float sign, float mn, float mx, float len, float def) {
    ImVec2 at = igGetCursorScreenPos();
    ImVec2 sz = vertical ? (ImVec2){ UI_SPLIT, len > 1.0f ? len : 1.0f } : (ImVec2){ len > 1.0f ? len : 1.0f, UI_SPLIT };
    igInvisibleButton(id, sz, 0);
    bool hot = igIsItemHovered(0), act = igIsItemActive();
    if (hot || act) igSetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (hot && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) *size = def;
    if (act) *size += (vertical ? igGetIO_Nil()->MouseDelta.x : igGetIO_Nil()->MouseDelta.y) * sign;
    if (mx < mn) mx = mn;
    *size = *size < mn ? mn : *size > mx ? mx : *size;
    ImDrawList* dl = igGetWindowDrawList();
    ImU32 col = act ? TH(TK_ACCENT) : hot ? THA(TK_ACCENT_STRONG, 200) : 0u; // (Unreal's blue while held)
    ImDrawList_AddRectFilled(dl, at, (ImVec2){ at.x + sz.x, at.y + sz.y }, col, 0, 0);
    if (!hot && !act) { // (a thin line in its middle: where to grab)
        if (vertical) ImDrawList_AddLine(dl, (ImVec2){ at.x + sz.x * 0.5f, at.y }, (ImVec2){ at.x + sz.x * 0.5f, at.y + sz.y }, TH(TK_N8), 1.0f);
        else ImDrawList_AddLine(dl, (ImVec2){ at.x, at.y + sz.y * 0.5f }, (ImVec2){ at.x + sz.x, at.y + sz.y * 0.5f }, TH(TK_N8), 1.0f);
    }
    return act;
}

// --- (Q5) The icon sheet (--icon-sheet; the icon_sheet test's PNG): every icon at 16, 24 and 32 px with its name, on
// the panel's surface, in the current theme. A window over the whole main viewport
void ui_icon_sheet_draw(void) {
    const ImGuiViewport* vp = igGetMainViewport();
    igSetNextWindowPos(vp->WorkPos, ImGuiCond_Always, (ImVec2){ 0, 0 });
    igSetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    igSetNextWindowFocus();
    if (igBegin("Icon Sheet###re_icon_sheet", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = igGetWindowDrawList();
        ImVec2 o = igGetCursorScreenPos();
        float fs = igGetFontSize(), cw = 32.0f + 24.0f + 16.0f + 24.0f + fs * 9.0f, ch = 40.0f;
        int cols = (int)((vp->WorkSize.x - 16.0f) / cw);
        if (cols < 1) cols = 1;
        char head[96];
        snprintf(head, sizeof head, "Rivora Engine icons: %d (16, 24, 32 px), theme %s", (int)ICON_COUNT, theme_current_name());
        ImDrawList_AddText_Vec2(dl, o, TH(TK_TEXT_STRONG), head, NULL);
        o.y += fs * 1.8f;
        uint32_t stroke = TH(TK_TEXT);
        for (int i = 0; i < ICON_COUNT; i++) {
            float x = o.x + (float)(i % cols) * cw, y = o.y + (float)(i / cols) * ch;
            ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + cw - 6.0f, y + ch - 4.0f }, TH(TK_BG_RAISED), 4.0f, 0);
            ui_icon(dl, (UiIcon)i, x + 4.0f, y + 2.0f, 32.0f, stroke);
            ui_icon(dl, (UiIcon)i, x + 42.0f, y + 6.0f, 24.0f, stroke);
            ui_icon(dl, (UiIcon)i, x + 72.0f, y + 10.0f, 16.0f, stroke);
            ImDrawList_AddRectFilled(dl, (ImVec2){ x + 94.0f, y + 10.0f }, (ImVec2){ x + 100.0f, y + 26.0f }, ui_icon_family_color((UiIcon)i), 2.0f, 0);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 106.0f, y + (ch - 4.0f - fs) * 0.5f }, TH(TK_TEXT_DIM), g_icon_names[i], NULL);
        }
    }
    igEnd();
}
