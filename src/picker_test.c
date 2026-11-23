// The asset picker's and Create from the selection's test (Q6, 2026-11-17; --picker-test, ctest asset_picker, area
// content-browser). A project with 1000 textures (Content/PickerTest, small BMPs) and the Mannequin:
//   1. the picker opened on textures, materials and Skeletal Meshes: 1000+ candidates, the time to read them and to draw
//      the first frame (the measure: notas-dev/content-browser.md);
//   2. a search by name and by folder + name finds the one;
//   3. the three views and the three sizes give the layouts they should (Name only: one per row; Small: more per row than
//      Large; Preview and name: taller rows than Preview only) and the choice survives editor.ini;
//   4. an asset chosen (as a double click) reaches its callback and the picker closes;
//   5. the right click's Create: an IK Rig from the Mannequin, a Material Instance from a material, a Material and a Light
//      Function from a texture; the Add menu's IK Rig... through the picker.
#include "picker_test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "asset_picker.h"
#include "content.h"
#include "editor.h"
#include "ikrig.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"

#define N_TEX 1000
static bool fail(const char* fmt, ...);
#define FAIL(...) fail(__VA_ARGS__)

static struct {
    bool on, done, ok;
    int stage, wait, sub;
    char picked[512];
    int picks;
    double draw_sum;
    int draw_n;
    double first_draw, steady;
} T;

static bool fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[RealEngine] picker-test FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    T.ok = false;
    return false;
}

static void on_pick(const char* ref, uint32_t user, const char* ctx) {
    (void)user;
    snprintf(T.picked, sizeof T.picked, "%s", ref);
    T.picks++;
    if (strcmp(ctx, "picker-test")) FAIL("the callback's context came back as '%s'", ctx);
}

// An 8 x 8 BMP of one color (WIC decodes it: a real thumbnail)
static bool write_bmp(const char* path, int i) {
    unsigned char h[54] = { 'B', 'M' };
    uint32_t data = 8 * 8 * 3, size = 54 + data;
    memcpy(h + 2, &size, 4);
    uint32_t off = 54, hs = 40, w = 8, hh = 8, comp = 0;
    uint16_t planes = 1, bpp = 24;
    memcpy(h + 10, &off, 4), memcpy(h + 14, &hs, 4), memcpy(h + 18, &w, 4), memcpy(h + 22, &hh, 4);
    memcpy(h + 26, &planes, 2), memcpy(h + 28, &bpp, 2), memcpy(h + 30, &comp, 4), memcpy(h + 34, &data, 4);
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    fwrite(h, 1, sizeof h, f);
    unsigned char px[3] = { (unsigned char)(i * 37), (unsigned char)(i * 91), (unsigned char)(255 - i % 256) };
    for (int k = 0; k < 64; k++) fwrite(px, 1, 3, f);
    fclose(f);
    return true;
}

bool picker_test_setup(Editor* ed) {
    (void)ed;
    memset(&T, 0, sizeof T);
    T.on = true, T.ok = true, T.wait = 30;
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s/PickerTest", content_root());
    re_remove_tree(dir); // (a run before left its assets: the names made must come out the same)
    if (!re_make_dirs(dir)) return FAIL("could not make %s", dir);
    double t0 = re_time_seconds();
    for (int i = 0; i < N_TEX; i++) {
        snprintf(path, sizeof path, "%s/Pick_%04d.bmp", dir, i);
        if (!write_bmp(path, i)) return FAIL("could not write %s", path);
    }
    printf("[RealEngine] picker-test: %d textures written in %.1f ms\n", N_TEX, (re_time_seconds() - t0) * 1000.0);
    content_mark_dirty();
    asset_picker_set_view(PICKER_VIEW_BOTH, 1);
    return true;
}

bool picker_test_finished(bool* ok) {
    if (ok) *ok = T.ok;
    return T.done;
}

