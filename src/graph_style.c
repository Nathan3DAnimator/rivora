// The graph editors' look (graph_style.h): node and wire styles, their previews
#include "graph_style.h"

#include "theme.h"
#include "ui_icons.h"
#include "ui_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#ifndef IM_COL32
#define IM_COL32(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#endif

static int g_node_style = GS_NODE_DEFAULT_STYLE, g_link_style = GS_LINK_DEFAULT_STYLE;

int graph_node_style(void) { return g_node_style; }
int graph_link_style(void) { return g_link_style; }
void graph_set_styles(int node_style, int link_style) {
    g_node_style = node_style >= 0 && node_style < GS_NODE_COUNT ? node_style : GS_NODE_DEFAULT_STYLE;
    g_link_style = link_style >= 0 && link_style < GS_LINK_COUNT ? link_style : GS_LINK_DEFAULT_STYLE;
}
void graph_set_styles_ini(int node_style, int link_style, bool v2) {
    if (!v2) { // (an editor.ini from before Q5: the first style was the only default, now Órbita; a choice stays)
        if (node_style == GS_NODE_DEFAULT) node_style = GS_NODE_DEFAULT_STYLE;
        if (link_style == GS_LINK_CURVE) link_style = GS_LINK_DEFAULT_STYLE;
    }
    graph_set_styles(node_style, link_style);
}
bool graph_orbit(void) { return g_node_style == GS_NODE_ORBIT; }

static const char* const g_node_names[GS_NODE_COUNT] = { "Classic", "Flat", "Minimal", "Glass", "Neon", "Orbit" };
static const char* const g_link_names[GS_LINK_COUNT] = { "Curve", "Straight", "Circuit", "Glow", "Flow", "Gradient" };
const char* graph_node_style_name(int s) { return s >= 0 && s < GS_NODE_COUNT ? g_node_names[s] : ""; }
const char* graph_link_style_name(int s) { return s >= 0 && s < GS_LINK_COUNT ? g_link_names[s] : ""; }

