// HUD modular, the drawing (hud.h, notas-dev/hud.md): each HUD piece drawn with ImGui's draw list into its rectangle
// (the Designer's preview too, with made-up content), the Health's floating numbers (one drawing for the widget and for
// the view when no widget shows them), the pieces' Details in the Designer and the HUD Layout component's. Every piece
// is timed and its vertices counted (the GPU's work: the HUD is a few hundred quads in the UI pass).
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "theme.h"
#include "components.h"
#include "game_settings.h"
#include "health.h"
#include "hud.h"
#include "i18n.h"
#include "platform.h"
#include "ui_tips.h"
#include "ui_widget.h"

#define COL(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define DEG 0.017453292f

static ImU32 colf(const float c[4], float a) {
    float x[4];
    for (int i = 0; i < 4; i++) x[i] = fminf(fmaxf(c[i], 0.0f), 1.0f);
    float al = fminf(fmaxf(x[3] * a, 0.0f), 1.0f);
    return COL((int)(x[0] * 255), (int)(x[1] * 255), (int)(x[2] * 255), (int)(al * 255));
}
static const float k_black[4] = { 0, 0, 0, 1 };

// a text centered (jx, jy: 0 left / top .. 1 right / bottom) on (x, y); bold: twice, a pixel apart; shadow under it
static void text_atw(ImDrawList* dl, const char* s, float x, float y, float size, const float col[4], float al, float jx, float jy, bool bold, bool shadow, float tw) {
    if (!s || !s[0] || al <= 0.003f) return;
    ImFont* font = igGetFont();
    size = fmaxf(size, 6.0f);
    if (size > 20.0f) size = 2.0f * roundf(size * 0.5f); // (fewer sizes baked by ImGui 1.92: the numbers grow and shrink every frame)
    ImVec2 ts = tw >= 0.0f ? (ImVec2){ tw, size } : jx > 0.0f ? ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, s, NULL, NULL) : (ImVec2){ 0.0f, size };
    ImVec2 p = { floorf(x - ts.x * jx), floorf(y - ts.y * jy) };
    float o = fmaxf(size / 18.0f, 1.0f);
    if (shadow) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + o, p.y + o }, colf(k_black, al * 0.75f), s, NULL, 0.0f, NULL);
    ImU32 c = colf(col, al);
    ImDrawList_AddText_FontPtr(dl, font, size, p, c, s, NULL, 0.0f, NULL);
    if (bold) ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2){ p.x + o * 0.7f, p.y }, c, s, NULL, 0.0f, NULL);
}
static void text_at(ImDrawList* dl, const char* s, float x, float y, float size, const float col[4], float al, float jx, float jy, bool bold, bool shadow) {
    text_atw(dl, s, x, y, size, col, al, jx, jy, bold, shadow, -1.0f);
}
// a diamond; its edge: a dark one a little bigger behind it (a fill is cheaper than a stroke, and as sharp)
static void diamond(ImDrawList* dl, float x, float y, float r, ImU32 fill, ImU32 edge) {
    if (edge) {
        float e = r + fmaxf(r * 0.2f, 1.0f);
        ImDrawList_AddQuadFilled(dl, (ImVec2){ x, y - e }, (ImVec2){ x + e, y }, (ImVec2){ x, y + e }, (ImVec2){ x - e, y }, edge);
    }
    ImDrawList_AddQuadFilled(dl, (ImVec2){ x, y - r }, (ImVec2){ x + r, y }, (ImVec2){ x, y + r }, (ImVec2){ x - r, y }, fill);
}

// The markers' texts measured once (by slot: the label's width at its size, the distance's text and width): a frame
// draws them without walking their glyphs twice
typedef struct MkText { uint32_t id; float size, lw, dsize, dw; char dist[16]; } MkText;
static MkText g_mkt[HUD_MARKERS_MAX];
static float label_w(int slot, const HudMarker* m, const char* label, float size) {
    MkText* t = &g_mkt[slot];
    if (t->id != m->id || t->size != size) t->id = m->id, t->size = size, t->lw = ImFont_CalcTextSizeA(igGetFont(), fmaxf(size, 6.0f), 1e9f, 0.0f, label, NULL, NULL).x, t->dist[0] = 0;
    return t->lw;
}
static const char* dist_w(int slot, const HudMarker* m, float cm, float size, float* w);

// The view's heading (degrees, 0 north, 90 east), 0 without a view
static float view_heading(void) {
    const HudView* v = hud_view();
    if (!v->set) return 0.0f;
    float d[3] = { v->fwd[0], v->fwd[1], 0.0f };
    if (fabsf(d[0]) + fabsf(d[1]) < 1e-4f) d[0] = v->up[0], d[1] = v->up[1]; // (straight down: the top of the view)
    return hud_bearing(d);
}
static float wrap180(float a) {
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}
static void dist_text(char* s, size_t cap, float cm) {
    if (cm >= 100000.0f) snprintf(s, cap, "%.1f km", cm / 100000.0f);
    else snprintf(s, cap, "%.0f m", cm / 100.0f);
}
static const char* dist_w(int slot, const HudMarker* m, float cm, float size, float* w) {
    MkText* t = &g_mkt[slot];
    char s[16];
    dist_text(s, sizeof s, cm);
    if (t->id != m->id) t->id = m->id, t->size = -1.0f;
    if (strcmp(s, t->dist) || t->dsize != size) memcpy(t->dist, s, sizeof s), t->dsize = size, t->dw = ImFont_CalcTextSizeA(igGetFont(), fmaxf(size, 6.0f), 1e9f, 0.0f, s, NULL, NULL).x;
    *w = t->dw;
    return t->dist;
}

