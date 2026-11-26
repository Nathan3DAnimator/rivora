// (2026-11-23) Emissive materials light the scene (lights.c lights_emissive_light, notas-dev/luz.md "Material emissivo
// ilumina"): --scene emit: a closed room of 8 x 8 x 3 m with no light at all and a sphere of 40 cm whose material glows
// orange (Emission Strength 20: 2000 nits); --scene emit_off: the same with Emissive Light 0. The shot (--shot) is read
// back: the floor around the sphere has to be lit (before: black, a material's glow lit nothing)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "emissive_test.h"
#include "lights.h"
#include "material.h"
#include "platform.h"
#include "content.h"
#include "platform_files.h"
#include "render.h"
#include "blueprint.h"
#include "bp_internal.h"
#include "mat_editor.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

static int g_scene; // 1 emit, 2 emit_off

static int sock_in(int type, const char* name) {
    const MatNodeDef* d = mat_node_def(type);
    for (int i = 0; d && i < d->nin; i++)
        if (!strcmp(d->in[i].name, name)) return i;
    return -1;
}

static Entity box(World* w, const char* name, int shape, float x, float y, float z, float sx, float sy, float sz, uint32_t mat) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_STATIC;
    t->scale[0] = sx, t->scale[1] = sy, t->scale[2] = sz;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape, m->material = mat, m->uv_scale = 1.0f, m->roughness = 0.6f, m->cast_shadow = 1;
    m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.7f;
    return e;
}

static bool ei_scene(Editor* ed);
bool emissive_test_scene(Editor* ed, const char* name) {
    if (!strcmp(name, "emit_intensity")) return ei_scene(ed);
    int sc = !strcmp(name, "emit") ? 1 : !strcmp(name, "emit_off") ? 2 : 0;
    if (!sc) return false;
    g_scene = sc;
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s\\EmitTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s\\M_EmitTest%s.rmat", dir, sc == 2 ? "_Off" : "");
    Material* m = mat_load(path);
    if (!m) m = mat_create(path, false, NULL);
    if (m && m->graph) {
        for (int i = 0; i < m->graph->node_count; i++) {
            MatNode* n = &m->graph->nodes[i];
            if (n->type != NODE_BSDF_PRINCIPLED) continue;
            int bc = sock_in(n->type, "Base Color"), ec = sock_in(n->type, "Emission Color"), es = sock_in(n->type, "Emission Strength");
            if (bc >= 0) n->in[bc][0] = n->in[bc][1] = n->in[bc][2] = 0.05f;
            if (ec >= 0) n->in[ec][0] = 1.0f, n->in[ec][1] = 0.55f, n->in[ec][2] = 0.2f;
            if (es >= 0) n->in[es][0] = 20.0f;
        }
        m->emissive_light = sc == 2 ? 0.0f : 1.0f;
        m->emissive_shadows = sc == 1; // (the shadowed path: by ray, the many lights' sampling)
        mat_save(m);
        mat_mark_dirty(m);
    }
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE);
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    ed->game_view = true; // (no icons or grid over the shot)
    // the room (cubes of 100 cm scaled): floor, ceiling, four walls; nothing lights it but the sphere
    box(w, "Floor", SHAPE_CUBE, 0, 0, -5, 8, 8, 0.1f, 0);
    box(w, "Ceiling", SHAPE_CUBE, 0, 0, 305, 8, 8, 0.1f, 0);
    box(w, "WallN", SHAPE_CUBE, 0, 405, 150, 8.2f, 0.1f, 3.2f, 0);
    box(w, "WallS", SHAPE_CUBE, 0, -405, 150, 8.2f, 0.1f, 3.2f, 0);
    box(w, "WallE", SHAPE_CUBE, 405, 0, 150, 0.1f, 8.2f, 3.2f, 0);
    box(w, "WallW", SHAPE_CUBE, -405, 0, 150, 0.1f, 8.2f, 3.2f, 0);
    Entity s = box(w, "GlowSphere", SHAPE_SPHERE, 0, 0, 60, 0.4f, 0.4f, 0.4f, m ? m->hash : 0);
    (void)s;
    ed->cam.pos[0] = -330.0f, ed->cam.pos[1] = -330.0f, ed->cam.pos[2] = 230.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -28.0f;
    printf("[RealEngine] emit scene %s: the sphere's material %s (Emissive Light %.0f)\n", name, m ? "ready" : "FAIL: not made",
           m ? m->emissive_light : 0.0f);
    return true;
}

