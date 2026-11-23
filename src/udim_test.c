// (Q18) UDIM's scenes and their checks (notas-dev/udim.md). The assets are made under Content/UDIM the first time
// (tiles as PNGs, imported to .rudim; the materials; the meshes):
//   --scene udim: the example: a 4 x 2 tile floor with a lit UDIM material (BC7 color, BC5 normal map) and a stone row
//   --scene udim_seams (--shot): the same floor seen from above, unlit (its base color as baked), whose tiles are one
//     smooth function across their edges; the shot is read back: no step between neighbor pixels bigger than the
//     function's own (a seam of the smallest mip, a bleed from the far edge or a missing tile would be tens of levels)
//     It also bakes a material whose .rudim has one tile (1001 alone: no MATF_UDIM, baked as any material): the bake has
//     to read that tile's source at the size baked, not the UD_MIN it's loaded at (2026-11-23: an imported 4K came out 64²)
//   --scene udim_stream: a 20-tile road (2048 a tile, 400 cm apart) under a small budget, the camera flying along it: only
//     the tiles seen climb, the ones behind go down to the least; the budget kept
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "material.h"
#include "mesh_build.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "udim.h"

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // mat_image.c

static struct {
    int scene; // 1 udim, 2 seams, 3 stream
    uint32_t tiles;
    uint32_t max_resident, samples;
    double fly0;
    uint64_t max_bytes;
    double import_ms;
} UT;

static void content(char* out, size_t cap, const char* sub) { snprintf(out, cap, "%s/UDIM/%s", mat_root(), sub); }

// A UDIM texture made when missing: count tiles from 1001 (cols a row), a smooth function of the whole UDIM space
static bool make_texture(const char* name, int count, int cols, uint32_t size, bool normal) {
    char out[900], dir[900], p[1000];
    snprintf(out, sizeof out, "%s.rudim", name);
    content(p, sizeof p, out);
    if (re_file_stat(p, NULL, NULL, NULL)) return true;
    content(dir, sizeof dir, "Textures/Source");
    re_make_dirs(dir);
    for (int i = 0; i < count; i++) {
        int udim = UDIM_FIRST + i % cols + (i / cols) * UDIM_COLS;
        char tp[1000];
        snprintf(tp, sizeof tp, "%s/%s.%d.png", dir, name, udim);
        if (!re_file_stat(tp, NULL, NULL, NULL)) udim_write_test_tile(tp, udim, size, size, normal);
    }
    char src[1000];
    snprintf(src, sizeof src, "%s/%s.%d.png", dir, name, UDIM_FIRST);
    UdimImportReport r;
    UdimImportOpts o = { normal ? 1 : 0, 0, false };
    bool ok = udim_import(src, p, &o, &r);
    UT.import_ms += r.ms;
    if (!ok) printf("[RealEngine] FAIL: UDIM: import of %s: %s\n", name, r.err);
    return ok;
}

static int sock(const MatNodeDef* d, const char* name, bool out) {
    int n = out ? d->nout : d->nin;
    for (int i = 0; i < n; i++)
        if (!strcmp(out ? d->out[i].name : d->in[i].name, name)) return i;
    return -1;
}
static int add(MatGraph* g, const char* idname, float x, float y) {
    const MatNodeDef* d = mat_node_def_by_idname(idname);
    return d ? mat_graph_add(g, d->type, x, y) : -1;
}
static void link(MatGraph* g, int from, const char* out, int to, const char* in) {
    if (from < 0 || to < 0) return;
    int a = sock(mat_node_def(g->nodes[from].type), out, true), b = sock(mat_node_def(g->nodes[to].type), in, false);
    if (a >= 0 && b >= 0) mat_graph_link(g, from, a, to, b);
}
static int principled(MatGraph* g) {
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) return i;
    return -1;
}

