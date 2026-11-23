// The asset viewport (asset_viewport.h; Q7, notas-dev/editor.md "Viewport de asset"): Unreal's asset editors' viewport,
// one for every editor. The GPU draws the meshes (render_preview: one picture per key, its view mode, the studio or the
// sky, the key light where the user turned it, the LOD of each mesh); ImGui draws over it what is not a mesh (bones,
// sockets, bodies, constraints, the gizmo, the UV layout), the toolbar (view mode, Show, Lighting, Camera, LOD) and the
// tabs. Nothing is kept between frames but the settings: no allocation (the draws are copied into a static array).
#include "asset_viewport.h"

#define THEME_SCENE // (overlays on the 3D picture: theme.h)
#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "anim.h"
#include "asset_viewer.h"
#include "i18n.h"
#include "ikrig.h"
#include "platform.h"
#include "render_gpu.h"
#include "ui_icons.h"
#include "ui_tips.h"

#define DEG2RAD 0.017453293f

static struct {
    int force_mode;                 // (the test) -1: off
    uint32_t frames[64];            // per key (mod 64): the picture asked for
    uint32_t views, frame_views;    // the viewports drawn last frame / this one
    double cpu_ms, frame_cpu_ms;
    int frame;
} G = { -1 };

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-8f) v[0] /= l, v[1] /= l, v[2] /= l;
}
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

void avp_init(AssetViewport* v) {
    memset(v, 0, sizeof *v);
    v->init = true;
    v->mode = PREVIEW_LIT, v->bones = AVP_BONES_ALL;
    v->sockets = v->bodies = v->constraints = v->floor = v->grid = v->stats = true;
    v->uv_layout = true;
    v->light_yaw = 35.0f, v->light_pitch = 50.0f, v->light_scale = 1.0f;
    v->fov = 40.0f, v->lod = -1, v->socket = -1, v->gizmo_axis = -1, v->gizmo_hot = -1;
}

void avp_test_force(int mode) { G.force_mode = mode; }
uint32_t avp_test_frames(unsigned key) { return G.frames[key & 63u]; }

AvpStats avp_stats(void) {
    AvpStats s = { G.views, G.cpu_ms, render_preview_stats()->gpu_ms };
    return s;
}

static void stats_frame(void) { // (once a frame: the last frame's sums kept)
    int f = igGetFrameCount();
    if (G.frame == f) return;
    G.views = G.frame_views, G.cpu_ms = G.frame_cpu_ms;
    G.frame_views = 0, G.frame_cpu_ms = 0.0, G.frame = f;
}

// --- The camera (bp_preview's orbit: yaw, pitch, distance around a target; Z up, the meshes face +X) ---

static void view_axes(AssetViewport* v, float x0, float y0, float x1, float y1) {
    const BpPreviewCam* c = &v->cam;
    float sp = sinf(c->pitch * DEG2RAD), cp = cosf(c->pitch * DEG2RAD), sy = sinf(c->yaw * DEG2RAD), cy = cosf(c->yaw * DEG2RAD);
    v->fwd[0] = cp * cy, v->fwd[1] = cp * sy, v->fwd[2] = sp;
    v->right[0] = -sy, v->right[1] = cy, v->right[2] = 0.0f;
    v->up[0] = -sp * cy, v->up[1] = -sp * sy, v->up[2] = cp;
    for (int k = 0; k < 3; k++) v->eye[k] = c->target[k] - v->fwd[k] * c->dist;
    v->x0 = x0, v->y0 = y0, v->x1 = x1, v->y1 = y1;
    v->focal = (y1 - y0) * 0.5f / tanf(fminf(fmaxf(v->fov, 5.0f), 150.0f) * 0.5f * DEG2RAD);
}

void avp_view_at(AssetViewport* v, float x, float y, float w, float h) { view_axes(v, x, y, x + w, y + h); }

bool avp_to_screen(const AssetViewport* v, const float p[3], float out[2]) {
    float d[3] = { p[0] - v->eye[0], p[1] - v->eye[1], p[2] - v->eye[2] };
    float z = dot3(d, v->fwd);
    float cz = fmaxf(z, 0.1f);
    out[0] = (v->x0 + v->x1) * 0.5f + dot3(d, v->right) * v->focal / cz;
    out[1] = (v->y0 + v->y1) * 0.5f - dot3(d, v->up) * v->focal / cz;
    return z > 0.5f;
}
static ImVec2 scr(const AssetViewport* v, const float p[3]) {
    float o[2];
    avp_to_screen(v, p, o);
    return (ImVec2){ o[0], o[1] };
}

void avp_screen_delta(const AssetViewport* v, const float at[3], float dx, float dy, float out[3]) {
    float d[3] = { at[0] - v->eye[0], at[1] - v->eye[1], at[2] - v->eye[2] };
    float z = fmaxf(dot3(d, v->fwd), 1.0f), k = z / fmaxf(v->focal, 1.0f);
    for (int i = 0; i < 3; i++) out[i] = (v->right[i] * dx - v->up[i] * dy) * k;
}

void avp_frame(AssetViewport* v, const float mn[3], const float mx[3]) {
    float e[3];
    for (int k = 0; k < 3; k++) v->cam.target[k] = (mn[k] + mx[k]) * 0.5f, e[k] = (mx[k] - mn[k]) * 0.5f;
    float r = fmaxf(sqrtf(dot3(e, e)), 10.0f);
    if (!v->cam.framed) v->cam.yaw = -145.0f, v->cam.pitch = -12.0f; // (the first time: from the front-left, a little above)
    v->cam.dist = r / sinf(fminf(fmaxf(v->fov, 5.0f), 150.0f) * 0.5f * DEG2RAD) * 1.1f;
    v->cam.framed = true;
}

// A segment cut at the near plane
static void line3(ImDrawList* dl, const AssetViewport* v, const float a[3], const float b[3], uint32_t col, float th) {
    float da[3] = { a[0] - v->eye[0], a[1] - v->eye[1], a[2] - v->eye[2] }, db[3] = { b[0] - v->eye[0], b[1] - v->eye[1], b[2] - v->eye[2] };
    float za = dot3(da, v->fwd), zb = dot3(db, v->fwd);
    const float nz = 1.0f;
    if (za < nz && zb < nz) return;
    float pa[3], pb[3];
    memcpy(pa, a, 12), memcpy(pb, b, 12);
    if (za < nz || zb < nz) {
        float t = (nz - za) / (zb - za);
        float* m = za < nz ? pa : pb;
        for (int k = 0; k < 3; k++) m[k] = a[k] + (b[k] - a[k]) * t;
    }
    ImDrawList_AddLine(dl, scr(v, pa), scr(v, pb), col, th);
}
static void ring3(ImDrawList* dl, const AssetViewport* v, const float c[3], const float u[3], const float w[3], float r, float a0, float a1,
                  uint32_t col, float th) {
    const int n = 20;
    float prev[3];
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n, p[3];
        for (int k = 0; k < 3; k++) p[k] = c[k] + (u[k] * cosf(a) + w[k] * sinf(a)) * r;
        if (i) line3(dl, v, prev, p, col, th);
        memcpy(prev, p, 12);
    }
}

// --- The input: orbit, pan, zoom, fly, dolly, the light, F ---