// --- (2026-11-25, the tester's block of items 3/5: the Emission Intensity without proof) --scene emit_intensity: the
// material's Emission Intensity written by the editor (2.5) and read back from its file; the field drawn in the
// material's Details and in a Material Instance's of it (a number, not a color); a parameter's base color's thumbnail
// in an instance's editor (the affine fold: its texture holds roots, the slot's coefficients the color: before, black);
// a Blueprint's Set Scalar Parameter Value on Materials (Emission Intensity 4) in Play: the actor's glow measured before
// and after (its slot's emission mean as it draws and the scene's brightest point)
static Material *g_ei_m, *g_ei_mi, *g_ei_tint;
static Entity g_ei_actor;
static int g_ei_frame, g_ei_fails;
static float g_ei_before[3], g_ei_lum_before;
#define EI_FAIL(...) (printf("[RealEngine] FAIL: emit intensity: " __VA_ARGS__), printf("\n"), g_ei_fails++)

static int bp_pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    return -1;
}
static bool ei_link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = bp_pin(g, a, 1, ap), pb = bp_pin(g, b, 0, bpn);
    return pa >= 0 && pb >= 0 && bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb);
}

// BP_EmitGlow: BeginPlay -> Set Scalar Parameter Value on Materials (Emission Intensity) = 4 on itself
static BpAsset* ei_blueprint(void) {
    char disk[800];
    snprintf(disk, sizeof disk, "%s\\EmitTest\\BP_EmitGlow.rbp", content_root());
    remove(disk); // (a run before left it: made again)
    BpAsset* a = bp_find_name(BPA_CLASS, "BP_EmitGlow");
    if (!a) a = bp_create(BPA_CLASS, "/Content/EmitTest", "BP_EmitGlow", "Actor");
    if (!a || !a->ngraphs) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    BpNode* begin = bp_add_node(a, g, BPK_EVENT_BEGIN_PLAY, 0, 0, NULL, NULL);
    BpNode* self = begin ? bp_add_node(a, g, BPK_SELF, 0, 200, NULL, NULL) : NULL;
    BpNode* set = self ? bp_add_node(a, g, BPK_MAT_SCALAR, 320, 0, MAT_EMISSION_INTENSITY, NULL) : NULL;
    if (!set) return NULL;
    uint32_t bid = begin->id, sid = self->id, mid = set->id;
    bool ok = ei_link(g, bid, "", mid, "") && ei_link(g, sid, "Self", mid, "Target");
    int pv = bp_pin(g, mid, 0, "Parameter Value");
    BpNode* sn = bp_node(g, mid);
    if (pv < 0 || !sn) ok = false;
    else sn->pins[pv].lit = (BpValue){ { 4.0f, 0, 0, 0 } };
    if (!ok) EI_FAIL("the Blueprint's nodes didn't link");
    bp_save(a);
    return a;
}

