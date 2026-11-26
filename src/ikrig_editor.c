// The IK Rig and Physics Asset editors (ikrig.h; Unreal's IK Rig Editor and Physics Asset Editor, simpler): the
// skeleton's bones on the left, the asset viewport in the middle (asset_viewport.h, Q7: the Skeletal Mesh itself drawn
// by the GPU at the pose, its bones, the bodies and constraints over it, the goals), the selected item's properties on
// the right. The window is the animation editors' (###asset_persona): its tabs go to the mesh's Model, Skeleton and
// Animation without closing it.
//   - IK Rig: goals (drag one in the preview: the solvers reach it live), solvers in order, bone settings.
//   - Physics Asset: bodies and constraints; Simulate drops the skeleton as a ragdoll in the preview (a push with the
//     mouse), Generate makes the bodies again from the bones.
#include "editor_docs.h"
#include "ikrig_editor.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "anim.h"
#include "anim_editor.h"
#include "asset_picker.h"
#include "asset_viewer.h"
#include "asset_viewport.h"
#include "bp_preview.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "i18n.h"
#include "ikrig.h"
#include "ui_icons.h"
#include "ui_tips.h"
#include "ui_widget.h"

#define PREVIEW_ENT THA(TK_CYAN, 127) // (the preview's state in ikrig.c: no entity of the level)

static struct {
    IkRig* rig;
    PhysicsAsset* phys;
    int bone, goal, solver, body, constraint; // the selection (-1: none)
    float yaw, pitch, zoom;
    float goal_pos[IK_MAX_GOALS][3];          // the preview's goals (mesh space)
    bool goal_on[IK_MAX_GOALS];
    bool simulate, unsaved;
    int drag_goal;
    char status[96];
    AssetViewport vp; // (Q7)
    int pending_tab;  // a tab clicked: switched after the window (-1: none)
    int chain;        // (Q10) the selected dynamic chain (-1: none)
    int sim_mode;     // (Q10) Simulate as: 0 a ragdoll, 1 an Active Ragdoll, 2 the dynamic bones (the skeleton swaying)
    float sway_t;
} E = { NULL, NULL, -1, -1, -1, -1, -1, 30.0f, 0.0f, 1.0f, { { 0 } }, { 0 }, false, false, -1, "", { 0 }, -1, -1, 0, 0.0f };
static const char* const g_sim_modes[3] = { "Ragdoll", "Active Ragdoll", "Dynamic Bones (Sway)" };

static const char* const g_shape_labels[PHYS_SHAPE_COUNT] = { "Capsule", "Sphere", "Box" };

bool ikrig_editor_handles(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    return dot && (!_stricmp(dot, ".rikrig") || !_stricmp(dot, ".rphys"));
}

static void close_all(void) {
    ikphys_forget(PREVIEW_ENT);
    E.rig = NULL, E.phys = NULL, E.simulate = false, E.drag_goal = -1;
}

void ikrig_editor_close(void) { close_all(); }

void ikrig_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    close_all();
    anim_editor_close(); // (one window: the animation editors')
    const char* dot = strrchr(vpath, '.');
    if (dot && !_stricmp(dot, ".rikrig")) E.rig = ikrig_load(vpath);
    else E.phys = phys_load(vpath);
    E.bone = E.goal = E.solver = E.body = E.constraint = E.chain = -1, E.unsaved = false, E.status[0] = 0;
    memset(E.goal_on, 0, sizeof E.goal_on);
}

bool ikrig_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8);
    if (!ui_begin_menu(ICON_CHARACTER, TR("IK Rig and Physics"), ok)) return false;
    // (Q6) one item each: the asset picker chooses the Skeletal Mesh (no list of every mesh by name here)
    if (ui_menu_item(ICON_CHARACTER, TR("IK Rig..."), NULL, false, true)) content_pick_and_make(CM_IK_RIG, folder_vpath);
    igSetItemTooltip("%s", TR("Pick a Skeletal Mesh: an IK Rig is made for it."));
    if (ui_menu_item(ICON_CAPSULE, TR("Physics Asset..."), NULL, false, true)) content_pick_and_make(CM_PHYSICS_ASSET, folder_vpath);
    igSetItemTooltip("%s", TR("Pick a Skeletal Mesh: its bones' bodies and joints are made."));
    if (ui_menu_item(ICON_CAPSULE, TR("Ragdoll..."), NULL, false, true)) content_pick_and_make(CM_RAGDOLL, folder_vpath);
    igSetItemTooltip("%s", TR("Pick a Skeletal Mesh: a Blueprint actor that falls as a ragdoll."));
    igEndMenu();
    return false; // (made when picked)
}

static int bone_index(const SkelMesh* m, const char* name) {
    for (uint32_t b = 0; m && b < m->nbones; b++)
        if (!_stricmp(m->bones[b].name, name)) return (int)b;
    return -1;
}