static bool file_exists(const char* vpath) {
    char d[1100];
    snprintf(d, sizeof d, "%s%s", content_root(), vpath + 8);
    return re_file_stat(d, NULL, NULL, NULL);
}

static void layouts(void) {
    // (view, size) -> per_row, row_h; measured the frame after each is set
    static const int order[][2] = { { 0, 0 }, { 0, 2 }, { 1, 1 }, { 2, 1 }, { 0, 1 } };
    static int per[5];
    static float rh[5];
    int k = T.sub;
    if (k > 0) asset_picker_layout(&per[k - 1], &rh[k - 1]);
    if (k < 5) {
        asset_picker_set_view(order[k][0], order[k][1]);
        T.sub++;
        return;
    }
    printf("[RealEngine] picker-test: layouts: preview S %d/row, preview L %d/row, name %d/row (row %.0f px), both M row %.0f px, preview M row %.0f px\n",
           per[0], per[1], per[2], rh[2], rh[3], rh[4]);
    if (per[2] != 1) FAIL("Name only shows %d per row, not one", per[2]);
    if (per[0] <= per[1]) FAIL("Small previews (%d per row) are not more than Large (%d)", per[0], per[1]);
    if (rh[3] <= rh[4]) FAIL("Preview and name's rows (%.0f) are not taller than Preview only's (%.0f)", rh[3], rh[4]);
    // the choice kept in editor.ini (written and read back)
    asset_picker_set_view(PICKER_VIEW_NAME, 2);
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s/PickerTest/prefs.txt", content_root());
    FILE* f = re_fopen(tmp, "w");
    if (f) asset_picker_write_prefs(f), fclose(f);
    asset_picker_set_view(PICKER_VIEW_PREVIEW, 0);
    f = re_fopen(tmp, "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        asset_picker_read_pref(line, eq + 1);
    }
    if (f) fclose(f);
    remove(tmp);
    int v, s;
    asset_picker_get_view(&v, &s);
    if (v != PICKER_VIEW_NAME || s != 2) FAIL("the view read back from editor.ini is %d/%d, not Name only / Large", v, s);
    asset_picker_set_view(PICKER_VIEW_BOTH, 1);
    T.stage++, T.sub = 0;
}