static void input(AssetViewport* v, bool hovered, bool active, const float mn[3], const float mx[3], float radius, AvpResult* r) {
    ImGuiIO* io = igGetIO_Nil();
    BpPreviewCam* c = &v->cam;
    bool lkey = igIsKeyDown_Nil(ImGuiKey_L);
    v->flying = false;
    if (active && !v->dragging) {
        float dx = io->MouseDelta.x, dy = io->MouseDelta.y;
        if (igIsMouseDown_Nil(ImGuiMouseButton_Right) && !io->KeyAlt) { // fly: look around the eye, WASD / QE move
            float eye[3];
            for (int k = 0; k < 3; k++) eye[k] = c->target[k] - v->fwd[k] * c->dist;
            c->yaw += dx * 0.25f, c->pitch = fminf(fmaxf(c->pitch - dy * 0.25f, -89.0f), 89.0f);
            float sp = sinf(c->pitch * DEG2RAD), cp = cosf(c->pitch * DEG2RAD), sy = sinf(c->yaw * DEG2RAD), cy = cosf(c->yaw * DEG2RAD);
            float f[3] = { cp * cy, cp * sy, sp }, rt[3] = { -sy, cy, 0.0f };
            float speed = (v->fly_speed > 0.0f ? v->fly_speed : fmaxf(radius * 2.0f, 50.0f)) * io->DeltaTime * (io->KeyShift ? 3.0f : 1.0f);
            float mv[3] = { 0, 0, 0 };
            if (igIsKeyDown_Nil(ImGuiKey_W)) for (int k = 0; k < 3; k++) mv[k] += f[k];
            if (igIsKeyDown_Nil(ImGuiKey_S)) for (int k = 0; k < 3; k++) mv[k] -= f[k];
            if (igIsKeyDown_Nil(ImGuiKey_D)) for (int k = 0; k < 3; k++) mv[k] += rt[k];
            if (igIsKeyDown_Nil(ImGuiKey_A)) for (int k = 0; k < 3; k++) mv[k] -= rt[k];
            if (igIsKeyDown_Nil(ImGuiKey_E)) mv[2] += 1.0f;
            if (igIsKeyDown_Nil(ImGuiKey_Q)) mv[2] -= 1.0f;
            for (int k = 0; k < 3; k++) eye[k] += mv[k] * speed, c->target[k] = eye[k] + f[k] * c->dist;
            if (hovered && io->MouseWheel != 0.0f) // (the wheel while flying: the speed)
                v->fly_speed = fminf(fmaxf((v->fly_speed > 0.0f ? v->fly_speed : fmaxf(radius * 2.0f, 50.0f)) * powf(1.25f, io->MouseWheel), 5.0f), 1e5f);
            v->flying = true;
        } else if (igIsMouseDown_Nil(ImGuiMouseButton_Right) && io->KeyAlt) { // dolly
            c->dist = fminf(fmaxf(c->dist * powf(1.006f, dy - dx), 2.0f), 1e6f);
        } else if (igIsMouseDown_Nil(ImGuiMouseButton_Middle)) { // pan in the view's plane
            float k = c->dist / fmaxf(v->focal, 1.0f);
            for (int i = 0; i < 3; i++) c->target[i] += (-v->right[i] * dx + v->up[i] * dy) * k;
        } else if (igIsMouseDown_Nil(ImGuiMouseButton_Left) && lkey) { // L + drag: the key light (Unreal's)
            v->light_custom = true;
            v->light_yaw += dx * 0.5f, v->light_pitch = fminf(fmaxf(v->light_pitch - dy * 0.3f, 3.0f), 89.0f);
        } else if (igIsMouseDown_Nil(ImGuiMouseButton_Left) && igIsMouseDragging(ImGuiMouseButton_Left, 2.0f)) { // orbit
            c->yaw += dx * 0.35f, c->pitch = fminf(fmaxf(c->pitch - dy * 0.35f, -89.0f), 89.0f);
        }
        if (r && (dx != 0.0f || dy != 0.0f)) r->busy = true;
    }
    if (hovered && !v->flying && io->MouseWheel != 0.0f) c->dist = fminf(fmaxf(c->dist * powf(0.88f, io->MouseWheel), 2.0f), 1e6f);
    if (hovered && !io->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_F, false)) avp_frame(v, mn, mx);
}

// --- The overlays ---

static uint32_t bone_col_a(bool sel, bool child, int a) { return sel ? THA(TK_ORANGE, a) : child ? THA(TK_AMBER_L, a * 230 / 255) : THA(TK_N15, a * 215 / 255); }
static uint32_t bone_col(bool sel, bool child) { return bone_col_a(sel, child, 255); }

// Unreal's bones: a thin diamond from the parent's head to the child's, the joint a dot
static void bone_shape(ImDrawList* dl, const AssetViewport* v, const float a[3], const float b[3], uint32_t col, uint32_t fill, bool sel) {
    float sa[2], sb[2];
    if (!avp_to_screen(v, a, sa) || !avp_to_screen(v, b, sb)) {
        line3(dl, v, a, b, col, sel ? 2.5f : 1.3f);
        return;
    }
    float dx = sb[0] - sa[0], dy = sb[1] - sa[1], l = sqrtf(dx * dx + dy * dy);
    if (l < 4.0f) {
        ImDrawList_AddLine(dl, (ImVec2){ sa[0], sa[1] }, (ImVec2){ sb[0], sb[1] }, col, sel ? 2.5f : 1.3f);
        return;
    }
    float wdt = fminf(fmaxf(l * 0.08f, 1.5f), 7.0f), nx = -dy / l * wdt, ny = dx / l * wdt;
    ImVec2 m = { sa[0] + dx * 0.18f, sa[1] + dy * 0.18f };
    ImVec2 q[4] = { { sa[0], sa[1] }, { m.x + nx, m.y + ny }, { sb[0], sb[1] }, { m.x - nx, m.y - ny } };
    ImDrawList_AddConvexPolyFilled(dl, q, 4, fill);
    ImDrawList_AddPolyline(dl, q, 4, col, sel ? 2.0f : 1.0f, ImDrawFlags_Closed);
}

static void axes_at(ImDrawList* dl, const AssetViewport* v, const float rows[3][4], float len, float th) {
    static const int cols[3] = { TK_RED, TK_GREEN, TK_BLUE };
    float o[3] = { rows[0][3], rows[1][3], rows[2][3] };
    for (int a = 0; a < 3; a++) {
        float d[3] = { rows[0][a], rows[1][a], rows[2][a] }, e[3];
        norm3(d);
        for (int k = 0; k < 3; k++) e[k] = o[k] + d[k] * len;
        line3(dl, v, o, e, TH(cols[a]), th);
    }
}

static bool is_child_of(const struct SkelMesh* m, int b, int sel) { return sel >= 0 && b >= 0 && m->bones[b].parent == sel; }

