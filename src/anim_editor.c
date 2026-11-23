// The animation editors (anim_editor.h): one window at a time for a Skeletal Mesh, an Animation Sequence or an Anim
// Montage (Unreal's asset editors), the Anim Graph tab of the Blueprint Editor. The previews are the asset viewport
// (asset_viewport.h, Q7: the mesh drawn by the GPU in any view mode, the bones, sockets, the light, the camera); the
// window's tabs (Model, Skeleton, Animation, Physics, IK Rig) switch what it edits without closing it: the Physics and
// IK Rig tabs hand the same window (###asset_persona) to the IK Rig / Physics Asset editor. Nothing runs while closed.
#include "anim_editor.h"
#include "asset_picker.h"
#include "theme.h"
#include "audio.h"
#include "audio_editor.h"
#include "ui_tips.h"
#include "graph_view.h"
#include "graph_layout.h"
#include "graph_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include "graph_style.h"

#include "anim.h"
#include "rig_detect.h" // (2026-11-23) the Rig Detected panel
#include "asset_viewport.h"
#include "ikrig.h"
#include "ikrig_editor.h"
#include "asset_viewer.h"
#include "blueprint.h"
#include "bp_preview.h"
#include "components.h"
#include "content.h"
#include "i18n.h"
#include "render.h"
#include "ui_icons.h"

enum { ED_NONE = 0, ED_MESH, ED_SEQ, ED_MONTAGE };

static struct {
    int kind;
    SkelMesh* mesh;
    AnimSeq* seq;
    AnimSeq* last_seq;       // (Q7) the Animation tab's (back to it from Model / Skeleton)
    AnimMontage* montage;
    AssetViewport vp;        // (Q7) the window's viewport (its settings and camera)
    bool skeleton_tab;       // (Q7) ED_MESH: the Skeleton tab (bones, sockets) rather than the Model's
    int bone, part;          // selected
    float time, rate;
    bool playing, show_bones, unsaved;
    int notify;              // selected notify
    char new_notify[ANIM_NAME];
    char status[160];
    // the Anim Graph tab's selection (per class: the last one shown)
    const void* graph_of;
    int state, trans;
    float pan[2];
    GraphView view; // the Anim Graph's canvas (graph_view.h)
    int drag_state;
} E = { .rate = 1.0f, .show_bones = true, .notify = -1, .state = -1, .trans = -1, .drag_state = -1 };

static void mm_editor_draw(void); // (anim_mm_editor.inc)
static void bs_draw(void);        // (anim_graph_ui.inc: the Blend Space editor)
static void tr_draw(void);        // (anim_graph_ui.inc: a transition's rule window)

// --- The preview: a mesh at a pose ---

// The mesh at a pose in the asset viewport (asset_viewport.h): its parts and skins drawn by the GPU, the bones and the
// sockets over them; tabs: the window's (0: none), edit: its sockets can be moved (the Skeleton tab)
static void preview_cam(AssetViewport* v, ImVec2 at, ImVec2 size, const SkelMesh* m, const AnimXf* pose, int sel_bone, int sel_part,
                        unsigned key, unsigned tabs, int tab, SkelMesh* edit, AvpResult* r) {
    static PreviewDraw draws[ANIM_MAX_PARTS + ANIM_MAX_SKINS];
    static float pal[ANIM_MAX_BONES][3][4], bones[ANIM_MAX_BONES][3][4];
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    anim_world(m, pose, id, bones);
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.draws = draws, sc.ndraws = avp_skel_draws(m, (const float(*)[3][4])bones, sel_bone, sel_part, draws, ANIM_MAX_PARTS + ANIM_MAX_SKINS, pal, NULL, NULL);
    sc.skel = m, sc.bones = (const float(*)[3][4])bones, sc.sel_bone = sel_bone, sc.edit_sockets = edit;
    sc.sel_body = sc.sel_constraint = -1, sc.tabs = tabs, sc.tab = tab;
    avp_draw(v, key, at.x, at.y, size.x, size.y, &sc, r);
}

// --- The Anim Blueprint's preview (Unreal's: its Viewport with the Anim Preview Editor, the Anim Graph's corner):
// its Skeletal Mesh posed by its graph every frame (anim_preview_pose), the class's variables at the values set here ---

static struct {
    uint32_t hash, nvars; // the class the values are for
    AnimPreviewVar vars[64];
    uint8_t type[64];
    AssetViewport vp[2];  // the Viewport's, the corner's (Q7: the asset viewport)
    bool paused, framed;
    int frame;            // (stepped once a frame: both previews of a class show the same pose)
    AnimXf pose[ANIM_MAX_BONES];
} AP;

static void abp_vars(const BpAsset* a) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < a->nvars; i++)
        if (a->vars[i].type == BPT_FLOAT || a->vars[i].type == BPT_BOOL || a->vars[i].type == BPT_INT) n++;
    if (AP.hash == a->hash && AP.nvars == n) return;
    AP.hash = a->hash, AP.nvars = 0, AP.frame = -1;
    for (uint32_t i = 0; i < a->nvars && AP.nvars < 64; i++) {
        const BpVar* v = &a->vars[i];
        if (v->type != BPT_FLOAT && v->type != BPT_BOOL && v->type != BPT_INT) continue;
        snprintf(AP.vars[AP.nvars].name, ANIM_NAME, "%s", v->name);
        AP.vars[AP.nvars].value = v->def.v[0], AP.type[AP.nvars] = v->type;
        AP.nvars++;
    }
}

// The values (Unreal's Anim Preview Editor): what the Event Graph would set while playing
static void abp_var_panel(void) {
    igSeparatorText(TR("Anim Preview Editor"));
    if (!AP.nvars) igTextDisabled("%s", TR("No Float, Integer or Boolean variables: add them in My Blueprint."));
    for (uint32_t i = 0; i < AP.nvars; i++) {
        igPushID_Int((int)i);
        AnimPreviewVar* v = &AP.vars[i];
        if (AP.type[i] == BPT_BOOL) {
            bool b = v->value != 0.0f;
            if (igCheckbox(v->name, &b)) v->value = b ? 1.0f : 0.0f;
        } else if (AP.type[i] == BPT_INT) {
            int k = (int)v->value;
            igSetNextItemWidth(-igGetFontSize() * 7.0f);
            if (igDragInt(v->name, &k, 0.2f, -100000, 100000, "%d", 0)) v->value = (float)k;
        } else {
            igSetNextItemWidth(-igGetFontSize() * 7.0f);
            igDragFloat(v->name, &v->value, 1.0f, -1e6f, 1e6f, "%.2f", 0);
        }
        igPopID();
    }
}

bool anim_bp_preview(BpAsset* a, float x, float y, float w, float h, bool corner) {
    AnimGraph* g = anim_graph_of(a, false);
    SkelMesh* m = g && g->skeleton[0] ? anim_mesh_name(g->skeleton) : NULL;
    ImDrawList* dl = igGetWindowDrawList();
    if (!m || w < 32.0f || h < 32.0f) {
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ x, y }, (ImVec2){ x + w, y + h }, TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_TOP), TH(TK_PREVIEW_BOTTOM), TH(TK_PREVIEW_BOTTOM));
        const char* msg = TR("Pick the Skeleton in the Anim Graph's settings to preview it here.");
        ImVec2 ts = igCalcTextSize(msg, NULL, false, w - 16.0f);
        igSetCursorScreenPos((ImVec2){ x + 8.0f, y + (h - ts.y) * 0.5f });
        igPushTextWrapPos(x + w - 8.0f);
        igTextDisabled("%s", msg);
        igPopTextWrapPos();
        igDummy((ImVec2){ 0, 0 });
        return false;
    }
    abp_vars(a);
    ImGuiIO* io = igGetIO_Nil();
    int fc = igGetFrameCount();
    if (AP.frame != fc) {
        anim_preview_pose(0, g, m, AP.frame < 0 ? -1.0f : AP.paused ? 0.0f : io->DeltaTime, AP.vars, AP.nvars, AP.pose);
        AP.frame = fc;
    }
    // the Viewport: the preview on the left, the variables on the right; the corner: the preview alone
    float pw = corner ? w : fmaxf(w - igGetFontSize() * 17.0f, w * 0.55f);
    preview_cam(&AP.vp[corner ? 1 : 0], (ImVec2){ x, y }, (ImVec2){ pw, h }, m, AP.pose, -1, -1, corner ? BP_PV_KEY_ANIM_GRAPH : BP_PV_KEY_ABP, 0, 0,
                NULL, NULL);
    // the state it is in, the pause and the restart (Unreal's preview controls)
    char st[96];
    snprintf(st, sizeof st, "%s", anim_preview_state(0));
    ImVec2 ts = igCalcTextSize(st, NULL, false, -1);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x + pw - ts.x - 10.0f, y + igGetFontSize() * 2.0f }, TH(TK_CYAN_L), st, NULL);
    ImVec2 back = igGetCursorScreenPos();
    igSetCursorScreenPos((ImVec2){ x + 8.0f, y + h - igGetFontSize() * 3.4f });
    char lb[64];
    snprintf(lb, sizeof lb, "%s##abp_pause", AP.paused ? TR("Play") : TR("Pause"));
    if (igSmallButton(lb)) AP.paused = !AP.paused;
    igSameLine(0, 4);
    snprintf(lb, sizeof lb, "%s##abp_restart", TR("Restart"));
    if (igSmallButton(lb)) AP.frame = -1;
    igSetItemTooltip("%s", TR("From the entry states again"));
    igSetCursorScreenPos(back);
    igDummy((ImVec2){ 0, 0 }); // (ImGui: a moved cursor needs an item after it)
    if (!corner) {
        igSetCursorScreenPos((ImVec2){ x + pw + 4.0f, y });
        igBeginChild_Str("abp_vars", (ImVec2){ w - pw - 4.0f, h }, ImGuiChildFlags_Borders, 0);
        abp_var_panel();
        igEndChild();
    }
    return true;
}