static bool ei_scene(Editor* ed) {
    g_scene = 3, g_ei_frame = 0, g_ei_fails = 0, g_ei_actor = ENTITY_NONE;
    char dir[700], path[800], vp[400];
    snprintf(dir, sizeof dir, "%s\\EmitTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s\\M_EmitIntensity.rmat", dir);
    remove(path);
    Material* m = mat_create(path, false, NULL);
    if (m && m->graph)
        for (int i = 0; i < m->graph->node_count; i++) {
            MatNode* n = &m->graph->nodes[i];
            if (n->type != NODE_BSDF_PRINCIPLED) continue;
            int bc = sock_in(n->type, "Base Color"), ec = sock_in(n->type, "Emission Color"), es = sock_in(n->type, "Emission Strength");
            if (bc >= 0) n->in[bc][0] = n->in[bc][1] = n->in[bc][2] = 0.05f;
            if (ec >= 0) n->in[ec][0] = 1.0f, n->in[ec][1] = 0.55f, n->in[ec][2] = 0.2f;
            if (es >= 0) n->in[es][0] = 20.0f;
        }
    if (m) m->emissive_light = 0.0f, mat_save(m), mat_mark_dirty(m); // (its glow only: no light to measure around)
    // its instance (no values of its own: the parent's Emission Intensity row)
    snprintf(path, sizeof path, "%s\\MI_EmitIntensity.rmi", dir);
    remove(path);
    if (m) {
        mat_vpath(m, vp, sizeof vp);
        g_ei_mi = mat_create(path, true, vp);
        if (g_ei_mi) mat_save(g_ei_mi);
    }
    // (c) a parameter's color straight into the Base Color (M_Toon's), and an instance with its own color
    snprintf(path, sizeof path, "%s\\M_EmitTint.rmat", dir);
    remove(path);
    Material* t = mat_create(path, false, NULL);
    if (t && t->graph) {
        int bsdf = -1;
        for (int i = 0; i < t->graph->node_count; i++)
            if (t->graph->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
        int rgb = mat_graph_add(t->graph, NODE_RGB, -400, 0);
        if (bsdf >= 0 && rgb >= 0) {
            snprintf(t->graph->nodes[rgb].param, sizeof t->graph->nodes[rgb].param, "Color");
            t->graph->nodes[rgb].value[0] = t->graph->nodes[rgb].value[1] = t->graph->nodes[rgb].value[2] = 0.8f, t->graph->nodes[rgb].value[3] = 1.0f;
            mat_graph_link(t->graph, rgb, 0, bsdf, 0);
        }
        mat_save(t), mat_mark_dirty(t);
        mat_vpath(t, vp, sizeof vp);
        snprintf(path, sizeof path, "%s\\MI_EmitTint.rmi", dir);
        remove(path);
        g_ei_tint = mat_create(path, true, vp);
        if (g_ei_tint) {
            g_ei_tint->nparams = 1;
            snprintf(g_ei_tint->params[0].name, MAT_NAME, "Color"), g_ei_tint->params[0].kind = 1;
            memcpy(g_ei_tint->params[0].value, (const float[4]){ 0.2f, 0.45f, 0.95f, 1.0f }, 16);
            mat_save(g_ei_tint), mat_mark_dirty(g_ei_tint);
        }
    }
    g_ei_m = m;
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE);
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    ed->game_view = true;
    box(w, "Floor", SHAPE_CUBE, 0, 0, -5, 8, 8, 0.1f, 0);
    // the actor: BP_EmitGlow with the glowing sphere (the mesh on the level's actor; the class: its graph)
    BpAsset* cls = ei_blueprint();
    if (cls && m) {
        g_ei_actor = bp_spawn(w, cls, (float[3]){ 0, 0, 60 }, (float[3]){ 0, 0, 0 });
        if (g_ei_actor) {
            ecs_add(w, g_ei_actor, COMP_STATIC_MESH);
            CStaticMesh* sm = ecs_get(w, g_ei_actor, COMP_STATIC_MESH);
            CTransform* tr = ecs_get(w, g_ei_actor, COMP_TRANSFORM);
            if (sm) sm->mesh = SHAPE_SPHERE, sm->material = m->hash, sm->uv_scale = 1.0f, sm->roughness = 0.6f, sm->cast_shadow = 1;
            if (sm) sm->base_color[0] = sm->base_color[1] = sm->base_color[2] = 0.7f;
            if (tr) tr->scale[0] = tr->scale[1] = tr->scale[2] = 0.8f, tr->flags = MOBILITY_MOVABLE;
            ecs_touch(w, g_ei_actor);
        }
    }
    if (!g_ei_actor) EI_FAIL("BP_EmitGlow wasn't placed");
    ed->cam.pos[0] = -300.0f, ed->cam.pos[1] = -300.0f, ed->cam.pos[2] = 160.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -12.0f;
    printf("[RealEngine] emit intensity scene: %s\n", m && g_ei_mi && g_ei_tint ? "ready" : "FAIL: the materials weren't made");
    return true;
}