static void makes(void) {
    // what the right click offers
    int opts[CM_COUNT], n = content_make_options(ASSET_SKELETAL_MESH, opts, CM_COUNT);
    bool ik = false, mi = false;
    for (int i = 0; i < n; i++) ik |= opts[i] == CM_IK_RIG;
    n = content_make_options(ASSET_MATERIAL, opts, CM_COUNT);
    for (int i = 0; i < n; i++) mi |= opts[i] == CM_MATERIAL_INSTANCE;
    if (!ik) FAIL("a Skeletal Mesh's right click has no Create > IK Rig");
    if (!mi) FAIL("a Material's right click has no Create > Material Instance");
    char out[800], msg[256];
    // an IK Rig from the Mannequin
    const char* sk = "/Content/Characters/Mannequin/SK_Mannequin.rskm";
    SkelMesh* m = anim_mesh_load(sk);
    if (!m) FAIL("no %s in the project", sk);
    else if (!content_make(sk, CM_IK_RIG, "/Content/PickerTest", out, sizeof out, msg, sizeof msg)) FAIL("Create > IK Rig: %s", msg);
    else {
        IkRig* r = ikrig_load(out);
        if (!r || r->skeleton != m->hash) FAIL("the IK Rig %s is not the Mannequin's", out);
        else printf("[RealEngine] picker-test: Create > IK Rig: %s (%u solvers)\n", out, r->nsolvers);
    }
    // a Material Instance from a material
    char grid[800];
    snprintf(grid, sizeof grid, "%s/Materials/M_Grid.rmat", content_root()); // (the engine's grid material, in every project)
    Material* base = mat_load(grid);
    char mvp[700];
    if (!base || !mat_vpath(base, mvp, sizeof mvp)) FAIL("no material to make an instance of");
    else if (!content_make(mvp, CM_MATERIAL_INSTANCE, "/Content/PickerTest", out, sizeof out, msg, sizeof msg)) FAIL("Create > Material Instance: %s", msg);
    else {
        char d[1100];
        snprintf(d, sizeof d, "%s%s", content_root(), out + 8);
        Material* inst = mat_load(d);
        if (!inst || !inst->instance || !strstr(inst->parent_path, base->name)) FAIL("%s is not an instance of %s", out, mvp);
        else printf("[RealEngine] picker-test: Create > Material Instance: %s (parent %s)\n", out, inst->parent_path);
    }
    // a Material and a Light Function from a texture
    const char* tex = "/Content/PickerTest/Pick_0007.bmp";
    for (int lf = 0; lf < 2; lf++) {
        if (!content_make(tex, lf ? CM_LIGHT_FUNCTION : CM_MATERIAL, NULL, out, sizeof out, msg, sizeof msg)) {
            FAIL("Create > %s: %s", lf ? "Light Function" : "Material", msg);
            continue;
        }
        char d[1100];
        snprintf(d, sizeof d, "%s%s", content_root(), out + 8);
        Material* mm = mat_load(d);
        bool has = false;
        for (int i = 0; mm && mm->graph && i < mm->graph->node_count; i++)
            has |= mm->graph->nodes[i].type == NODE_TEX_IMAGE && !strcmp(mm->graph->nodes[i].image, tex);
        if (!has) FAIL("%s has no Image Texture of %s", out, tex);
        if (mm && (mm->domain == MAT_DOMAIN_LIGHT_FUNCTION) != (lf == 1)) FAIL("%s's domain is %d", out, mm->domain);
        printf("[RealEngine] picker-test: Create > %s: %s\n", lf ? "Light Function" : "Material", out);
    }
    // (#164) the Tlender's packed set ("Real Engine (packed)"): Kit_BaseColor's Material wires Kit_NormalRoughnessMetallic
    // (normal with its Z rebuilt, roughness, metallic) and Kit_Emissive
    static const char* const KIT[] = { "Kit_BaseColor", "Kit_NormalRoughnessMetallic", "Kit_Emissive" };
    char kd[1100], from[1100];
    snprintf(from, sizeof from, "%s%s", content_root(), tex + 8);
    for (int k = 0; k < 3; k++) {
        snprintf(kd, sizeof kd, "%s/PickerTest/%s.bmp", content_root(), KIT[k]);
        re_copy_file(from, kd);
    }
    if (!content_make("/Content/PickerTest/Kit_BaseColor.bmp", CM_MATERIAL, NULL, out, sizeof out, msg, sizeof msg)) FAIL("Create > Material (packed set): %s", msg);
    else {
        char d[1100];
        snprintf(d, sizeof d, "%s%s", content_root(), out + 8);
        Material* mm = mat_load(d);
        const MatGraph* g = mm ? mm->graph : NULL;
        int bsdf = -1, nmap = -1, imgs = 0, to_bsdf = 0;
        for (int i = 0; g && i < g->node_count; i++) {
            if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
            if (g->nodes[i].type == NODE_NORMAL_MAP) nmap = i;
            imgs += g->nodes[i].type == NODE_TEX_IMAGE;
        }
        for (int i = 0; g && i < g->link_count; i++) to_bsdf += g->links[i].to == bsdf;
        // base color, roughness, metallic, normal, emission
        if (imgs != 3 || nmap < 0 || to_bsdf < 5) FAIL("packed set: %d images, Normal Map %d, %d inputs of the BSDF linked", imgs, nmap, to_bsdf);
        else printf("[RealEngine] picker-test: Create > Material (packed set): %s, 3 images, %d inputs linked\n", out, to_bsdf);
    }
}