// kind 0: lit (color + normal), 1: emissive (the seams' shot: no light), 2: the road's (lit color), 3: one tile (lit color)
static uint32_t make_material(const char* file, int kind, int res) {
    char p[1000];
    content(p, sizeof p, file);
    Material* m = re_file_stat(p, NULL, NULL, NULL) ? mat_load(p) : NULL;
    if (m) return m->hash;
    m = mat_create(p, false, NULL);
    if (!m) return 0;
    MatGraph* g = m->graph;
    int bsdf = principled(g);
    int img = add(g, "ShaderNodeTexImage", -520, 0);
    snprintf(g->nodes[img].image, sizeof g->nodes[img].image, "/Content/UDIM/%s",
             kind == 2 ? "T_UDIM_Road.rudim" : kind == 3 ? "T_UDIM_Lone.rudim" : "T_UDIM_Color.rudim");
    if (kind == 1) {
        link(g, img, "Color", bsdf, "Emission Color");
        mat_graph_unlink_input(g, bsdf, sock(mat_node_def(NODE_BSDF_PRINCIPLED), "Base Color", false));
        float* bc = g->nodes[bsdf].in[sock(mat_node_def(NODE_BSDF_PRINCIPLED), "Base Color", false)];
        bc[0] = bc[1] = bc[2] = 0.0f;
        g->nodes[bsdf].in[sock(mat_node_def(NODE_BSDF_PRINCIPLED), "Emission Strength", false)][0] = 20.0f; // (2000 nits: a lit sign)
    } else link(g, img, "Color", bsdf, "Base Color");
    if (kind == 0) {
        int nimg = add(g, "ShaderNodeTexImage", -520, -320), nm = add(g, "ShaderNodeNormalMap", -240, -320);
        snprintf(g->nodes[nimg].image, sizeof g->nodes[nimg].image, "/Content/UDIM/T_UDIM_Normal.rudim");
        g->nodes[nimg].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
        link(g, nimg, "Color", nm, "Color");
        link(g, nm, "Normal", bsdf, "Normal");
        g->nodes[bsdf].in[sock(mat_node_def(NODE_BSDF_PRINCIPLED), "Roughness", false)][0] = 0.45f;
    }
    m->resolution = res;
    mat_save(m);
    mat_mark_dirty(m);
    return m->hash;
}

// A quad of a tile's square: its uv from the position (u along +X; engine v 0, the image's top, at the tile's +Y side)
static void quad(GpuVertex* c, float x0, float y0, float x1, float y1, int tx, int ty, float ox, float oy, float size) {
    const float P[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    const int T[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) {
        GpuVertex* g = &c[k];
        memset(g, 0, sizeof *g);
        g->pos[0] = P[T[k]][0], g->pos[1] = P[T[k]][1], g->pos[2] = 0.0f;
        g->normal_oct = 0; // (+Z: the octahedral (0, 0))
        g->uv[0] = (float)tx + (g->pos[0] - ox) / size;
        g->uv[1] = (float)-ty + 1.0f - (g->pos[1] - oy) / size;
        g->uv2[0] = g->uv2[1] = -1.0f;
    }
}

// A floor of cols x rows tiles (size cm each, subdivided sub x sub: the per-pixel shading has vertices to follow), the UV
// a tile per square (engine V down: the row above 1001 is v -1..0); laid along +X then +Y
static bool make_mesh(const char* file, int cols, int rows, float size, int sub, bool road) {
    char p[1000], dir[1000];
    content(p, sizeof p, file);
    if (re_file_stat(p, NULL, NULL, NULL)) return false;
    content(dir, sizeof dir, "");
    re_make_dirs(dir);
    int n = cols * rows;
    uint32_t ntri = (uint32_t)(n * sub * sub * 2);
    GpuVertex* c = calloc((size_t)ntri * 3, sizeof *c);
    if (!c) return false;
    uint32_t t = 0;
    for (int i = 0; i < n; i++) {
        int tx = road ? i % UDIM_COLS : i % cols, ty = road ? i / UDIM_COLS : i / cols;
        float ox = road ? (float)i * size : (float)(i % cols) * size - cols * size * 0.5f, oy = road ? -size * 0.5f : (float)(i / cols) * size - rows * size * 0.5f;
        for (int b = 0; b < sub; b++)
            for (int a = 0; a < sub; a++, t += 2) {
                float s = size / (float)sub;
                quad(&c[t * 3], ox + a * s, oy + b * s, ox + (a + 1) * s, oy + (b + 1) * s, tx, ty, ox, oy, size);
            }
    }
    MeshBuilt mb;
    bool ok = mesh_build_corners(c, ntri, false, &mb);
    if (ok) ok = rmesh_write(p, &mb), mesh_built_free(&mb);
    free(c);
    return ok;
}

static Entity actor(World* w, uint64_t extra, const char* name, float x, float y, float z) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | extra);
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z, t->flags = MOBILITY_STATIC;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    return e;
}
static Entity mesh(World* w, const char* name, uint32_t mesh_id, uint32_t mat, float x, float y, float z) {
    Entity e = actor(w, COMP_BIT(COMP_STATIC_MESH), name, x, y, z);
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = mesh_id, m->material = mat, m->uv_scale = 1.0f, m->roughness = 0.5f, m->cast_shadow = 1;
    m->base_color[0] = m->base_color[1] = m->base_color[2] = 0.8f;
    return e;
}
static void lights(World* w) {
    Entity s = actor(w, COMP_BIT(COMP_LIGHT), "Sun", 0, 0, 1000);
    ((CLight*)ecs_get(w, s, COMP_LIGHT))->type = LIGHT_DIRECTIONAL;
    CTransform* t = ecs_get(w, s, COMP_TRANSFORM);
    t->rotation[0] = -40.0f, t->rotation[1] = 30.0f;
    Entity k = actor(w, COMP_BIT(COMP_LIGHT), "Sky", 0, 0, 900);
    ((CLight*)ecs_get(w, k, COMP_LIGHT))->type = LIGHT_SKY;
}