// The pose shown: the bind, then the IK (the dragged goals) or the ragdoll
static void preview_pose(const SkelMesh* m, float bones[][3][4]) {
    memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
    static World empty; // (no level: the ground at the feet, nothing to hit)
    if (E.rig) {
        for (uint32_t g = 0; g < E.rig->ngoals; g++)
            if (E.goal_on[g]) ikrig_set_goal(PREVIEW_ENT, E.rig->goals[g].name, E.goal_pos[g], NULL, 1.0f);
        ikrig_apply(&empty, PREVIEW_ENT, E.rig, m, bones);
    } else if (E.phys && E.simulate) {
        float dt = igGetIO_Nil()->DeltaTime;
        dt = dt > 0.0f ? fminf(dt, 1.0f / 20.0f) : 1.0f / 60.0f;
        if (E.sim_mode == 2) { // (Q10) the dynamic bones: the skeleton sways side to side (a walk's sway), the chains follow
            E.sway_t += dt;
            float x = 18.0f * sinf(E.sway_t * 4.4f), y = 6.0f * sinf(E.sway_t * 2.2f);
            for (uint32_t b = 0; b < m->nbones; b++) bones[b][0][3] += x, bones[b][1][3] += y;
        } else phys_apply(&empty, PREVIEW_ENT, E.phys, m, bones, dt);
        dyn_apply(&empty, PREVIEW_ENT, E.phys, m, bones, dt); // (Q10: earrings, hair, tails, straps on top)
    }
}

static void preview_ui(SkelMesh* m) {
    ImVec2 at = igGetCursorScreenPos(), size = igGetContentRegionAvail();
    if (size.x < 50 || size.y < 50) return;
    static float bones[ANIM_MAX_BONES][3][4], pal[ANIM_MAX_BONES][3][4];
    static PreviewDraw draws[ANIM_MAX_PARTS + ANIM_MAX_SKINS];
    preview_pose(m, bones);
    ImGuiIO* io = igGetIO_Nil();
    ImVec2 mp = io->MousePos;
    bool inside = mp.x >= at.x && mp.y >= at.y && mp.x < at.x + size.x && mp.y < at.y + size.y;
    // the goals (last frame's projection): one under the mouse takes the left button from the camera
    int hot_goal = -1;
    if (E.rig && E.vp.init)
        for (uint32_t g = 0; g < E.rig->ngoals; g++) {
            int b = bone_index(m, E.rig->goals[g].bone);
            if (b < 0) continue;
            if (!E.goal_on[g])
                for (int k = 0; k < 3; k++) E.goal_pos[g][k] = m->bind_world[b][k][3];
            float sg[2];
            if (avp_to_screen(&E.vp, E.goal_pos[g], sg) && (mp.x - sg[0]) * (mp.x - sg[0]) + (mp.y - sg[1]) * (mp.y - sg[1]) < 100.0f) hot_goal = (int)g;
        }
    if (inside && hot_goal >= 0 && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) E.drag_goal = hot_goal, E.goal = hot_goal, E.solver = -1;
    if (E.drag_goal >= 0 && !igIsMouseDown_Nil(ImGuiMouseButton_Left)) E.drag_goal = -1;
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.draws = draws, sc.ndraws = avp_skel_draws(m, (const float(*)[3][4])bones, E.bone, -1, draws, ANIM_MAX_PARTS + ANIM_MAX_SKINS, pal, NULL, NULL);
    sc.skel = m, sc.bones = (const float(*)[3][4])bones, sc.sel_bone = E.bone;
    sc.phys = E.phys, sc.sel_body = E.body, sc.sel_constraint = E.constraint;
    sc.tab = E.rig ? AVP_TAB_IKRIG : AVP_TAB_PHYSICS; // (the tabs: the window's toolbar)
    sc.lock_left = hot_goal >= 0 || E.drag_goal >= 0;
    sc.hint = E.rig ? TR("Drag a goal: the solvers reach it. Left drag: orbit, wheel: zoom, F: frame.")
                    : TR("Click a body to select it. Simulate: a ragdoll (Ctrl + click: a push). Left drag: orbit, F: frame.");
    AvpResult r;
    avp_draw(&E.vp, BP_PV_KEY_IKRIG, at.x, at.y, size.x, size.y, &sc, &r);
    if (r.tab >= 0) E.pending_tab = r.tab;
    if (r.body >= 0 && E.phys) E.body = r.body, E.constraint = -1, E.bone = bone_index(m, E.phys->bodies[r.body].bone);
    else if (r.bone >= 0) E.bone = r.bone;
    // a push: Ctrl + click while simulating gives every body a kick toward the camera's right
    if (E.phys && E.simulate && r.hovered && io->KeyCtrl && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        float imp[3], c[3] = { bones[0][0][3], bones[0][1][3], bones[0][2][3] };
        avp_screen_delta(&E.vp, c, 400.0f, -200.0f, imp);
        float d[3] = { c[0] - E.vp.eye[0], c[1] - E.vp.eye[1], c[2] - E.vp.eye[2] }, z = d[0] * E.vp.fwd[0] + d[1] * E.vp.fwd[1] + d[2] * E.vp.fwd[2];
        float k = E.vp.focal / fmaxf(z, 1.0f); // (px per cm there: the same push whatever the zoom)
        for (int q = 0; q < 3; q++) imp[q] *= k;
        phys_add_impulse(PREVIEW_ENT, NULL, imp);
    }
    if (E.rig) { // the goals: a handle each (drag to move: the solvers follow)
        ImDrawList* dl = igGetWindowDrawList();
        ImDrawList_PushClipRect(dl, at, (ImVec2){ at.x + size.x, at.y + size.y }, true);
        for (uint32_t g = 0; g < E.rig->ngoals; g++) {
            if (bone_index(m, E.rig->goals[g].bone) < 0) continue;
            float sg[2];
            if (!avp_to_screen(&E.vp, E.goal_pos[g], sg)) continue;
            ImU32 col = (int)g == E.goal || (int)g == hot_goal ? TH(TK_AMBER) : TH(TK_ORANGE);
            ImDrawList_AddCircle(dl, (ImVec2){ sg[0], sg[1] }, 7.0f, col, 16, 2.0f);
            ImDrawList_AddText_Vec2(dl, (ImVec2){ sg[0] + 9, sg[1] - 8 }, TH(TK_N14), E.rig->goals[g].name, NULL);
        }
        ImDrawList_PopClipRect(dl);
        if (E.drag_goal >= 0) {
            float d[3];
            avp_screen_delta(&E.vp, E.goal_pos[E.drag_goal], io->MouseDelta.x, io->MouseDelta.y, d);
            for (int k = 0; k < 3; k++) E.goal_pos[E.drag_goal][k] += d[k];
            E.goal_on[E.drag_goal] = true;
        }
    }
}

