// The second 3D view (Window > Viewport 2, notas-dev/render.md): a window of its own (docked, floating, on another
// monitor) showing the level from a camera of its own, ray traced by render_view2 (view2.comp) after the main view.
//   * Perspective (right button + WASD / QE flies, the wheel sets the speed) or Top / Front / Side, orthographic (the
//     right or middle button pans, the wheel zooms), like Unreal's viewports
//   * Lit, Unlit, Normals, Path Tracing (accumulates while the view and the scene stay)
//   * a click selects the actor under the mouse (its meshes' boxes, as the main viewport's pick)
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

#include "bp_internal.h"
#define THEME_SCENE // (overlays on the 3D scene: theme.h)
#include "theme.h"
#include "editor.h"
#include "gi.h"
#include "i18n.h"
#include "level.h"
#include "render.h"

enum { V2P_PERSPECTIVE = 0, V2P_TOP, V2P_FRONT, V2P_SIDE, V2P_COUNT };

static void cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// The view's axes and origin for the projection
static void v2_axes(const Editor* ed, View2* v) {
    if (ed->v2_proj == V2P_PERSPECTIVE) {
        level_camera_basis(&ed->cam2, v->fwd, v->right, v->up);
        memcpy(v->pos, ed->cam2.pos, 12);
        v->ortho_half = 0.0f;
        return;
    }
    LevelCamera c = ed->cam2;
    c.pitch = 0.0f, c.yaw = ed->v2_proj == V2P_SIDE ? 90.0f : 0.0f;
    float f[3], r[3], u[3];
    level_camera_basis(&c, f, r, u);
    if (ed->v2_proj == V2P_TOP) { // looking down, +X up the screen
        float down[3] = { 0, 0, -1 };
        memcpy(v->fwd, down, 12);
        float x[3] = { 1, 0, 0 };
        cross3(v->fwd, x, v->right); // (right = fwd x screen-up)
        memcpy(v->up, x, 12);
    } else {
        memcpy(v->fwd, f, 12), memcpy(v->right, r, 12);
        cross3(r, f, v->up);
    }
    memcpy(v->pos, ed->v2_target, 12);
    v->ortho_half = fmaxf(ed->v2_ortho, 10.0f);
}

// A point of the window (pixels from its top left) as a ray into the world
static void v2_ray(const View2* v, float px, float py, float w, float h, float o[3], float d[3]) {
    float nx = px / w * 2.0f - 1.0f, ny = py / h * 2.0f - 1.0f, aspect = w / h;
    if (v->ortho_half > 0.0f) {
        for (int k = 0; k < 3; k++)
            o[k] = v->pos[k] + v->right[k] * nx * v->ortho_half * aspect - v->up[k] * ny * v->ortho_half - v->fwd[k] * 500000.0f, d[k] = v->fwd[k];
        return;
    }
    float t = tanf(fminf(fmaxf(v->fov_deg, 5.0f), 170.0f) * 0.5f * 0.0174533f) / aspect;
    float l = 0.0f;
    for (int k = 0; k < 3; k++) d[k] = v->fwd[k] + v->right[k] * nx * t * aspect - v->up[k] * ny * t, l += d[k] * d[k];
    l = sqrtf(l);
    for (int k = 0; k < 3; k++) o[k] = v->pos[k], d[k] /= l;
}

// A world point on the window (false: behind / outside)
static bool v2_project(const View2* v, const float p[3], float w, float h, float* sx, float* sy) {
    float rel[3] = { p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2] }, aspect = w / h;
    float x = rel[0] * v->right[0] + rel[1] * v->right[1] + rel[2] * v->right[2];
    float y = rel[0] * v->up[0] + rel[1] * v->up[1] + rel[2] * v->up[2];
    float z = rel[0] * v->fwd[0] + rel[1] * v->fwd[1] + rel[2] * v->fwd[2];
    float nx, ny;
    if (v->ortho_half > 0.0f) nx = x / (v->ortho_half * aspect), ny = -y / v->ortho_half;
    else {
        if (z < 1.0f) return false;
        float t = tanf(fminf(fmaxf(v->fov_deg, 5.0f), 170.0f) * 0.5f * 0.0174533f) / aspect;
        nx = x / (z * t * aspect), ny = -y / (z * t);
    }
    *sx = (nx * 0.5f + 0.5f) * w, *sy = (ny * 0.5f + 0.5f) * h;
    return true;
}