bool udim_test_scene(Editor* ed, const char* name) {
    int sc = !strcmp(name, "udim") ? 1 : !strcmp(name, "udim_seams") ? 2 : !strcmp(name, "udim_stream") ? 3 : 0;
    if (!sc) return false;
    memset(&UT, 0, sizeof UT);
    UT.scene = sc;
    double t0 = re_time_seconds();
    bool ok = make_texture("T_UDIM_Color", 8, 4, 512, false) && make_texture("T_UDIM_Normal", 8, 4, 256, true);
    if (sc == 3) ok = ok && make_texture("T_UDIM_Road", 20, 10, 1024, false);
    if (sc == 2) ok = ok && make_texture("T_UDIM_Lone", 1, 1, 512, false);
    bool meshes = make_mesh("SM_UDIM_Floor.rmesh", 4, 2, 400.0f, 4, false);
    meshes |= sc == 3 && make_mesh("SM_UDIM_Road.rmesh", 20, 1, 400.0f, 2, true);
    if (meshes) render_meshes_reload(ed->vk);
    uint32_t lit = make_material("M_UDIM_Example.rmat", 0, 1024), glow = make_material("M_UDIM_Glow.rmat", 1, 1024);
    uint32_t road = sc == 3 ? make_material("M_UDIM_Road.rmat", 2, 2048) : 0;
    if (sc == 2 && !make_material("M_UDIM_Lone.rmat", 3, 1024)) ok = false;
    mat_bake_pending(NULL);
    mat_update(ed->vk, VK_NULL_HANDLE);
    printf("[RealEngine] UDIM scene %s: assets ready in %.0f ms (imports %.0f ms)%s\n", name, (re_time_seconds() - t0) * 1000.0, UT.import_ms,
           ok ? "" : " - FAIL");
    World* w = &ed->world;
    ed->selected = ENTITY_NONE;
    if (sc == 2) { // (the seams: nothing but the floor, unlit, no grid nor icons over it: the shot is the base color as baked)
        ecs_shutdown(w);
        ecs_init(w);
        lights(w);
        ed->game_view = true;
        ed->view_mode = VIEW_UNLIT;
    } // (else the project's level stays: its sun, sky and fog light the floors)
    uint32_t floor_id = render_mesh_id_of("/Content/UDIM/SM_UDIM_Floor.rmesh");
    if (render_mesh_slot(floor_id) < SHAPE_COUNT) printf("[RealEngine] FAIL: UDIM: the floor mesh didn't load\n");
    if (sc == 1) {
        mesh(w, "UDIM Floor", floor_id, lit, 0, 0, 2);
        Entity g = mesh(w, "UDIM Glow", floor_id, glow, 0, 1100, 2);
        CTransform* gt = ecs_get(w, g, COMP_TRANSFORM);
        gt->scale[0] = gt->scale[1] = 0.5f;
        ed->cam.pos[0] = -900.0f, ed->cam.pos[1] = -700.0f, ed->cam.pos[2] = 520.0f, ed->cam.yaw = 38.0f, ed->cam.pitch = -28.0f;
    } else if (sc == 2) {
        Entity f = mesh(w, "UDIM Seams", floor_id, lit, 0, 0, 0);
        (void)f;
        ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 600.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -89.9f; // (the floor fills the view: its X across)
    } else {
        uint32_t road_id = render_mesh_id_of("/Content/UDIM/SM_UDIM_Road.rmesh");
        if (render_mesh_slot(road_id) < SHAPE_COUNT) printf("[RealEngine] FAIL: UDIM: the road mesh didn't load\n");
        mesh(w, "UDIM Road", road_id, road, 0, 0, 2);
        ed->cam.pos[0] = -300.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 260.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = -18.0f;
    }
    return true;
}