// The bones; the joint under the mouse (within 7 px) as *hit
static void draw_bones(ImDrawList* dl, const AssetViewport* v, const AvpScene* s, ImVec2 mouse, int* hit) {
    const SkelMesh* m = s->skel;
    float best = 49.0f;
    for (uint32_t b = 0; b < m->nbones; b++) {
        int p = m->bones[b].parent;
        bool sel = (int)b == s->sel_bone, child = is_child_of(m, (int)b, s->sel_bone);
        bool show = v->bones == AVP_BONES_ALL || (v->bones == AVP_BONES_SELECTED && (sel || child || (int)b == (s->sel_bone >= 0 ? m->bones[s->sel_bone].parent : -2)));
        if (!show) continue;
        float h[3] = { s->bones[b][0][3], s->bones[b][1][3], s->bones[b][2][3] };
        if (p >= 0) {
            float hp[3] = { s->bones[p][0][3], s->bones[p][1][3], s->bones[p][2][3] };
            bone_shape(dl, v, hp, h, bone_col(sel, child), bone_col_a(sel, child, sel ? 144 : 72), sel);
        }
        float sh[2];
        if (avp_to_screen(v, h, sh)) {
            ImDrawList_AddCircleFilled(dl, (ImVec2){ sh[0], sh[1] }, sel ? 4.0f : 2.5f, bone_col(sel, child), 10);
            float d2 = (mouse.x - sh[0]) * (mouse.x - sh[0]) + (mouse.y - sh[1]) * (mouse.y - sh[1]);
            if (d2 < best) best = d2, *hit = (int)b;
            if (v->bone_names && (v->bones == AVP_BONES_ALL || sel || v->bones == AVP_BONES_SELECTED))
                ImDrawList_AddText_Vec2(dl, (ImVec2){ sh[0] + 6.0f, sh[1] - igGetFontSize() * 0.5f }, sel ? TH(TK_ORANGE) : THA(TK_N15, 190), m->bones[b].name, NULL);
        }
        if (v->bone_axes && (v->bones == AVP_BONES_ALL || sel)) axes_at(dl, v, s->bones[b], fmaxf(v->radius * 0.05f, 1.0f), sel ? 2.0f : 1.2f);
    }
}

// The sockets: a small diamond and its name; the gizmo's three arrows on the selected one (world axes) dragged
static void draw_sockets(ImDrawList* dl, AssetViewport* v, const AvpScene* s, ImVec2 mouse, int* hit, bool click, AvpResult* r) {
    const SkelMesh* m = s->skel;
    float best = 64.0f;
    for (uint32_t i = 0; i < m->nsockets; i++) {
        float rows[3][4];
        anim_socket_rows(m, &m->sockets[i], s->bones, rows);
        float o[3] = { rows[0][3], rows[1][3], rows[2][3] }, so[2];
        if (!avp_to_screen(v, o, so)) continue;
        bool sel = (int)i == v->socket;
        uint32_t col = sel ? TH(TK_ORANGE) : TH(TK_MAGENTA_L), fill = sel ? THA(TK_ORANGE, 192) : THA(TK_MAGENTA_L, 192);
        float z = sel ? 6.0f : 4.5f;
        ImVec2 q[4] = { { so[0], so[1] - z }, { so[0] + z, so[1] }, { so[0], so[1] + z }, { so[0] - z, so[1] } };
        ImDrawList_AddConvexPolyFilled(dl, q, 4, fill);
        ImDrawList_AddPolyline(dl, q, 4, TH(TK_N0), 1.0f, ImDrawFlags_Closed);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ so[0] + 8.0f, so[1] - igGetFontSize() * 0.5f }, col, m->sockets[i].name, NULL);
        if (sel) axes_at(dl, v, (const float(*)[4])rows, fmaxf(v->radius * 0.06f, 1.5f), 1.5f);
        float d2 = (mouse.x - so[0]) * (mouse.x - so[0]) + (mouse.y - so[1]) * (mouse.y - so[1]);
        if (d2 < best) best = d2, *hit = (int)i;
    }
    // the gizmo (moving: s->edit_sockets)
    if (!s->edit_sockets || v->socket < 0 || v->socket >= (int)m->nsockets) {
        v->dragging = false, v->gizmo_hot = -1;
        return;
    }
    AnimSocket* k = &s->edit_sockets->sockets[v->socket];
    float rows[3][4];
    anim_socket_rows(m, k, s->bones, rows);
    float o[3] = { rows[0][3], rows[1][3], rows[2][3] }, so[2];
    if (!avp_to_screen(v, o, so)) return;
    float d[3] = { o[0] - v->eye[0], o[1] - v->eye[1], o[2] - v->eye[2] };
    float len = fmaxf(dot3(d, v->fwd), 1.0f) * 70.0f / fmaxf(v->focal, 1.0f); // (70 px long whatever the distance)
    static const int cols[3] = { TK_RED, TK_GREEN, TK_BLUE };
    ImVec2 tip[3];
    int hot = -1;
    float hd = 49.0f;
    for (int a = 0; a < 3; a++) {
        float e[3] = { o[0], o[1], o[2] };
        e[a] += len;
        tip[a] = scr(v, e);
        float ax = tip[a].x - so[0], ay = tip[a].y - so[1], l2 = ax * ax + ay * ay;
        float t = l2 > 1.0f ? fminf(fmaxf(((mouse.x - so[0]) * ax + (mouse.y - so[1]) * ay) / l2, 0.0f), 1.0f) : 0.0f;
        float px = so[0] + ax * t - mouse.x, py = so[1] + ay * t - mouse.y;
        if (t > 0.15f && px * px + py * py < hd) hd = px * px + py * py, hot = a;
    }
    ImGuiIO* io = igGetIO_Nil();
    if (v->dragging) hot = v->gizmo_axis;
    for (int a = 0; a < 3; a++) {
        uint32_t c = a == hot ? TH(TK_AMBER_L) : TH(cols[a]);
        ImDrawList_AddLine(dl, (ImVec2){ so[0], so[1] }, tip[a], c, a == hot ? 3.5f : 2.5f);
        ImDrawList_AddCircleFilled(dl, tip[a], a == hot ? 5.5f : 4.5f, c, 10);
    }
    v->gizmo_hot = hot;
    if (click && hot >= 0 && !v->dragging) {
        v->dragging = true, v->gizmo_axis = hot;
        memcpy(v->drag_from, k->local.t, 12);
    }
    if (v->dragging) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            v->dragging = false, v->gizmo_axis = -1;
            return;
        }
        int a = v->gizmo_axis;
        float ax = tip[a].x - so[0], ay = tip[a].y - so[1], l2 = ax * ax + ay * ay;
        if (l2 < 1.0f) return;
        float move = (io->MouseDelta.x * ax + io->MouseDelta.y * ay) / l2 * len; // (cm along the world axis)
        if (move == 0.0f) return;
        float wd[3] = { 0, 0, 0 };
        wd[a] = move;
        avp_socket_move_world(s->edit_sockets, v->socket, s->bones, wd);
        s->edit_sockets->dirty = true;
        if (r) r->socket_moved = true, r->busy = true;
    }
}

// A body's place: its center and its axis (capsule) in the view's space from its bone's rows
static bool body_frame(const SkelMesh* m, const float (*bones)[3][4], const PhysBody* pb, float c[3], float ax[3], float u[3], float w[3]) {
    int b = -1;
    for (uint32_t i = 0; i < m->nbones && b < 0; i++)
        if (!_stricmp(m->bones[i].name, pb->bone)) b = (int)i;
    if (b < 0) return false;
    for (int k = 0; k < 3; k++) {
        c[k] = bones[b][k][3], ax[k] = 0.0f;
        for (int j = 0; j < 3; j++) c[k] += bones[b][k][j] * pb->offset[j], ax[k] += bones[b][k][j] * pb->axis[j];
    }
    float al = sqrtf(dot3(ax, ax));
    if (al < 1e-6f) ax[0] = 0, ax[1] = 0, ax[2] = 1, al = 1.0f;
    for (int k = 0; k < 3; k++) ax[k] /= al;
    float t[3] = { 0, 0, 1 };
    if (fabsf(ax[2]) > 0.9f) t[0] = 1, t[2] = 0;
    cross3(ax, t, u), norm3(u), cross3(ax, u, w);
    return true;
}