static void bind_pose(const SkelMesh* m, AnimXf* pose) {
    for (uint32_t b = 0; b < m->nbones; b++) pose[b] = m->bones[b].bind;
}

// --- Opening ---

static void open_reset(void) {
    E.vp.cam.framed = false, E.vp.socket = -1;
    E.skeleton_tab = false;
    E.bone = E.part = E.notify = -1;
    E.time = 0.0f, E.playing = true, E.unsaved = false, E.status[0] = 0;
    E.kind = ED_NONE;
}

// A loaded Skeletal Mesh (the Details' button: by the mesh itself, never by its path - a project opened from a path
// with backslashes or another case made "/Content/..." from it fail)
static void open_mesh(SkelMesh* m) {
    open_reset();
    if (m) E.mesh = m, E.seq = NULL, E.kind = ED_MESH;
    ikrig_editor_close(); // (one window: the Physics / IK Rig editor shares it)
}

void anim_editor_close(void) { E.kind = ED_NONE; }

// (Q7) the window on a mesh's tab (the IK Rig / Physics Asset editor's tabs back here): its camera kept
void anim_editor_open_mesh_tab(SkelMesh* m, int tab) {
    if (!m) return;
    bool same = E.mesh == m && E.kind != ED_NONE;
    ikrig_editor_close();
    if (!same) open_reset(), E.mesh = m, E.seq = NULL, E.last_seq = NULL;
    E.kind = ED_MESH, E.skeleton_tab = tab == AVP_TAB_SKELETON;
    if (tab == AVP_TAB_ANIMATION) {
        AnimSeq* q = E.last_seq;
        for (uint32_t i = 0; !q && i < anim_seq_count(); i++)
            if (!strcmp(anim_seq_at(i)->skeleton, m->name)) q = anim_seq_at(i);
        if (q) E.kind = ED_SEQ, E.seq = q, E.time = 0.0f, E.playing = true;
    }
    if (E.skeleton_tab && E.vp.init && E.vp.bones == AVP_BONES_NONE) E.vp.bones = AVP_BONES_ALL;
}

void anim_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    const char* dot = strrchr(vpath, '.');
    if (!dot) return;
    open_reset();
    ikrig_editor_close();
    if (!strcmp(dot, ".rskm") && (E.mesh = anim_mesh_load(vpath))) E.kind = ED_MESH, E.seq = NULL;
    else if (!strcmp(dot, ".ranim") && (E.seq = anim_seq_load(vpath))) E.kind = ED_SEQ, E.mesh = anim_mesh_name(E.seq->skeleton);
    else if (!strcmp(dot, ".rmontage") && (E.montage = anim_montage_load(vpath))) {
        E.kind = ED_MONTAGE;
        E.seq = anim_seq_name(E.montage->anim);
        E.mesh = E.seq ? anim_mesh_name(E.seq->skeleton) : NULL;
    }
}

bool anim_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8);
    if (!ui_begin_menu(ICON_CHARACTER, TR("Animation"), ok)) return false;
    // (Q6) one item each: the asset picker chooses the Skeletal Mesh or the animation it is made from
    if (ui_menu_item(ICON_BLUEPRINT, TR("Animation Blueprint..."), NULL, false, true)) content_pick_and_make(CM_ANIM_BLUEPRINT, folder_vpath);
    igSetItemTooltip("%s", TR("Pick a Skeletal Mesh: an Animation Blueprint for its skeleton."));
    if (ui_menu_item(ICON_UPDATE, TR("Anim Montage..."), NULL, false, true)) content_pick_and_make(CM_MONTAGE, folder_vpath);
    igSetItemTooltip("%s", TR("Pick an animation: a montage of it."));
    if (ui_menu_item(ICON_SLIDERS, TR("Blend Space..."), NULL, false, true)) content_pick_and_make(CM_BLEND_SPACE, folder_vpath);
    igSetItemTooltip("%s", TR("Pick an animation: an Anim Blueprint with a Blend Space 1D on Speed starting from it."));
    igEndMenu();
    return false; // (made when picked)
}

// --- The Skeletal Mesh Editor ---

static void skeleton_tree(SkelMesh* m) {
    for (uint32_t b = 0; b < m->nbones; b++) {
        int depth = 0;
        for (int p = m->bones[b].parent; p >= 0 && depth < 30; p = m->bones[p].parent) depth++;
        igPushID_Int((int)b);
        float x0 = igGetCursorPosX();
        igSetCursorPosX(x0 + (float)depth * igGetFontSize() * 0.8f);
        ui_icon_item(ICON_ANGLE, igGetFontSize(), TH(TK_N13));
        igSameLine(0, 4);
        if (igSelectable_Bool(m->bones[b].name, E.bone == (int)b, 0, (ImVec2){ 0, 0 })) E.bone = (int)b, E.part = -1;
        if (igBeginPopupContextItem("bone_ctx", ImGuiPopupFlags_MouseButtonRight)) { // (Q7) Unreal's Add Socket on a bone
            if (ui_menu_item(ICON_PLUS, TR("Add Socket"), NULL, false, m->nsockets < ANIM_MAX_SOCKETS)) {
                AnimSocket* k = anim_socket_add(m, (int)b);
                if (k) E.vp.socket = (int)(k - m->sockets), E.skeleton_tab = true, E.kind = ED_MESH, E.unsaved = true;
            }
            igEndPopup();
        }
        for (uint32_t k = 0; k < m->nsockets; k++) { // its sockets under it
            if (m->sockets[k].bone != (int)b) continue;
            igPushID_Int(20000 + (int)k);
            igSetCursorPosX(x0 + (float)(depth + 1) * igGetFontSize() * 0.8f);
            ui_icon_item(ICON_ACTOR, igGetFontSize(), TH(TK_MAGENTA_L));
            igSameLine(0, 4);
            if (igSelectable_Bool(m->sockets[k].name, E.vp.socket == (int)k, 0, (ImVec2){ 0, 0 }))
                E.vp.socket = (int)k, E.bone = (int)b, E.skeleton_tab = true, E.kind = ED_MESH;
            igPopID();
        }
        igPopID();
    }
}