static void header_grad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, uint32_t ca, uint32_t cb);
static uint32_t with_alpha(uint32_t c, uint32_t a) { return (c & 0x00FFFFFFu) | (a << 24); }
static uint32_t scale_c(uint32_t c, float k) {
    uint32_t r = (uint32_t)fminf((float)(c & 255) * k, 255.0f), g = (uint32_t)fminf((float)((c >> 8) & 255) * k, 255.0f);
    uint32_t b = (uint32_t)fminf((float)((c >> 16) & 255) * k, 255.0f);
    return (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

// A wire's points (the shape every style follows): the curve's (Curve, Glow, Flow), the straight line, the circuit's
// right angles; n points into out (at most 33)
static int wire_points(ImVec2 a, ImVec2 b, float z, int style, ImVec2* out) {
    if (style == GS_LINK_STRAIGHT) {
        out[0] = a, out[1] = b;
        return 2;
    }
    if (style == GS_LINK_CIRCUIT) { // out, across at the middle, in (a step back when the input is behind)
        float stub = 14.0f * z;
        if (b.x - a.x > 2.0f * stub) {
            float mx = (a.x + b.x) * 0.5f;
            out[0] = a, out[1] = (ImVec2){ mx, a.y }, out[2] = (ImVec2){ mx, b.y }, out[3] = b;
            return 4;
        }
        float my = (a.y + b.y) * 0.5f;
        out[0] = a, out[1] = (ImVec2){ a.x + stub, a.y }, out[2] = (ImVec2){ a.x + stub, my }, out[3] = (ImVec2){ b.x - stub, my };
        out[4] = (ImVec2){ b.x - stub, b.y }, out[5] = b;
        return 6;
    }
    float dx = fmaxf(fabsf(b.x - a.x) * 0.5f, 40.0f * z);
    ImVec2 c1 = { a.x + dx, a.y }, c2 = { b.x - dx, b.y };
    for (int i = 0; i <= 32; i++) {
        float t = (float)i / 32.0f, u = 1.0f - t;
        float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        out[i] = (ImVec2){ w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y };
    }
    return 33;
}

void graph_wire(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t col, float z, bool exec, int style) {
    if (style < 0) style = g_link_style;
    float th = (exec ? 3.2f : 2.4f) * z;
    ImVec2 pts[33];
    int n = wire_points(a, b, z, style, pts);
    switch (style) {
    case GS_LINK_GRADIENT:
        graph_wire2(dl, a, b, col, col, z, exec, style);
        break;
    default:
    case GS_LINK_CURVE:
    case GS_LINK_STRAIGHT:
    case GS_LINK_CIRCUIT:
        ImDrawList_AddPolyline(dl, pts, n, THA(TK_N0, 90), th + 2.5f * z, 0); // a dark edge: wires read over anything
        ImDrawList_AddPolyline(dl, pts, n, col, th, 0);
        break;
    case GS_LINK_GLOW: // a wide soft halo of its color under a bright thin core
        ImDrawList_AddPolyline(dl, pts, n, with_alpha(col, 40), th * 4.0f, 0);
        ImDrawList_AddPolyline(dl, pts, n, with_alpha(col, 90), th * 2.2f, 0);
        ImDrawList_AddPolyline(dl, pts, n, scale_c(col, 1.35f), th * 0.8f, 0);
        break;
    case GS_LINK_FLOW: { // a faint wire with dashes moving along it, the way the value (the execution) goes
        ImDrawList_AddPolyline(dl, pts, n, with_alpha(col, 70), th, 0);
        float len = 0.0f, seg[33];
        for (int i = 1; i < n; i++) seg[i] = sqrtf((pts[i].x - pts[i - 1].x) * (pts[i].x - pts[i - 1].x) + (pts[i].y - pts[i - 1].y) * (pts[i].y - pts[i - 1].y)), len += seg[i];
        float period = 18.0f * z, dash = 9.0f * z, off = fmodf((float)igGetTime() * 40.0f * z, period), at = 0.0f;
        for (int i = 1; i < n; i++) {
            float s0 = at, s1 = at + seg[i];
            for (float d = floorf((s0 - off) / period) * period + off; d < s1; d += period) {
                float d0 = fmaxf(d, s0), d1 = fminf(d + dash, s1);
                if (d1 <= d0 || seg[i] <= 0.0f) continue;
                float t0 = (d0 - s0) / seg[i], t1 = (d1 - s0) / seg[i];
                ImVec2 q0 = { pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t0, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t0 };
                ImVec2 q1 = { pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t1, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t1 };
                ImDrawList_AddLine(dl, q0, q1, col, th);
            }
            at = s1;
        }
        (void)len;
        break;
    }
    }
}

float graph_wire_dist(ImVec2 a, ImVec2 b, ImVec2 m, float z, int style) {
    if (style < 0) style = g_link_style;
    ImVec2 pts[33];
    int n = wire_points(a, b, z, style, pts);
    float best = 1e30f;
    for (int i = 1; i < n; i++) { // (to each segment)
        float ex = pts[i].x - pts[i - 1].x, ey = pts[i].y - pts[i - 1].y, l2 = ex * ex + ey * ey;
        float t = l2 > 0.0f ? fminf(fmaxf(((m.x - pts[i - 1].x) * ex + (m.y - pts[i - 1].y) * ey) / l2, 0.0f), 1.0f) : 0.0f;
        float dx = pts[i - 1].x + ex * t - m.x, dy = pts[i - 1].y + ey * t - m.y;
        best = fminf(best, dx * dx + dy * dy);
    }
    return sqrtf(best);
}

void graph_node_frame(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float header_h, float r, uint32_t header_col, uint32_t border_col,
                      float border_w, float z, int style) {
    ImVec2 h1 = { p1.x, p0.y + header_h };
    switch (style) {
    case GS_NODE_FLAT: // square corners, a solid header, no gradient
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N5, 250), 2.0f * z, 0);
        ImDrawList_AddRectFilled(dl, p0, h1, scale_c(header_col, 0.85f), 2.0f * z, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddRect(dl, p0, p1, border_col, 2.0f * z, border_w, 0);
        break;
    case GS_NODE_MINIMAL: // a dark see-through body, no header band: a colored line on top
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N3, 200), r, 0);
        ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p1.x, p0.y + 3.0f * z }, with_alpha(header_col, 255), r, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddLine(dl, (ImVec2){ p0.x + 8 * z, h1.y }, (ImVec2){ p1.x - 8 * z, h1.y }, THA(TK_N15, 25), 1.0f);
        ImDrawList_AddRect(dl, p0, p1, with_alpha(border_col, 160), r, border_w, 0);
        break;
    case GS_NODE_GLASS: // a frosted see-through body and header, rounder, a bright rim
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_BLUE_D, 120), 11.0f * z, 0);
        ImDrawList_AddRectFilled(dl, p0, h1, with_alpha(header_col, 130), 11.0f * z, ImDrawFlags_RoundCornersTop);
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ p0.x + 2, p0.y + 2 }, (ImVec2){ p1.x - 2, p0.y + header_h * 0.5f }, THA(TK_N15, 40),
                                           THA(TK_N15, 40), THA(TK_N15, 0), THA(TK_N15, 0));
        ImDrawList_AddRect(dl, p0, p1, THA(TK_N15, 70), 11.0f * z, 1.0f, 0);
        if (border_w > 1.5f) ImDrawList_AddRect(dl, p0, p1, border_col, 11.0f * z, border_w, 0);
        break;
    case GS_NODE_NEON: { // black, its header color glowing around it
        uint32_t glow = scale_c(header_col, 1.6f);
        for (int k = 3; k >= 1; k--)
            ImDrawList_AddRect(dl, (ImVec2){ p0.x - k * 1.5f * z, p0.y - k * 1.5f * z }, (ImVec2){ p1.x + k * 1.5f * z, p1.y + k * 1.5f * z },
                               with_alpha(glow, (uint32_t)(28 * (4 - k))), r + k * 1.5f * z, 1.5f * z, 0);
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N1, 245), r, 0);
        ImDrawList_AddLine(dl, (ImVec2){ p0.x + 6 * z, h1.y }, (ImVec2){ p1.x - 6 * z, h1.y }, with_alpha(glow, 200), 1.5f * z);
        ImDrawList_AddRect(dl, p0, p1, border_w > 1.5f ? border_col : with_alpha(glow, 230), r, border_w > 1.5f ? border_w : 1.4f * z, 0);
        break;
    }
    case GS_NODE_ORBIT: { // (Q5) a card: 8 px corners, the body a raised surface, the header a band of the family's color
        float rr = TMV(TM_RADIUS_NODE) * z;
        if (rr <= 0.0f) rr = 8.0f * z;
        if (rr > header_h && header_h > 0.0f) rr = header_h;
        bool loud = border_w > 1.5f; // (selected, an error: the halo of border_col)
        if (loud)
            for (int k = 3; k >= 1; k--) {
                float e = (float)k * 1.7f * z;
                ImDrawList_AddRect(dl, (ImVec2){ p0.x - e, p0.y - e }, (ImVec2){ p1.x + e, p1.y + e }, with_alpha(border_col, (uint32_t)(22 * (4 - k))),
                                   rr + e, 1.8f * z, 0);
            }
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_BG_RAISED, 248), rr, 0);
        if (header_h > 0.0f) {
            header_grad(dl, p0, h1, rr, theme_mix(header_col, TH(TK_BG_RAISED), 0.08f), theme_mix(header_col, TH(TK_BG_RAISED), 0.50f));
            ImDrawList_AddLine(dl, (ImVec2){ p0.x + rr * 0.7f, p0.y + 0.8f * z }, (ImVec2){ p1.x - rr * 0.7f, p0.y + 0.8f * z },
                               with_alpha(scale_c(header_col, 1.35f), 120), 1.0f * z); // (the light's edge)
            ImDrawList_AddLine(dl, (ImVec2){ p0.x, h1.y }, (ImVec2){ p1.x, h1.y }, with_alpha(header_col, 230), 1.4f * z);
        }
        ImDrawList_AddRect(dl, p0, p1, loud ? border_col : THA(TK_BORDER_STRONG, 230), rr, loud ? 1.6f * z : 1.0f, 0);
        break;
    }
    default: // (the default: the editor's own; a plain stand-in for the previews)
        ImDrawList_AddRectFilled(dl, p0, p1, THA(TK_N3, 238), r, 0);
        ImDrawList_AddRectFilledMultiColor(dl, p0, h1, header_col, scale_c(header_col, 0.45f), scale_c(header_col, 0.45f), header_col);
        ImDrawList_AddLine(dl, (ImVec2){ p0.x + 1, h1.y }, (ImVec2){ p1.x - 1, h1.y }, THA(TK_N0, 140), 1.0f);
        ImDrawList_AddRect(dl, p0, p1, border_col, r, border_w, 0);
        break;
    }
}

