// Create from the selection (Q6, 2026-11-17; notas-dev/content-browser.md): the Content Browser's right click on an asset
// offers what can be made from it, by its type (Unreal's Create submenu): a Skeletal Mesh an IK Rig, a Physics Asset, a
// Ragdoll, an Anim Blueprint; a Static Mesh a Blueprint, a physics actor, its LODs, a Cloth, a Vehicle; a Material its
// Material Instance; a Texture a Material or a Light Function; an Animation a Montage or a Blend Space. The Add menu's
// creations that need a base asset open the asset picker (asset_picker.h) instead of listing every asset by name.
#include "content.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "anim.h"
#include "anim_editor.h"
#include "asset_picker.h"
#include "blueprint.h"
#include "bp_editor.h"
#include "components.h"
#include "ecs.h"
#include "i18n.h"
#include "ikrig.h"
#include "ikrig_editor.h"
#include "mat_editor.h"
#include "material.h"
#include "mesh_build.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "ui_icons.h"

typedef struct MakeInfo {
    int from;          // the AssetKind it is made from
    int icon;          // UiIcon
    const char* label; // English (TR)
    const char* tip;   // its description (ui_tips_table.c has the same)
} MakeInfo;

static const MakeInfo g_make[CM_COUNT] = {
    [CM_IK_RIG] = { ASSET_SKELETAL_MESH, ICON_CHARACTER, "IK Rig", "An IK Rig for this Skeletal Mesh: its arms, legs and head found by their names, a Full Body solver and a Look At." },
    [CM_PHYSICS_ASSET] = { ASSET_SKELETAL_MESH, ICON_CAPSULE, "Physics Asset", "The bones' bodies and joints (a capsule per bone, limits by the kind of joint): what a ragdoll and the bones' physics use." },
    [CM_RAGDOLL] = { ASSET_SKELETAL_MESH, ICON_CAPSULE, "Ragdoll", "A Blueprint actor with this Skeletal Mesh falling as a ragdoll from the start (its Physics Asset made when it has none)." },
    [CM_ANIM_BLUEPRINT] = { ASSET_SKELETAL_MESH, ICON_BLUEPRINT, "Anim Blueprint", "An Animation Blueprint for this Skeletal Mesh's skeleton." },
    [CM_SKELETON] = { ASSET_SKELETAL_MESH, ICON_ANGLE, "Skeleton", "Opens the Skeletal Mesh's skeleton (its bones, sockets and bind pose) in its editor: the skeleton lives in the Skeletal Mesh." },
    [CM_BLUEPRINT] = { ASSET_STATIC_MESH, ICON_BLUEPRINT, "Blueprint Class", "A Blueprint actor with this mesh and its material: placed many times, scripted once." },
    [CM_PHYSICS_ACTOR] = { ASSET_STATIC_MESH, ICON_CUBE, "Physics Actor", "A Blueprint actor with this mesh that simulates physics (it falls, is pushed, collides)." },
    [CM_LODS] = { ASSET_STATIC_MESH, ICON_VM_LOD, "Generate LODs", "Makes the mesh's levels of detail (simplified copies drawn far away) when it has none." },
    [CM_CLOTH] = { ASSET_STATIC_MESH, ICON_GAMEPAD, "Cloth", "Places the mesh in the level as a simulated cloth (a flag, a curtain, a cape): its top band pinned." },
    [CM_VEHICLE] = { ASSET_STATIC_MESH, ICON_MOVEMENT, "Vehicle", "Places the mesh in the level as a car (its wheels found by their shape); its Vehicle asset goes to /Content/Vehicles." },
    [CM_SOFT_BODY] = { ASSET_STATIC_MESH, ICON_SPHERE, "Soft Body", "Places the mesh in the level as a soft body (a cushion, a mattress, a ball): it squashes and springs back, its bottom held." },
    [CM_MATERIAL_INSTANCE] = { ASSET_MATERIAL, ICON_IMAGE, "Material Instance", "A Material Instance of this material: its parameters changed without compiling it again." },
    [CM_MATERIAL] = { ASSET_TEXTURE_SOURCE, ICON_SPHERE, "Material", "A material with this texture as its Base Color (an Image Texture into the Principled BSDF)." },
    [CM_LIGHT_FUNCTION] = { ASSET_TEXTURE_SOURCE, ICON_BULB, "Light Function", "A Light Function material with this texture: put on a light, it shapes and colors its light (a window, leaves, a gobo)." },
    [CM_MONTAGE] = { ASSET_ANIMATION, ICON_GRAPH, "Anim Montage", "An Anim Montage of this animation (DefaultSlot): played by Play Montage over the Anim Blueprint." },
    [CM_BLEND_SPACE] = { ASSET_ANIMATION, ICON_SLIDERS, "Blend Space", "An Anim Blueprint whose state is a Blend Space 1D on Speed with this animation as its first sample: add the others in its editor." },
};

