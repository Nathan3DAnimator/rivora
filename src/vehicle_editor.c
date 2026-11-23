// The Vehicle editor (vehicle.h; Unreal's Chaos Vehicle Blueprint's settings, in one window): the chassis and the gears
// on the left with the parts' list, the vehicle in the asset viewport in the middle (asset_viewport.h, Q7: its body
// model or box, its wheels, floats, wings and rotors drawn by the GPU, every part's handle over them; click one to
// pick it), the picked part's numbers on the right.
#include "vehicle_editor.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "ui_widget.h"
#include "vehicle.h"
#include "asset_viewer.h"
#include "render.h"
#include "asset_viewport.h"
#include "bp_preview.h"

static struct {
    VehicleAsset* a;
    int part;
    bool unsaved;
    char status[96];
    AssetViewport vp; // (Q7)
} E = { NULL, -1, false, "", { 0 } };

static const uint32_t g_part_colors[VP_KIND_COUNT] = { TK_N9, TK_RED, TK_ORANGE, TK_AMBER, TK_GREEN, TK_AMBER, TK_N13,
                                                        TK_BLUE, TK_BLUE, TK_N14, TK_PINK, TK_BLUE, TK_N10 };

bool vehicle_editor_handles(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    return dot && !_stricmp(dot, ".rvehicle");
}
void vehicle_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    E.a = veh_load(vpath), E.part = -1, E.unsaved = false, E.status[0] = 0;
    E.vp.cam.framed = false;
}

bool vehicle_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_MOVEMENT, TR("Vehicles"), ok)) return false;
    igTextDisabled("%s", TR("Vehicle from a template"));
    for (int t = 0; t < VT_COUNT; t++) {
        if (!ui_menu_item(ICON_MOVEMENT, TR(g_veh_template_names[t]), NULL, false, true)) continue;
        char name[64], vp[700];
        snprintf(name, sizeof name, "VH_%s", g_veh_template_names[t]);
        for (char* c = name; *c; c++)
            if (*c == ' ') *c = '_';
        uint32_t h = veh_create(folder_vpath, name, t);
        if (h) {
            snprintf(vp, sizeof vp, "%s/%s.rvehicle", folder_vpath, name);
            vehicle_editor_open(vp);
            made = true;
        }
    }
    igEndMenu();
    return made;
}