void graph_style_preview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int node_style, int link_style) {
    float w = p1.x - p0.x, h = p1.y - p0.y, z = fmaxf(fminf(w / 220.0f, h / 120.0f), 0.3f);
    int keep = g_node_style;
    g_node_style = node_style; // (the background and the pins of the style shown)
    graph_background(dl, p0, (ImVec2){ w, h }, 10.0f, 10.0f, z, node_style == GS_NODE_ORBIT ? 0 : TH(TK_PREVIEW_BOTTOM));
    // an event (Ignition) into a function (blue), and a value (green) into it
    float nw = w * 0.36f, nh = h * 0.42f, hh = 15.0f * z;
    ImVec2 a0 = { p0.x + w * 0.06f, p0.y + h * 0.14f }, a1 = { a0.x + nw, a0.y + nh * 0.75f };
    ImVec2 b0 = { p0.x + w * 0.56f, p0.y + h * 0.40f }, b1 = { b0.x + nw, b0.y + nh };
    ImVec2 c0 = { p0.x + w * 0.10f, p0.y + h * 0.66f }, c1 = { c0.x + nw * 0.7f, c0.y + h * 0.20f };
    ImVec2 ea = { a1.x - 6 * z, a0.y + hh + 8 * z }, eb = { b0.x + 6 * z, b0.y + hh + 8 * z };
    ImVec2 va = { c1.x - 4 * z, (c0.y + c1.y) * 0.5f }, vb = { b0.x + 6 * z, b0.y + hh + 20 * z };
    graph_wire2(dl, ea, eb, TH(TK_PIN_EXEC), TH(TK_PIN_EXEC), z, true, link_style);
    graph_wire2(dl, va, vb, TH(TK_PIN_FLOAT), TH(TK_PIN_INT), z, false, link_style);
    float r = 6.0f * z;
    uint32_t sel = node_style == GS_NODE_ORBIT ? TH(TK_ACCENT) : TH(TK_AMBER);
    graph_node_frame(dl, a0, a1, hh, r, TH(TK_NODE_EVENT), TH(TK_N8), 1.0f, z, node_style);
    graph_node_frame(dl, b0, b1, hh, r, TH(TK_NODE_FUNCTION), sel, 2.0f * z, z, node_style); // (selected)
    if (node_style == GS_NODE_ORBIT) {
        float is = hh * 0.7f;
        ui_icon(dl, ICON_EVENT, a0.x + 5 * z, a0.y + (hh - is) * 0.5f, is, TH(TK_NODE_TITLE));
        ui_icon(dl, ICON_FUNCTION, b0.x + 5 * z, b0.y + (hh - is) * 0.5f, is, TH(TK_NODE_TITLE));
    }
    { // a getter pill
        ImDrawList_AddRectFilled(dl, c0, c1, THA(TK_N4, 240), (c1.y - c0.y) * 0.5f, 0);
        ImDrawList_AddRectFilled(dl, c0, c1, theme_alpha(TH(TK_PIN_FLOAT), 70), (c1.y - c0.y) * 0.5f, 0);
    }
    // the pins
    if (node_style == GS_NODE_ORBIT) {
        graph_pin(dl, ea, GP_EXEC, TH(TK_PIN_EXEC), 0, true, z);
        graph_pin(dl, eb, GP_EXEC, TH(TK_PIN_EXEC), 0, true, z);
        graph_pin(dl, va, GP_VALUE, TH(TK_PIN_FLOAT), 0, true, z);
        graph_pin(dl, vb, GP_VALUE, TH(TK_PIN_INT), 0, true, z);
        graph_pin(dl, (ImVec2){ b0.x + 6 * z, b0.y + hh + 32 * z }, GP_ARRAY, TH(TK_PIN_STRING), 0, false, z);
    } else {
        ImDrawList_AddCircleFilled(dl, ea, 3.0f * z, TH(TK_N15), 8);
        ImDrawList_AddCircleFilled(dl, eb, 3.0f * z, TH(TK_N15), 8);
        ImDrawList_AddCircleFilled(dl, vb, 3.0f * z, TH(TK_LIME), 8);
    }
    g_node_style = keep;
}

