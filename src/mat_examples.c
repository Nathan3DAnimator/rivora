// The engine's materials, made on first use under Content/Materials (material.h): M_Grid (Unreal's grid material: a
// world-aligned grid over a gray, to read the level's scale) and a few examples of node graphs (Examples/) that show
// what the nodes do — marble (noise into a ramp, a bump), a tinted checker with a parameter, rusted metal (Voronoi and
// noise driving metallic and roughness), an emissive panel.
#include <stdio.h>
#include <string.h>

#include "material.h"
#include "platform_files.h"
#include "vkctx.h" // (Q4: vkctx_png_write_rgb, the rooms' atlas)
#include <math.h>
#include <stdlib.h>

static int add(MatGraph* g, int type, float x, float y) { return mat_graph_add(g, type, x, y); }

static void set_in(MatGraph* g, int node, const char* sock, float a, float b, float c, float d) {
    const MatNodeDef* def = mat_node_def(g->nodes[node].type);
    for (int i = 0; def && i < def->nin; i++)
        if (!strcmp(def->in[i].name, sock)) {
            float v[4] = { a, b, c, d };
            memcpy(g->nodes[node].in[i], v, sizeof v);
        }
}

static void link(MatGraph* g, int from, const char* out, int to, const char* in) {
    const MatNodeDef *fd = mat_node_def(g->nodes[from].type), *td = mat_node_def(g->nodes[to].type);
    int fs = -1, ts = -1;
    for (int i = 0; fd && i < fd->nout; i++)
        if (!strcmp(fd->out[i].name, out)) fs = i;
    for (int i = 0; td && i < td->nin; i++)
        if (!strcmp(td->in[i].name, in)) ts = i;
    mat_graph_link(g, from, fs, to, ts);
}

static Material* g_making; // (the material build() fills: its own properties, M5's detail normal)

// A material at Content/Materials/<rel> made by build() when the file doesn't exist yet
static void make(const char* root, const char* rel, bool world_grid, void (*build)(MatGraph* g, int bsdf, int out)) {
    char path[700];
    snprintf(path, sizeof path, "%s/Materials/%s", root, rel);
    if (re_file_stat(path, NULL, NULL, NULL)) return;
    char dir[700];
    snprintf(dir, sizeof dir, "%s", path);
    char* cut = strrchr(dir, '/');
    if (cut) *cut = 0;
    re_make_dirs(dir);
    Material* m = mat_create(path, false, NULL);
    if (!m) return;
    MatGraph* g = m->graph;
    int bsdf = -1, out = -1;
    for (int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    }
    m->world_grid = world_grid;
    g_making = m;
    if (build) build(g, bsdf, out);
    g_making = NULL;
    mat_save(m);
    mat_mark_dirty(m);
}

static void grid(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.42f, 0.42f, 0.42f, 1);
    set_in(g, bsdf, "Roughness", 0.65f, 0, 0, 0);
}

static void marble(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -900, 0), noise = add(g, NODE_TEX_NOISE, -700, 0), ramp = add(g, NODE_COLOR_RAMP, -480, -40);
    int bump = add(g, NODE_BUMP, -480, 200);
    link(g, tc, "UV", noise, "Vector");
    set_in(g, noise, "Scale", 4.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 8.0f, 0, 0, 0);
    set_in(g, noise, "Distortion", 2.5f, 0, 0, 0);
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 3;
    float s0[5] = { 0.35f, 0.92f, 0.92f, 0.9f, 1 }, s1[5] = { 0.5f, 0.55f, 0.55f, 0.6f, 1 }, s2[5] = { 0.65f, 0.95f, 0.94f, 0.92f, 1 };
    memcpy(r->ramp[0], s0, sizeof s0), memcpy(r->ramp[1], s1, sizeof s1), memcpy(r->ramp[2], s2, sizeof s2);
    link(g, noise, "Fac", ramp, "Fac");
    link(g, ramp, "Color", bsdf, "Base Color");
    link(g, noise, "Fac", bump, "Height");
    set_in(g, bump, "Strength", 0.3f, 0, 0, 0);
    link(g, bump, "Normal", bsdf, "Normal");
    set_in(g, bsdf, "Roughness", 0.15f, 0, 0, 0);
}

static void checker(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -900, 0), chk = add(g, NODE_TEX_CHECKER, -680, 0), tint = add(g, NODE_RGB, -680, 220);
    int mix = add(g, NODE_MIX, -440, 60);
    link(g, tc, "UV", chk, "Vector");
    set_in(g, chk, "Scale", 8.0f, 0, 0, 0);
    set_in(g, chk, "Color1", 0.9f, 0.9f, 0.9f, 1);
    set_in(g, chk, "Color2", 0.15f, 0.15f, 0.15f, 1);
    MatNode* t = &g->nodes[tint];
    t->value[0] = 0.2f, t->value[1] = 0.55f, t->value[2] = 1.0f, t->value[3] = 1.0f;
    snprintf(t->param, sizeof t->param, "Tint"); // a parameter: Material Instances and Blueprints change it
    MatNode* mx = &g->nodes[mix];
    mx->prop[1] = BLEND_MUL;
    set_in(g, mix, "Factor", 1.0f, 0, 0, 0);
    link(g, chk, "Color", mix, "A_Color");
    link(g, tint, "Color", mix, "B_Color");
    link(g, mix, "Result_Color", bsdf, "Base Color");
}