// --- Attribute Bar ---
static void bar_part(ImDrawList* dl, float x0, float y0, float x1, float y1, int orient, float f0, float f1, ImU32 c, float r) {
    if (f1 <= f0 + 1e-4f) return;
    float a0, a1;
    if (orient == 2) { // bottom to top
        a0 = y1 - (y1 - y0) * f1, a1 = y1 - (y1 - y0) * f0;
        ImDrawList_AddRectFilled(dl, (ImVec2){ x0, a0 }, (ImVec2){ x1, a1 }, c, r, 0);
    } else if (orient == 1) { // right to left
        a0 = x1 - (x1 - x0) * f1, a1 = x1 - (x1 - x0) * f0;
        ImDrawList_AddRectFilled(dl, (ImVec2){ a0, y0 }, (ImVec2){ a1, y1 }, c, r, 0);
    } else {
        a0 = x0 + (x1 - x0) * f0, a1 = x0 + (x1 - x0) * f1;
        ImDrawList_AddRectFilled(dl, (ImVec2){ a0, y0 }, (ImVec2){ a1, y1 }, c, r, 0);
    }
}
static void draw_bar(ImDrawList* dl, const UiWidget* w, Entity owner, int index, float x0, float y0, float x1, float y1, float k, float al, bool designer, double now) {
    float f, trail, gain, value = 0.0f, max = 0.0f;
    if (designer || !hud_world()) f = fminf(fmaxf(w->percent, 0.0f), 1.0f), trail = fminf(f + 0.12f, 1.0f), gain = f, value = f * 100.0f, max = 100.0f;
    else {
        HudBarState* s = hud_bar_state(owner, index, w, hud_dt());
        if (!s || s->shown < -1.5f) return; // (no such attribute on the actor: hidden)
        f = s->shown, trail = s->trail, gain = s->gain, value = s->value, max = s->max;
    }
    float r = fminf(w->radius * k, (y1 - y0) * 0.5f);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, colf(w->bg, al), r, 0);
    int o = w->orient;
    float pad = fmaxf(w->border_w * k, 0.0f);
    float ix0 = x0 + pad, iy0 = y0 + pad, ix1 = x1 - pad, iy1 = y1 - pad;
    bar_part(dl, ix0, iy0, ix1, iy1, o, f, trail, colf(w->hover, al), r);
    bar_part(dl, ix0, iy0, ix1, iy1, o, f, gain, colf(w->press, al), r);
    float c[4] = { w->color[0], w->color[1], w->color[2], w->color[3] };
    if (w->vstep > 0.0f && f < w->vstep && f > 0.0f) { // low: it pulses
        float p = 0.5f + 0.5f * sinf((float)now * 7.0f);
        for (int i = 0; i < 3; i++) c[i] = c[i] + (1.0f - c[i]) * p * 0.45f;
    }
    bar_part(dl, ix0, iy0, ix1, iy1, o, 0.0f, f, colf(c, al), r);
    if (w->columns > 1) { // the segments
        ImU32 sc = colf(w->bg, al * 0.9f);
        for (int s = 1; s < w->columns; s++) {
            float t = (float)s / w->columns;
            if (o == 2) ImDrawList_AddLine(dl, (ImVec2){ ix0, iy1 - (iy1 - iy0) * t }, (ImVec2){ ix1, iy1 - (iy1 - iy0) * t }, sc, fmaxf(1.5f * k, 1.0f));
            else {
                float x = o == 1 ? ix1 - (ix1 - ix0) * t : ix0 + (ix1 - ix0) * t;
                ImDrawList_AddLine(dl, (ImVec2){ x, iy0 }, (ImVec2){ x, iy1 }, sc, fmaxf(1.5f * k, 1.0f));
            }
        }
    }
    if (w->border_w > 0.0f) ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, colf(w->border_col, al), r, fmaxf(w->border_w * k, 1.0f), 0);
    if (w->checked && max > 0.0f) {
        char s[48];
        snprintf(s, sizeof s, "%.0f / %.0f", ceilf(value - 0.001f), max);
        float size = fminf(w->font_size * k, (y1 - y0) * 0.95f);
        if (size >= 7.0f) text_at(dl, s, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, size, (const float[4]){ 1, 1, 1, 1 }, al, 0.5f, 0.5f, w->bold, true);
    }
}

// --- Compass ---
static void draw_compass(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    float span = w->value > 10.0f ? w->value : 120.0f, width = x1 - x0, cx = (x0 + x1) * 0.5f;
    float head = designer ? 0.0f : view_heading();
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, colf(w->bg, al), w->radius * k, 0);
    ImDrawList_PushClipRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, true);
    float mid = y0 + (y1 - y0) * 0.42f, size = w->font_size * k;
    // the ticks every 15 degrees, the cardinal points every 45 (N in red), fading at the ends
    int first = (int)floorf((head - span * 0.5f) / 15.0f), last = (int)ceilf((head + span * 0.5f) / 15.0f);
    for (int t = first; t <= last; t++) {
        float deg = (float)t * 15.0f, rel = deg - head, x = cx + rel / span * width;
        float fade = 1.0f - fminf(fabsf(rel) / (span * 0.5f), 1.0f);
        fade = fminf(fade * 3.0f, 1.0f);
        int d = ((t * 15) % 360 + 360) % 360;
        if (d % 45 == 0) {
            const float red[4] = { 1.0f, 0.32f, 0.26f, 1.0f };
            text_at(dl, hud_cardinal(d / 45), x, mid, d % 90 == 0 ? size : size * 0.72f, d == 0 ? red : w->color, al * fade, 0.5f, 0.5f, w->bold, true);
        } else
            ImDrawList_AddLine(dl, (ImVec2){ x, mid - size * 0.22f }, (ImVec2){ x, mid + size * 0.22f }, colf(w->color, al * fade * 0.6f), fmaxf(1.5f * k, 1.0f));
    }
    // the markers: a diamond where their bearing is (at the end when out of the strip), their distance under it
    uint32_t n = 0;
    const HudMarker* m = hud_markers(&n);
    const HudView* v = hud_view();
    for (uint32_t i = 0; !designer && v->set && i < n; i++) {
        if (!m[i].id || !(m[i].flags & HUD_MK_COMPASS)) continue;
        float d[3] = { m[i].pos[0] - v->pos[0], m[i].pos[1] - v->pos[1], 0.0f };
        float rel = wrap180(hud_bearing(d) - head), lim = span * 0.5f - 4.0f;
        bool out = fabsf(rel) > lim;
        if (out) rel = rel > 0.0f ? lim : -lim;
        float x = cx + rel / span * width, ym = y1 - (y1 - y0) * 0.3f;
        diamond(dl, x, ym, 7.0f * k * (out ? 0.75f : 1.0f), colf(m[i].color, al * (out ? 0.6f : 1.0f)), colf(k_black, al * 0.6f));
        if ((w->checked || (m[i].flags & HUD_MK_DISTANCE)) && !out) {
            char s[24];
            dist_text(s, sizeof s, sqrtf(d[0] * d[0] + d[1] * d[1]));
            text_at(dl, s, x + 10.0f * k, ym, size * 0.6f, w->color, al * 0.9f, 0.0f, 0.5f, false, true);
        }
    }
    if (designer) diamond(dl, cx + 25.0f / span * width, y1 - (y1 - y0) * 0.3f, 7.0f * k, colf(w->hover, al), 0);
    ImDrawList_PopClipRect(dl);
    // where the view points: a small triangle on top
    float s = 6.0f * k;
    ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - s, y0 }, (ImVec2){ cx + s, y0 }, (ImVec2){ cx, y0 + s * 1.3f }, colf(w->color, al));
}