const char* content_make_label(int make) { return make >= 0 && make < CM_COUNT ? g_make[make].label : "?"; }
int content_make_from_kind(int make) { return make >= 0 && make < CM_COUNT ? g_make[make].from : -1; }

static ContentEditorHooks s_hooks;
void content_set_editor_hooks(const ContentEditorHooks* h) {
    if (h) s_hooks = *h;
}

// --- Paths ---

static void disk_of(const char* vpath, char* out, size_t cap) { snprintf(out, cap, "%s%s", content_root(), vpath + 8); }
static void folder_of(const char* vpath, char* out, size_t cap) {
    const char* sl = strrchr(vpath, '/');
    snprintf(out, cap, "%.*s", sl ? (int)(sl - vpath) : (int)strlen(vpath), vpath);
}
static void stem_of(const char* vpath, char* out, size_t cap) {
    const char* sl = strrchr(vpath, '/');
    snprintf(out, cap, "%s", sl ? sl + 1 : vpath);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}
static bool exists(const char* vpath) {
    char d[1100];
    disk_of(vpath, d, sizeof d);
    return re_file_stat(d, NULL, NULL, NULL);
}
// "<folder>/<prefix><stem><suffix>.<ext>" not taken yet: name (no extension) and vpath
static bool unique(const char* folder, const char* prefix, const char* stem, const char* suffix, const char* ext, char* name,
                   size_t ncap, char* vpath, size_t vcap) {
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(name, ncap, "%s%s%s%d", prefix, stem, suffix, i + 1);
        else snprintf(name, ncap, "%s%s%s", prefix, stem, suffix);
        snprintf(vpath, vcap, "%s/%s.%s", folder, name, ext);
        if (!exists(vpath)) return true;
    }
    return false;
}

static int sock_in(int type, const char* name) {
    const MatNodeDef* d = mat_node_def(type);
    for (int i = 0; d && i < d->nin; i++)
        if (!strcmp(d->in[i].name, name)) return i;
    return -1;
}
static int sock_out(int type, const char* name) {
    const MatNodeDef* d = mat_node_def(type);
    for (int i = 0; d && i < d->nout; i++)
        if (!strcmp(d->out[i].name, name)) return i;
    return -1;
}

static void bp_var_float(BpAsset* a, const char* name) {
    for (uint32_t i = 0; i < a->nvars; i++)
        if (!strcmp(a->vars[i].name, name)) return;
    if (a->nvars >= sizeof a->vars / sizeof a->vars[0]) return;
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = BPT_FLOAT;
}

// --- Making ---