// (2026-11-23, the user: "a rig detection: it comes ready, knowing what is IK, FK and the hierarchy") What the engine
// found in this rig (rig_detect.h): its system, its class, how many main roles, its controls, its chains; the roles'
// bones (a click selects the bone); Analyze Again after a change
static void rig_panel(SkelMesh* m) {
    const RigInfo* ri = rig_info(m);
    igSeparatorText(TR("Rig Detected"));
    if (!ri) {
        igTextDisabled("%s", TR("Not analyzed yet."));
        if (igButton(TR("Analyze Rig"), (ImVec2){ 0, 0 })) rig_detect(m);
        return;
    }
    igTextWrapped("%s: %s", TR("System"), TR(rig_system_label(ri->system)));
    igTextWrapped("%s: %s", TR("Class"), TR(rig_class_label(ri->rig_class)));
    igTextWrapped(TR("Main roles: %d of %d (%.0f%% sure)"), ri->main_mapped, ri->main_total, ri->confidence * 100.0f);
    if (ri->control_bones) igTextWrapped(TR("Deforming bones: %d, controls: %d"), ri->deform_bones, ri->control_bones);
    int ik = 0;
    for (int l = 0; l < 4; l++) ik += ri->ik_handle[l] >= 0;
    if (ik) igTextWrapped(TR("IK handles: %d (legs and arms)"), ik);
    for (uint32_t c = 0; c < ri->nchains; c++)
        igBulletText("%s: %s", TR(rig_chain_kind_label(ri->chains[c].kind)), m->bones[ri->chains[c].first].name);
    if (igTreeNode_Str(TR("Roles"))) {
        for (int r = 1; r < CR_COUNT; r++) {
            if (ri->role[r] < 0) continue;
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##role%d", charrole_code(r), r);
            if (igSelectable_Bool(lbl, E.bone == ri->role[r], 0, (ImVec2){ 0, 0 })) E.bone = ri->role[r];
            igSameLine(igGetFontSize() * 7.5f, -1);
            igTextDisabled("%s", m->bones[ri->role[r]].name);
        }
        igTreePop();
    }
    if (igButton(TR("Analyze Rig"), (ImVec2){ 0, 0 })) rig_detect(m);
}

static bool xf_edit(AnimXf* x) {
    bool ch = igDragFloat3(PL(TR("Location")), x->t, 0.2f, -1e4f, 1e4f, "%.1f", 0);
    float r[3];
    anim_quat_euler(x->q, r);
    if (igDragFloat3(PL(TR("Rotation")), r, 0.5f, -360.0f, 360.0f, "%.1f", 0)) anim_euler_quat(r, x->q), ch = true;
    return ch;
}

static void mesh_details(SkelMesh* m) {
    if (E.bone < 0 || E.bone >= (int)m->nbones) {
        igTextDisabled("%s", TR("Select a bone in the Skeleton Tree."));
        return;
    }
    AnimBone* b = &m->bones[E.bone];
    igSeparatorText(b->name);
    igTextDisabled(TR("Parent: %s"), b->parent >= 0 ? m->bones[b->parent].name : "-");
    igSeparatorText(TR("Bind Pose"));
    if (xf_edit(&b->bind)) E.unsaved = true;
    igSeparatorText(TR("Parts"));
    for (uint32_t i = 0; i < m->nparts; i++) {
        SkPart* p = &m->parts[i];
        if (p->bone != E.bone) continue;
        igPushID_Int((int)i);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "%s %u", render_mesh_name(render_mesh_slot(p->mesh)), i);
        if (igCollapsingHeader_TreeNodeFlags(lbl, E.part == (int)i ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            E.part = (int)i;
            if (igBeginCombo(PL(TR("Mesh")), render_mesh_name(render_mesh_slot(p->mesh)), 0)) {
                for (uint32_t s = 0; s < render_mesh_count(); s++)
                    if (asset_selectable_mesh(render_mesh_name(s), render_mesh_slot(p->mesh) == s, render_mesh_id(s)))
                        p->mesh = render_mesh_id(s), E.unsaved = true;
                igEndCombo();
            }
            if (xf_edit(&p->local)) E.unsaved = true;
            float sc[3] = { p->local.s[0] * 100.0f, p->local.s[1] * 100.0f, p->local.s[2] * 100.0f };
            if (igDragFloat3(PL(TR("Size (cm)")), sc, 0.2f, 0.1f, 1000.0f, "%.1f", 0)) {
                for (int k = 0; k < 3; k++) p->local.s[k] = sc[k] / 100.0f;
                E.unsaved = true;
            }
            if (igColorEdit3(PL(TR("Base Color")), p->color, 0)) E.unsaved = true;
            if (igSliderFloat(PL(TR("Roughness")), &p->roughness, 0, 1, "%.2f", 0)) E.unsaved = true;
            if (igSliderFloat(PL(TR("Metallic")), &p->metallic, 0, 1, "%.2f", 0)) E.unsaved = true;
            if (igButton(TR("Delete Part"), (ImVec2){ 0, 0 })) {
                memmove(&m->parts[i], &m->parts[i + 1], sizeof(SkPart) * (m->nparts - i - 1));
                m->nparts--;
                E.part = -1, E.unsaved = true;
                igPopID();
                break;
            }
        }
        igPopID();
    }
    if (m->nparts < ANIM_MAX_PARTS && igButton(TR("Add Part"), (ImVec2){ 0, 0 })) {
        SkPart* p = &m->parts[m->nparts++];
        memset(p, 0, sizeof *p);
        p->bone = (int16_t)E.bone, p->mesh = SHAPE_CUBE;
        p->local.q[3] = 1.0f, p->local.s[0] = p->local.s[1] = p->local.s[2] = 0.1f;
        p->color[0] = p->color[1] = p->color[2] = 0.6f, p->roughness = 0.5f;
        E.part = (int)m->nparts - 1, E.unsaved = true;
    }
}

// The animations of a skeleton (Unreal's Asset Browser in the animation editors): a click opens one
static void asset_browser(const char* skeleton) {
    igSeparatorText(TR("Asset Browser"));
    uint32_t n = anim_seq_count();
    for (uint32_t i = 0; i < n; i++) {
        AnimSeq* s = anim_seq_at(i);
        if (skeleton && strcmp(s->skeleton, skeleton)) continue;
        igPushID_Int((int)i);
        ui_icon_item(ICON_UPDATE, igGetFontSize(), TH(TK_AMBER_L));
        igSameLine(0, 4);
        char lbl[96];
        snprintf(lbl, sizeof lbl, "%s  (%.2f s)", s->name, s->length);
        if (igSelectable_Bool(lbl, E.seq == s && E.kind == ED_SEQ, 0, (ImVec2){ 0, 0 })) {
            E.kind = ED_SEQ, E.seq = s, E.time = 0.0f, E.notify = -1, E.playing = true;
        }
        igPopID();
    }
    for (uint32_t i = 0; i < anim_montage_count(); i++) {
        AnimMontage* mo = anim_montage_at(i);
        const AnimSeq* s = anim_seq_name(mo->anim);
        if (skeleton && (!s || strcmp(s->skeleton, skeleton))) continue;
        igPushID_Int(1000 + (int)i);
        ui_icon_item(ICON_GRAPH, igGetFontSize(), TH(TK_AMBER_L));
        igSameLine(0, 4);
        if (igSelectable_Bool(mo->name, E.montage == mo && E.kind == ED_MONTAGE, 0, (ImVec2){ 0, 0 }))
            E.kind = ED_MONTAGE, E.montage = mo, E.seq = (AnimSeq*)s, E.time = 0.0f, E.playing = true;
        igPopID();
    }
}