static void draw_bodies(ImDrawList* dl, const AssetViewport* v, const AvpScene* s, ImVec2 mouse, int* hit) {
    const PhysicsAsset* pa = s->phys;
    const SkelMesh* m = s->skel;
    float best = 1e30f;
    float centers[PHYS_MAX_BODIES][3];
    bool ok[PHYS_MAX_BODIES] = { 0 };
    for (uint32_t i = 0; i < pa->nbodies; i++) {
        const PhysBody* pb = &pa->bodies[i];
        float c[3], ax[3], u[3], w[3];
        if (!body_frame(m, s->bones, pb, c, ax, u, w)) continue;
        memcpy(centers[i], c, 12), ok[i] = true;
        if (!v->bodies) continue;
        bool sel = (int)i == s->sel_body;
        uint32_t col = sel ? TH(TK_ORANGE) : pb->simulate ? THA(TK_GREEN_L, 220) : THA(TK_N12, 200);
        float th = sel ? 2.2f : 1.2f;
        const float PI = 3.14159265f;
        float rad = pb->radius;
        if (pb->shape == PHYS_BOX) {
            const float* e = pb->extent;
            rad = fmaxf(fmaxf(e[0], e[1]), e[2]);
            // its bone's axes (the box's): the rows' columns, normalized
            int b = -1;
            for (uint32_t q = 0; q < m->nbones && b < 0; q++)
                if (!_stricmp(m->bones[q].name, pb->bone)) b = (int)q;
            float bx[3][3];
            for (int a = 0; a < 3; a++) {
                for (int k = 0; k < 3; k++) bx[a][k] = s->bones[b][k][a];
                norm3(bx[a]);
            }
            float corner[8][3];
            for (int n = 0; n < 8; n++)
                for (int k = 0; k < 3; k++)
                    corner[n][k] = c[k] + bx[0][k] * e[0] * ((n & 1) ? 1 : -1) + bx[1][k] * e[1] * ((n & 2) ? 1 : -1) + bx[2][k] * e[2] * ((n & 4) ? 1 : -1);
            static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (int ed = 0; ed < 12; ed++) line3(dl, v, corner[edges[ed][0]], corner[edges[ed][1]], col, th);
        } else if (pb->shape == PHYS_SPHERE) {
            ring3(dl, v, c, u, w, rad, 0, 2 * PI, col, th), ring3(dl, v, c, ax, u, rad, 0, 2 * PI, col, th), ring3(dl, v, c, ax, w, rad, 0, 2 * PI, col, th);
        } else { // a capsule: its two end rings, four sides, the half spheres' arcs
            float h = pb->length * 0.5f, a0[3], a1[3];
            for (int k = 0; k < 3; k++) a0[k] = c[k] - ax[k] * h, a1[k] = c[k] + ax[k] * h;
            ring3(dl, v, a0, u, w, rad, 0, 2 * PI, col, th), ring3(dl, v, a1, u, w, rad, 0, 2 * PI, col, th);
            for (int sd = 0; sd < 4; sd++) {
                const float* dir = sd < 2 ? u : w;
                float sg = (sd & 1) ? -1.0f : 1.0f, p0[3], p1[3];
                for (int k = 0; k < 3; k++) p0[k] = a0[k] + dir[k] * rad * sg, p1[k] = a1[k] + dir[k] * rad * sg;
                line3(dl, v, p0, p1, col, th);
            }
            float nax[3] = { -ax[0], -ax[1], -ax[2] };
            ring3(dl, v, a1, u, ax, rad, 0, PI, col, th), ring3(dl, v, a1, w, ax, rad, 0, PI, col, th);
            ring3(dl, v, a0, u, nax, rad, 0, PI, col, th), ring3(dl, v, a0, w, nax, rad, 0, PI, col, th);
        }
        float sc[2];
        if (avp_to_screen(v, c, sc)) {
            float d[3] = { c[0] - v->eye[0], c[1] - v->eye[1], c[2] - v->eye[2] };
            float rpx = fmaxf(rad * v->focal / fmaxf(dot3(d, v->fwd), 1.0f), 8.0f);
            float d2 = (mouse.x - sc[0]) * (mouse.x - sc[0]) + (mouse.y - sc[1]) * (mouse.y - sc[1]);
            if (d2 < rpx * rpx && d2 / (rpx * rpx) < best) best = d2 / (rpx * rpx), *hit = (int)i;
        }
    }
    if (!v->constraints) return;
    for (uint32_t i = 0; i < pa->nconstraints; i++) { // a constraint: the line between its bodies, a ring where the child turns
        const PhysConstraint* pc = &pa->constraints[i];
        if (pc->parent >= pa->nbodies || pc->child >= pa->nbodies || !ok[pc->parent] || !ok[pc->child]) continue;
        bool sel = (int)i == s->sel_constraint;
        uint32_t col = sel ? TH(TK_ORANGE) : THA(TK_CYAN_L, 200);
        line3(dl, v, centers[pc->parent], centers[pc->child], col, sel ? 2.0f : 1.0f);
        float sc[2];
        if (avp_to_screen(v, centers[pc->child], sc))
            ImDrawList_AddCircle(dl, (ImVec2){ sc[0], sc[1] }, 3.0f + fminf(pc->swing, 90.0f) * 0.08f, col, 14, sel ? 2.0f : 1.0f);
    }
}

// The UV layout of the first mesh (Unreal's UV overlay), in a square at the bottom right; at most ~8000 triangles
static void draw_uv_layout(ImDrawList* dl, const AssetViewport* v, const PreviewDraw* d) {
    const GpuVertex* vv;
    const uint32_t* idx;
    uint32_t first, count;
    int32_t off;
    render_mesh_cpu(d->mesh, &vv, &idx, &first, &count, &off);
    if (!vv || !idx || !count) return;
    float side = fminf(v->x1 - v->x0, v->y1 - v->y0) * 0.36f;
    ImVec2 o = { v->x1 - side - 10.0f, v->y1 - side - igGetFontSize() * 2.6f };
    ImDrawList_AddRectFilled(dl, o, (ImVec2){ o.x + side, o.y + side }, THA(TK_N1, 200), 0, 0);
    for (int g = 1; g < 4; g++) {
        float t = side * (float)g / 4.0f;
        ImDrawList_AddLine(dl, (ImVec2){ o.x + t, o.y }, (ImVec2){ o.x + t, o.y + side }, THA(TK_N8, 120), 1.0f);
        ImDrawList_AddLine(dl, (ImVec2){ o.x, o.y + t }, (ImVec2){ o.x + side, o.y + t }, THA(TK_N8, 120), 1.0f);
    }
    uint32_t ntri = count / 3, step = ntri > 8000u ? (ntri + 7999u) / 8000u : 1u;
    uint32_t col = THA(TK_CYAN_L, 150);
    for (uint32_t t = 0; t < ntri; t += step) {
        ImVec2 p[3];
        for (int c = 0; c < 3; c++) {
            const GpuVertex* g = &vv[(int64_t)idx[first + t * 3 + c] + off];
            float u = g->uv[0] - floorf(g->uv[0]), w = g->uv[1] - floorf(g->uv[1]);
            if (g->uv[0] == 1.0f) u = 1.0f;
            if (g->uv[1] == 1.0f) w = 1.0f;
            p[c] = (ImVec2){ o.x + u * side, o.y + w * side };
        }
        ImDrawList_AddPolyline(dl, p, 3, col, 1.0f, ImDrawFlags_Closed);
    }
    ImDrawList_AddRect(dl, o, (ImVec2){ o.x + side, o.y + side }, THA(TK_N14, 160), 0.0f, 1.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ o.x + 4.0f, o.y + 2.0f }, THA(TK_N15, 200), TR("UV Channel 0"), NULL);
}