// --- The lists and properties ---
static bool bone_combo(const char* label, char* bone, const SkelMesh* m) {
    bool ch = false;
    if (igBeginCombo(PL(label), bone[0] ? bone : "-", 0)) {
        for (uint32_t b = 0; m && b < m->nbones; b++)
            if (igSelectable_Bool(m->bones[b].name, !strcmp(bone, m->bones[b].name), 0, (ImVec2){ 0, 0 }))
                snprintf(bone, ANIM_NAME, "%s", m->bones[b].name), ch = true;
        igEndCombo();
    }
    return ch;
}

static bool goal_combo(const char* label, char* goal, const IkRig* r) {
    bool ch = false;
    if (igBeginCombo(PL(label), goal[0] ? goal : "-", 0)) {
        for (uint32_t g = 0; g < r->ngoals; g++)
            if (igSelectable_Bool(r->goals[g].name, !strcmp(goal, r->goals[g].name), 0, (ImVec2){ 0, 0 }))
                snprintf(goal, ANIM_NAME, "%s", r->goals[g].name), ch = true;
        igEndCombo();
    }
    return ch;
}

static bool rig_lists(IkRig* r, const SkelMesh* m) {
    bool ch = false;
    igSeparatorText(TR("Goals"));
    for (uint32_t g = 0; g < r->ngoals; g++) {
        igPushID_Int((int)g);
        char lb[160];
        snprintf(lb, sizeof lb, "%s  (%s)", r->goals[g].name, r->goals[g].bone);
        if (igSelectable_Bool(lb, E.goal == (int)g, 0, (ImVec2){ 0, 0 })) E.goal = (int)g, E.solver = -1;
        igPopID();
    }
    if (r->ngoals < IK_MAX_GOALS && igButton(TR("+ Goal on the selected bone"), (ImVec2){ 0, 0 }) && E.bone >= 0 && m) {
        IkRigGoal* g = &r->goals[r->ngoals];
        memset(g, 0, sizeof *g);
        snprintf(g->bone, ANIM_NAME, "%s", m->bones[E.bone].name), snprintf(g->name, ANIM_NAME, "%s_Goal", m->bones[E.bone].name);
        E.goal = (int)r->ngoals++, ch = true;
    }
    igSeparatorText(TR("Solvers (in order)"));
    for (uint32_t s = 0; s < r->nsolvers; s++) {
        igPushID_Int(100 + (int)s);
        char lb[160];
        snprintf(lb, sizeof lb, "%u. %s  %s%s", s + 1, TR(g_ik_solver_names[r->solvers[s].kind]), r->solvers[s].start, r->solvers[s].on ? "" : " (off)");
        if (igSelectable_Bool(lb, E.solver == (int)s, 0, (ImVec2){ 0, 0 })) E.solver = (int)s, E.goal = -1;
        igPopID();
    }
    if (r->nsolvers < IK_MAX_SOLVERS && ui_begin_menu(ICON_PLUS, TR("+ Solver"), true)) {
        for (int k = 0; k < IKS_KIND_COUNT; k++)
            if (ui_menu_item(ICON_CHARACTER, TR(g_ik_solver_names[k]), NULL, false, true)) {
                IkRigSolver* s = &r->solvers[r->nsolvers];
                memset(s, 0, sizeof *s);
                s->kind = (uint8_t)k, s->on = 1, s->iterations = 10, s->root_stiffness = 0.5f, s->axis[0] = 1.0f;
                if (E.bone >= 0 && m) snprintf(s->start, ANIM_NAME, "%s", m->bones[E.bone].name);
                if (E.goal >= 0) snprintf(s->goal, ANIM_NAME, "%s", r->goals[E.goal].name), s->goal_mask = 1u << E.goal;
                E.solver = (int)r->nsolvers++, E.goal = -1, ch = true;
            }
        igEndMenu();
    }
    if (igButton(TR("Reset Preview"), (ImVec2){ 0, 0 })) memset(E.goal_on, 0, sizeof E.goal_on), ikrig_clear_goal(PREVIEW_ENT, NULL);
    return ch;
}