// The morph target curves of a sequence (Unreal's Curves: a morph target's weight over time): a row each with its plot
// (the keys as dots, right click deletes one), its value at the scrubber (a change keys it there); Add Curve: the
// preview mesh's morph targets or any name. Imported from a glTF's weights; they blend with the poses while playing
static void curves_panel(AnimSeq* s, float width) {
    if (!igCollapsingHeader_TreeNodeFlags(TR("Curves"), s->ncurves ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
    float len = s->length > 1e-3f ? s->length : 1.0f;
    ImGuiIO* io = igGetIO_Nil();
    ImDrawList* dl = igGetWindowDrawList();
    for (uint32_t c = 0; c < s->ncurves; c++) {
        igPushID_Int((int)c);
        const float* k = s->curve_keys + (size_t)s->curve_first[c] * 2;
        uint32_t n = s->curve_count[c];
        igAlignTextToFramePadding();
        igText("%s", s->curve_name[c]);
        igSameLine(igGetFontSize() * 9.0f, -1);
        float v = anim_curve_eval(s, c, E.time);
        igSetNextItemWidth(igGetFontSize() * 6.0f);
        bool keyed = igDragFloat("##curve_val", &v, 0.005f, -1.0f, 2.0f, "%.3f", 0);
        igSetItemTooltip("%s", TR("The weight at the scrubber's time: a change puts a key there"));
        igSameLine(0, 4);
        bool del = igSmallButton("x");
        igSetItemTooltip("%s", TR("Delete the curve"));
        // the plot: 0..1 high, the keys' line and dots, the scrubber
        igSameLine(0, 8);
        ImVec2 p = igGetCursorScreenPos();
        float pw = width - (p.x - igGetWindowPos().x) - 8.0f, ph = igGetFrameHeight();
        if (pw < 40.0f) pw = 40.0f;
        igInvisibleButton("plot", (ImVec2){ pw, ph }, ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
        ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + pw, p.y + ph }, TH(TK_N5), 2.0f, 0);
        int hit = -1;
        for (uint32_t i = 0; i < n; i++) {
            float x = p.x + pw * fminf(fmaxf(k[i * 2] / len, 0.0f), 1.0f), y = p.y + ph * (1.0f - fminf(fmaxf(k[i * 2 + 1], 0.0f), 1.0f));
            if (i) {
                float x0 = p.x + pw * fminf(fmaxf(k[(i - 1) * 2] / len, 0.0f), 1.0f), y0 = p.y + ph * (1.0f - fminf(fmaxf(k[(i - 1) * 2 + 1], 0.0f), 1.0f));
                ImDrawList_AddLine(dl, (ImVec2){ x0, y0 }, (ImVec2){ x, y }, TH(TK_AMBER), 1.5f);
            }
            if (n <= 64) ImDrawList_AddCircleFilled(dl, (ImVec2){ x, y }, 3.0f, TH(TK_ORANGE), 8);
            if (fabsf(io->MousePos.x - x) < 5 && fabsf(io->MousePos.y - y) < 6) hit = (int)i;
        }
        float sx = p.x + pw * fminf(E.time / len, 1.0f);
        ImDrawList_AddLine(dl, (ImVec2){ sx, p.y }, (ImVec2){ sx, p.y + ph }, TH(TK_RED), 1.5f);
        bool unkey = igIsItemClicked(ImGuiMouseButton_Right) && hit >= 0;
        if (keyed || unkey) { // the keys made again with the change
            float* kt = malloc(sizeof(float) * 2 * (n + 1));
            uint32_t m = 0;
            bool put = false;
            for (uint32_t i = 0; kt && i < n; i++) {
                if (unkey && (int)i == hit) continue;
                if (keyed && fabsf(k[i * 2] - E.time) < 1e-3f) kt[m] = k[i * 2], kt[n + 1 + m] = v, put = true, m++;
                else kt[m] = k[i * 2], kt[n + 1 + m] = k[i * 2 + 1], m++;
            }
            if (kt && keyed && !put) kt[m] = E.time, kt[n + 1 + m] = v, m++;
            if (kt) anim_curve_set(s, s->curve_name[c], kt, kt + n + 1, m);
            free(kt);
            s->dirty = E.unsaved = true;
        }
        igPopID();
        if (del) {
            anim_curve_set(s, s->curve_name[c], NULL, NULL, 0);
            s->dirty = E.unsaved = true;
            break;
        }
    }
    // a new curve: the preview mesh's morph targets, or a name typed
    static char nc[ANIM_MORPH_NAME];
    const SkelMesh* m = anim_mesh_name(s->skeleton);
    igSetNextItemWidth(igGetFontSize() * 10.0f);
    if (igBeginCombo("##addc", TR("Add Curve"), 0)) {
        for (uint32_t i = 0; m && i < m->nmorphs; i++)
            if (anim_curve_index(s, m->morph_name[i]) < 0 && igSelectable_Bool(m->morph_name[i], false, 0, (ImVec2){ 0, 0 })) {
                float t0 = 0.0f, v0 = anim_morph_weight(m, NULL, i);
                anim_curve_set(s, m->morph_name[i], &t0, &v0, 1);
                s->dirty = E.unsaved = true;
            }
        if (!m || !m->nmorphs) igTextDisabled("%s", TR("The skeleton's mesh has no morph targets: type a name"));
        igSetNextItemWidth(igGetFontSize() * 9.0f);
        bool enter = igInputTextWithHint("##ncn", TR("Morph target name"), nc, sizeof nc, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        if (enter && nc[0] && anim_curve_index(s, nc) < 0) {
            float t0 = 0.0f, v0 = 0.0f;
            anim_curve_set(s, nc, &t0, &v0, 1);
            nc[0] = 0, s->dirty = E.unsaved = true;
            igCloseCurrentPopup();
        }
        igEndCombo();
    }
    igSameLine(0, 10);
    igTextDisabled("%s", TR("A curve drives the morph target of that name while the sequence plays (over Set Morph Target)."));
}

// --- The timeline (sequences and montages): the scrubber, play / pause, the notifies' track ---

static void timeline(AnimSeq* s, float width) {
    float len = s->length > 1e-3f ? s->length : 1.0f;
    if (ui_icon_button("tl_play", E.playing ? ICON_UPDATE : ICON_FORWARD, E.playing ? TR("Pause") : TR("Play"), TH(TK_N14), E.playing, NULL))
        E.playing = !E.playing;
    igSameLine(0, 6);
    igSetNextItemWidth(igGetFontSize() * 6);
    igSliderFloat("##rate", &E.rate, 0.05f, 3.0f, "x%.2f", 0);
    igSameLine(0, 6);
    if (igCheckbox(PL(TR("Loop")), &s->loop)) s->dirty = E.unsaved = true;
    igSameLine(0, 12);
    igText("%.2f / %.2f s  (%u frames, %.0f fps)", E.time, len, s->nframes, s->fps);
    // the track: the time ruler with the notifies as diamonds (drag to move, right click to delete)
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFrameHeight() * 2.2f;
    igInvisibleButton("tl_track", (ImVec2){ width, h }, 0); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    bool active = igIsItemActive();
    ImGuiIO* io = igGetIO_Nil();
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + width, p.y + h }, TH(TK_N5), 3.0f, 0);
    for (int i = 0; i <= 10; i++) {
        float x = p.x + width * (float)i / 10.0f;
        ImDrawList_AddLine(dl, (ImVec2){ x, p.y }, (ImVec2){ x, p.y + h * 0.3f }, TH(TK_N10), 1.0f);
    }
    float px = p.x + width * fminf(E.time / len, 1.0f);
    int hit = -1;
    for (uint32_t i = 0; i < s->nnotifies; i++) {
        float x = p.x + width * s->notifies[i].time / len, y = p.y + h * 0.65f;
        bool sel = (int)i == E.notify;
        ImVec2 q[4] = { { x, y - 6 }, { x + 6, y }, { x, y + 6 }, { x - 6, y } };
        ImDrawList_AddConvexPolyFilled(dl, q, 4, sel ? TH(TK_ORANGE) : TH(TK_AMBER));
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 8, y - igGetFontSize() * 0.5f }, TH(TK_N14), s->notifies[i].name, NULL);
        if (fabsf(io->MousePos.x - x) < 7 && fabsf(io->MousePos.y - y) < 8) hit = (int)i;
    }
    if (igIsItemClicked(ImGuiMouseButton_Left)) E.notify = hit;
    if (active && E.notify >= 0 && E.notify < (int)s->nnotifies && hit < 0 && igIsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        s->notifies[E.notify].time = fminf(fmaxf((io->MousePos.x - p.x) / width * len, 0.0f), len);
        s->dirty = E.unsaved = true;
    } else if (active && E.notify < 0) { // scrubbing
        E.time = fminf(fmaxf((io->MousePos.x - p.x) / width * len, 0.0f), len);
        E.playing = false;
    }
    if (igIsItemClicked(ImGuiMouseButton_Right) && hit >= 0) {
        memmove(&s->notifies[hit], &s->notifies[hit + 1], sizeof(AnimNotify) * (s->nnotifies - (uint32_t)hit - 1));
        s->nnotifies--;
        E.notify = -1, s->dirty = E.unsaved = true;
    }
    ImDrawList_AddLine(dl, (ImVec2){ px, p.y }, (ImVec2){ px, p.y + h }, TH(TK_RED), 2.0f);
    // a new notify at the scrubber
    igSetNextItemWidth(igGetFontSize() * 10);
    igInputTextWithHint("##nn", TR("Notify name"), E.new_notify, sizeof E.new_notify, 0, NULL, NULL);
    igSameLine(0, 6);
    if (s->nnotifies >= 32) igBeginDisabled(true); // (Q23) full: said, not a click that does nothing
    bool add_notify = igButton(TR("Add Notify"), (ImVec2){ 0, 0 });
    if (s->nnotifies >= 32) {
        igEndDisabled();
        igSameLine(0, 6);
        igTextDisabled("%s", TR("(32 notifies: the most a sequence has)"));
    }
    if (add_notify && s->nnotifies < 32) {
        AnimNotify* n = &s->notifies[s->nnotifies++];
        memset(n, 0, sizeof *n);
        snprintf(n->name, sizeof n->name, "%s", E.new_notify[0] ? E.new_notify : "Notify");
        n->time = E.time, n->volume = n->pitch = 1.0f;
        s->dirty = E.unsaved = true;
    }
    igSameLine(0, 6);
    if (igButton(TR("Add Play Sound"), (ImVec2){ 0, 0 }) && s->nnotifies < 32) { // Unreal's Play Sound notify (its sound in Details)
        AnimNotify* n = &s->notifies[s->nnotifies++];
        memset(n, 0, sizeof *n);
        snprintf(n->name, sizeof n->name, "%s", "PlaySound");
        n->time = E.time, n->volume = n->pitch = 1.0f;
        E.notify = (int)s->nnotifies - 1;
        s->dirty = E.unsaved = true;
    }
    igSameLine(0, 12);
    igTextDisabled("%s", TR("A notify calls the Anim Blueprint's (and the actor's) custom event AnimNotify_<Name>. Right click deletes."));
    curves_panel(s, width); // (the morph target curves)
}