// The vehicle in 3D (Q7): a model of its own (its body's, a part's) as imported, else the kind's shapes: the chassis'
// box, the wheels as cylinders across Y, the floats as spheres, the wings as plates, the rotors as discs
static void rows_of(float rows[3][4], const float loc[3], const float rot[3], const float scale[3]) {
    float ax[3][3];
    rotator_axes(rot, ax);
    for (int k = 0; k < 3; k++) {
        for (int a = 0; a < 3; a++) rows[k][a] = ax[a][k] * (scale[a] != 0.0f ? scale[a] : 1.0f);
        rows[k][3] = loc[k];
    }
}
static void view3d(VehicleAsset* a, float w, float h) {
    static PreviewDraw d[VEH_MAX_PARTS + 1];
    uint32_t n = 0;
    static const float body_col[3] = { 0.55f, 0.56f, 0.6f }, tire[3] = { 0.06f, 0.06f, 0.065f }, metal[3] = { 0.7f, 0.7f, 0.72f };
    PreviewDraw* b = &d[n++];
    if (a->body_mesh) {
        avp_mesh_draw(b, a->body_mesh, a->body_material, body_col, 0.4f, 0.3f);
        rows_of(b->rows, a->body_loc, a->body_rot, a->body_scale);
    } else {
        avp_mesh_draw(b, SHAPE_CUBE, 0, body_col, 0.4f, 0.3f);
        for (int k = 0; k < 3; k++) b->rows[k][k] = a->extent[k] * 2.0f / 100.0f; // (the box: 100 cm wide)
    }
    b->selected = false;
    for (uint32_t i = 0; i < a->nparts && n < VEH_MAX_PARTS + 1; i++) {
        const VehPart* p = &a->parts[i];
        PreviewDraw* q = &d[n];
        bool sel = (int)i == E.part;
        if (p->mesh) {
            avp_mesh_draw(q, p->mesh, p->material, metal, 0.5f, 0.2f);
            float loc[3] = { p->mloc[0], p->mloc[1], p->mloc[2] };
            if (p->kind == VP_WHEEL) loc[0] += p->pos[0], loc[1] += p->pos[1], loc[2] += p->pos[2] - p->p[4] * 0.6f;
            rows_of(q->rows, loc, p->mrot, p->mscale);
        } else if (p->kind == VP_WHEEL) { // a cylinder (its height along its Z) turned across Y, hanging below its mount
            float r = p->p[0], wd = fmaxf(p->p[1], 1.0f);
            avp_mesh_draw(q, SHAPE_CYLINDER, 0, tire, 0.8f, 0.0f);
            memset(q->rows, 0, sizeof q->rows);
            q->rows[0][0] = r * 2.0f / 100.0f, q->rows[2][1] = r * 2.0f / 100.0f, q->rows[1][2] = wd / 100.0f;
            q->rows[0][3] = p->pos[0], q->rows[1][3] = p->pos[1], q->rows[2][3] = p->pos[2] - p->p[4] * 0.6f;
        } else if (p->kind == VP_FLOAT) {
            avp_mesh_draw(q, SHAPE_SPHERE, 0, metal, 0.5f, 0.0f);
            for (int k = 0; k < 3; k++) q->rows[k][k] = p->p[0] * 2.0f / 100.0f, q->rows[k][3] = p->pos[k];
        } else if (p->kind == VP_WING) {
            float half = sqrtf(fmaxf(p->p[0], 0.01f)) * 50.0f;
            avp_mesh_draw(q, SHAPE_CUBE, 0, metal, 0.4f, 0.4f);
            bool vertical = fabsf(p->axis[2]) < 0.5f;
            q->rows[0][0] = half * 0.6f / 100.0f, q->rows[1][1] = vertical ? 0.03f : half * 2.0f / 100.0f, q->rows[2][2] = vertical ? half / 100.0f : 0.03f;
            for (int k = 0; k < 3; k++) q->rows[k][3] = p->pos[k];
        } else if (p->kind == VP_ROTOR) {
            float r = sqrtf(p->p[0]) * 0.6f;
            avp_mesh_draw(q, SHAPE_CYLINDER, 0, metal, 0.4f, 0.6f);
            q->rows[0][0] = q->rows[1][1] = r * 2.0f / 100.0f, q->rows[2][2] = 0.02f;
            for (int k = 0; k < 3; k++) q->rows[k][3] = p->pos[k];
        } else
            continue;
        q->selected = sel;
        n++;
    }
    ImVec2 o = igGetCursorScreenPos();
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.draws = d, sc.ndraws = n, sc.sel_bone = sc.sel_body = sc.sel_constraint = -1;
    sc.hint = TR("Click a part's handle to pick it. Left drag: orbit, wheel: zoom, F: frame.");
    // a handle under the mouse (last frame's projection) takes the left button
    ImGuiIO* io = igGetIO_Nil();
    int hot = -1;
    if (E.vp.init)
        for (uint32_t i = 0; i < a->nparts; i++) {
            float sp[2];
            if (avp_to_screen(&E.vp, a->parts[i].pos, sp) && (io->MousePos.x - sp[0]) * (io->MousePos.x - sp[0]) + (io->MousePos.y - sp[1]) * (io->MousePos.y - sp[1]) < 64.0f)
                hot = (int)i;
        }
    sc.lock_left = hot >= 0;
    AvpResult r;
    avp_draw(&E.vp, BP_PV_KEY_VEHICLE, o.x, o.y, w, h, &sc, &r);
    if (r.hovered && hot >= 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) E.part = hot;
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_PushClipRect(dl, o, (ImVec2){ o.x + w, o.y + h }, true);
    float cm[2];
    if (avp_to_screen(&E.vp, a->com, cm)) ImDrawList_AddCircleFilled(dl, (ImVec2){ cm[0], cm[1] }, 4.0f, TH(TK_AMBER), 12); // (the center of mass)
    for (uint32_t i = 0; i < a->nparts; i++) {
        float sp[2];
        if (!avp_to_screen(&E.vp, a->parts[i].pos, sp)) continue;
        bool sel = (int)i == E.part;
        ImU32 col = sel ? TH(TK_AMBER) : TH(g_part_colors[a->parts[i].kind % VP_KIND_COUNT]);
        ImDrawList_AddCircleFilled(dl, (ImVec2){ sp[0], sp[1] }, sel || (int)i == hot ? 6.0f : 4.0f, col, 12);
        if (sel || (int)i == hot) ImDrawList_AddText_Vec2(dl, (ImVec2){ sp[0] + 8, sp[1] - 8 }, TH(TK_N15), a->parts[i].name, NULL);
    }
    ImDrawList_PopClipRect(dl);
}