static bool rig_props(IkRig* r, const SkelMesh* m) {
    bool ch = false;
    if (E.goal >= 0 && E.goal < (int)r->ngoals) {
        IkRigGoal* g = &r->goals[E.goal];
        igSeparatorText(TR("Goal"));
        ch |= igInputText(PL(TR("Name")), g->name, ANIM_NAME, 0, NULL, NULL);
        ch |= bone_combo(TR("Bone"), g->bone, m);
        ch |= igInputText(PL(TR("Follow Actor")), g->target_actor, ANIM_NAME, 0, NULL, NULL);
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("An actor of the level by name: the goal follows it while playing (empty: set by the Blueprint node Set IK Goal)."));
        ch |= igDragFloat3(PL(TR("Offset")), g->offset, 0.5f, -1000.0f, 1000.0f, "%.1f", 0);
        ch |= igSliderFloat(PL(TR("Alpha")), &g->alpha, 0.0f, 1.0f, "%.2f", 0);
        bool rot = g->rotation != 0;
        if (igCheckbox(PL(TR("Use Actor Rotation")), &rot)) g->rotation = rot, ch = true;
        if (igButton(TR("Delete Goal"), (ImVec2){ 0, 0 })) {
            memmove(g, g + 1, sizeof *g * (r->ngoals - (uint32_t)E.goal - 1));
            r->ngoals--, E.goal = -1, ch = true;
        }
    } else if (E.solver >= 0 && E.solver < (int)r->nsolvers) {
        IkRigSolver* s = &r->solvers[E.solver];
        igSeparatorText(TR(g_ik_solver_names[s->kind]));
        bool on = s->on != 0;
        if (igCheckbox(PL(TR("Enabled")), &on)) s->on = on, ch = true;
        ch |= bone_combo(s->kind == IKS_FULL_BODY ? TR("Root Bone") : s->kind == IKS_LOOK_AT ? TR("Bone") : TR("Start Bone"), s->start, m);
        if (s->kind == IKS_FULL_BODY) {
            igTextDisabled("%s", TR("Goals of this solver:"));
            for (uint32_t g = 0; g < r->ngoals; g++) {
                bool in = (s->goal_mask >> g) & 1u;
                igPushID_Int((int)g);
                if (igCheckbox(r->goals[g].name, &in)) s->goal_mask = in ? s->goal_mask | (1u << g) : s->goal_mask & ~(1u << g), ch = true;
                igPopID();
            }
            ch |= igSliderFloat(PL(TR("Root Stiffness")), &s->root_stiffness, 0.0f, 1.0f, "%.2f", 0);
        } else ch |= goal_combo(TR("Goal"), s->goal, r);
        if (s->kind == IKS_CHAIN || s->kind == IKS_LIMB) ch |= igDragFloat3(PL(TR("Pole Vector")), s->pole, 0.05f, -1.0f, 1.0f, "%.2f", 0);
        if (s->kind == IKS_LOOK_AT) ch |= igDragFloat3(PL(TR("Aim Axis")), s->axis, 0.05f, -1.0f, 1.0f, "%.2f", 0);
        if (s->kind != IKS_LOOK_AT) ch |= igSliderInt(PL(TR("Iterations")), &s->iterations, 1, 50, "%d", 0);
        if (E.solver > 0 && igButton(TR("Move Up"), (ImVec2){ 0, 0 })) {
            IkRigSolver t = r->solvers[E.solver - 1];
            r->solvers[E.solver - 1] = *s, r->solvers[E.solver] = t, E.solver--, ch = true;
        }
        if (igButton(TR("Delete Solver"), (ImVec2){ 0, 0 })) {
            memmove(s, s + 1, sizeof *s * (r->nsolvers - (uint32_t)E.solver - 1));
            r->nsolvers--, E.solver = -1, ch = true;
        }
    } else if (E.bone >= 0 && m && E.bone < (int)m->nbones) { // the bone's settings
        const char* bn = m->bones[E.bone].name;
        igSeparatorText(bn);
        int at = -1;
        for (uint32_t i = 0; i < r->nbones; i++)
            if (!_stricmp(r->bones[i].bone, bn)) at = (int)i;
        float stiff = at >= 0 ? r->bones[at].stiffness : 0.0f, lim = at >= 0 ? r->bones[at].limit : 0.0f;
        bool c1 = igSliderFloat(PL(TR("Stiffness")), &stiff, 0.0f, 1.0f, "%.2f", 0);
        bool c2 = igSliderFloat(PL(TR("Limit (deg)")), &lim, 0.0f, 180.0f, "%.0f", 0);
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("How far the solvers may turn this bone from its animated direction (0: free)."));
        if ((c1 || c2) && at < 0 && r->nbones < IK_MAX_BONE_SETTINGS) at = (int)r->nbones++, snprintf(r->bones[at].bone, ANIM_NAME, "%s", bn);
        if ((c1 || c2) && at >= 0) r->bones[at].stiffness = stiff, r->bones[at].limit = lim, ch = true;
    } else igTextDisabled("%s", TR("Select a goal, a solver or a bone."));
    return ch;
}

static float fs_of(void) { return igGetFontSize(); }

// (Q10) a montage field (the asset picker on the project's Anim Montages): its name in out (empty: none)
static bool montage_field(const char* label, char name[ANIM_NAME]) {
    const AnimMontage* mo = name[0] ? anim_montage_name(name) : NULL;
    char vp[700] = "", out[700];
    if (mo) ikphys_vpath(mo->path, vp, sizeof vp);
    if (asset_field(label, mo ? mo->name : TR("(none)"), vp, ASSET_ANIM_MONTAGE, AK(ASSET_ANIM_MONTAGE), true, out, sizeof out) != ASSET_FIELD_PICKED) return false;
    const AnimMontage* q = out[0] ? anim_montage_load(out) : NULL;
    snprintf(name, ANIM_NAME, "%s", q ? q->name : "");
    return true;
}

