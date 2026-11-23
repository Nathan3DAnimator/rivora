// The quests' markers over the view (quest.h): the tracked quest's objectives, a diamond on the actor with the distance
// under it; out of view (or behind) it sits at the screen's edge with an arrow pointing the way. Drawn after the widgets
// in the editor's PIE (editor_viewport.c) and the game (game_main.c), beside the damage numbers. quest_tick fills the list.
#include <math.h>
#include <stdio.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "quest.h"
#include "ui_widget.h"

static ImU32 rgba8(int r, int g, int b, int a) { return (ImU32)a << 24 | (ImU32)b << 16 | (ImU32)g << 8 | (ImU32)r; }

void quest_draw_markers(ImDrawList* dl, const float pos[3], const float fwd[3], const float right[3], const float up[3], float fov_deg,
                        float x0, float y0, float x1, float y1) {
    if (!dl || !quest_markers_shown() || x1 <= x0 || y1 <= y0) return;
    uint32_t n = 0;
    const QuestMarker* m = quest_markers(&n);
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    float focal = hw / tanf(fmaxf(fov_deg, 1.0f) * 0.5f * 0.017453292f);
    float k = (y1 - y0) / UI_REF_HEIGHT;
    float margin = 46.0f * k, s = 13.0f * k;
    ImFont* font = igGetFont();
    float fsize = fmaxf(21.0f * k, 10.0f);
    for (uint32_t i = 0; i < n; i++) {
        float d[3] = { m[i].pos[0] - pos[0], m[i].pos[1] - pos[1], m[i].pos[2] - pos[2] };
        float qz = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        float qx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], qy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        ImVec2 p = { cx, cy };
        bool on = false;
        if (qz > 10.0f) {
            p.x = cx + qx / qz * focal, p.y = cy - qy / qz * focal;
            on = p.x >= x0 + margin && p.x <= x1 - margin && p.y >= y0 + margin && p.y <= y1 - margin;
        }
        float dx = 0.0f, dy = 0.0f;
        if (!on) { // the screen's edge, the way to it
            dx = qx, dy = -qy;
            if (qz <= 10.0f && fabsf(dx) < 1.0f) dx = 1.0f; // (right behind: to a side)
            if (qz <= 10.0f) dy = fabsf(dy) > fabsf(dx) ? dy : dy * 0.3f; // (behind: mostly sideways)
            float l = sqrtf(dx * dx + dy * dy);
            if (l < 1e-4f) dx = 0.0f, dy = 1.0f, l = 1.0f;
            dx /= l, dy /= l;
            float tx = fabsf(dx) > 1e-4f ? (hw - margin) / fabsf(dx) : 1e9f, ty = fabsf(dy) > 1e-4f ? (hh - margin) / fabsf(dy) : 1e9f;
            float t = fminf(tx, ty);
            p.x = cx + dx * t, p.y = cy + dy * t;
        }
        ImU32 fill = m[i].optional ? rgba8(200, 205, 215, 235) : rgba8(255, 196, 64, 240), dark = rgba8(0, 0, 0, 170);
        ImVec2 a = { p.x, p.y - s * 1.25f }, b = { p.x + s, p.y }, c = { p.x, p.y + s * 1.25f }, e = { p.x - s, p.y };
        ImVec2 o = { 1.5f * k, 1.5f * k }; // (its shadow)
        ImDrawList_AddQuadFilled(dl, (ImVec2){ a.x + o.x, a.y + o.y }, (ImVec2){ b.x + o.x, b.y + o.y }, (ImVec2){ c.x + o.x, c.y + o.y },
                                 (ImVec2){ e.x + o.x, e.y + o.y }, dark);
        ImDrawList_AddQuadFilled(dl, a, b, c, e, fill);
        ImDrawList_AddQuad(dl, a, b, c, e, rgba8(40, 30, 10, 220), fmaxf(1.0f, 1.2f * k));
        if (!on) { // the arrow beyond it, toward the target
            float ax = p.x + dx * s * 2.1f, ay = p.y + dy * s * 2.1f, px = -dy, py = dx;
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ ax + dx * s * 0.9f, ay + dy * s * 0.9f }, (ImVec2){ ax + px * s * 0.7f, ay + py * s * 0.7f },
                                         (ImVec2){ ax - px * s * 0.7f, ay - py * s * 0.7f }, fill);
        }
        char t[24];
        float meters = m[i].dist / 100.0f;
        if (meters < 10.0f) snprintf(t, sizeof t, "%.1f m", meters);
        else snprintf(t, sizeof t, "%.0f m", meters);
        ImVec2 ts = ImFont_CalcTextSizeA(font, fsize, 1e9f, 0.0f, t, NULL, NULL);
        float ty0 = p.y + s * 1.25f + 4.0f * k;
        if (ty0 + ts.y > y1 - 2.0f) ty0 = p.y - s * 1.25f - 4.0f * k - ts.y; // (at the bottom edge: over it)
        ImVec2 at = { p.x - ts.x * 0.5f, ty0 };
        ImDrawList_AddText_FontPtr(dl, font, fsize, (ImVec2){ at.x + 1.5f * k, at.y + 1.5f * k }, dark, t, NULL, 0.0f, NULL);
        ImDrawList_AddText_FontPtr(dl, font, fsize, at, rgba8(255, 255, 255, 235), t, NULL, 0.0f, NULL);
    }
}