// A Static Mesh picked from the project's (0: none), with thumbnails
static bool mesh_combo(const char* label, uint32_t* mesh) {
    bool ch = false;
    uint32_t cur = *mesh ? render_mesh_slot(*mesh) : UINT32_MAX;
    if (igBeginCombo(label, cur != UINT32_MAX ? render_mesh_name(cur) : TR("(none: the kind's shapes)"), 0)) {
        if (igSelectable_Bool(TR("(none: the kind's shapes)"), !*mesh, 0, (ImVec2){ 0, 0 })) *mesh = 0, ch = true;
        for (uint32_t s = SHAPE_COUNT; s < render_mesh_count(); s++) // (the imported ones: the basic shapes are the default)
            if (asset_selectable_mesh(render_mesh_name(s), cur == s, render_mesh_id(s))) *mesh = render_mesh_id(s), ch = true;
        igEndCombo();
    }
    return ch;
}

static bool chassis_ui(VehicleAsset* a) {
    bool ch = false;
    igSeparatorText(TR("Your Model"));
    igTextWrapped("%s", TR("Use your imported mesh as the body. Fastest: select its pieces in the level, Actor > Make Vehicle from Selected."));
    if (mesh_combo(PL(TR("Body Mesh")), &a->body_mesh)) {
        if (a->body_mesh) veh_fit_to_body(a);
        ch = true;
    }
    if (a->body_mesh) {
        ch |= igDragFloat3(PL(TR("Body Offset (cm)")), a->body_loc, 0.5f, -5000.0f, 5000.0f, "%.0f", 0);
        float yaw = a->body_rot[1];
        if (igDragFloat(PL(TR("Body Turn (deg)")), &yaw, 1.0f, -360.0f, 360.0f, "%.0f", 0)) a->body_rot[1] = yaw, ch = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Turn it until its front looks along +X (right in the side view, up in the top view)."));
        if (igButton(TR("Fit to Body Mesh"), (ImVec2){ -1, 0 })) veh_fit_to_body(a), ch = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("The collision box, the center of mass and the wheels without a model of their own fitted to the body's size."));
    }
    igSeparatorText(TR("Chassis"));
    int kind = a->kind;
    const char* kinds[VK_KIND_COUNT];
    for (int k = 0; k < VK_KIND_COUNT; k++) kinds[k] = TR(g_veh_kind_names[k]);
    if (igCombo_Str_arr(PL(TR("Kind")), &kind, kinds, VK_KIND_COUNT, -1)) a->kind = (uint8_t)kind, ch = true;
    ch |= igDragFloat(PL(TR("Mass (kg)")), &a->mass, 5.0f, 1.0f, 1000000.0f, "%.0f", 0);
    ch |= igDragFloat3(PL(TR("Center of Mass (cm)")), a->com, 0.5f, -2000.0f, 2000.0f, "%.0f", 0);
    if (igIsItemHovered(0)) igSetTooltip("%s", TR("Lower is steadier: a car rolls less in turns, a boat rights itself."));
    ch |= igDragFloat3(PL(TR("Half Size (cm)")), a->extent, 0.5f, 1.0f, 5000.0f, "%.0f", 0);
    ch |= igDragFloat(PL(TR("Drag (Cd x A)")), &a->drag, 0.01f, 0.0f, 50.0f, "%.2f", 0);
    ch |= igDragFloat(PL(TR("Downforce")), &a->downforce, 0.01f, 0.0f, 50.0f, "%.2f", 0);
    ch |= igDragFloat(PL(TR("Top Speed (km/h)")), &a->max_speed, 1.0f, 0.0f, 2000.0f, a->max_speed > 0 ? "%.0f" : TR("none"), 0);
    ch |= igDragFloat(PL(TR("Camera Distance")), &a->cam_distance, 5.0f, 0.0f, 20000.0f, "%.0f cm", 0);
    ch |= igDragFloat(PL(TR("Camera Height")), &a->cam_height, 5.0f, -1000.0f, 5000.0f, "%.0f cm", 0);
    igSeparatorText(TR("Gears"));
    for (uint32_t g = 0; g < a->ngears; g++) {
        igPushID_Int((int)g);
        char lb[32];
        snprintf(lb, sizeof lb, "%u", g + 1);
        ch |= igDragFloat(PL(lb), &a->gears[g], 0.01f, 0.05f, 20.0f, "%.2f", 0);
        igPopID();
    }
    ch |= igDragFloat(PL(TR("Reverse")), &a->reverse, 0.01f, 0.05f, 20.0f, "%.2f", 0);
    if (a->ngears < VEH_MAX_GEARS && igSmallButton(TR("+ Gear"))) a->gears[a->ngears] = a->ngears ? a->gears[a->ngears - 1] * 0.8f : 3.0f, a->ngears++, ch = true;
    igSameLine(0, 6);
    if (a->ngears > 1 && igSmallButton(TR("- Gear"))) a->ngears--, ch = true;
    return ch;
}

