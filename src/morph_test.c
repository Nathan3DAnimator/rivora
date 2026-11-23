// Morph targets (--morph-test; notas-dev/animacao.md "Morph targets"): a glTF written here (a 128 x 128 grid standing
// up, 16 k vertices, no skin, two morph targets: "Wave" over the whole grid at its default 0.5, "Bulge" in its middle)
// imported as a one-bone Skeletal Mesh. Its skin's vertices read back from the GPU at the defaults, then Wave 1 and
// Bulge 1 set by the Set Morph Target node's function (anim_morph_set): every vertex must have moved by exactly the
// change of weight x its targets' offsets (checked against the grid's formula, under 0.05 cm). Then the measure: the
// weights animated every frame, the skin bent without its morphs (the bones' cost alone) and with them, the GPU's
// skin time (timestamps) and the CPU's; then still: nothing bent (zero cost while the weights stay).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"

#define GRID 128
#define NV (GRID * GRID)

static Entity g_actor;
static int g_state, g_frames, g_wait;
static bool g_ok = true, g_done;
static float (*g_a)[3], (*g_b)[3];
static double g_gpu[2], g_cpu[2];
static int g_n[2], g_still_bent;

// The targets' offsets (glTF meters, along glTF's +Z: the grid's normal) at a grid point (x -1..1, y 0..2)
static float wave(float x) { return 0.2f * sinf(x * 6.0f); }
static float bulge(float x, float y) { return 0.5f * expf(-(x * x + (y - 1.0f) * (y - 1.0f)) * 8.0f); }

static bool write_gltf(const char* dir, char* path, size_t cap, bool anim) {
    size_t npos = (size_t)NV * 12, nidx = (size_t)(GRID - 1) * (GRID - 1) * 6 * 2;
    size_t bytes = npos * 4 + nidx + (anim ? 6 * 4 : 0); // positions, normals, the two targets; the indices (uint16); the animation's
    const char* stem = anim ? "morph_anim" : "morph_grid";
    uint8_t* bin = calloc(1, bytes);
    if (!bin) return false;
    float* p = (float*)bin;
    float *nrm = p + NV * 3, *tw = nrm + NV * 3, *tb = tw + NV * 3;
    uint16_t* ix = (uint16_t*)(bin + npos * 4);
    for (int j = 0; j < GRID; j++)
        for (int i = 0; i < GRID; i++) {
            int v = j * GRID + i;
            float x = (float)i / (GRID - 1) * 2.0f - 1.0f, y = (float)j / (GRID - 1) * 2.0f;
            p[v * 3] = x, p[v * 3 + 1] = y, p[v * 3 + 2] = 0.0f;
            nrm[v * 3 + 2] = 1.0f;
            tw[v * 3 + 2] = wave(x), tb[v * 3 + 2] = bulge(x, y);
        }
    size_t k = 0;
    for (int j = 0; j + 1 < GRID; j++)
        for (int i = 0; i + 1 < GRID; i++) {
            uint16_t a = (uint16_t)(j * GRID + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + GRID), d = (uint16_t)(c + 1);
            ix[k++] = a, ix[k++] = b, ix[k++] = d, ix[k++] = a, ix[k++] = d, ix[k++] = c;
        }
    size_t ao = npos * 4 + nidx; // (the animation's: times 0 1; weights Wave, Bulge: 0 0, then 1 0.5)
    if (anim) {
        float* an = (float*)(bin + ao);
        an[0] = 0.0f, an[1] = 1.0f, an[2] = 0.0f, an[3] = 0.0f, an[4] = 1.0f, an[5] = 0.5f;
    }
    char bp[700];
    snprintf(bp, sizeof bp, "%s/%s.bin", dir, stem);
    FILE* f = re_fopen(bp, "wb");
    bool ok = f && fwrite(bin, 1, bytes, f) == bytes;
    if (f) ok = fclose(f) == 0 && ok;
    free(bin);
    snprintf(path, cap, "%s/%s.gltf", dir, stem);
    f = ok ? re_fopen(path, "wb") : NULL;
    if (!f) return false;
    char av[200] = "", aa[200] = "", an[400] = "";
    if (anim) {
        snprintf(av, sizeof av, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":8},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":16}", ao, ao + 8);
        snprintf(aa, sizeof aa, ",{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
                                "{\"bufferView\":6,\"componentType\":5126,\"count\":4,\"type\":\"SCALAR\"}");
        snprintf(an, sizeof an, ",\"animations\":[{\"name\":\"Smile\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}],"
                                "\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"}]}]");
    }
    fprintf(f,
            "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Real Engine morph test\"},\"scenes\":[{\"nodes\":[0]}],"
            "\"nodes\":[{\"name\":\"%s\",\"mesh\":0}],"
            "\"meshes\":[{\"name\":\"Grid\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":4,"
            "\"targets\":[{\"POSITION\":2},{\"POSITION\":3}]}],\"weights\":[0.5,0.0],\"extras\":{\"targetNames\":[\"Wave\",\"Bulge\"]}}],"
            "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":%zu}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}%s],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[-1,0,0],\"max\":[1,2,0]},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
            "{\"bufferView\":3,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},{\"bufferView\":4,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"}%s]%s}\n",
            anim ? "MorphAnim" : "MorphGrid", stem, bytes, npos, npos, npos, npos * 2, npos, npos * 3, npos, npos * 4, nidx, av, NV, NV, NV, NV,
            nidx / 2, aa, an);
    ok = fclose(f) == 0;
    return ok;
}

