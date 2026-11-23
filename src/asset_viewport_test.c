// The asset viewport's test (--asset-viewport-test; Q7, notas-dev/editor.md "Viewport de asset"): every asset editor
// with a 3D view opened on an example asset (the project's, or one made in /Content/Q7Test and removed at the end), in
// every view mode its GPU picture read back and its model's pixels counted against the backdrop (the floor, the grid
// and the sky off: what is not the backdrop is the model; the bones are ImGui's, never in it). Then a socket: made on
// the Mannequin's hand with a preview mesh (its pixels appear), moved (its place follows), saved and read back. Then the
// cost: one editor open, then three. A contact sheet of every editor x mode: asset_viewport_test.png.
#include "asset_viewport.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "anim_editor.h"
#include "asset_viewer.h"
#include "bp_editor.h"
#include "cloth.h"
#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "ikrig.h"
#include "ikrig_editor.h"
#include "mat_editor.h"
#include "particle_editor.h"
#include "platform_files.h"
#include "render.h"
#include "vehicle.h"
#include "vehicle_editor.h"
#include "vkctx.h"

#define MANNEQUIN "/Content/Characters/Mannequin/SK_Mannequin.rskm"
#define ABP "/Content/Characters/Mannequin/ABP_Mannequin.rbp"
#define THUMB_W 128
#define THUMB_H 80

enum { C_STATIC, C_SKEL, C_SKELETON, C_ANIM, C_MONTAGE, C_BLEND, C_ABP, C_PHYS, C_IKRIG, C_MATERIAL, C_PARTICLES, C_VEHICLE, C_CLOTH, C_COUNT };
static const char* const g_case_names[C_COUNT] = { "Static Mesh", "Skeletal Mesh", "Skeleton", "Animation", "Montage", "Blend Space", "Anim Blueprint",
                                                   "Physics Asset", "IK Rig", "Material", "Particle System", "Vehicle", "Cloth" };
static const unsigned g_case_keys[C_COUNT] = { BP_PV_KEY_STATIC_MESH, BP_PV_KEY_ANIM, BP_PV_KEY_ANIM, BP_PV_KEY_ANIM, BP_PV_KEY_ANIM,
                                               BP_PV_KEY_BLEND_SPACE, BP_PV_KEY_ABP, BP_PV_KEY_IKRIG, BP_PV_KEY_IKRIG, BP_PV_KEY_MATERIAL,
                                               BP_PV_KEY_PARTICLES, BP_PV_KEY_VEHICLE, BP_PV_KEY_CLOTH };

enum { S_COST1, S_COST3, S_OPEN, S_SETTLE, S_MODE, S_READ, S_SOCKET, S_DONE };

static struct {
    bool running, ok;
    int state, cas, mode, wait, fails;
    uint32_t frames0;
    SkelMesh* mesh;
    Entity cloth;
    double cpu[2], gpu[2];
    int samples;
    uint8_t* sheet; // C_COUNT rows x PREVIEW_MODE_COUNT columns of thumbnails (RGB)
    double lit_frac[C_COUNT];
    char dir[600];  // the test's folder on disk (/Content/Q7Test)
} T;

void avp_test_start(void) {
    memset(&T, 0, sizeof T);
    T.running = true, T.ok = true;
    T.state = S_COST1; // (first: the cost of one editor, then three, before the others open windows of their own)
    anim_editor_open(MANNEQUIN);
    T.sheet = calloc((size_t)THUMB_W * PREVIEW_MODE_COUNT * THUMB_H * C_COUNT * 3, 1);
    snprintf(T.dir, sizeof T.dir, "%s/Q7Test", content_root());
    re_make_dirs(T.dir);
}
bool avp_test_running(void) { return T.running; }

#define FAIL(...) (printf("[RealEngine] FAIL: asset viewport: " __VA_ARGS__), printf("\n"), T.fails++)

static void close_editors(Editor* ed) {
    asset_viewer_close(), mat_editor_close(), particle_editor_close(), vehicle_editor_close();
    anim_editor_close(), ikrig_editor_close();
    ed->show_cloth = false;
}