// Each frame of the measure: the stream's camera flies along the road
void udim_test_frame(Editor* ed, bool measuring, double now) {
    if (UT.scene != 3) return;
    const MatStats* st = mat_stats();
    if (measuring) {
        if (UT.fly0 == 0.0) UT.fly0 = now;
        float x = -300.0f + (float)(now - UT.fly0) * 800.0f; // (8 m/s, to 55 m: the road's last 25 m ahead at the end)
        ed->cam.pos[0] = x < 5500.0f ? x : 5500.0f;
        if (st->udim_resident > UT.max_resident) UT.max_resident = st->udim_resident;
        if (st->tex_bytes > UT.max_bytes) UT.max_bytes = st->tex_bytes;
        UT.samples++;
    }
    UT.tiles = st->udim_tiles;
}

// The shot's neighbor steps (luma, the viewport's middle 90%): the largest
static bool seams_check(const char* shot, const RenderView* rv) {
    uint32_t w = 0, h = 0;
    uint8_t* px = shot ? mat_load_image_rgba(shot, &w, &h) : NULL;
    if (!px) {
        printf("[RealEngine] FAIL: UDIM seams: no shot to read (--shot)\n");
        return false;
    }
    int worst = 0, wx = 0, wy = 0;
    double sum = 0.0;
    uint64_t n = 0;
    uint32_t x0 = (uint32_t)rv->x, y0 = (uint32_t)rv->y, vw = (uint32_t)rv->w, vh = (uint32_t)rv->h;
    if (!vw || !vh || x0 + vw > w || y0 + vh > h) x0 = y0 = 0, vw = w, vh = h;
    for (uint32_t y = y0 + vh / 20; y + 1 < y0 + vh - vh / 20; y++)
        for (uint32_t x = x0 + vw / 20; x + 1 < x0 + vw - vw / 20; x++) {
            const uint8_t* a = &px[((size_t)y * w + x) * 4];
            int l0 = a[0] * 2 + a[1] * 5 + a[2];
            int lx = a[4] * 2 + a[5] * 5 + a[6], ly = a[w * 4] * 2 + a[w * 4 + 1] * 5 + a[w * 4 + 2];
            int d = abs(lx - l0) > abs(ly - l0) ? abs(lx - l0) : abs(ly - l0);
            d = (d + 4) / 8; // (luma in 0..255)
            sum += d, n++;
            if (d > worst) worst = d, wx = (int)x, wy = (int)y;
        }
    free(px);
    double mean = n ? sum / (double)n : 0.0;
    printf("[RealEngine] UDIM seams: the largest step between neighbor pixels %d levels (at %d, %d), the mean %.2f\n", worst, wx, wy, mean);
    if (worst > 6) { // (right: 2 levels; the plain texture() read of the tile's fract uv: 13, at the tiles' cross)
        printf("[RealEngine] FAIL: UDIM seams: a step of %d levels (a seam between tiles: their mips, a bleed or a tile missing)\n", worst);
        return false;
    }
    return true;
}

bool udim_test_check(Editor* ed, const char* shot) {
    const MatStats* st = mat_stats();
    printf("[RealEngine] UDIM: %u tiles, %u seen above the least (most at once %u), %.1f MB of tiles; %u up, %u down, %u source uploads; "
           "textures %.0f of %.0f MB (most %.0f); bake slices GPU %.3f ms at worst, streaming CPU %.3f ms at worst\n",
           st->udim_tiles, st->udim_resident, UT.max_resident, (double)st->udim_bytes / 1048576.0, st->udim_ups, st->udim_downs,
           st->udim_src_uploads, (double)st->tex_bytes / 1048576.0, (double)st->budget / 1048576.0, (double)UT.max_bytes / 1048576.0,
           st->stream_gpu_max, st->stream_cpu_max);
    bool ok = true;
    uint32_t want_tiles = UT.scene == 3 ? 36 : 16; // (the color's 8 and the seams' 8; the stream: + the road's 20)
    if (st->udim_tiles < want_tiles) printf("[RealEngine] FAIL: UDIM: %u tiles baked, %u expected\n", st->udim_tiles, want_tiles), ok = false;
    if (UT.scene == 2) ok = seams_check(shot, &ed->render_view) && ok;
    if (UT.scene == 2) { // (one tile: its source read at the bake's size, 512 = the tile's full, not UD_MIN)
        uint32_t want = st->udim_lone_res < 512u ? st->udim_lone_res : 512u;
        printf("[RealEngine] UDIM one tile: baked at %u, its source read at %u\n", st->udim_lone_res, st->udim_lone_src);
        if (!st->udim_lone_res) printf("[RealEngine] FAIL: UDIM one tile: the material wasn't baked\n"), ok = false;
        else if (st->udim_lone_src < want)
            printf("[RealEngine] FAIL: UDIM one tile: the bake read its source at %u, not %u (blurred)\n", st->udim_lone_src, want), ok = false;
    }
    if (UT.scene == 3) {
        if (!st->streaming) {
            printf("[RealEngine] UDIM: texture streaming off on this GPU: not checked\n");
            return ok;
        }
        if (st->budget >= (1024ull << 20)) { // (every tile fits: baked full at once; the unseen still go down)
            printf("[RealEngine] UDIM: a budget of %.0f MB holds every tile: the climbs aren't checked (the test runs with REALENGINE_TEX_BUDGET_MB=256)\n",
                   (double)st->budget / 1048576.0);
            return ok;
        }
        if (!st->udim_ups) printf("[RealEngine] FAIL: UDIM: no tile climbed as the camera came\n"), ok = false;
        if (!st->udim_downs) printf("[RealEngine] FAIL: UDIM: no tile went down behind the camera\n"), ok = false;
        if (UT.max_resident >= st->udim_tiles) printf("[RealEngine] FAIL: UDIM: every tile was resident at once (only the seen ones should)\n"), ok = false;
        if (UT.max_bytes > st->budget) printf("[RealEngine] FAIL: UDIM: the textures passed the budget (%.0f MB)\n", (double)UT.max_bytes / 1048576.0), ok = false;
    }
    return ok;
}