void view2_window(Editor* ed, double dt) {
    ed->v2_on = false;
    if (!ed->show_view2) return;
    if (ed->cam2.fov_deg <= 0.0f) { // (its first opening: the main camera's view, then the top looks around it)
        ed->cam2 = ed->cam;
        float f[3], r[3], u[3];
        level_camera_basis(&ed->cam, f, r, u);
        for (int k = 0; k < 3; k++) ed->v2_target[k] = ed->cam.pos[k] + f[k] * 1000.0f;
        ed->v2_ortho = 1500.0f;
    }
    const ImGuiViewport* mv = igGetMainViewport();
    igSetNextWindowSize((ImVec2){ mv->Size.x * 0.35f, mv->Size.y * 0.4f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ mv->Pos.x + mv->Size.x * 0.05f, mv->Pos.y + mv->Size.y * 0.45f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    bool open = true;
    char title[96];
    snprintf(title, sizeof title, "%s###view2", TR("Viewport 2"));
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 2, 2 });
    bool shown = igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    igPopStyleVar(1);
    if (shown) {
        static const char* const proj[V2P_COUNT] = { "Perspective", "Top", "Front", "Side" };
        static const char* const modes[VIEW2_MODE_COUNT] = { "Lit", "Unlit", "Normals", "Path Tracing" };
        const char* pl[V2P_COUNT];
        const char* ml[VIEW2_MODE_COUNT];
        for (int k = 0; k < V2P_COUNT; k++) pl[k] = TR(proj[k]);
        for (int k = 0; k < VIEW2_MODE_COUNT; k++) ml[k] = TR(modes[k]);
        igSetNextItemWidth(igGetFontSize() * 8.0f);
        igCombo_Str_arr("##v2proj", &ed->v2_proj, pl, V2P_COUNT, -1);
        igSameLine(0, 6);
        igSetNextItemWidth(igGetFontSize() * 8.0f);
        igCombo_Str_arr("##v2mode", &ed->v2_mode, ml, VIEW2_MODE_COUNT, -1);
        if (ed->v2_mode == VIEW2_PATH_TRACING) {
            igSameLine(0, 10);
            igAlignTextToFramePadding();
            igTextDisabled(TR("%u samples"), render_view2_samples());
        }
        ImVec2 av = igGetContentRegionAvail(), p0 = igGetCursorScreenPos();
        if (av.x < 16.0f || av.y < 16.0f) {
            igEnd();
            return;
        }
        if (!gi_available()) {
            igTextDisabled("%s", TR("The second view is ray traced: it needs a GPU with ray tracing."));
        } else {
            // its picture (registered with the UI again when it was made again: a new size)
            uint32_t gen = 0;
            VkImageView iv = render_view2_image(&gen);
            if (iv && gen != ed->v2_gen) {
                if (ed->v2_tex) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)ed->v2_tex);
                ed->v2_tex = (void*)ImGui_ImplVulkan_AddTexture(iv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                ed->v2_gen = gen;
            }
            if (ed->v2_tex && iv) igImage((ImTextureRef_c){ NULL, (ImTextureID)(uintptr_t)ed->v2_tex }, av, (ImVec2){ 0, 0 }, (ImVec2){ 1, 1 });
            else igDummy(av);
            igSetCursorScreenPos(p0);
            igInvisibleButton("v2area", av, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle); // tips: none (a canvas, a node or a tile drawn with its own name and text)
            ImGuiIO* io = igGetIO_Nil();
            bool hov = igIsItemHovered(0), act = igIsItemActive();
            View2* v = &ed->v2;
            v->fov_deg = ed->cam2.fov_deg > 1.0f ? ed->cam2.fov_deg : 90.0f;
            v2_axes(ed, v);
            // the camera: perspective flies, the orthographic ones pan and zoom
            if (ed->v2_proj == V2P_PERSPECTIVE) {
                if (act && igIsMouseDown_Nil(ImGuiMouseButton_Right)) {
                    ed->cam2.yaw += io->MouseDelta.x * ed->mouse_sens;
                    ed->cam2.pitch = fminf(fmaxf(ed->cam2.pitch - io->MouseDelta.y * ed->mouse_sens * (ed->invert_y ? -1.0f : 1.0f), -89.0f), 89.0f);
                    float f[3], r[3], u[3];
                    level_camera_basis(&ed->cam2, f, r, u);
                    float mx = (float)(igIsKeyDown_Nil(ImGuiKey_D) - igIsKeyDown_Nil(ImGuiKey_A));
                    float mz = (float)(igIsKeyDown_Nil(ImGuiKey_W) - igIsKeyDown_Nil(ImGuiKey_S));
                    float my = (float)(igIsKeyDown_Nil(ImGuiKey_E) - igIsKeyDown_Nil(ImGuiKey_Q));
                    float step = (ed->cam2.speed > 1.0f ? ed->cam2.speed : 1000.0f) * (float)dt * (io->KeyShift ? 3.0f : 1.0f);
                    for (int k = 0; k < 3; k++) ed->cam2.pos[k] += (f[k] * mz + r[k] * mx) * step;
                    ed->cam2.pos[2] += my * step;
                    if (hov && io->MouseWheel != 0.0f) ed->cam2.speed = fminf(fmaxf(ed->cam2.speed * powf(1.25f, io->MouseWheel), 10.0f), 20000.0f);
                } else if (hov && io->MouseWheel != 0.0f) { // (the wheel: forward / back)
                    float f[3], r[3], u[3];
                    level_camera_basis(&ed->cam2, f, r, u);
                    for (int k = 0; k < 3; k++) ed->cam2.pos[k] += f[k] * io->MouseWheel * 100.0f;
                }
            } else {
                float k = v->ortho_half * 2.0f / av.y; // cm per pixel
                if (act && (igIsMouseDown_Nil(ImGuiMouseButton_Right) || igIsMouseDown_Nil(ImGuiMouseButton_Middle)))
                    for (int a = 0; a < 3; a++) ed->v2_target[a] -= v->right[a] * io->MouseDelta.x * k - v->up[a] * io->MouseDelta.y * k;
                if (hov && io->MouseWheel != 0.0f) ed->v2_ortho = fminf(fmaxf(ed->v2_ortho * powf(0.85f, io->MouseWheel), 50.0f), 500000.0f);
            }
            v2_axes(ed, v);
            // a click selects (the actor whose meshes' boxes the ray meets first; Shift: added)
            if (hov && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
                float o[3], d[3];
                v2_ray(v, io->MousePos.x - p0.x, io->MousePos.y - p0.y, av.x, av.y, o, d);
                Entity e = viewport_pick_ray(ed, o, d);
                if (e) editor_select(ed, e, io->KeyShift ? SEL_ADD : SEL_REPLACE);
                else if (!io->KeyShift) editor_select_none(ed);
            }
            // the selection: a ring at its place
            const CTransform* st = ed->selected ? ecs_get(&ed->world, ed->selected, COMP_TRANSFORM) : NULL;
            float sx, sy;
            if (st && v2_project(v, st->location, av.x, av.y, &sx, &sy))
                ImDrawList_AddCircle(igGetWindowDrawList(), (ImVec2){ p0.x + sx, p0.y + sy }, 9.0f, TH(TK_ORANGE), 20, 2.5f);
            ImDrawList_AddText_Vec2(igGetWindowDrawList(), (ImVec2){ p0.x + 8, p0.y + 6 }, THA(TK_N15, 192), pl[ed->v2_proj], NULL);
            float fs = io->DisplayFramebufferScale.x > 0.0f ? io->DisplayFramebufferScale.x : 1.0f;
            v->w = (uint32_t)(av.x * fs), v->h = (uint32_t)(av.y * fs);
            v->mode = (uint32_t)ed->v2_mode;
            ed->v2_on = true;
        }
    }
    igEnd();
    if (!open) ed->show_view2 = false;
}