// --- (Q7) The window's tabs (Unreal's Skeleton / Mesh / Animation / Physics at the top right) ---

// The Physics Asset and the IK Rig made for a mesh (their "/Content/..." paths, "" none): the project's searched when
// the mesh changes (not every frame), or after one is made
static struct { const SkelMesh* m; char phys[512], rig[512]; } g_rel;
static void related(const SkelMesh* m) {
    if (g_rel.m == m) return;
    g_rel.m = m, g_rel.phys[0] = g_rel.rig[0] = 0;
    if (!m) return;
    static char found[128][512];
    int n = content_search(".rphys", found, 128);
    for (int i = 0; i < n && !g_rel.phys[0]; i++) {
        PhysicsAsset* p = phys_load(found[i]);
        if (p && p->skeleton == m->hash) snprintf(g_rel.phys, sizeof g_rel.phys, "%s", found[i]);
    }
    n = content_search(".rikrig", found, 128);
    for (int i = 0; i < n && !g_rel.rig[0]; i++) {
        IkRig* r = ikrig_load(found[i]);
        if (r && r->skeleton == m->hash) snprintf(g_rel.rig, sizeof g_rel.rig, "%s", found[i]);
    }
}

void anim_persona_refresh(void) { g_rel.m = NULL; }

unsigned anim_persona_tabs(const SkelMesh* m) {
    if (!m) return 0;
    related(m);
    unsigned t = AVP_TAB(AVP_TAB_MODEL) | AVP_TAB(AVP_TAB_SKELETON) | AVP_TAB(AVP_TAB_PHYSICS);
    for (uint32_t i = 0; i < anim_seq_count(); i++)
        if (!strcmp(anim_seq_at(i)->skeleton, m->name)) {
            t |= AVP_TAB(AVP_TAB_ANIMATION);
            break;
        }
    if (g_rel.rig[0]) t |= AVP_TAB(AVP_TAB_IKRIG);
    return t;
}

bool anim_persona_switch(SkelMesh* m, int tab) {
    if (!m) return false;
    related(m);
    if (tab == AVP_TAB_PHYSICS) {
        if (!g_rel.phys[0]) { // (none yet: one made from its bones beside the mesh, Unreal's Create Physics Asset)
            char vp[600], name[ANIM_NAME + 8];
            if (!anim_mesh_vpath(m, vp, sizeof vp)) return false;
            char* sl = strrchr(vp, '/');
            if (sl) *sl = 0;
            snprintf(name, sizeof name, "PHYS_%s", m->name);
            uint32_t h = phys_create(vp, name, m->hash);
            if (!h) return false;
            snprintf(g_rel.phys, sizeof g_rel.phys, "%s/%s.rphys", vp, name);
        }
        E.kind = ED_NONE;
        ikrig_editor_open(g_rel.phys);
        return true;
    }
    if (tab == AVP_TAB_IKRIG) {
        if (!g_rel.rig[0]) return false;
        E.kind = ED_NONE;
        ikrig_editor_open(g_rel.rig);
        return true;
    }
    anim_editor_open_mesh_tab(m, tab);
    return true;
}

static int current_tab(void) {
    return E.kind == ED_MESH ? (E.skeleton_tab ? AVP_TAB_SKELETON : AVP_TAB_MODEL) : AVP_TAB_ANIMATION;
}

// The Skeleton tab's Details: the bone (parent, bind pose) and the sockets
static void skeleton_details(SkelMesh* m) {
    if (E.bone >= 0 && E.bone < (int)m->nbones) {
        AnimBone* b = &m->bones[E.bone];
        igSeparatorText(b->name);
        igTextDisabled(TR("Parent: %s"), b->parent >= 0 ? m->bones[b->parent].name : "-");
        igSeparatorText(TR("Bind Pose"));
        if (xf_edit(&b->bind)) E.unsaved = true;
    } else
        igTextDisabled("%s", TR("Select a bone in the Skeleton Tree."));
    if (avp_socket_panel(&E.vp, m, E.bone)) E.unsaved = true;
}

// --- The window ---