// --- Message Feed ---
static void draw_messages(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    uint32_t maxn = w->columns >= 1 ? w->columns : 5;
    if (!designer) hud_feed_config(maxn, w->value);
    const HudMessage* list[HUD_MSG_MAX];
    uint32_t n = 0, cap = 0;
    static HudMessage demo[3];
    if (designer) {
        const char* t[3] = { "+1 Coin", "Quest Complete", "+25 XP" };
        for (int i = 0; i < 3; i++) {
            snprintf(demo[i].text, sizeof demo[i].text, "%s", t[i]);
            memcpy(demo[i].color, i == 1 ? (const float[4]){ 1.0f, 0.82f, 0.3f, 1.0f } : w->color, 16);
            demo[i].age = 1.0f, demo[i].life = 3.0f, demo[i].serial = (uint32_t)i;
            list[n++] = &demo[i];
        }
    } else {
        const HudMessage* m = hud_messages(&cap);
        for (uint32_t i = 0; i < cap; i++)
            if (m[i].text[0] && m[i].age >= 0.0f) list[n++] = &m[i];
        for (uint32_t a = 1; a < n; a++) // by the order they came in
            for (uint32_t b = a; b > 0 && list[b]->serial < list[b - 1]->serial; b--) {
                const HudMessage* x = list[b];
                list[b] = list[b - 1], list[b - 1] = x;
            }
    }
    float size = w->font_size * k, row = size * 1.55f, jx = fminf(fmaxf(w->align[0], 0.0f), 1.0f);
    bool up = w->orient == 1; // (the newest on top)
    for (uint32_t r = 0; r < n && r < maxn; r++) {
        const HudMessage* m = list[up ? n - 1 - r : r];
        float in = fminf(m->age / 0.18f, 1.0f), outa = m->age > m->life ? 1.0f - (m->age - m->life) / 0.35f : 1.0f;
        float a = al * fmaxf(outa, 0.0f) * in;
        float slide = (1.0f - in) * 60.0f * k * (jx > 0.5f ? 1.0f : -1.0f);
        float y = y0 + row * (float)r + row * 0.5f;
        if (y + row * 0.5f > y1 + 0.5f) break;
        const char* s = designer ? m->text : gus_tr(m->text);
        ImVec2 ts = ImFont_CalcTextSizeA(igGetFont(), size, 1e9f, 0.0f, s, NULL, NULL);
        float tx = x0 + (x1 - x0) * jx + slide - ts.x * jx;
        if (w->bg[3] > 0.0f) {
            float px = 10.0f * k;
            ImDrawList_AddRectFilled(dl, (ImVec2){ tx - px, y - row * 0.45f }, (ImVec2){ tx + ts.x + px, y + row * 0.45f }, colf(w->bg, a), w->radius * k, 0);
        }
        text_at(dl, s, tx, y, size, m->color, a, 0.0f, 0.5f, w->bold, w->shadow);
    }
}