// --- The toolbar's menus ---

static bool g_compact; // (a narrow view: the toolbar's menus as icons)
static void show_menu(AssetViewport* v, const AvpScene* s) {
    if (ui_dropdown_button("avp_show", ICON_EYE, g_compact ? "" : TR("Show"), TH(TK_N15), TR("What the viewport shows over the model: bones, sockets, bodies, the floor, the UV layout, the statistics")))
        igOpenPopup_Str("avp_show_menu", 0);
    if (!igBeginPopup("avp_show_menu", 0)) return;
    bool sk = s->skel != NULL;
    if (ui_begin_menu(ICON_ANGLE, TR("Bones"), sk)) {
        if (ui_menu_item(ICON_CROSS, TR("No Bones"), NULL, v->bones == AVP_BONES_NONE, true)) v->bones = AVP_BONES_NONE;
        if (ui_menu_item(ICON_SELECT, TR("Selected Bones"), NULL, v->bones == AVP_BONES_SELECTED, true)) v->bones = AVP_BONES_SELECTED;
        if (ui_menu_item(ICON_ANGLE, TR("All Bones"), NULL, v->bones == AVP_BONES_ALL, true)) v->bones = AVP_BONES_ALL;
        igSeparator();
        ui_menu_item_ptr(ICON_RENAME, TR("Bone Names"), NULL, &v->bone_names, true);
        ui_menu_item_ptr(ICON_MOVE, TR("Bone Axes"), NULL, &v->bone_axes, true);
        igEndMenu();
    }
    ui_menu_item_ptr(ICON_ACTOR, TR("Sockets"), NULL, &v->sockets, sk);
    ui_menu_item_ptr(ICON_CAPSULE, TR("Physics Bodies"), NULL, &v->bodies, s->phys != NULL);
    ui_menu_item_ptr(ICON_ANGLE, TR("Constraints"), NULL, &v->constraints, s->phys != NULL);
    igSeparator();
    ui_menu_item_ptr(ICON_FLOOR, TR("Floor"), NULL, &v->floor, true);
    ui_menu_item_ptr(ICON_GRID, TR("Grid"), NULL, &v->grid, v->floor);
    ui_menu_item_ptr(ICON_GRID, TR("UV Layout"), NULL, &v->uv_layout, true);
    ui_menu_item_ptr(ICON_STATS, TR("Statistics"), NULL, &v->stats, true);
    igEndPopup();
}

static void light_menu(AssetViewport* v) {
    if (ui_dropdown_button("avp_light", ICON_SUN, g_compact ? "" : TR("Lighting"), TH(TK_N15), TR("The preview's light and environment: the studio or an outdoor sky, the key light's direction (L + drag) and intensity")))
        igOpenPopup_Str("avp_light_menu", 0);
    if (!igBeginPopup("avp_light_menu", 0)) return;
    igSeparatorText(TR("Environment"));
    if (ui_menu_item(ICON_VM_LIT, TR("Studio"), NULL, v->env == AVP_ENV_STUDIO, true)) v->env = AVP_ENV_STUDIO;
    if (ui_menu_item(ICON_SKY, TR("Outdoor Sky"), NULL, v->env == AVP_ENV_SKY, true)) v->env = AVP_ENV_SKY;
    igSeparatorText(TR("Key Light"));
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    if (igSliderFloat(TR("Light Yaw"), &v->light_yaw, -180.0f, 180.0f, "%.0f", 0)) v->light_custom = true;
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    if (igSliderFloat(TR("Light Pitch"), &v->light_pitch, 3.0f, 89.0f, "%.0f", 0)) v->light_custom = true;
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    igSliderFloat(TR("Light Intensity"), &v->light_scale, 0.1f, 4.0f, "%.2f", 0);
    if (igButton(TR("Follow the Camera"), (ImVec2){ 0, 0 })) v->light_custom = false, v->light_scale = 1.0f;
    igTextDisabled("%s", TR("L + left drag in the view turns the light."));
    igEndPopup();
}

static void camera_menu(AssetViewport* v, const float mn[3], const float mx[3]) {
    if (ui_dropdown_button("avp_cam", ICON_CAMERA, g_compact ? "" : TR("Camera"), TH(TK_N15), TR("The camera: its field of view, the flying speed, framing the model (F)")))
        igOpenPopup_Str("avp_cam_menu", 0);
    if (!igBeginPopup("avp_cam_menu", 0)) return;
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    igSliderFloat(TR("Field of View"), &v->fov, 10.0f, 120.0f, "%.0f", 0);
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    float sp = v->fly_speed;
    if (igSliderFloat(TR("Fly Speed"), &sp, 0.0f, 5000.0f, sp > 0.0f ? "%.0f cm/s" : TR("by its size"), ImGuiSliderFlags_Logarithmic)) v->fly_speed = sp;
    if (ui_menu_item(ICON_FOCUS, TR("Frame the Model"), "F", false, true)) avp_frame(v, mn, mx);
    if (ui_menu_item(ICON_UNDO, TR("Reset the Camera"), NULL, false, true)) v->cam.framed = false, v->fov = 40.0f, v->fly_speed = 0.0f, avp_frame(v, mn, mx);
    igSeparator();
    igTextDisabled("%s", TR("Left drag: orbit. Middle: pan. Wheel: zoom. Right drag + WASD/QE: fly (wheel: speed). Alt + right: dolly."));
    igEndPopup();
}

static void lod_menu(AssetViewport* v, uint32_t nlods, const uint32_t tris[4]) {
    char lbl[48];
    if (v->lod < 0) snprintf(lbl, sizeof lbl, TR("LOD Auto (%u)"), v->lod_shown);
    else snprintf(lbl, sizeof lbl, TR("LOD %d"), v->lod);
    if (g_compact) snprintf(lbl, sizeof lbl, v->lod < 0 ? "%u" : "%d", v->lod < 0 ? (int)v->lod_shown : v->lod);
    if (ui_dropdown_button("avp_lod", ICON_VM_LOD, lbl, TH(TK_N15), TR("The level of detail drawn: by the distance as in the game (Auto), or one forced")))
        igOpenPopup_Str("avp_lod_menu", 0);
    if (!igBeginPopup("avp_lod_menu", 0)) return;
    if (ui_menu_item(ICON_VM_LOD, TR("LOD Auto"), NULL, v->lod < 0, true)) v->lod = -1;
    for (uint32_t l = 0; l < nlods && l < 4; l++) {
        char t[64];
        snprintf(t, sizeof t, TR("LOD %u  (%u triangles)"), l, tris[l]);
        if (ui_menu_item(ICON_VM_TRIANGLES, t, NULL, v->lod == (int)l, true)) v->lod = (int)l;
    }
    igEndPopup();
}

static const UiIcon g_tab_icons[AVP_TAB_COUNT] = { ICON_CHARACTER, ICON_ANGLE, ICON_UPDATE, ICON_CAPSULE, ICON_MOVEMENT };
static const char* const g_tab_names[AVP_TAB_COUNT] = { "Model", "Skeleton", "Animation", "Physics", "IK Rig" };
static const char* const g_tab_tips[AVP_TAB_COUNT] = {
    "The Skeletal Mesh: its parts, skins and materials (the same window)",
    "The skeleton: its bones and sockets (the same window)",
    "The animations of this skeleton: play, scrub, notifies (the same window)",
    "The Physics Asset of this mesh: bodies, constraints, the ragdoll (the same window)",
    "The IK Rig of this mesh: goals and solvers (the same window)",
};

