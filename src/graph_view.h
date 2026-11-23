// The node graphs' view (Behavior Tree, Anim Graph state machines...): Unreal's moves on every canvas — pan with the
// right or the middle button, zoom with the wheel around the mouse, the grid that follows. Header-only (static inline).
#pragma once
#include <math.h>
#include <stdbool.h>

#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#include <cimgui.h>
#include <stdint.h>

#include "graph_style.h"
#ifndef IM_COL32
#define IM_COL32(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#endif

typedef struct GraphView {
    float pan[2]; // graph units at the canvas's corner (negated)
    float zoom;   // 0.25 .. 2
    bool panning;
} GraphView;

static inline ImVec2 gv_screen(const GraphView* v, ImVec2 o, float x, float y) {
    return (ImVec2){ o.x + (x + v->pan[0]) * v->zoom, o.y + (y + v->pan[1]) * v->zoom };
}
static inline void gv_graph(const GraphView* v, ImVec2 o, ImVec2 s, float* x, float* y) {
    *x = (s.x - o.x) / v->zoom - v->pan[0];
    *y = (s.y - o.y) / v->zoom - v->pan[1];
}

// After the canvas's background item: drags with the right / middle button pan, the wheel zooms. Returns true while
// panning (a right click that panned must not open the menu: gv_right_click tells)
static inline bool gv_input(GraphView* v, ImVec2 o, bool hovered) {
    ImGuiIO* io = igGetIO_Nil();
    if (v->zoom < 0.2f || v->zoom > 2.5f) v->zoom = 1.0f;
    bool drag = igIsMouseDragging(ImGuiMouseButton_Right, 4.0f) || igIsMouseDragging(ImGuiMouseButton_Middle, 2.0f);
    if (hovered && drag && (igIsMouseDown_Nil(ImGuiMouseButton_Right) || igIsMouseDown_Nil(ImGuiMouseButton_Middle))) v->panning = true;
    if (v->panning) {
        v->pan[0] += io->MouseDelta.x / v->zoom, v->pan[1] += io->MouseDelta.y / v->zoom;
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Right) && !igIsMouseDown_Nil(ImGuiMouseButton_Middle)) v->panning = false;
    }
    if (hovered && io->MouseWheel != 0.0f) {
        float gx, gy;
        gv_graph(v, o, io->MousePos, &gx, &gy);
        v->zoom = fminf(fmaxf(v->zoom * powf(1.12f, io->MouseWheel), 0.25f), 2.0f);
        v->pan[0] = (io->MousePos.x - o.x) / v->zoom - gx;
        v->pan[1] = (io->MousePos.y - o.y) / v->zoom - gy;
    }
    return v->panning;
}

// (2026-11-23, the user: "dragging a node or a link to the window's edge, the graph moves, as in Blender") Edge pan: the
// mouse near a canvas's edge (or past it) while a node or a wire is dragged; out: this frame's scroll in screen pixels
// (+x: the view goes on to the right, +y: down), faster the closer (and further past) it is. False: not near an edge
static inline bool gv_edge_scroll(ImVec2 o, ImVec2 size, float out[2]) {
    ImGuiIO* io = igGetIO_Nil();
    float margin = fminf(36.0f, fminf(size.x, size.y) * 0.12f), speed = 700.0f * fminf(io->DeltaTime, 0.05f);
    const float m[2] = { io->MousePos.x, io->MousePos.y }, lo[2] = { o.x, o.y }, hi[2] = { o.x + size.x, o.y + size.y };
    out[0] = out[1] = 0.0f;
    if (margin < 4.0f) return false;
    for (int a = 0; a < 2; a++) {
        float d = 0.0f;
        if (m[a] > hi[a] - margin) d = (m[a] - (hi[a] - margin)) / margin;
        else if (m[a] < lo[a] + margin) d = -((lo[a] + margin) - m[a]) / margin;
        if (d > 3.0f) d = 3.0f;
        if (d < -3.0f) d = -3.0f;
        out[a] = d * speed;
    }
    return out[0] != 0.0f || out[1] != 0.0f;
}

// A right click that didn't pan (released where it was pressed): the canvas's menu
static inline bool gv_right_click(void) {
    ImVec2 d = igGetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
    return igIsMouseReleased_Nil(ImGuiMouseButton_Right) && fabsf(d.x) < 4.0f && fabsf(d.y) < 4.0f;
}

// The canvas's background (Q5, graph_style.c): the Orbit style's dots, the older styles' lines over bg
static inline void gv_grid(const GraphView* v, ImDrawList* dl, ImVec2 o, ImVec2 size, ImU32 bg) {
    graph_background(dl, o, size, v->pan[0] * v->zoom, v->pan[1] * v->zoom, v->zoom, graph_orbit() ? 0 : bg);
}