// --- Q5: the Orbit look (graph_style.h) ---

// A thick line through n points, its color going from ca to cb along it, with a soft edge of 1 px (one strip, four
// vertices a point: no seam between the segments)
static void gradient_strip(ImDrawList* dl, const ImVec2* p, int n, uint32_t ca, uint32_t cb, float th) {
    if (n < 2) return;
    ImVec2 uv = igGetFontTexUvWhitePixel();
    float hw = fmaxf(th * 0.5f - 0.5f, 0.25f), aa = 1.0f;
    float seg[33], len = 0.0f, acc = 0.0f;
    for (int i = 1; i < n; i++) seg[i] = sqrtf((p[i].x - p[i - 1].x) * (p[i].x - p[i - 1].x) + (p[i].y - p[i - 1].y) * (p[i].y - p[i - 1].y)), len += seg[i];
    if (len <= 0.0f) len = 1.0f;
    ImDrawList_PrimReserve(dl, (n - 1) * 18, n * 4);
    unsigned base = dl->_VtxCurrentIdx;
    for (int i = 0; i < n; i++) {
        if (i) acc += seg[i];
        ImVec2 d0 = p[i > 0 ? i - 1 : 0], d1 = p[i + 1 < n ? i + 1 : n - 1];
        float dx = d1.x - d0.x, dy = d1.y - d0.y, l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-6f) dx = 1.0f, dy = 0.0f, l = 1.0f;
        float nx = -dy / l, ny = dx / l;
        uint32_t c = theme_mix(ca, cb, acc / len), c0 = c & 0x00FFFFFFu;
        ImDrawList_PrimWriteVtx(dl, (ImVec2){ p[i].x + nx * (hw + aa), p[i].y + ny * (hw + aa) }, uv, c0);
        ImDrawList_PrimWriteVtx(dl, (ImVec2){ p[i].x + nx * hw, p[i].y + ny * hw }, uv, c);
        ImDrawList_PrimWriteVtx(dl, (ImVec2){ p[i].x - nx * hw, p[i].y - ny * hw }, uv, c);
        ImDrawList_PrimWriteVtx(dl, (ImVec2){ p[i].x - nx * (hw + aa), p[i].y - ny * (hw + aa) }, uv, c0);
    }
    for (int i = 0; i + 1 < n; i++)
        for (int k = 0; k < 3; k++) {
            unsigned a = base + (unsigned)(i * 4 + k), b = a + 1, c = a + 5, d = a + 4;
            ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)a), ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)b), ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)c);
            ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)a), ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)c), ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)d);
        }
}