// The morph curves: the animated grid imported, its curves, the actor's weights driven by them, a curve saved, the cost
static Entity g_cactor;
static AnimSeq* g_cseq;
static int g_cframes;
static void curves_test(Editor* ed) {
    char dir[600], path[700];
    if (!re_cache_dir(dir, sizeof dir)) return;
    size_t l = strlen(dir);
    snprintf(dir + l, sizeof dir - l, "/morph_test");
    if (!write_gltf(dir, path, sizeof path, true) || !editor_import_tlend(ed, path, "/Content/MorphTest/Anim", true)) {
        printf("[RealEngine] FAIL: morph curves: the animated glTF not imported\n");
        g_ok = false;
        return;
    }
    World* w = &ed->world;
    Entity actor = ENTITY_NONE;
    CSkeletalMesh* c = NULL;
    for (uint32_t s = 1; s < w->slot_count && !actor; s++) {
        Entity e = ecs_entity_at(w, s);
        c = e ? ecs_get(w, e, COMP_SKELETAL_MESH) : NULL;
        const SkelMesh* m = c ? anim_mesh(c->mesh) : NULL;
        if (m && m->nmorphs == 2 && c->anim) actor = e;
    }
    AnimSeq* sq = actor ? anim_seq(c->anim) : NULL;
    if (!sq || sq->ncurves != 2) {
        printf("[RealEngine] FAIL: morph curves: no actor playing a sequence with the 2 curves (%u)\n", sq ? sq->ncurves : 0u);
        g_ok = false;
        return;
    }
    int cw = anim_curve_index(sq, "wave"), cb = anim_curve_index(sq, "Bulge");
    float w5 = anim_curve_eval(sq, (uint32_t)cw, 0.5f), b5 = anim_curve_eval(sq, (uint32_t)cb, 0.5f);
    printf("[RealEngine] Morph curves: %s, %u curves, %u keys; at 0.5 s Wave %.3f, Bulge %.3f\n", sq->name, sq->ncurves, sq->ncurve_keys, w5, b5);
    if (cw < 0 || cb < 0 || fabsf(w5 - 0.5f) > 0.01f || fabsf(b5 - 0.25f) > 0.01f) printf("[RealEngine] FAIL: morph curves: the keys\n"), g_ok = false;
    g_cactor = actor, g_cseq = sq, g_cframes = 0;
    anim_morph_set(w, actor, "Wave", 0.9f); // (by hand: the animation's curve goes over it)
}