static void rust(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -1000, 0), vor = add(g, NODE_TEX_VORONOI, -780, -120), noise = add(g, NODE_TEX_NOISE, -780, 140);
    int mixf = add(g, NODE_MIX, -540, 0), ramp = add(g, NODE_COLOR_RAMP, -320, -60), inv = add(g, NODE_MATH, -320, 200);
    link(g, tc, "UV", vor, "Vector");
    link(g, tc, "UV", noise, "Vector");
    set_in(g, vor, "Scale", 12.0f, 0, 0, 0);
    set_in(g, noise, "Scale", 9.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 6.0f, 0, 0, 0);
    g->nodes[mixf].prop[0] = MIX_DATA_FLOAT;
    link(g, vor, "Distance", mixf, "A_Float");
    link(g, noise, "Fac", mixf, "B_Float");
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 2;
    float s0[5] = { 0.42f, 0.33f, 0.12f, 0.05f, 1 }, s1[5] = { 0.55f, 0.62f, 0.62f, 0.64f, 1 };
    memcpy(r->ramp[0], s0, sizeof s0), memcpy(r->ramp[1], s1, sizeof s1);
    link(g, mixf, "Result_Float", ramp, "Fac");
    link(g, ramp, "Color", bsdf, "Base Color");
    link(g, ramp, "Alpha", inv, "Value"); // (alpha 1: the ramp's stops) unused: the metal mask is the luminance below
    g->nodes[inv].prop[0] = MATH_GREATER_THAN;
    set_in(g, inv, "Value_001", 0.5f, 0, 0, 0);
    link(g, mixf, "Result_Float", inv, "Value");
    link(g, inv, "Value", bsdf, "Metallic");
    link(g, noise, "Fac", bsdf, "Roughness");
}

static void emissive(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -900, 0), grad = add(g, NODE_TEX_WAVE, -680, 0), col = add(g, NODE_RGB, -680, 240);
    link(g, tc, "UV", grad, "Vector");
    set_in(g, grad, "Scale", 3.0f, 0, 0, 0);
    MatNode* c = &g->nodes[col];
    c->value[0] = 1.0f, c->value[1] = 0.45f, c->value[2] = 0.1f, c->value[3] = 1.0f;
    snprintf(c->param, sizeof c->param, "Glow Color");
    link(g, col, "Color", bsdf, "Emission Color");
    link(g, grad, "Fac", bsdf, "Emission Strength");
    set_in(g, bsdf, "Base Color", 0.05f, 0.05f, 0.05f, 1);
}

// Animated (the Time node): energy bands sliding along the surface, their glow pulsing (the loop: 2 s, baked frames)
static void animated(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -1100, 0), tm = add(g, NODE_RE_TIME, -1100, 260), wave = add(g, NODE_TEX_WAVE, -820, 0);
    int ang = add(g, NODE_MATH, -820, 260), str = add(g, NODE_MATH, -560, 200), col = add(g, NODE_RGB, -560, -160);
    link(g, tc, "UV", wave, "Vector");
    set_in(g, wave, "Scale", 4.0f, 0, 0, 0);
    set_in(g, wave, "Distortion", 2.0f, 0, 0, 0);
    g->nodes[ang].prop[0] = MATH_MULTIPLY; // the phase as an angle: the bands move one period per loop (seamless)
    link(g, tm, "Phase", ang, "Value");
    set_in(g, ang, "Value_001", 6.2831853f, 0, 0, 0);
    link(g, ang, "Value", wave, "Phase Offset");
    g->nodes[str].prop[0] = MATH_MULTIPLY_ADD; // glow = wave * (1.5 + sine)
    link(g, wave, "Fac", str, "Value");
    link(g, tm, "Sine", str, "Value_001");
    set_in(g, str, "Value_002", 1.5f, 0, 0, 0);
    MatNode* c = &g->nodes[col];
    c->value[0] = 0.15f, c->value[1] = 0.6f, c->value[2] = 1.0f, c->value[3] = 1.0f;
    int boost = add(g, NODE_MATH, -320, 200); // bright enough to read in daylight (Blender's strength 1: 100 nits)
    g->nodes[boost].prop[0] = MATH_MULTIPLY;
    link(g, str, "Value", boost, "Value");
    set_in(g, boost, "Value_001", 40.0f, 0, 0, 0);
    link(g, col, "Color", bsdf, "Emission Color");
    link(g, boost, "Value", bsdf, "Emission Strength");
    set_in(g, bsdf, "Base Color", 0.03f, 0.04f, 0.06f, 1);
    set_in(g, bsdf, "Roughness", 0.25f, 0, 0, 0);
}