// A header band: rounded on top (r), its color ca at the left going to cb at the right
static void header_grad(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, uint32_t ca, uint32_t cb) {
    ImDrawList_PathClear(dl);
    ImDrawList_PathLineTo(dl, (ImVec2){ p0.x, p1.y });
    ImDrawList_PathArcToFast(dl, (ImVec2){ p0.x + r, p0.y + r }, r, 6, 9);
    ImDrawList_PathArcToFast(dl, (ImVec2){ p1.x - r, p0.y + r }, r, 9, 12);
    ImDrawList_PathLineTo(dl, (ImVec2){ p1.x, p1.y });
    int n = dl->_Path.Size;
    ImVec2 uv = igGetFontTexUvWhitePixel();
    ImDrawList_PrimReserve(dl, (n - 2) * 3, n);
    unsigned base = dl->_VtxCurrentIdx;
    float w = fmaxf(p1.x - p0.x, 1.0f);
    for (int i = 0; i < n; i++) {
        ImVec2 q = dl->_Path.Data[i];
        ImDrawList_PrimWriteVtx(dl, q, uv, theme_mix(ca, cb, (q.x - p0.x) / w));
    }
    for (int i = 1; i + 1 < n; i++) {
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)base);
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + i));
        ImDrawList_PrimWriteIdx(dl, (ImDrawIdx)(base + i + 1));
    }
    ImDrawList_PathClear(dl);
}