void anim_editor_draw(struct Editor* ed, double now) {
    (void)ed, (void)now;
    mm_editor_draw(); // (its own window: the Motion Matching editor)
    bs_draw();        // (its own window: the Blend Space editor)
    tr_draw();        // (its own window: a transition's rule)
    if (E.kind == ED_NONE) return;
    const char* name = E.kind == ED_MESH ? E.mesh->name : E.kind == ED_SEQ ? E.seq->name : E.montage->name;
    const char* what = E.kind == ED_MESH ? (E.skeleton_tab ? TR("Skeleton") : TR("Skeletal Mesh")) : E.kind == ED_SEQ ? TR("Animation Sequence") : TR("Anim Montage");
    if (E.kind == ED_SEQ && E.seq) E.last_seq = E.seq;
    char title[200];
    snprintf(title, sizeof title, "%s%s - %s###asset_persona", name, E.unsaved ? "*" : "", what); // (Q7: the IK Rig / Physics editor's window too)
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 74, igGetFontSize() * 42 }, ImGuiCond_FirstUseEver);
    bool open = true;
    int pending_tab = -1;
    if (igBegin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        float dt = igGetIO_Nil()->DeltaTime;
        bool save = igButton(TR("Save"), (ImVec2){ 0, 0 }) ||
                    (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && igGetIO_Nil()->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false));
        if (save) {
            bool ok = E.kind == ED_MESH ? anim_mesh_save(E.mesh) : E.kind == ED_SEQ ? anim_seq_save(E.seq) : anim_montage_save(E.montage);
            if (E.kind == ED_MONTAGE && E.seq && E.seq->dirty) ok = anim_seq_save(E.seq) && ok;
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        if (E.kind == ED_SEQ) {
            igSameLine(0, 8);
            if (igButton(TR("Create Montage"), (ImVec2){ 0, 0 })) {
                char folder[600]; // (Q23) beside its sequence (it went to the Mannequin's folder whatever the clip's)
                size_t n = strlen(content_root());
                if (strlen(E.seq->path) > n && !_strnicmp(E.seq->path, content_root(), n)) {
                    snprintf(folder, sizeof folder, "/Content%s", E.seq->path + n);
                    for (char* c = folder; *c; c++)
                        if (*c == 92) *c = '/';
                    char* sl = strrchr(folder, '/');
                    if (sl && sl > folder + 8) *sl = 0;
                    else snprintf(folder, sizeof folder, "/Content");
                } else
                    snprintf(folder, sizeof folder, "/Content/Characters/Mannequin");
                AnimMontage* m = anim_montage_create(folder, E.seq);
                if (m) E.kind = ED_MONTAGE, E.montage = m;
            }
        }
        if (E.status[0]) {
            igSameLine(0, 10);
            igTextDisabled("%s", E.status);
        }
        if (E.mesh) pending_tab = avp_tabs_bar(anim_persona_tabs(E.mesh), current_tab()); // (Q7) Unreal's top right
        ImVec2 avail = igGetContentRegionAvail();
        static float side_w, right_w; // (the bars between the panels drag them; double-click: back)
        float side_def = igGetFontSize() * 17.0f, right_def = igGetFontSize() * 19.0f;
        if (side_w <= 0.0f) side_w = side_def;
        if (right_w <= 0.0f) right_w = right_def;
        side_w = fminf(side_w, fmaxf(avail.x - right_w - 240.0f, 120.0f));
        float side = side_w, right = right_w;
        float bottom = E.kind == ED_MESH ? 0.0f : igGetFrameHeightWithSpacing() * 4.2f;
        // left: the skeleton tree
        igBeginChild_Str("an_left", (ImVec2){ side, avail.y }, ImGuiChildFlags_Borders, 0);
        igSeparatorText(TR("Skeleton Tree"));
        if (E.mesh) skeleton_tree(E.mesh), rig_panel(E.mesh); // (2026-11-23) and what the rig was found to be
        else igTextDisabled("%s", TR("Its Skeletal Mesh is missing."));
        igEndChild();
        igSameLine(0, 0);
        ui_splitter("##an_lsplit", true, &side_w, 1.0f, 120.0f, avail.x - right - 240.0f, avail.y, side_def);
        igSameLine(0, 0);
        // middle: the viewport (and the timeline under it)
        float mid = avail.x - side - right - 2.0f * UI_SPLIT;
        igBeginChild_Str("an_mid", (ImVec2){ mid, avail.y }, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        AnimXf pose[ANIM_MAX_BONES];
        AnimSeq* s = E.kind == ED_MESH ? E.seq : E.seq;
        if (E.mesh) {
            if (s) {
                if (E.playing) {
                    float t_before = E.time;
                    E.time += dt * E.rate * (E.kind == ED_MONTAGE && E.montage->rate > 0.0f ? E.montage->rate : 1.0f);
                    for (uint32_t k = 0; k < s->nnotifies; k++) // the Play Sound notifies heard in the preview
                        if (s->notifies[k].sound && s->notifies[k].time > t_before && s->notifies[k].time <= E.time)
                            audio_play_2d(s->notifies[k].sound, s->notifies[k].volume, s->notifies[k].pitch);
                    if (E.time > s->length) E.time = s->loop || E.kind != ED_SEQ ? fmodf(E.time, fmaxf(s->length, 1e-3f)) : s->length;
                }
                anim_sample(s, E.mesh, E.time, pose);
            } else
                bind_pose(E.mesh, pose);
            ImVec2 at = igGetCursorScreenPos();
            AvpResult vr;
            int tab = current_tab();
            preview_cam(&E.vp, at, (ImVec2){ mid, avail.y - bottom - 4.0f }, E.mesh, pose, E.bone, E.part, BP_PV_KEY_ANIM, 0, tab,
                        tab == AVP_TAB_SKELETON ? E.mesh : NULL, &vr);
            if (vr.bone >= 0) E.bone = vr.bone, E.part = -1;
            if (vr.socket >= 0 && E.kind == ED_MESH) E.skeleton_tab = true;
            if (vr.socket_moved) E.unsaved = true;
        }
        if (bottom > 0.0f && s) {
            igSetCursorScreenPos((ImVec2){ igGetWindowPos().x, igGetWindowPos().y + avail.y - bottom });
            timeline(s, mid - 8.0f);
        }
        igEndChild();
        igSameLine(0, 0);
        ui_splitter("##an_rsplit", true, &right_w, -1.0f, 160.0f, avail.x - side - 240.0f, avail.y, right_def);
        igSameLine(0, 0);
        // right: Details and the Asset Browser
        igBeginChild_Str("an_right", (ImVec2){ right, avail.y }, ImGuiChildFlags_Borders, 0);
        igPushItemWidth(-igGetFontSize() * 6.0f);
        if (E.kind == ED_MESH && E.mesh && E.skeleton_tab) {
            skeleton_details(E.mesh);
            if (E.unsaved) E.mesh->dirty = true;
        } else if (E.kind == ED_MESH && E.mesh) {
            mesh_details(E.mesh);
            if (E.unsaved) E.mesh->dirty = true;
            if (E.mesh) {
                igSpacing();
                igSeparatorText(TR("Preview Animation"));
                if (igBeginCombo("##prev", E.seq ? E.seq->name : TR("Bind Pose"), 0)) {
                    if (igSelectable_Bool(TR("Bind Pose"), !E.seq, 0, (ImVec2){ 0, 0 })) E.seq = NULL;
                    for (uint32_t i = 0; i < anim_seq_count(); i++) {
                        AnimSeq* q = anim_seq_at(i);
                        if (!strcmp(q->skeleton, E.mesh->name) && asset_selectable(q->name, E.seq == q, q->path, ASSET_ANIMATION)) E.seq = q, E.time = 0;
                    }
                    igEndCombo();
                }
            }
        } else if (E.kind == ED_SEQ && E.seq) {
            igSeparatorText(TR("Animation"));
            igText(TR("Skeleton: %s"), E.seq->skeleton);
            igText(TR("%u tracks, %.2f s"), E.seq->ntracks, E.seq->length);
            if (E.notify >= 0 && E.notify < (int)E.seq->nnotifies) {
                igSeparatorText(TR("Notify"));
                AnimNotify* n = &E.seq->notifies[E.notify];
                if (igInputText(PL(TR("Name")), n->name, sizeof n->name, 0, NULL, NULL)) E.unsaved = E.seq->dirty = true;
                if (igDragFloat(PL(TR("Time")), &n->time, 0.01f, 0.0f, E.seq->length, "%.2f s", 0)) E.unsaved = E.seq->dirty = true;
                igSeparatorText(TR("Play Sound"));
                bool c = audio_pick(PL(TR("Sound")), &n->sound, -1);
                if (n->sound) {
                    if (n->volume <= 0.0f) n->volume = 1.0f;
                    if (n->pitch <= 0.0f) n->pitch = 1.0f;
                    c |= igDragFloat(PL(TR("Volume Multiplier")), &n->volume, 0.01f, 0.0f, 4.0f, "%.2f", 0);
                    c |= igDragFloat(PL(TR("Pitch Multiplier")), &n->pitch, 0.01f, 0.1f, 4.0f, "%.2f", 0);
                    if (E.mesh && igBeginCombo(PL(TR("Attach Name")), n->bone[0] ? n->bone : TR("(the mesh)"), 0)) {
                        if (igSelectable_Bool(TR("(the mesh)"), !n->bone[0], 0, (ImVec2){ 0, 0 })) n->bone[0] = 0, c = true;
                        for (uint32_t b = 0; b < E.mesh->nbones; b++)
                            if (igSelectable_Bool(E.mesh->bones[b].name, !strcmp(n->bone, E.mesh->bones[b].name), 0, (ImVec2){ 0, 0 }))
                                snprintf(n->bone, sizeof n->bone, "%s", E.mesh->bones[b].name), c = true;
                        igEndCombo();
                    }
                    bool f = n->follow != 0;
                    if (igCheckbox(PL(TR("Follow")), &f)) n->follow = f, c = true;
                }
                if (c) E.unsaved = E.seq->dirty = true;
            }
        } else if (E.kind == ED_MONTAGE && E.montage) {
            AnimMontage* m = E.montage;
            igSeparatorText(TR("Montage"));
            if (igBeginCombo(PL(TR("Animation")), m->anim[0] ? m->anim : "-", 0)) {
                for (uint32_t i = 0; i < anim_seq_count(); i++) {
                    AnimSeq* q = anim_seq_at(i);
                    if (asset_selectable(q->name, !strcmp(m->anim, q->name), q->path, ASSET_ANIMATION)) {
                        snprintf(m->anim, ANIM_NAME, "%s", q->name);
                        E.seq = q, E.mesh = anim_mesh_name(q->skeleton), E.unsaved = true;
                    }
                }
                igEndCombo();
            }
            static const char* const slots[2] = { "DefaultSlot", "UpperBody" };
            if (igBeginCombo(PL(TR("Slot")), m->slot, 0)) {
                for (int i = 0; i < 2; i++)
                    if (igSelectable_Bool(slots[i], !strcmp(m->slot, slots[i]), 0, (ImVec2){ 0, 0 })) snprintf(m->slot, ANIM_NAME, "%s", slots[i]), E.unsaved = true;
                igEndCombo();
            }
            igTextDisabled("%s", TR("DefaultSlot: the whole body. UpperBody: from the Anim Graph's upper bone up (the legs keep walking)."));
            E.unsaved |= igDragFloat(PL(TR("Blend In")), &m->blend_in, 0.01f, 0.0f, 5.0f, "%.2f s", 0);
            E.unsaved |= igDragFloat(PL(TR("Blend Out")), &m->blend_out, 0.01f, 0.0f, 5.0f, "%.2f s", 0);
            E.unsaved |= igDragFloat(PL(TR("Rate Scale")), &m->rate, 0.01f, 0.05f, 5.0f, "%.2f", 0);
            // (2026-11-23, anim_warp.inc) the root motion, the Motion Warping windows, the paired montage's partner
            E.unsaved |= igCheckbox(PL(TR("Root Motion")), &m->root_motion);
            if (m->root_motion) E.unsaved |= igCheckbox(PL(TR("Ignore Collision")), &m->no_collision);
            igSeparatorText(TR("Motion Warping"));
            for (uint32_t i = 0; i < m->nwarps; i++) {
                AnimWarpWindow* wn = &m->warps[i];
                igPushID_Int(300 + (int)i);
                igSetNextItemWidth(igGetFontSize() * 7);
                E.unsaved |= igInputText("##warp_target", wn->target, ANIM_NAME, 0, NULL, NULL);
                igSameLine(0, 4);
                igSetNextItemWidth(igGetFontSize() * 7);
                float r2[2] = { wn->start, wn->end };
                if (igDragFloat2("##warp_window", r2, 0.01f, 0.0f, 100.0f, "%.2f s", 0)) wn->start = fminf(r2[0], r2[1]), wn->end = fmaxf(r2[0], r2[1]), E.unsaved = true;
                bool wtr = wn->flags & ANIM_WARP_TRANSLATION, wro = wn->flags & ANIM_WARP_ROTATION, wiz = wn->flags & ANIM_WARP_IGNORE_Z;
                if (igCheckbox(TR("Place"), &wtr)) wn->flags ^= ANIM_WARP_TRANSLATION, E.unsaved = true;
                igSameLine(0, 8);
                if (igCheckbox(TR("Facing"), &wro)) wn->flags ^= ANIM_WARP_ROTATION, E.unsaved = true;
                igSameLine(0, 8);
                if (igCheckbox(TR("Ignore Height"), &wiz)) wn->flags ^= ANIM_WARP_IGNORE_Z, E.unsaved = true;
                igSameLine(0, 8);
                if (igSmallButton(TR("Remove"))) {
                    memmove(&m->warps[i], &m->warps[i + 1], sizeof m->warps[0] * (m->nwarps - i - 1));
                    m->nwarps--, E.unsaved = true;
                    igPopID();
                    break;
                }
                igPopID();
            }
            if (m->nwarps < ANIM_MAX_WARPS && igButton(TR("Add Warp Window"), (ImVec2){ 0, 0 })) {
                AnimWarpWindow* wn = &m->warps[m->nwarps++];
                memset(wn, 0, sizeof *wn);
                snprintf(wn->target, ANIM_NAME, "%s", "Target");
                wn->start = E.time, wn->end = E.seq ? E.seq->length : E.time + 0.5f;
                wn->flags = ANIM_WARP_TRANSLATION | ANIM_WARP_ROTATION | ANIM_WARP_IGNORE_Z;
                E.unsaved = true;
            }
            igSeparatorText(TR("Windows")); // (2026-11-23) State / Combo / Hit
            for (uint32_t i = 0; i < m->nwindows; i++) {
                AnimMontageWindow* wn = &m->windows[i];
                igPushID_Int(400 + (int)i);
                igSetNextItemWidth(igGetFontSize() * 6);
                E.unsaved |= igInputText("##win_name", wn->name, ANIM_NAME, 0, NULL, NULL);
                igSameLine(0, 4);
                static const char* const WK[ANIM_WIN_KINDS] = { "State", "Combo", "Hit" };
                int k = wn->kind < ANIM_WIN_KINDS ? wn->kind : 0;
                igSetNextItemWidth(igGetFontSize() * 4.5f);
                if (igCombo_Str_arr("##win_kind", &k, WK, ANIM_WIN_KINDS, -1)) wn->kind = (uint8_t)k, E.unsaved = true;
                igSameLine(0, 4);
                igSetNextItemWidth(igGetFontSize() * 7);
                float r2[2] = { wn->start, wn->end };
                if (igDragFloat2("##win_range", r2, 0.01f, 0.0f, 100.0f, "%.2f s", 0)) wn->start = fminf(r2[0], r2[1]), wn->end = fmaxf(r2[0], r2[1]), E.unsaved = true;
                igSameLine(0, 4);
                if (igSmallButton(TR("Remove"))) {
                    memmove(&m->windows[i], &m->windows[i + 1], sizeof m->windows[0] * (m->nwindows - i - 1));
                    m->nwindows--, E.unsaved = true;
                    igPopID();
                    break;
                }
                if (wn->kind == ANIM_WIN_COMBO) {
                    if (igBeginCombo(PL(TR("Next Section")), wn->next[0] ? wn->next : TR("(none)"), 0)) {
                        for (uint32_t s = 0; s < m->nsections; s++)
                            if (igSelectable_Bool(m->sections[s].name, !strcmp(wn->next, m->sections[s].name), 0, (ImVec2){ 0, 0 })) // tips: none (a section of this montage)
                                snprintf(wn->next, ANIM_NAME, "%s", m->sections[s].name), E.unsaved = true;
                        igEndCombo();
                    }
                } else if (wn->kind == ANIM_WIN_HIT) {
                    E.unsaved |= igInputText(PL(TR("Hit Bone")), wn->bone, ANIM_NAME, 0, NULL, NULL);
                    E.unsaved |= igDragFloat(PL(TR("Hit Radius")), &wn->radius, 0.5f, 1.0f, 200.0f, "%.0f cm", 0);
                }
                igPopID();
            }
            if (m->nwindows < ANIM_MAX_WINDOWS && igButton(TR("Add Window"), (ImVec2){ 0, 0 })) {
                AnimMontageWindow* wn = &m->windows[m->nwindows++];
                memset(wn, 0, sizeof *wn);
                snprintf(wn->name, ANIM_NAME, "%s", "Window"), snprintf(wn->bone, ANIM_NAME, "%s", "hand_r");
                wn->start = E.time, wn->end = E.time + 0.3f, wn->radius = 20.0f;
                E.unsaved = true;
            }
            igSeparatorText(TR("Paired Montage"));
            if (igBeginCombo(PL(TR("Partner Montage")), m->partner[0] ? m->partner : TR("(none)"), 0)) {
                if (igSelectable_Bool(TR("(none)"), !m->partner[0], 0, (ImVec2){ 0, 0 })) m->partner[0] = 0, E.unsaved = true;
                for (uint32_t i = 0; i < anim_montage_count(); i++) {
                    AnimMontage* q = anim_montage_at(i);
                    if (q && q != m && asset_selectable(q->name, !strcmp(m->partner, q->name), q->path, ASSET_ANIM_MONTAGE))
                        snprintf(m->partner, ANIM_NAME, "%s", q->name), E.unsaved = true;
                }
                igEndCombo();
            }
            if (m->partner[0]) {
                E.unsaved |= igDragFloat3(PL(TR("Partner Offset")), m->partner_offset, 1.0f, -10000.0f, 10000.0f, "%.0f cm", 0);
                E.unsaved |= igDragFloat(PL(TR("Partner Yaw")), &m->partner_yaw, 1.0f, -180.0f, 180.0f, "%.0f deg", 0);
                E.unsaved |= igDragFloat(PL(TR("Align Time")), &m->partner_align, 0.01f, 0.0f, 3.0f, "%.2f s", 0);
            }
            igSeparatorText(TR("Sections"));
            for (uint32_t i = 0; i < m->nsections; i++) {
                igPushID_Int((int)i);
                igSetNextItemWidth(igGetFontSize() * 7);
                E.unsaved |= igInputText("##sn", m->sections[i].name, ANIM_NAME, 0, NULL, NULL);
                igSameLine(0, 4);
                igSetNextItemWidth(igGetFontSize() * 4);
                E.unsaved |= igDragFloat("##ss", &m->sections[i].start, 0.01f, 0.0f, 100.0f, "%.2f", 0);
                if (m->nsections > 1) { // (Q23) a section removed (it could only be added; the first one stays)
                    igSameLine(0, 4);
                    if (igSmallButton(TR("Remove Section"))) {
                        memmove(&m->sections[i], &m->sections[i + 1], sizeof m->sections[0] * (m->nsections - i - 1));
                        m->nsections--, E.unsaved = true;
                        igPopID();
                        break;
                    }
                }
                igPopID();
            }
            if (m->nsections < 8 && igButton(TR("Add Section"), (ImVec2){ 0, 0 })) {
                snprintf(m->sections[m->nsections].name, ANIM_NAME, "Section%u", m->nsections);
                m->sections[m->nsections++].start = E.time;
                E.unsaved = true;
            }
            igTextDisabled("%s", TR("Played by the Blueprint node Play Montage (its name in the node's Details)."));
        }
        igPopItemWidth();
        igSpacing();
        asset_browser(E.mesh ? E.mesh->name : NULL);
        igEndChild();
    }
    igEnd();
    if (!open) E.kind = ED_NONE;
    if (pending_tab >= 0 && E.mesh && E.kind != ED_NONE && !anim_persona_switch(E.mesh, pending_tab)) // (Q7: after the window)
        snprintf(E.status, sizeof E.status, "%s", TR("Could not make its Physics Asset."));
}