static bool phys_lists(PhysicsAsset* p, const SkelMesh* m) {
    bool ch = false;
    if (igButton(TR("Generate Bodies"), (ImVec2){ 0, 0 }) && m) phys_autogen(p, m), E.body = E.constraint = -1, ch = true;
    if (igIsItemHovered(0)) igSetTooltip("%s", TR("Makes the bodies and constraints again from the bones (the current ones are replaced)."));
    // (Q10) the whole asset: the bodies' density (their masses by volume), the plain ragdoll's tone, the get-up montages
    if (igCollapsingHeader_TreeNodeFlags(TR("Ragdoll Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
        igPushItemWidth(-fs_of() * 6.5f);
        if (p->density <= 0.0f) p->density = 1.0f;
        ch |= igDragFloat(PL(TR("Density (g/cm3)")), &p->density, 0.01f, 0.2f, 3.0f, "%.2f", 0);
        if (igButton(TR("Masses from Volume"), (ImVec2){ 0, 0 })) phys_masses_from_volume(p), ch = true;
        ch |= igSliderFloat(PL(TR("Ragdoll Tone")), &p->tone, 0.0f, 2.0f, "%.2f", 0);
        ch |= montage_field(PL(TR("Get Up Face Down")), p->getup_front);
        ch |= montage_field(PL(TR("Get Up On Its Back")), p->getup_back);
        igPopItemWidth();
    }
    igSeparatorText(TR("Bodies"));
    for (uint32_t i = 0; i < p->nbodies; i++) {
        igPushID_Int((int)i);
        char lb[160];
        snprintf(lb, sizeof lb, "%s  %s%s", p->bodies[i].bone, TR(g_shape_labels[p->bodies[i].shape % PHYS_SHAPE_COUNT]), p->bodies[i].simulate ? "" : TR(" (kinematic)"));
        if (igSelectable_Bool(lb, E.body == (int)i, 0, (ImVec2){ 0, 0 })) E.body = (int)i, E.constraint = E.chain = -1, E.bone = bone_index(m, p->bodies[i].bone);
        igPopID();
    }
    if (p->nbodies < PHYS_MAX_BODIES && igButton(TR("+ Body on the selected bone"), (ImVec2){ 0, 0 }) && E.bone >= 0 && m) {
        PhysBody* b = &p->bodies[p->nbodies];
        memset(b, 0, sizeof *b);
        snprintf(b->bone, ANIM_NAME, "%s", m->bones[E.bone].name);
        b->shape = PHYS_SPHERE, b->simulate = 1, b->radius = 8.0f, b->mass = 3.0f, b->axis[1] = 1.0f;
        b->extent[0] = b->extent[1] = b->extent[2] = 8.0f;
        E.body = (int)p->nbodies++, ch = true;
    }
    igSeparatorText(TR("Constraints"));
    for (uint32_t i = 0; i < p->nconstraints; i++) {
        igPushID_Int(1000 + (int)i);
        const PhysConstraint* c = &p->constraints[i];
        char lb[160];
        snprintf(lb, sizeof lb, "%s - %s", c->parent < p->nbodies ? p->bodies[c->parent].bone : "?", c->child < p->nbodies ? p->bodies[c->child].bone : "?");
        if (igSelectable_Bool(lb, E.constraint == (int)i, 0, (ImVec2){ 0, 0 })) E.constraint = (int)i, E.body = E.chain = -1;
        igPopID();
    }
    // (Q10) the dynamic bones: chains of bones with springs (earrings, hair, a tail, straps, a bag)
    igSeparatorText(TR("Dynamic Bones"));
    for (uint32_t i = 0; i < p->nchains; i++) {
        igPushID_Int(3000 + (int)i);
        char lb[160];
        snprintf(lb, sizeof lb, "%s  (%s)%s", p->chains[i].bone, TR(g_dyn_preset_names[p->chains[i].preset % DYN_PRESET_COUNT]), p->chains[i].on ? "" : TR(" (off)"));
        if (igSelectable_Bool(lb, E.chain == (int)i, 0, (ImVec2){ 0, 0 })) E.chain = (int)i, E.body = E.constraint = -1, E.bone = bone_index(m, p->chains[i].bone);
        igPopID();
    }
    if (p->nchains < PHYS_MAX_CHAINS && ui_dropdown_button("phys_add_chain", ICON_PLUS, TR("+ Chain on the selected bone"), TH(TK_N15), TR("A dynamic chain from the selected bone down, with a preset for what it is")))
        igOpenPopup_Str("phys_add_chain_menu", 0);
    if (igBeginPopup("phys_add_chain_menu", 0)) {
        for (int k = 0; k < DYN_PRESET_COUNT; k++)
            if (ui_menu_item(ICON_PLUS, TR(g_dyn_preset_names[k]), NULL, false, E.bone >= 0 && m)) {
                int c = phys_add_chain(p, m->bones[E.bone].name, k);
                if (c >= 0) E.chain = c, E.body = E.constraint = -1, ch = true;
            }
        igEndPopup();
    }
    return ch;
}

static bool phys_props(PhysicsAsset* p, const SkelMesh* m) {
    bool ch = false;
    if (E.body >= 0 && E.body < (int)p->nbodies) {
        PhysBody* b = &p->bodies[E.body];
        igSeparatorText(TR("Body"));
        ch |= bone_combo(TR("Bone"), b->bone, m);
        int shape = b->shape;
        const char* labels[PHYS_SHAPE_COUNT] = { TR(g_shape_labels[0]), TR(g_shape_labels[1]), TR(g_shape_labels[2]) };
        if (igCombo_Str_arr(PL(TR("Shape")), &shape, labels, PHYS_SHAPE_COUNT, -1)) b->shape = (uint8_t)shape, ch = true;
        bool sim = b->simulate != 0;
        if (igCheckbox(PL(TR("Simulated")), &sim)) b->simulate = sim, ch = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Off: kinematic, it follows the animation and the others hang from it."));
        if (b->shape == PHYS_BOX) ch |= igDragFloat3(PL(TR("Half Size")), b->extent, 0.2f, 0.5f, 500.0f, "%.1f", 0);
        else ch |= igDragFloat(PL(TR("Radius")), &b->radius, 0.2f, 0.5f, 500.0f, "%.1f", 0);
        if (b->shape == PHYS_CAPSULE) ch |= igDragFloat(PL(TR("Length")), &b->length, 0.2f, 0.0f, 1000.0f, "%.1f", 0);
        ch |= igDragFloat3(PL(TR("Offset")), b->offset, 0.2f, -500.0f, 500.0f, "%.1f", 0);
        ch |= igDragFloat3(PL(TR("Axis")), b->axis, 0.02f, -1.0f, 1.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Mass (kg)")), &b->mass, 0.05f, 0.05f, 500.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Physical Animation")), &b->strength, 0.0f, 1.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Muscle")), &b->muscle, 0.0f, 2.0f, "%.2f", 0); // (2026-11-16) the active ragdoll's
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Active Ragdoll: how strongly its joint turns it toward the animation (1 normal, 0 loose, 2 stiff)."));
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("0: limp. Higher: the body is pulled toward the animated pose (jiggle and hits without falling)."));
        if (igButton(TR("Delete Body"), (ImVec2){ 0, 0 })) {
            uint32_t k = (uint32_t)E.body;
            memmove(b, b + 1, sizeof *b * (p->nbodies - k - 1));
            p->nbodies--;
            for (uint32_t c = 0; c < p->nconstraints;) { // its constraints go, the others' indices shift
                PhysConstraint* cs = &p->constraints[c];
                if (cs->parent == k || cs->child == k) {
                    memmove(cs, cs + 1, sizeof *cs * (p->nconstraints - c - 1));
                    p->nconstraints--;
                    continue;
                }
                if (cs->parent > k) cs->parent--;
                if (cs->child > k) cs->child--;
                c++;
            }
            E.body = -1, ch = true;
        }
    } else if (E.constraint >= 0 && E.constraint < (int)p->nconstraints) {
        PhysConstraint* c = &p->constraints[E.constraint];
        igSeparatorText(TR("Constraint"));
        // (Q10) its anatomy: the kind's limits from the bind pose (a knee folds backward only), asymmetric, each editable
        int kind = c->kind < PJ_KIND_COUNT ? c->kind : 0;
        const char* kl[PJ_KIND_COUNT];
        for (int k = 0; k < PJ_KIND_COUNT; k++) kl[k] = TR(g_joint_kind_names[k]);
        if (igCombo_Str_arr(PL(TR("Joint Kind")), &kind, kl, PJ_KIND_COUNT, -1) && m) phys_joint_defaults(p, m, (uint32_t)E.constraint, kind), ch = true;
        bool lim = igDragFloatRange2(PL(TR("Bend (deg)")), &c->bend_min, &c->bend_max, 0.5f, -180.0f, 180.0f, "%.0f", "%.0f", 0);
        lim |= igDragFloatRange2(PL(TR("Sides (deg)")), &c->side_min, &c->side_max, 0.5f, -180.0f, 180.0f, "%.0f", "%.0f", 0);
        lim |= igDragFloatRange2(PL(TR("Twist (deg)")), &c->twist_min, &c->twist_max, 0.5f, -180.0f, 180.0f, "%.0f", "%.0f", 0);
        if (lim) c->swing = fmaxf(fmaxf(-c->bend_min, c->bend_max), fmaxf(-c->side_min, c->side_max)), c->twist = fmaxf(-c->twist_min, c->twist_max), ch = true;
        ch |= igSliderFloat(PL(TR("Joint Damping")), &c->damping, 0.0f, 30.0f, "%.1f", 0);
        ch |= igSliderFloat(PL(TR("Joint Tone")), &c->tone, 0.0f, 1.0f, "%.2f", 0);
        if (igButton(TR("Anatomical Defaults"), (ImVec2){ 0, 0 }) && m) phys_joint_defaults(p, m, (uint32_t)E.constraint, c->kind), ch = true;
        igSameLine(0, 8);
        if (igButton(TR("Delete Constraint"), (ImVec2){ 0, 0 })) {
            memmove(c, c + 1, sizeof *c * (p->nconstraints - (uint32_t)E.constraint - 1));
            p->nconstraints--, E.constraint = -1, ch = true;
        }
    } else if (E.chain >= 0 && E.chain < (int)p->nchains) { // (Q10) a dynamic chain
        DynChain* c = &p->chains[E.chain];
        igSeparatorText(TR("Dynamic Chain"));
        ch |= bone_combo(TR("First Bone"), c->bone, m);
        ch |= bone_combo(TR("Last Bone"), c->end, m);
        int pr = c->preset % DYN_PRESET_COUNT;
        const char* pl[DYN_PRESET_COUNT];
        for (int k = 0; k < DYN_PRESET_COUNT; k++) pl[k] = TR(g_dyn_preset_names[k]);
        if (igCombo_Str_arr(PL(TR("Chain Preset")), &pr, pl, DYN_PRESET_COUNT, -1)) phys_chain_preset(c, pr), ch = true;
        bool on = c->on != 0, col = c->collide != 0;
        if (igCheckbox(PL(TR("Chain Enabled")), &on)) c->on = on, ch = true;
        if (igCheckbox(PL(TR("Collide With the Body")), &col)) c->collide = col, ch = true;
        ch |= igSliderFloat(PL(TR("Chain Stiffness")), &c->stiffness, 0.0f, 1.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Chain Damping")), &c->damping, 0.0f, 1.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Chain Gravity")), &c->gravity, 0.0f, 3.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Wind Share")), &c->wind, 0.0f, 2.0f, "%.2f", 0);
        ch |= igSliderFloat(PL(TR("Character Inertia")), &c->inertia, 0.0f, 1.0f, "%.2f", 0);
        ch |= igDragFloat(PL(TR("Chain Radius (cm)")), &c->radius, 0.1f, 0.0f, 50.0f, "%.1f", 0);
        ch |= igSliderFloat(PL(TR("Angle Limit (deg)")), &c->limit, 0.0f, 180.0f, "%.0f", 0);
        if (igButton(TR("Delete Chain"), (ImVec2){ 0, 0 })) {
            memmove(c, c + 1, sizeof *c * (p->nchains - (uint32_t)E.chain - 1));
            p->nchains--, E.chain = -1, ch = true;
        }
    } else igTextDisabled("%s", TR("Select a body, a constraint or a dynamic chain (or a body in the preview)."));
    return ch;
}