void graph_wire2(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t ca, uint32_t cb, float z, bool exec, int style) {
    if (style < 0) style = g_link_style;
    if (style != GS_LINK_GRADIENT) {
        graph_wire(dl, a, b, ca, z, exec, style);
        return;
    }
    float th = (exec ? 3.0f : 2.2f) * z;
    ImVec2 pts[33];
    int n = wire_points(a, b, z, GS_LINK_CURVE, pts);
    ImDrawList_AddPolyline(dl, pts, n, THA(TK_N0, 80), th + 2.2f * z, 0); // (a dark edge: reads over anything)
    gradient_strip(dl, pts, n, ca, cb, th);
}

void graph_pin(ImDrawList* dl, ImVec2 c, int shape, uint32_t col, uint32_t col2, bool linked, float z) {
    float r = 4.6f * z, lw = 1.6f * z;
    uint32_t hole = TH(TK_BG_FIELD);
    switch (shape) {
    case GP_EXEC: { // the chevron: the way the execution goes
        float e = 5.6f * z;
        ImVec2 pts[5] = { { c.x - e, c.y - e }, { c.x + e * 0.1f, c.y - e }, { c.x + e * 1.05f, c.y }, { c.x + e * 0.1f, c.y + e }, { c.x - e, c.y + e } };
        if (!linked) ImDrawList_AddConvexPolyFilled(dl, pts, 5, hole);
        if (linked) ImDrawList_AddConvexPolyFilled(dl, pts, 5, col);
        else ImDrawList_AddPolyline(dl, pts, 5, col, lw, ImDrawFlags_Closed);
        break;
    }
    case GP_ARRAY: { // a bar of three segments
        float w = r * 1.05f, h = r * 0.62f, g = r * 0.22f;
        for (int k = -1; k <= 1; k++) {
            ImVec2 q0 = { c.x - w, c.y + (float)k * (h + g) - h * 0.5f }, q1 = { c.x + w, c.y + (float)k * (h + g) + h * 0.5f };
            if (linked || k == 0) ImDrawList_AddRectFilled(dl, q0, q1, col, 1.0f * z, 0);
            else ImDrawList_AddRect(dl, q0, q1, col, 1.0f * z, 1.2f * z, 0);
        }
        break;
    }
    case GP_SET: // a double ring
        ImDrawList_AddCircle(dl, c, r * 1.15f, col, 18, lw);
        if (linked) ImDrawList_AddCircleFilled(dl, c, r * 0.55f, col, 14);
        else ImDrawList_AddCircle(dl, c, r * 0.55f, col, 14, lw * 0.9f);
        break;
    case GP_MAP: { // two circles linked: the key's color and the value's
        ImVec2 k = { c.x - r * 0.75f, c.y }, v = { c.x + r * 0.75f, c.y };
        ImDrawList_AddLine(dl, k, v, theme_mix(col, col2, 0.5f), lw);
        if (linked) ImDrawList_AddCircleFilled(dl, k, r * 0.62f, col, 12), ImDrawList_AddCircleFilled(dl, v, r * 0.62f, col2, 12);
        else {
            ImDrawList_AddCircleFilled(dl, k, r * 0.62f, hole, 12), ImDrawList_AddCircle(dl, k, r * 0.62f, col, 12, lw * 0.85f);
            ImDrawList_AddCircleFilled(dl, v, r * 0.62f, hole, 12), ImDrawList_AddCircle(dl, v, r * 0.62f, col2, 12, lw * 0.85f);
        }
        break;
    }
    case GP_CLASS: // a hollow ring (thick); a dot inside when linked
        ImDrawList_AddCircle(dl, c, r * 0.85f, col, 18, 2.4f * z);
        if (linked) ImDrawList_AddCircleFilled(dl, c, r * 0.3f, col, 8);
        break;
    case GP_SOFT: // a dashed ring: a soft reference (loaded when asked)
        for (int k = 0; k < 6; k++) {
            float a0 = (float)k * 1.0471976f;
            ImDrawList_PathArcTo(dl, c, r, a0, a0 + 0.6f, 5);
            ImDrawList_PathStroke(dl, col, lw, 0);
        }
        if (linked) ImDrawList_AddCircleFilled(dl, c, r * 0.5f, col, 10);
        break;
    case GP_DELEGATE: { // a small square
        float h = 4.2f * z;
        if (linked) ImDrawList_AddRectFilled(dl, (ImVec2){ c.x - h, c.y - h }, (ImVec2){ c.x + h, c.y + h }, col, 1.5f * z, 0);
        else ImDrawList_AddRect(dl, (ImVec2){ c.x - h, c.y - h }, (ImVec2){ c.x + h, c.y + h }, col, 1.5f * z, lw, 0);
        break;
    }
    default: // a value: a circle
        if (linked) ImDrawList_AddCircleFilled(dl, c, r, col, 16);
        else ImDrawList_AddCircleFilled(dl, c, r, hole, 16), ImDrawList_AddCircle(dl, c, r, col, 16, lw);
        break;
    }
}