int avp_tabs_bar(unsigned tabs, int tab) {
    if (!tabs) return -1;
    float tw = 0.0f, fh = igGetFrameHeight();
    for (int t = 0; t < AVP_TAB_COUNT; t++)
        if (tabs & AVP_TAB(t)) tw += fh * 0.95f + igCalcTextSize(TR(g_tab_names[t]), NULL, true, -1).x + igGetStyle()->FramePadding.x * 2.0f + 8.0f;
    igSameLine(0, 0);
    float x = igGetCursorPosX(), right = x + igGetContentRegionAvail().x;
    igSetCursorPosX(fmaxf(x + 8.0f, right - tw));
    int clicked = -1;
    bool first = true;
    for (int t = 0; t < AVP_TAB_COUNT; t++) {
        if (!(tabs & AVP_TAB(t))) continue;
        if (!first) igSameLine(0, 4);
        first = false;
        char id[24];
        snprintf(id, sizeof id, "avp_tab%d", t);
        if (ui_icon_button(id, g_tab_icons[t], TR(g_tab_names[t]), t == tab ? TH(TK_ACCENT) : TH(TK_N15), t == tab, TR(g_tab_tips[t])) && t != tab)
            clicked = t;
    }
    return clicked;
}

// --- The view ---

void avp_draw(AssetViewport* v, unsigned key, float x, float y, float w, float h, const AvpScene* s, AvpResult* r) {
    if (!v->init) avp_init(v);
    stats_frame();
    double t0 = re_time_seconds();
    AvpResult dummy;
    if (!r) r = &dummy;
    memset(r, 0, sizeof *r);
    r->tab = r->bone = r->body = r->socket = -1;
    if (w < 24.0f || h < 24.0f) return;
    if (!s->edit_sockets || !v->sockets) v->gizmo_hot = -1, v->dragging = false;
    int mode = G.force_mode >= 0 ? G.force_mode : v->mode;
    bool test = G.force_mode >= 0;
    // the bounds: the draws' and the bones'
    static PreviewDraw d[AVP_DRAWS_MAX];
    uint32_t n = 0;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < s->ndraws && n < AVP_DRAWS_MAX; i++) {
        d[n] = s->draws[i];
        float lmn[3], lmx[3];
        render_mesh_bounds(d[n].mesh, lmn, lmx);
        if (!d[n].palette) // (a skin: its bones frame it below)
            for (int a = 0; a < 3; a++) {
                float c = d[n].rows[a][3], e = 0.0f;
                for (int k = 0; k < 3; k++) c += d[n].rows[a][k] * (lmn[k] + lmx[k]) * 0.5f, e += fabsf(d[n].rows[a][k]) * (lmx[k] - lmn[k]) * 0.5f;
                mn[a] = fminf(mn[a], c - e), mx[a] = fmaxf(mx[a], c + e);
            }
        n++;
    }
    if (s->skel && s->bones)
        for (uint32_t b = 0; b < s->skel->nbones; b++)
            for (int a = 0; a < 3; a++) mn[a] = fminf(mn[a], s->bones[b][a][3] - 12.0f), mx[a] = fmaxf(mx[a], s->bones[b][a][3] + 12.0f);
    if (s->mn && s->mx) memcpy(mn, s->mn, 12), memcpy(mx, s->mx, 12);
    if (mn[0] > mx[0]) mn[0] = mn[1] = mn[2] = -50.0f, mx[0] = mx[1] = mx[2] = 50.0f;
    // the sockets' preview meshes (Unreal's Add Preview Asset)
    if (s->skel && s->bones && v->sockets)
        for (uint32_t i = 0; i < s->skel->nsockets && n < AVP_DRAWS_MAX; i++) {
            const AnimSocket* k = &s->skel->sockets[i];
            if (!k->has_preview) continue;
            PreviewDraw* pd = &d[n++];
            memset(pd, 0, sizeof *pd);
            pd->mesh = k->preview_mesh;
            anim_socket_rows(s->skel, k, s->bones, pd->rows);
            pd->color[0] = pd->color[1] = pd->color[2] = 0.6f, pd->roughness = 0.45f;
            pd->selected = (int)i == v->socket;
        }
    float e[3] = { (mx[0] - mn[0]) * 0.5f, (mx[1] - mn[1]) * 0.5f, (mx[2] - mn[2]) * 0.5f };
    v->radius = fmaxf(sqrtf(dot3(e, e)), 5.0f);
    if (!v->cam.framed) avp_frame(v, mn, mx);
    // the input (an invisible button under everything: the toolbar's buttons drawn over it take their clicks)
    ImGuiIO* io = igGetIO_Nil();
    igSetCursorScreenPos((ImVec2){ x, y });
    igSetNextItemAllowOverlap();
    igInvisibleButton("##avp", (ImVec2){ w, h }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas or a tile drawn with its own name: no description)
    bool hovered = igIsItemHovered(0), active = igIsItemActive();
    // the gizmo takes the press; a pick is a release without a drag (a drag from a bone orbits, it doesn't pick it)
    bool press = igIsItemClicked(ImGuiMouseButton_Left) && !igIsKeyDown_Nil(ImGuiKey_L) && !s->lock_left;
    bool click = hovered && igIsMouseReleased_Nil(ImGuiMouseButton_Left) && io->MouseDragMaxDistanceSqr[0] < 25.0f && !igIsKeyDown_Nil(ImGuiKey_L) &&
                 !s->lock_left;
    r->hovered = hovered;
    view_axes(v, x, y, x + w, y + h);
    if (!v->dragging && v->gizmo_hot < 0) input(v, hovered, active && !(s->lock_left && igIsMouseDown_Nil(ImGuiMouseButton_Left)), mn, mx, v->radius, r);
    view_axes(v, x, y, x + w, y + h);
    // the LODs: by the distance (cull.comp's choice) or forced; the triangles drawn
    uint32_t tris = 0, nl_max = 1, lod_tris[4] = { 0 }, shown = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lt[4];
        uint32_t nl = d[i].palette ? 1u : render_mesh_lods(d[i].mesh, lt, NULL);
        float sc = sqrtf(d[i].rows[0][0] * d[i].rows[0][0] + d[i].rows[1][0] * d[i].rows[1][0] + d[i].rows[2][0] * d[i].rows[2][0]);
        float c[3] = { d[i].rows[0][3] - v->eye[0], d[i].rows[1][3] - v->eye[1], d[i].rows[2][3] - v->eye[2] };
        uint32_t lod = v->lod >= 0 ? (uint32_t)v->lod : nl > 1 ? render_mesh_lod_for(d[i].mesh, sc, sqrtf(dot3(c, c)), v->focal) : 0u;
        if (lod >= nl) lod = nl - 1;
        d[i].lod = lod;
        if (nl > nl_max) {
            nl_max = nl;
            for (int k = 0; k < 4; k++) lod_tris[k] = lt[k];
        }
        if (nl > 1 && lod > shown) shown = lod;
        if (nl >= 1) tris += d[i].palette ? 0u : (nl > 1 ? lt[lod] : lt[0]);
        if (d[i].palette) { // (a skin: one LOD)
            uint32_t st[4];
            render_mesh_lods(d[i].mesh, st, NULL);
            tris += st[0];
        }
    }
    if (nl_max == 1 && n) render_mesh_lods(d[0].mesh, lod_tris, NULL);
    v->tris = tris, v->draws = n, v->lods = nl_max, v->lod_shown = shown;
    // the GPU's picture
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_PushClipRect(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, true);
    float fs = io->DisplayFramebufferScale.x > 0.0f ? io->DisplayFramebufferScale.x : 1.0f;
    PreviewView pv;
    memset(&pv, 0, sizeof pv);
    memcpy(pv.eye, v->eye, 12), memcpy(pv.fwd, v->fwd, 12), memcpy(pv.right, v->right, 12), memcpy(pv.up, v->up, 12);
    pv.fov_deg = v->fov;
    memcpy(pv.target, v->cam.target, 12);
    pv.target[0] = (mn[0] + mx[0]) * 0.5f, pv.target[1] = (mn[1] + mx[1]) * 0.5f;
    pv.radius = v->radius, pv.floor_z = fminf(mn[2], 0.0f);
    pv.w = (uint32_t)(w * fs), pv.h = (uint32_t)(h * fs);
    pv.mode = (uint32_t)mode;
    pv.flags = (v->floor && !test ? PREVIEW_FLOOR : 0u) | (v->grid && !test ? PREVIEW_GRID : 0u) | (v->env == AVP_ENV_SKY && !test ? PREVIEW_SKY : 0u);
    pv.time = (float)igGetTime();
    if (v->light_custom) {
        float ly = v->light_yaw * DEG2RAD, lp = v->light_pitch * DEG2RAD;
        pv.light_dir[0] = cosf(lp) * cosf(ly), pv.light_dir[1] = cosf(lp) * sinf(ly), pv.light_dir[2] = sinf(lp);
        pv.flags |= PREVIEW_LIGHT_WORLD;
    }
    pv.light_scale = v->light_scale;
    uint32_t gen = 0;
    float uvm[2];
    VkImageView iv = render_preview(key, &pv, d, n, &gen, uvm);
    if (iv) {
        void* t = bp_preview_texture(key, (void*)iv, gen);
        ImDrawList_AddImage(dl, (ImTextureRef_c){ NULL, (ImTextureID)(uintptr_t)t }, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, (ImVec2){ 0, 0 },
                            (ImVec2){ uvm[0], uvm[1] }, THEME_IMAGE_TINT);
        r->drawn = true;
        G.frames[key & 63u]++;
    } else {
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_BOTTOM),
                                           TH(TK_PREVIEW_BOTTOM));
        const char* msg = TR("The GPU preview is not available.");
        ImVec2 ts = igCalcTextSize(msg, NULL, false, -1);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + (w - ts.x) * 0.5f, y + h * 0.5f }, THA(TK_N15, 200), msg, NULL);
    }
    // the overlays (the test's picture is the GPU's: they don't matter to it)
    ImVec2 mouse = io->MousePos;
    int hit_bone = -1, hit_socket = -1, hit_body = -1;
    if (s->phys && s->skel && s->bones && (v->bodies || v->constraints)) draw_bodies(dl, v, s, mouse, &hit_body);
    if (s->skel && s->bones && v->bones != AVP_BONES_NONE) draw_bones(dl, v, s, mouse, &hit_bone);
    if (s->skel && s->bones && v->sockets) draw_sockets(dl, v, s, mouse, &hit_socket, press && hovered, r);
    if (mode == PREVIEW_UV && v->uv_layout && n) draw_uv_layout(dl, v, &d[0]);
    // a click (not a drag, not on the gizmo): the socket, else the body, else the bone under it
    if (click && hovered && !v->dragging && v->gizmo_hot < 0) {
        if (hit_socket >= 0) r->socket = hit_socket, v->socket = hit_socket;
        else if (hit_body >= 0) r->body = hit_body;
        else if (hit_bone >= 0) r->bone = hit_bone;
        else r->clicked_empty = true;
        if (hit_socket < 0) v->socket = -1;
    }
    if (v->dragging) r->busy = true;
    // the text: the controls' hint, the statistics
    float fh = igGetFontSize();
    const char* hint = s->hint ? s->hint : TR("Left drag: orbit | middle: pan | wheel: zoom | right drag + WASD: fly | F: frame | L + drag: light");
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 10.0f, y + h - fh - 8.0f }, THA(TK_N14, 170), hint, NULL);
    if (v->stats) {
        const PreviewStats* ps = render_preview_stats();
        char st[200];
        snprintf(st, sizeof st, TR("%u triangles | %u meshes | LOD %u of %u | GPU %.2f ms | CPU %.2f ms"), tris, n, shown, nl_max, ps->gpu_ms,
                 G.cpu_ms);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 10.0f, y + h - fh * 2.0f - 10.0f }, THA(TK_N15, 200), st, NULL);
    }
    ImDrawList_PopClipRect(dl);
    // the toolbar (top left) and the tabs (top right): items over the view
    ImVec2 back = igGetCursorScreenPos();
    igSetCursorScreenPos((ImVec2){ x + 8.0f, y + 8.0f });
    igPushID_Ptr(v);
    bp_preview_mode_button("avp_vm", &v->mode);
    igSameLine(0, 4);
    g_compact = w < igGetFontSize() * 44.0f;
    show_menu(v, s);
    igSameLine(0, 4);
    light_menu(v);
    igSameLine(0, 4);
    camera_menu(v, mn, mx);
    if (nl_max > 1 || v->lod >= 0) {
        igSameLine(0, 4);
        lod_menu(v, nl_max, lod_tris);
    }
    if (s->tabs) {
        float tw = 0.0f, fhh = igGetFrameHeight();
        for (int t = 0; t < AVP_TAB_COUNT; t++)
            if (s->tabs & AVP_TAB(t)) tw += fhh * 0.95f + igCalcTextSize(TR(g_tab_names[t]), NULL, true, -1).x + igGetStyle()->FramePadding.x * 2.0f + 6.0f;
        igSetCursorScreenPos((ImVec2){ fmaxf(x + w - tw - 8.0f, x + 8.0f), y + 8.0f + (w < tw + fhh * 22.0f ? fhh + 4.0f : 0.0f) });
        bool first = true;
        for (int t = 0; t < AVP_TAB_COUNT; t++) {
            if (!(s->tabs & AVP_TAB(t))) continue;
            if (!first) igSameLine(0, 4);
            first = false;
            char id[24];
            snprintf(id, sizeof id, "avp_tab%d", t);
            if (ui_icon_button(id, g_tab_icons[t], TR(g_tab_names[t]), t == s->tab ? TH(TK_ACCENT) : TH(TK_N15), t == s->tab, TR(g_tab_tips[t])) && t != s->tab)
                r->tab = t;
        }
    }
    igPopID();
    igSetCursorScreenPos(back);
    igDummy((ImVec2){ 0, 0 }); // (ImGui: a moved cursor needs an item after it)
    double ms = (re_time_seconds() - t0) * 1000.0;
    v->cpu_ms = ms;
    G.frame_views++, G.frame_cpu_ms += ms;
}