// The Principled's extra lobes (M1b: their constants, notas-dev/materiais.md): car paint under a clear coat, brushed
// metal (anisotropic), skin (subsurface), glass (transmission)
static void car_paint(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.55f, 0.02f, 0.03f, 1);
    set_in(g, bsdf, "Metallic", 0.4f, 0, 0, 0);
    set_in(g, bsdf, "Roughness", 0.45f, 0, 0, 0);
    set_in(g, bsdf, "Coat Weight", 1.0f, 0, 0, 0);
    set_in(g, bsdf, "Coat Roughness", 0.03f, 0, 0, 0);
}
// (Q27.8) the city at night: metallic paint with flakes under the coat; a road sign's white reflective film; a red
// reflector (a bike's, a car's back)
static void car_paint_flakes(MatGraph* g, int bsdf, int out) {
    car_paint(g, bsdf, out);
    set_in(g, bsdf, "Base Color", 0.08f, 0.16f, 0.42f, 1);
    set_in(g, bsdf, "Metallic", 0.8f, 0, 0, 0);
    set_in(g, bsdf, "Roughness", 0.35f, 0, 0, 0);
    if (g_making) g_making->flakes = 1.5f, g_making->flake_size = 0.04f, g_making->flake_spread = 0.35f;
}
static void road_sign(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.75f, 0.75f, 0.72f, 1);
    set_in(g, bsdf, "Roughness", 0.5f, 0, 0, 0);
    if (g_making) g_making->retro = 8.0f, g_making->retro_color[0] = g_making->retro_color[1] = g_making->retro_color[2] = 1.0f;
}
static void reflector(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.5f, 0.03f, 0.02f, 1);
    set_in(g, bsdf, "Roughness", 0.2f, 0, 0, 0);
    if (g_making) g_making->retro = 10.0f, g_making->retro_color[0] = 1.0f, g_making->retro_color[1] = 0.08f, g_making->retro_color[2] = 0.04f;
}
static void brushed(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.9f, 0.9f, 0.92f, 1);
    set_in(g, bsdf, "Metallic", 1.0f, 0, 0, 0);
    set_in(g, bsdf, "Roughness", 0.3f, 0, 0, 0);
    set_in(g, bsdf, "Anisotropic", 0.85f, 0, 0, 0);
}
static void skin(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.8f, 0.55f, 0.45f, 1);
    set_in(g, bsdf, "Roughness", 0.5f, 0, 0, 0);
    set_in(g, bsdf, "Subsurface Weight", 1.0f, 0, 0, 0);
    set_in(g, bsdf, "Subsurface Radius", 1.0f, 0.2f, 0.1f, 0);
    // (A9.3a: 1.2 cm in the red, 2.4 mm green, 1.2 mm blue: a person's skin; Blender's 5 cm default reads as wax)
    set_in(g, bsdf, "Subsurface Scale", 0.012f, 0, 0, 0);
    set_in(g, bsdf, "IOR", 1.4f, 0, 0, 0);
}
static void velvet(MatGraph* g, int bsdf, int out) { // (M3: the sheen's rim of light)
    (void)out;
    set_in(g, bsdf, "Base Color", 0.25f, 0.02f, 0.08f, 1);
    set_in(g, bsdf, "Roughness", 0.8f, 0, 0, 0);
    set_in(g, bsdf, "Sheen Weight", 1.0f, 0, 0, 0);
    set_in(g, bsdf, "Sheen Roughness", 0.3f, 0, 0, 0);
    set_in(g, bsdf, "Sheen Tint", 1.0f, 0.75f, 0.85f, 0);
}
static void soap_film(MatGraph* g, int bsdf, int out) { // (M4: a thin film's colors)
    (void)out;
    set_in(g, bsdf, "Base Color", 0.05f, 0.05f, 0.06f, 1);
    set_in(g, bsdf, "Roughness", 0.08f, 0, 0, 0);
    set_in(g, bsdf, "Thin Film Thickness", 520.0f, 0, 0, 0);
    set_in(g, bsdf, "Thin Film IOR", 1.33f, 0, 0, 0);
}
// M5: the Color Attribute multiplying a light base (a mesh's vertex colors tint it), and a detail normal: fine plaster
// bumps (M_DetailNoise's normal) over a smooth wall, repeated 12 times
static void vertex_color(MatGraph* g, int bsdf, int out) {
    (void)out;
    int vc = add(g, NODE_VERTEX_COLOR, -500, 0);
    link(g, vc, "Color", bsdf, "Base Color");
    set_in(g, bsdf, "Roughness", 0.45f, 0, 0, 0);
}
// B1: the HLOD's merged meshes (world_partition.c): their materials baked into the vertex colors, a matte surface
static void hlod(MatGraph* g, int bsdf, int out) {
    (void)out;
    int vc = add(g, NODE_VERTEX_COLOR, -500, 0);
    link(g, vc, "Color", bsdf, "Base Color");
    set_in(g, bsdf, "Roughness", 0.75f, 0, 0, 0);
}
static void detail_noise(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -900, 200), noise = add(g, NODE_TEX_NOISE, -700, 200), bump = add(g, NODE_BUMP, -480, 200);
    link(g, tc, "UV", noise, "Vector");
    set_in(g, noise, "Scale", 24.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 6.0f, 0, 0, 0);
    link(g, noise, "Fac", bump, "Height");
    set_in(g, bump, "Strength", 0.6f, 0, 0, 0);
    link(g, bump, "Normal", bsdf, "Normal");
    set_in(g, bsdf, "Base Color", 0.5f, 0.5f, 0.5f, 1);
}
static void detail_plaster(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.78f, 0.74f, 0.68f, 1);
    set_in(g, bsdf, "Roughness", 0.8f, 0, 0, 0);
    if (g_making) {
        snprintf(g_making->detail_path, sizeof g_making->detail_path, "/Content/Materials/Examples/M_DetailNoise.rmat");
        g_making->detail_tiling = 12.0f, g_making->detail_strength = 1.0f;
    }
}
// M6: a decal's material: grime, a dark blot whose Alpha (the decal's share) is a round falloff from the middle of
// the UV broken by noise (its edge ragged), rough and matte
static void decal_grime(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -1200, 0), sub = add(g, NODE_VECTOR_MATH, -1000, 0), grad = add(g, NODE_TEX_GRADIENT, -800, 0);
    int noise = add(g, NODE_TEX_NOISE, -1000, 260), madd = add(g, NODE_MATH, -800, 260), mul = add(g, NODE_MATH, -600, 100);
    int ramp = add(g, NODE_COLOR_RAMP, -400, 100);
    link(g, tc, "UV", sub, "Vector");
    g->nodes[sub].prop[0] = VMATH_SUBTRACT;
    set_in(g, sub, "Vector_001", 0.5f, 0.5f, 0.0f, 0);
    g->nodes[grad].prop[0] = GRADIENT_SPHERICAL;
    link(g, sub, "Vector", grad, "Vector");
    link(g, tc, "UV", noise, "Vector");
    set_in(g, noise, "Scale", 7.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 6.0f, 0, 0, 0);
    g->nodes[madd].prop[0] = MATH_MULTIPLY_ADD; // noise * 0.6 + 0.7: the falloff broken up
    link(g, noise, "Fac", madd, "Value");
    set_in(g, madd, "Value_001", 0.6f, 0, 0, 0);
    set_in(g, madd, "Value_002", 0.7f, 0, 0, 0);
    g->nodes[mul].prop[0] = MATH_MULTIPLY;
    link(g, grad, "Fac", mul, "Value");
    link(g, madd, "Value", mul, "Value_001");
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 2;
    float s0[5] = { 0.45f, 0, 0, 0, 1 }, s1[5] = { 0.68f, 1, 1, 1, 1 };
    memcpy(r->ramp[0], s0, sizeof s0), memcpy(r->ramp[1], s1, sizeof s1);
    link(g, mul, "Value", ramp, "Fac");
    link(g, ramp, "Color", bsdf, "Alpha");
    set_in(g, bsdf, "Base Color", 0.07f, 0.05f, 0.035f, 1);
    set_in(g, bsdf, "Roughness", 0.95f, 0, 0, 0);
}
// M7: cobblestones seen in depth: two Voronois' F2 - F1 as the height (0 on the cells' borders: the mortar sunk, the
// stones' tops flat), into the Material Output's Displacement (the parallax) and a Bump (the light on the stones' sides)
static void parallax_cobble(MatGraph* g, int bsdf, int out) {
    int tc = add(g, NODE_TEX_COORD, -1500, 0), vor = add(g, NODE_TEX_VORONOI, -1300, -120), vor2 = add(g, NODE_TEX_VORONOI, -1300, 160);
    int sub = add(g, NODE_MATH, -1100, 0), hgt = add(g, NODE_MATH, -900, 0);
    int ramp = add(g, NODE_COLOR_RAMP, -650, -160), bump = add(g, NODE_BUMP, -650, 220), disp = add(g, NODE_DISPLACEMENT, -300, 420);
    link(g, tc, "UV", vor, "Vector"), link(g, tc, "UV", vor2, "Vector");
    set_in(g, vor, "Scale", 6.0f, 0, 0, 0), set_in(g, vor2, "Scale", 6.0f, 0, 0, 0);
    g->nodes[vor2].prop[0] = VORONOI_F2;
    g->nodes[sub].prop[0] = MATH_SUBTRACT; // F2 - F1: 0 on the border between two cells (the mortar)
    link(g, vor2, "Distance", sub, "Value"), link(g, vor, "Distance", sub, "Value_001");
    g->nodes[hgt].prop[0] = MATH_MULTIPLY; // x 5, clamped: the stone's edge rounded, its top flat
    g->nodes[hgt].prop[1] = 1;
    link(g, sub, "Value", hgt, "Value");
    set_in(g, hgt, "Value_001", 5.0f, 0, 0, 0);
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 2;
    float s0[5] = { 0.05f, 0.16f, 0.15f, 0.14f, 1 }, s1[5] = { 0.35f, 0.46f, 0.44f, 0.41f, 1 };
    memcpy(r->ramp[0], s0, sizeof s0), memcpy(r->ramp[1], s1, sizeof s1);
    link(g, hgt, "Value", ramp, "Fac");
    link(g, ramp, "Color", bsdf, "Base Color");
    link(g, hgt, "Value", bump, "Height");
    set_in(g, bump, "Strength", 0.7f, 0, 0, 0);
    link(g, bump, "Normal", bsdf, "Normal");
    link(g, hgt, "Value", disp, "Height");
    set_in(g, disp, "Scale", 1.0f, 0, 0, 0);
    link(g, disp, "Displacement", out, "Displacement");
    set_in(g, bsdf, "Roughness", 0.75f, 0, 0, 0);
}
// (A9.3c) an eye: the Eye shading model; the pupil, the iris (two blues, fibers by a stretched noise), the limbal ring
// and the sclera by the distance from the UV's center (SM_Eye's UV: the iris at the center, radius 0.25)
static void eye(MatGraph* g, int bsdf, int out) {
    (void)out;
    int tc = add(g, NODE_TEX_COORD, -1100, 0), dist = add(g, NODE_VECTOR_MATH, -900, 0), ramp = add(g, NODE_COLOR_RAMP, -680, -40);
    int noise = add(g, NODE_TEX_NOISE, -900, 240), mix = add(g, NODE_MIX, -440, 60);
    g->nodes[dist].prop[0] = VMATH_DISTANCE;
    link(g, tc, "UV", dist, "Vector");
    set_in(g, dist, "Vector_001", 0.5f, 0.5f, 0.0f, 0);
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 6;
    float st[6][5] = { { 0.085f, 0.01f, 0.01f, 0.01f, 1 }, { 0.10f, 0.03f, 0.09f, 0.16f, 1 }, { 0.19f, 0.18f, 0.38f, 0.5f, 1 },
                       { 0.235f, 0.04f, 0.06f, 0.08f, 1 }, { 0.27f, 0.82f, 0.78f, 0.74f, 1 }, { 0.5f, 0.86f, 0.74f, 0.7f, 1 } };
    for (int i = 0; i < 6; i++) memcpy(r->ramp[i], st[i], sizeof st[i]);
    link(g, dist, "Value", ramp, "Fac");
    link(g, tc, "UV", noise, "Vector");
    set_in(g, noise, "Scale", 45.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 3.0f, 0, 0, 0);
    MatNode* mx = &g->nodes[mix];
    mx->prop[1] = BLEND_MUL;
    set_in(g, mix, "Factor", 0.35f, 0, 0, 0);
    link(g, ramp, "Color", mix, "A_Color");
    link(g, noise, "Color", mix, "B_Color");
    link(g, mix, "Result_Color", bsdf, "Base Color");
    set_in(g, bsdf, "Roughness", 0.4f, 0, 0, 0);
    if (g_making) g_making->shading_model = MAT_SHADING_EYE, g_making->eye_iris_radius = 0.25f, g_making->eye_iris_depth = 0.25f;
}
// (Q13) the eye that shines back at a flashlight (Eye Glow): an animal's, an animatronic's; the iris darker
static void eye_glow(MatGraph* g, int bsdf, int out) {
    eye(g, bsdf, out);
    if (g_making) g_making->eye_glow = 4.0f, g_making->eye_glow_color[0] = 0.9f, g_making->eye_glow_color[1] = 1.0f, g_making->eye_glow_color[2] = 0.75f;
}
static void glass(MatGraph* g, int bsdf, int out) {
    (void)out;
    set_in(g, bsdf, "Base Color", 0.95f, 0.98f, 1.0f, 1);
    set_in(g, bsdf, "Roughness", 0.03f, 0, 0, 0);
    set_in(g, bsdf, "Transmission Weight", 0.9f, 0, 0, 0);
}