void picker_test_frame(Editor* ed) {
    (void)ed;
    if (!T.on || T.done) return;
    if (T.wait > 0) {
        T.wait--;
        return;
    }
    switch (T.stage) {
    case 0: { // 1. opened: the candidates read, then the first frames drawn
        AssetPickerSpec s = { .title = "Pick Test", .kinds = AK(ASSET_TEXTURE_SOURCE) | AK(ASSET_MATERIAL) | AK(ASSET_SKELETAL_MESH),
                              .on_pick = on_pick, .ctx = "picker-test" };
        asset_picker_open(&s);
        int n = asset_picker_count();
        printf("[RealEngine] picker-test: opened with %d assets: read in %.2f ms\n", n, asset_picker_scan_ms());
        if (n < N_TEX) FAIL("only %d candidates (%d textures written)", n, N_TEX);
        T.stage++;
        break;
    }
    case 1: // the draws (the first, then 30 more: the thumbnails come in)
        if (!asset_picker_is_open()) {
            FAIL("the picker closed by itself");
            T.stage = 5;
            break;
        }
        if (T.draw_n == 0) T.first_draw = asset_picker_draw_ms();
        else T.draw_sum += asset_picker_draw_ms();
        if (T.draw_n == 31) T.steady = T.draw_sum, T.draw_sum = 0.0;
        if (++T.draw_n > 90) {
            printf("[RealEngine] picker-test: opening (read + first frame) %.2f ms; a frame of it %.3f ms while the thumbnails come in "
                   "(30 frames), %.3f ms after (60 frames, CPU)\n",
                   asset_picker_scan_ms() + T.first_draw, T.steady / 30.0, T.draw_sum / 60.0);
            T.stage++;
        }
        break;
    case 2: { // 2. the search
        asset_picker_set_filter("Pick_0421");
        int v = asset_picker_visible();
        if (v != 1 || !strstr(asset_picker_visible_ref(0), "Pick_0421")) FAIL("searching Pick_0421 shows %d (%s)", v, asset_picker_visible_ref(0));
        asset_picker_set_filter("pickertest 042");
        v = asset_picker_visible();
        if (v != 11) FAIL("searching 'pickertest 042' (folder and name) shows %d, not 11 (0042, 0420-0429)", v);
        asset_picker_set_filter("");
        T.stage++;
        break;
    }
    case 3: layouts(); break; // 3. the views and the sizes
    case 4: // 4. choose one
        if (T.sub == 0) {
            asset_picker_set_filter("Pick_0421");
            asset_picker_choose(0);
            T.sub = 1;
        } else {
            if (asset_picker_is_open()) FAIL("the picker is still open after choosing");
            if (T.picks != 1 || strcmp(T.picked, "/Content/PickerTest/Pick_0421.bmp")) FAIL("chose '%s' (%d calls)", T.picked, T.picks);
            else printf("[RealEngine] picker-test: chose %s\n", T.picked);
            T.stage++, T.sub = 0;
        }
        break;
    case 5: // 5. Create from the selection
        makes();
        // the Add menu's IK Rig...: the picker on the Skeletal Meshes, then made in the folder
        content_pick_and_make(CM_IK_RIG, "/Content/PickerTest");
        if (!asset_picker_is_open()) FAIL("Add > IK Rig... opened no picker");
        asset_picker_set_filter("SK_Mannequin");
        if (asset_picker_visible() < 1) FAIL("the Skeletal Mesh picker has no SK_Mannequin");
        asset_picker_choose(0);
        T.stage++;
        break;
    case 6:
        if (!file_exists("/Content/PickerTest/IK_SK_Mannequin2.rikrig")) FAIL("Add > IK Rig... (picked SK_Mannequin) made no IK_SK_Mannequin2");
        else printf("[RealEngine] picker-test: Add > IK Rig... through the picker: IK_SK_Mannequin2\n");
        T.stage++;
        break;
    default:
        printf("[RealEngine] picker-test: %s\n", T.ok ? "all passed" : "FAILED");
        T.done = true;
        break;
    }
}