void ikrig_editor_draw(Editor* ed) {
    (void)ed;
    if (!E.rig && !E.phys) return;
    const char* name = E.rig ? E.rig->name : E.phys->name;
    uint32_t skel = E.rig ? E.rig->skeleton : E.phys->skeleton;
    const SkelMesh* m = anim_mesh(skel);
    char title[200];
    snprintf(title, sizeof title, "%s%s - %s###asset_persona", name, E.unsaved ? "*" : "", E.rig ? TR("IK Rig") : TR("Physics Asset")); // (Q7: the animation editors' window)
    float fs = igGetFontSize();
    igSetNextWindowSize((ImVec2){ fs * 66, fs * 38 }, ImGuiCond_FirstUseEver);
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        bool save = igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
                    (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false));
        if (save) {
            bool ok = E.rig ? ikrig_save(E.rig) : phys_save(E.phys);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        if (E.phys) {
            igSameLine(0, 8);
            bool restart = igCheckbox(TR("Simulate"), &E.simulate);
            if (igIsItemHovered(0)) igSetTooltip("%s", TR("Drops the skeleton as a ragdoll in the preview (Ctrl + click in it: a push)."));
            igSameLine(0, 6);
            igSetNextItemWidth(fs_of() * 11.0f);
            const char* sm[3] = { TR(g_sim_modes[0]), TR(g_sim_modes[1]), TR(g_sim_modes[2]) };
            restart |= igCombo_Str_arr("##phys_sim_mode", &E.sim_mode, sm, 3, -1); // (Q10)
            if (restart) {
                ikphys_forget(PREVIEW_ENT);
                E.sway_t = 0.0f;
                if (E.simulate) phys_set_simulate(PREVIEW_ENT, E.sim_mode == 1 ? PHYS_ACTIVE : E.sim_mode == 2 ? PHYS_OFF : PHYS_RAGDOLL, NULL);
                if (E.simulate && E.sim_mode == 2) dyn_set_enabled(PREVIEW_ENT, true); // (its state: the chains alone)
            }
            const IkPhysStats* st = ikphys_stats();
            igSameLine(0, 12);
            igTextDisabled(TR("%u bodies, %u constraints"), E.phys->nbodies, E.phys->nconstraints);
            (void)st;
        }
        if (!m) igSameLine(0, 12), igTextColored(THV(TK_WARN), "%s", TR("Its Skeletal Mesh is missing."));
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        if (m) { // (Q7) the mesh's tabs at the top right (Unreal's): the switch after the window
            int t = avp_tabs_bar(anim_persona_tabs(m), E.rig ? AVP_TAB_IKRIG : AVP_TAB_PHYSICS);
            if (t >= 0) E.pending_tab = t;
        }
        float h = igGetContentRegionAvail().y;
        igBeginChild_Str("lists", (ImVec2){ fs * 16, h }, ImGuiChildFlags_Borders, 0);
        ch |= E.rig ? rig_lists(E.rig, m) : phys_lists(E.phys, m);
        if (m) {
            igSeparatorText(TR("Skeleton"));
            for (uint32_t b = 0; b < m->nbones; b++) {
                int depth = 0;
                for (int p = m->bones[b].parent; p >= 0 && depth < 24; p = m->bones[p].parent) depth++;
                igPushID_Int(5000 + (int)b);
                igIndent((float)depth * 6.0f + 1.0f);
                if (igSelectable_Bool(m->bones[b].name, E.bone == (int)b, 0, (ImVec2){ 0, 0 })) E.bone = (int)b, E.goal = E.solver = E.body = E.constraint = E.chain = -1;
                igUnindent((float)depth * 6.0f + 1.0f);
                igPopID();
            }
        }
        igEndChild();
        igSameLine(0, 6);
        igBeginChild_Str("preview", (ImVec2){ igGetContentRegionAvail().x - fs * 18, h }, ImGuiChildFlags_Borders, 0);
        if (m) preview_ui((SkelMesh*)m);
        igEndChild();
        igSameLine(0, 6);
        igBeginChild_Str("props", (ImVec2){ 0, h }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-fs * 7);
        ch |= E.rig ? rig_props(E.rig, m) : phys_props(E.phys, m);
        igPopItemWidth();
        igEndChild();
    }
    igEnd();
    if (ch) {
        E.unsaved = true;
        if (E.rig) E.rig->version++, E.rig->dirty = true;
        else E.phys->version++, E.phys->dirty = true;
    }
    if (!open) close_all();
    if (E.pending_tab >= 0) { // (Q7) a tab: the mesh's Model, Skeleton or Animation (the animation editors), or the other asset
        int tab = E.pending_tab;
        E.pending_tab = -1;
        if (m) anim_persona_switch((SkelMesh*)m, tab);
    }
}

