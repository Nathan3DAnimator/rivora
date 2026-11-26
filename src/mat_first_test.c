// (2026-11-24) --scene firstframe (ctest mat_first_frame, notas-dev/materiais.md "Shading mais leve"): a mesh shows its
// material's color from the first frame, before any bake (the placeholder: material_gpu.inc ph_write), and its textures
// come small and climb to the full size in steps (64, 256, 1024: the first bake and the streaming's climb). Four cubes
// filling the view, each with its own new 1024 material (red, green, blue, a yellow from an RGB node through a Mix);
// REALENGINE_MAT_FIRST_DELAY keeps the bakes off for the first frames; the first frame is shot and read back: each cube's
// mean color against its Base Color (not white). Prints the time to that first colored frame and the sizes climbed
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "content.h"
#include "ecs.h"
#include "editor.h"
#include "mat_first_test.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "vkctx.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

static struct {
    bool on;
    Material* m[4];
    float col[4][3];
    double t0, t_first;
    uint32_t frame, shot_frame;
    char shot[800];
    uint32_t sizes[8], nsizes;
    double t_full;
} F;

static int sock_in(int type, const char* name) {
    const MatNodeDef* d = mat_node_def(type);
    for (int i = 0; d && i < d->nin; i++)
        if (!strcmp(d->in[i].name, name)) return i;
    return -1;
}

static double lin8(uint8_t v) {
    double s = v / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static Entity cube(World* w, float x, float z, uint32_t mat) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "FirstCube");
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = 0.0f, t->location[2] = z, t->flags = MOBILITY_STATIC;
    t->scale[0] = 1.9f, t->scale[1] = 0.2f, t->scale[2] = 1.9f;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = SHAPE_CUBE, m->material = mat, m->uv_scale = 1.0f, m->roughness = 0.6f, m->cast_shadow = 1;
    m->base_color[0] = m->base_color[1] = m->base_color[2] = 1.0f; // (white: what a mesh without its material shows)
    return e;
}

bool mat_first_scene(Editor* ed) {
    memset(&F, 0, sizeof F);
    F.on = true;
    F.t0 = re_time_seconds();
    char dir[700], path[800];
    snprintf(dir, sizeof dir, "%s\\FirstFrame", content_root());
    re_make_dirs(dir);
    snprintf(F.shot, sizeof F.shot, "%s\\first_frame.png", dir);
    re_remove_file(F.shot);
    static const float cols[4][3] = { { 0.8f, 0.05f, 0.05f }, { 0.05f, 0.7f, 0.05f }, { 0.05f, 0.1f, 0.8f }, { 0.8f, 0.7f, 0.05f } };
    memcpy(F.col, cols, sizeof cols);
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s\\M_First%d.rmat", dir, i);
        re_remove_file(path);
        Material* m = mat_create(path, false, NULL);
        F.m[i] = m;
        if (!m || !m->graph) continue;
        m->resolution = 1024;
        MatGraph* g = m->graph;
        int bsdf = -1;
        for (int n = 0; n < g->node_count; n++)
            if (g->nodes[n].type == NODE_BSDF_PRINCIPLED) bsdf = n;
        int bc = sock_in(NODE_BSDF_PRINCIPLED, "Base Color");
        if (bsdf < 0 || bc < 0) continue;
        if (i < 3) memcpy(g->nodes[bsdf].in[bc], cols[i], 12);
        else { // the yellow from an RGB node mixed (factor 1) over a noise: the placeholder follows the Mix
            int noise = mat_graph_add(g, NODE_TEX_NOISE, -600, 0), rgb = mat_graph_add(g, NODE_RGB, -600, 200), mix = mat_graph_add(g, NODE_MIX, -300, 0);
            memcpy(g->nodes[rgb].value, cols[3], 12);
            g->nodes[mix].in[0][0] = 1.0f;
            mat_graph_link(g, noise, 1, mix, 5), mat_graph_link(g, rgb, 0, mix, 6), mat_graph_link(g, mix, 2, bsdf, bc);
        }
        mat_save(m);
        mat_mark_dirty(m);
    }
    World* w = &ed->world;
    ecs_shutdown(w);
    ecs_init(w);
    ed->selected = ENTITY_NONE;
    ed->game_view = true;
    ed->view_mode = VIEW_UNLIT; // (the base colors as they are: no light in the scene, as udim_test.c)
    for (int i = 0; i < 4; i++) cube(w, i % 2 ? 100.0f : -100.0f, i / 2 ? 60.0f : 260.0f, F.m[i] ? F.m[i]->hash : 0u);
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = -520.0f, ed->cam.pos[2] = 160.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = 0.0f;
    mat_hold_bakes(100000u); // (no bake until the first frame drawn is shot: mat_first_tick lets them go)
    printf("[RealEngine] first frame scene: 4 materials, none baked yet\n");
    return true;
}