static bool parts_ui(VehicleAsset* a) {
    bool ch = false;
    igSeparatorText(TR("Parts"));
    for (uint32_t i = 0; i < a->nparts; i++) {
        igPushID_Int((int)i);
        char lb[96];
        snprintf(lb, sizeof lb, "%s  (%s)", a->parts[i].name, TR(veh_part_info(a->parts[i].kind)->name));
        if (igSelectable_Bool(lb, E.part == (int)i, 0, (ImVec2){ 0, 0 })) E.part = (int)i;
        igPopID();
    }
    if (a->nparts < VEH_MAX_PARTS && ui_begin_menu(ICON_PLUS, TR("+ Part"), true)) {
        for (int k = 0; k < VP_KIND_COUNT; k++) {
            if (!ui_menu_item(ICON_MOVEMENT, TR(veh_part_info(k)->name), NULL, false, true)) continue;
            VehPart* p = &a->parts[a->nparts];
            memset(p, 0, sizeof *p);
            p->kind = (uint8_t)k, p->axis[2] = 1.0f;
            if (k == VP_WHEEL) p->axis[1] = 1.0f, p->axis[2] = 0.0f;
            snprintf(p->name, VEH_NAME, "%s%u", veh_part_info(k)->name, a->nparts);
            for (int j = 0; j < VEH_PARAMS; j++) p->p[j] = veh_part_info(k)->def[j];
            E.part = (int)a->nparts++, ch = true;
        }
        igEndMenu();
    }
    return ch;
}