// --- Helpers for the editors ---

uint32_t avp_skel_draws(const SkelMesh* m, const float (*bones)[3][4], int sel_bone, int sel_part, PreviewDraw* out, uint32_t cap,
                        float (*pal)[3][4], float mn[3], float mx[3]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->nparts && n < cap; i++) {
        const SkPart* p = &m->parts[i];
        PreviewDraw* d = &out[n++];
        memset(d, 0, sizeof *d);
        d->mesh = p->mesh, d->material = p->material;
        anim_part_rows(p, bones[p->bone], d->rows);
        memcpy(d->color, p->color, 12);
        d->roughness = p->roughness, d->metallic = p->metallic;
        d->selected = (int)i == sel_part || (sel_part < 0 && p->bone == sel_bone);
        if (mn)
            for (int a = 0; a < 3; a++) mn[a] = fminf(mn[a], d->rows[a][3] - 20.0f), mx[a] = fmaxf(mx[a], d->rows[a][3] + 20.0f);
    }
    if (m->nskins) { // the skins: bent by the palette (each bone's pose times its bind's inverse)
        for (uint32_t b = 0; b < m->nbones; b++)
            for (int a = 0; a < 3; a++)
                for (int c = 0; c < 4; c++)
                    pal[b][a][c] = bones[b][a][0] * m->bind_inv[b][0][c] + bones[b][a][1] * m->bind_inv[b][1][c] + bones[b][a][2] * m->bind_inv[b][2][c] +
                                   (c == 3 ? bones[b][a][3] : 0.0f);
        for (uint32_t i = 0; i < m->nskins && n < cap; i++) {
            const SkSkin* k = &m->skins[i];
            PreviewDraw* d = &out[n++];
            memset(d, 0, sizeof *d);
            d->mesh = k->mesh, d->material = k->material;
            d->rows[0][0] = d->rows[1][1] = d->rows[2][2] = 1.0f;
            memcpy(d->color, k->color, 12);
            d->roughness = k->roughness, d->metallic = k->metallic;
            d->palette = (const float(*)[3][4])pal, d->nbones = m->nbones;
        }
    }
    return n;
}