static void mm_editor_open(BpAsset* a, int state, int node); // (the Motion Matching editor, below)
#include "anim_graph_ui.inc"
#include "anim_mm_editor.inc"

// --- A Skeletal Mesh component's properties (the level's Details, a Blueprint's component) ---

// (Q6) a Blueprint's "/Content/..." path from its file's
static bool bp_vp(const BpAsset* a, char* out, size_t cap) {
    size_t n = strlen(content_root());
    if (!a || strlen(a->path) <= n) return false;
    snprintf(out, cap, "/Content%s", a->path + n);
    for (char* c = out; *c; c++)
        if (*c == 92) *c = '/';
    return true;
}

bool anim_component_details(CSkeletalMesh* c) {
    bool ch = false;
    const SkelMesh* m = anim_mesh(c->mesh);
    { // (Q6) the asset picker (thumbnails, search, folders) instead of a list of every mesh
        char vp[700] = "", out[700];
        if (m) anim_mesh_vpath(m, vp, sizeof vp);
        if (asset_field(PL(TR("Skeletal Mesh")), m ? m->name : "-", vp, ASSET_SKELETAL_MESH, AK(ASSET_SKELETAL_MESH), false, out, sizeof out) ==
                ASSET_FIELD_PICKED && out[0]) {
            SkelMesh* q = anim_mesh_load(out);
            if (q) c->mesh = q->hash, ch = true, m = q;
        }
    }
    // Unreal's Animation Mode: an Animation Blueprint or one sequence
    const BpAsset* cls = c->anim_class ? bp_find_hash(c->anim_class) : NULL;
    { // (Q6) the asset picker on the project's Animation Blueprints
        char vp[700] = "", out[700];
        if (cls) bp_vp(cls, vp, sizeof vp);
        int r = asset_field(PL(TR("Anim Class")), cls ? cls->name : TR("None (Use Animation Asset)"), vp, ASSET_BLUEPRINT, 0, true, out, sizeof out);
        if (r == ASSET_FIELD_OPENED)
            for (uint32_t i = 0; i < bp_asset_count(); i++) {
                BpAsset* a = bp_asset_at(i);
                char av[700];
                if (a->kind == BPA_CLASS && bp_is_child_of(a, "Anim Instance") && bp_vp(a, av, sizeof av))
                    asset_picker_add(a->name, av, ASSET_BLUEPRINT, -1, 1, a->hash);
            }
        if (r == ASSET_FIELD_PICKED) {
            if (!out[0]) c->anim_class = 0, ch = true, cls = NULL;
            else {
                BpAsset* a = bp_load(out);
                if (a) c->anim_class = a->hash, ch = true, cls = a;
            }
        }
    }
    if (!cls) {
        const AnimSeq* s = c->anim ? anim_seq(c->anim) : NULL;
        if (igBeginCombo(PL(TR("Anim to Play")), s ? s->name : "-", 0)) {
            if (igSelectable_Bool("-", !s, 0, (ImVec2){ 0, 0 })) c->anim = 0, ch = true;
            for (uint32_t i = 0; i < anim_seq_count(); i++) {
                AnimSeq* q = anim_seq_at(i);
                if (m && strcmp(q->skeleton, m->name)) continue;
                if (asset_selectable(q->name, q == s, q->path, ASSET_ANIMATION)) c->anim = q->hash, ch = true;
            }
            igEndCombo();
        }
        ch |= igDragFloat(PL(TR("Play Rate")), &c->play_rate, 0.01f, 0.0f, 10.0f, "%.2f", 0);
        bool loop = c->loop != 0;
        if (igCheckbox(PL(TR("Looping")), &loop)) c->loop = loop, ch = true;
    }
    if (m && m->nmorphs) { // Unreal's Morph Targets: a slider per target (its name), the set ones the actor's own
        igSeparatorText(TR("Morph Targets"));
        for (uint32_t i = 0; i < m->nmorphs && i < ANIM_MAX_MORPHS; i++) {
            float v = anim_morph_weight(m, c, i);
            char id[ANIM_MORPH_NAME + 16];
            snprintf(id, sizeof id, "%s##morph%u", m->morph_name[i], i);
            if (igSliderFloat(PL(id), &v, 0.0f, 1.0f, "%.2f", 0)) {
                c->morph_weight[i] = v, c->morph_set[i >> 5] |= 1u << (i & 31u);
                ch = true;
            }
        }
        if ((c->morph_set[0] | c->morph_set[1]) && igButton(TR("Reset Morph Targets"), (ImVec2){ 0, 0 })) {
            c->morph_set[0] = c->morph_set[1] = 0;
            ch = true;
        }
    }
    ch |= ikrig_component_details(c); // (its IK Rig, Physics Asset and simulation: ikrig_editor.c)
    if (m && igButton(TR("Open Skeletal Mesh Editor"), (ImVec2){ 0, 0 })) open_mesh(anim_mesh(c->mesh));
    return ch;
}