// (2026-11-16) the Wet Surface: concrete after the rain (porous: darker, a film with drops running down) and skin with
// sweat (a "Sweat" parameter a Blueprint raises while the character runs: no bake, mesh.frag reads it)
static int wet_over(MatGraph* g, int bsdf, int out) {
    int w = add(g, NODE_RE_WET, 120, 0);
    link(g, bsdf, "BSDF", w, "Shader");
    link(g, w, "Shader", out, "Surface");
    return w;
}
static void wet_concrete(MatGraph* g, int bsdf, int out) {
    int nz = add(g, NODE_TEX_NOISE, -600, 0);
    set_in(g, nz, "Scale", 18.0f, 0, 0, 0);
    int ramp = add(g, NODE_COLOR_RAMP, -380, 0);
    MatNode* r = &g->nodes[ramp];
    r->ramp_count = 2;
    float s0[5] = { 0.3f, 0.36f, 0.35f, 0.33f, 1 }, s1[5] = { 0.7f, 0.58f, 0.57f, 0.55f, 1 };
    memcpy(r->ramp[0], s0, sizeof s0), memcpy(r->ramp[1], s1, sizeof s1);
    link(g, nz, "Fac", ramp, "Fac");
    link(g, ramp, "Color", bsdf, "Base Color");
    set_in(g, bsdf, "Roughness", 0.85f, 0, 0, 0);
    int w = wet_over(g, bsdf, out);
    set_in(g, w, "Wetness", 0.85f, 0, 0, 0);
    set_in(g, w, "Water Film", 0.7f, 0, 0, 0);
    set_in(g, w, "Droplets", 0.45f, 0, 0, 0);
    set_in(g, w, "Running", 0.5f, 0, 0, 0);
}
static void sweaty_skin(MatGraph* g, int bsdf, int out) {
    skin(g, bsdf, out);
    int w = wet_over(g, bsdf, out);
    set_in(g, w, "Wetness", 1.0f, 0, 0, 0);
    set_in(g, w, "Water Film", 0.35f, 0, 0, 0);
    set_in(g, w, "Film Roughness", 0.12f, 0, 0, 0);
    set_in(g, w, "Droplets", 0.6f, 0, 0, 0);
    set_in(g, w, "Droplet Size", 2.5f, 0, 0, 0);
    set_in(g, w, "Running", 0.25f, 0, 0, 0);
    int sv = add(g, NODE_VALUE, -120, 300);
    g->nodes[sv].value[0] = 0.8f;
    snprintf(g->nodes[sv].param, sizeof g->nodes[sv].param, "Sweat"); // (a parameter: a Blueprint's Set Scalar Parameter Value)
    link(g, sv, "Value", w, "Sweat");
}