// The share of the picture that is not the backdrop (the theme's gradient; black in Overdraw)
static double model_share(const uint8_t* px, uint32_t w, uint32_t h, int mode, int cas, int col) {
    uint32_t top = TH(TK_PREVIEW_TOP), bot = TH(TK_PREVIEW_BOTTOM);
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; y++) {
        float t = ((float)y + 0.5f) / (float)h;
        int bg[3];
        for (int c = 0; c < 3; c++) {
            float a = (float)((top >> (8 * c)) & 0xFF), b = (float)((bot >> (8 * c)) & 0xFF);
            bg[c] = mode == PREVIEW_OVERDRAW ? 0 : (int)(a + (b - a) * t + 0.5f);
        }
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = &px[((size_t)y * w + x) * 4];
            int d = 0;
            for (int c = 0; c < 3; c++) d = abs((int)p[c] - bg[c]) > d ? abs((int)p[c] - bg[c]) : d;
            if (d > 20) n++;
        }
    }
    static int dump = -1; // (REALENGINE_AVP_DUMP=1: every picture at full size, avp_<case>_<mode>.png)
    if (dump < 0) dump = getenv("REALENGINE_AVP_DUMP") != NULL;
    if (dump && col >= 0) {
        uint8_t* rgb = malloc((size_t)w * h * 3);
        if (rgb) {
            for (size_t i = 0; i < (size_t)w * h; i++) rgb[i * 3] = px[i * 4], rgb[i * 3 + 1] = px[i * 4 + 1], rgb[i * 3 + 2] = px[i * 4 + 2];
            char name[64];
            snprintf(name, sizeof name, "avp_%02d_%02d.png", cas, col);
            vkctx_png_write_rgb(name, rgb, w, h);
            free(rgb);
        }
    }
    // its thumbnail on the sheet (nearest)
    if (T.sheet && col >= 0) {
        size_t sw = (size_t)THUMB_W * PREVIEW_MODE_COUNT;
        for (int y = 0; y < THUMB_H; y++)
            for (int x = 0; x < THUMB_W; x++) {
                const uint8_t* p = &px[((size_t)(y * h / THUMB_H) * w + (size_t)(x * w / THUMB_W)) * 4];
                uint8_t* o = &T.sheet[(((size_t)cas * THUMB_H + y) * sw + (size_t)col * THUMB_W + x) * 3];
                o[0] = p[0], o[1] = p[1], o[2] = p[2];
            }
    }
    return (double)n / ((double)w * h);
}