void avp_mesh_draw(PreviewDraw* d, uint32_t mesh, uint32_t material, const float color[3], float roughness, float metallic) {
    memset(d, 0, sizeof *d);
    d->mesh = mesh, d->material = material;
    d->rows[0][0] = d->rows[1][1] = d->rows[2][2] = 1.0f;
    if (color) memcpy(d->color, color, 12);
    else d->color[0] = d->color[1] = d->color[2] = 0.6f;
    d->roughness = roughness, d->metallic = metallic;
}

void avp_socket_move_world(SkelMesh* m, int socket, const float (*bones)[3][4], const float delta[3]) {
    if (!m || socket < 0 || socket >= (int)m->nsockets) return;
    AnimSocket* k = &m->sockets[socket];
    const float(*bm)[4] = bones[k->bone >= 0 && k->bone < (int)m->nbones ? k->bone : 0];
    // into the bone's space: its rows' 3x3 inverted (a bone may be scaled)
    float a = bm[0][0], b = bm[0][1], c = bm[0][2], d = bm[1][0], e = bm[1][1], f = bm[1][2], g = bm[2][0], h = bm[2][1], i = bm[2][2];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) return;
    float id = 1.0f / det;
    float inv[3][3] = { { (e * i - f * h) * id, (c * h - b * i) * id, (b * f - c * e) * id },
                        { (f * g - d * i) * id, (a * i - c * g) * id, (c * d - a * f) * id },
                        { (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id } };
    for (int r = 0; r < 3; r++) k->local.t[r] += inv[r][0] * delta[0] + inv[r][1] * delta[1] + inv[r][2] * delta[2];
    m->dirty = true;
}

bool avp_socket_panel(AssetViewport* v, SkelMesh* m, int sel_bone) {
    bool ch = false;
    igSeparatorText(TR("Sockets"));
    if (igButton(TR("Add Socket"), (ImVec2){ 0, 0 })) {
        AnimSocket* k = anim_socket_add(m, sel_bone >= 0 ? sel_bone : 0);
        if (k) v->socket = (int)(k - m->sockets), ch = true;
    }
    bool one = m->nbones <= 1; // (2026-11-23: a Static Mesh's - its own space, no bones to speak of)
    if (sel_bone < 0 && !one) {
        igSameLine(0, 6);
        igTextDisabled("%s", TR("(on the selected bone; none: the root)"));
    }
    for (uint32_t i = 0; i < m->nsockets; i++) {
        igPushID_Int(9000 + (int)i);
        ui_icon_item(ICON_ACTOR, igGetFontSize(), TH(TK_MAGENTA_L));
        igSameLine(0, 4);
        char lbl[ANIM_NAME + 64];
        if (one) snprintf(lbl, sizeof lbl, "%s", m->sockets[i].name);
        else snprintf(lbl, sizeof lbl, "%s  (%s)", m->sockets[i].name, m->sockets[i].bone >= 0 && m->sockets[i].bone < (int)m->nbones ? m->bones[m->sockets[i].bone].name : "-");
        if (igSelectable_Bool(lbl, v->socket == (int)i, 0, (ImVec2){ 0, 0 })) v->socket = (int)i;
        igPopID();
    }
    if (v->socket < 0 || v->socket >= (int)m->nsockets) {
        if (!m->nsockets) igTextDisabled("%s", one ? TR("No sockets: Add Socket puts one at the mesh's origin; drag it with the gizmo.") : TR("No sockets: Add Socket puts one on the selected bone."));
        return ch;
    }
    AnimSocket* k = &m->sockets[v->socket];
    igSeparatorText(k->name);
    char nm[ANIM_NAME];
    snprintf(nm, sizeof nm, "%s", k->name);
    if (igInputText(PL(TR("Socket Name")), nm, sizeof nm, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll, NULL, NULL) && nm[0]) {
        int other = anim_socket_index(m, nm);
        if (other < 0 || other == v->socket) snprintf(k->name, sizeof k->name, "%s", nm), ch = true;
    }
    if (!one && igBeginCombo(PL(TR("Bone")), k->bone >= 0 && k->bone < (int)m->nbones ? m->bones[k->bone].name : "-", 0)) {
        for (uint32_t b = 0; b < m->nbones; b++)
            if (igSelectable_Bool(m->bones[b].name, k->bone == (int)b, 0, (ImVec2){ 0, 0 })) k->bone = (int16_t)b, ch = true;
        igEndCombo();
    }
    ch |= igDragFloat3(PL(TR("Relative Location")), k->local.t, 0.1f, -1e4f, 1e4f, "%.2f", 0);
    float rd[3];
    anim_quat_euler(k->local.q, rd);
    if (igDragFloat3(PL(TR("Relative Rotation")), rd, 0.5f, -360.0f, 360.0f, "%.1f", 0)) anim_euler_quat(rd, k->local.q), ch = true;
    // the preview mesh (Unreal's Add Preview Asset: shown here only)
    uint32_t cur = k->has_preview ? render_mesh_slot(k->preview_mesh) : UINT32_MAX;
    if (igBeginCombo(PL(TR("Preview Mesh")), cur != UINT32_MAX ? render_mesh_name(cur) : TR("(none)"), 0)) {
        if (igSelectable_Bool(TR("(none)"), !k->has_preview, 0, (ImVec2){ 0, 0 })) k->has_preview = 0, k->preview_mesh = 0, ch = true;
        for (uint32_t sl = 0; sl < render_mesh_count(); sl++)
            if (asset_selectable_mesh(render_mesh_name(sl), cur == sl, render_mesh_id(sl))) k->preview_mesh = render_mesh_id(sl), k->has_preview = 1, ch = true;
        igEndCombo();
    }
    igTextDisabled("%s", TR("Drag the gizmo's arrows in the view to move it. The preview mesh is shown in the editors only."));
    if (igButton(TR("Delete Socket"), (ImVec2){ 0, 0 })) {
        memmove(k, k + 1, sizeof *k * (m->nsockets - (uint32_t)v->socket - 1u));
        m->nsockets--, v->socket = -1, ch = true;
    }
    if (ch) m->dirty = true;
    return ch;
}