void anim_mm_editor_open_asset(const char* vpath, bool query) {
    BpAsset* a = bp_load(vpath);
    AnimGraph* g = a ? anim_graph_of(a, false) : NULL;
    if (!g) return;
    for (uint32_t i = 0; i < g->nstates; i++)
        if (g->states[i].kind == ANIM_STATE_MOTION_MATCH) {
            mm_editor_open(a, (int)i, -1);
            M.mode = query ? 1 : 0;
            return;
        }
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == AN_MOTION_MATCH) {
            mm_editor_open(a, -1, (int)i);
            M.mode = query ? 1 : 0;
            return;
        }
}

void anim_blend_space_open_asset(const char* vpath) {
    BpAsset* a = bp_load(vpath);
    AnimGraph* g = a ? anim_graph_of(a, false) : NULL;
    if (!g) return;
    for (uint32_t i = 0; i < g->nstates; i++)
        if (g->states[i].kind == ANIM_STATE_BLEND1D || g->states[i].kind == ANIM_STATE_BLEND2D) {
            bs_open(a, -1, (int)i);
            return;
        }
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].kind == AN_BLEND_SPACE_1D || g->nodes[i].kind == AN_BLEND_SPACE_2D) {
            bs_open(a, (int)i, -1);
            return;
        }
}

void anim_transition_open_asset(const char* vpath) {
    BpAsset* a = bp_load(vpath);
    AnimGraph* g = a ? anim_graph_of(a, false) : NULL;
    if (g && g->ntrans) tw_open(a, 0);
}