// --- Damage numbers: the Health's (health.h), in the widget's colors (style NULL: the Health's own look) ---
static void draw_numbers(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    static const float d_dmg[4] = { 1.0f, 0.86f, 0.32f, 1 }, d_shield[4] = { 0.45f, 0.78f, 1.0f, 1 }, d_heal[4] = { 0.4f, 1.0f, 0.5f, 1 },
                       d_crit[4] = { 1.0f, 0.42f, 0.12f, 1 };
    const float* c_dmg = w ? w->color : d_dmg;
    const float* c_shield = w ? w->bg : d_shield;
    const float* c_heal = w ? w->hover : d_heal;
    const float* c_crit = w ? w->press : d_crit;
    float base = w ? w->font_size : 34.0f, rise = w && w->value > 0.0f ? w->value : 70.0f;
    if (designer) {
        text_at(dl, "25", x0 + (x1 - x0) * 0.4f, y0 + (y1 - y0) * 0.4f, base * k, c_dmg, al, 0.5f, 0.5f, true, true);
        text_at(dl, "+10", x0 + (x1 - x0) * 0.55f, y0 + (y1 - y0) * 0.45f, base * k, c_heal, al, 0.5f, 0.5f, true, true);
        text_at(dl, "120!", x0 + (x1 - x0) * 0.5f, y0 + (y1 - y0) * 0.33f, base * k * 1.5f, c_crit, al, 0.5f, 0.5f, true, true);
        return;
    }
    const HudView* v = hud_view();
    if (!v->set || !health_numbers_active()) return;
    uint32_t n = 0;
    const HealthNumber* num = health_numbers(&n);
    for (uint32_t i = 0; i < n; i++) {
        const HealthNumber* h = &num[i];
        if (!(h->amount > 0.0f) || h->age > HEALTH_NUMBER_LIFE) continue;
        float t = h->age / HEALTH_NUMBER_LIFE;
        float p[3] = { h->pos[0], h->pos[1], h->pos[2] + t * rise }, xy[2], z;
        if (!hud_project(p, x0, y0, x1, y1, xy, &z) || z < 10.0f || xy[0] < x0 || xy[0] > x1 || xy[1] < y0 || xy[1] > y1) continue;
        char s[24];
        snprintf(s, sizeof s, h->kind == HEALTH_NUM_HEAL ? "+%.0f" : h->critical ? "%.0f!" : "%.0f", h->amount < 1.0f ? 1.0f : h->amount);
        float pop = t < 0.12f ? 1.0f + (0.12f - t) * (h->critical ? 6.0f : 3.0f) : 1.0f; // (a pop as it appears; bigger when critical)
        float size = fmaxf(base * k * pop * fminf(fmaxf(600.0f / z, 0.55f), 1.4f) * (h->critical ? 1.5f : 1.0f), 9.0f);
        float a = al * (t > 0.6f ? 1.0f - (t - 0.6f) / 0.4f : 1.0f);
        const float* c = h->kind == HEALTH_NUM_HEAL ? c_heal : h->critical ? c_crit : h->kind == HEALTH_NUM_SHIELD ? c_shield : c_dmg;
        text_at(dl, s, xy[0], xy[1], size, (const float[4]){ c[0], c[1], c[2], 1.0f }, a, 0.5f, 0.5f, h->critical != 0, true);
    }
}

// --- Crosshair ---
static void draw_crosshair(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float sp = designer ? 0.3f : hud_spread();
    float gap = (w->value + sp * w->vmax) * k, len = fmaxf(w->vmin * k, 1.0f), th = fmaxf(w->border_w * k, 1.0f);
    const float dir[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int pass = w->bold ? 0 : 1; pass < 2; pass++) { // (a dark outline under the lines)
        ImU32 c = pass ? colf(w->color, al) : colf(k_black, al * 0.55f);
        float t = pass ? th : th + 2.0f * fmaxf(k, 0.5f);
        for (int i = 0; i < 4; i++) {
            float ax = cx + dir[i][0] * gap, ay = cy + dir[i][1] * gap;
            ImDrawList_AddLine(dl, (ImVec2){ ax, ay }, (ImVec2){ ax + dir[i][0] * len, ay + dir[i][1] * len }, c, t);
        }
        if (w->checked) ImDrawList_AddCircleFilled(dl, (ImVec2){ cx, cy }, pass ? th * 0.9f : th * 0.9f + 1.0f, c, 12);
    }
}

// --- Screen markers: over the actor, or at the edge with an arrow ---
static void draw_screen_markers(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    float size = w->font_size * k, r = 9.0f * k, margin = fmaxf(w->value * k, r * 2.0f);
    if (designer) {
        diamond(dl, (x0 + x1) * 0.55f, (y0 + y1) * 0.45f, r, colf(w->color, al), colf(k_black, al * 0.6f));
        text_at(dl, "Objective", (x0 + x1) * 0.55f, (y0 + y1) * 0.45f - r * 1.6f, size, w->color, al, 0.5f, 1.0f, false, true);
        text_at(dl, "35 m", (x0 + x1) * 0.55f, (y0 + y1) * 0.45f + r * 1.5f, size * 0.8f, w->color, al, 0.5f, 0.0f, false, true);
        return;
    }
    const HudView* v = hud_view();
    if (!v->set) return;
    uint32_t n = 0;
    const HudMarker* m = hud_markers(&n);
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    for (uint32_t i = 0; i < n; i++) {
        if (!m[i].id || !(m[i].flags & HUD_MK_SCREEN)) continue;
        float xy[2], z;
        bool front = hud_project(m[i].pos, x0, y0, x1, y1, xy, &z);
        bool inside = front && xy[0] >= x0 + r && xy[0] <= x1 - r && xy[1] >= y0 + r && xy[1] <= y1 - r;
        const float* c = m[i].color;
        if (inside) {
            diamond(dl, xy[0], xy[1], r, colf(c, al), colf(k_black, al * 0.6f));
            if (m[i].label[0]) {
                const char* lb = gus_tr(m[i].label);
                text_atw(dl, lb, xy[0], xy[1] - r * 1.6f, size, c, al, 0.5f, 1.0f, false, true, label_w((int)i, &m[i], lb, size));
            }
            if (w->bold || (m[i].flags & HUD_MK_DISTANCE)) {
                float dw;
                const char* s = dist_w((int)i, &m[i], m[i].dist, size * 0.8f, &dw);
                text_atw(dl, s, xy[0], xy[1] + r * 1.5f, size * 0.8f, c, al * 0.9f, 0.5f, 0.0f, false, true, dw);
            }
            continue;
        }
        if (!w->checked || !(m[i].flags & HUD_MK_OFFSCREEN)) continue;
        // at the edge: along the direction from the middle, kept margin inside; an arrow pointing out
        float dx = xy[0] - cx, dy = xy[1] - cy;
        if (fabsf(dx) + fabsf(dy) < 1e-3f) dy = 1.0f;
        float hx = (x1 - x0) * 0.5f - margin, hy = (y1 - y0) * 0.5f - margin;
        float s = fminf(fabsf(dx) > 1e-3f ? hx / fabsf(dx) : 1e9f, fabsf(dy) > 1e-3f ? hy / fabsf(dy) : 1e9f);
        float ex = cx + dx * s, ey = cy + dy * s, len = sqrtf(dx * dx + dy * dy), ux = dx / len, uy = dy / len;
        diamond(dl, ex, ey, r * 0.85f, colf(c, al * 0.9f), colf(k_black, al * 0.6f));
        float ax = ex + ux * r * 1.9f, ay = ey + uy * r * 1.9f, bw = r * 0.75f;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ ax + ux * bw, ay + uy * bw }, (ImVec2){ ax - uy * bw, ay + ux * bw }, (ImVec2){ ax + uy * bw, ay - ux * bw }, colf(c, al));
        if (w->bold || (m[i].flags & HUD_MK_DISTANCE)) {
            float dw;
            const char* sd = dist_w((int)i, &m[i], m[i].dist, size * 0.75f, &dw);
            text_atw(dl, sd, ex - ux * r * 2.2f, ey - uy * r * 2.2f, size * 0.75f, c, al * 0.9f, 0.5f, 0.5f, false, true, dw);
        }
    }
}