// --- The Skeletal Mesh's Details (anim_editor.c's anim_component_details) ---
bool ikrig_component_details(CSkeletalMesh* c) {
    bool ch = false;
    const SkelMesh* m = anim_mesh(c->mesh);
    IkRig* rig = c->ik_rig ? ikrig_find(c->ik_rig) : NULL;
    if (igBeginCombo(PL(TR("IK Rig")), rig ? rig->name : TR("(none)"), 0)) {
        if (igSelectable_Bool(TR("(none)"), !rig, 0, (ImVec2){ 0, 0 })) c->ik_rig = 0, ch = true;
        for (uint32_t i = 0; i < ikrig_count(); i++) {
            IkRig* r = ikrig_at(i);
            if (r && (!m || !r->skeleton || r->skeleton == m->hash) && asset_selectable(r->name, r == rig, r->path, ASSET_IK_RIG)) c->ik_rig = r->hash, ch = true;
        }
        igEndCombo();
    }
    PhysicsAsset* pa = c->physics_asset ? phys_find(c->physics_asset) : NULL;
    if (igBeginCombo(PL(TR("Physics Asset")), pa ? pa->name : TR("(none)"), 0)) {
        if (igSelectable_Bool(TR("(none)"), !pa, 0, (ImVec2){ 0, 0 })) c->physics_asset = 0, ch = true;
        for (uint32_t i = 0; i < phys_count(); i++) {
            PhysicsAsset* p = phys_at(i);
            if (p && (!m || !p->skeleton || p->skeleton == m->hash) && asset_selectable(p->name, p == pa, p->path, ASSET_PHYSICS_ASSET))
                c->physics_asset = p->hash, ch = true;
        }
        igEndCombo();
    }
    if (pa) {
        const char* modes[4] = { TR("No (animation)"), TR("Ragdoll (every body)"), TR("Bodies below a bone"), TR("Active Ragdoll (follows the animation)") };
        int mode = (int)(c->simulate % 4u);
        if (igCombo_Str_arr(PL(TR("Simulate Physics")), &mode, modes, 4, -1)) c->simulate = (uint32_t)mode, ch = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("How it starts when the game plays; Blueprint nodes change it (Set Simulate Physics, Set All Bodies Below Simulate Physics)."));
        if (c->simulate == 2) {
            char bone[ANIM_NAME];
            snprintf(bone, sizeof bone, "%s", c->simulate_bone);
            if (bone_combo(TR("Below Bone"), bone, m)) snprintf(c->simulate_bone, sizeof c->simulate_bone, "%s", bone), ch = true;
        }
        ch |= igSliderFloat(PL(TR("Physics Blend Weight")), &c->physics_blend, 0.0f, 1.0f, "%.2f", 0);
    }
    if ((rig || pa) && igButton(rig ? TR("Open IK Rig") : TR("Open Physics Asset"), (ImVec2){ 0, 0 })) {
        char vp[700];
        ikphys_vpath(rig ? rig->path : pa->path, vp, sizeof vp);
        ikrig_editor_open(vp);
    }
    return ch;
}