// (#164) The Tlender's texture set "Real Engine (packed)" (its texexport preset "realengine"): <name>_BaseColor (RGB +
// opacity), <name>_NormalRoughnessMetallic (the tangent normal's X and Y in OpenGL, the roughness, the metallic) and
// <name>_Emissive. A Material made from the _BaseColor finds the other two next to it (same extension, else PNG, TGA,
// EXR, JPG or .rudim) and wires them: the normal's Z rebuilt (sqrt(1 - x^2 - y^2)) into a Normal Map.
static bool sibling(const char* src, const char* tag, char* out, size_t cap) {
    char folder[512], stem[256];
    folder_of(src, folder, sizeof folder), stem_of(src, stem, sizeof stem);
    size_t sl = strlen(stem), bl = strlen("_BaseColor");
    if (sl <= bl || _stricmp(stem + sl - bl, "_BaseColor")) return false;
    stem[sl - bl] = 0;
    const char* dot = strrchr(src, '.');
    const char* exts[] = { dot ? dot + 1 : "png", "png", "tga", "exr", "jpg", "rudim" };
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        snprintf(out, cap, "%s/%s%s.%s", folder, stem, tag, exts[i]);
        if (exists(out)) return true;
    }
    return false;
}
static int packed_node(MatGraph* g, int type, float x, float y, int prop0) {
    int k = mat_graph_add(g, type, x, y);
    if (k >= 0 && prop0 >= 0) g->nodes[k].prop[0] = prop0;
    return k;
}
static void packed_set(MatGraph* g, const char* src, int img, int bsdf, float bx, float by) {
    char nrm[600], emi[600];
    if (img >= 0 && sibling(src, "_NormalRoughnessMetallic", nrm, sizeof nrm)) {
        int t = packed_node(g, NODE_TEX_IMAGE, bx - 1300.0f, by - 360.0f, -1);
        int sep = packed_node(g, NODE_SEPARATE_COLOR, bx - 1050.0f, by - 360.0f, -1);
        if (t < 0 || sep < 0) return;
        snprintf(g->nodes[t].image, sizeof g->nodes[t].image, "%s", nrm);
        g->nodes[t].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
        int co = sock_out(NODE_TEX_IMAGE, "Color"), al = sock_out(NODE_TEX_IMAGE, "Alpha");
        mat_graph_link(g, t, co, sep, 0);
        mat_graph_link(g, sep, 2, bsdf, sock_in(NODE_BSDF_PRINCIPLED, "Roughness")); // B: roughness
        mat_graph_link(g, t, al, bsdf, sock_in(NODE_BSDF_PRINCIPLED, "Metallic"));   // A: metallic
        // the normal: x = 2R - 1, y = 2G - 1, z = sqrt(max(1 - x^2 - y^2, 0)), back to 0.5 + n / 2
        int mx = packed_node(g, NODE_MATH, bx - 820.0f, by - 300.0f, MATH_MULTIPLY_ADD);
        int my = packed_node(g, NODE_MATH, bx - 820.0f, by - 420.0f, MATH_MULTIPLY_ADD);
        int xx = packed_node(g, NODE_MATH, bx - 640.0f, by - 300.0f, MATH_MULTIPLY);
        int sq = packed_node(g, NODE_MATH, bx - 640.0f, by - 420.0f, MATH_MULTIPLY_ADD);
        int om = packed_node(g, NODE_MATH, bx - 480.0f, by - 420.0f, MATH_SUBTRACT);
        int mz = packed_node(g, NODE_MATH, bx - 480.0f, by - 520.0f, MATH_MAXIMUM);
        int sz = packed_node(g, NODE_MATH, bx - 320.0f, by - 520.0f, MATH_SQRT);
        int bz = packed_node(g, NODE_MATH, bx - 320.0f, by - 420.0f, MATH_MULTIPLY_ADD);
        int cc = packed_node(g, NODE_COMBINE_COLOR, bx - 180.0f, by - 360.0f, -1);
        int nm = packed_node(g, NODE_NORMAL_MAP, bx - 40.0f, by - 360.0f, -1);
        if (mx < 0 || my < 0 || xx < 0 || sq < 0 || om < 0 || mz < 0 || sz < 0 || bz < 0 || cc < 0 || nm < 0) return;
        g->nodes[mx].in[1][0] = g->nodes[my].in[1][0] = 2.0f, g->nodes[mx].in[2][0] = g->nodes[my].in[2][0] = -1.0f;
        g->nodes[om].in[0][0] = 1.0f, g->nodes[mz].in[1][0] = 0.0f, g->nodes[bz].in[1][0] = 0.5f, g->nodes[bz].in[2][0] = 0.5f;
        mat_graph_link(g, sep, 0, mx, 0), mat_graph_link(g, sep, 1, my, 0);
        mat_graph_link(g, mx, 0, xx, 0), mat_graph_link(g, mx, 0, xx, 1);
        mat_graph_link(g, my, 0, sq, 0), mat_graph_link(g, my, 0, sq, 1), mat_graph_link(g, xx, 0, sq, 2);
        mat_graph_link(g, sq, 0, om, 1), mat_graph_link(g, om, 0, mz, 0), mat_graph_link(g, mz, 0, sz, 0), mat_graph_link(g, sz, 0, bz, 0);
        mat_graph_link(g, sep, 0, cc, 0), mat_graph_link(g, sep, 1, cc, 1), mat_graph_link(g, bz, 0, cc, 2);
        mat_graph_link(g, cc, 0, nm, sock_in(NODE_NORMAL_MAP, "Color"));
        mat_graph_link(g, nm, 0, bsdf, sock_in(NODE_BSDF_PRINCIPLED, "Normal"));
    }
    if (sibling(src, "_Emissive", emi, sizeof emi)) {
        int t = packed_node(g, NODE_TEX_IMAGE, bx - 340.0f, by - 640.0f, -1);
        if (t < 0) return;
        snprintf(g->nodes[t].image, sizeof g->nodes[t].image, "%s", emi);
        mat_graph_link(g, t, sock_out(NODE_TEX_IMAGE, "Color"), bsdf, sock_in(NODE_BSDF_PRINCIPLED, "Emission Color"));
        int es = sock_in(NODE_BSDF_PRINCIPLED, "Emission Strength");
        if (es >= 0) g->nodes[bsdf].in[es][0] = 1.0f;
    }
}