// --- The damage's direction: arcs around the middle toward who hurt ---
static void draw_damage_dir(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rad = fminf(w->vmin * k, fminf(x1 - x0, y1 - y0) * 0.48f), th = fmaxf(w->border_w * k, 2.0f);
    float life = w->value > 0.05f ? w->value : 1.2f;
    float head = designer ? 0.0f : view_heading();
    const HudView* v = hud_view();
    uint32_t n = 0;
    const HudHit* hits = hud_hits(&n);
    for (uint32_t i = 0; i < (designer ? 1u : n); i++) {
        float rel, a;
        if (designer) rel = 60.0f, a = al;
        else {
            if (hits[i].age >= life || !v->set) continue;
            float d[3] = { hits[i].from[0] - v->pos[0], hits[i].from[1] - v->pos[1], 0.0f };
            rel = wrap180(hud_bearing(d) - head);
            a = al * (1.0f - hits[i].age / life);
        }
        float c = (rel - 90.0f) * DEG, half = 26.0f * DEG; // (ImGui's angles: 0 right, clockwise; up is -90)
        ImDrawList_PathClear(dl);
        ImDrawList_PathArcTo(dl, (ImVec2){ cx, cy }, rad, c - half, c + half, 20);
        ImDrawList_PathStroke(dl, colf(w->color, a), th, 0);
        // its tip: a small triangle out of the arc's middle, pointing the way
        float tip = rad + th * 1.4f, base = rad + th * 0.4f;
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx + cosf(c) * tip, cy + sinf(c) * tip }, (ImVec2){ cx + cosf(c - 0.06f) * base, cy + sinf(c - 0.06f) * base },
                                     (ImVec2){ cx + cosf(c + 0.06f) * base, cy + sinf(c + 0.06f) * base }, colf(w->color, a));
    }
}

// --- Minimap: the view from above around the player ---
typedef struct MapCtx { float cx, cy, R, scale, cr, sr, es, c0[2]; bool round; float k; } MapCtx;
// a world point onto the map (east right, north up, turned by the view's heading); false: past the range (kept at the edge)
static bool map_pt(const MapCtx* m, float wx, float wy, float* ox, float* oy) {
    float n = wx - m->c0[0], e = (wy - m->c0[1]) * m->es;
    float rx = (e * m->cr - n * m->sr) * m->scale, ry = (e * m->sr + n * m->cr) * m->scale, lim = m->R - 8.0f * m->k;
    bool in;
    if (m->round) {
        float l = sqrtf(rx * rx + ry * ry);
        in = l <= lim;
        if (!in) rx *= lim / l, ry *= lim / l;
    } else {
        in = fabsf(rx) <= lim && fabsf(ry) <= lim;
        rx = fminf(fmaxf(rx, -lim), lim), ry = fminf(fmaxf(ry, -lim), lim);
    }
    *ox = m->cx + rx, *oy = m->cy - ry;
    return in;
}
static void draw_minimap(ImDrawList* dl, const UiWidget* w, float x0, float y0, float x1, float y1, float k, float al, bool designer) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, R = fminf(x1 - x0, y1 - y0) * 0.5f;
    bool round = w->rounded != 0;
    if (round) ImDrawList_AddCircleFilled(dl, (ImVec2){ cx, cy }, R, colf(w->bg, al), 48);
    else ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, colf(w->bg, al), w->radius * k, 0);
    float range = w->value > 100.0f ? w->value : 3000.0f, scale = (R - 6.0f * k) / range;
    float head = designer ? 0.0f : view_heading();
    float rot = w->checked ? head : 0.0f; // (turning with the view: the view's way is up)
    World* world = designer ? NULL : hud_world();
    const HudView* v = hud_view();
    float c0[3] = { 0, 0, 0 };
    Entity pl = hud_player();
    const CTransform* pt = world && pl && ecs_alive(world, pl) ? ecs_get(world, pl, COMP_TRANSFORM) : NULL;
    if (pt) memcpy(c0, pt->location, 12);
    else if (v->set) memcpy(c0, v->pos, 12);
    MapCtx mc = { cx, cy, R, scale, cosf(rot * DEG), sinf(rot * DEG), hud_bearing((float[3]){ 0, 1, 0 }) > 180.0f ? -1.0f : 1.0f, { c0[0], c0[1] }, round, k };
    float px, py;
    bool in;
    // the actors with Health (not the player): dots
    for (uint32_t ai = 0; world && ai < world->archetype_count; ai++) {
        const EcsArchetype* a = &world->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const CHealth* hc = ecs_arch_column(a, COMP_HEALTH);
        const Entity* es = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (es[i] == pl || (hc[i].flags & HEALTH_DEAD)) continue;
            float dx = t[i].location[0] - c0[0], dy = t[i].location[1] - c0[1];
            if (dx * dx + dy * dy > range * range * 1.02f) continue;
            in = map_pt(&mc, t[i].location[0], t[i].location[1], &px, &py);
            ImDrawList_AddCircleFilled(dl, (ImVec2){ px, py }, 3.5f * k, colf(w->press, al), 8);
        }
    }
    // the compass and minimap markers (at the edge when far)
    uint32_t n = 0;
    const HudMarker* m = hud_markers(&n);
    for (uint32_t i = 0; !designer && i < n; i++) {
        if (!m[i].id || !(m[i].flags & (HUD_MK_COMPASS | HUD_MK_MINIMAP))) continue;
        in = map_pt(&mc, m[i].pos[0], m[i].pos[1], &px, &py);
        diamond(dl, px, py, 6.0f * k * (in ? 1.0f : 0.75f), colf(m[i].color, al), colf(k_black, al * 0.6f));
    }
    if (designer) diamond(dl, cx + R * 0.4f, cy - R * 0.3f, 6.0f * k, colf(w->hover, al), 0), ImDrawList_AddCircleFilled(dl, (ImVec2){ cx - R * 0.3f, cy + R * 0.2f }, 3.5f * k, colf(w->press, al), 8);
    // north on the edge; the player: an arrow in the middle (its way: up when the map turns)
    float na = (-90.0f - rot) * DEG, nr = R - 9.0f * k;
    text_at(dl, hud_cardinal(0), cx + cosf(na) * nr, cy + sinf(na) * nr, w->font_size * k, (const float[4]){ 1.0f, 0.35f, 0.3f, 1.0f }, al, 0.5f, 0.5f, true, true);
    float pa = (head - rot - 90.0f) * DEG, s = 9.0f * k;
    ImVec2 tip = { cx + cosf(pa) * s * 1.3f, cy + sinf(pa) * s * 1.3f };
    ImVec2 l = { cx + cosf(pa + 2.5f) * s, cy + sinf(pa + 2.5f) * s }, r = { cx + cosf(pa - 2.5f) * s, cy + sinf(pa - 2.5f) * s };
    ImDrawList_AddTriangleFilled(dl, tip, l, r, colf(w->color, al));
    if (w->border_w > 0.0f) {
        if (round) ImDrawList_AddCircle(dl, (ImVec2){ cx, cy }, R, colf(w->border_col, al), 48, fmaxf(w->border_w * k, 1.0f));
        else ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, colf(w->border_col, al), w->radius * k, fmaxf(w->border_w * k, 1.0f), 0);
    }
    (void)in;
}