static bool part_ui(VehicleAsset* a, VehPart* p) {
    bool ch = false;
    const VehPartInfo* pi = veh_part_info(p->kind);
    igSeparatorText(TR(pi->name));
    igTextWrapped("%s", TR(pi->tip));
    ch |= igInputText(PL(TR("Name")), p->name, VEH_NAME, 0, NULL, NULL);
    ch |= igInputText(PL(TR("Component")), p->attach, VEH_NAME, 0, NULL, NULL);
    if (igIsItemHovered(0)) igSetTooltip("%s", TR("The actor's component this part moves (its name): a wheel's mesh, a door's panel. Empty: physics only."));
    if (mesh_combo(PL(TR("Mesh")), &p->mesh)) {
        if (p->mesh && !p->attach[0]) snprintf(p->attach, VEH_NAME, "%s", p->name); // (a model moves as a component)
        if (p->mesh && p->mscale[0] == 0.0f) p->mscale[0] = p->mscale[1] = p->mscale[2] = 1.0f;
        if (p->mesh && p->kind != VP_WHEEL) memcpy(p->mloc, p->pos, sizeof p->mloc);
        ch = true;
    }
    if (p->mesh) {
        ch |= igDragFloat3(PL(TR("Mesh Turn (deg)")), p->mrot, 1.0f, -360.0f, 360.0f, "%.0f", 0);
        ch |= igDragFloat3(PL(TR("Mesh Scale")), p->mscale, 0.01f, 0.001f, 1000.0f, "%.3f", 0);
        if (p->kind != VP_WHEEL) ch |= igDragFloat3(PL(TR("Mesh Place (cm)")), p->mloc, 0.5f, -5000.0f, 5000.0f, "%.0f", 0);
    }
    if (p->kind == VP_PISTON || p->kind == VP_GEAR) {
        ch |= igInputText(PL(TR("Link")), p->link, VEH_NAME, 0, NULL, NULL);
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("A piston: the part it reaches. A gear: Engine, Steer, a wheel or a part it turns with."));
    }
    if (p->kind == VP_HINGE || p->kind == VP_SLIDER || p->kind == VP_PISTON || p->kind == VP_WING) {
        int in = p->input;
        const char* names[VI_COUNT];
        for (int k = 0; k < VI_COUNT; k++) names[k] = TR(g_veh_input_names[k]);
        if (igCombo_Str_arr(PL(TR("Input")), &in, names, VI_COUNT, -1)) p->input = (uint8_t)in, ch = true;
    }
    ch |= igDragFloat3(PL(TR("Position (cm)")), p->pos, 0.5f, -5000.0f, 5000.0f, "%.0f", 0);
    if (p->kind != VP_ENGINE && p->kind != VP_GEARBOX && p->kind != VP_DIFFERENTIAL && p->kind != VP_FLOAT && p->kind != VP_BOGIE)
        ch |= igDragFloat3(PL(TR("Axis")), p->axis, 0.01f, -1.0f, 1.0f, "%.2f", 0);
    for (int k = 0; k < pi->nparams; k++) {
        igPushID_Int(k);
        float span = pi->hi[k] - pi->lo[k];
        ch |= igDragFloat(PL(TR(pi->params[k])), &p->p[k], fmaxf(span / 400.0f, 0.001f), pi->lo[k], pi->hi[k], "%.3g", ImGuiSliderFlags_AlwaysClamp);
        igPopID();
    }
    if (igButton(TR("Delete Part"), (ImVec2){ 0, 0 })) {
        uint32_t k = (uint32_t)(p - a->parts);
        memmove(p, p + 1, sizeof *p * (a->nparts - k - 1));
        a->nparts--, E.part = -1, ch = true;
    }
    return ch;
}

void vehicle_editor_draw(Editor* ed) {
    (void)ed;
    if (!E.a) return;
    VehicleAsset* a = E.a;
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###vehicle_editor", a->name, E.unsaved ? "*" : "", TR("Vehicle"));
    float fs = igGetFontSize();
    igSetNextWindowSize((ImVec2){ fs * 70, fs * 40 }, ImGuiCond_FirstUseEver);
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        bool save = igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
                    (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false));
        if (save) {
            bool ok = veh_save(a);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        igSameLine(0, 10);
        if (ui_begin_menu(ICON_UNDO, TR("Reset to Template"), true)) {
            for (int t = 0; t < VT_COUNT; t++)
                if (ui_menu_item(ICON_MOVEMENT, TR(g_veh_template_names[t]), NULL, false, true)) veh_template(a, t), E.part = -1, ch = true;
            igEndMenu();
        }
        igSameLine(0, 10);
        igTextDisabled("%s", TR("Changes apply to the vehicles playing at once."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        float h = igGetContentRegionAvail().y;
        igBeginChild_Str("left", (ImVec2){ fs * 17, h }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-fs * 7);
        ch |= chassis_ui(a);
        ch |= parts_ui(a);
        igPopItemWidth();
        igEndChild();
        igSameLine(0, 6);
        float mid = igGetContentRegionAvail().x - fs * 19;
        igBeginChild_Str("views", (ImVec2){ mid, h }, ImGuiChildFlags_Borders, 0);
        view3d(a, igGetContentRegionAvail().x, igGetContentRegionAvail().y);
        igEndChild();
        igSameLine(0, 6);
        igBeginChild_Str("part", (ImVec2){ 0, h }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-fs * 8);
        if (E.part >= 0 && E.part < (int)a->nparts) ch |= part_ui(a, &a->parts[E.part]);
        else igTextDisabled("%s", TR("Pick a part in the list or in the views."));
        igPopItemWidth();
        igEndChild();
    }
    igEnd();
    if (ch) E.unsaved = true, a->version++, a->dirty = true;
    if (!open) E.a = NULL;
}

void vehicle_editor_close(void) { E.a = NULL, E.part = -1; }