void mat_first_tick(VkCtx* vk) {
    if (!F.on) return;
    F.frame++;
    // the first frame the viewport draws (its pipelines made: before that it is black, whatever the materials): shot, the
    // bakes held until it is on disk (two frames later), then let go
    if (!F.shot_frame && F.frame >= 2 && render_stats()->visible >= 4) { // (the last frame drew the four cubes)
        F.shot_frame = F.frame;
        F.t_first = re_time_seconds();
        vkctx_request_shot(vk, F.shot);
    } else if (F.shot_frame && F.frame == F.shot_frame + 3) mat_hold_bakes(0u);
    uint32_t s = F.m[0] ? mat_baked_size(F.m[0]) : 0u;
    if (s && (!F.nsizes || F.sizes[F.nsizes - 1] != s) && F.nsizes < 8) {
        F.sizes[F.nsizes++] = s;
        if (F.m[0] && s >= (uint32_t)F.m[0]->resolution && !F.t_full) F.t_full = re_time_seconds();
    }
}

bool mat_first_check(Editor* ed) {
    if (!F.on) return true;
    bool ok = true;
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(F.shot, &w, &h);
    if (!px) {
        printf("[RealEngine] FAIL: first frame: no shot of the first frame (%s)\n", F.shot);
        return false;
    }
    const RenderView* rv = &ed->render_view;
    uint32_t x0 = (uint32_t)rv->x, y0 = (uint32_t)rv->y, vw = (uint32_t)rv->w, vh = (uint32_t)rv->h;
    if (!vw || !vh || x0 + vw > w || y0 + vh > h) x0 = y0 = 0, vw = w, vh = h;
    // the pixels of each material's hue (its color's shares of the sum, saturated): a cube that came out white or grey
    // (its material not shown yet) has none. Robust to the field of view: the cubes are wherever they fall on screen
    uint64_t cnt[4] = { 0, 0, 0, 0 }, total = 0;
    for (uint32_t y = y0; y < y0 + vh; y += 2)
        for (uint32_t x = x0; x < x0 + vw; x += 2, total++) {
            const uint8_t* a = &px[((size_t)y * w + x) * 4];
            double r = a[0], g = a[1], b = a[2], mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
            if (mx < 8.0 || (mx - mn) / mx < 0.35) continue;
            r = lin8(a[0]), g = lin8(a[1]), b = lin8(a[2]); // (the shot is sRGB; the materials' colors linear)
            double sum = r + g + b;
            for (int i = 0; i < 4; i++) {
                double cs = F.col[i][0] + F.col[i][1] + F.col[i][2];
                double d = fmax(fabs(r / sum - F.col[i][0] / cs), fmax(fabs(g / sum - F.col[i][1] / cs), fabs(b / sum - F.col[i][2] / cs)));
                if (d < 0.12) cnt[i]++;
            }
        }
    for (int i = 0; i < 4; i++) {
        double share = total ? 100.0 * (double)cnt[i] / (double)total : 0.0;
        printf("[RealEngine] first frame: cube %d (color %.2f %.2f %.2f): %.1f%% of the view in its hue\n", i, F.col[i][0], F.col[i][1], F.col[i][2], share);
        if (share < 0.5) printf("[RealEngine] FAIL: first frame: cube %d isn't its material's color on the first frame\n", i), ok = false;
    }    free(px);
    printf("[RealEngine] first frame: the first frame drawn, with its colors, %.0f ms after the scene was made (frame %u, no bake before it); the first material's sizes:", (F.t_first - F.t0) * 1000.0, F.shot_frame);
    for (uint32_t k = 0; k < F.nsizes; k++) printf(" %u", F.sizes[k]);
    printf(" (full %s after %.0f ms)\n", F.t_full > 0.0 ? "reached" : "NOT reached", F.t_full > 0.0 ? (F.t_full - F.t0) * 1000.0 : 0.0);
    uint32_t full = F.m[0] ? (uint32_t)F.m[0]->resolution : 0u;
    if (F.nsizes < 3 || F.sizes[0] >= full || F.sizes[F.nsizes - 1] != full)
        printf("[RealEngine] FAIL: first frame: the textures didn't climb in steps to the full size (%u)\n", full), ok = false;
    { // (2026-11-25) each climb ended with a whole bake's mips (Kaiser, wrapping): the blits left a tiling texture's seam
        // in its small mips (the RPG demo's grass: a light line every meter far off)
        const MatStats* ms = mat_stats();
        printf("[RealEngine] first frame: the climbs' ends: %u with the Kaiser mips, %u with blits\n", ms->climb_hq, ms->climb_blit);
        if (!ms->climb_hq || ms->climb_blit)
            printf("[RealEngine] FAIL: first frame: a climb ended with blitted mips (a seam every tile far off)\n"), ok = false;
    }
    printf("[RealEngine] first frame: %s\n", ok ? "ok" : "FAILED");
    return ok;
}