// (2026-11-17, Q4) Light Functions (Material Domain: Light Function): what the light is multiplied by
static void lf_blinds(MatGraph* g, int bsdf, int out) { // a window's blinds: soft bands
    (void)out;
    g_making->domain = MAT_DOMAIN_LIGHT_FUNCTION;
    int tc = add(g, NODE_TEX_COORD, -900, 0), wave = add(g, NODE_TEX_WAVE, -640, 0), ramp = add(g, NODE_MATH, -380, 0);
    link(g, tc, "UV", wave, "Vector");
    set_in(g, wave, "Scale", 6.0f, 0, 0, 0);
    set_in(g, wave, "Detail", 0.0f, 0, 0, 0);
    g->nodes[ramp].prop[0] = MATH_MULTIPLY_ADD; // (bands of light and shade, never quite black: the slats' gaps)
    link(g, wave, "Fac", ramp, "Value");
    set_in(g, ramp, "Value_001", 1.1f, 0, 0, 0);
    set_in(g, ramp, "Value_002", -0.1f, 0, 0, 0);
    link(g, ramp, "Value", bsdf, "Base Color");
}
static void lf_clouds(MatGraph* g, int bsdf, int out) { // clouds passing (a directional light): bands drifting, distorted
    (void)out;
    g_making->domain = MAT_DOMAIN_LIGHT_FUNCTION;
    g_making->anim_frames = 32;
    int tc = add(g, NODE_TEX_COORD, -1100, 0), tm = add(g, NODE_RE_TIME, -1100, 260), wave = add(g, NODE_TEX_WAVE, -820, 0);
    int ang = add(g, NODE_MATH, -820, 260), lift = add(g, NODE_MATH, -500, 0);
    link(g, tc, "UV", wave, "Vector");
    set_in(g, wave, "Scale", 1.5f, 0, 0, 0);
    set_in(g, wave, "Distortion", 6.0f, 0, 0, 0);
    set_in(g, wave, "Detail", 4.0f, 0, 0, 0);
    g->nodes[ang].prop[0] = MATH_MULTIPLY; // (one period a loop: seamless)
    link(g, tm, "Phase", ang, "Value");
    set_in(g, ang, "Value_001", 6.2831853f, 0, 0, 0);
    link(g, ang, "Value", wave, "Phase Offset");
    g->nodes[lift].prop[0] = MATH_MULTIPLY_ADD; // (the clouds' shade: 30% of the sun under them)
    link(g, wave, "Fac", lift, "Value");
    set_in(g, lift, "Value_001", 0.7f, 0, 0, 0);
    set_in(g, lift, "Value_002", 0.3f, 0, 0, 0);
    link(g, lift, "Value", bsdf, "Base Color");
}
static void lf_flicker(MatGraph* g, int bsdf, int out) { // a failing bulb: a random level a step, a few steps a second
    (void)out;
    g_making->domain = MAT_DOMAIN_LIGHT_FUNCTION;
    g_making->anim_frames = 24;
    int tm = add(g, NODE_RE_TIME, -1100, 0), mul = add(g, NODE_MATH, -860, 0), fl = add(g, NODE_MATH, -640, 0);
    int wn = add(g, NODE_TEX_WHITE_NOISE, -420, 0), lv = add(g, NODE_MATH, -200, 0);
    g->nodes[mul].prop[0] = MATH_MULTIPLY;
    link(g, tm, "Phase", mul, "Value");
    set_in(g, mul, "Value_001", 12.0f, 0, 0, 0);
    g->nodes[fl].prop[0] = MATH_FLOOR;
    link(g, mul, "Value", fl, "Value");
    link(g, fl, "Value", wn, "Vector");
    g->nodes[lv].prop[0] = MATH_MULTIPLY_ADD; // (never fully off: 25..100%)
    link(g, wn, "Value", lv, "Value");
    set_in(g, lv, "Value_001", 0.75f, 0, 0, 0);
    set_in(g, lv, "Value_002", 0.25f, 0, 0, 0);
    link(g, lv, "Value", bsdf, "Base Color");
}
static void lf_projector(MatGraph* g, int bsdf, int out) { // a projector's slide: a colored checker
    (void)out;
    g_making->domain = MAT_DOMAIN_LIGHT_FUNCTION;
    int tc = add(g, NODE_TEX_COORD, -800, 0), ck = add(g, NODE_TEX_CHECKER, -520, 0);
    link(g, tc, "UV", ck, "Vector");
    set_in(g, ck, "Scale", 6.0f, 0, 0, 0);
    set_in(g, ck, "Color1", 1.0f, 0.75f, 0.35f, 1);
    set_in(g, ck, "Color2", 0.15f, 0.35f, 1.0f, 1);
    link(g, ck, "Color", bsdf, "Base Color");
}