void hud_widget_draw(ImDrawList* dl, const UiWidget* w, Entity owner, int index, float x0, float y0, float x1, float y1, float k, float al, bool designer, double now) {
    if (!dl || !w || (!designer && hud_piece_hidden(owner, w->kind))) return;
    double t0 = re_time_seconds();
    int v0 = dl->VtxBuffer.Size, i0 = dl->IdxBuffer.Size;
    switch (w->kind) {
    case UIW_ATTR_BAR: draw_bar(dl, w, owner, index, x0, y0, x1, y1, k, al, designer, now); break;
    case UIW_COMPASS: draw_compass(dl, w, x0, y0, x1, y1, k, al, designer); break;
    case UIW_MESSAGES: draw_messages(dl, w, x0, y0, x1, y1, k, al, designer); break;
    case UIW_DAMAGE_NUMBERS:
        draw_numbers(dl, w, x0, y0, x1, y1, k, al, designer);
        if (!designer) hud_numbers_drawn();
        break;
    case UIW_CROSSHAIR: draw_crosshair(dl, w, x0, y0, x1, y1, k, al, designer); break;
    case UIW_SCREEN_MARKERS: draw_screen_markers(dl, w, x0, y0, x1, y1, k, al, designer); break;
    case UIW_DAMAGE_DIR: draw_damage_dir(dl, w, x0, y0, x1, y1, k, al, designer); break;
    case UIW_MINIMAP: draw_minimap(dl, w, x0, y0, x1, y1, k, al, designer); break;
    default: return;
    }
    if (designer) {
        ImDrawList_AddRect(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y1 }, THA(TK_N15, 48), 0.0f, 1.0f, 0);
        return;
    }
    hud_draw_account(w->kind, (re_time_seconds() - t0) * 1000.0, (uint32_t)(dl->VtxBuffer.Size - v0), (uint32_t)(dl->IdxBuffer.Size - i0));
}

// The Health's numbers over the view when no Damage Numbers widget drew them (ui_widget_draw.c ui_draw_damage_numbers)
void hud_draw_numbers_default(ImDrawList* dl, float x0, float y0, float x1, float y1) {
    if (hud_numbers_take_drawn() || !hud_shown() || x1 <= x0 || y1 <= y0) return;
    double t0 = re_time_seconds();
    int v0 = dl->VtxBuffer.Size, i0 = dl->IdxBuffer.Size;
    draw_numbers(dl, NULL, x0, y0, x1, y1, (y1 - y0) / UI_REF_HEIGHT, 1.0f, false);
    if (dl->VtxBuffer.Size != v0) hud_draw_account(UIW_DAMAGE_NUMBERS, (re_time_seconds() - t0) * 1000.0, (uint32_t)(dl->VtxBuffer.Size - v0), (uint32_t)(dl->IdxBuffer.Size - i0));
}