// Played (frames: the new mesh loads first): the pose's curves drive the weights, over the value set by hand
static bool curves_play(Editor* ed) {
    World* w = &ed->world;
    AnimSeq* sq = g_cseq;
    double t0, upd = anim_stats()->ms; // (the Play poses it: the editor's anim_update)
    float wv = anim_morph_get(w, g_cactor, "Wave"), bv = anim_morph_get(w, g_cactor, "Bulge");
    if (++g_cframes < 120 && (wv == 0.9f || wv <= 0.05f)) return false; // (not posed yet / at the start)
    printf("[RealEngine] Morph curves: played (%d frames): Wave %.3f, Bulge %.3f (Set Morph Target had put Wave at 0.9); anim_update %.3f ms\n",
           g_cframes, wv, bv, upd);
    if (wv <= 0.05f || wv >= 0.9f || fabsf(bv - 0.5f * wv) > 0.02f) printf("[RealEngine] FAIL: morph curves: the weights don't follow the curves\n"), g_ok = false;
    int cw = anim_curve_index(sq, "Wave");
    // a curve authored (the sequence editor's way) on a copy of the sequence: Bulge 1 -> 0, saved in its .ranim
    AnimSeq cp = *sq;
    cp.curve_keys = NULL, cp.ncurve_keys = 0, cp.ncurves = 0;
    char dir[600];
    re_cache_dir(dir, sizeof dir);
    snprintf(cp.path, sizeof cp.path, "%s/morph_test/A_Authored.ranim", dir);
    float kt[2] = { 0.0f, 1.0f }, kv[2] = { 1.0f, 0.0f };
    if (!anim_curve_set(&cp, "Bulge", kt, kv, 2) || !anim_seq_save(&cp)) printf("[RealEngine] FAIL: morph curves: the curve not saved\n"), g_ok = false;
    FILE* f = re_fopen(cp.path, "rb");
    char text[8192] = "";
    if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    if (!strstr(text, "\"curves\"") || !strstr(text, "\"Bulge\"") || fabsf(anim_curve_eval(&cp, 0, 0.25f) - 0.75f) > 1e-4f)
        printf("[RealEngine] FAIL: morph curves: the .ranim lacks the authored curve\n"), g_ok = false;
    free(cp.curve_keys);
    // the cost: a curve's value (a binary search in its keys)
    volatile float sink = 0.0f;
    t0 = re_time_seconds();
    for (int i = 0; i < 1000000; i++) sink += anim_curve_eval(sq, (uint32_t)cw, (float)(i % 1000) * 0.001f);
    printf("[RealEngine] Morph curves: %.1f ns a curve's value (%u keys)\n", (re_time_seconds() - t0) * 1e9 / 1e6, sq->curve_count[cw]);
    (void)sink;
    return true;
}

bool morph_test_setup(Editor* ed) {
    char dir[600], path[700];
    if (!re_cache_dir(dir, sizeof dir)) return false;
    size_t l = strlen(dir);
    snprintf(dir + l, sizeof dir - l, "/morph_test");
    re_make_dirs(dir);
    if (!write_gltf(dir, path, sizeof path, false)) {
        printf("[RealEngine] FAIL: morph test: the glTF not written\n");
        return false;
    }
    if (!editor_import_tlend(ed, path, "/Content/MorphTest", true)) return false;
    World* w = &ed->world;
    for (uint32_t s = 1; s < w->slot_count && !g_actor; s++) {
        Entity e = ecs_entity_at(w, s);
        const CSkeletalMesh* c = e ? ecs_get(w, e, COMP_SKELETAL_MESH) : NULL;
        const SkelMesh* m = c ? anim_mesh(c->mesh) : NULL;
        if (m && m->nmorphs == 2) g_actor = e;
    }
    if (!g_actor) {
        printf("[RealEngine] FAIL: morph test: no Skeletal Mesh with the 2 morph targets imported\n");
        return false;
    }
    CTransform* t = ecs_get(w, g_actor, COMP_TRANSFORM);
    if (t) t->location[0] = 0.0f, t->location[1] = 1500.0f, t->location[2] = 50.0f; // (away from the default level's shapes)
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = 1500.0f + 450.0f, ed->cam.pos[2] = 150.0f; // (the grid faces +Y: glTF's +Z)
    ed->cam.yaw = 270.0f, ed->cam.pitch = 0.0f;
    g_a = malloc(sizeof *g_a * NV), g_b = malloc(sizeof *g_b * NV);
    return g_a && g_b;
}

bool morph_test_done(void) { return g_done; }