// (2026-11-17, Q4) The Interior Mapping's example atlas: 4 x 4 rooms (1024 px), each as seen from its window (the back
// wall the middle half, the side walls, floor and ceiling in perspective): a wall color, a wooden floor or a rug, a
// picture or a window or shelves on the back wall, a sofa, a bed or a desk against it, a ceiling lamp. Made once
static float rh(int t, int k) { unsigned x = (unsigned)(t * 7919 + k * 104729 + 17); x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15; return (float)(x & 0xFFFF) / 65535.0f; }
static void rooms_atlas(const char* root) {
    char path[800];
    snprintf(path, sizeof path, "%s/Materials/Examples/Textures/T_RoomsAtlas.png", root);
    if (re_file_stat(path, NULL, NULL, NULL)) return;
    char dir[800];
    snprintf(dir, sizeof dir, "%s/Materials/Examples/Textures", root);
    re_make_dirs(dir);
    const int N = 1024, T = 256;
    uint8_t* px = malloc((size_t)N * N * 3);
    if (!px) return;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int t = (y / T) * 4 + x / T;
            float u = ((float)(x % T) + 0.5f) / T * 2.0f - 1.0f, v = 1.0f - ((float)(y % T) + 0.5f) / T * 2.0f; // (v up)
            float wall[3] = { 0.55f + 0.35f * rh(t, 1), 0.5f + 0.35f * rh(t, 2), 0.45f + 0.35f * rh(t, 3) };
            float c[3];
            float ax = fabsf(u), ay = fabsf(v);
            float depth; // (0 at the window, 1 at the back wall)
            if (ax <= 0.5f && ay <= 0.5f) { // the back wall
                depth = 1.0f;
                for (int k = 0; k < 3; k++) c[k] = wall[k] * 0.9f;
                int kind = (int)(rh(t, 4) * 3.0f);
                if (kind == 0 && fabsf(u - 0.05f) < 0.18f && v > 0.0f && v < 0.3f) c[0] = 0.2f + 0.6f * rh(t, 5), c[1] = 0.25f + 0.5f * rh(t, 6), c[2] = 0.3f + 0.5f * rh(t, 7); // a picture
                if (kind == 1 && fabsf(u) < 0.2f && v > -0.05f && v < 0.35f) c[0] = 0.35f, c[1] = 0.55f, c[2] = 0.85f;                 // a window on the far side
                if (kind == 2 && u < -0.1f && u > -0.45f && v > -0.5f && v < 0.4f) { float sh = fmodf((v + 0.5f) * 5.0f, 1.0f); float b = sh < 0.15f ? 0.25f : 0.45f + 0.3f * rh(t * 31 + (int)(u * 40.0f), 8); c[0] = b, c[1] = b * 0.8f, c[2] = b * 0.6f; } // shelves
                int fur = (int)(rh(t, 9) * 3.0f);
                if (fur == 0 && fabsf(u) < 0.35f && v < -0.25f) c[0] = 0.25f + 0.4f * rh(t, 10), c[1] = 0.2f + 0.3f * rh(t, 11), c[2] = 0.2f + 0.3f * rh(t, 12); // a sofa
                if (fur == 1 && fabsf(u + 0.1f) < 0.3f && v < -0.3f) c[0] = 0.85f, c[1] = 0.85f, c[2] = 0.8f; // a bed
                if (fur == 2 && fabsf(u - 0.2f) < 0.22f && v < -0.2f && v > -0.27f) c[0] = 0.4f, c[1] = 0.27f, c[2] = 0.15f; // a desk
            } else if (ay > ax) { // floor (below) or ceiling
                depth = (1.0f - ay) / 0.5f;
                if (v < 0.0f) {
                    float plank = fmodf((u / ay) * 4.0f + 10.0f, 1.0f);
                    float b = 0.75f + 0.25f * (plank < 0.06f ? 0.4f : 1.0f);
                    if (rh(t, 13) < 0.5f) c[0] = 0.42f * b, c[1] = 0.27f * b, c[2] = 0.15f * b;
                    else c[0] = 0.5f * rh(t, 14) + 0.2f, c[1] = 0.25f, c[2] = 0.3f * rh(t, 15) + 0.2f; // a rug
                } else {
                    c[0] = c[1] = c[2] = 0.88f;
                    float lu = u / ay, lv = (1.0f - ay) / 0.5f; // a ceiling lamp in the middle of the room
                    if (fabsf(lu) < 0.12f && fabsf(lv - 0.6f) < 0.1f) c[0] = 1.0f, c[1] = 0.95f, c[2] = 0.8f;
                }
            } else { // side walls
                depth = (1.0f - ax) / 0.5f;
                for (int k = 0; k < 3; k++) c[k] = wall[k];
                if (u > 0.0f && rh(t, 16) < 0.4f && fabsf(v / ax + 0.1f) < 0.45f && depth > 0.35f && depth < 0.65f) c[0] = 0.45f, c[1] = 0.32f, c[2] = 0.2f; // a door
            }
            float shade = 0.6f + 0.4f * (1.0f - depth * 0.6f); // (nearer the window: brighter)
            for (int k = 0; k < 3; k++) {
                float s = c[k] * shade;
                px[((size_t)y * N + x) * 3 + k] = (uint8_t)(255.0f * powf(s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s, 1.0f / 2.2f) + 0.5f);
            }
        }
    vkctx_png_write_rgb(path, px, N, N);
    free(px);
}
static void building_atlas(MatGraph* g, int bsdf, int out) { // a facade with the atlas's rooms behind it
    int im = add(g, NODE_RE_INTERIOR, 200, 0);
    snprintf(g->nodes[im].image, sizeof g->nodes[im].image, "/Content/Materials/Examples/Textures/T_RoomsAtlas.png");
    g->nodes[im].prop[0] = 3, g->nodes[im].prop[1] = 3; // (4 x 4)
    set_in(g, im, "Rooms U", 4, 0, 0, 0);
    set_in(g, im, "Rooms V", 10, 0, 0, 0);
    set_in(g, bsdf, "Base Color", 0.55f, 0.5f, 0.46f, 1);
    set_in(g, bsdf, "Roughness", 0.8f, 0, 0, 0);
    int nv = add(g, NODE_VALUE, -200, 300);
    snprintf(g->nodes[nv].param, sizeof g->nodes[nv].param, "Night");
    link(g, bsdf, "BSDF", im, "Shader");
    link(g, im, "Shader", out, "Surface");
    link(g, nv, "Value", im, "Night");
}