// --- The Designer's Details of a piece ---
static bool check(const char* label, uint8_t* v) {
    bool b = *v != 0;
    if (!igCheckbox(label, &b)) return false;
    *v = b;
    return true;
}
bool hud_widget_details(UiWidget* w) {
    bool ch = false;
    switch (w->kind) {
    case UIW_ATTR_BAR: {
        ch |= igInputText(PL(TR("Attribute")), w->hint, sizeof w->hint, 0, NULL, NULL);
        ch |= igInputText(PL(TR("Actor (empty: the player)")), w->text, sizeof w->text, 0, NULL, NULL);
        igTextDisabled("%s", TR("Health and Shield: the actor's Health. Others: a Blueprint float variable (its max: Max<Name>) or a module's (Stamina, Mana)."));
        ch |= igSliderFloat(PL(TR("Percent")), &w->percent, 0.0f, 1.0f, "%.2f", 0);
        int o = w->orient > 2 ? 0 : w->orient;
        const char* os[3] = { TR("Left to Right"), TR("Right to Left"), TR("Bottom to Top") };
        if (igCombo_Str_arr(PL(TR("Fill Direction")), &o, os, 3, -1)) w->orient = (uint8_t)o, ch = true;
        ch |= igColorEdit4(PL(TR("Fill Color")), w->color, 0);
        ch |= igColorEdit4(PL(TR("Lost Chunk Color")), w->hover, 0);
        ch |= igColorEdit4(PL(TR("Gain Color")), w->press, 0);
        ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
        ch |= igDragFloat(PL(TR("Smoothing")), &w->value, 0.005f, 0.0f, 2.0f, "%.2f s", 0);
        ch |= igDragFloat(PL(TR("Lost Chunk Delay")), &w->vmin, 0.01f, 0.0f, 5.0f, "%.2f s", 0);
        ch |= igDragFloat(PL(TR("Lost Chunk Drain")), &w->padding, 0.01f, 0.05f, 10.0f, "%.2f / s", 0);
        ch |= igDragFloat(PL(TR("Low Pulse Below")), &w->vstep, 0.005f, 0.0f, 1.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Max (variables)")), &w->vmax, 1.0f, 0.0f, 1e7f, "%.0f", 0);
        int seg = w->columns;
        if (igInputInt(PL(TR("Segments")), &seg, 1, 1, 0)) w->columns = (uint8_t)(seg < 0 ? 0 : seg > 50 ? 50 : seg), ch = true;
        ch |= check(TR("Show Numbers"), &w->checked);
        igSameLine(0, -1);
        ch |= check(TR("Bold"), &w->bold);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
        break;
    }
    case UIW_COMPASS:
        ch |= igDragFloat(PL(TR("Degrees Shown")), &w->value, 1.0f, 30.0f, 360.0f, "%.0f", 0);
        ch |= check(TR("Show Distances"), &w->checked);
        igSameLine(0, -1);
        ch |= check(TR("Bold"), &w->bold);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
        ch |= igColorEdit4(PL(TR("Text Color")), w->color, 0);
        ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
        igTextDisabled("%s", TR("Markers: Add Compass Marker (an actor, a label, a color). North is +X."));
        break;
    case UIW_MESSAGES: {
        int c = w->columns;
        if (igInputInt(PL(TR("Max Shown")), &c, 1, 1, 0)) w->columns = (uint8_t)(c < 1 ? 1 : c > HUD_MSG_MAX ? HUD_MSG_MAX : c), ch = true;
        ch |= igDragFloat(PL(TR("Duration")), &w->value, 0.05f, 0.2f, 60.0f, "%.1f s", 0);
        int o = w->orient == 1;
        const char* os[2] = { TR("Newest at the Bottom"), TR("Newest on Top") };
        if (igCombo_Str_arr(PL(TR("Order")), &o, os, 2, -1)) w->orient = (uint8_t)o, ch = true;
        ch |= igSliderFloat(PL(TR("Justification")), &w->align[0], 0.0f, 1.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
        ch |= igColorEdit4(PL(TR("Row Background")), w->bg, 0);
        ch |= check(TR("Bold"), &w->bold);
        igSameLine(0, -1);
        ch |= check(TR("Shadow"), &w->shadow);
        igTextDisabled("%s", TR("Show HUD Message adds one; the same text stacks (\"+1 Coin\" twice: \"+2 Coin\"), the rest wait their turn."));
        break;
    }
    case UIW_DAMAGE_NUMBERS:
        ch |= igColorEdit4(PL(TR("Damage Color")), w->color, 0);
        ch |= igColorEdit4(PL(TR("Shield Color")), w->bg, 0);
        ch |= igColorEdit4(PL(TR("Heal Color")), w->hover, 0);
        ch |= igColorEdit4(PL(TR("Critical Color")), w->press, 0);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 200.0f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Rise")), &w->value, 1.0f, 0.0f, 1000.0f, "%.0f cm", 0);
        igTextDisabled("%s", TR("The Health's numbers (Damage Numbers on) and Show Damage Number's. Without this widget they show in the Health's colors."));
        break;
    case UIW_CROSSHAIR:
        ch |= igDragFloat(PL(TR("Gap")), &w->value, 0.25f, 0.0f, 200.0f, "%.1f", 0);
        ch |= igDragFloat(PL(TR("Line Length")), &w->vmin, 0.25f, 1.0f, 200.0f, "%.1f", 0);
        ch |= igDragFloat(PL(TR("Max Spread")), &w->vmax, 0.25f, 0.0f, 400.0f, "%.1f", 0);
        ch |= igDragFloat(PL(TR("Thickness")), &w->border_w, 0.1f, 0.5f, 20.0f, "%.1f", 0);
        ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0);
        ch |= check(TR("Center Dot"), &w->checked);
        igSameLine(0, -1);
        ch |= check(TR("Dark Outline"), &w->bold);
        igTextDisabled("%s", TR("Opens with Set Crosshair Spread (0-1) and the player's speed (HUD Layout)."));
        break;
    case UIW_SCREEN_MARKERS:
        ch |= igDragFloat(PL(TR("Edge Margin")), &w->value, 0.5f, 0.0f, 400.0f, "%.0f", 0);
        ch |= check(TR("Show Off Screen"), &w->checked);
        igSameLine(0, -1);
        ch |= check(TR("Show Distances"), &w->bold);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
        igTextDisabled("%s", TR("Markers: Add Screen Marker. Out of the view they wait at the edge with an arrow."));
        break;
    case UIW_DAMAGE_DIR:
        ch |= igColorEdit4(PL(TR("Color and Opacity")), w->color, 0);
        ch |= igDragFloat(PL(TR("Radius")), &w->vmin, 0.5f, 10.0f, 1000.0f, "%.0f", 0);
        ch |= igDragFloat(PL(TR("Thickness")), &w->border_w, 0.1f, 1.0f, 60.0f, "%.1f", 0);
        ch |= igDragFloat(PL(TR("Fade Time")), &w->value, 0.05f, 0.1f, 10.0f, "%.2f s", 0);
        break;
    case UIW_MINIMAP:
        ch |= igDragFloat(PL(TR("Range")), &w->value, 10.0f, 200.0f, 1e6f, "%.0f cm", 0);
        ch |= check(TR("Turn with the View"), &w->checked);
        igSameLine(0, -1);
        ch |= check(TR("Round"), &w->rounded);
        ch |= igColorEdit4(PL(TR("Player Color")), w->color, 0);
        ch |= igColorEdit4(PL(TR("Actors with Health")), w->press, 0);
        ch |= igColorEdit4(PL(TR("Background Color")), w->bg, 0);
        ch |= igDragFloat(PL(TR("Font Size")), &w->font_size, 0.5f, 6.0f, 100.0f, "%.0f", 0);
        break;
    default: break;
    }
    if (w->kind == UIW_ATTR_BAR || w->kind == UIW_COMPASS || w->kind == UIW_MESSAGES || w->kind == UIW_MINIMAP) {
        ch |= igDragFloat(PL(TR("Corner Radius")), &w->radius, 0.25f, 0.0f, 500.0f, "%.0f", 0);
        if (w->kind == UIW_ATTR_BAR || w->kind == UIW_MINIMAP) {
            ch |= igDragFloat(PL(TR("Outline Width")), &w->border_w, 0.1f, 0.0f, 50.0f, "%.1f", 0);
            if (w->border_w > 0.0f) ch |= igColorEdit4(PL(TR("Outline Color")), w->border_col, 0);
        }
    }
    return ch;
}

// --- The HUD Layout component's Details ---
static bool flag(const char* label, uint32_t* f, uint32_t bit) {
    bool b = (*f & bit) != 0;
    if (!igCheckbox(label, &b)) return false;
    *f = b ? *f | bit : *f & ~bit;
    return true;
}
bool hud_ui_layout(CHudLayout* c) {
    bool ch = false;
    const BpAsset* cur = c->widget ? bp_find_hash(c->widget) : NULL;
    if (igBeginCombo(PL(TR("HUD Widget")), cur ? cur->name : "WBP_GameHUD", 0)) {
        if (igSelectable_Bool("WBP_GameHUD", !c->widget, 0, (ImVec2){ 0, 0 })) c->widget = 0, ch = true;
        for (uint32_t i = 0; i < bp_asset_count(); i++) {
            const BpAsset* a = bp_asset_at(i);
            if (!a->ui || !strcmp(a->name, "WBP_GameHUD")) continue;
            if (igSelectable_Bool(a->name, a->hash == c->widget, 0, (ImVec2){ 0, 0 })) c->widget = a->hash, ch = true;
        }
        igEndCombo();
    }
    ch |= flag(TR("Show When Play Starts"), &c->flags, HUD_LAYOUT_VISIBLE);
    int m = c->scale_mode < HUD_SCALE_COUNT ? (int)c->scale_mode : 0;
    const char* ms[HUD_SCALE_COUNT];
    for (int i = 0; i < HUD_SCALE_COUNT; i++) ms[i] = TR(g_hud_scale_modes[i]);
    if (igCombo_Str_arr(PL(TR("Scale By")), &m, ms, HUD_SCALE_COUNT, -1)) c->scale_mode = (uint32_t)m, ch = true;
    ch |= igDragFloat(PL(TR("HUD Scale")), &c->scale, 0.01f, 0.1f, 8.0f, "%.2f", 0);
    float lim[2] = { c->min_scale, c->max_scale };
    if (igDragFloat2(PL(TR("Scale Limits (0: none)")), lim, 0.01f, 0.0f, 8.0f, "%.2f", 0)) c->min_scale = lim[0], c->max_scale = lim[1], ch = true;
    ch |= igDragFloat4(PL(TR("Safe Margins % (L T R B)")), c->safe, 0.1f, 0.0f, 40.0f, "%.1f", 0);
    ch |= flag(TR("Device Safe Area"), &c->flags, HUD_LAYOUT_SAFE_AREA);
    ch |= flag(TR("Hide in Photo Mode"), &c->flags, HUD_LAYOUT_HIDE_PHOTO);
    ch |= flag(TR("Hide in Sequences"), &c->flags, HUD_LAYOUT_HIDE_SEQUENCE);
    ch |= flag(TR("Speed Opens Crosshair"), &c->flags, HUD_LAYOUT_SPEED_SPREAD);
    ch |= igInputInt(PL(TR("Z Order")), &c->zorder, 1, 10, 0);
    if (igTreeNodeEx_Str(TR("Pieces Shown"), ImGuiTreeNodeFlags_DefaultOpen)) {
        static const struct { const char* name; uint32_t bit; } p[] = {
            { "Attribute Bars", HUD_PIECE_BARS }, { "Compass", HUD_PIECE_COMPASS }, { "Message Feed", HUD_PIECE_MESSAGES },
            { "Damage Numbers", HUD_PIECE_NUMBERS }, { "Crosshair", HUD_PIECE_CROSSHAIR }, { "Screen Markers", HUD_PIECE_MARKERS },
            { "Damage Direction", HUD_PIECE_DAMAGE_DIR }, { "Minimap", HUD_PIECE_MINIMAP },
        };
        for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
            if (i & 1) igSameLine(igGetContentRegionAvail().x * 0.5f + igGetCursorPosX() * 0.0f + 140.0f, -1);
            ch |= flag(TR(p[i].name), &c->pieces, p[i].bit);
        }
        igTreePop();
    }
    return ch;
}