// --udim-import-test (no GPU): an OBJ whose MTL names one tile of a UDIM set (map_Kd T_Body.1001.png, its 1002 beside
// it) imported like the Content Browser does: the material reads one .rudim of the set (the importers share
// import_material: glTF, FBX and .tlend the same), with both tiles
#include "tlend_import.h"
int udim_import_selftest(void) {
    char root[700], dir[800], p[900];
    if (!re_cache_dir(root, sizeof root - 40)) snprintf(root, sizeof root, ".");
    strncat(root, "/udim_import_test", sizeof root - strlen(root) - 1);
    re_remove_tree(root);
    snprintf(dir, sizeof dir, "%s/src", root);
    re_make_dirs(dir);
    char content_dir[800];
    snprintf(content_dir, sizeof content_dir, "%s/Content", root);
    re_make_dirs(content_dir);
    for (int u = 1001; u <= 1002; u++) snprintf(p, sizeof p, "%s/T_Body.%d.png", dir, u), udim_write_test_tile(p, u, 64, 64, false);
    snprintf(p, sizeof p, "%s/body.mtl", dir);
    FILE* f = re_fopen(p, "wb");
    if (f) fputs("newmtl Body\nKd 1 1 1\nmap_Kd T_Body.1001.png\n", f), fclose(f);
    snprintf(p, sizeof p, "%s/body.obj", dir);
    f = re_fopen(p, "wb");
    if (f) fputs("mtllib body.mtl\no Body\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0.2 0.2\nvt 1.8 0.2\nvt 1.8 0.8\nvt 0.2 0.8\nusemtl Body\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n", f), fclose(f);
    TlendReport r;
    memset(&r, 0, sizeof r);
    bool ok = obj_import(p, "/Content/Body", content_dir, NULL, &r);
    int fails = 0;
    if (!ok || !r.nmat_paths) printf("[RealEngine] FAIL: UDIM import test: the OBJ didn't import (%s)\n", r.text), fails++;
    char text[8192] = "";
    if (r.nmat_paths) {
        FILE* m = re_fopen(r.mat_paths[0], "rb");
        size_t n = m ? fread(text, 1, sizeof text - 1, m) : 0;
        if (m) fclose(m);
        text[n] = 0;
    }
    if (!strstr(text, "/Content/Body/Textures/T_Body.rudim")) printf("[RealEngine] FAIL: UDIM import test: the material doesn't read T_Body.rudim\n"), fails++;
    UdimFile uf;
    snprintf(p, sizeof p, "%s/Body/Textures/T_Body.rudim", content_dir);
    if (udim_open(p, &uf, NULL, 0)) {
        if (uf.count != 2 || udim_find(&uf, 1002) < 0) printf("[RealEngine] FAIL: UDIM import test: %u tiles in the .rudim (2 expected)\n", uf.count), fails++;
        udim_close(&uf);
    } else printf("[RealEngine] FAIL: UDIM import test: no T_Body.rudim\n"), fails++;
    re_remove_tree(root);
    printf("[RealEngine] UDIM import test: %s (the OBJ's material reads the set's .rudim)\n", fails ? "FAIL" : "passed");
    return fails;
}