// (Q11) the Toon examples (notas-dev/toon.md): a base color (a "Color" parameter: Material Instances and Blueprints change
// it) and the toon's values on the material (g_making)
static void toon_base(MatGraph* g, int bsdf, float r, float gr, float b) {
    int col = add(g, NODE_RGB, -560, 0);
    MatNode* t = &g->nodes[col];
    t->value[0] = r, t->value[1] = gr, t->value[2] = b, t->value[3] = 1.0f;
    snprintf(t->param, sizeof t->param, "Color");
    link(g, col, "Color", bsdf, "Base Color");
    set_in(g, bsdf, "Roughness", 0.6f, 0, 0, 0);
}
static MatToon* toon_of_making(void) {
    if (!g_making) return NULL;
    g_making->shading_model = MAT_SHADING_TOON;
    toon_defaults(&g_making->toon);
    return &g_making->toon;
}
static void toon_basic(MatGraph* g, int bsdf, int out) { // two bands, a cool shadow, a highlight, a black outline
    (void)out;
    toon_base(g, bsdf, 0.95f, 0.42f, 0.12f);
    MatToon* t = toon_of_making();
    if (t) t->outline = true;
}
static void toon_ramp(MatGraph* g, int bsdf, int out) { // four bands by a ramp of four points, a rim of light
    (void)out;
    toon_base(g, bsdf, 0.35f, 0.85f, 0.55f);
    MatToon* t = toon_of_making();
    if (!t) return;
    t->bands = 4, t->softness = 0.05f, t->nstops = 4;
    const float st[4][4] = { { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.35f, 0.25f, 0.3f, 0.55f }, { 0.7f, 0.7f, 0.75f, 0.85f }, { 1.0f, 1.0f, 1.0f, 1.0f } };
    for (int i = 0; i < 4; i++) t->stop_pos[i] = st[i][0], t->stop_col[i][0] = st[i][1], t->stop_col[i][1] = st[i][2], t->stop_col[i][2] = st[i][3];
    t->shadow_color[0] = 0.12f, t->shadow_color[1] = 0.16f, t->shadow_color[2] = 0.35f;
    t->rim_strength = 1.2f, t->rim_width = 0.28f;
    t->outline = true, t->outline_color[0] = 0.02f, t->outline_color[1] = 0.12f, t->outline_color[2] = 0.06f;
}
static void toon_hatch(MatGraph* g, int bsdf, int out) { // three bands, hatching on the screen where it is dark
    (void)out;
    toon_base(g, bsdf, 0.92f, 0.86f, 0.72f);
    MatToon* t = toon_of_making();
    if (!t) return;
    t->bands = 3, t->hatch_strength = 0.85f, t->hatch_threshold = 0.8f, t->spec_strength = 0.0f;
    t->shadow_color[0] = t->shadow_color[1] = t->shadow_color[2] = 0.55f;
    t->recv_color[0] = t->recv_color[1] = t->recv_color[2] = 0.45f;
    t->outline = true;
}
static void toon_skin(MatGraph* g, int bsdf, int out) { // soft bands, a warm shadow, a red cast shadow, a warm rim
    (void)out;
    toon_base(g, bsdf, 0.98f, 0.76f, 0.62f);
    MatToon* t = toon_of_making();
    if (!t) return;
    t->softness = 0.3f, t->shadow_threshold = 0.45f;
    t->shadow_color[0] = 0.78f, t->shadow_color[1] = 0.48f, t->shadow_color[2] = 0.48f;
    t->recv_color[0] = 0.7f, t->recv_color[1] = 0.36f, t->recv_color[2] = 0.38f;
    t->spec_strength = 0.25f, t->spec_size = 0.15f;
    t->rim_color[0] = 1.0f, t->rim_color[1] = 0.85f, t->rim_color[2] = 0.7f, t->rim_strength = 0.8f, t->rim_width = 0.22f;
    t->outline = true, t->outline_color[0] = 0.25f, t->outline_color[1] = 0.1f, t->outline_color[2] = 0.08f, t->outline_width = 2.0f;
}
static void toon_ground(MatGraph* g, int bsdf, int out) { // the stage: three bands, no outline (the actors' lines read over it)
    (void)out;
    toon_base(g, bsdf, 0.55f, 0.6f, 0.5f);
    MatToon* t = toon_of_making();
    if (t) t->bands = 3, t->spec_strength = 0.0f;
}
// (Q11) MI_ToonBlue: M_Toon in blue, three bands and a blue outline (its own toon values over its parent's)
static void toon_instance(const char* root) {
    char path[700];
    snprintf(path, sizeof path, "%s/Materials/Examples/Toon/MI_ToonBlue.rmi", root);
    if (re_file_stat(path, NULL, NULL, NULL)) return;
    Material* mi = mat_create(path, true, "/Content/Materials/Examples/Toon/M_Toon.rmat");
    if (!mi) return;
    MatParam* p = &mi->params[0];
    snprintf(p->name, sizeof p->name, "Color");
    p->kind = 1, p->value[0] = 0.2f, p->value[1] = 0.45f, p->value[2] = 0.95f, p->value[3] = 1.0f;
    mi->nparams = 1;
    mi->toon_own = true;
    toon_defaults(&mi->toon);
    mi->toon.bands = 3, mi->toon.outline = true;
    mi->toon.outline_color[0] = 0.02f, mi->toon.outline_color[1] = 0.05f, mi->toon.outline_color[2] = 0.3f, mi->toon.outline_width = 3.0f;
    mat_save(mi);
    mat_mark_dirty(mi);
}