void graph_background(ImDrawList* dl, ImVec2 o, ImVec2 size, float offx, float offy, float z, uint32_t bg) {
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + size.x, o.y + size.y }, bg ? bg : TH(TK_GRAPH_BG), 0, 0);
    if (g_node_style != GS_NODE_ORBIT) { // the older styles: lines every 16 units and every 128
        for (int pass = 0; pass < 2; pass++) {
            float step = (pass ? 128.0f : 16.0f) * z;
            if (step < 6.0f) continue;
            uint32_t col = pass ? TH(TK_GRAPH_GRID_MAJOR) : TH(TK_GRAPH_GRID);
            float sx = fmodf(offx, step), sy = fmodf(offy, step);
            if (sx < 0) sx += step;
            if (sy < 0) sy += step;
            for (float x = sx; x < size.x; x += step) ImDrawList_AddLine(dl, (ImVec2){ o.x + x, o.y }, (ImVec2){ o.x + x, o.y + size.y }, col, pass ? 2.0f : 1.0f);
            for (float y = sy; y < size.y; y += step) ImDrawList_AddLine(dl, (ImVec2){ o.x, o.y + y }, (ImVec2){ o.x + size.x, o.y + y }, col, pass ? 2.0f : 1.0f);
        }
        return;
    }
    // Orbit: a dot every 16 units (every 32, 64... when they'd crowd), a bigger one every 128; past zoom 1 the 128
    // grid's lines fade in. Quads straight into the draw list (4 vertices a dot)
    float step = 16.0f * z;
    int every = 8;
    while (step < 12.0f && every > 1) step *= 2.0f, every /= 2;
    float sx = fmodf(offx, step), sy = fmodf(offy, step);
    if (sx < 0) sx += step;
    if (sy < 0) sy += step;
    int nx = (int)((size.x - sx) / step) + 1, ny = (int)((size.y - sy) / step) + 1;
    if (nx <= 0 || ny <= 0 || (long)nx * ny > 60000) return;
    long ix0 = (long)floorf((sx - offx) / step + 0.5f), iy0 = (long)floorf((sy - offy) / step + 0.5f); // (the first dot's index)
    uint32_t minor = TH(TK_GRAPH_GRID_MAJOR), major = theme_mix(TH(TK_GRAPH_GRID_MAJOR), TH(TK_TEXT_DIM), 0.4f);
    float d = fmaxf(1.0f, 1.2f * fminf(z, 1.6f)), D = d * 1.7f;
    ImDrawList_PrimReserve(dl, nx * ny * 6, nx * ny * 4);
    for (int j = 0; j < ny; j++) {
        float y = o.y + sy + (float)j * step;
        bool mj = ((iy0 + j) % every + every) % every == 0;
        for (int i = 0; i < nx; i++) {
            float x = o.x + sx + (float)i * step;
            bool m = mj && ((ix0 + i) % every + every) % every == 0;
            float h = (m ? D : d) * 0.5f;
            ImDrawList_PrimRect(dl, (ImVec2){ x - h, y - h }, (ImVec2){ x + h, y + h }, m ? major : minor);
        }
    }
    if (z > 1.0f) { // the bigger grid surfacing
        float k = fminf((z - 1.0f) / 0.6f, 1.0f), big = 128.0f * z;
        uint32_t lc = theme_alpha(TH(TK_GRAPH_GRID), (unsigned)(k * (float)(TH(TK_GRAPH_GRID) >> 24)));
        float bx = fmodf(offx, big), by = fmodf(offy, big);
        if (bx < 0) bx += big;
        if (by < 0) by += big;
        for (float x = bx; x < size.x; x += big) ImDrawList_AddLine(dl, (ImVec2){ o.x + x, o.y }, (ImVec2){ o.x + x, o.y + size.y }, lc, 1.0f);
        for (float y = by; y < size.y; y += big) ImDrawList_AddLine(dl, (ImVec2){ o.x, o.y + y }, (ImVec2){ o.x + size.x, o.y + y }, lc, 1.0f);
    }
}