// --- The self-test: the view's projection and its inverse agree, a drag moves a goal in the view's plane (the asset
// viewport's, Q7) ---
int ikrig_editor_selftest(void) {
    int fails = 0;
    for (int t = 0; t < 4; t++) {
        AssetViewport v;
        avp_init(&v);
        v.cam.yaw = (float)t * 37.0f, v.cam.pitch = (float)t * 15.0f - 20.0f, v.cam.dist = 300.0f, v.cam.framed = true;
        avp_view_at(&v, 0.0f, 0.0f, 800.0f, 600.0f);
        float p[3] = { 10, -20, 100 }, d[3], q[3], a[2], b[2];
        avp_screen_delta(&v, p, 30.0f, -14.0f, d);
        for (int k = 0; k < 3; k++) q[k] = p[k] + d[k];
        avp_to_screen(&v, p, a), avp_to_screen(&v, q, b);
        if (fabsf(b[0] - a[0] - 30.0f) > 0.05f || fabsf(b[1] - a[1] + 14.0f) > 0.05f) {
            printf("[RealEngine] FAIL: ikrig editor: a drag at yaw %.0f pitch %.0f moved (%.2f, %.2f)\n", v.cam.yaw, v.cam.pitch, b[0] - a[0], b[1] - a[1]);
            fails++;
        }
    }
    printf("[RealEngine] ikrig editor self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

// (2026-11-25, bug-fixer) Its documents for editor_docs.h: a project switch and the editor's close end them before
// the systems free what they point at
int ikrig_editor_docs(int op, struct Editor* ed, char* names, size_t cap) {
    (void)ed;
    if (!E.rig && !E.phys) return 0;
    if (op == DOCS_OPEN) return 1;
    if (op == DOCS_CLOSE) return close_all(), E.unsaved = false, 1;
    if (!E.unsaved) return 0;
    if (op == DOCS_UNSAVED) return editor_docs_add_name(names, cap, E.rig ? E.rig->name : E.phys->name), 1;
    bool ok = E.rig ? ikrig_save(E.rig) : phys_save(E.phys);
    if (ok) E.unsaved = false;
    return !ok;
}