void mat_make_engine_materials(const char* content_root) {
    make(content_root, "Examples/Toon/M_Toon.rmat", false, toon_basic); // (Q11)
    make(content_root, "Examples/Toon/M_ToonRamp.rmat", false, toon_ramp);
    make(content_root, "Examples/Toon/M_ToonHatch.rmat", false, toon_hatch);
    make(content_root, "Examples/Toon/M_ToonSkin.rmat", false, toon_skin);
    make(content_root, "Examples/Toon/M_ToonGround.rmat", false, toon_ground);
    toon_instance(content_root);
    rooms_atlas(content_root); // (Q4)
    make(content_root, "Examples/M_BuildingAtlas.rmat", false, building_atlas);
    make(content_root, "Examples/LightFunctions/LF_Blinds.rmat", false, lf_blinds); // (Q4)
    make(content_root, "Examples/LightFunctions/LF_Clouds.rmat", false, lf_clouds);
    make(content_root, "Examples/LightFunctions/LF_Flicker.rmat", false, lf_flicker);
    make(content_root, "Examples/LightFunctions/LF_Projector.rmat", false, lf_projector);
    make(content_root, "M_Grid.rmat", true, grid);
    make(content_root, "M_HLOD.rmat", false, hlod);
    make(content_root, "Examples/M_Marble.rmat", false, marble);
    make(content_root, "Examples/M_TintedChecker.rmat", false, checker);
    make(content_root, "Examples/M_RustedMetal.rmat", false, rust);
    make(content_root, "Examples/M_EmissivePanel.rmat", false, emissive);
    make(content_root, "Examples/M_AnimatedEnergy.rmat", false, animated);
    make(content_root, "Examples/M_CarPaint.rmat", false, car_paint);
    make(content_root, "Examples/M_CarPaintFlakes.rmat", false, car_paint_flakes); // (Q27.8)
    make(content_root, "Examples/M_RoadSign.rmat", false, road_sign);
    make(content_root, "Examples/M_Reflector.rmat", false, reflector);
    make(content_root, "Examples/M_BrushedMetal.rmat", false, brushed);
    make(content_root, "Examples/M_Skin.rmat", false, skin);
    make(content_root, "Examples/M_HumanSkin.rmat", false, skin); // (A9.3a: skin()'s 1.2 cm, also where an old M_Skin keeps 5)
    make(content_root, "Examples/M_Glass.rmat", false, glass);
    make(content_root, "Examples/M_Eye.rmat", false, eye);
    make(content_root, "Examples/M_EyeGlow.rmat", false, eye_glow); // (Q13)
    make(content_root, "Examples/M_Velvet.rmat", false, velvet);
    make(content_root, "Examples/M_SoapFilm.rmat", false, soap_film);
    make(content_root, "Examples/M_VertexColor.rmat", false, vertex_color);
    make(content_root, "Examples/M_DetailNoise.rmat", false, detail_noise);
    make(content_root, "Examples/M_DetailPlaster.rmat", false, detail_plaster);
    make(content_root, "Examples/M_DecalGrime.rmat", false, decal_grime);
    make(content_root, "Examples/M_ParallaxCobble.rmat", false, parallax_cobble);
    make(content_root, "Examples/M_WetConcrete.rmat", false, wet_concrete);
    make(content_root, "Examples/M_SweatySkin.rmat", false, sweaty_skin);
}

// The texture streaming's stress (sugestão #51, --stress-textures): a 4096 material whose base color is a noise times
// a "Tint" parameter (so each instance has textures of its own) and n Material Instances of it with their own tints, under
// Content/Stress (made again each run). The instances' hashes into out (n of them: the cubes' materials).
uint32_t mat_make_stress(const char* content_root, uint32_t n, uint32_t* out) {
    char dir[700], path[760];
    snprintf(dir, sizeof dir, "%s/Stress", content_root);
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/M_StressTex.rmat", dir);
    Material* m = mat_create(path, false, NULL);
    if (!m) return 0;
    MatGraph* g = m->graph;
    int bsdf = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
    int tc = add(g, NODE_TEX_COORD, -900, 0), noise = add(g, NODE_TEX_NOISE, -700, 0), tint = add(g, NODE_RGB, -700, 260);
    int mix = add(g, NODE_MIX, -440, 60), bump = add(g, NODE_BUMP, -440, 300);
    link(g, tc, "UV", noise, "Vector");
    set_in(g, noise, "Scale", 24.0f, 0, 0, 0);
    set_in(g, noise, "Detail", 6.0f, 0, 0, 0);
    MatNode* t = &g->nodes[tint];
    t->value[0] = 1.0f, t->value[1] = 0.8f, t->value[2] = 0.6f, t->value[3] = 1.0f;
    snprintf(t->param, sizeof t->param, "Tint");
    MatNode* mx = &g->nodes[mix];
    mx->prop[1] = BLEND_MUL;
    mx->prop[3] = 1; // (2026-11-24: Clamp Result: not affine, the Tint stays baked; else the fold made the instances share
                     // their parent's textures and nothing was left to stream)
    set_in(g, mix, "Factor", 1.0f, 0, 0, 0);
    link(g, noise, "Color", mix, "A_Color");
    link(g, tint, "Color", mix, "B_Color");
    link(g, mix, "Result_Color", bsdf, "Base Color");
    link(g, noise, "Fac", bump, "Height");
    set_in(g, bump, "Strength", 0.4f, 0, 0, 0);
    link(g, bump, "Normal", bsdf, "Normal");
    set_in(g, bsdf, "Roughness", 0.5f, 0, 0, 0);
    m->resolution = 4096;
    mat_save(m);
    mat_mark_dirty(m);
    uint32_t made = 0;
    for (uint32_t i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/MI_Stress_%04u.rmi", dir, i);
        Material* mi = mat_create(path, true, "/Content/Stress/M_StressTex.rmat");
        if (!mi) break;
        float h = (float)i * 0.618034f;
        h -= (float)(int)h;
        MatParam* p = &mi->params[0];
        snprintf(p->name, sizeof p->name, "Tint");
        p->kind = 1;
        p->value[0] = 0.4f + 0.6f * h, p->value[1] = 0.4f + 0.6f * (1.0f - h), p->value[2] = 0.5f + 0.5f * (h > 0.5f ? h - 0.5f : 0.5f - h);
        p->value[3] = 1.0f;
        mi->nparams = 1;
        mat_save(mi);
        mat_mark_dirty(mi);
        out[made++] = mi->hash;
    }
    return made;
}