// The case's editor on its asset (false: no such asset here)
static bool open_case(Editor* ed, int c) {
    static char found[64][512];
    switch (c) {
    case C_STATIC: {
        int n = content_search(".rmesh", found, 64);
        if (n <= 0) return false;
        // (a mesh with volume: a Landscape's chunk, made by an earlier test in a fresh build, is a flat patch seen edge on,
        // nothing left of it in Wireframe: the first build of a worktree failed here, Q25)
        int pick = 0;
        for (int i = 0; i < n; i++)
            if (!strstr(found[i], "Landscapes")) { pick = i; break; }
        asset_viewer_open(found[pick]);
        return true;
    }
    case C_SKEL:
        anim_editor_open(MANNEQUIN);
        T.mesh = anim_mesh_load(MANNEQUIN);
        return T.mesh != NULL;
    case C_SKELETON: return T.mesh && anim_persona_switch(T.mesh, AVP_TAB_SKELETON);
    case C_ANIM: return T.mesh && anim_persona_switch(T.mesh, AVP_TAB_ANIMATION);
    case C_MONTAGE: {
        int n = content_search(".rmontage", found, 64);
        if (n <= 0) return false;
        anim_editor_open(found[0]);
        return true;
    }
    case C_BLEND: anim_blend_space_open_asset(ABP); return true;
    case C_ABP: bp_editor_open_viewport(ABP); return true;
    case C_PHYS:
    case C_IKRIG: { // (made for the Mannequin in Q7Test when the project has none: the tab finds it)
        if (!T.mesh) return false;
        anim_editor_open(MANNEQUIN);
        if (!(anim_persona_tabs(T.mesh) & AVP_TAB(c == C_PHYS ? AVP_TAB_PHYSICS : AVP_TAB_IKRIG)) || c == C_PHYS) {
            char path[700];
            snprintf(path, sizeof path, "%s/%s", T.dir, c == C_PHYS ? "PHYS_Q7.rphys" : "IK_Q7.rikrig");
            uint64_t sz;
            int64_t mt;
            bool isdir;
            if (!re_file_stat(path, &sz, &mt, &isdir)) {
                if (c == C_PHYS) phys_create("/Content/Q7Test", "PHYS_Q7", T.mesh->hash);
                else ikrig_create("/Content/Q7Test", "IK_Q7", T.mesh->hash);
                anim_persona_refresh();
            }
        }
        return anim_persona_switch(T.mesh, c == C_PHYS ? AVP_TAB_PHYSICS : AVP_TAB_IKRIG);
    }
    case C_MATERIAL: {
        int n = content_search(".rmat", found, 64);
        if (n <= 0) return false;
        mat_editor_open(found[0]);
        return true;
    }
    case C_PARTICLES: {
        int n = content_search(".rparticle", found, 64);
        if (n <= 0) return false;
        const char* pick = found[0];
        for (int i = 0; i < n; i++)
            if (strstr(found[i], "Fountain")) pick = found[i];
        particle_editor_open(pick);
        return true;
    }
    case C_VEHICLE: {
        char vp[600];
        snprintf(vp, sizeof vp, "/Content/Q7Test/VH_Q7.rvehicle");
        veh_create("/Content/Q7Test", "VH_Q7", VT_CAR);
        vehicle_editor_open(vp);
        return true;
    }
    case C_CLOTH: {
        T.cloth = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CLOTH));
        if (T.cloth == ENTITY_NONE) return false;
        CTransform* t = ecs_get(&ed->world, T.cloth, COMP_TRANSFORM);
        t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_MOVABLE;
        t->location[2] = 300.0f;
        cloth_apply_preset(ecs_get(&ed->world, T.cloth, COMP_CLOTH), 0);
        snprintf(((CName*)ecs_get(&ed->world, T.cloth, COMP_NAME))->text, RE_NAME_MAX, "Q7TestCloth");
        editor_select(ed, T.cloth, SEL_REPLACE);
        ed->show_cloth = true;
        return true;
    }
    }
    return false;
}

// The socket: on the hand, a cube shown on it (its pixels add to the Skeletal Mesh's), moved up 10 cm (its place
// follows), saved into a copy and read back
static void socket_test(void) {
    SkelMesh* m = T.mesh;
    int hand = -1;
    for (uint32_t b = 0; b < m->nbones; b++)
        if (!_stricmp(m->bones[b].name, "hand_r")) hand = (int)b;
    if (hand < 0) hand = (int)m->nbones / 2;
    uint32_t before = m->nsockets;
    bool dirty = m->dirty;
    AnimSocket* k = anim_socket_add(m, hand);
    if (!k) {
        FAIL("the socket could not be made");
        return;
    }
    snprintf(k->name, sizeof k->name, "Q7TestSocket");
    k->has_preview = 1, k->preview_mesh = SHAPE_CUBE, k->local.s[0] = k->local.s[1] = k->local.s[2] = 0.25f;
    int si = (int)(k - m->sockets);
    // its place at the bind pose, then moved by 10 cm up in the world
    float bones[ANIM_MAX_BONES][3][4], r0[3][4], r1[3][4];
    memcpy(bones, m->bind_world, sizeof(float) * 12 * m->nbones);
    anim_socket_rows(m, k, (const float(*)[3][4])bones, r0);
    float up[3] = { 0, 0, 10.0f };
    avp_socket_move_world(m, si, (const float(*)[3][4])bones, up);
    anim_socket_rows(m, k, (const float(*)[3][4])bones, r1);
    if (fabsf(r1[0][3] - r0[0][3]) > 0.05f || fabsf(r1[1][3] - r0[1][3]) > 0.05f || fabsf(r1[2][3] - r0[2][3] - 10.0f) > 0.05f)
        FAIL("the moved socket is at %.2f %.2f %.2f, not 10 cm above", r1[0][3] - r0[0][3], r1[1][3] - r0[1][3], r1[2][3] - r0[2][3]);
    // saved into a copy (the Mannequin's file untouched) and read back
    char keep[512], copy[700];
    snprintf(keep, sizeof keep, "%s", m->path);
    snprintf(copy, sizeof copy, "%s/SK_Q7Socket.rskm", T.dir);
    snprintf(m->path, sizeof m->path, "%s", copy);
    bool saved = anim_mesh_save(m);
    snprintf(m->path, sizeof m->path, "%s", keep);
    SkelMesh* back = saved ? anim_mesh_load("/Content/Q7Test/SK_Q7Socket.rskm") : NULL;
    int bi = back ? anim_socket_index(back, "Q7TestSocket") : -1;
    if (!saved || bi < 0) FAIL("the socket was not saved and read back");
    else {
        const AnimSocket* q = &back->sockets[bi];
        if (q->bone != hand || !q->has_preview || q->preview_mesh != SHAPE_CUBE || fabsf(q->local.t[0] - k->local.t[0]) > 0.01f ||
            fabsf(q->local.t[2] - k->local.t[2]) > 0.01f)
            FAIL("the socket read back differs (bone %d, preview %d)", q->bone, q->has_preview);
    }
    printf("[RealEngine] Asset viewport: socket \"%s\" on %s made, moved, saved and read back\n", k->name, m->bones[hand].name);
    (void)before;
    m->dirty = dirty;
}