static bool make_impl(const char* src, int make, const char* dest, char* out, size_t cap, char* err, size_t ecap) {
    char folder[600], stem[128], name[160], vp[800];
    folder_of(src, folder, sizeof folder);
    if (dest && dest[0]) snprintf(folder, sizeof folder, "%s", dest);
    stem_of(src, stem, sizeof stem);
    out[0] = 0;
    if (strncmp(folder, "/Content", 8)) return snprintf(err, ecap, "%s", TR("Only in the project's Content folder.")), false;
    switch (make) {
    case CM_IK_RIG:
    case CM_PHYSICS_ASSET: {
        SkelMesh* m = anim_mesh_load(src);
        if (!m) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        bool ik = make == CM_IK_RIG;
        if (!unique(folder, ik ? "IK_" : "PHYS_", m->name, "", ik ? "rikrig" : "rphys", name, sizeof name, vp, sizeof vp)) return false;
        uint32_t h = ik ? ikrig_create(folder, name, m->hash) : phys_create(folder, name, m->hash);
        if (!h) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_RAGDOLL: {
        SkelMesh* m = anim_mesh_load(src);
        if (!m) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        uint32_t ph = 0;
        for (uint32_t i = 0; i < phys_count() && !ph; i++)
            if (phys_at(i)->skeleton == m->hash) ph = phys_at(i)->hash;
        if (!ph) {
            char pn[160], pvp[800];
            if (unique(folder, "PHYS_", m->name, "", "rphys", pn, sizeof pn, pvp, sizeof pvp)) ph = phys_create(folder, pn, m->hash);
        }
        if (!ph) return snprintf(err, ecap, "%s", TR("Could not make its Physics Asset.")), false;
        if (!unique(folder, "BP_", m->name, "_Ragdoll", "rbp", name, sizeof name, vp, sizeof vp)) return false;
        BpAsset* a = bp_create(BPA_CLASS, folder, name, "Actor");
        if (!a) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        a->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH); // (its body is the Skeletal Mesh)
        int mi = bp_scs_add(a, SCS_SKELETAL_MESH, BP_SCS_ROOT, "Mesh");
        if (mi >= 0) {
            CSkeletalMesh* c = (CSkeletalMesh*)a->scs[mi].data;
            c->mesh = m->hash, c->physics_asset = ph, c->simulate = 1, c->physics_blend = 1.0f, c->play_rate = 1.0f;
        }
        bp_save(a);
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_ANIM_BLUEPRINT:
    case CM_BLEND_SPACE: {
        const char* skeleton = NULL;
        AnimSeq* s = NULL;
        SkelMesh* m = NULL;
        if (make == CM_ANIM_BLUEPRINT) {
            m = anim_mesh_load(src);
            if (!m) return snprintf(err, ecap, TR("Could not read %s."), src), false;
            skeleton = m->name;
        } else {
            s = anim_seq_load(src);
            if (!s) return snprintf(err, ecap, TR("Could not read %s."), src), false;
            skeleton = s->skeleton;
        }
        bool bs = make == CM_BLEND_SPACE;
        if (!unique(folder, bs ? "ABP_BS_" : "ABP_", bs ? s->name : m->name, "", "rbp", name, sizeof name, vp, sizeof vp)) return false;
        if (bp_find_name(BPA_CLASS, name)) return snprintf(err, ecap, TR("%s already exists."), name), false;
        BpAsset* a = bp_create(BPA_CLASS, folder, name, "Anim Instance");
        if (!a) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        AnimGraph* g = anim_graph_of(a, true);
        if (g) {
            snprintf(g->skeleton, ANIM_NAME, "%s", skeleton);
            if (bs) { // one state: a Blend Space 1D on Speed, this clip its first sample (at 0 cm/s)
                memset(g->states, 0, sizeof g->states);
                g->nstates = 1, g->ntrans = 0, g->entry = 0;
                AnimState* st = &g->states[0];
                snprintf(st->name, ANIM_NAME, "Blend Space");
                st->kind = ANIM_STATE_BLEND1D, st->loop = true, st->rate = 1.0f;
                snprintf(st->param, ANIM_NAME, "Speed");
                st->nsamples = 1;
                snprintf(st->samples[0], ANIM_NAME, "%s", s->name);
                st->sample_at[0] = 0.0f;
                st->axis[0] = 0.0f, st->axis[1] = 600.0f;
                g->nnodes = 0; // (its nodes made again from the state machine)
                anim_graph_ensure_nodes(g);
                bp_var_float(a, "Speed");
            }
        }
        bp_save(a);
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_SKELETON:
        if (!anim_mesh_load(src)) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        snprintf(out, cap, "%s", src); // (opened: the skeleton is the Skeletal Mesh's)
        return true;
    case CM_BLUEPRINT:
    case CM_PHYSICS_ACTOR: {
        uint32_t mesh = render_mesh_id_of(src);
        if (!mesh) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        bool phys = make == CM_PHYSICS_ACTOR;
        if (!unique(folder, "BP_", stem, phys ? "_Physics" : "", "rbp", name, sizeof name, vp, sizeof vp)) return false;
        BpAsset* a = bp_create(BPA_CLASS, folder, name, "Actor");
        if (!a) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        a->comp_mask |= COMP_BIT(COMP_STATIC_MESH);
        CStaticMesh* sm = (CStaticMesh*)a->comp_data[COMP_STATIC_MESH];
        sm->mesh = mesh;
        sm->material = content_mesh_material(src);
        sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f, sm->roughness = 0.5f, sm->cast_shadow = 1;
        if (phys) {
            a->comp_mask |= COMP_BIT(COMP_RIGID_BODY);
            ecs_component_defaults(COMP_RIGID_BODY, a->comp_data[COMP_RIGID_BODY]);
        }
        bp_save(a);
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_LODS: {
        char d[1100];
        disk_of(src, d, sizeof d);
        MeshBuilt mb = { 0 }, nb = { 0 };
        if (!rmesh_read(d, &mb, 1u << 26, 1u << 28)) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        bool ok = false;
        if (mb.lods.count > 1) snprintf(err, ecap, TR("It already has %u LODs."), mb.lods.count);
        else if (mb.col || mb.morph.count || (mb.flags & RMESH_SKINNED))
            snprintf(err, ecap, "%s", TR("Its LODs come from its import (it has vertex colors, morph targets or a skin)."));
        else if (!mesh_build_indexed(mb.v, mb.nv, mb.idx, mb.lods.count ? mb.lods.index_count[0] : mb.ni, !(mb.flags & RMESH_NO_ATLAS), true, &nb))
            snprintf(err, ecap, "%s", TR("Could not simplify it."));
        else if (!rmesh_write(d, &nb)) snprintf(err, ecap, TR("Could not write %s."), src);
        else {
            ok = true;
            snprintf(err, ecap, TR("%u LODs made."), nb.lods.count);
            if (s_hooks.meshes_changed) s_hooks.meshes_changed();
        }
        mesh_built_free(&mb);
        mesh_built_free(&nb);
        if (ok) snprintf(out, cap, "%s", src);
        return ok;
    }
    case CM_CLOTH:
    case CM_VEHICLE:
    case CM_SOFT_BODY:
        if (!s_hooks.actor_from) return snprintf(err, ecap, "%s", TR("Only in the level editor.")), false;
        if (!s_hooks.actor_from(src, make, err, ecap)) return false;
        snprintf(out, cap, "%s", src);
        return true;
    case CM_MATERIAL_INSTANCE: {
        if (!unique(folder, "MI_", stem, "", "rmi", name, sizeof name, vp, sizeof vp)) return false;
        char d[1100];
        disk_of(vp, d, sizeof d);
        Material* mi = mat_create(d, true, src);
        if (!mi) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_MATERIAL:
    case CM_LIGHT_FUNCTION: {
        bool lf = make == CM_LIGHT_FUNCTION;
        if (!unique(folder, lf ? "LF_" : "M_", stem, "", "rmat", name, sizeof name, vp, sizeof vp)) return false;
        char d[1100];
        disk_of(vp, d, sizeof d);
        Material* m = mat_create(d, false, NULL);
        if (!m || !m->graph) return snprintf(err, ecap, TR("Could not create %s."), vp), false;
        MatGraph* g = m->graph;
        int bsdf = -1;
        for (int i = 0; i < g->node_count; i++)
            if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
        float bx = bsdf >= 0 ? g->nodes[bsdf].x : 0.0f, by = bsdf >= 0 ? g->nodes[bsdf].y : 0.0f;
        int img = mat_graph_add(g, NODE_TEX_IMAGE, bx - 340.0f, by);
        if (img >= 0) {
            snprintf(g->nodes[img].image, sizeof g->nodes[img].image, "%s", src);
            int co = sock_out(NODE_TEX_IMAGE, "Color"), bc = sock_in(NODE_BSDF_PRINCIPLED, "Base Color");
            if (bsdf >= 0 && co >= 0 && bc >= 0) mat_graph_link(g, img, co, bsdf, bc);
        }
        if (!lf && bsdf >= 0) packed_set(g, src, img, bsdf, bx, by);
        if (lf) m->domain = MAT_DOMAIN_LIGHT_FUNCTION;
        mat_save(m);
        mat_mark_dirty(m);
        snprintf(out, cap, "%s", vp);
        return true;
    }
    case CM_MONTAGE: {
        AnimSeq* s = anim_seq_load(src);
        if (!s) return snprintf(err, ecap, TR("Could not read %s."), src), false;
        AnimMontage* mo = anim_montage_create(folder, s);
        if (!mo) return snprintf(err, ecap, "%s", TR("Could not create the montage.")), false;
        const char* sl = strrchr(mo->path, '/');
        const char* bs = strrchr(mo->path, '\\');
        if (bs && (!sl || bs > sl)) sl = bs;
        snprintf(out, cap, "%s/%s", folder, sl ? sl + 1 : mo->path);
        return true;
    }
    default: return false;
    }
}

bool content_make(const char* src_vpath, int make, const char* dest_folder, char* out, size_t cap, char* msg, size_t mcap) {
    char tmp_out[800], tmp_msg[256];
    if (!out) out = tmp_out, cap = sizeof tmp_out;
    if (!msg) msg = tmp_msg, mcap = sizeof tmp_msg;
    msg[0] = 0;
    if (!src_vpath || make < 0 || make >= CM_COUNT) return false;
    bool ok = make_impl(src_vpath, make, dest_folder, out, cap, msg, mcap);
    if (!ok && !msg[0]) snprintf(msg, mcap, TR("Could not make the %s."), TR(g_make[make].label));
    return ok;
}

// Made from a menu (the right click, a pick): the new asset named in the Content Browser first, then its editor
static void made_one(int make, const char* out, const char* msg) {
    content_mark_dirty();
    if (make == CM_LODS || make == CM_CLOTH || make == CM_VEHICLE || make == CM_SOFT_BODY) {
        if (msg && msg[0]) content_message("%s", msg);
        return;
    }
    if (make == CM_SKELETON) {
        anim_editor_open(out);
        return;
    }
    content_created(out);
}

static void do_make(const char* src, int make, const char* dest) {
    char out[800], msg[256];
    if (content_make(src, make, dest, out, sizeof out, msg, sizeof msg)) made_one(make, out, msg);
    else content_message("%s", msg);
}

int content_make_options(int kind, int* out, int cap) {
    int n = 0;
    for (int m = 0; m < CM_COUNT && n < cap; m++)
        if (g_make[m].from == kind) out[n++] = m;
    return n;
}

bool content_create_menu(const char* vpath, int kind, bool editable) {
    int opts[CM_COUNT];
    int n = content_make_options(kind, opts, CM_COUNT);
    if (!n) return false;
    igSeparatorText(TR("Create"));
    bool made = false;
    for (int i = 0; i < n; i++) {
        const MakeInfo* mi = &g_make[opts[i]];
        if (ui_menu_item((UiIcon)mi->icon, TR(mi->label), NULL, false, editable)) do_make(vpath, opts[i], NULL), made = true;
        igSetItemTooltip("%s", TR(mi->tip));
    }
    igSeparator();
    return made;
}

// --- The Add menu: a single item that opens the picker of its base asset ---

static void picked(const char* ref, uint32_t user, const char* ctx) {
    // ctx: "<make>|<folder>"
    if (!ref[0]) return;
    int make = atoi(ctx);
    const char* folder = strchr(ctx, '|');
    do_make(ref, make, folder ? folder + 1 : NULL);
    (void)user;
}

void content_pick_and_make(int make, const char* folder_vpath) {
    if (make < 0 || make >= CM_COUNT) return;
    static const char* const titles[ASSET_KIND_COUNT] = {
        [ASSET_SKELETAL_MESH] = "Pick Skeletal Mesh", [ASSET_STATIC_MESH] = "Pick Static Mesh", [ASSET_MATERIAL] = "Pick Material",
        [ASSET_TEXTURE_SOURCE] = "Pick Texture", [ASSET_ANIMATION] = "Pick Animation",
    };
    int from = g_make[make].from;
    char ctx[600], hint[200];
    snprintf(ctx, sizeof ctx, "%d|%s", make, folder_vpath ? folder_vpath : "");
    snprintf(hint, sizeof hint, TR("The %s is made from the asset you pick."), TR(g_make[make].label));
    AssetPickerSpec s = { .title = TR(titles[from] ? titles[from] : "Pick Asset"), .hint = hint, .kinds = AK(from), .on_pick = picked, .ctx = ctx };
    asset_picker_open(&s);
}