void morph_test_frame(Editor* ed, VkCtx* vk) {
    if (g_done || !g_actor) return;
    World* w = &ed->world;
    g_frames++;
    if (g_frames > 1200 && g_state < 5) { // (20 s at 60 fps)
        printf("[RealEngine] FAIL: morph test: stuck in step %d (no skin bent)\n", g_state);
        g_ok = false, g_done = true;
        return;
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    switch (g_state) {
    case 0: { // the defaults: Wave 0.5 (the file's), Bulge 0
        uint32_t n = render_skin_readback(vk, w, g_actor, g_a, NV);
        if (n != NV) return; // (not bent yet: the meshes still loading)
        float wv = anim_morph_get(w, g_actor, "Wave"), bg = anim_morph_get(w, g_actor, "bulge"); // (names: case ignored)
        if (fabsf(wv - 0.5f) > 1e-6f || fabsf(bg) > 1e-6f) {
            printf("[RealEngine] FAIL: morph test: the default weights are %.3f %.3f (0.5 0 expected)\n", wv, bg);
            g_ok = false;
        }
        if (!anim_morph_set(w, g_actor, "Wave", 1.0f) || !anim_morph_set(w, g_actor, "Bulge", 1.0f) || anim_morph_set(w, g_actor, "Nope", 1.0f)) {
            printf("[RealEngine] FAIL: morph test: Set Morph Target by name\n");
            g_ok = false;
        }
        g_state = 1, g_wait = 3;
        break;
    }
    case 1: { // Wave 1, Bulge 1: each vertex moved along the grid's normal by 0.5 wave + 1 bulge (the Engine's +Y, cm)
        uint32_t n = render_skin_readback(vk, w, g_actor, g_b, NV);
        if (n != NV) return;
        double worst = 0.0, most = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            float x = g_a[i][0] / 100.0f, y = g_a[i][2] / 100.0f; // (glTF x, y: the Engine's x, z)
            float want[3] = { 0.0f, 100.0f * (0.5f * wave(x) + bulge(x, y)), 0.0f };
            for (int k = 0; k < 3; k++) {
                double d = fabs((double)(g_b[i][k] - g_a[i][k]) - want[k]);
                if (d > worst) worst = d;
            }
            double mv = fabs((double)(g_b[i][1] - g_a[i][1]));
            if (mv > most) most = mv;
        }
        printf("[RealEngine] Morph test: %u vertices read back, the most moved %.2f cm (the bulge's middle: 50), worst error %.4f cm\n", n,
               most, worst);
        if (worst > 0.05 || most < 45.0 || most > 55.0) {
            printf("[RealEngine] FAIL: morph test: the vertices did not move by the targets (error %.4f cm)\n", worst);
            g_ok = false;
        }
        g_state = 2, g_frames = 0;
        render_morph_off(true);
        break;
    }
    case 2: { // the measure: 90 frames bones only, 90 with the morphs (the weights changing every frame), then still
        int phase = g_frames <= 90 ? 0 : g_frames <= 180 ? 1 : 2;
        if (phase == 1 && g_frames == 91) render_morph_off(false);
        float s = 0.5f + 0.5f * sinf((float)g_frames * 0.2f);
        if (phase < 2) anim_morph_set(w, g_actor, "Wave", s), anim_morph_set(w, g_actor, "Bulge", 1.0f - s);
        uint32_t sk_n, sk_b, sk_v;
        double sk_ms;
        render_skin_stats(&sk_n, &sk_b, &sk_v, &sk_ms);
        int f = phase == 0 ? g_frames : g_frames - 90;
        if (phase < 2 && f > 10) { // (the timestamps: two frames late; the first ones warm up)
            g_gpu[phase] += render_stats()->gpu_skin_ms, g_cpu[phase] += sk_ms, g_n[phase]++;
        }
        if (phase == 2 && g_frames > 190) g_still_bent += (int)sk_b; // (still weights: nothing bent)
        if (g_frames >= 220) g_state = 3;
        break;
    }
    case 3: // the morph curves: the animated grid imported
        curves_test(ed);
        if (g_cseq) editor_play(ed, 0.0); // (the curves drive the weights while playing, as the poses do)
        g_state = g_cseq ? 4 : 5;
        break;
    case 4: // played until its weights follow the curves
        if (curves_play(ed)) g_state = 5, editor_stop(ed, 0.0);
        break;
    case 5: {
        uint32_t mm, mj, mv;
        render_morph_stats(&mm, &mj, &mv);
        double gb = g_n[0] ? g_gpu[0] / g_n[0] : 0.0, gm = g_n[1] ? g_gpu[1] / g_n[1] : 0.0;
        double cb = g_n[0] ? g_cpu[0] / g_n[0] : 0.0, cm = g_n[1] ? g_cpu[1] / g_n[1] : 0.0;
        printf("[RealEngine] Morph cost (%d vertices, 2 targets, bent every frame): GPU %.4f ms bones only, %.4f ms with the morphs (+%.4f); "
               "CPU %.4f ms, %.4f ms; still: %d bends in 30 frames\n",
               NV, gb, gm, gm - gb, cb, cm, g_still_bent);
        if (g_still_bent) {
            printf("[RealEngine] FAIL: morph test: the skin was bent while its weights stayed the same\n");
            g_ok = false;
        }
        if (g_ok) printf("[RealEngine] Morph test: OK (%u meshes with morph targets)\n", mm);
        g_done = true;
        break;
    }
    }
}

bool morph_test_check(Editor* ed) {
    (void)ed;
    if (!g_done) printf("[RealEngine] FAIL: morph test: not finished\n");
    free(g_a), free(g_b), g_a = g_b = NULL;
    return g_ok && g_done;
}