// Called each editor frame after the UI: true when it finished (*ok: passed)
bool avp_test_frame(Editor* ed, bool* ok) {
    if (!T.running) return false;
    switch (T.state) {
    case S_OPEN:
        if (T.cas >= C_COUNT) {
            T.state = S_DONE;
            break;
        }
        if (T.cas != C_SKELETON && T.cas != C_ANIM) close_editors(ed); // (the persona's window kept: its tabs switch it)
        if (!open_case(ed, T.cas)) {
            printf("[RealEngine] Asset viewport: %s: no example asset here, skipped\n", g_case_names[T.cas]);
            T.cas++;
            break;
        }
        T.state = S_SETTLE, T.wait = 0, T.mode = 0, T.frames0 = avp_test_frames(g_case_keys[T.cas]);
        avp_test_force(PREVIEW_LIT);
        break;
    case S_SETTLE: // (the meshes, the materials' bakes, the window laid out)
        if (++T.wait < 40) break;
        if (avp_test_frames(g_case_keys[T.cas]) == T.frames0) {
            FAIL("%s: its viewport was never drawn", g_case_names[T.cas]);
            T.cas++, T.state = S_OPEN;
            break;
        }
        T.state = S_MODE, T.wait = 0;
        break;
    case S_MODE:
        avp_test_force(T.mode);
        if (++T.wait < 4) break;
        render_preview_readback(g_case_keys[T.cas]);
        T.state = S_READ, T.wait = 0;
        break;
    case S_READ: {
        const uint8_t* px;
        uint32_t w, h;
        if (!render_preview_read(g_case_keys[T.cas], &px, &w, &h)) {
            if (++T.wait > 90) FAIL("%s: %s: the picture was not read back", g_case_names[T.cas], bp_preview_mode_name(T.mode)), T.mode = PREVIEW_MODE_COUNT;
            else break;
        } else {
            double share = model_share(px, w, h, T.mode, T.cas, T.mode);
            bool particles = T.cas == C_PARTICLES; // (its particles are drawn over the picture: counted by the editor)
            if (particles) {
                if (T.mode == 0 && particle_editor_preview_count() <= 0) FAIL("%s: no particles drawn", g_case_names[T.cas]);
            } else if (share < 0.003)
                FAIL("%s: %s: the model is not in the picture (%.3f%% of it)", g_case_names[T.cas], bp_preview_mode_name(T.mode), share * 100.0);
            if (T.mode == PREVIEW_LIT) T.lit_frac[T.cas] = share;
            if (T.mode == 0)
                printf("[RealEngine] Asset viewport: %s: %ux%u, the model %.1f%% of the picture%s\n", g_case_names[T.cas], w, h, share * 100.0,
                       particles ? " (its particles over it)" : "");
            T.mode++;
        }
        if (T.mode >= PREVIEW_MODE_COUNT) {
            if (T.cas == C_SKELETON) { // (the socket: a cube on the hand adds to the Skeletal Mesh's pixels)
                T.state = S_SOCKET, T.wait = 0;
                socket_test();
                avp_test_force(PREVIEW_LIT);
                break;
            }
            if (T.cas == C_CLOTH && T.cloth) ecs_destroy(&ed->world, T.cloth), T.cloth = ENTITY_NONE, editor_select_none(ed);
            T.cas++, T.state = S_OPEN;
        } else
            T.state = S_MODE, T.wait = 0;
        break;
    }
    case S_SOCKET: { // its preview cube drawn: more of the picture than without it
        if (++T.wait == 6) render_preview_readback(BP_PV_KEY_ANIM);
        const uint8_t* px;
        uint32_t w, h;
        if (T.wait > 6 && render_preview_read(BP_PV_KEY_ANIM, &px, &w, &h)) {
            double share = model_share(px, w, h, PREVIEW_LIT, C_SKELETON, -1);
            if (share <= T.lit_frac[C_SKEL] + 0.0003)
                FAIL("the socket's preview mesh is not drawn (%.3f%% with it, %.3f%% without)", share * 100.0, T.lit_frac[C_SKEL] * 100.0);
            else printf("[RealEngine] Asset viewport: the socket's preview mesh drawn (%.2f%% of the picture, %.2f%% without)\n", share * 100.0, T.lit_frac[C_SKEL] * 100.0);
            int si = anim_socket_index(T.mesh, "Q7TestSocket");
            if (si >= 0) { // (the Mannequin as it was)
                bool dirty = T.mesh->dirty;
                memmove(&T.mesh->sockets[si], &T.mesh->sockets[si + 1], sizeof(AnimSocket) * (T.mesh->nsockets - (uint32_t)si - 1u));
                T.mesh->nsockets--;
                T.mesh->dirty = dirty;
            }
            T.cas++, T.state = S_OPEN;
        } else if (T.wait > 100) {
            FAIL("the socket's picture was not read back");
            T.cas++, T.state = S_OPEN;
        }
        break;
    }
    case S_COST1:
    case S_COST3: { // 60 frames to settle (the meshes, the bakes), 120 measured
        int k = T.state == S_COST1 ? 0 : 1;
        T.wait++;
        if (T.wait > 60) {
            AvpStats st = avp_stats();
            T.cpu[k] += st.cpu_ms, T.gpu[k] += st.gpu_ms, T.samples++;
        }
        if (T.wait >= 180) {
            T.cpu[k] /= (double)T.samples, T.gpu[k] /= (double)T.samples, T.samples = 0, T.wait = 0;
            if (k == 0) { // then three: the Skeletal Mesh, a Static Mesh and a material
                static char found[8][512];
                if (content_search(".rmesh", found, 8) > 0) asset_viewer_open(found[0]);
                if (content_search(".rmat", found, 8) > 0) mat_editor_open(found[0]);
                T.state = S_COST3;
            } else {
                printf("[RealEngine] Asset viewport cost: 1 editor CPU %.3f ms GPU %.3f ms; 3 editors CPU %.3f ms GPU %.3f ms (%u views)\n", T.cpu[0],
                       T.gpu[0], T.cpu[1], T.gpu[1], avp_stats().views);
                if (T.gpu[1] > 3.0 || T.cpu[1] > 1.5) FAIL("3 editors cost more than 3 ms GPU / 1.5 ms CPU");
                close_editors(ed);
                T.state = S_OPEN, T.wait = 0;
            }
        }
        break;
    }
    case S_DONE: {
        close_editors(ed);
        if (T.sheet) {
            vkctx_png_write_rgb("asset_viewport_test.png", T.sheet, THUMB_W * PREVIEW_MODE_COUNT, THUMB_H * C_COUNT);
            free(T.sheet), T.sheet = NULL;
        }
        if (!getenv("REALENGINE_AVP_KEEP")) re_remove_tree(T.dir); // (what the test made: the Physics Asset, IK Rig, vehicle, the socket's copy)
        T.running = false;
        *ok = T.fails == 0;
        printf("[RealEngine] Asset viewport test: %s (%d editors x %d view modes)\n", T.fails ? "FAIL" : "OK", C_COUNT, PREVIEW_MODE_COUNT);
        return true;
    }
    }
    return false;
}