static void ei_copy(const char* from, const char* to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[4096];
    size_t n;
    while (b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static bool ei_recent(int frame) { return frame >= 0 && igGetFrameCount() - frame <= 3; }

// each frame: the stages by frame count
void emissive_test_tick(Editor* ed, double dt) {
    (void)dt;
    if (g_scene != 3 || !g_ei_m) return;
    int f = ++g_ei_frame;
    char vp[400];
    if (f == 10) { // (1) 2.5 written by the editor's Save and read back from the file
        mat_vpath(g_ei_m, vp, sizeof vp);
        mat_editor_open(vp);
        g_ei_m->emission_intensity = 2.5f;
        bool saved = mat_save(g_ei_m); // (the editor's Save button calls it on its material: the same one, mat_load's)
        char disk[800], copy[800];
        snprintf(disk, sizeof disk, "%s\\EmitTest\\M_EmitIntensity.rmat", content_root());
        snprintf(copy, sizeof copy, "%s\\EmitTest\\M_EmitIntensityCopy.rmat", content_root());
        ei_copy(disk, copy);
        Material* back = mat_load(copy); // (a path not loaded yet: parsed from the file)
        float got = back ? back->emission_intensity : -1.0f;
        printf("[RealEngine] emit intensity: saved %d, read back from the file %.3f (wrote 2.500)\n", saved, got);
        if (!saved || fabsf(got - 2.5f) > 1e-4f) EI_FAIL("Emission Intensity 2.5 written and read back as %.3f", got);
    } else if (f == 40) { // (2) the field in the material's Details
        int mf, mif, mis;
        mat_editor_test_ei(&mf, &mif, &mis);
        printf("[RealEngine] emit intensity: the material's Details drew the field: %s\n", ei_recent(mf) ? "yes" : "no");
        if (!ei_recent(mf)) EI_FAIL("the Emission Intensity field isn't in the emissive material's Details");
        mat_vpath(g_ei_mi, vp, sizeof vp);
        mat_editor_open(vp);
    } else if (f == 70) { // ... and in its Material Instance's, as a number
        int mf, mif, mis;
        mat_editor_test_ei(&mf, &mif, &mis);
        printf("[RealEngine] emit intensity: the instance's Details drew the parent's Emission Intensity: %s, as a number: %s\n", ei_recent(mif) ? "yes" : "no",
               mis ? "yes" : "no (a color)");
        if (!ei_recent(mif) || !mis) EI_FAIL("the Emission Intensity isn't a number in the Material Instance's Details");
        // (c) the tester's case: the template's MI_ToonBlue (M_Toon's Color parameter straight into the Base Color: its
        // shared texture holds the fold's roots; a Material Instance made here baked its own and never showed it)
        mat_editor_open("/Content/Materials/Examples/Toon/MI_ToonBlue.rmi");
    } else if (f == 85) { // its Color edited in its editor (as the panel does: the value, then marked): the fold writes the slot only
        char disk[800];
        snprintf(disk, sizeof disk, "%s\\Materials\\Examples\\Toon\\MI_ToonBlue.rmi", content_root());
        Material* mi = mat_load(disk);
        for (int i = 0; mi && i < mi->nparams; i++)
            if (!strcmp(mi->params[i].name, "Color")) memcpy(mi->params[i].value, (const float[4]){ 0.2f, 0.45f, 0.95f, 1.0f }, 16);
        if (mi) mat_mark_dirty(mi);
        else EI_FAIL("MI_ToonBlue isn't in the template's Content");
    } else if (f == 110) { // (c) the instance's base color thumbnail shows its color (0.2, 0.45, 0.95), not the fold's roots
        int how;
        float tint[3], solid[3];
        mat_editor_test_base(&how, tint, solid);
        const float* c = how == MAT_PREVIEW_SOLID ? solid : tint;
        float err = fabsf(c[0] - 0.2f) + fabsf(c[1] - 0.45f) + fabsf(c[2] - 0.95f);
        printf("[RealEngine] emit intensity: MI_ToonBlue's base color thumbnail: %s (%.3f, %.3f, %.3f), want (0.200, 0.450, 0.950)\n",
               how == MAT_PREVIEW_SOLID ? "a solid color" : how == MAT_PREVIEW_IMAGE ? "the image times" : "not drawn", c[0], c[1], c[2]);
        if (how < 0 || err > 0.03f) EI_FAIL("the instance's base color thumbnail isn't its color (black before: the fold's roots)");
        mat_editor_close();
        g_ei_m->emission_intensity = 1.0f; // (the glow as baked for the measure)
        mat_mark_dirty(g_ei_m);
    } else if (f == 150) { // (3) the glow before Play, then Play: BeginPlay sets 4
        uint32_t slot = mat_slot_of(g_ei_actor, g_ei_m);
        mat_slot_emission(slot, g_ei_before);
        double lmax, lmean;
        render_probe_lum(&lmax, &lmean);
        g_ei_lum_before = (float)lmax;
        editor_play(ed, re_time_seconds());
    } else if (f == 210) {
        float after[3] = { 0, 0, 0 };
        uint32_t slot = mat_slot_of(g_ei_actor, g_ei_m);
        mat_slot_emission(slot, after);
        double lmax, lmean;
        render_probe_lum(&lmax, &lmean);
        float b = g_ei_before[0] + g_ei_before[1] + g_ei_before[2], a = after[0] + after[1] + after[2];
        printf("[RealEngine] emit intensity: Play, Set Scalar Parameter Value on Materials (Emission Intensity 4): the actor's glow %.1f -> %.1f nits (x%.2f), "
               "the scene's brightest point %.1f -> %.1f\n", b * 100.0f / 3.0f, a * 100.0f / 3.0f, b > 0.0f ? a / b : 0.0f, g_ei_lum_before, (float)lmax);
        if (!(b > 0.0f) || fabsf(a / b - 4.0f) > 0.4f) EI_FAIL("the Blueprint's Emission Intensity 4 didn't make the glow 4 times (x%.2f)", b > 0.0f ? a / b : 0.0f);
        if (!((float)lmax > g_ei_lum_before * 2.0f)) EI_FAIL("the picture's glow didn't brighten (%.1f -> %.1f)", g_ei_lum_before, (float)lmax);
        editor_stop(ed, re_time_seconds());
    }
}

bool emissive_test_ei_done(void) { return g_scene != 3 || g_ei_frame > 220; }

// The shot's floor (the viewport's lower half, its mean luma) and the emissive lights sent
bool emissive_test_check(Editor* ed, const char* shot) {
    if (g_scene == 3) {
        if (g_ei_frame <= 210) EI_FAIL("the measure ended at frame %d, before every stage (210)", g_ei_frame);
        printf("[RealEngine] emit intensity: %s\n", g_ei_fails ? "FAILED" : "ok");
        return g_ei_fails == 0;
    }
    if (!g_scene) return true;
    uint32_t w = 0, h = 0;
    uint8_t* px = shot ? mat_load_image_rgba(shot, &w, &h) : NULL;
    if (!px) {
        printf("[RealEngine] FAIL: emit: no shot to read (--shot)\n");
        return false;
    }
    const RenderView* rv = &ed->render_view;
    uint32_t x0 = (uint32_t)rv->x, y0 = (uint32_t)rv->y, vw = (uint32_t)rv->w, vh = (uint32_t)rv->h;
    if (!vw || !vh || x0 + vw > w || y0 + vh > h) x0 = y0 = 0, vw = w, vh = h;
    double sum = 0.0;
    uint64_t n = 0;
    for (uint32_t y = y0 + vh * 6 / 10; y < y0 + vh * 95 / 100; y++)
        for (uint32_t x = x0 + vw / 10; x < x0 + vw * 9 / 10; x++) {
            const uint8_t* a = &px[((size_t)y * w + x) * 4];
            sum += 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2], n++;
        }
    free(px);
    double floor = n ? sum / (double)n : 0.0;
    uint32_t locals = lights_local_count();
    printf("[RealEngine] emit: the floor's mean luma %.1f (0-255), %u local light(s) (the sphere's: %s)\n", floor, locals,
           g_scene == 1 ? "1 expected" : "none expected");
    bool ok = true;
    if (g_scene == 1) {
        if (locals != 1) printf("[RealEngine] FAIL: emit: the glowing sphere didn't become a light\n"), ok = false;
        if (floor < 12.0) printf("[RealEngine] FAIL: emit: the floor around the glowing sphere is dark (%.1f)\n", floor), ok = false;
    } else if (locals != 0) printf("[RealEngine] FAIL: emit_off: Emissive Light 0 still made a light\n"), ok = false;
    printf("[RealEngine] emit: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