uint32_t graph_family_color(int fam) {
    static const int tok[GF_COUNT] = { TK_NODE_EVENT, TK_NODE_FUNCTION, TK_NODE_PURE, TK_NODE_LATENT, TK_NODE_MACRO, TK_NODE_FLOW,
                                       TK_PIN_WILDCARD, TK_NODE_CAST, TK_NODE_ENTRY };
    return TH(fam >= 0 && fam < GF_COUNT ? tok[fam] : TK_NODE_FUNCTION);
}

int graph_family_icon(int fam) {
    static const int ic[GF_COUNT] = { ICON_EVENT, ICON_FUNCTION, ICON_PURE, ICON_TIMER, ICON_MACRO, ICON_FLOW, ICON_VARIABLE, ICON_CAST, ICON_FUNCTION };
    return fam >= 0 && fam < GF_COUNT ? ic[fam] : ICON_FUNCTION;
}

float graph_card_title_x(ImVec2 p0, float header_h, int icon, float z) {
    float x = p0.x + 9.0f * z;
    if (icon >= 0 && header_h > 0.0f) x += fminf(header_h * 0.62f, igGetFontSize()) + 4.0f * z;
    return x;
}

void graph_card(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float header_h, uint32_t header_col, int icon, bool sel, bool hov, bool err,
                float z) {
    bool orbit = g_node_style == GS_NODE_ORBIT;
    float r = (orbit ? 8.0f : 6.0f) * z;
    if (orbit || g_node_style == GS_NODE_DEFAULT)
        for (int s = 3; s >= 1; s--) { // (a soft shadow)
            float e = (float)s * 1.5f * z;
            ImDrawList_AddRectFilled(dl, (ImVec2){ p0.x - e + 2 * z, p0.y - e + 3 * z }, (ImVec2){ p1.x + e + 2 * z, p1.y + e + 3 * z },
                                     THA(TK_N0, 30), r + e, 0);
        }
    uint32_t bc = err ? TH(TK_ERROR) : sel ? (orbit ? TH(TK_ACCENT) : TH(TK_AMBER)) : hov ? TH(TK_N11) : TH(TK_N8);
    graph_node_frame(dl, p0, p1, header_h, r, header_col, bc, (sel || err ? 2.2f : 1.0f) * z, z, g_node_style);
    if (icon >= 0 && header_h > 0.0f) {
        float is = fminf(header_h * 0.62f, igGetFontSize());
        ui_icon(dl, (UiIcon)icon, p0.x + 8.0f * z, p0.y + (header_h - is) * 0.5f, is, TH(TK_NODE_TITLE));
    }
    if (err && orbit) graph_error_badge(dl, (ImVec2){ p1.x, p0.y }, z);
}

void graph_error_badge(ImDrawList* dl, ImVec2 corner, float z) {
    float br = 6.0f * z;
    ImVec2 c = { corner.x - br * 0.3f, corner.y + br * 0.3f };
    ImDrawList_AddCircleFilled(dl, c, br, TH(TK_ERROR), 14);
    ImDrawList_AddLine(dl, (ImVec2){ c.x, c.y - br * 0.55f }, (ImVec2){ c.x, c.y + br * 0.12f }, TH(TK_TEXT_ON_ACCENT), 1.6f * z);
    ImDrawList_AddCircleFilled(dl, (ImVec2){ c.x, c.y + br * 0.5f }, 0.95f * z, TH(TK_TEXT_ON_ACCENT), 6);
}
